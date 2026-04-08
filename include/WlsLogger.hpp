#pragma once

#include <Eigen/Dense>
#include <fstream>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @file WlsLogger.hpp
     * @brief Lightweight logger for recording WLS (Weighted Least Squares) internal data during processing.
     *
     * This header declares the `WlsLogger` class which provides convenience functions to persist
     * state vectors, covariance matrices, design matrices, innovations and weight/normal matrices
     * to a text file for debugging, analysis or offline inspection.
     */

    /**
     * @class WlsLogger
     * @brief Provides methods to write WLS-related matrices and vectors to a log file.
     *
     * The logger writes human-readable lines containing a timestamp, a label, a GPS time-of-week (TOW),
     * the iteration index and optional epoch information followed by the serialized matrix/vector contents.
     * The implementation is responsible for opening and closing the file.
     */
    class WlsLogger {
    public:
        /**
         * @brief Construct a new WlsLogger and open the target log file.
         * @param filename Path to the output log file. The file will be opened for appending/truncation
         *                 depending on the implementation in the .cpp.
         */
        WlsLogger(const std::string& filename);

        /**
         * @brief Destroy the WlsLogger and close the log file if open.
         */
        ~WlsLogger();

        /**
         * @brief Log a state vector (e.g., estimated parameters).
         * @param state Eigen vector containing the state values.
         * @param label Short label identifying the logged data (e.g., "state_post").
         * @param tow GPS time-of-week (seconds) for the epoch being logged.
         * @param iteration Iteration index within the solver (0-based or 1-based depending on caller).
         * @param epoch_info Optional free-form string with additional epoch metadata.
         */
        void logStateVector(const Eigen::VectorXd& state, const std::string& label, double tow, int iteration,
            const std::string& epoch_info = {});

        /**
         * @brief Log a covariance matrix associated with a state estimate.
         * @param covariance Symmetric covariance matrix (size matches the corresponding state vector).
         * @param label Short label identifying the logged data (e.g., "cov_post").
         * @param tow GPS time-of-week (seconds) for the epoch being logged.
         * @param iteration Iteration index within the solver.
         * @param epoch_info Optional free-form string with additional epoch metadata.
         */
        void logCovarianceMatrix(const Eigen::MatrixXd& covariance, const std::string& label, double tow, int iteration,
            const std::string& epoch_info = {});

        /**
         * @brief Log the design (geometry) matrix H.
         * @param H Design matrix (rows: observations, cols: parameters).
         * @param label Short label identifying the logged data (e.g., "design").
         * @param tow GPS time-of-week (seconds) for the epoch being logged.
         * @param iteration Iteration index within the solver.
         * @param satellite_ids Vector of satellite identifiers corresponding to each observation row.
         *                      Should have length equal to H.rows().
         * @param signal_types Vector of signal type strings corresponding to each observation row.
         *                     Should have length equal to H.rows().
         * @param epoch_info Optional free-form string with additional epoch metadata.
         */
        void logDesignMatrix(const Eigen::MatrixXd& H, const std::string& label, double tow, int iteration,
            const std::vector<std::string>& satellite_ids, const std::vector<std::string>& signal_types,
            const std::string& epoch_info = {});

        /**
         * @brief Log the innovations (residuals) vector.
         * @param z Innovations (observation residuals) vector.
         * @param label Short label identifying the logged data (e.g., "innovations").
         * @param tow GPS time-of-week (seconds) for the epoch being logged.
         * @param iteration Iteration index within the solver.
         * @param satellite_ids Vector of satellite identifiers corresponding to each innovation element.
         *                      Should have length equal to z.size().
         * @param signal_types Vector of signal type strings corresponding to each innovation element.
         *                     Should have length equal to z.size().
         * @param epoch_info Optional free-form string with additional epoch metadata.
         */
        void logInnovationsVector(const Eigen::VectorXd& z, const std::string& label, double tow, int iteration,
            const std::vector<std::string>& satellite_ids, const std::vector<std::string>& signal_types,
            const std::string& epoch_info = {});

        /**
         * @brief Log the weight matrix W used in the WLS formulation.
         * @param W Weight matrix (typically diagonal or full inverse of observation covariance).
         * @param label Short label identifying the logged data (e.g., "weights").
         * @param tow GPS time-of-week (seconds) for the epoch being logged.
         * @param iteration Iteration index within the solver.
         * @param epoch_info Optional free-form string with additional epoch metadata.
         */
        void logWeightMatrix(const Eigen::MatrixXd& W, const std::string& label, double tow, int iteration,
            const std::string& epoch_info = {});

        /**
         * @brief Log the normal matrix H^T * W * H produced during the normal equations assembly.
         * @param HtWH Normal matrix (square matrix matching parameter count).
         * @param label Short label identifying the logged data (e.g., "normal").
         * @param tow GPS time-of-week (seconds) for the epoch being logged.
         * @param iteration Iteration index within the solver.
         * @param condition_number Numerical condition number of the normal matrix (for diagnostics).
         * @param epoch_info Optional free-form string with additional epoch metadata.
         */
        void logNormalMatrix(const Eigen::MatrixXd& HtWH, const std::string& label, double tow, int iteration,
            double condition_number, const std::string& epoch_info = {});

    private:
        std::ofstream log_file_; /**< Output file stream used for logging. */

        /**
         * @brief Return a timestamp string (used as a prefix for each log entry).
         * @return ISO-8601-like timestamp string (implementation-defined).
         */
        std::string getTimestamp() const;
    };

} // namespace GnssProcessor