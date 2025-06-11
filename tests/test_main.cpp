#include <gtest/gtest.h>
#include <Eigen/Dense>

TEST(TestMatrix, TestIdentity) {
    Eigen::MatrixXd mat = Eigen::MatrixXd::Identity(3, 3);
    EXPECT_EQ(mat.sum(), 3.0);
}