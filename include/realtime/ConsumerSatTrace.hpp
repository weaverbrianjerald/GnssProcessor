#pragma once

/**
 * @file ConsumerSatTrace.hpp
 * @brief Per-satellite readout of the closed-loop PPP-RTK consumer: one
 *        POD row per (satellite, raw phase signal) per consumer epoch, plus its CSV
 *        schema.
 *
 * Purpose.  `logs/closed_loop_trace.csv` carries position, `n_sats` and the AR
 * columns only.  Without per-satellite consumer content, a float re-convergence
 * cannot be separated from the slant-iono half of the RF-0b applied pair, and an
 * undeclared wire step cannot be seen reaching the consumer; this row supplies
 * that content.  See `docs/refound/EV6_hour6_decouple_event_
 * 2026-09-15.md` §9 item 1.
 *
 * Data flow.  @ref GnssProcessor::RealTimeEngine collects one epoch of rows at the
 * tail of @c processEpoch (seam @c traceConsumerSat_ in RealTimeEngineSsrDiag.cpp,
 * the consumer-side observability seam of the real-time engine's decision tree)
 * and keeps them in a member buffer.  The network producer's emit/store seam
 * (@c networkEmitStore) reads that buffer after @c consumer->processEpoch and
 * passes it to @c NetworkRunTraces::writeConsumerSatRows, which owns the stream.
 * @c closed_loop_trace.csv uses the same ownership split.
 *
 * Measure-only.  The filter reads nothing back from this row.  Rows are collected
 * only when @c network.trace_consumer_sat is armed, so an unarmed run does not
 * allocate, does not record an event and does not open a file.
 */

#include <string>

namespace GnssProcessor {

/// CSV header of `logs/closed_loop_sat_trace.csv`.  Kept beside the
/// row struct and the formatter so the three stay in step.
///
/// Column groups, in order:
///  - identity: `week,tow,sat,sig,code_sig,elev_deg,n_sig`
///  - applied wire corrections (m), as the consumer's own gated reads return them
///    this epoch: `clk_m` (satellite clock; blank when the SSR clock overlay
///    supplied nothing), `cb_m` (satellite code bias on `code_sig`), `pb_m`
///    (satellite phase bias on `sig`)
///  - the producer's declaration counters as the consumer sees them in the store:
///    `clk_disc,cb_disc,pb_disc` (blank when the channel carries none)
///  - the consumer's reactions to those declarations this epoch:
///    `ev_pb,ev_cb,ev_clk`, each `none` or the action names passed to
///    @c RealTimeEngine::noteConsumerSatEvent_ for that key, semicolon-joined when
///    a key took more than one.  `ev_pb` reads the `<sat>|<sig>` key, `ev_cb` the
///    `<sat>|<code_sig>` key and `ev_clk` the `<sat>|CLK` key.  The names include
///    reset_disc, reset_flip, carry_disc, carry_flip, translate_disc,
///    translate_undeclared, translate_refused, reset_<refusal> (reset_first_sight,
///    reset_basis, reset_bssd, reset_unknown_key), reset_hold_gen, reset_gen,
///    reinflate_disc, reinflate_flip, reinflate_isb, flip_to_fresh, flip_to_stale
///    and refloat_level.
///  - pre-fit innovations (m): `innov_code_m,innov_phase_m`
///  - post-fit residuals (m): `res_code_m,res_phase_m`
///  - consumer state: `amb_m,amb_var` (ambiguity state `<sat>_<sig>`),
///    `iono_m,iono_var` (per-satellite slant iono state `<sat>`)
///
/// A blank field means the consumer had no such quantity this epoch and is kept
/// distinct from zero: an absent phase bias and a zero phase bias are different
/// wires, and telling them apart is what the trace is for.
inline constexpr const char* kConsumerSatTraceHeader =
    "week,tow,sat,sig,code_sig,elev_deg,n_sig,"
    "clk_m,cb_m,pb_m,"
    "clk_disc,cb_disc,pb_disc,"
    "ev_pb,ev_cb,ev_clk,"
    "innov_code_m,innov_phase_m,"
    "res_code_m,res_phase_m,"
    "amb_m,amb_var,iono_m,iono_var\n";

/**
 * @brief One row of `closed_loop_sat_trace.csv`: a satellite × raw phase signal
 *        of the consumer's solution at one epoch.
 *
 * One row per (sat, signal), rather than one per satellite with keyed signal
 * columns, because that matches the consumer's state layout exactly: the ambiguity
 * state key is `<sat>_<sig>` (StateKeySchema::makeAmbiguityKey), the satellite
 * phase bias and code bias are per-signal in the SSR store, and the post-fit
 * residual rows in @c EpochMeasurement are labelled (satellite_ids,
 * signal_types) = (sat, RINEX code).  The per-satellite quantities (clock, slant
 * iono with key `<sat>` from StateKeySchema::makeIonosphereKey, elevation) repeat
 * across a satellite's rows, and @c n_sig gives the number of those rows.
 *
 * @c code_sig is the code signal paired with @c sig by band and attribute
 * (L1W→C1W, L2W→C2W).  The RF-0b pair the consumer must apply together is the
 * code bias on that signal and the slant iono, so the two share a row.
 *
 * Not included: the gain-weighted position effect of a row.  It needs the Kalman
 * gain K = P⁻HᵀS⁻¹, and neither K nor the prior P⁻ of the committed update
 * survives @c processEpoch (only the posterior does).  Reconstructing it would take
 * an O(m³) re-solve per epoch against a covariance that is no longer the one the
 * update used, which is neither cheap nor exact.  The pre-fit innovation
 * (@c innov_phase_m / @c innov_code_m) is both, and the row carries it instead.
 */
struct ConsumerSatRow {
    std::string sat;        ///< RINEX satellite id, e.g. "G23".
    std::string sig;        ///< Raw phase signal code, e.g. "L1W" (never an IF combo).
    std::string code_sig;   ///< Band+attribute-paired code signal, e.g. "C1W"; empty when the epoch carries none.
    double      elev_deg{ 0.0 };  ///< Satellite elevation (deg) from the consumer's own modeled obs.
    int         n_sig{ 0 };       ///< Raw phase signals this satellite contributes this epoch.

    bool   have_clk{ false }; double clk_m{ 0.0 };  ///< Applied satellite clock correction (m).
    bool   have_cb{ false };  double cb_m{ 0.0 };   ///< Applied satellite code bias on @ref code_sig (m).
    bool   have_pb{ false };  double pb_m{ 0.0 };   ///< Applied satellite phase bias on @ref sig (m).

    bool   have_clk_disc{ false }; int clk_disc{ 0 };  ///< Producer clock discontinuity counter.
    bool   have_cb_disc{ false };  int cb_disc{ 0 };   ///< Producer code-bias discontinuity counter on @ref code_sig.
    bool   have_pb_disc{ false };  int pb_disc{ 0 };   ///< Producer phase-bias discontinuity counter on @ref sig.

    std::string ev_pb{ "none" };   ///< Consumer reaction to the phase-bias declaration this epoch.
    std::string ev_cb{ "none" };   ///< Consumer reaction to the code-bias declaration this epoch.
    std::string ev_clk{ "none" };  ///< Consumer reaction to the clock declaration this epoch.

    bool   have_innov_code{ false };  double innov_code_m{ 0.0 };   ///< Pre-fit innovation, code row (m).
    bool   have_innov_phase{ false }; double innov_phase_m{ 0.0 };  ///< Pre-fit innovation, phase row (m).
    bool   have_res_code{ false };    double res_code_m{ 0.0 };     ///< Post-fit residual, code row (m).
    bool   have_res_phase{ false };   double res_phase_m{ 0.0 };    ///< Post-fit residual, phase row (m).

    bool   have_amb{ false };  double amb_m{ 0.0 };  double amb_var{ 0.0 };   ///< Ambiguity state value (m) + variance (m²).
    bool   have_iono{ false }; double iono_m{ 0.0 }; double iono_var{ 0.0 };  ///< Slant-iono state value (m) + variance (m²).
};

/**
 * @brief Format one @ref ConsumerSatRow as a CSV line for @ref kConsumerSatTraceHeader.
 *
 * Pure (no I/O, no engine state), so the schema is unit-testable on its own
 * (tests/test_consumer_sat_trace.cpp).  Fixed precision per column group: 3 dp for
 * tow and elevation, 6 dp for every metre quantity (sub-mm, so a 0.5 mm wire step
 * is still visible), 6 significant digits for variances.  An absent quantity
 * writes an empty field.
 *
 * @param week GPS week of the consumer epoch.
 * @param tow  GPS time of week of the consumer epoch (s).
 * @param r    The row.
 * @return The CSV line, newline-terminated.
 */
std::string formatConsumerSatRow(int week, double tow, const ConsumerSatRow& r);

} // namespace GnssProcessor
