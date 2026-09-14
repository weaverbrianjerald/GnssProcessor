# QPR 5–8 — items 5–8: the level contract, the half-epoch back-off ritual, the SH health-gate release policy, and the per-LOS ionosphere representation

**STATUS**

- **id:** QPR 5–8
- **topic:** queues
- **status:** open
- **summary:** items 5–8: the level contract, the half-epoch back-off ritual, the SH health-gate release policy, and the per-LOS ionosphere representation
- **dates in the text:** 2026-08-14 → 2026-08-21
- **knobs / posture:** `network.level_authority_csgf`, `network.raim_withhold_adapt_escalate`, `network.estimate_iono_sh`, `network.carry_iono`
- **hashes named in the text** (commit / exe / byte-cert): `65ba362`, `a4b3eee9`
- **run dirs named in the text:** `runs/t3iono`, `runs/q8a_ctl`, `runs/q8a_sh`, `runs/camp60_refprod`, `runs/camp60_f4`
- **source:** `docs/archive/QUEUE_principled_replacements.md` — 5, 6, 7, 8

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

