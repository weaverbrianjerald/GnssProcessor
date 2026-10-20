#pragma once
/**
 * @file RowRules.hpp
 * @brief The rules that decide whether a row enters the update and what it weighs, as free
 *        functions of configuration, product tables and one observation.
 *
 * Three rules sit beside the observation equation and read no state vector:
 *  - usability (@ref isUsableMeasurement): whether an observation's signal becomes a row;
 *  - product coverage (@ref ifCombHasFullBia, @ref biaCoversSignal, @ref ssrCoversCodeSignal,
 *    @ref ssrCoversPhaseSignal, @ref phaseBiasCoveredForAr, @ref ifCombHasFullRxAtx): whether
 *    a .BIA product, the SSR stream or a receiver antenna entry already corrects a signal;
 *  - the row variance (@ref rowVariance): the stochastic-model variance plus the consumer's
 *    network-correction terms, times the no-BIA code downweight.
 *
 * They were members of @c MeasurementModel, which holds a @c StateManager, although none of
 * them reads a state.  As free functions they run for any caller that holds a configuration,
 * the model's product tables and an SSR store: the single-receiver update, the network
 * producer's pre-pass and the real-time engine all take the same answer.
 *
 * The definitions are in MeasurementModelRow.cpp (usability and coverage) and
 * MeasurementModelVariance.cpp (the variance).  Plain value types and free functions.
 */

#include "common/PppObservation.hpp"
#include "common/GnssTypes.hpp"      // System

#include <map>
#include <set>
#include <string>

namespace GnssProcessor {

class GnssConfig;
class LogManager;
class PppModel;
class SsrCorrectionStore;
struct GnssTime;

/**
 * @brief Per-constellation set of full RINEX-3 signal codes the BSSD pivot tracks.
 *
 * Key: constellation name ("GPS"); value: its signals ({"L1W", "L1C", "C1W", ...}).  Empty
 * when no pivot filter is active.  Per-signal (not per-band) granularity, because a
 * non-pivot's L1C is single-difference eligible only if the pivot also tracks L1C, even when
 * the pivot tracks L1W on the same band.
 */
using PivotSignalMap = std::map<std::string, std::set<std::string>>;

/**
 * @brief What the coverage checks and the row variance read besides the observation: the
 *        configuration, the model's bias and antenna tables, and the SSR store.
 *
 * Three non-owning references, built per call by the owner of the model
 * (@c MeasurementModel::rowProducts).  The store is the live store (the closed-loop
 * consumer points it at the producer's), the process-wide singleton otherwise.
 */
struct RowProducts {
    const GnssConfig&         config;     ///< Processing configuration.
    const PppModel&           ppp_model;  ///< Source of the .BIA and receiver ATX presence tables.
    const SsrCorrectionStore& ssr;        ///< The SSR store the rows are corrected from.
};

// ---------------------------------------------------------------------------
// Usability
// ---------------------------------------------------------------------------

/**
 * @brief Whether an observation's signal enters this epoch's update.
 *
 * False for: GLONASS under BSSD unless enable_glonass_bssd; a code, phase or Doppler signal
 * the BSSD pivot does not track; a signal that is neither code nor phase (nor Doppler with
 * velocity estimation); an iono-free pair in uncombined mode, a raw signal in iono-free
 * mode, a Doppler pair; a signal without an observed, variance and modeled value; a row the
 * Kalman filter's rebuild loop excluded (@c "<sat>_<signal>", logged at debug).
 *
 * @param config        Processing configuration.
 * @param log_manager   Diagnostic logging (the exclusion line).
 * @param pivot_signals The signals the BSSD pivot tracks, per constellation (empty: no filter).
 * @param obs           The satellite's modeled observation.
 * @param ref_type      The signal (a RINEX code, or an iono-free pair "a_b").
 * @param excluded      The rebuild loop's excluded row signatures.
 * @param epoch_prefix  Log prefix.
 * @return True when the signal is usable.
 */
bool isUsableMeasurement(const GnssConfig& config,
                         LogManager& log_manager,
                         const PivotSignalMap& pivot_signals,
                         const PppObservation& obs,
                         const std::string& ref_type,
                         const std::set<std::string>& excluded,
                         const std::string& epoch_prefix);

/**
 * @brief Whether an observation's stored geometry can carry a row: a satellite position and
 *        range of a satellite's orbit, an elevation above the mask, a line of sight with a
 *        finite unit vector and a positive wet mapping function.
 *
 * Logs the first failing check (a warning for an invalid value, a debug line for a satellite
 * below the mask).
 *
 * @param config       Processing configuration (the elevation mask, the mode label).
 * @param log_manager  Diagnostic logging.
 * @param obs          The satellite's modeled observation.
 * @param epoch_prefix Log prefix.
 * @return True when the geometry is valid.
 */
bool hasValidGeometry(const GnssConfig& config, LogManager& log_manager,
                      const PppObservation& obs, const std::string& epoch_prefix);

// ---------------------------------------------------------------------------
// Product coverage
// ---------------------------------------------------------------------------

/**
 * @brief Whether both component signals of an IF combination have BIA product coverage.
 * @param products  The configuration, tables and store.
 * @param satellite Satellite id.
 * @param if_key    Iono-free pair "a_b"; any other key counts as covered.
 * @return True when both signals have a .BIA entry (or the key is not a pair).
 */
bool ifCombHasFullBia(const RowProducts& products, const std::string& satellite,
                      const std::string& if_key);

/**
 * @brief Whether the live SSR stream supplies a code bias for this exact (satellite, raw
 *        signal), i.e. whether the SSR bias term will correct the row.
 *
 * Matches the apply-site activation: false in the network producer mode (which estimates
 * satellite biases as states rather than importing them); otherwise a presence check via
 * @c SsrCorrectionStore::hasCodeBias.  When the staleness gate is on, the check is age-gated
 * with the same effective stale-hold limit as the apply site: a bias the gate has dropped no
 * longer defines this row's datum, so the sat-bias state and the no-BIA downweight apply
 * again.  Used to suppress redundant per-(satellite, signal) sat-bias state allocation for
 * signals the stream already corrects.  Post-processing leaves the SSR store empty, so this
 * is always false there and the state layout is unchanged.
 *
 * @note Raw signals only.  IF-combined rows (key contains '_') get no SSR bias term (SSR code
 *       biases are per raw signal), so their sat-bias state stays the only correction and
 *       must not be suppressed; pass raw signal keys here.
 *
 * @param products  The configuration, tables and store.
 * @param satellite Satellite id.
 * @param signal    Raw code signal (e.g. "C1C").
 * @param obs_time  Observation epoch for the staleness comparison (ignored unless the
 *                  staleness gate is on).
 * @return True when the stream corrects the row.
 */
bool ssrCoversCodeSignal(const RowProducts& products, const std::string& satellite,
                         const std::string& signal, const GnssTime& obs_time);

/**
 * @brief Whether a loaded .BIA entry counts as correcting a signal under this configuration.
 *
 * True, except when @c model_corrections.bias_coverage_requires_apply is on, the mode is not
 * the network producer's and @c enable_code_phase_bias is off, so that the entry is loaded but
 * not folded into @c obs.modeled.  The half of @ref biaCoversSignal that reads the
 * configuration; a caller that has the presence of the entries by other means (the slant
 * ionosphere seed) applies it directly.
 *
 * @param config Processing configuration.
 * @return True when a present entry is also applied (or the rule is off or exempt).
 */
bool biaEntryApplies(const GnssConfig& config);

/**
 * @brief Whether a .BIA entry counts as bias coverage for the "already corrected?" gates:
 *        code rows (sat-bias absorber allocation, no-BIA code downweight) and phase rows (AR
 *        eligibility via @ref phaseBiasCoveredForAr).
 *
 * Legacy semantics (default): a pure @c hasBiasEntry presence check.  A .BIA that is loaded
 * but not applied (@c enable_code_phase_bias false, e.g. forced off by
 * @c network.estimate_iono_sh and inherited by the derived closed-loop consumer) still
 * reports coverage, so the row runs at full code weight with no absorber while carrying the
 * raw satellite OSB content.  With @c model_corrections.bias_coverage_requires_apply the
 * entry counts only while @c enable_code_phase_bias is actually folding it into
 * @c obs.modeled.  The network producer keeps legacy semantics under either setting: its
 * code rows are corrected by network-estimated shared code-bias states, not by the .BIA.
 *
 * @param products  The configuration, tables and store.
 * @param satellite Satellite id.
 * @param signal    Raw signal.
 * @return True when the .BIA entry covers the signal.
 */
bool biaCoversSignal(const RowProducts& products, const std::string& satellite,
                     const std::string& signal);

/**
 * @brief Whether the live SSR stream supplies a phase bias for this exact (satellite, raw
 *        signal), i.e. whether the phase SSR bias term will correct the row.
 *
 * Phase-channel counterpart of @ref ssrCoversCodeSignal: false in the network producer mode
 * (which estimates satellite phase biases as states rather than importing them); otherwise a
 * presence check via @c SsrCorrectionStore::getPhaseBias, age-gated with the same effective
 * stale-hold limit as the apply site when the staleness gate is on.  Post-processing leaves
 * the SSR store empty, so this is always false there.
 *
 * @param products  The configuration, tables and store.
 * @param satellite Satellite id.
 * @param signal    Raw phase signal.
 * @param obs_time  Observation epoch for the staleness comparison (ignored unless the
 *                  staleness gate is on).
 * @return True when the stream corrects the row.
 */
bool ssrCoversPhaseSignal(const RowProducts& products, const std::string& satellite,
                          const std::string& signal, const GnssTime& obs_time);

/**
 * @brief AR-eligibility phase-bias coverage for an ambiguity's signal: whether the satellite
 *        phase bias on this (satellite, signal) is removed from the phase observable, so that
 *        its float ambiguity may be fixed to an integer.
 *
 * With @c network.require_signal_integer_for_ar on, a covering network phase bias present in
 * the SSR store (age-gated when the staleness gate is on) that declares
 * @c signal_integer = false vetoes eligibility; for an IF key, either component can veto.
 * With the toggle off, or with no such bias in the store, the checks below decide unchanged.
 *
 * Legacy semantics (default, @c model_corrections.bias_coverage_requires_apply off): raw
 * .BIA presence (@ref ifCombHasFullBia for IF-combined keys, @c hasBiasEntry for raw
 * signals), unchanged from the original gate.  With the toggle on, a signal is covered iff
 * the .BIA entry is actually applied (@ref biaCoversSignal) or a live network/SSR phase bias
 * corrects the row (@ref ssrCoversPhaseSignal); for an IF key, both components must be
 * covered by the same source.  This lets the closed-loop PPP-RTK consumer, which inherits
 * @c enable_code_phase_bias=false from the producer, key AR eligibility on the network phase
 * biases it applies rather than on a .BIA it does not apply.
 *
 * @param products  The configuration, tables and store.
 * @param satellite Satellite id.
 * @param signal    Raw phase signal (e.g. "L1C") or IF key ("L1C_L5Q").
 * @param obs_time  Observation epoch for the SSR staleness comparison.
 * @return True when the signal's phase bias is removed.
 */
bool phaseBiasCoveredForAr(const RowProducts& products, const std::string& satellite,
                           const std::string& signal, const GnssTime& obs_time);

/**
 * @brief Whether both component signals of an IF combination have receiver antenna ATX
 *        coverage (PCO/PCV).
 * @param products The configuration, tables and store.
 * @param if_key   Iono-free pair "a_b"; any other key counts as covered.
 * @param system   The satellite's system.
 * @return True when both signals have a receiver ATX entry (or the key is not a pair).
 */
bool ifCombHasFullRxAtx(const RowProducts& products, const std::string& if_key, System system);

// ---------------------------------------------------------------------------
// Variance
// ---------------------------------------------------------------------------

/**
 * @brief The measurement variance of one row (m^2): the stochastic-model variance plus the
 *        consumer's network-correction terms, times the no-BIA code downweight.
 *
 * In order: obs.variances[ref_type]; with consume_combined_sigma and a fresh network
 * correction carrying a combined sigma, that sigma (age-inflated) in place of the next two
 * terms; otherwise the network URA with the ZOH clock-staleness term and the held-clock
 * fallback, and on phase rows the network phase-bias sigma; then, without
 * estimate_sat_biases, the no-BIA downweight factor on a code row without .BIA or SSR
 * coverage.  The network producer's own rows get none of the network terms.
 *
 * @param products The configuration, tables and store.
 * @param obs      The satellite's modeled observation.
 * @param ref_type The signal (one with a variance: the row loop admits only usable signals).
 * @return The row's variance, before the row loop's non-finite guard.
 */
double rowVariance(const RowProducts& products, const PppObservation& obs,
                   const std::string& ref_type);

} // namespace GnssProcessor
