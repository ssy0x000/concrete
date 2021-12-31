// Part of the Concrete Compiler Project, under the BSD3 License with Zama
// Exceptions. See
// https://github.com/zama-ai/homomorphizer/blob/master/LICENSE.txt for license
// information.

#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/SmallVector.h"
#include <iostream>

#include "concretelang/Conversion/Passes.h"
#include "concretelang/Dialect/FHE/IR/FHEDialect.h"
#include "concretelang/Dialect/FHE/IR/FHEOps.h"
#include "concretelang/Dialect/FHELinalg/IR/FHELinalgDialect.h"
#include "concretelang/Dialect/FHELinalg/IR/FHELinalgOps.h"
#include "concretelang/Support/Constants.h"

struct DotToLinalgGeneric
    : public ::mlir::OpRewritePattern<mlir::concretelang::FHELinalg::Dot> {
  DotToLinalgGeneric(::mlir::MLIRContext *context)
      : ::mlir::OpRewritePattern<::mlir::concretelang::FHELinalg::Dot>(
            context, mlir::concretelang::DEFAULT_PATTERN_BENEFIT) {}

  // This rewrite pattern transforms any instance of
  // `FHELinalg.dot_eint_int` to an instance of `linalg.generic` with an
  // appropriate region using `FHE.mul_eint_int` and
  // `FHE.add_eint` operations, an appropriate specification for the
  // iteration dimensions and appropriate operations managing the
  // accumulator of `linalg.generic`.
  //
  // Example:
  //
  //   %o = "FHELinalg.dot_eint_int"(%arg0, %arg1) :
  //     (tensor<4x!FHE.eint<0>>,
  //      tensor<4xi32>) -> (!FHE.eint<0>)
  //
  // becomes:
  //
  //   %0 = "FHE.zero"() : () -> !FHE.eint<0>
  //   %1 = tensor.from_elements %0 : tensor<1x!FHE.eint<0>>
  //   %2 = linalg.generic {
  //          indexing_maps = [#map0, #map0, #map1],
  //          iterator_types = ["reduction"]
  //        }
  //        ins(%arg0, %arg1 : tensor<2x!FHE.eint<0>>, tensor<2xi32>)
  //        outs(%1 : tensor<1x!FHE.eint<0>>) {
  //          ^bb0(%arg2: !FHE.eint<0>, %arg3: i32, %arg4: !FHE.eint<0>):
  //            %4 = "FHE.mul_eint_int"(%arg2, %arg3) :
  //                    (!FHE.eint<0>, i32) -> !FHE.eint<0>
  //
  //            %5 = "FHE.add_eint"(%4, %arg4) :
  //                    (!FHE.eint<0>, !FHE.eint<0>) -> !FHE.eint<0>
  //
  //            linalg.yield %5 : !FHE.eint<0>
  //        } -> tensor<1x!FHE.eint<0>>
  //
  //   %c0 = constant 0 : index
  //   %o = tensor.extract %2[%c0] : tensor<1x!FHE.eint<0>>
  //
  ::mlir::LogicalResult
  matchAndRewrite(::mlir::concretelang::FHELinalg::Dot dotOp,
                  ::mlir::PatternRewriter &rewriter) const override {
    // Zero value to initialize accumulator
    mlir::Value zeroCst = rewriter.create<mlir::concretelang::FHE::ZeroEintOp>(
        dotOp.getLoc(),
        dotOp.lhs().getType().cast<mlir::ShapedType>().getElementType());

    // Create one-dimensional accumulator with a single element
    // (`tensor.from_elements` does not allow for the creation of 0d
    // tensors)
    mlir::tensor::FromElementsOp feOp =
        rewriter.create<mlir::tensor::FromElementsOp>(dotOp.getLoc(), zeroCst);

    mlir::Value accu = feOp.getResult();

    // Create `linalg.generic` op
    llvm::SmallVector<mlir::Type, 1> resTypes{accu.getType()};
    llvm::SmallVector<mlir::Value, 2> ins{dotOp.lhs(), dotOp.rhs()};
    llvm::SmallVector<mlir::Value, 1> outs{accu};
    llvm::SmallVector<mlir::AffineMap, 3> maps{
        mlir::AffineMap::getMultiDimIdentityMap(1, this->getContext()),
        mlir::AffineMap::getMultiDimIdentityMap(1, this->getContext()),
        mlir::AffineMap::get(1, 0, {rewriter.getAffineConstantExpr(0)},
                             this->getContext())};

    llvm::SmallVector<llvm::StringRef, 1> itTypes{"reduction"};
    llvm::StringRef doc{""};
    llvm::StringRef call{""};

    auto regBuilder = [&](mlir::OpBuilder &nestedBuilder,
                          mlir::Location nestedLoc,
                          mlir::ValueRange blockArgs) {
      mlir::concretelang::FHE::MulEintIntOp mul =
          nestedBuilder.create<mlir::concretelang::FHE::MulEintIntOp>(
              dotOp.getLoc(), blockArgs[0], blockArgs[1]);
      mlir::concretelang::FHE::AddEintOp add =
          nestedBuilder.create<mlir::concretelang::FHE::AddEintOp>(
              dotOp.getLoc(), mul, blockArgs[2]);

      nestedBuilder.create<mlir::linalg::YieldOp>(dotOp.getLoc(),
                                                  add.getResult());
    };

    mlir::linalg::GenericOp gop = rewriter.create<mlir::linalg::GenericOp>(
        dotOp.getLoc(), resTypes, ins, outs, maps, itTypes, doc, call,
        regBuilder);

    // Return value is still a 1-dimensional tensor; extract first
    // element and use it as a replacement for the result of the dot
    // operation
    mlir::Value idx0 =
        rewriter.create<mlir::arith::ConstantIndexOp>(dotOp.getLoc(), 0);
    llvm::SmallVector<mlir::Value, 1> indexes{idx0};
    mlir::Value res = rewriter.create<mlir::tensor::ExtractOp>(
        dotOp.getLoc(), gop.getResult(0), indexes);

    rewriter.replaceOp(dotOp, {res});

    return ::mlir::success();
  };
};

mlir::AffineMap
getBroadcastedAffineMap(const mlir::RankedTensorType &resultType,
                        const mlir::RankedTensorType &operandType,
                        ::mlir::PatternRewriter &rewriter) {
  mlir::SmallVector<mlir::AffineExpr, 4> affineExprs;
  auto resultShape = resultType.getShape();
  auto operandShape = operandType.getShape();
  affineExprs.reserve(operandShape.size());
  size_t deltaNumDim = resultShape.size() - operandShape.size();
  for (size_t i = 0; i < operandShape.size(); i++) {
    if (operandShape[i] == 1 && resultShape[i + deltaNumDim] != 1) {
      affineExprs.push_back(rewriter.getAffineConstantExpr(0));
    } else {
      affineExprs.push_back(rewriter.getAffineDimExpr(i + deltaNumDim));
    }
  }
  return mlir::AffineMap::get(resultShape.size(), 0, affineExprs,
                              rewriter.getContext());
}

// This create an affine map following the broadcasting rules, but also takes
// out one specific element of the LUT from the LUT dimension, which should be
// the last.
//
// Example:
//
// resultType: 4x2x5, operandType: 4x2x8, lut_index: 3
// return: affine_map<(d0, d1, d2) -> (d0, d1, 3)
// last dimension of the operand is the lut size, and we take the map takes out
// the element at index 3
mlir::AffineMap
getBroadcastedAffineMapMultiLUT(const mlir::RankedTensorType &resultType,
                                const mlir::RankedTensorType &operandType,
                                const int64_t lut_index,
                                ::mlir::PatternRewriter &rewriter) {
  mlir::SmallVector<mlir::AffineExpr, 4> affineExprs;
  auto resultShape = resultType.getShape();
  auto operandShape = operandType.getShape();
  affineExprs.reserve(operandShape.size());
  // Don't take the lut dimension into account
  size_t deltaNumDim = resultShape.size() - operandShape.size() + 1;
  for (size_t i = 0; i < operandShape.size() - 1; i++) {
    if (operandShape[i] == 1 && resultShape[i + deltaNumDim] != 1) {
      affineExprs.push_back(rewriter.getAffineConstantExpr(0));
    } else {
      affineExprs.push_back(rewriter.getAffineDimExpr(i + deltaNumDim));
    }
  }
  // Index a specific element of the LUT
  affineExprs.push_back(rewriter.getAffineConstantExpr(lut_index));
  return mlir::AffineMap::get(resultShape.size(), 0, affineExprs,
                              rewriter.getContext());
}

// This template rewrite pattern transforms any instance of
// operators `FHELinalgOp` that implements the broadasting rules to an
// instance of `linalg.generic` with an appropriate region using `FHEOp`
// operation, an appropriate specification for the iteration dimensions and
// appropriate operations managing the accumulator of `linalg.generic`.
//
// Example:
//
// %res = FHELinalg.op(%lhs, %rhs):
// (tensor<D$Ax...xD1x!FHE.eint<p>>, tensor<D$B'x...xD1'xT>)
//    -> tensor<DR"x...xD1"x!FHE.eint<p>>
//
// becomes:
//
// #maps_0 = [
//    affine_map<(a$R", ..., a$A, ..., a1) ->
//        (dim(lhs, $A) == 1 ? 0 : a$A,..., dim(lhs, 1) == 1 ? 0 : a1)>,
//    affine_map<(a$R", ..., a1) ->
//        (dim(rhs, $B') == 1 ? 0 : a$B', ..., dim(rhs, 1) == 1 ? 0 : a1)>,
//    affine_map<(a$R", ..., a1) -> (a$R", ..., a1)
// ]
// #attributes_0 {
//     indexing_maps = #maps_0,
//     iterator_types = ["parallel", ..., "parallel"], // $R" parallel
// }
// %init = linalg.init_tensor [DR",...,D1"]
//            : tensor<DR"x...xD1"x!FHE.eint<p>>
// %res = linalg.generic {
//     ins(%lhs, %rhs: tensor<DAx...xD1x!FHE.eint<p>>,tensor<DB'x...xD1'xT>)
//     outs(%init : tensor<DR"x...xD1"x!FHE.eint<p>>)
//     {
//         ^bb0(%arg0: !FHE.eint<p>, %arg1: T):
//             %0 = FHE.op(%arg0, %arg1): !FHE.eint<p>, T ->
//             !FHE.eint<p>
//         linalg.yield %0 : !FHE.eint<p>
//     }
// }
//
template <typename FHELinalgOp, typename FHEOp>
struct FHELinalgOpToLinalgGeneric : public mlir::OpRewritePattern<FHELinalgOp> {
  FHELinalgOpToLinalgGeneric(::mlir::MLIRContext *context,
                             mlir::PatternBenefit benefit =
                                 mlir::concretelang::DEFAULT_PATTERN_BENEFIT)
      : ::mlir::OpRewritePattern<FHELinalgOp>(context, benefit) {}

  ::mlir::LogicalResult
  matchAndRewrite(FHELinalgOp linalgOp,
                  ::mlir::PatternRewriter &rewriter) const override {
    mlir::RankedTensorType resultTy =
        ((mlir::Type)linalgOp->getResult(0).getType())
            .cast<mlir::RankedTensorType>();
    mlir::RankedTensorType lhsTy =
        ((mlir::Type)linalgOp.lhs().getType()).cast<mlir::RankedTensorType>();
    mlir::RankedTensorType rhsTy =
        ((mlir::Type)linalgOp.rhs().getType()).cast<mlir::RankedTensorType>();
    //  linalg.init_tensor for initial value
    mlir::Value init = rewriter.create<mlir::linalg::InitTensorOp>(
        linalgOp.getLoc(), resultTy.getShape(), resultTy.getElementType());

    // Create the affine #maps_0
    llvm::SmallVector<mlir::AffineMap, 3> maps{
        getBroadcastedAffineMap(resultTy, lhsTy, rewriter),
        getBroadcastedAffineMap(resultTy, rhsTy, rewriter),
        getBroadcastedAffineMap(resultTy, resultTy, rewriter),
    };

    // Create the iterator_types
    llvm::SmallVector<llvm::StringRef> iteratorTypes(resultTy.getShape().size(),
                                                     "parallel");

    // Create the body of the `linalg.generic` op
    auto bodyBuilder = [&](mlir::OpBuilder &nestedBuilder,
                           mlir::Location nestedLoc,
                           mlir::ValueRange blockArgs) {
      FHEOp fheOp = nestedBuilder.create<FHEOp>(linalgOp.getLoc(), blockArgs[0],
                                                blockArgs[1]);

      nestedBuilder.create<mlir::linalg::YieldOp>(linalgOp.getLoc(),
                                                  fheOp.getResult());
    };

    // Create the `linalg.generic` op
    llvm::SmallVector<mlir::Type, 1> resTypes{init.getType()};
    llvm::SmallVector<mlir::Value, 2> ins{linalgOp.lhs(), linalgOp.rhs()};
    llvm::SmallVector<mlir::Value, 1> outs{init};
    llvm::StringRef doc{""};
    llvm::StringRef call{""};

    mlir::linalg::GenericOp genericOp =
        rewriter.create<mlir::linalg::GenericOp>(linalgOp.getLoc(), resTypes,
                                                 ins, outs, maps, iteratorTypes,
                                                 doc, call, bodyBuilder);

    rewriter.replaceOp(linalgOp, {genericOp.getResult(0)});

    return ::mlir::success();
  };
};

template <class T> inline mlir::RankedTensorType getRankedTensorType(T v) {
  return ((mlir::Type)v.getType()).cast<mlir::RankedTensorType>();
}

llvm::SmallVector<llvm::StringRef> parallelIteratorType(int n) {
  return llvm::SmallVector<llvm::StringRef>(n, "parallel");
}

// This class rewrite pattern transforms any instance of
// operators `FHELinalg.ApplyMappedLookupTableEintOp` that implements the
// broadasting rules to an instance of `linalg.generic` with an appropriate
// region using `FHE.ApplyLookupTableEintOp` operation, an appropriate
// specification for the iteration dimensions and appropriate operations
// managing the accumulator of `linalg.generic`.
//
// The current implementation does not rely on 'tensor.extract_slice'
// because of a bug in lowering this operation.
//
// Example:
// %res = "FHELinalg.apply_mapped_lookup_table"(%t, %luts, %map)
// : (tensor<2x3x!FHE.eint<2>>, tensor<5x4xi64>, tensor<2x3xindex>)
// -> tensor<2x3x!FHE.eint<2>>
//
// becomes:
//
// #map = affine_map<(d0, d1) -> (d0, d1)>
// %init = linalg.init_tensor [2, 3] : tensor<2x3x!TFHE.glwe<{_,_,_}{2}>>
// %output = linalg.generic {indexing_maps = [#map, #map, #map], iterator_types
// = ["parallel", "parallel"]} ins(%arg0, %arg2 :
// tensor<2x3x!TFHE.glwe<{_,_,_}{2}>>, tensor<2x3xindex>) outs(%0 :
// tensor<2x3x!TFHE.glwe<{_,_,_}{2}>>) {
//          ^bb0(%arg3: !TFHE.glwe<{_,_,_}{2}>, %lut_idx: index, %arg5:
//          !TFHE.glwe<{_,_,_}{2}>):  // no predecessors
//          // SHOULD BE
//          %lut = tensor.extract_slice %arg1[%[[LUTIDX]], 0] [1,4] [1, 1]
//                 : tensor<5x4xi64> to tensor<4xi64>
//          // BUT IS
//          %i0 = arith.constant 0 : index
//          ...
//          %i3 = arith.constant 3 : index
//          %e0 = tensor.extract %arg5[%lut_idx, %i0] : tensor<5x4xi64>
//          ...
//          %e3 = tensor.extract %arg5[%lut_idx, %i3] : tensor<5x4xi64>
//          %lut = tensor.from_elements %e0, ..., %e3 : tensor<4xi64>
//          %res  = "TFHE.apply_lookup_table"(%arg3, %[[LUT]])
//                    {baseLogBS = -1 : i32, baseLogKS = -1 : i32, k = -1 : i32,
//                      levelBS = -1 : i32, levelKS = -1 : i32, outputSizeKS =
//                      -1 : i32, polynomialSize = -1 : i32}
//                 : (!TFHE.glwe<{_,_,_}{2}>, tensor<4xi64>) ->
//          !TFHE.glwe<{_,_,_}{2}> linalg.yield %res :
//          !TFHE.glwe<{_,_,_}{2}>
// } -> tensor<2x3x!TFHE.glwe<{_,_,_}{2}>>

namespace FHELinalg = mlir::concretelang::FHELinalg;

struct FHELinalgApplyMappedLookupTableToLinalgGeneric
    : public mlir::OpRewritePattern<FHELinalg::ApplyMappedLookupTableEintOp> {
  FHELinalgApplyMappedLookupTableToLinalgGeneric(
      ::mlir::MLIRContext *context, mlir::PatternBenefit benefit = 1)
      : ::mlir::OpRewritePattern<FHELinalg::ApplyMappedLookupTableEintOp>(
            context, benefit) {}

  ::mlir::LogicalResult
  matchAndRewrite(FHELinalg::ApplyMappedLookupTableEintOp mappedLookup,
                  ::mlir::PatternRewriter &rewriter) const override {
    namespace arith = mlir::arith;
    namespace linalg = mlir::linalg;
    namespace tensor = mlir::tensor;
    namespace FHE = mlir::concretelang::FHE;
    using Values = llvm::SmallVector<mlir::Value>;
    using Types = llvm::SmallVector<mlir::Type>;
    using AffineMaps = llvm::SmallVector<mlir::AffineMap>;
    using sliceArg = llvm::SmallVector<mlir::OpFoldResult>;

    auto input = mappedLookup.t();
    auto luts = mappedLookup.luts();
    auto map = mappedLookup.map();

    auto loc = mappedLookup.getLoc();
    auto tensorTy = getRankedTensorType(input);
    auto lutsTy = getRankedTensorType(luts);
    auto resultTy = getRankedTensorType(mappedLookup->getResult(0));
    auto elementTy = resultTy.getElementType();
    auto lutElmtTy = lutsTy.getElementType();
    auto lutsShape = lutsTy.getShape();
    auto lutSize = lutsShape[lutsShape.size() - 1];
    auto resultShape = resultTy.getShape();

    auto integer = [&](auto v) -> mlir::Attribute {
      return rewriter.getI64IntegerAttr(v);
    };

    auto _0_ = integer(0);
    auto _1_ = integer(1);
    auto lutSizeValue = integer(lutSize);

    // Create the body of the `linalg.generic` op
    // %arg0 is an element of t (encrypted int)
    // %arg1 is an element of map (i64)
    // %arg2 is the output element
    auto lambdaBlock = [&](mlir::OpBuilder &nestedBuilder,
                           mlir::Location nestedLoc,
                           mlir::ValueRange blockArgs) {
      auto tElmt = blockArgs[0];
      auto lutIdx = blockArgs[1];
      auto indexTy = rewriter.getIndexType();

      // %lut = extract_slice %luts[%lutIdx, 0][1, lutSize][1, 1]  :
      // tensor<NxKxi64> to tensor<Kxi64>
      mlir::Value lut;
      const bool WORKAROUND_EXTRACT_SLICE = true;
      if (!WORKAROUND_EXTRACT_SLICE) {
        sliceArg offsets{lutIdx, _0_};
        sliceArg sizes{_1_, lutSizeValue};
        sliceArg strides{_1_, _1_};
        auto lutTy = mlir::RankedTensorType::get(
            {static_cast<int64_t>(lutSize)}, lutElmtTy);
        lut = nestedBuilder.create<tensor::ExtractSliceOp>(
            loc, lutTy, luts, offsets, sizes, strides);
      } else {
        // WORKAROUND BEGIN
        // A bug in linalg-bufferize prevents rank reduction in extract_slice
        // Reshaping does not work either or is too complicated so let's rebuild
        // the tensor from scratch
        llvm::SmallVector<mlir::Value> consts;
        llvm::SmallVector<mlir::Value> extracts;
        for (int i = 0; i < lutSize; i++) {
          consts.push_back(
              // %5 = arith.constant(<i> : index) : index
              nestedBuilder.create<mlir::arith::ConstantOp>(
                  loc, indexTy, rewriter.getIndexAttr(i)));
        }
        for (int i = 0; i < lutSize; i++) {
          extracts.push_back(
              // %8 = tensor.extract %luts[<lutIdx>, <i>] : ...
              nestedBuilder.create<tensor::ExtractOp>(
                  loc, luts, mlir::ValueRange({lutIdx, consts[i]})));
        }
        // %12 = tensor.from_elements %8, ... : ...
        lut = nestedBuilder.create<tensor::FromElementsOp>(loc, extracts);
      } // WORKAROUND END
      // %res1 = apply_lookup_table %arg0 %lut
      auto lookup = nestedBuilder.create<FHE::ApplyLookupTableEintOp>(
          loc, elementTy, tElmt, lut);
      // linalg.yield %res1 : !FHE.eint<2>
      nestedBuilder.create<linalg::YieldOp>(loc, lookup.getResult());
    };

    auto output =
        rewriter.create<linalg::InitTensorOp>(loc, resultShape, elementTy);

    // Create the `linalg.g eneric` op
    Types resTys{resultTy};
    Values ins{input, map};
    Values outs{output};
    auto indexOfInput = getBroadcastedAffineMap(resultTy, tensorTy, rewriter);
    AffineMaps affineMaps{indexOfInput, indexOfInput, indexOfInput};
    auto iteratorTypes = parallelIteratorType(resultShape.size());
    auto genericOp = rewriter.create<linalg::GenericOp>(
        loc, resTys, ins, outs, affineMaps, iteratorTypes, lambdaBlock);
    rewriter.replaceOp(mappedLookup, {genericOp.getResult(0)});

    return ::mlir::success();
  };
};

// This class rewrite pattern transforms any instance of
// operators `FHELinalg.ApplyMultiLookupTableEintOp` that implements the
// broadasting rules to an instance of `linalg.generic` with an appropriate
// region using `FHE.ApplyLookupTableEintOp` operation, an appropriate
// specification for the iteration dimensions and appropriate operaztions
// managing the accumulator of `linalg.generic`.
//
// Example:
//
// %res = "FHELinalg.apply_multi_lookup_table"(%t, %luts):
// (tensor<4x3x!FHE.eint<2>>, tensor<3x4xi64>) -> tensor<4x3x!FHE.eint<2>>
//
// becomes:
//
// #maps_0 = [
//    affine_map<(d0, d1) -> (d0, d1)>
//    affine_map<(d0, d1) -> (d1, 0)>
//    affine_map<(d0, d1) -> (d1, 1)>
//    affine_map<(d0, d1) -> (d1, 2)>
//    affine_map<(d0, d1) -> (d1, 3)>
// ]
// #attributes_0 {
//     indexing_maps = #maps_0,
//     iterator_types = ["parallel", "parallel"],
// }
// %init = linalg.init_tensor [4, 3]
//            : tensor<4x3x!FHE.eint<2>>
// %res = linalg.generic {
//     ins(%t, %luts, %luts, %luts, %luts: tensor<4x3x!FHE.eint<p>>,
//     tensor<3x4xi64>, tensor<3x4xi64>, tensor<3x4xi64>, tensor<3x4xi64>)
//     outs(%init : tensor<4x3x!FHE.eint<2>>)
//     {
//         ^bb0(%arg0: !FHE.eint<2>, %arg1: i64, %arg2: i64, %arg3: i64,
//         %arg4: i64, %arg5: !FHE.eint<2>):
//             %lut = tensor.from_elements %arg1, %arg2, %arg3, %arg4 :
//             tensor<4xi64> %0 = "TFHE.apply_lookup_table"(%arg0, %lut)
//             {baseLogBS = -1 : i32, baseLogKS = -1 : i32, glweDimension = -1 :
//             i32, levelBS = -1 : i32, levelKS = -1 : i32, outputSizeKS = -1 :
//             i32, polynomialSize = -1 : i32} : (!TFHE.glwe<{_,_,_}{2}>,
//             tensor<4xi64>) -> !TFHE.glwe<{_,_,_}{2}>
//         linalg.yield %0 : !FHE.eint<2>
//     }
// }
//
struct FHELinalgApplyMultiLookupTableToLinalgGeneric
    : public mlir::OpRewritePattern<
          mlir::concretelang::FHELinalg::ApplyMultiLookupTableEintOp> {
  FHELinalgApplyMultiLookupTableToLinalgGeneric(
      ::mlir::MLIRContext *context,
      mlir::PatternBenefit benefit =
          mlir::concretelang::DEFAULT_PATTERN_BENEFIT)
      : ::mlir::OpRewritePattern<
            mlir::concretelang::FHELinalg::ApplyMultiLookupTableEintOp>(
            context, benefit) {}

  ::mlir::LogicalResult matchAndRewrite(
      mlir::concretelang::FHELinalg::ApplyMultiLookupTableEintOp fheLinalgLutOp,
      ::mlir::PatternRewriter &rewriter) const override {
    mlir::RankedTensorType resultTy =
        ((mlir::Type)fheLinalgLutOp->getResult(0).getType())
            .cast<mlir::RankedTensorType>();
    mlir::RankedTensorType tensorTy = ((mlir::Type)fheLinalgLutOp.t().getType())
                                          .cast<mlir::RankedTensorType>();
    mlir::RankedTensorType lutsTy =
        ((mlir::Type)fheLinalgLutOp.luts().getType())
            .cast<mlir::RankedTensorType>();
    //  linalg.init_tensor for initial value
    mlir::Value init = rewriter.create<mlir::linalg::InitTensorOp>(
        fheLinalgLutOp.getLoc(), resultTy.getShape(),
        resultTy.getElementType());

    auto lutsShape = lutsTy.getShape();
    auto lut_size = lutsShape[lutsShape.size() - 1];
    // Create the affine maps
    llvm::SmallVector<mlir::AffineMap> maps{
        // Input tensor map
        getBroadcastedAffineMap(resultTy, tensorTy, rewriter)};
    maps.reserve(lut_size + 1);
    // Create as much affine maps as the size of the lut dimension
    for (int64_t i = 0; i < lut_size; i++)
      maps.push_back(
          getBroadcastedAffineMapMultiLUT(resultTy, lutsTy, i, rewriter));
    // Result map
    maps.push_back(getBroadcastedAffineMap(resultTy, resultTy, rewriter));

    // Create the iterator_types
    llvm::SmallVector<llvm::StringRef> iteratorTypes(resultTy.getShape().size(),
                                                     "parallel");

    // Create the body of the `linalg.generic` op
    auto bodyBuilder = [&](mlir::OpBuilder &nestedBuilder,
                           mlir::Location nestedLoc,
                           mlir::ValueRange blockArgs) {
      mlir::tensor::FromElementsOp lut =
          nestedBuilder.create<mlir::tensor::FromElementsOp>(
              fheLinalgLutOp.getLoc(), blockArgs.slice(1, lut_size));
      mlir::concretelang::FHE::ApplyLookupTableEintOp lutOp =
          nestedBuilder.create<mlir::concretelang::FHE::ApplyLookupTableEintOp>(
              fheLinalgLutOp.getLoc(), resultTy.getElementType(), blockArgs[0],
              lut.result());

      nestedBuilder.create<mlir::linalg::YieldOp>(fheLinalgLutOp.getLoc(),
                                                  lutOp.getResult());
    };

    // Create the `linalg.generic` op
    llvm::SmallVector<mlir::Type, 1> resTypes{init.getType()};
    llvm::SmallVector<mlir::Value> ins{fheLinalgLutOp.t()};
    ins.reserve(lut_size + 2);
    // We extract one value at a time from one LUT using different maps, so we
    // need to pass the LUT `lut_size` time
    for (auto i = 0; i < lut_size; i++)
      ins.push_back(fheLinalgLutOp.luts());
    llvm::SmallVector<mlir::Value, 1> outs{init};
    llvm::StringRef doc{""};
    llvm::StringRef call{""};

    mlir::linalg::GenericOp genericOp =
        rewriter.create<mlir::linalg::GenericOp>(
            fheLinalgLutOp.getLoc(), resTypes, ins, outs, maps, iteratorTypes,
            doc, call, bodyBuilder);

    rewriter.replaceOp(fheLinalgLutOp, {genericOp.getResult(0)});

    return ::mlir::success();
  };
};

// This template rewrite pattern transforms any instance of
// operators `FHELinalg.apply_lookup_table` that implements the broadasting
// rules to an instance of `linalg.generic` with an appropriate region using
// `FHE.apply_lookup_table` operation, an appropriate specification for the
// iteration dimensions and appropriate operations managing the accumulator of
// `linalg.generic`.
//
// Example:
//
// FHELinalg.apply_lookup_table(%t, %lut):
//  tensor<DNx...xD1x!FHE.eint<p>>, tensor<DAxi64>
//      -> tensor<DNx...xD1x!FHE.eint<p'>>
//
// becomes:
//
// #maps_0 = [
//    affine_map<(aN, ..., a1) -> (aN, ..., a1)>,
//    affine_map<(aN, ..., a1) -> (aN, ..., a1)>
// ]
// #attributes_0 {
//     indexing_maps = #maps_0,
//     iterator_types = ["parallel",..],//N parallel
// }
// %init = linalg.init_tensor [DN,...,D1]
//            : tensor<DNx...xD1x!FHE.eint<p'>>
// %res = linalg.generic {
//     ins(%t: tensor<DNx...xD1x!FHE.eint<p>>)
//     outs(%init : tensor<DNx...xD1x!FHE.eint<p'>>)
//     {
//         ^bb0(%arg0: !FHE.eint<p>):
//             %0 = FHE.apply_lookup_table(%arg0, %lut): !FHE.eint<p>,
//             tensor<4xi64> -> !FHE.eint<p'>
//         linalg.yield %0 : !FHE.eint<p'>
//     }
// }
//
struct FHELinalgApplyLookupTableToLinalgGeneric
    : public mlir::OpRewritePattern<
          mlir::concretelang::FHELinalg::ApplyLookupTableEintOp> {
  FHELinalgApplyLookupTableToLinalgGeneric(
      ::mlir::MLIRContext *context,
      mlir::PatternBenefit benefit =
          mlir::concretelang::DEFAULT_PATTERN_BENEFIT)
      : ::mlir::OpRewritePattern<
            mlir::concretelang::FHELinalg::ApplyLookupTableEintOp>(context,
                                                                   benefit) {}

  ::mlir::LogicalResult
  matchAndRewrite(mlir::concretelang::FHELinalg::ApplyLookupTableEintOp lutOp,
                  ::mlir::PatternRewriter &rewriter) const override {
    mlir::RankedTensorType resultTy =
        ((mlir::Type)lutOp->getResult(0).getType())
            .cast<mlir::RankedTensorType>();
    mlir::RankedTensorType tTy =
        ((mlir::Type)lutOp.t().getType()).cast<mlir::RankedTensorType>();

    //  linalg.init_tensor for initial value
    mlir::Value init = rewriter.create<mlir::linalg::InitTensorOp>(
        lutOp.getLoc(), resultTy.getShape(), resultTy.getElementType());

    // Create the affine #maps_0
    llvm::SmallVector<mlir::AffineMap, 2> maps{
        mlir::AffineMap::getMultiDimIdentityMap(tTy.getShape().size(),
                                                this->getContext()),
        mlir::AffineMap::getMultiDimIdentityMap(resultTy.getShape().size(),
                                                this->getContext()),
    };

    // Create the iterator_types
    llvm::SmallVector<llvm::StringRef> iteratorTypes(resultTy.getShape().size(),
                                                     "parallel");

    // Create the body of the `linalg.generic` op
    auto bodyBuilder = [&](mlir::OpBuilder &nestedBuilder,
                           mlir::Location nestedLoc,
                           mlir::ValueRange blockArgs) {
      mlir::concretelang::FHE::ApplyLookupTableEintOp fheOp =
          nestedBuilder.create<mlir::concretelang::FHE::ApplyLookupTableEintOp>(
              lutOp.getLoc(), resultTy.getElementType(), blockArgs[0],
              lutOp.lut());

      nestedBuilder.create<mlir::linalg::YieldOp>(lutOp.getLoc(),
                                                  fheOp.getResult());
    };

    // Create the `linalg.generic` op
    llvm::SmallVector<mlir::Type, 1> resTypes{init.getType()};
    llvm::SmallVector<mlir::Value, 1> ins{lutOp.t()};
    llvm::SmallVector<mlir::Value, 1> outs{init};
    llvm::StringRef doc{""};
    llvm::StringRef call{""};

    mlir::linalg::GenericOp genericOp =
        rewriter.create<mlir::linalg::GenericOp>(lutOp.getLoc(), resTypes, ins,
                                                 outs, maps, iteratorTypes, doc,
                                                 call, bodyBuilder);

    rewriter.replaceOp(lutOp, {genericOp.getResult(0)});

    return ::mlir::success();
  };
};

// This template rewrite pattern transforms any instance of
// operators `FHELinalg.neg_eint` to an instance of `linalg.generic` with an
// appropriate region using `FHE.neg_eint` operation, an appropriate
// specification for the iteration dimensions and appropriate operations
// managing the accumulator of `linalg.generic`.
//
// Example:
//
// FHELinalg.neg_eint(%tensor):
//  tensor<DNx...xD1x!FHE.eint<p>> -> tensor<DNx...xD1x!FHE.eint<p'>>
//
// becomes:
//
// #maps_0 = [
//    affine_map<(aN, ..., a1) -> (aN, ..., a1)>,
//    affine_map<(aN, ..., a1) -> (aN, ..., a1)>
// ]
// #attributes_0 {
//     indexing_maps = #maps_0,
//     iterator_types = ["parallel",..],//N parallel
// }
// %init = linalg.init_tensor [DN,...,D1]
//            : tensor<DNx...xD1x!FHE.eint<p'>>
// %res = linalg.generic {
//     ins(%tensor: tensor<DNx...xD1x!FHE.eint<p>>)
//     outs(%init : tensor<DNx...xD1x!FHE.eint<p'>>)
//     {
//         ^bb0(%arg0: !FHE.eint<p>):
//             %0 = FHE.neg_eint(%arg0): !FHE.eint<p> -> !FHE.eint<p'>
//         linalg.yield %0 : !FHE.eint<p'>
//     }
// }
//
struct FHELinalgNegEintToLinalgGeneric
    : public mlir::OpRewritePattern<mlir::concretelang::FHELinalg::NegEintOp> {
  FHELinalgNegEintToLinalgGeneric(
      ::mlir::MLIRContext *context,
      mlir::PatternBenefit benefit =
          mlir::concretelang::DEFAULT_PATTERN_BENEFIT)
      : ::mlir::OpRewritePattern<mlir::concretelang::FHELinalg::NegEintOp>(
            context, benefit) {}

  ::mlir::LogicalResult
  matchAndRewrite(mlir::concretelang::FHELinalg::NegEintOp negEintOp,
                  ::mlir::PatternRewriter &rewriter) const override {
    mlir::RankedTensorType resultTy =
        ((mlir::Type)negEintOp->getResult(0).getType())
            .cast<mlir::RankedTensorType>();
    mlir::RankedTensorType tensorTy = ((mlir::Type)negEintOp.tensor().getType())
                                          .cast<mlir::RankedTensorType>();

    //  linalg.init_tensor for initial value
    mlir::Value init = rewriter.create<mlir::linalg::InitTensorOp>(
        negEintOp.getLoc(), resultTy.getShape(), resultTy.getElementType());

    // Create the affine #maps_0
    llvm::SmallVector<mlir::AffineMap, 2> maps{
        mlir::AffineMap::getMultiDimIdentityMap(tensorTy.getShape().size(),
                                                this->getContext()),
        mlir::AffineMap::getMultiDimIdentityMap(resultTy.getShape().size(),
                                                this->getContext()),
    };

    // Create the iterator_types
    llvm::SmallVector<llvm::StringRef> iteratorTypes(resultTy.getShape().size(),
                                                     "parallel");

    // Create the body of the `linalg.generic` op
    auto bodyBuilder = [&](mlir::OpBuilder &nestedBuilder,
                           mlir::Location nestedLoc,
                           mlir::ValueRange blockArgs) {
      mlir::concretelang::FHE::NegEintOp fheOp =
          nestedBuilder.create<mlir::concretelang::FHE::NegEintOp>(
              negEintOp.getLoc(), resultTy.getElementType(), blockArgs[0]);

      nestedBuilder.create<mlir::linalg::YieldOp>(negEintOp.getLoc(),
                                                  fheOp.getResult());
    };

    // Create the `linalg.generic` op
    llvm::SmallVector<mlir::Type, 1> resTypes{init.getType()};
    llvm::SmallVector<mlir::Value, 1> ins{negEintOp.tensor()};
    llvm::SmallVector<mlir::Value, 1> outs{init};
    llvm::StringRef doc{""};
    llvm::StringRef call{""};

    mlir::linalg::GenericOp genericOp =
        rewriter.create<mlir::linalg::GenericOp>(negEintOp.getLoc(), resTypes,
                                                 ins, outs, maps, iteratorTypes,
                                                 doc, call, bodyBuilder);

    rewriter.replaceOp(negEintOp, {genericOp.getResult(0)});

    return ::mlir::success();
  };
};

// This template rewrite pattern transforms any instance of
// operators `FHELinalgMatmulOp` to an instance of `linalg.generic`
// with an appropriate region using a builder that create the multiplication
// operators and `FHE.add_eint` operation, an appropriate specification for
// the iteration dimensions and appropriate operations managing the accumulator
// of `linalg.generic`.
//
// Example:
//
//  "FHELinalg.matmul_eint_int(%a, %b) :
//      (tensor<MxPx!FHE.eint<p>>, tensor<PxNxip'>) ->
//          tensor<MxNx!FHE.eint<p>>"

//
// becomes:
//
// #maps_0 = [
//   (m, n, p) -> (m, p),
//   (m, n, p) -> (p, n),
//   (m, n, p) -> (m, n)
// ]
// #attributes_0 = {
//   indexing_maps = #maps_0,
//   iterator_types = ["parallel", "parallel", "reduction"]
// }
// %init = linalg.generate {
//   ^bb0(%i : index, %j : index, %k : index):
//     %z = "FHE.zero" : () -> !FHE.eint<2>
//     linalg.yield %z
// }: tensor<MxNx!FHE.eint<p>>
// linalg.generic #attributes_0
//   ins(%A, %B : tensor<MxPx!FHE.eint<p>>,
//                tensor<PxNxip'>)
//   outs(%C : tensor<MxNx!FHE.eint<p>>)
//   {
//      ^bb0(%a: !FHE.eint<p>, %b: ip', %c: !FHE.eint<p>) :
//        %d = createMulOp(%a, %b): !FHE.eint<p>
//        %e = "FHE.add_eint"(%c, %d):
//              (!FHE.eint<p>, !FHE.eint<p>) -> !FHE.eint<p>
//        linalg.yield %e : !FHE.eint<p>
//   }
//
template <typename FHELinalgMatmulOp>
struct FHELinalgMatmulToLinalgGeneric
    : public mlir::OpRewritePattern<FHELinalgMatmulOp> {
  FHELinalgMatmulToLinalgGeneric(
      mlir::MLIRContext *context,
      std::function<mlir::concretelang::FHE::MulEintIntOp(
          mlir::OpBuilder &, mlir::Location, mlir::Type, mlir::Value,
          mlir::Value)>
          createMulOp,
      mlir::PatternBenefit benefit =
          mlir::concretelang::DEFAULT_PATTERN_BENEFIT)
      : ::mlir::OpRewritePattern<FHELinalgMatmulOp>(context, benefit),
        createMulOp(createMulOp) {}

  ::mlir::LogicalResult
  matchAndRewrite(FHELinalgMatmulOp matmulOp,
                  ::mlir::PatternRewriter &rewriter) const override {
    mlir::Location matmulLoc = matmulOp.getLoc();
    mlir::RankedTensorType resultTy =
        ((mlir::Type)matmulOp->getResult(0).getType())
            .cast<mlir::RankedTensorType>();
    mlir::Type resultElementTy = resultTy.getElementType();
    // Create tensor.generate for initial value
    auto generateBody = [&](mlir::OpBuilder &nestedBuilder,
                            mlir::Location nestedLoc,
                            mlir::ValueRange blockArgs) {
      // %z = "FHE.zero" : () -> !FHE.eint<2>
      mlir::concretelang::FHE::ZeroEintOp zeroOp =
          nestedBuilder.create<mlir::concretelang::FHE::ZeroEintOp>(
              matmulLoc, resultElementTy);
      // linalg.yield %z : !FHE.eint<p>
      nestedBuilder.create<mlir::tensor::YieldOp>(matmulLoc,
                                                  zeroOp.getResult());
    };
    mlir::tensor::GenerateOp init = rewriter.create<mlir::tensor::GenerateOp>(
        matmulLoc, (mlir::Type)resultTy, mlir::ValueRange{}, generateBody);
    // Create the affine #maps_0
    llvm::SmallVector<mlir::AffineMap> maps{
        // (m, n, p) -> (m, p),
        mlir::AffineMap::get(
            3, 0, {rewriter.getAffineDimExpr(0), rewriter.getAffineDimExpr(2)},
            rewriter.getContext()),
        // (m, n, p) -> (p, n),
        mlir::AffineMap::get(
            3, 0, {rewriter.getAffineDimExpr(2), rewriter.getAffineDimExpr(1)},
            rewriter.getContext()),
        // (m, n, p) -> (m, n)
        mlir::AffineMap::get(
            3, 0, {rewriter.getAffineDimExpr(0), rewriter.getAffineDimExpr(1)},
            rewriter.getContext()),
    };

    // Create the iterator_types
    llvm::SmallVector<llvm::StringRef> iteratorTypes{"parallel", "parallel",
                                                     "reduction"};

    // Create the body of the `linalg.generic` op
    auto bodyBuilder = [&](mlir::OpBuilder &nestedBuilder,
                           mlir::Location nestedLoc,
                           mlir::ValueRange blockArgs) {
      // "FHE.mul_eint_int"(%a, %b) : (!FHE.eint<p>, ip') -> !FHE.eint<p>
      mlir::concretelang::FHE::MulEintIntOp mulEintIntOp =
          createMulOp(nestedBuilder, matmulLoc, resultElementTy, blockArgs[0],
                      blockArgs[1]);
      // "FHE.add_eint"(%c, %d): (!FHE.eint<p>, !FHE.eint<p>) ->
      // !FHE.eint<p>
      mlir::concretelang::FHE::AddEintOp addEintOp =
          nestedBuilder.create<mlir::concretelang::FHE::AddEintOp>(
              matmulLoc, resultElementTy, blockArgs[2], mulEintIntOp);
      // linalg.yield %e : !FHE.eint<p>
      nestedBuilder.create<mlir::linalg::YieldOp>(matmulLoc,
                                                  addEintOp.getResult());
    };

    // Create the `linalg.generic` op
    llvm::SmallVector<mlir::Type> resTypes{init.getType()};
    llvm::SmallVector<mlir::Value> ins{matmulOp.lhs(), matmulOp.rhs()};
    llvm::SmallVector<mlir::Value> outs{init};
    llvm::StringRef doc{""};
    llvm::StringRef call{""};

    mlir::linalg::GenericOp genericOp =
        rewriter.create<mlir::linalg::GenericOp>(matmulLoc, resTypes, ins, outs,
                                                 maps, iteratorTypes, doc, call,
                                                 bodyBuilder);

    rewriter.replaceOp(matmulOp, {genericOp.getResult(0)});

    return ::mlir::success();
  };

private:
  std::function<mlir::concretelang::FHE::MulEintIntOp(
      mlir::OpBuilder &, mlir::Location, mlir::Type, mlir::Value, mlir::Value)>
      createMulOp;
};

// This rewrite pattern transforms any instance of operators
// `FHELinalg.zero` to an instance of `linalg.generate` with an
// appropriate region yielding a zero value.
//
// Example:
//
//   %out = "FHELinalg.zero"() : () -> tensor<MxNx!FHE.eint<p>>
//
// becomes:
//
//   %0 = tensor.generate   {
//     ^bb0(%arg2: index, %arg3: index):
//        %zero = "FHE.zero"() : () -> !FHE.eint<p>
//        tensor.yield %zero : !FHE.eint<p>
//   } : tensor<MxNx!FHE.eint<p>>
//
struct FHELinalgZeroToLinalgGenerate
    : public mlir::OpRewritePattern<mlir::concretelang::FHELinalg::ZeroOp> {
  FHELinalgZeroToLinalgGenerate(::mlir::MLIRContext *context,
                                mlir::PatternBenefit benefit =
                                    mlir::concretelang::DEFAULT_PATTERN_BENEFIT)
      : ::mlir::OpRewritePattern<mlir::concretelang::FHELinalg::ZeroOp>(
            context, benefit) {}

  ::mlir::LogicalResult
  matchAndRewrite(mlir::concretelang::FHELinalg::ZeroOp zeroOp,
                  ::mlir::PatternRewriter &rewriter) const override {
    mlir::RankedTensorType resultTy =
        zeroOp->getResult(0).getType().cast<mlir::RankedTensorType>();

    auto generateBody = [&](mlir::OpBuilder &nestedBuilder,
                            mlir::Location nestedLoc,
                            mlir::ValueRange blockArgs) {
      mlir::Value zeroScalar =
          nestedBuilder.create<mlir::concretelang::FHE::ZeroEintOp>(
              zeroOp.getLoc(), resultTy.getElementType());
      nestedBuilder.create<mlir::tensor::YieldOp>(zeroOp.getLoc(), zeroScalar);
    };
    mlir::tensor::GenerateOp generateOp =
        rewriter.create<mlir::tensor::GenerateOp>(
            zeroOp.getLoc(), resultTy, mlir::ValueRange{}, generateBody);

    rewriter.replaceOp(zeroOp, {generateOp.getResult()});

    return ::mlir::success();
  };
};

namespace {
struct FHETensorOpsToLinalg
    : public FHETensorOpsToLinalgBase<FHETensorOpsToLinalg> {

  void runOnFunction() final;
};

void FHETensorOpsToLinalg::runOnFunction() {
  mlir::FuncOp function = this->getFunction();

  mlir::ConversionTarget target(getContext());

  target.addLegalDialect<mlir::linalg::LinalgDialect>();
  target.addLegalDialect<mlir::StandardOpsDialect>();
  target.addLegalDialect<mlir::memref::MemRefDialect>();
  target.addLegalDialect<mlir::concretelang::FHE::FHEDialect>();
  target.addLegalDialect<mlir::tensor::TensorDialect>();
  target.addLegalDialect<mlir::arith::ArithmeticDialect>();
  target.addIllegalOp<mlir::concretelang::FHELinalg::Dot>();
  target.addIllegalDialect<mlir::concretelang::FHELinalg::FHELinalgDialect>();

  mlir::OwningRewritePatternList patterns(&getContext());
  patterns.insert<DotToLinalgGeneric>(&getContext());
  patterns.insert<
      FHELinalgOpToLinalgGeneric<mlir::concretelang::FHELinalg::AddEintOp,
                                 mlir::concretelang::FHE::AddEintOp>>(
      &getContext());
  patterns.insert<
      FHELinalgOpToLinalgGeneric<mlir::concretelang::FHELinalg::AddEintIntOp,
                                 mlir::concretelang::FHE::AddEintIntOp>>(
      &getContext());
  patterns.insert<
      FHELinalgOpToLinalgGeneric<mlir::concretelang::FHELinalg::SubIntEintOp,
                                 mlir::concretelang::FHE::SubIntEintOp>>(
      &getContext());
  patterns.insert<
      FHELinalgOpToLinalgGeneric<mlir::concretelang::FHELinalg::MulEintIntOp,
                                 mlir::concretelang::FHE::MulEintIntOp>>(
      &getContext());
  patterns.insert<FHELinalgApplyLookupTableToLinalgGeneric>(&getContext());
  patterns.insert<FHELinalgNegEintToLinalgGeneric>(&getContext());
  patterns.insert<FHELinalgMatmulToLinalgGeneric<
      mlir::concretelang::FHELinalg::MatMulEintIntOp>>(
      &getContext(), [](mlir::OpBuilder &builder, mlir::Location loc,
                        mlir::Type type, mlir::Value arg0, mlir::Value arg1) {
        return builder.create<mlir::concretelang::FHE::MulEintIntOp>(
            loc, type, arg0, arg1);
      });
  patterns.insert<FHELinalgMatmulToLinalgGeneric<
      mlir::concretelang::FHELinalg::MatMulIntEintOp>>(
      &getContext(), [](mlir::OpBuilder &builder, mlir::Location loc,
                        mlir::Type type, mlir::Value arg0, mlir::Value arg1) {
        return builder.create<mlir::concretelang::FHE::MulEintIntOp>(
            loc, type, arg1, arg0);
      });
  patterns.insert<FHELinalgApplyMultiLookupTableToLinalgGeneric>(&getContext());
  patterns.insert<FHELinalgApplyMappedLookupTableToLinalgGeneric>(
      &getContext());
  patterns.insert<FHELinalgZeroToLinalgGenerate>(&getContext());

  if (mlir::applyPartialConversion(function, target, std::move(patterns))
          .failed())
    this->signalPassFailure();
}

} // namespace

namespace mlir {
namespace concretelang {
std::unique_ptr<mlir::FunctionPass> createConvertFHETensorOpsToLinalg() {
  return std::make_unique<FHETensorOpsToLinalg>();
}
} // namespace concretelang
} // namespace mlir
