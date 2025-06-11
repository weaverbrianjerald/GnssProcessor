#include <iostream>
#include <spdlog/spdlog.h>
#include <Eigen/Dense>

int main() {
    spdlog::info("GNSS PPP-RTK Processor started");
    Eigen::MatrixXd test_matrix = Eigen::MatrixXd::Identity(3, 3);
    std::cout << "Test Matrix:\n" << test_matrix << std::endl;
    return 0;
}