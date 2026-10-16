#pragma once
/**
 * @file NetworkTestRun.hpp
 * @brief A network run and one epoch for unit tests: everything a per-epoch seam's epoch form
 *        `(NetworkRun&, const NetworkEpoch&[, kept...][, outputs...])` reads (card NOM3 step EP2;
 *        step HV1 builds on it).
 *
 * Card NOM3 step EP1 gave every per-epoch seam of the network producer one form over the run
 * (network/driver/Run.hpp) and the epoch (network/driver/Epoch.hpp).  A unit test that drives a seam
 * builds both here instead of keeping an explicit-parameter overload alive for it:
 *
 *   - the configuration is the test's own (copied in), and the run context is built from it, so
 *     every knob a seam reads (NetworkKnobs) is the loader's resolution of that configuration,
 *     exactly as in a run; a test that needs a knob arms it in the configuration;
 *   - the traces open no stream unless the test opens them (traces().openAll, in the
 *     directory the constructor names), so by default a seam writes no file;
 *   - the setup (stations, sync, emitter, products) is default-constructed and the test fills
 *     what its seam reads, `setup().stations` first among them;
 *   - the run owns its engines (est(), the slip-QC channel, the DD-carry driver), built from
 *     networkEngineSpec of the configuration, as runNetwork builds them;
 *   - the epoch is rebuilt by newEpoch(t): a fresh NetworkEpoch over a fresh SyncedEpoch whose
 *     time tag is @p t, every group empty, as at the epoch's construction in
 *     Application::networkProcessEpoch_.  The test writes the groups its seam reads (the
 *     driver's stage order is not enforced here) and passes them as the seam's outputs.
 *
 * Usage:
 * @code
 *   NetworkTestRun fx(cfg);
 *   fx.setup().stations = makeStations();
 *   NetworkEpoch& e = fx.newEpoch(GnssTime{2313, 259230.0});
 *   e.clock = EpochClock{30.0, TimeUtils::continuousGpsSeconds(e.se.time), true};
 *   networkCarryDecide(fx.run(), e);
 * @endcode
 *
 * Neither copyable nor movable (the run holds references into the fixture).  Build it on the
 * heap (std::make_unique) when a test needs several, since a NetworkRun is large.
 */
#include "network/driver/Epoch.hpp"
#include "network/driver/Run.hpp"
#include "network/state/RunContext.hpp"
#include "network/driver/SetupResult.hpp"
#include "config/GnssConfig.hpp"
#include "network/solve/Estimator.hpp"
#include "network/diag/RunTraces.hpp"
#include "math/TimeUtils.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <memory>
#include <new>
#include <optional>
#include <utility>

namespace GnssProcessor {

class NetworkTestRun {
public:
    /// @param config    The run's configuration (copied; the context's knobs are resolved from it).
    /// @param log       The run's logger; null selects spdlog's default logger.
    /// @param trace_dir The traces' (and the run's) log directory; empty selects the system
    ///                  temporary directory.  No stream opens until the test calls
    ///                  traces().openAll; traces().closeAll flushes them for reading.
    explicit NetworkTestRun(const GnssConfig& config = GnssConfig{},
                            std::shared_ptr<spdlog::logger> log = nullptr,
                            std::filesystem::path trace_dir = {})
        : config_(config),
          log_(log ? std::move(log) : spdlog::default_logger()),
          log_dir_(trace_dir.empty() ? std::filesystem::temp_directory_path() : std::move(trace_dir)),
          ctx_(config_, config_.getNetworkConfig()),
          traces_(log_dir_),
          run_(ctx_, config_, config_.getNetworkConfig(), log_, traces_, setup_, log_dir_,
               networkEngineSpec(config_, config_.getNetworkConfig(), ctx_)) {
        epoch_.emplace(se_);
    }
    NetworkTestRun(const NetworkTestRun&) = delete;
    NetworkTestRun& operator=(const NetworkTestRun&) = delete;

    NetworkRun&         run()    { return run_; }             ///< The run every epoch form takes first.
    NetworkRunContext&  ctx()    { return ctx_; }             ///< run().ctx: knobs (const), carry, counters.
    NetworkSetupResult& setup()  { return setup_; }           ///< run().setup: stations and the setup products.
    NetworkRunTraces&   traces() { return traces_; }          ///< run().traces (no stream open).
    NetworkEstimator&   est()    { return run_.engines.est; } ///< run().engines.est.
    const GnssConfig&   config() const { return config_; }    ///< run().config.

    /// Replaces the run's estimator with a fresh default-constructed one, in place, and returns
    /// it: a test that models several epochs' layouts registers each on a new estimator.  Every
    /// reference to the previous estimator (est(), run().engines.est) names the new one; the
    /// replacement is valid because NetworkEstimator has no const or reference data member.
    NetworkEstimator& renewEstimator() {
        NetworkEstimator* p = &run_.engines.est;
        p->~NetworkEstimator();
        return *::new (static_cast<void*>(p)) NetworkEstimator();
    }

    /// The current epoch (an empty one at time zero until the first newEpoch).
    NetworkEpoch& ep() { return *epoch_; }

    /// Ends the current epoch and builds a fresh one whose observations carry time tag @p t.
    /// @return The new epoch: every group empty, ep().se.time == @p t.
    NetworkEpoch& newEpoch(const GnssTime& t) {
        epoch_.reset();
        se_ = SyncedEpoch{};
        se_.time = t;
        epoch_.emplace(se_);
        return *epoch_;
    }

    /// The current epoch's observations (write them before the seam reads ep().se).
    SyncedEpoch& se() { return se_; }

private:
    GnssConfig                       config_;
    std::shared_ptr<spdlog::logger>  log_;
    std::filesystem::path            log_dir_;
    NetworkRunContext                ctx_;
    NetworkRunTraces                 traces_;
    NetworkSetupResult               setup_;
    NetworkRun                       run_;
    SyncedEpoch                      se_;
    std::optional<NetworkEpoch>      epoch_;
};

} // namespace GnssProcessor
