# QUEUE — principled replacements for symptom-level patches (cross-family index)

2026-08-14. Which standing "bandaid" patches — symptom filters, hand tunings, avoidance rituals,
binary backstops — have a known theoretically correct replacement, and in what order to build them.
Written from the pivot-gauge handover review (`docs/SPEC_pivot_visibility_gauge_defect.md` §5.1,
implementation `ca19d0d`). ⚠ **The validation arms LANDED** (`353f99f`, `58f650c`) and the handover
alone does NOT close the defect: the prioritized union already suppresses the legacy deletion (1
col_removed vs 6) and the prioritized arm's residual churn is a THIRD root with three hypotheses
falsified — detail in `SPEC_pivot_visibility_gauge_defect.md` §5.0–§5.08, memory
`project_pivot_visibility_gauge_defect`.

**Relationship to the existing queues (do not reconcile by guessing):**

- `docs/HANDOFF_edge_response_queue.md` (2026-08-05) ranks the BARE frame/coupling EDGES by
  measured damage (5 edges). This doc ranks ACROSS families (edges + test statistics + numerics),
  post-dates the pivot-gauge card, and re-scores readiness. Where they overlap, the edge queue
  carries the measurement detail; this doc carries only the ranking. ⚠ Its session-constraints
  block encodes a phase-specific run budget (the 2026-08-05 "ultra-fast turnaround" directive) —
  do not inherit it blindly.
- `docs/SPEC_level_information_budget.md` §27 ranks LEVEL next-actions and §30 (LV-3) declares the
  level frame. Both stand.
- The open valve inventory (valves with a bare edge) is
  `docs/SPEC_prefit_census_observability.md` §13 (= edge 5 of the edge queue).

Ranking = measured payoff × readiness, scored 2026-08-14. Every item names its sim-bench validation
shape — the synthetic network (truth-graded, injectable) is the venue for all of these.

**BUILD ORDER — CURRENT (2026-08-14, fourth ranking).** Item numbers are STABLE — cited by section
from other docs and from `include/config/GnssConfig.hpp:3469` and `scripts/t3_null_fit.py`, so a
re-ranking is expressed HERE rather than by renumbering.

> ### **9 → 8(b) → 4(R2/R4) → 2 → 1(b) → 5 → 7 → 6**  *(fifth ranking, 2026-08-20)*
> with **8(a) EXECUTED 2026-08-20 (verdict: PERSISTS — see §8)**, **4(R1) HELD**,
> **3 dissolved into 1(d)**, **0 downgraded**, **1(a) demoted**, and
> **R0 + 1(d) already EXECUTED, LANDED and VALIDATED** (2026-08-14).
>
> **Item 8(b) — EXECUTED 2026-08-20/21 (verdict: the carry family is FALSIFIED
> at all four points — solve-fed × {relevel, always} and CS-GF code-anchored ×
> {relevel, always} — with the wiring proven live; the EVENT channel is
> exonerated and the plateau is the per-epoch exchange equilibrium under
> mid-arc level lock-in.  `SPEC_iono_carry_8b.md` §6.5/§6.6; execution entry
> in §8(b) below.  Everything default-OFF, byte-certified.  The next 8-family
> card is the O2 / carry_invariant-lineage accumulator, unranked until its
> derivation exists.**
>
> **Item 9 — EXECUTED 2026-08-20 (same session): the walk is DEAD under
> `network.cycle_slip.relevel_free_epoch` (+ `relevel_delta_bridge`), built,
> suite-green, byte-certified, validated on the slipped fixture (ZWD hourly rms
> FLAT 2.3–3.1 m vs 2.6→6.5 √t; `SPEC_slip_relevel_walk.md` §8).  The deeper
> code fact: the pending-reseed mark was CONSUMED by PASS-B before the inject
> read it — the leveling slip refloat had been structurally inert all along.
> Residues: the stationary ~2.3–3.1 m plateau (→ 8(b)), bridge v2 cross-epoch
> pairing (3 959 unpaired MW candidates), retap truth-neutral (default-OFF),
> field arm owed.  Original filing below.**
>
> **Item 9 was NEW and ranked FIRST — the slip-RE-LEVEL level walk**
> (`docs/SPEC_slip_relevel_walk.md`): on the first long/multi-GNSS/realistic-slip
> truth fixture, every refloat re-levels its arc from ONE epoch of code and freezes
> the result (zero-q), injecting station-common metre-class iono error into the
> leveling datum; the ZWD/clock exchange integrates the steps into a √t walk
> (6 h → ~6.5 m ZWD rms, clock contrast 14 m) with EVERY internal instrument
> silent.  Isolated by a six-arm ladder: iono posture / arc persistence /
> coupling stack / Galileo all exonerated; the QUIET twin converges (0.15 m).
> The T4 arc-refloat storm (21 k arcs / 6 h) is a co-symptom riding the same
> iono-level error — halving it moves nothing, so 4(R1)-class statistic work
> stays SECOND to fixing the re-level edge.  Fix options §6 of the SPEC
> (predicted-phase re-level / validated-delta carry repair / post-re-level vet
> window / at minimum the injection counter).  This is the measured reason a
> "robust network run" does not yet survive a realistic slip environment.

Why this order — the measurement that earned each call lives in the item's own section:

- **8 is NEW and ranks FIRST.** Item 4's investigation ended by measuring that the T3 false-alarm
  floor, the per-station residual heterogeneity, the L1C excess AND the integer-resolution failure
  are **ONE defect: per-LOS ionosphere estimation error** (`SPEC_t3_null_derivation.md` §7.8-§7.9).
  **8(a) is one knob on an existing arm** and decides the scope of everything else.
- **4(R1) — the stratified per-station variance component — is HELD, not scheduled**: it would
  absorb a measured 0.11-2.55 m per-station ionosphere error into the noise model, i.e. *bury the
  root*. **4(R2) (declare the station factor as its own statistic) and 4(R4) (population hygiene)
  survive and stay cheap**; 4(R3) (FDR) is correct but second-order.
- **5 (the level) slips behind 8 and re-scopes** — same ionospheric field, smaller term (overall
  level μ 0.328 m vs the 0.764 m rms datum-free interaction).
- **2 outranks 1 but slips behind 4/8**: datum election is measured 5.9× on the datum-free C
  contrast with a 40 % T3 cut and its remaining work is plumbing — but electing a better datum
  reduces *exposure* to an edge that 1(d) *fixes*.
- **1(a) is DEMOTED** — measured **null** on the channel it targets. **3 is DISSOLVED into 1(d)**:
  the root was never clock work, it is phase-bias-datum CARRY work
  (`SPEC_pivot_visibility_gauge_defect.md` §5.07); item 3 retains only always-armed marking and
  1(b). **0 is DOWNGRADED** — a measured net LOSS on the clock channel (its 1.9× code-bias win
  stands; see §0).
- **The D5 / "re-deciding the representation" hold is substantially lifted** — it was held as "a
  science program, not a patch swap" only because the representation error had never been
  quantified. It now is, with a truth-graded instrument.
- ⚠ **`clock_monitor_couple_raim` is NOT the fix** despite naming the right coupling: **85.5 % of
  Σ|dev| is emitted before the stash it acts through.**
- **2, 1(b), 7, 6 are unchanged in kind** and simply move down.

*Provenance (superseded orders, kept so a stale citation can be placed):* (1) `0 → 2 → 4 → 1(b) →
5 → 7 → 6`; (2) after the third root was named, `R0(telemetry) → 1(d) → regenerate the null arm →
4 → 2 → 1(b) → 5 → 7 → 6` (A/B `runs/inj_weab_{on,off}`, configs `_simnet24_inj_weab_{on,off}.json`);
(3) after R0 and 1(d) landed and item 4 was derived, `4 → 2 → 1(b) → 5 → 7 → 6`. The "regenerate the
null arm" step is satisfied by **`runs/pareref_on`**, and the pre-fit gate passed on it: σ̂₀_first at
the assessed handover epochs reads **1.000–1.323** against a run median of 1.247
(OFF arm 1.37–13.27 against 1.83), σ̂₀_first ≡ σ̂₀ at every one ⇒ a clean null instrument.

---

## 0 — Arm the pin bridge (one knob, already built, already measured)

**Bandaid:** none — a validated mechanism sitting unarmed.

`network.phase_datum_time_linked`. `NetworkAnchorHygiene.cpp:231`'s `want_bridge` is reached only
when `handover_pending`, and its `min_arc_ep <= 0` disjunct is the DEFAULT — so arming the knob
bridges **every** handover: the key holds its shared phase bias at `pb_level_prev` through a soft
row (σ = 0.05 m) with **no station pin at all**. The gauge moves to the SHARED side,
station-independently, reachable from the prioritized anchor election today (not eq34-only).

**Measured** (24-sta arm, single knob vs `_simnet24_inj_null_pdp.json`): 27 bridges opened, 453
row-folds, **0 reattaches performed** — the gauge never moved to another station. **B rms 1.573 →
0.813 m (1.9×)**, datum-free C contrast 18.655 → 17.214 (7.7 %). It does NOT reduce the clock
re-realizations (7 → 8 episodes, Σ|dev| 160.5 → 165.3 m). **Remaining work:** a field arm, and a
decision on the σ and `bridge_max_ep` defaults (0.05 m / 20 ep were never tuned for this use).
Config `config/test_suite/_simnet24_inj_bridge.json`.

⚠ **DOWNGRADED 2026-08-14 — net LOSS on the clock channel, with a mechanism.** Σ|dev| 160.457 →
**165.271**, episodes 7 → 8. The specific harm: the *silent* 302490 G12 handover becomes a
**+33.617 m** episode at 303090 — exactly 20 epochs later, one `bridge_max_ep` lifetime. The bridge
**postpones** an un-transformed re-pin rather than transforming the carry, converting silent
handovers into loud ones (§5.07). The 1.9× code-bias win stands; this is a B-channel remedy, not a
clock remedy, and it should be sequenced AFTER 1(d) so the two are not confounded.

⚠ **LIKELY SUPERSEDED 2026-08-14 (later) — 1(d) alone took the B channel further than the bridge
did.** §5.08.3: B truth rms 1.5725 → **0.0752 m** and rx DCB contrast 4.41 → 1.21 m on the same
posture, without the bridge's clock-channel loss. If this knob is ever wanted, re-measure it ON TOP
of 1(d); do not arm it on the old evidence.

---

## 1 — Complete the S-transform re-reference family

**Bandaids today:** the PB emission-continuity filter — v1's `emit_pb_filter_jump_k: 8.0` is a
retired hand tuning, v2 measured null-to-negative (`runs/agu63_pbv2`) — filters the WIRE while
**91 % of emitted PB discontinuities are ref-path slip re-referencing** (AGU-26 amendment 2) that no
emission filter can reach; and the one-epoch fold-skip "free fall" at clock-pivot handover
(`handover_affects` mirrored across **4 files / 14 occurrences** — `NetworkRunContext.cpp` 5,
`NetworkClockQc.cpp` 5, `NetworkStashStatus.cpp` 3, `NetworkSolveGlue.cpp` 1; re-counted 2026-08-14,
the earlier "five sites" was stale and the number scopes 1(b) — see
`docs/SPEC_pivot_visibility_gauge_defect.md` §8.2).

**Principled replacement:** an S-transform at every re-reference edge, in four sub-cards.

- **(a) Slip-triggered continuous handover in the prioritized PB election.** ⚠ **DEMOTED and
  RE-SCOPED 2026-08-14.** When the pin-holder's arc slips, elect an unslipped peer and hand over at
  the ledger target — the mechanism half of the review's slip-hole finding (the election currently
  receives NO slip input; `carry_slipped_ambs` at `ApplicationNetwork.cpp:1441` already has the
  ledger's exact `pair<int,string>` key type, and the eq34 election already takes it). Machinery
  (ledger, counters, PBGAUGE trace) landed `ca19d0d`. **The premise changed:** with item 0 armed the
  pin bridge INTERCEPTS every handover before the ledger is consulted (27 bridges, **0 reattaches
  performed**), so the two mechanisms compete for the same edge and the bridge measures better. The
  handover itself measured null on its target channel: C contrast 18.655 → 18.604, episodes 7 → 7,
  every episode merely DEFERRED by exactly one epoch — the signature of a one-epoch-stale ledger
  (§5.0.1). The remaining question is the much smaller **"does a slip-triggered handover add
  anything over bridging?"**, which the fault fixture can answer directly (a slip on the pin-holder
  is exactly where `prior_fresh` may fail and the bridge declines to open). Answer that before
  writing any election code.
- **(b) `docs/SPEC_pivot_visibility_gauge_defect.md §8`** — SPEC'd 2026-08-04, unbuilt: re-express
  carried priors at the clock-datum handover instead of the free-fall skip; exact and cheap per
  LASER (S := I − Eᵀ(KEᵀ)⁻¹K on mean AND dispersion).
- **(c) eq34 chain-datum handover continuity** — named "a separate card" by the loader guard in
  `GnssConfigLoadNetwork.cpp`; unwritten. The FIELD posture's instance (the GG-Q lineage's 2
  avoidable unmarked G01 events).
- **(d) ⭐ THE PHASE_ANCHOR CARRY RE-REFERENCE — ✅ LANDED AND VALIDATED 2026-08-14 as
  `network.phase_anchor_carry_reref` (default OFF).** The named root of the clock churn
  (`SPEC_pivot_visibility_gauge_defect.md` §5.07): at a `PHASE_ANCHOR` handover the datum is
  re-realized on the **design matrix only** and the carried bˢ_σ is folded forward *in the retired
  frame* at `q_pb = 1e-12`, because neither carry-skip guard fires — `datum_changed` reads the
  clock-pivot map only (`NetworkSolveGlue.cpp:461-462`) and `eq34_reref` is populated only in
  `NetworkChainDatum.cpp:778`. With `sat_clock_q = 0.01` against `sat_phase_bias_q = 1e-12`
  (1e10:1), the clock absorbs the entire frame step — measured 41 σ on G21. **Built exactly as
  specified** (election populates the set beside `handovers_out`; D2/C1 fold marginalizes on both
  branches exactly as `eq34_reref`; local carries erased at every station; VCE monitor + prires
  census mirror the skip; loader INERT ladder; fingerprint-exempt). **Measured** (§5.08.3,
  single-knob, schedule-identical 30/30): Σ|dev| 160.46 → 1.53 m, episodes 7 → 1 (ignition), C
  contrast 18.66 → 0.50 m — at the clean-election bound — with every other instrument improving in
  the same direction. ⚠ Marginalize (delete) is now MEASURED sufficient on this fixture; the
  S-transform (which would keep one epoch of carry information per handed-over family) remains
  unbuilt and its extra value unmeasured — build it only if a larger fixture shows the one-epoch
  refloat cost. ⚠ n = 1; field arm owed (the flagship field lineages run eq34, which owns
  `eq34_reref` already — 1(d) covers the prioritized-anchor posture).

**Sim validation:** (a) fault fixture `_simnet24_inj_fault` with a slip injected at the pin-holder /
next-ranked candidate on a known handover epoch; grade PBGAUGE continuous/cold counters, the emitted
PB disc census, and the datum-free C contrast. (b)/(c) per their specs, eq34 on the sim tri-slant
posture.

- **(e)/(f)/(g) ✅ THE TRUNCATED-ROW GAUGE DEFECT — BUILT 2026-08-15, default OFF, n = 0.**
  The carry↔datum handover campaign's fix program A/B/C
  (memory `project_carry_datum_handover_theory`), landed as three knobs.  Full derivation:
  `docs/SPEC_card1_coupling_carry.md` §13.
  - **A → `network.carry_coupling_gauge_project` (Card 1f).**  A DEPARTED coupling column is
    marginalized by *deleting* its coefficient, leaving `Σ_family M = −M_dep`, so the row becomes a
    high-weight observation of the CURRENT gauge pinned at the OLD realization (measured: null ev
    **1e-11 → 9.43e-3** in one epoch ⇒ 4 spurious OVER-specified withholds).  The fix seeds
    `tuGaugeRestore`'s deficit with the truncated coefficients, holding each (constellation,kind) sum
    at its **HARVEST** value.  ⚠ **The plan's "annihilate the sum" is WRONG and was not built** — the
    harvest target is `−g_r(c)`, and §1 measures a Galileo ambiguity row at `Σ_E = −1` exactly, so
    annihilation would itself inject a gauge observation.  ⚠ **No measured `V` stash was built
    either**, and deliberately: the rank gate is `EigenvaluesOnly` so no eigenvector solve of the
    border runs on a default config, a stash would be one epoch stale, and on a truncation epoch the
    previous epoch's near-null set is precisely the one MISSING the filled direction.  Restoring to
    the harvest sum needs no basis at all and is frame-agnostic.
  - **B → `network.carry_coupling_reinit_sources` (Card 1g).**  `tuReinitSince` was gated on the κ
    knob AND fed only by clock-QC sources — **no realization-step source reached it**.  Key widened to
    `(kind,sat,signal)` with an empty signal as the sat-wide wildcard; fed from `phase_anchor_reref`,
    `eq34_reref`, `datum_reref_pb_sats`; honoured with κ off.  Designed to be armed WITH 1f (the
    loader warns otherwise): marginalizing a re-realized column opens the same deficit a departure
    opens.
  - **C → `network.shared_carry_gauge_free` (Card C-GF).**  The gauge-free downdate was reachable
    only through the unrelated Q3 `clock_anchor_gauge_free`; it is now armed as a property of the
    CARRY, through **one resolved predicate** (`ctx.knobs.carry_gauge_free`) that every fold, withdraw
    and replay site reads — a fold/withdraw mismatch leaves a **negative-semidefinite** residue and
    the solve returns a silently wrong `x_s`.  Added a **support-closure** test (a direction is
    downdated only when its whole support is carried; otherwise the downdate deletes an *estimable*
    carried-vs-uncarried contrast).  ⚠ **Fixed a pre-existing latent defect in the same change:** the
    within-epoch RAIM replay re-folded with NO gauge-free flag, so the replayed system differed from
    the primary solve — latent while the downdate was Q3-only, live the moment it arms with the carry.
  - **Invariant-#7 counter (unconditional, no knob):** per-epoch datum-direction information ratio
    `R = (vᵀNv/vᵀv)/cutoff` on the gate's own normals and cutoff, with breach/clear edges.  `R ≪ 1` =
    the datum still owns its gauge; `R ≥ 1` = a pre-gate fold is pinning it.  Cheap because `vᵀNv`
    needs no eigenvector.
  - **Gate-advice correction:** the run-end "RE-CALIBRATE, do not chase the datum" is **provably
    unactionable** for a carry-filled null — gauge depth 1e-11, filled direction 9.43e-3, weakest-real
    PB layer 1.25e-2 ⇒ the admissible band is EMPTY.  The advice now branches on `ddi_over_breach` and
    says "attribute the fold" for the breached case, keeping "re-calibrate" only for the genuine one.
  - ✅ **POSITIVE CONTROL PASSED 2026-08-15 for 1f+1g** (`SPEC_card1_coupling_carry.md` §13.7;
    configs `_s24cpl_{off,on}.json`, runs `runs/s24cpl_{off,on}`).  The OFF arm **reproduces the
    defect on demand**: 4 OVER-specified withholds at TOW **305220/305250/305280/305310** — the
    named epoch, spanning exactly the predicted 120 s = `pl_max_gap_s` self-heal.  ON:
    withholds **4 → 0**, epochs **116 → 120/120**, invariant-#7 ratio **7.123e+06 → 2.086e-03**
    (9.5 decades), observed near-null == declared on **every** epoch, and **4 of 4** withholds
    coincided with a breach so the new advice named the right knob instead of "re-calibrate".
    Matched-epoch truth grade: code-bias rms FLAT (+0.9 %), sum-to-zero datum check **1.6×
    tighter**, phase channel unresolved in both arms (uniform-random rms — no signal).
    ⚠ **n = 1, 24 sta, GPS, sim, 1 h, one lineage.**  Registered **RECOMMEND_SCOPED** in the
    promotion ledger + loader POSTURE rows (parity verified); code defaults deliberately
    unchanged — both knobs are fingerprinted, so a default flip is a snapshot migration.
    ✅ **FIELD + SCALE ARMS RUN 2026-08-15 — PASS at every rung** (`SPEC_card1_coupling_carry.md`
    §13.8).  Ladder, all single-variable: **24 sta sim** withholds 4→0 (R 7.1e+06→2.1e-03) ·
    **50 sta FIELD** 20/121→0, i.e. **16.5 % of epochs recovered**, R 1.6e+03→3.1e-06 ·
    **150 sta FIELD** 20/121→0, R 7.8e+02→9.3e-06.  The field defect is **5× more prevalent
    than the sim one**, so the sim control under-stated the payoff.  At 150 sta departed
    coupling terms also fell **111 599 → 6 393 (17×)**: the OFF arm's withholds age the carry
    past `pl_max_gap_s`, blanket expiry manufactures more departures, and those breach again —
    the fix breaks that cycle.
    ⚠ **At 150 sta Card 1g diverted ZERO terms**, so that rung's whole gain is **1f alone**.
    New measured **FEED INTERLOCK**: 1g's sources come only from `phase_datum_eq34` or
    `phase_anchor_carry_reref`; with neither armed it is inert.  Now a loader WARN, a ledger
    interlock, and a tightened posture/ledger regime.
    ⚠ **305 sta: the knobs are structurally INERT AS SHIPPED** — every 305 config in-tree runs
    `carry_invariant: true`, under which `do_amb_carry` is false and the coupling rows are never
    built.  Forced to a non-Card-4 posture the defect IS present at 305 and at full strength:
    20/121 withheld, R max **7.537e+03**, **284 618** departed coupling terms over 1 h.
    ❌ **A "dense rosters see every satellite, so departures are rare at scale" hypothesis was
    FALSIFIED** — it came from an 11-epoch probe that was simply too short.  Departures scale UP
    with station count (15 k @50 · 112 k @150 · 285 k @305), so the defect gets MORE frequent
    with scale, not less.
    ✅ **QUALITY improved, not merely neutral**: field datum-invariant wire step (pivoted
    between-sat SD) **5.631 → 4.445 cyc/ep (−21 %)**, 1.2549 → 0.9983 m/ep — achieved while
    emitting 20 MORE epochs.  Sim truth grade flat (+0.9 %) with the datum check 1.6× tighter.
    ✅ **No runtime cost**: 305-sta arm 15.4 min OFF vs 14.7 min ON (the fix suppresses the
    withhold→expiry→more-departures cycle, so there is less marginalization work).
    ⇒ **1f PROMOTED to RECOMMEND_ON** in the ledger and moved to the loader's ON tier (gaps now
    WARN).  Code defaults still unchanged (fingerprinted; a default flip is a snapshot
    migration, not an edit).
    ✅ **2×2 ISOLATION RUN 2026-08-17** (SPEC §13.9 — full factorial on the fld50 eq34 lineage
    AND the s24cpl sim lineage, one build, corner cells byte-identical to the ladder arms):
    **1f owns availability + gauge honesty + the datum check; 1g owns product quality, and the
    channel is the CLOCK** — sim datum-free clock contrast rms 4.157 → **0.435 m** alone (~10×,
    max |err| 17.8 → 1.0 m), 0.378 m with 1f; field matched-epoch pivoted wire 5.631 → **1.532**
    alone (−73 %), 3.935 with 1f.  The gain holds on the datum-free contrast, so it is real
    accuracy, NOT the lane-A/B over-retention shape.  ⚠ 1g ALONE recovers zero epochs and makes
    the eq34 gauge breach **6.6× hotter** (R 1.6e+03 → 1.0e+04) — "arm WITH 1f" is now a
    measured rule, not advice.  **1g stays SCOPED** (the clock-truth instrument is sim-only).
    ✅ **SPEC §14 FIXED 2026-08-17** — the eq34 D4 v2-shift now re-bases `coupling[].s_prev`
    (`rebaseCouplingPhaseBiasSprev` over local/rpb/zwd carries, run counter in the Card 3/eq34
    line, unit-tested); wiring proven live on the archive firing set (225 v2-shifts → 902
    re-bases, clean run).  No fingerprint token discriminates pre/post-fix on a
    carry_shift+coupling lineage — re-baseline any A/B that spans it there.
  - ✅ **C-GF FIRST VARIATION RUN 2026-08-17** (SPEC §13.10): four single-knob arms (field eq34
    + sim, both corners) — **ALL FOUR BYTE-IDENTICAL to their parents.  INERT-BY-CLOSURE**: the
    support-closure test correctly declines the sole CLK|G direction essentially every epoch
    (full-constellation support never holds on a real roster), and the plain-fold decline path
    is a proven no-op.  Ledger verdict **UNVALIDATED → KEEP_OFF** (arming buys nothing and
    splits the `cta:gf` fingerprint).  Successor is a **derivation card** — re-derive the datum
    functional within the CARRIED subset (pins + carried span); a gate tweak cannot fix it, and
    the run-end refusal message explains why a naive subset restriction would delete estimable
    contrast instead of gauge.

- **(h) FILED, NOT FIXED — eq34 D4 shifts `value_abs` but not `coupling[].s_prev`.**
  `docs/SPEC_card1_coupling_carry.md` §14.  `CoupledShared::s_prev` and `CarryPrior::value` are the
  same number for the same identity; D4 shifts the second and leaves the first, injecting exactly the
  frame step as row bias.  ⚠ **Latent-and-LIVE, not latent-and-harmless**: the D4 ERASE does not cover
  it — at the population site `if (!k) eq34_reref.insert(...) else if (s != 0.0) shift_sat[...] = s`
  are mutually exclusive, so the shifted keys are exactly those whose carries SURVIVE the erase.  Fix
  is one loop with an in-tree precedent (`NetworkRelevel.cpp`'s Card-6 eph cutover, same principle).
  `config/archive/_dcmp_50sta_10ep_eq34x.json` already arms the full firing set.

**1(b) precondition PRICED 2026-08-21 — build stays DEFERRED** (`SPEC_pivot_visibility_gauge_defect.md`
§11): the clock-pivot handover flux measures ZERO over 4 h on the sticky field lineage and 3
single-epoch edges over 6 h on the non-sticky sim — the transform's target population is near-empty
on every current lineage, and `sticky_pivot` is the cheaper first response wherever flux appears.
The §8 design is fully derived and stays ready; concrete triggers in §11.  (The gate-v2 lesson,
applied before the build.)

## 2 — Datum-station election by criterion, not luck

**Bandaid:** the field posture dodges the pivot-gauge defect only because MBAR00UGA was hand-picked
(equatorial, loses almost nothing) — §4.2 of the pivot spec calls it "the §5.1 remedy arrived at by
luck"; the 300-sta sim arm's default datum carries 8/8 avoidable events;
`simnet_make_producer_config.py` warns about the POLAR fallback only.

**Principled replacement:** election ranked on satellite-visibility continuity
(`scripts/datum_gauge_audit.py --fixture` already ranks rosters), equatorial preference among ties
(§9.8: 8× wander from a polar pivot), formalized in the config generator + a field-config preflight;
`--run` as a harness gate (exit 1 on any avoidable removal).

**Sim validation:** already done — the three-arm prediction test measured 5.9× C-contrast and a 40 %
T3 cut for clean election. Remaining work is plumbing.


✅ **EXECUTED 2026-08-21 — the plumbing is landed and validated** (`SPEC_pivot_visibility_gauge_defect.md`
§10): generator election (losses → equatorial tiebreak → name, deterministic and fingerprint-stable;
silent most-polar fallback replaced by a hard error), field-config preflight
(`datum_gauge_audit.py --config`, extends-chain-aware, exit 1 on unset/absent/polar-with-alternative +
marking-channel and slip-response warnings), `--run` harness gate confirmed (exit 1 on avoidable
removals).  Reproduces the record: 24-sta GPS ⇒ S06000SIM (0 losses, arm B); 60-sta ⇒ S30000SIM
(10 losses, −0.9°, wins the equatorial tiebreak over S26000SIM) — the camp60 hand-election
retro-validated.  First gate readings: camp60_i3 ⇒ 0 avoidable; **fld63_relevel4h_on ⇒ 12 avoidable
pivot-only removals / 4 h, ALL unmarked** (the ext63 lineage lacks `clock_disc_jump_test` +
`datum_station_detect` — filed).  No producer code; the falsified producer-side family stays dead.

## 3 — Clock: from per-event patches to a re-realization-free posture (= edge 2)

**Bandaids:** CLK-R emission re-base (correct triage — census 613→1 — but applied at the emission
boundary while the estimator underneath still re-realizes; the clock is the worst re-realizer, 13
product-wide jumps/12 h vs 2 CB / 0 PB) and unarmed markers on field lineages (`emit_clock_disc`
unset ⇒ all 13 silent).

**Principled replacement — with the falsification honored:** datum-choice-at-emit is NOT the
consumer fix. Ensemble PB datum, clock sum-to-zero, and broadcast-anchored were all A/B'd dead as
consumer-visible improvements — common-mode is consumer-absorbable
(`docs/SPEC_pivot_visibility_gauge_defect.md §8` "what this card is NOT";
`project_network_falsified_approaches`). The principled path is producer-internal: the carry
S-transform at the clock handover (item 1b), pivot ELECTION as a product-quality decision
(datum-free contrast moves 2.2× with the pivot's geometry, §9.8), sum-to-zero for wire-frame wander
only (sim PASS 0.193 → 0.036 m; field arm still owed), and always-armed marking (`emit_clock_disc` +
`clock_disc_jump_test`) so whatever remains is at least declared to the consumer.

✅ **UNHELD then DISSOLVED 2026-08-14 — the root is named and it is not clock work.** It is
phase-bias-datum CARRY work: see **item 1(d)** and `SPEC_pivot_visibility_gauge_defect.md` §5.07.
What remains under item 3 is only **always-armed marking** (declaration, not estimation) and
**1(b)**. ⚠ Marking needs a **cumulative/level** companion, not just the per-epoch step test: G28
re-realized −11.28 m permanently with **zero** CLK_STEP marks because every increment sat under the
1.5 m/epoch bar, while G21's marked −22.5 m step **recovered 86 %** and was never a re-realization
at all.

⚠ **The superseded HELD note, retained for provenance.** FOUR independent phase-bias-gauge
interventions are measured null on the clock channel (datum election on the prioritized path,
handover-continuous, the pin bridge, the coverage gate); see the blocked section. Of the list above,
only **always-armed marking** and **1(b)** survive that evidence unqualified — marking because it is
declaration rather than estimation, 1(b) because it acts on the CLOCK datum handover, a different
edge from the PB gauge those four all touched. Pivot election and sum-to-zero stay listed but
inherit the caveat.

**Sim validation:** the §9.8 synthetic 50-sta matrix is the precedent; edge 2 of the edge queue holds
the candidate list.

## 4 — T3-class test statistics: correct nulls, not recalibrated constants

⚠ **DOWNGRADED 2026-08-14 (third amendment): R1 is HELD behind item 8.** The investigation below
found the root — per-LOS ionosphere error — and a stratified per-station variance component would
ABSORB it (0.11–2.55 m per station) into the noise model. Build **R2 (declare) and R4 (population
hygiene)**, correct regardless; hold **R1** until 8(a) reports.

✅ **R2 + R4 BUILT AND VALIDATED 2026-08-21** (`SPEC_t3_null_derivation.md` §15):
`network.raim_station_factor` (the declared §7.5 trimmed variance component, EWMA slow channel,
both-edge `T3_stafactor` depart/return events, feeds NO denominator — R1 stays HELD) +
`network.raim_rate_hygiene` (untestable cells out of the T3 opportunity count; T3c opportunities
unconditional).  Both measure-only, default-OFF, byte-identical OFF (fld63 recert vs the `65ba362`
references); solve-identity proven by intervention (armed ≡ control on `runs/staf_{ctl,on}`);
**corr(log fitted station scale, log run-end EWMA factor) = +0.9589** over all 24 stations, the §4
structure reproduced to the digit on the fresh exe; suite 1491/208 green.  Collateral finding:
§4's "1.67 % untestable" was a FORMATTING artifact of the 3-decimal L13 trace — the structurally
row-less population is 9 cells (0.04 %); T3c's like-for-like rate reads 1.10e-3.  One recorded
deviation: a single depart/return transient (run-max ratio 6.45) during EWMA convergence, closed
by the hysteresis — recorded, not tuned.  Still owed: the §8-step-2 different-seed null verify,
the field B2 unclamp arm before any FIELD grading, a station-fault bench for depart power.

**Bandaid:** a Gaussian 6σ quantile under a statistic that is a max-over-rows normalized by a
median-over-rows. Measured implied α: 3.78e7× nominal on the quiet 24-sta fixture, 2.7e7× on the
quiet 300-sta fixture; the implied-scale-vs-quantile curve rises monotonically (1.83 → 313) ⇒ the
residue is SHAPE, not scale; a 4.5 % floor survives datum election (pivot spec §6). Sibling in class:
the rx-CB admission gate's flat 26.7–28 % trip rate — but ⚠ that sibling does **NOT generalize**:
its numerator is a frame offset (`value_abs − x_lin`), not a residual, so it has no null to derive;
its replacement is already built (LV-4's frame-invariant `rxCbStepGate`) and blocked by
`SPEC_level_information_budget.md` §19.3 (+64 % postfit phase rms, ~15 TECU level move) and by the
absence of any truth-clean fixture arming `carry_rx_code_bias`. That chain is a POINTER, not a
verified finding.

~~**Principled replacement:** derive the null of the statistic actually computed — extreme-value /
order-statistics of correlated standardized residuals, or per-row w-tests under FDR.~~

✅ **DERIVED AND MEASURED 2026-08-14 — `docs/SPEC_t3_null_derivation.md` carries the full
derivation. The extreme-value framing above is the WRONG replacement.** All numbers below are on the
24-sta field-N sim null arm `runs/pareref_on` (1 h, GPS-only, slant-only posture) unless a row says
otherwise.

**§3 — the statistic is SCALE-BLIND.** `S_c = max_{i∈c}|z_i| / max(1, 1.4826·med_{j∈P}|z_j|)` is
invariant under `z → λz` (numerator and denominator both homogeneous of degree 1 in the same pool),
so no global variance-model fix and no recalibrated constant can move its false-alarm rate — only
the pool's INTERNAL heterogeneity can.

| quantity | value | reading |
|---|---|---|
| exact per-cell null `1−(2Φ(k)−1)^n_c`, k=6, n_c=3 | **5.92e-9** = **3.00×** the quoted per-row α | the EV correction is a rounding error … |
| observed excess over nominal | **1.86e7×** | … against this |
| denominator noise on the α=1e-6 threshold (N=1400) | 5.10 → **5.06** | negligible; the `max(1,·)` floor makes the shipped test *conservative* under a nominal null ⇒ **k=6 is already stricter than a 1e-6 per-cell test**, and a too-strict threshold cannot cause a 1e7× excess |

**§4 — the pool is a MIXTURE on a fixture with ONE global error model.**
`config/sim/simnet24_inject.json` has no per-station noise term and no `multipath` block, and the
emitted `_child_*.json` differ only in `station.seed_salt` — every station draws an INDEPENDENT
REALIZATION of the SAME model, worth a few percent of spread in a robust scale. Measured instead:

| axis | multiplicative spread | note |
|---|---|---|
| station | **4.7×** | flags concentrate in 11 of 24 stations, **S20 alone 33.7 %**, **4.13× overdispersed vs Binomial** |
| signal | **2.75×** (L1C **2.74** vs L2W **1.00**) | *not* elevation weighting (shared across signals at a cell) and *not* ionosphere in the naive sense (that would make L2/L5 worse) |
| epoch — the only axis σ̂₀ estimates | 1.3× | |

No observational covariate explains the station axis (|corr| ≤ 0.24 against elevation and truth iono).

**§5 — stratifying works OUT-OF-SAMPLE.** Station scale fitted on the first half of the settled
window and applied to the held-out half: flag rate **4.4625 % → 0.9736 %** at k=6, implied-c curve
flattening **2.25→3.93 to 1.97→2.39**, and the factor is transferable (corr(log scale, half 1 vs 2)
= **+0.847** over 23 stations).

❌ **FALSIFIED — the redundancy standardization** (`network.raim_redundancy_normalize`, default false
at `GnssConfigLoadNetwork.cpp:3461`). The statistic standardizes a POST-FIT residual by the
MEASUREMENT σ (`z ~ N(0,r_i)`, not `N(0,1)`) — true, and **not the cause**. Single-knob A/B
`runs/t3b2_{off,on}` (OFF arm bit-identical to `runs/pareref_on/logs_v2` on all four graded CSVs —
also the exe-provenance control): numerator **×1.4527**, denominator **×1.4179**, statistic
**×1.0248** — a 1.025 multiplier against a 1.86e7× excess; settled flag rate 4.381 % → **4.460 %** (slightly worse); kurtosis and station spread
unchanged; flags **798 → 772 at Jaccard 0.622** — it reshuffles WHICH cells flag without changing
HOW MANY, at **+38 % runtime**. ⚠ **Sim-scoped**: on the field its ×1.45 lift would *unpin* a clamped
σ̂₀ rather than cancel, so "do not arm it" does not transfer — a field B2 arm is owed.

✅ **GATE PASSED — the fault-fixture MDB check (§12).** Fault arm `runs/t3fault` (the 24-event
±{1,2,3,5,10,50}-cycle ladder in the validated null posture), station scale fitted on the NULL arm:
**20/24 detected at k=6 with a byte-identical per-rung profile** across shipped / station /
station×epoch, while flagged cells fall **912 → 235 (3.9×)**. Matched-budget at 200 flags: shipped
**17/24**, stratified **20/24**. Run-level, the 24 injected slips add only **0.52 points**
(**3.663e-02 → 4.186e-02**) on top of the false-alarm floor — the case for the card in one number.
T3 carries the whole SMALL end (±1/±3/±5 all 4/4) while ±10/±50 are grabbed upstream; 3 of the 4 T3
misses were caught upstream (**T0** MW / T1 IF / T2), and **one event is invisible to EVERY tier** —
S05000SIM G20 L2W **+50 cy at 50.1° elevation** — a genuine detection gap unrelated to this card.

⚠⚠ **FIELD (§13): the STRUCTURE reproduces, the LEVERAGE does not.**

| arm | station spread | half-to-half transferability | OOS station stratification | implied-c curve |
|---|---|---|---|---|
| sim 24 sta, 1 h (`pareref_on`) | 4.7× | +0.847 | 4.46 % → 0.97 % (**4.6×**) | 2.25→3.93 ⇒ 1.97→2.39 |
| field 49 sta, 1 h GPS+Gal (`net_matrix_2024-129/net50_1h`, DOY-129) | **10.5×** | **+0.873** | 27.58 % → 18.48 % (**1.5×**) | stays steep, 3.07 → 6.02 |
| field 303 sta, 354 ep, 1.02 M cells (`pdp305_pinaware/h3_guard`, 3 h) | — | **+0.782** | 9.57 % → **10.31 %** (WORSE than shipped); station × epoch 8.42 % (**1.14×**) | — |

Two mechanisms, both measured. (1) The per-station factor is **not stationary over 3 h** (+0.782), so
a once-fitted factor is stale by the second half — and not for sample-size reasons (~1300
cells/station/half at 305 vs ~200 at 24). (2) **σ̂₀ is clamped at the 1.0 floor on 95.7 % of
`net50_1h` epochs and 88.1 % of `h3_guard`'s, against 33 % on the sim**; in that regime the
denominator is constant and the statistic is an unnormalized max against a fixed 6σ bar, so a
stratified pool has nothing to fix. Why the field pool sits under the floor closes the loop: for a
post-fit residual E[w·v²] = r_i < 1, so the missing √r_i is exactly what pushes the median under 1.
⇒ **UNCLAMP FIRST on the field**, and R1's slow factor must TRACK (EWMA, time constant unmeasured),
not be fitted once; the slow/fast split survives, "slow = constant over the run" does not.
⚠ **Field runs have no truth** — **27.3 %** is not a false-alarm rate, and the tail (q999 **879**,
max **38 935**, 40 cells > 1000) contains real events; these are archived pre-VS2026 runs read for
residual STRUCTURE only, never for an accuracy verdict. Falsified in passing: the constellation axis
is a null (GPS vs Galileo scale **1.05×**, flag rates **27.23 %** vs **27.48 %**).
⚠ `scripts/t3_null_fit.py` **timed out (50 min) on 1 M cells** — a tool defect, since field scale is
exactly where it matters.

✅ **THE L1C EXCESS IS EXPLAINED — the residual lives in the IONO DIRECTION (§7.6).** Decomposing the
phase residual 3-vector onto (1,1,1) / (1,γ₂,γ₅) / perp: **common 5.02 % · IONO 89.76 % · perp
5.22 %**. With the common mode removed the iono direction is **(−0.804, +0.280, +0.525) —
L1-dominant**, because L1 has the SMALLEST iono coefficient (1 vs **1.65**/**1.79**) and sits
furthest from the mean. L1C is not noisy; **it is where unabsorbed iono-direction error lands.** The
per-station heterogeneity is the same effect: loudest 4 stations **93.2 %** iono share at **36.3 mm**
rms, quietest 4 **65.6 %** at **8.2 mm**. *Structural corollary:* every signal-independent error
(clock, tropo, orbit, position, tides) is identical in METRES across signals and can only appear in
the common direction — 5 %. So the culprit is necessarily frequency-dependent, and wind-up (∝λ) and
the ionosphere itself (∝1/f²) both have the **wrong sign**. ⚠ `network.estimate_iono_sh` is **OFF**
on this arm (free per-LOS slant iono), so an SH-truncation explanation is excluded.

✅✅ **MECHANISM — per-LOS IONOSPHERE ESTIMATION ERROR, truth-graded (§7.8).**
`network.trace_ionosphere` was frame-broken exactly as CLAUDE.md warns (it emitted the correction
`x_r` alone against a state SEEDED at allocation — median **0.95 m** vs the true **8.88 m**, ~11 % of
the quantity). Fixed on the ZWD block's pattern: `iono_m` unchanged, `x_lin_m` + **`iono_total_m`**
appended; trace-class, so with it armed the four graded CSVs are bit-identical to
`runs/pareref_on/logs_v2`. Arm `runs/t3iono`, config `_simnet24_inj_t3iono.json`, graded against
`truth_atmo.csv` `I_L1_m` over **13 054** settled triples. The state is LUMPED, so per invariant #5
only the datum-free **interaction** r (after removing receiver a(sta) and satellite b(sat)) is
gradable: **rms 0.764 m**, per-station **0.106 … 2.551 m**.
**corr(log station variance factor, log iono interaction rms) = +0.9258**; the **lumped** term
correlates **-0.103** — so it is NOT the receiver code-bias partition and not anything the datum can
move. **Causal chain:** iono state error **0.817 m** rms → ~96 % absorbed by the collinear per-arc
ambiguity → unabsorbed ~4 % lands in the phase residual in the iono direction (**0.033 m** rms) →
L1-dominant per §7.6 → the 29× per-station variance-factor spread. ⚠ It tracks the error's **LEVEL**
(+0.532), **not** its rate of change (+0.046) — the natural "the ambiguity soaks up the constant,
only the variation shows" guess is falsified. ⚠ **R2 is therefore non-negotiable**: a per-station
variance component would bury a measured 0.11-2.55 m per-station ionosphere error inside the noise
model.

✅✅ **AND IT IS THE INTEGER-RESOLUTION ROOT — MEASURED (§7.9).** From the phase observation equation
an iono error d forces `e_j = (gamma_j/lambda_j)·d` cycles into the ambiguity; in DD form the station
and satellite lumps cancel exactly, so the driver is the DD of the §7.8 interaction.

| band / lane | sensitivity (cyc/m) | rel. to L1C | measured rms DD error (rel.) | corr with DD iono error | var. explained, 0 free params | `round(e)=0` |
|---|---|---|---|---|---|---|
| L1C | **5.255** | 1.000 | 1.000 | **+0.9266** | **67.9 %** | 5.1 % |
| L2W | **6.744** | 0.283 | 0.085 | **+0.9543** | **81.0 %** | 51.0 % |
| L5Q | **7.037** | 0.056 | 0.018 | **+0.9585** | **82.8 %** | **95.9 %** |
| WL | **−1.489** | — | — | — | — | — |
| EWL | **−0.293** | — | — | — | — | — |

(7326 quadruples of `dd_cyc` − DD_true from `truth_ambiguities`. ⚠ the documented 1.49/0.29 lane
figures are MAGNITUDES; the sign is negative.) **EWL is the only near-integer rung and it is the one
with 18× less iono sensitivity** — the pre-registered handoff shape, now measured not inferred.
⚠ Two honest gaps: the FITTED per-band slope is ~**1.85×** the predicted one (**9.7 vs 5.26** on L1C)
and is unexplained (not a datum artifact — the DD annihilates both lumps); and the measured lane
ladder is ~3× steeper than the sensitivity ladder, so the lanes cancel more than iono alone.

❌ **TEN further candidates falsified (§7.7) — do NOT re-run**: phase multipath (arm) · code
multipath (arm) · redundancy r_i (census) · elevation **+0.24** · arc age (flat) · iono MAGNITUDE
**+0.089** · iono RATE OF CHANGE **+0.102** · receiver code biases **+0.264** (S05 has the LARGEST GF
bias and is nearly the QUIETEST) · phase-bias anchor/datum (universal at 22/23 stations) ·
constellation (1.05×). ⚠ Method note: candidate 8 was proposed from two cherry-picked extremes and
collapsed under correlation over all 23 stations — **propose from the correlation, not the ends.**

❌ **The top lead was the multipath, and it is FALSIFIED (§7.1).** The sim's Gauss-Markov phase
multipath (2 mm zenith, τ = 300 s, per-station seed) was zeroed outright
(`config/sim/simnet24_inject_mp0.json` → `runs/simnet24_mp0` → `runs/t3mp0`), with the control that
regenerating the *unmodified* fixture reproduces `runs/simnet24_inject_null` **byte-identically**
(5/5 RINEX by sha256). Station spread **4.7× → 4.8×**, per-signal spread **2.70× → 2.77×**, flag rate
went **UP (3.663e-02 → 3.906e-02)** — every structural feature survives removing the error source
entirely. Bonus: the removal lowered all three per-signal levels ~8–10 % uniformly and the statistic
did not follow — a second, independent confirmation of the invariance theorem.

❌ **The redundancy family is falsified too (§7.2), and the prerequisite got built.** New
**`network.trace_raim_redundancy`** (default OFF) emits one row per post-fit RAIM **phase row** with
`w, r_i, resid, z_meas, z_baarda, below_floor` — one level finer than the cell. It makes `row_r`
COMPUTED but never USED, so unlike `raim_redundancy_normalize` **the census is solve-neutral**: with
it armed all six graded CSVs are bit-identical to `runs/pareref_on/logs_v2` (+17 % runtime, **65 364
rows**). Arm `runs/t3ri`, config `_simnet24_inj_t3ri.json`. **r_i is uniform**: per signal L1C
**0.5469** / L2W **0.5767** / L5Q **0.4630** = **1.25× spread** against a `z_meas` spread of
**2.59×** — and dividing by √r_i makes the signal spread *worse* (2.59× → **2.72×**). Per station r_i
spreads **1.23×** against `z_meas` **3.01×**, corr(log z_meas, log r_i) = **−0.284**. Whole settled
population: median r_i **0.5114**, q01 **0.0015**, q99 **0.6687**. So "L2W/L5Q are suppressed by
absorption" is wrong — absorption would mean LOW r_i on exactly those signals. **The heterogeneity is
genuine misfit varying by station and signal, on an estimator whose information distribution is
uniform.**

✅ **`rowRedundancy` AUDITED and EXONERATED on four links (§7.4)** — it IS the right covariance: the
arrowhead block-inverse identity is EXACT (re-derived by hand); the datum is a true Dirichlet
elimination (contribution to RHS, row+col zeroed, diagonal set — `NetworkEstimator.cpp:280-292`), so
`shared_cov_` is genuinely the datum-CONDITIONAL covariance and the zeroing only strips the
`1/pin_scale` artifact; the priors are in the same system (`addStationPrior` throws if `solved_`; DD
priors folded pre-solve); and `reconstructStation` uses the identical `N_rr`/`N_sr`. The Bayesian
expansion also shows `Cov(v) = W⁻¹ − AΣAᵀ` survives a prior — **provided P is the true dispersion of
θ about θ₀**. That proviso is the whole story.

⚠⚠ **So the defect is the MODEL, measured as a VARIANCE FACTOR.** With `E[w v²] = r_i`, the estimator
`σ̂² = Σw·v²/Σr_i` must read 1.0. On **29 333** settled rows: **global 11.80**; by signal **L1C 27.37
/ L2W 2.05 / L5Q 5.87** (13.4× spread); by station **2.14 … 61.80** (**28.9× spread**) — all while
mean r_i stays flat at **0.45–0.55 (1.24×)**. **r_i correctly describes how much freedom the
estimator HAS; it cannot describe how wrong its model IS, and only the latter varies.** L2W at 2.05
is nearly model-consistent while L1C is 27×, so the mis-specification is CONCENTRATED on L1C rather
than spread over the pool. ⚠ Outlier-sensitive — aggregate **11.80** vs robust median **1.78** says a
robust numerator is not cosmetic.

~~⚠⚠ **A sharper defect fell out (§7.3): E[w·v²] ≠ r_i.**~~ *(superseded by §7.4 — the measurement
stands, the attribution moves from the covariance to the model.)* If `v` were a correct post-fit
residual, `1.4826·med|z_baarda|` would be 1.0. Measured: **z_meas 1.3683** (the shipped,
theoretically WRONG standardization) vs **z_baarda 1.9291** (the theoretically CORRECT one) — the
correct one lands **2.5× further from nominal**. Candidate then considered: `rowRedundancy` evaluates
on the gauge-nulled free covariance (`NetworkEstimator.cpp:451-453`), which is not the system that
produced `v` if the datum constraints bind. It explains §5 cleanly — B2 measured null *and* pushed
σ̂₀ **1.246 → 1.819** because it divides by something that is not the residual's variance. Incidental:
**the ignition ramp IS r_i** (0.0000 → ~0.52, then flat), so `s_raw`'s **0.836 → 1.816** rise is
redundancy converging, not the residual growing; the floor drops only **0.49 %** of settled rows,
identically across signals (**49/10026** each).

⚠ **"Excess kurtosis 4391" does NOT reproduce** — settled `s_raw` on the null arm has excess kurtosis
**10.38**. Do not carry 4391 forward without re-deriving it.

**Replacement, specified (NOT built):** a slow per-station factor × the existing fast network-wide
scale — the split is forced by the noise budget (a per-station-per-epoch scale has **24.3 % SD**;
pooled over the window, **5.7 %**) — with the station factor promoted to a **declared statistic**
rather than silently absorbed (invariant #7: absorbing model error into a denominator masks it), α
set by **FDR** over the (cell × epoch) family instead of a fixed k, and the **1.67 %**
structurally-untestable cells (`s_raw` exactly 0) dropped from the opportunity count. The right
per-stratum form is the variance component **σ̂²_stratum = Σ w·v² / Σ r_i** (§7.5) — what
`docs/SPEC_prior_residual_census.md §14` derives globally, now computable per station and per signal
because r_i is streamed; unbiased under the model rather than calibrated to a reference quantile, no
`n_c` assumption, and it degrades gracefully where a median of maxima conflates "quiet" with
"unchecked". Full build plan and the four owed validations: `docs/SPEC_t3_null_derivation.md` §8.

**What survives:** ~90 % iono-direction residual (two independent fixtures) and a 29× per-station
variance factor stable in time. Next probe: grade the **estimated states against truth**
(`truth_atmo`, `truth_bias`) instead of inferring from residuals.

**The class, ranked** (§11): **T3c (2.85e6×), T4a (4.89e5×, and it acts at ANY σ̂₀) and T4 are free
riders** on this fix — same σ̂₀, same `raim_k`, same file.

⚠⚠ **Two collateral defects found while inventorying, both verified on the null arm, both needing
their own card** (§14):

1. **The prefit census's PHASE statistic is degenerate by 4–5 decades on truth-clean data** —
   `pc_phase_max_s` median **1.024e5 σ** and `pc_phase_n1k` median **112 rows/epoch**, against a sane
   code class (**75 σ**, `n1k` = 0). `trace_prefit_census` is in the STANDARD ARMED SET, so this
   column is published network-wide; it is inert only because `prefit_event_classes` defaults to
   `code,sh,other` (`GnssConfigLoadNetwork.cpp:1410-1412`) — **adding `phase` to that mask, or arming
   `prefit_fuse_classes` with phase, yields ~112 fuse events per epoch on clean data.**
2. **T1's flag rate excludes an acting sub-path** — reported `26482 opp / 0 flags / rate 0` while
   `network_events.csv` carries **13 `T1_if_slip/refsat_reset` events**. `fr_if_slip` counts only the
   per-pair loop (`NetworkSlipQc.cpp:530`); the reference-satellite common-mode path refloats with no
   counter and no tier — standing invariant #4, one tier over from the T4a line added to fix exactly
   this.

**Sim validation — two standing rules for anyone fitting this null:**

- ⚠ **The fixture cannot calibrate a 6σ-class quantile empirically at all.** 9975 settled cells
  resolve no rate below ≈ 1e-4; a 5.9e-9 threshold would need ~1e8 cells, i.e. ~1e4 of these runs.
  The instrument is for measuring the pool's STRUCTURE and for the fit → out-of-sample verify
  protocol — never for reading a tail quantile off the data. This is why the replacement had to be
  DERIVED rather than fitted. Harness: `scripts/t3_null_fit.py` (`--selftest` / `--run` / `--fit …
  --verify …`).
- ⚠ **The verification arm must be SAME-REGIME, different-seed — NOT the 60-sta fixture.**
  `runs/simnet60_tri` is LEGACY-N (N ∈ [1e5,1e6)) and a different posture while the 24-sta null is
  field-N; the regime alone moves the DD channel by 24 points (`project_sim_field_n_regime`). Fitting
  on one and verifying on the other conflates REGIME with STATISTIC. Generate the verification
  fixture from the same `config/sim/simnet24_inject.json` with a different roster salt — the roster
  is deterministic in N, so only the noise/integer realization moves.

*(Both blockers that once held this item are CLEARED: R0 landed — σ̂₀_first is a per-epoch
`network_epoch_status.csv` column on every assessed epoch, where before it was emitted only inside a
trip event's `detail` span at `NetworkSolveGlue.cpp:1640`, so there was no null sample on disk at
all; and 1(d) landed, making `runs/pareref_on` the regenerated null arm — measurement-clean AND
carry-marginalized at all 30 handovers, terminal σ̂₀_first **1.39** vs the OFF arm's **2.16**. The
earlier premise "the truth-clean fixtures ARE the null-calibration instrument" was falsified for
`runs/inj_null_pdp`, which is measurement-clean in the strict sense — all 25 `truth_slips.csv` are
header-only — but carries **10 genuine state discontinuities**, the anchor handovers, so fitting a
quantile there calibrates it to the discontinuities.)*

**⚠ NOT in this class:** MW slip thresholds — empirical by standing decision (data-rate-dependent);
do not "theorize" them.

## 5 — The level: enforce the written contract (= LV-3 + edge 3)

⚠ **RE-SCOPED 2026-08-14: same field, smaller term.** The §7.8 truth decomposition puts the overall
iono LEVEL mu at **0.328 m** against a datum-free per-LOS interaction of **0.764 m rms**. This card
is aimed at the minority component of the same error; it slips behind item 8.

**Status:** further along than it looks — `docs/SPEC_level_information_budget.md §30` already
declares per-posture ownership and measures the accident (rx-CB carry ~100 info/epoch on the level
direction vs the designed authorities' 0.25/0.01); LV-2's demean removes the carry's net force. The
card's own verdict stands: freeze ⇒ +20 TECU, loosen ⇒ +25, remove ⇒ −9/epoch collapse — the carry
is the wrong vehicle in every tuning direction.

**Remaining:** a per-epoch data-derived authority for postures where the designed owners are weak —
INTERNAL data only (GIM stays diagnostics-only, standing directive).

✅ **TWO MOVES EXECUTED 2026-08-21.**  (1) The deferred **S2H 12 h tracking verdict was READ**
(`SPEC_level_information_budget.md` §31): fold ceilings hold, P2 drains perfect, but |a00 − GIM| =
13.6/9.6 TECU at h0/h11 — the SECOND pre-registered branch: the per-epoch-valued a00 anchor is the
designed next authority (and the rxcb σ̂₀-compounding sub-gate failed: ON growth 2.255 vs ctl 1.831 —
S2H is fold-side cheap, not free, at 12 h).  (2) That authority is being built internally-sourced as
**LV-6 (`SPEC_level_authority_internal.md`)** — the per-epoch fleet CS-GF census
(`network.level_authority_csgf`, measure-only, adversary-reviewed; the fold stage is a future card
gated on the census's §4 tracking verdict, with G1 on a REGENERATED wsh fixture carrying injected
receiver biases and G2 against the sampling-matched censusOperator{GIM} reference — the panel killed
the naive fleet-median-vs-planetary-mean grade before it shipped).  The item stays OPEN until the
census verdict lands; the falsified-tuning ledger is unchanged.

**Sim validation:** the one venue where the LEVEL grades against truth directly. ⚠ Level claims need
the ≥2 h convergence budget — 1 h windows are rank-only.

## 6 — Retire the half-epoch back-off ritual

**Bandaid:** every `start_offset_hours` is backed off half an epoch because bare hour values sit on a
double-residue boundary (CLAUDE.md "cold-start hygiene") — a floating-point bug institutionalized as
ritual.

**Principled replacement:** integer week/TOW arithmetic or round-to-nearest-epoch semantics at the
epoch seek; then delete the convention from CLAUDE.md. Bundle the recorded leap-second lookup defect
(memory `project_point_codebase`, `TimeUtils.cpp` leap table) into the same hygiene card.

**Sim validation:** unit tests + a bare-hour config sweep on the 24-sta fixture that must produce
epoch sets identical to the backed-off twins.

## 7 — SH health gate release policy (= edge 4)

**Bandaid:** a binary withhold with no release/degrade path — a sick-field era withholds the wire for
its full duration (95+ min measured, DOY-129), and the doctrine itself grades a withhold as an
adaptation failure.

**Principled replacement:** attribute-adapt-emit within the detected epoch, with the release decision
evaluated against the ADAPTED solution — the degrade-during-withhold DEADLOCK is already documented
(`docs/SPEC_day129_frame_guards.md Parts C–F`); the fix must not re-test against the withheld state.

**Sim validation:** needs an injected sick-field / storm mode in the injection bench — a bench
extension, owed before this card can be graded.

✅ **The A4 HALF of this item EXECUTED 2026-08-21 — mechanism VALIDATED, promotion REFUSED**
(`docs/SPEC_withhold_era_release.md`; the SH-gate emission-only half above remains open and is NOT
covered).  The fld63 29-epoch A4 deadlock era (`SPEC_slip_relevel_walk.md` §8.4) was root-caused to a
BUDGET-limited within-epoch tier (`n_adapt` pinned at the hard cap 5 for 29 epochs with culprits
still clearing k=6) whose verdicts die with each withheld epoch; the v1 fix (commit the recipe's
carry erases durably) was KILLED in adversarial review before build — the scratch re-solve had
already removed the culprits and σ̂₀ stayed 9.3–26.5, and the commit's blast radius under a stable
scapegoat re-implements the 900 s mass-expiry cliff as policy (SPEC §2-R; do not rebuild).  The v2
build — `network.raim_withhold_adapt_escalate` (era-gated deeper peel budget, scratch-solve only,
verdict provably untouched: 315/315 pre-divergence epoch-status rows byte-equal) +
`raim_withhold_free_erase` (the era-slip response hole) — collapsed the era **29 → 1 withheld epoch
(three 1-epoch eras, all `escalate_landed`, 0 exhausted)** at availability **478/483** (beats both
references) and absorbed the mid-era second event withhold-free.  **But the pre-registered settled-σ̂₀
gate FAILED (2.42 vs ≤ 1.35)**: armed-OFF's reference was cliff-flattered AND the escalation removes
the cliff's accidental therapy — the stressed zero-q carry population persists and pays out under T4
attrition (the 8(b) plateau's representation defect on a third instrument).  Both knobs stay
default-OFF research; arming is a service-posture (availability-first) decision with its own
consumer-graded card.  Byte-certified OFF (products + five CSVs ≡ `65ba362` refs), suite 1480/206
green, no A4-tune (the ceiling and statistic are untouched).

## 8 — The per-LOS ionosphere representation (NEW 2026-08-14, ranks FIRST)

**Bandaid:** none — a *measured root* that three separate cards have been treating downstream of.
Item 4 was calibrating a test statistic against it, the level card is tuning a carry against a
different component of it, and the AR channel has been graded as an integer problem when 68–83 % of
it is this.

**Measured** — the §7.8/§7.9 numbers and the consumer chain are in item 4 above and not repeated;
what this item adds is the OWNERSHIP split of the same decomposition `e = mu + a(sta) + b(sat) + r`
(arm `runs/t3iono`, 13 054 settled triples, per standing invariant #5):

| term | value | who owns it today |
|---|---|---|
| overall level mu | 0.328 m | item 5 (the LEVEL card) |
| station lump a(sta) | −3.14 … +4.16 m | the rx-CB / DCB datum |
| satellite lump b(sat) | −3.12 … +4.74 m | the sat-DCB datum |
| **interaction r — datum-free, nobody owns it** | **rms 0.764 m** (0.106 … 2.551 by station) | **this item** |

Chain in one line (detail in item 4): r → ~96 % absorbed by the collinear per-arc ambiguity → the
unabsorbed ~4 % is 89.8 % of post-fit phase residual variance, L1-dominant → T3 false alarms and DD
integer error (68–83 % explained, corr 0.93–0.96).

### 8(a) — re-measure under the DIRECTIVE posture.  One knob.  Do this first.

⚠ **The entire item-4 lineage ran with `network.estimate_iono_sh` OFF**, and that knob is a Tier::ON
posture item carrying a standing USER DIRECTIVE ("always on";
`GnssConfigNetworkPosture.cpp:174-179`). It is absent from `_simnet24_inj_pareref_on.json` and
therefore from the 1(d) validation arms too. The posture condition (`n_sta >= 3 && tri`) *is*
satisfied on this fixture, so the check fires and the arm is a genuine deviation.

So **the 0.764 m is measured on a free-slant-only posture, not the mandated one**, and the first
question is whether it survives with the SH field armed. Everything needed exists: the trace landed
(§7.8), the grader is a scratch join, the arm is ~35 s Release. Three outcomes, all decisive:

- **Error collapses with SH on** ⇒ the root is *slant states underdetermined without the SH field*;
  item 8 largely closes, and item 4's null should be refitted on the correct posture (every §4/§7
  number is then posture-specific).
- **Error persists** ⇒ the root is the representation itself, 8(b) is the real card, and the D5 hold
  is genuinely lifted.
- **Error grows** ⇒ the SH field is fighting the slant states, which is its own finding.

⚠ **Grade `iono_total_m`, never `iono_m`** — the latter is the seed-less correction and was ~11 % of
the quantity (§7.8).

✅ **8(a) MEASURED 2026-08-20 — the error PERSISTS.** Same-build two-arm mini-ladder
(exe `a4b3eee9`, configs `_q8a_{ctl,sh}.json`, both re-run so the control shares the build;
grader promoted from the scratch join to `scripts/simnet_iono_interaction.py`, identical
13 281 settled triples in both arms):

| arm | posture | interaction rms | by station | b(sat) lump range |
|---|---|---|---|---|
| `runs/q8a_ctl` | free-slant + GF datum (the item-4 lineage) | **0.7061 m** | 0.175–2.232 (S06 worst) | −4.22…+3.86 m |
| `runs/q8a_sh` | DIRECTIVE (`estimate_iono_sh` deg-8, cutoff 25, GF datum off per interlock) | **0.6691 m** | 0.225–2.202 (S06 worst) | −1.29…+0.77 m |

The SH field absorbs the satellite-common structure (the b(sat) lump collapses 5×, as it
should) and moves the interaction **−5 % only**.  Per the pre-registered outcomes: **the
root is the representation itself, 8(b) is the real card, and the D5 hold is genuinely
lifted.**  Item 4's sim null numbers are NOT invalidated by the posture deviation — the
heterogeneity survives the directive posture with the same worst station.  Note the arm
could not literally be "one knob": `code_bias_gf_datum` is hard-refused with
`estimate_iono_sh`, so the swap is GF-datum-off + SH-on (datum change ⇒ grader change —
the interaction decomposition is datum-free by construction, so it grades across the pair).

**Collateral finding (needs its own derivation before any defect claim):** the fitted
per-station iono lump a(sta) tracks the demeaned truth rx-GF bias (2-band convention
`(d1−d2)/(1−μ12)`) at **slope +1.86, corr +0.77, IDENTICAL across both postures** — a
clean Table-8 split would read ~0 (rx-DCB states hold it) or ~±1 (iono absorbs it whole).
The fixture is TRIPLE-band, so the 2-band GF predictor may simply be the wrong lump
coefficient — derive the 3-band absorbed lump before reading 1.86 as over-absorption.

### 8(b) — the representation decision, now with an instrument

Slant vs SH vs hybrid, previously held as "a science program, not a patch swap" because nothing
quantified the representation error. The interaction rms is that number: datum-free, graded against
truth on the sim, with a measured consumer chain. Related standing material:
`project_slant_iono_vs_sh_speed` (2.42× faster, 2.2× worse), `project_global_sh_iono`, and the
lane-spectrometer verdict that *"chord churn IS the iono representation"* — which this measurement
now supports quantitatively rather than by inference.

⚠ **The FIELD evidence on this axis already exists and is negative in BOTH directions —
`docs/RUNQUEUE_pdfix_reask.md` P3 / P3b**, 63-sta DOY-129 on a working (pd_safe) fold: subtractive
(layer2 off, mech-B off, σ_rep 5 → 50) = **complete null**; additive (degree 8 → 15) = **an expensive
null (+34.4 % wall)** and σ_rep 5 → 1.5 = a **FAIL** with the documented aliasing signature. The
cross-band ratio read **1.263-1.265 in all seven arms** against common-cycle 1.2833 — the residual's
DIRECTION never moved, so the defect is not the shell's spatial-spectral flexibility. Scope this card
so it does not re-run that ladder: the new thing here is a *truth-graded* representation-error
measurement, which the field could not supply.

**Sim validation:** already the venue — the fixture supplies truth slant iono per LOS
(`truth_atmo.csv` `I_L1_m`), which the field never could. Grade the datum-free interaction, never the
raw error (the lumps are 4–6× larger than the signal).

⚠ **What is NOT established:** n = 1 fixture, GPS-only, 1 h, slant-only posture; the
fitted-vs-predicted DD slope discrepancy (~1.85×) is unexplained (§7.9); and no field arm is possible
in the graded sense — the field has no truth ionosphere, so item 8 can be *graded* only on the sim
and *monitored* on the field.

✅ **8(b) EXECUTED 2026-08-20/21 — the temporal-accumulation-channel family is FALSIFIED at all
four points, wiring proven live; the EVENT channel is exonerated.**  Full record
`SPEC_iono_carry_8b.md` (diagnosis §2, design §3, results §6, closure §6.6); this entry indexes.
Built: `network.carry_iono` (+ `iono_carry_scope` relevel|always, `iono_carry_source`
solve|smoothed_gf, q/floor/gap knobs, `NetworkQClass::IonoCarry`, prires family `iono`, seven
counters both-edges per invariant #7, conditional fingerprint) and the CS-GF authority
(`networkIonoCsUpdate`, Hatch-class carrier-smoothed GF-code slant iono, slip-funnel reset) — all
default-OFF, OFF path byte-certified four exe increments deep vs `runs/camp60_refprod` +
`runs/camp60_f4/logs`, suite 1469/205 green.  Arms on the slipped 60-sta fixture
(exe 6B3935C3…, `_camp60_{i1,i2,i3,i4}.json`, single-knob over the certified OFF path):

| arm | source × scope | interaction rms | ZWD settled | T4 arcs | verdict |
|---|---|---|---|---|---|
| OFF (f2/f4) | — | 3.568 | 2.845 | 7 876 | baseline |
| i1 | solve × relevel | 3.800 | 3.099 | 8 165 | **FAIL** (churn killed, plateau ROSE) |
| i2 | solve × always | 3.726 | 3.140 | — | **no-drain band as pre-registered** |
| i3 | smoothed_gf × relevel | 3.592 | 2.852 | 8 105 | **FAIL** (= OFF; authority live, 677 848 blends) |
| i4 | smoothed_gf × always | 3.181 | **3.656 ramp** | **13 982** | **FAIL** (relocation, not drainage) |

Pre-registration (`_camp60_i3.json`): interaction ≤ 2.0 REQUIRED; > 3 ⇒ the code-authority drain
model is also wrong ⇒ stop.  Both CS-GF arms > 3 ⇒ stopped, no tuning.  The i1 consumer win
(1.90 vs 3.26) is source-specific (consensus re-assertion = wire continuity) and does not survive
CS-GF (4.48/4.28) — no surviving carrier, do not quote it.  ⚠ The first i3/i4 attempt was a
MISWIRE (all counters 0 — caught by invariant #4, fixed at `NetworkRawObservables.cpp:114`,
byte-recertified); never quote its numbers.  **Closure: three interventions (churn kill,
event-time code re-draw, per-epoch code pinning) all leave the plateau — it is not event-fed; it
is the stationary equilibrium of the per-epoch ZWD⇄iono⇄level exchange under mid-arc zero-q level
lock-in.**  Residues: the O2 / carry_invariant-lineage accumulator (mid-arc transport NOT the
leveling carry) needs its own derivation card before ranking; the CS-GF kernel stays as
instrumentation; quiet-twin CS-GF arm dropped as moot; the plateau keeps its owner here.

---

---

## Blocked on root cause — do not build the "correct" fix yet

- ~~**⚠ NEW 2026-08-14 — the clock re-realization churn itself (item 3).**~~ **✅ RESOLVED same day —
  root NAMED and A/B-confirmed. Moved to item 1(d).** The four nulls and three falsifications below
  are all *correct as measured* and all *on the wrong side of the edge*: every one probes the
  design-matrix side of the gauge, while the defect is on the CARRY side. The live lead recorded
  below — "threshold crossings of a slowly-evolving quantity … shifted by exactly one epoch" — was
  the right instinct and resolved structurally, not statistically: `NetworkPhaseDatum.cpp:339` sets
  `target = 0.0` and overrides it only `if (is_handover && handover)`, so at T+1 the target reverts
  and the continuous pin is a **one-epoch deferral by construction**. The decisive test deleted the
  entire within-epoch QC tier and the re-realization SURVIVED (terminal `clock_std_m` 15.28 → 14.63,
  against 2.15 on the pdp-OFF twin) — so the tier is the *impulse channel*, not the source. Full
  evidence: `SPEC_pivot_visibility_gauge_defect.md` §5.07.

- **⚠ ORIGINAL (superseded) — the clock re-realization churn itself (item 3).** FOUR independent
  interventions on the phase-bias gauge left it untouched on the 24-sta arm: datum election on the
  prioritized path, `phase_datum_handover_continuous`, the pin bridge, and the
  `phase_datum_min_stations` coverage gate (that last byte-identical at 1/2/3 — the gate never binds
  at this density). Every arm holds at **7–8 episodes and Σ|dev| ≈ 160 m**. Three candidate roots
  tested and falsified: anchor handovers (compensated, no effect), column-set changes (0/15
  correlate), and the CLOCK datum station's own visibility (1/6 — the mechanism that DOES explain the
  legacy PB defect). Four nulls plus three falsifications is strong evidence this is **not a gauge
  problem at all**. Live lead: the episodes behave like threshold crossings of a slowly-evolving
  quantity — arming the handover knob shifted every one of the seven by exactly one epoch.

- **Trio-runaway (settled triple-freq divergence):** the INC-1 FREEZE is a bandaid that measured as
  ~35 % contributor, NOT root (runaway survives 1720 → 1132 cyc). Mechanism card first (suspects: tree
  re-hosts refolding invariant carry; chord-carry × slant-iono feedback). FLOAT stays the track.
- ✅ **D5 born-chord gating — HOLD SUBSTANTIALLY LIFTED 2026-08-14.** The hold below was "replacing D5
  means re-deciding the representation — a science program, not a patch swap", and the reason it
  stayed a science program is that the representation error had never been quantified. It now is:
  datum-free per-LOS interaction **rms 0.764 m**, truth-graded, with a measured consumer chain into
  both T3 and the DD integers (`SPEC_t3_null_derivation.md` §7.8–§7.9). See item 8. ⚠ Still hold D5
  itself until 8(a) says whether the error survives the directive posture.
- **D5 born-chord gating (original):** the lane spectrometer verdict is that chord churn IS the iono
  representation. Replacing D5 means re-deciding the representation (slant vs SH) — a science
  program, not a patch swap. Keep D5 until then.

## Leave alone — falsified or decided; do not re-litigate

- ZOH clock dynamics (C1: the rate state measured net-harmful; ZOH is MMSE here).
- MW slip thresholds empirical (standing user decision).
- Lane layer OFF (measured at both 63 and 305 stations, settled windows).
- Era-2 adaptive iono q (falsified).
- Datum-choice-at-emit as a consumer fix — ensemble PB, clock s2z, broadcast-anchored (falsified;
  see item 3).
- Reset-over-repair and withhold-as-backstop doctrines — principled; the gap is item 7's release
  policy, not the doctrines.

## Maintenance

Strike items as their cards land; this doc indexes, the SPECs carry the detail. When item 1a/1c land,
update `docs/SPEC_pivot_visibility_gauge_defect.md` §5.1 and the memory card
`project_pivot_visibility_gauge_defect` in the same commit. Item numbers are STABLE — express a
re-ranking in the BUILD ORDER block at the top rather than renumbering, so cross-references from
other docs keep resolving.
