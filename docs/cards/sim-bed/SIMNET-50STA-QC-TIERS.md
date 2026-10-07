# G-A 50-sta: the 50-station global arm, the QC tiers and the MW null space

**STATUS**

- **id:** G-A 50-sta
- **topic:** sim-bed
- **status:** closed
- **summary:** At 50 stations 4 of 7 QC tiers go live; the datum station had NO slip detection (proven), fixed by `cycle_slip.datum_station_detect` at zero availability cost; MW mid-arc silence left OPEN.
- **dates in the text:** 2026-08-11
- **knobs / posture:** `network.clock_model`, `network.phase_datum_prioritized`, `network.trace_slip_raw`, `network.cycle_slip.if_phase_td`, `network.trace_slip_events`, `network.cycle_slip.datum_station_detect` (landed default OFF)
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** `runs/simnet50_mwblind2`
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md` §9.7 (9.7.1 to 9.7.10)

**Outcome.** The card asked what a 50-station global roster adds to the n = 3 / 5 grade. The per-satellite clock error converges (5.43 to 0.69 m) while a pivot-relative common level wanders ~1.3 m at every n, and T3 fires 3.7 million times its implied α on clean data. Slips at the sim rate inflate the per-satellite clock 7.6x and leave the common level untouched. IF-TD, once armed, covers the MW null space. The [ledger](../../CONFIG_promotion_ledger.md) records `datum_station_detect` as sim-validated only and runtime-inert under the prioritized datum that the [re-founding goldens](../../refound/RF-3-prep_golden_rebase_2026-09-15.md) pin.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
## §9.7 The 50-STATION GLOBAL arm — the vf trend and the QC tiers (2026-08-11)

Fixture `config/sim/simnet50_ga.json` (roster from `scripts/simnet_make_roster.py`, 50 solve
+ 1 consumer, Fibonacci lattice, \|lat\| ≤ 72°), 240 epochs = 2 h, GPS-only, quiet.

**Geometry certified before any arm ran** (§2.1): bipartite components **k = 1 at every
epoch**, 50/50 stations present, all **32** GPS satellites visible each epoch, **minimum 12
stations per satellite** against a hardcoded `kMinStationsPerSat` = 2, **zero**
satellite-epochs dropped.  A global roster is where fragmentation would be expected; it does
not occur at this size.

Two arms: **matched** (`_simnet50_matched.json`) — the SAME minimal posture as n = 3 / 5, so
the vf trend is one knob (station count); **armed** (`_simnet50_ga.json`) — the full
standard armed set, so the QC tiers have opportunities.  Their postures are NOT comparable.

### 9.7.1 The vf trend — and the two channels diverge

Matched posture throughout, so this is station count alone:

| channel | n = 3 | n = 5 | **n = 50** |
|---|---|---|---|
| C residual rms | 5.517 m | 2.183 m | **1.486 m** |
| C **vf** | 768.2 | 392.3 | **455.2** |
| ZWD residual mean | +4.307 m | +0.029 m | **+0.007 m** |
| ZWD residual rms | 4.558 m | 1.056 m | **0.896 m** |
| ZWD **vf** | 87.5 | 6.5 | **1.63** |
| consumer 3D rms | 3.645 m | 3.098 m | — |

> ⚠️ **AMENDED 2026-08-11 by adversarial review — the clock row's SHAPE is not readable.**
> The n = 50 clock vf is **78 % of its variance in ONE common direction**.  Settled C
> residual as fleet-common (vs pivot) ⊕ per-satellite: n=3 5.517 = 0.974 ⊕ 5.428, n=5
> 2.183 = 1.342 ⊕ 1.714, n=50 1.486 = **1.315 ⊕ 0.685** m rms.  The **per-satellite error
> DOES converge** with n (5.43 → 0.69); what does not is a pivot-relative common level
> wandering ~1.3 m rms at every n (ties to the CB-Q "wire drifts in unpinned directions"
> finding).  Median row vf at n=50 is **15.5**, not the mean 455 — the mean is a
> three-satellite tail (G31/G28/G03 drifting −0.2 → −2.4 m *together*).  Effective sample
> size for the common direction is ≈ 1, so **"455 vs 392 = a plateau" is realization noise
> and must not be read as a shape.**  vf is still GROWING at 2 h at every n.
>
> ⚠️ **The ZWD "vf 1.63 = calibrated" row needs its asterisk**: truth is a 0.12 m
> near-constant (±0.055 m fleet spread) while `std_m` ≈ 0.79 m, so the channel is honestly
> UNINFORMATIVE — ~30× worse than just predicting the constant.  The 87.5 → 6.5 → 1.63 trend
> is numerator-driven (the error shrinking), not a σ that became trustworthy.

**The two channels behave completely differently, and that is the finding.**  ZWD converges
to CALIBRATED (87.5 → 6.5 → **1.63**): its n = 3 mis-calibration was a determinability
artifact that disappears with scale.  The satellite clock does NOT (768 → 392 → **455**):
accuracy improves (5.52 → 1.49 m rms) but the formal σ shrinks just as fast, so it stays
**~21× too optimistic in σ** at every scale tested.  A channel whose σ is structurally
optimistic *independent of network size* is an estimator property, not a small-network
artifact; the clock channel is such a channel, and whether the code-bias channel is one too
still requires the W6 arm.

### 9.7.2 The QC tiers — 0/7 live at n = 3, 4/7 at n = 50

Armed arm, quiet data (no injected faults), 232/240 epochs emitted, **0 withheld by the rank
gate**, 8 by the A4 σ̂₀ ceiling.  78.5 ms/epoch, 50 stations, PARALLEL/20 threads.

| tier | opp | flag | observed rate | implied α | verdict |
|---|---|---|---|---|---|
| IF-phase slip (T1) | **0** | 0 | — | 1.24e-15 | INERT — needs `cycle_slip.if_phase_td` |
| within-ep trigger (T2) | 226 | 7 | 3.10e-02 | 2.70e-03 | **live, 11× its implied α** |
| post-fit RAIM cell (T3) | 103 347 | 747 | 7.23e-03 | 1.97e-09 | **live, 3.7 MILLION× its implied α** |
| RAIM consensus (T3) | 724 | 0 | 0 | 1.97e-09 | live, silent (correct on a quiet arm) |
| attribution SAT (T4) | 7 135 | 0 | 0 | 1.97e-09 | live, silent (correct) |
| clock-model NIS (T5) | **0** | 0 | — | 1.97e-09 | INERT — needs `network.clock_model` |
| clock-monitor (T6) | **0** | 0 | — | — | INERT — GPS-only degenerates its magnitude test |

Events on the same quiet arm: `T0_mw_detect / mw_wl_slip` **44**, `T2_within_epoch /
we_refloat_arc` 13 + `we_refloat_sat` 9, `T4_attrib / attrib_arc` 13, `SIGMA0 / withhold` 8,
plus 326 topology `row_kinds`.

| # | finding |
|---|---|
| 1 | **Station count is what makes the QC stack live.**  0 of 7 tiers had a single opportunity at n = 3; 4 of 7 do at n = 50.  T2/T3/T4 need cross-station populations a minimum network cannot supply |
| 2 | **The post-fit RAIM cell tier fires 3.7 × 10⁶ times its own implied Gaussian α on clean synthetic data** — the SAME finding as the clock vf, a second way: vf ≈ 455 means σ is ~21× too small, and a residual 21× larger than its σ crosses a k = 6 threshold constantly.  Two instruments agreeing is what makes it a result |
| 3 | **44 false MW slip detections** on an arm with `rate_base = 0` — no slip injected anywhere.  ~1.5e-4 false alarms per station-satellite-epoch |
| 4 | **Three tiers remain inert even at 50 stations**, each for a *named* reason: T1 needs `cycle_slip.if_phase_td`, T5/T6 need `network.clock_model`.  Arming those is the next arm, not a re-run |
| 5 | **`estimate_iono_sh` cannot simply be added.**  It co-estimates the SH-VTEC field JOINTLY WITH THE SATELLITE CODE BIASES — the whole coupled W6 slice — and the realized datum then no longer matches `declared_dimension`: the rank gate withheld **all 240 epochs** and the producer emitted nothing.  **`sh_health_gate` is not gradeable by this fixture** until the W6 slice is armed as a whole |

⚠️ **The armed arm is NOT posture-comparable to the matched one**, for two independent
reasons.  *Posture*: its ZWD vf is **3466** against the matched arm's **1.63** while its ZWD
rms is *better* (0.659 vs 0.896 m) — the armed set makes the estimate more accurate and its σ
three orders of magnitude too small; `phase_leveling` is the prime suspect but the arms
differ by many knobs, so **a bisect is owed before that is stated as a fact.**  *σ-scale*:
the published URA is `s0 · sqrt(scov)` with `s0 = max(1.0, σ̂₀_EWMA)`
(`NetworkSolPrep.cpp:430,443`), and that EWMA is structurally unavailable unless `emit_ura`
+ `raim` + `phase_leveling` are ALL armed (the RTB-6 sentinel):

| arm | RTB-6 sentinel | σ̂₀ EWMA | **s0** | published URA |
|---|---|---|---|---|
| n = 3 / 5 / 50 **matched** | present | unavailable | **1.000** | the pure marginal formal σ |
| n = 50 **armed** | absent | **1.674** | **1.674** | formal σ inflated 1.674× |

⇒ **the whole matched trend (768 → 392 → 455) is on the pure formal σ and is directly
comparable.**  The armed arm's clock vf of 7866 is *deflated* by s0² = 2.80 relative to the
same measure — on the pure formal σ it would be ≈ 2.2 × 10⁴.  The reported figure is
CONSERVATIVE and no conclusion's direction changes, but the two must never be pooled.  Any
future arm that flips `phase_leveling` moves this scale silently.

---

### 9.7.3 QC DETECTION — quiet vs fault, one knob apart

`config/sim/simnet50_qcfault.json`, identical roster and producer posture to the armed quiet
arm; the only difference is the injected faults, each verified present in the emitted RINEX
before grading:

| fault | where | magnitude IN THE DATA | detected? | by what |
|---|---|---|---|---|
| cycle slip, unflagged | S01 / G26 / L1C, ep 60, 19° elev | **+12.26 widelane cycles** (MW from the RINEX) | **MISSED** | nothing |
| cycle slip, unflagged | S02 / G06 / L2W, ep 90, 44° elev | **+6.70 widelane cycles** | partial | `T4_attrib attrib_arc` only — no slip event |
| cycle slip, **LLI-flagged** | S03 / G19 / L1C, ep 160, 22° elev | +45 cyc, LLI = 1 | **YES** | `T0_mw_detect lli_slip` |
| satellite clock step | G14, ep 120, all stations | +10 m | **YES, and MARKED** | `CLK_STEP mark` + `T4_attrib attrib_arc` |

(detection columns from `network_events.csv` within ±10 epochs of each injection.)
Per-tier flag counts, quiet → fault: T1 inert (0 opp both); T2 7 → 7 (Δ 0); **T3 cell
747 → 603 (Δ −144)**; **T3 RAIM consensus 0 → 29 (+29 — responded)**; T4 0 → 0; T5/T6 inert.

| # | finding |
|---|---|
| 1 | **The clock-step marking gap from §9.6 is CLOSED.**  With `emit_clock_disc` + `clock_disc_jump_test` armed the 10 m step produces a `CLK_STEP mark`; §9.6's zero marks were an unarmed marker, exactly as that section said |
| 2 | **RAIM consensus is the tier that works** — 0 → 29 flags, the only tier whose flag count responds to the faults at all.  It needs the cross-station population n = 3/5 cannot supply: the case for the 50-station variant in one line |
| 3 | ⚠️ **`mw_detect` missed both unflagged slips — and they were not subtle.**  The MW widelane jumps are **+12.26** and **+6.70 cycles** from the emitted RINEX and MW noise is sub-cycle, so these are the easiest targets a widelane detector has.  The detector was demonstrably LIVE (44 `mw_wl_slip` events elsewhere on the same data) and the one slip it did catch was caught by the receiver's own **LLI flag**, not the MW statistic.  On this arm: **44 false alarms, 0 of 2 true detections, and the only true catch came from a flag the receiver supplied.**  Chased in §9.7.4–§9.7.5 |
| 4 | **T4 attribution never flags.**  7135 opportunities, 0 flags in both arms, including for a 10 m satellite clock step seen by all 50 stations — the exact fault class it exists to attribute.  It emits `attrib_arc` events but no flags.  (⚠️ CORRECTED by §9.7.10: an INSTRUMENT gap, not a dead tier) |
| 5 | **The post-fit RAIM cell tier's flag count goes DOWN with faults added** (747 → 603): with a false-alarm population that large a real fault just reshuffles which cells exceed — consistent with finding 2 of §9.7.2 |

### 9.7.4 CHASING THE mw_detect MISS — one defect PROVEN, one still open

Three hypotheses, each a one-knob A/B on the same fixture data:

| # | hypothesis | test | result |
|---|---|---|---|
| 1 | the rounding-cleanliness gate is too tight (`turbo_edit_frac_max` = 0.15, default) | 0.15 → **0.49** | **FALSIFIED** — 44 → 66 events, still only at epochs **2 and 4**; nothing at 60/90/160 |
| 2 | no BIA ⇒ no BSSD integer WL lock ⇒ `slip_wl_known` never true (`has_bia ? "BSSD-pass2" : "direct-no-BIA"`, MwSlipDetector.cpp:2696) | `enable_code_phase_bias` false → **true** | **FALSIFIED** — bit-for-bit the same 44 events at epoch 2, same misses |
| 3 | **station index 0 is structurally excluded from slip detection** | roster reordered | **PROVEN** — below |

#### The proven defect: the datum station gets NO slip detection at all

```cpp
const int i_lo = phase_datum_prioritized ? 0 : 1;   // ApplicationNetworkAssembly.cpp:1281
```

The knob is **`network.phase_datum_prioritized`**, default **false**
(`GnssConfigLoadNetwork.cpp:4240`) — a knob in its own right, NOT `phase_datum_eq34` (an
earlier draft misattributed it; the `i_lo` proof is unaffected, the named remedy was wrong).
With it false `i_lo = 1`, and every slip-detection loop in `NetworkSlipQc.cpp` (lines 69,
169, 385 — MW **and** LLI) runs `for (int i = i_lo; ...)`, so **`stations[0]` is skipped
entirely.**  *Controlled proof*, identical fixture data, ONLY roster order changed: with
`S03000SIM` at index 2 its G19 LLI slip is **detected** (`T0_mw_detect / lli_slip`, 1 event
in the run); moved to **index 0** it is **MISSED** (**0** events).

The in-code rationale — "Pivot (stations[0]) is never touched (its ambiguities are
gauge-pinned)" — is sound as far as it goes, but the consequence is not benign: the datum
station is the receiver whose ambiguity rides inside every emitted phase bias (contract
§B.2), so an undetected slip there re-references the P record for that satellite **across
the whole network**, and nothing in the run records it — not the flag-rate summary, not the
event log, not a counter.  Standing invariant #1 in its exact form, at the one station where
it matters most.  Fixed in §9.7.10 (`cycle_slip.datum_station_detect`).

#### Still open: MW produces no mid-arc candidates at ANY station

`i_lo` explains S01 (index 0) completely.  It does **not** explain S02 (index 1, −7 cyc
unflagged, 44° elevation), which sat inside the loop and was still missed by MW; only
`T4_attrib / attrib_arc` fired.  Across both fault arms every one of the 44 `mw_wl_slip`
events is at **epoch 2**, and the frac_max arm adds epoch 4 — the MW path emits candidates
only during ignition, then nothing for 238 epochs.  Hypotheses 1 and 2 are eliminated; the
remaining candidates are the arc-maturity / Welford noise-floor state and the BSSD MW
pivot's own resolution (`slip_wl_known` is documented false "if no pivot could be made — MW
pivot slipped with an unresolved integer").  Discriminating instrument:
`network.trace_slip_raw`, run, answered in §9.7.5.

### 9.7.5 RESOLVED — the detector SAW the jump and formed no candidate

`network.trace_slip_raw` armed (`network_slip_raw.csv` publishes per-(station,sat) raw MW /
GF / phase-minus-code / LLI every epoch from `raw_observed`, **independently of whether any
detector is enabled**).  At the injected epochs, against each arc's own robust
epoch-to-epoch scatter (1.4826·MAD):

| station / sat | MW jump | cycles | **vs arc scatter** | LLI | detected |
|---|---|---|---|---|---|
| **S02 / G06** (idx 1) | +5.760 m | **+6.68** | **13.3×** | 0/0 | **MISSED** |
| S03 / G19 (idx 2) | +38.821 m | +45.04 | 51.9× | **1**/0 | caught — by **LLI only** |
| S01 / G26 (idx 0) | +10.421 m | +12.09 | 7.7× | 0/0 | MISSED (`i_lo` blind spot) |

**This closes §9.7.4's open question.**  The jump is in the producer's own trace, at the
exact epoch, at 13× the arc's own scatter, with a clean LLI — and the detector emitted no
candidate.  The failure is in candidate FORMATION, not the observable.  With the two
falsified hypotheses (frac gate, BIA), the remaining surface is the BSSD/integer-lock path.

### 9.7.6 The MW NULL SPACE — measured, and RAIM does not currently cover it

The MW widelane is N₁ − N₂, so a slip with **ΔN₁ = ΔN₂ changes it by exactly zero** —
structurally invisible to any MW-only detector at any threshold.  A paired arm
(`config/sim/simnet50_mwblind.json`) puts a (+2,+2) and a (+2,0) slip on the **same
satellite, same epoch, same magnitude**, so the only difference is whether the pair lies in
MW's null space.  Verified in the emitted RINEX:

| slip | ΔWL measured | ΔWL predicted | ΔGF measured | ΔGF predicted |
|---|---|---|---|---|
| (+2,+2) | **−0.019 cyc** | **0 exactly** | −0.142 m | n(λ₁−λ₂) = −0.108 m |
| (+2, 0) | +2.199 cyc | +2 | +0.397 m | nλ₁ = +0.381 m |

The pair IS observable — ΔGF = −10.8 cm, and the iono-free phase moves
n·c/(f₁+f₂) = **+21.4 cm** — just not by MW: the argument for a geometry-based backstop
rather than a better MW gate.  **But the backstop does not currently fire.**  QC response to
the pair vs the quiet baseline: T1 0 (0 opp) both; T2 7 → 7; **T3 cell 747 → 743** (−4,
noise); **RAIM consensus 0 → 0**; T4 0 → 0; T5/T6 0 opp both.  **A 2-cycle slip pair — 38 cm
on L1, 21.4 cm in the iono-free phase — passes through all seven tiers untouched.**  Neither
the MW-blind (+2,+2) nor the MW-visible (+2,0) control produced a slip event; the visible one
raised only a `T4_attrib / attrib_arc`.  RAIM consensus, which DID respond to the 10 m clock
step (§9.7.3, 0 → 29), does not respond at this magnitude.  So "MW has a null space,
therefore we need RAIM" is a **requirement**, not a description of the built system.

⚠️ **CORRECTED by §9.7.10 correction 3: this was an UNARMED-TIER reading.**  T1
(`network.cycle_slip.if_phase_td`) was off in every §9.7 arm.  Armed, it catches both G11
slips at their exact injection epochs with zero false alarms.  §15.3's `--signal-mode
mw-blind` generator makes this discriminator reproducible on demand.

### 9.7.7 The remedy is real but NOT free — do not make it standard yet

`network.phase_datum_prioritized: true` sets `i_lo = 0`.  One-knob arm off the fault fixture:

| | default (`i_lo`=1) | **armed** (`i_lo`=0) |
|---|---|---|
| station-0 T0 events | **0** | **5** |
| run `mw_wl_slip` total | 44 | 49 |
| S01/G26 slip | MISSED | **still MISSED** |
| **epochs emitted** | **232 / 240** | **143 / 240** |
| withheld by A4 σ̂₀ ceiling | 8 | **97** |

Two conclusions pointing opposite ways: it **confirms the `i_lo` mechanism a second,
independent way** (station 0 goes 0 → 5 T0 events on this knob alone), and it **costs 97 of
240 epochs** (40 % of the product) to the A4 σ̂₀ ceiling against 8 at default without
recovering the S01 slip — the §9.7.5 candidate-formation failure applies at station 0 too,
so **`i_lo` was necessary, not sufficient.**

⇒ **Arming it as a standing default is NOT justified by this evidence.**  SCOPED ledger,
validated-but-not-promoted, cost recorded (loader defaults stay off; each new config arms its
set explicitly).  The blind spot is fixed instead by §9.7.10's
`cycle_slip.datum_station_detect`, at **zero availability cost**.

### 9.7.8 ⚠️ CORRECTION — RAIM was NOT blind.  The consensus rule vetoed it.

**§9.7.3 and §9.7.6 graded against the wrong instrument.**  `network_events.csv` does not
record RAIM refloats, so "MISSED" there meant "no event row".  `network_raim.csv`
(`network.trace_slip_events`) carries the actual decision chain and says the opposite:

| slip | `s_max_norm` | `raim_k` | `spiked` | `n_sta_spiked` | `min_sta` | `refloated` |
|---|---|---|---|---|---|---|
| S02/G06, −7 cyc | **58.2** | 6 | **1** | **3** | 3 | **1 — acted** |
| S03/G11, +2 cyc | **9.4** | 6 | **1** | **1** | 3 | **0 — vetoed** |

RAIM **detected both, comfortably.**  The failure is in the ACTION rule:

```cpp
if (kv.second >= raim_min_sta) flagged.push_back(kv.first);   // NetworkRaim.cpp:266
```

The count is keyed by **satellite**, counting how many *stations* spiked on it, because the
tier attributes **satellite** faults.  **A cycle slip is a receiver event, so exactly one
station can ever spike** — `n_sta_spiked = 1 < min_sta = 3`, at any magnitude.  MW-blind arm
run totals: **743 spiked, 0 refloated**.  The −7 cyc slip got through only by reaching
`n_sta_spiked` = 3 = exactly the minimum, the slip having perturbed the SHARED satellite
parameters enough to push two other stations' G06 residuals over k — contamination
masquerading as consensus.

**`raim_min_stations` is not a knob to lower** — it exists to stop the tier attributing every
station's noise to a satellite.  The fix is a separate station-local attribution path (same
statistic, different consensus logic).  `raim_attribution` (T4) reporting **7135
opportunities and 0 flags in every arm** is suspicious in the same direction — checked in
§9.7.10, which corrects both the "100 % detection / 0 % action" framing and the T4 reading.

### 9.7.9 HOW MUCH DO SLIPS CONTAMINATE?  Measured — and they are ORTHOGONAL to the level problem

One knob off the quiet 50-station arm: `cycle_slips.rate_base` 0 → 0.0002 (the sim default),
identical fixture, identical per-station seeds.  **465 slips across all 51 stations, |Δ| 13 …
52 (median) … 123 cycles.**  At realistic magnitudes the QC stack DOES act — RAIM 2707 spiked
/ **2751 refloated** (vs 0 at 2 cycles), consensus flags 0 → 191, `mw_wl_slip` 44 → 121,
`lli_slip` 0 → 132: large slips hit many stations at once, so the consensus rule is
satisfied.  **The 2-cycle case is below the effective ACTION threshold, not below the
detection threshold.**

| channel, settled half | 0 slips | **465 slips** | ratio |
|---|---|---|---|
| C rms | 1.102 m | **4.630 m** | **4.2×** |
| C median \|·\| | 0.804 m | 0.996 m | 1.24× |
| C max \|·\| | 3.569 m | **19.452 m** | 5.4× |
| ZWD rms | 0.659 m | 2.158 m | 3.3× |
| consumer 3D rms | 1.417 m | 1.719 m | 1.2× |

Median barely moves while rms goes 4.2× and max 5.4× ⇒ slips act through a heavy **TAIL**,
not a uniform shift.  Also `CLK_STEP mark` **0 → 17** — station-local slips marked on the
wire as SATELLITE CLOCK discontinuities — and `T2_within_epoch / emitted_contaminated`
**0 → 11**.

#### The decomposition — and the negative result that matters

Settled C residual as fleet-common (vs pivot) ⊕ per-satellite: 0 slips 1.102 = **0.920** ⊕
0.608 m; 465 slips 4.630 = **0.466** ⊕ **4.606** m.  **Slips inflate the PER-SATELLITE error
7.6× (0.61 → 4.61 m) and leave the COMMON direction untouched — it does not grow, it shrinks
slightly.**  So the two problems are ORTHOGONAL: slip contamination is real, first-order on
the per-satellite channel, and a fixable defect (the §9.7.8 action rule); the pivot-relative
common-mode level wander (~0.9–1.3 m rms at every station count) is **NOT slip
contamination** — identical with 0 and with 465 slips.  That is the channel the field
campaign has been chasing, and this arm rules slips out as its cause.  Fixing the slip
response would clean up the per-satellite tail and the 17 spurious clock marks; it would not
move the level.

### 9.7.10 The §9.7.8 trail CLOSED — corrections, fixes LANDED, and their measurements (2026-08-11)

Follow-up session working from the retained `runs/simnet50_mwblind2/run/logs` decision
traces.

#### Corrections to §9.7.3 / §9.7.6 / §9.7.8

| # | correction |
|---|---|
| 1 | **T4's 7135 opp / 0 flag is an INSTRUMENT gap, not a dead tier.**  The flag-rate counter (`fr_attrib`, NetworkRaim.cpp) counts only the whole-SAT criterion, deliberately conjoined with `σ̂0 > raim_sigma0_trigger` (the 305-24 h lock-in guard: at a robust-clean epoch a high column effect is a median-polish artifact, and wholesale refloats destabilize).  A single-satellite fault cannot move a network-wide robust MEDIAN (one sat is ~1/31 of the row pool), so the SAT flag is **structurally unreachable at a clean epoch — for exactly the fault class the tier is named after.**  It CAN fire in globally-contaminated postures: the `phase_datum_prioritized` arm (16 flags amid 37 within-epoch triggers) proves the counter wiring.  Meanwhile the tier's ARC sub-path (isolated-arc interaction, gated at any σ̂0) **acted 14 times on the mwblind2 arm — including the real (+2,0) slip at S03/G11 at its exact injection epoch** (interaction 12.88 = 7.2×σ̂0, `attrib_arc` event, refloat applied), and none of those actions appeared in any flag-rate line |
| 2 | **§9.7.8's "100 % detection / 0 % action" over-claimed both halves.**  `network_raim.csv`'s `refloated` records the CONSENSUS action only (§0 error class, instance 4); and most of the 743 "detections" are the σ-miscalibration false-alarm population of §9.7.2 — on a clean arm, NOT acting on them is correct.  Measured true-event record: 2 injected slips → 1 with any post-fit signature → 1 acted (same epoch, arc path).  The consensus rule's receiver-event veto stands as designed; the station-local action path it calls for **already exists and works** — it is the T4 arc path |
| 3 | **The MW-blind (+2,+2) pair produced NO post-fit signature at all** — no cell spike at any epoch (its GF signature is absorbed by the free per-LOS iono, the remainder redistributed below the cell threshold), so no action rule could have caught it post-fit.  §9.7.6's "the geometry-based backstop does not fire" was an UNARMED-TIER reading: T1 (`network.cycle_slip.if_phase_td`) was off in every §9.7 arm (0 opportunities in every flag-rate table).  Armed (`_simnet50_mwblind_ifp`, one knob): **both G11 slips caught at the exact injection epoch** — S02 ΔΔφ_IF 0.205 m > thr 0.135 (the MW-null pair; prediction 2·λ_IF(1,1) = 0.214 m) and S03 0.989 m > thr 0.257 — with **2 flags in 109 137 samples = zero false alarms.**  The MW null space is covered by the tier that already existed; the standing 30/60 s doctrine (relaxed MW + LLI/IF-TD) simply was not applied to the sim posture.  (The loader's posture auditor already nags: "POSTURE BUNDLE 'zero-q phase memory + slip response': PARTIAL 1/2 — MISSING: network.cycle_slip.if_phase_td") |

#### Fix 1 — T4a flag-rate line (landed)

`fr_attrib_arc` counts the ARC sub-path's criterion (`interaction/σ̂0 > raim_k`, pre
persistence/maturity — mirroring how the SAT line mirrors `bad_sats`), emitted as
`T4a,attribution ARC (T4)` in the summary and CSV.  On the mwblind2 rerun: **103 347 opp / 14
flag — matching the 14 `attrib_arc` actions in the retained events one-for-one.**  The
summary now separates "SAT criterion structurally quiet" from "ARC sub-path live and acting".

#### Fix 2 — `network.cycle_slip.datum_station_detect` (landed, default OFF)

The §9.7.4 blind spot, closed with the mechanism-specific response instead of the §9.7.7
knob.  Setup builds the pivot's MW detector under the toggle; a serial PASS-A block rolls it
(`ApplicationNetworkAssembly`); the MW drain routes station-0 MW candidates and
(Card-L-qualified) LLI assertions to a two-edge network re-reference (invariant #7).  **Queue
edge** at detection: `datum_slip` event + the sat queued for a HARD skip-stash of its shared
PhaseBias carry at this epoch's stash (before the pin-bridge harvest, both stash branches).
**Apply edge** at the first drain AFTER that skip-stash consumed (per-sat guard, so a
withheld epoch cannot decouple the edges): `datum_reref` event, pending-reseed of the sat's
ambiguities at EVERY non-pivot station (present or absent — an absent station's mark waits
for its own re-coupling edge), local-carry erase, Card-4 C3c feed, per-sat disc mark via the
Edge-1 mark-and-serialize queue (whose SolPrep drain condition gains the third producer — its
first arm queued marks into a run with `ura_retro` and `mark_refsat` both off and the mark
silently never landed, the exact leak the drain comment predicts), and run-end counters
(`datum_slip_detections` / `datum_slip_rerefs`, invariant #4 line in the teardown).  Loader:
requires `mw_detect` (parsed after the invariant-#1 auto-arm so leveling counts); inert under
any no-pivot datum.  Unit suite: `DatumStationSlipDetectConfig` (4 cases, incl. the auto-arm
ordering).

**Validation, OFF (byte-identity).**  The mwblind2 arm rerun with the new build, toggles off:
`products.txt` and every decision trace (`network_raim.csv`, `network_events.csv`,
`network_raim_attrib.csv`, `network_slip_raw.csv`, `network_epoch_status.csv`)
**bit-identical to the retained run**; `network_flag_rates.csv` differs by exactly the added
T4a row.

**Validation, ON (`_simnet50_pivotswap_datum` vs `_simnet50_pivotswap_b`, one knob, identical
qcfault fixture data, settled half).**  The index-0 G19 LLI slip — MISSED in §9.7.4 — is now
caught end-to-end: `datum_slip` at the LLI epoch, `datum_reref` one epoch later (28
ambiguities network-wide), `R,G19` disc mark on the wire, counters 2/2.

| | OFF (blind spot) | **ON (datum detect)** | |
|---|---|---|---|
| C residual rms | 1.657 m | **0.485 m** | 3.4× |
| C residual max | **18.14 m** | **1.44 m** | 12.6× — the §9.7.9 heavy tail |
| C contrast rms (datum-free) | 2.245 m | 0.468 m | cross-check agrees |
| ZWD rms | 1.181 m | 0.685 m | 1.7× |
| `CLK_STEP` marks | **12** | **1** (the injected G14 step) | the unhandled pivot slip was being marked as ELEVEN spurious per-sat CLOCK discontinuities — poison for every consumer |
| `emitted_contaminated` | 27 | 0 | |
| A4 σ̂0 withholds | 14 | **14** | **zero availability cost** — vs 97/240 for `phase_datum_prioritized` (§9.7.7) |
| consumer 3D rms | 1.362 m | 1.227 m | AR-confounded, modest |

**Known costs / open edges (triaged):**

| edge | triage |
|---|---|
| one ignition false positive — the pivot's MW detector emitted a G12 candidate at epoch 2 (the §9.7.4 ignition-candidate class) → one spurious re-reference, marked honestly (`R,G12`) | **field-inert** (ignition is pre-convergence); a maturity gate on the datum path is a cheap follow-up once §9.7.5's mid-arc candidate-formation defect — which bounds MW's usefulness at the pivot exactly as everywhere else — is chased.  LLI is the demonstrated live catcher for the pivot |
| the pivot remains OUTSIDE post-fit RAIM cells and has NO IF-TD detector (setup gate) | coverage is MW + LLI + this response.  A pivot slip in MW's null space with no LLI stays uncovered; extending IF-TD to the pivot is the named follow-up |
| warm resume drops pending re-reference queues | same transient class as `refsat_mark_sats`; the slip then surfaces to post-fit RAIM at non-pivot stations |
| §9.7.9 orthogonality stands | this cleans the per-satellite tail and the wire hygiene.  **It does not move the pivot-relative common-mode LEVEL** — the datum-wander channel remains the separate (CB-Q) campaign |

#### What this does and does not license

- The `i_lo` finding is **proven** (controlled single-variable A/B) and actionable: extend
  detection to station 0 in the legacy datum posture, or document the blind spot and make the
  eq34 posture mandatory where slips matter.
- The **MW mid-arc silence is measured but unexplained** — one fixture, one data rate (30 s),
  one elevation range.  A lead, not a verdict on the detector.  OPEN.
- Caveat from §9.7.3: the sim's slips here are injected on ONE band (the classic widelane
  target); a two-band slip is a different test — reproducible via §15.3's `--signal-mode
  mw-blind`.

---

