# SPEC — the lane-integer slip cost is RESPONSE-inflicted (decomposition + fix ladder)

**Status: measured 2026-08-21/22 on `runs/camp60wu_f1`; the F3 hypothesis arm is
the open validation.  Follow-up to `SPEC_slip_relevel_walk.md` §9's finding that
live slips cost ~25 points of at-own-true-integer rate at BOTH lane rungs
(EWL(2,5) 95.1 → 70.2 %, WL(1,2) 46.2 → 21.1 %, matched slipped-vs-quiet).**

Instrument: `scripts/simnet_lane_slip_age.py` — joins
`network_dd_fractional.csv` to the fixture's true integers exactly as
`simnet_amb_integer.py` does (python-int truth, cumulative `truth_slips`
deltas), forms lane errors `e_lane = e_band1 − e_band2` on matched
`(tow, sat, ref, sta_a, sta_b)` quadruples, then buckets them THREE ways:

1. by TRUTH slip age of the quadruple's legs (in-lane-band vs any-band),
2. by ESTIMATOR response age (`network_events.csv`, refloat-class events
   `{if_slip, mw_wl_slip, lli_slip, attrib_arc}` — false alarms included),
3. by the CLASS of the nearest response (≤ 20 ep).

Method caveats carried from the O2 card: rows are heavily repeated per key —
only medians and rates over large n are quoted, never p-values; the trace pairs
against one ref PRN per (const, signal), so quadruples share ref legs; the
"quadruple" is a truth-side identity, blind to the estimator's internal chord
lifecycle (which is the point — the response events supply that side).

## §1 The headline decomposition (GPS EWL(2,5), slipped arm `camp60wu_f1`)

| population | n | med\|e\| (cyc) | at own integer |
|---|---|---|---|
| **RESPONSE-VIRGIN** (no refloat-class event on any leg, ever) | 16 036 | **0.065** | **98.9 %** |
| within 20 ep of `mw_wl_slip` | 45 744 | 0.224 | 75.4 % |
| within 20 ep of `lli_slip` | 9 876 | 0.292 | 70.3 % |
| within 20 ep of `if_slip` | 6 240 | 0.305 | 65.6 % |
| **within 20 ep of `attrib_arc` (T4)** | **178 965** | 0.326 | **65.4 %** |
| response age 61–200 ep | 49 677 | 0.199 | 83.8 % |

Galileo EWL(5,7) response-virgin: **100.0 %** (n 15 435, med|e| 0.063).
GPS WL(1,2) response-virgin: 67.7 % vs 17–21 % near-response.

**Reading:** on 2 486-slip data, chords no response ever touched are
essentially perfect at the EWL rung — better than the quiet twin's pooled
95.1 % (which pools its own young arcs).  The entire lane-integer slip cost is
**response-adjacent**: each refloat-class event re-randomizes the lane level,
recovery takes 60–200 ep, and at 12 356 events / 720 ep the network never gets
that much quiet time on a leg.  T4 `attrib_arc` alone is 7 648 of the 12 356
events and ~74 % of the near-response row exposure at the worst per-event
quality — the same T4 storm `SPEC_slip_relevel_walk.md` §5 called a CO-SYMPTOM
is, on the lane-integer instrument, the single largest destroyer of integer
structure.

## §2 Truth-side corroboration (independent of the event log)

- **In-band truth-slip age shows NO healing under the refloat response**:
  GPS EWL 65.3 % at age 1–5 ep → 64.9 % at 21–60 → 72.6 % at 201+ — the
  re-level bakes in a wrong lane level for the life of the arc.
- **Other-band-only leg slips** (truth continuous in BOTH lane bands, vertex
  refloated anyway): EWL 85.1 → 74.8 %, WL 34.6 → 19.4 % — 10–15 points of
  pure response-inflicted damage, no in-band information loss at all.
- **Galileo EWL(5,7) is robust everywhere** (94.1 % virgin → 87–93 % aged):
  λ_EWL(5,7) ≈ 9.8 m makes a code-derived re-level good to ~0.03 cyc, so the
  response barely dents it.  GPS EWL(2,5) at λ 5.86 m absorbs ~2× the damage;
  per-band (λ 0.19–0.25 m) absorbs all of it.  The damage scales exactly as
  1/λ_lane — consistent with metre-class level error injected per response.

## §3 The fix ladder (leverage order), and what each must pre-flight

1. **T4 keep+retap instead of refloat — RAN, and the gate fired the OTHER way
   (FALSIFIED as a blanket policy).**  `runs/camp60wu_f3` (`_camp60wu_f3.json`,
   retap live: 24 870 kept / 462 escalated, 720/720 emitted; the bridge applied
   33 times — negligible, the retap is the mover):

   | settled (last 25 %) | F1 refloat-always | F3 keep-always |
   |---|---|---|
   | GPS EWL(2,5) at own integer | 70.2 % | **54.0 %** |
   | GPS WL(1,2) | 21.1 % | 13.9 % |
   | ZWD hourly err rms | 1.85–3.08 flat | 2.20–3.28 flat |

   Blanket keep is 16 points WORSE than blanket refloat on the lane instrument
   while staying ZWD-NEUTRAL — the §8 "truth-neutral" grade was
   instrument-limited, not wrong.  The age decomposition explains it: under
   keep, rows within 5 ep of a response fall to EWL 47.7 % (refloat: 66.6 %)
   with med|e| 0.544 — **at flag time the flagged arc's level really is wrong
   more often than a code re-level**, and var 0.25 anchors it.  Both blanket
   policies lose; the population is heterogeneous and only per-arc EVIDENCE
   separates it.

   **Rung 1′ — the corroborated retap (BUILT 2026-08-22, default-OFF):**
   `network.cycle_slip.relevel_retap_corroborate` gates the keep on POSITIVE
   pre-solve raw-observable continuity — the slip-QC QUALIFY verdicts (measured
   AND not stepped), the same tri-state the D5-v3 corroboration gate consumes.
   Unmeasured / stepped / no-evidence arcs take the full erase (invariant #1).
   Wiring: `NetworkRaim.cpp` keep-branch gate fed by `RawSlipEpoch*` through
   the seam signature; the knob registers as a slip-QC DECISION card
   (`SlipQcParams::retap_corr` in `anyConsumer` + `cardsArmed` — without that
   the qualify pass never fills the verdicts and the gate silently degenerates
   to refloat-always, the exact trap the D5-v3 auto-arm comment names).
   Counters both ways (`corr_kept` / `corr_stepped` / `corr_unmeasured`),
   teardown line prints all three; loader interlocks: corroborate without
   retap_statistical warns + disarms, and dies with its base under
   carry_invariant.
   **Validation arm pre-registered (`_camp60wu_f4c.json` = free_epoch +
   retap_statistical + corroborate, NO bridge): corr counters nonzero BOTH
   ways; settled GPS EWL ≥ F1's 70.2 % (must not lose to refloat-always),
   target toward the 85 %+ virgin band; ZWD flat in F1's band; census
   unchanged.**
2. **Band-scoped response**: LLI detections are band-specific and the §2
   other-band class shows vertex-wide refloat costs 10–15 points on bands
   whose truth is continuous.  Refloat only the detected band's level where
   the detector localizes (LLI always; MW/GF pairs localize under 3 bands).
3. **Lane-preserving re-level**: when band b must refloat, seed its new level
   to keep the MEASURED lane delta integer: the EWL(2,5) MW-analogue measures
   the lane jump at σ ≈ 0.05 cyc single-epoch (0.3 m code on λ 5.86 m) — a
   confident integer snap for EWL, marginal for WL (σ ≈ 0.35), impossible
   per-band (the 6.66° geometry, `project_why_non_integer_rootcause`).  Use
   the bridge-v2 pend-and-corroborate shape (`SPEC_slip_relevel_walk.md`
   §8.2), NOT the killed mw_only direct write (§8.1: 0/49 correct — that
   repaired the PER-BAND level, a different and impossible object).

Invariant pre-flight (standing invariants #1/#4/#7):
- #1: a KEPT arc past a real slip carries a stale level — retap's variance
  re-tap is the wired response; the blind spot is a slip equal-in-cycles in
  ALL bands (lane-silent), which GF still sees (equal cycles ⇒ ΔGF =
  (λ₁−λ₂)·n ≠ 0) — any keep-gate must consult GF before keeping.
- #4: keep/refloat decisions need counters with identities on BOTH outcomes.
- #7: a keep-gate is a new edge at both ends — event rows at decide time,
  and the kept arc re-validates (or re-taps) at the next MW confirm.

## §4 What this does NOT claim

- Nothing here credits or involves the wind-up sign fix — the sim is
  structurally blind to it (`SPEC_phase_windup_sign.md` §2).
- The quiet twin's own EWL ceiling (95.1 %) and the response-virgin 98.9 %
  bound what lane-fixing can deliver on THIS posture; the per-band rung stays
  owned by iono representation regardless.
- Field transfer is not automatic: the field's T4 population and false-alarm
  mix differ (the campaign's field arm hit an A4-withhold deadlock era on an
  unrelated axis) — a field leg re-grades this before any default flips.

## §5 Rung 1′ VERDICT (2026-08-22, `runs/camp60wu_f4c`) — FALSIFIED AS ARMED; the evidence lacks resolution

720/720 emitted; counters both ways (24 315 kept-with-evidence, 360
refused-STEPPED, 0 refused-UNMEASURED — the wiring is live and the qualify
pass ran).  But the refusal rate is **1.5 %**: the raw MW/GF empirical step
gates call almost every T4-flagged arc FLAT, so the corroborated keep
degenerates to near-blanket keep and inherits its harm:

| settled GPS lane | F1 refloat-always | F4C corroborated keep | F3 keep-always |
|---|---|---|---|
| EWL(2,5) at own integer | **70.2 %** | 59.6 % | 54.0 % |
| WL(1,2) | **21.1 %** | 17.0 % | 13.9 % |

**Root cause — the evidence measures the wrong failure mode.**  A raw MW/GF
step gate detects DISCRETE jumps at multi-tenths-of-a-cycle thresholds; the
levels T4 catches are DRIFTS (sub-threshold accumulation) that the network
solve sees at > kσ̂₀ while the per-arc band-pair observables see nothing.
"No step" and "level wrong vs the network" are simultaneously true, and the
T4 flag IS the higher-resolution evidence — corroborating it against a
coarser instrument and keeping on the coarser instrument's silence was
backwards.  The knob stays built, default-OFF, counters intact (it is the
correct SHAPE for any future finer evidence source), but do NOT arm it with
the raw-step verdicts as the evidence.

**Standing conclusion of this card:** for T4-flagged arcs the refloat is the
better response (the code re-level is EWL-good at λ 5.86 m); the recoverable
losses are (a) rung 2 — vertex-wide responses hitting truth-continuous bands
(10–15 pts, band-scoped refloat where the detector localizes), and (b) rung 3
— making the refloat LANE-PRESERVING: after the per-band code re-level,
integer-snap the EWL lane to its EWL-MW-measured delta (σ ≈ 0.05 cyc
single-epoch ⇒ a confident snap), bridge-v2 corroboration shape.  Rung 3 is
the next build; this decomposition (F1/F3/F4C + the age tables) is its brief.

## §6 Rung 3 v1 BUILT — the lane-preserving seed snap, pre-registered

> **⚠ SUPERSEDED by §7.2/§7.3.**  The mechanism described below was DELETED on
> 2026-08-22: its only write was an `x_lin` shift on a local ambiguity column,
> which the reduced network solve absorbs exactly.  Read this section as the
> record of what was tried, not as a description of the code.

`network.cycle_slip.relevel_lane_snap` (default OFF, byte-identical): on every
fresh per-band ambiguity seed (birth or post-refloat re-level) in the per-band
seed loop (`MeasurementModel::ensureRequiredStates`), the non-anchor bands of
the (station, sat) are re-anchored so each lane equals the epoch's IONO-FREE
MW float — replacing the (α_a/λ_a − α_b/λ_b)·(I−Î) iono-seeded lane error
(measured med 0.22–0.33 EWL cyc post-response) with the MW measurement
(σ ≈ 0.05 cyc EWL, ≈ 0.35 WL).  Anchor preference: lowest-band KEPT live
state (lane continuity through a band-scoped event), else lowest fresh seed.
**FLOAT snap, never rounded** (per-arc MW bias ⇒ rounding is an integer
commitment, the killed §8.1 shape).  **Values only — the diffuse seed
variance is untouched**, so nothing becomes stiff and no invariant-#1 event
edge is created; skip channels (`no_pair`, `wild` at a 25-cyc cap) and both
anchor classes are counted per station and summed at teardown (invariant #4).
BSSD-SD seeding is excluded (different algebra; not the producer path).

Rung-2 note: with the snap ON, even VERTEX-WIDE refloats become
lane-preserving (all-band events take fresh-anchor snaps — lanes land on MW
regardless), and band-scoped events take the kept-anchor path automatically —
so rung 3 subsumes rung 2's LANE benefit.  Rung 2's residual value is
per-band ABSOLUTE continuity only; narrow the LLI response scope later only
if F5 leaves points on the table.

**Validation arm pre-registered (`_camp60wu_f5.json` = free_epoch +
lane_snap, retap and bridge OFF): snapped counter in the 1e4–1e5 class with
small skip channels; settled GPS EWL at-own-integer ≥ F1's 70.2 % with the
target the 98.9 % response-virgin ceiling; WL improves materially (0.35-class
snap vs the 0.97–1.6 inherited error); per-band UNCHANGED (the snap moves
lanes, not absolutes — a per-band move means the mechanism did something it
must not); ZWD flat in F1's band; census unchanged.**

## §7 Rung 3 v1 FALSIFIED BY ITS OWN COUNTERS; v2 is the DELTA form

> **⚠ SUPERSEDED by §7.2/§7.3.**  Both diagnoses below are correct about the
> symptom and wrong about the cause: the site could not actuate the solve at
> all.  The DELTA form itself survives — it is the algebra §7.3 measures, moved
> to the carry domain.

`runs/camp60wu_f5` (v1, absolute-MW snap): 720/720, but **24 368 of 26 372
snap attempts (92 %) blew the 25-cyc sanity cap** and the 2 004 applied
(mean |d| 2.96 cyc) left the integer grade statistically at F1 (settled EWL
70.2 %, traces differ, effect diluted to invisibility).  Root cause, in two
layers:

1. **Bias-convention mismatch.**  The seed lane is state-bias-CLEANED (the
   modeled phase subtracts the shared PB states; code biases live in CB/rxCB
   states), while the raw MW float CARRIES the true satellite+receiver code
   and phase biases.  d = implied − mw measures the bias gap, not the lane
   error — tens of cycles through the per-signal phase biases.
2. **DD-closure break.**  Even where applied, an ABSOLUTE re-reference moves
   snapped arcs onto the raw-MW convention while unsnapped arcs stay on the
   state convention; a DD mixing the two sees the convention DIFFERENCE.
   Partial application is structurally wrong for any absolute snap.

**v2 (the card's original delta design, now implemented):**
`lane_target = pre_lane + (mw_now − mw_pre)` — the pair's own pre-event lane
(the erased state values, captured before the overwrite) plus the MW-measured
lane JUMP across the event.  Every per-(station, sat, pair) bias is constant
across the event and cancels in BOTH differences, so each arc keeps its own
convention and snapped/unsnapped arcs mix freely in a DD.  `mw_pre` = median
of the pair's rolling MW history aged (60, 330] s (the 2-epoch exclusion
absorbs detector latency); canonical (lo, hi) pair keys with a sign map;
births skipped and counted (no pre-event lane to preserve).

**F5B pre-registration (same arm config, v2 exe): wild count COLLAPSES
(v1's 92 % was the falsification signature); snapped dominated by the reseed
population with |correction| in the 0.1–2 cyc class; settled GPS EWL ≥ F1's
70.2 %, target the 98.9 % virgin ceiling; per-band unchanged; ZWD flat.**

### §7.1 v2.1 verdict (`runs/camp60wu_f5c`) — the SEED SITE is the wrong site; rung 3 OPEN at the leveling inject

> **⚠ SUPERSEDED by §7.2.**  The working hypothesis below was right about the
> destination and understated the severity.  Note in particular that "the knob +
> counters + MW-history infrastructure stand" is **no longer true**: the counter
> struct and the MW ring were deleted with the mechanism; only the knob NAME
> survives, now arming the §7.3 observatory.  ⚠ §7.1's closing directives are
> RETRACTED: "do NOT arm `relevel_lane_snap`" and "the field lane-snap arm is ON
> HOLD" both referred to the DELETED seed-site mechanism.  The knob now arms a
> measure-only observatory, and `_fld63_lanesnap4h.json` is re-pointed at it.

History fixed (74 no-history vs F5B's 18 399), yet **21 461 of 22 068
attempts (97 %) still blow the 25-cyc cap** and the 533 applied average
15.9 cyc — the discrepancy `implied − (pre_lane + Δmw)` is STRUCTURAL and
its scale matches the x_lin-DEVIATION frame class
(`SPEC_o2_carry_invariant_accumulator`: state z vs absolute (z+z_lin)/λ,
median 78.5 cyc).  Working hypothesis: on the leveling lineage the
per-station ambiguity seed is not the object that carries the lane — the
LEVELING CARRY (local_carry) re-leveled at the free-epoch INJECT is — and
the pre-event state value and the fresh seed do not live in one frame the
lane algebra can difference.  Three counter-caught iterations (v1
bias-convention, v2 history placement, v2.1 frame) all died BEFORE reaching
a field arm — the pre-registration + counter discipline working as designed.

**Rung 3 status: OPEN, re-scoped to the leveling-inject site** (apply the
delta to the CARRY levels at `relevel_free_epoch`'s inject, where the lane
actually lives).  The knob + counters + MW-history infrastructure stand;
do NOT arm `relevel_lane_snap` until the inject-site derivation card lands.
The field lane-snap arm (`_fld63_lanesnap4h.json`) is ON HOLD accordingly;
the field CONTROL (`_fld63_relevel4h_ctlw.json`) runs regardless — it is
the standalone field-stability measurement on the wind-up-fixed binary.

### §7.2 Rung 3 CLOSED at the seed site — the mechanism was an exact solve NO-OP (2026-08-22)

§7.1 named the v2.1 residual "structural" and matched its scale to the
x_lin-DEVIATION frame class, then filed a working hypothesis about *where* the
lane lives.  The hypothesis was right about the destination and wrong about the
severity: the seed site was not a mis-framed actuator, it was **not an actuator
at all**, and all three iterations (v1 bias-convention, v2 history placement,
v2.1 frame) were debugging a write that could never move the solve.

**The derivation, in two independent kills.**

1. **The write is an x_lin shift, and x_lin is a FRAME.**  The snap's only
   effect was `state_manager_.setStateVector(x_snap)` on a local ambiguity
   column.  On the network producer the per-station `StateManager` vector is a
   frozen LINEARIZATION POINT: it is written once at setup
   (`ApplicationNetworkSetup.cpp:574`) and the solve is never written back.
   Shifting local column *c* by δ moves the data rows as `y → y − A_r e_c δ`,
   hence `u_s → u_s − N_sr e_c δ` and `u_r → u_r − N_rr e_c δ`, leaving

   ```
   b_red = u_s − N_sr N_rr⁻¹ u_r      EXACTLY invariant
   ```

   and `x_r → x_r + e_c δ`, so the ABSOLUTE estimate `x_lin + x_r` is unchanged.
   The carried leveling prior does not rescue it either — its RHS is built as
   `η = Λ·(value_abs − x_lin)` (`ApplicationNetworkAssembly.cpp`), which absorbs
   the same δ.  And the graded instrument is x_lin-invariant too:
   `network_dd_fractional.csv` forms `value_m = x_lin + x_r`
   (`NetworkDdFrac.cpp`).  **The snap could neither be right nor be seen.**
   That is why F5/F5B/F5C all "left the integer grade statistically at F1 with
   traces differing" — the trace differences were second-order leakage through
   x_lin-dependent row-set gates (the SD-EWL `max_abs_z_m` correction-frame
   screen, the INC-2 prefit fuse), not the mechanism working weakly.

2. **`d` was never a lane error.**  With a KEPT anchor the anchor is by
   construction excluded from `lane_snap_seeded`, so `pre_a == x_snap(idx_a)`
   identically, the anchor term cancels, and for an unslipped anchor the
   integer cancels against `s·ΔMW`.  What survives is

   ```
   d = −[ e_b(t) − e_b(t_b0) ] / λ_b   =   −Δe_b / λ_b
   ```

   — the change in **one band's** frozen-frame model error (frozen receiver
   clock, frozen ZWD, frozen iono seed; `StateManager::computeAmbiguitySeed`)
   between that band's last seed epoch and now, divided by a **single-band**
   wavelength.  `1/λ_L2 = 4.095 cyc/m` is **3.53×** the lane coefficient
   `1/λ_WL = 1.160 cyc/m`, so F5C's applied mean |d| = 15.9 cyc is **3.9 m of
   frozen-frame drift** on the band-b seed.  The 97 % cap-blow rate was the
   mechanism correctly reporting that it was measuring the wrong quantity.

**Landed:** the seed-site mechanism, its counter struct (`LaneSnapCounters`),
its MW ring (`lane_mw_hist_`) and its run-end readout are **DELETED**, with the
derivation kept in-code at the deletion site so it cannot be rebuilt by a future
session reading only the SPEC.  `network.cycle_slip.relevel_lane_snap` is kept
as a name but **warn-and-disarmed unconditionally** in the loader until the
carry-domain replacement lands — the previous parse comment ("works with or
without phase_leveling … so no interlock") is exactly what let an inert knob be
armed in two configs and graded as a null result.

**Where the lane actually lives — the v3 target, derived not guessed.**
`LocalCarry::value_abs` is *"the carried ABSOLUTE ambiguity estimate (m)"*
(`NetworkRunTypes.hpp`), formed at the post-solve harvest as
`value_abs = pa.x_lin + xr(pa.col)` (`NetworkRunContext.cpp`) and injected next
epoch as a prior row.  That IS the estimate, so the frozen linearization is not
merely small there — it is **structurally absent**.  The correction

```
δ = [C_a(t⁺)/λ_a − C_b(t⁺)/λ_b] − [C_a(t⁻)/λ_a − C_b(t⁻)/λ_b] − ΔM
```

carries residual `ε_L(t⁺) − ε_L(t⁻) − ε_MW`, with `ε_MW = √2·0.713·σ_P/λ_WL` =
0.351 cyc on GPS L1/L2 and **0.051 cyc on L2/L5 EWL** at σ_P = 0.3 m.  Expected
|δ| is O(0.1–2) cyc, not O(16).  **The algebra closes in the carry domain.**

One frame availability fact makes v3 buildable at all: `relevel_free_epoch` only
SKIPS the carried-prior inject (`++R.relevel_free_skips; continue;`) — it never
erases — and the harvest overwrites `local_carry` entry-by-entry.  So `C(t⁻)`
and the free re-level `C(t⁺)` coexist, in ONE gauge, within ONE epoch, on the
serial path.

**⚠ Two hazards the v3 build must answer before it is armed**, both surfaced in
review and neither present in the seed-site design: (a) `value_abs` is
per-(sat,signal) **gauge-bound** and is shifted by the eq34 D4 S-transform
(`NetworkChainDatum.cpp`) between a PASS-A capture and the harvest, so a pend
must be voided on `datum_changed`/`eq34_reref` and that void must be COUNTED;
and (b) the phase-leveling T0 repair adds `dN·λ` to `value_abs` earlier in the
same epoch, so a same-epoch T0-repair + free on one (sta,sat) makes `pre_lane`
already post-slip while `ΔM` still carries the raw step — a whole integer,
mis-attributable to detector latency.  Both are interlock-and-count, not
redesign.

**Status: rung 3 seed site CLOSED (deleted).  Rung 3 v3 (carry domain) is BUILT
as an OBSERVATORY in §7.3, on exactly this reasoning — compute δ and
histogram it without applying — because three consecutive iterations died on
unreconciled counters and the |δ| distribution is the one number that decides
whether the apply leg is worth building.**  The field arm `_fld63_lanesnap4h.json`
is re-pointed at the observatory (§7.3), not on hold — but note it is now
**byte-identical to `_fld63_relevel4h_ctlw.json` on the solve**, which is itself
the wiring check: the two arms must grade identically while only the observatory
counters differ.

### §7.3 Rung 3 v3 BUILT — the carry-domain lane OBSERVATORY (2026-08-22, pre-registered)

`network.cycle_slip.relevel_lane_snap` is re-pointed from the deleted seed site
to the leveling carry, and its first leg is **measure-only**.  That is a
deliberate response to the failure pattern: three iterations died on
unreconciled counters, and the |δ| DISTRIBUTION is the single number that
decides whether an apply leg is worth building at all.  An armed run is
**byte-identical on every solve artifact** — a product difference is a bug, not
an effect.

**The mechanism.**  Two halves, both keyed to one epoch:

- *PASS A* (after both slip drains, so the freed set is complete) stashes only
  the RAW MW observable per (station, satellite) into `ctx.carry.relevel.lane_mw`
  — the pair `(sig1, sig2)`, its wavelengths, and `ΔM = d_mw / λ_WL` in WL cycles
  with the detector's own `measured` flag.
- *The harvest* reads BOTH lane levels: a pre-event snapshot taken before the
  overwrite loop, and the post-event levels after it, then forms

  ```
  δ = [C_a(t⁺)/λ_a − C_b(t⁺)/λ_b] − [C_a(t⁻)/λ_a − C_b(t⁻)/λ_b] − ΔM
  ```

**Why the split is exactly there.**  `LocalCarry::value_abs` is per-(sat,signal)
**gauge-bound** — the eq34 D4 and phase-datum S-transforms shift it — so a
pre-lane captured at PASS A and a post-lane read at the harvest would straddle a
frame step, which is the same class of unmodelled frame term that killed v1/v2/
v2.1, one layer up.  Carrying only the OBSERVABLE across the seam (raw MW is
untouched by any S-transform) and reading both levels inside the harvest puts
them in one gauge **by construction**, rather than interlocking around the
hazard after the fact.

**Counters** (`n_lane_*`), with two closure identities printed at run end and
flagged `*** MISWIRE ***` when they fail:

```
attempts == measured + skip_no_mw + skip_unmeasured
                     + skip_no_pre + skip_no_post
                     + skip_bridged + skip_bad_lambda
measured == Σ hist[0..8]
```

The |δ| histogram (< 0.1 / 0.25 / 0.5 / 1 / 2 / 5 / 10 / 25 / ≥ 25 cyc) is
accumulated **unconditionally** — the seed site's `abs_delta_sum_cyc` summed only
AFTER its cap, which is precisely why its own falsification took three runs to
surface.  The measured population is split `both_freed` (vertex-wide response)
vs `one_freed` (band-scoped; the partner's carry is continuous), because rung 2's
residual value is exactly that contrast.

Interlocks (warn-and-disarm): requires `phase_leveling` (no leveling carry, no
levels to difference); requires `relevel_free_epoch` **evaluated on the SETTLED
flag** (`free_ambs` is the population — without it every counter is a structural
zero indistinguishable from dead wiring); dies under `carry_invariant` with its
base.  Pinned by `tests/test_lane_observatory_config.cpp` — the END STATES only: the
rider's ordering (it sits after the free_epoch disarm so it reads the settled
flag) is not separately observable through the public getters, because every
posture that settles `relevel_free_epoch` to false also disarms the observatory
by another route.  The ordering rests on code placement, and the test says so.

**Pre-registered reading of the first arm** (`_camp60wu_f5c`'s posture with the
observatory on — slipped wu93 sim, where truth integers exist):

- **PASS**: the mass of |δ| sits **below ~2 cyc**, with the `<0.5` bins
  populated.  Residual is `ε_L(t⁺) − ε_L(t⁻) − ε_MW`; `ε_MW` is 0.351 cyc on
  GPS L1/L2 and **0.051 cyc on L2/L5 EWL**, and `ε_L(t⁺)` is exactly the free
  re-level's lane error this card exists to remove — so a median δ in the
  0.2–0.4 cyc class on EWL would both confirm the frame AND size the prize
  against §1's measured 0.326 cyc near-response median.
- **FALSIFIED**: mass in the `≥ 10` / `≥ 25` bins.  That is the seed site's own
  signature (97 % above 25 cyc) and would mean the carry domain has a frame term
  too.  **No apply leg is to be built on a falsified observatory** — that is the
  whole point of running this leg first.
- **MISWIRE, not a null**: `attempts = 0` on a slip-bearing armed run, or either
  closure identity failing.
- `skip_unmeasured` dominating means the MW step is rarely formable at the free
  epoch — a real limitation on the delta form itself, and a redesign trigger
  rather than a tuning one.

Only if the observatory passes does the apply leg get specified, and it inherits
two hazards already identified and NOT yet built for: the same-epoch T0 phase-
leveling repair (which adds `dN·λ` to `value_abs` earlier in the epoch, making
`pre_lane` post-slip while `ΔM` still carries the raw step — a whole integer,
mis-attributable to detector latency), and the variance question (a value moved
by a model correction re-injected at the unmoved data-derived weight is
invariant #1's stiff-prior shape re-created by the response).

### §7.4 First smoke — the observatory caught its OWN dead wiring (2026-08-22)

Recorded because the failure mode is the reusable part, not the fix.

`_camp60wu_f6smoke.json` (short clone of the `_camp60wu_f5c` slipped posture,
~40 epochs, wiring check only — 40 epochs is deep inside ignition and says
nothing about the settled lane) reported:

```
159 attempt(s); 0 MEASURED
Skipped: 0 no MW pair, 159 MW unmeasured (no adjacent predecessor), 0 no
pre-event carry, 0 not re-harvested, 0 delta-bridged, 0 bad wavelength
CLOSURE 159 vs attempts 159 (OK); histogram sum 0 vs measured 0 (OK)
```

**Both closure identities held and the card was still completely inert.** Zero
"no MW pair" proved the raw ROWS were present and dual; every candidate died on
`RawSlipVerdict::measured`. Root cause: the raw-observable QUALIFICATION pass —
the only writer of `measured` and `d_mw` — runs behind `SlipQcChannel::cardsArmed()`,
and the observatory had been added to neither that predicate nor `anyConsumer()`.
Rows are produced by `networkComputeRawObservables` (gated on `anyConsumer`);
verdicts by `networkQualifyRawObservables` (gated on `cardsArmed`). Arming the
first and not the second yields exactly this signature: full rows, empty
verdicts, perfect closure, zero information.

Two lessons worth carrying past this card:

1. **A closure identity is not an aliveness check.** It proves the accounting is
   complete, not that the mechanism ran — every candidate landing in ONE skip
   channel closes just as cleanly as a healthy distribution. The aliveness check
   is a channel-SHAPE expectation, so the run-end line now says outright that
   `skip_unmeasured` ≈ 100 % of attempts means the qualification pass did not
   run, i.e. ARMED-BUT-INERT rather than null.
2. **A consumer of a gated upstream must register in every gate that upstream
   has, not just the outermost.** `SlipQcParams::lane_snap` now appears in BOTH
   `anyConsumer()` and `cardsArmed()`, with the reason written at the member.

The same smoke exposed a second, quieter defect: `attempts` had been counted over
`lane_mw ∩ freed`, so a freed arc the raw pass never saw silently shrank the
DENOMINATOR instead of appearing as a skip. It is now counted over the freed set
with an explicit `n_lane_skip_no_row` channel (invariant #3 — measure the
deferral, never absorb it).

#### The guarded smoke — the carry-frame diagnosis CONFIRMED on its own instrument

⚠ First reported on an UNGUARDED build and RETRACTED in review; the numbers below
are the guarded re-run.  Three defects were found between the two, and two of them
biased toward this section's own conclusion — recorded because that is exactly the
direction a card is least likely to audit itself in:

- the `any_freed` guard was lost when the denominator moved to the freed LOS set,
  so pairs where a THIRD band was freed (L5 on a triple posture, a second channel
  under `channel_mode: all`) were measured with BOTH carries continuous and filed
  as "band-scoped".  Ordinary carried-prior evolution entered the histogram at
  small |δ|.  Now `n_lane_skip_no_freed_band`;
- `n_lane_skip_no_post` tested carry PRESENCE, not re-harvest.  An un-refreshed
  entry survives a full `pl_max_gap_s`, and accepting it contributes the PRE value
  as the POST value — collapsing δ either to a one-band code re-level (top bins)
  or to exactly −ΔMW ≈ 0.35 cyc (bottom bins).  Now tested on `last_seen_s`, with
  a matching one-epoch freshness test on the PRE side;
- the pair's band codes were matched by FULL RINEX code, but the raw pass names a
  band by its lexicographically first attribute (L1C) while the carry uses the
  estimator-selected code (L1W under a W-before-C priority list).  On a network
  tracking both, every lookup would miss and be reported as "no pre-event carry" —
  a false population verdict rather than a wiring alarm.  Now resolved by BAND
  DIGIT against the carry itself.

Guarded result (exe `906B7C03…`, same config):

```
177 attempt(s); 140 MEASURED (116 vertex-wide / 24 band-scoped)
mean |delta| 1.295 cyc, max 13.929
|delta| hist: <0.1 9 | <0.25 10 | <0.5 24 | <1 38 | <2 37 | <5 18 | <10 2 | <25 2 | >=25 0
Skipped: 0 no raw row, 0 no MW pair, 0 MW unmeasured, 0 pair changed,
         16 no freed band in THIS pair, 21 no pre-event carry,
         0 stale pre-event carry, 0 not RE-STASHED this epoch,
         0 delta-bridged, 0 bad wavelength
CLOSURE 177 vs 177 (OK); histogram sum 140 vs measured 140 (OK)
```

| | deleted SEED site (F5C) | carry-domain OBSERVATORY |
|---|---|---|
| \|δ\| ≥ 25 cyc | **97 %** (21 461 / 22 068) | **0 %** (0 / 140) |
| applied / measured mean \|δ\| | 15.9 cyc | **1.295 cyc** |
| mass < 2 cyc | — | **84.3 %** (118 / 140) |

**Pre-registered PASS condition met** (§7.3: mass below ~2 cyc with the `<0.5`
bins populated — 84.3 % below 2, 43 rows below 0.5) **and the falsifier absent**
(≥ 10 / ≥ 25 bins: 2 rows, 1.4 %).  The ~12× drop in mean |δ| and the total
collapse of the ≥ 25 tail are the frame result the card predicted.

What the guards changed, precisely: the band-scoped population fell **39 → 24**
(the ~15 unfreed pairs), while the vertex-wide population was untouched at 116;
mean |δ| moved 1.284 → 1.295 and the sub-2 fraction 83.2 % → 84.3 %.  **The two
freshness channels came back ZERO**, so that contamination path was latent on this
fixture rather than active — the retraction was procedurally right and materially
small.  Do not read that as licence to skip the guards: the stale-post failure is
bidirectional, and on a lineage with station dropouts it would fire.

⚠ **This is a FRAME confirmation, not a grade.**  40 epochs is deep inside
ignition, and the standing rule against verdicts in ignition applies to the
DISTRIBUTION (mean 1.295 cyc will move on a settled run) — but not to the tail
collapse, which is structural.  The pre-registered settled reading in §7.3 stands
and still needs the full arm.  21/177 in `no pre-event carry` (11.9 %) is the
expected births + T4-erased population.

**What this unlocks:** the apply leg is now worth specifying, because the
quantity it would move is measured and O(1) cyc rather than O(16) cyc of frame
drift. It still inherits the two hazards named at the end of §7.2 (the same-epoch
T0 repair, and the variance question), and neither is built.

⚠ Consequence for grading: the retirement census on this same run showed
**981 T4 attribution refloats** against 678 free-epoch latches over ~40 epochs,
confirming that on this posture the freed population is dominated by T4 — which
catches sub-threshold DRIFTS, not steps (§5). If `skip_unmeasured` stays high on
the fixed build for that reason rather than a wiring one, the delta form itself
is limited on T4-freed arcs, and that is a redesign trigger for the apply leg,
not a tuning one. The counters now separate those two causes.

### §7.5 SETTLED observatory arm + the byte-identity and integer cross-checks (2026-08-22)

`runs/_wd/laneobs` (`_camp60wu_f6obs.json` — the full 6 h slipped wu93 posture,
720 epochs, exe `92FC13A7…`).  The frame result **holds on settling**:

| | smoke (40 ep) | **settled (720 ep)** |
|---|---|---|
| attempts / measured | 177 / 140 | 3661 / **3026** |
| vertex-wide / band-scoped | 116 / 24 | 2298 / **728** |
| mean \|δ\| | 1.295 cyc | **1.181 cyc** |
| < 0.5 cyc | 30.7 % | **35.5 %** |
| < 2 cyc | 84.3 % | **82.9 %** |
| ≥ 10 cyc | 1.4 % | **1.8 %** |
| ≥ 25 cyc | 0 | **0** |

Skips: 375 no-freed-band-in-pair, 260 no-pre-event-carry, and **zero** on every
other channel (no-row, no-MW, unmeasured, pair-changed, stale-pre, not-re-stashed,
bridged, bad-lambda).  Both closure identities OK.

**Pre-registered gates (1)(2)(3) PASS**: measured is in the 1e3 class, the mass
stays below ~2 cyc with the `<0.5` bins well populated (1 073 rows), and it did
NOT migrate into the ≥ 10 / ≥ 25 bins as the run settled — the falsifier is
absent on a settled window, not merely on an ignition one.

⚠ **Gate (4) is NOT EVALUABLE with the counters as built.**  It asked whether the
band-scoped population (partner carry continuous) is CLEANER than the vertex-wide
one — the rung-2 contrast.  The counters split the two populations by COUNT but
share one histogram, so the two distributions cannot be separated.  Recorded as a
gap rather than answered: closing it is one `n_lane_hist_bandscoped[9]` array.
Do not infer the rung-2 contrast from the aggregate.

#### Byte-identity: PASS

`_camp60wu_f6smoke` vs `_camp60wu_f6smokeoff` (single knob, same exe, separate
working directories): **20 of 22 CSV artifacts bit-identical, products.txt
identical**.  The two that differ are `network_run_summary.csv` and
`network_service_health.csv` — exactly where the new counters are reported.  This
matters more than usual here: arming `relevel_lane_snap` also arms
`SlipQcParams::lane_snap`, which turns on the raw-observable pass AND the
QUALIFICATION pass, populating `RawSlipEpoch::stepped` and the verdict maps that
other seams read.  Reading those call sites suggested every consumer was
knob-gated; the A/B proves it.

#### Integer cross-check: the observatory changed NOTHING, as designed

`scripts/simnet_amb_integer.py` on the settled run, graded against the fixture's
known true integers (2 068 671 rows, 720 epochs, 1 715 truth slips), settled
quarter:

| | at its own true integer | med \|e\| |
|---|---|---|
| EWL(2,5) | **70.2 %** | 0.277 cyc |
| WL(1,2) | **21.1 %** | 1.387 cyc |
| L1C / L2W / L5Q / L7Q | 3.0–3.4 % | 9.6–11.0 cyc |

**70.2 % / 21.1 % reproduce §5's F1 refloat-always column to the digit.**  That is
the intended result and the strongest available confirmation that the observatory
is inert on the solve: an instrument that moved the estimate could not land on the
prior baseline exactly.

It is also the honest scale of the remaining problem, and it should be quoted
whenever this card's |δ| numbers are: **the ~12× improvement in |δ| is a
MEASUREMENT improvement, not an estimation one.**  The lane integers are where
they always were — EWL with real structure but far from fixable, WL essentially
without it, per-band with none — against a response-virgin ceiling of 98.9 %
(§1).  The apply leg that would convert the now-measurable δ into recovered lane
structure **does not exist yet**; §7.3's two unbuilt hazards (the same-epoch T0
repair, and the variance question) are its remaining design debt.

### §7.6 FIELD arm (`_fld63_lanesnap4h`, 2026-08-23) — NOT ADJUDICABLE FROM AGGREGATE; the card owes the instrument, not a verdict

`runs/_wd/lanesnap` (61 sta GPS dual WL(1,2), 481 epochs / 4 h, DOY-129
00:00–04:00, warm resume, `relevel_free_epoch` armed, observatory
measure-only).  **The only field-registered pre-commitment (§7.2's wiring
check) PASSES**: `products.txt` SHA256-identical to `_fld63_relevel4h_ctlw`
and `field_lane_frac.py` grades identical — solve-inertness holds in the
field.

Readout (teardown TOW 273630): **143 attempts; 100 MEASURED (94 vertex-wide /
6 band-scoped), mean |δ| 5.945 cyc, max 119.534**.

```
histogram (cyc)  <0.1 5 | <0.25 11 | <0.5 11 | <1 10 | <2 11 | <5 26 | <10 12 | <25 12 | >=25 2
skips            1 MW-unmeasured, 2 pair-changed, 40 no-pre-event-carry (28.0 % vs settled sim 7.1 %), all else 0
closures         143 = 100+1+2+40 (OK); histogram 100 = measured (OK)
```

| | sim settled (§7.5) | **FIELD** |
|---|---|---|
| mass < 2 cyc | 82.9 % | **48 %** |
| ≥ 10 cyc | 1.8 % | **14 %** |
| ≥ 25 cyc | 0 | **2 %** |
| mean \|δ\| | 1.181 cyc | **5.945 cyc** |

**Against §7.3 as written** (a SIM-scoped rule; no field numeric rule was ever
registered): not PASS — the mass does not sit below ~2 cyc, though the `<0.5`
bins ARE populated (27 rows).  Not FALSIFIED — the falsifier names the seed
site's signature (97 % ABOVE 25 cyc, a deterministic O(16)-cyc frame term);
14 % ≥ 10 with 2 rows ≥ 25 over a populated low end is tail migration, not
that signature.  Not MISWIRE (closures OK, attempts nonzero); not
skip_unmeasured-dominant (1/143).  **The result lands in the branch the rule
does not cover, and the rule is applied as written — "mass" does not get a
percentage threshold invented for it after the histogram is on the table, in
either direction.**

Why the aggregate cannot carry the verdict — two structural confounds, both
instrument gaps rather than tuning questions:

1. **The field's dominant freeing channel is INVISIBLE to the observatory —
   verified in source, both directions (2026-08-23, post-adjudication).**
   Live freed-unit census over the same 4 h (console TOW 259230–273630):
   MW 104 + IF-TD 80 arcs vs **6 564 T4/RAIM-attribution refloats — 97.3 % of
   all freeing** — and the T4 channel RAMPS (911 → 1 559 → 2 009 → 2 085 /h)
   where the sim's injected slips are flat (~410/h).  The observatory's
   denominator is `free_ambs` (`NetworkRunContext.cpp:2629`), and `free_ambs`
   has exactly TWO fill sites, both detector drains
   (`NetworkSlipQc.cpp:418` MW/LLI, `:910` IF-TD).  The T4 actuator on this
   phase-leveling lineage is `local_carry.erase` +
   `markAmbiguityPendingReseed` (`NetworkRaim.cpp:789`/`:892`) and never
   touches `free_ambs`; `raim_refloat_pending` exists but drains into the C3c
   funnel only, under `raim_ci_refloat` on carry_invariant lineages — not
   this one.  **So the 143 attempts are detector-driven BY CONSTRUCTION**
   (the census closes: ~184 detector arcs ≈ 143 (station,sat) pairs), and the
   T4 channel's free re-levels — produced through carry ERASURE — are
   structurally unmeasured.  This is a COVERAGE gap, not the mixing confound
   the adjudication first assumed, and it cuts sharper both ways: the
   ≥ 5-cyc mass sits on a nominally CLEAN detector population (the alarm is
   real, pending cause-tag verification row-by-row), while the dominant
   response channel has never had its lane cost measured at all — and δ on a
   T4-freed arc would measure the carry error the refloat REMOVED (dMW ≈ 0
   there), i.e. large δ can be the response WORKING; that census does not
   exist.  T4's erasure churn is also the likely driver of the 28 %
   no-pre-event-carry starvation (T4 erased the carry before the detector
   event arrived — per-event rows would confirm).  (The sim dodged all of
   this because the wu93 fixture's walk is dead, 720/720 — its T4 channel was
   quiet; the field's is the loudest thing in the run.)
2. **The 2–5 bin (26 rows — the field MODE) is elevation-conditional in the
   null.**  The WL(1,2) delta null for this chain — σ_δ = c·σ_P(el),
   c ∈ [0.88, 1.17] cyc/m across the iono-seeding bracket, plus the 0.40-cyc
   O2 floor — predicts median 0.50–0.86 cyc and up to ~15 % spill ABOVE 2 cyc
   at this run's own slip-elevation mix (IF-TD flags p50 24.2°, obs mask 15°;
   the 25° cutoff is SH-weighting only).  Unconditioned 2–5-cyc mass is
   therefore EXPECTED and grades nothing; only the **≥ 5 bins are null-free
   discriminants** (null P(>5) ≤ 0.2 %, P(>10) ≈ 0).  The field's 26 % at
   ≥ 5 cyc is genuinely not measurement noise — but with no per-event rows it
   cannot be conditioned on elevation, cause-verified, or localized to
   (station, sat, TOW), and the model-σ null carries no field multipath, so
   the low-elevation share of that mass is not yet separable from real lane
   damage.

Add §7.5's inherited gate-(4) gap (one shared histogram — the 94/6
vertex/band-scoped distributions are inseparable) and no per-event attribution
exists for the 14 rows ≥ 10 cyc or the 119.534-cyc max (119.5 lane cyc is
~80 m of L1 iono if read as frame error — implausible; attribution owed, not
narrative).

**Verdict: NOT ADJUDICABLE FROM AGGREGATE.**  Neither confirmation nor
falsification of the carry frame is available from the counters as built; the
≥ 5-cyc excess is real signal-or-fault, unattributed.  **The apply leg stays
BLOCKED** (§7.3's gate is a PASS, which did not occur) — and it is not dead
(FALSIFIED did not occur either).

**Owed instrument** (all measure-only, inside the existing
`relevel_lane_snap` knob, fingerprint-SAFE):
(a) per-event rows on the EXISTING events stream (`network_events.csv`,
tier=LANE_OBS — the LV-2 "do not create a new stream" precedent; arm
`network.trace_events`, already in the standard set) — one row per attempt,
detector (`delta`) and T4 (`t4_delta`) alike: TOW, station, sat, freeing
CAUSE (MW / LLI / IFTD / T4A / T3C bitmask), vertex-wide vs band-scoped,
band pair, elevation, δ (statistic), dMW_cyc (threshold), outcome =
measured | skip channel;
(b) the gate-(4) closer `n_lane_hist_bandscoped[9]`;
(c) the cause split on the attempt/measured counters (cause plumbs from the
`free_ambs` fill sites, `NetworkSlipQc.cpp:418`/`:910` — the only two, per
confound (1); MW vs LLI split at `:418` if the drain distinguishes them);
(d) **T4-side coverage** — the dominant channel joins the observatory: at the
T4 erase site (`NetworkRaim.cpp:789`/`:892`) capture the about-to-be-erased
`value_abs` per band into a one-epoch pre-erase map (the values are in hand
immediately before the erase); at the NEXT harvest, for (station, sat) pairs
whose bands were T4-erased last epoch, form the SAME δ against that epoch's
ΔMW and emit rows tagged cause = T4 into (a), with a separate T4 histogram —
the refloat-justification census the rule below reads apart from the
lane-cost grade.  Same one-gauge argument as §7.3 (the raw ΔMW crosses the
seam; both levels are read inside the harvest); same freshness guards.

**Pre-registered rule for the instrumented field re-run — registered NOW,
before the run**: grade the DETECTOR-driven (non-T4) sub-population at
el > 30°.  **PASS**: ≥ 5-cyc mass ≈ 0 there AND median |δ| inside the null
band (0.4–0.9 cyc).  **FALSIFIED**: persistent ≥ 5-cyc mass on that clean
sub-population.  The T4-freed sub-population is read separately as a
refloat-justification census (large δ = removed carry error) and is never
pooled into the lane-cost grade.  Detector sub-population too thin to grade
(< ~30 measured rows) is §7.4's redesign trigger: the delta form is then the
wrong instrument for the field's freed population, and the apply leg's design
starts from the T4 channel instead.
### §7.7 The INSTRUMENTED re-run (2026-08-23) — the pre-registered rule fires its REDESIGN TRIGGER; first T4 census

**Instrument validation, all gates PASS** (build `FF586129…`, Debug suite
1539+1 green):
- Sim A/B `_camp60wu_f6smoke{,off}` (single knob, same exe, separate CWDs
  `runs/_wd/f6on76`/`f6off76`; prior §7.4/§7.5 record preserved at
  `runs/camp60wu_f6smoke{,_off}_pre76`): `products.txt` byte-identical;
  every non-LANE_OBS events row identical; the ONLY deltas are the 477
  LANE_OBS rows + run_summary/service_health/manifest.  The detector readout
  reproduces the §7.4 guarded smoke TO THE DIGIT (177 attempts / 140
  measured) — the instrument perturbs nothing it measures.
- New teardown lines close: band-scoped hist sum 24 = one-freed 24 (gate (4)
  CLOSED on sim: 19/24 below 2 cyc); T4 census closure 300 = 300.  Sim T4
  baseline (healthy fixture, walk dead): 297 measured, mean 3.02 cyc.
- FIELD (`runs/_wd/lanesnap76`, same config): `products.txt` SHA256-identical
  to the control for the THIRD time, now across the instrumented build that
  touches `NetworkRaim` — the capture sites are solve-inert.  Detector
  aggregate reproduces §7.6 to the digit (143/100, mean 5.945).

**The pre-registered rule (§7.6) fires its REDESIGN TRIGGER, not a verdict:**
the detector population at el > 30° has **22 measured rows < 30**.  As
registered: the δ form is the wrong primary instrument for the field's freed
population, and **the apply leg's design starts from the T4 channel**.
Reported (not graded): the 22 high-el rows sit med |δ| 1.342 cyc, 59.1 % < 2,
5 rows ≥ 5 (max 10.3); low-el (78 rows) med 2.298, 26.9 % ≥ 5.  Cause split
over measured: **IF-TD 60 / LLI 62 / MW 6** — the MW-confirmed slip class is
nearly absent from the field's detector-freed population; grader:
`scripts/field_lane_obs_grade.py`.

**First T4 refloat-justification census** (the channel §7.6 confound (1)
showed is 97.3 % of field freeing and was structurally unmeasured):
**3 918 attempts / 2 530 measured; med |δ| 5.98 cyc, mean 14.6**; el > 30°:
1 485 measured, **med 4.36 cyc, 46.1 % ≥ 5** (vs the sim baseline mean 3.02
on a healthy fixture).  ΔMW on these rows is uniformly sub-cycle — the raw
band-pair observables see nothing while the carried lane is typically
MULTI-CYCLE away from the fresh re-level.  Reading: field T4 refloats are
typically removing REAL accumulated carried-lane error (consistent with §5's
resolution finding — T4 catches what MW/GF structurally cannot), and any
apply-leg that would preserve lanes across T4 must first explain a ~4–6 cyc
median discrepancy, not a sub-cycle one.  `t4_skip_gauge` = 32 (the eq34
guard is live); `t4_skip_no_pre` = 784 at el > 30 (attribute re-selection or
absence on re-entry).

⚠ **The extreme T4 tail is NOT yet attributable — one guard is missing.**
The top rows (604 cyc @ el 65°, 445, 356, …; 393 rows ≥ 25 cyc) include
SATELLITE-COHERENT clusters: G14 at TOW 265560–265590 reads ~200–211 cyc
simultaneously at WTZR/WTZS/URUM/SGOC, with `BLOCKBAARDA corrob=raw_flat`
and a PRIORRES pl-family σ̂₀ of 162 at the same TOWs — an estimation-side,
satellite-side event.  The capture is a copy across the epoch boundary and
the eq34 re-reference guard does not cover the ANCHOR-HANDOVER / phase-bias
re-pin frame channel: a family re-pin between capture and measurement would
read as a coherent multi-hundred-cycle δ on every station of that satellite.
Frame-step vs carried-error is not separable in the tail without a handover
cross-guard (join against the handover/pivot events, or a second skip
channel).  **Quote the census by its MEDIAN and the el-conditioned mass, not
by its tail, until that guard exists.**  The 119.5-cyc detector max from
§7.6 carries the same caveat.

**Owed next (in order):** (1) the T4-tail handover cross-guard (a
measure-only join or skip channel — small); (2) the apply-leg DESIGN
re-scoped to the T4 channel per the fired trigger, carrying §7.2's two
hazards plus the new question this census poses (a lane-preserving response
must beat a channel that is measurably removing 4–6 cyc of real error);
(3) the detector-side δ record stays open as a secondary instrument — the
population is simply too thin at high elevation on this posture.
### §7.8 The T4-tail handover cross-guard + the coherence re-grade (2026-08-23)

**Guard extended to every traced frame channel** (measure-only, same knob):
the T4 measurement now skips (`t4_skip_gauge`) when the pair's family appears
in ANY of — the eq34 `(sat,σ)` re-reference set (already live in §7.7), the
phase-anchor election's `phase_anchor_reref` (the non-eq34 lineage channel;
the set deliberately persists across withheld epochs until a stash commits
the new frame), or the new `datum_reref_sats` (the datum-station slip's
network-wide reseed, tagged at the apply edge in `NetworkSlipQc`).  On THIS
lineage the two new channels are inert (zero PBGAUGE / datum events in the
run) — i.e. the §7.7 census was already fully vetted against the traced
channels at its measuring epochs; the extension is correctness for the
postures that use them.

**The eq34 attribution gap is closed**: the chain datum counted 683
re-references but traced only the 148 reattach-hygiene rows — a frame step
could not be joined against the lane census post-hoc.  The full per-epoch
`eq34_reref` set is now emitted as `PHASE_DATUM/eq34_reref` event rows from
the leveling seam (658 rows on the field re-run; the 25-row shortfall vs the
counter is withheld epochs, where the seam does not run).

**Coherence re-grade of the §7.7 census** (`field_lane_obs_grade.py`
`--cluster-*`: same-satellite, ≥ 3 stations, |δ| ≥ 5, gap ≤ 90 s; the
`spread/med` column is the discriminator — a frame constant has ~zero
spread, per-arc carried error does not):

- **122 clusters / 583 measured rows.  Only 34 clusters (127 rows = 5.0 % of
  measured) are TIGHT (spread/med < 1) — the frame-suspect class.**  Exemplar:
  G28 @ TOW 268680, five Americas stations at 74.4–85.6 cyc, spread 0.13.
- The bulk is heterogeneous: the TOW-265590 event — G14 across 22 stations
  AND G22 across 24 stations simultaneously — has spread/med 6–9, i.e. an
  ATTRIBUTION STORM refloating real per-arc error network-wide, not a frame
  event.  (Its epoch carried `BLOCKBAARDA corrob=raw_flat` + a pl-family
  prior-residual σ̂₀ of 162 — the estimation-side signature, with no frame
  event in any traced channel.)
- **De-clustered census — the quotable numbers**: all-T4 med |δ| 3.77 cyc,
  40.8 % ≥ 5; **el > 30°: med 2.94 cyc, 32.2 % ≥ 5** (from 4.36 / 46.1 %
  clustered).  The 604-cyc max is a SINGLE-station event (never coherent) and
  survives exclusion.

**Verdict of the re-grade: the §7.7 census core claim STANDS.**  Field T4
refloats typically remove real multi-cycle carried-lane error — median ~3 cyc
at high elevation with every coherent event excluded — and the frame-suspect
share is 5 %, now permanently separable (grader split + attribution rows).
The §7.7 apply-leg brief is unchanged and sharpened: the mechanism must beat
a channel that is measurably removing ~3 cyc of real error per refloat.

**Certification (build `A9C47C45…`)**: sim A/B PASS — products ON ≡ OFF ≡ the
§7.7 build's products; non-LANE_OBS event rows identical; 477 LANE_OBS rows
reproduced exactly.  Field products SHA256-identical to the control for the
FOURTH time; T4 closures OK; gauge skips unchanged (62).
