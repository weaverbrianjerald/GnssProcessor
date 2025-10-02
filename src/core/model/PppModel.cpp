#include "GnssConfig.hpp"
#include "PppModel.hpp"
#include "PppTypes.hpp"
#include "RinexParser.hpp"
#include "StateManager.hpp"
#include "ProductParser.hpp"
#include "Constants.hpp"
#include "MeasurementModel.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace GnssProcessor {

    PppModel::PppModel(const GnssConfig& config, ProductParser& product_parser, LogManager& log_manager,
        const RinexParser& rinex_parser, StateManager& state_manager)
        : config_(config), product_parser_(product_parser), log_manager_(log_manager),
        rinex_parser_(rinex_parser), state_manager_(state_manager), wls_logger_("wls_debug.log"),
        signal_priorities_(config.getSignalPriorities()), elevation_threshold_(config.getElevationMaskDeg()),
        multipath_histories_(), klobuchar_coeffs_(), sun_ephemeris_(), orbits_(), clocks_(), antennas_(), biases_(), erps_() {
        loadProducts();
        log_manager_.getLogger("ppp_model")->info("PppModel initialized with mode: {}, uncombined_enabled: {}, elevation mask: {} degrees, {} signal priority sets",
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK",
            config_.isUncombinedEnabled(), elevation_threshold_, signal_priorities_.size());
    }

    void PppModel::loadProducts() {
        auto sp3_paths = product_parser_.getOrbitPaths();
        if (!sp3_paths.empty()) {
            if (product_parser_.parseSp3(sp3_paths, orbits_)) {
                std::string paths_str = std::accumulate(sp3_paths.begin(), sp3_paths.end(), std::string(),
                    [](const std::string& a, const std::filesystem::path& p) {
                        return a.empty() ? p.string() : a + ", " + p.string();
                    });
                log_manager_.getLogger("ppp_model")->info("Loaded SP3 files: [{}]", paths_str);
            }
            else {
                log_manager_.getLogger("ppp_model")->error("Failed to load SP3 files: [{}]",
                    std::accumulate(sp3_paths.begin(), sp3_paths.end(), std::string(),
                        [](const std::string& a, const std::filesystem::path& p) {
                            return a.empty() ? p.string() : a + ", " + p.string();
                        }));
            }
        }
        else {
            log_manager_.getLogger("ppp_model")->warn("No SP3 file paths provided for orbit data");
        }

        std::filesystem::path clk_path = product_parser_.getProductPath("CLK");
        if (!clk_path.empty()) {
            if (product_parser_.parseClk(clk_path, clocks_)) {
                log_manager_.getLogger("ppp_model")->info("Loaded CLK file: {}", clk_path.string());
            }
            else {
                log_manager_.getLogger("ppp_model")->error("Failed to load CLK file: {}", clk_path.string());
            }
        }
        else {
            log_manager_.getLogger("ppp_model")->warn("No CLK file path provided");
        }

        std::filesystem::path atx_path = product_parser_.getProductPath("ATX");
        if (!atx_path.empty()) {
            if (product_parser_.parseAtx(atx_path, antennas_)) {
                log_manager_.getLogger("ppp_model")->info("Loaded ATX file: {}", atx_path.string());
            }
            else {
                log_manager_.getLogger("ppp_model")->error("Failed to load ATX file: {}", atx_path.string());
            }
        }
        else {
            log_manager_.getLogger("ppp_model")->warn("No ATX file path provided");
        }

        std::filesystem::path bia_path = product_parser_.getProductPath("BIA");
        if (!bia_path.empty()) {
            if (product_parser_.parseBia(bia_path, biases_)) {
                log_manager_.getLogger("ppp_model")->info("Loaded BIA file: {}", bia_path.string());
            }
            else {
                log_manager_.getLogger("ppp_model")->error("Failed to load BIA file: {}", bia_path.string());
            }
        }
        else {
            log_manager_.getLogger("ppp_model")->warn("No BIA file path provided");
        }

        std::filesystem::path erp_path = product_parser_.getProductPath("ERP");
        if (!erp_path.empty()) {
            if (product_parser_.parseErp(erp_path, erps_)) {
                log_manager_.getLogger("ppp_model")->info("Loaded ERP file: {}", erp_path.string());
            }
            else {
                log_manager_.getLogger("ppp_model")->error("Failed to load ERP file: {}", erp_path.string());
            }
        }
        else {
            log_manager_.getLogger("ppp_model")->warn("No ERP file path provided");
        }

        std::filesystem::path rnx_path = product_parser_.getProductPath("RNX");
        if (!rnx_path.empty()) {
            if (product_parser_.parseKlobuchar(rnx_path, klobuchar_coeffs_)) {
                log_manager_.getLogger("ppp_model")->info("Loaded Klobuchar coefficients from RNX file: {}, coeffs size: {}",
                    rnx_path.string(), klobuchar_coeffs_.size());
            }
            else {
                log_manager_.getLogger("ppp_model")->error("Failed to load Klobuchar coefficients from RNX file: {}", rnx_path.string());
            }
        }
        else {
            log_manager_.getLogger("ppp_model")->warn("No RNX file path provided for Klobuchar or Sun ephemeris");
        }

        // Compute Sun ephemeris using epoch times from RinexParser
        std::vector<GnssTime> epoch_times = rinex_parser_.getEpochTimes();
        if (parseSunEphemeris(epoch_times, sun_ephemeris_)) {
            log_manager_.getLogger("ppp_model")->info("Computed Sun ephemeris for {} observation epochs", sun_ephemeris_.size());
        }
        else {
            log_manager_.getLogger("ppp_model")->error("Failed to compute Sun ephemeris for observation epochs");
        }
    }

    void PppModel::initializeState(PppState& state) {
        size_t core_size = config_.getProcessingMode() == ProcessingMode::PPP ? 5 : 6;
        if (state.position.norm() == 0.0 || state.position.norm() > 1e9 || state.position.hasNaN() ||
            state.covariance.rows() != static_cast<long>(core_size)) {
            Eigen::Vector3d init_pos = rinex_parser_.getApproxPositionXyz();
            if (init_pos.norm() > 1e3 && !init_pos.hasNaN()) {
                state.position = init_pos;
                log_manager_.getLogger("ppp_model")->info("Initialized position from RINEX: ({:.3f}, {:.3f}, {:.3f}) m, norm: {:.3f}",
                    init_pos.x(), init_pos.y(), init_pos.z(), init_pos.norm());
            }
            else {
                state.position = Eigen::Vector3d::Zero();
                log_manager_.getLogger("ppp_model")->warn("Invalid or zero RINEX approximate position, using default: (0, 0, 0) m");
            }

            GnssTime init_time = rinex_parser_.getTimeOfFirstObs();
            if (init_time.tow > 0.0 && init_time.week > 0) {
                state.epoch_time = init_time;
                log_manager_.getLogger("ppp_model")->info("Initialized state epoch time from RINEX: week {}, TOW {:.3f}",
                    init_time.week, init_time.tow);
            }
            else {
                state.epoch_time = GnssTime{ 0, 0.0 };
                log_manager_.getLogger("ppp_model")->warn("Invalid RINEX time of first observation, using default: TOW 0.0");
            }

            state.clock_bias = 0.0;
            state.troposphere_zenith_delay = 2.0;
            if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
                if (init_pos.norm() > 1e3 && !init_pos.hasNaN() && init_time.tow > 0.0 && klobuchar_coeffs_.size() == 8) {
                    state.ionosphere_zenith_delay = computeInitialIonoDelay(init_time, init_pos);
                    log_manager_.getLogger("ppp_model")->info("Initialized ionospheric zenith delay from Klobuchar: {:.3f} m",
                        state.ionosphere_zenith_delay);
                }
                else {
                    state.ionosphere_zenith_delay = 5.0;
                    log_manager_.getLogger("ppp_model")->warn("Using default ionospheric zenith delay: 5.0 m (insufficient Klobuchar data)");
                }
            }

            state.covariance = Eigen::MatrixXd::Identity(core_size, core_size);
            const auto& initial_std = config_.getInitialStd();
            double pos_std = initial_std.position;
            double clock_std = initial_std.clock;
            double tropo_std = initial_std.troposphere;
            double iono_std = initial_std.ionosphere;
            if (pos_std <= 0 || clock_std <= 0 || tropo_std <= 0 ||
                (config_.getProcessingMode() == ProcessingMode::PPP_RTK && iono_std <= 0)) {
                log_manager_.getLogger("ppp_model")->warn("Invalid initial standard deviations, using defaults: pos=100, clock=1000, tropo=0.5, iono=5.0");
                pos_std = 100.0;
                clock_std = 1000.0;
                tropo_std = 0.5;
                iono_std = 5.0;
            }
            state.covariance.block(0, 0, 3, 3) *= pos_std * pos_std;
            state.covariance(3, 3) = clock_std * clock_std;
            state.covariance(4, 4) = tropo_std * tropo_std;
            if (config_.getProcessingMode() == ProcessingMode::PPP_RTK) {
                state.covariance(5, 5) = iono_std * iono_std;
            }
            log_manager_.getLogger("ppp_model")->debug("Initialized state covariance: pos_std={:.3f}, clock_std={:.3f}, tropo_std={:.3f}{}",
                pos_std, clock_std, tropo_std,
                config_.getProcessingMode() == ProcessingMode::PPP_RTK ? ", iono_std=" + std::to_string(iono_std) : "");
        }
        else {
            log_manager_.getLogger("ppp_model")->debug("State position already initialized: ({:.3f}, {:.3f}, {:.3f}) m, norm: {:.3f}",
                state.position.x(), state.position.y(), state.position.z(), state.position.norm());
        }
    }

    std::pair<Eigen::VectorXd, Eigen::MatrixXd> PppModel::estimateStateWls(const GnssTime& epoch_time, const std::vector<PppObservation>& observations) {
        const size_t core_size = 5; // Position (3), clock bias (1), tropospheric zenith delay (1)
        Eigen::VectorXd init_state(core_size);
        init_state.setZero();
        Eigen::MatrixXd init_covariance = Eigen::MatrixXd::Identity(core_size, core_size);
        double epoch_tow = epoch_time.tow;

        if (observations.empty() || observations.size() < core_size) {
            log_manager_.getLogger("ppp_model")->warn("No or insufficient observations for WLS estimation at TOW {:.3f}: {} available, {} required",
                epoch_tow, observations.size(), core_size);
            return { Eigen::VectorXd(), Eigen::MatrixXd() };
        }

        // Collect ionosphere-free code combinations
        std::vector<double> observation_vector;
        std::vector<double> modeled_vector;
        //std::vector<double> residuals;
        std::vector<double> variances;
        std::vector<double> mappings;
        std::vector<std::string> satellite_ids;
        std::vector<std::string> combo_types;
        std::vector<Eigen::Vector3d> sat_positions;
        std::vector<double> initial_ranges;

        for (const auto& obs : observations) {
            std::string system_str = MeasurementModel::getSystemString(obs.system);
            if (system_str != "GPS") {
                log_manager_.getLogger("ppp_model")->debug("Skipping non-GPS satellite {} at TOW {:.3f}", obs.satellite, obs.time.tow);
                continue;
            }
            if (obs.sat_pos.norm() == 0.0 || !obs.sat_pos.allFinite() || obs.range <= 0.0 ||
                !std::isfinite(obs.elevation) || !std::isfinite(obs.azimuth) || obs.los.norm() == 0.0 || !obs.los.allFinite()) {
                log_manager_.getLogger("ppp_model")->debug("Invalid geometry for satellite {} at TOW {:.3f}", obs.satellite, obs.time.tow);
                continue;
            }
            if (obs.elevation < elevation_threshold_) {
                log_manager_.getLogger("ppp_model")->debug("Satellite {} below elevation mask ({} < {} deg)", obs.satellite, obs.elevation, elevation_threshold_);
                continue;
            }
            for (const auto& [type, observed] : obs.observed) {
                //if (type[0] == 'C' && type.find("_") != std::string::npos) { // Iono-free combination (e.g., C1W_C2W)
                if (type.find("C1") != std::string::npos && type.find("C2") != std::string::npos) { // Iono-free L1/L2 code (e.g., C1W_C2W)
                    if (!obs.modeled.count(type) || !obs.variances.count(type)) {
                        log_manager_.getLogger("ppp_model")->debug("Missing modeled or variance data for {} {} at TOW {:.3f}",
                            obs.satellite, type, obs.time.tow);
                        continue;
                    }
                    //double residual = obs.residuals.count(type) ? obs.residuals.at(type) : observed - obs.modeled.at(type);
                    //residuals.push_back(residual);
                    if (!obs.observed.count(type)) continue;
                    observation_vector.push_back(obs.observed.at(type));
                    if (!obs.modeled.count(type)) continue;
                    modeled_vector.push_back(obs.modeled.at(type));
                    if (!obs.variances.count(type)) continue;
                    variances.push_back(obs.variances.at(type));
                    double mapping = 1.0 / (std::sin(obs.elevation * Constants::PI / 180.0) +
                        0.00143 / (std::tan(obs.elevation * Constants::PI / 180.0) + 0.0445));
                    mappings.push_back(mapping);
                    satellite_ids.push_back(obs.satellite);
                    combo_types.push_back(type);
                    sat_positions.push_back(obs.sat_pos);
                    initial_ranges.push_back(obs.range);
                }
            }
        }

        if (observation_vector.size() < core_size) {
            log_manager_.getLogger("ppp_model")->warn("Insufficient iono-free code combinations for WLS at TOW {:.3f}: {} available, {} required",
                epoch_tow, observation_vector.size(), core_size);
            return { Eigen::VectorXd(), Eigen::MatrixXd() };
        }

        // Iterated WLS setup
        Eigen::Vector3d current_pos = rinex_parser_.getApproxPositionXyz();
        double current_clock = 0.0;
        double current_trop_z = 2.3;
        const int max_iter = 5;
        const double tol = 0.1; // Convergence tolerance in meters
        double observed_minus_computed = 0.0;
        Eigen::MatrixXd HtWH;
        int iter;
        for (iter = 0; iter < max_iter; ++iter) {
            Eigen::MatrixXd H(observation_vector.size(), core_size);
            Eigen::VectorXd z(observation_vector.size());
            Eigen::MatrixXd W = Eigen::MatrixXd::Zero(observation_vector.size(), observation_vector.size());

            for (size_t i = 0; i < observation_vector.size(); ++i) {
                Eigen::Vector3d delta_r = sat_positions[i] - current_pos;
                double current_range = delta_r.norm();
                if (current_range == 0.0) continue;
                Eigen::Vector3d current_los = delta_r / current_range;
                double sin_elev = current_los.dot(current_pos.normalized());
                double elev_rad = std::asin(sin_elev);
                double elev_deg = elev_rad * 180.0 / Constants::PI;
                if (elev_deg < elevation_threshold_) continue; // Rare, but skip if below
                double current_model = modeled_vector[i];
                double observed = observation_vector[i];

                //double current_mapping = 1.0 / (sin_elev + 0.00143 / (std::tan(elev_rad) + 0.0445));

                // Form OC, where residuals contain observed-minus-computed without state information
                //double geom_delta = current_range - initial_ranges[i];
                double predicted = current_range + current_model + current_clock + mappings[i] * current_trop_z;
                //observed_minus_computed = residuals[i] - geom_delta - ( current_clock + mappings[i] * current_trop_z );

                //z(i) = residuals[i] - (geom_delta + current_clock + mappings[i] * current_trop_z );
                observed_minus_computed = observed - predicted;
                z(i) = observed_minus_computed;
                //z(i) = initial_ranges[i] - computed; // observed-minus-computed
                //z(i) = observed_minus_computed; // observed-minus-computed
                H(i, 0) = -current_los.x();
                H(i, 1) = -current_los.y();
                H(i, 2) = -current_los.z();
                H(i, 3) = 1.0; // meters
                H(i, 4) = mappings[i];
                W(i, i) = 1.0 / variances[i];
            }

            HtWH = H.transpose() * W * H;
            Eigen::JacobiSVD<Eigen::MatrixXd> svd(HtWH);
            double cond = svd.singularValues()(0) / svd.singularValues()(svd.singularValues().size() - 1);

            // Log all matrices and vectors for this iteration
            wls_logger_.logDesignMatrix(H, "WLS Design Matrix H", epoch_tow, iter + 1, satellite_ids, combo_types);
            wls_logger_.logInnovationsVector(z, "WLS Innovations Vector z", epoch_tow, iter + 1, satellite_ids, combo_types);
            wls_logger_.logWeightMatrix(W, "WLS Weight Matrix W", epoch_tow, iter + 1);
            wls_logger_.logNormalMatrix(HtWH, "WLS Normal Matrix HtWH", epoch_tow, iter + 1, cond);

            if (HtWH.determinant() <= 1e-6) {
                log_manager_.getLogger("ppp_model")->warn("WLS estimation failed at TOW {:.3f}, iter {}: singular HtWH matrix, determinant={:.3e}",
                    epoch_tow, iter, HtWH.determinant());
                break;
            }

            Eigen::VectorXd delta = HtWH.inverse() * H.transpose() * W * z;
            current_pos += delta.segment<3>(0);
            current_clock += delta(3);
            current_trop_z += delta(4);

            // Log state for this iteration
            Eigen::VectorXd current_state(core_size);
            current_state << current_pos, current_clock, current_trop_z;
            wls_logger_.logStateVector(current_state, "WLS Intermediate State Vector", epoch_tow, iter + 1);

            double delta_norm = delta.norm();

            if (delta_norm < tol) {
                break;
            }
        }

        init_state.segment<3>(0) = current_pos;
        init_state(3) = current_clock;
        init_state(4) = current_trop_z;
        init_covariance = HtWH.inverse();
        log_manager_.getLogger("ppp_model")->info("Iterated WLS estimation successful after {} iterations using {} iono-free code combinations at TOW {:.3f}: position=({:.3f}, {:.3f}, {:.3f}) m, "
            "clock={:.3e} m, tropo={:.3f} m",
            iter + 1, observation_vector.size(), epoch_tow, init_state(0), init_state(1), init_state(2),
            init_state(3), init_state(4));

        // Log final state and covariance
        wls_logger_.logStateVector(init_state, "WLS Final State Vector", epoch_tow, iter + 1);
        wls_logger_.logCovarianceMatrix(init_covariance, "WLS Final Covariance Matrix", epoch_tow, iter + 1);

        return { init_state, init_covariance };
    }

    bool PppModel::computeSatelliteGeometry(const RinexObservation& obs, const PppState& state,
        Eigen::Vector3d& sat_pos,         // [m] satellite ECEF position (at reception time, after rotation)
        Eigen::Vector3d& rx_pos,          // [m] receiver ECEF position (input state)
        double& elevation,                // [deg]
        double& azimuth,                  // [deg]
        Eigen::Vector3d& los,             // unit vector from receiver to satellite
        double& range,                    // [m] geometric distance
        double& tropo_delay,              // [m]
        double& sat_clock,                // [s] emission-time clock bias (incl. relativistic corr)
        double& shapiro_delay,            // [m]
        double& sagnac_corr,              // [m]
        double& atm_loading_corr,         // [m]
        double& relativistic_corr,        // [s]
        double& solid_tide) const         // [m]
    {
        const double c = Constants::SPEED_OF_LIGHT;
        const double omegaE = Constants::EARTH_ANGULAR_VELOCITY; // rad/s

        // --- Step 1: Validate receiver position ---
        rx_pos = state.position;
        if (rx_pos.norm() < 1e3 || !rx_pos.allFinite()) {
            log_manager_.getLogger("ppp_model")->warn("Invalid receiver position for {} at TOW {:.3f}",
                obs.satellite, obs.time.tow);
            return false;
        }

        // Initial satellite position (approx. reception time)
        Eigen::Vector3d sat_pos_init = getSatellitePosition(obs.satellite, obs.time);
        if (sat_pos_init.norm() < 1e3 || !sat_pos_init.allFinite()) {
            log_manager_.getLogger("ppp_model")->warn("Invalid initial satellite position for {} at TOW {:.3f}",
                obs.satellite, obs.time.tow);
            return false;
        }

        // Initial time-of-flight (TOF) guess
        double tof = (sat_pos_init - rx_pos).norm() / c; // [s]
        double tof_delta = 1.0;
        constexpr int max_iter = 5;

        Eigen::Vector3d sat_pos_em, sat_vel_em;
        double sat_clock_em = 0.0;

        // --- Step 2: Iterative TOF refinement ---
        for (int iter = 0; iter < max_iter && tof_delta > 1e-8; ++iter) {
            // Emission time = reception time - TOF
            GnssTime emission_time = obs.time;
            emission_time.tow -= tof;
            if (emission_time.tow < 0) {  // week rollover
                emission_time.tow += 604800.0;
                emission_time.week -= 1;
            }

            // Satellite position/velocity/clock at emission time
            sat_pos_em = getSatellitePosition(obs.satellite, emission_time);   // ECEF [m]
            sat_vel_em = getSatelliteVelocity(obs.satellite, emission_time);   // ECEF [m/s]
            sat_clock_em = getSatelliteClockCorrection(obs.satellite, emission_time); // [s]

            if (sat_pos_em.norm() < 1e3 || !sat_pos_em.allFinite() || !std::isfinite(sat_clock_em)) {
                log_manager_.getLogger("ppp_model")->warn("Invalid satellite data at emission time for {} at TOW {:.3f}",
                    obs.satellite, emission_time.tow);
                return false;
            }

            // --- Step 3: Relativistic correction (applied to satellite clock) ---
            relativistic_corr = computeRelativisticCorrection(obs.satellite, sat_pos_em, sat_vel_em); // [s]
            sat_clock = sat_clock_em + relativistic_corr; // [s]

            // --- Step 4: Earth rotation correction (emission → reception) ---
            double theta = omegaE * tof;
            Eigen::Matrix3d R;
            R << cos(theta), -sin(theta), 0,
                sin(theta), cos(theta), 0,
                0, 0, 1;
            sat_pos = R * sat_pos_em; // rotated to reception time

            // --- Step 5: Update TOF ---
            double new_range = (sat_pos - rx_pos).norm(); // [m]
            double new_tof = new_range / c;             // [s]
            tof_delta = std::abs(new_tof - tof);
            tof = new_tof;

            log_manager_.getLogger("ppp_model")->trace(
                "TOF iteration {} for {}: TOF={:.3e} s, range={:.3f} m, delta={:.3e} s",
                iter, obs.satellite, tof, new_range, tof_delta);
        }

        if (tof_delta > 1e-7) {
            log_manager_.getLogger("ppp_model")->warn("TOF iteration failed to converge for {}: delta={:.3e} s",
                obs.satellite, tof_delta);
            return false;
        }

        // --- Step 6: Final geometry ---
        range = (sat_pos - rx_pos).norm(); // [m]
        los = (sat_pos - rx_pos) / range;
        elevation = computeElevation(rx_pos, sat_pos); // [deg]
        azimuth = computeAzimuth(rx_pos, sat_pos);   // [deg]

        if (elevation < elevation_threshold_) {
            log_manager_.getLogger("ppp_model")->debug(
                "Satellite {} below elevation threshold: {:.3f} deg < {:.3f} deg",
                obs.satellite, elevation, elevation_threshold_);
            return false;
        }

        // --- Step 7: Propagation corrections ---
        // TODO: Replace tropospheric delay with model
        tropo_delay = getTroposphericDelay(elevation, state.troposphere_zenith_delay); // [m]
        shapiro_delay = computeShapiroDelay(obs.satellite, rx_pos, sat_pos);             // [m]
        sagnac_corr = computeSagnacCorrection(rx_pos, sat_pos);                        // [m]
        atm_loading_corr = computeAtmosphericLoadingCorrection(rx_pos, obs.time);           // [m]

        double mjd = TimeUtils::gpsTimeToJulianDate(obs.time.week, obs.time.tow);
        Eigen::Vector3d sun_pos = computeSunPosition(mjd);
        Eigen::Vector3d moon_pos = computeMoonPosition(mjd);
        solid_tide = computeSolidEarthTide(rx_pos, sat_pos, sun_pos, moon_pos);

        // --- Diagnostics ---
        log_manager_.getLogger("ppp_model")->trace(
            "Satellite geometry for {} at TOW {:.3f}: range={:.3f} m, elev={:.3f} deg, az={:.3f} deg, "
            "tropo={:.3f} m, sat_clock={:.3e} s ({:.3f} m), shapiro={:.3e} m, sagnac={:.3f} m, "
            "atm_load={:.3f} m, relativistic={:.3e} s ({:.3f} m)", "solid_tide={:.3f} m",
            obs.satellite, obs.time.tow, range, elevation, azimuth,
            tropo_delay, sat_clock, sat_clock * c,
            shapiro_delay, sagnac_corr, atm_loading_corr,
            relativistic_corr, relativistic_corr * c, solid_tide);

        return true;
    }

    double PppModel::computeSolidEarthTide(const Eigen::Vector3d& receiver_pos, const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sun_pos, const Eigen::Vector3d& moon_pos) const {
        const double h0 = 0.6078;
        const double h2 = -0.0006;
        const double h3 = 0.292;
        const double l0 = 0.0847;
        const double l2 = 0.0002;
        const double l3 = 0.015;
        const double MS2E = 332946.0;
        const double MM2E = 0.01230002;
        const double re = 6378137.0;

        Eigen::Vector3d l = receiver_pos - sat_pos;
        double l_norm = l.norm();
        if (l_norm == 0.0) return 0.0;
        Eigen::Vector3d los = l / l_norm;

        double lat = std::atan2(receiver_pos.z(), std::sqrt(receiver_pos.x() * receiver_pos.x() + receiver_pos.y() * receiver_pos.y()));
        double trm = 3.0 * std::sin(lat) * std::sin(lat) - 1.0;
        double h = h0 + h2 * trm;
        double l_coeff = l0 + l2 * trm;

        double sun_dist = sun_pos.norm();
        double moon_dist = moon_pos.norm();
        double rec_dist = receiver_pos.norm();
        if (sun_dist == 0.0 || moon_dist == 0.0 || rec_dist == 0.0) return 0.0;

        Eigen::Vector3d sun_uni = sun_pos / sun_dist;
        Eigen::Vector3d moon_uni = moon_pos / moon_dist;
        Eigen::Vector3d rec_uni = receiver_pos / rec_dist;

        double dotSR = sun_uni.dot(rec_uni);
        double dotMR = moon_uni.dot(rec_uni);

        Eigen::Vector3d aSun = sun_uni - dotSR * rec_uni;
        Eigen::Vector3d aMoon = moon_uni - dotMR * rec_uni;

        double DRS = std::pow(re, 4) / std::pow(sun_dist, 3);
        double DRM = std::pow(re, 4) / std::pow(moon_dist, 3);
        double DRM2 = std::pow(re, 5) / std::pow(moon_dist, 4);

        double s1 = (3.0 * dotSR * dotSR - 1.0) / 2.0;
        Eigen::Vector3d d2s = MS2E * DRS * ((h * rec_uni) * s1 + (3.0 * l_coeff * dotSR) * aSun);

        double s2 = (3.0 * dotMR * dotMR - 1.0) / 2.0;
        Eigen::Vector3d d2m = MM2E * DRM * ((h * rec_uni) * s2 + (3.0 * l_coeff * dotMR) * aMoon);

        double s3 = (5.0 / 2.0) * std::pow(dotMR, 3) - (3.0 / 2.0) * dotMR;
        double s4 = (15.0 / 2.0) * dotMR * dotMR - (3.0 / 2.0);
        Eigen::Vector3d d3m = MM2E * DRM2 * ((h3 * rec_uni) * s3 + (l3 * s4) * aMoon);

        double stide = (d2s + d2m + d3m).dot(los);

        /*log_manager_.getLogger("ppp_model")->trace(
            "Solid Earth tide for sat {}: stide={:.3f} m, lat={:.3f} rad, dotSR={:.3f}, dotMR={:.3f}",
            obs.satellite, stide, lat, dotSR, dotMR);*/

        return stide;
    }

    Eigen::Vector3d PppModel::computeSunPosition(double mjd) const {
        const double AU = 149597870700.0;  // Astronomical unit (m)
        const double d2r = Constants::PI / 180.0;

        double fday = mjd - std::floor(mjd);
        double JDN = mjd - 15019.5;
        double v1 = std::fmod(279.696678 + 0.9856473354 * JDN, 360.0);
        double gstr = std::fmod(279.690983 + 0.9856473354 * JDN + 360.0 * fday + 180.0, 360.0);
        double g = std::fmod(358.475845 + 0.9856002670 * JDN, 360.0) * d2r;
        double slong = v1 + (1.91946 - 0.004789 * JDN / 36525.0) * std::sin(g) + 0.020094 * std::sin(2.0 * g);
        double obliq = (23.45229 - 0.0130125 * JDN / 36525.0) * d2r;
        double slp = (slong - 0.005686) * d2r;
        double snd = std::sin(obliq) * std::sin(slp);
        double csd = std::sqrt(1.0 - snd * snd);
        double sdec = std::atan2(snd, csd) * 180.0 / Constants::PI;
        double sra = 180.0 - std::atan2(snd / csd / std::tan(obliq), -std::cos(slp) / csd) * 180.0 / Constants::PI;

        Eigen::Vector3d s_pos;
        s_pos << std::cos(sdec * d2r) * std::cos(sra * d2r) * AU,
            std::cos(sdec * d2r)* std::sin(sra * d2r)* AU,
            std::sin(sdec * d2r)* AU;

        s_pos = rotateVector(s_pos, gstr * d2r, 3);

        log_manager_.getLogger("ppp_model")->trace(
            "Sun position at MJD {:.3f}: ({:.3f}, {:.3f}, {:.3f}) m",
            mjd, s_pos.x(), s_pos.y(), s_pos.z());

        return s_pos;
    }

    Eigen::Vector3d PppModel::computeMoonPosition(double mjd) const {
        const double T = (mjd - 51544.5) / 36525.0;
        const double d2r = Constants::PI / 180.0;

        double L0 = 218.31617 + 481267.88088 * T - 1.3972 * T;
        double l = 134.96292 + 477198.86753 * T;
        double lp = 357.52543 + 35999.04944 * T;
        double F = 93.27283 + 483202.01873 * T;
        double D = 297.85027 + 445267.11135 * T;

        double long_terms = (22640.0 * std::sin(l * d2r) + 769.0 * std::sin(2.0 * l * d2r) -
            4586.0 * std::sin((l - 2.0 * D) * d2r) + 2370.0 * std::sin(2.0 * D * d2r) -
            668.0 * std::sin(lp * d2r) - 412.0 * std::sin(2.0 * F * d2r) -
            212.0 * std::sin((2.0 * l - 2.0 * D) * d2r) - 206.0 * std::sin((l + lp - 2.0 * D) * d2r) +
            192.0 * std::sin((l + 2.0 * D) * d2r) - 165.0 * std::sin((lp - 2.0 * D) * d2r) +
            148.0 * std::sin((l - lp) * d2r) - 125.0 * std::sin(D * d2r) -
            110.0 * std::sin((l + lp) * d2r) - 55.0 * std::sin((2.0 * F - 2.0 * D) * d2r)) / 3600.0;
        double long_ = std::fmod(L0 + long_terms, 360.0);

        double lat = (18520.0 * std::sin((F + long_ - L0 + (412.0 * std::sin(2.0 * F * d2r) + 541.0 * std::sin(lp * d2r)) / 3600.0) * d2r) -
            526.0 * std::sin((F - 2.0 * D) * d2r) + 44.0 * std::sin((l + F - 2.0 * D) * d2r) -
            31.0 * std::sin((-l + F - 2.0 * D) * d2r) - 25.0 * std::sin((-2.0 * l + F) * d2r) -
            23.0 * std::sin((lp + F - 2.0 * D) * d2r) + 21.0 * std::sin((-l + F) * d2r) +
            11.0 * std::sin((-lp + F - 2.0 * D) * d2r)) / 3600.0;
        lat = std::fmod(lat, 360.0);

        double dist = (385000.0 - 20905.0 * std::cos(l * d2r) - 3699.0 * std::cos((2.0 * D - l) * d2r) -
            2956.0 * std::cos(2.0 * D * d2r) - 570.0 * std::cos(2.0 * l * d2r) +
            246.0 * std::cos((2.0 * l - 2.0 * D) * d2r) - 205.0 * std::cos((lp - 2.0 * D) * d2r) -
            171.0 * std::cos((l + 2.0 * D) * d2r) - 152.0 * std::cos((l + lp - 2.0 * D) * d2r)) * 1000.0;

        Eigen::Vector3d m_pos;
        m_pos << dist * std::cos(long_ * d2r) * std::cos(lat * d2r),
            dist* std::sin(long_ * d2r)* std::cos(lat * d2r),
            dist* std::sin(lat * d2r);

        const double obl = 23.43929111 * d2r;
        m_pos = rotateVector(m_pos, -obl, 1);

        double fday = mjd - std::floor(mjd);
        double JDN = mjd - 15019.5;
        double gstr = std::fmod(279.690983 + 0.9856473354 * JDN + 360.0 * fday + 180.0, 360.0) * d2r;
        m_pos = rotateVector(m_pos, gstr, 3);

        log_manager_.getLogger("ppp_model")->trace(
            "Moon position at MJD {:.3f}: ({:.3f}, {:.3f}, {:.3f}) m",
            mjd, m_pos.x(), m_pos.y(), m_pos.z());

        return m_pos;
    }

    Eigen::Vector3d PppModel::rotateVector(const Eigen::Vector3d& vec, double angle, int axis) const {
        Eigen::Matrix3d R;
        double c = std::cos(angle);
        double s = std::sin(angle);
        if (axis == 1) {  // X-axis
            R << 1, 0, 0,
                0, c, -s,
                0, s, c;
        }
        else if (axis == 3) {  // Z-axis
            R << c, -s, 0,
                s, c, 0,
                0, 0, 1;
        }
        else {
            log_manager_.getLogger("ppp_model")->warn("Invalid rotation axis: {}", axis);
            return vec;
        }
        return R * vec;
    }

    double PppModel::computeShapiroDelay(const std::string& satellite, const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos) const {
        constexpr double GM = Constants::GRAVITATIONAL_CONSTANT; // GM = G * M_Earth (m³/s²)
        constexpr double C_CUBED = Constants::SPEED_OF_LIGHT * Constants::SPEED_OF_LIGHT * Constants::SPEED_OF_LIGHT;
        constexpr double MIN_POSITION_NORM = 1.8e7; // ~20,000 km
        constexpr double MAX_POSITION_NORM = 3.2e7; // ~32,000 km
        constexpr double MAX_DELAY = 1e-4; // Max 0.1 ms

        // Validate inputs
        std::string system_str = satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown";
        if (!rx_pos.allFinite() || !sat_pos.allFinite()) {
            log_manager_.getLogger("ppp_model")->warn("Invalid Shapiro delay inputs for {} ({}): rx_pos=({:.3f}, {:.3f}, {:.3f}), sat_pos=({:.3f}, {:.3f}, {:.3f})",
                satellite, system_str, rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }
        double r = (sat_pos - rx_pos).norm();
        double sat_norm = sat_pos.norm();
        if (r < 1e3 || sat_norm < MIN_POSITION_NORM || sat_norm > MAX_POSITION_NORM) {
            log_manager_.getLogger("ppp_model")->warn("Unrealistic Shapiro delay norms for {} ({}): range={:.3e} m, sat_norm={:.3e} m",
                satellite, system_str, r, sat_norm);
            return 0.0;
        }

        // Compute Shapiro delay: (2GM/c³) * ln((r + r_s + r_r)/(r + r_s - r_r))
        double r_rx = rx_pos.norm();
        double delay = (2.0 * GM / C_CUBED) * std::log((r + sat_norm + r_rx) / (r + sat_norm - r_rx));
        if (!std::isfinite(delay) || std::abs(delay) > MAX_DELAY) {
            log_manager_.getLogger("ppp_model")->warn("Invalid Shapiro delay for {} ({}): {:.3e} s, range={:.3e} m, sat_norm={:.3e} m, rx_norm={:.3e} m",
                satellite, system_str, delay, r, sat_norm, r_rx);
            return 0.0;
        }

        // Convert to meters
        delay *= Constants::SPEED_OF_LIGHT;
        log_manager_.getLogger("ppp_model")->trace("Shapiro delay for {} ({}): {:.3e} m, range={:.3e} m, sat_norm={:.3e} m, rx_norm={:.3e} m",
            satellite, system_str, delay, r, sat_norm, r_rx);
        return delay;
    }

    bool PppModel::computeModels(const std::vector<RinexObservation>& observations, PppState& state, std::vector<PppObservation>& modeled_obs) {
        if (observations.empty()) {
            log_manager_.getLogger("ppp_model")->error("No observations provided for PPP modeling");
            return false;
        }

        modeled_obs.clear();
        modeled_obs.reserve(observations.size());

        // Initialize state if needed
        initializeState(state);

        for (const auto& obs : observations) {
            PppObservation ppp_obs;
            ppp_obs.satellite = obs.satellite;
            ppp_obs.system = obs.system;
            ppp_obs.time = obs.time;
            ppp_obs.observed = obs.measurements;
            ppp_obs.variances = obs.variances;
            ppp_obs.lli_flags = obs.lli_flags;

            ppp_obs.sat_pos = Eigen::Vector3d::Zero();
            ppp_obs.elevation = 0.0;
            ppp_obs.azimuth = 0.0;
            ppp_obs.los = Eigen::Vector3d::Zero();
            ppp_obs.range = 0.0;

            // Compute satellite geometry and corrections
            Eigen::Vector3d sat_pos, rx_pos, los;
            double elevation, azimuth, range, tropo_delay, sat_clock, shapiro_delay, sagnac_corr, atm_loading_corr, relativistic_corr, solid_tide;
            if (!computeSatelliteGeometry(obs, state, sat_pos, rx_pos, elevation, azimuth, los, range, tropo_delay,
                sat_clock, shapiro_delay, sagnac_corr, atm_loading_corr, relativistic_corr, solid_tide)) {
                continue;
            }

            // Populate geometric fields in PppObservation
            ppp_obs.sat_pos = sat_pos;
            ppp_obs.elevation = elevation;
            ppp_obs.azimuth = azimuth;
            ppp_obs.los = los;
            ppp_obs.range = range;

            // Log geometric data storage
            log_manager_.getLogger("ppp_model")->trace("Stored geometry for {} at TOW {:.3f}: sat_pos=({:.3f}, {:.3f}, {:.3f}) m, "
                "elevation={:.3f} deg, azimuth={:.3f} deg, los=({:.3f}, {:.3f}, {:.3f}), range={:.3f} m",
                obs.satellite, obs.time.tow,
                sat_pos.x(), sat_pos.y(), sat_pos.z(),
                elevation, azimuth,
                los.x(), los.y(), los.z(), range);

            // Get system string
            std::string system_str = MeasurementModel::getSystemString(obs.system);
            if (system_str == "Unknown") {
                log_manager_.getLogger("ppp_model")->warn("Unknown system for satellite {} at TOW {:.3f}", obs.satellite, obs.time.tow);
                continue;
            }

            // Select observation types
            auto obs_types = selectObservationTypes(obs.measurements, obs.system, system_str);
            if (obs_types.empty()) {
                log_manager_.getLogger("ppp_model")->warn("No valid observation types for system {} for satellite {}", system_str, obs.satellite);
                continue;
            }

            // Get receiver antenna type
            std::string rx_antenna_type = rinex_parser_.getReceiverAntennaType();

            // Process single-frequency measurements
            if (config_.isUncombinedEnabled()) {
                for (const auto& type : obs_types) {
                    if (!processSingleFreqObservation(obs, state, ppp_obs, system_str, type, rx_pos, sat_pos, elevation,
                        azimuth, rx_antenna_type, range, tropo_delay, sat_clock, shapiro_delay,
                        sagnac_corr, atm_loading_corr, relativistic_corr, solid_tide)) {
                        continue;
                    }
                }
            }

            // Determine available ionosphere-free combinations
            std::vector<std::pair<std::string, std::string>> selected_signals;
            bool is_iono_free;
            std::string comb_type;
            if (!selectSignals(obs, system_str, selected_signals, is_iono_free, comb_type)) {
                continue;
            }
            if (is_iono_free && !config_.isUncombinedEnabled()) {
                if (!processIonoFreeObservation(obs, state, ppp_obs, system_str, selected_signals, comb_type, rx_pos, sat_pos,
                    elevation, azimuth, rx_antenna_type, range, tropo_delay, sat_clock,
                    shapiro_delay, sagnac_corr, atm_loading_corr, relativistic_corr, solid_tide)) {
                    continue;
                }
            }

            if (!ppp_obs.modeled.empty()) {
                modeled_obs.push_back(ppp_obs);
            }
        }

        if (modeled_obs.empty()) {
            log_manager_.getLogger("ppp_model")->error("No valid PPP observations modeled");
            return false;
        }
        log_manager_.getLogger("ppp_model")->info("Modeled {} PPP observations for mode {}", modeled_obs.size(),
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
        return true;
    }

    bool PppModel::selectSignals(const RinexObservation& obs, const std::string& system_str,
        std::vector<std::pair<std::string, std::string>>& selected_signals,
        bool& is_iono_free, std::string& comb_type) {
        // Get signal priorities from config
        auto it = signal_priorities_.find(system_str);
        const auto& signals = it != signal_priorities_.end() ? it->second : std::vector<std::string>{};
        if (signals.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No signal priorities for system {}, skipping satellite {}", system_str, obs.satellite);
            return false;
        }

        // Define frequency band mappings per system
        static const std::map<std::string, std::vector<std::pair<std::string, std::string>>> system_to_bands = {
            {"GPS", {{"C1", "L1"}, {"L1", "L1"}, {"C2", "L2"}, {"L2", "L2"}, {"C5", "L5"}, {"L5", "L5"}}},
            {"Galileo", {{"C1", "E1"}, {"L1", "E1"}, {"C5", "E5a"}, {"L5", "E5a"}, {"C7", "E5b"}, {"L7", "E5b"}, {"C6", "E6"}, {"L6", "E6"}, {"C8", "E5"}, {"L8", "E5"}}},
            {"GLONASS", {{"C1", "G1"}, {"L1", "G1"}, {"C2", "G2"}, {"L2", "G2"}, {"C3", "G3"}, {"L3", "G3"}}},
            {"BeiDou", {{"C1", "B1C"}, {"L1", "B1C"}, {"C2", "B1I"}, {"L2", "B1I"}, {"C7", "B2"}, {"L7", "B2"}, {"C6", "B3"}, {"L6", "B3"}}},
            {"QZSS", {{"C1", "L1"}, {"L1", "L1"}, {"C2", "L2"}, {"L2", "L2"}, {"C5", "L5"}, {"L5", "L5"}, {"C6", "L6"}, {"L6", "L6"}}},
            {"SBAS", {{"C1", "L1"}, {"L1", "L1"}, {"C5", "L5"}, {"L5", "L5"}}}
        };

        // Map available signals to frequency bands, respecting priority order
        std::map<std::string, std::pair<std::string, std::string>> freq_band_to_signal; // band -> {code, phase}
        std::vector<std::string> freq_bands;
        auto bands_it = system_to_bands.find(system_str);
        if (bands_it == system_to_bands.end()) {
            log_manager_.getLogger("ppp_model")->warn("No frequency band mappings for system {}, skipping satellite {}", system_str, obs.satellite);
            return false;
        }

        // Signal band definitions for each constellation + mode
        static const std::map<std::string, std::map<SignalMode, std::vector<std::string>>> band_map = {
            {"GPS", {
                {SignalMode::Dual, {"L1", "L2"}},
                {SignalMode::Triple, {"L1", "L2", "L5"}},
                {SignalMode::All, {"L1", "L2", "L5"}}
            }},
            {"Galileo", {
                {SignalMode::Dual, {"E1", "E5a"}},
                {SignalMode::Triple, {"E1", "E5a", "E5b"}},
                {SignalMode::All, {"E1", "E5a", "E5b", "E6", "E5"}}
            }},
            {"GLONASS", {
                {SignalMode::Dual, {"G1", "G2"}},
                {SignalMode::Triple, {"G1", "G2", "G3"}},
                {SignalMode::All, {"G1", "G2", "G3"}}
            }},
            {"BeiDou", {
                {SignalMode::Dual, {"B1I", "B3"}},
                {SignalMode::Triple, {"B1I", "B3", "B2"}},
                {SignalMode::All, {"B1C", "B1I", "B2", "B3"}}
            }},
            {"QZSS", {
                {SignalMode::Dual, {"L1", "L2"}},
                {SignalMode::Triple, {"L1", "L2", "L5"}},
                {SignalMode::All, {"L1", "L2", "L5", "L6"}}
            }},
            {"SBAS", {
                {SignalMode::Dual, {"L1", "L5"}},
                {SignalMode::Triple, {"L1", "L5"}},
                {SignalMode::All, {"L1", "L5"}}
            }}
        };

        // Map signals to bands, allowing multiple signals per band
        for (const auto& sig : signals) {
            if (!obs.measurements.count(sig)) continue;
            for (const auto& [pattern, band] : bands_it->second) {
                if (std::find(band_map.at(system_str).at(config_.getSignalMode()).begin(),
                    band_map.at(system_str).at(config_.getSignalMode()).end(), band) ==
                    band_map.at(system_str).at(config_.getSignalMode()).end()) {
                    continue; // Skip non-target bands
                }
                if (sig.find(pattern) == 0) {
                    if (freq_band_to_signal.find(band) == freq_band_to_signal.end()) {
                        freq_band_to_signal[band] = { "", "" };
                        freq_bands.push_back(band);
                    }
                    // Prefer higher-priority signals, but don't overwrite existing phase with code or vice versa
                    if (sig[0] == 'C' && (freq_band_to_signal[band].first.empty() ||
                        std::find(signals.begin(), signals.end(), sig) <
                        std::find(signals.begin(), signals.end(), freq_band_to_signal[band].first))) {
                        freq_band_to_signal[band].first = sig;
                    }
                    if (sig[0] == 'L' && (freq_band_to_signal[band].second.empty() ||
                        std::find(signals.begin(), signals.end(), sig) <
                        std::find(signals.begin(), signals.end(), freq_band_to_signal[band].second))) {
                        freq_band_to_signal[band].second = sig;
                    }
                }
            }
        }

        // Log available bands and signals
        std::string available_signals;
        for (const auto& [band, sig_pair] : freq_band_to_signal) {
            available_signals += band + ":(" + (sig_pair.first.empty() ? "none" : sig_pair.first) + "," +
                (sig_pair.second.empty() ? "none" : sig_pair.second) + ")";
            if (band != freq_bands.back()) available_signals += ", ";
        }
        log_manager_.getLogger("ppp_model")->debug("Available signals for {} {}: [{}], signal_mode={}",
            obs.satellite, system_str, available_signals.empty() ? "none" : available_signals,
            config_.getSignalMode() == SignalMode::Dual ? "Dual" :
            config_.getSignalMode() == SignalMode::Triple ? "Triple" : "All");

        // Determine combination type based on available bands and signal mode
        selected_signals.clear();
        is_iono_free = freq_bands.size() >= 2 && !config_.isUncombinedEnabled();
        comb_type = freq_bands.size() >= 3 ? "triple" : freq_bands.size() == 2 ? "dual" : "single";

        if (config_.isUncombinedEnabled()) {
            // Uncombined mode: select all available signals
            for (const auto& sig : signals) {
                if (obs.measurements.count(sig)) {
                    selected_signals.emplace_back(sig, "");
                }
            }
            if (selected_signals.empty()) {
                log_manager_.getLogger("ppp_model")->warn(
                    "No valid signals for {} {} at TOW {:.3f}",
                    obs.satellite, system_str, obs.time.tow
                );
                return false;
            }
            comb_type = "uncombined";
        }
        else if (is_iono_free && band_map.count(system_str)) {
            auto target_bands = band_map.at(system_str).at(config_.getSignalMode());
            std::vector<std::pair<std::string, std::string>> candidate_pairs;
            for (size_t i = 0; i < target_bands.size(); ++i) {
                for (size_t j = i + 1; j < target_bands.size(); ++j) {
                    const auto& b1 = target_bands[i];
                    const auto& b2 = target_bands[j];
                    if (freq_band_to_signal.count(b1) && freq_band_to_signal.count(b2)) {
                        const auto& s1 = freq_band_to_signal.at(b1);
                        const auto& s2 = freq_band_to_signal.at(b2);
                        // Code pair
                        if (!s1.first.empty() && !s2.first.empty()) {
                            candidate_pairs.emplace_back(s1.first, s2.first);
                            log_manager_.getLogger("ppp_model")->trace(
                                "Selected iono-free code pair for {} {}: {} / {}, bands: {} / {}, signal_mode={}",
                                obs.satellite, system_str, s1.first, s2.first, b1, b2,
                                config_.getSignalMode() == SignalMode::Dual ? "Dual" :
                                config_.getSignalMode() == SignalMode::Triple ? "Triple" : "All"
                            );
                        }
                        // Phase pair
                        if (!s1.second.empty() && !s2.second.empty()) {
                            candidate_pairs.emplace_back(s1.second, s2.second);
                            log_manager_.getLogger("ppp_model")->trace(
                                "Selected iono-free phase pair for {} {}: {} / {}, bands: {} / {}, signal_mode={}",
                                obs.satellite, system_str, s1.second, s2.second, b1, b2,
                                config_.getSignalMode() == SignalMode::Dual ? "Dual" :
                                config_.getSignalMode() == SignalMode::Triple ? "Triple" : "All"
                            );
                        }
                        else if (s1.second.empty() || s2.second.empty()) {
                            log_manager_.getLogger("ppp_model")->debug(
                                "Missing phase signal for {} {}: band {} phase={}, band {} phase={}, signal_mode={}",
                                obs.satellite, system_str, b1, s1.second, b2, s2.second,
                                config_.getSignalMode() == SignalMode::Dual ? "Dual" :
                                config_.getSignalMode() == SignalMode::Triple ? "Triple" : "All"
                            );
                        }
                    }
                    else {
                        log_manager_.getLogger("ppp_model")->debug(
                            "Missing band signals for {} {}: band {} present={}, band {} present={}, signal_mode={}",
                            obs.satellite, system_str, b1, freq_band_to_signal.count(b1), b2, freq_band_to_signal.count(b2),
                            config_.getSignalMode() == SignalMode::Dual ? "Dual" :
                            config_.getSignalMode() == SignalMode::Triple ? "Triple" : "All"
                        );
                    }
                }
            }
            selected_signals = std::move(candidate_pairs);
            if (selected_signals.empty()) {
                comb_type = "single";
                is_iono_free = false;
            }
        }

        // Fallback to single-frequency if no iono-free combination is possible
        if (!is_iono_free && selected_signals.empty()) {
            for (const auto& sig : signals) {
                if (obs.measurements.count(sig) && sig[0] == 'C') {
                    selected_signals.emplace_back(sig, "");
                    break;
                }
            }
            for (const auto& sig : signals) {
                if (obs.measurements.count(sig) && sig[0] == 'L') {
                    selected_signals.emplace_back("", sig);
                    break;
                }
            }
            comb_type = "single";
        }

        if (selected_signals.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No valid signals for {} {} at TOW {:.3f}", obs.satellite, system_str, obs.time.tow);
            return false;
        }

        // Log selected signals
        std::string signal_list;
        for (size_t i = 0; i < selected_signals.size(); ++i) {
            signal_list += (selected_signals[i].first.empty() ? "" : selected_signals[i].first) +
                (selected_signals[i].second.empty() ? "" : (selected_signals[i].first.empty() ? "" : "/") + selected_signals[i].second);
            if (i < selected_signals.size() - 1) signal_list += ",";
        }
        log_manager_.getLogger("ppp_model")->trace("Selected signals for {} {}: [{}], comb_type={}, is_iono_free={}, mode={}",
            obs.satellite, system_str, signal_list.empty() ? "none" : signal_list, comb_type, is_iono_free,
            config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");

        return true;
    }

    bool PppModel::processIonoFreeObservation(const RinexObservation& obs, PppState& state,
        PppObservation& ppp_obs, const std::string& system_str,
        const std::vector<std::pair<std::string, std::string>> selected_signals,
        const std::string& comb_type, const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos, double elevation, double azimuth,
        const std::string& rx_antenna_type, double range, double tropo_delay,
        double sat_clock, double shapiro_delay, double sagnac_corr,
        double atm_loading_corr, double relativistic_corr, double solid_tide) {

        size_t num_freq = (comb_type == "triple") ? 3 : (comb_type == "dual") ? 2 : 0;
        if (num_freq == 0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid combination type {} for {}", comb_type, obs.satellite);
            return false;
        }
        // Validate number of signal pairs
        if (selected_signals.size() > num_freq || selected_signals.size() < 2) {
            log_manager_.getLogger("ppp_model")->warn("Invalid number of signal pairs for {} combination for {} (expected up to {}, got {})",
                comb_type, obs.satellite, num_freq, selected_signals.size());
            return false;
        }

        // Assign and validate code and phase signals
        std::vector<std::string> code_signals(1), phase_signals(1);
        bool code_pair_found = false, phase_pair_found = false;
        for (size_t f = 0; f < selected_signals.size(); ++f) {
            const auto& pair = selected_signals[f];
            if (!pair.first.empty() && !pair.second.empty()) {
                // Check if pair is code/code or phase/phase based on first signal's type
                if (pair.first[0] == 'C' && pair.second[0] == 'C') {
                    if (code_pair_found) {
                        log_manager_.getLogger("ppp_model")->warn("Multiple code pairs for {} {}, expected one: {} / {}",
                            obs.satellite, system_str, pair.first, pair.second);
                        return false;
                    }
                    code_signals[0] = pair.first;
                    code_signals.push_back(pair.second);
                    code_pair_found = true;
                }
                else if (pair.first[0] == 'L' && pair.second[0] == 'L') {
                    if (phase_pair_found) {
                        log_manager_.getLogger("ppp_model")->warn("Multiple phase pairs for {} {}, expected one: {} / {}",
                            obs.satellite, system_str, pair.first, pair.second);
                        return false;
                    }
                    phase_signals[0] = pair.first;
                    phase_signals.push_back(pair.second);
                    phase_pair_found = true;
                }
                else {
                    log_manager_.getLogger("ppp_model")->warn("Invalid signal type pair for {} {} in pair {}: {} / {}",
                        obs.satellite, system_str, f + 1, pair.first, pair.second);
                    return false;
                }

                // Validate measurements exist in obs
                if (!obs.measurements.count(pair.first)) {
                    log_manager_.getLogger("ppp_model")->warn("Missing measurement for signal {} for {} {} at TOW {:.3f}",
                        pair.first, obs.satellite, system_str, obs.time.tow);
                    return false;
                }
                if (!obs.measurements.count(pair.second)) {
                    log_manager_.getLogger("ppp_model")->warn("Missing measurement for signal {} for {} {} at TOW {:.3f}",
                        pair.second, obs.satellite, system_str, obs.time.tow);
                    return false;
                }
            }
            else {
                log_manager_.getLogger("ppp_model")->warn("Empty signal in pair {} for {} {}: first={}, second={}",
                    f + 1, obs.satellite, system_str, pair.first, pair.second);
                return false;
            }
        }

        // Ensure both code and phase pairs were found
        if (!code_pair_found || !phase_pair_found) {
            log_manager_.getLogger("ppp_model")->warn("Missing required pair for {} {}: code_pair={}, phase_pair={}",
                obs.satellite, system_str, code_pair_found, phase_pair_found);
            return false;
        }

        bool success = false;
        std::vector<std::pair<std::string, std::vector<std::string>>> meas_groups = {
            {"CODE", code_signals},
            {"PHASE", phase_signals}
        };

        // Define pairs for combinations
        std::vector<std::vector<size_t>> pairs;
        if (comb_type == "dual") {
            pairs = { {0, 1} };
        }
        else { // triple
            pairs = { {0, 1}, {0, 2} }; // L1/L2 and L1/L5
        }

        for (const auto& group : meas_groups) {
            const std::string& meas_str = group.first;
            const auto& sel_sigs = group.second;
            char meas_type = (meas_str == "CODE" ? 'C' : 'L');

            for (const auto& pair : pairs) {
                if (pair.size() != 2) continue;
                size_t i1 = pair[0], i2 = pair[1];
                if (i1 >= num_freq || i2 >= num_freq) continue;

                std::string ref_type = sel_sigs[i1] + "_" + sel_sigs[i2];

                std::vector<double> modeled_obs(2, 0.0);
                std::vector<double> variances(2, 0.0);
                std::vector<double> frequencies(2, 0.0);
                std::vector<double> wavelengths(2, 0.0);
                if (meas_type == 'L') {
                    //continue; //!< TODO: continue here removes carrier phase measurements for debugging
                    wavelengths[0] = getWavelength(obs.system, sel_sigs[i1]);
                    wavelengths[1] = getWavelength(obs.system, sel_sigs[i2]);
                    if (wavelengths[0] <= 0.0 || wavelengths[1] <= 0.0) {
                        log_manager_.getLogger("ppp_model")->warn("Invalid wavelength for pair {} on system {} for satellite {}: wl1={:.3f}, wl2={:.3f} m",
                            ref_type, system_str, obs.satellite, wavelengths[0], wavelengths[1]);
                        continue;
                    }
                }

                for (size_t j = 0; j < 2; ++j) {
                    size_t idx = (j == 0 ? i1 : i2);
                    const auto& type = sel_sigs[idx];
                    double antenna_corr = getAntennaCorrection(obs.satellite, type, obs.system, rx_pos, sat_pos,
                        elevation, azimuth, rx_antenna_type, obs.time);
                    double bias_corr = getBiasCorrection(obs.satellite, type, obs.time);

                    // Computed or modeled observation
                    //  1) State-independent
                    modeled_obs[j] = -(sat_clock + relativistic_corr) * Constants::SPEED_OF_LIGHT +
                        antenna_corr + bias_corr + shapiro_delay + sagnac_corr + atm_loading_corr + solid_tide;

                    //  2) State-dependent
                    /*modeled_obs[j] = range + state.clock_bias + state.troposphere_zenith_delay -
                        (sat_clock + relativistic_corr) * Constants::SPEED_OF_LIGHT +
                        antenna_corr + bias_corr + shapiro_delay + sagnac_corr + atm_loading_corr;*/

                    variances[j] = obs.variances.count(type) ? obs.variances.at(type) : (meas_type == 'C' ? 0.3 * 0.3 : 0.0015 * 0.0015);
                    frequencies[j] = getFrequency(type, obs.system, obs.satellite);
                    if (frequencies[j] <= 0.0) {
                        log_manager_.getLogger("ppp_model")->warn("Invalid frequency for type {} on system {} for satellite {}",
                            type, system_str, obs.satellite);
                        continue;
                    }
                }

                double observed_value = 0.0;
                double modeled_value = 0.0;
                double variance_value = 0.0;
                double c1 = 0.0, c2 = 0.0;

                double f1_sq = frequencies[0] * frequencies[0];
                double f2_sq = frequencies[1] * frequencies[1];
                c1 = f1_sq / (f1_sq - f2_sq);
                c2 = -f2_sq / (f1_sq - f2_sq);
                if (!std::isfinite(c1) || !std::isfinite(c2)) {
                    log_manager_.getLogger("ppp_model")->warn("Non-finite coefficients for dual-frequency pair {} for {}", ref_type, obs.satellite);
                    continue;
                }

                // Form linear measurement combination
                modeled_value = c1 * modeled_obs[0] + c2 * modeled_obs[1];
                variance_value = c1 * c1 * variances[0] + c2 * c2 * variances[1];

                // Compute observed_value in meters, here it is ionosphere-free
                for (size_t j = 0; j < 2; ++j) {
                    double coeff = (j == 0 ? c1 : c2);
                    double meas = obs.measurements.at(sel_sigs[j == 0 ? i1 : i2]);
                    if (meas_type == 'L') meas *= wavelengths[j]; // Convert Carrier Phase Units: Cycles to Meters
                    observed_value += coeff * meas;
                }
                if (observed_value == 0.0) {
                    log_manager_.getLogger("ppp_model")->warn("Failed to compute dual-frequency observed value for {} ({})", ref_type, obs.satellite);
                    continue;
                }

                // Ambiguity initialization, add to state vector
                if (meas_type == 'L') {
                    std::string amb_key = obs.satellite + "_" + ref_type;
                    if (!state.ambiguities.count(amb_key)) {
                        state.ambiguities[amb_key] = 0.0; // initial value
                        //state.ambiguities[amb_key] = observed_value - modeled_value; // initial value
                        state_manager_.addAmbiguity(amb_key, state.ambiguities[amb_key]); // Ambiguity in meters
                        log_manager_.getLogger("ppp_model")->debug("Initialized iono-free ambiguity for {}: {:.3f} m",
                            amb_key, state.ambiguities[amb_key]);
                    }
                    /*else {
                        modeled_value += state.ambiguities[amb_key];
                    }*/
                }
                

                ppp_obs.observed[ref_type] = observed_value;
                ppp_obs.modeled[ref_type] = modeled_value;
                ppp_obs.residuals[ref_type] = observed_value - modeled_value;
                ppp_obs.variances[ref_type] = variance_value;

                if (meas_type == 'C') {
                    double mp_weight = computeMultipathVarianceWeight(ppp_obs, sel_sigs[i1], elevation);
                    ppp_obs.variances[ref_type] *= (1.0 / mp_weight);
                    log_manager_.getLogger("ppp_model")->trace("Multipath weight for {} {}: {:.3f}, adjusted variance: {:.6f}",
                        obs.satellite, ref_type, mp_weight, ppp_obs.variances[ref_type]);
                }

                log_manager_.getLogger("ppp_model")->trace("Iono-free {} for {} ({}): observed={:.3f} m, modeled={:.3f} m, residual={:.3f} m, variance={:.6f}, comb_type=dual(pair), mode={}",
                    ref_type, obs.satellite, meas_str, observed_value, modeled_value, ppp_obs.residuals[ref_type],
                    ppp_obs.variances[ref_type], config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");

                success = true;
            }
        }

        return success;
    }

    bool PppModel::processSingleFreqObservation(const RinexObservation& obs, PppState& state,
        PppObservation& ppp_obs, const std::string& system_str,
        const std::string& type, const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos, double elevation, double azimuth,
        const std::string& rx_antenna_type, double range, double tropo_delay,
        double sat_clock, double shapiro_delay, double sagnac_corr,
        double atm_loading_corr, double relativistic_corr, double solid_tide) {
        if (!obs.measurements.count(type)) {
            log_manager_.getLogger("ppp_model")->warn("Missing measurement for {} {} in uncombined mode", obs.satellite, type);
            return false;
        }
        double observed_value = obs.measurements.at(type);
        double modeled_value = 0.0;
        double variance_value = obs.variances.count(type) ? obs.variances.at(type) : (type[0] == 'C' ? 0.3 * 0.3 : 0.0015 * 0.0015);

        try {
            double iono_delay = config_.getProcessingMode() == ProcessingMode::PPP_RTK ?
                computeIonosphericDelay(obs.time, rx_pos, sat_pos, type, obs.system, obs.measurements) :
                computeInitialIonoDelay(obs.time, rx_pos);
            double antenna_corr = getAntennaCorrection(obs.satellite, type, obs.system, rx_pos, sat_pos,
                elevation, azimuth, rx_antenna_type, obs.time);
            double bias_corr = getBiasCorrection(obs.satellite, type, obs.time);

            double model_summation = state.clock_bias * Constants::SPEED_OF_LIGHT +
                tropo_delay -
                (sat_clock + relativistic_corr) * Constants::SPEED_OF_LIGHT +
                iono_delay +
                antenna_corr +
                bias_corr +
                shapiro_delay +
                sagnac_corr +
                atm_loading_corr;

            if (type[0] == 'L') {
                double wavelength = getWavelength(obs.system, type);
                if (wavelength <= 0.0) {
                    log_manager_.getLogger("ppp_model")->warn("Invalid wavelength for type {} on system {} for satellite {}",
                        type, system_str, obs.satellite);
                    return false;
                }
                double observed_meters = observed_value * wavelength; // Convert cycles to meters
                modeled_value = range + model_summation; // Modeled in meters
                std::string amb_key = obs.satellite + "_" + type;
                if (!state.ambiguities.count(amb_key)) {
                    state.ambiguities[amb_key] = observed_meters - modeled_value;
                    //state.ambiguities[amb_key] = std::round(state.ambiguities[amb_key] / wavelength) * wavelength; // comment, use float ambiguities
                    state_manager_.addAmbiguity(amb_key, state.ambiguities[amb_key]);
                    log_manager_.getLogger("ppp_model")->debug("Initialized uncombined ambiguity for {}: {:.3f} cycles, wavelength: {:.3f} m, value: {:.3f} m",
                        amb_key, state.ambiguities[amb_key] / wavelength, wavelength, state.ambiguities[amb_key]);
                }
                modeled_value += state.ambiguities[amb_key]; // Add ambiguity in meters
                ppp_obs.observed[type] = observed_meters; // Store observed in meters
            }
            else if (type[0] == 'C') {
                modeled_value = range + model_summation; // Code units: meters
                ppp_obs.observed[type] = observed_value; // Already in meters
            }

            ppp_obs.modeled[type] = modeled_value;
            ppp_obs.residuals[type] = ppp_obs.observed[type] - modeled_value;
            ppp_obs.variances[type] = variance_value;

            // Apply multipath mitigation for code observations
            if (type[0] == 'C') {
                double mp_weight = computeMultipathVarianceWeight(ppp_obs, type, elevation);
                ppp_obs.variances[type] *= (1.0 / mp_weight);
                log_manager_.getLogger("ppp_model")->trace("Multipath weight for {} {}: {:.3f}, adjusted variance: {:.6f}",
                    obs.satellite, type, mp_weight, ppp_obs.variances[type]);
            }

            log_manager_.getLogger("ppp_model")->trace("Uncombined {} for {}: observed={:.3f} m, modeled={:.3f} m, residual={:.3f} m, mode={}",
                type, obs.satellite, ppp_obs.observed[type], modeled_value, ppp_obs.residuals[type],
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger("ppp_model")->warn("Error processing uncombined measurement for {} {}: {}, mode={}",
                obs.satellite, type, e.what(),
                config_.getProcessingMode() == ProcessingMode::PPP ? "PPP" : "PPP-RTK");
            return false;
        }
    }

    double PppModel::computeGeometricRange(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const {
        return (sat_pos - rx_pos).norm(); // meters
    }

    double PppModel::getTroposphericDelay(double elevation, double zenith_delay) const {
        // Validate inputs
        if (elevation < 0.0 || elevation > 90.0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid elevation for tropospheric delay: {:.3f} deg", elevation);
            return 10.0;
        }
        if (zenith_delay < 0.5 || zenith_delay > 5.0 || !std::isfinite(zenith_delay)) {
            log_manager_.getLogger("ppp_model")->warn("Invalid zenith delay: {:.3f} m (expected 0.5 to 5.0 m)", zenith_delay);
            return 2.3;
        }

        // Compute mapping function (Saastamoinen-based) with numerical stability
        double el_rad = elevation * Constants::PI / 180.0;
        double sin_el = std::sin(el_rad);
        double tan_el = std::tan(el_rad);
        // Clamp denominator to avoid numerical instability at low elevations
        constexpr double MIN_DENOM = 0.01;
        double denom = sin_el + 0.00143 / (tan_el + 0.0445);
        if (denom < MIN_DENOM) {
            log_manager_.getLogger("ppp_model")->debug("Clamped mapping function denominator at {:.3f} for elevation {:.3f} deg", MIN_DENOM, elevation);
            denom = MIN_DENOM;
        }
        double mapping = 1.0 / denom;
        double delay = zenith_delay * mapping;

        // Validate output
        if (!std::isfinite(delay) || delay < 0.0) {
            log_manager_.getLogger("ppp_model")->warn("Non-finite or negative tropospheric delay: {:.3f} m, elevation: {:.3f} deg", delay, elevation);
            //return 0.0;
            return 2.3;
        }

        log_manager_.getLogger("ppp_model")->trace("Computed tropospheric delay: {:.3f} m, elevation: {:.3f} deg, zenith_delay: {:.3f} m, mapping: {:.3f}",
            delay, elevation, zenith_delay, mapping);
        return delay;
    }

    double PppModel::computeIonosphericDelay(const GnssTime& time,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        const std::string& freq_type,
        System system,
        const std::map<std::string, double>& measurements) const {
        double freq = getFrequency(freq_type, system, "");
        if (freq <= 0.0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid frequency for {} on system {}", freq_type, static_cast<int>(system));
            return 0.0;
        }

        std::string system_str = MeasurementModel::getSystemString(system);
        auto it = signal_priorities_.find(system_str);
        if (it == signal_priorities_.end()) {
            log_manager_.getLogger("ppp_model")->warn("No signal priorities defined for system {}", system_str);
            return 0.0;
        }
        const auto& signals = it->second;

        if (signals.size() >= 3 && measurements.count(signals[0]) && measurements.count(signals[1]) && measurements.count(signals[2])) {
            log_manager_.getLogger("ppp_model")->trace("Triple-frequency iono-free combination available for {}: ionospheric delay=0.0 m", freq_type);
            return 0.0; // Ionosphere-free, no delay
        }
        if (signals.size() >= 2 && measurements.count(signals[0]) && measurements.count(signals[1])) {
            log_manager_.getLogger("ppp_model")->trace("Dual-frequency iono-free combination available for {}: ionospheric delay=0.0 m", freq_type);
            return 0.0; // Ionosphere-free, no delay
        }
        if (klobuchar_coeffs_.size() != 8) {
            log_manager_.getLogger("ppp_model")->warn("No Klobuchar coefficients available for {}", freq_type);
            return 0.0;
        }
        double delay = computeKlobucharDelay(time, rx_pos, sat_pos, freq);
        log_manager_.getLogger("ppp_model")->trace("Computed Klobuchar ionospheric delay for {}: {:.3f} m", freq_type, delay);
        return delay;
    }

    double PppModel::computeKlobucharDelay(const GnssTime& time,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        double frequency) const {
        double a0 = klobuchar_coeffs_[0], a1 = klobuchar_coeffs_[1], a2 = klobuchar_coeffs_[2], a3 = klobuchar_coeffs_[3];
        double b0 = klobuchar_coeffs_[4], b1 = klobuchar_coeffs_[5], b2 = klobuchar_coeffs_[6], b3 = klobuchar_coeffs_[7];

        double t = fmod(time.tow, 86400.0);
        double phi = std::asin(rx_pos.z() / rx_pos.norm()) * 180.0 / Constants::PI; // Geodetic latitude (degrees)
        double lambda = std::atan2(rx_pos.y(), rx_pos.x()) * 180.0 / Constants::PI; // Geodetic longitude (degrees)
        double el = computeElevation(rx_pos, sat_pos);
        double az = computeAzimuth(rx_pos, sat_pos);
        if (az < 0.0) az += 360.0;

        double psi = 0.0137 / (el / 180.0 + 0.11) - 0.022;
        double phi_i = phi / 180.0 + psi * std::cos(az * Constants::PI / 180.0);
        if (phi_i > 0.416) phi_i = 0.416;
        if (phi_i < -0.416) phi_i = -0.416;
        double lambda_i = lambda / 180.0 + psi * std::sin(az * Constants::PI / 180.0) / std::cos(phi_i * Constants::PI);
        double phi_m = phi_i + 0.064 * std::cos((lambda_i - 1.617) * Constants::PI);

        double t_i = 43200.0 * lambda_i + t;
        t_i = fmod(t_i, 86400.0);
        if (t_i < 0) t_i += 86400.0;

        double F = 1.0 + 16.0 * std::pow((0.53 - el / 180.0), 3);
        double AMP = a0 + a1 * phi_m + a2 * std::pow(phi_m, 2) + a3 * std::pow(phi_m, 3);
        if (AMP < 0) AMP = 0;
        double PER = b0 + b1 * phi_m + b2 * std::pow(phi_m, 2) + b3 * std::pow(phi_m, 3);
        if (PER < 72000) PER = 72000;
        double X = 2.0 * Constants::PI * (t_i - 50400.0) / PER;
        double delay;
        if (std::abs(X) < 1.57) {
            delay = F * (5e-9 + AMP * (1 - X * X / 2 + X * X * X * X / 24));
        }
        else {
            delay = F * 5e-9;
        }

        double f_L1 = Constants::GPS_L1_FREQ;
        delay *= (f_L1 * f_L1) / (frequency * frequency);
        return delay * Constants::SPEED_OF_LIGHT;
    }

    double PppModel::computeIonoFreeCombination(const std::map<std::string, double>& measurements,
        System system,
        const std::string& signal1,
        const std::string& signal2) const {
        if (!measurements.count(signal1) || !measurements.count(signal2)) {
            log_manager_.getLogger("ppp_model")->warn("Missing measurements for signals {} or {} in iono-free combination", signal1, signal2);
            return 0.0;
        }

        double freq1 = getFrequency(signal1, system, "");
        double freq2 = getFrequency(signal2, system, "");
        if (freq1 <= 0.0 || freq2 <= 0.0 || std::abs(freq1 - freq2) < 1e-6) {
            log_manager_.getLogger("ppp_model")->warn("Invalid or identical frequencies for {} ({:.3f} MHz) and {} ({:.3f} MHz)",
                signal1, freq1 / 1e6, signal2, freq2 / 1e6);
            return 0.0;
        }

        double f1_sq = freq1 * freq1, f2_sq = freq2 * freq2;
        double c1 = f1_sq / (f1_sq - f2_sq);
        double c2 = -f2_sq / (f1_sq - f2_sq);
        if (!std::isfinite(c1) || !std::isfinite(c2)) {
            log_manager_.getLogger("ppp_model")->warn("Non-finite coefficients for iono-free combination: c1={:.3f}, c2={:.3f}", c1, c2);
            return 0.0;
        }

        double combination = c1 * measurements.at(signal1) + c2 * measurements.at(signal2);
        if (!std::isfinite(combination)) {
            log_manager_.getLogger("ppp_model")->warn("Non-finite iono-free combination for signals {} and {}: {:.3f}",
                signal1, signal2, combination);
            return 0.0;
        }

        log_manager_.getLogger("ppp_model")->trace("Computed iono-free combination for signals {} and {}: value={:.3f} m, "
            "freqs=[{:.3f}, {:.3f}] MHz, coeffs=[{:.3f}, {:.3f}]",
            signal1, signal2, combination, freq1 / 1e6, freq2 / 1e6, c1, c2);
        return combination;
    }

    double PppModel::getSatelliteClockCorrection(const std::string& satellite, const GnssTime& time) const {
        // Validate inputs
        if (satellite.empty()) {
            log_manager_.getLogger("ppp_model")->warn("Empty satellite ID for clock correction at TOW {:.3f}", time.tow);
            return 0.0;
        }
        if (time.tow < 0.0 || time.week < 0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid time for clock correction for satellite {}: week {}, TOW {:.3f}",
                satellite, time.week, time.tow);
            return 0.0;
        }
        if (clocks_.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No clock data available for satellite {} at TOW {:.3f}", satellite, time.tow);
            return 0.0;
        }

        // Interpolate clock correction
        double clock_corr = interpolateClock(satellite, time, clocks_);
        std::string system_str = satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown";
        if (std::isfinite(clock_corr) && std::abs(clock_corr) < 1e-3) { // Max 1 ms
            log_manager_.getLogger("ppp_model")->trace("Interpolated clock correction for {} ({}) at TOW {:.3f}: {:.3e} s",
                satellite, system_str, time.tow, clock_corr);
            return clock_corr;
        }

        log_manager_.getLogger("ppp_model")->warn("Invalid clock correction for {} ({}) at TOW {:.3f}: {:.3e} s",
            satellite, system_str, time.tow, clock_corr);
        return 0.0;
    }

    double PppModel::interpolateClock(const std::string& satellite, const GnssTime& time,
        const std::vector<ClockData>& clocks) const {
        constexpr int order = 4; // e.g., 6th-order Lagrange interpolation (7 points)
        constexpr double MAX_TIME_WINDOW = 7200.0; // 2-hour window
        constexpr double MIN_TIME_DIFF = 1e-3; // Minimum time difference (s)
        constexpr int MIN_ORDER = 2; // Minimum points for interpolation (linear)

        // Collect candidates within time window
        std::vector<ClockData> candidates;
        std::string system_str = satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown";
        for (const auto& clock : clocks) {
            if (clock.satellite == satellite && std::abs(clock.time.tow - time.tow) < MAX_TIME_WINDOW &&
                std::isfinite(clock.clock_bias)) {
                candidates.push_back(clock);
            }
        }

        // Handle insufficient data
        if (candidates.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No clock data for {} ({}) at TOW {:.3f}", satellite, system_str, time.tow);
            return 0.0;
        }
        if (candidates.size() < MIN_ORDER) {
            log_manager_.getLogger("ppp_model")->debug("Insufficient clock data for {} ({}) at TOW {:.3f}: found {} points, need at least {}",
                satellite, system_str, time.tow, candidates.size(), MIN_ORDER);
            log_manager_.getLogger("ppp_model")->trace("Using nearest clock for {} ({}) at TOW {:.3f}: {:.3e} s",
                satellite, system_str, candidates[0].time.tow, candidates[0].clock_bias);
            return candidates[0].clock_bias;
        }

        // Log pre-sort candidates
        log_manager_.getLogger("ppp_model")->trace("Found {} candidate CLK points for {} ({}) at TOW {:.3f} (window ±{:.0f} s)",
            candidates.size(), satellite, system_str, time.tow, MAX_TIME_WINDOW);
        // Log only the first and last candidate CLK points (if they exist)
        if (!candidates.empty()) {
            // Log first point
            log_manager_.getLogger("ppp_model")->trace(
                "First CLK point for {} at TOW {:.3f}: {:.3e} s",
                satellite, candidates[0].time.tow, candidates[0].clock_bias);

            // Log last point if there are at least two candidates
            if (candidates.size() > 1) {
                log_manager_.getLogger("ppp_model")->trace(
                    "Last CLK point for {} at TOW {:.3f}: {:.3e} s",
                    satellite, candidates[candidates.size() - 1].time.tow,
                    candidates[candidates.size() - 1].clock_bias);
            }
        }

        // Sort by time proximity to select closest points
        std::sort(candidates.begin(), candidates.end(),
            [&time](const ClockData& a, const ClockData& b) {
                double diff_a = std::abs(a.time.tow - time.tow);
                double diff_b = std::abs(b.time.tow - time.tow);
                return diff_a < diff_b || (std::abs(diff_a - diff_b) < 1e-6 && a.time.tow < b.time.tow);
            });

        // Adjust order if insufficient points
        size_t effective_order = std::min(static_cast<size_t>(order), candidates.size());
        candidates.resize(effective_order);
        log_manager_.getLogger("ppp_model")->trace("Selected {} closest points for {} ({}) at TOW {:.3f}",
            effective_order, satellite, system_str, time.tow);

        // Sort selected points chronologically by TOW for stable interpolation
        std::sort(candidates.begin(), candidates.end(),
            [](const ClockData& a, const ClockData& b) {
                return a.time.tow < b.time.tow;
            });

        // Log sorted CLK points
        for (size_t i = 0; i < candidates.size(); ++i) {
            double time_diff = candidates[i].time.tow - time.tow;
            log_manager_.getLogger("ppp_model")->trace("Sorted CLK point {} for {} ({}) at TOW {:.3f} (diff {:.1f} s): {:.3e} s",
                i, satellite, system_str, candidates[i].time.tow, time_diff, candidates[i].clock_bias);
        }

        // Check for sufficient time separation
        for (size_t i = 0; i < candidates.size(); ++i) {
            for (size_t j = i + 1; j < candidates.size(); ++j) {
                if (std::abs(candidates[i].time.tow - candidates[j].time.tow) < MIN_TIME_DIFF) {
                    log_manager_.getLogger("ppp_model")->warn("Time difference too small for {} ({}) at TOW {:.3f}: {:.3f} s between epochs",
                        satellite, system_str, time.tow, std::abs(candidates[i].time.tow - candidates[j].time.tow));
                    auto it = std::min_element(candidates.begin(), candidates.end(),
                        [&time](const ClockData& a, const ClockData& b) {
                            return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
                        });
                    log_manager_.getLogger("ppp_model")->trace("Fallback to nearest clock for {} ({}) at TOW {:.3f}: {:.3e} s",
                        satellite, system_str, it->time.tow, it->clock_bias);
                    return it->clock_bias;
                }
            }
        }

        // Lagrange interpolation
        double clock_corr = 0.0;
        double t = time.tow;
        for (size_t i = 0; i < effective_order; ++i) {
            double li = 1.0;
            for (size_t j = 0; j < effective_order; ++j) {
                if (i != j) {
                    li *= (t - candidates[j].time.tow) / (candidates[i].time.tow - candidates[j].time.tow);
                }
            }
            clock_corr += li * candidates[i].clock_bias;
        }

        // Validate result
        if (!std::isfinite(clock_corr) || std::abs(clock_corr) > 1e-3) { // Max 1 ms
            log_manager_.getLogger("ppp_model")->warn("Invalid clock correction for {} ({}) at TOW {:.3f}: {:.3e} s",
                satellite, system_str, time.tow, clock_corr);
            auto it = std::min_element(candidates.begin(), candidates.end(),
                [&time](const ClockData& a, const ClockData& b) {
                    return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
                });
            log_manager_.getLogger("ppp_model")->trace("Fallback to nearest clock for {} ({}) at TOW {:.3f}: {:.3e} s",
                satellite, system_str, it->time.tow, it->clock_bias);
            return it->clock_bias;
        }

        // Log successful interpolation
        log_manager_.getLogger("ppp_model")->trace("Interpolated clock correction for {} ({}) at TOW {:.3f}: {:.3e} s",
            satellite, system_str, time.tow, clock_corr);
        return clock_corr;
    }

    double PppModel::computeRelativisticCorrection(const std::string& satellite, const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel) const {
        constexpr double C_SQUARED = Constants::SPEED_OF_LIGHT * Constants::SPEED_OF_LIGHT;
        constexpr double GM = Constants::GRAVITATIONAL_CONSTANT; // GM = G * M_Earth
        constexpr double MAX_CORRECTION = 1e-7; // Max 100 ns
        constexpr double MIN_POSITION_NORM = 1.8e7; // ~20,000 km
        constexpr double MAX_POSITION_NORM = 3.2e7; // ~32,000 km
        constexpr double MAX_VELOCITY_NORM = 1e4; // ~10 km/s

        // Validate inputs
        std::string system_str = satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown";
        if (!sat_pos.allFinite() || !sat_vel.allFinite()) {
            log_manager_.getLogger("ppp_model")->warn("Invalid relativistic inputs for {} ({}): sat_pos=({:.3f}, {:.3f}, {:.3f}), sat_vel=({:.3f}, {:.3f}, {:.3f})",
                satellite, system_str, sat_pos.x(), sat_pos.y(), sat_pos.z(), sat_vel.x(), sat_vel.y(), sat_vel.z());
            return 0.0;
        }
        double pos_norm = sat_pos.norm();
        double vel_norm = sat_vel.norm();
        if (pos_norm < MIN_POSITION_NORM || pos_norm > MAX_POSITION_NORM || vel_norm > MAX_VELOCITY_NORM) {
            log_manager_.getLogger("ppp_model")->warn("Unrealistic relativistic input norms for {} ({}): pos_norm={:.3e} m, vel_norm={:.3e} m/s",
                satellite, system_str, pos_norm, vel_norm);
            return 0.0;
        }

        // Compute special relativistic correction: -2 * (r · v) / c²
        double r_dot_v = sat_pos.dot(sat_vel);
        double special_correction = -2.0 * r_dot_v / C_SQUARED;

        // Compute Schwarzschild (gravitational) correction: -2GM/(c²r)
        double gravitational_correction = -2.0 * GM / (C_SQUARED * pos_norm);

        double correction = special_correction + gravitational_correction;
        if (!std::isfinite(correction) || std::abs(correction) > MAX_CORRECTION) {
            log_manager_.getLogger("ppp_model")->warn("Excessive relativistic correction for {} ({}): {:.3e} s, special={:.3e}, gravitational={:.3e}, r_dot_v={:.3e}",
                satellite, system_str, correction, special_correction, gravitational_correction, r_dot_v);
            return 0.0;
        }

        log_manager_.getLogger("ppp_model")->trace("Relativistic correction for {} ({}): {:.3e} s (special={:.3e}, gravitational={:.3e}), r_dot_v={:.3e}",
            satellite, system_str, correction, special_correction, gravitational_correction, r_dot_v);
        return correction;
    }

    Eigen::Vector3d PppModel::getSatellitePosition(const std::string& satellite, const GnssTime& time) const {
        constexpr int order = 11; // Interpolation order for position
        constexpr double MIN_POSITION_NORM = 1.8e7; // Typical GNSS orbit ~20,000 km
        constexpr double MAX_POSITION_NORM = 3.2e7;
        constexpr double MIN_TIME_DIFF = 1.0; // Minimum time difference between points (s)
        constexpr double MAX_TIME_WINDOW = 7200.0; // Maximum time window for interpolation (s)

        // Validate inputs
        if (satellite.empty()) {
            log_manager_.getLogger("ppp_model")->warn("Empty satellite ID for position interpolation at TOW {:.3f}", time.tow);
            return Eigen::Vector3d::Zero();
        }
        if (orbits_.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No orbit data available for {} at TOW {:.3f}", satellite, time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Find candidate orbit data
        std::vector<OrbitData> candidates;
        std::string system_str = satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown";
        for (const auto& data : orbits_) {
            if (data.satellite == satellite && data.position_valid) {
                double time_diff = std::abs(data.time.tow - time.tow);
                if (time_diff < MAX_TIME_WINDOW && data.position.allFinite() &&
                    data.position.norm() >= MIN_POSITION_NORM && data.position.norm() <= MAX_POSITION_NORM) {
                    candidates.push_back(data);
                }
            }
        }

        // Log pre-sort candidates
        log_manager_.getLogger("ppp_model")->trace("Found {} candidate SP3 points for {} ({}) at TOW {:.3f} (window ±{:.0f} s)",
            candidates.size(), satellite, system_str, time.tow, MAX_TIME_WINDOW);
        // Log only the first and last candidate points (if they exist)
        if (!candidates.empty()) {
            // Log first point
            log_manager_.getLogger("ppp_model")->trace(
                "First SP3 point for {} at TOW {:.3f}: ({:.3f}, {:.3f}, {:.3f}) km",
                satellite, candidates[0].time.tow,
                candidates[0].position.x() / 1000.0, candidates[0].position.y() / 1000.0, candidates[0].position.z() / 1000.0);

            // Log last point if there are at least two candidates
            if (candidates.size() > 1) {
                log_manager_.getLogger("ppp_model")->trace(
                    "Last SP3 point for {} at TOW {:.3f}: ({:.3f}, {:.3f}, {:.3f}) km",
                    satellite, candidates[candidates.size() - 1].time.tow,
                    candidates[candidates.size() - 1].position.x() / 1000.0,
                    candidates[candidates.size() - 1].position.y() / 1000.0,
                    candidates[candidates.size() - 1].position.z() / 1000.0);
            }
        }

        // Check if sufficient data points
        if (candidates.size() < order) {
            log_manager_.getLogger("ppp_model")->debug("Insufficient orbit data for {} ({}) at TOW {:.3f}: found {} points, need {}",
                satellite, system_str, time.tow, candidates.size(), order);
            if (!candidates.empty()) {
                std::sort(candidates.begin(), candidates.end(),
                    [&time](const OrbitData& a, const OrbitData& b) {
                        double diff_a = std::abs(a.time.tow - time.tow);
                        double diff_b = std::abs(b.time.tow - time.tow);
                        return diff_a < diff_b || (std::abs(diff_a - diff_b) < 1e-6 && a.time.tow < b.time.tow);
                    });
                auto it = candidates.begin(); // Closest after sort
                log_manager_.getLogger("ppp_model")->trace("Using nearest position for {} ({}) at TOW {:.3f}: ({:.3f}, {:.3f}, {:.3f}) m, norm: {:.3e}",
                    satellite, system_str, it->time.tow, it->position.x(), it->position.y(), it->position.z(), it->position.norm());
                last_interpolated_velocity_.erase(satellite);
                return it->position;
            }
            log_manager_.getLogger("ppp_model")->warn("No valid orbit data for {} ({}) at TOW {:.3f}", satellite, system_str, time.tow);
            last_interpolated_velocity_.erase(satellite);
            return Eigen::Vector3d::Zero();
        }

        // Sort candidates by time proximity to select the closest points
        std::sort(candidates.begin(), candidates.end(),
            [&time](const OrbitData& a, const OrbitData& b) {
                double diff_a = std::abs(a.time.tow - time.tow);
                double diff_b = std::abs(b.time.tow - time.tow);
                return diff_a < diff_b || (std::abs(diff_a - diff_b) < 1e-6 && a.time.tow < b.time.tow);
            });
        candidates.resize(order);

        // Find nearest point for error calculation (before chronological sort)
        auto nearest_it = std::min_element(candidates.begin(), candidates.end(),
            [&time](const OrbitData& a, const OrbitData& b) {
                return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
            });
        Eigen::Vector3d nearest_pos = nearest_it->position;

        // Sort the selected points chronologically by TOW for stable interpolation
        std::sort(candidates.begin(), candidates.end(),
            [](const OrbitData& a, const OrbitData& b) {
                return a.time.tow < b.time.tow;
            });

        // Log sorted SP3 points
        for (size_t i = 0; i < candidates.size(); ++i) {
            double time_diff = candidates[i].time.tow - time.tow;
            log_manager_.getLogger("ppp_model")->trace("Sorted SP3 point {} for {} ({}) at TOW {:.3f} (diff {:.1f} s): ({:.3f}, {:.3f}, {:.3f}) km, ({:.3f}, {:.3f}, {:.3f}) m",
                i, satellite, system_str, candidates[i].time.tow, time_diff,
                candidates[i].position.x() / 1000.0, candidates[i].position.y() / 1000.0, candidates[i].position.z() / 1000.0,
                candidates[i].position.x(), candidates[i].position.y(), candidates[i].position.z());
        }

        // Check for sufficient time separation
        for (size_t i = 0; i < candidates.size(); ++i) {
            for (size_t j = i + 1; j < candidates.size(); ++j) {
                if (std::abs(candidates[i].time.tow - candidates[j].time.tow) < MIN_TIME_DIFF) {
                    log_manager_.getLogger("ppp_model")->warn("Time difference too small for {} ({}) at TOW {:.3f}: {:.3f} s between epochs",
                        satellite, system_str, time.tow, std::abs(candidates[i].time.tow - candidates[j].time.tow));
                    std::sort(candidates.begin(), candidates.end(),
                        [&time](const OrbitData& a, const OrbitData& b) {
                            double diff_a = std::abs(a.time.tow - time.tow);
                            double diff_b = std::abs(b.time.tow - time.tow);
                            return diff_a < diff_b || (std::abs(diff_a - diff_b) < 1e-6 && a.time.tow < b.time.tow);
                        });
                    auto it = candidates.begin();
                    log_manager_.getLogger("ppp_model")->trace("Fallback to nearest position for {} ({}) at TOW {:.3f}: ({:.3f}, {:.3f}, {:.3f}) m",
                        satellite, system_str, it->time.tow, it->position.x(), it->position.y(), it->position.z());
                    last_interpolated_velocity_.erase(satellite);
                    return it->position;
                }
            }
        }


        // Lagrange interpolation for position
        Eigen::Vector3d interpolated_pos(0.0, 0.0, 0.0);
        double t = time.tow;
        for (size_t i = 0; i < order; ++i) {
            double li = 1.0;
            for (size_t j = 0; j < order; ++j) {
                if (i != j) {
                    li *= (t - candidates[j].time.tow) / (candidates[i].time.tow - candidates[j].time.tow);
                }
            }
            interpolated_pos += li * candidates[i].position;
        }

        // Validate interpolated position
        if (!interpolated_pos.allFinite() ||
            interpolated_pos.norm() < MIN_POSITION_NORM || interpolated_pos.norm() > MAX_POSITION_NORM) {
            log_manager_.getLogger("ppp_model")->warn("Invalid interpolated position for {} ({}) at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, norm: {:.3e}",
                satellite, system_str, time.tow, interpolated_pos.x(), interpolated_pos.y(), interpolated_pos.z(), interpolated_pos.norm());
            std::sort(candidates.begin(), candidates.end(),
                [&time](const OrbitData& a, const OrbitData& b) {
                    double diff_a = std::abs(a.time.tow - time.tow);
                    double diff_b = std::abs(b.time.tow - time.tow);
                    return diff_a < diff_b || (std::abs(diff_a - diff_b) < 1e-6 && a.time.tow < b.time.tow);
                });
            auto it = candidates.begin();
            log_manager_.getLogger("ppp_model")->trace("Fallback to nearest position for {} ({}) at TOW {:.3f}: ({:.3f}, {:.3f}, {:.3f}) m",
                satellite, system_str, it->time.tow, it->position.x(), it->position.y(), it->position.z());
            last_interpolated_velocity_.erase(satellite);
            return it->position;
        }

        // Log position error vs. nearest SP3 point
        // TODO: Use time_diff to find nearest point
        //Eigen::Vector3d pos_error = interpolated_pos - nearest_pos;
        //log_manager_.getLogger("ppp_model")->trace("Position error vs. nearest SP3 for {} ({}) at TOW {:.3f}: ({:.3f}, {:.3f}, {:.3f}) m, norm: {:.3f}",
        //    satellite, system_str, time.tow, pos_error.x(), pos_error.y(), pos_error.z(), pos_error.norm());

        // Log successful interpolation
        log_manager_.getLogger("ppp_model")->trace("Interpolated position for {} ({}) at TOW {:.3f}: ({:.3f}, {:.3f}, {:.3f}) m, norm: {:.3e}",
            satellite, system_str, time.tow, interpolated_pos.x(), interpolated_pos.y(), interpolated_pos.z(), interpolated_pos.norm());
        return interpolated_pos;
}

    double PppModel::computeElevation(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const {
        Eigen::Vector3d los = (sat_pos - rx_pos).normalized();
        double elevation = std::asin(los(2)) * 180.0 / Constants::PI;
        log_manager_.getLogger("ppp_model")->trace("Computed elevation: {:.3f} degrees", elevation);
        return elevation;
    }

    double PppModel::computeAzimuth(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const {
        // Compute line-of-sight vector in ECEF
        Eigen::Vector3d los = sat_pos - rx_pos;
        if (los.norm() < 1e-6) {
            log_manager_.getLogger("ppp_model")->warn("Invalid LOS for azimuth computation: rx_pos=({:.3f}, {:.3f}, {:.3f}), sat_pos=({:.3f}, {:.3f}, {:.3f})",
                rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }

        // Project LOS onto local ENU (East, North, Up) plane
        double lat = std::asin(rx_pos.z() / rx_pos.norm());
        double lon = std::atan2(rx_pos.y(), rx_pos.x());
        Eigen::Matrix3d ecef_to_enu;
        ecef_to_enu << -std::sin(lon), std::cos(lon), 0.0,
            -std::sin(lat) * std::cos(lon), -std::sin(lat) * std::sin(lon), std::cos(lat),
            std::cos(lat)* std::cos(lon), std::cos(lat)* std::sin(lon), std::sin(lat);
        Eigen::Vector3d los_enu = ecef_to_enu * los;

        // Compute azimuth in degrees (0 to 360)
        double azimuth = std::atan2(los_enu.x(), los_enu.y()) * 180.0 / Constants::PI;
        if (azimuth < 0.0) {
            azimuth += 360.0;
        }
        if (!std::isfinite(azimuth) || azimuth < 0.0 || azimuth >= 360.0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid azimuth: {:.3f} deg, rx_pos=({:.3f}, {:.3f}, {:.3f}), sat_pos=({:.3f}, {:.3f}, {:.3f})",
                azimuth, rx_pos.x(), rx_pos.y(), rx_pos.z(), sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }

        log_manager_.getLogger("ppp_model")->trace("Computed azimuth: {:.3f} degrees", azimuth);
        return azimuth;
    }

    double PppModel::getWavelength(System system, const std::string& freq_type) const {
        // Validate input
        if (freq_type.empty()) {
            log_manager_.getLogger("ppp_model")->warn("Empty frequency type for wavelength computation, system={}",
                MeasurementModel::getSystemString(system));
            return 0.0;
        }

        // Get frequency, passing empty satellite ID as specific satellite not needed here
        double freq = getFrequency(freq_type, system, "");
        if (freq <= 0.0 || !std::isfinite(freq)) {
            log_manager_.getLogger("ppp_model")->warn("Invalid frequency for type {} on system {}: {:.3f} MHz",
                freq_type, MeasurementModel::getSystemString(system), freq / 1e6);
            return 0.0;
        }

        // Compute wavelength (meters) = speed of light / frequency
        double wavelength = Constants::SPEED_OF_LIGHT / freq;
        if (!std::isfinite(wavelength) || wavelength <= 0.0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid wavelength for type {} on system {}: {:.3f} m",
                freq_type, MeasurementModel::getSystemString(system), wavelength);
            return 0.0;
        }

        log_manager_.getLogger("ppp_model")->trace("Computed wavelength for type {} on system {}: {:.3f} m (freq={:.3f} MHz)",
            freq_type, MeasurementModel::getSystemString(system), wavelength, freq / 1e6);
        return wavelength;
    }

    double PppModel::getAntennaCorrection(const std::string& satellite,
        const std::string& freq_type,
        System system,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        double elevation,
        double azimuth,
        const std::string& rx_antenna_type,
        const GnssTime& time) const {
        // Initialize correction
        double correction = 0.0;
        bool is_phase = freq_type[0] == 'L';
        double wavelength = is_phase ? getWavelength(system, freq_type) : 0.0;

        // Validate inputs
        if (!rx_pos.allFinite() || !sat_pos.allFinite() || !std::isfinite(elevation) || !std::isfinite(azimuth)) {
            log_manager_.getLogger("ppp_model")->warn("Invalid inputs for antenna correction: satellite={}, freq_type={}, "
                "rx_pos=({}, {}, {}), sat_pos=({}, {}, {}), elevation={:.3f}, azimuth={:.3f}",
                satellite, freq_type, rx_pos.x(), rx_pos.y(), rx_pos.z(),
                sat_pos.x(), sat_pos.y(), sat_pos.z(), elevation, azimuth);
            return 0.0;
        }
        if (elevation < 0.0 || elevation > 90.0 || azimuth < 0.0 || azimuth >= 360.0) {
            log_manager_.getLogger("ppp_model")->warn("Out-of-range elevation ({:.3f}) or azimuth ({:.3f}) for satellite {}, freq_type={}",
                elevation, azimuth, satellite, freq_type);
            return 0.0;
        }

        // Map frequency type to ATX frequency code
        std::string atx_freq;
        if (system == System::GPS) {
            if (freq_type.find("C1") == 0 || freq_type.find("L1") == 0) {atx_freq = "G01";}
            else if (freq_type.find("C2") == 0 || freq_type.find("L2") == 0) {atx_freq = "G02";}
            else if (freq_type.find("C5") == 0 || freq_type.find("L5") == 0) {atx_freq = "G05";}
        }
        else if (system == System::Galileo) {
            if (freq_type.find("C1") == 0 || freq_type.find("L1") == 0) {atx_freq = "E01";}
            else if (freq_type.find("C5") == 0 || freq_type.find("L5") == 0) { atx_freq = "E05"; }
            else if (freq_type.find("C7") == 0 || freq_type.find("L7") == 0) { atx_freq = "E07"; }
            else if (freq_type.find("C6") == 0 || freq_type.find("L6") == 0) { atx_freq = "E06"; }
            else if (freq_type.find("C8") == 0 || freq_type.find("L8") == 0) { atx_freq = "E08"; }
        }
        else if (system == System::GLONASS) {
            if (freq_type.find("C1") == 0 || freq_type.find("L1") == 0) {atx_freq = "R01";}
            else if (freq_type.find("C2") == 0 || freq_type.find("L2") == 0) { atx_freq = "R02"; }
            else if (freq_type.find("C3") == 0 || freq_type.find("L3") == 0) { atx_freq = "R03"; }
        }
        else if (system == System::BeiDou) {
            if (freq_type.find("C2") == 0 || freq_type.find("L2") == 0) { atx_freq = "C02"; }
            else if (freq_type.find("C7") == 0 || freq_type.find("L7") == 0) { atx_freq = "C07"; }
            else if (freq_type.find("C6") == 0 || freq_type.find("L6") == 0) { atx_freq = "C06"; }
            else if (freq_type.find("C1") == 0 || freq_type.find("L1") == 0) { atx_freq = "C01"; }
            else if (freq_type.find("C5") == 0 || freq_type.find("L5") == 0) { atx_freq = "C05"; }
            else if (freq_type.find("C8") == 0 || freq_type.find("L8") == 0) { atx_freq = "C08"; }
        }
        else if (system == System::QZSS) {
            if (freq_type.find("C1") == 0 || freq_type.find("L1") == 0) { atx_freq = "J01"; }
            else if (freq_type.find("C2") == 0 || freq_type.find("L2") == 0) { atx_freq = "J02"; }
            else if (freq_type.find("C5") == 0 || freq_type.find("L5") == 0) { atx_freq = "J05"; }
            else if (freq_type.find("C6") == 0 || freq_type.find("L6") == 0) { atx_freq = "J06"; }
        }
        else if (system == System::SBAS) {
            if (freq_type.find("C1") == 0 || freq_type.find("L1") == 0) { atx_freq = "S01"; }
            else if (freq_type.find("C5") == 0 || freq_type.find("L5") == 0) { atx_freq = "S05"; }
        }

        if (atx_freq.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No ATX frequency mapping for satellite {}, freq_type={}, system={}",
                satellite, freq_type, static_cast<int>(system));
            return 0.0;
        }

        // Compute line-of-sight vector
        Eigen::Vector3d los = (sat_pos - rx_pos);
        double los_norm = los.norm();
        if (los_norm == 0.0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid LOS for satellite {}, freq_type={} at TOW {:.3f}", satellite, freq_type, time.tow);
            return 0.0;
        }
        los /= los_norm;

        // Satellite antenna correction
        double sat_pco_corr = 0.0, sat_pcv_corr = 0.0;
        std::string sat_antenna_type = satellite;
        auto sat_it = std::find_if(antennas_.begin(), antennas_.end(),
            [&sat_antenna_type](const AntennaData& a) {
                return a.type.find(sat_antenna_type) != std::string::npos;
            });
        if (sat_it != antennas_.end() && sat_it->pco.count(atx_freq)) {
            // PCO correction: Project onto line-of-sight (mm to m)
            Eigen::Vector3d pco = sat_it->pco.at(atx_freq) * 1e-3;
            if (pco.allFinite()) {
                sat_pco_corr = pco.dot(los);
                correction += sat_pco_corr;
            }
            else {
                log_manager_.getLogger("ppp_model")->warn("Invalid satellite PCO for {} {} atx_freq={} at TOW {:.3f}",
                    satellite, sat_antenna_type, atx_freq, time.tow);
            }

            // PCV correction: Interpolate based on zenith and azimuth
            if (sat_it->pcv.count(atx_freq) && !sat_it->pcv.at(atx_freq).empty()) {
                const auto& pcv_table = sat_it->pcv.at(atx_freq);
                double zenith = 90.0 - elevation;
                double dazi = sat_it->dazi > 0.0 ? sat_it->dazi : 5.0; // Use ATX DAZI or default
                double dzen = sat_it->dzen > 0.0 ? sat_it->dzen : 5.0; // Use ATX DZEN or default
                double pcv = interpolatePcv(pcv_table, zenith, azimuth, dazi, dzen) * 1e-3; // mm to m
                if (std::isfinite(pcv)) {
                    sat_pcv_corr = pcv;
                    correction += sat_pcv_corr;
                }
                else {
                    log_manager_.getLogger("ppp_model")->warn("Invalid satellite PCV for {} {} atx_freq={} at TOW {:.3f}, zenith={:.3f}, azimuth={:.3f}, dazi={:.3f}, dzen={:.3f}",
                        satellite, sat_antenna_type, atx_freq, time.tow, zenith, azimuth, dazi, dzen);
                }
            }
        }

        // Receiver antenna correction
        double rx_pco_corr = 0.0, rx_pcv_corr = 0.0;
        auto rx_it = std::find_if(antennas_.begin(), antennas_.end(),
            [&rx_antenna_type](const AntennaData& a) {
                return a.type.find(rx_antenna_type) != std::string::npos;
            });
        if (rx_it != antennas_.end() && rx_it->pco.count(atx_freq)) {
            // PCO correction: Project onto line-of-sight (mm to m)
            Eigen::Vector3d pco = rx_it->pco.at(atx_freq) * 1e-3;
            if (pco.allFinite()) {
                rx_pco_corr = pco.dot(los);
                correction -= rx_pco_corr;
            }
            else {
                log_manager_.getLogger("ppp_model")->warn("Invalid receiver PCO for {} {} atx_freq={} at TOW {:.3f}",
                    satellite, rx_antenna_type, atx_freq, time.tow);
            }

            // PCV correction: Interpolate based on zenith and azimuth
            if (rx_it->pcv.count(atx_freq) && !rx_it->pcv.at(atx_freq).empty()) {
                const auto& pcv_table = rx_it->pcv.at(atx_freq);
                double zenith = 90.0 - elevation;
                double dazi = rx_it->dazi > 0.0 ? rx_it->dazi : 5.0; // Use ATX DAZI or default
                double dzen = rx_it->dzen > 0.0 ? rx_it->dzen : 5.0; // Use ATX DZEN or default
                double pcv = interpolatePcv(pcv_table, zenith, azimuth, dazi, dzen) * 1e-3; // mm to m
                if (std::isfinite(pcv)) {
                    rx_pcv_corr = pcv;
                    correction -= rx_pcv_corr;
                }
                else {
                    log_manager_.getLogger("ppp_model")->warn("Invalid receiver PCV for {} {} atx_freq={} at TOW {:.3f}, zenith={:.3f}, azimuth={:.3f}, dazi={:.3f}, dzen={:.3f}",
                        satellite, rx_antenna_type, atx_freq, time.tow, zenith, azimuth, dazi, dzen);
                }
            }
        }

        // Phase wind-up correction (for carrier phase only)
        double wind_up_corr = 0.0;
        if (is_phase && wavelength > 0.0) {
            double wind_up = computePhaseWindUp(satellite, system, rx_pos, sat_pos, wavelength, time);
            if (std::isfinite(wind_up)) {
                wind_up_corr = wind_up;
                correction += wind_up_corr;
            }
            else {
                log_manager_.getLogger("ppp_model")->warn("Invalid phase wind-up for {} {} at TOW {:.3f}", satellite, freq_type, time.tow);
            }
        }

        // Log detailed correction components
        if (std::isfinite(correction) && correction != 0.0) {
            log_manager_.getLogger("ppp_model")->trace("Antenna correction for {} {}: total={:.6f} m (sat_pco={:.6f}, sat_pcv={:.6f}, "
                "rx_pco={:.6f}, rx_pcv={:.6f}, wind_up={:.6f}) at TOW {:.3f}, dazi={:.3f}, dzen={:.3f}",
                satellite, freq_type, correction, sat_pco_corr, sat_pcv_corr,
                -rx_pco_corr, -rx_pcv_corr, wind_up_corr, time.tow,
                sat_it != antennas_.end() ? sat_it->dazi : 5.0, sat_it != antennas_.end() ? sat_it->dzen : 5.0);
        }
        else {
            log_manager_.getLogger("ppp_model")->trace("No antenna correction for {} {} at TOW {:.3f}", satellite, freq_type, time.tow);
        }
        return correction;
    }

    double PppModel::interpolatePcv(const std::vector<std::vector<double>>& pcv_table,
        double zenith,
        double azimuth,
        double dazi,
        double dzen) const {
        if (pcv_table.empty() || pcv_table[0].empty()) {
            return 0.0;
        }

        // Zenith interpolation
        size_t zen_idx = static_cast<size_t>(zenith / dzen);
        double zen_frac = (zenith - zen_idx * dzen) / dzen;
        if (zen_idx >= pcv_table.size()) {
            zen_idx = pcv_table.size() - 1;
            zen_frac = 0.0;
        }

        // Azimuth interpolation
        size_t az_idx = static_cast<size_t>(azimuth / dazi);
        double az_frac = (azimuth - az_idx * dazi) / dazi;
        size_t num_az = pcv_table[0].size();
        if (num_az <= 1) {
            // No azimuth dependence
            double pcv = pcv_table[zen_idx][0];
            if (zen_idx + 1 < pcv_table.size()) {
                double pcv_next = pcv_table[zen_idx + 1][0];
                pcv = pcv + zen_frac * (pcv_next - pcv);
            }
            return pcv;
        }

        if (az_idx >= num_az) {
            az_idx = num_az - 1;
            az_frac = 0.0;
        }
        size_t az_idx_next = (az_idx + 1) % num_az;
        double az_frac_next = 1.0 - az_frac;

        // Bilinear interpolation
        double pcv00 = pcv_table[zen_idx][az_idx];
        double pcv10 = zen_idx + 1 < pcv_table.size() ? pcv_table[zen_idx + 1][az_idx] : pcv00;
        double pcv01 = pcv_table[zen_idx][az_idx_next];
        double pcv11 = zen_idx + 1 < pcv_table.size() ? pcv_table[zen_idx + 1][az_idx_next] : pcv01;

        double pcv = pcv00 * (1.0 - zen_frac) * az_frac_next +
            pcv10 * zen_frac * az_frac_next +
            pcv01 * (1.0 - zen_frac) * az_frac +
            pcv11 * zen_frac * az_frac;

        return pcv;
    }

    double PppModel::computePhaseWindUp(const std::string& satellite,
        System system,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        double wavelength,
        const GnssTime& time) const {

        // Phase wind-up correction for carrier phase
        Eigen::Vector3d los = (sat_pos - rx_pos).normalized();
        Eigen::Vector3d k = los;

        // Satellite antenna orientation
        Eigen::Vector3d d_s, y_s;
        Eigen::Vector3d sat_vel(0.0, 0.0, 0.0);
        auto orbit_it = std::find_if(orbits_.begin(), orbits_.end(),
            [&satellite](const OrbitData& o) { return o.satellite == satellite; });
        if (orbit_it != orbits_.end() && orbit_it->velocity_valid) {
            sat_vel = orbit_it->velocity;
        }
        computeSatelliteAttitude(system, sat_pos, sat_vel, d_s, y_s, time);

        // Receiver antenna orientation
        const auto& orientation = config_.getReceiverAntennaOrientation();
        double az = orientation.azimuth_deg * Constants::PI / 180.0;
        double el = orientation.elevation_deg * Constants::PI / 180.0;
        Eigen::Vector3d d_r(std::cos(el) * std::sin(az), std::cos(el) * std::cos(az), std::sin(el));
        // Override with RINEX antenna offsets if available
        Eigen::Vector3d hen = rinex_parser_.getAntennaDeltaHen();
        if (hen.norm() > 1e-6) {
            // Approximate orientation from H/E/N (assuming H is vertical)
            d_r = Eigen::Vector3d(hen.y(), hen.x(), hen.z()).normalized();
        }

        // Effective dipole vectors
        Eigen::Vector3d d_s_eff = d_s - k * (k.dot(d_s)) + k.cross(y_s);
        Eigen::Vector3d d_r_eff = d_r - k * (k.dot(d_r));
        if (d_s_eff.norm() < 1e-6 || d_r_eff.norm() < 1e-6) {
            log_manager_.getLogger("ppp_model")->trace("Phase wind-up for {}: 0 m (degenerate vectors)", satellite);
            return 0.0;
        }
        d_s_eff.normalize();
        d_r_eff.normalize();

        Eigen::Vector3d cross = d_s_eff.cross(d_r_eff);
        double dot = d_s_eff.dot(d_r_eff);
        if (dot > 1.0) dot = 1.0;
        if (dot < -1.0) dot = -1.0;
        double sign = k.dot(cross) >= 0.0 ? 1.0 : -1.0;
        double phi = sign * std::acos(dot) * wavelength / (2.0 * Constants::PI);

        log_manager_.getLogger("ppp_model")->trace("Phase wind-up for {}: {} m", satellite, phi);
        return phi;
    }

    double PppModel::getBiasCorrection(const std::string& satellite,
        const std::string& freq_type,
        const GnssTime& time) const {
        // Validate inputs
        if (satellite.empty() || freq_type.empty()) {
            log_manager_.getLogger("ppp_model")->warn("Invalid inputs for bias correction: satellite='{}', freq_type='{}' at TOW {:.3f}",
                satellite, freq_type, time.tow);
            return 0.0;
        }
        if (time.tow < 0.0 || time.week < 0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid time for bias correction for satellite {}: week {}, TOW {:.3f}",
                satellite, time.week, time.tow);
            return 0.0;
        }

        // Find bias data
        auto it = std::find_if(biases_.begin(), biases_.end(),
            [&satellite, &freq_type, &time](const BiasData& b) {
                return b.satellite == satellite && b.signal == freq_type &&
                    b.start_time.tow <= time.tow && time.tow <= b.end_time.tow;
            });
        if (it != biases_.end()) {
            log_manager_.getLogger("ppp_model")->trace("Bias correction for {} ({}) {} at TOW {:.3f}: {:.3e} m",
                satellite, satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown",
                freq_type, time.tow, it->bias);
            return it->bias;
        }

        log_manager_.getLogger("ppp_model")->trace("No bias correction for {} ({}) {} at TOW {:.3f}",
            satellite, satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown",
            freq_type, time.tow);
        return 0.0;
    }

    Eigen::Vector3d PppModel::computePoleTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const {
        // Validate inputs
        if (!rx_pos.allFinite() || rx_pos.norm() < 1e3) {
            log_manager_.getLogger("ppp_model")->warn("Invalid receiver position for pole tide correction at TOW {:.3f}: rx_pos=({:.3f}, {:.3f}, {:.3f}), norm={:.3e}",
                time.tow, rx_pos.x(), rx_pos.y(), rx_pos.z(), rx_pos.norm());
            return Eigen::Vector3d::Zero();
        }
        if (time.tow < 0.0 || time.week < 0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid time for pole tide correction: week {}, TOW {:.3f}",
                time.week, time.tow);
            return Eigen::Vector3d::Zero();
        }
        if (erps_.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No ERP data available for pole tide correction at TOW {:.3f}", time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Find closest ERP data
        auto it = std::min_element(erps_.begin(), erps_.end(),
            [&time](const ErpData& a, const ErpData& b) {
                return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
            });
        if (it == erps_.end() || !std::isfinite(it->xp) || !std::isfinite(it->yp)) {
            log_manager_.getLogger("ppp_model")->trace("No valid ERP data for pole tide correction at TOW {:.3f}", time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Compute pole tide correction (simplified model, IERS 2010 conventions)
        double phi = std::asin(rx_pos.z() / rx_pos.norm()); // Geodetic latitude (radians)
        double lambda = std::atan2(rx_pos.y(), rx_pos.x()); // Geodetic longitude (radians)
        double xp = it->xp * 4.848136811e-6; // Convert arcsec to radians
        double yp = it->yp * 4.848136811e-6; // Convert arcsec to radians

        // Pole tide displacement in radial, north, east directions (meters)
        double R = rx_pos.norm();
        double radial = -0.033 * std::sin(2.0 * phi) * (xp * std::cos(lambda) + yp * std::sin(lambda));
        double north = -0.009 * std::cos(2.0 * phi) * (xp * std::sin(lambda) - yp * std::cos(lambda));
        double east = -0.009 * std::sin(phi) * (xp * std::cos(lambda) + yp * std::sin(lambda));

        // Convert to ECEF coordinates
        Eigen::Vector3d correction(
            (radial * std::cos(phi) * std::cos(lambda) - north * std::sin(phi) * std::cos(lambda) - east * std::sin(lambda)),
            (radial * std::cos(phi) * std::sin(lambda) - north * std::sin(phi) * std::sin(lambda) + east * std::cos(lambda)),
            (radial * std::sin(phi) + north * std::cos(phi))
        );

        if (!correction.allFinite() || correction.norm() > 0.1) { // Max 10 cm
            log_manager_.getLogger("ppp_model")->warn("Invalid pole tide correction at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, norm={:.3e}",
                time.tow, correction.x(), correction.y(), correction.z(), correction.norm());
            return Eigen::Vector3d::Zero();
        }

        log_manager_.getLogger("ppp_model")->trace("Pole tide correction at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, xp={:.3e} rad, yp={:.3e} rad",
            time.tow, correction.x(), correction.y(), correction.z(), xp, yp);
        return correction;
    }

    Eigen::Vector3d PppModel::computeOceanTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const {
        // Validate inputs
        if (!rx_pos.allFinite() || rx_pos.norm() < 1e3) {
            log_manager_.getLogger("ppp_model")->warn("Invalid receiver position for ocean tide correction at TOW {:.3f}: rx_pos=({:.3f}, {:.3f}, {:.3f}), norm={:.3e}",
                time.tow, rx_pos.x(), rx_pos.y(), rx_pos.z(), rx_pos.norm());
            return Eigen::Vector3d::Zero();
        }
        if (time.tow < 0.0 || time.week < 0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid time for ocean tide correction: week {}, TOW {:.3f}",
                time.week, time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Placeholder: Simplified ocean tide correction (requires external tidal model data)
        // Using a basic sinusoidal model based on position and time
        double phi = std::asin(rx_pos.z() / rx_pos.norm()); // Geodetic latitude (radians)
        double t = std::fmod(time.tow, 86400.0); // Seconds of day
        double amplitude = 0.01; // Typical ocean tide amplitude: 1 cm
        double period = 12.4206 * 3600.0; // M2 tidal constituent period (s)
        double radial = amplitude * std::cos(2.0 * Constants::PI * t / period) * std::cos(phi);
        double north = amplitude * std::sin(2.0 * Constants::PI * t / period) * std::sin(phi);
        double east = 0.0; // Simplified, no east component

        // Convert to ECEF coordinates
        double lambda = std::atan2(rx_pos.y(), rx_pos.x());
        Eigen::Vector3d correction(
            (radial * std::cos(phi) * std::cos(lambda) - north * std::sin(phi) * std::cos(lambda)),
            (radial * std::cos(phi) * std::sin(lambda) - north * std::sin(phi) * std::sin(lambda)),
            (radial * std::sin(phi) + north * std::cos(phi))
        );

        if (!correction.allFinite() || correction.norm() > 0.1) { // Max 10 cm
            log_manager_.getLogger("ppp_model")->warn("Invalid ocean tide correction at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, norm={:.3e}",
                time.tow, correction.x(), correction.y(), correction.z(), correction.norm());
            return Eigen::Vector3d::Zero();
        }

        log_manager_.getLogger("ppp_model")->trace("Ocean tide correction at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m, amplitude={:.3e} m",
            time.tow, correction.x(), correction.y(), correction.z(), amplitude);
        return correction;
    }
    double PppModel::getFrequency(const std::string& freq_type, System system, const std::string& satellite) const {
        if (system == System::GPS) {
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                return Constants::GPS_L1_FREQ;
            }
            if (freq_type.find("C2") != std::string::npos || freq_type.find("L2") != std::string::npos) {
                return Constants::GPS_L2_FREQ;
            }
            if (freq_type.find("C5") != std::string::npos || freq_type.find("L5") != std::string::npos) {
                return Constants::GPS_L5_FREQ;
            }
        }
        else if (system == System::Galileo) {
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                return Constants::GAL_E1_FREQ;
            }
            if (freq_type.find("C5") != std::string::npos || freq_type.find("L5") != std::string::npos) {
                return Constants::GAL_E5A_FREQ;
            }
            if (freq_type.find("C7") != std::string::npos || freq_type.find("L7") != std::string::npos) {
                return Constants::GAL_E5B_FREQ;
            }
        }
        else if (system == System::GLONASS) {
            // Return reference frequency for rescaled phase observations
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                log_manager_.getLogger("ppp_model")->trace("Using GLONASS G1 reference frequency: {} MHz for type {}",
                    Constants::GLONASS_G1_BASE_FREQ / 1e6, freq_type);
                return Constants::GLONASS_G1_BASE_FREQ; // 1602 MHz
            }
            if (freq_type.find("C2") != std::string::npos || freq_type.find("L2") != std::string::npos) {
                log_manager_.getLogger("ppp_model")->trace("Using GLONASS G2 reference frequency: {} MHz for type {}",
                    Constants::GLONASS_G2_BASE_FREQ / 1e6, freq_type);
                return Constants::GLONASS_G2_BASE_FREQ; // 1246 MHz
            }
            log_manager_.getLogger("ppp_model")->warn("Invalid GLONASS frequency type: {}", freq_type);
            return 0.0;
        }
        else if (system == System::BeiDou) {
            if (freq_type.find("C2") != std::string::npos || freq_type.find("L2") != std::string::npos) {
                return Constants::BDS_B1_FREQ;
            }
            if (freq_type.find("C7") != std::string::npos || freq_type.find("L7") != std::string::npos) {
                return Constants::BDS_B2_FREQ;
            }
            if (freq_type.find("C6") != std::string::npos || freq_type.find("L6") != std::string::npos) {
                return Constants::BDS_B3_FREQ;
            }
        }
        else if (system == System::QZSS) {
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                return Constants::QZSS_L1_FREQ;
            }
            if (freq_type.find("C2") != std::string::npos || freq_type.find("L2") != std::string::npos) {
                return Constants::QZSS_L2_FREQ;
            }
            if (freq_type.find("C5") != std::string::npos || freq_type.find("L5") != std::string::npos) {
                return Constants::QZSS_L5_FREQ;
            }
        }
        else if (system == System::SBAS) {
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                return Constants::SBAS_L1_FREQ;
            }
            if (freq_type.find("C5") != std::string::npos || freq_type.find("L5") != std::string::npos) {
                return Constants::SBAS_L5_FREQ;
            }
        }

        log_manager_.getLogger("ppp_model")->warn("Unknown frequency for {} on system {}", freq_type, static_cast<int>(system));
        return 0.0;
    }

    double PppModel::getReferenceFrequency(const std::string& freq_type, System system) const {
        // Validate input
        if (freq_type.empty()) {
            log_manager_.getLogger("ppp_model")->warn("Empty frequency type for reference frequency, system={}",
                MeasurementModel::getSystemString(system));
            return 0.0;
        }

        if (system == System::GLONASS) {
            if (freq_type.find("C1") != std::string::npos || freq_type.find("L1") != std::string::npos) {
                return Constants::GLONASS_G1_BASE_FREQ; // Center frequency: 1602 MHz
            }
            if (freq_type.find("C2") != std::string::npos || freq_type.find("L2") != std::string::npos) {
                return Constants::GLONASS_G2_BASE_FREQ; // Center frequency: 1246 MHz
            }
            if (freq_type.find("C3") != std::string::npos || freq_type.find("L3") != std::string::npos) {
                return Constants::GLONASS_G3_BASE_FREQ; // Center frequency: 1202.025 MHz
            }
        }

        // For non-GLONASS systems, return actual frequency (no rescaling needed)
        double freq = getFrequency(freq_type, system, "");
        if (freq <= 0.0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid reference frequency for type {} on system {}: {:.3f} MHz",
                freq_type, MeasurementModel::getSystemString(system), freq / 1e6);
        }
        return freq;
    }

    double PppModel::rescaleGlonassObservation(double observed_data, const std::string& freq_type,
        System system, const std::string& satellite) const {
        if (system != System::GLONASS) {
            return observed_data; // No rescaling for non-GLONASS systems
        }

        // Validate inputs
        if (freq_type.empty() || satellite.empty()) {
            log_manager_.getLogger("ppp_model")->warn("Invalid inputs for GLONASS rescaling: freq_type='{}', satellite='{}'",
                freq_type, satellite);
            return observed_data;
        }

        double actual_freq = getFrequency(freq_type, system, satellite);
        double ref_freq = getReferenceFrequency(freq_type, system);
        if (actual_freq <= 0.0 || ref_freq <= 0.0 || !std::isfinite(actual_freq) || !std::isfinite(ref_freq)) {
            log_manager_.getLogger("ppp_model")->warn("Cannot rescale GLONASS observation for type {}, satellite {}: actual_freq={:.3f} MHz, ref_freq={:.3f} MHz",
                freq_type, satellite, actual_freq / 1e6, ref_freq / 1e6);
            return observed_data;
        }

        // Rescale phase: L_new = L_old * (f_actual / f_ref)
        double scaled_data = observed_data * (actual_freq / ref_freq);
        log_manager_.getLogger("ppp_model")->trace("Rescaled GLONASS observation for type {}, satellite {}: from {:.3f} to {:.3f} cycles, actual_freq={:.3f} MHz, ref_freq={:.3f} MHz",
            freq_type, satellite, observed_data, scaled_data, actual_freq / 1e6, ref_freq / 1e6);
        return scaled_data;
    }

    std::vector<std::string> PppModel::selectObservationTypes(const std::map<std::string, double>& measurements,
        System system,
        const std::string& system_str) const {
        std::vector<std::string> selected_types;
        auto it = signal_priorities_.find(system_str);
        if (it == signal_priorities_.end()) {
            return selected_types;
        }

        if (system == System::GLONASS) {
            // GLONASS: Handle channel priorities
            for (const auto& signal : it->second) {
                // Check for standard observation types (e.g., "C1C", "L1C")
                if (measurements.count(signal)) {
                    selected_types.push_back(signal);
                }
                // Check for channel-specific types (e.g., "C1C_k0", "C1C_m1")
                for (int channel = -7; channel <= 7; ++channel) { // Typical GLONASS channel range
                    std::string type = signal + "_k" + (channel >= 0 ? std::to_string(channel) : "m" + std::to_string(-channel));
                    if (measurements.count(type)) {
                        selected_types.push_back(type);
                    }
                }
            }
        }
        else {
            // Other systems: Use priority list directly
            for (const auto& type : it->second) {
                if (measurements.count(type)) {
                    selected_types.push_back(type);
                }
            }
        }
        return selected_types;
    }

    double PppModel::computeSagnacCorrection(const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos) const {
        // Sagnac effect: Correction for Earth's rotation during signal propagation
        // Formula: (ω_E / c) * (x_s * y_r - y_s * x_r), where ω_E is Earth's angular velocity, c is speed of light
        constexpr double EARTH_ANGULAR_VELOCITY = 7.2921151467e-5; // rad/s
        constexpr double C = Constants::SPEED_OF_LIGHT; // m/s
        constexpr double MAX_SAGNAC_CORRECTION = 50.0; // Max expected correction in meters
        constexpr double MIN_RX_NORM = 6e6; // Earth radius ~6.3e6 m
        constexpr double MAX_RX_NORM = 7e6;
        constexpr double MIN_SAT_NORM = 2e7; // Typical GNSS orbit ~2e7 m
        constexpr double MAX_SAT_NORM = 3e7;

        // Validate inputs
        if (!rx_pos.allFinite() || !sat_pos.allFinite()) {
            log_manager_.getLogger("ppp_model")->warn("Invalid Sagnac inputs: rx_pos=({}, {}, {}), sat_pos=({}, {}, {})",
                rx_pos.x(), rx_pos.y(), rx_pos.z(),
                sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }
        double rx_norm = rx_pos.norm();
        double sat_norm = sat_pos.norm();
        if (rx_norm < MIN_RX_NORM || rx_norm > MAX_RX_NORM || sat_norm < MIN_SAT_NORM || sat_norm > MAX_SAT_NORM) {
            log_manager_.getLogger("ppp_model")->warn("Unrealistic Sagnac input norms: rx_norm={:.3e} m, sat_norm={:.3e} m, "
                "rx_pos=({}, {}, {}), sat_pos=({}, {}, {})",
                rx_norm, sat_norm,
                rx_pos.x(), rx_pos.y(), rx_pos.z(),
                sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }

        // Compute correction: (ω_E / c) * (x_s * y_r - y_s * x_r)
        double cross_term = sat_pos.x() * rx_pos.y() - sat_pos.y() * rx_pos.x();
        double correction = (EARTH_ANGULAR_VELOCITY / C) * cross_term;
        if (!std::isfinite(correction) || std::abs(correction) > MAX_SAGNAC_CORRECTION) {
            log_manager_.getLogger("ppp_model")->warn("Excessive Sagnac correction: {:.3e} m, cross_term: {:.3e}, "
                "rx_pos=({}, {}, {}), sat_pos=({}, {}, {})",
                correction, cross_term,
                rx_pos.x(), rx_pos.y(), rx_pos.z(),
                sat_pos.x(), sat_pos.y(), sat_pos.z());
            return 0.0;
        }

        log_manager_.getLogger("ppp_model")->trace("Sagnac correction: {:.3f} m, cross_term: {:.3e}, "
            "rx_pos=({}, {}, {}), sat_pos=({}, {}, {})",
            correction, cross_term,
            rx_pos.x(), rx_pos.y(), rx_pos.z(),
            sat_pos.x(), sat_pos.y(), sat_pos.z());
        return correction;
    }

    double PppModel::computeAtmosphericLoadingCorrection(const Eigen::Vector3d& rx_pos,
        const GnssTime& time) const {
        // Placeholder: Simplified atmospheric loading correction
        // Model: Small sinusoidal variation based on time and position latitude
        // Note: Full implementation requires external loading models (e.g., IERS grids)
        double latitude = std::asin(rx_pos.z() / rx_pos.norm()) * 180.0 / Constants::PI; // Approx. latitude (degrees)
        double t = fmod(time.tow, 86400.0); // Seconds of day
        double amplitude = 0.001; // 1 mm amplitude (typical)
        double correction = amplitude * std::cos(2.0 * Constants::PI * t / 86400.0) * std::cos(latitude * Constants::PI / 180.0);
        log_manager_.getLogger("ppp_model")->trace("Atmospheric loading correction: {} m", correction);
        return correction;
    }

    const RinexParser& PppModel::getRinexParser() const {
        return rinex_parser_;
    }

    std::string PppModel::getReceiverAntennaType() const {
        std::string antenna_type = rinex_parser_.getReceiverAntennaType();
        if (antenna_type.empty()) {
            log_manager_.getLogger("ppp_model")->warn("Empty receiver antenna type returned from RINEX parser");
        }
        return antenna_type;
    }

    void PppModel::computeSatelliteAttitude(System system,
        const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel,
        Eigen::Vector3d& d_s,
        Eigen::Vector3d& y_s,
        const GnssTime& time) const {
        // Get Sun position
        Eigen::Vector3d sun_pos = interpolateSunPosition(time);
        if (sun_pos.norm() < 1e6) { // Fallback if no ephemeris
            sun_pos = Eigen::Vector3d(1.496e11, 0.0, 0.0); // 1 AU along x-axis
            log_manager_.getLogger("ppp_model")->warn("No Sun ephemeris for time TOW {0}", time.tow);
        }
        Eigen::Vector3d e = (sun_pos - sat_pos).normalized(); // Sun direction

        // Compute yaw-steering attitude
        Eigen::Vector3d y = sat_vel.normalized().cross(e);
        if (y.norm() < 1e-6) { // Handle collinearity
            y = Eigen::Vector3d(0.0, 1.0, 0.0); // Arbitrary fallback
        }
        y_s = y.normalized();
        Eigen::Vector3d z_s = -sat_pos.normalized();
        d_s = y_s.cross(z_s).normalized();
    }

    Eigen::Vector3d PppModel::getSatelliteVelocity(const std::string& satellite, const GnssTime& time) const {
        constexpr int velocity_order = 6; // Interpolation order for velocity
        constexpr double MIN_POSITION_NORM = 1.8e7; // ~20,000 km
        constexpr double MAX_POSITION_NORM = 3.2e7; // ~32,000 km
        constexpr double MAX_VELOCITY_NORM = 1e4; // ~10 km/s
        constexpr double MIN_TIME_DIFF = 1.0; // Minimum time difference (s)
        constexpr double MAX_TIME_WINDOW = 3600.0; // Maximum time window (s)

        // Validate inputs
        if (satellite.empty()) {
            log_manager_.getLogger("ppp_model")->warn("Empty satellite ID for velocity interpolation at TOW {:.3f}", time.tow);
            return Eigen::Vector3d::Zero();
        }
        if (orbits_.empty()) {
            log_manager_.getLogger("ppp_model")->warn("No orbit data available for {} at TOW {:.3f}", satellite, time.tow);
            return Eigen::Vector3d::Zero();
        }
        if (time.tow < 0.0 || time.week < 0) {
            log_manager_.getLogger("ppp_model")->warn("Invalid time for velocity interpolation for {}: week {}, TOW {:.3f}",
                satellite, time.week, time.tow);
            return Eigen::Vector3d::Zero();
        }

        // Check cached velocity
        auto vel_it = last_interpolated_velocity_.find(satellite);
        if (vel_it != last_interpolated_velocity_.end() && vel_it->second.allFinite() &&
            vel_it->second.norm() <= MAX_VELOCITY_NORM) {
            log_manager_.getLogger("ppp_model")->trace("Using cached velocity for {} at TOW {:.3f}: ({:.3f}, {:.3f}, {:.3f}) m/s, norm: {:.3e}",
                satellite, time.tow, vel_it->second.x(), vel_it->second.y(), vel_it->second.z(), vel_it->second.norm());
            return vel_it->second;
        }

        // Collect candidates
        std::vector<OrbitData> candidates;
        std::string system_str = satellite.substr(0, 1) == "G" ? "GPS" : satellite.substr(0, 1) == "E" ? "Galileo" : "Unknown";
        for (const auto& data : orbits_) {
            if (data.satellite == satellite && data.position_valid &&
                data.position.allFinite() && data.position.norm() >= MIN_POSITION_NORM &&
                data.position.norm() <= MAX_POSITION_NORM) {
                double time_diff = std::abs(data.time.tow - time.tow);
                if (time_diff < MAX_TIME_WINDOW) {
                    candidates.push_back(data);
                }
            }
        }

        // Check if sufficient data
        if (candidates.size() < velocity_order) {
            log_manager_.getLogger("ppp_model")->debug("Insufficient orbit data for velocity of {} ({}) at TOW {:.3f}: found {} points, need {}",
                satellite, system_str, time.tow, candidates.size(), velocity_order);
            if (!candidates.empty()) {
                auto it = std::min_element(candidates.begin(), candidates.end(),
                    [&time](const OrbitData& a, const OrbitData& b) {
                        return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
                    });
                if (it->velocity_valid && it->velocity.allFinite() && it->velocity.norm() <= MAX_VELOCITY_NORM) {
                    last_interpolated_velocity_[satellite] = it->velocity;
                    log_manager_.getLogger("ppp_model")->trace("Using nearest velocity for {} ({}) at TOW {:.3f}: ({:.3f}, {:.3f}, {:.3f}) m/s, norm: {:.3e}",
                        satellite, system_str, it->time.tow, it->velocity.x(), it->velocity.y(), it->velocity.z(), it->velocity.norm());
                    return it->velocity;
                }
            }
            log_manager_.getLogger("ppp_model")->warn("No valid velocity data for {} ({}) at TOW {:.3f}", satellite, system_str, time.tow);
            last_interpolated_velocity_.erase(satellite);
            return Eigen::Vector3d::Zero();
        }

        // Sort candidates by time proximity
        std::sort(candidates.begin(), candidates.end(),
            [&time](const OrbitData& a, const OrbitData& b) {
                return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
            });
        candidates.resize(velocity_order);

        // Check time separation
        for (size_t i = 0; i < candidates.size(); ++i) {
            for (size_t j = i + 1; j < candidates.size(); ++j) {
                if (std::abs(candidates[i].time.tow - candidates[j].time.tow) < MIN_TIME_DIFF) {
                    log_manager_.getLogger("ppp_model")->warn("Time difference too small for {} ({}) at TOW {:.3f}: {:.3f} s between epochs",
                        satellite, system_str, time.tow, std::abs(candidates[i].time.tow - candidates[j].time.tow));
                    last_interpolated_velocity_.erase(satellite);
                    return Eigen::Vector3d::Zero();
                }
            }
        }

        // Compute velocity using central difference
        Eigen::Vector3d interpolated_vel(0.0, 0.0, 0.0);
        bool velocity_computed = false;
        for (size_t i = 2; i < velocity_order - 2; ++i) {
            double t_prev = candidates[i - 1].time.tow;
            double t_next = candidates[i + 1].time.tow;
            double dt = t_next - t_prev;
            if (dt >= MIN_TIME_DIFF) {
                interpolated_vel += (candidates[i + 1].position - candidates[i - 1].position) / dt;
                velocity_computed = true;
            }
        }
        if (velocity_computed) {
            interpolated_vel /= (velocity_order - 4);
        }

        // Use SP3 velocity if available
        bool use_sp3_velocity = true;
        for (size_t i = 0; i < velocity_order; ++i) {
            if (!candidates[i].velocity_valid || !candidates[i].velocity.allFinite() ||
                candidates[i].velocity.norm() > MAX_VELOCITY_NORM) {
                use_sp3_velocity = false;
                break;
            }
        }
        if (use_sp3_velocity) {
            interpolated_vel = Eigen::Vector3d(0.0, 0.0, 0.0);
            double t = time.tow;
            for (size_t i = 0; i < velocity_order; ++i) {
                double li = 1.0;
                for (size_t j = 0; j < velocity_order; ++j) {
                    if (i != j) {
                        li *= (t - candidates[j].time.tow) / (candidates[i].time.tow - candidates[j].time.tow);
                    }
                }
                interpolated_vel += li * candidates[i].velocity;
            }
            velocity_computed = true;
        }

        // Validate interpolated velocity
        if (!velocity_computed || !interpolated_vel.allFinite() || interpolated_vel.norm() > MAX_VELOCITY_NORM) {
            log_manager_.getLogger("ppp_model")->warn("Invalid interpolated velocity for {} ({}) at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e}) m/s, norm: {:.3e}",
                satellite, system_str, time.tow, interpolated_vel.x(), interpolated_vel.y(), interpolated_vel.z(), interpolated_vel.norm());
            last_interpolated_velocity_.erase(satellite);
            return Eigen::Vector3d::Zero();
        }

        // Cache and return
        last_interpolated_velocity_[satellite] = interpolated_vel;
        log_manager_.getLogger("ppp_model")->trace("Interpolated velocity for {} ({}) at TOW {:.3f}: ({:.3f}, {:.3f}, {:.3f}) m/s, norm: {:.3e}",
            satellite, system_str, time.tow, interpolated_vel.x(), interpolated_vel.y(), interpolated_vel.z(), interpolated_vel.norm());
        return interpolated_vel;
    }

    Eigen::Vector3d PppModel::interpolateSunPosition(const GnssTime& time) const {
        if (sun_ephemeris_.empty()) {
            return Eigen::Vector3d(0.0, 0.0, 0.0);
        }

        // Find 2 closest epochs for linear interpolation
        std::vector<SunEphemeris> candidates;
        for (const auto& data : sun_ephemeris_) {
            double time_diff = std::abs(data.time.tow - time.tow);
            if (time_diff < 86400.0) { // Within 1 day
                candidates.push_back(data);
            }
        }

        if (candidates.size() < 2) {
            if (!candidates.empty()) {
                return candidates[0].position;
            }
            return Eigen::Vector3d(0.0, 0.0, 0.0);
        }

        std::sort(candidates.begin(), candidates.end(),
            [&time](const SunEphemeris& a, const SunEphemeris& b) {
                return std::abs(a.time.tow - time.tow) < std::abs(b.time.tow - time.tow);
            });
        candidates.resize(2);

        double t = time.tow;
        double t0 = candidates[0].time.tow;
        double t1 = candidates[1].time.tow;
        Eigen::Vector3d p0 = candidates[0].position;
        Eigen::Vector3d p1 = candidates[1].position;

        if (t0 == t1) {
            return p0;
        }

        double frac = (t - t0) / (t1 - t0);
        return p0 + frac * (p1 - p0);
    }

    double PppModel::computeInitialIonoDelay(const GnssTime& time, const Eigen::Vector3d& rx_pos) const {
        if (klobuchar_coeffs_.size() != 8) {
            return 5.0; // Default fallback
        }

        // Klobuchar model for zenith delay at receiver position
        double a0 = klobuchar_coeffs_[0], a1 = klobuchar_coeffs_[1], a2 = klobuchar_coeffs_[2], a3 = klobuchar_coeffs_[3];
        double b0 = klobuchar_coeffs_[4], b1 = klobuchar_coeffs_[5], b2 = klobuchar_coeffs_[6], b3 = klobuchar_coeffs_[7];

        double t = fmod(time.tow, 86400.0);
        double phi = std::asin(rx_pos.z() / rx_pos.norm()) * 180.0 / Constants::PI; // Geodetic latitude (degrees)
        double lambda = std::atan2(rx_pos.y(), rx_pos.x()) * 180.0 / Constants::PI; // Geodetic longitude (degrees)

        double phi_i = phi / 180.0; // Normalized latitude
        double lambda_i = lambda / 180.0; // Normalized longitude
        double phi_m = phi_i + 0.064 * std::cos((lambda_i - 1.617) * Constants::PI);

        double t_i = 43200.0 * lambda_i + t;
        t_i = fmod(t_i, 86400.0);
        if (t_i < 0) t_i += 86400.0;

        double AMP = a0 + a1 * phi_m + a2 * std::pow(phi_m, 2) + a3 * std::pow(phi_m, 3);
        if (AMP < 0) AMP = 0;
        double PER = b0 + b1 * phi_m + b2 * std::pow(phi_m, 2) + b3 * std::pow(phi_m, 3);
        if (PER < 72000) PER = 72000;
        double X = 2.0 * Constants::PI * (t_i - 50400.0) / PER;

        double delay;
        if (std::abs(X) < 1.57) {
            delay = 5e-9 + AMP * (1 - X * X / 2 + X * X * X * X / 24);
        }
        else {
            delay = 5e-9;
        }

        // Convert to meters (zenith delay at L1 frequency)
        delay *= Constants::SPEED_OF_LIGHT;
        return delay;
    }

    double PppModel::computeMultipathVarianceWeight(const PppObservation& obs, const std::string& type, double elevation) const {
        // Validate inputs
        if (type.empty() || obs.satellite.empty()) {
            log_manager_.getLogger("ppp_model")->warn("Invalid inputs for multipath weighting: type='{}', satellite='{}'", type, obs.satellite);
            return 1.0;
        }
        if (elevation < 0.0 || elevation > 90.0 || !std::isfinite(elevation)) {
            log_manager_.getLogger("ppp_model")->warn("Invalid elevation for multipath weighting for {} {}: {:.3f} deg",
                obs.satellite, type, elevation);
            return 1.0;
        }

        // Elevation-based weighting
        double elev_weight = std::sin(elevation * Constants::PI / 180.0);
        elev_weight = std::max(MultipathHistory::ELEVATION_MIN_WEIGHT, elev_weight); // Ensure minimum weight

        // Only apply code-minus-carrier analysis for code observations
        if (type[0] != 'C') {
            return 1.0; // No multipath adjustment for phase or other observations
        }

        // Map code to corresponding phase (e.g., C1C -> L1C)
        std::string phase_type = "L" + type.substr(1);
        if (!obs.observed.count(phase_type)) {
            log_manager_.getLogger("ppp_model")->debug("No phase data for {} {} (phase_type={}), using elevation weight: {:.3f}",
                obs.satellite, type, phase_type, elev_weight);
            return elev_weight;
        }

        // Compute CMC residual: P - λ * φ
        double code = obs.observed.at(type);
        double phase = obs.observed.at(phase_type);
        double wavelength = getWavelength(obs.system, phase_type);
        if (wavelength <= 0.0 || !std::isfinite(wavelength)) {
            log_manager_.getLogger("ppp_model")->warn("Invalid wavelength for {} {} (phase_type={}): {:.3f} m, using elevation weight: {:.3f}",
                obs.satellite, type, phase_type, wavelength, elev_weight);
            return elev_weight;
        }
        double cmc = code - wavelength * phase;
        if (!std::isfinite(cmc)) {
            log_manager_.getLogger("ppp_model")->warn("Non-finite CMC for {} {}: code={:.3f}, phase={:.3f}, wavelength={:.3f}, using elevation weight: {:.3f}",
                obs.satellite, type, code, phase, wavelength, elev_weight);
            return elev_weight;
        }

        // Update multipath history
        std::string key = obs.satellite + "_" + type;
        auto& history = const_cast<std::map<std::string, MultipathHistory>&>(multipath_histories_)[key];
        history.cmc_values.push_back(cmc);
        history.timestamps.push_back(obs.time);
        if (history.cmc_values.size() > MultipathHistory::WINDOW_SIZE) {
            history.cmc_values.pop_front();
            history.timestamps.pop_front();
        }

        // Check time continuity and compute statistics
        if (history.cmc_values.size() == MultipathHistory::WINDOW_SIZE) {
            for (size_t i = 1; i < history.timestamps.size(); ++i) {
                double dt = history.timestamps[i].tow - history.timestamps[i - 1].tow;
                if (dt <= 0.0 || dt > MultipathHistory::TIME_GAP_THRESHOLD) {
                    log_manager_.getLogger("ppp_model")->debug("Invalid time gap for {} {}: dt={:.3f} s, using elevation weight: {:.3f}",
                        obs.satellite, type, dt, elev_weight);
                    return elev_weight;
                }
            }

            // Compute mean and standard deviation of CMC values
            double sum = 0.0, sum_sq = 0.0;
            size_t n = history.cmc_values.size();
            for (double value : history.cmc_values) {
                sum += value;
                sum_sq += value * value;
            }
            double mean = sum / n;
            double variance = n > 1 ? (sum_sq / n - mean * mean) * n / (n - 1) : 0.0;
            double std_dev = variance > 0.0 ? std::sqrt(variance) : 1e-6;

            // Check CMC residual for multipath
            double cmc_residual = std::abs(cmc - mean);
            if (cmc_residual > MultipathHistory::MULTIPATH_CMC_THRESHOLD * std_dev) {
                elev_weight *= 0.5; // Down-weight by 50% if multipath detected
                log_manager_.getLogger("ppp_model")->trace("Multipath detected for {} {}: CMC residual={:.3f} m, threshold={:.3f} m, adjusted weight: {:.3f}",
                    obs.satellite, type, cmc_residual, MultipathHistory::MULTIPATH_CMC_THRESHOLD * std_dev, elev_weight);
            }
        }

        log_manager_.getLogger("ppp_model")->trace("Multipath variance weight for {} {}: {:.3f}, elevation={:.3f} deg, CMC={:.3f} m",
            obs.satellite, type, elev_weight, elevation, cmc);
        return elev_weight;
    }

    bool PppModel::parseSunEphemeris(const std::vector<GnssTime>& epoch_times, std::vector<SunEphemeris>& sun_ephemeris) const {
        try {
            sun_ephemeris.clear();
            if (epoch_times.empty()) {
                log_manager_.getLogger("ppp_model")->error("No epoch times provided for Sun ephemeris computation");
                return false;
            }

            // Constants for Sun position (JPL low-precision ephemeris approximation)
            const double AU = 1.495978707e11; // Astronomical unit in meters
            const double epsilon = 23.439281 * Constants::PI / 180.0; // Obliquity of ecliptic (radians)
            const double T_0 = 2451545.0; // J2000 epoch (JD)
            const double M_0 = 357.5291 * Constants::PI / 180.0; // Mean anomaly at J2000 (radians)
            const double n = 0.98560028 * Constants::PI / 180.0 / 86400.0; // Mean motion (rad/s)

            for (const auto& epoch_time : epoch_times) {
                // Validate epoch time
                if (epoch_time.week < 0 || epoch_time.tow < 0 || epoch_time.tow >= 604800.0) {
                    log_manager_.getLogger("ppp_model")->warn("Invalid epoch time: week {}, tow {:.3f}", epoch_time.week, epoch_time.tow);
                    continue;
                }

                // Convert GPS time to Julian Date
                double gps_t = TimeUtils::gpsTimeToJulianDate(epoch_time.week, epoch_time.tow);
                double T = (gps_t - T_0) / 36525.0; // Julian centuries since J2000

                // Mean anomaly
                double M = M_0 + n * (gps_t - T_0) * 86400.0;
                M = std::fmod(M, 2.0 * Constants::PI); // Normalize to [0, 2π]

                // Approximate eccentric anomaly (simplified, low eccentricity ~0.0167)
                double E = M; // First-order approximation
                double cosE = std::cos(E);
                double sinE = std::sin(E);

                // Heliocentric coordinates in ecliptic plane
                double x_helio = AU * (cosE - 0.0167);
                double y_helio = AU * std::sqrt(1.0 - 0.0167 * 0.0167) * sinE;
                double z_helio = 0.0; // Assume Sun in ecliptic plane

                // Transform to ECEF (simplified, apply obliquity rotation)
                double x_ecef = x_helio;
                double y_ecef = y_helio * std::cos(epsilon);
                double z_ecef = y_helio * std::sin(epsilon);

                SunEphemeris data;
                data.time = epoch_time;
                data.position = Eigen::Vector3d(x_ecef, y_ecef, z_ecef);
                if (!data.position.allFinite()) {
                    log_manager_.getLogger("ppp_model")->warn("Non-finite Sun position at TOW {:.3f}: ({:.3e}, {:.3e}, {:.3e})",
                        epoch_time.tow, x_ecef, y_ecef, z_ecef);
                    continue;
                }
                sun_ephemeris.push_back(data);
            }

            if (sun_ephemeris.empty()) {
                log_manager_.getLogger("ppp_model")->error("No valid Sun ephemeris entries computed");
                return false;
            }
            log_manager_.getLogger("ppp_model")->info("Computed {} Sun ephemeris entries for observation epochs", sun_ephemeris.size());
            return true;
        }
        catch (const std::exception& e) {
            log_manager_.getLogger("ppp_model")->error("Exception computing Sun ephemeris: {}", e.what());
            sun_ephemeris.clear();
            return false;
        }
    }

} // namespace GnssProcessor
//#endif