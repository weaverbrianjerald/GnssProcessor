# 0910 morning: truth censuses, two falsified cards, field phase-bias steps

**STATUS**

- **id:** 0910 morning
- **topic:** ionosphere
- **status:** closed
- **summary:** Oracle slip repair FAILS every gate and PB-gauge levelling is FALSIFIED (KEEP_OFF); the emitted phase bias carries an arbitrary receiver's float, and field phase biases step by metres.
- **dates in the text:** 2026-09-10
- **knobs / posture:** `network.phase_datum_prioritized`, `network.cycle_slip.oracle_relevel`, `network.raim_arc_persist_epochs`, `network.emit_pb_gauge_level`, `network.emit_pb_gauge_mode`, `network.ambiguity_resolution`
- **hashes named in the text** (commit / exe / byte-cert): `319a6aed`, `00d5fdf`, `18aa2854b2575658`, `9b59a2c`
- **run dirs named in the text:** `runs/simnet60_campaign_wu93`, `runs/fldf3_on2_zwdblk`, `runs/fldf3_on2`, `runs/fldf3_on2_ctl_sb`, `runs/golden_sim_campaign`, `runs/golden_sim_campaign_zwdblk`, `runs/golden_sim_consumer_ar_a`, `runs/golden_sim_pbgauge`
- **source:** `docs/archive/SPEC_amb_iono_split_overcount.md`, sections 6.17, 6.17a, 6.17b, 6.17c, 6.17d, 6.17e, 6.17f, 6.17g, 6.17h

**Outcome.** The morning asked what holds the slipped sim bed's metre-class ZWD plateau and why the wire carries no integers. T4 mostly re-erases arcs its own earlier re-level damaged (the first erase made 64.4 % of T4 chains worse), which supersedes bounded T4 identification (item 22). Exact oracle repair FAILED every pre-registered gate, worse still with T4 starved; PB-gauge levelling met its wire gate on 0 of 12 channels; the consumer WL→NL cascade is DEAD ON ARRIVAL. Both in-flight items closed that day: emission continuity FALSIFIED as a class, producer AR NO-GO for Phase 3 ([0910 second half](../ionosphere/AMB-IONO-0910-SECOND-HALF.md)).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_amb_iono_split_overcount.md -->
## §6.17 The 2026-09-10 morning — the same-build control, three truth censuses, two falsified cards, and the field's phase-bias steps

Everything below is one morning's work on the two beds §6.16 opened: the slipped 60-station sim
campaign (`runs/simnet60_campaign_wu93`, 2 452 truth slips / 6 h) and the DOY-129 63-station field
chain. Nothing in §6.16 is edited. Seven results, in the order they landed; (a) closes an owed
control, (b)–(d) are truth censuses that redirect two open items, (e)–(f) are two falsified cards,
(g) is where the field actually is, (h) is what is in flight.

---

### 6.17a The F2 same-build control — the A/B is same-build by proof

§6.16.7d's Arm F2 compared `runs/fldf3_on2_zwdblk` (HEAD exe `gnss_processor_zwdblk_main.exe`,
319a6aed…) against `runs/fldf3_on2`, whose exe came from the `00d5fdf` worktree. The OFF path was
byte-certified by smokes, but the 12 h control was not same-build, and §7 item 24 recorded that as
owed.

`config/test_suite/_fldf3_on2_ctl_sb.json` re-runs `_fldf3_on2` unchanged on the HEAD exe
(63 sta, 12 h, ~7 min wall). The result is **byte-identical to `runs/fldf3_on2`**:

| artifact | `runs/fldf3_on2` | `runs/fldf3_on2_ctl_sb` |
|---|---|---|
| `products.txt` sha256 | `18aa2854b2575658…` | `18aa2854b2575658…` |
| `logs/closed_loop_trace.csv` | — | identical |
| `logs/network_zwd.csv` | — | identical |
| `logs/network_flag_rates.csv` | — | identical |

⇒ **the F2 A/B is same-build by proof**, not by smoke inference, and every number in §6.16.7d stands
as a single-knob-pair contrast. §7 item 24's last open clause is closed.

---

### 6.17b The T4 erase census, truth-graded — T4 re-erases the damage its own previous re-level created

`scripts/simnet_t4_census.py` (`--selftest` 6/6) joins `logs/network_events.csv`'s `T4_attrib /
attrib_arc` rows to the DD-fraction trace and to `truth_slips.csv`, on `runs/golden_sim_campaign`
(E0) and `runs/golden_sim_campaign_zwdblk`. Both the as-traced and the ref-free DD statistic were
run (§6.12); the two agree on every share, so the as-traced numbers are quoted.

**⚠ METHOD — the slipped bed has no clean floor, so calibrate against SAME-EPOCH quiet arcs.** The
bed's ambient per-band DD error is **1.699 cyc** (median over a 3 000-arc control of arcs with no
re-level within ±600 s; 1.729 on the strict variant that admits no T0/T1/T4 row at all; 1.653 as the
epoch-median reference used in the age profile). An absolute 0.5-cyc "OFF" cut therefore classifies
**83 %** of untouched QUIET arcs as OFF. Every share below is a ratio to that same-epoch control;
a raw cut on this bed measures the bed, not the event.

**Population.** 7 656 T4 events = **22 968 arc erases** (the events CSV is per (station, satellite),
signal-blind; each event erases 3 signals). The DD trace covers 42 of 60 stations, and 2 043 events
(26.7 %) have ≥ 3 chords on ≥ 1 signal at t₀−30 s, i.e. are *measurable*; the event rate is unbiased
across the covered/uncovered split. Truth-slip proximity: **1.4 %** of events sit within 90 s of a
truth slip on that (station, satellite), **4.9 %** within 10 min.

**The discriminator — is the erased arc actually damaged?**

| population | n | med \|e_pre\| | ratio to same-epoch control | > 2× | control quantile | **below the control MEDIAN** |
|---|---|---|---|---|---|---|
| T4-erased arcs | 2 043 | **4.546** | **2.59** | 55.7 % | 0.781 | **29.0 %** |
| detector refloats | 993 | 2.585 | 1.58 | 41.3 % | 0.640 | 38.1 % |
| quiet arcs (control) | 3 000 | 1.699 | 1.00 | — | 0.50 | — |

So T4 is **not** firing at random — the median erased arc is 2.6× the ambient — but **29 % of its
erases land on arcs the same epoch's untouched population cannot distinguish from healthy** (H-B
false alarms).

**It is not fresh damage.** Censoring at the arc's own previous re-level, the erased arc was already
at that level 10 min earlier (med \|e\| then 3.048 vs 3.176 at t₀; grew in 44.5 % of cases — a coin
flip) and 20 min earlier (2.656 vs 2.574, grew 44.9 %). T4 fires on a **chronically** bad arc.

**H-C IS THE MECHANISM — the re-level VALUE is worse than what it replaced.**

| chain | n | med gap | med \|e\| BEFORE 1st erase | AFTER 1st erase | BEFORE 2nd erase | 1st erase made it WORSE |
|---|---|---|---|---|---|---|
| T4 → T4 on one (S,s) | 990 | **210 s** | 8.345 | **13.197** | 14.082 | **64.4 %** |
| any re-level → any | 1 811 | 120 s | 4.594 | 6.082 | 5.288 | 56.8 % |

**48.4 %** of all T4 events fire within 15 min of a previous re-level on the same (station, signal)
(cross-tab: 1 161 < 2 min + 846 at 2–5 min + 1 701 at 5–15 min of 7 656). Improvement 10 epochs
later is a coin flip for the OFF class (**52.3 %**) and essentially never happens when the arc was
FINE (**1.7 %**, n = 59) or AMBIGUOUS (22.1 %).

**The re-level profile, on a FIXED cohort** (only events with no next re-level anywhere in the
window — the censored profile drops re-hit arcs as k grows, which is survivorship bias):

| k (epochs since the erase) | 0 | 1 | 5 | 10 | 20 | 40 | ambient |
|---|---|---|---|---|---|---|---|
| med \|e\| (cyc) | 3.031 | **4.192** | 3.195 | 2.877 | 1.885 | **2.059** | **1.653** |

An erase injects ≈ 2.5× ambient at k = 1 and walks back over ~20 min to a level that **never reaches
the floor** (k=1 → k=40 ratio 0.50, against 1/√40 = 0.16 for full averaging and 1.00 for frozen).

**The zwdblk twin says the same thing with the ZWD pinned.** `runs/golden_sim_campaign_zwdblk`:
9 871 T4 events (**+29 %**), T4 → T4 med gap 120 s, and the chain's injected error is **half** —
3.529 → 5.429 → 4.529 against E0's 8.345 → 13.197 → 14.082. Pinning the wet delay removes half of
what a fresh re-level dumps, and buys more erases.

**⇒ VERDICT: BOUNDED IDENTIFICATION IS THE WRONG LEVER.** §7 item 22 opened on "T4 erases good arcs;
bound the identification with a separability test". The census says T4 mostly confirms genuinely-off
arcs — whose error **its own previous re-level created**. The disease is the re-level VALUE
inheriting the station's corrupted per-epoch states (ZWD / iono / clock), i.e. a station-wide
positive-feedback loop, not the identification. Item 22 is superseded (§7).

---

### 6.17c The consumer's float, joined to truth — the lanes are PRECISE and the offset is a BIAS

`scripts/consumer_float_truth_join.py` joins `logs/ambiguity_trace.csv` from
`runs/golden_sim_consumer_ar_a` (§6.16.7b, quiet 24 h bed, SZZ000SIM) to `truth_ambiguities.csv`,
forms within-constellation between-satellite SDs (only those are comparable — the satellite phase
OSB does not cancel between satellites, the receiver bias and the ISB do), and splits the wrapped
residue into **within-arc scatter** (what averaging removes) and **per-arc offset**
(`|wrap(arc mean)|` — the irreducible part).

Settled h12–24, one fixed reference per constellation:

| channel | n_arcs | arc len (ep) | within-arc std p50 | **\|offset\| p50** | off < 0.15 |
|---|---|---|---|---|---|
| GPS EWL (L2W−L5Q) | 25 | 264 | **0.0260** | **0.1734** | 44 % |
| GPS WL (L1C−L2W) | 25 | 264 | 0.0845 | 0.2895 | 24 % |
| GPS NL | 35 | 112 | 0.0528 | 0.2188 | 34 % |
| GPS L1C / L2W / L5Q | 24–25 | 264 | 0.203 / 0.223 / 0.227 | 0.244 / 0.281 / 0.273 | 24–29 % |
| Gal EWL (L5Q−L7Q) | 14 | 521 | **0.0148** | 0.2951 | 29 % |
| Gal WL (L1C−L5Q) | 14 | 518 | 0.0545 | 0.2507 | 29 % |
| Gal NL | 20 | 171 | 0.0553 | 0.2423 | 35 % |
| Gal L1C / L5Q / L7Q | 13–14 | 510–598 | 0.193 / 0.182 / 0.193 | 0.244 / 0.287 / 0.295 | 21–38 % |
| *uniform null* | | | | *0.2500* | *30 %* |

**Read it in two halves.** The lanes are already precise **within** an arc — GPS EWL 0.026 cyc,
Galileo EWL 0.015, WL 0.05–0.08, NL 0.053 — so nothing about noise blocks a fix. But **every**
channel's per-arc offset sits at the uniform null, and only GPS EWL is partly concentrated (0.173,
44 % inside ±0.15). That is a **BIAS, not noise**: more integration cannot remove it. The consumer's
own per-band float σ confirms the other half — SD σ **0.41 / 0.51 / 0.53 cyc** (GPS L1C/L2W/L5Q) and
0.48 / 0.63 / 0.62 (Galileo) after 12 h at a cm-level static position, which is why AR dies at the
ratio battery (2 879 attempts, 0 fixes, ratio p50 1.01, `ratio_battery` 2 596; §6.16.7b).

**⇒ the consumer-side WL→NL cascade of §7 item 27 is DEAD ON ARRIVAL as specified.** The cascade's
WL would come from `computeSatMwBias`'s MW SD wide-lane, which subtracts *the wire's own* satellite
MW bias — the same biases that produce the uniform per-arc offsets above. It would lock onto wrong
integers with a high ratio, which is exactly the Jul-30 user-path failure
(`project_pppar_user_control`: FAIL-UNSAFE, never trust its fix count). The order flips: **the wire's
SD integer consistency has to be established first.** Rider, same run: 2 274 SSR phase-bias resets
(0.79/epoch) + 1 513 code-bias discontinuities + 47 clock discontinuities restart consumer arcs
continuously (median settled arc 2.2 h).

---

### 6.17d The wire, joined to truth directly — every channel is at the null, and the gauge is the ANCHOR's FLOAT

`scripts/simnet_wire_integer_consistency.py` removes the consumer from the question. It forms
`E = [clk_emitted + P_emitted] − [clk_true + osb_true]` per (satellite, signal, epoch) on the same
quiet bed, differences between satellites within a constellation, and grades the wrapped fraction.

**⚠ Fixture semantics, both of which bite.** Satellite-clock truth is written only when
`sat_clk_err.active()`, so `truth_satclock.csv` is all-zero on this fixture; OSB truth is
`truth_bias.csv`'s `osb_m` **on the phase code** (the code rows are a different quantity).

**Result 1 — the wire is at the uniform null on every channel** (settled h12–24, p50 |wrapped SD
fraction|, uniform null 0.2500; n = 43 824 GPS / 29 424 Gal):

| GPS | L1C | L2W | L5Q | WL | EWL | NL |
|---|---|---|---|---|---|---|
| p50 \|f\| | 0.2372 | 0.2854 | 0.2726 | 0.2507 | 0.2382 | 0.2712 |

| Galileo | L1C | L5Q | L7Q | WL | EWL | NL |
|---|---|---|---|---|---|---|
| p50 \|f\| | 0.2652 | 0.2149 | 0.2337 | 0.3143 | 0.2200 | 0.2070 |

Undifferenced it is the same story: `dP/λ` p50 |frac| 0.227–0.271 across all six (satellite, signal)
channels, and `W/λ = (dC − dP)/λ` — what a consumer's ZD ambiguity absorbs — 0.24–0.31.

**Result 2 — the consumer inherits the wire EXACTLY, and the sign is MINUS.** Both sides
re-differenced against the same per-epoch reference, UNWRAPPED (the wrapped test cannot see this: a
> 1-cycle float scatter fills the circle regardless):

| | slope⁻ | Pearson r⁻ | slope⁺ | Pearson r⁺ | resid sd |
|---|---|---|---|---|---|
| mean over 6 channels | **+0.888** | **+0.936** | −0.79 | **−0.859** | **13.7 cyc** |

The residual scatter *is* the consumer's own float error (§6.17c). So `W = dC − dP` is the right
convention — the consumer adds −C and +P, and its float absorbs λN_true + (C − C_true) − (P −
osb_true).

**Result 3 — the offset is a STABLE per-(sat, band) gauge, and it is nobody's integer.** Within one
PBGAUGE/PHASE_ANCHOR tenure the wrapped fraction is a constant to **0.024–0.036 cyc** circular std
(settled; tenures ≈ 86 ep GPS / 148 ep Galileo, 295 / 133 steps), stepping by 0.030–0.057 cyc p50 at
tenure boundaries (p90 up to 0.38). Inverting the gauge — `n = round(−dP/λ)`, then asking which of
the 61 stations' `truth_ambiguities.csv` holds that integer:

| channel | \|dP/λ\| p50 (cyc) | share \|wrap\| < 0.25 | **== SOME station's truth N** | **== the CURRENT anchor's** |
|---|---|---|---|---|
| GPS L1C | 28.1 | 0.516 | 0.703 | **0.004** |
| GPS L2W | 15.9 | 0.461 | 0.753 | **0.031** |
| GPS L5Q | 22.8 | 0.480 | 0.728 | **0.011** |
| Gal L1C / L5Q / L7Q | 24.5–27.7 | 0.497–0.529 | 0.50–0.74 | **0.000–0.003** |

⇒ **the emitted phase bias is 16–28 cycles from truth with a uniform fraction**, and the integer it
carries is *a* receiver's — almost never the current anchor's (`network.phase_datum_prioritized`
pins each (sat, signal) at its own highest-coverage receiver, and the sticky handover keeps an
earlier frame). Splitting the identical population on same-anchor vs different-anchor (STEP 4c)
gives large R at large |μ| on a population of 6–7 satellites — **a common non-integer offset, not
integer content**.

**Result 4 — the shape, made identifiable.** The three-band model `dE_b = r + γ_b i + λ_b n_b + ε_b`
fitted on WRAPPED fractions is **vacuous**: 3 obs − 2 params − 3 free integers = 1 dof, and a
size-matched Monte-Carlo null of independent U(−0.5, 0.5) fractions fits equally well. Fit it on
consecutive-epoch DIFFERENCES instead (integer jumps excluded, 0.05 % of pairs) and it becomes exact:

| settled | full (r, i) | range-only | iono-only | null |
|---|---|---|---|---|
| GPS R² / resid | **0.9999** / 0.31 mm-cyc | 0.9887 | 0.9589 | 0 |
| Galileo R² / resid | **0.9996** / 0.42 mm-cyc | 0.9838 | 0.9594 | 0 |

with \|dr\| p50 **4.1 mm** / 30 s and \|di\| p50 **0.15 mm** (GPS; 3.1 mm / 0.27 mm Galileo).

**⇒ THE READING THE DATA SUPPORTS.** The wire's between-satellite motion is a pure per-satellite
two-factor range+iono offset — no band-specific content above 0.1 mm — sitting on a per-(sat, band)
level that is **an arbitrary receiver's float ambiguity**, held to 0.03 cyc within a tenure and
re-elected at every handover. The non-integer budget of the emitted product is therefore **the
anchor's own float error**, and no amount of consumer-side work reaches it. This is the object
`docs/network_ambiguity_resolution_design.md` Phase 3 exists to replace (float-valued `w=1e10`
pivot pins, `ApplicationNetworkAssembly.cpp:2559`; handover pins at the successor's float,
`NetworkPhaseDatum.cpp:335-428`).

---

### 6.17e The ORACLE slip response — the mechanism is exact, the plateau does not move, and that is the finding

Given 6.17b, the alternative to bounding T4 is to fix the **re-level value**: preserve the level
across a detected slip instead of freeing the arc. Building that honestly (EWL/WL-MW integer
estimation of the jump, `SPEC_lane_slip_response`) is expensive, so its **ceiling** was measured
first. `network.cycle_slip.oracle_relevel` (sim-only; `include/slip/NetworkSlipOracle.hpp`,
`src/core/slip/NetworkSlipOracle.cpp`, 19 tests, OFF byte-identical `D8D94923…`) looks each
detector actuation's TRUE integer jump up in the fixture's `truth_slips.csv` and shifts
`LocalCarry::value_abs` by exactly Σδ·λ instead of freeing the arc. Everything else — the detector
population, the gap re-entry drain, the T4 erase path — is untouched. Full knob contract:
`docs/SPEC_slip_qc_hardening.md` §13.

**The mechanism is exact.** 1 161 repairs; \|δ\| applied p50 50 cyc (p10 26, p90 87); detection
latency 0 for **1 124 of 1 161** (so a pre/post slope test is uninformative — the pre-sample is
already post-slip on the other 37 — and the ratio statistics below are the instrument):

| statistic | value |
|---|---|
| **unrepaired fraction** \|e(t₀) − e(t₀−30)\| / \|δ\|, latency-0 | **p50 0.005** (0.239 cyc), p90 0.020 |
| \|e(t₀)\| / \|δ\| (0 = repaired, 1 = inert, 2 = wrong sign) | **0.070** |
| \|e(t₀+150 s)\| / \|δ\| | **0.078** |

**The slip-side population split** (2 452 truth slips, and what answered each):

| answered by | n | share | \|e\|/\|δ\| at t₀ | at t₀+150 s |
|---|---|---|---|---|
| **REPAIRED** by the oracle | 1 161 | **47.3 %** | 0.073 | 0.076 |
| detected, **KEPT** on every row (window/keying missed it) | 627 | **25.6 %** | 0.892 | 0.112 |
| **FREED** — a no-carry `refloat` row (the entry was gone) | 520 | **21.2 %** | 0.943 | 0.096 |
| **SILENT** — no detector row on (S,s) in the window | 144 | 5.9 % | 0.483 | 0.077 |

Of the 2 697 `oracle_keep` rows, 603 are the partner band of a repair at the same (t₀, S, s), 430
are the same slip already repaired at an earlier epoch (the `last_applied_s` bound, correct), **0**
are a carry re-born by a T4 erase after the slip (the `birth_s` floor, by design), 69 are standalone
with a truth slip within ±600 s, and only **8 are genuine data-domain detector false alarms**. The
remaining 1 587 are unexplained misses whose arcs sit at \|e\|/\|δ\| ≈ 0.889 at t₀ — never repaired —
which is the 25.6 % KEPT class seen from the event side. **That is an open defect of the ceiling
instrument, and it biases the ceiling DOWNWARD.**

**And the plateau did not move.**

| settled | E0 (`golden_sim_campaign`) | E0 + ORACLE | zwdblk | zwdblk + ORACLE |
|---|---|---|---|---|
| ZWD truth rms (m) | 2.098 | **1.779** | 1.188 | 1.104 |
| consumer 3D p50 / rms (m) | 1.752 / 1.799 | 1.736 / 1.896 | 1.085 / 2.389 | **2.234** / 3.493 |
| ambient per-band DD \|e\| (cyc) | 1.653 | **2.429 (+47 % WORSE)** | 1.654 | 1.892 |
| T4 `attrib_arc` events | 7 656 | **13 562 (+77 %)** | 9 871 | 16 095 |
| detector re-levels | 4 545 | **687 (−85 %)** | 4 545 | 717 |
| ref-free WL(1,2) at own integer | 81.5 % | 81.5 % | 86.2 % | 85.9 % |
| ref-free per-band L1C at own integer | 18.1 % | **11.2 %** | 21.0 % | 16.1 % |
| sat-clock contrast rms (m) | 8.050 | 7.777 | 11.791 | 11.806 |

Every pre-registered gate FAILS (ZWD ≤ 0.5 m; ambient ≤ 0.30 cyc; T4 ≤ 1 500; consumer p50 ≤ 0.50 m;
sat-clock ≤ 3 m; WL ≥ 95 %). The mechanism actuated (P1: 1 161 applied) and the wire got *worse* on
the two channels that matter most.

**⇒ THE (ρ, I) SPLIT FREEZE — the reading the data supports.** Detector re-levels fell 85 % while T4
grew 77 %: **arcs restored to their true integer are then rejected by the corrupted MAJORITY.** The
per-band DD errors are equal in CYCLES while the lanes are clean — E0 ref-free medians L1C 0.896,
L2W 1.086, L5Q 0.980, L7Q 0.816 cyc against EWL(2,5) **0.032** and WL(1,2) **0.141** — and an error
that is a constant ε cycles on every band is exactly a (range, iono) split, not an ambiguity error:
solving `δρ + γ_b δI = ε λ_b` on the L1/L2 pair gives (δρ, δI) = (0.1069, 0.0834)·ε metres, which
predicts L5's own per-band error to **0.6 %**. So the station's zero-q levels are frozen around a
*self-consistent* mis-split of range against ionosphere — the (I, N, ZWD) trade of §6.16, latched at
every re-level — and a single-arc level preservation, however perfect, cannot undo a station-wide
one. **Level preservation is the CEILING of half a fix.**

**THE DISCRIMINATOR — oracle + E1 (2026-09-10 11:15, `runs/golden_sim_campaign{,_zwdblk}_oracle_e1`,
both exit 0, 719/720 epochs emitted, 1 A4 σ̂₀ ceiling skip).** The pre-registration (written into the
arm configs' `_comment`, before the runs): **ZWD settled truth rms ≤ 0.5 m PASS** = the plateau is
the T4 chain re-freezing the split on top of the re-levels; 0.5–0.9 PARTIAL; **≥ 0.9 (no better than
E1 alone) FAIL** = the freeze is independent of both. `network.raim_arc_persist_epochs: 1000000`
took: `T4_attrib/attrib_arc` rows are **0** in both arms.

| settled | E0 | **E1** (T4 starve) | oracle | **oracle + E1** | zwdblk | zwdblk + oracle | **zwdblk + oracle + E1** |
|---|---|---|---|---|---|---|---|
| **ZWD truth rms (m)** | 2.098 | **0.933** | 1.779 | **1.819** | 1.188 | 1.104 | **1.239** |
| consumer 3D p50 (m) | 1.751 | 2.559 | 1.730 | **3.781** | 1.084 | 2.234 | **3.334** |
| consumer 3D rms (m) | 1.799 | — | 1.896 | **4.909** | 2.389 | 3.493 | **4.294** |
| ambient per-band DD \|e\| (cyc) | 1.65 | — | 2.43 | **6.09** | 1.65 | 1.89 | **5.66** |
| T4 `attrib_arc` events | 7 656 | 0 | 13 562 | **0** | 9 871 | 16 095 | **0** |
| detector re-levels | 4 545 | — | 687 | **275** | 4 545 | 717 | **275** |
| ref-free WL(1,2) at own integer | 81.5 % | 65.4 % | 81.5 % | **35.4 %** | 86.2 % | 85.9 % | **39.7 %** |
| ref-free EWL(2,5) | 99.0 % | — | 98.6 % | **75.5 %** | 99.2 % | 99.0 % | **74.7 %** |
| ref-free per-band L1C | 39.1 % | — | 29.8 % | **19.9 %** | 44.0 % | 38.0 % | **20.7 %** |
| sat-clock contrast rms (m) | 8.050 | 2.76 | 7.777 | **3.159** | 11.791 | 11.806 | **3.243** |
| sat code-bias residual rms (m) | 0.608 | 0.29 | 0.537 | **0.362** | 0.678 | 0.638 | **0.356** |

**VERDICT: FAIL, and worse than that.** Both arms are ≥ 0.9 m, so the pre-registered FAIL branch
fires — *the split freeze is independent of both mechanisms*. But the arms are also **worse than E1
alone on every channel E1 improved**: ZWD 0.933 → 1.819, consumer p50 2.559 → 3.781, WL 65.4 →
35.4 %. Only the two channels E1 owned outright — sat-clock contrast and code bias — kept E1's gain
(8.05 → 3.16 m, 0.61 → 0.36 m). **The two "fixes" are anti-synergistic.**

**And the ceiling instrument got BETTER while the result got worse**, which removes the obvious
objection. With T4 starved the oracle repairs **1 549 of 2 452 truth slips (63 %, up from 1 161 =
47 %)** on an *identical* detector population (13 485 actuations in both arms), and the `no_carry`
fall-through — 6.17e's 21.2 % FREED class — collapses **3 149 → 1 396 (−56 %)**, with the
`birth_s` lower bound binding on 273 instead of 1 041. **That confirms the FREED class was T4
deleting the carry out from under the repair**; removing T4 recovers it, and the plateau still does
not move. Slip DETECTION is flat at **94.0 %** on every arm (`simnet_slip_census.py`) — nothing here
is a detection story.

**⇒ THE LEVER IS THE LEVEL'S UNCERTAINTY ALONG THE SPLIT DIRECTION, NOT THE RESPONSE.** With every
erase starved AND every detected slip repaired to truth, the ambient per-band DD error **rises to
6.09 cyc** (E0 1.65) while the lanes hold better than the bands. Nothing is left to disturb the
frozen (ρ, I) split, so it compounds: T4's erases, destructive as 6.17b shows them to be, were also
the only mechanism periodically re-randomising it. A zero-q level that is *individually* correct is
still wrong *jointly* — the station's per-LOS ionosphere and its levels share a direction the data
does not pin, and neither a perfect detector response nor removing the erase tier addresses that.
The candidate build is a **per-LOS ionosphere carried IN-BLOCK** (the same vehicle distinction the
ZWD card just paid for, §6.16.7d) — ⚠ **re-read `project_iono_carry_8b_campaign`'s four
falsifications for the double-count reading before building it**; a diagonal per-LOS iono row is
exactly the shape that was falsified there, and §6.16.7d's whole finding is that the diagonal
vehicle is the double count. §7 item 33.

---

### 6.17f PB-gauge LEVELLING — FALSIFIED as built (branch `worktree-agent-a7e2586003a605548`, commit `9b59a2c`, NOT merged)

6.17d says the emitted phase bias carries an arbitrary receiver's float. The obvious emission-side
answer is to estimate the common fractional part per (sat, signal) — δ̂ = circular mean over the
carried stations of the SD fraction against a sticky reference satellite, unwrapped EWMA — and emit
`P' = P + λδ̂`. Built as `network.emit_pb_gauge_level` (+`_alpha` 0.2, `_min_stations` 5,
`_min_concentration` 0.6, `_trace`) and validated `runs/golden_sim_pbgauge` vs `_ctl`, quiet 24 h,
single knob, consumer `hold_variance` 1e-6, graded with the two truth joins above.

**The mechanism actuated and changed nothing it exists to change.** 347 683 of 457 392 P records
(76.0 %) adjusted across 143 families, mean \|δ\| 0.65 cyc — and:

| pre-registered gate | result |
|---|---|
| G1 wire SD fraction p50 \|f\| 0.25 → ≤ 0.08 on **every** band | **FAIL — 0 of 12 channels.** ON wins 6, loses 6; best is Gal NL 0.138, the six primary bands 0.228–0.280 |
| G2 consumer SD arc-offset p50 → ≤ 0.10 | **FAIL — 1 of 12** (GPS EWL 0.173 → 0.036); 5 of 12 got WORSE |
| G3 consumer fixes > 0 at p_succ ≥ 0.999 | **FAIL — 0 fixes in BOTH arms**, ratio max 1.21 vs a 3.0 threshold |
| G4 float consumer 3D rms unchanged ±10 % | **FAIL — 0.0761 → 0.1037 m rms (+36 %), p50 0.0603 → 0.0907 (+50 %)** |
| G5 PB-disc resets ≤ +10 % | PASS (2 274 → 2 313, +1.7 %) |
| P1 mechanism-alive | PASS |

Worse, the wire's **within-tenure** circular std rose **1.1–1.9×** and its tenure-to-tenure step
rose up to 2.1×: the gauge added wander instead of removing an offset.

**Two independent root causes, both visible in its own trace.**

1. **The premise is only weakly true.** The carried per-station SD fractions have R p50 **0.572** at
   n ≥ 5 — against ≈ 0.41 for pure uniform noise at n = 6 — and **the carry covers a median of 6 of
   61 stations** per family per epoch. Three quarters of all innovations never reach the EWMA:
   39.0 % rejected `n < 5`, 36.2 % rejected `R < 0.6`, 24.9 % accepted. The "network-common
   fractional part" being measured is largely the estimator's own scatter.
2. **The unwrapped EWMA is a RANDOM WALK.** Per-epoch \|δ step\| is tiny (p99 0.039 cyc, well inside
   the α/2 = 0.1 bound; only 0.70 % of epochs above 0.05), but the never-wrap design integrates a
   persistently-signed innovation: per-family \|δ\| span p50 **1.146 cyc**, p90 2.52, **max 7.67**;
   **half of all families walk more than a full cycle over 24 h**; 19 families exceed 2 cyc.
   G03/L1C runs −0.06…−0.08 cyc/epoch for six straight hours under ONE unbroken reference (G09, no
   switch) and arrives at −7.63 cyc = **−1.45 m injected into the emitted L1C phase bias**. It is
   uncorrelated with PHASE_ANCHOR churn (20.18 events/sat on the big-δ families vs 20.78 on the
   others) — an integrator property, not an event-response failure.

**⇒ KEEP_OFF, FALSIFIED as built.** The lesson generalizes: an emission-side gauge estimate needs a
**bounded/wrapped** δ̂ with per-epoch authority and a much higher concentration bar — and even a
perfect one would not have been fixable here, because the consumer's own float is 13.8 cyc sd
(6.17c/6.17d) and AR never reaches p_succ. The branch is **not merged**; it survives as the vehicle
for the field CONTINUITY mode (6.17h), which is a different mechanism with no integer content.

---

### 6.17g The FIELD after the ZWD fix — one wandering direction, and phase biases that step by METRES

**The consumer residual is ONE slowly-wandering axis** (`scripts/field_consumer_residual_census.py`,
`runs/fldf3_on2_zwdblk` vs the control `runs/fldf3_on2`, settled ≥ h6):

| | in-block (`fldf3_on2_zwdblk`) | control (`fldf3_on2`) |
|---|---|---|
| PC1 share of settled ENU variance | **0.733** | 0.767 |
| PC1 direction | **el 49.8° / az 9.3°** (NNE) | el 73.2° / az 137.3° |
| corr(N, UP) | **+0.822** | −0.390 |
| UP ACF < 0.5 | **11.5 min** | 13.0 min |
| UP sd(30-min block means) / sd(within block) | 1.466 / 1.232 (**59 %** of UP variance is between blocks) | 2.038 / 1.389 |
| site ZWD deviation sd | **0.0101 m** | 0.1247 m |
| fleet ZWD span p50 / max | **0.143 / 5.12 m** | 1.195 / 491.5 m |
| AR fixes (settled) | 0 | 0 |

The axis points into the site's own constellation void (zero LOS-epochs at az 315–45°, el 30–60°),
i.e. it is the float solution's weakest direction. **The tropospheric channel is CLOSED**: fleet
ZWD deviation sd 21.5 → 4.9 mm, regressing the site's own ZWD out of UP removes 1.8 % of the
variance, and the slope (+33.8 m/m) is nowhere near mapping-collinear. ⚠ **Block-level "drivers" do
not survive**: the top Spearman correlations at 30-min block resolution (`n_dw_engaged` +0.82,
`PBGAUGE` −0.74, `zwd_fleet_dev` −0.72) are slow levels whose autocorrelation-corrected p is
0.24–0.37; the fast per-epoch counters (arc refloats, T4, `n_eff` ≈ 304) find nothing.

**So the instrument moved to the wire** (`scripts/field_wire_phase_clock_census.py` — truth-free
30-min local-linear smoothness of clk and clk+P per satellite, clipped at every PBGAUGE /
PHASE_ANCHOR / retro-bump / pivot / datum-change / absence boundary, per-epoch trimmed mean removed
as the datum). On `runs/fldf3_on2_zwdblk`, settled 6 h, 32 GPS satellites, 22 725 sat-epochs:

| channel | within-tenure deviation rms p50 | p90 | max | × COD-final's own floor |
|---|---|---|---|---|
| **clk** | **0.0256 m** | 0.082 | 0.127 | 3.9× (floor 0.0066 m) |
| pb_L1C | 0.2817 | 0.645 | 1.56 | 42.6× |
| pb_L2W | 0.6345 | 1.555 | 6.79 | — |
| **pc_IF** (= clk + P, IF) | **0.4642** | **2.297** | **10.535** (G06; G12 4.62) | **70.2×** |

**The emitted CLOCK is smooth — essentially at the final-clock floor — and the emitted PHASE BIASES
STEP BY METRES.** Decomposing: cutting every within-tenure one-epoch step > 0.50 m removes **86.3 %**
of pb_L1C's variance, **91.9 %** of pc_L2W's and **69.2 %** of pc_IF's, against only 27.5 % of the
clock's. These are **unmarked** steps by construction (the one-epoch differences are re-centred on
their own cross-satellite median, so anything constellation-common is invisible), i.e. invariant #7
edges with no event: 1 012 pc_IF steps > 0.25 m in 6 h = **5.3 per satellite per hour**. The largest
are network-wide — 26 satellites at tow 282150 (events: T4_attrib ×4), 18 at 283770 (T4_attrib ×7),
12 at 299490 (**T4_attrib ×22, and nothing else**).

**The handovers are not continuous either.** In the 5 min after a PBGAUGE (349) or PHASE_ANCHOR
(382) event the deviation is **6.9 / 6.5 m rms against 1.4 m elsewhere — ×5**. The successor's
variance exceeds `handover_max_var` 0.01, so the "continuous" handover cold re-realizes at a
metre-class float. Every such step trips the 0.25 λ discontinuity test ⇒ a consumer ambiguity
RESET ⇒ the field consumer never holds an arc (665 resets / 12 h, §6.16.7c).

**⚠ The geometry closure FAILS, and that is informative.** Projecting each satellite's wire wander
through the consumer's own geometry (`G = [−e_ENU, 1]`, `W = diag(sin²el)`, four preparations ×
two trims) gives corr(predicted, actual) ≈ 0 in E/N/U on every combination (|z| < 2.2 everywhere;
`n_eff` 15–369 after Bartlett correction), and the predicted axis is el 76–79° against the measured
el 49.8° / az 9.3°. Only the geometry-free magnitude link survives, weakly: corr(rms visible
\|δ\|, \|consumer error on the axis\|) = **+0.396**, |z| 2.05. ⇒ **the consumer's 12-min wander is
NOT a linear projection of the wire's fast (ACF 1–2.5 min) bias wander — it is the consumer's
perpetual RE-CONVERGENCE after each reset, expressed in its weakest direction.** Two independent
sanity controls hold: a white-in-time surrogate with the same per-satellite rms predicts 25.8 m on
the axis (real wander 2.98 m), and adding a ZWD column to the design inflates the prediction rather
than absorbing it.

---

### 6.17h What is in flight (pre-registered; results append here, nothing above is edited)

1. **Field emission CONTINUITY** — `network.emit_pb_gauge_mode: continuity` on the same branch as
   6.17f, but a different mechanism with **no integer content**: a per-family emission offset that
   absorbs every frame step (0.10–40 cyc) so the emitted bias is continuous across handovers and
   mass refloats. The satellite's true bias is constant and the gauge is arbitrary and common to
   every station's product, so absorbing the step is free. Arms `_fldf3_on2_zwdblk_pbcont` vs
   `_logctl`; pre-registered: consumer resets −70 %, pc_IF within-tenure deviation 0.46 → ≤ 0.10 m,
   consumer 3D 2.93 → ≤ 2.0 m. Note `emit_pb_filter` (the v1 continuity KF) is structurally OFF on
   the leveling vehicle — it is nested under `emit_rebase`, which is `carry_invariant`-only.
2. **Phase-2 producer AR, re-run with a gradeable trace.** `NetworkProducerAr` /
   `network.ambiguity_resolution.*` exists and was falsified only on the pre-F3 bed (near_int 34 %);
   the quiet golden now carries 87 % ref-free DDs at the integer. It is **log-only** today, so a
   trace (`network_producer_ar.csv`: per-DD float, fixed, ratio, and the pair identity) is a
   prerequisite for grading its fixes against `truth_ambiguities.csv`. See §7 item 30.

---

