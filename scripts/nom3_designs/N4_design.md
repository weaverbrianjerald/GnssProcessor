# Card NOM steps N4 and N5: entity tables own the run state, then `EpochEvents` (build design)

- **Steps:** card NOM steps N4 and N5 (`docs/refound/NOM_network_object_model_2026-10-08.md` sections 3.5, 3.6,
  5, 6 and 8), carried by card NOM4 (`docs/refound/NOM4_network_object_model_finish_2026-10-15.md`).
- **Measured at:** branch `nom/wave3` at `4816d1d` (main plus NOM3 steps EP2 to EP4, SG, PB1a/b, HV1 and NOM step
  N3b; suite 3042 run, 3018 passed, 24 fixture-gated skips).
- **Status:** designed and pre-registered; no code. Every default in section 8 is the author's standing default.
- **Appendix:** `N4_fields.tsv` beside this file: one generated row per member of `NetworkCarryState` (364 rows) with
  its key type, proposed entity, scope, persistence, owning sub-step, spread and snapshot role.
- **Evidence:** the survey scripts (`parse.py`, `survey.py`, `snap.py`, `classify.py`, `final.py`, `files.py`) and
  their JSON outputs at `D:\GnssProcessorSupport\nom3_evidence\n4d\`, re-run by each sub-step (section 6.0).

## 1. What the two steps deliver, and the bar

**N4.** The cross-epoch run state leaves the flat `NetworkCarryState` (364 members, 1,658 lines of
`include/app/NetworkRunContext.hpp`) for six entity tables keyed by the typed identities of `common/GnssIdentity.hpp`.
Each moved field declares its persistence; the snapshot writer and reader iterate the declared fields and fill the
same records. One entity per sub-step (section 6).

**N5.** The per-epoch members (83 found) and the one-epoch latches (4) leave the cross-epoch struct for an
`EpochEvents` record built fresh in every `NetworkEpoch`; each seam binds entity tables and its `EpochEvents`
group in place of per-field aliases (236 `auto& x = ctx.carry.x;` bindings in 53 files at the base).

**The bar** (card section 10, decision 3): clarity first, G0 a measurement. Every sub-step is designed to be
byte-identical on the gate set (products without `#` lines, `closed_loop_trace.csv`, `network_epoch_status.csv`,
and `snap.txt` with the UTC field of the M line masked); an unexplained move is not landed.

**Non-goals** (card section 8): no snapshot format change (the DTO `NetworkStateSnapshot` and its `serialize` /
`parse` are not edited); no knob renames; no change of what the solve estimates; no merge with the user-side
`StateManager`; the dense covariance stays in the estimator (an entity holds ids, scalars and indices).

## 2. Survey (measured at `4816d1d`)

**Method.** A parser over the two struct bodies; one tokenization pass over `src/`, `include/`, `tests/` for member
access (`.f`, `->f`), carry bindings (`carry.f`, `cs.f`) and write-like sites; the alias blocks of the snapshot
writer (`app/NetworkStateSnapshotWrite.cpp`) and reader (`app/NetworkWarmResume.cpp`); the record tags from the
parse branches of `products/NetworkStateSnapshot.cpp`. Counts by file are proxies (a name shared with another
struct inflates `files_member_access`; `files_carry_bind` is the tighter column).

### 2.1 The two structs

| struct | members | scalar | map | set | vector | aggregate | files touching |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `NetworkCarryState` | 364 | 168 | 106 | 42 | 22 | 26 | 204 (member access) |
| `NetworkServiceCounters` | 711 | 618 | 17 | 1 | 24 | 51 | 186 |

Spread of the carry members: 102 are touched in one file, 101 in two, 57 in six or more. The most bound:
`local_carry` (38 files), `prev_carry` (25), `relevel` (14), `eq34_reref` (12), `zwd_carry` and `prev_carry_cov`
(11), `pb_hold`, `clk_anchor`, `ccl`, `anchor_incumbent` (10). No member is an unordered container. Three hold a
`unique_ptr` (`slip_oracle`, `oracle_pin`, `rhs_prev_frame`).

### 2.2 Proposed placement (every row in the TSV)

| target | fields | cross-epoch | per-epoch | pending | carry-bind sites | snapshot records |
| --- | --- | --- | --- | --- | --- | --- |
| `ConstellationState` | 11 | 6 | 5 | 0 | 51 | V |
| `ConstellationState::channels` | 7 | 6 | 1 | 0 | 24 | CBP EQD GG |
| `SatelliteState` | 61 | 41 | 15 | 5 | 172 | A C CBI CBS EQS I J K LB QR R SV X Y |
| `SatSignalState` | 15 | 9 | 4 | 2 | 82 | PA PT |
| `StationState` (+ `::signals`) | 13 + 5 | 16 | 1 | 1 | 53 | EQP RC Z |
| `LosState` | 23 | 20 | 3 | 0 | 66 | EQE SD |
| `ArcState` | 12 | 9 | 1 | 2 | 74 | EQR EQT L |
| run remainder: state | 125 | 66 | 53 | 6 | 411 | CA D M QK QP U |
| run remainder: telemetry | 85 | 85 | 0 | 0 | 128 | none |
| run remainder: label-keyed sets | 7 | 7 | 0 | 0 | 8 | none |

Entities: 147 fields (the card estimated about 133); run remainder 217. By owning step (the TSV's `step` column):
N4a 16, N4b 12, N4c 20, N4d 11, N4e 46, N4f 9 (114 entity moves: the cross-epoch fields and the drain queues);
N5a 87 (83 per-epoch plus 4 latches); N4g 163 (the remainder declared in place).

**Confidence.** 267 rows placed by key type (`auto`), 90 placed from the member's comment (`override`: composite
string keys such as `"sat|signal"`, `"<station>|<sat>"`, `"<constellation>|<signal>"`, and `std::string` maps
whose key is a constellation), 7 marked `verify` (`phase_basis`, `tu_reinit_hist`, `anchor_arc_age`,
`eq34_rx_parent`, `eq34_sat_root`, `datum_reref_pb_sats`, `datum_reref_amb_sats`): the owning sub-step reads the
writer of each and confirms its key before moving it.

**Scope.** 50 members are per-epoch by name (`this_epoch_*`, `ep_*`) and 33 more by comment and clear site
(`cxu_epoch`, `ccl`, `shx`, `relevel`, `gl_act_*`, `sts_*`, `bb_hits`, ...); the card estimated about 50. Sixteen are named `*pending*` (the card counted 18 on an earlier base), sorted by drain rule:

| drain rule | members | destination |
| --- | --- | --- |
| until a product carries the mark | `clk_anchor_disc_pending`, `eph_cutover_disc_pending` (record X), `cb_reseed_disc_pending`, `cb_excursion_pending_bump`, `gl2_wire_pending` | `SatelliteState` (N4e): one cause bitmask plus the prune flag (D7) |
| until a birth | `smh_pending`, `smh_block_pending` | `SatSignalState` (N4d) |
| one epoch (a latch) | `carry_innov_pending`, `carry_innov_pending_flat`, `redun_escalate_pending`, `rhs_trip_pending` | N5a: the previous epoch's latches |
| resume hand-off (set by the resume, read at the first leg epoch) | `clm_restore_raise_pending`, `if_pin_holds_resume_pending`, `adm_resume_pending`, `resume_pb_seam_pending`, `resume_cpl_census_pending` | one `ResumeHandoff` group in the remainder (N4g) |

**Persistence** (TSV column): `Record:<tag>` 38 members over 33 tags, `ColdStart` 192, `Epoch` 83,
`ResumeSeeded` 19 (set only by the resume path), `WriterTelemetry` 14 (`snap_*`), `Pending` 6, `ResumeHandoff` 5,
`Latch` 4, `RunConst` 3.

**Records with other owners** (unchanged by N4): the generation id of M, N, most of U (the emitter), FA FP FE
(`run.emit.clk_rebase`), QS QL QZ (the carry driver's coupling dictionary), ER (`run.emit.rebase`), CN and RL (the
station models), the carry and emitter sections. M's fingerprint and U's EWMA fields are carry scalars (N4g).

**`NetworkServiceCounters`** (711) is run telemetry owned by the run-end reports (card NOM step TD1b); G1 names
only `NetworkCarryState`, and N4 moves none of it (its 17 keyed maps, such as `pivot_handovers` and
`clm_raised_by_sat`, stay telemetry). The opposite move is proposed: the 85 telemetry members inside
`NetworkCarryState` (`*_total`, `pf_rows_*`, `sb_*`, `shdev_*`, ...) join it in N4g (D6).

**Name collisions.** `GnssProcessor::ArcState` is the arc ledger's lifecycle enum (`filter/ArcLedger.hpp`:81);
`StationState` is in `filter/NetworkCongruence.hpp`:472, `SatelliteState` in `products/LiveEphemerisStore.hpp`:63.
Hence D1.

**The PRM traversal.** `filter/NetworkCarrySites` declares 26 carry sites over `NetworkCarryState` containers;
each site's accessor is re-pointed by the sub-step that moves its container, so after N4 every site except
`PrevCarry` names an entity field (the card's "traversal ... becomes iteration over fields").

## 3. The entity types in code

New files: `include/app/NetworkEntities.hpp` and `src/core/app/NetworkEntities.cpp` (the declaration tables and the
snapshot fill functions; a manual `CMakeLists.txt` entry). Namespace `GnssProcessor::net` (D1).
`NetworkRunContext` gains `net::NetworkEntities entities;` beside `carry`; each sub-step moves members from `carry`
into `entities`.

```cpp
namespace GnssProcessor::net {

/// How a field survives a restart (card NOM section 3.5); the TSV's persistence column.
enum class Persist : std::uint8_t { Record, ResumeSeeded, ColdStart, Pending, RunConst, WriterTelemetry };

/// An entity's lifecycle: a return is never read as a first sighting (standing invariant 7).
enum class Life : std::uint8_t { Absent, Present, Departed, Returned };
struct Lifecycle {
    Life      life{Life::Absent};
    long long ep_first{-1};   ///< loop epoch of the first sighting
    long long ep_last{-1};    ///< loop epoch of the latest sighting
};

/// One declared field of an entity: one row of the entity's field table.
template <class Table>
struct FieldDecl {
    const char* name;          ///< the member, as spelled in the row struct
    const char* was;           ///< its former NetworkCarryState member (the G1 trail)
    Persist     persist;
    const char* record;        ///< snapshot tag ("K"); "" unless Persist::Record
    const char* fingerprint;   ///< the knob whose fingerprint makes a toggle cold-start, or ""
    void (*to_snapshot)(const Table&, NetworkStateSnapshot&);    ///< Record only
    void (*from_snapshot)(const NetworkStateSnapshot&, Table&);  ///< Record only
    std::size_t (*rows)(const Table&);                           ///< present rows (the reach table)
};

/// Constellations order by RINEX letter, the order of today's char and one-letter keys (O5).
struct ByLetter { bool operator()(System a, System b) const { return rinexLetter(a) < rinexLetter(b); } };

struct ChannelState {                                   // one signal family of a constellation (O4)
    std::optional<int>                     group_id;        // was carry_group_id    Record GG
    std::optional<std::map<SatId, double>> code_bias_prev;  // was code_bias_prev_   Record CBP
    std::optional<std::string>             datum_rx;        // was eq34_datum_rx     Record EQD
    // ddfrac_ref_prev, ddfrac_ref_age (ColdStart); ar_basis once N4b confirms its family key
};
struct ConstellationState {
    std::optional<std::string> prev_pivot, last_pivot;      // Record V
    // cxu_pivot, pfq_pivot (ResumeSeeded), pb_hold_frz_M, sd_ewl_r0 (ColdStart)
    std::map<SignalId, ChannelState> channels;              // (letter, signal) order under O1 and O5
};
struct SatelliteState {
    Lifecycle seen;
    std::optional<ClockOscCarry>            osc;            // was clock_osc         Record K
    std::optional<std::pair<double,double>> prev_clk;       // was prev_clk          Record J
    unsigned disc_pending{0};       // one bit per cause (D7); record X is the EphCutover bit
    bool     wire_prune_pending{false}, c1_zoh_active{false};   // c1_zoh_active: Record C
    // ... the TSV's remaining Satellite rows, values in today's types (D3)
};
// SatSignalState, StationState (+ signals keyed by today's state-key string, O4), LosState, ArcState
// (amb: was local_carry, Record L; N4f2 adds the ledger's ArcRecord): the TSV's rows of each entity.

template <class Id, class Row, class Cmp = std::less<Id>> using Table = std::map<Id, Row, Cmp>;

struct NetworkEntities {
    Table<System, ConstellationState, ByLetter> constellations;
    Table<SatId, SatelliteState>                sats;
    Table<SatSignalId, SatSignalState>          sat_signals;   // (SatId, SignalId), added to GnssIdentity in N4.0
    Table<StationId, StationState>              stations;
    Table<LosId, LosState>                      los;
    Table<ArcId, ArcState>                      arcs;
};

extern const std::vector<FieldDecl<Table<SatId, SatelliteState>>> kSatelliteFields;  // defined in the .cpp
// ... kConstellationFields, kSatSignalFields, kStationFields, kLosFields, kArcFields, kRunFields (N4g)
}  // namespace GnssProcessor::net
```

**Presence.** A member that was a map entry becomes `std::optional<V>` in the row (present exactly when the old map
held the key); a set membership becomes a `bool` or a bit. A row is created by the first write of any of its
fields and is never erased in N4 (D8); erasing an entry resets the field. Plain free functions over a table, no
member behaviour:

```cpp
template <class T, class R, class F> std::size_t present(const T& t, F R::*f);              // replaces .size()/.empty()
template <class T, class R, class F> const F*   get(const T& t, const typename T::key_type& id, F R::*f);  // .find()/.count()
template <class T, class R, class F, class Fn> void forEach(const T& t, F R::*f, Fn fn);   // id order, present rows only
template <class T, class Fn> void byStationName(const T& t, const StationRoster& r, Fn fn); // declared order (O3)
```

**Values.** Keys become ids; values keep their present types in N4 (an identity value such as a pivot satellite
stays a string), which keeps every sub-step mechanical (D3).

## 4. The iteration-order rule

Today every keyed member is a `std::map` / `std::set` over strings, ints or tuples of them, and their iteration
order reaches the output: the inject order of rank-1 updates, the fold order, the lexical reference choices, the
order of trace rows, and the fill order of vector-typed snapshot records.

- **O1, one order.** Every table is an ordered `std::map` keyed by a typed id whose comparator reproduces the
  string order of the key it replaces (N0a: "C01" < "E05" < "G05" < "G10" for satellites; (letter, digit,
  attribute) for signals; roster index for stations; (station, satellite) and (station, satellite, signal) for
  lines of sight and arcs, each proven by the N0a unit tests). A loop over a moved member iterates the table in id
  order and skips rows where the member is absent (`forEach`), so its visit sequence is unchanged.
- **O2, bijection.** Each converter from an old key to an id round-trips (`toString(id(old)) == old`). The arc
  key's signal is a phase code (`arcLifeKeys` uses `obsCode(ObsType::Phase)`), so `SignalId` reproduces its order
  within a satellite; a container mixing code and phase codes uses `ObservableId`; a key that does not round-trip
  (the empty-signal wildcard of `tu_reinit_hist`, the ambiguity-key strings of `amb_flag_ep`) keeps its key (O4).
  The reach table counts round-trip failures per converter: zero on every arm.
- **O3, station names.** `StationId` orders by roster index, and the roster is in configuration order (setup sorts
  only the navigation staging). Members keyed by station NAME (`sh_dev_carry`, `eq34_rx_parent`, the rate-limit
  maps `redun_event_last`, `mdb_event_last`, `adm_event_last`) move to their entity, and every range loop over one
  that reaches output iterates through `byStationName`; lookups need no order. Zero unconverted loops.
- **O4, declared order kept.** A member whose order a typed key cannot reproduce keeps its old key inside the
  entity that owns it, named in its sub-step with the reason. Examples: `code_bias_prev_` stays nested channel then
  satellite under `ConstellationState::channels` (never re-nested satellite then channel); the receiver per-signal
  carries stay keyed by their state-key string under `StationState::signals`.
- **O5, constellations by letter.** `ByLetter`, because the `System` enum puts GLONASS before Galileo (the
  GnssIdentity header note) while today's keys are chars or one-letter strings.
- **O6, snapshot fill.** A map-typed DTO member sorts by its own key on insert, so a map-typed record's line order
  does not depend on the fill order. The vector-typed DTO members fed by `NetworkCarryState` are D (`shared_carry`,
  from `prev_carry`, which stays a vector: D2) and GG (`carry_group_id`, `vector<tuple<char, string, int>>`): the
  new GG fill walks constellations by letter and channels by `SignalId`, equal to today's (char, string) order
  under O1 and O5. Every other vector-typed record (QS, QL, QZ, QK, QP, CN, RL) has an owner N4 does not touch.
- **O7, no fusion.** A loop that iterates one member and looks up others keeps that shape (iterate the table,
  filter on the driving member). No loop is fused across members in N4: fusing changes the visit order where the
  populations differ.
- **O8, range scans.** Every `lower_bound` / `upper_bound` / `equal_range` on a moved member (all arcs of a
  station, all lines of sight of a satellite) is listed and rewritten with minimum ids (`ArcId{sta, SatId::min(),
  SignalId::min()}`); the scan bounds are unit-tested against the string bounds they replace.

**What stays.** `prev_carry`, the D2 stash in fold order, stays one ordered vector in the run remainder (D2): the
fold and inject order are numerically load-bearing, and the card's "clock carry entry and its border index" is an
index into it.

## 5. The snapshot writer and reader

**Plan.** The DTO and its `serialize` / `parse` are not edited. For each moved member, today's block in
`networkWriteStateSnapshot` (an alias line plus a copy loop) becomes the `to_snapshot` function of its `FieldDecl`
row, and today's restore block in `networkTryWarmResume` becomes its `from_snapshot`. The writer calls, entity by
entity, every Record row's `to_snapshot`; its order is free because the DTO is filled first and serialized in its
own fixed order. The reader's order is not free: today's restore blocks have side effects (the `n_resume_*`
counters, `makeRestoredLocalCarry`'s derived values, the restored-arc age seed) and cross-record dependencies. The
reader therefore calls the rows in today's restore order, recorded once as `kRestoreOrder` (a table of
`(entity, row)` pairs), which no sub-step reorders. The non-Record restore work (ResumeSeeded members, the warm-up
counters, the resume hand-offs) stays in `NetworkWarmResume.cpp` where it is.

**Record by record** (the records fed by `NetworkCarryState`; each line order comes from the DTO map, except GG
(fill order, O6) and D (stash order)):

| tag | DTO member | today | after | sub-step |
| --- | --- | --- | --- | --- |
| Z | `zwd_carry` | `zwd_carry` (int) | `StationState::zwd` | N4a |
| EQP | `eq34_rx_parent` | `eq34_rx_parent` (receiver name, verify) | `StationState` | N4a |
| RC | `rx_cb_carry` | `rx_cb_carry` (station, state key) | `StationState::signals` | N4a |
| V | `prev_pivot`, `last_pivot` | same (constellation string) | `ConstellationState` | N4b |
| GG | `carry_group_id` (vector) | `carry_group_id` ((char, signal) to id) | `ChannelState::group_id` | N4b |
| CBP | `code_bias_prev` | `code_bias_prev_` | `ChannelState::code_bias_prev` | N4b |
| EQD | `eq34_datum_rx` | `eq34_datum_rx` | `ChannelState::datum_rx` | N4b |
| EQE | `eq34_prev_el` | `eq34_prev_el` (station, sat) | `LosState` | N4c |
| SD | `sh_dev_carry` | `sh_dev_carry` ("<station>\|<sat>") | `LosState` (O3) | N4c |
| PA, PT | `anchor_incumbent`, `anchor_pin_target` | same ("sat_signal") | `SatSignalState` | N4d |
| K, J | `clock_osc`, `prev_clk` | same | `SatelliteState` | N4e |
| R, A, Y | `ura_retro_sats`, `retro_active`, `ura_refloat_horizon` | same | `SatelliteState` | N4e |
| C | `c1_zoh_active` | same | `SatelliteState` | N4e |
| I, X | `relevel_iode`, `eph_cutover_disc_pending` | same | `SatelliteState` (X from the bit) | N4e |
| SV | `carry_sat_vertex` | same | `SatelliteState` | N4e |
| QR | `clock_refloat_sats` | `prev_clock_refloat_sats` | `SatelliteState` | N4e |
| CBS, CBI | `code_bias_sat_seen`, `code_bias_ifpin_pair` | `code_bias_sat_seen_`, `code_bias_ifpin_pair_` | `SatelliteState` | N4e |
| EQS | `eq34_sat_root` | same (verify) | `SatelliteState` | N4e |
| LB | `level_info` | `clm_info_c`, `clm_n` | `SatelliteState` | N4e |
| L | `local_carry` | `local_carry` | `ArcState::amb` | N4f |
| EQT, EQR | `eq34_prev_target`, `eq34_retained_edges` | same | `ArcState` | N4f |
| D | `shared_carry` | `prev_carry` | run remainder, unchanged (D2) | none |
| CA | `clock_anchor` | `clk_anchor.sat` | run remainder in N4 (D5) | none |
| QK, QP | `carry_cov_keys`, `carry_cov_rows` | `prev_carry_cov` | run remainder | none |
| U, M | the URA EWMA, ramp, first emit; the live fingerprint | scalars | run remainder | none |

**Identity checks per sub-step.** (1) The `snapw` arm's `snap.txt` byte-identical with the M line's UTC masked
(`D:/GnssProcessorSupport/gates/g5/gate_side.sh` already compares it). (2) The resumed arms (`snapr`; HV1's `cplw`,
`cplr`, `zseam`) identical in products and traces. (3) A permanent unit round trip per Record row: a table with
two present rows and one absent, `to_snapshot` then `serialize`, `parse`, `from_snapshot`, compared field by
field. (4) The eight restore suites (`tests/test_resume_*_restore.cpp`, `test_restored_arc_age`) pass, re-pointed. (5) A per-tag line
census of `snap.txt` before and after (equal counts; a cheap diagnosis when the bytes differ).

## 6. The sub-steps (one entity per step, ordered by risk)

### 6.0 Common to every sub-step

**Start.** Re-run the survey on the step's base (the TSV diff names members added or removed since `4816d1d`),
then read the writer of each `verify` row.
**Files.** `include/app/NetworkRunContext.hpp` (remove), `include/app/NetworkEntities.hpp` and
`src/core/app/NetworkEntities.cpp` (add rows and fill functions), the snapshot writer and reader, the seams and
tests listed per step. **Verifier** `scripts/nom_n4_verify.py --entity <name>` (built in N4.0):

- V1 (G1 coverage): each member the step moves is gone from `NetworkCarryState`, present in exactly one entity
  struct and in exactly one `FieldDecl` row with a persistence; the TSV row agrees, or is edited in the same
  commit with its reason.
- V2 (no stale access): zero `carry.<f>`, `cs.<f>`, `ctx.carry.<f>` for moved members, confirmed by grep and by
  the compiler (two tools).
- V3 (order): the list of range loops over moved members, each `forEach` in id order or `byStationName` (O3);
  every scan of O8 rewritten.
- V4 (round trip): zero converter failures in the reach table on every gate arm (O2).
- V5 (parallel): every write site of a moved member classified against PB1's parallel contract; zero row inserts
  inside the parallel produce loop (a data race; HV1's serial harvests stay the only insert points).
- V6 (snapshot): every Record row has both functions; the checks of section 5.
- V7: `python scripts/nom_epoch_lint.py` passes; `tests/test_network_epoch_contract.cpp` unchanged in N4.
- The suite: 3042 run at the base; tests re-pointed, none deleted; only the round-trip tests are added.

**Gates.** G0 on the 23 G5 arms of `D:/GnssProcessorSupport/gates/g5/make_arms.sh` and `make_new_arms.sh` (f2,
s2, zero, joint, bcl, wcff, pdx, vce, dec, trc, shd, snapw, snapr, wkro, wkroci, a4, gate, fcf, raw, hand, legacy,
cul, census) at every sub-step, plus the step's extra arms below, from the existing sets
(`D:/GnssProcessorSupport/gates/{ep4,hv1,n3b,pb1a,pb1b,sg}/make_extra_arms.sh`, 34 arms) and four new ones
(`gates/n4/make_extra_arms.sh`, calibrated on the base's Release exe before they gate, as G4 did):
`sdewl` (`$FG 0.25 network.sd_ewl.enable=true network.sd_ewl.fix_enable=true`), `spike`
(`$FG 0.25 network.raim_arc_persist_epochs=3`), `smh` (`$FG 0.25 network.emit_pb_hold_smoothed=true`), `ddet`
(`$FG 0.25 network.cycle_slip.datum_station_detect=true`). G3: the field golden's `f2` wall within +3 %. The new
run-end log lines (the reach table) are declared to the gate through `NOM_GATE_DROP_RE`.

**Unreached branches** (expected; the reach table confirms them): `sd_ewl_*` without `sdewl`, `arc_spike_streak`
without `spike`, `smh_clone` and `smh_pending` without `smh`, `smh_block` and `smh_block_pending` (their switch is
not among the loader's `emit_pb_hold_*` keys at the base), `datum_reref_*` without `ddet`, `iono_carry` and
`gl_hold_iono` (the retired `network.carry_iono` has no loader key: D9). A member at zero rows on every arm is
identical by construction only (review plus its unit test) and is listed in the step's results.

### 6.1 The steps

| step | moves | records | key risk | spread (bind sites; main files) | extra gate arms |
| --- | --- | --- | --- | --- | --- |
| **N4.0** machinery | none | none | none | new files; `GnssIdentity.hpp` (`SatSignalId` + order test); `kNetworkRunEndReports` (one row: "entities as carried"); `scripts/nom_n4_verify.py` | none (G5 only) |
| **N4a** Station | 16 (11 + 5 per-signal) | Z EQP RC | lowest: int keys, `StationId` is the index order; `eq34_rx_parent` keyed by name (O3) | 53; `NetworkRaim`, `NetworkGrossLos`, `NetworkRedundancyCensus`, `NetworkPrefitCensus`, ... | zwdb zwdc rxclk rxjump eq34x sph ser cplw cplr zseam cbadm sdewl |
| **N4b** Constellation | 12 (6 + 6 channel) | V GG CBP EQD | the pivots drive the handover S-transform (every carried clock's frame); letter order (O5); GG fill order (O6); channels nested (O4) | 75; `NetworkHandoverStransform`, `NetworkBorderStash`, `NetworkCarryDisposition`, `NetworkPbHold`, ... | hand pdx anchor s2z isc eq34x fcf cplw cplr zseam sdewl |
| **N4c** Los | 20 | EQE SD | the in-block ionosphere carry's inject order; station-name keys (O3); the gross-LOS holds | 66; `NetworkGrossLos` (21), `NetworkPrefitCensus`, `NetworkPhaseColourCarry`, `NetworkPriorResidual`, ... | ionob lgm lgmt gim fuse eq34x szi spike sdewl (plus G5's shd, census, cul, wcff) |
| **N4d** SatSignal | 11 | PA PT | new `SatSignalId`; composite string keys of several forms ("sat\|signal", "sat_signal", (sat, signal) pairs: O2); the phase-bias hold | 82; `NetworkPbHoldClone`, `NetworkPbHold`, `NetworkPhaseAnchorEpoch`, `NetworkAnchorHygiene`, ... | pdx anchor eq34x isc cplw cplr zseam smh |
| **N4e1** Satellite: clock | about 20 (the clock carries, `clm_*`, `readmit_*`, the URA and retro sets) | K J LB C R A Y | the clock carry's predict and the border prior; 172 bind sites over both halves | `NetworkBorderStash`, `NetworkBorderPrior`, `NetworkClockLevel`, `NetworkSolPrep`, ... | hand k3 weci era a4h vceap cbadm wkro wkroci cplw cplr zseam |
| **N4e2** Satellite: datum and QC | about 26 (`relevel_iode`, the disc queues as one bitmask (D7), `carry_sat_vertex`, the code-bias and IF-pin latches, `gl2_*`, ...) | I X SV QR CBS CBI EQS | the bitmask merge of five drain queues; the IF-pin latches | `NetworkSolPrep`, `NetworkGrossLos`, `NetworkSbasis`, `NetworkSbasisCompletion`, ... | sbc isc raw wkro wkroci eq34x ddet cplw cplr zseam |
| **N4f1** Arc: the carries | 9 (`local_carry`, `gl_hold_amb`, `relevel_pend`, `amb_flag_ep`, ...) | L EQT EQR | highest: `local_carry` is the rank-1 inject order (38 files); arc keys through `ArcId` (O2); range scans per station (O8) | 74; `NetworkGrossLos`, `ApplicationNetworkAssembly`, `SignalRoster`, `NetworkArcLife`, ... | ail lgmt floor stride rxjump cplw cplr zseam sph all rdr lex sorc opin fcf ser szi |
| **N4f2** Arc: the ledger | the `ArcLedger` record map into `ArcState` (card: SID's ledger promoted from observatory to owner) | none | the ledger's forget window versus never-erased rows (D8); `arcLifeStamp` reads one row | `filter/ArcLedger`, `SignalRoster`, `NetworkArcLife` | as N4f1 |
| **N4g** remainder | 163 declared in place; 85 telemetry members to `NetworkServiceCounters` (D6); the five resume hand-offs as `ResumeHandoff`; the seven label-keyed sets declared | D CA QK QP U M (unchanged) | none numeric; churn | `NetworkFrameDiag`, `NetworkShDevVtec`, `NetworkResumeRunEnd`, `NetworkStationBlock`, ... | the full union (23 + 34 + 4 = 61 arms) |

After N4g every one of the 364 members has exactly one `FieldDecl` row (an entity table's or `kRunFields`), so G1
holds for the whole struct, not only the moved members. G2 after N4 (card section 6): the E05 and slip traces of
the card's section 1 are re-walked and their file count reported.

**The reach table** (N4.0, a run-end report row, log lines only): per declared member, its maximum and final
present rows, persistence and record; per converter, the round-trip failures. It is the reader's view of the run
state (card section 3.6) and the evidence that an arm reached a member.

## 7. N5: `EpochEvents` and the seam bindings

**7.1 What moves.** The 83 per-epoch members and the 4 latches (the TSV's `N5a` rows), in two kinds:

- **Events** (flags, sets, verdicts and counts read by later stages and the traces: `this_epoch_*`, `eq34_reref`,
  `phase_anchor_reref`, `gl_act_*`, `gl_emit_*`, `sts_*`, `bb_hits`, `cg_hits`, the `sid_*` epoch outcomes and the
  `ep_*` trace accumulators) go to `struct EpochEvents` in `app/NetworkEpoch.hpp`, one sub-struct per writing stage
  (`EpochEvents::Gross`, `::ChainDatum`, `::CodeDatum`, `::Handover`, `::Raim`, `::Traces`, ...), per the EP1
  contract (an output is an epoch group its seam is listed as a writer of, with its row in
  `tests/test_network_epoch_contract.cpp`). `ep.events` is fresh every epoch because `NetworkEpoch` is.
- **Workspaces** (`cxu_epoch`, `ccl`, `shx`'s per-epoch part, `relevel`, `mdb`, `amb_state_rows`, the
  `pb_hold_*_step` vectors): the `NetworkEpoch` group of their writing stage.
- **Latches** (`carry_innov_pending`, `carry_innov_pending_flat`, `redun_escalate_pending`, `rhs_trip_pending`):
  `EpochEvents::Latches` of the writing epoch; the run keeps one `EpochEvents::Latches prev_latches`
  (`Persist::Pending`, cold on resume as today: none is a record), replaced at exactly the site where today's code
  rewrites the member, so a skipped or withheld epoch leaves it as today does.

**7.2 Verifying "per-epoch".** A member moves only if every read in an epoch follows a write in the same epoch,
or reads the previous epoch through `prev_latches`. Members move in batches by writing stage with G0 per batch; a
member whose move changes the output carried the previous epoch's content and is reclassified cross-epoch
(declared, with the reading site named) rather than forced. `shx` is a known mixed case ("the held compression
and this epoch's history columns"): its held part stays run state.

**7.3 Seam signatures.** EP1 already replaced the card's "75 calls with 807 positional arguments" by
`R networkX(NetworkRun&, const NetworkEpoch&[, outputs...])`; what remains is the binding block, which names
entity tables and the events group instead of per-field aliases:

```cpp
const net::Table<SatId, net::SatelliteState>& sats = run.ctx.entities.sats;   // read
net::Table<ArcId, net::ArcState>&             arcs = run.ctx.entities.arcs;   // written
EpochEvents::Gross&                           gross = gross_out;              // written (output)
```

A seam that binds a table non-const is listed in `kEntityWriters` (`app/NetworkEntities.hpp`: one row per seam,
the tables it writes); `scripts/nom_epoch_lint.py` is extended to fail a non-const binding missing from its row.
**Sub-steps:** N5a (EpochEvents, workspaces, latches; G0 per batch), N5b (the bindings, `kEntityWriters`, the
lint). **Gates:** G0 on the full union of 61 arms; the suite; the lint; G3.

## 8. Decisions for the author (each with its default)

- **D1, names.** Default: namespace `GnssProcessor::net` with the card's six names. Alternative: rename the
  ledger's enum (`ArcPhase`) and keep the global namespace.
- **D2, `prev_carry`.** Default: stays the ordered stash vector in the run remainder; entities hold indices only
  when a reader needs one; the stash's ownership is N6's.
- **D3, values.** Default: keys become ids, values keep their types in N4; typed values are a later pass.
- **D4, lifecycle.** Default: `Lifecycle` is declared on the satellite, station and arc rows; it is filled where an
  owner computes it today (arcs from the ledger in N4f2, stations from the topology and departure state); the
  satellite lifecycle stays `Absent` until a card needs it.
- **D5, aggregates.** Default: the 26 aggregate members (`ClockAnchorState`, `AnchorHandoverLedger`,
  `ArSuspectLedger`, `FixedCycleState`, `NetworkCongruence`, the oracles, ...) stay whole in the run remainder, each
  one owner per concept already; `ClockAnchorState::sat` (the per-satellite Q3 anchor, record CA, listed by the card
  under `SatelliteState`) is queued as N4e3.
- **D6, telemetry.** Default: the 85 telemetry members of `NetworkCarryState` move to `NetworkServiceCounters` in
  N4g.
- **D7, discontinuity queues.** Default: the five drain-on-emission satellite sets become one `disc_pending`
  bitmask (one bit per cause) plus `wire_prune_pending`; record X is written from its bit.
- **D8, rows.** Default: rows are never erased in N4 (memory bounded by the run's identities: about 70
  satellites, 60 stations, 4,000 lines of sight, 16,000 arcs on a 58-station day).
- **D9, dead state.** Default: `iono_carry` and `gl_hold_iono` (the retired diagonal vehicle) are deleted in N4c if
  the reach table shows them empty on every arm (principle 2: a declaration that cannot be wired is deleted), with
  the deletion stated in the step's results.
- **D10, the reach table.** Default: lands in N4.0 and stays (trace-class log lines).
- **D11, order.** Default: N4.0, Station, Constellation, Los, SatSignal, Satellite (two parts), Arc (two parts),
  the remainder, then N5a and N5b.
- **D12, `NetworkServiceCounters`.** Default: not moved by N4 or N5 (outside G1); its keyed maps stay telemetry.

## 9. Risks

- **R1, order.** A key that does not round-trip, orders by station name, or fills GG (O6) is walked at an output
  site. Caught by V3, V4 and G0; fallback O4.
- **R2, concurrency.** `NetworkRunContext.hpp` and the seams are edited daily by other sessions; each sub-step
  lists its files (section 6.1), re-surveys first and lands in a short window.
- **R3, parallel races.** A row insert from the parallel produce loop; V5.
- **R4, restore order.** The reader's side effects and dependencies; `kRestoreOrder` (section 5).
- **R5, cost.** Presence filtering over the arc table (16,000 rows) in per-epoch loops; G3. Mitigation: a present
  count per member kept by the helpers.
- **R6, test churn.** Tests construct carry members directly (about 69 sites in `test_network_shared_history`,
  45 in `test_pb_hold_block`); re-pointed, never deleted.
- **R7, survey precision.** The placement is a proposal from key types and comments; 7 rows are `verify`; the
  per-epoch class of 33 members rests on their comments and clear sites (N5's batch G0 is the test).
- **R8, hidden aliases.** Functions that take a moved container by reference (`arcLifeStampOf`, the
  `NetworkCarrySites` accessors, `NetworkGrossLos.hpp` signatures) change signature; the compiler finds every one.

## 10. Appendix: `N4_fields.tsv`

Tab-separated: four `#` lines, a header, 364 rows. `key_type` is a map's or set's first template argument (a
vector's element type); `entity`, `scope`, `persistence` and `step` are sections 2.2 and 6.1; `snap_writer` /
`snap_reader` are 1 when the writer or reader binds the member; `top_files` names the four files with the most
member accesses (`Network` shortened to `N`).
