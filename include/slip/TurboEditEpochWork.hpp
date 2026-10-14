#pragma once

/**
 * @file TurboEditEpochWork.hpp
 * @brief The working values one @c TurboEditResolver::resolve call threads
 *        through its steps, and the three small helpers those steps share.
 *
 * @c resolve is a short ordered driver over private step members (the table
 * in TurboEditResolver.hpp).  The sets those steps hand to one another were
 * function-scope locals of a single 1,120-line function; they are now the
 * members of two values:
 *   - @c TurboEditResolver::EpochWork lives for one call: the inputs, the
 *     resolution being built, the MW flag index, the pass-1 rows and pools,
 *     the IVI, and the integer staging of pass 2;
 *   - @c TurboEditResolver::RowWork lives for one pass-2 row: the row (by
 *     value, since the late re-route rewrites it), its GF arc, the evidence
 *     the adjudication reads, and the decision the trace row records.
 * Each step opens with an alias preamble (for example
 * @c const auto& pe = row.pe;) naming exactly the members it reads
 * (@c const) and writes, so its body reads the names it always had.
 *
 * Internal to the TurboEdit implementation: included by TurboEditResolver.cpp,
 * TurboEditChain.cpp, TurboEditAdjudication.cpp and TurboEditBridge.cpp only.
 */

#include "slip/TurboEditResolver.hpp"
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/// Helpers shared by the TurboEdit step files (formerly file-local).
namespace turbo_edit {

/// Satellite id from an MW result (prefer the field; fall back to amb_key prefix).
/// @param sr An MW slip result.
/// @return The satellite id ("G01"), or the whole key when it has no '_'.
inline std::string satOf(const MwSlipResult& sr) {
    if (!sr.satellite.empty()) return sr.satellite;
    const auto p = sr.amb_key.find('_');
    return (p != std::string::npos) ? sr.amb_key.substr(0, p) : sr.amb_key;
}

/// Elevation shaping for the cross-sat TD2 pool: 1/sin(el), with el clamped at
/// 10° so a near-horizon arc cannot produce an unbounded weight.  Mirrors the MW
/// detector's mwElevationWeight.  The pool stores zenith-normalized values and
/// the floor re-expands them to the consuming arc's elevation, so elevation sets
/// the shape of the floor while its magnitude stays data-driven.
/// @param elev_deg Elevation (deg); non-finite or below 10 deg uses 10 deg.
/// @return The weight 1/sin(el) (dimensionless, at least 1).
inline double gfElevWeight(double elev_deg) {
    constexpr double kMinElevDeg = 10.0;
    constexpr double kDegToRad   = 0.017453292519943295;
    const double el = (std::isfinite(elev_deg) && elev_deg > kMinElevDeg)
                          ? elev_deg : kMinElevDeg;
    return 1.0 / std::sin(el * kDegToRad);
}

/// Receiver-declared loss of lock on either phase signal of a pair.
/// Mirrors MwSlipDetector::hasAnyPhaseLockLoss's `lli != 0` convention.
/// A set LLI corroborates that integer continuity broke, so the code-domain
/// veto does not apply; the caller's LLI handling governs.
/// @param obs  The observation carrying the per-signal LLI flags.
/// @param sig1 First phase signal of the pair (e.g. "L1C").
/// @param sig2 Second phase signal of the pair (e.g. "L2W").
/// @return True when either signal carries a nonzero LLI flag.
inline bool pairHasLockLoss(const PppObservation& obs,
                            const std::string& sig1, const std::string& sig2) {
    const auto set = [&obs](const std::string& sig) {
        const auto it = obs.lli_flags.find(sig);
        return it != obs.lli_flags.end() && it->second != 0;
    };
    return set(sig1) || set(sig2);
}

}  // namespace turbo_edit

/**
 * @brief An MW wide-lane flag on one arc this epoch (step R2), deduplicated
 *        from the detector's two entries per pair.
 */
struct TurboEditResolver::MwFlag {
    int    dN_wl{ 0 };            ///< MW wide-lane integer of the flag (valid when @c known).
    bool   known{ false };        ///< MW published an integer for the pair this epoch.
    /// Float WL-step estimate (cycles, ZD frame).  Selects candidates for
    /// the gap-bridge solve when the integer is withheld (for example on
    /// the locked-arc TD-confirmation path).  NaN when unavailable.
    double wl_float{ std::numeric_limits<double>::quiet_NaN() };
};

/**
 * @brief One pass-1 row: a dual-band phase pair of one observation with its GF
 *        value and either its TD2 or, on a bridged row, the drift-corrected
 *        across-gap jump (step R3).  Pass 2 adjudicates every row once.
 */
struct TurboEditResolver::PairEpoch {
    const PppObservation* obs;  ///< The pair's observation (an element of the epoch's input).
    std::string key, sig1, sig2;  ///< Arc key "G01_L1C_L2W"; the pair, higher frequency first.
    double gf{ 0.0 }, td2{ 0.0 }, lambda1{ 0.0 }, lambda2{ 0.0 };  ///< GF (m), TD2 or across-gap jump (m), wavelengths (m).
    double thresh{ std::numeric_limits<double>::quiet_NaN() };  ///< Detection gate on |td2| (m; gap-widened when bridged).
    double thresh_unfloored{ std::numeric_limits<double>::quiet_NaN() };  ///< kσ·MAD, no floor (trace diagnostic)
    bool   gf_exceeds{ false };  ///< |TD2| > thresh (outlier, ignoring cooldown)
    /// True when @c td2 holds not a TD2 but the drift-corrected across-gap
    /// jump from a stashed pre-gap level (see the header's gap-bridge
    /// note); @c gap_s is the span back to the stashed reference.
    bool   bridged{ false };
    double gap_s{ 0.0 };  ///< Span back to the stashed pre-gap level (s; bridged rows only).
};

/**
 * @brief One staged per-(satellite, signal) integer (pass 2): the integer slip,
 *        and the wavelength and rounding residual a repair reports.
 *
 * The three were parallel maps keyed alike (@c cand, @c cand_lambda,
 * @c cand_frac), always written together on a signal's first staging; one map
 * of this value keeps the same keys, order and values.
 */
struct TurboEditResolver::StagedInteger {
    int    dN{ 0 };          ///< Integer slip in this signal's cycles (0 = adjudicated clean).
    double lambda{ 0.0 };    ///< Signal wavelength (m).
    double frac{ 0.0 };      ///< Rounding residual of the float estimate the integer came from.
};

/**
 * @brief Per-epoch working value of @c TurboEditResolver::resolve.
 *
 * Built by the driver from its arguments and the module logger, filled and read
 * by the steps in order (R0 to R9, the table in TurboEditResolver.hpp),
 * discarded when the call returns.  It owns nothing that outlives the epoch:
 * what must persist lives in the resolver (@c gf_arcs_, @c sig_runs_,
 * @c held_across_lli_, the cross-satellite tiers, @c prev_epoch_s_).  The
 * members are grouped by the step that first writes them.
 */
struct TurboEditResolver::EpochWork {
    /**
     * @brief Bind the call's inputs.
     * @param mw_results_in      MW slip results of this epoch.
     * @param mw_clean_pairs_in  Arc keys whose MW observable was affirmatively clean.
     * @param observations_in    Modeled observations of this epoch.
     * @param wl_gap_evidence_in Settling across-gap WL estimates per arc key.
     * @param log_in             The "turbo_edit_resolver" module logger.
     */
    EpochWork(const std::vector<MwSlipResult>& mw_results_in,
              const std::set<std::string>& mw_clean_pairs_in,
              const std::vector<PppObservation>& observations_in,
              const std::map<std::string, MwSlipDetector::AcrossGapWlEvidence>&
                  wl_gap_evidence_in,
              std::shared_ptr<spdlog::logger> log_in)
        : mw_results(mw_results_in),
          mw_clean_pairs(mw_clean_pairs_in),
          observations(observations_in),
          wl_gap_evidence(wl_gap_evidence_in),
          log(std::move(log_in)) {}

    // --- The call's inputs ----------------------------------------------------
    const std::vector<MwSlipResult>& mw_results;      ///< MW slip results (the source of dN_WL).
    const std::set<std::string>& mw_clean_pairs;      ///< Affirmatively MW-clean arc keys.
    const std::vector<PppObservation>& observations;  ///< Modeled observations.
    /// Settling across-gap WL estimates per arc key (the WL-settle deferral).
    const std::map<std::string, MwSlipDetector::AcrossGapWlEvidence>& wl_gap_evidence;
    std::shared_ptr<spdlog::logger> log;              ///< "turbo_edit_resolver" logger.

    // --- R0, R1: the epoch frame ----------------------------------------------
    /// The epoch's resolution: written by every step that repairs, resets,
    /// vetoes or defers, and returned by the driver.
    EpochResolution out;
    /// Continuous GPS seconds of this epoch; 0 when no observation carries a
    /// satellite (the epoch then leaves @c prev_epoch_s_ unchanged).
    double epoch_now_s = 0.0;

    // --- R2: the MW flag index --------------------------------------------------
    /// MW flags by arc key ("G01_L1C_L2W").
    std::map<std::string, MwFlag> mw_by_pair;

    // --- R3: pass 1 -------------------------------------------------------------
    /// Every row of this epoch, in observation and pair order.  Pass 2 reads
    /// rows by value and never writes this vector.
    std::vector<PairEpoch> cache;
    /// Per-constellation TD2 values of the non-bridged rows (the IVI pools).
    std::map<char, std::vector<double>> cons_td2;
    /// Rows by arc key, built at the end of pass 1 (the sibling-pair
    /// affirmation's lookup into this epoch's snapshot).
    std::map<std::string, const PairEpoch*> pe_by_key;

    // --- R4: the IVI -------------------------------------------------------------
    /// Cross-satellite IVI per constellation character (m).
    std::map<char, double> ivi_by_cons;

    // --- R5: pass 2 staging ------------------------------------------------------
    // Per-(sat, signal) integer candidates, reconciled across a sat's pairs.
    std::map<std::string, std::map<std::string, StagedInteger>> cand;
    // Bridged-moot markers: the staged integer is the signal's true
    // across-gap slip (so cross-pair consistency checks stay valid), but the
    // signal missed an epoch during the gap.  Its ambiguity was pruned and
    // reseeded and already absorbs the slip, so emission moves it from
    // repairs to EpochResolution::moot_signals (mandate suppression, no state
    // action).
    std::map<std::string, std::set<std::string>> cand_moot;
    std::set<std::string> conflict_sats;  ///< Satellites whose pairs staged different integers for one signal.
};

/**
 * @brief Per-row working value of pass 2 (step R5, the row steps A0 to A5).
 *
 * Built by @c adjudicateRow from one cached row and its GF arc, discarded when
 * the row is done.  The evidence members are written once by @c openRow (A0);
 * the decision members start at their "no action" values and are written by
 * the adjudication that applies (A2 or A3), then read by the trace row (A4)
 * and the arc update (A5).
 */
struct TurboEditResolver::RowWork {
    /**
     * @brief Copy the row and bind its arc.
     * @param cached The cached pass-1 row (copied: A1 may rewrite the copy).
     * @param arc_in The row's GF arc in @c gf_arcs_.
     */
    RowWork(const PairEpoch& cached, GfArc& arc_in)
        : pe(cached), arc(arc_in), sat(pe.obs->satellite) {}

    // --- The row ------------------------------------------------------------------
    /// The row, by value: the late across-gap re-route (A1) rewrites its TD2,
    /// gap and bridged flag, while the cached row stays this epoch's snapshot
    /// for the sibling-pair affirmation.
    PairEpoch pe;
    GfArc& arc;               ///< The row's GF jump arc (written by A1, A2, A3, A5).
    const std::string& sat;   ///< The row's satellite ("G01").

    // --- A0: evidence ---------------------------------------------------------------
    double ivi{ 0.0 };                ///< IVI of the row's constellation (m).
    bool   iono_stable_cons{ true };  ///< That IVI is below @c kIviThreshold.
    double split_gate_m{ 0.0 };       ///< Split-capability bound @c split_max_sigma_·|λ1−λ2| (m).
    /// TD2 noise σ the gates were scaled by (m; the gap-widened value on a
    /// bridged adjudication); traced.
    double sigma_trace{ 0.0 };
    /// The row's MW flag in @c EpochWork::mw_by_pair, or its end.
    std::map<std::string, MwFlag>::const_iterator mwit;
    bool   mw_flagged{ false };       ///< MW flagged the row's pair this epoch.
    bool   gf_flagged{ false };       ///< The GF chain exceeded its gate outside the cooldown.
    bool   flagged{ false };          ///< MW or GF flagged the row.

    // --- A2, A3: the decision (traced by A4, applied to the arc by A5) --------------
    bool did_slip = false;        ///< treat as discontinuity → reset arc + cooldown
    int  dN1 = 0, dN2 = 0;        ///< Per-signal integers of the decision (dN1 on sig1, dN2 on sig2); traced.
    double frac2 = 0.0;           ///< Rounding residual of the split; traced.
    int  dN_wl_trace = 0;         ///< WL integer the decision used; traced.
    bool wl_known_trace = true;   ///< Whether that integer was known; traced.
    const char* action = "none";  ///< Trace action: none, repair, reset, code_event, moot, defer or coarse_gf.
};

/**
 * @brief The across-gap integer candidate solve of one bridged row (A2): the
 *        WL candidates and where they came from, then the unique survivor.
 */
struct TurboEditResolver::BridgeSolve {
    // --- WL candidates (collectBridgeWlCandidates) ------------------------------------
    std::vector<int> k_cands;     ///< WL integer candidates k (cycles).
    bool k_published = false;     ///< k is the MW-published integer.
    bool k_settled   = false;     ///< k is the settled across-gap estimate (a deferral epoch).
    // --- The candidate that survives the gates (selectBridgeCandidate) ----------------
    int    best_k = 0, best_dN2 = 0;   ///< The best surviving candidate and its dN2.
    double best_frac = 1.0;           ///< Its rounding residual.
    int    n_acceptable = 0;          ///< Candidates that passed every gate (1 = unique).
    int    n_sibling_pruned = 0;      ///< Candidates pruned by the sibling-pair affirmation.
};

} // namespace GnssProcessor
