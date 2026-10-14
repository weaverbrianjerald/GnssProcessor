/*!
 * \file GnssConfigLoadNetwork.cpp
 * \brief JSON configuration loading for the PPP-RTK @c network section.
 *
 * Split out of @c GnssConfigLoad.cpp (decomposition INC-1; see
 * docs/network_decomposition_plan.md).  @c loadNetworkSection_ is a thin
 * orchestrator that calls one helper per JSON subsection, the idiom the eleven
 * top-level section loaders in @c GnssConfigLoad.cpp use.  Most cards add knobs
 * to this file, and per-subsection helpers keep concurrent edits by different
 * sessions in separate hunks.  Contains:
 *
 *   - the file-local @c parseCoordEpochYear helper (its only caller is
 *     @c loadNetworkRosterAndDatum_),
 *   - @c GnssConfig::loadNetworkSection_, a fixed-order sequence of
 *     @c loadNetworkXxx_ / @c validateNetworkXxx_ calls,
 *   - those per-subsection helpers, defined below in call order except
 *     @c loadNetworkInject_, which is defined last,
 *   - the consumer-coupling helpers @c applyNetworkConsumerCouplings,
 *     @c deriveClosedLoopConsumerConfig and @c applyNetworkConsumerProfile.
 *
 * @c GnssConfig::logNetworkPosture_ and its recommendation tables are in
 * @c GnssConfigNetworkPosture.cpp.
 *
 * **The call order in @c loadNetworkSection_ is load-bearing.**  The cross-knob
 * forcings are expressed as sequence: the @c parallel_carry / @c parallel_reduce
 * mutex, the RTF-3 refusal of inert arming (which reads state parsed
 * before it), the D5 corroboration gate that auto-arms the slip-QC
 * instrumentation feeding it, the RAIM tier auto-couplings, the RTB-8 service
 * arming and its P3 re-validation, and the @c network.process_noise table
 * (resolved first, settled last).  Several helpers therefore read state that a
 * later subsection has not written yet, and several validate state that an
 * earlier one wrote; each says so in its brief.  A new subsection goes in as a
 * new helper at the point in the sequence where its prerequisites are already
 * resolved.  Do not reorder the existing calls.
 *
 * Every helper body was moved verbatim from the single original function, so the
 * decomposition did not change behavior.  The V1/W6 cross-section forcing writes
 * (18 sites over 12 non-network fields) moved with them.  Five of those are the
 * @c network.process_noise write-through (@c process_noise_.sat_clock /
 * .sat_phase_bias / .sat_code_bias and @c global_vtec_sh_.q_per_degree /
 * .layer2_q_per_degree), which fire only when the scoped table supplies a class
 * the config did not set through its legacy key.  Writing across sections is safe
 * because this loader runs only in @c PPP_RTK_Network mode (the PPP and real-time
 * paths never execute it), and within a network run those members name the same
 * physical quantity the table scopes.  The estimation section's raw-JSON peek at
 * @c network.estimate_glonass stays in @c GnssConfigLoad.cpp on purpose: it reads
 * the JSON directly, not this code.
 */

#include "config/GnssConfig.hpp"
#include "filter/NetworkPrefitCensus.hpp"   // parseFuseClassMask / fuseClassMaskName (SPEC_prefit_census_observability INC-2)
#include "filter/NetworkCarryVce.hpp"       // parseComponents (prior-residual census family list)
#include "filter/NetworkCarryVceApply.hpp"  // familySlot (M2 v2 apply-family validation)
#include "filter/NetworkOraclePinScope.hpp" // oraclePinParseScope (the one grammar for the oracle_pin scope)
#include "filter/NetworkParamInventory.hpp" // rxExtraBandDelayEffective (card L5B, the one predicate)
#include "filter/NetworkSecondCodeScope.hpp" // secondCodeAllowEntryValid (card SCC allow-list grammar)
#include "filter/NetworkSecondPhaseScope.hpp" // secondPhaseAllowEntryValid (card SID phase 6a)
#include "filter/NetworkSbasis.hpp"         // pinPairContinuityBlocker (card GFC)
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
         * @brief Parse a coordinate-epoch declaration into a decimal year (RTA-12).
         *
         * Accepts the two documented @c network.coord_epoch shapes: a decimal year
         * ("2024.5", within 1900..2200) or an ISO calendar date ("2024-07-01",
         * mapped to the day centre).  Anything else, including trailing characters,
         * is rejected, so the coordinate-age advisory can tell "unset" from
         * "unparseable".
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

        /**
         * @brief Parse the fixed-cycle fold's knobs (card FCF,
         *        @c network.ambiguity_resolution.fixed_cycles.*) into @p ar.
         *
         * Values only, each checked on its own: an unknown mode or source name, a hold sigma
         * that is not a positive finite number, an alpha outside (0, 1), a negative pass
         * count, a non-positive shift bound, a @c persist_ep below 1 or a negative @c lapse_ep
         * is refused (the fold would change the emitted product, so a value is never silently
         * replaced).  @c persist_ep is the ledger's admission streak (step FCF-3), read whenever
         * the ledger runs (@c trace_lattice, or the fold with source lattice); @c lapse_ep is
         * read under source lattice only.  The cross-knob interlocks
         * read knobs parsed in later helpers and run at the end of the section
         * (@c GnssConfig::validateNetworkFixedCycles_).
         *
         * @param json Parsed root config object.
         * @param ar   The producer AR block being filled.
         * @throws std::runtime_error on a refused value.
         */
        void loadFixedCycleKnobs(const nlohmann::json& json, NetworkArConfig& ar) {
            const std::string pre = "network.ambiguity_resolution.fixed_cycles.";
            auto refuse = [&](const std::string& key, const std::string& why) {
                throw std::runtime_error(pre + key + " " + why + " (card FCF)");
            };
            const std::string fold = jval<std::string>(json, pre + "fold", "", "off");
            if (fold == "off")          ar.fixed_cycle_fold = FixedCycleFold::Off;
            else if (fold == "observe") ar.fixed_cycle_fold = FixedCycleFold::Observe;
            else if (fold == "emit")    ar.fixed_cycle_fold = FixedCycleFold::Emit;
            else refuse("fold", "= '" + fold + "' is not one of off, observe, emit");
            const std::string source = jval<std::string>(json, pre + "source", "", "");
            if (source.empty())         ar.fixed_cycle_source = FixedCycleSource::Unset;
            else if (source == "epoch") ar.fixed_cycle_source = FixedCycleSource::Epoch;
            else if (source == "lattice") ar.fixed_cycle_source = FixedCycleSource::Lattice;
            else refuse("source", "= '" + source + "' is not one of epoch, lattice");
            ar.fixed_cycle_sigma_cyc = jval<double>(json, pre + "sigma_cyc", "", 0.003);
            if (!(std::isfinite(ar.fixed_cycle_sigma_cyc) && ar.fixed_cycle_sigma_cyc > 0.0))
                refuse("sigma_cyc", "must be a positive finite number of cycles");
            ar.fixed_cycle_unit_alpha = jval<double>(json, pre + "unit_alpha", "", 1e-6);
            if (!(ar.fixed_cycle_unit_alpha > 0.0 && ar.fixed_cycle_unit_alpha < 1.0))
                refuse("unit_alpha", "must lie in (0, 1)");
            ar.fixed_cycle_gate_alpha = jval<double>(json, pre + "gate_alpha", "", 1e-4);
            if (!(ar.fixed_cycle_gate_alpha > 0.0 && ar.fixed_cycle_gate_alpha < 1.0))
                refuse("gate_alpha", "must lie in (0, 1)");
            ar.fixed_cycle_max_iter = jval<int>(json, pre + "max_iter", "", 3);
            if (ar.fixed_cycle_max_iter < 0) refuse("max_iter", "must be 0 or more");
            ar.fixed_cycle_max_shift_m = jval<double>(json, pre + "max_shift_m", "", 0.5);
            if (!(std::isfinite(ar.fixed_cycle_max_shift_m) && ar.fixed_cycle_max_shift_m > 0.0))
                refuse("max_shift_m", "must be a positive finite number of metres");
            ar.fixed_cycle_persist_ep = jval<int>(json, pre + "persist_ep", "", 10);
            if (ar.fixed_cycle_persist_ep < 1) refuse("persist_ep", "must be 1 or more");
            ar.fixed_cycle_lapse_ep = jval<int>(json, pre + "lapse_ep", "", 0);
            if (ar.fixed_cycle_lapse_ep < 0) refuse("lapse_ep", "must be 0 (off) or more");
            ar.fixed_cycle_trace = jval<bool>(json, pre + "trace", "", false);
            ar.fixed_cycle_emit_measurement_only =
                jval<bool>(json, pre + "emit_measurement_only", "", false);
        }

        /**
         * @brief The producer AR record's warning in its Emit variant (card FCF section 2).
         *
         * Printed in place of the record's "the emitted product stays FLOAT" warning by a
         * run that arms @c fixed_cycles.fold emit, whose first clause would otherwise be
         * false.  Every run without Emit prints the record's own text, unchanged.
         *
         * @param ar The resolved producer AR block.
         * @return The warning's text.
         */
        std::string fixedCycleEmitWarning(const NetworkArConfig& ar) {
            return fmt::format(
                "Network producer AR ARMED with fixed_cycles.fold EMIT (card FCF): the emitted "
                "product of this run is NOT the float solve.  Solution prep and the emitter read "
                "the held solve of a single-epoch search (source epoch: this epoch's determined "
                "L1/L2 pairs, held at {} cycle), screened by a per-unit test (alpha {:.0e}) and "
                "block acceptance (alpha {:.0e}), which see a wrong integer only where the float "
                "disagrees with it; against a slip no detector flags (a stale zero-q float) the "
                "guard is the slip fixes of card ARL section 13, required here.  The estimator, "
                "the carry, the search, the DD trace, post-fit QC and every emitted sigma stay "
                "float.  This is a measurement "
                "arm of card FCF decision D1 on a sim fixture, not a product: the producer AR "
                "record (docs/network_ambiguity_resolution_design.md) stands until the author "
                "rules.  Search gates: min_epochs={}, FFR>={:.4f}, ratio>={:.1f}, trace {}.",
                ar.fixed_cycle_sigma_cyc, ar.fixed_cycle_unit_alpha, ar.fixed_cycle_gate_alpha,
                ar.min_epochs, ar.min_success_rate, ar.ratio_threshold, ar.trace ? "ON" : "off");
        }

        /**
         * @brief The stations whose RINEX file does not lie in a sim fixture (card FCF, the
         *        check behind @c fixed_cycles.emit_measurement_only).
         *
         * A sim fixture writes @c truth_ambiguities.csv beside each station's RINEX file (the
         * rule @c scripts/simnet_fixed_cycle_grade.py's @c fixture_of reads), so a station
         * whose path is empty or whose directory lacks that file is not on a sim bed.
         *
         * @param stations The network roster.
         * @return Their names, in roster order; empty when every station is on a sim bed.
         */
        std::vector<std::string> stationsOffSimFixture(const std::vector<StationConfig>& stations) {
            std::vector<std::string> off;
            for (const StationConfig& s : stations) {
                std::error_code ec;
                const bool on_bed = !s.rinex_path.empty()
                    && std::filesystem::is_regular_file(
                           std::filesystem::path(s.rinex_path).parent_path() / "truth_ambiguities.csv", ec);
                if (!on_bed) off.push_back(s.name);
            }
            return off;
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

        // Read the service-profile flag first (RTB-8).  Several coherence warnings in
        // this section (per_const no-op, contract-rebroadcast inert, warmup-URA no-op,
        // emit_ura-without-raim, retro-bump-no-tier, RT0-5 inert rate) would be false
        // under service_mode, because the bundle arms their prerequisites in the
        // arming block near the end of this section; they are therefore gated on
        // !service_mode.  The arming itself still runs last, after every armed toggle
        // is parsed.  The flag is kept as a member (networkServiceMode()) for
        // provenance: the run manifest (L15) records that the run's toggle set came
        // from the bundle rather than from hand-tuning.  It is written here and
        // nowhere else, which lets the helpers that gate on it re-derive the old
        // driver's `service_mode` local from the member in an alias preamble.
        network_service_mode_ =
            jval<bool>(json, "network.service_mode", "network_service_mode", false);

        // The subsection sequence.  The order is load-bearing (see the file banner):
        // every cross-knob forcing in this section is expressed as "a later helper
        // reads what an earlier one resolved".  Do not reorder; insert a new helper
        // at the point where its prerequisites are already final.
        if (!loadNetworkProcessNoise_(json, log)) return false;
        if (!loadNetworkRosterAndDatum_(json, nw, log)) return false;
        loadNetworkProducerAr_(json, log);
        loadNetworkLaneLayers_(json, log);
        loadNetworkOutputAndParallel_(json, log);
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
        loadNetworkSignalIntegrity_(json, log);
        if (!loadNetworkEmissionFilters_(json, log)) return false;
        if (!validateNetworkDatumCoherence_(log)) return false;
        if (!validateNetworkLaneLayers_(json, log)) return false;
        if (!validateNetworkPhaseDatum_(log)) return false;
        loadNetworkContractHardening_(json, log);
        applyNetworkServiceProfile_(json, log);
        if (!validateNetworkCarryPosture_(json, log)) return false;
        logNetworkRxExtraBandDelay_(log);
        loadNetworkClosedLoop_(nw, log);
        // The event harness loads last, so its cross-knob warnings read final values
        // (a check beside a knob's own parse would read the code defaults of knobs
        // that are parsed later).
        if (!loadNetworkInject_(nw, log)) return false;
        logNetworkSummary_(log);
        finalizeNetworkProcessNoise_(json, log);

        // The smoothed hold's two load-time refusals (card SMH sections 8 and 9), on the
        // resolved values: the within-epoch tier can be armed by raim_withhold_sigma0 or
        // raim_we_code_iono, and the anchor is disarmed under the clock model.  Neither is
        // left to degrade at run time.
        if (network_config_.emit_pb_hold_smoothed && !network_raim_within_epoch_)
            throw std::runtime_error("network.emit_pb_hold_smoothed requires "
                                     "network.raim_within_epoch: its fold capture is the record "
                                     "of which keys the final solve folded and at which prior "
                                     "means, which the clone's revision reads (card SMH)");
        if (network_config_.emit_pb_hold_smoothed
            && (!network_clock_temporal_anchor_ || network_clock_model_))
            throw std::runtime_error("network.emit_pb_hold_smoothed requires "
                                     "network.clock_temporal_anchor (and not network.clock_model, "
                                     "which disarms it): the clone lives while the anchor holds its "
                                     "clock, and without the anchor the clock's q is the legacy "
                                     "rate, a datum device the smoother would pass as motion "
                                     "(card SMH section 9)");

        // Card NBP: the native broadcast clock a priori ships with card SMH's smoothed hold
        // and refuses card LAH's phase-clock hold, which is valid only with a precise a
        // priori clock product (card SCL section 3.1: LAH under the broadcast clock reads
        // 1.191 m).  On the resolved values, beside SMH's own refusals.
        if (network_apriori_source_ == NetworkAprioriSource::Sp3BrdcClock) {
            if (network_config_.emit_pb_hold_phase_clock)
                throw std::runtime_error("network.apriori_source sp3_brdc_clock refuses "
                                         "network.emit_pb_hold_phase_clock: the phase-clock hold "
                                         "is valid only with a precise a priori clock product "
                                         "(card SCL section 3.1; card NBP)");
            if (!(network_config_.emit_pb_hold && network_config_.emit_pb_hold_smoothed))
                throw std::runtime_error("network.apriori_source sp3_brdc_clock requires "
                                         "network.emit_pb_hold with network.emit_pb_hold_smoothed "
                                         "(card SMH's smoothed hold, the hold the posture was "
                                         "measured with; card NBP)");
        }

        // Card FCF: the fixed-cycle fold's interlocks, here for the reason given for the
        // smoothed hold's refusals above: most of the knobs they read are parsed after the
        // fold's own knobs (loadNetworkProducerAr_).
        validateNetworkFixedCycles_(log);

        // Feed interlock for network.carry_coupling_reinit_sources (card 1g).  It sits
        // here, at the end of the section, rather than beside the knob's own parse:
        // `phase_datum_eq34` and `phase_anchor_carry_reref` are parsed in later helpers
        // (and the latter can still be disarmed by validateNetworkPhaseDatum_), so a
        // check at the parse site would read their code defaults instead of the
        // values the run uses.
        //
        // The realization-step sources are populated only by the eq34 chain datum
        // (eq34_reref) and the prioritized phase-anchor carry re-reference
        // (phase_anchor_reref).  With neither armed, the widened probe has nothing to
        // match and the knob is inert: 0 diverted terms on a 150-station field arm
        // with phase_datum_prioritized but without phase_anchor_carry_reref, against
        // 37441 on a 50-station eq34 arm and 507 on a prioritized + pa_carry_reref sim
        // arm.
        if (network_carry_coupling_reinit_sources_
            && !network_phase_datum_eq34_ && !network_phase_anchor_carry_reref_)
            log->warn("Network producer (Card 1g): carry_coupling_reinit_sources is armed but "
                      "NEITHER network.phase_datum_eq34 NOR network.phase_anchor_carry_reref is "
                      "on, so no realization-step source will ever populate the reinit history "
                      "and the card is INERT (the clock-QC feeds it shares with Card 1d are "
                      "unaffected).  Arm one of those two, or drop this knob — do not read the "
                      "resulting A/B as 'no effect'.");

        // Anchor-slip handover interlock (SPEC_event_harness §12.7), placed here for
        // the reason given above: the phase-datum mode flags are parsed in
        // loadNetworkDatumSignalling_, after the cycle_slip block, so a check at the
        // parse site would read their code defaults.  The handover acts inside
        // networkElectPhaseAnchors, the `else` arm of the two-way mode dispatch.
        // Under the eq34 datum, or with the prioritized election off, there is no
        // anchor to hand over and the knob is a structural zero (invariant #4).
        if (network_config_.cycle_slip_anchor_slip_handover
            && (!network_phase_datum_prioritized_ || network_phase_datum_eq34_)) {
            log->warn("Network: network.cycle_slip.anchor_slip_handover needs the "
                      "PRIORITIZED anchor election (network.phase_datum_prioritized "
                      "without phase_datum_eq34) — that is the "
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
        // MW evidence-refloat interlock, placed here for the same reason: mw_detect is
        // parsed in loadNetworkSlipDetectors_, but a posture later in that function
        // can still force it on, so a check at the parse site would read a value the
        // run does not use.  Without the detector there is no MW flag to admit and
        // the knob is a structural zero (invariant #4).
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
        // Common-band scope interlock, placed here for the same reason as the evidence
        // refloat above.  The exoneration evidence is the MW detector's set of
        // affirmatively clean pairs, so without mw_detect there are no silent pairs,
        // nothing is exonerated, and every verdict falls back to the all-band
        // response: a structural zero (invariant #4).
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
        // Reference-arc clause interlock (§22).  The clause is part of the scope's
        // exoneration rule: with the scope off no verdict is formed, nothing is
        // refused, and the knob is a structural zero (invariant #4).  It follows the
        // block above because the scope's own disarm has just run, so this is the
        // first point where the scope flag is final.
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
        // Re-level interlock (SPEC_slip_relevel_walk), placed here for the reason given
        // above: network.carry_invariant is parsed in loadNetworkDatumSignalling_,
        // after the cycle_slip block, so a check at the parse site would read its code
        // default.  Under carry_invariant the leveling local carry is superseded and
        // structurally empty, so the re-level response cannot change anything there
        // (the DD invariant carry has its own slip response), and arming it would
        // only mislead (invariant #4).
        if (network_cycle_slip_relevel_free_epoch_ && network_carry_invariant_) {
            log->warn("Network: network.cycle_slip.relevel_free_epoch (and its "
                      "delta_bridge rider) is inert under network.carry_invariant "
                      "(the leveling local carry is superseded and structurally "
                      "empty there) — both disabled.");
            network_cycle_slip_relevel_free_epoch_ = false;
            network_cycle_slip_relevel_delta_bridge_ = false;
        }
        // Gap re-entry response interlocks (card G; docs/SPEC_slip_qc_hardening.md
        // §G3/W1), placed here for the load-order reason above: network.carry_invariant
        // is parsed after the cycle_slip block, and the relevel_free_epoch disarm it
        // drives has just run, so this is the first point where the flags of both
        // vehicles are settled.
        //
        // These checks warn and do not disarm, unlike the other interlocks in this
        // block.  Without the free-epoch latch the response is still wired (it marks
        // pending-reseed and feeds the C3c funnel), but the mark does not survive to
        // the PASS-B leveling inject, because PASS B's measurement pre-pass consumes
        // it first (the root defect of SPEC_slip_relevel_walk §6).  Disarming would
        // hide the size of that loss; instead the drain counts every arc it would have
        // freed as `gap_reentry_no_relevel`, and the run-end card-G line reports it.
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
        // Per-signal rider interlock (card GRS;
        // docs/refound/GRS_gap_reentry_per_signal_2026-09-20.md).  It warns and leaves
        // the knob armed but inert: the per-signal tier skips any satellite that
        // produced a row-level resumption in the same epoch, and that list is
        // collected only when the row-level card is armed, so the rider alone would
        // count (and respond to) one break twice.  The knob stays armed so the run-end
        // counters show the inertness instead of the knob disappearing silently
        // (invariant #4).
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
        // Co-resumed share interlock (network.cycle_slip.gap_reentry_coresumed_share).
        // It warns and leaves the knob armed but inert, as the GRS interlock above
        // does: the share reads the per-signal resumption list and acts through the
        // per-signal response, so without that tier (and the row-level response it
        // rides on) there is nothing to share.  The run-end line states the inertness.
        if (network_config_.slip_qc.gap_reentry_coresumed_share
            && !(network_config_.slip_qc.gap_reentry_per_signal
                 && network_config_.slip_qc.gap_reentry)) {
            log->warn("Network (slip-QC co-resumed share): "
                      "network.cycle_slip.gap_reentry_coresumed_share is armed WITHOUT {}.  "
                      "The share reads the per-signal resumption list and frees through the "
                      "per-signal response, so it is INERT in this posture: no co-resumed "
                      "signal is freed and every share counter in the run-end line reads 0 "
                      "(armed-and-inert, not a quiet run).",
                      !network_config_.slip_qc.gap_reentry_per_signal
                          ? (!network_config_.slip_qc.gap_reentry
                                 ? "network.cycle_slip.gap_reentry_per_signal and "
                                   "network.cycle_slip.gap_reentry_relevel"
                                 : "network.cycle_slip.gap_reentry_per_signal")
                          : "network.cycle_slip.gap_reentry_relevel");
        }
        if (network_config_.slip_qc.gap_reentry_coresumed_share
            && network_config_.slip_qc.gap_reentry_per_signal
            && network_config_.slip_qc.gap_reentry)
            log->info("Network (slip-QC co-resumed share): CO-RESUMED SHARE ON.  The phase "
                      "signals of one (station, satellite) that return from a gap at the SAME "
                      "epoch are one re-acquisition: when the per-signal tier gives any of "
                      "them a verdict (shifted, LLI {}, unmeasurable {}), every other "
                      "co-resumed signal of that line of sight is freed too (cause `shared`, "
                      "event scope=los_shared).  A geometry-free test against a partner that "
                      "tracked through cannot see a step common to both, which a channel "
                      "delay change at re-acquisition is.  Signals that tracked through the "
                      "gap are never freed by the share.  SOLVE-AFFECTING.",
                      network_config_.slip_qc.gap_reentry_lli ? "ON" : "OFF",
                      network_config_.slip_qc.gap_reentry_per_signal_unmeasurable ? "ON"
                                                                                   : "OFF");
        // Tracker-key interlock (card GRK;
        // docs/refound/GRK_gap_reentry_key_mismatch_2026-09-22.md).  The knob changes
        // which signal the arc-level tracker names per band.  With no card-G consumer
        // armed, that choice reaches only the slip_raw trace and the MW/GF detections,
        // so the warning states that the response half of the fix is not in this
        // posture.
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
        // Arc-scoped GF jump trigger interlock (card ARL; include/slip/NetworkGfArcJump.hpp),
        // placed here for the load-order reason above.  It warns and does not disarm, as
        // the card-G checks do: without the free-epoch latch the trigger still marks
        // pending-reseed, and its run-end flag closure states what it did.
        if (network_config_.slip_qc.gf_arc_jump) {
            const auto& sq = network_config_.slip_qc;
            if (!network_cycle_slip_relevel_free_epoch_ && !network_carry_invariant_
                && network_phase_leveling_)
                log->warn("Network (GF arc jump, card ARL): network.cycle_slip.gf_arc_jump is "
                          "armed on a phase-leveling lineage with "
                          "network.cycle_slip.relevel_free_epoch OFF.  A flag marks the pair "
                          "pending-reseed, and PASS B's measurement pre-pass consumes that "
                          "mark before the leveling inject reads it, so the stale level "
                          "still folds (SPEC_slip_relevel_walk section 6).  Arm "
                          "relevel_free_epoch to make the trigger act.");
            log->info("Network (GF arc jump, card ARL): arc-scoped GF jump trigger ON.  Per "
                      "dual-band arc, r = d(t) - d(last clean step), d the one-epoch "
                      "geometry-free phase step; a flag when |r| > max({:.3f} m, {} * 1.4826 * "
                      "MAD of the arc's last {} clean r, once {} are held) frees both bands of "
                      "the pair (cause gf_jump).  SOLVE-AFFECTING; not fingerprinted (a "
                      "forward-only slip response).",
                      sq.gf_arc_jump_m, sq.gf_arc_jump_k, sq.gf_arc_jump_window,
                      sq.gf_arc_jump_min_count);
        }
        // Interlocks for the sim-only oracle slip response
        // (include/slip/NetworkSlipOracle.hpp), placed here for the same load-order
        // reason as the blocks above: carry_invariant and phase_leveling are both
        // parsed well after the cycle_slip block.
        //
        // These warn and disarm, unlike the card-G checks.  The oracle's only output
        // is a ceiling number, and a ceiling measured on a vehicle it cannot actuate
        // is a wrong verdict, not a partial one.  The oracle writes
        // LocalCarry::value_abs: without phase_leveling there is no carry to write,
        // and under carry_invariant the leveling carry is structurally empty (that
        // vehicle has its own C3c funnel).
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
            // The two policies are independent, so one corner of the 2x2 has both
            // halves declining and the oracle actuating on nothing.  That corner is a
            // legitimate byte-identity control, so it warns rather than refuses, and
            // the warning says what the arm measures: a reader expecting a ceiling
            // would otherwise read a null result as a falsification (the
            // armed-and-inert case of invariant #4).
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

        // Slant-iono carry interlocks (QUEUE 8b), placed here for the same
        // parse-order reason as above.  All of them warn and disarm (invariant #4):
        // an armed but inert knob misleads more than a refused one.
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

        // Evidence interlock for the D5 v3/v4 consumers (card 4, M1), placed here for
        // the load-order reason above.  The D5 consumers read per-(station, satellite)
        // raw MW/GF step verdicts, which are filled by the slip-QC qualify pass.  That
        // pass runs only when a decision card is armed: SlipQcState::cardsArmed() =
        // lli_corroborate || gap_instrument || bridge_gf || iono_cs
        // (include/slip/NetworkRawObservables.hpp; the gate is the qc.cardsArmed() test
        // in ApplicationNetworkAssembly.cpp).  `network_config_.trace_slip_raw` does not
        // supply the verdicts: it arms only the compute pass
        // (SlipQcParams::anyConsumer).
        //
        // Without a decision card the failure is silent.  The compute pass sets
        // `epoch.computed = true`, and `epoch.reset(n_sta)` sizes `epoch.verdict` to
        // n_sta with empty maps, so the consumers' `slip_qc.epoch.computed` gates in
        // NetworkInstallCarry.cpp pass, every per-satellite lookup misses, and the
        // suppression set is empty.  The gate logs as armed and suppresses nothing
        // (invariant #4).  The gap is also latent: the sets fill as soon as any
        // decision card arms (the LV-6 census arming the CS advance does that), which
        // changes the solve through a knob that was not touched.  See
        // SPEC_level_authority_internal §5.3.
        //
        // The check cannot sit at the parse site in loadNetworkDatumSignalling_:
        // `network_carry_iono_`, one of the two inputs behind `iono_cs`, still holds
        // its raw parsed value there, and the carry_iono interlocks above can clear it.
        // The first of them (carry_iono under carry_invariant) fires on the
        // carry_invariant lineages these D5 knobs are used on.  Here every input is
        // final.
        //
        // The two derived arms mirror the SlipQcParams construction in
        // ApplicationNetwork.cpp (sp.bridge_gf, sp.iono_cs); keep them in step.
        // gap_instrument (card G) is instrumentation only (counters and trace rows,
        // verified byte-identical), which is why it, and not the mw_lli_corroborate
        // decision card, is auto-armed: a decision card would perturb the population
        // being measured.  The prior-residual census (tail of
        // loadNetworkSlipDetectors_) and block-Baarda census (loadNetworkQcTiers_)
        // auto-arms make the same choice.
        //
        // Every committed config that resolves either knob on already resolves
        // `cycle_slip.gap_instrument` on (census of 242 configs), so the interlock does
        // not fire on them; it covers future configs.
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

        // Withhold-era release interlocks (SPEC_withhold_era_release §3), placed here
        // for the load-order reason above: both responders act on the A4 withhold
        // path, the ceiling can be armed as late as the service-mode bundle, and
        // relevel_free_epoch is parsed in the cycle_slip block.  Warn-and-disarm
        // (invariant #4).
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
        // Interlocks for R2 and R4 (queue item 4), placed here because raim and
        // trace_flag_rates are both settled by this point, including by the
        // service-mode arming block.
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

        // Card PRM phase 3b step 2 (network.carry_rebase_complete), on the final values: the
        // knob widens the walks of the three re-base mechanisms that carry owed exemptions, so
        // without any of them armed it is a structural zero (invariant #4) and is disarmed,
        // which also keeps its fingerprint token off.  Each condition is the one
        // NetworkKnobs::snapshot resolves for the mechanism (carryMechanismArmed).
        if (network_carry_rebase_complete_) {
            const bool relevel = network_apriori_source_ == NetworkAprioriSource::BrdcRts;
            const bool eq34 = network_phase_datum_eq34_
                && ((network_phase_datum_eq34_carry_shift_ && !network_carry_invariant_)
                    || (network_phase_datum_eq34_seed_round_
                        && network_phase_datum_eq34_seed_round_rehost_frame_));
            const bool anchored = network_phase_datum_prioritized_ && !network_phase_datum_eq34_;
            const bool dho = network_config_.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf
                && network_config_.amb_float_declared_handover && anchored
                && network_phase_anchor_carry_reref_ && network_phase_datum_handover_continuous_;
            if (!relevel && !eq34 && !dho) {
                log->warn("Network: network.carry_rebase_complete is INERT: no carry re-base "
                          "mechanism with owed sites is armed (network.apriori_source "
                          "brdc_rts, the eq34 chain datum's carry shift or "
                          "seed-round re-host, the float vehicle's declared anchor handover) "
                          "-- disabled.");
                network_carry_rebase_complete_ = false;
            } else {
                log->info("Network producer (card PRM phase 3b): carry re-base COMPLETE ({}{}{}): "
                          "each armed re-base mechanism also moves the carried references its "
                          "exemption table marks as owed (the stash's pivot value, the clock "
                          "baselines, the last solved phase bias, an open pin bridge, the gross "
                          "screen's held ambiguity, every coupling map's terms), each with the "
                          "mechanism's own rule; the by-design exemptions and the phase hold's "
                          "held value stay.  FINGERPRINTED: fresh convergence only.",
                          relevel ? "ephemeris re-level" : "",
                          eq34 ? (relevel ? ", eq34 shift" : "eq34 shift") : "",
                          dho ? ((relevel || eq34) ? ", declared handover"
                                                   : "declared handover")
                              : "");
            }
        }

        logNetworkPosture_(log);
        return true;
    }

    // =================================================================
    //  network subsection helper: loadNetworkProcessNoise_
    // =================================================================
    /**
     * @brief Resolve the scoped per-parameter-class process-noise table (@c network.process_noise).
     *
     * Runs first in the section: classes whose legacy key lives in an earlier section are
     * written through here, while classes parsed later in this section pick up the
     * resolved value as their @c jval default at their own parse site.  Rejects a
     * non-numeric or negative rate and an unknown class name (a misspelled class would be
     * silently inert, the case invariant #4 describes).  @see finalizeNetworkProcessNoise_
     * for the settle after clamping.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     * @return @c false on a fatal misconfiguration (aborts @c loadFromJson).
     */
    bool GnssConfig::loadNetworkProcessNoise_(const nlohmann::json& json,
                                              const std::shared_ptr<spdlog::logger>& log) {
        // --- Scoped per-parameter-class process noise (network.process_noise) ---
        // One authoring surface for the time-linked, network-owned q's (the
        // kNetworkQClassCount = 12 classes of kNetworkQClassTable), whose legacy keys
        // are spread over the filter, estimation and network sections.  It is
        // resolved here, first in the section, because the resolution feeds two
        // mechanisms depending on where the legacy key is parsed:
        //
        //   * classes whose legacy key belongs to an earlier section
        //     (filter.process_noise.*, estimation.global_vtec_sh.*) are already in
        //     their canonical members, so the table writes through to them below,
        //     but only where the legacy key was not set explicitly;
        //   * classes parsed later in this section pass the resolved value as the
        //     `jval` default at their existing parse site, so `jval`'s own
        //     "explicit key beats default" rule supplies the precedence and the
        //     parse sites keep their shape.  This ordering also keeps the derived
        //     network.ura_staleness_q2 correct: it defaults to clock_model_q2 and is
        //     read after it, so it inherits the resolved value.
        //
        // Every class defaults to its legacy key's own default (a unit test asserts
        // that equality for kNetworkQClassTable), so a config with no
        // network.process_noise block resolves to the legacy values: the table is
        // live by default and byte-identical until a value is edited.
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
                    // jval/value() would throw out of loadFromJson on a string or
                    // object here, with no config-path context in the message.  A q
                    // table invites `"sat_clock": "1e-2"`, so the error names the key.
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
            // A misspelled class would otherwise be silently inert (the "looks armed,
            // does nothing" failure the run-end telemetry rule exists to prevent), so
            // it is rejected at load.
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
            if (sj.contains("position_ecef") && sj["position_ecef"].is_array()
                && sj["position_ecef"].size() == 3) {
                sc.position_ecef = {
                    sj["position_ecef"][0].get<double>(),
                    sj["position_ecef"][1].get<double>(),
                    sj["position_ecef"][2].get<double>()
                };
                sc.has_position = true;
            }
            // Optional per-station live-transport descriptor for the
            // network.streaming lanes (RTF-3).  When absent, as in every batch
            // config, sc.transport keeps its default (input_type "" = not streamed):
            // the block is parse-only and changes nothing until streaming is armed.
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

        // --- Contract identity declarations and SIF roster coordinates (RTA-12) ---
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
            // network holds coordinates fixed, so it cannot bootstrap them).  Under
            // network.use_sif_coordinates a missing position_ecef may be resolved
            // from the loaded SIF registry (RTA-12); that is the only relaxation of
            // the hard error.  An explicit JSON ECEF always wins (this branch is
            // never entered for it), and with the toggle off the hard error below
            // applies unchanged.
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
                               "be resolved from the SIF registry (unknown station; a 4-char "
                               "prefix candidate rejected because the name is not an exact "
                               "SIF ID or DOMES, see the station_info log; or no usable "
                               "occupation at {:.4f}) — supply position_ecef.",
                               s.name, *run_year);
                    return false;
                }
                // The coordinate is extrapolated past (or before) its occupation
                // segment: an equipment change after a closed segment invalidates
                // the extrapolated coordinate, so say how far and whether a change
                // lies in the gap.
                if (rc->extrapolated)
                    log->warn("Network roster: station '{}' SIF coordinate at {:.4f} is "
                              "EXTRAPOLATED {:+.3f} yr outside its occupation segment "
                              "[{:.4f}, {:.4f}); antenna change in gap: {}; receiver "
                              "change in gap: {}",
                              s.name, *run_year, rc->extrapolation_years,
                              rc->segment_start_year, rc->segment_end_year,
                              rc->antenna_change_in_gap ? "yes" : "no",
                              rc->receiver_change_in_gap ? "yes" : "no");
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
            // When network.coord_epoch is empty and any coordinate was
            // SIF-propagated, the declared coordinate epoch defaults to the
            // propagation date, so the S-contract cepoch= field states the epoch the
            // roster is valid at.  An explicit network.coord_epoch always wins.
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
        // Coordinate-age advisory (RTA-12 item 4).  Plate motion is about 2-7 cm/yr,
        // so fixed roster coordinates more than coord_age_warn_years past their
        // declared epoch should be re-propagated before they bias a cm-level product
        // datum.  Advisory only (a single warning): datum-event handling (automatic
        // re-propagation or product re-basing on a coordinate update) is an
        // RTF-scale follow-on and out of scope here.
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
     * This is the only parse site for these keys and the only status line for them.
     *
     * The gates are stricter than on the user path (FFR 0.9999, per-pair frac 0.10,
     * partial AR on) because a wrong network fix corrupts every downstream user.  The
     * default @c enable=false skips the AR block entirely, so the float product is
     * byte-identical (docs/network_ambiguity_resolution_design.md).
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
            // Card ARL: partial fix in the decorrelated space (default off).
            ar.decorrelated_partial = jval<bool>(json, "network.ambiguity_resolution.decorrelated_partial",
                                                 "network_ar_decorrelated_partial", false);
            ar.decorrelated_determined_sigma_cyc =
                jval<double>(json, "network.ambiguity_resolution.decorrelated_determined_sigma_cyc",
                             "network_ar_decorrelated_determined_sigma_cyc", 0.05);
            if (!(std::isfinite(ar.decorrelated_determined_sigma_cyc)
                  && ar.decorrelated_determined_sigma_cyc > 0.0))
                ar.decorrelated_determined_sigma_cyc = 0.05;
            ar.tree_basis = jval<bool>(json, "network.ambiguity_resolution.tree_basis",
                                       "network_ar_tree_basis", false);
            ar.block_search = jval<bool>(json, "network.ambiguity_resolution.block_search",
                                         "network_ar_block_search", false);
            // Card ARL: the suspect-corner mask (filter/NetworkArSuspect.hpp), in loop
            // epochs; 0 = off.  A negative window is named and replaced by off.
            ar.suspect_epochs = jval<int>(json, "network.ambiguity_resolution.suspect_epochs",
                                          "network_ar_suspect_epochs", 0);
            if (ar.suspect_epochs < 0) {
                log->warn("Network producer AR: ambiguity_resolution.suspect_epochs = {} is "
                          "negative; the suspect-corner mask is OFF (0).", ar.suspect_epochs);
                ar.suspect_epochs = 0;
            }
            if (ar.suspect_epochs > 0 && !ar.enable)
                log->warn("Network producer AR: ambiguity_resolution.suspect_epochs = {} but "
                          ".enable is OFF; the search never runs, so the suspect-corner mask "
                          "is INERT.", ar.suspect_epochs);
            else if (ar.suspect_epochs > 0)
                log->info("Network producer AR: suspect-corner mask ARMED (card ARL): a DD "
                          "with a (station, satellite) corner freed, re-levelled, dropped, "
                          "RAIM-dropped, MW-unknown or GF-jump flagged is kept out of the "
                          "next {} search(es) (still traced, in_subset 0, with the "
                          "suspect_bits and reentry columns).  The release is time-only.  "
                          "Log-only: the products do not change.", ar.suspect_epochs);
            // Card AGT (step AGT-5): the sticky full-coverage cycle basis
            // (filter/NetworkArCycleBasis.hpp).  It replaces tree_basis's per-signal Prim and
            // keeps decorrelated_partial's search and block_search's blocks, so it needs all
            // three; otherwise it is named and forced off.
            ar.cycle_basis = jval<bool>(json, "network.ambiguity_resolution.cycle_basis",
                                        "network_ar_cycle_basis", false);
            ar.cycle_basis_sticky = jval<bool>(json, "network.ambiguity_resolution.cycle_basis_sticky",
                                               "network_ar_cycle_basis_sticky", true);
            if (ar.cycle_basis && !(ar.tree_basis && ar.decorrelated_partial && ar.block_search)) {
                log->warn("Network producer AR: ambiguity_resolution.cycle_basis requires "
                          "tree_basis, decorrelated_partial and block_search (here {}, {}, {}); "
                          "the cycle basis is forced OFF.",
                          ar.tree_basis ? "on" : "off", ar.decorrelated_partial ? "on" : "off",
                          ar.block_search ? "on" : "off");
                ar.cycle_basis = false;
            }
            if (ar.cycle_basis && !ar.enable)
                log->warn("Network producer AR: ambiguity_resolution.cycle_basis is ON but "
                          ".enable is OFF; the search never runs, so the cycle basis is INERT.");
            else if (ar.cycle_basis)
                log->info("Network producer AR: cycle basis ARMED (card AGT): per constellation "
                          "the two signals with the most arcs share one graph; a {} baseline "
                          "forest is closed into one DD per chord of every band (the whole "
                          "cycle lattice, E - V + C per band; fundamental fallbacks counted, "
                          "not searched), one search block per station.  Trace columns "
                          "cycle_kind, band_only, basis_age_ep, map_state.  Log-only: the "
                          "products do not change.",
                          ar.cycle_basis_sticky ? "sticky (incumbent links kept)"
                                                : "NON-sticky (rebuilt every search)");
            if (!ar.cycle_basis_sticky && !ar.cycle_basis)
                log->warn("Network producer AR: ambiguity_resolution.cycle_basis_sticky is false "
                          "but cycle_basis is OFF; the setting is INERT.");
            // Card AGT (step AGT-6): the measure-only lattice observatory
            // (filter/NetworkArLattice.hpp).  Its offers are the cycle basis's determined DDs and
            // its releases the cycle basis's arc life readings, so it needs the cycle basis;
            // otherwise it is named and forced off.
            ar.trace_lattice = jval<bool>(json, "network.ambiguity_resolution.trace_lattice",
                                          "network_ar_trace_lattice", false);
            if (ar.trace_lattice && !ar.cycle_basis) {
                log->warn("Network producer AR: ambiguity_resolution.trace_lattice requires "
                          "cycle_basis (OFF here, or forced off above); the lattice observatory "
                          "is forced OFF.");
                ar.trace_lattice = false;
            }
            if (ar.trace_lattice && !ar.enable)
                log->warn("Network producer AR: ambiguity_resolution.trace_lattice is ON but "
                          ".enable is OFF; the search never runs, so the lattice observatory is "
                          "INERT (no file is opened).");
            else if (ar.trace_lattice)
                log->info("Network producer AR: lattice observatory ARMED (card AGT): each search "
                          "epoch releases the lattice arcs whose life ended, then offers the "
                          "determined DDs (logs/network_ar_lattice.csv, "
                          "logs/network_ar_lattice_events.csv, a run-end report).  Measure-only: "
                          "the products do not change.");
            loadFixedCycleKnobs(json, ar);
            ar.min_epochs = jval<int>(json, "network.ambiguity_resolution.min_epochs",
                                      "network_ar_min_epochs", 10);
            // Cap on the MLAMBDA search dimension (0 = uncapped legacy).  Required at
            // scale: an uncapped ~2500-dim basis at 305 stations does not return.
            ar.ils_max_dim = jval<int>(json, "network.ambiguity_resolution.ils_max_dim",
                                       "network_ar_ils_max_dim", 0);
            // Per-DD trace of the search (logs/network_producer_ar.csv).  A fix count
            // is not a grade: without the per-DD identities the epoch log line cannot
            // be joined to truth_ambiguities.csv, and that join is what re-running
            // Phase 2 is for (docs/network_ambiguity_resolution_design.md, 2026-09-10
            // §4.1).
            ar.trace = jval<bool>(json, "network.ambiguity_resolution.trace",
                                  "network_ar_trace", false);
            if (ar.trace && !ar.enable) {
                log->warn("Network producer AR: ambiguity_resolution.trace is ON but "
                          ".enable is OFF — the Phase-2 search never runs, so there is "
                          "nothing to trace.  network_producer_ar.csv is NOT opened "
                          "(no header-only file).  Arm .enable to get rows.");
            }
            if (ar.enable && ar.fixed_cycle_fold == FixedCycleFold::Emit) {
                // Card FCF section 2: under fixed_cycles.fold emit (the D1 measurement arms) the
                // record's first clause below, that the emitted product stays float, would be
                // false, so that run prints this variant instead.  Every run without Emit prints
                // the record's text, on its condition, unchanged.
                log->warn("{}", fixedCycleEmitWarning(ar));
            } else if (ar.enable) {
                // networkProducerAr() (src/core/filter/NetworkProducerAr.cpp, INC-9b), the
                // Phase-2 DD-fixability diagnostic, consumes these gates: min_epochs and
                // ils_max_dim directly, and min_success_rate / ratio_threshold /
                // ratio_upper_bound / nl_max_per_pair_frac / enable_partial_ar through
                // ils::IlsGateConfig.
                //
                // The message stays a warning (RT0-2) because Phase 2 is read-only: it
                // writes nothing back into the run context and re-pins nothing (that is
                // Phase 3), so the emitted product is still float.  The message must not
                // say "enabled", which would imply that integer fixing reaches the wire.
                log->warn("Network producer AR ARMED, but the emitted product stays FLOAT — "
                          "no integer fixing is applied to it.  What the gates actually drive "
                          "is the Phase-2 LOG-ONLY DD-fixability diagnostic "
                          "(networkProducerAr; 'Network AR (Phase 2, log-only)' lines, from "
                          "epoch min_epochs={}): it reports how many between-station DDs the "
                          "network COULD fix, and re-pin / re-solve is Phase 3 and unbuilt.  "
                          "Live gates: FFR>={:.4f}, ratio>={:.1f} (<{:.0e}), per-pair "
                          "frac<={:.2f} cyc, partial_ar {}, ils_max_dim {}, trace {}.  "
                          "GPS+Galileo only; GLONASS stays float.{}",
                          ar.min_epochs, ar.min_success_rate, ar.ratio_threshold,
                          ar.ratio_upper_bound, ar.nl_max_per_pair_frac,
                          ar.enable_partial_ar ? "on" : "off",
                          ar.ils_max_dim > 0 ? std::to_string(ar.ils_max_dim)
                                             : std::string("uncapped"),
                          ar.trace ? "ON (logs/network_producer_ar.csv)" : "off",
                          // Card AGT: named only when armed, so an unarmed line is unchanged.
                          std::string(ar.cycle_basis ? (ar.cycle_basis_sticky
                                                            ? "  Basis: cycle_basis, sticky."
                                                            : "  Basis: cycle_basis, NON-sticky (control).")
                                                     : "")
                              + (ar.trace_lattice ? "  Lattice observatory ON." : ""));
            }
        }
    }

    // =================================================================
    //  network validator: validateNetworkFixedCycles_ (card FCF)
    // =================================================================
    /**
     * @brief The fixed-cycle fold's interlocks (card FCF sections 2 and 4), on resolved values.
     *
     * Called at the end of @c loadNetworkSection_, because the knobs it reads are parsed by
     * later helpers.  With the fold off it only names fold knobs set without it.  With the
     * fold on it refuses (throws) every posture card FCF does not cover:
     *  - the fold needs the search (@c .enable) on card AGT's cycle basis (its units are the
     *    basis's L1/L2 pairs), the float vehicle (@c network.ambiguity_vehicle float_kf) and
     *    the held S-basis (@c network.phase_datum_held_basis);
     *  - it refuses @c carry_invariant (the held solve reads the unfolded station factors, and
     *    the Card-4 DD priors would fold into the same normals), @c phase_datum_eq34 (another
     *    phase datum, never measured with the fold),
     *    @c cycle_slip.turbo_edit and @c cycle_slip.relevel_delta_bridge (integer repairs of
     *    the float the fold reads), and @c sd_ewl.fix_enable (a second integer path);
     *  - the source is always named; @c lattice (the ledger of fixed cycles, step FCF-3) runs
     *    under Observe only, and Emit with it is refused (decision D1 failed, so step FCF-4 is
     *    not built, and the author's ruling is not quoted);
     *  - Emit requires the slip fixes of card ARL section 13 and what makes them act (the GF
     *    arc trigger at k at most 6 with the free epoch, the IF-TD detector with its post-flag
     *    re-seed, the suspect-corner mask): the per-unit test and block acceptance see a wrong
     *    integer only where the float disagrees with it, and a slip no detector flags leaves
     *    the zero-q float on the wrong integer (card FCFB section 5).  Observe without them is
     *    named;
     *  - Emit refuses @c emit_pb_hold_smoothed (its clock clones are estimator-side and read
     *    the float) and @c network.state_snapshot_path (a snapshot written under Emit carries
     *    emitter state derived from held values into whatever leg resumes it, and a resume
     *    into Emit would read state the snapshot does not carry);
     *  - until the author's ruling on the producer AR record, Emit requires source epoch,
     *    @c emit_measurement_only, and every station's RINEX file in a sim fixture (the D1
     *    measurement arms, section 2: the acknowledgement is checked, not trusted).
     * A gross-LOS screen without @c gross_los_emit_exclude is named: its declarations at an
     * epoch then act after that epoch's fold.  @c lapse_ep set under source epoch is named
     * inert (it acts under source lattice only).
     *
     * @param log The "gnss_config" logger.
     * @throws std::runtime_error on a refused posture.
     */
    void GnssConfig::validateNetworkFixedCycles_(const std::shared_ptr<spdlog::logger>& log) const {
        const NetworkArConfig& ar = network_config_.ambiguity_resolution;
        if (ar.fixed_cycle_fold == FixedCycleFold::Off) {
            if (ar.fixed_cycle_trace || ar.fixed_cycle_emit_measurement_only
                || ar.fixed_cycle_source != FixedCycleSource::Unset || ar.fixed_cycle_lapse_ep != 0)
                log->warn("Network producer AR (card FCF): a fixed_cycles knob is set but "
                          "fixed_cycles.fold is OFF; the fixed-cycle fold is INERT (no held solve, "
                          "no trace).");
            return;
        }
        const char* const mode = ar.fixed_cycle_fold == FixedCycleFold::Emit ? "emit" : "observe";
        auto refuse = [&](const std::string& why) {
            throw std::runtime_error(std::string("network.ambiguity_resolution.fixed_cycles.fold ")
                                     + mode + " " + why + " (card FCF)");
        };
        if (!ar.enable)
            refuse("requires network.ambiguity_resolution.enable: the fold holds what the search fixed");
        if (!ar.cycle_basis)
            refuse("requires network.ambiguity_resolution.cycle_basis (card AGT, with tree_basis, "
                   "decorrelated_partial and block_search): its units are the basis's L1/L2 pairs");
        if (network_config_.ambiguity_vehicle != NetworkAmbiguityVehicle::FloatKf)
            refuse("requires network.ambiguity_vehicle float_kf, the only vehicle it was designed on");
        if (!network_phase_datum_held_basis_)
            refuse("requires network.phase_datum_held_basis, the only phase datum it was designed on");
        if (network_carry_invariant_)
            refuse("refuses network.carry_invariant: the held solve reads the unfolded station "
                   "factors, and the Card-4 DD priors fold into the same normals");
        if (network_phase_datum_eq34_)
            refuse("refuses network.phase_datum_eq34 (a phase datum never measured with the fold)");
        if (network_cycle_slip_turbo_edit_)
            refuse("refuses network.cycle_slip.turbo_edit (an integer repair of the float the fold reads)");
        if (network_cycle_slip_relevel_delta_bridge_)
            refuse("refuses network.cycle_slip.relevel_delta_bridge (an integer repair of the float "
                   "the fold reads)");
        if (network_config_.sd_ewl.fix_enable)
            refuse("refuses network.sd_ewl.fix_enable (a second integer path)");
        if (ar.fixed_cycle_source == FixedCycleSource::Unset)
            refuse("requires fixed_cycles.source (epoch or lattice): the source is always named");
        if (ar.fixed_cycle_source == FixedCycleSource::Lattice && ar.fixed_cycle_fold == FixedCycleFold::Emit)
            refuse("refuses fixed_cycles.source lattice: the hold from the ledger (step FCF-4) is not "
                   "built (decision D1 failed, card FCFB section 4) and the author's ruling on the "
                   "producer AR record is not quoted in card FCF; source lattice runs under observe");

        // The guard against a slip no detector flags (card FCFB section 5).  The per-unit test
        // and block acceptance see a wrong integer only where the float disagrees with it; a
        // missed slip leaves the zero-q float carry on the wrong integer, so both are blind
        // there (the S8 twin without the GF trigger: one stale pair over 30 epochs, its T 0.18 to
        // 7.9 against a bar of 27.6, folded at 24 of them).  The guard is card ARL section 13's slip fixes, with
        // the knobs that make two of them act: the GF arc trigger frees through the free epoch,
        // and the post-flag re-seed is inert without the IF-TD detector.
        const SlipQcConfig& sq = network_config_.slip_qc;
        std::vector<std::string> slip_missing;
        if (!sq.gf_arc_jump)
            slip_missing.push_back("network.cycle_slip.gf_arc_jump");
        else if (sq.gf_arc_jump_k > 6.0)
            slip_missing.push_back(fmt::format("network.cycle_slip.gf_arc_jump_k at most 6 (it is {})",
                                               sq.gf_arc_jump_k));
        if (!network_cycle_slip_relevel_free_epoch_)
            slip_missing.push_back("network.cycle_slip.relevel_free_epoch");
        if (!network_cycle_slip_if_phase_td_)
            slip_missing.push_back("network.cycle_slip.if_phase_td");
        if (!sq.if_phase_td_reseed_on_flag)
            slip_missing.push_back("network.cycle_slip.if_phase_td_reseed_on_flag");
        if (ar.suspect_epochs <= 0)
            slip_missing.push_back("network.ambiguity_resolution.suspect_epochs above 0");
        const std::string slip_list = detail::joinStrings(slip_missing, ", ");

        if (ar.fixed_cycle_fold == FixedCycleFold::Emit) {
            if (!slip_missing.empty())
                refuse("requires the slip fixes of card ARL section 13 (missing: " + slip_list
                       + "): the per-unit test and block acceptance cannot see a slip no detector "
                         "flags, because the zero-q float then sits on the wrong integer (card "
                         "FCFB section 5)");
            if (network_config_.emit_pb_hold_smoothed)
                refuse("refuses network.emit_pb_hold_smoothed: its clock clones are estimator-side "
                       "and read the float");
            if (!network_config_.state_snapshot_path.empty())
                refuse("refuses network.state_snapshot_path: a snapshot written under Emit carries "
                       "emitter state derived from held values into any leg that resumes it, and a "
                       "resume into Emit would read state the snapshot does not carry");
            if (ar.fixed_cycle_source != FixedCycleSource::Epoch || !ar.fixed_cycle_emit_measurement_only)
                refuse("requires fixed_cycles.source epoch and fixed_cycles.emit_measurement_only "
                       "true until the author's ruling on the producer AR record is quoted in card "
                       "FCF: only the D1 measurement arms emit (card FCF section 2)");
            const std::vector<std::string> off_bed = stationsOffSimFixture(network_config_.stations);
            if (!off_bed.empty())
                refuse("with fixed_cycles.emit_measurement_only requires every station's RINEX file "
                       "in a sim fixture (a truth_ambiguities.csv beside it): the D1 arms are sim "
                       "measurements (card FCF section 2); not on a sim bed: "
                       + detail::joinStrings(std::vector<std::string>(
                                         off_bed.begin(),
                                         off_bed.begin() + std::min<std::size_t>(off_bed.size(), 5)),
                                     ", ")
                       + (off_bed.size() > 5 ? fmt::format(" and {} more", off_bed.size() - 5) : ""));
        } else if (!slip_missing.empty()) {
            log->warn("Network producer AR (card FCF): fixed_cycles.fold observe runs without the "
                      "slip fixes of card ARL section 13 (missing: {}).  A slip no detector flags "
                      "leaves the zero-q float on the wrong integer, which the per-unit test and "
                      "block acceptance cannot see, so the {} it and traces it as folded "
                      "(cards FCFB section 5, FCFC section 5).  Measure-only: the products do not "
                      "change.",
                      slip_list,
                      ar.fixed_cycle_source == FixedCycleSource::Lattice
                          ? "ledger keeps holding it, also after the search stops fixing it, until "
                            "its arc leaves the pool, the float moves away or fixed_cycles.lapse_ep "
                            "ends its folding, and the fold folds"
                          : "probe folds");
        }
        if (ar.fixed_cycle_lapse_ep > 0 && ar.fixed_cycle_source != FixedCycleSource::Lattice)
            log->warn("Network producer AR (card FCF): fixed_cycles.lapse_ep is set and INERT under "
                      "source epoch (each epoch's units are the pairs the search determined at that "
                      "epoch); it acts under source lattice.");
        if (network_config_.gross_los_screen && !network_config_.gross_los_emit_exclude)
            log->warn("Network producer AR (card FCF): network.gross_los_screen is on without "
                      "network.gross_los_emit_exclude, so a gross line-of-sight declaration at an "
                      "epoch acts after that epoch's fixed-cycle fold (a unit through it is held "
                      "for one epoch).");
        const bool lattice = ar.fixed_cycle_source == FixedCycleSource::Lattice;
        log->info("Network producer AR (card FCF): fixed-cycle fold ARMED, mode {}, source {}: hold "
                  "{} cycle, unit test alpha {:.0e} (chi-square, 2 dof), block acceptance alpha "
                  "{:.0e} with up to {} drop-and-refold pass(es), shift bound {} m; trace {}.  {}",
                  mode,
                  lattice ? fmt::format("lattice (the L1/L2 pairs the ledger of fixed cycles implies, "
                                        "admitted after {} epoch(s) at one integer, re-validated "
                                        "against the search's float every epoch; {})",
                                        ar.fixed_cycle_persist_ep,
                                        ar.fixed_cycle_lapse_ep > 0
                                            ? fmt::format("a unit lapses after {} epoch(s) the search "
                                                          "does not determine its pair",
                                                          ar.fixed_cycle_lapse_ep)
                                            : std::string("no lapse"))
                          : std::string("epoch (this epoch's determined L1/L2 pairs; no memory)"),
                  ar.fixed_cycle_sigma_cyc, ar.fixed_cycle_unit_alpha,
                  ar.fixed_cycle_gate_alpha, ar.fixed_cycle_max_iter, ar.fixed_cycle_max_shift_m,
                  ar.fixed_cycle_trace
                      ? (lattice ? "ON (logs/network_fixed_cycles.csv, logs/network_fixed_cycle_shift.csv, "
                                   "logs/network_fixed_cycle_events.csv)"
                                 : "ON (logs/network_fixed_cycles.csv, logs/network_fixed_cycle_shift.csv)")
                      : "off",
                  ar.fixed_cycle_fold == FixedCycleFold::Emit
                      ? "Solution prep and the emitter read the held solve (a D1 measurement arm)."
                      : "Measure-only: the products do not change.");
    }

    // =================================================================
    //  network subsection helper: loadNetworkLaneLayers_
    // =================================================================
    /**
     * @brief The SD pre-reduce EWL constraint: @c network.sd_ewl.*.
     *
     * Parse, range clamps and the armed-state reports.  Its inert check needs the
     * slip-detector flags, which are parsed later, so it runs in
     * @c validateNetworkLaneLayers_.  The MW/WL lane keys (@c network.mw_wl.*) that
     * also loaded here were retired with their cards (WL-1, WL-2, WL-3, UWL-1).
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkLaneLayers_(const nlohmann::json& json,
                                            const std::shared_ptr<spdlog::logger>& log) {
        // --- SD pre-reduce EWL constraint (docs/SPEC_sd_ewl_prereduce.md) ---------
        // Adds the between-satellite SD extra-wide-lane MW statistic as a pseudo-obs
        // row in each station's own pre-reduce block, so it reaches the shared
        // satellite phase and code bias columns directly instead of through a DD
        // chord that is 80.7% eq34 tree pins.  Default off => zero rows appended =>
        // byte-identical.
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
            // --- Increments 2a and 2b: integer fixing (2c, the zero-q hold, is not built)
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
                // sd_ewl.enable is false.  An armed knob with no effect is the case
                // invariant #4 exists to catch, so it is refused here with a
                // warning rather than letting the run report zero fixed rows.
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
    }

    // =================================================================
    //  network subsection helper: loadNetworkOutputAndParallel_
    // =================================================================
    /**
     * @brief Product output path/flush, per-station compute parallelism, and the pyviz trace toggles.
     *
     * Holds the @c parallel_reduce / @c parallel_carry mutex (both OpenMP-fan the per-station
     * passes against Eigen-threaded dense algebra, so armed together they oversubscribe) and
     * @c network.epoch_decimation_n.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkOutputAndParallel_(const nlohmann::json& json,
                                                   const std::shared_ptr<spdlog::logger>& log) {
        network_config_.product_output_path =
            jval<std::string>(json, "network.product_output_path", "product_output_path", std::string{});
        // Per-epoch product-file flush for a live tail consumer (RTA-6).  Flushing
        // changes no bytes of the product; the default is false per the default-off
        // toggle discipline.
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
        // parallel_reduce and parallel_carry are mutually exclusive.  Both fan the
        // per-station passes out over OpenMP, and est.solve()'s dense algebra (the
        // fold's GEMMs and the K×K middle inverse) is itself Eigen-threaded, so armed
        // together they nest OpenMP × Eigen inside reduce() and oversubscribe.  The
        // conflict is resolved here, at load time (one early, config-visible point
        // rather than in the epoch driver), by forcing parallel_carry off when both are
        // set.  No shipped config arms parallel_reduce, so this only guards new configs.
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
        // Cap on the DD pairs emitted and covariance-propagated per epoch (0 = unlimited; T2b).
        network_config_.trace_dd_fractional_max_pairs =
            jval<int>(json, "network.trace_dd_fractional_max_pairs",
                      "trace_dd_fractional_max_pairs", 0);
        if (network_config_.trace_dd_fractional_max_pairs < 0)
            network_config_.trace_dd_fractional_max_pairs = 0;
        // DD-trace stride (trace-only) and epoch decimation (estimation-relevant; T2e).
        network_config_.trace_dd_fractional_epoch_stride =
            jval<int>(json, "network.trace_dd_fractional_epoch_stride",
                      "trace_dd_fractional_epoch_stride", 1);
        if (network_config_.trace_dd_fractional_epoch_stride < 1)
            network_config_.trace_dd_fractional_epoch_stride = 1;

        // Which satellite the DD trace differences against (§6.12.5).  An unrecognised
        // name is logged as an error and falls back to the default.  Unlike the Q3
        // denominator, this knob cannot corrupt a solve (it is trace-only), so taking the
        // default keeps a typo from ending a long run.  The message is still required:
        // the knob exists to make the reference identity a stated choice rather than a
        // result of lexical ordering.
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

        // Band-scoped responses (§Z, docs/SPEC_event_harness.md).  Both change the
        // solve when armed and default to the behavior from before the knobs existed,
        // so an unarmed run is byte-identical (invariant #6).  A typo must not silently
        // arm or disarm a carry-shaping response, so an unknown value is logged as an
        // error and the default is taken.
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

        // Per-(cons, signal, sat) -> anchor station map for the spectrometer's anchor
        // partition (S1; docs/SPEC_lane_spectrometer.md §3.3).  Trace-only, with no
        // stride: the row count scales with gauge freedoms (sats x signals), not with
        // station pairs.
        network_config_.trace_phase_anchor =
            jval<bool>(json, "network.trace_phase_anchor", "trace_phase_anchor", false);
        network_config_.epoch_decimation_n =
            jval<int>(json, "network.epoch_decimation_n", "epoch_decimation_n", 1);
        if (network_config_.epoch_decimation_n < 1)
            network_config_.epoch_decimation_n = 1;
    }

    // =================================================================
    //  network subsection helper: loadNetworkStreaming_
    // =================================================================
    /**
     * @brief Multi-station streaming front-end (@c network.streaming.*; RTF-3).
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
        // --- Multi-station streaming front-end (network.streaming; RTF-3) ------
        // Parsed after the stations array so the inert-arming refusal below can see
        // the per-station transport descriptors.  Default off => the batch epoch
        // source is untouched (byte-identical); every clamp is gated on the master
        // switch, so with it off the values pass through unexamined.
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
                // epochs and log cleanly, so the load fails instead.
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
                // (dispatch with whichever stations have arrived); other positive
                // values are accepted for testing, and non-positive ones are clamped
                // with a warning.
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
     * @brief Observability surface and the producer state snapshot (RTB-7).
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
        // Closed-loop iono supply: RSS the SH formal σ with the floor; skip stale OSBs (T3f).
        network_config_.iono_supply_formal_sigma =
            jval<bool>(json, "network.iono_supply_formal_sigma",
                       "iono_supply_formal_sigma", false);
        network_config_.trace_flag_rates =
            jval<bool>(json, "network.trace_flag_rates", "trace_flag_rates", false);
        // One-shot full-arrowhead NEQ export at the Nth processed epoch (<=0 = off).
        network_config_.neq_export_epoch =
            jval<int>(json, "network.neq_export_epoch", "neq_export_epoch", -1);
        // Reduced-RHS class attribution (diagnostics-only and not fingerprinted, so a
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
        // The attribution's named columns (card ICF instrument A'): each dump also writes
        // cols_<tow>.csv, the solution share of every (station, kind) at these columns, and
        // for the station-local labels LOC:<station>:<key> loc_<tow>.csv (card L5B).
        // Same class as rhs_attrib_tow (diagnostics only, not fingerprinted).  The string
        // entries of an array are kept in order, without duplicates; anything else is
        // refused by name and the list stays empty, so the knob is inert, never wrong.
        network_rhs_attrib_columns_.clear();
        const nlohmann::json* cols = detail::resolvePath(json, "network.rhs_attrib_columns");
        if (!cols && json.contains("rhs_attrib_columns")) cols = &json["rhs_attrib_columns"];
        if (cols) {
            if (!cols->is_array()) {
                log->error("Network (rhs-attrib, card ICF): network.rhs_attrib_columns must be "
                           "an array of column labels (e.g. [\"CLK:G07\", "
                           "\"CB:G07:C1W\", \"LOC:MAJU00MHL:GPS_DCB\"]); ignored.");
            } else {
                for (const auto& c : *cols) {
                    if (!c.is_string() || c.get<std::string>().empty()) {
                        log->error("Network (rhs-attrib, card ICF): network.rhs_attrib_columns "
                                   "holds an entry that is not a non-empty string; ignored.");
                        continue;
                    }
                    const std::string lbl = c.get<std::string>();
                    if (std::find(network_rhs_attrib_columns_.begin(),
                                  network_rhs_attrib_columns_.end(), lbl)
                        == network_rhs_attrib_columns_.end())
                        network_rhs_attrib_columns_.push_back(lbl);
                }
            }
        }
        if (!network_rhs_attrib_columns_.empty()) {
            std::string names;
            bool any_local = false;
            for (const auto& c : network_rhs_attrib_columns_) {
                names += (names.empty() ? "" : ", ") + c;
                any_local = any_local || c.rfind("LOC:", 0) == 0;
            }
            const bool on_trip = jval<bool>(json, "network.rhs_attrib_on_trip",
                                            "rhs_attrib_on_trip", false);
            log->info("Network: reduced-RHS attribution solution shares at {} named column(s) "
                      "[{}] (diagnostics-only; logs/rhs_attrib/cols_<tow>.csv for the shared "
                      "labels at every dump, loc_<tow>.csv for the LOC:<station>:<key> labels "
                      "at the armed dumps and a trip's current-epoch dump{})",
                      network_rhs_attrib_columns_.size(), names,
                      (network_config_.rhs_attrib_tow < 0.0 && !on_trip)
                          ? "; no dump is armed, so nothing is written" : "");
            // The station-local readout needs the solve's x_s, which a trip's retained
            // previous frame does not hold: with the trip recorder alone, the LOC labels
            // are decomposed only at the epoch a trip fires, never at the frame before it.
            if (any_local && network_config_.rhs_attrib_tow < 0.0 && on_trip)
                log->warn("Network (rhs-attrib, card L5B): network.rhs_attrib_columns names "
                          "LOC:<station>:<key> labels with only network.rhs_attrib_on_trip "
                          "armed: loc_<tow>.csv is written for a trip's own epoch only, never "
                          "for the retained previous frame (it holds no solve x_s); arm "
                          "network.rhs_attrib_tow to decompose a chosen epoch.");
        }
        // Redundancy / testability census (docs/SPEC_redundancy_census.md).
        // Measure-only and not fingerprinted, the same class as the rhs_attrib block
        // above, so it can be armed on a warm resume against an untraced snapshot.
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
            // Clamp the thresholds that would make the census useless rather than
            // merely noisy.  For eps: eps <= 0 flags nothing, and eps >= 1 flags every
            // row (r_i is in [0,1] by construction).
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
                // already rejects any station whose smallest local pivot is below
                // 1e-12 x the block reference scale, so a floor at or under that
                // leaves INC-2 unable to fire.  That is a silently inert
                // configuration, not a strict one, and the warning says so.
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
        // --- Prefit census, structural fuse, seed-birth tripwire, topology ledger
        //     and flight recorder (docs/SPEC_prefit_census_observability.md).
        // Same class as the two blocks above: all default off, structurally
        // byte-identical when off, and none fingerprinted, so any of them may be
        // armed on a warm resume against an untraced snapshot.  The fuse is
        // solve-affecting, but it changes the meaning of no stored state, which is
        // what the fingerprint protects.
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
        // --- Product-referenced measurement admission
        //     (docs/SPEC_measurement_admission.md).  Same class as the two blocks
        //     above: default off, structurally byte-identical when off, and not
        //     fingerprinted.  It is a forward-only admission policy (RAIM-class) that
        //     re-references no carried quantity and changes the meaning of no
        //     serialized state, so an off/on pair on one shared snapshot stays
        //     A/B-legal.  `adm_screen` is solve-affecting when armed (it removes
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
        // Arc re-entry vet (INC-4, card §10).  Same scope class as the rest of the
        // block: default off, not fingerprinted, and inert on a clean re-entry by
        // construction.
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
        // Range hygiene runs before the arming report, so the report states what the
        // run will do rather than what was typed.  The negated-positive form also
        // rejects NaN, which `<= 0.0` does not.
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
        // Inert guard, following the signal-integrity block: the vet is a verdict and
        // the measure-only arm makes no verdicts, so arming the vet without the screen
        // cannot do what the knob says.  It is disabled with a warning rather than
        // accepted, because a knob that reads on in the manifest and does nothing in
        // the run is the "live counters, dead wiring" case.
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
                // The flag is published by the prefit walk, which runs when any of three
                // knobs is on, not only trace_prefit_census.  Testing that union keeps
                // the trigger available on a topology-only or birth-check-only arm and
                // keeps this warning from firing falsely there.
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
        // Trigger instrumentation (SPEC_day129_frame_guards P1-3/P1-4).  Both knobs are
        // trace-only and absent from the resume fingerprint (the config fingerprint in
        // NetworkSnapshotFingerprint.cpp), so they may be armed on a warm resume against an
        // untraced snapshot.
        network_config_.trace_sh_spectrum =
            jval<bool>(json, "network.trace_sh_spectrum", "trace_sh_spectrum", false);
        network_config_.trace_sh_spectrum_stride =
            jval<int>(json, "network.trace_sh_spectrum_stride", "trace_sh_spectrum_stride", 60);
        network_config_.trace_postfit_sigma0 =
            jval<bool>(json, "network.trace_postfit_sigma0", "trace_postfit_sigma0", false);
        // RHS localization rider (§4.3): keyed (station / satellite / signal) post-fit
        // pools on the same P1-4 residual pass.  Trace-only and fingerprint-safe; it
        // makes the pass run even without the sigma0 trace.
        network_config_.trace_postfit_keyed =
            jval<bool>(json, "network.trace_postfit_keyed", "trace_postfit_keyed", false);
        if (network_config_.trace_postfit_keyed)
            log->info("Network (P2-RHS): network.trace_postfit_keyed ON — per-station / "
                      "per-satellite / per-signal robust post-fit pools -> "
                      "network_pf_keyed.csv (trace-only; the P1-4 residual pass runs "
                      "whenever this or trace_postfit_sigma0 is armed).");
        // Signed per-(station, satellite, signal) post-fit residual rows from the same
        // P1-4 pass (SPEC_event_harness §24.6).  Trace-only and fingerprint-safe (absent
        // from NetworkSnapshotFingerprint.cpp's fingerprint list, like the two riders above);
        // it makes the pass run even without the sigma0 / keyed traces.
        network_config_.trace_postfit_rows =
            jval<bool>(json, "network.trace_postfit_rows", "trace_postfit_rows", false);
        if (network_config_.trace_postfit_rows)
            log->info("Network (§24.6 rows): network.trace_postfit_rows ON — SIGNED per-(station, "
                      "satellite, signal) post-fit residuals (v = y − A·x, observed − predicted, "
                      "metres) with the row weight and LOS elevation -> network_pf_rows.csv "
                      "(trace-only; the P1-4 residual pass runs whenever this, "
                      "trace_postfit_keyed or trace_postfit_sigma0 is armed).");
        // Per-station local-block audit (SPEC_event_harness §24.10, card ZP).
        // Trace-only, fingerprint-safe (absent from NetworkSnapshotFingerprint.cpp's fingerprint
        // list, like the three riders above) and measure-only: it reads the solved
        // estimator through const accessors after the solve.
        network_config_.trace_station_block =
            jval<bool>(json, "network.trace_station_block", "trace_station_block", false);
        if (network_config_.trace_station_block)
            log->info("Network (§24.10 card ZP): network.trace_station_block ON — per-station "
                      "LOCAL-block audit (rank, condition number, the 3 smallest eigenpairs and "
                      "the local states spanning them, the ZWD column's effective vs raw "
                      "information, the reconstructed local correction, and zero-redundancy row "
                      "counts by kind) -> network_station_block.csv on the first 20 EMITTED "
                      "epochs and every 60th thereafter (trace-only, measure-only).");
        // Names the null local column behind each addStation PD rejection (card RF-PD).
        // Trace-only, measure-only (the eigen read runs only on a block the kernel is
        // already discarding) and fingerprint-safe: absent from NetworkRunContext's
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
        // Per-satellite readout of the closed-loop consumer (card CST).  Trace-only,
        // measure-only (the harvest reads state the consumer's epoch has already
        // committed) and fingerprint-safe.  The closed-loop sink check is not here:
        // network.closed_loop.enable is parsed in loadNetworkClosedLoop_, which runs
        // after this helper, so testing it here would read a default and warn on a run
        // that has the harness armed (the same reason as the trace_events note below).
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
        // Screens the phase-bias discontinuity test on the de-commoned step (card JT).
        // Emit-side only (the solve never sees a discontinuity counter), but not a
        // trace knob: it changes the R records on the wire, so it is fingerprinted (the
        // resume fingerprint appends discdecom only when armed) and it travels in the S
        // contract as discdecom=, so a file replay reproduces the counters.
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
        // Scopes a per-satellite discontinuity mark to the channel that moved
        // (card MS).  Emit-side only (no estimate, no datum), but like card JT it changes
        // the declared population on the wire, so it is fingerprinted and travels in the
        // S contract as discscope=.
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
        // Emit-side phase-bias hold (card PBH).  Emit-side only (no estimate, no datum,
        // no row of the normal equations), but it changes the emitted value, so it is
        // fingerprinted and travels in the S contract as pbhold=.
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
        // The level-aware hold (card LAH): the held key keeps the satellite's
        // phase-visible clock, not its phase bias.  Emit-side only, like the hold.
        network_config_.emit_pb_hold_phase_clock =
            jval<bool>(json, "network.emit_pb_hold_phase_clock", "emit_pb_hold_phase_clock", false);
        if (network_config_.emit_pb_hold_phase_clock) {
            if (!network_config_.emit_pb_hold)
                throw std::runtime_error("network.emit_pb_hold_phase_clock requires "
                                         "network.emit_pb_hold (it is a mode of the hold; card LAH)");
            log->info("Network (card LAH): network.emit_pb_hold_phase_clock = true: a held "
                      "(satellite, signal) keeps its phase-visible clock over the generation. "
                      "The held emission is p(t0) + [c~(t) - c~(t0)], c~ the emitted clock less "
                      "the accumulated median clock step of the satellite's constellation over "
                      "its satellites present at consecutive emissions, so a level move reaches "
                      "the code side only.  Posture limit: within a generation the phase-visible "
                      "clock motion is the a priori clock product's (valid with a precise a "
                      "priori, not with brdc_rts).  S line pbhold=2; the solve, C and B records "
                      "are unchanged.");
            std::string lah_apr = jval<std::string>(json, "network.apriori_source",
                                                    "apriori_source", std::string{"sp3"});
            std::transform(lah_apr.begin(), lah_apr.end(), lah_apr.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (lah_apr == "brdc_rts")
                log->warn("Network (card LAH): network.emit_pb_hold_phase_clock with "
                          "network.apriori_source brdc_rts: within a generation the consumer's "
                          "phase-visible clock motion is the broadcast clock's, which drops the "
                          "network's real clock corrections (decimetres per hour).  The mode was "
                          "validated only with a precise a priori clock.");
        }
        // The smoothed hold (card SMH): the held phase bias follows the clock clone
        // c(t0|t); the phase-clock transfer after a clone dies.  Emit-side only.
        network_config_.emit_pb_hold_smoothed =
            jval<bool>(json, "network.emit_pb_hold_smoothed", "emit_pb_hold_smoothed", false);
        if (network_config_.emit_pb_hold_smoothed) {
            if (!network_config_.emit_pb_hold)
                throw std::runtime_error("network.emit_pb_hold_smoothed requires "
                                         "network.emit_pb_hold (it is a mode of the hold; card SMH)");
            if (network_config_.emit_pb_hold_phase_clock)
                throw std::runtime_error("network.emit_pb_hold_smoothed excludes "
                                         "network.emit_pb_hold_phase_clock: the smoothed hold carries "
                                         "the phase-clock transfer as its fallback (card SMH)");
            if (!jval<bool>(json, "network.carry_coupling_exact_update",
                            "carry_coupling_exact_update", false))
                throw std::runtime_error("network.emit_pb_hold_smoothed requires "
                                         "network.carry_coupling_exact_update: the clock clone is "
                                         "time-updated by the exact rule, and card 1d's shrink "
                                         "would destroy it in one epoch (card SMH)");
            // network.raim_within_epoch and card Q3's anchor are refused at the end of this
            // section, on their resolved values (the within-epoch tier can be armed by
            // other knobs, and the anchor is disarmed by the clock model).
            log->info("Network (card SMH): network.emit_pb_hold_smoothed = true: a held "
                      "(satellite, signal) emits the phase-clock transfer minus the passed motion "
                      "Phi = c(t|t) - c(t0|t), with c(t0|t) a clock clone time-updated by the "
                      "exact rule and alive while card Q3's anchor holds the clock; the "
                      "phase-clock transfer bridges the rest.  S line pbhold=3; "
                      "the solve, C and B records are unchanged; trace "
                      "logs/network_pb_hold_clone.csv.");
        }
        // Installs the station's own datum rows before the PD test (card RF-PD-fix).
        // Solve-affecting (it changes which stations are in the network); the
        // rx_code_bias_datum cross-check is deferred to that knob's own block, which is
        // parsed further down.
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
        // The trace_events cross-check is not here: network.trace_events is parsed
        // further down (see the sink check right after it), so testing it here would
        // read a default and warn on a run that has the sink armed.
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
        // Per-epoch section-timer breakdown of the network solve (diagnostic; byte-identical off; D0).
        network_config_.trace_section_timers =
            jval<bool>(json, "network.trace_section_timers", "trace_section_timers", false);
        network_config_.health_interval_s =
            jval<double>(json, "network.health_interval_s", "health_interval_s", 0.0);
        if (network_config_.health_interval_s < 0.0) {
            log->warn("Network: network.health_interval_s {} < 0 clamped to 0 (no operator "
                      "health heartbeat).", network_config_.health_interval_s);
            network_config_.health_interval_s = 0.0;
        }
        // Size-based rotation of the unbounded service CSVs (network_trace /
        // network_solution / network_service_health; P1).  Default 0 = off ⇒ no
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
        // Producer state snapshot and warm resume (RTB-7(a)).  An empty path (the
        // default) = off, byte-identical; the interval is inert without the path.
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
        // Branch-faithful resume: also serialize the card-4 invariant DD carry
        // (RTB-7(c)).  Raw parse here; the carry_invariant and path requirements are
        // enforced in loadNetworkEmissionFilters_, after network_carry_invariant_ is parsed.
        network_config_.state_snapshot_carry =
            jval<bool>(json, "network.state_snapshot_carry", "state_snapshot_carry", false);
        // Diagonal-only Pᶻ in the carry blob (RTB-7(e); write side, the reader is
        // content-driven).  Validated with the rest of the carry block.
        network_config_.state_snapshot_carry_diagonal =
            jval<bool>(json, "network.state_snapshot_carry_diagonal",
                       "state_snapshot_carry_diagonal", false);
        // Also write each chord's linearization DD (the ZL row), so a resumed run can
        // re-base the correction-frame means onto its own x_lin (RS-2, §21.4).  Write
        // side only; validated with the rest of the carry block.
        network_config_.state_snapshot_carry_lin =
            jval<bool>(json, "network.state_snapshot_carry_lin",
                       "state_snapshot_carry_lin", false);
        // Resume-event response (RE-2), A/B safety (HB-1) and blob history (BR-1); RTB-7(d).
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
        // Resume frame capture: the write side of network.resume_frame_restore (§18.10).
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
        // Card-1 coupling-regressor capture, the write side of
        // network.resume_coupling_restore (§18.12).  It has its own knob rather than
        // sharing the frame capture's: the payload is ~37x the rest of the file, so
        // sharing the frame knob would inflate every existing frame recipe and make the
        // branch library ambiguous.
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
        // Datum capture, the write side of network.resume_datum_restore (RS-3, §21.7).
        // Same shape as the frame capture above: its own knob, path-gated, write-only.
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
        // Emit-layer capture, the write side of network.resume_emit_restore (RS-5,
        // §21.10).  Same shape as the frame and datum captures above: its own knob,
        // path-gated, write-only.  The rows come from two outputs-only engines, so
        // arming it can change no estimate and no trace, only the snapshot bytes.
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
                          "one previous emitted bias per sat|signal) as format-forward "
                          "rows under the existing header.  The engine is a bare run "
                          "local today, so without these rows a warm resume emits the RAW "
                          "solve-frame phase bias at its first epoch while the continuous "
                          "wire carries the accumulated re-base; a resume armed with "
                          "network.resume_emit_restore continues the writer's emission "
                          "level instead.  Write-side only; the products and every trace "
                          "are byte-identical.");
        }
        // Station-layer capture, the write side of network.resume_station_restore
        // (RS-6, §21.13).  Same shape as the frame, datum and emit captures above: its
        // own knob, path-gated, write-only.  The rows are plain copies of two ctx.carry
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
        // Canonical-signal capture, the write side of
        // network.resume_signal_canon_restore (RS-8, §21.16).  Same shape as the frame,
        // datum, emit and station captures above: its own knob, path-gated,
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
        // Code-bias linearization-point capture, the write side of
        // network.resume_rxcb_lin_restore (RS-9, §21.18).  It sits beside the station
        // capture above and requires it: the RL row is the reference the RC row's inject
        // gate differences against, so a file with RL and no RC would restore a frame
        // for a carry that is not there.  Write-only; the block reads the station state
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
        // Sink dependency for card RF-PD (deferred from its arming block above,
        // because this toggle is parsed only here).  Invariant #4: an armed instrument
        // whose output has nowhere to go has to say so.
        if (network_config_.trace_station_pd_null && !network_config_.trace_events)
            log->warn("Network (card RF-PD): network.trace_station_pd_null is armed but "
                      "network.trace_events is OFF — network_events.csv is never opened, so "
                      "every sta_pd_dropped row is DISCARDED and only the run-end counter "
                      "survives.  Arm network.trace_events too.");
        // Redundancy census sink dependencies (deferred from the arming block above,
        // because both toggles are parsed only here).  Invariant #4: an armed
        // instrument whose output has nowhere to go has to say so, because its log is
        // otherwise indistinguishable from a quiet network.
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
        // Sink dependencies of the prefit census, fuse, birth check and topology
        // ledger (same deferral, same invariant-#4 reason).  The fuse gets its own
        // warning: it still acts with the sinks off, so a silent fuse is a solve
        // change with no record of what it cut, the one configuration of the
        // prefit-census card that could mislead a later reader.
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
        // reason).  The screen gets its own warning for the same reason as the fuse:
        // it still acts with the sinks off, so a silent screen is a solve change with
        // no record of which channels it removed.
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
        // The shared (satellite) and receiver bias-state traces (L14 / L14b).  They
        // exist because the B product record carries no sigma in any configuration
        // (docs/SPEC_synthetic_minimum_network.md §4.8), so the satellite code-bias
        // variance factor cannot be obtained from the product, and the receiver DCB,
        // which the W6 slice force-arms, appears nowhere as an estimate with a sigma.
        // Both are trace-only: no product grammar change, and byte-identical when off
        // (no file opened, no rows written).
        network_config_.trace_sat_bias_states =
            jval<bool>(json, "network.trace_sat_bias_states", "trace_sat_bias_states", false);
        if (network_config_.trace_sat_bias_states) {
            log->info("Network: satellite bias-state trace ENABLED — network_sat_bias_states.csv "
                      "logs every emitted (sat, code signal) bias with its sigma0-scaled formal "
                      "sigma from the gauge-nulled shared covariance (the B record itself has no "
                      "sigma field; this is the code-bias variance-factor channel).");
            // The "is it inert?" warning is not here: the shared note at the tail of
            // loadNetworkAprioriAndGlonass_ owns it.
        }
        network_config_.trace_rx_states =
            jval<bool>(json, "network.trace_rx_states", "trace_rx_states", false);
        if (network_config_.trace_rx_states) {
            log->info("Network: receiver state trace ENABLED — network_rx_states.csv logs each "
                      "station's receiver clock and receiver code-bias (DCB/IFB) states with "
                      "their datum-conditioned formal sigma each emitted epoch.");
            // The "clock rows only" warning is not here: the shared note at the tail
            // of loadNetworkAprioriAndGlonass_ owns it.
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
        network_config_.replay_allow_undeclared_wire =
            jval<bool>(json, "network.replay_allow_undeclared_wire",
                       "replay_allow_undeclared_wire", false);
        if (network_config_.replay_allow_undeclared_wire)
            log->warn("Network (card OWD): network.replay_allow_undeclared_wire ON — a consumer "
                      "replay of a product file WITHOUT an S contract line runs anyway, with the "
                      "DEFAULT emitter (no clock-discontinuity channel): a legacy leg graded on "
                      "purpose, not a product-quality measurement.");
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
        // V1 scope: estimable satellite clock and phase bias only (the
        // integer-recovery essentials).  The satellite code bias is deferred to W6:
        // estimating it would put the IF code-bias lump in a second estimable place
        // (it is already absorbed into the estimable clock) and need an extra
        // code-bias datum pin.  Clock and phase bias are therefore forced on and code
        // bias off; the satellite code delay then re-partitions into the per-station
        // rx code bias and the local slant iono (the existing L1-referenced iono+DCB
        // convention; the emitted clock is code-blind and phase-consistent).
        // loadEstimationSection_ runs earlier, so this overrides the JSON estimation
        // block.
        estimate_sat_clocks_       = true;
        estimate_sat_phase_biases_ = true;
        estimate_sat_biases_       = false;  // satellite code bias deferred for V1 (W6 below re-enables)
        promote_sat_code_bias_     = false;  // satellite code bias deferred for V1 (W6 below re-enables)
        log->info("Network producer: forcing estimate_sat_clocks + estimate_sat_phase_biases = true");

        // --- Satellite code-bias (OSB) estimation (W6) ---------------------------
        // With network.estimate_sat_code_bias on, the network estimates its own
        // per-(satellite, signal) code biases (all non-GLONASS constellations: GPS,
        // Galileo, BeiDou) and emits them, so the user can skip the external .BIA.
        // This is a single coupled slice: the satellite code-bias column, the
        // receiver DCB (estimate_ifb) and the GIM ionosphere weighting (the
        // constraint that makes the satellite DCB observable against the per-(rx,
        // sat) slant iono) must all be present, or the shared null-space rank gate
        // withholds every epoch.  Forcing them here keeps them in lockstep behind the
        // one flag.
        estimate_network_sat_code_bias_ = jval<bool>(
            json, "network.estimate_sat_code_bias", "estimate_sat_code_bias", false);

        // --- Satellite biases from the external .BIA (EXT-CB) ---------------------
        // The third satellite-bias posture (see GnssConfig::networkExternalSatCodeBias).
        // Parsed here, before the estimate_iono_sh coupling below, because its purpose
        // is to suppress that coupling: co-estimating the SH-VTEC field needs no
        // satellite DCB column when the product already removed the satellite code
        // bias, and the W6 chain would otherwise force enable_code_phase_bias off and
        // make "SH iono + external .BIA" unreachable.
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

        // Co-estimated global SH-VTEC ionosphere.  It replaces the external GIM as the
        // iono constraint that makes the satellite DCB observable, so it implies, and
        // forces on, the whole W6 satellite code-bias slice.  Setting it here (before
        // the W6 chain below) lets that chain fire unchanged; the driver then uses the
        // SH row builder instead of the external-GIM one and does not load a GIM (the
        // two iono sources are never both applied).
        //
        // EXT-CB suppresses only the W6 force.  The SH field itself is untouched:
        // ApplicationNetworkSetup's model construction, the assembly's coefficient
        // registration and the SH row builder all gate on estimateNetworkIonoSh()
        // alone, never on the code-bias flag, so the field is co-estimated as before,
        // with the satellite DCB supplied by the product instead of a column.  (That
        // is the classical known-DCB global-iono setup.)
        estimate_network_iono_sh_ = jval<bool>(
            json, "network.estimate_iono_sh", "estimate_iono_sh", false);
        if (estimate_network_iono_sh_) {
            if (!network_external_sat_code_bias_)
                estimate_network_sat_code_bias_ = true;   // SH co-estimation is the W6 iono constraint
            global_vtec_sh_.enabled             = true;
        }

        // Cold-start receiver-clock pre-seed.  On the network path nothing
        // pre-solves the receiver clock before the first ambiguity seed
        // (syncPppStateToStateVector, the PPP-path writer of x(clock), never runs
        // from ApplicationNetwork), so a free-running receiver's whole clock offset
        // leaks uniformly into every cold ambiguity seed through
        // computeAmbiguitySeed's clk_term = x(clock) = 0.  When on, the state
        // pre-pass writes the median GPS code residual into x(clock) once per
        // station.  Default off => the seeder is never called (byte-identical).
        network_cold_clock_seed_ = jval<bool>(
            json, "network.cold_clock_seed", "cold_clock_seed", false);
        if (network_cold_clock_seed_) {
            log->info("Network: cold_clock_seed ON — pre-seeding each station's cold "
                      "receiver clock from the median GPS code residual before the first "
                      "ambiguity seed (removes the cold-start clock leak into ambiguity seeds).");
        }

        // --- Per-satellite geometry-free code-bias pin (CB-GF-DATUM) --------------
        // Odijk et al. 2016 (J Geod 90:15-44) §5.1 / Table 7, rank deficiency 3b: the
        // satellite-side mirror of the receiver gauge below, which closes rd 2b
        // (receiver biases vs slant iono); this one closes 3b.  Parsed before the W6
        // chain because it selects which posture that chain arms: with the GF datum
        // the producer runs Odijk's slant model (no ionosphere constraint), without it
        // the classical single-layer one.
        network_code_bias_gf_datum_ = jval<bool>(
            json, "network.code_bias_gf_datum", "code_bias_gf_datum", false);
        // The same rd-3b pin, extended to each partitioned secondary receiver channel
        // family (FAMILY-GF; docs/SPEC_family_gf_datum.md).  Parsed beside its parent;
        // its interlock sits below the CB-GF-DATUM validation block on purpose, so it
        // inherits every hard error of that block.
        network_code_bias_gf_datum_per_family_ = jval<bool>(
            json, "network.code_bias_gf_datum_per_family",
            "code_bias_gf_datum_per_family", false);

        // --- Ionosphere-constraint selector of the W6 slice (RF-0b) --------------
        // docs/refound/RF-0_baseline_2026-09-14.md Finding 2.  Parsed here, beside
        // code_bias_gf_datum and before the W6 chain, for the same reason: it selects
        // which posture the chain arms.  "auto" is the three-way selection from before
        // the knob existed (SH field / GF-pinned slant / external GIM) and is
        // byte-identical; "none" removes the ionosphere constraint and leaves the
        // satellite code-bias datum to the IF pin and the per-(constellation, signal)
        // sum-to-zero rows that NetworkSbasis::buildCodeBiasDatumRows already folds
        // every epoch.
        //
        // A typo must not silently pick a different datum for the product, so an
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

        // --- Hard per-satellite geometry-free datum (RF-0c) ----------------------
        // Parsed beside its parent selector because it has meaning only inside the
        // no-constraint posture: with an ionosphere constraint present, the direction
        // this pins is already determined, and pinning it anyway over-constrains it.
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

        // --- The two arms that answer the consumer regression of RF-0d (RF-0e) -----
        // e1 restores an absolute level datum that the geometry-free pins do not
        // provide; e2 instead makes the geometry-free rows carry like every other datum
        // row on a time-linked lineage.  Both default off (byte-identical), and both
        // are inert without the geometry-free datum itself.
        network_config_.sat_if_level_datum =
            jval<bool>(json, "network.sat_if_level_datum", "sat_if_level_datum", false);
        network_config_.sat_gf_datum_time_linked =
            jval<bool>(json, "network.sat_gf_datum_time_linked",
                       "sat_gf_datum_time_linked", false);
        // Leaves the band-2 same-band companion unpinned, so its ISC is a free
        // per-satellite difference rather than an asserted zero (RF-0f).
        network_config_.sat_isc_state =
            jval<bool>(json, "network.sat_isc_state", "sat_isc_state", false);
        // Leaves an extra-band column free where the data determine it, and lets the
        // channel's sum-to-zero row carry the level (B5D).  This is the band-5 half of
        // the principle RF-0f applied to the band-2 companion: there the correction is
        // centimetres and estimable without ionosphere, here it is metres and
        // estimable with the pinned pair's ionosphere and clock (card TBI section 5).
        network_config_.sat_gf_extra_band_free =
            jval<bool>(json, "network.sat_gf_extra_band_free",
                       "sat_gf_extra_band_free", false);
        // The receiver-side counterpart (card L5B): estimate the receiver's extra-band
        // code delay instead of holding it at its seed (RxIfbRule,
        // filter/NetworkParamInventory.hpp).  Its interlocks need the resolved carry and
        // datum knobs, so they are logged by logNetworkRxExtraBandDelay_ after the carry
        // posture is validated.
        network_config_.estimate_rx_extra_band_code_delay =
            jval<bool>(json, "network.estimate_rx_extra_band_code_delay",
                       "estimate_rx_extra_band_code_delay", false);
        // Code-weight half of the band-5-code-removed emulation (card L5P).  With B5D
        // armed, the extra-band code-bias column stays determined by the extra-band
        // code rows alone, so raising their sigma keeps the wire's band-5 code bias
        // while the rows lose their pull on the shared states.  1.0 is the default and
        // is never applied (byte-identical).  A value below 1 is refused (reset to 1.0
        // with an error): a config that asks to trust an extra band more than the
        // pinned pair asks for the opposite of this knob, which indicates a typo, not a
        // posture.
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
        // Second code channel (card SCC).  A station contributes every allow-listed
        // band-1 code it reports besides the one its canonical latch selected, so the
        // C1C and C1W satellite families stop competing for the same stations.  The
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
                    // The list is explicit (R1): a refused entry is dropped and named, never
                    // silently coerced, and an all-refused list leaves the allow-list empty,
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
        // Second phase channel (card SID phase 6a), the phase twin of the block above.
        // A station contributes every allow-listed band-1 carrier phase it reports
        // besides the one its canonical latch selected, so the L1C and L1W satellite
        // phase-bias families stop competing for the same stations.  The three
        // interlocks are checked in the post-parse block below, where the vehicle, the
        // held basis and the channel mode are all populated.
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
                    // The list is explicit: a refused entry is dropped and named, never
                    // silently coerced, and an all-refused list leaves the allow-list
                    // empty, so the knob disarms rather than acting on a guess.
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
            estimate_ifb_          = true;   // receiver DCB/IFB: an explicit rx code-bias state on the
                                             // undifferenced rows, or the satellite OSB absorbs it
            // GIM-weighted iono constraint: the satellite DCB is co-linear with the
            // free per-(rx, sat) slant iono until the GIM imposes between-satellite
            // structure (common-mode-free R leaves the per-constellation level free,
            // pinned by the explicit sum-to-zero DCB datum).  runNetwork hard-errors if
            // the GIM is absent at startup (the slice would be inert without it).
            //
            // The force does not apply when the slant posture is armed.
            // network.code_bias_gf_datum selects Odijk's §5.1 model instead: the
            // ionosphere stays free per-LOS slant, the geometry-free code biases are
            // pinned as datum (rd 2b/3b) and absorbed by that ionosphere, and the
            // estimable code bias is the modernized DCB from the third frequency
            // onward.  Forcing an iono constraint there would contradict the model and
            // destroy the pinned direction, so the force is conditional.
            //
            // Nor does it apply when the no-constraint posture of RF-0b is selected.
            // Under GPS-only dual-frequency the GF-datum escape above is unreachable (it
            // hard-errors at the frequency gate below), so without this branch a dual W6
            // run could not be self-contained: it would demand an external IONEX GIM,
            // against the standing "no external-data dependence" rule, and on the sim
            // bed the COD GIM is the truth ionosphere.  network.w6_iono_constraint =
            // "none" clears the force explicitly rather than skipping it, so a config
            // that also sets estimation.ionosphere_weighting.enable = true (every RF-0
            // lineage does, because the loader forces it in the "auto" posture) cannot
            // leave a half-armed constraint behind.  The GIM itself is never loaded on this
            // branch (ApplicationNetworkSetup), so the flag would be inert, but a flag
            // that reads on while nothing folds is the case invariant #4 describes.
            if (w6_iono_none)                 iono_weighting_.enabled = false;
            else if (!network_code_bias_gf_datum_) iono_weighting_.enabled = true;
            // The full satellite code bias is estimated from scratch in the network's
            // own datum, not as a residual to .BIA.  The external .BIA is skipped on
            // both the producer and (through the derived consumer config) the held-out
            // consumer, so the network OSB is the single code-bias source and nothing
            // is corrected twice.
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
            // The satellite code and phase biases come from the .BIA, so there is no
            // satellite bias state and no OSB datum; what has to travel with the
            // product is the receiver side.
            //
            // enable_code_phase_bias is the apply switch for the whole .BIA, code and
            // phase alike (PppModel::computeBaseModeledValue is keyed on the raw signal
            // string).  An explicit false in the JSON contradicts the posture: the
            // network would then have neither an external nor an estimated satellite
            // code bias, which is the V1 slant-absorption posture under this knob's
            // name.  The load fails rather than silently overriding the setting.
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
            // Estimability trap #4 of the W6 slice applies unchanged: the network is
            // DifferencingMode::None, so on undifferenced rows the per-(rx,
            // constellation) GF code lump has nowhere to go unless the receiver DCB is
            // an explicit pinned state.  Under W6 the satellite OSB would absorb it;
            // here there is no satellite column, so it would enter the slant iono and
            // conflict with the SH field (a per-station constant the global field
            // cannot represent, pulled into a₀₀ and the per-station residual).
            estimate_ifb_ = true;
            if (estimate_network_iono_sh_) {
                // Keep the SH slice's own coupled bundle intact: the elevation cutoff
                // under estimation.ionosphere_weighting is load-bearing for the SH row
                // builder (the iw.elevation_cutoff_deg tests in
                // ApplicationNetworkAssembly.cpp), and the posture ledger checks the
                // enable alongside it.
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
                // The one combination that destroys content, so it is a hard error and
                // not a warning: with a single-layer constraint present, the
                // geometry-free satellite code bias is the estimable DCB (Odijk 2016
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
            // Frequency gate.  Below three frequencies the GF datum is the whole model,
            // not merely unhelpful: IF + GF together fix d_1 and d_2 exactly, so a
            // dual-frequency run would emit satellite code biases that are identically
            // the datum (zeros presented as estimates).  Odijk §5.1 states the same
            // thing as estimability ("only for a triple- or higher-frequency
            // constellation").  frequency_mode is the only pre-run signal available;
            // "all" is data-dependent, so it passes with a warning.
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
            // Announce the posture.  An armed datum that logs nothing is the case
            // invariant #4 describes, and the truth grader auto-detects this posture
            // from the console: without this line it grades the slant arm with the
            // single-layer transform (metre-scale phantom residuals on the two pinned
            // channels, which are datum-fixed to zero by construction).
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

        // --- Validation of the no-constraint W6 posture (RF-0b) -----------------
        // Placed right after the CB-GF-DATUM block, so the two postures without an
        // ionosphere constraint are validated side by side as the alternatives they
        // are.  Both answer rank deficiency 3b and differ only in the datum: the GF
        // pin fixes the per-satellite geometry-free bias to zero (and needs a third
        // frequency to leave anything estimable), while "none" leaves it unpinned and
        // lets the per-(constellation, signal) sum-to-zero rows realize the IGS
        // Sum_s DCB^s = 0 convention.
        if (w6_iono_none) {
            if (!estimate_network_sat_code_bias_) {
                // No OSB columns ⇒ no satellite-DCB rank deficiency to answer ⇒ the
                // option decides nothing.  Invariant #4: an armed but inert knob has to
                // read as inert.  A warning, not an error: nothing is broken, the knob
                // is idle.
                log->warn("Network (RF-0b): network.w6_iono_constraint = none is INERT without "
                          "network.estimate_sat_code_bias — with no satellite code-bias columns "
                          "there is no satellite-DCB / slant-ionosphere rank deficiency for an "
                          "ionosphere constraint to answer, and nothing about the ionosphere "
                          "posture changes.  Arm the W6 slice, or drop the option.");
            }
            if (estimate_network_iono_sh_) {
                // The co-estimated SH-VTEC field is a single-layer ionosphere
                // constraint (the loader arms the W6 slice from it at the
                // estimate_iono_sh parse above), so asking for "no constraint"
                // alongside it is a contradiction, and letting one win silently would
                // ship a product whose datum nobody chose.  The GF datum applies the same
                // hard error against the same knob.
                log->error("Network (RF-0b): network.w6_iono_constraint = none is armed together "
                           "with network.estimate_iono_sh.  Mutually exclusive: the co-estimated "
                           "SH-VTEC field IS the W6 ionosphere constraint (it is what arms the "
                           "slice at all), so 'none' would be either a lie or a silent deletion "
                           "of the field's constraint rows.  Drop one — 'none' is the free-slant "
                           "posture, estimate_iono_sh the single-layer one.");
                return false;
            }
            if (network_code_bias_gf_datum_) {
                // Both are postures without an ionosphere constraint, so this is not a
                // rank conflict but a datum ambiguity, which is worse: the run would
                // still solve and emit a product whose satellite code-bias frame
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
            // Announce the posture.  An armed datum that logs nothing is the case
            // invariant #4 describes, and the truth and wire graders auto-detect the
            // posture from the console: without this line an RF-0b product would be
            // graded against the single-layer transform.  The declared S-basis
            // dimension is named here on purpose: the code-bias gauge rows are
            // constraint rows folded before the null-space gate (NetworkSolveGlue), and
            // NetworkSbasis::buildSharedDatum sets plan.declared_dimension from the
            // per-constellation clock pin alone, so removing the ionosphere constraint
            // cannot move it.  What moves is the observed count, which the gate reports.
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

        // --- Validation of the hard per-satellite geometry-free datum (RF-0c) ----
        // Placed after the RF-0b block so `w6_iono_none` and the W6 chain's forced
        // `iono_weighting_.enabled` are both settled and are tested as they will run,
        // not as the JSON wrote them.
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
                // The knob's own precondition.  An ionosphere constraint determines this
                // direction from data (the GIM's common-mode-free R, or the SH field's
                // between-satellite structure); pinning it as well adds a second datum
                // that competes with the data for the same parameter, and the emitted
                // code biases would then be neither the data's nor the pin's.
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
                // Redundant check: w6_iono_constraint = none already refuses the SH field
                // and forces the weighting off, so reaching this point means a later edit
                // re-opened a path to an armed constraint.  The load fails rather than
                // emitting a doubly datumed product that still passes the gate.
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

        // --- Validation of the two level-datum arms (RF-0e) ---------------------
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

        // --- Validation of the ISC reparameterization (RF-0f) ---------------------
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

        // --- Validation of the extra-band code datum (B5D) -----------------------
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

        // --- The rd-3b pin per partitioned secondary channel family (FAMILY-GF) ---
        // Placed after the CB-GF-DATUM block closes, so it inherits that block's four
        // hard errors (sat code bias required; iono_sh, iono_weighting and dual all
        // refused above, each with `return false`).  The only interlock left to state
        // is the parent-knob one: without the primary GF frame there is nothing to be
        // secondary to.
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

        // --- Sum-to-zero over the receiver code-bias set (RXCB-DATUM) -------------
        // The receiver-side counterpart of the W6 satellite code-bias gauge.
        // Parsed after the EXT-CB slice so its estimate_ifb force is visible to the
        // inert check below.
        network_rx_code_bias_datum_ = jval<bool>(
            json, "network.rx_code_bias_datum", "rx_code_bias_datum", false);
        network_rx_code_bias_datum_sigma_m_ = jval<double>(
            json, "network.rx_code_bias_datum_sigma_m", "rx_code_bias_datum_sigma_m", 10.0);
        if (network_rx_code_bias_datum_ && !(network_rx_code_bias_datum_sigma_m_ > 0.0)) {
            // sigma <= 0 means info = 1/0 = inf, i.e. a hard pin of every receiver DCB
            // to zero: the opposite of a loose bound, and a silent datum takeover that
            // would destroy the estimable between-station differences.
            log->error("Network: network.rx_code_bias_datum_sigma_m must be > 0 (got {}); "
                       "a non-positive sigma is an infinite-information HARD pin of every "
                       "receiver DCB to zero, not a datum.", network_rx_code_bias_datum_sigma_m_);
            return false;
        }
        // Cross-check for card RF-PD-fix (deferred from station_prior_before_pd's own
        // arming block, because rx_code_bias_datum is parsed only here).  Invariant #4:
        // the fix moves the RXCB-DATUM rows earlier, so without that datum there is
        // nothing to move and the knob is inert while reading armed.
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
                // Legal and not over-constraining (the two gauges act on disjoint
                // parameter sets: satellite columns vs receiver columns), but under W6
                // the satellite gauge already determines the receiver half through the
                // iono, so the receiver gauge is redundant rather than load-bearing.
                // Redundant minimum-constraint coverage is harmless (Weyl: PSD
                // information cannot create a new near-zero, the same argument that
                // makes W6's per-signal over-pinning safe), but the log states it, so
                // the datum does not read as what keeps the run alive.
                // Card L5B: with the receiver extra-band code delay estimated, the held
                // delay no longer closes each station's geometry-free level direction, and
                // this datum is what does; the line says so instead of "redundant".  The
                // unarmed text is unchanged.
                if (rxExtraBandDelayEffective(*this))
                    log->info("Network producer (RXCB-DATUM): receiver code-bias sum-to-zero ON "
                              "ALONGSIDE the W6 satellite code-bias gauge, and LOAD-BEARING on this "
                              "run: with network.estimate_rx_extra_band_code_delay effective (card "
                              "L5B) the receiver extra-band code delay is estimated, so it is the "
                              "only closure of each station's geometry-free level direction "
                              "(ionosphere level, receiver DCB, ambiguities).");
                else
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

        // Run-latched W6 code-bias admission census and station-local quarantine
        // (CB-ADM).  It replaces the per-epoch >= 2-station gate with a one-shot
        // startup census: at BeiDou scale that gate's count flaps, which churns
        // prune / re-register / re-seed and creates datum-orphan near-null directions
        // that withhold whole epochs.  Effective only when the W6 slice is armed;
        // armed without W6 it is inert (the code-bias registration block never runs)
        // and warns, per the armed-but-ineffective pattern.
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

        // Seeds the per-LOS slant iono states in the field's frame rather than the
        // receiver-bias-in frame (card LV-1; docs/SPEC_level_information_budget.md).
        // Parsed here, next to the SH field it seeds against; the banner and the
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
     * @brief A-priori orbit/clock source (RTF-1/RTF-2) and GLONASS admission.
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
        // --- Producer a-priori orbit/clock source (RTF-1/RTF-2) -------------------
        // network.apriori_source selects what the producer's per-satellite a-priori
        // clock (and the orbit its geometry uses) is referenced to.  The default
        // "sp3" is the precise SP3/CLK identity and is byte-identical everywhere.
        // "brdc_rts" references the a-priori to the broadcast ephemeris (plus
        // external IGS-SSR 4076 orbits): the self-sufficient real-time service mode
        // (docs/rt_service_conventions.md §1).  This block only parses and validates;
        // the producer core path (nav load, phase-gate suppression, anchor re-base,
        // C0 delta-to-broadcast) is in the network setup and driver.  Mirrors the
        // receiver.troposphere_apriori_source string-match pattern (with its own
        // local to_lower; the receiver-section lambda is out of scope here).  Placed
        // after the estimate_iono_sh coupling above so the brdc_rts validation sees
        // final flag values.
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
            } else if (src == "sp3_brdc_clock") {
                network_apriori_source_ = NetworkAprioriSource::Sp3BrdcClock;
            } else {
                if (src != "sp3")
                    log->warn("Unknown network.apriori_source='{}', falling back to 'sp3' "
                              "(precise SP3/CLK a-priori).", src);
                network_apriori_source_ = NetworkAprioriSource::Sp3;
            }
        }
        // Ephemeris selection rule of the native broadcast clock a priori (card NBP).
        // ttom is the only rule; anything else is a hard error under sp3_brdc_clock and a
        // warning otherwise (the knob is read only under that value).
        {
            std::string sel = jval<std::string>(json, "network.brdc_select", "brdc_select",
                                                std::string{"ttom"});
            std::transform(sel.begin(), sel.end(), sel.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            network_brdc_select_ = NetworkBrdcSelect::Ttom;
            if (sel != "ttom") {
                if (network_apriori_source_ == NetworkAprioriSource::Sp3BrdcClock) {
                    log->error("Network (card NBP): network.brdc_select '{}' is not a rule; "
                               "'ttom' (the latest healthy record transmitted at or before the "
                               "epoch) is the only one.", sel);
                    return false;
                }
                log->warn("Network (card NBP): network.brdc_select '{}' is unknown and inert "
                          "(read only under network.apriori_source sp3_brdc_clock).", sel);
            }
            if (network_apriori_source_ == NetworkAprioriSource::Sp3BrdcClock)
                log->info("Network (card NBP): network.apriori_source = sp3_brdc_clock: SP3 "
                          "orbits and the broadcast clock of the navigation file (rule {}), no "
                          "CLK product; the a priori is continuous across cut-overs through a "
                          "per-ephemeris offset table from 00:00 of the observation day, and the "
                          "wire's clock refers to the stamped ephemeris (C_wire = C_est + O, "
                          "iode=k).  Fingerprinted; fresh convergence only.", sel);
        }
        // A-priori clock anchor σ (m; D6): 0 = auto (resolved per mode at the
        // producer), an explicit positive value wins.  Parsed and stored only here.
        network_apriori_clock_anchor_sigma_m_ = jval<double>(
            json, "network.apriori_clock_anchor_sigma_m", "apriori_clock_anchor_sigma_m", 0.0);
        if (network_apriori_clock_anchor_sigma_m_ < 0.0) {
            log->warn("Network: network.apriori_clock_anchor_sigma_m {} < 0 is meaningless; "
                      "clamped to 0 (auto).", network_apriori_clock_anchor_sigma_m_);
            network_apriori_clock_anchor_sigma_m_ = 0.0;
        }

        if (network_apriori_source_ == NetworkAprioriSource::BrdcRts) {
            // Validation 1 (hard error): the satellite code-bias slice (W6) needs an
            // iono constraint that makes the satellite DCB observable.  Under brdc_rts
            // that constraint has to be the co-estimated SH-VTEC field, because the
            // external GIM path is post-processing-only (a daily product; see the
            // Phase 3b GIM/SH exclusivity in ApplicationNetwork and conventions §1).
            // estimate_iono_sh forces estimate_sat_code_bias on above, so the only
            // illegal combination is sat-code-bias on with iono_sh off (the GIM path).
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
            // Validation 1b (hard error): EXT-CB consumes a daily `.BIA` bias product,
            // which the self-sufficient real-time service does not have (the same
            // post-processing-only reasoning that rejects the external-GIM code-bias
            // path here).  A real-time producer takes its satellite biases from an SSR
            // stream, not a Bias-SINEX file; that is a different mechanism with its own
            // age/continuity semantics.
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
            // Validation 2 (a warning, because no per-mount dialect declaration exists
            // yet; RTF-3): the producer's SSR orbit corrections must be IGS-SSR 4076
            // (CoM); legacy 1240-1270 (APC) mountpoints are user-engine-only (RTB-5,
            // honor_ssr_reference_point).  The dialect is decoded from the message type
            // at runtime, not declared in config, so this cannot be a load-time hard
            // error yet.
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
        // FDMA makes a single shared per-(sat, signal) satellite OSB unphysical, so
        // GLONASS contributes a per-satellite clock only.  The receiver group delay is
        // carried by the per-FCN-slot receiver IFB as a local nuisance
        // (Schur-eliminated, never emitted), and no GLONASS satellite phase bias is
        // produced.  Admitting GLONASS therefore requires the per-slot IFB machinery:
        // its prerequisites are forced on, and GLONASS must be in active_systems
        // (otherwise GLONASS rows would build with no shared clock column and pin the
        // receiver clock, corrupting the GPS/Galileo products).
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
            // A GLONASS signal_priorities entry is the other half of admitting it:
            // without one, PppModel::decideConstellationCanonicals finds no priority
            // string and skips every GLONASS satellite at signal selection, so the
            // network would silently emit zero GLONASS products (no clock, no per-slot
            // IFB ever allocated) while the GPS/Galileo solution looks nominal (the
            // documented multi-GNSS signal_priorities silent-drop failure).  The load
            // fails here so the misconfiguration is caught early, matching the error
            // string above that already tells the user to add the entry.
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

        // GLONASS and the satellite code bias are not jointly supported in V1:
        // admitting GLONASS makes the receiver-bias allocation skip the GPS/Galileo
        // IFB (only the GLONASS per-slot IFB is created; see MeasurementModel's
        // network allocation gate), but separating the satellite code bias requires
        // an explicit GPS/Galileo receiver DCB on the undifferenced rows.  The load
        // fails rather than silently producing an unidentifiable system.
        if (estimate_network_sat_code_bias_ && enable_glonass_network_) {
            log->error("Network: network.estimate_sat_code_bias and network.estimate_glonass cannot "
                       "both be on in V1 — GLONASS admission skips the GPS/Galileo receiver DCB that "
                       "the satellite code-bias estimation needs.  Disable one of them.");
            return false;
        }

        // --- Galileo admission consistency (card GAL, R1) -----------------------
        // Signal selection admits a Galileo observation whenever
        // processing.signal_priorities has a Galileo list (PppModel's canonical
        // decision and selectSignals), while the station state manager allocates the
        // Galileo inter-system bias only for a system in processing.active_systems.
        // The two have to agree:
        //  - a priority list for an inactive system would build rows with no
        //    inter-system bias column, and every station's GPS receiver clock would
        //    absorb both systems (the Galileo clock common mode would leak into the GPS
        //    clocks, and every Galileo row would carry the full inter-system bias as a
        //    residual).  The network admits observations by active_systems (the
        //    synced epochs are stripped of inactive systems; card OWD), so such a list
        //    is ignored with a warning;
        //  - Galileo active without a list admits no Galileo signal; each station
        //    still holds an inter-system bias state that no row touches (the assembly
        //    drops the structurally-zero column), so the run is a GPS run that claims
        //    otherwise.  That configuration does nothing, so it warns and names the
        //    consequence.
        // These mirror the GLONASS pair of checks above.
        {
            const bool gal_active =
                std::find(active_systems_.begin(), active_systems_.end(), "Galileo")
                != active_systems_.end();
            const bool gal_listed = signal_priorities_.count("Galileo") != 0;
            // The network admits observations by processing.active_systems (the synced
            // epochs are stripped of every inactive system right after the sync, in
            // ApplicationNetworkSetup), so a priority list for an inactive system is
            // ignored rather than admitted without its inter-system bias (card OWD, O4).
            // Ignoring instead of refusing keeps archived GPS-only configs with such a
            // list loadable.  One warning per ignored list.
            for (const auto& [sys, list] : signal_priorities_) {
                if (std::find(active_systems_.begin(), active_systems_.end(), sys)
                    != active_systems_.end())
                    continue;
                log->warn("Network (card OWD): processing.signal_priorities has a '{}' list "
                          "({} signal(s)) but '{}' is not in processing.active_systems: the list "
                          "is IGNORED — the network admits observations by active_systems, so "
                          "no {} observation reaches the solve.  Add '{}' to active_systems to "
                          "use it, or remove the list.",
                          sys, list.size(), sys, sys, sys);
            }
            if (gal_active && !gal_listed)
                log->warn("Network (card GAL): 'Galileo' is in processing.active_systems but "
                          "processing.signal_priorities has no 'Galileo' list, so NO Galileo "
                          "signal is admitted: every station carries an inter-system bias "
                          "state that no observation reaches, and the run is a GPS run.  Add "
                          "a Galileo priority list (for example C1C, C1X, L1C, L1X, C5Q, C5X, "
                          "L5Q, L5X) or remove Galileo from active_systems.");
            if (gal_active && gal_listed)
                log->info("Network (card GAL): Galileo ACTIVE with a priority list of {} "
                          "signal(s): each station carries a Galileo inter-system bias "
                          "(a free per-epoch local column), the Galileo satellite clock "
                          "common mode is closed by its own pivot satellite, and under "
                          "network.sat_gf_datum = broadcast_tgd the (E1, E5a) pin pair is "
                          "targeted at c*BGD(E1, E5a) from the navigation message.",
                          signal_priorities_.at("Galileo").size());
        }

        // --- Trace-inertness warnings, evaluated after resolution ---------------
        // Both warnings judge whether an armed trace has any state to write.  They
        // cannot sit beside their own parse in loadNetworkObservabilityAndSnapshot_,
        // which runs two helpers earlier, before anything they test is resolved;
        // there they would read header defaults:
        //
        //   trace_sat_bias_states: `!estimate_network_sat_code_bias_ &&
        //   !network_external_sat_code_bias_` reads two members that are both parsed
        //   in loadNetworkSharedStateForcings_, so at the parse site it is always
        //   true, and the warning would fire on every run that arms the trace,
        //   including the W6 and EXT-CB runs where the trace is live.  A live
        //   instrument reporting itself inert teaches the reader to ignore the one
        //   line that would matter (invariant #4 in reverse).
        //
        //   trace_rx_states: `!estimate_ifb_` would be read before all three sites
        //   that force it on: the W6 slice and the SH slice in
        //   loadNetworkSharedStateForcings_, and the GLONASS slice in this helper.
        //
        // This is the tail of the last helper that touches any of them, so every
        // value below is final.  A new forcing added after this point has to move
        // these checks with it.
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
     * family and @c network.carry_zwd / @c carry_rx_code_bias / @c carry_vce.  Prerequisite
     * violations here throw (the coupling cards are exact only against a dense
     * shared-border carry).
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkCarries_(const nlohmann::json& json,
                                         const std::shared_ptr<spdlog::logger>& log) {
        // --- Phase leveling: carry per-station ambiguities across epochs ---------
        // Moves the producer from a per-epoch absolute snapshot solve to one that
        // carries each non-pivot station's float ambiguities as a diagonal prior on
        // its own local block, so carrier-phase precision (not only code) sharpens the
        // satellite clock, phase-bias and code-bias products.  Additive and purely
        // local: off is byte-identical (the bisection anchor for the whole feature).
        // q_amb, warmup and max_gap are heuristics flagged for tuning.
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
        // Per-LOS B_eff scale of the carried ambiguity row variance at inject (F1;
        // SPEC_amb_iono_split_overcount).  Default off = byte-identical.
        network_phase_leveling_los_scale_ =
            jval<bool>(json, "network.phase_leveling_los_scale", "phase_leveling_los_scale", false);
        // Station-wide jointly whitened local block on the carried ambiguity inject
        // (F3; SPEC_amb_iono_split_overcount).  Default off = byte-identical.
        network_phase_leveling_local_block_ =
            jval<bool>(json, "network.phase_leveling_local_block", "phase_leveling_local_block", false);
        // Admission age gate for the F3 block (SPEC_amb_iono_split_overcount §6.12): legs
        // younger than this many loop epochs are injected per column, not in the block.
        // 0 = off.
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
        // Age-aware young-arc variance inflation (SPEC_amb_iono_split_overcount
        // §6.17–§6.18).  A column whose last re-level is recent carries a code-quality
        // level at a phase-stiff harvested variance, so the T4 tier erases it, and the
        // repeated erase and re-level sustains the ZWD/clock plateau.  V0 = 0 is off and
        // byte-identical.  The two shape knobs keep their defaults on an off run and
        // are validated here anyway, so an armed run can never reach the inject with a
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
        // ---- The ambiguity vehicle and its supporting knobs (RF-1;
        //      docs/refound/RF-1_float_ambiguities_design.md §7.1).
        //
        // String enum on the established loader pattern (network.trace_dd_fractional_ref,
        // network.raim_erase_band_scope): an unknown value is logged as an error naming
        // what the knob changes and falls back to the default "level" rather than
        // ending the run.  "float_kf" changes the solve (the joint block of
        // CouplingRowBuilder::buildLocalBlock, the floored birth prior and bounded drop of
        // filter/NetworkAmbFloat.hpp) and reaches the driver as
        // NetworkKnobs::amb_float_kf.  The cross-section interlocks (float_kf requires
        // phase_leveling and !carry_invariant; F1/F3 are refused under it) live in
        // validateNetworkCarryPosture_, the first point where network_carry_invariant_ is
        // populated.
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
        // The per-line-of-sight phase nuisance state (card LGM section 4.1).  The
        // cross-section interlocks are in validateNetworkCarryPosture_.
        network_config_.phase_colour_state =
            jval<bool>(json, "network.phase_colour_state", "phase_colour_state", false);
        network_config_.phase_colour_sigma_zen_m =
            jval<double>(json, "network.phase_colour_sigma_zen_m", "phase_colour_sigma_zen_m", 0.005);
        network_config_.phase_colour_tau_s =
            jval<double>(json, "network.phase_colour_tau_s", "phase_colour_tau_s", 300.0);
        network_config_.phase_colour_elev_floor_deg =
            jval<double>(json, "network.phase_colour_elev_floor_deg",
                         "phase_colour_elev_floor_deg", 20.0);
        network_config_.phase_colour_max_gap_s =
            jval<double>(json, "network.phase_colour_max_gap_s", "phase_colour_max_gap_s", 900.0);
        network_config_.trace_phase_colour =
            jval<bool>(json, "network.trace_phase_colour", "trace_phase_colour", false);
        network_config_.trace_amb_if =
            jval<bool>(json, "network.trace_amb_if", "trace_amb_if", false);   // card AIL
        if (!(network_config_.phase_colour_sigma_zen_m > 0.0)
            || !(network_config_.phase_colour_tau_s > 0.0)
            || !(network_config_.phase_colour_max_gap_s > 0.0)
            || !(network_config_.phase_colour_elev_floor_deg >= 0.0
                 && network_config_.phase_colour_elev_floor_deg < 90.0))
            throw std::runtime_error("network.phase_colour_*: sigma_zen_m, tau_s and max_gap_s "
                                     "must be > 0 and elev_floor_deg in [0, 90)");
        network_config_.amb_float_birth_var_m2 =
            jval<double>(json, "network.amb_float_birth_var_m2", "amb_float_birth_var_m2", 0.0);
        if (network_config_.amb_float_birth_var_m2 < 0.0)
            throw std::runtime_error("network.amb_float_birth_var_m2 must be >= 0 "
                                     "(0 = use the arc's own first-stash variance unfloored)");
        // Drop-only floor (RF-3a; docs/refound/RF-3_code_row_qc_2026-09-22.md §4 A1).
        // Separate from the birth floor because the defect is the re-coupling edge of
        // the slip and gap re-entry response, not the birth: a birth floor removes the
        // post-drop walk but also slows every birth (hour 1 0.56 -> 0.66 m at
        // WTZZ00DEU, census hour 1 +0.03 to +0.04 m), while the drop is where the stale
        // stiff variance is handed back.
        network_config_.amb_float_drop_var_m2 =
            jval<double>(json, "network.amb_float_drop_var_m2", "amb_float_drop_var_m2", 0.0);
        network_amb_float_drop_decouple_always_ =
            jval<bool>(json, "network.amb_float_drop_decouple_always",
                       "amb_float_drop_decouple_always", false);
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
        // Warm-up gate on the drop floor (RF-3c).  Separates the convergence
        // population (a drop while the network is still finding its clocks) from the
        // settled population the floor is meant for.
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
        // Convergence-gated leveling-carry warmup (T1d; default off = fixed epoch count).
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
        // Full dense cross-covariance carry of the shared border (C1): an
        // information-form KF on the border that recovers the clock↔phase-bias
        // off-diagonals the diagonal D2 stash discards.  Independent of phase_leveling
        // (it acts on the shared D2 carry, not the per-station ambiguity carry), so it
        // is parsed and announced here.
        network_shared_carry_full_cov_ =
            jval<bool>(json, "network.shared_carry_full_cov", "shared_carry_full_cov", false);
        if (network_shared_carry_full_cov_)
            log->info("Network producer (C1): FULL-COVARIANCE shared carry enabled — the D2 "
                      "cross-epoch prior folds the dense (P_prev + Q)⁻¹ block over the carried "
                      "clock/phase-bias/code-bias columns (information-form KF on the shared "
                      "border) instead of independent per-key diagonals.  Departed/pinned/"
                      "retired keys are marginalized; a non-PD carried block degrades to the "
                      "diagonal fold for that epoch (warned).");
        // Gauge-free carry fold, armed as a property of the carry (card C-GF).  The
        // downdate Λ ← Λ − ΛU(UᵀΛU)⁻¹UᵀΛ removes the declared datum directions from the
        // carried information, so the carry constrains only estimable combinations and
        // leaves the gauge to the S-basis.  The downdate is also reachable through
        // network.clock_anchor_gauge_free, a Q3 temporal-anchor knob unrelated to it;
        // this knob makes it available to a config that runs the dense carry without
        // that anchor, since the carry is what deposits gauge information.  It also
        // generalizes U: declared directions with a support-closure test, rather than
        // the carried clocks with a bare ">= 2 members" floor.
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
        // NOTE: `network_clock_anchor_gauge_free_` is parsed in a later helper
        // (loadNetworkEmissionFilters_), so it does not appear in this predicate: reading
        // it here would read its default, not the user's value.  The advisory is
        // therefore phrased to cover the case where the Q3 path supplies the downdate.
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
        // Rigorous conditional local carry (card 1).  Each persistent per-station local
        // (phase-leveling ambiguities and ZWD) is carried as one pseudo-observation row
        // coupling it to the free shared border (regression M_r, pure conditional
        // variance (N_rr⁻¹)_{rr}) instead of as an independent diagonal prior.  This
        // removes the double count of shared information that the diagonal carry incurs
        // against the dense border carry, so the sequential recursion equals the batch
        // solve.  It is exact only against a dense border carry, so
        // shared_carry_full_cov is a hard requirement.
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
        // Joint whitening of a station's coupling rows, instead of marginalizing each
        // departed shared column per row into a scalar variance (card 1b).  Restores the
        // between-row (DD) correlation that the diagonal marginalization discards (the
        // T7 flat-σ defect).
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
        // Border time update of the coupling rows at injection (card 1d).  A harvested
        // row is exact at the harvest epoch only; the border then moves s⁺ = s + w,
        // w ~ N(0, Q), so making the row independent of s⁺ again needs the per-column
        // κ shrink M' = (P+Q)⁻¹P·M and the MᵀQM-style variance inflation.  Card 1
        // applies neither to layout-present columns, the middle case between "static
        // border" and the departed-column (q→∞) marginalization.
        network_carry_coupling_time_update_ =
            jval<bool>(json, "network.carry_coupling_time_update",
                       "carry_coupling_time_update", false);
        if (network_carry_coupling_time_update_ && !network_carry_conditional_coupling_)
            throw std::runtime_error(
                "network.carry_coupling_time_update requires network.carry_conditional_coupling = true "
                "(it only corrects how that card's coupling rows re-map to the moved border).");
        // The exact conditional time update (card CTU).  It conditions every row on the
        // stashed dense border block, so the dense carry must exist; it replaces card 1d's
        // kappa on the rows it can take, so card 1d must be armed for the rest; and card
        // 1b's joint departed-column whitening is a different treatment of the same terms,
        // not built for the exact rule.
        network_carry_coupling_exact_update_ =
            jval<bool>(json, "network.carry_coupling_exact_update",
                       "carry_coupling_exact_update", false);
        if (network_carry_coupling_exact_update_) {
            if (!network_carry_coupling_time_update_)
                throw std::runtime_error(
                    "network.carry_coupling_exact_update requires "
                    "network.carry_coupling_time_update = true (the card 1d kappa stays the "
                    "treatment of the rows and terms the exact rule cannot take).");
            if (!network_shared_carry_full_cov_)
                throw std::runtime_error(
                    "network.carry_coupling_exact_update requires network.shared_carry_full_cov "
                    "= true (the rule conditions each row on the stashed dense border block).");
            if (network_carry_coupling_joint_whiten_)
                throw std::runtime_error(
                    "network.carry_coupling_exact_update refuses "
                    "network.carry_coupling_joint_whiten (card 1b's joint marginalization of the "
                    "departed terms is not built for the exact rule, which re-attributes them).");
            log->info("Network producer (card CTU): coupling-row time update EXACT — each row "
                      "harvested at the stashed border is re-expressed on the moved border by "
                      "M_new = (P_SS + Q_S)^-1 P_S,K M_K with the fold's own inflation Q, the "
                      "bracket variance added per row, decoupled and departed columns "
                      "re-attributed through P, and the clock-family deficit moved onto the "
                      "pinned pivot.  Card 1d's kappa remains for stale rows, terms with no "
                      "dense row and SH terms.");
        }
        // Card PFQ: the carried clocks' process noise in the pivot frame.  The pairwise half
        // lives in the dense fold, so the dense carry is required; the anchor-free clock
        // datums have no pivot and the clock model carries no clock in the border, so both
        // are refused.  Those keys load in later sections and are read raw here.
        network_clock_pivot_frame_q_ =
            jval<bool>(json, "network.clock_pivot_frame_q", "clock_pivot_frame_q", false);
        if (network_clock_pivot_frame_q_) {
            if (!network_shared_carry_full_cov_)
                throw std::runtime_error(
                    "network.clock_pivot_frame_q requires network.shared_carry_full_cov = true "
                    "(card PFQ: two carried clocks share the pivot's walk, a covariance only the "
                    "dense fold carries).");
            if (jval<bool>(json, "network.clock_datum_sum_to_zero",
                           "network_clock_datum_sum_to_zero", false)
                || jval<bool>(json, "network.clock_datum_broadcast_anchored",
                              "network_clock_datum_broadcast_anchored", false))
                throw std::runtime_error(
                    "network.clock_pivot_frame_q refuses network.clock_datum_sum_to_zero and "
                    "network.clock_datum_broadcast_anchored (card PFQ: the term is the pinned "
                    "pivot's walk; an anchor-free clock datum has no pivot).");
            if (jval<bool>(json, "network.clock_model", "network_clock_model", false))
                throw std::runtime_error(
                    "network.clock_pivot_frame_q refuses network.clock_model (card PFQ: the clock "
                    "model carries no satellite clock in the shared border).");
            log->info("Network producer (card PFQ): network.clock_pivot_frame_q = true — the "
                      "carried satellite clocks take their process noise in the pivot frame: "
                      "each folding clock other than the pinned pivot adds the pivot's walk "
                      "r_p dt_j (r_p the model's physical rate: the anchor's q when armed), and "
                      "two such clocks of a "
                      "constellation share r_p min(dt_j, dt_k) in the dense fold and the exact "
                      "update's Q_S.  Counted at the run end.");
        }
        // The same bracket between two rows of a station (card WSX): the exact update's
        // within-station covariance, added in the whitened local block.
        network_carry_coupling_exact_cross_ =
            jval<bool>(json, "network.carry_coupling_exact_cross",
                       "carry_coupling_exact_cross", false);
        if (network_carry_coupling_exact_cross_) {
            if (!network_carry_coupling_exact_update_)
                throw std::runtime_error(
                    "network.carry_coupling_exact_cross requires "
                    "network.carry_coupling_exact_update = true (it adds that rule's bracket "
                    "between two rows of a station; card WSX).");
            log->info("Network producer (card WSX): network.carry_coupling_exact_cross = true — "
                      "the exact update's bracket M_i' B M_k between two rows of a station that "
                      "both took the exact route is added to the station's whitened local "
                      "block (the cross-station half stays out); inert without the local "
                      "block, counted at the run end.");
        }
        // The clock clone (card SCL): the previous epoch's satellite clocks as one-epoch
        // border columns, so the exact rule's rows keep their clock coefficients and the
        // walk enters the border prior once.  It is a mode of the exact rule over the dense
        // block, so both must be armed.  The clock model carries no clock in the border, and
        // the gauge-free fold's direction would have to span the clones; both are refused.
        // The two refused keys are read raw here: their own sections load later.
        network_carry_coupling_clock_clone_ =
            jval<bool>(json, "network.carry_coupling_clock_clone",
                       "carry_coupling_clock_clone", false);
        if (network_carry_coupling_clock_clone_) {
            if (!network_carry_coupling_exact_update_)
                throw std::runtime_error(
                    "network.carry_coupling_clock_clone requires "
                    "network.carry_coupling_exact_update = true (card SCL: the clone is a mode "
                    "of the exact coupling time update).");
            if (!network_shared_carry_full_cov_)
                throw std::runtime_error(
                    "network.carry_coupling_clock_clone requires network.shared_carry_full_cov "
                    "= true (card SCL: the clones stand in for the clocks of the stashed dense "
                    "border block).");
            if (jval<bool>(json, "network.clock_model", "network_clock_model", false))
                throw std::runtime_error(
                    "network.carry_coupling_clock_clone refuses network.clock_model (card SCL: "
                    "the clock model carries no satellite clock in the dense border, so there is "
                    "nothing to clone).");
            if (jval<bool>(json, "network.clock_anchor_gauge_free",
                           "network_clock_anchor_gauge_free", false))
                throw std::runtime_error(
                    "network.carry_coupling_clock_clone refuses network.clock_anchor_gauge_free "
                    "(card SCL: the gauge-free fold's clock direction would have to include the "
                    "clone columns).");
            if (network_shared_carry_gauge_free_)
                throw std::runtime_error(
                    "network.carry_coupling_clock_clone refuses network.shared_carry_gauge_free "
                    "(card SCL: the same gauge-free downdate, armed by the dense carry; its clock "
                    "direction would have to include the clone columns).");
            log->info("Network producer (card SCL): network.carry_coupling_clock_clone = true: "
                      "each satellite clock with a row in the stashed dense block gets a one-epoch "
                      "clone column c(t-1) in the shared border; the coupling rows keep their "
                      "harvested clock coefficients on the clone, the walk enters the border prior "
                      "once at the fold's own variance, the rank gate judges the border with the "
                      "clones marginalized, and the harvest marginalizes them out of every row.  "
                      "UNVALIDATED; run-end line in the card CTU block.");
        }
        // The shared history (card SCL2 X2): the clones' shared conditional, which B1's
        // per-station harvest drops across stations, kept as at most k constant border
        // columns per cloned clock.  It is a mode of B1's harvest, so B1 must be armed (and
        // with it, transitively, the exact update, the dense carry and B2).  The rank is
        // checked whether or not the knob is armed: an out-of-range value is a typo.
        network_carry_coupling_shared_history_ =
            jval<bool>(json, "network.carry_coupling_shared_history",
                       "carry_coupling_shared_history", false);
        network_carry_coupling_shared_history_rank_ =
            jval<int>(json, "network.carry_coupling_shared_history_rank",
                      "carry_coupling_shared_history_rank", 1);
        if (network_carry_coupling_shared_history_rank_ < 1
            || network_carry_coupling_shared_history_rank_ > 8)
            throw std::runtime_error(fmt::format(
                "network.carry_coupling_shared_history_rank = {} is outside [1, 8] (card SCL2 X2: "
                "history columns kept per cloned clock).",
                network_carry_coupling_shared_history_rank_));
        if (network_carry_coupling_shared_history_) {
            if (!network_carry_coupling_clock_clone_)
                throw std::runtime_error(
                    "network.carry_coupling_shared_history requires "
                    "network.carry_coupling_clock_clone = true (card SCL2 X2: the history is the "
                    "clones' shared conditional, kept at B1's harvest).");
            log->info("Network producer (card SCL2 X2): network.carry_coupling_shared_history = "
                      "true, rank {}: at the harvest each carried row's terms on the clones and "
                      "the previous history are regressed onto the free border and compressed "
                      "across stations into at most {} history column(s) per cloned clock "
                      "(prior N(0, 1), never emitted or stashed); the remainder is added per "
                      "station and its cross-station part dropped (counted).  UNVALIDATED; "
                      "run-end line in the card CTU block.",
                      network_carry_coupling_shared_history_rank_,
                      network_carry_coupling_shared_history_rank_);
        }
        // Card BCL: the clone over the rest of the stashed border, the walking biases (BCL-a)
        // and the keys card CTU re-attributes (BCL-d).  Both are modes of B1's clone.
        network_carry_coupling_bias_clone_ =
            jval<bool>(json, "network.carry_coupling_bias_clone", "carry_coupling_bias_clone",
                       false);
        network_carry_coupling_departed_clone_ =
            jval<bool>(json, "network.carry_coupling_departed_clone",
                       "carry_coupling_departed_clone", false);
        if ((network_carry_coupling_bias_clone_ || network_carry_coupling_departed_clone_)
            && !network_carry_coupling_clock_clone_)
            throw std::runtime_error(fmt::format(
                "network.carry_coupling_{}_clone requires network.carry_coupling_clock_clone = "
                "true (card BCL: the clones of the rest of the border are a mode of card SCL's "
                "clock clone).",
                network_carry_coupling_bias_clone_ ? "bias" : "departed"));
        if (network_carry_coupling_bias_clone_)
            log->info("Network producer (card BCL-a): network.carry_coupling_bias_clone = true: "
                      "each satellite phase and code bias with a row in the stashed dense block "
                      "gets a one-epoch clone column; the rows keep their bias coefficients on "
                      "it and each bias walk enters the border prior once, in the clones' joint "
                      "walk factor.  UNVALIDATED; run-end line in the card CTU block.");
        if (network_carry_coupling_departed_clone_)
            log->info("Network producer (card BCL-d): network.carry_coupling_departed_clone = "
                      "true: a stashed key the fold does not carry (departed, reinit, "
                      "re-referenced, skipped) keeps the rows' terms on a one-epoch clone with "
                      "the stashed prior and no walk, instead of card CTU's per-row "
                      "re-attribution.  UNVALIDATED; run-end line in the card CTU block.");
        // Full marginalization of the coupling rows' SH-VTEC terms (card 1d-SH).  The SH
        // border is a mean-reverting GM (climatology-anchored), not the pure random walk
        // the κ update models, so shrink-carrying its terms compounds row weight against
        // the anchor (the ~4 h a₀₀ runaway in the 12 h low-floor runs).  The
        // q=∞ / departed treatment removes that feedback; it is conservative and always
        // consistent.
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
        // Gauge projection of a truncated coupling row (card 1f).  A harvested row is
        // gauge-invariant by the identity Mᵀg_s = −g_r(c): it observes the datum gauge
        // exactly as much as its own A_r = +1 local requires.  Deleting a departed
        // column's coefficient breaks that: the family sum moves to −M_dep, and the row
        // becomes a high-weight observation of the current gauge pinned at the old
        // realization.  Measured (simnet24, TOW 305220, 7 G-clock departures): the
        // smallest null eigenvalue went from 1e-11 to 9.43e-3 in one epoch, five
        // decades over the gate cutoff and level with the weakest real PB layer, which
        // gave 4 spurious over-specified withholds that cleared only at pl_max_gap_s
        // through blanket carry expiry.  The fix holds each (constellation, kind)
        // coefficient sum at its harvest value, which subsumes card 1d's κ
        // re-projection and reduces to it exactly when nothing was truncated.  The
        // target is not zero: it is −g_r(c), and a Galileo ambiguity row measures
        // Σ_E = −1 exactly.
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
        // Feeds the coupling reinit history from every realization-step source
        // (phase-anchor re-reference, eq34 chain re-reference, datum-slip PB
        // re-realization) and honours it with the κ time update off (card 1g).  Each of
        // those erases the re-referenced family's own carry but leaves every other
        // carried local's coupling[] entry pointing at that column with a retired-frame
        // ŝ_prev, which buildCplRow would fold at full weight, carrying the stale
        // ŝ_prev onto the wire (30 PBGAUGE + 27 CLK_STEP re-realizations in 120
        // epochs).  A re-realized column's step is unknown and cannot be transformed,
        // so the only correct treatment is the departed route (κ = 0); card 1f then
        // repairs the gauge deficit that marginalization opens.
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
        // Folds the same carried ZWD as one more column of the station's F3 whitened
        // block instead of as a diagonal row outside it (B9-in-block; the §7 item 17 rule
        // "block all carried local columns": a diagonal ZWD row double-counts the
        // station-common direction the ambiguity rows already assert).  Validated below.
        network_carry_zwd_in_block_ =
            jval<bool>(json, "network.carry_zwd_in_block", "carry_zwd_in_block", false);
        // The same rule applied to the per-LOS slant-iono columns (I-in-block; SPEC_amb_
        // iono_split_overcount §7 item 17).  Not a sub-knob of the legacy
        // network.carry_iono, which is the diagonal vehicle this replaces; the two are
        // mutually exclusive (validated below).  q is per epoch, m² at L1.
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
        // The same "block all carried local columns" rule applied to the station's
        // receiver clock and, under the sub-knob, its ISB columns (RX-CLK-in-block).
        // On this path the clock is a local A_r column with no temporal prior
        // (filter.process_noise.clock_bias_q is inert here; measured byte-identical), so
        // it is the free per-epoch sink into which the ZWD and iono cards push their
        // residual rho-direction error.  q is per epoch, m^2; the drift is a carried
        // state with its own variance ladder (see the getters).  Validated below.
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
        // Seam-scoped ZWD re-seed (SPEC_synthetic_minimum_network §17, finding 3 of the
        // carry_zwd re-base arms): the resume-path-only successor to the continuous B9
        // carry.  Injection window and horizon; the write side (Z rows in every
        // snapshot) is unconditional and has no knob.
        network_zwd_seam_reseed_ =
            jval<bool>(json, "network.zwd_seam_reseed", "zwd_seam_reseed", false);
        network_zwd_seam_reseed_epochs_ =
            jval<int>(json, "network.zwd_seam_reseed_epochs", "zwd_seam_reseed_epochs", 480);
        // The seam window's own stiffness (RS-10b; docs/SPEC_event_harness.md §21.21).
        // network.zwd_carry_q is in the warm-resume fingerprint (a continuous carry q
        // must cold-start), while the seam response that borrows its weight is not, so
        // without this knob the one parameter the response exists to tune could not be
        // varied on a resume leg.  Negative (the -1 default) = borrow zwd_carry_q, so
        // byte-identical when unset; positive = used instead, and only inside the live
        // seam window.  Not fingerprinted, the same exemption class as zwd_seam_reseed.
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
        // Resume frame restore and post-resume slip warm-up (SPEC_synthetic_minimum_
        // network §18.10).  Both are forward-only resume response policies (not
        // fingerprinted), and both are inert on a run that never resumes.
        network_resume_frame_restore_ =
            jval<bool>(json, "network.resume_frame_restore", "resume_frame_restore", false);
        network_resume_slip_warmup_ep_ =
            jval<int>(json, "network.resume_slip_warmup_ep", "resume_slip_warmup_ep", 0);
        if (network_resume_slip_warmup_ep_ < 0)
            throw std::runtime_error("network.resume_slip_warmup_ep must be >= 0");
        // The declaration half of the same seam (RS-4, §21.9).  The CLK-R resume_seam
        // mark is suppressed exactly when the frame restore works (the held-frame
        // engine comes back with its epochs), so a lineage whose clock is continuous
        // and whose phase-bias level is not emits datum_changed 0.  Same exemption
        // class as the two knobs above: forward-only resume response policy, not
        // fingerprinted, and unreachable on a run that never resumes.
        network_resume_pb_seam_mark_ =
            jval<bool>(json, "network.resume_pb_seam_mark", "resume_pb_seam_mark", false);
        // The same idea one layer up (RS-2b, §21.6): the estimation-side fine-slip gate
        // becomes diagnostic-only for K post-resume epochs.  Same exemption class as the
        // drain warm-up above: forward-only resume response policy, inert without a
        // resume.
        network_resume_d5_warmup_ep_ =
            jval<int>(json, "network.resume_d5_warmup_ep", "resume_d5_warmup_ep", 0);
        if (network_resume_d5_warmup_ep_ < 0)
            throw std::runtime_error("network.resume_d5_warmup_ep must be >= 0");
        if (network_resume_d5_warmup_ep_ > 0)
            log->info("Network (RS-2b): resume D5 warm-up armed — for the first {} PROCESSED "
                      "epoch(s) after a warm resume the Card-4 D5 innovation gate is "
                      "DIAGNOSTIC-ONLY: a chord that trips k·σ̂₀ is counted (run-end "
                      "d5_warmup_suppressed and the epoch's n_d5_flags — n_d5_retaps stays "
                      "0) but is NOT "
                      "re-tapped, NOT marked slipped for the next epoch, and its block is "
                      "not refolded.  A MEASUREMENT knob: it holds the restored branch "
                      "through the seam so \"branch right, cold station layer wrong\" can be "
                      "told from \"branch inconsistent\".  The σ̂₀ accumulator is fed BEFORE "
                      "the decision (calibration is not censored), the v3 corroboration gate "
                      "is untouched, and the MW/LLI/IF-TD drains stay live — a genuine "
                      "in-gap slip keeps a response.  Cold starts never arm it; NOT in the "
                      "warm-resume fingerprint.",
                      network_resume_d5_warmup_ep_);
        // Card-1 coupling-regressor restore, the read side of
        // network.state_snapshot_coupling_capture (§18.12).  Same exemption class as the
        // two above: forward-only resume response policy, inert on a run that never
        // resumes.
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
        // Datum restore, the read side of network.state_snapshot_datum_capture (RS-3,
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
        // Emit-layer restore, the read side of network.state_snapshot_emit_capture
        // (RS-5, SPEC_event_harness §21.10).  Same exemption class as the restores
        // above: forward-only resume response policy, inert on a run that never resumes.
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
        // Station-layer restore, the read side of network.state_snapshot_station_capture
        // (RS-6, SPEC_event_harness §21.13).  Same exemption class as the restores
        // above: forward-only resume response policy, inert on a run that never
        // resumes.  The two sub-switches are diagnostic (default true): they let one
        // leg of the attribution matrix restore a single half, and they mean nothing
        // without the read knob itself.
        network_resume_station_restore_ =
            jval<bool>(json, "network.resume_station_restore", "resume_station_restore",
                       false);
        network_resume_station_restore_rxcb_ =
            jval<bool>(json, "network.resume_station_restore_rxcb",
                       "resume_station_restore_rxcb", true);
        network_resume_station_restore_shdev_ =
            jval<bool>(json, "network.resume_station_restore_shdev",
                       "resume_station_restore_shdev", true);
        // Dense-border restore, the read side of the snapshot's QK/QP records (RS-7,
        // SPEC_event_harness §21.14).  Same exemption class as every restore above:
        // forward-only resume response policy, inert on a run that never resumes.
        //
        // Not gated on network.carry_coupling_gauge_project (card 1f) or
        // network.carry_conditional_coupling, on purpose.  One capture knob writes two
        // independent populations.  The coupling rows (QL/QZ) are per-row regressions
        // on the border, so a truncated one breaks the family identity
        // Mᵀg_s = −g_r(c) and needs the 1f repair; they stay behind
        // resume_coupling_restore and its interlock.  The QK/QP block restored here is
        // a covariance: it carries no regression, no gauge identity and no family sum,
        // so it is legal on a carry_invariant lineage, where 1f is out of regime and
        // resume_coupling_restore is refused.  This split lets a field warm resume
        // restore its dense border.
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
        // Canonical-signal restore, the read side of
        // network.state_snapshot_signal_capture (RS-8, SPEC_event_harness §21.16).  Same
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
        // rx-CB linearization-point restore, the read side of
        // network.state_snapshot_rxcb_lin_capture (RS-9, SPEC_event_harness §21.18).
        // Same exemption class as every restore above: forward-only resume response
        // policy, inert on a run that never resumes.
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
        // Resume carry re-base (RS-2, SPEC_event_harness §21.4), the read side of
        // network.state_snapshot_carry_lin, and the same exemption class as the
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
        // Re-admission continuity: E2 seed-first re-admission and the block re-coupling
        // edge (SPEC_synthetic_minimum_network §18.4, SPEC_amb_iono_split_overcount §7
        // FIRST).  Both default off = byte-identical; neither is fingerprinted (parking
        // adds entries and changes no carried value's meaning; the windows restart
        // empty).  Cross-knob checks live in the armed summary below, after every knob
        // is parsed.
        network_readmit_seed_horizon_s_ =
            jval<double>(json, "network.readmit_seed_horizon_s", "readmit_seed_horizon_s", 0.0);
        if (!(network_readmit_seed_horizon_s_ >= 0.0))
            throw std::runtime_error("network.readmit_seed_horizon_s must be >= 0");
        network_readmit_coupling_marg_ep_ =
            jval<int>(json, "network.readmit_coupling_marg_ep", "readmit_coupling_marg_ep", 0);
        if (network_readmit_coupling_marg_ep_ < 0)
            throw std::runtime_error("network.readmit_coupling_marg_ep must be >= 0");
        // Sub-knob of the seed horizon: park and seed the code-bias columns too (the
        // default), or the clock alone (on a 24 h run the first seeded return rejected
        // the third-frequency C5Q seed at 43 σ while the IF-pair seeds held).
        network_readmit_seed_code_bias_ =
            jval<bool>(json, "network.readmit_seed_code_bias", "readmit_seed_code_bias", true);
        // Canonical-pair admission (card CPA;
        // docs/refound/CPA_canonical_pair_admission_2026-09-20.md).  Default off =
        // byte-identical; not fingerprinted.  The tolerance is clamped rather than
        // refused: 0 is a meaningful arm (admit only the epochs that hold the pair),
        // and a negative value has no other reading.
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
        // Armed/inert status line of the datum restore (RS-3).  Its prerequisite is
        // checked above, so here it can only be inert for lack of a snapshot to resume
        // from.
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
        // Armed/inert status line of the emit-layer restore (RS-5, §21.10).  Its
        // prerequisite is checked above, so here it can only be inert for lack of a
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
                          "values instead of first-sighting every key.  "
                          "The rows are captured under "
                          "network.state_snapshot_emit_capture; a file without them "
                          "leaves the re-base exactly as today and says so.  Outputs-only "
                          "(no estimate moves) and not in the warm-resume fingerprint "
                          "(forward-only resume response policy).");
        }
        // Armed/inert status line of the station-layer restore (RS-6, §21.13).  Its
        // prerequisite is checked above, so here it can only be inert for lack of a
        // snapshot to resume from.  The sub-switches are named only when one is off: an
        // attribution leg has to describe itself in the log, and the ordinary
        // both-halves arm does not need a clause that says nothing.
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
        // Armed/inert status line of the canonical-signal restore (RS-8, §21.16), beside
        // the RS-6 one.  Its prerequisite is checked above, so here it can only be inert
        // for lack of a snapshot to resume from.
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
        // Armed/inert status line of the rx-CB linearization-point restore (RS-9,
        // §21.18).  Both prerequisites are hard throws above, so here the knob can only
        // be inert for lack of a snapshot to resume from.
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
        // Armed/inert status line of the dense-border restore (RS-7, §21.14), same
        // shape.  The full-cov prerequisite is warn-disarmed above, so here the knob can
        // only be inert for lack of a snapshot to resume from.
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
        // Armed/inert status line of the declaration half (RS-4, §21.9), same shape.
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
        // Slant-iono carry (QUEUE 8b; docs/SPEC_slip_relevel_walk.md s8 residual):
        // carries per-LOS slant iono across epochs as a diagonal RW prior through the
        // same addStationPrior path.  A new carry seam: it is not the phantom
        // filter.process_noise.ionosphere_q (which has no network reader) and not the
        // falsified adaptive q.
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
        // What feeds the carried value (the CS-GF authority, successor to the falsified
        // i1; SPEC_iono_carry_8b §6.2).  "solve" = the est-reconstructed per-LOS iono
        // (measured to accumulate the level-locked displacement); "smoothed_gf" = the
        // carrier-smoothed GF-code authority (level-independent; drains toward the
        // code realization).
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
        // RXCB-IN-BLOCK (card CTU section 14): the carried receiver code-bias columns
        // folded inside the station's whitened local block with their border regression,
        // instead of the diagonal fold at the conditional variance.  Validated in the
        // post-parse block after the carry's own guard (which can still force it off).
        network_carry_rx_code_bias_in_block_ =
            jval<bool>(json, "network.carry_rx_code_bias_in_block",
                       "carry_rx_code_bias_in_block", false);
        // De-means the rx-CB carry's common mode (card LV-2;
        // docs/SPEC_level_information_budget.md).  Parsed next to the carry it modifies;
        // its guard and banner live in the post-parse block with the carry's own
        // (carry_rx_code_bias can still be forced off there by the
        // code_datum_time_linked guard, and a de-mean of a disarmed carry must read as
        // inert, not as armed).
        network_rx_cb_carry_demean_ =
            jval<bool>(json, "network.rx_cb_carry_demean", "rx_cb_carry_demean", false);
        network_rx_cb_carry_demean_min_stations_ =
            jval<int>(json, "network.rx_cb_carry_demean_min_stations",
                      "rx_cb_carry_demean_min_stations", 8);
        // Reduces μ between PASS A and PASS B instead of at the harvest, which removes
        // the lag-1 delay (card LV-5; docs/SPEC_level_information_budget.md §12).
        network_rx_cb_carry_demean_within_ =
            jval<bool>(json, "network.rx_cb_carry_demean_within",
                       "rx_cb_carry_demean_within", false);
        // Replaces the carry's frame-dependent inject screen with the frame-invariant
        // harvest-side step (card LV-4; docs/SPEC_level_information_budget.md §11).
        network_rx_cb_carry_step_gate_ =
            jval<bool>(json, "network.rx_cb_carry_step_gate", "rx_cb_carry_step_gate", false);
        network_rx_cb_carry_step_k_sigma_ =
            jval<double>(json, "network.rx_cb_carry_step_k_sigma",
                         "rx_cb_carry_step_k_sigma", 10.0);
        // The S-consistent harvest: a post-solve Λ-weighted de-mean of the harvested
        // absolute rx-CB realization, per family (card S2H;
        // docs/SPEC_level_information_budget.md §17 item 3).  Guard and banner are in
        // the post-parse block with the other LV cards (the carry can still be forced
        // off there).
        network_rx_cb_harvest_demean_ =
            jval<bool>(json, "network.rx_cb_harvest_demean", "rx_cb_harvest_demean", false);
        network_rx_cb_harvest_screen_z_ =
            jval<double>(json, "network.rx_cb_harvest_screen_z",
                         "rx_cb_harvest_screen_z", 8.0);
        // NOTE: validation (datum guard and banner) lives in the post-parse block next
        // to the carry_zwd validation, because code_datum_time_linked is parsed after
        // this site.
        // Carry-row variance-component monitor (M2, LS-VCE carry-q).  A calibration
        // instrument: it accumulates carry innovations and logs σ̂ against the hand
        // floors, with no feedback into the solve (v1).  Default off keeps the
        // accumulator and logging inert, so the output is byte-identical.
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
        // Variance-factor apply (M2 v2; docs/SPEC_carry_vce_apply.md): LASER 5-95's
        // variance-factor iteration.  Every carry fold variance is multiplied by a
        // per-(family × constellation) α² that tracks the monitor's measured
        // robust_floor / hand_fold_var, inflate-only and slew-limited.  Unlike the
        // monitor this changes the solve, so it is fingerprinted, and the banner says
        // so.
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
            // A family with no monitor has no σ̂ to apply.  This is a hard error rather
            // than a silent unity factor: an inert apply that logs clean is the
            // dead-wiring failure invariant #4 forbids.
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
            // NOTE: the `pl` inert guard is not here: it needs network_carry_invariant_,
            // which is parsed in a later helper.  It lives in the post-parse block next
            // to the phase-leveling B4 floor banner.
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

        // Prior-residual census (docs/SPEC_prior_residual_census.md).  The M2 monitor
        // above pools the per-row partial redundancies into a variance component; this
        // census tests them one by one and reports the empirical exceedance ladder per
        // (family × label).  Same hook, same arithmetic, no new solve, and, like the
        // monitor, no feedback: the solve is byte-identical with it armed.  Parsed
        // here, next to carry_vce, because the two share the statistic and the family
        // names.
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
            // Constraint-aware global variance component
            // (docs/SPEC_prior_residual_census.md §14), a rider on the census
            // accumulator, parsed with it because it shares its rows.  The
            // raim_redundancy_normalize tripwire cannot live here: loadNetworkQcTiers_
            // runs after this helper and has not resolved that flag yet, so the
            // tripwire sits at the tail of that helper next to the MDB precondition
            // (the section's load-order rule).
            pnc.prior_residual_global_sigma0 =
                jval<bool>(json, "network.prior_residual_global_sigma0",
                           "prior_residual_global_sigma0", false);
            // Invariant #4 tripwire: a corroboration arm without the census would
            // silently produce nothing.  A hard error rather than a silent disarm: a
            // config that asks for the contingency table and gets an empty run is the
            // "live counter over dead wiring" failure.
            if (pnc.prior_residual_corroborate && !pnc.prior_residual_census)
                throw std::runtime_error(
                    "network.prior_residual_corroborate requires network.prior_residual_census = true "
                    "(the corroboration join runs inside the census; arming it alone produces no rows, "
                    "no contingency table and no warning at run time).");
            // Same tripwire, same reason: the prior side of the global form is the
            // census's folded rows, so without the census it has no numerator and no
            // `l`, and it would emit a data-only number under a name that promises a
            // constraint-aware one, which is worse than emitting nothing.
            if (pnc.prior_residual_global_sigma0 && !pnc.prior_residual_census)
                throw std::runtime_error(
                    "network.prior_residual_global_sigma0 requires network.prior_residual_census = true "
                    "(the global form's prior side ẽ₀ᵀP₀ẽ₀ and its constraint count `l` ARE the census's "
                    "folded rows; without the census the column would carry a DATA-ONLY variance factor "
                    "under a name that promises a constraint-aware one).");
            if (pnc.prior_residual_census) {
                // Invariant-#4 tripwire: a family with no hook in this build.  The
                // census would report it as "armed, zero rows" at run end, which is
                // indistinguishable from a real negative result, so it is named here,
                // at load, where the typo can be fixed.
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
                // NOTE: the INC-3 auto-arm of network.cycle_slip.gap_instrument is not
                // here: loadNetworkSlipDetectors_ runs after this helper and would
                // overwrite it.  Per the section's load-order rule ("a later helper
                // reads what an earlier one resolved"), it lives at the tail of that
                // helper and reads the flag this block just set.
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
    }

    // =================================================================
    //  network subsection helper: loadNetworkSlipDetectors_
    // =================================================================
    /**
     * @brief Cycle-slip detectors (@c network.cycle_slip.*) and the slip-QC hardening cards R/L/G.
     *
     * Ends with the auto-arm that invariant #1 requires: phase leveling carries carrier
     * phase as a near-random constant, so a config that enables it with no pre-fit detector
     * gets @c network.cycle_slip.mw_detect armed (the robust choice, since it needs no
     * a-priori position).
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkSlipDetectors_(const nlohmann::json& json,
                                               const std::shared_ptr<spdlog::logger>& log) {
        // Cycle-slip repair (T0, increment 2): meaningful only with the carry on.
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
        // Which MW pairs the network forms (card L5P).  Detection-shaping when armed,
        // and the default is the every-pair detector, so an unarmed run is
        // byte-identical (invariant #6).  Same loader shape as if_phase_td_pairs
        // below: a typo must not silently narrow a detector, so an unknown value is
        // named in an error and replaced by the default.
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
        // IF-phase triple-difference slip detection: between-satellite, between-epoch
        // ionosphere-free phase OMC.  Detection-only: a flagged IF pair refloats both
        // band ambiguities (pending-reseed) so the carry re-estimates them from data;
        // no MW/GF confirmation, no integer repair.  Off is byte-identical; meaningful
        // for products only with the carry on.
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
        // How many band pairs the IF-TD detector evaluates per satellite (§14.10).
        // Detection-shaping when armed, and the default is the single pair from before
        // the knob, so an unarmed run is byte-identical (invariant #6).  A typo must
        // not silently widen or narrow a detector, so an unknown value is logged as an
        // error and the default is taken.
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

        // --- Slip re-level response (SPEC_slip_relevel_walk §6) ----------------
        // Root defect: a detector's pending-reseed mark is consumed by the PASS-B
        // measurement pre-pass before the leveling-carry inject reads it, so without
        // these knobs the documented "re-float from data this epoch" does not happen:
        // the stale pre-slip level is injected at converged weight and the level
        // channels random-walk (~6.5 m ZWD / 6 h at a realistic slip rate).
        //   relevel_free_epoch: latches the marked set where the inject site can
        //     still see it, so a detector-marked arc's carry is not folded that
        //     epoch (the free epoch the mark promised).
        //   relevel_delta_bridge: better than freeing when the delta was measured;
        //     shifts the carried level by the float per-band solve of (MW wide-lane
        //     integer, IF-TD phase step) so the arc keeps its level at phase
        //     precision.  Every fallback lands on the free epoch.
        // Both default off, byte-identical when off, and exempt from the fingerprint
        // (forward-only response policy; see networkSnapshotFingerprint's exemption
        // list).
        network_cycle_slip_relevel_free_epoch_ =
            jval<bool>(json, "network.cycle_slip.relevel_free_epoch",
                       "network_cycle_slip_relevel_free_epoch", false);
        // --- Anchor-slip handover (SPEC_event_harness §12.6/§12.7) ---------------
        // Root defect (measured, Study 4): every (sat, signal) phase-bias family is
        // realized at one station's arc, the anchor the sticky election elected.  When
        // a slip response frees that arc, the election keeps the incumbent (the ban
        // that would move it exists, but without this knob only the harness injector
        // stages one), and the family's gauge is re-realized from the freed arc's
        // one-epoch code re-level: 157 T4 erases on healthy arcs at eight other
        // stations, 10.7 m on the station's own clock, a 9 m step in the satellite's
        // emitted clock.  Armed, the freed anchor's family is banned for that epoch
        // only, so the production election picks a successor by its own rules and the
        // continuous branch pins it at its own carried value_abs; the same slip then
        // costs 0 erases / 2 mm / 1 cm.  Default off, byte-identical when off, and
        // exempt from the fingerprint (a response knob: it changes only what a run
        // does after a slip, never the state a snapshot restores).  The interlocks sit
        // in the late band of loadNetworkSection_ (the phase-datum mode flags are
        // parsed well after this block).
        network_config_.cycle_slip_anchor_slip_handover =
            jval<bool>(json, "network.cycle_slip.anchor_slip_handover",
                       "network_cycle_slip_anchor_slip_handover", false);
        // --- MW evidence refloat ------------------------------------------------
        // Root defect (measured on the event harness): the PASS-A MW admission reuses
        // the T0 integer-repair gate (known integer, frac <= turbo_edit_frac_max) on a
        // detect-only path whose consumer, the refloat drain, never applies the
        // integer.  Rounding cleanliness is the wrong test there: a -99 cycle L5 slip
        // flagged at 78x threshold with the integer known and frac 0.183 (1.07 m on
        // the 5.86 m lane, inside its own 1 sigma) was dropped, as was the same event
        // on a second lane at 8.9x with an unknown integer, with no counter and no
        // event row.  Armed, a flag is admitted on its own strength instead: bias-free
        // lane, mature arc, |dev|/threshold above the gate.  Default off and
        // byte-identical when off; the counters are collected either way (an off run
        // reports its would-admit population).  The interlock on mw_detect sits in the
        // late band of loadNetworkSection_.
        network_config_.cycle_slip_mw_evidence_refloat =
            jval<bool>(json, "network.cycle_slip.mw_evidence_refloat",
                       "network_cycle_slip_mw_evidence_refloat", false);
        network_config_.cycle_slip_mw_evidence_ratio_min =
            jval<double>(json, "network.cycle_slip.mw_evidence_ratio_min",
                         "network_cycle_slip_mw_evidence_ratio_min", 3.0);
        network_config_.cycle_slip_mw_evidence_min_arc_epochs =
            jval<int>(json, "network.cycle_slip.mw_evidence_min_arc_epochs",
                      "network_cycle_slip_mw_evidence_min_arc_epochs", 10);
        // Common-band scope (SPEC_event_harness §14).  Default off is byte-identical:
        // no verdict object is built and no drain consults one.
        network_config_.cycle_slip_common_band_scope =
            jval<bool>(json, "network.cycle_slip.common_band_scope",
                       "network_cycle_slip_common_band_scope", false);
        network_config_.cycle_slip_common_band_min_arc_epochs =
            jval<int>(json, "network.cycle_slip.common_band_min_arc_epochs",
                      "network_cycle_slip_common_band_min_arc_epochs", 10);
        // Reference-arc clause (§22), which covers the scope's IF-TD dead zone.  Default
        // off is byte-identical: the untested set is never populated.
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
        // Bridge v2 (pend-and-replace, SPEC §8.2): the corroboration gate (the bridged
        // level must agree with the interim free re-level within this many cycles per
        // band; correct bridges err ≤ 0.5 cyc against truth, the wrong ones in the §8.1
        // audit 6–113 cyc) and the pend expiry window (the MW integer arrives 1–2
        // epochs after the T1 flag).
        network_cycle_slip_relevel_bridge_corrob_cyc_ =
            jval<double>(json, "network.cycle_slip.relevel_bridge_corrob_cyc",
                         "network_cycle_slip_relevel_bridge_corrob_cyc", 3.0);
        network_cycle_slip_relevel_bridge_pend_window_s_ =
            jval<double>(json, "network.cycle_slip.relevel_bridge_pend_window_s",
                         "network_cycle_slip_relevel_bridge_pend_window_s", 120.0);
        // Bridge v3 (the better second equation): the GF-phase-TD float row (E4, which
        // closes the (1,2) plane in the same epoch without waiting for MW, with zero
        // integer-hypothesis risk).  The EWL-MW integer slot (E3) is retired.  E5
        // (multi-epoch MW accumulation) was refused at design review: the
        // λ_IF-amplified cap needs σ_WL ≤ 0.11 cyc ⇒ el ≥ ~48°, above where any
        // fixture slip lives, and a rounded belated float re-imports mw_only.
        network_cycle_slip_relevel_bridge_gf_ =
            jval<bool>(json, "network.cycle_slip.relevel_bridge_gf",
                       "network_cycle_slip_relevel_bridge_gf", false);
        network_cycle_slip_relevel_bridge_gf_sigma_floor_m_ =
            jval<double>(json, "network.cycle_slip.relevel_bridge_gf_sigma_floor_m",
                         "network_cycle_slip_relevel_bridge_gf_sigma_floor_m", 0.01);
        // Interlocks (warn and disarm: an arm that cannot change anything is disabled
        // here rather than discovered after a field run):
        if (network_cycle_slip_relevel_free_epoch_ && !network_phase_leveling_) {
            log->warn("Network: network.cycle_slip.relevel_free_epoch needs "
                      "network.phase_leveling (its target is the leveling carry) "
                      "— disabled.");
            network_cycle_slip_relevel_free_epoch_ = false;
        }
        if (network_cycle_slip_relevel_delta_bridge_
            && !network_cycle_slip_relevel_free_epoch_) {
            log->warn("Network: network.cycle_slip.relevel_delta_bridge requires "
                      "network.cycle_slip.relevel_free_epoch (its fallbacks land on "
                      "the free epoch) — disabled.");
            network_cycle_slip_relevel_delta_bridge_ = false;
        }
        // windup_restart_refloat (SPEC_phase_windup_sign §10.4): the wired response to
        // an accumulator restart (an exact integer-cycle step on every signal of the
        // satellite, MW-blind because it is band-common).  Refloats the stepped arcs at
        // the restart epoch, from the accumulator's own verdict.  The restart verdict
        // exists only in accumulated mode, so on a fractional lineage the knob is a
        // structural zero (the armed-but-inert case of invariant #4), hence the disarm
        // below.
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
        // IF-TD pair width (§14.10): interlock and arm line, for the same reason as the
        // band-scope clauses: without the detector there is no pair to widen, so 'all'
        // would be a silently inert knob rather than a structural zero.
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
        if (network_cycle_slip_relevel_bridge_gf_
            && !network_cycle_slip_relevel_delta_bridge_) {
            log->warn("Network: network.cycle_slip.relevel_bridge_gf is a v3 rider on "
                      "cycle_slip.relevel_delta_bridge, which is off — disabled.");
            network_cycle_slip_relevel_bridge_gf_ = false;
        }

        // --- Slip-QC hardening cards R / L / G (docs/SPEC_slip_qc_hardening.md) ---
        // Three independent, default-off qualifications of decisions the detector
        // arms above already make.  None of them drops a station, satellite or
        // observation (project rule 5), and none derives a threshold from the
        // stochastic model (rule 4).  All off ⇒ byte-identical.
        {
            auto& sq = network_config_.slip_qc;
            // Hardened IF-TD reference-satellite gate (card R).  Replaces the fixed
            // 0.5·λ_IF, n≥2 gate, which re-fires every epoch on a sub-slip drift.
            sq.ref_gate = jval<bool>(json, "network.cycle_slip.if_phase_td_ref_gate",
                                     "network_cycle_slip_if_phase_td_ref_gate", false);
            // The reference arm on the additional chains (REF-FAN, §22.6).  Interlocked
            // below (needs pairs = all and the empirical gate).
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
            // LLI corroboration in the MW drain (card L).
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
            // Gap-coincident instrumentation (card G; counters and trace rows only).
            sq.gap_instrument = jval<bool>(json, "network.cycle_slip.gap_instrument",
                                           "network_cycle_slip_gap_instrument", false);
            // Gap re-entry response (card G, §G3/W1): the wired invariant-#1 response to
            // the bridged re-entry that card G measures.  Unlike every other knob in this
            // struct it changes the solve: it marks ambiguities slipped.
            sq.gap_reentry =
                jval<bool>(json, "network.cycle_slip.gap_reentry_relevel",
                           "network_cycle_slip_gap_reentry_relevel", false);
            sq.gap_reentry_lli =
                jval<bool>(json, "network.cycle_slip.gap_reentry_relevel_lli",
                           "network_cycle_slip_gap_reentry_relevel_lli", true);
            sq.gap_reentry_unmeasurable =
                jval<bool>(json, "network.cycle_slip.gap_reentry_relevel_unmeasurable",
                           "network_cycle_slip_gap_reentry_relevel_unmeasurable", false);
            // The same response keyed on the signal (card GRS).  A rider of the knob
            // above: the row-level tier owns the de-duplication list this one skips
            // against, so it is inert (and warns) without it.
            sq.gap_reentry_per_signal =
                jval<bool>(json, "network.cycle_slip.gap_reentry_per_signal",
                           "network_cycle_slip_gap_reentry_per_signal", false);
            sq.gap_reentry_per_signal_unmeasurable =
                jval<bool>(json, "network.cycle_slip.gap_reentry_per_signal_unmeasurable",
                           "network_cycle_slip_gap_reentry_per_signal_unmeasurable", true);
            // The per-signal tier's co-resumed share: the signals of one line of sight
            // that return at the same epoch are one re-acquisition, so a verdict on one
            // frees the others.  A rider of the per-signal knob above, inert (and warned)
            // without it.
            sq.gap_reentry_coresumed_share =
                jval<bool>(json, "network.cycle_slip.gap_reentry_coresumed_share",
                           "network_cycle_slip_gap_reentry_coresumed_share", false);
            // The arc-level tracker's per-band signal, chosen from the model rather than
            // lexically, so the tracker, the carried ambiguity key and the gap re-entry
            // response name the same signal (card GRK).
            sq.tracker_modeled_first =
                jval<bool>(json, "network.cycle_slip.tracker_modeled_first",
                           "network_cycle_slip_tracker_modeled_first", false);
            // Arc-scoped GF jump trigger (card ARL): the pre-solve witness of the slips
            // MW and the IF-phase TD both miss.  Changes the solve; default off.
            sq.gf_arc_jump =
                jval<bool>(json, "network.cycle_slip.gf_arc_jump",
                           "network_cycle_slip_gf_arc_jump", false);
            sq.gf_arc_jump_m =
                jval<double>(json, "network.cycle_slip.gf_arc_jump_m",
                             "network_cycle_slip_gf_arc_jump_m", 0.04);
            sq.gf_arc_jump_k =
                jval<double>(json, "network.cycle_slip.gf_arc_jump_k",
                             "network_cycle_slip_gf_arc_jump_k", 0.0);
            sq.gf_arc_jump_window =
                jval<int>(json, "network.cycle_slip.gf_arc_jump_window",
                          "network_cycle_slip_gf_arc_jump_window", 20);
            sq.gf_arc_jump_min_count =
                jval<int>(json, "network.cycle_slip.gf_arc_jump_min_count",
                          "network_cycle_slip_gf_arc_jump_min_count", 10);
            // A bar that is not a positive finite number would flag every step (or none)
            // and read as a detector result; a typo is named and the default taken.
            if (!(sq.gf_arc_jump_m > 0.0) || !std::isfinite(sq.gf_arc_jump_m)) {
                log->error("Network (GF arc jump, card ARL): network.cycle_slip.gf_arc_jump_m "
                           "= {} is not a positive finite length; the default 0.04 m is used.",
                           sq.gf_arc_jump_m);
                sq.gf_arc_jump_m = 0.04;
            }
            if (!(sq.gf_arc_jump_k >= 0.0) || !std::isfinite(sq.gf_arc_jump_k)) {
                log->error("Network (GF arc jump, card ARL): network.cycle_slip.gf_arc_jump_k "
                           "= {} is negative or non-finite; 0 (the scale term off) is used.",
                           sq.gf_arc_jump_k);
                sq.gf_arc_jump_k = 0.0;
            }
            if (sq.gf_arc_jump_window < 1) {
                log->error("Network (GF arc jump, card ARL): network.cycle_slip.gf_arc_jump_window "
                           "= {} is below 1; the default 20 is used.", sq.gf_arc_jump_window);
                sq.gf_arc_jump_window = 20;
            }
            if (sq.gf_arc_jump_min_count < 1 || sq.gf_arc_jump_min_count > sq.gf_arc_jump_window) {
                const int clamped = std::clamp(sq.gf_arc_jump_min_count, 1, sq.gf_arc_jump_window);
                log->warn("Network (GF arc jump, card ARL): network.cycle_slip.gf_arc_jump_min_count "
                          "= {} lies outside [1, window = {}]; {} is used (a count the window "
                          "cannot hold would leave the scale term off for the whole run).",
                          sq.gf_arc_jump_min_count, sq.gf_arc_jump_window, clamped);
                sq.gf_arc_jump_min_count = clamped;
            }
            // IF-TD post-flag re-seed (card ARL): keep a flagged satellite's gradient so
            // the epoch after the flag has a double difference.  Changes the solve only
            // through what that evaluation flags; default off.  network_cycle_slip_if_phase_td_
            // is parsed above in this helper, so the interlock can sit here.
            sq.if_phase_td_reseed_on_flag =
                jval<bool>(json, "network.cycle_slip.if_phase_td_reseed_on_flag",
                           "network_cycle_slip_if_phase_td_reseed_on_flag", false);
            if (sq.if_phase_td_reseed_on_flag) {
                if (!network_cycle_slip_if_phase_td_)
                    log->warn("Network (IF-TD re-seed, card ARL): "
                              "network.cycle_slip.if_phase_td_reseed_on_flag is armed without "
                              "network.cycle_slip.if_phase_td.  No IF-TD detector is built, so "
                              "nothing is flagged or re-seeded: the knob is INERT.");
                else
                    log->info("Network (IF-TD re-seed, card ARL): post-flag re-seed ON.  A pooled "
                              "IF-TD flag keeps the satellite's flag-epoch gradient as the next "
                              "epoch's origin instead of restarting its chain, so the epoch after "
                              "a flag is evaluated and a slip repeated there is seen.  "
                              "SOLVE-AFFECTING through those evaluations; not fingerprinted.");
            }
            // Sim-only oracle slip response (include/slip/NetworkSlipOracle.hpp): shifts
            // the carried level by the true integer jump instead of freeing the arc, to
            // measure the ceiling of a level-preserving re-level.  Changes the solve;
            // default off ⇒ byte-identical.  The truth-root existence check below keeps
            // it inert on field data by construction.
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
            // The other half of the 2x2: what to do when truth does put a slip in the
            // window.  "repair" (the default) is the ceiling arm; "free" declines, so the
            // caller runs the ordinary response, which paired with false_alarm "keep"
            // isolates the false-alarm cost.
            sq.oracle_true_slip =
                jval<std::string>(json, "network.cycle_slip.oracle_true_slip",
                                  "network_cycle_slip_oracle_true_slip",
                                  std::string("repair"));
            // Detector-latency window (seconds).  The truth-slip window's lower bound is
            // max(last_applied_s, carry.birth_s, now_s - this), so this window is what
            // makes a detector that fires 1-3 epochs late repairable instead of
            // mis-counted as a false alarm: a zero-q carried level is not re-set by the
            // intervening solves, so the jump is still unabsorbed even though
            // LocalCarry::last_seen_s has advanced past it.  Clamped rather than
            // refused: 0 (same-epoch only) is a legitimate pessimistic rung of the
            // ladder, though a degenerate one (the lower bound is exclusive, so window 0
            // admits nothing and the run-end line prints its armed-and-inert warning),
            // and a negative value is a typo whose only sensible reading is that rung.
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
                // Hard errors, not warn-and-disarm: an oracle that silently degrades
                // to "no repair" would report a ceiling of zero and be read as a
                // falsification of the mechanism.  That is the armed-and-inert
                // failure of invariant #4, except that here the inert arm produces a
                // wrong scientific verdict, so it must not be survivable.
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

            // Arming diagnostics: a card whose host arm is off is inert, and a silent
            // inert card is the "clean log, dead wiring" failure invariant #4 exists to
            // prevent.
            if (sq.ref_gate && !network_cycle_slip_if_phase_td_) {
                log->warn("Network (slip-QC card R): network.cycle_slip.if_phase_td_ref_gate "
                          "is set but .if_phase_td is OFF — no IF-phase detector is built, so "
                          "the hardened reference gate is INERT this run.");
                sq.ref_gate = false;
            }
            // Items 1 and 2 of §6.14.6 are adjudication refinements inside the hardened
            // gate: evaluateRefGate is their only reader, and it is reached only when
            // ref_gate is armed.  Left set with the host arm off, they would read as
            // posture in a config that cannot act on them.
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
            // Reference arm on the additional chains (REF-FAN, §22.6).  Two hard
            // prerequisites, both structural:
            //  * `if_phase_td_pairs = all`: with one chain per satellite there are no
            //    additional chains, so the knob would be a dead flag (invariant #4:
            //    never leave inert posture in a config that reads as armed);
            //  * `if_phase_td_ref_gate`: without the empirical gate the legacy
            //    candidate condition |median| > 0.5·λ_IF is the decision, and on a wide
            //    combination noise alone meets that condition: (L2W,L5Q) has IF
            //    coefficients 12.26 / −11.26 (noise amplification 16.6) against
            //    λ_IF = 0.1248 m, measured at 5 351 candidates / 64 fires over a quiet
            //    70-epoch 60-station null (§14.10.3).  The one-arm rule exists to
            //    prevent that flood, so the arm is re-opened only behind the per-chain
            //    empirical scale, which tracks each chain's own dispersion.  The
            //    detector refuses the combination a second time.
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
            // Auto-arm for the prior-residual census (INC-3;
            // docs/SPEC_prior_residual_census.md §4).  loadNetworkCarries_ resolved
            // prior_residual_corroborate one helper earlier; the flag it needs is parsed
            // here, so per this section's load-order rule the forcing lives at this end
            // of the chain.  gap_instrument makes networkQualifyRawObservables run and
            // fill the per-(station, satellite) raw MW/GF verdicts the join reads;
            // without it the whole contingency table collapses to the 'no_evidence'
            // column and the census's headline b-cell cannot be measured.  Not
            // mw_lli_corroborate: that is a decision card and would perturb the
            // population being measured (the same reason the D5 v3 interlock picks
            // gap_instrument).
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
        // Carrying carrier-phase ambiguities as zero-process-noise random constants is sound
        // only if an unscreened cycle slip cannot freeze a wrong value into the shared clock:
        // a slip makes the post-slip phase self-consistent with the slipped integer, so the
        // stiff carry does not resist it; it locks it in.  Carrying phase therefore requires
        // at least one pre-fit slip detector (mw_detect, if_phase_td, or the turbo_edit
        // repair, which also runs the MW detector).  If a config enables phase leveling with
        // none, the loader arms network.cycle_slip.mw_detect, the robust default: the
        // wide-lane + LLI detector needs no a-priori position (it works even when a station
        // sits far from its reference), whereas IF-phase TD relies on accurate coordinates
        // (fine in-network, where they are fixed near truth, but not in general).  Both are
        // ionosphere-free and valid at the 30 s product rate (unlike the MW+GF integer
        // repair).  Mirrors the raim_within_epoch ⟹ raim + attribution auto-arm in
        // loadNetworkQcTiers_.  IF-phase TD stays an explicit opt-in for the complementary
        // geometric-jump coverage where coordinates are trusted.
        if (network_phase_leveling_ && !network_cycle_slip_mw_detect_
            && !network_cycle_slip_if_phase_td_ && !network_cycle_slip_turbo_edit_) {
            log->warn("Network: network.phase_leveling carries carrier-phase ambiguities but no "
                      "pre-fit slip detector is enabled — an un-screened slip would freeze into "
                      "the shared clock.  Enabling network.cycle_slip.mw_detect (wide-lane + LLI, "
                      "needs no a-priori position).  Add network.cycle_slip.if_phase_td for "
                      "complementary geometric-jump coverage where coordinates are trusted.");
            network_cycle_slip_mw_detect_ = true;
        }

        // Datum-station slip detection and network re-reference
        // (docs/SPEC_synthetic_minimum_network.md §9.7.10).  Under the legacy
        // single-pivot datum every slip loop starts at i_lo = 1, so the datum station,
        // the one receiver whose ambiguity is inside every emitted phase bias, has no
        // slip exposure (§9.7.4, proven by roster reorder).  This arms MW/LLI detection
        // at stations[0] with the mechanism-specific response: not a local refloat (the
        // pivot's ambiguities are gauge-pinned) but a per-satellite network
        // re-reference, which hard-skip-stashes the satellite's PhaseBias carry,
        // reseeds every station's ambiguity at the epoch the bias re-seeds, and marks a
        // per-satellite discontinuity at the next emit.  Parsed after the invariant-#1
        // auto-arm above, so an mw_detect armed through leveling satisfies the
        // requirement.  Default off = byte-identical.
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

        // The pair scope of card L5P is a scope on the MW drain, so without that drain
        // it is inert.  It warns rather than refuses (the B5D interlock precedent): the
        // knob changes nothing on its own, and accepting it silently would let an arm
        // read as armed while the detector it scopes never ran.  Checked after the
        // invariant-#1 auto-arm above, so a leveling run that auto-armed mw_detect
        // never trips it.
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
     * @brief Post-fit / within-epoch QC tiers: RAIM and the clock monitor.
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
        // section, and never again, so this is the value the unsplit code saw.
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
        // Isolated-arc refloat maturity gate (T2d; default off = byte-identical).
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
        // off = every station contributes (byte-identical).
        network_phase_bias_exclude_unmatched_antenna_ =
            jval<bool>(json, "network.phase_bias_exclude_unmatched_antenna",
                       "network_phase_bias_exclude_unmatched_antenna", false);
        // Policy backstop (invariant #6: default off): hard-drops any station whose
        // receiver antenna still has no ATX match after the always-on radome→NONE
        // fallback (an unknown antenna type means unmodelled receiver PCO/PCV, a
        // systematic error in every observation feeding the shared clocks and biases).
        network_exclude_unmodeled_antenna_stations_ =
            jval<bool>(json, "network.exclude_unmodeled_antenna_stations",
                       "network_exclude_unmodeled_antenna_stations", false);
        // The roster's configured coordinates are monument coordinates, and the
        // observations refer to the antenna (card UPO).  Armed, each station's
        // position_ecef is reduced to its antenna reference point at setup through
        // Coordinates::markerToArp, the function the held-out consumer's truth also
        // uses.  Default off = byte-identical; the eccentricities are read and reported
        // in both states, so an off run states the size of the defect.
        network_station_marker_to_arp_ =
            jval<bool>(json, "network.station_marker_to_arp",
                       "network_station_marker_to_arp", false);
        // The arc ledger's trace switch (card SID phase 1;
        // docs/refound/SID_signal_identity_2026-09-22.md section 5).  Since card NOM
        // step N3a the per-station signal roster, the arc ledger and their census are
        // built on every network run, and this key only decides whether the ledger's
        // trace is written (logs/network_arc_ledger.csv and the ARC event rows of the
        // build and the audit).  The key keeps its name, so a config that arms it loads
        // and gets the trace it asked for.  Default off; trace class.
        network_signal_roster_ =
            jval<bool>(json, "network.signal_roster", "network_signal_roster", false);
        // The arc-level slip tracker becomes the roster's first reader
        // (card SID phase 2, section 6 of the same card).  Default off; the supersession
        // notice against card GRK's knob is raised in validateNetworkPhaseDatum_, late in
        // the section.  The roster it reads is built on every run (card NOM step N3a).
        network_roster_tracker_ =
            jval<bool>(json, "network.roster_tracker", "network_roster_tracker", false);
        // The gap re-entry response keys on the ledger's admitted lifecycle instead of
        // on the raw tiers' resumptions (card SID phase 3, section 7 of the same card).
        // Default off; armed, it changes the solve (it changes which carried levels
        // survive a break), so it is graded as a behavior change.  The interlock
        // against a missing observatory or a missing roster read is raised in
        // validateNetworkPhaseDatum_.
        network_ledger_reentry_ =
            jval<bool>(json, "network.ledger_reentry", "network_ledger_reentry", false);
        {
            // The policy for an arc no raw tier saw this epoch.  Parsed as a string,
            // so a typo falls back to the default with a warning rather than reading
            // as a silent integer; the same shape network.trace_dd_fractional_ref uses.
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
        // Reason-mask writers at every screen that removes a modelled row, a satellite
        // or a station, and the anchor election read against the ledger
        // (card SID phase 4a, section 9 of the same card).  Default off; on is an
        // observatory and byte-identical on the solve, which is the bar of 4a.  The
        // interlock against a missing observatory is raised in validateNetworkPhaseDatum_.
        network_ledger_screens_ =
            jval<bool>(json, "network.ledger_screens", "network_ledger_screens", false);
        // The election treats a return as a return (card SID phase 4b).  Default off;
        // armed, it changes the solve (the election's memory across a gap changes what
        // the carried anchor means), so it is graded as a behavior change and is
        // fingerprinted.  The interlock against a missing 4a is in
        // validateNetworkPhaseDatum_.
        network_ledger_anchor_ =
            jval<bool>(json, "network.ledger_anchor", "network_ledger_anchor", false);
        // The emitter's per-family bookkeeping read against the ledger, and the
        // producer's declaration of its signal roster on the wire (card SID phase 5a).
        // Default off; on is an observatory: the solve and every C / B / P / E record
        // are byte-identical, and the only difference in products.txt is the N
        // declaration block.  The interlock against a missing observatory is raised in
        // validateNetworkPhaseDatum_.
        network_ledger_emit_ =
            jval<bool>(json, "network.ledger_emit", "network_ledger_emit", false);
        network_raim_attribution_ =
            jval<bool>(json, "network.raim_attribution", "network_raim_attribution", false);
        // RAIM pool re-host (docs/SPEC_raim_pool_rehost.md).  Parsed here with the rest
        // of the RAIM family; the inert guards come later, because they read
        // network_carry_invariant_, which is populated in loadNetworkDatumSignalling_.
        network_raim_carry_invariant_ =
            jval<bool>(json, "network.raim_carry_invariant",
                       "network_raim_carry_invariant", false);
        network_raim_carry_invariant_refloat_ =
            jval<bool>(json, "network.raim_carry_invariant_refloat",
                       "network_raim_carry_invariant_refloat", false);
        // The carried-phase RAIM pools restricted to observation phase rows (card RPA).
        // Parsed with the RAIM family; its interlock (network.raim) is checked after the
        // RAIM auto-couplings and the service profile, in validateNetworkCarryPosture_.
        // A QC knob: not in the warm-resume fingerprint, as for the other raim_* knobs.
        network_raim_pool_data_rows_only_ =
            jval<bool>(json, "network.raim_pool_data_rows_only",
                       "network_raim_pool_data_rows_only", false);
        network_raim_within_epoch_ =
            jval<bool>(json, "network.raim_within_epoch", "network_raim_within_epoch", false);
        network_raim_sigma0_trigger_ =
            jval<double>(json, "network.raim_sigma0_trigger", "network_raim_sigma0_trigger", 3.0);
        network_raim_withhold_sigma0_ =
            jval<double>(json, "network.raim_withhold_sigma0", "network_raim_withhold_sigma0", 0.0);
        // A σ̂0 withhold ceiling in (0,1] is pathological (R11/R1): a clean epoch's √(reduced
        // χ²), the σ̂0 the gate compares against, sits at ~1, so a ceiling at or below 1 would
        // withhold nearly every warm armed epoch (total loss of availability), the opposite
        // of the intended rare escape hatch.  A sensible value is ~2× network.raim_sigma0_trigger.
        // It is treated as a config error: warn and disable (0 = emit anyway, the documented
        // default) rather than silently withholding the whole run.  Done before the
        // within-epoch coupling below, so a ceiling reset to 0 does not spuriously arm
        // within-epoch adaptation.
        if (network_raim_withhold_sigma0_ != 0.0 && network_raim_withhold_sigma0_ <= 1.0) {
            log->warn("Network: network.raim_withhold_sigma0 {} is <= 1 — σ̂0 is floored at 1, so "
                      "a ceiling at or below 1 would withhold most warm epochs (and a negative "
                      "value is meaningless).  Disabling it (set ~2x network.raim_sigma0_trigger, "
                      "e.g. 6, to use it).",
                      network_raim_withhold_sigma0_);
            network_raim_withhold_sigma0_ = 0.0;
        }
        // Withhold-era release policy (SPEC_withhold_era_release §3): two independent
        // default-off responders on the A4 path.  Parsed here beside their gate; the
        // cross-knob interlocks (withhold ceiling present, relevel_free_epoch for the
        // erase half) live at the end of loadNetworkSection_, per the load-order rule:
        // the service-mode arming block runs after this site, and relevel_free_epoch
        // can still be disarmed by the carry_invariant interlock at the end of the
        // section.
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
        // Per-constellation carve-out of the A4 σ̂0 withhold (RTA-10).  It acts only at a
        // tripped withhold (raim_withhold_sigma0 > 0), so without that ceiling it is a silent
        // no-op; the loader warns rather than auto-arms, because choosing a σ̂0 ceiling is a
        // numeric decision with no safe default to inject.
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
        // Suppressed under service_mode (P4): the bundle arms raim_withhold_sigma0 (2x
        // trigger) in the arming block below, so the "silent no-op" claim would be
        // false by the end of the load.
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
        // Two measure-only knobs, R2 and R4 (queue item 4; SPEC_t3_null_derivation §6),
        // both default off.  Parsed here beside their statistic; the raim and
        // trace_flag_rates dependencies are interlocked at the end of
        // loadNetworkSection_, once the service-mode arming block has run.
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
        // Level-authority census (LV-6; SPEC_level_authority_internal §3): measure-only,
        // with no interlocks beyond the reject bands; it arms the CS-GF advance itself
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
        // Pierce-point dump for LV-6 G2 (SPEC_level_authority_internal §4): the census's
        // own per-LOS pierce points, so a reference field (GIM in the field, truth in
        // sim) can be pushed through the identical sampling rather than a re-derived
        // one.  Trace-only, in its own file; inert without the census.
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
        // The withhold ceiling reuses the within-epoch σ̂0 (A4/P0-4), which is published only
        // when the within-epoch adaptation runs; otherwise sigma0_adapt stays 0 and the gate
        // never fires.  So raim_withhold_sigma0 is a silent no-op without
        // network.raim_within_epoch.  The loader enables that tier (which then pulls in raim
        // and attribution just below) so the diffuse-contamination escape hatch is active,
        // and logs it.
        if (network_raim_withhold_sigma0_ > 0.0 && !network_raim_within_epoch_) {
            log->warn("Network: network.raim_withhold_sigma0 > 0 needs the within-epoch σ̂0 — "
                      "enabling network.raim_within_epoch so the withhold escape-hatch is active "
                      "(it is the tier that computes the σ̂0 the ceiling compares against).");
            network_raim_within_epoch_ = true;
        }
        // The within-epoch adaptation refloats (reinitializes) a contaminated sat/arc's carried
        // ambiguity in place: the re-solve lets it find its new value and the harvest re-stashes
        // it, so within-epoch is self-contained (no lock-in even at zero process noise).  The
        // post-fit RAIM prominence-attribution refloat is the complementary tier: it catches
        // slips too small to trip the within-epoch σ̂0 trigger yet large enough to be flagged by
        // the standardized residual (e.g. a single-satellite spike that does not move the
        // robust network σ̂0).  The two are halves of one backstop, so both are enabled together.
        if (network_raim_within_epoch_ && (!network_raim_ || !network_raim_attribution_)) {
            log->warn("Network: enabling network.raim + network.raim_attribution alongside "
                      "network.raim_within_epoch — the post-fit attribution refloat is the "
                      "complementary tier that catches slips below the within-epoch σ̂0 trigger.");
            network_raim_ = true;
            network_raim_attribution_ = true;
        }
        // Extends the within-epoch screening to the code and iono-constraint row pools
        // (WE-CI).  Without it those rows have no screening statistic in any tier (the
        // P1-4 pass excludes SH-touching rows; the within-epoch pool is carried-ambiguity
        // phase rows only), and the DOY-129 19:26:00 SH failure entered through them.
        // Follows the A4 precedent: arming the extension pulls in the tier it extends.
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

        // Gross line-of-sight screen (card GL, docs/SPEC_event_harness.md §24.18).
        // Parsed here, next to the post-fit tiers whose blind spot it closes: each of
        // them pools carried-ambiguity phase rows, so a code-only gross error on one
        // line of sight is invisible to all of them (the `|A_r(row, amb_col)| > 0.5`
        // admission test in NetworkRaim.cpp), and the one tier that does see the row,
        // MDB, publishes a reliability number and the literal action "observe"
        // (NetworkMdb.cpp:286).  The only solve-affecting knob in this neighbourhood;
        // default off, byte-identical when off, and not fingerprinted (it changes no
        // carried state's meaning, only which rows are trusted this epoch, so a
        // snapshot written with it off stays readable with it on, as for trace knobs).
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
            // --- The four sub-rules that complete the screen (RF-3b;
            // docs/refound/RF-3_code_row_qc_2026-09-22.md §4).  All default to the
            // behavior from before the sub-rules, so an existing arm's bytes are
            // unchanged.
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
            // The cold-start absolute bound (card ICF response 1).  A negative or
            // non-finite value cannot express a bound in metres, so it is clamped to 0
            // (off) loudly rather than read as "declare everything".
            gnc.gross_los_cold_bound_m =
                jval<double>(json, "network.gross_los_cold_bound_m",
                             "gross_los_cold_bound_m", 0.0);
            if (!(std::isfinite(gnc.gross_los_cold_bound_m)
                  && gnc.gross_los_cold_bound_m >= 0.0)) {
                log->warn("Network (gross-LOS, card ICF): network.gross_los_cold_bound_m = {} "
                          "is not a non-negative number of metres; clamped to 0 (off).",
                          gnc.gross_los_cold_bound_m);
                gnc.gross_los_cold_bound_m = 0.0;
            }
            if (gnc.gross_los_cold_bound_m > 0.0 && !gnc.gross_los_screen)
                log->warn("Network (gross-LOS, card ICF): network.gross_los_cold_bound_m = {:g} m "
                          "while network.gross_los_screen is OFF; the seam is never entered, so "
                          "the bound is INERT.  Read this as a miswired arm, not as a null "
                          "result.", gnc.gross_los_cold_bound_m);
            // Test-only zero-ionosphere rows (card ARL).  Clamped to 0 when not a
            // non-negative number of metres; loud when armed, since it is wrong on real data.
            gnc.sim_zero_iono_sigma_m =
                jval<double>(json, "network.sim_zero_iono_sigma_m", "sim_zero_iono_sigma_m", 0.0);
            if (!(std::isfinite(gnc.sim_zero_iono_sigma_m) && gnc.sim_zero_iono_sigma_m >= 0.0))
                gnc.sim_zero_iono_sigma_m = 0.0;
            if (gnc.sim_zero_iono_sigma_m > 0.0)
                log->warn("Network (card ARL): network.sim_zero_iono_sigma_m = {:g} m: every slant "
                          "ionosphere is tied to absolute zero.  TEST-ONLY, for a fixture simulated "
                          "without ionosphere; wrong on real data.", gnc.sim_zero_iono_sigma_m);
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
                // Clamp, with a warning, the knobs that mean nothing outside their
                // range.  A screen configured with k ≤ 0 or a deweight ≤ 1 is not a
                // gentler screen: it declares everything or responds with nothing, and
                // either reads as a clean negative result in the run-end counters
                // (invariant #4).
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

                // --- Banners for the RF-3b sub-rules: one line per armed sub-rule, each
                // naming the population it reaches and the measurement behind it, so a
                // run's stdout states which of the four parts of card RF-3b were in force.
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
                // --- The cold-start bound (card ICF response 1) ---------------------
                if (gnc.gross_los_cold_bound_m > 0.0) {
                    if (gnc.gross_los_bound)
                        log->warn("Network (gross-LOS, card ICF): network.gross_los_cold_bound_m "
                                  "= {:g} m is INERT under network.gross_los_bound: the cold "
                                  "response erases the faulted carried ambiguity every epoch "
                                  "the bound is exceeded, and the hold would keep it.",
                                  gnc.gross_los_cold_bound_m);
                    else if (gnc.gross_los_warmup_epochs == 0)
                        log->warn("Network (gross-LOS, card ICF): network.gross_los_cold_bound_m "
                                  "= {:g} m with network.gross_los_warmup_epochs = 0: there is no "
                                  "maturity hold for the bound to act in, so it never declares.",
                                  gnc.gross_los_cold_bound_m);
                    else
                        log->info("Network producer (gross-LOS, card ICF cold-start bound): "
                                  "inside the first {} emitted epochs a line of sight whose "
                                  "largest POST-FIT code |v| exceeds {:g} m is declared AT ONCE "
                                  "(one epoch, no persistence) with the screen's response: its "
                                  "carried ambiguities are erased and marked pending-reseed and "
                                  "its code rows fold at sigma x {:g}.  While it stands, every "
                                  "epoch over the bound re-erases the carried ambiguity (a "
                                  "re-stashed ambiguity would make the faulty line of sight "
                                  "redundant again); the release erases it once more, so the "
                                  "line of sight reconnects on a fresh ambiguity.  No cold "
                                  "declaration at a station with more than {:.0f} % of its lines "
                                  "of sight over the bound.",
                                  gnc.gross_los_warmup_epochs, gnc.gross_los_cold_bound_m,
                                  gnc.gross_los_deweight, 100.0 * gnc.gross_los_max_station_frac);
                }
            }
        }

        // Baarda reliability numbers / MDB (docs/SPEC_redundancy_census.md).  Parsed
        // here, next to the B2 redundancy knob it rides, and after
        // loadNetworkObservabilityAndSnapshot_ has resolved the redundancy-census
        // arming, so the precondition below reads final values of both r_i sources.
        // Measure-only and not fingerprinted: same class as the census.
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
                // A hard error, not a silent disarm.  Without a redundancy source there
                // is no r_i to divide by, so MDB would run, bin zero rows, and print a
                // clean summary indistinguishable from "this network cross-checks
                // everything" (the live-counter-over-dead-wiring failure).
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
                // Invariant-#4 tripwire at load time: without the P1-4 post-fit pass the
                // scaled variant can never populate, and a run that quotes formal MDB
                // alone repeats the D5 v1 mistake.  A warning rather than an auto-arm:
                // trace_postfit_sigma0 costs a full residual pass per epoch, and buying
                // that silently is not this knob's decision.
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

        // Block-Baarda census (BLOCKFORMAL): the (station, satellite) LOS block outlier
        // census (docs/SPEC_redundancy_census.md).  Parsed here, next to the MDB
        // precondition and for the same load-order reason: it rides the redundancy
        // census's swept stations, and loadNetworkObservabilityAndSnapshot_ has already
        // resolved that census's arming, so the precondition below reads a final value.
        // Measure-only and not fingerprinted: same class as the census.
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

            // Invariant #4 tripwire: a corroboration arm without the census produces
            // nothing.  A hard error rather than a silent disarm, as in the
            // prior-residual census.
            if (bnc.block_baarda_corroborate && !bnc.block_baarda)
                throw std::runtime_error(
                    "network.block_baarda_corroborate requires network.block_baarda = true "
                    "(the corroboration join runs inside the block sweep; arming it alone "
                    "produces no blocks, no contingency table and no warning at run time).");

            if (bnc.block_baarda) {
                // A hard error, not a silent disarm.  SPEC §6: the block sweep has no
                // scheduler of its own by design; it hangs off the redundancy census's
                // swept branch, so the stride, the targeted escalation and the
                // trip-forced full sweep are inherited exactly.  With no host the sweep
                // would run, enumerate zero blocks, and print a clean summary
                // indistinguishable from "this network has no smeared faults" (the
                // live-counter-over-dead-wiring failure).
                const bool have_host = bnc.redundancy_census_stride > 0
                                    || bnc.redundancy_escalate_sigma > 0.0;
                if (!have_host)
                    throw std::runtime_error(
                        "network.block_baarda requires the redundancy census as its sweep host "
                        "(network.redundancy_census_stride > 0 or network.redundancy_escalate_sigma "
                        "> 0).  The block sweep deliberately adds no scheduler: it rides that "
                        "card's swept stations, so with the census off it would enumerate zero "
                        "blocks and report a clean run indistinguishable from a genuine negative.");

                // Invariant-#4 tripwire: a constellation letter with no hook.  The census
                // would report it as "armed, zero blocks" at run end, which reads like a
                // real negative, so it is named here, where the typo is still cheap to
                // fix.
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

                // Auto-arm of network.cycle_slip.gap_instrument (INC-2).  The gap_instrument
                // jval has already run (in loadNetworkSlipDetectors_), so this forcing is
                // safe inline.  The prior-residual census's auto-arm instead sits at the
                // tail of loadNetworkSlipDetectors_, because its own knobs are parsed one
                // helper earlier and the later jval would overwrite a forcing made there.
                // gap_instrument makes networkQualifyRawObservables run and fill the
                // per-(station, satellite) raw MW/GF verdicts the join reads; without it
                // every contingency cell lands in 'no_evidence' and the b-cell of SPEC
                // §8.3 cannot be measured.  Not mw_lli_corroborate: that is a decision
                // card and would perturb the population being measured (the same choice
                // the prior-residual loader and D5 v3 make).
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
                // Invariant-#4 sink dependency: the ladder survives without trace_events,
                // but every block identity is emitted through emitEvent, which is a hard
                // no-op when that sink is closed.  A warning rather than an auto-arm: a
                // trace sink is the user's decision, and the counters still make the
                // finding checkable.
                if (!bnc.trace_events)
                    log->warn("Network (block-Baarda census): network.trace_events is off, so "
                              "no block IDENTITY will be written — the ladders and the run-end "
                              "counters still populate, but the worst-block names live only in "
                              "the log line.  Arm trace_events if any loud block is going to be "
                              "chased back to a satellite.");

                // Convergence caveat, stated at load rather than after the run: on a
                // cold start a free ambiguity absorbs its own arc's phase misfit
                // exactly, so those post-fit residuals are identically zero and their
                // rows read r ≈ 0 (43 % of data rows on the FT63 cold smoke).  A block
                // dominated by them comes back uncheckable by construction, not because
                // of data quality.
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

        // Station congruence: coordinate-fault detection in position space
        // (docs/SPEC_redundancy_census.md).  Parsed here, right after the block census
        // and for the same load-order reason: it rides the same redundancy-census
        // sweep, whose arming loadNetworkObservabilityAndSnapshot_ has already
        // resolved, so the precondition below reads a final value.  Measure-only and
        // not fingerprinted: same class as the two censuses above.
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
                // A hard error, not a silent disarm.  SPEC §3: the screen has no
                // scheduler of its own by design; it hangs off the redundancy census's
                // swept branch, so the stride, the targeted escalation and the
                // trip-forced full sweep are inherited exactly.  With no host it would
                // run, sweep zero stations, and print a clean summary indistinguishable
                // from "every held coordinate on this network is right": the
                // live-counter-over-dead-wiring failure, in the one screen whose purpose
                // is to disprove a silent negative.
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

                // Invariant-#4 sink dependency, same shape as the block census's: the
                // per-station trace row is the headline product and survives without
                // trace_events, but the loud-station identities are emitted through
                // emitEvent, a hard no-op when that sink is closed.
                if (!cnc.trace_events)
                    log->warn("Network (station congruence): network.trace_events is off, so "
                              "no loud-station IDENTITY will be written — network_congruence.csv "
                              "and the run-end MDB table still populate, but the alarm stream "
                              "does not exist.  Arm trace_events if a flagged station is going "
                              "to be chased at the epoch it was flagged.");

                // Regime caveat, stated at load rather than after the run.  Under a
                // free-ambiguity (carry_invariant) lineage every phase row fits its own
                // arc exactly, so those rows' residual cofactor is ~0 and they contribute
                // almost nothing to M: the screen is code-grade and its MDB is dm-class.
                // Stating it here keeps a dm-class MDB table from being read at run end
                // as a defect in the screen.
                //
                // The line names both regimes rather than resolving which one is
                // active: network_carry_invariant_ is parsed in
                // loadNetworkDatumSignalling_, which runs after this helper, so a
                // conditional here would read the struct default and print the wrong
                // regime.  The MDB table at run end is measured, not predicted, and
                // settles it.
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

        // Constraint-aware global σ̂₀ (docs/SPEC_prior_residual_census.md §14.4).  The
        // knob itself is parsed in loadNetworkCarries_, next to the census it rides;
        // this is the data-side precondition, and it lives here because network_raim_
        // and network_raim_redundancy_normalize_ are resolved earlier in this helper
        // (the section's load-order rule, the same reason the INC-3 gap_instrument
        // auto-arm sits at the tail of loadNetworkSlipDetectors_ rather than next to
        // its own knob).
        //
        // A warning, not a hard error and not an auto-arm: the r ≡ 1 fallback is a
        // specified degraded path (§2), it is flagged per epoch in-band, and
        // raim_redundancy_normalize changes the solve, so silently buying a different
        // refloat population to improve a measure-only column is not this knob's
        // decision.
        if (network_config_.prior_residual_global_sigma0) {
            // The driver brackets networkPostFitRaim with `if (raim_on && carry_warm)`,
            // and carry_warm = phase_leveling && … (the carry_warm definition in
            // ApplicationNetwork.cpp).  So on a lineage without phase_leveling the
            // post-fit pass never runs, network.raim reads armed in every log line, and
            // the data side of the global form is structurally dead: 0 rows over the
            // whole run, as measured on the first FT63 smoke.  The in-band NO_DATA flag
            // catches it at run time, one run too late: the fix is a different lineage,
            // and at load it is still cheap.
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
        // physically impossible value, which the σ̂0 RAIM cannot see because it needs the
        // shared residual level to move, and refloats its carried clock (and the coupled
        // phase/code bias) so the stiff sat_clock_q carry cannot freeze the wrong value.
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
        // fingerprint (NetworkSnapshotFingerprint.cpp), so an unarmed run's fingerprint is
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
    }

    // =================================================================
    //  network subsection helper: loadNetworkClockModelAndChannels_
    // =================================================================
    /**
     * @brief Two-state clock model, C1 emission, and the B1/B1b/M4 quality channels.
     *
     * @c network.clock_model*, @c emit_generation_time, the RT0-5 runaway
     * quarantine, the RTA-10 epoch-status heartbeat, and URA / phase-bias-sigma /
     * combined-sigma.  The R5 @c ura_staleness_q2 capture sits after the clock-model
     * validity restore on purpose, so it inherits the restored q2.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkClockModelAndChannels_(const nlohmann::json& json,
                                                       const std::shared_ptr<spdlog::logger>& log) {
        // Alias preamble: the body below reads the driver local it was written
        // against.  network_service_mode_ is written once, at the top of the
        // section, and never again, so this is the value the unsplit code saw.
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
        // --- Product generation-time metadata (RT0-4) ---
        network_emit_generation_time_ =
            jval<bool>(json, "network.emit_generation_time", "network_emit_generation_time", false);
        if (network_emit_generation_time_)
            log->info("Network: network.emit_generation_time on — each emitted epoch carries a "
                      "wall-clock time-of-generation (G record) for consumer latency measurement. "
                      "Note: the product is NO LONGER byte-reproducible (wall-clock stamps differ "
                      "per run) — leave off for regression/byte-identity runs.");
        // --- Per-constellation clock-runaway quarantine (RT0-5) ---
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
            // Gated on !service_mode (P4): the bundle arms network.clock_model below,
            // so under service_mode the rate ceiling is not inert.
            if (!network_clock_model_ && !service_mode)
                log->info("Network (RT0-5): the rate ceiling ({:.2f} m/s) is inert without "
                          "network.clock_model (no drift is estimated); the magnitude ceiling "
                          "({:.0f} m) still applies.", network_clock_runaway_rate_m_s_,
                          network_clock_runaway_mag_m_);
            // Degenerate single-constellation run: the global clock median is that
            // constellation's own median, so the magnitude test measures only the
            // constellation's internal scatter about its own center, and a common-mode
            // runaway (every satellite walking off together, the BeiDou-style fault the
            // check targets) is invisible to it.  Only the rate ceiling (which needs
            // network.clock_model for a non-zero drift) can catch that.
            if (active_systems_.size() == 1)
                log->warn("Network (RT0-5): clock_runaway_check with a single active system ('{}') — "
                          "the magnitude test degenerates to the constellation's own-median scatter, "
                          "so a common-mode runaway of the whole constellation is invisible; only the "
                          "rate ceiling ({:.2f} m/s, requires network.clock_model) can catch it.",
                          active_systems_.front(), network_clock_runaway_rate_m_s_);
        }
        // --- Product-level epoch-status heartbeat (RTA-10) ---
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
            // Gated on !service_mode (P4): the bundle arms product_flush_per_epoch below.
            if (!network_config_.product_flush_per_epoch && !service_mode)
                log->info("Network (RTA-10): consider network.product_flush_per_epoch alongside "
                          "the heartbeat — a live tail only sees the H line when the stream "
                          "flushes.");
        }
        // --- Census-fed H σ̂₀ (H-rider, edge-queue §1.6 item 3a) ---
        // Parsed here (after emit_epoch_status_record); the dependency guard lives in
        // loadNetworkEmissionFilters_, because this helper returns void and the
        // empty-mask case is a hard load error, which only a bool helper can signal.
        network_h_sigma0_prefit_census_ =
            jval<bool>(json, "network.h_sigma0_prefit_census",
                       "network_h_sigma0_prefit_census", false);
        network_h_sigma0_classes_ =
            jval<std::string>(json, "network.h_sigma0_classes",
                              "network_h_sigma0_classes", "code,sh,other");
        // --- URA quality channel (B1) ---
        network_emit_ura_ =
            jval<bool>(json, "network.emit_ura", "network_emit_ura", false);
        network_ura_ewma_alpha_ =
            jval<double>(json, "network.ura_ewma_alpha", "network_ura_ewma_alpha", 0.1);
        network_consume_ura_ =
            jval<bool>(json, "network.consume_ura", "network_consume_ura", false);
        // --- Phase-bias σ channel (B1b; Tienstra product covariance, phase-bias block) ---
        network_emit_phase_bias_sigma_ =
            jval<bool>(json, "network.emit_phase_bias_sigma", "network_emit_phase_bias_sigma", false);
        network_consume_phase_bias_sigma_ =
            jval<bool>(json, "network.consume_phase_bias_sigma", "network_consume_phase_bias_sigma", false);
        // --- Combined-correction σ channel (M4, contract v2; Zhang 2021 / Psychas 2024b) ---
        network_emit_combined_sigma_ =
            jval<bool>(json, "network.emit_combined_sigma", "network_emit_combined_sigma", false);
        network_combined_sigma_agek_ =
            jval<double>(json, "network.combined_sigma_agek", "network_combined_sigma_agek", 0.0);
        // Consumer AR-eligibility veto on the product signal_integer flag (T3a; default off).
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
        // network_ura_staleness_q2_ (which defaults to network_clock_model_q2_) is read after
        // the clock_model validity restore below, so it inherits the restored q2, not a
        // rejected-then-reset value (R5).  See the read just past that block.
        network_ura_retro_bump_ =
            jval<bool>(json, "network.ura_retro_bump", "network_ura_retro_bump", false);
        network_mark_refsat_reset_disc_ =
            jval<bool>(json, "network.mark_refsat_reset_disc",
                       "network_mark_refsat_reset_disc", false);
        network_ura_refloat_inflate_ =
            jval<double>(json, "network.ura_refloat_inflate", "network_ura_refloat_inflate", 1.0);
        // service_mode is read at the top of the section (RTB-8), so every coherence
        // warning, including the ones above this point, can gate on it.
        if (network_emit_ura_)
            log->info("Network: network.emit_ura on — per-satellite URA quality channel "
                      "(EWMA α {:.2f}) written to the product; consumer R-inflation {}.",
                      network_ura_ewma_alpha_, network_consume_ura_ ? "ON" : "off");
        if (network_emit_ura_ && !network_raim_ && !service_mode)
            log->warn("Network: network.emit_ura is on but network.raim is off — the σ̂0 EWMA that "
                      "scales the URA is only updated by the post-fit RAIM pass, so the emitted URA "
                      "is the FORMAL clock σ only (no robust variance-factor scaling).  Enable "
                      "network.raim for σ̂0-scaled URA.");
        // The warm-up URA floor acts on the URA channel (RTB-7(b)): without emit_ura the
        // URA pass never runs and the floor is a silent no-op, so the loader warns.
        // Under service_mode (P4) the bundle arms emit_ura below, so the floor is not a
        // no-op, and the ramp info line is logged instead of the warning.
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
        // The ZOH clock-staleness q² is read after the validity restore above, so its default
        // inherits the restored network_clock_model_q2_ (R5): a rejected clock_model_q2 is
        // reset to 1.5e-7 there, and reading before would carry the bad value into the
        // default.  Defaulting to clock_model_q2 rather than 0 keeps the age inflation active
        // whenever network.consume_ura is on; an explicit 0 disables it (P1-3).
        // Consumer-side only.
        network_ura_staleness_q2_ =
            jval<double>(json, "network.ura_staleness_q2", "network_ura_staleness_q2",
                         network_clock_model_q2_);
        // A negative value (a typo, or inherited from a bad user-set clock_model_q2 when
        // clock_model is off and the validity restore above never ran) is inert at the
        // consumer (q2 > 0 guard), so it is reported instead of silently disabling the
        // inflation.
        if (network_ura_staleness_q2_ < 0.0) {
            log->warn("Network: network.ura_staleness_q2 {} < 0 → 0 (ZOH staleness inflation "
                      "off; check network.clock_model_q2, which supplies the default).",
                      network_ura_staleness_q2_);
            network_ura_staleness_q2_ = 0.0;
        }
        // Summary lines, logged only when on, for the B1 producer/consumer knobs that have
        // no other log line (emit_ura logs above; R3).  Informational only.
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
     * @brief Datum realization and discontinuity signalling knobs, and the card-3/card-4 datum family.
     *
     * Raw parse only for the clock gauge (pivot / sum-to-zero / broadcast-anchored), the
     * phase-bias gauge (prioritized / eq34) and @c network.carry_invariant*; the
     * mutexes and armed-state reports live in @c validateNetworkDatumCoherence_ and
     * @c validateNetworkPhaseDatum_.  The D5 v3/v4 knobs are parsed and host-checked here;
     * the slip-QC instrumentation that feeds them is auto-armed at the end of
     * @c loadNetworkSection_, once every input is final.
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
        // Per-λ discontinuity threshold as a fraction of a carrier cycle (H3; frac·λ
        // jump gate).  0 = legacy flat 0.05 m (byte-identical).
        network_disc_threshold_frac_cycles_ =
            jval<double>(json, "network.disc_threshold_frac_cycles",
                         "network_disc_threshold_frac_cycles", 0.0);
        // Sticky clock-pivot selection and per-constellation scoping of the producer's
        // internal handover response (carry skip / oscillator clear; RTA-1/RTA-2).
        network_sticky_pivot_ =
            jval<bool>(json, "network.sticky_pivot", "network_sticky_pivot", false);
        // Anchor-free clock gauge: fix each constellation's clock common-mode with a
        // sum-to-zero over its own satellite clocks (folded like the code-bias gauge)
        // instead of a hard pivot pin.  Removes the pivot/handover machinery entirely
        // for the clock; ignores network.clock_datum_satellite (no pivot to place).
        network_clock_datum_sum_to_zero_ =
            jval<bool>(json, "network.clock_datum_sum_to_zero",
                       "network_clock_datum_sum_to_zero", false);
        // Broadcast-anchored clock gauge (brdc_rts only): per-satellite soft anchors
        // to each satellite's own broadcast clock are the datum (folded pre-gate).  No
        // pivot, no handover seam, and, unlike sum-to-zero, whose raw-set reference
        // redistributes onto the survivors as satellites rise and set, no common-mode
        // wander: the absolute clock-product frame holds still against the broadcast
        // reference (the RT datum-stability contract).
        network_clock_datum_broadcast_anchored_ =
            jval<bool>(json, "network.clock_datum_broadcast_anchored",
                       "network_clock_datum_broadcast_anchored", false);
        // Time-consistent (δtˢ, dˢ) emission: holds the per-satellite clock↔code-bias
        // IF-pin reference signal pair fixed across epochs (latch on first sight,
        // re-latch only on genuine loss) instead of re-selecting the two lowest
        // distinct-frequency signals from each epoch's coverage.  Without it, a sparse
        // channel entering or leaving ≥2-station coverage re-anchors the per-satellite
        // clock by the satellite's inter-channel code bias, a differential-clock wander
        // that a receiver clock cannot absorb.  Only the W6 code-bias IF pin realizes
        // this split, so the toggle is inert without network.estimate_sat_code_bias.
        network_clock_signal_basis_hold_ =
            jval<bool>(json, "network.clock_signal_basis_hold",
                       "network_clock_signal_basis_hold", false);
        // Imposes the W6 code-bias datum (per-sat IF pin + per-(const, signal)
        // sum-to-zero) as a time datum: pins the change from the previous epoch's solved
        // values instead of pinning the level to zero (card 2).  Same row count; removes
        // the set-mean wander and relaxation after every rise/set (defect 6-7).
        // Validated later (both-armed error, inert warning), once the
        // estimate_sat_code_bias coupling has resolved.
        network_code_datum_time_linked_ =
            jval<bool>(json, "network.code_datum_time_linked",
                       "network_code_datum_time_linked", false);
        // Card GFC: two refinements of the time-linked datum, read only under it.  Pair
        // continuity holds the IF-pin pair while it stays registered and keeps the
        // satellite clock across a pair change that does happen; the family
        // difference realizes the inter-family code offset as Sum (d_X - d_P) instead of
        // the combined channel's sum-to-zero.  Validated with the time link (inert
        // warning, INFO when effective).
        network_code_datum_pair_continuity_ =
            jval<bool>(json, "network.code_datum_pair_continuity",
                       "network_code_datum_pair_continuity", false);
        network_code_datum_family_difference_ =
            jval<bool>(json, "network.code_datum_family_difference",
                       "network_code_datum_family_difference", false);
        // Card GFC step 2: the first-sighting family prior, a refinement of the family
        // difference (read only with it, under the time link).  0 is off.  Validated with
        // the two arms (validateNetworkDatumCoherence_: a negative or non-finite sigma is
        // refused, an inert arm warned).
        network_code_family_birth_prior_sigma_m_ =
            jval<double>(json, "network.code_family_birth_prior_sigma_m",
                         "network_code_family_birth_prior_sigma_m", 0.0);
        network_scoped_handover_response_ =
            jval<bool>(json, "network.scoped_handover_response",
                       "network_scoped_handover_response", false);
        network_handover_stransform_ =
            jval<bool>(json, "network.handover_stransform",
                       "network_handover_stransform", false);
        network_handover_stransform_before_rows_ =
            jval<bool>(json, "network.handover_stransform_before_rows",
                       "network_handover_stransform_before_rows", false);
        // Card SCL B2: the exact S P S^T at the handover, and card Q3's anchor kept across it.
        // Interlocks after the transform's own (below), once clock_temporal_anchor is read.
        network_handover_stransform_dense_ =
            jval<bool>(json, "network.handover_stransform_dense",
                       "network_handover_stransform_dense", false);
        network_clock_anchor_datum_keep_ =
            jval<bool>(json, "network.clock_anchor_datum_keep",
                       "network_clock_anchor_datum_keep", false);
        // Card PRM phase 3b step 2: the re-base mechanisms also move their owed sites.  Its
        // interlock (no re-base mechanism armed) reads the final values at the end of the
        // section, after every mechanism's own knobs and refusals have resolved.
        network_carry_rebase_complete_ =
            jval<bool>(json, "network.carry_rebase_complete",
                       "network_carry_rebase_complete", false);
        // Phase-bias gauge: single clock-pivot station (legacy) vs per-(sat,signal)
        // prioritized anchor receiver.  The two sub-knobs (coverage-preference
        // elevation, per-(sat,signal) stickiness) are inert unless the master toggle
        // is on.
        network_phase_datum_prioritized_ =
            jval<bool>(json, "network.phase_datum_prioritized",
                       "network_phase_datum_prioritized", false);
        // Eq-34 / de Jonge single-receiver-chain phase-bias datum (card 3).  Reuses the
        // prioritized SPOF-cure machinery (no pivot-first, every station carries its
        // arcs) but replaces the anchor election with a spanning-tree chain election
        // over per-(const, signal) components and per-(receiver, system, signal) RPB
        // split states.  Two sub-knobs (preferred datum receiver, re-pin rounding) are
        // inert unless the master toggle is on.
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
        // Frame-continuous re-host (seed-round INC-2; docs/SPEC_eq34_seed_round.md
        // §8.5).  Strictly a refinement of the seed-round target rule (it needs the
        // seed branch's state read to exist), so it is refused without it rather than
        // silently arming a branch that can never be reached.
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
        // Datum-station slip detection is a legacy-pivot-datum mechanism: every
        // no-pivot datum (prioritized / eq34) runs the slip loops from
        // i_lo = 0 already, so station 0 has ordinary coverage there and the
        // dedicated path never engages (runtime-guarded).  Parsed in
        // loadNetworkSlipDetectors_, which runs before this helper.
        if (network_cycle_slip_datum_station_detect_
            && (network_phase_datum_prioritized_ || network_phase_datum_eq34_)) {
            log->info("Network: network.cycle_slip.datum_station_detect is INERT under a "
                      "no-pivot phase datum (prioritized/eq34) — station 0 is an "
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
                // NOTE: the "…without carry_invariant" advisory cannot live here:
                // network_carry_invariant_ is parsed a few lines below this block, so it is
                // still false at this point and the warning would fire on every run,
                // including runs that arm carry_invariant.  It lives in the cross-knob
                // validation pass instead, where every flag is settled.
            }
        }
        // DD cycle-basis invariant carry (network.carry_invariant*; card 4, M1).
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
        network_carry_invariant_innov_gate_ =
            jval<bool>(json, "network.carry_invariant_innov_gate",
                       "network_carry_invariant_innov_gate", false);
        // Invariant #1 interlock (config level): a zero-q invariant carry with the
        // innovation gate off is a stiff time-linked quantity whose only event
        // responses are the pre-processor slip detectors, so a jump they miss is
        // carried at full weight indefinitely and distorts the solution silently (the
        // runaway arm of SPEC_synthetic_minimum_network §13.7 is this posture; see the
        // hygiene item in SPEC_o2_carry_invariant_accumulator).  It warns and does not
        // disarm: silently arming a solve-affecting gate would be worse, and qz > 0 also
        // satisfies this check.  Both flags are final here (network_carry_invariant_ is
        // written once and never cleared).
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
        network_carry_invariant_innov_calibrate_ =
            jval<bool>(json, "network.carry_invariant_innov_calibrate",
                       "network_carry_invariant_innov_calibrate", false);
        network_carry_invariant_innov_calibrate_window_ =
            jval<double>(json, "network.carry_invariant_innov_calibrate_window",
                         "network_carry_invariant_innov_calibrate_window", 60.0);
        // Raw-observable corroboration gate on the D5 re-tap (card 4, M1, D5 v3).
        network_carry_invariant_innov_corroborate_ =
            jval<bool>(json, "network.carry_invariant_innov_corroborate",
                       "network_carry_invariant_innov_corroborate", false);
        network_carry_invariant_innov_corroborate_backstop_k_ =
            jval<double>(json, "network.carry_invariant_innov_corroborate_backstop_k",
                         "network_carry_invariant_innov_corroborate_backstop_k", 0.0);
        if (network_carry_invariant_innov_corroborate_) {
            // Two ways this arms into silence, both invariant-#4 cases ("clean log, dead
            // wiring"), so neither passes quietly.
            if (!network_carry_invariant_innov_gate_) {
                log->warn("Network (Card 4 D5 v3): network.carry_invariant_innov_corroborate is "
                          "set but .carry_invariant_innov_gate is OFF — there are no D5 re-taps "
                          "to corroborate, so the gate is INERT this run.");
                network_carry_invariant_innov_corroborate_ = false;
            } else {
                // The suppression evidence is the slip-QC qualify pass, and the card that
                // makes that pass run is auto-armed from the settled-state interlock at the
                // end of loadNetworkSection_, not here.  One of cardsArmed()'s four inputs
                // (network_carry_iono_) still holds its raw parsed value at this point and
                // is cleared by an interlock that runs later, so a test here would read a
                // value that never reaches the estimator.  @see the "Evidence interlock
                // for the D5 v3/v4 consumers" block in loadNetworkSection_.
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
        // Only carried chords are gated; a born chord has no H0 (card 4, M1, D5 v5).
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
        // PD-safe DD fold (card 4, M1; docs/SPEC_pd_safe_dd_fold.md).  Parse only: the
        // "set without carry_invariant" advisory lives in the cross-knob validation pass
        // (@ref validateNetworkDatumCoherence_), where every flag is settled.
        // network_carry_invariant_ happens to be parsed above this block, but a check
        // that depends on that ordering is the trap the eq34 slip-aware note above
        // describes.
        network_carry_invariant_pd_safe_ =
            jval<bool>(json, "network.carry_invariant_pd_safe",
                       "network_carry_invariant_pd_safe", false);
        // Carry-aware covariance readback (docs/SPEC_carry_aware_covariance.md).  Same
        // parse-only discipline and for the same reason as the line above.
        network_carry_aware_covariance_ =
            jval<bool>(json, "network.carry_aware_covariance",
                       "network_carry_aware_covariance", false);
        // The flag re-taps in-epoch and stops there: the funnel entry does not exclude
        // the arc from next epoch's carry membership (card 4, M1, D5 v4).
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
                // qualify pass run is auto-armed from the settled-state interlock at the
                // end of loadNetworkSection_, where all four of cardsArmed()'s inputs are
                // final.  @see the "Evidence interlock for the D5 v3/v4 consumers" block.
                log->info("Network (Card 4 D5 v4): D5 re-tap-ONLY ON — a flagged chord whose arc "
                          "the raw observables MEASURED as continuous is reset in-epoch and its "
                          "arc is NOT excluded from next epoch's carry membership, so the gate "
                          "stops discarding the DD it just re-tapped.  A chord arc has cycle "
                          "support 1 under a stable tree, so on that population the exclusion "
                          "protected nothing.  FAIL-SAFE: stepped and unmeasurable arcs still "
                          "refloat, and so does any arc a raw detector flags in the current epoch "
                          "— the refloat is what COMPLETES a real slip's reset (measured 0.48 cyc, "
                          "GATE 10b), so invariant #1 is narrowed, never removed.  The funnel "
                          "INSERT is unchanged: the Iono-2 v3k erase classifier and every other "
                          "consumer still see the entry.  Watch the "
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
     * Ordered here because every guard needs both the phase-datum mode and
     * @c network_carry_invariant_ (the slip-funnel feed), which the helper above parsed.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkReattachHygiene_(const nlohmann::json& json,
                                                 const std::shared_ptr<spdlog::logger>& log) {
        // --- eq34 reattach hygiene (docs/SPEC_eq34_reattach_hygiene.md) --------
        // Placed here because every guard below needs both the phase-datum mode and
        // network_carry_invariant_ (the slip-funnel feed), which
        // loadNetworkDatumSignalling_ has just parsed.  All three increments are
        // default off and, when off, allocate no state and fold no rows.  Not appended
        // to the warm-resume fingerprint, on purpose (see the NetworkConfig doxygen and
        // networkSnapshotFingerprint): forward-only election/row policy, so an off and
        // an on leg may share one snapshot, which is what makes the §6 shared-snapshot
        // event A/B legal.
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
            // < 1 would tighten an already hard gauge pin (meaningless) and <= 0 would
            // invert the row weight, so both are refused rather than folding a nonsense
            // datum.
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
            // The hygiene acts on the per-(sat, signal) anchor election.  Without one
            // (the legacy pivot datum) there is nothing to re-host and nothing to bridge:
            // the knobs are inert and are disarmed with a warning.
            const bool anchored_datum = network_phase_datum_prioritized_
                                        || network_phase_datum_eq34_;
            if (hyg_any && !anchored_datum) {
                log->warn("Network (eq34 hygiene): the reattach-hygiene knobs are set "
                          "but this run has no per-(sat,signal) anchor election "
                          "(needs network.phase_datum_eq34 or "
                          "network.phase_datum_prioritized) — there is no reattach to "
                          "guard, so they are INERT (disabled).");
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
                // Invariant #1 disclosure: the arc-age map's slip response reads the
                // card-4 slip funnel (carry_invariant-gated) unioned with the
                // phase-leveling pending-reseed flags (carry_warm-gated).  With neither
                // live, the age resets only on arc absence, so a slip repaired in place
                // would leave a "mature" age on a just-refloated ambiguity, which is the
                // code-seeded host INC-1 exists to refuse.  The warning says so, so the
                // configuration does not look guarded.
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
    //  network subsection helper: loadNetworkSignalIntegrity_
    // =================================================================
    /**
     * @brief Flicker / gap signal integrity (@c network.signal_integrity.*).
     *
     * Ordered after the cycle-slip block and the carry parse: the inert guards read
     * @c network_carry_invariant_ / @c network_phase_leveling_, the two refloat funnels
     * INC-3's regain response rides.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkSignalIntegrity_(const nlohmann::json& json,
                                                 const std::shared_ptr<spdlog::logger>& log) {
        // --- Flicker / gap signal integrity (docs/SPEC_flicker_gap_integrity.md) --
        // Placed here because the inert guards below need network_carry_invariant_ and
        // network_phase_leveling_ (the two slip-funnel feeds INC-3's refloat uses) and
        // the cycle-slip block (parsed far above) to be resolved already.  Default off;
        // off allocates no map, changes no row weight and touches no observable.  Not
        // appended to the warm-resume fingerprint, on purpose (forward-only
        // weight/refloat policy, as for the eq34 hygiene), which
        // keeps a shared-snapshot A/B legal.
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

            // Range hygiene runs before the arming report, so the report states what the
            // run will do rather than what was typed.
            if (si.revet_ep < 0) si.revet_ep = 0;
            if (si.flicker_max_ep < 0) si.flicker_max_ep = 0;
            // Positive-definiteness headroom.  The demotion divides a phase row's
            // information by scale²; a phase pivot ~1/σ_φ² ≈ 1e5 sits against a
            // per-station relative bar of 1e-12·d_ref, and at an anchor station d_ref can
            // be the 1e10 hard datum pin, so the bar is ~1e-2 and a scale beyond ~1e4
            // pushes the row into it.  A row that trips the PD guard drops the whole
            // station for the epoch, which is the hard exclusion the fallback doctrine
            // forbids, so a typo is clamped rather than allowed to cause it.
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

            // The demotion has three resolved dispositions, and the arming line names the
            // one in force: withhold (rows removed), deweight (σ scaled), or none (the
            // census-only leg of spec §6C).  A scale in (0, 1] is not an error (it is how
            // a census leg is expressed without a second knob), but it is reported,
            // because a config that set 0.5 expecting a tightening would otherwise get
            // no feedback.
            const bool withhold = si.withhold_instead || si.unvetted_sigma_scale <= 0.0;
            const bool deweight = !withhold && si.unvetted_sigma_scale > 1.0;
            const bool acts     = withhold || deweight || si.refloat_on_regain;

            // The INC-2/3 actions require the census: there is no band state to act on
            // unless it is tracked, and an action wired behind a classifier that never
            // ran is the "clean log over dead wiring" failure invariant #4 exists to
            // prevent.  The loader warns and disables rather than silently promoting the
            // master.
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
                // Cold start and warm resume (spec addendum) are different situations and
                // are treated oppositely, so the log states which is which rather than
                // letting a reader assume symmetry.
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
                // Invariant #1 disclosure, mirroring the eq34 hygiene warning: INC-3's
                // refloat uses the same two funnels as the slip detectors.  With neither
                // live there is nothing to insert into, so the regain response degrades
                // to a counter; the warning says so, so an armed knob does not look
                // guarded.
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
     * @brief Snapshot-carry validation, the emission re-base knobs, and the estimability knobs.
     *
     * RTB-7(c/d/e) carry-blob requirements (now that @c network.carry_invariant is known),
     * the phase-datum sub-knobs, the OSB prior of card KP, the
     * rank-gate tolerance of card DG and the pin-aware station PD guard.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     * @return @c false on a fatal misconfiguration (aborts @c loadFromJson).
     */
    bool GnssConfig::loadNetworkEmissionFilters_(const nlohmann::json& json,
                                                 const std::shared_ptr<spdlog::logger>& log) {
        // --- Census-fed H σ̂₀ guard (H-rider, edge-queue §1.6 item 3a) ----------
        // Parsed in loadNetworkClockModelAndChannels_ (next to the heartbeat it
        // feeds); validated here because the empty-mask case is a hard load error and
        // that helper returns void.  Both prerequisites are parsed by now.
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
        // The branch-faithful carry blob (RTB-7(c)) requires the invariant carry to be
        // armed and a snapshot path to write to (parsed above); enforced and logged here
        // now that network_carry_invariant_ is known.
        if (network_config_.state_snapshot_carry) {
            if (network_config_.state_snapshot_path.empty() || !network_carry_invariant_) {
                log->warn("Network (RTB-7c): network.state_snapshot_carry requires "
                          "state_snapshot_path + carry_invariant; disabling (the carry blob "
                          "would be inert).");
                network_config_.state_snapshot_carry = false;
                network_config_.state_snapshot_carry_diagonal = false;  // no blob to shape (RTB-7(e))
                network_config_.state_snapshot_carry_lin = false;       // no blob to base (RS-2)
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
        // Clock emission re-base (card CLK-R; docs/SPEC_clock_emit_rebase.md): stage 1
        // holds the per-constellation common mode at emission (the re-realization class
        // seen in 613 of 1440 census epochs), stage 2 marks the surviving per-sat step
        // class through the R-record retro-bump.  Both outputs-only, independent,
        // default off.
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
        // Guarded temporal clock anchor (card Q3; docs/SPEC_clock_temporal_anchor.md).
        // Solve-affecting and fingerprinted; every knob is inert unless
        // network.clock_temporal_anchor is on, so an unarmed run is byte-identical.
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
        // Card CTU section 14: the guard's statistic in the phase-visible complement.
        {
            const std::string sp = jval<std::string>(json, "network.clock_anchor_guard_space",
                                                     "clock_anchor_guard_space",
                                                     std::string{"clock"});
            if (sp == "clock")             network_clock_anchor_guard_space_ = 0;
            else if (sp == "phase_shadow") network_clock_anchor_guard_space_ = 1;
            else if (sp == "phase")        network_clock_anchor_guard_space_ = 2;
            else {
                log->error("Network: network.clock_anchor_guard_space = '{}' is not one of "
                           "clock|phase_shadow|phase.", sp);
                return false;
            }
            network_clock_anchor_phase_sigma_floor_m_ =
                jval<double>(json, "network.clock_anchor_phase_sigma_floor_m",
                             "clock_anchor_phase_sigma_floor_m", 0.02);
            if (!(network_clock_anchor_phase_sigma_floor_m_ > 0.0)) {
                log->error("Network: network.clock_anchor_phase_sigma_floor_m must be > 0.");
                return false;
            }
        }
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
        // Bisection knob (card Q3 §2.1).  Parsed as a name rather than a bool pair
        // because the four settings are not two independent flags: `auto` is a
        // reproduction contract (it must keep tracking whatever the fold does), and
        // `post` exists only to reproduce the shipped gauge-free arm exactly.  An
        // unrecognised name is logged as an error and never falls back silently: a
        // mistyped denominator would look like a clean run with the wrong statistic,
        // the failure this knob exists to expose.
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
            // Once the carry binds, the realized walk is √(q·Δt) exactly (steady-state
            // scalar identity), so the armed q is reported as the walk it targets, the
            // number the wire census is graded against.
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
            // The guard's denominator is a separate choice from the fold, and naming the
            // armed one is the purpose of the bisection: two arms that differ only here
            // are otherwise indistinguishable in the log.
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
            // NOTE: the clock_model interlock is not here: service_mode force-arms
            // network_clock_model_ later in the section (applyNetworkServiceProfile_), so
            // a check here would read the value before arming and misreport the active
            // mechanism, the defect card C1 recorded against this knob pair.  It lives in
            // the P3 post-arming re-validation block instead.
        }
        // Card CTU (docs/refound/CTU_coupling_time_update_2026-09-28.md section 9.3): the
        // measure-only level monitor and the raise of the stashed block to its bound.
        network_clock_level_monitor_ =
            jval<bool>(json, "network.clock_level_monitor", "network_clock_level_monitor", false);
        network_clock_level_innov_window_ep_ =
            jval<int>(json, "network.clock_level_innov_window_ep",
                      "clock_level_innov_window_ep", 60);
        // The phase-visible consistency instrument (card PVN).  Measure-only; the prior it
        // needs is the dense fold's capture, which only the within-epoch RAIM keeps.
        network_phase_visible_monitor_ =
            jval<bool>(json, "network.phase_visible_monitor", "phase_visible_monitor", false);
        network_phase_visible_window_ep_ =
            jval<int>(json, "network.phase_visible_window_ep", "phase_visible_window_ep", 60);
        if (network_phase_visible_window_ep_ < 2) {
            log->error("Network: network.phase_visible_window_ep must be >= 2.");
            return false;
        }
        if (network_phase_visible_monitor_ && !network_raim_within_epoch_) {
            log->error("Network (card PVN): network.phase_visible_monitor requires "
                       "network.raim_within_epoch (its fold capture is the instrument's prior).");
            return false;
        }
        if (network_phase_visible_monitor_)
            log->info("Network (card PVN): network.phase_visible_monitor = true: per satellite the "
                      "ionosphere-free phase-visible clock D = c - sum_j w_j p_j (one phase bias per "
                      "band) is traced with its contrast update against the fold's prior, "
                      "normalized by the contrast-projected P- - P+ without a floor (NIS toward 1 "
                      "for an honest covariance), and its posterior contrast value and variance "
                      "(logs/network_phase_visible.csv; window {} epochs).  Measure-only: the "
                      "solve and every product are unchanged.",
                      network_phase_visible_window_ep_);
        network_clock_level_restore_raise_ =
            jval<bool>(json, "network.clock_level_restore_raise", "clock_level_restore_raise", false);
        // Card CTU G5 / card EXL: the switch-on resume direction (a snapshot written without
        // the card's tokens resumed into a run that arms them) is admitted only on request;
        // card CTU2 section 8 graded that seam and it failed G5.
        network_ctu_switch_on_resume_ =
            jval<bool>(json, "network.ctu_switch_on_resume", "ctu_switch_on_resume", false);
        if (network_ctu_switch_on_resume_)
            log->warn("Network (card CTU G5): network.ctu_switch_on_resume ON: a snapshot written "
                      "without the exact update, the bound raise, the in-block receiver code-bias "
                      "carry, the drop decouple or the phase-space guard is resumed into a run that "
                      "arms them.  That seam FAILED card CTU's G5 (the restored ambiguities hold the "
                      "frozen level); a measurement lineage only.");
        network_clock_level_restore_credit_ep_ =
            jval<int>(json, "network.clock_level_restore_credit_ep", "clock_level_restore_credit_ep", 0);
        if (network_clock_level_restore_credit_ep_ < 0) {
            log->error("Network: network.clock_level_restore_credit_ep must be >= 0.");
            return false;
        }
        if (network_clock_level_restore_raise_ && !network_clock_level_monitor_) {
            log->error("Network: network.clock_level_restore_raise requires "
                       "network.clock_level_monitor (it raises to the monitor's code-only "
                       "level information).");
            return false;
        }
        if (network_clock_level_innov_window_ep_ < 2) {
            log->error("Network: network.clock_level_innov_window_ep must be >= 2 (got {}).",
                       network_clock_level_innov_window_ep_);
            return false;
        }
        network_clock_level_bound_raise_ =
            jval<bool>(json, "network.clock_level_bound_raise",
                       "network_clock_level_bound_raise", false);
        if (network_clock_level_bound_raise_ && !network_clock_level_monitor_)
            throw std::runtime_error(
                "network.clock_level_bound_raise requires network.clock_level_monitor = true "
                "(the raise reads the monitor's per-satellite information).");
        if (network_clock_level_monitor_) {
            if (!network_shared_carry_full_cov_)
                throw std::runtime_error(
                    "network.clock_level_monitor requires network.shared_carry_full_cov = true "
                    "(it reads the level variance from the stashed dense border block).");
            log->info("Network producer (card CTU): level monitor ON: per satellite the "
                      "code-only level information I_L (undecayed) against the stashed block's "
                      "level variance P_L; the ratio P_L/(1/I_L) traced and read out.  Bound "
                      "raise {}.", network_clock_level_bound_raise_
                                   ? "ON (solve-affecting: P += (1/I_L - P_L) u u' where the ratio is below 1)"
                                   : "off (measure-only, solve byte-identical)");
        }
        {
            // Card HLT: broadcast satellite health on the network path.
            const std::string health =
                jval<std::string>(json, "network.sat_health", "network_sat_health",
                                  std::string("off"));
            if (health == "off") {
                network_sat_health_ = 0;
            } else if (health == "pivot") {
                network_sat_health_ = 1;
            } else if (health == "exclude") {
                network_sat_health_ = 2;
            } else {
                log->warn("Network (card HLT): network.sat_health = '{}' is not one of off, "
                          "pivot, exclude; using off.", health);
                network_sat_health_ = 0;
            }
            if (network_sat_health_ != 0)
                log->info("Network producer (card HLT): broadcast satellite health '{}': {}.  "
                          "SOLVE-AFFECTING + FINGERPRINTED: fresh convergence only.",
                          health,
                          network_sat_health_ == 1
                              ? "the clock-pivot election passes over a satellite the broadcast "
                                "flags unhealthy at the epoch, whose observations are processed"
                              : "the observations of a satellite the broadcast flags unhealthy "
                                "at an epoch are removed from the synced epochs");
        }
        network_phase_datum_pref_elev_deg_ =
            jval<double>(json, "network.phase_datum_pref_elev_deg",
                         "network_phase_datum_pref_elev_deg", 15.0);
        network_phase_datum_sticky_ =
            jval<bool>(json, "network.phase_datum_sticky",
                       "network_phase_datum_sticky", true);
        // Two-edge continuous handover (docs/SPEC_pivot_visibility_gauge_defect.md).
        // Stickiness minimizes the number of handovers; this makes the ones that still
        // happen frame-preserving.  The two are complementary, not alternatives: sticky
        // alone still re-realizes on every handover it cannot avoid, which is what
        // produced 15 clock steps under the prioritized election.
        network_phase_datum_handover_continuous_ =
            jval<bool>(json, "network.phase_datum_handover_continuous",
                       "network_phase_datum_handover_continuous", false);
        // ---- The phase datum as a held S-basis (RF-2;
        //      docs/refound/RF-1_steps345_mechanism_2026-09-15.md).  RF-1 made every
        //      ambiguity a float state, including the one that is the satellite phase
        //      bias's datum; in the textbook formulation the S-basis ambiguities are
        //      constants, not states.
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
        // ---- The held-basis handover in its own frame algebra (RF-2d;
        //      docs/refound/RF-3-read_handover_frame_2026-09-15.md): release a cold
        //      family's retired-frame carries, and declare exactly the cold handovers.
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
        // The σ-scale ceiling (SPEC_amb_iono_split_overcount §6.8.3 / §7 item 15): a
        // ledger value is usable only if var <= ratio × its arc's first-stash
        // conditional variance.  0 = off (the absolute ceiling above alone).
        network_phase_datum_handover_max_var_ratio_ =
            jval<double>(json, "network.phase_datum_handover_max_var_ratio",
                         "network_phase_datum_handover_max_var_ratio", 0.0);
        if (!(network_phase_datum_handover_max_var_ratio_ >= 0.0)) {
            log->error("Network: network.phase_datum_handover_max_var_ratio must be >= 0 "
                       "(got {}); 0 disables the ratio test.",
                       network_phase_datum_handover_max_var_ratio_);
            return false;
        }
        // Cold anchor fallback (docs/SPEC_event_harness.md §15 + §16.8).  The two knobs
        // above decide what target a handover's successor is pinned at; this one decides
        // which station reaches that decision.  Without it the election takes the first
        // coverage-ranked tracker and only then discovers that its carry is unusable, so
        // the family re-realizes cold while a lower-ranked tracker with a mature usable
        // arc sits unused (43 of 47 non-cold-start cold handovers on the storm arm had
        // >= 3 mature alternatives).  Default off = byte-identical.
        network_phase_datum_candidacy_usable_ =
            jval<bool>(json, "network.phase_datum_candidacy_usable",
                       "network_phase_datum_candidacy_usable", false);
        // The satellite clause, iteration 2 (SPEC_event_harness §15.8).  The per-family
        // screen of iteration 1 let a satellite's three bands elect three hosts:
        // producer channels improved on every axis while the consumer median rose 49 %,
        // with split-referenced satellites going from 0.0 to 0.9 mean / 3 max.  Default
        // true, so the knob above ships its corrected form; off reproduces iteration 1
        // exactly, which makes the A/B a single knob.
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
        // Carry re-reference at the PHASE_ANCHOR handover (queue item 1(d);
        // docs/SPEC_pivot_visibility_gauge_defect.md §5.07.5 R1).  The handover knob
        // above acts on the design-matrix side of the edge (what target the incoming
        // pin gets); this one acts on the carry side (the retired-frame prior is
        // marginalized for one epoch instead of being folded against the re-realized
        // datum at q_pb = 1e-12).  §5.07 measured the untransformed carry, not the pin
        // target, as the root cause that the satellite clock absorbs.
        network_phase_anchor_carry_reref_ =
            jval<bool>(json, "network.phase_anchor_carry_reref",
                       "network_phase_anchor_carry_reref", false);
        // Coverage-threshold gating of the anchored union: a shared phase-bias
        // column enters the union only when >= this many present stations track it
        // with a usable ambiguity.  1 (default) = every tracked key, the original
        // prioritized-datum behavior (byte-identical); 2-3 excludes the thinly-
        // tracked columns that eject marginal stations as non-PD, re-level on a
        // one-station anchor pool, and leak their anchor lump into the clock↔PB
        // split.  Inert unless network.phase_datum_prioritized is on.
        network_phase_datum_min_stations_ =
            jval<int>(json, "network.phase_datum_min_stations",
                      "network_phase_datum_min_stations", 1);
        if (network_phase_datum_min_stations_ < 1) {
            log->error("Network: network.phase_datum_min_stations must be >= 1 (got {}); "
                       "1 = no gating (every tracked key enters the anchored union).",
                       network_phase_datum_min_stations_);
            return false;
        }
        // Drop edge of the coverage gate (card EV7 §6;
        // docs/refound/EV7_all_sat_clock_decouple_2026-09-21.md §5.3).  The gate above
        // assumes the tracking stations' float ambiguities absorb the removed bias; on
        // the float vehicle that float is a carried state at a centimetre prior, so it
        // absorbs the whole bias in one epoch and the satellite clocks pay for it.
        // Armed, the dropped family joins the re-reference set and its carries are
        // erased at the edge, as at the family's return.
        network_phase_datum_gate_drop_erase_ =
            jval<bool>(json, "network.phase_datum_gate_drop_erase",
                       "network_phase_datum_gate_drop_erase", false);
        // The conditional second lever (card KP; docs/SPEC_beidou_w6_datum.md §4),
        // behind the DG rank-gate calibration and the PA drop-and-heal.  Default off;
        // inert without estimate_sat_code_bias (no OSB columns exist).
        network_cb_kaula_prior_ =
            jval<bool>(json, "network.cb_kaula_prior", "network_cb_kaula_prior", false);
        network_cb_kaula_sigma_m_ =
            jval<double>(json, "network.cb_kaula_sigma_m",
                         "network_cb_kaula_sigma_m", 10.0);
        if (network_cb_kaula_prior_ && !(network_cb_kaula_sigma_m_ > 0.0)) {
            // info = 1/sigma^2 with sigma <= 0 is a hard pin on every admitted OSB, a
            // silent datum takeover under a "loose prior" label, so it is refused.
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
        // The shared-border null-space rank gate's relative eigenvalue tolerance as a
        // per-config calibration parameter (card DG; docs/SPEC_beidou_w6_datum.md §2).
        // Default 1e-9 == NetworkEstimator::kNullSpaceTol (which stays the single source
        // of that default), so an unset key sends the identical double through the
        // identical arithmetic: byte-identical.  The resolved value is handed to
        // NetworkEstimator::setRankGateTol once, before the loop, and read from there by
        // all three consumers (gate, estimability dump, prioritized drop-and-heal), per
        // the DG2 "no private tolerances" rule.
        network_rank_gate_tol_ =
            jval<double>(json, "network.rank_gate_tol",
                         "network_rank_gate_tol", 1e-9);
        // The dense border fold's relative-pivot bound (card CTU section 14).  Default 1e-12
        // from card SMH section 9 (2026-09-29): on the clean start the honest engagement
        // block is admitted, the level gates and card PVN are unchanged or better, and the
        // golden posture is byte-identical to the historical 1e-6 (2 h, both beds).
        network_dense_fold_kappa_ =
            jval<double>(json, "network.dense_fold_kappa", "dense_fold_kappa", 1e-12);
        if (!(network_dense_fold_kappa_ > 0.0) || network_dense_fold_kappa_ > 1e-6) {
            log->error("Network: network.dense_fold_kappa must be in (0, 1e-6] (got {:g}).",
                       network_dense_fold_kappa_);
            return false;
        }
        if (!(network_rank_gate_tol_ > 0.0)) {
            // NaN also lands here (every comparison is false), on purpose: a NaN cutoff
            // makes every `ev < cutoff` test false and would report a rank-0 null space
            // on a well-posed system.
            log->error("Network: network.rank_gate_tol must be > 0 (got {:g}); it is the "
                       "RELATIVE eigenvalue tolerance of the shared null-space rank gate "
                       "(cutoff = tol x max(lambda_max, 1)).  Default 1e-9.",
                       network_rank_gate_tol_);
            return false;
        }
        if (network_rank_gate_tol_ < 1e-16) {
            // Clamp rather than fail: a tolerance below double's relative resolution
            // against lambda_max cannot count any direction as null, which silently
            // turns the gate into "declared_dim must be 0", an off switch disguised as a
            // tuning value.  Invariant #4: an inert safety gate must never look clean.
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
        // SBC S-basis completion (network.sbasis_complete): derives the epoch's realized
        // S-basis dimension before the gate and constrains the near-null directions
        // beyond the declared structural dimension along their own eigenvectors (RHS =
        // carried level), instead of withholding the epoch or anchoring columns as
        // card PA does.  Built for the FT63 GPS+Gal signature of 4 observed against 2
        // declared: per-satellite channel-family phase-bias↔iono trade directions that
        // a second constellation's C/Q-vs-X receiver split makes reachable and the
        // per-(sat, signal) anchor pins cannot close.
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
        // Datum graph census (docs/SPEC_datum_graph_census.md).  Parsed here, next to
        // SBC, because it mirrors SBC: SBC measures the realized S-basis dimension
        // spectrally and closes it, and this census predicts the same dimension from
        // the epoch's support topology (LASER 5-62/5-63) and reconciles the two.
        // Measure-only and fingerprint-safe: it moves no weight, no state and no
        // declared dimension, so it is legal on a warm resume and against a shared
        // snapshot.
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
        // Pin-aware station PD guard: references the addStation local-block rank test
        // to the median |pivot| instead of the max, so a deliberate in-block 1e10
        // anchor pin (prioritized phase datum) does not inflate the rejection bar by
        // ~10 orders and eject a sound marginal station every epoch (the TWTF/VOIM
        // class; an A/B with the coverage gate on and off showed identical failing
        // pivots, so the ejection is pin-driven).
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
     * @c innov_gate -> @c carry_invariant).
     *
     * @param log   The "gnss_config" logger.
     * @return @c false on a fatal misconfiguration (aborts @c loadFromJson).
     */
    bool GnssConfig::validateNetworkDatumCoherence_(
                                       const std::shared_ptr<spdlog::logger>& log) {
        // Alias preamble: the body below reads the driver local it was written
        // against.  network_service_mode_ is written once, at the top of the
        // section, and never again, so this is the value the unsplit code saw.
        const bool service_mode = network_service_mode_;

        // The B1 retro-bump steps a flagged satellite's phase, code and clock discontinuity
        // counters, but the emitter maintains and stamps the clock counter only when
        // emit_clock_disc is on (otherwise disc_counter stays 0 for byte-identity).  With
        // ura_retro_bump on and emit_clock_disc off, the clock channel would silently never
        // bump, and the consumer would reset the phase/code ambiguities of a contaminated
        // satellite but keep integrating its bad clock.  The two are therefore coupled: the
        // clock-disc channel is armed so the retro-bump reaches it (R4).
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
        // already switched the product to the clock-disc format, so a knob that has no effect
        // except a format change gets a warning.
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
            // A pivot pin and a sum-to-zero are mutually exclusive gauge realizations:
            // sum-to-zero has no pivot to place, so an explicit pin is a no-op here.
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
            // Exactly one clock-gauge realization may be selected: two datums on the same
            // gauge would over-constrain it, and the rank gate would not catch that (both
            // close the deficiency).  The load fails.
            if (network_clock_datum_sum_to_zero_) {
                log->error("Network: network.clock_datum_broadcast_anchored and "
                           "network.clock_datum_sum_to_zero are mutually exclusive clock-gauge "
                           "realizations.  Pick one.");
                return false;
            }
            // The anchor target is each satellite's own a-priori clock, and which
            // a-priori that is changes what the toggle means:
            //
            //  - brdc_rts: the target is the broadcast clock, a stable external
            //    reference the datum exists to hold the absolute frame against, and the
            //    auto resolution (3.0 m, broadcast-grade) binds it.
            //  - sp3: the target is the precise product clock.  Anchoring to it
            //    re-derives the product, which is the purpose of the external-clock hold
            //    rung of the external-constraint ladder: the satellite-clock state is a
            //    residual to the a-priori (the estimateSatClocks() block of the
            //    measurement model in MeasurementModel.cpp), so Δδtˢ → 0 at a tight σ
            //    means "hold the satellite clock at the .CLK value", and σ is a
            //    continuous dial from fully external to fully self-contained through
            //    one code path.  The sp3 sigma auto resolution, however, is
            //    initial_std.sat_clock (default 1000 m), which leaves the gauge
            //    unrealized at the binding gate below.  So sp3 is legal only with an
            //    explicit σ; an omitted key there is always a mistake, never a posture.
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
            // The anchors are the gauge: each satellite's single-entry anchor row folds
            // (1/σ²)·I onto that constellation's clock columns, so the clock common-mode
            // direction u = (1/√N)·1 realizes eigenvalue uᵀ(σ⁻²I)u = 1/σ², independent of
            // N (not ≈ N/σ²).  That eigenvalue must clear the per-epoch rank-gate cutoff
            // (≈ 1e-9·smax, and smax grows with the station count), or the datum reads as
            // under-specified and every epoch is withheld.  A fixed load-time bound can
            // only be a necessary floor: the clearly non-binding is rejected here, and the
            // range that a large network would reject at run time gets a warning.  The
            // brdc_rts auto default is 3.0 m (1/σ² ≈ 0.11, safe at any realistic scale);
            // only an explicit override can be this weak.
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
                // sp3 + anchored = the external-clock hold.  The log says so, and says
                // what it invalidates: the emitted satellite clock is then the precise
                // product plus a σ-bounded residual by construction, so grading it
                // against that product measures the anchor, not the network.  The
                // information this posture produces is on the receiver side (rx clock,
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
            // estimate_sat_code_bias there is no satellite code-bias state and no split
            // to hold, so arming the toggle alone changes nothing.  It warns rather than
            // silently ignoring the knob (a common mis-arming: the toggle targets the
            // same datum whose instability motivates it, which exists only in W6).
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
        // The time-linked code-bias datum (card 2) and clock_signal_basis_hold both
        // re-target the same per-satellite IF-pin row (the clock↔code split).  Running
        // both would realize that split twice with conflicting references; time-linking
        // already subsumes the hold for the pin value (its RHS references the previous
        // solved IF combination directly).  The load fails rather than letting one win
        // silently.
        if (network_code_datum_time_linked_ && network_clock_signal_basis_hold_)
            throw std::runtime_error(
                "network.code_datum_time_linked and network.clock_signal_basis_hold are "
                "mutually exclusive (both re-target the per-satellite IF-pin row; the time-"
                "linked datum subsumes the hold for the pin value).  Enable at most one.");
        // Time-linking makes the GF pin and the sum-to-zero rows inconsistent, not merely
        // redundant (FAMILY-GF §2.1).  The GF rows over a channel pair sum exactly to
        // (s2z(sig2) - s2z(sig1))/(mu2-mu1), a rank-1 dependency that is harmless while
        // every RHS is 0, which is the untimed case.  Time-linking moves the s2z targets
        // to Sigma b_prev, generally different per channel, so the same dependency then
        // demands Sigma d_2 - Sigma d_1 = 0 and Sigma d_j = rhs_j with rhs_2 != rhs_1.
        // Nothing throws, because these fold as heavily weighted pseudo-observations
        // rather than exact constraints: the solve absorbs the conflict by least
        // squares, injecting tension of order (rhs_2 - rhs_1)/(mu2 - mu1).  Latent (no
        // shipped config combines them) and pre-existing for the primary pin, so this
        // warns rather than refuses; it is named here so the next arm is aware of it.
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
        // Card GFC's two arms refine the time-linked datum and are read only under it (and
        // under the W6 satellite code bias it re-times); pair continuity also only where no
        // row re-asserts an absolute target at a pin pair change
        // (NetworkSbasis::pinPairContinuityBlocker, the builder's own test).  Not forced
        // off: an inert arm is warned here, naming only what is missing, and reported as
        // inert at run end.
        if (network_code_datum_pair_continuity_ || network_code_datum_family_difference_) {
            const bool pc = network_code_datum_pair_continuity_;
            const bool fd = network_code_datum_family_difference_;
            std::string missing;
            if (!network_code_datum_time_linked_) missing = "network.code_datum_time_linked";
            if (!estimate_network_sat_code_bias_)
                missing += std::string(missing.empty() ? "" : " and ")
                         + "network.estimate_sat_code_bias";
            if (!missing.empty()) {
                log->warn("Network (GFC): {} {} INERT without {}.  {} the time-linked code "
                          "datum, so this run realizes the datum without {}.",
                          pc && fd ? "network.code_datum_pair_continuity and "
                                     "network.code_datum_family_difference"
                          : pc     ? "network.code_datum_pair_continuity"
                                   : "network.code_datum_family_difference",
                          pc && fd ? "are" : "is", missing,
                          pc && fd ? "Both refine" : "It refines", pc && fd ? "them" : "it");
            } else {
                const char* pc_blocker = NetworkSbasis::pinPairContinuityBlocker(
                    network_code_bias_gf_datum_, network_config_.sat_gf_datum,
                    network_config_.sat_gf_datum_time_linked,
                    network_config_.sat_if_level_datum);
                if (pc && pc_blocker)
                    log->warn("Network (GFC): network.code_datum_pair_continuity is INERT "
                              "under {}.  A continued pin would hold the new pair's "
                              "ionosphere-free combination while that row moves its other "
                              "combination, a step no discontinuity declares, so the pin "
                              "follows the selection and every pin pair change re-seeds as "
                              "without the arm.",
                              pc_blocker);
                else if (pc)
                    log->info("Network producer (GFC): IF-pin PAIR CONTINUITY ON (hold, then "
                              "continue).  At an entering edge a satellite keeps its last "
                              "kept pin pair for the one kept epoch the entering signal "
                              "needs to get a previous solved value (the held pair's own "
                              "previous combination carries; each hold's begin and end is a "
                              "CLK_BASIS/held event); no hold where the held pair has no "
                              "previous values or, under sat_gf_datum broadcast_tgd, the "
                              "group delay describes the selected pair and not the held one.  "
                              "At a pair change whose two new signals both have a previous "
                              "solved value (a released hold or a leaving signal), the pin "
                              "targets the new pair's previous solved ionosphere-free "
                              "combination, so the satellite clock continues: no re-seed "
                              "mark, and with network.emit_clock_disc no re-selection "
                              "discontinuity (a CLK_BASIS/continued event).  Without previous "
                              "values the re-seed at 0 stands, and a channel-level re-seed "
                              "still marks the satellites it re-references.  Unvalidated "
                              "(card GFC).");
                if (fd)
                    log->info("Network producer (GFC): FAMILY-DIFFERENCE code datum ON "
                              "(Galileo).  A secondary-family (X) code channel whose "
                              "sum-to-zero row survives carries Sum (d_X - d_P) over the "
                              "satellites tracking it and its same-band primary-family partner "
                              "instead (the inter-family mean zero of COD's convention), "
                              "time-linked on the satellites with previous values on both; the "
                              "sum-to-zero row is kept with no partner, no common satellite, "
                              "spanned columns all pinned by other rows, or for the one epoch "
                              "a family re-seed would replace a carried level.  Each switch is "
                              "a CB_DATUM/family_row event.  Unvalidated (card GFC).");
            }
        }
        // Card GFC step 2: the first-sighting family prior refines the family difference and
        // is read only with it, under the time link and the W6 satellite code bias.  A
        // negative or non-finite sigma would fold a hard pin, or nothing, under a "soft
        // prior" label, so it is refused.  So is a sigma whose information exceeds ten times
        // the code-bias gauge weight (sigma < 0.01 m): the rows would then override the
        // family row, the sum-to-zero row and the ionosphere-free / geometry-free pins they
        // refine wherever they disagree.  At or above the gauge weight itself (sigma at or
        // below 1/sqrt(1e3), about 0.032 m) the prior is as stiff as a datum row, which is
        // warned.  Not forced off when inert: the warning names only what is missing, and
        // the run end reports the arm as inert.
        {
            const double bp_sigma = network_code_family_birth_prior_sigma_m_;
            if (!(std::isfinite(bp_sigma) && bp_sigma >= 0.0)) {
                log->error("Network (GFC): network.code_family_birth_prior_sigma_m must be "
                           "finite and >= 0 (got {:g}); it is the standard deviation of the "
                           "first-sighting family prior (information 1/sigma^2), and 0 is off.",
                           bp_sigma);
                return false;
            }
            constexpr double kBirthMinSigmaM = 0.01;   // information 1e4 = 10 x the gauge weight
            const double gauge_w = NetworkSbasis::kCodeBiasGaugeWeight;
            if (bp_sigma > 0.0 && bp_sigma < kBirthMinSigmaM) {
                log->error("Network (GFC): network.code_family_birth_prior_sigma_m = {:g} m is "
                           "below {:g} m: its information {:g} exceeds ten times the code-bias "
                           "gauge weight {:g}, so the soft prior would override the datum rows "
                           "it refines (the family row, the sum-to-zero row, the "
                           "ionosphere-free and geometry-free pins) wherever they disagree.",
                           bp_sigma, kBirthMinSigmaM, 1.0 / (bp_sigma * bp_sigma), gauge_w);
                return false;
            }
            if (bp_sigma > 0.0 && 1.0 / (bp_sigma * bp_sigma) >= gauge_w)
                log->warn("Network (GFC): network.code_family_birth_prior_sigma_m = {:g} m gives "
                          "information {:g}, at or above the code-bias gauge weight {:g} (sigma "
                          "<= {:.4f} m): the first-sighting prior is as stiff as the datum rows "
                          "it refines and competes with them where they disagree, rather than "
                          "acting as a soft prior.",
                          bp_sigma, 1.0 / (bp_sigma * bp_sigma), gauge_w,
                          1.0 / std::sqrt(gauge_w));
            if (bp_sigma > 0.0) {
                std::string missing;
                const auto need = [&](bool have, const char* knob) {
                    if (!have) missing += std::string(missing.empty() ? "" : " and ") + knob;
                };
                need(network_code_datum_family_difference_, "network.code_datum_family_difference");
                need(network_code_datum_time_linked_, "network.code_datum_time_linked");
                need(estimate_network_sat_code_bias_, "network.estimate_sat_code_bias");
                if (!missing.empty())
                    log->warn("Network (GFC): network.code_family_birth_prior_sigma_m = {:g} m "
                              "is INERT without {}.  The prior refines the time-linked "
                              "family-difference datum, so this run folds no first-sighting "
                              "family row.",
                              bp_sigma, missing);
                else
                    log->info("Network producer (GFC): first-sighting FAMILY PRIOR ON, sigma "
                              "{:g} m (Galileo band 1).  When a satellite's secondary-family "
                              "(X) code column or its primary partner's is born (the carry "
                              "does not continue it; a READMIT seed does) while the other is "
                              "registered, one soft shared row d_X - d_P = mu (mu = the mean "
                              "previous difference over the family row's veterans, or 0 where "
                              "the family row re-seeds at 0; no row where neither exists) is "
                              "folded with information {:g}, once per (satellite, birth): the "
                              "column is carried from the next kept epoch on.  Each fold is a "
                              "CB_DATUM/family_birth_prior event.  Unvalidated (card GFC step "
                              "2).",
                              bp_sigma, 1.0 / (bp_sigma * bp_sigma));
            }
        }
        // The sum-to-zero clock gauge re-references on a clock-satellite set change; with the
        // clock-disc channel live, that re-reference must bump the clock disc counters or the
        // consumer integrates across an unsignalled datum step (T3c).  The set-change trigger
        // is therefore auto-armed when both its preconditions (sum_to_zero + emit_clock_disc)
        // hold but it was left off, per the product-contract rule "auto-arm, or fail".  Fires
        // only under this combination; every other config is untouched.
        if (network_clock_datum_sum_to_zero_ && network_emit_clock_disc_
            && !network_clock_disc_on_set_change_) {
            log->warn("Network (T3c): auto-arming network.clock_disc_on_set_change — under the "
                      "sum-to-zero clock datum with the clock-disc channel on, a clock-satellite "
                      "set change re-references the gauge and MUST bump the clock disc counters "
                      "(otherwise the consumer integrates across an unsignalled datum step).");
            network_clock_disc_on_set_change_ = true;
        }
        if (network_clock_disc_on_set_change_) {
            // The trigger models "membership change = clock-datum re-reference", which
            // holds only for the sum-to-zero gauge (the pivot pin holds while the pivot
            // is up; the broadcast anchors never move).  Armed under any other datum it
            // would create spurious consumer resets, the churn class this work removes,
            // so the load fails.
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
        // The slip-aware election advisory (cards 3/4) belongs here, not in the parse block
        // that reads the knob: network_carry_invariant_ is parsed after that block, so a
        // check there would fire on every run regardless of the config.  By this point
        // every flag is settled.
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
        // The changed constellation's own carry is re-expressed, not dropped (queue 1(b)).
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
        // Card PRM phase 2: the handover's S-transform before the coupling rows.
        if (network_handover_stransform_before_rows_) {
            if (!network_handover_stransform_) {
                log->warn("Network: network.handover_stransform_before_rows is INERT without "
                          "an armed network.handover_stransform (it moves that transform before "
                          "PASS B).");
                network_handover_stransform_before_rows_ = false;
            } else if (network_carry_rx_clock_in_block_) {
                throw std::runtime_error(
                    "network.handover_stransform_before_rows refuses "
                    "network.carry_rx_clock_in_block: the receiver clock is on the clock datum's "
                    "direction, and the transform before the rows moves its coupling terms but not "
                    "its carried value, so its block row would be off by c0 at the handover epoch "
                    "(card PRM section 16.6)");
            } else if (network_apriori_source_ == NetworkAprioriSource::BrdcRts) {
                throw std::runtime_error(
                    "network.handover_stransform_before_rows refuses network.apriori_source "
                    "brdc_rts: the ephemeris cut-over re-level runs in the solve glue, after "
                    "PASS B, and must precede the handover's S-transform (the two re-bases "
                    "compose in the order the carry experienced them; card PRM section 16)");
            } else {
                log->info("Network producer (card PRM phase 2): handover S-transform BEFORE THE "
                          "ROWS: at a clock-pivot handover the election and the handover test are "
                          "previewed before the fold decision and PASS B, and the transform "
                          "re-expresses every clock reference of the changed constellation the "
                          "coupling rows read (the carried clocks, parked ones included, the "
                          "coupling terms' s_prev and free p_prev, the stash's pivot value), so the "
                          "rows are conditioned on the re-expressed variance.  FINGERPRINTED: fresh "
                          "convergence only.");
            }
        }
        // Card SCL B2: the dense S-transform at the handover.
        if (network_handover_stransform_dense_) {
            if (!network_handover_stransform_) {
                log->warn("Network: network.handover_stransform_dense is INERT without an armed "
                          "network.handover_stransform (it replaces that transform's diagonal "
                          "kernel).");
                network_handover_stransform_dense_ = false;
            } else if (!network_shared_carry_full_cov_) {
                log->warn("Network: network.handover_stransform_dense is INERT without "
                          "network.shared_carry_full_cov: there is no dense block to "
                          "re-express.");
                network_handover_stransform_dense_ = false;
            } else if (network_handover_stransform_before_rows_) {
                throw std::runtime_error(
                    "network.handover_stransform_dense refuses "
                    "network.handover_stransform_before_rows: the exact transform zeroes the "
                    "incoming pivot's row, which before the rows would put a pinned column into "
                    "card CTU's S and swap the pinned route of every coupling row, and the undo "
                    "of a skipped preview would need the saved block (card SCL section 5)");
            } else if (network_carry_rx_clock_in_block_) {
                throw std::runtime_error(
                    "network.handover_stransform_dense refuses network.carry_rx_clock_in_block: "
                    "the receiver clock is on the clock datum's direction and its carried value "
                    "is not re-expressed, so the block would hold two frames (card PRM section "
                    "16.6)");
            } else {
                log->info("Network producer (card SCL B2): handover S-transform DENSE: at a "
                          "clock-pivot handover the stashed border is re-expressed exactly, "
                          "x' = S x and P' = S P S^T with S = I - u e_q' over the constellation's "
                          "clock family (the incoming pivot's row zeroed, the outgoing pivot "
                          "appended at -c0), instead of the diagonal bump; a parked clock takes "
                          "the diagonal rule, and an edge the kernel cannot apply falls back to "
                          "the diagonal kernel (counted).  FINGERPRINTED: fresh convergence "
                          "only.");
            }
        }
        // Card SCL B1 needs B2: a clock-pivot handover re-expressed by the diagonal kernel
        // leaves the constellation's clones free while the rows still reference them, and
        // their common mode is then a null direction the datum does not remove (card SCL
        // section 10: 30 epochs of failed solves at the golden handover).  Read here, where
        // both knobs are resolved.
        if (network_carry_coupling_clock_clone_ && !network_handover_stransform_dense_)
            throw std::runtime_error(
                "network.carry_coupling_clock_clone requires network.handover_stransform_dense "
                "(armed, with network.handover_stransform and network.shared_carry_full_cov): "
                "a handover re-expressed by the diagonal kernel leaves the constellation's "
                "clock clones free and referenced, a null direction the clock datum does not "
                "remove (card SCL section 10)");
        if (network_clock_anchor_datum_keep_) {
            if (!network_handover_stransform_dense_) {
                log->warn("Network: network.clock_anchor_datum_keep is INERT without an armed "
                          "network.handover_stransform_dense: only an exactly re-expressed "
                          "carry may keep the anchor across a handover.");
                network_clock_anchor_datum_keep_ = false;
            } else if (!network_clock_temporal_anchor_) {
                log->warn("Network: network.clock_anchor_datum_keep is INERT without "
                          "network.clock_temporal_anchor: there is no anchor to keep.");
                network_clock_anchor_datum_keep_ = false;
            } else {
                log->info("Network producer (card SCL B2): the clock anchor is KEPT across a "
                          "handover whose carry the dense S-transform re-expressed (self-check "
                          "passed, a block row, an anchored fold): no datum decouple, no disc "
                          "mark, the guard's statistic still tests the satellite.  "
                          "FINGERPRINTED: fresh convergence only.");
            }
        }
        // Card-4 dependency (M1): the DD cycle-basis carry lives on the eq34 chain datum
        // (it needs the per-receiver RPB split and the spanning tree that supply the
        // fundamental-cycle DDs), so the incoherent combination is rejected.
        if (network_carry_invariant_ && !network_phase_datum_eq34_) {
            log->error("Network: network.carry_invariant requires network.phase_datum_eq34 — the DD "
                       "cycle-basis carry is defined only on the eq34 chain datum (RPB split + "
                       "spanning tree supply the invariant DDs).  Enable phase_datum_eq34 or disable "
                       "carry_invariant.");
            return false;
        }
        // The fine-slip innovation gate (card 4, M1, D5) is defined only on the carried DDs.
        if (network_carry_invariant_innov_gate_ && !network_carry_invariant_) {
            log->error("Network: network.carry_invariant_innov_gate requires network.carry_invariant — "
                       "the innovation test standardizes the CARRIED double-difference residual against "
                       "the fold's H0 covariance; with no z-store there is nothing to gate.  Enable "
                       "carry_invariant or disable the innovation gate.");
            return false;
        }
        // ---------------------------------------------------------------------
        // Invariant #1, applied to the card-4 invariant DD carry.
        // ---------------------------------------------------------------------
        // The carried z is a stiff time-linked quantity: the recursion is a Kalman
        // filter with Φ = I on z, and at the shipped default q_z = 0.0 it is a pure
        // random constant.  Invariant #1 requires such a quantity to have a wired event
        // response before an ON field run ("erase, refloat, or re-tap").
        //
        // What counts as that response for this carry (nothing else does):
        //   * the pre-fit raw detectors, network.cycle_slip.mw_detect (wide-lane + LLI)
        //     and network.cycle_slip.if_phase_td (IF-phase triple difference).  Both
        //     drain into carry_slipped_ambs, which networkBuildCarryDdPriors reads
        //     through ambSlipped() to re-tap the chord from a diffuse prior.  The drain
        //     is guarded by `if (carry_invariant)` alone, so it is live from epoch 1 and
        //     does not need the innovation gate.
        //   * the post-fit D5 innovation gate, network.carry_invariant_innov_gate, which
        //     re-taps in-epoch and enters the funnel for the next epoch.  D5 is the only
        //     response that fires on model error rather than on a detected raw slip, so
        //     it covers a failure mode the pre-fit detectors cannot see.
        //
        // q_z is not accepted as a response.  It adds q_z·dt to the diagonal of Pᶻ only
        // (NetworkInvariantCarryDriver::beginEpoch); it never moves the carried mean,
        // never touches the off-diagonals that pin a stale value in place, and is
        // applied identically to slipped and clean chords.  That is undirected decay,
        // not erase/refloat/re-tap: a q_z > 0 posture is leaky, not guarded, which is
        // the distinction invariant #1 draws.
        //
        // turbo_edit is not accepted either, even though the phase-leveling auto-arm in
        // loadNetworkSlipDetectors_ counts it as a pre-fit detector.  Under
        // carry_invariant its PASS-A branch takes the repair arm instead of collecting
        // MW refloat candidates, and the repair list it produces is consumed against the
        // phase-leveling local carry, the one carry_invariant supersedes.  It therefore
        // contributes nothing to carry_slipped_ambs.  A leveling config that arms
        // turbo_edit alone satisfies that auto-arm and still leaves this carry with no
        // response; this check covers that case.
        //
        // Relation to the posture bundle "zero-q phase memory + slip response" (the row
        // of that name in GnssConfigNetworkPosture.cpp).  That row names this invariant
        // too, but it is an info line inside the once-per-run posture dump, and its
        // companion list is the two raw detectors only, so it reports "0/2 companions
        // armed" on the 107 configs whose wired response is the D5 gate, and a reader
        // who skims past those false positives will skim past a real one.  This
        // interlock is the precise complement: warning severity, D5 counted as the
        // response it is, turbo_edit explicitly refused, and no false positives.
        //
        // A warning, not a rejection, and no state change: the solve stays
        // byte-identical (invariant #6) and no loader default moves.  In a census of the
        // config corpus (1004 files resolved through their "extends" chains, not
        // grepped), 633 arm carry_invariant and none reach this branch: 526 carry a
        // pre-fit detector and 107 rely on the D5 gate alone.  The invariant holds by
        // convention across every config rather than by construction; this check makes
        // a missing response impossible to reach silently.
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
            // A response is present, but on a q_z = 0 carry it is the pre-fit detectors
            // only.  Two gaps remain and are named before a grading run: a slip the raw
            // detectors miss has no second line of defence, and a model-error excursion
            // (the target of D5, not of the detectors) has none at all.  This is the
            // posture the settled-trio arm of SPEC_synthetic_minimum_network §13.7 was
            // measured on.
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
        // PD-safe DD fold (card 4, M1): every mechanism it changes (the fold's middle
        // inverse, the posterior read-back, the degenerate-block rebirth, the restore
        // guard) lives inside the invariant carry.  Without carry_invariant there is no
        // DD-prior fold, so the knob is inert, not wrong: it warns and disarms rather
        // than rejecting (a config that carries it for a later leg is not a config
        // error).  Placed in this cross-knob pass, not the parse block, per the note
        // there.
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
        // priors' rank-K term, so without carry_invariant there is nothing to correct;
        // the knob is inert, not wrong.  Same warn-and-disarm as the PD-safe knob above,
        // for the same reason, in the same cross-knob pass.
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
        // The LS-VCE calibration (card 4, M1, D5 v2) only re-scales the gate's threshold.
        if (network_carry_invariant_innov_calibrate_ && !network_carry_invariant_innov_gate_) {
            log->error("Network: network.carry_invariant_innov_calibrate requires "
                       "network.carry_invariant_innov_gate — it standardizes the gate's innovation by "
                       "the empirical σ̂₀²; with the gate off there is nothing to calibrate.  Enable "
                       "the innovation gate or disable the calibration.");
            return false;
        }

        return true;
    }

    // =================================================================
    //  network subsection helper: validateNetworkLaneLayers_
    // =================================================================
    /**
     * @brief The SD pre-reduce EWL constraint's inert check (@c network.sd_ewl.enable).
     *
     * Split from @c loadNetworkLaneLayers_ because the check needs the slip-detector
     * flags (phase_leveling, mw_detect, turbo_edit), which are parsed after the lane
     * block.  The MW/WL lane contracts that also lived here were retired with their
     * keys (cards WL-1, WL-2, WL-3 and UWL-1).
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     * @return @c false on a fatal misconfiguration (aborts @c loadFromJson); this
     *         check only warns.
     */
    bool GnssConfig::validateNetworkLaneLayers_(const nlohmann::json& json,
                                                const std::shared_ptr<spdlog::logger>& log) {
        // ---- SD pre-reduce EWL: the statistic source must exist. ----
        // NetStation::mw_slip is constructed only for non-pivot stations and only
        // when network.phase_leveling && (cycle_slip.mw_detect || turbo_edit).
        // Without it arcZdMean() is unreachable at every station and the feature is
        // silently inert, the armed-but-dead case invariant #4 exists to catch, so
        // the loader warns.
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

        return true;
    }

    // =================================================================
    //  network subsection helper: validateNetworkPhaseDatum_
    // =================================================================
    /**
     * @brief Phase-bias gauge selection report (eq34 / prioritized) and its mutexes.
     *
     * One if / else-if chain over the two realizations, so exactly one is reported; also
     * carries the eq34-under-carry_invariant inert warnings.
     *
     * @param log   The "gnss_config" logger.
     * @return @c false on a fatal misconfiguration (aborts @c loadFromJson).
     */
    bool GnssConfig::validateNetworkPhaseDatum_(
                                   const std::shared_ptr<spdlog::logger>& log) {
        if (network_phase_datum_eq34_) {
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
            // The seed-round INC-2 re-host gets its own line rather than a widened summary
            // string: an off run's log (and the run-summary hash the V1 byte-identity rung
            // reads) must stay unchanged by this knob.
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
                          "network.phase_datum_prioritized — the anchor "
                          "election is replaced by the chain election.");
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
                          "is a non-event by construction.  emit_rebase={}.",
                          network_carry_invariant_qz_,
                          network_carry_invariant_emit_rebase_ ? "true" : "false");
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
            }
            // Inert warnings (spec D7): the invariant carry subsumes the eq34
            // frame-continuity hygiene (the datum targets are plain 0 and continuity lives
            // in the carried DDs), so pin_round / carry_shift / rpb_q are gated off at their
            // use sites when the carry is armed.
            if (network_phase_datum_eq34_pin_round_)
                log->warn("Network: network.phase_datum_eq34_pin_round is INERT under "
                          "carry_invariant (per-epoch datum targets are 0; DD continuity is the carry).");
            if (network_phase_datum_eq34_carry_shift_)
                log->warn("Network: network.phase_datum_eq34_carry_shift is INERT under "
                          "carry_invariant (no carry S-transform — the datum is re-realized freely).");
            if (network_phase_datum_eq34_rpb_q_m2s_ > 0.0)
                log->warn("Network: network.phase_datum_eq34_rpb_q_m2s is INERT under carry_invariant "
                          "(the RPB re-estimates fresh; the DD carry is the phase time-link).");
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

        // Two-edge continuous handover: inert guards, then the arming line.  Follows the
        // eq34-hygiene / pool-re-host precedent: an arm that cannot change anything is
        // disabled here with the reason, rather than discovered after a field run
        // (invariant #4).
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
        // The σ-scale ratio ceiling uses the same mechanism, so it inherits the same inert
        // ladder: no continuous handover ⇒ no ledger value is ever tested.
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
        // Cold anchor fallback (SPEC_event_harness §15): the same inert ladder.  With no
        // continuous handover there is no ledger to screen candidates against, so the
        // candidacy pass has nothing to read and would elect by coverage rank as it does
        // without the knob.  The warning says so, so an armed knob does not read as active.
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
        // Re-admission continuity (E2 seed-first re-admission and the block re-coupling edge).
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
        // Armed line for card CPA.  There is no inert ladder here: the rule has no
        // prerequisite knob, and the one way it can be inert (no station latched a
        // canonical code+phase pair on band digits 1 and 2) is a property of the data,
        // which only the run-end counters can report.
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
        // Carry re-reference at the anchor handover (queue item 1(d)): the same inert
        // guard ladder as the handover knob above, for the same reasons (no anchors ⇒ no
        // anchor handovers ⇒ nothing to re-reference).  Unlike the ladder above, the
        // eq34 guard comes first on purpose: an eq34-only config never sets
        // phase_datum_prioritized, so a prioritized-first ladder would disarm with the
        // wrong "legacy datum elects no anchor" diagnostic (eq34 elects a chain datum)
        // and the accurate message would be unreachable.
        if (network_phase_anchor_carry_reref_ && network_phase_datum_eq34_) {
            log->warn("Network (phase-anchor carry reref): "
                      "network.phase_anchor_carry_reref is on with the eq34 CHAIN datum, "
                      "which already owns the eq34_reref carry hygiene this knob mirrors.  "
                      "DISABLED — arming both would double-treat the same edge.");
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
        // The continuous handover composes with the reref through the R2 sticky target
        // (SPEC §5.09).  With a one-epoch-deferred ledger pin the frame step would land
        // at T+1 (§5.07.3) while the reref mark covers T, so the pair would marginalize
        // the valid fold and miss the stale one.  The R2 sticky target removes the T+1
        // revert (a continuous handover realizes the inherited frame for the successor's
        // whole tenure), and the election marks only cold re-realizations, so each
        // mechanism treats exactly its own edge: continuous handovers keep frame and
        // carries; cold handovers mark, marginalize and erase as validated in §5.08.3.
        // This composition stops the reref's arc truncation (582 of 621 cold handovers
        // erased 10 236 leveling carries on a 12 h zero-slip fixture, SPEC §5.09).
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
        // Drop edge of the coverage gate (card EV7 §6).  Two hard dependencies: the gate
        // has to be on for an edge to exist, and the erase uses the re-reference set,
        // which only phase_anchor_carry_reref populates.  Each inert case is reported
        // rather than the knob being flipped off silently, so an arm that named the
        // knob and got nothing is visible in the log.  The edges are detected and
        // counted in every posture, so an off (or inert) run still reports the class
        // size.
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
        // Armed line for card UPO.  There is no inert case (the reduction is
        // unconditional at setup and reports itself per station and in a summary), so a
        // single info line naming the frame change tells a reader of the config log that
        // this run's roster is not at the published coordinates.
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
        // Armed line for the ledger's trace switch.  Trace class: the roster, the ledger
        // and their census are built on every run whatever the key says (card NOM step
        // N3a), so the line names only the files this run writes in addition.
        if (network_signal_roster_)
            log->info("Network producer (card SID phase 1): network.signal_roster is ON: the "
                      "ARC LEDGER's trace is written, logs/network_arc_ledger.csv (one row per "
                      "arc transition) and the ARC event rows of the roster's build and audit in "
                      "network_events.csv (holdoff-throttled).  The signal roster, the ledger "
                      "and their disagreement census are built on every run whatever this key "
                      "says (card NOM step N3a), so the solve and the wire are unchanged by it.");
        // SID phase 2 (the tracker reads the roster) needs no other knob: the roster it
        // reads is built on every run (card NOM step N3a), so the refusal that guarded an
        // unbuilt roster is gone.
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
        // SID phase 3 (the response keys on the ledger) is refused without the roster
        // read: the tracker's rows would otherwise name a band by a different rule than
        // the population does, reintroducing card GRK's defect one layer up.  The arc
        // ledger it reads is built on every run (card NOM step N3a).
        if (network_ledger_reentry_ && !network_roster_tracker_) {
            log->error("Network producer (card SID phase 3): network.ledger_reentry is armed "
                       "WITHOUT network.roster_tracker, so there is no roster read behind the "
                       "tracker's band names.  DISARMED.  Arm the phase-2 roster read "
                       "alongside it.");
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
        // SID phase 4a (the reason-mask writers and the anchor observatory) needs no
        // other knob: the ledger it folds into is built on every run (card NOM step N3a).
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
        // SID phase 4b (the election acts on the read) is refused without 4a, whose
        // buffer carries the drop-edge memory and the generation test the restore
        // decides on.
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
        // SID phase 5a (the emitter read and the declared roster) needs no other knob:
        // its two inputs, the arc ledger and the roster the declaration is built from,
        // exist on every run (card NOM step N3a).  An epoch whose roster has no modelled
        // role still yields no declaration (networkLedgerEmitBuildDeclaration refuses an
        // empty one).
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

        return true;
    }

    // =================================================================
    //  network subsection helper: loadNetworkContractHardening_
    // =================================================================
    /**
     * @brief Product wire and S-contract hardening for late joiners and restarts (RTA-3/4/5/11).
     *
     * @c network.emit_generation_id, @c serialize_disc_counters, @c contract_rebroadcast_s,
     * @c emitter_staleness_horizon_s.  All default off => store and file byte-identical.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkContractHardening_(const nlohmann::json& json,
                                                   const std::shared_ptr<spdlog::logger>& log) {
        // Product wire/contract hardening for late joiners and producer restarts
        // (RTA-3/RTA-4/RTA-5/RTA-11).  All default off ⇒ store and file byte-identical.
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
        // The S contract is part of every wire, so a re-broadcast always has a contract to
        // repeat (card OWD, O1).
        if (network_contract_rebroadcast_s_ > 0.0)
            log->info("Network producer (RTA-5): S-contract re-broadcast ON — a CURRENT S line "
                      "(refreshed realized pivots) every {:.0f} s of product time and on any "
                      "datum-change emit.", network_contract_rebroadcast_s_);
    }

    // =================================================================
    //  network subsection helper: applyNetworkServiceProfile_
    // =================================================================
    /**
     * @brief Curated service profile (RTB-8) and the post-arming re-validation (P3).
     *
     * Runs last of the arming steps, so it sees final values for every toggle it bundles.
     * The bundle wins: an armed bool is forced true even over an explicit user @c false, and
     * each such override is warned individually.  The two parse-site validations the arming can
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
        // section, and never again, so this is the value the unsplit code saw.
        const bool service_mode = network_service_mode_;

        // --- Curated service profile (RTB-8) -------------------------------------
        // One switch (network.service_mode) arms the coherent set of producer toggles a
        // live PPP-RTK service needs, instead of hand-assembling ~20 interlocking
        // default-off flags, each with implicit couplings.  The bundle wins: a bool the
        // profile arms is forced true even over an explicit user `false`, and a numeric
        // at 0 (the default, an explicit 0, or a negative clamped to 0 above) is armed to
        // its service value; to keep a specific knob off, run without service_mode.
        // Every override of a value the user wrote (presence-checked on both the dotted
        // and the flat legacy key form, the enable_ssr_bias_max_age_explicit_ pattern) is
        // warned individually (P1); a user value that already equals the bundle's choice
        // is not an override and stays silent.  Placed after every armed toggle is parsed
        // so it sees final values; the two parse-site validations it can bypass
        // (withhold clamp, clock_model restore) are re-run just below (P3).  Default off
        // ⇒ nothing armed (byte-identical).
        // Not armed, on purpose (see the do-not-arm notes below the arms):
        //   - network.state_snapshot_path: needs a user-chosen path; a missing one gets
        //     a recommendation info line instead.
        //   - network.warmup_ura_floor_m: policy numeric with no universal default; the
        //     RTB-7 no-op warning above covers a misconfiguration.
        //   - network.clock_runaway_* ceilings: numeric policy, default-off quarantine;
        //     scope-limiting a runaway is an operator decision.
        //   - network.apriori_source (RTF-1): an input-source selection, not a toggle,
        //     and orthogonal to the bundle: a service can run on sp3 ultra-rapids (RTB-4)
        //     or brdc_rts equally.  Left exactly as set.
        // The S contract itself (RTA-5 re-broadcast + RTA-12 identity fields +
        // checkContract), which this profile turns on, cross-validates the realized
        // product against the consumer.  service_mode is read at the top of the section
        // so the coherence warnings can be suppressed under it.
        if (service_mode) {
            // Presence check (P1): did the user write this key, in either form?
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
            armBool(network_phase_leveling_,                // cross-epoch carry: the carry_warm prerequisite of both RAIM tiers
                    "network.phase_leveling", "phase_leveling");
            armBool(network_raim_,                          // post-fit RAIM: the σ̂0 EWMA that scales the emitted URA is fed by this base flag (not within_epoch), and the raim_within_epoch auto-coupling ran before this block, so it is armed explicitly
                    "network.raim", "network_raim");
            armBool(network_raim_attribution_,              // complementary post-fit attribution refloat tier (the two halves of one backstop)
                    "network.raim_attribution", "network_raim_attribution");
            armBool(network_raim_within_epoch_,             // fault detect → drop-culprit → re-solve
                    "network.raim_within_epoch", "network_raim_within_epoch");
            armDouble(network_raim_withhold_sigma0_,        // withhold a still-contaminated epoch at ~2× the trigger (design guidance); re-validated below (P3)
                      2.0 * network_raim_sigma0_trigger_,
                      "network.raim_withhold_sigma0", "network_raim_withhold_sigma0");
            armBool(network_per_const_withhold_,            // the bundle injects the withhold ceiling above, so the parse-site "no safe ceiling" reason for not arming the carve-out does not apply: degrade scope, not availability (P2/RTA-10)
                    "network.per_const_withhold", "network_per_const_withhold");
            armBool(network_emit_epoch_status_record_,      // H heartbeat: a service's live tail must tell "withheld" from "producer died" (P2/RTA-10)
                    "network.emit_epoch_status_record", "network_emit_epoch_status_record");
            // Pivot-selection and handover-signalling machinery is provably inert under
            // the anchor-free sum-to-zero and broadcast-anchored clock datums (no pivot
            // exists; pivotHandoverSignalled always returns false), so these knobs are
            // not force-armed there: leaving them false keeps the operator from being
            // told that pivot machinery is active, and keeps the "INERT under ..."
            // warnings above consistent with the realized flags.  An explicit user
            // setting still stands and is warned above.
            if (!network_clock_datum_sum_to_zero_
                && !network_clock_datum_broadcast_anchored_) {
                armBool(network_pivot_vanish_return_signal_,    // without it a vanish-return pivot change is an unsignalled datum break that emit_clock_disc cannot cover (P2/A1)
                        "network.pivot_vanish_return_signal", "network_pivot_vanish_return_signal");
                armBool(network_sticky_pivot_,                  // minimize the handover rate (datum re-references the consumer pays for; P2/RTA-1)
                        "network.sticky_pivot", "network_sticky_pivot");
            }
            armBool(network_scoped_handover_response_,      // limit a handover's internal effects to the affected constellation(s) (P2/RTA-2)
                    "network.scoped_handover_response", "network_scoped_handover_response");
            armBool(network_emit_generation_id_,            // restart / datum-break detection (RTA-3)
                    "network.emit_generation_id", "network_emit_generation_id");
            armBool(network_serialize_disc_counters_,       // late-joiner counter recovery (RTA-4)
                    "network.serialize_disc_counters", "network_serialize_disc_counters");
            armDouble(network_contract_rebroadcast_s_, 30.0,      // periodic re-broadcast (RTA-5)
                      "network.contract_rebroadcast_s", "network_contract_rebroadcast_s");
            armBool(network_config_.product_flush_per_epoch,      // live-tail visibility (RTA-6)
                    "network.product_flush_per_epoch", "product_flush_per_epoch");
            armDouble(network_config_.health_interval_s, 30.0,    // operator heartbeat (RTB-6)
                      "network.health_interval_s", "health_interval_s");
            armDouble(network_config_.trace_rotate_mb, 256.0,     // a 24/7 service must not grow its CSVs without bound; keep stays at its parsed default 3 (P2/RTB-6)
                      "network.trace_rotate_mb", "trace_rotate_mb");
            // Every file-side knob the bundle just armed (per-epoch flush, disc-counter
            // serialization, v2 S contract + re-broadcast, generation id, H heartbeat)
            // writes into the product file; with no product_output_path there is no file
            // and all of them are inert (P4(b)).  The parse-site RTA-6 flush warning
            // cannot catch this (it ran before the arming), so it is reported here, once,
            // listing the inert knobs.
            if (network_config_.product_output_path.empty())
                log->warn("Network (RTB-8): service_mode is ON but network.product_output_path "
                          "is empty — no product file is written, so the bundle's file-side "
                          "knobs (per-epoch flush, disc-counter serialization, v2 S contract + "
                          "{:.0f} s re-broadcast, generation id, epoch-status heartbeat) are "
                          "ALL inert.  Set network.product_output_path.",
                          network_contract_rebroadcast_s_);
            // Not armed (it needs a user-chosen path), but a 24/7 service without a
            // snapshot cold-starts a new generation on every restart, so it is recommended.
            if (network_config_.state_snapshot_path.empty())
                log->info("Network (RTB-8): service_mode is on with no "
                          "network.state_snapshot_path — consider setting one so a producer "
                          "restart WARM-resumes from the RTB-7 snapshot (generation id "
                          "preserved, no consumer-wide ambiguity reset) instead of "
                          "cold-starting a new generation.");
        }

        // --- Post-arming re-validation (P3) --------------------------------------
        // Two parse-site checks earlier in this section run before the service arming
        // block and would be bypassed by service-armed values:
        //   (a) the pathological σ̂0-withhold clamp (R11/R1): the bundle arms the
        //       withhold at 2× network.raim_sigma0_trigger, which for a small explicit
        //       trigger (<= 0.5) lands in the pathological (0,1] band the parse-site
        //       clamp exists to reject (σ̂0 is floored at 1 ⇒ near-total loss of
        //       availability);
        //   (b) the clock_model_* validity restore, gated on the pre-arming
        //       network_clock_model_: service_mode arms the oscillator, so a stray
        //       invalid clock_model_q2 would otherwise run unvalidated.
        // The parse-site originals are kept (moving them here would change non-service
        // semantics: the within-epoch auto-coupling reads the clamped withhold, and the
        // R5 ura_staleness_q2 default reads the restored q2; both sit between the two
        // sites).  These re-runs are idempotent for non-service runs: they re-apply the
        // exact parse-site conditions, which the parse-site pass already discharged (only
        // the arming block writes these values in between), so they fire, with their one
        // warning, only when the arming itself introduced the invalid value.
        //
        // (c) Card Q3 × network.clock_model.  Under the two-state oscillator the
        //     satellite clock is carried by clock_osc and is not stashed to the D2 carry
        //     at all (networkStashCarry's `if (!clk_model_on)`), so the temporal anchor
        //     has nothing to fold: it would be silently inert while its armed banner
        //     claimed otherwise.  This check has to run after the arming: service_mode
        //     force-arms the oscillator, and defect #2 of card C1 is a clock-knob
        //     interaction logged from the pre-arming value.
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
        // Re-read the R5 ZOH clock-staleness q² after the service-armed clock_model_q2
        // restore above (m8).  The parse-site default (in
        // loadNetworkClockModelAndChannels_) read the pre-arming clock_model_q2; under
        // service_mode a stray invalid clock_model_q2 was left unvalidated there (the
        // parse-site validity restore is gated on the pre-arming clock_model flag), and
        // the negative guard then zeroed the derived ura_staleness_q2, so a service run
        // that armed the oscillator would carry staleness inflation off despite the
        // model being on.  Re-running the same read picks up the restored q2.  Gated on
        // service_mode so the non-service path is byte-identical (an explicit user value
        // still wins through jval, as at the parse site).
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

        // The armed service profile is announced after the P3 re-validation above, so the
        // logged withhold is the post-clamp value (m9).  Armed at 2× a small explicit
        // trigger, the withhold can land in the pathological (0,1] band and be disabled
        // by the clamp just above; a log line inside the arming block would print the
        // pre-clamp ceiling one line before the clamp retracts it.
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
     * @c network.emitter_staleness_horizon_s and @c start_offset_hours, then the armed-state
     * reports for phase leveling, the rx-CB and ZWD carries, IF-phase TD detection and post-fit
     * RAIM, each with the inert warning for its missing prerequisite.
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
        // Inert guard for F1: the scale acts on the phase-leveling carry rows at their
        // inject site, which exists only when that carry is the live vehicle
        // (phase_leveling on and carry_invariant off; card 4's DD carry supersedes it and
        // `do_amb_carry` skips the inject outright).  network_carry_invariant_ is
        // populated by this point (the M2 v2 guard below relies on the same).
        if (network_phase_leveling_los_scale_
            && (!network_phase_leveling_ || network_carry_invariant_))
            log->warn("Network: network.phase_leveling_los_scale is on but the phase-leveling "
                      "ambiguity carry is NOT folded on this lineage (phase_leveling={}, "
                      "carry_invariant={}) — F1 scales those carry rows at inject, so there is "
                      "nothing to scale: this knob is INERT on this posture (standing invariant "
                      "#4).  Arm it on a phase_leveling, non-carry_invariant lineage.",
                      network_phase_leveling_, network_carry_invariant_);
        // Inert guard for F3: the block is built at the same inject site as F1 (so the
        // same phase_leveling / carry_invariant conditions apply), and its rows live in
        // A_s/A_r as card-1 coupling rows, so it needs carry_conditional_coupling; the
        // card-1b joint departed-column whitening owns that station's row block in this
        // build (mutually exclusive: the block path is predicated on !carry_cpl_joint at
        // the inject).  All four members are populated by here.
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
        // ---- Ambiguity-vehicle interlocks (RF-1;
        //      docs/refound/RF-1_float_ambiguities_design.md §7.1).  Placed here because
        //      network_carry_invariant_ is populated only by loadNetworkDatumSignalling_,
        //      which runs after the carries.
        //
        // One vehicle, two beds (the re-founding decision): the field golden's carry
        // (carry_invariant) and the sim golden's (leveling) had diverged without being
        // re-validated as a whole, and the subject of RF-1 is the leveling carry's fold,
        // so float_kf on a lineage that does not fold it is not a degraded run but a
        // meaningless one.  A hard error rather than a warning, because an inert arm
        // reads exactly like a clean one (invariant #4).
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
            // The F1 scale (×B_eff) and the F3 station-wide whitened block are the
            // mechanisms float_kf subsumes: the joint block removes the per-LOS iono
            // over-count exactly where F1 corrects it approximately, and F3's
            // buildLocalBlock is the fold that float_kf makes structural (design §3.2 /
            // §4.1).  Leaving either armed beside the vehicle would make the effective fold
            // unattributable between two knobs, so both are refused with a log line rather
            // than silently ignored.
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
            // ---- The block's own prerequisites (RF-1 step 3).  The vehicle promises the
            //      joint fold, so a posture where that fold cannot be built is not a
            //      degraded arm but a silent one: the inject would take the per-column
            //      route on every station-epoch, and the run would read as a clean
            //      float_kf arm while carrying the over-count (invariant #4).  The two
            //      conditions are the live predicate the inject applies:
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
            // The young-arc inflate is a time-decaying patch on the inject for the defect
            // that float_kf's floored birth prior and bounded drop fix at the source
            // (design §4.1).  It is not refused (it is a validated mechanism, and an arm
            // may want the contrast on purpose), but an unnoticed carry-over from an RF-0
            // config would confound the birth-floor calibration, so it is named.
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
        // ---- Interlocks of the held S-basis (RF-2).  It is a statement about the
        //      ambiguity that is the phase datum, so it is meaningless on a vehicle where
        //      the other ambiguities are not float states: the level vehicle's frozen
        //      zero-q anchor, which the held basis replaces, is still in force there.
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
        // The vehicle changes the solve from step 3 on.  The line lists what is live on
        // this posture rather than naming a build number, so a reader of a run manifest
        // cannot infer a mechanism the arm did not carry ("float_kf" alone does not say
        // what ran).
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
                || network_config_.amb_float_declared_handover))
            log->warn("Network (RF-1 §7.1): amb_float_* knobs are set (birth_var_m2={}, "
                      "declared_handover={}) but network.ambiguity_vehicle is "
                      "\"level\" — they are INERT on this posture (standing invariant #4).",
                      network_config_.amb_float_birth_var_m2,
                      network_config_.amb_float_declared_handover);
        // Post-parse inert guard for the `pl` apply family (M2 v2;
        // docs/SPEC_carry_vce_apply.md); network_carry_invariant_ is populated by this
        // point.
        //
        // The phase-leveling ambiguity carry is folded only when carry_warm (which
        // requires network.phase_leveling) and not carry_invariant: card 4's DD carry
        // supersedes it as a solve input, so ApplicationNetworkAssembly's `do_amb_carry`
        // skips the inject outright and R.pl_amb stays empty, which means the monitor
        // sees no pl rows either.  On such a lineage an apply armed on `pl` cannot
        // change anything.  The run-end summary would say so ("NO GROUPS" / "NEVER
        // STEPPED"), but only after a whole field run, so the check runs at load.
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
        // rx-CB carry (SPEC_rx_cb_carry): post-parse validation; code_datum_time_linked
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
        // RXCB-IN-BLOCK (card CTU section 14): interlocks, after the carry's own guard.
        // Every refusal is a hard error: an armed knob that silently rides the diagonal
        // fold would measure the very double count it exists to remove.
        if (network_carry_rx_code_bias_in_block_) {
            const bool float_kf =
                network_config_.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf;
            if (!network_carry_rx_code_bias_) {
                log->error("Network: network.carry_rx_code_bias_in_block requires "
                           "network.carry_rx_code_bias (armed, with code_datum_time_linked).");
                return false;
            }
            if (!network_carry_conditional_coupling_ || network_carry_coupling_joint_whiten_) {
                log->error("Network: network.carry_rx_code_bias_in_block requires "
                           "network.carry_conditional_coupling and refuses "
                           "network.carry_coupling_joint_whiten (the block rows are the "
                           "card 1 coupling rows, whitened per station).");
                return false;
            }
            if (!float_kf && !network_phase_leveling_local_block_) {
                log->error("Network: network.carry_rx_code_bias_in_block needs a local "
                           "block: network.ambiguity_vehicle float_kf, or "
                           "network.phase_leveling_local_block.");
                return false;
            }
            if (network_rx_cb_carry_demean_ || network_rx_cb_carry_demean_within_
                || network_rx_cb_carry_step_gate_ || network_rx_cb_harvest_demean_) {
                log->error("Network: network.carry_rx_code_bias_in_block refuses the LV-2 / "
                           "LV-5 de-mean, the LV-4 step gate and the S2H harvest de-mean: "
                           "all four act on the diagonal fold or its standalone harvest, "
                           "which this knob replaces.");
                return false;
            }
            log->info("Network (RXCB-IN-BLOCK): receiver code-bias carry folded INSIDE each "
                      "station's whitened local block as conditional coupling rows (border "
                      "regression + cross covariances to the carried ambiguities), not as the "
                      "diagonal prior at the conditional variance.  No diagonal fallback: "
                      "before the leveling carry is warm and on a non-SPD block the columns "
                      "carry nothing (counted).");
        }
        // The per-line-of-sight phase nuisance state (card LGM section 4.1): its carry is a
        // column of the float vehicle's whitened local block, built from the card 1
        // conditional coupling rows.
        if (network_config_.phase_colour_state) {
            const bool float_kf =
                network_config_.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf;
            if (!float_kf || !network_phase_leveling_ || network_carry_invariant_
                || !network_carry_conditional_coupling_ || network_carry_coupling_joint_whiten_) {
                log->error("Network (card LGM): network.phase_colour_state requires "
                           "network.ambiguity_vehicle float_kf, network.phase_leveling and "
                           "network.carry_conditional_coupling, and refuses "
                           "network.carry_invariant and network.carry_coupling_joint_whiten "
                           "(the state's carry is a column of the float vehicle's whitened "
                           "local block).");
                return false;
            }
            log->info("Network (card LGM): per-line-of-sight phase nuisance state ON: one "
                      "band-common, phase-only Gauss-Markov state per (station, satellite), "
                      "sigma_zen {:g} m, tau {:g} s, elevation floor {:g} deg, carried in the "
                      "station's whitened local block (phi = exp(-gap/tau)), born at its "
                      "stationary prior after a gap above {:g} s or an erase.  SOLVE-AFFECTING.",
                      network_config_.phase_colour_sigma_zen_m, network_config_.phase_colour_tau_s,
                      network_config_.phase_colour_elev_floor_deg,
                      network_config_.phase_colour_max_gap_s);
        } else if (network_config_.trace_phase_colour) {
            log->warn("Network (card LGM): network.trace_phase_colour is set without "
                      "network.phase_colour_state; the trace is inert.");
        }
        // The two interlocks of the second code channel (card SCC, R6).  Checked here,
        // after the rx-CB carry block, because both inputs are final only at this point:
        // the carry itself can still be forced off above, and channel_mode belongs to the
        // processing section.  A refused interlock disarms the knob rather than failing
        // the load: the posture is then exactly the unarmed one, which is a valid run.
        if (network_config_.second_code_channel) {
            if (channel_mode_ == ChannelMode::All) {
                // Subsumption, not refusal (card SID phase 6b, R1).  The all-channel
                // widening adds every admitted code of every selected band, a superset of
                // card SCC's allow-listed band-1 set, and phase 6b routes every one of
                // those rows through the receiver inter-signal state card SCC created.
                // So the knob stays armed and its allow-list is ignored; only its sigma
                // scale still applies, now over every non-canonical code row rather than
                // over the listed ones.
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
        // The three interlocks of the second phase channel (card SID phase 6a, R6).
        // Checked beside card SCC's, after the vehicle and the datum knobs are final.  A
        // refused interlock disarms the knob rather than failing the load: the posture is
        // then exactly the unarmed one, which is a valid run.
        if (network_config_.second_phase_channel) {
            if (channel_mode_ == ChannelMode::All) {
                // Subsumption, not refusal (card SID phase 6b, R1), for the same reason as
                // card SCC above.  The widening's phase half is a superset of the
                // allow-listed band-1 set of card SID phase 6a, and the added rows are
                // modelled identically (an ordinary float arc in the existing satellite
                // phase-bias family), so the allow-list is ignored and only the sigma scale
                // still applies, over every non-canonical phase row.
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
        // `processing.channel_mode: all` on the network path (card SID phase 6b, R7).
        // Checked here, beside cards SCC and SID phase 6a, because it subsumes both and
        // because the vehicle and the datum knobs are final at this point.  A refused
        // interlock clamps the mode back to 'single' rather than failing the load: the
        // posture is then the historical one, which is a valid run.
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
        // The S-consistent harvest (card S2H; docs/SPEC_level_information_budget.md §17
        // item 3 / §27 item 1).  Validated before the LV-2/LV-5 guards because it
        // supersedes them: both mechanisms act on the same channel (the carried rx-CB
        // common mode), and composing them would de-mean a set that is already zero-mean
        // by construction; the LV-2 μ would measure numerical noise, and its fold-side
        // subtraction would reintroduce the lag this harvest removes.  Same placement
        // after the carry guard as the LV cards.
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
            // The vote floor is the shared LV knob; LV-2's own >= 1 validation runs only
            // when LV-2 is armed, so it is repeated here for the S2H-only posture.
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
        // rx-CB carry common-mode de-mean (card LV-2; docs/SPEC_level_information_budget.md).
        // Validated after the carry's own guard above, which may have forced the carry
        // off: a de-mean of a disarmed carry is inert, not armed.
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
        // Within-epoch μ (card LV-5; docs/SPEC_level_information_budget.md §12).
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
        // Invariant carry screen (card LV-4; docs/SPEC_level_information_budget.md §11).
        // Same placement after the carry guard as LV-2: a screen on a disarmed carry is
        // inert, not armed.
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
                // Measured pairing guards (docs/SPEC_level_information_budget.md §11.3,
                // §12.3).  This screen roughly triples how much carry information folds
                // (43 % → 97 % on the DOY-129 EXT-CB arm).  That is its purpose, but it
                // also triples the loop gain of whatever handles the carry's common mode,
                // and both failure modes below were measured.
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
        // Bias-free per-LOS iono seeding (card LV-1; docs/SPEC_level_information_budget.md).
        // Validated here because estimate_iono_sh and the SH block are populated by this
        // point.
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
        // Interlocks for B9-in-block.  Armed-and-inert is not allowed (invariant #4: a
        // run-end "0 ZWD columns" must mean the mechanism found nothing, never that the
        // knob could not act), so a miswired arm is disabled here rather than left inert.
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
                // The F3 block is armed but cannot be built on this posture (the guard above
                // already warned about F3 itself).  Riding an inert vehicle is the same
                // armed-and-inert failure, so this knob is disabled with it rather than
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
            // ZWD on the diagonal vehicle.  Log-only, no behavior change (a promotion of the
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
        // Interlocks for I-in-block (network.carry_iono_in_block): the ZWD rules plus one
        // addition that is a hard error rather than a warning.  The legacy diagonal
        // network.carry_iono is not a different setting of the same mechanism; it is the
        // over-count this knob removes (QUEUE 8b, falsified ×4).  Running both would
        // assert each LOS's iono direction three times per epoch (B band priors + the
        // diagonal row + the block row), and no run-end counter could tell the two
        // vehicles apart, so the combination is refused at load.
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
        // The max-gap sentinel is resolved before the disable branches, so the getter
        // always reports effective seconds, even on a disabled arm.
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
                // The F3 block is armed but cannot be built on this posture.  Riding an
                // inert vehicle is the same armed-and-inert failure the ZWD interlock
                // refuses (invariant #4), so this knob is disabled with it.
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
        // Interlocks for the age-aware young-arc inflate
        // (network.phase_leveling_young_inflate_var_m2).  The knob acts at the
        // phase-leveling carry's inject site, so it needs that carry to be the live
        // vehicle: phase_leveling on, carry_invariant off (card 4's DD carry supersedes
        // the leveling inject outright: `do_amb_carry` is false and there is no column to
        // inflate).  F3 is not required, on purpose: in-block the addition is a
        // congruence scaling of Σ, out of block a plain var_eff += var_add on the
        // per-column row, and both are wired.
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
        // Interlocks for RX-CLK-in-block (network.carry_rx_clock_in_block): the ZWD rules
        // plus hard errors on the variance ladder.  Each of q, rate_var0 and rate_q lands
        // on a Cholesky diagonal, and a negative one would make a non-SPD block look like
        // a data problem several seams away from the typo that caused it.
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
        // The max-gap sentinel is resolved before the disable branches, so the getter
        // always reports effective seconds, even on a disabled arm.
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
                // The F3 block is armed but cannot be built on this posture.  Riding an
                // inert vehicle is the same armed-and-inert failure the ZWD and iono
                // interlocks refuse (invariant #4), so this knob is disabled with them.
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
            // The seam q may be the window's own, decoupled from the fingerprinted
            // continuous carry q (RS-10b, §21.21).  The log reports which one is live and
            // where it came from; this is the on-path telemetry for the bracket.
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
        // Inert guards for the RAIM pool re-host (docs/SPEC_raim_pool_rehost.md).
        // Post-parse, because network_carry_invariant_ is populated only in
        // loadNetworkDatumSignalling_.  Follows the eq34-hygiene / M2-v2 precedent: an arm
        // that cannot change anything is disabled here rather than discovered after a
        // field run (invariant #4).
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
        // Card RPA: the pool restriction acts inside the RAIM pools, so without
        // network.raim (after its auto-couplings and the service profile, both above) it
        // has nothing to act on.  Disabled rather than left to read as armed (invariant #4).
        if (network_raim_pool_data_rows_only_ && !network_raim_) {
            log->warn("Network (RAIM pool, card RPA): network.raim_pool_data_rows_only is on "
                      "but network.raim is OFF: there is no RAIM pool to restrict.  DISABLED.");
            network_raim_pool_data_rows_only_ = false;
        }
        if (network_raim_pool_data_rows_only_)
            log->info("Network (RAIM pool, card RPA): network.raim_pool_data_rows_only ON: the "
                      "post-fit and within-epoch RAIM pools admit the observation phase rows "
                      "only (data_phase, data_phase_second), each to the pool of the ambiguity "
                      "column its tag names; the carried-prior and SD-EWL rows are refused.  "
                      "Solve-affecting (T3/T4 and the within-epoch adaptation read observation "
                      "residuals only); fingerprint-safe.");
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
            // `!network_carry_invariant_` is load-bearing.  ApplicationNetworkAssembly's
            // `do_amb_carry` is `(prioritized || !is_pivot) && !carry_invariant`, so under
            // carry_invariant `R.pl_amb` stays empty for every station whatever
            // phase_leveling says, and `ambPool()` then hands the seam an empty pool,
            // which skips every station.  Without this conjunct a `phase_leveling +
            // carry_invariant + !raim_carry_invariant` run would take this branch and log
            // "ENABLED" while screening nothing: T3 would report 0 opportunities / 0 flags,
            // and trace_station_residuals and trace_raim_redundancy would write header-only
            // files (the "reports ARMED and screens NOTHING" case this ladder exists to
            // catch).  The carry_invariant branch below catches that posture.
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
        // The per-station residual field is emitted only inside the post-fit RAIM cell loop
        // (gated raim_on && carry_warm; carry_warm needs phase_leveling), so arming it
        // without its runtime prerequisites gets a warning rather than a surprise empty file
        // (L13).
        if (network_config_.trace_station_residuals && !network_raim_)
            log->warn("Network: network.trace_station_residuals is on but network.raim is off — "
                      "the per-station residual field is emitted from the post-fit RAIM cell loop, "
                      "so it will be EMPTY.  Enable network.raim AND network.phase_leveling.");
        // SPEC_t3_null_derivation §7.1: same runtime prerequisite, same failure mode.
        if (network_config_.trace_raim_redundancy && !network_raim_)
            log->warn("Network: network.trace_raim_redundancy is on but network.raim is off — "
                      "the per-row redundancy census is emitted from the post-fit RAIM row loop, "
                      "so it will be EMPTY.  Enable network.raim AND network.phase_leveling.");
        // Arming the census together with raim_redundancy_normalize is not an error: the
        // census then reports the r_i the standardization already uses.  The log says so,
        // because the two knobs differ in their effect on the solve.
        if (network_config_.trace_raim_redundancy && network_raim_redundancy_normalize_)
            log->info("Network: network.trace_raim_redundancy alongside "
                      "network.raim_redundancy_normalize — the census reports the SAME r_i the "
                      "standardization consumes.  Note the census alone is solve-neutral; the "
                      "normalization is not (it drops rows at the r_i floor).");

        return true;
    }

    // =================================================================
    //  network subsection helper: logNetworkRxExtraBandDelay_
    // =================================================================
    /**
     * @brief The interlocks and the posture line of
     *        @c network.estimate_rx_extra_band_code_delay (card L5B).
     *
     * Warnings only, never a refusal (standing invariant #4: an armed knob that changes
     * nothing says so).  Whether the knob acts is decided by @c rxExtraBandDelayEffective
     * (filter/NetworkParamInventory.hpp), the predicate the rule, the snapshot fingerprint
     * and the run-end report read too.  Runs after @c validateNetworkCarryPosture_, so the
     * carry and datum knobs it names are final.  Prints nothing unless the knob is armed.
     *
     * @param log The "gnss_config" logger.
     */
    void GnssConfig::logNetworkRxExtraBandDelay_(const std::shared_ptr<spdlog::logger>& log) const {
        if (!network_config_.estimate_rx_extra_band_code_delay) return;

        // Inert without W6: the station split drops the receiver IFB columns only when the
        // satellite code biases are estimated.  This also covers network.estimate_glonass,
        // whose allocation skips every receiver IFB except the GLONASS per-slot one
        // (MeasurementModel::ensureRequiredStates): W6 refuses it
        // (loadNetworkAprioriAndGlonass_), so the knob can be effective only without it.
        if (!estimate_network_sat_code_bias_) {
            log->warn("Network (card L5B): network.estimate_rx_extra_band_code_delay is INERT "
                      "without network.estimate_sat_code_bias: the receiver IFB columns are "
                      "left out of the station block only when the satellite code biases are "
                      "estimated, so there is no drop to turn off.");
            return;
        }
        // Inert when no active constellation tracks a band outside a two-band pair under the
        // frequency mode (rxExtraBandTracked: a CDMA constellation with a third band).
        if (!rxExtraBandDelayEffective(*this)) {
            const char* mode = frequency_mode_ == FrequencyMode::Dual        ? "dual"
                             : frequency_mode_ == FrequencyMode::Triple      ? "triple"
                             : frequency_mode_ == FrequencyMode::All         ? "all"
                                                                             : "independent";
            log->warn("Network (card L5B): network.estimate_rx_extra_band_code_delay is INERT "
                      "on processing.frequency_mode {} with active systems [{}]: no active "
                      "CDMA constellation has a band outside a two-band pair under this mode, "
                      "so no station holds a receiver IFB column to estimate (the rule keeps "
                      "its drop, as unarmed).",
                      mode, detail::joinStrings(active_systems_));
            return;
        }

        // The datum of the new null direction (per constellation, extra-band code family and
        // connected component of the family's station-satellite graph, a common shift of the
        // receivers' extra-band delays against the satellites' extra-band code biases).
        const bool sum_to_zero = network_config_.sat_gf_datum == NetworkSatGfDatum::NoneMode
                              || network_config_.sat_gf_extra_band_free;
        const std::string closure = sum_to_zero
            ? fmt::format("the per-(constellation, signal) sum-to-zero rows of the satellite "
                          "code-bias datum ({}); one row per signal, so a family whose graph "
                          "splits into k components leaves k - 1 directions to the S-basis "
                          "completion or the rank gate",
                          network_config_.sat_gf_datum == NetworkSatGfDatum::NoneMode
                              ? "network.sat_gf_datum none"
                              : "network.sat_gf_extra_band_free on")
            : std::string("the per-signal assertion rows of network.sat_gf_datum (each "
                          "extra-band satellite code bias held at its broadcast target; "
                          "network.sat_gf_extra_band_free off)");
        const std::string carry = !network_carry_rx_code_bias_
            ? std::string("not carried (network.carry_rx_code_bias off): re-estimated every epoch")
            : network_carry_rx_code_bias_in_block_
                ? std::string("carried in the station's whitened block "
                              "(network.carry_rx_code_bias_in_block)")
                : std::string("carried as a diagonal prior (network.carry_rx_code_bias)");
        log->info("Network producer (card L5B): the receiver extra-band code delay (receiver IFB "
                  "outside each constellation's clock-reference pair, e.g. GPS_IFB5) is ESTIMATED "
                  "as a local column of each station's block, {}; the drop of rule rx_ifb "
                  "(network.estimate_sat_code_bias) is turned off.  An extra-band code row whose "
                  "satellite code bias is not a shared column carries that bias unmodelled and "
                  "is excluded (zero weight).  New null direction, one per (constellation, "
                  "extra-band code family) and per connected component of that family's "
                  "station-satellite graph: a common shift of the receivers' extra-band delays "
                  "against the satellites' extra-band code biases, closed on this run by {}.  "
                  "The station geometry-free level direction (ionosphere level, receiver DCB, "
                  "ambiguities) loses the closure the held delay gave it and is closed by "
                  "network.rx_code_bias_datum{}.",
                  carry, closure,
                  network_rx_code_bias_datum_
                      ? fmt::format(" (sigma {:.1f} m)", network_rx_code_bias_datum_sigma_m_)
                      : std::string(", which is OFF on this run"));
        if (!network_rx_code_bias_datum_)
            log->warn("Network (card L5B): network.rx_code_bias_datum is OFF.  With the receiver "
                      "extra-band code delay estimated, the extra-band code rows no longer close "
                      "each station's geometry-free level direction (ionosphere level, receiver "
                      "DCB, ambiguities); the receiver DCB datum is the closure this posture "
                      "relies on.  Arm network.rx_code_bias_datum.");
        else if (!network_config_.station_prior_before_pd)
            log->warn("Network (card L5B): network.station_prior_before_pd is OFF.  The receiver "
                      "DCB datum is the only closure of each station's geometry-free level "
                      "direction on this posture, and without the fix its rows fold after the "
                      "station's positive-definite test, which then sees that direction as a "
                      "near-null first.  Arm network.station_prior_before_pd.");
        // The diagonal route's per-key tools act on the state key, and one key
        // ("GPS_IFB5") spans every extra-band code family of the constellation (C5Q and C5X
        // stations), whose levels two independent satellite-side datums realize: a
        // per-key mean or step population mixes the two frames.  The in-block route refuses
        // these tools already (validateNetworkCarryPosture_).
        if (network_carry_rx_code_bias_ && !network_carry_rx_code_bias_in_block_
            && (network_rx_cb_carry_demean_ || network_rx_cb_carry_demean_within_
                || network_rx_cb_carry_step_gate_ || network_rx_cb_harvest_demean_))
            log->warn("Network (card L5B): the LV-2 / LV-5 de-mean, the LV-4 step gate or the "
                      "S2H harvest de-mean is armed on the diagonal receiver code-bias carry.  "
                      "Each acts on one population per state key, and a receiver IFB key "
                      "(GPS_IFB5) spans the stations of every extra-band code family (C5Q, "
                      "C5X), whose levels independent satellite-side datum rows realize: the "
                      "mean and the step scale mix two frames.  Use "
                      "network.carry_rx_code_bias_in_block, or disarm those tools.");
    }

    // =================================================================
    //  network subsection helper: loadNetworkClosedLoop_
    // =================================================================
    /**
     * @brief Optional closed-loop integer-recovery harness (@c network.closed_loop.*).
     *
     * A held-out consumer station that is, on purpose, not in @c network.stations, plus the
     * Q5 replay path.  Inert unless @c closed_loop.enable.
     *
     * @param nw    The @c network section object (already validated non-null).
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::loadNetworkClosedLoop_(const nlohmann::json& nw,
                                            const std::shared_ptr<spdlog::logger>& log) {
        // --- Optional closed-loop integer-recovery harness (held-out consumer) ---
        // A station that is, on purpose, not in network.stations and consumes the
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
            clc.consume_combined_sigma   = cl.value("consume_combined_sigma", false);  // contract v2 (M4)
            clc.consumer_undifferenced   = cl.value("consumer_undifferenced", false);
            // (A) the declared-step ambiguity carry (derived into the consumer engine
            // as processing.ssr_phase_bias_disc_carry) and (B) the consumer snapshot
            // beside each network history copy (card CC).  Both default false ⇒
            // byte-identical; neither reaches the producer's solve or its resume
            // fingerprint.
            clc.pb_disc_carry            = cl.value("pb_disc_carry", false);
            // The presence edges of a held phase bias (card PBF; derived into the
            // consumer engine as processing.ssr_pb_hold_presence_flip).  Same contract
            // as pb_disc_carry: default false, and it never reaches the producer's solve
            // or its resume fingerprint.
            clc.pb_hold_presence_flip    = cl.value("pb_hold_presence_flip", false);
            // The consumer's band latch and its freshness response follow the
            // producer's declared roster (card SID phase 5b; derived into the consumer
            // engine as GnssConfig::consumerRosterLatch).  Same contract as
            // pb_disc_carry: default false, and it never reaches the producer's solve
            // or its resume fingerprint.  Inert on a wire that carries no declaration,
            // which the consumer detects and logs.
            clc.roster_latch             = cl.value("roster_latch", false);
            // The consumer's own channel mode, separate from the producer's
            // (card SID phase 6b, R6).  A producer at `all` does not widen the consumer
            // unless this key asks for it, so an arm of the producer's widening moves
            // only the wire.  Anything other than "all" reads as "single", the historical
            // behavior of every consumer arm.
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
            // The every-step ambiguity translation (card PBT, A; derived into the
            // consumer engine as processing.ssr_phase_bias_step_translate).  Same
            // contract as pb_disc_carry: default false ⇒ byte-identical, and it never
            // reaches the producer's solve or its resume fingerprint.
            clc.pb_step_translate        = cl.value("pb_step_translate", false);
            clc.pb_step_translate_eps_m  = cl.value("pb_step_translate_eps_m", 0.005);
            // Card CLV2: the consumer's response to a declared code-level step (derived
            // into the consumer engine as processing.ssr_level_step_inflate).  Default
            // false ⇒ byte-identical, and it never reaches the producer's solve.
            clc.level_step_inflate       = cl.value("level_step_inflate", false);
            clc.level_step_inflate_k     = cl.value("level_step_inflate_k", 1.0);
            // The consumer's ambiguity level re-validation (card ALC; derived into the
            // consumer engine as processing.amb_level_check).  Same contract as
            // pb_step_translate: default false ⇒ byte-identical, and it never reaches
            // the producer's solve or its resume fingerprint.
            clc.amb_level_check           = cl.value("amb_level_check", false);
            clc.amb_level_check_window_ep = cl.value("amb_level_check_window_ep", 20);
            clc.amb_level_check_k         = cl.value("amb_level_check_k", 4.0);
            clc.amb_level_check_mode      = cl.value("amb_level_check_mode", std::string("act"));
            clc.state_snapshot           = cl.value("state_snapshot", false);
            // Replay (Q5): consumes an already-written product file instead of the
            // live producer.  Presence alone arms the replay driver (the enable flag
            // still gates the station validation below), so a config that sets it is
            // a consumer-only run.
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
        // Sink dependency of card CST (deferred from the observability helper, which runs
        // before this one and so cannot see clc.enable).  Invariant #4: an armed
        // instrument whose subject does not exist has to say so rather than write an
        // empty file that reads as "nothing happened".
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
        // The ambiguity vehicle, logged on every run (RF-1 §7.1): it is the product's
        // provenance, so it is stated whether or not it is the default, together with
        // what the vehicle does, so a reader does not infer the mechanism from the knob
        // name alone.
        log->info("Network (RF-1): ambiguity vehicle = \"{}\"{}.  amb_float_birth_var_m2={} "
                  "m^2, amb_float_declared_handover={}, "
                  "trace_amb_state={}{}.",
                  network_config_.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf
                      ? "float_kf" : "level",
                  network_config_.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf
                      ? " — the joint block is STRUCTURAL (RF-1 step 3); solve-affecting"
                      : " — the zero-q scalar carry (today's vehicle)",
                  network_config_.amb_float_birth_var_m2,
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
     * @brief Settle the scoped process-noise table after every clamp, arming rule and restore.
     *
     * Re-reads the canonical members so the struct (and therefore the run manifest) reports
     * the rate the estimator will use, and resolves each class's provenance
     * (Legacy / Table / Derived / Default).  @see loadNetworkProcessNoise_.
     *
     * @param json  Parsed root config object.
     * @param log   The "gnss_config" logger.
     */
    void GnssConfig::finalizeNetworkProcessNoise_(const nlohmann::json& json,
                                                  const std::shared_ptr<spdlog::logger>& log) {
        // --- Finalize the scoped process-noise table (network.process_noise) ---
        // Re-reads the canonical members after every clamp, arming rule and validity
        // restore in this section (network.clock_model_q1/q2 in particular are reset
        // wholesale when the clock-model parameters fail validation), so the struct, and
        // therefore the run manifest, reports the rate the estimator will use rather than
        // the one requested.  Provenance settles here too: `jval` already let an explicit
        // legacy key beat the table-supplied default, so a class whose legacy key is
        // present reads Legacy whatever the table said.
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
                { QC::IonoCarry,         network_iono_carry_q_ },
            };
            // One row per class.  A class without a row keeps its table default and
            // source in the manifest while the estimator reads the member.
            static_assert(sizeof(finals) / sizeof(finals[0]) == kNetworkQClassCount,
                          "finals[] needs one row per NetworkQClass");
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
            // Only the non-default classes are listed, so the line reports the q's the
            // posture moved; when none moved, a single "all classes at documented
            // defaults" line is logged instead.
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
        // The one place the network-consumer couplings live (RTA-7(a)): an extraction of
        // the closed-loop derivation body that deriveClosedLoopConsumerConfig and
        // applyNetworkConsumerProfile both call, so the in-process closed-loop harness
        // and the standalone real-time consumer cannot diverge.  No coupling keys off
        // consume_bias yet; it is in the signature so a future bias-side coupling lands
        // here (see header).
        (void)consume_bias;
        // The consumer is an uncombined PPP-RTK user (single receiver), not the joint
        // network producer.
        c.processing_mode_ = ProcessingMode::PPP_RTK;
        // The network producer estimates the shared satellite states; the consumer
        // takes them from the injected SSR store, so the network-forced estimation
        // flags are turned back off to their PPP-RTK defaults.
        c.estimate_sat_clocks_       = false;
        c.estimate_sat_phase_biases_ = false;
        c.promote_sat_code_bias_     = false;
        c.estimate_network_sat_code_bias_ = false;  // producer-only switch; the consumer applies the OSB
        c.estimate_network_iono_sh_ = false;        // producer-only; the consumer estimates its own local iono
        // EXT-CB is likewise producer-only.  It is not translated into a consumer-side
        // change, on purpose: the consumer inherits enable_code_phase_bias = true (never
        // force-set false under this posture), so it applies the same external .BIA the
        // producer modelled against, which is right, because the producer emitted no OSB
        // to replace it.
        c.network_external_sat_code_bias_ = false;
        c.global_vtec_sh_.enabled   = false;
        // The consumer applies the network's satellite code biases via ssr_bias_term (not
        // gated by enable_code_phase_bias), and inherits enable_code_phase_bias = false
        // from the producer when the network estimates its own OSB, so the network OSB is
        // the only code-bias source and the .BIA never corrects twice.
        // Satellite code-bias handling depends on which clock the consumer uses.  The
        // network folds the satellite code delay into its estimable clock, so a consumer
        // using the network clock (FULL mode) must not re-estimate a separate satellite
        // code bias.  On the precise clock (BIAS-ONLY / BASELINE) there is no folded
        // delay: the consumer is an ordinary PPP-RTK user and should estimate residual
        // per-(sat, signal) code biases for signals its .BIA does not cover, or the
        // uncorrected code pulls the position estimate.
        c.estimate_sat_biases_ = !consume_clock;
        // The network solve keeps code fully weighted (no_bia_code_downweight ≈ 1) to
        // separate the satellite clock from the phase bias.  A single-receiver user that
        // inherits that weights code with uncorrected biases fully, which pulls the
        // position systematically, so the user default is restored and code on signals
        // without .BIA coverage is downweighted.
        c.no_bia_code_downweight_ = 100.0;

        if (undifferenced) {
            // Undifferenced uncombined PPP-RTK user (the end state).  No
            // observation-space differencing: the network satellite phase biases are
            // applied directly to the raw float (ssr_bias_term, undifferenced; there is
            // no applyDifferencing to re-difference them away), and the
            // AmbiguityResolver forms the between-satellite difference internally at fix
            // time (y_dd = x[i] - x[ref]), which cancels the receiver phase bias and
            // yields integer SD ambiguities.  Integer recovery therefore needs no BSSD
            // filter machinery and no pivot handover.
            c.enable_between_satellite_sd_ = false;
            // Direct per-band LAMBDA, not the MW/.BIA-frame WL: the state is already
            // PerBand, and forcing wl_only off makes the driver drop the MW WL
            // (setArIgnoreMwWl(true)), so AR runs pure MLAMBDA on the
            // network-bias-corrected float, which removes the network-bias↔.BIA frame
            // conflict.  No WL or WL/NL cascade.
            c.ar_wl_only_ = false;
            // Direct NL LAMBDA needs a converged float to keep the integer search
            // bounded: a loose σ gate (safe for the robust long-λ WL) lets the per-band
            // search explode.  The gate is capped so AR fires only once the float has
            // settled (a BIAS-ONLY float reaches cm well within this).
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
        // Shared couplings (RTA-7(a); one source of truth with the standalone profile).
        // consume_clock mirrors the local variable from before the extraction: the clock
        // coupling applies only when products are consumed.
        const bool consuming_network_clock =
            cl.consume_network_products && cl.consume_network_clock;
        applyNetworkConsumerCouplings(c, consuming_network_clock,
                                      cl.consume_network_products,
                                      cl.consumer_undifferenced);
        // Closed-loop counterpart of RTA-9(a): the derived consumer consumes live network
        // biases like the standalone profile, but this path does not run the profile, so
        // it arms the staleness gate itself under consumption, with the same respect for
        // an explicit false.  Without it the consumer would apply unboundedly stale
        // biases across producer withhold gaps and never arm the A5 freshness/presence
        // flip detectors (the source of the withhold-aligned excursions in the
        // closed-loop audit).  The arming happens on the derived config only, so the
        // producer run is untouched.  (No LogManager here; the effective value surfaces
        // through the consumer engine's config echo.)
        if (cl.consume_network_products && !c.enable_ssr_bias_max_age_
            && !c.enable_ssr_bias_max_age_explicit_)
            c.enable_ssr_bias_max_age_ = true;
        // The harness knob of card CC (A) lands on the derived consumer config as the
        // engine-level processing key (a standalone RT consumer sets the processing key
        // itself).  Never cleared here: an explicit processing.ssr_phase_bias_disc_carry
        // on the base config is kept.
        if (cl.pb_disc_carry)
            c.ssr_phase_bias_disc_carry_ = true;
        // The same derivation for the presence edges of a held phase bias (card PBF).
        // Never cleared here: an explicit processing.ssr_pb_hold_presence_flip on the
        // base config is kept.
        if (cl.pb_hold_presence_flip)
            c.ssr_pb_hold_presence_flip_ = true;
        // The same derivation for the declared-roster latch and freshness response
        // (card SID phase 5b).  Never cleared here, and the consumer detects the
        // declaration's presence from the wire rather than from this flag, so arming it
        // against a wire without a declaration is inert and says so.
        if (cl.roster_latch)
            c.consumer_roster_latch_ = true;
        // The consumer's channel mode is the consumer's (card SID phase 6b, R6).  The
        // derived config is a full copy of the producer's, so a producer at `all` would
        // otherwise widen the held-out station too and move two things at once.  Set
        // unconditionally from the harness key (default "single"), the one place the two
        // modes are separated.
        c.channel_mode_ = (cl.channel_mode == "all") ? ChannelMode::All
                                                     : ChannelMode::Single;
        // The same derivation for the every-step translation (card PBT, A).  Never
        // cleared here: an explicit processing.ssr_phase_bias_step_translate on the base
        // config is kept.  The eps is carried alongside so the harness knob is
        // self-contained; it is read only when the translation is armed.
        if (cl.pb_step_translate) {
            c.ssr_phase_bias_step_translate_     = true;
            c.ssr_phase_bias_step_translate_eps_m_ = cl.pb_step_translate_eps_m;
        }
        // Card CLV2: same derivation for the declared level-step response.  Never cleared
        // here; a non-positive scale falls back to 1.
        if (cl.level_step_inflate) {
            c.ssr_level_step_inflate_   = true;
            c.ssr_level_step_inflate_k_ =
                (cl.level_step_inflate_k > 0.0) ? cl.level_step_inflate_k : 1.0;
        }
        // The same derivation for the ambiguity level check (card ALC).  Never cleared
        // here: an explicit processing.amb_level_check on the base config is kept.  The
        // three tuning knobs are carried along so the harness key is self-contained;
        // they are read only when the check is armed.  The validation (window >= 2,
        // k > 0, mode in {act, observe}) ran on the base config's parse, so the harness
        // values are re-validated here rather than trusted.
        if (cl.amb_level_check) {
            c.amb_level_check_ = true;
            c.amb_level_check_window_ep_ = (cl.amb_level_check_window_ep >= 2)
                                         ? cl.amb_level_check_window_ep : 20;
            c.amb_level_check_k_ = (cl.amb_level_check_k > 0.0) ? cl.amb_level_check_k : 4.0;
            c.amb_level_check_mode_ =
                (cl.amb_level_check_mode == "observe") ? "observe" : "act";
        }
        // The canonical-signal rule is a network-side lever (card CSL).  The derived
        // consumer is a full copy of the producer config and runs its own PppModel, so
        // without this line processing.canonical_signal_rule would silently re-decide
        // the held-out station's per-band tracking attribute too, and the consumer's
        // latch is a separate, already-graded variable (card CSL section 1: moving the
        // consumer to the civil family costs it 162 → 80 epochs below a decimetre).  The
        // consumer is pinned on the historical rule so an arm of this knob changes only
        // the producer.
        c.canonical_signal_rule_ = CanonicalSignalRule::Count;
        // The per-station scope is the same network-side lever (card CSL-2): a scope that
        // happened to name the held-out station would put the consumer on 'priority'
        // through another path (the scope decides on its own, so pinning the rule alone
        // is not enough).  The list is cleared too, which makes the rule pin above
        // authoritative.
        c.canonical_signal_priority_stations_.clear();
        // The code half of the rule is the same network-side lever (card CSL-3).  With
        // the consumer pinned on 'count' it is unread there (the follow clause belongs to
        // 'priority' alone), so this line is redundant today, but it keeps the pin true if
        // the rule pin above is ever relaxed, and the census graded this knob by
        // replaying consumers, whose own code latch must not move with the producer's.
        c.canonical_signal_code_follows_phase_ = true;
        // The extra-band code deweight is a producer-side lever on the network's own
        // normal equations (card L5P).  The derived consumer is a full copy of the
        // producer config and would otherwise deweight its own band-5 code as well, a
        // separate variable and not the one under test (B5D section 3 graded the
        // consumer's band-5 code residual at its own weight).  Pinned at 1 so an arm of
        // this knob moves only the wire.
        c.network_config_.extra_band_code_sigma_scale = 1.0;
        // The second code channel is a producer-side mechanism (card SCC, R6): it exists
        // so the satellite C1C and C1W families share the network's stations.  A consumer
        // is one station and has no family to support; arming it there would only add a
        // receiver inter-signal state and a second code row to the held-out solution, a
        // separate variable and not the one under test.  Pinned off (and its scale at 1)
        // so an arm of this knob moves only the wire.
        c.network_config_.second_code_channel     = false;
        c.network_config_.second_code_sigma_scale = 1.0;
        // The second phase channel is a producer-side mechanism for the same reason
        // (card SID phase 6a, R6): it exists so the satellite L1C and L1W phase-bias
        // families share the network's stations.  A consumer is one station with no
        // family to support; arming it there would only add a second float ambiguity to
        // the held-out solution, a separate variable and not the one under test.  Pinned
        // off (and its scale at 1) so an arm of this knob moves only the wire.
        c.network_config_.second_phase_channel     = false;
        c.network_config_.second_phase_sigma_scale = 1.0;
        // Closed-loop-specific overlay: ambiguity resolution is enabled for the consumer
        // (the producer itself is float), because the closed loop validates integer
        // recovery on the held-out station from the producer's products.
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
        // A standalone consumer of network code biases needs receiver code-bias states
        // (estimate_ifb) to absorb a code-bias datum re-reference.  The closed-loop
        // consumer inherits estimate_ifb from the producer config, but a standalone user
        // config has no producer to inherit from, so it is armed up front, and the
        // engine's one-shot A5 warning (kept for non-profile paths) never fires on the
        // profile path.
        if (nc.bias && !estimate_ifb_) {
            estimate_ifb_ = true;
            log->info("Network-consumer profile: arming estimation.estimate_ifb — receiver "
                      "code-bias states are required to absorb network code-bias datum "
                      "re-references (disc-counter driven reinflation).");
        }
        // Bias consumption without the bias staleness gate silently applies unboundedly
        // stale biases across producer withhold gaps, so the gate is armed by default
        // under consumption (RTA-9(a)).  Presence-checked at load
        // (enable_ssr_bias_max_age_explicit_): an explicit user 'false' is respected.  The
        // coupling lives in the profile, not in applyNetworkConsumerCouplings, so the
        // closed-loop harness path does not run it; deriveClosedLoopConsumerConfig arms
        // the same gate for that path.
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
     * @brief Parse @c network.inject into @c NetworkConfig::inject (event harness).
     *
     * @param nw   The resolved @c network section.
     * @param log  Logger; every rejection is an error line naming the element index.
     * @return @c false on any malformed element (the load fails and the run aborts).
     *
     * An absent @c network.inject leaves the struct default-constructed
     * (@c enable=false, no events) and nothing in the injector is reachable, so the
     * run is byte-identical.  @c enable defaults to true when a non-empty @c events
     * array is present: arming the harness takes one key, and an events list that
     * silently did nothing is the failure the harness is built to remove.  Set
     * @c "enable": false to park a schedule without deleting it (a warning then says
     * the events are parked).
     *
     * Strictness follows the simulator's scripted-slip loader (@c SimConfig.cpp): a
     * paste-accident cap, per-index error context, and required-field checks per
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
            // Cap: a paste-accident guard, not a design statement.  The dose studies
            // (SPEC_event_harness §12) script a whole slip schedule (the campaign bed's
            // 6.8 slips/station-hour over 60 stations for 2 h is ~800 events), so the
            // guard sits well above any schedule a 24 h fixture can hold.
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
                    && ev.kind != "oracle_pin" && ev.kind != "fixed_cycle_plant") {
                    log->error("Config: {}.kind '{}' is not one of slip | outage | "
                               "anchor_handover | arc_erase | sat_clock_step | "
                               "rx_clock_step | oracle_pin | fixed_cycle_plant.",
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
                    // Diagnostic sub-knob (docs/SPEC_event_harness.md §9): which of
                    // the erase path's four extra actions the injected event performs.
                    // The default is the production path, so an event that names no
                    // mode performs the production erase.
                    if (ev.mode.empty()) ev.mode = "erase";
                    // Optional band scope: "" / "all" => every signal of the
                    // satellite (both production paths' scope); otherwise a
                    // ";"-separated list of RINEX-3 phase codes.
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
                } else if (ev.kind == "fixed_cycle_plant") {
                    // Card FCF (step FCF-3): a wrong integer planted in the ledger of fixed
                    // cycles on one arc, consumed by the fixed-cycle seam.
                    if (!need(!ev.station.empty(), "a station")) return false;
                    if (!need(!ev.sat.empty(), "a sat")) return false;
                    if (!need(!ev.signal.empty() && ev.signal[0] == 'L',
                              "a PHASE signal code (leading L), the arc's band")) return false;
                    if (!need(ev.delta_cycles != 0.0
                                  && ev.delta_cycles == std::round(ev.delta_cycles),
                              "a non-zero integer delta_cycles")) return false;
                } else if (ev.kind == "oracle_pin") {
                    // Sim-only diagnostic (include/filter/NetworkOraclePin.hpp).  Both
                    // fields are validated, and a failure names the field: a scope
                    // that silently widened, or a sigma that silently became the
                    // default, would be graded as though it were the one the operator
                    // wrote, the same failure the rx_clock_step 'sat' refusal names.
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
                } else {   // rx_clock_step: the exact dual of sat_clock_step
                    if (!need(!ev.station.empty(), "a station")) return false;
                    if (!need(ev.step_m != 0.0, "a non-zero step_m")) return false;
                    // A `sat` here would read as a scope and is not one: a receiver
                    // clock jump is station-common by construction and touches every
                    // satellite.  It is refused rather than silently ignored, because a
                    // typo that fell through would arm a different event than the
                    // operator wrote, and the leg would be graded as the intended one.
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
        // --- oracle_pin: the field-inertness mechanism ---------------------------
        // A hard error, not warn-and-disarm (the oracle_relevel precedent): a pin that
        // silently degraded to "no pin" would report the model's error budget without
        // the ambiguities held at truth and be read as a ceiling.  That is the
        // armed-and-inert failure of invariant #4, except that here the inert arm
        // produces a wrong scientific verdict, so it must not survive.
        //
        // It is also what makes this knob field-inert: a field lineage has no truth
        // root, so an oracle_pin event cannot be armed on one.
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
