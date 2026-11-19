/**
 * @file RingFixSeams.cpp
 * @brief The network seams of the ring fix: setup, the release and hold table before PASS B, the
 *        PASS B hold rows, the decision after the solve, and the run-end report (see
 *        network/ar/RingFix.hpp).  The hold epoch's and the decision's stages are in
 *        RingFixHold.cpp and RingFixDecide.cpp.
 */

#include "network/ar/RingFix.hpp"
#include "network/ar/RingFixDecide.hpp"         // the decision's stages
#include "network/ar/RingFixHold.hpp"           // the hold epoch's stages

#include "config/GnssConfig.hpp"
#include "common/GnssIdentity.hpp"            // ArcId (the latch's arcs)
#include "filter/StateKeySchema.hpp"            // StateKeys::makeAmbiguityKey
#include "network/assembly/Contributions.hpp"   // makeRowTag
#include "network/assembly/StationBuild.hpp"    // StationBuild, StationCarryFrame
#include "network/diag/RunTraces.hpp"
#include "network/slip/OraclePin.hpp"            // networkOraclePinParseAmbCsv
#include "network/slip/RawObservables.hpp"       // GapResumption (the keep-carry evidence)
#include "network/slip/SlipOracle.hpp"           // networkSlipOracleParseCsv
#include "network/driver/Epoch.hpp"
#include "network/driver/Run.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/driver/SetupResult.hpp"
#include "network/runend/RunEnd.hpp"
#include "network/solve/ChainFrame.hpp"
#include "network/state/RunContext.hpp"
#include "network/state/StationParams.hpp"
#include "rinex/MultiRinexSync.hpp"             // SyncedEpoch

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>

namespace GnssProcessor {

// ---- The helpers the seams and their stages share ----

std::string ringFixGroupOf(const std::string& sat, const std::string& sig) { return sat.substr(0, 1) + "|" + sig; }

std::set<net::ArcTuple> ringFixLatchedArcs(const RelevelEpochState& relevel) {
    std::set<net::ArcTuple> out;
    for (const auto& [sta, amb_key] : relevel.free_ambs) {
        const std::optional<ArcId> arc = net::arcKey(sta, amb_key);
        if (!arc) continue;
        out.insert({sta, net::satKey(arc->sat), net::signalKey(arc->signal)});
    }
    return out;
}

std::vector<RingFixNearNullIn> ringFixNearNullInputs(const NetworkRingFix& rf) {
    std::vector<RingFixNearNullIn> nn_in;
    for (const auto& [id, L] : rf.links) {
        if (!L.held || L.nn_n < rf.nn_min_epochs) continue;
        if (id.sat.empty() || id.sat[0] != 'E' || id.sig1.size() < 2 || id.sig2.size() < 2 || id.sig1[1] != '1'
            || id.sig2[1] != '5')
            continue;
        if (!(L.lambda1 > 0.0 && L.lambda2 > L.lambda1)) continue;
        nn_in.push_back({id, 4.0 * L.lambda2 - 3.0 * L.lambda1, L.nn_gf_m, L.nn_if_m});
    }
    return nn_in;
}

std::string ringFixStationName(const std::vector<NetStation>& stations, int i) {
    return (i >= 0 && static_cast<std::size_t>(i) < stations.size()) ? stations[static_cast<std::size_t>(i)].name
                                                                       : std::string{};
}

void ringFixGatherKeepEvidence(NetworkRingFix& rf, const std::vector<GapResumption>& resumptions) {
    rf.keep_evidence.clear();
    if (!rf.rehost_keep_carry || !rf.entryGapChecked()) return;
    std::map<RingFixLinkId, RingFixNearNullVerdict> signature;   // the last epoch's code residuals (entry_revalidate)
    if (rf.entry_revalidate)
        for (const RingFixNearNullVerdict& v :
             ringFixNearNullVerdicts(ringFixNearNullInputs(rf), rf.nn_tol_gf_m, rf.nn_max_if_m))
            signature.emplace(v.id, v);
    for (const auto& [id, L] : rf.links) {
        if (!L.held) continue;
        RingFixEntryEvidence ev;   // usable: set by the election from this epoch's latch
        for (const GapResumption& g : resumptions) {
            if (g.station != id.station || g.satellite != id.sat) continue;
            ev.resumed = true;
            if ((g.sig1 == id.sig1 && g.sig2 == id.sig2) || (g.sig1 == id.sig2 && g.sig2 == id.sig1)) {
                ev.gap_measured = g.measurable;
                ev.gap_shifted = g.shifted;
            }
        }
        if (const auto sg = signature.find(id); sg != signature.end()) {
            ev.signature = true;
            ev.signature_k = sg->second.k;
            ev.signature_if_m = sg->second.dev_if_m;
        }
        rf.keep_evidence.emplace(id, ev);
    }
}

namespace {

// ---- The parts of the setup, in the order networkRingFixArm calls them ----

/// The decision rule, the held-first walk, the settled flags' switch, the order, the gate, the gap horizon and the
/// audit file, then the run-start line that names the frozen rule.
void ringFixArmRule(NetworkRingFix& rf, const NetworkConfig& nc, double pl_max_gap_s,
                    const std::shared_ptr<spdlog::logger>& log) {
    rf.rule = nc.ring_fix.rule == "plain" ? RingFixRule::Plain
            : nc.ring_fix.rule == "log"   ? RingFixRule::Log
                                          : RingFixRule::Gated;
    rf.held_first = nc.ring_fix.held_first;
    rf.held_first_by_life = rf.held_first && nc.ring_fix.held_first_by_life;
    if (rf.held_first_by_life)
        log->warn("Network (ring fix): HELD FIRST BY LIFE: whenever the chain's walk takes a held arc to attach a vertex, "
                  "it takes the held arc of the most remaining life (the satellite's elevation at the station, plus 90 "
                  "degrees while rising; a slipped arc last), so a setting satellite is re-hosted onto the held link it "
                  "keeps longest.");
    rf.max_decisions_per_epoch = nc.ring_fix.max_decisions_per_epoch;
    rf.settled_flags_on = nc.ring_fix.settled_flags;
    if (rf.settled_flags_on)
        log->warn("Network (ring fix): SETTLED FLAGS: on every phase-bias record of a held family, signal_integer 1 "
                  "only while the satellite is reached from its datum receiver through held links and tree edges "
                  "that entered as held links, wide_lane_integer 1; both 0 on the other records.");
    rf.order = nc.ring_fix.order == "certain" ? RingFixOrder::Certain : RingFixOrder::Ring;
    rf.gate_rule = nc.ring_fix.gate == "odds" ? RingFixGateRule::Odds : RingFixGateRule::Sigma;
    rf.max_gap_s = pl_max_gap_s;
    rf.audit_file = nc.ring_fix.audit_frame_file;
    if (!rf.audit_file.empty()) rf.audit = ringFixReadAuditFile(rf.audit_file);
    log->warn("Network (ring fix): ARMED, rule {} ({}). Chords of pair families are decided ring by ring "
              "from the chain's root (gate: sigma at most {:.2f} cycle and fraction at most {:.2f} of the "
              "ionosphere-free N1 with the wide lane rounded; event window {:.0f} s; {} epochs of the same "
              "integers; the lattice vetoes), held by a prior row of sigma {:g} m from the next epoch, and "
              "released at a slip latch, a refloat, a gap beyond {:.0f} s, a chain event or the residual "
              "monitor ({:.3f} m at {} epochs). Audit: {}.",
              ringFixRuleName(rf.rule),
              rf.rule == RingFixRule::Plain ? "NEGATIVE CONTROL: every candidate rounded"
              : rf.rule == RingFixRule::Log ? "LOG ONLY: the frozen rule is traced, nothing is held"
                                            : "the frozen rule",
              kRingFixMaxSigmaCyc, kRingFixMaxFracCyc, kRingFixWindowS, kRingFixPersistEpochs,
              kRingFixHoldSigmaM, rf.max_gap_s, kRingFixMonitorBoundM, kRingFixMonitorEpochs,
              rf.audit_file.empty() ? std::string("none")
                                    : fmt::format("'{}' ({} arcs)", rf.audit_file, rf.audit.size()));
}

/// Simulation only: the fixture's truth (every held integer audited in the frame of its epoch) and the oracle phase.
/// @throws std::runtime_error when a roster station has no truth file.
void ringFixArmTruth(NetworkRingFix& rf, const NetworkConfig& nc, const std::vector<NetStation>& stations,
                     const std::shared_ptr<spdlog::logger>& log) {
    rf.oracle_until_s = nc.ring_fix.oracle_until_s;
    if (!nc.ring_fix.truth_root.empty()) {
        auto T = std::make_unique<RingFixTruth>();
        const std::filesystem::path root(nc.ring_fix.truth_root);
        long long n_arcs = 0, n_slips = 0;
        for (std::size_t i = 0; i < stations.size(); ++i) {
            const std::string& name = stations[i].name;
            const auto amb_file = root / name / "truth_ambiguities.csv";
            const auto slip_file = root / name / "truth_slips.csv";
            const auto wu_file = root / name / "truth_windup.csv";
            for (const auto* f : {&amb_file, &slip_file, &wu_file})
                if (!std::filesystem::exists(*f))
                    throw std::runtime_error("network.ring_fix.truth_root: no truth file for roster station '" + name
                                             + "' at '" + f->string() + "' (simulation only).");
            const auto ambs = networkOraclePinParseAmbCsv(amb_file.string());
            const auto slips = networkSlipOracleParseCsv(slip_file.string());
            for (const auto& [key, nv] : ambs) {
                RingFixTruth::Arc a;
                a.n_int = nv.first;
                const auto sit = slips.find(key);
                if (sit != slips.end())
                    for (const auto& sl : sit->second) {
                        a.slips.emplace_back(sl.s, sl.delta_cyc);
                        ++n_slips;
                    }
                T->arcs[{static_cast<int>(i), key.first, key.second}] = std::move(a);
                ++n_arcs;
            }
            for (auto& [sat, series] : ringFixParseWindupCsv(wu_file.string()))
                T->windup[{static_cast<int>(i), sat}] = std::move(series);
        }
        log->warn("Network (ring fix): TRUTH from '{}' (simulation only): {} arcs, {} slips; every held integer is "
                  "audited in the frame of its epoch.", nc.ring_fix.truth_root, n_arcs, n_slips);
        rf.truth = std::move(T);
    }
    if (rf.oracle_until_s > 0.0)
        log->warn("Network (ring fix): ORACLE PHASE for the first {:.0f} s (simulation only): every chord with a true "
                  "integer is held at it; afterwards the rule alone decides.", rf.oracle_until_s);
}

/// The decision's options, each named only when armed (a run under the defaults logs what it logged before): the order
/// certain, the odds gate, the pair families considered and the wide lane's bound, the settled flags' wait and rule.
void ringFixArmDecisionOptions(NetworkRingFix& rf, const NetworkConfig& nc,
                               const std::shared_ptr<spdlog::logger>& log) {
    if (rf.order == RingFixOrder::Certain)
        log->warn("Network (ring fix): order CERTAIN: every chord with a float is a candidate (rings reported, not "
                  "enforced); the links that pass are decided the highest log-odds of N1 first, {}.",
                  rf.max_decisions_per_epoch > 0 ? fmt::format("at most {} per epoch", rf.max_decisions_per_epoch)
                                                 : std::string("with no cap per epoch"));
    if (rf.gate_rule == RingFixGateRule::Odds)
        log->warn("Network (ring fix): gate ODDS: in place of the sigma bound, the log-odds of N1 and of the wide "
                  "lane's own float at least {:.4f} (ln 1e6), the fraction of N1 at most {:.2f}.",
                  kRingFixMinLogOdds, kRingFixMaxFracCyc);
    for (const std::string& e : nc.ring_fix.pairs)
        rf.pairs.insert({e.substr(0, 1), e.substr(2, 3), e.substr(6, 3)});
    rf.wl_frac_max = nc.ring_fix.wl_frac_max;
    if (!rf.pairs.empty()) {
        std::string list;
        for (const std::string& e : nc.ring_fix.pairs) list += (list.empty() ? "" : ", ") + e;
        log->warn("Network (ring fix): PAIRS {} only: a chord of another pair family is never decided, held or "
                  "traced (counted at run end).", list);
    }
    rf.settled_largest = rf.settled_flags_on && nc.ring_fix.settled_rule == "largest";
    rf.settled_wait = rf.settled_flags_on ? nc.ring_fix.settled_wait_emissions : 0;
    if (rf.settled_wait > 0)
        log->warn("Network (ring fix): SETTLED WAIT {}: a satellite the settled flags' rule puts in a family's set is "
                  "flagged only from its {}-th consecutive emission in the set.", rf.settled_wait,
                  rf.settled_wait + 1);
    if (rf.settled_largest)
        log->warn("Network (ring fix): SETTLED RULE LARGEST: the settled flags flag the largest set of a family's "
                  "satellites mutually tied through held links and certified tree edges, whatever receiver roots it, "
                  "once it holds {} satellites; the incumbent set is kept until another holds {} more.",
                  kRingFixSettledMinSats, kRingFixSettledHysteresis);
}

/// The near-null guard and its release form, the near-null monitor, the entry re-validation and the re-attachment of
/// untied satellites, each named only when armed.
void ringFixArmNearNull(NetworkRingFix& rf, const NetworkConfig& nc, const std::shared_ptr<spdlog::logger>& log) {
    rf.nn_guard = nc.ring_fix.nn_guard;
    rf.nn_min_epochs = nc.ring_fix.nn_guard_min_epochs;
    rf.nn_tol_gf_m = nc.ring_fix.nn_guard_tol_gf_m;
    rf.nn_max_if_m = nc.ring_fix.nn_guard_max_if_m;
    rf.nn_guard_release = rf.nn_guard && nc.ring_fix.nn_guard_release;
    if (rf.nn_guard)
        log->warn("Network (ring fix): NEAR-NULL GUARD: a held Galileo E1/E5a chord whose post-fit code residuals, "
                  "averaged over its last {} epochs and centred on its satellite's family and then its receiver, sit at "
                  "k near-null steps (geometry-free within {:.2f} m of k (4 lambda2 - 3 lambda1), ionosphere-free within "
                  "{:.2f} m) {}; tree edges are not moved.",
                  rf.nn_min_epochs, rf.nn_tol_gf_m, rf.nn_max_if_m,
                  rf.nn_guard_release ? "is released at its first flag (RELEASE FORM: its integer is never moved)"
                                      : "moves by -k (3, 4); a second flag during the same hold releases it");
    rf.nn_monitor = nc.ring_fix.nn_monitor;
    if (rf.nn_monitor)
        log->warn("Network (ring fix): NEAR-NULL MONITOR (measure-only): each held Galileo E1/E5a chord's code signature "
                  "(averaged over {} epochs, centred) is tallied where it sits at a whole near-null step with an "
                  "ionosphere-free deviation within {:.2f} m; events at each episode's edges and a run-end table; nothing "
                  "is moved or released.",
                  rf.nn_min_epochs, rf.nn_max_if_m);
    rf.entry_revalidate = nc.ring_fix.entry_revalidate;
    if (rf.entry_revalidate)
        log->warn("Network (ring fix): ENTRY RE-VALIDATION: a held link that enters the tree is a known step across the "
                  "cut only when, having come back across a gap (or entering a returning satellite's tree), the gap "
                  "re-entry test measured its own held pair and found no shift, and, for a Galileo E1/E5a link, its "
                  "near-null signature (code residuals averaged over {} epochs, centred) is not at a whole step with an "
                  "ionosphere-free deviation within {:.2f} m; otherwise the step is unknown and the crossing links are "
                  "released.  A chain re-host kept on a held link (network.ring_fix.rehost_keep_carry) asks the same "
                  "checks on the evidence of the epoch's start, and a refused keep is released as without the option.",
                  rf.nn_min_epochs, rf.nn_max_if_m);
    rf.reattach = rf.settled_flags_on && nc.ring_fix.reattach;
    if (rf.reattach)
        log->warn("Network (ring fix): RE-ATTACHMENT OF UNTIED SATELLITES: a satellite of a family's tree unsettled "
                  "and without a tie for {:.0f} s, with at least {} candidate links in the flagged piece (window "
                  "passed, ionosphere-free sigma at most {:.2f} cycle) whose fractions agree (circular spreads at most "
                  "{:.2f} cycle ionosphere-free, {:.2f} wide lane) on a common offset (at least {:.2f} or {:.2f} "
                  "cycle), has its tree edge moved at the next election onto the candidate nearest their medians, in "
                  "every band of its family (an ordinary re-host); not again within {:.0f} s.",
                  kRingFixReattachDwellS, kRingFixReattachMinLinks, kRingFixMaxSigmaCyc, kRingFixReattachMaxSdIfCyc,
                  kRingFixReattachMaxSdWlCyc, kRingFixReattachMinOffsetIfCyc, kRingFixReattachMinOffsetWlCyc,
                  kRingFixReattachHoldoffS);
}

/// The configured families of the chord table, the cascades and their options, the settled flags per family, then
/// the wide lane's bound named (its line follows the cascades' lines).
/// @throws std::runtime_error when a cascade names a signal of unknown wavelength.
void ringFixArmCascades(NetworkRingFix& rf, const NetworkConfig& nc, const std::shared_ptr<spdlog::logger>& log) {
    rf.form_configured_pairs = nc.ring_fix.form_configured_pairs && !rf.pairs.empty();
    if (rf.form_configured_pairs) {
        for (const auto& [c, s1, s2] : rf.pairs) rf.configured_pairs[c[0]].emplace_back(s1, s2);
        log->warn("Network (ring fix): CONFIGURED FAMILIES FORMED: the chord table forms every family of the pairs "
                  "on an arc that tracks both its signals, beside the rule that pairs band 1 with the lowest other "
                  "band.");
    }
    for (const std::string& e : nc.ring_fix.cascade) {
        RingFixCascade c;
        c.letter = e[0];
        c.ck = e.substr(0, 1);
        c.s1 = e.substr(2, 3);
        c.s2 = e.substr(6, 3);
        c.s3 = e.substr(10, 3);
        c.ref = ringFixCascadeReference(std::string(1, c.letter) + "01", c.s1, c.s2, c.s3);
        c.name = e;
        if (c.ref.empty())
            throw std::runtime_error("network.ring_fix.cascade: '" + e + "' has a signal of unknown wavelength.");
        log->warn("Network (ring fix): CASCADE {}: on every link the pair holds, {}'s integer is decided against "
                  "{}'s held integer from their float difference (fraction within {:.2f} cycle, the odds gate, {} "
                  "epochs), then held; released with the pair, at a slip of {}, across a cut of its band, by the "
                  "monitor or after a gap.", e, c.s3, c.ref, kRingFixCascadeMaxFracCyc, kRingFixPersistEpochs, c.s3);
        rf.cascades.push_back(c);
    }
    rf.cascade_tree = nc.ring_fix.cascade_tree && !rf.cascades.empty();
    if (rf.cascade_tree)
        log->warn("Network (ring fix): CASCADE TREE: a cascade's third band grows its tree on its pair's forest: the "
                  "chain's walk takes first the tree edges of both of the pair's bands, then the links the pair holds, "
                  "then the band's own incumbents and new edges.");
    rf.cascade_rehost_keep = nc.ring_fix.cascade_rehost_keep && !rf.cascades.empty();
    if (rf.cascade_rehost_keep)
        log->warn("Network (ring fix): CASCADE RE-HOST KEEPS THE CARRY: a chain re-host of a third band onto a link the "
                  "cascade holds, whose carry sits on the held integer, takes it at a step of 0 and keeps the moved "
                  "side's carries; the held cascade links across it stay held.");
    rf.cascade_log = nc.ring_fix.cascade_log && !rf.cascades.empty();
    if (rf.cascade_log)
        log->warn("Network (ring fix): CASCADE LOG: the cascades decide and trace, never hold: a link that passes the "
                  "gate for the persistence is traced as would_hold at every such epoch, and its third signal stays "
                  "float.");
    rf.flags_per_family = rf.settled_flags_on && !rf.cascades.empty();
    if (rf.flags_per_family)
        log->warn("Network (ring fix): SETTLED FLAGS PER FAMILY: a satellite signal is flagged only while every family "
                  "it belongs to flags the satellite; a cascade's third signal has the cascade's own family.");
    if (rf.wl_frac_max > 0.0)
        log->warn("Network (ring fix): WIDE-LANE BOUND: a link every other test would decide is refused while its wide "
                  "lane's fraction exceeds {:.3f} cycle (refusal wl_fraction).", rf.wl_frac_max);
}

/// The odds gate's factor on the formal sigmas, the code-event window and the re-host's keep of the carry, each named
/// only when set.
void ringFixArmRehost(NetworkRingFix& rf, const NetworkConfig& nc, const std::shared_ptr<spdlog::logger>& log) {
    rf.sigma_scale = nc.ring_fix.sigma_scale;
    rf.code_event_window_s = nc.ring_fix.code_event_window_s;
    rf.rehost_keep_carry = nc.ring_fix.rehost_keep_carry;
    if (rf.rehost_keep_carry)
        log->warn("Network (ring fix): RE-HOST KEEPS THE CARRY: a chain re-host onto a held link whose carry sits "
                  "on the held integer (within 1e-4 cycle) takes it at a step of 0 and keeps the moved side's "
                  "carried entries, receiver and satellite phase biases instead of releasing them; a held link "
                  "whose carry is off its integer is released as before.");
    if (rf.code_event_window_s > 0.0)
        log->warn("Network (ring fix): CODE-EVENT WINDOW {:.0f} s: no decision on a satellite's links within that "
                  "time after its code reference moves or its code datum re-seeds (refusal code_event); links "
                  "already held are kept.", rf.code_event_window_s);
    if (rf.sigma_scale != 1.0)
        log->warn("Network (ring fix): SIGMA SCALE {:.2f}: the log-odds of N1 and of the wide lane are computed with "
                  "{:.2f} times their formal sigmas (the trace keeps the formal sigmas).", rf.sigma_scale,
                  rf.sigma_scale);
}

/// Simulation only: the slip mask of one arc.
/// @throws std::runtime_error when the masked arc's station is not in the roster.
/// The response to a tracking break of a held link's own (network.ring_fix.resume_response), named at run start when on.
void ringFixArmResume(NetworkRingFix& rf, const NetworkConfig& nc, const std::shared_ptr<spdlog::logger>& log) {
    const std::string& v = nc.ring_fix.resume_response;
    rf.resume_response = v == "release"      ? RingFixResumeResponse::Release
                       : v == "revalidate"   ? RingFixResumeResponse::Revalidate
                                             : RingFixResumeResponse::Off;
    if (rf.resume_response == RingFixResumeResponse::Release)
        log->warn("Network (ring fix): RESUME RESPONSE release: a held link whose own station and satellite resume after "
                  "a tracking break with a shifted or unmeasured level (the slip layer's gap resumption records of the "
                  "epoch, on the link's pair or one of its signals) is released in that epoch, before PASS B, cause "
                  "resume, its carried entries re-opened; a clean measured resumption keeps the hold.");
    else if (rf.resume_response == RingFixResumeResponse::Revalidate)
        log->warn("Network (ring fix): RESUME RESPONSE revalidate: such a held link is suspended in that epoch (not held, "
                  "carried entries re-opened, its integers kept) and held again at them once its own float accepts them "
                  "at the level {:g} (chi-square, two degrees of freedom, formal covariance times sigma_scale^2) and the "
                  "odds rule decides them, at {} consecutive decision epochs; a rejection while the odds rule decides "
                  "other integers, the decision window of {:.0f} s without re-validation, a slip, a chain event or a new "
                  "break give the integers up (cause resume); a clean measured resumption keeps the hold.",
                  kRingFixResumeLevel, kRingFixPersistEpochs, kRingFixWindowS);
}

void ringFixArmSlipMask(NetworkRingFix& rf, const NetworkConfig& nc, const std::vector<NetStation>& stations,
                        const std::shared_ptr<spdlog::logger>& log) {
    if (nc.ring_fix.mask_slip_arc.empty()) return;
    const std::string& arc = nc.ring_fix.mask_slip_arc;
    const std::string sta = arc.substr(0, arc.find(':'));
    rf.mask_sat = arc.substr(arc.find(':') + 1);
    for (std::size_t i = 0; i < stations.size(); ++i)
        if (stations[i].name == sta) rf.mask_station = static_cast<int>(i);
    if (rf.mask_station < 0)
        throw std::runtime_error("network.ring_fix.mask_slip_arc: no roster station '" + sta + "'.");
    rf.mask_tow = nc.ring_fix.mask_slip_tow;
    rf.mask_n_epochs = nc.ring_fix.mask_slip_epochs;
    log->warn("Network (ring fix): SLIP MASK (simulation only): the producer's slip flags of {} are masked for {} "
              "epoch(s) from TOW {:.0f}: its carried entries are restored after the epoch's slip drains and it is "
              "taken out of the free-epoch latch, the slip funnel and the bridge pends.",
              arc, rf.mask_n_epochs, rf.mask_tow);
}

}  // namespace

// ---- Setup ----

void networkRingFixArm(NetworkRun& run) {
    NetworkRunContext& ctx = run.ctx;
    const NetworkConfig& nc = run.nc;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    const std::vector<NetStation>& stations = run.setup.stations;
    const double pl_max_gap_s = ctx.knobs.pl_max_gap_s;
    // ---- End of the bindings ----
    if (!nc.ring_fix.enable) return;
    auto rf = std::make_unique<NetworkRingFix>();
    ringFixArmRule(*rf, nc, pl_max_gap_s, log);
    ringFixArmTruth(*rf, nc, stations, log);
    ringFixArmDecisionOptions(*rf, nc, log);
    ringFixArmNearNull(*rf, nc, log);
    ringFixArmCascades(*rf, nc, log);
    ringFixArmRehost(*rf, nc, log);
    ringFixArmSlipMask(*rf, nc, stations, log);
    ringFixArmResume(*rf, nc, log);
    ctx.carry.ring_fix = std::move(rf);
}

// ---- Before PASS B: the releases and the hold table ----

void networkRingFixHoldEpoch(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch ----
    const NetworkRunContext& ctx = run.ctx;
    const ChainEpochObs* obs_rec = ep.events.phase_datum.chain.get();
    const RelevelEpochState& relevel = ep.events.slip.relevel;
    // ---- End of the bindings ----
    if (!ctx.carry.ring_fix) return;
    networkRingFixHoldTruth(run, ep);
    const RingFixChainFacts facts = ringFixChainFacts(obs_rec);
    const std::set<net::ArcTuple> latched = ringFixLatchedArcs(relevel);
    const std::vector<RingFixCut> cuts = networkRingFixHoldCuts(run, ep, latched);
    networkRingFixHoldThroughCuts(run, ep, latched, cuts);
    networkRingFixHoldToTree(run, ep, facts);
    networkRingFixHoldRoots(run, ep, facts);
    networkRingFixHoldReleases(run, ep, facts, latched);
    networkRingFixHoldCascadeReleases(run, ep, latched, cuts);
    networkRingFixHoldInherit(run, ep, facts, latched, cuts);
    networkRingFixHoldTargets(run, ep);
    networkRingFixHoldTable(run);
}

// ---- PASS B: the hold rows ----

void networkRingFixPriors(StationCarryFrame& carry) {
    StationBuild& build = carry.build;
    const NetworkRunContext& ctx = build.ctx;
    const NetworkSetupResult& setup = build.setup;
    const int i = build.i;
    NetStation& st = build.st;
    StationAssembly& R = build.R;
    std::map<int, int>& h2local = R.h2local;
    const bool legacy_local_inject = carry.legacy_local_inject;
    const Eigen::VectorXd& x_lin = carry.x_lin;
    std::vector<std::tuple<Eigen::Index, double, double, RowTag>>& inj = carry.inj;
    const auto& stations = setup.stations;
    // This epoch's hold table, built serially before PASS B and only read here; null on every
    // run without the ring fix, so this step is unreachable when off.
    const std::map<std::tuple<int, std::string, std::string>, double>* table =
        ctx.carry.ring_fix ? &ctx.carry.ring_fix->table : nullptr;
    const bool rfix_on = table != nullptr && !table->empty();
    // ---- End of the bindings ----
    if (!(legacy_local_inject && rfix_on)) return;
    const double lam = 1.0 / (kRingFixHoldSigmaM * kRingFixHoldSigmaM);
    for (const auto& [arc, amb_idx] : st.params.amb) {
        const std::string arc_sat = net::satKey(arc.sat);
        const std::string arc_signal = net::signalKey(arc.signal);
        const auto it = table->find(std::make_tuple(i, arc_sat, arc_signal));
        if (it == table->end()) continue;
        ++R.rfix_offered;
        // A tree edge's 1e10 pin is the frame: a second row there would fight it.  The release
        // step releases a link that became a tree edge before PASS B; this is the backstop.
        // Each skip names its arc in this station's own list of its cause (merged serially, card CNN row 4).
        const auto skip = [&](RingFixRowSkip c, long long& n) {
            ++n;
            R.rfix_skip_arcs[static_cast<std::size_t>(c)].emplace_back(arc_sat, arc_signal);
        };
        if (stations[static_cast<std::size_t>(i)].pinned_amb_targets.count(
                StateKeys::makeAmbiguityKey(arc_sat, arc_signal)) != 0) {
            skip(RingFixRowSkip::TreeEdge, R.rfix_gauge_skipped);
            continue;
        }
        if (amb_idx >= x_lin.size()) { skip(RingFixRowSkip::IndexPastState, R.rfix_no_column_index); continue; }
        const auto hit = h2local.find(amb_idx);
        if (hit == h2local.end() || !std::isfinite(x_lin(amb_idx))) {
            skip(RingFixRowSkip::NoLocalColumn, R.rfix_no_column_local);
            continue;
        }
        const double eta = lam * (it->second - x_lin(amb_idx));
        if (!std::isfinite(eta)) { skip(RingFixRowSkip::NonFiniteRow, R.rfix_no_column_eta); continue; }
        inj.emplace_back(static_cast<Eigen::Index>(hit->second), lam, eta,
                         makeRowTag(ContributionKind::PriorAmbiguityRingHold, arc_sat, arc_signal));
        ++R.rfix_rows;
    }
}

namespace {

/// A hold row's link as the run-end line names it: the row's station and satellite, its signal with the pair (or
/// cascade) it belongs to, and that link's ring now; a row whose link is not found keeps its signal alone.
RingFixNamedLink ringFixRowLink(const NetworkRingFix& rf, int station, const std::string& sat, const std::string& sig) {
    RingFixNamedLink out{station, sat, sig, -1};
    for (auto it = rf.links.lower_bound({station, sat, "", ""});
         it != rf.links.end() && it->first.station == station && it->first.sat == sat; ++it)
        if (it->first.sig1 == sig || it->first.sig2 == sig) {
            out.signals = sig + " of " + it->first.sig1 + "/" + it->first.sig2;
            out.ring = it->second.ring;
            return out;
        }
    for (auto it = rf.cascade_links.lower_bound({station, sat, "", ""});
         it != rf.cascade_links.end() && it->first.station == station && it->first.sat == sat; ++it)
        if (it->first.sig2 == sig) {   // a cascade link keeps no ring of its own
            out.signals = sig + " of cascade " + it->first.sig1 + ">" + it->first.sig2;
            return out;
        }
    return out;
}

}  // namespace

void networkRingFixStationTally(NetworkRunContext& ctx, const std::vector<StationAssembly>& asm_res) {
    if (!ctx.carry.ring_fix) return;
    NetworkRingFix& rf = *ctx.carry.ring_fix;
    RingFixCounters& C = rf.counters;
    for (std::size_t i = 0; i < asm_res.size(); ++i) {
        const StationAssembly& Rc = asm_res[i];
        C.rows_offered         += Rc.rfix_offered;
        C.rows_injected        += Rc.rfix_rows;
        C.rows_gauge_skipped   += Rc.rfix_gauge_skipped;
        C.rows_no_column_index += Rc.rfix_no_column_index;
        C.rows_no_column_local += Rc.rfix_no_column_local;
        C.rows_no_column_eta   += Rc.rfix_no_column_eta;
        for (std::size_t c = 0; c < kRingFixRowSkipCount; ++c)
            for (const auto& [sat, sig] : Rc.rfix_skip_arcs[c])
                C.rows_skipped_links[c].add(ringFixRowLink(rf, static_cast<int>(i), sat, sig));
    }
}

// ---- After the solve: the monitor, the audit and the decisions ----

void networkRingFixDecide(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch ----
    const NetworkRunContext& ctx = run.ctx;
    const ChainEpochObs* obs_rec = ep.events.phase_datum.chain.get();
    // ---- End of the bindings ----
    if (!ctx.carry.ring_fix || obs_rec == nullptr || !obs_rec->armed) return;
    networkRingFixDecideReports(run, ep);
    const RingFixDecideChain chain = networkRingFixDecideChain(run, ep);
    const RingFixDecideReadings readings = networkRingFixDecideReadings(run, ep, chain);
    const RingFixDecideAudit audit = networkRingFixDecideAudit(run, readings);
    networkRingFixDecideLiveHolds(run, ep, readings, audit);
    const std::vector<RingFixEvent> decisions = networkRingFixDecideLinks(run, ep, readings);
    const RingFixDecideCascades cascades = networkRingFixDecideCascades(run, ep, chain, audit);
    networkRingFixDecideTrace(run, ep, readings, audit, decisions, cascades);
    networkRingFixDecideSettled(run, ep, chain, readings, cascades);
    networkRingFixDecideNearNull(run, ep);
}

// ---- The near-null monitor (an epoch observer) ----

void networkRingFixNearNullMonitor(NetworkRun& run, const NetworkEpoch& ep) {
    NetworkRunContext& ctx = run.ctx;
    NetworkRunTraces& traces = run.traces;
    const std::vector<NetStation>& stations = run.setup.stations;
    const GnssTime& t = ep.se.time;
    // ---- End of the bindings ----
    if (!ctx.carry.ring_fix || !ctx.carry.ring_fix->nn_monitor) return;
    NetworkRingFix& rf = *ctx.carry.ring_fix;
    const auto nameOf = [&](int i) {
        return (i >= 0 && static_cast<std::size_t>(i) < stations.size()) ? stations[static_cast<std::size_t>(i)].name
                                                                           : std::string{};
    };
    const std::vector<RingFixNearNullVerdict> v =
        ringFixNearNullVerdicts(ringFixNearNullInputs(rf), rf.nn_tol_gf_m, rf.nn_max_if_m);
    for (const RingFixNnMonitorEdge& e : ringFixNnMonitorStep(rf.nn_mon, v, t.tow, rf.nn_max_if_m))
        traces.emitEvent(t, "RINGFIX", e.opened ? "nn_monitor_on" : "nn_monitor_off", e.id.sat, nameOf(e.id.station),
                         e.dev_gf_m, std::nullopt, "monitor",
                         fmt::format("sig1={} sig2={} k={} dev_gf={:.3f} dev_if={:.3f}", e.id.sig1, e.id.sig2, e.k,
                                     e.dev_gf_m, e.dev_if_m));
}

// ---- Run-end report ----

int networkRingFixRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    // ---- End of the bindings ----
    if (!ctx.carry.ring_fix) return 0;
    const NetworkRingFix& rf = *ctx.carry.ring_fix;
    const RingFixCounters& C = rf.counters;
    long long held_end = 0;
    std::map<int, long long> held_by_ring;
    for (const auto& [id, L] : rf.links)
        if (L.held) { ++held_end; ++held_by_ring[L.ring]; }
    std::string by_ring, first, held_rings;
    for (int r = 0; r < 8; ++r) {
        if (C.decided_by_ring[r] > 0)
            by_ring += fmt::format("{}ring {}: {}", by_ring.empty() ? "" : ", ", r, C.decided_by_ring[r]);
        if (C.first_hold_s[r] >= 0.0)
            first += fmt::format("{}ring {} at TOW {:.0f}", first.empty() ? "" : ", ", r,
                                 std::fmod(C.first_hold_s[r], 604800.0));
    }
    for (const auto& [r, n] : held_by_ring)
        held_rings += fmt::format("{}ring {}: {}", held_rings.empty() ? "" : ", ", r, n);
    std::string released, refused;
    for (std::size_t k = 0; k < kRingFixReleaseCount; ++k) {
        // The resume cause (last) is named only with network.ring_fix.resume_response on: off, the line is unchanged.
        if (static_cast<RingFixRelease>(k) == RingFixRelease::Resume && rf.resume_response == RingFixResumeResponse::Off)
            continue;
        released += fmt::format("{}{} {}", k ? ", " : "", ringFixReleaseName(static_cast<RingFixRelease>(k)),
                                C.released[k]);
    }
    for (std::size_t k = 0; k < kRingFixRefuseCount; ++k) {
        const auto c = static_cast<RingFixRefuse>(k);
        if (c == RingFixRefuse::Odds && rf.gate_rule != RingFixGateRule::Odds) continue;
        if (c == RingFixRefuse::Cap && rf.order != RingFixOrder::Certain) continue;
        if (c == RingFixRefuse::WlFraction && !(rf.wl_frac_max > 0.0)) continue;
        if (c == RingFixRefuse::CodeEvent && !(rf.code_event_window_s > 0.0)) continue;
        refused += fmt::format("{}{} {}", k ? ", " : "", ringFixRefuseName(c), C.refused[k]);
    }
    long long rel_total = 0;
    for (std::size_t k = 0; k < kRingFixReleaseCount; ++k) rel_total += C.released[k];
    const long long rows_no_column = C.rows_no_column_index + C.rows_no_column_local + C.rows_no_column_eta;
    const bool rows_closed = C.rows_offered == C.rows_injected + C.rows_gauge_skipped + rows_no_column;
    log->warn("Network (ring fix, rule {}): {} link(s) decided over {} decision epoch(s) ({}); first hold by "
              "ring: {}; held at the end {} ({}); {} held link-epoch(s); {} hold-table hit(s): {} hold row(s) "
              "folded, {} skipped on a tree edge, {} without a column ({} index past the state, {} not a column of "
              "the epoch, {} a non-finite row; {}).",
              ringFixRuleName(rf.rule), C.decided, C.epochs_decided,
              by_ring.empty() ? "none" : by_ring, first.empty() ? "none" : first, held_end,
              held_rings.empty() ? "none" : held_rings, C.held_link_epochs, C.rows_offered, C.rows_injected,
              C.rows_gauge_skipped, rows_no_column, C.rows_no_column_index, C.rows_no_column_local,
              C.rows_no_column_eta, rows_closed ? "CLOSED" : "OPEN: the sum differs");
    // The links behind the hold rows that never reached the solve, by cause (card CNN row 4); only when any.
    std::vector<std::string> station_names;
    station_names.reserve(v.setup.stations.size());
    for (const NetStation& st : v.setup.stations) station_names.push_back(st.name);
    const auto linkText = [&](const RingFixNamedLink& l) { return ringFixNamedLinkText(l, station_names); };
    if (C.rows_gauge_skipped + rows_no_column > 0) {
        std::string by_cause;
        for (std::size_t k = 0; k < kRingFixRowSkipCount; ++k) {
            const BoundedTally<RingFixNamedLink>& t = C.rows_skipped_links[k];
            if (t.empty()) continue;
            by_cause += fmt::format("{}{} {}: {}", by_cause.empty() ? "" : "; ",
                                    ringFixRowSkipName(static_cast<RingFixRowSkip>(k)), t.total(), t.text(linkText));
        }
        log->warn("Network (ring fix, hold rows not folded): {}.", by_cause);
    }
    // A link that became a tree edge left the held set without a release (the chain pins it).
    const std::string to_tree = C.to_tree > 0 ? fmt::format(" + to tree {}", C.to_tree) : std::string();
    // An open suspension (network.ring_fix.resume_response "revalidate") is neither held nor released.
    const long long suspended_end = ringFixActiveCrossings(rf);
    const std::string suspended = suspended_end > 0 ? fmt::format(" + suspended {}", suspended_end) : std::string();
    log->warn("Network (ring fix): released {} ({}); {} carried entr(ies) re-opened by the release; "
              "decided {} = released {}{} + held {}{}: {}.  Refused link-epochs: {}.",
              rel_total, released, C.carry_reopened, C.decided, rel_total, to_tree, held_end, suspended,
              C.decided == rel_total + C.to_tree + held_end + suspended_end ? "closed" : "NOT CLOSED", refused);
    if (rf.resume_response != RingFixResumeResponse::Off) {
        const auto byClass = [&](const char* what, const long long* n) {
            return fmt::format("{} {} (shifted {}, unmeasured {})", what,
                               n[static_cast<std::size_t>(RingFixResumeClass::Shifted)]
                                   + n[static_cast<std::size_t>(RingFixResumeClass::Unmeasured)],
                               n[static_cast<std::size_t>(RingFixResumeClass::Shifted)],
                               n[static_cast<std::size_t>(RingFixResumeClass::Unmeasured)]);
        };
        long long susp = 0, closed = 0;
        for (std::size_t k = 0; k < kRingFixResumeClassCount; ++k) {
            susp += C.resume_suspended[k];
            closed += C.resume_revalidated[k] + C.resume_contradicted[k] + C.resume_expired[k] + C.resume_ended[k];
        }
        const std::string tail =
            rf.resume_response == RingFixResumeResponse::Release
                ? byClass("released", C.resume_released)
                : fmt::format("{}; of them {}, {}, {}, {}; open at the end {}: {}", byClass("suspended", C.resume_suspended),
                              byClass("held again", C.resume_revalidated),
                              byClass("contradicted", C.resume_contradicted), byClass("expired", C.resume_expired),
                              byClass("ended", C.resume_ended), suspended_end,
                              susp == closed + suspended_end ? "closed" : "NOT CLOSED");
        log->warn("Network (ring fix, resume response {}): {} clean resumption(s) on held links kept; {}; {} at a missing "
                  "carry; the first links: {}.",
                  ringFixResumeResponseName(rf.resume_response), C.resume_kept_clean, tail, C.resume_fresh,
                  C.resume_links.text(linkText));
    }
    // The first links refused by each cause (card CNN corrections item 7); one line per cause that refused any.
    for (std::size_t k = 0; k < kRingFixRefuseCount; ++k) {
        const BoundedTally<RingFixNamedLink>& t = C.refused_links[k];
        if (t.empty()) continue;
        log->warn("Network (ring fix, refused links): {} {} link-epoch(s), the first links: {}.",
                  ringFixRefuseName(static_cast<RingFixRefuse>(k)), t.total(), t.text(linkText));
    }
    if (C.cuts > 0 || C.to_tree > 0 || C.inherited > 0 || C.root_kept > 0)
        log->warn("Network (ring fix, the chain's events): {} cut(s) on a band, {} with a known step ({} of 0); {} held "
                  "link(s) re-expressed, {} released across an unknown step; {} became tree edges; {} old host(s) "
                  "inherited; {} kept through a pinned root handover; {} stamp rebase(s) after a cut's erase.",
                  C.cuts, C.cuts_known, C.cuts_known_zero, C.reexpressed, C.cut_released, C.to_tree, C.inherited,
                  C.root_kept, C.stamps_rebased);
    if (C.cuts > 0 || rf.held_first)
        log->warn("Network (ring fix, cuts by kind): {} held first ({} with a step of 0), {} other known, {} unknown; "
                  "{} re-attachment(s) through a held link kept {} held link(s); {} usable held link(s) released "
                  "across an unknown step; {} cut(s) with a nested release; {} carried entr(ies) re-opened to the "
                  "release floor of {:g} m^2.{}",
                  C.cuts_held_first, C.cuts_held_first_zero, C.cuts_known - std::min(C.cuts_known, C.cuts_held_first),
                  C.cuts - C.cuts_known, C.reattached, C.held_first_kept, C.cut_released_usable, C.cuts_nested,
                  C.release_floored, kRingFixReleaseFloorM2,
                  rf.held_first ? " Held first ARMED (network.ring_fix.held_first)." : "");
    if (rf.settled_flags_on)
        log->warn("Network (ring fix, settled flags): {} satellite-signal-epochs of held families flagged settled, "
                  "{} not; {} flips between consecutive epochs.{}",
                  C.settled_signal_epochs, C.unsettled_signal_epochs, C.settled_flips,
                  (rf.settled_largest ? fmt::format("  Rule largest: {} change(s) of identity, {} satellite(s) changed "
                                                    "side at them.", C.settled_identity_changes,
                                                    C.settled_side_changes)
                                      : std::string())
                      + (rf.settled_wait > 0 ? fmt::format("  Wait of {} emission(s): {} satellite-signal-epochs in "
                                                           "the set held back before the raise.", rf.settled_wait,
                                                           C.settled_wait_held)
                                             : std::string()));
    if (rf.reattach)
        log->warn("Network (ring fix, re-attachment of untied satellites): {} move(s) requested; {} band-move(s) "
                  "applied, {} missed; past the {:.0f} s dwell without a move: {} satellite-epoch(s) with fewer than "
                  "{} candidates, {} whose candidates disagree, {} whose candidates converge (offsets below {:.2f} and "
                  "{:.2f} cycle), {} within a {:.0f} s hold-off; moved satellites flagged within {:.0f} s: {}, not: {}.",
                  C.untied_requests, C.untied_applied, C.untied_missed, kRingFixReattachDwellS, C.untied_few,
                  kRingFixReattachMinLinks, C.untied_spread, C.untied_converging, kRingFixReattachMinOffsetIfCyc,
                  kRingFixReattachMinOffsetWlCyc, C.untied_held_off, kRingFixReattachHoldoffS, C.untied_flagged_after,
                  C.untied_unflagged_after);
    if (rf.nn_guard)
        log->warn("Network (ring fix, near-null guard): {} held chord-epoch(s) checked; {} flagged (a deviation fitting k "
                  "near-null steps within {:.2f} m, ionosphere-free within {:.2f} m), {} refused for the ionosphere-free "
                  "deviation; {} pair(s) moved by -k (3, 4), {} of them by one step; {} chord(s) released for a second "
                  "flag; {} carried entr(ies) re-opened.",
                  C.nn_link_checks, C.nn_flagged, rf.nn_tol_gf_m, rf.nn_max_if_m, C.nn_refused_if, C.nn_moves,
                  C.nn_moves_one, C.nn_releases, C.nn_reopened);
    if (rf.nn_monitor) {
        const RingFixNnMonitor& M = rf.nn_mon;
        std::vector<std::pair<RingFixLinkId, RingFixNnMonitorLink>> rows(M.links.begin(), M.links.end());
        rows.erase(std::remove_if(rows.begin(), rows.end(), [](const auto& r) { return r.second.epochs == 0; }), rows.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.epochs > b.second.epochs; });
        log->warn("Network (ring fix, near-null monitor): {} held chord-epoch(s) with a signature, {} at a whole step "
                  "(ionosphere-free within {:.2f} m); {} chord(s) ever at a step; the chords with the most step epochs "
                  "follow (measure-only: nothing moved or released).",
                  M.checks, M.steps, rf.nn_max_if_m, rows.size());
        const auto& names = v.setup.stations;
        for (std::size_t i = 0; i < rows.size() && i < 20; ++i) {
            const auto& [id, L] = rows[i];
            const std::string name = (id.station >= 0 && static_cast<std::size_t>(id.station) < names.size())
                                         ? names[static_cast<std::size_t>(id.station)].name : std::string("?");
            log->warn("   {}-{} {}/{}: {} step epoch(s) in {} episode(s), k {}, mean geometry-free {:+.3f} m, "
                      "ionosphere-free {:+.3f} m, tow {:.0f} to {:.0f}",
                      name, id.sat, id.sig1, id.sig2, L.epochs, L.episodes, L.k,
                      L.sum_gf_m / static_cast<double>(L.epochs), L.sum_if_m / static_cast<double>(L.epochs),
                      L.first_tow, L.last_tow);
        }
    }
    if (rf.entryGapChecked())
        log->warn("Network (ring fix, entry re-validation): {} cut(s) on a band whose entry link was a usable held link; "
                  "{} taken as a known step; {} refused: {} with the held pair not measured across a gap, {} shifted "
                  "across it, {} for the near-null signature (their steps unknown, the crossing links released).",
                  C.entry_checked, C.entry_known, C.entry_checked - std::min(C.entry_checked, C.entry_known),
                  C.entry_refused_unchecked, C.entry_refused_shifted, C.entry_refused_signature);
    if (rf.entryGapChecked() && rf.rehost_keep_carry)
        log->warn("Network (ring fix, entry re-validation of kept re-hosts): {} re-host(s) onto a usable held link "
                  "checked (per band); {} kept at a step of 0; {} refused: {} with the held pair not measured across a "
                  "gap, {} shifted across it, {} for the near-null signature (released as without the option).",
                  C.keep_checked, C.keep_known, C.keep_checked - std::min(C.keep_checked, C.keep_known),
                  C.keep_refused_unchecked, C.keep_refused_shifted, C.keep_refused_signature);
    for (const RingFixCascade& c : rf.cascades) {
        const auto it = rf.cascade_counters.find(c.name);
        const RingFixCascadeCounters CC = it != rf.cascade_counters.end() ? it->second : RingFixCascadeCounters{};
        std::string rel;
        long long rel_n = 0;
        for (const auto& [cause, n] : CC.released) {
            rel += fmt::format("{}{} {}", rel.empty() ? "" : ", ", cause, n);
            rel_n += n;
        }
        log->warn("Network (ring fix, cascade {} against {}): decided {}; held link-epochs {}; released {}{}; to the "
                  "tree {}; refused link-epochs: fraction {}, odds {}, persistence {}, no float {}.{}{}{}{}",
                  c.name, c.ref, CC.decided, CC.held_link_epochs, rel_n, rel.empty() ? std::string() : " (" + rel + ")",
                  CC.to_tree, CC.refused_fraction, CC.refused_odds, CC.refused_persist, CC.refused_no_float,
                  rf.truth ? fmt::format("  Truth: held link-epochs right {}, WRONG {}; decisions WRONG {}.",
                                         CC.truth_right, CC.truth_wrong, CC.decided_truth_wrong)
                           : std::string(),
                  rf.cascade_tree ? fmt::format("  Tree on the pair's forest: {} tree edge-epochs of {}, {} on the "
                                                "pair's tree edges, {} on links the pair holds, {} otherwise.",
                                                CC.tree_edges, c.s3, CC.tree_pair, CC.tree_held, CC.tree_other)
                                  : std::string(),
                  rf.cascade_log ? fmt::format("  Log mode: {} would-hold link-epochs, none held{}.", CC.would_hold,
                                               rf.truth ? fmt::format(" (truth: WRONG {})", CC.would_truth_wrong)
                                                        : std::string())
                                 : std::string(),
                  rf.cascade_rehost_keep
                      ? fmt::format("  Re-hosts of {} onto held links: {} kept at the held integer, {} released off "
                                    "it, {} released for another cause.", c.s3, CC.rehost_kept, CC.rehost_off_integer,
                                    CC.rehost_not_kept)
                      : std::string());
    }
    if (rf.rehost_keep_carry)
        log->warn("Network (ring fix, re-hosts onto held links): {} kept with their carries (the new host's carry on "
                  "the held integer); {} released because that carry was 1e-4 cycle or more off the held integer (a "
                  "link decided the epoch before); {} released for another cause (no carry at the new host, an "
                  "unknown draft step).",
                  C.rehost_kept, C.rehost_off_integer, C.rehost_not_kept);
    if (!rf.pairs.empty()) {
        // Each listed pair with the link-epochs it was considered at, and the families left out by name (card CNN
        // row 2); a listed pair that matched no chord gets a warning of its own.
        std::string considered;
        std::vector<std::string> unmatched;
        for (const auto& [sys, s1, s2] : rf.pairs) {
            const std::string fam = sys + ":" + s1 + "/" + s2;
            const auto it = C.pair_link_epochs.find(fam);
            const long long n = it != C.pair_link_epochs.end() ? it->second : 0;
            considered += fmt::format("{}{} {}", considered.empty() ? "" : ", ", fam, n);
            if (n == 0) unmatched.push_back(fam);
        }
        log->warn("Network (ring fix, pairs): {} chord link-epoch(s) of other pair families were not considered "
                  "({}); considered by listed pair: {}.",
                  C.excluded_link_epochs, C.excluded_families.text([](const std::string& f) { return f; }), considered);
        for (const std::string& fam : unmatched)
            log->warn("Network (ring fix, pairs): the listed pair {} matched no chord link-epoch over the run: no "
                      "station tracked it as a chord of a pair family, or its signals are read under another family "
                      "(a member signal under its identity's).", fam);
    }
    if (rf.mask_station >= 0)
        log->warn("Network (ring fix, slip mask, simulation only): {} masked epoch(s); {} latch, funnel or pend entr(ies) "
                  "removed; {} carried entr(ies) the drains had changed restored.",
                  C.mask_epochs, C.mask_keys_removed, C.mask_carries_restored);
    if (rf.rule == RingFixRule::Log)
        log->warn("Network (ring fix, log rule): the frozen rule would have decided {} link(s) at {} link-epoch(s), "
                  "{} of them with integers other than the link's first{}; nothing was held.",
                  C.log_links, C.log_link_epochs, C.log_changed,
                  rf.truth ? fmt::format("; against truth {} right, {} WRONG", C.log_truth_right, C.log_truth_wrong)
                           : std::string());
    if (rf.oracle_until_s > 0.0)
        log->warn("Network (ring fix, oracle phase of {:.0f} s): {} link(s) held at their true integers, {} held "
                  "integer(s) set again, {} hold(s) put off to the next epoch (released, latched, re-levelled or "
                  "cut in the epoch).", rf.oracle_until_s, C.oracle_decided, C.oracle_corrected, C.oracle_waited);
    if (rf.truth)
        log->warn("Network (ring fix truth audit, in each epoch's frame): held link-epochs right {}, WRONG {}, without "
                  "a true integer {}; decisions WRONG {}; {} branch(es) started without a wind-up row{}.",
                  C.truth_right, C.truth_wrong, C.truth_none, C.decided_truth_wrong, rf.truth->arcs_without_windup,
                  C.truth_file_checked > 0
                      ? fmt::format("; against the audit file {} chord-band-epochs compared, {} differ",
                                    C.truth_file_checked, C.truth_file_mismatch)
                      : std::string());
    if (rf.audit.empty())
        log->warn("Network (ring fix audit): no audit file; nothing was compared against truth.");
    else
        log->warn("Network (ring fix audit, '{}'): decisions right {}, WRONG {}; held link-epochs right {}, "
                  "WRONG {}, not listed {}.",
                  rf.audit_file, C.decided_right, C.decided_wrong, C.audit_right, C.audit_wrong,
                  C.audit_unlisted);
    return 0;
}

} // namespace GnssProcessor
