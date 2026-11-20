/**
 * @file SnapshotFingerprint.cpp
 * @brief @internal networkSnapshotFingerprint: the configuration fingerprint that a
 *        state snapshot carries and a warm resume checks.
 *
 * One function, called once by the driver (network/driver/Run.cpp) before the warm
 * resume, and directly by unit tests.  With a snapshot path it sets
 * ctx.carry.live_fingerprint and, under network.ctu_switch_on_resume, its twin
 * ctx.carry.live_fingerprint_ctu_off; without one it does nothing.
 *
 * The fingerprint is the 64-bit hash of a token string.  The tokens are the rows of one
 * declared table, kFingerprintTokens: each row has a key (the knob or concept the token
 * stands for), a condition (when the row contributes) and a text (the tokens it appends).
 * The table is walked top to bottom, so the row order is the order of the tokens in the
 * string, and a row's comment records why its knob is a term.  A new term is a new row.
 * A row appended only when armed keeps the fingerprint of every snapshot written with the
 * knob off; a row inserted or reordered changes every stored fingerprint, so rows are added
 * at the position that keeps the existing tokens in their order.  The values of the
 * fingerprints of the goldens are pinned by tests/network/test_snapshot_fingerprint_pins.cpp.
 *
 * Three-place rule (include/network/state/RunContext.hpp): a snapshotted field is added to
 * the fingerprint here, to the restore in WarmResume.cpp and to the write in
 * StateSnapshotWrite.cpp.
 *
 * One of the seven files that define the network run context's functions, split
 * by concern.  The declarations, each naming its file, and the map of the seven files are
 * in include/network/state/RunContext.hpp.
 */

#include "network/state/RunContext.hpp"

#include "config/GnssConfig.hpp"
#include "network/solve/Sbasis.hpp"          // kWindUpYawModelId (wind-up model revision token)
#include "products/NetworkStateSnapshot.hpp" // NetworkStateSnapshotIO::fnv1a64

#include <fmt/format.h>

namespace GnssProcessor {

namespace {

/// The tokens one row appends, in order.
using Tokens = std::vector<std::string>;

/// What a token row reads: the run context (its knobs), the configuration and the station roster.
struct FingerprintScope {
    const NetworkRunContext& ctx;
    const NetworkKnobs& k;                 ///< ctx.knobs
    const GnssConfig& config;
    const NetworkConfig& nc;               ///< the network section of the configuration
    const GlobalVtecShConfig& shc;         ///< the global VTEC SH section
    const std::vector<NetStation>& stations;
};

using Scope = const FingerprintScope&;

/// One row of the token table.
struct FingerprintToken {
    const char* key;                       ///< The knob or concept the token stands for.
    bool (*when)(Scope);                   ///< Whether the row contributes.
    Tokens (*text)(Scope);                 ///< The tokens it appends (each followed by the separator).
};

bool always(Scope) { return true; }

/// A value formatted with the default format, as the tokens of the fixed terms are.
template <class T>
std::string val(const T& v) { return fmt::format("{}", v); }

/**
 * The token table.  The text of every row is formatted exactly as the token it replaced, so
 * the string (and its hash) is unchanged for every configuration.
 */
const FingerprintToken kFingerprintTokens[] = {
    // Config fingerprint over the datum-relevant fields.  A resume under a changed
    // S-basis or pivot pin, station roster (its order defines the carry's station
    // indices), carry q values, gap bounds or floors, clock-model parameters, or
    // emitter channel config has to cold-start, because the carried state would be
    // read in the wrong frame or under the wrong rules.
    {"station roster", always, [](Scope s) {
         Tokens t;
         for (const auto& st : s.stations) { t.push_back(st.name); t.push_back(st.is_pivot ? "P" : "-"); }
         return t;
     }},
    {"network.clock_datum_station", always, [](Scope s) { return Tokens{s.nc.clock_datum_station}; }},
    {"network.clock_datum_satellite", always, [](Scope s) { return Tokens{s.nc.clock_datum_satellite}; }},
    // With network.station_marker_to_arp armed, the roster's fixed coordinates are
    // reduced from the monument to the antenna reference point, so every carried
    // quantity that references a station's geometry (the phase-leveling zero-q levels,
    // the carried ambiguities, the local blocks) refers to a different position across
    // the flip.  Appended only when armed, so the fingerprint of a snapshot
    // written with it off is unchanged.
    {"network.station_marker_to_arp", [](Scope s) { return s.config.networkStationMarkerToArp(); },
     [](Scope) { return Tokens{"roster:arp"}; }},
    // The phase-family table (network.phase_families): a member's phase is read in under its family identity, so
    // its ambiguities, phase biases and chain groups carry another identity than in a run without the row; a
    // snapshot of a two-family run is never resumed into a one-family run, nor the reverse.  One token per row,
    // appended only when armed (card NRF section 109.1, amendment 4).
    {"network.phase_families", always, [](Scope s) {
         Tokens t;
         for (const std::string& row : s.nc.phase_families) t.push_back("phfam:" + row);
         return t;
     }},
    {"network.phase_family_codes",
     [](Scope s) { return s.nc.phase_family_codes && !s.nc.phase_families.empty(); },
     [](Scope) { return Tokens{"phfam_codes:1"}; }},
    // The chain memory section (network.state_snapshot_chain_capture) carries its own fingerprint of the keys that
    // give its lines their meaning, in its header, checked at its restore (network/state/ChainMemory.hpp): neither
    // the capture nor the restore is a term here, so a mismatched pair loses the section, never the warm resume.
    // Carrier-phase wind-up model revision.  Unlike the other tokens this is a code
    // constant, not a knob, because what changes the meaning of the carried state is a
    // source fix, not a config.  The earlier dipole sign left +2*phi_wu in every phase
    // observation (about 0.4 cycles of ZD arc range), which the carried ambiguities,
    // the phase-leveling zero-q level and the shared PB/CB carry absorbed.  Resuming a
    // snapshot written under the old convention into a corrected binary would
    // re-reference stiff carries levelled in the superseded frame, with nothing
    // re-validating what it reconnects.  The rest of
    // the fingerprint hashes config knobs only, with no code version or binary hash,
    // so this token is what forces a cold start across the fix.
    {"wind-up model revision (code constant)", always, [](Scope) { return Tokens{kWindUpYawModelId}; }},
    // The phase-bias gauge realization (single-pivot vs prioritized anchor)
    // defines which station's ambiguities carry the datum; a resume across a
    // toggle change would interpret the carried anchors in the wrong frame.
    {"network.phase_datum_prioritized", always,
     [](Scope s) { return Tokens{val(s.k.phase_datum_prioritized)}; }},
    // The eq34 chain datum references a different carried-ambiguity frame
    // (per-receiver RPB split plus spanning-tree pins) than the anchor or pivot
    // datums, so a resume across the flip has to cold-start.  Appended only
    // when armed, so a non-eq34 snapshot's fingerprint is unchanged.
    {"network.phase_datum_eq34", [](Scope s) { return s.k.phase_datum_eq34; },
     [](Scope) { return Tokens{"phdatum:eq34"}; }},
    // The invariant carry snapshots a DD z-store that the legacy carries do not have,
    // so a resume across a carry_invariant flip has to cold-start.
    // Appended only when armed, so the fingerprint of a snapshot without it is
    // unchanged.
    {"network.carry_invariant", [](Scope s) { return s.k.carry_invariant; },
     [](Scope) { return Tokens{"carry:inv"}; }},
    // PD-safe DD fold.  The carried Pᶻ a pd_safe run writes comes from a different
    // recursion (product-form posterior, equilibrated factorization, eigen-clamp
    // repairs, degenerate blocks rebirthed rather than frozen) and is PD by
    // construction.  Resuming one arm's z-store into the other would mix the two
    // arithmetics inside a single lineage, which is the mid-chain flip the fingerprint
    // exists to prevent.  Appended only when armed, so the fingerprint of a snapshot
    // written with it off is unchanged.  (A snapshot from an off run restored into a
    // pd_safe run is handled separately: the restore guard in InvariantCarryDriver.cpp
    // rebirths any non-PD block it finds in the blob.)
    {"network.carry_invariant_pd_safe", [](Scope s) { return s.k.carry_invariant_pd_safe; },
     [](Scope) { return Tokens{"carry:pdsafe"}; }},
    // network.carry_aware_covariance is left out of the fingerprint on purpose.  It is
    // a read-back knob: its two consumers (dd_sigma_cyc, the log-only AR probe) feed
    // nothing back, so no byte of the snapshot (carry blob, stash, emitter state)
    // changes meaning when it flips.  Fingerprinting it would strand every existing
    // snapshot for a trace column; it follows the loader's fingerprint-safe list of
    // trace-class knobs that stay armable on a warm resume against an untraced
    // snapshot (GnssConfigLoadNetwork.cpp).  A later knob that routes the correction
    // into the phase-leveling stash (the stored prior variance) is a different knob
    // and has to be fingerprinted.
    {"network.phase_leveling", always, [](Scope s) { return Tokens{val(s.k.pl_on)}; }},
    {"network.phase_leveling_q_amb", always, [](Scope s) { return Tokens{val(s.k.pl_q_amb)}; }},
    {"network.phase_leveling_warmup_epochs", always, [](Scope s) { return Tokens{val(s.k.pl_warmup)}; }},
    {"network.phase_leveling_max_gap_s", always, [](Scope s) { return Tokens{val(s.k.pl_max_gap_s)}; }},
    // Local block (network.phase_leveling_local_block).  The carried levels were
    // accumulated under a different prior (joint against independent rows), and the
    // harvest block itself is not in the snapshot, so a resume across the flip has to
    // cold-start, as for pl_var_floor.  Appended only when armed, so the fingerprint
    // of a snapshot written with it off is unchanged.
    {"network.phase_leveling_local_block", [](Scope s) { return s.k.pl_local_block; },
     [](Scope) { return Tokens{"pl:losblock"}; }},
    // ZWD in the local block (network.carry_zwd_in_block) affects the
    // solve for the same reason: the carried ZWD and the carried levels were
    // accumulated under a different prior geometry (one joint Σ over {ambiguities,
    // ZWD} against a block plus an independent diagonal row), so a warm resume cannot
    // cross the flip.  The harvest block is not snapshotted either way.  Appended only
    // when armed, beside the pl:losblock tag, so a snapshot written with it off keeps
    // its fingerprint.
    {"network.carry_zwd_in_block", [](Scope s) { return s.config.networkCarryZwdInBlock(); },
     [](Scope) { return Tokens{"zwd:blk"}; }},
    // Iono in the local block (network.carry_iono_in_block) affects the solve for the
    // same reason: the carried levels were accumulated against a prior geometry that
    // did or did not include the station's iono columns in the joint Σ, so a warm
    // resume cannot cross the flip.  The in-block iono carry itself is not snapshotted
    // (it cold-starts), but the levels that resume were shaped by it.  Appended only
    // when armed, beside the tags above, so a snapshot written with it off keeps
    // its fingerprint.  q and the gap bound are included because both change the
    // prior's weight.
    {"network.carry_iono_in_block", [](Scope s) { return s.config.networkCarryIonoInBlock(); },
     [](Scope s) {
         return Tokens{"iono:blk", val(s.config.networkCarryIonoInBlockQM2()),
                       val(s.config.networkCarryIonoInBlockMaxGapS())};
     }},
    // Receiver clock in the local block (network.carry_rx_clock_in_block) affects the
    // solve for the same reason: the carried levels were accumulated against a prior
    // geometry that did or did not include the station's receiver-clock column in the
    // joint Σ, so a warm resume cannot cross the flip.  The clock carry itself is not
    // snapshotted (it cold-starts, which gives one epoch of white-clock behaviour), but
    // the levels that resume were shaped by it.  Appended only when armed, beside the
    // tags above, so a snapshot written with it off keeps its fingerprint.
    // The whole variance ladder is included: q, rate_q, rate_var0 and the ISB flag
    // each change the prior's weight, and jump_k changes which epochs carried anything.
    {"network.carry_rx_clock_in_block", [](Scope s) { return s.config.networkCarryRxClockInBlock(); },
     [](Scope s) {
         return Tokens{"rxclk:blk", val(s.config.networkCarryRxClockQM2()),
                       val(s.config.networkCarryRxClockMaxGapS()),
                       val(s.config.networkCarryRxClockRateVar0()),
                       val(s.config.networkCarryRxClockRateQ()),
                       val(s.config.networkCarryRxClockJumpK()),
                       val(s.config.networkCarryRxClockIsb())};
     }},
    // All-channel mode decides which columns exist: a receiver
    // inter-signal code state per extra code and per band, a float ambiguity per extra
    // phase, and the shared satellite families those rows feed.  A resume across the
    // flip would restore a carry whose column set the other side does not have, the
    // same argument as for the second code and phase channels below.  Appended only
    // when armed, so a snapshot written with it off keeps its fingerprint.
    {"channel_mode (all)", [](Scope s) { return s.config.networkChannelModeAll(); },
     [](Scope) { return Tokens{"chanmode:all"}; }},
    // Second code channel: adds a receiver inter-signal state per
    // station and band-1 companion, carried by the receiver code-bias carry whose
    // stash is in the snapshot.  A resume across the flip would restore (or fail to
    // restore) receiver code-bias columns that the other side does not have, and the
    // companion code rows that determine them change the local block every epoch, so
    // the knob affects the solve on both counts.  Appended only when armed, beside the
    // tags above, so a snapshot written with it off keeps its
    // fingerprint.  The allow-list and the sigma scale are included: the list decides
    // which columns exist, and the scale sets the weight at which their rows were
    // accumulated.
    {"network.second_code_channel", [](Scope s) { return s.config.networkSecondCodeChannel(); },
     [](Scope s) {
         Tokens t{"scc:2ndcode"};
         for (const auto& sig : s.config.networkSecondCodeSignals()) t.push_back(sig);
         t.push_back(val(s.config.networkSecondCodeSigmaScale()));
         return t;
     }},
    // Constellation-aware second-code row test: decides which code rows were
    // accumulated at the second-code scale (another constellation's rows with the same
    // observation code no longer are), the same argument as for the scale above.
    // Appended only when it acts, so a snapshot written with it off keeps its
    // fingerprint.
    {"network.second_code_system_aware",
     [](Scope s) { return s.config.networkSecondCodeRowsActive() && s.config.networkSecondCodeSystemAware(); },
     [](Scope) { return Tokens{"scc:sysaware"}; }},
    // Second phase channel: adds a float ambiguity per extra
    // arc and feeds an existing satellite phase-bias family from stations that did not
    // support it before, so the carried levels and the shared border in a snapshot
    // mean different things across the flip.  Appended only when armed, as above,
    // so a snapshot written with it off keeps its fingerprint.  The allow-list and the
    // sigma scale are included: the list decides which arcs exist, and the scale sets
    // the weight at which their rows were accumulated.
    {"network.second_phase_channel", [](Scope s) { return s.config.networkSecondPhaseChannel(); },
     [](Scope s) {
         Tokens t{"sid:2ndphase"};
         for (const auto& sig : s.config.networkSecondPhaseSignals()) t.push_back(sig);
         t.push_back(val(s.config.networkSecondPhaseSigmaScale()));
         return t;
     }},
    // Ledger re-entry response: the gap re-entry response keys
    // on the arc ledger's admitted lifecycle, which decides which carried levels
    // survive a break.  A resume across the flip would restore levels that the other
    // side would have refloated, and the ledger is not snapshotted (by design), so the
    // two sides cannot be reconciled afterwards.  Appended only when armed, as above,
    // so a snapshot written with it off keeps its fingerprint.  The no-verdict
    // policy is included because it decides the disposition of the arcs the raw
    // tiers never saw.
    {"network.ledger_reentry", [](Scope s) { return s.config.networkLedgerReentry(); },
     [](Scope s) {
         return Tokens{"sid:ledger_reentry",
                       s.config.networkLedgerReentryNoVerdict() == LedgerReentryNoVerdict::Refloat
                           ? "refloat" : "bridge"};
     }},
    // Election memory across a gap.  When armed, a family that
    // the coverage gate erased and that returns at the same generation is re-pinned on
    // its previous incumbent rather than on the station the coverage rank picks, so
    // the carried ambiguities a resumed run restores were accumulated against a
    // different realization of that family's datum.  The drop memo that decides it is
    // not snapshotted (by design), so the two sides cannot be reconciled afterwards.
    // Appended only when armed, as above, so a snapshot written with it off keeps
    // its fingerprint.  network.ledger_screens is not appended: it is an observatory
    // and changes no byte of the solve, the same trace-knob rule the loader's
    // fingerprint-safe list applies.
    {"network.ledger_anchor", [](Scope s) { return s.config.networkLedgerAnchor(); },
     [](Scope) { return Tokens{"sid:ledger_anchor"}; }},
    // The pin pair as the BIA clock-reference pair moves the pinned code combination
    // and so the datum the carried code biases were accumulated in.  Appended only
    // when armed, so a snapshot written with it off keeps its fingerprint.
    {"network.n3c_bia_pin_pair", [](Scope s) { return s.config.networkN3cBiaPinPair(); },
     [](Scope) { return Tokens{"n3c:bia_pin_pair"}; }},
    // The canonical pair read from the constellation traits changes which observables
    // the admission requires (Galileo becomes screened), so a snapshot written on the
    // other side holds carries the screen never saw.  Appended only when armed.
    {"network.n3c_canonical_pair_traits", [](Scope s) { return s.config.networkN3cCanonicalPairTraits(); },
     [](Scope) { return Tokens{"n3c:canonical_pair_traits"}; }},
    // The tracker pair without the lexical substitute changes which satellites the slip
    // detectors see and what their history holds.  Appended only when armed.
    {"network.n3c_band_signal", [](Scope s) { return s.config.networkN3cBandSignal(); },
     [](Scope) { return Tokens{"n3c:band_signal"}; }},
    // The wire's per-signal records carry values the columns do not, so a closed-loop
    // consumer resumed across the flip would hold a different bias set.  Appended only
    // when armed.
    {"network.n3c_wire_per_signal", [](Scope s) { return s.config.networkN3cWirePerSignal(); },
     [](Scope) { return Tokens{"n3c:wire_per_signal"}; }},
    // The coverage gate's count of gapped arcs decides which families are dropped and
    // so which carries are erased; the election's restore is fingerprinted above.
    // Appended only when armed.
    {"network.n3c_ledger_lifecycle", [](Scope s) { return s.config.networkN3cLedgerLifecycle(); },
     [](Scope) { return Tokens{"n3c:ledger_lifecycle"}; }},
    {"network.phase_leveling_var_floor_m2", always, [](Scope s) { return Tokens{val(s.k.pl_var_floor)}; }},
    {"network.shared_carry_var_floor_pb_m2", always, [](Scope s) { return Tokens{val(s.k.pb_var_floor)}; }},
    {"network.shared_carry_var_floor_cb_m2", always, [](Scope s) { return Tokens{val(s.k.cb_var_floor)}; }},
    {"network.carry_zwd", always, [](Scope s) { return Tokens{val(s.k.zwd_on)}; }},
    {"network.zwd_carry_q", always, [](Scope s) { return Tokens{val(s.k.zwd_q)}; }},
    {"network.clock_model", always, [](Scope s) { return Tokens{val(s.k.clk_model_on)}; }},
    {"network.clock_model_q1", always, [](Scope s) { return Tokens{val(s.k.clk_model_q1)}; }},
    {"network.clock_model_q2", always, [](Scope s) { return Tokens{val(s.k.clk_model_q2)}; }},
    {"network.clock_model_k_sigma", always, [](Scope s) { return Tokens{val(s.k.clk_model_k)}; }},
    {"network.clock_model_max_gap_s", always, [](Scope s) { return Tokens{val(s.k.clk_model_max_gap)}; }},
    // The retired network.emit_clock_rate held this position, always false
    // on a snapshot that can resume today; the constant keeps every stored
    // fingerprint written with the key off unchanged.
    {"retired network.emit_clock_rate", always, [](Scope) { return Tokens{"false"}; }},
    {"network.emit_ura", always, [](Scope s) { return Tokens{val(s.k.ura_on)}; }},
    {"network.ura_ewma_alpha", always, [](Scope s) { return Tokens{val(s.k.ura_alpha)}; }},
    {"network.ura_retro_bump", always, [](Scope s) { return Tokens{val(s.k.ura_retro)}; }},
    {"network.ura_refloat_inflate", always, [](Scope s) { return Tokens{val(s.k.ura_refloat_inflate)}; }},
    {"network.emit_clock_disc", always, [](Scope s) { return Tokens{val(s.k.emit_clock_disc)}; }},
    {"network.emitter_staleness_horizon_s", always,
     [](Scope s) { return Tokens{val(s.config.networkEmitterStalenessHorizonS())}; }},
    {"network.sticky_pivot", always, [](Scope s) { return Tokens{val(s.k.sticky_pivot)}; }},
    {"network.scoped_handover_response", always, [](Scope s) { return Tokens{val(s.k.scoped_handover)}; }},
    // Handover S-transform: it changes which carried priors fold at a
    // pivot switch, so a resume across a toggle change would read a carry that was
    // (or was not) re-expressed in the wrong datum.  Appended only when armed, as for
    // phase_datum_eq34, so the off path keeps its fingerprint.  When armed, the
    // lineage needs a fresh convergence.
    {"network.handover_stransform", [](Scope s) { return s.k.handover_stransform; },
     [](Scope) { return Tokens{"sts:1"}; }},
    // The transform before the coupling rows changes the handover
    // epoch's rows (armed-only token, so every existing fingerprint is unchanged).
    {"network.handover_stransform_before_rows", [](Scope s) { return s.k.handover_stransform_before_rows; },
     [](Scope) { return Tokens{"stsrows:1"}; }},
    // The exact S P S^T and the anchor kept across the handover change the
    // handover epoch's fold and the anchor's state (armed-only tokens, so every existing
    // fingerprint is unchanged).
    {"network.handover_stransform_dense", [](Scope s) { return s.k.handover_stransform_dense; },
     [](Scope) { return Tokens{"stsdense:1"}; }},
    {"network.clock_anchor_datum_keep", [](Scope s) { return s.k.cta_datum_keep; },
     [](Scope) { return Tokens{"ctakeep:1"}; }},
    // The re-base mechanisms also move their owed sites, which
    // changes the carry they hand on (armed-only token, so every existing fingerprint is
    // unchanged).
    {"network.carry_rebase_complete", [](Scope s) { return s.k.rebase_complete; },
     [](Scope) { return Tokens{"rbc:1"}; }},
    {"network.emit_generation_id", always, [](Scope s) { return Tokens{val(s.k.emit_gen_id)}; }},
    {"estimate_sat_code_bias", always, [](Scope s) { return Tokens{val(s.config.estimateNetworkSatCodeBias())}; }},
    {"estimate_iono_sh", always, [](Scope s) { return Tokens{val(s.config.estimateNetworkIonoSh())}; }},
    // The receiver code-bias datum changes which receiver-DCB combination
    // is realized, so the carried rx-CB levels mean something different with it armed.
    // Appended only when armed, like the others.
    {"network.rx_code_bias_datum", [](Scope s) { return s.config.networkRxCodeBiasDatum(); },
     [](Scope s) { return Tokens{fmt::format("rxcbdatum:{}", s.config.networkRxCodeBiasDatumSigmaM())}; }},
    // The anchor-level hold chooses the value of every held station level, an
    // inestimable combination a snapshot written without it realized differently, so
    // the carried levels and the phase biases they define do not resume across the
    // flip.  Appended only when armed, like the others; the scope is part of the token.
    {"network.phase_datum_anchor_level_hold", [](Scope s) { return s.k.pd_anchor_level_hold; },
     [](Scope s) { return Tokens{s.k.pd_anchor_level_hold_all ? "aldhold:all" : "aldhold:anchors"}; }},
    // The receiver extra-band code delay estimated: the receiver IFB columns
    // join the local block and the carried receiver code-bias set, so a snapshot written
    // with them held carries a different state set.  Appended only when the knob turned
    // the drop off (the rule's own flag), so a snapshot written without it keeps its
    // fingerprint.
    {"network.estimate_rx_extra_band_code_delay", [](Scope s) { return s.ctx.param_inventory.rx_ifb.estimated_by_knob; },
     [](Scope) { return Tokens{"rxifbest:1"}; }},
    {"process_noise.sat_clock_q", always, [](Scope s) { return Tokens{val(s.k.q_clock)}; }},
    {"process_noise.sat_phase_bias_q", always, [](Scope s) { return Tokens{val(s.k.q_pb)}; }},
    {"process_noise.sat_code_bias_q", always, [](Scope s) { return Tokens{val(s.k.q_cb)}; }},
    {"warmup_ura_floor_m", always, [](Scope s) { return Tokens{val(s.nc.warmup_ura_floor_m)}; }},
    {"warmup_epochs", always, [](Scope s) { return Tokens{val(s.nc.warmup_epochs)}; }},
    // The phase nuisance state changes what every carried ambiguity level
    // means (the colour is no longer in it), and its own carry is not snapshotted, so a
    // warm resume cannot cross the flip.  Appended only when armed.
    {"network.phase_colour_state", [](Scope s) { return s.nc.phase_colour_state; },
     [](Scope s) {
         return Tokens{fmt::format("pcol:{}:{}:{}:{}", s.nc.phase_colour_sigma_zen_m, s.nc.phase_colour_tau_s,
                                   s.nc.phase_colour_elev_floor_deg, s.nc.phase_colour_max_gap_s)};
     }},
    // rx-CB carry: appended only when armed, so a snapshot
    // written with it off keeps its fingerprint.
    {"network.carry_rx_code_bias", [](Scope s) { return s.k.rx_cb_on; },
     [](Scope s) {
         Tokens t{"rxcb:1", val(s.k.rx_cb_q)};
         // The extra-band delay's own noise: appended only when set, so a snapshot written without it keeps
         // its fingerprint.
         if (s.k.rx_ebd_q > 0.0) t.push_back(fmt::format("rxebd:{}", s.k.rx_ebd_q));
         return t;
     }},
    // The clock anchor changes the temporal model of the carried satellite clock: a
    // snapshot written by an anchored run carries variances produced by a different
    // process, and its per-satellite anchored/loose state is not carried across on
    // purpose (a warm resume is a jump event for it, so every satellite re-earns the
    // anchor).  Mixing the two inside one lineage would fold a stiffness without the
    // evidence behind it.  Appended only when armed, so a snapshot written with it off
    // keeps its fingerprint.
    {"network.clock_temporal_anchor", [](Scope s) { return s.k.cta_on; },
     [](Scope s) {
         return Tokens{fmt::format("cta:{}:{}:{}:{}:{}:{}:{}:{}", s.k.cta_q,
                                   s.k.cta_var_floor, s.k.cta_max_gap_s,
                                   s.k.cta_k, s.k.cta_abs_m,
                                   s.k.cta_sigma_floor_m, s.k.cta_recouple_ep,
                                   s.k.cta_resolve)};
     }},
    // Gauge-free fold: a snapshot written by a gauge-free run holds a carry whose
    // clock information is in a different subspace, so a resume cannot cross the
    // flip.  Appended only when armed, so a plain-anchor lineage keeps its fingerprint.
    {"network.clock_anchor_gauge_free", [](Scope s) { return s.k.carry_gauge_free; },
     [](Scope) { return Tokens{"cta:gf"}; }},
    // Guard statistic: the denominator decides which epochs trip, and a
    // trip withdraws a prior and re-solves the epoch, so two runs that differ only here
    // produce different carried state and cannot resume across each other.  Appended
    // only when the resolved pair differs from what `auto` would choose, so a lineage
    // on `auto` keeps its fingerprint.
    {"network.clock_temporal_anchor guard statistic",
     [](Scope s) {
         return s.k.cta_on
             && (s.k.cta_stat_proj_post != s.k.cta_gauge_free || s.k.cta_stat_proj_prior);
     },
     [](Scope s) {
         return Tokens{fmt::format("cta:gfstat:{}:{}", s.k.cta_stat_proj_post ? 1 : 0,
                                   s.k.cta_stat_proj_prior ? 1 : 0)};
     }},
    // The phase-space guard decides trips, which changes the carried
    // state; the shadow mode is trace-only.  Appended only in phase mode.
    {"network.clock_temporal_anchor phase space", [](Scope s) { return s.k.cta_on && s.k.cta_space == 2; },
     [](Scope s) { return Tokens{fmt::format("cta:space:phase:{:.4f}", s.k.cta_phi_floor_m)}; }},
    // The health rule changes the clock pivot ("pivot") or the satellite set
    // ("exclude"), so a snapshot written with it armed carries a different datum or
    // state.  Conditional append, so every earlier fingerprint is unchanged.
    {"network.sat_health", [](Scope s) { return s.k.sat_health != 0; },
     [](Scope s) { return Tokens{fmt::format("hlt:{}", s.k.sat_health)}; }},
    // The coverage election changes which satellite is the clock pivot, so a snapshot
    // written with it armed carries a different datum; the horizon and step change the
    // ranking. Conditional append, so every earlier fingerprint is unchanged.
    {"network.pivot_election", [](Scope s) { return s.k.pivot_election != 0; },
     [](Scope s) {
         return Tokens{fmt::format("piv:{}:{:.0f}:{:.0f}", s.k.pivot_election,
                                   s.k.pivot_election_horizon_s, s.k.pivot_election_step_s)};
     }},
    // Carry VCE (variance component estimation) apply: the variance factor multiplies every carry fold
    // variance, so a carried (value, var) pair means a different prior with it armed
    // than without.  That makes it a fingerprinted knob, unlike the carry_vce monitor,
    // which is fingerprint-free.  The fingerprint is necessary but not sufficient here:
    // both legs of an apply-on chain fingerprint identically while α² itself is not
    // snapshotted, so networkTryWarmResume refuses the resume outright.  Appended
    // only when armed, so a snapshot written with it off keeps its fingerprint.
    {"network.carry_vce_apply", [](Scope s) { return s.config.networkCarryVceApply(); },
     [](Scope s) {
         return Tokens{"vceapply:1",
                       s.config.networkCarryVceApplyFamilies(),
                       val(s.config.networkCarryVceApplySlew()),
                       val(s.config.networkCarryVceApplyMax()),
                       val(s.config.networkCarryVceApplyMinRows()),
                       val(s.config.networkCarryVceComponents()),
                       val(s.config.networkCarryVceWindowEpochs())};
     }},
    // apriori_source and the anchor σ are datum-relevant.  The carried clock frame is
    // Δδtˢ against broadcast under brdc_rts and Δδtˢ against precise under sp3 (they
    // differ by metres), and only brdc_rts carries the relevel_iode baseline, so a warm
    // resume across an apriori_source change would fold a clock carry in the wrong
    // frame and re-level against a stale baseline.
    {"network.apriori_source", always,
     [](Scope s) { return Tokens{val(static_cast<int>(s.config.networkAprioriSource()))}; }},
    {"network.apriori_clock_anchor_sigma_m", always,
     [](Scope s) { return Tokens{val(s.config.networkAprioriClockAnchorSigmaM())}; }},
    // The selection rule defines which ephemeris the carried clock refers to
    // through the offset table, so it is part of the frame.  Appended only under
    // sp3_brdc_clock (whose enum value already differs), so every other fingerprint
    // is unchanged.
    {"network.brdc_select", [](Scope s) { return s.config.networkAprioriBrdcClock(); },
     [](Scope s) { return Tokens{fmt::format("nbp:sel={}", static_cast<int>(s.config.networkBrdcSelect()))}; }},
    // The tracked a priori puts the carried clock on the frame a + U: a resume across the
    // knob would fold a clock carry in the other frame.  Appended only when armed.
    {"network.apriori_clock_track",
     [](Scope s) { return s.config.networkAprioriBrdcClock() && s.config.networkAprioriClockTrack(); },
     [](Scope) { return Tokens{"brdtrack"}; }},
    // The clock-gauge realization defines the frame of the carried clock states (pin,
    // sum-to-zero and broadcast-anchored differ by per-constellation or common-mode
    // constants), so a resume across a realization change has to cold-start.  Appended
    // only when a non-default realization is armed, so a pin-mode snapshot keeps its
    // fingerprint.
    {"network.clock_datum_sum_to_zero, network.clock_datum_broadcast_anchored", [](Scope s) { return s.k.clock_sum_to_zero || s.k.clock_brdc_anchored; },
     [](Scope s) {
         return Tokens{fmt::format("clkdatum:s2z={},bca={}", s.k.clock_sum_to_zero, s.k.clock_brdc_anchored)};
     }},
    // The optional emitter discontinuity channels are construction parameters of the
    // snapshotted emitter state (restoreState fails on a mismatch); appended only when
    // armed, so default fingerprints are unchanged.
    {"network.code_bias_disc_jump_test, network.clock_disc_on_set_change",
     [](Scope s) { return s.config.networkCodeBiasDiscJumpTest() || s.config.networkClockDiscOnSetChange(); },
     [](Scope s) {
         return Tokens{fmt::format("emitchan:cbj={},cks={}", s.config.networkCodeBiasDiscJumpTest(),
                                   s.config.networkClockDiscOnSetChange())};
     }},
    // The per-channel mark scope decides which channel's counters a
    // landed mark steps, those counters are snapshotted emitter state, and the
    // pending-mark section carries a channel mask.  Appended
    // only when armed, so a snapshot written with it off keeps its fingerprint.
    {"network.disc_mark_scope", [](Scope s) { return s.config.getNetworkConfig().disc_mark_scope == "channel"; },
     [](Scope) { return Tokens{"discscope=channel"}; }},
    // Epoch decimation changes the carried-state cadence, so a resume has to match it.
    // Appended only when != 1, so a config without decimation keeps its
    // fingerprint.  The DD-trace stride is trace-only and left out on purpose: arming
    // a trace on resume does not invalidate an untraced snapshot.
    {"network.epoch_decimation_n", [](Scope s) { return s.nc.epoch_decimation_n != 1; },
     [](Scope s) { return Tokens{fmt::format("edecim={}", s.nc.epoch_decimation_n)}; }},
    // The Gauss-Markov deviation changes the estimation (it is not trace-only), and its
    // carry is not persisted in the snapshot, so a resume across a tau change has to
    // cold-start.
    {"estimation.global_vtec_sh.rep_error_tau_s", [](Scope s) { return s.shc.rep_error_tau_s > 0.0; },
     [](Scope s) { return Tokens{fmt::format("shdevtau={}", s.shc.rep_error_tau_s)}; }},
    // Dual-layer ionosphere: the second shell adds carried SH coefficients
    // whose meaning depends on its degree and height (a resume across a re-layering
    // would fold layer-fitted coefficients into a different shell geometry).  Appended
    // only when armed, so a single-layer config keeps its fingerprint.
    {"estimation.global_vtec_sh.layer2_max_degree", [](Scope s) { return s.shc.layer2_max_degree > 0; },
     [](Scope s) { return Tokens{fmt::format("shl2={}@{}", s.shc.layer2_max_degree, s.shc.layer2_height_km)}; }},
    // The conditional local carry changes what the per-station carry means (a
    // shared-coupled conditional row against an independent diagonal marginal), so a
    // resume across a toggle change would fold a carry of the wrong kind; that is the
    // reason for the token.  The per-column coupling data (M_r rows, ŝ_prev, P_prev) is
    // persisted, but only into a format-v3 snapshot written under
    // network.state_snapshot_coupling_capture, and only a reader that arms
    // network.resume_coupling_restore rebuilds it; a v1/v2 file carries none
    // of it, and a restored row degrades to a plain diagonal.  The token stays in
    // either case (removing it would strand every stored seed).  Appended only when
    // armed, so a snapshot written with it off keeps its fingerprint.
    {"network.carry_conditional_coupling", [](Scope s) { return s.k.carry_cpl_cond; },
     [](Scope s) { return Tokens{fmt::format("carrycplcond={}", s.k.carry_cpl_cond)}; }},
    // The time-linked code-bias datum changes what the emitted OSB/clock split means (a
    // time-continuity datum against a per-epoch set-mean datum), and its
    // per-(const,signal) previous-value map is persisted only under
    // network.state_snapshot_datum_capture, so a resume across a toggle change would
    // compare a solve in one datum against a carry in the other.  Appended only when
    // armed, so a snapshot written with it off keeps its fingerprint.
    {"network.code_datum_time_linked", [](Scope s) { return s.k.cb_time_linked; },
     [](Scope s) { return Tokens{fmt::format("cbtimelink={}", s.k.cb_time_linked)}; }},
    // Pair continuity changes which satellites re-seed their clock at a pin
    // pair change, and the family difference changes which combination realizes the
    // inter-family code offset, so a carried code-bias map was realized under a
    // different datum across either flip.  Armed-only tokens, so every existing
    // fingerprint is unchanged.
    {"network.code_datum_pair_continuity", [](Scope s) { return s.config.networkCodeDatumPairContinuity(); },
     [](Scope) { return Tokens{"cbpaircont=1"}; }},
    {"network.code_datum_family_difference", [](Scope s) { return s.config.networkCodeDatumFamilyDifference(); },
     [](Scope) { return Tokens{"cbfamdiff=1"}; }},
    // The first-sighting family prior changes the carried code biases
    // of every satellite born beside its family partner, by an amount set by sigma, so
    // the token carries the value.  Armed-only, as for the two arms above.
    {"network.code_family_birth_prior_sigma_m", [](Scope s) { return s.config.networkCodeFamilyBirthPriorSigmaM() > 0.0; },
     [](Scope s) { return Tokens{fmt::format("cbfambirth={:g}", s.config.networkCodeFamilyBirthPriorSigmaM())}; }},
    // The border time update changes what an injected coupling row means (a
    // κ-shrunk, re-independenced pseudo-observation of the moved border against the raw
    // harvest-epoch row), and the κ inputs (per-column P_prev and the q ladder) are not
    // persisted in the v1 snapshot.  Appended only when armed, so a snapshot written
    // with it off keeps its fingerprint.
    {"network.carry_coupling_time_update", [](Scope s) { return s.k.carry_cpl_tu; },
     [](Scope s) { return Tokens{fmt::format("carrycpltu={}", s.k.carry_cpl_tu)}; }},
    // The exact update changes every injected row's coefficients and variance
    // again, on the same footing; appended only when armed.
    {"network.carry_coupling_exact_update", [](Scope s) { return s.k.carry_cpl_exact; },
     [](Scope s) { return Tokens{fmt::format("carrycplexact={}", s.k.carry_cpl_exact)}; }},
    // The within-station bracket changes the rows' joint weight; armed only.
    {"network.carry_coupling_exact_cross", [](Scope s) { return s.k.carry_cpl_exact_cross; },
     [](Scope s) { return Tokens{fmt::format("carrycplexactx={}", s.k.carry_cpl_exact_cross)}; }},
    // The pivot-frame clock noise changes every carried clock's prior and the
    // exact update's Q_S; armed-only token.
    {"network.clock_pivot_frame_q", [](Scope s) { return s.k.pfq_on; },
     [](Scope s) { return Tokens{fmt::format("pfq={}", s.k.pfq_on)}; }},
    // The clock clone changes the rows, the fold and the carry each epoch hands
    // on (the harvest marginalizes the clones); armed-only token, so every existing
    // fingerprint is unchanged.
    {"network.carry_coupling_clock_clone", [](Scope s) { return s.k.carry_cpl_clone; },
     [](Scope) { return Tokens{"ccl:1"}; }},
    // The shared history changes the rows, the fold and the carried
    // variances (the remainder only); armed-only token with its rank.
    {"network.carry_coupling_shared_history", [](Scope s) { return s.k.carry_cpl_shx; },
     [](Scope s) { return Tokens{fmt::format("shx:{}", s.k.shx_rank)}; }},
    // The bias and departed clones change the rows and the fold; armed-only
    // tokens.
    {"network.carry_coupling_bias_clone", [](Scope s) { return s.k.carry_cpl_bclone; },
     [](Scope) { return Tokens{"bcl:a"}; }},
    {"network.carry_coupling_departed_clone", [](Scope s) { return s.k.carry_cpl_dclone; },
     [](Scope) { return Tokens{"bcl:d"}; }},
    // The bound raise moves the carried border block; the monitor alone is
    // trace class and stays out of the fingerprint.
    {"network.clock_level_bound_raise", [](Scope s) { return s.k.clm_raise; },
     [](Scope s) { return Tokens{fmt::format("clmraise={}", s.k.clm_raise)}; }},
    // The in-block receiver code bias changes what a carried receiver code-bias entry is
    // (a block row with a border regression instead of a diagonal prior); appended only
    // when armed.
    {"network.carry_rx_code_bias_in_block", [](Scope s) { return s.k.rxcb_blk; },
     [](Scope s) { return Tokens{fmt::format("rxcbblk={}", s.k.rxcb_blk)}; }},
    // The in-block receiver phase bias changes what a carried receiver phase-bias entry
    // is, in the same way; appended only when armed.
    {"network.phase_datum_eq34_rpb_in_block", [](Scope s) { return s.k.rpb_blk; },
     [](Scope s) { return Tokens{fmt::format("rpbblk={}", s.k.rpb_blk)}; }},
    // A drop that decouples at birth strength changes the carried state; appended
    // only when armed.
    {"network.amb_float_drop_decouple_always", [](Scope s) { return s.k.amb_drop_decouple_always; },
     [](Scope) { return Tokens{"ambdropdecouple=true"}; }},
    // Gauge projection and source re-init both change which
    // information an injected coupling row carries: the projection alters the coefficient vector
    // of every truncated row, and the re-init moves whole columns onto the departed route.  Same
    // footing as carry_cpl_tu above; appended only when armed, so existing
    // fingerprints are unchanged.
    {"network.carry_coupling_gauge_project", [](Scope s) { return s.k.carry_cpl_gauge; },
     [](Scope s) { return Tokens{fmt::format("carrycplgauge={}", s.k.carry_cpl_gauge)}; }},
    {"network.carry_coupling_reinit_sources", [](Scope s) { return s.k.carry_cpl_reinit; },
     [](Scope s) { return Tokens{fmt::format("carrycplreinit={}", s.k.carry_cpl_reinit)}; }},
    // The code-bias excursion guard (network.cb_excursion_reinit) changes what a
    // carried code bias means across an excursion (a reinit-decorrelated state against
    // a stiff continuation), and its pending-bump queue is not snapshotted, so a resume
    // that flips it would fold a carry of the wrong kind and could drop a queued
    // discontinuity without a trace.  Same footing as cbtimelink / carrycpl* above.
    // Appended only when armed, so an unarmed fingerprint is unchanged.
    {"network.cb_excursion_reinit", [](Scope s) { return s.nc.cb_excursion_reinit; },
     [](Scope s) {
         return Tokens{fmt::format("cbexc={},{}", s.nc.cb_excursion_median_m,
                                   s.nc.cb_excursion_reinit_all_sats)};
     }},
    // The full-equation seeds change the value a new state is born at (on a producer
    // only where a term the legacy seeds leave out is non-zero, as a receiver phase
    // bias or a phase colour is), so a resume across the key would mix states born under
    // two rules.  Appended only when armed, so every existing fingerprint is unchanged.
    {"seed_full_equation", [](Scope s) { return s.config.seedFullEquation(); },
     [](Scope) { return Tokens{"seed:full"}; }},
    // The stations' troposphere gradient columns change every station block the carried
    // states were estimated in, so a resume across the key has to cold-start.  Appended
    // only when armed, so every existing fingerprint is unchanged.
    {"network.estimate_tropo_gradients", [](Scope s) { return s.nc.estimate_tropo_gradients; },
     [](Scope) { return Tokens{"tgrad:1"}; }},
    // Their prior changes the same station blocks; appended only with the gradients armed.
    {"network.tropo_gradient_prior_sigma_m", [](Scope s) { return s.nc.estimate_tropo_gradients; },
     [](Scope s) { return Tokens{fmt::format("tgrad_prior:{}", s.nc.tropo_gradient_prior_sigma_m)}; }},
};

/**
 * @brief Walks the token table and joins the tokens of the rows whose condition holds.
 * @param scope What the rows read.
 * @return The token string: every token followed by the unit separator (0x1f).
 */
std::string fingerprintString(Scope scope) {
    std::string fp;
    for (const FingerprintToken& row : kFingerprintTokens) {
        if (!row.when(scope)) continue;
        for (const std::string& v : row.text(scope)) { fp += v; fp += '\x1f'; }
    }
    return fp;
}

/**
 * @brief The fingerprint of @p fp with the tokens of the switch-on knobs removed, or 0 when none is present.
 * @param fp The token string of the run.
 * @return The twin fingerprint, or 0 when the string holds none of the removed tokens.
 */
std::uint64_t fingerprintWithoutSwitchOnTokens(const std::string& fp) {
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
    return (off != fp) ? NetworkStateSnapshotIO::fnv1a64(off) : 0;
}

} // namespace

void networkSnapshotFingerprint(NetworkRunContext& ctx, const GnssConfig& config_,
                                const std::vector<NetStation>& stations) {
    if (ctx.carry.snap_path.empty()) return;
    const FingerprintScope scope{ctx, ctx.knobs, config_, config_.getNetworkConfig(),
                                 config_.getGlobalVtecShConfig(), stations};
    const std::string fp = fingerprintString(scope);

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
    //   eq34 election family    phase_datum_eq34_family and phase_datum_eq34_family_pairs, like the
    //                           other election knobs
    //                           (phase_datum_eq34_{longevity_election,extend_only,
    //                           slip_aware_election}), none of which is fingerprinted: they
    //                           choose which tree the next election grows, and a frame step
    //                           the new tree puts on a carry goes through the same re-reference
    //                           and shift responses as any other re-host
    //   eq34 event rows         phase_datum_eq34_event_rows: a row's integer rule decides which
    //                           target and step an election gives an edge, not what a stored
    //                           value means.  Its pin life is never serialized, so a resume
    //                           starts every row cold and the row holds its first edges one
    //                           election late.  The returning row's memory of the vertices
    //                           seen in an earlier tree is never serialized either: after a
    //                           resume the first election's absent vertices are births.  The
    //                           rehost row keeps no memory of its own: it reads the incumbent tree of the
    //                           last election.  The root row's pinned receiver phase-bias targets are
    //                           not serialized either (the loader drops the entry when a snapshot is
    //                           written): a resumed leg would pin a root at 0 under carries in the old
    //                           frame.  The slip row reads the one-epoch free-epoch latch and the same
    //                           memory of earlier trees, so after a resume it also reads a first absent
    //                           vertex as a birth.  The merge and split row's memory of the last election's
    //                           trees is not serialized either, and the loader drops its entry with the
    //                           root row's
    //   signal integrity        network.signal_integrity.*
    //   measurement admission   adm_{screen,bound_m,min_sats,heal_ep,max_frac,
    //                           trace} and adm_revet{,_gap_ep,_ep,_census,_hold}
    //   carry hygiene           phase_anchor_carry_reref
    //   slip response           cycle_slip.{relevel_free_epoch,relevel_delta_bridge},
    //                           cycle_slip.gf_arc_jump{,_m,_k,_window,_min_count}
    //                           (its per-arc state is never serialized),
    //                           cycle_slip.if_phase_td_reseed_on_flag (the
    //                           IF-TD gradient chains are never serialized),
    //                           cycle_slip.if_phase_td_sat_clock_{event,min_stations,
    //                           min_fraction,coherence} and emit_pb_hold_trip_generation{,_trigger}
    //                           (each acts only on an event of its own epoch and
    //                           leaves no state behind)
    //   resume response         zwd_seam_reseed, resume_frame_restore,
    //                           resume_slip_warmup_ep, resume_coupling_restore,
    //                           resume_d5_warmup_ep,
    //                           resume_datum_restore,
    //                           resume_emit_restore,
    //                           resume_hold_restore (card CRS2: the hold writes the wire
    //                           only, the solve does not read it),
    //                           resume_station_restore{,_rxcb},
    //                           resume_signal_canon_restore,
    //                           resume_border_restore,
    //                           resume_rxcb_lin_restore,
    //                           zwd_seam_reseed_q
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
    //   state_snapshot_datum_capture is a representation question like the
    //   coupling capture below, with one difference: its rows use the existing header
    //   (as the optional var_first field does), so a binary that predates it rejects
    //   the file on the unknown record tag rather than on the header literal.  Both are
    //   this format's documented cold-start signal and both throw under
    //   state_snapshot_require, so the safety is the same; keeping the header stable
    //   lets an A/B diff two snapshots and see only the new rows.  The datum
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
    //   the emit-layer (state_snapshot_emit_capture), hold (state_snapshot_hold_capture,
    //   card CRS2; the hold is fingerprinted by nothing because it never reaches the
    //   solve) and station-layer
    //   (state_snapshot_station_capture) captures: both add rows under the existing
    //   header and leave the writer's products and traces byte-identical, and the
    //   quantities themselves are fingerprinted by the knobs that create them
    //   (carry_invariant_emit_rebase for the emit layer;
    //   carry_rx_code_bias and global_vtec_sh.rep_error_tau_s for the two station
    //   carries), so a writer/reader mismatch on their meaning cannot occur.
    //
    //   state_snapshot_signal_capture is the same representation
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
    ctx.carry.live_fingerprint = NetworkStateSnapshotIO::fnv1a64(fp);
    // The same fingerprint with the tokens of the exact update, the bound raise, the
    // in-block rx-CB carry, the drop decouple and the phase-space guard removed, so a
    // run that switches them ON can resume from a snapshot written without them (the
    // switch-on seam).  The reverse direction is not accepted:
    // a snapshot written with them holds their state, which a run without them
    // would read under the old rules.  Only under network.ctu_switch_on_resume: the
    // seam failed its grading, and those knobs are
    // fresh convergence only.  Off, the twin is 0 and the resume gate compares the
    // live fingerprint alone.
    ctx.carry.live_fingerprint_ctu_off = 0;
    if (config_.networkCtuSwitchOnResume())
        ctx.carry.live_fingerprint_ctu_off = fingerprintWithoutSwitchOnTokens(fp);
}

} // namespace GnssProcessor
