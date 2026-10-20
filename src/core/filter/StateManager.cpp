/*!
 * \file StateManager.cpp
 * \brief Kalman filter state, covariance and ambiguity bookkeeping.
 *
 * Organized as: file-scope helpers; the internal row edits (appendState,
 * removeStateAtIndex, indicesOf_); construction; core accessors and mutators;
 * process noise and transition; index getters and state labels; ambiguity
 * lookup and birth; PppState synchronization; the SSR discontinuity memory;
 * ambiguity reset, shift, restore, rename, removal, seed and prune; the slant
 * ionosphere family; the re-inflations and their shared rule; the inputs of the
 * process-noise rules (elevations, azimuths, frozen pivots, overrides); then one
 * section per remaining family (receiver code bias, satellite code bias,
 * satellite phase bias, receiver phase bias, phase colour, satellite clock), the
 * satellite code-bias cache, the unresolvable and pending-reseed marks, and the
 * gap-recovery edits.
 */

#include "filter/StateManager.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"
#include "common/Constants.hpp"
#include "math/TimeUtils.hpp"
#include <limits>
#include <stdexcept>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <Eigen/Dense>

// Formatter specialization for Eigen::VectorXd
template <>
struct fmt::formatter<Eigen::VectorXd> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const Eigen::VectorXd& vec, FormatContext& ctx) const -> decltype(ctx.out()) {
        auto out = ctx.out();
        out = fmt::format_to(out, "[");
        for (Eigen::Index i = 0; i < vec.size(); ++i) {
            out = fmt::format_to(out, "{:.3f}", vec(i));
            if (i < vec.size() - 1)
                out = fmt::format_to(out, ", ");
        }
        return fmt::format_to(out, "]");
    }
};

namespace GnssProcessor {

    // ===================================================================
    // Internal state vector / covariance manipulation
    // ===================================================================

    size_t StateManager::appendState(double initial_value, double initial_variance) {
        const size_t index = getStateSize();
        Eigen::VectorXd new_sv(index + 1);
        Eigen::MatrixXd new_cov = Eigen::MatrixXd::Zero(index + 1, index + 1);

        if (index > 0) {
            new_sv.head(index) = state_vector_;
            new_cov.topLeftCorner(index, index) = covariance_;
        }
        new_sv(index) = initial_value;
        new_cov(index, index) = initial_variance;

        state_vector_ = std::move(new_sv);
        covariance_ = std::move(new_cov);
        return index;
    }

    void StateManager::removeStateAtIndex(size_t rem_idx) {
        const size_t state_size = getStateSize();
        const size_t new_size = state_size - 1;
        const auto ri = static_cast<Eigen::Index>(rem_idx);

        Eigen::VectorXd new_sv = Eigen::VectorXd::Zero(new_size);
        Eigen::MatrixXd new_cov = Eigen::MatrixXd::Zero(new_size, new_size);

        // Head (before removed index)
        if (rem_idx > 0) {
            new_sv.head(ri) = state_vector_.head(ri);
            new_cov.topLeftCorner(ri, ri) = covariance_.topLeftCorner(ri, ri);
        }

        // Tail (after removed index)
        if (rem_idx + 1 < state_size) {
            const auto tail = static_cast<Eigen::Index>(state_size - rem_idx - 1);
            new_sv.segment(ri, tail) = state_vector_.segment(ri + 1, tail);
            new_cov.block(ri, ri, tail, tail) = covariance_.block(ri + 1, ri + 1, tail, tail);
            // Cross-covariance between head and tail
            if (rem_idx > 0) {
                new_cov.block(0, ri, ri, tail) = covariance_.block(0, ri + 1, ri, tail);
                new_cov.block(ri, 0, tail, ri) = covariance_.block(ri + 1, 0, tail, ri);
            }
        }

        state_vector_ = std::move(new_sv);
        covariance_ = std::move(new_cov);

        // Decrement all dynamic-state indices that were above the removed index
        for (const auto& spec : kStateFamilies)
            for (auto& kv : indicesOf_(spec.family))
                if (kv.second > rem_idx) --kv.second;
    }

    std::map<std::string, size_t>& StateManager::indicesOf_(StateFamily family) {
        switch (family) {
            case StateFamily::Ionosphere:        return ionosphere_indices_;
            case StateFamily::IonosphereRate:    return iono_rate_indices_;
            case StateFamily::Ambiguity:         return ambiguity_indices_;
            case StateFamily::ReceiverCodeBias:  return code_bias_indices_;
            case StateFamily::SatBias:           return sat_bias_indices_;
            case StateFamily::SatPhaseBias:      return sat_phase_bias_indices_;
            case StateFamily::ReceiverPhaseBias: return receiver_phase_bias_indices_;
            case StateFamily::PhaseColour:       return phase_colour_indices_;
            case StateFamily::SatClock:          return sat_clock_indices_;
        }
        throw std::logic_error("StateManager::indicesOf_: unknown StateFamily");
    }

    const std::map<std::string, size_t>& StateManager::indicesOf(StateFamily family) const {
        return const_cast<StateManager*>(this)->indicesOf_(family);
    }

    // ===================================================================
    // Construction: StateManagerLayout.cpp
    // ===================================================================

    // ===================================================================
    // Core accessors / mutators
    // ===================================================================

    GnssTime StateManager::getStateEpochTime() const {
        return state_epoch_time_;
    }

    void StateManager::setStateEpochTime(const GnssTime& time) {
        state_epoch_time_ = time;
        auto logger = log_manager_.getLogger("state_manager");
        logger->trace("Set state epoch time: week={}, TOW={:.3f}", time.week, time.tow);
    }

    void StateManager::setStateVector(const Eigen::VectorXd& state) {
        auto logger = log_manager_.getLogger("state_manager");
        if (static_cast<size_t>(state.size()) != getStateSize()) {
            logger->error("State vector size {} does not match expected size {}", state.size(), getStateSize());
            throw std::invalid_argument("State vector size " + std::to_string(state.size()) +
                " does not match expected size " + std::to_string(getStateSize()));
        }
        if (!state.allFinite()) {
            logger->error("State vector contains non-finite values: {}", state);
        }
        state_vector_ = state;
        logger->trace("Set state vector: size={}, values={}, expected_size={}, ambiguities={}", state.size(),
            state, getStateSize(), ambiguity_indices_.size());
    }

    void StateManager::setCovariance(const Eigen::MatrixXd& covariance) {
        auto logger = log_manager_.getLogger("state_manager");
        if (static_cast<size_t>(covariance.rows()) != getStateSize() ||
            static_cast<size_t>(covariance.cols()) != getStateSize()) {
            logger->error("Covariance matrix dimensions ({}x{}) do not match state size {}",
                covariance.rows(), covariance.cols(), getStateSize());
            throw std::invalid_argument("Covariance matrix dimensions (" +
                std::to_string(covariance.rows()) + "x" +
                std::to_string(covariance.cols()) +
                ") do not match state size " + std::to_string(getStateSize()));
        }
        if (!covariance.allFinite()) {
            logger->error("Covariance matrix contains non-finite values");
            throw std::invalid_argument("Covariance matrix contains non-finite values");
        }
        covariance_ = covariance;
        logger->trace("Set covariance matrix: size={}x{}, expected_size={}, ambiguities={}", covariance.rows(), covariance.cols(),
            getStateSize(), ambiguity_indices_.size());
    }

    size_t StateManager::getStateSize() const {
        size_t n = core_state_size_;
        for (const auto& spec : kStateFamilies) n += indicesOf(spec.family).size();
        return n;
    }

    Eigen::VectorXd StateManager::getStateVector() const {
        return state_vector_;
    }

    Eigen::MatrixXd StateManager::getCovariance() const {
        return covariance_;
    }

    double StateManager::getStateVariance(int idx) const {
        if (idx < 0 || idx >= covariance_.rows()) return 0.0;
        return covariance_(idx, idx);
    }

    // ===================================================================
    // Process noise / transition: StateManagerProcessNoise.cpp
    // ===================================================================

    // ===================================================================
    // Index getters
    // ===================================================================

    size_t StateManager::getPositionIndex() const { return 0; }
    size_t StateManager::getVelocityIndex() const { return velocity_idx_; }
    bool   StateManager::hasVelocity() const { return velocity_idx_ != std::numeric_limits<size_t>::max(); }
    size_t StateManager::getClockIndex() const { return clock_idx_; }
    bool   StateManager::hasClock() const { return clock_idx_ != std::numeric_limits<size_t>::max(); }
    size_t StateManager::getClockRateIndex() const { return clock_rate_idx_; }
    bool   StateManager::hasClockRate() const { return clock_rate_idx_ != std::numeric_limits<size_t>::max(); }
    size_t StateManager::getPhaseClockIndex() const { return phase_clock_idx_; }
    bool   StateManager::hasPhaseClock() const { return phase_clock_idx_ != std::numeric_limits<size_t>::max(); }
    size_t StateManager::getTropoIndex() const { return tropo_idx_; }
    size_t StateManager::getDifferentialTropoIndex() const {
        return differential_tropo_idx_;
    }
    bool StateManager::hasDifferentialTropo() const {
        return differential_tropo_idx_ != std::numeric_limits<size_t>::max();
    }

    size_t StateManager::getIonoIndex() const {
        // Deprecated: per-satellite iono states are managed dynamically.
        return std::numeric_limits<size_t>::max();
    }

    size_t StateManager::getTropoGradientNorthIndex() const { return gradient_north_idx_; }
    size_t StateManager::getTropoGradientEastIndex() const { return gradient_east_idx_; }
    bool   StateManager::hasTropoGradients() const { return gradient_north_idx_ != std::numeric_limits<size_t>::max(); }

    bool   StateManager::hasImuBiases() const { return accel_bias_idx_ != std::numeric_limits<size_t>::max(); }
    size_t StateManager::getAccelBiasIndex() const { return accel_bias_idx_; }
    size_t StateManager::getGyroBiasIndex() const { return gyro_bias_idx_; }
    size_t StateManager::getAttitudeIndex() const { return attitude_idx_; }



    size_t StateManager::getSystemTimeOffsetIndex(const std::string& system_name) const {
        auto it = system_time_offset_indices_.find(system_name);
        return it != system_time_offset_indices_.end() ? it->second : std::numeric_limits<size_t>::max();
    }

    size_t StateManager::getSystemTimeRateOffsetIndex(const std::string& system_name) const {
        auto it = system_time_rate_offset_indices_.find(system_name);
        return it != system_time_rate_offset_indices_.end()
             ? it->second : std::numeric_limits<size_t>::max();
    }

    size_t StateManager::getNumSystemTimeOffsets() const {
        return systems_with_time_offsets_.size();
    }

    std::vector<std::string> StateManager::getStateLabels() const {
        const size_t n = getStateSize();
        std::vector<std::string> labels(n);
        for (size_t i = 0; i < n; ++i) labels[i] = "STATE_" + std::to_string(i);

        constexpr size_t kNone = std::numeric_limits<size_t>::max();
        auto put = [&](size_t idx, const std::string& name) {
            if (idx < n) labels[idx] = name;
        };

        // --- Fixed-core block.  Core states carry no stored name strings, so
        //     synthesize labels from the index getters (guarded by their
        //     has*()/sentinel checks; the layout is mode-dependent). ---
        put(0, "POS_X");
        put(1, "POS_Y");
        put(2, "POS_Z");
        if (hasClock())               put(getClockIndex(), "CLK");
        if (getTropoIndex() != kNone) put(getTropoIndex(), "ZWD");
        if (hasVelocity()) {
            put(getVelocityIndex() + 0, "VEL_X");
            put(getVelocityIndex() + 1, "VEL_Y");
            put(getVelocityIndex() + 2, "VEL_Z");
        }
        if (hasClockRate())  put(getClockRateIndex(),  "CLK_RATE");
        if (hasPhaseClock()) put(getPhaseClockIndex(), "PHASE_CLK");
        if (hasTropoGradients()) {
            put(getTropoGradientNorthIndex(), "TGRAD_N");
            put(getTropoGradientEastIndex(),  "TGRAD_E");
        }
        if (hasImuBiases()) {
            for (size_t k = 0; k < 3; ++k) {
                put(getAttitudeIndex()  + k, "ATT_"  + std::to_string(k));
                put(getAccelBiasIndex() + k, "BACC_" + std::to_string(k));
                put(getGyroBiasIndex()  + k, "BGYR_" + std::to_string(k));
            }
        }
        if (hasDifferentialTropo()) put(getDifferentialTropoIndex(), "DZWD");

        // Inter-system time / rate offsets (ISB / ISBR), keyed by system name.
        for (const auto& sys : systems_with_time_offsets_) {
            const size_t oi = getSystemTimeOffsetIndex(sys);
            if (oi != kNone) put(oi, "ISB_" + sys);
            const size_t ri = getSystemTimeRateOffsetIndex(sys);
            if (ri != kNone) put(ri, "ISBR_" + sys);
        }

        // --- Dynamic key->index maps.  The keys themselves are the stable labels
        //     (StateKeySchema); the family's label prefix (kStateFamilies) is
        //     prepended so that keys from different maps cannot collide (e.g.
        //     iono "G01" vs sat-bias "G01|C1W"). ---
        for (const auto& spec : kStateFamilies)
            for (const auto& kv : indicesOf(spec.family))
                put(kv.second, spec.label_prefix + kv.first);

        return labels;
    }

    const std::vector<std::string>& StateManager::getSystemsWithTimeOffsets() const {
        return systems_with_time_offsets_;
    }

    // ===================================================================
    // Ambiguity management
    // ===================================================================

    int StateManager::getAmbiguityIndex(const std::string& key) const {
        auto it = ambiguity_indices_.find(key);
        return it != ambiguity_indices_.end() ? static_cast<int>(it->second) : -1;
    }

    const std::map<std::string, size_t>& StateManager::getAmbiguities() const {
        return ambiguity_indices_;
    }

    uint64_t StateManager::getAmbiguityGeneration(const std::string& key) const {
        const auto it = ambiguity_generations_.find(key);
        return it == ambiguity_generations_.end() ? 0 : it->second;
    }

    bool StateManager::noteAmbiguityDiscontinuity(const std::string& key) {
        if (!ambiguity_indices_.count(key)) return false;
        ambiguity_generations_[key] = ++next_ambiguity_generation_;
        return true;
    }

    size_t StateManager::addAmbiguity(const std::string& key, double initial_value,
                                        double initial_std_override) {
        auto logger = log_manager_.getLogger("state_manager");
        if (ambiguity_indices_.count(key)) {
            logger->trace("Ambiguity {} already exists at index {}", key, ambiguity_indices_[key]);
            return ambiguity_indices_[key];
        }
        const double amb_std = (initial_std_override > 0.0)
                                ? initial_std_override
                                : config_.getAmbiguityStd();
        const size_t index = appendState(initial_value, amb_std * amb_std);
        ambiguity_indices_[key] = index;
        noteAmbiguityDiscontinuity(key);

        logger->info("Added ambiguity {} at index {} with initial value {:.3f}, std={:.3f} m{}",
                     key, index, initial_value, amb_std,
                     initial_std_override > 0.0 ? " (override)" : "");
        return index;
    }

    // ===================================================================
    // PppState synchronization
    // ===================================================================

    void StateManager::updatePppStateFromVector(const Eigen::VectorXd& current_state, PppState& ppp_state, const StateManager& state_manager) const {
        ppp_state.position         = current_state.segment<3>(state_manager.getPositionIndex());
        ppp_state.clock_bias       = state_manager.hasClock()
                                     ? current_state(state_manager.getClockIndex())
                                     : 0.0;
        ppp_state.troposphere_zwd  = current_state(state_manager.getTropoIndex());

        if (state_manager.hasVelocity())
            ppp_state.velocity = current_state.segment<3>(state_manager.getVelocityIndex());

        if (state_manager.hasPhaseClock())
            ppp_state.phase_clock_offset = current_state(state_manager.getPhaseClockIndex());
        if (state_manager.hasTropoGradients()) {
            ppp_state.tropo_gradient_north = current_state(state_manager.getTropoGradientNorthIndex());
            ppp_state.tropo_gradient_east  = current_state(state_manager.getTropoGradientEastIndex());
        }

        if (config_.carriesSlantIonosphere()) {
            ppp_state.slant_ionosphere.clear();
            for (const auto& [sat, idx] : ionosphere_indices_)
                if (idx < static_cast<size_t>(current_state.size()))
                    ppp_state.slant_ionosphere[sat] = current_state(idx);
            ppp_state.slant_ionosphere_rate.clear();
            for (const auto& [sat, idx] : iono_rate_indices_)
                if (idx < static_cast<size_t>(current_state.size()))
                    ppp_state.slant_ionosphere_rate[sat] = current_state(idx);
        }

        ppp_state.system_time_offsets.clear();
        for (const auto& sys : systems_with_time_offsets_) {
            const size_t idx = getSystemTimeOffsetIndex(sys);
            if (idx < static_cast<size_t>(current_state.size()))
                ppp_state.system_time_offsets[sys] = current_state(idx);
        }

        for (const auto& [key, idx] : state_manager.getAmbiguities())
            if (state_manager.getAmbiguityIndex(key) > 0)
                ppp_state.ambiguities[key] = current_state(state_manager.getAmbiguityIndex(key));
    }

    void StateManager::syncPppStateToStateVector(const PppState& ppp_state) {
        Eigen::VectorXd sv = state_vector_;
        sv.segment<3>(getPositionIndex()) = ppp_state.position;
        if (hasClock())
            sv(getClockIndex()) = ppp_state.clock_bias;
        sv(getTropoIndex()) = ppp_state.troposphere_zwd;

        if (hasVelocity())
            sv.segment<3>(getVelocityIndex()) = ppp_state.velocity;

        if (hasPhaseClock())
            sv(getPhaseClockIndex()) = ppp_state.phase_clock_offset;
        if (hasTropoGradients()) {
            sv(getTropoGradientNorthIndex()) = ppp_state.tropo_gradient_north;
            sv(getTropoGradientEastIndex())  = ppp_state.tropo_gradient_east;
        }

        if (config_.carriesSlantIonosphere()) {
            for (const auto& [sat, value] : ppp_state.slant_ionosphere) {
                auto it = ionosphere_indices_.find(sat);
                if (it != ionosphere_indices_.end() && it->second < static_cast<size_t>(sv.size()))
                    sv(it->second) = value;
            }
            for (const auto& [sat, value] : ppp_state.slant_ionosphere_rate) {
                auto it = iono_rate_indices_.find(sat);
                if (it != iono_rate_indices_.end() && it->second < static_cast<size_t>(sv.size()))
                    sv(it->second) = value;
            }
        }

        for (const auto& [sys, value] : ppp_state.system_time_offsets) {
            const size_t idx = getSystemTimeOffsetIndex(sys);
            if (idx < static_cast<size_t>(sv.size()))
                sv(idx) = value;
        }

        setStateVector(sv);
    }

    bool StateManager::noteSsrDiscontinuity(const std::string& key, int counter) {
        auto it = last_ssr_discontinuity_.find(key);
        if (it == last_ssr_discontinuity_.end()) {
            last_ssr_discontinuity_.emplace(key, counter);   // first sighting: record only
            return false;
        }
        if (it->second != counter) {                         // changed (any direction) -> bump
            it->second = counter;
            return true;
        }
        return false;
    }

    bool StateManager::resetAmbiguity(const std::string& key, double std_m) {
        auto logger = log_manager_.getLogger("state_manager");

        if (key.empty()) {
            logger->warn("resetAmbiguity called with empty key");
            return false;
        }
        if (!(std::isfinite(std_m) && std_m > 0.0)) {
            logger->warn("resetAmbiguity called with invalid std_m for key {}: {}", key, std_m);
            return false;
        }

        const int idx_i = getAmbiguityIndex(key);
        if (idx_i < 0) {
            // Refuse to auto-create the state for an unknown key.
            //
            // Slip detectors iterate @c obs.raw_observed, which keeps every
            // RINEX-tracked signal, while the state vector holds only ambiguities
            // passing the @c signal_mode band filter.  Without this guard a reset
            // reported for L5X on a Dual-mode run would call @c addAmbiguity and
            // sneak a band-filtered signal into the state vector.
            //
            // "Create-on-reset" was meant for first-epoch creation, but
            // @c ensureRequiredStates now owns that and seeds new ambiguities from
            // geometry, so reset keys for non-existent ambiguities are no-ops.
            logger->debug("resetAmbiguity ignoring unknown key {} "
                          "(slip detector referenced signal not in current state — "
                          "likely band-filtered out by signal_mode)", key);
            return false;
        }

        const auto idx = static_cast<Eigen::Index>(idx_i);
        if (idx >= state_vector_.size() || idx >= covariance_.rows()) {
            logger->error("resetAmbiguity index out of bounds for key {}: idx={}, state_size={}, cov={}x{}",
                key, idx_i, state_vector_.size(), covariance_.rows(), covariance_.cols());
            return false;
        }

        // The current value is kept; ensureRequiredStates() overwrites it with a
        // geometry-based seed.
        covariance_.row(idx).setZero();
        covariance_.col(idx).setZero();
        covariance_(idx, idx) = std_m * std_m;

        markAmbiguityPendingReseed(key);

        logger->info("Reset ambiguity {} at index {}: marked pending reseed, std={:.3f} m (var={:.3e})",
            key, idx_i, std_m, std_m * std_m);
        return true;
    }

    bool StateManager::shiftAmbiguity(const std::string& key, double delta_m) {
        auto logger = log_manager_.getLogger("state_manager");

        if (key.empty()) {
            logger->warn("shiftAmbiguity called with empty key");
            return false;
        }
        if (!std::isfinite(delta_m)) {
            logger->warn("shiftAmbiguity called with non-finite delta for key {}: {}", key, delta_m);
            return false;
        }

        const int idx_i = getAmbiguityIndex(key);
        if (idx_i < 0) {
            // Same guard as resetAmbiguity: no state is created for a key the
            // band filter kept out of the state vector (the datum-break
            // reactions iterate the raw modeled signals, as the slip detectors
            // do).
            logger->debug("shiftAmbiguity ignoring unknown key {} "
                          "(signal not in current state — likely band-filtered out by "
                          "signal_mode, or not yet created this epoch)", key);
            return false;
        }

        const auto idx = static_cast<Eigen::Index>(idx_i);
        if (idx >= state_vector_.size()) {
            logger->error("shiftAmbiguity index out of bounds for key {}: idx={}, state_size={}",
                key, idx_i, state_vector_.size());
            return false;
        }

        // Value-only translation: the covariance and the pending-reseed set are
        // deliberately untouched (see the header).
        state_vector_(idx) += delta_m;
        if (delta_m != 0.0) noteAmbiguityDiscontinuity(key);
        logger->debug("Shifted ambiguity {} at index {} by {:+.4f} m (value-only, covariance kept)",
                      key, idx_i, delta_m);
        return true;
    }

    bool StateManager::restoreDynamicState(StateFamily family, const std::string& key,
                                           double value, size_t expect_idx) {
        auto logger = log_manager_.getLogger("state_manager");
        if (key.empty()) {
            logger->error("restoreDynamicState: empty key at expected index {}", expect_idx);
            return false;
        }
        if (!specOf(family).snapshot_tag) {
            logger->error("restoreDynamicState: the {} family is not serialized by the RT "
                          "snapshot and cannot be restored ('{}')", specOf(family).name, key);
            return false;
        }
        std::map<std::string, size_t>* map = &indicesOf_(family);
        if (map->count(key)) {
            logger->error("restoreDynamicState: key '{}' already registered — the resume "
                          "replay must run on a core-only manager", key);
            return false;
        }
        // The landing index is verified before appending.  getStateSize() is
        // derived from the maps (core + Σ registered map sizes), so a row
        // appended but not registered would put the vector out of step with the
        // reported size, and the next append's head copy would silently truncate
        // it.  Every refusal from this function therefore leaves the state
        // unmodified.
        if (getStateSize() != expect_idx) {
            logger->error("restoreDynamicState: '{}' would land at index {} but the "
                          "snapshot serialized it at {} — layout divergence, resume "
                          "aborted (no mutation)",
                          key, getStateSize(), expect_idx);
            return false;
        }
        // Placeholder unit variance: the resume's wholesale setCovariance follows
        // the layout rebuild, so nothing ever reads this value.
        const size_t idx = appendState(value, 1.0);
        (*map)[key] = idx;
        if (family == StateFamily::Ambiguity)
            noteAmbiguityDiscontinuity(key);
        return true;
    }

    bool StateManager::renameAmbiguity(const std::string& old_key, const std::string& new_key) {
        auto logger = log_manager_.getLogger("state_manager");
        if (old_key.empty() || new_key.empty()) {
            logger->warn("renameAmbiguity called with empty key (old='{}', new='{}')", old_key, new_key);
            return false;
        }
        if (old_key == new_key) return true;  // no-op
        auto it = ambiguity_indices_.find(old_key);
        if (it == ambiguity_indices_.end()) {
            logger->trace("renameAmbiguity: old_key '{}' not found", old_key);
            return false;
        }
        if (ambiguity_indices_.count(new_key)) {
            logger->warn("renameAmbiguity: new_key '{}' already exists (old='{}')", new_key, old_key);
            return false;
        }
        const size_t idx = it->second;
        ambiguity_indices_.erase(it);
        ambiguity_indices_[new_key] = idx;
        ambiguity_generations_.erase(old_key);
        noteAmbiguityDiscontinuity(new_key);
        logger->trace("Renamed ambiguity '{}' -> '{}' at index {}", old_key, new_key, idx);
        return true;
    }

    bool StateManager::removeAmbiguity(const std::string& key) {
        auto logger = log_manager_.getLogger("state_manager");
        if (key.empty()) {
            logger->warn("removeAmbiguity called with empty key");
            return false;
        }
        auto it = ambiguity_indices_.find(key);
        if (it == ambiguity_indices_.end()) {
            logger->trace("removeAmbiguity: key {} not found", key);
            return false;
        }

        const size_t rem_idx = it->second;
        if (rem_idx < core_state_size_ || getStateSize() <= core_state_size_) {
            logger->warn("removeAmbiguity: invalid index {} for key {}", rem_idx, key);
            return false;
        }

        removeStateAtIndex(rem_idx);
        ambiguity_indices_.erase(key);
        ambiguity_generations_.erase(key);

        logger->info("Removed ambiguity {} at former index {}. New state size={}", key, rem_idx, getStateSize());
        return true;
    }

    void StateManager::pruneAmbiguitiesExcept(const std::set<std::string>& keep_keys) {
        auto logger = log_manager_.getLogger("state_manager");
        if (ambiguity_indices_.empty()) {
            logger->trace("pruneAmbiguitiesExcept: no ambiguities to prune");
            return;
        }

        std::vector<std::string> to_remove;
        to_remove.reserve(ambiguity_indices_.size());
        for (const auto& [key, idx] : ambiguity_indices_)
            if (keep_keys.find(key) == keep_keys.end())
                to_remove.push_back(key);

        if (to_remove.empty()) {
            logger->trace("pruneAmbiguitiesExcept: nothing to remove (keep_keys size={})", keep_keys.size());
            return;
        }

        logger->info("Pruning {} ambiguities (keeping {} keys)", to_remove.size(), keep_keys.size());
        for (const auto& key : to_remove)
            removeAmbiguity(key);
    }

    // ===================================================================
    // Per-satellite ionosphere state management (uncombined processing)
    // ===================================================================

    int StateManager::getIonosphereIndex(const std::string& satellite) const {
        auto it = ionosphere_indices_.find(satellite);
        return it != ionosphere_indices_.end() ? static_cast<int>(it->second) : -1;
    }

    const std::map<std::string, size_t>& StateManager::getIonosphereStates() const {
        return ionosphere_indices_;
    }

    int StateManager::getIonosphereRateIndex(const std::string& satellite) const {
        auto it = iono_rate_indices_.find(satellite);
        return it != iono_rate_indices_.end() ? static_cast<int>(it->second) : -1;
    }

    const std::map<std::string, size_t>& StateManager::getIonosphereRateStates() const {
        return iono_rate_indices_;
    }

    size_t StateManager::addIonosphereState(const std::string& satellite, double initial_value,
                                             double std_scale) {
        auto logger = log_manager_.getLogger("state_manager");
        if (ionosphere_indices_.count(satellite)) {
            logger->trace("Ionosphere state for {} already exists at index {}", satellite, ionosphere_indices_[satellite]);
            return ionosphere_indices_[satellite];
        }

        // Scale initial variance by obliquity factor 1/sin²(el) when elevation is available.
        // std_scale applies on top to inflate uncertainty when the seed value is unreliable.
        const double iono_std = config_.getInitialStd().ionosphere * std_scale;
        double init_var = iono_std * iono_std;
        auto el_it = ionosphere_elevations_.find(satellite);
        if (el_it != ionosphere_elevations_.end() && el_it->second > 0.0) {
            double sin2_el = std::sin(el_it->second * Constants::PI / 180.0);
            sin2_el *= sin2_el;
            if (sin2_el < 0.01) sin2_el = 0.01;
            init_var /= sin2_el;
        }

        const size_t index = appendState(initial_value, init_var);
        ionosphere_indices_[satellite] = index;

        // Constant-velocity model: append the companion iono-rate state Idot_s
        // (seed 0, cold-start variance from initial_std.ionosphere_rate).  The
        // two rows are adjacent when appended, but after prunes and re-adds they
        // need not be at (idx, idx+1); always address them through their maps.
        if (config_.enableIonoRate() && iono_rate_indices_.find(satellite) == iono_rate_indices_.end()) {
            const double r_std = config_.getInitialStd().ionosphere_rate;
            const double r_var = r_std * r_std;
            const size_t r_index = appendState(0.0, r_var);
            iono_rate_indices_[satellite] = r_index;
            logger->info("Added iono-rate state for {} at index {}, init_std={:.3f} m/s",
                satellite, r_index, r_std);
        }

        logger->info("Added ionosphere state for {} at index {} with initial value {:.3f} m, init_std={:.1f} m",
            satellite, index, initial_value, std::sqrt(init_var));
        return index;
    }

    void StateManager::pruneIonosphereStatesExcept(const std::set<std::string>& keep_keys) {
        auto logger = log_manager_.getLogger("state_manager");
        if (ionosphere_indices_.empty())
            return;

        std::vector<std::string> to_remove;
        for (const auto& [sat, idx] : ionosphere_indices_)
            if (keep_keys.find(sat) == keep_keys.end())
                to_remove.push_back(sat);
        if (to_remove.empty()) return;

        logger->info("Pruning {} ionosphere states (keeping {} satellites)", to_remove.size(), keep_keys.size());
        for (const auto& sat : to_remove) {
            auto it = ionosphere_indices_.find(sat);
            if (it == ionosphere_indices_.end()) continue;

            const size_t rem_idx = it->second;
            if (rem_idx < core_state_size_ || getStateSize() <= core_state_size_) {
                logger->warn("pruneIonosphereStatesExcept: invalid index {} for {}", rem_idx, sat);
                continue;
            }

            // Constant-velocity model: the satellite owns two rows (level + rate).
            // removeStateAtIndex shifts every higher dynamic index down by one, so
            // the higher index is removed first.  removeStateAtIndex sizes the
            // shrunk matrix from getStateSize(), which counts the index maps, so
            // the map count has to match the matrix at each call: each row's map
            // entry is erased right after that row is removed, not both up front.
            auto rit = iono_rate_indices_.find(sat);
            if (rit != iono_rate_indices_.end()) {
                const size_t rate_idx = rit->second;
                const size_t hi = std::max(rem_idx, rate_idx);
                const size_t lo = std::min(rem_idx, rate_idx);
                auto erase_entry = [&](size_t which) {
                    if (which == rate_idx) iono_rate_indices_.erase(sat);
                    else                   ionosphere_indices_.erase(sat);
                };
                removeStateAtIndex(hi);   // matrix -1; keep both map entries so getStateSize()==old
                erase_entry(hi);          // now maps count == matrix (old-1)
                removeStateAtIndex(lo);   // matrix -1; getStateSize()==old-1 matches
                erase_entry(lo);          // maps count == matrix (old-2)
                ionosphere_elevations_.erase(sat);
                ionosphere_azimuths_.erase(sat);
                logger->debug("Removed ionosphere+rate states for {} at former indices {} / {}",
                              sat, rem_idx, rate_idx);
                continue;
            }
            removeStateAtIndex(rem_idx);
            ionosphere_indices_.erase(sat);
            ionosphere_elevations_.erase(sat);
            ionosphere_azimuths_.erase(sat);

            logger->debug("Removed ionosphere state for {} at former index {}", sat, rem_idx);
        }
    }

    void StateManager::reinflateIonosphereVariance(const std::string& satellite,
                                                    double new_variance) {
        auto logger = log_manager_.getLogger("state_manager");

        const auto it = ionosphere_indices_.find(satellite);
        if (it == ionosphere_indices_.end()) {
            logger->debug("reinflateIonosphereVariance: no iono state for {}", satellite);
            return;
        }
        const auto idx = static_cast<Eigen::Index>(it->second);
        if (idx >= state_vector_.size() || idx >= covariance_.rows()) {
            logger->error("reinflateIonosphereVariance index out of bounds for {}: idx={}, state_size={}, cov={}x{}",
                satellite, idx, state_vector_.size(), covariance_.rows(), covariance_.cols());
            return;
        }

        // A frozen pivot row is left alone: PivotManager holds the BSSD
        // pivot's iono state at (0, 0), and re-inflating it would corrupt the
        // BSSD frame.  The callers (CycleSlipManager / Application) already
        // skip pivots; this guard covers a call site that does not.
        const double current_diag = covariance_(idx, idx);
        if (!(current_diag > 0.0)) {
            logger->warn("reinflateIonosphereVariance refused on frozen pivot {} "
                          "(idx={}, P_diag={:.3e}); BSSD invariant preserved",
                          satellite, idx, current_diag);
            return;
        }

        if (!(new_variance > 0.0)) {
            const double io_std = config_.getInitialStd().ionosphere;
            logger->warn("reinflateIonosphereVariance: non-positive variance {} for {}; "
                          "falling back to initial_std.ionosphere² = {}",
                          new_variance, satellite, io_std * io_std);
            new_variance = io_std * io_std;
        }

        const double prev_value = state_vector_(idx);
        const double prev_diag  = current_diag;

        const double applied =
            reinflateDiag_(idx, current_diag, new_variance, Reinflation::Ionosphere);
        // The value is kept: a loss of phase lock at the receiver does not
        // change the slant iono.  Only the joint (iono, sat-bias) covariance
        // partition is broken.

        logger->info("Reinflated iono state for {} at index {}: "
                      "value={:+.4f} m (unchanged), std {:.4f} -> {:.4f} m ({})",
                      satellite, idx, prev_value,
                      std::sqrt(std::max(prev_diag, 0.0)), std::sqrt(applied),
                      applied > new_variance ? "kept: the current variance is larger, a "
                                               "re-inflation never shrinks"
                                             : "raised");
    }

    void StateManager::reinflateIonosphereRateVariance(const std::string& satellite,
                                                       double new_variance) {
        if (!config_.enableIonoRate()) return;
        auto logger = log_manager_.getLogger("state_manager");

        const auto it = iono_rate_indices_.find(satellite);
        if (it == iono_rate_indices_.end()) {
            logger->debug("reinflateIonosphereRateVariance: no iono-rate state for {}", satellite);
            return;
        }
        const auto idx = static_cast<Eigen::Index>(it->second);
        if (idx >= state_vector_.size() || idx >= covariance_.rows()) {
            logger->error("reinflateIonosphereRateVariance index out of bounds for {}: idx={}, "
                          "state_size={}, cov={}x{}",
                          satellite, idx, state_vector_.size(), covariance_.rows(), covariance_.cols());
            return;
        }

        // Refuse to disturb a frozen pivot row (held at (0,0) by PivotManager).
        const double current_diag = covariance_(idx, idx);
        if (!(current_diag > 0.0)) {
            logger->warn("reinflateIonosphereRateVariance refused on frozen pivot {} "
                          "(idx={}, P_diag={:.3e}); BSSD invariant preserved",
                          satellite, idx, current_diag);
            return;
        }

        if (!(new_variance > 0.0)) {
            const double ir_std = config_.getInitialStd().ionosphere_rate;
            new_variance = ir_std * ir_std;
        }

        const double prev_value = state_vector_(idx);
        const double prev_diag  = current_diag;
        const double applied =
            reinflateDiag_(idx, current_diag, new_variance, Reinflation::IonosphereRate);
        // Value preserved: the physical slant-iono rate is continuous across a
        // phase-lock loss (the ionosphere keeps ramping), just as the level
        // value is preserved; only the cross-covariance partition is broken so
        // the filter re-partitions cleanly over the next several epochs.

        logger->info("Reinflated iono-rate state for {} at index {}: "
                      "value={:+.5f} m/s (unchanged), std {:.4f} -> {:.4f} m/s ({})",
                      satellite, idx, prev_value,
                      std::sqrt(std::max(prev_diag, 0.0)), std::sqrt(applied),
                      applied > new_variance ? "kept" : "raised");
    }

    void StateManager::reinflateCodeBiasVariance(const std::string& key,
                                                 double new_variance) {
        auto logger = log_manager_.getLogger("state_manager");

        const auto it = code_bias_indices_.find(key);
        if (it == code_bias_indices_.end()) {
            logger->debug("reinflateCodeBiasVariance: no code-bias state for {}", key);
            return;
        }
        const auto idx = static_cast<Eigen::Index>(it->second);
        if (idx >= state_vector_.size() || idx >= covariance_.rows()) {
            logger->error("reinflateCodeBiasVariance index out of bounds for {}: idx={}, "
                          "state_size={}, cov={}x{}",
                          key, idx, state_vector_.size(), covariance_.rows(), covariance_.cols());
            return;
        }

        // Refuse to disturb a frozen row.  The undifferenced network consumer
        // has no frozen pivot, and GLONASS per-slot IFB columns are never
        // targeted by the code-bias path (GLONASS carries no satellite code
        // bias in the network); this guard defends against a future caller that
        // forgets, mirroring reinflateIonosphereVariance.
        const double current_diag = covariance_(idx, idx);
        if (!(current_diag > 0.0)) {
            logger->warn("reinflateCodeBiasVariance refused on frozen state {} "
                         "(idx={}, P_diag={:.3e}); invariant preserved",
                         key, idx, current_diag);
            return;
        }

        if (!(new_variance > 0.0)) {
            const double cb_std = config_.getInitialStd().code_bias;
            logger->warn("reinflateCodeBiasVariance: non-positive variance {} for {}; "
                         "falling back to initial_std.code_bias² = {}",
                         new_variance, key, cb_std * cb_std);
            new_variance = cb_std * cb_std;
        }

        const double prev_value = state_vector_(idx);
        const double prev_diag  = current_diag;

        const double applied =
            reinflateDiag_(idx, current_diag, new_variance, Reinflation::CodeBias);
        // The value is kept: the receiver hardware delay is unchanged; only the
        // satellite-bias gauge it is referenced to has stepped.  Breaking the
        // (code-bias, rest) covariance partition lets the next update absorb the
        // common-mode step without a multi-epoch transient.

        logger->info("Reinflated code-bias state {} at index {}: "
                     "value={:+.4f} m (unchanged), std {:.4f} -> {:.4f} m ({})",
                     key, idx, prev_value,
                     std::sqrt(std::max(prev_diag, 0.0)), std::sqrt(applied),
                     applied > new_variance ? "kept: the current variance is larger, a "
                                              "re-inflation never shrinks"
                                            : "raised");
    }

    void StateManager::reinflateSystemTimeOffsetVariance(const std::string& system_name,
                                                         double new_variance) {
        auto logger = log_manager_.getLogger("state_manager");

        const size_t oi = getSystemTimeOffsetIndex(system_name);
        if (oi == std::numeric_limits<size_t>::max()) {
            logger->debug("reinflateSystemTimeOffsetVariance: no ISB state for {}",
                          system_name);
            return;
        }
        const auto idx = static_cast<Eigen::Index>(oi);
        if (idx >= state_vector_.size() || idx >= covariance_.rows()) {
            logger->error("reinflateSystemTimeOffsetVariance index out of bounds for {}: "
                          "idx={}, state_size={}, cov={}x{}",
                          system_name, idx, state_vector_.size(), covariance_.rows(),
                          covariance_.cols());
            return;
        }

        // Refuse to disturb a frozen row, mirroring reinflateCodeBiasVariance.
        const double current_diag = covariance_(idx, idx);
        if (!(current_diag > 0.0)) {
            logger->warn("reinflateSystemTimeOffsetVariance refused on frozen state ISB_{} "
                         "(idx={}, P_diag={:.3e}); invariant preserved",
                         system_name, idx, current_diag);
            return;
        }

        if (!(new_variance > 0.0)) {
            const double isb_std = config_.getInitialStd().system_time_offset;
            logger->warn("reinflateSystemTimeOffsetVariance: non-positive variance {} for {}; "
                         "falling back to initial_std.system_time_offset² = {}",
                         new_variance, system_name, isb_std * isb_std);
            new_variance = isb_std * isb_std;
        }

        const double prev_value = state_vector_(idx);
        const double prev_diag  = current_diag;

        const double applied =
            reinflateDiag_(idx, current_diag, new_variance, Reinflation::SystemTimeOffset);
        // The value is kept: the receiver inter-system hardware offset is
        // unchanged; only the constellation clock gauge it is referenced to has
        // stepped.  Breaking the (ISB, rest) partition lets the next update
        // absorb the differential datum step in one epoch.

        logger->info("Reinflated system-time-offset state ISB_{} at index {}: "
                     "value={:+.4f} m (unchanged), std {:.4f} -> {:.4f} m ({})",
                     system_name, idx, prev_value,
                     std::sqrt(std::max(prev_diag, 0.0)), std::sqrt(applied),
                     applied > new_variance ? "kept: the current variance is larger, a "
                                              "re-inflation never shrinks"
                                            : "raised");
    }

    // =================================================================
    // Shared re-inflation rule and its counters
    // =================================================================

    double StateManager::reinflateDiag_(Eigen::Index idx, double current_diag,
                                        double requested, Reinflation kind) {
        const double applied = std::max(current_diag, requested);
        covariance_.row(idx).setZero();
        covariance_.col(idx).setZero();
        covariance_(idx, idx) = applied;
        ReinflateCounter& c = reinflate_counters_[static_cast<std::size_t>(kind)];
        if (current_diag > requested) ++c.kept;
        else                          ++c.raised;
        return applied;
    }

    std::string StateManager::reinflateSummary(const ReinflateCounters& counters) {
        std::string s;
        for (std::size_t k = 0; k < counters.size(); ++k) {
            s += fmt::format("{}{} raised={} kept={}", s.empty() ? "" : " | ",
                             kReinflationNames[k],
                             counters[k].raised, counters[k].kept);
        }
        return s;
    }

    void StateManager::setIonosphereElevation(const std::string& satellite, double elevation_deg) {
        ionosphere_elevations_[satellite] = elevation_deg;
    }

    const std::map<std::string, double>& StateManager::getIonosphereElevations() const {
        return ionosphere_elevations_;
    }

    void StateManager::setIonosphereAzimuth(const std::string& satellite, double azimuth_deg) {
        ionosphere_azimuths_[satellite] = azimuth_deg;
    }

    const std::map<std::string, double>& StateManager::getIonosphereAzimuths() const {
        return ionosphere_azimuths_;
    }

    void StateManager::setFrozenPivotSatellites(const std::set<std::string>& sats) {
        frozen_pivot_sats_ = sats;
    }

    void StateManager::setAmbiguityProcessNoiseOverrides(
        const std::map<std::string, double>& overrides) {
        amb_q_overrides_ = overrides;
    }

    void StateManager::setIonoProcessNoiseOverrides(
        const std::map<std::string, double>& overrides) {
        iono_q_overrides_ = overrides;
    }

    const std::set<std::string>& StateManager::getFrozenPivotSatellites() const {
        return frozen_pivot_sats_;
    }

    // ===================================================================
    // Receiver code bias (inter-frequency bias) state management
    // ===================================================================

    int StateManager::getCodeBiasIndex(const std::string& key) const {
        auto it = code_bias_indices_.find(key);
        return it != code_bias_indices_.end() ? static_cast<int>(it->second) : -1;
    }

    const std::map<std::string, size_t>& StateManager::getCodeBiasStates() const {
        return code_bias_indices_;
    }

    size_t StateManager::addCodeBiasState(const std::string& key, double initial_value) {
        auto logger = log_manager_.getLogger("state_manager");
        if (code_bias_indices_.count(key)) {
            logger->trace("Code bias state for {} already exists at index {}", key, code_bias_indices_[key]);
            return code_bias_indices_[key];
        }

        const double cb_std = config_.getInitialStd().code_bias;
        const size_t index = appendState(initial_value, cb_std * cb_std);
        code_bias_indices_[key] = index;

        logger->info("Added code bias state for {} at index {}, initial={:.3f} m, std={:.1f} m",
            key, index, initial_value, cb_std);
        return index;
    }

    void StateManager::pruneCodeBiasStatesExcept(const std::set<std::string>& keep_keys) {
        auto logger = log_manager_.getLogger("state_manager");
        if (code_bias_indices_.empty()) return;

        std::vector<std::string> to_remove;
        for (const auto& [key, idx] : code_bias_indices_)
            if (keep_keys.find(key) == keep_keys.end())
                to_remove.push_back(key);
        if (to_remove.empty()) return;

        logger->info("Pruning {} code bias states (keeping {})", to_remove.size(), keep_keys.size());
        for (const auto& key : to_remove) {
            auto it = code_bias_indices_.find(key);
            if (it == code_bias_indices_.end()) continue;

            const size_t rem_idx = it->second;
            if (rem_idx < core_state_size_ || getStateSize() <= core_state_size_) {
                logger->warn("pruneCodeBiasStatesExcept: invalid index {} for {}", rem_idx, key);
                continue;
            }

            removeStateAtIndex(rem_idx);
            code_bias_indices_.erase(key);

            logger->debug("Removed code bias state for {} at former index {}", key, rem_idx);
        }
    }

    // ===================================================================
    // Per-satellite signal bias state management
    // ===================================================================

    int StateManager::getSatBiasIndex(const std::string& key) const {
        auto it = sat_bias_indices_.find(key);
        return it != sat_bias_indices_.end() ? static_cast<int>(it->second) : -1;
    }

    const std::map<std::string, size_t>& StateManager::getSatBiasStates() const {
        return sat_bias_indices_;
    }

    size_t StateManager::addSatBiasState(const std::string& key, double initial_value) {
        const double sb_std = config_.getInitialStd().sat_bias;
        return addSatBiasStateWithVariance(key, initial_value, sb_std * sb_std);
    }

    size_t StateManager::addSatBiasStateWithVariance(const std::string& key,
                                                     double initial_value,
                                                     double initial_variance) {
        auto logger = log_manager_.getLogger("state_manager");
        if (sat_bias_indices_.count(key)) {
            logger->trace("Sat bias state for {} already exists at index {}", key, sat_bias_indices_[key]);
            return sat_bias_indices_[key];
        }
        if (!(initial_variance > 0.0)) {
            const double sb_std = config_.getInitialStd().sat_bias;
            logger->warn("addSatBiasStateWithVariance: non-positive variance {} for {}; "
                         "falling back to initial_std.sat_bias² = {}",
                         initial_variance, key, sb_std * sb_std);
            initial_variance = sb_std * sb_std;
        }

        const size_t index = appendState(initial_value, initial_variance);
        sat_bias_indices_[key] = index;

        logger->info("Added sat bias state for {} at index {}, initial={:.3f} m, std={:.3f} m",
            key, index, initial_value, std::sqrt(initial_variance));
        return index;
    }

    void StateManager::resetSatBiasState(const std::string& key,
                                          double new_value,
                                          double new_variance) {
        auto logger = log_manager_.getLogger("state_manager");

        const auto it = sat_bias_indices_.find(key);
        if (it == sat_bias_indices_.end()) {
            logger->debug("resetSatBiasState: unknown key {}", key);
            return;
        }
        const auto idx = static_cast<Eigen::Index>(it->second);
        if (idx >= state_vector_.size() || idx >= covariance_.rows()) {
            logger->error("resetSatBiasState index out of bounds for key {}: idx={}, state_size={}, cov={}x{}",
                key, idx, state_vector_.size(), covariance_.rows(), covariance_.cols());
            return;
        }

        // A frozen pivot row is left alone.  PivotManager holds the BSSD
        // pivot's sat-bias at (value=0, variance=0); re-inflating its
        // variance would let it drift away from 0 on the next update and
        // break the BSSD frame for every other satellite in the
        // constellation/band group.  Callers already screen pivots through
        // getFrozenPivotSatellites(); this guard covers a call site that
        // does not.
        const double current_diag = covariance_(idx, idx);
        if (!(current_diag > 0.0)) {
            logger->warn("resetSatBiasState refused on frozen pivot row {} "
                          "(idx={}, P_diag={:.3e}); BSSD invariant preserved",
                          key, idx, current_diag);
            return;
        }

        if (!(new_variance > 0.0)) {
            const double sb_std = config_.getInitialStd().sat_bias;
            logger->warn("resetSatBiasState: non-positive variance {} for {}; "
                          "falling back to initial_std.sat_bias² = {}",
                          new_variance, key, sb_std * sb_std);
            new_variance = sb_std * sb_std;
        }

        const double prev_value = state_vector_(idx);
        const double prev_diag  = current_diag;

        // Same operation as resetAmbiguity: decorrelate (zero row and
        // column) and seed a fresh diagonal.  The off-diagonals are
        // cleared because the prior covariance assumed the bias state
        // was correct; after a reset that assumption does not hold, and
        // the cross-correlations to position, clock and other biases
        // would mislead the next update.
        covariance_.row(idx).setZero();
        covariance_.col(idx).setZero();
        covariance_(idx, idx) = new_variance;
        state_vector_(idx)    = new_value;

        logger->info("Reset sat bias state {} at index {}: "
                      "value {:+.4f} -> {:+.4f} m, std {:.4f} -> {:.4f} m",
                      key, idx,
                      prev_value, new_value,
                      std::sqrt(std::max(prev_diag, 0.0)),
                      std::sqrt(new_variance));
    }

    void StateManager::reinflateSatBiasVariancesForSat(const std::string& satellite,
                                                         double new_variance) {
        auto logger = log_manager_.getLogger("state_manager");

        if (sat_bias_indices_.empty()) return;

        if (!(new_variance > 0.0)) {
            const double sb_std = config_.getInitialStd().sat_bias;
            logger->warn("reinflateSatBiasVariancesForSat: non-positive variance {} for {}; "
                          "falling back to initial_std.sat_bias² = {}",
                          new_variance, satellite, sb_std * sb_std);
            new_variance = sb_std * sb_std;
        }

        // Walk the key map.  The keys are "satellite|signal", so a
        // prefix match needs no parsing.  Pivots are skipped by the
        // per-state diag==0 guard below, the same mechanism
        // resetSatBiasState uses.
        const std::string prefix = satellite + "|";
        std::vector<std::pair<std::string, Eigen::Index>> touched;
        std::vector<std::string> frozen;
        long long n_kept = 0;   // states whose larger variance was kept
        for (const auto& [key, raw_idx] : sat_bias_indices_) {
            if (key.compare(0, prefix.size(), prefix) != 0) continue;
            const auto idx = static_cast<Eigen::Index>(raw_idx);
            if (idx >= state_vector_.size() || idx >= covariance_.rows()) {
                logger->error("reinflateSatBiasVariancesForSat: idx {} out of bounds for {}",
                               idx, key);
                continue;
            }
            const double current_diag = covariance_(idx, idx);
            if (!(current_diag > 0.0)) {
                // Frozen pivot row, left alone: the BSSD pivot bias is
                // held at (0, 0), and re-inflating it would let it drift
                // on the next update and break the BSSD frame for the
                // whole constellation/band group.
                frozen.push_back(key);
                continue;
            }
            if (reinflateDiag_(idx, current_diag, new_variance, Reinflation::SatBias)
                > new_variance)
                ++n_kept;
            // The value is kept: the hardware delay is constant.
            touched.emplace_back(key, idx);
        }

        if (!touched.empty()) {
            std::string joined;
            for (size_t i = 0; i < touched.size(); ++i) {
                if (i) joined += ", ";
                joined += touched[i].first;
            }
            logger->info("Reinflated {} sat-bias state(s) for {} to std>={:.4f} m ({} kept a "
                          "larger variance): {}",
                          touched.size(), satellite,
                          std::sqrt(new_variance), n_kept, joined);
        }
        if (!frozen.empty()) {
            logger->debug("reinflateSatBiasVariancesForSat: skipped {} frozen pivot row(s) for {}",
                           frozen.size(), satellite);
        }
    }

    void StateManager::pruneSatBiasStatesExcept(const std::set<std::string>& keep_keys) {
        auto logger = log_manager_.getLogger("state_manager");
        // Always run an eviction sweep so cache entries don't accumulate
        // indefinitely on satellites that never re-acquire.  Cheap when
        // the cache is small and a no-op when it's empty.
        evictExpiredSatBiasCache(state_epoch_time_);

        if (sat_bias_indices_.empty()) return;

        std::vector<std::string> to_remove;
        for (const auto& [key, idx] : sat_bias_indices_)
            if (keep_keys.find(key) == keep_keys.end())
                to_remove.push_back(key);
        if (to_remove.empty()) return;

        logger->info("Pruning {} sat bias states (keeping {}; cache holds {} stale entries pre-prune)",
            to_remove.size(), keep_keys.size(), sat_bias_cache_.size());
        for (const auto& key : to_remove) {
            auto it = sat_bias_indices_.find(key);
            if (it == sat_bias_indices_.end()) continue;

            const size_t rem_idx = it->second;
            if (rem_idx < core_state_size_ || getStateSize() <= core_state_size_) {
                logger->warn("pruneSatBiasStatesExcept: invalid index {} for {}", rem_idx, key);
                continue;
            }

            // Cache the value and diagonal variance before removal so a
            // re-acquisition soon after can warm-restart instead of
            // re-seeding cold.  An existing cache entry for the same key
            // is overwritten: the most recent drop holds the newer, better
            // estimate.
            const double cached_value    = state_vector_(static_cast<Eigen::Index>(rem_idx));
            const double cached_variance = covariance_(static_cast<Eigen::Index>(rem_idx),
                                                       static_cast<Eigen::Index>(rem_idx));
            sat_bias_cache_[key] = SatBiasCacheEntry{
                cached_value, cached_variance, state_epoch_time_
            };

            removeStateAtIndex(rem_idx);
            sat_bias_indices_.erase(key);

            logger->debug("Removed sat bias state for {} at former index {} "
                          "(cached value={:+.4f} m, std={:.4f} m for potential restore)",
                          key, rem_idx, cached_value, std::sqrt(std::max(cached_variance, 0.0)));
        }
    }

    // ===================================================================
    // Satellite phase-bias states (PPP_RTK_Network producer)
    // -------------------------------------------------------------------
    // Counterpart of the satellite code-bias block above, on the separate
    // sat_phase_bias_indices_ map.  The code-bias warm-restart cache has no
    // phase-bias counterpart (deferred to W4 with the network estimator's
    // restart semantics); prune removes the state without caching it.
    // ===================================================================

    int StateManager::getSatPhaseBiasIndex(const std::string& key) const {
        auto it = sat_phase_bias_indices_.find(key);
        return it != sat_phase_bias_indices_.end() ? static_cast<int>(it->second) : -1;
    }

    const std::map<std::string, size_t>& StateManager::getSatPhaseBiasStates() const {
        return sat_phase_bias_indices_;
    }

    size_t StateManager::addSatPhaseBiasState(const std::string& key, double initial_value) {
        const double spb_std = config_.getInitialStd().sat_phase_bias;
        return addSatPhaseBiasStateWithVariance(key, initial_value, spb_std * spb_std);
    }

    size_t StateManager::addSatPhaseBiasStateWithVariance(const std::string& key,
                                                          double initial_value,
                                                          double initial_variance) {
        auto logger = log_manager_.getLogger("state_manager");
        if (sat_phase_bias_indices_.count(key)) {
            logger->trace("Sat phase-bias state for {} already exists at index {}",
                          key, sat_phase_bias_indices_[key]);
            return sat_phase_bias_indices_[key];
        }
        if (!(initial_variance > 0.0)) {
            const double spb_std = config_.getInitialStd().sat_phase_bias;
            logger->warn("addSatPhaseBiasStateWithVariance: non-positive variance {} for {}; "
                         "falling back to initial_std.sat_phase_bias^2 = {}",
                         initial_variance, key, spb_std * spb_std);
            initial_variance = spb_std * spb_std;
        }

        const size_t index = appendState(initial_value, initial_variance);
        sat_phase_bias_indices_[key] = index;

        logger->info("Added sat phase-bias state for {} at index {}, initial={:.3f} m, std={:.3f} m",
            key, index, initial_value, std::sqrt(initial_variance));
        return index;
    }

    void StateManager::reinflateSatPhaseBiasVariancesForSat(const std::string& satellite,
                                                            double new_variance) {
        auto logger = log_manager_.getLogger("state_manager");

        if (sat_phase_bias_indices_.empty()) return;

        if (!(new_variance > 0.0)) {
            const double spb_std = config_.getInitialStd().sat_phase_bias;
            logger->warn("reinflateSatPhaseBiasVariancesForSat: non-positive variance {} for {}; "
                          "falling back to initial_std.sat_phase_bias^2 = {}",
                          new_variance, satellite, spb_std * spb_std);
            new_variance = spb_std * spb_std;
        }

        // Keys are "<sat>|<signal>|PB"; prefix-match on "<sat>|" without parsing.
        const std::string prefix = satellite + "|";
        std::vector<std::pair<std::string, Eigen::Index>> touched;
        std::vector<std::string> frozen;
        long long n_kept = 0;   // states whose larger variance was kept
        for (const auto& [key, raw_idx] : sat_phase_bias_indices_) {
            if (key.compare(0, prefix.size(), prefix) != 0) continue;
            const auto idx = static_cast<Eigen::Index>(raw_idx);
            if (idx >= state_vector_.size() || idx >= covariance_.rows()) {
                logger->error("reinflateSatPhaseBiasVariancesForSat: idx {} out of bounds for {}",
                               idx, key);
                continue;
            }
            const double current_diag = covariance_(idx, idx);
            if (!(current_diag > 0.0)) {
                frozen.push_back(key);   // frozen pivot row: the datum is left alone
                continue;
            }
            if (reinflateDiag_(idx, current_diag, new_variance, Reinflation::SatPhaseBias)
                > new_variance)
                ++n_kept;
            // The value is kept: the phase hardware delay is constant.
            touched.emplace_back(key, idx);
        }

        if (!touched.empty()) {
            std::string joined;
            for (size_t i = 0; i < touched.size(); ++i) {
                if (i) joined += ", ";
                joined += touched[i].first;
            }
            logger->info("Reinflated {} sat-phase-bias state(s) for {} to std>={:.4f} m ({} kept "
                          "a larger variance): {}",
                          touched.size(), satellite,
                          std::sqrt(new_variance), n_kept, joined);
        }
        if (!frozen.empty()) {
            logger->debug("reinflateSatPhaseBiasVariancesForSat: skipped {} frozen pivot row(s) for {}",
                           frozen.size(), satellite);
        }
    }

    void StateManager::pruneSatPhaseBiasStatesExcept(const std::set<std::string>& keep_keys) {
        auto logger = log_manager_.getLogger("state_manager");
        if (sat_phase_bias_indices_.empty()) return;

        std::vector<std::string> to_remove;
        for (const auto& [key, idx] : sat_phase_bias_indices_)
            if (keep_keys.find(key) == keep_keys.end())
                to_remove.push_back(key);
        if (to_remove.empty()) return;

        logger->info("Pruning {} sat phase-bias states (keeping {})",
            to_remove.size(), keep_keys.size());
        for (const auto& key : to_remove) {
            auto it = sat_phase_bias_indices_.find(key);
            if (it == sat_phase_bias_indices_.end()) continue;

            const size_t rem_idx = it->second;
            if (rem_idx < core_state_size_ || getStateSize() <= core_state_size_) {
                logger->warn("pruneSatPhaseBiasStatesExcept: invalid index {} for {}", rem_idx, key);
                continue;
            }

            // removeStateAtIndex decrements all higher indices across every
            // dynamic map (incl. sat_phase_bias_indices_), so the next key's
            // index is read from the map again on the following iteration.
            removeStateAtIndex(rem_idx);
            sat_phase_bias_indices_.erase(key);

            logger->debug("Removed sat phase-bias state for {} at former index {}", key, rem_idx);
        }
    }

    // ===================================================================
    // Receiver phase-bias states (network.phase_datum_eq34): the
    // per-(receiver, system, signal) split δ_{r,σ}, keyed "<sys>_RPB_<sig>".
    // Local and re-estimated each epoch (no process noise, no carry).
    // ===================================================================

    int StateManager::getReceiverPhaseBiasIndex(const std::string& key) const {
        auto it = receiver_phase_bias_indices_.find(key);
        return it != receiver_phase_bias_indices_.end() ? static_cast<int>(it->second) : -1;
    }

    const std::map<std::string, size_t>& StateManager::getReceiverPhaseBiasStates() const {
        return receiver_phase_bias_indices_;
    }

    size_t StateManager::addReceiverPhaseBiasState(const std::string& key, double initial_value) {
        auto logger = log_manager_.getLogger("state_manager");
        if (receiver_phase_bias_indices_.count(key)) {
            logger->trace("Receiver phase-bias state for {} already exists at index {}",
                          key, receiver_phase_bias_indices_[key]);
            return receiver_phase_bias_indices_[key];
        }
        const double rpb_std = config_.getInitialStd().sat_phase_bias;
        const size_t index = appendState(initial_value, rpb_std * rpb_std);
        receiver_phase_bias_indices_[key] = index;
        logger->debug("Added receiver phase-bias state for {} at index {}, initial={:.3f} m",
                      key, index, initial_value);
        return index;
    }

    void StateManager::pruneReceiverPhaseBiasStatesExcept(const std::set<std::string>& keep_keys) {
        auto logger = log_manager_.getLogger("state_manager");
        if (receiver_phase_bias_indices_.empty()) return;

        std::vector<std::string> to_remove;
        for (const auto& [key, idx] : receiver_phase_bias_indices_)
            if (keep_keys.find(key) == keep_keys.end())
                to_remove.push_back(key);
        if (to_remove.empty()) return;

        for (const auto& key : to_remove) {
            auto it = receiver_phase_bias_indices_.find(key);
            if (it == receiver_phase_bias_indices_.end()) continue;

            const size_t rem_idx = it->second;
            if (rem_idx < core_state_size_ || getStateSize() <= core_state_size_) {
                logger->warn("pruneReceiverPhaseBiasStatesExcept: invalid index {} for {}", rem_idx, key);
                continue;
            }
            // removeStateAtIndex decrements all higher indices across every
            // dynamic map (incl. receiver_phase_bias_indices_), so the next
            // key's index is read from the map again on the next iteration.
            removeStateAtIndex(rem_idx);
            receiver_phase_bias_indices_.erase(key);
            logger->trace("Removed receiver phase-bias state for {} at former index {}", key, rem_idx);
        }
    }

    // ===================================================================
    // Phase-colour states (network.phase_colour_state): one band-common,
    // phase-only Gauss-Markov nuisance per satellite, keyed by the bare satellite id.
    // The network carries the estimate; this map holds the column only.
    // ===================================================================

    int StateManager::getPhaseColourIndex(const std::string& satellite) const {
        auto it = phase_colour_indices_.find(satellite);
        return it != phase_colour_indices_.end() ? static_cast<int>(it->second) : -1;
    }

    const std::map<std::string, size_t>& StateManager::getPhaseColourStates() const {
        return phase_colour_indices_;
    }

    size_t StateManager::addPhaseColourState(const std::string& satellite, double initial_var_m2) {
        auto it = phase_colour_indices_.find(satellite);
        if (it != phase_colour_indices_.end()) return it->second;
        const size_t index = appendState(0.0, initial_var_m2);
        phase_colour_indices_[satellite] = index;
        log_manager_.getLogger("state_manager")->trace(
            "Added phase-colour state for {} at index {}", satellite, index);
        return index;
    }

    void StateManager::prunePhaseColourStatesExcept(const std::set<std::string>& keep_sats) {
        if (phase_colour_indices_.empty()) return;
        std::vector<std::string> to_remove;
        for (const auto& [sat, idx] : phase_colour_indices_)
            if (!keep_sats.count(sat)) to_remove.push_back(sat);
        for (const auto& sat : to_remove) {
            auto it = phase_colour_indices_.find(sat);
            if (it == phase_colour_indices_.end()) continue;
            const size_t rem_idx = it->second;
            if (rem_idx < core_state_size_ || getStateSize() <= core_state_size_) continue;
            // removeStateAtIndex decrements every higher index in every dynamic map
            // (this one included), so the next satellite's index is re-read from the map.
            removeStateAtIndex(rem_idx);
            phase_colour_indices_.erase(sat);
            phase_colour_elevations_.erase(sat);
        }
    }

    void StateManager::setPhaseColourElevation(const std::string& satellite, double elevation_deg) {
        phase_colour_elevations_[satellite] = elevation_deg;
    }

    double StateManager::getPhaseColourElevation(const std::string& satellite) const {
        auto it = phase_colour_elevations_.find(satellite);
        return it != phase_colour_elevations_.end() ? it->second
                                                     : std::numeric_limits<double>::quiet_NaN();
    }

    // ===================================================================
    // Satellite clock states (PPP_RTK_Network producer): counterpart of the
    // satellite phase-bias block above, one per satellite, keyed "<sat>|SC".
    // ===================================================================

    int StateManager::getSatClockIndex(const std::string& key) const {
        auto it = sat_clock_indices_.find(key);
        return it != sat_clock_indices_.end() ? static_cast<int>(it->second) : -1;
    }

    const std::map<std::string, size_t>& StateManager::getSatClockStates() const {
        return sat_clock_indices_;
    }

    size_t StateManager::addSatClockState(const std::string& key, double initial_value) {
        const double sc_std = config_.getInitialStd().sat_clock;
        return addSatClockStateWithVariance(key, initial_value, sc_std * sc_std);
    }

    size_t StateManager::addSatClockStateWithVariance(const std::string& key,
                                                      double initial_value,
                                                      double initial_variance) {
        auto logger = log_manager_.getLogger("state_manager");
        if (sat_clock_indices_.count(key)) {
            logger->trace("Sat clock state for {} already exists at index {}",
                          key, sat_clock_indices_[key]);
            return sat_clock_indices_[key];
        }
        if (!(initial_variance > 0.0)) {
            const double sc_std = config_.getInitialStd().sat_clock;
            logger->warn("addSatClockStateWithVariance: non-positive variance {} for {}; "
                         "falling back to initial_std.sat_clock^2 = {}",
                         initial_variance, key, sc_std * sc_std);
            initial_variance = sc_std * sc_std;
        }

        const size_t index = appendState(initial_value, initial_variance);
        sat_clock_indices_[key] = index;

        logger->info("Added sat clock state for {} at index {}, initial={:.3f} m, std={:.3f} m",
            key, index, initial_value, std::sqrt(initial_variance));
        return index;
    }

    void StateManager::reinflateSatClockVarianceForSat(const std::string& satellite,
                                                       double new_variance) {
        auto logger = log_manager_.getLogger("state_manager");
        if (sat_clock_indices_.empty()) return;

        const std::string key = StateKeys::makeSatClockKey(satellite);
        const auto it = sat_clock_indices_.find(key);
        if (it == sat_clock_indices_.end()) return;

        if (!(new_variance > 0.0)) {
            const double sc_std = config_.getInitialStd().sat_clock;
            logger->warn("reinflateSatClockVarianceForSat: non-positive variance {} for {}; "
                          "falling back to initial_std.sat_clock^2 = {}",
                          new_variance, satellite, sc_std * sc_std);
            new_variance = sc_std * sc_std;
        }

        const auto idx = static_cast<Eigen::Index>(it->second);
        if (idx >= state_vector_.size() || idx >= covariance_.rows()) {
            logger->error("reinflateSatClockVarianceForSat: idx {} out of bounds for {}", idx, key);
            return;
        }
        const double current_diag = covariance_(idx, idx);
        if (!(current_diag > 0.0)) {   // frozen pivot row: the datum is left alone
            logger->debug("reinflateSatClockVarianceForSat: skipped frozen pivot row {}", key);
            return;
        }
        const double applied =
            reinflateDiag_(idx, current_diag, new_variance, Reinflation::SatClock);
        // The value is kept so the clock estimate stays continuous.
        logger->info("Reinflated sat-clock state {} to std={:.4f} m ({})", key,
                     std::sqrt(applied), applied > new_variance ? "kept" : "raised");
    }

    void StateManager::pruneSatClockStatesExcept(const std::set<std::string>& keep_keys) {
        auto logger = log_manager_.getLogger("state_manager");
        if (sat_clock_indices_.empty()) return;

        std::vector<std::string> to_remove;
        for (const auto& [key, idx] : sat_clock_indices_)
            if (keep_keys.find(key) == keep_keys.end())
                to_remove.push_back(key);
        if (to_remove.empty()) return;

        logger->info("Pruning {} sat clock states (keeping {})",
            to_remove.size(), keep_keys.size());
        for (const auto& key : to_remove) {
            auto it = sat_clock_indices_.find(key);
            if (it == sat_clock_indices_.end()) continue;

            const size_t rem_idx = it->second;
            if (rem_idx < core_state_size_ || getStateSize() <= core_state_size_) {
                logger->warn("pruneSatClockStatesExcept: invalid index {} for {}", rem_idx, key);
                continue;
            }

            // removeStateAtIndex decrements all higher indices across every
            // dynamic map (incl. sat_clock_indices_), so the next key's index is
            // read from the map again on the following iteration.
            removeStateAtIndex(rem_idx);
            sat_clock_indices_.erase(key);

            logger->debug("Removed sat clock state for {} at former index {}", key, rem_idx);
        }
    }

    namespace {
        /**
         * @brief Seconds elapsed from @p earlier to @p later.
         *
         * Works across GPS-week rollovers because GnssTime carries the
         * absolute GPS week.  A negative result means @p later precedes
         * @p earlier; callers treat that as a malformed gap (clamped to
         * zero, or evicted from the cache).
         */
        double gnssTimeDiffSeconds(const GnssTime& later, const GnssTime& earlier) {
            return static_cast<double>(later.week - earlier.week)
                       * static_cast<double>(TimeUtils::SECONDS_IN_WEEK)
                 + (later.tow - earlier.tow);
        }
    }

    std::optional<StateManager::SatBiasRestoration>
    StateManager::tryRestoreSatBias(const std::string& key, const GnssTime& now) {
        auto it = sat_bias_cache_.find(key);
        if (it == sat_bias_cache_.end()) return std::nullopt;

        const double gap_s = std::max(0.0, gnssTimeDiffSeconds(now, it->second.drop_time));
        const double warm_age = config_.getSatBiasCacheWarmAgeS();
        const double max_age  = config_.getSatBiasCacheMaxAgeS();

        if (gap_s > max_age) {
            // The entry is too old to use for either value or variance;
            // it is evicted and the caller falls back to a cold WLS seed.
            sat_bias_cache_.erase(it);
            return std::nullopt;
        }

        SatBiasRestoration r{};
        r.value  = it->second.value;
        r.gap_s  = gap_s;
        if (gap_s <= warm_age) {
            // Warm restart: the cached variance is aged forward by the
            // configured random-walk PSD (pn.sat_bias · gap_s), so it
            // reflects the uncertainty growth over the outage.
            const double q = config_.getProcessNoise().sat_bias;
            r.variance = it->second.variance + std::max(0.0, q) * gap_s;
            r.is_warm  = true;
        } else {
            // Cold-value restart: after a gap longer than the warm age the
            // cached variance is not used.  The cached value is reused with
            // the cold-start initial variance, which is better than a
            // single-epoch WLS seed when the value is correct.
            const double sb_std = config_.getInitialStd().sat_bias;
            r.variance = sb_std * sb_std;
            r.is_warm  = false;
        }
        return r;
    }

    void StateManager::consumeSatBiasCacheEntry(const std::string& key) {
        sat_bias_cache_.erase(key);
    }

    void StateManager::evictExpiredSatBiasCache(const GnssTime& now) {
        const double max_age = config_.getSatBiasCacheMaxAgeS();
        if (sat_bias_cache_.empty()) return;
        for (auto it = sat_bias_cache_.begin(); it != sat_bias_cache_.end(); ) {
            const double gap_s = gnssTimeDiffSeconds(now, it->second.drop_time);
            if (gap_s > max_age || gap_s < 0.0) {
                it = sat_bias_cache_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ===================================================================
    // Unresolvable ambiguity support
    // ===================================================================

    void StateManager::markAmbiguityUnresolvable(const std::string& amb_key) {
        unresolvable_ambiguities_.insert(amb_key);
    }

    bool StateManager::isAmbiguityUnresolvable(const std::string& amb_key) const {
        return unresolvable_ambiguities_.count(amb_key) > 0;
    }

    void StateManager::clearUnresolvableAmbiguities() {
        unresolvable_ambiguities_.clear();
    }

    bool StateManager::isAmbiguityPendingReseed(const std::string& amb_key) const {
        return pending_reseed_ambiguities_.count(amb_key) > 0;
    }

    void StateManager::clearPendingReseed(const std::string& amb_key) {
        pending_reseed_ambiguities_.erase(amb_key);
    }

    void StateManager::markAmbiguityPendingReseed(const std::string& amb_key) {
        pending_reseed_ambiguities_.insert(amb_key);
        noteAmbiguityDiscontinuity(amb_key);
    }

    // ===================================================================
    // Gap-recovery helpers
    // ===================================================================

    namespace {
        // Collect the indices of the families a gap decouples
        // (StateFamilySpec::gap_decouple: iono, iono rate, ambiguity and sat
        // bias), in table order.  A gap invalidates their cross-covariance with
        // position and clock (different geometry, different multipath, possibly
        // a different satellite set).  The receiver code bias is excluded: it is
        // receiver hardware, nearly constant, and its cross-covariance to
        // position stays physically meaningful.
        std::vector<Eigen::Index>
        collectSatSpecificIndices(const StateManager& sm) {
            std::vector<Eigen::Index> out;
            std::size_t n = 0;
            for (const auto& spec : kStateFamilies)
                if (spec.gap_decouple) n += sm.indicesOf(spec.family).size();
            out.reserve(n);
            for (const auto& spec : kStateFamilies) {
                if (!spec.gap_decouple) continue;
                for (const auto& [_, idx] : sm.indicesOf(spec.family))
                    out.push_back(static_cast<Eigen::Index>(idx));
            }
            return out;
        }
    }

    void StateManager::zeroPositionCrossCovariancesToSatStates() {
        auto logger = log_manager_.getLogger("state_manager");
        const auto sat_idx = collectSatSpecificIndices(*this);
        if (sat_idx.empty()) {
            logger->trace("zeroPositionCrossCov: no sat-specific states; no-op");
            return;
        }
        const Eigen::Index pos = static_cast<Eigen::Index>(getPositionIndex());
        for (Eigen::Index k = 0; k < 3; ++k) {
            for (Eigen::Index j : sat_idx) {
                covariance_(pos + k, j) = 0.0;
                covariance_(j, pos + k) = 0.0;
            }
        }
        logger->info("Zeroed position cross-covariances to {} sat-specific states",
                     sat_idx.size());
    }

    void StateManager::zeroClockCrossCovariancesToSatStates() {
        auto logger = log_manager_.getLogger("state_manager");
        if (!hasClock()) {
            logger->trace("zeroClockCrossCov: no clock state (BSSD/baseline); no-op");
            return;
        }
        const auto sat_idx = collectSatSpecificIndices(*this);
        if (sat_idx.empty()) {
            logger->trace("zeroClockCrossCov: no sat-specific states; no-op");
            return;
        }
        const Eigen::Index clk = static_cast<Eigen::Index>(getClockIndex());
        for (Eigen::Index j : sat_idx) {
            covariance_(clk, j) = 0.0;
            covariance_(j, clk) = 0.0;
        }
        logger->info("Zeroed clock cross-covariances to {} sat-specific states",
                     sat_idx.size());
    }

    void StateManager::inflatePositionDiagonal(double sigma_m) {
        auto logger = log_manager_.getLogger("state_manager");
        if (!(std::isfinite(sigma_m) && sigma_m > 0.0)) {
            logger->warn("inflatePositionDiagonal: invalid sigma_m={}, skipping", sigma_m);
            return;
        }
        const double var_add = sigma_m * sigma_m;
        const Eigen::Index pos = static_cast<Eigen::Index>(getPositionIndex());
        for (Eigen::Index k = 0; k < 3; ++k) {
            covariance_(pos + k, pos + k) += var_add;
        }
        logger->info("Inflated position diagonal by sigma={:.3f} m (var+={:.3e} m²)",
                     sigma_m, var_add);
    }

    void StateManager::resetVelocityToLoose(double sigma_mps) {
        auto logger = log_manager_.getLogger("state_manager");
        if (!hasVelocity()) {
            logger->trace("resetVelocityToLoose: no velocity state; no-op");
            return;
        }
        if (!(std::isfinite(sigma_mps) && sigma_mps > 0.0)) {
            logger->warn("resetVelocityToLoose: invalid sigma_mps={}, skipping", sigma_mps);
            return;
        }
        const Eigen::Index vel = static_cast<Eigen::Index>(getVelocityIndex());
        const double var = sigma_mps * sigma_mps;
        for (Eigen::Index k = 0; k < 3; ++k) {
            // Zero the value and cross-covariances, then set the diagonal
            // (the covariance pattern of resetAmbiguity).
            state_vector_(vel + k) = 0.0;
            covariance_.row(vel + k).setZero();
            covariance_.col(vel + k).setZero();
            covariance_(vel + k, vel + k) = var;
        }
        logger->info("Reset velocity to 0 with sigma={:.3f} m/s (var={:.3e})",
                     sigma_mps, var);
    }

    void StateManager::clearSatBiasCache() {
        auto logger = log_manager_.getLogger("state_manager");
        const auto n = sat_bias_cache_.size();
        sat_bias_cache_.clear();
        if (n) logger->info("Cleared sat-bias cache ({} entries dropped)", n);
    }

} // namespace GnssProcessor
