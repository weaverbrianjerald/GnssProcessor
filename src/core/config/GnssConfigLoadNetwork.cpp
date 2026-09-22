/*!
 * \file GnssConfigLoadNetwork.cpp
 * \brief JSON configuration loading for the PPP-RTK @c network section.
 *
 * Split out of @c GnssConfigLoad.cpp (decomposition INC-1, 2026-07-16;
 * see docs/network_decomposition_plan.md), then decomposed
 * internally (2026-08-01) from one 4.4k-line @c loadNetworkSection_ body into a
 * thin orchestrator plus one helper per JSON subsection — the same idiom the
 * eleven top-level section loaders in @c GnssConfigLoad.cpp already use.  Every
 * card adds knobs to this file and concurrent sessions are the norm here, so
 * per-subsection helpers are what keep two sessions' edits out of one hunk.
 * Contains:
 *
 *   - the file-local @c parseCoordEpochYear helper (sole caller lives in
 *     @c loadNetworkRosterAndDatum_),
 *   - @c GnssConfig::loadNetworkSection_ — now a fixed-order sequence of
 *     @c loadNetworkXxx_ / @c validateNetworkXxx_ calls,
 *   - those per-subsection helpers, defined below in call order,
 *   - the consumer-coupling helpers @c applyNetworkConsumerCouplings,
 *     @c deriveClosedLoopConsumerConfig and @c applyNetworkConsumerProfile.
 *
 * @c GnssConfig::logNetworkPosture_ and its recommendation tables moved to
 * @c GnssConfigNetworkPosture.cpp in the same pass.
 *
 * **The call ORDER in @c loadNetworkSection_ is load-bearing, and the split
 * preserves it exactly.**  The cross-knob forcings are all expressed as
 * sequence — the @c parallel_carry / @c parallel_reduce mutex, the FC-1 and
 * RTF-3 inert-arming refusals reading state parsed above them, the D5
 * corroboration gate auto-arming the slip-QC instrumentation that feeds it, the
 * RAIM tier auto-couplings, the RTB-8 service arming and its P3 re-validation,
 * and the @c network.process_noise table (resolved first, settled last).
 * Several helpers therefore read state a LATER subsection has not written yet,
 * and several validate state an EARLIER one did; each says so in its brief.  Add
 * a new subsection as a new helper at the point in the sequence where its
 * prerequisites are already resolved — never reorder the existing calls.
 *
 * Behavior is unchanged by the decomposition: every helper body was moved
 * verbatim, and the V1/W6 cross-section forcing writes (18 sites over 12
 * non-network fields) travel with it.  Five of those are the
 * @c network.process_noise write-through (@c process_noise_.sat_clock /
 * .sat_phase_bias / .sat_code_bias and @c global_vtec_sh_.q_per_degree /
 * .layer2_q_per_degree), which fire ONLY when the scoped table supplies a class
 * the config did not set through its legacy key.  They are safe to reach across
 * sections because this loader runs only in @c PPP_RTK_Network mode — the PPP
 * and real-time paths never execute it — and within a network run those members
 * name the same physical quantity the table is scoping.  The estimation
 * section's raw-JSON peek at @c network.estimate_glonass deliberately stays in
 * @c GnssConfigLoad.cpp (it reads the JSON directly, not this code).
 */

#include "config/GnssConfig.hpp"
#include "filter/NetworkPrefitCensus.hpp"   // parseFuseClassMask / fuseClassMaskName (SPEC_prefit_census_observability INC-2)
#include "filter/NetworkCarryVce.hpp"       // parseComponents (prior-residual census family list)
#include "filter/NetworkCarryVceApply.hpp"  // familySlot (M2 v2 apply-family validation)
#include "filter/NetworkOraclePinScope.hpp" // oraclePinParseScope (ONE grammar for the oracle_pin scope)
#include "filter/NetworkSecondCodeScope.hpp" // secondCodeAllowEntryValid (card SCC allow-list grammar)
#include "filter/NetworkSecondPhaseScope.hpp" // secondPhaseAllowEntryValid (card SID phase 6a)
#include "config/GnssConfigJsonHelpers.hpp"
#include "common/Constants.hpp"                   // GPS_Lx_WAVELENGTH (H3 per-λ log line)
#include "products/NetworkProductEmitter.hpp"   // kDefaultDiscThresholdM (R9 single source of truth)
#include "products/StationInfoFile.hpp"         // resolveRosterCoordinate (RTA-12 SIF roster)
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <spdlog/spdlog.h>
#include <stdexcept>


namespace GnssProcessor {

    using detail::jval;
    using detail::jsection;
    using detail::jhas;

    namespace {

        /**
         * @brief RTA-12: parse a coordinate-epoch declaration into a decimal year.
         *
         * Accepts the two documented @c network.coord_epoch shapes: a decimal year
         * ("2024.5") or an ISO calendar date ("2024-07-01", mapped to the day
         * centre).  Anything else — including trailing junk — is rejected so the
         * coordinate-age advisory can distinguish "unset" from "unparseable".
         *
         * @param s Epoch string as typed in the config.
         * @return Decimal year, or @c std::nullopt when unparseable.
         */
        std::optional<double> parseCoordEpochYear(const std::string& s) {
            if (s.empty()) return std::nullopt;
            // ISO date YYYY-MM-DD -> decimal year at the day centre.
            if (s.size() == 10 && s[4] == '-' && s[7] == '-') {
                int y = 0, m = 0, d = 0;
                if (std::sscanf(s.c_str(), "%4d-%2d-%2d", &y, &m, &d) == 3
                    && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
                    static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181,
                                                 212, 243, 273, 304, 334 };
                    const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
                    const double doy = cum[m - 1] + d + ((leap && m > 2) ? 1 : 0);
                    return static_cast<double>(y) + (doy - 0.5) / (leap ? 366.0 : 365.0);
                }
                return std::nullopt;
            }
            // Decimal year (whole string must consume; sane geodetic range).
            try {
                std::size_t pos = 0;
                const double v = std::stod(s, &pos);
                if (pos == s.size() && v > 1900.0 && v < 2200.0) return v;
            } catch (const std::exception&) {}
            return std::nullopt;
        }

    } // namespace

    // =================================================================
    //  Section: network  (PPP_RTK_Network producer)
    // =================================================================
    bool GnssConfig::loadNetworkSection_(const nlohmann::json& json, LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        const auto& nw = jsection(json, "network", "network");
        if (nw.is_null() || !nw.is_object()) {
            log->error("PPP-RTK-Network mode selected but no 'network' section found in config");
            return false;
        }

        // RTB-8: read the service-profile flag FIRST.  Several coherence warnings
        // in this section (per_const no-op, contract-rebroadcast inert, warmup-URA
        // no-op, emit_ura-without-raim, retro-bump-no-tier, RT0-5 inert rate) would
        // be FALSE under service_mode — the bundle arms their prerequisites in the
        // arming block near the end of this section — so they are gated on
        // !service_mode.  The arming ITSELF still runs LAST, after every armed
        // toggle is parsed.  Kept as a member (networkServiceMode) purely for
        // provenance: the L15 run manifest records that the run's toggle set came
        // from the bundle rather than hand-tuning.  It is written HERE and never
        // again, which is what lets the five helpers that gate on it re-derive the
        // driver's old `service_mode` local from the member in an alias preamble.
        network_service_mode_ =
            jval<bool>(json, "network.service_mode", "network_service_mode", false);

        // The subsection sequence.  ORDER IS LOAD-BEARING (see the file banner):
        // every cross-knob forcing in this section is expressed as "a later helper
        // reads what an earlier one resolved".  Do not reorder; insert a new helper
        // at the point where its prerequisites are already final.
        if (!loadNetworkProcessNoise_(json, log)) return false;
        if (!loadNetworkRosterAndDatum_(json, nw, log)) return false;
        loadNetworkProducerAr_(json, log);
        loadNetworkLaneLayers_(json, log);
        loadNetworkOutputAndParallel_(json, log);
        loadNetworkFastClock_(json, log);
        if (!loadNetworkStreaming_(json, log)) return false;
        loadNetworkObservabilityAndSnapshot_(json, log);
        if (!loadNetworkSharedStateForcings_(json, log)) return false;
        if (!loadNetworkAprioriAndGlonass_(json, log)) return false;
        loadNetworkCarries_(json, log);
        loadNetworkSlipDetectors_(json, log);
        loadNetworkQcTiers_(json, log);
        loadNetworkClockModelAndChannels_(json, log);
        loadNetworkDatumSignalling_(json, log);
        loadNetworkReattachHygiene_(json, log);
        loadNetworkDeweightReleaseHygiene_(json, log);
        loadNetworkSignalIntegrity_(json, log);
        if (!loadNetworkEmissionFilters_(json, log)) return false;
        if (!validateNetworkDatumCoherence_(log)) return false;
        if (!validateNetworkLaneLayers_(json, log)) return false;
        if (!validateNetworkPhaseDatum_(log)) return false;
        loadNetworkContractHardening_(json, log);
        applyNetworkServiceProfile_(json, log);
        if (!validateNetworkCarryPosture_(json, log)) return false;
        loadNetworkClosedLoop_(nw, log);
        // EVENT HARNESS: last, so its cross-knob warnings read FINAL values (the
        // file banner's rule — a check beside its own parse reads code defaults).
        if (!loadNetworkInject_(nw, log)) return false;
        logNetworkSummary_(log);
        finalizeNetworkProcessNoise_(json, log);

        // Card 1g FEED INTERLOCK, measured 2026-08-15.  Deliberately HERE, at the end of
        // the section, not beside the knob's own parse: `phase_anchor_carry_reref` is
        // parsed ~2800 lines later and `phase_datum_eq34` ~1700 later, so a check at the
        // parse site would read their code defaults instead of the user's values.
        //
        // The realization-step sources are populated ONLY by the eq34 chain datum
        // (eq34_reref) and the prioritized phase-anchor carry re-reference
        // (phase_anchor_reref).  With neither armed the widened probe has nothing to match
        // and the card is INERT: measured 0 diverted terms on a 150-station field arm
        // (phase_datum_prioritized WITHOUT phase_anchor_carry_reref) against 37441 on a
        // 50-station eq34 arm and 507 on a prioritized+pa_carry_reref sim arm.
        if (network_carry_coupling_reinit_sources_
            && !network_phase_datum_eq34_ && !network_phase_anchor_carry_reref_)
            log->warn("Network producer (Card 1g): carry_coupling_reinit_sources is armed but "
                      "NEITHER network.phase_datum_eq34 NOR network.phase_anchor_carry_reref is "
                      "on, so no realization-step source will ever populate the reinit history "
                      "and the card is INERT (the clock-QC feeds it shares with Card 1d are "
                      "unaffected).  Arm one of those two, or drop this knob — do not read the "
                      "resulting A/B as 'no effect'.");

        // SPEC_slip_relevel_walk interlock, deliberately HERE (the Card-1g
        // pattern above): network.carry_invariant parses ~1500 lines after the
        // cycle_slip block, so a check at the parse site would read its code
        // default.  Under carry_invariant the leveling local carry is
        // superseded and structurally EMPTY — the re-level response cannot
        // move a byte there (and the DD invariant carry owns its own slip
        // funnel), so arming it would only mislead (standing invariant #4).
        // ANCHOR-SLIP HANDOVER interlocks (SPEC_event_harness §12.7), deliberately
        // HERE (the Card-1g pattern above): the phase-datum MODE flags parse ~1700
        // lines after the cycle_slip block, so a check at the parse site would read
        // their code defaults.  The card acts inside networkElectPhaseAnchors, which
        // is the `else` arm of the three-way mode dispatch — under the ensemble or
        // eq34 datum, or with the whole prioritized election off, there is no anchor
        // to hand over and the knob is a structural zero (invariant #4).
        if (network_config_.cycle_slip_anchor_slip_handover
            && (!network_phase_datum_prioritized_ || network_phase_datum_ensemble_
                || network_phase_datum_eq34_)) {
            log->warn("Network: network.cycle_slip.anchor_slip_handover needs the "
                      "PRIORITIZED anchor election (network.phase_datum_prioritized "
                      "without phase_datum_ensemble / phase_datum_eq34) — that is the "
                      "only realization with a per-family anchor to hand over "
                      "— disabled.");
            network_config_.cycle_slip_anchor_slip_handover = false;
        }
        if (network_config_.cycle_slip_anchor_slip_handover)
            log->info("Network (anchor-slip handover, SPEC_event_harness §12.7): ARMED "
                      "— a slip response that frees a family's ANCHOR arc stages a "
                      "one-epoch candidacy ban, so the same epoch's election hands the "
                      "family to a successor at its own carried value_abs instead of "
                      "re-realizing the gauge from the freed arc.  Run-end reports the "
                      "families handed over and the continuous / cold split; ZERO staged "
                      "on a run whose detectors fired is dead wiring, not a quiet run.");
        // MW EVIDENCE REFLOAT interlock, deliberately HERE (the Card-1g pattern
        // above): mw_detect parses in loadNetworkSlipDetectors_ but can still be
        // FORCED ON by a posture later in that same function, so a check at the
        // parse site would read a value the run does not use.  Without the
        // detector there is no MW flag to admit and the knob is a structural zero
        // (invariant #4).
        if (network_config_.cycle_slip_mw_evidence_refloat
            && !network_cycle_slip_mw_detect_) {
            log->warn("Network: network.cycle_slip.mw_evidence_refloat needs "
                      "network.cycle_slip.mw_detect — it widens the admission of that "
                      "detector's flags and has nothing to admit without it "
                      "— disabled.");
            network_config_.cycle_slip_mw_evidence_refloat = false;
        }
        if (network_config_.cycle_slip_mw_evidence_refloat)
            log->info("Network (MW evidence refloat): ARMED — a DETECT-ONLY MW wide-lane "
                      "flag is admitted to the refloat path on the STRENGTH of the "
                      "detection (bias-free lane, arc >= {} samples, |dev|/threshold >= "
                      "{:.1f}) as well as on the legacy integer gate (known integer, "
                      "frac <= {:.2f}).  The refloat still applies no integer, and an "
                      "integer-UNKNOWN candidate skips the delta bridge.  Run-end "
                      "reports legacy vs evidence admissions and the two DROP "
                      "populations; those counters are collected with the knob OFF too, "
                      "so an OFF run already states what this would have admitted.",
                      network_config_.cycle_slip_mw_evidence_min_arc_epochs,
                      network_config_.cycle_slip_mw_evidence_ratio_min,
                      network_cycle_slip_turbo_edit_frac_max_);
        // COMMON-BAND SCOPE interlock, same placement rationale as the evidence
        // refloat above: the EXONERATION evidence IS the MW detector's
        // affirmative-cleanliness set, so without mw_detect there are no silent
        // pairs, nothing is ever exonerated, and every verdict falls back to
        // today's response — a structural zero (invariant #4).
        if (network_config_.cycle_slip_common_band_scope
            && !network_cycle_slip_mw_detect_) {
            log->warn("Network: network.cycle_slip.common_band_scope needs "
                      "network.cycle_slip.mw_detect — the exoneration evidence IS that "
                      "detector's clean-pair set, so without it nothing is ever "
                      "exonerated and every verdict is today's response "
                      "— disabled.");
            network_config_.cycle_slip_common_band_scope = false;
        }
        if (network_config_.cycle_slip_common_band_scope)
            log->info("Network (common-band scope): ARMED — ONE verdict per (station, "
                      "satellite, epoch) formed BEFORE the drains: a SILENT, mature MW "
                      "pair (arc >= {} samples) exonerates both of its bands unless the "
                      "IF-TD detector flagged that pair this epoch or the band carries "
                      "LLI; each drain then frees only (its bands INTERSECT implicated). "
                      "No exoneration, nothing implicated, or implicated empty with a "
                      "pair fired => today's all-band response, verbatim.  Run-end "
                      "reports verdicts / narrowed / bands kept and the two fallback "
                      "populations; those counters are collected with the knob OFF too, "
                      "so an OFF run already states what this would have narrowed.",
                      network_config_.cycle_slip_common_band_min_arc_epochs);
        // §22 REFERENCE-ARC CLAUSE interlock.  It is a clause OF the scope's
        // exoneration rule — with the scope off no verdict is formed, nothing is
        // ever refused, and the knob is a structural zero (invariant #4).  Placed
        // with the block above for the same reason: the scope's own disarm has
        // just run, so this is the first point where its flag is final.
        if (network_config_.cycle_slip_common_band_ref_arc
            && !network_config_.cycle_slip_common_band_scope) {
            log->warn("Network: network.cycle_slip.common_band_ref_arc needs "
                      "network.cycle_slip.common_band_scope — it is a clause of that "
                      "rule's exoneration test, so without the scope no verdict is "
                      "formed and nothing is ever refused — disabled.");
            network_config_.cycle_slip_common_band_ref_arc = false;
        }
        if (network_config_.cycle_slip_common_band_ref_arc)
            log->info("Network (common-band scope, §22 reference-arc clause): ARMED — a "
                      "silent MW pair whose IF-TD sample is the chain's REFERENCE "
                      "satellite is UNTESTED, not clean: that sample's double difference "
                      "is identically 0 by construction, so the pooled IF-TD test could "
                      "not have flagged it however large the slip.  Such a pair is "
                      "refused as an exonerator (basis token iftd(<pair>)=ref_arc) and "
                      "the verdict falls back to today's all-band response when nothing "
                      "else exonerates.  Applies to EVERY chain's reference, first chain "
                      "included.  NAMED COST: a genuine single-band slip on a reference "
                      "arc also stops narrowing when its only silent pair is the untested "
                      "one.  Run-end reports `ref-arc refusals` with a per-pair "
                      "breakdown; 0 on a leg whose event sits on a known reference arc is "
                      "dead wiring.");
        if (network_cycle_slip_relevel_free_epoch_ && network_carry_invariant_) {
            log->warn("Network: network.cycle_slip.relevel_free_epoch (and its "
                      "delta_bridge rider) is inert under network.carry_invariant "
                      "(the leveling local carry is superseded and structurally "
                      "empty there) — both disabled.");
            network_cycle_slip_relevel_free_epoch_ = false;
            network_cycle_slip_relevel_delta_bridge_ = false;
            // Rider: the lane observatory reads local_carry.value_abs and the
            // freed set, both structurally empty here.  Riders die with their
            // base (SPEC_lane_slip_response §7.3).
            network_cycle_slip_relevel_lane_snap_ = false;
        }
        // Card G RESPONSE interlocks (docs/SPEC_slip_qc_hardening.md §G3/W1),
        // deliberately HERE for the same load-order reason as the block above:
        // network.carry_invariant parses ~1500 lines after the cycle_slip block,
        // and the relevel_free_epoch disarm it drives has just run, so this is
        // the first point where BOTH vehicles' flags are settled.
        //
        // WARN-ONLY, never warn-and-disarm — the opposite of the pattern
        // everywhere else in this band, and deliberately so.  The response IS
        // wired without the free-epoch latch (it marks pending-reseed and feeds
        // the C3c funnel); what it loses is the ability to make the mark SURVIVE
        // to the PASS-B leveling inject, which consumes it in its pre-pass
        // (SPEC_slip_relevel_walk §6's root defect).  Disarming would hide the
        // size of that loss; instead the drain counts every would-have-freed arc
        // as `gap_reentry_no_relevel` and the run-end Card-G line reports it.
        if (network_config_.slip_qc.gap_reentry
            && !network_cycle_slip_relevel_free_epoch_
            && !network_carry_invariant_ && network_phase_leveling_) {
            log->warn("Network (slip-QC card G response): network.cycle_slip.gap_reentry_relevel "
                      "is armed on a PHASE-LEVELING lineage with cycle_slip.relevel_free_epoch "
                      "OFF.  The re-entry mark alone does NOT deliver the free epoch — PASS B's "
                      "measurement pre-pass consumes the pending-reseed flag before the leveling "
                      "inject reads it, so the stale pre-gap level still folds across the break "
                      "(SPEC_slip_relevel_walk §6).  The response is NOT disabled: every arc it "
                      "would have freed is counted as gap_reentry_no_relevel and reported in the "
                      "run-end Card-G line, so the loss is measured rather than hidden.  Arm "
                      "network.cycle_slip.relevel_free_epoch to make it act.");
        }
        if (network_config_.slip_qc.gap_reentry
            && !network_phase_leveling_ && !network_carry_invariant_) {
            log->warn("Network (slip-QC card G response): network.cycle_slip.gap_reentry_relevel "
                      "is armed with NEITHER network.phase_leveling NOR network.carry_invariant — "
                      "there is no stiff time-linked carry for a re-entry to bridge, so the "
                      "response has nothing to protect and every arc will report as "
                      "gap_reentry_cold_skipped.  Armed-and-inert (standing invariant #4); left "
                      "on so the counters say so rather than being silently dropped.");
        }
        if (network_config_.slip_qc.gap_reentry)
            log->info("Network (slip-QC card G response): gap RE-ENTRY refloat ON — an arc that "
                      "re-appears after ≥1 missed processed epoch is marked SLIPPED in that same "
                      "epoch when the raw MW/GF level shifted across the break{}{}.  No detector "
                      "covers a return epoch (MW needs an adjacent predecessor, IF-TD three "
                      "consecutive epochs), so this is the ONLY wired response between such a "
                      "re-entry and the zero-q leveling carry bridging it (standing invariant "
                      "#1).  SOLVE-AFFECTING; both bands of the primary pair are freed, never "
                      "repaired — across a loss of lock the slip magnitude is unbounded and "
                      "unknowable.",
                      network_config_.slip_qc.gap_reentry_lli
                          ? ", or the receiver asserted LLI on either band at the return epoch"
                          : "",
                      network_config_.slip_qc.gap_reentry_unmeasurable
                          ? ", or no pre-gap dual-band level existed to check (conservative "
                            "reset)"
                          : "");
        // Card GRS rider interlock (docs/refound/GRS_gap_reentry_per_signal_2026-09-20.md).
        // WARN-ONLY and inert, not warn-and-disarm: the per-signal tier skips any
        // satellite that produced a ROW-level resumption in the same epoch, and
        // that list is only collected when the row-level card is armed, so
        // running the rider alone would let one break be counted (and responded
        // to) twice.  Left armed so the run-end counters state the inertness
        // rather than the knob vanishing silently (standing invariant #4).
        if (network_config_.slip_qc.gap_reentry_per_signal
            && !network_config_.slip_qc.gap_reentry) {
            log->warn("Network (slip-QC card GRS): network.cycle_slip.gap_reentry_per_signal "
                      "is armed WITHOUT network.cycle_slip.gap_reentry_relevel.  The per-signal "
                      "RESPONSE is gated on it and cannot act; the per-signal COLLECTION also "
                      "needs a row-level card armed (gap_instrument or gap_reentry_relevel), "
                      "because it de-duplicates against the row-level resumption list and "
                      "would otherwise answer a whole-satellite gap twice.  Every counter in "
                      "the run-end Card-G per-signal clause will read 0: armed-and-inert, not "
                      "a quiet run.  Arm network.cycle_slip.gap_reentry_relevel to make it act.");
        }
        if (network_config_.slip_qc.gap_reentry_per_signal
            && network_config_.slip_qc.gap_reentry)
            log->info("Network (slip-QC card GRS): PER-SIGNAL gap re-entry ON.  A phase signal "
                      "that re-appears after ≥1 missed processed epoch is marked SLIPPED even "
                      "when its satellite stayed present on another signal.  The row-level tier "
                      "is blind to that case by construction (the raw row survives on a "
                      "different pair, so the return epoch has an adjacent predecessor and no "
                      "detector covers it), and it is how a third band damages the L1/L2 wire.  "
                      "The level test is a geometry-free difference against a partner signal "
                      "snapshotted at the candidate's own last-seen epoch, gated by the "
                      "station's empirical GF scale; a re-entry with no usable partner is "
                      "{}.  SOLVE-AFFECTING and BAND-SCOPED: only the returning signal is "
                      "freed, never the bands that tracked through.",
                      network_config_.slip_qc.gap_reentry_per_signal_unmeasurable
                          ? "FREED (a partial dropout with no witness is the measured poison "
                            "case)"
                          : "left alone");
        // Card GRK (docs/refound/GRK_gap_reentry_key_mismatch_2026-09-22.md).
        // The knob changes which signal the arc-level tracker names per band.
        // With no Card-G consumer armed that choice reaches only the slip_raw
        // trace and the MW/GF detections, so the operator is told that the
        // response half of the fix is not in this posture.
        if (network_config_.slip_qc.tracker_modeled_first
            && !network_config_.slip_qc.gap_reentry
            && !network_config_.slip_qc.gap_instrument) {
            log->warn("Network (slip-QC card GRK): network.cycle_slip.tracker_modeled_first "
                      "is armed with NEITHER network.cycle_slip.gap_reentry_relevel NOR "
                      "network.cycle_slip.gap_instrument.  The defect it closes is a KEY "
                      "MISMATCH between the tracker's band signal and the carried ambiguity "
                      "the gap re-entry response looks up, and with no Card-G consumer armed "
                      "there is no such lookup: the changed choice reaches only the "
                      "trace_slip_raw rows and the MW/GF detections.  Arm "
                      "network.cycle_slip.gap_reentry_relevel for the response half.");
        }
        if (network_config_.slip_qc.tracker_modeled_first)
            log->info("Network (slip-QC card GRK): tracker MODELED-FIRST ON.  The arc-level "
                      "tracker takes, on each band, the phase signal this station LATCHED for "
                      "that band, and falls back to the lexically lowest raw phase with a "
                      "matching code only where the latch names none or the latched phase is "
                      "not reported this epoch.  The tracker's key, the carried "
                      "ambiguity key and the gap re-entry response's lookup then agree by "
                      "construction; unarmed, a station reporting L1C and L1W tracks band 1 "
                      "as L1C while its carry is keyed on the canonical latch (L1W under "
                      "processing.canonical_signal_rule = priority), the response counts "
                      "gap_reentry_no_carry and the stale level folds across the break.  "
                      "SOLVE-AFFECTING through the detections and the response; the "
                      "gap_tracker_key_mismatch and gap_reentry_unreachable counters are "
                      "reported at run end in BOTH knob states.");
        // SIM-ONLY ORACLE slip response interlocks (include/slip/NetworkSlipOracle.hpp),
        // HERE for the same load-order reason as the two blocks above: carry_invariant
        // and phase_leveling both parse well after the cycle_slip block.
        //
        // Warn-and-DISARM (not the Card-G warn-only shape): the oracle's whole
        // output is a ceiling number, and a ceiling measured on a vehicle it
        // cannot actuate is a wrong verdict, not a partial one.  It writes
        // LocalCarry::value_abs, so with no phase_leveling there is no carry to
        // write and under carry_invariant the leveling carry is structurally
        // empty (that vehicle owns its own C3c funnel).
        if (network_config_.slip_qc.oracle_relevel && network_carry_invariant_) {
            log->warn("Network (slip ORACLE): network.cycle_slip.oracle_relevel is INERT under "
                      "network.carry_invariant — the oracle shifts LocalCarry::value_abs, and "
                      "the leveling local carry is superseded and structurally EMPTY there "
                      "(the DD invariant carry owns its own slip funnel).  DISABLED: a ceiling "
                      "measured on a vehicle the mechanism cannot actuate would read as a "
                      "falsification of level preservation.");
            network_config_.slip_qc.oracle_relevel = false;
        }
        if (network_config_.slip_qc.oracle_relevel && !network_phase_leveling_) {
            log->warn("Network (slip ORACLE): network.cycle_slip.oracle_relevel needs "
                      "network.phase_leveling — that carry IS the vehicle it repairs "
                      "(LocalCarry::value_abs).  DISABLED.");
            network_config_.slip_qc.oracle_relevel = false;
        }
        if (network_config_.slip_qc.oracle_relevel) {
            log->warn("Network (slip ORACLE): oracle_relevel ON — SIM-ONLY DIAGNOSTIC, "
                      "SOLVE-AFFECTING.  Every detector refloat (MW wide-lane, RINEX LLI, "
                      "IF-phase TD, wind-up restart) on a CARRIED arc is answered from the "
                      "truth table at '{}'.  POLICY PAIR: a truth slip in the window is "
                      "'{}', an actuation with none is '{}'; injected noise sigma {:.4f} cyc; "
                      "detector-latency window {:.0f} s (a slip is repairable up to that far "
                      "back, bounded below by the carry's own BIRTH second so an erased-and-"
                      "reborn level is never re-shifted). "
                      " The gap-re-entry drain and the T4 attribution erase are deliberately "
                      "UNCHANGED (the question is what a perfect DETECTOR response buys with "
                      "everything else held fixed).  This measures the CEILING of a "
                      "level-preserving re-level — never promote it, never put it in a golden.",
                      network_config_.slip_qc.oracle_truth_root,
                      network_config_.slip_qc.oracle_true_slip,
                      network_config_.slip_qc.oracle_false_alarm,
                      network_config_.slip_qc.oracle_noise_cyc,
                      network_config_.slip_qc.oracle_window_s);
            // The two policies are independent, so the 2x2 has a corner in which
            // BOTH halves decline and the oracle actuates on nothing at all.  That
            // is a legitimate byte-identity control, not a typo — warn rather than
            // refuse, but say it out loud: a reader who expects a ceiling from this
            // arm would otherwise read a null result as a falsification (the
            // armed-and-inert failure standing invariant #4 names).
            if (network_config_.slip_qc.oracle_true_slip == "free"
                && network_config_.slip_qc.oracle_false_alarm == "free")
                log->warn("Network (slip ORACLE): true_slip 'free' + false_alarm 'free' is a "
                          "pure NO-OP arm — BOTH halves decline, so every actuation runs "
                          "today's ordinary response and the oracle writes nothing.  Legal "
                          "(it is the byte-identity control for the hook itself), but it "
                          "measures no ceiling: do not read its ZWD as a result.");
            else if (network_config_.slip_qc.oracle_true_slip == "free")
                log->warn("Network (slip ORACLE): true_slip 'free' — this is the FALSE-ALARM "
                          "ISOLATION arm, not the ceiling.  Real slips take today's ordinary "
                          "free/re-level response and only actuations with NO truth slip in "
                          "the window keep their level, so the run-end 'applied' count is 0 "
                          "BY CONSTRUCTION and the arm's actuation counter is "
                          "'false-alarm kept'.");
        }
        if (network_cycle_slip_relevel_retap_statistical_ && network_carry_invariant_) {
            log->warn("Network: network.cycle_slip.relevel_retap_statistical is inert "
                      "under network.carry_invariant (the leveling local carry is "
                      "superseded and structurally empty there) — disabled.");
            network_cycle_slip_relevel_retap_statistical_ = false;
            network_cycle_slip_relevel_retap_corroborate_ = false;   // rider dies with its base
        }

        // Slant-iono carry (QUEUE 8b) interlocks, deliberately HERE (same Card-1g
        // parse-order rationale as above).  All warn-and-disarm per invariant #4:
        // an armed-but-inert knob misleads worse than a refused one.
        if (network_carry_iono_ && network_carry_invariant_) {
            log->warn("Network: network.carry_iono scope=relevel keys on the relevel "
                      "response, which network.carry_invariant disarms (leveling local "
                      "carry superseded); the scope=always/carry_invariant pairing is "
                      "the future O2 accumulation card and is unvalidated — disabled.");
            network_carry_iono_ = false;
        }
        if (network_carry_iono_ && !network_phase_leveling_) {
            log->warn("Network: network.carry_iono requires network.phase_leveling "
                      "(the carry seam's harvest/inject/warmup infrastructure) — "
                      "disabled.");
            network_carry_iono_ = false;
        }
        if (network_carry_iono_ && estimate_network_iono_sh_) {
            log->warn("Network: network.carry_iono under network.estimate_iono_sh is "
                      "unvalidated — the carried station-common mode and the a00 column "
                      "form the F4-class quasi-null with no demean derivation yet "
                      "(docs/SPEC_iono2_mechB_v3.md) — disabled.");
            network_carry_iono_ = false;
        }
        if (network_carry_iono_ && global_vtec_sh_.rep_error_tau_s > 0.0) {
            log->warn("Network: network.carry_iono is mutually exclusive with the mech-B "
                      "deviation carry (estimation.global_vtec_sh.rep_error_tau_s > 0): "
                      "two temporal channels on the same physical column double-count — "
                      "disabled.");
            network_carry_iono_ = false;
        }
        if (network_carry_iono_ && network_iono_carry_scope_ == "relevel" &&
            !network_cycle_slip_relevel_free_epoch_) {
            log->warn("Network: network.carry_iono scope=relevel keys its slip-KEEP edge "
                      "on network.cycle_slip.relevel_free_epoch, which is off — the "
                      "detector-freed population would never inject — disabled.");
            network_carry_iono_ = false;
        }
        if (network_carry_iono_)
            log->info("Network producer (slant-iono carry, QUEUE 8b): carrying per-LOS "
                      "slant iono across epochs, scope={} — RW q {:.3e} m²/s, inject var "
                      "floor {:.3f} m², eviction gap {:.0f} s.  KEEP through freed slips, "
                      "ERASE on un-freed funnel entries and T4/consensus refloats.  The "
                      "expected win is the re-draw-event churn kill (the freed level "
                      "re-levels against the transported pre-event iono instead of one "
                      "epoch of GF code), NOT dm-class iono and NOT the O2/NL "
                      "accumulation channel on a leveled posture.",
                      network_iono_carry_scope_, network_iono_carry_q_,
                      network_iono_carry_var_floor_, network_iono_carry_max_gap_s_);

        // Card 4 (M1) D5 v3/v4 EVIDENCE interlock, deliberately HERE (the Card-1g order
        // rule above).  This used to sit beside each knob's own parse in
        // loadNetworkDatumSignalling_, which was TWO order bugs at once:
        //
        //  (a) it tested `network_config_.trace_slip_raw` as if the TRACE supplied the
        //      evidence.  It does not.  trace_slip_raw arms only the COMPUTE pass
        //      (SlipQcParams::anyConsumer); the per-(station, satellite) verdicts the D5
        //      consumers read are filled by the QUALIFY pass, which runs only for a
        //      DECISION card — SlipQcState::cardsArmed() = lli_corroborate ||
        //      gap_instrument || bridge_gf || iono_cs
        //      (include/slip/NetworkRawObservables.hpp, gated at
        //      ApplicationNetworkAssembly.cpp:1417).  The failure is SILENT, not loud:
        //      the compute pass sets `epoch.computed = true` and `epoch.reset(n_sta)` SIZES
        //      `epoch.verdict` to n_sta with EMPTY maps, so the consumers' `epoch.computed`
        //      gate (NetworkInstallCarry.cpp:152 and :692) PASSES, every per-satellite
        //      lookup misses, and the suppression set arrives EMPTY.  The gate logs as
        //      armed and suppresses nothing — standing invariant #4 — and it is LATENT:
        //      those sets spring to life the moment ANY card arms (the LV-6 census arming
        //      the CS advance is exactly that), a hidden solve change on a knob nobody
        //      touched.  Filed at SPEC_level_authority_internal §5.3.
        //
        //  (b) it could not have been fixed in place.  `network_carry_iono_` — one of the
        //      two inputs behind `iono_cs` — is still the RAW parsed value inside
        //      loadNetworkDatumSignalling_; the five carry_iono interlocks a few lines
        //      above run LATER and can clear it, and the FIRST of them (carry_iono under
        //      carry_invariant) fires on precisely the carry_invariant lineages these D5
        //      knobs live on.  Testing it at the parse site would have re-opened the hole
        //      in a new form.  Here every input is final.
        //
        // The two derived arms mirror ApplicationNetwork.cpp's construction of
        // SlipQcParams exactly (sp.bridge_gf at :310, sp.iono_cs at :317) — keep them in
        // step.  gap_instrument (Card G) is instrumentation-only (counters + trace rows,
        // verified byte-identical), which is why it, and never the mw_lli_corroborate
        // DECISION card, is what gets auto-armed: a decision card would perturb the very
        // population being measured.  Same choice the prior-residual (:3718) and
        // block-Baarda (:4221) census loaders make.
        //
        // Census, 2026-08-21: across all 242 configs in the repo that resolve either knob
        // ON, `cycle_slip.gap_instrument` already resolves TRUE — so this interlock fires
        // on ZERO committed lineages and the fix is a provable no-op on the corpus.  It
        // closes a latent hole for future configs, nothing more.
        if (network_carry_invariant_innov_corroborate_
            || network_carry_invariant_innov_retap_only_) {
            const bool iono_cs_armed =
                (network_carry_iono_ && network_iono_carry_source_ == "smoothed_gf")
                || network_level_authority_csgf_;
            const bool cards_armed = network_config_.slip_qc.lli_corroborate
                                     || network_config_.slip_qc.gap_instrument
                                     || network_cycle_slip_relevel_bridge_gf_
                                     || iono_cs_armed;
            if (!cards_armed) {
                network_config_.slip_qc.gap_instrument = true;
                log->info("Network (Card 4 D5 {}): auto-arming "
                          "network.cycle_slip.gap_instrument — the raw MW/GF step verdicts "
                          "it needs are filled by the slip-QC QUALIFY pass, which runs only "
                          "for a DECISION card (cycle_slip.mw_lli_corroborate / "
                          "cycle_slip.gap_instrument / cycle_slip.relevel_bridge_gf / the "
                          "CS-GF authority).  network.trace_slip_raw does NOT qualify — it "
                          "computes the observables but adjudicates none, so the suppression "
                          "set would have arrived EMPTY and the gate would have been silently "
                          "inert.  Card G is instrumentation-only (counters + trace rows), so "
                          "the solve is unchanged.",
                          network_carry_invariant_innov_corroborate_
                              ? (network_carry_invariant_innov_retap_only_ ? "v3/v4" : "v3")
                              : "v4");
            }
        }

        // Withhold-era release interlocks (SPEC_withhold_era_release §3), deliberately
        // HERE (Card-1g order rule): both responders ride the A4 withhold path, and the
        // ceiling can be armed as late as the service-mode bundle; relevel_free_epoch
        // parses in the cycle_slip block above.  Warn-and-disarm per invariant #4.
        if (network_raim_withhold_adapt_escalate_ > 0 && network_raim_withhold_sigma0_ <= 0.0) {
            log->warn("Network (withhold-era): network.raim_withhold_adapt_escalate needs an "
                      "armed A4 ceiling (network.raim_withhold_sigma0 > 0) — without it there "
                      "are no withhold eras to escalate inside — disabled.");
            network_raim_withhold_adapt_escalate_ = 0;
        }
        if (network_raim_withhold_free_erase_ && network_raim_withhold_sigma0_ <= 0.0) {
            log->warn("Network (withhold-era): network.raim_withhold_free_erase needs an armed "
                      "A4 ceiling (network.raim_withhold_sigma0 > 0) — the erase site is the "
                      "withhold branch — disabled.");
            network_raim_withhold_free_erase_ = false;
        }
        if (network_raim_withhold_free_erase_ && !network_cycle_slip_relevel_free_epoch_) {
            log->warn("Network (withhold-era): network.raim_withhold_free_erase consumes the "
                      "free-epoch latch, which network.cycle_slip.relevel_free_epoch fills — "
                      "without it the latch is always empty — disabled.");
            network_raim_withhold_free_erase_ = false;
        }
        // QUEUE item 4 R2/R4 interlocks (Card-1g site: raim and trace_flag_rates are
        // both settled by here, including the service-mode arming block).
        if (network_raim_station_factor_ && !network_raim_) {
            log->warn("Network (R2): network.raim_station_factor rides the post-fit RAIM "
                      "seam (Pass 1 residuals), which network.raim arms — disabled.");
            network_raim_station_factor_ = false;
        }
        if (network_raim_rate_hygiene_ && !network_raim_) {
            log->warn("Network (R4): network.raim_rate_hygiene corrects the T3/T3c "
                      "opportunity accounting inside the post-fit RAIM seam — without "
                      "network.raim there is nothing to correct — disabled.");
            network_raim_rate_hygiene_ = false;
        }
        if (network_raim_rate_hygiene_ && !network_config_.trace_flag_rates) {
            log->warn("Network (R4): network.raim_rate_hygiene corrects flag-rate "
                      "counters that only exist under network.trace_flag_rates — "
                      "disabled.");
            network_raim_rate_hygiene_ = false;
        }
        if (network_raim_station_factor_)
            log->info("Network (R2 station factor, SPEC_t3_null_derivation §6): DECLARED "
                      "per-station variance component (trim at k·σ̂₀, EWMA τ {:.0f} s, "
                      "depart ratio {:.1f} / return at 0.8×) — measure-only; feeds NO "
                      "denominator (absorption is R1, HELD behind the measured per-station "
                      "iono error it would bury).",
                      network_raim_station_factor_tau_s_,
                      network_raim_station_factor_ratio_);

        if (network_raim_withhold_adapt_escalate_ > 0 || network_raim_withhold_free_erase_)
            log->info("Network (withhold-era release, SPEC_withhold_era_release): {}{}{} — "
                      "era edges evented as SIGMA0/era_enter|era_exit with both-edge "
                      "counters; the A4 verdict statistic and ceiling are unchanged.",
                      network_raim_withhold_adapt_escalate_ > 0
                          ? fmt::format("escalated era adaptation budget {} (stock 5)",
                                        network_raim_withhold_adapt_escalate_)
                          : "",
                      network_raim_withhold_adapt_escalate_ > 0 &&
                              network_raim_withhold_free_erase_
                          ? " + "
                          : "",
                      network_raim_withhold_free_erase_
                          ? "detector-freed carry erase at the withhold branch"
                          : "");

        logNetworkPosture_(log);
        return true;
    }

    // =================================================================
    //  network subsection helper: loadNetworkProcessNoise_
    // =================================================================
    /**
     * @brief Resolve the scoped per-parameter-class process-noise table (@c network.process_noise).
     *
     * Runs FIRST in the section: classes whose legacy key lives in an EARLIER section are
     * written through here, while classes parsed LATER in this section pick the resolved
     * value up as their @c jval default at their own parse site.  Rejects a non-numeric or
     * negative rate and an unknown class name (an inert misspelling is invariant #4's
     * quietest failure).  @see finalizeNetworkProcessNoise_ for the post-clamp settle.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     * @return @c false on a fatal misconfiguration (aborts @c loadFromJson).
     */
    bool GnssConfig::loadNetworkProcessNoise_(const nlohmann::json& json,
                                              const std::shared_ptr<spdlog::logger>& log) {
        // --- Scoped per-parameter-class process noise (network.process_noise) ---
        // One authoring surface for the eleven time-linked NETWORK-OWNED q's that
        // were previously spelled across five config sections.  Resolved HERE,
        // first thing in the section, because the resolution feeds two different
        // mechanisms depending on where the legacy key is parsed:
        //
        //   * classes whose legacy key belongs to an EARLIER section
        //     (filter.process_noise.*, estimation.global_vtec_sh.*) are already
        //     in their canonical members, so the table writes through to them
        //     below — but only where the legacy key was not set explicitly;
        //   * classes parsed LATER in THIS section pass the resolved value as the
        //     `jval` DEFAULT at their existing parse site, so `jval`'s own
        //     "explicit key beats default" rule supplies the precedence and the
        //     parse sites keep their shape.  This ordering also keeps the derived
        //     network.ura_staleness_q2 correct: it defaults to clock_model_q2 and
        //     is captured after it, so it inherits the resolved value for free.
        //
        // Every class defaults to its legacy key's OWN default (kNetworkQClassTable
        // asserts that equality in a unit test), so a config with no
        // network.process_noise block resolves to today's numbers exactly: the
        // table is live by default and still byte-identical until a value is edited.
        {
            using QC  = NetworkQClass;
            using Src = NetworkProcessNoise::Source;
            auto& npn = network_config_.process_noise;
            const auto& pn_sec = jsection(json, "network.process_noise", "network_process_noise");
            for (std::size_t i = 0; i < kNetworkQClassCount; ++i) {
                const NetworkQDesc& d = kNetworkQClassTable[i];
                const bool legacy_set = jhas(json, d.legacy_path, d.legacy_flat);
                const bool table_set  = pn_sec.is_object() && pn_sec.contains(d.name);
                double v   = d.fallback;
                Src    src = Src::Default;
                if (table_set) {
                    // jval/value() would THROW out of loadFromJson on a string or
                    // object here, with no config-path context in the message.  A
                    // q table invites `"sat_clock": "1e-2"`, so name the offender.
                    if (!pn_sec[d.name].is_number()) {
                        log->error("Network: network.process_noise.{} must be a NUMBER (got {}); "
                                   "quote-free scientific notation is fine (1e-2), a quoted "
                                   "string is not.", d.name, pn_sec[d.name].type_name());
                        return false;
                    }
                    v = pn_sec.value(d.name, d.fallback);
                    if (!(v >= 0.0)) {
                        log->error("Network: network.process_noise.{} must be >= 0 (got {}); a "
                                   "negative process-noise rate is meaningless.", d.name, v);
                        return false;
                    }
                    src = Src::Table;
                }
                if (table_set && legacy_set) {
                    log->warn("Network: process-noise class '{}' is set BOTH in "
                              "network.process_noise (={}) and via the legacy key '{}' — the "
                              "LEGACY key wins (explicit beats inherited).  Drop one; the "
                              "table entry is being ignored for this run.",
                              d.name, v, d.legacy_path);
                }
                npn.set(static_cast<QC>(i), v, src);
            }
            // A misspelled class would otherwise be silently inert — the exact
            // "looks armed, does nothing" failure the run-end telemetry rule
            // exists to prevent.  Reject it at load instead of at post-mortem.
            if (pn_sec.is_object()) {
                for (auto it = pn_sec.begin(); it != pn_sec.end(); ++it) {
                    bool known = false;
                    for (const NetworkQDesc& d : kNetworkQClassTable)
                        if (it.key() == d.name) { known = true; break; }
                    if (known) continue;
                    std::string valid;
                    for (const NetworkQDesc& d : kNetworkQClassTable) {
                        if (!valid.empty()) valid += ", ";
                        valid += d.name;
                    }
                    log->error("Network: unknown process-noise class "
                               "'network.process_noise.{}'.  Valid classes: {}.",
                               it.key(), valid);
                    return false;
                }
            }
            // Write-through for the classes whose legacy key lives in an earlier
            // section and is therefore already resolved into its canonical member.
            // Skipped when that key was set explicitly, so an existing config wins.
            auto applyEarly = [&](QC c, double& member) {
                const NetworkQDesc& d = kNetworkQClassTable[static_cast<std::size_t>(c)];
                if (npn.source(c) == Src::Table && !jhas(json, d.legacy_path, d.legacy_flat))
                    member = npn.get(c);
            };
            applyEarly(QC::SatClock,     process_noise_.sat_clock);
            applyEarly(QC::SatPhaseBias, process_noise_.sat_phase_bias);
            applyEarly(QC::SatCodeBias,  process_noise_.sat_code_bias);
            applyEarly(QC::VtecSh,       global_vtec_sh_.q_per_degree);
            applyEarly(QC::VtecShLayer2, global_vtec_sh_.layer2_q_per_degree);
            // loadEstimationSection_ forces a non-positive layer-2 q back to the
            // default; that guard ran before this write-through, so re-assert it
            // rather than letting a table-supplied 0 through a path the rest of the
            // dual-layer code assumes is positive.
            if (!(global_vtec_sh_.layer2_q_per_degree > 0.0)) {
                log->warn("Network: network.process_noise.vtec_sh_layer2 must be > 0 (got {}); "
                          "restoring {}.", global_vtec_sh_.layer2_q_per_degree,
                          kNetworkQClassTable[static_cast<std::size_t>(QC::VtecShLayer2)].fallback);
                global_vtec_sh_.layer2_q_per_degree =
                    kNetworkQClassTable[static_cast<std::size_t>(QC::VtecShLayer2)].fallback;
                npn.set(QC::VtecShLayer2, global_vtec_sh_.layer2_q_per_degree, Src::Default);
            }
        }

        return true;
    }

    // =================================================================
    //  network subsection helper: loadNetworkRosterAndDatum_
    // =================================================================
    /**
     * @brief Peer-station roster, RTA-12 contract identity, and the datum declaration fields.
     *
     * Owns @c network.stations (incl. each station's optional RTF-3 @c transport block),
     * @c network.{frame_id,coord_epoch,use_sif_coordinates,coord_age_warn_years} and
     * @c network.{clock_datum_station,clock_datum_satellite,epoch_sync_tolerance_s}.
     * The roster must be built before anything that counts or names stations.
     *
     * @param json  Parsed root config object.
     * @param nw    The @c network section object (already validated non-null).
     * @param log   The "gnss_config" logger.
     * @return @c false on a fatal misconfiguration (aborts @c loadFromJson).
     */
    bool GnssConfig::loadNetworkRosterAndDatum_(const nlohmann::json& json,
                                                const nlohmann::json& nw,
                                                const std::shared_ptr<spdlog::logger>& log) {
        // --- Peer stations (>=2; each holds coordinates fixed) ---
        network_config_.stations.clear();
        auto addStation = [&](const nlohmann::json& sj) {
            StationConfig sc;
            sc.name       = sj.value("name", std::string{});
            sc.rinex_path = sj.value("rinex_path", std::string{});
            sc.fixed      = sj.value("fixed", true);  // network holds station coordinates fixed
            if (sj.contains("position_ecef") && sj["position_ecef"].is_array()
                && sj["position_ecef"].size() == 3) {
                sc.position_ecef = {
                    sj["position_ecef"][0].get<double>(),
                    sj["position_ecef"][1].get<double>(),
                    sj["position_ecef"][2].get<double>()
                };
                sc.has_position = true;
            }
            // RTF-3: optional per-station live-transport descriptor (the
            // network.streaming lanes).  Absent — the norm for every batch
            // config — leaves sc.transport at its default (input_type "" = not
            // streamed): parse-only, no behavior anywhere until streaming arms.
            if (sj.contains("transport") && sj["transport"].is_object()) {
                const auto& tj = sj["transport"];
                auto& tc = sc.transport;
                tc.input_type        = tj.value("input_type", std::string{});
                tc.host              = tj.value("host", std::string{});
                tc.port              = tj.value("port", 2101);
                tc.mountpoint        = tj.value("mountpoint", std::string{});
                tc.username          = tj.value("username", std::string{});
                tc.password          = tj.value("password", std::string{});
                tc.use_tls           = tj.value("use_tls", false);
                tc.tls_insecure      = tj.value("tls_insecure", false);
                tc.input_path        = tj.value("input_path", std::string{});
                tc.reconnect_delay_s = tj.value("reconnect_delay_s", 5);
            }
            network_config_.stations.push_back(std::move(sc));
        };
        if (nw.contains("stations") && nw["stations"].is_array()) {
            for (const auto& sj : nw["stations"])
                if (sj.is_object()) addStation(sj);
        }

        if (network_config_.stations.size() < 2) {
            log->error("Network: need >=2 stations in network.stations (got {})",
                       network_config_.stations.size());
            return false;
        }

        // --- RTA-12: contract identity declarations + SIF roster coordinates ---
        // frame_id / coord_epoch are pure declarations stamped into the product
        // S-contract (frame=/cepoch= fields; empty = omitted, byte-identical).
        network_config_.frame_id =
            jval<std::string>(json, "network.frame_id", "frame_id", std::string{});
        network_config_.coord_epoch =
            jval<std::string>(json, "network.coord_epoch", "coord_epoch", std::string{});
        network_config_.use_sif_coordinates =
            jval<bool>(json, "network.use_sif_coordinates", "use_sif_coordinates", false);
        network_config_.coord_age_warn_years =
            jval<double>(json, "network.coord_age_warn_years", "coord_age_warn_years", 0.0);

        // Run-date hint for SIF velocity propagation and the coordinate-age
        // advisory: the data_download {year, doy} pair (loadDataDownloadSection_
        // has already run), re-used as the generic date hint exactly as
        // PathResolver does under data_source=internal.  0/0 = no hint.
        std::optional<double> run_year;
        if (download_config_.year > 0 && download_config_.doy > 0) {
            const int  y    = download_config_.year;
            const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
            run_year = static_cast<double>(y)
                     + (static_cast<double>(download_config_.doy) - 0.5) / (leap ? 366.0 : 365.0);
        }

        std::size_t n_sif_resolved = 0;
        for (auto& s : network_config_.stations) {
            if (s.name.empty()) {
                log->error("Network: every station needs a non-empty 'name'");
                return false;
            }
            // An empty rinex_path is tolerated (PathResolver fills it from the
            // internal tree by station name); a known position is required (the
            // network holds coordinates fixed, so it cannot bootstrap them).
            // RTA-12: under network.use_sif_coordinates a missing position_ecef
            // may be resolved from the loaded SIF registry — the ONLY relaxation
            // of the hard error; explicit JSON ECEF always wins (this branch is
            // never entered for it), and with the toggle off the pre-RTA-12 hard
            // error below is unchanged.
            if (!s.has_position && network_config_.use_sif_coordinates) {
                if (!station_info_) {
                    log->error("Network: station '{}' has no position_ecef and "
                               "network.use_sif_coordinates is on, but no SIF registry is "
                               "loaded — arm processing.enable_station_info (+ "
                               "files.station_info_path/s) or supply position_ecef.",
                               s.name);
                    return false;
                }
                if (!run_year) {
                    log->error("Network: station '{}' has no position_ecef and SIF "
                               "velocity propagation needs a run date — set "
                               "data_download.year/doy or supply position_ecef.",
                               s.name);
                    return false;
                }
                const auto rc = resolveRosterCoordinate(*station_info_, s.name, *run_year);
                if (!rc) {
                    log->error("Network: station '{}' has no position_ecef and could not "
                               "be resolved from the SIF registry (unknown station or no "
                               "usable occupation at {:.4f}) — supply position_ecef.",
                               s.name, *run_year);
                    return false;
                }
                s.position_ecef     = { rc->ecef.x(), rc->ecef.y(), rc->ecef.z() };
                s.has_position      = true;
                s.position_from_sif = true;
                ++n_sif_resolved;
            }
            if (!s.has_position) {
                log->error("Network: station '{}' needs a known 'position_ecef' (held fixed)", s.name);
                return false;
            }
        }
        if (network_config_.use_sif_coordinates) {
            // Default the declared coordinate epoch to the propagation date when the
            // user left it empty and any coordinate was actually SIF-propagated —
            // the S-contract cepoch= field then states the epoch the roster is
            // really valid at.  An explicit network.coord_epoch always wins.
            if (n_sif_resolved > 0 && network_config_.coord_epoch.empty()) {
                network_config_.coord_epoch = fmt::format("{:.4f}", *run_year);
                log->info("Network (RTA-12): network.coord_epoch defaulted to the SIF "
                          "propagation date {} (decimal year).", network_config_.coord_epoch);
            }
            // Per-station provenance, once at roster build (toggle-gated so a
            // default run's log is unchanged).
            for (const auto& s : network_config_.stations)
                log->info("Network roster: station '{}' coordinates {} "
                          "({:.4f}, {:.4f}, {:.4f}) m",
                          s.name,
                          s.position_from_sif
                              ? fmt::format("SIF-propagated to {:.4f}", *run_year)
                              : "json-explicit",
                          s.position_ecef[0], s.position_ecef[1], s.position_ecef[2]);
        }
        // RTA-12(4): cheap coordinate-age advisory.  Plate motion is ~2-7 cm/yr, so
        // fixed roster coordinates more than coord_age_warn_years beyond their
        // declared epoch should be re-propagated before they poison a cm-level
        // product datum.  Advisory ONLY (single WARN) — datum-event machinery
        // (automatic re-propagation / product re-basing on a coordinate update) is
        // an RTF-scale follow-on, deliberately out of this card's scope.
        if (network_config_.coord_age_warn_years > 0.0
            && !network_config_.coord_epoch.empty() && run_year) {
            const auto epoch_year = parseCoordEpochYear(network_config_.coord_epoch);
            if (!epoch_year) {
                log->warn("Network: network.coord_epoch '{}' is not parseable (decimal "
                          "year or YYYY-MM-DD) — coordinate-age advisory skipped.",
                          network_config_.coord_epoch);
            } else if (*run_year - *epoch_year > network_config_.coord_age_warn_years) {
                log->warn("Network: roster coordinate epoch {} is {:.2f} years before the "
                          "run date ({:.4f}), beyond network.coord_age_warn_years = {:.2f} "
                          "— plate motion is ~2-7 cm/yr; re-propagate the station "
                          "coordinates (or arm network.use_sif_coordinates).",
                          network_config_.coord_epoch, *run_year - *epoch_year,
                          *run_year, network_config_.coord_age_warn_years);
            }
        }

        // --- Datum fields ---
        network_config_.clock_datum_station =
            jval<std::string>(json, "network.clock_datum_station", "clock_datum_station", std::string{});
        network_config_.clock_datum_satellite =
            jval<std::string>(json, "network.clock_datum_satellite", "clock_datum_satellite", std::string{});
        network_config_.epoch_sync_tolerance_s =
            jval<double>(json, "network.epoch_sync_tolerance_s", "epoch_sync_tolerance_s", 0.01);

        return true;
    }

    // =================================================================
    //  network subsection helper: loadNetworkProducerAr_
    // =================================================================
    /**
     * @brief Producer-side network AR gates (@c network.ambiguity_resolution.*).
     *
     * The SOLE parse site.  Until 2026-08-01 these keys were read twice — here and
     * again at the end of the section — with identical paths and identical defaults,
     * so the second read overwrote the first with the same values and both logged.  An
     * armed config therefore got two contradictory lines ("AR ON" then "NOT
     * IMPLEMENTED").  Collapsed to one read and one accurate line.
     *
     * Gates are deliberately stricter than the user path (FFR 0.9999, per-pair frac
     * 0.10, partial AR on) because a wrong network fix poisons every downstream user.
     * Default @c enable=false => the AR block is skipped entirely => byte-identical
     * float product (docs/network_ambiguity_resolution_design.md).
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkProducerAr_(const nlohmann::json& json,
                                            const std::shared_ptr<spdlog::logger>& log) {
        {
            auto& ar = network_config_.ambiguity_resolution;
            ar.enable = jval<bool>(json, "network.ambiguity_resolution.enable",
                                   "network_ar_enable", false);
            ar.min_success_rate = jval<double>(json, "network.ambiguity_resolution.min_success_rate",
                                               "network_ar_min_success_rate", 0.9999);
            ar.ratio_threshold = jval<double>(json, "network.ambiguity_resolution.ratio_threshold",
                                              "network_ar_ratio_threshold", 3.0);
            ar.ratio_upper_bound = jval<double>(json, "network.ambiguity_resolution.ratio_upper_bound",
                                                "network_ar_ratio_upper_bound", 1e4);
            ar.nl_max_per_pair_frac = jval<double>(json, "network.ambiguity_resolution.nl_max_per_pair_frac",
                                                   "network_ar_nl_max_per_pair_frac", 0.10);
            ar.enable_partial_ar = jval<bool>(json, "network.ambiguity_resolution.enable_partial_ar",
                                              "network_ar_enable_partial_ar", true);
            ar.wl_cascade = jval<bool>(json, "network.ambiguity_resolution.wl_cascade",
                                       "network_ar_wl_cascade", false);
            ar.min_epochs = jval<int>(json, "network.ambiguity_resolution.min_epochs",
                                      "network_ar_min_epochs", 10);
            // Cap on the MLAMBDA search dimension (0 = uncapped legacy).  Required at
            // scale: an uncapped ~2500-dim basis at 305 stations does not return.
            ar.ils_max_dim = jval<int>(json, "network.ambiguity_resolution.ils_max_dim",
                                       "network_ar_ils_max_dim", 0);
            // Per-DD trace of the search (logs/network_producer_ar.csv).  A fix COUNT
            // is not a grade: without the per-DD identities the epoch log line cannot
            // be joined to truth_ambiguities.csv, which is the whole point of re-running
            // Phase 2 (docs/network_ambiguity_resolution_design.md, 2026-09-10 §4.1).
            ar.trace = jval<bool>(json, "network.ambiguity_resolution.trace",
                                  "network_ar_trace", false);
            if (ar.trace && !ar.enable) {
                log->warn("Network producer AR: ambiguity_resolution.trace is ON but "
                          ".enable is OFF — the Phase-2 search never runs, so there is "
                          "nothing to trace.  network_producer_ar.csv is NOT opened "
                          "(no header-only file).  Arm .enable to get rows.");
            }
            if (ar.enable) {
                // RT0-2 (revised 2026-08-01).  The original warning said the section was
                // "parsed but unused" with "inert gates".  That was true when RT0-2 was
                // written, and is NOT true now: decomposition INC-9b (2026-07-22) landed
                // networkProducerAr() (src/core/filter/NetworkProducerAr.cpp), the Phase-2
                // DD-fixability diagnostic, which consumes eight of these nine gates —
                // min_epochs and ils_max_dim directly, and min_success_rate /
                // ratio_threshold / ratio_upper_bound / nl_max_per_pair_frac /
                // enable_partial_ar through ils::IlsGateConfig.  Only wl_cascade has no
                // consumer.
                //
                // What has NOT changed is the load-bearing half of the warning, so it stays
                // a warn: Phase 2 is READ-ONLY (it writes nothing back into the run context
                // and re-pins nothing — that is Phase 3), so the EMITTED PRODUCT IS STILL
                // FLOAT.  Do not phrase this as "ENABLED": that implies integer fixing
                // reaches the wire.
                log->warn("Network producer AR ARMED, but the emitted product stays FLOAT — "
                          "no integer fixing is applied to it.  What the gates actually drive "
                          "is the Phase-2 LOG-ONLY DD-fixability diagnostic "
                          "(networkProducerAr; 'Network AR (Phase 2, log-only)' lines, from "
                          "epoch min_epochs={}): it reports how many between-station DDs the "
                          "network COULD fix, and re-pin / re-solve is Phase 3 and unbuilt.  "
                          "Live gates: FFR>={:.4f}, ratio>={:.1f} (<{:.0e}), per-pair "
                          "frac<={:.2f} cyc, partial_ar {}, ils_max_dim {}, trace {}.  "
                          "INERT: wl_cascade{}.  GPS+Galileo only; GLONASS stays float.",
                          ar.min_epochs, ar.min_success_rate, ar.ratio_threshold,
                          ar.ratio_upper_bound, ar.nl_max_per_pair_frac,
                          ar.enable_partial_ar ? "on" : "off",
                          ar.ils_max_dim > 0 ? std::to_string(ar.ils_max_dim)
                                             : std::string("uncapped"),
                          ar.trace ? "ON (logs/network_producer_ar.csv)" : "off",
                          ar.wl_cascade ? " (set true, but no consumer reads it)"
                                        : "");
            }
        }
    }

    // =================================================================
    //  network subsection helper: loadNetworkLaneLayers_
    // =================================================================
    /**
     * @brief MW/WL lane layers: @c network.mw_wl.*, @c network.sd_ewl.*, @c network.lane_carry.*.
     *
     * Parse + range clamps only.  Every cross-knob mutex these layers participate in
     * (the carry_invariant requirement, the EWL/UWL/lane-carry contracts) is enforced later
     * by @c validateNetworkLaneLayers_, which runs after @c network.carry_invariant is
     * parsed.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkLaneLayers_(const nlohmann::json& json,
                                            const std::shared_ptr<spdlog::logger>& log) {
        // --- MW-seeded wide-lane pseudo-obs layer (M5 card WL-1; network.mw_wl.*) ---
        // Feeds the per-arc Melbourne-Wubbena Welford means, DD'd over the Card-4 chord
        // basis, into the solve as rank-K WL pseudo-obs blocks — the code-coherence
        // accumulator the per-epoch network lacks (docs/SPEC_mw_wl_card.md).  Default
        // enable=false => byte-identical.  The carry_invariant requirement + Phase-B
        // warnings are enforced in the carry_invariant validation block below (after
        // network.carry_invariant is parsed).
        {
            auto& mw = network_config_.mw_wl;
            mw.enable = jval<bool>(json, "network.mw_wl.enable", "network_mw_wl_enable", false);
            mw.min_arc_epochs = jval<int>(json, "network.mw_wl.min_arc_epochs",
                                          "network_mw_wl_min_arc_epochs", 10);
            mw.sigma_floor_cyc = jval<double>(json, "network.mw_wl.sigma_floor_cyc",
                                              "network_mw_wl_sigma_floor_cyc", 0.05);
            mw.max_rows_per_epoch = jval<int>(json, "network.mw_wl.max_rows_per_epoch",
                                              "network_mw_wl_max_rows_per_epoch", 0);
            mw.fix_enable = jval<bool>(json, "network.mw_wl.fix_enable",
                                       "network_mw_wl_fix_enable", false);
            mw.fix_sigma_cyc = jval<double>(json, "network.mw_wl.fix_sigma_cyc",
                                            "network_mw_wl_fix_sigma_cyc", 0.10);
            mw.fix_frac_cyc = jval<double>(json, "network.mw_wl.fix_frac_cyc",
                                           "network_mw_wl_fix_frac_cyc", 0.15);
            mw.fix_weight_sigma_cyc = jval<double>(json, "network.mw_wl.fix_weight_sigma_cyc",
                                                   "network_mw_wl_fix_weight_sigma_cyc", 0.01);
            mw.f6_dump = jval<bool>(json, "network.mw_wl.f6_dump",
                                    "network_mw_wl_f6_dump", false);
            // Row budget + block span for the F6 dump.  The legacy dump sampled 8 rows of ONE
            // block; that fixed size is a SAMPLE and says nothing about how many cycles the
            // layer built (a lesson paid for by misreading it as a ~99.7% quarantine).  Both
            // default to the legacy shape ⇒ an unset config dumps exactly as before.
            mw.f6_max_rows = jval<int>(json, "network.mw_wl.f6_max_rows",
                                       "network_mw_wl_f6_max_rows", 8);
            mw.f6_all_blocks = jval<bool>(json, "network.mw_wl.f6_all_blocks",
                                          "network_mw_wl_f6_all_blocks", false);
            mw.debug_r_scale = jval<double>(json, "network.mw_wl.debug_r_scale",
                                            "network_mw_wl_debug_r_scale", 1.0);
            mw.ewl_fold_rows = jval<bool>(json, "network.mw_wl.ewl_fold_rows",
                                          "network_mw_wl_ewl_fold_rows", false);
            if (mw.ewl_fold_rows)
                log->warn("network.mw_wl.ewl_fold_rows=true — F-A STUDY SHAPE: folding the EWL "
                          "integer rows into the solve re-creates the ~2x D5 elevation the "
                          "Jul-16 A/B attributed (border-mediated tension against the stiff "
                          "carries).  The WL cascade does not need the fold (it reads the "
                          "detector statistic).  Diagnostic only, NOT a production shape.");
            mw.d5_dump = jval<bool>(json, "network.mw_wl.d5_dump",
                                    "network_mw_wl_d5_dump", false);
            mw.lane_trace = jval<bool>(json, "network.mw_wl.lane_trace",
                                       "network_mw_wl_lane_trace", false);
            // --- SD PRE-REDUCE EWL constraint (docs/SPEC_sd_ewl_prereduce.md) ---------
            // The between-satellite SD extra-wide-lane MW statistic as a pseudo-obs row in
            // each station's OWN pre-reduce block, so it reaches the shared satellite PHASE
            // and CODE bias columns instead of a DD chord that is 80.7% eq34 tree pins.
            // Default OFF => zero rows appended => byte-identical.
            {
                auto& se = network_config_.sd_ewl;
                se.enable = jval<bool>(json, "network.sd_ewl.enable",
                                       "network_sd_ewl_enable", false);
                se.min_arc_epochs = jval<int>(json, "network.sd_ewl.min_arc_epochs",
                                              "network_sd_ewl_min_arc_epochs", 10);
                se.sigma_floor_m = jval<double>(json, "network.sd_ewl.sigma_floor_m",
                                                "network_sd_ewl_sigma_floor_m", 0.05);
                se.max_abs_z_m = jval<double>(json, "network.sd_ewl.max_abs_z_m",
                                              "network_sd_ewl_max_abs_z_m", 0.0);
                se.max_rows_per_station =
                    jval<int>(json, "network.sd_ewl.max_rows_per_station",
                              "network_sd_ewl_max_rows_per_station", 0);
                se.trace = jval<bool>(json, "network.sd_ewl.trace",
                                      "network_sd_ewl_trace", false);
                // --- increment 2a+2b: FIX the integer (2c, the zero-q hold, is not built)
                se.fix_enable = jval<bool>(json, "network.sd_ewl.fix_enable",
                                           "network_sd_ewl_fix_enable", false);
                se.fix_min_ffr = jval<double>(json, "network.sd_ewl.fix_min_ffr",
                                              "network_sd_ewl_fix_min_ffr", 0.999);
                se.fix_max_frac_cyc =
                    jval<double>(json, "network.sd_ewl.fix_max_frac_cyc",
                                 "network_sd_ewl_fix_max_frac_cyc", 0.15);
                se.fix_sigma_floor_m =
                    jval<double>(json, "network.sd_ewl.fix_sigma_floor_m",
                                 "network_sd_ewl_fix_sigma_floor_m", 1.0);
                if (se.fix_enable && !se.enable) {
                    // The integer has nowhere to go without the row layer: 2b folds
                    // inside networkAppendSdEwlRows_, which returns immediately when
                    // sd_ewl.enable is false.  Armed-and-dead is exactly the shape
                    // standing invariant #4 exists to catch, so refuse it loudly here
                    // rather than let the run report zero fixed rows.
                    log->warn("Network: network.sd_ewl.fix_enable is INERT without "
                              "network.sd_ewl.enable — the integer decision has no row "
                              "layer to fold into (networkAppendSdEwlRows_ returns "
                              "immediately when enable is false).  Disarming fix_enable.");
                    se.fix_enable = false;
                }
                if (se.fix_enable) {
                    if (!(se.fix_min_ffr > 0.0) || se.fix_min_ffr >= 1.0) {
                        log->warn("Network: network.sd_ewl.fix_min_ffr = {} is outside "
                                  "(0,1) — clamped to 0.999.  It is a PROBABILITY (the "
                                  "bootstrapped rounding success rate), not a sigma "
                                  "multiplier.", se.fix_min_ffr);
                        se.fix_min_ffr = 0.999;
                    }
                    if (!(se.fix_max_frac_cyc > 0.0) || se.fix_max_frac_cyc > 0.5) {
                        log->warn("Network: network.sd_ewl.fix_max_frac_cyc = {} is "
                                  "outside (0,0.5] — clamped to 0.15.  Above 0.5 the "
                                  "gate cannot reject anything (every real number is "
                                  "within half a cycle of some integer).",
                                  se.fix_max_frac_cyc);
                        se.fix_max_frac_cyc = 0.15;
                    }
                    if (!(se.fix_sigma_floor_m > 0.0)) {
                        log->warn("Network: network.sd_ewl.fix_sigma_floor_m = {} is "
                                  "non-positive — clamped to 1.0 m (amendment A1's "
                                  "measured per-SAMPLE honest point).",
                                  se.fix_sigma_floor_m);
                        se.fix_sigma_floor_m = 1.0;
                    }
                    log->info("Network SD EWL INTEGER FIX ON (increment 2a+2b) — Route B "
                              "between-station DD of the SD EWL decides the integer with "
                              "NO a-priori satellite bias (so it cannot be poisoned by "
                              "one), a network-COMMON tracking-channel pair is elected "
                              "per (constellation, satellite) so B^s cancels exactly in "
                              "the DD, and the A2 DD->SD datum propagates spanning-tree "
                              "fixes from a sticky per-component reference station.  "
                              "Gates: FFR >= {:.4f}, |frac| <= {:.3f} cyc, per-sample "
                              "sigma floor {:.2f} m.  A fixed row REMOVES its four local "
                              "ambiguity coefficients (rank reduction, not a stiff "
                              "prior) and hands the whole row weight to the shared "
                              "satellite PB/CB columns.  The zero-q HOLD (2c) is NOT "
                              "built: the decision is re-derived every epoch and an "
                              "integer that disagrees with the previous epoch on a "
                              "continuous arc is COUNTED and VETOED (dd_flip), which is "
                              "the standing-invariant-#1 event response.",
                              se.fix_min_ffr, se.fix_max_frac_cyc, se.fix_sigma_floor_m);
                }
                if (se.enable) {
                    if (se.min_arc_epochs < 2) {
                        log->warn("Network: network.sd_ewl.min_arc_epochs = {} is below 2 "
                                  "(arcZdMean needs >=2 LAGGED samples for a finite "
                                  "variance) — clamped to 2.", se.min_arc_epochs);
                        se.min_arc_epochs = 2;
                    }
                    if (!(se.sigma_floor_m > 0.0)) {
                        log->warn("Network: network.sd_ewl.sigma_floor_m = {} is "
                                  "non-positive — clamped to 1e-3 m.  The floor exists "
                                  "because arcZdMean serves a VARIANCE OF THE MEAN "
                                  "(~1/n), so a long quiet arc would otherwise dominate "
                                  "the shared border.", se.sigma_floor_m);
                        se.sigma_floor_m = 1e-3;
                    }
                    log->info("Network SD PRE-REDUCE EWL constraint ON — the "
                              "between-satellite SD extra-wide-lane MW statistic enters "
                              "each station's OWN pre-reduce block (min_arc_epochs={}, "
                              "sigma_floor={:.3f} m, max_abs_z={}, max_rows/sta={}).  "
                              "Reaches the shared satellite PHASE + CODE bias columns "
                              "directly; composes EXACTLY with the post-reduce Card-4 DD "
                              "fold.  FLOAT rows only — no integer fixing (increment 2, "
                              "unbuilt).  Rows are skipped at the clock-datum station "
                              "(no MW detector there) and wherever an arc is immature or "
                              "mixed-BIA-frame; watch the run-end counters.",
                              se.min_arc_epochs, se.sigma_floor_m,
                              se.max_abs_z_m > 0.0 ? fmt::format("{:.1f} m", se.max_abs_z_m)
                                                   : std::string("off"),
                              se.max_rows_per_station > 0
                                  ? std::to_string(se.max_rows_per_station)
                                  : std::string("unlimited"));
                }
            }

            // --- M5 card WL-3: lane-coordinate carry (docs/SPEC_lane_carry_card.md).
            // Parsed + clamped here; the carry_invariant / ewl_enable mutex + the ON
            // summary live in the validation block below (after network.carry_invariant
            // is parsed), mirroring the mw_wl mutexes.
            {
                auto& lc = network_config_.lane_carry;
                lc.enable = jval<bool>(json, "network.lane_carry.enable",
                                       "network_lane_carry_enable", false);
                // DEFAULT 0.01 (measured Jul-16, 15-ep A/B).  The SPEC §3.1 "10³ × q_nl"
                // numerology assumed q_nl ≈ 0; at the lineage qz = 1e-4 m²/s it adds
                // (√3 m ≈ 11.8 WL cyc)² of lane variance PER 30 s EPOCH — swamping every
                // accumulated lane measurement and destroying each FFR fix within one
                // epoch.  0.01×qz ≈ (0.04 cyc)²/epoch keeps the √N accumulation alive
                // (constrained circR 0.471 / med|f| 0.061 vs 0.251/0.247 at scale 1) while
                // still relaxing a stale lane in tens of epochs.  The lane starts DIFFUSE
                // and earns tightness only from measurements, so a large q_wl is not
                // needed to prevent unearned stiffness.
                lc.q_wl_scale = std::max(0.0,
                    jval<double>(json, "network.lane_carry.q_wl_scale",
                                 "network_lane_carry_q_wl_scale", 0.01));
                // DEFAULT 0.0 = keep the ISOTROPIC diffuse prior at birth.  The SPEC §3.1
                // mean-zero "code-seed scale" fresh lane prior is FALSIFIED on real data
                // (measured Jul-16: D5 691 vs 125 without it): the correction-frame origin
                // is NOT the code-seed lane — a cycle's ambiguities seed at different
                // epochs, so per-station clock error at seed time does NOT cancel in the
                // lane (× (f1−f2) ⇒ hundreds of cycles of mis-centering at σ=3 cyc).  The
                // honest code-seed-scale information enters instead through the FIRST
                // per-epoch MW lane increment (correctly centered on the detector value).
                // Positive values (study shape) clamp to ≥0.1 cyc.
                lc.sigma_wl_fresh_cyc =
                    jval<double>(json, "network.lane_carry.sigma_wl_fresh_cyc",
                                 "network_lane_carry_sigma_wl_fresh_cyc", 0.0);
                lc.sigma_wl_fresh_cyc = (lc.sigma_wl_fresh_cyc <= 0.0)
                    ? 0.0 : std::max(0.1, lc.sigma_wl_fresh_cyc);
                if (lc.sigma_wl_fresh_cyc > 0.0)
                    log->warn("Network: network.lane_carry.sigma_wl_fresh_cyc={} — the mean-zero "
                              "fresh lane prior is a FALSIFIED mechanism on real data (seed-time "
                              "clock diversity mis-centers it by O(10²) cycles; measured D5 "
                              "691-vs-125, Jul-16).  Study shape only; the default (0 = diffuse "
                              "birth + first MW increment) is the honest form.",
                              lc.sigma_wl_fresh_cyc);
                lc.lane_meas = jval<bool>(json, "network.lane_carry.lane_meas",
                                          "network_lane_carry_lane_meas", true);
                lc.bridge_meas = jval<bool>(json, "network.lane_carry.bridge_meas",
                                            "network_lane_carry_bridge_meas", true);
                // LF-1 INC-1 (docs/SPEC_lane_carry_card.md Appendix LF-1 §3.2): the optimized blocked/sparse
                // lane fold.  A PERF knob — it changes only HOW the linear algebra is applied
                // (O(K) sparse SET + hoisted symmetrize), never what folds — so it stays OUT of
                // the equivalence_shape discriminator below.  Equivalence class (NOT bit-identical).
                lc.fold_blocked = jval<bool>(json, "network.lane_carry.fold_blocked",
                                             "network_lane_carry_fold_blocked", false);
                // LF-1 INC-2 (§3.3): pinned-cycle increment skip.  Also a PERF knob (stays out
                // of the equivalence_shape discriminator).  within_n clamps to ≥1 (a 0 window
                // would resume increments the epoch after every decision — the skip never fires).
                lc.skip_pinned_increments =
                    jval<bool>(json, "network.lane_carry.skip_pinned_increments",
                               "network_lane_carry_skip_pinned_increments", false);
                lc.skip_pinned_within_n = std::max<long long>(1,
                    jval<long long>(json, "network.lane_carry.skip_pinned_within_n",
                                    "network_lane_carry_skip_pinned_within_n", 3));
                // LF-1 INC-4 (§3.5): component-parallel MW fold.  PERF knob (out of the
                // equivalence_shape discriminator); byte-identical to serial.
                lc.parallel_components =
                    jval<bool>(json, "network.lane_carry.parallel_components",
                               "network_lane_carry_parallel_components", false);
            }
            // --- M5 card WL-2: EWL-first tri-frequency cascade (SPEC_ewl_wl_cascade_card.md §4). ---
            mw.ewl_enable = jval<bool>(json, "network.mw_wl.ewl_enable",
                                       "network_mw_wl_ewl_enable", false);
            mw.ewl_min_arc_epochs = jval<int>(json, "network.mw_wl.ewl_min_arc_epochs",
                                              "network_mw_wl_ewl_min_arc_epochs", 2);
            mw.ewl_fix_sigma_cyc = jval<double>(json, "network.mw_wl.ewl_fix_sigma_cyc",
                                                "network_mw_wl_ewl_fix_sigma_cyc", 0.10);
            mw.ewl_fix_frac_cyc = jval<double>(json, "network.mw_wl.ewl_fix_frac_cyc",
                                               "network_mw_wl_ewl_fix_frac_cyc", 0.20);
            mw.wl_inject_enable = jval<bool>(json, "network.mw_wl.wl_inject_enable",
                                             "network_mw_wl_wl_inject_enable", false);
            mw.wl_fix_sigma_cyc = jval<double>(json, "network.mw_wl.wl_fix_sigma_cyc",
                                               "network_mw_wl_wl_fix_sigma_cyc", 0.10);
            mw.wl_fix_frac_cyc = jval<double>(json, "network.mw_wl.wl_fix_frac_cyc",
                                              "network_mw_wl_wl_fix_frac_cyc", 0.15);
            mw.bridge_max_sigma_cyc = jval<double>(json, "network.mw_wl.bridge_max_sigma_cyc",
                                                   "network_mw_wl_bridge_max_sigma_cyc", 0.15);
            mw.inject_disagree_min_cyc = jval<double>(json, "network.mw_wl.inject_disagree_min_cyc",
                                                      "network_mw_wl_inject_disagree_min_cyc", 0.75);
            mw.lane_max_abs_xlin_m = jval<double>(json, "network.mw_wl.lane_max_abs_xlin_m",
                                                  "network_mw_wl_lane_max_abs_xlin_m", 1000.0);
            // --- M5 card UWL-1: per-constellation ultra-wide-lane top tier (SPEC_uwl_card.md §5). ---
            mw.uwl_enable = jval<bool>(json, "network.mw_wl.uwl_enable",
                                       "network_mw_wl_uwl_enable", false);
            {
                const int gtb = jval<int>(json, "network.mw_wl.uwl_gal_top_band",
                                          "network_mw_wl_uwl_gal_top_band", 7);
                if (gtb == 7 || gtb == 8) {
                    mw.uwl_gal_top_band = static_cast<char>('0' + gtb);
                } else {
                    log->warn("Network: network.mw_wl.uwl_gal_top_band = {} is not 7 (E5b, "
                              "λ 9.768 m) or 8 (E5 AltBOC, λ 19.537 m) — keeping the default 7.",
                              gtb);
                    mw.uwl_gal_top_band = '7';
                }
            }
            mw.uwl_mid_enable = jval<bool>(json, "network.mw_wl.uwl_mid_enable",
                                           "network_mw_wl_uwl_mid_enable", false);
            // Increment 5: dual-top tier (band-8 preferred / (5,7) fallback + sibling closure).
            mw.uwl_dual_top = jval<bool>(json, "network.mw_wl.uwl_dual_top",
                                         "network_mw_wl_uwl_dual_top", false);
            mw.uwl_sib_gate = jval<bool>(json, "network.mw_wl.uwl_sib_gate",
                                         "network_mw_wl_uwl_sib_gate", false);
            // v2 census recovery: per-family lane pair groups.
            mw.uwl_family_pairs = jval<bool>(json, "network.mw_wl.uwl_family_pairs",
                                             "network_mw_wl_uwl_family_pairs", false);
            mw.uwl_family_min_arcs = jval<int>(json, "network.mw_wl.uwl_family_min_arcs",
                                               "network_mw_wl_uwl_family_min_arcs", 8);
            if (mw.uwl_family_min_arcs < 1) {
                log->warn("Network: network.mw_wl.uwl_family_min_arcs = {} is below 1 — "
                          "clamping to 1 (a pair group needs at least one arc).",
                          mw.uwl_family_min_arcs);
                mw.uwl_family_min_arcs = 1;
            }
            // 60-ep age-bias fix: per-sample estimable-iono de-bias of the bridge.
            mw.bridge_sample_debias = jval<bool>(json, "network.mw_wl.bridge_sample_debias",
                                                 "network_mw_wl_bridge_sample_debias", false);
            mw.uwl_cold_fix = jval<bool>(json, "network.mw_wl.uwl_cold_fix",
                                         "network_mw_wl_uwl_cold_fix", false);
            mw.uwl_cold_sigma_m = jval<double>(json, "network.mw_wl.uwl_cold_sigma_m",
                                               "network_mw_wl_uwl_cold_sigma_m", 0.7);
            mw.uwl_cold_min_lambda_m = jval<double>(json, "network.mw_wl.uwl_cold_min_lambda_m",
                                                    "network_mw_wl_uwl_cold_min_lambda_m", 8.0);
            if (!(mw.uwl_cold_sigma_m > 0.0)) {
                log->warn("Network: network.mw_wl.uwl_cold_sigma_m = {} is non-positive — "
                          "clamping to 0.7 m (the cold decision needs an honest a-priori R).",
                          mw.uwl_cold_sigma_m);
                mw.uwl_cold_sigma_m = 0.7;
            }
            if (mw.uwl_cold_min_lambda_m < 6.0) {
                log->warn("Network: network.mw_wl.uwl_cold_min_lambda_m = {} m would admit the "
                          "GPS 5.861 m EWL to the n=1 cold fix — the cold fix is a UWL privilege "
                          "earned by wavelength (SPEC_uwl_card.md §3.3); clamping to 6.0 m.",
                          mw.uwl_cold_min_lambda_m);
                mw.uwl_cold_min_lambda_m = 6.0;
            }
            if (mw.ewl_min_arc_epochs < 2) {
                log->warn("Network: network.mw_wl.ewl_min_arc_epochs = {} is below 2 (a "
                          "variance-of-mean needs >=2 samples) — clamping to 2.",
                          mw.ewl_min_arc_epochs);
                mw.ewl_min_arc_epochs = 2;
            }
            if (mw.min_arc_epochs < 2) {
                log->warn("Network: network.mw_wl.min_arc_epochs = {} is below 2 (a "
                          "variance-of-mean needs >=2 samples) — clamping to 2.",
                          mw.min_arc_epochs);
                mw.min_arc_epochs = 2;
            }
            if (!(mw.sigma_floor_cyc > 0.0)) {
                log->warn("Network: network.mw_wl.sigma_floor_cyc = {} is non-positive — "
                          "clamping to 0.05 cyc (an un-floored R lets a lucky-tight arc fold "
                          "a near-exact WL constraint).", mw.sigma_floor_cyc);
                mw.sigma_floor_cyc = 0.05;
            }
        }
    }

    // =================================================================
    //  network subsection helper: loadNetworkOutputAndParallel_
    // =================================================================
    /**
     * @brief Product output path/flush, per-station compute parallelism, and the pyviz trace toggles.
     *
     * Holds the @c parallel_reduce / @c parallel_carry mutex (both OpenMP-fan the per-station
     * passes against Eigen-threaded dense algebra, so armed together they oversubscribe) and
     * @c network.epoch_decimation_n, which the FC-1 inert-arming refusal reads immediately
     * below.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkOutputAndParallel_(const nlohmann::json& json,
                                                   const std::shared_ptr<spdlog::logger>& log) {
        network_config_.product_output_path =
            jval<std::string>(json, "network.product_output_path", "product_output_path", std::string{});
        // RTA-6: per-epoch product-file flush for a live tail consumer.  Flushing
        // changes no bytes (OFF run byte-identical); default false per the
        // default-off toggle discipline.
        network_config_.product_flush_per_epoch =
            jval<bool>(json, "network.product_flush_per_epoch", "product_flush_per_epoch", false);
        if (network_config_.product_flush_per_epoch) {
            if (network_config_.product_output_path.empty())
                log->warn("Network: network.product_flush_per_epoch is set but "
                          "network.product_output_path is empty — no product file is "
                          "written, so the knob is inert.");
            else
                log->info("Network producer (RTA-6): per-epoch product-file flush ON — a "
                          "live tail consumer sees each epoch block as soon as it is "
                          "emitted (no byte change; ordering/latency only).");
        }

        // --- Per-station compute parallelism (geometry + measurement assembly) ---
        network_config_.parallel_stations =
            jval<bool>(json, "network.parallel_stations", "parallel_stations", true);
        network_config_.parallel_min_stations =
            jval<int>(json, "network.parallel_min_stations", "parallel_min_stations", 10);
        network_config_.parallel_max_threads =
            jval<int>(json, "network.parallel_max_threads", "parallel_max_threads", 0);
        network_config_.parallel_reduce =
            jval<bool>(json, "network.parallel_reduce", "parallel_reduce", false);
        network_config_.parallel_seam =
            jval<bool>(json, "network.parallel_seam", "parallel_seam", false);
        network_config_.parallel_carry =
            jval<bool>(json, "network.parallel_carry", "parallel_carry", false);
        // Guard (Fable lean review, Jul-14 eve): parallel_reduce and parallel_carry are
        // MUTUALLY EXCLUSIVE.  Both OpenMP-fan the per-station passes, and est.solve()'s
        // dense algebra (the fold's GEMMs + the K×K middle inverse) is itself Eigen-
        // threaded — so armed together they nest OpenMP × Eigen inside reduce() and
        // oversubscribe.  Resolve it HERE, at load time (one early, config-visible point,
        // not buried in the epoch driver): force parallel_carry OFF when both are set.  No
        // shipped config arms parallel_reduce, so this only closes the trap.
        if (network_config_.parallel_reduce && network_config_.parallel_carry) {
            log->warn("Network: parallel_reduce AND parallel_carry both set — forcing "
                      "parallel_carry OFF (both OpenMP-fan the per-station passes and "
                      "est.solve() is Eigen-threaded, so together they nest OpenMP × Eigen "
                      "inside reduce()).  Set one false in config to silence this.");
            network_config_.parallel_carry = false;
        }

        // --- Visualization traces (pyviz network viewers) ---
        network_config_.trace_stations =
            jval<bool>(json, "network.trace_stations", "trace_stations", true);
        network_config_.trace_ionosphere =
            jval<bool>(json, "network.trace_ionosphere", "trace_ionosphere", false);
        network_config_.trace_estimability =
            jval<bool>(json, "network.trace_estimability", "trace_estimability", false);
        network_config_.trace_estimability_budget =
            jval<int>(json, "network.trace_estimability_budget",
                      "trace_estimability_budget", 6);
        network_config_.trace_estimability_deficit_only =
            jval<bool>(json, "network.trace_estimability_deficit_only",
                       "trace_estimability_deficit_only", false);
        network_config_.trace_slip_events =
            jval<bool>(json, "network.trace_slip_events", "trace_slip_events", false);
        network_config_.trace_slip_raw =
            jval<bool>(json, "network.trace_slip_raw", "trace_slip_raw", false);
        network_config_.trace_clock_full =
            jval<bool>(json, "network.trace_clock_full", "trace_clock_full", false);
        network_config_.trace_anchor_info =
            jval<bool>(json, "network.trace_anchor_info", "trace_anchor_info", false);
        network_config_.trace_dd_fractional =
            jval<bool>(json, "network.trace_dd_fractional", "trace_dd_fractional", false);
        // T2b: cap DD pairs emitted/covariance-propagated per epoch (0 = unlimited).
        network_config_.trace_dd_fractional_max_pairs =
            jval<int>(json, "network.trace_dd_fractional_max_pairs",
                      "trace_dd_fractional_max_pairs", 0);
        if (network_config_.trace_dd_fractional_max_pairs < 0)
            network_config_.trace_dd_fractional_max_pairs = 0;
        // T2e: DD-trace stride (trace-only) + epoch decimation (estimation-relevant).
        network_config_.trace_dd_fractional_epoch_stride =
            jval<int>(json, "network.trace_dd_fractional_epoch_stride",
                      "trace_dd_fractional_epoch_stride", 1);
        if (network_config_.trace_dd_fractional_epoch_stride < 1)
            network_config_.trace_dd_fractional_epoch_stride = 1;

        // §6.12.5 — WHICH satellite the DD trace differences against.  An unrecognised name
        // is reported and falls back to the default: unlike the Q3 denominator this knob
        // cannot corrupt a solve (trace-only), so a warn-and-default keeps a typo from
        // killing a long run, but it must never pass silently — the whole point of the knob
        // is that the reference identity is now a stated choice rather than an accident of
        // lexical ordering.
        {
            const std::string s = jval<std::string>(json, "network.trace_dd_fractional_ref",
                                                    "trace_dd_fractional_ref",
                                                    std::string{"lowest"});
            if (s.empty() || s == "lowest")
                network_config_.trace_dd_fractional_ref = NetworkDdFracRef::Lowest;
            else if (s == "oldest")
                network_config_.trace_dd_fractional_ref = NetworkDdFracRef::Oldest;
            else {
                network_config_.trace_dd_fractional_ref = NetworkDdFracRef::Lowest;
                log->error("Network (§6.12.5): network.trace_dd_fractional_ref = '{}' is not one of "
                           "lowest|oldest — falling back to 'lowest'.  Every chord in "
                           "network_dd_fractional.csv carries the reference satellite's own error, so "
                           "the grade depends on which one this is.", s);
            }
        }

        // §Z band-scoped responses (docs/SPEC_event_harness.md).  BOTH are
        // SOLVE-AFFECTING when armed and default to the pre-knob behaviour, so an
        // unarmed run is byte-identical (invariant #6).  A typo must not silently
        // arm or disarm a carry-shaping response: warn loudly and take the default.
        {
            const std::string s = jval<std::string>(json, "network.raim_erase_band_scope",
                                                    "raim_erase_band_scope",
                                                    std::string{"all"});
            if (s.empty() || s == "all")
                network_config_.raim_erase_band_scope = NetworkRaimEraseBandScope::All;
            else if (s == "min_keep_one")
                network_config_.raim_erase_band_scope = NetworkRaimEraseBandScope::MinKeepOne;
            else {
                network_config_.raim_erase_band_scope = NetworkRaimEraseBandScope::All;
                log->error("Network (§Z): network.raim_erase_band_scope = '{}' is not one of "
                           "all|min_keep_one — falling back to 'all' (today's whole-arc erase).  "
                           "min_keep_one keeps the least-suspect band pinned so the line of "
                           "sight retains a level anchor across the erase.", s);
            }
        }
        {
            const std::string s = jval<std::string>(json, "network.cycle_slip.refloat_band_scope",
                                                    "network_cycle_slip_refloat_band_scope",
                                                    std::string{"all"});
            if (s.empty() || s == "all")
                network_config_.cycle_slip_refloat_band_scope = NetworkRefloatBandScope::All;
            else if (s == "implicated")
                network_config_.cycle_slip_refloat_band_scope = NetworkRefloatBandScope::Implicated;
            else {
                network_config_.cycle_slip_refloat_band_scope = NetworkRefloatBandScope::All;
                log->error("Network (§Z): network.cycle_slip.refloat_band_scope = '{}' is not one "
                           "of all|implicated — falling back to 'all' (today's whole-satellite "
                           "free).  'implicated' frees only the bands the detector named and, "
                           "when that is every band, keeps the least-suspect one pinned.", s);
            }
        }

        // S1 (docs/SPEC_lane_spectrometer.md §3.3): the per-(cons,signal,sat) -> anchor
        // STATION map, for the spectrometer's anchor partition.  Trace-only, no stride —
        // the row count scales with gauge freedoms (sats x signals), not station pairs.
        network_config_.trace_phase_anchor =
            jval<bool>(json, "network.trace_phase_anchor", "trace_phase_anchor", false);
        network_config_.epoch_decimation_n =
            jval<int>(json, "network.epoch_decimation_n", "epoch_decimation_n", 1);
        if (network_config_.epoch_decimation_n < 1)
            network_config_.epoch_decimation_n = 1;
    }

    // =================================================================
    //  network subsection helper: loadNetworkFastClock_
    // =================================================================
    /**
     * @brief FC-1 split-cadence clocks-only fast pass (@c network.fast_clock.*).
     *
     * Ordered after @c network.epoch_decimation_n and @c network.product_output_path: both
     * inert-arming refusals here read them (an armed non-shadow tier with no decimated-out
     * epochs would drive zero ticks and log perfectly cleanly).
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkFastClock_(const nlohmann::json& json,
                                           const std::shared_ptr<spdlog::logger>& log) {
        // --- FC-1: split-cadence clocks-only fast pass (docs/SPEC_fast_clock_split_cadence.md) ---
        // Parsed HERE, after epoch_decimation_n, because the inert-arming refusal below
        // reads it: without decimation there are no decimated-out epochs for a tick to
        // ride, so an armed non-shadow tier would run exactly zero ticks and report a
        // perfectly clean log (standing invariant #4's subtlest failure mode).
        // Default OFF => the T2e hook keeps its bare `continue` => byte-identical.
        {
            auto& fc = network_config_.fast_clock;
            fc.enable = jval<bool>(json, "network.fast_clock.enable",
                                   "network_fast_clock_enable", false);
            fc.shadow = jval<bool>(json, "network.fast_clock.shadow",
                                   "network_fast_clock_shadow", false);
            fc.emit = jval<bool>(json, "network.fast_clock.emit",
                                 "network_fast_clock_emit", false);
            fc.min_stations_per_sat =
                jval<int>(json, "network.fast_clock.min_stations_per_sat",
                          "network_fast_clock_min_stations_per_sat", 3);
            fc.min_sats_per_station =
                jval<int>(json, "network.fast_clock.min_sats_per_station",
                          "network_fast_clock_min_sats_per_station", 2);
            fc.screen_k_sigma = jval<double>(json, "network.fast_clock.screen_k_sigma",
                                             "network_fast_clock_screen_k_sigma", 5.0);
            fc.allow_single_band =
                jval<bool>(json, "network.fast_clock.allow_single_band",
                           "network_fast_clock_allow_single_band", false);
            fc.emit_max_gap_s = jval<double>(json, "network.fast_clock.emit_max_gap_s",
                                             "network_fast_clock_emit_max_gap_s", 60.0);
            fc.trace = jval<bool>(json, "network.fast_clock.trace",
                                  "network_fast_clock_trace", false);
            fc.trace_sat_stride = jval<int>(json, "network.fast_clock.trace_sat_stride",
                                            "network_fast_clock_trace_sat_stride", 0);

            // Inert-arming disarms (invariant #4): refuse to run an armed-and-dead tier.
            if (fc.shadow && !fc.enable) {
                log->warn("Network (FC-1): network.fast_clock.shadow is INERT without "
                          "network.fast_clock.enable — the shadow pass runs the SAME tick "
                          "body, which is not built when the tier is off.  Disarming shadow.");
                fc.shadow = false;
            }
            if (fc.emit && !fc.enable) {
                log->warn("Network (FC-1): network.fast_clock.emit is INERT without "
                          "network.fast_clock.enable — there are no fast clocks to emit.  "
                          "Disarming emit.");
                fc.emit = false;
            }
            if (fc.enable && !fc.shadow && network_config_.epoch_decimation_n <= 1) {
                log->warn("Network (FC-1): network.fast_clock.enable is armed but "
                          "network.epoch_decimation_n = {} leaves NO decimated-out epochs "
                          "for a fast tick to ride, so this run would drive exactly zero "
                          "ticks while logging cleanly (standing invariant #4).  Set "
                          "epoch_decimation_n > 1 for a split cadence, or "
                          "network.fast_clock.shadow = true to grade the tick against the "
                          "full solve on every epoch.  Disarming fast_clock for this run.",
                          network_config_.epoch_decimation_n);
                fc.enable = false;
                fc.emit   = false;
            }
            if (fc.emit && network_config_.product_output_path.empty()) {
                log->warn("Network (FC-1): network.fast_clock.emit is set but "
                          "network.product_output_path is empty — no product file is "
                          "written, so the fast rows have nowhere to land.  Disarming emit "
                          "(the tick still runs and traces).");
                fc.emit = false;
            }

            // Clamps, gated on the master.
            if (fc.enable) {
                if (fc.min_stations_per_sat < 2) {
                    log->warn("Network (FC-1): network.fast_clock.min_stations_per_sat = {} "
                              "is below 2 — a single station cannot separate its own clock "
                              "from the satellite's (the bipartite fit is rank-deficient). "
                              "Clamped to 2.", fc.min_stations_per_sat);
                    fc.min_stations_per_sat = 2;
                }
                if (fc.min_sats_per_station < 1) {
                    log->warn("Network (FC-1): network.fast_clock.min_sats_per_station = {} "
                              "is below 1 — clamped to 1.", fc.min_sats_per_station);
                    fc.min_sats_per_station = 1;
                }
                if (!(fc.screen_k_sigma > 0.0)) {
                    log->warn("Network (FC-1): network.fast_clock.screen_k_sigma = {} is "
                              "non-positive, which would screen every LOS and leave the "
                              "tick with nothing to fit — clamped to the 5.0 default.",
                              fc.screen_k_sigma);
                    fc.screen_k_sigma = 5.0;
                }
                if (fc.emit_max_gap_s < 0.0) {
                    log->warn("Network (FC-1): network.fast_clock.emit_max_gap_s = {} < 0 "
                              "clamped to 0 (0 = emit only when the anchor is this epoch's "
                              "own emitted solve).", fc.emit_max_gap_s);
                    fc.emit_max_gap_s = 0.0;
                }
                if (fc.trace_sat_stride < 0) fc.trace_sat_stride = 0;

                // gnss_rate_s = 0 means "use the native RINEX rate", which is unknown
                // until data is read — the same caveat the emit_clock_rate inert-arming
                // guard carries.  Report the cadence as a ratio in that case rather than
                // printing a fabricated "0 s".
                const std::string fc_cadence =
                    gnss_rate_s_ > 0.0
                        ? fmt::format("full solve every {:.0f} s (gnss_rate_s {:.0f} x "
                                      "epoch_decimation_n {}), clocks-only ticks every {:.0f} s",
                                      gnss_rate_s_ * network_config_.epoch_decimation_n,
                                      gnss_rate_s_, network_config_.epoch_decimation_n,
                                      gnss_rate_s_)
                        : fmt::format("full solve every {} native epochs "
                                      "(epoch_decimation_n; gnss_rate_s = 0 => native RINEX "
                                      "rate, unknown until data is read), clocks-only ticks "
                                      "on each of the other {}",
                                      network_config_.epoch_decimation_n,
                                      network_config_.epoch_decimation_n - 1);
                log->info("Network (FC-1): split-cadence fast clock ENABLED — {}.  Floors: "
                          "{} station(s)/sat, {} sat(s)/station; screen k={:.1f} sigma; {}; "
                          "R11 emission bound {:.0f} s.  shadow={}, emit={}, trace={}.",
                          fc_cadence, fc.min_stations_per_sat, fc.min_sats_per_station,
                          fc.screen_k_sigma,
                          fc.allow_single_band ? "single-band arcs ADMITTED (iono change "
                                                 "survives in the residual)"
                                               : "dual-band arcs only (IF kills the iono change)",
                          fc.emit_max_gap_s, fc.shadow, fc.emit, fc.trace);
                if (fc.emit)
                    log->info("Network (FC-1): fast rows will be EMITTED to the product "
                              "wire — they refresh the satellite CLOCK value and its age "
                              "only; bias / phase-bias / iono entries and their ages belong "
                              "to the slow era and are untouched, and no discontinuity "
                              "counter advances on a fast row.");
            }
        }
    }

    // =================================================================
    //  network subsection helper: loadNetworkStreaming_
    // =================================================================
    /**
     * @brief RTF-3 multi-station streaming front-end (@c network.streaming.*).
     *
     * Ordered after the roster so the inert-arming refusal can count the per-station
     * @c transport descriptors: an armed front-end with none runs zero lanes.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     * @return @c false on a fatal misconfiguration (aborts @c loadFromJson).
     */
    bool GnssConfig::loadNetworkStreaming_(const nlohmann::json& json,
                                           const std::shared_ptr<spdlog::logger>& log) {
        // --- RTF-3: multi-station streaming front-end (network.streaming) ------
        // Parsed AFTER the stations array so the inert-arming refusal below can
        // see the per-station transport descriptors.  Default OFF => the batch
        // epoch source is untouched (byte-identical); every clamp is gated on
        // the master so an OFF config's values pass through unexamined.
        {
            auto& ns = network_config_.streaming;
            ns.enable = jval<bool>(json, "network.streaming.enable",
                                   "network_streaming_enable", false);
            ns.wait_budget_s = jval<double>(json, "network.streaming.wait_budget_s",
                                            "network_streaming_wait_budget_s", 0.7);
            ns.min_stations = jval<int>(json, "network.streaming.min_stations",
                                        "network_streaming_min_stations", 2);
            ns.require_datum_station =
                jval<bool>(json, "network.streaming.require_datum_station",
                           "network_streaming_require_datum_station", true);
            ns.queue_depth_epochs =
                jval<int>(json, "network.streaming.queue_depth_epochs",
                          "network_streaming_queue_depth_epochs", 8);
            ns.epoch_interval_s =
                jval<double>(json, "network.streaming.epoch_interval_s",
                             "network_streaming_epoch_interval_s", 1.0);
            ns.max_pending_epochs =
                jval<int>(json, "network.streaming.max_pending_epochs",
                          "network_streaming_max_pending_epochs", 16);
            ns.msm_lli_from_locktime =
                jval<bool>(json, "network.streaming.msm_lli_from_locktime",
                           "network_streaming_msm_lli_from_locktime", false);
            ns.log_interval_s = jval<double>(json, "network.streaming.log_interval_s",
                                             "network_streaming_log_interval_s", 30.0);

            if (ns.enable) {
                // Inert-arming refusal (invariant #4): an armed front-end with no
                // station carrying a transport would run zero lanes, dispatch zero
                // epochs and log cleanly.  Refuse the arm instead.
                std::size_t n_transports = 0;
                for (const auto& s : network_config_.stations)
                    if (s.transport.configured()) ++n_transports;
                if (n_transports == 0) {
                    log->error("Network (RTF-3): network.streaming.enable is set but NO "
                               "roster station carries a 'transport' descriptor — the "
                               "front-end would run zero lanes and dispatch zero epochs.  "
                               "Add transport {{input_type, host/port/mountpoint | "
                               "input_path}} to the streamed stations.");
                    return false;
                }
                // Clamps (armed only).  The wait budget's design range is 0.3-1 s
                // (dispatch with whoever arrived); tolerate testing values but keep
                // them sane and loud.
                if (!(ns.wait_budget_s > 0.0)) {
                    log->warn("Network (RTF-3): network.streaming.wait_budget_s = {} is "
                              "non-positive — every epoch would dispatch on first arrival "
                              "with a single station.  Clamped to the 0.7 default.",
                              ns.wait_budget_s);
                    ns.wait_budget_s = 0.7;
                }
                if (ns.min_stations < 2) {
                    log->warn("Network (RTF-3): network.streaming.min_stations = {} is "
                              "below the 2-station estimability floor for any "
                              "between-station content — clamped to 2.", ns.min_stations);
                    ns.min_stations = 2;
                }
                if (ns.queue_depth_epochs < 1) {
                    log->warn("Network (RTF-3): network.streaming.queue_depth_epochs = {} "
                              "< 1 — clamped to 1 (a zero-depth queue could never hand an "
                              "epoch to the solve).", ns.queue_depth_epochs);
                    ns.queue_depth_epochs = 1;
                }
                if (!(ns.epoch_interval_s > 0.0)) {
                    log->warn("Network (RTF-3): network.streaming.epoch_interval_s = {} is "
                              "non-positive — clamped to the 1.0 s MSM default.",
                              ns.epoch_interval_s);
                    ns.epoch_interval_s = 1.0;
                }
                if (ns.max_pending_epochs < 2) {
                    log->warn("Network (RTF-3): network.streaming.max_pending_epochs = {} "
                              "< 2 — the aligner needs at least the assembling epoch plus "
                              "one straggler bucket.  Clamped to 2.", ns.max_pending_epochs);
                    ns.max_pending_epochs = 2;
                }
                if (network_config_.closed_loop.enable)
                    log->warn("Network (RTF-3): closed_loop is armed under streaming — the "
                              "held-out consumer station receives live epochs ONLY if it "
                              "carries its own transport descriptor; otherwise it is absent "
                              "from every dispatched epoch and the consumer grades zero "
                              "epochs (honest zero, not a defect).");
                log->info("Network (RTF-3): STREAMING front-end ARMED — {} of {} roster "
                          "stations carry transports; wait budget {:.2f} s, min {} "
                          "stations/epoch, datum-required={}, queue depth {} epochs "
                          "(drop-oldest), cadence {:.2f} s.",
                          n_transports, network_config_.stations.size(),
                          ns.wait_budget_s, ns.min_stations,
                          ns.require_datum_station ? "yes" : "no",
                          ns.queue_depth_epochs, ns.epoch_interval_s);
            }
        }

        return true;
    }

    // =================================================================
    //  network subsection helper: loadNetworkObservabilityAndSnapshot_
    // =================================================================
    /**
     * @brief Observability surface + RTB-7 producer state snapshot.
     *
     * Trace / heartbeat / rotation toggles, the warm-up URA floor, and the raw parse of the
     * @c network.state_snapshot_* family.  The snapshot's carry-blob requirements are
     * validated later (in @c loadNetworkEmissionFilters_) once @c network.carry_invariant is
     * known.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkObservabilityAndSnapshot_(const nlohmann::json& json,
                                                          const std::shared_ptr<spdlog::logger>& log) {
        // T3f: RSS the SH formal σ + floor + stale-OSB skip on the closed-loop iono supply.
        network_config_.iono_supply_formal_sigma =
            jval<bool>(json, "network.iono_supply_formal_sigma",
                       "iono_supply_formal_sigma", false);
        network_config_.trace_flag_rates =
            jval<bool>(json, "network.trace_flag_rates", "trace_flag_rates", false);
        // One-shot full-arrowhead NEQ export at the Nth processed epoch (<=0 = off).
        network_config_.neq_export_epoch =
            jval<int>(json, "network.neq_export_epoch", "neq_export_epoch", -1);
        // Reduced-RHS class attribution (diagnostics-only; NOT fingerprinted, so a
        // shared-snapshot replay leg with only this armed stays A/B-legal).
        network_config_.rhs_attrib_tow =
            jval<double>(json, "network.rhs_attrib_tow", "rhs_attrib_tow", -1.0);
        network_config_.rhs_attrib_n_ep =
            jval<int>(json, "network.rhs_attrib_n_ep", "rhs_attrib_n_ep", 2);
        if (network_config_.rhs_attrib_tow >= 0.0) {
            log->info("Network: reduced-RHS class attribution ARMED at TOW {:.1f} "
                      "for {} epoch(s) (diagnostics-only; logs/rhs_attrib/)",
                      network_config_.rhs_attrib_tow,
                      std::max(1, network_config_.rhs_attrib_n_ep));
        }
        // Redundancy / testability census (docs/SPEC_redundancy_census.md).
        // Measure-only and NOT fingerprinted, same class as the rhs_attrib block
        // above — armable on a warm resume against an untraced snapshot.
        network_config_.redundancy_census_stride =
            jval<int>(json, "network.redundancy_census_stride",
                      "redundancy_census_stride", 0);
        network_config_.redundancy_eps =
            jval<double>(json, "network.redundancy_eps", "redundancy_eps", 0.05);
        network_config_.redundancy_hot_sigma =
            jval<double>(json, "network.redundancy_hot_sigma", "redundancy_hot_sigma",
                         1000.0);
        network_config_.redundancy_pivot_eps =
            jval<double>(json, "network.redundancy_pivot_eps", "redundancy_pivot_eps",
                         1e-8);
        network_config_.redundancy_escalate_sigma =
            jval<double>(json, "network.redundancy_escalate_sigma",
                         "redundancy_escalate_sigma", 0.0);
        network_config_.redundancy_event_holdoff_ep =
            jval<int>(json, "network.redundancy_event_holdoff_ep",
                      "redundancy_event_holdoff_ep", 20);
        if (network_config_.redundancy_census_stride > 0
            || network_config_.redundancy_escalate_sigma > 0.0) {
            // Clamp the two thresholds that can silently make the census useless
            // rather than merely noisy: eps <= 0 flags nothing at all, and
            // eps >= 1 flags EVERY row (r_i is in [0,1] by construction).
            if (!(network_config_.redundancy_eps > 0.0)
                || network_config_.redundancy_eps >= 1.0) {
                log->warn("Network (redundancy census): network.redundancy_eps = {:g} is "
                          "outside (0,1) — r_i lives in [0,1], so this would flag either "
                          "nothing or every row.  Clamped to the 0.05 default.",
                          network_config_.redundancy_eps);
                network_config_.redundancy_eps = 0.05;
            }
            if (!(network_config_.redundancy_hot_sigma > 0.0)) {
                log->warn("Network (redundancy census): network.redundancy_hot_sigma = "
                          "{:g} is non-positive — every uncheckable row would fire the "
                          "cross-flag, which is exactly the false-positive surface the "
                          "cross-flag exists to remove.  Clamped to the 1000 default.",
                          network_config_.redundancy_hot_sigma);
                network_config_.redundancy_hot_sigma = 1000.0;
            }
            if (!(network_config_.redundancy_pivot_eps > 0.0)) {
                log->warn("Network (redundancy census): network.redundancy_pivot_eps = "
                          "{:g} is non-positive — the INC-2 pivot census would flag "
                          "nothing.  Clamped to the 1e-8 default.",
                          network_config_.redundancy_pivot_eps);
                network_config_.redundancy_pivot_eps = 1e-8;
            } else if (network_config_.redundancy_pivot_eps <= 1e-12) {
                // The kernel's own PD guard (NetworkNormalEquations::addStation)
                // already REJECTS any station whose smallest local pivot is below
                // 1e-12 x the block reference scale, so a floor at or under that
                // leaves INC-2 structurally incapable of ever firing.  That is a
                // silent-inert configuration, not a strict one — say so.
                log->warn("Network (redundancy census): network.redundancy_pivot_eps = "
                          "{:g} is at or below the kernel's own 1e-12 PD rejection "
                          "floor — a station that degenerate never reaches a solve, so "
                          "the INC-2 pivot census can NEVER fire at this setting.  Its "
                          "zero count would mean 'unfirable', not 'no orphaned states'. "
                          "Use 1e-8 (the default) or looser.",
                          network_config_.redundancy_pivot_eps);
            }
            log->info("Network (redundancy census): ARMED — stride {} (0 = escalation "
                      "only), uncheckable r < {:g}, cross-flag s > {:g}, pivot floor "
                      "{:g} x median, escalation {} , event holdoff {} sweep pass(es).  "
                      "MEASURE-ONLY: no weight, state or decision is touched; the "
                      "cross-flag names parked garbage DURING the park.",
                      network_config_.redundancy_census_stride,
                      network_config_.redundancy_eps,
                      network_config_.redundancy_hot_sigma,
                      network_config_.redundancy_pivot_eps,
                      (network_config_.redundancy_escalate_sigma > 0.0)
                          ? fmt::format("s > {:g}",
                                        network_config_.redundancy_escalate_sigma)
                          : std::string("off"),
                      std::max(0, network_config_.redundancy_event_holdoff_ep));
            // NOTE the two sink dependencies are checked further down, once
            // trace_epoch_status / trace_events have actually been parsed.
        }
        // --- PREFIT census / structural fuse / seed-birth tripwire / topology
        //     ledger / flight recorder (docs/SPEC_prefit_census_observability.md).
        // Same class as the two blocks above: all default-OFF, OFF structurally
        // byte-identical, and NONE fingerprinted, so any of them may be armed on a
        // warm resume against an untraced snapshot.  The FUSE is the exception that
        // proves the rule — it IS solve-affecting, but it changes no stored-state
        // MEANING, which is what the fingerprint protects.
        network_config_.trace_prefit_census =
            jval<bool>(json, "network.trace_prefit_census", "trace_prefit_census", false);
        network_config_.prefit_event_sigma =
            jval<double>(json, "network.prefit_event_sigma", "prefit_event_sigma", 1000.0);
        network_config_.prefit_event_classes =
            jval<std::string>(json, "network.prefit_event_classes",
                              "prefit_event_classes", std::string("code,sh,other"));
        network_config_.prefit_event_holdoff_ep =
            jval<int>(json, "network.prefit_event_holdoff_ep",
                      "prefit_event_holdoff_ep", 20);
        network_config_.prefit_fuse_sigma =
            jval<double>(json, "network.prefit_fuse_sigma", "prefit_fuse_sigma", 0.0);
        network_config_.prefit_fuse_classes =
            jval<std::string>(json, "network.prefit_fuse_classes",
                              "prefit_fuse_classes", std::string("code,sh"));
        network_config_.prefit_fuse_require_uncheckable =
            jval<bool>(json, "network.prefit_fuse_require_uncheckable",
                       "prefit_fuse_require_uncheckable", false);
        network_config_.seed_birth_check_sigma =
            jval<double>(json, "network.seed_birth_check_sigma",
                         "seed_birth_check_sigma", 0.0);
        network_config_.trace_topology =
            jval<bool>(json, "network.trace_topology", "trace_topology", false);
        network_config_.rhs_attrib_on_trip =
            jval<bool>(json, "network.rhs_attrib_on_trip", "rhs_attrib_on_trip", false);
        network_config_.rhs_attrib_on_trip_prev =
            jval<bool>(json, "network.rhs_attrib_on_trip_prev",
                       "rhs_attrib_on_trip_prev", true);
        network_config_.rhs_attrib_prev_max_stations =
            jval<int>(json, "network.rhs_attrib_prev_max_stations",
                      "rhs_attrib_prev_max_stations", 128);
        if (network_config_.prefit_event_sigma <= 0.0) {
            log->warn("Network (prefit census): network.prefit_event_sigma = {:g} is "
                      "non-positive — EVERY data row would raise an exceedance event.  "
                      "Clamped to the 1000 default.", network_config_.prefit_event_sigma);
            network_config_.prefit_event_sigma = 1000.0;
        }
        if (network_config_.prefit_fuse_require_uncheckable) {
            // §4 composition with docs/SPEC_redundancy_census.md.  The redundancy
            // census is post-solve and the fuse acts pre-addStation, so the
            // cross-flag would have to reach the fuse through a PERSISTENT
            // uncheckable-cell set that no card has built yet.  Fail loudly rather
            // than let a config written against the companion card silently fuse
            // every large row.
            log->warn("Network (prefit fuse): network.prefit_fuse_require_uncheckable is "
                      "set, but the redundancy cross-flag has no channel into the "
                      "PRE-solve fuse site (the census is post-solve; no persistent "
                      "uncheckable-cell set exists).  Forcing it OFF — the fuse acts on "
                      "magnitude alone this run.  Re-arm it once the redundancy card "
                      "publishes a carried uncheckable set.");
            network_config_.prefit_fuse_require_uncheckable = false;
        }
        {
            const bool walk = network_config_.trace_prefit_census
                            || network_config_.seed_birth_check_sigma > 0.0
                            || network_config_.trace_topology;
            if (walk) {
                std::vector<std::string> ev_unknown;
                const unsigned ev_mask =
                    parseDataClassMask(network_config_.prefit_event_classes, &ev_unknown);
                for (const std::string& tokn : ev_unknown)
                    log->warn("Network (prefit census): unknown class token '{}' in "
                              "network.prefit_event_classes = \"{}\" — IGNORED.  Valid "
                              "tokens: code, phase, other, sh, all, none.",
                              tokn, network_config_.prefit_event_classes);
                log->info("Network (prefit census): ARMED — census {} (event s > {:g} on "
                          "classes [{}], holdoff {} epoch(s)), seed-birth check {}, "
                          "topology ledger {}.  The walk runs whenever ANY of the three "
                          "is on; the metric is per-station per-type "
                          "COMMON-MODE-REMOVED (a receiver clock step cancels, a parked "
                          "cell does not).  MEASURE-ONLY.",
                          network_config_.trace_prefit_census ? "on" : "off (rider only)",
                          network_config_.prefit_event_sigma,
                          dataClassMaskName(ev_mask),
                          std::max(0, network_config_.prefit_event_holdoff_ep),
                          (network_config_.seed_birth_check_sigma > 0.0)
                              ? fmt::format("s > {:g}",
                                            network_config_.seed_birth_check_sigma)
                              : std::string("off"),
                          network_config_.trace_topology ? "on" : "off");
                if (network_config_.trace_prefit_census
                    && (ev_mask & dataClassBit(NetRowClass::ObsPhase)) != 0)
                    log->warn("Network (prefit census): obs_phase is IN the event "
                              "domain.  A phase row's prefit innovation is ARC-SPECIFIC "
                              "(it carries that arc's ambiguity linearization point), so "
                              "a station's phase rows are not a common-mode population "
                              "and their median is not a centre — measured 608 of 665 "
                              "events on one FT63 leg, none of them findings.  Expect "
                              "the real signal to be buried; the per-class COLUMNS "
                              "report phase either way.");
            }
            if (network_config_.prefit_fuse_sigma > 0.0) {
                std::vector<std::string> unknown;
                const unsigned mask =
                    parseDataClassMask(network_config_.prefit_fuse_classes, &unknown);
                for (const std::string& tokn : unknown)
                    log->warn("Network (prefit fuse): unknown class token '{}' in "
                              "network.prefit_fuse_classes = \"{}\" — IGNORED.  Valid "
                              "tokens: code, phase, other, sh, all, none.  A misspelt "
                              "token silently changes a SOLVE-AFFECTING domain, so fix "
                              "it rather than relying on the resolved set below.",
                              tokn, network_config_.prefit_fuse_classes);
                log->warn("Network (prefit fuse): ARMED at S_fuse = {:g} over classes "
                          "[{}] — this is the ONE SOLVE-AFFECTING knob of the "
                          "prefit-census card.  A DATA row of an in-domain class whose "
                          "common-mode-removed s exceeds S is CAPPED to w*(S/s)^2 (not "
                          "dropped); pins (w >= 1e6) and rows whose station type-median "
                          "was unavailable are skipped.  Grade this arm on its own leg "
                          "— it is NOT byte-identical to a control.",
                          network_config_.prefit_fuse_sigma, dataClassMaskName(mask));
                if (mask == 0)
                    log->warn("Network (prefit fuse): the resolved class domain is EMPTY "
                              "— the fuse is armed and can never act.  That is a "
                              "byte-identical arm wearing a fuse label; either fix "
                              "network.prefit_fuse_classes or disarm prefit_fuse_sigma.");
                if ((mask & dataClassBit(NetRowClass::ObsPhase)) != 0)
                    log->warn("Network (prefit fuse): obs_phase is IN the domain.  A "
                              "phase row's prefit innovation carries its arc's FREE "
                              "ambiguity linearization point, so on a warm-resume or "
                              "cold-start epoch — when every arc is re-seeded at once — "
                              "healthy rows reach s ~ 1e5 (measured: MRO100AUS phase "
                              "median -4248 m two epochs after resume).  Capping those "
                              "slows every new arc's convergence for no fault.  This is "
                              "a deliberate arm, not the default; grade it accordingly.");
            }
            if (network_config_.rhs_attrib_on_trip)
                log->info("Network (rhs-attrib INC-5): flight recorder ARMED — the "
                          "capture recorder runs EVERY epoch and the attribution fires "
                          "at the first trip of each QC era (SH-gate engage, sigma0 "
                          "withhold, clock runaway, a00-band exit).  Previous-epoch "
                          "retention {} (max {} stations).{}",
                          network_config_.rhs_attrib_on_trip_prev ? "ON" : "off",
                          network_config_.rhs_attrib_prev_max_stations,
                          (network_config_.rhs_attrib_tow >= 0.0)
                              ? "  NOTE rhs_attrib_tow is ALSO armed: the hand-picked "
                                "window and the trip dumps share logs/rhs_attrib/."
                              : "");
        }
        // --- PRODUCT-REFERENCED MEASUREMENT ADMISSION
        //     (docs/SPEC_measurement_admission.md).  Same class as the two blocks
        //     above: default-OFF, OFF structurally byte-identical, and NOT
        //     fingerprinted -- a forward-only ADMISSION policy (RAIM-class) that
        //     re-references no carried quantity and changes what no serialized
        //     state MEANS, so an OFF/ON pair on ONE shared snapshot stays
        //     A/B-legal.  `adm_screen` IS solve-affecting when armed (it removes
        //     observables from the model set); `adm_trace` alone is measure-only.
        network_config_.adm_screen =
            jval<bool>(json, "network.adm_screen", "adm_screen", false);
        network_config_.adm_bound_m =
            jval<double>(json, "network.adm_bound_m", "adm_bound_m", 1000.0);
        network_config_.adm_min_sats =
            jval<int>(json, "network.adm_min_sats", "adm_min_sats", 4);
        network_config_.adm_heal_ep =
            jval<int>(json, "network.adm_heal_ep", "adm_heal_ep", 5);
        network_config_.adm_max_frac =
            jval<double>(json, "network.adm_max_frac", "adm_max_frac", 0.5);
        network_config_.adm_trace =
            jval<bool>(json, "network.adm_trace", "adm_trace", false);
        // INC-4, the ARC RE-ENTRY vet (card §10).  Same scope class as the rest of
        // the block: default-OFF, not fingerprinted, and inert on a healthy
        // re-entry by construction.
        network_config_.adm_revet =
            jval<bool>(json, "network.adm_revet", "adm_revet", false);
        network_config_.adm_revet_gap_ep =
            jval<int>(json, "network.adm_revet_gap_ep", "adm_revet_gap_ep", 1);
        network_config_.adm_revet_ep =
            jval<int>(json, "network.adm_revet_ep", "adm_revet_ep", 8);
        network_config_.adm_revet_bound_scale =
            jval<double>(json, "network.adm_revet_bound_scale",
                         "adm_revet_bound_scale", 0.2);
        network_config_.adm_revet_census =
            jval<bool>(json, "network.adm_revet_census", "adm_revet_census", true);
        network_config_.adm_revet_hold =
            jval<bool>(json, "network.adm_revet_hold", "adm_revet_hold", false);
        // Range hygiene BEFORE the arming report, so the report states what the run
        // will actually DO rather than what was typed.  The negated-positive form
        // also rejects NaN, which `<= 0.0` does not.
        if (!(network_config_.adm_bound_m > 0.0)) {
            log->warn("Network (measurement admission): network.adm_bound_m = {:g} is "
                      "non-positive or non-finite -- EVERY code channel in the network "
                      "would be rejected.  Clamped to the 1000 m default.",
                      network_config_.adm_bound_m);
            network_config_.adm_bound_m = 1000.0;
        }
        if (network_config_.adm_min_sats < 2) {
            log->warn("Network (measurement admission): network.adm_min_sats = {} "
                      "cannot bound a median (two values have no centre to speak of).  "
                      "Clamped to the 4 default.", network_config_.adm_min_sats);
            network_config_.adm_min_sats = 4;
        }
        if (network_config_.adm_heal_ep < 1) {
            log->warn("Network (measurement admission): network.adm_heal_ep = {} would "
                      "readmit a rejected channel on the first epoch it looks clean, "
                      "which makes the screen a per-epoch flicker rather than an "
                      "admission decision.  Clamped to 1.",
                      network_config_.adm_heal_ep);
            network_config_.adm_heal_ep = 1;
        }
        if (!(network_config_.adm_max_frac > 0.0)
            || network_config_.adm_max_frac >= 1.0) {
            log->warn("Network (measurement admission): network.adm_max_frac = {:g} is "
                      "outside (0, 1) -- at or below 0 every epoch with one bad channel "
                      "becomes a station event, and at or above 1 the guard can never "
                      "fire and the screen would happily reject off a corrupted centre.  "
                      "Clamped to the 0.5 default.", network_config_.adm_max_frac);
            network_config_.adm_max_frac = 0.5;
        }
        if (network_config_.adm_revet_gap_ep < 1) {
            log->warn("Network (measurement admission): network.adm_revet_gap_ep = {} "
                      "would make EVERY screened epoch of every channel a re-entry, so "
                      "no arc could ever become established and the vet window would "
                      "never close.  Clamped to 1 (any missing epoch).",
                      network_config_.adm_revet_gap_ep);
            network_config_.adm_revet_gap_ep = 1;
        }
        if (network_config_.adm_revet_ep < 0) {
            log->warn("Network (measurement admission): network.adm_revet_ep = {} is "
                      "negative.  Clamped to 0 -- the re-entry is still re-screened as "
                      "a birth, but no vet window follows it and the census trigger can "
                      "never fire.", network_config_.adm_revet_ep);
            network_config_.adm_revet_ep = 0;
        }
        if (!(network_config_.adm_revet_bound_scale > 0.0)
            || network_config_.adm_revet_bound_scale > 1.0) {
            log->warn("Network (measurement admission): network.adm_revet_bound_scale = "
                      "{:g} is outside (0, 1] -- at or below 0 every re-entry would be "
                      "refused, and above 1 the 'tighter' vet bound would be LOOSER than "
                      "network.adm_bound_m, which inverts the knob's meaning.  Clamped "
                      "to the 0.2 default (200 m at the 1000 m bound; measured at 3.2x "
                      "above the worst legitimate re-entry |z| in the card's sample).",
                      network_config_.adm_revet_bound_scale);
            network_config_.adm_revet_bound_scale = 0.2;
        }
        // INERT-guard, the signal-integrity block's precedent: the vet is a VERDICT
        // and the measure-only arm makes no verdicts, so arming it without the screen
        // is a configuration that cannot do what it says.  Disabled loudly rather
        // than accepted quietly -- a knob that reads ON in the manifest and does
        // nothing in the run is exactly the "live counters, dead wiring" trap.
        if (network_config_.adm_revet && !network_config_.adm_screen) {
            log->warn("Network (measurement admission): network.adm_revet requires "
                      "network.adm_screen -- the re-entry vet decides ADMISSION, and "
                      "the census-only arm decides nothing.  DISABLED for this run.");
            network_config_.adm_revet = false;
        }
        if (network_config_.adm_screen || network_config_.adm_trace) {
            log->info("Network (measurement admission): ARMED -- screen {}, census on, "
                      "|z| bound {:g} m, median floor {} satellite(s), heal after {} "
                      "clean epoch(s), station guard above {:g} of a station's channels.  "
                      "The statistic is the ABSOLUTE product-referenced CODE term "
                      "(observed - modeled: geometric range, satellite clock and the "
                      "deterministic corrections all removed, no filter state in it) "
                      "centred on the station MEDIAN, so a receiver clock offset or a "
                      "1 ms step cancels exactly while a corrupt channel does not.  "
                      "Phase is deliberately not screened: a constant phase offset is "
                      "indistinguishable from an ambiguity, i.e. gauge, not corruption.",
                      network_config_.adm_screen ? "ON (SOLVE-AFFECTING)"
                                                 : "off (measure-only)",
                      network_config_.adm_bound_m, network_config_.adm_min_sats,
                      network_config_.adm_heal_ep, network_config_.adm_max_frac);
            if (network_config_.adm_screen)
                log->warn("Network (measurement admission): the screen is the ONE "
                          "solve-affecting knob of this card -- a failing (station, "
                          "satellite, band) has BOTH its observables removed from the "
                          "model set for the epoch, its orphaned satellite phase-bias / "
                          "code-bias / clock states pruned, and (except at a pure BIRTH, "
                          "where nothing was ever seeded) its arc refloated through the "
                          "standard funnel.  Grade this arm on its own leg; it is NOT "
                          "byte-identical to a control.");
            if (network_config_.adm_revet) {
                log->info("Network (measurement admission): INC-4 RE-ENTRY VET ARMED -- "
                          "a channel returning after {} or more absent screened epoch(s) "
                          "is re-screened as a BIRTH instead of inheriting its pre-gap "
                          "verdict, then vetted for {} epoch(s).  While re-entering or "
                          "vetting it answers to a TIGHTER bound of {:g} m ({:g} x "
                          "adm_bound_m){}.  Posture on a CLEAN re-entry: {}.  Measured on "
                          "the FT63 24 h day, 16 of 17 mid-arc onsets were unscreened "
                          "re-entries -- twelve mislabelled at no cost, four re-admitted "
                          "for 2-8 epochs at up to +20 TECU of a00 before the bound "
                          "caught them.",
                          network_config_.adm_revet_gap_ep,
                          network_config_.adm_revet_ep,
                          network_config_.adm_bound_m
                              * network_config_.adm_revet_bound_scale,
                          network_config_.adm_revet_bound_scale,
                          network_config_.adm_revet_census
                              ? ", and a PREFIT-census obs_code exceedance on the line of "
                                "sight refuses independently of it -- the two cover "
                                "DISJOINT failure shapes (large-on-arrival vs slow-ramp) "
                                "and neither subsumes the other"
                              : " (census trigger OFF -- vet bound only; the slow-ramp "
                                "class that arrives UNDER the vet bound is then unreachable)",
                          network_config_.adm_revet_hold
                              ? "WITHHELD until it re-earns admission through the heal "
                                "path (strict)"
                              : "admitted provisionally, no withhold and no refloat");
                if (network_config_.adm_revet_bound_scale < 1.0)
                    log->warn("Network (measurement admission): INC-4's tighter vet bound "
                              "is SOLVE-AFFECTING on arrival -- a re-entry over {:g} m is "
                              "refused at its return epoch and never seeded, where the "
                              "plain {:g} m bound would have admitted it.  That is the "
                              "point (it is what closes the +19.2 TECU G16@BSHM excursion "
                              "the census trigger structurally cannot reach), but it means "
                              "an armed arm is NOT byte-identical to a control.  Set "
                              "adm_revet_bound_scale = 1.0 for the label-only vet.",
                              network_config_.adm_bound_m
                                  * network_config_.adm_revet_bound_scale,
                              network_config_.adm_bound_m);
                // The flag is published by the prefit WALK, which is the union of
                // three knobs — not by trace_prefit_census alone.  Testing the union
                // is what keeps the trigger available on a topology-only or
                // birth-check-only arm, and keeps this warn from crying wolf there.
                const bool pf_walk = network_config_.trace_prefit_census
                                  || network_config_.seed_birth_check_sigma > 0.0
                                  || network_config_.trace_topology;
                if (network_config_.adm_revet_census
                    && (!pf_walk || !(network_config_.prefit_event_sigma > 0.0))) {
                    log->warn("Network (measurement admission): network.adm_revet_census "
                              "is armed but the prefit walk is not ({}), so no census "
                              "flag will ever be written and the vet will run on the |z| "
                              "bound alone.  That is the configuration in which the four "
                              "EXPOSED re-entries of the FT63 day were caught 2-8 epochs "
                              "late.  Arm one of network.trace_prefit_census / "
                              "seed_birth_check_sigma / trace_topology (all three are "
                              "fingerprint-SAFE trace-class knobs).  Every skipped "
                              "consultation is counted (revet_census_absent), so this is "
                              "never silently inert.",
                              pf_walk ? "network.prefit_event_sigma is not positive"
                                      : "none of trace_prefit_census / "
                                        "seed_birth_check_sigma / trace_topology is on");
                }
            }
        }
        network_config_.trace_epoch_status =
            jval<bool>(json, "network.trace_epoch_status", "trace_epoch_status", false);
        // SPEC_day129_frame_guards P1-3/P1-4: trigger instrumentation.  Both are
        // TRACE-ONLY and absent from the resume fingerprint (NetworkRunContext.cpp
        // :172-307) — they may be armed on a warm resume against an untraced snapshot.
        network_config_.trace_sh_spectrum =
            jval<bool>(json, "network.trace_sh_spectrum", "trace_sh_spectrum", false);
        network_config_.trace_sh_spectrum_stride =
            jval<int>(json, "network.trace_sh_spectrum_stride", "trace_sh_spectrum_stride", 60);
        network_config_.trace_postfit_sigma0 =
            jval<bool>(json, "network.trace_postfit_sigma0", "trace_postfit_sigma0", false);
        // §4.3 RHS localization rider: keyed (station / satellite / signal)
        // post-fit pools on the same P1-4 residual pass.  Trace-only,
        // fingerprint-SAFE; implies the pass runs even without the sigma0 trace.
        network_config_.trace_postfit_keyed =
            jval<bool>(json, "network.trace_postfit_keyed", "trace_postfit_keyed", false);
        if (network_config_.trace_postfit_keyed)
            log->info("Network (P2-RHS): network.trace_postfit_keyed ON — per-station / "
                      "per-satellite / per-signal robust post-fit pools -> "
                      "network_pf_keyed.csv (trace-only; the P1-4 residual pass runs "
                      "whenever this or trace_postfit_sigma0 is armed).");
        // SPEC_event_harness §24.6: SIGNED per-(station, satellite, signal) post-fit
        // residual rows off the same P1-4 pass.  Trace-only, fingerprint-SAFE (absent
        // from NetworkRunContext.cpp's fingerprint list like the two riders above);
        // implies the pass runs even without the sigma0 / keyed traces.
        network_config_.trace_postfit_rows =
            jval<bool>(json, "network.trace_postfit_rows", "trace_postfit_rows", false);
        if (network_config_.trace_postfit_rows)
            log->info("Network (§24.6 rows): network.trace_postfit_rows ON — SIGNED per-(station, "
                      "satellite, signal) post-fit residuals (v = y − A·x, observed − predicted, "
                      "metres) with the row weight and LOS elevation -> network_pf_rows.csv "
                      "(trace-only; the P1-4 residual pass runs whenever this, "
                      "trace_postfit_keyed or trace_postfit_sigma0 is armed).");
        // SPEC_event_harness §24.10 (card ZP): the per-station LOCAL-BLOCK audit.
        // Trace-only, fingerprint-SAFE (absent from NetworkRunContext.cpp's
        // fingerprint list like the three riders above), and measure-only — it reads
        // the solved estimator through const accessors after the solve.
        network_config_.trace_station_block =
            jval<bool>(json, "network.trace_station_block", "trace_station_block", false);
        if (network_config_.trace_station_block)
            log->info("Network (§24.10 card ZP): network.trace_station_block ON — per-station "
                      "LOCAL-block audit (rank, condition number, the 3 smallest eigenpairs and "
                      "the local states spanning them, the ZWD column's effective vs raw "
                      "information, the reconstructed local correction, and zero-redundancy row "
                      "counts by kind) -> network_station_block.csv on the first 20 EMITTED "
                      "epochs and every 60th thereafter (trace-only, measure-only).");
        // Card RF-PD: name the null local column behind each addStation PD rejection.
        // Trace-only, measure-only (the eigen read runs only on a block the kernel is
        // already throwing away) and fingerprint-SAFE — absent from NetworkRunContext's
        // fingerprint list, like trace_station_block above.
        network_config_.trace_station_pd_null =
            jval<bool>(json, "network.trace_station_pd_null", "trace_station_pd_null", false);
        if (network_config_.trace_station_pd_null) {
            log->info("Network (card RF-PD): network.trace_station_pd_null ON — every "
                      "addStation PD rejection now NAMES its null direction: one "
                      "TOPOLOGY/sta_pd_dropped row in network_events.csv per dropped "
                      "station-epoch carrying the smallest eigenvalue of the rejected local "
                      "block, the near-zero eigenvalue COUNT, the pivot triple "
                      "(d_min/d_ref/d_max) the decision used, and the top-5 |weight| columns "
                      "of the null eigenvector as StateManager labels.  One symmetric "
                      "eigen-decomposition per REJECTED block only; the rejection and the "
                      "station drop are unchanged (measure-only, solve byte-identical).");
        }
        // Card CST: per-satellite readout of the CLOSED-LOOP CONSUMER.  Trace-only,
        // measure-only (the harvest reads state the consumer's epoch has already
        // committed) and fingerprint-SAFE.  The closed-loop SINK check is NOT here:
        // network.closed_loop.enable is parsed in loadNetworkClosedLoop_, which runs
        // AFTER this helper, so testing it at this point would read a default and warn
        // on a run that HAS the harness armed (the trace_events precedent above).
        network_config_.trace_consumer_sat =
            jval<bool>(json, "network.trace_consumer_sat", "trace_consumer_sat", false);
        if (network_config_.trace_consumer_sat) {
            log->info("Network (card CST): network.trace_consumer_sat ON — the held-out "
                      "closed-loop consumer now writes logs/closed_loop_sat_trace.csv, one row "
                      "per (satellite, raw phase signal) per consumer epoch: the APPLIED "
                      "clock/code-bias/phase-bias corrections as the consumer's own gated reads "
                      "return them, the producer's discontinuity COUNTERS as the store presents "
                      "them, WHAT THE CONSUMER DID with each declaration (reset / carry / "
                      "re-inflate / none), the pre-fit innovations, the post-fit residuals, and "
                      "the ambiguity + slant-iono STATE values with their variances.  "
                      "EV6 §9 item 1's owed instrument; measure-only, consumer solve "
                      "byte-identical.");
        }
        // Card JT: screen the phase-bias discontinuity test on the DE-COMMONED step.
        // EMIT-side only (the solve never sees a discontinuity counter), but NOT a
        // trace knob: it changes the R records on the wire, so it is fingerprinted
        // (the resume fingerprint appends discdecom only when armed) and it travels
        // in the S contract as discdecom= so a file replay reproduces the counters.
        network_config_.disc_jump_test_decommon =
            jval<bool>(json, "network.disc_jump_test_decommon", "disc_jump_test_decommon", false);
        if (network_config_.disc_jump_test_decommon) {
            log->info("Network (card JT): network.disc_jump_test_decommon ON — the emitter's "
                      "phase-bias jump test now screens the DE-COMMONED step (raw dPB minus the "
                      "MEDIAN dPB over the satellites emitted on that SIGNAL at both epochs) "
                      "against the same 0.25*lambda bar, instead of the frame-blind raw step.  "
                      "Standing invariant #5: a satellite-common phase-bias move is absorbed by "
                      "a consumer's receiver phase clock, so the de-commoned step is what lands "
                      "in a consumer ambiguity (CST S4: a -1.451 m L1W frame move hid G32 L1W's "
                      "+1.409 m step under a 0.0476 m bar).  A signal with fewer than {} "
                      "fresh-baseline satellites falls back to the RAW test and is counted.  "
                      "Per-cell PB_JUMP events + four run-end class counters (invariant #7).  "
                      "WARNING (CST S7.3 ordering): this ADDS declarations, and a reset-only "
                      "consumer turns each one into an added mass ambiguity reset — arm it only "
                      "with network.closed_loop.pb_disc_carry.",
                      NetworkProductEmitter::kDiscDecommonMinSats);
        }
        // Card MS: scope a per-satellite discontinuity MARK to the channel that
        // moved.  EMIT-side only (no estimate, no datum), but like card JT it
        // changes the declared population on the wire, so it is fingerprinted and
        // travels in the S contract as discscope=.
        network_config_.disc_mark_scope =
            jval<std::string>(json, "network.disc_mark_scope", "disc_mark_scope",
                              std::string("sat"));
        if (network_config_.disc_mark_scope != "sat"
            && network_config_.disc_mark_scope != "channel") {
            log->warn("Network: unknown disc_mark_scope '{}' (expected 'sat' or 'channel'); "
                      "falling back to 'sat' (the legacy satellite-wide scope).",
                      network_config_.disc_mark_scope);
            network_config_.disc_mark_scope = "sat";
        }
        if (network_config_.disc_mark_scope == "channel") {
            log->info("Network (card MS): network.disc_mark_scope = 'channel' — a per-satellite "
                      "discontinuity mark now bumps ONLY the generations of the channels the "
                      "marking site reports (clock / code-bias / phase-bias), instead of all "
                      "three.  markSatDiscontinuity drops a satellite into one forced-bump set "
                      "that the clock, code-bias and phase-bias emit blocks all read, so a "
                      "CLOCK-only guard (the Q3 anchor decouple, the CLK-R step test) declared a "
                      "PHASE-BIAS step that did not happen: 117 of 390 declared consumer "
                      "cell-epochs were EMPTY on RF-0hv (< 0.02 m of PB motion, CST S7.1).  The "
                      "emitter's own per-(sat,signal) jump tests are untouched and remain the "
                      "backstop for a genuine re-realization on an unnamed channel.  Per-channel "
                      "mark counters + a MARK_SCOPE event per suppressed (sat, channel) "
                      "(invariant #7).  WARNING: an R record carries no channel set, so a "
                      "NetworkProductIO REPLAY re-marks with the full satellite scope — the S "
                      "contract carries discscope= so checkContract names the divergence.");
        }
        // Card PBH: the emit-side phase-bias HOLD.  EMIT-side only (no estimate, no
        // datum, no row of the normal equations), but it changes the emitted VALUE,
        // so it is fingerprinted and travels in the S contract as pbhold=.
        network_config_.emit_pb_hold =
            jval<bool>(json, "network.emit_pb_hold", "emit_pb_hold", false);
        network_config_.emit_pb_hold_max_gap_s =
            jval<double>(json, "network.emit_pb_hold_max_gap_s", "emit_pb_hold_max_gap_s", 300.0);
        if (network_config_.emit_pb_hold) {
            log->info("Network (card PBH): network.emit_pb_hold = true — the emitted phase bias "
                      "of each (satellite, signal) is HELD at the first value of its generation "
                      "and a new generation is DECLARED on the wire (gen= token, "
                      "SsrPhaseBiasEntry::hold_generation), with max_gap_s = {:.1f}.  PBH read 3 "
                      "graded the wire against truth: the phase bias ALONE carries 0.0227 m "
                      "(sim) / 0.0467 m (field) of within-arc RMS against the clock's 0.0030 / "
                      "0.0274 m, and the first differences of the two errors correlate at +0.006 "
                      "— the phase bias does not compensate the clock, it adds to it, and on the "
                      "simulator the true bias is constant so every movement is error.  The hold "
                      "is the LAST writer of the emitted value and sits AHEAD of the emitter's "
                      "phase-bias value-jump test, so the declared population screens what the "
                      "consumer receives.  The estimator is untouched: the solve, the C records "
                      "and the B records are byte-identical to the OFF run.  Both edges are "
                      "events with counters (invariant #7): PB_HOLD/gen_start rows with cause "
                      "'first' or 'gap', the |estimate - held| distribution the hold hides, and "
                      "the de-commoned clock step at every coupled clock+phase-bias mark that "
                      "reaches a held key (the cost of not transferring a coupled frame move).",
                      network_config_.emit_pb_hold_max_gap_s);
        }
        // Card RF-PD-fix: install the station's own DATUM rows BEFORE the PD test.
        // SOLVE-AFFECTING (it changes which stations are in the network) — the
        // rx_code_bias_datum cross-check is deferred to that knob's own block, which
        // is parsed further down.
        network_config_.station_prior_before_pd =
            jval<bool>(json, "network.station_prior_before_pd", "station_prior_before_pd", false);
        if (network_config_.station_prior_before_pd) {
            log->info("Network (card RF-PD-fix): network.station_prior_before_pd ON — each "
                      "station's RXCB-DATUM rows are folded into its local block N_rr/u_r "
                      "BEFORE addStation's positive-definiteness test, so the test judges the "
                      "same matrix the solve uses (they were installed one call LATER, by "
                      "addStationPrior, and the station was rejected for the missing datum "
                      "row).  The same rows are withheld from that later call — the two paths "
                      "do not sum.  The phase-leveling / rx-CB CARRY rows stay on the "
                      "post-test path deliberately, so no carried value enters the admission "
                      "decision.  SOLVE-AFFECTING: expect MORE stations in the solve.");
        }
        // The trace_events cross-check is NOT here: network.trace_events is parsed
        // further down (see the sink check right after it), so testing it at this
        // point reads a default and warns on a run that HAS the sink armed.
        if (network_config_.trace_sh_spectrum)
            log->info("Network (P1-3): network.trace_sh_spectrum ON — per-epoch reduced-shared-"
                      "normal spectrum + per-family eigenvector mass partition into "
                      "network_sh_spectrum.csv (mode detail every {} traced epoch(s)).  One "
                      "SelfAdjointEigenSolver of N_ss per emitted epoch; measure-only.",
                      network_config_.trace_sh_spectrum_stride);
        if (network_config_.trace_postfit_sigma0)
            log->info("Network (P1-4): network.trace_postfit_sigma0 ON — robust post-fit "
                      "variance factor over EVERY station's residuals (code/phase pools "
                      "separated) appended to network_epoch_status.csv.  MEASURE-ONLY: the A4 "
                      "withhold ceiling still reads the within-epoch RAIM's sigma0_adapt, so "
                      "this changes no decision.");
        // D0: per-epoch section-timer breakdown of the network solve (diagnostic; byte-identical OFF).
        network_config_.trace_section_timers =
            jval<bool>(json, "network.trace_section_timers", "trace_section_timers", false);
        network_config_.health_interval_s =
            jval<double>(json, "network.health_interval_s", "health_interval_s", 0.0);
        if (network_config_.health_interval_s < 0.0) {
            log->warn("Network: network.health_interval_s {} < 0 clamped to 0 (no operator "
                      "health heartbeat).", network_config_.health_interval_s);
            network_config_.health_interval_s = 0.0;
        }
        // P1: size-based rotation of the unbounded service CSVs (network_trace /
        // network_solution / network_service_health).  Default 0 = off ⇒ no
        // close/rename ever happens (byte-identical).
        network_config_.trace_rotate_mb =
            jval<double>(json, "network.trace_rotate_mb", "trace_rotate_mb", 0.0);
        network_config_.trace_rotate_keep =
            jval<int>(json, "network.trace_rotate_keep", "trace_rotate_keep", 3);
        if (network_config_.trace_rotate_mb < 0.0) {
            log->warn("Network: network.trace_rotate_mb {} < 0 clamped to 0 (trace rotation "
                      "off).", network_config_.trace_rotate_mb);
            network_config_.trace_rotate_mb = 0.0;
        }
        if (network_config_.trace_rotate_keep < 1) {
            log->warn("Network: network.trace_rotate_keep {} < 1 clamped to 1 (at least one "
                      "rolled generation is kept when rotation is on).",
                      network_config_.trace_rotate_keep);
            network_config_.trace_rotate_keep = 1;
        }
        if (network_config_.trace_rotate_mb > 0.0)
            log->info("Network (P1): trace-CSV rotation ON — network_trace.csv / "
                      "network_solution.csv / network_service_health.csv roll at {:.1f} MiB, "
                      "keeping {} rolled generation(s) (.1 oldest-shifted, header re-written; "
                      "rotated suffixes are NOT auto-discovered by the pyviz viewers).",
                      network_config_.trace_rotate_mb, network_config_.trace_rotate_keep);
        network_config_.warmup_ura_floor_m =
            jval<double>(json, "network.warmup_ura_floor_m", "warmup_ura_floor_m", 0.0);
        network_config_.warmup_epochs =
            jval<int>(json, "network.warmup_epochs", "warmup_epochs", 0);
        // RTB-7(a): producer state snapshot + warm resume.  Path empty (default)
        // = OFF, byte-identical; the interval is inert without the path.
        network_config_.state_snapshot_path =
            jval<std::string>(json, "network.state_snapshot_path", "state_snapshot_path",
                              std::string{});
        network_config_.state_snapshot_interval_s =
            jval<double>(json, "network.state_snapshot_interval_s",
                         "state_snapshot_interval_s", 10.0);
        if (network_config_.state_snapshot_interval_s <= 0.0) {
            log->warn("Network: network.state_snapshot_interval_s {} must be > 0; clamped to "
                      "the default 10 s.", network_config_.state_snapshot_interval_s);
            network_config_.state_snapshot_interval_s = 10.0;
        }
        if (!network_config_.state_snapshot_path.empty())
            log->info("Network (RTB-7): producer state snapshot ON — atomic rewrite of '{}' "
                      "every {:.0f} s of product time (driver carries/oscillators/pivots/"
                      "pending bumps + emitter discontinuity state).  At startup a fresh, "
                      "config-matching snapshot WARM-resumes (generation id preserved, no "
                      "consumer reset); a stale/mismatched/corrupt one cold-starts with a "
                      "new generation id.",
                      network_config_.state_snapshot_path,
                      network_config_.state_snapshot_interval_s);
        // RTB-7(c): branch-faithful resume — also serialize the Card-4 invariant DD carry.
        // Raw parse here; the carry_invariant + path requirement is enforced in the
        // carry_invariant validation block below (network_carry_invariant_ is parsed later).
        network_config_.state_snapshot_carry =
            jval<bool>(json, "network.state_snapshot_carry", "state_snapshot_carry", false);
        // RTB-7(e): diagonal-only Pᶻ in the carry blob (write side; the reader is
        // content-driven).  Validated with the rest of the carry block below.
        network_config_.state_snapshot_carry_diagonal =
            jval<bool>(json, "network.state_snapshot_carry_diagonal",
                       "state_snapshot_carry_diagonal", false);
        // RS-2 (§21.4): also write each chord's LINEARIZATION DD (the ZL row) so a resumed
        // run can re-base the correction-frame means onto its OWN x_lin.  Write side only;
        // validated with the rest of the carry block below.
        network_config_.state_snapshot_carry_lin =
            jval<bool>(json, "network.state_snapshot_carry_lin",
                       "state_snapshot_carry_lin", false);
        // RTB-7(d): resume-event response (RE-2), A/B safety (HB-1), blob history (BR-1).
        network_config_.state_snapshot_resume_shock_m =
            jval<double>(json, "network.state_snapshot_resume_shock_m",
                         "state_snapshot_resume_shock_m", 0.0);
        if (network_config_.state_snapshot_resume_shock_m < 0.0) {
            log->warn("Network (RTB-7d): network.state_snapshot_resume_shock_m {} < 0 — "
                      "clamped to 0 (OFF).", network_config_.state_snapshot_resume_shock_m);
            network_config_.state_snapshot_resume_shock_m = 0.0;
        }
        network_config_.state_snapshot_require =
            jval<bool>(json, "network.state_snapshot_require", "state_snapshot_require", false);
        network_config_.state_snapshot_keep_history =
            jval<bool>(json, "network.state_snapshot_keep_history",
                       "state_snapshot_keep_history", false);
        // §18.10 resume FRAME capture (write side of network.resume_frame_restore).
        network_config_.state_snapshot_frame_capture =
            jval<bool>(json, "network.state_snapshot_frame_capture",
                       "state_snapshot_frame_capture", false);
        if (network_config_.state_snapshot_frame_capture) {
            if (network_config_.state_snapshot_path.empty())
                log->warn("Network (resume frame capture): state_snapshot_frame_capture armed "
                          "but state_snapshot_path is empty — no snapshot is written, INERT.");
            else
                log->info("Network (resume frame capture): every snapshot also carries the "
                          "realization-frame rows (PA/PT phase-anchor incumbents + sticky "
                          "targets, CA clock-anchor per-sat state, FA/FP/FE CLK-R held frame) "
                          "under the format-v2 header — a resume armed with "
                          "network.resume_frame_restore can continue the pre-seam frame.  "
                          "Write-side only; the products and every trace are byte-identical.");
        }
        // §18.12 Card-1 coupling-regressor capture (write side of
        // network.resume_coupling_restore).  Its OWN knob, not the frame capture's:
        // the payload is ~37x the rest of the file, so riding the frame knob would
        // blow up every existing frame recipe and make the branch library ambiguous.
        network_config_.state_snapshot_coupling_capture =
            jval<bool>(json, "network.state_snapshot_coupling_capture",
                       "state_snapshot_coupling_capture", false);
        if (network_config_.state_snapshot_coupling_capture) {
            if (!network_config_.state_snapshot_frame_capture)
                throw std::runtime_error(
                    "network.state_snapshot_coupling_capture requires "
                    "network.state_snapshot_frame_capture: the coupling restore is only "
                    "reachable behind network.resume_frame_restore, which is INERT on a "
                    "snapshot with no frame rows — a coupling-only snapshot is unusable "
                    "by construction.");
            if (network_config_.state_snapshot_path.empty())
                log->warn("Network (coupling capture): state_snapshot_coupling_capture "
                          "armed but network.state_snapshot_path is empty — no snapshot "
                          "is written, INERT.");
            else
                log->info("Network (coupling capture): every snapshot also carries the "
                          "Card-1 coupling regressors (QS shared-column dictionary + one "
                          "packed QL/QZ line per carried row + the QR write-epoch "
                          "clock-refloat set) under the format-v3 header.  Write-side "
                          "only; products and every trace are byte-identical.  ⚠ ~37x the "
                          "snapshot size (~9.9 MB at 63 stations, 131 terms/row); with "
                          "network.state_snapshot_keep_history a 12 h leg writes ~14 GB.");
        }
        // RS-3 DATUM capture (write side of network.resume_datum_restore, §21.7).
        // Same shape as the frame capture above — its own knob, path-gated, write-only.
        network_config_.state_snapshot_datum_capture =
            jval<bool>(json, "network.state_snapshot_datum_capture",
                       "state_snapshot_datum_capture", false);
        if (network_config_.state_snapshot_datum_capture) {
            if (network_config_.state_snapshot_path.empty())
                log->warn("Network (resume datum capture): state_snapshot_datum_capture "
                          "armed but network.state_snapshot_path is empty — no snapshot is "
                          "written, INERT.");
            else
                log->info("Network (resume datum capture): every snapshot also carries the "
                          "two datum memories (EQD/EQP/EQS/EQT/EQE/EQR — the eq34 chain "
                          "datum's last-epoch spanning tree — and CBP/CBS — the time-linked "
                          "code-bias previous-value map + ever-seen set) as format-forward "
                          "rows under the existing header.  A resume armed with "
                          "network.resume_datum_restore then continues the pre-seam datum "
                          "instead of re-electing the tree and re-seeding every code-bias "
                          "channel.  ⚠ The tree's SATELLITE side lives in the frame rows "
                          "(anchor_incumbent), so pair this with "
                          "network.state_snapshot_frame_capture{}.  Write-side only; the "
                          "products and every trace are byte-identical.",
                          network_config_.state_snapshot_frame_capture
                              ? " (armed)"
                              : " — which is NOT armed here, so a restore would seed only "
                                "half the tree");
        }
        // RS-5 EMIT-LAYER capture (write side of network.resume_emit_restore, §21.10).
        // Same shape as the frame / datum captures above — its own knob, path-gated,
        // write-only.  The rows come from two OUTPUTS-ONLY engines, so arming it can
        // change no estimate and no trace, only the snapshot bytes.
        network_config_.state_snapshot_emit_capture =
            jval<bool>(json, "network.state_snapshot_emit_capture",
                       "state_snapshot_emit_capture", false);
        if (network_config_.state_snapshot_emit_capture) {
            if (network_config_.state_snapshot_path.empty())
                log->warn("Network (resume emit capture): state_snapshot_emit_capture "
                          "armed but network.state_snapshot_path is empty — no snapshot is "
                          "written, INERT.");
            else
                log->info("Network (resume emit capture): every snapshot also carries the "
                          "EMIT LAYER (ER — the Card-4 eq34 emit re-base's emission frame, "
                          "one previous emitted bias per sat|signal — and PF/PS — the PB "
                          "emission filter's per-key scalar-KF posterior and its "
                          "per-constellation step-noise accumulators) as format-forward "
                          "rows under the existing header.  Both engines are bare run "
                          "locals today, so without these rows a warm resume emits the RAW "
                          "solve-frame phase bias at its first epoch while the continuous "
                          "wire carries the accumulated re-base; a resume armed with "
                          "network.resume_emit_restore continues the writer's emission "
                          "level instead.  Write-side only; the products and every trace "
                          "are byte-identical.");
        }
        // RS-6 STATION-LAYER capture (write side of network.resume_station_restore,
        // §21.13).  Same shape as the frame / datum / emit captures above — its own
        // knob, path-gated, write-only.  The rows are plain copies of two ctx.carry
        // maps taken after the emit, so arming it can change no estimate and no trace,
        // only the snapshot bytes.
        network_config_.state_snapshot_station_capture =
            jval<bool>(json, "network.state_snapshot_station_capture",
                       "state_snapshot_station_capture", false);
        if (network_config_.state_snapshot_station_capture) {
            if (network_config_.state_snapshot_path.empty())
                log->warn("Network (resume station capture): state_snapshot_station_capture "
                          "armed but network.state_snapshot_path is empty — no snapshot is "
                          "written, INERT.");
            else
                log->info("Network (resume station capture): every snapshot also carries the "
                          "STATION LAYER (RC — the rx-CB carry, one receiver code-bias "
                          "posterior per (station, CB family) in the Z row's field set — and "
                          "SD — the T5/Iono-2 per-LOS SH representation-error deviation "
                          "carry, one ShDevCarry per STATION|SAT) as format-forward rows "
                          "under the existing header.  Both maps are cross-epoch state that "
                          "no snapshot held, so without these rows a warm resume restarts "
                          "both COLD: the rx-CB restart re-splits code bias / iono / clock "
                          "per station and signal, and the deviation restart is the SH a₀₀ "
                          "transient.  A resume armed with network.resume_station_restore "
                          "continues both instead.  Write-side only; the products and every "
                          "trace are byte-identical.");
        }
        // RS-8 CANONICAL-SIGNAL capture (write side of
        // network.resume_signal_canon_restore, §21.16).  Same shape as the frame /
        // datum / emit / station captures above — its own knob, path-gated,
        // write-only.  The rows are read out of each station's PppModel after the
        // emit, so arming it can change no estimate and no trace, only the snapshot
        // bytes.
        network_config_.state_snapshot_signal_capture =
            jval<bool>(json, "network.state_snapshot_signal_capture",
                       "state_snapshot_signal_capture", false);
        if (network_config_.state_snapshot_signal_capture) {
            if (network_config_.state_snapshot_path.empty())
                log->warn("Network (resume signal capture): state_snapshot_signal_capture "
                          "armed but network.state_snapshot_path is empty — no snapshot "
                          "is written, INERT.");
            else
                log->info("Network (resume signal capture): every snapshot also carries "
                          "the CANONICAL-SIGNAL latch (CN — one row per (station, "
                          "constellation, band, C|L) naming the locked RINEX code).  That "
                          "latch is decided from a station's FIRST processed epoch's "
                          "satellite counts and held for the run, and it lived in no "
                          "snapshot, so a warm resume RE-decides it from the resume "
                          "epoch's counts (measured: GAMB00PYF L2X→L2W, CPVG00CPV "
                          "L2W→L2X, OAFA00ARG L2X→L2W ⇒ the L2X tracker set changes, the "
                          "eq34 L2X component loses its datum receiver, and six "
                          "phase-bias families step ~25 m at +1).  A resume armed with "
                          "network.resume_signal_canon_restore continues the writer's "
                          "latch instead.  Write-side only; the products and every trace "
                          "are byte-identical.");
        }
        // RS-9 code-bias LINEARIZATION-POINT capture (write side of
        // network.resume_rxcb_lin_restore, §21.18).  Beside the station capture above and
        // REQUIRING it: the RL row is the REFERENCE the RC row's inject gate differences
        // against, so a file with RL and no RC would restore a frame for a carry that is
        // not there — the defect inverted.  Write-only; the block reads the station state
        // vectors after the emit and writes into `snap` alone.
        network_config_.state_snapshot_rxcb_lin_capture =
            jval<bool>(json, "network.state_snapshot_rxcb_lin_capture",
                       "state_snapshot_rxcb_lin_capture", false);
        if (network_config_.state_snapshot_rxcb_lin_capture
            && !network_config_.state_snapshot_station_capture) {
            log->warn("Network (RS-9 rx-CB lin capture): state_snapshot_rxcb_lin_capture "
                      "armed but network.state_snapshot_station_capture is OFF — the RL "
                      "row is the REFERENCE the RC row's inject gate differences against "
                      "(innov = value_abs − x_lin), so writing it without the carry it "
                      "judges would restore a frame for a carry that is not there.  "
                      "Disarmed.");
            network_config_.state_snapshot_rxcb_lin_capture = false;
        }
        if (network_config_.state_snapshot_rxcb_lin_capture) {
            if (network_config_.state_snapshot_path.empty())
                log->warn("Network (RS-9 rx-CB lin capture): state_snapshot_rxcb_lin_capture "
                          "armed but network.state_snapshot_path is empty — no snapshot is "
                          "written, INERT.");
            else
                log->info("Network (RS-9 rx-CB lin capture): every snapshot also carries the "
                          "code-bias LINEARIZATION POINT (RL — one x_lin per (station, CB "
                          "state key), beside each RC row) as format-forward rows under the "
                          "existing header.  x_lin is seeded ONCE from one epoch of code "
                          "behind a one-shot latch and never written back, so a warm resume "
                          "re-derives the rx-CB inject gate's REFERENCE from the resume "
                          "epoch while RS-6 restores its numerator: 17 of 61 field stations "
                          "then sat outside the gate's fixed 5.13 m bar every epoch of the "
                          "leg (§21.18).  A resume armed with network.resume_rxcb_lin_restore "
                          "continues the writer's frame instead.  Write-side only; the "
                          "products and every trace are byte-identical.");
        }
        if (network_config_.state_snapshot_require)
            log->info("Network (RTB-7d/HB-1): warm resume REQUIRED — any resume failure "
                      "(missing/parse/fingerprint/age/emitter) is a hard run error instead "
                      "of a silent cold start.  Arm this in every same-snapshot A/B.");
        if (network_config_.state_snapshot_keep_history)
            log->info("Network (RTB-7d/BR-1): per-epoch snapshot history ON — each snapshot "
                      "is also written to '<path>.ep<NNNN>' for branch grading "
                      "(scripts/diff_carry_blobs.py).  Write-side only; heavy on disk.");
        network_config_.trace_events =
            jval<bool>(json, "network.trace_events", "trace_events", false);
        if (network_config_.trace_events)
            log->info("Network: per-event QC trace ENABLED — network_events.csv logs every "
                      "tiered QC action (T1-T6, GATE/SIGMA0/PIVOT/RETRO) in long format.");
        // Card RF-PD sink dependency (deferred from its arming block above — this
        // toggle is parsed only here).  Standing invariant #4: an armed instrument
        // whose output has nowhere to go must SAY so.
        if (network_config_.trace_station_pd_null && !network_config_.trace_events)
            log->warn("Network (card RF-PD): network.trace_station_pd_null is armed but "
                      "network.trace_events is OFF — network_events.csv is never opened, so "
                      "every sta_pd_dropped row is DISCARDED and only the run-end counter "
                      "survives.  Arm network.trace_events too.");
        // Redundancy census sink dependencies (deferred from the arming block
        // above — both toggles are parsed only here).  Standing invariant #4: an
        // armed instrument whose output has nowhere to go must SAY so, because
        // its log is otherwise indistinguishable from a quiet network.
        if (network_config_.redundancy_census_stride > 0
            || network_config_.redundancy_escalate_sigma > 0.0) {
            if (!network_config_.trace_events)
                log->warn("Network (redundancy census): ARMED but network.trace_events is "
                          "OFF — the run-end totals and the per-epoch scalars survive, but "
                          "every uncheckable_hot_cell / unobservable_local IDENTITY is "
                          "dropped on the floor.  A census that cannot name the cell it "
                          "found is half an instrument; arm trace_events with it.");
            if (!network_config_.trace_epoch_status)
                log->warn("Network (redundancy census): ARMED but "
                          "network.trace_epoch_status is OFF — the per-epoch "
                          "n_uncheckable_rows / uncheckable_max_s / n_unobservable_local "
                          "columns have nowhere to go.  Only the run-end totals and the "
                          "events will survive.");
        }
        // Prefit-census / fuse / birth-check / topology sink dependencies (same
        // deferral, same invariant-#4 reason).  The FUSE is called out separately:
        // it still ACTS with the sinks off, so a silent fuse is a solve change with
        // no record of what it cut — the one configuration of this card that could
        // mislead a later reader.
        {
            const bool walk = network_config_.trace_prefit_census
                            || network_config_.seed_birth_check_sigma > 0.0
                            || network_config_.trace_topology;
            if (walk && !network_config_.trace_events)
                log->warn("Network (prefit census): ARMED but network.trace_events is OFF "
                          "— the run-end totals and the per-epoch columns survive, but "
                          "every prefit_exceedance / seed_birth_exceedance / TOPOLOGY "
                          "IDENTITY is dropped on the floor.  The whole point of the "
                          "census is that it NAMES the cell; arm trace_events with it.");
            if (network_config_.trace_prefit_census
                && !network_config_.trace_epoch_status)
                log->warn("Network (prefit census): ARMED but network.trace_epoch_status "
                          "is OFF — the per-class max_s / p99_s / n_s_gt_10 / "
                          "n_s_gt_1000 columns have nowhere to go.  Only the run-end "
                          "totals and the events will survive.");
            if (network_config_.prefit_fuse_sigma > 0.0
                && !network_config_.trace_events)
                log->warn("Network (prefit fuse): ARMED at S_fuse = {:g} with "
                          "network.trace_events OFF.  The fuse still CAPS weights — a "
                          "solve change — but nothing records WHICH rows it cut beyond "
                          "the run-end count.  Arm trace_events on any leg you intend to "
                          "grade.", network_config_.prefit_fuse_sigma);
        }
        // Measurement-admission sink dependencies (same deferral, same invariant-#4
        // reason).  The screen is called out separately for the same reason the
        // fuse is: it still ACTS with the sinks off, so a silent screen is a solve
        // change with no record of which channels it removed.
        if (network_config_.adm_screen || network_config_.adm_trace) {
            if (!network_config_.trace_events)
                log->warn("Network (measurement admission): ARMED but "
                          "network.trace_events is OFF — the run-end totals and the "
                          "per-epoch |z| columns survive, but every adm_reject_birth / "
                          "adm_reject_resume / adm_onset / adm_heal / adm_station_event "
                          "IDENTITY is dropped on the floor.  This card's whole claim is "
                          "that it NAMES the corrupt channel; arm trace_events with it.");
            if (!network_config_.trace_epoch_status)
                log->warn("Network (measurement admission): ARMED but "
                          "network.trace_epoch_status is OFF — the two per-epoch |z| "
                          "census columns have nowhere to go, so the census reads as a "
                          "clean quiet run whether or not it found anything.  Only the "
                          "run-end totals and the events will survive.");
        }
        if (network_config_.adm_screen && !network_config_.trace_events)
            log->warn("Network (measurement admission): the SCREEN is armed with "
                      "network.trace_events OFF.  It still removes observables from the "
                      "model set — a solve change — but nothing records WHICH channels "
                      "beyond the run-end counts.  Arm trace_events on any leg you "
                      "intend to grade.");
        network_config_.trace_zwd =
            jval<bool>(json, "network.trace_zwd", "trace_zwd", false);
        if (network_config_.trace_zwd)
            log->info("Network: per-station ZWD trace ENABLED — network_zwd.csv logs each "
                      "station's estimated ZWD + datum-conditioned formal sigma each emitted epoch.");
        // L14 / L14b: the two SHARED-and-RECEIVER bias state traces.  These exist
        // because the B product record carries NO sigma in any configuration
        // (docs/SPEC_synthetic_minimum_network.md §4.8), so the satellite code-bias
        // variance factor is unobtainable from the product, and the receiver DCB —
        // which the W6 slice force-arms — appears nowhere as an estimate with a
        // sigma at all.  Both are trace-only: no product grammar change, and
        // byte-identical when off (no file opened, no rows written).
        network_config_.trace_sat_bias_states =
            jval<bool>(json, "network.trace_sat_bias_states", "trace_sat_bias_states", false);
        if (network_config_.trace_sat_bias_states) {
            log->info("Network: satellite bias-state trace ENABLED — network_sat_bias_states.csv "
                      "logs every emitted (sat, code signal) bias with its sigma0-scaled formal "
                      "sigma from the gauge-nulled shared covariance (the B record itself has no "
                      "sigma field; this is the code-bias variance-factor channel).");
            // The "is it INERT?" warning is deliberately NOT here — see the shared
            // note at the tail of loadNetworkAprioriAndGlonass_, which now owns it.
        }
        network_config_.trace_rx_states =
            jval<bool>(json, "network.trace_rx_states", "trace_rx_states", false);
        if (network_config_.trace_rx_states) {
            log->info("Network: receiver state trace ENABLED — network_rx_states.csv logs each "
                      "station's receiver clock and receiver code-bias (DCB/IFB) states with "
                      "their datum-conditioned formal sigma each emitted epoch.");
            // The "CLOCK rows only" warning is deliberately NOT here — see the shared
            // note at the tail of loadNetworkAprioriAndGlonass_, which now owns it.
        }
        network_config_.trace_station_residuals =
            jval<bool>(json, "network.trace_station_residuals", "trace_station_residuals", false);
        network_config_.trace_raim_redundancy =
            jval<bool>(json, "network.trace_raim_redundancy", "trace_raim_redundancy", false);
        network_config_.write_run_manifest =
            jval<bool>(json, "network.write_run_manifest", "write_run_manifest", false);
        if (network_config_.write_run_manifest)
            log->info("Network: run-provenance manifest ENABLED — network_run_manifest.json "
                      "records the config hash, build stamp, and full toggle set (JSONL).");
        network_config_.trace_ssr_consumer =
            jval<bool>(json, "network.trace_ssr_consumer", "trace_ssr_consumer", false);
        if (network_config_.trace_ssr_consumer)
            log->info("Network: consumer-side SSR trace ENABLED — the SSR consumer writes "
                      "ssr_consumer_trace.csv (age, clk-applied, URA R-inflation, disc counters).");
        network_config_.serialize_sbasis_contract =
            jval<bool>(json, "network.serialize_sbasis_contract", "serialize_sbasis_contract", false);
        if (network_config_.serialize_sbasis_contract)
            log->info("Network: S-basis contract serialization ENABLED — product header carries the "
                      "S-basis declaration, a-priori orbit/clock product identity, and phase-bias "
                      "discontinuity threshold (v2 'S' line) for consumer cross-check.");
    }

    // =================================================================
    //  network subsection helper: loadNetworkSharedStateForcings_
    // =================================================================
    /**
     * @brief Resolve the clock-datum station, then force the shared satellite-state estimation.
     *
     * V1 forces sat clock + phase bias on and defers the satellite code bias; W6
     * (@c network.estimate_sat_code_bias, implied by @c network.estimate_iono_sh) re-enables
     * it as one coupled slice (sat code bias + rx DCB + iono constraint, .BIA off).  Also
     * owns @c network.cold_clock_seed and the CB-ADM admission census.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     * @return @c false on a fatal misconfiguration (aborts @c loadFromJson).
     */
    bool GnssConfig::loadNetworkSharedStateForcings_(const nlohmann::json& json,
                                                     const std::shared_ptr<spdlog::logger>& log) {
        if (network_config_.clock_datum_station.empty()) {
            network_config_.clock_datum_station = network_config_.stations.front().name;
            log->info("Network: clock_datum_station unset; defaulting to first station '{}'",
                      network_config_.clock_datum_station);
        } else {
            bool found = false;
            for (const auto& s : network_config_.stations)
                if (s.name == network_config_.clock_datum_station) { found = true; break; }
            if (!found) {
                log->error("Network: clock_datum_station '{}' not found among stations",
                           network_config_.clock_datum_station);
                return false;
            }
        }

        // --- Force the shared satellite-state estimation for V1 (network producer) ---
        // V1 scope = estimable satellite CLOCK + PHASE BIAS only (the integer-
        // recovery essentials).  Satellite CODE bias is DEFERRED to W6: estimating
        // it would add the IF-code-bias lump as a SECOND estimable place (it is
        // already absorbed into the estimable clock), needing an extra code-bias
        // datum pin.  So force clock + phase-bias ON and leave code-bias OFF —
        // the satellite code delay then re-partitions into the per-station rx code
        // bias + local slant iono (the existing L1-referenced iono+DCB convention;
        // the emitted clock is code-blind / phase-consistent).  loadEstimation-
        // Section_ runs earlier, so this overrides the JSON estimation block.
        estimate_sat_clocks_       = true;
        estimate_sat_phase_biases_ = true;
        estimate_sat_biases_       = false;  // V1 default: defer satellite code bias (W6 below re-enables)
        promote_sat_code_bias_     = false;  // V1 default: defer satellite code bias (W6 below re-enables)
        log->info("Network producer: forcing estimate_sat_clocks + estimate_sat_phase_biases = true");

        // --- W6: satellite CODE-bias (OSB) estimation -----------------------------
        // When network.estimate_sat_code_bias is on, the network estimates its OWN
        // per-(satellite,signal) code biases (all non-GLONASS constellations — GPS,
        // Galileo, BeiDou) and emits them, so the
        // user can SKIP the external .BIA.  This is a single coupled slice — the
        // satellite code-bias column, the receiver DCB (estimate_ifb), and the GIM
        // ionosphere weighting (the load-bearing constraint that makes the satellite
        // DCB observable against the per-(rx,sat) slant iono) must ALL be present or
        // the shared null-space rank gate withholds every epoch.  Forcing them here
        // keeps them in lockstep behind the one flag.
        estimate_network_sat_code_bias_ = jval<bool>(
            json, "network.estimate_sat_code_bias", "estimate_sat_code_bias", false);

        // --- EXT-CB: satellite biases come from the external .BIA -----------------
        // The third satellite-bias posture (see GnssConfig::networkExternalSatCodeBias).
        // Parsed HERE, before the estimate_iono_sh coupling below, because its whole
        // job is to SUPPRESS that coupling: co-estimating the SH-VTEC field does not
        // need a satellite DCB column when the product already removed the satellite
        // code bias, and the W6 chain would otherwise force enable_code_phase_bias
        // OFF and make "SH iono + external .BIA" unreachable.
        network_external_sat_code_bias_ = jval<bool>(
            json, "network.external_sat_code_bias", "external_sat_code_bias", false);
        if (network_external_sat_code_bias_ && estimate_network_sat_code_bias_) {
            log->error("Network: network.external_sat_code_bias and "
                       "network.estimate_sat_code_bias are mutually exclusive satellite "
                       "code-bias SOURCES — one consumes the external .BIA, the other "
                       "estimates the network's own OSB and skips the .BIA entirely.  "
                       "Arming both would ask for a satellite code-bias state on rows the "
                       "product has already corrected (the state would absorb only the "
                       "residual, in a datum the emitted OSB does not describe).  Pick one.");
            return false;
        }

        // Co-estimated global SH-VTEC ionosphere.  This REPLACES the external GIM as
        // the iono constraint that makes the satellite DCB observable, so it implies
        // — and forces on — the whole W6 satellite code-bias slice.  Setting it here
        // (before the W6 chain below) lets that chain fire unchanged; the driver then
        // uses the SH row builder instead of the external-GIM one and does NOT load a
        // GIM (the two iono sources are never double-applied).
        //
        // EXT-CB suppresses the W6 force ONLY.  The SH field itself is untouched:
        // ApplicationNetworkSetup's model construction, the assembly's coefficient
        // registration and the SH row builder all gate on estimateNetworkIonoSh()
        // alone, never on the code-bias flag — so the field is co-estimated exactly
        // as before, with the satellite DCB supplied by the product instead of a
        // column.  (That is the classical known-DCB global-iono setup.)
        estimate_network_iono_sh_ = jval<bool>(
            json, "network.estimate_iono_sh", "estimate_iono_sh", false);
        if (estimate_network_iono_sh_) {
            if (!network_external_sat_code_bias_)
                estimate_network_sat_code_bias_ = true;   // SH co-estimation is the W6 iono constraint
            global_vtec_sh_.enabled             = true;
        }

        // Cold-start receiver-clock pre-seed.  On the network path nothing
        // pre-solves the receiver clock before the first ambiguity seed
        // (syncPppStateToStateVector, the PPP-path writer of x(clock), never
        // runs from ApplicationNetwork), so a free-running receiver's whole
        // clock offset leaks UNIFORMLY into every cold ambiguity seed via
        // computeAmbiguitySeed's clk_term=x(clock)=0.  When on, the state
        // pre-pass writes the median GPS code residual into x(clock) once per
        // station.  Default OFF => the seeder is never called (byte-identical).
        network_cold_clock_seed_ = jval<bool>(
            json, "network.cold_clock_seed", "cold_clock_seed", false);
        if (network_cold_clock_seed_) {
            log->info("Network: cold_clock_seed ON — pre-seeding each station's cold "
                      "receiver clock from the median GPS code residual before the first "
                      "ambiguity seed (removes the cold-start clock leak into ambiguity seeds).");
        }

        // --- CB-GF-DATUM: per-satellite geometry-free code-bias pin --------------
        // Odijk et al. 2016 (J Geod 90:15-44) §5.1 / Table 7, rank deficiency 3b — the
        // satellite-side mirror of the receiver gauge below: that one closes rd 2b
        // (receiver biases vs slant iono), this one closes 3b.  Parsed BEFORE the W6
        // chain because it selects which posture that chain arms: with the GF datum the
        // producer runs Odijk's SLANT model (no ionosphere constraint at all), without
        // it the classical single-layer one.
        network_code_bias_gf_datum_ = jval<bool>(
            json, "network.code_bias_gf_datum", "code_bias_gf_datum", false);
        // FAMILY-GF (docs/SPEC_family_gf_datum.md): the same rd-3b pin, extended to each
        // PARTITIONED secondary receiver channel family.  Parsed beside its parent; its
        // interlock is deliberately BELOW the CB-GF-DATUM validation block so it inherits
        // every one of that block's hard errors by construction.
        network_code_bias_gf_datum_per_family_ = jval<bool>(
            json, "network.code_bias_gf_datum_per_family",
            "code_bias_gf_datum_per_family", false);

        // --- RF-0b: the W6 ionosphere-constraint SELECTOR ------------------------
        // docs/refound/RF-0_baseline_2026-09-14.md Finding 2.  Parsed HERE, beside
        // code_bias_gf_datum and before the W6 chain, for the same reason that one is:
        // it selects which posture the chain arms.  "auto" is the pre-knob three-way
        // selection (SH field / GF-pinned slant / external GIM) and is byte-identical;
        // "none" removes the ionosphere constraint outright and leaves the satellite
        // code-bias datum to the IF pin + per-(constellation, signal) sum-to-zero rows
        // NetworkSbasis::buildCodeBiasDatumRows already folds every epoch.
        //
        // A typo must not silently pick a DIFFERENT DATUM for the product, so an
        // unrecognized value falls back to "auto" with an error (the same contract
        // network.raim_erase_band_scope and network.trace_dd_fractional_ref use).
        {
            const std::string s = jval<std::string>(json, "network.w6_iono_constraint",
                                                    "w6_iono_constraint",
                                                    std::string{"auto"});
            if (s.empty() || s == "auto")
                network_config_.w6_iono_constraint = NetworkW6IonoConstraint::Auto;
            else if (s == "none")
                network_config_.w6_iono_constraint = NetworkW6IonoConstraint::None;
            else {
                network_config_.w6_iono_constraint = NetworkW6IonoConstraint::Auto;
                log->error("Network (RF-0b): network.w6_iono_constraint = '{}' is not one of "
                           "auto|none — falling back to 'auto' (today's selection: the SH field "
                           "if network.estimate_iono_sh, else the geometry-free slant posture if "
                           "network.code_bias_gf_datum, else the external GIM).  'none' runs the "
                           "W6 slice with NO ionosphere constraint and the sum-to-zero code-bias "
                           "datum, which is a DIFFERENT product datum — never selected by a typo.",
                           s);
            }
        }
        const bool w6_iono_none =
            network_config_.w6_iono_constraint == NetworkW6IonoConstraint::None;

        // --- RF-0c: the HARD per-satellite geometry-free datum -------------------
        // Parsed beside its parent selector because it only has meaning inside the
        // no-constraint posture: with an ionosphere constraint present the direction
        // this pins is already determined, and pinning it anyway is an over-constraint.
        // Validated below, after the W6 chain, so the chain's forced values are visible.
        {
            const std::string s = jval<std::string>(json, "network.sat_gf_datum",
                                                    "sat_gf_datum", std::string{"none"});
            if (s.empty() || s == "none")
                network_config_.sat_gf_datum = NetworkSatGfDatum::NoneMode;
            else if (s == "zero")
                network_config_.sat_gf_datum = NetworkSatGfDatum::Zero;
            else if (s == "broadcast_tgd")
                network_config_.sat_gf_datum = NetworkSatGfDatum::BroadcastTgd;
            else {
                network_config_.sat_gf_datum = NetworkSatGfDatum::NoneMode;
                log->error("Network (RF-0c): network.sat_gf_datum = '{}' is not one of "
                           "none|zero|broadcast_tgd — falling back to 'none' (no "
                           "geometry-free datum row).  The three modes emit DIFFERENT "
                           "satellite code-bias frames and a different estimable "
                           "ionosphere, so the datum is never selected by a typo.", s);
            }
        }
        const NetworkSatGfDatum sat_gf_mode = network_config_.sat_gf_datum;
        const bool sat_gf_on = sat_gf_mode != NetworkSatGfDatum::NoneMode;

        // --- RF-0e: the two arms that answer RF-0d's consumer regression ----------
        // e1 restores an ABSOLUTE level datum the geometry-free pins do not provide;
        // e2 instead makes the geometry-free rows carry like every other datum row on a
        // time-linked lineage.  Both default OFF (byte-identical) and both are INERT
        // without the geometry-free datum itself.
        network_config_.sat_if_level_datum =
            jval<bool>(json, "network.sat_if_level_datum", "sat_if_level_datum", false);
        network_config_.sat_gf_datum_time_linked =
            jval<bool>(json, "network.sat_gf_datum_time_linked",
                       "sat_gf_datum_time_linked", false);
        // RF-0f: leave the band-2 same-band companion UNPINNED so its ISC is a free
        // per-satellite difference rather than an asserted zero.
        network_config_.sat_isc_state =
            jval<bool>(json, "network.sat_isc_state", "sat_isc_state", false);
        // B5D: leave an EXTRA-BAND column free where the data determine it, and let the
        // channel's sum-to-zero row carry the level.  The band-5 half of the same
        // principle RF-0f applied to the band-2 companion — there the correction is
        // centimetres and estimable without ionosphere, here it is metres and estimable
        // with the pinned pair's ionosphere and clock (card TBI section 5).
        network_config_.sat_gf_extra_band_free =
            jval<bool>(json, "network.sat_gf_extra_band_free",
                       "sat_gf_extra_band_free", false);
        // Card L5P: the CODE-WEIGHT half of the band-5-code-removed emulation.  With
        // B5D armed the extra-band code-bias column stays determined by the extra-band
        // code rows alone, so raising their sigma keeps the wire's band-5 code bias
        // while the rows lose their pull on the shared states.  1.0 is the default and
        // is never applied (byte-identical); below 1 is refused rather than clamped —
        // a config that asks to TRUST an extra band more than the pinned pair is asking
        // for the opposite of this knob and is a typo, not a posture.
        network_config_.extra_band_code_sigma_scale =
            jval<double>(json, "network.extra_band_code_sigma_scale",
                         "extra_band_code_sigma_scale", 1.0);
        if (!(network_config_.extra_band_code_sigma_scale >= 1.0)) {
            log->error("Network (card L5P): network.extra_band_code_sigma_scale = {} is not at "
                       "or above 1 (or is not a number) — falling back to 1.0 (no scaling).  "
                       "The knob only ever DEWEIGHTS the code rows of a signal whose band is "
                       "neither band of the constellation's ionosphere-free pin pair.",
                       network_config_.extra_band_code_sigma_scale);
            network_config_.extra_band_code_sigma_scale = 1.0;
        }
        // Card SCC: the SECOND CODE CHANNEL.  A station contributes every allow-listed
        // band-1 code it reports besides the one its canonical latch selected, so the
        // C1C and C1W satellite families stop competing for the same 59 stations.  The
        // two interlocks (channel_mode and carry_rx_code_bias) are checked in the
        // post-parse block below, where both of their inputs are populated.
        network_config_.second_code_channel =
            jval<bool>(json, "network.second_code_channel", "second_code_channel", false);
        {
            const auto& nwj = jsection(json, "network");
            if (nwj.contains("second_code_signals")) {
                const auto& arr = nwj["second_code_signals"];
                if (!arr.is_array()) {
                    log->error("Network (card SCC): network.second_code_signals must be an "
                               "array of RINEX code types (e.g. [\"C1C\", \"C1W\"]) — keeping "
                               "the default list.");
                } else {
                    // The list is EXPLICIT (R1): a refused entry is dropped and named, never
                    // silently coerced, and an all-refused list leaves the allow-list empty
                    // so the knob is inert rather than wrong.
                    std::vector<std::string> sigs;
                    for (const auto& s : arr) {
                        if (!s.is_string()) {
                            log->error("Network (card SCC): network.second_code_signals holds a "
                                       "non-string entry — ignored.");
                            continue;
                        }
                        const std::string sig = s.get<std::string>();
                        std::string why;
                        if (!secondCodeAllowEntryValid(sig, &why)) {
                            log->error("Network (card SCC): network.second_code_signals entry "
                                       "'{}' is refused ({}) — ignored.", sig, why);
                            continue;
                        }
                        if (std::find(sigs.begin(), sigs.end(), sig) != sigs.end()) continue;
                        sigs.push_back(sig);
                    }
                    network_config_.second_code_signals = std::move(sigs);
                }
            }
        }
        network_config_.second_code_sigma_scale =
            jval<double>(json, "network.second_code_sigma_scale",
                         "second_code_sigma_scale", 1.0);
        if (!(network_config_.second_code_sigma_scale >= 1.0)) {
            log->error("Network (card SCC): network.second_code_sigma_scale = {} is not at or "
                       "above 1 (or is not a number) — falling back to 1.0 (no scaling).  The "
                       "knob only ever DEWEIGHTS the code rows that route through the receiver "
                       "inter-signal state.",
                       network_config_.second_code_sigma_scale);
            network_config_.second_code_sigma_scale = 1.0;
        }
        // Card SID phase 6a: the SECOND PHASE CHANNEL, the phase twin of the block
        // above.  A station contributes every allow-listed band-1 carrier phase it
        // reports besides the one its canonical latch selected, so the L1C and L1W
        // satellite phase-bias families stop competing for the same stations.  The
        // three interlocks are checked in the post-parse block below, where the
        // vehicle, the held basis and the channel mode are all populated.
        network_config_.second_phase_channel =
            jval<bool>(json, "network.second_phase_channel", "second_phase_channel", false);
        {
            const auto& nwj = jsection(json, "network");
            if (nwj.contains("second_phase_signals")) {
                const auto& arr = nwj["second_phase_signals"];
                if (!arr.is_array()) {
                    log->error("Network (card SID phase 6a): network.second_phase_signals "
                               "must be an array of RINEX phase types (e.g. [\"L1C\", "
                               "\"L1W\"]) — keeping the default list.");
                } else {
                    // The list is EXPLICIT: a refused entry is dropped and named, never
                    // silently coerced, and an all-refused list leaves the allow-list
                    // empty so the knob disarms rather than acting on a guess.
                    std::vector<std::string> sigs;
                    for (const auto& s : arr) {
                        if (!s.is_string()) {
                            log->error("Network (card SID phase 6a): "
                                       "network.second_phase_signals holds a non-string "
                                       "entry — ignored.");
                            continue;
                        }
                        const std::string sig = s.get<std::string>();
                        std::string why;
                        if (!secondPhaseAllowEntryValid(sig, &why)) {
                            log->error("Network (card SID phase 6a): "
                                       "network.second_phase_signals entry '{}' is refused "
                                       "({}) — ignored.", sig, why);
                            continue;
                        }
                        if (std::find(sigs.begin(), sigs.end(), sig) != sigs.end()) continue;
                        sigs.push_back(sig);
                    }
                    network_config_.second_phase_signals = std::move(sigs);
                }
            }
        }
        network_config_.second_phase_sigma_scale =
            jval<double>(json, "network.second_phase_sigma_scale",
                         "second_phase_sigma_scale", 1.0);
        if (!(network_config_.second_phase_sigma_scale >= 1.0)) {
            log->error("Network (card SID phase 6a): network.second_phase_sigma_scale = {} "
                       "is not at or above 1 (or is not a number) — falling back to 1.0 (no "
                       "scaling).  The knob only ever DEWEIGHTS the second-phase rows.",
                       network_config_.second_phase_sigma_scale);
            network_config_.second_phase_sigma_scale = 1.0;
        }

        if (network_config_.extra_band_code_sigma_scale > 1.0)
            log->info("Network (card L5P): network.extra_band_code_sigma_scale = {:.3f} — "
                      "EXTRA-BAND code rows fold at sigma x {:.3f} (information / {:.3f}) in "
                      "the network solve; the closed-loop consumer stays at 1.",
                      network_config_.extra_band_code_sigma_scale,
                      network_config_.extra_band_code_sigma_scale,
                      network_config_.extra_band_code_sigma_scale
                          * network_config_.extra_band_code_sigma_scale);

        if (estimate_network_sat_code_bias_) {
            estimate_sat_biases_   = true;   // promote the satellite code bias to a state
            promote_sat_code_bias_ = true;   // estimate it on every raw code row (not fallback-only)
            estimate_ifb_          = true;   // receiver DCB/IFB — an explicit rx code-bias state on the
                                             // undifferenced rows, or the satellite OSB absorbs it
            // GIM-weighted iono constraint: the satellite DCB is co-linear with the
            // free per-(rx,sat) slant iono until the GIM imposes between-satellite
            // structure (common-mode-free R leaves the per-constellation level free,
            // pinned by the explicit sum-to-zero DCB datum).  runNetwork hard-errors
            // if the GIM is absent at startup (the slice would be inert without it).
            //
            // ...UNLESS the SLANT posture is armed.  network.code_bias_gf_datum selects
            // Odijk's §5.1 model instead: the ionosphere stays free per-LOS slant, the
            // geometry-free code biases are pinned as datum (rd 2b/3b) and absorbed by
            // that ionosphere, and the estimable code bias is the modernized DCB from
            // the THIRD frequency onward.  Forcing an iono constraint there would both
            // contradict the model and destroy the pinned direction, so the force is
            // conditional rather than unconditional.
            //
            // ...or UNLESS RF-0b's no-constraint posture is selected.  Under GPS-only
            // DUAL the GF-datum escape above is unreachable (it hard-errors at the
            // frequency gate below), so this force was the reason a dual W6 run could
            // not be self-contained: it demanded an external IONEX GIM, against the
            // standing "no external-data dependence" rule, and on the sim bed the COD
            // GIM IS the truth ionosphere.  network.w6_iono_constraint = "none" clears
            // the force explicitly — not merely skips it — so a config that ALSO wrote
            // estimation.ionosphere_weighting.enable = true (every RF-0 lineage did,
            // because the loader forced it anyway) cannot leave a half-armed constraint
            // behind.  The GIM itself is never loaded on this branch
            // (ApplicationNetworkSetup), so the flag would be inert, but a flag that
            // reads ON while nothing folds is standing invariant #4's exact shape.
            if (w6_iono_none)                 iono_weighting_.enabled = false;
            else if (!network_code_bias_gf_datum_) iono_weighting_.enabled = true;
            // Estimate the FULL satellite code bias from scratch in the network's own
            // datum — NOT a residual to .BIA.  Skip the external .BIA on both producer
            // and (via the derived consumer config) the held-out consumer, so the
            // network OSB is the single code-bias source and nothing double-corrects.
            model_corrections_.enable_code_phase_bias = false;
            // The producer never needs the phase-clock split for V1; keep it off so it
            // cannot add a spurious gauge to the shared system.
            estimate_phase_clock_ = false;
            log->info("Network producer (W6): satellite code-bias estimation ENABLED — forcing "
                      "estimate_sat_biases + promote_sat_code_bias + estimate_ifb (rx DCB) + "
                      "ionosphere_weighting = {}, enable_code_phase_bias (.BIA) = OFF; the network "
                      "emits its own satellite code biases (GPS + Galileo + BeiDou) under a "
                      "sum-to-zero per-constellation DCB datum.",
                      w6_iono_none ? "OFF (RF-0b: network.w6_iono_constraint = none)" : "on");
        } else if (network_external_sat_code_bias_) {
            // --- EXT-CB coupled slice (W6 minus the satellite column) --------------
            // The satellite code AND phase biases come from the .BIA, so there is no
            // satellite bias state and no OSB datum; what DOES have to travel with
            // the product is the receiver side.
            //
            // enable_code_phase_bias is the apply switch for the whole .BIA — code
            // and phase alike (PppModel::computeBaseModeledValue is keyed on the raw
            // signal string).  An explicit false in the JSON contradicts the posture
            // outright: the network would then have neither an external nor an
            // estimated satellite code bias, which is the V1 slant-absorption posture
            // wearing this knob's name.  Fail loud rather than silently override.
            if (!model_corrections_.enable_code_phase_bias) {
                log->error("Network: network.external_sat_code_bias is on but "
                           "model_corrections.enable_code_phase_bias is explicitly false — "
                           "the .BIA is the satellite bias SOURCE in this posture, so "
                           "disabling its application leaves the satellite code bias neither "
                           "applied nor estimated (that is the V1 slant-absorption posture: "
                           "drop network.external_sat_code_bias instead).  Remove the "
                           "explicit false, or drop the knob.");
                return false;
            }
            // Estimability trap #4 of the W6 slice, unchanged: the network is
            // DifferencingMode::None, so on undifferenced rows the per-(rx,
            // constellation) GF code lump has nowhere to go unless the receiver DCB
            // is an explicit pinned state.  Under W6 the satellite OSB would absorb
            // it; here there is no satellite column, so it would ride inside the
            // slant iono and fight the SH field (a per-station constant the global
            // field cannot represent, pulled into a₀₀ and the per-station residual).
            estimate_ifb_ = true;
            if (estimate_network_iono_sh_) {
                // Keep the SH slice's own coupled bundle intact — the elevation
                // cutoff under estimation.ionosphere_weighting is load-bearing for
                // the SH row builder (ApplicationNetworkAssembly.cpp:876,987) and
                // the posture ledger checks the enable alongside it.
                iono_weighting_.enabled = true;
                log->info("Network producer (EXT-CB): satellite code+phase biases taken from "
                          "the EXTERNAL .BIA (enable_code_phase_bias stays ON) with the "
                          "SH-VTEC ionosphere CO-ESTIMATED — the classical known-DCB global "
                          "iono setup.  No satellite code-bias state, no OSB datum; forcing "
                          "estimate_ifb (rx DCB) on so the receiver GF code lump has an "
                          "explicit state instead of riding inside the slant iono.  Satellite "
                          "CLOCK and PHASE BIAS are still estimated (as residuals to the "
                          "product), so the shared border is non-empty and the datum "
                          "machinery is still exercised.");
            } else {
                log->info("Network producer (EXT-CB): satellite code+phase biases taken from "
                          "the EXTERNAL .BIA (enable_code_phase_bias stays ON), free per-(rx,sat) "
                          "slant ionosphere, estimate_ifb (rx DCB) forced on.  NOTE: with no iono "
                          "constraint this differs from the plain V1 posture only by the forced rx "
                          "DCB state — the knob's purpose (SH-VTEC iono WITH external satellite "
                          "biases) needs network.estimate_iono_sh = true.");
            }
        } else {
            log->info("Network producer (V1): satellite code bias deferred "
                      "(estimate_sat_biases / promote_sat_code_bias = false)");
        }

        // --- CB-GF-DATUM validation (the parse is above, before the W6 chain) ----
        if (network_code_bias_gf_datum_) {
            if (!estimate_network_sat_code_bias_) {
                log->error("Network: network.code_bias_gf_datum is armed without "
                           "network.estimate_sat_code_bias — there are no satellite "
                           "code-bias columns to gauge, so the rows would fold onto "
                           "nothing.  Arm the W6 slice or drop the GF datum.");
                return false;
            }
            if (estimate_network_iono_sh_) {
                // The one genuinely destructive combination, so it is a hard error and
                // not a warning: with a single-layer constraint present, the
                // geometry-free satellite code bias IS the estimable DCB (Odijk 2016
                // Eq. 29-32), and pinning it to zero deletes the only code-bias content
                // a dual-frequency W6 product carries.
                log->error("Network: network.code_bias_gf_datum is armed together with "
                           "network.estimate_iono_sh.  Mutually exclusive by construction: "
                           "rank deficiency 3b (satellite hardware biases vs the free slant "
                           "ionosphere) only EXISTS when the ionosphere is unconstrained, "
                           "and under a single-layer constraint the geometry-free bias is "
                           "the estimable DCB — pinning it there throws away the code-bias "
                           "content the W6 product exists to emit.  Drop one.");
                return false;
            }
            if (iono_weighting_.enabled) {
                log->error("Network: network.code_bias_gf_datum is armed with "
                           "estimation.ionosphere_weighting.enable — same exclusivity as "
                           "above (an external-GIM constraint is still a single-layer "
                           "constraint).  Turn the iono weighting off for the slant "
                           "posture, or drop the GF datum.");
                return false;
            }
            // Frequency gate.  Below three frequencies the GF datum is not merely
            // unhelpful, it is the whole model: IF + GF together fix d_1 and d_2
            // exactly, so a dual-frequency run would emit satellite code biases that are
            // identically the datum — zeros wearing an estimate's clothing.  Odijk §5.1
            // states the same thing as estimability ("only for a triple- or
            // higher-frequency constellation").  frequency_mode is the only pre-run
            // signal available; "all" is data-dependent, so it passes with a warning
            // rather than silently.
            if (frequency_mode_ == FrequencyMode::Dual) {
                log->error("Network: network.code_bias_gf_datum with "
                           "processing.frequency_mode = {} is structurally empty — the IF "
                           "pin and the GF pin together fix BOTH pinned-pair code biases, "
                           "so nothing estimable remains and every emitted B record would "
                           "be the datum itself.  Odijk 2016 §5.1: code biases are "
                           "estimable only from the THIRD frequency onward under a slant "
                           "ionosphere.  Use frequency_mode triple/all, or drop the slant "
                           "posture.", "dual");
                return false;
            }
            // Announce it.  An armed datum that logs nothing is invariant #4's exact
            // shape — and it bit immediately: the truth grader auto-detects this posture
            // from the console, and without this line it silently graded the slant arm
            // with the single-layer transform (metre-scale phantom residuals on the two
            // pinned channels, which are datum-fixed to zero by construction).
            log->info("Network producer (CB-GF-DATUM): per-satellite GEOMETRY-FREE code-bias "
                      "pin ARMED (Odijk 2016 rd 3b) — (d2-d1)/(mu2-mu1) = 0 on the IF-pinned "
                      "pair.  With the IF pin this fixes the first two frequencies' code "
                      "biases EXACTLY, leaving the modernized DCB (j >= 3) as the estimable "
                      "content; the geometry-free lump is absorbed by the free slant "
                      "ionosphere (their Table 8), so a consumer must apply the ionosphere "
                      "and the code biases AS A PAIR.");
            if (frequency_mode_ == FrequencyMode::All)
                log->warn("Network (CB-GF-DATUM): processing.frequency_mode = all — the "
                          "third frequency this posture needs is then DATA-dependent.  A "
                          "satellite tracked on only two bands has its code bias fully "
                          "fixed by the datum with nothing estimable left; the run-end "
                          "count of such satellites is reported (n_gf_only_pair).");
        }

        // --- RF-0b validation: the no-constraint W6 posture ---------------------
        // Placed immediately after the CB-GF-DATUM block so the two no-ionosphere-
        // constraint postures are validated side by side and read as the alternatives
        // they are.  Both answer rank deficiency 3b; they differ ONLY in the datum:
        // the GF pin fixes the per-satellite geometry-free bias to zero (and needs a
        // third frequency to leave anything estimable), while "none" leaves it
        // unpinned and lets the per-(constellation, signal) sum-to-zero rows realize
        // the IGS Sum_s DCB^s = 0 convention.
        if (w6_iono_none) {
            if (!estimate_network_sat_code_bias_) {
                // No OSB columns ⇒ no satellite-DCB rank deficiency to answer ⇒ the
                // option decides nothing.  Standing invariant #4: an armed-but-inert
                // knob must READ as inert rather than as the thing keeping the run
                // honest.  A warn, not an error: nothing is broken, it is just idle.
                log->warn("Network (RF-0b): network.w6_iono_constraint = none is INERT without "
                          "network.estimate_sat_code_bias — with no satellite code-bias columns "
                          "there is no satellite-DCB / slant-ionosphere rank deficiency for an "
                          "ionosphere constraint to answer, and nothing about the ionosphere "
                          "posture changes.  Arm the W6 slice, or drop the option.");
            }
            if (estimate_network_iono_sh_) {
                // The co-estimated SH-VTEC field IS a single-layer ionosphere
                // constraint (the loader arms the W6 slice FROM it, :2844) — asking
                // for "no constraint" alongside it is a contradiction, not a
                // preference, and silently letting one win would ship a product whose
                // datum nobody chose.  Same fail-loud contract the GF datum uses
                // against the same knob.
                log->error("Network (RF-0b): network.w6_iono_constraint = none is armed together "
                           "with network.estimate_iono_sh.  Mutually exclusive: the co-estimated "
                           "SH-VTEC field IS the W6 ionosphere constraint (it is what arms the "
                           "slice at all), so 'none' would be either a lie or a silent deletion "
                           "of the field's constraint rows.  Drop one — 'none' is the free-slant "
                           "posture, estimate_iono_sh the single-layer one.");
                return false;
            }
            if (network_code_bias_gf_datum_) {
                // Both are no-ionosphere-constraint postures, so this is not a rank
                // conflict — it is a DATUM ambiguity, which is worse, because the run
                // would still solve and emit a product whose satellite code-bias frame
                // depends on which branch happened to be tested first.
                log->error("Network (RF-0b): network.w6_iono_constraint = none is armed together "
                           "with network.code_bias_gf_datum.  Both remove the ionosphere "
                           "constraint, but they are DIFFERENT datums for the same rank "
                           "deficiency 3b: the GF pin fixes each satellite's geometry-free code "
                           "bias to zero (estimable content only from the third frequency), while "
                           "'none' leaves it free under the per-(constellation, signal) "
                           "sum-to-zero rows.  Pick one — arming both makes the emitted OSB frame "
                           "depend on branch order.");
                return false;
            }
            // Announce it.  An armed datum that logs nothing is standing invariant #4's
            // exact shape, and the truth/wire graders auto-detect the posture from the
            // console: without this line an RF-0b product would be graded against the
            // single-layer transform.  The declared S-basis dimension is named here on
            // purpose — the code-bias gauge rows are CONSTRAINT rows folded before the
            // null-space gate (NetworkSolveGlue), and NetworkSbasis::buildSharedDatum
            // sets plan.declared_dimension from the per-constellation CLOCK pin alone,
            // so removing the ionosphere constraint cannot move it.  What moves is the
            // OBSERVED count, and the gate is the instrument that reports it.
            if (estimate_network_sat_code_bias_)
                log->info("Network producer (RF-0b / W6 no-constraint): NO ionosphere constraint "
                          "— free per-LOS slant ionosphere, no external GIM loaded and "
                          "estimation.ionosphere_weighting forced OFF.  THE SATELLITE CODE-BIAS "
                          "DATUM IN FORCE IS the per-satellite ionosphere-free pin plus the "
                          "per-(constellation, signal) SUM-TO-ZERO rows (IGS Sum_s DCB^s = 0), "
                          "folded as constraint rows before the null-space gate — so the DECLARED "
                          "S-basis dimension is UNCHANGED at the per-constellation clock-pin "
                          "count.  The geometry-free lump is absorbed by the slant ionosphere, so "
                          "a consumer must apply the emitted ionosphere and the emitted code "
                          "biases AS A PAIR.  WARNING: the sum-to-zero rows pin each channel's "
                          "constellation-wide LEVEL only; the n_sat-1 BETWEEN-satellite "
                          "geometry-free directions the GIM used to close are left to whatever "
                          "loose prior sits on the OSB columns (network.cb_kaula_prior).  Read the "
                          "null-space gate's observed-vs-declared and the run-end W6-IONO-NONE "
                          "line before grading the emitted code biases.");
        }

        // --- RF-0c validation: the hard per-satellite geometry-free datum -------
        // Placed after the RF-0b block so `w6_iono_none` and the W6 chain's forced
        // `iono_weighting_.enabled` are both settled and can be tested as they will
        // actually run, not as the JSON wrote them.
        if (sat_gf_on) {
            const char* mode_name =
                sat_gf_mode == NetworkSatGfDatum::Zero ? "zero" : "broadcast_tgd";
            if (!estimate_network_sat_code_bias_) {
                log->warn("Network (RF-0c): network.sat_gf_datum = {} is INERT without "
                          "network.estimate_sat_code_bias — there are no satellite "
                          "code-bias columns, so there is no geometry-free combination to "
                          "pin and the rows would fold onto nothing.  Arm the W6 slice, or "
                          "drop the datum.", mode_name);
            }
            if (!w6_iono_none) {
                // The card's own precondition.  An ionosphere constraint DETERMINES this
                // direction from data (the GIM's common-mode-free R, or the SH field's
                // between-satellite structure); pinning it as well is not redundancy, it
                // is a second datum fighting the data for the same parameter — and the
                // emitted code biases would then be neither the data's nor the pin's.
                log->error("Network (RF-0c): network.sat_gf_datum = {} requires "
                           "network.w6_iono_constraint = none.  The per-satellite "
                           "geometry-free direction is a DATUM only when the ionosphere is "
                           "unconstrained; with the external GIM or the co-estimated "
                           "SH-VTEC field armed it is DETERMINED BY DATA (Odijk 2016 rd 3b "
                           "exists only under a free slant ionosphere), so this pin would "
                           "fight the constraint instead of replacing it.  Arm "
                           "w6_iono_constraint = none, or drop sat_gf_datum.", mode_name);
                return false;
            }
            if (estimate_network_iono_sh_ || iono_weighting_.enabled) {
                // Belt and braces: w6_iono_constraint = none already refuses the SH field
                // and forces the weighting off, so reaching here means a future edit
                // re-opened a path to an armed constraint.  Fail rather than emit a
                // double-datumed product that still looks healthy at the gate.
                log->error("Network (RF-0c): network.sat_gf_datum = {} is armed with an "
                           "ionosphere constraint still live (estimate_iono_sh = {}, "
                           "estimation.ionosphere_weighting.enable = {}).  Mutually "
                           "exclusive — one datum per rank deficiency.  Drop one.",
                           mode_name, estimate_network_iono_sh_, iono_weighting_.enabled);
                return false;
            }
            if (network_code_bias_gf_datum_) {
                // Both build a geometry-free pin on the same pair.  Two identical rows is
                // not a stronger datum, it is a singular constraint stack.
                log->error("Network (RF-0c): network.sat_gf_datum = {} is armed together "
                           "with network.code_bias_gf_datum.  Both pin the SAME "
                           "per-satellite geometry-free combination on the SAME "
                           "ionosphere-free pin pair — arming both folds the row twice.  "
                           "Use code_bias_gf_datum for the triple-frequency Odijk posture, "
                           "sat_gf_datum for the dual-frequency one.", mode_name);
                return false;
            }
            if (sat_gf_mode == NetworkSatGfDatum::BroadcastTgd)
                log->info("Network producer (RF-0c): HARD per-satellite GEOMETRY-FREE "
                          "code-bias datum ARMED at the BROADCAST TGD — one pin row per "
                          "satellite on (d2-d1)/(mu2-mu1) over the ionosphere-free pin's "
                          "own pair, RHS = c*T_GD from the navigation message "
                          "(IS-GPS-200 §20.3.3.3.3.2: the broadcast clock is L1/L2 "
                          "ionosphere-free, so d1 = c*T_GD and d2 = c*gamma*T_GD, whose "
                          "IF combination is EXACTLY zero and whose geometry-free "
                          "combination is EXACTLY c*T_GD — the two pins compose).  "
                          "Run-latched per satellite so the datum cannot churn mid-run.  "
                          "Folded as a constraint row BEFORE the null-space gate, so the "
                          "DECLARED S-basis dimension is UNCHANGED.  The estimable slant "
                          "ionosphere is then referenced to the broadcast DCB convention; "
                          "a consumer must apply the emitted ionosphere and code biases AS "
                          "A PAIR.");
            else
                log->info("Network producer (RF-0c): HARD per-satellite GEOMETRY-FREE "
                          "code-bias datum ARMED at ZERO — one pin row per satellite on "
                          "(d2-d1)/(mu2-mu1) over the ionosphere-free pin's own pair, RHS "
                          "0 (Odijk 2016 rd 3b, Eq. 30; the same arbitrary-but-legal S-basis "
                          "choice network.code_bias_gf_datum makes, reached here at DUAL "
                          "frequency).  Folded as a constraint row BEFORE the null-space "
                          "gate, so the DECLARED S-basis dimension is UNCHANGED.  The "
                          "geometry-free lump moves into the slant ionosphere (their "
                          "Table 8), so a consumer must apply the emitted ionosphere and "
                          "code biases AS A PAIR.");
        }

        // --- RF-0e validation: the two level-datum arms -------------------------
        if (network_config_.sat_if_level_datum || network_config_.sat_gf_datum_time_linked) {
            if (!sat_gf_on) {
                log->warn("Network (RF-0e): network.sat_if_level_datum / "
                          "network.sat_gf_datum_time_linked are INERT without "
                          "network.sat_gf_datum — the first replaces the per-signal "
                          "sum-to-zero rows that ONLY the geometry-free datum suppresses, "
                          "and the second re-targets rows that only it builds.  Arm "
                          "sat_gf_datum, or drop these.");
            }
            if (network_config_.sat_if_level_datum
                && network_config_.sat_gf_datum_time_linked) {
                // They are the two arms of one A/B on the same regression.  Arming both
                // does not measure either, and the result would be attributed to
                // whichever the reader happened to believe in.
                log->error("Network (RF-0e): network.sat_if_level_datum and "
                           "network.sat_gf_datum_time_linked are the TWO ARMS of one A/B "
                           "on the same defect (RF-0d's consumer regression: an absolute "
                           "geometry-free RHS against time-linked IF pins).  Arming both "
                           "measures neither — the outcome would be attributable to "
                           "whichever mechanism the reader already believed.  Pick one.");
                return false;
            }
            if (network_config_.sat_if_level_datum && sat_gf_on)
                log->info("Network producer (RF-0e e1): ABSOLUTE IF-LEVEL datum ARMED — one "
                          "row per constellation pinning Sum_s (a1*d1 + a2*d2) = 0 over the "
                          "satellites with a valid ionosphere-free pin pair, folded at the "
                          "gauge weight beside the per-satellite pins.  It constrains ONLY "
                          "the ionosphere-free part, which the geometry-free pins leave "
                          "free (they fix differences within a satellite, never the level), "
                          "so the two are consistent by construction — and under "
                          "broadcast_tgd the target is EXACTLY true, because the broadcast "
                          "clock is L1/L2 ionosphere-free and every satellite's "
                          "ionosphere-free code bias is zero in that frame.  This is the "
                          "level anchor network.sat_gf_datum's sum-to-zero suppression "
                          "removed: with code_datum_time_linked the per-satellite IF pins "
                          "are CHANGE pins, so each level is anchored once at first "
                          "sighting and carried, and nothing holds the SET.  NOTE the row is "
                          "STRONG, not gentle: its Jacobian carries one entry per satellite, so "
                          "at equal gauge weight it beats the n single-entry change pins and "
                          "re-zeroes the LEVEL each epoch (realized level = carried/(n+1)); the "
                          "per-satellite DIFFERENCES stay carried.");
            if (network_config_.sat_gf_datum_time_linked && sat_gf_on)
                log->info("Network producer (RF-0e e2): geometry-free datum rows TIME-LINKED "
                          "— each row's RHS is that satellite's PREVIOUS kept-epoch solved "
                          "geometry-free combination when both pinned signals carry a "
                          "veteran value, and the absolute target ({}) only at FIRST "
                          "SIGHTING.  The alternative reading of RF-0d's regression: rather "
                          "than add a level row, stop the absolute geometry-free family "
                          "pulling against the time-linked IF pins.  The first-sighting "
                          "anchor stays in force, so the frame is still the broadcast one; "
                          "what changes is that it is carried rather than re-asserted.",
                          sat_gf_mode == NetworkSatGfDatum::BroadcastTgd ? "c*T_GD" : "0");
        }

        // --- RF-0f validation: the ISC reparameterization ------------------------
        if (network_config_.sat_isc_state) {
            if (!sat_gf_on) {
                log->warn("Network (RF-0f): network.sat_isc_state is INERT without "
                          "network.sat_gf_datum — it decides which companion columns that "
                          "datum does NOT pin, and without the datum nothing is pinned.");
            } else {
                log->info("Network producer (RF-0f): band-2 ISC left as a FREE per-satellite "
                          "difference — the broadcast T_GD pin applies to the P-code pair "
                          "only, and a same-band companion on the pin's second band (GPS C2X "
                          "beside C2W) gets NO geometry-free row.  Its satellite code bias is "
                          "then b(C2W) + ISC^s on the column it already owns, and the ISC is "
                          "estimable at any station tracking BOTH signals of that satellite "
                          "(their difference is a pure bias difference — no ionosphere, no "
                          "clock).  RF-0d's row asserted ISC = 0, a value RINEX navigation "
                          "does not carry.  A satellite with NO dual-tracking station has an "
                          "unobservable ISC: its companion is pinned at 0 as before, counted, "
                          "and its bias is NOT emitted — the wire carries isc=obs only on "
                          "records whose ISC the data actually determined.");
            }
        }

        // --- B5D validation: the extra-band code datum ---------------------------
        if (network_config_.sat_gf_extra_band_free) {
            if (!sat_gf_on) {
                log->warn("Network (B5D): network.sat_gf_extra_band_free is INERT without "
                          "network.sat_gf_datum — it decides which EXTRA-BAND columns that "
                          "datum does NOT pin, and without the datum nothing is pinned.");
            } else {
                log->info("Network producer (B5D): EXTRA-BAND satellite code-bias columns "
                          "left FREE where the data determine them — the broadcast T_GD pin "
                          "applies to the ionosphere-free pair only, and a signal on a THIRD "
                          "band (GPS C5Q/C5X beside the C1/C2 pair) gets NO geometry-free "
                          "row wherever some station carries code on both of the pair's "
                          "bands and on that signal for that satellite.  With the slant "
                          "ionosphere and the clock fixed by the pinned pair, d_k is then "
                          "determined up to ONE constant per (constellation, signal) family "
                          "— the constant the receiver's own extra-band code bias absorbs — "
                          "and the channel's sum-to-zero row is that constant's datum, "
                          "re-enabled automatically because a freed column is not in the "
                          "pinned set.  RF-0d's row asserted an inter-signal correction of "
                          "ZERO, which is centimetres between same-band channels and METRES "
                          "across a band (card TBI: the Block III satellites sit 4 to 5 m "
                          "above the Block IIF ones in the estimator-free triple-code "
                          "combination).  A column the data do NOT determine this epoch is "
                          "carried at its own previous solved geometry-free combination, "
                          "or, at a first sighting with nothing to carry, HELD by a level "
                          "row in the channel's own frame and WITHHELD from the wire — the "
                          "absolute broadcast target is never folded into a family that now "
                          "stands in the sum-to-zero frame.");
                if (!network_config_.sat_gf_datum_time_linked)
                    log->warn("Network (B5D): network.sat_gf_datum_time_linked is OFF.  The "
                              "extra-band columns the data do not determine still CARRY "
                              "their own previous solved combination (R3 forbids the "
                              "absolute target in the sum-to-zero frame), so those rows are "
                              "time-linked while every other geometry-free row on this "
                              "posture re-asserts its absolute target each epoch.  That is "
                              "the mixed-frame pull RF-0e measured; arm "
                              "sat_gf_datum_time_linked with this knob.");
            }
        }

        // --- FAMILY-GF: the rd-3b pin, per partitioned secondary channel family ---
        // Placed AFTER the CB-GF-DATUM block closes so it inherits that block's four hard
        // errors for free (sat code bias required; iono_sh / iono_weighting / dual all
        // refused above, each with `return false`).  The only interlock left to state is
        // the parent-knob one: there is no primary GF frame to be secondary TO.
        if (network_code_bias_gf_datum_per_family_) {
            if (!network_code_bias_gf_datum_) {
                log->error("Network: network.code_bias_gf_datum_per_family is armed without "
                           "network.code_bias_gf_datum.  The per-family rows are SECONDARY to "
                           "the primary geometry-free pin — they gauge the families the "
                           "primary pin cannot reach — so without it there is no primary GF "
                           "frame to be secondary to.  Arm network.code_bias_gf_datum, or "
                           "drop this knob.");
                return false;
            }
            log->info("Network producer (FAMILY-GF): per-(satellite, secondary-family) "
                      "GEOMETRY-FREE code-bias pin ARMED.  Odijk rd 3b re-derives PER "
                      "RECEIVER CHANNEL FAMILY when the station population partitions (the "
                      "Galileo C/Q-vs-X tracking split): the primary pin never reaches the "
                      "secondary family's columns.  One GF row per (sat, partitioned family) "
                      "and deliberately NO IF row — the satellite clock is shared and already "
                      "realized by the primary IF pin, so the secondary family's IF "
                      "combination is the ESTIMABLE inter-family ISC.  The run-latched family "
                      "census reports the partition verdict and the folded row count; with no "
                      "partitioned family this knob folds NOTHING and the run is "
                      "byte-identical.");
        }

        // --- RXCB-DATUM: sum-to-zero over the RECEIVER code-bias set -------------
        // The receiver-side counterpart of the W6 satellite code-bias gauge.
        // Parsed after the EXT-CB slice so its estimate_ifb force is visible to the
        // inert check below.
        network_rx_code_bias_datum_ = jval<bool>(
            json, "network.rx_code_bias_datum", "rx_code_bias_datum", false);
        network_rx_code_bias_datum_sigma_m_ = jval<double>(
            json, "network.rx_code_bias_datum_sigma_m", "rx_code_bias_datum_sigma_m", 10.0);
        if (network_rx_code_bias_datum_ && !(network_rx_code_bias_datum_sigma_m_ > 0.0)) {
            // sigma <= 0 means info = 1/0 = inf, i.e. a HARD pin of every receiver DCB
            // to zero — the opposite of a loose bound, and a silent datum takeover that
            // would destroy the estimable between-station differences.
            log->error("Network: network.rx_code_bias_datum_sigma_m must be > 0 (got {}); "
                       "a non-positive sigma is an infinite-information HARD pin of every "
                       "receiver DCB to zero, not a datum.", network_rx_code_bias_datum_sigma_m_);
            return false;
        }
        // Card RF-PD-fix cross-check (deferred from station_prior_before_pd's own
        // arming block — rx_code_bias_datum is parsed only here).  Standing
        // invariant #4: the fix moves the RXCB-DATUM rows earlier, so without that
        // datum there is nothing to move and the knob is INERT while reading armed.
        if (network_config_.station_prior_before_pd && !network_rx_code_bias_datum_) {
            log->warn("Network (card RF-PD-fix): network.station_prior_before_pd is armed but "
                      "network.rx_code_bias_datum is OFF — the fix moves exactly the RXCB-DATUM "
                      "rows ahead of the PD test, so with no datum rows to move it is INERT and "
                      "the addStation rejections will be unchanged.  Arm rx_code_bias_datum too.");
        }
        if (network_rx_code_bias_datum_) {
            if (!estimate_ifb_) {
                // No explicit receiver-DCB state ⇒ no columns ⇒ nothing to gauge.
                // Standing invariant #4: an armed-but-inert knob reads as working.
                log->warn("Network: network.rx_code_bias_datum is INERT without "
                          "estimation.estimate_ifb — with no explicit receiver DCB state the "
                          "receiver code delay rides inside the slant ionosphere and there are "
                          "no columns to gauge.  Arm estimate_ifb (network.external_sat_code_bias "
                          "forces it) or drop the datum.");
            } else if (estimate_network_sat_code_bias_) {
                // Legal and not over-constraining — the two gauges act on DISJOINT
                // parameter sets (satellite columns vs receiver columns) — but under
                // W6 the satellite gauge already determines the receiver half
                // through the iono, so the receiver gauge is REDUNDANT rather than
                // load-bearing.  Redundant minimum-constraint coverage is harmless
                // (Weyl: PSD information cannot create a new near-zero, the same
                // argument that makes W6's per-SIGNAL over-pinning safe), but say so
                // rather than let it read as the thing keeping the run alive.
                log->info("Network producer (RXCB-DATUM): receiver code-bias sum-to-zero ON "
                          "ALONGSIDE the W6 satellite code-bias gauge.  Legal (disjoint column "
                          "sets, redundant coverage is PSD-safe) but the receiver half is "
                          "already determined through the satellite gauge here — this datum is "
                          "load-bearing only under network.external_sat_code_bias.");
            } else {
                log->info("Network producer (RXCB-DATUM): receiver code-bias datum ON — a loose "
                          "zero-mean prior d_r ~ N(0, {:.1f}^2 m) on every station's receiver DCB, "
                          "folded PRE-reduce through the diagonal prior channel (the same "
                          "loose-physical-bound idiom as cb_kaula_prior on the satellite side).  "
                          "It is a DATUM, not a constraint: the between-station DIFFERENCES are "
                          "estimable and carry orders more data information, so the prior does not "
                          "move them; the COMMON MODE has essentially none, so the prior alone "
                          "sets it — to zero-mean-over-receivers, the IGS DCB convention.  This "
                          "replaces the datum the satellite code-bias sum-to-zero was silently "
                          "also providing: without it the receiver-DCB common mode is confounded "
                          "with the global ionosphere LEVEL (only the mapping-function spread "
                          "separates them) and a00 walks while its formal sigma collapses.  "
                          "Realized common-mode information is 1/sigma^2 = {:.3e}, INDEPENDENT of "
                          "the station count — it must clear the per-epoch rank cutoff "
                          "(~1e-9*lambda_max) on its own.  Pre-reduce deliberately: a post-reduce "
                          "cross-station Woodbury gauge cannot compose with the Card-4 DD priors "
                          "(NetworkEstimator::solve refuses it).",
                          network_rx_code_bias_datum_sigma_m_,
                          1.0 / (network_rx_code_bias_datum_sigma_m_
                                 * network_rx_code_bias_datum_sigma_m_));
            }
        }

        // CB-ADM: run-latched W6 code-bias admission census + station-local
        // quarantine.  Replaces the per-epoch >= 2-station gate (whose count
        // FLAPPING at BeiDou scale churns prune / re-register / re-seed and
        // spawns datum-orphan near-null directions that withhold whole epochs)
        // with a one-shot startup census.  Only effective when the W6 slice is
        // armed; armed-without-W6 is INERT (the code-bias registration block
        // never runs) — warn, per the established armed-but-ineffective pattern.
        network_cb_admission_ =
            jval<bool>(json, "network.cb_admission", "cb_admission", false);
        network_cb_admission_min_stations_ = jval<int>(
            json, "network.cb_admission_min_stations", "cb_admission_min_stations", 4);
        network_cb_admission_census_epochs_ = jval<int>(
            json, "network.cb_admission_census_epochs", "cb_admission_census_epochs", 15);
        if (network_cb_admission_min_stations_ < 1) {
            log->error("Network: network.cb_admission_min_stations must be >= 1 "
                       "(got {}); effective_min = max(2, min(this, n_census_stations)).",
                       network_cb_admission_min_stations_);
            return false;
        }
        if (network_cb_admission_census_epochs_ < 1) {
            log->error("Network: network.cb_admission_census_epochs must be >= 1 (got {}).",
                       network_cb_admission_census_epochs_);
            return false;
        }
        if (network_cb_admission_) {
            if (estimate_network_sat_code_bias_) {
                log->info("Network producer (CB-ADM): W6 code-bias admission census ON — "
                          "latching each (sat,signal) code pair over the first {} loop "
                          "epochs (>= {} supporting stations, each sustained over "
                          ">= ceil(K_eff/2) epochs, and >= 2 distinct bands per satellite "
                          "for the IF pin).  Admitted pairs become shared border columns; "
                          "sub-threshold pairs keep their measurements and their per-station "
                          "sat-bias state as station-local sponges (kept, never pruned, "
                          "never datumed, never emitted).",
                          network_cb_admission_census_epochs_,
                          network_cb_admission_min_stations_);
            } else {
                log->warn("Network: network.cb_admission = true is INERT — the W6 code-bias "
                          "slice is off (network.estimate_sat_code_bias / estimate_iono_sh), "
                          "so no code-bias columns are registered for the census to gate.");
            }
        }

        if (estimate_network_iono_sh_) {
            log->info("Network producer (SH-iono): co-estimating the global single-layer "
                      "SH-VTEC ionosphere (degree {}, shell {:.0f} km, {}) jointly with the "
                      "satellite code biases — the network's OWN iono constraint REPLACES the "
                      "external GIM (.INX is NOT loaded; no double-apply). Every coefficient "
                      "carries a Kaula prior (a₀₀ loose: {:.0f}±{:.0f} TECU) so the null-space "
                      "rank gate stays at #constellations regardless of coverage.",
                      global_vtec_sh_.max_degree, global_vtec_sh_.layer_height_km,
                      global_vtec_sh_.sun_fixed ? "sun-fixed" : "geographic",
                      global_vtec_sh_.mean_vtec_prior_tecu,
                      global_vtec_sh_.mean_vtec_prior_sigma_tecu);
        }

        // Card LV-1 (docs/SPEC_level_information_budget.md): seed the per-LOS slant
        // iono states in the FIELD's frame rather than the receiver-bias-in frame.
        // Parsed here, next to the SH field it seeds against; the banner + the
        // inert-without-a-field guard run in the post-parse block (with the LV-2 one).
        network_iono_seed_field_frame_ = jval<bool>(
            json, "network.iono_seed_field_frame", "iono_seed_field_frame", false);
        network_iono_seed_field_frame_min_sats_ = jval<int>(
            json, "network.iono_seed_field_frame_min_sats",
            "iono_seed_field_frame_min_sats", 5);

        return true;
    }

    // =================================================================
    //  network subsection helper: loadNetworkAprioriAndGlonass_
    // =================================================================
    /**
     * @brief RTF-1/RTF-2 a-priori orbit/clock source and GLONASS admission.
     *
     * Ordered after the W6 / iono-SH coupling above so the @c brdc_rts validation sees final
     * flag values.  GLONASS admission forces the per-FCN-slot receiver IFB and is mutually
     * exclusive with the satellite code-bias slice in V1.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     * @return @c false on a fatal misconfiguration (aborts @c loadFromJson).
     */
    bool GnssConfig::loadNetworkAprioriAndGlonass_(const nlohmann::json& json,
                                                   const std::shared_ptr<spdlog::logger>& log) {
        // --- RTF-1/RTF-2: producer a-priori orbit/clock source -------------------
        // network.apriori_source selects what the producer's per-satellite
        // a-priori clock (and the orbit its geometry uses) is referenced to.  The
        // default "sp3" is today's behaviour (precise SP3/CLK identity) and is
        // byte-identical everywhere.  "brdc_rts" references the a-priori to the
        // broadcast ephemeris (+ external IGS-SSR 4076 orbits) — the
        // self-sufficient real-time service mode (docs/rt_service_conventions.md
        // §1).  Parse-only in this card; the producer core path (nav load,
        // phase-gate suppression, anchor re-base, C0 delta-to-broadcast) lands in
        // later RTF-1 cards.  Mirrors the receiver.troposphere_apriori_source
        // string-match pattern (own local to_lower — the receiver-section lambda
        // is out of scope here).  Placed AFTER the estimate_iono_sh coupling above
        // so the brdc_rts validation sees final flag values.
        {
            auto to_lower = [](std::string s) {
                std::transform(s.begin(), s.end(), s.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return s;
            };
            const std::string src = to_lower(jval<std::string>(
                json, "network.apriori_source", "apriori_source", std::string{"sp3"}));
            if (src == "brdc_rts") {
                network_apriori_source_ = NetworkAprioriSource::BrdcRts;
            } else {
                if (src != "sp3")
                    log->warn("Unknown network.apriori_source='{}', falling back to 'sp3' "
                              "(precise SP3/CLK a-priori).", src);
                network_apriori_source_ = NetworkAprioriSource::Sp3;
            }
        }
        // D6: a-priori clock anchor σ (m); 0 = auto (mode-resolved at the
        // producer), an explicit positive value wins.  Parse+store only here.
        network_apriori_clock_anchor_sigma_m_ = jval<double>(
            json, "network.apriori_clock_anchor_sigma_m", "apriori_clock_anchor_sigma_m", 0.0);
        if (network_apriori_clock_anchor_sigma_m_ < 0.0) {
            log->warn("Network: network.apriori_clock_anchor_sigma_m {} < 0 is meaningless; "
                      "clamped to 0 (auto).", network_apriori_clock_anchor_sigma_m_);
            network_apriori_clock_anchor_sigma_m_ = 0.0;
        }

        if (network_apriori_source_ == NetworkAprioriSource::BrdcRts) {
            // Validation 1 (hard error): the satellite code-bias slice (W6) needs
            // an iono constraint that makes the satellite DCB observable.  In
            // brdc_rts that constraint MUST be the co-estimated SH-VTEC field —
            // the external GIM path is post-processing-only (a daily product; see
            // Phase 3b GIM/SH exclusivity in ApplicationNetwork + conventions §1).
            // estimate_iono_sh forces estimate_sat_code_bias on above, so the only
            // illegal combination is sat-code-bias ON with iono_sh OFF (the GIM
            // path).
            if (estimate_network_sat_code_bias_ && !estimate_network_iono_sh_) {
                log->error("Network: network.apriori_source = brdc_rts with "
                           "network.estimate_sat_code_bias but NOT network.estimate_iono_sh — "
                           "the external GIM iono constraint that makes the satellite code bias "
                           "observable is post-processing-only, so the real-time producer has no "
                           "iono constraint for the code-bias slice.  Set network.estimate_iono_sh "
                           "= true (co-estimate the SH-VTEC field) or drop "
                           "network.estimate_sat_code_bias.");
                return false;
            }
            // Validation 1b (hard error): EXT-CB consumes a daily `.BIA` bias
            // product, which the self-sufficient real-time service does not have —
            // the same post-processing-only reasoning that rejects the external-GIM
            // code-bias path here.  (A real-time producer takes its satellite biases
            // from an SSR stream, not a Bias-SINEX file; that is a different
            // mechanism with its own age/continuity semantics.)
            if (network_external_sat_code_bias_) {
                log->error("Network: network.apriori_source = brdc_rts with "
                           "network.external_sat_code_bias — the `.BIA` is a daily "
                           "post-processing bias product and is not available to the "
                           "self-sufficient real-time service (same reason the external-GIM "
                           "code-bias path is post-processing-only).  Use "
                           "network.estimate_sat_code_bias + network.estimate_iono_sh for a "
                           "real-time producer, or drop apriori_source = brdc_rts.");
                return false;
            }
            // Validation 2 (warn — no per-mount dialect declaration exists yet,
            // RTF-3): the producer's SSR ORBIT corrections must be IGS-SSR 4076
            // (CoM); legacy 1240-1270 (APC) mountpoints are user-engine-only
            // (RTB-5, honor_ssr_reference_point).  Dialect is decoded from the
            // message type at runtime, not declared in config, so this cannot be a
            // load-time hard error yet — downgrade to a warn per the plan.
            if (realtime_config_.corrections.enabled) {
                log->warn("Network: network.apriori_source = brdc_rts with a realtime "
                          "corrections stream configured — the producer's SSR ORBIT corrections "
                          "must be IGS-SSR 4076 (CoM); legacy 1240-1270 (APC) dialect is NOT "
                          "supported for the producer and is not validated at load (no per-mount "
                          "dialect declaration — RTF-3).  Ensure the corrections mountpoint "
                          "streams 4076.");
            }
            log->info("Network producer (RTF-1): a-priori source = broadcast + external RTS "
                      "orbits (brdc_rts) — the emitted clock C0 becomes a delta against "
                      "broadcast(IODE) + the applied RTS orbit; a-priori clock anchor σ = {} m "
                      "(0 = auto → broadcast grade).  NOTE: the producer core path (RINEX-nav "
                      "load, broadcast-phase-gate suppression, anchor re-base) lands in later "
                      "RTF-1 cards — this card is config plumbing only.",
                      network_apriori_clock_anchor_sigma_m_);
        }

        // --- GLONASS in the network (code-only: sat clock + per-FCN-slot rx IFB) ---
        // FDMA makes a single shared per-(sat,signal) satellite OSB unphysical, so
        // GLONASS contributes a per-satellite CLOCK only.  The receiver group delay
        // is carried by the per-FCN-slot receiver IFB as a LOCAL nuisance (Schur-
        // eliminated, never emitted), and NO GLONASS satellite phase bias is
        // produced.  Admitting GLONASS therefore REQUIRES the per-slot IFB
        // machinery: force its prerequisites on, and require GLONASS in
        // active_systems (otherwise GLONASS rows would build with no shared clock
        // column and pin the receiver clock, corrupting the GPS/Galileo products).
        enable_glonass_network_ = jval<bool>(json, "network.estimate_glonass",
                                             "estimate_glonass", false);
        if (enable_glonass_network_) {
            const bool has_glonass =
                std::find(active_systems_.begin(), active_systems_.end(), "GLONASS")
                != active_systems_.end();
            if (!has_glonass) {
                log->error("Network: network.estimate_glonass=true but 'GLONASS' is not in "
                           "processing.active_systems.  Add GLONASS to active_systems AND give it "
                           "a processing.signal_priorities entry, or set estimate_glonass=false.");
                return false;
            }
            // A GLONASS signal_priorities entry is the OTHER half of admitting it:
            // without one, PppModel::decideConstellationCanonicals finds no priority
            // string and skips every GLONASS satellite at signal selection, so the
            // network would silently emit ZERO GLONASS products (no clock, no per-slot
            // IFB ever allocated) while the GPS/Galileo solution looks nominal — the
            // documented multi-GNSS signal_priorities silent-drop failure mode.  Fail
            // loud here so the misconfig is caught at load, matching the error string
            // above that already instructs the user to add the entry.
            if (signal_priorities_.find("GLONASS") == signal_priorities_.end()) {
                log->error("Network: network.estimate_glonass=true but 'GLONASS' has no "
                           "processing.signal_priorities entry.  Without it every GLONASS "
                           "observation is dropped at signal selection (a silent zero-emit of "
                           "GLONASS products).  Add a GLONASS signal_priorities entry (e.g. "
                           "\"GLONASS\": \"CP\" for the FDMA G1/G2 bands), or set "
                           "estimate_glonass=false.");
                return false;
            }
            if (!uncombined_enabled_) {
                log->error("Network: network.estimate_glonass=true requires uncombined PPP-RTK "
                           "(the per-FCN-slot IFB is an uncombined receiver-bias state).");
                return false;
            }
            estimate_ifb_       = true;   // enables the receiver-bias allocation loop
            enable_glonass_ifb_ = true;   // enables the GLONASS per-FCN-slot resolver branch
            log->info("Network producer: GLONASS ENABLED (code-only) — forcing estimate_ifb + "
                      "enable_glonass_ifb = true; emitting a GLONASS satellite clock with the "
                      "per-FCN-slot receiver IFB as a local nuisance (no GLONASS satellite phase bias).");
        }

        // GLONASS + satellite code-bias are not jointly supported in V1: admitting
        // GLONASS forces the receiver-bias allocation to skip GPS/Galileo IFB (only
        // the GLONASS per-slot IFB is created — see MeasurementModel's network alloc
        // gate), but the satellite code-bias separation REQUIRES an explicit GPS/
        // Galileo receiver DCB on the undifferenced rows.  Fail loud rather than
        // silently produce an unidentifiable system.
        if (estimate_network_sat_code_bias_ && enable_glonass_network_) {
            log->error("Network: network.estimate_sat_code_bias and network.estimate_glonass cannot "
                       "both be on in V1 — GLONASS admission skips the GPS/Galileo receiver DCB that "
                       "the satellite code-bias estimation needs.  Disable one of them.");
            return false;
        }

        // --- POST-RESOLUTION trace-inertness warnings -------------------------
        // Both of these judge whether an armed trace has any state to write, and
        // both USED to sit beside their own parse in loadNetworkObservabilityAndSnapshot_
        // — which runs two helpers EARLIER, before anything they test is resolved.
        // The predicates were therefore evaluated against header defaults:
        //
        //   trace_sat_bias_states: `!estimate_network_sat_code_bias_ &&
        //   !network_external_sat_code_bias_` read two members that are BOTH parsed
        //   in loadNetworkSharedStateForcings_, so it was `!false && !false` — TRUE
        //   ALWAYS.  The warning fired on every run that armed the trace, including
        //   the W6 and EXT-CB runs where the trace is fully live.  That is standing
        //   invariant #4 in reverse: a live instrument reporting itself inert, which
        //   trains the reader to ignore the one line that would matter.
        //
        //   trace_rx_states: `!estimate_ifb_` was read before ALL THREE sites that
        //   force it on — the W6 slice and the SH slice in
        //   loadNetworkSharedStateForcings_, and the GLONASS slice in this helper.
        //
        // This is the tail of the LAST helper that touches any of them, so every
        // value below is final.  A new forcing added after this point must move
        // these with it.
        if (network_config_.trace_sat_bias_states
            && !estimate_network_sat_code_bias_ && !network_external_sat_code_bias_)
            log->warn("Network: network.trace_sat_bias_states is INERT in this posture — "
                      "with neither estimate_sat_code_bias (W6) nor external_sat_code_bias "
                      "there are no satellite code-bias columns and no B records, so the "
                      "trace opens and stays empty.  Standing invariant #4: an armed-but-"
                      "inert instrument reads exactly like a clean one.");
        if (network_config_.trace_rx_states && !estimate_ifb_)
            log->warn("Network: network.trace_rx_states will emit CLOCK rows only — "
                      "estimation.estimate_ifb is off, so there is no explicit receiver "
                      "code-bias state (the receiver code delay rides inside the slant "
                      "ionosphere instead).  The W6, EXT-CB and GLONASS slices each force "
                      "estimate_ifb on; none of them is armed here.");

        return true;
    }

    // =================================================================
    //  network subsection helper: loadNetworkCarries_
    // =================================================================
    /**
     * @brief Cross-epoch carries: phase leveling, the shared-border carry and its couplings.
     *
     * @c network.phase_leveling*, @c network.shared_carry_*, the Card-1 conditional-coupling
     * family, @c network.carry_zwd / @c carry_rx_code_bias / @c carry_vce, and the
     * station-adaptive SH-VTEC constraint deweight.  Prerequisite violations here THROW (the
     * coupling cards are exact only against a dense shared-border carry).
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkCarries_(const nlohmann::json& json,
                                         const std::shared_ptr<spdlog::logger>& log) {
        // --- Phase-leveling: carry per-station ambiguities across epochs ---------
        // Move the producer from a per-epoch ABSOLUTE snapshot solve to one that
        // carries each non-pivot station's float ambiguities as a diagonal prior on
        // its own local block, so carrier-phase precision (not just code) sharpens
        // the satellite clock/phase-bias/code-bias products.  Additive and purely
        // local: OFF is byte-identical to today (the bisection anchor for the whole
        // feature).  q_amb / warmup / max_gap are heuristics flagged for tuning.
        network_phase_leveling_ =
            jval<bool>(json, "network.phase_leveling", "phase_leveling", false);
        network_phase_leveling_q_amb_ =
            jval<double>(json, "network.phase_leveling_q_amb", "phase_leveling_q_amb",
                         network_config_.process_noise.get(NetworkQClass::LevelingAmbiguity));
        network_phase_leveling_warmup_epochs_ =
            jval<int>(json, "network.phase_leveling_warmup_epochs", "phase_leveling_warmup_epochs", 6);
        network_phase_leveling_max_gap_s_ =
            jval<double>(json, "network.phase_leveling_max_gap_s", "phase_leveling_max_gap_s", 120.0);
        network_phase_leveling_var_floor_m2_ =
            jval<double>(json, "network.phase_leveling_var_floor_m2", "phase_leveling_var_floor_m2", 0.0);
        // F1 (SPEC_amb_iono_split_overcount): per-LOS B_eff scale of the carried
        // ambiguity row variance at inject.  Default OFF = byte-identical.
        network_phase_leveling_los_scale_ =
            jval<bool>(json, "network.phase_leveling_los_scale", "phase_leveling_los_scale", false);
        // F3 (SPEC_amb_iono_split_overcount): station-wide jointly-whitened local block
        // on the carried ambiguity inject.  Default OFF = byte-identical.
        network_phase_leveling_local_block_ =
            jval<bool>(json, "network.phase_leveling_local_block", "phase_leveling_local_block", false);
        // F3 block ADMISSION age gate (SPEC_amb_iono_split_overcount §6.12): legs younger than
        // this many loop epochs are injected per-column, not in the block.  0 = OFF.
        network_phase_leveling_local_block_min_age_ep_ =
            jval<int>(json, "network.phase_leveling_local_block_min_age_ep",
                      "phase_leveling_local_block_min_age_ep", 0);
        if (network_phase_leveling_local_block_min_age_ep_ < 0)
            throw std::runtime_error("network.phase_leveling_local_block_min_age_ep must be >= 0");
        network_phase_leveling_local_block_young_mode_ =
            jval<std::string>(json, "network.phase_leveling_local_block_young_mode",
                              "phase_leveling_local_block_young_mode", "free");
        if (network_phase_leveling_local_block_young_mode_ != "free"
            && network_phase_leveling_local_block_young_mode_ != "diagonal")
            throw std::runtime_error("network.phase_leveling_local_block_young_mode must be "
                                     "\"free\" or \"diagonal\"");
        if (network_phase_leveling_local_block_min_age_ep_ > 0
            && network_phase_leveling_local_block_young_mode_ == "diagonal")
            log->warn("Network (F3 age gate): young_mode \"diagonal\" is MEASURED HARMFUL (runs/"
                      "readmit8h_young: every hour back at the pre-F1 over-count — any diagonal "
                      "leveling row re-asserts the station-common directions ~n*B-fold); armed "
                      "for the record only.");
        if (network_phase_leveling_local_block_min_age_ep_ > 0 && !network_phase_leveling_local_block_)
            log->warn("Network (F3 age gate): network.phase_leveling_local_block_min_age_ep is set "
                      "but network.phase_leveling_local_block is OFF — there is no block to keep a "
                      "young leg out of, so the gate is INERT this run.");
        // AGE-AWARE young-arc variance inflation (SPEC_amb_iono_split_overcount
        // §6.17–§6.18): a column whose LAST RE-LEVEL is recent carries a CODE-quality
        // level at a PHASE-stiff harvested variance, so the T4 tier erases it and the
        // erase–re-level treadmill sustains the ZWD/clock plateau.  V0 = 0 is OFF =
        // byte-identical; the two shape knobs keep their defaults on an OFF run and are
        // validated here anyway, so an armed run can never reach the inject with a
        // degenerate exponential.
        network_phase_leveling_young_inflate_var_m2_ =
            jval<double>(json, "network.phase_leveling_young_inflate_var_m2",
                         "phase_leveling_young_inflate_var_m2", 0.0);
        network_phase_leveling_young_inflate_tau_ep_ =
            jval<double>(json, "network.phase_leveling_young_inflate_tau_ep",
                         "phase_leveling_young_inflate_tau_ep", 20.0);
        network_phase_leveling_young_inflate_max_age_ep_ =
            jval<int>(json, "network.phase_leveling_young_inflate_max_age_ep",
                      "phase_leveling_young_inflate_max_age_ep", 60);
        if (network_phase_leveling_young_inflate_var_m2_ < 0.0)
            throw std::runtime_error("network.phase_leveling_young_inflate_var_m2 must be >= 0 "
                                     "(0 = off)");
        if (network_phase_leveling_young_inflate_tau_ep_ <= 0.0)
            throw std::runtime_error("network.phase_leveling_young_inflate_tau_ep must be > 0 "
                                     "(a non-positive e-folding age turns the decay into a step)");
        if (network_phase_leveling_young_inflate_max_age_ep_ <= 0)
            throw std::runtime_error("network.phase_leveling_young_inflate_max_age_ep must be > 0 "
                                     "(0 would arm a knob that can never inflate anything)");
        // ---- RF-1 (docs/refound/RF-1_float_ambiguities_design.md §7.1): the AMBIGUITY
        //      VEHICLE and its supporting knobs.
        //
        // String enum on the established loader pattern (network.trace_dd_fractional_ref,
        // network.raim_erase_band_scope): an unknown value is an ERROR naming what the
        // knob changes, and falls back to the default "level" rather than killing the
        // run.  ⚠ STEP 1: the vehicle is REGISTERED and carries NO mechanism — "level"
        // and "float_kf" are byte-identical in this build (standing invariant #6).  The
        // cross-section interlocks (float_kf requires phase_leveling and !carry_invariant;
        // F1/F3 are refused under it) live in validateNetworkCarryPosture_, which is the
        // first point where network_carry_invariant_ is populated.
        {
            const std::string s = jval<std::string>(json, "network.ambiguity_vehicle",
                                                    "ambiguity_vehicle",
                                                    std::string{"level"});
            if (s.empty() || s == "level")
                network_config_.ambiguity_vehicle = NetworkAmbiguityVehicle::Level;
            else if (s == "float_kf")
                network_config_.ambiguity_vehicle = NetworkAmbiguityVehicle::FloatKf;
            else {
                network_config_.ambiguity_vehicle = NetworkAmbiguityVehicle::Level;
                log->error("Network (RF-1 §7.1): network.ambiguity_vehicle = '{}' is not one of "
                           "level|float_kf — falling back to 'level'.  The vehicle decides HOW a "
                           "station ambiguity is carried between epochs: 'level' is today's "
                           "zero-process-noise scalar prior carrying last epoch's marginal "
                           "variance, 'float_kf' the float state whose carried JOINT covariance "
                           "is folded as a proper prior with a floored birth prior and a bounded "
                           "DROP as the slip response.  A typo here would silently ship the "
                           "wrong vehicle in the product's provenance.", s);
            }
        }
        network_config_.amb_float_birth_var_m2 =
            jval<double>(json, "network.amb_float_birth_var_m2", "amb_float_birth_var_m2", 0.0);
        if (network_config_.amb_float_birth_var_m2 < 0.0)
            throw std::runtime_error("network.amb_float_birth_var_m2 must be >= 0 "
                                     "(0 = use the arc's own first-stash variance unfloored)");
        // RF-3a (docs/refound/RF-3_code_row_qc_2026-09-22.md §4 A1): the DROP-ONLY floor.
        // Separate from the birth floor because the RF-3 read measured that the defect is
        // the re-coupling edge of the slip and gap re-entry response, not the birth: a
        // birth floor removes the post-drop walk AND slows every birth (hour 1 0.56 ->
        // 0.66 m at WTZZ00DEU, census hour 1 +0.03 to +0.04 m), while the drop is where
        // the stale stiff variance is handed back.
        network_config_.amb_float_drop_var_m2 =
            jval<double>(json, "network.amb_float_drop_var_m2", "amb_float_drop_var_m2", 0.0);
        if (network_config_.amb_float_drop_var_m2 < 0.0)
            throw std::runtime_error("network.amb_float_drop_var_m2 must be >= 0 "
                                     "(0 = a drop restores max(var_birth, var_first) "
                                     "unfloored, which is today's behaviour)");
        if (network_config_.amb_float_drop_var_m2 > 0.0
            && network_config_.ambiguity_vehicle != NetworkAmbiguityVehicle::FloatKf)
            log->warn("Network (RF-3a): network.amb_float_drop_var_m2 = {:g} m^2 is set but "
                      "network.ambiguity_vehicle is not 'float_kf' — the bounded DROP is the "
                      "float vehicle's response, so the floor is INERT on this posture.  "
                      "Read this as a miswired arm, not as a null result.",
                      network_config_.amb_float_drop_var_m2);
        else if (network_config_.amb_float_drop_var_m2 > 0.0)
            log->info("Network (RF-3a drop-only floor): ON — every DROP that restores the "
                      "birth prior restores max(var_birth, var_first, {:g}, {:g}) m^2, "
                      "elevation-mapped at the code weights' power.  The BIRTH stash is "
                      "untouched.  SOLVE-AFFECTING.  ⚠ MEASURED: the birth stash being "
                      "untouched does NOT leave hour 1 untouched — drops happen during "
                      "convergence too, and the floor cost 0.09 to 0.16 m of hour 1 at the "
                      "held-out consumer on the 6 h field bed.  See "
                      "network.amb_float_drop_floor_after_ep.",
                      network_config_.amb_float_birth_var_m2,
                      network_config_.amb_float_drop_var_m2);
        // RF-3c: the WARM-UP GATE on the drop floor.  Separates the convergence
        // population (a drop while the network is still finding its clocks) from the
        // settled one the floor is a statement about.
        network_config_.amb_float_drop_floor_after_ep =
            jval<int>(json, "network.amb_float_drop_floor_after_ep",
                      "amb_float_drop_floor_after_ep", 0);
        if (network_config_.amb_float_drop_floor_after_ep < 0) {
            log->warn("Network (RF-3c): network.amb_float_drop_floor_after_ep = {} is "
                      "negative — clamped to 0 (no gate, RF-3a's behaviour).",
                      network_config_.amb_float_drop_floor_after_ep);
            network_config_.amb_float_drop_floor_after_ep = 0;
        }
        if (network_config_.amb_float_drop_floor_after_ep > 0
            && !(network_config_.amb_float_drop_var_m2 > 0.0))
            log->warn("Network (RF-3c): network.amb_float_drop_floor_after_ep = {} is set "
                      "while network.amb_float_drop_var_m2 is 0 — the gate guards a floor "
                      "that is not armed, so it is INERT.  Read this as a miswired arm, "
                      "not as a null result.",
                      network_config_.amb_float_drop_floor_after_ep);
        else if (network_config_.amb_float_drop_floor_after_ep > 0)
            log->info("Network (RF-3c drop-floor warm-up gate): the {:g} m^2 drop floor "
                      "applies only to drops taken at processed epoch >= {}; before that a "
                      "drop restores max(var_birth, var_first, {:g}) — today's RF-3a-free "
                      "quantity.  Both populations are counted at run end.",
                      network_config_.amb_float_drop_var_m2,
                      network_config_.amb_float_drop_floor_after_ep,
                      network_config_.amb_float_birth_var_m2);
        network_config_.amb_float_q_m2s =
            jval<double>(json, "network.amb_float_q_m2s", "amb_float_q_m2s", 0.0);
        if (network_config_.amb_float_q_m2s < 0.0)
            throw std::runtime_error("network.amb_float_q_m2s must be >= 0 (the card's posture "
                                     "is 0.0 — an ambiguity is a random constant; any nonzero "
                                     "value is a separate, evidence-bearing arm)");
        network_config_.amb_float_declared_handover =
            jval<bool>(json, "network.amb_float_declared_handover",
                       "amb_float_declared_handover", false);
        network_config_.trace_amb_state =
            jval<bool>(json, "network.trace_amb_state", "trace_amb_state", false);
        network_config_.trace_amb_state_every =
            jval<int>(json, "network.trace_amb_state_every", "trace_amb_state_every", 1);
        if (network_config_.trace_amb_state_every < 1)
            network_config_.trace_amb_state_every = 1;
        network_shared_carry_var_floor_pb_m2_ =
            jval<double>(json, "network.shared_carry_var_floor_pb_m2", "shared_carry_var_floor_pb_m2", 0.0);
        // T1d: convergence-gated leveling-carry warmup (default OFF = fixed epoch count).
        network_phase_leveling_warmup_on_convergence_ =
            jval<bool>(json, "network.phase_leveling_warmup_on_convergence",
                       "phase_leveling_warmup_on_convergence", false);
        network_phase_leveling_refloat_at_convergence_ =
            jval<bool>(json, "network.phase_leveling_refloat_at_convergence",
                       "phase_leveling_refloat_at_convergence", false);
        network_phase_leveling_conv_rms_thresh_tecu_ =
            jval<double>(json, "network.phase_leveling_conv_rms_thresh_tecu",
                         "phase_leveling_conv_rms_thresh_tecu", 0.5);
        network_shared_carry_var_floor_cb_m2_ =
            jval<double>(json, "network.shared_carry_var_floor_cb_m2", "shared_carry_var_floor_cb_m2", 0.0);
        // C1: full dense cross-covariance carry of the shared border (information-form
        // KF on the border; recovers the clock↔phase-bias off-diagonals the diagonal
        // D2 stash discards).  Independent of phase_leveling — it acts on the SHARED
        // D2 carry, not the per-station ambiguity carry — so parsed and announced here.
        network_shared_carry_full_cov_ =
            jval<bool>(json, "network.shared_carry_full_cov", "shared_carry_full_cov", false);
        if (network_shared_carry_full_cov_)
            log->info("Network producer (C1): FULL-COVARIANCE shared carry enabled — the D2 "
                      "cross-epoch prior folds the dense (P_prev + Q)⁻¹ block over the carried "
                      "clock/phase-bias/code-bias columns (information-form KF on the shared "
                      "border) instead of independent per-key diagonals.  Departed/pinned/"
                      "retired keys are marginalized; a non-PD carried block degrades to the "
                      "diagonal fold for that epoch (warned).");
        // Card C-GF: the gauge-free carry fold, armed as a property of the CARRY.  The
        // downdate Λ ← Λ − ΛU(UᵀΛU)⁻¹UᵀΛ removes the DECLARED datum directions from the
        // carried information, so the carry constrains only estimable combinations and
        // leaves the gauge to the S-basis.  It existed already but was reachable ONLY via
        // network.clock_anchor_gauge_free — a Q3 temporal-anchor knob with no logical
        // relation to it — so a config running the dense carry without that anchor could
        // not have it, even though the carry is what deposits gauge information.  Also
        // generalizes U: declared directions with a SUPPORT-CLOSURE test rather than the
        // carried clocks with a bare ">= 2 members" floor.
        network_shared_carry_gauge_free_ =
            jval<bool>(json, "network.shared_carry_gauge_free", "shared_carry_gauge_free", false);
        if (network_shared_carry_gauge_free_ && !network_shared_carry_full_cov_)
            throw std::runtime_error(
                "network.shared_carry_gauge_free requires network.shared_carry_full_cov = true "
                "(it marginalizes the datum directions out of THAT card's dense carried "
                "information block).");
        if (network_shared_carry_gauge_free_)
            log->info("Network producer (Card C-GF): GAUGE-FREE shared carry enabled — the "
                      "declared datum directions are marginalized out of the carried "
                      "information (exact information-side downdate), with a support-closure "
                      "test so a direction whose support is only PARTLY carried is SKIPPED "
                      "rather than having its carried-vs-uncarried contrast (an ESTIMABLE "
                      "quantity) deleted.  Fold, withdraw and the within-epoch RAIM replay "
                      "all read one resolved predicate, so they cannot disagree.");
        // NOTE: `network_clock_anchor_gauge_free_` is parsed much later in this function,
        // so it deliberately does NOT appear in this predicate — reading it here would
        // read its default, not the user's value.  The advisory is therefore phrased to
        // cover the case where the Q3 path supplies the downdate instead.
        if (network_shared_carry_full_cov_ && network_shared_carry_gauge_free_)
            log->info("Network producer (Card C-GF): shared_carry_gauge_free is armed, but the "
                      "first-variation A/B (SPEC_card1_coupling_carry §13.10, 2026-08-17) "
                      "measured it INERT-BY-CLOSURE on every roster tested: the support-closure "
                      "test correctly declines the declared direction essentially every epoch "
                      "(a carried clock set almost never spans the full registered "
                      "constellation), all four arms were byte-identical to their parents, and "
                      "arming it splits the warm-resume fingerprint (cta:gf) for nothing.  "
                      "Ledger verdict KEEP_OFF.  The carry-fills-gauge defect it targeted is "
                      "handled by network.carry_coupling_gauge_project (Card 1f, RECOMMEND_ON); "
                      "the gauge-free successor is a derivation card over the CARRIED subset, "
                      "not this knob.");
        // Card 1: rigorous conditional local carry.  The persistent per-station locals
        // (phase-leveling ambiguities + ZWD) are carried as ONE pseudo-observation row
        // coupling each to the FREE shared border (regression M_r, pure conditional
        // variance (N_rr⁻¹)_{rr}) instead of an independent diagonal prior — this removes
        // the shared-information double count the diagonal carry incurs against the dense
        // border carry, so the sequential recursion equals the batch solve.  It is only
        // EXACT against a dense border carry, so hard-require shared_carry_full_cov.
        network_carry_conditional_coupling_ =
            jval<bool>(json, "network.carry_conditional_coupling", "carry_conditional_coupling", false);
        if (network_carry_conditional_coupling_ && !network_shared_carry_full_cov_)
            throw std::runtime_error(
                "network.carry_conditional_coupling requires network.shared_carry_full_cov = true "
                "(the conditional local carry is exact only against a dense shared-border carry).");
        if (network_carry_conditional_coupling_)
            log->info("Network producer (Card 1): CONDITIONAL local carry enabled — each carried "
                      "phase-leveling ambiguity / ZWD is injected as a pseudo-observation row "
                      "coupling it to the free shared border (coeff +1 on the local, regression "
                      "M_r on the border, variance = pure conditional (N_rr⁻¹)_rr), not an "
                      "independent diagonal prior.  Departed shared columns are marginalized into "
                      "the row variance.  Removes the shared-info double count → the recursion "
                      "equals the batch solve.");
        // Card 1b: JOINTLY whiten a station's coupling rows instead of marginalizing each
        // departed shared column per-row into a scalar variance — restores the between-row
        // (DD) correlation the diagonal marginalization discards (the T7 flat-σ bug).
        network_carry_coupling_joint_whiten_ =
            jval<bool>(json, "network.carry_coupling_joint_whiten",
                       "carry_coupling_joint_whiten", false);
        if (network_carry_coupling_joint_whiten_ && !network_carry_conditional_coupling_)
            throw std::runtime_error(
                "network.carry_coupling_joint_whiten requires network.carry_conditional_coupling = true "
                "(it only changes how that card's coupling-row noise is applied).");
        if (network_carry_coupling_joint_whiten_)
            log->info("Network producer (Card 1b): JOINT coupling-row whitening enabled — a "
                      "station's conditional coupling rows are built as a group with the full "
                      "joint covariance R = diag(var_eff) + M_w·diag(P_w)·M_wᵀ over the distinct "
                      "DEPARTED shared columns and LLᵀ-whitened, so their shared common-mode "
                      "(which cancels in any DD) is common-mode-free and the between-row DD "
                      "information is preserved instead of individually deweighted to nothing.");
        // Card 1d: border TIME UPDATE of the coupling rows at injection.  A harvested row
        // is exact at the harvest epoch only; the border then moves s⁺ = s + w, w~N(0,Q),
        // so re-independencing the row against s⁺ needs the per-column κ shrink
        // M' = (P+Q)⁻¹P·M and the MᵀQM-style variance inflation.  Card 1 applies neither
        // to layout-present columns — the un-handled middle case between "static border"
        // and its departed-column (q→∞) marginalization.
        network_carry_coupling_time_update_ =
            jval<bool>(json, "network.carry_coupling_time_update",
                       "carry_coupling_time_update", false);
        if (network_carry_coupling_time_update_ && !network_carry_conditional_coupling_)
            throw std::runtime_error(
                "network.carry_coupling_time_update requires network.carry_conditional_coupling = true "
                "(it only corrects how that card's coupling rows re-map to the moved border).");
        // Card 1d-SH: fully marginalize the coupling rows' SH-VTEC terms.  The SH border
        // is a mean-reverting GM (climatology-anchored), not the pure RW the κ update
        // models — shrink-carrying its terms compounds row weight against the anchor
        // (the ~4 h a₀₀ runaway in the 12 h low-floor runs).  q=∞/departed treatment
        // severs that feedback; conservative and always consistent.
        network_carry_coupling_sh_marginalize_ =
            jval<bool>(json, "network.carry_coupling_sh_marginalize",
                       "carry_coupling_sh_marginalize", false);
        if (network_carry_coupling_sh_marginalize_ && !network_carry_conditional_coupling_)
            throw std::runtime_error(
                "network.carry_coupling_sh_marginalize requires network.carry_conditional_coupling "
                "= true (it only changes how that card's SH coupling terms are treated).");
        if (network_carry_coupling_sh_marginalize_)
            log->info("Network producer (Card 1d-SH): SH-VTEC coupling terms fully MARGINALIZED "
                      "(q=∞ / departed treatment: coefficient dropped, M²·P_prev into the row "
                      "variance) — severs the ambiguity↔SH carry feedback (the GM-carried border "
                      "violates the κ update's random-walk model; shrink-carrying its terms "
                      "compounds row weight against the climatology anchor).");
        if (network_carry_coupling_time_update_)
            log->info("Network producer (Card 1d): coupling-row border TIME UPDATE enabled — "
                      "each PRESENT coupled column of every injected coupling row is shrunk by "
                      "κ_j = P_prev/(P_prev + q_j·Δt) (coefficient + RHS term) and the row "
                      "variance inflated by M²·κ_j·q_j·Δt, with the per-kind q ladder exactly "
                      "the dense border fold's (clock-monitor-reinit sats treated as q=∞ ⇒ full "
                      "marginalization).  Removes the stale-row over-assertion (the carried-DD "
                      "random-walk / flat-σ defect, Card 1c).");
        // Card 1f: GAUGE PROJECTION of a TRUNCATED coupling row.  A harvested row is
        // gauge-invariant by the identity Mᵀg_s = −g_r(c) — it observes the datum gauge
        // exactly as much as its own A_r = +1 local requires.  Deleting a DEPARTED
        // column's coefficient breaks that: the family sum moves to −M_dep and the row
        // becomes a high-weight observation of the CURRENT gauge pinned at the OLD
        // realization.  Measured (simnet24, TOW 305220, 7 G-clock departures): smallest
        // null eigenvalue 1e-11 → 9.43e-3 in ONE epoch — five decades over the gate
        // cutoff, level with the weakest-real PB layer — ⇒ 4 spurious OVER-specified
        // withholds, self-healing only at pl_max_gap_s by blanket carry expiry.  The fix
        // holds each (constellation,kind) coefficient sum at its HARVEST value, which
        // subsumes Card 1d's κ re-projection and reduces to it exactly when nothing
        // truncated.  NOT "annihilate the sum": the harvest target is −g_r(c), and a
        // Galileo ambiguity row measures Σ_E = −1 exactly.
        network_carry_coupling_gauge_project_ =
            jval<bool>(json, "network.carry_coupling_gauge_project",
                       "carry_coupling_gauge_project", false);
        if (network_carry_coupling_gauge_project_ && !network_carry_conditional_coupling_)
            throw std::runtime_error(
                "network.carry_coupling_gauge_project requires network.carry_conditional_coupling "
                "= true (it only changes how that card's TRUNCATED coupling rows treat the datum "
                "gauge).");
        if (network_carry_coupling_gauge_project_)
            log->info("Network producer (Card 1f): coupling-row DATUM-GAUGE PROJECTION enabled — "
                      "each (constellation,kind) coefficient sum of a TRUNCATED row is held at "
                      "its HARVEST value instead of at the surviving-present value, so a departed "
                      "or re-realized column can no longer turn the row into a high-weight "
                      "observation of the current datum gauge pinned at the old realization.  "
                      "Run-end counters: gauge-held / gauge-orphan families (invariant #4 — both "
                      "zero means no row was ever truncated, i.e. INERT, not clean).");
        // Card 1g: feed the coupling reinit history from every REALIZATION-STEP source
        // (phase-anchor re-reference, eq34 chain re-reference, datum-slip PB
        // re-realization) and honour it with the κ time update OFF.  Each of those
        // erases the re-referenced family's OWN carry but leaves every OTHER carried
        // local's coupling[] entry pointing at that column with a RETIRED-frame ŝ_prev,
        // which buildCplRow then folds at full weight — the stale-ŝ_prev wire-poison
        // channel (30 PBGAUGE + 27 CLK_STEP re-realizations in 120 epochs).  A
        // re-realized column's step is UNKNOWN and cannot be transformed, so the only
        // correct treatment is the departed route (κ=0); Card 1f then repairs the gauge
        // deficit that marginalization opens.
        network_carry_coupling_reinit_sources_ =
            jval<bool>(json, "network.carry_coupling_reinit_sources",
                       "carry_coupling_reinit_sources", false);
        if (network_carry_coupling_reinit_sources_ && !network_carry_conditional_coupling_)
            throw std::runtime_error(
                "network.carry_coupling_reinit_sources requires network.carry_conditional_coupling "
                "= true (it only changes which of that card's coupling terms take the departed "
                "route).");
        if (network_carry_coupling_reinit_sources_ && !network_carry_coupling_gauge_project_)
            log->warn("Network producer (Card 1g): carry_coupling_reinit_sources is armed WITHOUT "
                      "network.carry_coupling_gauge_project.  Routing a re-realized column to the "
                      "departed route removes its stale frame step but OPENS the same "
                      "family-sum deficit a departure opens — the row then observes the datum "
                      "gauge.  The two cards are designed to be armed together.");
        if (network_carry_coupling_reinit_sources_)
            log->info("Network producer (Card 1g): coupling REINIT sources widened — the reinit "
                      "history is now keyed (kind,sat,signal) with an empty signal as the "
                      "sat-wide wildcard, fed by the phase-anchor / eq34 / datum-slip "
                      "re-reference sets as well as the clock-QC sources, and honoured "
                      "independently of network.carry_coupling_time_update.");
        // ZWD random-walk carry (B9): carry per-station tropospheric ZWD across epochs as a
        // diagonal RW prior via the same addStationPrior path as the ambiguity carry.
        network_carry_zwd_ =
            jval<bool>(json, "network.carry_zwd", "carry_zwd", false);
        network_zwd_carry_q_ =
            jval<double>(json, "network.zwd_carry_q", "zwd_carry_q",
                         network_config_.process_noise.get(NetworkQClass::StationZwd));
        // B9-in-block: fold that same carried ZWD as one more column of the station's F3
        // whitened block instead of as a diagonal row outside it (the §7 item 17 "block ALL
        // carried local columns" rule — a diagonal ZWD row double-counts the station-common
        // direction the ambiguity rows already assert).  Validated below.
        network_carry_zwd_in_block_ =
            jval<bool>(json, "network.carry_zwd_in_block", "carry_zwd_in_block", false);
        // I-in-block: the same rule applied to the per-LOS slant-iono columns (SPEC_amb_
        // iono_split_overcount §7 item 17).  Deliberately NOT a sub-knob of the legacy
        // network.carry_iono — that one is the DIAGONAL vehicle this replaces, and the
        // two are mutually exclusive (validated below).  q is PER EPOCH, m² at L1.
        network_carry_iono_in_block_ =
            jval<bool>(json, "network.carry_iono_in_block", "carry_iono_in_block", false);
        network_carry_iono_in_block_q_m2_ =
            jval<double>(json, "network.carry_iono_in_block_q_m2",
                         "carry_iono_in_block_q_m2", 2.5e-3);
        // 0 (the default) is the "= the leveling max gap" sentinel; resolved here so the
        // getter always reports effective seconds and no consumer has to re-derive it.
        network_carry_iono_in_block_max_gap_s_ =
            jval<double>(json, "network.carry_iono_in_block_max_gap_s",
                         "carry_iono_in_block_max_gap_s", 0.0);
        // RX-CLK-in-block: the same "block ALL carried local columns" rule applied to the
        // station's RECEIVER CLOCK (and, under the sub-knob, its ISB columns).  The clock
        // is a LOCAL A_r column with NO temporal prior at all on this path
        // (filter.process_noise.clock_bias_q is inert here — measured byte-identical), so
        // it is the free per-epoch sink the ZWD and iono cards push their residual
        // rho-direction error into.  q is PER EPOCH, m^2; the drift is a carried state
        // with its own variance ladder (see the getters).  Validated below.
        network_carry_rx_clock_in_block_ =
            jval<bool>(json, "network.carry_rx_clock_in_block", "carry_rx_clock_in_block",
                       false);
        network_carry_rx_clock_q_m2_ =
            jval<double>(json, "network.carry_rx_clock_q_m2", "carry_rx_clock_q_m2", 0.09);
        // 0 (the default) is the "= the leveling max gap" sentinel; resolved below so the
        // getter always reports effective seconds and no consumer has to re-derive it.
        network_carry_rx_clock_max_gap_s_ =
            jval<double>(json, "network.carry_rx_clock_max_gap_s",
                         "carry_rx_clock_max_gap_s", 0.0);
        network_carry_rx_clock_rate_var0_ =
            jval<double>(json, "network.carry_rx_clock_rate_var0_m2ps2",
                         "carry_rx_clock_rate_var0_m2ps2", 1.0);
        network_carry_rx_clock_rate_q_ =
            jval<double>(json, "network.carry_rx_clock_rate_q_m2ps2",
                         "carry_rx_clock_rate_q_m2ps2", 1e-10);
        network_carry_rx_clock_jump_k_ =
            jval<double>(json, "network.carry_rx_clock_jump_k", "carry_rx_clock_jump_k",
                         10.0);
        network_carry_rx_clock_isb_ =
            jval<bool>(json, "network.carry_rx_clock_isb", "carry_rx_clock_isb", true);
        // Seam-scoped ZWD re-seed (SPEC_synthetic_minimum_network §17, carry_zwd
        // re-base arms finding 3): the resume-path-only successor to the continuous
        // B9 carry.  Injection window + horizon; the WRITE side (Z rows in every
        // snapshot) is unconditional and knob-free.
        network_zwd_seam_reseed_ =
            jval<bool>(json, "network.zwd_seam_reseed", "zwd_seam_reseed", false);
        network_zwd_seam_reseed_epochs_ =
            jval<int>(json, "network.zwd_seam_reseed_epochs", "zwd_seam_reseed_epochs", 480);
        // RS-10b (docs/SPEC_event_harness.md §21.21): the seam window's OWN stiffness.
        // network.zwd_carry_q is IN the warm-resume fingerprint (a continuous carry q
        // must cold-start), while the seam response that borrowed its weight
        // deliberately is not — so the one knob this response exists to tune could not
        // be varied on a resume leg at all.  Negative (the -1 default) = borrow
        // zwd_carry_q, i.e. byte-identical when unset; positive = used instead, and
        // ONLY inside the live seam window.  Not fingerprinted, same exemption class
        // as zwd_seam_reseed itself.
        network_zwd_seam_reseed_q_ =
            jval<double>(json, "network.zwd_seam_reseed_q", "zwd_seam_reseed_q", -1.0);
        if (std::isnan(network_zwd_seam_reseed_q_))
            throw std::runtime_error("network.zwd_seam_reseed_q must be a number "
                                     "(> 0 to arm, negative to borrow network.zwd_carry_q)");
        if (network_zwd_seam_reseed_q_ == 0.0)
            throw std::runtime_error("network.zwd_seam_reseed_q must be > 0 when set (0 arms a "
                                     "window whose variance never inflates while reading as "
                                     "armed); use a negative value (the -1 default) to borrow "
                                     "network.zwd_carry_q");
        network_amb_seam_reinflate_var_m2_ =
            jval<double>(json, "network.amb_seam_reinflate_var_m2",
                         "amb_seam_reinflate_var_m2", 0.0);
        // Resume FRAME restore + post-resume slip warm-up (SPEC_synthetic_minimum_
        // network §18.10).  Both are forward-only RESUME response policies (not
        // fingerprinted); both are inert on a run that never resumes.
        network_resume_frame_restore_ =
            jval<bool>(json, "network.resume_frame_restore", "resume_frame_restore", false);
        network_resume_slip_warmup_ep_ =
            jval<int>(json, "network.resume_slip_warmup_ep", "resume_slip_warmup_ep", 0);
        if (network_resume_slip_warmup_ep_ < 0)
            throw std::runtime_error("network.resume_slip_warmup_ep must be >= 0");
        // RS-4 (§21.9): the DECLARATION half of the same seam.  The CLK-R resume_seam
        // mark is suppressed exactly when the frame restore works (the held-frame engine
        // comes back with its epochs), so a lineage whose clock is continuous and whose
        // phase-bias level is not emits datum_changed 0.  Same exemption class as the
        // two knobs above: forward-only resume response policy, not fingerprinted, and
        // structurally unreachable on a run that never resumes.
        network_resume_pb_seam_mark_ =
            jval<bool>(json, "network.resume_pb_seam_mark", "resume_pb_seam_mark", false);
        // RS-2b (§21.6): the same idea one layer up — the ESTIMATION-side fine-slip gate,
        // diagnostic-only for K post-resume epochs.  Same exemption class as the drain
        // warm-up above: forward-only resume response policy, inert without a resume.
        network_resume_d5_warmup_ep_ =
            jval<int>(json, "network.resume_d5_warmup_ep", "resume_d5_warmup_ep", 0);
        if (network_resume_d5_warmup_ep_ < 0)
            throw std::runtime_error("network.resume_d5_warmup_ep must be >= 0");
        if (network_resume_d5_warmup_ep_ > 0)
            log->info("Network (RS-2b): resume D5 warm-up armed — for the first {} PROCESSED "
                      "epoch(s) after a warm resume the Card-4 D5 innovation gate is "
                      "DIAGNOSTIC-ONLY: a chord that trips k·σ̂₀ is counted (run-end "
                      "d5_warmup_suppressed, the network_d5_flags.csv warmup_suppressed "
                      "column, and the epoch's n_d5_flags — n_d5_retaps stays 0) but is NOT "
                      "re-tapped, NOT marked slipped for the next epoch, and its block is "
                      "not refolded.  A MEASUREMENT knob: it holds the restored branch "
                      "through the seam so \"branch right, cold station layer wrong\" can be "
                      "told from \"branch inconsistent\".  The σ̂₀ accumulator is fed BEFORE "
                      "the decision (calibration is not censored), the v3 corroboration gate "
                      "is untouched, and the MW/LLI/IF-TD drains stay live — a genuine "
                      "in-gap slip keeps a response.  Cold starts never arm it; NOT in the "
                      "warm-resume fingerprint.",
                      network_resume_d5_warmup_ep_);
        // §18.12 Card-1 coupling-regressor restore (read side of
        // network.state_snapshot_coupling_capture).  Same exemption class as the two
        // above: forward-only resume response policy, inert on a run that never resumes.
        network_resume_coupling_restore_ =
            jval<bool>(json, "network.resume_coupling_restore",
                       "resume_coupling_restore", false);
        if (network_resume_coupling_restore_) {
            if (!network_resume_frame_restore_)
                throw std::runtime_error(
                    "network.resume_coupling_restore requires "
                    "network.resume_frame_restore: the restored ŝ_prev are only "
                    "frame-valid once the pre-seam realization is restored, and the "
                    "first incumbent-less election's QUEUE-1(d) clause would erase the "
                    "leveling carries — coupling and all — before PASS B reads them.");
            if (!network_carry_conditional_coupling_)
                throw std::runtime_error(
                    "network.resume_coupling_restore requires "
                    "network.carry_conditional_coupling: buildCplRow is never called "
                    "otherwise, so the restored regressors would be dead weight.");
            if (!network_carry_coupling_gauge_project_)
                throw std::runtime_error(
                    "network.resume_coupling_restore requires "
                    "network.carry_coupling_gauge_project (Card 1f): the resume seam is "
                    "the maximum-scale truncation regime for a carried row, and 1f is "
                    "the shipped repair for the family-sum defect a truncated row leaves "
                    "(Mᵀg_s = −g_r(c)).  Without it the W5 null-space gate can withhold "
                    "every rows-carrying epoch.");
            if (network_amb_seam_reinflate_var_m2_ > 0.0)
                throw std::runtime_error(
                    "network.resume_coupling_restore and "
                    "network.amb_seam_reinflate_var_m2 > 0 are mutually exclusive.  Under "
                    "conditional coupling `var` IS the row's pure conditional variance, so "
                    "inflating it loosens the very row the regressors make border-neutral "
                    "while the row still asserts Σ M_j·ŝ_prev,j — a combination no "
                    "continuous epoch produces, and the in-tree precedent runs the other "
                    "way (NetworkRaim.cpp:771-772 CLEARS the regression whenever it "
                    "loosens the variance).  SPEC §18.11 rung 3 measured the inflation "
                    "alone at a 5.13 m consumer walk in h12.  Set "
                    "amb_seam_reinflate_var_m2 to 0.");
        }
        // RS-3 DATUM restore (read side of network.state_snapshot_datum_capture,
        // SPEC_event_harness §21.7).  Same exemption class as the three restores above:
        // forward-only resume response policy, inert on a run that never resumes.
        network_resume_datum_restore_ =
            jval<bool>(json, "network.resume_datum_restore", "resume_datum_restore", false);
        if (network_resume_datum_restore_ && !network_resume_frame_restore_)
            throw std::runtime_error(
                "network.resume_datum_restore requires network.resume_frame_restore: "
                "anchor_incumbent holds the SATELLITE-side parent edges of the very "
                "spanning tree these maps complete, and the frame restore is what brings "
                "it back.  Without it the first election runs incumbent-less and "
                "re-references every phase-bias family before the restored receiver-side "
                "maps can change anything — the restore would be a silent no-op.");
        // RS-5 EMIT-LAYER restore (read side of network.state_snapshot_emit_capture,
        // SPEC_event_harness §21.10).  Same exemption class as the restores above:
        // forward-only resume response policy, inert on a run that never resumes.
        network_resume_emit_restore_ =
            jval<bool>(json, "network.resume_emit_restore", "resume_emit_restore", false);
        if (network_resume_emit_restore_ && !network_resume_frame_restore_)
            throw std::runtime_error(
                "network.resume_emit_restore requires network.resume_frame_restore: the "
                "emit layer is a re-base of THIS run's emitted biases onto the writer's "
                "emission frame, which is only meaningful when the realization frame "
                "underneath it also continues.  Without the frame restore the first "
                "election re-references every phase-bias family, so the restored "
                "emission frame would re-base the new realization onto a level the "
                "writer never emitted — strictly worse than cold-starting the engines.");
        // RS-6 STATION-LAYER restore (read side of network.state_snapshot_station_capture,
        // SPEC_event_harness §21.13).  Same exemption class as the restores above:
        // forward-only resume response policy, inert on a run that never resumes.  The
        // two sub-switches are DIAGNOSTIC (default true) — they exist so one leg of the
        // attribution matrix can restore a single half, and they mean nothing without
        // the read knob itself.
        network_resume_station_restore_ =
            jval<bool>(json, "network.resume_station_restore", "resume_station_restore",
                       false);
        network_resume_station_restore_rxcb_ =
            jval<bool>(json, "network.resume_station_restore_rxcb",
                       "resume_station_restore_rxcb", true);
        network_resume_station_restore_shdev_ =
            jval<bool>(json, "network.resume_station_restore_shdev",
                       "resume_station_restore_shdev", true);
        // RS-7 DENSE-BORDER restore (read side of the snapshot's QK/QP records,
        // SPEC_event_harness §21.14).  Same exemption class as every restore above:
        // forward-only resume response policy, inert on a run that never resumes.
        //
        // ⚠ Deliberately NOT gated on network.carry_coupling_gauge_project (Card 1f) or
        // network.carry_conditional_coupling — THAT IS THE CARD.  One capture knob writes
        // two independent populations: the coupling ROWS (QL/QZ) are per-row REGRESSIONS
        // on the border, so a truncated one breaks the family identity Mᵀg_s = −g_r(c)
        // and needs 1f's repair — they stay behind resume_coupling_restore and its
        // interlock.  The QK/QP block restored here is a COVARIANCE: it carries no
        // regression, no gauge identity and no family sum, so it is legal on a
        // carry_invariant lineage where 1f is out of regime and resume_coupling_restore
        // is refused.  Before the split, a field warm resume could not restore its dense
        // border at all.
        network_resume_border_restore_ =
            jval<bool>(json, "network.resume_border_restore", "resume_border_restore",
                       false);
        if (network_resume_border_restore_ && !network_resume_frame_restore_)
            throw std::runtime_error(
                "network.resume_border_restore requires network.resume_frame_restore: "
                "the dense border is expressed IN the realization frame underneath it — "
                "its keys are absolute satellite clocks and code/phase biases against "
                "the datum that same solve realizes.  Without the frame restore the "
                "first election re-realizes that frame, so the restored cross-covariance "
                "would describe a level the writer never held: a stiff prior carried "
                "across a jump, which invariant #1 says does not degrade gracefully.");
        if (network_resume_border_restore_ && !network_shared_carry_full_cov_) {
            log->warn("Network (RS-7 border restore): resume_border_restore armed but "
                      "network.shared_carry_full_cov is OFF — nothing consumes a dense "
                      "carried block (every key folds through the per-key diagonal path "
                      "by construction), so the knob is INERT.  Disarmed.");
            network_resume_border_restore_ = false;
        }
        if (network_resume_station_restore_ && !network_resume_frame_restore_)
            throw std::runtime_error(
                "network.resume_station_restore requires network.resume_frame_restore: "
                "both station memories are expressed IN the realization frame underneath "
                "them — the rx-CB carry is an absolute receiver code bias against the "
                "code-bias datum, the T5 deviation is a residual against the SH field the "
                "same solve realizes.  Without the frame restore the first election "
                "re-realizes that frame, so the restored memories would re-assert a level "
                "the writer never held: a stale stiff prior across a jump, which "
                "invariant #1 says does not degrade gracefully.");
        // RS-8 CANONICAL-SIGNAL restore (read side of
        // network.state_snapshot_signal_capture, SPEC_event_harness §21.16).  Same
        // exemption class as the restores above: forward-only resume response policy,
        // inert on a run that never resumes.
        network_resume_signal_canon_restore_ =
            jval<bool>(json, "network.resume_signal_canon_restore",
                       "resume_signal_canon_restore", false);
        if (network_resume_signal_canon_restore_ && !network_resume_frame_restore_)
            throw std::runtime_error(
                "network.resume_signal_canon_restore requires "
                "network.resume_frame_restore: the arcs the restored realization frame "
                "names ARE the writer's canonical signals' — restoring the latch onto a "
                "re-elected frame would pin each station to the writer's channels while "
                "the datum underneath them is re-realized from this run's own tracker "
                "set, which is a different, unmeasured seam rather than the continuous "
                "one the card is about.");
        // RS-9 rx-CB LINEARIZATION-POINT restore (read side of
        // network.state_snapshot_rxcb_lin_capture, SPEC_event_harness §21.18).  Same
        // exemption class as every restore above: forward-only resume response policy,
        // inert on a run that never resumes.
        network_resume_rxcb_lin_restore_ =
            jval<bool>(json, "network.resume_rxcb_lin_restore", "resume_rxcb_lin_restore",
                       false);
        if (network_resume_rxcb_lin_restore_ && !network_resume_station_restore_)
            throw std::runtime_error(
                "network.resume_rxcb_lin_restore requires network.resume_station_restore: "
                "the RL rows restore the REFERENCE the rx-CB inject gate differences the "
                "CARRY against (innov = value_abs − x_lin), so without the carry restore "
                "the gate would judge a freshly harvested value against the writer's "
                "frame — the §21.18 defect inverted, not fixed.");
        if (network_resume_rxcb_lin_restore_ && !network_resume_station_restore_rxcb_)
            throw std::runtime_error(
                "network.resume_rxcb_lin_restore requires network.resume_station_restore_"
                "rxcb: that sub-switch is what restores the rx-CB carry itself, and "
                "restoring only the linearization point it is judged against leaves the "
                "gate comparing a cold first-sighted value to the writer's frame.");
        if (network_resume_rxcb_lin_restore_ && network_rx_code_bias_datum_)
            throw std::runtime_error(
                "network.resume_rxcb_lin_restore refuses network.rx_code_bias_datum: the "
                "restore is legal ONLY because an x_lin shift is an exact solve no-op for "
                "a state that enters linearly, so the gate is its only consumer.  The "
                "receiver code-bias DATUM breaks that — its prior's RHS is η = λ·(0 − "
                "x_lin(cb)) (ApplicationNetworkAssembly.cpp), so moving x_lin MOVES the "
                "target and therefore the solve.  Run the RS-9 restore without the datum, "
                "or the datum without the restore.");
        // RS-2 resume CARRY RE-BASE (SPEC_event_harness §21.4) — the reader half of
        // network.state_snapshot_carry_lin, and the same exemption class as the two
        // restores above: forward-only resume response policy, inert without a resume.
        network_resume_carry_rebase_ =
            jval<bool>(json, "network.resume_carry_rebase", "resume_carry_rebase", false);
        if (network_resume_carry_rebase_)
            log->info("Network (RS-2): resume CARRY RE-BASE armed — at the first DD prior "
                      "build after a warm resume, every restored chord the build projects is "
                      "re-based onto THIS run's linearization (ẑ += W·zl_writer − zl_now, "
                      "with W the cycle-basis projection the build already forms and zl_now "
                      "this epoch's Σ c·x_lin over the same incidence that forms z_free).  "
                      "Pᶻ is untouched — an exact frame translation, not a re-weighting.  A "
                      "component whose restored contributors carry no ZL row is marginalized "
                      "(born fresh on the diffuse prior) rather than folded frame-wrong, and "
                      "counted.  A blob with no ZL rows at all leaves today's behaviour and "
                      "warns.  NOT in the warm-resume fingerprint.");
        // READMIT continuity (E2 seed-first re-admission + the block re-coupling edge;
        // SPEC_synthetic_minimum_network §18.4, SPEC_amb_iono_split_overcount §7 FIRST).
        // Both default OFF = byte-identical; neither is fingerprinted (parking adds
        // entries and changes no carried value's meaning; the windows restart empty).
        // Cross-knob checks live in the ARMED summary below, after every knob is parsed.
        network_readmit_seed_horizon_s_ =
            jval<double>(json, "network.readmit_seed_horizon_s", "readmit_seed_horizon_s", 0.0);
        if (!(network_readmit_seed_horizon_s_ >= 0.0))
            throw std::runtime_error("network.readmit_seed_horizon_s must be >= 0");
        network_readmit_coupling_marg_ep_ =
            jval<int>(json, "network.readmit_coupling_marg_ep", "readmit_coupling_marg_ep", 0);
        if (network_readmit_coupling_marg_ep_ < 0)
            throw std::runtime_error("network.readmit_coupling_marg_ep must be >= 0");
        // Sub-knob of the seed horizon: park + seed the code-bias columns too (default), or
        // the clock alone (the first seeded return of runs/readmit24h_e2 rejected the
        // third-frequency C5Q seed at 43 σ while the IF-pair seeds held).
        network_readmit_seed_code_bias_ =
            jval<bool>(json, "network.readmit_seed_code_bias", "readmit_seed_code_bias", true);
        // Card CPA, canonical-pair admission
        // (docs/refound/CPA_canonical_pair_admission_2026-09-20.md).  Default OFF =
        // byte-identical; not fingerprinted.  The tolerance is CLAMPED rather than
        // refused: 0 is a meaningful arm (admit only the epochs that hold the pair)
        // and a negative value has no reading other than that one.
        network_canonical_pair_admission_ =
            jval<bool>(json, "network.canonical_pair_admission", "canonical_pair_admission",
                       false);
        network_canonical_pair_keep_epochs_ =
            jval<int>(json, "network.canonical_pair_keep_epochs", "canonical_pair_keep_epochs",
                      3);
        if (network_canonical_pair_keep_epochs_ < 0) {
            log->warn("Network (card CPA): network.canonical_pair_keep_epochs = {} is "
                      "negative, clamped to 0 (admit only the epochs that hold the "
                      "canonical pair).", network_canonical_pair_keep_epochs_);
            network_canonical_pair_keep_epochs_ = 0;
        }
        if (network_resume_frame_restore_ || network_resume_slip_warmup_ep_ > 0) {
            if (network_config_.state_snapshot_path.empty())
                log->warn("Network (resume frame restore): resume_frame_restore / "
                          "resume_slip_warmup_ep armed but network.state_snapshot_path is "
                          "empty — nothing can resume, both are INERT this run.");
            else
                log->info("Network (resume frame restore): frame restore {} — at a warm "
                          "resume the snapshot's phase-anchor incumbents + sticky pin "
                          "targets, Q3 clock-anchor per-sat state and CLK-R held frame "
                          "are restored before the first election/fold/emit (§18.10; "
                          "frame rows are captured under network.state_snapshot_frame_"
                          "capture, snapshot format v2 — v3 when "
                          "network.state_snapshot_coupling_capture also carries the Card-1 "
                          "regressors, §18.12); MW/LLI "
                          "drain diagnostic-only for {} post-resume epoch(s).  Neither is "
                          "in the warm-resume fingerprint (forward-only resume response "
                          "policy).",
                          network_resume_frame_restore_ ? "ON" : "off",
                          network_resume_slip_warmup_ep_);
        }
        // RS-3: the datum restore's own ARMED/INERT line (its prerequisite is checked
        // above, so by here it can only be inert for want of a snapshot to resume from).
        if (network_resume_datum_restore_) {
            if (network_config_.state_snapshot_path.empty())
                log->warn("Network (resume datum restore): resume_datum_restore armed but "
                          "network.state_snapshot_path is empty — nothing can resume, "
                          "INERT this run.");
            else
                log->info("Network (resume datum restore): ON — at a warm resume the "
                          "snapshot's eq34 spanning-tree maps (sticky datum receiver, "
                          "receiver parent edges, satellite component roots, pin targets, "
                          "longevity elevations, gap-retained edges) are restored before "
                          "the first election, and the time-linked code-bias "
                          "previous-value map + ever-seen set are restored with the "
                          "'resumed' latch left FALSE, so a code-bias re-seed disc-bumps "
                          "only where it genuinely re-seeds.  The rows are captured under "
                          "network.state_snapshot_datum_capture; a file without them "
                          "leaves each half exactly as today and says so.  Not in the "
                          "warm-resume fingerprint (forward-only resume response policy).");
        }
        // RS-5 (§21.10): the emit-layer restore's own ARMED/INERT line.  Its
        // prerequisite is checked above, so by here it can only be inert for want of a
        // snapshot to resume from.
        if (network_resume_emit_restore_) {
            if (network_config_.state_snapshot_path.empty())
                log->warn("Network (RS-5 emit restore): resume_emit_restore armed but "
                          "network.state_snapshot_path is empty — nothing can resume, "
                          "INERT this run.");
            else
                log->info("Network (RS-5 emit restore): ON — at a warm resume the "
                          "snapshot's emit-layer rows are restored BEFORE the first emit: "
                          "the eq34 emit re-base's emission frame (ER) so the first "
                          "post-resume epoch re-bases onto the writer's last emitted "
                          "values instead of first-sighting every key, and the PB emission "
                          "filter's per-key posterior + step-noise accumulators (PF/PS) so "
                          "it continues smoothing at the gate the writer had measured.  "
                          "The rows are captured under "
                          "network.state_snapshot_emit_capture; a file without them "
                          "leaves each half exactly as today and says so.  Outputs-only "
                          "(no estimate moves) and not in the warm-resume fingerprint "
                          "(forward-only resume response policy).");
        }
        // RS-6 (§21.13): the station-layer restore's own ARMED/INERT line.  Its
        // prerequisite is checked above, so by here it can only be inert for want of a
        // snapshot to resume from.  The sub-switches are named only when one is OFF —
        // an attribution leg has to be self-describing in the log, and the ordinary
        // both-halves arm must not grow a line that says nothing.
        if (network_resume_station_restore_) {
            if (network_config_.state_snapshot_path.empty())
                log->warn("Network (RS-6 station restore): resume_station_restore armed "
                          "but network.state_snapshot_path is empty — nothing can resume, "
                          "INERT this run.");
            else
                log->info("Network (RS-6 station restore): ON{} — at a warm resume the "
                          "snapshot's station-layer rows are restored BEFORE the first "
                          "epoch: the rx-CB carry (RC) through the restored-arc sentinel "
                          "so the receiver code-bias priors continue instead of the whole "
                          "network re-seeding from one epoch of code, and the T5/Iono-2 "
                          "per-LOS deviation carry (SD) with its harvest stamp, which is "
                          "what makes a restored entry read WARM rather than cold-seed at "
                          "steady state.  The rows are captured under "
                          "network.state_snapshot_station_capture; a file without them "
                          "leaves each half exactly as today and says so.  Not in the "
                          "warm-resume fingerprint (forward-only resume response policy).",
                          (network_resume_station_restore_rxcb_
                           && network_resume_station_restore_shdev_)
                              ? ""
                              : (network_resume_station_restore_rxcb_
                                     ? " (rx-CB half ONLY — resume_station_restore_shdev "
                                       "false, a §21.13 attribution leg)"
                                     : network_resume_station_restore_shdev_
                                           ? " (T5 deviation half ONLY — "
                                             "resume_station_restore_rxcb false, a §21.13 "
                                             "attribution leg)"
                                           : " (⚠ BOTH sub-switches false — the knob is "
                                             "armed and restores NOTHING)"));
        }
        // RS-8 (§21.16): the canonical-signal restore's own ARMED/INERT line, beside
        // the RS-6 one.  Its prerequisite is checked above, so by here it can only be
        // inert for want of a snapshot to resume from.
        if (network_resume_signal_canon_restore_) {
            if (network_config_.state_snapshot_path.empty())
                log->warn("Network (RS-8 canonical restore): resume_signal_canon_restore "
                          "armed but network.state_snapshot_path is empty — nothing can "
                          "resume, INERT this run.");
            else
                log->info("Network (RS-8 canonical restore): ON — at a warm resume the "
                          "snapshot's CN rows are restored into each station's PppModel "
                          "BEFORE the first epoch, so the per-(constellation, band, C|L) "
                          "canonical-signal latch continues the writer's instead of being "
                          "RE-decided from the resume epoch's satellite counts.  The rows "
                          "are captured under network.state_snapshot_signal_capture; a "
                          "file without them leaves today's behaviour and says so.  Not "
                          "in the warm-resume fingerprint (forward-only resume response "
                          "policy).");
        }
        // RS-9 (§21.18): the rx-CB LINEARIZATION-POINT restore's own ARMED/INERT line.
        // Both prerequisites are hard throws above, so by here the knob can only be
        // inert for want of a snapshot to resume from.
        if (network_resume_rxcb_lin_restore_) {
            if (network_config_.state_snapshot_path.empty())
                log->warn("Network (RS-9 rx-CB frame restore): resume_rxcb_lin_restore "
                          "armed but network.state_snapshot_path is empty — nothing can "
                          "resume, INERT this run.");
            else
                log->info("Network (RS-9 rx-CB frame restore): ON — at a warm resume each "
                          "station's code-bias state is seeded from the snapshot's RL rows "
                          "(the writer's x_lin) instead of from the resume epoch's one-shot "
                          "code median, so the rx-CB inject gate differences the RS-6-"
                          "restored carry against the frame the writer judged it against.  "
                          "Without it the gate's fixed 5.13 m bar refused 17 of 61 field "
                          "stations EVERY epoch of a leg against the continuous run's 7, "
                          "and those stations carried no receiver-DCB memory at all while "
                          "the run-end line still read '61 carried' (§21.18).  An EXACT "
                          "solve no-op (a code bias enters linearly; the gate is the only "
                          "consumer) — network.rx_code_bias_datum, whose RHS reads x_lin, "
                          "is refused with it.  Not in the warm-resume fingerprint "
                          "(forward-only resume response policy).");
        }
        // RS-7 (§21.14): the DENSE-BORDER restore's own ARMED/INERT line, same shape.
        // The full-cov prerequisite is warn-disarmed above, so by here the knob can only
        // be inert for want of a snapshot to resume from.
        if (network_resume_border_restore_) {
            if (network_config_.state_snapshot_path.empty())
                log->warn("Network (RS-7 border restore): resume_border_restore armed but "
                          "network.state_snapshot_path is empty — nothing can resume, "
                          "INERT this run.");
            else
                log->info("Network (RS-7 border restore): ON — at a warm resume the C1 "
                          "dense shared-carry covariance is rebuilt from the snapshot's "
                          "QK/QP records for every key that also has a restored D row, so "
                          "the FIRST warm fold runs the dense (P_prev+Q)⁻¹ path instead of "
                          "re-seeding the border's cross-covariance from one epoch of "
                          "data.  Keys with no restored row are dropped (row AND column) "
                          "and counted; a file without QK/QP rows leaves the border "
                          "exactly as today and says so.  The coupling ROWS (QL/QZ) are a "
                          "SEPARATE population and stay behind "
                          "network.resume_coupling_restore with its Card-1f interlock — a "
                          "covariance block carries no regression and no gauge identity, "
                          "so this half is legal on a carry_invariant lineage.  Not in "
                          "the warm-resume fingerprint (forward-only resume response "
                          "policy).");
        }
        // RS-4 (§21.9): the DECLARATION half's own ARMED/INERT line, same shape.
        if (network_resume_pb_seam_mark_) {
            if (network_config_.state_snapshot_path.empty())
                log->warn("Network (RS-4 PB seam mark): resume_pb_seam_mark armed but "
                          "network.state_snapshot_path is empty — nothing can resume, so "
                          "the one-shot mark can never fire.  INERT this run.");
            else
                log->info("Network (RS-4 PB seam mark): ON — at the FIRST emit after a warm "
                          "resume every emitted satellite phase bias is compared with its "
                          "RESTORED emitter baseline (the snapshot's epb rows) over the jump "
                          "test's own population, BEFORE the baselines are overwritten; a "
                          "constellation with more than {:.0f} % of that population beyond "
                          "discThresholdM is markDatumChange'd exactly as the CLK-R "
                          "resume_seam rule marks (E-record datum_changed 1, one "
                          "PB_FRAME/resume_seam event, the n_pb_seam_marks counter).  Closes "
                          "the gap that rule leaves: it fires only when the held-frame clock "
                          "engine reports epochs==0, so a lineage that RESTORES the frame is "
                          "never marked — exactly the case where the clock is continuous and "
                          "the phase-bias level is not (~17 m p50 measured, datum_changed 0).  "
                          "One shot; structurally unreachable on a cold start.  Outputs-only "
                          "and not in the warm-resume fingerprint (forward-only resume "
                          "response policy).",
                          GnssConfig::kResumePbSeamMarkFraction * 100.0);
        }
        // Slant-iono carry (QUEUE 8b, docs/SPEC_slip_relevel_walk.md s8 residual): carry
        // per-LOS slant iono across epochs as a diagonal RW prior via the same
        // addStationPrior path.  NOT the phantom filter.process_noise.ionosphere_q (which
        // has no network reader) and NOT the falsified adaptive-q — a new carry seam.
        network_carry_iono_ =
            jval<bool>(json, "network.carry_iono", "carry_iono", false);
        network_iono_carry_q_ =
            jval<double>(json, "network.iono_carry_q", "iono_carry_q",
                         network_config_.process_noise.get(NetworkQClass::IonoCarry));
        network_iono_carry_scope_ =
            jval<std::string>(json, "network.iono_carry_scope", "iono_carry_scope",
                              std::string("relevel"));
        network_iono_carry_var_floor_ =
            jval<double>(json, "network.iono_carry_var_floor", "iono_carry_var_floor", 0.04);
        network_iono_carry_max_gap_s_ =
            jval<double>(json, "network.iono_carry_max_gap_s", "iono_carry_max_gap_s", 300.0);
        // CS-GF authority (the i1 falsification's successor, SPEC_iono_carry_8b
        // §6.2): what feeds the carried value.  "solve" = the est-reconstructed
        // per-LOS iono (measured accumulating the level-locked displacement);
        // "smoothed_gf" = the carrier-smoothed GF-code authority (level-
        // independent, drains toward the code realization).
        network_iono_carry_source_ =
            jval<std::string>(json, "network.iono_carry_source", "iono_carry_source",
                              std::string("solve"));
        if (network_iono_carry_q_ < 0.0)
            throw std::runtime_error("network.iono_carry_q must be >= 0");
        if (network_iono_carry_var_floor_ < 0.0)
            throw std::runtime_error("network.iono_carry_var_floor must be >= 0");
        if (network_iono_carry_scope_ != "relevel" && network_iono_carry_scope_ != "always")
            throw std::runtime_error("network.iono_carry_scope must be \"relevel\" or \"always\"");
        if (network_iono_carry_source_ != "solve" && network_iono_carry_source_ != "smoothed_gf")
            throw std::runtime_error("network.iono_carry_source must be \"solve\" or \"smoothed_gf\"");
        // rx-CB carry (docs/SPEC_rx_cb_carry.md): carry receiver code biases across
        // epochs.  Independent of the phase-leveling gate by construction.
        network_carry_rx_code_bias_ =
            jval<bool>(json, "network.carry_rx_code_bias", "carry_rx_code_bias", false);
        network_rx_cb_carry_q_ =
            jval<double>(json, "network.rx_cb_carry_q", "rx_cb_carry_q", 1e-9);
        // Card LV-2 (docs/SPEC_level_information_budget.md): de-mean the rx-CB carry's
        // common mode.  Parsed next to the carry it modifies; its guard/banner lives
        // in the post-parse block with the carry's own (carry_rx_code_bias can still
        // be forced OFF there by the code_datum_time_linked guard, and a de-mean of a
        // disarmed carry must read as INERT, not as armed).
        network_rx_cb_carry_demean_ =
            jval<bool>(json, "network.rx_cb_carry_demean", "rx_cb_carry_demean", false);
        network_rx_cb_carry_demean_min_stations_ =
            jval<int>(json, "network.rx_cb_carry_demean_min_stations",
                      "rx_cb_carry_demean_min_stations", 8);
        // Card LV-5 (docs/SPEC_level_information_budget.md §12): reduce μ between
        // PASS A and PASS B instead of at the harvest — removes the lag-1 delay.
        network_rx_cb_carry_demean_within_ =
            jval<bool>(json, "network.rx_cb_carry_demean_within",
                       "rx_cb_carry_demean_within", false);
        // Card LV-4 (docs/SPEC_level_information_budget.md §11): swap the carry's
        // FRAME-DEPENDENT inject screen for the frame-invariant harvest-side step.
        network_rx_cb_carry_step_gate_ =
            jval<bool>(json, "network.rx_cb_carry_step_gate", "rx_cb_carry_step_gate", false);
        network_rx_cb_carry_step_k_sigma_ =
            jval<double>(json, "network.rx_cb_carry_step_k_sigma",
                         "rx_cb_carry_step_k_sigma", 10.0);
        // Card S2H (docs/SPEC_level_information_budget.md §17 item 3): the
        // S-consistent harvest — post-solve Λ-weighted de-mean of the harvested
        // ABSOLUTE rx-CB realization, per family.  Guard/banner in the post-parse
        // block with the other LV cards (the carry can still be forced OFF there).
        network_rx_cb_harvest_demean_ =
            jval<bool>(json, "network.rx_cb_harvest_demean", "rx_cb_harvest_demean", false);
        network_rx_cb_harvest_screen_z_ =
            jval<double>(json, "network.rx_cb_harvest_screen_z",
                         "rx_cb_harvest_screen_z", 8.0);
        // NOTE: validation (datum guard + banner) lives in the post-parse block next
        // to the carry_zwd validation — code_datum_time_linked parses AFTER this site.
        // M2 (LS-VCE carry-q): the carry-row variance-component MONITOR.  A
        // calibration/honesty instrument — accumulates carry innovations and logs
        // σ̂ against the hand floors; NO feedback into the solve (v1).  Default OFF
        // keeps the accumulator + logging inert → byte-identical.
        network_carry_vce_ =
            jval<bool>(json, "network.carry_vce", "carry_vce", false);
        network_carry_vce_apply_ =
            jval<bool>(json, "network.carry_vce_apply", "carry_vce_apply", false);
        if (network_carry_vce_apply_ && !network_carry_vce_)
            throw std::runtime_error(
                "network.carry_vce_apply requires network.carry_vce = true "
                "(the apply path feeds back the estimator's σ̂, which only runs when the monitor does).");
        network_carry_vce_components_ =
            jval<std::string>(json, "network.carry_vce_components", "carry_vce_components", "pl");
        network_carry_vce_window_epochs_ =
            jval<int>(json, "network.carry_vce_window_epochs", "carry_vce_window_epochs", 60);
        // M2 v2 APPLY (docs/SPEC_carry_vce_apply.md) — LASER 5-95's variance-factor
        // iteration.  Every carry fold variance is multiplied by a per-(family ×
        // constellation) α² that tracks the monitor's measured
        // robust_floor / hand_fold_var, INFLATE-ONLY and slew-limited.  Unlike the
        // monitor this is SOLVE-AFFECTING, so it is fingerprinted and the loader
        // states that in the banner rather than leaving it to the reader.
        network_carry_vce_apply_families_ =
            jval<std::string>(json, "network.carry_vce_apply_families",
                              "carry_vce_apply_families", "pl");
        network_carry_vce_apply_slew_ =
            jval<double>(json, "network.carry_vce_apply_slew", "carry_vce_apply_slew", 1.05);
        network_carry_vce_apply_max_ =
            jval<double>(json, "network.carry_vce_apply_max", "carry_vce_apply_max", 1000.0);
        network_carry_vce_apply_min_rows_ =
            jval<int>(json, "network.carry_vce_apply_min_rows", "carry_vce_apply_min_rows", 200);
        if (network_carry_vce_apply_) {
            // A family with no monitor has no σ̂ to apply.  HARD ERROR rather than a
            // silent unity factor: an inert apply that logs clean is precisely the
            // dead-wiring failure standing invariant #4 exists to forbid.
            const auto mon_fams = NetworkCarryVce::parseComponents(network_carry_vce_components_);
            const auto app_fams = NetworkCarryVce::parseComponents(network_carry_vce_apply_families_);
            if (app_fams.empty())
                throw std::runtime_error(
                    "network.carry_vce_apply is armed but network.carry_vce_apply_families is empty "
                    "— the apply path would be inert (standing invariant #4).");
            for (const auto& f : app_fams) {
                if (NetworkCarryVceApply::familySlot(f) < 0)
                    throw std::runtime_error(
                        "network.carry_vce_apply_families contains unknown carry family '" + f +
                        "' (known: pl, pb, cb).");
                if (std::find(mon_fams.begin(), mon_fams.end(), f) == mon_fams.end())
                    throw std::runtime_error(
                        "network.carry_vce_apply_families contains '" + f + "' which is NOT in "
                        "network.carry_vce_components ('" + network_carry_vce_components_ + "') — "
                        "the apply path can only scale a family the monitor measures.");
            }
            if (network_carry_vce_apply_slew_ <= 1.0)
                log->warn("Network (M2 v2): network.carry_vce_apply_slew {} <= 1 — the per-epoch "
                          "slew limiter is DISABLED (alpha2 jumps to its target in one step).  "
                          "The inflate-only clamp is still the safety argument, but the solve's "
                          "conditioning will step.", network_carry_vce_apply_slew_);
            if (network_carry_vce_apply_max_ < 1.0) {
                log->warn("Network (M2 v2): network.carry_vce_apply_max {} < 1 — raised to 1 "
                          "(the factor is inflate-only, so a ceiling below 1 would pin it).",
                          network_carry_vce_apply_max_);
                network_carry_vce_apply_max_ = 1.0;
            }
            if (network_carry_vce_apply_min_rows_ < 0) network_carry_vce_apply_min_rows_ = 0;
            // NOTE the `pl` INERT guard is NOT here: it needs network_carry_invariant_,
            // which is parsed ~1500 lines further down.  It lives in the post-parse
            // block next to the phase-leveling B4 floor banner.
            const bool apply_shared =
                std::find(app_fams.begin(), app_fams.end(), std::string("pb")) != app_fams.end()
             || std::find(app_fams.begin(), app_fams.end(), std::string("cb")) != app_fams.end();
            if (apply_shared)
                log->warn("Network (M2 v2): network.carry_vce_apply_families includes a SHARED "
                          "carry family (pb/cb).  The M2 field record puts the pb mean/robust gap "
                          "at ~200-1000x (it carries the metre-scale ensemble-datum re-level "
                          "tail), and docs/SPEC_carry_vce_apply.md defers pb/cb behind the pl "
                          "gate for exactly that reason — the robust/mean split has not been "
                          "re-examined on those families.  Wired and countable, but UNVALIDATED.");
            log->info("Network producer (M2 v2 / LASER 5-95): carry variance-FACTOR apply ENABLED "
                      "(families='{}', slew={}, alpha2_max={}, min_rows={}).  Per (family x "
                      "constellation) the carry fold variance is multiplied by alpha2, iterated "
                      "alpha2 <- clamp(alpha2 * clamp(robust_floor/hand_fold_var/alpha2, "
                      "[1/slew,slew]), [1,max]) once per epoch from the M2 monitor, and frozen on "
                      "contaminated or under-populated epochs.  INFLATE-ONLY: alpha2 >= 1, so a "
                      "stiffer-than-truth carry prior is structurally unreachable (standing "
                      "invariant #1).  *** SOLVE-AFFECTING and FINGERPRINTED *** — fresh "
                      "convergence only; a warm resume with this armed is a hard run error, and "
                      "seed and legs must ALL carry it (never flip mid-chain).  Convergence "
                      "readout: sigma_hat_post -> 1 per group in network_carry_vce_apply.csv.",
                      network_carry_vce_apply_families_, network_carry_vce_apply_slew_,
                      network_carry_vce_apply_max_, network_carry_vce_apply_min_rows_);
        }
        if (network_carry_vce_ && network_carry_vce_window_epochs_ < 1) {
            log->warn("Network (M2/LS-VCE): network.carry_vce_window_epochs {} < 1 — treating as "
                      "whole-run accumulation (no forgetting).",
                      network_carry_vce_window_epochs_);
        }
        if (network_carry_vce_)
            log->info("Network producer (M2/LS-VCE): carry-row variance-component MONITOR enabled "
                      "(components='{}', window={} epochs) — per family per constellation the "
                      "post-fit carry innovations are folded into a residual-form LS-VCE estimator "
                      "and logged as σ̂ (Floor m² + Rate m²/s) against the hand floors.  No feedback "
                      "into the solve (calibration/honesty instrument; solution byte-identical).",
                      network_carry_vce_components_, network_carry_vce_window_epochs_);

        // PRIOR-RESIDUAL CENSUS (docs/SPEC_prior_residual_census.md).  The M2
        // monitor above POOLS the per-row partial redundancies into a variance
        // component; this card TESTS them one by one and reports the empirical
        // exceedance ladder per (family × label).  Same hook, same arithmetic, no
        // new solve — and, like the monitor, no feedback: the solve is
        // byte-identical with it armed.  Parsed here, next to carry_vce, because
        // the two share the statistic and the families spelling.
        {
            NetworkConfig& pnc = network_config_;
            pnc.prior_residual_census =
                jval<bool>(json, "network.prior_residual_census", "prior_residual_census", false);
            pnc.prior_residual_families =
                jval<std::string>(json, "network.prior_residual_families",
                                  "prior_residual_families", "pl,pb,cb");
            pnc.prior_residual_window_epochs =
                jval<int>(json, "network.prior_residual_window_epochs",
                          "prior_residual_window_epochs", 60);
            pnc.prior_residual_min_rows =
                jval<int>(json, "network.prior_residual_min_rows",
                          "prior_residual_min_rows", 200);
            pnc.prior_residual_report_sigma =
                jval<double>(json, "network.prior_residual_report_sigma",
                             "prior_residual_report_sigma", 4.0);
            pnc.prior_residual_event_holdoff_ep =
                jval<int>(json, "network.prior_residual_event_holdoff_ep",
                          "prior_residual_event_holdoff_ep", 20);
            pnc.prior_residual_corroborate =
                jval<bool>(json, "network.prior_residual_corroborate",
                           "prior_residual_corroborate", false);
            // CONSTRAINT-AWARE GLOBAL VARIANCE COMPONENT
            // (docs/SPEC_prior_residual_census.md §14) — a rider on the census
            // accumulator, parsed with it because it shares its rows.  The
            // raim_redundancy_normalize tripwire canNOT live here:
            // loadNetworkQcTiers_ runs AFTER this helper and has not resolved
            // that flag yet, so it sits at the tail of that helper next to the
            // MDB precondition (the section's load-order rule).
            pnc.prior_residual_global_sigma0 =
                jval<bool>(json, "network.prior_residual_global_sigma0",
                           "prior_residual_global_sigma0", false);
            // Standing invariant #4 tripwire: a corroboration arm without the
            // census would silently produce nothing at all.  HARD ERROR rather
            // than a silent disarm — a config that asks for the contingency table
            // and gets an empty run is the exact "live counter over dead wiring"
            // failure this project keeps paying for.
            if (pnc.prior_residual_corroborate && !pnc.prior_residual_census)
                throw std::runtime_error(
                    "network.prior_residual_corroborate requires network.prior_residual_census = true "
                    "(the corroboration join runs inside the census; arming it alone produces no rows, "
                    "no contingency table and no warning at run time).");
            // Same tripwire, same reason: the global form's PRIOR side IS the
            // census's folded rows, so without the census it has no numerator
            // and no `l` — it would emit a data-only number under a name that
            // promises a constraint-aware one, which is worse than emitting
            // nothing.
            if (pnc.prior_residual_global_sigma0 && !pnc.prior_residual_census)
                throw std::runtime_error(
                    "network.prior_residual_global_sigma0 requires network.prior_residual_census = true "
                    "(the global form's prior side ẽ₀ᵀP₀ẽ₀ and its constraint count `l` ARE the census's "
                    "folded rows; without the census the column would carry a DATA-ONLY variance factor "
                    "under a name that promises a constraint-aware one).");
            if (pnc.prior_residual_census) {
                // Invariant-#4 tripwire: a family with no hook in this build.
                // The census would report it as "armed, zero rows" at run end,
                // which is indistinguishable from a real negative result — so
                // name it HERE, at load, where the typo is fixable.
                static const char* kKnownFamilies[] = {"pl", "pb", "cb", "zwd", "iono", "rxcb", "clk"};
                for (const std::string& f :
                     NetworkCarryVce::parseComponents(pnc.prior_residual_families)) {
                    bool known = false;
                    for (const char* k : kKnownFamilies) if (f == k) { known = true; break; }
                    if (!known)
                        log->warn("Network (prior-residual census): family '{}' in "
                                  "network.prior_residual_families has NO feed hook in this build "
                                  "(known: pl, pb, cb, zwd, iono, rxcb, clk) — it will report zero rows, which "
                                  "is indistinguishable from a genuine negative result.  Fix the "
                                  "spelling or drop it.", f);
                }
                if (pnc.prior_residual_window_epochs < 1)
                    log->warn("Network (prior-residual census): "
                              "network.prior_residual_window_epochs {} < 1 — treating as whole-run "
                              "accumulation (no forgetting) for σ̂₀.",
                              pnc.prior_residual_window_epochs);
                if (!(pnc.prior_residual_report_sigma > 0.0)) {
                    log->warn("Network (prior-residual census): "
                              "network.prior_residual_report_sigma = {:g} is not positive — no row "
                              "would ever be traced or evented.  Restoring the 4.0 default.",
                              pnc.prior_residual_report_sigma);
                    pnc.prior_residual_report_sigma = 4.0;
                }
                if (pnc.prior_residual_min_rows < 0) pnc.prior_residual_min_rows = 0;
                // NOTE: the INC-3 auto-arm of network.cycle_slip.gap_instrument is
                // NOT here — loadNetworkSlipDetectors_ runs AFTER this helper and
                // would overwrite it.  Per the section's load-order rule ("a later
                // helper reads what an earlier one resolved") it lives at the tail
                // of that helper, reading the flag this block just set.
                log->info("Network producer (prior-residual census): ARMED "
                          "(families='{}', window={} ep, min_rows={}, report={:g}σ̂₀, "
                          "corroborate={}).  Per carried prior per epoch: the LASER 5-74 residual of "
                          "prior information ê = z₀ − ξ̂ with its dispersion D{{ê}} = r·fold_var "
                          "(folded) or fold_var + post_var (not folded), standardized by a per-family "
                          "empirical σ̂₀ (D5 v2's accumulator) and counted onto a fixed exceedance "
                          "ladder per (family × label ∈ CLEAN/RESPONDED/FRAME).  MEASURE-ONLY: no "
                          "gate, no deweight, no refloat — report_sigma is a TRACE threshold and the "
                          "solve is byte-identical.  The critical value is EMPIRICAL, never tabular "
                          "(D5 v1's t-table cost three field cycles).",
                          pnc.prior_residual_families, pnc.prior_residual_window_epochs,
                          pnc.prior_residual_min_rows, pnc.prior_residual_report_sigma,
                          pnc.prior_residual_corroborate ? "on" : "off");
                if (pnc.prior_residual_global_sigma0)
                    log->info("Network producer (constraint-aware global σ̂₀, "
                              "docs/SPEC_prior_residual_census.md §14): ARMED — per epoch "
                              "σ̂₀²_global = (Σw·v² + Σw·ê²)/(Σr_data + Σr_prior), i.e. LASER 5-74's "
                              "prior residuals in the NUMERATOR and the constraints in the "
                              "REDUNDANCY, with Σw·ê²/Σw·v² ('how much of the epoch's tension lives "
                              "in the carry') as the headline.  Data side rides the post-fit RAIM "
                              "row walk; prior side is this census's FOLDED rows only.  MEASURE-ONLY "
                              "AND ONE LEVEL DEEPER THAN THE CENSUS: the A4 raim_withhold_sigma0 "
                              "guard, every gate and every deweight stay on the ROBUST MEDIAN σ̂₀ — "
                              "v1 wires zero decisions to the new quantity, and it is a SCALE, not a "
                              "test statistic.");
            }
        }

        // Station-adaptive SH-VTEC-constraint deweight (invariant #6: default OFF /
        // byte-identical).  Relaxes the per-LOS-iono ↔ global SH field coupling per
        // station by a factor w_s ∈ (0,1] (σ² ×= 1/w_s) — the surgical alternative to
        // dropping a hostile (scintillation-belt) station: it keeps serving the shared
        // clocks / biases / datum / position while its per-LOS iono stops poisoning the
        // field and its own ambiguities.  v1 static list + v2 adaptive IGG gate.
        {
            IonoConstraintDeweightConfig& idw = iono_constraint_deweight_;
            idw.stations.clear();
            const auto& dw_arr = jsection(json, "network.iono_constraint_deweight_stations",
                                          "iono_constraint_deweight_stations");
            if (dw_arr.is_array())
                idw.stations = dw_arr.get<std::vector<std::string>>();
            idw.factor = jval<double>(json, "network.iono_constraint_deweight_factor",
                                      "iono_constraint_deweight_factor", 0.0);
            idw.adaptive = jval<bool>(json, "network.iono_constraint_deweight_adaptive",
                                      "iono_constraint_deweight_adaptive", false);
            idw.window_epochs = jval<double>(json, "network.iono_constraint_deweight_window_epochs",
                                             "iono_constraint_deweight_window_epochs", 60.0);
            idw.sigma_hi = jval<double>(json, "network.iono_constraint_deweight_sigma_hi",
                                        "iono_constraint_deweight_sigma_hi", 3.0);
            idw.sigma_lo = jval<double>(json, "network.iono_constraint_deweight_sigma_lo",
                                        "iono_constraint_deweight_sigma_lo", 2.0);
            idw.floor = jval<double>(json, "network.iono_constraint_deweight_floor",
                                     "iono_constraint_deweight_floor", 0.0);
            idw.rate_limit = jval<double>(json, "network.iono_constraint_deweight_rate_limit",
                                          "iono_constraint_deweight_rate_limit", 0.25);
            idw.min_epochs = jval<int>(json, "network.iono_constraint_deweight_min_epochs",
                                       "iono_constraint_deweight_min_epochs", 30);
            // Clamps: keep the mechanism well-posed (never NaN / never information-inflating).
            if (idw.factor < 0.0) idw.factor = 0.0;
            if (idw.factor > 1.0) idw.factor = 1.0;
            if (idw.floor < 0.0)  idw.floor = 0.0;
            if (idw.floor >= 1.0) idw.floor = 0.999;
            if (idw.window_epochs < 1.0) idw.window_epochs = 1.0;
            if (idw.rate_limit <= 0.0) idw.rate_limit = 1.0;   // 0 ⇒ no slew limit (snap)
            if (idw.min_epochs < 1) idw.min_epochs = 1;
            const bool dw_static_on = !idw.stations.empty() && idw.factor < 1.0;
            if (idw.adaptive && idw.sigma_lo >= idw.sigma_hi) {
                log->warn("Network (iono deweight): sigma_lo ({:.2f}) >= sigma_hi ({:.2f}) — "
                          "hysteresis band ill-formed; forcing sigma_lo = 0.5*sigma_hi.",
                          idw.sigma_lo, idw.sigma_hi);
                idw.sigma_lo = 0.5 * idw.sigma_hi;
            }
            if (dw_static_on)
                log->info("Network (iono deweight v1): {} station(s) statically deweighted at "
                          "w_s={:.3f} ({} — {} the SH field) — they still serve the shared "
                          "clocks/biases/datum/position.",
                          idw.stations.size(), idw.factor,
                          (idw.factor <= 0.0) ? "w_s=0, free-floating iono" : "partial info",
                          (idw.factor <= 0.0) ? "fully decoupled from"
                                              : "loosely coupled to");
            if (idw.adaptive)
                log->info("Network (iono deweight v2): adaptive IGG gate ON — engage a station when "
                          "its EWMA SH-misfit z-score (median/MAD across the network, half-life "
                          "{:.0f} ep, ≥{} clean samples) exceeds {:.2f}σ, release below {:.2f}σ; "
                          "ramp to w_s floor {:.3f}, slew ≤{:.3f}/epoch.",
                          idw.window_epochs, idw.min_epochs, idw.sigma_hi, idw.sigma_lo,
                          idw.floor, idw.rate_limit);
        }
    }

    // =================================================================
    //  network subsection helper: loadNetworkSlipDetectors_
    // =================================================================
    /**
     * @brief Cycle-slip detectors (@c network.cycle_slip.*) and the slip-QC hardening cards R/L/G.
     *
     * Ends with standing invariant #1's auto-arm: phase leveling carries carrier phase as a
     * near-random constant, so a config that enables it with NO pre-fit detector gets
     * @c network.cycle_slip.mw_detect armed (the robust choice — it needs no a-priori
     * position).
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkSlipDetectors_(const nlohmann::json& json,
                                               const std::shared_ptr<spdlog::logger>& log) {
        // T0 cycle-slip repair (Increment 2): only meaningful with the carry on.
        network_cycle_slip_turbo_edit_ =
            jval<bool>(json, "network.cycle_slip.turbo_edit", "network_cycle_slip_turbo_edit", false);
        network_cycle_slip_turbo_edit_frac_max_ =
            jval<double>(json, "network.cycle_slip.turbo_edit_frac_max",
                         "network_cycle_slip_turbo_edit_frac_max", 0.15);
        network_cycle_slip_turbo_edit_dn_max_ =
            jval<int>(json, "network.cycle_slip.turbo_edit_dn_max",
                      "network_cycle_slip_turbo_edit_dn_max", 50);
        network_cycle_slip_mw_detect_ =
            jval<bool>(json, "network.cycle_slip.mw_detect",
                       "network_cycle_slip_mw_detect", false);
        // Card L5P: which MW PAIRS the network forms.  Detection-shaping when
        // armed and defaults to today's every-pair detector, so an unarmed run is
        // byte-identical (invariant #6).  Same loader shape as if_phase_td_pairs
        // above: a typo must not silently narrow a detector, so it is named,
        // refused and defaulted loudly.
        {
            const std::string s = jval<std::string>(json, "network.cycle_slip.mw_detect_pairs",
                                                    "network_cycle_slip_mw_detect_pairs",
                                                    std::string{"all"});
            if (s.empty() || s == "all")
                network_config_.cycle_slip_mw_detect_pairs = NetworkMwDetectPairs::All;
            else if (s == "primary")
                network_config_.cycle_slip_mw_detect_pairs = NetworkMwDetectPairs::Primary;
            else {
                network_config_.cycle_slip_mw_detect_pairs = NetworkMwDetectPairs::All;
                log->error("Network (card L5P): network.cycle_slip.mw_detect_pairs = '{}' is not "
                           "one of all|primary — falling back to 'all' (today's every-pair "
                           "wide-lane detector).  'primary' forms only the pair whose two "
                           "signals sit on the constellation's ionosphere-free pin bands, so "
                           "no pair carrying an extra band (GPS band 5 on this posture) becomes "
                           "a candidate, refloats an arc or enters the common-band evidence.", s);
            }
        }
        // IF-phase triple-difference slip DETECTION (this increment): between-
        // satellite, between-epoch ionosphere-free phase OMC.  Detection-only — a
        // flagged IF pair refloats BOTH band ambiguities (pending-reseed) so the
        // carry re-estimates them from data; no MW/GF confirm, no integer repair.
        // OFF is byte-identical; meaningful for products only with the carry on.
        network_cycle_slip_if_phase_td_ =
            jval<bool>(json, "network.cycle_slip.if_phase_td",
                       "network_cycle_slip_if_phase_td", false);
        network_cycle_slip_if_phase_td_k_mad_ =
            jval<double>(json, "network.cycle_slip.if_phase_td_k_mad",
                         "network_cycle_slip_if_phase_td_k_mad", 8.0);
        network_cycle_slip_if_phase_td_floor_frac_ =
            jval<double>(json, "network.cycle_slip.if_phase_td_floor_frac",
                         "network_cycle_slip_if_phase_td_floor_frac", 1.0);
        network_cycle_slip_if_phase_td_min_pool_ =
            jval<int>(json, "network.cycle_slip.if_phase_td_min_pool",
                      "network_cycle_slip_if_phase_td_min_pool", 30);
        network_cycle_slip_if_phase_td_ewma_alpha_ =
            jval<double>(json, "network.cycle_slip.if_phase_td_ewma_alpha",
                         "network_cycle_slip_if_phase_td_ewma_alpha", 0.15);
        network_cycle_slip_if_phase_td_shrink_kappa_ =
            jval<double>(json, "network.cycle_slip.if_phase_td_shrink_kappa",
                         "network_cycle_slip_if_phase_td_shrink_kappa", 20.0);
        // §14.10: how many BAND PAIRS the IF-TD detector evaluates per satellite.
        // Detection-shaping when armed and defaults to the pre-knob single pair,
        // so an unarmed run is byte-identical (invariant #6).  A typo must not
        // silently widen or narrow a detector: warn loudly and take the default.
        {
            const std::string s = jval<std::string>(json, "network.cycle_slip.if_phase_td_pairs",
                                                    "network_cycle_slip_if_phase_td_pairs",
                                                    std::string{"first"});
            if (s.empty() || s == "first")
                network_config_.cycle_slip_if_phase_td_pairs = NetworkIfPhaseTdPairs::First;
            else if (s == "all")
                network_config_.cycle_slip_if_phase_td_pairs = NetworkIfPhaseTdPairs::All;
            else {
                network_config_.cycle_slip_if_phase_td_pairs = NetworkIfPhaseTdPairs::First;
                log->error("Network (§14.10): network.cycle_slip.if_phase_td_pairs = '{}' is not "
                           "one of first|all — falling back to 'first' (today's ONE pair per "
                           "satellite: the first two distinct phase bands).  'all' evaluates "
                           "every distinct band pair, each with its own pool, gradient chain "
                           "and reference election, which is what closes the EQUAL-COUNT trap "
                           "on the pairs the single-pair detector never watched.", s);
            }
        }

        // --- SPEC_slip_relevel_walk §6: the slip re-level response ------------
        // Root defect: a detector's pending-reseed mark is CONSUMED by the
        // PASS-B measurement pre-pass before the leveling-carry inject reads
        // it, so the documented "re-float from data this epoch" never happened
        // — the stale pre-slip level injected at converged weight and the level
        // channels random-walked (~6.5 m ZWD / 6 h at a realistic slip rate).
        //   relevel_free_epoch — latch the marked set where the inject site can
        //     still see it: a detector-marked arc's carry is NOT folded that
        //     epoch (the free epoch the mark always promised).
        //   relevel_delta_bridge — better than freeing when the delta was
        //     MEASURED: shift the carried level by the float per-band solve of
        //     (MW wide-lane integer, IF-TD phase step) so the arc keeps its
        //     level at phase precision; every fallback lands on the free epoch.
        // Both default-OFF, byte-identical OFF, fingerprint-EXEMPT (forward-only
        // response policy — see networkSnapshotFingerprint's exemption list).
        network_cycle_slip_relevel_free_epoch_ =
            jval<bool>(json, "network.cycle_slip.relevel_free_epoch",
                       "network_cycle_slip_relevel_free_epoch", false);
        // --- SPEC_event_harness §12.6/§12.7: the ANCHOR-SLIP HANDOVER ---------
        // Root defect (measured, Study 4): every (sat, signal) phase-bias family
        // is realized at ONE station's arc — the anchor the sticky election
        // elected.  When a slip response FREES that arc, the election keeps the
        // incumbent (the ban that would move it exists, but only the harness
        // injector ever staged one) and the family's gauge is re-realized from
        // the freed arc's one-epoch code re-level: 157 T4 erases on HEALTHY arcs
        // at eight other stations, 10.7 m on the station's own clock, a 9 m step
        // in the satellite's emitted clock.  Armed, the freed anchor's family is
        // banned for THAT epoch only, so the production election picks a
        // successor by its own rules and the CONTINUOUS branch pins it at its own
        // carried value_abs — the same slip then costs 0 erases / 2 mm / 1 cm.
        // Default OFF, byte-identical OFF, fingerprint-EXEMPT (a RESPONSE knob:
        // it changes only what a run DOES after a slip, never the state a
        // snapshot restores).  Interlocks sit in the late band (the phase-datum
        // mode flags parse well after this block).
        network_config_.cycle_slip_anchor_slip_handover =
            jval<bool>(json, "network.cycle_slip.anchor_slip_handover",
                       "network_cycle_slip_anchor_slip_handover", false);
        // --- MW EVIDENCE REFLOAT ---------------------------------------------
        // Root defect (measured on the event harness): the PASS-A MW admission
        // re-uses the T0 integer-REPAIR gate (known integer, frac <=
        // turbo_edit_frac_max) on a DETECT-ONLY path whose consumer — the refloat
        // drain — never applies the integer.  Rounding cleanliness is the wrong
        // question there: a -99 cycle L5 slip flagged at 78x threshold with the
        // integer KNOWN and frac 0.183 (1.07 m on the 5.86 m lane, inside its own
        // 1 sigma) was dropped, as was the same event on a second lane at 8.9x
        // with an unknown integer.  Silently — no counter, no event row.  Armed,
        // a flag is admitted on its own STRENGTH instead: bias-free lane, mature
        // arc, |dev|/threshold above the gate.  Default OFF and byte-identical
        // OFF; the counters are collected either way (an OFF run reports its
        // would-admit population).  Interlock on mw_detect sits in the late band.
        network_config_.cycle_slip_mw_evidence_refloat =
            jval<bool>(json, "network.cycle_slip.mw_evidence_refloat",
                       "network_cycle_slip_mw_evidence_refloat", false);
        network_config_.cycle_slip_mw_evidence_ratio_min =
            jval<double>(json, "network.cycle_slip.mw_evidence_ratio_min",
                         "network_cycle_slip_mw_evidence_ratio_min", 3.0);
        network_config_.cycle_slip_mw_evidence_min_arc_epochs =
            jval<int>(json, "network.cycle_slip.mw_evidence_min_arc_epochs",
                      "network_cycle_slip_mw_evidence_min_arc_epochs", 10);
        // COMMON-BAND SCOPE (SPEC_event_harness §14).  Default OFF is
        // byte-identical: no verdict object is built and no drain consults one.
        network_config_.cycle_slip_common_band_scope =
            jval<bool>(json, "network.cycle_slip.common_band_scope",
                       "network_cycle_slip_common_band_scope", false);
        network_config_.cycle_slip_common_band_min_arc_epochs =
            jval<int>(json, "network.cycle_slip.common_band_min_arc_epochs",
                      "network_cycle_slip_common_band_min_arc_epochs", 10);
        // §22 REFERENCE-ARC CLAUSE, the scope's IF-TD dead zone.  Default OFF is
        // byte-identical: the untested set is never populated.
        network_config_.cycle_slip_common_band_ref_arc =
            jval<bool>(json, "network.cycle_slip.common_band_ref_arc",
                       "network_cycle_slip_common_band_ref_arc", false);
        network_cycle_slip_relevel_delta_bridge_ =
            jval<bool>(json, "network.cycle_slip.relevel_delta_bridge",
                       "network_cycle_slip_relevel_delta_bridge", false);
        network_cycle_slip_relevel_bridge_max_cycles_ =
            jval<double>(json, "network.cycle_slip.relevel_bridge_max_cycles",
                         "network_cycle_slip_relevel_bridge_max_cycles", 200.0);
        network_cycle_slip_relevel_bridge_max_sigma_cyc_ =
            jval<double>(json, "network.cycle_slip.relevel_bridge_max_sigma_cyc",
                         "network_cycle_slip_relevel_bridge_max_sigma_cyc", 0.50);
        network_cycle_slip_relevel_bridge_wl_sigma_floor_cyc_ =
            jval<double>(json, "network.cycle_slip.relevel_bridge_wl_sigma_floor_cyc",
                         "network_cycle_slip_relevel_bridge_wl_sigma_floor_cyc", 0.02);
        network_cycle_slip_relevel_bridge_if_sigma_floor_m_ =
            jval<double>(json, "network.cycle_slip.relevel_bridge_if_sigma_floor_m",
                         "network_cycle_slip_relevel_bridge_if_sigma_floor_m", 0.005);
        // Bridge v2 (pend-and-replace, SPEC §8.2): the corroboration gate — the
        // bridged level must agree with the interim free re-level within this
        // many cycles per band (correct bridges err ≤ 0.5 cyc vs truth, the
        // §8.1 audit's wrong ones 6–113 cyc) — and the pend expiry window
        // (the MW integer arrives 1–2 epochs after the T1 flag).
        network_cycle_slip_relevel_bridge_corrob_cyc_ =
            jval<double>(json, "network.cycle_slip.relevel_bridge_corrob_cyc",
                         "network_cycle_slip_relevel_bridge_corrob_cyc", 3.0);
        network_cycle_slip_relevel_bridge_pend_window_s_ =
            jval<double>(json, "network.cycle_slip.relevel_bridge_pend_window_s",
                         "network_cycle_slip_relevel_bridge_pend_window_s", 120.0);
        // Bridge v3 (the better second equation): the GF-phase-TD float row (E4
        // — closes the (1,2) plane same-epoch without the MW wait; zero
        // integer-hypothesis risk) and the EWL-MW integer slot (E3 — band-3
        // coverage from the instantly-roundable extra-widelane).  E5
        // (multi-epoch MW accumulation) was REFUSED at design review: the
        // λ_IF-amplified cap needs σ_WL ≤ 0.11 cyc ⇒ el ≥ ~48°, above where any
        // fixture slip lives, and a rounded belated float re-imports mw_only.
        network_cycle_slip_relevel_bridge_gf_ =
            jval<bool>(json, "network.cycle_slip.relevel_bridge_gf",
                       "network_cycle_slip_relevel_bridge_gf", false);
        network_cycle_slip_relevel_bridge_ewl_ =
            jval<bool>(json, "network.cycle_slip.relevel_bridge_ewl",
                       "network_cycle_slip_relevel_bridge_ewl", false);
        network_cycle_slip_relevel_bridge_gf_sigma_floor_m_ =
            jval<double>(json, "network.cycle_slip.relevel_bridge_gf_sigma_floor_m",
                         "network_cycle_slip_relevel_bridge_gf_sigma_floor_m", 0.01);
        network_cycle_slip_relevel_bridge_ewl_sigma_floor_cyc_ =
            jval<double>(json, "network.cycle_slip.relevel_bridge_ewl_sigma_floor_cyc",
                         "network_cycle_slip_relevel_bridge_ewl_sigma_floor_cyc", 0.05);
        //   relevel_retap_statistical — the STATISTICAL-refloat half (T2/T3/T4
        //     post-fit RAIM): a refloat with NO measured delta hit an arc whose
        //     level was overwhelmingly CORRECT (the FA population) — keep the
        //     level, re-tap the confidence to a loose SET value so a genuinely
        //     missed slip still loses to the data within an epoch or two.  The
        //     escalation streak falls back to the erase for an arc that keeps
        //     re-tripping (a real missed slip the loose prior cannot shake).
        network_cycle_slip_relevel_retap_statistical_ =
            jval<bool>(json, "network.cycle_slip.relevel_retap_statistical",
                       "network_cycle_slip_relevel_retap_statistical", false);
        //   relevel_retap_corroborate — SPEC_lane_slip_response §3: the keep above
        //     fires ONLY on POSITIVE pre-solve evidence of arc continuity (slip-QC
        //     raw MW/GF: measured AND not stepped — the same tri-state the D5-v3
        //     corroboration gate consumes).  Blanket keep was measured 16 points
        //     WORSE on settled EWL at-own-integer than blanket refloat, and
        //     blanket refloat ~29 points worse than response-virgin — only
        //     evidence separates the flagged population.
        network_cycle_slip_relevel_retap_corroborate_ =
            jval<bool>(json, "network.cycle_slip.relevel_retap_corroborate",
                       "network_cycle_slip_relevel_retap_corroborate", false);
        //   relevel_lane_snap — SPEC_lane_slip_response §7.3.  ⚠ THIS KNOB
        //     CHANGED MEANING.  The SEED-SITE mechanism it used to drive
        //     (v1/v2/v2.1) is DELETED: its only write was an x_lin shift on a
        //     local ambiguity column, which the reduced network solve absorbs
        //     exactly, so no arm of it ever moved the ESTIMATE (the two
        //     default-off prefit screens that gate on H·x_lin are the sole
        //     exception; see the DELETED banner in
        //     MeasurementModel::ensureRequiredStates for the full derivation,
        //     including why its `d` measured −Δe_b/λ_b rather than a lane error).
        //     It now arms the CARRY-DOMAIN lane observatory: the lane response of
        //     the free re-level, measured in local_carry.value_abs — the frame
        //     where the lane actually lives — and MEASURE-ONLY, so an armed run
        //     is byte-identical on every solve artifact.
        //     Hence the two interlocks below, which the old mechanism lacked:
        //     its comment claimed it "works with or without phase_leveling … so
        //     no interlock", and that is exactly what let an inert knob be armed
        //     in two configs and graded as a null result.
        network_cycle_slip_relevel_lane_snap_ =
            jval<bool>(json, "network.cycle_slip.relevel_lane_snap",
                       "network_cycle_slip_relevel_lane_snap", false);
        network_cycle_slip_relevel_retap_var_m2_ =
            jval<double>(json, "network.cycle_slip.relevel_retap_var_m2",
                         "network_cycle_slip_relevel_retap_var_m2", 0.25);
        network_cycle_slip_relevel_retap_streak_max_ =
            jval<int>(json, "network.cycle_slip.relevel_retap_streak_max",
                      "network_cycle_slip_relevel_retap_streak_max", 3);
        // Interlocks (the warn-and-disarm pattern — an arm that cannot move a
        // byte is DISABLED here rather than discovered after a field run):
        if (network_cycle_slip_relevel_retap_statistical_ && !network_phase_leveling_) {
            log->warn("Network: network.cycle_slip.relevel_retap_statistical needs "
                      "network.phase_leveling (its target is the leveling carry) "
                      "— disabled.");
            network_cycle_slip_relevel_retap_statistical_ = false;
        }
        // AFTER the leveling disarm above so it reads the SETTLED base flag.
        if (network_cycle_slip_relevel_retap_corroborate_
            && !network_cycle_slip_relevel_retap_statistical_) {
            log->warn("Network: network.cycle_slip.relevel_retap_corroborate is set "
                      "but relevel_retap_statistical is OFF (or was disarmed) — "
                      "there is no keep branch to gate, so it is INERT this run.");
            network_cycle_slip_relevel_retap_corroborate_ = false;
        }
        // relevel_lane_snap interlocks.  Its object is the LEVELING CARRY, so
        // both are hard requirements — unlike the deleted seed-site mechanism,
        // whose "works with or without phase_leveling, so no interlock" comment
        // is exactly what let an inert knob be armed in two configs and graded
        // as a null result.  Sited AFTER the free_epoch disarm below?  No — it
        // must read the SETTLED base flags, so the free_epoch check is repeated
        // at the end of this interlock band instead (see the rider below).
        if (network_cycle_slip_relevel_lane_snap_ && !network_phase_leveling_) {
            log->warn("Network: network.cycle_slip.relevel_lane_snap needs "
                      "network.phase_leveling (the lane it measures lives in the "
                      "leveling carry's absolute levels) — disabled.");
            network_cycle_slip_relevel_lane_snap_ = false;
        }
        if (network_cycle_slip_relevel_free_epoch_ && !network_phase_leveling_) {
            log->warn("Network: network.cycle_slip.relevel_free_epoch needs "
                      "network.phase_leveling (its target is the leveling carry) "
                      "— disabled.");
            network_cycle_slip_relevel_free_epoch_ = false;
        }
        // AFTER the free_epoch disarm above so it reads the SETTLED base flag:
        // free_ambs IS the observatory's population, so without it every epoch
        // takes the `any_freed` continue and the counters read a structural
        // zero that is indistinguishable from dead wiring (invariant #4).
        if (network_cycle_slip_relevel_lane_snap_
            && !network_cycle_slip_relevel_free_epoch_) {
            log->warn("Network: network.cycle_slip.relevel_lane_snap requires "
                      "network.cycle_slip.relevel_free_epoch (its population is the "
                      "freed set; without it every counter is a structural zero) "
                      "— disabled.");
            network_cycle_slip_relevel_lane_snap_ = false;
        }
        if (network_cycle_slip_relevel_delta_bridge_
            && !network_cycle_slip_relevel_free_epoch_) {
            log->warn("Network: network.cycle_slip.relevel_delta_bridge requires "
                      "network.cycle_slip.relevel_free_epoch (its fallbacks land on "
                      "the free epoch) — disabled.");
            network_cycle_slip_relevel_delta_bridge_ = false;
        }
        // windup_restart_refloat — SPEC_phase_windup_sign §10.4: the WIRED
        // response to an accumulator restart (an exact integer-cycle step on
        // every signal of the satellite, MW-blind because it is band-common).
        // Refloats the stepped arcs at the restart epoch, from the accumulator's
        // OWN verdict.  The restart verdict exists ONLY in accumulated mode, so
        // arming this on a fractional lineage is a structural zero — the
        // armed-but-inert shape invariant #4 names — hence the disarm below.
        network_cycle_slip_windup_restart_refloat_ =
            jval<bool>(json, "network.cycle_slip.windup_restart_refloat",
                       "network_cycle_slip_windup_restart_refloat", false);
        if (network_cycle_slip_windup_restart_refloat_ && !accumulate_phase_windup_) {
            log->warn("Network: network.cycle_slip.windup_restart_refloat needs "
                      "estimation.accumulate_phase_windup (the restart verdict it "
                      "responds to exists only in accumulated mode; fractional "
                      "wind-up wraps instead of restarting) — disabled.");
            network_cycle_slip_windup_restart_refloat_ = false;
        }
        if (network_cycle_slip_windup_restart_refloat_ && !network_phase_leveling_) {
            log->warn("Network: network.cycle_slip.windup_restart_refloat needs "
                      "network.phase_leveling (the stale value it exists to refloat "
                      "is the zero-q leveling carry) — disabled.");
            network_cycle_slip_windup_restart_refloat_ = false;
        }
        // §14.10 IF-TD PAIR WIDTH.  Interlock + arm line, same rationale as the
        // band-scope clauses: without the detector there is no pair to widen, so
        // 'all' would be a silently inert knob rather than a structural zero.
        if (network_config_.cycle_slip_if_phase_td_pairs == NetworkIfPhaseTdPairs::All
            && !network_cycle_slip_if_phase_td_) {
            log->warn("Network: network.cycle_slip.if_phase_td_pairs = 'all' needs "
                      "network.cycle_slip.if_phase_td (there is no IF-TD detector to widen) "
                      "— reset to 'first'.");
            network_config_.cycle_slip_if_phase_td_pairs = NetworkIfPhaseTdPairs::First;
        }
        if (network_config_.cycle_slip_if_phase_td_pairs == NetworkIfPhaseTdPairs::All)
            log->info("Network (IF-TD pairs): ALL — every distinct pair of a satellite's "
                      "tracked phase bands is evaluated (3 on a triple-band arc), each with "
                      "its OWN pooled centre/scale/shrinkage, gradient chain, reference "
                      "election and reset key, so the first pair's decisions are unchanged "
                      "and the extra pairs are a pure addition.  This is what closes the "
                      "EQUAL-COUNT trap on the pairs the single-pair detector never watched: "
                      "two bands slipping by the same integer leave their MW lane silent "
                      "while the iono-free combination moves 0.107 m per cycle.  Run-end "
                      "reports evaluated / flagged / refloated per pair kind.");
        if (network_cycle_slip_relevel_delta_bridge_
            && (!network_cycle_slip_mw_detect_ || !network_cycle_slip_if_phase_td_)) {
            log->warn("Network: network.cycle_slip.relevel_delta_bridge needs BOTH "
                      "cycle_slip.mw_detect (the wide-lane integer) and "
                      "cycle_slip.if_phase_td (the IF phase step) — disabled; "
                      "relevel_free_epoch stays as configured.");
            network_cycle_slip_relevel_delta_bridge_ = false;
        }
        if (network_cycle_slip_relevel_delta_bridge_
            && network_cycle_slip_turbo_edit_) {
            log->warn("Network: network.cycle_slip.relevel_delta_bridge is mutually "
                      "exclusive with cycle_slip.turbo_edit (the T0 integer REPAIR "
                      "already shifts the carried level — a second shift would "
                      "double-apply) — bridge disabled.");
            network_cycle_slip_relevel_delta_bridge_ = false;
        }
        if ((network_cycle_slip_relevel_bridge_gf_
             || network_cycle_slip_relevel_bridge_ewl_)
            && !network_cycle_slip_relevel_delta_bridge_) {
            log->warn("Network: network.cycle_slip.relevel_bridge_gf / "
                      "relevel_bridge_ewl are v3 riders on "
                      "cycle_slip.relevel_delta_bridge, which is off — both "
                      "disabled.");
            network_cycle_slip_relevel_bridge_gf_ = false;
            network_cycle_slip_relevel_bridge_ewl_ = false;
        }

        // --- Slip-QC hardening Cards R / L / G (docs/SPEC_slip_qc_hardening.md) ---
        // Three independent, default-OFF qualifications of decisions the detector
        // arms above already make.  None of them drops a station, satellite or
        // observation (project rule 5) and none derives a threshold from the
        // stochastic model (rule 4).  All-OFF ⇒ byte-identical.
        {
            auto& sq = network_config_.slip_qc;
            // Card R — hardened IF-TD reference-satellite gate (fixes the fixed
            // 0.5·λ_IF, n≥2 gate that re-fired every epoch on a sub-slip drift).
            sq.ref_gate = jval<bool>(json, "network.cycle_slip.if_phase_td_ref_gate",
                                     "network_cycle_slip_if_phase_td_ref_gate", false);
            // REF-FAN (§22.6) — the reference arm on the ADDITIONAL chains.
            // Interlocked below (needs pairs = all AND the empirical gate).
            sq.ref_arm_all = jval<bool>(json, "network.cycle_slip.if_phase_td_ref_arm_all",
                                        "network_cycle_slip_if_phase_td_ref_arm_all", false);
            sq.ref_k = jval<double>(json, "network.cycle_slip.if_phase_td_ref_k",
                                    "network_cycle_slip_if_phase_td_ref_k", 6.0);
            sq.ref_min_n = jval<int>(json, "network.cycle_slip.if_phase_td_ref_min_n",
                                     "network_cycle_slip_if_phase_td_ref_min_n", 4);
            sq.ref_thin_widen =
                jval<double>(json, "network.cycle_slip.if_phase_td_ref_thin_widen",
                             "network_cycle_slip_if_phase_td_ref_thin_widen", 3.0);
            sq.ref_coherence =
                jval<double>(json, "network.cycle_slip.if_phase_td_ref_coherence",
                             "network_cycle_slip_if_phase_td_ref_coherence", 0.5);
            sq.ref_max_consecutive =
                jval<int>(json, "network.cycle_slip.if_phase_td_ref_max_consecutive",
                          "network_cycle_slip_if_phase_td_ref_max_consecutive", 1);
            sq.ref_ewma_alpha =
                jval<double>(json, "network.cycle_slip.if_phase_td_ref_ewma_alpha",
                             "network_cycle_slip_if_phase_td_ref_ewma_alpha", 0.15);
            sq.ref_shrink_kappa =
                jval<double>(json, "network.cycle_slip.if_phase_td_ref_shrink_kappa",
                             "network_cycle_slip_if_phase_td_ref_shrink_kappa", 20.0);
            sq.ref_floor_frac =
                jval<double>(json, "network.cycle_slip.if_phase_td_ref_floor_frac",
                             "network_cycle_slip_if_phase_td_ref_floor_frac", 0.5);
            sq.ref_warmup_abstain =
                jval<bool>(json, "network.cycle_slip.if_phase_td_ref_warmup_abstain",
                           "network_cycle_slip_if_phase_td_ref_warmup_abstain", false);
            sq.ref_elev_norm =
                jval<bool>(json, "network.cycle_slip.if_phase_td_ref_elev_norm",
                           "network_cycle_slip_if_phase_td_ref_elev_norm", false);
            sq.ref_elev_clamp_deg =
                jval<double>(json, "network.cycle_slip.if_phase_td_ref_elev_clamp_deg",
                             "network_cycle_slip_if_phase_td_ref_elev_clamp_deg", 5.0);
            // Card L — LLI corroboration in the MW drain.
            sq.lli_corroborate = jval<bool>(json, "network.cycle_slip.mw_lli_corroborate",
                                            "network_cycle_slip_mw_lli_corroborate", false);
            sq.lli_chatter_frac_hi =
                jval<double>(json, "network.cycle_slip.mw_lli_chatter_frac_hi",
                             "network_cycle_slip_mw_lli_chatter_frac_hi", 0.6);
            sq.lli_chatter_frac_lo =
                jval<double>(json, "network.cycle_slip.mw_lli_chatter_frac_lo",
                             "network_cycle_slip_mw_lli_chatter_frac_lo", 0.3);
            sq.lli_min_epochs = jval<int>(json, "network.cycle_slip.mw_lli_min_epochs",
                                          "network_cycle_slip_mw_lli_min_epochs", 1);
            sq.lli_ewma_alpha = jval<double>(json, "network.cycle_slip.mw_lli_ewma_alpha",
                                             "network_cycle_slip_mw_lli_ewma_alpha", 0.4);
            sq.lli_k_mw = jval<double>(json, "network.cycle_slip.mw_lli_k_mw",
                                       "network_cycle_slip_mw_lli_k_mw", 5.0);
            sq.lli_k_gf = jval<double>(json, "network.cycle_slip.mw_lli_k_gf",
                                       "network_cycle_slip_mw_lli_k_gf", 5.0);
            sq.lli_mw_floor_cyc =
                jval<double>(json, "network.cycle_slip.mw_lli_mw_floor_cyc",
                             "network_cycle_slip_mw_lli_mw_floor_cyc", 0.5);
            sq.lli_gf_floor_m = jval<double>(json, "network.cycle_slip.mw_lli_gf_floor_m",
                                             "network_cycle_slip_mw_lli_gf_floor_m", 0.04);
            sq.lli_step_alpha = jval<double>(json, "network.cycle_slip.mw_lli_step_alpha",
                                             "network_cycle_slip_mw_lli_step_alpha", 0.15);
            // Card G — gap-coincident instrumentation (counters + trace rows only).
            sq.gap_instrument = jval<bool>(json, "network.cycle_slip.gap_instrument",
                                           "network_cycle_slip_gap_instrument", false);
            // Card G RESPONSE (§G3/W1) — the wired invariant-#1 answer to the
            // bridged re-entry Card G measured.  SOLVE-AFFECTING, unlike every
            // other knob in this struct: it marks ambiguities slipped.
            sq.gap_reentry =
                jval<bool>(json, "network.cycle_slip.gap_reentry_relevel",
                           "network_cycle_slip_gap_reentry_relevel", false);
            sq.gap_reentry_lli =
                jval<bool>(json, "network.cycle_slip.gap_reentry_relevel_lli",
                           "network_cycle_slip_gap_reentry_relevel_lli", true);
            sq.gap_reentry_unmeasurable =
                jval<bool>(json, "network.cycle_slip.gap_reentry_relevel_unmeasurable",
                           "network_cycle_slip_gap_reentry_relevel_unmeasurable", false);
            // Card GRS: the same response keyed on the SIGNAL.  A rider of the
            // knob above: the row-level tier owns the de-duplication list this
            // one skips against, so it is inert (and warns) without it.
            sq.gap_reentry_per_signal =
                jval<bool>(json, "network.cycle_slip.gap_reentry_per_signal",
                           "network_cycle_slip_gap_reentry_per_signal", false);
            sq.gap_reentry_per_signal_unmeasurable =
                jval<bool>(json, "network.cycle_slip.gap_reentry_per_signal_unmeasurable",
                           "network_cycle_slip_gap_reentry_per_signal_unmeasurable", true);
            // Card GRK: the arc-level tracker's per-band signal, chosen from the
            // MODEL rather than lexically, so the tracker, the carried ambiguity
            // key and the gap re-entry response name the same signal.
            sq.tracker_modeled_first =
                jval<bool>(json, "network.cycle_slip.tracker_modeled_first",
                           "network_cycle_slip_tracker_modeled_first", false);
            // SIM-ONLY ORACLE slip response (include/slip/NetworkSlipOracle.hpp):
            // shift the carried level by the TRUE integer jump instead of freeing
            // the arc, to measure the CEILING of a level-preserving re-level.
            // SOLVE-AFFECTING; default OFF ⇒ byte-identical.  The truth-root
            // existence check below is what makes it FIELD-INERT by construction.
            sq.oracle_relevel =
                jval<bool>(json, "network.cycle_slip.oracle_relevel",
                           "network_cycle_slip_oracle_relevel", false);
            sq.oracle_truth_root =
                jval<std::string>(json, "network.cycle_slip.oracle_truth_root",
                                  "network_cycle_slip_oracle_truth_root", std::string{});
            sq.oracle_noise_cyc =
                jval<double>(json, "network.cycle_slip.oracle_noise_cyc",
                             "network_cycle_slip_oracle_noise_cyc", 0.0);
            sq.oracle_false_alarm =
                jval<std::string>(json, "network.cycle_slip.oracle_false_alarm",
                                  "network_cycle_slip_oracle_false_alarm",
                                  std::string("keep"));
            // The OTHER half of the 2x2: what to do when truth DOES put a slip in
            // the window.  "repair" (default) is the shipped ceiling; "free"
            // declines so the caller runs today's ordinary response, which paired
            // with false_alarm "keep" isolates the FALSE-ALARM cost on its own.
            sq.oracle_true_slip =
                jval<std::string>(json, "network.cycle_slip.oracle_true_slip",
                                  "network_cycle_slip_oracle_true_slip",
                                  std::string("repair"));
            // Detector-LATENCY window (seconds).  The truth-slip window's lower
            // bound is max(last_applied_s, carry.birth_s, now_s - this), so it is
            // what makes a detector that fires 1-3 epochs LATE repairable instead
            // of mis-counted as a false alarm: a zero-q carried level is not
            // re-set by the intervening solves, so the jump is still un-absorbed
            // even though LocalCarry::last_seen_s has advanced past it.  CLAMPED
            // rather than refused: 0 (same-epoch only) is a legitimate pessimistic
            // rung of the ladder (WARNING - a degenerate one: the lower bound is
            // exclusive, so window 0 admits nothing and the run-end line fires its
            // ARMED-AND-INERT warning), and a negative value is a typo whose only
            // sensible reading is that rung.
            sq.oracle_window_s =
                jval<double>(json, "network.cycle_slip.oracle_window_s",
                             "network_cycle_slip_oracle_window_s", 90.0);
            if (!(sq.oracle_window_s >= 0.0) || !std::isfinite(sq.oracle_window_s)) {
                if (sq.oracle_relevel)
                    log->warn("Network (slip ORACLE): network.cycle_slip.oracle_window_s "
                              "({}) is negative or non-finite — CLAMPED to 0 s, which is the "
                              "DEGENERATE rung: the lower bound is exclusive, so the window "
                              "admits nothing and every actuation will report a false alarm.",
                              sq.oracle_window_s);
                sq.oracle_window_s = 0.0;
            }
            if (sq.oracle_relevel) {
                // HARD errors, not warn-and-disarm: an oracle that silently
                // degrades to "no repair" would report a ceiling of zero and be
                // read as a falsification of the mechanism (the exact
                // armed-and-inert failure standing invariant #4 names — except
                // here the inert arm produces a WRONG SCIENTIFIC VERDICT, so it
                // must not be survivable).
                if (sq.oracle_false_alarm != "keep" && sq.oracle_false_alarm != "free")
                    throw std::runtime_error(
                        "network.cycle_slip.oracle_false_alarm must be \"keep\" or \"free\"");
                if (sq.oracle_true_slip != "repair" && sq.oracle_true_slip != "free")
                    throw std::runtime_error(
                        "network.cycle_slip.oracle_true_slip must be \"repair\" or \"free\"");
                if (sq.oracle_noise_cyc < 0.0)
                    throw std::runtime_error(
                        "network.cycle_slip.oracle_noise_cyc must be >= 0");
                if (sq.oracle_truth_root.empty()
                    || !std::filesystem::exists(sq.oracle_truth_root))
                    throw std::runtime_error(
                        "network.cycle_slip.oracle_relevel is ON but "
                        "network.cycle_slip.oracle_truth_root ('" + sq.oracle_truth_root
                        + "') does not exist.  This is a SIM-ONLY DIAGNOSTIC that reads a "
                          "simulator fixture's truth_slips.csv; it is field-inert by "
                          "construction and this error is that mechanism.");
            }

            // Arming diagnostics: a card whose host arm is off is INERT, and a
            // silent inert card is exactly the "clean log, dead wiring" failure
            // standing invariant #4 exists to prevent.
            if (sq.ref_gate && !network_cycle_slip_if_phase_td_) {
                log->warn("Network (slip-QC card R): network.cycle_slip.if_phase_td_ref_gate "
                          "is set but .if_phase_td is OFF — no IF-phase detector is built, so "
                          "the hardened reference gate is INERT this run.");
                sq.ref_gate = false;
            }
            // §6.14.6 items 1 and 2 are ADJUDICATION refinements INSIDE the
            // hardened gate — evaluateRefGate is the only thing that reads them,
            // and it is only reached when ref_gate is armed.  Left set with the
            // host arm off they would read as posture in a config that cannot
            // act on them.
            if ((sq.ref_warmup_abstain || sq.ref_elev_norm) && !sq.ref_gate) {
                log->warn("Network (slip-QC card R): if_phase_td_ref_{}{}{} set but "
                          ".if_phase_td_ref_gate is OFF — the legacy fixed floor IS the "
                          "decision, so the warm-up abstention and the elevation-normalized "
                          "scale are both INERT this run.",
                          sq.ref_warmup_abstain ? "warmup_abstain" : "",
                          (sq.ref_warmup_abstain && sq.ref_elev_norm) ? " / " : "",
                          sq.ref_elev_norm ? "elev_norm" : "");
                sq.ref_warmup_abstain = false;
                sq.ref_elev_norm      = false;
            }
            // REF-FAN (§22.6): the reference arm on the ADDITIONAL chains.  TWO
            // hard prerequisites, both structural rather than stylistic:
            //  * `if_phase_td_pairs = all` — with one chain per satellite there
            //    are no additional chains, so the knob is a dead flag, not a
            //    behaviour (standing invariant #4: never leave inert posture in
            //    a config that reads as armed);
            //  * `if_phase_td_ref_gate` — WITHOUT the empirical gate the LEGACY
            //    candidate condition |median| > 0.5·λ_IF is the decision, and on
            //    a wide combination that condition is met by NOISE: (L2W,L5Q)
            //    has IF coefficients 12.26 / −11.26 (noise amplification 16.6)
            //    against λ_IF = 0.1248 m, measured at 5 351 candidates / 64 fires
            //    over a quiet 70-epoch 60-station null (§14.10.3).  That is the
            //    flood the ONE-arm rule exists to prevent and it must never gate
            //    a wide chain again — this card re-opens the arm only behind the
            //    per-chain EMPIRICAL scale, which tracks each chain's own
            //    dispersion.  The detector refuses the combination a second time.
            if (sq.ref_arm_all
                && (network_config_.cycle_slip_if_phase_td_pairs != NetworkIfPhaseTdPairs::All
                    || !sq.ref_gate)) {
                log->warn("Network (§22.6 REF-FAN): network.cycle_slip.if_phase_td_ref_arm_all "
                          "needs BOTH .if_phase_td_pairs = 'all' (currently '{}' — with one "
                          "chain per satellite there is no ADDITIONAL chain to arm) and "
                          ".if_phase_td_ref_gate (currently {}) — DISARMED.  The gate is not "
                          "optional: without it the LEGACY candidate condition "
                          "|median| > 0.5·lambda_IF is the decision, and on the wide "
                          "combinations that condition is met by NOISE ((L2W,L5Q): noise "
                          "amplification 16.6, lambda_IF 0.125 m — 5 351 candidates / 64 fires "
                          "on a quiet 70-epoch null, §14.10.3).  The legacy condition must "
                          "never gate a wide chain again.",
                          network_config_.cycle_slip_if_phase_td_pairs
                                  == NetworkIfPhaseTdPairs::All ? "all" : "first",
                          sq.ref_gate ? "ON" : "OFF");
                sq.ref_arm_all = false;
            }
            if (sq.ref_arm_all)
                log->info("Network (§22.6 REF-FAN): IF-TD reference arm on ALL chains ARMED — "
                          "an ADDITIONAL chain with an OWN-ELECTED reference runs the arm on "
                          "that chain through the EMPIRICAL gate (its own ref_scale EWMA, its "
                          "own lambda_IF); one with an ADOPTED reference inherits the first "
                          "chain's verdict for that satellite this epoch (reset of THIS pair's "
                          "reference arc, median rebase by THIS chain's own median on a "
                          "FireRebase).  Closes the reference-slip FAN-OUT: a slip ON an "
                          "additional chain's reference reads abs_dev = 0 on its own rows by "
                          "construction and is otherwise attributed to every OTHER satellite "
                          "of that chain (field seed: six false refloats on G11/G25/G32 plus a "
                          "13.7 m phase-bias datum re-realization from one −99 cyc L5X slip on "
                          "GOPE00CZE/G24).  Run-end reports opportunities / candidates / own "
                          "fires / inherited fires / fan-out rows suppressed PER CHAIN.");
            if (sq.lli_corroborate && !network_cycle_slip_mw_detect_) {
                log->warn("Network (slip-QC card L): network.cycle_slip.mw_lli_corroborate is "
                          "set but .mw_detect is OFF — the LLI arm it qualifies never runs, so "
                          "the corroboration gate is INERT this run.");
                sq.lli_corroborate = false;
            }
            if (sq.lli_chatter_frac_lo > sq.lli_chatter_frac_hi) {
                log->warn("Network (slip-QC card L): mw_lli_chatter_frac_lo ({:.2f}) exceeds "
                          "_hi ({:.2f}) — the hysteresis band is inverted and the credibility "
                          "state would chatter.  Clamping _lo to _hi.",
                          sq.lli_chatter_frac_lo, sq.lli_chatter_frac_hi);
                sq.lli_chatter_frac_lo = sq.lli_chatter_frac_hi;
            }
            if (sq.ref_gate)
                log->info("Network (slip-QC card R): hardened IF-TD reference gate ON — fire "
                          "only when |median(non-ref ΔΔ)| > max({:.1f}·s_eff, {:.2f}·λ_IF) AND "
                          "the family agrees (MAD ≤ {:.2f}·|median|); thin families (< {} DDs) "
                          "widen the physical floor {:.1f}× and are counted; at most {} "
                          "consecutive fires per reference before a forced re-selection.",
                          sq.ref_k, sq.ref_floor_frac,
                          sq.ref_coherence, sq.ref_min_n, sq.ref_thin_widen,
                          sq.ref_max_consecutive);
            if (sq.ref_gate && (sq.ref_warmup_abstain || sq.ref_elev_norm))
                log->info("Network (slip-QC card R, §6.14.6): {}{}{} — the reference arm's "
                          "own noise is {:.0f}% of the tested median and no family size "
                          "averages it away, so the bar is rebuilt from the elevation it "
                          "actually depends on rather than from the family's raw scatter.",
                          sq.ref_warmup_abstain
                              ? fmt::format("ABSTAIN until the empirical scale holds {:.0f} "
                                            "sample(s) (no legacy-floor fallback while cold)",
                                            sq.ref_shrink_kappa)
                              : std::string{},
                          (sq.ref_warmup_abstain && sq.ref_elev_norm) ? "; " : "",
                          sq.ref_elev_norm
                              ? fmt::format("ELEVATION-NORMALIZED scale (EWMA stores "
                                            "disp·⟨sin el⟩, bar = {:.1f}·hypot(s_z/sin el_ref, "
                                            "1.2533·s_raw/√n), elevation clamped at {:.1f}°)",
                                            sq.ref_k, sq.ref_elev_clamp_deg)
                              : std::string{},
                          88.0);
            if (sq.lli_corroborate)
                log->info("Network (slip-QC card L): LLI corroboration ON — an assertion acts "
                          "when an empirical MW step (> max({:.1f}·σ̂, {:.2f} WL cyc)) or GF step "
                          "(> max({:.1f}·σ̂, {:.3f} m)) confirms it, or while the station's "
                          "LLI-set-fraction EWMA (α={:.2f}) stays credible (demote > {:.2f}, "
                          "promote < {:.2f}, arm after {} epoch(s)).  Suppressed assertions are "
                          "emitted with action=suppressed and counted — never dropped silently.",
                          sq.lli_k_mw, sq.lli_mw_floor_cyc, sq.lli_k_gf, sq.lli_gf_floor_m,
                          sq.lli_ewma_alpha, sq.lli_chatter_frac_hi, sq.lli_chatter_frac_lo,
                          sq.lli_min_epochs);
            // PRIOR-RESIDUAL CENSUS INC-3 auto-arm (docs/SPEC_prior_residual_census.md
            // §4).  loadNetworkCarries_ resolved prior_residual_corroborate one helper
            // earlier; the flag it needs is parsed HERE, so per this section's load-order
            // rule the forcing lives at this end of the chain.  gap_instrument is what
            // makes networkQualifyRawObservables run and fill the per-(station,
            // satellite) raw MW/GF verdicts the join reads — without it the whole
            // contingency table collapses to the 'no_evidence' column and the card's
            // headline b-cell is structurally unmeasurable.  Deliberately NOT
            // mw_lli_corroborate: that is a DECISION card and would perturb the very
            // population being measured (the same reason D5 v3's loader picks this one).
            if (network_config_.prior_residual_corroborate && !sq.gap_instrument) {
                sq.gap_instrument = true;
                log->warn("Network (prior-residual census INC-3): AUTO-ARMING "
                          "network.cycle_slip.gap_instrument — the corroboration join reads the "
                          "raw MW/GF step verdicts and nothing else in this config would have "
                          "computed them, so every contingency cell would have landed in the "
                          "'no_evidence' column.  gap_instrument is instrumentation-only and "
                          "verified byte-identical; mw_lli_corroborate is deliberately NOT armed "
                          "(a decision card would perturb what is being measured).");
            }
            if (sq.gap_instrument)
                log->info("Network (slip-QC card G): gap-coincident instrumentation ON — "
                          "counting arc resumptions across a tracking break, whether the raw "
                          "MW/GF level shifted, and whether the arc re-entered with BRIDGED "
                          "carry state.  Instrumentation only: zero mechanism change.");
        }
        // Carrying carrier-phase ambiguities as zero-process-noise random constants is only
        // sound if an un-screened cycle slip cannot freeze a wrong value into the shared clock:
        // a slip makes the post-slip phase self-consistent with the SLIPPED integer, so the
        // stiff carry does not fight it — it locks it in.  Carrying phase therefore REQUIRES at
        // least one pre-fit slip detector (mw_detect, if_phase_td, or the turbo_edit repair,
        // which also runs the MW detector).  If a config enables phase-leveling with none, arm
        // network.cycle_slip.mw_detect — the ROBUST default: the wide-lane + LLI detector needs
        // NO a-priori position (works even when a station sits far from its reference), whereas
        // IF-phase TD relies on accurate coordinates (fine in-network, where they are fixed at
        // near-truth, but not in general).  Both are ionosphere-free and valid at the 30 s
        // product rate (unlike the MW+GF integer REPAIR).  Mirrors the raim_within_epoch ⟹
        // raim+attribution auto-arm below.  IF-phase TD stays an explicit opt-in for the
        // complementary geometric-jump coverage where coordinates are trusted.
        if (network_phase_leveling_ && !network_cycle_slip_mw_detect_
            && !network_cycle_slip_if_phase_td_ && !network_cycle_slip_turbo_edit_) {
            log->warn("Network: network.phase_leveling carries carrier-phase ambiguities but no "
                      "pre-fit slip detector is enabled — an un-screened slip would freeze into "
                      "the shared clock.  Enabling network.cycle_slip.mw_detect (wide-lane + LLI, "
                      "needs no a-priori position).  Add network.cycle_slip.if_phase_td for "
                      "complementary geometric-jump coverage where coordinates are trusted.");
            network_cycle_slip_mw_detect_ = true;
        }

        // Datum-station slip detection + network re-reference
        // (docs/SPEC_synthetic_minimum_network.md §9.7.10).  Under the legacy
        // single-pivot datum every slip loop starts at i_lo = 1, so the datum
        // station — the one receiver whose ambiguity rides inside every emitted
        // phase bias — has NO slip exposure at all (§9.7.4, proven by roster
        // reorder).  This arms MW/LLI detection at stations[0] with the
        // mechanism-specific response: not a local refloat (the pivot's
        // ambiguities are gauge-pinned) but a per-satellite network
        // re-reference — hard skip-stash of the sat's PhaseBias carry, an
        // all-station ambiguity reseed aligned to the epoch the bias re-seeds,
        // and a per-sat disc mark at the next emit.  Parsed AFTER the
        // invariant-#1 auto-arm above so a leveling-auto-armed mw_detect
        // satisfies the requirement.  Default OFF = byte-identical.
        network_cycle_slip_datum_station_detect_ =
            jval<bool>(json, "network.cycle_slip.datum_station_detect",
                       "network_cycle_slip_datum_station_detect", false);
        if (network_cycle_slip_datum_station_detect_ && !network_cycle_slip_mw_detect_) {
            log->warn("Network: network.cycle_slip.datum_station_detect needs the MW/LLI "
                      "detection drain (network.cycle_slip.mw_detect) as its vehicle and it "
                      "is not armed — disabling datum_station_detect.  Arm mw_detect (or "
                      "network.phase_leveling, which auto-arms it) first.");
            network_cycle_slip_datum_station_detect_ = false;
        }

        // Card L5P: the pair scope is a scope ON the MW drain, so without that
        // drain it is inert.  Warn rather than refuse (the B5D interlock
        // precedent): the knob changes nothing on its own, and silently accepting
        // it would let an arm read as armed while the detector it scopes never
        // ran.  Checked AFTER the invariant-#1 auto-arm above, so a leveling run
        // that auto-armed mw_detect never trips it.
        if (network_config_.cycle_slip_mw_detect_pairs == NetworkMwDetectPairs::Primary
            && !network_cycle_slip_mw_detect_) {
            log->warn("Network (card L5P): network.cycle_slip.mw_detect_pairs = 'primary' is "
                      "INERT without network.cycle_slip.mw_detect — it scopes the wide-lane "
                      "detector's pairs and that detector is not armed, so no pair is formed "
                      "either way.");
        }
    }

    // =================================================================
    //  network subsection helper: loadNetworkQcTiers_
    // =================================================================
    /**
     * @brief Post-fit / within-epoch QC tiers: RAIM, clock monitor, SH health gate, adaptive iono q.
     *
     * Holds the tier auto-couplings (a withhold ceiling pulls in the within-epoch tier, which
     * pulls in post-fit RAIM + attribution; WE-CI pulls in the tier it extends) and the
     * CB-excursion reinit event response.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkQcTiers_(const nlohmann::json& json,
                                         const std::shared_ptr<spdlog::logger>& log) {
        // Alias preamble: the body below reads the driver local it was written
        // against.  network_service_mode_ is written once, at the top of the
        // section, and never again — so this is the value the un-split code saw.
        const bool service_mode = network_service_mode_;

        // Post-fit network RAIM (Increment 3, T2): the safety net for the zero-q carry.
        network_raim_ =
            jval<bool>(json, "network.raim", "network_raim", false);
        network_raim_k_sigma_ =
            jval<double>(json, "network.raim_k_sigma", "network_raim_k_sigma", 6.0);
        network_raim_sigma0_floor_ =
            jval<double>(json, "network.raim_sigma0_floor", "network_raim_sigma0_floor", 1.0);
        network_raim_min_stations_ =
            jval<int>(json, "network.raim_min_stations", "network_raim_min_stations", 3);
        network_raim_arc_persist_epochs_ =
            jval<int>(json, "network.raim_arc_persist_epochs", "network_raim_arc_persist_epochs", 1);
        // T2d: isolated-arc refloat maturity gate (default OFF = byte-identical).
        network_raim_refloat_maturity_gate_ =
            jval<bool>(json, "network.raim_refloat_maturity_gate",
                       "network_raim_refloat_maturity_gate", false);
        network_raim_refloat_maturity_epochs_ =
            jval<int>(json, "network.raim_refloat_maturity_epochs",
                      "network_raim_refloat_maturity_epochs", 0);
        if (network_raim_refloat_maturity_epochs_ < 0)
            network_raim_refloat_maturity_epochs_ = 0;
        network_signal_min_station_fraction_ =
            jval<double>(json, "network.signal_min_station_fraction", "network_signal_min_station_fraction", 0.0);
        network_phase_bias_min_station_fraction_ =
            jval<double>(json, "network.phase_bias_min_station_fraction", "network_phase_bias_min_station_fraction", 0.0);
        // Per-station analogue of the phase-bias coverage float-gate above: withhold a
        // station whose receiver antenna has no ATX model match (zero receiver PCO/PCV
        // → uncalibrated carrier phase) from every satellite phase-bias state.  Default
        // OFF = every station contributes (byte-identical).
        network_phase_bias_exclude_unmatched_antenna_ =
            jval<bool>(json, "network.phase_bias_exclude_unmatched_antenna",
                       "network_phase_bias_exclude_unmatched_antenna", false);
        // Policy backstop (invariant #6: default OFF): hard-drop any station whose
        // receiver antenna still has no ATX match AFTER the always-on radome→NONE
        // fallback (a genuinely unknown antenna TYPE = unmodelled receiver PCO/PCV,
        // a systematic in every obs feeding the shared clocks/biases).
        network_exclude_unmodeled_antenna_stations_ =
            jval<bool>(json, "network.exclude_unmodeled_antenna_stations",
                       "network_exclude_unmodeled_antenna_stations", false);
        // Card UPO: the roster's configured coordinates are MONUMENT coordinates and the
        // observations refer to the antenna.  Armed, each station's position_ecef is
        // reduced to its antenna reference point at setup through the same
        // Coordinates::markerToArp the held-out consumer's truth has used since
        // 2026-07-29.  Default OFF = byte-identical; the eccentricities are read and
        // reported in BOTH states, so an OFF run states the size of the defect.
        network_station_marker_to_arp_ =
            jval<bool>(json, "network.station_marker_to_arp",
                       "network_station_marker_to_arp", false);
        // Card SID phase 1 (docs/refound/SID_signal_identity_2026-09-22.md section 5):
        // the per-station SIGNAL ROSTER and the ARC LEDGER, built once per epoch and
        // read by NOTHING but the disagreement counters and the trace.  Default OFF;
        // ON is byte-identical on the solve, the wire and every graded trace, which
        // is the phase's own bar (standing invariant 6 in both knob states).
        network_signal_roster_ =
            jval<bool>(json, "network.signal_roster", "network_signal_roster", false);
        // Card SID phase 2 (section 6 of the same card): the arc-level slip tracker
        // becomes the roster's first READER.  Default OFF; the interlock against a
        // roster that was never built, and the supersession notice against card GRK's
        // knob, are raised in the posture block at the end of this section (the slip-QC
        // knobs parse well below this line).
        network_roster_tracker_ =
            jval<bool>(json, "network.roster_tracker", "network_roster_tracker", false);
        // Card SID phase 3 (section 7 of the same card): the gap RE-ENTRY RESPONSE
        // keys on the ledger's ADMITTED lifecycle instead of on the raw tiers'
        // resumptions.  Default OFF; ARMED IS SOLVE-AFFECTING (it changes which
        // carried levels survive a break), so it is graded as a behaviour change.
        // The interlock against a missing observatory or a missing roster read is
        // raised in the posture block at the end of this section.
        network_ledger_reentry_ =
            jval<bool>(json, "network.ledger_reentry", "network_ledger_reentry", false);
        {
            // The policy for an arc no raw tier saw this epoch.  Parsed as a string
            // so a typo is a loud fallback to the default rather than a silent
            // integer; the same shape network.trace_dd_fractional_ref uses.
            const std::string nv = jval<std::string>(
                json, "network.ledger_reentry_no_verdict",
                "network_ledger_reentry_no_verdict", std::string("bridge"));
            if (nv == "refloat") {
                network_ledger_reentry_no_verdict_ = LedgerReentryNoVerdict::Refloat;
            } else {
                network_ledger_reentry_no_verdict_ = LedgerReentryNoVerdict::Bridge;
                if (nv != "bridge" && !nv.empty())
                    log->warn("Network producer (card SID phase 3): "
                              "network.ledger_reentry_no_verdict = '{}' is not one of "
                              "'bridge' or 'refloat'.  Falling back to 'bridge' (the "
                              "default: a no-verdict re-entry is left to the PASS-B "
                              "inject and only counted).", nv);
            }
        }
        // Card SID phase 4a (section 9 of the same card): the REASON-MASK WRITERS at
        // every screen that removes a modelled row, a satellite or a station, and
        // the anchor election READ against the ledger.  Default OFF; ON is an
        // OBSERVATORY and byte-identical on the solve, which is 4a's own bar.  The
        // interlock against a missing observatory is raised in the posture block at
        // the end of this section.
        network_ledger_screens_ =
            jval<bool>(json, "network.ledger_screens", "network_ledger_screens", false);
        // Card SID phase 4b: the election treats a RETURN as a return.  Default OFF;
        // ARMED IS SOLVE-AFFECTING (the election's memory across a gap changes what
        // the carried anchor means), so it is graded as a behaviour change and it is
        // fingerprinted.  Interlock against a missing 4a below.
        network_ledger_anchor_ =
            jval<bool>(json, "network.ledger_anchor", "network_ledger_anchor", false);
        // Card SID phase 5a: the EMITTER's per-family bookkeeping read against the
        // ledger, and the producer's DECLARATION of its signal roster on the wire.
        // Default OFF; ON is an OBSERVATORY: the solve and every C / B / P / E
        // record are byte-identical, and the only difference in products.txt is
        // the N declaration block.  The interlock against a missing observatory is
        // raised in the posture block at the end of this section.
        network_ledger_emit_ =
            jval<bool>(json, "network.ledger_emit", "network_ledger_emit", false);
        network_raim_attribution_ =
            jval<bool>(json, "network.raim_attribution", "network_raim_attribution", false);
        // POOL RE-HOST (docs/SPEC_raim_pool_rehost.md).  Parsed here with the rest of the
        // RAIM family; the INERT guards live further down, because they read
        // network_carry_invariant_, which is not populated until the eq34 section.
        network_raim_carry_invariant_ =
            jval<bool>(json, "network.raim_carry_invariant",
                       "network_raim_carry_invariant", false);
        network_raim_carry_invariant_refloat_ =
            jval<bool>(json, "network.raim_carry_invariant_refloat",
                       "network_raim_carry_invariant_refloat", false);
        network_raim_within_epoch_ =
            jval<bool>(json, "network.raim_within_epoch", "network_raim_within_epoch", false);
        network_raim_sigma0_trigger_ =
            jval<double>(json, "network.raim_sigma0_trigger", "network_raim_sigma0_trigger", 3.0);
        network_raim_withhold_sigma0_ =
            jval<double>(json, "network.raim_withhold_sigma0", "network_raim_withhold_sigma0", 0.0);
        // R11/R1: a σ̂0 withhold ceiling in (0,1] is pathological — a healthy epoch's reduced-
        // χ² √ (the σ̂0 the gate compares against) sits at ~1, so a ceiling at or below 1 would
        // WITHHOLD essentially every warm armed epoch (total availability loss), the opposite
        // of the intended rare escape-hatch.  A sensible value is ~2× network.raim_sigma0_trigger.
        // Treat it as a config error: warn and disable (0 = emit-anyway, the documented default)
        // rather than silently withholding the whole run.  Done before the within-epoch coupling
        // below so a clamped-to-0 ceiling does not spuriously arm within-epoch adaptation.
        if (network_raim_withhold_sigma0_ != 0.0 && network_raim_withhold_sigma0_ <= 1.0) {
            log->warn("Network: network.raim_withhold_sigma0 {} is <= 1 — σ̂0 is floored at 1, so "
                      "a ceiling at or below 1 would withhold most warm epochs (and a negative "
                      "value is meaningless).  Disabling it (set ~2x network.raim_sigma0_trigger, "
                      "e.g. 6, to use it).",
                      network_raim_withhold_sigma0_);
            network_raim_withhold_sigma0_ = 0.0;
        }
        // Withhold-era release policy (SPEC_withhold_era_release §3) — two independent
        // default-OFF responders on the A4 path.  Parsed here beside their gate; the
        // cross-knob interlocks (withhold ceiling present, relevel_free_epoch for the
        // erase half) live at the END of the network section per the Card-1g order rule
        // (cycle_slip and the service-mode arming block both parse after this site).
        network_raim_withhold_adapt_escalate_ =
            jval<int>(json, "network.raim_withhold_adapt_escalate",
                      "network_raim_withhold_adapt_escalate", 0);
        if (network_raim_withhold_adapt_escalate_ < 0 ||
            (network_raim_withhold_adapt_escalate_ > 0 &&
             network_raim_withhold_adapt_escalate_ <= 5)) {
            log->warn("Network (withhold-era): network.raim_withhold_adapt_escalate {} is not "
                      "above the stock within-epoch budget (5) — an \"escalation\" at or below "
                      "it is a no-op that would read as armed.  Disabling (0).  Set > 5 "
                      "(the fld63 arm pre-registered 12).",
                      network_raim_withhold_adapt_escalate_);
            network_raim_withhold_adapt_escalate_ = 0;
        }
        network_raim_withhold_free_erase_ =
            jval<bool>(json, "network.raim_withhold_free_erase",
                       "network_raim_withhold_free_erase", false);
        // RTA-10: per-constellation carve-out of the A4 σ̂0 withhold.  Activates ONLY at a
        // tripped withhold (raim_withhold_sigma0 > 0), so without that ceiling it is a silent
        // no-op — warn rather than auto-arm (choosing a σ̂0 ceiling is a numeric decision the
        // user must make; there is no safe default to inject).
        network_per_const_withhold_ =
            jval<bool>(json, "network.per_const_withhold", "network_per_const_withhold", false);
        network_per_const_withhold_min_rows_ =
            jval<int>(json, "network.per_const_withhold_min_rows",
                      "network_per_const_withhold_min_rows", 20);
        if (network_per_const_withhold_min_rows_ < 1) {
            log->warn("Network (RTA-10): network.per_const_withhold_min_rows {} < 1 clamped to 1 "
                      "(a constellation needs at least one standardized-residual row to be judged).",
                      network_per_const_withhold_min_rows_);
            network_per_const_withhold_min_rows_ = 1;
        }
        // P4: suppressed under service_mode — the bundle arms raim_withhold_sigma0
        // (2x trigger) in the arming block below, so the "silent no-op" claim would
        // be false by the end of the load.
        if (network_per_const_withhold_ && network_raim_withhold_sigma0_ <= 0.0 && !service_mode)
            log->warn("Network (RTA-10): network.per_const_withhold is on but "
                      "network.raim_withhold_sigma0 is 0 (off) — the carve-out only runs when the "
                      "A4 σ̂0 withhold trips, so it is a silent no-op.  Set a withhold ceiling "
                      "(~2x network.raim_sigma0_trigger) to activate it.");
        if (network_per_const_withhold_ && network_raim_withhold_sigma0_ > 0.0)
            log->info("Network (RTA-10): per-constellation σ̂0 carve-out ON — a tripped withhold "
                      "(ceiling {:.2f}) publishes each constellation whose own σ̂0_c passes the "
                      "same ceiling with >= {} rows, and prunes the rest (whole-epoch withhold "
                      "only when NO constellation passes).",
                      network_raim_withhold_sigma0_, network_per_const_withhold_min_rows_);
        network_raim_surgical_station_ =
            jval<bool>(json, "network.raim_surgical_station", "network_raim_surgical_station", true);
        network_raim_station_drop_bias_ =
            jval<double>(json, "network.raim_station_drop_bias", "network_raim_station_drop_bias", 1.5);
        network_raim_redundancy_normalize_ =
            jval<bool>(json, "network.raim_redundancy_normalize", "network_raim_redundancy_normalize", false);
        // QUEUE item 4 R2/R4 (SPEC_t3_null_derivation §6): both measure-only, both
        // default-OFF.  Parsed here beside their statistic; the raim / trace_flag_rates
        // dependencies are interlocked at the END of the network section (Card-1g
        // order rule — trace_flag_rates parses in the observability block).
        network_raim_station_factor_ =
            jval<bool>(json, "network.raim_station_factor", "network_raim_station_factor", false);
        network_raim_station_factor_tau_s_ =
            jval<double>(json, "network.raim_station_factor_tau_s",
                         "network_raim_station_factor_tau_s", 1800.0);
        if (network_raim_station_factor_tau_s_ <= 0.0) {
            log->warn("Network (R2): network.raim_station_factor_tau_s {} <= 0 — the slow "
                      "channel must TRACK (SPEC_t3_null_derivation §12.1b); reset to 1800.",
                      network_raim_station_factor_tau_s_);
            network_raim_station_factor_tau_s_ = 1800.0;
        }
        network_raim_station_factor_ratio_ =
            jval<double>(json, "network.raim_station_factor_ratio",
                         "network_raim_station_factor_ratio", 6.0);
        if (network_raim_station_factor_ratio_ <= 1.0) {
            log->warn("Network (R2): network.raim_station_factor_ratio {} <= 1 would flag "
                      "at-or-below the network median — reset to 6.0 (above the measured "
                      "4.7x model-error spread).",
                      network_raim_station_factor_ratio_);
            network_raim_station_factor_ratio_ = 6.0;
        }
        network_raim_rate_hygiene_ =
            jval<bool>(json, "network.raim_rate_hygiene", "network_raim_rate_hygiene", false);
        // LV-6 census (SPEC_level_authority_internal §3) — measure-only, no
        // interlocks beyond the reject bands: it arms the CS-GF advance itself
        // and depends on no other card.
        network_level_authority_csgf_ =
            jval<bool>(json, "network.level_authority_csgf",
                       "network_level_authority_csgf", false);
        network_level_authority_min_n_ =
            jval<int>(json, "network.level_authority_min_n",
                      "network_level_authority_min_n", 10);
        if (network_level_authority_min_n_ < 2) {
            log->warn("Network (LV-6): network.level_authority_min_n {} < 2 — a 1-sample "
                      "CS-GF state is raw code; reset to 10.",
                      network_level_authority_min_n_);
            network_level_authority_min_n_ = 10;
        }
        network_level_authority_min_los_ =
            jval<int>(json, "network.level_authority_min_los",
                      "network_level_authority_min_los", 4);
        // LV-6 G2 support (SPEC_level_authority_internal §4): the census's own
        // per-LOS pierce points, so a reference field (GIM in the field, truth in
        // sim) can be pushed through the IDENTICAL sampling rather than a
        // re-derived one.  Trace-only, its own file; INERT without the census.
        network_level_authority_los_dump_ =
            jval<bool>(json, "network.level_authority_los_dump",
                       "network_level_authority_los_dump", false);
        if (network_level_authority_los_dump_ && !network_level_authority_csgf_) {
            log->warn("Network (LV-6): network.level_authority_los_dump is set but "
                      ".level_authority_csgf is OFF — there is no census to dump the "
                      "pierce points of, so it is INERT this run.");
            network_level_authority_los_dump_ = false;
        }
        if (network_level_authority_min_los_ < 1) {
            log->warn("Network (LV-6): network.level_authority_min_los {} < 1 reset to 4.",
                      network_level_authority_min_los_);
            network_level_authority_min_los_ = 4;
        }
        if (network_level_authority_csgf_)
            log->info("Network (LV-6 level-authority census, SPEC_level_authority_internal): "
                      "per-epoch fleet CS-GF reduction (min_n {}, min_los {}) — MEASURE-ONLY, "
                      "folds nothing; the fold is a future card gated on this census's "
                      "tracking verdict.  ⚠ A/B pairs must pin cycle_slip.gap_instrument in "
                      "BOTH arms (the cardsArmed() side-effect, SPEC §5.3).",
                      network_level_authority_min_n_, network_level_authority_min_los_);
        if (network_raim_station_drop_bias_ < 1.0) {
            log->warn("Network: network.raim_station_drop_bias {} < 1 clamped to 1 (a bias below 1 "
                      "would PREFER whole-station exclusion, the opposite of its purpose).",
                      network_raim_station_drop_bias_);
            network_raim_station_drop_bias_ = 1.0;
        }
        // A4/P0-4: the withhold ceiling reuses the WITHIN-EPOCH σ̂0 (published only when the
        // within-epoch adaptation runs — otherwise sigma0_adapt stays 0 and the gate never
        // fires).  So raim_withhold_sigma0 is a silent no-op without network.raim_within_epoch.
        // Enable that tier (which then pulls in raim + attribution just below) so the
        // diffuse-contamination escape-hatch is actually active, and say so.
        if (network_raim_withhold_sigma0_ > 0.0 && !network_raim_within_epoch_) {
            log->warn("Network: network.raim_withhold_sigma0 > 0 needs the within-epoch σ̂0 — "
                      "enabling network.raim_within_epoch so the withhold escape-hatch is active "
                      "(it is the tier that computes the σ̂0 the ceiling compares against).");
            network_raim_within_epoch_ = true;
        }
        // The within-epoch adaptation REFLOATS (reinitializes) a contaminated sat/arc's carried
        // ambiguity in-place — the re-solve lets it find its new value and the harvest re-stashes
        // it, so within-epoch is self-contained (no lock-in even at zero process noise).  The
        // post-fit RAIM prominence-attribution refloat is the COMPLEMENTARY tier: it catches
        // slips too small to trip the within-epoch σ̂0 trigger yet large enough to standardized-
        // residual-flag (e.g. a single-satellite spike that does not move the robust network σ̂0).
        // They are the two halves of one backstop, so enable both together for complete coverage.
        if (network_raim_within_epoch_ && (!network_raim_ || !network_raim_attribution_)) {
            log->warn("Network: enabling network.raim + network.raim_attribution alongside "
                      "network.raim_within_epoch — the post-fit attribution refloat is the "
                      "complementary tier that catches slips below the within-epoch σ̂0 trigger.");
            network_raim_ = true;
            network_raim_attribution_ = true;
        }
        // WE-CI: extend the within-epoch screening to the CODE + IONO-CONSTRAINT
        // row pools.  Those rows historically had NO screening statistic in any
        // tier (the P1-4 pass excludes SH-touching rows; the within-epoch pool is
        // carried-ambiguity phase rows only) — the DOY-129 19:26:00 SH detonation
        // entered through exactly them.  Follows the A4 precedent: arming the
        // extension pulls in the tier it extends.
        network_raim_we_code_iono_ =
            jval<bool>(json, "network.raim_we_code_iono", "network_raim_we_code_iono", false);
        network_raim_we_code_trigger_ =
            jval<double>(json, "network.raim_we_code_trigger", "network_raim_we_code_trigger", 10.0);
        network_raim_we_iono_trigger_ =
            jval<double>(json, "network.raim_we_iono_trigger", "network_raim_we_iono_trigger", 12.0);
        if (network_raim_we_code_iono_ && !network_raim_within_epoch_) {
            log->warn("Network: network.raim_we_code_iono extends the within-epoch adaptation — "
                      "enabling network.raim_within_epoch (the loop the code/iono pools feed).");
            network_raim_within_epoch_ = true;
            if (!network_raim_ || !network_raim_attribution_) {
                network_raim_ = true;
                network_raim_attribution_ = true;
            }
        }
        if (network_raim_we_code_iono_)
            log->info("Network producer (WE-CI): within-epoch CODE+IONO row screening ON — "
                      "per-pool robust σ̂0 with triggers code {:g} / iono {:g}; responses: "
                      "epoch-scoped station exclusion (epicenter class) or network-wide "
                      "satellite code-row de-weight (σ×10, fallback-not-drop).  The phase "
                      "pool, σ̂0_adapt and the A4 ceiling are unchanged.",
                      network_raim_we_code_trigger_, network_raim_we_iono_trigger_);

        // GROSS LINE-OF-SIGHT SCREEN (card GL, docs/SPEC_event_harness.md §24.18).
        // Parsed HERE, next to the post-fit tiers it complements and whose blind
        // spot it exists to close: every one of them pools carried-ambiguity PHASE
        // rows, so a CODE-only gross error on one line of sight is invisible to all
        // of them (NetworkRaim.cpp:328's `|A_r(row, amb_col)| > 0.5` admission), and
        // the one tier that DOES see the row — MDB — publishes a reliability number
        // and the literal action "observe" (NetworkMdb.cpp:286).  The ONLY
        // solve-affecting knob in this neighbourhood; default OFF, OFF
        // byte-identical, NOT fingerprinted (it changes no carried state's MEANING —
        // it changes which rows are trusted THIS epoch — so a snapshot written
        // with it off remains readable with it on, the trace-knob precedent).
        {
            NetworkConfig& gnc = network_config_;
            gnc.gross_los_screen =
                jval<bool>(json, "network.gross_los_screen", "gross_los_screen", false);
            gnc.gross_los_k =
                jval<double>(json, "network.gross_los_k", "gross_los_k", 8.0);
            gnc.gross_los_min_rows =
                jval<int>(json, "network.gross_los_min_rows", "gross_los_min_rows", 2);
            gnc.gross_los_epochs =
                jval<int>(json, "network.gross_los_epochs", "gross_los_epochs", 2);
            gnc.gross_los_clear_sigma =
                jval<double>(json, "network.gross_los_clear_sigma", "gross_los_clear_sigma", 3.0);
            gnc.gross_los_clear_epochs =
                jval<int>(json, "network.gross_los_clear_epochs", "gross_los_clear_epochs", 2);
            gnc.gross_los_deweight =
                jval<double>(json, "network.gross_los_deweight", "gross_los_deweight", 100.0);
            gnc.gross_los_max_gap_s =
                jval<double>(json, "network.gross_los_max_gap_s", "gross_los_max_gap_s", 900.0);
            gnc.gross_los_event_holdoff_ep =
                jval<int>(json, "network.gross_los_event_holdoff_ep",
                          "gross_los_event_holdoff_ep", 0);
            // --- RF-3b (docs/refound/RF-3_code_row_qc_2026-09-22.md §4): the four
            // sub-rules that make the screen complete.  All default to today's behaviour,
            // so an existing arm's bytes are unchanged.
            {
                const std::string mrr =
                    jval<std::string>(json, "network.gross_los_min_rows_rule",
                                      "gross_los_min_rows_rule", std::string{"fixed"});
                if (mrr.empty() || mrr == "fixed")
                    gnc.gross_los_min_rows_rule = NetworkGrossLosMinRowsRule::Fixed;
                else if (mrr == "available")
                    gnc.gross_los_min_rows_rule = NetworkGrossLosMinRowsRule::Available;
                else {
                    gnc.gross_los_min_rows_rule = NetworkGrossLosMinRowsRule::Fixed;
                    log->error("Network (RF-3b B1): network.gross_los_min_rows_rule = '{}' is "
                               "not one of fixed|available — falling back to 'fixed'.  The "
                               "rule decides whether a line of sight with FEWER code rows "
                               "than network.gross_los_min_rows is reachable at all; a typo "
                               "here silently ships the unreachable half of the population.",
                               mrr);
                }
            }
            gnc.gross_los_bound =
                jval<bool>(json, "network.gross_los_bound", "gross_los_bound", false);
            gnc.gross_los_emit_exclude =
                jval<bool>(json, "network.gross_los_emit_exclude",
                           "gross_los_emit_exclude", false);
            gnc.gross_los_station_tier =
                jval<bool>(json, "network.gross_los_station_tier",
                           "gross_los_station_tier", false);
            if (!gnc.gross_los_screen
                && (gnc.gross_los_min_rows_rule != NetworkGrossLosMinRowsRule::Fixed
                    || gnc.gross_los_bound || gnc.gross_los_emit_exclude
                    || gnc.gross_los_station_tier))
                log->warn("Network (RF-3b): a gross-LOS sub-rule is set "
                          "(min_rows_rule={}, bound={}, emit_exclude={}, station_tier={}) "
                          "while network.gross_los_screen is OFF — the seam is never "
                          "entered, so all four are INERT.  Read this as a miswired arm, "
                          "not as a null result.",
                          gnc.gross_los_min_rows_rule == NetworkGrossLosMinRowsRule::Available
                              ? "available" : "fixed",
                          gnc.gross_los_bound ? "true" : "false",
                          gnc.gross_los_emit_exclude ? "true" : "false",
                          gnc.gross_los_station_tier ? "true" : "false");
            gnc.gross_los_warmup_epochs =
                jval<int>(json, "network.gross_los_warmup_epochs", "gross_los_warmup_epochs", 120);
            gnc.gross_los_max_station_frac =
                jval<double>(json, "network.gross_los_max_station_frac",
                             "gross_los_max_station_frac", 0.5);
            gnc.gross_los_sat_standdown =
                jval<bool>(json, "network.gross_los_sat_standdown",
                           "gross_los_sat_standdown", false);
            gnc.gross_los_sat_min_stations =
                jval<int>(json, "network.gross_los_sat_min_stations",
                          "gross_los_sat_min_stations", 3);
            gnc.gross_los_sat_wire_withhold =
                jval<bool>(json, "network.gross_los_sat_wire_withhold",
                           "gross_los_sat_wire_withhold", true);
            if (gnc.gross_los_screen) {
                // Clamp the knobs that cannot mean anything outside their range,
                // LOUDLY.  A screen configured with k ≤ 0 or a deweight ≤ 1 is not
                // "a gentler screen", it is a mechanism that declares everything or
                // responds with nothing, and either reads as a clean negative
                // result in the run-end counters (standing invariant #4).
                auto clampLow = [&](const char* name, double& v, double lo) {
                    if (!(v >= lo)) {
                        log->warn("Network (gross-LOS screen): network.{} = {:g} is below the "
                                  "floor {:g} — clamped.  Below it the knob cannot express a "
                                  "screen (it would declare everything, or respond with "
                                  "nothing).", name, v, lo);
                        v = lo;
                    }
                };
                clampLow("gross_los_k", gnc.gross_los_k, 1.0);
                clampLow("gross_los_clear_sigma", gnc.gross_los_clear_sigma, 0.0);
                clampLow("gross_los_deweight", gnc.gross_los_deweight, 1.0);
                clampLow("gross_los_max_gap_s", gnc.gross_los_max_gap_s, 0.0);
                if (gnc.gross_los_min_rows < 1) gnc.gross_los_min_rows = 1;
                if (gnc.gross_los_epochs < 1) gnc.gross_los_epochs = 1;
                if (gnc.gross_los_clear_epochs < 1) gnc.gross_los_clear_epochs = 1;
                if (gnc.gross_los_warmup_epochs < 0) gnc.gross_los_warmup_epochs = 0;
                if (gnc.gross_los_sat_min_stations < 2) {
                    log->warn("Network (gross-LOS, GL-2): network.gross_los_sat_min_stations "
                              "= {} cannot express a CROSS-STATION agreement — clamped to 2.  "
                              "The companion rule (network.raim_min_stations) runs 3.",
                              gnc.gross_los_sat_min_stations);
                    gnc.gross_los_sat_min_stations = 2;
                }
                if (gnc.gross_los_sat_standdown)
                    log->info("Network producer (gross-LOS, GL-2 whole-SATELLITE stand-down): "
                              "ON — the same satellite over the {:.1f}σ bar at >= {} stations "
                              "in ONE epoch is a SATELLITE fault, not {} line-of-sight faults: "
                              "its per-LOS declarations are refused (a standing one released) "
                              "and it is routed to the responses this producer already owns "
                              "for a cross-station CONSENSUS fault — the per-satellite retro "
                              "DISC BUMP (the network.raim_min_stations path: ura_retro_sats "
                              "-> NetworkSolPrep -> markSatDiscontinuity, mark == land){}.  "
                              "NEVER an epoch withhold; every other satellite still emits.  "
                              "MEASURED motivation: on runs/_gl_on6 at TOW 271200/271260 TEN "
                              "declarations fired and every one was G17, at ten stations on "
                              "four continents — the held-out consumer went to 56 m for an "
                              "hour.",
                              gnc.gross_los_k, gnc.gross_los_sat_min_stations,
                              gnc.gross_los_sat_min_stations,
                              gnc.gross_los_sat_wire_withhold
                                  ? ", and the per-satellite WIRE WITHHOLD (pruned from "
                                    "sol.sats the way the RT0-5 runaway quarantine prunes "
                                    "its own)"
                                  : " ONLY (network.gross_los_sat_wire_withhold is OFF, so "
                                    "the satellite still EMITS and the consumer is merely "
                                    "told)");
                else
                    log->info("Network producer (gross-LOS): GL-2 (the whole-SATELLITE "
                              "stand-down, network.gross_los_sat_standdown) is OFF — a "
                              "satellite faulted at many stations at once will be answered "
                              "as N independent line-of-sight faults, which is how "
                              "runs/_gl_on6 took its held-out consumer to 56 m at TOW "
                              "271200.  §24.18's status line: NOT consumer-safe without "
                              "§24.19.");

                // --- RF-3b banners.  One line per armed sub-rule, each naming the
                // population it reaches and the measurement that motivated it, so a run's
                // stdout states which of the four halves of card RF-3b were in force.
                if (gnc.gross_los_min_rows_rule == NetworkGrossLosMinRowsRule::Available)
                    log->info("Network producer (gross-LOS, RF-3b B1): the declaration bar is "
                              "min(gross_los_min_rows = {}, the LOS's code rows this epoch) — a "
                              "line of sight that has ONE code row is reachable, a line of "
                              "sight that has three still needs {}.  MEASURED reason: four of "
                              "the six largest episodes on runs/_rf0hv9_field6 (BSHM00ISR G13 "
                              "and G22, KIT300UZB G22) have exactly one code row because the "
                              "station lost every other signal, so the fixed bar of 2 is "
                              "unsatisfiable by construction.",
                              gnc.gross_los_min_rows, gnc.gross_los_min_rows);
                if (gnc.gross_los_bound)
                    log->info("Network producer (gross-LOS, RF-3b B2): the DECOUPLING edge is "
                              "BOUNDED — a declared line of sight's carried ambiguity levels "
                              "and its per-LOS iono carry are HELD at their pre-declaration "
                              "values and variances with zero process noise (border coupling "
                              "and joint block cleared) instead of being erased, and are "
                              "re-applied after every harvest while the declaration stands.  "
                              "The folded code row's residual then stays a MEASURE of the "
                              "fault: with the erase it ran to 445.5 m on BSHM00ISR G13 and "
                              "the release test could never fire, so 2 of 7 declarations ended "
                              "by EXPIRY (standing invariant #7).");
                if (gnc.gross_los_emit_exclude)
                    log->info("Network producer (gross-LOS, RF-3b B3): the screen's DECISION "
                              "half runs BEFORE the epoch's emission, and a declared line of "
                              "sight leaves its satellite's CONTRIBUTOR SET for that epoch — "
                              "the satellite is WITHHELD when fewer than {} contributing "
                              "stations remain, otherwise emitted with a per-satellite "
                              "discontinuity mark.  ⚠ The emitted clock and code bias are "
                              "joint-solve shared states, so the VALUE is not re-solved "
                              "without the line of sight; what lands on the declaring epoch is "
                              "the contributor-set exclusion and its declaration on the wire.  "
                              "The carry response still runs after the harvest.",
                              gnc.gross_los_sat_min_stations);
                if (gnc.gross_los_station_tier)
                    log->info("Network producer (gross-LOS, RF-3b B4): the whole-station guard "
                              "is a VERDICT, not only a hold — a station over the {:g} fraction "
                              "declares at STATION level with its own two edges, folds every "
                              "one of its code rows at sigma x {:g}, and releases when its "
                              "over-bar fraction sits at or under the cap for {} consecutive "
                              "screened epochs.  Today's hold merely defers: at BSHM00ISR the "
                              "guard fired at TOW 269310 (n_over=5 of n_los=7) and the per-LOS "
                              "declaration landed at 269340 on the same evidence, which is how "
                              "a station fault becomes N line-of-sight faults.",
                              gnc.gross_los_max_station_frac, gnc.gross_los_deweight,
                              gnc.gross_los_clear_epochs);
                if (!(gnc.gross_los_max_station_frac > 0.0)) {
                    log->warn("Network (gross-LOS screen): network.gross_los_max_station_frac = "
                              "{:g} would stand the screen down at every station — clamped to 1.0 "
                              "(guard disabled).  Use 0 < frac <= 1.",
                              gnc.gross_los_max_station_frac);
                    gnc.gross_los_max_station_frac = 1.0;
                }
                if (gnc.gross_los_max_station_frac > 1.0) gnc.gross_los_max_station_frac = 1.0;
                if (gnc.gross_los_warmup_epochs == 0)
                    log->warn("Network (gross-LOS screen): network.gross_los_warmup_epochs = 0 — "
                              "the screen may DECLARE from the first emitted epoch.  MEASURED on "
                              "the cert smoke: with no maturity horizon it declared 520 lines of "
                              "sight in 20 cold epochs, the code deweight removed the information "
                              "the cold solve needs for the receiver clocks, post-fit code "
                              "sigma-hat-0 went 3.2 -> 2334 and the SH health gate withheld 9 of "
                              "20 epochs.  A cold post-fit CODE residual is the un-converged "
                              "station block, not a fault.  Arm 0 only on a deliberately warm "
                              "lineage.");
                if (gnc.gross_los_max_station_frac >= 1.0)
                    log->warn("Network (gross-LOS screen): the WHOLE-STATION stand-down is "
                              "DISABLED (max_station_frac = 1.0), so a station whose every line "
                              "of sight is over the bar will have every one of its code rows "
                              "deweighted — which removes exactly the information that "
                              "determines its clock and troposphere.  That population exists on "
                              "this lineage (BSHM00ISR, hour 5 of the 6 h field bed: a 197 m ZWD "
                              "with all ten satellites past 20 m).");
                if (gnc.gross_los_clear_sigma >= gnc.gross_los_k)
                    log->warn("Network (gross-LOS screen): clear_sigma {:g} is not BELOW k {:g} "
                              "— the declare and clear bars have no hysteresis between them, so "
                              "a line of sight sitting near the bar will flap declare/clear "
                              "every other epoch.  Running as configured.",
                              gnc.gross_los_clear_sigma, gnc.gross_los_k);
                log->info("Network producer (gross-LOS screen, card GL): ON — per (station, "
                          "satellite) POST-FIT CODE residuals standardized by each row's OWN σ; "
                          "≥ {} rows over {:g}σ for {} consecutive epochs DECLARES the line of "
                          "sight gross (event, decoupling edge): its carried ambiguity levels "
                          "and per-LOS iono carry are erased, its ambiguities are marked "
                          "pending-reseed, and its CODE rows fold at σ × {:g} until the worst "
                          "row returns under {:g}σ for {} epochs (event, re-coupling edge).  "
                          "A declared LOS unseen for {:g} s EXPIRES (its own event).  The epoch "
                          "always EMITS — adapt-and-emit; nothing here withholds.  This is the "
                          "ONE acting post-fit tier that screens CODE: every other pools "
                          "carried-ambiguity PHASE rows, and MDB's action is 'observe'.",
                          gnc.gross_los_min_rows, gnc.gross_los_k, gnc.gross_los_epochs,
                          gnc.gross_los_deweight, gnc.gross_los_clear_sigma,
                          gnc.gross_los_clear_epochs, gnc.gross_los_max_gap_s);
                log->info("Network producer (gross-LOS screen) GUARDS: no declaration before {} "
                          "emitted epoch(s) (the MATURITY horizon — a cold post-fit code residual "
                          "is the un-converged station block, and an unguarded screen destroyed "
                          "its own solve on the 20-epoch cert smoke), and NO declaration at a "
                          "station where more than {:.0f} % of its lines of sight are over the bar "
                          "in one epoch (the WHOLE-STATION stand-down — that is a station fault, "
                          "not a line-of-sight fault, and de-weighting all of its code rows "
                          "removes what determines its clock).  Both guards SCREEN and COUNT "
                          "normally; they only hold the declaration, and neither blocks a CLEAR.",
                          gnc.gross_los_warmup_epochs, 100.0 * gnc.gross_los_max_station_frac);
            }
        }

        // BAARDA RELIABILITY NUMBERS / MDB (docs/SPEC_redundancy_census.md).
        // Parsed HERE, next to the B2 redundancy knob it rides, and AFTER
        // loadNetworkObservabilityAndSnapshot_ has resolved the redundancy-census
        // arming — so the precondition below reads final values of BOTH r_i
        // sources.  MEASURE-ONLY and NOT fingerprinted: same class as the census.
        {
            NetworkConfig& mnc = network_config_;
            mnc.mdb = jval<bool>(json, "network.mdb", "mdb", false);
            mnc.mdb_event_m =
                jval<double>(json, "network.mdb_event_m", "mdb_event_m", 1.0);
            mnc.mdb_event_holdoff_ep =
                jval<int>(json, "network.mdb_event_holdoff_ep",
                          "mdb_event_holdoff_ep", 20);
            if (mnc.mdb) {
                // The two r_i sources.  The RAIM one needs the tier it rides to be
                // live as well: raim_redundancy_normalize alone computes nothing,
                // because networkPostFitRaim is not called at all without
                // network.raim.
                const bool r_from_raim   = network_raim_ && network_raim_redundancy_normalize_;
                const bool r_from_census = mnc.redundancy_census_stride > 0
                                        || mnc.redundancy_escalate_sigma > 0.0;
                // HARD ERROR, not a silent disarm.  Without a redundancy source
                // there is no r_i to divide by, so the card would run, bin zero
                // rows, and print a clean summary indistinguishable from "this
                // network cross-checks everything" — the exact live-counter-over-
                // dead-wiring failure this project keeps paying for.
                if (!r_from_raim && !r_from_census)
                    throw std::runtime_error(
                        "network.mdb requires a per-row redundancy source: either "
                        "network.raim_redundancy_normalize on a live network.raim, or the "
                        "redundancy census (network.redundancy_census_stride > 0 or "
                        "network.redundancy_escalate_sigma > 0).  With neither there is no "
                        "r_i to divide by and MDB would report zero rows with no warning.");
                if (!(mnc.mdb_event_m >= 0.0)) {
                    log->warn("Network (MDB): network.mdb_event_m = {:g} is negative — a "
                              "reporting threshold below zero would event EVERY finite row. "
                              "Restoring the 1.0 m default.", mnc.mdb_event_m);
                    mnc.mdb_event_m = 1.0;
                }
                if (mnc.mdb_event_holdoff_ep < 0) mnc.mdb_event_holdoff_ep = 0;
                // Invariant-#4 tripwire at LOAD time: without the P1-4 post-fit
                // pass the scaled variant can never populate, and a run that quotes
                // formal MDB alone is repeating D5 v1.  WARN rather than auto-arm —
                // trace_postfit_sigma0 costs a full residual pass per epoch, and
                // silently buying that is not this card's call to make.
                if (!mnc.trace_postfit_sigma0)
                    log->warn("Network (MDB): network.trace_postfit_sigma0 is off, so the "
                              "per-family empirical sigma0 does not exist and every "
                              "mdb_scaled column will be BLANK.  Formal MDB inherits the "
                              "formal-sigma optimism (D5 measured it 10-30x optimistic on "
                              "this data) and MDB scales linearly with sigma — arm the "
                              "post-fit pass if the numbers are going to be quoted.");
                log->info("Network (MDB): ARMED — Baarda minimal detectable bias "
                          "MDB_i = sigma_i * delta_0 / sqrt(r_i) with delta_0 = k + 0.8416 "
                          "= {:.4f} (k = estimation.data_snooping_critical = {:g}, beta = "
                          "0.20), r_i from {}{}{}.  Reporting threshold {:g} m, per-cell "
                          "holdoff {} pass(es).  MEASURE-ONLY: no gate, no deweight, no "
                          "admission reads it; MDB is a property of the GEOMETRY, and its "
                          "value is making the census's UNCHECKABLE binary quantitative.",
                          data_snooping_critical_ + 0.8416, data_snooping_critical_,
                          r_from_raim ? "the RAIM pool" : "",
                          (r_from_raim && r_from_census) ? " + " : "",
                          r_from_census ? "the redundancy census sweep" : "",
                          mnc.mdb_event_m, std::max(0, mnc.mdb_event_holdoff_ep));
            }
        }

        // BLOCKFORMAL — the (station, satellite) LOS block outlier census
        // (docs/SPEC_redundancy_census.md).  Parsed HERE, next to the MDB precondition
        // and for the same load-order reason: this card RIDES the redundancy
        // census's swept stations, and loadNetworkObservabilityAndSnapshot_ has
        // already resolved that card's arming, so the precondition below reads a
        // final value.  MEASURE-ONLY, NOT fingerprinted: same class as the census.
        {
            NetworkConfig& bnc = network_config_;
            bnc.block_baarda = jval<bool>(json, "network.block_baarda", "block_baarda", false);
            bnc.block_baarda_families =
                jval<std::string>(json, "network.block_baarda_families",
                                  "block_baarda_families", "G,E,R,C");
            bnc.block_baarda_window_epochs =
                jval<int>(json, "network.block_baarda_window_epochs",
                          "block_baarda_window_epochs", 60);
            bnc.block_baarda_min_blocks =
                jval<int>(json, "network.block_baarda_min_blocks",
                          "block_baarda_min_blocks", 200);
            bnc.block_baarda_report_sigma =
                jval<double>(json, "network.block_baarda_report_sigma",
                             "block_baarda_report_sigma", 4.0);
            bnc.block_baarda_event_holdoff_ep =
                jval<int>(json, "network.block_baarda_event_holdoff_ep",
                          "block_baarda_event_holdoff_ep", 20);
            bnc.block_baarda_corroborate =
                jval<bool>(json, "network.block_baarda_corroborate",
                           "block_baarda_corroborate", false);

            // Standing invariant #4 tripwire: a corroboration arm without the
            // census produces nothing at all.  HARD ERROR rather than a silent
            // disarm, exactly as the prior-residual census does it.
            if (bnc.block_baarda_corroborate && !bnc.block_baarda)
                throw std::runtime_error(
                    "network.block_baarda_corroborate requires network.block_baarda = true "
                    "(the corroboration join runs inside the block sweep; arming it alone "
                    "produces no blocks, no contingency table and no warning at run time).");

            if (bnc.block_baarda) {
                // HARD ERROR, not a silent disarm.  SPEC §6: the block sweep has no
                // scheduler of its own by design — it hangs off the redundancy
                // census's SWEPT branch so the stride, the targeted escalation and
                // the trip-forced full sweep are inherited exactly.  With no host
                // the card would run, enumerate zero blocks, and print a clean
                // summary indistinguishable from "this network has no smeared
                // faults" — the live-counter-over-dead-wiring failure again.
                const bool have_host = bnc.redundancy_census_stride > 0
                                    || bnc.redundancy_escalate_sigma > 0.0;
                if (!have_host)
                    throw std::runtime_error(
                        "network.block_baarda requires the redundancy census as its sweep host "
                        "(network.redundancy_census_stride > 0 or network.redundancy_escalate_sigma "
                        "> 0).  The block sweep deliberately adds no scheduler: it rides that "
                        "card's swept stations, so with the census off it would enumerate zero "
                        "blocks and report a clean run indistinguishable from a genuine negative.");

                // Invariant-#4 tripwire: a constellation letter with no hook.  The
                // census would report it as "armed, zero blocks" at run end, which
                // reads exactly like a real negative — name it HERE, where the typo
                // is still cheap to fix.
                static const char* kKnownSystems[] = {"G", "E", "R", "C", "J", "I", "S"};
                std::size_t n_fam = 0;
                for (const std::string& f :
                     NetworkCarryVce::parseComponents(bnc.block_baarda_families)) {
                    ++n_fam;
                    bool known = false;
                    for (const char* k : kKnownSystems) if (f == k) { known = true; break; }
                    if (!known)
                        log->warn("Network (block-Baarda census): family '{}' in "
                                  "network.block_baarda_families is not a RINEX system letter "
                                  "(known: G, E, R, C, J, I, S) — it will match no satellite and "
                                  "report zero blocks, which is indistinguishable from a genuine "
                                  "negative result.  Fix the spelling or drop it.", f);
                }
                if (n_fam == 0)
                    throw std::runtime_error(
                        "network.block_baarda is armed but network.block_baarda_families is empty "
                        "— every block would be dropped for want of a family and the run would "
                        "report zero blocks with no warning.");

                if (bnc.block_baarda_window_epochs < 1)
                    log->warn("Network (block-Baarda census): "
                              "network.block_baarda_window_epochs {} < 1 — treating as whole-run "
                              "accumulation (no forgetting) for σ̂₀.",
                              bnc.block_baarda_window_epochs);
                if (!(bnc.block_baarda_report_sigma > 0.0)) {
                    log->warn("Network (block-Baarda census): "
                              "network.block_baarda_report_sigma = {:g} is not positive — no block "
                              "would ever be traced or evented, and INC-3's smearing fraction "
                              "would be undefined.  Restoring the 4.0 default.",
                              bnc.block_baarda_report_sigma);
                    bnc.block_baarda_report_sigma = 4.0;
                }
                if (bnc.block_baarda_min_blocks < 0) bnc.block_baarda_min_blocks = 0;
                if (bnc.block_baarda_event_holdoff_ep < 0)
                    bnc.block_baarda_event_holdoff_ep = 0;

                // INC-2 auto-arm of network.cycle_slip.gap_instrument.  Unlike the
                // prior-residual census's — which has to sit at the tail of
                // loadNetworkSlipDetectors_ because its own knobs are parsed one
                // helper EARLIER (step 151) and the later jval would overwrite the
                // forcing — this block is step 153 and the gap_instrument jval is
                // step 152, so the forcing is safe INLINE.  gap_instrument is what
                // makes networkQualifyRawObservables run and fill the per-(station,
                // satellite) raw MW/GF verdicts the join reads; without it every
                // contingency cell lands in 'no_evidence' and SPEC §8.3's b-cell is
                // structurally unmeasurable.  Deliberately NOT mw_lli_corroborate:
                // that is a DECISION card and would perturb the very population
                // being measured (the same choice the prior-residual loader and
                // D5 v3 both make).
                if (bnc.block_baarda_corroborate && !bnc.slip_qc.gap_instrument) {
                    bnc.slip_qc.gap_instrument = true;
                    log->warn("Network (block-Baarda census INC-2): AUTO-ARMING "
                              "network.cycle_slip.gap_instrument — the corroboration join "
                              "reads the raw MW/GF step verdicts and nothing else in this "
                              "config would have computed them, so every contingency cell "
                              "would have landed in the 'no_evidence' column and the b-cell "
                              "would be unmeasurable.  gap_instrument is instrumentation-only "
                              "and verified byte-identical; mw_lli_corroborate is deliberately "
                              "NOT armed (a decision card would perturb what is measured).");
                }
                // Invariant-#4 sink dependency: the ladder survives without
                // trace_events, but every block IDENTITY is emitted through
                // emitEvent, which is a hard no-op when that sink is closed.  WARN
                // rather than auto-arm — a trace sink is the user's call, and the
                // counters still make the finding checkable.
                if (!bnc.trace_events)
                    log->warn("Network (block-Baarda census): network.trace_events is off, so "
                              "no block IDENTITY will be written — the ladders and the run-end "
                              "counters still populate, but the worst-block names live only in "
                              "the log line.  Arm trace_events if any loud block is going to be "
                              "chased back to a satellite.");

                // The convergence-hygiene reading, at load rather than after the
                // run: on a cold start a free ambiguity absorbs its own arc's phase
                // misfit EXACTLY, so those post-fit residuals are identically zero
                // and their rows read r ≈ 0 (43 % of data rows on the FT63 cold
                // smoke).  A block dominated by them comes back UNCHECKABLE by
                // construction, not by data quality.
                log->info("Network producer (block-Baarda census, "
                          "docs/SPEC_redundancy_census.md): ARMED (families='{}', window={} ep, "
                          "min_blocks={}, report={:g}·t_B, corroborate={}).  Per (station, "
                          "satellite) LOS block of the redundancy census's SWEPT stations: "
                          "T_B = ẽ_Bᵀ Q_ẽ,B⁻¹ ẽ_B / b over the b ≤ 8 code+phase rows of one "
                          "line of sight, standardized by a per-constellation empirical σ̂₀ "
                          "and counted onto the census ladder per (family × label ∈ "
                          "CLEAN/RESPONDED/FRAME).  This is the MIDDLE tier: one fault "
                          "smeared over a LOS can be sub-threshold on every row while the "
                          "block is loud (LASER 5-98).  MEASURE-ONLY: no gate, no deweight, "
                          "no refloat — report_sigma is a TRACE threshold and the solve is "
                          "byte-identical.  NOTE t_B is a VARIANCE RATIO (E = 1 under H₀ for "
                          "every b), NOT σ units — do not compare its ladder rungs against "
                          "the prior-residual census's without reading both units.  Grade on "
                          "a CONVERGED WARM window: a free ambiguity zeroes its own arc's "
                          "post-fit phase residuals, so a cold start reports a large and "
                          "STRUCTURAL uncheckable-block population.",
                          bnc.block_baarda_families, bnc.block_baarda_window_epochs,
                          bnc.block_baarda_min_blocks, bnc.block_baarda_report_sigma,
                          bnc.block_baarda_corroborate ? "on" : "off");
            }
        }

        // STATION CONGRUENCE — coordinate-fault detection in position space
        // (docs/SPEC_redundancy_census.md).  Parsed HERE, immediately after the
        // block census and for the same load-order reason: it rides the SAME
        // redundancy-census sweep, whose arming loadNetworkObservabilityAndSnapshot_
        // has already resolved, so the precondition below reads a final value.
        // MEASURE-ONLY, NOT fingerprinted: same class as the two censuses above.
        {
            NetworkConfig& cnc = network_config_;
            cnc.congruence_screen =
                jval<bool>(json, "network.congruence_screen", "congruence_screen", false);
            cnc.congruence_report_sigma =
                jval<double>(json, "network.congruence_report_sigma",
                             "congruence_report_sigma", 4.0);
            cnc.congruence_event_holdoff_ep =
                jval<int>(json, "network.congruence_event_holdoff_ep",
                          "congruence_event_holdoff_ep", 20);

            if (cnc.congruence_screen) {
                // HARD ERROR, not a silent disarm.  SPEC §3: this card has no
                // scheduler of its own by design — it hangs off the redundancy
                // census's SWEPT branch so the stride, the targeted escalation and
                // the trip-forced full sweep are inherited exactly.  With no host
                // it would run, sweep zero stations, and print a clean summary
                // indistinguishable from "every held coordinate on this network is
                // right" — the live-counter-over-dead-wiring failure, on the one
                // card whose whole purpose is to disprove a silent negative.
                const bool have_host = cnc.redundancy_census_stride > 0
                                    || cnc.redundancy_escalate_sigma > 0.0;
                if (!have_host)
                    throw std::runtime_error(
                        "network.congruence_screen requires the redundancy census as its "
                        "sweep host (network.redundancy_census_stride > 0 or "
                        "network.redundancy_escalate_sigma > 0).  The congruence screen "
                        "deliberately adds no scheduler: it rides that card's swept "
                        "stations, so with the census off it would sweep zero stations and "
                        "report a clean run indistinguishable from a genuine negative.");

                if (!(cnc.congruence_report_sigma > 0.0)) {
                    log->warn("Network (station congruence): "
                              "network.congruence_report_sigma = {:g} is not positive — "
                              "every station-epoch would be 'reported', flooding the event "
                              "stream and making the INC-3 separability table meaningless. "
                              "Restoring the 4.0 default.",
                              cnc.congruence_report_sigma);
                    cnc.congruence_report_sigma = 4.0;
                }
                if (cnc.congruence_event_holdoff_ep < 0)
                    cnc.congruence_event_holdoff_ep = 0;

                // Invariant-#4 sink dependency, same shape as the block census's:
                // the per-station TRACE row is the headline product and survives
                // without trace_events, but the loud-station identities are emitted
                // through emitEvent, a hard no-op when that sink is closed.
                if (!cnc.trace_events)
                    log->warn("Network (station congruence): network.trace_events is off, so "
                              "no loud-station IDENTITY will be written — network_congruence.csv "
                              "and the run-end MDB table still populate, but the alarm stream "
                              "does not exist.  Arm trace_events if a flagged station is going "
                              "to be chased at the epoch it was flagged.");

                // The regime reading, at load rather than after the run.  Under a
                // free-ambiguity (carry_invariant) lineage every phase row fits its
                // own arc exactly, so those rows' residual cofactor is ~0 and they
                // contribute ~nothing to M — the screen is honestly CODE-grade and
                // its MDB is dm-class.  Saying so HERE is what stops a dm-class MDB
                // table being read at run end as a defect in the card.
                //
                // The line names BOTH regimes rather than resolving which one is
                // active: network_carry_invariant_ is parsed in loadNetworkCarries_,
                // which runs AFTER this helper, so a conditional here would read the
                // struct default and confidently print the wrong regime.  The MDB
                // table at run end is measured, not predicted, and settles it.
                log->info("Network producer (station congruence, "
                          "docs/SPEC_redundancy_census.md): ARMED (report={:g}·t, "
                          "holdoff={} ep).  Per SWEPT station of the redundancy census: "
                          "δ̂ = M⁺H_BᵀP_Bẽ_B over its code+phase rows with M = "
                          "H_BᵀP_BQ_ẽ,BP_BH_B, tested as T = δ̂ᵀMδ̂/(q·σ̂₀²) and reported "
                          "with a per-direction coordinate MDB.  THE ONE FAULT CLASS NO "
                          "ARMED TIER CAN SEE: a wrong HELD coordinate is coherent across "
                          "every LOS, so median polish removes it, per-row w-tests see "
                          "crumbs and the LOS-block tier pools one line of sight (JPLM's "
                          "6.10 cm ARP error is the precedent — found by hand).  δ̂ is the "
                          "HELD COORDINATE'S ERROR: +East ⇒ the held mark sits that far "
                          "East of the data, SUBTRACT to correct.  MEASURE-ONLY: no "
                          "coordinate is moved, deweighted or re-estimated and the solve "
                          "is byte-identical; report_sigma is a TRACE threshold.  NOTE t "
                          "is a VARIANCE RATIO (E = 1 under H₀ for every q), NOT σ units.  "
                          "REGIME: on a carry_invariant (free-ambiguity) lineage every "
                          "phase row fits its own arc exactly, so the phase tier "
                          "contributes ~nothing to M and this screen is honestly "
                          "CODE-grade — a dm-class MDB is the EXPECTED v1 answer, not a "
                          "defect; on a leveled lineage the phase tier engages and the "
                          "MDB should fall toward cm-class.  The run-end MDB table is "
                          "measured, not predicted, and states which regime the run was "
                          "actually in.",
                          cnc.congruence_report_sigma, cnc.congruence_event_holdoff_ep);
            }
        }

        // CONSTRAINT-AWARE GLOBAL σ̂₀ (docs/SPEC_prior_residual_census.md §14.4).
        // The knob itself was parsed in loadNetworkCarries_ next to the census it
        // rides; this is the DATA-SIDE precondition, and it has to live here
        // because network_raim_ / network_raim_redundancy_normalize_ are resolved
        // a few hundred lines above in THIS helper (the section's load-order rule
        // — the same reason the INC-3 gap_instrument auto-arm sits at the tail of
        // loadNetworkSlipDetectors_ rather than next to its own knob).
        //
        // WARN, not a hard error and not an auto-arm: the r ≡ 1 fallback is a
        // SPECIFIED degraded path (§2), it is flagged per epoch in-band, and
        // raim_redundancy_normalize is SOLVE-AFFECTING — silently buying a
        // different refloat population to improve a measure-only column is not
        // this card's call to make.
        if (network_config_.prior_residual_global_sigma0) {
            // MEASURED on the first FT63 smoke, 2026-08-04, and the reason this
            // tripwire exists at all: the driver brackets networkPostFitRaim with
            // `if (raim_on && carry_warm)`, and carry_warm = phase_leveling && …
            // (ApplicationNetwork.cpp:1318).  So on a lineage WITHOUT
            // phase_leveling the post-fit pass never runs, network.raim reads
            // ARMED in every log line, and the data side of the global form is
            // structurally dead — 0 rows over the whole run.  The in-band NO_DATA
            // flag catches it at run time, but that is one run too late: the fix
            // is a different lineage, and this is where that is still cheap.
            if (!network_phase_leveling_)
                log->warn("Network (constraint-aware global σ̂₀): network.phase_leveling is OFF, "
                          "so the POST-FIT RAIM pass — which the data side rides — is never "
                          "called: the driver brackets it with `raim_on && carry_warm` and "
                          "carry_warm = phase_leveling && warmup.  network.raim will still read "
                          "ARMED everywhere, and the data side will see ZERO rows for the whole "
                          "run: every epoch carries the NO_DATA flag, prior_tension_ratio is "
                          "UNDEFINED (there is no denominator) and sigma0_global degrades to a "
                          "PRIOR-ONLY variance component.  The headline of this card is NOT "
                          "measurable on this lineage — grade it on a phase_leveling one (see "
                          "config/test_suite/_ft63_s0g_raimlive.json).");
            if (!network_raim_)
                log->warn("Network (constraint-aware global σ̂₀): network.raim is OFF, so the "
                          "post-fit RAIM row walk never runs and the DATA side of the global form "
                          "sees zero rows.  Every epoch will carry the NO_DATA flag and "
                          "sigma0_global will be a PRIOR-ONLY variance component — not LASER 5-74's "
                          "global statistic.  Arm network.raim, or read the column as the carry's "
                          "own variance factor and nothing more.");
            else if (!network_raim_redundancy_normalize_)
                log->warn("Network (constraint-aware global σ̂₀): network.raim_redundancy_normalize "
                          "is OFF, so no per-row Baarda r_i exists and the data side falls back to "
                          "r ≡ 1.  Σr_data is then a ROW COUNT rather than a redundancy: the "
                          "denominator is too large and sigma0_global reads LOW.  Every affected "
                          "epoch sets the APPROX flag in the s0g_flags column and the run-end line "
                          "repeats it — the number is degraded, not wrong-and-silent.  Note that "
                          "arming raim_redundancy_normalize is SOLVE-AFFECTING (it changes which "
                          "arcs refloat), so an A/B against this arm is not byte-identical.");
        }

        // Per-satellite carried-clock sanity monitor (the shared-parameter analogue of the
        // carried-ambiguity refloat): detects a lone satellite whose clock has locked into a
        // physically impossible value — invisible to the σ̂0 RAIM, which needs the shared
        // residual level to move — and refloats its carried clock (+ coupled phase/code bias)
        // so the stiff sat_clock_q carry can no longer freeze the wrong value.
        network_clock_monitor_ =
            jval<bool>(json, "network.clock_monitor", "network_clock_monitor", false);
        network_clock_monitor_abs_max_m_ =
            jval<double>(json, "network.clock_monitor_abs_max_m", "network_clock_monitor_abs_max_m", 20.0);
        network_clock_monitor_jump_max_m_ =
            jval<double>(json, "network.clock_monitor_jump_max_m", "network_clock_monitor_jump_max_m", 10.0);
        network_clock_monitor_couple_raim_ =
            jval<bool>(json, "network.clock_monitor_couple_raim", "network_clock_monitor_couple_raim", false);
        // A flagged satellite's carried state is loosened to this σ (a soft large-q reinit that
        // keeps a weak anchor at the previous value) rather than being withheld entirely; ≤0
        // restores the hard skip-stash refloat.
        network_clock_monitor_reinit_sigma_m_ =
            jval<double>(json, "network.clock_monitor_reinit_sigma_m", "network_clock_monitor_reinit_sigma_m", 30.0);
        // SPEC_day129_frame_guards Phase 2.1: CB-carry event response (invariant #1).
        // Solve-determining when armed ⇒ conditional-appended to the resume
        // fingerprint (NetworkRunContext.cpp), so an unarmed run's fingerprint is
        // byte-identical and a resume cannot cross a flip of the toggle.
        network_config_.cb_excursion_reinit =
            jval<bool>(json, "network.cb_excursion_reinit", "cb_excursion_reinit", false);
        network_config_.cb_excursion_median_m =
            jval<double>(json, "network.cb_excursion_median_m", "cb_excursion_median_m", 50.0);
        network_config_.cb_excursion_reinit_all_sats =
            jval<bool>(json, "network.cb_excursion_reinit_all_sats",
                       "cb_excursion_reinit_all_sats", false);
        if (network_config_.cb_excursion_reinit) {
            if (!(network_config_.cb_excursion_median_m > 0.0)) {
                log->error("Network: network.cb_excursion_reinit is on but "
                           "cb_excursion_median_m = {} is not > 0 — the detector would "
                           "trip on every epoch.  Disabling the guard.",
                           network_config_.cb_excursion_median_m);
                network_config_.cb_excursion_reinit = false;
            } else if (!estimate_network_sat_code_bias_) {
                log->warn("Network: network.cb_excursion_reinit is on but "
                          "estimate_sat_code_bias is off — there is no satellite "
                          "code-bias carry to reinitialize, so the guard is INERT.");
            } else {
                log->info("Network (Phase 2.1): CB excursion reinit ARMED — a per-epoch "
                          "network-wide common-mode trip (median |Δclock| over satellites "
                          "> {:.1f} m) marks the {} code-bias carry for REINIT "
                          "(inflate_q, σ={:.0f} m next epoch) and queues a code-bias "
                          "discontinuity bump at the following emit.  Reset-not-repair; "
                          "no measurement is dropped.  Datum-change epochs are exempt "
                          "(a handover moves every clock — a datum artifact, not a fault).",
                          network_config_.cb_excursion_median_m,
                          network_config_.cb_excursion_reinit_all_sats
                              ? "ALL satellites'" : "clock-PIVOT satellite's",
                          network_clock_monitor_reinit_sigma_m_);
            }
        }

        // SPEC_day129_frame_guards Phase 2.2: SH-field health GATE (gate-not-warn
        // rework of the T1a a00 band probe; handoff §7).  EMISSION-ONLY — the solve
        // and carry are untouched — so it is NOT in the resume fingerprint (same
        // class as raim / clock_runaway_check).
        network_config_.sh_health_gate =
            jval<bool>(json, "network.sh_health_gate", "sh_health_gate", false);
        network_config_.sh_gate_min_epochs =
            jval<int>(json, "network.sh_gate_min_epochs", "sh_gate_min_epochs", 1);
        network_config_.sh_gate_rms_max_tecu =
            jval<double>(json, "network.sh_gate_rms_max_tecu", "sh_gate_rms_max_tecu", 0.0);
        if (network_config_.sh_health_gate) {
            const GlobalVtecShConfig& shc_gate = global_vtec_sh_;
            const bool band_active = shc_gate.a00_warn_low_tecu < shc_gate.a00_warn_high_tecu;
            const bool rms_active  = network_config_.sh_gate_rms_max_tecu > 0.0;
            if (network_config_.sh_gate_min_epochs < 1) {
                log->warn("Network: network.sh_gate_min_epochs = {} clamped to 1.",
                          network_config_.sh_gate_min_epochs);
                network_config_.sh_gate_min_epochs = 1;
            }
            if (!estimate_network_iono_sh_) {
                log->warn("Network: network.sh_health_gate is on but estimate_iono_sh is "
                          "off — there is no SH field to band-check, so the gate is INERT "
                          "(disabled).");
                network_config_.sh_health_gate = false;
            } else if (!band_active && !rms_active) {
                log->warn("Network: network.sh_health_gate is on but the a00 band is "
                          "silenced (low {} >= high {}) and sh_gate_rms_max_tecu is 0 — "
                          "no active condition, so the gate is INERT (disabled).",
                          shc_gate.a00_warn_low_tecu, shc_gate.a00_warn_high_tecu);
                network_config_.sh_health_gate = false;
            } else {
                log->info("Network (Phase 2.2): SH health GATE armed — an epoch with total "
                          "a00 outside [{:.1f}, {:.1f}] TECU{} for >= {} consecutive "
                          "epoch(s) is WITHHELD from emission (solve/carry/QC continue; "
                          "counted per EPOCH, transitions logged un-throttled).",
                          shc_gate.a00_warn_low_tecu, shc_gate.a00_warn_high_tecu,
                          rms_active ? fmt::format(" (or rms_all > {:.1f} TECU)",
                                                   network_config_.sh_gate_rms_max_tecu)
                                     : std::string{},
                          network_config_.sh_gate_min_epochs);
            }
        }

        // Adaptive iono process noise on the NETWORK path (network.adaptive_iono_q).
        // The user-path flag estimation.adaptive_iono_process_noise.enable is a
        // PHANTOM here — no per-station KalmanFilter::predict consumes iono q
        // overrides on the network path (slant iono is re-estimated white each
        // epoch).  This flag instead scales the T5v2 GM deviation innovation per
        // (station,satellite) by the same IonoVariabilityMonitor GF-TD2 clamped
        // ratio² — the deviation channel is the only time-linked slant-iono element
        // on this path, and its fixed σ_ss(1−λ²) is what lags storm TEC
        // acceleration into the pin deweight.  Tuning (windows, max multiplier,
        // min epochs) is read from estimation.adaptive_iono_process_noise; its
        // .enable stays user-path-only.  Deliberately NOT in the warm-resume
        // fingerprint: a forward-only Q modifier does not change the meaning of
        // serialized state (same class as filter.process_noise.*), so OFF/ON legs
        // may share one snapshot for event A/Bs.
        network_config_.adaptive_iono_q =
            jval<bool>(json, "network.adaptive_iono_q", "network_adaptive_iono_q", false);
        if (network_config_.adaptive_iono_q) {
            const bool dev_channel_on = estimate_network_iono_sh_
                                        && global_vtec_sh_.rep_error_tau_s > 0.0;
            if (!dev_channel_on) {
                log->warn("Network: network.adaptive_iono_q is on but the T5v2 GM "
                          "deviation channel is not (needs estimate_iono_sh and "
                          "global_vtec_sh.rep_error_tau_s > 0) — no actuation site, so "
                          "the flag is INERT (disabled).");
                network_config_.adaptive_iono_q = false;
            } else {
                log->info("Network: ADAPTIVE IONO Q ON — per-station GF-TD2 variability "
                          "monitors (IonoVariabilityMonitor) scale the T5v2 GM deviation "
                          "innovation per (station,satellite), clamped [1, {:.0f}x] "
                          "(recent {} / baseline {} epochs, min {}).  Storm response for "
                          "the iono representation: the per-LOS deviation can FOLLOW TEC "
                          "acceleration instead of lagging into the pin deweight.",
                          adaptive_iono_q_.max_multiplier, adaptive_iono_q_.recent_window,
                          adaptive_iono_q_.baseline_window, adaptive_iono_q_.min_epochs);
            }
        }
    }

    // =================================================================
    //  network subsection helper: loadNetworkClockModelAndChannels_
    // =================================================================
    /**
     * @brief Two-state clock model, C1 emission, and the B1/B1b/M4 quality channels.
     *
     * @c network.clock_model*, @c emit_clock_rate, @c emit_generation_time, the RT0-5 runaway
     * quarantine, the RTA-10 epoch-status heartbeat, and URA / phase-bias-sigma /
     * combined-sigma.  The R5 @c ura_staleness_q2 capture sits deliberately AFTER the
     * clock-model validity-restore so it inherits the RESTORED q2.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkClockModelAndChannels_(const nlohmann::json& json,
                                                       const std::shared_ptr<spdlog::logger>& log) {
        // Alias preamble: the body below reads the driver local it was written
        // against.  network_service_mode_ is written once, at the top of the
        // section, and never again — so this is the value the un-split code saw.
        const bool service_mode = network_service_mode_;

        // Two-state (offset+drift) oscillator model for the carried satellite clock, with
        // Allan/Hadamard-derived process noise, replacing the scalar random-walk carry: the tight
        // drift-following prediction turns a self-consistent lock-in into a large normalized
        // innovation (a detectable dynamics fault) and sizes the detection band physically.
        network_clock_model_ =
            jval<bool>(json, "network.clock_model", "network_clock_model", false);
        network_clock_model_q1_ =
            jval<double>(json, "network.clock_model_q1", "network_clock_model_q1",
                         network_config_.process_noise.get(NetworkQClass::SatClockWfm));
        network_clock_model_q2_ =
            jval<double>(json, "network.clock_model_q2", "network_clock_model_q2",
                         network_config_.process_noise.get(NetworkQClass::SatClockRwfm));
        network_clock_model_k_sigma_ =
            jval<double>(json, "network.clock_model_k_sigma", "network_clock_model_k_sigma", 6.0);
        network_clock_model_meas_std_m_ =
            jval<double>(json, "network.clock_model_meas_std_m", "network_clock_model_meas_std_m", 0.05);
        // R11: dedicated max-gap for the two-state clock-model carry (reseed / drift-reset /
        // C1-ZOH bound), distinct from the ambiguity/ZWD phase-leveling max-gap.  Defaults to
        // network_phase_leveling_max_gap_s_ (loaded above) so a config that does not set it is
        // byte-identical to the previous behaviour, which borrowed that same value.
        network_clock_model_max_gap_s_ =
            jval<double>(json, "network.clock_model_max_gap_s", "network_clock_model_max_gap_s",
                         network_phase_leveling_max_gap_s_);
        if (!(network_clock_model_max_gap_s_ > 0.0)) {
            // The phase-leveling gap is itself user-settable and unvalidated, so it
            // cannot be the unconditional fallback (a pl gap <= 0 here would leave the
            // clock model permanently reseeding: no prior, drift 0, C1 0).
            const double gap_fallback = (network_phase_leveling_max_gap_s_ > 0.0)
                                            ? network_phase_leveling_max_gap_s_ : 120.0;
            log->warn("Network: network.clock_model_max_gap_s {} must be > 0; restoring "
                      "{:.0f} s (phase-leveling max-gap, or 120 s when that is also invalid).",
                      network_clock_model_max_gap_s_, gap_fallback);
            network_clock_model_max_gap_s_ = gap_fallback;
        }
        if (network_clock_model_ &&
            network_clock_model_max_gap_s_ != network_phase_leveling_max_gap_s_)
            log->info("Network (R11): network.clock_model_max_gap_s = {:.0f} s (clock-model carry "
                      "reseed / drift-reset / C1-ZOH bound; independent of the {:.0f} s "
                      "ambiguity/ZWD phase-leveling max-gap).",
                      network_clock_model_max_gap_s_, network_phase_leveling_max_gap_s_);
        // Emit C1 (clock rate) from the solved clock WITHOUT the two-state carry prior — the
        // recalibration-supported alternative to network.clock_model (which is a no-go: the
        // estimable is white-FM, not RWFM, so the carry buys ~1× yet risks a NIS storm; the
        // exploitable stability is the deterministic RATE, emitted here without carrying).
        network_emit_clock_rate_ =
            jval<bool>(json, "network.emit_clock_rate", "network_emit_clock_rate", false);
        if (network_emit_clock_rate_ && network_clock_model_)
            log->info("Network: network.emit_clock_rate is on but network.clock_model already "
                      "supplies C1 from its two-state drift — emit_clock_rate is inert this run "
                      "(clock_model wins).");
        else if (network_emit_clock_rate_) {
            log->info("Network: network.emit_clock_rate on — C1 = finite-differenced solved clock "
                      "(reseed / drift-reset / C1-ZOH bound {:.0f} s); solution byte-identical, only "
                      "the emitted C1 changes. Consumer extrapolates c0 + c1·dt (SSR saw-tooth fix).",
                      network_clock_model_max_gap_s_);
            // Inert-arming guard: every FD pair spans one emitted-epoch gap, so if the
            // configured epoch spacing already exceeds the C1-ZOH bound, EVERY pair is
            // stale — the toggle silently ships C1=0 all run.  Only the config-determinable
            // spacing (gnss_rate_s × epoch_decimation_n) is checked here; with
            // gnss_rate_s = 0 the native RINEX rate is unknown until data is read, so a
            // decimated native-rate run (the 300 s dev tier) is not catchable at load.
            const double known_spacing_s =
                gnss_rate_s_ * static_cast<double>(network_config_.epoch_decimation_n);
            if (known_spacing_s > network_clock_model_max_gap_s_)
                log->info("Network: emit_clock_rate is INERT at this cadence — the epoch spacing "
                          "({:.0f} s = gnss_rate_s {:.0f} × epoch_decimation_n {}) exceeds the "
                          "{:.0f} s C1-ZOH bound (clock_model_max_gap_s), so every finite-difference "
                          "pair is stale and C1 stays 0 (ZOH). Raise clock_model_max_gap_s or run at "
                          "a higher rate (the rate asset is high-rate anyway — see "
                          "docs/CLOCK_HADAMARD_CALIB).",
                          known_spacing_s, gnss_rate_s_, network_config_.epoch_decimation_n,
                          network_clock_model_max_gap_s_);
        }
        // --- RT0-4: product generation-time metadata ---
        network_emit_generation_time_ =
            jval<bool>(json, "network.emit_generation_time", "network_emit_generation_time", false);
        if (network_emit_generation_time_)
            log->info("Network: network.emit_generation_time on — each emitted epoch carries a "
                      "wall-clock time-of-generation (G record) for consumer latency measurement. "
                      "Note: the product is NO LONGER byte-reproducible (wall-clock stamps differ "
                      "per run) — leave off for regression/byte-identity runs.");
        // --- RT0-5: per-constellation clock-runaway quarantine ---
        network_clock_runaway_check_ =
            jval<bool>(json, "network.clock_runaway_check", "network_clock_runaway_check", false);
        network_clock_runaway_mag_m_ =
            jval<double>(json, "network.clock_runaway_mag_m", "network_clock_runaway_mag_m", 1000.0);
        network_clock_runaway_rate_m_s_ =
            jval<double>(json, "network.clock_runaway_rate_m_s", "network_clock_runaway_rate_m_s", 10.0);
        if (network_clock_runaway_check_) {
            log->info("Network (RT0-5): clock-runaway quarantine on — a constellation whose median "
                      "|clock−global median| exceeds {:.0f} m or median |drift| exceeds {:.2f} m/s is "
                      "dropped from the epoch's emission and flagged (scope-limits a runaway).",
                      network_clock_runaway_mag_m_, network_clock_runaway_rate_m_s_);
            // P4: gated on !service_mode — the bundle arms network.clock_model below,
            // so under service_mode the rate ceiling is NOT inert.
            if (!network_clock_model_ && !service_mode)
                log->info("Network (RT0-5): the rate ceiling ({:.2f} m/s) is inert without "
                          "network.clock_model (no drift is estimated); the magnitude ceiling "
                          "({:.0f} m) still applies.", network_clock_runaway_rate_m_s_,
                          network_clock_runaway_mag_m_);
            // Degenerate single-constellation run: the global clock median IS that
            // constellation's own median, so the magnitude test measures only the
            // constellation's INTERNAL scatter about its own center — a common-mode
            // runaway (every satellite walking off together, the very BeiDou-style
            // fault the check targets) is invisible to it.  Only the rate ceiling
            // (which needs network.clock_model for a non-zero drift) can catch that.
            if (active_systems_.size() == 1)
                log->warn("Network (RT0-5): clock_runaway_check with a single active system ('{}') — "
                          "the magnitude test degenerates to the constellation's own-median scatter, "
                          "so a common-mode runaway of the whole constellation is invisible; only the "
                          "rate ceiling ({:.2f} m/s, requires network.clock_model) can catch it.",
                          active_systems_.front(), network_clock_runaway_rate_m_s_);
        }
        // --- RTA-10: product-level epoch-status heartbeat ---
        network_emit_epoch_status_record_ =
            jval<bool>(json, "network.emit_epoch_status_record",
                       "network_emit_epoch_status_record", false);
        if (network_emit_epoch_status_record_) {
            log->info("Network (RTA-10): network.emit_epoch_status_record on — one positional H "
                      "heartbeat line per epoch in the product file (including gate / σ̂0 "
                      "withholds, which otherwise emit nothing), so a tail consumer can tell "
                      "\"withheld\" from \"producer died\".  Withheld epochs that precede the "
                      "first emit are buffered and flushed when the deferred v2 writer opens, "
                      "so a leg opening inside a withhold era still gets wire coverage (a "
                      "never-emitting run WARNs at teardown instead — no file is fabricated).");
            // P4: gated on !service_mode — the bundle arms product_flush_per_epoch below.
            if (!network_config_.product_flush_per_epoch && !service_mode)
                log->info("Network (RTA-10): consider network.product_flush_per_epoch alongside "
                          "the heartbeat — a live tail only sees the H line when the stream "
                          "flushes.");
        }
        // --- H-rider (edge-queue §1.6 item 3a): census-fed H σ̂₀ ---
        // Parsed here (after emit_epoch_status_record); the dependency GUARD lives
        // in loadNetworkEmissionFilters_ — this helper is void, and the empty-mask
        // case is a hard load error, which only a bool helper can signal.
        network_h_sigma0_prefit_census_ =
            jval<bool>(json, "network.h_sigma0_prefit_census",
                       "network_h_sigma0_prefit_census", false);
        network_h_sigma0_classes_ =
            jval<std::string>(json, "network.h_sigma0_classes",
                              "network_h_sigma0_classes", "code,sh,other");
        // --- B1: URA quality channel ---
        network_emit_ura_ =
            jval<bool>(json, "network.emit_ura", "network_emit_ura", false);
        network_ura_ewma_alpha_ =
            jval<double>(json, "network.ura_ewma_alpha", "network_ura_ewma_alpha", 0.1);
        network_consume_ura_ =
            jval<bool>(json, "network.consume_ura", "network_consume_ura", false);
        // --- B1b: phase-bias σ channel (Tienstra product-covariance, phase-bias block) ---
        network_emit_phase_bias_sigma_ =
            jval<bool>(json, "network.emit_phase_bias_sigma", "network_emit_phase_bias_sigma", false);
        network_consume_phase_bias_sigma_ =
            jval<bool>(json, "network.consume_phase_bias_sigma", "network_consume_phase_bias_sigma", false);
        // --- M4 (contract v2): combined-correction σ channel (Zhang 2021 / Psychas 2024b) ---
        network_emit_combined_sigma_ =
            jval<bool>(json, "network.emit_combined_sigma", "network_emit_combined_sigma", false);
        network_combined_sigma_agek_ =
            jval<double>(json, "network.combined_sigma_agek", "network_combined_sigma_agek", 0.0);
        // T3a: consumer AR-eligibility veto on the product signal_integer flag (default OFF).
        network_require_signal_integer_for_ar_ =
            jval<bool>(json, "network.require_signal_integer_for_ar",
                       "network_require_signal_integer_for_ar", false);
        if (network_emit_phase_bias_sigma_)
            log->info("Network: network.emit_phase_bias_sigma on — per-(sat,signal) phase-bias "
                      "formal σ (EWMA(σ̂0)×√shared-cov, gauge-exact) written to SsrPhaseBiasEntry."
                      "sigma_m; consumer R-inflation {}.  (B1b: Tienstra phase-bias block of Qŷ_B1.)",
                      network_consume_phase_bias_sigma_ ? "ON" : "off");
        if (network_emit_combined_sigma_)
            log->info("Network: network.emit_combined_sigma on — per-key COMBINED-correction σ "
                      "(sigc=clock+phasebias, sigcc=clock+codebias, cross-cov included) + code/phase "
                      "correlation rho written to the sigc=/sigcc=/rho= wire tokens{}.  Consumer folds "
                      "them (age-inflated) instead of the URA+phase-bias-σ RSS when "
                      "closed_loop.consume_combined_sigma is on.  (M4: Zhang 2021 / Psychas 2024b — "
                      "combined σ is mm-level where the individual σ are cm-level during "
                      "rise-reconvergence.)",
                      network_combined_sigma_agek_ != 0.0
                          ? fmt::format("; age scaling agek={} on the S-line", network_combined_sigma_agek_)
                          : std::string(" (no age scaling: combined_sigma_agek=0)"));
        // R5: network_ura_staleness_q2_ (which defaults to network_clock_model_q2_) is captured
        // AFTER the clock_model validity-restore below, so it inherits the RESTORED q2 (not a
        // rejected-then-reset value).  See the load just past that block.
        network_ura_retro_bump_ =
            jval<bool>(json, "network.ura_retro_bump", "network_ura_retro_bump", false);
        network_mark_refsat_reset_disc_ =
            jval<bool>(json, "network.mark_refsat_reset_disc",
                       "network_mark_refsat_reset_disc", false);
        network_ura_refloat_inflate_ =
            jval<double>(json, "network.ura_refloat_inflate", "network_ura_refloat_inflate", 1.0);
        // (RTB-8: service_mode was read at the very top of this section so ALL
        // coherence warnings — including those above this point — can gate on it.)
        if (network_emit_ura_)
            log->info("Network: network.emit_ura on — per-satellite URA quality channel "
                      "(EWMA α {:.2f}) written to the product; consumer R-inflation {}.",
                      network_ura_ewma_alpha_, network_consume_ura_ ? "ON" : "off");
        if (network_emit_ura_ && !network_raim_ && !service_mode)
            log->warn("Network: network.emit_ura is on but network.raim is off — the σ̂0 EWMA that "
                      "scales the URA is only updated by the post-fit RAIM pass, so the emitted URA "
                      "is the FORMAL clock σ only (no robust variance-factor scaling).  Enable "
                      "network.raim for σ̂0-scaled URA.");
        // RTB-7(b): the warmup URA floor rides the URA channel — without emit_ura
        // the URA pass never runs and the floor is a SILENT NO-OP.  Say so.
        // P4: under service_mode the bundle arms emit_ura below, so the floor is
        // NOT a no-op — fall through to the ramp-ON info instead of the warn.
        if (network_config_.warmup_ura_floor_m > 0.0 && !network_emit_ura_ && !service_mode)
            log->warn("Network (RTB-7): network.warmup_ura_floor_m {:.1f} is set but "
                      "network.emit_ura is off — the startup quality ramp floors the EMITTED "
                      "URA, so without the URA channel it is a silent no-op.  Enable "
                      "network.emit_ura.", network_config_.warmup_ura_floor_m);
        else if (network_config_.warmup_ura_floor_m > 0.0)
            log->info("Network (RTB-7): startup quality ramp ON — emitted URA floored at "
                      "{:.1f} m until n_emitted >= max(warmup_epochs {}, "
                      "phase_leveling_warmup_epochs) AND the σ̂0 EWMA has received >= {} "
                      "updates (count waived when the EWMA is structurally unavailable); "
                      "cold-start-to-product-grade is logged at the lift.",
                      network_config_.warmup_ura_floor_m, network_config_.warmup_epochs,
                      network_config_.warmup_epochs);
        if (network_ura_ewma_alpha_ <= 0.0 || network_ura_ewma_alpha_ > 1.0) {
            log->warn("Network: network.ura_ewma_alpha {} out of (0,1]; clamped to 0.1.",
                      network_ura_ewma_alpha_);
            network_ura_ewma_alpha_ = 0.1;
        }
        if (network_clock_model_ &&
            (!(network_clock_model_q2_ > 0.0) || network_clock_model_q1_ < 0.0 ||
             !(network_clock_model_k_sigma_ > 0.0) || !(network_clock_model_meas_std_m_ > 0.0))) {
            log->warn("Network: invalid network.clock_model_* parameters (q1={}, q2={}, k_sigma={}, "
                      "meas_std={}); restoring defaults (q1=0, q2=1.5e-7, k_sigma=6, meas_std=0.05).",
                      network_clock_model_q1_, network_clock_model_q2_,
                      network_clock_model_k_sigma_, network_clock_model_meas_std_m_);
            network_clock_model_q1_ = 0.0; network_clock_model_q2_ = 1.5e-7;
            network_clock_model_k_sigma_ = 6.0; network_clock_model_meas_std_m_ = 0.05;
        }
        // R5: capture the ZOH clock-staleness q² AFTER the validity-restore above so its default
        // inherits the RESTORED network_clock_model_q2_ (a rejected clock_model_q2 was reset to
        // 1.5e-7 there; capturing before would have propagated the bad value into the default).
        // P1-3: defaulting to clock_model_q2 (not 0) keeps the age inflation active whenever
        // network.consume_ura is on; an explicit 0 disables it.  Consumer-side only.
        network_ura_staleness_q2_ =
            jval<double>(json, "network.ura_staleness_q2", "network_ura_staleness_q2",
                         network_clock_model_q2_);
        // A negative value (typo, or inherited from a user-set bad clock_model_q2 when
        // clock_model is OFF and the validity-restore above never ran) is inert at the
        // consumer (q2 > 0 guard) — surface it instead of silently disabling inflation.
        if (network_ura_staleness_q2_ < 0.0) {
            log->warn("Network: network.ura_staleness_q2 {} < 0 → 0 (ZOH staleness inflation "
                      "off; check network.clock_model_q2, which supplies the default).",
                      network_ura_staleness_q2_);
            network_ura_staleness_q2_ = 0.0;
        }
        // R3: ON-only summary lines for the B1 producer/consumer knobs that previously logged
        // nothing (emit_ura already logs above).  Purely informational; no behaviour change.
        if (network_ura_retro_bump_)
            log->info("Network (B1): network.ura_retro_bump on — a satellite flagged by any "
                      "post-emit QC tier gets its discontinuity counter bumped on the NEXT emit "
                      "(edge-triggered, per-satellite) so the consumer resets the ambiguity that "
                      "integrated the contaminated epoch.");
        if (network_mark_refsat_reset_disc_)
            log->info("Network (edge-1 piece 2): network.mark_refsat_reset_disc on — an IF-TD "
                      "REFERENCE-path slip marks that SATELLITE with a per-sat discontinuity at "
                      "its next actual emit, serialized as an R record (not reconstructable on "
                      "replay, so it must travel on the wire).  Targets the subset the emitter's "
                      "value-jump test cannot see: SPEC 25.3 measured excursion onsets as "
                      "refsat_reset-enriched 5.0x with PB value steps = 0.  PRODUCT-AFFECTING "
                      "(adds R records); grade with scripts/product_step_census.py + "
                      "scripts/edge_reaction_correlate.py.");
        if (network_ura_refloat_inflate_ > 1.0)
            log->info("Network (B1): network.ura_refloat_inflate {:.2f}x — a recently-refloated "
                      "satellite's URA is inflated for the phase-leveling warmup horizon (honest "
                      "surrogate for the post-refloat DIA conditioning penalty).",
                      network_ura_refloat_inflate_);
        if (network_consume_ura_ && network_ura_staleness_q2_ > 0.0)
            log->info("Network (B1): consumer ZOH clock-staleness q2 = {:.3e} m^2/s^3 (folded as "
                      "q2*dt^3/3 into the applied clock variance as SSR corrections age).",
                      network_ura_staleness_q2_);
        if (network_clock_model_ && network_clock_monitor_) {
            log->info("Network: network.clock_model on — the two-state oscillator normalized-"
                      "innovation test supersedes the clock_monitor absolute/jump bounds for "
                      "flagging; clock_monitor reinit action (refloat/inflate-q) still applies.");
        }
        if (network_clock_monitor_ &&
            (!(network_clock_monitor_abs_max_m_ > 0.0) || !(network_clock_monitor_jump_max_m_ > 0.0))) {
            log->warn("Network: network.clock_monitor_{{abs_max_m,jump_max_m}} must be > 0 "
                      "(got {}, {}); a non-positive bound would flag every satellite — "
                      "restoring defaults 20 / 10 m.",
                      network_clock_monitor_abs_max_m_, network_clock_monitor_jump_max_m_);
            network_clock_monitor_abs_max_m_ = 20.0;
            network_clock_monitor_jump_max_m_ = 10.0;
        }
    }

    // =================================================================
    //  network subsection helper: loadNetworkDatumSignalling_
    // =================================================================
    /**
     * @brief Datum realization + discontinuity signalling knobs, and the Card-3/Card-4 datum family.
     *
     * Raw parse only for the clock gauge (pivot / sum-to-zero / broadcast-anchored), the
     * phase-bias gauge (prioritized / ensemble / eq34) and @c network.carry_invariant*; the
     * mutexes and ON reports live in @c validateNetworkDatumCoherence_ and
     * @c validateNetworkPhaseDatum_.  The D5 v3 corroboration gate resolves here because it
     * auto-arms the slip-QC instrumentation that feeds it.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkDatumSignalling_(const nlohmann::json& json,
                                                 const std::shared_ptr<spdlog::logger>& log) {
        // Datum-change signalling (A1/A2): vanish-return pivot handover detection +
        // per-satellite clock discontinuity channel with per-constellation bump scoping.
        network_pivot_vanish_return_signal_ =
            jval<bool>(json, "network.pivot_vanish_return_signal",
                       "network_pivot_vanish_return_signal", false);
        network_emit_clock_disc_ =
            jval<bool>(json, "network.emit_clock_disc", "network_emit_clock_disc", false);
        // Optional emitter discontinuity channels (both default OFF, both travel
        // in the S-contract so file replay reproduces the counters).
        network_code_bias_disc_jump_test_ =
            jval<bool>(json, "network.code_bias_disc_jump_test",
                       "network_code_bias_disc_jump_test", false);
        network_clock_disc_on_set_change_ =
            jval<bool>(json, "network.clock_disc_on_set_change",
                       "network_clock_disc_on_set_change", false);
        // H3: per-λ discontinuity threshold as a fraction of a carrier cycle
        // (frac·λ jump gate).  0 = legacy flat 0.05 m (byte-identical).
        network_disc_threshold_frac_cycles_ =
            jval<double>(json, "network.disc_threshold_frac_cycles",
                         "network_disc_threshold_frac_cycles", 0.0);
        // RTA-1/RTA-2: sticky clock-pivot selection + per-constellation scoping of the
        // producer's INTERNAL handover response (carry skip / oscillator clear).
        network_sticky_pivot_ =
            jval<bool>(json, "network.sticky_pivot", "network_sticky_pivot", false);
        // Anchor-free clock gauge: fix each constellation's clock common-mode with a
        // sum-to-zero over its own satellite clocks (folded like the code-bias gauge)
        // instead of a hard pivot pin.  Removes the pivot/handover machinery entirely
        // for the clock; ignores network.clock_datum_satellite (no pivot to place).
        network_clock_datum_sum_to_zero_ =
            jval<bool>(json, "network.clock_datum_sum_to_zero",
                       "network_clock_datum_sum_to_zero", false);
        // Broadcast-anchored clock gauge (brdc_rts only): per-satellite soft
        // anchors to each satellite's OWN broadcast clock ARE the datum (folded
        // pre-gate).  No pivot, no handover seam, and — unlike sum-to-zero, whose
        // raw-set reference redistributes onto the survivors as satellites
        // rise/set — no common-mode wander: the absolute clock-product frame
        // holds still against the broadcast reference (the RT datum-stability
        // contract).
        network_clock_datum_broadcast_anchored_ =
            jval<bool>(json, "network.clock_datum_broadcast_anchored",
                       "network_clock_datum_broadcast_anchored", false);
        // Time-consistent (δtˢ, dˢ) emission: HOLD the per-satellite clock↔code-bias
        // IF-pin reference signal pair fixed across epochs (latch on first sight,
        // re-latch only on genuine loss) instead of re-selecting the two lowest
        // distinct-frequency signals from each epoch's coverage.  Fixes the per-sat
        // clock re-anchoring (by the satellite's inter-channel code bias) that a
        // sparse channel entering/leaving ≥2-station coverage otherwise triggers —
        // the differential-clock wander a receiver clock cannot absorb.  Only the
        // W6 code-bias IF pin realizes this split, so the toggle is inert without
        // network.estimate_sat_code_bias.
        network_clock_signal_basis_hold_ =
            jval<bool>(json, "network.clock_signal_basis_hold",
                       "network_clock_signal_basis_hold", false);
        // Card 2: impose the W6 code-bias datum (per-sat IF pin + per-(const,signal)
        // sum-to-zero) as a TIME datum — pin the CHANGE of the previous epoch's solved
        // values instead of the level to zero.  Same row count; cures the set-mean
        // wander/relaxation after every rise/set (defect 6-7).  Validated below (both-
        // armed error + inert warn) once estimate_sat_code_bias coupling has resolved.
        network_code_datum_time_linked_ =
            jval<bool>(json, "network.code_datum_time_linked",
                       "network_code_datum_time_linked", false);
        network_scoped_handover_response_ =
            jval<bool>(json, "network.scoped_handover_response",
                       "network_scoped_handover_response", false);
        network_handover_stransform_ =
            jval<bool>(json, "network.handover_stransform",
                       "network_handover_stransform", false);
        // Phase-bias gauge: single clock-pivot station (legacy) vs per-(sat,signal)
        // prioritized anchor receiver.  The two sub-knobs (coverage-preference
        // elevation, per-(sat,signal) stickiness) are INERT unless the master toggle
        // is on.
        network_phase_datum_prioritized_ =
            jval<bool>(json, "network.phase_datum_prioritized",
                       "network_phase_datum_prioritized", false);
        // Fix A: anchor-free ensemble (sum-to-zero over receivers) phase-bias gauge.
        // Reuses the prioritized SPOF-cure machinery (no pivot-first, every station
        // carries its arcs) but elects no anchor and folds Σ_r zˢ_{r,σ}=0 through the
        // kernel instead of a single-anchor pin.
        network_phase_datum_ensemble_ =
            jval<bool>(json, "network.phase_datum_ensemble",
                       "network_phase_datum_ensemble", false);
        // Card 3: Eq-34 / de Jonge single-receiver-chain phase-bias datum.
        // Reuses the prioritized SPOF-cure machinery (no pivot-first, every
        // station carries its arcs) but replaces the anchor election with a
        // spanning-tree chain election over per-(const,signal) components +
        // per-(receiver,system,signal) RPB split states.  Two sub-knobs
        // (preferred datum receiver, re-pin rounding) are INERT unless the
        // master toggle is on.
        network_phase_datum_eq34_ =
            jval<bool>(json, "network.phase_datum_eq34",
                       "network_phase_datum_eq34", false);
        network_phase_datum_eq34_receiver_ =
            jval<std::string>(json, "network.phase_datum_eq34_receiver",
                              "network_phase_datum_eq34_receiver", "");
        network_phase_datum_eq34_pin_round_ =
            jval<bool>(json, "network.phase_datum_eq34_pin_round",
                       "network_phase_datum_eq34_pin_round", true);
        network_phase_datum_eq34_seed_round_ =
            jval<bool>(json, "network.phase_datum_eq34_seed_round",
                       "network_phase_datum_eq34_seed_round", false);
        if (network_phase_datum_eq34_seed_round_ && !network_phase_datum_eq34_) {
            log->warn("Network: network.phase_datum_eq34_seed_round requires "
                      "network.phase_datum_eq34 — forced OFF (the seed-round is a "
                      "chain-datum pin-target rule; there are no chain pins to "
                      "re-target without the chain).");
            network_phase_datum_eq34_seed_round_ = false;
        }
        // SEED-ROUND INC-2 (docs/SPEC_eq34_seed_round.md §8.5): frame-continuous
        // re-host.  Strictly a refinement of the seed-round target rule — it needs
        // the seed branch's state read to exist at all — so it is refused without
        // it rather than silently arming a branch that can never be reached.
        network_phase_datum_eq34_seed_round_rehost_frame_ =
            jval<bool>(json, "network.phase_datum_eq34_seed_round_rehost_frame",
                       "network_phase_datum_eq34_seed_round_rehost_frame", false);
        if (network_phase_datum_eq34_seed_round_rehost_frame_
            && !network_phase_datum_eq34_seed_round_) {
            log->warn("Network: network.phase_datum_eq34_seed_round_rehost_frame "
                      "requires network.phase_datum_eq34_seed_round — forced OFF "
                      "(INC-2 computes the re-host frame step from the SEED-ROUND "
                      "state read; without the seed branch there is no pre-pin "
                      "value to step from and the re-host stays unknown).");
            network_phase_datum_eq34_seed_round_rehost_frame_ = false;
        }
        network_phase_datum_eq34_rpb_prior_sigma_m_ =
            jval<double>(json, "network.phase_datum_eq34_rpb_prior_sigma_m",
                         "network_phase_datum_eq34_rpb_prior_sigma_m", 1.0);
        network_phase_datum_eq34_rpb_q_m2s_ =
            jval<double>(json, "network.phase_datum_eq34_rpb_q_m2s",
                         "network_phase_datum_eq34_rpb_q_m2s",
                         network_config_.process_noise.get(NetworkQClass::ChainDatumRpb));
        network_phase_datum_eq34_carry_shift_ =
            jval<bool>(json, "network.phase_datum_eq34_carry_shift",
                       "network_phase_datum_eq34_carry_shift", false);
        network_phase_datum_eq34_longevity_election_ =
            jval<bool>(json, "network.phase_datum_eq34_longevity_election",
                       "network_phase_datum_eq34_longevity_election", false);
        network_phase_datum_eq34_extend_only_ =
            jval<bool>(json, "network.phase_datum_eq34_extend_only",
                       "network_phase_datum_eq34_extend_only", false);
        network_phase_datum_eq34_slip_aware_election_ =
            jval<bool>(json, "network.phase_datum_eq34_slip_aware_election",
                       "network_phase_datum_eq34_slip_aware_election", false);
        // Datum-station slip detection is a LEGACY-pivot-datum mechanism: every
        // no-pivot datum (prioritized / ensemble / eq34) runs the slip loops from
        // i_lo = 0 already, so station 0 has ordinary coverage there and the
        // dedicated path never engages (runtime-guarded).  Parsed in
        // loadNetworkSlipDetectors_, which runs BEFORE this helper.
        if (network_cycle_slip_datum_station_detect_
            && (network_phase_datum_prioritized_ || network_phase_datum_ensemble_
                || network_phase_datum_eq34_)) {
            log->info("Network: network.cycle_slip.datum_station_detect is INERT under a "
                      "no-pivot phase datum (prioritized/ensemble/eq34) — station 0 is an "
                      "ordinary station there and every slip loop already covers it.");
        }
        if (network_phase_datum_eq34_slip_aware_election_) {
            if (!network_phase_datum_eq34_) {
                log->warn("Network: network.phase_datum_eq34_slip_aware_election is set but "
                          ".phase_datum_eq34 is OFF — there is no chain spanning tree to keep "
                          "slipped arcs out of, so it is INERT this run.");
                network_phase_datum_eq34_slip_aware_election_ = false;
            } else {
                log->warn("Network (Card 3/4): SLIP-AWARE eq34 election ON — ⚠ MEASURED "
                          "NET-HARMFUL, see docs/SPEC_d5_churn_limit_cycle.md.  The MECHANISM "
                          "works: on a 3 h 63-sta DOY-129 leg it de-seeded 102 slipped "
                          "incumbents against only 9 unavoidable re-admissions (ratio 0.09 — the "
                          "graph nearly always had an alternative) and cut chord refloats 48%.  "
                          "But de-seating an incumbent IS a datum handover, and 102 of them cost "
                          "total_IF spread_med +15.7% (7.871 -> 9.104), worst_med +11.6% and "
                          "clk_std +20% against the same-build baseline — an order more than the "
                          "<=1% band every D5 arm on that leg sat in.  The graph-theoretic win is "
                          "real and smaller than the chain-datum churn it pays for.  Arm this "
                          "only to re-measure, never as a default.");
                // NOTE: the "…without carry_invariant" advisory canNOT live here.
                // network_carry_invariant_ is parsed a few lines BELOW this block, so it is
                // still false at this point and the warning fires on every run — measured
                // 2026-08-02 on a config that plainly arms carry_invariant.  It lives in
                // the cross-knob validation pass instead, where every flag is settled.
            }
        }
        // Card 4 (M1): the DD cycle-basis invariant carry (network.carry_invariant*).
        network_carry_invariant_ =
            jval<bool>(json, "network.carry_invariant",
                       "network_carry_invariant", false);
        network_carry_invariant_qz_ =
            jval<double>(json, "network.carry_invariant_qz",
                         "network_carry_invariant_qz",
                         network_config_.process_noise.get(NetworkQClass::DdCarry));
        network_carry_invariant_emit_rebase_ =
            jval<bool>(json, "network.carry_invariant_emit_rebase",
                       "network_carry_invariant_emit_rebase", true);
        network_carry_invariant_honesty_ =
            jval<bool>(json, "network.carry_invariant_honesty",
                       "network_carry_invariant_honesty", false);
        network_carry_invariant_innov_gate_ =
            jval<bool>(json, "network.carry_invariant_innov_gate",
                       "network_carry_invariant_innov_gate", false);
        // Standing invariant #1 interlock (config-level): a zero-q invariant carry
        // with the innovation gate OFF is a stiff time-linked quantity whose ONLY
        // event responses are the pre-processor slip detectors — a jump they miss
        // is carried at full weight forever and warps the solution silently (the
        // SPEC_synthetic_minimum_network §13.7 runaway arm is exactly this
        // posture, and all 7 sim carry_invariant configs shipped it unwarned —
        // SPEC_o2_carry_invariant_accumulator hygiene item).  WARN, do not
        // disarm: silently arming a solve-affecting gate would be worse, and
        // qz > 0 is an equally valid cure.  Both flags are parsed-final here
        // (network_carry_invariant_ is written once, never cleared).
        if (network_carry_invariant_ && network_carry_invariant_qz_ <= 0.0
            && !network_carry_invariant_innov_gate_) {
            log->warn("Network (Card 4 / invariant #1): carry_invariant is ON with "
                      "carry_invariant_qz = {} and carry_invariant_innov_gate OFF — a "
                      "zero-q carried cycle basis with NO wired jump response beyond the "
                      "pre-processor slip detectors.  A missed jump is carried stiff "
                      "indefinitely.  Arm network.carry_invariant_innov_gate or set a "
                      "physical carry_invariant_qz before any ON field run.",
                      network_carry_invariant_qz_);
        }
        network_carry_invariant_innov_k_ =
            jval<double>(json, "network.carry_invariant_innov_k",
                         "network_carry_invariant_innov_k", 4.0);
        network_carry_invariant_innov_refold_ =
            jval<bool>(json, "network.carry_invariant_innov_refold",
                       "network_carry_invariant_innov_refold", false);
        network_carry_invariant_innov_calibrate_ =
            jval<bool>(json, "network.carry_invariant_innov_calibrate",
                       "network_carry_invariant_innov_calibrate", false);
        network_carry_invariant_innov_calibrate_window_ =
            jval<double>(json, "network.carry_invariant_innov_calibrate_window",
                         "network_carry_invariant_innov_calibrate_window", 60.0);
        // Card 4 (M1) D5 v3: raw-observable corroboration gate on the D5 re-tap.
        network_carry_invariant_innov_corroborate_ =
            jval<bool>(json, "network.carry_invariant_innov_corroborate",
                       "network_carry_invariant_innov_corroborate", false);
        network_carry_invariant_innov_corroborate_backstop_k_ =
            jval<double>(json, "network.carry_invariant_innov_corroborate_backstop_k",
                         "network_carry_invariant_innov_corroborate_backstop_k", 0.0);
        if (network_carry_invariant_innov_corroborate_) {
            // Two ways this arms into silence — both are standing-invariant-#4 traps
            // ("clean log, dead wiring"), so neither is allowed to pass quietly.
            if (!network_carry_invariant_innov_gate_) {
                log->warn("Network (Card 4 D5 v3): network.carry_invariant_innov_corroborate is "
                          "set but .carry_invariant_innov_gate is OFF — there are no D5 re-taps "
                          "to corroborate, so the gate is INERT this run.");
                network_carry_invariant_innov_corroborate_ = false;
            } else {
                // The suppression evidence is the slip-QC QUALIFY pass, and the card that
                // makes that pass run is auto-armed from the SETTLED-state interlock at the
                // end of loadNetworkSection_ — NOT here.  One of cardsArmed()'s four inputs
                // (network_carry_iono_) is still the RAW parsed value at this point and is
                // cleared by an interlock that runs later, so a test here would read a value
                // that never reaches the estimator.  @see the "Card 4 (M1) D5 v3/v4 EVIDENCE
                // interlock" block for the full derivation.
                log->info("Network (Card 4 D5 v3): D5 re-tap corroboration ON — a flagged chord "
                          "is re-tapped UNLESS the raw observables positively measured its arc "
                          "as continuous.  No-evidence still re-taps (invariant #1).  Magnitude "
                          "backstop {} (0 = pure corroboration).  Watch the run-end "
                          "innov_suppressed counter: 0 means the flat set never arrived.",
                          network_carry_invariant_innov_corroborate_backstop_k_);
            }
        }
        if (network_carry_invariant_innov_corroborate_backstop_k_ < 0.0) {
            log->warn("Network (Card 4 D5 v3): network.carry_invariant_innov_corroborate_backstop_k "
                      "= {} is negative; clamping to 0 (pure corroboration).",
                      network_carry_invariant_innov_corroborate_backstop_k_);
            network_carry_invariant_innov_corroborate_backstop_k_ = 0.0;
        }
        // Card 4 (M1) D5 v5: only CARRIED chords are gated (a born chord has no H0).
        network_carry_invariant_innov_carried_only_ =
            jval<bool>(json, "network.carry_invariant_innov_carried_only",
                       "network_carry_invariant_innov_carried_only", false);
        if (network_carry_invariant_innov_carried_only_) {
            if (!network_carry_invariant_innov_gate_) {
                log->warn("Network (Card 4 D5 v5): network.carry_invariant_innov_carried_only "
                          "is set but .carry_invariant_innov_gate is OFF — there is no gate to "
                          "restrict, so it is INERT this run.");
                network_carry_invariant_innov_carried_only_ = false;
            } else {
                log->info("Network (Card 4 D5 v5): CARRIED-ONLY innovation gate ON — a chord "
                          "BORN this epoch is no longer tested.  The gate standardizes against "
                          "Cov(v), the H0 covariance about the CARRIED prior; a fresh chord's "
                          "prior is the diffuse fresh-chord variance, so there is no hypothesis "
                          "to test, its 'reset' is a no-op (a diffuse chord's posterior already "
                          "IS the data-only tap-off), and the flag's only lasting effect is the "
                          "funnel entry that re-borns it next epoch.  Measured on a 3 h 63-sta "
                          "DOY-129 leg: 95.3% of all D5 flags were born chords at exactly the "
                          "diffuse sigma, median |v| 19.2 km, some at duty 0.99 — the churn's "
                          "engine.  v2 already excluded born chords from the sigma0 accumulator "
                          "for the same reason; this applies it to the DECISION.  Watch the "
                          "run-end born-skipped counter.");
            }
        }
        // Card 4 (M1) PD-SAFE DD fold (docs/SPEC_pd_safe_dd_fold.md).  Parse only — the
        // "set without carry_invariant" advisory lives in the CROSS-KNOB validation pass
        // (@ref validateNetworkDatumCoherence_) where every flag is settled, NOT here:
        // network_carry_invariant_ is parsed ABOVE this block only by luck of ordering and
        // the churn card already paid for that trap once (measured 2026-08-02).
        network_carry_invariant_pd_safe_ =
            jval<bool>(json, "network.carry_invariant_pd_safe",
                       "network_carry_invariant_pd_safe", false);
        // Carry-aware covariance readback (docs/SPEC_carry_aware_covariance.md).  Same
        // parse-only discipline and for the same reason as the line above.
        network_carry_aware_covariance_ =
            jval<bool>(json, "network.carry_aware_covariance",
                       "network_carry_aware_covariance", false);
        // Card 4 (M1) D5 v4: the flag re-taps IN-EPOCH and stops there — the funnel
        // entry no longer excludes the arc from next epoch's carry membership.
        network_carry_invariant_innov_retap_only_ =
            jval<bool>(json, "network.carry_invariant_innov_retap_only",
                       "network_carry_invariant_innov_retap_only", false);
        if (network_carry_invariant_innov_retap_only_) {
            if (!network_carry_invariant_innov_gate_) {
                log->warn("Network (Card 4 D5 v4): network.carry_invariant_innov_retap_only is set "
                          "but .carry_invariant_innov_gate is OFF — there are no D5 flags to keep "
                          "out of the funnel, so it is INERT this run.");
                network_carry_invariant_innov_retap_only_ = false;
            } else {
                // Same arming gap as v3, closed the same way: the card that makes the
                // QUALIFY pass run is auto-armed from the SETTLED-state interlock at the end
                // of loadNetworkSection_, where all four of cardsArmed()'s inputs are final.
                // @see the "Card 4 (M1) D5 v3/v4 EVIDENCE interlock" block.
                log->info("Network (Card 4 D5 v4): D5 re-tap-ONLY ON — a flagged chord whose arc "
                          "the raw observables MEASURED as continuous is reset in-epoch and its "
                          "arc is NOT excluded from next epoch's carry membership, so the gate "
                          "stops discarding the DD it just re-tapped.  A chord arc has cycle "
                          "support 1 under a stable tree, so on that population the exclusion "
                          "protected nothing.  FAIL-SAFE: stepped and unmeasurable arcs still "
                          "refloat, and so does any arc a raw detector flags in the current epoch "
                          "— the refloat is what COMPLETES a real slip's reset (measured 0.48 cyc, "
                          "GATE 10b), so invariant #1 is narrowed, never removed.  The funnel "
                          "INSERT is unchanged: the Iono-2 v3k erase classifier, the WL-3 MW-row "
                          "slip-skip and every other consumer still see the entry.  Watch the "
                          "run-end suppressed-kill counter: 0 means nothing reached it.");
                if (network_carry_invariant_innov_corroborate_)
                    log->warn("Network (Card 4 D5 v4): .carry_invariant_innov_corroborate is ALSO "
                              "armed.  v3 suppresses the measured-and-flat flags one level "
                              "earlier, so they never reach the funnel and this knob is largely "
                              "INERT by construction — expect a near-zero suppressed-kill count.  "
                              "The two are alternative responses to the same population (leave "
                              "the DD alone vs re-centre it on the data); arm ONE per arm to keep "
                              "the A/B a single treatment.");
            }
        }
    }

    // =================================================================
    //  network subsection helper: loadNetworkReattachHygiene_
    // =================================================================
    /**
     * @brief eq34 reattach hygiene INC-1/2/3 (docs/SPEC_eq34_reattach_hygiene.md).
     *
     * Ordered here because every guard needs BOTH the phase-datum mode and
     * @c network_carry_invariant_ (the slip-funnel feed), which the block above parsed.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkReattachHygiene_(const nlohmann::json& json,
                                                 const std::shared_ptr<spdlog::logger>& log) {
        // --- eq34 REATTACH HYGIENE (docs/SPEC_eq34_reattach_hygiene.md) --------
        // Placed here because every guard below needs BOTH the phase-datum mode
        // (parsed just above) and network_carry_invariant_ (the slip-funnel feed).
        // All three increments are default-OFF and, OFF, allocate no state and fold
        // no rows.  Deliberately NOT appended to the warm-resume fingerprint (see the
        // NetworkConfig doxygen and networkSnapshotFingerprint): forward-only
        // election/row policy, so an OFF and an ON leg may share one snapshot — which
        // is what makes the §6 shared-snapshot event A/B legal.
        network_config_.eq34_anchor_min_arc_ep =
            jval<int>(json, "network.eq34_anchor_min_arc_ep",
                      "network_eq34_anchor_min_arc_ep", 0);
        network_config_.eq34_prior_realization_max_gap_s =
            jval<double>(json, "network.eq34_prior_realization_max_gap_s",
                         "network_eq34_prior_realization_max_gap_s", 1800.0);
        network_config_.phase_datum_time_linked =
            jval<bool>(json, "network.phase_datum_time_linked",
                       "network_phase_datum_time_linked", false);
        network_config_.phase_datum_bridge_sigma_m =
            jval<double>(json, "network.phase_datum_bridge_sigma_m",
                         "network_phase_datum_bridge_sigma_m", 0.05);
        network_config_.phase_datum_bridge_max_ep =
            jval<int>(json, "network.phase_datum_bridge_max_ep",
                      "network_phase_datum_bridge_max_ep", 20);
        network_config_.eq34_fresh_pin_sigma_scale =
            jval<double>(json, "network.eq34_fresh_pin_sigma_scale",
                         "network_eq34_fresh_pin_sigma_scale", 1.0);
        network_config_.eq34_fresh_pin_ramp_ep =
            jval<int>(json, "network.eq34_fresh_pin_ramp_ep",
                      "network_eq34_fresh_pin_ramp_ep", 0);
        if (network_config_.eq34_anchor_min_arc_ep < 0) {
            log->warn("Network (eq34 hygiene): network.eq34_anchor_min_arc_ep = {} is "
                      "negative; clamping to 0 (legacy — no eligibility gate).",
                      network_config_.eq34_anchor_min_arc_ep);
            network_config_.eq34_anchor_min_arc_ep = 0;
        }
        if (network_config_.eq34_fresh_pin_ramp_ep < 0)
            network_config_.eq34_fresh_pin_ramp_ep = 0;
        if (!(network_config_.eq34_fresh_pin_sigma_scale >= 1.0)) {
            // < 1 would TIGHTEN an already-hard gauge pin (meaningless) and <= 0 would
            // invert the row weight — refuse both rather than fold a nonsense datum.
            log->warn("Network (eq34 hygiene): network.eq34_fresh_pin_sigma_scale = {} "
                      "is not >= 1 (a datum pin can only be LOOSENED by the ramp); "
                      "resetting to 1.0 (off).",
                      network_config_.eq34_fresh_pin_sigma_scale);
            network_config_.eq34_fresh_pin_sigma_scale = 1.0;
        }
        {
            const bool hyg_any = network_config_.eq34_anchor_min_arc_ep > 0
                              || network_config_.phase_datum_time_linked
                              || (network_config_.eq34_fresh_pin_sigma_scale > 1.0
                                  && network_config_.eq34_fresh_pin_ramp_ep > 0);
            // The card acts on the per-(sat,signal) ANCHOR election.  The ensemble
            // datum elects no anchor at all (Σ_r z = 0 over every receiver), so there
            // is nothing to re-host and nothing to bridge — INERT, disarm loudly.
            const bool anchored_datum = (network_phase_datum_prioritized_
                                         || network_phase_datum_eq34_)
                                        && !network_phase_datum_ensemble_;
            if (hyg_any && !anchored_datum) {
                log->warn("Network (eq34 hygiene): the reattach-hygiene knobs are set "
                          "but this run has no per-(sat,signal) anchor election "
                          "(needs network.phase_datum_eq34 or "
                          "network.phase_datum_prioritized, and NOT "
                          "phase_datum_ensemble) — there is no reattach to guard, so "
                          "they are INERT (disabled).");
                network_config_.eq34_anchor_min_arc_ep = 0;
                network_config_.phase_datum_time_linked = false;
                network_config_.eq34_fresh_pin_sigma_scale = 1.0;
                network_config_.eq34_fresh_pin_ramp_ep = 0;
            } else if (hyg_any) {
                if (network_config_.eq34_anchor_min_arc_ep > 0)
                    log->info("Network (eq34 hygiene INC-1): anchor-host ELIGIBILITY "
                              "armed — a (sat,signal) key may only be RE-HOSTED onto an "
                              "arc with >= {} consecutive present epoch(s).  The "
                              "incumbent host is exempt (keeping it is not a reattach) "
                              "and a FIRST-SIGHTING key is never gated (cold start is "
                              "unchanged).  With no eligible host the key {} this "
                              "epoch.  WARM-CONDITIONED: the block applies only while "
                              "the key's last EMITTED realization is younger than "
                              "{:.0f} s{} — a restart or a longer outage has no warm "
                              "memory for a cold pin to fight, so it converges the "
                              "legacy way instead of extending the outage.",
                              network_config_.eq34_anchor_min_arc_ep,
                              network_config_.phase_datum_time_linked
                                  ? "is BRIDGED (INC-2)"
                                  : "sits OUT of the anchored set (its bias folds into "
                                    "each station's float ambiguity, exactly as a "
                                    "coverage-gated key does)",
                              network_config_.eq34_prior_realization_max_gap_s,
                              network_config_.eq34_prior_realization_max_gap_s > 0.0
                                  ? "" : " (DISABLED — always block on cold hosts)");
                if (network_config_.phase_datum_time_linked) {
                    if (!(network_config_.phase_datum_bridge_sigma_m > 0.0)) {
                        log->error("Network (eq34 hygiene INC-2): "
                                   "network.phase_datum_bridge_sigma_m = {} is not > 0 "
                                   "— the bridge row would carry infinite information "
                                   "(a silent HARD datum takeover).  Disabling the "
                                   "bridge.",
                                   network_config_.phase_datum_bridge_sigma_m);
                        network_config_.phase_datum_time_linked = false;
                    } else if (network_config_.phase_datum_bridge_max_ep < 1) {
                        log->warn("Network (eq34 hygiene INC-2): "
                                  "network.phase_datum_bridge_max_ep = {} < 1 — a "
                                  "bridge could never be folded.  Disabling the bridge.",
                                  network_config_.phase_datum_bridge_max_ep);
                        network_config_.phase_datum_time_linked = false;
                    } else {
                        log->info("Network (eq34 hygiene INC-2): TIME-LINKED phase datum "
                                  "armed — a blocked/handed-over key holds its last "
                                  "solved shared phase bias through a soft pseudo-obs "
                                  "row (sigma {:g} m, info {:.3e}) folded BEFORE the "
                                  "rank gate, for at most {} epoch(s).  Standing "
                                  "invariant #1 event response is wired: the bridge "
                                  "DROPS on a slip-funnel hit of the bridged key, on "
                                  "the epoch after a clock-datum change, on staleness, "
                                  "and when the key leaves the shared set.",
                                  network_config_.phase_datum_bridge_sigma_m,
                                  1.0 / (network_config_.phase_datum_bridge_sigma_m
                                         * network_config_.phase_datum_bridge_sigma_m),
                                  network_config_.phase_datum_bridge_max_ep);
                        if (network_config_.eq34_anchor_min_arc_ep <= 0)
                            log->warn("Network (eq34 hygiene INC-2): armed WITHOUT INC-1 "
                                      "(eq34_anchor_min_arc_ep = 0) — every handover is "
                                      "bridged rather than re-hosted, which is a much "
                                      "broader change than the eligibility gate.  This "
                                      "is the spec's documented 'armed without INC-1' "
                                      "mode; arm INC-1 too unless that breadth is "
                                      "intended.");
                    }
                }
                if (network_config_.eq34_fresh_pin_sigma_scale > 1.0
                    && network_config_.eq34_fresh_pin_ramp_ep > 0)
                    log->info("Network (eq34 hygiene INC-3): fresh-pin deweight ramp "
                              "armed — a datum row pinned on a host younger than {} + {} "
                              "epochs is folded with its sigma scaled by up to {:g}x, "
                              "decaying linearly over the ramp.",
                              network_config_.eq34_anchor_min_arc_ep,
                              network_config_.eq34_fresh_pin_ramp_ep,
                              network_config_.eq34_fresh_pin_sigma_scale);
                // Standing invariant #1 disclosure: the arc-age map's SLIP response
                // reads the Card-4 slip funnel (carry_invariant-gated) unioned with
                // the phase-leveling pending-reseed flags (carry_warm-gated).  With
                // NEITHER live the age only resets on arc ABSENCE, so a repaired
                // in-place slip would leave a "mature" age on a just-refloated
                // ambiguity — precisely the code-seeded host this card exists to
                // refuse.  Say so rather than let it look guarded.
                if (!network_carry_invariant_ && !network_phase_leveling_)
                    log->warn("Network (eq34 hygiene): neither network.carry_invariant "
                              "nor network.phase_leveling is on, so NEITHER slip feed "
                              "into the arc-age map exists — an arc age then resets only "
                              "on ABSENCE.  A slip repaired in place keeps a mature age "
                              "and can still host a reattach (standing invariant #1 "
                              "degradation; arm one of the two feeds for full coverage).");
            }
        }
    }

    // =================================================================
    //  network subsection helper: loadNetworkDeweightReleaseHygiene_
    // =================================================================
    /**
     * @brief Iono-deweight RELEASE hygiene INC-1/2/3
     *        (docs/SPEC_deweight_release_hygiene.md).
     *
     * Ordered here because every guard below needs the iono-deweight gate config and
     * the SH-field / deviation-channel settings, all parsed above; and because the
     * INC-1 refloat rides the same two funnels the reattach-hygiene block just
     * reported on (@c network_carry_invariant_ / @c network_phase_leveling_).
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkDeweightReleaseHygiene_(
        const nlohmann::json& json, const std::shared_ptr<spdlog::logger>& log) {
        // --- IONO-DEWEIGHT RELEASE HYGIENE (docs/SPEC_deweight_release_hygiene.md) --
        // All three increments are default-OFF and, OFF, allocate no state, write no
        // state and remove no row.  Deliberately NOT appended to the warm-resume
        // fingerprint (see the NetworkConfig doxygen and networkSnapshotFingerprint):
        // forward-only release-time state/row policy, so an OFF and an ON leg may share
        // one snapshot — which is what makes the §5 shared-snapshot event A/B legal.
        network_config_.iono_dw_release_reseed =
            jval<bool>(json, "network.iono_dw_release_reseed",
                       "network_iono_dw_release_reseed", false);
        network_config_.iono_dw_release_floor_eps =
            jval<double>(json, "network.iono_dw_release_floor_eps",
                         "network_iono_dw_release_floor_eps", 1e-9);
        network_config_.iono_dw_park_bound_tecu =
            jval<double>(json, "network.iono_dw_park_bound_tecu",
                         "network_iono_dw_park_bound_tecu", 0.0);
        network_config_.iono_dw_release_validate_k =
            jval<double>(json, "network.iono_dw_release_validate_k",
                         "network_iono_dw_release_validate_k", 0.0);
        network_config_.iono_dw_admit_validate_k =
            jval<double>(json, "network.iono_dw_admit_validate_k",
                         "network_iono_dw_admit_validate_k", 0.0);

        // --- Clamps: every bad value falls back to the OFF/legacy behaviour, loudly.
        if (!(network_config_.iono_dw_release_floor_eps > 0.0)) {
            log->warn("Network (dw release hygiene): "
                      "network.iono_dw_release_floor_eps = {} is not > 0 — a release "
                      "edge is 'weight rose ABOVE eps', which at eps <= 0 can never be "
                      "detected from a floor of 0.  Restoring the default 1e-9 (the same "
                      "constant the gate's n_zero aggregate uses).",
                      network_config_.iono_dw_release_floor_eps);
            network_config_.iono_dw_release_floor_eps = 1e-9;
        }
        if (network_config_.iono_dw_park_bound_tecu < 0.0)
            network_config_.iono_dw_park_bound_tecu = 0.0;
        if (network_config_.iono_dw_release_validate_k < 0.0)
            network_config_.iono_dw_release_validate_k = 0.0;
        if (network_config_.iono_dw_admit_validate_k < 0.0)
            network_config_.iono_dw_admit_validate_k = 0.0;

        const bool dwr_any = network_config_.iono_dw_release_reseed
                          || network_config_.iono_dw_park_bound_tecu > 0.0
                          || network_config_.iono_dw_release_validate_k > 0.0
                          || network_config_.iono_dw_admit_validate_k > 0.0;
        if (!dwr_any) return;

        // --- INERT guards.  Each one disarms the WHOLE card, because every increment
        // is defined against the SH<->slant tie rows: with no field there is nothing to
        // reseed from, nothing to decouple, and no row to hold.
        if (!estimate_network_iono_sh_) {
            log->warn("Network (dw release hygiene): configured, but "
                      "network.estimate_iono_sh is OFF — there is no SH field to reseed "
                      "from and no SH<->slant tie row to guard, so every increment is "
                      "INERT (disabled).");
            network_config_.iono_dw_release_reseed = false;
            network_config_.iono_dw_park_bound_tecu = 0.0;
            network_config_.iono_dw_release_validate_k = 0.0;
            network_config_.iono_dw_admit_validate_k = 0.0;
            return;
        }
        // Every screen and bound in this card standardizes the SOLVED per-LOS deviation
        // that networkShDevVtec harvests into the T5 stash.  Without that channel there
        // is no statistic at all — and screening the linearization point instead would
        // be strictly wrong, not merely degraded (it is provably invariant in the
        // reduced solve).  So the two screens and the bound disarm together.
        if (!(global_vtec_sh_.rep_error_tau_s > 0.0)
            && (network_config_.iono_dw_release_validate_k > 0.0
                || network_config_.iono_dw_admit_validate_k > 0.0
                || network_config_.iono_dw_park_bound_tecu > 0.0)) {
            log->warn("Network (dw release hygiene): the T5 deviation channel is OFF "
                      "(global_vtec_sh.rep_error_tau_s <= 0), so there is no harvested "
                      "per-LOS deviation to standardize — INC-2's bound and the INC-3 / "
                      "INC-3b screens have no statistic and are INERT (disabled).  The "
                      "linearization point is NOT a substitute: shifting it leaves the "
                      "reduced shared RHS algebraically unchanged, so a screen on it "
                      "would be measuring an arbitrary frame.");
            network_config_.iono_dw_park_bound_tecu = 0.0;
            network_config_.iono_dw_release_validate_k = 0.0;
            network_config_.iono_dw_admit_validate_k = 0.0;
            if (!network_config_.iono_dw_release_reseed) return;
        }
        // INC-1 alone additionally needs the gate that produces the release edge.  The
        // static (v1) list cannot release — it holds a fixed weight for the whole run —
        // so the edge only exists on the v2 adaptive path.  INC-2 and INC-3 remain
        // meaningful without it (INC-2 still covers the gate-cold / warm-resume case;
        // INC-3 is a pure row screen), so this disarms INC-1 ONLY.
        if (network_config_.iono_dw_release_reseed
            && !iono_constraint_deweight_.adaptive) {
            log->warn("Network (dw release hygiene INC-1): "
                      "network.iono_dw_release_reseed is armed but "
                      "network.iono_constraint_deweight_adaptive is OFF — a statically "
                      "listed station holds ONE weight for the whole run and therefore "
                      "never produces a release edge, so the reseed is INERT (disabled). "
                      " INC-2 / INC-3 are unaffected.");
            network_config_.iono_dw_release_reseed = false;
        }

        // --- Arming log (one line per live increment, each naming what it will do).
        if (network_config_.iono_dw_release_reseed) {
            log->info("Network (dw release hygiene INC-1): release-time state hygiene "
                      "ARMED (eps {:g}) — when a station's deweight rises above eps, its "
                      "per-LOS slant-iono states are reseeded from the CURRENT SH field, "
                      "its float ambiguities are refloated through the slip funnel (one "
                      "epoch deferred, into BOTH the funnel and its RAW subset so the "
                      "v3k d5_keep policy cannot resurrect the parked deviation), and its "
                      "deviation-stash entries are erased.  The re-engaged tie rows then "
                      "fold information instead of whatever the zero-weight era parked.",
                      network_config_.iono_dw_release_floor_eps);
            // Standing invariant #1 disclosure, exactly as the eq34 card reports it: the
            // refloat is only as live as the funnel it feeds.
            if (!network_carry_invariant_ && !network_phase_leveling_)
                log->warn("Network (dw release hygiene INC-1): neither "
                          "network.carry_invariant nor network.phase_leveling is on, so "
                          "the queued ambiguity refloat reaches NO carry consumer — the "
                          "slant reseed and the stash erase still land, but the phase "
                          "side re-seeds only through the per-epoch MeasurementModel "
                          "path.  Check the run-end refloat counter before crediting the "
                          "ambiguity half of INC-1.");
            if (!(global_vtec_sh_.rep_error_tau_s > 0.0))
                log->info("Network (dw release hygiene INC-1): the T5 deviation channel "
                          "is off (global_vtec_sh.rep_error_tau_s <= 0), so the stash "
                          "erase has nothing to erase — the slant reseed and the refloat "
                          "are the whole response this run.");
        }
        if (network_config_.iono_dw_park_bound_tecu > 0.0)
            log->info("Network (dw release hygiene INC-2): while-decoupled park bound "
                      "ARMED at {:g} slant TECU ({:.1f} m of L1 delay) — a slant-iono "
                      "state beyond it is reset to the field evaluation immediately, "
                      "while the station's weight is <= eps AND (warm-resume coverage) "
                      "while the gate holds no samples for it at all.  No gate state is "
                      "serialized, so a resumed leg starts every station at w = 1 and "
                      "re-folds every tie row on its FIRST assembly with no release edge "
                      "to hook; the gate-cold trigger is what bounds that shock.",
                      network_config_.iono_dw_park_bound_tecu,
                      network_config_.iono_dw_park_bound_tecu * 0.1623727);
        if (network_config_.iono_dw_release_validate_k > 0.0)
            log->info("Network (dw release hygiene INC-3): tie-row RELEASE screen ARMED "
                      "at k = {:g} — an already-admitted LOS whose standardized deviation "
                      "|dev| / {:g} TECU exceeds k keeps its SH<->slant tie row WITHHELD "
                      "(it stays decoupled) and is re-tested every epoch until it passes.",
                      network_config_.iono_dw_release_validate_k,
                      global_vtec_sh_.representation_sigma_tecu);
        if (network_config_.iono_dw_admit_validate_k > 0.0)
            log->info("Network (dw release hygiene INC-3b): tie-row ADMISSION screen "
                      "ARMED at k = {:g} — a LOS entering a station's tie stack for the "
                      "FIRST time is held for one epoch (its deviation is still "
                      "harvested), then admitted only if that deviation passes k.  This "
                      "is the valve MEASURED at DOY-129 TOW 329160: the station carrying "
                      "the parked column was at w = 1.0 throughout and never released — "
                      "a satellite crossed the {:g} deg elevation cutoff and coupled an "
                      "unvalidated slant column to the field.  Expect one quarantine "
                      "LOS-epoch per rising satellite per station; only the REJECT "
                      "counter is the guarded event.",
                      network_config_.iono_dw_admit_validate_k,
                      iono_weighting_.elevation_cutoff_deg);
    }

    // =================================================================
    //  network subsection helper: loadNetworkSignalIntegrity_
    // =================================================================
    /**
     * @brief Flicker / gap signal integrity (@c network.signal_integrity.*).
     *
     * Ordered after the cycle-slip block and the carry parse: the INERT guards read
     * @c network_carry_invariant_ / @c network_phase_leveling_, the two refloat funnels
     * INC-3's regain response rides.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkSignalIntegrity_(const nlohmann::json& json,
                                                 const std::shared_ptr<spdlog::logger>& log) {
        // --- FLICKER / GAP SIGNAL INTEGRITY (docs/SPEC_flicker_gap_integrity.md) --
        // Placed here because the INERT guards below need network_carry_invariant_ /
        // network_phase_leveling_ (the two slip-funnel feeds INC-3's refloat rides)
        // and the cycle-slip block (parsed far above) to already be resolved.
        // Default-OFF; OFF allocates no map, changes no row weight and touches no
        // observable.  Deliberately NOT appended to the warm-resume fingerprint
        // (forward-only weight/refloat policy — the adaptive_iono_q / eq34-hygiene
        // precedent), which is what keeps a shared-snapshot A/B legal.
        {
            auto& si = network_config_.signal_integrity;
            si.census = jval<bool>(json, "network.signal_integrity.census",
                                   "network_signal_integrity_census", false);
            si.unvetted_sigma_scale =
                jval<double>(json, "network.signal_integrity.unvetted_sigma_scale",
                             "network_signal_integrity_unvetted_sigma_scale", 10.0);
            si.revet_ep = jval<int>(json, "network.signal_integrity.revet_ep",
                                    "network_signal_integrity_revet_ep", 3);
            si.flicker_max_ep = jval<int>(json, "network.signal_integrity.flicker_max_ep",
                                          "network_signal_integrity_flicker_max_ep", 5);
            si.refloat_on_regain =
                jval<bool>(json, "network.signal_integrity.refloat_on_regain",
                           "network_signal_integrity_refloat_on_regain", true);
            si.withhold_instead =
                jval<bool>(json, "network.signal_integrity.withhold_instead",
                           "network_signal_integrity_withhold_instead", false);
            si.resume_warmup_vet =
                jval<bool>(json, "network.signal_integrity.resume_warmup_vet",
                           "network_signal_integrity_resume_warmup_vet", true);

            // Range hygiene BEFORE the arming report, so the report states what the
            // run will actually do rather than what was typed.
            if (si.revet_ep < 0) si.revet_ep = 0;
            if (si.flicker_max_ep < 0) si.flicker_max_ep = 0;
            // Positive-definiteness headroom.  The demotion divides a phase row's
            // information by scale²; a phase pivot ~1/σ_φ² ≈ 1e5 sits against a
            // per-station relative bar of 1e-12·d_ref, and at an anchor station
            // d_ref can be the 1e10 hard datum pin — so the bar is ~1e-2 and a scale
            // beyond ~1e4 walks the row into it.  A row that trips the PD guard drops
            // the WHOLE STATION for the epoch, which is the hard exclusion the
            // fallback doctrine forbids; clamp rather than let a typo cause it.
            constexpr double kMaxUnvettedScale = 1.0e4;
            if (si.unvetted_sigma_scale > kMaxUnvettedScale) {
                log->warn("Network (signal integrity INC-2): "
                          "unvetted_sigma_scale = {:g} exceeds {:g}; a row deweighted "
                          "that far approaches the per-station positive-definiteness "
                          "bar at an anchor station, and a row that trips it drops the "
                          "WHOLE station for the epoch (a hard exclusion, not a "
                          "fallback).  Clamping.  Use withhold_instead if the intent "
                          "is genuinely to remove the phase.",
                          si.unvetted_sigma_scale, kMaxUnvettedScale);
                si.unvetted_sigma_scale = kMaxUnvettedScale;
            }

            // The demotion has three resolved dispositions, and the arming line must
            // name the one in force: WITHHOLD (rows removed), DEWEIGHT (σ scaled), or
            // NONE (the spec §6C census-only leg).  A scale in (0, 1] is NOT an error —
            // it is exactly how a census leg is expressed without a second knob — but
            // it IS reported, because a user who typed 0.5 expecting a tightening got
            // silence otherwise.
            const bool withhold = si.withhold_instead || si.unvetted_sigma_scale <= 0.0;
            const bool deweight = !withhold && si.unvetted_sigma_scale > 1.0;
            const bool acts     = withhold || deweight || si.refloat_on_regain;

            // INC-2/3 REQUIRE the census: you cannot act on a band state you are not
            // tracking, and an action wired behind a classifier that never ran is the
            // "clean log over dead wiring" failure standing invariant #4 exists to
            // prevent.  Warn + disable rather than silently promote the master.
            if (!si.census && acts) {
                log->warn("Network (signal integrity): action knobs are set "
                          "(unvetted_sigma_scale = {:g}, withhold_instead = {}, "
                          "refloat_on_regain = {}) but "
                          "network.signal_integrity.census is OFF — the band-state "
                          "classifier never runs, so there is no state to act on and "
                          "every action is INERT (disabled).  Arm .census to use them.",
                          si.unvetted_sigma_scale, si.withhold_instead,
                          si.refloat_on_regain);
                si.unvetted_sigma_scale = 1.0;
                si.withhold_instead     = false;
                si.refloat_on_regain    = false;
                si.resume_warmup_vet    = false;
            } else if (si.census) {
                log->info("Network (signal integrity INC-0/1): band-state classifier ON "
                          "— every present (station, satellite) line of sight is "
                          "classified each epoch as STABLE / NEW / REENTRY / "
                          "FLICKER-REGAIN / DEGRADED, and the behaviour census "
                          "(re-entries by outage length, band losses/regains, "
                          "single-frequency continuations of dual-born arcs, standing "
                          "single-frequency arcs) is counted.  A band absent for <= {} "
                          "epoch(s) while the LOS stayed present is a FLICKER; beyond "
                          "that the reference band set is REDUCED and the arc re-vets "
                          "on it, so a genuinely single-frequency arc is not demoted "
                          "forever.",
                          si.flicker_max_ep);
                if (withhold)
                    log->info("Network (signal integrity INC-2): unvetted phase "
                              "WITHHELD — while a LOS is not STABLE its phase "
                              "observables are removed for the epoch and its CODE rows "
                              "are kept untouched (they are slip-immune and they are "
                              "what re-anchors the float).  This is the non-default "
                              "disposition: removing a phase row can also remove the "
                              "only support of an elected datum pin, leaving the W5 "
                              "rank gate as the backstop — grade the gate on this arm.");
                else if (deweight)
                    log->info("Network (signal integrity INC-2): unvetted phase "
                              "DEWEIGHTED — while a LOS is not STABLE its PHASE rows "
                              "fold at sigma x {:g} (weight /= {:g}); CODE rows are "
                              "untouched.  Prefer-fallback, never exclusion: an "
                              "unmonitorable phase never carries full weight, but it is "
                              "never dropped either.",
                              si.unvetted_sigma_scale,
                              si.unvetted_sigma_scale * si.unvetted_sigma_scale);
                else
                    log->info("Network (signal integrity INC-2): DISARMED "
                              "(unvetted_sigma_scale = {:g} is not > 1 and "
                              "withhold_instead is off) — this is the CENSUS-ONLY leg: "
                              "the classifier runs and counts, and not one row weight "
                              "changes.",
                              si.unvetted_sigma_scale);
                // Cold start vs warm resume (spec ADDENDUM, Aug-1).  These are NOT the
                // same situation and the card treats them oppositely, so say which is
                // which rather than let a reader assume symmetry.
                log->info("Network (signal integrity): an arc that has NEVER completed "
                          "a re-vet in this run is EXEMPT from the demotion — a phase "
                          "row's damage potential is its weight times the TIGHTNESS of "
                          "the state it updates, and a never-converged arc has no warm "
                          "carry for a slip to poison (its own priors are the "
                          "demotion).  So a cold start is not blanket-vetted; the vet "
                          "begins once an arc has something to protect.");
                if (si.resume_warmup_vet)
                    log->info("Network (signal integrity RESUME WARMUP): a warm resume "
                              "vets EVERY arc for {} epoch(s).  The snapshot restores "
                              "TIGHT carries but NO detector state (no MW Welford count "
                              "and no IF-TD arc history is serialized), so the first "
                              "epochs of a resumed run carry fully-warm state with zero "
                              "time-difference coverage — the flicker hazard at network "
                              "scale.  Grading note: this perturbs the first {} epoch(s) "
                              "of a replay leg relative to the continuous run, so a "
                              "byte-grade resume A/B must arm or disarm it on BOTH legs.",
                              si.revet_ep, si.revet_ep);
                else
                    log->warn("Network (signal integrity RESUME WARMUP): DISABLED — a "
                              "warm resume will run its first epochs with fully-warm "
                              "carries and no detector history at all.  That is the one "
                              "situation this card exists for, at network scale; "
                              "disable only for a branch-faithful byte comparison.");
                if (si.refloat_on_regain)
                    log->info("Network (signal integrity INC-3): regain REFLOAT ON — a "
                              "band that returns without a producer-registered gap "
                              "refloats the line of sight's ambiguities on ALL bands "
                              "through the existing slip funnel (reset-not-repair: "
                              "across a loss of lock the slip magnitude is unbounded "
                              "and unknowable, and a proportional counter reset is "
                              "invisible to BOTH MW and GF).  A re-entry that already "
                              "refloated is left alone and counted.");
                else
                    log->info("Network (signal integrity INC-3): regain refloat OFF — "
                              "band regains are classified and counted but nothing is "
                              "reset.");
                if (si.revet_ep <= 0)
                    log->warn("Network (signal integrity INC-3b): revet_ep = {} — a LOS "
                              "is STABLE the instant it is full-band again, so the "
                              "post-transition window in which NO time-difference "
                              "detector can operate (MW needs 2 samples, IF-TD needs 3 "
                              "epochs) carries FULL phase weight.  That is the exact "
                              "hazard this card exists to close; 3 is the spec default.",
                              si.revet_ep);
                else
                    log->info("Network (signal integrity INC-3b): re-vet window {} "
                              "consecutive full-band epoch(s) after ANY transition.  At "
                              "3 the vet ends exactly when monitoring begins — MW (2 "
                              "time-difference samples) and IF-TD (3 epochs) are both "
                              "live for at least one epoch before the phase returns to "
                              "full weight.",
                              si.revet_ep);
                // Standing invariant #1 disclosure, mirroring the eq34 hygiene warn:
                // INC-3's refloat rides the SAME two funnels the slip detectors use.
                // With neither live there is nothing to insert into, so the regain
                // response degrades to a counter.  Say so rather than let an armed
                // knob look guarded.
                if (si.refloat_on_regain && !network_carry_invariant_
                    && !network_phase_leveling_)
                    log->warn("Network (signal integrity INC-3): refloat_on_regain is on "
                              "but neither network.carry_invariant nor "
                              "network.phase_leveling is — NEITHER refloat funnel "
                              "exists, so a regain is COUNTED and nothing is reset.  "
                              "The classifier and the INC-2 demotion still work; arm "
                              "one of the two feeds for the reset response.");
            }
        }
    }

    // =================================================================
    //  network subsection helper: loadNetworkEmissionFilters_
    // =================================================================
    /**
     * @brief Snapshot-carry validation, the PB emission-continuity filter, and the estimability knobs.
     *
     * RTB-7(c/d/e) carry-blob requirements (now that @c network.carry_invariant is known),
     * @c network.emit_pb_filter*, the phase-datum sub-knobs, the Card-KP OSB prior, the
     * Card-DG rank-gate tolerance and the pin-aware station PD guard.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     * @return @c false on a fatal misconfiguration (aborts @c loadFromJson).
     */
    bool GnssConfig::loadNetworkEmissionFilters_(const nlohmann::json& json,
                                                 const std::shared_ptr<spdlog::logger>& log) {
        // --- H-rider (edge-queue §1.6 item 3a): census-fed H σ̂₀ guard ----------
        // Parsed in loadNetworkClockModelAndChannels_ (next to the heartbeat it
        // feeds); validated HERE because the empty-mask case is a hard load error
        // and that helper is void.  Both prerequisites are long parsed by now.
        if (network_h_sigma0_prefit_census_) {
            if (!network_emit_epoch_status_record_) {
                log->warn("Network (H-rider): network.h_sigma0_prefit_census is ARMED but "
                          "emit_epoch_status_record is OFF — there are no H records to "
                          "feed, so this knob is INERT (standing invariant #4).  Arm "
                          "network.emit_epoch_status_record, or drop the census feed.");
                network_h_sigma0_prefit_census_ = false;
            } else if (!network_config_.trace_prefit_census) {
                log->warn("Network (H-rider): network.h_sigma0_prefit_census is ARMED but "
                          "trace_prefit_census is OFF — the born-cell deferral reads the "
                          "census walk's lag-1 presence map, so the statistic would count "
                          "healthy births as exceedances.  Forced OFF; arm "
                          "network.trace_prefit_census (fingerprint-SAFE, standard armed "
                          "set) with it.");
                network_h_sigma0_prefit_census_ = false;
            } else {
                std::vector<std::string> unknown;
                const unsigned mask = parseDataClassMask(network_h_sigma0_classes_, &unknown);
                for (const auto& tok : unknown)
                    log->warn("Network (H-rider): unrecognized token '{}' in "
                              "network.h_sigma0_classes ('{}') — ignored.",
                              tok, network_h_sigma0_classes_);
                if (mask == 0) {
                    log->error("Network: network.h_sigma0_classes ('{}') resolves to an "
                               "EMPTY class set — the census-fed H σ̂₀ would be a "
                               "permanent 0.0000 masquerading as a measurement.  Fix the "
                               "class list or drop network.h_sigma0_prefit_census.",
                               network_h_sigma0_classes_);
                    return false;
                }
                log->info("Network (H-rider): H heartbeat σ̂₀ fed from the PREFIT CENSUS "
                          "pool (classes '{}') — pre-solve, station-common-mode-removed, "
                          "1.4826·median, available on EVERY H site including the gate "
                          "withhold.  A DIFFERENT statistic from the within-epoch RAIM's "
                          "sigma0_adapt (which stays the A4 ceiling input, unchanged); "
                          "the S contract advertises hsig=pc so consumers can tell the "
                          "populations apart.  Product bytes change when armed (H field "
                          "5 + the hsig token).",
                          network_h_sigma0_classes_);
            }
        }
        // RTB-7(c): the branch-faithful carry blob requires the invariant carry to be
        // armed and a snapshot path to write to (parsed above); enforce + log here now
        // that network_carry_invariant_ is known.
        if (network_config_.state_snapshot_carry) {
            if (network_config_.state_snapshot_path.empty() || !network_carry_invariant_) {
                log->warn("Network (RTB-7c): network.state_snapshot_carry requires "
                          "state_snapshot_path + carry_invariant; disabling (the carry blob "
                          "would be inert).");
                network_config_.state_snapshot_carry = false;
                network_config_.state_snapshot_carry_diagonal = false;  // RTB-7(e): no blob to shape
                network_config_.state_snapshot_carry_lin = false;       // RS-2: no blob to base
            } else {
                log->info("Network (RTB-7c): BRANCH-FAITHFUL resume ON — the invariant DD "
                          "carry (z-store means + {} Pᶻ + vertex/group numbering) is "
                          "serialized into the snapshot, so a warm resume reproduces the "
                          "phase-ambiguity branch exactly instead of re-converging onto a "
                          "different one.  Snapshot grows with the carried DD {}.",
                          network_config_.state_snapshot_carry_diagonal ? "DIAGONAL"
                                                                        : "dense",
                          network_config_.state_snapshot_carry_diagonal
                              ? std::string("count (LINEARLY — blob v3)")
                              : std::string("count"));
                if (network_config_.state_snapshot_carry_diagonal)
                    log->info("Network (RTB-7e): carry blob DIAGONAL-ONLY (v3) — only each "
                              "component's Pᶻ diagonal is written (Σ K_c numbers instead of "
                              "Σ K_c², ~1663x smaller at 305 stations).  The restored means "
                              "are exact; chord CROSS-covariances start at zero and are "
                              "re-earned from data.  This is a measured trade (a diagonal "
                              "prior tightens correlated chords' common mode) — grade it on "
                              "datum-invariant quantities before trusting it.  Reading is "
                              "unaffected: v1/v2/v3 blobs all restore under either setting.");
                if (network_config_.state_snapshot_carry_lin)
                    log->info("Network (RS-2): carry blob carries the LINEARIZATION DD — one "
                              "ZL row per block beside Z, zl = Σ c·x_lin over the same "
                              "incidence that forms z_free.  The stored ẑ is the "
                              "CORRECTION-frame DD (λN − Σ c·x_lin) and is meaningless "
                              "against a different linearization; a resumed run re-seeds "
                              "every station ambiguity x_lin from code, so without this row "
                              "the restored branch folds wrong by Σ c·Δx_lin on every chord "
                              "at once.  Reader half: network.resume_carry_rebase.  "
                              "Format-forward — a reader that finds no ZL row behaves exactly "
                              "as before, so this is NOT in the warm-resume fingerprint; only "
                              "the snapshot bytes change.");
                if (network_config_.state_snapshot_resume_shock_m > 0.0)
                    log->info("Network (RTB-7d/RE-2): resume EVENT RESPONSE armed — σ = "
                              "{:.3f} m added to every restored DD's Pᶻ diagonal at resume "
                              "(variance {:.4g} m²).  The branch (means + cross-covariance) "
                              "is kept; the widened own-variance lets the fold absorb the "
                              "un-restored station layer's re-convergence instead of the D5 "
                              "gate discarding the branch — gate stays armed (invariant #1).",
                              network_config_.state_snapshot_resume_shock_m,
                              network_config_.state_snapshot_resume_shock_m
                                  * network_config_.state_snapshot_resume_shock_m);
            }
        } else {
            if (network_config_.state_snapshot_resume_shock_m > 0.0)
                log->warn("Network (RTB-7d/RE-2): state_snapshot_resume_shock_m {:.3f} is "
                          "INERT without state_snapshot_carry (nothing is restored to "
                          "shock).", network_config_.state_snapshot_resume_shock_m);
            if (network_config_.state_snapshot_carry_diagonal) {
                log->warn("Network (RTB-7e): state_snapshot_carry_diagonal is INERT without "
                          "state_snapshot_carry (no carry blob is written to shape).");
                network_config_.state_snapshot_carry_diagonal = false;
            }
            if (network_config_.state_snapshot_carry_lin) {
                log->warn("Network (RS-2): state_snapshot_carry_lin is INERT without "
                          "state_snapshot_carry (no carry blob is written to carry the ZL "
                          "rows).");
                network_config_.state_snapshot_carry_lin = false;
            }
        }
        // PB emission-continuity filter (M5 prerequisite) — outputs-only smoother
        // on the re-based emitted phase biases + a self-tuning k·σ̂_step jump gate.
        network_emit_pb_filter_ =
            jval<bool>(json, "network.emit_pb_filter", "network_emit_pb_filter", false);
        network_emit_pb_filter_q_ =
            jval<double>(json, "network.emit_pb_filter_q", "network_emit_pb_filter_q", 3E-07);
        network_emit_pb_filter_jump_k_ =
            jval<double>(json, "network.emit_pb_filter_jump_k", "network_emit_pb_filter_jump_k", 4.0);
        network_emit_pb_filter_meas_sigma_m_ =
            jval<double>(json, "network.emit_pb_filter_meas_sigma_m",
                         "network_emit_pb_filter_meas_sigma_m", 0.05);
        network_emit_pb_filter_max_gap_s_ =
            jval<double>(json, "network.emit_pb_filter_max_gap_s",
                         "network_emit_pb_filter_max_gap_s", 600.0);
        network_emit_pb_filter_window_epochs_ =
            jval<double>(json, "network.emit_pb_filter_window_epochs",
                         "network_emit_pb_filter_window_epochs", 60.0);
        network_emit_pb_filter_emit_sigma_ =
            jval<bool>(json, "network.emit_pb_filter_emit_sigma",
                       "network_emit_pb_filter_emit_sigma", false);
        network_emit_pb_filter_gate_v2_ =
            jval<bool>(json, "network.emit_pb_filter_gate_v2",
                       "network_emit_pb_filter_gate_v2", false);
        // Card CLK-R (docs/SPEC_clock_emit_rebase.md): clock emission re-base —
        // stage 1 holds the per-constellation common mode at emission (the
        // 613/1440-census-epoch re-realization class), stage 2 marks the
        // surviving per-sat step class via the R-record retro-bump.  Both
        // outputs-only, independent, default OFF.
        network_emit_clock_frame_hold_ =
            jval<bool>(json, "network.emit_clock_frame_hold",
                       "network_emit_clock_frame_hold", false);
        network_clock_frame_leash_m_ =
            jval<double>(json, "network.clock_frame_leash_m",
                         "network_clock_frame_leash_m", 50.0);
        network_clock_disc_jump_test_ =
            jval<bool>(json, "network.clock_disc_jump_test",
                       "network_clock_disc_jump_test", false);
        network_clock_disc_jump_m_ =
            jval<double>(json, "network.clock_disc_jump_m",
                         "network_clock_disc_jump_m", 1.5);
        network_clock_disc_gap_jump_m_ =
            jval<double>(json, "network.clock_disc_gap_jump_m",
                         "network_clock_disc_gap_jump_m", 0.0);
        if (network_clock_disc_gap_jump_m_ < 0.0) {
            log->warn("Network (Card CLK-R/E1): network.clock_disc_gap_jump_m {} < 0 — "
                      "clamped to 0 (OFF).", network_clock_disc_gap_jump_m_);
            network_clock_disc_gap_jump_m_ = 0.0;
        }
        if (network_clock_disc_gap_jump_m_ > 0.0 && !network_clock_disc_jump_test_)
            log->warn("Network (Card CLK-R/E1): network.clock_disc_gap_jump_m is set but "
                      "network.clock_disc_jump_test is OFF — stage 2b rides stage 2's engine "
                      "walk, so the gap test is INERT.  Arm clock_disc_jump_test with it.");
        if (network_clock_disc_gap_jump_m_ > 0.0 && network_clock_disc_jump_test_)
            log->info("Network (Card CLK-R/E1): GAP-AWARE clock step test ON — a satellite "
                      "returning after > the stage-2 horizon is tested against its RETAINED "
                      "pre-gap held-frame baseline at {:.2f} m (0 was the old behavior: an "
                      "untested first sighting — the measured eclipse-readmission blindness, "
                      "6.3-6.7 m re-realizations with zero CLK_STEP marks).  Marks route to "
                      "the same markSatDiscontinuity retro-bump chain.",
                      network_clock_disc_gap_jump_m_);
        if (network_emit_clock_frame_hold_) {
            log->info("Network producer (Card CLK-R): CLOCK EMISSION FRAME HOLD ON — the emitted "
                      "satellite clocks ride a per-constellation HELD frame (per-epoch median "
                      "common-mode innovation drained into a frame accumulator that never re-enters "
                      "the wire; between-sat differences preserved exactly).  Outputs-only: solve "
                      "byte-identical, fingerprint-SAFE.  Leash |A_c| > {:.0f} m re-anchors WITH a "
                      "datum mark (markDatumChange — invariant #7's marked re-coupling edge).",
                      network_clock_frame_leash_m_);
            if (network_clock_frame_leash_m_ > 0.0 && network_clock_frame_leash_m_ < 5.0)
                log->warn("Network (Card CLK-R): network.clock_frame_leash_m = {:.1f} is BELOW 5 m — "
                          "the measured 12 h frame wander is ~23 m, so a tight leash re-anchors (and "
                          "datum-marks) repeatedly, defeating the hold.  Raise it or accept the churn.",
                          network_clock_frame_leash_m_);
        }
        if (network_clock_disc_jump_test_) {
            log->info("Network producer (Card CLK-R): CLOCK DISC JUMP TEST ON — per-sat common-"
                      "removed emitted-clock steps > {:.2f} m route to the markSatDiscontinuity "
                      "retro-bump (R record + per-sat phase/code/clock counters; the consumer resets "
                      "that satellite at the event epoch).  The emitter's clock counter keeps NO "
                      "internal magnitude test (RTA-11) — this test lives at the emission-prep tier"
                      "{}.",
                      network_clock_disc_jump_m_,
                      network_emit_clock_frame_hold_
                          ? "; statistic taken in the HELD frame (frame hold armed)"
                          : "; statistic taken on the RAW wire (frame hold off)");
            if (network_clock_disc_jump_m_ < 0.5)
                log->warn("Network (Card CLK-R): network.clock_disc_jump_m = {:.2f} is BELOW the "
                          "0.5 m census outlier floor — the per-sat channel is a random walk "
                          "(robust step σ ≈ 0.1 m), so a sub-floor gate marks the walk itself "
                          "(~70+/h) and storms the consumer with resets.",
                          network_clock_disc_jump_m_);
        }
        // Card Q3 (docs/SPEC_clock_temporal_anchor.md): the GUARDED temporal clock
        // anchor.  Solve-affecting and fingerprinted — every knob is inert unless
        // network.clock_temporal_anchor, so an unarmed run is byte-identical.
        network_clock_temporal_anchor_ =
            jval<bool>(json, "network.clock_temporal_anchor",
                       "network_clock_temporal_anchor", false);
        network_clock_anchor_q_ =
            jval<double>(json, "network.clock_anchor_q",
                         "network_clock_anchor_q", 1e-4);
        network_clock_anchor_var_floor_m2_ =
            jval<double>(json, "network.clock_anchor_var_floor_m2",
                         "network_clock_anchor_var_floor_m2", 0.0);
        network_clock_anchor_max_gap_s_ =
            jval<double>(json, "network.clock_anchor_max_gap_s",
                         "network_clock_anchor_max_gap_s", 120.0);
        network_clock_anchor_k_ =
            jval<double>(json, "network.clock_anchor_k",
                         "network_clock_anchor_k", 6.0);
        network_clock_anchor_abs_m_ =
            jval<double>(json, "network.clock_anchor_abs_m",
                         "network_clock_anchor_abs_m", 2.0);
        network_clock_anchor_sigma_floor_m_ =
            jval<double>(json, "network.clock_anchor_sigma_floor_m",
                         "network_clock_anchor_sigma_floor_m", 0.02);
        network_clock_anchor_recouple_epochs_ =
            jval<int>(json, "network.clock_anchor_recouple_epochs",
                      "network_clock_anchor_recouple_epochs", 4);
        network_clock_anchor_mark_ =
            jval<bool>(json, "network.clock_anchor_mark",
                       "network_clock_anchor_mark", true);
        network_clock_anchor_resolve_ =
            jval<bool>(json, "network.clock_anchor_resolve",
                       "network_clock_anchor_resolve", true);
        network_clock_anchor_gauge_free_ =
            jval<bool>(json, "network.clock_anchor_gauge_free",
                       "network_clock_anchor_gauge_free", false);
        // Card Q3 §2.1 BISECTION knob.  Parsed as a NAME rather than a bool pair
        // because the four settings are not two independent flags: `auto` is a
        // reproduction contract (it must keep tracking whatever the fold does), and
        // `post` exists only to reproduce the shipped gauge-free arm exactly.  An
        // unrecognised name is an ERROR, never a silent fall-back to the default —
        // a mis-typed denominator would look like a clean run with the wrong
        // statistic, which is the failure mode this whole knob exists to expose.
        {
            const std::string s = jval<std::string>(json, "network.clock_anchor_gauge_free_stat",
                                                    "network_clock_anchor_gauge_free_stat",
                                                    std::string{"auto"});
            if (s.empty() || s == "auto")      network_clock_anchor_gauge_free_stat_ = NetworkClockAnchorStat::Auto;
            else if (s == "raw")               network_clock_anchor_gauge_free_stat_ = NetworkClockAnchorStat::Raw;
            else if (s == "post")              network_clock_anchor_gauge_free_stat_ = NetworkClockAnchorStat::Post;
            else if (s == "proj")              network_clock_anchor_gauge_free_stat_ = NetworkClockAnchorStat::Proj;
            else {
                network_clock_anchor_gauge_free_stat_ = NetworkClockAnchorStat::Auto;
                log->error("Network (Card Q3 §2.1): network.clock_anchor_gauge_free_stat = '{}' is not "
                           "one of auto|raw|post|proj — falling back to 'auto'.  Fix the config: a "
                           "mis-named denominator produces a clean-looking run standardized by the "
                           "wrong variance.", s);
            }
        }
        if (network_clock_temporal_anchor_) {
            // Realized walk once the carry binds is √(q·Δt) EXACTLY (steady-state
            // scalar identity), so the armed q is reported as the walk it targets —
            // the number the wire census will be graded against.
            const double sigma_w_30 = std::sqrt(std::max(0.0, network_clock_anchor_q_) * 30.0);
            log->info("Network producer (Card Q3): GUARDED TEMPORAL CLOCK ANCHOR ON — the satellite "
                      "clock carry folds at q = {:.3e} m²/s (targets a √(q·Δt) = {:.2f} cm walk at "
                      "30 s, vs the measured 9.23 cm and the ~2 cm physical) over a PER-SATELLITE "
                      "elapsed time capped at {:.0f} s, variance floor {:.3e} m². Guard: |w| > {:.2f} "
                      "on the common-removed predicted residual (√S floored at {:.3f} m) or |ê| > "
                      "{:.2f} m ⇒ REFLOAT{}{}; re-coupling needs {} consecutive in-band epochs. "
                      "SOLVE-AFFECTING + FINGERPRINTED: fresh convergence only.",
                      network_clock_anchor_q_, 100.0 * sigma_w_30,
                      network_clock_anchor_max_gap_s_, network_clock_anchor_var_floor_m2_,
                      network_clock_anchor_k_, network_clock_anchor_sigma_floor_m_,
                      network_clock_anchor_abs_m_,
                      network_clock_anchor_resolve_ ? " + in-epoch re-solve (adapt-and-EMIT)"
                                                    : " (NO in-epoch re-solve: one anchor-warped "
                                                      "epoch ships per event, marked)",
                      network_clock_anchor_mark_ ? " + wire mark" : " (UNMARKED — wire-invisible)",
                      network_clock_anchor_recouple_epochs_);
            log->info("Network producer (Card Q3 §2.1): clock prior folds {} — {}.",
                      network_clock_anchor_gauge_free_ ? "GAUGE-FREE" : "on the RAW state",
                      network_clock_anchor_gauge_free_
                          ? "the per-constellation clock common direction is marginalized out of "
                            "the folded information, so the anchor constrains only the estimable "
                            "between-satellite differences and the gauge is left to the datum "
                            "(measured motive: the raw-state anchor suppressed CLK-R's drained "
                            "common mode 49.4 -> 27.8 m over 3 h, i.e. the carry was doing 44 % of "
                            "the gauge's work).  EXPECT the drained common mode to RETURN toward "
                            "the control's level — that is the mechanism working, not a regression"
                          : "it therefore competes with the S-basis datum for the per-constellation "
                            "common mode; arm network.clock_anchor_gauge_free to stop it");
            // The guard's DENOMINATOR is a separate choice from the fold, and saying
            // which one is armed is the whole point of the bisection: two arms that
            // differ only here are otherwise indistinguishable in the log.
            {
                const bool proj_post = network_clock_anchor_gauge_free_stat_ != NetworkClockAnchorStat::Raw
                                    && (network_clock_anchor_gauge_free_stat_ != NetworkClockAnchorStat::Auto
                                        || network_clock_anchor_gauge_free_);
                const bool proj_prior = network_clock_anchor_gauge_free_stat_ == NetworkClockAnchorStat::Proj;
                const char* name =
                    network_clock_anchor_gauge_free_stat_ == NetworkClockAnchorStat::Raw  ? "raw"
                  : network_clock_anchor_gauge_free_stat_ == NetworkClockAnchorStat::Post ? "post"
                  : network_clock_anchor_gauge_free_stat_ == NetworkClockAnchorStat::Proj ? "proj"
                  : (network_clock_anchor_gauge_free_ ? "auto->post" : "auto->raw");
                log->info("Network producer (Card Q3 §2.1): guard/census DENOMINATOR = '{}' — P⁻ from the "
                          "{} prior variance, P⁺ from the {} posterior variance.  The statistic is on the "
                          "COMMON-REMOVED residual, so both halves have to be the variances of that "
                          "CONTRAST: 'proj' is the only setting that projects both, and it is the "
                          "consistent one for EITHER fold.  This knob is SOLVE-AFFECTING through the trip "
                          "population and is fingerprinted whenever it is not 'auto'.",
                          name,
                          proj_prior ? "GAUGE-PROJECTED" : "raw per-satellite",
                          proj_post  ? "GAUGE-PROJECTED" : "raw marginal");
            }
            // The interlocks that decide whether the mechanism can work at all.
            if (!(network_clock_anchor_q_ > 0.0))
                log->error("Network (Card Q3): network.clock_anchor_q = {:.3e} is not positive — the "
                           "anchor cannot fold.  Set a positive rate or disarm the card.",
                           network_clock_anchor_q_);
            if (network_clock_anchor_q_ >= process_noise_.sat_clock)
                log->warn("Network (Card Q3): network.clock_anchor_q ({:.3e}) is NOT tighter than "
                          "filter.process_noise.sat_clock_q ({:.3e}) — the anchor is then looser than "
                          "the carry it replaces and the card is inert by construction (its whole "
                          "premise is that √(q·Δt) must fall BELOW the realized per-epoch spread).",
                          network_clock_anchor_q_, process_noise_.sat_clock);
            if (network_clock_anchor_max_gap_s_ <= 0.0)
                log->warn("Network (Card Q3): network.clock_anchor_max_gap_s = {:.1f} disables the "
                          "staleness guard — a binding prior would then fold an arbitrarily old "
                          "carried clock as if it were one epoch old (the DOY-129 278-epoch withhold "
                          "class).  This is a standing-invariant-#1 violation; raise it.",
                          network_clock_anchor_max_gap_s_);
            if (network_clock_anchor_var_floor_m2_ <= 0.0)
                log->warn("Network (Card Q3): network.clock_anchor_var_floor_m2 = 0 leaves the stiff "
                          "clock carry UNFLOORED.  A diagonal (or dense-diagonal) carry accumulates "
                          "information ~n against a time-correlated error floor, so its formal "
                          "variance drifts to 0 — assessment deviation #1, the mechanism behind the "
                          "measured lock-ins.  The B4 phase/code-bias carries are floored for exactly "
                          "this reason; the clock's historical exemption assumed a carry that never "
                          "bound.");
            if (network_clock_anchor_recouple_epochs_ < 1)
                log->warn("Network (Card Q3): network.clock_anchor_recouple_epochs = {} re-engages the "
                          "anchor with NO re-validation — standing invariant #7 requires the "
                          "re-coupling edge to re-validate or re-seed what it reconnects.",
                          network_clock_anchor_recouple_epochs_);
            // NOTE the clock_model interlock is deliberately NOT here: service_mode
            // FORCE-ARMS network_clock_model_ several hundred lines BELOW this parse
            // site, so a check here would read the pre-arming value and affirmatively
            // misreport the active mechanism — the exact defect the C1 card recorded
            // against this very knob pair.  It lives in the P3 post-arming
            // re-validation block instead.
        }
        network_phase_datum_hold_sigma_m_ =
            jval<double>(json, "network.phase_datum_hold_sigma_m",
                         "network_phase_datum_hold_sigma_m", 0.0);
        network_phase_datum_pref_elev_deg_ =
            jval<double>(json, "network.phase_datum_pref_elev_deg",
                         "network_phase_datum_pref_elev_deg", 15.0);
        network_phase_datum_sticky_ =
            jval<bool>(json, "network.phase_datum_sticky",
                       "network_phase_datum_sticky", true);
        // TWO-EDGE CONTINUOUS HANDOVER (docs/SPEC_pivot_visibility_gauge_defect.md).
        // Stickiness minimizes the NUMBER of handovers; this makes the ones that
        // still happen frame-preserving.  The two are complementary, not
        // alternatives — sticky alone still re-realizes on every handover it cannot
        // avoid, which is what turned the prioritized election into 15 clock steps.
        network_phase_datum_handover_continuous_ =
            jval<bool>(json, "network.phase_datum_handover_continuous",
                       "network_phase_datum_handover_continuous", false);
        // ---- RF-2 (docs/refound/RF-1_steps345_mechanism_2026-09-15.md): the phase datum
        //      as a HELD S-BASIS.  RF-1 made every ambiguity a float state — including,
        //      accidentally, the one that IS the satellite phase bias's datum.  The
        //      textbook makes the S-basis ambiguities CONSTANTS, not states.
        network_phase_datum_held_basis_ =
            jval<bool>(json, "network.phase_datum_held_basis",
                       "network_phase_datum_held_basis", false);
        network_phase_anchor_min_age_ep_ =
            jval<int>(json, "network.phase_anchor_min_age_ep",
                      "network_phase_anchor_min_age_ep", 10);
        network_phase_anchor_flag_quiet_ep_ =
            jval<int>(json, "network.phase_anchor_flag_quiet_ep",
                      "network_phase_anchor_flag_quiet_ep", 5);
        if (network_phase_anchor_min_age_ep_ < 0)
            throw std::runtime_error("network.phase_anchor_min_age_ep must be >= 0 "
                                     "(0 = no age gate)");
        if (network_phase_anchor_flag_quiet_ep_ < 0)
            throw std::runtime_error("network.phase_anchor_flag_quiet_ep must be >= 0 "
                                     "(0 = no quiet-window gate)");
        // ---- RF-2d (docs/refound/RF-3-read_handover_frame_2026-09-15.md): the held-basis
        //      handover in its own frame algebra — release a COLD family's retired-frame
        //      carries, and declare exactly the cold handovers.
        network_phase_datum_held_cold_release_ =
            jval<bool>(json, "network.phase_datum_held_cold_release",
                       "network_phase_datum_held_cold_release", false);
        network_phase_datum_held_mark_cold_ =
            jval<bool>(json, "network.phase_datum_held_mark_cold",
                       "network_phase_datum_held_mark_cold", false);
        network_phase_datum_handover_max_var_m2_ =
            jval<double>(json, "network.phase_datum_handover_max_var_m2",
                         "network_phase_datum_handover_max_var_m2", 1.0);
        if (!(network_phase_datum_handover_max_var_m2_ > 0.0)) {
            log->error("Network: network.phase_datum_handover_max_var_m2 must be > 0 "
                       "(got {}); it is the re-validation ceiling that stops an "
                       "ill-determined estimate being stamped in as a datum.",
                       network_phase_datum_handover_max_var_m2_);
            return false;
        }
        // The HONEST-σ-SCALE ceiling (SPEC_amb_iono_split_overcount §6.8.3 / §7 item 15):
        // a ledger value is usable only if var <= ratio × its arc's first-stash
        // conditional variance.  0 = OFF (the absolute ceiling above alone).
        network_phase_datum_handover_max_var_ratio_ =
            jval<double>(json, "network.phase_datum_handover_max_var_ratio",
                         "network_phase_datum_handover_max_var_ratio", 0.0);
        if (!(network_phase_datum_handover_max_var_ratio_ >= 0.0)) {
            log->error("Network: network.phase_datum_handover_max_var_ratio must be >= 0 "
                       "(got {}); 0 disables the ratio test.",
                       network_phase_datum_handover_max_var_ratio_);
            return false;
        }
        // COLD ANCHOR-FALLBACK (docs/SPEC_event_harness.md §15 + §16.8).  The two
        // knobs above decide what TARGET a handover's successor is pinned at; this
        // one decides WHICH STATION reaches that decision.  Today's election takes
        // the first coverage-ranked tracker and only then discovers its carry is
        // unusable, so the family re-realizes cold while a lower-ranked tracker with
        // a mature usable arc sits unused (43 of 47 non-cold-start cold handovers on
        // the storm arm had >= 3 mature alternatives).  Default OFF = byte-identical.
        network_phase_datum_candidacy_usable_ =
            jval<bool>(json, "network.phase_datum_candidacy_usable",
                       "network_phase_datum_candidacy_usable", false);
        // Iteration 2 (SPEC_event_harness §15.8) — the SATELLITE clause.  Iteration 1's
        // per-family screen let a satellite's three bands elect three hosts: producer
        // channels improved on every axis and the consumer median rose 49 % with
        // split-referenced satellites 0.0 -> 0.9 mean / 3 max.  Default TRUE, so the
        // knob above ships its corrected form; OFF reproduces iteration 1 exactly,
        // which is what makes the A/B a single knob.
        network_phase_datum_candidacy_satellite_ =
            jval<bool>(json, "network.phase_datum_candidacy_satellite",
                       "network_phase_datum_candidacy_satellite", true);
        network_phase_datum_candidacy_min_age_ep_ =
            jval<int>(json, "network.phase_datum_candidacy_min_age_ep",
                      "network_phase_datum_candidacy_min_age_ep", 60);
        if (network_phase_datum_candidacy_min_age_ep_ < 0) {
            log->error("Network: network.phase_datum_candidacy_min_age_ep must be >= 0 "
                       "(got {}); 0 disables the maturity test and lets the variance "
                       "ceiling decide alone.",
                       network_phase_datum_candidacy_min_age_ep_);
            return false;
        }
        // QUEUE item 1(d) — the PHASE_ANCHOR carry re-reference
        // (docs/SPEC_pivot_visibility_gauge_defect.md §5.07.5 R1).  The handover
        // knob above acts on the DESIGN-MATRIX side of the edge (what target the
        // incoming pin gets); this one acts on the CARRY side (the retired-frame
        // prior is marginalized for one epoch instead of being folded against the
        // re-realized datum at q_pb = 1e-12).  §5.07 measured the un-transformed
        // carry, not the pin target, as the root the satellite clock absorbs.
        network_phase_anchor_carry_reref_ =
            jval<bool>(json, "network.phase_anchor_carry_reref",
                       "network_phase_anchor_carry_reref", false);
        // Coverage-threshold gating of the anchored union: a shared phase-bias
        // column enters the union only when >= this many present stations track it
        // with a usable ambiguity.  1 (default) = every tracked key, the original
        // prioritized-datum behavior (byte-identical); 2-3 excludes the thinly-
        // tracked columns that eject marginal stations as non-PD, re-level on a
        // one-station anchor pool, and leak their anchor lump into the clock↔PB
        // split.  INERT unless network.phase_datum_prioritized.
        network_phase_datum_min_stations_ =
            jval<int>(json, "network.phase_datum_min_stations",
                      "network_phase_datum_min_stations", 1);
        if (network_phase_datum_min_stations_ < 1) {
            log->error("Network: network.phase_datum_min_stations must be >= 1 (got {}); "
                       "1 = no gating (every tracked key enters the anchored union).",
                       network_phase_datum_min_stations_);
            return false;
        }
        // Card EV7 §6 — the gate's DROP EDGE
        // (docs/refound/EV7_all_sat_clock_decouple_2026-09-21.md §5.3).  The gate
        // above assumes the tracking stations' float ambiguities absorb the removed
        // bias; on the FLOAT vehicle that float is a CARRIED state at a centimetre
        // prior, so it absorbs the whole bias in one epoch and the satellite clocks
        // pay for it.  Armed, the dropped family joins the re-reference set and its
        // carries are erased at the edge, exactly as at the family's return.
        network_phase_datum_gate_drop_erase_ =
            jval<bool>(json, "network.phase_datum_gate_drop_erase",
                       "network_phase_datum_gate_drop_erase", false);
        // Card KP (docs/SPEC_beidou_w6_datum.md §4) — the CONDITIONAL second lever,
        // behind the DG rank-gate calibration and the PA drop-and-heal.  Default
        // OFF; INERT without estimate_sat_code_bias (no OSB columns exist).
        network_cb_kaula_prior_ =
            jval<bool>(json, "network.cb_kaula_prior", "network_cb_kaula_prior", false);
        network_cb_kaula_sigma_m_ =
            jval<double>(json, "network.cb_kaula_sigma_m",
                         "network_cb_kaula_sigma_m", 10.0);
        if (network_cb_kaula_prior_ && !(network_cb_kaula_sigma_m_ > 0.0)) {
            // info = 1/sigma^2 with sigma <= 0 is a HARD pin on every admitted OSB,
            // i.e. a silent datum takeover wearing a "loose prior" label.  Refuse.
            log->error("Network: network.cb_kaula_sigma_m must be > 0 (got {:g}); it is the "
                       "standard deviation of the admitted-OSB physical-bound prior "
                       "(info = 1/sigma^2).  Default 10.0 m (~33 ns).",
                       network_cb_kaula_sigma_m_);
            return false;
        }
        if (network_cb_kaula_prior_) {
            if (!estimate_network_sat_code_bias_)
                log->warn("Network (Card KP): network.cb_kaula_prior is ON but "
                          "network.estimate_sat_code_bias is OFF — there are no satellite "
                          "code-bias columns to put a prior on, so the toggle is INERT.  "
                          "[Standing invariant #4: an armed card with nothing to act on is "
                          "a miswire, not a quiet run.]");
            else
                log->info("Network producer (Card KP): admitted-OSB physical-bound prior ON — "
                          "value 0, sigma {:g} m (info {:g}) on every REGISTERED satellite "
                          "code-bias column, folded through the diagonal prior channel (NOT "
                          "the tight code-bias gauge rows), so the declared S-basis dimension "
                          "is unchanged.  Targets a CODE-dominated near-null direction the "
                          "phase drop-and-heal cannot reach (it anchors PB columns only).  "
                          "The K2 anti-fragility margin vs the armed rank-gate cutoff is "
                          "computed and logged at the first fold.",
                          network_cb_kaula_sigma_m_,
                          1.0 / (network_cb_kaula_sigma_m_ * network_cb_kaula_sigma_m_));
        }
        // Card DG (docs/SPEC_beidou_w6_datum.md §2) — the shared-border null-space
        // rank gate's RELATIVE eigenvalue tolerance becomes a per-config
        // calibration parameter.  Default 1e-9 == NetworkEstimator::kNullSpaceTol
        // (which stays the single source of that default), so an unset key sends
        // the identical double through the identical arithmetic: byte-identical.
        // The resolved value is handed to NetworkEstimator::setRankGateTol once,
        // pre-loop, and read from there by ALL THREE consumers (gate, estimability
        // dump, prioritized drop-and-heal) — DG2's "no private tolerances" rule.
        network_rank_gate_tol_ =
            jval<double>(json, "network.rank_gate_tol",
                         "network_rank_gate_tol", 1e-9);
        if (!(network_rank_gate_tol_ > 0.0)) {
            // NaN also lands here (every comparison false) — deliberately, since a
            // NaN cutoff makes every `ev < cutoff` test false and would report a
            // rank-0 null space on a perfectly healthy system.
            log->error("Network: network.rank_gate_tol must be > 0 (got {:g}); it is the "
                       "RELATIVE eigenvalue tolerance of the shared null-space rank gate "
                       "(cutoff = tol x max(lambda_max, 1)).  Default 1e-9.",
                       network_rank_gate_tol_);
            return false;
        }
        if (network_rank_gate_tol_ < 1e-16) {
            // Clamp rather than fail: a tolerance below double's relative
            // resolution against lambda_max cannot count ANY direction as null,
            // which silently converts the gate into "declared_dim must be 0" — an
            // OFF switch disguised as a tuning value.  Standing invariant #4: an
            // inert safety gate must never look clean.
            log->warn("Network: network.rank_gate_tol {:g} is below the 1e-16 floor and was "
                      "clamped to 1e-16 — a cutoff that deep counts NO direction as null, "
                      "which turns the rank gate off rather than tuning it.  Place the "
                      "value from the config's own estimability dump (>= 2 decades above "
                      "the gauge depth, >= 2 decades below the weakest-real layer).",
                      network_rank_gate_tol_);
            network_rank_gate_tol_ = 1e-16;
        }
        if (network_rank_gate_tol_ != 1e-9)
            log->info("Network producer: rank-gate tolerance CALIBRATED to {:g} (default 1e-9) "
                      "— cutoff = {:g} x max(lambda_max, 1), shared by the gate, the "
                      "estimability dump and the prioritized drop-and-heal.  This is an "
                      "exact-equality gate that throws in BOTH directions: a value below "
                      "this config's own gauge depth stops counting the declared gauges and "
                      "withholds a HEALTHY config.  Justify it from this config's "
                      "network_estimability.csv (network.trace_estimability).",
                      network_rank_gate_tol_, network_rank_gate_tol_);
        // SBC S-basis completion (network.sbasis_complete): derive the epoch's
        // REALIZED S-basis dimension pre-gate and constrain the near-null
        // directions BEYOND the declared structural dimension along their own
        // eigenvectors (RHS = carried level) instead of withholding the epoch /
        // Card-PA column-anchoring.  Built for the FT63 GPS+Gal observed-4-vs-
        // declared-2 signature: per-satellite channel-family phase-bias↔iono
        // trade directions a second constellation's C/Q-vs-X receiver split
        // makes reachable and the per-(sat,signal) anchor pins cannot close.
        network_sbasis_complete_ =
            jval<bool>(json, "network.sbasis_complete",
                       "network_sbasis_complete", false);
        network_sbasis_complete_max_ =
            jval<int>(json, "network.sbasis_complete_max",
                      "network_sbasis_complete_max", 6);
        if (network_sbasis_complete_) {
            if (network_sbasis_complete_max_ <= 0)
                log->warn("Network (SBC): network.sbasis_complete_max {} removes the "
                          "per-epoch direction cap — every observed-beyond-declared "
                          "near-null direction will be constrained, including a "
                          "pathological burst.  Diagnostic arms only; shipping configs "
                          "keep a small cap (default 6, measured need 1-3).",
                          network_sbasis_complete_max_);
            log->info("Network producer: SBC S-basis completion ON (cap {}/epoch) — the "
                      "rank gate's declared dimension stays the STRUCTURAL count; "
                      "realized extra near-null directions are constrained along their "
                      "own eigenvectors at the carried level (engage/release events, "
                      "tier GATE) instead of withholding or Card-PA column-anchoring.",
                      network_sbasis_complete_max_);
        }
        // DATUM GRAPH CENSUS (docs/SPEC_datum_graph_census.md).  Parsed HERE,
        // next to SBC, because it is SBC's mirror: SBC measures the realized
        // S-basis dimension spectrally and closes it, this census PREDICTS the
        // same dimension from the epoch's support topology (LASER 5-62/5-63) and
        // reconciles the two.  MEASURE-ONLY and fingerprint-SAFE — it moves no
        // weight, no state and no declared dimension, so it is legal on a warm
        // resume and against a shared snapshot.
        {
            NetworkConfig& dgc = network_config_;
            dgc.datum_graph_census =
                jval<bool>(json, "network.datum_graph_census", "datum_graph_census", false);
            dgc.datum_graph_event_holdoff_ep =
                jval<int>(json, "network.datum_graph_event_holdoff_ep",
                          "datum_graph_event_holdoff_ep", 20);
            if (dgc.datum_graph_census) {
                if (dgc.datum_graph_event_holdoff_ep < 0) {
                    log->warn("Network (datum graph): network.datum_graph_event_holdoff_ep "
                              "{} is negative — clamped to 0 (every identity events every "
                              "epoch).  The CSV rows are never rate-limited either way.",
                              dgc.datum_graph_event_holdoff_ep);
                    dgc.datum_graph_event_holdoff_ep = 0;
                }
                if (network_clock_datum_sum_to_zero_)
                    log->warn("Network (datum graph): armed under the SUM-TO-ZERO clock "
                              "datum, where Plan::declared_dimension is 0 because the "
                              "clock gauges are folded BEFORE the rank gate.  The census "
                              "still runs and tier 1 still counts components, but read its "
                              "d = 1 as 'one folded gauge row per constellation, assumed "
                              "to serve one component' — NOT as a declared null direction. "
                              "A k > 1 reading there means the single sum-to-zero row is "
                              "serving k disjoint clock sets, which the spectrum sees as a "
                              "deficiency the gate cannot attribute.");
                if (!estimate_network_sat_code_bias_)
                    log->warn("Network (datum graph): armed with "
                              "network.estimate_sat_code_bias OFF — no satellite code-bias "
                              "column exists, so the tier-2 phase-bias<->iono trade the "
                              "predicate names cannot form and BOTH tier-2 predicates are "
                              "held off (counted as cells_no_cb_layer).  Tier 1 is "
                              "unaffected.  This is the correct reading, not a defect: "
                              "with no code column the code observations pin the slant "
                              "ionosphere directly.");
                log->info("Network producer: DATUM GRAPH CENSUS ON (event holdoff {} ep) — "
                          "per epoch, union-find over the admitted (station, satellite) "
                          "support graph per constellation (tier 1, LASER 5-63) plus the "
                          "(satellite x channel-family) code-support partition (tier 2, the "
                          "Aug-2 mechanism), reconciled against the epoch's SPECTRAL "
                          "observation into predicted-and-observed / predicted-only / "
                          "observed-only cells.  Trace network_datum_graph.csv, events tier "
                          "DATUMGRAPH.  MEASURE-ONLY: nothing here feeds the null-space "
                          "gate, SBC, Card PA or Plan::declared_dimension.  Identities are "
                          "available on the spectral side only with network.sbasis_complete "
                          "armed; without it the reconciliation is COUNT-level and says so "
                          "(obs_src=count).",
                          dgc.datum_graph_event_holdoff_ep);
            }
        }
        // Pin-aware station PD guard: reference the addStation local-block rank test
        // to the MEDIAN |pivot| instead of the max, so a deliberate in-block 1e10
        // anchor pin (prioritized phase datum) no longer inflates the rejection bar
        // ~10 orders and ejects a healthy marginal station every epoch (the
        // TWTF/VOIM class — proven pin-driven by the 2026-07-04 A/B: identical
        // failing pivots with the coverage gate on/off).
        network_station_pd_pin_aware_ =
            jval<bool>(json, "network.station_pd_pin_aware",
                       "network_station_pd_pin_aware", false);
        if (network_station_pd_pin_aware_)
            log->info("Network producer: PIN-AWARE station PD guard ON — the local-block "
                      "rank test references the median |LDLᵀ pivot| (robust to gauge-pin "
                      "rows) instead of the max; marginal stations carrying anchor pins "
                      "are no longer ejected for a healthy ~1e-3 weakest parameter.");

        return true;
    }

    // =================================================================
    //  network subsection helper: validateNetworkDatumCoherence_
    // =================================================================
    /**
     * @brief Cross-knob coherence for the datum, discontinuity and carry families.
     *
     * The single place the datum realizations are made mutually exclusive and reported:
     * retro-bump -> clock-disc coupling, sum-to-zero / broadcast-anchored / pivot,
     * @c code_datum_time_linked vs @c clock_signal_basis_hold, the set-change trigger, and
     * the Card-4 requirement chain (@c carry_invariant -> @c phase_datum_eq34;
     * @c innov_gate -> @c carry_invariant; @c mw_wl.enable -> @c carry_invariant).
     *
     * @param log   The "gnss_config" logger.
     * @return @c false on a fatal misconfiguration (aborts @c loadFromJson).
     */
    bool GnssConfig::validateNetworkDatumCoherence_(
                                       const std::shared_ptr<spdlog::logger>& log) {
        // Alias preamble: the body below reads the driver local it was written
        // against.  network_service_mode_ is written once, at the top of the
        // section, and never again — so this is the value the un-split code saw.
        const bool service_mode = network_service_mode_;

        // R4: the B1 retro-bump steps a flagged satellite's phase, code AND clock discontinuity
        // counters, but the emitter only maintains/stamps the CLOCK counter when emit_clock_disc
        // is on (otherwise disc_counter stays 0 for byte-identity).  So with ura_retro_bump on
        // but emit_clock_disc off, the clock channel would silently never bump — the consumer
        // would reset the phase/code ambiguities of a contaminated satellite but keep integrating
        // its bad clock.  Couple them: arm the clock-disc channel so the retro-bump reaches it.
        if (network_ura_retro_bump_ && !network_emit_clock_disc_) {
            log->warn("Network (R4): enabling network.emit_clock_disc alongside "
                      "network.ura_retro_bump — the per-satellite retro-bump only reaches the "
                      "CLOCK discontinuity counter when the clock-disc channel is armed.  "
                      "(This also scopes the handover datum bump per constellation, A2.)");
            network_emit_clock_disc_ = true;
        }
        // The retro-bump's flag sources are the QC tiers: within-epoch/post-fit/attribution
        // feed it only under the raim toggles, the clock tiers under clock_monitor/clock_model.
        // With every source off the retro-bump can never fire, yet the R4 coupling above has
        // already switched the product to the clock-disc format — a dead knob with a format
        // side-effect deserves a loud note.
        if (network_ura_retro_bump_ && !network_raim_ && !network_raim_within_epoch_ &&
            !network_clock_monitor_ && !network_clock_model_ && !service_mode)
            log->warn("Network (B1): network.ura_retro_bump is on but NO flag-producing QC tier "
                      "is armed (raim / raim_within_epoch / clock_monitor / clock_model all "
                      "off) — the retro-bump can never fire.  Enable at least network.raim.");
        if (network_pivot_vanish_return_signal_)
            log->info("Network producer (A1): vanish-return pivot handover signalling ON — a "
                      "constellation absent for an epoch that returns with a different clock pivot "
                      "now forces a discontinuity bump (persistent last-ever-pivot map).");
        if (network_emit_clock_disc_)
            log->info("Network producer (A2): clock discontinuity channel ON + per-constellation "
                      "datum-bump scoping — the forced handover bump is scoped to the affected "
                      "constellation(s) and satellite clocks carry a disc counter.");
        if (network_sticky_pivot_)
            log->info("Network producer (RTA-1): sticky clock-pivot selection ON — the incumbent "
                      "pivot satellite (per constellation, from the last emitted product) is kept "
                      "while it remains registered; a handover occurs only when it sets.  A "
                      "config-pinned network.clock_datum_satellite still has priority.");
        if (network_clock_datum_sum_to_zero_) {
            log->info("Network producer: SUM-TO-ZERO clock datum ON — each constellation's clock "
                      "common-mode is fixed by Σ δtˢ = 0 over its satellite clocks (folded like the "
                      "code-bias gauge), NOT a pivot pin.  Anchor-free: no clock-pivot satellite, no "
                      "handover seam, no pivot-absent epoch skip; the S-contract isb_datum records it.");
            // A pivot pin and a sum-to-zero are mutually exclusive gauge realizations
            // — sum-to-zero has no pivot to place, so an explicit pin is a no-op here.
            if (!network_config_.clock_datum_satellite.empty())
                log->warn("Network: network.clock_datum_satellite '{}' is IGNORED under "
                          "network.clock_datum_sum_to_zero (the sum-to-zero clock datum has no pivot "
                          "satellite).  Clear one of the two to silence this warning.",
                          network_config_.clock_datum_satellite);
            // Sticky-pivot / vanish-return signalling are pivot-handover machinery that
            // the anchor-free datum removes; note the redundancy rather than silently
            // ignoring a user knob.
            if (network_sticky_pivot_ || network_pivot_vanish_return_signal_)
                log->warn("Network: network.sticky_pivot / network.pivot_vanish_return_signal are "
                          "INERT under network.clock_datum_sum_to_zero — the sum-to-zero clock datum "
                          "never re-references (there is no pivot to keep or hand over).");
        }
        if (network_clock_datum_broadcast_anchored_) {
            // Exactly one clock-gauge realization may be selected — two datums on
            // the same gauge would over-constrain (and the rank gate would not
            // catch it: both close the deficiency).  Fail loud.
            if (network_clock_datum_sum_to_zero_) {
                log->error("Network: network.clock_datum_broadcast_anchored and "
                           "network.clock_datum_sum_to_zero are mutually exclusive clock-gauge "
                           "realizations.  Pick one.");
                return false;
            }
            // The anchor target is each satellite's OWN a-priori clock, and WHICH
            // a-priori that is changes what the toggle means:
            //
            //  - brdc_rts: the target is the broadcast clock — a stable EXTERNAL
            //    reference the datum exists to hold the absolute frame against, and
            //    the auto resolution (3.0 m, broadcast-grade) binds it.
            //  - sp3: the target is the PRECISE product clock.  Anchoring to it
            //    re-derives the product — which is exactly the point of the
            //    EXTERNAL-CLOCK HOLD rung of the external-constraint ladder (the
            //    satellite-clock state is a RESIDUAL to the a-priori,
            //    MeasurementModel.cpp:1517-1531, so Δδtˢ → 0 at a tight σ IS "hold
            //    the satellite clock at the .CLK value", and σ is a continuous dial
            //    from fully-external to fully-self-contained through one code path).
            //    But the sp3 sigma AUTO resolution is initial_std.sat_clock (default
            //    1000 m), which leaves the gauge unrealized at the binding gate
            //    below.  So sp3 is legal only with an EXPLICIT σ — an omitted key
            //    there is always a mistake, never a posture.
            if (network_apriori_source_ != NetworkAprioriSource::BrdcRts
                && !(network_apriori_clock_anchor_sigma_m_ > 0.0)) {
                log->error("Network: network.clock_datum_broadcast_anchored under "
                           "network.apriori_source = 'sp3' needs an EXPLICIT "
                           "network.apriori_clock_anchor_sigma_m > 0.  The sp3 auto resolution "
                           "is initial_std.sat_clock ({} m), which is not a binding anchor, so "
                           "the clock gauge would read as unrealized and every epoch would be "
                           "withheld.  Set the sigma to the strength of the hold you want "
                           "(the .CLK formal sigma is ~0.01 m; 3 m is a loose frame tie), or "
                           "switch to apriori_source = 'brdc_rts' where 3.0 m auto-resolves.",
                           initial_std_.sat_clock);
                return false;
            }
            // The anchors ARE the gauge: each satellite's single-entry anchor row
            // folds (1/σ²)·I onto that constellation's clock columns, so the clock
            // common-mode direction u = (1/√N)·1 realizes eigenvalue uᵀ(σ⁻²I)u =
            // 1/σ² — independent of N (NOT ≈ N/σ²).  That eigenvalue must clear the
            // per-epoch rank-gate cutoff (≈ 1e-9·smax, and smax grows with the
            // station count), or the datum reads as under-specified and every epoch
            // is withheld.  A fixed load-time bound can only be a necessary floor:
            // reject the clearly non-binding here, and warn in the range that a large
            // network would reject at run time.  The brdc_rts auto default is 3.0 m
            // (1/σ² ≈ 0.11, safe at any realistic scale); only an explicit override
            // can be this weak.
            const double eff_sigma = resolveNetworkAprioriClockAnchorSigmaM();
            if (!(std::isfinite(eff_sigma) && eff_sigma > 0.0 && eff_sigma < 100.0)) {
                log->error("Network: network.clock_datum_broadcast_anchored needs a BINDING "
                           "per-satellite anchor sigma; the resolved "
                           "network.apriori_clock_anchor_sigma_m is {} m.  The realized clock "
                           "common-mode eigenvalue is 1/σ², which must exceed the per-epoch "
                           "rank-gate cutoff (~1e-9·smax); remove the explicit override "
                           "(brdc_rts auto-resolves 3.0 m) or set a binding value.",
                           eff_sigma);
                return false;
            }
            if (eff_sigma > 30.0) {
                log->warn("Network: network.apriori_clock_anchor_sigma_m = {:.1f} m is weak for "
                          "the broadcast-anchored clock datum — the realized common-mode "
                          "eigenvalue 1/σ² = {:.2e} may fall below the per-epoch rank cutoff "
                          "(~1e-9·smax) as the network grows (a ~305-station network rejects "
                          "σ ≳ 60 m as under-specified and withholds every epoch).  The brdc_rts "
                          "default (3.0 m) is safe at any scale.",
                          eff_sigma, 1.0 / (eff_sigma * eff_sigma));
            }
            if (network_apriori_source_ == NetworkAprioriSource::BrdcRts) {
                log->info("Network producer: BROADCAST-ANCHORED clock datum ON — each satellite's "
                          "clock is gauged by a soft anchor to its OWN broadcast clock (sigma "
                          "{:.2f} m, folded pre-gate as the datum), NOT a pivot pin or sum-to-zero. "
                          "No handover seam, and the absolute clock frame does NOT wander on "
                          "rise/set; the S-contract isb_datum records it.", eff_sigma);
            } else {
                // sp3 + anchored = the EXTERNAL-CLOCK HOLD.  Say so explicitly, and say
                // what it invalidates: the emitted satellite clock is now the precise
                // product plus a σ-bounded residual BY CONSTRUCTION, so grading it
                // against that product measures the anchor, not the network.  The
                // information this posture produces is on the RECEIVER side (rx clock,
                // rx DCB, iono, ambiguities) and at the held-out consumer.
                log->info("Network producer: EXTERNAL-CLOCK HOLD ON — clock_datum_broadcast_anchored "
                          "under apriori_source = 'sp3', so each satellite's clock is soft-held at "
                          "its PRECISE (.CLK) a-priori by a per-satellite anchor at sigma {:.4f} m "
                          "(realized common-mode eigenvalue 1/sigma^2 = {:.3e}), folded pre-gate as "
                          "the datum.  This is the external-constraint ladder's clock rung: the "
                          "satellite-clock state is a RESIDUAL to the a-priori, so sigma dials "
                          "continuously from fully-external (tight) to fully-self-contained (loose) "
                          "through one code path.  NOTE the emitted satellite clock is now the "
                          "precise product plus a sigma-bounded residual BY CONSTRUCTION — do NOT "
                          "grade it against the .CLK it was anchored to; grade the receiver side "
                          "and the held-out consumer.", eff_sigma,
                          1.0 / (eff_sigma * eff_sigma));
            }
            if (!network_config_.clock_datum_satellite.empty())
                log->warn("Network: network.clock_datum_satellite '{}' is IGNORED under "
                          "network.clock_datum_broadcast_anchored (the broadcast-anchored clock "
                          "datum has no pivot satellite).  Clear one of the two to silence this "
                          "warning.", network_config_.clock_datum_satellite);
            if (network_sticky_pivot_ || network_pivot_vanish_return_signal_)
                log->warn("Network: network.sticky_pivot / network.pivot_vanish_return_signal are "
                          "INERT under network.clock_datum_broadcast_anchored — the broadcast-"
                          "anchored clock datum never re-references (there is no pivot to keep "
                          "or hand over).");
        }
        if (network_clock_signal_basis_hold_) {
            // The per-satellite clock↔code-bias IF pin is a W6 construct; without
            // estimate_sat_code_bias there is no satellite code-bias state, so no
            // split to hold — arming the toggle alone changes nothing.  Warn rather
            // than silently ignore (this is a common mis-arming: the toggle targets
            // the SAME datum whose instability motivates it, which only exists in W6).
            if (!estimate_network_sat_code_bias_)
                log->warn("Network: network.clock_signal_basis_hold is INERT without "
                          "network.estimate_sat_code_bias — the per-satellite clock↔code-bias "
                          "IF pin it holds fixed only exists when the network estimates its own "
                          "satellite code biases (W6).");
            else
                log->info("Network producer: HOLD clock signal basis ON — each satellite's "
                          "ionosphere-free clock reference pair is latched on first registration "
                          "and held while both signals stay in the ≥2-station set, so the emitted "
                          "(clock, code-bias) split is time-consistent (the IGS fixed-clock-"
                          "convention analogue) rather than re-anchored by the satellite's "
                          "inter-channel bias whenever a sparse channel enters/leaves coverage.");
        }
        // Card 2: the time-linked code-bias datum and clock_signal_basis_hold BOTH
        // re-target the SAME per-satellite IF-pin row (the clock↔code split).  Running
        // both would double-realize that split with conflicting references — time-
        // linking already subsumes the hold for the pin VALUE (its RHS references the
        // previous solved IF combination directly).  Fail loud rather than silently
        // let one win.
        if (network_code_datum_time_linked_ && network_clock_signal_basis_hold_)
            throw std::runtime_error(
                "network.code_datum_time_linked and network.clock_signal_basis_hold are "
                "mutually exclusive (both re-target the per-satellite IF-pin row; the time-"
                "linked datum subsumes the hold for the pin value).  Enable at most one.");
        // FAMILY-GF §2.1: time-linking makes the GF pin and the sum-to-zero rows
        // INCONSISTENT, not merely redundant.  The GF rows over a channel pair sum
        // EXACTLY to (s2z(sig2) - s2z(sig1))/(mu2-mu1) — a rank-1 dependency that is
        // harmless while every RHS is 0, which is the untimed case.  Time-linking moves
        // the s2z targets to Sigma b_prev, generally different per channel, so the same
        // dependency now demands Sigma d_2 - Sigma d_1 = 0 AND Sigma d_j = rhs_j with
        // rhs_2 != rhs_1.  Nothing throws, because these fold as heavily weighted
        // pseudo-observations rather than exact constraints — it least-squares, quietly
        // injecting tension of order (rhs_2 - rhs_1)/(mu2 - mu1).  Latent today (no
        // shipped config combines them) and PRE-EXISTING for the primary pin, so this
        // warns rather than refuses; it is named here so the next arm does not pay for
        // it blind.
        if (network_code_datum_time_linked_ && network_code_bias_gf_datum_)
            log->warn("Network: network.code_datum_time_linked is armed together with "
                      "network.code_bias_gf_datum.  The geometry-free rows are rank-1 "
                      "dependent with the per-(constellation,signal) sum-to-zero rows, and "
                      "time-linking gives those rows NONZERO and per-channel-DIFFERENT "
                      "targets — so the dependency becomes inconsistent and least-squares "
                      "silently against the gauge weight instead of failing.  See "
                      "docs/SPEC_family_gf_datum.md §2.1.  Prefer the untimed code datum in "
                      "the slant posture.");
        if (network_code_datum_time_linked_) {
            // The datum it re-times (IF pin + per-(const,signal) sum-to-zero) only exists
            // in W6; without estimate_sat_code_bias there is nothing to time-link.
            if (!estimate_network_sat_code_bias_)
                log->warn("Network: network.code_datum_time_linked is INERT without "
                          "network.estimate_sat_code_bias — the code-bias IF-pin + sum-to-zero "
                          "datum it re-times only exists when the network estimates its own "
                          "satellite code biases (W6).");
            else
                log->info("Network producer (Card 2): TIME-LINKED code-bias datum ON — the per-"
                          "satellite IF pin and per-(constellation,signal) sum-to-zero rows pin "
                          "the CHANGE of the previous epoch's solved values (veteran-referenced "
                          "sum-to-zero RHS, previous-IF-combination pin RHS) instead of the level "
                          "to zero.  Same row count (rank unchanged); a no-veteran channel or a "
                          "first split re-seeds at 0 with a per-satellite disc bump so the value "
                          "step coincides with a signalled discontinuity.  Eliminates the set-mean "
                          "wander/relaxation after every satellite rise/set.");
        }
        // T3c: the sum-to-zero clock gauge re-references on a clock-satellite SET change;
        // with the clock-disc channel live that re-reference MUST bump the clock disc counters
        // or the consumer integrates across an unsignalled datum step.  Auto-arm the set-change
        // trigger when both its preconditions (sum_to_zero + emit_clock_disc) are satisfied but
        // it was left off — the "auto-arm (or fail loud)" the contract-honesty card requires.
        // Fires ONLY under this specific advanced combination; every other config is untouched.
        if (network_clock_datum_sum_to_zero_ && network_emit_clock_disc_
            && !network_clock_disc_on_set_change_) {
            log->warn("Network (T3c): auto-arming network.clock_disc_on_set_change — under the "
                      "sum-to-zero clock datum with the clock-disc channel on, a clock-satellite "
                      "set change re-references the gauge and MUST bump the clock disc counters "
                      "(otherwise the consumer integrates across an unsignalled datum step).");
            network_clock_disc_on_set_change_ = true;
        }
        if (network_clock_disc_on_set_change_) {
            // The trigger models "membership change = clock-datum re-reference",
            // which is true ONLY for the sum-to-zero gauge (the pivot pin holds
            // while the pivot is up; the broadcast anchors never move).  Armed
            // under any other datum it would mint spurious consumer resets — the
            // exact churn class this work removes.  Fail loud.
            if (!network_clock_datum_sum_to_zero_) {
                log->error("Network: network.clock_disc_on_set_change requires "
                           "network.clock_datum_sum_to_zero — only the sum-to-zero clock gauge "
                           "re-references on a membership change (pivot and broadcast-anchored "
                           "datums hold still).  Disable one of the two.");
                return false;
            }
            if (!network_emit_clock_disc_) {
                log->warn("Network: enabling network.emit_clock_disc alongside "
                          "network.clock_disc_on_set_change — the set-change trigger only "
                          "reaches the wire through the clock discontinuity channel.");
                network_emit_clock_disc_ = true;
            }
            log->info("Network producer: clock set-change discontinuity ON — a constellation "
                      "whose clock-satellite set changes between emits steps its clock disc "
                      "counters (the sum-to-zero datum re-reference, previously unsignalled); "
                      "replay-reproducible via the S-contract clkset= field.");
        }
        // Card 3/4 slip-aware election: the advisory belongs HERE, not in the parse block
        // that reads the knob — network_carry_invariant_ is parsed AFTER that block, so a
        // check placed there fires on every run regardless of the config (measured
        // 2026-08-02).  By this point every flag is settled.
        if (network_phase_datum_eq34_slip_aware_election_ && !network_carry_invariant_)
            log->warn("Network (Card 3/4): the slip-aware eq34 election is armed WITHOUT "
                      "network.carry_invariant — it will still keep slipped arcs out of the "
                      "spanning tree, but the payoff it exists for is measured in Card-4 chord "
                      "refloats, which do not exist on the legacy carry path.  Harmless; just "
                      "understand that nothing in this run will show the benefit.");
        if (network_code_bias_disc_jump_test_) {
            if (!estimate_network_sat_code_bias_)
                log->warn("Network: network.code_bias_disc_jump_test is INERT without "
                          "network.estimate_sat_code_bias (no code biases are emitted).");
            log->info("Network producer: code-bias VALUE jump test ON — disc_per_signal also "
                      "bumps on a fresh-baseline jump > {:.2f} m (on top of the structural "
                      "set/band-change triggers), so weak-separation code-bias wander is "
                      "signalled to the consumer; replay-reproducible via the S-contract "
                      "cbjump= field.", NetworkProductEmitter::kDefaultDiscThresholdM);
        }
        if (network_disc_threshold_frac_cycles_ > 0.0) {
            log->info("Network producer: per-λ discontinuity threshold ON — the phase (and, "
                      "under code_bias_disc_jump_test, code) jump test uses {:.3f}·λ(sat,signal) "
                      "instead of the flat {:.2f} m (uniform in CYCLES, still a fixed contract; "
                      "e.g. L1/E1 {:.4f} m, L2 {:.4f} m, L5/E5a {:.4f} m); replay-reproducible "
                      "via the S-contract discfrac= field.",
                      network_disc_threshold_frac_cycles_,
                      NetworkProductEmitter::kDefaultDiscThresholdM,
                      network_disc_threshold_frac_cycles_ * Constants::GPS_L1_WAVELENGTH,
                      network_disc_threshold_frac_cycles_ * Constants::GPS_L2_WAVELENGTH,
                      network_disc_threshold_frac_cycles_ * Constants::GPS_L5_WAVELENGTH);
        }
        if (network_scoped_handover_response_)
            log->info("Network producer (RTA-2): scoped handover response ON — the internal D2 "
                      "carry skip / clock-oscillator clear / jump-test suppression on a pivot "
                      "handover applies only to the constellation(s) whose datum changed, not "
                      "network-wide (the SH-VTEC carry skip stays global).");
        // Queue 1(b): the changed constellation's OWN carry — re-expressed, not dropped.
        if (network_handover_stransform_) {
            if (!network_scoped_handover_response_) {
                log->warn("Network: network.handover_stransform is INERT without "
                          "network.scoped_handover_response — the transform releases the D2 "
                          "carry skip for the CHANGED constellation, but with the global "
                          "response still armed every OTHER constellation would keep "
                          "free-falling, so the edge is not fixed.  Arm "
                          "scoped_handover_response (the golden pins it) or drop this knob.");
                network_handover_stransform_ = false;
            } else if (network_clock_datum_sum_to_zero_
                       || network_clock_datum_broadcast_anchored_) {
                log->warn("Network: network.handover_stransform is INERT under an anchor-free "
                          "clock datum (sum_to_zero / broadcast_anchored) — those realize the "
                          "clock gauge without a pivot satellite, so no handover seam exists "
                          "to transform across.");
                network_handover_stransform_ = false;
            } else {
                log->info("Network producer (queue 1(b)): handover S-TRANSFORM ON — at a "
                          "clock-pivot handover the CHANGED constellation's carried shared "
                          "means are re-expressed in the incoming datum along the structural "
                          "clock null direction (c0 = x_q / v_c[q] with q the new pivot; "
                          "x -= v_c*c0; var += v_c[i]^2*var_q) and the D2 shared-carry fold is "
                          "then NOT skipped for them — the carried information is valid, merely "
                          "expressed in the retired datum, so it is transformed instead of "
                          "discarded (SPEC_pivot_visibility_gauge_defect §8).  The emitted wire "
                          "still steps by the datum delta BY DESIGN; this is producer-internal "
                          "continuity.  ⚠ FINGERPRINTED: fresh convergence only — never flip it "
                          "mid-chain.");
            }
        }
        // Card 4 (M1) dependency: the DD cycle-basis carry lives on the eq34 chain
        // datum (it needs the per-receiver RPB split + the spanning tree that supply
        // the fundamental-cycle DDs).  Reject the incoherent combination fail-loud.
        if (network_carry_invariant_ && !network_phase_datum_eq34_) {
            log->error("Network: network.carry_invariant requires network.phase_datum_eq34 — the DD "
                       "cycle-basis carry is defined only on the eq34 chain datum (RPB split + "
                       "spanning tree supply the invariant DDs).  Enable phase_datum_eq34 or disable "
                       "carry_invariant.");
            return false;
        }
        // Card 4 (M1) D5: the fine-slip innovation gate is defined only on the carried DDs.
        if (network_carry_invariant_innov_gate_ && !network_carry_invariant_) {
            log->error("Network: network.carry_invariant_innov_gate requires network.carry_invariant — "
                       "the innovation test standardizes the CARRIED double-difference residual against "
                       "the fold's H0 covariance; with no z-store there is nothing to gate.  Enable "
                       "carry_invariant or disable the innovation gate.");
            return false;
        }
        // ---------------------------------------------------------------------
        // Standing invariant #1, applied to the Card-4 invariant DD carry.
        // ---------------------------------------------------------------------
        // The carried z is a stiff time-linked quantity: the recursion is a Kalman
        // filter with Φ = I on z, and at the shipped default q_z = 0.0 it is a pure
        // random CONSTANT.  Invariant #1 says such a quantity MUST have a wired
        // event response before an ON field run — "erase, refloat, or re-tap".
        //
        // What actually counts as that response for THIS carry (nothing else does):
        //   * the pre-fit raw detectors — network.cycle_slip.mw_detect (wide-lane +
        //     LLI) and network.cycle_slip.if_phase_td (IF-phase triple difference).
        //     Both drain into carry_slipped_ambs, which networkBuildCarryDdPriors
        //     reads via ambSlipped() to re-tap the chord from a diffuse prior.  The
        //     drain is guarded by `if (carry_invariant)` alone, so it is live from
        //     epoch 1 and does NOT need the innovation gate.
        //   * the post-fit D5 innovation gate — network.carry_invariant_innov_gate —
        //     which re-taps in-epoch and enters the funnel for the next one.  D5 is
        //     the only response that fires on MODEL ERROR rather than on a detected
        //     raw slip, so it covers a failure mode the pre-fit detectors cannot see.
        //
        // q_z is deliberately NOT accepted as a response.  It adds q_z·dt to Pᶻ's
        // DIAGONAL only (NetworkInvariantCarryDriver::beginEpoch); it never moves the
        // carried mean and never touches the off-diagonals that pin a stale value in
        // place, and it is applied identically to slipped and clean chords.  That is
        // undirected decay, not erase/refloat/re-tap — a q_z > 0 posture is leaky,
        // not guarded, which is exactly the distinction invariant #1 was written to
        // stop us from blurring.
        //
        // turbo_edit is NOT accepted either, even though the phase-leveling auto-arm
        // in loadNetworkSlipDetectors_ counts it as a pre-fit detector.  Under
        // carry_invariant its PASS-A branch takes the repair arm INSTEAD of collecting
        // MW refloat candidates, and the repair list it produces is consumed against
        // the phase-leveling LOCAL carry — the one carry_invariant supersedes.  It
        // therefore contributes nothing to carry_slipped_ambs.  A leveling config that
        // arms turbo_edit alone satisfies that auto-arm and still leaves this carry
        // with no response; naming it here is what closes that seam.
        //
        // Relation to the POSTURE BUNDLE "zero-q phase memory + slip response"
        // (GnssConfigNetworkPosture.cpp:595).  That row already names this invariant,
        // but it is an INFO line inside the once-per-run posture dump and its companion
        // list is the two raw detectors ONLY — so it reports "0/2 companions armed" on
        // the 107 configs whose wired response is the D5 gate, and a reader who has
        // learned to skim it past those false positives will skim it past a real one.
        // This interlock is the precise complement: WARN severity, D5 counted as the
        // response it is, turbo_edit explicitly refused, and zero false positives.
        //
        // WARN, not reject, and no state change: the solve stays byte-identical
        // (invariant #6) and no loader DEFAULT moves.  Measured over the whole config
        // corpus (1004 files resolved through their "extends" chains, not grepped),
        // 633 arm carry_invariant and ZERO reach this branch — 526 carry a pre-fit
        // detector and 107 rely on the D5 gate alone.  The invariant holds today by
        // convention across every config rather than by construction; this makes the
        // absence of a response impossible to reach silently.
        if (network_carry_invariant_ && !network_cycle_slip_mw_detect_
            && !network_cycle_slip_if_phase_td_ && !network_carry_invariant_innov_gate_) {
            log->warn("Network (Card 4, standing invariant #1): network.carry_invariant carries "
                      "the DD cycle basis as a stiff time-linked state (q_z = {:g} m²/s) but NO "
                      "event response is wired — network.cycle_slip.mw_detect and "
                      ".cycle_slip.if_phase_td are both off, so carry_slipped_ambs is never fed, "
                      "and network.carry_invariant_innov_gate is off, so there is no D5 re-tap "
                      "either.  An un-screened slip does not degrade this carry gracefully: the "
                      "post-slip phase is self-consistent with the SLIPPED integer, so the stiff "
                      "carry does not fight the slip, it LOCKS IT IN and the tension is absorbed "
                      "by the clock and iono states.  This posture is NOT field-runnable.  Arm "
                      "network.cycle_slip.mw_detect (the robust default — wide-lane + LLI, needs "
                      "no a-priori position), and/or .cycle_slip.if_phase_td, and/or "
                      "network.carry_invariant_innov_gate.  NOTE turbo_edit does NOT satisfy this "
                      "requirement under carry_invariant (it suppresses MW refloat collection and "
                      "repairs only the superseded leveling carry), and neither does a nonzero "
                      "q_z — that inflates Pᶻ's diagonal without moving the carried mean, which "
                      "is decay, not an event response.",
                      network_carry_invariant_qz_);
        } else if (network_carry_invariant_ && network_carry_invariant_qz_ <= 0.0
                   && !network_carry_invariant_innov_gate_) {
            // Response present, but it is pre-fit-detector-ONLY on a q_z = 0 carry.
            // Two gaps remain open and both are worth naming before a grading run:
            // a slip the raw detectors miss has no second line of defence, and a
            // MODEL-ERROR excursion (D5's target, not the detectors') has none at all.
            // This is the posture SPEC_synthetic_minimum_network §13.7's settled-trio
            // arm was measured on.
            log->warn("Network (Card 4, standing invariant #1): network.carry_invariant is armed "
                      "with q_z = 0 (random-constant DDs) and network.carry_invariant_innov_gate "
                      "OFF, so the ONLY event response on the carry is the pre-fit raw detector "
                      "drain (mw_detect={}, if_phase_td={}).  That covers DETECTED slips and "
                      "nothing else: a missed slip has no second line of defence, and a MODEL-"
                      "ERROR excursion — which is what the D5 innovation gate exists to catch — "
                      "has no response at all, while q_z = 0 means the carry cannot even leak "
                      "such an excursion off.  Legitimate as a research contrast; before grading "
                      "an integer or level channel on this posture, consider arming "
                      "network.carry_invariant_innov_gate so the runaway mode is instrumented.",
                      network_cycle_slip_mw_detect_, network_cycle_slip_if_phase_td_);
        }
        // Card 4 (M1) PD-safe DD fold: every mechanism it changes — the fold's middle
        // inverse, the posterior read-back, the degenerate-block rebirth, the restore
        // guard — lives inside the invariant carry.  Without carry_invariant there is no
        // DD-prior fold at all, so the knob is INERT, not wrong: WARN and disarm rather
        // than reject (a config that carries it for a later leg is not a config error).
        // Placed in this cross-knob pass, not the parse block, per the comment there.
        if (network_carry_invariant_pd_safe_ && !network_carry_invariant_) {
            log->warn("Network (Card 4 PD-safe fold): network.carry_invariant_pd_safe is set "
                      "WITHOUT network.carry_invariant — there is no DD-prior fold to make "
                      "PD-safe, so nothing in this run is evidence about the card.  "
                      "DISARMED for this run.");
            network_carry_invariant_pd_safe_ = false;
        } else if (network_carry_invariant_pd_safe_) {
            log->info("Network (Card 4 PD-safe fold): ON — the DD-prior fold reads its "
                      "posterior back in PRODUCT form sym(Pᶻ·M·B) instead of subtracting "
                      "Pᶻ−PᶻMPᶻ, factorizes the EQUILIBRATED S(Pᶻ+B)S against a "
                      "scale-RELATIVE pivot floor, repairs a still-tripping block by "
                      "clamping its eigenvalues (counted), and REBIRTHS a block that "
                      "still cannot factor — means kept, Pᶻ re-seeded diffuse — at "
                      "install AND at warm-resume restore.  This replaces a SILENT "
                      "permanent M:=0 freeze behind which 78% of the carried state was "
                      "measured diffusing (2026-08-02).  FINGERPRINTED: fresh convergence "
                      "only, never flip mid-chain.  Watch the run-end ddfold counters — "
                      "repairs should be dust-rate and rebirths should be rare.");
        }
        // Carry-aware covariance readback: the correction it applies is the folded DD
        // priors' rank-K term, so without carry_invariant there is nothing to correct —
        // INERT, not wrong.  Same WARN-and-disarm as the PD-safe knob above, same reason,
        // same cross-knob pass.
        if (network_carry_aware_covariance_ && !network_carry_invariant_) {
            log->warn("Network (carry-aware covariance): network.carry_aware_covariance is "
                      "set WITHOUT network.carry_invariant — there are no folded DD priors "
                      "to read back, so the correction is identically zero and nothing in "
                      "this run is evidence about the card.  DISARMED for this run.");
            network_carry_aware_covariance_ = false;
        } else if (network_carry_aware_covariance_) {
            log->info("Network (carry-aware covariance): ON — dd_sigma_cyc and the "
                      "producer-side AR probe now read Cov(x_r) from the SAME model the "
                      "solve used (Ã⁻¹ = N_rr⁻¹ − Σ_b Y_b M_b Y_bᵀ, the fold's own rank-K "
                      "term) instead of the prior-free N_rr⁻¹.  Both consumers are "
                      "READ-ONLY, so the SOLVE must stay BYTE-IDENTICAL to the OFF arm — "
                      "that is this card's gate, not merely its OFF-path check.  NOT "
                      "fingerprinted (no stored state changes meaning).  Watch the run-end "
                      "covcarry line: ratio ≡ 1.00 means the correction never moved "
                      "anything (dead wiring), and any nonpos_rows is the first "
                      "measurement of the fold's cross-block approximation error.");
        }
        // Card 4 (M1) D5 v2: the LS-VCE calibration only re-scales the gate's threshold.
        if (network_carry_invariant_innov_calibrate_ && !network_carry_invariant_innov_gate_) {
            log->error("Network: network.carry_invariant_innov_calibrate requires "
                       "network.carry_invariant_innov_gate — it standardizes the gate's innovation by "
                       "the empirical σ̂₀²; with the gate off there is nothing to calibrate.  Enable "
                       "the innovation gate or disable the calibration.");
            return false;
        }
        // M5 card WL-1: the WL layer rides the Card-4 chord basis, so v1 REQUIRES the
        // carry (mutex-check, SPEC §11 Q1).  Fail-loud rather than silently no-op.
        if (network_config_.mw_wl.enable && !network_carry_invariant_) {
            log->error("Network: network.mw_wl.enable requires network.carry_invariant — the "
                       "wide-lane pseudo-observations are double-differenced over the Card-4 "
                       "invariant carry's fundamental-cycle chord basis; with no carry there is "
                       "no cycle basis to seed.  Enable carry_invariant or disable mw_wl.");
            return false;
        }
        if (network_config_.mw_wl.enable) {
            log->info("Network MW-WL seed ON (M5 card WL-1) — MW Welford means DD'd over the "
                      "Card-4 chord basis fold as WL pseudo-obs (min_arc_epochs={}, "
                      "sigma_floor={} cyc, max_rows_per_epoch={}).  GLONASS excluded (with the "
                      "carry).  Per-station MW detector is built even with phase_leveling off.",
                      network_config_.mw_wl.min_arc_epochs, network_config_.mw_wl.sigma_floor_cyc,
                      network_config_.mw_wl.max_rows_per_epoch);
            // Phase B (WL integer fixing) — config contract only in v1.  The un-fix
            // wiring (standing invariant #1: no stiff integer assertion without a
            // wired+force-fire-tested slip response) is a follow-up increment; warn
            // rather than silently fold nothing.
            if (network_config_.mw_wl.fix_enable)
                log->warn("Network: network.mw_wl.fix_enable is set but Phase-B WL integer "
                          "fixing is NOT yet implemented (v1 is Phase-A float only) — the fix "
                          "gate is INERT.  Grade Phase A first (SPEC §7.7), then arm Phase B "
                          "once its un-fix wiring is force-fire tested (SPEC §7.4).");
            if (network_config_.mw_wl.f6_dump)
                log->info("Network: network.mw_wl.f6_dump ON — one-shot F6 attribution dump "
                          "to logs/network_mw_wl_f6.csv on the first arming epoch (per-arc "
                          "MW̄/x_lin + per-row z_MW/z_lin/z_corr/reconstructed_abs/innovation). "
                          "Log-only; does not change the solve.");
        } else if (network_config_.mw_wl.fix_enable) {
            log->warn("Network: network.mw_wl.fix_enable is INERT without network.mw_wl.enable "
                      "(Phase B requires Phase A).");
        }

        return true;
    }

    // =================================================================
    //  network subsection helper: validateNetworkLaneLayers_
    // =================================================================
    /**
     * @brief WL-2 / UWL-1 / WL-3 lane contracts and their ON reports.
     *
     * Split from @c loadNetworkLaneLayers_ because every rule here needs
     * @c network_carry_invariant_ and the slip-detector flags, which are parsed after the
     * lane block.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     * @return @c false on a fatal misconfiguration (aborts @c loadFromJson).
     */
    bool GnssConfig::validateNetworkLaneLayers_(const nlohmann::json& json,
                                                const std::shared_ptr<spdlog::logger>& log) {
        // ---- M5 card WL-2: EWL-first tri-frequency cascade (SPEC_ewl_wl_cascade_card.md). ----
        {
            auto& mw = network_config_.mw_wl;
            // Both lanes ride the Card-4 chord basis (EWL rows on it; inject edits the
            // carried z's), so both REQUIRE the carry — fail-loud like WL-1.
            if ((mw.ewl_enable || mw.wl_inject_enable) && !network_carry_invariant_) {
                log->error("Network: network.mw_wl.{{ewl_enable,wl_inject_enable}} require "
                           "network.carry_invariant — the EWL/WL lanes ride the invariant carry's "
                           "fundamental-cycle chord basis (and the WL inject edits its carried DDs). "
                           "Enable carry_invariant or disable the WL-2 lanes.");
                return false;
            }
            // The WL bridge path (§3.4) needs the EWL integers; without ewl_enable the WL
            // lane runs MW(1,2)-fallback only (§3.5) — allowed, but warn so it's intentional.
            if (mw.wl_inject_enable && !mw.ewl_enable)
                log->warn("Network: network.mw_wl.wl_inject_enable is ON without ewl_enable — the "
                          "phase-bridge WL path (SPEC §3.4) is disabled; the WL lane runs the plain "
                          "MW(1,2) fallback only (SPEC §3.5, FFR at n≈25–50 ep).  Set ewl_enable for "
                          "the fast tri-frequency bridge.");
            // WL-1's float layer (mw.enable) is superseded by WL-2 (SPEC §3.0.2): it stays as a
            // default-OFF diagnostic; running it WITH the inject double-drives the WL columns.
            if (mw.enable && mw.wl_inject_enable)
                log->warn("Network: network.mw_wl.enable (WL-1 float rows) is a DIAGNOSTIC under "
                          "WL-2 and is armed together with wl_inject_enable — the float pseudo-obs "
                          "and the inject both drive the WL columns (F6: the float layer loses and "
                          "adds churn).  Disable mw_wl.enable for a clean WL-2 A/B.");
            // Deprecate the WL-1 Phase-B posterior-gated surface — WL-2 fixes on DETECTOR
            // statistics (SPEC §3.0.1), never the carry-contaminated posterior.
            if ((mw.ewl_enable || mw.wl_inject_enable) && mw.fix_enable)
                log->warn("Network: network.mw_wl.fix_enable (WL-1 Phase-B, never implemented) is "
                          "REPLACED under WL-2 by statistic-gated fixing "
                          "(ewl_fix_*/wl_fix_*/inject_disagree_min_cyc).  The fix_enable flag is "
                          "ignored.");
            if (mw.ewl_enable)
                log->info("Network WL-2 EWL lane ON (M5 card WL-2) — extra-wide-lane statistic rows "
                          "on the Card-4 chord basis, third band UNCARRIED so they win cleanly; FFR "
                          "fix at σ<{} / |frac|<{} cyc, eligible at n>={} (ep ~{}).  Requires "
                          "frequency_mode with the 3rd band (GPS L5 / Galileo E5b).",
                          mw.ewl_fix_sigma_cyc, mw.ewl_fix_frac_cyc, mw.ewl_min_arc_epochs,
                          mw.ewl_min_arc_epochs + 1);
            if (mw.wl_inject_enable)
                log->info("Network WL-2 WL inject ON (M5 card WL-2) — WL integers (EWL-bridge {} / "
                          "MW(1,2) fallback) injected into the Card-4 carry via injectLaneTarget "
                          "(set ∇ΔN₁−∇ΔN₂, preserve NL) + hold row + 1-epoch D5 exemption; trigger "
                          "|stat−carried|>{} cyc, FFR σ<{}/|frac|<{}, bridge σ guard {} cyc.",
                          mw.ewl_enable ? "armed" : "OFF", mw.inject_disagree_min_cyc,
                          mw.wl_fix_sigma_cyc, mw.wl_fix_frac_cyc, mw.bridge_max_sigma_cyc);
            // ---- M5 card UWL-1: per-constellation ultra-wide-lane top tier (SPEC_uwl_card.md). ----
            if (mw.uwl_enable && !mw.ewl_enable) {
                log->error("Network: network.mw_wl.uwl_enable requires network.mw_wl.ewl_enable — "
                           "the UWL top tier IS the EWL machinery run on the per-constellation "
                           "lane-tier pair (statistic layer, FFR gates, bridge, telemetry).  "
                           "Enable ewl_enable or disable uwl_enable.");
                return false;
            }
            if (mw.uwl_cold_fix && !mw.uwl_enable)
                log->warn("Network: network.mw_wl.uwl_cold_fix is INERT without uwl_enable — the "
                          "n=1 cold fix applies only to lane tiers with λ ≥ {} m, which only the "
                          "UWL table provides (GPS's 5.861 m EWL is excluded by design).",
                          mw.uwl_cold_min_lambda_m);
            if (mw.uwl_mid_enable && !mw.uwl_enable)
                log->warn("Network: network.mw_wl.uwl_mid_enable is INERT without uwl_enable — "
                          "the E6 mid-ladder hop rides the armed lane-tier table (Galileo "
                          "(E6,E5a) λ 2.931 m rung).");
            // ---- Increment 5: dual-top tier contract. ----
            if (mw.uwl_dual_top && !mw.uwl_enable)
                log->warn("Network: network.mw_wl.uwl_dual_top is INERT without uwl_enable — "
                          "the preferred/fallback top rung is a property of the armed "
                          "lane-tier table.");
            if (mw.uwl_dual_top && mw.uwl_enable && mw.ewl_fold_rows) {
                log->warn("Network: network.mw_wl.ewl_fold_rows is FORCED OFF under "
                          "uwl_dual_top — a cycle decided by both the preferred and the "
                          "fallback rung must never fold two integer rows (the F-A refold "
                          "class), and fold-off is the validated production shape anyway.");
                mw.ewl_fold_rows = false;
            }
            if (mw.uwl_dual_top && mw.uwl_enable
                && jval<int>(json, "network.mw_wl.uwl_gal_top_band",
                             "network_mw_wl_uwl_gal_top_band", 7) != 7)
                log->info("Network: network.mw_wl.uwl_gal_top_band is IGNORED under "
                          "uwl_dual_top — the dual tier always prefers '8' (E5 AltBOC, "
                          "λ 19.537 m) per cycle and falls back to '7' (E5b, λ 9.768 m).");
            // ---- SD pre-reduce EWL: the statistic source must actually exist. ----
            // NetStation::mw_slip is constructed only for NON-pivot stations and only
            // when network.phase_leveling && (cycle_slip.mw_detect || turbo_edit).
            // Without it arcZdMean() is unreachable at EVERY station and the feature is
            // silently inert — exactly the armed-but-dead shape standing invariant #4
            // exists to catch, so say so loudly at load.
            if (network_config_.sd_ewl.enable) {
                const bool det = network_cycle_slip_mw_detect_
                                 || network_cycle_slip_turbo_edit_;
                if (!network_phase_leveling_ || !det) {
                    log->warn("Network: network.sd_ewl.enable is INERT — the MW arc "
                              "accumulator it reads (NetStation::mw_slip -> arcZdMean) is "
                              "constructed ONLY when network.phase_leveling && "
                              "(network.cycle_slip.mw_detect || turbo_edit); currently "
                              "phase_leveling={}, mw_detect={}, turbo_edit={}.  Zero SD "
                              "EWL rows will be built at any station.",
                              network_phase_leveling_,
                              network_cycle_slip_mw_detect_,
                              network_cycle_slip_turbo_edit_);
                }
            }
            if (mw.uwl_sib_gate && !(mw.uwl_dual_top && mw.uwl_enable))
                log->warn("Network: network.mw_wl.uwl_sib_gate is INERT without uwl_dual_top — "
                          "the sibling closure gate compares the untaken {7,8} rung against "
                          "the decided one, which only exists under the dual tier.");
            // ---- v2: per-family lane pair groups contract. ----
            if (mw.uwl_family_pairs
                && !(mw.uwl_enable && network_config_.lane_carry.enable))
                log->warn("Network: network.mw_wl.uwl_family_pairs is INERT without uwl_enable "
                          "AND network.lane_carry.enable — the admissible-pair census extends "
                          "the JOINT pair census (uwl) and the family groups are lane-carry "
                          "pair groups.");
            // ---- Per-sample bridge de-bias (60-ep age-bias fix) contract. ----
            if (mw.bridge_sample_debias && !mw.ewl_enable)
                log->warn("Network: network.mw_wl.bridge_sample_debias is INERT without "
                          "ewl_enable — it changes how the WL-2 GF bridge accumulates.");
            if (mw.bridge_sample_debias && mw.ewl_enable)
                log->info("Network bridge PER-SAMPLE iono de-bias ON (60-ep age-bias fix) — "
                          "bridge samples fold as GF_k − C_iono·ı̃(t_k) (estimable-iono, "
                          "one-epoch-lagged push; Σc·ı̃ over a closed cycle = ∇ΔI exactly, "
                          "Odijk 2016 Table 8); the read-time de-bias is disabled.  The "
                          "decision statistic is stationary at arbitrary arc age.");
            if (mw.uwl_enable && mw.uwl_mid_enable)
                log->info("Network UWL-1 E6 MID HOP ON — the Galileo WL determination chains "
                          "top({}) → (E6,E5a) λ 2.931 m (rounded mid integer) → WL λ 0.751 m, "
                          "cutting each bridge hop's λ amplification ~3.3×; the direct hop "
                          "remains the per-cycle fallback + cross-check, MW(6,5) is the mid "
                          "closure.  Requires frequency_mode covering Galileo E6.",
                          mw.uwl_gal_top_band == '8' ? "E5-AltBOC, 19.537 m" : "E5b, 9.768 m");
            if (mw.uwl_enable)
                log->info("Network UWL-1 top tier ON (M5 card UWL-1) — per-constellation lane-tier "
                          "table armed: GPS EWL (L2,L5) λ 5.861 m; Galileo UWL ({},E5a) λ {:.3f} m; "
                          "BDS-3 deferred (clock divergence), GLONASS excluded (FDMA).  Cold fix "
                          "{} (σ_apriori={} m, λ floor={} m).  Requires frequency_mode covering "
                          "the tier bands (Galileo E5a+{}).",
                          mw.uwl_dual_top ? "E5-AltBOC pref | E5b fallback"
                              : mw.uwl_gal_top_band == '8' ? "E5-AltBOC" : "E5b",
                          mw.uwl_dual_top ? 19.537
                              : mw.uwl_gal_top_band == '8' ? 19.537 : 9.768,
                          mw.uwl_cold_fix ? "ON" : "off",
                          mw.uwl_cold_sigma_m, mw.uwl_cold_min_lambda_m,
                          mw.uwl_dual_top ? "E5b+E5(AltBOC)"
                              : mw.uwl_gal_top_band == '8' ? "E5(AltBOC)" : "E5b");
            if (mw.uwl_enable && mw.uwl_dual_top)
                log->info("Network UWL-1 DUAL-TOP tier ON (increment 5) — Galileo prefers the "
                          "(E5-AltBOC,E5a) λ 19.537 m rung per CYCLE where band 8 is tracked "
                          "across every arc, falls back to (E5b,E5a) λ 9.768 m otherwise; both "
                          "{{7,8}} uncarried; the untaken sibling is a closure statistic "
                          "(2·N(8,5) = N(7,5)) — hard gate {}.",
                          mw.uwl_sib_gate ? "ON (0.5+3σ_sib)" : "OFF (measure-only)");
            if (mw.uwl_family_pairs && mw.uwl_enable && network_config_.lane_carry.enable)
                log->info("Network UWL-1 FAMILY PAIR GROUPS ON (v2 census recovery) — minority "
                          "receiver-family primary pairs with joint-census count ≥ {} join "
                          "their OWN lane pair groups (latched for the run); majority-pair "
                          "steering unchanged.  Recovers the skip_basis population the single-"
                          "pair census left per-band.",
                          mw.uwl_family_min_arcs);
            // ---- M5 card WL-3: lane-coordinate carry (SPEC_lane_carry_card.md). ----
            auto& lc = network_config_.lane_carry;
            if (lc.enable && (!network_carry_invariant_ || !mw.ewl_enable)) {
                log->error("Network: network.lane_carry.enable requires network.carry_invariant "
                           "AND network.mw_wl.ewl_enable — the lane-coordinate carry restructures "
                           "the invariant carry's pair groups and consumes the WL-2 MW/bridge "
                           "statistics as its lane measurements.  Enable both or disable "
                           "lane_carry.");
                return false;
            }
            if (lc.enable && mw.wl_inject_enable)
                log->warn("Network: network.lane_carry SUPERSEDES network.mw_wl.wl_inject_enable "
                          "at runtime (SPEC_lane_carry_card.md §3.3) — the WL-2 hold/inject walk "
                          "is replaced by continuous lane increments + fold-on-change fixing; "
                          "injectCarriedChord remains only as the pair-block fallback.  Disable "
                          "wl_inject_enable for a clean lane-carry shape.");
            if (lc.enable) {
                const bool equivalence_shape = lc.q_wl_scale == 1.0
                                            && lc.sigma_wl_fresh_cyc <= 0.0
                                            && !lc.lane_meas && !lc.bridge_meas;
                log->info("Network WL-3 lane-coordinate carry ON (M5 card WL-3) — primary-pair "
                          "chords carried as jointly-covarianced (band-1, band-2) blocks; shape = "
                          "{}.  q_wl_scale={}, sigma_wl_fresh={} cyc, lane_meas={}, bridge_meas={}, "
                          "fold_blocked={}.",
                          equivalence_shape
                              ? "EQUIVALENCE (SPEC §6.1: split per-band emission, no lane "
                                "features — the structural-parity gate; ≤1e-9 equivalence holds "
                                "in the unit harness, field runs diverge benignly via cycle-path-"
                                "dependent membership, see SPEC_lane_carry_card.md §10 deviation 5)"
                              : "LANE (joint 2K blocks, diffuse lane birth, per-epoch MW/bridge "
                                "increments, per-decided-epoch idempotent FFR lane re-pin)",
                          lc.q_wl_scale, lc.sigma_wl_fresh_cyc,
                          lc.lane_meas ? "true" : "false",
                          lc.bridge_meas ? "true" : "false",
                          lc.fold_blocked ? "true (LF-1 INC-1: O(K) sparse SET + hoisted symmetrize; "
                                            "EQUIVALENCE class, not bit-identical)" : "false");
                if (lc.skip_pinned_increments)
                    log->info("Network WL-3 lane-fold LF-1 INC-2: pinned-cycle increment skip ON "
                              "(within_n={} epochs) — committed-decided chords skip MW/bridge "
                              "increments; any slip/D5/re-tap resumes them.  EQUIVALENCE class.",
                              lc.skip_pinned_within_n);
                if (lc.parallel_components)
                    log->info("Network WL-3 lane-fold LF-1 INC-4: component-parallel MW fold ON "
                              "(one joint block per thread; needs parallel_stations).  "
                              "BYTE-IDENTICAL to serial.");
            }
        }

        return true;
    }

    // =================================================================
    //  network subsection helper: validateNetworkPhaseDatum_
    // =================================================================
    /**
     * @brief Phase-bias gauge selection report (eq34 / ensemble / prioritized) + its mutexes.
     *
     * One if / else-if chain over the three realizations, so exactly one is reported; also
     * carries the eq34-under-carry_invariant INERT warnings and the PB-filter inert note.
     *
     * @param log   The "gnss_config" logger.
     * @return @c false on a fatal misconfiguration (aborts @c loadFromJson).
     */
    bool GnssConfig::validateNetworkPhaseDatum_(
                                   const std::shared_ptr<spdlog::logger>& log) {
        if (network_phase_datum_eq34_) {
            // Card 3 mutual exclusion (mirror the clock-gauge fail-loud at
            // network_clock_datum_broadcast_anchored above): eq34 and the
            // ensemble are two DIFFERENT realizations of the same phase-bias
            // gauge; neither subsumes the other, so refuse to load both.
            if (network_phase_datum_ensemble_) {
                log->error("Network: network.phase_datum_eq34 and network.phase_datum_ensemble "
                           "are mutually exclusive realizations of the SAME phase-bias gauge "
                           "(single-receiver chain vs sum-to-zero over receivers).  Pick one.");
                return false;
            }
            log->info("Network producer: Eq-34 SINGLE-RECEIVER-CHAIN phase-bias datum ON (Card 3) — "
                      "each (constellation,signal) connected component gets a per-(receiver,system,"
                      "signal) RPB split state δ_{{r,σ}}, ONE datum-receiver RPB pin, and a "
                      "spanning-tree of ambiguity pins over the bipartite (receiver × satellite) "
                      "observation graph.  The emitted between-satellite phase-bias reference then "
                      "stays integer-consistent across ALL satellites (each chain hop contributes "
                      "only an integer) while the frame stays full-rank at global scale.  Reuses the "
                      "prioritized SPOF-cure machinery but replaces the anchor election with a chain "
                      "election; SUPERSEDES network.phase_datum_prioritized.  pin-round={}, "
                      "seed-round={}, rpb-prior sigma={} m (0=off).",
                      network_phase_datum_eq34_pin_round_ ? "true" : "false",
                      network_phase_datum_eq34_seed_round_ ? "true" : "false",
                      network_phase_datum_eq34_rpb_prior_sigma_m_);
            // INC-2 gets its own line rather than a widened summary string: an OFF
            // run's log (and the run-summary hash the V1 byte-identity rung reads)
            // must be untouched by this card.
            if (network_phase_datum_eq34_seed_round_rehost_frame_)
                log->info("Network producer: eq34 SEED-ROUND INC-2 (frame-continuous "
                          "re-host) ON — a tree pin that moves to a NEW host takes its "
                          "frame step from that host's own ambiguity state and "
                          "S-transforms the carried values, instead of an unknown step "
                          "that re-references the (sat,signal) and marginalizes its "
                          "carried phase-bias prior for the epoch.  A/B lever "
                          "(docs/SPEC_eq34_seed_round.md §8.5); the run-end seed-round "
                          "block reports framed vs cold re-hosts.");
            if (network_phase_datum_prioritized_)
                log->info("Network producer: network.phase_datum_eq34 SUPERSEDES "
                          "network.phase_datum_prioritized (the ensemble precedent) — the anchor "
                          "election is replaced by the chain election.");
            if (network_phase_datum_hold_sigma_m_ > 0.0)
                log->warn("Network: network.phase_datum_hold_sigma_m = {} is INERT under "
                          "network.phase_datum_eq34 — the hold is an ensemble-only stopgap; the "
                          "chain datum's frame is pinned hard (one RPB pin + tree ambiguity pins), "
                          "so it needs no temporal hold.", network_phase_datum_hold_sigma_m_);
            if (!network_station_pd_pin_aware_)
                log->warn("Network: RECOMMEND network.station_pd_pin_aware = true with "
                          "network.phase_datum_eq34 — hub stations (high spanning-tree degree) "
                          "carry many 1e10 ambiguity pins, and the median-|pivot| PD guard keeps a "
                          "healthy hub from being ejected as non-PD (the TWTF/VOIM relative-rank "
                          "lesson).");
            if (network_phase_datum_min_stations_ > 1)
                log->info("Network producer: eq34 per-(sat,signal) coverage gate ON — a shared "
                          "phase-bias column enters the graph only when >= {} present stations "
                          "track it (gated keys keep legacy per-station lumping).",
                          network_phase_datum_min_stations_);
            if (network_phase_datum_eq34_longevity_election_)
                log->info("Network producer: eq34 LONGEVITY-AWARE tree growth ON (I4) — new "
                          "spanning-tree edges are grown from r1 by arc longevity (rising arcs "
                          "first, then high elevation; high-coverage receivers first) instead of "
                          "lexical order, so tree edges outlive the epoch and b_s re-references "
                          "fewer times (spec D2 step 3).  Incumbent-edge seed unchanged.");
            if (network_phase_datum_eq34_extend_only_)
                log->info("Network producer: eq34 EXTEND-ONLY membership ON (I2, de Jonge §3.5) — "
                          "a tree edge's sticky identity is retained across a momentary row gap "
                          "while its phase-leveling carry is alive (age-stamped to "
                          "phase_leveling_max_gap_s = {} s), so a returning arc re-pins to its "
                          "pre-gap integer (step ~0) instead of re-leveling from a first-sighting "
                          "0.  Tree growth unchanged (live arcs only); watch the gap-repin counter "
                          "in the run-end producer summary.",
                          network_phase_leveling_max_gap_s_);
            if (network_carry_invariant_)
                log->info("Network producer: Card 4 (M1) INVARIANT CARRY ON — the DD cycle-basis "
                          "z-store time-links the S-system-invariant double differences (chords of "
                          "the eq34 spanning tree, q_z={} m²/s) through the producer solve, "
                          "SUPERSEDING the PhaseBias shared carry + phase-leveling local carry + RPB "
                          "carry + eq34 pin-round/shift/reref hygiene for phase keys (GLONASS "
                          "excluded, FDMA).  The per-epoch datum is re-realized freely; basis churn "
                          "is a non-event by construction.  emit_rebase={}, honesty={}.",
                          network_carry_invariant_qz_,
                          network_carry_invariant_emit_rebase_ ? "true" : "false",
                          network_carry_invariant_honesty_ ? "true" : "false");
            if (network_carry_invariant_innov_gate_) {
                log->info("Network producer: Card 4 (M1) D5 FINE-SLIP INNOVATION GATE ON — after each "
                          "accepted solve, every carried DD's standardized innovation "
                          "|z_free−Gᵀx_s−ẑ|/√(M⁻¹−GᵀΣG) is tested at k={}; a chord above threshold is "
                          "re-tapped FRESH (reset, not repair) and its arc marked slipped for the next "
                          "epoch (the C3c membership-exclusion path).  The Card-4-native replacement "
                          "for the RAIM fine-slip layer the invariant bypass empties; watch "
                          "innov_flags/retaps in the run-end summary.", network_carry_invariant_innov_k_);
                if (network_carry_invariant_innov_calibrate_)
                    log->info("Network producer: Card 4 (M1) D5 v2 LS-VCE CALIBRATION ON — the gate "
                              "standardizes by the empirical innovation inflation σ̂₀² (robust geo-mean "
                              "of the standardized innovations, the M2 NetworkCarryVce instrument; "
                              "forgetting half-life {} epochs) instead of the raw formal Cov(v) that the "
                              "M2 monitor measured 10-30× optimistic — so k={} is now a TRUE Nσ gate "
                              "that auto-adapts per regime (one k for both em10/em25).  σ̂₀ is reported "
                              "in the run-end summary.", network_carry_invariant_innov_calibrate_window_,
                              network_carry_invariant_innov_k_);
                if (network_carry_invariant_innov_k_ < 3.0)
                    log->warn("Network: network.carry_invariant_innov_k = {} is BELOW 3 — a low "
                              "threshold storms the carry with false-alarm refloats (each shreds the "
                              "accumulated DD information).  Recommend k >= 4.",
                              network_carry_invariant_innov_k_);
                if (network_carry_invariant_innov_refold_)
                    log->warn("Network: network.carry_invariant_innov_refold (D5 v2 within-epoch refold) "
                              "is parsed for the config contract but NOT YET IMPLEMENTED — the v1 "
                              "reset-only path is active (the one already-solved epoch retains bounded "
                              "contamination; q_z is the backstop).");
            } else if (network_carry_invariant_innov_refold_) {
                log->warn("Network: network.carry_invariant_innov_refold is INERT without "
                          "network.carry_invariant_innov_gate (it routes the gate's flagged set).");
            }
            if (network_emit_pb_filter_) {
                if (network_carry_invariant_emit_rebase_)
                    log->info("Network producer: PB EMISSION-CONTINUITY FILTER ON (M5 prerequisite) — "
                              "a slip-gated per-key scalar KF on the POST-REBASE emitted phase biases "
                              "(q={} m²/s) smooths the per-epoch estimation noise the re-base leaves, and "
                              "a self-tuning k·σ̂_step jump gate (k={}, seed σ̂_step {:.3f} m, window {} ep) "
                              "resets ONLY on a reference-path slip, a genuine event, or a >{:.0f} s gap — "
                              "clearing the ~200/epoch threshold-in-noise discontinuity floor.  "
                              "Outputs-only + S-transparent (the solve, user DDs and dd-frac grading are "
                              "byte-unchanged); the emitter jump test stays the SOLE disc authority.  "
                              "emit_posterior_sigma={}.",
                              network_emit_pb_filter_q_, network_emit_pb_filter_jump_k_,
                              network_emit_pb_filter_meas_sigma_m_,
                              network_emit_pb_filter_window_epochs_,
                              network_emit_pb_filter_max_gap_s_,
                              network_emit_pb_filter_emit_sigma_ ? "true" : "false");
                else
                    log->warn("Network: network.emit_pb_filter is INERT without "
                              "network.carry_invariant_emit_rebase — it filters the RE-BASED emission "
                              "frame, so the re-base must run for the filter to fire.");
                if (network_emit_pb_filter_jump_k_ < 2.0)
                    log->warn("Network: network.emit_pb_filter_jump_k = {} is BELOW 2 — the gate sits "
                              "inside the step noise and leaves the discontinuity floor uncleared.",
                              network_emit_pb_filter_jump_k_);
                if (network_emit_pb_filter_gate_v2_ && network_carry_invariant_emit_rebase_)
                    log->info("Network producer: PB filter GATE v2 ON — data/model-driven reset "
                              "replacing the v1 per-cadence hand k/seed: (a) CENSORING-FREE σ̂_step feed "
                              "(innovation-gated rows feed too, killing the v1 low-σ̂ death-spiral), "
                              "(b) MODEL-SCALED gate |innov| > k·√(Ppred+R) so dt enters via the prior's "
                              "q·gap term and ONE k={:.1f} transfers 30 s ↔ 300 s, (c) seed σ̂_step "
                              "demoted to a numerical guard (floor ~1 mm).  OFF = v1 (k·σ̂_step, "
                              "smoothed-rows-only feed).  Retire any per-rate emit_pb_filter_jump_k / "
                              "_meas_sigma_m override once the 30 s+300 s A/B transfer (G5) passes.",
                              network_emit_pb_filter_jump_k_);
            }
            // Warn-INERT (spec D7): the eq34 frame-continuity hygiene is subsumed by the
            // invariant carry — the datum targets are plain 0 and continuity lives in the
            // carried DDs, so pin_round / carry_shift / rpb_q are forced off when armed.
            if (network_phase_datum_eq34_pin_round_)
                log->warn("Network: network.phase_datum_eq34_pin_round is INERT under "
                          "carry_invariant (per-epoch datum targets are 0; DD continuity is the carry).");
            if (network_phase_datum_eq34_carry_shift_)
                log->warn("Network: network.phase_datum_eq34_carry_shift is INERT under "
                          "carry_invariant (no carry S-transform — the datum is re-realized freely).");
            if (network_phase_datum_eq34_rpb_q_m2s_ > 0.0)
                log->warn("Network: network.phase_datum_eq34_rpb_q_m2s is INERT under carry_invariant "
                          "(the RPB re-estimates fresh; the DD carry is the phase time-link).");
        } else if (network_phase_datum_ensemble_) {
            log->info("Network producer: ANCHOR-FREE ENSEMBLE phase-bias datum ON (Fix A) — the "
                      "per-(sat,signal) phase-bias gauge is fixed by a sum-to-zero over the "
                      "tracking receivers' ambiguities (Σ_r zˢ_{{r,σ}}=0, kernel Schur-projected "
                      "fold), NOT a single-anchor pin.  No pivot, no handover (the emitted bias "
                      "stops re-referencing → phase accumulates), no single-anchor SPOF, and every "
                      "satellite referenced to the SAME receiver ensemble (the user's "
                      "between-satellite fractional bias cancels → DD ambiguity stays integer).  "
                      "Reuses the prioritized SPOF-cure machinery but elects no anchor; supersedes "
                      "network.phase_datum_prioritized.");
            if (network_phase_datum_min_stations_ > 1)
                log->info("Network producer: ensemble coverage gate ON — a shared phase-bias "
                          "column (and its gauge) is emitted only when >= {} present stations "
                          "track it.", network_phase_datum_min_stations_);
        } else if (network_phase_datum_prioritized_) {
            log->info("Network producer: prioritized phase-bias anchor ON — the per-(sat,signal) "
                      "phase-bias gauge is pinned at the highest-coverage receiver tracking each "
                      "satellite (pref elevation {:.1f} deg, sticky={}), decoupled from the single "
                      "clock-pivot station.  Cures the low-elevation gauge jump + the pivot-absent "
                      "skip; per-(sat,signal) anchor handovers are signalled to the consumer as "
                      "discontinuities.  Clock datum unchanged.",
                      network_phase_datum_pref_elev_deg_,
                      network_phase_datum_sticky_ ? "true" : "false");
            if (network_phase_datum_min_stations_ > 1)
                log->info("Network producer: anchored-union coverage gate ON — a shared "
                          "phase-bias column is registered/emitted only when >= {} present "
                          "stations track it with a usable ambiguity (thin columns are the "
                          "non-PD-ejection / one-station-re-level / split-leak class; their "
                          "phase data still serves the clock through the local blocks).",
                          network_phase_datum_min_stations_);
        } else if (network_phase_datum_min_stations_ > 1) {
            log->warn("Network: network.phase_datum_min_stations = {} is INERT — the anchored-"
                      "union coverage gate only applies under network.phase_datum_prioritized "
                      "(the legacy pivot datum has no union to gate).",
                      network_phase_datum_min_stations_);
        }

        // TWO-EDGE CONTINUOUS HANDOVER — INERT guards, then the arming line.
        // Follows the eq34-hygiene / pool-re-host precedent: an arm that cannot move
        // a byte is DISABLED here with the reason, rather than discovered after a
        // field run (standing invariant #4).
        if (network_phase_datum_handover_continuous_ && !network_phase_datum_prioritized_) {
            log->warn("Network (phase-datum handover): "
                      "network.phase_datum_handover_continuous is on but "
                      "network.phase_datum_prioritized is OFF — the legacy datum elects no "
                      "anchor at all (its shared set IS the clock-pivot station's own "
                      "phase-bias states), so there is no handover to make continuous.  On "
                      "that posture a pivot that loses a satellite DELETES the column "
                      "network-wide instead of handing it over, which is the more severe "
                      "form of the same defect.  DISABLED (no behaviour change).");
            network_phase_datum_handover_continuous_ = false;
        }
        if (network_phase_datum_handover_continuous_ && network_phase_datum_ensemble_) {
            log->warn("Network (phase-datum handover): "
                      "network.phase_datum_handover_continuous is on with the ENSEMBLE datum, "
                      "which elects no anchor and holds no pin (the gauge is the kernel "
                      "sum-to-zero fold), so there is nothing to hand over.  DISABLED.");
            network_phase_datum_handover_continuous_ = false;
        }
        if (network_phase_datum_handover_continuous_ && network_phase_datum_eq34_) {
            log->warn("Network (phase-datum handover): "
                      "network.phase_datum_handover_continuous is on with the eq34 CHAIN datum, "
                      "which owns its own pin-target policy (pin-round / seed-round) and would "
                      "fight this one for the same target.  DISABLED — the chain datum's "
                      "handover continuity is a separate card.");
            network_phase_datum_handover_continuous_ = false;
        }
        if (network_phase_datum_handover_continuous_)
            log->info("Network producer (PB-GAUGE HANDOVER, two-edge + R2 sticky target): an "
                      "anchor handover pins the INCOMING anchor at its own absolute ambiguity "
                      "estimate in the OUTGOING frame instead of at 0, and the target is "
                      "STICKY — re-applied every epoch of the successor's tenure — so the "
                      "gauge change is an S-transform whose frame persists (the pre-R2 "
                      "one-epoch deferral reverted to 0 at T+1 and measured null, "
                      "SPEC_pivot_visibility_gauge_defect §5.0/§5.09).  Both edges are "
                      "counted and traced (PBGAUGE rows); a successor with no ledger entry, "
                      "or one whose conditional variance exceeds "
                      "network.phase_datum_handover_max_var_m2 ({} m^2 — keep this TIGHT: the "
                      "inherited frame is preserved only to the ledger value's own error), "
                      "still pins at 0 and is counted as a COLD handover — an unavoidable "
                      "re-realization the consumer is TOLD about.  Ledger is harvested "
                      "post-solve and consumed one epoch later.",
                      network_phase_datum_handover_max_var_m2_);
        // The honest-σ-scale ceiling rides the same mechanism, so it inherits the same
        // INERT ladder: no continuous handover ⇒ no ledger value is ever tested.
        if (network_phase_datum_handover_max_var_ratio_ > 0.0) {
            if (!network_phase_datum_handover_continuous_)
                log->warn("Network (PB-GAUGE HANDOVER ratio ceiling): "
                          "network.phase_datum_handover_max_var_ratio is set but "
                          "network.phase_datum_handover_continuous is OFF (or was disarmed above) "
                          "— no ledger value is ever tested, so the ratio is INERT.");
            else
                log->info("Network producer (PB-GAUGE HANDOVER, honest-sigma-scale ceiling): a "
                          "ledger value is pinned only if its conditional variance is <= {} x the "
                          "arc's FIRST-STASH conditional variance (an unknown first-stash variance "
                          "is refused); the absolute {} m^2 ceiling stays as a rail.  Both refusals "
                          "are counted separately and every PBGAUGE handover row carries "
                          "var / var_first.",
                          network_phase_datum_handover_max_var_ratio_,
                          network_phase_datum_handover_max_var_m2_);
        }
        // COLD ANCHOR-FALLBACK (SPEC_event_harness §15): same INERT ladder — with no
        // continuous handover there is no ledger to screen candidates against, so the
        // candidacy pass has nothing to read and would elect by coverage rank exactly
        // as today.  Say so rather than let an armed knob read as active.
        if (network_phase_datum_candidacy_usable_) {
            if (!network_phase_datum_handover_continuous_)
                log->warn("Network (COLD ANCHOR-FALLBACK): "
                          "network.phase_datum_candidacy_usable is set but "
                          "network.phase_datum_handover_continuous is OFF (or was disarmed "
                          "above) — there is no handover ledger to screen candidates "
                          "against, so the candidacy pass is INERT.");
            else
                log->info("Network producer (COLD ANCHOR-FALLBACK): a handover's successor is "
                          "chosen from the coverage-ranked candidates whose CARRY IS USABLE "
                          "by the continuous branch's own predicate (finite value, 0 < var <= "
                          "{} m^2{}, not freed this epoch, arc age >= {} loop epochs — 0 or "
                          "unknown = mature), and only if NONE is usable does the election "
                          "fall through to today's take-the-first-tracker sweeps.  So "
                          "handover_cold then counts exactly the UNAVOIDABLE "
                          "re-realizations; candidacy_rank_changed counts the elections this "
                          "moved.",
                          network_phase_datum_handover_max_var_m2_,
                          network_phase_datum_handover_max_var_ratio_ > 0.0
                              ? ", ratio ceiling included" : "",
                          network_phase_datum_candidacy_min_age_ep_);
            if (network_phase_datum_candidacy_satellite_)
                log->info("Network producer (COLD ANCHOR-FALLBACK, SATELLITE clause): a "
                          "satellite's families that hand over in the SAME epoch take ONE "
                          "successor (the first coverage-ranked candidate usable for the most "
                          "of them; a family it is not usable for is pinned THERE through the "
                          "cold path — a frame step, not a split), and a PARTIAL handover "
                          "prefers the station already anchoring the satellite's other "
                          "families.  Iteration 1's per-family screen bought producer quality "
                          "with split references (0.0 -> 0.9 mean / 3 max) and cost the "
                          "consumer 49 % (SPEC_event_harness §15.6).");
            else
                log->warn("Network producer (COLD ANCHOR-FALLBACK): "
                          "network.phase_datum_candidacy_satellite is OFF — this is "
                          "ITERATION 1, the per-FAMILY screen, whose storm arm measured "
                          "split-referenced satellites 0.0 -> 0.9 / 3 and a +49 % consumer "
                          "median.  Arm it only for the single-knob A/B.");
        }
        // READMIT continuity (E2 seed-first re-admission + the block re-coupling edge).
        if (network_readmit_seed_horizon_s_ > 0.0)
            log->info("Network producer (READMIT seed-first re-admission, E2): a departed "
                      "satellite's D2 clock + code-bias carry is PARKED for up to {} s and seeds "
                      "its re-registered columns at var + q*gap — the Q3 anchor folds ANCHORED "
                      "over the true gap up to that bound and its NIS guard is the "
                      "mark-if-moved.  The phase-bias family is NOT seeded (its frame is "
                      "re-realized at the cold re-pin).{}",
                      network_readmit_seed_horizon_s_,
                      network_clock_model_
                          ? "  ⚠ clock_model is ON: the clock rides the oscillator, not the D2 "
                            "carry, so only code-bias columns can be seeded."
                          : "");
        // Card CPA, the ARMED line.  There is no INERT ladder to run here: the rule
        // has no prerequisite knob, and the one way it can be inert (no station
        // latched a canonical code+phase pair on band digits 1 and 2) is a property
        // of the DATA, which only the run-end counters can report.
        if (network_canonical_pair_admission_)
            log->info("Network producer (card CPA, CANONICAL-PAIR ADMISSION): ON with a "
                      "tolerance of {} processed epoch(s): a (station, satellite) record "
                      "enters the network path only while the station holds its canonical "
                      "PHASE and CODE on band digits 1 and 2 (that station's own "
                      "decideConstellationCanonicals latch), or the last good epoch is "
                      "within the tolerance; it is WITHHELD before the first good epoch and "
                      "beyond it, and a withheld record is absent for everything downstream "
                      "exactly as if the RINEX record were missing (so the row-level gap "
                      "re-entry tier reads its return as a RE-ENTRY).  A constellation with "
                      "no canonical pair decided on those digits is left untouched, and the "
                      "held-out closed-loop consumer is never screened.  Both edges are "
                      "ADMISSION/canonical_pair events; read the run-end withheld counters "
                      "before crediting the rule.{}",
                      network_canonical_pair_keep_epochs_,
                      network_canonical_pair_keep_epochs_ == 0
                          ? "  Tolerance 0: only the epochs that hold the pair are admitted "
                            "It is measured WORSE than 3 on the field TRIPLE bed (288 vs 461 "
                            "epochs below a decimetre), where a satellite without its band-2 "
                            "phase is still a two-band satellite."
                          : "");
        if (network_readmit_coupling_marg_ep_ > 0)
            log->info("Network producer (READMIT re-coupling edge): for {} epoch(s) after a "
                      "satellite's clock column returns from a >= 3-epoch absence, every Card-1 "
                      "coupling term on its shared columns takes the DEPARTED route (block/row "
                      "re-coupling edge).{}",
                      network_readmit_coupling_marg_ep_,
                      network_carry_conditional_coupling_
                          ? ""
                          : "  ⚠ carry_conditional_coupling is OFF: no coupling rows exist, the "
                            "knob is INERT (invariant #4: read the run-end 'diverted' counter).");
        // QUEUE item 1(d) — same INERT-guard ladder as the handover knob above,
        // for the same reasons: no anchors ⇒ no anchor handovers ⇒ nothing to
        // re-reference.  The eq34 guard is FIRST, deliberately unlike the ladder
        // above: an eq34-only config never sets phase_datum_prioritized, so a
        // prioritized-first ladder would disarm with the factually wrong "legacy
        // datum elects no anchor" diagnostic (eq34 elects a CHAIN datum) and the
        // accurate message would be unreachable.
        if (network_phase_anchor_carry_reref_ && network_phase_datum_eq34_) {
            log->warn("Network (phase-anchor carry reref): "
                      "network.phase_anchor_carry_reref is on with the eq34 CHAIN datum, "
                      "which already owns the eq34_reref carry hygiene this knob mirrors.  "
                      "DISABLED — arming both would double-treat the same edge.");
            network_phase_anchor_carry_reref_ = false;
        }
        if (network_phase_anchor_carry_reref_ && network_phase_datum_ensemble_) {
            log->warn("Network (phase-anchor carry reref): "
                      "network.phase_anchor_carry_reref is on with the ENSEMBLE datum, "
                      "which elects no anchor and holds no pin, so no handover exists to "
                      "trigger the re-reference.  DISABLED.");
            network_phase_anchor_carry_reref_ = false;
        }
        if (network_phase_anchor_carry_reref_ && !network_phase_datum_prioritized_) {
            log->warn("Network (phase-anchor carry reref): "
                      "network.phase_anchor_carry_reref is on but "
                      "network.phase_datum_prioritized is OFF — the legacy datum elects no "
                      "anchor, so there is no PHASE_ANCHOR handover whose carry could be "
                      "re-referenced (that posture DELETES the column network-wide instead).  "
                      "DISABLED (no behaviour change).");
            network_phase_anchor_carry_reref_ = false;
        }
        // The continuous-handover PAIRING is INCOHERENT with the reref and is
        // COMPOSED as of R2 (SPEC §5.09).  The 2026-08-14 review disarmed this
        // pairing because the ledger pin was a one-epoch deferral: the frame
        // step landed at T+1 (§5.07.3) while the reref mark covered T, so the
        // pair marginalized the valid fold and missed the stale one.  The R2
        // sticky target removes the T+1 revert — a continuous handover now
        // realizes the inherited frame for the successor's whole tenure — and
        // the election marks ONLY cold re-realizations, so each mechanism
        // treats exactly its own edge: continuous handovers keep frame AND
        // carries; cold handovers mark, marginalize and erase as validated in
        // §5.08.3.  This composition is what stops the reref's arc truncation
        // (582/621 cold handovers erased 10 236 leveling carries on a 12 h
        // zero-slip fixture, SPEC §5.09).
        if (network_phase_anchor_carry_reref_
            && network_phase_datum_handover_continuous_)
            log->info("Network (phase-anchor carry reref + continuous handover, R2 "
                      "composition): CONTINUOUS handovers inherit the outgoing frame via a "
                      "sticky ledger target and keep the family's carries; COLD handovers "
                      "(no ledger value, or conditional variance above "
                      "network.phase_datum_handover_max_var_m2) re-realize honestly and are "
                      "marked for the one-epoch carry marginalization + leveling-carry "
                      "erase.  Keep the variance ceiling TIGHT — the preserved frame is "
                      "only as good as the ledger value it pins.");
        if (network_phase_anchor_carry_reref_)
            log->info("Network producer (PHASE_ANCHOR carry re-reference, QUEUE 1(d)): at an "
                      "anchor handover the (sat,signal) family's shared phase-bias carry is "
                      "MARGINALIZED for that one epoch and its phase-leveling local carries "
                      "are erased at every station — the retired-frame prior is not folded "
                      "against the re-realized datum, so the frame step lands on the free "
                      "phase bias instead of being absorbed by the satellite clock "
                      "(SPEC_pivot_visibility_gauge_defect §5.07: 41-sigma clock innovations, "
                      "q_pb:q_clock = 1e10:1).  Families and erased carries are counted at "
                      "run end.");
        // Card EV7 §6 — the gate's DROP EDGE.  Two dependencies, both hard: the
        // gate has to be ON for an edge to exist at all, and the erase RIDES the
        // re-reference set, which only phase_anchor_carry_reref populates.  Report
        // each INERT case rather than silently flipping the knob off: a run whose
        // arm named the knob and got nothing is exactly the class of wasted arm
        // this project keeps paying for.  The edges are DETECTED and counted in
        // every posture, so an OFF (or inert) run still reports the class size.
        if (network_phase_datum_gate_drop_erase_
            && network_phase_datum_min_stations_ <= 1)
            log->warn("Network (EV7 gate-drop erase): network.phase_datum_gate_drop_erase is on "
                      "but network.phase_datum_min_stations = 1, so the coverage gate excludes "
                      "nothing and no drop edge can exist.  INERT.");
        if (network_phase_datum_gate_drop_erase_ && !network_phase_anchor_carry_reref_)
            log->warn("Network (EV7 gate-drop erase): network.phase_datum_gate_drop_erase is on "
                      "but network.phase_anchor_carry_reref is OFF — the erase rides that "
                      "knob's re-reference set, which is never populated here.  INERT: the "
                      "drop edges are still counted and traced, but no carry is erased.");
        if (network_phase_datum_gate_drop_erase_ && network_phase_anchor_carry_reref_
            && network_phase_datum_min_stations_ > 1)
            log->info("Network producer (EV7 gate-drop erase, card EV7 §6): when the coverage "
                      "gate (< {} trackers) removes a (sat,signal) phase-bias column that was "
                      "ANCHORED last epoch, the family joins this epoch's re-reference set and "
                      "every station's CARRIED ambiguity of it is erased before assembly — the "
                      "response the drop edge has never had (standing invariant #7; the return "
                      "edge already re-births at a zero pin).  Without it the surviving "
                      "station's carried float absorbs the whole removed bias in one epoch "
                      "against a centimetre prior (+15.829 m measured) and the satellite clocks "
                      "absorb what the prior will not yield.  Edges, edges handled and arcs "
                      "erased are counted at run end.",
                      network_phase_datum_min_stations_);
        // Card UPO: the armed line.  There is no INERT case (the reduction is
        // unconditional at setup and self-reports per station and in a summary), so a
        // single info line naming the frame change is what a reader of the config log
        // needs to see that this run's roster is NOT at the published coordinates.
        if (network_station_marker_to_arp_)
            log->info("Network producer (card UPO): network.station_marker_to_arp is ON — every "
                      "roster station's configured position_ecef is treated as a MONUMENT "
                      "coordinate and reduced to its antenna reference point at setup with that "
                      "station's RINEX ANTENNA: DELTA H/E/N (Coordinates::markerToArp, the same "
                      "helper the held-out consumer's truth uses).  The observations refer to the "
                      "antenna and the ATX phase-centre model is ARP-relative, so a station held "
                      "at the monument misfits every range by H*sin(elev); its zenith wet delay "
                      "absorbs about a quarter of H and the rest reaches the satellite clocks.  "
                      "Per-station lines and a summary follow at station build; a station whose "
                      "header carries no eccentricity record is left unchanged and warned.");
        // Card SID phase 1: the armed line.  Trace-class and byte-identical on the
        // solve in BOTH states, so it carries no INERT case; what a reader needs is
        // that this run pays the observatory's cost and writes its extra artifacts.
        if (network_signal_roster_)
            log->info("Network producer (card SID phase 1): network.signal_roster is ON — the "
                      "per-station SIGNAL ROSTER (tracked / modelled / companion / excluded per "
                      "station, constellation and band digit, reproduced from the canonical latch) "
                      "and the ARC LEDGER (Absent / Tracking / Gap / Reentered / Dead per "
                      "(station, satellite, signal)) are built once per epoch and read by NOTHING "
                      "but the disagreement counters and the trace, so the solve, the wire and "
                      "every graded trace are byte-identical against an unarmed run.  Extra "
                      "artifacts: logs/network_arc_ledger.csv (one row per transition), the ARC "
                      "event tier in network_events.csv (holdoff-throttled), and the run-end "
                      "disagreement table.");
        // Card SID phase 2: the tracker reads the roster.  REFUSED without the
        // observatory, because the reader would then have no roster to read and a
        // silent fallback to the canonical latch would make an armed run look like it
        // had exercised the reader (standing invariant 4: the counter has to mean
        // what its name says).
        if (network_roster_tracker_ && !network_signal_roster_) {
            log->error("Network producer (card SID phase 2): network.roster_tracker is armed "
                       "WITHOUT network.signal_roster, so there is no roster to read.  "
                       "DISARMED.  Arm network.signal_roster (the phase-1 observatory, "
                       "byte-identical on the solve) alongside it.");
            network_roster_tracker_ = false;
        }
        if (network_roster_tracker_)
            log->info("Network producer (card SID phase 2): network.roster_tracker is ON — the "
                      "arc-level slip tracker (networkComputeRawObservables) names each band by "
                      "the ROSTER's modelled_phase for that (station, constellation, band) when "
                      "that phase is reported with its matching code this epoch, and falls back "
                      "to the lexically lowest raw phase with a matching code only where the "
                      "roster names none.  The per-signal tier of card GRS is untouched (it keys "
                      "on the signal).  The roster is built upstream of computeModels, so the "
                      "FIRST epoch of a constellation has no modelled role yet and the reader "
                      "falls back to that station's latch there; the two fall-back counts are "
                      "reported at run end.");
        if (network_roster_tracker_ && network_config_.slip_qc.tracker_modeled_first)
            log->info("Network producer (card SID phase 2): network.roster_tracker and "
                      "network.cycle_slip.tracker_modeled_first are BOTH armed.  The ROSTER READ "
                      "SUPERSEDES the latch lookup: the tracker's band signal comes from the "
                      "roster's modelled_phase, and card GRK's latch lookup is not bound at all.  "
                      "The result is identical either way (the roster's modelled role IS that "
                      "latch, phase 1's roster_latch_disagree counter at its bar of 0), so this "
                      "is a notice and not a conflict.");
        // Card SID phase 3: the response keys on the ledger.  REFUSED without BOTH
        // predecessors — without the observatory there is no admitted lifecycle to
        // read, and without the roster read the tracker's rows would still name a
        // band by a different rule than the population does, which is card GRK's
        // defect re-introduced one layer up.
        if (network_ledger_reentry_
            && (!network_signal_roster_ || !network_roster_tracker_)) {
            log->error("Network producer (card SID phase 3): network.ledger_reentry is armed "
                       "WITHOUT network.signal_roster{}, so there is {}.  DISARMED.  Arm the "
                       "phase-1 observatory and the phase-2 roster read alongside it.",
                       network_roster_tracker_ ? "" : " and network.roster_tracker",
                       network_signal_roster_
                           ? "no roster read behind the tracker's band names"
                           : "no arc ledger to take the population from");
            network_ledger_reentry_ = false;
        }
        if (network_ledger_reentry_ && !network_config_.slip_qc.gap_reentry) {
            log->error("Network producer (card SID phase 3): network.ledger_reentry is armed "
                       "WITHOUT network.cycle_slip.gap_reentry_relevel, so there is no gap "
                       "re-entry RESPONSE for the ledger to drive.  DISARMED.  Arm the "
                       "row-level response (the card G drain) alongside it.");
            network_ledger_reentry_ = false;
        }
        if (network_ledger_reentry_)
            log->info("Network producer (card SID phase 3): network.ledger_reentry is ON — the "
                      "gap RE-ENTRY RESPONSE takes its population from the arc ledger's ADMITTED "
                      "lifecycle (every arc whose admitted state goes Gap to Reentered at this "
                      "epoch, whose signal the roster calls Modelled, and whose gap is inside "
                      "network.phase_leveling_max_gap_s), and looks the raw tiers' verdict up per "
                      "arc: the row-level tier's resumption verdict for the arc's satellite, the "
                      "per-signal tier's for the arc's signal.  The raw tiers keep running and "
                      "their detections, verdicts and traces are unchanged; they no longer DRIVE "
                      "the response.  An arc no raw tier saw this epoch has NO verdict and is "
                      "handled by network.ledger_reentry_no_verdict = {}.  ⚠ SOLVE-AFFECTING: "
                      "this changes which carried levels survive a break, so it is graded as a "
                      "behaviour change and it is FINGERPRINTED (fresh convergence only).",
                      network_ledger_reentry_no_verdict_ == LedgerReentryNoVerdict::Refloat
                          ? "refloat (a no-verdict re-entry is treated as UNMEASURABLE, which "
                            "network.cycle_slip.gap_reentry_relevel_unmeasurable then decides)"
                          : "bridge (a no-verdict re-entry is left to the PASS-B inject, as "
                            "today, but COUNTED and EVENTED as ARC arc_reentry verdict=none)");
        // Card SID phase 4a: the reason-mask writers and the anchor observatory.
        // REFUSED without the phase-1 observatory: there is no ledger to fold a
        // reason into and no arc state for the election read to ask about, and a
        // silent no-op would make an armed run look like it had measured something.
        if (network_ledger_screens_ && !network_signal_roster_) {
            log->error("Network producer (card SID phase 4a): network.ledger_screens is armed "
                       "WITHOUT network.signal_roster, so there is no arc ledger to fold a "
                       "reason into and no arc state for the anchor read.  DISARMED.  Arm "
                       "network.signal_roster (the phase-1 observatory, byte-identical on the "
                       "solve) alongside it.");
            network_ledger_screens_ = false;
        }
        if (network_ledger_screens_)
            log->info("Network producer (card SID phase 4a): network.ledger_screens is ON — every "
                      "screen that removes a modelled row, a satellite or a station (the "
                      "elevation mask, the model's own no-product / no-pair refusals, the network "
                      "signal allow list, the measurement-admission and DW withholds, the readmit "
                      "park, the coverage gate, the gross-LOS screen and the slip refloats) "
                      "records the arcs it removed and its own reason bit into a per-epoch "
                      "buffer, and the roster's audit pass folds that buffer into the ledger, so "
                      "an admitted gap carries the reason(s) that opened it and one with no "
                      "writer carries 'unknown'.  A gross-LOS DE-WEIGHT is recorded as 'deweight' "
                      "and is never counted as a gap.  The anchor election is READ against the "
                      "ledger at the same time: a first sighting whose family has arcs in "
                      "Reentered or Gap is counted and evented, each coverage-gate drop edge "
                      "reports its arcs' states and the generation test at the family's return, "
                      "and anchor_arc_age is compared with the ledger's own age.  OBSERVATORY: "
                      "nothing in the solve reads any of it, so the solve, the wire and every "
                      "graded trace are byte-identical against an unarmed run.");
        // Card SID phase 4b: the election acts on the read.  REFUSED without 4a,
        // whose buffer carries the drop-edge memory and the generation test the
        // restore decides on.
        if (network_ledger_anchor_ && !network_ledger_screens_) {
            log->error("Network producer (card SID phase 4b): network.ledger_anchor is armed "
                       "WITHOUT network.ledger_screens, so the election has no drop-edge memory "
                       "and no generation test to decide a restore on.  DISARMED.  Arm "
                       "network.ledger_screens (the phase-4a observatory) alongside it.");
            network_ledger_anchor_ = false;
        }
        if (network_ledger_anchor_)
            log->info("Network producer (card SID phase 4b): network.ledger_anchor is ON — a "
                      "(satellite, signal) family the coverage gate's DROP EDGE erased, and that "
                      "returns to the anchored union with the previous incumbent still tracking "
                      "it at the SAME admitted generation, restores that incumbent as the "
                      "candidate of record instead of being elected afresh as a first sighting.  "
                      "The election's existing rules still decide: the restored station must "
                      "track the key, be present, pass the hygiene host test and, under a held "
                      "S-basis, the minimum-age and quiet rules.  A return whose arcs are NEW "
                      "generations proceeds exactly as today.  Both outcomes are events "
                      "(PHASE_ANCHOR reentry_restore / reentry_new) with counters, and "
                      "anchor_arc_age is derived from the ledger's admitted age.  ⚠ "
                      "SOLVE-AFFECTING: this changes which station realizes the family's datum "
                      "after a gate drop, so it is graded as a behaviour change and it is "
                      "FINGERPRINTED (fresh convergence only).");
        if (network_ledger_anchor_ && network_phase_datum_min_stations_ <= 1)
            log->warn("Network producer (card SID phase 4b): network.ledger_anchor is armed with "
                      "network.phase_datum_min_stations at 1, so the coverage gate never drops a "
                      "family and the restore has no population.  The knob is INERT here.");
        // Card SID phase 5a: the emitter read and the declared roster.  REFUSED
        // without the phase-1 observatory, which owns both inputs: the arc ledger
        // the emitter's bookkeeping is compared with, and the roster the
        // declaration is built from.  A silent no-op would write an EMPTY
        // declaration to the wire, which says "the network carries nothing": a
        // false statement a consumer would act on.
        if (network_ledger_emit_ && !network_signal_roster_) {
            log->error("Network producer (card SID phase 5a): network.ledger_emit is armed "
                       "WITHOUT network.signal_roster, so there is no arc ledger to read the "
                       "emitter's bookkeeping against and no roster to build the declaration "
                       "from: the wire would carry an EMPTY declaration, which tells a "
                       "consumer the network carries no family for any signal.  DISARMED.  Arm "
                       "network.signal_roster (the phase-1 observatory, byte-identical on the "
                       "solve) alongside it.");
            network_ledger_emit_ = false;
        }
        if (network_ledger_emit_)
            log->info("Network producer (card SID phase 5a): network.ledger_emit is ON: the "
                      "emitter's per-family bookkeeping is READ against the arc ledger (card "
                      "PBH's hold generation against the ledger's admitted arc births, both "
                      "directions counted and evented; the emitter's first-sighting test "
                      "against the ledger's memory of the family, so a RETURN read as a first "
                      "sighting is named), and the producer DECLARES its signal roster on the "
                      "wire as a block of N records naming, per constellation and band digit, "
                      "the modelled phase and code signals, the card SCC companions and the "
                      "S-basis datum designation.  OBSERVATORY: nothing on the producer reads "
                      "either back, so the solve and every C / B / P / E record are "
                      "byte-identical against an unarmed run and the only difference in "
                      "products.txt is the N records themselves.");
        if (network_ledger_emit_ && !network_config_.emit_pb_hold)
            log->warn("Network producer (card SID phase 5a): network.ledger_emit is armed "
                      "without network.emit_pb_hold, so no emitted phase bias carries a hold "
                      "generation and the E1a class (the generation against the ledger's arc "
                      "births) has a ZERO DENOMINATOR: dead wiring, not agreement.  The "
                      "first-sighting class and the declaration are unaffected.");
        if (!network_ledger_reentry_
            && network_ledger_reentry_no_verdict_ == LedgerReentryNoVerdict::Refloat)
            log->warn("Network producer (card SID phase 3): "
                      "network.ledger_reentry_no_verdict = refloat is INERT without "
                      "network.ledger_reentry — the no-verdict population only exists when the "
                      "ledger drives the response.");
        // PB emission-continuity filter INERT unless the invariant carry is armed
        // (its re-based emission frame is the filter's input); the carry-ON case is
        // reported inside the carry_invariant block above.
        if (network_emit_pb_filter_ && !network_carry_invariant_)
            log->warn("Network: network.emit_pb_filter is INERT without network.carry_invariant "
                      "(+ network.carry_invariant_emit_rebase) — it filters the re-based emission "
                      "frame, which only exists under the invariant carry.");

        return true;
    }

    // =================================================================
    //  network subsection helper: loadNetworkContractHardening_
    // =================================================================
    /**
     * @brief RTA-3/4/5/11 product wire + S-contract hardening for late joiners and restarts.
     *
     * @c network.emit_generation_id, @c serialize_disc_counters, @c contract_rebroadcast_s,
     * @c emitter_staleness_horizon_s.  All default OFF => store and file byte-identical.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkContractHardening_(const nlohmann::json& json,
                                                   const std::shared_ptr<spdlog::logger>& log) {
        // Alias preamble: the body below reads the driver local it was written
        // against.  network_service_mode_ is written once, at the top of the
        // section, and never again — so this is the value the un-split code saw.
        const bool service_mode = network_service_mode_;

        // RTA-3/RTA-4/RTA-5/RTA-11: product wire/contract hardening for late joiners
        // and producer restarts.  All default OFF ⇒ store and file byte-identical.
        network_emit_generation_id_ =
            jval<bool>(json, "network.emit_generation_id", "network_emit_generation_id", false);
        network_serialize_disc_counters_ =
            jval<bool>(json, "network.serialize_disc_counters",
                       "network_serialize_disc_counters", false);
        network_contract_rebroadcast_s_ =
            jval<double>(json, "network.contract_rebroadcast_s",
                         "network_contract_rebroadcast_s", 0.0);
        network_emitter_staleness_horizon_s_ =
            jval<double>(json, "network.emitter_staleness_horizon_s",
                         "network_emitter_staleness_horizon_s", 0.0);
        if (network_contract_rebroadcast_s_ < 0.0) {
            log->warn("Network: network.contract_rebroadcast_s {} < 0 clamped to 0 (no "
                      "S-contract re-broadcast).", network_contract_rebroadcast_s_);
            network_contract_rebroadcast_s_ = 0.0;
        }
        if (network_emitter_staleness_horizon_s_ < 0.0) {
            log->warn("Network: network.emitter_staleness_horizon_s {} < 0 clamped to 0 "
                      "(keep-forever emitter baselines).", network_emitter_staleness_horizon_s_);
            network_emitter_staleness_horizon_s_ = 0.0;
        }
        if (network_emit_generation_id_)
            log->info("Network producer (RTA-3): solution-generation ID ON — every emitted epoch "
                      "carries a uint32 generation id derived from the first emitted epoch's GPS "
                      "time (optional I record + S-line gid=); a consumer treats a CHANGE of a "
                      "present id as a global datum break (producer restart).");
        if (network_serialize_disc_counters_)
            log->info("Network producer (RTA-4): discontinuity-counter VALUES serialized ON — "
                      "C/B/P records gain optional trailing post-emit counter fields so a "
                      "late-joining/tail reader reconstructs counters without a full replay "
                      "(reader adopts present values, replay recompute kept as a self-check).");
        if (network_contract_rebroadcast_s_ > 0.0) {
            if (network_config_.serialize_sbasis_contract)
                log->info("Network producer (RTA-5): S-contract re-broadcast ON — a CURRENT S line "
                          "(refreshed realized pivots) every {:.0f} s of product time and on any "
                          "datum-change emit.", network_contract_rebroadcast_s_);
            // P4: warn suppressed under service_mode — the bundle arms
            // serialize_sbasis_contract in the arming block below, so the
            // "inert" claim would be false by the end of the load.
            else if (!service_mode)
                log->warn("Network: network.contract_rebroadcast_s {} is set but "
                          "network.serialize_sbasis_contract is OFF — there is no S contract to "
                          "re-broadcast, so the knob is inert.  Enable serialize_sbasis_contract.",
                          network_contract_rebroadcast_s_);
        }
    }

    // =================================================================
    //  network subsection helper: applyNetworkServiceProfile_
    // =================================================================
    /**
     * @brief RTB-8 curated service profile + P3 post-arming re-validation.
     *
     * Runs LAST of the arming steps so it sees final values for every toggle it bundles.
     * BUNDLE-WINS: an armed bool is forced true even over an explicit user @c false, and each
     * such override is warned individually.  The two parse-site validations the arming can
     * bypass (the pathological withhold clamp and the clock-model validity restore) are
     * re-run here, idempotently, before the profile is announced.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::applyNetworkServiceProfile_(const nlohmann::json& json,
                                                 const std::shared_ptr<spdlog::logger>& log) {
        // Alias preamble: the body below reads the driver local it was written
        // against.  network_service_mode_ is written once, at the top of the
        // section, and never again — so this is the value the un-split code saw.
        const bool service_mode = network_service_mode_;

        // --- RTB-8: curated service profile -------------------------------------
        // One switch (network.service_mode) arms the coherent set of producer
        // toggles a live PPP-RTK service needs, instead of hand-assembling ~20
        // interlocking default-off flags (each with implicit couplings that were
        // only ever wired up in scattered test configs).  BUNDLE-WINS semantics:
        // a bool the profile arms is forced true even over an explicit user
        // `false`, and a numeric at 0 (default, an explicit 0, or a negative
        // clamped to 0 above) is armed to its service value — to keep a specific
        // knob off, run without service_mode.  P1: every override of a value the
        // USER actually wrote (presence-checked on both the dotted and the flat
        // legacy key form, the enable_ssr_bias_max_age_explicit_ pattern) is
        // individually WARNED; a user value that already equals the bundle's
        // choice is not an override and stays silent.  Placed after every armed
        // toggle is parsed so it sees final values; the two parse-site validations
        // it can bypass (withhold clamp, clock_model restore) are RE-RUN just
        // below (P3).  Default off ⇒ nothing armed (byte-identical).
        // Deliberately NOT armed (see the do-not-arm notes below the arms):
        //   - network.state_snapshot_path — needs a user-chosen path; a missing
        //     one gets a recommendation info line instead.
        //   - network.warmup_ura_floor_m — policy numeric with no universal
        //     default; the RTB-7 no-op warn above covers a misconfig.
        //   - network.clock_runaway_* ceilings — numeric policy, default-off
        //     quarantine; scope-limiting a runaway is an operator decision.
        //   - network.apriori_source (RTF-1) — an input-SOURCE selection, not a
        //     toggle, and orthogonal to the bundle: a service can run on sp3
        //     ultra-rapids (RTB-4) or brdc_rts equally.  Left exactly as set.
        // Cross-validation of the realized product against the consumer is the
        // S contract itself (RTA-5 re-broadcast + RTA-12 identity fields +
        // checkContract), which this profile turns on.  service_mode was read at
        // the TOP of this section so the coherence warnings could suppress under it.
        if (service_mode) {
            // P1: presence check — did the user write this key (either form)?
            auto userWrote = [&](const char* dotted, const char* flat) {
                return detail::resolvePath(json, dotted) != nullptr || json.contains(flat);
            };
            auto armBool = [&](bool& f, const char* dotted, const char* flat) {
                if (!f && userWrote(dotted, flat))
                    log->warn("Network (RTB-8): service_mode overrides explicit {}=false -> true "
                              "(opinionated bundle; run without service_mode to hand-tune).",
                              dotted);
                if (!f) f = true;
            };
            auto armDouble = [&](double& f, double v, const char* dotted, const char* flat) {
                if (f != 0.0) return;                       // explicit non-zero value wins
                if (userWrote(dotted, flat))
                    log->warn("Network (RTB-8): service_mode overrides explicit {} "
                              "(disabled: 0, or clamped to 0 above) -> {:.1f} (opinionated "
                              "bundle; run without service_mode to hand-tune).", dotted, v);
                f = v;
            };
            armBool(network_emit_clock_disc_,               // consumer resets on clock-pivot handover
                    "network.emit_clock_disc", "network_emit_clock_disc");
            armBool(network_emit_ura_,                      // per-sat quality → consumer weighting
                    "network.emit_ura", "network_emit_ura");
            armBool(network_ura_retro_bump_,                // per-sat post-fit disc
                    "network.ura_retro_bump", "network_ura_retro_bump");
            armBool(network_clock_model_,                   // two-state clock → C1 drift extrapolation
                    "network.clock_model", "network_clock_model");
            armBool(network_phase_leveling_,                // cross-epoch carry — carry_warm prerequisite of BOTH RAIM tiers
                    "network.phase_leveling", "phase_leveling");
            armBool(network_raim_,                          // post-fit RAIM: the σ̂0 EWMA that scales the emitted URA is fed by THIS base flag (NOT within_epoch), and its auto-coupling at the raim_within_epoch parse ran BEFORE this block, so arm it explicitly
                    "network.raim", "network_raim");
            armBool(network_raim_attribution_,              // complementary post-fit attribution refloat tier (the two halves of one backstop)
                    "network.raim_attribution", "network_raim_attribution");
            armBool(network_raim_within_epoch_,             // fault detect → drop-culprit → re-solve
                    "network.raim_within_epoch", "network_raim_within_epoch");
            armDouble(network_raim_withhold_sigma0_,        // withhold a still-contaminated epoch at ~2× the trigger (design guidance); re-validated below (P3)
                      2.0 * network_raim_sigma0_trigger_,
                      "network.raim_withhold_sigma0", "network_raim_withhold_sigma0");
            armBool(network_per_const_withhold_,            // P2/RTA-10: the bundle injects the withhold ceiling above, so the parse-site "no safe ceiling" rationale for not arming the carve-out is void here — degrade SCOPE, not availability
                    "network.per_const_withhold", "network_per_const_withhold");
            armBool(network_emit_epoch_status_record_,      // P2/RTA-10: H heartbeat — a service's live tail must distinguish "withheld" from "producer died"
                    "network.emit_epoch_status_record", "network_emit_epoch_status_record");
            // Pivot-selection / handover-signalling machinery: PROVABLY INERT under the
            // anchor-free sum-to-zero and broadcast-anchored clock datums (no pivot
            // exists, pivotHandoverSignalled always returns false), so don't force-arm
            // dead knobs there — leaving them false keeps the operator from being misled
            // that pivot machinery is active (and keeps the "INERT under ..." warns above
            // consistent with the realized flag state).  An EXPLICIT user set still
            // stands and is warned above.
            if (!network_clock_datum_sum_to_zero_
                && !network_clock_datum_broadcast_anchored_) {
                armBool(network_pivot_vanish_return_signal_,    // P2/A1: without it a vanish-return pivot change is an UNSIGNALLED datum break (emit_clock_disc cannot cover it)
                        "network.pivot_vanish_return_signal", "network_pivot_vanish_return_signal");
                armBool(network_sticky_pivot_,                  // P2/RTA-1: minimize handover rate (datum re-references the consumer pays for)
                        "network.sticky_pivot", "network_sticky_pivot");
            }
            armBool(network_scoped_handover_response_,      // P2/RTA-2: limit a handover's internal blast radius to the affected constellation(s)
                    "network.scoped_handover_response", "network_scoped_handover_response");
            armBool(network_emit_generation_id_,            // RTA-3 restart / datum-break detection
                    "network.emit_generation_id", "network_emit_generation_id");
            armBool(network_serialize_disc_counters_,       // RTA-4 late-joiner counter recovery
                    "network.serialize_disc_counters", "network_serialize_disc_counters");
            armBool(network_config_.serialize_sbasis_contract,    // v2 S contract header
                    "network.serialize_sbasis_contract", "serialize_sbasis_contract");
            armDouble(network_contract_rebroadcast_s_, 30.0,      // RTA-5 periodic re-broadcast
                      "network.contract_rebroadcast_s", "network_contract_rebroadcast_s");
            armBool(network_config_.product_flush_per_epoch,      // RTA-6 live-tail visibility
                    "network.product_flush_per_epoch", "product_flush_per_epoch");
            armDouble(network_config_.health_interval_s, 30.0,    // RTB-6 operator heartbeat
                      "network.health_interval_s", "health_interval_s");
            armDouble(network_config_.trace_rotate_mb, 256.0,     // P2/RTB-6: a 24/7 service must not grow its CSVs unbounded (keep stays at its parsed default 3)
                      "network.trace_rotate_mb", "trace_rotate_mb");
            // P4(b): EVERY file-side knob the bundle just armed (per-epoch flush,
            // disc-counter serialization, v2 S contract + re-broadcast, generation
            // id, H heartbeat) writes into the product FILE — with no
            // product_output_path there is no file and all of them are inert.  The
            // parse-site RTA-6 flush warn cannot catch this (it ran before the
            // arming), so say it here, once, listing the inert knobs.
            if (network_config_.product_output_path.empty())
                log->warn("Network (RTB-8): service_mode is ON but network.product_output_path "
                          "is empty — no product file is written, so the bundle's file-side "
                          "knobs (per-epoch flush, disc-counter serialization, v2 S contract + "
                          "{:.0f} s re-broadcast, generation id, epoch-status heartbeat) are "
                          "ALL inert.  Set network.product_output_path.",
                          network_contract_rebroadcast_s_);
            // Not armed (needs a user-chosen path) — but a 24/7 service without a
            // snapshot cold-starts a new generation on every restart, so recommend it.
            if (network_config_.state_snapshot_path.empty())
                log->info("Network (RTB-8): service_mode is on with no "
                          "network.state_snapshot_path — consider setting one so a producer "
                          "restart WARM-resumes from the RTB-7 snapshot (generation id "
                          "preserved, no consumer-wide ambiguity reset) instead of "
                          "cold-starting a new generation.");
        }

        // --- P3: post-arming re-validation ---------------------------------------
        // Two parse-site checks earlier in this section run BEFORE the service
        // arming block and would be bypassed by service-armed values:
        //   (a) the R11/R1 pathological σ̂0-withhold clamp — the bundle arms the
        //       withhold at 2× network.raim_sigma0_trigger, which for a small
        //       explicit trigger (<= 0.5) lands in the pathological (0,1] band the
        //       parse-site clamp exists to reject (σ̂0 is floored at 1 ⇒ near-total
        //       availability loss);
        //   (b) the clock_model_* validity-restore, gated on the PRE-arming
        //       network_clock_model_ — service_mode arms the oscillator, so a stray
        //       invalid clock_model_q2 would otherwise run it unvalidated.
        // The parse-site originals are KEPT (moving them here would change
        // non-service semantics: the within-epoch auto-coupling reads the CLAMPED
        // withhold, and the R5 ura_staleness_q2 default capture reads the RESTORED
        // q2 — both sit between the two sites).  These re-runs are IDEMPOTENT for
        // non-service runs: they re-apply the exact parse-site conditions, which
        // the parse-site pass already discharged (only the arming block writes
        // these values in between), so they fire — with their one warn — only when
        // the ARMING itself introduced the invalid value.
        //
        // (c) Card Q3 × network.clock_model.  Under the two-state oscillator the
        //     satellite clock is carried by clock_osc and is NOT stashed to the D2
        //     carry at all (networkStashCarry's `if (!clk_model_on)`), so the
        //     temporal anchor has nothing to fold: it would be silently INERT while
        //     its armed banner claimed otherwise.  This check MUST live post-arming —
        //     service_mode force-arms the oscillator, and the C1 card's defect #2 is
        //     precisely a clock-knob interaction logged from the pre-arming value.
        if (network_clock_temporal_anchor_ && network_clock_model_) {
            log->error("Network (Card Q3): network.clock_temporal_anchor is armed but "
                       "network.clock_model is ON{} — the two-state oscillator owns the clock's "
                       "cross-epoch memory and the D2 clock carry the anchor modifies is never "
                       "stashed, so the anchor is INERT (0 folds; its run-end counters will say "
                       "so).  The oscillator is a measured NO-GO on this lineage (Hadamard p ≈ 1 "
                       "⇒ white-FM, RWFM undetectable); disarm network.clock_model.",
                       network_service_mode_ ? " (FORCE-ARMED by network.service_mode)" : "");
        }
        if (network_raim_withhold_sigma0_ != 0.0 && network_raim_withhold_sigma0_ <= 1.0) {
            log->warn("Network: network.raim_withhold_sigma0 {} (service-armed at 2x "
                      "network.raim_sigma0_trigger {}) is <= 1 — σ̂0 is floored at 1, so a "
                      "ceiling at or below 1 would withhold most warm epochs (near-total "
                      "availability loss).  Disabling it; raise network.raim_sigma0_trigger "
                      "or set an explicit network.raim_withhold_sigma0 > 1 to restore the "
                      "withhold (the per-constellation carve-out is inert until then).",
                      network_raim_withhold_sigma0_, network_raim_sigma0_trigger_);
            network_raim_withhold_sigma0_ = 0.0;
        }
        if (network_clock_model_ &&
            (!(network_clock_model_q2_ > 0.0) || network_clock_model_q1_ < 0.0 ||
             !(network_clock_model_k_sigma_ > 0.0) || !(network_clock_model_meas_std_m_ > 0.0))) {
            log->warn("Network: invalid network.clock_model_* parameters (q1={}, q2={}, k_sigma={}, "
                      "meas_std={}) under the service-armed clock model; restoring defaults "
                      "(q1=0, q2=1.5e-7, k_sigma=6, meas_std=0.05).",
                      network_clock_model_q1_, network_clock_model_q2_,
                      network_clock_model_k_sigma_, network_clock_model_meas_std_m_);
            network_clock_model_q1_ = 0.0; network_clock_model_q2_ = 1.5e-7;
            network_clock_model_k_sigma_ = 6.0; network_clock_model_meas_std_m_ = 0.05;
        }
        // m8: re-capture the R5 ZOH clock-staleness q² AFTER the service-armed
        // clock_model_q2 restore above.  The parse-site default (line ~3600) read the
        // PRE-arming clock_model_q2; under service_mode a stray-invalid clock_model_q2
        // was left unvalidated there (the parse-site validity-restore is gated on the
        // PRE-arming clock_model flag) and the negative guard then zeroed the derived
        // ura_staleness_q2 — so a service run that armed the oscillator carried
        // staleness inflation OFF despite the model being on.  Re-running the exact
        // capture picks up the NOW-restored q2.  Gated on service_mode so the
        // non-service path is byte-identical (an explicit user value still wins via
        // jval, exactly as at the parse site).
        if (service_mode) {
            network_ura_staleness_q2_ =
                jval<double>(json, "network.ura_staleness_q2", "network_ura_staleness_q2",
                             network_clock_model_q2_);
            if (network_ura_staleness_q2_ < 0.0) {
                log->warn("Network: network.ura_staleness_q2 {} < 0 → 0 (ZOH staleness inflation "
                          "off; check network.clock_model_q2, which supplies the default).",
                          network_ura_staleness_q2_);
                network_ura_staleness_q2_ = 0.0;
            }
        }

        // m9: announce the armed service profile AFTER the P3 re-validation above, so
        // the logged withhold reflects its POST-clamp value.  Armed at 2× a small
        // explicit trigger the withhold can land in the pathological (0,1] band and be
        // disabled by the clamp just above; logging inside the arming block printed the
        // pre-clamp ceiling one line before the clamp retracted it.
        if (service_mode)
            log->info("Network (RTB-8): service_mode ON — armed the RT-service producer set "
                      "(clock/URA disc + retro-bump, clock-model, phase-leveling, post-fit + "
                      "within-epoch RAIM, withhold @ {:.1f} with per-constellation carve-out, "
                      "epoch-status heartbeat, pivot vanish-return signalling, sticky pivot, "
                      "scoped handover response, generation-id, disc-counter serialization, "
                      "v2 contract + {:.0f} s re-broadcast, per-epoch flush, {:.0f} s health "
                      "heartbeat, {:.0f} MiB trace rotation).  service_mode is an OPINIONATED "
                      "bundle: it overrides explicit disables of the armed keys (each override "
                      "is warned individually) — run without service_mode to hand-tune.  Left "
                      "user-set: state_snapshot_path (needs a path), warmup_ura_floor_m and "
                      "the clock_runaway_* ceilings (policy numerics with no universal default).",
                      network_raim_withhold_sigma0_, network_contract_rebroadcast_s_,
                      network_config_.health_interval_s, network_config_.trace_rotate_mb);
    }

    // =================================================================
    //  network subsection helper: validateNetworkCarryPosture_
    // =================================================================
    /**
     * @brief Post-arming reports and validation for the carry / slip / RAIM posture.
     *
     * @c network.emitter_staleness_horizon_s and @c start_offset_hours, then the ON reports
     * for phase leveling, the rx-CB and ZWD carries, IF-phase TD detection and post-fit RAIM
     * — each with the INERT warning for its missing prerequisite.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     * @return @c false on a fatal misconfiguration (aborts @c loadFromJson).
     */
    bool GnssConfig::validateNetworkCarryPosture_(const nlohmann::json& json,
                                                  const std::shared_ptr<spdlog::logger>& log) {
        if (network_emitter_staleness_horizon_s_ > 0.0)
            log->info("Network producer (RTA-11): emitter staleness horizon {:.0f} s ON — a "
                      "(sat[,signal]) key returning after a longer product-time gap reseeds its "
                      "phase/code/clock baselines as a first sighting (no discontinuity bump); "
                      "the horizon travels in the S contract (stale=) for replay.",
                      network_emitter_staleness_horizon_s_);

        // Testing fast-forward: skip ahead to a late-hour event (e.g. the ~4.3 h lock-in) in a
        // few warm-up epochs instead of hours of processing.  Cold-starts the carry at the
        // offset, so it reproduces an externally-driven event, not the exact prior history.
        network_start_offset_hours_ =
            jval<double>(json, "network.start_offset_hours", "network_start_offset_hours", 0.0);
        if (network_start_offset_hours_ < 0.0) {
            log->warn("Network: network.start_offset_hours {} < 0 clamped to 0 (process from "
                      "the start).", network_start_offset_hours_);
            network_start_offset_hours_ = 0.0;
        }
        if (network_phase_leveling_) {
            if (network_phase_leveling_q_amb_ < 0.0) {
                log->error("Network: network.phase_leveling_q_amb must be >= 0 (got {}); a "
                           "negative carry process noise is meaningless.",
                           network_phase_leveling_q_amb_);
                return false;
            }
            if (network_phase_leveling_q_amb_ == 0.0)
                log->info("Network producer (phase-leveling): carrying per-station carrier-phase "
                          "ambiguities with ZERO process noise — the correct random-constant model, "
                          "so each ambiguity ACCUMULATES carrier-phase precision indefinitely "
                          "between resets.  Continuity is enforced by the IF-phase / MW-GF slip "
                          "detectors (refloat), gaps by the {:.0f} s max-gap drop — NOT by "
                          "continuous dilation.  warmup {} epochs.  Non-pivot ambiguity columns "
                          "only; the pivot's 1e10 pins still define the phase-bias gauge.",
                          network_phase_leveling_max_gap_s_,
                          network_phase_leveling_warmup_epochs_);
            else
                log->info("Network producer (phase-leveling): carrying per-station carrier-phase "
                          "ambiguities across epochs (soft gap-reacquisition q_amb {:.1e} m²/s, "
                          "warmup {} epochs, max gap {:.0f} s).  Non-pivot ambiguity columns only; "
                          "the pivot's 1e10 pins still define the phase-bias gauge.",
                          network_phase_leveling_q_amb_, network_phase_leveling_warmup_epochs_,
                          network_phase_leveling_max_gap_s_);
            if (network_cycle_slip_turbo_edit_) {
                log->info("Network producer (phase-leveling T0): per-station MW+GF TurboEdit "
                          "cycle-slip REPAIR ENABLED — only confident integer slips (frac ≤ {:.2f}, "
                          "|dN| ≤ {} cyc) shift the carried ambiguity continuously (value += dN·λ); "
                          "less-confident detections are LEFT untouched (BIA-less float-MW false "
                          "positives dominate).  Non-pivot, raw-per-signal ambiguities only.",
                          network_cycle_slip_turbo_edit_frac_max_,
                          network_cycle_slip_turbo_edit_dn_max_);
            }
            if (network_cycle_slip_mw_detect_) {
                if (network_cycle_slip_turbo_edit_)
                    log->warn("Network: network.cycle_slip.mw_detect and .turbo_edit are BOTH on — "
                              "TurboEdit already runs the MW detector for integer REPAIR; the "
                              "mw_detect REFLOAT path is redundant with it and stays inert (repair "
                              "wins).  Use mw_detect INSTEAD of turbo_edit at the 30 s product rate.");
                else
                    log->info("Network producer (phase-leveling): MW slip DETECTOR enabled (mw_detect) — "
                              "a confident wide-lane integer jump (frac ≤ {:.2f}) OR a receiver LLI "
                              "loss-of-lock REFLOATS the carried ambiguity (pending-reseed), never "
                              "applying an integer.  Iono-safe at 30 s (no GF repair).  Complements "
                              "the IF-phase TD detector; non-pivot only.",
                              network_cycle_slip_turbo_edit_frac_max_);
            }
            if (network_shared_carry_var_floor_pb_m2_ > 0.0 || network_shared_carry_var_floor_cb_m2_ > 0.0)
                log->info("Network producer (phase-leveling, B4): D2 shared-carry variance floors — "
                          "sat phase-bias {:.2e} m², sat code-bias {:.2e} m² (caps Λ so a stiff zero-q "
                          "carry cannot lock a biased product in; 0 = off).",
                          network_shared_carry_var_floor_pb_m2_, network_shared_carry_var_floor_cb_m2_);
            if (network_phase_leveling_los_scale_)
                log->info("Network producer (phase-leveling F1): per-LOS variance scale ARMED — at "
                          "inject every carried ambiguity row's variance is multiplied by B_eff, the "
                          "number of carried ambiguity columns injected on the same (station, "
                          "satellite) line of sight this epoch (after q·gap, before the var floor "
                          "and the VCE factor), so the B rows of one LOS sum to ONE unit of "
                          "iono-direction precision instead of B — undoing the per-LOS B-fold "
                          "over-count of the iono-direction carry precision "
                          "(SPEC_amb_iono_split_overcount: p_k = B·p_k-1 + p_code froze the level "
                          "at K_eff = (B+1)/(B-1) epochs).  B_eff = 1 ⇒ unchanged; loosens only.  "
                          "Run-end line 'Network (leveling LOS-scale F1)' is the ON-path telemetry.");
            if (network_phase_leveling_local_block_)
                log->info("Network producer (phase-leveling F3): station-wide jointly-whitened "
                          "local block ARMED — each station's injectable carried ambiguity "
                          "columns are carried as ONE block: the harvest stashes the conditional "
                          "CROSS covariances (N_rr^-1)_(ci,cj) beside each column's variance "
                          "(same per-column solve, no extra cost), the inject marginalizes "
                          "every non-admitted column (departed / stale / freed / reseed / "
                          "pinned) out of Sigma, factors Sigma = L L^T and emits n unit-weight "
                          "rows L^-1 [I | M | y] — exact for EVERY local-local direction given "
                          "the border, i.e. the per-LOS iono direction AND the station-common "
                          "rx clock / ZWD / rx-DCB directions F1 leaves over-counted "
                          "(SPEC_amb_iono_split_overcount).  A non-SPD factor falls back to the "
                          "per-column rows for that station-epoch and is counted.  Run-end line "
                          "'Network (leveling local-block F3)' is the ON-path telemetry.{}",
                          network_phase_leveling_los_scale_
                              ? "  phase_leveling_los_scale is ALSO armed: F3 supersedes it on "
                                "every whitened block; the F1 scale applies only on a "
                                "factorization fallback."
                              : "");
        } else if (network_cycle_slip_turbo_edit_) {
            log->warn("Network: network.cycle_slip.turbo_edit is on but network.phase_leveling is "
                      "off — T0 slip repair acts on the carried ambiguities, so it is INERT "
                      "without phase-leveling.  Enable network.phase_leveling too.");
        }
        // F1 INERT guard: the scale acts on the phase-leveling carry rows at their
        // inject site, which exists only when the carry is the live vehicle
        // (phase_leveling on AND not carry_invariant — Card 4's DD carry supersedes
        // it and `do_amb_carry` skips the inject outright).  network_carry_invariant_
        // is populated by this point (the M2 v2 guard below relies on the same).
        if (network_phase_leveling_los_scale_
            && (!network_phase_leveling_ || network_carry_invariant_))
            log->warn("Network: network.phase_leveling_los_scale is on but the phase-leveling "
                      "ambiguity carry is NOT folded on this lineage (phase_leveling={}, "
                      "carry_invariant={}) — F1 scales those carry rows at inject, so there is "
                      "nothing to scale: this knob is INERT on this posture (standing invariant "
                      "#4).  Arm it on a phase_leveling, non-carry_invariant lineage.",
                      network_phase_leveling_, network_carry_invariant_);
        // F3 INERT guard: the block is built at the same inject site as F1 (so the
        // same phase_leveling / carry_invariant conditions apply) AND its rows live in
        // A_s/A_r as Card-1 coupling rows, so it needs carry_conditional_coupling; the
        // Card-1b joint departed-column whitening owns that station's row block in
        // this build (mutually exclusive — the block path is predicated on
        // !carry_cpl_joint at the inject).  All four members are populated by here.
        if (network_phase_leveling_local_block_
            && (!network_phase_leveling_ || network_carry_invariant_
                || !network_carry_conditional_coupling_ || network_carry_coupling_joint_whiten_))
            log->warn("Network: network.phase_leveling_local_block is on but the station-wide "
                      "whitened block cannot be built on this lineage (phase_leveling={}, "
                      "carry_invariant={}, carry_conditional_coupling={}, "
                      "carry_coupling_joint_whiten={}) — it needs the phase-leveling carry "
                      "folded as Card-1 coupling rows (phase_leveling on, carry_invariant "
                      "off, carry_conditional_coupling on) and is mutually exclusive with "
                      "the Card-1b joint departed-column whitening: this knob is INERT on "
                      "this posture (standing invariant #4).",
                      network_phase_leveling_, network_carry_invariant_,
                      network_carry_conditional_coupling_, network_carry_coupling_joint_whiten_);
        // ---- RF-1 (docs/refound/RF-1_float_ambiguities_design.md §7.1): the AMBIGUITY
        //      VEHICLE interlocks.  Placed here because network_carry_invariant_ is only
        //      populated by loadNetworkDatumSignalling_, which runs after the carries.
        //
        // ONE VEHICLE, TWO BEDS (the re-founding decision of 2026-09-14): the field
        // golden's carry (carry_invariant) and the sim golden's (leveling) diverged in
        // July and were never re-validated as a whole, and RF-1's whole subject is the
        // leveling carry's fold — so float_kf on a lineage that does not fold it at all
        // is not a degraded run, it is a MEANINGLESS one.  A hard error, not a warn:
        // invariant #4's lesson is that an inert arm reads exactly like a clean one.
        if (network_config_.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf) {
            if (!network_phase_leveling_ || network_carry_invariant_) {
                log->error("Network (RF-1 §7.1): network.ambiguity_vehicle = \"float_kf\" requires "
                           "network.phase_leveling = true AND network.carry_invariant = false "
                           "(got phase_leveling={}, carry_invariant={}).  float_kf replaces the "
                           "LEVELING carry's scalar prior with a float state carrying a joint "
                           "covariance; on a carry_invariant lineage the Card-4 DD carry IS the "
                           "ambiguity time-link and ApplicationNetworkAssembly's do_amb_carry "
                           "skips the leveling inject outright, so there is nothing for the "
                           "vehicle to replace and the arm could not move a byte.",
                           network_phase_leveling_, network_carry_invariant_);
                return false;
            }
            // F1 (×B_eff) and F3 (the station-wide whitened block) are the mechanisms
            // float_kf SUBSUMES: the joint block removes the per-LOS iono over-count
            // EXACTLY where F1 corrects it approximately, and F3's buildLocalBlock IS the
            // fold float_kf makes structural (design §3.2 / §4.1).  Leaving either armed
            // beside the vehicle would make the effective fold un-attributable between two
            // knobs, so they are REFUSED WITH A LINE rather than silently ignored.
            if (network_phase_leveling_los_scale_) {
                log->error("Network (RF-1 §7.1): network.phase_leveling_los_scale (F1) is REFUSED "
                           "under network.ambiguity_vehicle = \"float_kf\" and has been forced "
                           "OFF.  F1's ×B_eff is an APPROXIMATE correction for the per-LOS iono "
                           "over-count that the vehicle's joint block removes EXACTLY; arming "
                           "both would apply the correction twice and make the fold "
                           "un-attributable between two knobs.");
                network_phase_leveling_los_scale_ = false;
            }
            if (network_phase_leveling_local_block_) {
                log->error("Network (RF-1 §7.1): network.phase_leveling_local_block (F3) is "
                           "REFUSED under network.ambiguity_vehicle = \"float_kf\" and has been "
                           "forced OFF.  float_kf IS the joint block, made structural rather than "
                           "optional — CouplingRowBuilder::buildLocalBlock is re-pointed by the "
                           "vehicle with the birth / drop / handover semantics fixed, so the F3 "
                           "knob beside it would be a second, independently-armed switch on the "
                           "same fold.  ⚠ Forcing the knob off does NOT disarm the block: the "
                           "vehicle supplies it structurally (NetworkKnobs::pl_local_block is the "
                           "resolved live predicate).");
                network_phase_leveling_local_block_ = false;
            }
            // ---- RF-1 step 3: the block's own PREREQUISITES.  The vehicle promises the
            //      joint fold, so a posture where that fold cannot be built is not a
            //      degraded arm, it is a SILENT one — the inject would take the
            //      per-column route on every station-epoch and the run would read as a
            //      clean float_kf arm while carrying today's over-count (invariant #4).
            //      The two conditions are exactly the live predicate the inject applies:
            //      `pl_local_block && carry_cpl_cond && !carry_cpl_joint`.
            if (!network_carry_conditional_coupling_) {
                log->error("Network (RF-1 step 3): network.ambiguity_vehicle = \"float_kf\" "
                           "requires network.carry_conditional_coupling = true.  The vehicle's "
                           "fold IS CouplingRowBuilder::buildLocalBlock, whose rows live in "
                           "A_s/A_r as Card-1 coupling rows and whose off-diagonals are the "
                           "harvested conditional cross covariances; without the conditional "
                           "coupling there is no block to build and every station-epoch would "
                           "silently take the per-column route — an arm that reads clean and "
                           "carries today's over-count.  (carry_conditional_coupling itself "
                           "requires network.shared_carry_full_cov.)");
                return false;
            }
            if (network_carry_coupling_joint_whiten_) {
                log->error("Network (RF-1 step 3): network.ambiguity_vehicle = \"float_kf\" is "
                           "MUTUALLY EXCLUSIVE with network.carry_coupling_joint_whiten "
                           "(Card 1b).  Both own the station's coupling row block — 1b whitens "
                           "the DEPARTED-column marginalization, the vehicle whitens the joint "
                           "LOCAL prior — and the inject's own predicate already excludes the "
                           "block path under 1b, so arming both would disarm the vehicle "
                           "silently.  Turn one off.");
                return false;
            }
            // The young-arc inflate is a TIME-DECAYING PATCH ON THE INJECT for exactly
            // the defect float_kf's floored birth prior + bounded drop fix at the source
            // (design §4.1).  It is not refused — it is a validated mechanism and an arm
            // may deliberately want the contrast — but an unnoticed carry-over from an
            // RF-0 config would confound the birth-floor calibration, so it is named.
            if (network_phase_leveling_young_inflate_var_m2_ > 0.0)
                log->warn("Network (RF-1 §7.1): network.phase_leveling_young_inflate_var_m2 = {} "
                          "is armed under ambiguity_vehicle = \"float_kf\".  The young-arc "
                          "inflate is a time-decaying patch on the INJECT for the same defect the "
                          "vehicle's floored birth prior and bounded drop fix at the source "
                          "(design §4.1: it should stay unarmed and be retired if RF-1 wins).  "
                          "Two mechanisms acting on the same young-arc variance make the birth "
                          "floor's calibration un-attributable — arm it only as a deliberate "
                          "contrast.", network_phase_leveling_young_inflate_var_m2_);
        }
        // ---- RF-2: the HELD S-BASIS's own interlocks.  It is a statement about the
        //      ambiguity that IS the phase datum, so it is meaningless on a vehicle where
        //      the other ambiguities are not float states — the accident it replaces (the
        //      level vehicle's frozen zero-q anchor) is still in force there.
        if (network_phase_datum_held_basis_) {
            if (network_config_.ambiguity_vehicle != NetworkAmbiguityVehicle::FloatKf) {
                log->error("Network (RF-2): network.phase_datum_held_basis requires "
                           "network.ambiguity_vehicle = \"float_kf\".  The held basis "
                           "exists because RF-1 made every ambiguity a live state "
                           "INCLUDING the one that realizes the satellite phase-bias "
                           "datum; on the \"level\" vehicle the zero-q anchor is already "
                           "an (accidental) held basis, so arming this would change "
                           "nothing it claims to change.");
                return false;
            }
            if (!network_phase_datum_prioritized_) {
                log->error("Network (RF-2): network.phase_datum_held_basis requires "
                           "network.phase_datum_prioritized — the per-(satellite, signal) "
                           "anchor election is the thing whose datum is being held.  "
                           "Under the legacy single-pivot datum every freedom is pinned "
                           "at the pivot and there is no election to hold.");
                return false;
            }
            log->info("Network (RF-2): network.phase_datum_held_basis ON — each pinned "
                      "(satellite, signal) ambiguity is the S-BASIS, FROZEN at its value "
                      "at election for the whole of that anchor's tenure and never "
                      "re-derived mid-tenure; the satellite phase bias is then estimable "
                      "as (PB + that constant).  Election gates: arc age >= {} loop "
                      "epoch(s), no screen flag within {} epoch(s).  Solve-affecting.",
                      network_phase_anchor_min_age_ep_,
                      network_phase_anchor_flag_quiet_ep_);
            if (network_phase_datum_held_cold_release_ || network_phase_datum_held_mark_cold_)
                log->info("Network (RF-2d): held-basis handover — cold-family release {}, "
                          "declare-cold-only {}.  Solve-affecting (release) / wire-affecting "
                          "(declaration).",
                          network_phase_datum_held_cold_release_ ? "ON" : "off",
                          network_phase_datum_held_mark_cold_ ? "ON" : "off");
        } else if (network_phase_datum_held_cold_release_ || network_phase_datum_held_mark_cold_) {
            log->warn("Network (RF-2d): phase_datum_held_cold_release / _held_mark_cold are "
                      "set but network.phase_datum_held_basis is OFF — they act on the HELD "
                      "basis handover and are INERT this run (standing invariant #4).");
        }
        if (!network_phase_datum_held_basis_
            && (network_phase_anchor_min_age_ep_ != 10
                || network_phase_anchor_flag_quiet_ep_ != 5)) {
            log->warn("Network (RF-2): phase_anchor_min_age_ep / _flag_quiet_ep are set "
                      "but network.phase_datum_held_basis is OFF — they gate the HELD "
                      "basis election and are INERT this run (standing invariant #4).");
        }
        // ⚠ The vehicle is SOLVE-AFFECTING from step 3 on.  The line enumerates what is
        // actually live on THIS posture rather than naming a build number, so a reader of
        // a run manifest can never infer a mechanism the arm did not carry (the step-1
        // lesson: "float_kf" alone said nothing about what ran).
        if (network_config_.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf)
            log->warn("Network (RF-1): network.ambiguity_vehicle = \"float_kf\" — SOLVE-AFFECTING. "
                      "Live on this posture: the JOINT BLOCK is structural (step 3 — "
                      "buildLocalBlock re-pointed as the vehicle's fold, so the carried "
                      "covariance is folded as a proper prior instead of N independent "
                      "marginals); the floored birth prior + bounded DROP (step 4) {}; the "
                      "declared anchor S-transform (step 5) {}.  Grade against a \"level\" arm "
                      "on the two instruments, never against a previous float_kf build.",
                      network_config_.amb_float_birth_var_m2 > 0.0
                          ? fmt::format("ARE ARMED (birth floor {} m^2)",
                                        network_config_.amb_float_birth_var_m2)
                          : std::string("are ARMED at an UNFLOORED birth prior "
                                        "(amb_float_birth_var_m2 = 0 ⇒ var_birth = var_first)"),
                      network_config_.amb_float_declared_handover
                          ? "is ARMED" : "is OFF (the anchor re-reference still ERASES)");
        if (network_config_.ambiguity_vehicle == NetworkAmbiguityVehicle::Level
            && (network_config_.amb_float_birth_var_m2 > 0.0
                || network_config_.amb_float_q_m2s > 0.0
                || network_config_.amb_float_declared_handover))
            log->warn("Network (RF-1 §7.1): amb_float_* knobs are set (birth_var_m2={}, "
                      "q_m2s={}, declared_handover={}) but network.ambiguity_vehicle is "
                      "\"level\" — they are INERT on this posture (standing invariant #4).",
                      network_config_.amb_float_birth_var_m2,
                      network_config_.amb_float_q_m2s,
                      network_config_.amb_float_declared_handover);
        // M2 v2 (docs/SPEC_carry_vce_apply.md): post-parse INERT guard for the `pl`
        // apply family — network_carry_invariant_ is only populated by this point.
        //
        // This is the RAIM-family lesson applied before the run instead of after it.
        // The phase-leveling ambiguity carry is FOLDED only when carry_warm (which
        // requires network.phase_leveling) and NOT carry_invariant: Card 4's DD carry
        // supersedes it as a solve input, so ApplicationNetworkAssembly's
        // `do_amb_carry` skips the inject outright and R.pl_amb stays empty — which
        // means the monitor sees no pl rows either.  On such a lineage an apply armed
        // on `pl` cannot move a single byte.  The run-end summary would say so ("NO
        // GROUPS" / "NEVER STEPPED"), but that costs a whole field run to learn.
        if (network_carry_vce_apply_) {
            const auto app_fams =
                NetworkCarryVce::parseComponents(network_carry_vce_apply_families_);
            const bool apply_pl =
                std::find(app_fams.begin(), app_fams.end(), std::string("pl")) != app_fams.end();
            if (apply_pl && (!network_phase_leveling_ || network_carry_invariant_))
                log->warn("Network (M2 v2): carry_vce_apply_families includes 'pl' but the "
                          "phase-leveling ambiguity carry is NOT folded on this lineage "
                          "(phase_leveling={}, carry_invariant={}) — Card 4's DD carry "
                          "supersedes it, so the pl carry has no inject site and the M2 "
                          "monitor sees no pl rows.  The pl factor will sit at 1.0 for the "
                          "whole run: this arm is structurally INERT (standing invariant #4). "
                          "Run the apply on a phase_leveling lineage, or apply pb/cb instead.",
                          network_phase_leveling_, network_carry_invariant_);
        }
        // rx-CB carry (SPEC_rx_cb_carry): post-parse validation — code_datum_time_linked
        // and global_vtec_sh_ are both populated by this point.
        if (network_carry_rx_code_bias_) {
            if (network_rx_cb_carry_q_ < 0.0) {
                log->error("Network: network.rx_cb_carry_q must be >= 0 (got {}).",
                           network_rx_cb_carry_q_);
                return false;
            }
            if (!network_code_datum_time_linked_) {
                log->warn("Network: carry_rx_code_bias requires code_datum_time_linked (a "
                          "time-linked code datum keeps the carried receiver-bias lump "
                          "frame-continuous across epochs) — FORCED OFF.");
                network_carry_rx_code_bias_ = false;
            } else {
                if (!global_vtec_sh_.rep_error_demean)
                    log->warn("Network: carry_rx_code_bias without "
                              "estimation.global_vtec_sh.rep_error_demean — the rx-CB ↔ "
                              "iono-level ↔ deviation-mean direction is a three-way "
                              "quasi-null; arm rep_error_demean so each channel owns its "
                              "share (SPEC_rx_cb_carry §3).");
                log->info("Network (rx-CB carry): carrying receiver code biases across epochs "
                          "as diagonal RW priors (q {:.3e} m²/s; var_eff = var + q·Δt; "
                          "inject innovation gate 10σ + 0.5 m guard = the receiver-reset "
                          "response; harvested outside the phase-leveling gate — live on "
                          "the carry_invariant lineage).",
                          network_rx_cb_carry_q_);
            }
        }
        // Card SCC (R6): the second code channel's two INTERLOCKS.  Checked here, after
        // the rx-CB carry block, because both inputs are only final at this point — the
        // carry itself can still be FORCED OFF above, and channel_mode belongs to the
        // processing section.  A refused interlock disarms the knob rather than failing
        // the load: the posture is then exactly the unarmed one, which is a valid run.
        if (network_config_.second_code_channel) {
            if (channel_mode_ == ChannelMode::All) {
                // Card SID phase 6b (R1): SUBSUMPTION, not refusal.  The all-channel
                // widening adds every admitted code of every selected band, which is a
                // superset of this card's allow-listed band-1 set, and phase 6b routes
                // every one of those rows through the receiver inter-signal state this
                // card created.  So the knob is left ARMED and its allow-list is
                // ignored; only its SIGMA SCALE still reads, now over every
                // non-canonical code row rather than over the listed ones.
                log->info("Network (card SID phase 6b): network.second_code_channel is "
                          "SUBSUMED by processing.channel_mode = 'all'.  The widening adds "
                          "every admitted code attribute of every selected band, which "
                          "contains card SCC's allow-listed band-1 set, so "
                          "network.second_code_signals is IGNORED and "
                          "network.second_code_sigma_scale ({:.3f}) applies to every "
                          "NON-CANONICAL code row instead.  Every such row still routes "
                          "through the receiver inter-signal state <system>_ISC_<signal>.",
                          network_config_.second_code_sigma_scale);
            } else if (!network_carry_rx_code_bias_) {
                log->error("Network (card SCC): network.second_code_channel requires "
                           "network.carry_rx_code_bias — the receiver INTER-SIGNAL state the "
                           "card creates is a receiver code bias like any other and has to be "
                           "restored across epochs by that carry, or it cold-starts every "
                           "epoch and determines itself from one epoch of code.  DISARMED.");
                network_config_.second_code_channel = false;
            } else if (network_config_.second_code_signals.size() < 2) {
                log->error("Network (card SCC): network.second_code_channel is armed but "
                           "network.second_code_signals holds {} admitted entr(y/ies) — a "
                           "second code needs at least two band-1 codes in the list to ever be "
                           "one.  DISARMED.", network_config_.second_code_signals.size());
                network_config_.second_code_channel = false;
            } else {
                log->info("Network (card SCC): SECOND CODE CHANNEL armed — a station "
                          "contributes every allow-listed band-1 code it reports [{}] besides "
                          "the one its canonical latch selected, as CODE-ONLY rows, each "
                          "modelled with its own receiver inter-signal state "
                          "(<system>_ISC_<signal>, unit coefficient, no zero-mean prior).  No "
                          "phase row, no ambiguity, no phase-bias column and no datum change; "
                          "the Melbourne-Wubbena detector ignores the added rows.  Code sigma "
                          "x {:.3f} on the rows that route through the new state.",
                          [&] {
                              std::string s;
                              for (const auto& sig : network_config_.second_code_signals) {
                                  if (!s.empty()) s += ", ";
                                  s += sig;
                              }
                              return s;
                          }(),
                          network_config_.second_code_sigma_scale);
            }
        }
        // Card SID phase 6a (R6): the second phase channel's three INTERLOCKS.  Checked
        // beside card SCC's, after the vehicle and the datum knobs are final.  A refused
        // interlock disarms the knob rather than failing the load: the posture is then
        // exactly the unarmed one, which is a valid run.
        if (network_config_.second_phase_channel) {
            if (channel_mode_ == ChannelMode::All) {
                // Card SID phase 6b (R1): SUBSUMPTION, not refusal, for the same reason
                // as card SCC above.  The widening's phase half is a superset of this
                // card's allow-listed band-1 set and the added rows are modelled
                // identically (an ordinary float arc in the existing satellite
                // phase-bias family), so the allow-list is ignored and only the sigma
                // scale still reads, over every non-canonical phase row.
                log->info("Network (card SID phase 6b): network.second_phase_channel is "
                          "SUBSUMED by processing.channel_mode = 'all'.  The widening adds "
                          "every admitted phase attribute of every selected band, which "
                          "contains card SID phase 6a's allow-listed band-1 set, so "
                          "network.second_phase_signals is IGNORED and "
                          "network.second_phase_sigma_scale ({:.3f}) applies to every "
                          "NON-CANONICAL phase row instead.",
                          network_config_.second_phase_sigma_scale);
            } else if (network_config_.ambiguity_vehicle != NetworkAmbiguityVehicle::FloatKf
                       || !network_phase_datum_held_basis_) {
                log->error("Network (card SID phase 6a): network.second_phase_channel "
                           "requires network.ambiguity_vehicle = 'float_kf' with "
                           "network.phase_datum_held_basis — on the LEVELING vehicle a "
                           "second phase per band would create a zero-q level for every "
                           "extra arc, and no arm exists there (standing invariant #1: a "
                           "stiff time-linked quantity needs a wired event response "
                           "before it runs).  DISARMED.");
                network_config_.second_phase_channel = false;
            } else if (network_config_.second_phase_signals.size() < 2) {
                log->error("Network (card SID phase 6a): network.second_phase_channel is "
                           "armed but network.second_phase_signals holds {} admitted "
                           "entr(y/ies) — a second phase needs at least two band-1 phases "
                           "in the list to ever be one.  DISARMED.",
                           network_config_.second_phase_signals.size());
                network_config_.second_phase_channel = false;
            } else {
                if (!network_config_.slip_qc.gap_reentry_per_signal) {
                    network_config_.second_phase_warn_no_per_signal = true;
                    log->warn("Network (card SID phase 6a): network.second_phase_channel is "
                              "armed WITHOUT network.cycle_slip.gap_reentry_per_signal.  The "
                              "row-level gap re-entry tier keys on the tracker's ONE signal "
                              "per band, so a second-phase arc's re-entry is answered by the "
                              "per-signal tier and by nothing else at the row level; without "
                              "it the only response left is the post-fit T4 attribution "
                              "(standing invariant #1).  Armed anyway, counted, and reported "
                              "at run end.");
                }
                if (network_cycle_slip_if_phase_td_
                    && network_config_.cycle_slip_if_phase_td_pairs
                           != NetworkIfPhaseTdPairs::All) {
                    network_config_.second_phase_warn_iftd_pairs = true;
                    log->warn("Network (card SID phase 6a): network.second_phase_channel is "
                              "armed with network.cycle_slip.if_phase_td on but "
                              "if_phase_td_pairs is NOT 'all'.  That detector forms ONE "
                              "ionosphere-free pair out of the first two phase signals, "
                              "and with two band-1 phases present those two share a "
                              "frequency: the pair is degenerate, so the detector produces "
                              "NO rows at all for such a satellite.  Set if_phase_td_pairs "
                              "to 'all' (the working bases do) or the IF-TD tier is silent "
                              "on this posture (standing invariant #4).");
                }
                log->info("Network (card SID phase 6a): SECOND PHASE CHANNEL armed — a "
                          "station contributes every allow-listed band-1 phase it reports "
                          "[{}] besides the one its canonical latch selected, each as an "
                          "ordinary float ambiguity joining the EXISTING satellite "
                          "phase-bias family of that signal.  No receiver inter-signal "
                          "phase state (a float ambiguity absorbs a constant per arc, so "
                          "one would sit in the null space) and no new datum row; the "
                          "anchor election simply gains candidates.  Phase sigma x {:.3f} "
                          "on the added rows.",
                          [&] {
                              std::string s;
                              for (const auto& sig : network_config_.second_phase_signals) {
                                  if (!s.empty()) s += ", ";
                                  s += sig;
                              }
                              return s;
                          }(),
                          network_config_.second_phase_sigma_scale);
            }
        }
        // Card SID phase 6b (R7): `processing.channel_mode: all` on the NETWORK path.
        // Checked here, beside cards SCC and SID phase 6a, because it subsumes both and
        // because the vehicle and the datum knobs are final at this point.  A refused
        // interlock CLAMPS the mode back to 'single' rather than failing the load: the
        // posture is then exactly the historical one, which is a valid run.
        if (channel_mode_ == ChannelMode::All
            && processing_mode_ == ProcessingMode::PPP_RTK_Network) {
            if (network_config_.ambiguity_vehicle != NetworkAmbiguityVehicle::FloatKf
                || !network_phase_datum_held_basis_) {
                log->error("Network (card SID phase 6b): processing.channel_mode = 'all' "
                           "requires network.ambiguity_vehicle = 'float_kf' with "
                           "network.phase_datum_held_basis — on the LEVELING vehicle every "
                           "extra phase channel would create its own zero-q level with no "
                           "wired event response (standing invariant #1), and card SID "
                           "phase 6a's interlock says the same thing about one extra "
                           "channel.  CLAMPED to 'single'.");
                channel_mode_ = ChannelMode::Single;
            } else {
                log->info("Network (card SID phase 6b): ALL-CHANNEL widening armed — every "
                          "ADMITTED code and phase attribute of every selected band is "
                          "modelled at every station.  Each extra PHASE is an ordinary "
                          "float ambiguity joining the existing satellite phase-bias family "
                          "of its signal (phase sigma x {:.3f}); each extra CODE is a code "
                          "row routed through the receiver inter-signal state "
                          "<system>_ISC_<signal> on ANY band digit (code sigma x {:.3f}).  "
                          "The network coverage gate network.signal_min_station_fraction = "
                          "{:.3f} decides which attributes are admitted at all ({} = every "
                          "tracked channel).  The Melbourne-Wubbena detector's own channel "
                          "mode is DECOUPLED: processing.mw_channel_mode = '{}'.",
                          network_config_.second_phase_sigma_scale,
                          network_config_.second_code_sigma_scale,
                          network_signal_min_station_fraction_,
                          network_signal_min_station_fraction_ > 0.0 ? "above 0 drops sparse "
                                                                       "channels network-wide; 0"
                                                                     : "0",
                          mw_channel_mode_ == MwChannelMode::Single ? "single"
                              : (mw_channel_mode_ == MwChannelMode::All ? "all" : "mirror"));
                if (!network_config_.sat_isc_state)
                    log->warn("Network (card SID phase 6b): channel_mode 'all' is armed with "
                              "network.sat_isc_state OFF.  A companion CODE on band 2 or "
                              "above then keeps the RF-0d geometry-free pin, which ASSERTS "
                              "an inter-signal correction of ZERO on that column and pushes "
                              "the real difference into the satellite family — card B5D's "
                              "class, measured at metres on a third band.  Band-1 companions "
                              "are unaffected (no geometry-free combination exists against "
                              "the reference, so they are left estimable already).  Counted "
                              "at run end.");
                if (!network_carry_rx_code_bias_)
                    log->warn("Network (card SID phase 6b): channel_mode 'all' is armed "
                              "WITHOUT network.carry_rx_code_bias.  The receiver "
                              "inter-signal code states the widening creates are receiver "
                              "code biases like any other and cold-start every epoch without "
                              "that carry, determining themselves from one epoch of code "
                              "(standing invariant #1).  Card SCC refuses its own knob on "
                              "this ground.");
            }
        }
        // Card S2H (docs/SPEC_level_information_budget.md §17 item 3 / §27 item 1):
        // the S-consistent harvest.  Validated BEFORE the LV-2/LV-5 guards because it
        // SUPERSEDES them: both mechanisms act on the same channel (the carried
        // rx-CB common mode) and composing them would de-mean a set that is already
        // zero-mean-by-construction — the LV-2 μ would measure numerical noise and
        // its fold-side subtraction would re-introduce exactly the lag this card
        // removes.  Same post-carry-guard placement as the LV cards.
        if (network_rx_cb_harvest_demean_) {
            if (!(network_rx_cb_harvest_screen_z_ > 0.0)
                || !std::isfinite(network_rx_cb_harvest_screen_z_)) {
                log->error("Network: network.rx_cb_harvest_screen_z must be a finite "
                           "value > 0 (got {}); the peer screen is a contamination "
                           "guard on the μ vote — set it large to effectively disable "
                           "it, never non-positive.",
                           network_rx_cb_harvest_screen_z_);
                return false;
            }
            // The vote floor is the shared LV knob; LV-2's own >= 1 validation only
            // runs when LV-2 is armed, so repeat it here for the S2H-only posture.
            if (network_rx_cb_carry_demean_min_stations_ < 1) {
                log->error("Network: network.rx_cb_carry_demean_min_stations must be "
                           ">= 1 (got {}); it is the S2H vote floor below which the "
                           "harvest transform stands down (identity).",
                           network_rx_cb_carry_demean_min_stations_);
                return false;
            }
            if (!network_carry_rx_code_bias_) {
                log->warn("Network (S2H): network.rx_cb_harvest_demean is ARMED but the "
                          "rx-CB carry is OFF — there is no harvested set to transform, "
                          "so this knob is INERT (standing invariant #4; the run-end s2h "
                          "line will read 0 epochs).  Arm network.carry_rx_code_bias, or "
                          "drop the harvest de-mean.");
                network_rx_cb_harvest_demean_ = false;
            } else {
                if (network_rx_cb_carry_demean_ || network_rx_cb_carry_demean_within_) {
                    log->warn("Network (S2H): network.rx_cb_carry_demean{} is armed "
                              "alongside network.rx_cb_harvest_demean — SUPERSEDED "
                              "mechanism on the same channel, forced OFF.  The harvest "
                              "transform makes the carried set zero-mean-by-construction "
                              "each epoch (frame re-derived from THIS epoch's solution); "
                              "the fold-side de-mean would then measure noise and re-pin "
                              "the mode to the frozen seed frame (SPEC §13.2).",
                              network_rx_cb_carry_demean_within_ ? " (+_within)" : "");
                    network_rx_cb_carry_demean_ = false;
                    network_rx_cb_carry_demean_within_ = false;
                }
                log->info("Network (S2H, SPEC_level_information_budget §17 item 3): "
                          "S-CONSISTENT HARVEST on — at the serial post-solve harvest "
                          "(after the LV-4 erase pass and the gap prune, so the member "
                          "set is exactly the fold-eligible set), the Λ-weighted mean "
                          "of each family's harvested ABSOLUTE realizations is "
                          "subtracted from every carried entry, and the variance leg is "
                          "the exact diagonal of S·Q·Sᵀ (voters var − 1/Σλ, screened "
                          "var + 1/Σλ).  The carried set is zero-mean-by-construction "
                          "in the SAME absolute frame the rx-CB datum row targets, so "
                          "the carry exerts zero net common-mode force and transports "
                          "only between-station differences; the frame is re-derived "
                          "per epoch (no frozen pin, no lag-1 feedback, nothing fed "
                          "back).  LASER Net3DLsa big-system S-transform, Λ-generalized "
                          "(λ ≡ const reduces to the GPS3710_test_PMINOLESS oracle "
                          "case).  Peer screen z = {} (μ-vote contamination guard); "
                          "vote floor {} stations (below it: IDENTITY transform, "
                          "counted — never a frozen μ).  This changes what carried "
                          "state MEANS, so it is appended to the snapshot fingerprint — "
                          "never flip it mid-chain.",
                          network_rx_cb_harvest_screen_z_,
                          network_rx_cb_carry_demean_min_stations_);
            }
        }
        // Card LV-2 (docs/SPEC_level_information_budget.md): rx-CB carry common-mode
        // de-mean.  Validated AFTER the carry's own guard above, which may have forced
        // the carry off — a de-mean of a disarmed carry is INERT, not armed.
        if (network_rx_cb_carry_demean_) {
            if (network_rx_cb_carry_demean_min_stations_ < 1) {
                log->error("Network: network.rx_cb_carry_demean_min_stations must be >= 1 "
                           "(got {}); it is the floor below which μ FREEZES rather than "
                           "trusting a thin mean.",
                           network_rx_cb_carry_demean_min_stations_);
                return false;
            }
            if (!network_carry_rx_code_bias_) {
                log->warn("Network (LV-2): network.rx_cb_carry_demean is ARMED but the rx-CB "
                          "carry is OFF — there are no carry folds to de-mean, so this knob is "
                          "INERT (standing invariant #4; the run-end lv2 line will read 0 "
                          "folds).  Arm network.carry_rx_code_bias, or drop the de-mean.");
                network_rx_cb_carry_demean_ = false;
            } else {
                log->info("Network (LV-2, SPEC_level_information_budget): rx-CB carry "
                          "COMMON-MODE DE-MEAN on — every fold injects η = λ·(innov − μ_c) "
                          "with μ_c = Σλ·innov / Σλ over ALL stations of the constellation, "
                          "computed at the serial post-solve harvest and applied LAG-1, and "
                          "the 10σ innovation gate judges |innov − μ_c|.  Σλ(innov−μ) = 0 in "
                          "the fold's DELTA coordinates: the carry transports between-station "
                          "DIFFERENCES at full stiffness (arm 14 proved that rigidity "
                          "load-bearing) and stops re-asserting LAST epoch's common-mode "
                          "realization.  NOTE what it does NOT do (measured 2026-08-05): it "
                          "does not leave the common mode free.  The de-mean moves the RHS, "
                          "never the information, so the carry still puts ~100 units on that "
                          "direction — in ABSOLUTE terms it now pins the receiver-DCB common "
                          "mode to mean(x_lin), the FROZEN first-epoch code-bias seed frame. "
                          "A rolling memory is swapped for a frozen one, and the designed "
                          "authority (datum 0.01, a₀₀ anchor 0.25) is still outvoted ~400:1. "
                          "Fewer than {} contributing "
                          "stations in an epoch FREEZES μ (thin-mean guard, invariant #7).  "
                          "This changes what carried state MEANS, so it is appended to the "
                          "snapshot fingerprint — never flip it mid-chain.",
                          network_rx_cb_carry_demean_min_stations_);
            }
        }
        // Card LV-5 (docs/SPEC_level_information_budget.md §12): the within-epoch μ.
        // Validated after LV-2's own guard, which may have forced the de-mean off.
        if (network_rx_cb_carry_demean_within_) {
            if (!network_rx_cb_carry_demean_) {
                log->warn("Network (LV-5): network.rx_cb_carry_demean_within is ARMED but "
                          "the de-mean itself is OFF — there is no μ to compute early, so "
                          "this knob is INERT (standing invariant #4).  Arm "
                          "network.rx_cb_carry_demean, or drop the within-epoch knob.");
                network_rx_cb_carry_demean_within_ = false;
            } else {
                log->info("Network (LV-5, SPEC_level_information_budget §12): the rx-CB "
                          "de-mean μ is reduced WITHIN the epoch — serially between PASS A "
                          "and PASS B, over the stations present THIS epoch, so PASS B folds "
                          "against the current μ instead of last epoch's.  Both terms of the "
                          "carry innovation (the stashed absolute posterior and the frozen "
                          "x_lin) are already known before PASS B, so the lag was never "
                          "necessary — and it is not harmless: its loop gain scales with how "
                          "much carry information folds, which limit-cycled the level (a₀₀ "
                          "settled sd 4.98 → 15.53 TECU, ~3-epoch period) as soon as the "
                          "LV-4 screen admitted the full carry.  The post-solve harvest still "
                          "reduces the FOLDED set's mean, now as a diagnostic: it is the "
                          "residual common-mode force that survived the correction.");
            }
        }
        // Card LV-4 (docs/SPEC_level_information_budget.md §11): the invariant carry
        // screen.  Same post-carry-guard placement as LV-2 — a screen on a disarmed
        // carry is INERT, not armed.
        if (network_rx_cb_carry_step_gate_) {
            if (!(network_rx_cb_carry_step_k_sigma_ > 0.0)) {
                log->error("Network: network.rx_cb_carry_step_k_sigma must be > 0 (got {}); "
                           "a non-positive threshold erases every carried receiver code bias "
                           "on every epoch.", network_rx_cb_carry_step_k_sigma_);
                return false;
            }
            if (!network_carry_rx_code_bias_) {
                log->warn("Network (LV-4): network.rx_cb_carry_step_gate is ARMED but the "
                          "rx-CB carry is OFF — there is nothing to screen, so this knob is "
                          "INERT (standing invariant #4).  Arm network.carry_rx_code_bias, "
                          "or drop the step gate.");
                network_rx_cb_carry_step_gate_ = false;
            } else {
                log->info("Network (LV-4, SPEC_level_information_budget §11): rx-CB carry "
                          "screened on the FRAME-INVARIANT step.  The legacy inject test "
                          "|value_abs − x_lin(cb)| is DISABLED (gap + variance sanity remain) "
                          "because x_lin is a frozen linearization point: that statistic "
                          "measures where the first-epoch seed landed, not what the receiver "
                          "did — it withheld 48-67 % of all folds for whole runs on DOY-129 "
                          "with no receiver event, and it is structurally blind to a jump "
                          "between two epochs (at inject the carried value is still pre-jump "
                          "and x_lin has not moved).  The event response now lives at the "
                          "SERIAL harvest, where both the fresh posterior and the carried "
                          "value are ABSOLUTE and their difference is invariant under any "
                          "reframing: z = |Δ| / sqrt(var_new + var_prev + q·Δt + 0.25) > {} "
                          "ERASES the entry (reset-not-repair: the post-jump posterior was "
                          "solved in the epoch that folded a stale prior, so it is suspect "
                          "too), and the next harvest re-seeds it cold.  The run-end line "
                          "also reports what the OLD gate WOULD have withheld, so the A/B is "
                          "visible inside this one run.  NOTE k and the 0.5 m guard are "
                          "inherited from the legacy gate and are NOT recalibrated for the "
                          "step statistic (detection floor ~5 m) — read the reset count.",
                          network_rx_cb_carry_step_k_sigma_);
                // MEASURED PAIRING GUARDS (docs/SPEC_level_information_budget.md §11.3,
                // §12.3).  This screen roughly TRIPLES how much carry information folds
                // (43 % → 97 % on the DOY-129 EXT-CB arm).  That is the point, but it
                // also triples the loop gain of whatever handles the carry's common
                // mode, and both failure modes below were measured, not imagined.
                if (!network_rx_cb_carry_demean_)
                    log->warn("Network (LV-4): the invariant screen is armed WITHOUT "
                              "network.rx_cb_carry_demean.  It admits ~3x more carry "
                              "information (measured 43 % → 97 % folded), and with no "
                              "de-mean ALL of that arrives with its common mode intact — "
                              "the carry then owns the receiver-DCB frame outright "
                              "(~100 information units against the datum's 0.01).  Arm "
                              "network.rx_cb_carry_demean with it.");
                else if (!network_rx_cb_carry_demean_within_)
                    log->warn("Network (LV-4): the invariant screen is armed with a LAG-1 "
                              "de-mean (network.rx_cb_carry_demean_within is off).  "
                              "MEASURED on this lineage: at full admission the lag-1 loop "
                              "LIMIT-CYCLES the ionosphere level — a₀₀ settled sd 4.98 → "
                              "15.53 TECU on a ~3-epoch period, range 54 TECU — because the "
                              "loop gain scales with the folded fraction.  Arm "
                              "network.rx_cb_carry_demean_within (sd back to 4.69, level on "
                              "the GIM reference) or expect the oscillation.");
            }
        }
        // Card LV-1 (docs/SPEC_level_information_budget.md): bias-free per-LOS iono
        // seeding.  Validated here because estimate_iono_sh / the SH block are
        // populated by this point.
        if (network_iono_seed_field_frame_) {
            if (network_iono_seed_field_frame_min_sats_ < 2) {
                log->error("Network: network.iono_seed_field_frame_min_sats must be >= 2 "
                           "(got {}); a median over fewer than two seeds is the seed itself, "
                           "which would reframe every station to its own single channel.",
                           network_iono_seed_field_frame_min_sats_);
                return false;
            }
            if (!estimate_network_iono_sh_) {
                log->warn("Network (LV-1): network.iono_seed_field_frame is ARMED but "
                          "network.estimate_iono_sh is OFF — there is no field to seed INTO, "
                          "so the reframe is INERT (standing invariant #4; the run-end lv1 "
                          "line will read 0 reframed).  Arm the SH-VTEC field, or drop the "
                          "knob.");
                network_iono_seed_field_frame_ = false;
            } else {
                log->info("Network (LV-1, SPEC_level_information_budget): per-LOS iono seeds "
                          "reframed into the FIELD's frame at allocation — seed' = seed_raw − "
                          "median_r(seed_raw) + m(el)·VTEC_SH(IPP), median over >= {} of this "
                          "station's satellites this epoch.  The raw seed carries the "
                          "receiver's own GF hardware bias (the modeled term removes the "
                          "satellite side and the geometry, not the receiver DCB), so every "
                          "seed of a station is born in a bias-in frame and the per-LOS "
                          "ensemble's stiffness toward it (~0.04 info x N_LOS) dwarfs the a₀₀ "
                          "anchor (0.25) and the rx-DCB datum common mode (0.01).  The field "
                          "is the LAG-1 solved SH field (serial post-solve snapshot; the "
                          "parallel produce phase only reads it); at cold start the a₀₀ "
                          "climatology prior mean ({:.2f} TECU) stands in — the designed level "
                          "authority.  Inside the seed path deliberately (invariant #1): slip "
                          "refloats and gap re-births inherit the CURRENT frame.  The removed "
                          "per-station offset is logged once per station — it is a direct "
                          "estimate of that receiver's GF bias.",
                          network_iono_seed_field_frame_min_sats_,
                          global_vtec_sh_.mean_vtec_prior_tecu);
            }
        }
        if (network_carry_zwd_) {
            if (network_zwd_carry_q_ < 0.0) {
                log->error("Network: network.zwd_carry_q must be >= 0 (got {}); a negative carry "
                           "process noise is meaningless.", network_zwd_carry_q_);
                return false;
            }
            if (network_phase_leveling_)
                log->info("Network producer (ZWD carry, B9): carrying per-station tropospheric ZWD "
                          "across epochs with random-walk q {:.3e} m²/s (var_eff = var + q·Δt); slant "
                          "iono left white.  Reuses the phase-leveling warmup ({} epochs) / max-gap "
                          "({:.0f} s) housekeeping; non-pivot stations only.",
                          network_zwd_carry_q_, network_phase_leveling_warmup_epochs_,
                          network_phase_leveling_max_gap_s_);
            else
                log->warn("Network: network.carry_zwd is on but network.phase_leveling is off — the "
                          "ZWD carry reuses the phase-leveling warmup gate, so it is INERT without "
                          "phase-leveling.  Enable network.phase_leveling too.");
        }
        // B9-in-block interlocks.  Armed-and-inert is forbidden (invariant #4: a run-end
        // "0 ZWD columns" must mean the mechanism found nothing, never that the knob could
        // not act), so a miswired arm is DISABLED here rather than left silently dead.
        if (network_carry_zwd_in_block_) {
            if (!network_carry_zwd_) {
                log->warn("Network: network.carry_zwd_in_block is on but network.carry_zwd is OFF — "
                          "there is no carried ZWD to put in the block (the sub-knob only changes "
                          "the VEHICLE the B9 carry folds through, it never creates one).  "
                          "DISABLING carry_zwd_in_block; arm network.carry_zwd too.");
                network_carry_zwd_in_block_ = false;
            } else if (!network_phase_leveling_local_block_) {
                log->warn("Network: network.carry_zwd_in_block is on but "
                          "network.phase_leveling_local_block (F3) is OFF — there is no station "
                          "block to put the ZWD column into, so the knob could only ever fall back "
                          "to the diagonal B9 row it exists to replace.  DISABLING "
                          "carry_zwd_in_block; arm network.phase_leveling_local_block too.");
                network_carry_zwd_in_block_ = false;
            } else if (!network_phase_leveling_ || network_carry_invariant_
                       || !network_carry_conditional_coupling_
                       || network_carry_coupling_joint_whiten_) {
                // F3 is ARMED but structurally cannot build a block on this posture (the
                // guard above already warned about F3 itself).  Riding an inert vehicle is
                // the same armed-and-inert failure, so this dies with it rather than
                // reporting 0 columns from a knob that never had a block to join.
                log->warn("Network: network.carry_zwd_in_block is on and "
                          "network.phase_leveling_local_block is armed, but F3 cannot build a "
                          "block on this lineage (phase_leveling={}, carry_invariant={}, "
                          "carry_conditional_coupling={}, carry_coupling_joint_whiten={}) — the "
                          "ZWD column would have no block to join.  DISABLING "
                          "carry_zwd_in_block (see the phase_leveling_local_block INERT warning "
                          "above for the fix).",
                          network_phase_leveling_, network_carry_invariant_,
                          network_carry_conditional_coupling_,
                          network_carry_coupling_joint_whiten_);
                network_carry_zwd_in_block_ = false;
            } else {
                log->info("Network producer (ZWD carry IN BLOCK): the carried ZWD column joins the "
                          "station's F3 whitened local block (harvested (N_rr^-1) cross terms "
                          "against every carried ambiguity column, one Cholesky for the whole "
                          "station) instead of folding as a diagonal coupling row outside it — the "
                          "station-common ZWD direction is then asserted ONCE per epoch, not once "
                          "per carried row.  Exempt from the F3 young-column age gate ({} ep): a "
                          "ZWD is not an arc.  A non-SPD block degrades this station-epoch to the "
                          "B9 diagonal row (counted).  RW q {:.3e} m²/s.",
                          network_phase_leveling_local_block_min_age_ep_, network_zwd_carry_q_);
            }
        } else if (network_carry_zwd_ && network_phase_leveling_local_block_) {
            // The converse of the interlocks above, and the only combination the ledger calls a
            // configuration error rather than a conservative choice: an F3 lineage that carries
            // ZWD on the DIAGONAL vehicle.  Log-only — no behaviour change (a promotion of the
            // default is a fingerprint-migration card, not a loader edit).
            log->warn("Network: RECOMMEND network.carry_zwd_in_block = true with "
                      "network.carry_zwd under network.phase_leveling_local_block (the diagonal "
                      "ZWD row inside an F3 station is the falsified double count; ledger "
                      "2026-09-10).  The station-common ZWD direction is already asserted by "
                      "every carried ambiguity row of that station, so the diagonal row asserts "
                      "it ~n_LOS times over: quiet-sim consumer 3D rms 0.723 m diagonal vs "
                      "0.0877 m in-block, field T3c consensus 7619 flags vs 849 (SPEC_amb_iono_"
                      "split_overcount §6.16.7d, arms Q/Z/F/F2).");
        }
        // I-in-block interlocks (network.carry_iono_in_block) — the ZWD card's rules with
        // one addition that is a HARD ERROR rather than a warn: the legacy DIAGONAL
        // network.carry_iono is not a different setting of the same mechanism, it is the
        // very over-count this card removes (QUEUE 8b, falsified ×4).  Running both would
        // assert each LOS's iono direction THREE times per epoch (B band priors + the
        // diagonal row + the block row) and no run-end counter could tell the two vehicles
        // apart, so it is refused at load rather than logged.
        if (network_carry_iono_in_block_q_m2_ < 0.0)
            throw std::runtime_error("network.carry_iono_in_block_q_m2 must be >= 0");
        if (network_carry_iono_in_block_max_gap_s_ < 0.0)
            throw std::runtime_error("network.carry_iono_in_block_max_gap_s must be >= 0 "
                                     "(0 = use network.phase_leveling_max_gap_s)");
        if (network_carry_iono_in_block_ && network_carry_iono_)
            throw std::runtime_error(
                "network.carry_iono_in_block and network.carry_iono are MUTUALLY "
                "EXCLUSIVE vehicles for the same carried quantity: carry_iono folds each "
                "LOS as an independent DIAGONAL prior row (the B-fold over-count of "
                "SPEC_amb_iono_split_overcount §7 item 17, FALSIFIED ×4 in QUEUE 8(b)), "
                "while carry_iono_in_block folds the same columns inside the F3 "
                "jointly-whitened block where the harvested cross terms subtract that "
                "overlap exactly.  Arm exactly one.");
        // Resolve the max-gap sentinel BEFORE the disable branches, so the value the
        // getter reports is always effective seconds even on a disabled arm.
        if (network_carry_iono_in_block_max_gap_s_ <= 0.0)
            network_carry_iono_in_block_max_gap_s_ = network_phase_leveling_max_gap_s_;
        if (network_carry_iono_in_block_) {
            if (!network_phase_leveling_) {
                log->warn("Network: network.carry_iono_in_block is on but "
                          "network.phase_leveling is OFF — the in-block iono carry rides "
                          "the phase-leveling carry's harvest/inject seam, so it would "
                          "never see a block.  DISABLING carry_iono_in_block; arm "
                          "network.phase_leveling too.");
                network_carry_iono_in_block_ = false;
            } else if (network_carry_invariant_) {
                log->warn("Network: network.carry_iono_in_block is on but "
                          "network.carry_invariant is ON — the Card-4 invariant DD carry "
                          "is a different VEHICLE and makes F3 structurally inert (no "
                          "ambiguity carry, no local block), so the iono columns would "
                          "have nothing to join.  DISABLING carry_iono_in_block.");
                network_carry_iono_in_block_ = false;
            } else if (!network_phase_leveling_local_block_) {
                log->warn("Network: network.carry_iono_in_block is on but "
                          "network.phase_leveling_local_block (F3) is OFF — there is no "
                          "station block to put the iono columns into, and this card "
                          "deliberately has NO diagonal fallback row (a diagonal iono "
                          "prior IS the falsified network.carry_iono vehicle).  DISABLING "
                          "carry_iono_in_block; arm network.phase_leveling_local_block too.");
                network_carry_iono_in_block_ = false;
            } else if (!network_carry_conditional_coupling_
                       || network_carry_coupling_joint_whiten_) {
                // F3 is ARMED but structurally cannot build a block on this posture.
                // Riding an inert vehicle is the same armed-and-inert failure the ZWD
                // card refuses (invariant #4), so this dies with it.
                log->warn("Network: network.carry_iono_in_block is on and "
                          "network.phase_leveling_local_block is armed, but F3 cannot "
                          "build a block on this lineage (carry_conditional_coupling={}, "
                          "carry_coupling_joint_whiten={}) — the iono columns would have "
                          "no block to join.  DISABLING carry_iono_in_block (see the "
                          "phase_leveling_local_block INERT warning above for the fix).",
                          network_carry_conditional_coupling_,
                          network_carry_coupling_joint_whiten_);
                network_carry_iono_in_block_ = false;
            } else {
                log->info("Network producer (slant-iono carry IN BLOCK): each station's "
                          "present per-LOS iono columns join its F3 whitened local block "
                          "(harvested (N_rr^-1) cross terms against every other carried "
                          "column of the station, one Cholesky for the whole station) "
                          "instead of the FALSIFIED diagonal network.carry_iono row — the "
                          "per-LOS iono direction is then asserted ONCE per epoch on top "
                          "of the B band priors, not twice.  ABSOLUTE value carried "
                          "(x_lin + x_r).  RW q {:.3e} m² PER EPOCH at L1 (⚠ per epoch, "
                          "not per second), stale drop at {:.0f} s{}.  No var floor, no "
                          "F1 los_scale, no F3 young-column age gate (first sighting "
                          "already carries nothing).  A non-SPD block folds NO iono this "
                          "station-epoch (counted) — there is deliberately no diagonal "
                          "fallback row.",
                          network_carry_iono_in_block_q_m2_,
                          network_carry_iono_in_block_max_gap_s_,
                          network_carry_iono_in_block_max_gap_s_
                                  == network_phase_leveling_max_gap_s_
                              ? " (= phase_leveling_max_gap_s)" : "");
            }
        }
        // AGE-AWARE young-arc inflate interlocks (network.phase_leveling_young_inflate_
        // var_m2).  The knob acts at the phase-leveling carry's INJECT site, so it needs
        // that carry to be the live vehicle: phase_leveling on, carry_invariant off
        // (Card 4's DD carry supersedes the leveling inject outright — `do_amb_carry` is
        // false and there is no column to inflate).  F3 is deliberately NOT required:
        // in-block the addition is a congruence scaling of Σ, out of block a plain
        // var_eff += var_add on the per-column row, and both are wired.
        if (network_phase_leveling_young_inflate_var_m2_ > 0.0) {
            if (!network_phase_leveling_) {
                log->warn("Network: network.phase_leveling_young_inflate_var_m2 is set "
                          "({:.4g} m²) but network.phase_leveling is OFF — the inflation "
                          "acts on the phase-leveling carry's inject, so there is no "
                          "carried column to inflate.  DISABLING the young-arc inflate; "
                          "arm network.phase_leveling too.",
                          network_phase_leveling_young_inflate_var_m2_);
                network_phase_leveling_young_inflate_var_m2_ = 0.0;
            } else if (network_carry_invariant_) {
                log->warn("Network: network.phase_leveling_young_inflate_var_m2 is set "
                          "({:.4g} m²) but network.carry_invariant is ON — the Card-4 "
                          "invariant DD carry supersedes the leveling ambiguity inject "
                          "(do_amb_carry is false), so no leveling column is injected and "
                          "the inflation would be ARMED AND INERT.  DISABLING the "
                          "young-arc inflate.",
                          network_phase_leveling_young_inflate_var_m2_);
                network_phase_leveling_young_inflate_var_m2_ = 0.0;
            } else {
                log->info("Network producer (young-arc inflate): AGE-AWARE variance "
                          "inflation of freshly RE-LEVELLED leveling arcs ARMED — a "
                          "carried ambiguity column whose LAST RE-LEVEL (creation, or a "
                          "relevel_free_epoch response) is younger than {} loop epoch(s) "
                          "gets V0*exp(-age/tau) added to its prior variance, V0 = "
                          "{:.4g} m² (sigma {:.3f} m, the CODE-level error of a one-epoch "
                          "re-level) with tau = {:.4g} epoch(s).  Motive "
                          "(SPEC_amb_iono_split_overcount 6.17-6.18): the re-stashed "
                          "level's VALUE is code-quality while its harvested VARIANCE is "
                          "phase-stiff (conditioned on the station's mature arcs), so the "
                          "T4 tier erases an arc that is merely UNREFINED and the "
                          "erase-re-level treadmill sustains a metre-class ZWD/clock "
                          "plateau.  {}  Age is counted in LOOP EPOCHS using the run's "
                          "OWN observed inter-epoch spacing.  Run-end line 'Network "
                          "(young-arc inflate)' is the ON-path telemetry.",
                          network_phase_leveling_young_inflate_max_age_ep_,
                          network_phase_leveling_young_inflate_var_m2_,
                          std::sqrt(network_phase_leveling_young_inflate_var_m2_),
                          network_phase_leveling_young_inflate_tau_ep_,
                          network_phase_leveling_local_block_
                              ? "IN BLOCK (phase_leveling_local_block on): applied as a "
                                "CONGRUENCE scaling of the arc's row/column of the "
                                "harvested Sigma before the whitening, so Sigma stays PD "
                                "and every correlation coefficient is EXACTLY unchanged "
                                "-- only the arc's own marginal loosens."
                              : "OUT OF BLOCK (phase_leveling_local_block off): applied "
                                "as var_eff += var_add on the per-column diagonal row.");
            }
        }
        // RX-CLK-in-block interlocks (network.carry_rx_clock_in_block).  The ZWD card's
        // rules, plus hard errors on the variance ladder: every one of q, rate_var0 and
        // rate_q lands on a Cholesky diagonal, and a negative one would make a non-SPD
        // block look like a data problem several seams away from the typo that caused it.
        if (network_carry_rx_clock_q_m2_ < 0.0)
            throw std::runtime_error("network.carry_rx_clock_q_m2 must be >= 0");
        if (network_carry_rx_clock_max_gap_s_ < 0.0)
            throw std::runtime_error("network.carry_rx_clock_max_gap_s must be >= 0 "
                                     "(0 = use network.phase_leveling_max_gap_s)");
        if (network_carry_rx_clock_rate_var0_ < 0.0)
            throw std::runtime_error("network.carry_rx_clock_rate_var0_m2ps2 must be >= 0");
        if (network_carry_rx_clock_rate_q_ < 0.0)
            throw std::runtime_error("network.carry_rx_clock_rate_q_m2ps2 must be >= 0");
        if (!(network_carry_rx_clock_jump_k_ > 0.0))
            throw std::runtime_error("network.carry_rx_clock_jump_k must be > 0 (it is the "
                                     "sigma multiplier of the clock-jump response; 0 would "
                                     "erase the carry every epoch)");
        // Resolve the max-gap sentinel BEFORE the disable branches, so the value the
        // getter reports is always effective seconds even on a disabled arm.
        if (network_carry_rx_clock_max_gap_s_ <= 0.0)
            network_carry_rx_clock_max_gap_s_ = network_phase_leveling_max_gap_s_;
        if (network_carry_rx_clock_in_block_) {
            if (!network_phase_leveling_) {
                log->warn("Network: network.carry_rx_clock_in_block is on but "
                          "network.phase_leveling is OFF — the in-block clock carry rides "
                          "the phase-leveling carry's harvest/inject seam, so it would "
                          "never see a block.  DISABLING carry_rx_clock_in_block; arm "
                          "network.phase_leveling too.");
                network_carry_rx_clock_in_block_ = false;
            } else if (network_carry_invariant_) {
                log->warn("Network: network.carry_rx_clock_in_block is on but "
                          "network.carry_invariant is ON — the Card-4 invariant DD carry "
                          "is a different VEHICLE and makes F3 structurally inert (no "
                          "ambiguity carry, no local block), so the clock column would "
                          "have nothing to join.  DISABLING carry_rx_clock_in_block.");
                network_carry_rx_clock_in_block_ = false;
            } else if (!network_phase_leveling_local_block_) {
                log->warn("Network: network.carry_rx_clock_in_block is on but "
                          "network.phase_leveling_local_block (F3) is OFF — there is no "
                          "station block to put the receiver-clock column into, and this "
                          "card deliberately has NO diagonal fallback row (a diagonal "
                          "prior on the most station-COMMON direction there is would be "
                          "the n_LOS over-count that falsified the diagonal ZWD carry).  "
                          "DISABLING carry_rx_clock_in_block; arm "
                          "network.phase_leveling_local_block too.");
                network_carry_rx_clock_in_block_ = false;
            } else if (!network_carry_conditional_coupling_
                       || network_carry_coupling_joint_whiten_) {
                // F3 is ARMED but structurally cannot build a block on this posture.
                // Riding an inert vehicle is the same armed-and-inert failure the ZWD and
                // iono cards refuse (invariant #4), so this dies with them.
                log->warn("Network: network.carry_rx_clock_in_block is on and "
                          "network.phase_leveling_local_block is armed, but F3 cannot "
                          "build a block on this lineage (carry_conditional_coupling={}, "
                          "carry_coupling_joint_whiten={}) — the receiver-clock column "
                          "would have no block to join.  DISABLING "
                          "carry_rx_clock_in_block (see the phase_leveling_local_block "
                          "INERT warning above for the fix).",
                          network_carry_conditional_coupling_,
                          network_carry_coupling_joint_whiten_);
                network_carry_rx_clock_in_block_ = false;
            } else {
                log->info("Network producer (receiver CLOCK carry IN BLOCK): each station's "
                          "receiver-clock column{} joins its F3 whitened local block "
                          "(harvested (N_rr^-1) cross terms against every other carried "
                          "column of the station, one Cholesky for the whole station) — "
                          "the per-epoch clock is otherwise a FREE local column with no "
                          "temporal prior (filter.process_noise.clock_bias_q is inert on "
                          "this path), i.e. the rho-direction sink.  ⚠ x_lin(clock) is a "
                          "run-constant here (the producer never writes its solution back), "
                          "so the carry is a random walk about a CARRIED DRIFT, not a bare "
                          "RW: bias q {:.3e} m^2 PER EPOCH (⚠ per epoch, not per second), "
                          "drift q {:.3e} m^2/s^2 per epoch from an initial {:.3e} m^2/s^2 "
                          "(one increment collapses it), injected mean = value_abs + "
                          "rate*gap, injected var = var + q*gap_ep + rate_var*gap^2.  "
                          "Stale drop at {:.0f} s{}; clock-JUMP response at {:.1f} sigma on "
                          "the one-step prediction (erase + cold rebirth, counted).  A "
                          "non-SPD block folds NO clock this station-epoch (counted) — "
                          "there is deliberately no diagonal fallback row.",
                          network_carry_rx_clock_isb_
                              ? " and every per-system ISB column (pure RW, no drift state)"
                              : " (ISB columns NOT carried: network.carry_rx_clock_isb off)",
                          network_carry_rx_clock_q_m2_, network_carry_rx_clock_rate_q_,
                          network_carry_rx_clock_rate_var0_,
                          network_carry_rx_clock_max_gap_s_,
                          network_carry_rx_clock_max_gap_s_
                                  == network_phase_leveling_max_gap_s_
                              ? " (= phase_leveling_max_gap_s)" : "",
                          network_carry_rx_clock_jump_k_);
            }
        }
        if (network_zwd_seam_reseed_) {
            if (network_zwd_seam_reseed_epochs_ <= 0) {
                log->error("Network: network.zwd_seam_reseed_epochs must be > 0 (got {}); a "
                           "non-positive horizon arms a window that can never inject.",
                           network_zwd_seam_reseed_epochs_);
                return false;
            }
            if (network_zwd_carry_q_ < 0.0) {
                log->error("Network: network.zwd_carry_q must be >= 0 (got {}); the seam re-seed "
                           "inflates the restored variance by q*dt.", network_zwd_carry_q_);
                return false;
            }
            if (network_config_.state_snapshot_path.empty())
                log->warn("Network (ZWD seam re-seed): network.zwd_seam_reseed is on but "
                          "network.state_snapshot_path is EMPTY — no warm resume can ever happen, "
                          "so the seam window is structurally INERT on this run.");
            if (!network_phase_leveling_)
                log->warn("Network (ZWD seam re-seed): network.zwd_seam_reseed is on but "
                          "network.phase_leveling is off — the seam inject rides the carry_warm "
                          "(phase-leveling) gate, so it is INERT without phase-leveling.");
            if (network_carry_zwd_)
                log->info("Network (ZWD seam re-seed): network.carry_zwd is ALSO on — B9 already "
                          "injects on every epoch, so the seam window is subsumed (its counters "
                          "still tick during the post-resume horizon).");
            // RS-10b (§21.21): the seam q may now be the window's own, decoupled from
            // the fingerprinted continuous carry q.  Report which one is live and where
            // it came from — this is the ON-path telemetry for the bracket.
            const bool   seam_q_armed = network_zwd_seam_reseed_q_ > 0.0;
            const double seam_q_eff   = seam_q_armed ? network_zwd_seam_reseed_q_
                                                     : network_zwd_carry_q_;
            log->info("Network (ZWD seam re-seed): armed — after a warm resume that restores Z "
                      "rows, the B9 ZWD inject+stash runs for {} emitted epoch(s) with RW q "
                      "{:.3e} m²/s ({}), then retires (the station filter re-establishes its own "
                      "memory; continuous epochs NEVER carry — that path is falsified).  "
                      "Deliberately NOT in the warm-resume fingerprint (forward-only resume "
                      "response policy).",
                      network_zwd_seam_reseed_epochs_, seam_q_eff,
                      seam_q_armed
                          ? "network.zwd_seam_reseed_q — the SEAM's own stiffness, RS-10b; "
                            "network.zwd_carry_q is untouched and still governs any continuous "
                            "carry row"
                          : "inherited from network.zwd_carry_q — network.zwd_seam_reseed_q "
                            "unset");
        }
        if (network_amb_seam_reinflate_var_m2_ != 0.0) {
            if (network_amb_seam_reinflate_var_m2_ < 0.0) {
                log->error("Network: network.amb_seam_reinflate_var_m2 must be >= 0 (got {}); a "
                           "negative variance ADD would deflate the restored carries.",
                           network_amb_seam_reinflate_var_m2_);
                return false;
            }
            if (network_config_.state_snapshot_path.empty())
                log->warn("Network (amb seam re-inflate): network.amb_seam_reinflate_var_m2 is set "
                          "but network.state_snapshot_path is EMPTY — no warm resume can ever "
                          "happen, so the re-inflation is structurally INERT on this run.");
            if (!network_phase_leveling_)
                log->warn("Network (amb seam re-inflate): network.amb_seam_reinflate_var_m2 is set "
                          "but network.phase_leveling is off — there are no local ambiguity "
                          "carries to re-inflate; INERT.");
            log->info("Network (amb seam re-inflate): armed — at a warm resume, every restored "
                      "phase-leveling local ambiguity carry gets +{:.3e} m² variance (one-shot "
                      "soft refloat at the seam; the carried value stays as prior mean, the data "
                      "re-earns the stiffness).  Treats the invariant-#1 drag of h-vintage "
                      "zero-q carries against fresh station filters (SPEC_synthetic_minimum_"
                      "network §18).  Deliberately NOT in the warm-resume fingerprint "
                      "(forward-only resume response policy).",
                      network_amb_seam_reinflate_var_m2_);
        }
        if (network_cycle_slip_if_phase_td_) {
            if (network_phase_leveling_)
                log->info("Network producer (IF-phase slip): between-satellite, between-epoch "
                          "ionosphere-free phase triple-difference slip DETECTION enabled — a "
                          "flagged IF pair refloats BOTH of its band ambiguities (pending-reseed) "
                          "so the carry re-estimates them from this epoch's data.  Threshold is "
                          "elevation-normalized (1/sin el) and per (station, signal-pair): the "
                          "network pool seeds each station-pair, then an EWMA of its own noise "
                          "scale takes over as it accumulates clean samples (shrink κ={:.0f}, "
                          "α={:.2f}).  Non-pivot only; detection-only (no MW/GF confirm, no "
                          "integer repair).",
                          network_cycle_slip_if_phase_td_shrink_kappa_,
                          network_cycle_slip_if_phase_td_ewma_alpha_);
            else if (network_carry_invariant_)
                log->info("Network producer (IF-phase slip): between-satellite, between-epoch "
                          "ionosphere-free phase triple-difference slip DETECTION enabled — a "
                          "flagged IF pair feeds the Card-4 DD invariant-carry C3c slip funnel "
                          "(carry_slipped_ambs), so BOTH of its band ambiguities are "
                          "membership-excluded and re-tapped fresh next epoch (the same path the "
                          "MW/LLI coarse detectors take).  Threshold elevation-normalized "
                          "(1/sin el), per (station, signal-pair); non-pivot only; detection-only "
                          "(no MW/GF confirm, no integer repair).");
            else
                log->info("Network producer (IF-phase slip): detection enabled in DIAGNOSTIC-ONLY "
                          "mode — neither network.phase_leveling nor network.carry_invariant is "
                          "on, so there is no carry to refloat; slips are logged but have no "
                          "product effect (every non-pivot ambiguity is re-estimated free each "
                          "epoch in the absolute solve).");
        }
        // POOL RE-HOST INERT guards (docs/SPEC_raim_pool_rehost.md).  Post-parse, because
        // network_carry_invariant_ is only populated by the eq34 section above.  Follows
        // the eq34-hygiene / M2-v2 precedent: an arm that cannot move a byte is DISABLED
        // here rather than discovered after a field run (standing invariant #4).
        if (network_raim_carry_invariant_ && !network_carry_invariant_) {
            log->warn("Network (RAIM pool re-host): network.raim_carry_invariant is on but "
                      "network.carry_invariant is OFF — the re-host exists to give post-fit "
                      "RAIM a pool on the Card-4 lineage, and off that lineage `pl_amb` is "
                      "already the right pool.  DISABLED (no behaviour change).");
            network_raim_carry_invariant_ = false;
        }
        if (network_raim_carry_invariant_ && !network_raim_) {
            log->warn("Network (RAIM pool re-host): network.raim_carry_invariant is on but "
                      "network.raim is OFF — there is no screen to re-host.  DISABLED.");
            network_raim_carry_invariant_ = false;
        }
        if (network_raim_carry_invariant_refloat_ && !network_raim_carry_invariant_) {
            log->warn("Network (RAIM pool re-host): network.raim_carry_invariant_refloat is on "
                      "but network.raim_carry_invariant is OFF — the actuator has no screen to "
                      "act on (the seam's legacy actions cannot move the Card-4 solve: the "
                      "local_carry erase is a no-op on an empty map and the pending-reseed mark "
                      "leaves the reduced RHS algebraically unchanged).  DISABLED.");
            network_raim_carry_invariant_refloat_ = false;
        }
        if (network_raim_carry_invariant_ && network_raim_withhold_sigma0_ > 0.0)
            log->warn("Network (RAIM pool re-host): the re-host makes σ̂0 REAL on this lineage "
                      "for the first time (with the pool empty it never left its 1.0 "
                      "initializer), and network.raim_withhold_sigma0 = {:.1f} is armed — so "
                      "this run can begin WITHHOLDING epochs, and the emitted URA moves with "
                      "the σ̂0 EWMA.  Neither is behind the refloat knob: a measure-only arm is "
                      "NOT emission-neutral.  Grade it as emission-affecting, or disarm the "
                      "withhold for the first validation leg.",
                      network_raim_withhold_sigma0_);
        if (network_raim_) {
            // ⚠ `!network_carry_invariant_` is load-bearing, and its absence was a live
            // hole.  ApplicationNetworkAssembly's `do_amb_carry` is
            // `(prioritized || !is_pivot) && !carry_invariant`, so under carry_invariant
            // `R.pl_amb` stays empty for EVERY station no matter what phase_leveling
            // says — and `ambPool()` then hands the seam an empty pool, which skips every
            // station.  Without this conjunct a `phase_leveling + carry_invariant +
            // !raim_carry_invariant` run took THIS branch and logged "ENABLED" while
            // screening nothing: T3 reported 0 opportunities / 0 flags and both
            // trace_station_residuals and trace_raim_redundancy wrote header-only files.
            // Measured on a 305-station 20-epoch smoke (2026-08-14) that cost 32 minutes
            // to learn — the exact "reports ARMED and screens NOTHING" class this ladder
            // exists to catch.  The carry_invariant branch below now catches it instead.
            if (network_phase_leveling_ && !network_carry_invariant_
                && !network_raim_carry_invariant_)
                log->info("Network producer (RAIM, Increment 3 / T2): post-fit cross-station "
                          "residual consensus ENABLED — a carried ambiguity whose slip the "
                          "pre-processors missed is caught when the network's OTHER stations "
                          "(pinned to their own carries) cannot absorb its leaked shared-clock "
                          "error and their post-fit residuals spike (>{:.1f}σ); a satellite so "
                          "flagged at >= {} stations is refloated network-wide (pending-reseed). "
                          "The redundancy-based safety net that makes zero-process-noise carry sound.",
                          network_raim_k_sigma_, network_raim_min_stations_);
            else if (network_raim_carry_invariant_)
                log->info("Network producer (RAIM POOL RE-HOST): post-fit RAIM re-hosted onto the "
                          "carry-neutral `qc_amb` census — it now RUNS on the Card-4 "
                          "invariant-carry lineage (phase_leveling={}), where it was previously "
                          "inert twice over (seam not called; `pl_amb` pool never filled).  "
                          "Refloat actuator is {} — {}.",
                          network_phase_leveling_,
                          network_raim_carry_invariant_refloat_ ? "ARMED" : "OFF (measure-only)",
                          network_raim_carry_invariant_refloat_
                              ? "flags queue into raim_refloat_pending and drain into the C3c "
                                "funnel at the next epoch's head"
                              : "flags are counted and traced but reach no carry consumer");
            else if (network_carry_invariant_)
                log->warn("Network: network.raim is on with network.carry_invariant (phase_leveling"
                          "={}) but WITHOUT network.raim_carry_invariant — post-fit RAIM is INERT: "
                          "its residual pool `pl_amb` is never filled under carry_invariant "
                          "(ApplicationNetworkAssembly's `do_amb_carry` requires !carry_invariant, "
                          "because Card 4's DD carry supersedes the phase-leveling ambiguity "
                          "carry), so `ambPool()` hands the seam an EMPTY pool and every station is "
                          "skipped.  ⚠ phase_leveling does NOT rescue this — it satisfies the "
                          "carry_warm CALL-SITE gate only, which is why such a run reports ARMED "
                          "and screens NOTHING: T3 = 0 opportunities, and "
                          "trace_station_residuals / trace_raim_redundancy write header-only "
                          "files.  Arm network.raim_carry_invariant to re-host the pool onto this "
                          "lineage.",
                          network_phase_leveling_);
            else
                log->warn("Network: network.raim is on but network.phase_leveling is off — the "
                          "post-fit RAIM protects the carried ambiguities, so it is INERT without "
                          "phase-leveling.  Enable network.phase_leveling too.");
            if (network_raim_redundancy_normalize_)
                log->info("Network producer (RAIM, B2): Baarda redundancy normalization ENABLED — "
                          "residuals standardized by v/(σ·√r_i) using per-row redundancy from the "
                          "cached Schur factors; low-redundancy setting arcs are no longer under-flagged.");
        }
        // L13: the per-station residual field is emitted only inside the post-fit RAIM cell loop
        // (gated raim_on && carry_warm; carry_warm needs phase_leveling) — warn if armed without
        // its runtime prerequisites so an empty file is not a surprise.
        if (network_config_.trace_station_residuals && !network_raim_)
            log->warn("Network: network.trace_station_residuals is on but network.raim is off — "
                      "the per-station residual field is emitted from the post-fit RAIM cell loop, "
                      "so it will be EMPTY.  Enable network.raim AND network.phase_leveling.");
        // SPEC_t3_null_derivation §7.1: same runtime prerequisite, same failure mode.
        if (network_config_.trace_raim_redundancy && !network_raim_)
            log->warn("Network: network.trace_raim_redundancy is on but network.raim is off — "
                      "the per-row redundancy census is emitted from the post-fit RAIM row loop, "
                      "so it will be EMPTY.  Enable network.raim AND network.phase_leveling.");
        // It is NOT an error to arm the census together with raim_redundancy_normalize —
        // the census then simply reports the r_i the standardization is already using.
        // Say so, because the two knobs differ in solve-effect and that is easy to mix up.
        if (network_config_.trace_raim_redundancy && network_raim_redundancy_normalize_)
            log->info("Network: network.trace_raim_redundancy alongside "
                      "network.raim_redundancy_normalize — the census reports the SAME r_i the "
                      "standardization consumes.  Note the census alone is solve-neutral; the "
                      "normalization is not (it drops rows at the r_i floor).");

        return true;
    }

    // =================================================================
    //  network subsection helper: loadNetworkClosedLoop_
    // =================================================================
    /**
     * @brief Optional closed-loop integer-recovery harness (@c network.closed_loop.*).
     *
     * A held-out consumer station deliberately NOT in @c network.stations, plus the Q5
     * replay path.  Inert unless @c closed_loop.enable.
     *
     * @param nw    The @c network section object (already validated non-null).
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkClosedLoop_(const nlohmann::json& nw,
                                            const std::shared_ptr<spdlog::logger>& log) {
        // --- Optional closed-loop integer-recovery harness (held-out consumer) ---
        // A station deliberately NOT in network.stations that consumes the
        // network's emitted SSR products through the user-side PPP-RTK + AR
        // pipeline (see ClosedLoopConfig).  Inert unless closed_loop.enable.
        network_config_.closed_loop = ClosedLoopConfig{};
        if (nw.contains("closed_loop") && nw["closed_loop"].is_object()) {
            const auto& cl = nw["closed_loop"];
            auto& clc = network_config_.closed_loop;
            clc.enable = cl.value("enable", false);
            if (cl.contains("station") && cl["station"].is_object()) {
                const auto& sj = cl["station"];
                clc.station.name       = sj.value("name", std::string{});
                clc.station.rinex_path = sj.value("rinex_path", std::string{});
                clc.station.fixed      = true;   // truth coordinates, held for scoring
                if (sj.contains("position_ecef") && sj["position_ecef"].is_array()
                    && sj["position_ecef"].size() == 3) {
                    clc.station.position_ecef = {
                        sj["position_ecef"][0].get<double>(),
                        sj["position_ecef"][1].get<double>(),
                        sj["position_ecef"][2].get<double>()
                    };
                    clc.station.has_position = true;
                }
            }
            clc.ar_mode    = cl.value("ar_mode", 1);
            clc.ar_wl_only = cl.value("ar_wl_only", true);
            clc.consume_network_products = cl.value("consume_network_products", true);
            clc.consume_network_clock    = cl.value("consume_network_clock", true);
            clc.consume_network_iono     = cl.value("consume_network_iono", false);
            clc.consume_combined_sigma   = cl.value("consume_combined_sigma", false);  // M4 (contract v2)
            clc.consumer_undifferenced   = cl.value("consumer_undifferenced", false);
            // Card CC: (A) the declared-step ambiguity carry (derived into the
            // consumer engine as processing.ssr_phase_bias_disc_carry) and (B) the
            // consumer snapshot beside each network history copy.  Both default
            // false ⇒ byte-identical; neither reaches the producer's solve or its
            // resume fingerprint.
            clc.pb_disc_carry            = cl.value("pb_disc_carry", false);
            // Card SID phase 5b: the consumer's band latch and its freshness
            // response follow the producer's DECLARED roster (derived into the
            // consumer engine as GnssConfig::consumerRosterLatch).  Same contract
            // as pb_disc_carry: default false, and it never reaches the
            // producer's solve or its resume fingerprint.  INERT on a wire that
            // carries no declaration, which the consumer detects and logs.
            clc.roster_latch             = cl.value("roster_latch", false);
            // Card SID phase 6b (R6): the CONSUMER's own channel mode, separate from
            // the producer's.  A producer at `all` does not widen the consumer unless
            // this key asks for it, so an arm of the producer's widening moves the
            // WIRE alone.  Anything other than "all" reads as "single", which is the
            // historical behaviour of every consumer arm.
            {
                std::string cm = cl.value("channel_mode", std::string("single"));
                std::transform(cm.begin(), cm.end(), cm.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (cm != "single" && cm != "all") {
                    log->warn("Network (card SID phase 6b): network.closed_loop.channel_mode "
                              "'{}' is not 'single' or 'all'; using 'single'.", cm);
                    cm = "single";
                }
                clc.channel_mode = cm;
            }
            // Card PBT (A): the EVERY-step ambiguity translation (derived into the
            // consumer engine as processing.ssr_phase_bias_step_translate).  Same
            // contract as pb_disc_carry — default false ⇒ byte-identical, and it
            // never reaches the producer's solve or its resume fingerprint.
            clc.pb_step_translate        = cl.value("pb_step_translate", false);
            clc.pb_step_translate_eps_m  = cl.value("pb_step_translate_eps_m", 0.005);
            // Card ALC: the consumer's ambiguity LEVEL re-validation (derived into
            // the consumer engine as processing.amb_level_check).  Same contract as
            // pb_step_translate — default false ⇒ byte-identical, and it never
            // reaches the producer's solve or its resume fingerprint.
            clc.amb_level_check           = cl.value("amb_level_check", false);
            clc.amb_level_check_window_ep = cl.value("amb_level_check_window_ep", 20);
            clc.amb_level_check_k         = cl.value("amb_level_check_k", 4.0);
            clc.amb_level_check_mode      = cl.value("amb_level_check_mode", std::string("act"));
            clc.state_snapshot           = cl.value("state_snapshot", false);
            // Q5 replay: consume an already-written product file instead of the
            // live producer.  Presence alone arms the replay driver (the enable
            // flag still gates the station validation below), so a config that
            // sets it is unambiguously a consumer-only run.
            clc.replay_products_path     = cl.value("replay_products_path", std::string{});
            if (cl.contains("ar_active_systems") && cl["ar_active_systems"].is_array()) {
                for (const auto& s : cl["ar_active_systems"])
                    if (s.is_string()) clc.ar_active_systems.push_back(s.get<std::string>());
            }
            if (clc.enable) {
                if (clc.station.name.empty() || clc.station.rinex_path.empty()
                    || !clc.station.has_position) {
                    log->error("Network closed_loop.enable=true but the consumer station is "
                               "incomplete (needs name + rinex_path + position_ecef); disabling.");
                    clc.enable = false;
                } else if (!clc.replay_products_path.empty()) {
                    log->info("Network closed-loop REPLAY (Q5) armed: consumer '{}' vs product "
                              "file '{}' (AR mode {}, wl_only {}) — the joint producer does NOT "
                              "run; runNetwork short-circuits to the consumer-only driver.",
                              clc.station.name, clc.replay_products_path, clc.ar_mode,
                              clc.ar_wl_only ? "on" : "off");
                } else {
                    log->info("Network closed-loop harness ENABLED: held-out consumer '{}' "
                              "(AR mode {}, wl_only {})", clc.station.name, clc.ar_mode,
                              clc.ar_wl_only ? "on" : "off");
                }
            }
        }
        // Card CST sink dependency (deferred from the observability helper, which runs
        // BEFORE this one and so cannot see clc.enable).  Standing invariant #4: an
        // armed instrument whose subject does not exist must SAY so rather than write
        // an empty file that reads as "nothing happened".
        if (network_config_.trace_consumer_sat && !network_config_.closed_loop.enable) {
            log->warn("Network (card CST): network.trace_consumer_sat is ON but the "
                      "closed-loop harness is NOT enabled (network.closed_loop.enable) — "
                      "there is no consumer to read, so no closed_loop_sat_trace.csv will be "
                      "opened and the knob is INERT on this run.");
        }
    }

    // =================================================================
    //  network subsection helper: logNetworkSummary_
    // =================================================================
    /**
     * @brief One-line network roster / datum / parallelism summary.
     *
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::logNetworkSummary_(const std::shared_ptr<spdlog::logger>& log) const {
        log->info("Network mode: {} stations, clock datum station '{}'{}, sync tol {:.3f} s",
                  network_config_.stations.size(),
                  network_config_.clock_datum_station,
                  network_config_.clock_datum_satellite.empty()
                      ? std::string{}
                      : (" + datum sat '" + network_config_.clock_datum_satellite + "'"),
                  network_config_.epoch_sync_tolerance_s);
        if (network_config_.parallel_stations
            && static_cast<int>(network_config_.stations.size()) > network_config_.parallel_min_stations) {
            log->info("Network: per-station compute parallelism ENABLED ({} stations > threshold {}; "
                      "thread cap {})", network_config_.stations.size(),
                      network_config_.parallel_min_stations,
                      network_config_.parallel_max_threads > 0
                          ? std::to_string(network_config_.parallel_max_threads)
                          : std::string("auto"));
        } else {
            log->info("Network: per-station compute runs SERIALLY ({} stations <= threshold {} or "
                      "parallel_stations=false)", network_config_.stations.size(),
                      network_config_.parallel_min_stations);
        }
        if (network_config_.parallel_seam) {
            log->info("Network: deterministic-parallel post-solve seam ENABLED "
                      "(carry-fold + D5 readouts computed per-block in parallel, merged in "
                      "fixed block order — byte-identical to the serial fold)");
        }
        // RF-1 §7.1: the AMBIGUITY VEHICLE, on every run — it is the product's
        // provenance, so it is stated whether or not it is the default.  The
        // "no mechanism" clause is part of the line for as long as steps 3–5 are
        // unlanded: a reader must never infer the mechanism from the knob.
        log->info("Network (RF-1): ambiguity vehicle = \"{}\"{}.  amb_float_birth_var_m2={} "
                  "m^2, amb_float_q_m2s={} m^2/s, amb_float_declared_handover={}, "
                  "trace_amb_state={}{}.",
                  network_config_.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf
                      ? "float_kf" : "level",
                  network_config_.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf
                      ? " — the joint block is STRUCTURAL (RF-1 step 3); solve-affecting"
                      : " — the zero-q scalar carry (today's vehicle)",
                  network_config_.amb_float_birth_var_m2,
                  network_config_.amb_float_q_m2s,
                  network_config_.amb_float_declared_handover,
                  network_config_.trace_amb_state,
                  (network_config_.trace_amb_state
                   && network_config_.trace_amb_state_every > 1)
                      ? fmt::format(" (every {} epoch(s))",
                                    network_config_.trace_amb_state_every)
                      : std::string{});
    }

    // =================================================================
    //  network subsection helper: finalizeNetworkProcessNoise_
    // =================================================================
    /**
     * @brief Settle the scoped process-noise table AFTER every clamp, arming rule and restore.
     *
     * Re-reads the canonical members so the struct — and therefore the run manifest — reports
     * the rate the estimator will ACTUALLY use, and resolves each class's provenance
     * (Legacy / Table / Derived / Default).  @see loadNetworkProcessNoise_.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::finalizeNetworkProcessNoise_(const nlohmann::json& json,
                                                  const std::shared_ptr<spdlog::logger>& log) {
        // --- Finalize the scoped process-noise table (network.process_noise) ---
        // Re-read the canonical members AFTER every clamp, arming rule and
        // validity-restore in this section — network.clock_model_q1/q2 in
        // particular are reset wholesale when the clock-model parameters fail
        // validation — so the struct, and therefore the run manifest, reports the
        // rate the estimator will ACTUALLY use rather than the one requested.
        // Provenance settles here too: `jval` already let an explicit legacy key
        // beat the table-supplied default, so a class whose legacy key is present
        // reads Legacy whatever the table said.
        {
            using QC  = NetworkQClass;
            using Src = NetworkProcessNoise::Source;
            auto& npn = network_config_.process_noise;
            const std::pair<QC, double> finals[] = {
                { QC::SatClock,          process_noise_.sat_clock },
                { QC::SatPhaseBias,      process_noise_.sat_phase_bias },
                { QC::SatCodeBias,       process_noise_.sat_code_bias },
                { QC::VtecSh,            global_vtec_sh_.q_per_degree },
                { QC::VtecShLayer2,      global_vtec_sh_.layer2_q_per_degree },
                { QC::StationZwd,        network_zwd_carry_q_ },
                { QC::LevelingAmbiguity, network_phase_leveling_q_amb_ },
                { QC::ChainDatumRpb,     network_phase_datum_eq34_rpb_q_m2s_ },
                { QC::DdCarry,           network_carry_invariant_qz_ },
                { QC::SatClockWfm,       network_clock_model_q1_ },
                { QC::SatClockRwfm,      network_clock_model_q2_ },
            };
            std::string shown;
            for (const auto& entry : finals) {
                const QC     c           = entry.first;
                const double final_value = entry.second;
                const NetworkQDesc& d    = kNetworkQClassTable[static_cast<std::size_t>(c)];
                Src src = npn.source(c);
                if (jhas(json, d.legacy_path, d.legacy_flat)) {
                    src = Src::Legacy;              // explicit key beat the table default
                } else if (final_value != npn.get(c)) {
                    src = Src::Derived;             // a clamp or arming rule moved it
                }
                npn.set(c, final_value, src);
                if (src != Src::Default) {
                    if (!shown.empty()) shown += ", ";
                    shown += fmt::format("{}={:g} {} [{}]", d.name, final_value, d.units,
                                         NetworkProcessNoise::sourceLabel(src));
                }
            }
            // Only the non-default classes are listed: on a stock config this line
            // reports the handful of q's the posture actually moved, and its
            // ABSENCE is the signal that every rate is at its documented default.
            if (shown.empty())
                log->info("Network process noise: all {} classes at documented defaults.",
                          kNetworkQClassCount);
            else
                log->info("Network process noise ({} classes; non-default shown): {}",
                          kNetworkQClassCount, shown);
        }
    }

    void GnssConfig::applyNetworkConsumerCouplings(GnssConfig& c, bool consume_clock,
                                                   bool consume_bias, bool undifferenced) {
        // RTA-7(a): the ONE place the network-consumer couplings live.  Pure
        // extraction of the closed-loop derivation body — deriveClosedLoopConsumerConfig
        // and applyNetworkConsumerProfile both call this, so the in-process
        // closed-loop harness and the standalone real-time consumer can never
        // diverge.  No coupling keys off consume_bias today; it is in the
        // signature so a future bias-side coupling lands here (see header).
        (void)consume_bias;
        // The consumer is an uncombined PPP-RTK USER (single receiver), not the
        // joint network producer.
        c.processing_mode_ = ProcessingMode::PPP_RTK;
        // The network producer ESTIMATES the shared satellite states; the
        // consumer CONSUMES them from the injected SSR store, so turn the
        // network-forced estimation flags back off to their PPP-RTK defaults.
        c.estimate_sat_clocks_       = false;
        c.estimate_sat_phase_biases_ = false;
        c.promote_sat_code_bias_     = false;
        c.estimate_network_sat_code_bias_ = false;  // producer-only switch; consumer CONSUMES the OSB
        c.estimate_network_iono_sh_ = false;        // producer-only; the consumer estimates its own local iono
        // EXT-CB is likewise producer-only.  It is deliberately NOT translated into
        // a consumer-side change: the consumer INHERITS enable_code_phase_bias =
        // true (never force-set false under this posture), so it applies the SAME
        // external .BIA the producer modelled against — which is exactly right,
        // because the producer emitted no OSB to replace it.
        c.network_external_sat_code_bias_ = false;
        c.global_vtec_sh_.enabled   = false;
        // The consumer applies the network's satellite code biases via ssr_bias_term
        // (not gated by enable_code_phase_bias), and inherits enable_code_phase_bias
        // = false from the producer when the network estimates its own OSB — so the
        // network OSB is the SOLE code-bias source and the .BIA never double-corrects.
        // Satellite CODE bias handling depends on WHICH clock the consumer uses.
        // The network folds the satellite code delay into its estimable CLOCK, so
        // a consumer using the NETWORK clock (FULL mode) must NOT re-estimate a
        // separate sat code bias.  But on the PRECISE clock (BIAS-ONLY / BASELINE)
        // there is no folded delay — the consumer is an ordinary PPP-RTK user and
        // SHOULD estimate residual per-(sat,signal) code biases for signals its
        // .BIA does not cover, or the uncorrected code pulls the position estimate.
        c.estimate_sat_biases_ = !consume_clock;
        // The network SOLVE keeps code fully weighted (no_bia_code_downweight ≈ 1)
        // to separate the satellite clock from the phase bias.  A single-receiver
        // USER that inherits that fully weights code carrying uncorrected biases →
        // a systematic position pull.  Restore the user default so code on signals
        // without .BIA coverage is downweighted.
        c.no_bia_code_downweight_ = 100.0;

        if (undifferenced) {
            // UNDIFFERENCED uncombined PPP-RTK USER (the end-state). No
            // observation-space differencing: the network satellite phase biases
            // are applied directly to the raw float (ssr_bias_term, undifferenced
            // — there is no applyDifferencing to re-difference them away), and the
            // AmbiguityResolver forms the between-satellite difference INTERNALLY
            // at fix time (y_dd = x[i] - x[ref]), which cancels the receiver phase
            // bias and yields integer SD ambiguities. So integer recovery needs no
            // BSSD filter machinery / pivot handover at all.
            c.enable_between_satellite_sd_ = false;
            // Direct per-band LAMBDA, NOT the MW/.BIA-frame WL: the state is
            // already PerBand, and forcing wl_only off makes the driver drop the
            // MW WL (setArIgnoreMwWl(true)) so AR runs pure MLAMBDA on the network-
            // bias-corrected float — dissolving the network-bias↔.BIA frame
            // conflict. No WL or WL/NL cascade.
            c.ar_wl_only_ = false;
            // Direct NL LAMBDA needs a converged float to keep the integer search
            // bounded — a loose σ-gate (safe for the robust long-λ WL) lets the
            // per-band search explode. Cap the gate so AR only fires once the
            // float has settled (a BIAS-ONLY float reaches cm well within this).
            if (!(c.ar_max_position_sigma_m_ > 0.0) || c.ar_max_position_sigma_m_ > 0.20)
                c.ar_max_position_sigma_m_ = 0.20;
        } else {
            // Legacy BSSD consumer: the user-side S-transform removes the receiver
            // clock/bias datum so the SD ambiguity is integer once the network
            // satellite phase bias is applied. The producer runs undifferenced
            // (no receiver datum to remove); a BSSD single receiver recovers
            // integers through the pivot S-transform.  (The closed-loop wrapper
            // overlays its configured ar_wl_only for this branch; the standalone
            // profile keeps the user's own setting.)
            c.enable_between_satellite_sd_ = true;
        }
    }

    GnssConfig GnssConfig::deriveClosedLoopConsumerConfig() const {
        GnssConfig c = *this;
        const ClosedLoopConfig& cl = network_config_.closed_loop;
        // Shared couplings (RTA-7(a) — one source of truth with the standalone
        // profile).  consume_clock mirrors the pre-extraction local: the clock
        // coupling applies only when products are actually consumed.
        const bool consuming_network_clock =
            cl.consume_network_products && cl.consume_network_clock;
        applyNetworkConsumerCouplings(c, consuming_network_clock,
                                      cl.consume_network_products,
                                      cl.consumer_undifferenced);
        // RTA-9(a) closed-loop counterpart: the derived consumer consumes live
        // network biases exactly like the standalone profile, but this path
        // never ran the profile's staleness-gate arming — so it silently applied
        // unboundedly stale biases across producer withhold gaps AND never armed
        // the A5 freshness/presence flip detectors (the sp3_6h withhold-aligned
        // excursions of the Jul-2026 closed-loop audit).  Arm the gate under
        // consumption with the same explicit-false respect; the arming happens
        // on the DERIVED config only, so the producer run is untouched.  (No
        // LogManager here — the effective value surfaces via the consumer
        // engine's config echo.)
        if (cl.consume_network_products && !c.enable_ssr_bias_max_age_
            && !c.enable_ssr_bias_max_age_explicit_)
            c.enable_ssr_bias_max_age_ = true;
        // Card CC (A): the harness knob lands on the DERIVED consumer config as
        // the engine-level processing key (a standalone RT consumer sets the
        // processing key itself).  Never cleared here — an explicit
        // processing.ssr_phase_bias_disc_carry on the base config is kept.
        if (cl.pb_disc_carry)
            c.ssr_phase_bias_disc_carry_ = true;
        // Card SID phase 5b: the same derivation for the declared-roster latch and
        // freshness response.  Never cleared here, and the consumer detects the
        // declaration's presence from the wire rather than from this flag, so
        // arming it against a pre-card wire is inert and says so.
        if (cl.roster_latch)
            c.consumer_roster_latch_ = true;
        // Card SID phase 6b (R6): the consumer's channel mode is the consumer's.  The
        // derived config is a full copy of the producer's, so a producer at `all`
        // would otherwise widen the held-out station too and move two things at once.
        // Set UNCONDITIONALLY from the harness key (default "single"), which is the
        // one place the two modes are separated.
        c.channel_mode_ = (cl.channel_mode == "all") ? ChannelMode::All
                                                     : ChannelMode::Single;
        // Card PBT (A): same derivation for the EVERY-step translation.  Never
        // cleared here — an explicit processing.ssr_phase_bias_step_translate on
        // the base config is kept.  The eps is carried alongside so the harness
        // knob is self-contained; it is only read when the translation is armed.
        if (cl.pb_step_translate) {
            c.ssr_phase_bias_step_translate_     = true;
            c.ssr_phase_bias_step_translate_eps_m_ = cl.pb_step_translate_eps_m;
        }
        // Card ALC: same derivation for the ambiguity LEVEL check.  Never cleared
        // here — an explicit processing.amb_level_check on the base config is kept.
        // The three tuning knobs ride along so the harness key is self-contained;
        // they are only read when the check is armed.  The validation (window >= 2,
        // k > 0, mode in {act, observe}) already ran on the BASE config's parse, so
        // the harness values are re-validated here rather than trusted.
        if (cl.amb_level_check) {
            c.amb_level_check_ = true;
            c.amb_level_check_window_ep_ = (cl.amb_level_check_window_ep >= 2)
                                         ? cl.amb_level_check_window_ep : 20;
            c.amb_level_check_k_ = (cl.amb_level_check_k > 0.0) ? cl.amb_level_check_k : 4.0;
            c.amb_level_check_mode_ =
                (cl.amb_level_check_mode == "observe") ? "observe" : "act";
        }
        // Card CSL: the canonical-signal rule is a NETWORK-side lever.  The
        // derived consumer is a full copy of the producer config and runs its own
        // PppModel, so without this line processing.canonical_signal_rule would
        // silently re-decide the HELD-OUT station's per-band tracking attribute
        // too — and the consumer's latch is a separate, already-graded variable
        // (card CSL section 1: moving the consumer to the civil family costs it
        // 162 → 80 epochs below a decimetre).  Pin the consumer on the historical
        // rule so an arm of this knob changes the producer alone.
        c.canonical_signal_rule_ = CanonicalSignalRule::Count;
        // Card CSL-2: the per-station SCOPE is the same network-side lever, and a
        // scope that happened to name the held-out station would put the consumer
        // on 'priority' through the back door (the scope decides on its own, so
        // pinning the rule alone is not enough).  Clear the list too, which is the
        // configuration that makes the rule pin above authoritative.
        c.canonical_signal_priority_stations_.clear();
        // Card CSL-3: the CODE half of the rule is the same network-side lever.
        // With the consumer pinned on 'count' it is unread there (the follow
        // clause belongs to 'priority' alone), so this line is belt and braces —
        // but it is the line that keeps the pin true if the rule pin above is
        // ever relaxed, and the census graded this knob by REPLAYING consumers,
        // whose own code latch must not move with the producer's.
        c.canonical_signal_code_follows_phase_ = true;
        // Card L5P: the extra-band code deweight is a PRODUCER-side lever on the
        // network's own normal equations.  The derived consumer is a full copy of
        // the producer config and would otherwise deweight its own band-5 code as
        // well — a separate variable, and not the one under test (B5D section 3
        // graded the consumer's band-5 code residual at its own weight).  Pin it
        // at 1 so an arm of this knob moves the WIRE alone.
        c.network_config_.extra_band_code_sigma_scale = 1.0;
        // Card SCC (R6): the second code channel is a PRODUCER-side mechanism — it
        // exists so the satellite C1C and C1W families share the network's stations.
        // A consumer is one station and has no family to support; arming it there
        // would only add a receiver inter-signal state and a second code row to the
        // held-out solution, which is a separate variable and not the one under test.
        // Pin it OFF (and its scale at 1) so an arm of this knob moves the WIRE alone.
        c.network_config_.second_code_channel     = false;
        c.network_config_.second_code_sigma_scale = 1.0;
        // Card SID phase 6a (R6): the second PHASE channel is a PRODUCER-side mechanism
        // for the same reason — it exists so the satellite L1C and L1W phase-bias
        // families share the network's stations.  A consumer is one station with no
        // family to support; arming it there would only add a second float ambiguity to
        // the held-out solution, which is a separate variable and not the one under
        // test.  Pin it OFF (and its scale at 1) so an arm of this knob moves the WIRE
        // alone.
        c.network_config_.second_phase_channel     = false;
        c.network_config_.second_phase_sigma_scale = 1.0;
        // Closed-loop-specific overlay: enable ambiguity resolution for the
        // consumer (the producer itself is float) — the closed loop validates
        // integer RECOVERY on the held-out station from the producer's products.
        c.ar_mode_    = cl.ar_mode;
        if (!cl.ar_active_systems.empty())
            c.ar_active_systems_ = cl.ar_active_systems;
        if (!cl.consumer_undifferenced)
            c.ar_wl_only_ = cl.ar_wl_only;   // BSSD consumer takes the harness's WL-only choice
        return c;
    }

    void GnssConfig::applyNetworkConsumerProfile(LogManager& log_manager) {
        auto log = log_manager.getLogger("gnss_config");
        const NetworkConsumerConfig& nc = network_consumer_config_;
        applyNetworkConsumerCouplings(*this, nc.clock, nc.bias, nc.undifferenced);
        // A standalone consumer of network CODE biases needs receiver code-bias
        // states (estimate_ifb) to absorb a code-bias datum re-reference — the
        // closed-loop consumer inherits estimate_ifb from the producer config,
        // but a standalone user config has no producer to inherit from.  Arm it
        // up front so the engine's one-shot A5 warning (kept for non-profile
        // paths) never fires on the profile path.
        if (nc.bias && !estimate_ifb_) {
            estimate_ifb_ = true;
            log->info("Network-consumer profile: arming estimation.estimate_ifb — receiver "
                      "code-bias states are required to absorb network code-bias datum "
                      "re-references (disc-counter driven reinflation).");
        }
        // RTA-9(a): bias consumption without the bias staleness gate silently
        // applies unboundedly stale biases across producer withhold gaps — arm
        // the gate by default UNDER CONSUMPTION.  Presence-checked at load
        // (enable_ssr_bias_max_age_explicit_): an explicit user 'false' is
        // respected; the closed-loop harness path never runs this (the coupling
        // lives in the profile, not applyNetworkConsumerCouplings).
        if (nc.bias && !enable_ssr_bias_max_age_ && !enable_ssr_bias_max_age_explicit_) {
            enable_ssr_bias_max_age_ = true;
            log->info("Network-consumer profile (RTA-9a): arming processing.enable_ssr_bias_max_age "
                      "— consumed code/phase biases are age-gated at {:.1f}s (stale-hold to "
                      "{:.1f}s = {:.2f}x).  Set the key explicitly false to opt out.",
                      ssr_bias_max_age_s_,
                      ssr_bias_max_age_s_ * ssr_bias_stale_hold_factor_,
                      ssr_bias_stale_hold_factor_);
        }
        log->info("Network-consumer profile APPLIED: PPP-RTK user, consume clock {} / bias {}, "
                  "{} model, orbit_source {}, estimate_sat_biases {}, ar_wl_only {}, "
                  "ar_ignore_mw_wl {}, bias_max_age {} (effective).",
                  nc.clock ? "on" : "off", nc.bias ? "on" : "off",
                  nc.undifferenced ? "undifferenced" : "BSSD",
                  nc.orbit_source,
                  estimate_sat_biases_ ? "on" : "off",
                  ar_wl_only_ ? "on" : "off",
                  networkConsumerArIgnoreMwWl() ? "true" : "false",
                  enable_ssr_bias_max_age_ ? "on" : "off");
    }


    /**
     * @brief EVENT HARNESS — parse @c network.inject into @c NetworkConfig::inject.
     *
     * @param nw   The resolved @c network section.
     * @param log  Logger; every rejection is an error line naming the element index.
     * @return @c false on ANY malformed element (the load fails and the run aborts).
     *
     * Absent @c network.inject leaves the struct default-constructed
     * (@c enable=false, no events) and nothing in the injector is reachable, so the
     * run is byte-identical.  @c enable defaults to TRUE when a non-empty
     * @c events array is present — arming the harness should be one key, and an
     * events list that silently did nothing is exactly the failure this card is
     * built to remove.  Set @c "enable": false to park a schedule without deleting
     * it (and get a warning saying the events are parked).
     *
     * Strictness follows the simulator's scripted-slip loader (@c SimConfig.cpp):
     * a paste-accident cap, per-index error context, and REQUIRED-field checks per
     * @c kind.  A typo in a kind's required field must not fall through to a
     * different kind's defaults.
     */
    bool GnssConfig::loadNetworkInject_(const nlohmann::json& nw,
                                        const std::shared_ptr<spdlog::logger>& log) {
        auto& ic = network_config_.inject;
        ic = NetworkInjectConfig{};                     // absent => OFF, byte-identical

        if (!nw.contains("inject")) return true;
        const auto& ij = nw["inject"];
        if (!ij.is_object()) {
            log->error("Config: network.inject must be a JSON object with an "
                       "'events' array.");
            return false;
        }

        bool have_events = false;
        if (ij.contains("events")) {
            const auto& arr = ij["events"];
            if (!arr.is_array()) {
                log->error("Config: network.inject.events must be an array of event "
                           "objects.");
                return false;
            }
            // Cap: a paste-accident guard, not a design statement.  The harness began
            // as a ONE-EVENT instrument (64 was generous); the DOSE studies
            // (SPEC_event_harness §12) script a whole slip SCHEDULE — the campaign
            // bed's 6.8 slips/station-hour over 60 stations for 2 h is ~800 events —
            // so the guard now sits well above any schedule a 24 h fixture can hold.
            constexpr std::size_t kMaxInjectEvents = 4096;
            if (arr.size() > kMaxInjectEvents) {
                log->error("Config: network.inject.events has {} entries (> {}; "
                           "paste-accident guard).", arr.size(), kMaxInjectEvents);
                return false;
            }
            have_events = !arr.empty();

            int idx = 0;
            for (const auto& el : arr) {
                const std::string where =
                        "network.inject.events[" + std::to_string(idx++) + "]";
                if (!el.is_object()) {
                    log->error("Config: {} must be a JSON object.", where);
                    return false;
                }
                NetworkInjectEvent ev;
                ev.kind = el.value("kind", std::string{});
                if (ev.kind != "slip" && ev.kind != "outage"
                    && ev.kind != "anchor_handover" && ev.kind != "arc_erase"
                    && ev.kind != "sat_clock_step" && ev.kind != "rx_clock_step"
                    && ev.kind != "oracle_pin") {
                    log->error("Config: {}.kind '{}' is not one of slip | outage | "
                               "anchor_handover | arc_erase | sat_clock_step | "
                               "rx_clock_step | oracle_pin.",
                               where, ev.kind);
                    return false;
                }
                if (!el.contains("tow") || !el["tow"].is_number()) {
                    log->error("Config: {}.tow is required (GPS seconds of week at "
                               "which the event starts).", where);
                    return false;
                }
                ev.tow = el["tow"].get<double>();
                if (!(ev.tow >= 0.0) || ev.tow >= 604800.0) {
                    log->error("Config: {}.tow {} is outside [0, 604800).",
                               where, ev.tow);
                    return false;
                }
                ev.station      = el.value("station", std::string{});
                ev.sat          = el.value("sat", std::string{});
                ev.signal       = el.value("signal", std::string{});
                ev.mode         = el.value("mode", std::string{});
                ev.delta_cycles = el.value("delta_cycles", 0.0);
                ev.flag_lli     = el.value("flag_lli", false);
                ev.duration_s   = el.value("duration_s", 0.0);
                ev.step_m       = el.value("step_m", 0.0);
                ev.scope        = el.value("scope", std::string{});
                ev.sigma_m      = el.value("sigma_m", 1.0e-4);

                auto need = [&](bool ok, const char* what) {
                    if (!ok) log->error("Config: {} (kind '{}') requires {}.",
                                        where, ev.kind, what);
                    return ok;
                };
                if (ev.kind == "slip") {
                    if (!need(!ev.station.empty(), "a station")) return false;
                    if (!need(!ev.sat.empty(), "a sat")) return false;
                    if (!need(!ev.signal.empty(),
                              "a signal (RINEX-3 phase code, e.g. L1C)")) return false;
                    if (!need(ev.signal[0] == 'L',
                              "a PHASE signal code (leading L)")) return false;
                    if (!need(ev.delta_cycles != 0.0,
                              "a non-zero delta_cycles")) return false;
                } else if (ev.kind == "outage") {
                    if (!need(!ev.station.empty(), "a station")) return false;
                    if (!need(!ev.sat.empty(), "a sat")) return false;
                    if (!need(ev.duration_s > 0.0,
                              "a positive duration_s")) return false;
                } else if (ev.kind == "anchor_handover") {
                    if (!need(!ev.sat.empty(), "a sat")) return false;
                    if (ev.signal.empty()) ev.signal = "all";
                    if (ev.mode.empty())   ev.mode   = "continuous";
                    if (!need(ev.mode == "cold" || ev.mode == "continuous",
                              "mode 'cold' or 'continuous'")) return false;
                } else if (ev.kind == "arc_erase") {
                    if (!need(!ev.station.empty(), "a station")) return false;
                    if (!need(!ev.sat.empty(), "a sat")) return false;
                    // DIAGNOSTIC sub-knob (docs/SPEC_event_harness.md §9): which
                    // of the erase path's four extra actions the injected event
                    // performs.  Default = the PRODUCTION path, so an event that
                    // does not name a mode is exactly what it was before.
                    if (ev.mode.empty()) ev.mode = "erase";
                    // Optional BAND SCOPE: "" / "all" => every signal of the
                    // satellite (both production paths' scope); otherwise a
                    // ";"-separated list of RINEX-3 PHASE codes.
                    if (!ev.signal.empty() && ev.signal != "all"
                        && !need(ev.signal[0] == 'L',
                                 "a PHASE signal scope (leading L), 'all', or none"))
                        return false;
                    if (!need(ev.mode == "erase" || ev.mode == "free_epoch"
                              || ev.mode == "erase_keep_iono"
                              || ev.mode == "erase_no_reseed"
                              || ev.mode == "erase_no_funnel",
                              "mode 'erase' (default) | 'free_epoch' | "
                              "'erase_keep_iono' | 'erase_no_reseed' | "
                              "'erase_no_funnel'")) return false;
                } else if (ev.kind == "sat_clock_step") {
                    if (!need(!ev.sat.empty(), "a sat")) return false;
                    if (!need(ev.step_m != 0.0, "a non-zero step_m")) return false;
                } else if (ev.kind == "oracle_pin") {
                    // SIM-ONLY DIAGNOSTIC (include/filter/NetworkOraclePin.hpp).
                    // Both fields are refused BY NAME: a scope that silently
                    // widened, or a sigma that silently became the default, would
                    // be graded as though it were the one the operator wrote —
                    // the same failure the rx_clock_step 'sat' refusal names.
                    if (!need(!ev.scope.empty(),
                              "a scope ('all' | 'band:<sig>[+<sig>...]' | "
                              "'station:<name>' | 'sat:<id>' | "
                              "'arc:<sta>/<sat>/<sig>')")) return false;
                    std::string why;
                    if (oraclePinParseScope(ev.scope, why).kind
                        == OraclePinScope::Kind::Invalid) {
                        log->error("Config: {} (kind 'oracle_pin'): {}.", where, why);
                        return false;
                    }
                    if (!(ev.sigma_m > 0.0) || !std::isfinite(ev.sigma_m)) {
                        log->error("Config: {} (kind 'oracle_pin').sigma_m ({}) must be "
                                   "> 0 and finite — it is the pin prior's sigma in "
                                   "METRES (default 1e-4).", where, ev.sigma_m);
                        return false;
                    }
                    if (!ev.station.empty() || !ev.sat.empty() || !ev.signal.empty()) {
                        log->error("Config: {} (kind 'oracle_pin') must NOT carry a "
                                   "'station' / 'sat' / 'signal' field — WHICH arcs are "
                                   "pinned is said by 'scope' alone, and a second, "
                                   "silently-ignored selector would arm a different set "
                                   "than the operator wrote.", where);
                        return false;
                    }
                } else {   // rx_clock_step — the exact dual of sat_clock_step
                    if (!need(!ev.station.empty(), "a station")) return false;
                    if (!need(ev.step_m != 0.0, "a non-zero step_m")) return false;
                    // A `sat` here would read as a scope and is not one: a receiver
                    // clock jump is station-COMMON by construction and touches every
                    // satellite.  Refuse rather than silently ignore — a typo that
                    // fell through would arm a different event than the operator wrote
                    // and the leg would be graded as though it were the intended one.
                    if (!ev.sat.empty()) {
                        log->error("Config: {} (kind 'rx_clock_step') must NOT carry a 'sat' "
                                   "('{}' given).  A receiver-clock jump is station-COMMON: it "
                                   "shifts EVERY satellite's code and phase at that station by "
                                   "step_m.  Use sat_clock_step for a per-satellite step.",
                                   where, ev.sat);
                        return false;
                    }
                    if (!ev.signal.empty()) {
                        log->error("Config: {} (kind 'rx_clock_step') must NOT carry a 'signal' "
                                   "('{}' given).  A receiver-clock jump is NON-DISPERSIVE: it "
                                   "shifts every signal of every satellite at that station "
                                   "equally.", where, ev.signal);
                        return false;
                    }
                }
                ic.events.push_back(std::move(ev));
            }
        }

        ic.enable = ij.value("enable", have_events);
        if (ic.enable && ic.events.empty()) {
            log->warn("Config: network.inject.enable is true but no events are listed "
                      "— the injector is INERT (and the run is byte-identical).");
            ic.enable = false;
        }
        if (!ic.enable && have_events)
            log->warn("Config: network.inject has {} event(s) but enable is false — "
                      "the schedule is PARKED and nothing will be injected.",
                      ic.events.size());
        if (ic.enable) {
            log->warn("Config: network.inject is ARMED ({} event(s)).  This run's input "
                      "is deliberately corrupted; its products are an EVENT HARNESS "
                      "fixture, never a solution.  An armed event that never fires is a "
                      "HARD run-end error.  docs/SPEC_event_harness.md",
                      ic.events.size());
            if (!network_config_.trace_events)
                log->warn("Config: network.inject is armed but network.trace_events is "
                          "OFF — the INJECT rows in network_events.csv (the harness's "
                          "own attribution channel) will not be written.");
        }
        // --- oracle_pin: the FIELD-INERTNESS mechanism ---------------------------
        // A HARD error, not warn-and-disarm (the oracle_relevel precedent): a pin
        // that silently degraded to "no pin" would report the model's error budget
        // WITHOUT the ambiguities held at truth and be read as a ceiling.  That is
        // the armed-and-inert failure standing invariant #4 names, except here the
        // inert arm produces a WRONG SCIENTIFIC VERDICT, so it must not survive.
        //
        // It is also what makes this knob field-inert: a field lineage has no
        // truth root, so an oracle_pin event cannot be armed on one at all.
        {
            bool any_pin = false;
            for (const auto& e : ic.events) if (e.kind == "oracle_pin") any_pin = true;
            if (any_pin && ic.enable) {
                const auto& root = network_config_.slip_qc.oracle_truth_root;
                if (root.empty() || !std::filesystem::exists(root))
                    throw std::runtime_error(
                        "network.inject.events[] arms an 'oracle_pin' event but "
                        "network.cycle_slip.oracle_truth_root ('" + root
                        + "') does not exist.  oracle_pin is a SIM-ONLY DIAGNOSTIC that "
                          "reads a simulator fixture's truth_ambiguities.csv / "
                          "truth_slips.csv; it is field-inert by construction and this "
                          "error is that mechanism.  It must never appear in a golden or "
                          "in docs/CONFIG_promotion_ledger.json.");
                log->warn("Config: network.inject arms {} 'oracle_pin' event(s) — the "
                          "producer's float ambiguities in scope will be HELD AT TRUTH "
                          "from '{}'.  This run's products are a DIAGNOSTIC FIXTURE, "
                          "never a solution.  docs/SPEC_oracle_pin.md",
                          [&] {
                              int n = 0;
                              for (const auto& e : ic.events)
                                  if (e.kind == "oracle_pin") ++n;
                              return n;
                          }(), root);
            }
        }
        return true;
    }

} // namespace GnssProcessor
