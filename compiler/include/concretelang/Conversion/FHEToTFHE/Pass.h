// Part of the Concrete Compiler Project, under the BSD3 License with Zama
// Exceptions. See
// https://github.com/zama-ai/homomorphizer/blob/master/LICENSE.txt for license
// information.

#ifndef CONCRETELANG_CONVERSION_FHETOTFHE_PASS_H_
#define CONCRETELANG_CONVERSION_FHETOTFHE_PASS_H_

#include "mlir/Pass/Pass.h"

namespace mlir {
namespace concretelang {
/// Create a pass to convert `FHE` dialect to `TFHE` dialect.
std::unique_ptr<OperationPass<ModuleOp>> createConvertFHEToTFHEPass();
} // namespace concretelang
} // namespace mlir

#endif