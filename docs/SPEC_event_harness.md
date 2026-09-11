# SPEC — the EVENT HARNESS (producer-side fault injection on a warm resume)

**Status: BUILT and VALIDATED on the sim quiet bed, 2026-09-10.** Default OFF and
structurally byte-identical (§7). Directive that motivated it: *"run the network to
convergence once, then inject one simulated event and continue a few epochs to study
the effects — minutes, not day-long runs."*

---

## 1. What it is

Grading a mechanism on this project has meant running the network for hours and
waiting for nature to supply an event. The turnaround for one question is a day, and
the answer is a mixture of the event, its neighbours, and the run's own ignition.

The harness inverts that:

1. **Converge ONCE** into a state snapshot (`scripts/event_leg.py write-seed`). One
   6 h run on the sim bed; snapshots every 60 emitted epochs.
2. **Resume for a few tens of epochs** with exactly ONE scripted fault armed
   (`network.inject`), and again with none (the NULL leg).
3. **Difference the two** (`scripts/event_grade.py`). Both legs resume from the same
   snapshot with the same config except `network.inject`, so the warm-resume seam
   transient, the bed and the posture are common and cancel.

Cost: **~2 min per leg** (61 epochs at 60 stations), against a 6 h seed you pay once.

It is **not** a solution producer. An armed leg's products are a harness fixture; the
loader and the run-start banner both say so in as many words.

## 2. The six injectable events

`network.inject.events` is a JSON array; `enable` defaults to true when the array is
non-empty. `tow` is GPS seconds-of-week at which the event starts (the run's week is
implied; a leg never spans a rollover), matched with a ±0.25 s half-epoch guard.

| kind | fields | what it does |
|---|---|---|
| `slip` | `station, sat, signal, tow, delta_cycles, flag_lli` | adds `delta_cycles` to that station's phase observable of (sat, signal), **from `tow` onward** — a STEP, not a ramp. `flag_lli` raises LLI bit 0x1 on the onset epoch only. |
| `outage` | `station, sat, tow, duration_s` | drops every observation of (station, sat) for `[tow, tow+duration_s)`. The return is a natural re-admission. |
| `anchor_handover` | `sat, signal ("all"\|code), tow, mode ("cold"\|"continuous")` | excludes the incumbent phase-anchor station from candidacy for that family for ONE epoch, forcing a successor. `cold` additionally refuses the continuous-handover branch, so the family takes the re-realize + re-reference path. |
| `arc_erase` | `station, sat, tow` | runs the T4 attribution refloat for that (station, sat) — appended to `bad_arcs` so it executes **the same code** a real attribution does (carry erase, iono-carry erase, pending-reseed, C3c queue). |
| `sat_clock_step` | `sat, tow, step_m` | adds `step_m` to the a-priori satellite clock used by every station, from `tow` on. |
| `rx_clock_step` | `station, tow, step_m` | adds `step_m` to the receiver clock of ONE station — i.e. to **every** code and phase observable of **every** satellite there, from `tow` on.  The exact dual of `sat_clock_step`; a `sat` or `signal` field is REFUSED by the loader because neither scopes it. |

### 2.1 Where the injection acts, and why it is one site

Four of the six kinds are corruptions of the producer's INPUT. They are applied to a per-epoch
mutable copy of the `SyncedEpoch` at the very top of the epoch body
(`ApplicationNetwork.cpp`, immediately after the `start_offset_hours` gate) — before
`networkFastClockTick`, before `computeModels`, before every T0 detector, before
`networkComputeRawObservables`, before assembly.

That one site is sufficient because **both downstream representations are derived
from it**. `PppModel::computeModels` builds

* `PppObservation::observed` — phase in **metres** (`observed_value *= wavelength`),
  what `MeasurementModel` turns into design rows; and
* `PppObservation::raw_observed` — phase in **cycles**, what MW / GF / IF-TD read

from the same `RinexObservation::measurements` entry, which is in **cycles**. So a
single scalar write on the `RinexObservation` moves the slip detectors and the design
matrix together and consistently. Injecting into `modeled` instead would need a dual
write with a wavelength lookup, at a site inside an OpenMP region, plus a second site
for the legacy pivot — and would still miss the fast-clock tier, which reads
`se.by_station` directly.

`sat_clock_step` and `rx_clock_step` are applied at the same site and this is
**exact, not an approximation**. The uncombined residual is
`y = L̃ − (ρ + c·δt_r − c·δtˢ + …)`, so adding `step_m` metres to every code
observable and `step_m/λ` cycles to every phase observable

* **of one satellite at every station is** `+step_m` on the a-priori satellite
  clock `c·δtˢ` (`sat_clock_step`), and
* **of every satellite at one station is** `+step_m` on that station's receiver
  clock `c·δt_r` (`rx_clock_step`) —

the same scalar write swept over the other index of the same residual, which is
the whole duality between the two (they share one helper, `addRangeStep`, so they
cannot drift apart). Doing it here keeps the injector out of `ModelGeometry`,
which has no access to the run context.

The two decision-side kinds are staged as one-epoch directives in
`NetworkRunContext::inject` and consumed by the seam that owns the decision:
`anchor_handover` → `networkElectPhaseAnchors` (a candidacy **ban**, so the successor
is still chosen by the production rules — the harness forces the EVENT, never the
outcome); `arc_erase` → the T4 block in `networkPostFitRaim`.

### 2.2 Never-fired is a HARD error

Verbatim the simulator's scripted-slip rule (`ScriptedSlips::unfinished`,
`src/sim/main.cpp`): an armed event that never fired fails the run at run-end, by
name, with the three things to check (roster, obs code, epoch window). A harness whose
fixture silently did not exist is worse than one that refuses to finish — the leg
would otherwise be graded as a null and reported as "the event had no effect".

## 3. Files

| file | role |
|---|---|
| `include/filter/NetworkInject.hpp`, `src/core/filter/NetworkInject.cpp` | the seam: schedule, per-epoch application, staged directives, run-end clause |
| `include/config/GnssConfigTypes.hpp` | `NetworkInjectEvent`, `NetworkInjectConfig` |
| `src/core/config/GnssConfigLoadNetwork.cpp` | `loadNetworkInject_` (strict: per-index errors, 64-event cap, per-kind required fields) |
| `include/app/NetworkRunContext.hpp` | `NetworkInjectState inject` |
| `src/core/app/ApplicationNetwork.cpp` | arm + the per-epoch seam + the anchor-handover readback |
| `src/core/filter/NetworkPhaseDatum.{hpp,cpp}` | the anchor candidacy ban + forced-cold branch |
| `src/core/filter/NetworkRaim.cpp` | the injected T4 erase |
| `src/core/filter/NetworkTeardown.cpp` | run-end clause + the never-fired exit code |
| `src/core/logging/NetworkRunTraces.cpp` | manifest keys (armed-only) |
| `scripts/event_leg.py` | `write-seed` / `make-leg` / `null` / `rerun` |
| `scripts/event_grade.py` | the event-minus-null grader |
| `scripts/ev_launch.ps1` | detached launcher (per-run cwd, always writes `exitcode.txt`) |
| `tests/test_network_inject.cpp` | 28 cases: parse contract + epoch semantics (5 of them `rx_clock_step`) |
| `tests/test_warm_resume_require.cpp` | 6 cases: the HB-1 `state_snapshot_require` policy (§11.1) |
| `scripts/event_frame_grade.py` | the station-FRAME grader; `--step-m` adds the absorption / residual channel |

## 4. The leg recipe

```powershell
# ONE seed (6 h, ~40 min at 60 stations; snapshots at emitted epochs 1, 61, 121, ...)
python scripts\event_leg.py write-seed --name quiet6h --hours 6
pwsh scripts\ev_launch.ps1 -Exe <release exe> -Config runs\evseed_quiet6h\seed.json `
                           -RunDir runs\evseed_quiet6h        # detached; poll exitcode.txt

# the NULL control (every event leg is graded against this)
python scripts\event_leg.py null --seed-run runs/evseed_quiet6h --snapshot-ep 721 `
       --epochs 60 --name null --run

# one event, at +10 epochs (past the ~5-epoch resume transient)
python scripts\event_leg.py make-leg --seed-run runs/evseed_quiet6h --snapshot-ep 721 `
       --epochs 60 --name slip10 --at-ep 10 --run `
       --event '{"kind":"slip","station":"S12000SIM","sat":"G05","signal":"L1C","delta_cycles":10}'

python scripts\event_grade.py runs/evleg_slip10 --null runs/evleg_null `
       --fixture C:/GnssProcessor/runs/simnet60_quiet_wu93_24h
```

`make-leg` copies the chosen `.epNNNN` snapshot to `runs/evleg_<name>/seed.txt` (never
the seed's live file — a killed leg must not be able to destroy the state it resumed
from), derives `network.start_offset_hours` from the snapshot's `M` line with the
half-epoch back-off `(week·604800 + tow − 1399161600 + 15)/3600`, sets
`processing.max_duration_s = 30·(epochs+1)` (anchored on the first PROCESSED epoch —
the offset gate returns before the duration gate is reached), and arms the leg-E
warm-restart posture: `state_snapshot_require`, `resume_frame_restore`,
`resume_coupling_restore` on a writer that captured frame + coupling (QK/QP).

⚠ **A leg is not re-runnable in place** — it overwrites its own `seed.txt` at its
first emitted epoch (§6b). Use `python scripts/event_leg.py rerun <legdir>`, which
re-seeds from the pristine `.epNNNN` first.

`--at-ep` counts from the leg's FIRST processed epoch, so the default 10 puts the
event past the ~5-epoch resume transient that `SPEC_synthetic_minimum_network` §18.14
tells you to discard.

## 5. Reading the grader

Rows are epochs relative to t0. Columns: `lvl` (the injected arc's chord-median DD
level error in cycles, with the injected delta folded into the truth so a perfect
response reads ~0), `zwd` (station ZWD error vs `truth_tropo.csv`), `clk_d`/`pb_d`
(the emitted satellite clock and phase bias **minus the null leg's** — the wire
signature), `cons` (consumer 3D error and `ar_gate`), and `qc` (tier/event rows from
`network_events.csv` on that (station, sat)). `_d` columns are event − null.

⚠ **`lvl` inherits the DD trace's single reference satellite** (`SPEC_amb_iono` §6.12):
its own between-station error is in every chord. Read the DIFF column, which cancels
it wherever the two legs share the reference — they do, except where the event itself
moves the reference.

## 6. Validation, 2026-09-10

Seed `runs/evseed_quiet6h`: `golden_sim_tri_w6` on `simnet60_quiet_wu93_24h`, 60
stations + 1 held-out consumer (SZZ000SIM), GPS+Galileo triple, 6 h = 721 epochs,
**1540 s wall**, exit 0. Snapshots at emitted epochs 1, 61, …, 665.

Every leg below: 61 epochs from `state_snapshot.txt.ep0665` (tow 279150), event at
+10, graded against `runs/evleg_null`. **Wall time 148–151 s per leg** (four legs
concurrently; 105 s alone). Target arc `S21000SIM / G10 / L1C` — a mature arc, not
the clock-datum station (S30000SIM), not the consumer, and G10 is not the DD trace's
reference satellite (G01 / E02).

### (1) The seam transient — and why grading is event-minus-null

`runs/evleg_seam` (a NULL leg from ep0061) against the continuous seed run over the
same 60 epochs, `event_grade.py --seam-ref`:

| leg epoch | max abs dC (m) | rms dC | max abs dP (m) | rms dP |
|---|---|---|---|---|
| +0 | 0.1323 | 0.0224 | 0.1231 | 0.0329 |
| +9 | 0.1420 | 0.0255 | 0.3303 | 0.1330 |
| +18 | 0.1437 | 0.0263 | 0.4727 | 0.1864 |
| +19 | 0.1435 | 0.0263 | **10.587** | 0.869 |
| +59 | 0.1440 | 0.0298 | **20.521** | 2.361 |

**The raw wire NEVER settles below 1 cm on either channel.** The clock channel is
flat (max 0.13 -> 0.14 m, rms 0.022 -> 0.030 m over 60 epochs — it does not decay,
it does not grow); the phase-bias channel drifts to ~0.5 m and then takes two
discrete re-realization steps (+19 and later) to 20.5 m max / 2.4 m rms.

Those P steps are a **frame** difference, not an error — standing invariant #5's
exact warning about grading raw wire steps. The consequence for the harness is the
design decision it validates: **every event leg is graded against a NULL leg resumed
from the same snapshot, never against the continuous run.** Both legs carry this
transient identically and the `_d` columns cancel it. The measured event signatures
below are 0.0001–0.5 m on channels whose seam transient is 0.14–20 m; without the
null they would be unreadable.

### (2) `slip`, 10 cycles on S21000SIM/G10/L1C

* **Detected at +0** — the same epoch, zero latency — by `T1_if_slip/if_slip`,
  statistic **4.876 against gate 0.612 (8.0x)**, action `refloat` on `L1C;L2W`. The
  NULL leg has no event on that station in the window, so the row is entirely the
  injected slip's.
* Arc level error (event - null), mean abs by band:
  **+1..+5 0.029 cyc, +6..+10 0.043, +11..+20 0.048, +21..+30 0.048, +31..+45 0.040.**
  A 10-cycle step leaves a **~0.04 cycle** residual from the first epoch after the
  response, flat thereafter. The response is correct.
* Station ZWD error (event - null) peaks **+1.4 mm** at +38; wire C step **+0.4 mm**,
  P(L1C) **+0.1 mm**; consumer 3D (event - null) **-0.001 m**.

### (3) `arc_erase` on the SAME arc — the re-level treadmill, priced

A T4 erase injected on a **healthy** arc, executing the production attribution path
(`refloated 3 carried chord(s)`, sigma0 = 1.000, event row `T4_attrib/attrib_arc`):

| band | mean abs level error, event - null (cyc) | slip leg, same bands |
|---|---|---|
| +1..+5 | **1.194** | 0.029 |
| +6..+10 | **1.314** | 0.043 |
| +11..+20 | **0.508** | 0.048 |
| +21..+30 | **0.452** | 0.048 |
| +31..+45 | **0.454** | 0.040 |

Peak **-1.905 cyc at +2**; it decays to a **persistent ~0.45 cycle floor by +14 and
stays there for the remaining 31 epochs**. Station ZWD error peaks **+3.8 mm** at +31
(2.7x the slip leg's). Wire C **+0.7 mm**, P(L1C) **+0.2 mm**; consumer -0.001 m.

**One unnecessary erase costs ~11x the residual level error of a correctly handled
real 10-cycle slip, and it does not go away.** That is the re-level treadmill
(standing invariant #1's re-coupling edge, `SPEC_slip_relevel_walk`) isolated to a
single event and measured in 148 s, on a bed with no slip at all.

### (4) `anchor_handover` on G10 — cold vs continuous, 50x

Both modes retired the same incumbent and elected the same successor, so the only
variable is the branch:

```
INJECT,anchor_handover,G10,,3.000000,,injected,mode=cold;families=3;forced_cold=3;
       retired=S29000SIM|S29000SIM|S29000SIM;elected=S06000SIM|S06000SIM|S06000SIM
INJECT,anchor_handover,G10,,3.000000,,injected,mode=continuous;families=3;forced_cold=0;
       retired=S29000SIM|S29000SIM|S29000SIM;elected=S06000SIM|S06000SIM|S06000SIM
```

| | wire P(L1C) step (event - null) | consumer 3D (event - null) |
|---|---|---|
| **cold** | **+20.079 m at +0** | **-0.546 m at +45** (still growing) |
| **continuous** | **+0.403 m at +45** (0.001 m at +0) | +0.084 m at +1, ~0 after |

**50x on the wire, and the consumer difference is the point**: the cold re-realization
is a metre-class consumer error that has not recovered 45 epochs later, while the
continuous handover is a centimetre-class one-epoch blip. This reproduces the
single-pivot PB gauge defect and the value of the two-edge continuous handover as a
controlled A/B, in 300 s of wall clock.

### (5) `outage`, 20 epochs on S21000SIM/G10

Ledger exact: `outage_begin;dropped=1` at t0, `released;epochs_dropped=20` at +20 —
the window is exactly the 600 s asked for. Effects:

* **The leveling-carry retirement census moves by exactly +3** (264 -> 267 permanent
  amb-carry prunings) — the three signals L1C/L2W/L5Q of that arc, pruned on the
  120 s gap rule.
* **`READMIT continuity` reports 0 re-admissions**, correctly: `readmit_seed_horizon_s`
  tracks satellites departing the NETWORK, and G10 stayed visible at 59 other stations.
* **`network_events.csv` carries NOTHING for that arc at either edge** — no detector
  row at the drop, no row at the return. The decoupling edge is counted only in a
  run-end census and the re-coupling edge is a fresh birth with no re-validation.
  That is standing invariant #7's named failure mode, on a per-station arc gap.
* Station ZWD error peaks **+4.4 mm** at +31; wire C **+1.1 mm**, P(L1C) **+0.5 mm**;
  consumer 3D **+0.005 m** at +37; arc level error back under 0.5 cyc from +27.

### (6) `sat_clock_step`, +0.5 m on G10

The emitted clock absorbs the step **within one epoch and essentially in full**:

```
ep      -1      +0      +1      +2      +5     +10     +15
dC(m) 0.0000 -0.5006 -0.5009 -0.4993 -0.5009 -0.5021 -0.5022
```

i.e. **100.4 %** of the injected 0.5 m, settling at -0.5022 m; P(L1C) moves **-0.6 mm**
— the step lands where it physically belongs and nowhere else. The clock temporal
anchor both detected and signalled it:

```
CLK_ANCHOR,cta_decouple,G10,,11.080647,6.000000,refloat,reason=nis;e=-0.2188;...
CLK_ANCHOR,cta_disc_landed,G10,,,,disc_bump,clock_anchor_decoupled
CLK_ANCHOR,cta_recouple,G10,,0.004518,6.000000,reengage,in_band=4;reseed_var=1.511e-05
```

NIS **11.08 against gate 6.0** at +0, a discontinuity bump raised for the consumer,
and re-coupling at **+4 epochs** after 4 in-band epochs — both edges instrumented.
Consumer 3D (event - null) -0.029 m at +0.

### Byte-identity and the suite

* **Golden 61-epoch smoke, same build config, products.txt SHA256**
  `3BF7831B29916C7A1E20434AC91F2ADFEB74EB06974560DF6E050FC43AC3F357` from BOTH the
  new exe and the main-HEAD `gnss_processor_oracle2.exe`
  (`98B2CB21D1BB7EF76652D3D2AC099D5EAB6F1AECEDB43D6104358887CCC500CA`). Identical.
* **Debug suite: 1713 cases / 241 suites, 1696 passed, 0 failed, 17 skipped**
  (16 of those are fixture-gated — `data/` is not bootstrapped in this worktree).
  23 of the cases are this card's.

## 6b. A defect the harness found on its first day — FIXED, §11.1

**As reported (2026-09-10):** *"`state_snapshot_require: true` does NOT hard-fail on
the non-positive-age refusal ... it warns, falls through to a COLD START, and the run
then died `0xC0000409` 25.8 s later."*

**As diagnosed in source (§11.1): half right, and the half that was wrong changed the
fix.** The branch *was* wired to `hb1` and the run *was* refused. What was broken is
that the refusal was **unreadable and uncontrolled**: the `COLD START` warning fired
first and unconditionally, so the log's last word claimed a cold start; then `hb1`
threw a `std::runtime_error` that **nothing catches** — `main()` has no handler and
`Application::run()` did not wrap `runNetwork()` — so MSVC's `std::terminate` →
`abort()` fast-failed with `0xC0000409` **and the exception text, which held the
reason, was printed nowhere.** The 25.8 s is the 60-station RINEX load; the refusal
fired exactly where it should, before the first epoch.

Fixed on branch `worktree-rxstep`: one refusal policy
(`NetworkStateSnapshotIO::warmResumeRefusalMessage`) for all six branches, the
COLD-START warnings suppressed under `require`, and the throw caught at the
`runNetwork()` call site so the run ends with **one clear error line naming the
reason and the remedy, and exit code 1**. Reproduced end to end and verified (§11.6).

**The cause of the trap is unchanged and still worth knowing:** a leg writes its own
snapshot at its first emitted epoch — `state_snapshot_interval_s` does not suppress
that write — so after one run `seed.txt` holds the leg's own second epoch and the
relaunch resumes to an epoch the snapshot already contains. **`python
scripts/event_leg.py rerun <legdir>`** restores the pristine snapshot from
`leg_meta.json` first; never invoke the exe on an existing `leg.json` directly. The
refusal is now a clean, self-describing exit rather than a crash, but it is still a
refusal.

## 7. Byte-identity

`network.inject` absent (or `enable` false, or an empty `events` array) leaves
`NetworkInjectState::armed` false. `networkInjectEpoch` then returns a reference to
the caller's own `SyncedEpoch` — **no copy is taken at all** — the anchor election and
the RAIM seam see a null/empty directive, and the manifest keys are emitted only when
armed (the `mdb` / `adm_screen` precedent, because the decomposition golden hashes
that file).

## 8. Known gaps — what is NOT injectable

* **Only the six kinds above.** Notably absent: multipath, a station-wide outage, an
  ephemeris/orbit error, a troposphere step, an antenna change, and a *partial*
  signal degradation (the harness drops or shifts, it does not deweight).  (A
  receiver clock jump was on this list until §11.2 added `rx_clock_step`.)
* **`arc_erase` rides the RAIM seam and its attribution pass.** Three conditions
  must hold: the seam runs (`raim_on && (carry_warm || raim_ci_on)`), the attribution
  cell pass runs (`need_attrib = raim_attribution || trace_flag_rates`), and the cell
  pool is non-empty. The goldens satisfy all three. Where one is closed the event
  cannot fire — and, correctly, the run then fails at run-end rather than reporting a
  null. Both decision-side kinds RE-STAGE their directive every epoch until the owning
  seam actually acts, so the event fires at the first epoch at or after `tow` where
  that seam runs; the event row records the epoch it really landed on.
* **`anchor_handover` is a one-epoch ban.** The retired incumbent is eligible again
  the next epoch; whether it is re-elected is the sticky rule's business, not the
  harness's. There is no "retire for N epochs" form.
* **`sat_clock_step` is a step, not a drift.** A rate would need a second field and a
  per-epoch accumulation; the algebra is the same.
* **The injector is outside the warm-resume fingerprint and the snapshot** — by
  design (two legs off one snapshot must differ ONLY in their schedule). The
  consequence is that a leg that itself writes a snapshot passes NO record of its
  corruption downstream. Do not chain legs.
* **Slip repair is not modelled.** A `slip` shifts the observable forever; the
  producer's response is what is being measured. There is no "the receiver
  re-acquires cleanly at +N" form.

---

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

## 11. Cards A and B — the `require` hard-fail, and the receiver-clock step

**Date 2026-09-10, branch `worktree-rxstep`.**  Two gaps §6b and §8 named, closed
together because the second one cannot be graded without the first: Study 2b's legs
are relaunch-heavy and every relaunch hits §6b.

### 11.1 Card A — `network.state_snapshot_require` now MEANS refuse

§6b reported that the non-positive-age refusal "is not wired to the HB-1 hook: it
warns, falls through to a COLD START, and the run then died `0xC0000409` 25.8 s
later."  Re-read against the source, the diagnosis sharpens and the fix changes:

* The branch **was** wired (`NetworkRunContext.cpp`, `hb1("gate rejected the
  snapshot: " + gate)` — `dee2824`).  The run was genuinely refused.
* What was broken is that **the refusal was unreadable and unsurvivable**.  The
  `log->warn(... COLD START ...)` line fired FIRST and unconditionally, so the last
  thing in the log claimed a cold start; then `hb1` threw a `std::runtime_error`
  that **nothing catches** — `main()` has no handler and `Application::run()` does
  not wrap `runNetwork()` — so MSVC's `std::terminate`→`abort()` fast-failed the
  process with `0xC0000409` **and the exception text, which held the reason, was
  never printed anywhere.**  The 25.8 s is the 60-station RINEX load, i.e. the
  refusal fired where it should: before the first epoch.

So "not wired" was the right symptom read from the outside and the wrong mechanism.
The fix is three parts, all on the error path only:

1. **One policy, one place.**  `NetworkStateSnapshotIO::warmResumeRefusalMessage
   (require, WarmResumeRefusal, detail)` is now the only thing that decides what a
   refused resume means; `warmResumeAllowed` gained an optional out-parameter that
   returns the refusal **class** (`Fingerprint` / `NonPositiveAge` / `Stale`) so the
   caller acts on the class, not on a substring of the prose.  Every branch —
   missing file, parse failure, no live epochs, gate rejection, emitter-restore
   failure, and the invariant-carry blob that will not bind — goes through it, so
   `require` cannot mean different things on different branches again.
2. **The COLD-START warnings are suppressed under `require`.**  Announcing a cold
   start the run is about to refuse is exactly the confusion §6b reported.
3. **The throw is caught at the `runNetwork()` call site**, which logs `e.what()` as
   the ONE error line and returns 1.  A refused leg now ends with a readable line
   naming the reason and the remedy, and `exitcode.txt` says `1`, not `-1073740791`.

The non-positive-age class carries its own remedy text (`event_leg.py rerun`,
§6b) because it is the one refusal an operator reaches by doing something
reasonable.  Nothing on the success path is touched, and `require: false` behaves
exactly as before (the message builder returns empty, the warnings still fire).

Tests: `tests/test_warm_resume_require.cpp`, 6 cases — the gate classifies a
relaunch-in-place as `NonPositiveAge`, each gate refusal gets its own class, every
class is fatal under `require` and none is without it, the harness remedy appears
on that class and on no other, and the gate's own prose survives into the fatal
message.

### 11.2 Card B — `rx_clock_step`, the dual of `sat_clock_step`

```json
{"kind":"rx_clock_step","station":"S21000SIM","at_ep":10,"step_m":0.5}
```

From the event epoch on, **every** code and phase observable of **every** satellite
at that station carries an additional constant `+step_m` metres.  Same site as the
other observation-side kinds (§2.1), same persistence rule as `slip` and
`sat_clock_step` (a step, re-applied every epoch — a receiver that jumped its clock
does not jump back), same never-fired hard error, same `INJECT` event row
(`rx_clock_step_onset;sats=N;rows=M`) and the same generic run-end ledger.

It is EXACT for the same reason `sat_clock_step` is.  With
`y = L̃ − (ρ + c·δt_r − c·δtˢ + …)`, adding `s` metres to every code and `s/λ`
cycles to every phase observable

* of ONE satellite at every station **is** `+s` on the a-priori satellite clock, and
* of EVERY satellite at ONE station **is** `+s` on that station's receiver clock —

the same scalar write swept over the other index of the same residual.  The two
kinds now share one helper (`addRangeStep`), so they cannot drift apart.

The loader **refuses** a `sat` or a `signal` on this kind rather than ignoring it: a
receiver-clock jump is station-common and non-dispersive by construction, and a
field that reads as a scope but is not one would arm a different event than the
operator wrote — the same rule as the rest of the parse contract.

Tests: 5 new cases in `tests/test_network_inject.cpp` — the kind parses with no
sat/signal, all four of those malformed forms fail the load, one station's every
satellite moves by the same metres on code and the same cycles on a shared band
while a second station does not move at all, the delta is constant across epochs
and absent before `tow`, and an absent station is a hard run-end error.

### 11.3 A new instrument: the jump gate in metres

`network.carry_rx_clock_jump_k` is 10 σ on the one-step prediction, and `var_pred`
is per station-epoch (harvested variance + drift extrapolation + q), so "the gate"
is a distribution, not a constant — and `k` alone cannot say whether a given step is
above or below it.  The run-end F3 line now carries, armed-only, the census
`k·√var_pred` mean / min / max, the largest |innovation| seen, and the refusal
count.  Measure-only; an OFF run moves none of it.

### 11.4 Study 2b — PRE-REGISTRATION (written before any ON leg was run)

Six legs, quiet bed, `state_snapshot.txt.ep0665` (tow 279150), 61 epochs, event at
+10 (tow 279480), station **S21000SIM**, graded on satellite **G10** for continuity
with §6 and §10.  OFF legs resume the OFF seed (fingerprint `6a3531a1afb2b40b`), ON
legs the ON seed (`8a849480e7ef02af`) — §10.1's rule, re-verified on the copies used
here.  Each event leg is graded against **its own arm's null**; ON-null − OFF-null is
the noise floor.

Predictions, in advance:

* **OFF absorbs ≥ 99 % of the step in the estimated receiver clock by +1 epoch**,
  with the other arcs' frame reaction at the null floor.  Precedent: §6(6)'s
  `sat_clock_step` absorbed **100.4 %** in one epoch with P(L1C) moving 0.6 mm.
  ⚠ One station's step is 1/N network-common, so a 60-station contrast can only
  show `(1 − 1/N)` ≈ 98.3 % of it; the RAW station clock-error difference is the
  unbiased channel and both are reported.
* **ON above the gate**: exactly **1** jump refusal, at the event epoch and at that
  station (`n_rxclk_carry_refused` 0 → 1); absorption and frame reaction equal to
  OFF within the null-vs-null floor.
* **ON below the gate**: the measurable cost.  The carried prior does not refuse the
  step, so it fights it; the leak is quoted into the other arcs' levels and the
  consumer.
* 0.5 m is expected **above** the gate and 0.05 m **below** it; §11.3's census
  decides that from the run rather than from the knob.

Verdict shape, fixed in advance: (a) harmless above the gate and harmful below it —
then the gate value matters and the leak is reported against step/σ; (b) inert on
its own target event too — then KEEP_OFF **with a mechanism**; (c) helpful.

### 11.5 The gate census refuted half the pre-registration before the ON legs were graded

`network.carry_rx_clock_jump_k` is 10 σ, and §11.3's census says what that is in
metres on this seed — **identical in all three ON legs** (variances do not depend on
observation values, so this is a property of the posture, not of the event):

```
rx-clock JUMP GATE: k = 10.0 sigma over 3660 station-epoch test(s)
  => gate mean 7.9863 m, min 3.0261 m, max 300.0161 m;
     largest |innovation| seen 9.5735 m; 0 refusal(s).
```

The 300 m tail is the documented first-increment case (an uninformative rate prior,
σ_d = 1 m/s over 30 s ⇒ ~30 m, ×10); 60 of the 3660 tests are those, and removing
them puts the SETTLED gate at ≈ **3.1 m**, consistent with the 3.0261 m minimum.

So **0.5 m is not above the gate — it is 6× below the tightest station-epoch on the
seed**, and 0.05 m is 60× below.  The pre-registered "ON above the gate" arm was
therefore unreachable at the default `k`, and both event sizes land in the
"below the gate" arm.  Rather than report a vacuous half-experiment, one more pair
was added, **pre-registered here before it was run**:

* **7. `rx_clock_step` 10 m OFF, 8. `rx_clock_step` 10 m ON** — 10 m (33 ns) is a
  perfectly ordinary receiver-clock jump and is above the ≈3.1 m settled gate.
  Predictions: OFF absorbs 100 % again (a sink with no gate cannot notice size); ON
  produces **exactly 1** `n_rxclk_carry_refused` at the event epoch, and the question
  the card actually asks is whether that refusal buys anything the OFF arm did not
  already have.

### 11.6 The table

Eight legs, 61 epochs each, all exit 0, all `1 event(s) armed, 1 fired
(rx_clock_step=1), 0 never fired`, all ledgering
`INJECT,rx_clock_step,,S21000SIM,<step>,,injected,rx_clock_step_onset;sats=18;rows=108`
(18 satellites × 3 bands × {code, phase} = 108 observables at that station, and at no
other).  `absorb` is `clk_raw_d / step_m` — the fraction of the injected step the
station's estimated receiver clock took, on the RAW clock error, not the contrast
(§11.4's 1/N caveat: the contrast can structurally show at most 59/60 of it, and it
does, to five digits: `clk_d` 0.49167 for a 0.5 m step).  `oth_d` is the station's
OTHER arcs' mean |level error|, event − null — the frame reaction.

| arm | absorb +0 | +1 | +2 | +45 | res rms (m) | oth_d rms (cyc) | ZWD_d pk (m) | ion_d pk (m) | Call_d rms (m) | cons_d +45 (m) |
|---|---|---|---|---|---|---|---|---|---|---|
| **null floor (ON null − OFF null)** | — | — | — | — | — | **0.0016575** | −0.000588 | −0.005882 | 0.0283 | **0.0218** |
| 0.05 m OFF | **1.000000** | 1.000000 | 1.000000 | 1.000000 | 1.4e-06 | 6.8e-06 | 8e-06 | 1.1e-05 | 1.1e-07 | 0.0000 |
| 0.05 m ON | 0.998960 | 1.000080 | 1.000080 | 1.000020 | 1.4e-06 | 4.9e-06 | 2.5e-05 | 3.5e-05 | 1.9e-06 | 0.0000 |
| 0.5 m OFF | **1.000000** | 1.000000 | 1.000000 | 1.000006 | 2.4e-06 | 1.5e-05 | 1.4e-05 | 5.4e-05 | 1.2e-06 | 0.0000 |
| 0.5 m ON | 0.998960 | 1.000090 | 1.000090 | 1.000026 | 1.5e-05 | 1.7e-05 | 2.5e-04 | 3.5e-04 | 1.9e-05 | −0.0001 |
| 10 m OFF | **1.000000** | 1.000000 | 0.999997 | 0.999998 | 3.1e-05 | 3.8e-05 | 3.9e-05 | 4.5e-05 | 4.6e-06 | 0.0000 |
| 10 m ON *(1 refusal)* | 0.998962 | 0.999945 | 0.999979 | 0.999999 | 5.6e-05 | 1.8e-04 | 5.0e-03 | 7.0e-03 | 4.0e-04 | −0.0018 |

**Read the first row against the rest.**  Every number in every event row except the
clock's own is **at or far below** the two nulls' difference, which is the floor below
which nothing means anything.

**1. OFF absorbs the step 100.000 % in the epoch it lands, at every size.**  Not at
+1 — at **+0**, the epoch the corrupted observables arrive.  Faster and cleaner than
§6(6)'s `sat_clock_step` precedent (100.4 % in one epoch, P(L1C) −0.6 mm).  Settled
residual 1.4–31 µm on steps of 0.05–10 m, i.e. **3–6 ppm of the step**.  And nothing
else in the network moves at all: the station's other arcs 5e-06…3.8e-05 cyc (**44–220×
below the null floor**), its ZWD ≤ 3.9e-05 m, its slant iono ≤ 5.4e-05 m, the emitted
satellite clocks 1e-06…5e-06 m rms with a per-epoch MEAN shift of ≤ 5e-06 m, and the
consumer 0.0000 m to four decimals.

That is the mechanism §10.5 named, now measured on its own event: **the network's
receiver clock is a free per-epoch parameter with no temporal prior, so a
range-direction step at one station is EXACTLY estimable there and leaks nowhere.**
The event is not hard; it is *free*.

**2. ON is not neutral — it is worse, by exactly the carried prior's share.**
`absorb_0` is **0.998962 at all three sizes**, to five digits.  That scale-linearity is
the whole story: the carried prior takes a fixed **0.104 %** of whatever step arrives
(0.5 mm at 0.05 m, 5.2 mm at 0.5 m, 10.4 mm at 10 m), because that is its weight in
that epoch's normal equations.  The frame reaction grows with the leak (`oth_d` rms
3.8e-05 → 1.8e-04 cyc at 10 m; ZWD peak 3.9e-05 → 5.0e-03 m; consumer 0.0000 →
−0.0018 m).  All of it is still below the null floor, so the honest statement is **no
measurable benefit and a measurable-but-sub-floor cost** — but the SIGN is unambiguous
and it is the same sign at every size.

**3. The jump gate is too loose to see a realistic receiver step, and its response is
one epoch late by construction.**  §11.5's census puts the settled gate at ≈ 3.1 m
(k = 10 σ ⇒ σ ≈ 0.31 m), so 0.5 m is 6× below it.  The 10 m pair reached it and behaved
exactly as pre-registered on the counter — **`n_rxclk_carry_refused` 0 → 1**, and the
gate-test count 3660 → 3659 because the erased station has no predecessor to test
against on the following epoch, which is the decoupling and re-coupling edges both
showing up in one number.  But the refusal fires at **harvest, after the solve**: the
epoch the step lands on has already folded the stale prior, so the 10.4 mm leak at +0
happens **with the refusal armed**, and the gate only cleans up from +1 (0.55 mm) on.
A jump response that cannot act on the epoch of the jump cannot beat having no prior
at all — which is what the OFF column is.

⚠ **OPEN (invariant #7):** the refusal has a counter and **no identity** — no event
row, no station, no epoch.  On this bed the attribution was available only because
exactly one station was stepped and the count went 0 → 1.  On a field lineage
"N refusals" cannot be attributed to a receiver, and that is the gap to close if this
gate is ever armed for real.

### 11.7 Verdict

**`network.carry_rx_clock_in_block` is KEEP_OFF — outcome (b), inert on its own target
event, with a mechanism.**

The receiver-clock step was the one event this card was designed for, and the producer
already handles it perfectly without the card: the station's free per-epoch clock
absorbs 100.000 % of the step in the epoch it arrives, at 0.05 m, 0.5 m and 10 m
alike, and no other state in the network moves by more than a few hundredths of the
null-vs-null floor.  Arming the carry makes that *worse* by a scale-linear 0.104 % of
the step, because the carried prior is a stiff opinion about a quantity the data
determines exactly, and the jump gate — 3.1 m at the settled k = 10 σ, i.e. blind to
anything smaller than a 10 ns receiver step — fires only after the solve that already
paid the leak.  §10 found the knob inert on slip, `arc_erase` and a cold handover and
closed with "inert where measured, untested where designed"; it is now tested where
designed, and the answer is the same with the sign against it.

What would change this verdict is a receiver step the OFF arm CANNOT absorb — one
large enough to break the linearization or to trip the producer's own admission
screens (a 1 ms / 299 792 m jump is five decades above the gate), or a station whose
geometry is too thin for the clock to be estimable epoch-by-epoch.  Neither is
reachable on this bed, and neither is this knob's advertised regime.

### 11.8 Byte identity, the suite, and reproducing

* **Byte identity (invariant #6).**  `config/test_suite/_golden_sim_off60.json` with
  ONLY `network.product_output_path` repointed, 61-epoch cold golden smoke, this
  branch's Release exe (`gnss_processor_rxstep.exe`, sha256 `207C5374…`):
  `products.txt` sha256
  **`3BF7831B29916C7A1E20434AC91F2ADFEB74EB06974560DF6E050FC43AC3F357`** — the exact
  hash §6 and §10.7 record for main HEAD.  Both cards are error-path-only or
  strictly armed-only, so an unarmed run cannot reach either.
* **Suite: 1796 cases / 245 suites, 1779 passed, 0 failed, 17 skipped** (fixture-gated;
  `data/` is not bootstrapped in this worktree) — main's 1785 plus this branch's 11.
* **Card A verified end to end**, not only by unit test: a leg relaunched in place
  (`runs/_hb1probe`, a copy of a run `null_off` leg whose `seed.txt` its own run had
  already overwritten) now exits **1**, prints the single line

  > `[main_log] [error] Network (RTB-7d/HB-1): network.state_snapshot_require is ARMED
  > and the snapshot is NOT OLDER than the first epoch this run would process — … (age
  > 0.000 s …).  The run is REFUSED BEFORE ITS FIRST EPOCH … Re-seed … event_leg.py
  > rerun <legdir> …`

  and emits **no** `COLD START` line.  Before the fix the same input printed
  `COLD START` and fast-failed at `0xC0000409`.

```powershell
# the legs (OFF arm on the OFF seed, ON arm on the ON seed — §10.1's fingerprint rule)
python scripts\event_leg.py make-leg --seed-run runs/evseed_quiet6h --snapshot-ep 665 `
       --epochs 60 --name s2b_rxstep50_off --at-ep 10 `
       --event '{"kind":"rx_clock_step","station":"S21000SIM","step_m":0.5}'
python scripts\event_frame_grade.py runs/evleg_s2b_rxstep50_off --null runs/evleg_s2b_null_off `
       --fixture C:/GnssProcessor/runs/simnet60_quiet_wu93_24h --sat G10
```

`--sat` is required for this kind: a station-wide step names no satellite, which is the
event.  `--step-m` overrides the leg's own `step_m` (use it to grade a non-step leg on
the same axis, or to check the arithmetic).
