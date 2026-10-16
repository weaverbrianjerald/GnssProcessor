# NOM3 step HV1: the local-carry harvest cut by carried kind (buildable design)

Read-only design for card NOM3 step HV1 (`docs/refound/NOM3_network_giants_2026-10-09.md`,
section 3 "HV", gates of section 5, order of section 6). No repository file was changed.
Evidence and scripts beside this file, in `nextsteps/hv1/`: `main.cpp` and `post.cpp` (the
harvest file at main and after the REMOVE-key deletion), `sec.py`, `secm.py`, `names.py`
(the measurements), `hv1_table.py` (the draft canonical table with its checks, output in
`table_check.txt`; `python -I hv1_table.py --controls` exits 0: 1 162 of 1 162 frozen code
lines claimed exactly once with every declared drop subtracted from its piece, every dropped
code line of its declared kind, 23 of 23 anchors resolving to exactly one statement, and five
negative controls failing as required). Scratch checks in `nextsteps/hv1chk/` (`anchors.py`, the
composed post-deletion files in `compose/`).

**Revision 2 (2026-10-14).** Six review findings were checked and accepted; one was wider than
reported. Section 10 lists each with its evidence and the change made.

**Coordinates.** `M:n` is line n of `src/core/app/NetworkLocalCarryHarvest.cpp` at main
`31c393f`. `P:n` is line n of `nextsteps/hv1/post.cpp`: main plus the deletion's preparation
commit `a621b16` plus its five track branches `rmk/{I,L,R1,R2,K}`, composed with
`git merge-file` (one textual conflict, a comment, section 1.2). Other files are cited at main
unless marked. The P coordinates are provisional: the deletion is paused and not on main, so the
generator re-derives every range from anchors on the post-deletion main (the EP1 tooling's rule:
no row carries a line number).

---

## 0. The design in one page

1. **What remains to cut.** `networkHarvestLocalCarries` is 2 602 lines at main (M:121-2722,
   1 657 code lines, 17 parameters, 42 alias declarations). The deletion removes 731 of them
   (495 code lines) and the 70-line LV-5 pre-pass beside it: the lane observatory (rmk/L),
   the diagonal slant ionosphere, LV-1, LV-2, LV-4, LV-5 and S2H (rmk/I), F1 and the young-arc
   counters (rmk/R1). HV1 cuts the remaining 1 871 lines (1 162 code lines, P:45-1915).
2. **Kinds that survive.** Seven carried maps and one ledger: ambiguities (`local_carry`) with the
   anchor handover ledger, phase colour (`pc_carry`), in-block slant ionosphere
   (`iono_blk_carry`), receiver clock with its drift (`rx_clk_carry`, `rx_clk_rate`), receiver
   code bias in block and diagonal (`rx_cb_carry`), ZWD (`zwd_carry`), eq34 receiver phase bias
   (`rpb_carry`). Instruments beside them: the VCE and prior-residual feeds, the ambiguity-state
   rows, the AIL trace, the RXCB-DATUM census, the post-resume coupling census and its latch.
3. **The cut.** 22 functions plus one layout helper, in one spine file and nine owner files,
   called from a two-parameter orchestrator `networkHarvestLocalCarries(NetworkRun&, const
   NetworkEpoch&)` in today's order. Twenty functions (the orchestrator and 19 it calls) have
   EP1's seam form `(NetworkRun&, const NetworkEpoch&, kept...)`; the two post-resume functions
   read no epoch value and take the run only; the layout helper is pure. Each function opens
   with a generated preamble that binds the frozen names; every body is the frozen text,
   shifted, with a closed list of declared transforms (guard, unwrap, extract in place, lambda
   to function, continue to return, the layout reads, one proven merge) and a closed list of
   declared drops, each validated by kind.
4. **The F3 layout as one value.** The eleven per-station hand-forward vectors (P:338-379,
   written at P:471-491, read at eleven lines) become one `std::vector<StationHarvestBlock>`:
   per station the block's columns, each segment's first row and column count in the declared
   `BlockSegment` order (amb, iono, receiver clock, receiver code bias, phase colour, ZWD last),
   and the block.
5. **The counter move.** After the deletion four rx-CB run fields remain in carry state
   (`rx_cb_injected_total`, `rx_cb_innov_skips_total`, `rx_cb_harvested_total`,
   `rx_cb_sta_tally`; `include/app/NetworkRunContext.hpp` 2237-2238, 2253); they move to
   `NetworkServiceCounters` in their own commit. Of the card's 26 counters three survive; the
   other 23 (`lv1_*`, `lv2_*`, `lv4_*`, `lv5_*`, `s2h_*`) go with C19. The per-station tally map,
   not among the 26, is the same family's run record and moves with them.
6. **Gates.** G-R by a new `scripts/nom_hv1_verify.py` built on `scripts/nom_cpp.py` and EP1's
   binding check; G-EP whole run (`scripts/nom_td_gate.py --whole`) on the EP1 core arms, the
   harvest family arms and ten new arms (six branch arms and a four-arm coupling-restore resume
   family, the only arms that reach the post-resume census); one declared output text move (nine
   printed site citations), as step HV0 did.

---

## 1. The measured state

### 1.1 The file and the function at main `31c393f`

`src/core/app/NetworkLocalCarryHarvest.cpp` is 2 724 lines, two functions:
`networkRxCbDemeanPrepass` M:50-119 (70 lines) and `networkHarvestLocalCarries` M:121-2722
(2 602 lines; 1 657 code lines). The card measured 2 577 at `6e39dc4`; the 25 extra lines
arrived before HV0, which moved the body byte for byte (`2fdc636`).

| measure | main `31c393f` | after the deletion (P) |
| --- | --- | --- |
| lines / code lines | 2 602 / 1 657 | 1 871 / 1 162 |
| parameters | 17 (M:121-134) | 17 (P:45-58) |
| alias declarations (one block) | 42 (M:135-227, 93 lines) | 35 (P:59-143, 85 lines) |
| station loops | 12 | 10 |
| `reconstructStation` calls | 9 | 8 |
| diagonal or pure variance calls (comment mentions P:265, P:309 excluded) | 8 | 7 (P:188, 452, 1146, 1261, 1533, 1725, 1819) |
| coupled-column calls (`harvestCoupling(`) | 6 | 6 |
| gap prunes (`carryGapPrune`) | 8 | 7 |
| prior-residual feeds | 4 (pl, zwd, iono, rxcb) | 3 (pl, zwd, rxcb) |
| `continue;` statements (comment mentions P:394, P:1790 excluded) | 105 | 62 |
| early `return` of the function | none | none |
| log calls | 1 (M:2126-2135) | 1 (P:1686-1695) |
| `prmRespond` sites | 11 | 9 |
| distinct `ctx.carry.` fields touched | 58 | 27 |
| distinct `ctx.counters.` fields touched | 79 | 41 |
| direct `config_` reads | 5 | 3 (P:80, 253, 255) |

**Exits.** The function has no early return; it runs to its end on every epoch. Station-level
`continue` statements skip a station: four in the station-loop head (P:381, 385, 402, 453) and
others at the head of each independent per-kind station loop.

**Callers.** One production call, `src/core/filter/NetworkLeveling.cpp` 70-74 (inside
`networkLeveling`, after the solve), and one test, `tests/test_rx_cb_datum_gauge.cpp` 136-139.
Declared at `include/app/NetworkRunContext.hpp` 5265-5280 with a one-line `@brief` and no
`@param`.

**Writes to run state after the deletion** (P coordinates):
- 23 `NetworkCarryState` fields written: nine carried maps (`local_carry`, `handover`,
  `pc_carry`, `iono_blk_carry`, `rx_clk_carry`, `rx_clk_rate`, `rx_cb_carry`, `zwd_carry`,
  `rpb_carry`); six buffers drained elsewhere (`amb_state_rows`, `amb_if_rows`, `pc_trace_rows`,
  `sid_carry_prune_deaths`, `resume_cpl_census_rel`, `resume_cpl_census_rhs`); two seam fields
  (`resume_cpl_census_pending`, `zwd_seam_ep_left`); two harvest clocks (`pl_prev_now_s` P:1031,
  `rxclk_prev_now_s` P:1409); the four rx-CB run fields (P:1509, 1786, 1792, 1803, 1885).
  Read only: `this_epoch_cb_reseed_channels`, `relevel.free_ambs`, `relevel.bridged`,
  `vce_apply`, `eq34_reref`. Through callees: `ctx.carry.shx` (the SCL2 compression).
- About 40 service counters written directly (35 by name, five through `auto& C = ctx.counters;`
  in the receiver-clock gate census), plus `prm_resp` and the clone and history counters
  through callees.
- Station models: `stations[i].sm->clearPendingReseed` (P:872) and `stations[i].l5b.reseed_erase`
  (P:1458, P:1837). The engines `carry_vce_est` and `prires_est` (rows).

### 1.2 How the post-deletion text was obtained, and what to recheck

Each track branch starts at `a621b16` (the deletion's commit C1, which renames the young-arc
stamp call M:928 to `localCarryStampRelevel`). Composition: `git merge-file` of `rmk/I`,
`rmk/L`, `rmk/R1`, `rmk/R2`, `rmk/K` onto `a621b16`'s file. One conflict: M:243, a comment
listing three pending flags, edited by `rmk/I` `ebf0f9e` (C15, drops `dw_release_pending`) and by
`rmk/R2` `3fa1f56` (C29, drops `raim_refloat_pending`); resolved here to
`carry_innov_pending.`. Two tracks end in ungated review rounds (`rmk/I` `6923a8e`,
`rmk/R1` `45e93a7`; the latter edits the comment M:170-173). `rmk/K` changes nothing in this
file beyond `a621b16`. The same composition of `NetworkLeveling.cpp` (263 to 110 lines,
`nextsteps/hv1/lev_post.cpp`) leaves `networkLeveling` as its alias preamble, the harvest call
(lev_post 61-65) and the eq34 re-reference rows (lev_post 74-76).

### 1.3 The function region by region

"Golden" says whether the region is entered on `config/golden/golden_field_refound.json` and
`golden_sim_refound.json`: both arm phase leveling, the coupling carry, the float vehicle, the
in-block rx-CB carry (`carry_rx_code_bias_in_block`), the continuous handover, VCE (`pl,pb,cb`),
the prior-residual census (`pl,pb,cb,zwd,rxcb`), `trace_amb_state` and `rx_code_bias_datum`;
neither arms the ZWD, in-block ionosphere or receiver-clock carries, phase colour, eq34, AIL, or a
snapshot path. After the deletion 578 of the 1 162 code lines sit in regions the goldens enter,
584 in regions they do not.

| id | main | after | lines / code (after) | what | golden | tag |
| --- | --- | --- | --- | --- | --- | --- |
| pre-pass | M:50-119 | (gone) | 70 / 60 (main) | `networkRxCbDemeanPrepass`, LV-5 mean before PASS B | off | **deleted** `rmk/I` `1c2b093` (C19) |
| P0 | M:121-134 | P:45-58 | 14 / 14 | the 17 parameters | live | kept; **rebound by EP1** (networkLeveling's preamble, EP1 Appendix A row 66); replaced by HV1 |
| P1 | M:135-227 | P:59-143 | 85 / 43 | the alias block | live | kept; **deleted** M:139, 179-182, 190 (`633de7a`, C18), M:150 (`18ffc0d`, C22); comments M:170-173 (`4fec9c0`, `45e93a7`), M:200-201 (`74b038e`, C23) |
| H1 | M:229-298 | P:145-214 | 70 / 34 | anchor handover ledger | live | kept; comment M:243 (conflict, 1.2) |
| bracket | M:300-306 | P:216-222 | 7 / 1 | `if (carry_warm) {` | live | kept |
| H2a | M:307-360 | P:223-276 | 54 / 43 | border begin: `col2desc`, `cov_df`, clone and shared-history begin | live | kept |
| H2b | M:361-416 | P:277-332 | 56 / 30 | `harvestCoupling` lambda: comment P:277-283, head P:284-287, body P:288-331, close P:332 | live | kept |
| H2c | M:417-458 | (gone) | 42 / 22 (main) | lane observatory pre-snapshot | off | **deleted** `rmk/L` `9df7baf` (C7) |
| H2d | M:459-505 | P:333-379 | 47 / 30 | eleven hand-forward vectors | sized under block kinds | kept |
| H2e1 | M:506-621 | P:380-495 | 116 / 64 | station-loop head: gates, `cols`, reconstruct, F3 block | live | kept |
| H2e2 | M:622-643 | P:496-517 | 22 / 19 | per ambiguity: coupled or diagonal variance | live | kept |
| H2e3 | M:644-782 | P:518-636 | 119 / 55 | VCE row and prior-residual `pl` row | live | kept; **deleted** F1 mirror M:693-698, 706-714 (`18ffc0d`, C22), retap label M:684-686, 748-749, 768-769, 775 (`9df7baf`, C7) |
| H2e4 | M:783-826 | P:637-680 | 44 / 34 | post-resume coupling census, L rows | off; reached only on a warm resume with `network.resume_coupling_restore` (new arm `cplr`, section 5), not on `snapr` | kept |
| H2e5 | M:827-1002 | P:681-855 | 175 / 86 | the stash: var_first, var_birth, drop walk, birth and relevel stamps, amb-state row, store, history note | live | kept; **deleted** M:931-932 (`4fec9c0`, C21); M:928 renamed (`a621b16`, C1) |
| H2e6 | M:1003-1020 | P:856-873 | 18 / 8 | F3 row a, `clearPendingReseed` | live | kept |
| H2e7 | M:1021-1133 | P:874-986 | 113 / 94 | AIL ionosphere-free variance trace | off | kept |
| H2e8 | M:1134-1174 | P:987-1027 | 41 / 35 | phase-colour stash (LGM) | off | kept |
| H2f | M:1176-1465 | (gone) | 290 / 217 (main) | lane observatory measurement | off | **deleted** `9df7baf` (C7) |
| H2g | M:1466-1468 | P:1029-1031 | 3 / 1 | `pl_prev_now_s` stamp | live | kept (comment rewritten by `9df7baf`) |
| H2h | M:1470-1480 | P:1033-1043 | 11 / 7 | phase-colour prune | off | kept |
| H2i | M:1482-1543 | P:1045-1106 | 62 / 37 | ambiguity gap prune and retirement census | live | kept |
| H2j | M:1544-1546 | (gone) | 3 / 1 (main) | retap set clear | live no-op | **deleted** `9df7baf` (C7) |
| H2k | M:1547-1651 | P:1107-1211 | 105 / 66 | in-block slant ionosphere | off | kept |
| H2l | M:1652-1850 | P:1212-1410 | 199 / 128 | in-block receiver clock, drift filter, jump test | off | kept |
| H2m | M:1851-1952 | P:1411-1512 | 102 / 76 | in-block receiver code bias, L5B erase, step test | live | kept |
| H2n | M:1953-2138 | P:1513-1698 | 186 / 122 | ZWD stash, `zwd` feed, Z census, prune, seam window | off (capture on `snapw`; the Z census P:1608-1640 only on `cplr`, like H2e4) | kept |
| H2o | M:2139-2145 | P:1699-1705 | 7 / 2 | clone end, shared-history end | inert | kept |
| H2p | M:2146-2188 | P:1706-1748 | 43 / 35 | eq34 RPB | off | kept |
| H2q | M:2189-2278 | (gone) | 90 / 69 (main) | diagonal slant ionosphere (queue 8b) | off | **deleted** `rmk/I` `633de7a` (C18) |
| H3 | M:2281-2299 | P:1751-1769 | 19 / 7 | RXCB-DATUM gauge census | live | kept |
| H4ab | M:2301-2458 | P:1771-1887 | 117 / 75 | rx-CB produce tallies and the diagonal stash | tallies live, stash off | kept; **deleted** M:2313-2317, 2342-2346, 2427-2453, argument M:2396-2400 (`1c2b093`, C19) |
| H4c | M:2459-2500 | (gone) | 42 / 30 (main) | LV-4 peer-relative step screen | off | **deleted** `1c2b093` (C19) |
| H4d | M:2501-2506 | P:1888-1894 | 7 / 6 | rx-CB gap prune | live | kept |
| H4e | M:2507-2573 | (gone) | 67 / 44 (main) | S2H de-mean | off | **deleted** `1c2b093` (C19) |
| H4f | M:2574-2663 | (gone) | 90 / 52 (main) | LV-2 lag-1 mean, LV-5 residual | off | **deleted** `1c2b093` (C19) |
| H5 | M:2666-2701 | (gone) | 36 / 24 (main) | LV-1 SH field snapshot | off | **deleted** `1c2b093` (C19) |
| H6 | M:2703-2721 | P:1896-1914 | 19 / 6 | first-fold CTA trips, latch retired | live (no-op); the trip count on `snapr` (its dense-carry resume sets the latch) | kept |

**Moved by EP1: nothing in the body.** EP1 converts the caller, `networkLeveling`, to
`networkLeveling(NetworkRun& run, const NetworkEpoch& ep)` with a generated preamble; the
harvest call's 17 arguments then resolve to run and epoch members (table 1.4) and the call text
is unchanged. EP1's W1 row for `networkRxCbDemeanPrepass` (`scripts/nom_ep1/rows_app.py` on
branch `nom/ep1-tools`) has to leave the canonical table with C19: the survey refuses a row whose
seam is gone (`scripts/nom_ep1/README.md`, "The frozen base"). EP1c makes the estimator const in `networkLeveling`; that compiles only
if the harvest's `est` parameter is const too (every estimator call the harvest makes is const:
`include/filter/NetworkEstimator.hpp` 582, 666, 683, 710, 766, 795, 164-171, 218), so after EP1c
the frozen declarator is `const NetworkEstimator& est`.

### 1.4 The parameters and where they bind after EP1

| parameter (frozen declarator) | binding (through networkLeveling's EP1 preamble) |
| --- | --- |
| `NetworkRunContext& ctx` | `run.ctx` |
| `const GnssConfig& config_` | `run.config` |
| `NetworkEstimator& est` (const after EP1c) | `run.engines.est` |
| `const Eigen::VectorXd& x_s` | `ep.solve.x_s` |
| `std::vector<NetStation>& stations` | `run.setup.stations` (networkLeveling's alias `auto& stations = setup.stations;`) |
| `const std::vector<int>& est_index_of` | `ep.assembly.est_index_of` |
| `const std::vector<StationAssembly>& asm_res` | `ep.assembly.asm_res` |
| `const NetworkSbasis::Plan& plan` | `ep.solve.plan` |
| `const SphericalHarmonicIonosphere* sh_model`, `sh_model2` | `run.setup.sh_model.get()`, `run.setup.sh_model2.get()` |
| `NetworkCarryVce& carry_vce_est` | `run.engines.carry_vce` |
| `NetworkPriorResidual& prires_est` | `run.engines.prires` |
| `bool carry_warm` | `ep.clock.carry_warm` (copy) |
| `double pl_now_s` | `ep.clock.pl_now_s` (copy) |
| `registered_clock_sats`, `registered_code_keys` | `ep.columns.registered_clock_sats`, `ep.columns.registered_code_keys` |
| `const std::set<std::string>& shared_phase_keys` | `ep.pass_a.shared_phase_keys` |

Every group read is written before the solve, so every harvest function satisfies EP1's
availability rule (V10) at networkLeveling's spine line. Name pre-flight: no frozen body uses
`run`, `ep`, `border`, `blocks` or `sb` as an identifier (scanned, comments only), so no
fallback names are needed.

---

## 2. The new types and functions

### 2.1 The spine header, `include/app/NetworkLocalCarryHarvest.hpp` (new)

```cpp
#pragma once
/**
 * @file NetworkLocalCarryHarvest.hpp
 * @brief The station-local carries' post-solve harvest: its table of contents and the
 *        machinery its kinds share (card NOM3 step HV1).
 *
 * After the epoch's solve, networkLeveling (filter/NetworkLeveling.cpp) calls
 * networkHarvestLocalCarries, which reads each station's posterior out of the solved normal
 * equations and refreshes the carried maps that the next epoch's PASS B folds as priors.
 * The orchestrator is a list of calls, one per carried kind, in a fixed order; each kind's
 * harvest lives in the file of its carry:
 *
 *   carried state                       harvest                                  file
 *   anchor handover ledger              networkHarvestHandoverLedger             filter/NetworkLevelingCarry
 *   ambiguities (local_carry)           networkHarvestAmbiguityStation,          filter/NetworkLevelingCarry
 *                                       networkAmbiguityMonitorFeeds,
 *                                       networkPruneAmbiguities
 *   ionosphere-free variance trace      networkAmbIfTraceStation                 filter/NetworkAmbStateTrace
 *   phase colour (pc_carry)             networkHarvestPhaseColourStation,        filter/NetworkPhaseColourCarry
 *                                       networkPrunePhaseColour
 *   slant ionosphere (iono_blk_carry)   networkHarvestSlantIonoBlock             filter/NetworkSlantIonoCarry
 *   receiver clock (rx_clk_carry/rate)  networkHarvestRxClock                    filter/NetworkRxClockCarry
 *   receiver code bias (rx_cb_carry)    networkHarvestRxCbBlock,                 filter/NetworkRxCodeBiasCarry
 *                                       networkRxCbDatumGaugeCensus,
 *                                       networkHarvestRxCbCarry
 *   zenith wet delay (zwd_carry)        networkHarvestZwd, networkZwdSeamWindowStep  filter/NetworkZwdCarry
 *   eq34 receiver phase bias (rpb)      networkHarvestRxPhaseBias                filter/NetworkChainDatum
 *   the first post-resume harvest       networkResumeCouplingCensusRow,          app/NetworkWarmResume
 *                                       networkResumeFirstEpochRetire
 *
 * The order is load-bearing (each constraint is checked by scripts/nom_hv1_verify.py):
 *   1. The handover ledger reads each arc's first-stash variance from the previous epoch's
 *      carry, so it runs before the ambiguity stash rewrites it.
 *   2. Per station: the block layout, the ambiguity rows, the ionosphere-free trace (it reads
 *      the rows just written), then phase colour.  The shared-history notes therefore run
 *      ambiguities then phase colour per station, then kind by kind (every station's
 *      ionosphere, then receiver clock, receiver code bias and ZWD); card SCL2's compression
 *      is sensitive to that order.
 *   3. The ambiguity rows read the pending-reseed mark before they clear it; RAIM and the
 *      gross line-of-sight screen run after the harvest because they set new marks.
 *   4. The leveling clock pl_prev_now_s is advanced after every station (the stash's trace age
 *      reads the previous stamp) and before the prunes.
 *   5. Every writer of an F3 block row runs before networkHarvestBorderEnd (the clone end and
 *      the shared-history compression read the finished blocks).
 *   6. networkResumeFirstEpochRetire runs last: it closes the latch both census rows read.
 *   7. The receiver code-bias census and carry run outside the warm bracket: the carry must
 *      work on the carry_invariant lineage without phase leveling.
 */
#include "filter/NetworkClockClone.hpp"      // ClockCloneHarvest
#include "filter/NetworkSharedHistory.hpp"   // SharedHistoryHarvest
#include "filter/NetworkRunTypes.hpp"        // CoupledShared, PlHarvestBlock
#include <Eigen/Core>
#include <array>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace GnssProcessor {

struct NetworkRun;
struct NetworkEpoch;

/// The shared border every carried column of one warm epoch regresses on (formerly four
/// locals of networkHarvestLocalCarries).  Built by networkHarvestBorderBegin, read by
/// networkHarvestColumn for every carried column, closed by networkHarvestBorderEnd after the
/// last row.  Empty on the legacy diagonal path (no network.carry_conditional_coupling).
struct HarvestBorder {
    std::map<Eigen::Index, CoupledShared> col2desc;  ///< Shared column -> its identity (clock, phase bias, code bias, SH coefficient).
    Eigen::MatrixXd      cov_df;  ///< The shared covariance in the datum frame: datum-pinned and ensemble-gauge columns zeroed.
    ClockCloneHarvest    ccl_h;   ///< Card SCL: the clones' posterior regression on the rest of the border.
    SharedHistoryHarvest shx_h;   ///< Card SCL2 X2: where each harvested row's entry was written.
};

/// The segments of a station's F3 harvest block, in the order their columns are appended.
/// The ZWD is last, so its row is the block's last (the B9-in-block invariant).  PASS B's
/// block build (app/ApplicationNetworkAssembly.cpp, the gate at 4668 and the appends at
/// 4821-4843 on main 31c393f) must open a block on the same station-epochs; it pairs entries
/// by block identity and row index, not by this order.
enum class BlockSegment : int { Amb, Iono, RxClock, RxCodeBias, PhaseColour, Zwd };
inline constexpr std::size_t kBlockSegmentCount = 6;

/// One segment's rows of the block: its first row and its column count (0: absent).
struct BlockSpan { Eigen::Index row0{0}; Eigen::Index n{0}; };

/// The spans from the station-loop head's row offsets (the one place the layout arithmetic
/// lives; unit-tested in tests/test_network_harvest_layout.cpp).
std::array<BlockSpan, kBlockSegmentCount> harvestBlockSpans(
    Eigen::Index iono_row0, Eigen::Index rxclk_row0, Eigen::Index rxcb_row0,
    Eigen::Index pc_row0, Eigen::Index n_cols, bool zwd_in_cols);

/// One station's harvest layout for this epoch (replaces the eleven per-station hand-forward
/// vectors).  Written by networkHarvestStationBlock, read by every kind that writes a row of
/// the station's block.  A station the loop head skips keeps ok false and no block.
struct StationHarvestBlock {
    bool                      ok{false};  ///< The station is harvested this epoch.
    std::vector<Eigen::Index> cols;       ///< The block's local columns, segment by segment.
    std::array<BlockSpan, kBlockSegmentCount> span{};  ///< Each segment's rows.
    Eigen::VectorXd           xr;         ///< The station's back-substituted locals (reconstructStation).
    Eigen::VectorXd           vard;       ///< The legacy diagonal variances (empty under the coupling carry).
    std::shared_ptr<PlHarvestBlock> blk;  ///< The F3 block; null without one this station-epoch.

    /// The block a segment's rows go to: blk when the segment has columns, else null.
    std::shared_ptr<PlHarvestBlock> segmentBlock(BlockSegment s) const {
        return span[static_cast<std::size_t>(s)].n > 0 ? blk : nullptr;
    }
    /// The segment's first block row.
    Eigen::Index rowOf(BlockSegment s) const { return span[static_cast<std::size_t>(s)].row0; }
};

/**
 * @brief Harvest, stash and prune the station-local carries from this epoch's solve.
 *
 * The table of contents above, in its order.  Writes run state only (the carried maps, the
 * service counters, the station models' reseed marks and L5B tallies, the VCE and
 * prior-residual engines); reads the epoch's solution, assembly, columns and clock.
 * Called by networkLeveling at the SB_LEVELING spine line.
 *
 * @param run The run: its context (carry, counters, knobs), setup (stations) and engines.
 * @param ep  This epoch after the solve: solve.x_s, solve.plan, assembly, columns,
 *            pass_a.shared_phase_keys and clock (carry_warm, pl_now_s) are read.
 */
void networkHarvestLocalCarries(NetworkRun& run, const NetworkEpoch& ep);

/// @brief The warm epoch's shared border: column identities, the datum-frame covariance and the
///        clone and shared-history begin (inert pieces stay empty when their cards are off).
HarvestBorder networkHarvestBorderBegin(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief One carried column's pure conditional variance and its sparsified, identity-keyed
 *        regression on the free shared border (formerly the harvestCoupling lambda).
 * @param run        The run (the estimator, the context's clone counters).
 * @param ep         This epoch (solve.x_s: the shared estimates each kept term records).
 * @param border     This harvest's border (identities, datum-frame covariance, clone rows).
 * @param est_idx    The station's estimator slot.
 * @param col        The local column.
 * @param cross_cols With F3: the station's other block columns, gathered by the same solve; else null.
 * @param cross      With F3: receives (N_rr^-1)_{c c_j} over @p cross_cols; else null.
 * @return {cond_var, coupling}; cond_var <= 0 when the regression failed (the caller skips).
 */
std::pair<double, std::vector<CoupledShared>> networkHarvestColumn(
    NetworkRun& run, const NetworkEpoch& ep, HarvestBorder& border, std::size_t est_idx,
    Eigen::Index col, const std::vector<Eigen::Index>* cross_cols, Eigen::VectorXd* cross);

/// @brief Station @p i's layout: whether it is harvested, its block columns in BlockSegment
///        order, its back-substituted locals and its F3 block (registered with the clone harvest).
StationHarvestBlock networkHarvestStationBlock(NetworkRun& run, const NetworkEpoch& ep,
                                               HarvestBorder& border, std::size_t i);

/// @brief Close the border: the clones' within-station covariance into the F3 blocks, then the
///        shared-history compression (both no-ops when their cards are off).
void networkHarvestBorderEnd(NetworkRun& run, const NetworkEpoch& ep, HarvestBorder& border);

} // namespace GnssProcessor
```

### 2.2 The orchestrator (`src/core/app/NetworkLocalCarryHarvest.cpp`)

Frozen lines kept in place: the warm bracket P:216-222 and P:1749, the station-loop header P:380
and its close P:1028, the stamp P:1029-1031, the function close P:1915. Everything else is new
and declared.

```cpp
void networkHarvestLocalCarries(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch (card NOM3 step HV1; generated by scripts/nom_hv1_cut.py,
    //      each line checked by scripts/nom_hv1_verify.py). ----
    NetworkRunContext& ctx = run.ctx;
    std::vector<NetStation>& stations = run.setup.stations;
    bool carry_warm = ep.clock.carry_warm;
    double pl_now_s = ep.clock.pl_now_s;
    // ---- End of the bindings ----
    networkHarvestHandoverLedger(run, ep);

    // --- Phase-leveling carry: harvest posterior and stash (increment 1) ----   (frozen comment)
    if (carry_warm) {
        HarvestBorder border = networkHarvestBorderBegin(run, ep);
        std::vector<StationHarvestBlock> blocks(stations.size());
        for (size_t i = 0; i < stations.size(); ++i) {
            blocks[i] = networkHarvestStationBlock(run, ep, border, i);
            if (!blocks[i].ok) continue;
            networkHarvestAmbiguityStation(run, ep, border, blocks[i], i);
            networkAmbIfTraceStation(run, ep, blocks[i], i);
            networkHarvestPhaseColourStation(run, ep, border, blocks[i], i);
        }
        // Stamp this harvest: the next epoch differences it against its own stamp for
        // the observed inter-epoch spacing.
        ctx.carry.pl_prev_now_s = pl_now_s;
        networkPrunePhaseColour(run, ep);
        networkPruneAmbiguities(run, ep);
        networkHarvestSlantIonoBlock(run, ep, border, blocks);
        networkHarvestRxClock(run, ep, border, blocks);
        networkHarvestRxCbBlock(run, ep, border, blocks);
        networkHarvestZwd(run, ep, border, blocks);
        networkHarvestBorderEnd(run, ep, border);
        networkHarvestRxPhaseBias(run, ep);
    }
    networkRxCbDatumGaugeCensus(run, ep);
    networkHarvestRxCbCarry(run, ep);
    networkResumeFirstEpochRetire(run);
}
```

The caller's edit (`src/core/filter/NetworkLeveling.cpp`, after EP1): the 17-argument call
(main 70-74, lev_post 61-65) becomes `networkHarvestLocalCarries(run, ep);`; the aliases that
only fed it (`sh_model`, `sh_model2`, `stations`, lev_post 56-58) and the EP1 preamble lines
that only fed it (`config_`, `setup`, `est`, `x_s`, `plan`, `asm_res`, `est_index_of`,
`carry_vce_est`, `prires_est`, `carry_warm`, `pl_now_s`, the three column sets; `log` is unread
already) are dropped, declared.
networkLeveling keeps `ctx`, `traces` and `se` (its eq34 re-reference rows).

### 2.3 The per-kind functions, their files and their Doxygen

Three signature forms, and no function writes an epoch member:
- **Epoch form, 20 functions** (rows 1 to 20: the orchestrator and the 19 it calls):
  `(NetworkRun& run, const NetworkEpoch& ep[, harvest-local kept...])`, EP1's seam form, which
  `scripts/nom_epoch_lint.py` recognizes by its first two parameters.
- **Run only, 2 functions** (rows 21 and 22): `(NetworkRun& run[, kept...])`. Both post-resume
  bodies read run state and the restored and fresh regressions only, no epoch group, so an
  epoch parameter would be unread.
- **Pure, 1 helper** (row 23): `harvestBlockSpans`, no run and no epoch.

Each of the 22 opens with a generated preamble (2.4) and is the frozen text below it. "Shape"
is how the frozen statement became the body (section 3).

| # | function and signature (rows 1 to 20 after `NetworkRun& run, const NetworkEpoch& ep`; rows 21 and 22 after `NetworkRun& run`) | file (header) | frozen (after) | shape | Doxygen `@brief` (intent) |
| --- | --- | --- | --- | --- | --- |
| 1 | `networkHarvestLocalCarries()` | app/NetworkLocalCarryHarvest (new .hpp) | P:216-222, 380, 1028-1031, 1749, 1915 | orchestrator | 2.1 |
| 2 | `HarvestBorder networkHarvestBorderBegin()` | same | P:223-276 | verbatim, four locals to members | 2.1 |
| 3 | `networkHarvestColumn(HarvestBorder&, est_idx, col, cross_cols, cross)` | same | P:277-283 (comment), 288-331 (body); head 284-287 and close 332 dropped | lambda to function | 2.1 |
| 4 | `StationHarvestBlock networkHarvestStationBlock(HarvestBorder&, std::size_t i)` | same | P:381-495 less the hand-forward writes 468-491 | verbatim, continue to return, hand-forward writes out, layout epilogue | 2.1 |
| 5 | `networkHarvestBorderEnd(HarvestBorder&)` | same | P:1699-1705 | verbatim | 2.1 |
| 6 | `networkHarvestHandoverLedger()` | filter/NetworkLevelingCarry (new) | P:145-214 | guard | The two-edge continuous handover's ledger: each anchor candidate's absolute ambiguity in this epoch's frame and its border-exact variance, consumed by the next epoch's election.  No-op unless network.phase_datum_handover_continuous. |
| 7 | `networkHarvestAmbiguityStation(HarvestBorder&, const StationHarvestBlock& sb, std::size_t i)` | same | P:496-873 less two extracts | verbatim, two extracts in place | Station i's ambiguities: variance and regression, the monitor feeds, the post-resume census, the stash (first-stash variance, birth floor, drop walk, birth and re-level stamps, the amb-state row), the F3 row, and the cleared reseed mark. |
| 8 | `networkAmbiguityMonitorFeeds(std::size_t i, const PlAmb& pa, double value_abs, double var)` | same | P:518-636 | extracted | One ambiguity's LS-VCE row and prior-residual "pl" row, the PASS B inject predicate reproduced, read before the stash overwrites the prior.  Measure-only. |
| 9 | `networkPruneAmbiguities()` | same | P:1045-1106 | unwrap | The ambiguity gap prune (the registry's horizon) with its retirement census, the amb-state erase rows, the roster death buffer and PRM's prune-after-drop count. |
| 10 | `networkAmbIfTraceStation(const StationHarvestBlock& sb, std::size_t i)` | filter/NetworkAmbStateTrace | P:874-986 | guard | Card AIL: the ionosphere-free ambiguity's prior and posterior variance per line of sight with both bands in the block, raw and de-meaned per constellation.  Measure-only; no-op unless network.trace_amb_if. |
| 11 | `networkHarvestPhaseColourStation(HarvestBorder&, const StationHarvestBlock& sb, std::size_t i)` | filter/NetworkPhaseColourCarry (new) | P:987-1027 | guard | Card LGM: each phase-colour column's posterior, regression and F3 row, and its trace row. |
| 12 | `networkPrunePhaseColour()` | same | P:1033-1043 | verbatim | Card LGM: drop phase-colour entries past network.phase_colour_max_gap_s (counted). |
| 13 | `networkHarvestSlantIonoBlock(HarvestBorder&, const std::vector<StationHarvestBlock>& blocks)` | filter/NetworkSlantIonoCarry (new) | P:1107-1211 | guard | network.carry_iono_in_block: each line of sight's slant ionosphere with its regression and F3 row, then the horizon prune. |
| 14 | `networkHarvestRxClock(HarvestBorder&, const std::vector<StationHarvestBlock>& blocks)` | filter/NetworkRxClockCarry (new) | P:1212-1410 | guard | network.carry_rx_clock_in_block: receiver clock and ISB columns with their F3 rows; the primary clock's (bias, drift) filter, its gate census and jump response; the prune. |
| 15 | `networkHarvestRxCbBlock(HarvestBorder&, const std::vector<StationHarvestBlock>& blocks)` | filter/NetworkRxCodeBiasCarry (new) | P:1411-1512 | guard | network.carry_rx_code_bias_in_block: receiver code-bias columns with their regression and F3 row, the L5B family re-seed erase and the 10-sigma step test. |
| 16 | `networkRxCbDatumGaugeCensus()` | same | P:1751-1769 | guard | RXCB-DATUM: the zero-mean receiver-DCB datum's folds, counted under its own knob outside every carry bracket. |
| 17 | `networkHarvestRxCbCarry()` | same | P:1771-1894 | guard | network.carry_rx_code_bias: the produce tallies, the diagonal stash (skipped under the in-block route) with its prior-residual row and L5B erase, then the gap prune. |
| 18 | `networkHarvestZwd(HarvestBorder&, const std::vector<StationHarvestBlock>& blocks)` | filter/NetworkZwdCarry (new) | P:1513-1698 less two extracts | guard, two extracts | B9: the ZWD posterior (carry, live seam window, or capture-only for the snapshot), its prior-residual row, the post-resume census, the stash and F3 row, the prune, the seam window step. |
| 19 | `networkZwdSeamWindowStep()` | same | P:1670-1697 | extracted, guard | network.zwd_seam_reseed: count the window's folds, advance its retirement clock and log the decoupling edge. |
| 20 | `networkHarvestRxPhaseBias()` | filter/NetworkChainDatum (exists) | P:1706-1748 | guard | Card 3 (eq34): each receiver phase bias with its diagonal conditional variance (no coupling, by design), then the prune. |
| 21 | `networkResumeCouplingCensusRow(const LocalCarry& restored, const std::vector<CoupledShared>& coupling)`, run only | app/NetworkWarmResume (declared in NetworkRunContext.hpp) | P:652-678, merged with P:1612-1638 | extracted, merge | The first post-resume harvest's check of one restored regression against the fresh one (matched terms, unmatched terms, relative change, RHS change).  Measure-only. |
| 22 | `networkResumeFirstEpochRetire()`, run only | same | P:1896-1914 | verbatim | Card P9a: the first post-resume fold's CTA trip count, then the latch retired. |
| 23 | `harvestBlockSpans(...)` (pure) | app/NetworkLocalCarryHarvest | new | new | 2.1 |

File sizes after the cut (estimates: frozen lines carried with the declared drops subtracted,
comments included, plus preamble and signature; the carried counts are `hv1_table.py`'s):
spine `.cpp` about 320 lines (orchestrator 42, border begin 72, column 58 from 51 carried,
station block about 113 from 91 carried plus the 9-line epilogue, border end 17, spans 15),
header about 190; `NetworkLevelingCarry.cpp` about 570 (ledger 83, ambiguity station 270, feeds
137, prune 73); `NetworkPhaseColourCarry.cpp` about 90; `NetworkSlantIonoCarry.cpp` about 130;
`NetworkRxClockCarry.cpp` about 230; `NetworkRxCodeBiasCarry.cpp` about 300 (block 119, census
29, carry 140); `NetworkZwdCarry.cpp` about 210 (ZWD 158, seam 40). The three existing owner
files, measured on the composed post-deletion tree (`a621b16` plus the five track branches,
`nextsteps/hv1chk/compose/`): `NetworkAmbStateTrace.cpp` 169 (no track edits it) to about 300;
`NetworkChainDatum.cpp` 1 181 (no track edits it) to about 1 240; `NetworkWarmResume.cpp` 1 402
(1 502 at main; `rmk/R2` and `rmk/K` edit it, 1 451 and 1 453 lines alone, composing cleanly)
to about 1 465 (the census row about 36, the latch retire about 27).
The largest functions are the ambiguity station (about 270 lines, 122 code) and the receiver
clock (about 222, 128 code); section 7 lists the HV2 cuts that shorten both.

### 2.4 The preamble rule, with one worked example

A harvest function's preamble is generated, in this order: the frozen parameters its body reads,
each as `<frozen declarator> = <binding of table 1.4>;` (`const` may be added on a reference);
the frozen alias lines (P:61-143) its body reads, text-identical, with every alias they depend
on; then the harvest-local bindings it reads (border members, the station layout, the frozen
loop-head declarations that are pure re-evaluations). The frozen body follows unchanged below
`// ---- End of the bindings ----`. The marker lines follow EP1's (EP1 specification 3.2) and name HV1's scripts.

```cpp
void networkHarvestSlantIonoBlock(NetworkRun& run, const NetworkEpoch& ep, HarvestBorder& border,
                                  const std::vector<StationHarvestBlock>& blocks) {
    // ---- From the run, the epoch and the harvest (card NOM3 step HV1; generated by
    //      scripts/nom_hv1_cut.py, each line checked by scripts/nom_hv1_verify.py). ----
    NetworkRunContext& ctx = run.ctx;
    const NetworkEstimator& est = run.engines.est;
    const Eigen::VectorXd& x_s = ep.solve.x_s;
    std::vector<NetStation>& stations = run.setup.stations;
    const std::vector<int>& est_index_of = ep.assembly.est_index_of;
    const std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    const NetworkSbasis::Plan& plan = ep.solve.plan;
    double pl_now_s = ep.clock.pl_now_s;
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond;
    const bool   pl_local_block = ctx.knobs.pl_local_block; // stash the harvest cross-covariance block beside the per-column carry (F3)
    const bool   iono_blk_on = ctx.knobs.iono_blk && pl_local_block && carry_cpl_cond;
    SharedHistoryHarvest& shx_h = border.shx_h;
    // ---- End of the bindings ----
    if (!iono_blk_on) return;                                         // guard of P:1128
    auto& iono_blk_carry = ctx.carry.iono_blk_carry;                  // P:1129, shifted 8
    for (size_t i = 0; i < stations.size(); ++i) {
        ...
        const std::shared_ptr<PlHarvestBlock> iblk = blocks[i].segmentBlock(BlockSegment::Iono);   // P:1135, LAYOUT_READ
        const std::vector<Eigen::Index>* icross_cols =
            iblk ? &blocks[i].cols : nullptr;                                                     // P:1137, LAYOUT_READ
        const Eigen::Index irow0 = iblk ? blocks[i].rowOf(BlockSegment::Iono) : Eigen::Index{-1}; // P:1138, LAYOUT_READ
        ...
                auto hc = networkHarvestColumn(run, ep, border, est_idx, icol, icross_cols,     // P:1158, DIRECT_CALL
                                               iblk ? &icross : nullptr);
```

Station-layout bindings, for the three per-station row functions (7, 10, 11):
`const StationAssembly& R = asm_res[i];` and
`const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);` and
`const bool pc_in_cols = ctx.knobs.pc_on && !R.pl_pc.empty();` repeat the frozen loop-head
lines P:386, 403, 442 (pure in their inputs); `const std::vector<Eigen::Index>& cols = sb.cols;`,
`const Eigen::VectorXd& xr = sb.xr;`, `const Eigen::VectorXd& vard = sb.vard;`,
`const std::shared_ptr<PlHarvestBlock>& hblk = sb.blk;` and
`const Eigen::Index pc_row0 = sb.rowOf(BlockSegment::PhaseColour);` bind the layout (value-equal
to the frozen locals, which the verifier proves by H8).

Three alias lines read run state, not knobs: `amb_state_tr` (P:86-90, `ctx.counters.n_loop_epochs`),
`amb_state_dt_ep_s` (P:96-99, `ctx.carry.pl_prev_now_s`) and `zwd_seam_live` (P:143,
`ctx.carry.zwd_seam_ep_left`). Today they are evaluated once at the harvest's entry; in HV1 they
are evaluated at the entry of each function that reads them (the ambiguity station, the ZWD and
its seam step). Nothing in between writes their inputs (the stamp P:1031 follows every station;
the decrement P:1682 is inside the seam step); check H5 proves it on every tree.

### 2.5 The station-block epilogue (the layout recorded once)

`networkHarvestStationBlock` opens with `StationHarvestBlock sb;`, carries P:381-495 with its
four station-level `continue;` (P:381, 385, 402, 453) written `return sb;`, drops the
hand-forward writes P:468-491, and ends:

```cpp
    // ---- The station's layout, recorded once (card NOM3 step HV1; generated) ----
    sb.ok   = true;
    sb.span = harvestBlockSpans(iono_row0, rxclk_row0, rxcb_row0, pc_row0,
                                static_cast<Eigen::Index>(cols.size()), zwd_in_cols);
    sb.cols = std::move(cols);
    sb.xr   = std::move(xr);
    sb.vard = std::move(vard);
    sb.blk  = std::move(hblk);
    return sb;
```

`harvestBlockSpans` returns `{0, iono_row0}`, `{iono_row0, rxclk_row0 - iono_row0}`,
`{rxclk_row0, rxcb_row0 - rxclk_row0}`, `{rxcb_row0, pc_row0 - rxcb_row0}`,
`{pc_row0, z - pc_row0}` and `{z, zwd_in_cols ? 1 : 0}` with `z = n_cols - (zwd_in_cols ? 1 : 0)`.
Equivalence with the frozen hand-forward, segment by segment: a frozen consumer saw `hblk` exactly
when the station had a block and the segment's `_in_cols` held (P:471-491); a segment's column
count is positive exactly when its `_in_cols` held (P:415-447: nothing is appended otherwise),
so `segmentBlock` returns the same pointer; `rowOf` returns the same frozen `_row0` local; the
`_cols_of_sta[i]` copies all equalled the one `cols`. The clone registration (P:494) still runs
before `cols` is moved, in station order (its block list is read only by its end,
`src/core/filter/NetworkClockClone.cpp` 1168-1177 and 1179-1208), and the shared-history handles
are identities, not pointers (`include/filter/NetworkSharedHistory.hpp` 145-159), so returning
the border and the layout by value is safe.

### 2.6 HV1b: the receiver code-bias run fields move to the service counters

After the deletion, `NetworkCarryState` still holds four rx-CB run fields
(`include/app/NetworkRunContext.hpp` 2236-2253 at main; at `rmk/I` 2180-2197):
`long long rx_cb_injected_total, rx_cb_innov_skips_total, rx_cb_harvested_total` and
`std::map<std::string, std::pair<long long, long long>> rx_cb_sta_tally`. They move, with their
declarators, initializers and Doxygen unchanged, into `NetworkServiceCounters` beside the
`n_rxcb_blk_*` counters (main 3668). Writers: the harvest, P:1509 (function 15) and P:1786,
1792, 1803, 1885 (function 17). Reader: `networkRxCodeBiasCarryRunEnd`
(`src/core/filter/NetworkRxCodeBiasCarryRunEnd.cpp` at `rmk/I` 47, 57, 65, 66). Nothing
snapshots, fingerprints or copies them (checked with `git grep` over `src`, `include`, `tests`
and `scripts`; no whole-struct copy of `ctx.carry` or `ctx.counters` exists), so the move changes
no output. The run-end body is generated by step TD's tool, so `scripts/nom_td_cut.py` gains four
declared substitutions on the frozen teardown lines `067fdc1` 4477, 4487, 4495, 4496
(`ctx.carry.` to `ctx.counters.`), and `scripts/nom_td_manifest.json` is regenerated.

### 2.7 HV1c: the printed site citations (a declared output text move)

`src/core/filter/NetworkResponses.cpp` cites each response site by file in its "where" column,
printed by the parameter-responses run end for every site armed or fired (510). After the deletion
nine rows name `NetworkLocalCarryHarvest.cpp` (`rmk/I` 58, 87, 94, 97, 103, 106, 110, 117, 122).
Each then names the file that holds the site's `prmRespond` call: `AmbGapPrune` NetworkLevelingCarry.cpp,
`IonoGapPrune` NetworkSlantIonoCarry.cpp, `PcGapPrune` NetworkPhaseColourCarry.cpp,
`RxcbStepErase` and `RxcbGapPrune` NetworkRxCodeBiasCarry.cpp, `RxclkJumpErase` and
`RxclkGapPrune` NetworkRxClockCarry.cpp, `RpbGapPrune` NetworkChainDatum.cpp, `ZwdGapPrune`
NetworkZwdCarry.cpp. Step HV0 set the precedent (`827fba6`): its own commit, the log text moves by
that substitution and nothing else, no script parses it. Comment-only citations of the old
harvest (`include/products/NetworkStateSnapshot.hpp` 355-356, 382-383, 396;
`src/core/app/NetworkStateSnapshotWrite.cpp` 237; `src/core/app/NetworkResumeRunEnd.cpp` 72;
`src/core/filter/NetworkLevelingCarryRunEnd.cpp` 5-7 and its generator string
`scripts/nom_td_cut.py` 281; `src/core/filter/NetworkRxCodeBiasCarryRunEnd.cpp` 5;
`include/filter/NetworkRunEnd.hpp` 22; `src/core/filter/NetworkRedundancyCensus.cpp` 488;
`src/core/filter/NetworkChainDatum.cpp` 885; `include/app/NetworkRunContext.hpp` 5039-5040;
`tests/test_rx_cb_datum_gauge.cpp` 10) are updated in HV1a.

---

## 3. The cut

### 3.1 The table (frozen range to function and file)

Ranges are P lines (comments included; a comment block directly above a moved statement moves
with it); "shift" is the leading spaces removed. A drop declared inside a piece is subtracted
from it: the piece carries its range minus its drops, so no line is both carried and dropped.
The draft table with its checks is `nextsteps/hv1/hv1_table.py`.

**The anchor rule.** A row's anchor is a token prefix of one statement (comment-insensitive,
`nom_cpp.lex`), matched against every statement start inside the frozen function at every depth
(`nom_cpp.statements`, descending into every brace block). It must match exactly one statement
start; the row's optional `at` then moves it by whole statements at the same depth, and the
statement reached must be the first code statement of the row's first piece. Where a piece's
first statement is not unique, the prefix runs on into that statement's interior (a guard's
first interior statement, a bare block's first declaration); where the whole statement repeats,
the anchor is a unique neighbour and `at` steps to it. Five rows need this (checked by
`nextsteps/hv1chk/anchors.py` and `hv1_table.py` on `post.cpp`):
- the station block's first statement `if (est_index_of[i] < 0) continue;` occurs nine times
  (P:169, 381, 1131, 1246, 1422, 1525, 1714, 1765, 1784): anchor P:385, the unique pivot skip,
  with `at` -1;
- the prune's bare `{` occurs four times (P:204, 681, 1052, 1468): prefix
  `{ const long long fleet_before =`;
- `if (iono_blk_on) {`, `if (rxclk_blk_on) {` and `if (rxcb_blk_on) {` also open the dropped
  sizing blocks (P:353, 366, 375): each prefix runs into the guard's first statement
  (`auto& iono_blk_carry`, `auto& rx_clk_carry`, `auto& rx_cb_carry`; the last alone also
  occurs at P:1780, so the guard tokens are needed).

All 23 rows (22 functions and the hand-forward drop) resolve to exactly one statement, at their
piece's first statement.

| function | home | pieces (P, shift) | anchor | transforms |
| --- | --- | --- | --- | --- |
| networkHarvestLocalCarries | app/NetworkLocalCarryHarvest | 216-222 (0), 380 (0), 1028-1031 (0), 1749 (0), 1915 (0) | `if (carry_warm) {` | SIGNATURE, orchestrator calls and `blocks` (INSERT) |
| networkHarvestHandoverLedger | filter/NetworkLevelingCarry | 145-166 (0), 167-213 (4), 214 | `if (ctx.knobs.pd_handover_continuous) {` | GUARD |
| networkHarvestBorderBegin | app/NetworkLocalCarryHarvest | 223-276 (4) | `std::map<Eigen::Index, CoupledShared> col2desc;` | LOCAL_TO_MEMBER at 227, 228, 231, 276; INSERT `HarvestBorder border;`, `return border;` |
| networkHarvestColumn | app/NetworkLocalCarryHarvest | 277-283 (8, comment), 288-331 (8) | `NetworkNormalEquations::StationRegression reg;` (P:288, the first body statement) | LAMBDA_TO_FUNCTION: drop 284-287 (kind lambda_head) and 332 (kind lambda_close) |
| (hand-forward) | dropped | none | `std::vector<std::shared_ptr<PlHarvestBlock>> zblk_of_sta;` (P:338) | drop 333-379 (kind hand_forward) |
| networkHarvestStationBlock | app/NetworkLocalCarryHarvest | 381-495 (8) less 468-491 | `if (!phase_datum_prioritized && stations[i].is_pivot) continue;` (P:385), `at` -1 (P:381) | CONTINUE_TO_RETURN at 381, 385, 402, 453; drop 468-491 (kind hand_forward); LAYOUT_RECORD epilogue |
| networkHarvestAmbiguityStation | filter/NetworkLevelingCarry | 496-873 (8) | `for (size_t a = 0; a < R.pl_amb.size(); ++a) {` | DIRECT_CALL 505; EXTRACT 518-636; EXTRACT 652-678 (merge) |
| networkAmbiguityMonitorFeeds | filter/NetworkLevelingCarry | 518-636 (12) | `const bool prires_want = prires_pl && prires_est.enabled();` | extracted body |
| networkResumeCouplingCensusRow | app/NetworkWarmResume | 652-678 (20) | `const auto idOf = [](const CoupledShared& c) {` | extracted body; one substitution at 665 (`old->second.coupling` to `restored.coupling`); MERGE with 1612-1638 |
| networkAmbIfTraceStation | filter/NetworkAmbStateTrace | 874-883 (8), 884-985 (12), 986 | `if (ctx.knobs.amb_if_trace && hblk && !R.pl_amb.empty()) {` | GUARD |
| networkHarvestPhaseColourStation | filter/NetworkPhaseColourCarry | 987-993 (8), 994-1026 (12), 1027 | `if (pc_in_cols && carry_cpl_cond) {` | GUARD; DIRECT_CALL 1001 |
| networkPrunePhaseColour | filter/NetworkPhaseColourCarry | 1033-1043 (4) | `if (ctx.knobs.pc_on)` | verbatim (braceless if) |
| networkPruneAmbiguities | filter/NetworkLevelingCarry | 1045-1052 (4), 1053-1105 (8), 1106 | `{ const long long fleet_before =` (P:1052) | UNWRAP |
| networkHarvestSlantIonoBlock | filter/NetworkSlantIonoCarry | 1107-1128 (4), 1129-1210 (8), 1211 | `if (iono_blk_on) { auto& iono_blk_carry` (P:1128) | GUARD; LAYOUT_READ 1135, 1137, 1138; DIRECT_CALL 1158 |
| networkHarvestRxClock | filter/NetworkRxClockCarry | 1212-1233 (4), 1234-1409 (8), 1410 | `if (rxclk_blk_on) { auto& rx_clk_carry` (P:1233) | GUARD; LAYOUT_READ 1250, 1252, 1253; DIRECT_CALL 1273 |
| networkHarvestRxCbBlock | filter/NetworkRxCodeBiasCarry | 1411-1419 (4), 1420-1511 (8), 1512 | `if (rxcb_blk_on) { auto& rx_cb_carry` (P:1419) | GUARD; LAYOUT_READ 1426, 1428, 1429; DIRECT_CALL 1438; FIELD_MOVE 1509 (HV1b) |
| networkHarvestZwd | filter/NetworkZwdCarry | 1513-1521 (4), 1522-1697 (8), 1698 | `if (zwd_on \|\| zwd_capture \|\| zwd_seam_live) {` | GUARD; LAYOUT_READ 1549, 1551; DIRECT_CALL 1554; EXTRACT 1612-1638 (merge); EXTRACT 1670-1697 |
| networkZwdSeamWindowStep | filter/NetworkZwdCarry | 1670-1676 (8), 1677-1696 (12), 1697 | `if (zwd_seam_live) {` | extracted; GUARD |
| networkHarvestBorderEnd | app/NetworkLocalCarryHarvest | 1699-1705 (4) | `networkClockCloneHarvestEnd(ccl_h, ctx);` | verbatim |
| networkHarvestRxPhaseBias | filter/NetworkChainDatum | 1706-1712 (4), 1713-1747 (8), 1748 | `if (eq34_rpb_q > 0.0) {` | GUARD (`if (!(eq34_rpb_q > 0.0)) return;`) |
| networkRxCbDatumGaugeCensus | filter/NetworkRxCodeBiasCarry | 1751-1763 (0), 1764-1768 (4), 1769 | `if (ctx.knobs.rx_cb_datum) {` | GUARD |
| networkHarvestRxCbCarry | filter/NetworkRxCodeBiasCarry | 1771-1779 (0), 1780-1893 (4), 1894 | `if (ctx.knobs.rx_cb_on) {` | GUARD; FIELD_MOVE 1786, 1792, 1803, 1885 (HV1b) |
| networkResumeFirstEpochRetire | app/NetworkWarmResume | 1896-1914 (0) | `if (ctx.carry.resume_cpl_census_pending)` | verbatim |
| (signature, aliases) | replaced | 45-58, 59-143 | | SIGNATURE; ALIAS_REPEAT |

Coverage (`hv1_table.py`, exit 0): the 1 162 code lines of P:45-1915 are each claimed exactly
once: 1 051 carried by one function (its pieces minus its drops) or by the merged census (its
twin), 57 replaced (signature 14, alias block 43), 54 dropped (lambda_head 4, lambda_close 1,
hand_forward 49). Every dropped code line is checked against its kind: `lambda_head` must be the
`auto harvestCoupling = [&](` head through its `{` with no `;` in it, `lambda_close` exactly
`};`, `hand_forward` a declaration, sizing or write of one of the eleven `*_of_sta` vectors or
the `if (<flag>) {` / `}` around them (flags `zwd_blk_on`, `iono_blk_on`, `rxclk_blk_on`,
`rxcb_blk_on` and the four `*_in_cols`). A body statement in a dropped range fails. The guard
rows' `if (C) {` and `}` are not drops: they stay in their pieces and GUARD rewrites them.
Negative controls in `hv1_table.py --controls`, each failing as required: C-T1 the lambda head
declared 284-288 (revision 1's table, which dropped `reg`), C-T2 a hand-forward drop stretched
over P:494 (`networkClockCloneHarvestBlock`), C-T3 and C-T4 revision 1's station-block and
ionosphere anchors (nine and two matches), C-T5 a guard's close left unclaimed.

### 3.2 Declared drops

| frozen (P) | code lines | kind (validated) | what | replaced by |
| --- | --- | --- | --- | --- |
| 45-58 | 14 | replaced | the 17-parameter signature | `(NetworkRun&, const NetworkEpoch&)` and the preambles |
| 59-143 | 43 | replaced | the alias block | each line repeated in the preamble of every function that reads its name |
| 284-287 | 4 | lambda_head | `auto harvestCoupling = [&](...)` through `-> std::pair<...> {` (P:288, the declaration of `reg`, is the first body statement and is carried) | the signature of `networkHarvestColumn` |
| 332 | 1 | lambda_close | the lambda's `};` | the function's `}` |
| 333-379 | 30 | hand_forward | the eleven hand-forward vectors and their four sizing blocks | `std::vector<StationHarvestBlock> blocks(stations.size());` (orchestrator) |
| 468-491 | 19 | hand_forward | the four hand-forward writes with their `if (*_in_cols) {` and `}` (471-474, 476-480, 482-486, 487-491) | the layout epilogue (2.5) |
| 1612-1638 | 27 | merge twin (H9) | the Z copy of the resume census body | the call `networkResumeCouplingCensusRow(run, zold->second, coupling);` (MERGE) |
| caller | 3 aliases + 15 preamble lines | caller | networkLeveling's names that only fed the call (every EP1 preamble line but `ctx`, `traces` and `se`; `log` is already unread after the deletion) | `networkHarvestLocalCarries(run, ep);` |

Not drops: the GUARD rows' `if (C) {` and `}` lines (P:166 and 214, and the like in every guard
row) stay in their pieces; GUARD rewrites the first to `if (!C) return;` and removes the second
(3.3, H1).

### 3.3 Declared substitutions: the closed list

| class | count | where | what the verifier proves |
| --- | --- | --- | --- |
| SIGNATURE | 1 | P:45-58 | the frozen call site's 17 bindings (table 1.4) equal the preamble bindings (H3) |
| PARAM_BINDING | 17 parameters, bound in every function that reads them | preambles | declarator text is the frozen parameter's, `const` alone addable on a reference; binding derived from networkLeveling's EP1 preamble |
| ALIAS_REPEAT | 35 alias lines | preambles | text-identical to P:61-143; dependency closure in frozen order (H4) |
| GUARD | 11 | the guard rows (functions 6, 10, 11, 13 to 20) | TD's rule: `if (C) {` to `if (!C) return;` (TD's `negate`), interior one level out, nothing after the `}` in the piece |
| UNWRAP | 1 | P:1052-1106 | a bare block's interior one level out |
| LAMBDA_TO_FUNCTION | 1 | head P:284-287 and close P:332 dropped; body P:288-331 carried | parameter list and return type equal the lambda's head; the body from its first statement (`reg`, P:288) to its last carried character-exact; every capture (`ctx`, `est`, `x_s`, `ccl_h`, `col2desc`, `cov_df`) bound to the object it captured |
| DIRECT_CALL | 6 | P:505, 1001, 1158, 1273, 1438, 1554 | `harvestCoupling(` to `networkHarvestColumn(run, ep, border, `; compared token by token (continuation lines re-aligned by the generator) |
| LOCAL_TO_MEMBER | 4 + 5 | P:227, 228, 231, 276; the layout bindings `cols`, `xr`, `vard`, `hblk`, `pc_row0` | a frozen local becomes a reference to the border or layout member that holds the same object or value (H8 for the layout) |
| RE_EVALUATED | 3 | P:386, 403, 442 repeated in functions 7, 10, 11 | the initializer reads only const inputs nobody writes in between (H5) |
| CONTINUE_TO_RETURN | 4 | P:381, 385, 402, 453 | each `continue` is at the station loop's scope (H7) |
| HAND_FORWARD_DROP, LAYOUT_READ, LAYOUT_RECORD | 49 dropped code lines (30 declarations and sizing, 19 writes with their guards), 11 reads, 1 epilogue | 3.2; P:1135, 1137, 1138, 1250, 1252, 1253, 1426, 1428, 1429, 1549, 1551 | H2 (drop kind), H8 |
| EXTRACT | 3 | P:518-636, 652-678, 1670-1697 (and 1612-1638 through MERGE) | the range is one statement sequence at one scope, replaced by one call at its position; every local it reads arrives as a parameter bound to that local (H3, H6) |
| MERGE | 1 | P:652-678 and 1612-1638 | token-identical under the renaming `idOfZ, znow, zmaxabs, zrhs_new, zrhs_old, zdmax, zmatched, zgone, zold` to their L names (H9) |
| EVAL_POINT | 3 | `amb_state_tr`, `amb_state_dt_ep_s`, `zwd_seam_live` | H5 |
| CALL_SITE | 1 | NetworkLeveling.cpp | the 17 frozen arguments bind what the new preambles bind (H3); the dropped names have no other reader |
| FIELD_MOVE (HV1b) | 5 harvest lines, 4 run-end lines, 2 declarations | 2.6 | H10 |
| CITATION (HV1c) | 9 strings | 2.7 | H12 |

### 3.4 The generator, `scripts/nom_hv1_cut.py`

TD's shape (`scripts/nom_td_cut.py`): one canonical table in the script, the frozen source read
with `git show` at `--base`, generated regions in each home file between markers, a manifest
(`scripts/nom_hv1_manifest.json`) the verifier reads for pairing only, `--survey` (print each row's
resolved range and transform at the base, refuse on a missing anchor), `--check` (idempotence).
A row, in TD's style:

```python
H("networkHarvestSlantIonoBlock", home="filter/NetworkSlantIonoCarry",
  anchor="if (iono_blk_on) {", shape="guard",
  kept=("border", "blocks"),
  subs=[("iblk_of_sta[i]",        "blocks[i].segmentBlock(BlockSegment::Iono)"),
        ("&iblk_cols_of_sta[i]",  "&blocks[i].cols"),
        ("iblk_row0_of_sta[i]",   "blocks[i].rowOf(BlockSegment::Iono)"),
        ("harvestCoupling(",      "networkHarvestColumn(run, ep, border, ")],
  what="network.carry_iono_in_block: ...")
```

Subs and drops are keyed by anchor-relative statement, not by line, so the table survives the
base move. `--base` is the main commit after the deletion and EP1 (and EP1c when it has landed).
The generator, and `--survey` before it, refuses a base where any anchor (3.1's rule: a token
prefix matched at every statement start of the frozen function, at every depth) matches zero
statement starts or more than one, or where the statement it reaches after `at` is not the first
code statement of the row's first piece; and it refuses a table in which a dropped range holds a
code line its kind does not allow (3.1, the kind check). A row carries its drops as
`drops={(first, last): kind}` inside a piece, and the piece is emitted with those lines removed.
The preamble is computed, not written in the table: names read by the body (nom_cpp's identifier
scan inside the function's scopes), mapped through table 1.4, the alias block and the layout
table. What the generator does not write: the header Doxygen prose of 2.1 and 2.3 (hand-written,
checked by H11), the unit tests, HV1b and HV1c (small hand commits checked by H10 and H12).

---

## 4. The relocation verifier (gate G-R), `scripts/nom_hv1_verify.py`

**Reused.**
- `scripts/nom_cpp.py` (branch `nom/ep1-tools`): `GitTree` (frozen base from git, no copy),
  `lex`/`Tokens` (comment-insensitive token streams), `statements`/`stmt_end` (statement pairing
  and the coverage unit), `FileIndex` (each new function's declaration and definition),
  `Scopes` and `Resolver` (bindings: a reference followed to its initializer, a copy as `Copy`),
  `calls_in` (call extraction with argument spans).
- From `scripts/nom_ep1_verify.py`: the binding model and check V3a (per call site, per
  parameter, the storage object derived from the frozen and the new source separately, declarator
  kind included), V1b's "below the bindings equals frozen", V3e (no free `run` or `ep`), V13 (the
  name pre-flight), V6's event vocabulary (calls, log calls with level and format literal), the
  in-memory negative-control harness.
- From `scripts/nom_td_cut.py` / `nom_td_verify.py`: pieces with shift, the guard and unwrap
  shapes with `negate`, coverage over every frozen code line, the manifest pairing.
- `scripts/nom_epoch_lint.py`: V10 extended to the 19 nested harvest functions in epoch form
  (table 2.3 rows 2 to 20; each is called with networkLeveling's own `run` and `ep`, the case
  the lint already handles for the solve's replay builders). The lint recognizes a converted
  seam by its definition, first parameter `NetworkRun&` and second `const NetworkEpoch&`
  (its module docstring), so the two run-only functions are outside V10 by construction: they
  read no epoch group, and V10 has nothing to check in them.
- `tests/test_network_epoch_contract.cpp`: the 20 epoch-form harvest signatures pinned (the
  orchestrator and rows 2 to 20; each takes `const NetworkEpoch&` and no epoch output), and,
  separately, the two run-only signatures pinned as declared:
  `void networkResumeCouplingCensusRow(NetworkRun&, const LocalCarry&, const std::vector<CoupledShared>&)`
  and `void networkResumeFirstEpochRetire(NetworkRun&)` (no `NetworkEpoch` parameter of any
  kind). `harvestBlockSpans` is pinned by its own unit test (5).

**New checks** (exit 0 only when all pass):

| check | proves |
| --- | --- |
| H1 bodies | each function = its preamble, then the frozen pieces with their declared drops removed, their shifts and shapes, the declared substitutions (token comparison on substituted statements, character-exact elsewhere) and the declared inserts, nothing else |
| H2 coverage and drops | every frozen code line of P:45-1915 is claimed exactly once: carried (a piece minus the drops it contains), replaced, or dropped; a drop is subtracted from the piece that holds it, so a line both carried and dropped is a double claim and fails. Every dropped code line is the text of its declared kind (3.1: `lambda_head`, `lambda_close`, `hand_forward`; the signature and alias block as replaced); a dropped range that holds any other statement fails, so a body statement cannot be dropped silently. The anchor of every row resolves to exactly one statement start of the frozen function and, after `at`, to the row's first statement (3.1) |
| H3 bindings | every preamble line is a PARAM_BINDING whose object equals the frozen call site's (through networkLeveling's EP1 preamble), an ALIAS_REPEAT line, or a declared border or layout binding; every EXTRACT call's argument binds the local of the same name at the frozen position; the caller's dropped names have no other reader |
| H4 closure | every preamble initializer reads only names bound above it, `run`, `ep`, `border`, `blocks`, `sb` or `i` |
| H5 evaluation point | for the three run-state aliases and the three re-evaluated loop-head lines, no statement between the frozen evaluation point and the new one (in the reconstructed order of H6) writes what they read (`pl_prev_now_s`, `zwd_seam_ep_left`, `n_loop_epochs`, `asm_res`, `est_index_of`, `ctx.knobs`) |
| H6 order | inlining every harvest call back into the orchestrator (preambles stripped, transforms reversed, the MERGE by its renaming) reproduces the frozen statement sequence; hence the ordered calls (including the six `networkSharedHistoryNoteEntry` and the clone calls), log calls, `prmRespond` sites and container writes are equal |
| H7 control flow | each CONTINUE_TO_RETURN `continue` has the station loop P:380 as its innermost loop; each new `return sb;` is at the function scope of networkHarvestStationBlock; no other `continue` or `break` changed target |
| H8 layout | each LAYOUT_READ names its own kind's segment (`iblk` Iono, `rblk` RxClock, `cblk` RxCodeBias, `zblk` Zwd); the epilogue passes the frozen row-offset locals in segment order; `segmentBlock` and `harvestBlockSpans` are the declared definitions (their tests pass) |
| H9 merge | the L and Z census bodies are token-identical under the declared renaming; the merged function equals the L body with the one substitution |
| H10 field move | every frozen `ctx.carry.` token of the four fields became `ctx.counters.`, the declarations moved with identical declarator and initializer, no occurrence remains (Grep and `git grep`); TD's verifier passes with the four new subs |
| H11 declarations | one declaration per function in its home header with `@brief` and every `@param`; the old declaration in NetworkRunContext.hpp gone and its seven-file map updated; the six new `.cpp` files listed in `CMakeLists.txt` |
| H12 citations | each changed kTable "where" string names the file that holds that site's `prmRespond` call |

**Negative controls**, pre-registered (each mutation of the candidate tree must fail its check
before a pass is believed): C1 the receiver-clock harvest called before the ionosphere's (H6);
C2 phase colour in its own station loop after the ambiguity loop, the SCL2 note interleave of
inventory risk 1 (H6); C3 the ionosphere harvest reading `BlockSegment::RxClock` (H8); C4 the
stamp moved before the station loop (H5, H6); C5 a `continue` of the per-ambiguity loop turned into
`return` (H7); C6 `const Eigen::VectorXd x_s = ep.solve.x_s;` (a copy for a reference, H3); C7
`pl_now_s` bound to `ep.clock.dt` (H3); C8 the merged census accumulating `rhs_old` over the fresh
coupling (H9); C9 the ZWD prune's PRM response dropped (H2); C10 a guard's negation lost (H1); C11
an alias line edited (`pl_max_gap_s = ctx.knobs.pl_q_amb`, H3); C12 the Z call passing the L entry
(H3); C13 a counter field moved with another initializer (H10); C14 a kTable string naming the wrong
file (H12); C15 networkHarvestColumn generated without the declaration of `reg` (revision 1's
head 284-288 declared as the drop; H2 fails on the drop kind and H1 on the body); C16 a
hand-forward drop stretched over P:492-494, which removes the clone registration (H2, H6); C17
a table whose station-block anchor is `if (est_index_of[i] < 0) continue;` (the generator's
`--survey` refuses: nine matches). The table-level twins of C15 to C17 already fail in
`hv1_table.py --controls` (C-T1 to C-T4). Positive control: on the base alone (nothing cut)
every check that applies passes and the rest report n/a.

**What G-R cannot see, and who covers it.** A layout read that is value-equal on every arm but
wrong in a posture no arm runs: the unit tests of `harvestBlockSpans` and `segmentBlock` and the
witness arms `ionoblk`, `rxclkblk`, `zwdblk`, `lgm` (their F3 run-end lines count missing cross
terms and rows outside the block). The order of floating-point accumulation inside a moved body is
unchanged by construction (character-exact), and across bodies by H6.

---

## 5. Gate arms (G-EP, `scripts/nom_td_gate.py --whole`, same-build Release, renamed executables)

Base and candidate built from the same tree, both run on D: (`RAPID_RUNS_ROOT`). Arms are made
from the post-deletion goldens with `scripts/rapid_arm.py make` (card NOM's family smokes,
`runs/_rap_n13*`, are re-made; a deleted key in an old arm is ignored by the loader, so
an old arm would silently test less). Every arm compares the whole stdout and main_log, every CSV
(`network_amb_state.csv`, `network_amb_if.csv`, `network_phase_colour.csv` and the prior-residual
CSVs are the harvest's), the manifest, `rt_engine.log` and the exit code; HV1c's arms compare after
reversing the nine-string substitution, as HV0's G1 did.

| branch (function) | arm(s) | evidence |
| --- | --- | --- |
| the not-warm path: ledger, RXCB-DATUM census, rx-CB carry, latch only (1, 6, 16, 17, 22) | every arm's first `phase_leveling_warmup_epochs` (default 6, `src/core/config/GnssConfigLoadNetwork.cpp` 5013) emitted epochs | run starts |
| handover ledger (6) | `f2`, `s2`; off on `bridge` (eq34 turns it off, `NetworkKnobs.cpp` 438) | handover run-end lines |
| border with coupling, column, layout amb + rx-CB segments (2, 3, 4) | `f2`, `s2` | coupling and RXCB-IN-BLOCK lines |
| VtecSh identities in the border (2) | `sh` (estimate_iono_sh with the coupling carry) | SH lines |
| clone rows, blocks, end (3, 4, 5) | `bcl`, `shx` | SCL run-end line |
| SCL2 notes and compression (5) | `shx` | SCL2 run-end line |
| legacy diagonal path, `vard` (4, 7, 18) | `replay`, `diagpri` (coupling off) | n_cpl_* stay 0 |
| pivot skip, prioritized datum off (4) | EP1's `legacy` | its PIVOT lines |
| ambiguity stash, drop walk, amb-state rows (7) | `f2`, `s2` | network_amb_state.csv |
| amb-state stride (7) | **stride** (new: `network.trace_amb_state_every` 4) | event rows between strides |
| birth floor (7) | **floor** (new: `network.amb_float_birth_var_m2` 0.25) | `n_amb_birth_floored` |
| restored sentinels, the latch and its retire (7, 22) | `snapr` | its "C1 dense border, warm resume" line (413 carried keys on `runs/_rap_g2_ref2_snapr`): the dense-carry resume that sets the latch (`NetworkWarmResume.cpp` 1128-1131 at main), so the first-fold CTA count of 22 is written |
| L and Z post-resume census through the merged function (7, 18, 21) | **cplw** / **cplr** (new pair). `cplw` = `snapw` plus `network.state_snapshot_frame_capture` and `network.state_snapshot_coupling_capture` (QS / QL / QZ records). `cplr` = `snapr` resuming `cplw`'s snapshot plus `network.resume_frame_restore` and `network.resume_coupling_restore` | the "resume coupling restore" run-end line: L rows > 0 and Z rows > 0 restored, and `n_resume_cpl_census_rows` > 0 |
| the L call and the Z call of 21 told apart (7, 18) | **cplr_l**, **cplr_z** (new): `cplr` on a copy of `cplw`'s snapshot with every QZ line, or every QL line, deleted | `cplr_z`: 0 L rows restored, Z rows > 0, census rows > 0, which only the Z call can produce; `cplr_l` the converse |
| monitor feeds (8) | `f2`, `s2`; `vce` (pl only) | VCE and prior-residual lines |
| ambiguity prune, roster deaths, prune after drop (9) | `f2`, `s2` | retirement census lines |
| AIL trace (10) | **ail** (new: `network.trace_amb_if`) | network_amb_if.csv |
| phase colour stash and prune (11, 12) | `lgm`; **lgmt** (new: `lgm` plus `network.trace_phase_colour`) | LGM line, network_phase_colour.csv |
| in-block slant ionosphere (13) | `ionoblk` | its F3 lines |
| receiver clock, drift, gate census (14) | `rxclkblk`; **rxjump** (new: `rxclkblk` plus EP1's injected `rx_clock_step`) for the jump erase | `n_rxclk_carry_refused` > 0 |
| rx-CB in block, step test, L5B erase (15) | `f2`, `s2`, `tri`, `gal` | RXCB-IN-BLOCK line, L5B `reseed_erase` |
| rx-CB diagonal stash and its prior-residual row (17) | `diagpri`, `replay` | rx-CB carry line |
| ZWD with coupling and block row, `zwd` feed (18) | `zwdblk` | ZWD lines |
| ZWD capture-only (18) | `snapw` | snapshot bytes equal (UTC masked) |
| ZWD seam window and its retirement line (19) | **zseam** (new: `snapr` plus `network.zwd_seam_reseed`, window shorter than the arm; like `snapr` it does not reach the census) | `n_zwd_seam_folds`, the RETIRED line |
| eq34 RPB (20) | `bridge` | `n_cr_rpb_gap_prune` line |
| week arithmetic in the rows (7, 10, 11) | EP1's `roll`, if built | rows at the boundary |

**Why `snapr` does not reach the census, and what the census arms rest on.** `snapr`
(`runs/_rap_g2_ref2_snapr/rapid.json`) sets the snapshot path and capture keys and no `resume_*`
key; `network.resume_coupling_restore` defaults to false (`GnssConfigLoadNetwork.cpp` 5732-5734
at main) and its stdout says neither `resume_border_restore` nor `resume_coupling_restore` is
armed. A restored `LocalCarry` then has an empty `coupling` (its member comment,
`include/filter/NetworkRunTypes.hpp` 620), so both census gates are false (P:651
`!old->second.coupling.empty()`, P:1611 `!zold->second.coupling.empty()`) although the latch is
set, and `n_resume_cpl_census_rows` stays 0; the census line itself is printed only under
`config_.networkResumeCouplingRestore()` (`NetworkResumeRunEnd.cpp` 103 at main). The new arms
rest on these checked facts: the coupling capture requires the frame capture
(`GnssConfigLoadNetwork.cpp` 3361-3367) and the coupling restore requires the frame restore,
`carry_conditional_coupling` and `carry_coupling_gauge_project` (both pinned on the field
golden) and `amb_seam_reinflate_var_m2` 0 (its default) (5735-5767); the restore sets the latch
from the rebuilt rows (`NetworkWarmResume.cpp` 954); QZ records exist without the ZWD carry
because a snapshot path arms the capture-only ZWD harvest (`NetworkKnobs.cpp` 156), which stashes
`coupling` (P:1641-1642), and the reader's assembly records `pl_zwd_present` on capture epochs
(`ApplicationNetworkAssembly.cpp` 5514-5523); a snapshot with its QL or QZ lines deleted is a
valid input, since each QL / QZ line is checked on its own against its L or Z row and no digest
covers the file (`src/core/products/NetworkStateSnapshot.cpp` 581-610, 786-802). The writer's
snapshot is about 37 times the plain one (the loader's own estimate, about 10 MB at 63 stations).
If any of `cplr`, `cplr_l`, `cplr_z` shows a zero census count, the body it was to reach (the L
copy, the Z copy, or both, and with them the merged function) moves to the list below with H9
and H6 as its only, static, coverage, and the result says so.

**Named unreached unless an arm shows a count:** every `catch (...)` after a reconstruction or
variance call (P:189, 453, 1148, 1263, 1431, 1535, 1727, 1821) and the regression's own (P:290), plus the column-lookup catches of the border (P:238-261); static coverage by H1 and H6;
a failed coupled-column regression (`n_cpl_harvest_fail`, `n_pc_stash_fail`,
`n_rxcb_blk_harvest_fail`); the receiver clock's past-horizon rate reset (P:1353); the rx-CB in-block
10-sigma step reset and both L5B erases unless `f2`, `tri` or `gal` count them; the defensive
row-outside-block branches (P:1010, 1188, 1385, 1497); `n_rxcb_blk_no_block` (P:1504); the ZWD
diagonal `vard.size() == 0` branch (P:1558). Each goes
into the card's result section with its static coverage.

**Suite** passes (new `tests/test_network_harvest_layout.cpp`: `harvestBlockSpans` for every
combination of present segments, `segmentBlock` null without a block or without columns, the
ZWD always last; `tests/test_rx_cb_datum_gauge.cpp` moved onto the test run fixture, 8.D4).
**Cost**: an idle-machine ABBA pair on both 2 h goldens within 1 %; expected nil (no new solve or
allocation in the hot path beyond one `StationHarvestBlock` per station-epoch, moved not copied).

---

## 6. Work split and order

**Prerequisites.** (a) The REMOVE-key deletion on main: it removes 731 of the function's lines
and the pre-pass, and its integration resolves the M:243 comment conflict. (b) EP1 on main:
`NetworkRun`, `NetworkEpoch`, networkLeveling in seam form, `scripts/nom_cpp.py` and the EP1
verifier and lint. (c) EP1c if it is scheduled first (then `est` is const at the base). (d) A
test run fixture that builds a `NetworkRun` and a `NetworkEpoch` (EP2's planned
`tests/support/NetworkTestRun.hpp`), or HV1 brings its minimal form (8.D4).

**Steps, one worker** (the generator writes every file from one table and the orchestrator is
shared, so parallel workers would only hand-merge generated text):
1. **HV1-0, tooling** (scripts only): `scripts/nom_hv1_cut.py`, `scripts/nom_hv1_verify.py`, the
   lint and contract-test extensions. Can be built now against a scratch tree that merges the five
   track branches (as `post.cpp` here); `--survey` re-run on the landed deletion before HV1a. The
   table, its anchor rule, its drop kinds and its five table-level controls start from
   `nextsteps/hv1/hv1_table.py`.
2. **HV1a, the cut** (one generated commit): the spine header and file, six new owner pairs, the
   three existing owner files, networkLeveling's call, `CMakeLists.txt`, the comment citations
   (2.7), `tests/test_network_harvest_layout.cpp`, the moved test. Gates: G-R, the suite, G-EP on
   every arm of section 5.
3. **HV1b, the counter move** (hand commit, H10 and TD's verifier): NetworkRunContext.hpp, the two
   harvest files, the rx-CB run-end report, `nom_td_cut.py` subs, `nom_td_manifest.json`. G-EP on
   the core arms (`f2`, `s2`, `zero`, `diagpri`).
4. **HV1c, the citation text** (hand commit, H12): NetworkResponses.cpp. G-EP on the core arms
   after reversing the declared substitution.
5. **HV1d, docs**: the card's dated result (the measured counts, the arms, the unreached list), the
   NetworkRunContext.hpp file map, CLAUDE.md's network tour sentence on the harvest and its
   line counts.

**File ownership against the neighbouring steps.**
- EP1 (W4) owns `NetworkLeveling.cpp` until it lands; HV1 starts after.
- SG (NetworkSolveGlue and its new homes), PB1a (ApplicationNetworkAssembly.cpp's parameter
  groups), EP2's driver blocks, EP3 and EP4: disjoint from HV1; in parallel by file.
- PB1b puts PASS B's half of each contribution family into the owner files HV1 creates
  (`NetworkZwdCarry`, `NetworkRxClockCarry`, ...): HV1 lands first.
- TD2 (one run-end report per subsystem) then moves the phase-colour, ZWD, ionosphere,
  receiver-clock and RPB lines of `networkLevelingRetirementRunEnd` and `networkPhaseColourRunEnd`
  (`src/core/filter/NetworkLevelingCarryRunEnd.cpp`) beside their harvests: after HV1.
- `include/app/NetworkRunContext.hpp` is shared with EP1 (three declarations) and the deletion;
  HV1a touches only the harvest's declaration, the two resume declarations and the file map;
  HV1b the four fields.
- HV2 (NOM step N4's entity tables and the explained moves of section 7) follows HV1.

---

## 7. Deviations from NOM3 section 3, each with its reason

1. **22 functions and a helper in one spine file and nine owner files**, not "about 25 in nine":
   two of the inventory's owners have nothing left (the lane observatory, C7; the level authority's
   LV-1 snapshot, C19), and five kinds are gone (lane, diagonal ionosphere, LV-1, LV-4, S2H with
   LV-2/LV-5). The spine (border, column, layout) is the tenth file because every kind uses it.
2. **The orchestrator takes two parameters**, `(NetworkRun&, const NetworkEpoch&)`, not four: the
   four (context, configuration, an epoch view, the two engines) are what EP1's run and epoch
   already hold; a `LocalHarvestEpoch` view would be a third spelling of EP1's groups.
3. **Four rx-CB run fields move, not 26**: three of the 26 counters survive and the other 23
   (`lv1_field_snapshots`, `lv2_*`, `lv4_*`, `lv5_*`, `s2h_*`) are deleted with C19; the tally map
   `rx_cb_sta_tally` (not among the 26) moves with the three. The move is its own commit (HV1b) so HV1a stays a pure relocation.
4. **The layout is recorded per station inside the station loop**, immediately before that station's
   rows, not in a separate pass over all stations: the output is the same either way (the clone
   block list is read only at its end and the rows are keyed by station and column), but the
   per-station form keeps today's statement order and needs no argument.
5. **PASS B does not read the layout yet.** The declared value is shared by the harvest's kinds; the
   two-file block-gate contract with PASS B stays documented on both sides until PB1b.
6. **Added**: the L and Z post-resume census merged into one proven function; the ambiguity
   monitor feeds and the ZWD seam window extracted in place; a declared output text move (nine
   site citations), as HV0.
7. **Harvest-local values travel as kept parameters.** The border, the layout vector, the station
   index and the per-ambiguity locals an extracted body reads (`pa`, `value_abs`, `var`,
   `coupling`) live for one harvest only, so they are neither run state nor epoch members; they
   occupy the seam rule's "kept" slot, and the epoch stays const in every harvest function.
8. **Left for HV2** (each is a rewrite or an output move, not a relocation): one
   `reconstructStation` per station (8 calls per station-epoch today); the six F3 row writes as one
   helper; the receiver clock's drift step as a pure function (it needs a `continue` out of a
   helper); the amb-state trajectory row out of the stash (it writes `n_folds` back); the three
   direct `config_` reads as knobs (P:80, 253, 255); the rx-CB produce tallies split from the
   diagonal stash; emitting the harvest's buffers from the kinds (would reorder CSV rows).

---

## 8. Decisions for the author (defaults in parentheses)

- **D1.** Orchestrator form: EP1's `(run, ep)` (default) or the card's four-parameter form with a
  harvest view.
- **D2.** The L/Z census merge in HV1 under check H9 (default) or two verbatim copies merged in HV2.
- **D3.** Per-station layout in the station loop (default) or the inventory's separate layout pass.
- **D4.** The unit test `tests/test_rx_cb_datum_gauge.cpp`: onto a test run fixture, calling the
  orchestrator with `carry_warm` false so its "counts once" case still covers the whole harvest
  (default; HV1 waits for EP2's fixture commit or brings its minimal form), or a narrower
  explicit-form census adapter kept until EP2.
- **D5.** Const in the preambles: `est` const (default; EP1c requires it), `stations` frozen
  non-const (two kinds write `l5b.reseed_erase`); further tightening in a compile-proven commit.
- **D6.** The citation text move as its own commit with a declared substitution (default, HV0's
  precedent) or the strings left naming the old file.
- **D7.** Owner of the RXCB-DATUM census: `NetworkRxCodeBiasCarry` (default: the receiver DCB at the
  harvest) or `NetworkSbasis` (the datum's owner, which reads the count at run end).
- **D8.** The harvest functions in EP1's contract test and lint (default yes: it pins that the
  harvest writes no epoch member): the 20 epoch-form functions in the contract test, the 19
  nested ones under the lint's V10, and the two run-only resume functions pinned separately in
  the contract test (outside V10, which recognizes only epoch-form definitions).
- **D9.** Build the ten new arms (`ail`, `lgmt`, `floor`, `stride`, `zseam`, `rxjump`, and the
  census family `cplw`, `cplr`, `cplr_l`, `cplr_z`) (default yes) or name those branches
  statically covered only; without the census family, both census copies and the merged
  function are named unreached with H9 and H6 as their only coverage.
- **D10.** `resume_cpl_census_rel` and `_rhs` (run-level census samples filed in carry state): leave
  for N4 (default) or move with the rx-CB fields in HV1b.
- **D11.** The stale clause the deletion leaves in the stash comment (P:691-692, birth_ep as "the
  clock of the F3 block admission age gate", whose gate C22 removes; P:688-690 is the code
  statement `const double var_first = ...`): fixed in HV1a as a declared comment edit located
  by anchor on the landed deletion, never by line (default), or by the deletion's own review.

---

## 9. Risks

1. **The base is not on main.** The deletion is paused; two tracks end in ungated review rounds
   (`rmk/I` `6923a8e`, `rmk/R1` `45e93a7`) and two tracks conflict on one comment line (M:243).
   Every P coordinate, count and size here is provisional; `--survey` on the landed deletion is
   the first step of HV1a.
2. **Floating-point order across kinds.** The SCL2 compression and the clone end read rows in note
   and registration order; the orchestrator keeps today's order and H6 proves it, control C2 is the
   failure the per-station interleave prevents.
3. **Evaluation points.** Three aliases read run state; a later edit that writes `pl_prev_now_s` or
   `zwd_seam_ep_left` before their readers would change them silently. H5 runs on every tree.
4. **Two-file contract.** The station-block gate (P:399-402) and PASS B's block build
   (`ApplicationNetworkAssembly.cpp` 4668-4669) must agree; HV1 documents it on both sides, PB1b
   makes it one declaration.
5. **Coverage.** About half the code lines are unarmed on the goldens and several event branches
   (receiver clock jump, rx-CB step reset, L5B erases, the catch paths) are reached only if an arm
   counts them; G-R covers them statically. The one non-verbatim body merge (the L and Z census
   into function 21) is reached only by the coupling-restore arms `cplr`, `cplr_l`, `cplr_z`,
   none of which has been run; two of them read a hand-edited snapshot copy, a test input
   valid by the reader's own per-line checks.
6. **Sequencing.** EP1c's const estimator forces a harvest declarator change if it lands first;
   the test fixture is EP2's; PB1b and TD2 edit the owner files HV1 creates.
7. **Anchor drift.** An anchor unique on `post.cpp` may repeat on the landed deletion (the
   review rounds of `rmk/I` and `rmk/R1` are ungated). The generator refuses rather than
   guesses (3.4), so drift costs a table edit, not a wrong cut.

---

## 10. Revision 2: review findings and their dispositions

Each finding was checked against `post.cpp`, the arm directories and the composed
post-deletion tree before any change; all six were accepted. The anchor finding was wider than
reported.

| # | finding | check | change |
| --- | --- | --- | --- |
| 1 | The cut table dropped P:288, the lambda's first body statement and the row's own anchor; the drop count was inconsistent; the table script never subtracted a drop from its piece. | Confirmed: the head is P:284-287 (P:287 ends `-> std::pair<...> {`), P:288 declares `reg`; 284-288 plus 332 is 6 code lines. | Head 284-287 and close 332 dropped (5 lines), body 288-331 carried (3.1, 3.2, 3.3, table 2.3). Drops are subtracted from their pieces, and every dropped code line is validated by kind in H2, the generator and `hv1_table.py`; controls C-T1, C-T2, C15, C16. |
| 2 | `snapr` cannot reach either census body; table 1.3 said otherwise. | Confirmed: `snapr` arms no `resume_*` key; restored entries carry empty `coupling`, so the gates at P:651 and P:1611 are false; the census print needs the coupling restore. `snapr` does set the latch (its dense-carry resume), so function 22 stays credited to it. | New arm family `cplw` / `cplr` / `cplr_l` / `cplr_z` with its evidence and the facts it rests on (section 5); table 1.3 rows H2e4, H2n, H6 corrected; a fallback to named unreached if any shows a zero count; D9 and risk 5. |
| 3 | Several anchors were not unique. | Confirmed and wider: besides the station block (9 matches) and the bare `{` (4), the ionosphere, receiver-clock and rx-CB block guards each match twice (the dropped sizing blocks P:353, 366, 375). | Anchor rule stated (3.1): a token prefix matched at every statement start, exactly one match, an optional `at`; five rows re-anchored; 23 of 23 resolve (`hv1_table.py`, `hv1chk/anchors.py`); the generator refuses otherwise (3.4); controls C-T3, C-T4, C17. |
| 4 | Counts and sizes did not reproduce or were not post-deletion. | Confirmed: P:468-491 holds 19 code lines (49 drops in all); variance calls are 8 and 7 (two comment mentions); `NetworkWarmResume.cpp` composes to 1 402 lines. Found in the re-measure: the `continue;` count had the same comment-mention error (105 and 62, not 107 and 64). | Table 1.1, 3.2, 3.3 and 2.3's size estimates corrected; existing owner files measured on the composed tree (`hv1chk/compose/`). |
| 5 | D11 cited P:689-690, which is code. | Confirmed: P:688-690 is `const double var_first = ...`; the clause is at P:691-692. | D11 cites P:691-692, to be located by anchor on the landed deletion. |
| 6 | The signature form and its counts were stated inconsistently. | Confirmed: rows 21 and 22 are run-only; the lint recognizes only `(NetworkRun&, const NetworkEpoch&)` definitions. | Three forms stated (2.3, section 0): 20 epoch-form (19 nested), 2 run-only, 1 pure; contract test pins the 20 and, separately, the two run-only signatures; V10 covers the 19 (section 4, D8). |

