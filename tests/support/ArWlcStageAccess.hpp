#pragma once
/**
 * @file ArWlcStageAccess.hpp
 * @brief Shared test seam for the stages of the WL-constrained user resolver
 *        (@c AmbiguityResolver::resolveWlConstrained, stage table @ref kWlcStages), and a small
 *        scene builder for them.
 *
 * The stages are private member functions.  @c ArWlcStageTestAccess is befriended by
 * AmbiguityResolver and defined here only, so every test file that includes this header
 * sees one definition (a type defined in two translation units must be token for token
 * identical).  @c gnss_test::ArStageRig builds a resolver whose @c ambiguity_resolver log is
 * captured at debug level; @c gnss_test::ArScene builds DD pairs, a float state and a
 * covariance from chosen integers, so every expected number is known by algebra.
 */

#include "common/Constants.hpp"
#include "filter/AmbiguityResolver.hpp"
#include "logging/LogManager.hpp"
#include "support/TestTempDir.hpp"

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace GnssProcessor {

/// Test seam for the stages of resolveWlConstrained (friend in AmbiguityResolver.hpp).
struct ArWlcStageTestAccess {
    using Pair  = AmbiguityResolver::DdPair;
    using Epoch = AmbiguityResolver::WlcEpoch;
    using Group = AmbiguityResolver::WlcGroup;
    using Path  = AmbiguityResolver::WlcFixPath;

    /// Stage S1.
    static void group(const AmbiguityResolver& r, const std::vector<Pair>& pairs,
                      const std::vector<PppObservation>& obs, Epoch& ep) {
        r.groupSatellitePairs_(pairs, obs, ep);
    }
    /// Stage S9.
    static void report(AmbiguityResolver& r, Path path, int n_nl, int n_fixed, double ratio,
                       double p_s, double max_frac, const QbCond& pre, const QbCond& post) {
        const Epoch ep;
        r.reportFix_(ep, path, n_nl, n_fixed, ratio, p_s, max_frac, pre, post);
    }
    /// The whole resolver body, as the existing seams call it.
    static AmbiguityResolution run(AmbiguityResolver& r, const std::vector<Pair>& pairs,
                                   const Eigen::VectorXd& x, const Eigen::MatrixXd& P,
                                   const std::vector<PppObservation>& obs) {
        return r.resolveWlConstrained(pairs, x, P, {}, obs);
    }
    /// Put one hold in the resolver's memory, as AmbiguityResolver::resolve does after a fix.
    static void hold(AmbiguityResolver& r, const AmbiguityResolution::HeldPairFix& h) {
        r.held_pairs_[h.sat_i + "|" + h.sat_ref + "|" + h.sig_hi + "|" + h.sig_lo] = h;
    }
    static std::size_t heldCount(const AmbiguityResolver& r) { return r.held_pairs_.size(); }
    static bool isHeld(const AmbiguityResolver& r, const std::string& key) {
        return r.held_pairs_.count(key) > 0;
    }
};

} // namespace GnssProcessor

namespace gnss_test {

/// A resolver whose ambiguity_resolver log is captured at debug level.
struct ArStageRig {
    GnssProcessor::LogManager log;
    std::ostringstream text;
    std::unique_ptr<GnssProcessor::AmbiguityResolver> resolver;

    explicit ArStageRig(const GnssProcessor::AmbiguityResolverConfig& cfg) {
        const std::filesystem::path dir = tempDir() / "ar_stage_logs";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        spdlog::drop("ambiguity_resolver");
        log.initialize(dir, GnssProcessor::LogLevel::Info);
        log.getLogger("ambiguity_resolver")->set_level(spdlog::level::debug);
        log.getLogger("ambiguity_resolver")->sinks().push_back(
            std::make_shared<spdlog::sinks::ostream_sink_mt>(text));
        resolver = std::make_unique<GnssProcessor::AmbiguityResolver>(cfg, log);
    }
    ~ArStageRig() { spdlog::drop("ambiguity_resolver"); }

    /// The captured text so far, every line ending in a newline.
    std::string logText() {
        log.getLogger("ambiguity_resolver")->flush();
        return text.str();
    }
};

/// GPS L1/L2 and Galileo E1/E5a carrier frequencies [Hz].
constexpr double kFL1 = 1575.42e6, kFL2 = 1227.60e6, kFL5 = 1176.45e6;

/// One satellite of a scene against the scene's reference: integer-plus-offset floats in cycles.
struct ArSceneSat {
    std::string sat;
    double n_hi;   ///< DD float on the higher signal [cycles].
    double n_lo;   ///< DD float on the lower signal [cycles].
};

/**
 * @brief DD pairs, state and covariance of one constellation on two signals against one
 *        reference.  States: the reference's two signals first, then each satellite's two.
 *        Values in metres, the reference at zero.
 */
struct ArScene {
    std::vector<GnssProcessor::ArWlcStageTestAccess::Pair> pairs;
    Eigen::VectorXd x;
    Eigen::MatrixXd P;
    std::vector<GnssProcessor::PppObservation> obs;

    ArScene(const std::string& ref, const std::vector<ArSceneSat>& sats,
            GnssProcessor::System system = GnssProcessor::System::GPS,
            const std::string& sig_hi = "L1C", const std::string& sig_lo = "L2W",
            double f_hi = kFL1, double f_lo = kFL2, double var_m2 = 1e-8, double tow = 345600.0) {
        using GnssProcessor::Constants::SPEED_OF_LIGHT;
        const int n = 2 * (1 + static_cast<int>(sats.size()));
        x = Eigen::VectorXd::Zero(n);
        P = Eigen::MatrixXd::Identity(n, n) * var_m2;
        const double lam_hi = SPEED_OF_LIGHT / f_hi, lam_lo = SPEED_OF_LIGHT / f_lo;
        for (int j = 0; j < static_cast<int>(sats.size()); ++j) {
            const int s_hi = 2 * (1 + j), s_lo = s_hi + 1;
            x(s_hi) = sats[j].n_hi * lam_hi;
            x(s_lo) = sats[j].n_lo * lam_lo;
            pairs.push_back({ s_hi, 0, sats[j].sat, ref, sig_hi });
            pairs.push_back({ s_lo, 1, sats[j].sat, ref, sig_lo });
            GnssProcessor::PppObservation o;
            o.satellite = sats[j].sat;
            o.system    = system;
            o.time.tow  = tow;
            obs.push_back(o);
        }
    }
};

} // namespace gnss_test
