#pragma once
/**
 * @file SameBits.hpp
 * @brief Equality of floating-point values in every bit, for the tests that pin the order of
 *        a floating-point sum.
 */
#include <Eigen/Dense>

#include <cstring>

namespace gnss_test {

/// Whether two doubles hold the same bits (so +0.0 and -0.0 differ and a NaN equals the same NaN).
inline bool sameBits(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }

/// Whether two vectors have the same size and every entry the same bits.
inline bool sameBits(const Eigen::VectorXd& a, const Eigen::VectorXd& b) {
    if (a.size() != b.size()) return false;
    for (Eigen::Index i = 0; i < a.size(); ++i)
        if (!sameBits(a(i), b(i))) return false;
    return true;
}

}  // namespace gnss_test
