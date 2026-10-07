# Study 2: the receiver-clock carry under single events

**STATUS**

- **id:** Study 2
- **topic:** harness
- **status:** closed
- **summary:** `network.carry_rx_clock_in_block` is INERT on all three injectable events (slip, `arc_erase`, cold handover), every OFF-to-ON change at or below the null-vs-null floor; Study 2b later closed it KEEP_OFF.
- **dates in the text:** 2026-09-10
- **knobs / posture:** `network.carry_rx_clock_in_block`; rung `network.phase_leveling_young_inflate_var_m2` (KEEP_OFF confirmed)
- **hashes named in the text** (commit / exe / byte-cert): `6a3531a1afb2b40b`, `8a849480e7ef02af`
- **run dirs named in the text:** `runs/evseed_rxclk6h`, `runs/simnet60_quiet_wu93_24h`, `runs/evleg_s2_null_yng`, `runs/evleg_s2_null_off`, `runs/evleg_s2_slip_off`, `runs/evleg_s2_erase_on`, `runs/evleg_s2_null_on`
- **source:** `docs/archive/SPEC_event_harness.md`, sections 10, 10.1, 10.2, 10.3, 10.4, 10.5, 10.6, 10.7, 10.8

**Outcome.** The study asked whether carrying the receiver clock in the F3 block holds a station's frame under one injected event (quiet 60-station sim bed). The largest OFF-to-ON change sat 6× below the floor on the frame reaction and 9× below on the clock contrast, at a null cost under one per cent: the carried value already contains the error, so it can only stop a jump, and the designed target (a receiver-clock step) was not injectable. The young-inflate rung made the erase floor 3.3× worse: KEEP_OFF. Study 2b graded the knob on `rx_clock_step`: KEEP_OFF, a scale-linear 0.104 % leak ([Cards A+B](../harness/CARD-AB-require-and-rx-clock-step.md), section 11.7).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 10. Study 2 — the receiver-clock carry under single events

**Date 2026-09-10.  Verdict: `network.carry_rx_clock_in_block` is INERT on every event
the harness can inject, at a cost on the null of under one per cent.  It transports the
station's ρ-direction error; it does not reduce it.**

*(Lettered, not numbered, on purpose: Study 1 lands from a different worktree and this
section must not collide with its number.)*

**The question.** On the network path the station receiver clock is a LOCAL `A_r` column
with **no temporal prior at all** (`filter.process_noise.clock_bias_q` is inert here —
measured byte-identical), so it is the free per-epoch **ρ-direction sink**: any
range-direction error an event leaves in a station can be absorbed by re-estimating that
station's clock, at zero cost to the residuals and with nothing saying so.
`network.carry_rx_clock_in_block` carries it inside the F3 whitened local block as a
random walk about a carried DRIFT, with a jump gate (`carry_rx_clock_jump_k`, 10 σ on the
one-step prediction) as invariant #1's wired response.  Study 2 asks whether giving that
sink a memory makes the station's **frame** hold under one event, and what the memory
costs on a quiet station.

### 10.1 The knob is FINGERPRINTED, and that decided the design

`carry_rx_clock_in_block` and `carry_iono_in_block` are both inside the warm-resume
fingerprint (`NetworkRunContext.cpp`, armed-only conditional appends beside the
`pl:losblock` / `zwd:blk` tags, on the F3 precedent — the carried LEVELS in a snapshot
were accumulated under a different prior geometry).  Measured, not inferred: the OFF
seed's snapshots carry fingerprint `6a3531a1afb2b40b`, the ON seed's `8a849480e7ef02af`.
`phase_leveling_young_inflate_var_m2` is **not** fingerprinted — it is a forward-only
policy in the documented exemption class.

That is not a formality, it is the experiment design:

* **Fingerprinted ⇒ the ON arm needs its own seed.**  `runs/evseed_rxclk6h` is a second
  6 h seed (exit 0, 1156 s) written by the same exe from a config differing from the OFF
  seed's by exactly ONE settings key — verified by a flattened-config diff:
  `network.carry_rx_clock_in_block` plus the two output paths.  Each arm's event legs are
  graded against **that arm's own null**; ON-null-vs-OFF-null is the lineage contrast and
  **is also this study's noise floor** (§10.4).
* It is also the better experiment.  Resuming an OFF snapshot with the knob ON — what a
  fingerprint-SAFE knob would have permitted — starts the clock carry COLD, ten epochs
  before the event.  The card is about a SETTLED carry; a cold one measures its ignition.
* **`young_inflate` being exempt is exactly why its rung rides the OFF seed** (one knob
  against `erase_off`), with its own matched null leg.  One fingerprint finding, two
  different experiment shapes.

### 10.2 Design

Same bed, snapshot and arc as §6: `runs/simnet60_quiet_wu93_24h`,
`state_snapshot.txt.ep0665` (tow 279150), 61 epochs, event at +10 (tow 279480), target
arc **S21000SIM / G10 / L1C** — a mature arc, not the clock-datum station, and G10 is not
the DD trace's reference (G01 / E02, unchanged in both arms).  Ten legs, 136–160 s each.

New instrument: **`scripts/event_frame_grade.py`**, the station-frame companion to
`event_grade.py` (which grades the ARC and the WIRE).  Per epoch and as event−null it
adds the arc's level error **per band**, the station's **other arcs'** mean |level error|
(the frame reaction — the number this study exists for), the station's **receiver-clock
error vs `truth_clock.csv` as the CONTRAST** (minus the per-epoch network mean: the
common part is the receiver-clock datum, absorbed by the satellite clocks and not a
station property — invariant #5), the station ZWD error, and the **per-LOS slant iono
error** for the injected satellite (`iono_total_m`, the absolute column, never `iono_m`).

ON-path telemetry (invariant #4), identical in all four ON legs and non-trivial in the
seed, so nothing below is a report on an inert knob:

```
leg   Rx clock in block: 7320 col(s) (3660 ISB), 0 not folded, 0 missing cross term(s),
                         7260 RAIM-pool admission(s), 0 refused, 0 dropped at gaps
seed  Rx clock in block: 85559 col(s) (42780 ISB), 0 not folded, 0 missing cross term(s),
                         85239 RAIM-pool admission(s), 1 refused, 0 dropped at gaps
```

7320 = 2 columns (clock + ISB) × 60 stations × 61 epochs: every station folded its clock
every epoch.  **One** clock-JUMP refusal in 6 h across 60 stations.

### 10.3 The OFF arm first — the events are NOT alike, and the big one is not a ρ error

A pre-registered reading, from the OFF arm alone, before the ON legs were graded.  The
`arc_erase` level error is **dispersive**: L2/L1 = 1.285–1.307 and L5/L1 = 1.359–1.363 in
cycles, against the pure-L1-iono prediction f₁/f₂ = **1.2833**, f₁/f₅ = **1.3391**; a
non-dispersive (range / clock / ZWD) error would give the reciprocals, 0.779 and 0.747.
And the arc's L1 level error in metres equals the per-LOS iono displacement to 2–5 %
(−0.3625 vs −0.3701 m at +2; −0.0793 vs −0.0832 m at +45).

**An unnecessary erase is a (level, slant-iono) SPLIT error: it lives in the DISPERSIVE
direction, not in ρ.**  Therefore — written down in advance — a ρ-direction memory should
do little for it.  The cold handover looked like the place a ρ memory could grip: it is a
mixture (L2/L1 1.141 at +2) and carries 3× the erase's clock signature (`clk_d` rms
0.0160 vs 0.0049 m).  That half of the prediction turned out to be **wrong** (§10.4).

The OFF ladder of frame reaction is clean and monotone — a correctly detected and
refloated real slip barely moves the station, an unnecessary erase moves it 3.7×, and a
network datum re-realization moves a BYSTANDER station 11×:

| event (OFF) | `oth_d` rms +14..+45 (cyc) | ion_d peak (m) | clk_d rms (m) |
|---|---|---|---|
| slip 10 cyc (detected at +0, refloat) | **0.0054** | +0.007 | 0.0026 |
| `arc_erase` on a healthy arc | **0.0201** (3.7×) | −0.370 | 0.0049 |
| `anchor_handover` cold, at a bystander | **0.0611** (11×) | −0.388 | 0.0160 |

### 10.4 The table — {event} × {OFF, ON}

`lvl` columns are the injected arc's L1C level error (cycles, event − null); `oth` is the
station's other arcs' mean |level error|; `clk` is the receiver-clock contrast (m); `cons`
is consumer 3D (m).  ⚠ `cons_d` is a **perturbation magnitude, not a degradation** — the
null's own consumer error is ~2.2 m on this bed, so a favourable sign means only that the
event moved a pre-existing bias (the same caveat applies to §6(4)'s "−0.546 m").

| arm | peak arc \|e\| | +14 | +30 | +45 | oth_d rms | oth \|e\| +45 | clk rms | clk_d rms | ZWD_d pk | ion_d pk | cons_d +45 | cons +45 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **null (ON − OFF) — the noise floor** | 0.0107 | 0.0042 | 0.0072 | 0.0098 | **0.0017** | 0.3622 | 0.9702 | **0.0015** | −0.0006 | −0.0059 | **0.0218** | 2.2464 |
| slip 10 cyc  OFF | 0.0537 | 0.0507 | 0.0439 | 0.0370 | 0.0054 | 0.3549 | 0.9663 | 0.0026 | 0.0014 | 0.0074 | −0.0005 | 2.2241 |
| slip 10 cyc  ON | 0.0529 | 0.0499 | 0.0427 | 0.0359 | 0.0054 | 0.3555 | 0.9677 | 0.0026 | 0.0015 | 0.0073 | −0.0008 | 2.2456 |
| `arc_erase`  OFF | −1.9050 | −0.4147 | −0.5881 | −0.4166 | 0.0201 | 0.3400 | 0.9640 | 0.0049 | 0.0038 | −0.3701 | −0.0005 | 2.2241 |
| `arc_erase`  ON | −1.9046 | −0.4192 | −0.5922 | −0.4224 | 0.0204 | 0.3401 | 0.9653 | 0.0051 | 0.0039 | −0.3700 | −0.0015 | 2.2449 |
| `anchor_handover` cold  OFF | −1.2951 | 0.0114 | −0.1260 | −0.0939 | 0.0611 | 0.3774 | 0.9842 | 0.0160 | 0.0043 | −0.3882 | −0.5460 | 1.6786 |
| `anchor_handover` cold  ON | −1.2972 | 0.0094 | −0.1272 | −0.1031 | 0.0611 | 0.3777 | 0.9858 | 0.0161 | 0.0044 | −0.3899 | −0.5574 | 1.6890 |
| `arc_erase` + young_inflate (own null) | −1.7792 | −1.5448 | −1.4163 | −1.3823 | 0.0842 | 1.5858 | 1.0338 | 0.0044 | 0.0106 | −0.2993 | −0.0016 | 1.3688 |
| young_inflate null − OFF null | −0.5126 | −0.2328 | −0.1590 | −0.1771 | 1.3338 | 1.6381 | 1.0344 | 0.0680 | −0.0350 | −0.0249 | −0.8542 | 1.3704 |

**Read the first row against the rest.**  The two nulls are the same bed, the same
posture and the same 6 h of data differing by one knob, so their difference is the floor
below which no OFF→ON change in an event signature means anything.  Every OFF→ON change
in the table is at or under that floor:

| quantity | null floor (ON−OFF) | largest OFF→ON event change | ratio |
|---|---|---|---|
| `oth_d` rms (frame reaction) | 0.00166 cyc | 0.00028 (erase) | **6× below** |
| `clk_d` rms | 0.00149 m | 0.00017 (ho) | **9× below** |
| consumer 3D | 0.0218 m | 0.0114 (ho) | **2× below** |
| arc level at +45 | 0.0098 cyc | 0.0093 (ho) | at the floor |

**Verdict, per event.**

* **slip 10 cyc — NO EFFECT.**  Persistent floor 0.0370 → 0.0359 cyc, frame reaction
  0.00538 → 0.00543, iono +7.4 → +7.3 mm.  The T1 detector already handles this event at
  zero latency; there is nothing left for a clock memory to hold.
* **`arc_erase` — NO EFFECT, and §10.3 says why.**  Persistent floor 0.4166 → 0.4224 cyc
  (1.4 % *worse*), frame reaction 0.0201 → 0.0204, per-LOS iono −0.3701 → −0.3700 m.  The
  erase's error is dispersive; a ρ-direction prior has the wrong basis direction for it.
  **The prediction held.**
* **`anchor_handover` cold — NO EFFECT, and the prediction did NOT hold.**  This was the
  mixed-direction event with 3× the clock signature, so it was where a ρ memory should
  have gripped.  It did not: frame reaction 0.06109 → 0.06106 (0.05 %), wire P(L1C)
  20.079 → 20.069 m, consumer perturbation 0.546 → 0.557 m.  A carried clock does not
  defend a bystander station against a phase-datum re-realization.
* **Cost on the null — under one per cent, but not zero.**  Network-wide over the settled
  window (60 stations × 39 epochs): receiver-clock contrast rms **1.3096 → 1.3099 m
  (+0.02 %)**, ZWD rms 0.0708 → 0.0712 m (+0.53 %), consumer 3D rms 2.387 → 2.408 m
  (+0.90 %).  The knob is *not* a no-op on the product — the two nulls' emitted G10 clock
  differs by up to **43 mm** and the phase bias by 4.5 mm — it simply does not change the
  ERROR.

### 10.5 The mechanism, and where the card would actually bite

The receiver-clock contrast error on this bed is **1.31 m rms network-wide** (p50 1.16,
p90 1.90) — structured, not noise — while the carry's own per-epoch prior is σ = 0.30 m.
The prior is tighter than the error and still nothing moves, because the carried value is
harvested from the previous epoch's solve and therefore already *contains* that error.
**A memory cannot correct an error that is already in the memory; it can only stop a
JUMP.**  On a quiet bed with a working slip detector, an erase whose error is dispersive,
and a handover that re-realizes the phase datum rather than the range datum, there are no
ρ-direction jumps to stop — which is exactly what the counter says: **1 refusal in 6 h ×
60 stations.**

So this is not evidence that the card is wrong; it is evidence that **its target event is
the one the harness cannot inject.**  §8 already lists *"a receiver clock jump"* among the
non-injectable kinds.  That is the gap to close before `carry_rx_clock_in_block` can be
accepted or rejected on event evidence: a `rx_clock_step` kind (station, tow, step_m —
add `step_m` to every observable of every satellite *at one station*, the exact dual of
`sat_clock_step`'s per-satellite form, one scalar write at the same §2.1 site).  Until
then the honest statement is: **inert where measured, untested where designed.**

### 10.6 The extra rung — `phase_leveling_young_inflate_var_m2` on one erase

It FAILED on the storm bed; the harness says what it does to ONE event, and separates two
things the storm bed could not.  Because the knob is NOT fingerprinted it rides the OFF
seed with a matched null (`runs/evleg_s2_null_yng`), one knob apart from `null_off`:

* **Standing cost on a QUIET station, no event anywhere** (`null_yng` − `null_off`): the
  station's other arcs go from 0.36 to **1.64 cyc** mean |level error| (`oth_d` rms
  **1.334 cyc**), the receiver-clock contrast moves **68 mm** rms, ZWD **−35 mm**, the
  consumer **0.854 m**.  Network-wide the ZWD rms goes 0.0708 → **0.1074 m (+52 %)**.
  Arming it is not a response, it is a standing re-weighting of every re-levelled column
  in the network, and it is metre-class on the level channel.
* **Effect on the erase itself** (`erase_yng` − `null_yng`): the persistent floor is
  **3.3× worse** (−1.54 / −1.42 / −1.38 cyc at +14 / +30 / +45 against the plain erase's
  −0.41 / −0.59 / −0.42) and the frame reaction **4.2× worse** (`oth_d` rms 0.084 vs
  0.020).  It makes the treadmill longer, not shorter.
* Telemetry says it was working, not idle: **28455 column-epochs inflated, all 28455
  congruence-scaled inside an F3 block**, mean age 25.1 ep, Σ var_add 971 m², 752
  re-level stamps (458 created / 294 freed).
* Its consumer 3D at +45 is *lower* (1.369 vs 2.225 m) and network-wide rms 2.387 → 1.728.
  Per the `cons_d` caveat above that is a perturbation of a 2.2 m standing bias, not a
  quality gain; on the level and ZWD channels — the ones with truth — it is unambiguously
  worse.  **KEEP_OFF confirmed, now with a mechanism.**

### 10.7 Byte-identity and the suite (branch `worktree-ionoblk`)

* **OFF is byte-identical to main HEAD.**  `config/test_suite/_golden_sim_off60.json`
  (61-epoch golden smoke, output path repointed only), `gnss_processor_rxclk2.exe`
  (sha256 `38FC0BB1…`): `products.txt` sha256
  **`3BF7831B29916C7A1E20434AC91F2ADFEB74EB06974560DF6E050FC43AC3F357`** — the exact hash
  §6 records for main HEAD's `gnss_processor_ev_main.exe` (sha256 `3C3CA15B…`).
* **Stronger, and free:** `runs/evleg_s2_null_off` and `runs/evleg_s2_slip_off` — WARM
  resumes of an OFF snapshot, one of them with an injected event — hash byte-for-byte to
  the main-HEAD-exe legs `evleg_null` (`95DC556F…`) and `evleg_slip` (`E86791CF…`).  OFF
  identity therefore covers the resume path and the injector, not just a cold run.
* **Debug suite: 1783 cases / 244 suites, 1766 passed, 0 failed, 17 skipped** (main's
  1713 / 241 plus this branch's three cards: 18 + 28 + 24 = 70).
* ⚠ **No perf claim from these runs.**  The seeds and legs were run 2–4 at a time on one
  box; the ON seed's 1156 s against §6's OFF 1540 s is a contention artifact in both
  directions, not a measurement.

### 10.8 Reproducing

```powershell
# OFF arm rides the existing seed; ON arm needs its own (the fingerprint, §10.1)
python scripts\event_leg.py write-seed --name rxclk6h --base config\test_suite\_ev_rxclk_on.json --hours 6
python scripts\event_leg.py make-leg --seed-run runs/evseed_rxclk6h --snapshot-ep 665 --epochs 60 `
       --name s2_erase_on --at-ep 10 --event '{"kind":"arc_erase","station":"S21000SIM","sat":"G10"}'
python scripts\event_frame_grade.py runs/evleg_s2_erase_on --null runs/evleg_s2_null_on `
       --fixture C:/GnssProcessor/runs/simnet60_quiet_wu93_24h
```

`event_frame_grade.py` takes `--station` / `--sat` overrides for kinds that carry no
station (`anchor_handover`) and for a null-vs-null contrast.

---

