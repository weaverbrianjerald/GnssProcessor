# QPR 0–3 — items 0–3: the pin bridge, the S-transform re-reference family, datum-station election by criterion, and the clock's re-realization-free posture

**STATUS**

- **id:** QPR 0–3
- **topic:** queues
- **status:** open
- **summary:** items 0–3: the pin bridge, the S-transform re-reference family, datum-station election by criterion, and the clock's re-realization-free posture
- **dates in the text:** 2026-08-04 → 2026-08-21
- **knobs / posture:** `network.phase_datum_time_linked`, `network.phase_anchor_carry_reref`, `network.carry_coupling_gauge_project`, `network.carry_coupling_reinit_sources`, `network.shared_carry_gauge_free`
- **hashes named in the text** (commit / exe / byte-cert): `ca19d0d`
- **run dirs named in the text:** `runs/agu63_pbv2`, `runs/s24cpl_`
- **source:** `docs/archive/QUEUE_principled_replacements.md` — 0, 1, 2, 3

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: QUEUE_principled_replacements.md -->
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

