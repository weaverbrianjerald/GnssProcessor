/**
 * @file SnapshotFingerprint.cpp
 * @brief @internal networkSnapshotFingerprint: the configuration fingerprint (RTB-7) that a
 *        state snapshot carries and a warm resume checks.
 *
 * One function, called once by the driver (network/driver/ApplicationNetwork.cpp) before the warm
 * resume, and directly by unit tests.  With a snapshot path it sets
 * ctx.carry.live_fingerprint and, under network.ctu_switch_on_resume, the CTU twin
 * ctx.carry.live_fingerprint_ctu_off; without one it does nothing.  The comments in the
 * body record why each knob is or is not a term.
 *
 * Three-place rule (include/network/state/RunContext.hpp): a snapshotted field is added to
 * the fingerprint here, to the restore in WarmResume.cpp and to the write in
 * StateSnapshotWrite.cpp.
 *
 * The body opens with an alias block (decomposition INC-4) that binds the driver's former
 * local names to members of the context, so the relocated code keeps those names.
 *
 * One of the seven files that define the network run context's functions, split
 * out of the former app/NetworkRunContext.cpp by concern, each body
 * unchanged.  The declarations, each naming its file, and the map of the seven files are
 * in include/network/state/RunContext.hpp.
 */

#include "network/state/RunContext.hpp"

#include "config/GnssConfig.hpp"
#include "network/solve/Sbasis.hpp"          // kWindUpYawModelId (wind-up model revision token)
#include "products/NetworkStateSnapshot.hpp" // NetworkStateSnapshotIO::fnv1a64

#include <fmt/format.h>

namespace GnssProcessor {

void networkSnapshotFingerprint(NetworkRunContext& ctx, const GnssConfig& config_,
                                const std::vector<NetStation>& stations) {
    // Alias block (INC-4): the relocated body reads the former runNetwork() locals under
    // their original names.
    const auto& nc = config_.getNetworkConfig();
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    auto& snap_path = ctx.carry.snap_path;
    auto& live_fingerprint = ctx.carry.live_fingerprint;
    const bool   phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    const bool   phase_datum_eq34 = ctx.knobs.phase_datum_eq34;
    const bool   carry_invariant = ctx.knobs.carry_invariant;
    const bool   carry_invariant_pd_safe = ctx.knobs.carry_invariant_pd_safe;
    const bool   pl_on = ctx.knobs.pl_on;
    const double pl_q_amb = ctx.knobs.pl_q_amb;
    const int    pl_warmup = ctx.knobs.pl_warmup;
    const double pl_max_gap_s = ctx.knobs.pl_max_gap_s;
    const double pl_var_floor = ctx.knobs.pl_var_floor;
    const bool   pl_local_block = ctx.knobs.pl_local_block; // station-wide whitened local block at inject (F3)
    const double pb_var_floor = ctx.knobs.pb_var_floor;
    const double cb_var_floor = ctx.knobs.cb_var_floor;
    const bool   zwd_on = ctx.knobs.zwd_on;
    const double zwd_q = ctx.knobs.zwd_q;
    const bool   rx_cb_on = ctx.knobs.rx_cb_on;
    const double rx_cb_q = ctx.knobs.rx_cb_q;
    const bool   clk_model_on = ctx.knobs.clk_model_on;
    const double clk_model_q1 = ctx.knobs.clk_model_q1;
    const double clk_model_q2 = ctx.knobs.clk_model_q2;
    const double clk_model_k = ctx.knobs.clk_model_k;
    const double clk_model_max_gap = ctx.knobs.clk_model_max_gap;
    const bool   ura_on = ctx.knobs.ura_on;
    const double ura_alpha = ctx.knobs.ura_alpha;
    const bool   ura_retro = ctx.knobs.ura_retro;
    const double ura_refloat_inflate = ctx.knobs.ura_refloat_inflate;
    const bool   emit_clock_disc = ctx.knobs.emit_clock_disc;
    const bool   sticky_pivot = ctx.knobs.sticky_pivot;
    const bool   scoped_handover = ctx.knobs.scoped_handover;
    const bool   emit_gen_id = ctx.knobs.emit_gen_id;
    const double q_clock = ctx.knobs.q_clock;
    const double q_pb = ctx.knobs.q_pb;
    const double q_cb = ctx.knobs.q_cb;
    const bool   clock_sum_to_zero = ctx.knobs.clock_sum_to_zero;
    const bool   clock_brdc_anchored = ctx.knobs.clock_brdc_anchored;
    const bool   cb_time_linked = ctx.knobs.cb_time_linked;
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond;
    const bool   carry_cpl_tu = ctx.knobs.carry_cpl_tu;

    if (!snap_path.empty()) {
        // Config fingerprint over the datum-relevant fields.  A resume under a changed
        // S-basis or pivot pin, station roster (its order defines the carry's station
        // indices), carry q values, gap bounds or floors, clock-model parameters, or
        // emitter channel config has to cold-start, because the carried state would be
        // read in the wrong frame or under the wrong rules.
        std::string fp;
        auto add = [&fp](const std::string& v) { fp += v; fp += '\x1f'; };
        for (const auto& st : stations) { add(st.name); add(st.is_pivot ? "P" : "-"); }
        add(nc.clock_datum_station); add(nc.clock_datum_satellite);
        // With network.station_marker_to_arp armed, the roster's fixed coordinates are
        // reduced from the monument to the antenna reference point, so every carried
        // quantity that references a station's geometry (the phase-leveling zero-q levels,
        // the carried ambiguities, the local blocks) refers to a different position across
        // the flip.  Appended only when armed, so the fingerprint of a snapshot
        // written with it off is unchanged.
        if (config_.networkStationMarkerToArp()) add("roster:arp");
        // Carrier-phase wind-up model revision.  Unlike the other tokens this is a code
        // constant, not a knob, because what changes the meaning of the carried state is a
        // source fix, not a config.  The earlier dipole sign left +2*phi_wu in every phase
        // observation (about 0.4 cycles of ZD arc range), which the carried ambiguities,
        // the phase-leveling zero-q level and the shared PB/CB carry absorbed.  Resuming a
        // snapshot written under the old convention into a corrected binary would
        // re-reference stiff carries levelled in the superseded frame (standing
        // invariant #1, a stale zero-q value carried past a jump, and invariant #7, a
        // re-coupling edge with nothing re-validating what it reconnects).  The rest of
        // the fingerprint hashes config knobs only, with no code version or binary hash,
        // so this token is what forces a cold start across the fix.  See
        // docs/SPEC_phase_windup_sign.md.
        add(kWindUpYawModelId);
        // The phase-bias gauge realization (single-pivot vs prioritized anchor)
        // defines which station's ambiguities carry the datum; a resume across a
        // toggle change would interpret the carried anchors in the wrong frame.
        add(fmt::format("{}", phase_datum_prioritized));
        // The eq34 chain datum references a different carried-ambiguity frame
        // (per-receiver RPB split plus spanning-tree pins) than the anchor or pivot
        // datums, so a resume across the flip has to cold-start.  Appended only
        // when armed, so a non-eq34 snapshot's fingerprint is unchanged.
        if (phase_datum_eq34) add("phdatum:eq34");
        // The invariant carry snapshots a DD z-store that the legacy carries do not have,
        // so a resume across a carry_invariant flip has to cold-start.
        // Appended only when armed, so the fingerprint of a snapshot without it is
        // unchanged.
        if (carry_invariant) add("carry:inv");
        // PD-safe DD fold.  The carried Pᶻ a pd_safe run writes comes from a different
        // recursion (product-form posterior, equilibrated factorization, eigen-clamp
        // repairs, degenerate blocks rebirthed rather than frozen) and is PD by
        // construction.  Resuming one arm's z-store into the other would mix the two
        // arithmetics inside a single lineage, which is the mid-chain flip the fingerprint
        // exists to prevent.  Appended only when armed, so the fingerprint of a snapshot
        // written with it off is unchanged.  (A snapshot from an off run restored into a
        // pd_safe run is handled separately: the INC-2c restore guard rebirths any non-PD
        // block it finds in the blob.)
        if (carry_invariant_pd_safe) add("carry:pdsafe");
        // network.carry_aware_covariance is left out of the fingerprint on purpose.  It is
        // a read-back knob: its two consumers (dd_sigma_cyc, the log-only AR probe) feed
        // nothing back, so no byte of the snapshot (carry blob, stash, emitter state)
        // changes meaning when it flips.  Fingerprinting it would strand every existing
        // snapshot for a trace column; it follows the loader's fingerprint-safe list of
        // trace-class knobs that stay armable on a warm resume against an untraced
        // snapshot (GnssConfigLoadNetwork.cpp).  A later knob that routes the correction
        // into the phase-leveling stash (the stored prior variance) is a different knob
        // and has to be fingerprinted.
        add(fmt::format("{}", pl_on));           add(fmt::format("{}", pl_q_amb));
        add(fmt::format("{}", pl_warmup));       add(fmt::format("{}", pl_max_gap_s));
        // Local block (F3, network.phase_leveling_local_block).  The carried levels were
        // accumulated under a different prior (joint against independent rows), and the
        // harvest block itself is not in the snapshot, so a resume across the flip has to
        // cold-start, as for pl_var_floor.  Appended only when armed, so the fingerprint
        // of a snapshot written with it off is unchanged.
        if (pl_local_block) add("pl:losblock");
        // ZWD in the local block (B9-in-block, network.carry_zwd_in_block) affects the
        // solve for the same reason F3 does: the carried ZWD and the carried levels were
        // accumulated under a different prior geometry (one joint Σ over {ambiguities,
        // ZWD} against a block plus an independent diagonal row), so a warm resume cannot
        // cross the flip.  The harvest block is not snapshotted either way.  Appended only
        // when armed, beside the F3 tag, so a snapshot written with it off keeps its
        // fingerprint.
        if (config_.networkCarryZwdInBlock()) add("zwd:blk");
        // Iono in the local block (network.carry_iono_in_block) affects the solve for the
        // same reason: the carried levels were accumulated against a prior geometry that
        // did or did not include the station's iono columns in the joint Σ, so a warm
        // resume cannot cross the flip.  The in-block iono carry itself is not snapshotted
        // (it cold-starts), but the levels that resume were shaped by it.  Appended only
        // when armed, beside the F3/ZWD tags, so a snapshot written with it off keeps
        // its fingerprint.  q and the gap bound are included because both change the
        // prior's weight.
        if (config_.networkCarryIonoInBlock()) {
            add("iono:blk");
            add(fmt::format("{}", config_.networkCarryIonoInBlockQM2()));
            add(fmt::format("{}", config_.networkCarryIonoInBlockMaxGapS()));
        }
        // Receiver clock in the local block (network.carry_rx_clock_in_block) affects the
        // solve for the same reason: the carried levels were accumulated against a prior
        // geometry that did or did not include the station's receiver-clock column in the
        // joint Σ, so a warm resume cannot cross the flip.  The clock carry itself is not
        // snapshotted (it cold-starts, which gives one epoch of white-clock behaviour), but
        // the levels that resume were shaped by it.  Appended only when armed, beside the
        // F3/ZWD/iono tags, so a snapshot written with it off keeps its fingerprint.
        // The whole variance ladder is included: q, rate_q, rate_var0 and the ISB flag
        // each change the prior's weight, and jump_k changes which epochs carried anything.
        if (config_.networkCarryRxClockInBlock()) {
            add("rxclk:blk");
            add(fmt::format("{}", config_.networkCarryRxClockQM2()));
            add(fmt::format("{}", config_.networkCarryRxClockMaxGapS()));
            add(fmt::format("{}", config_.networkCarryRxClockRateVar0()));
            add(fmt::format("{}", config_.networkCarryRxClockRateQ()));
            add(fmt::format("{}", config_.networkCarryRxClockJumpK()));
            add(fmt::format("{}", config_.networkCarryRxClockIsb()));
        }
        // Second code channel: adds a receiver inter-signal state per
        // station and band-1 companion, carried by the receiver code-bias carry whose
        // stash is in the snapshot.  A resume across the flip would restore (or fail to
        // restore) receiver code-bias columns that the other side does not have, and the
        // companion code rows that determine them change the local block every epoch, so
        // the knob affects the solve on both counts.  Appended only when armed, beside the
        // F3/ZWD/iono/rxclk tags, so a snapshot written with it off keeps its
        // fingerprint.  The allow-list and the sigma scale are included: the list decides
        // which columns exist, and the scale sets the weight at which their rows were
        // accumulated.
        // All-channel mode decides which columns exist: a receiver
        // inter-signal code state per extra code and per band, a float ambiguity per extra
        // phase, and the shared satellite families those rows feed.  A resume across the
        // flip would restore a carry whose column set the other side does not have, the
        // same argument as for SCC and SID phase 6a.  Appended only when armed, so a
        // snapshot written with it off keeps its fingerprint.
        if (config_.networkChannelModeAll()) add("chanmode:all");
        if (config_.networkSecondCodeChannel()) {
            add("scc:2ndcode");
            for (const auto& sig : config_.networkSecondCodeSignals()) add(sig);
            add(fmt::format("{}", config_.networkSecondCodeSigmaScale()));
        }
        // Second phase channel: adds a float ambiguity per extra
        // arc and feeds an existing satellite phase-bias family from stations that did not
        // support it before, so the carried levels and the shared border in a snapshot
        // mean different things across the flip.  Appended only when armed, as for SCC,
        // so a snapshot written with it off keeps its fingerprint.  The allow-list and the
        // sigma scale are included: the list decides which arcs exist, and the scale sets
        // the weight at which their rows were accumulated.
        if (config_.networkSecondPhaseChannel()) {
            add("sid:2ndphase");
            for (const auto& sig : config_.networkSecondPhaseSignals()) add(sig);
            add(fmt::format("{}", config_.networkSecondPhaseSigmaScale()));
        }
        // Ledger re-entry response: the gap re-entry response keys
        // on the arc ledger's admitted lifecycle, which decides which carried levels
        // survive a break.  A resume across the flip would restore levels that the other
        // side would have refloated, and the ledger is not snapshotted (by design), so the
        // two sides cannot be reconciled afterwards.  Appended only when armed, as for
        // SCC, so a snapshot written with it off keeps its fingerprint.  The no-verdict
        // policy is included because it decides the disposition of the arcs the raw
        // tiers never saw.
        if (config_.networkLedgerReentry()) {
            add("sid:ledger_reentry");
            add(config_.networkLedgerReentryNoVerdict()
                    == LedgerReentryNoVerdict::Refloat ? "refloat" : "bridge");
        }
        // Election memory across a gap.  When armed, a family that
        // the coverage gate erased and that returns at the same generation is re-pinned on
        // its previous incumbent rather than on the station the coverage rank picks, so
        // the carried ambiguities a resumed run restores were accumulated against a
        // different realization of that family's datum.  The drop memo that decides it is
        // not snapshotted (by design), so the two sides cannot be reconciled afterwards.
        // Appended only when armed, as for SCC, so a snapshot written with it off keeps
        // its fingerprint.  network.ledger_screens is not appended: it is an observatory
        // and changes no byte of the solve, the same trace-knob rule the loader's
        // fingerprint-safe list applies.
        if (config_.networkLedgerAnchor()) add("sid:ledger_anchor");
        // The pin pair as the BIA clock-reference pair moves the pinned code combination
        // and so the datum the carried code biases were accumulated in.  Appended only
        // when armed, so a snapshot written with it off keeps its fingerprint.
        if (config_.networkN3cBiaPinPair()) add("n3c:bia_pin_pair");
        // The canonical pair read from the constellation traits changes which observables
        // the admission requires (Galileo becomes screened), so a snapshot written on the
        // other side holds carries the screen never saw.  Appended only when armed.
        if (config_.networkN3cCanonicalPairTraits()) add("n3c:canonical_pair_traits");
        // The tracker pair without the lexical substitute changes which satellites the slip
        // detectors see and what their history holds.  Appended only when armed.
        if (config_.networkN3cBandSignal()) add("n3c:band_signal");
        // The wire's per-signal records carry values the columns do not, so a closed-loop
        // consumer resumed across the flip would hold a different bias set.  Appended only
        // when armed.
        if (config_.networkN3cWirePerSignal()) add("n3c:wire_per_signal");
        // The coverage gate's count of gapped arcs decides which families are dropped and
        // so which carries are erased; the election's restore is fingerprinted above.
        // Appended only when armed.
        if (config_.networkN3cLedgerLifecycle()) add("n3c:ledger_lifecycle");
        add(fmt::format("{}", pl_var_floor));    add(fmt::format("{}", pb_var_floor));
        add(fmt::format("{}", cb_var_floor));    add(fmt::format("{}", zwd_on));
        add(fmt::format("{}", zwd_q));           add(fmt::format("{}", clk_model_on));
        add(fmt::format("{}", clk_model_q1));    add(fmt::format("{}", clk_model_q2));
        add(fmt::format("{}", clk_model_k));     add(fmt::format("{}", clk_model_max_gap));
        // The retired network.emit_clock_rate held this position, always false
        // on a snapshot that can resume today; the constant keeps every stored
        // fingerprint written with the key off unchanged.
        add("false");
        add(fmt::format("{}", ura_on));          add(fmt::format("{}", ura_alpha));
        add(fmt::format("{}", ura_retro));       add(fmt::format("{}", ura_refloat_inflate));
        add(fmt::format("{}", emit_clock_disc));
        add(fmt::format("{}", config_.networkEmitterStalenessHorizonS()));
        add(fmt::format("{}", sticky_pivot));    add(fmt::format("{}", scoped_handover));
        // Handover S-transform (queue 1(b)): it changes which carried priors fold at a
        // pivot switch, so a resume across a toggle change would read a carry that was
        // (or was not) re-expressed in the wrong datum.  Appended only when armed, as for
        // phase_datum_eq34, so the off path keeps its fingerprint.  When armed, the
        // lineage needs a fresh convergence.
        if (ctx.knobs.handover_stransform) add("sts:1");
        // The transform before the coupling rows changes the handover
        // epoch's rows (armed-only token, so every existing fingerprint is unchanged).
        if (ctx.knobs.handover_stransform_before_rows) add("stsrows:1");
        // The exact S P S^T and the anchor kept across the handover change the
        // handover epoch's fold and the anchor's state (armed-only tokens, so every existing
        // fingerprint is unchanged).
        if (ctx.knobs.handover_stransform_dense) add("stsdense:1");
        if (ctx.knobs.cta_datum_keep) add("ctakeep:1");
        // The re-base mechanisms also move their owed sites, which
        // changes the carry they hand on (armed-only token, so every existing fingerprint is
        // unchanged).
        if (ctx.knobs.rebase_complete) add("rbc:1");
        add(fmt::format("{}", emit_gen_id));
        add(fmt::format("{}", config_.estimateNetworkSatCodeBias()));
        add(fmt::format("{}", config_.estimateNetworkIonoSh()));
        // The receiver code-bias datum (RXCB-DATUM) changes which receiver-DCB combination
        // is realized, so the carried rx-CB levels mean something different with it armed.
        // Appended only when armed, like the others.
        if (config_.networkRxCodeBiasDatum())
            add(fmt::format("rxcbdatum:{}", config_.networkRxCodeBiasDatumSigmaM()));
        // The receiver extra-band code delay estimated: the receiver IFB columns
        // join the local block and the carried receiver code-bias set, so a snapshot written
        // with them held carries a different state set.  Appended only when the knob turned
        // the drop off (the rule's own flag), so a snapshot written without it keeps its
        // fingerprint.
        if (ctx.param_inventory.rx_ifb.estimated_by_knob) add("rxifbest:1");
        add(fmt::format("{}", q_clock)); add(fmt::format("{}", q_pb)); add(fmt::format("{}", q_cb));
        add(fmt::format("{}", nc.warmup_ura_floor_m)); add(fmt::format("{}", nc.warmup_epochs));
        // The phase nuisance state changes what every carried ambiguity level
        // means (the colour is no longer in it), and its own carry is not snapshotted, so a
        // warm resume cannot cross the flip.  Appended only when armed.
        if (nc.phase_colour_state)
            add(fmt::format("pcol:{}:{}:{}:{}", nc.phase_colour_sigma_zen_m, nc.phase_colour_tau_s,
                            nc.phase_colour_elev_floor_deg, nc.phase_colour_max_gap_s));
        // rx-CB carry: appended only when armed (the shl2/iono2 pattern), so a snapshot
        // written with it off keeps its fingerprint.
        if (rx_cb_on) {
            add("rxcb:1"); add(fmt::format("{}", rx_cb_q));
        }
        // The clock anchor changes the temporal model of the carried satellite clock: a
        // snapshot written by an anchored run carries variances produced by a different
        // process, and its per-satellite anchored/loose state is not carried across on
        // purpose (a warm resume is an invariant #1 event, so every satellite re-earns the
        // anchor).  Mixing the two inside one lineage would fold a stiffness without the
        // evidence behind it.  Appended only when armed, so a snapshot written with it off
        // keeps its fingerprint.
        if (ctx.knobs.cta_on)
            add(fmt::format("cta:{}:{}:{}:{}:{}:{}:{}:{}", ctx.knobs.cta_q,
                            ctx.knobs.cta_var_floor, ctx.knobs.cta_max_gap_s,
                            ctx.knobs.cta_k, ctx.knobs.cta_abs_m,
                            ctx.knobs.cta_sigma_floor_m, ctx.knobs.cta_recouple_ep,
                            ctx.knobs.cta_resolve));
        // Gauge-free fold (§2.1): a snapshot written by a gauge-free run holds a carry whose
        // clock information is in a different subspace, so a resume cannot cross the
        // flip.  Appended only when armed, so a plain-anchor lineage keeps its fingerprint.
        if (ctx.knobs.carry_gauge_free) add("cta:gf");
        // Guard statistic (§2.1 bisection): the denominator decides which epochs trip, and a
        // trip withdraws a prior and re-solves the epoch, so two runs that differ only here
        // produce different carried state and cannot resume across each other.  Appended
        // only when the resolved pair differs from what `auto` would choose, so a lineage
        // on `auto` keeps its fingerprint.
        if (ctx.knobs.cta_on
            && (ctx.knobs.cta_stat_proj_post != ctx.knobs.cta_gauge_free
                || ctx.knobs.cta_stat_proj_prior))
            add(fmt::format("cta:gfstat:{}:{}", ctx.knobs.cta_stat_proj_post ? 1 : 0,
                            ctx.knobs.cta_stat_proj_prior ? 1 : 0));
        // The phase-space guard decides trips, which changes the carried
        // state; the shadow mode is trace-only.  Appended only in phase mode.
        if (ctx.knobs.cta_on && ctx.knobs.cta_space == 2)
            add(fmt::format("cta:space:phase:{:.4f}", ctx.knobs.cta_phi_floor_m));
        // The health rule changes the clock pivot ("pivot") or the satellite set
        // ("exclude"), so a snapshot written with it armed carries a different datum or
        // state.  Conditional append, so every earlier fingerprint is unchanged.
        if (ctx.knobs.sat_health != 0)
            add(fmt::format("hlt:{}", ctx.knobs.sat_health));
        // Carry VCE apply (M2 v2): the variance factor multiplies every carry fold
        // variance, so a carried (value, var) pair means a different prior with it armed
        // than without.  That makes it a fingerprinted knob, unlike the carry_vce monitor,
        // which is fingerprint-free.  The fingerprint is necessary but not sufficient here:
        // both legs of an apply-on chain fingerprint identically while α² itself is not
        // snapshotted, so networkTryWarmResume refuses the resume outright (spec §8, the
        // one blocking deferral).  Appended only when armed, so a snapshot written with it
        // off keeps its fingerprint.
        if (config_.networkCarryVceApply()) {
            add("vceapply:1");
            add(config_.networkCarryVceApplyFamilies());
            add(fmt::format("{}", config_.networkCarryVceApplySlew()));
            add(fmt::format("{}", config_.networkCarryVceApplyMax()));
            add(fmt::format("{}", config_.networkCarryVceApplyMinRows()));
            add(fmt::format("{}", config_.networkCarryVceComponents()));
            add(fmt::format("{}", config_.networkCarryVceWindowEpochs()));
        }
        // apriori_source and the anchor σ are datum-relevant.  The carried clock frame is
        // Δδtˢ against broadcast under brdc_rts and Δδtˢ against precise under sp3 (they
        // differ by metres), and only brdc_rts carries the relevel_iode baseline, so a warm
        // resume across an apriori_source change would fold a clock carry in the wrong
        // frame and re-level against a stale baseline.
        add(fmt::format("{}", static_cast<int>(config_.networkAprioriSource())));
        add(fmt::format("{}", config_.networkAprioriClockAnchorSigmaM()));
        // The selection rule defines which ephemeris the carried clock refers to
        // through the offset table, so it is part of the frame.  Appended only under
        // sp3_brdc_clock (whose enum value already differs), so every other fingerprint
        // is unchanged.
        if (config_.networkAprioriBrdcClock())
            add(fmt::format("nbp:sel={}", static_cast<int>(config_.networkBrdcSelect())));
        // The clock-gauge realization defines the frame of the carried clock states (pin,
        // sum-to-zero and broadcast-anchored differ by per-constellation or common-mode
        // constants), so a resume across a realization change has to cold-start.  Appended
        // only when a non-default realization is armed, so a pin-mode snapshot keeps its
        // fingerprint.
        if (clock_sum_to_zero || clock_brdc_anchored)
            add(fmt::format("clkdatum:s2z={},bca={}", clock_sum_to_zero,
                            clock_brdc_anchored));
        // The optional emitter discontinuity channels are construction parameters of the
        // snapshotted emitter state (restoreState fails on a mismatch); appended only when
        // armed, so default fingerprints are unchanged.
        if (config_.networkCodeBiasDiscJumpTest() || config_.networkClockDiscOnSetChange())
            add(fmt::format("emitchan:cbj={},cks={}", config_.networkCodeBiasDiscJumpTest(),
                            config_.networkClockDiscOnSetChange()));
        // The per-channel mark scope decides which channel's counters a
        // landed mark steps, those counters are snapshotted emitter state, and the
        // pending-mark section carries a channel mask.  Appended
        // only when armed, so a snapshot written with it off keeps its fingerprint.
        if (config_.getNetworkConfig().disc_mark_scope == "channel")
            add("discscope=channel");
        // Epoch decimation changes the carried-state cadence, so a resume has to match it
        // (T2e).  Appended only when != 1, so a config without decimation keeps its
        // fingerprint.  The DD-trace stride is trace-only and left out on purpose: arming
        // a trace on resume does not invalidate an untraced snapshot.
        if (nc.epoch_decimation_n != 1)
            add(fmt::format("edecim={}", nc.epoch_decimation_n));
        // The GM deviation (T5) changes the estimation (it is not trace-only), and its
        // carry is not persisted in the snapshot, so a resume across a tau change has to
        // cold-start.
        if (shc.rep_error_tau_s > 0.0)
            add(fmt::format("shdevtau={}", shc.rep_error_tau_s));
        // Dual-layer ionosphere (Iono-2): the second shell adds carried SH coefficients
        // whose meaning depends on its degree and height (a resume across a re-layering
        // would fold layer-fitted coefficients into a different shell geometry).  Appended
        // only when armed, so a single-layer config keeps its fingerprint.
        if (shc.layer2_max_degree > 0)
            add(fmt::format("shl2={}@{}", shc.layer2_max_degree, shc.layer2_height_km));
        // The conditional local carry changes what the per-station carry means (a
        // shared-coupled conditional row against an independent diagonal marginal), so a
        // resume across a toggle change would fold a carry of the wrong kind; that is the
        // reason for the token.  The per-column coupling data (M_r rows, ŝ_prev, P_prev) is
        // persisted, but only into a format-v3 snapshot written under
        // network.state_snapshot_coupling_capture, and only a reader that arms
        // network.resume_coupling_restore rebuilds it (§18.12); a v1/v2 file carries none
        // of it, and a restored row degrades to a plain diagonal.  The token stays in
        // either case (removing it would strand every stored seed).  Appended only when
        // armed, so a snapshot written with it off keeps its fingerprint.
        if (carry_cpl_cond)
            add(fmt::format("carrycplcond={}", carry_cpl_cond));
        // The time-linked code-bias datum changes what the emitted OSB/clock split means (a
        // time-continuity datum against a per-epoch set-mean datum), and its
        // per-(const,signal) previous-value map is persisted only under
        // network.state_snapshot_datum_capture, so a resume across a toggle change would
        // compare a solve in one datum against a carry in the other.  Appended only when
        // armed, so a snapshot written with it off keeps its fingerprint.
        if (cb_time_linked)
            add(fmt::format("cbtimelink={}", cb_time_linked));
        // Pair continuity changes which satellites re-seed their clock at a pin
        // pair change, and the family difference changes which combination realizes the
        // inter-family code offset, so a carried code-bias map was realized under a
        // different datum across either flip.  Armed-only tokens, so every existing
        // fingerprint is unchanged.
        if (config_.networkCodeDatumPairContinuity()) add("cbpaircont=1");
        if (config_.networkCodeDatumFamilyDifference()) add("cbfamdiff=1");
        // The first-sighting family prior changes the carried code biases
        // of every satellite born beside its family partner, by an amount set by sigma, so
        // the token carries the value.  Armed-only, as for the two arms above.
        if (config_.networkCodeFamilyBirthPriorSigmaM() > 0.0)
            add(fmt::format("cbfambirth={:g}", config_.networkCodeFamilyBirthPriorSigmaM()));
        // The border time update changes what an injected coupling row means (a κ-shrunk,
        // re-independenced pseudo-observation of the moved border against the raw
        // harvest-epoch row), and the κ inputs (per-column P_prev and the q ladder) are not
        // persisted in the v1 snapshot.  Appended only when armed, so a snapshot written
        // with it off keeps its fingerprint.
        if (carry_cpl_tu)
            add(fmt::format("carrycpltu={}", carry_cpl_tu));
        // The exact update changes every injected row's coefficients and variance
        // again, on the same footing; appended only when armed.
        if (ctx.knobs.carry_cpl_exact)
            add(fmt::format("carrycplexact={}", ctx.knobs.carry_cpl_exact));
        // The within-station bracket changes the rows' joint weight; armed only.
        if (ctx.knobs.carry_cpl_exact_cross)
            add(fmt::format("carrycplexactx={}", ctx.knobs.carry_cpl_exact_cross));
        // The pivot-frame clock noise changes every carried clock's prior and the
        // exact update's Q_S; armed-only token.
        if (ctx.knobs.pfq_on)
            add(fmt::format("pfq={}", ctx.knobs.pfq_on));
        // The clock clone changes the rows, the fold and the carry each epoch hands
        // on (the harvest marginalizes the clones); armed-only token, so every existing
        // fingerprint is unchanged.
        if (ctx.knobs.carry_cpl_clone) add("ccl:1");
        // The shared history changes the rows, the fold and the carried
        // variances (the remainder only); armed-only token with its rank.
        if (ctx.knobs.carry_cpl_shx) add(fmt::format("shx:{}", ctx.knobs.shx_rank));
        // The bias and departed clones change the rows and the fold; armed-only
        // tokens.
        if (ctx.knobs.carry_cpl_bclone) add("bcl:a");
        if (ctx.knobs.carry_cpl_dclone) add("bcl:d");
        // The bound raise moves the carried border block; the monitor alone is
        // trace class and stays out of the fingerprint.
        if (ctx.knobs.clm_raise)
            add(fmt::format("clmraise={}", ctx.knobs.clm_raise));
        // RXCB-IN-BLOCK changes what a carried receiver code-bias entry is (a block row with
        // a border regression instead of a diagonal prior); appended only when armed.
        if (ctx.knobs.rxcb_blk)
            add(fmt::format("rxcbblk={}", ctx.knobs.rxcb_blk));
        // A drop that decouples at birth strength changes the carried state; appended
        // only when armed.
        if (ctx.knobs.amb_drop_decouple_always)
            add("ambdropdecouple=true");
        // Gauge projection and source re-init both change which
        // information an injected coupling row carries: the projection alters the coefficient vector
        // of every truncated row, and the re-init moves whole columns onto the departed route.  Same
        // footing as carry_cpl_tu above; appended only when armed, so existing
        // fingerprints are unchanged.
        if (ctx.knobs.carry_cpl_gauge)
            add(fmt::format("carrycplgauge={}", ctx.knobs.carry_cpl_gauge));
        if (ctx.knobs.carry_cpl_reinit)
            add(fmt::format("carrycplreinit={}", ctx.knobs.carry_cpl_reinit));
        // The CB-excursion guard (SPEC_day129_frame_guards phase 2.1) changes what a
        // carried code bias means across an excursion (a reinit-decorrelated state against
        // a stiff continuation), and its pending-bump queue is not snapshotted, so a resume
        // that flips it would fold a carry of the wrong kind and could drop a queued
        // discontinuity without a trace.  Same footing as cbtimelink / carrycpl* above.
        // Appended only when armed, so an unarmed fingerprint is unchanged.
        if (nc.cb_excursion_reinit)
            add(fmt::format("cbexc={},{}", nc.cb_excursion_median_m,
                            nc.cb_excursion_reinit_all_sats));
        // --- Not fingerprinted: the forward-only policy exemption ---
        //
        // The knobs listed below are left out of the fingerprint for one shared reason,
        // recorded here so that the omission is not "fixed" later.
        //
        // A knob is exempt when it is a forward-only policy: it changes what the run does
        // from here on (which rows fold, at what weight, which observables are admitted,
        // which arcs re-tap, what a resume rebuilds), but it re-references no carried
        // quantity retroactively and changes the meaning of no serialized state.  A
        // carried ambiguity, code bias, slant or ZWD is the same quantity in the same frame
        // whether or not a screen decides what feeds it next epoch.  Each such knob's own
        // state is per-epoch or per-resume and is never serialized, so a resumed leg starts
        // with it cold.  That is the conservative direction, because a resume is a window
        // in which no time-difference detector can operate.
        //
        // In practice, a fingerprinted knob cannot be A/B tested on one snapshot; keeping
        // these out lets every off/on validation pair replay from a shared seed under
        // `state_snapshot_require: true`.
        //
        // Exempt families:
        //   eq34 reattach hygiene   eq34_anchor_min_arc_ep, phase_datum_time_linked,
        //                           eq34_fresh_pin_sigma_scale
        //   signal integrity        network.signal_integrity.*
        //   measurement admission   adm_{screen,bound_m,min_sats,heal_ep,max_frac,
        //                           trace} and adm_revet{,_gap_ep,_ep,_census,_hold}
        //   carry hygiene           phase_anchor_carry_reref
        //   slip response           cycle_slip.{relevel_free_epoch,relevel_delta_bridge},
        //                           cycle_slip.gf_arc_jump{,_m,_k,_window,_min_count}
        //                           (its per-arc state is never serialized),
        //                           cycle_slip.if_phase_td_reseed_on_flag (the
        //                           IF-TD gradient chains are never serialized)
        //   resume response         zwd_seam_reseed, resume_frame_restore,
        //                           resume_slip_warmup_ep, resume_coupling_restore,
        //                           resume_d5_warmup_ep,
        //                           resume_datum_restore (RS-3, §21.7),
        //                           resume_emit_restore (RS-5, §21.10),
        //                           resume_station_restore{,_rxcb} (RS-6, §21.13),
        //                           resume_signal_canon_restore (RS-8, §21.16),
        //                           resume_border_restore (RS-7, §21.14),
        //                           resume_rxcb_lin_restore (RS-9, §21.18),
        //                           zwd_seam_reseed_q (RS-10b, §21.21)
        //
        // Notes on individual entries:
        //
        //   adm_revet reads a cross-epoch record, which looks fingerprint-relevant but is
        //   not: `vet_left` / `last_ep` live in the same unserialized record map as
        //   `admitted`, so a resumed leg opens no vet windows and every channel reads as a
        //   first sighting, which is the restore case and is already handled.
        //   (`resume.adm` makes the first restored epoch a screened restore rather
        //   than a birth, the one case the seed-birth tripwire cannot see.)
        //
        //   state_snapshot_datum_capture (RS-3) is a representation question like the
        //   coupling capture below, with one difference: its rows use the existing header
        //   (as the optional var_first field does), so a binary that predates it rejects
        //   the file on the unknown record tag rather than on the header literal.  Both are
        //   this format's documented cold-start signal and both throw under
        //   state_snapshot_require, so the safety is the same; keeping the header stable
        //   lets an RS-3 A/B diff two snapshots and see only the new rows.  The datum
        //   quantities themselves are fingerprinted by the knobs that create them
        //   (phase_datum_eq34, code_datum_time_linked), so a writer/reader mismatch on
        //   their meaning cannot occur.
        //
        //   state_snapshot_coupling_capture is a serialization representation, not a
        //   policy: it adds rows and bumps the header to v3.  That is a question of binary
        //   vintage (a reader that predates it fails the header check and cold-starts), not
        //   of the fingerprint, and the writer's products and traces stay byte-identical
        //   because the capture block reads ctx.carry after the emit and writes only into
        //   `snap`.  The coupling arithmetic tokens those rows depend on are already in fp
        //   above, so a writer/reader mismatch on the coupling arithmetic cannot occur.
        //   The same reasoning covers the unconditional Z-row and frame-row captures and
        //   the RS-5 emit-layer (state_snapshot_emit_capture) and RS-6 station-layer
        //   (state_snapshot_station_capture) captures: both add rows under the existing
        //   header and leave the writer's products and traces byte-identical, and the
        //   quantities themselves are fingerprinted by the knobs that create them
        //   (carry_invariant_emit_rebase for the emit layer;
        //   carry_rx_code_bias and global_vtec_sh.rep_error_tau_s for the two station
        //   carries), so a writer/reader mismatch on their meaning cannot occur.
        //
        //   state_snapshot_signal_capture (RS-8, §21.16) is the same representation
        //   question: CN rows under the existing header, read from the station models after
        //   the emit, with the writer's products and traces byte-identical.  The station
        //   roster (which defines what the row's index means) is already in fp above, so an
        //   index cannot name a different station undetected.  Its own contents
        //   (signal_priorities and the coverage allowlist) are left out on purpose: they are
        //   not in the fingerprint, adding them would strand every stored snapshot, and
        //   a mismatch is visible anyway.  A restored channel that this run does not track
        //   has no observation, which is what the band reads without the restore (a station
        //   that tracks nothing on a band has no canonical for it either way), and the
        //   restore audit reports the disagreement.
        //
        // None of these belongs in the fingerprint: adding one would strand every stored
        // snapshot and rule out the same-snapshot off/on A/B that its validation depends
        // on.  (zwd_on / zwd_q above stay fingerprinted, because the continuous carry
        // affects the solve on both sides of a seam.)
        live_fingerprint = NetworkStateSnapshotIO::fnv1a64(fp);
        // CTU G5: the same fingerprint with the CTU tokens removed, so a run
        // that switches the exact update, the bound raise, the in-block rx-CB carry or the
        // phase-space guard ON can resume from a snapshot written without them (the
        // switch-on seam that G5 grades).  The reverse direction is not accepted:
        // a snapshot written with them holds their state, which a run without them
        // would read under the old rules.  Only under network.ctu_switch_on_resume: the
        // seam failed G5, and the ledger rows (the knobs armed on the goldens) are
        // fresh convergence only.  Off, the twin is 0 and the resume gate compares the
        // live fingerprint alone.
        ctx.carry.live_fingerprint_ctu_off = 0;
        if (config_.networkCtuSwitchOnResume()) {
            std::string off = fp;
            for (const std::string& tok : {std::string("carrycplexact=true"),
                                           std::string("clmraise=true"),
                                           std::string("rxcbblk=true"),
                                           std::string("ambdropdecouple=true")}) {
                const std::string t = tok + '\x1f';
                const auto pos = off.find(t);
                if (pos != std::string::npos) off.erase(pos, t.size());
            }
            const auto cs = off.find("cta:space:phase:");
            if (cs != std::string::npos) {
                const auto end = off.find('\x1f', cs);
                if (end != std::string::npos) off.erase(cs, end - cs + 1);
            }
            ctx.carry.live_fingerprint_ctu_off =
                (off != fp) ? NetworkStateSnapshotIO::fnv1a64(off) : 0;
        }
    }
}

} // namespace GnssProcessor
