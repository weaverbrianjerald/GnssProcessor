# Card ORACLE — `oracle_relevel`, the sim-only CEILING of a level-preserving re-level — the mechanism is exact and the plateau does not move

**STATUS**

- **id:** Card ORACLE
- **topic:** slip
- **status:** closed
- **summary:** `oracle_relevel`, the sim-only CEILING of a level-preserving re-level — the mechanism is exact and the plateau does not move
- **dates in the text:** 2026-09-10
- **knobs / posture:** `network.cycle_slip.oracle_relevel`, `network.cycle_slip.oracle_truth_root`, `network.cycle_slip.oracle_noise_cyc`, `network.cycle_slip.oracle_false_alarm`, `network.cycle_slip.oracle_window_s`, `network.carry_invariant`, `network.phase_leveling`, `network.phase_leveling_q_amb`, `network.phase_leveling_var_floor_m2`, `network.raim_arc_persist_epochs` … (+2 more in the text)
- **hashes named in the text** (commit / exe / byte-cert): `3dd1284`
- **run dirs named in the text:** `runs/golden_sim_campaign`, `runs/simnet60_campaign_wu93`, `runs/golden_sim_campaign_plq7`, `runs/golden_sim_campaign_zwdblk_orafa`
- **source:** `docs/archive/SPEC_slip_qc_hardening.md` — 13, 13.1, 13.2, 13.3, 13.4, 13.5, 13.6, 13.7

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_slip_qc_hardening.md -->
## 13. Card ORACLE — `network.cycle_slip.oracle_relevel`, the sim-only CEILING of a level-preserving re-level (2026-09-10)

§12.7 item 1 left "a level-preserving re-entry response is still unbuilt and now unsourced", and the
truth-graded T4 census on the slipped 60-station bed (`docs/SPEC_amb_iono_split_overcount.md` §6.17b)
added the reason it matters: **the re-level VALUE is the defect** — an erase injects ≈ 2.5× the
bed's ambient DD error, walks back over ~20 min without reaching the floor, and 48 % of T4 events
fire within 15 min of a previous re-level on the same (station, signal), the first erase having made
it WORSE in 64 % of consecutive pairs.

Building the engineering answer (EWL / WL-MW integer estimation of the jump,
`docs/SPEC_lane_slip_response.md`) is expensive. This card measures its **CEILING** first.

### 13.1 The knobs

| knob | default | meaning |
|---|---|---|
| `network.cycle_slip.oracle_relevel` | `false` | arm the response |
| `network.cycle_slip.oracle_truth_root` | `""` | the fixture root holding `<STATION>/truth_slips.csv`; **missing root or missing station file = hard error** |
| `network.cycle_slip.oracle_noise_cyc` | `0.0` | 1-σ of a deterministic Gaussian added to each applied Σδ (the noise ladder; 0.0 is the exact ceiling) |
| `network.cycle_slip.oracle_false_alarm` | `"keep"` | `keep` = an oracle knows it was a false alarm and leaves the level alone (the ceiling); `free` = fall through to today's response, so the arm measures repair-only and keeps the false-alarm cost |
| `network.cycle_slip.oracle_window_s` | `90.0` | detector-latency bound (three 30 s epochs) |

Parsed in `src/core/config/GnssConfigLoadNetwork.cpp:4151-4190`; the arming banner and both
warn-and-DISARM interlocks at `:270-307`.

**It is FIELD-INERT by construction.** A field lineage has no truth root, so the loader hard-errors
when the knob is armed and the root does not exist, and setup hard-errors when any ROSTER station is
missing its `truth_slips.csv`. It is warn-and-DISARMED under `network.carry_invariant` (it shifts
`LocalCarry::value_abs`, which that vehicle does not own) and without `network.phase_leveling`
(there is no carried level to preserve). **Never promote it; never put it in a golden.**

### 13.2 Where it actuates, and where it deliberately does not

One helper (`networkSlipOracleHandle`, `include/slip/NetworkSlipOracle.hpp`) is called from each of
the four DETECTOR refloat actuation sites in `src/core/slip/NetworkSlipQc.cpp`: `refloat_signal`
(the MW confident-wide-lane drain and the RINEX-LLI drain both funnel through it), `refloat_pair`
(the IF-phase triple-difference driver, both bands), and `networkSlipDrainWindupRestart`
(whole-satellite, every signal). When it returns `Repaired` or `Kept` the caller skips every freeing
action for that arc — no `free_ambs` latch, no `carry_slipped_ambs` funnel insert, no
pending-reseed mark, no bridge-pend void, no erase — but still emits the site's event row with
action `oracle_repair` / `oracle_keep`, so the trace population stays comparable to the control's.

`networkSlipDrainGapReentry` (§12) and the T4 RAIM-attribution erase in
`src/core/filter/NetworkRaim.cpp` are deliberately **left alone**: the census question is what a
perfect DETECTOR response buys with every other mechanism held fixed.

### 13.3 The window — a detector-latency bound floored by the carry's BIRTH

    ( max(last_applied_s, carry.birth_s, now_s − window_s),  now_s ]

lower bound EXCLUSIVE, upper INCLUSIVE. Each term earns its place, and two of them were paid for:

- **`now_s − window_s`** is the load-bearing one. Detectors fire 1–3 epochs after the slip (MW needs
  its window to fill, IF-TD needs the triple difference), which is the 90 s the truth census calls
  DETECTED.
- ⚠ **`LocalCarry::last_seen_s` is NOT usable** even though it looks like the natural "already
  absorbed" mark. It is the PREVIOUS epoch's stash time, and a zero-q carried level is not re-set by
  an intervening solve (the stiff prior barely moves against one epoch of phase), so the jump is
  still un-absorbed when a late detector arrives. Using it made the normal population read as false
  alarms — **701 "false-alarm kept" against 88 repaired on a 60-epoch smoke** — and the ceiling that
  arm would have reported was a badly biased lower bound.
- **`LocalCarry::birth_s`** (new; stamped once at creation, preserved across re-stashes, alongside
  the existing loop-epoch-index `birth_ep`, which is NOT usable here) is the hard floor. A T4 erase
  deletes the entry and the next harvest re-creates it from a fresh solve whose level ALREADY
  contains any earlier jump, so a truth slip stamped before the entry's birth is absorbed by
  construction and re-applying it would inject the very error this card exists to remove. It is
  deliberately **not** snapshotted, so a warm-resume-restored entry reads 0 and the window falls
  back to the latency bound — the conservative direction for a diagnostic.
- **`ArcState::last_applied_s`** guards the same arc being handled twice before a re-harvest (two
  detectors in one epoch, or on consecutive epochs).

A window holding no truth slip is a genuine FALSE ALARM and is answered by the `false_alarm` policy.

### 13.4 Counters (standing invariant #4)

Run-end clause, `src/core/filter/NetworkTeardown.cpp:2532-2560`. Closure is asserted:
`seen == applied + fa_kept + fa_freed + no_carry + no_lambda`. Beyond the outcome counts it reports
**which of the three lower-bound terms was BINDING** (`bound_window` / `bound_birth` /
`bound_applied`) — without that the only readable outcome is "N repaired, M false alarm", which
cannot separate a window that is too narrow from a population that genuinely did not jump, and the
two have opposite fixes.

E0 arm's own clause: `13485 detector actuation(s) on carried arcs, 1161 repaired (sum |delta|
62424.0 cyc, max 125.0, 0 multi-slip), 9175 false-alarm kept, 0 false-alarm freed, 3149 fell through
with no carry, 0 with no wavelength; … lower bound set by the window on 6968, by the carry's birth
on 1041, by an earlier repair on 2327`. On the 60-epoch smoke, widening the window to 1e5 s moved
`applied` 88 → 90, so on this bed **late detection is not what the false-alarm population is made
of**.

### 13.5 The four arms — the mechanism is EXACT and the plateau does not move

`runs/golden_sim_campaign{,_zwdblk}{,_oracle}{,_oracle_e1}` against
`runs/simnet60_campaign_wu93`, 720 epochs / 6 h / 60 stations, graded with
`scripts/simnet_oracle_grade.py`, `simnet_grade.py`, `simnet_amb_integer.py --ref-free`,
`simnet_t4_census.py`, `simnet_slip_census.py`.

**Mechanism** (`scratchpad/oracle/repair_effect_A.txt`): 1 161 repairs; |δ| applied p50 50 cyc;
detection latency 0 for 1 124 of 1 161 (so a pre/post slope test is uninformative — the ratio is the
instrument); **unrepaired fraction |e(t₀) − e(t₀−30)| / |δ| p50 0.005** (0.239 cyc), |e(t₀)|/|δ|
**0.070**, |e(t₀+150 s)|/|δ| **0.078**. The jump is gone.

**Slip-side census** — of the 2 452 truth slips: **47.3 % REPAIRED**, **25.6 % detected but KEPT**
(the window/keying missed them — an open defect that biases the ceiling DOWNWARD), **21.2 % FREED**
with no carry entry (60 % of those within 3 epochs of a T4 erase that had deleted it), **5.9 %
SILENT** (no detector row at all). Of 2 697 `oracle_keep` rows: 603 partner-band of a repair, 430
the same slip already repaired earlier (`last_applied_s`, correct), **0** carry-re-born-by-T4
(`birth_s` floor, by design), 69 standalone with a truth slip within ±600 s, and only **8 genuine
data-domain detector false alarms**.

**Results:**

| settled | E0 (`golden_sim_campaign`) | **E0 + ORACLE** | E0 + ORACLE + E1 | zwdblk | **zwdblk + ORACLE** | zwdblk + ORACLE + E1 |
|---|---|---|---|---|---|---|
| ZWD truth rms (m) — gate ≤ 0.5 | 2.098 | **1.779** | 1.819 | 1.188 | 1.104 | 1.239 |
| consumer 3D p50 / rms (m) | 1.751 / 1.799 | 1.730 / 1.896 | **3.781 / 4.909** | 1.084 / 2.389 | 2.234 / 3.493 | 3.334 / 4.294 |
| ambient per-band DD \|e\| (cyc) | 1.65 | **2.43** | **6.09** | 1.65 | 1.89 | 5.66 |
| T4 `attrib_arc` events | 7 656 | **13 562 (+77 %)** | 0 (E1) | 9 871 | 16 095 (+63 %) | 0 (E1) |
| detector re-levels | 4 545 | **687 (−85 %)** | 275 | 4 545 | 717 | 275 |
| ref-free WL(1,2) at own integer | 81.5 % | 81.5 % | **35.4 %** | 86.2 % | 85.9 % | 39.7 % |
| ref-free EWL(2,5) | 99.0 % | 98.6 % | 75.5 % | 99.2 % | 99.0 % | 74.7 % |
| sat-clock contrast rms (m) | 8.050 | 7.777 | 3.159 | 11.791 | 11.806 | 3.243 |
| sat code-bias residual rms (m) | 0.608 | 0.537 | 0.362 | 0.678 | 0.638 | 0.356 |
| oracle repairs applied | — | 1 161 | **1 549** | — | 1 142 | 1 549 |
| … `no_carry` fall-throughs | — | 3 149 | **1 396** | — | 3 285 | 1 396 |

Slip DETECTION is flat at **94.0 %** on every arm (`simnet_slip_census.py`, 2 486 truth slips) —
nothing in this table is a detection story.  ⚠ **The E1 pair is the sharpest reading.** Starving the
T4 erase recovers exactly what the oracle was losing — repairs 1 161 → 1 549 on an *identical*
13 485-actuation detector population, `no_carry` −3 149 → 1 396, `bound_birth` 1 041 → 273, i.e. the
21 % FREED class of §13.5 WAS T4 deleting the carry out from under the repair — and the result is
WORSE than E1 alone on every channel E1 improved (ZWD 0.933 → 1.819, consumer p50 2.559 → 3.781, WL
65.4 → 35.4 %).  A better ceiling instrument, a worse product: the two mechanisms are
anti-synergistic, and the ambient DD error rises to 6.09 cyc with nothing left to disturb the frozen
split.

**Verdict: FAIL on every pre-registered gate; the card is a measured CEILING, not a fix.**
Detector re-levels fell **85 %** while T4 grew **77 %**: arcs restored to their true integer are
rejected by the corrupted MAJORITY. The per-band DD errors are equal in CYCLES while the lanes are
clean (E0 ref-free L1C 0.896 / L2W 1.086 / L5Q 0.980 / L7Q 0.816 cyc against EWL 0.032, WL 0.141),
which is a (range, iono) split rather than an ambiguity error — solving `δρ + γ_b δI = ε λ_b` on the
L1/L2 pair gives (δρ, δI) = (0.1069, 0.0834)·ε m and predicts L5's own error to 0.6 %. **The station's
zero-q levels are frozen around a self-consistent mis-split of range against ionosphere, and a
single-arc level preservation, however perfect, cannot undo a station-wide one.** Full reading and
the T4 census it rests on: `docs/SPEC_amb_iono_split_overcount.md` §6.17b / §6.17e, §7 items 29 / 33.

Ledger: `network.cycle_slip.oracle_relevel` (+ `_truth_root`, `_noise_cyc`, `_false_alarm`,
`_window_s`) = **KEEP_OFF**, scope "SIM-ONLY DIAGNOSTIC — a truth-fed response; never a posture".
Unit contract `tests/test_slip_oracle.cpp` (19 cases): window arithmetic including all three lower
bounds, the birth floor, multi-slip summation, the unknown-signal fall-through, both false-alarm
policies, the noise ladder's determinism, closure of the counters, and the loader's two
warn-and-DISARM interlocks plus both hard errors.

### 13.6 Addendum, 2026-09-10 afternoon — the two levers this card named, both measured

§13.5's verdict ("a single-arc level preservation cannot undo a station-wide (ρ, I) mis-split")
named two successors. Both ran the same afternoon, config-only, on the same bed; full record
`docs/SPEC_amb_iono_split_overcount.md` §6.18a–§6.18c and §7 items 34 / 35 / 37.

**1. The levels' MEMORY — FALSIFIED ×4.** `network.phase_leveling_q_amb` 1e-7 / 1e-6 m²/s
(`runs/golden_sim_campaign_plq7` / `_plq6`) and `network.phase_leveling_var_floor_m2` 1e-4 / 1e-3 m²
(`_plvf4` / `_plvf3`), each a single knob off E0. The q half HALVES the T4 churn (arc erases
22 968 → 12 348 / 12 477, −46 % — so the erase tier *is* sensitive to level stiffness) and still
leaves ZWD truth rms at 1.93 / 1.96 m against E0's 2.098, with the truth |residual| MEDIAN WORSE
(p50 0.123 → 0.256 / 0.450 m) and the consumer +57 % (settled p50 1.751 → 2.76 / 2.75). The
var-floor half is NEUTRAL on all four channels (ZWD 2.089 / 2.060, consumer 1.85 / 1.89, T4
22 524 / 21 429). Both are non-monotone in their own value across a decade. ⇒ softening the level
lets it MOVE but gives it nothing to move TOWARD while arcs keep restarting; the freeze is not the
levels' stiffness. Both knobs now carry KEEP_OFF ledger rows.

**2. Starving the erases — the other half of §13.5's "T4 was the only thing re-randomising the
split", now graded ON THE WIRE.** The `network.raim_arc_persist_epochs` ladder K = 1 / 10 / 40 / 1e6
on the in-block posture improves every producer channel monotonically (ZWD truth rms 1.188 → 0.818 m,
satellite-clock datum-free contrast 11.79 → 2.66 m, −77 %) while the CONSUMER degrades monotonically
(settled p50 1.085 → 2.296 → 2.727 m). The wire says why: within-tenure WANDER rises with K (Galileo
L1C 0.249 → 0.793 cyc, GPS L1C 0.507 → 0.944) while per-epoch jitter is flat (~0.07 cyc), step size
is flat and **handover counts are identical in every arm** — K changes the CORRELATION of the steps,
not their number or size — and the motion is a 2-parameter (range, iono) walk at R² 0.993–0.996 whose
SHAPE is invariant in K. Arc-level-removed, the clock channel is flat at 5–12 cm across the ladder
(the 4.4× clock gain is ALL arc LEVEL) while the arc-removed PHASE-clock degrades on every Galileo
band. Within each run the consumer's |error|-vs-arc-age correlation FLIPS sign (−0.335 at K = 1 to
+0.20…+0.42 at K ≥ 10). ⇒ **the T4 erases are the crude cure for the anchors' walk** — they chop a
continuous walk into steps a consumer flushes by resetting — so neither a level-preserving response,
nor T4-off, nor a K is the fix. The named build is now the (ρ, I) split's I half carried in-block
(`network.carry_iono_in_block`, branch `worktree-ionoblk`), under the B9/Q1 quiet-bed protocol.

⚠ **Two riders for anyone re-using §13.5's numbers.** (a) `sat-clock contrast` is not a
consumer-relevant metric by itself: it improves 4.4× across the K ladder while the consumer gets 2.5×
worse — grade a consumer-facing slip response on the per-satellite PHASE-clock (C + P). (b)
`scripts/simnet_wire_integer_consistency.py` treated `truth_ambiguities.csv` as a STATIC integer, so
on a slipped bed (2 486 slip rows touching 32.3 % of the keys) its integer-dependent steps were
contaminated; it now accumulates the truth slips through `simnet_amb_integer.load_truth` / `n_at`.
Everything quoted from that grader in §13.5 and here is integer-free.

### 13.7 The FALSE-ALARM-ONLY corner — `oracle_true_slip`, and what it proves (2026-09-10 evening)

§13.5 measured the ceiling of a level-preserving response to a TRUE slip.  It left the other half of
the 2×2 unmeasured: what the detectors' FALSE alarms are costing.  On this bed that population is not
a rounding error.

**The measurement first (`scripts/simnet_slip_census.py`, in-block arm, post-Card-R goldens).**  Of the
bed's 2 486 truth slips, **2 337 (94.0 %) are DETECTED** — MW 2 006, IF-TD 331, T4 59, raim 16 — and
**74 (3.0 %) are SILENT**, mostly ≥ 90-cycle jumps at el ≈ 10°.  But of **4 708 slip-tier events,
2 371 (50 %) have NO truth slip anywhere in the window: MW 1 132, IF-TD 1 239** (ignition candidates
and two refsat resets included).  That is ≈ **6.4 false re-levels per station-hour against 6.8 true
slips per station-hour**.  After Card R fixed the IF-TD reference gate on the QUIET bed (§11 / SPEC_amb_iono
§6.14.6), BOTH detectors still fire as often on nothing as on something on the SLIPPED bed — consistent
with the fixture's phase noise being ~5× the field's while the MW thresholds are field-empirical
(`feedback_mw_thresholds_empirical`; a fixed PHYSICAL threshold does not port between beds).

⚠ **Two counting bases, never to be mixed in a rate.**  The census counts EVENTS per
(station, satellite) — ~2 300 false per 6 h on this bed.  The oracle's counters below count per-signal
ACTUATIONS — 6 804.  The earlier "13 485 actuations" figure was the per-signal OFFER count, a third
basis again.

**The knob (commit `3dd1284`).**  `network.cycle_slip.oracle_true_slip` = `"repair"` (default —
§13.5's ceiling, shift the level by the true Σδ·λ) | `"free"` (decline; the caller runs today's
ordinary free/re-level for that arc).  It is independent of the existing
`network.cycle_slip.oracle_false_alarm` = `"keep"` | `"free"`, so the two span a 2×2 whose fourth
corner (both `"free"`) is a pure no-op — the loader WARNs rather than refuses, because that corner is
the byte-identity control for the hook itself.  The false-alarm keeps are now also split **per tier**
in the run-end counters (invariant #4), which is what makes the arm below attributable.

**The arm — `runs/golden_sim_campaign_zwdblk_orafa`** (`true_slip: free` + `false_alarm: keep` on the
in-block posture).  The ONLY difference from the control is that actuations with no truth slip in the
window keep their level; every real slip pays the shipped response.  **6 804 false alarms kept**
(MW 4 156 / IF-TD 2 131 / LLI 517), 3 409 true slips freed as today.

| | in-block control | false alarms KEPT |
|---|---|---|
| ZWD truth rms | 1.188 m | 1.066 (−10 %) |
| ZWD truth p50 | 0.048 | **0.104** |
| consumer settled p50 | 1.085 m | **2.92** |
| consumer settled rms | 2.389 | **4.46 (2.7×)** |
| T4 arc erases | 29 613 | **48 078 (+62 %)** |

**FAIL — and decisive in the direction opposite to the one it was built to test.**  NOT re-levelling an
arc that had no slip makes the consumer 2.7× worse and forces 62 % more T4 erases.  ⇒ **the
false-alarm re-levels are a RESYNC mechanism**: on this bed their function is not slip repair, it is
dragging a drifting arc back into its station's current (walking) frame, and the detectors' 50 %
false-alarm rate is doing useful work by accident.

**What this closes.**  "Tighten the detector thresholds on this bed" is dead as a card — a better
detector would remove the resyncs and leave the walk.  Taken with §13.6's two ladders and
SPEC_amb_iono §6.17b (T4 mostly re-erases the damage its own previous re-level created), all three
accuracy-of-response levers are now measured and none of them is what the consumer is buying: the
response's ACCURACY is not the lever, the frame's OBSERVABILITY is (SPEC_amb_iono §6.19a / §7 item 40,
and the receiver-clock card of §7 item 42).

`oracle_true_slip` is a **sim-only diagnostic** — it reads `truth_slips.csv` — and carries a KEEP_OFF
ledger row for the same reason `oracle_relevel` does.  ⚠ It is also the reason §13.5's "26 % detected
but KEPT" class must be re-read: some of that class is this one.
