/**
 * @file MeasurementModelSeeds.cpp
 * @brief The first-epoch seeding of the single-receiver state and its ambiguity seed:
 *        MeasurementModel asks measurement/Seeds.hpp for the values and applies them to its
 *        @c StateManager, in the order the seeding has always run in.
 *
 * Each member evaluates its seed against the state vector as it is now, writes the values the
 * seed returns, and logs on the "state_initializer" logger.  A seed for a parameter that does
 * not exist is not returned, so nothing here checks for a missing state.  Whether a seed has
 * run is the latch of @c StateInitializer; the callers (computeMeasurementUpdate and the
 * state pre-pass) set it.
 */

#include "measurement/MeasurementModel.hpp"
#include "measurement/Seeds.hpp"
#include "measurement/UserParamView.hpp"
#include "common/Constants.hpp"

#include <algorithm>
#include <cmath>

namespace GnssProcessor {

    // ===================================================================
    // The inputs of the seeds
    // ===================================================================

    ObsInput MeasurementModel::residualSeedInput() const {
        return ObsInput{rowProducts(), state_initializer_.frequencyHelper(), &state_initializer_.resolver(),
                        nlwl_ref_band_, config_.getStateAmbiguityBasis(), clockRefs()};
    }

    ObsInput MeasurementModel::ambiguitySeedInput() const {
        return ObsInput{rowProducts(), ppp_utils_, &state_initializer_.resolver(), nlwl_ref_band_,
                        config_.getStateAmbiguityBasis(), clockRefs(), FdmaChannel::Zero};
    }

    double MeasurementModel::ambiguitySeed(const PppObservation& obs, const std::string& ref_type,
                                           const Eigen::VectorXd& x) const {
        return seedAmbiguityOfState(ambiguitySeedInput(), state_manager_, x, obs, ref_type);
    }

    // ===================================================================
    // Inter-system biases
    // ===================================================================

    bool MeasurementModel::initializeInterSystemBiases(
        const std::vector<PppObservation>& observations, const std::string& epoch_prefix) {

        const Eigen::VectorXd x = state_manager_.getStateVector();
        const UserParamView view(state_manager_, x, static_cast<std::size_t>(x.size()));
        const IsbSeeds result = seedIsb(residualSeedInput(), ParamLookup(view), observations);
        auto log = log_manager_.getLogger("state_initializer");

        if (!result.settled) {
            log->warn("{}Cannot initialize ISB: no GPS code observations", epoch_prefix);
            return false;   // retry next epoch; the caller does not latch as initialized
        }

        Eigen::VectorXd x_updated = state_manager_.getStateVector();
        for (const IsbSeed& seed : result.seeds) {
            x_updated(view.find(isbRef(seed.sys)).column) = seed.value_m;
            log->info("{}Initialized {} ISB: {:.3f} m ({:.3e} s) from {} code obs (GPS ref: {} obs)",
                      epoch_prefix, systemName(seed.sys), seed.value_m,
                      seed.value_m / Constants::SPEED_OF_LIGHT, seed.n_obs, result.reference_obs);
        }
        state_manager_.setStateVector(x_updated);
        return true;   // GPS reference present, so the ISB seeds are final
    }

    // ===================================================================
    // The cold receiver clock (network; opt-in)
    // ===================================================================
    //
    // The receiver clock state is the GPS-referenced offset (the inter-system biases carry
    // every other system's delta), so an estimator that has not yet been fed a code clock
    // holds x(clock) = 0.  On the network path nothing pre-solves it before the ambiguities
    // are first seeded, so the whole free-running clock offset would enter every cold
    // ambiguity seed uniformly.  Seeding x(clock) with the median GPS code residual removes it.

    void MeasurementModel::preseedColdReceiverClock(
        const std::vector<PppObservation>& observations, const std::string& epoch_prefix) {

        // Runs after the ionosphere seed (so the residual removes the seeded slant
        // ionosphere from the code) and before the ambiguity seeds, once per station.  The
        // latch is set only on success, as for the inter-system biases.  With the knob off
        // the seed is never formed.
        if (!config_.networkColdClockSeed() || state_initializer_.isClockSeedInitialized())
            return;

        const Eigen::VectorXd x = state_manager_.getStateVector();
        const UserParamView view(state_manager_, x, static_cast<std::size_t>(x.size()));
        const ColdClockSeed seed = seedColdClock(residualSeedInput(), ParamLookup(view),
                                                 observations);
        auto log = log_manager_.getLogger("state_initializer");

        switch (seed.outcome) {
            case ColdClockOutcome::NoClockState:
                break;   // no clock state in this configuration: nothing to seed or retry
            case ColdClockOutcome::AlreadySeeded:
                log->debug("{}Cold receiver-clock seed skipped: clock already {:.3f} m (not cold)",
                           epoch_prefix, seed.current_m);
                break;   // already seeded (e.g. by the PPP path): latch, do not overwrite
            case ColdClockOutcome::NoReference:
                log->warn("{}Cannot pre-seed cold receiver clock: no GPS code observations this epoch",
                          epoch_prefix);
                break;   // retry next epoch; do not latch
            case ColdClockOutcome::Seeded: {
                Eigen::VectorXd x_updated = state_manager_.getStateVector();
                x_updated(static_cast<Eigen::Index>(state_manager_.getClockIndex())) = seed.value_m;
                state_manager_.setStateVector(x_updated);
                log->info("{}Pre-seeded cold receiver clock: {:.3f} m ({:.3e} s) from {} GPS code obs",
                          epoch_prefix, seed.value_m, seed.value_m / Constants::SPEED_OF_LIGHT,
                          seed.n_obs);
                break;
            }
        }
        if (seed.settled()) state_initializer_.markClockSeedInitialized();
    }

    // ===================================================================
    // The phase clock
    // ===================================================================

    void MeasurementModel::initializePhaseClockOffset(
        const std::vector<PppObservation>& observations, const std::string& epoch_prefix) {

        const Eigen::VectorXd x = state_manager_.getStateVector();
        const UserParamView view(state_manager_, x, static_cast<std::size_t>(x.size()));
        const PhaseClockSeed seed = seedPhaseClock(residualSeedInput(), ParamLookup(view),
                                                   observations);
        auto log = log_manager_.getLogger("state_initializer");

        if (!seed.found) {
            log->warn("{}Cannot initialize phase clock offset: no satellite has both code and phase observations",
                      epoch_prefix);
            return;
        }

        Eigen::VectorXd x_updated = state_manager_.getStateVector();
        x_updated(state_manager_.getPhaseClockIndex()) = seed.value_m;
        state_manager_.setStateVector(x_updated);

        // Tighten the phase clock covariance to reflect the seed accuracy.
        {
            const double code_std = config_.getDefaultMeasurementNoise().code_std_m;
            const double seed_std = code_std / std::sqrt(static_cast<double>(seed.n_pairs));
            const double clamped_std = std::max(seed_std, code_std);
            const size_t pc_idx = state_manager_.getPhaseClockIndex();

            Eigen::MatrixXd P = state_manager_.getCovariance();
            P.row(pc_idx).setZero();
            P.col(pc_idx).setZero();
            P(pc_idx, pc_idx) = clamped_std * clamped_std;
            state_manager_.setCovariance(P);

            log->info("{}Set phase clock covariance: std={:.3f} m (seed_std={:.3f} m from {} pairs)",
                      epoch_prefix, clamped_std, seed_std, seed.n_pairs);
        }

        log->info("{}Initialized phase clock offset: {:.3f} m from {} code-phase pairs",
                  epoch_prefix, seed.value_m, seed.n_pairs);
    }

    // ===================================================================
    // The receiver code biases
    // ===================================================================

    void MeasurementModel::initializeCodeBiases(
        const std::vector<PppObservation>& observations, const std::string& epoch_prefix) {

        if (!config_.estimateIfb() || !config_.isUncombinedEnabled()) return;

        const Eigen::VectorXd x = state_manager_.getStateVector();
        const UserParamView view(state_manager_, x, static_cast<std::size_t>(x.size()));
        const CodeBiasSeeds result =
            seedCodeBias(residualSeedInput(), ParamLookup(view), observations,
                         state_initializer_.codeBiasSeedOverride());
        auto log = log_manager_.getLogger("state_initializer");

        for (std::size_t i = 0; i < result.refused; ++i)
            state_initializer_.noteCodeBiasRestore(/*applied=*/false);
        if (result.seeds.empty()) return;

        Eigen::VectorXd x_updated = x;
        for (const CodeBiasSeed& seed : result.seeds) {
            x_updated(view.find(rxCodeBiasRef(seed.key)).column) = seed.value_m;
            const std::string key = userCodeBiasKey(seed.key);
            if (seed.restored) {
                state_initializer_.noteCodeBiasRestore(/*applied=*/true);
                log->info("{}RS-9: code bias {} seeded from the RESTORED "
                          "linearization point: {:.6f} m (the one-epoch code "
                          "median is NOT used)", epoch_prefix, key, seed.value_m);
            } else {
                log->info("{}Initialized code bias {}: {:.3f} m from {} satellites",
                          epoch_prefix, key, seed.value_m, seed.n_sats);
            }
        }
        state_manager_.setStateVector(x_updated);
    }

} // namespace GnssProcessor
