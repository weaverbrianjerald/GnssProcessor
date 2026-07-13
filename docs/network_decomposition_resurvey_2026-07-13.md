# ApplicationNetwork.cpp Decomposition — Re-survey (2026-07-13)

**Status:** DRAFT for review (uncommitted). Supersedes the Jul-4 survey
(`network_decomposition_plan.md` / `network_decomposition_cutmap.md`), which was
taken against the now-obsolete freeze SHA `d0036983`.

**Method:** two multi-agent survey workflows (16 + 4 agents) mapping the current
file region-by-region, reconciling symbols against the Jul-4 cut map, extending
the state inventory, re-checking the Tier 1/2/3 rules, and confirming the
sequencing gates. All line numbers verified against **HEAD `a4c9af4`** (Card 4
D5 v2). Load-bearing claims spot-checked by hand (foldAmb destination,
fingerprint coverage).

---

## 0. Headline

`ApplicationNetwork.cpp` grew **7,265 → 11,739 lines (+4,474, +62%)** since Jul-4.
`runNetwork()` is now a single ~11,100-line function (605 → EOF) with ~55 lambdas.

The growth is **concentrated, not diffuse**: git-confirmed as +4,512 insertions
across 30 post-Jul-4 commits, ALL from four Card workstreams the original plan
never saw. ~2,300 of those lines are genuinely new logic:

| Workstream | ~Lines | Lands in | Effect on plan |
|---|---:|---|---|
| Card-3 eq34 chain-datum election | 812 | PhaseDatum | **triples** it (400→1,200) → sub-split |
| Card-1/1b/1d conditional coupling | 350 | Assembly | pushes Assembly →1,800 → new seam |
| Card-4 carry driver + D5 gate + phase-D rebase | ~400 | **new** InvariantCarryDriver + Emit | new seam |
| M2 LS-VCE carry monitor | 110 | cross-cutting | no single Jul-4 home |

**Verdict: the Jul-4 domain-first seam map still holds** — every new block routes
to an existing or one-adjacent seam, no unit was deleted or fundamentally
relocated (287/288 original units are present-shifted with unchanged
destination). But three structural additions and a re-freeze are required.

---

## 1. Updated destination-TU inventory

`tu : estLinesNow (jul4 → Δ)` — sums of units assigned across all regions.

| Destination TU | Now | Jul-4 | Δ | Key spans (HEAD) |
|---|---:|---:|---:|---|
| `config/GnssConfigLoadNetwork.cpp` | 2,480 | 1,780 | +700 | `loadNetworkSection_` 2866–5127 |
| `filter/NetworkRunState.hpp` | 530 | 330 | +200 | persistent decls 1870–2213, counters/knobs 2362–2525 |
| `filter/NetworkRunTypes.hpp` | 300 | 280 | +20 | NetStation 127–178, StationAssembly 311–360, ShDevCarry 2760 |
| `filter/NetworkRobustStats.hpp` | 200 | 190 | +10 | robustProminence 507 (template), medianPolish2Way 538 |
| `slip/NetworkSlipQc.{hpp,cpp}` | 660 | 620 | +40 | refloat_signal 3720, refloat_pair 3823 |
| `logging/NetworkRunTraces.{hpp,cpp}` | 1,950 | 1,650 | +300 | dd_frac 9299, vtec_sh_health 9602, carry_vce CSV |
| `app/NetworkRunContext.{hpp,cpp}` | 1,160 | 1,000 | +160 | fingerprint 2982–3089, restore 3091–3253, harvest 9871, snapshot 11192 |
| `app/ApplicationNetworkSetup.cpp` | 820 | 850 | −30 | buildStation 997, product/brdc-rts stage |
| `app/ApplicationNetworkAssembly.cpp` | **1,450** | 1,200 | +250¹ | splitStation 2536, iono appenders 2671–2890, **do_fold pins 5195–5314**, PASS-B produce/consume |
| `filter/NetworkPhaseDatum.{hpp,cpp}` | **~390** | 400 | −10² | buildEnsembleGauges 6032, healNearNullPhaseBias 6726 |
| `filter/NetworkChainDatum.{hpp,cpp}` **(NEW)** | 812 | — | +812 | eq34 election 4216–4903 (carved from PhaseDatum) |
| `filter/NetworkCoupling.{hpp,cpp}` **(NEW)** | 350 | — | +350 | buildCplRow 5404–5647, regcol/harvestCoupling 9790–9870, whitenCouplingRows 373 |
| `filter/NetworkInvariantCarryDriver` (glue) **(NEW)** | ~200 | — | +200 | buildCarryDdPriors 6113–6177, beginEpoch 6178–6187, installPosterior 7877–7929, D5 seed 3491 |
| `filter/NetworkEstimator` (additions) | 200 | 150 | +50 | AR Phase-2 diag 7931–8081 |
| `filter/NetworkSbasis` (additions) | 500 | 170 | +330 | W6 code-bias datum 6189–6494 (+Card-2 prevCb time-link) |
| `filter/NetworkRaim.{hpp,cpp}` | 1,350 | 1,300 | +50 | assess 7417, resolve_dropping 7497, post-fit 10067–10529 |
| `filter/NetworkClockQc.{hpp,cpp}` | 700 | 650 | +50 | relevel 6935, osc block 10531–10689, monitor 10691 |
| `app/ApplicationNetworkEmit.cpp` | 1,250 | 800 | +450 | emit 8083–8754, consumer iono 8995–9159, stash 10918 |
| `app/ApplicationNetworkEmit.cpp` (rebase, 11b) | 118 | — | +118 | phase-D NetworkEmitRebase wiring 8801–8940 |
| `orchestrator survivor` | ~640 | 550 | +90 | includes 1–117, epoch-loop skeleton 3264–3409, solve 7306, teardown 11286–EOF |

¹ Assembly is 1,800 raw; the **−350 coupling carve-out (NetworkCoupling)** brings
it to ~1,450. The `do_fold` pin block (see §2c) stays in Assembly.
² PhaseDatum shrinks below its Jul-4 estimate once the **812-line eq34 block is
carved into NetworkChainDatum**; the residual (ensemble gauges + heal) is ~390.

---

## 2. New material & the three structural changes

### 2a. NEW seam `filter/NetworkInvariantCarryDriver` (glue) — Card 4
The driver **class** is already a committed, Fable-reviewed TU
(`@282eba2/@a4c9af4`: `buildDdPriors/installPosterior/beginEpoch`). What remains
in `ApplicationNetwork.cpp` is its ~200-line call-site glue:
`buildCarryDdPriors` (6113–6177, builds `GroupInput` from the eq34 tree →
`est.setDdPriors`), `beginEpoch`+z-store predict (6178–6187), `installPosterior`
+ D5 flag walk + counters (7877–7929), and the D5 innov-pending seed (3491–3494).
Absent from the Jul-4 plan entirely.

### 2b. NEW seam `filter/NetworkCoupling` (recommended) — Card 1/1b/1d
A cohesive ~350-line cluster whose **injection** (`buildCplRow`+`resolveSharedCol`
+`couplingId/Q`+`TuEnt`+`tuGaugeRestore`+`tuReinitSince`, 5404–5647) and
**harvest** (`regcol`+`harvestCoupling`, 9790–9870) sit ~4,000 lines apart but
share the `col2desc`/`white_map`/`CoupledShared` contract + `whitenCouplingRows`
(373). Leaving it in Assembly pushes that TU to ~1,800; carving it keeps Assembly
≤~1,450. PODs `CoupledShared`/`CplRowRaw`/`PendCplRow`/`TuEnt` → RunState/RunTypes.

### 2c. Sub-split `filter/NetworkChainDatum` out of PhaseDatum — Card 3
The eq34 chain-datum election (4216–4903: 12 lambdas + ~688L inline union-find /
sticky-tree BFS / shift-walk S-transform) is the **single largest new block** and
triples PhaseDatum. It fits the anchor-election seam by scope but is big enough to
stand alone. Provenance **Card-3, not Card-4**.

### 2c-CORRECTION. `foldAmb`/`foldRpb` are Assembly, NOT the carry driver ✅ verified
The phase-1 synth initially listed `foldAmb`/`foldRpb` (5203–5227) under the
InvariantCarryDriver glue. **Hand-verified false.** They are nested inside the
`do_fold` block (5195–5314) in the OpenMP per-station produce loop and build
**eq34 chain-datum hard gauge pins (w=1e10)** directly into the station normal
equations (`R.A_r/R.w/R.y`), consuming `pinned_amb_targets`/`pinned_rpb_keys`
(eq34 election output) + `rpb_carry`. The `target==0.0 ? -piv_x : target-piv_x`
form is a deliberate OFF-golden-hash preservation. This block is **assembly-
resident application of the chain datum** → it moves with **INC 7 (Assembly)**, or
becomes a `NetworkChainDatum::applyPins(R, station, …)` helper *called from* the
assembly loop. It must **NOT** move into INC 9b. The genuine carry-driver builder
is `buildCarryDdPriors` (6113), which uses `carry_driver.buildDdPriors()` — the
GMGᵀ reduced-DD fold, a different mechanism. **INC 9b's member list drops
foldAmb/foldRpb.** (This is exactly the mislabel that would have broken the OFF
golden during extraction.)

### 2d. M2 LS-VCE monitor — cross-cutting
No single Jul-4 home: state (`carry_vce_est`) → RunState/RobustStats; the two
`addRow` feeds (9926 + 11086) → Assembly-harvest + M2-stash; hour-flush + run-end
writers → Traces. Needs an explicit placement decision (see §4 ordering rule).

---

## 3. Revised increment plan

Jul-4 numbering preserved where it maps; **7b/8b/9b/11b** are new. Sequence
**8b (chain datum) and 9b (carry driver) BEFORE finalizing INC 9 (RAIM)** —
`resolve_dropping` calls sibling `buildEnsembleGauges` + `buildCarryDdPriors`, so
the three must land in a consistent include order or the RAIM re-solve replay
diverges from the primary solve.

| INC | Title | ~Lines | Tier | Δ vs Jul-4 |
|---|---|---:|---|---|
| 0 | Preconditions + harness + cut-map regen + **6 ON-path goldens** | 220 | milestone | grown — matrix must exercise 6 new toggles |
| 1 | Config split → `GnssConfigLoadNetwork.cpp` | 2,480 | mechanical | grown 1,780→2,480, same clean move |
| 2 | Type hoist (+7 new coupling/T5 PODs; unit tests) | 230 | mechanical | grown |
| 3 | Logging unit (+dd_frac, vtec-health, carry-VCE writers) | 1,950 | mechanical | grown 1,650→1,950 |
| 4 | Context + carry lifecycle + RTB-7 trio | 1,160 | **milestone** | state inventory RE-DERIVED (§4); fingerprint byte-sensitive |
| 5 | Setup TU | 820 | mechanical | ~flat |
| 6 | Slip QC (+C3c `carry_slipped_ambs` feed) | 660 | mechanical | +shared channel |
| 7 | Assembly TU (excl. coupling→7b, eq34→8b; **incl. do_fold pins**) | 1,450 | milestone | grown even after carve-out; /bigobj likely |
| **7b** | **NetworkCoupling seam (NEW)** | 350 | milestone | NEW — Card-1 |
| 8 | Existing-seam moves (heal→PhaseDatum; descriptors→Estimator; W6→Sbasis) | 700 | mechanical | Sbasis 170→500 |
| **8b** | **eq34 chain-datum → NetworkChainDatum (NEW)** | 812 | fable-only | NEW — largest new block; ~8 mutable-ref params |
| 9 | RAIM unit | 1,350 | milestone | +50; depends on 8b+9b landing first |
| **9b** | **InvariantCarryDriver glue (NEW)** — buildCarryDdPriors, installPosterior, D5 seed | ~200 | fable-only | NEW; **foldAmb/foldRpb NOT here (→INC 7)** |
| 10 | Clock QC unit | 700 | mechanical | ~flat |
| 11 | Emit TU | 1,250 | **milestone** | grown 800→1,250; upgraded mechanical→milestone |
| **11b** | **phase-D emit rebase (NEW)** | 118 | mechanical | NEW — engine already extracted |
| 12 | Final trim + validation (orchestrator **~650**, not 550) | 640 | milestone | matrix adds eq34/carry/coupling/D5/rebase axes |

**Orchestrator target relaxed 550 → ~650** (epoch-loop skeleton grew: Card-8 nav
catch-up, T2e decimation, D0 section timers). No longer cleanly hits 550.

---

## 4. Extended state inventory (Fable idea #2/#3)

~38 post-Jul-4 run-state entities enumerated and tiered. Tier-1 = NetworkRunContext
(by-ref, persists across epochs); Tier-2 = EpochWorkspace (fresh each iteration,
never hoisted); Tier-3 = QC owns none.

**PLAN-ASSUMPTION DEVIATION — three-place rule → two-place rule.** NONE of the
~13 new Tier-1 carries (carry_driver z-store, carry_innov_pending, rpb_carry,
eq34_rx_parent/sat_root/prev_target/retained_edges/anchor_incumbent,
prev_carry_cov, CarryPrior.coupling, sh_dev_carry, emit-rebase frame,
carry_vce_est windows) were added to the snapshot restore (3092–3245) / write
(11214–11276). They are **deliberately cold-started** and gated ONLY by a
fingerprint conditional-append. **The migration plan must carve out an explicit
"cold-start carries" rule**, or a maintainer mechanically following the old
three-place rule will add these to the snapshot without persisting all coupled
substate (e.g. `CarryPrior.coupling` without `prev_carry_cov`) → inconsistent warm
resume.

### Cross-pass / cross-epoch channels (the migration hazards)

| Channel | Tier | Writers → Readers | Migration rule |
|---|---|---|---|
| `carry_innov_pending` | 1 | installPosterior 7926 → next-epoch PASS-A seed 3491 | **Only field crossing BOTH pass seam AND epoch boundary.** By-ref single owner; value-copy = D5 re-tap goes counter-silently inert. Consume+clear (3491) must precede write (7926) — source order guarantees it. |
| `carry_slipped_ambs` | 2 | SlipQc 3732/3831 + D5 seed 3492 → fold 6156 | Fresh per epoch, ONE instance by-ref to all 4 TUs. A value-copy anywhere silently disables the C3c slip funnel → stiff pre-slip prior (**invariant #1, 10.9 cyc/ep FAIL**). Slip-detect TU strictly before fold TU. |
| `carry_arc_amb_key` | 2 | buildCarryDdPriors 6170 (clear 6117) → installPosterior 7910 | Fresh/rebuilt each build; the `.clear()` at 6117 is **load-bearing** — a hoisted stale entry mis-resolves a D5 flag to the WRONG ambiguity (`map_miss` catches missing, not wrong-but-present). Do NOT hoist. |
| `carry_we_drop_sats/arcs` | 2 | RAIM 7407 → fold 6157 | Non-owning const ptrs; pointee co-resident in same EpochWorkspace, nulled each epoch (7755). Never outlive pointee across a TU boundary. |
| `shared_phase_keys` | 2 | eq34 election 4095/4214 → PASS-A build 3584 + fold 6139 | Fresh per epoch, single instance; election write precedes parallel read; read-only in parallel section. |
| `carry_vce_est` | 1 | addRow 9926 + 11086 → estimate 11442 | Both addRow inside the SAME `beginEpoch` (9771) bracket; window eviction is epoch-atomic. Keep beginEpoch+both feeds+estimate in one serial sequence. |
| eq34 tree (`anchor_incumbent`+`eq34_rx_parent`+`eq34_sat_root`+`eq34_prev_target`) | 1 | election sticky-update → next-epoch election | ChainDatum by-ref. **`eq34_prev_target` MUST stay keyed per-(station,sat_signal)** — amb-only key collides (measured 4e8-cycle blowup). Cold-start via phdatum:eq34. |
| `sh_dev_carry` | 1 | serial age + serial post-solve update → parallel READ-only 2817 | serial-age / parallel-read-only / serial-update discipline; a parallel writer races. Cold-start via shdevtau. |
| `prev_carry` (+`_cov`+`prev_clock_refloat_sats`+`tu_reinit_hist`+`CarryPrior.coupling`) | 1 | serial post-solve stash → next-epoch assembly | Keep co-resident (one logical carry). Not snapshotted → dense degrades to diagonal / coupling to plain-diagonal one epoch (graceful). |

### Corrections to earlier framing (owned honestly)
- **`carry_arc_amb_key` is declared INSIDE the epoch body (L6102)**, not hoisted
  outside the loop as an earlier grep-based read inferred. It is genuinely Tier-2,
  self-cleaning. The real risk is a *future* refactor dropping the per-build clear,
  not the current code.

---

## 5. Symbol reconciliation (Fable idea #1)

**287 of 288** Jul-4 cut-map units are **present-shifted** (same destination TU,
line numbers moved by the +4,474-line pure-addition growth). Every major named
unit grep-hits at HEAD. **No unit was deleted.**

**Exceptions (2):**
1. **`foldAmb` — MERGED.** The original OMP-region anchor-pin `foldAmb` is gone as
   a standalone unit; subsumed into the generalized `foldAmb(amb_key, target)` at
   5203 (`target==0` reproduces the legacy `-piv_x` pin byte-identically) + new
   sibling `foldRpb`. Only ONE `foldAmb` exists at HEAD. Destination corrected in
   §2c above.
2. **`LocalCarry.var` / `splitStation` — augmented in place** (same TU, wider
   role). `LocalCarry.var` now doubles as the pure conditional `(N_rr⁻¹)_rr` under
   `carry_cpl_cond`, not just the marginal; `splitStation` now drives the Card-1
   conditional-coupling column split. Flagged so the decomposer knows these carry
   NEW responsibilities beyond the cut-map description.

Three proposed destination headers now **physically exist** and are `#include`d:
`products/NetworkEmitRebase` (69), `filter/NetworkCarryVce` (79),
`filter/NetworkInvariantCarryDriver` (80). The two still-inline carve-outs are
`NetworkCoupling` (7b) and `NetworkChainDatum` (8b).

---

## 6. Sequencing gates & pre-freeze blockers (Fable idea #4)

**Jul-4 gates: 3 hold, 2 don't.**

| Gate | Holds? | Status now |
|---|---|---|
| 1 — C1 full-cov carry lands+validates | ✅ | Committed in d0036983; in tree at HEAD. **But** the post-C1 state inventory must be re-derived (§4) — Card work added more cross-epoch mutables. |
| 2 — S1 phase-datum committed | ✅ | `phase_datum_prioritized` committed; met. |
| 3 — pdp coverage card lands FIRST | ✅ | `phase_datum_min_stations`+`station_pd_pin_aware` committed + validated. Concern retired. |
| 4 — Card 7/8 triaged | ❌ | RT Card 8 landed (no longer races), but the explicit before/after-INC-12 triage **decision was never recorded**. Close at re-freeze. |
| 5 — Freeze the TU | ❌ | **VIOLATED — the load-bearing failure.** 30 commits / +4,410 lines landed in the file after the Jul-5 freeze; the series never advanced past INC-0 tooling. Both the freeze SHA and any goldens taken against d0036983 are stale. |

**New pre-freeze blockers (5):**
1. **[BLOCKING] Baseline `d0036983` is obsolete.** Re-freeze at `a4c9af4`;
   regenerate the cut map + state inventory; recapture the ENTIRE INC-0 golden
   matrix + mask freeze against the new SHA.
2. **[BLOCKING] Six default-OFF toggles have NO ON-path goldens** (`carry_invariant`,
   D5 `innov_gate/_calibrate`, Card-1 `coupling/_joint_whiten/_time_update`,
   `phase_datum_eq34`, `carry_vce`, `emit_rebase`). Invariant #4: an unexercised
   gate passes vacuously — the series could silently relocate broken ON-path code
   and stay green. Add ON-armed configs each proven to trip via non-zero counters.
   *Note:* `carry_vce_apply` is NOT implemented (throws in loader) — the VCE golden
   is monitor-only. Respect loader dependency chains (coupling requires
   `shared_carry_full_cov`; joint/tu require coupling).
3. **[BLOCKING] Card-4 not truly frozen.** Holstered **v3 robust-M** (touches D5
   glue) and the **per-LOS iono repr card** (touches Assembly iono appenders) would
   re-touch the exact files being decomposed. Need an explicit user decision:
   stay-holstered for the 1.5–2 week series, or land before re-freeze.
4. **[RECOMMENDED] Re-pin the 305 SULP+OWMG 3h crash signature** against the new
   SHA — the d0036983 signature is stale (+4,410 lines shifted RAIM/carry/emit
   branches). Keep the crash out of scope for fixing; pin as the INC-12 comparand.
5. **[NOTE] Nine untracked `config/test_suite/` JSONs** (`_eq34_carryinv_*`,
   `_ev*_win`) — inert to the build but leave the tree undecided for a clean
   freeze. Likely the staged ON-path golden configs; commit or exclude before
   re-freezing.

**Re-freeze recommendation:** freeze at `a4c9af4` (or one commit later) after
(a) user confirms the two holstered cards stay holstered; (b) the nine ON-path
configs are committed; (c) the cut map + state inventory are regenerated; (d) the
full INC-0 matrix (expanded with the 6 ON-armed toggles) is recaptured; (e) the
3h crash signature is re-pinned. Do NOT amend/rebase d0036983 — record the new SHA
and treat its goldens/masks as READ-ONLY once frozen.

---

## 7. `carry_cpl_joint` fingerprint gap — TRACED, REAL (low severity), FIXED

**Traced (Jul-13).** Three of the four Card-1 coupling knobs are conditional-
appended to the warm-resume fingerprint (`carrycplcond` 3068, `carrycpltu` 3083,
`cplshmarg` 3087). **The fourth — `carry_cpl_joint` (Card-1b joint-whiten, decl
2130) — was not.**

**Mechanism (hand-verified):**
- `carry_cpl_joint` acts ONLY on the **injection-side** row construction
  (`buildCplRow` 5512–5588): departed columns become shared `M_w` marginalization
  columns instead of a per-row scalar σ² addend. It reads `lc.coupling`, writes
  `R.cpl_*`/`craw`. It never writes `local_carry` or any persisted field.
- The persisted snapshot carry is `{value_abs, var, last_seen_s}` (write 11262);
  `.coupling` rows are NOT persisted. Restore (3178–3180) rebuilds `LocalCarry`
  with `.coupling` **empty** → `buildCplRow`'s coupling loop no-ops on restored
  carries → joint and scalar paths produce the identical base row. **Joint is inert
  on the first post-resume epoch.**

**Verdict: REAL gap, LOW severity.** It does NOT corrupt the frame/meaning of any
persisted state (unlike `cond`, which redefines `local_carry.var` as conditional vs
marginal — so the tier agent's "differently-whitened carry folded without cold-
starting" framing over-stated it; there is no whitened carry *in* the snapshot).
The actual defect: `joint` is solve-determining and its three injection-side
siblings (`cond`/`tu`/`shmarg`) are all fingerprinted under the design's own rule
("changes what an injected coupling row MEANS"); `joint` was the lone omission. The
concrete failure it permitted is a **warm-resume-pair divergence across a joint
flip** the fingerprint allowed — exactly what the resume-pair gate exists to catch.

**FIXED (uncommitted, staged for the pre-freeze commit).** Guarded tail append after
`cplshmarg` (3088): `if (carry_cpl_joint) add(fmt::format("carrycpljoint={}", …))`.
Byte-identical for every non-joint config (default OFF); only armed-joint (research)
configs see the new term — which is the point (they were under-gated). **Acceptance:
a warm-resume-pair test with `carry_cpl_joint` flipped must now correctly REJECT the
cross-config resume (cold-start), matching `tu`/`shmarg` behavior.** Land as a source
commit in the housekeeping sequence BEFORE the freeze SHA.

---

## 8. Decisions — RESOLVED (user, Jul-13)

1. **Holster confirmation — YES, both stay holstered for the full series.** v3
   robust-M is field-inert with data (em25 1.68 vs 1.50, both ≪3 bar; em10 is the
   deployment regime and v2 delivers). Per-LOS iono is an unbounded research card
   that touches exactly the Assembly/iono/estimator code being decomposed — lands
   better post-series in small dedicated TUs. **Escape hatch:** the series is
   pausable at any increment boundary; if the 305 all-on run or a field event
   forces an estimation card mid-series, pause at a boundary → land → re-freeze →
   recapture, never interleave. All-on-motivated work queues as a post-series card.
2. **Structural seams — APPROVED (7b, 8b, 9b) + tiering as proposed.** 8b/9b are
   fable-only (9b owns the two most dangerous channels — `carry_innov_pending` and
   `carry_slipped_ambs`). 8b+9b land BEFORE INC 9 (RAIM re-solve replay). The 2c
   `do_fold` pins stay in Assembly; the `NetworkChainDatum::applyPins` helper is a
   default-NO decided at INC-7 time (leave pins textually in Assembly = byte-id-safest).
3. **`carry_cpl_joint` — TRACED & FIXED** (see §7): REAL low-severity gap; one-line
   guarded append applied (uncommitted), resume-pair test is the acceptance.
4. **Re-freeze — PROCEED with §6, two amendments:**
   - **Freeze SHA is post-housekeeping HEAD, not literally `a4c9af4`.** Housekeeping
     commits first: the untracked config lineage (now 10 files incl.
     `_net305_allon_1h.json`), the survey doc, handoff docs, ON-path golden configs,
     and the `carry_cpl_joint` fix. Regenerate cut-map line anchors against that SHA.
   - **ON-path golden coverage:** the 305 all-on config covers **5 of 6** toggles in
     one arm (carry_invariant, D5 gate+calibrate, eq34, carry_vce, emit_rebase) —
     NOT the Card-1 coupling trio (deliberately excluded from the science run). The
     INC-0 matrix needs a **separate coupling-armed arm** (loader chain: coupling
     requires `shared_carry_full_cov`; `joint`/`tu` require coupling). Cheaper
     variants (20/50-sta or 10-min smokes) are fine — counters proving each toggle
     trips is the acceptance (invariant #4).
   - **Close gate 4** in the same act: record the Card 7/8 triage decision explicitly
     ("after INC-12"). **Re-pin the 305 3h crash signature** (one ~1.5–2h unattended run).

## 9. Order of operations from here

1. `carry_cpl_joint` trace → **DONE (REAL, fixed uncommitted)**.
2. Commit housekeeping (10 configs + survey doc + handoff docs + ON-path golden
   configs + the cpl_joint fix) → **new freeze SHA**.
3. Regenerate the symbol-anchored cut-map line anchors + post-Card state inventory
   against the freeze SHA.
4. **INC-0 capture — FABLE-ONLY** per the executor split (harness + full golden
   matrix incl. the 6 ON-armed toggle arms + coupling-armed arm + 3h crash-signature
   re-pin; goldens/masks READ-ONLY after).
5. **Opus starts INC 1** (config split → `GnssConfigLoadNetwork.cpp`).
