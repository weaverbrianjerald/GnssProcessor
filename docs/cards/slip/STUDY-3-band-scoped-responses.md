# Study 3 — band-scoped slip responses, the production knobs — the combined arm FAILS at storm scale

**STATUS**

- **id:** Study 3
- **topic:** slip
- **status:** closed
- **summary:** band-scoped slip responses, the production knobs — the combined arm FAILS at storm scale
- **dates in the text:** 2026-09-10
- **knobs / posture:** `network.raim_erase_band_scope`, `network.cycle_slip.refloat_band_scope`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** `runs/evleg_z_slip3_on`, `runs/evseed_quiet6h`, `runs/evleg_z_`, `runs/simnet60_campaign_wu93`, `runs/golden_sim_campaign_zwdblk_bscope`, `runs/golden_sim_campaign_zwdblk_bscope_t4`
- **source:** `docs/archive/SPEC_event_harness.md` — Z, Z.1, Z.2, Z.3, Z.4, Z.6, Z.5, Z.7

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## Z. Study 3 — band-scoped responses, the production knobs

**Answer, up front: the T4 half is a large, consistent win and the detector half
is not. `network.raim_erase_band_scope: min_keep_one` drops the level floor an
unnecessary attribution erase leaves by 9x / 29x / 64x on the three Study-1 arcs
(0.445 -> 0.049, 0.375 -> 0.013, 0.130 -> 0.002 cyc) and takes the standing slant
iono error with it (-0.083 -> +0.006 m at +45).
`network.cycle_slip.refloat_band_scope: implicated` is byte-exact on a
correctly-scoped single-band slip, but on a GENUINE all-band slip its fallback
pins a band that really moved and the arc is 15x WORSE (floor 0.498 -> 7.658 cyc,
slant iono -0.089 -> -1.302 m). Arm the erase scope; leave the detector scope OFF
until the lane-preserving refloat (`SPEC_lane_slip_response` §7) can re-level the
kept band instead of trusting it.  The 6 h storm-bed arm that carries BOTH (Z.6)
takes 30 % off the ZWD settled truth rms and loses 29 points of settled WL — the
split the leg matrix predicts, on a bed where the detector half pinned a
genuinely-slipped band 1 091 times.** Measured 2026-09-10, 7 legs + 1 storm arm.

### Z.1 What was built

Study 1 (§9) ended by naming the production change it pointed at and deliberately
not making it: the sub-knobs it used were injector-only. §Z is that change.

| knob | values | acts on |
|---|---|---|
| `network.raim_erase_band_scope` | `all` (default) / `min_keep_one` | the T4 attribution erase, `refloat_sat_at(..., statistical_arc=true)` in `NetworkRaim.cpp` |
| `network.cycle_slip.refloat_band_scope` | `all` (default) / `implicated` | the whole-satellite detector refloats in `NetworkSlipQc.cpp` — MW, LLI, the IF-TD pair, the wind-up restart drain |

Both default to today's behaviour and both are **solve-affecting when armed**, so
each is emitted into `run_manifest.json` only when armed (the `mdb` / `adm_screen`
precedent) and each appears in the acting event's `network_events.csv` detail as
`band_scope=...;kept=<band>`.

**`min_keep_one`.** Erase every band of the flagged (station, satellite) whose OWN
post-fit normalized residual |r|/sigma0 exceeds the T4 gate `k*sigma0`, and ALWAYS
spare the band with the smallest |r|/sigma0. Two edges are named rather than
assumed: when NO band clears the gate (the cell was flagged by the median-polish
INTERACTION, which is net of the row and column effects) everything but the pin is
still erased — absence of per-band evidence never suppresses the response
(invariant #1); when EVERY band clears it the pin is the least-BAD band and
`kept_over_gate` counts it. A single-band cell is skipped outright: there is no
band to spare, and freeing the only one is exactly the failure the scope exists to
prevent.

*Per-signal residual source.* `cells`, built in Pass 1 of `networkPostFitRaim` as
one row per **(station, satellite, SIGNAL)** carrying that carried ambiguity's max
standardized post-fit phase residual — the same number the median polish consumes
after `cell_max` collapses the signal axis away. Nothing is re-derived; the scope
reads the value the attribution was computed from, one aggregation step earlier.
The CONSENSUS whole-satellite path (`statistical_arc=false`) is deliberately NOT
scoped: a >= min_sta cross-station agreement is evidence about the satellite, not
about one arc's bands.

**`implicated`.** Free only the bands the detector named; when they cover every
live band of the arc, keep the least-suspect one pinned. "Least suspect" is the
MW/GF decomposition of the arc's primary pair: dN1 - dN2 = dMW/lambda_WL and
lambda1*dN1 - lambda2*dN2 = dGF separate exactly, so
dN1 = (dGF - lambda2*dMW)/(lambda1-lambda2) and
dN2 = (dGF - lambda1*dMW)/(lambda1-lambda2). Bands off that pair carry no evidence
and never out-rank a measured one; with no usable evidence the pin falls to the
lexically lowest survivor (deterministic, and the shortest wavelength). A
single-band arc is never spared here — unlike T4 the detector has POSITIVE slip
evidence, and with one band there is no LOS anchor to preserve anyway.

### Z.2 The defect the harness found on its first ON leg: the invariant is per EPOCH

The first build scoped each drain independently. `runs/evleg_z_slip3_on` (an LLI
on all three bands) then came out **byte-identical to its control** with the
counters reading `1 arc -> 2 freed / 1 kept`. The trace says why:

```
T0_mw_detect,lli_slip,G10,S21000SIM,refloat,LLI;band_scope=implicated;kept=L1C
T1_if_slip,  if_slip, G10,S21000SIM,refloat,L1C;L2W
```

The MW/LLI drain kept L1C. The IF-TD driver then implicated (L1C, L2W) — which
does **not** cover a 3-band arc, so its own scope is correctly a no-op — and freed
L1C anyway. The arc ended the epoch with every band freed, exactly as before.

**A per-drain scope cannot hold a per-arc invariant when several drains act on the
same arc in one epoch.** The decision is now taken against this drain's bands
UNION what earlier drains have already latched this epoch (read from
`relevel->free_ambs` and the Card-4 `carry_slipped_ambs` — both, because which one
is live is a posture decision the scope must not depend on), and the survivor is
chosen only among bands that are still pinned. A `bscope_too_late` counter records
the case where every band was already gone before the drain ran: nonzero means a
freeing site upstream is not routed through the scope, which is invariant #4
applied to the scope's own coverage. It is 0 on every leg here.

This also showed that **the detector scope is NOT inert on a quiet bed**: the
null-ON leg took 20 decisions in 61 epochs, because MW fires on (L1C,L5Q) and
(L1C,L2W) of the same satellite in one epoch and their union covers the arc. That
is a real, correct trigger — and it means **every ON arm must be graded against an
ON null**, which is how the table below is built. Graded against the OFF null the
ON rows carried a pre-event |e| peak at ep -1 and an identical +0.021 m consumer
offset: the background, not the event. (The one-per-satellite MW event row also
hid those keeps entirely; a dedicated `T0_mw_detect / band_scope / keep` row now
carries them — invariant #7 wants an identity at the edge, not a counter alone.)

### Z.3 The legs

Seed `runs/evseed_quiet6h` `state_snapshot.txt.ep0665` (tow 279150), 61 epochs,
event at +10, exactly the §6/§9 geometry. OFF arms are the §6/§9 legs already on
disk; ON arms are `runs/evleg_z_*` (`scripts/ev_study3_legs.py`). Each arm is
graded against the null of its OWN posture (`scripts/ev_study3_table.py`).

**Structural gate first (invariant #6).** `_golden_sim_off60_bscope.json` — the
golden 61-epoch smoke with both knobs ABSENT — reproduces `products.txt` SHA256
`3BF7831B29916C7A1E20434AC91F2ADFEB74EB06974560DF6E050FC43AC3F357`, the pre-card
hash, on the new exe. Unarmed, the new code is unreachable.

| leg | peak \|e\| (cyc) | @ep | +14 | +45 | **floor** | ZWD pk (mm) | dIono +45 (m) | T4/T3 | cons pk (m) |
|---|---|---|---|---|---|---|---|---|---|
| arc1 T4 erase 14.2deg **OFF** | 1.905 | +2 | 0.415 | 0.417 | **0.445** | +3.8 | -0.0832 | 0 | -0.001 |
| arc1 T4 erase 14.2deg **ON** | 0.067 | +1 | 0.059 | 0.041 | **0.049** | -1.9 | +0.0061 | 0 | -0.001 |
| | | | | | **x8.99** | | | | |
| arc2 T4 erase 81.3deg **OFF** | 1.052 | +1 | 0.270 | 0.073 | **0.130** | -4.2 | +0.0206 | 0 | -0.011 |
| arc2 T4 erase 81.3deg **ON** | 0.003 | +19 | 0.001 | 0.002 | **0.002** | -1.6 | -0.0003 | 0 | -0.002 |
| | | | | | **x63.53** | | | | |
| arc3 T4 erase 13.8deg **OFF** | 0.803 | +1 | 0.448 | 0.207 | **0.375** | -2.0 | +0.0405 | 0 | -0.001 |
| arc3 T4 erase 13.8deg **ON** | 0.061 | +3 | 0.035 | 0.002 | **0.013** | -2.5 | -0.0002 | 0 | -0.002 |
| | | | | | **x29.28** | | | | |
| arc1 slip x3 all-band **OFF** | 2.018 | +0 | 0.517 | 0.447 | **0.498** | +4.0 | -0.0891 | 0 | -0.001 |
| arc1 slip x3 all-band **ON** | 9.858 | +0 | 8.652 | 6.766 | **7.658** | +25.8 | -1.3016 | 0 | +0.003 |
| | | | | | **x0.06** | | | | |
| arc1 slip x1 control **OFF** | 0.054 | +11 | 0.051 | 0.037 | **0.044** | +1.4 | +0.0056 | 0 | -0.001 |
| arc1 slip x1 control **ON** | 0.054 | +11 | 0.051 | 0.037 | **0.044** | +1.4 | +0.0056 | 0 | -0.001 |
| | | | | | **x1.00** | | | | |

Per band on the all-band slip pair (floor +14..+45), which is where the fallback's
cost is visible:

| band | OFF (free all 3) | ON (keep L1C) | OFF/ON |
|---|---|---|---|
| L1C | 0.498 | 7.658 | x0.06 |
| L2W | 0.665 | 9.763 | x0.07 |
| L5Q | 0.700 | 10.197 | x0.07 |

### Z.4 Verdict

1. **`raim_erase_band_scope: min_keep_one` does what Study 1 predicted, on all
   three arcs.** arc 1's 0.445 floor becomes 0.049 — the 0.050 the scoped
   injector arm measured, reproduced by the production path. The slant-iono
   column is the direct witness and changes SIGN as well as magnitude
   (-0.083 -> +0.006 m at +45 on arc 1): with one band pinned the two re-tapped
   levels land on the survivor's ionosphere instead of on one epoch of code.
   The win is largest where the floor was cheapest (arc 2, 81.3 deg, x63) and
   still 29x on the 13.8 deg arc — this response does not care about elevation
   the way the unscoped one does, because it is no longer re-drawing (rho, I, N)
   from code at all.
2. **`cycle_slip.refloat_band_scope: implicated` is inert where it should be and
   harmful where the header warned it would be.** The single-band slip control is
   **identical to three decimals on every column** — the knob does not perturb a
   detector response that was already correctly scoped, which is the structural
   check. On a genuine all-band slip it is **15x worse**: the kept L1C really
   moved 10 cycles, the pin injects that into the LOS level, and the other two
   bands re-tap onto it (floor 0.498 -> 7.658, slant iono -0.089 -> -1.302 m,
   ZWD +4.0 -> +25.8 mm). This is the one case the design named as a deliberate
   trade; the trade is now PRICED, and at this price it is not worth taking.
3. **The trade is not symmetric, and that is the useful part.** A T4 attribution
   erase fires on a population that is overwhelmingly NOT slipped (the isolated-arc
   refloat population is FA-dominated — `SPEC_slip_relevel_walk` §6), so keeping a
   band costs nothing and saves the re-level. A detector refloat fires on positive
   raw evidence, so keeping a band is a bet against the detector. Same mechanism,
   opposite prior — which is why one knob is armed and the other is not.
4. **The open build is unchanged and now motivated by a number.** The honest
   handling of a whole-satellite loss of lock is neither "free every band" (0.498)
   nor "keep one and trust it" (7.658) but the LANE-PRESERVING refloat of
   `SPEC_lane_slip_response` §7 — re-level the kept band from the EWL-MW snap
   (sigma ~ 0.05 cyc) instead of carrying its pre-slip value. Until that exists,
   `refloat_band_scope` stays `all`.
5. **QC stays silent, again.** 0 post-event T4/T3 re-flags on the arc in every leg
   of the matrix, including the 7.7-cycle one. §9.6(5)'s finding is unchanged: an
   arc that has just been given a metre-class level error is not flagged for it.

### Z.6 Storm-bed validation — the combined arm FAILS, and Z.4 said which half

`config/test_suite/_golden_sim_campaign_zwdblk_bscope.json` = main's
`_golden_sim_campaign_zwdblk.json` + BOTH knobs (a 3-key diff: the two knobs and
the run path). 6 h, 60 solve stations + 1 held-out consumer, the SLIPPED campaign
bed `runs/simnet60_campaign_wu93` (2 486 truth slips), Release, exit 0 in 1 099.7 s
-> `C:/GnssProcessor/runs/golden_sim_campaign_zwdblk_bscope/`. Gates frozen before
the run.

| gate | zwdblk (reference, same commands) | bscope arm | gate | verdict |
|---|---|---|---|---|
| **ZWD settled truth rms** (`simnet_plateau_decompose` D0) | 1.1884 m | **0.8347 m** (-30 %) | <= 0.5 PASS, <= 0.9 PARTIAL | **PARTIAL** |
| **consumer 3D p50 / rms** (`simnet_grade`) | 1.0853 / 2.3890 m | **2.8920 / 3.0084 m** | p50 <= 0.6 AND rms <= 1.5 | **FAIL** (2.7x / 1.3x WORSE) |
| **ambient DD \|e\|** (`simnet_t4_census` control, n=3000) | 1.735 cyc | **2.974 cyc** | <= 0.5 | **FAIL** (1.7x WORSE) |
| **T4 events** (`simnet_t4_census`) | 9 871 | **9 181** (-7 %) | <= 5 000 | **FAIL** |
| **ref-free WL(1,2) at own integer, settled** (`simnet_amb_integer --ref-free`) | 86.2 % | **56.9 %** | >= 88 % | **FAIL** (-29 pts) |
| ref-free EWL(2,5), same window | 99.2 % | 88.5 % | — | worse |
| slip detection (`simnet_slip_census`) | 2337 det / 75 resp / 74 silent | 2338 / 78 / 70 | unchanged | unchanged |

Run-end counters (invariant #4 — the wiring is live, not inert):

```
T4 erases 9181 cell(s) -> 15323 band(s) erased / 9181 kept (4224 kept over gate,
0 single-band cells skipped); detector refloats 1091 arc(s) -> 1091 band(s) freed
/ 1091 kept (1091 all-band fallbacks; 0 pins on RAW MW/GF evidence, the rest
lexical; 0 single-band arcs; 0 TOO LATE).
```

**Reading.** The wire's tropospheric channel improves exactly as the mechanism
predicts — 30 % off the ZWD settled truth rms, the largest single move any arm has
made on that number since the ZWD-in-block card itself, and the hourly series falls
1.95 / 1.27 / 0.84 / 0.68 / 0.92 / 0.88 against 1.98 / 1.31 / 0.96 / 0.58 / 1.05 /
1.67, i.e. the late-run climb is gone. Everything that touches the INTEGERS is
worse, and the arm cannot attribute that between its two knobs.

**Z.4 already can, and it points at the detector half.** 1 091 all-band fallbacks
on a bed with 2 486 truth slips means the fallback pinned a band that really moved
roughly once per two slips — and Z.3 priced one such event at **15x** the level
floor of freeing all three (0.498 -> 7.658 cyc, slant iono -0.089 -> -1.302 m).
1 091 of those is the size of the observed integer collapse; nothing in the T4 half
(which Z.3 measured at 9x-64x BETTER, on a population that is FA-dominated by
construction) predicts a loss. Note also that **0 of the 1 091 pins were chosen on
raw MW/GF evidence** — every one fell to the lexical fallback, because a whole-arc
detector implication on this bed is an LLI-on-every-band case whose primary-pair
step is not usable. The detector half therefore ran, on this bed, as "keep L1C
whatever happened to it", which is the worst version of the trade.

**Verdict.** `network.raim_erase_band_scope: min_keep_one` is VALIDATED at leg
scale (Z.3, three arcs, 9x / 29x / 64x with the slant-iono witness) and is the
knob to promote; `network.cycle_slip.refloat_band_scope: implicated` is KEEP_OFF
— falsified at leg scale on a genuine all-band slip and consistent with the storm
arm's integer collapse. **The decisive follow-up is a T4-ONLY storm arm**
(`raim_erase_band_scope: min_keep_one`, `refloat_band_scope` left at `all`), which
the one-long-arm budget of this session did not allow; the prediction it tests is
ZWD at or better than 0.835 m with the consumer and the lanes at or better than
zwdblk's 1.085 / 86.2 %. **A quiet-bed 24 h check is also OWED and was NOT run
here** — the quiet null leg (Z.2) shows the detector scope acting 20x in 61 epochs
on a bed with no slips at all, so the quiet arm is not a formality.

**Caveat on the reference numbers.** The gates' "ambient 1.65 cyc" was
pre-registered from an earlier read; re-measured with the same command on the same
reference run it is 1.735 (strict variant 1.608). The arm is graded against the
re-measured value, and the direction of every gate is the same either way.

### Z.5 Reproducing

```powershell
python scripts\ev_study3_legs.py make
python scripts\ev_study3_legs.py run z_null_on z_a1erase_on z_slip3_on   # <=3 at a time
python scripts\ev_study3_legs.py run z_a2erase_on z_a3erase_on z_slip1_on
python scripts\ev_study3_legs.py run z_slip3_off
python scripts\ev_study3_table.py --bands
```

Wall clock: 137-192 s per leg with 3 concurrent, 7 legs in ~8 min; Release build
135-244 s, Debug suite 51 s. Unit suite after the card: **1738 cases / 244 suites,
1721 passed, 0 failed, 17 skipped** (fixture-gated; `data/` is not bootstrapped in
this worktree) — the card adds 22 cases in `tests/test_band_scoped_response.cpp`.

**Caveats.** (a) One quiet sim bed, three arcs, 61-epoch legs; the floor is
measured to +45, not to convergence. (b) The all-band leg's fallback cost is
measured on ONE arc with a 10/8/6-cycle slip; the cost scales with the kept band's
own slip, so it is a magnitude, not a constant. (c) The ON arms' background (Z.2)
is cancelled by grading against an ON null, not eliminated — an A/B that used the
OFF null would be measuring both. (d) The T4 scope was exercised here only through
INJECTED erases on healthy arcs; a real attribution on a genuinely bad arc is the
storm-bed arm below.

### Z.7 The T4-only storm arm — the cure is falsified at storm scale, the mechanism is not

`runs/golden_sim_campaign_zwdblk_bscope_t4` = the §Z.6 arm with
`cycle_slip.refloat_band_scope` back at `all` and `raim_erase_band_scope:
min_keep_one` alone (config `_golden_sim_campaign_zwdblk_bscope_t4.json`, the
card exe, 720/720 epochs, exit 0, 1 256.6 s on a shared box).  Same recipe as
§Z.6, reference re-measured with the same commands.

| gate | zwdblk control | both knobs (§Z.6) | **T4 scope alone** | gate | verdict |
|---|---|---|---|---|---|
| ZWD settled truth rms (D0) | 1.188 m | 0.835 | **0.845 m** (−29 %) | ≤ 0.5 / ≤ 0.9 | **PARTIAL** |
| consumer settled p50 / rms | 1.084 / 2.389 | 2.891 / 3.008 | **2.173 / 2.644** | p50 ≤ 0.6 and rms ≤ 1.5 | **FAIL** (2.0× worse) |
| ambient chord-median \|e\| (n = 3000) | 1.66 cyc | 3.07 | **2.92 cyc** | ≤ 0.5 | **FAIL** |
| T4 events | 9 871 | 9 181 | **9 144** (−7 %) | ≤ 5 000 | **FAIL** |
| ref-free WL / EWL / L1C, settled | 86.2 / 99.2 / 44.0 % | 56.9 / 88.5 / 28.8 | **58.1 / 88.6 / 29.3 %** | WL ≥ 88 | **FAIL** (−28 pts) |
| CLK-R Σ\|μ\| drained / max \|A_c\| | 41.5 / 25.6 m | 28.1 / 11.2 | **27.1 / 10.7 m** | report | better |
| T3 consensus flags | 13 450 / 30 033 | 16 533 / 32 205 | 16 580 / 32 202 | report | worse |

Run-end clause: `T4 erases 9144 cell(s) → 15360 band(s) erased / 9144 kept
(4400 kept over gate — every band of the cell stood above k·σ̂₀ so the pin is
the least-BAD band); detector refloats 0 arc(s)` — the wiring is live and the
detector half is provably out of the picture.

**Reading.** The T4 scope alone reproduces the combined arm's collapse, so §Z.6's
attribution to the detector-side fallbacks was wrong: those fallbacks are a real
cost, but they are not what broke the integers.  Why the ×9–×63 of §Z.3 does not
transfer: on the quiet seed the erased arc was HEALTHY, so any pinned band carried
a correct level and the pin could only help.  On the storm bed 4 400 of 9 144
cells (48 %) had every band above the gate, the census puts the erased arcs at
1.9–2.2× the quiet arcs' |e_pre| (T4 finds real damage about half the time), and
the kept band is chosen by the post-fit RESIDUAL — the statistic §9.6(5) and
§Z.4(5) showed is blind to a level error absorbed into the (ρ, I) split (0
re-flags on a 7.7-cycle error).  Pinning by residual therefore pins a wrong level
about half the time on a bed whose ambient level error is already 1.7–2.9 cycles,
and each wrong pin is a permanent injection into the LOS — the §Z.3 all-band-slip
failure mode, arriving through the other population.

**Verdict.** Both knobs KEEP_OFF (ledger rows carry these numbers).  The
mechanism of §9 stands; this implementation of the cure does not.  The band to
keep must be chosen on evidence that can see a level error — the lane integers
(EWL-MW / WL consistency), i.e. the lane-preserving refloat of
`SPEC_lane_slip_response` §7 — and the next single-event test of any such cure
must run from a SLIPPED-bed seed (converged on the campaign fixture, ambient
cycles off), not the quiet one: a quiet-seed harness win certifies the mechanism,
not the cure.

