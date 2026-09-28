#pragma once

/**
 * @file SoftLliBridge.hpp
 * @brief Shared soft-LLI gap-bridge orchestration for the post-processing
 *        (@c CycleSlipManager) and real-time (@c RealTimeEngine) slip paths.
 *
 * @par Motivation
 * A receiver-declared loss of lock (LLI) is normally authoritative: both slip
 * paths reset the satellite's phase ambiguities and erase the TurboEdit GF arc.
 * When the LLI gap is short and high-rate on an AR-capable dual-frequency
 * satellite, the @c TurboEditResolver gap bridge can recover the per-signal
 * integer slip across the gap, provided its pre-gap GF level survives and the
 * ambiguity is held instead of reset.  This helper owns the state and
 * mechanics for that, shared so the two paths cannot diverge:
 *
 *  1. A rolling "last good" ambiguity snapshot (value / variance), captured
 *     after the update each epoch for converged keys only: the pre-gap anchor.
 *  2. Soft-LLI qualification (toggle, bridge active, high-rate, AR-capable).
 *  3. Hold registration: snapshot the satellite's recently converged phase
 *     signals, mark them held-continuous in the resolver
 *     (@c TurboEditResolver::markHeldAcrossLli) so the bridge adjudicates a
 *     repair instead of a moot, and expose the held keys for KF measurement
 *     exclusion (the held ambiguities are still slipped and must not
 *     contaminate shared states during the MW one-epoch confirmation delay).
 *  4. Resolution: at the bridge's confirm epoch, apply a covariance-restoring
 *     @c snapshot + dN·λ repair and release the hold; otherwise fall back to
 *     a reset or a timeout.  The timeout is per held key: each signal gets the
 *     full cap from its own hold start, so a signal that drops late does not
 *     inherit the group's nearly spent timer.  The bridge can fire only
 *     because the caller preserves the GF arc (it skips its own
 *     @c resetSatellite for a held satellite).
 *
 * @par Caveat
 * A lossless repair beats a reset only when the carrier loop held phase
 * coherence across the LLI.  If it re-acquired, @c snapshot + dN·λ still
 * reconstructs the current integer ambiguity (dN absorbs the re-acquisition)
 * at the pre-gap precision, but a wrong unique integer solve puts at least
 * |dN·λ| into the KF phase innovation when the signal rejoins, and the reset
 * happens one epoch late.  Everything is gated by
 * @c estimation.cycle_slip.turbo_edit_bridge_lli (default off).  See
 * docs/turbo_edit_lli_bridge_design.md.
 */

#include "common/PppTypes.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "slip/TurboEditResolver.hpp"
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @class SoftLliBridge
 * @brief Owns the soft-LLI hold state + snapshot-restore mechanics shared by
 *        the post-processing and real-time cycle-slip paths.
 */
class SoftLliBridge {
public:
    /// Callback applying a normal (path-specific) ambiguity reset to
    /// @c amb_key: @c resetAmbiguityBssdAware in post-processing, a plain
    /// @c StateManager::resetAmbiguity in real-time.  Used for the timeout
    /// and restore-failure fallbacks so the helper stays path-agnostic.
    using ResetFn = std::function<void(const std::string& amb_key,
                                       const std::string& sat,
                                       const std::string& signal)>;

    SoftLliBridge(const GnssConfig& config, LogManager& log_manager);

    /// @brief Whether the soft-LLI bridge is enabled (toggle on).
    bool enabled() const;

    /// @brief Forward the effective observation sampling interval (s); 0 =
    ///        unknown (SBF/RTCM streams), in which case the measured spacing
    ///        to the previous epoch gates the high-rate requirement.
    void setSampleInterval(double interval_s) { sample_interval_s_ = interval_s; }

    /// @brief Roll the "last good" snapshot from the converged float posterior.
    ///        Call once per epoch after the KF update and before AR fix-and-hold.
    ///        Only keys below the converged-variance ceiling update, so a fresh
    ///        reset never overwrites the pre-gap anchor.  No-op unless enabled.
    void recordSnapshot(const StateManager& state_manager, double epoch_now_s);

    /// @brief Phase-measurement keys ("G03_L2W") currently held.  The caller
    ///        seeds the KF update's @c excluded_measurements with these.
    std::set<std::string> heldKeys() const;

    /// @brief True if there are no outstanding holds (skip the resolve pass).
    bool empty() const { return held_pending_bridge_.empty(); }

    /// @brief Whether a satellite's LLI is held for a bridge repair instead of
    ///        reset immediately.  Requires the bridge enabled, an interval of at
    ///        most 1 s (the declared interval when known, else the measured
    ///        spacing), and a non-GLONASS satellite.  The caller is responsible
    ///        for the BSSD-pivot exclusion (it owns the pivot map).
    bool qualifies(const std::string& sat_id, double epoch_now_s) const;

    /// @brief Register a soft-LLI hold: snapshot the satellite's recently
    ///        converged phase signals (including one that dropped just before
    ///        the LLI and is already pruned out of @p amb_keys), record them
    ///        for measurement exclusion, and mark them held-continuous in
    ///        @p resolver.  The caller must not reset @p resolver's arc for
    ///        this satellite, because the bridge needs the pre-gap level.
    void registerHold(const std::string& sat_id,
                      const std::set<std::string>& amb_keys,
                      TurboEditResolver& resolver,
                      double epoch_now_s,
                      const std::string& epoch_prefix);

    /// Predicate identifying a constellation's BSSD pivot satellite (its SD
    /// ambiguity is frozen at 0, so an in-frame integer repair is meaningless
    /// there).  Supplied by the caller, which owns the pivot map.
    using IsBssdPivotFn = std::function<bool(const std::string& sat_id)>;

    /**
     * @brief Register holds for phase signals that dropped this epoch with no
     *        receiver LLI: the per-signal-outage counterpart of
     *        @c registerHold.
     *
     * Without a hold, a signal absent from this epoch's observations has its
     * ambiguity state deleted by the measurement model's prune-on-first-miss
     * and reseeded on return from the post-gap carrier, which may be slipped.
     * That deletes the pre-gap state the TurboEdit gap bridge needs for a
     * lossless @c snapshot + dN·λ repair.  This call scans the current
     * ambiguity states for raw phase keys with a recent converged snapshot
     * that produced no observation this epoch, and holds them: they are
     * registered in @c heldKeys() (the caller's prune-protection and KF
     * measurement-exclusion source) and marked held-continuous in
     * @p resolver so the bridge adjudicates a repair instead of a moot.
     *
     * Per-key gates: raw phase signal, not already held, snapshot within the
     * bridge cap (a long-untracked or never-converged key falls through to
     * the normal prune), @c qualifies() (high-rate, AR-capable), and not the
     * constellation's BSSD pivot.  Resolution and the timeout fallback are
     * shared with the LLI path via @c resolveHolds.  No-op unless enabled.
     *
     * @param modeled_obs   This epoch's modeled observations.
     * @param state_manager Filter state (read-only; supplies the live keys).
     * @param is_bssd_pivot Pivot predicate; may be empty (no pivot exclusion).
     * @param resolver      Gap-bridge resolver to mark held-continuous in.
     * @param epoch_now_s   Continuous GPS seconds of this epoch.
     * @param epoch_prefix  Log prefix.
     */
    void registerGapHolds(const std::vector<PppObservation>& modeled_obs,
                          const StateManager& state_manager,
                          const IsBssdPivotFn& is_bssd_pivot,
                          TurboEditResolver& resolver,
                          double epoch_now_s,
                          const std::string& epoch_prefix);

    /// @brief Resolve outstanding holds against this epoch's bridge output:
    ///        apply @c snapshot + dN·λ integer repairs (covariance restored),
    ///        release on bridge reset, or reset on timeout.
    ///
    /// The timeout is per held key: each signal gets the full cap
    /// (@c turbo_edit_max_gap_s, extended by @c turbo_edit_wl_settle_max_s
    /// while the resolver reports the satellite pending) measured from its
    /// own hold start, so a signal that drops late into an existing outage
    /// does not inherit the group's earlier timer.  Bridge adjudication
    /// (repair / reset) stays satellite-granular, because the resolver emits
    /// per-satellite outcomes.  An expired key takes the path-specific reset
    /// and has its resolver state voided
    /// (@c TurboEditResolver::resetSatelliteSignal: the held-continuous
    /// assertion and the GF arcs that touch the signal).  Its integer
    /// continuity is broken, so a later bridged adjudication must not treat
    /// it as continuous.
    ///
    /// @param resolver  The gap-bridge resolver holding the held-continuous
    ///             marks; per-key timeouts void their signal's marks/arcs.
    /// @param[out] repaired_sats Satellites whose held ambiguities were
    ///             integer-repaired; the caller skips its own reset for them.
    /// @param[in,out] n_repaired,n_reset Counters incremented for reporting.
    /// @param reset_fn Path-specific reset for the timeout fallback.
    void resolveHolds(const TurboEditResolver::EpochResolution& te,
                      TurboEditResolver& resolver,
                      const std::vector<PppObservation>& modeled_obs,
                      StateManager& state_manager,
                      double epoch_now_s,
                      const std::string& epoch_prefix,
                      std::set<std::string>& repaired_sats,
                      int& n_repaired,
                      int& n_reset,
                      const ResetFn& reset_fn);

    /// @brief Advance the previous-epoch marker (measured-spacing gate source).
    ///        Call once per epoch that carried observations.
    void advanceEpoch(double epoch_now_s) {
        if (epoch_now_s > 0.0) prev_epoch_s_ = epoch_now_s;
    }

    /// @brief Drop all snapshot / hold state (hard restart).
    void reset();

private:
    const GnssConfig& config_;
    LogManager& log_manager_;
    PppUtils ppp_utils_;

    double sample_interval_s_{ 0.0 };
    double prev_epoch_s_{ 0.0 };

    struct AmbSnapshot { double value{ 0.0 }; double variance{ 0.0 }; double epoch_s{ 0.0 }; };
    std::map<std::string, AmbSnapshot> last_good_amb_;

    struct HeldBridge {
        std::string sat;
        std::string signal;
        double snap_value{ 0.0 };
        double snap_variance{ 0.0 };
        double hold_start_s{ 0.0 };
    };
    std::map<std::string, HeldBridge> held_pending_bridge_;

    /// Converged-variance ceiling (m²) for @c recordSnapshot (≈ std 10 m).
    static constexpr double kSnapshotMaxVariance = 100.0;

    static std::string signalOf(const std::string& amb_key);
};

} // namespace GnssProcessor
