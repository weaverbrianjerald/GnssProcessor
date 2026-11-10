# ZWD sink: sink census, slipped-bed golden FAIL, and the integer information

**STATUS**

- **id:** ZWD sink
- **topic:** sim-bed
- **status:** closed
- **summary:** Settled ZWD span is a truth-validated sink proxy; the golden FAILs all three gates on the slipped bed; `carry_zwd_in_block` RECOMMEND_SCOPED (field span p50 1.195 → 0.143 m).
- **dates in the text:** 2026-09-09 → 2026-09-10
- **knobs / posture:** `network.raim_arc_persist_epochs`, `estimation.ambiguity_resolution.hold_variance`, `network.product_output_path`, `estimation.ionosphere_weighting.std_scale`, `network.carry_zwd_in_block`, `network.phase_leveling_local_block`, `network.carry_zwd`, `network.phase_leveling_local_block_min_age_ep`, `network.code_datum_time_linked`, `network.cycle_slip.gap_instrument`
- **hashes named in the text** (commit / exe / byte-cert): `36ace5e`, `966d4fd`, `7b32a74`, `00d5fdf`, `319a6aed`
- **run dirs named in the text:** `runs/golden_sim_tri_w6`, `runs/fldf3_ci`, `runs/fldf3_pl2`, `runs/fldf3_on2`, `runs/camp60wu_f5c`, `runs/fldf3_pl_gppdp3h`, `runs/golden_sim_campaign`, `runs/simnet60_campaign_wu93` … (+11 more in the text)
- **source:** `docs/archive/SPEC_amb_iono_split_overcount.md` §6.16, 6.16.1, 6.16.2, 6.16.3, 6.16.4, 6.16.5, 6.16.6, §6.16.7

**Outcome.** The card asked where the metre-class ZWD sink sits and whether the emitted wire carries integer information. Dropping the worst station relocates the sink; starving the T4 erase cuts slipped-bed ZWD truth rms 56 % but costs the consumer, so the named fix is bounded identification. Consumer AR on the quiet golden fixes 0 of 2 879 because its own float σ is 0.32 to 0.44 cyc: "no integers on the wire" is not established. `code_datum_time_linked` stays armed. The owed same-build F2 control re-ran byte-identical, so the F2 A/B stands ([0910 morning](../ionosphere/AMB-IONO-0910-MORNING.md) §6.17a); the re-founding goldens keep `carry_zwd` off ([LSR](../../refound/LSR_long_scale_remainder_2026-10-01.md)).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_amb_iono_split_overcount.md -->
## §6.16 The ZWD sink census, the slipped-bed golden, and where the integer information is (2026-09-09)

Four measurements from one day, in the order they constrain each other: a truth-free instrument that
reads the same defect on every bed; the one station that looked like the cause and is not; the golden
posture failing on a slipped truth bed (so the sim now reproduces the field's metre-class wire); and
the first honest look at whether the EMITTED wire carries integer information at all.

### 6.16.1 The instrument — settled per-station ZWD span

`logs/network_zwd.csv` (`week,tow,station,zwd_m,std_m,carried`); per-station **span = max − min over a
SETTLED window** (field ≥ h6; sim last 50 %). Physical ZWD varies < 0.5 m / 12 h, so any span above
~1 m is a sink, not weather — the estimator is parking something in the wet delay.

⚠ **Never quote a whole-run span.** Ignition dominates it on every bed: `runs/golden_sim_tri_w6`
reads whole-run p50 **2.011 m / max 17.23 m** and settled (h12+) p50 **0.0632 m / max 0.0797 m, 0 of
60 above 0.10 m** (`runs/golden_sim_tri_w6/BASELINE.md` §2).

**It is truth-validated on the sim**: quiet golden span 0.076 m ↔ ZWD truth rms 0.009 m; slipped
`camp60wu_f5c` span 2.29 m ↔ truth rms 2.85 m. That licenses it as a FIELD proxy, where no truth
exists.

| bed / vehicle | settled span p50 | n > 1 m / > 10 m | top stations |
|---|---|---|---|
| FIELD shipped control `runs/fldf3_ci` (carry_invariant + eq34 + SH, DOY-129, 63 sta) | **21.4 m** | 61 / 50 | BSHM 94, HKSL 67, KRGG 55, GAMB 51 |
| FIELD converging `runs/fldf3_pl2` (leveling vehicle, gauge projection + prioritized family) | **0.797 m** | 24 / 3 | **BSHM 368.9**, CUSV 12.8, COCO 12.7 |
| FIELD `runs/fldf3_on2` (the same + F3) | 1.20 m | 39 / 4 | INEG 492, BSHM 35 |
| SIM golden lineage, quiet 24 h (`readmit24h_e2`, `camp60wu_quiet24h_sticky`, `nonint_losblock24h_hv`) | **0.076 m** | 0 / 0 | S06 0.20 |
| SIM slipped campaign, pre-F3 (`runs/camp60wu_f5c`, h3–h6) | 2.29 m | 52 / 4 | S45 28 |
| SIM `carry_invariant` + eq34 arms, Aug code (`simnet60_ab_eci_eq34_ci`, `ab_d5_*`, `simnet60_tri_slant_trio_480`) | 389 – 34 083 m | 60 / 57–60 | km-class divergence |

(Census of 2026-09-09; each figure recomputed from that run's own `network_zwd.csv`. The three
`fldf3_pl2*` rows were re-derived for this section and reproduce to the printed digits — p50 0.797 /
0.660 / 0.904 m on control / `gap_reentry_relevel` / bridge arms respectively.)

**Reading.** The shipped field vehicle's per-epoch ZWD is a metre-to-decametre sink at EVERY station,
and the same vehicle reproduces km-class on the sim. The converging leveling posture (§6.15.4) brings
the fleet centre to sub-metre; the quiet sim golden at 7.6 cm is what "honest" looks like. **The
sink, not the ambiguity level, is now the field's leading residual.**

### 6.16.2 BSHM00ISR is a phase-side receiver fault — and roster edits are not the lever

BSHM00ISR is broken from the FIRST epoch on every field arm: its ZWD is **−5.46 m at tow 259200**
(`runs/fldf3_pl2/logs/network_zwd.csv`; a physical ZWD is 0–0.4 m), run min/max **−2445.97 / +359.43 m**,
and the station-congruence rider flags it CLEAN-labelled with d = **(−417.82, −144.14, −770.88) m** ENU
at t = **2.188e6** on `n_code 0 / n_phase 14` (`runs/fldf3_pl2/logs/network_congruence.csv`, tow
271200, `reported=1`, measure-only). Code side admits it, phase side does not: the fault is on PHASE.
Its roster ECEF is 17 cm from the RINEX APPROX POSITION, so this is not a coordinate blunder. (The
−2446 m excursion at 271200 specifically is the gap-re-entry poison case —
`docs/SPEC_slip_qc_hardening.md` §12.1 — not the baseline pathology.)

**Footprint A/B (3 h, DOY-129, settled ≥ h1, 57 stations common to all three arms):**

| arm | 57-sta Σ settled span | INEG00MEX | OAFA00ARG | consumer h2 rms |
|---|---|---|---|---|
| base `runs/fldf3_pl_gppdp3h` | **59.8 m** | **1.85 m** | 9.12 m | **2.02 m** |
| `_nobshm` (BSHM dropped) | **61.5 m** | **26.32 m** | 8.21 m | **3.92 m** |
| `_notop4` (BSHM/OAFA/INEG/CUSV dropped) | **53.8 m** | — | — | **1.53 m** |

**Dropping the worst station does not shrink the sink — it relocates it.** INEG00MEX takes it
(1.85 → 26.3 m) and the fleet sum goes UP. Dropping the top four improves most instruments but the
consumer is non-monotone in roster (2.02 / 3.92 / 1.53 m). ⇒ **The residual null direction parks on
whichever station is weakest; roster editing is not a fix, and a per-station ZWD excursion is not
evidence about that station.** The shipped `carry_invariant` vehicle hides BSHM entirely because its
free-ambiguity rows absorb the step (ZWD σ 0.85 m there vs 0.028 m on the leveling vehicle) — the
sink is a property of the vehicle, not of the roster.

### 6.16.3 The golden posture on the SLIPPED sim bed — FAIL on all three gates

`runs/golden_sim_campaign` (= `config/golden/golden_sim_tri_w6.json` verbatim, 61 `rinex_path`
re-pointed at `runs/simnet60_campaign_wu93`, duration and output paths only; exe
`gnss_processor_golden0.exe` sha256 `04267126…` = HEAD `36ace5e`), 720/720 epochs, exit 0.
Full card `runs/golden_sim_campaign/VERDICT.md`.

| gate | pre-registered | golden-on-campaign | pre-F3 `camp60wu_f5c` | verdict |
|---|---|---|---|---|
| ZWD settled truth rms | ≤ 0.10 m | **2.098 m** | 2.742 | **FAIL** (21×) |
| per-sta settled ZWD span p50 | ≤ 0.20 m | **1.590 m** | 2.292 | **FAIL** (8×) |
| consumer settled h3–h6 p50 3D | ≤ 0.30 m | **1.752 m** | 1.614 | **FAIL** (6×, worse than pre-F3) |

**The integer half, however, collapses in the good direction** (`simnet_amb_integer.py --ref-free`,
"at own true integer", settled): EWL(2,5) **99.0 %** (f5c 79.1), WL(1,2) **81.5 %** (33.7), L1C
**39.1 %** (18.6); as-traced EWL 98.7 / WL 70.5. ⇒ **F3 fixed the ambiguity over-count and left the
station-level ZWD⇄iono exchange intact** (interaction 3.54 → 2.46; median exchange correlation
−0.433 → −0.350; corr(log ZWD rms, event rate) 0.692 → **0.765**, i.e. the plateau is MORE tightly
tied to the per-station event rate). **The sim truth bed now reproduces the field's metre-class wire
under realistic slips** — which is the useful outcome of a FAIL.

### 6.16.4 E1 / E2 — two separable halves, and neither knob is the fix

Single-knob arms off the same file, same exe, same fixture, 720/720 each (VERDICT.md
§"E1/E2 discriminators").

⚠ **E1's knob is `network.raim_arc_persist_epochs: 1000000`, NOT `raim_attribution: false`** — the
loader force-re-enables `raim` + `raim_attribution` whenever `raim_within_epoch` is armed, so that
knob is a proven byte-identical no-op. The persistence gate is what starves the T4 isolated-arc
ERASE while leaving both detection tiers armed (`NetworkRaim.cpp:608-649`).

| row (settled) | **E0** golden | **E1** no arc-refloat | **E2** F3 off | `f5c` pre-F3 | quiet `readmit24h_e2` |
|---|---|---|---|---|---|
| ZWD truth rms (m) | 2.098 | **0.933** | **3.799** | 2.742 | 0.007 |
| per-sta span p50 (m) | 1.590 | 1.312 | 3.227 | 2.292 | 0.053 |
| consumer h3–h6 p50 (m) | 1.751 | **2.559** | 1.996 | 1.615 | 0.075 |
| consumer last-30 median (m) | 1.423 | **0.686** | 2.808 | 1.720 | 0.068 |
| iono⇄ZWD interaction rms (m) | 2.456 | 2.493 | 4.090 | 3.544 | — |
| sat-clock contrast rms (m) | 8.050 | **2.761** | 13.307 | 13.001 | — |
| sat code-bias residual rms (m) | 0.608 | **0.289** | 0.978 | 1.252 | — |
| **T4 one-solve arc refloats** | 22 968 | **0** | **35 010** | — | 147 (24 h) |
| post-fit σ̂₀(phase), last ep | 1.424 | **2.687** | 1.217 | 0.906 | 1.314 |
| T3 cell rate | 1.561e-1 | **3.191e-1** | 8.350e-2 | 5.148e-2 | 3.919e-4 |
| T3c consensus rate | 4.414e-1 | **8.159e-1** | 2.638e-1 | 2.024e-1 | 6.463e-1 |
| WL(1,2) ref-free at own integer | **81.5 %** | 65.4 % | 37.6 % | 33.7 % | 99.1 % |
| EWL(2,5) ref-free | **99.0 %** | 91.0 % | 84.6 % | 79.1 % | 100 % |
| D3 arc-age interaction rms, 0–60 s → 1200+ s (m) | 4.264 → 1.974 | **3.114 → 2.932 (FLAT)** | 6.011 → 2.995 | 5.252 → 2.785 | — |
| corr(log ZWD rms, log event rate) | 0.765 | **0.123** | 0.853 | 0.692 | — |

**Reading — the halves separate, and each has a different agent.**

**(a) The T4 erase channel is the dominant single contributor to the metre-class ZWD plateau.**
Starving it: ZWD rms −56 %, sat-clock contrast −66 %, code-bias −52 %, the arc-age slope GONE, the
plateau-to-event-rate correlation 0.765 → 0.123, while the iono⇄ZWD interaction is FLAT (2.456 →
2.493). So the ZWD gain is not a change in the exchange — it is the removal of a repeated
re-convergence forcing: **every erase is a code-level re-level, i.e. a young arc, i.e. a ZWD/clock
sink** (invariant #1's re-coupling edge again).

**(b) The carries T4 was catching are real.** Without erases the consumer settled p50 is 46 % worse,
ref-free WL gives back a third of what F3 bought, σ̂₀(phase) 1.42 → 2.69, and T3c runs at 0.816 —
nothing is being erased, so the post-fit pool is loud and un-screened. Slip response shifts tier, not
size (`T4_attrib` 53 → 0 while `raim_refloat` 19 → 84; DETECTED flat at 94.0 %).
⇒ **The fix is BOUNDED IDENTIFICATION (Tier-C3 solution-separation confirmation of the median-polish
proposals), NOT switching T4 off.** `raim_arc_persist_epochs` stays a probe.

**(c) F3 is load-bearing and stays armed.** E2 is worse on every channel (ZWD 3.80 m, WL 37.6 %,
consumer rms 3.61) — and **T4 goes UP without F3 (35 010 vs 22 968)**, which falsifies the standing
suspicion that F3's block rows are what drives the T4 storm (§7 item 4).

Ordering on the wire: E1 (0.93) < E0 (2.10) < f5c (2.74) < E2 (3.80). On the integers:
E0 > E1 > E2 > f5c. **Nothing on this bed is simultaneously best at both.**

### 6.16.5 The HEAD baseline on the quiet bed (PARTIAL, 18.6 h) — the ~5 cm KPI holds

`runs/golden_sim_tri_w6` = the golden verbatim on `runs/simnet60_quiet_wu93_24h`, HEAD exe. ⚠ The run
was killed by a reaped shell at 2 230 epochs (18.6 h of 24 h): **no run-end telemetry, no
`network_flag_rates.csv` / `network_run_summary.csv`, `network_raim.csv` 0 bytes ⇒ the QC tier table
is NOT gradeable on it**; consumer "settled" is h12–18.6 (`BASELINE.md` header).

| channel | `golden_sim_tri_w6` (18.6 h) | `readmit24h_e2`, matched window |
|---|---|---|
| consumer settled 3D rms / p50 (m) | **0.0823 / 0.0506** | 0.0882 / 0.0779 |
| consumer last-30 median (m) | 0.0305 | 0.0294 |
| ZWD residual rms / p50 / max (m) | **0.0070 / 0.0045 / 0.0374** | 0.0076 / 0.0047 / 0.1596 |
| per-sta settled ZWD span p50 (m) | **0.0632** (0 of 60 > 0.10) | 0.0533 |
| sat clock residual rms (m) | 1.5354 | 1.6285 |
| sat code bias rms (m) | 0.1469 | 0.4435 |
| DD ref-free settled at own integer | **EWL 100 %, WL 99.2 %, L1C 90.5 %**, L2W 88.1, L5Q 87.4, L7Q 87.3 | EWL 100, WL 99.1, L1C 90.0, L2W 86.9, L5Q 86.4, L7Q 87.1 |
| DD ref-free settled med\|u\| (cyc) | L1C 0.068, WL 0.025, EWL 0.010 | L1C 0.079, WL 0.028, EWL 0.011 |

At or slightly better than the 24 h reference on every like-for-like row despite 2.7 h less settling.
**The ~5 cm consumer KPI and the sub-cm ZWD hold at HEAD** — so nothing in §6.16.3's failure is a
regression of the code; it is what the SLIPPED bed does to this posture.

### 6.16.6 Where the integer information is — the P records carry none, and that is gauge-ambiguous

Measured on the quiet golden `runs/readmit24h_e2` (truth bed), 2026-09-09; session-scratchpad
analysis recorded in the ZWD-sink census memory record, not re-derived here:

* The ESTIMATOR's DD ambiguities are ~**87 %** at the true integer ref-free (per-band ref-free
  87.1–90.5 %, §6.16.5).
* The EMITTED P records carry **no fractional structure**: within one epoch the between-satellite SD
  fractions of the phase biases are uniformly scattered — within-group concentration p50 **0.18**
  against the uniform null ≈ **0.28** for n ≈ 10; per-satellite circR p50 **0.43** as-traced /
  **0.52** ref-free.
* **1 267** ref-free steps > 0.15 cyc in h12–24 across 156 (cst, sig, sat) series, and **NONE within
  90 s of a PBGAUGE handover** (942 continuous + 93 cold on the run). So SPEC_amb_iono §6.8's
  "P-record fractional uniform on every arm (open)" is a **high-frequency per-epoch wander, not
  handover steps** — the handover card is not the explanation.

**Reading (pre-registered, NOT proven).** On the free-slant-iono posture each satellite's per-band
phase-bias pair carries an iono-direction gauge (δ, γδ) pinned only by code (σ 0.3 m zenith on this
fixture). That direction is invisible to between-station DDs — hence the 87 % — and is EXACTLY
absorbed by a consumer that estimates its own slant ionosphere. **⇒ the per-band SD-fraction grade
(`simnet_grade.grade_phase`) is not the integer test that matters on this posture; it is
gauge-ambiguous.** The test that matters is the closed-loop consumer fixing integers against
`truth_ambiguities.csv`, and it has NEVER been run on the sim golden (`closed_loop.ar_mode: 0`).

**Arms queued** (written 2026-09-09, not yet run): `config/test_suite/_golden_sim_consumer_ar_a.json`
and `_b.json` — `golden_sim_tri_w6.json` verbatim on the quiet 24 h bed with the consumer's
`closed_loop.ar_mode` 0 → 1, `ratio_threshold` 3.0, `enable_partial_ar`, `logging.enable_ambiguity_trace`,
producer UNCHANGED (float); a/b differ ONLY in `estimation.ambiguity_resolution.hold_variance`
(1e-3 vs 1e-5) and their own output paths.

**AR sequencing decision (binding until the consumer test reports).**
1. Run the consumer-AR truth test first. If integers recover, the wire already carries them and the
   next question is robustness under slips (campaign bed) and then the field.
2. Only if the consumer test says the WIRE loses integers does the network datum work come first
   (emission chain / a phase GF datum / design-doc Phase 3 integer-leveled pins —
   `docs/network_ambiguity_resolution_design.md` §2–§4: pivot pins are float-valued `w = 1e10`
   targets, `ApplicationNetworkAssembly.cpp:2559`; anchor handovers pin at the successor's float
   value, `NetworkPhaseDatum.cpp:335-428`).
3. **Producer DD-fixing (M5) stays DO-NOT-BUILD** until the consumer test says the wire needs it; its
   prerequisite (DD convergence) is met on the quiet bed only.
⚠ The FIELD golden runs `ar_mode 1` on a metre-class wire — wrong fixes by construction; consider
`ar_mode 0` there until the wire is sub-decimetre.

### §6.16.7 Late results (2026-09-10 00:00–02:30)

Five results that landed after §6.16 was written: the full-length HEAD baseline that §6.16.5 could
only sample; the consumer-AR test §6.16.6 pre-registered; a per-satellite accounting of the FIELD
consumer's error; the ZWD-in-block card's three arms; and the `code_datum_time_linked` A/B that
closes `docs/SPEC_family_gf_datum.md` §2.1's owed work. **Nothing above this line is edited.** Every
figure below was recomputed for this section from the named run's own files.

#### 6.16.7a The full 24 h HEAD baseline — §6.16.5's PARTIAL, replaced

`runs/golden_sim_tri_w6_full` = `config/test_suite/_golden_sim_full.json`, which is
`config/golden/golden_sim_tri_w6.json` **verbatim with `network.product_output_path` as the only
differing key** (flattened config diff run for this section; its `_comment_arm` is the only other
addition), exe `gnss_processor_golden0.exe` = HEAD
`36ace5e`. 2 879/2 879 epochs emitted, 0 skipped, exit 0. Grade files under `grade/`
(`simnet_grade.py --settle-frac 0.5`, `simnet_amb_integer.py --ref-free`).

| channel (settled = last 50 %, h12–24) | **full 24 h** | §6.16.5 PARTIAL 18.6 h | quiet ref `readmit24h_e2` |
|---|---|---|---|
| consumer 3D rms / p50 / max (m) | **0.0762 / 0.0604 / 0.2073** | 0.0823 / 0.0506 / — | 0.0882 / 0.0779 |
| consumer UP mean / rms / max (m) | **+0.0066 / 0.0644 / 0.1918** | — | — |
| consumer last-30 median / final 3D (m) | 0.018 / 0.117 | 0.0305 | 0.0294 |
| ZWD residual rms / p50 / max (m) | **0.0072 / 0.0046 / 0.1436** | 0.0070 / 0.0045 / 0.0374 | 0.0076 / 0.0047 / 0.1596 |
| sat clock residual rms / contrast rms (m) | 1.5005 / 1.4563 | 1.5354 / — | 1.6285 |
| sat code bias rms / p50 / max (m) | 0.1482 / 0.0961 / 0.5231 | 0.1469 | 0.4435 |
| rx clock contrast rms (m) | 1.8730 | — | — |
| DD ref-free, settled, at own true integer | **EWL 100.0 %, WL 99.1 %, L1C 86.4 %**, L2W 85.8, L5Q 83.5, L7Q 80.5 | EWL 100, WL 99.2, L1C 90.5 | EWL 100, WL 99.1, L1C 90.0 |
| DD ref-free settled med\|u\| (cyc) | EWL 0.010, WL 0.025, L1C 0.078 | EWL 0.010, WL 0.025, L1C 0.068 | — |
| DD **as-traced** settled at own integer | EWL 100.0 %, WL 97.9 %, **L1C 54.1 %** | — | — |
| P-record between-sat SD fraction, rms / p50\|·\| (cyc) | 0.277–0.301 / 0.213–0.261 | — | — |

Run-end, same console: **1 035 anchor handovers, 953 CONTINUOUS + 82 cold** (largest avoided
`|target|` 27.94 m), 328 marked family-epochs marginalized, 1 463 leveling local carries erased;
**28 re-admissions detected**, 110 entries parked, 26 clock + 84 code-bias columns seeded on return
(all 26 clock seeds folded ANCHORED).

**Readings.** (1) **The ~5 cm consumer KPI holds at HEAD over a full day** — 7.6 cm rms / 6.0 cm p50,
and the UP channel is unbiased to 7 mm with 6.4 cm scatter. (2) The 18.6 h PARTIAL was not
optimistic: every like-for-like row agrees within a few per cent, so §6.16.5's conclusion survives
its own truncation. (3) The as-traced / ref-free split of §6.12 is reproduced verbatim on a
full-length run — L1C reads **54.1 % as-traced and 86.4 % ref-free**, a 32-point instrument
artifact, while the lanes (EWL/WL) barely move because the reference satellite's error is
lane-suppressed. (4) The P-record fractions sit AT the uniform null on all six (cst, sig) channels
(rms 0.277–0.301 cyc against 1/√12 = 0.289), i.e. §6.16.6's "the emitted wire carries no fractional
structure" is confirmed on the full run — and §6.16.7b says what that does and does not mean.

#### 6.16.7b Consumer AR on the quiet golden — 0 fixes in 2 879 attempts, and the blocker is the CONSUMER's own float

Arm A ran: `runs/golden_sim_consumer_ar_a` (`_golden_sim_consumer_ar_a.json`, the golden with the
consumer's `closed_loop.ar_mode` 0 → 1, `ratio_threshold` 3.0, partial AR, `hold_variance` 1e-3,
`logging.enable_ambiguity_trace`; **producer untouched**), quiet 24 h bed, 2 879/2 879 epochs.

| quantity | value | source |
|---|---|---|
| AR invoked / accepted | **2 879 / 0** (0.0 % fix rate), first fix NONE | console run-end block |
| gate that refused | `ratio_battery` on **2 596** of 2 879, `min_epochs` 10, `sigma_gate` the rest | `logs/closed_loop_trace.csv` |
| ILS ratio (rows that reached the battery) | p50 **1.01**, min 1.00, max **1.26**; hourly p50 **1.00–1.07 in all 24 hours** | same |
| candidates per epoch | p50 **20** (range 0–30) | same |
| position, settled h12–24 | 3D rms 0.0762 / p50 0.0604 m — **identical to the float baseline** | `grade/` of both runs |

**Why it cannot fix, measured with truth.** Joining the consumer's own float ambiguities
(`logs/ambiguity_trace.csv`: `week,tow_s,key,value_m,std_m`) to
`runs/simnet60_quiet_wu93_24h/SZZ000SIM/truth_ambiguities.csv` (one `N_int` + wavelength per
(sat, signal)) over the settled window h12–24:

| band | n | float σ, p50 (cyc) | med\|frac(e)\| vs truth (cyc) |
|---|---|---|---|
| L1C | 22 292 | **0.321** | 0.244 |
| L2W | 12 837 | **0.368** | 0.230 |
| L5Q | 22 292 | **0.422** | 0.251 |
| L7Q | 9 455 | **0.439** | 0.239 |

(The session record's "0.29–0.45 cyc" is the same quantity over its own window; the p50s above are
the recomputation.) **A per-band float σ of 0.32–0.44 cyc cannot produce a ratio > 3 no matter what
the wire carries** — the ILS search is choosing between near-equiprobable candidates, which is
exactly the observed ratio ≈ 1.0. The med|frac| column is at the uniform null (0.25) for the same
reason: with a free per-LOS slant ionosphere at the consumer and **no WL/NL cascade** (the
undifferenced consumer runs direct per-band LAMBDA; `ar_wl_only` is inert under
`consumer_undifferenced`), the per-band ambiguities are **undetermined by construction**.

**⇒ Two consequences, both binding.** (1) The 23:00 reading "the wire carries no integers" is **NOT
established either way** — this test cannot see the wire through a float that weak, and neither can
the P-record per-band SD-fraction grade (§6.16.6), which is confounded twice: the iono-direction
gauge AND the consumer's float weakness. (2) **The AR sequencing of §6.16.6 is corrected**: step 1 is
now the **consumer-side MW-WL → IF-NL cascade on the undifferenced model** (MW-WL is iono-free, σ ≈
0.05 cyc over hours), step 2 an anchor-aware TRUTH grader on the WL/IF combinations of the P records,
and only then network-side integer leveling (design-doc Phase 3). Producer DD-fixing (M5) stays
DO-NOT-BUILD. Arm B (`hold_variance` 1e-5) was killed as moot — the gate never reaches the hold.
Field note: `ar_mode 1` on the FLD63 chain also never fixes; the `_fldf3_pl2_arfloat` probe
(`ar_mode 0`) is **byte-identical in position** to its control (settled 3.178 rms / 2.418 p50 on
both, AR invoked 1 440 → 0), so the field consumer's AR is inert-in-effect, neither harm nor help.

#### 6.16.7c The FIELD consumer's residual is a slowly-varying VERTICAL wander that rides the producer's ZWD

All arms below share ONE wire (`runs/fldf3_pl2_ctl_new`, the converging leveling posture of §6.15.4,
DOY-129 00–12 UT, 61 stations); the probes differ only in consumer-side or log-only keys and their
`products.txt` is the control's.

**(i) The error is vertical, and it is not the phase-bias churn.** Settled (≥ h6, n = 720):
3D rms **3.178 m**, p50 2.418; E −0.28 ± 0.40, N +0.56 ± 1.18, **UP −2.018 ± 2.018 m** — the vertical
carries **80.7 %** of the settled sum of squares. Per-satellite churn census (scratchpad
`satchurn/`, runs `fldf3_pl2_ctl_new` + `_rtlog`):

| finding | number |
|---|---|
| consumer resets / 12 h, distinct (sat,signal) keys | **665 on 52 keys**, all on L1W/L2W (the signals WTZZ00DEU tracks) |
| wire PB bumps (adjacent-epoch \|ΔPB\| > 0.05 m in the P records) | 5 025 on 114 keys; **48.8 % (2 450) sit on L1C/L2X keys the consumer never sees** |
| producer PB-discontinuity counter bumps | 4 839 on 114 keys, max 280/key (state-snapshot `ep,` lines) |
| **eclipse re-admission link** | **3 of 665 resets (0.5 %)** within ±10 min of THAT satellite's own return; consumer error after returns 3.195 m vs 3.185 m elsewhere ⇒ **DEAD on the field** |
| clock-column departures with a return inside 3 900 s | 8 of 8 — the only population `readmit_seed_horizon_s` could act on, and `estimate_sat_code_bias` is OFF on this chain (the FALSIFIED clock-only variant) |
| coverage / elevation at bump epochs | median ratio (bump N)/(satellite median N) = **1.000**; 39 below / 23 equal / 52 above ⇒ no coverage signal |
| reset concentration | **52.2 % of all resets in one hour** (04:30–05:30, 7 satellites) = the run's peak prior tension, 1.3–2.1 h AFTER the returns |
| the counter-example | **h3: 1 reset (0.2 %) yet rms 4.325 m, mean UP −4.037 m** — the largest vertical hour has essentially no reset activity |

Window-exclusion of the top-3 satellites' (G12/G25/G24) reset windows takes the settled rms
3.178 → **1.496 m** on the 301 surviving epochs — but at **p(null ≤ obs) = 0.110** under a
circular-shift null that preserves the burst structure and destroys only its phase (200 draws; null
median Δ +0.154, 5–95 % [−1.739, +1.590]; scratchpad `satchurn/q5b.txt`). The same file's
variance-share cut says where the error actually is: **h6/h7/h8 carry 42.8 / 30.2 / 19.2 % of the
settled sum of squares** and h9–h11 carry 7.8 %; at h9–h11's level the settled rms would be 1.25 m.
⇒ **PB churn is at most secondary; the residual is the long-open consumer
vertical defect** (`project_closedloop_consumer_vertical_bug`).

**(ii) It is not the consumer's ionosphere and not its AR.** Three consumer-only probes on the
identical wire (settled ≥ h6): SH-VTEC slant feed OFF (`consume_network_iono: false`,
`_fldf3_pl2_noiono`) → **3.771 m rms** (WORSE — the feed helps, as the 305-sta record said);
iono-supply σ×3 (`estimation.ionosphere_weighting.std_scale` 1 → 3, `_fldf3_pl2_ionoscale3`) →
**3.721 m**, UP mean −2.447 (also worse, ≈ feed OFF ⇒ the honest σ is near-optimal); AR off
(`_fldf3_pl2_arfloat`) → **byte-identical** 3.178 / 2.418. The −2 m DC and the ±2 m wander survive
all three.

**(iii) It rides the producer's per-epoch ZWD.** From the consumer's own state series
(`_fldf3_pl2_statetrace`, log-only, `logs/state_trace.csv`; products identical to the control):

* the consumer's own ZWD state is **physical** — settled 0.414–0.572 m, sd **5.5 cm** —
* yet its height error tracks that state at **dUP/dZWD = −28.9 m/m, corr −0.785**, an order of
  magnitude above the classic ZWD⇄height trade ⇒ **a common EXTERNAL driver**, not the
  consumer's own tropo estimation;
* consumer UP vs the producer's **fleet-mean per-epoch ZWD deviation** (per-station deviation from
  its own settled median, meaned over the 61 stations; sd 2.241 m): **corr +0.842, slope +0.758 m/m**.

**(iv) Two leak regimes.** Per-station: **BSHM00ISR alone gives corr +0.850 at slope +0.0129 m/m**,
against 1/61 = 0.0164 — i.e. a sick station's per-epoch ZWD excursion enters the shared satellite
clocks/PBs at very nearly its weight in the solve and arrives at the consumer looking like tropo.
On the arms where BSHM's excursion is gone (`gapfree2`, `nosh`, `on2`) the top correlations move to
HEALTHY stations at slopes 10–60 m/m — a **network-wide common tropo-like wander** that every
station's ZWD and the consumer's height ride together, i.e. a weakly-determined per-epoch common
(ZWD ⇄ clock) direction with all ZWDs free every epoch. Same missing piece in both regimes: **a
temporal prior on the producer's ZWD** (§6.16.7d), plus a station-level response to the congruence
screen (§6.16.2's t = 2.2e6, measure-only today).

> ⚠ **(unsourced) — one figure in the session record does not reproduce.** "healthy-57 median ZWD
> wander sd 9 mm" could not be reproduced from `runs/fldf3_pl2_ctl_new/logs/network_zwd.csv` under
> any of four definitions of "wander" over the settled window, excluding BSHM/CUSV/COCO/BOR1:
> raw per-station sd of the deviation **71.4 mm** (p25 45 / p75 161), common-mode-removed sd
> 78.0 mm, MAD 32.8 mm, adjacent-epoch-difference sd 29.7 mm. The nearest mm-scale number on that
> file is the median per-station **formal σ, 7.7 mm**, which is a different quantity. Use 71 mm as
> the healthy-fleet wander until someone re-derives the 9 mm.

**(v) The logging trap that produced a false "0 consumer resets".** The FLD63 chain leaves
`logging.enable_rt_engine` at its loader default FALSE while the sim golden sets it true, so a field
console carries **no `[RT]` lines and `rt_engine.log` is 0 bytes** — measured: 0 bytes on
`fldf3_pl2_ctl_new` and `fldf3_pl2_zwdblk`, 480 717 bytes / 665 reset lines on `_rtlog` where the
logger is armed. **Any consumer-churn claim must come from `rt_engine.log` with the logger armed,
never from the console.** The log-independent cross-check is the emitter's own PB-discontinuity
counter in the state snapshot (4 839 bumps / 114 keys above).

#### 6.16.7d The ZWD-in-block card (`network.carry_zwd_in_block`, merged 966d4fd + 7b32a74) — four arms

Built to remove §7 item 17's double count: a **diagonal** B9 row asserts the station-common ZWD
direction a second time on top of the assertion every carried ambiguity row of that station already
makes implicitly; inside F3's jointly-whitened local block the harvested (N_rr⁻¹) cross terms
subtract the overlap exactly. Default OFF, OFF twin byte-identical (`products.txt` sha256
`47102044…` on both smoke arms), 10 unit tests. Full card:
`runs/golden_sim_campaign_zwdblk/VERDICT.md`.

**Arm Q — quiet 24 h (`runs/golden_sim_zwdblk_quiet`), the load-bearing negative control.**

| settled h12–24 | gate | **in-block** | uncarried, full 24 h (§6.16.7a) | uncarried, PARTIAL 18.6 h | **DIAGONAL `carry_zwd`** |
|---|---|---|---|---|---|
| consumer 3D rms | ≤ 0.10 m | **0.0877** | 0.0762 | 0.082 | **0.723** |
| consumer 3D p50 | ≤ 0.06 m | **0.0525** | 0.0604 | 0.051 | — |
| ZWD truth rms | ≤ 0.02 m | **0.0057** | 0.0072 | 0.0069 | — |
| per-sta settled ZWD span p50 / max | — | 0.032 / 0.054 m | — | 0.063 / 0.080 | — |

(The card's own VERDICT graded arm Q against the PARTIAL control, which did not cover h12–24; the
full 24 h baseline of §6.16.7a is the matched-window control and is added here. Against it the
in-block carry is 15 % worse on consumer rms, 13 % better on p50, and **21 % better on the ZWD
channel it targets** — the gate verdict is unchanged.)

Wiring: 172 320 blocks whitened, **172 320 ZWD columns, 0 diagonal fallbacks, 0 missing cross
terms**, 0 factorization fallbacks; `network_zwd.csv carried` = 172 320/172 740 = 99.8 % (the 420
uncarried rows are exactly 60 stations × the 7-epoch leveling warm-up). **⇒ Q1 PASS, and it is the
result that matters: what falsified B9 on the quiet bed was the DOUBLE COUNT, not the idea of
carrying ZWD** (0.723 m diagonal vs 0.088 m in-block, same bed, same carry).

**Arm Z — slipped campaign bed, 6 h (`runs/golden_sim_campaign_zwdblk`), single knob vs E0.**

| channel (settled) | E0 golden | **in-block** | Δ |
|---|---|---|---|
| per-sta settled ZWD span p50 | 1.590 m | **0.207 m** | **−87 %** (Z2 PASS) |
| ZWD settled truth rms | 2.098 m | **1.188 m** | −43 % (Z1 FAIL vs ≤ 0.5; falsifier 1.5 not reached) |
| stations with span > 1 m | 39/60 | 15/60 | −62 % |
| ZWD hourly rms h0..h5 | 2.10 1.37 1.84 1.95 2.12 2.21 | 1.98 1.31 0.96 0.58 1.05 1.67 | better in every hour |
| WL(1,2) / EWL(2,5) ref-free at own integer | 81.5 / 99.0 % | **86.2 / 99.2 %** | Z4 PASS |
| L1C ref-free at own integer | 39.1 % | 44.0 % | better |
| **sat-clock contrast rms** | 8.050 m | **11.700 m** | **+45 % (worse)** |
| iono⇄ZWD interaction rms | 2.456 m | 3.030 m | +23 % (worse) |
| consumer settled h3–h6 p50 / rms | 1.752 / 1.799 m | **1.085** / **2.389** m | Z3 FAIL (p50 −38 %, rms +33 %) |
| consumer hourly rms h0..h5 | 2.06 3.81 2.92 1.71 1.85 1.83 | 1.59 2.19 1.56 1.03 **2.55 3.09** | better h0–h3, worse h4–h5 |
| T4 one-solve arc refloats | 22 968 | **29 613** | +29 % |
| slip census DETECTED / RESPONDED / SILENT | 2337 / 72 / 77 | 2337 / 75 / 74 | flat (not a detection artifact) |

`raim_pool_zwd_rows` = **42 682 of 42 780 (99.8 %)** — §7 item 4's interaction, now MEASURED rather
than assumed: the post-fit RAIM pool admits a row by `|A_r(row, amb_col)| > 0.5`, a test written for
data phase rows, and a whitened ZWD block row clears it on essentially every station-epoch. This
card changes nothing about the pool; the number is published so the next card decides on data.

**Arm F — FIELD, 12 h (`runs/fldf3_pl2_zwdblk` vs `_ctl_new`).**

> ⚠ **THE FIELD ARM DID NOT MEASURE THE IN-BLOCK VEHICLE — the loader disabled the sub-knob and the
> run carried the DIAGONAL B9 row instead.** `config/test_suite/_fldf3_pl2_zwdblk.json` arms
> `carry_zwd` + `carry_zwd_in_block` on the **pl2** posture, which is the converging leveling vehicle
> **with F3 OFF**; the interlock at `GnssConfigLoadNetwork.cpp:8778-8784` therefore fired —
> *"carry_zwd_in_block is on but network.phase_leveling_local_block (F3) is OFF … DISABLING
> carry_zwd_in_block"* (verbatim in `runs/fldf3_pl2_zwdblk/console.txt`) — and the run shows **no
> "ZWD in block" run-end clause at all** while `network_zwd.csv carried` reads **87 392 / 87 819**.
> The interlock behaved exactly as designed (armed-and-inert is forbidden, invariant #4). **So the
> field numbers below grade `network.carry_zwd` (diagonal B9) on the converging leveling vehicle,
> not the in-block variant.** They are still a first: the diagonal carry DIVERGED on the shipped
> `carry_invariant` vehicle (§6.15.4 arm 0z) and does not diverge here. **The in-block field arm is
> still OWED and must run on a posture that arms F3 (`_fldf3_on2`-class), not pl2.**

| settled ≥ h6 | control `ctl_new` | **`zwdblk` (= diagonal `carry_zwd`)** | pre-registered |
|---|---|---|---|
| BSHM00ISR settled ZWD span | 368.87 m | **0.87 m** | < 5 m **PASS** |
| fleet settled span p50 / n > 2 m / n > 10 m | 0.797 m / 14 / 3 | **0.275 m / 2 / 0** | p50 < 0.3 **PASS** |
| consumer UP mean / sd / rms | −2.018 / 2.018 / **2.854** m | **−0.713** / 2.660 / 2.754 m | UP rms < 1.5 **FAIL** |
| consumer 3D rms / p50 | 3.178 / 2.418 m | 3.322 / 2.316 m | < 2.0 **FAIL** |
| corr(UP, BSHM ZWD deviation) | +0.850 | **−0.722** | < 0.4 **FAIL** (sign flipped, still coupled) |
| vertical share of settled 3D SS | 80.7 % | 68.7 % | — |
| consumer hourly rms h0..h11 | 3.78 2.31 2.02 4.33 2.35 3.64 5.09 4.28 3.42 0.79 1.24 1.59 | 2.96 1.48 1.17 2.43 2.61 **7.36** 4.75 **5.14** 2.48 0.62 1.61 2.84 | — |
| **T3c consensus flags / opportunities** | 1 105 / 15 763 | **7 619 / 29 795** (6.9×) | reported |
| T3 post-fit cell flags | 22 713 / 665 332 | **60 222 / 665 338** (2.7×) | reported |
| T4 ARC attribution flags | 2 032 | 2 723 | reported |
| T4 one-solve arc refloats (run-end census) | 3 899 | 5 156 | reported |

**Arm F2 — FIELD IN-BLOCK on the F3 posture, 12 h (`runs/fldf3_on2_zwdblk` vs `runs/fldf3_on2`).**
The arm Arm F said was still owed. Base = `_fldf3_on2`, the F3-armed converging field leveling posture (pl2 +
`network.phase_leveling_local_block`, §6.15.4 second round); the arm adds `network.carry_zwd: true`
+ `network.carry_zwd_in_block: true` and **nothing else** — a flattened sorted-JSON diff of the two
configs differs on exactly those two keys plus the two run paths and the header comment. DOY-129,
63 stations, 12 h (tow 259 200–302 370), WTZZ closed-loop consumer, exit 0 both.

| settled ≥ h6 | control `fldf3_on2` | **`fldf3_on2_zwdblk` (in-block)** | pre-registered |
|---|---|---|---|
| fleet settled ZWD span p50 | 1.195 m | **0.143 m** | p50 < 0.3 **PASS** |
| fleet n > 2 m / n > 10 m | 19 / 4 | **3 / 0** | no station > 10 m **PASS** |
| worst station span | 491.51 m (INEG00MEX) | **5.12 m** (BSHM00ISR) | — |
| BSHM00ISR settled ZWD span | 34.85 m | **5.12 m** | — |
| consumer UP mean / sd / rms | **+2.834** / 2.388 / 3.706 m | **+0.072** / 1.861 / **1.862** m | \|mean\| < 1.0 **PASS**; rms < 1.5 **FAIL** (1.86, close) |
| consumer 3D rms / p50 | 4.034 / 3.058 m | **2.926 / 2.375 m** (−27 % / −22 %) | < 2.0 **FAIL** |
| consumer E rms / N rms | 1.009 / 1.233 m | 1.364 / 1.798 m | — (**worse**, +35 % / +46 %) |
| corr(UP, BSHM ZWD deviation) | −0.161 | **+0.058** | < 0.4 **PASS** |
| vertical share of settled 3D SS | 84.4 % | **40.5 %** | — |
| T3 post-fit cell flags | 15 435 / 663 738 (2.33e-2) | **10 717 / 665 335 (1.61e-2)** | reported |
| **T3c consensus flags / opportunities** | 1 038 / 9 000 (1.15e-1) | **849 / 5 379** (1.58e-1) | reported |
| T4 ARC attribution flags | 1 187 (1.79e-3) | **952** (1.43e-3) | reported |
| T4 one-solve arc refloats (run-end census) | 2 159 | **1 750** | reported |
| within-epoch trigger (T2) flags | 41 | **2** | reported |
| consumer hourly rms h0..h11 | 3.83 1.63 1.90 3.17 4.06 2.33 2.98 **6.78** 3.67 3.59 2.39 3.28 | 5.34 2.49 1.27 1.55 2.52 3.23 4.78 2.25 1.95 2.83 2.12 2.67 | — |

Wiring, verbatim from `runs/fldf3_on2_zwdblk/console.txt` — *"Network (leveling local-block F3):
87313 block(s) whitened … **ZWD in block: 87313 col(s), 98 diagonal fallback(s), 297 missing cross
term(s), 87274 RAIM-pool admission(s) of the ZWD row**"* — i.e. the vehicle actually ran this time:
87 313/87 313 columns in the block, 0.11 % diagonal fallback, and the F3 interlock at
`GnssConfigLoadNetwork.cpp:8778-8784` is **silent**. `network_zwd.csv carried` = 87 392/87 819 =
99.5 % against the control's 0/87 608 (`carry_zwd` OFF there). Re-derived with
`scripts/field_consumer_zwd_grade.py <run> --settle-h 6`; every headline number above is that
script's output, not the console's.

⚠ **Not a same-build A/B.** The control's exe is from the `00d5fdf` worktree, the arm's is the HEAD
build after merge `7b32a74` (`gnss_processor_zwdblk_main.exe`, sha256 `319a6aed…`); the OFF-path
differences between them are byte-certified by the merge smokes, not by this pair. A same-build
control `runs/fldf3_on2_ctl_sb` is in flight and will be appended here.

⚠ **The T3c count fell but its RATE rose** (1.15e-1 → 1.58e-1) because opportunities nearly halved
(9 000 → 5 379). A T3c opportunity is a `(tow, satellite)` with at least one spiked cell, so the
halving *is* the quieting — 31 % fewer spiked cells over the same cell population — and the higher
rate says only that, conditional on a satellite spiking anywhere, more stations now agree. Both
readings point the same way; quote the counts with the opportunities attached.

**Reading, after four arms — one mechanism, and one inference that Arm F2 retracts.**
* **On its own channel the prior works, on every bed and every vehicle.** Quiet sim: ZWD truth
  5.7 mm and the diagonal regression NOT reproduced. Slipped sim: span −87 %, truth −43 %, integers
  up. Field, diagonal (F): the sick station can no longer park corrupt phase in its own ZWD
  (368.87 → 0.87 m). Field, in-block (F2): fleet span p50 1.195 → 0.143 m, the > 10 m tail empties
  (4 → 0), and the worst station in the fleet goes 491.5 m → 5.1 m. Four arms, two beds, three
  vehicles, same direction.
* **The "it RELOCATES the tension into a per-SATELLITE misattribution storm" inference was drawn
  from the DIAGONAL field arm, and Arm F2 does not reproduce it.** Arm F ran `carry_zwd` with the
  in-block sub-knob loader-disabled, so its T3c 6.9× / T3 2.7× storm is the **diagonal** row's — the
  double count of §7 item 17, asserting the station-common ZWD direction a second time on top of
  every carried ambiguity row. On the in-block F3 vehicle every QC tier goes the other way: T3 cells
  −31 %, T3c 1 038 → 849, T4a 1 187 → 952, T4 one-solve refloats 2 159 → 1 750, T2 41 → 2 — **quieter
  on the same field data**, while the consumer improves 27 % on 3D rms and the vertical DC bias
  (UP mean +2.83 m) goes to **+0.07 m**, with corr(UP, BSHM ZWD deviation) −0.16 → +0.06 and the
  vertical share of the settled 3D sum-of-squares 84.4 % → 40.5 %. So on the field there is **no
  storm to bound**: the in-block vehicle removes the vertical DC bias and quiets the QC at the same
  time. What still stands, unretracted, is the SIM-BED cost — the slipped campaign bed relocates
  into the satellite clock (contrast 8.05 → 11.70 m, iono⇄ZWD 2.46 → 3.03 m) — and that relocation
  is bed-specific evidence, not a field prediction.
* **What remains on the field is a 1.86 m UP wander plus ~1–1.8 m N/E, and it is unattributed.**
  The vertical is now a zero-mean wander (sd 1.861 m ⇒ rms 1.862 m) rather than a bias, which is why
  the pre-registered UP rms < 1.5 gate misses by 0.36 m and consumer 3D < 2.0 misses at 2.93 m.
  ⚠ The horizontal got **worse**, not flat: E rms 1.009 → 1.364 m, N rms 1.233 → 1.798 m, with the
  component means moving too (E −0.31 → −0.57, N +0.29 → +0.84). Pinning the wet delay took the
  vertical's dominance away (84.4 % → 40.5 % of the settled SS) and left a residual that is no
  longer mostly vertical — so the next lever must be graded on all three components, not on UP.
  Candidate levers, in the order the evidence supports them: **bounded identification** with a
  station-level quarantine response (§7 items 22/26 — still motivated, but by E1 and by the sim
  clock relocation, not by a field misattribution storm); a **young-arc age gate inside the block**
  (`network.phase_leveling_local_block_min_age_ep` > 0, the named-but-unrun sim arm); and a
  **level-preserving re-level** so the erases that drive the leak stop creating fresh code-level
  birth error in the first place.
* ⇒ **`network.carry_zwd_in_block` is promoted to RECOMMEND_SCOPED in regime `carry_zwd` AND
  `phase_leveling_local_block` (ledger, 2026-09-10):** whenever `carry_zwd` is armed on an F3
  lineage the in-block vehicle is **mandatory**, because the diagonal row under F3 is the falsified
  double count (quiet sim 0.723 m vs 0.0877 m; field diagonal T3c 7 619 vs in-block 849). The
  **pair** (`carry_zwd` + `carry_zwd_in_block`) is a RECOMMEND_SCOPED **candidate** for
  `phase_leveling` + F3 lineages — sim quiet consumer rms +7 % / p50 −13 % / ZWD −21 %, field F3
  consumer −27 % with the vertical DC bias removed — whose **golden pin awaits the user's decision**;
  the loader now emits a RECOMMEND line for the `carry_zwd` + F3 + in-block-OFF combination
  (`GnssConfigLoadNetwork.cpp`, log-only). `carry_zwd` itself stays **KEEP_OFF for the DIAGONAL
  vehicle** on every lineage.

#### 6.16.7e `code_datum_time_linked` OFF — the co-armed "inconsistent gauge" pair costs nothing measurable, and time-linking is load-bearing

`runs/golden_sim_tlink_off` = the sim golden with **`network.code_datum_time_linked: false`** plus
the trace-only `network.cycle_slip.gap_instrument: true` (flattened config diff run for this section:
those two keys, the output path and an `_comment_arm` are the only differences), same exe `gnss_processor_golden0.exe`, quiet 24 h
bed, 2 879/2 879 epochs, exit 0. Control = `runs/golden_sim_tri_w6_full` (§6.16.7a).

| settled h12–24 | ON (golden) | **OFF** | Δ |
|---|---|---|---|
| consumer 3D rms / p50 / max (m) | 0.0762 / 0.0604 / 0.2073 | **0.1013 / 0.0749 / 0.2708** | **+33 % rms (WORSE)** |
| consumer UP rms (m) | 0.0644 | 0.0862 | +34 % |
| sat code-bias residual rms / p50 / max (m) | 0.1482 / 0.0961 / 0.5231 | **0.4182** / 0.0146 / **3.7038** | rms 2.8× worse, p50 6.6× better |
| … per signal, worst two | E:C7Q 0.1376, G:C5Q 0.1578 | **E:C7Q 0.3092, G:C5Q 0.9022** | the tail is two signals |
| sum-to-zero datum check (emitted per-channel mean / rms) | +1.92e-2 / 6.23e-2 m | −2.59e-4 / 2.08e-2 m | OFF holds the datum tighter |
| sat clock residual rms (m) | 1.5005 | 1.5100 | flat |
| ZWD residual rms (m) | 0.0072 | 0.0073 | flat |

**Reading.** The two arms split cleanly: **ON carries a bias-like offset** (per-channel mean +1.9 cm,
p50 9.6 cm, max 0.52 m — a level the sum-to-zero datum does not hold at exactly), **OFF carries a
heavy tail** (p50 1.5 cm but max 3.70 m, concentrated on E:C7Q and G:C5Q). The consumer prefers ON by
a third on rms. **⇒ the §2.1 "inconsistent gauge" pair (time-linked s2z RHS + the primary GF code-bias
pin) injects NO measurable tension on this bed, and time-linking is load-bearing for the consumer:
keep it armed, and the loader WARN stays informational.** `SPEC_family_gf_datum` §2.1's "quantifying
it on a real arm is owed work" is CLOSED by this arm (see that document's §2.1 addendum).

**Rider, from the same arm's Card-G instrumentation.** The quiet bed reads **2 376 resumptions =
2 308 SHIFTED + 68 unshifted + 0 unmeasurable**, and of the shifted arcs **0 re-entered bridged of
4 616** — every one was fresh. ⇒ **the gap-poison class of `SPEC_slip_qc_hardening` §12 does not
exist on the simulator** (its only gaps are eclipse returns, far beyond the 120 s loader-default
leveling horizon), which is why that card is field-only until the simulator gets a dropout scenario.

---

