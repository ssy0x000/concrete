// RUN: concretecompiler --passes concrete-to-bconcrete --action=dump-bconcrete --split-input-file %s 2>&1| FileCheck %s

// CHECK: func.func @main(%[[A0:.*]]: tensor<2049xi64>, %[[A1:.*]]: tensor<2049xi64>, %[[A2:.*]]: tensor<2049xi64>, %[[A3:.*]]: tensor<2049xi64>, %[[A4:.*]]: tensor<2049xi64>, %[[A5:.*]]: tensor<2049xi64>) -> tensor<6x2049xi64> {
// CHECK:   %[[V0:.*]] = bufferization.alloc_tensor() : tensor<6x2049xi64>
// CHECK:   %[[V1:.*]] = tensor.insert_slice %[[A0]] into %[[V0]][0, 0] [1, 2049] [1, 1] : tensor<2049xi64> into tensor<6x2049xi64>
// CHECK:   %[[V2:.*]] = tensor.insert_slice %[[A1]] into %[[V1]][1, 0] [1, 2049] [1, 1] : tensor<2049xi64> into tensor<6x2049xi64>
// CHECK:   %[[V3:.*]] = tensor.insert_slice %[[A2]] into %[[V2]][2, 0] [1, 2049] [1, 1] : tensor<2049xi64> into tensor<6x2049xi64>
// CHECK:   %[[V4:.*]] = tensor.insert_slice %[[A3]] into %[[V3]][3, 0] [1, 2049] [1, 1] : tensor<2049xi64> into tensor<6x2049xi64>
// CHECK:   %[[V5:.*]] = tensor.insert_slice %[[A4]] into %[[V4]][4, 0] [1, 2049] [1, 1] : tensor<2049xi64> into tensor<6x2049xi64>
// CHECK:   %[[V6:.*]] = tensor.insert_slice %[[A5]] into %[[V5]][5, 0] [1, 2049] [1, 1] : tensor<2049xi64> into tensor<6x2049xi64>
// CHECK:   return %[[V6]] : tensor<6x2049xi64>
// CHECK: }
func.func @main(%arg0 : !Concrete.lwe_ciphertext<2048,4>, %arg1 : !Concrete.lwe_ciphertext<2048,4>, %arg2 : !Concrete.lwe_ciphertext<2048,4>, %arg3 : !Concrete.lwe_ciphertext<2048,4>, %arg4 : !Concrete.lwe_ciphertext<2048,4>, %arg5 : !Concrete.lwe_ciphertext<2048,4>) -> tensor<6x!Concrete.lwe_ciphertext<2048,4>> {
  %0 = tensor.from_elements %arg0, %arg1, %arg2, %arg3, %arg4, %arg5 : tensor<6x!Concrete.lwe_ciphertext<2048,4>>
  return %0 : tensor<6x!Concrete.lwe_ciphertext<2048,4>>
}

// -----

// CHECK: func.func @main(%[[A0:.*]]: tensor<2049xi64>, %[[A1:.*]]: tensor<2049xi64>, %[[A2:.*]]: tensor<2049xi64>, %[[A3:.*]]: tensor<2049xi64>, %[[A4:.*]]: tensor<2049xi64>, %[[A5:.*]]: tensor<2049xi64>) -> tensor<2x3x2049xi64> {
// CHECK:   %[[V0:.*]] = bufferization.alloc_tensor() : tensor<2x3x2049xi64>
// CHECK:   %[[V1:.*]] = tensor.insert_slice %[[A0]] into %[[V0]][0, 0, 0] [1, 1, 2049] [1, 1, 1] : tensor<2049xi64> into tensor<2x3x2049xi64>
// CHECK:   %[[V2:.*]] = tensor.insert_slice %[[A1]] into %[[V1]][0, 1, 0] [1, 1, 2049] [1, 1, 1] : tensor<2049xi64> into tensor<2x3x2049xi64>
// CHECK:   %[[V3:.*]] = tensor.insert_slice %[[A2]] into %[[V2]][0, 2, 0] [1, 1, 2049] [1, 1, 1] : tensor<2049xi64> into tensor<2x3x2049xi64>
// CHECK:   %[[V4:.*]] = tensor.insert_slice %[[A3]] into %[[V3]][1, 0, 0] [1, 1, 2049] [1, 1, 1] : tensor<2049xi64> into tensor<2x3x2049xi64>
// CHECK:   %[[V5:.*]] = tensor.insert_slice %[[A4]] into %[[V4]][1, 1, 0] [1, 1, 2049] [1, 1, 1] : tensor<2049xi64> into tensor<2x3x2049xi64>
// CHECK:   %[[V6:.*]] = tensor.insert_slice %[[A5]] into %[[V5]][1, 2, 0] [1, 1, 2049] [1, 1, 1] : tensor<2049xi64> into tensor<2x3x2049xi64>
// CHECK:   return %[[V6]] : tensor<2x3x2049xi64>
// CHECK: }
func.func @main(%arg0 : !Concrete.lwe_ciphertext<2048,4>, %arg1 : !Concrete.lwe_ciphertext<2048,4>, %arg2 : !Concrete.lwe_ciphertext<2048,4>, %arg3 : !Concrete.lwe_ciphertext<2048,4>, %arg4 : !Concrete.lwe_ciphertext<2048,4>, %arg5 : !Concrete.lwe_ciphertext<2048,4>) -> tensor<2x3x!Concrete.lwe_ciphertext<2048,4>> {
  %0 = tensor.from_elements %arg0, %arg1, %arg2, %arg3, %arg4, %arg5 : tensor<2x3x!Concrete.lwe_ciphertext<2048,4>>
  return %0 : tensor<2x3x!Concrete.lwe_ciphertext<2048,4>>
}
