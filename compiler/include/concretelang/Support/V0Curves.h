// Part of the Concrete Compiler Project, under the BSD3 License with Zama
// Exceptions. See
// https://github.com/zama-ai/homomorphizer/blob/master/LICENSE.txt for license
// information.

#ifndef CONCRETELANG_SUPPORT_V0CURVES_H_
#define CONCRETELANG_SUPPORT_V0CURVES_H_

#include <cstddef>

namespace mlir {
namespace concretelang {

#define SECURITY_LEVEL_80 0
#define SECURITY_LEVEL_128 1
#define SECURITY_LEVEL_192 2
#define SECURITY_LEVEL_256 3
#define SECURITY_LEVEL_MAX 4

#define KEY_FORMAT_BINARY 0
#define KEY_FORMAT_MAX 1

struct V0Curves {
  int securityLevel;
  double linearTerm1;
  double linearTerm2;
  int nAlpha;
  int keyFormat;
  V0Curves(int securityLevel, double linearTerm1, double linearTerm2,
           int nAlpha, int keyFormat)
      : securityLevel(securityLevel), linearTerm1(linearTerm1),
        linearTerm2(linearTerm2), nAlpha(nAlpha), keyFormat(keyFormat) {}

  double getVariance(int glweDimension, int polynomialSize, int logQ) {
    auto a = std::pow(
        2, (linearTerm1 * glweDimension * polynomialSize + linearTerm2) * 2);
    auto b = std::pow(2, -2 * (logQ - 2));
    return a > b ? a : b;
  }
};

V0Curves *getV0Curves(int securityLevel, int keyFormat);

} // namespace concretelang
} // namespace mlir
#endif