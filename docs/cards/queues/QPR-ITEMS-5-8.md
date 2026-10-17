# QPR 5–8: level contract, half-epoch back-off, SH-gate release, per-LOS iono

**STATUS**

- **id:** QPR 5–8
- **topic:** queues
- **status:** superseded
- **summary:** Item 8's ionosphere error PERSISTS with SH on and 8(b) is FALSIFIED; items 5, 7 and 8 SUPERSEDED or moot under the re-founding (GF pin at broadcast T_GD, no SH, float vehicle); item 6 built 2026-10-16 (start-offset seek on a millisecond grid, leap table fixed).
- **dates in the text:** 2026-08-14 → 2026-10-16
- **knobs / posture:** `network.level_authority_csgf`, `network.raim_withhold_adapt_escalate`, `network.estimate_iono_sh`, `network.carry_iono`, `network.start_offset_hours`
- **hashes named in the text** (commit / exe / byte-cert): `65ba362`, `a4b3eee9`, `e7a65b2`
- **run dirs named in the text:** `runs/t3iono`, `runs/q8a_ctl`, `runs/q8a_sh`, `runs/camp60_refprod`, `runs/camp60_f4`, `D:/GnssProcessorSupport/runs_nom/_rap_seekb_*` and `_rap_seekn_*`
- **source:** `docs/archive/QUEUE_principled_replacements.md`, items 5, 6, 7, 8

**Outcome.** Item 8(a) found the per-LOS ionosphere error PERSISTS with SH on (0.7061 to 0.6691 m); 8(b)'s carry family FALSIFIED at all four arms; item 5's LV-6 census failed both gates; item 7's A4 escalation passed its mechanism but failed the σ̂₀ gate (2.42). The [re-founding](../../refound/NETWORK_REFOUNDING_2026-09-14.md) replaced all three: the level is the per-signal GF pin at broadcast T_GD ([RF-0c](../../refound/RF-0c_gf_pin_2026-09-15.md)), SH left the producer, the float vehicle has no zero-q level, and the goldens withhold no epochs ([EXL](../../refound/EXL_exact_update_lah_promotion_2026-10-01.md)). Item 6, the start-offset seek, was built on 2026-10-16 (branch `fix/seek`; result section at the end of this file): the seek compares on a millisecond grid, the half-epoch back-off keeps its epoch, and a bare offset lands on the epoch it names. Status reviewed 2026-10-16.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: QUEUE_principled_replacements.md -->
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
leveling carry) — **DERIVED AND CLOSED UNBUILT 2026-08-21,
`docs/SPEC_o2_carry_invariant_accumulator.md`; never ranked, and it closes the 8-family.  Note
its §1: BOTH ingredients of the mechanism named in this paragraph (the zero-q level AND the ZWD
carry) are absent on the carry_invariant posture — the ZWD carry rides `carry_warm` and
`phase_leveling` is absent from the whole `_ft63_*`/`_ext63_*` family — so the plateau as stated
has no counterpart there and the successor question was never the one filed.** The CS-GF kernel
stays as instrumentation; quiet-twin CS-GF arm dropped as moot; the plateau keeps its owner here.

---

---


## 2026-10-16: item 6 built (branch `fix/seek`, commit `e7a65b2`)

**Question.** Can the start-offset seek select the epoch an offset names without the half-epoch back-off, while every offset that carries the back-off keeps selecting the epoch it selects today?

**Defect.** The seek compared the elapsed span between two floating GPS second counts with the offset. The calendar to GPS conversion leaves up to 2.0e-5 s of residue on an epoch time (2,880 epochs of DOY 129 at 30 s: 1,408 negative), so an offset that lies on an epoch started the run on that epoch or one epoch later, by the sign of the residue. From a 00:00:00 anchor the bare hours 1, 4, 7, 10, 13, 16, 19 and 22 landed one epoch late and the other 15 landed on the hour. An offset a half epoch off the grid has no epoch within 15 s of it, so the back-off hid the residue.

**Rule.** `TimeUtils::offsetReached` compares on a whole-millisecond grid: an epoch has reached the offset when its elapsed time and the offset, each rounded to the nearest millisecond, satisfy elapsed >= offset. The residue (2e-5 s) is far below the grid and the grid is below any real epoch schedule. One function serves the four sites that had their own copy of the comparison: the producer fast-forward (`networkFastForward`), the start of the code-bias admission census, the age reference of the warm-resume gate, and the consumer replay. The replay's hour banding reads the same `TimeUtils::elapsedMs`. No knob: the seek has one correct behaviour, and an offset moves its first epoch only when it lies within half a millisecond of an epoch, where the old result was the residue's sign.

**Same first epoch with the back-off.** Unit tests (`tests/test_epoch_seek.cpp`) build a day of 30 s epochs with the production conversion, from a 00:00:00 and a 00:00:30 anchor:

- every on-grid offset (k times 30 s, k = 1 to 2,879) starts on epoch k; the old comparison misses 8 of the 23 bare hours (the test counts them, so it fails if the residue ever leaves the conversion);
- every half-epoch back-off offset (k times 30 s minus 15 s, every epoch of the day) selects the epoch the old comparison selects;
- the 16 half-epoch values written in the shipped configs select the old epoch; the 9 on-grid values select the epoch they name;
- `networkFastForward` skips 120 epochs and processes the 121st for a 1 h offset (the old comparison: the 122nd).

Release smoke, 10 epochs (`max_duration_s` 300, 11 epochs processed), one control exe built from the branch base `4318fe3` and one from the branch, same arm file, `RAPID_RUNS_ROOT` `D:/GnssProcessorSupport/runs_nom` (`_rap_seekb_*` control, `_rap_seekn_*` branch):

| arm | `start_offset_hours` | first epoch tow, control | first epoch tow, branch | `products.txt` |
|---|---|---|---|---|
| field golden | none | 259200 | 259200 | byte-identical |
| field golden | 5.9958333 (back-off) | 280800 | 280800 | byte-identical |
| sim golden | 0.504167 (back-off) | 261060 | 261060 | byte-identical |
| field golden | 1.0 (bare hour) | 262830 | 262800 | differs (control one epoch late) |
| sim golden | 1.0 (bare hour) | 262860 | 262830 | differs (control one epoch late) |

The datum station of the field bed starts at 00:00:00 and that of the sim bed at 00:00:30, so a 1 h offset names tow 262800 and 262830. The 2 h golden arms (byte-identity of the products and the traces) belong to the check stage.

**Shipped configs.** 107 files under `config/` set the offset. Replaying each on its datum station's real epoch schedule with an IEEE-double replica of the conversion (102 replayed; 5 have no RINEX on disk and carry half-epoch values): 93 select the same first epoch (75 carry the half-epoch back-off, 18 are on-grid offsets the residue happened to land correctly) and 9 select the epoch one step earlier, the epoch they name: `config/test_suite/` `_dcb305_cold`, `_dcb305_resume`, `_rs305_B2off`, `_rs305_B2on` (0.0916667 h), `_dcmp_50sta_10ep_snap_resume` (0.0833333 h), `_gg305_cold`, `_gg305_resume` (1.0083333 h), `_night305_resume`, `_night50_resume` (6.0083333 h). Their offsets were bare, so the old exe started them one epoch late. The files are not edited.

**Leap table (bundled).** Two defects, neither reached by a 2024 run (GPS-UTC is 18 s there and unchanged):

- the lookup keyed by a GPS-scale JD compared it with UTC dates, so each new offset applied from 0 h on the GPS clock instead of when UTC reaches the date, early by the new offset (18 s at 2017-01-01). `leapSecondsGpsUtc` now moves the GPS instant onto UTC first; the calendar UTC to GPS conversion, whose input is already UTC, uses the new `leapSecondsAtUtcJd`;
- the 2012-07-01 row carried the JD of 2012-01-01 (2455927.5), so GPS-UTC read 16 s for six months it was 15 s. Corrected to 2456109.5. Found by `LeapSecondScaleTest`, which walks the seconds around every insertion (UTC to GPS to UTC returns the calendar time); every other row was checked against its stated date.

**Can the back-off be retired?** Yes, for any exe containing this change: an offset on an epoch starts on that epoch, and an offset between epochs starts on the next one. The back-off is still written in the `CLAUDE.md` network-run rule, `scripts/event_leg.py` and `scripts/chain_make_leg.py` (the `+15 s` of the resume offset), the shipped configs and archived arms, and an exe built before this commit still needs it. Removing it from those is left to the author.

**Not changed.** The `processing.max_duration_s` cutoff compares the same way (`now - anchor >= max_duration`) and has the same class of defect: replaying the 2024-129 epochs, a 7,200 s window runs 240 or 241 epochs depending on the anchor (877 of 2,638 anchors run the extra one), a 300 s window 10 or 11 (1,589 of 2,868 anchors; the smoke arms above all processed 11). Moving it would change the last epoch of existing arms, so it is filed in [OPEN_ITEMS](../../status/OPEN_ITEMS.md).

**Verdict.** Built; the back-off keeps its epoch in every smoke and unit check; bare offsets land on the epoch they name. Standing: `TimeUtils::offsetReached` (`include/math/TimeUtils.hpp`).
