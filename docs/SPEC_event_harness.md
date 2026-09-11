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
| `arc_erase` | `station, sat, tow` [, `mode`, `signal`] | runs the T4 attribution refloat for that (station, sat) — appended to `bad_arcs` so it executes **the same code** a real attribution does (carry erase, iono-carry erase, pending-reseed, C3c queue).  The optional DIAGNOSTIC `mode` (§9.3) subtracts ONE of those actions, or (`free_epoch`) routes the event to a detector refloat instead; the optional `signal` is a `;`-separated BAND SCOPE.  Defaults (`"erase"`, every signal) are the production path. |
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
| `src/core/config/GnssConfigLoadNetwork.cpp` | `loadNetworkInject_` (strict: per-index errors, 4096-event paste-accident cap — raised from 64 for the §12 dose schedules, per-kind required fields) |
| `include/app/NetworkRunContext.hpp` | `NetworkInjectState inject` |
| `src/core/app/ApplicationNetwork.cpp` | arm + the per-epoch seam + the anchor-handover readback |
| `src/core/filter/NetworkPhaseDatum.{hpp,cpp}` | the anchor candidacy ban + forced-cold branch |
| `src/core/filter/NetworkRaim.cpp` | the injected T4 erase |
| `src/core/filter/NetworkTeardown.cpp` | run-end clause + the never-fired exit code |
| `src/core/logging/NetworkRunTraces.cpp` | manifest keys (armed-only) |
| `scripts/event_leg.py` | `write-seed` / `make-leg` / `null` / `rerun` |
| `scripts/event_grade.py` | the event-minus-null grader |
| `include/slip/NetworkSlipQc.hpp`, `src/core/slip/NetworkSlipQc.cpp` | §9: `networkSlipDrainInjectedFree` — the injected `free_epoch` response (a PASS-A drain, beside the real detectors) |
| `src/core/app/ApplicationNetworkAssembly.cpp` | §9: the drain's call site (after the slip drains, before the PASS-B leveling inject) |
| `scripts/ev_study1_legs.py`, `scripts/ev_study1_table.py` | §9: the Study-1 leg matrix + summary table |
| `scripts/ev_launch.ps1` | detached launcher (per-run cwd, always writes `exitcode.txt`) |
| `tests/test_network_inject.cpp` | 28 cases: parse contract + epoch semantics (5 of them `rx_clock_step`) |
| `tests/test_warm_resume_require.cpp` | 6 cases: the HB-1 `state_snapshot_require` policy (§11.1) |
| `scripts/event_frame_grade.py` | the station-FRAME grader; `--step-m` adds the absorption / residual channel |
| `scripts/ev_dose_legs.py` | §12: slip-DOSE schedules (campaign statistics, Poisson or fixed count; `--from-leg` + `--stations` / `--thin` carve identical-event subsets; `--extra` attribution twins) |
| `scripts/event_dose_grade.py` | §12: the multi-event grader — folds EVERY injected slip into the event leg's truth, station-frame trajectories for dosed stations and the bystander pool, per-slip residual table, coverage block, (tier, event, action) census |
| `scripts/ev_slip_census.py` | §13.1: detector-coverage census over a dose leg's injected slips — what caught each one (tier / pair / lag), by band, flag and elevation; the inert-injection filter (no raw row ⇒ outside the solved set) |
| `scripts/ev_mw_detect_census.py` | §13.4: census of the MW detector's own decision lines (`logging.enable_mw_slip_detector`; reads the rotated `mw_slip_detector.N.log` too) — lane, arc age, evidence ratio, integer known / rounding residual; prices candidate refloat gates against the false-alarm population |

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
* **`arc_erase` mode / band scope are HARNESS-ONLY.**  They gate what the
  INJECTED erase does; a real attribution takes the production path on every
  mode, and an unarmed run reaches none of it (§9.3).  Two of the four modes
  (`erase_keep_iono`, `erase_no_funnel`) are structurally inert on the sim
  golden's posture and byte-identical to `erase` there — see §9.4.
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

## 9. Study 1 — erase vs refloat: what actually costs the 0.45-cycle floor

**Answer, up front: none of the four actions the T4 erase performs and a detector
refloat does not. The floor is bought by the BAND SCOPE — freeing (or erasing)
ALL of a satellite's signals at one station at once. Restrict either path to 2 of
3 bands and the 0.445-cycle floor becomes 0.050.** Measured 2026-09-10, 14 legs,
~14 min of wall clock.

### 9.1 The question

§6(2) and §6(3) measured, on the SAME healthy arc, that a detector-handled
10-cycle `slip` leaves ~0.04 cyc of residual level error while an `arc_erase`
leaves a peak −1.9 cyc and a persistent ~0.45 cyc floor from +14 through +45 —
11×, with ZWD +3.8 mm. On the slipped storm bed the attribution tier erases ~21
arcs per station-hour, so that floor is the ambient 1.7-cycle plateau priced per
event. Which extra thing does the erase do?

### 9.2 The exact difference list (read from the source, not inferred)

`refloat_sat_at` in `src/core/filter/NetworkRaim.cpp` (~:751-880), per matching
ambiguity of the flagged (station, satellite):

| # | action | T4 erase | detector refloat (`refloat_signal`, `slip/NetworkSlipQc.cpp` ~:415-470) |
|---|---|---|---|
| 1 | `LocalCarry` entry | **`local_carry.erase(...)`** — deleted; the re-stash is a NEW entry (birth reset, `var_first` reset, F3 block membership restarts) | **KEPT** |
| 2 | LOS iono carry | **`ctx.carry.iono_carry.erase({i, sat})`** (`n_iono_carry_erased_t4`) | not touched |
| 3 | pending-reseed | `markAmbiguityPendingReseed(amb_key)` | `markAmbiguityPendingReseed(amb_key)` — **the same call** |
| 4 | C3c funnel | `raim_refloat_pending.insert(...)` under `raim_ci_refloat`, consumed NEXT epoch | `carry_slipped_ambs.insert(...)` under `carry_invariant`, consumed THIS epoch |
| 5 | free-epoch latch | **none** — the RAIM path never touches `free_ambs` | **`relevel->free_ambs.insert(...)` + `free_cause`** — the set the PASS-B leveling inject actually reads |
| 6 | bridge pend | none | voids any outstanding `relevel_pend` on that (station, sat) |
| 7 | seam / timing | POST-fit (`networkPostFitRaim`), so the event epoch's own solve is untouched and the damage starts at +1 | PASS A, so the event epoch's own solve is already freed (visible at +0) |

Two of those are not differences at all: **(3) is the same call on both paths**,
and **(4) is the same idea on two vehicles**. The real asymmetry is (1) vs (5):
the erase *removes the level*, the refloat *keeps the level and skips injecting it
for one epoch*. Both end with the arc re-levelled from one epoch of data — which
is the point the measurement lands on.

### 9.3 The diagnostic sub-knobs (harness-only)

`network.inject.events[].mode` on kind `arc_erase`, default `"erase"` = the
production path, so an event that names no mode is exactly what it was before:

| mode | LocalCarry | LOS iono | pending-reseed | C3c queue | free-epoch latch | seam |
|---|---|---|---|---|---|---|
| `erase` (default) | ERASED | co-erased | marked | queued | no | post-fit RAIM |
| `erase_keep_iono` | ERASED | **KEPT** | marked | queued | no | post-fit RAIM |
| `erase_no_reseed` | ERASED | co-erased | **NOT** | queued | no | post-fit RAIM |
| `erase_no_funnel` | ERASED | co-erased | marked | **NOT** | no | post-fit RAIM |
| `free_epoch` | **KEPT** | KEPT | marked | detector's | **YES** | PASS-A drain |

Plus an optional BAND SCOPE: `"signal": "L1C;L2W"` restricts the action to those
signals (empty / `"all"` = every signal, which is what BOTH production paths do).

`free_epoch` is routed to a new PASS-A drain, `networkSlipDrainInjectedFree`
(`slip/NetworkSlipQc.{hpp,cpp}`, called from `ApplicationNetworkAssembly.cpp`
beside the other slip drains). It has to live there, not in the RAIM block:
`relevel.free_ambs` is cleared at the top of PASS A and read by the PASS-B
leveling inject, so a `free_ambs` insert from post-fit RAIM would never be seen.
Run-end census `Study-1 path census (mode=chords acted)` says which path ran —
invariant #4.

### 9.4 The legs

Seed `runs/evseed_quiet6h` `state_snapshot.txt.ep0665` (tow 279150), 61 epochs,
event at +10 (tow 279480), everything graded against the ONE null leg
`runs/evleg_null`. Three arcs, all GPS triple (L1C / L2W / L5Q):

| arc | station / sat | mean elevation |
|---|---|---|
| arc 1 | `S21000SIM` / `G10` (the §6 validation arc) | 14.2° |
| arc 2 | `S50000SIM` / `G31` | **81.3°** |
| arc 3 | `S40000SIM` / `G31` (arc 2's satellite, low station) | 13.8° |

**Two structural gates first.**

* **Byte-identity (invariant #6).** `runs/evleg_s1_nullgate` — a NULL leg on the
  new exe — reproduces `runs/evleg_null`'s `products.txt` SHA256 exactly
  (`95DC556F1EDBB6EBB06D2E6BDC03282F9169887C63BBCEF0…`). The sub-knobs are
  unreachable unarmed, and the §6 validation legs stay comparable.
* **Two of the four actions are STRUCTURALLY INERT on this lineage, and the
  harness proves it rather than assuming it.** `erase_keep_iono` and
  `erase_no_funnel` are **byte-identical to `erase`** (`products.txt`
  `7027918C0DE61D0E11C67646AB023A1179B2AB03…` for all three), because the sim
  golden runs `carry_iono: false` (⇒ `ctx.knobs.iono_on` false, so there is no
  iono carry to co-erase) and `carry_invariant: false` (⇒ `raim_ci_refloat`
  false, so nothing is queued). **The LOS-iono co-erase and the C3c funnel
  therefore CANNOT be the cause of the measured floor** — they never execute.
  What their arms certify is the mode dispatch: exactly the named action moved
  and nothing else did. On a `carry_iono` / `carry_invariant` lineage they are
  live and untested — that is an owed leg, not a closed question.

### 9.5 The table

Arc level error is the chord-median DD error of the arc, **event − null**, in
cycles, from `logs/network_dd_fractional.csv` joined to the fixture's true
integers (`scripts/ev_study1_table.py`). `floor` = mean |e| over +14..+45. ZWD
and iono are event − null too; iono is `iono_total_m` for that (station, sat)
at +45. `T4/T3` counts post-event re-flags on the arc inside 45 epochs.

| leg | peak \|e\| (cyc) | @ep | +14 | +30 | +45 | **floor** | ZWD pk (mm) | Δiono +45 (m) | T4/T3 | cons pk (m) |
|---|---|---|---|---|---|---|---|---|---|---|
| **arc1** `slip` 10 cyc (detector freed L1C;L2W) | 0.054 | +11 | 0.051 | 0.044 | 0.037 | **0.044** | +1.4 | +0.0056 | 0 | −0.001 |
| arc1 `erase` | 1.905 | +2 | 0.415 | 0.588 | 0.417 | **0.445** | +3.8 | −0.0832 | 0 | −0.001 |
| arc1 `erase_keep_iono` | 1.905 | +2 | 0.415 | 0.588 | 0.417 | **0.445** | +3.8 | −0.0832 | 0 | −0.001 |
| arc1 `erase_no_reseed` | 1.905 | +2 | 0.415 | 0.588 | 0.417 | **0.445** | +3.8 | −0.0832 | 0 | −0.001 |
| arc1 `erase_no_funnel` | 1.905 | +2 | 0.415 | 0.588 | 0.417 | **0.445** | +3.8 | −0.0832 | 0 | −0.001 |
| arc1 `free_epoch` (all 3 bands) | 2.018 | +0 | 0.515 | 0.633 | 0.447 | **0.496** | +4.0 | −0.0890 | 0 | −0.001 |
| **arc1 `erase` scoped L1C;L2W** | 0.067 | +1 | 0.059 | 0.050 | 0.041 | **0.050** | −1.9 | +0.0061 | 0 | −0.001 |
| **arc1 `free_epoch` scoped L1C;L2W** | 0.054 | +11 | 0.051 | 0.044 | 0.037 | **0.044** | +1.4 | +0.0056 | 0 | −0.001 |
| arc2 `erase` | 1.052 | +1 | 0.270 | 0.067 | 0.073 | **0.130** | −4.2 | +0.0206 | 0 | −0.011 |
| arc2 `erase_no_reseed` | 1.052 | +1 | 0.270 | 0.067 | 0.073 | **0.130** | −4.2 | +0.0206 | 0 | −0.011 |
| arc2 `free_epoch` (all 3) | 0.434 | +1 | 0.213 | 0.088 | 0.077 | **0.116** | −3.8 | +0.0221 | 0 | −0.012 |
| **arc2 `free_epoch` scoped L1C;L2W** | 0.019 | +1 | 0.007 | 0.004 | 0.003 | **0.004** | −1.8 | −0.0004 | 0 | −0.005 |
| arc3 `erase` | 0.803 | +1 | 0.448 | 0.379 | 0.207 | **0.375** | −2.0 | +0.0405 | 0 | −0.001 |
| arc3 `erase_no_reseed` | 0.803 | +1 | 0.448 | 0.379 | 0.207 | **0.375** | −2.0 | +0.0405 | 0 | −0.001 |
| arc3 `free_epoch` (all 3) | 0.498 | +10 | 0.372 | 0.325 | 0.179 | **0.319** | −2.5 | +0.0353 | 0 | −0.001 |

Per band on arc 1 (floor +14..+45), which is where the mechanism is visible:

| band | slip (det. L1C;L2W) | erase all-3 | free all-3 | erase L1C;L2W | free L1C;L2W |
|---|---|---|---|---|---|
| L1C | 0.044 | 0.445 | 0.496 | 0.050 | **0.044** |
| L2W | 0.052 | 0.598 | 0.664 | 0.061 | **0.052** |
| L5Q | 0.042 | 0.630 | 0.698 | 0.046 | **0.042** |

### 9.6 Verdict

1. **No single erase-path action produces the floor.** Removing the iono
   co-erase, the C3c queue, or the pending-reseed mark leaves the floor at 0.445
   on arc 1 (the first two byte-identically; `erase_no_reseed`'s products differ
   but every graded column agrees to three decimals). Erasing the `LocalCarry`
   entry is not it either: **`free_epoch` — a literal detector refloat that keeps
   the entry, keeps the iono, queues nothing and latches the free epoch —
   reproduces the floor and is marginally WORSE on arc 1 (0.496 vs 0.445).** The
   premise "the erase path is the expensive one" is falsified. Both paths cost
   the same because both end in the same place: the arc re-levelled from one
   epoch of data.
2. **The variable that matters is BAND SCOPE.** Restricting either path to 2 of
   the 3 bands drops the floor 9× (erase 0.445 → 0.050) and 11× (free 0.496 →
   0.044) on arc 1, 29× on arc 2 (0.116 → 0.004), and does it on all three bands
   at once — including L5Q, which was never freed. The scoped `free_epoch` leg
   reproduces the real-`slip` leg to three decimals on every band (0.044 / 0.052
   / 0.042), which is the identity check the mechanism predicts: the §6(2) slip
   was cheap **because its `T1_if_slip` detector acted on `L1C;L2W` and left L5Q
   pinned**, not because a slip response is intrinsically gentler than an erase.
3. **The mechanism.** Freeing every band of a LOS removes that LOS's last level
   anchor, so (ρ, I, N₁, N₂, N₃) must be re-drawn together from one epoch of
   code — the amb/iono split over-count (`SPEC_amb_iono` F1) at its worst, and
   standing invariant #1's re-coupling edge verbatim: *"a refloat that re-levels
   from ONE epoch of code CREATES a fresh zero-q value seeded with
   station-common iono error."* The iono column is the direct witness: −0.083 m
   of slant-iono error still standing at +45 on the all-3 arms against +0.006 m
   scoped. Leave ONE band pinned and the two re-tapped levels land on the
   survivor's ionosphere instead of on code noise.
4. **It scales with elevation, not with the path.** floor(erase): 81.3° arc
   0.130, 13.8° arc 0.375, 14.2° arc 0.445 — the one-epoch re-level is only as
   good as that epoch's code, and a low LOS has the worse code.
5. **The QC tiers stay silent.** 0 post-event T4/T3 re-flags on the arc in every
   leg, on both paths: the arc that has just been given a metre-class level error
   is not flagged for it. Invariant #7's re-coupling edge with no instrument.
6. Consumer 3D (event − null) is ≤ 12 mm on every leg, so one event is a wire
   defect long before it is a consumer defect; the plateau is the accumulation of
   many.

**The production change this points at** (not made here — the sub-knobs are
injector-only): band-scope the T4 attribution erase and the whole-satellite
detector refloats — refloat the implicated bands and leave the least-suspect
band's level pinned as the LOS's iono anchor — which is the "band-scoped
refloat" already named as the open build in `SPEC_lane_slip_response`.

### 9.7 Reproducing

```powershell
python scripts\ev_study1_legs.py make                       # 14 leg.json
python scripts\ev_study1_legs.py run  s1_free s1_keepiono s1_noreseed   # <=3 at a time
python scripts\ev_study1_table.py                           # the table above
```

Wall clock: 150–163 s per leg with 3 concurrent (211 s in the one wave that
overlapped another session's build), 14 legs in ~14 min; Release build 379 s
(+123 s incremental for the band scope), Debug suite 367 s.
Unit suite after the card: **1716 cases / 241 suites, 1699 passed, 0 failed, 17
skipped** (fixture-gated; `data/` is not bootstrapped in this worktree) — the
card adds one epoch-semantics case (`ArcEraseStudy1ModeAndBandScope`) and two
parse-rejection entries.

**Caveats.** (a) One quiet sim bed, three arcs, 60-epoch legs — the floor is
measured out to +45, not to convergence. (b) The iono co-erase and the C3c
funnel are inert here; their arms are certification of the dispatch, not of the
actions. (c) `mode` gates the INJECTED erase only; a real attribution takes the
production path on every mode, and an unarmed run cannot reach any of it.
(d) The `free_epoch` legs move at +0 and the erase legs at +1 — that is the two
seams' real timing (PASS-A drain vs post-fit RAIM), not a grading offset.

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

## 12. Study 4 — the slip DOSE: does a CONVERGED network survive the campaign's slip rate?

*Pre-registered 2026-09-10 evening, local (the snapshot files carry 2026-09-11 UTC stamps), Fable manager session, BEFORE any dose leg was graded.
The legs were generated first, the gates written second, the numbers read third.*

### 12.0 Why this study replaced "lane-evidence band choice"

§Z.7 closed with "choose the kept band on evidence that can see a level error —
the lane integers".  The campaign bed's own numbers refuse that before anything is
built: on `runs/golden_sim_campaign_zwdblk` the ref-free lanes are RIGHT
(EWL(2,5) 99.2 %, WL(1,2) 86.2 % at their own true integer) while the per-band
levels are WRONG (L1C 44 % at integer, ambient per-band DD error 1.7 cycles,
per-chord medians 2.9–3.4 cycles per band).  A level error that leaves both lanes
intact is one that is EQUAL IN CYCLES across the bands — exactly the (ρ, I) split
direction §6.17 identified (δρ ≈ −1.28 δI) — and the pure-iono direction Study 2
measured on the erase floor moves WL by only ≈ 0.13 cycle per 0.45-cycle level
error and EWL by ≈ 0.03.  The lanes are therefore blind to the very error the
band choice has to see; the item is dropped, not deferred.

What the campaign statistics DO say (measured from the fixture's 61
`truth_slips.csv`): 2486 slips in 5.98 h = **6.8 per station-hour**; 2476 of
2481 events are **single-band**; |Δ| 12–125 cycles (p50 50); 50.6 % LLI-flagged.
Study 1 priced a detector-handled single-band slip at **0.04 cycle** and an
all-band erase at **0.45 cycle for 45+ epochs**; T4 fires 21 times per
station-hour on the storm bed against 6.8 true slips.  Every storm arm starts
COLD on a bed that is already slipping, so no arm has ever asked whether a
network that converged first would decay under the same schedule.  That is the
question a harness can answer in minutes and a storm bed cannot answer at all.

### 12.1 Mechanics

`scripts/ev_dose_legs.py make` resumes the quiet seed (`evseed_quiet6h`, ep0665,
the snapshot every validation leg used) and injects a Poisson schedule of
single-band slips sampled from the campaign's empirical |Δ| population, band
uniform within the constellation, LLI on half, elevation ≥ 15°, no two events on
one (station, sat) within 20 epochs, none in the last 30 epochs (every slip gets
an aftermath).  Same `--rng` ⇒ same schedule, so an attribution twin changes one
knob and nothing else.  The injector cap moved 64 → 4096
(`GnssConfigLoadNetwork.cpp`, `kMaxInjectEvents`; a paste guard, not a design
statement).  Grader: `scripts/event_dose_grade.py` (folds EVERY injected slip
into the truth; station-frame trajectories for the dosed stations and the
bystander pool; the (tier, event, action) census split into injected vs other
arcs).  Exe: `gnss_processor_dose.exe` = main HEAD + the cap (3760bc71…);
its null must reproduce the gate null (`evleg_gate4_null`, 95dc556f…, itself
byte-identical to the merged harness null) over the shared 62 epochs.

Legs (all 240 epochs = 2 h, ~7 min each; 3 concurrent at most):

| leg | schedule | question |
|---|---|---|
| `evleg_null240` | none | the control every diff is read against |
| `evleg_dose1_s21` | S21000SIM alone, campaign rate (9 events, rng 1) | does ONE station's frame walk under its own churn? |
| `evleg_doseall` | all 60 stations, campaign rate (655 events, rng 1) | does the converged NETWORK decay? |
| conditional `dose3_s21` | S21000SIM, 27 events | dose–response, if dose1 is ambiguous |
| conditional `dose1_s21_not4` + matched `null240_not4` | same rng-1 schedule, `raim_arc_persist_epochs: 1e6` (E1's starve — `raim_attribution: false` is silently RE-ARMED by the golden's `raim_within_epoch`, loader line ~4752) | is the walk the T4 chain or the detector re-levels? (only if T4 fires ≥ 3× on S21 in dose1) |
| conditional `doseall720` | all stations, 6 h | if doseall has a trend at +240 that has not saturated |

### 12.2 Hypotheses and gates (frozen)

Read at the last bin (+220..+240), event − null, dosed station(s) and bystanders:

- **H1 — the plateau is an IGNITION artifact.** A converged network absorbs the
  campaign's slips at Study 1's per-slip cost and does not decay.  Prediction on
  `doseall`: dosed-pool median |level error| diff < 0.15 cycle, ZWD rms diff
  < 5 cm, consumer 3D diff < 10 cm, T4 census ≤ 10 rows network-wide over the
  leg.  If H1 holds, the storm-bed plateau is a CONVERGENCE problem (how to
  converge while slipping), not a slip-response problem, and every response
  knob falsified on the storm bed was being graded against a network that
  never converged.
- **H2 — station-local walk.** The dosed station's frame walks under its own
  churn.  Prediction on `dose1_s21`: ZWD |diff| > 10 cm OR median |level error|
  diff > 0.5 cycle OR ≥ 6 T4 rows on S21000SIM, with bystanders flat (their
  pool diff within the null-vs-null floor).
- **H3 — network-coupled walk.** `dose1_s21` flat, `doseall` decays (the walk
  needs many stations' re-levels interacting through the anchors / wire).
- **H4 — false alarms drive it.** Detector rows on NON-injected arcs of dosed
  stations exceed the null's by ≥ 2× (offers and actuations separated by the
  `action` column).

Verdict words: **flat** = |ZWD diff| < 2 cm AND level diff < 0.15 cycle AND
T4 rows ≤ 2 on the station over the leg; **walk** = the H2 numbers; anything
between = ambiguous → the 720-epoch leg before any verdict.  A `walk` on
`dose1_s21` makes the harness the development loop for the cure at 7 min per
iteration; a `flat` on `doseall` retires the slip-response family and opens the
convergence card.

### 12.3 First pass (2026-09-10 evening, hand-graded with the trace-complete channels; the dose grader re-grades on landing)

Gate: the dose exe's `evleg_null240` reproduces `evleg_gate4_null` over the 62
shared epochs (0 differing lines of 25 989), and the gate itself reproduced the
merged harness null (95dc556f…).  Byte-identity holds on the null path.

| leg | injected slips | station ZWD, event − null, last bin (+200..+239) | T4 erases | detector rows on injected arcs | bystanders |
|---|---|---|---|---|---|
| `dose1_s21` (S21000SIM, campaign rate) | 9 | **+4.3 mm** (creep 0.4 → 4.3 mm over the leg); rx-clock contrast 1–2 mm; consumer −2 mm | **0** | MW 9 / LLI 4 / IF-TD 5 — 9 of 9 caught | ZWD diff median 0.01 mm, max 0.15 mm (59 sta) |
| `dose3_s21` (S21000SIM, 3× rate) | 27 | **+4.0 mm** | **0** | MW 23 / LLI 12 / IF-TD 13 | — |
| `doseall` (all 60, campaign rate) | 655 | median \|diff\| **221 mm**, rms **1.83 m**; 11 stations > 1 m, 15 > 0.5 m, 45 > 0.1 m (of 60) | **3 099** = 25.8 per station-hour, flat from the FIRST bin (402 / 384 / 779 / 517 / 555 / 452 per 30–40 ep) | MW +581 / IF-TD 350 / LLI 312 | none (all dosed) |

Other doseall counters vs the null: `CLK_ANCHOR` decouple/recouple **258 / 259**
(null 2 / 3), `PBGAUGE handover_cold` **22** (null 5), `CLK_STEP mark` **16** (0),
`T1_if_slip refsat_reset` **78** (0).  Consumer 3D event − null by bin: +0.3, +0.5,
−0.8, −0.4, +0.6, **+1.4 m** (the null's own consumer sits at 0.6–2.4 m on a
resumed wire — §6(1) — so this channel is read for trend only).

**Per-slip price on the converged station** (dose1, signed event − null with the
injected delta folded into the EVENT leg's truth only; GPS arcs only — the DD
trace has no Galileo chords at S21000SIM, its reference E02 is below the horizon
there, and its GPS chords end near +160 when G01 sets: the coverage half of
§6.13): G27 L1C +69 cycles — MW(1,5) and IF-TD(1,2) fire together, so all THREE
bands are freed — residual **−0.65 cycle at +1, −0.53 at +5, −0.40 at +20, −0.11
at +45** (the §9 all-band floor, decaying); G27 L5Q +57 cycles — MW(1,5) alone,
L2W stays pinned — **−1.57 at +1, −0.52 at +5, −0.11 at +10**.  The station's
OTHER satellites: median |diff| 0.01–0.04 cycle per bin (max 0.63, which is the
slipped satellite's other bands re-drawing).  One injected slip (G16 L1C, +152)
drew no detector row at all: G16 is not in the network's solution at S21000SIM
(no chord in the trace either), so the generator must pick satellites the
network USES at the station, not satellites the truth says are visible.

**Where the outlier tier fires on `doseall`, with the base rate** (the honest
half of a lag attribution — at this density every station slips every ~11 min and
every satellite every ~6 min, so "a slip nearby just happened" is nearly always
true; the base rate is 20 000 random (station, sat, tow) triples through the same
ordered categories):

| category (ordered) | T4 erases | base rate | enrichment |
|---|---|---|---|
| same ARC slipped within 30 min | 15.6 % | 4.7 % | **3.3×** — the re-erase chain (§6.17 H-C) |
| same STATION, other satellite, within 10 min | 58.6 % | 59.3 % | none |
| same SATELLITE, other station, within 10 min | 15.8 % | 19.4 % | none |
| none of the above | 10.1 % | 16.6 % | — |

Per station: corr(injected slips, T4 erases) = **0.13** — a station's erase count
does not follow its own slip count (T4 per station 23 / 45 / 157 min / median /
max); corr(T4 erases, |ZWD error| at the end) = **0.68** — where the erases land,
the frame walks (S43 7.6 m, S32 −5.5 m, S28 4.8 m with 56 / 77 / 121 erases;
S06 / S42 / S45 within 25 mm with 27–39).  Erases hit 974 distinct (station,
sat) arcs, 475 of them ≥ 3 times, max 18; 45.7 % landed on arcs that had an
injected slip at all.

**Reading before the twins land.** H1 is falsified: the converged network decays
inside the first 30 epochs of network-wide slips, so the plateau is not an
ignition artifact.  H2 is falsified twice: one station at the campaign rate and
at three times it stays flat (4 mm, zero erases, other satellites at 0.03 cycle).
H3 is what the numbers show: the outlier tier fires only when MANY stations
slip, and it fires on arcs that have nothing to do with the slips (no
same-station or same-satellite enrichment), at a rate that does not track the
station's own churn.  The one enriched category is the arc's own re-erase, 11 %
of the total.  Pending: `doseall_not4` (same 655 events, T4 starved — is the
tier the amplifier or a symptom?), `dose24` / `dose12` / `doseall_half` (is the
onset a density THRESHOLD or linear in the dose?).

### 12.4 The twins and the density ladder (2026-09-10 evening, `scripts/event_dose_grade.py` + the quick frame read)

All legs share the `doseall` schedule (`--from-leg`): a rung is a SUBSET of the same
655 events, so the events themselves are identical across rungs.  Every leg is
graded against `evleg_null240` (the T4-starve knob is inert on the null: it fired
0 times there).

| leg | slips / stations | T4 erases per bin (10–39 / 40–79 / 80–119 / 120–159 / 160–199 / 200–239) | onset | ZWD rms diff at +200..+239 | wire diff (m) at +100 / +180 / +220 | consumer diff, last bin |
|---|---|---|---|---|---|---|
| `dose1_s21` | 9 / 1 (S21, anchors 0 families) | 0 everywhere | never | 4 mm | — | −0.001 |
| `dose3_s21` | 27 / 1 (S21) | 0 everywhere | never | 4 mm | — | +0.000 |
| `dose_s30` | 13 / 1 (**S30 = clock datum**, anchors 9 families) | 0 / 0 / 0 / 45 / 294 / 12 | **+120..+159** | (single station) | 0.0003 / 1.36 / 1.76 | +0.22 |
| `dose12` (incl. S30) | 128 / 12 | 0 / 0 / 50 / 141 / 309 / 12 | +80..+119 | 0.80 m (dosed), 48 bystanders 0.06 m | 0.005 / 1.44 / 1.80 | +0.05 |
| `dose24` (incl. S30) | 262 / 24 | 0 / 0 / 61 / 195 / 293 / 150 | +80..+119 | 1.6 m (dosed), 36 bystanders 0.08 m | 0.019 / 1.79 / 2.09 | +0.51 |
| `doseall_half` | 328 / 60 | 168 / 6 / 443 / 287 / 346 / 174 | +10..+39 | 1.3 m | 2.75 / 3.31 / 3.51 | +0.46 |
| `doseall` | 655 / 60 | 402 / 384 / 779 / 517 / 555 / 452 | **+11** | 1.83 m (median 221 mm) | 3.80 / 4.74 / 5.18 | +1.36 |
| `doseall_not4` (T4 starved) | 655 / 60 | 0 everywhere (by construction) | — | 1.49 m (**median 441 mm**) | 1.21 / 1.73 / 2.21 | +1.34 |

**(1) The outlier tier is a symptom.**  With every T4 erase removed the walk
persists; the median station is WORSE (441 vs 221 mm), the rms is a little
better (fewer blow-ups), the consumer ends the same (+1.34 vs +1.36 m), and the
clock-anchor decouple count is unchanged (267 vs 258).  The detector rows on
NON-injected arcs in `doseall_not4` equal the null's (no false-alarm cascade
either).  What is left as the engine is the 655 detector re-levels themselves.
The levels of untouched satellites drift more without T4 (`oth_d` 5.4 vs 3.0
cycles at the end as-traced) while the wire is SMOOTHER without T4 (2.2 vs
5.2 m): the erases keep the levels closer and chop the wire into steps.
⚠ The as-traced level channels carry the trace's reference satellite (§6.13), and
in a dose leg that satellite's own arcs are re-levelled at many stations, so the
diff no longer cancels it.  `event_dose_grade.py --ref-free` (the
`simnet_dd_reref.py` method: per-(ref, pair, band, epoch) group median removed,
groups of ≥ 3 satellites, identical group structure in both legs) puts the
reference term at 0.014 of the 0.037-cycle single-slip residual (38 %) and at
more than half of the dose legs' drift.  REF-FREE, at +220..+239: `doseall`
untouched arcs 0.95 cycle (as-traced 2.23), injected arcs 0.18 (1.30);
`doseall_not4` untouched 2.07 (4.96), injected 0.94 (5.10); `dose24` untouched
0.18 (0.52).  The ordering survives the correction: untouched arcs drift by a
cycle over two hours in the 60-station dose, twice that without the erases, and
the injected arcs settle far better WITH the erases (0.18 vs 0.94) — the tier
re-draws them closer to the network's current frame, wrong as that frame is.

**(2) It is a cascade with an incubation that shrinks with density.**  On
`dose24` the network is silent for 80 epochs (ZWD diff 0.1–1 mm, levels 0.000,
wire diff 0.6 mm) and then the wire disturbance DOUBLES roughly every 20 epochs
(0.9 → 3 → 9 → 19 → 40 → 78 mm → 0.94 m at +160 → 2.1 m), the 36 quiet
bystander stations walk with it (median 22 mm, rms 80 mm at +240: coupling
through the wire, proven on stations with no slips at all), and the outlier
tier starts erasing only once the wire is already off by centimetres.  At 60
stations the doubling is immediate; at one non-datum station the gain is
below one and nothing grows.

**(3) Which station slips matters more than how many.**  S21000SIM (anchors no
phase-bias family; not the clock datum) stays flat at 27 slips.  S30000SIM, the
`clock_datum_station` (and anchor of 9 families), cascades ALONE with 13 slips,
onset +120..+159, wire to 1.76 m.  A code-quality re-level at the datum station
cannot be absorbed by its own free receiver clock (that clock IS the network's
zero), so it goes into every satellite clock instead; the same for a family's
anchor arc, whose re-level re-references the whole satellite.  Enrichment on
`doseall`: the 35 slips that landed on the current anchor arc of their
(sat, signal) are followed by 8.1 clock-anchor decouples in 3 epochs (base 3.5)
and 86 T4 erases (base 41).  Pending to separate "clock datum" from "anchor
load": `dose_s29b` / `dose_s31b` (S29 / S31 anchor 18 families each, neither is
the clock datum, 14 / 15 slips) and `doseall_noS30` (642 slips, everything but
the datum station).

**(4) Working model.**  A re-level from one epoch of code inherits the station
frame's displacement at that moment (invariant #1's re-coupling edge).  Young
arcs at the datum / anchor stations move the satellite products directly; young
arcs elsewhere move them by their weight.  Every station's frame absorbs the
moved products into its clock / ZWD / iono because its old arcs are stiff.  The
next re-levels inherit MORE displacement.  Above a critical (rate × coupling)
the loop's gain exceeds one and the network walks; the storm beds live far above
it from their first epoch, which is why every per-arc response knob tested on
them moved the plateau by tens of percent at most.  The test of the model is
`doseall_bscope` (running): the same 655 events with both band-scope knobs ON,
so every single-band slip frees ONE band and the re-level stays phase-precise
(no code-quality young arcs) — if the converged network HOLDS, the fuel is the
whole-satellite refloat and the storm-bed falsification of band scope (§Z.7)
was an artifact of starting cold on a slipping bed.

**(5) Harness defects fixed on the way.**  `event_leg.py` / `chain_make_leg.py`
anchored `start_offset_hours` on the GPS DAY START while the producer's
fast-forward gate anchors on the FIRST SYNCED OBSERVATION: the campaign
fixture (12:00 start) was refused 12 h late, and every quiet-fixture leg before
the fix resumed ONE EPOCH LATE (first emitted tow = snapshot + 60 s).  Both
scripts now read `TIME OF FIRST OBS` (`fixture_first_obs_s`); a post-fix null
resumes at snapshot + 30 s (verified, `evleg_null_fixchk`).  Post-fix legs are
therefore NOT byte-comparable with pre-fix legs; every §12.3–12.4 leg is
pre-fix and internally consistent (`doseall_bscope` and `null240_bscope` are
the first post-fix pair, compared only with each other).  Also: the generator
must pick satellites the network USES at a station (one injected slip landed on
a satellite outside S21000SIM's solution and drew no response at all), and the
slipped seed's last snapshot (ep0665) leaves only 55 epochs of the 6 h
fixture — legs on `evseed_slipped6h` start from ep0604.

### 12.5 The band-scope twin FAILS, and the datum-station timeline names the engine (2026-09-10, evening local)

**`doseall_bscope`** (same 655 events; `refloat_band_scope: implicated` +
`raim_erase_band_scope: min_keep_one`, graded against its own matched null,
both post-fix legs): the scope acted (run-end clause: T4 4 019 cells → 6 855
bands erased / 4 019 kept; detector refloats band-scoped, 72 + 56 + 56
`band_scope` rows) and the network cascades exactly as before or worse — T4
onset +10..+39 (353), ZWD rms 0.90 → 2.24 → 1.78 m, wire 4.9 m, consumer
**+1.98 m** (doseall +1.36), clock-anchor decouples 295, refsat resets 79.
§12.4(4)'s prediction is FALSIFIED: phase-precise single-band re-levels do
not prevent the cascade, so code-quality young arcs are not the fuel.

**`dose_s30` epoch by epoch** (the clock-datum station alone, 13 slips; S30
has no DD chords at all, so the level channels are blind there and the
station's ZWD is the instrument):

| epoch | event at S30000SIM | response | clock-anchor decouples | T4 erases (station:count) | S30 ZWD diff |
|---|---|---|---|---|---|
| +23 … +105 | six single-band slips | detector refloats (2 of 3 bands; one on the IF-TD reference sat) | 0–1 | 0 | 0.3 → 6.7 mm |
| **+122** | G25 L5Q +56 cyc (elev 39°) | **no detector row — caught by T4 in the same epoch** (`attrib_arc`, all bands) | 3 (E36, G15, G25) | 3 (S30:1 S43:1 S59:1) | **−807 mm in that epoch**, −37 next, +22 by +129 |
| **+151** | G20 L5Q −57 cyc | MW refloat L1C;L5Q (L2W kept) | 2 (E36, G20) | 5 — ALL at S30 | **+911 mm** |
| +152 … +157 | — | — | 0 | 6 / 7 / 5 / 3 / 5 / 5 — all at S30 | 2.4 → 4.4 → 6.8 → 8.3 → 9.0 → 9.6 m |
| +160 … +185 | two more slips (+161, +166) | detector refloats | 0 | 1–3 per epoch, all at S30 | plateau **10.2 m**, decaying 9.4 m by +185 |
| **+186** | G12 L1C +45 cyc (LLI) | LLI + IF-TD + MW | **15 satellites in one epoch** (G04 G05 G06 G09 G10 G11 G12 G15 G18 G19 …) | 29 → 44 → 43 → 59 → 47 → 32 → 21 per epoch, at S36 S38 S43 S12 S20 S48 S25 S15 S04 S09 S07 … | 9.65 m |
| +193 … +240 | — | — | 0–2 | 1 per epoch at S30 | 6.0 → 2.6 m, still decaying |

**The engine.**  S30000SIM is `clock_datum_station`: its receiver clock is the
network's clock zero, so it has NO free receiver clock to absorb a slip
response.  At a non-datum station (S21000SIM, 27 slips, flat) the epoch's
inconsistency after a refloat is absorbed by the station's per-epoch clock at
no cost; at the datum station it goes into the ZWD and the satellite clocks.
The displaced ZWD makes the station's OTHER arcs inconsistent, the outlier
tier erases them (5–7 per epoch, all at S30), each erase re-draws an arc from
code against the displaced ZWD, and the frame runs away to 10 m in six epochs
— a self-consistent, catastrophically wrong station frame that then decays
over an hour.  The next slip response at that station (+186) re-realizes the
clock datum from a 10-m-wrong frame and broadcasts a step into 15 satellite
clocks at once; every station sees its satellites step and the outlier tier
fires network-wide — the same signature (`cta_decouple` bursts of up to 20
satellites in one epoch) the `doseall` legs showed from +11.  The +122 miss
adds a second lesson: a slip that reaches the solve (caught post-fit) at the
datum station puts 0.8 m into the emitted products of that epoch.

This is why band scope, T4 starvation, level memory and every other per-arc
response tested on the storm beds could not move the plateau: the bed's datum
station is one of the 60 slipping stations from its first epoch.  Pending
discriminators: `doseall_noS30` (642 slips, everything but the datum station —
does the network cascade without it?), `dose_s29b` / `dose_s31b` (the two
stations that anchor 18 phase-bias families each, neither the clock datum —
does anchor load cascade alone?).  Candidate cures follow from the mechanism:
a clock datum that is not one station's fixed clock (`clock_datum_sum_to_zero`,
sim-validated for wander in the readmit card, currently a golden exception), or
a datum HANDOVER away from a station for the epochs of its own slip response
(the phase-anchor sticky handover's sibling for the clock pivot).

### 12.6 The gauge is the engine — both gauges (2026-09-10, evening local)

| leg | station(s) dosed | phase-bias families the station anchors at +10 | clock datum? | slips | T4 erases (onset bin) | clock-anchor decouples | wire diff at +220 | consumer diff, last bin |
|---|---|---|---|---|---|---|---|---|
| `dose1_s21` / `dose3_s21` | S21000SIM | **0** | no | 9 / 27 | 0 (never) | 2 (= null) | — | 0.00 |
| `dose_s30` | S30000SIM | 9 | **yes** | 13 | 352 (+120..+159) | 27 | 1.76 m | +0.22 |
| `dose_s29b` | S29000SIM | **18** | no | 14 | 163 (**+10..+39**) | 18 | 0.57 m | +1.26 |
| `dose_s31b` | S31000SIM | **18** | no | 15 | 777 (+40..+79) | 77 | 5.04 m | +0.88 |
| `doseall_noS30` | 59 (all but the datum) | — | no | 642 | 2 897 (**+10..+39**) | 266 | 5.60 m | +1.80 |
| `doseall` | 60 | — | incl. | 655 | 3 099 (+10..+39) | 258 | 5.18 m | +1.36 |

A station that anchors NO family and is not the clock datum can slip 27 times
in two hours and the network does not notice.  A station that anchors 18
families cascades alone with 14–15 slips; the clock-datum station cascades
alone with 13 (§12.5); the network without its datum station cascades exactly
as fast as with it.  The engine is therefore not "many slips" but **a slip
response on a GAUGE-DEFINING arc**: the arc whose ambiguity realizes a
satellite's phase-bias datum (one per (sat, signal) family), or any arc of the
station whose clock realizes the clock datum.  On `doseall`, 35 of the 655
slips landed on the current anchor arc of their family; the anchor never moved
afterwards (0 of 35 — no handover of either kind, no `anchor_handover` row,
the anchor station unchanged in the trace for the following 3 epochs), so the
family's gauge was re-realized in place from the freed-and-re-levelled arc.
The same 35 were followed by 2.3× the base rate of clock-anchor decouples and
2× the T4 erases within 3 epochs.  A continuous handover exists in the code
(`NetworkPhaseDatum.cpp`, the `usable` branch: pin the successor at its own
carried `value_abs`) but nothing invokes it when the incumbent's arc is freed
by a slip response — the sticky election keeps the slipped incumbent.

**The slipped seed cannot read this at S21000SIM** (worker report, `evleg_sl_erase`
on `evseed_slipped6h` ep0604): the leg ran warm and clean (first emitted tow
= snapshot + 30 s, erase fired, 24 chords refloated, 0 re-flags, ZWD −2 mm,
consumer +0.57 m peak, L1C phase-bias diff +2.2 m at +38) but the DD trace has
NO rows for S21000SIM on the campaign fixture — its GPS reference is G02,
which S21000SIM never sees, and only 36 of 60 stations appear in that trace
at all.  Any slipped-bed level grade must pick a station inside the trace's
coverage (§6.13's coverage half, again).

**Pre-registered test of the cure, written BEFORE the legs were read.**
Three 60-epoch legs on the quiet seed (post-fix; null = `evleg_null_fixchk`),
one +40-cycle L1C slip at +10 on S29000SIM: `anc_slip` on G10 (S29 anchors
G10's three families), `nonanc_slip` on G27 (S29 is not G27's anchor), and
`anc_slip_ho` = the G10 slip plus an injected `anchor_handover` {G10, all,
continuous} in the SAME epoch (the harness's candidacy ban, forcing the
production election to pick a successor and take the continuous branch).
Gates: (i) `nonanc_slip` must read like Study 1's slip (residual ≤ 0.1 cycle,
frame reaction ≤ 0.02 cycle, no clock-anchor decouple burst, ≤ 2 T4 rows);
(ii) `anc_slip` is expected to show the engine — a clock-anchor decouple burst
within 3 epochs, T4 rows, a wire C/P step; (iii) the CURE passes if
`anc_slip_ho` brings the decouples, T4 rows and the wire C/P peaks to within
2× of `nonanc_slip`'s, and its frame reaction to ≤ 0.05 cycle; it FAILS if
the forced continuous handover leaves the burst or if the handover itself
steps the wire (Study 1's continuous handover cost +0.40 m for one epoch).

### 12.7 The cure, validated on the harness before it exists in production (2026-09-10, evening local)

⚠ Correction to §12.6's pre-registration: `nonanc_slip` was NOT a non-anchor
control — S29000SIM anchors G27 too (its anchor list was truncated when the leg
was chosen; S29 anchors E08, E15, G10, G23, G27, G28 at the snapshot).  Both
legs slipped an anchor arc; a true non-anchor control at S29 is `anc_ctl_s29`
(below).  The three legs, frame grader vs `evleg_null_fixchk` (post-fix):

| leg | +40-cycle L1C slip on | forced continuous handover? | other arcs of S29 (cycles) at +1 / +8 | S29 rx clock diff at +1 / +8 (m) | S29 ZWD diff at +1 / +8 (m) | emitted C diff for the sat at +8 (m) | T4 erases +0..+45 | clock-anchor decouples +0..+45 (null 1) | consumer diff at +14 |
|---|---|---|---|---|---|---|---|---|---|
| Study 1 `evleg_slip` (S21000SIM, anchors nothing) | S21/G10 | — | 0.004 / — | 0.001 / — | −0.001 / — | 0.000 | 0 | 0 | 0.000 |
| `anc_slip` | S29/G10 (anchor of 3 families) | no | **13.1 / 11.7** | **10.7 / 8.8** | −2.0 / −2.1 | **−8.9** | **157** | **13** | +1.53 |
| `nonanc_slip` (actually an anchor arc) | S29/G27 (anchor of 3 families) | no | 4.3 / 2.2 | 2.0 / 5.1 | 0.60 / −0.47 | −7.8 | 71 | 7 | +1.25 |
| **`anc_slip_ho`** | S29/G10 | **yes — G10's three families to S06000SIM in the slip epoch** | **0.07 / 0.12** | **−0.010 / −0.019** | 0.001 / 0.003 | **0.002** | **0** | 1 | +0.09 |

The anchor-arc slip without a handover re-realizes the family from the freed
arc: S29's clock swings 10 m, its ZWD 2 m, the satellite's emitted clock steps
9 m, and the outlier tier erases G10's HEALTHY arcs at eight other stations in
the same epoch (S11 S16 S19 S24 S27 S37 S40 S45), then S29's own other arcs —
the cascade of §12.4–12.6 in miniature.  With the handover forced (the
harness's `anchor_handover` directive, `continuous`; the production election
picked S06000SIM and took the continuous branch for all three bands:
`handover_continuous` 21 vs 18, `handover_cold` unchanged at 3), the same slip
costs what Study 1's slip cost: 0.05–0.12 cycle on the station's other arcs,
millimetres on its clock and ZWD, 2 mm on the wire, no erase anywhere.  The one
channel left is the injected arc's own slant iono at S29 (−0.64 → −0.79 m; the
arc re-levels as a non-anchor with L5Q pinned and absorbs the change there) —
bounded to that LOS, and to be read against `anc_ctl_s29`.

**The production card that follows (build, default OFF, byte-identical):**
`network.cycle_slip.anchor_slip_handover` — when a slip response frees an arc
that currently anchors a (sat, signal) family, stage the same one-epoch
candidacy ban the injector uses, in `continuous` mode, so the election held
later in the SAME epoch hands the family to a successor at the successor's own
carried value BEFORE the freed arc's level is re-drawn.  Counters: families
handed over on a slip, continuous vs cold outcomes (a cold outcome means no
usable successor — count it, do not hide it), and PHASE_ANCHOR event rows with
the freed arc's identity (invariant #7).  Validation gates, in order: OFF
byte-identical to main; `anc_slip` with the knob ON reproduces `anc_slip_ho`'s
numbers; `doseall` with the knob ON on the quiet seed — the 35 anchor-arc slips
are handled, the 13 clock-datum-station slips are not, so the pre-registered
expectation is a LATER and WEAKER cascade, not none; then `doseall` on the
sum-to-zero seed (`evseed_quiet6h_s2z`, prepared) with the knob ON — the
full-cure test, PASS = ZWD rms diff < 0.15 m and T4 ≤ 50 over the leg.

### 12.8 Two more twins (2026-09-10, evening local)

**`dose_s30_not4`** (the clock-datum station's 13 slips with T4 starved, vs
the post-fix `null240_pf`): S30's ZWD still runs away — −0.55 m at +120..+139,
+1.70 m at +160..+179, +2.53 m at +180..+199, +1.18 m at +220..+239 (with T4:
10.2 m) — and its clock contrast −1.0 → −3.4 → +2.8 → +3.7 m; but the
BROADCAST is much weaker: wire 0.07 → 0.26 m (with T4: 1.36 → 1.76 m),
consumer +0.33 m (with T4 +0.22).  The two halves of the datum-station engine
are therefore separable: the LEAK (a slip response at the station whose clock
is the datum goes into its ZWD/clock — no free clock to absorb it) does not
need the outlier tier; the outlier tier's erases of that station's other arcs
against the displaced ZWD amplify the leak 4–10× and are what broadcasts it
into fifteen satellite clocks at once.  The cure for the leak is a datum with a
free clock at every station (`clock_datum_sum_to_zero`; seed
`evseed_quiet6h_s2z` converging); the anchor-slip handover card does not touch
this path.

**`anc_ctl_s29`** (S29000SIM / G26 L1C +40, G26 not anchored at S29) is an
INVALID control: the network does not use G26 at S29 (no detector row, no
residual row, every channel exactly 0.000 — the slip landed on an observation
outside the solution, as G16 did at S21 in `dose1_s21`).  The generator's
satellite pick must come from the station's SOLVED set (the null leg's
`network_station_residuals.csv`), not from truth visibility; a second control
on a solved, non-anchored satellite follows.

**`anc_ctl2_s29`** (S29000SIM / G31 L1C +40; G31 is SOLVED at S29 and not anchored
there) — the valid control.  MW(1,5) and IF-TD(1,2) both fire, so all three
bands are freed; the station's other arcs move 0.11 → 0.26 cycle (as-traced),
its clock < 3 mm, its ZWD < 1 mm, the emitted clock of G31 < 1 mm, the phase
bias < 5 mm, consumer −0.01 → −0.10 m, **no T4 row, clock-anchor decouples 1
(= null)**.  With `anc_slip` (G10, anchored at S29: 157 erases, 10.7 m clock,
−8.9 m wire) and `anc_slip_ho` (0 erases) this closes the identification: at
the same station, in the same epoch, with the same slip size and the same
detector responses, only the ANCHOR arc's slip breaks the network, and only
the missing handover makes it do so.

A second observation from this control, filed for the AR track, not the
cascade: the freed arc itself re-levels **4.0 / 5.2 / 5.4 cycles off** on
L1C / L2W / L5Q (ratios 1 : 1.29 : 1.34 = the pure-iono direction of §10.3),
i.e. a 0.8–1.1 m error on that LOS's slant iono (`ion_d` −0.81 → −1.08 m),
not decaying over the leg — ten times Study 1's all-band floor at S21000SIM
(0.45 cycle).  An all-band re-level draws the level from code and inherits the
station's own receiver-DCB / iono split, which is evidently ~1 m at S29 and
~0.1 m at S21; nothing downstream sees it (no residual, no erase, no wire
step) because the free per-LOS iono absorbs it exactly.  It costs the integers,
not the products.

### 12.9 Pre-registration — the datum half of the cure (written before any leg on the new seed was run)

Seed `runs/evseed_quiet6h_s2z` = the sim golden + `network.clock_datum_sum_to_zero: true`
(each constellation's clock common-mode fixed by Σδtˢ = 0 over its satellite
clocks — no station's receiver clock is pinned; the loader notes sticky_pivot
is inert under it, there being no pivot), converged 6 h on the quiet fixture,
snapshots every 60 epochs, `gnss_processor_dose.exe`.  The knob is
fingerprinted (`clkdatum:s2z=`), which is why it needs its own seed.  Legs
resume ep0665 (same tow as the pre-fix seed's, so `--from-leg` schedules carry
over unchanged); every leg is post-fix and graded against `null240_s2z`.

| leg | schedule | gate (PASS) | what a FAIL means |
|---|---|---|---|
| `dose_s30_s2z` | `dose_s30`'s 13 slips on S30000SIM | S30 ZWD diff ≤ 2 cm in every bin, T4 ≤ 2 over the leg, clock-anchor decouples ≤ null + 2 | the datum-station leak is not the pinned clock (§12.5's reading is wrong) |
| `doseall_s2z` | `doseall`'s 655 slips, anchor-slip handover OFF | report only; expected: the anchor cascade remains (35 anchor-arc slips), with the datum-station contribution gone | — |
| `doseall_s2z_knob` | the same 655 slips, `anchor_slip_handover: true` (worker's exe) | **the full cure**: ZWD rms diff < 0.15 m and median < 2 cm at +200..+239, T4 ≤ 50 over the leg, wire diff < 0.05 m, consumer diff within ±0.1 m, clock-anchor decouples ≤ null + 5 | a third gauge or a non-gauge path exists; go back to the single-event legs |

The comparison that matters is `doseall_s2z_knob` against `doseall` (§12.4):
3 099 → ≤ 50 erases, 1.83 m → < 0.15 m ZWD rms, 5.2 m → < 0.05 m wire.

### 12.10 The datum half is FALSIFIED — there is one gauge, and it is the phase-bias anchor (2026-09-10, evening local)

`dose_s30_s2z` (the datum station's 13 slips on the sum-to-zero seed, vs
`null240_s2z`): the cascade is reproduced to the epoch — T4 0 / 0 / 0 / 0 /
56 / 279 / 15 per bin (pinned datum: 0 / 0 / 0 / 0 / 45 / 294 / 12), S30's
ZWD **10.05 m** at +160..+179 (pinned: 10.2 m), its clock contrast −21 m, its
slant iono +5.5 m, wire 1.27 → 1.63 m (pinned 1.36 → 1.76), 351 erases
(pinned 352).  §12.5's reading ("no free clock to absorb the response") is
wrong, and §12.9's PASS gate for this leg was not met by a mile.  What the
per-slip census on BOTH seeds shows instead:

| S30000SIM slip | anchored at S30? | response | T4 erases in +0..+5 (pinned / s2z) | clock-anchor decouples in +0..+2 |
|---|---|---|---|---|
| +23 E10, +44 G24, +54 G29, +94 E24, +97 E18, +105 E09, +129 E18 | no (anchors elsewhere) | detector refloats | **0 / 0** each | 0 |
| +122 G25 L5Q +56 | no | **missed by every detector, caught post-fit by T4** | 9 / 11 | 3 / 4 |
| **+151 G20 L5Q −57** | **YES** | MW refloat | **31 / 30** (the 10-m ZWD runaway starts here) | 2 / 2 |
| +161 E34, +166 E09 | no | detector refloats | 5 / 3, 4 / 6 (inside the runaway) | 0–1 |
| **+186 G12 L1C +45** | **YES** | LLI + MW + IF-TD | **254 / 224** (the 15-satellite broadcast) | 16 / 14 |
| +190 G24 L2W | no | detector refloats | 102 / 102 (the storm already running) | 1 / 3 |

S30000SIM behaves exactly like S29000SIM and S31000SIM: it is an anchor
station (nine families: G05, G12, G20), and its slips on NON-anchored
satellites cost nothing while its two slips on anchored satellites started
and broadcast the cascade.  Being the clock datum adds nothing — the same
schedule on a datum with every receiver clock free reproduces every number.
The ONE remaining secondary path is the +122 kind: a slip the detectors miss
reaches the solve for an epoch before the outlier tier removes it (0.8 m into
that epoch's ZWD and products; a small burst) — a detector-coverage item, not
a gauge item.

**Consequence.**  The full-cure test does not need the sum-to-zero seed: it is
`doseall` on the ORIGINAL seed with `anchor_slip_handover` ON (the worker's
G3, `doseall_knob` vs `null240_knob`), graded against §12.9's last-row gates
(ZWD rms diff < 0.15 m and median < 2 cm at +200..+239, T4 ≤ 50 over the leg,
wire diff < 0.05 m, consumer diff within ±0.1 m, clock-anchor decouples ≤ null
+ 5), plus `dose_s30` with the knob ON as the single-station gate (expected:
the +151 and +186 events cost what `anc_slip_ho` cost, the +122 miss remains).
`clock_datum_sum_to_zero` stays where the ledger left it (a golden exception);
`runs/evseed_quiet6h_s2z` is kept as a fixture (its null differs from the
pinned-datum null: 135 vs 148 MW rows, 5 vs 2 decouples over 240 epochs).

### 12.11 The card on the datum station's schedule — PASS (2026-09-10, 23:15 local; worker's exe 204736F4…, matched null `null240_knob`)

`dose_s30_knob` = `dose_s30`'s 13 slips with `network.cycle_slip.anchor_slip_handover: true`:

| channel | without the card (`dose_s30`, §12.5) | **with the card** |
|---|---|---|
| S30 ZWD diff, worst bin | **10.2 m** (+160..+179) | **0.055 m** (+120..+139), 0.014 m at the end |
| S30 rx-clock contrast diff, worst bin | −21 m (s2z) / −3.4 m | −0.13 m |
| S30 slant-iono diff | +5.5 m | ≤ 0.005 m |
| T4 erases over the leg | 352 | **11** (10 of them in +120..+159 = the +122 slip every detector missed; null 1) |
| clock-anchor decouples | 27–30 | 5 (null 3) |
| wire diff (emitted clocks, de-meaned rms) | 1.36 → 1.76 m | ≤ 0.002 m |
| consumer 3D diff, last bin | +0.22 m | −0.008 m |
| anchor-slip handovers (event / null) | — | 19 / 16, 18 continuous + 1 cold fallback (run-end clause) |

The null itself carries 16 handovers: the quiet bed's false-alarm MW refloats
free anchor arcs too, and the card handles those the same way.  What remains
in the event leg is exactly the secondary path §12.10 named — a slip missed
by every detector reaches the solve for one epoch — worth its own detector
item, not a gauge item.  Pre-registered gate (§12.10 last paragraph) met.

### 12.12 The card's gates (worker report, branch `worktree-anchorslip` @ ec26de1; 2026-09-10 ~23:40 local)

Built: `network.cycle_slip.anchor_slip_handover` (default OFF, not fingerprinted;
disarmed unless the prioritized anchor election runs), the staging seam
`networkStageAnchorSlipHandovers` in `NetworkPhaseDatum.cpp` (reads
`relevel.free_ambs` and `carry_slipped_ambs`, one directive per (sat, signal),
into a new `NetworkInjectState::anchor_now_slip`; `anchorFor` checks the
injector's own directives first), the call sited immediately before
`networkElectPhaseAnchors`, counters `n_anc_slip_{staged,handed,continuous,
cold,no_successor,inert,epochs}`, `PHASE_ANCHOR/anchor_slip_handover` rows
(station = retired incumbent, detail `to=<successor>;branch=…`), the armed-only
run-end clause and manifest key, 12 tests in `tests/test_anchor_slip_handover.cpp`;
suite 1816 pass / 0 fail / 17 skip.

| gate | result |
|---|---|
| G1 OFF byte-identity (`null_anchorslip_off` vs `null_fixchk`) | **PASS** — identical sha256 |
| G2 `anc_slip_knob` vs `anc_slip` / `anc_slip_ho` | **PASS** — T4 0 (was 157), decouples 1 (was 13), other arcs 0.146 / 0.058 cyc at +1 / +8 (was 13.1 / 11.7), clock −0.015 / −0.031 m (was 10.7 / 8.8), emitted-clock peak 0.0027 m (was −9.7).  Two families handed over, not three: the IF-TD response freed L1C;L2W only, so G10 L5Q's anchor arc was never freed and the card correctly staged nothing for it — the pre-registration over-specified the count. |
| G3 `doseall_knob` vs `null240_knob` (655 slips, all fired) | T4 **3 099 → 200** (onset +80 instead of +11; 0 / 0 / 104 / 42 / 21 / 32 / 1 per bin), clock-anchor decouples **258 → 44**, wire **5.96 → 0.04 m**, consumer **+1.46 → −0.13 m**, slant-iono rms 2.43 → 0.09 m, untouched-arc level drift 0.83 → −0.05 cyc; anchor-slip handovers 120 (107 continuous / 13 cold fallbacks); detector populations unchanged (MW 560, IF-TD 350, LLI 312, refsat 78).  ZWD rms **1.76 → 0.94 m** — but see below. |

**The ZWD rms is ONE station.**  At +200..+239 the median |ZWD diff| over the 60
stations is **5.3 mm**, 59 stations are within 42 mm, and S36000SIM alone
reads **+7.49 m** with **124 of the 200** erases; 7.49 / √60 = 0.97 m is the
whole rms.  S36's cascade starts at +89 (seven of its arcs erased in one
epoch) from a slip that drew NO response row at all — one of **15 slips in
655 (2.3 %) that no detector caught within two epochs** (post-fit T4 caught 0
of them on this leg).  A slip that stays in the solve is the §12.10 secondary
path, and one is enough to wreck a station, though on this leg it did not
propagate (the other 76 erases are scattered; the wire and the consumer are
flat).  The 13 cold fallbacks are NOT where the damage is (S60 3, S30 1, S31 1
— all within 21 mm).  So the anchor-slip handover removes the gauge engine;
what is left is detector coverage, a separate card.

**Knob-on nulls differ from knob-off nulls** by construction: the warm-resume
refloat burst frees anchor arcs too, and the card hands them over (16 families
at +1 on this seed), so every knob-on leg is graded against a knob-on null.

**Collateral (worker):** `scripts/vs2026_build.ps1` hard-codes the main project
path — from a worktree it reconfigured and rebuilt MAIN's Release tree (same
source as HEAD, no harm) and left the worktree unbuilt; the worker used a
repointed copy.  Worth a `-Project` switch.

**The 15 "missed" slips, resolved (from the traces, not the sky).**  Fourteen
fell on (station, satellite) pairs that were NOT in the solved set at the slip
epoch (no residual row within ±1 epoch, no `slip_raw` row) — G16 at eight
stations, E27 at three, G26, E30: the satellites are solved elsewhere and at
other times, but at that station and epoch the observation was outside the
solution, so the injection was inert, not missed.  (Generator item: pick
(station, sat, epoch) from the solved set — the null leg's residual and anchor
traces — not from truth visibility; `dose1_s21`'s G16 was the same.)  The ONE
real miss is S36000SIM / G04 L5Q, −99 cycles at 20.5°, no LLI — and **S36000SIM
was G04's anchor for all three bands at that epoch.**  Its `slip_raw` row
carries the L1C;L2W pair only, so no MW pair covered L5 for that arc at that
moment; IF-TD's pair is L1C;L2W (blind to L5); nothing freed the arc, so the
card had nothing to hand over; the pinned anchor row kept −25 m of misfit in
the solve, the station frame absorbed it, and at +89 the outlier tier erased
seven of S36's INNOCENT arcs (pinned anchor arcs are outside the leveling RAIM
pool, so the tier can never blame the anchor itself) — the 7.5 m runaway.
The datum station's +122 miss (§12.10) had the same shape: G25 L5Q at 20°, no
LLI, caught only post-fit.  Two follow-ups, both precise: (a) DETECTOR
COVERAGE — the L5 MW pair must exist for every triple-frequency arc the moment
it is solved (why was only L1C;L2W in the feed for a 20° arc?); a harness card
that injects L5Q slips at 15–25° on solved arcs and counts detections by tier;
(b) ATTRIBUTION — an undetected slip on a pinned anchor arc is structurally
invisible to the outlier tier and gets charged to the station's other arcs;
when a station-wide burst lands on a station that anchors families, test (or
hand over) the anchor arcs before erasing anything else.

### 12.13 STORM-BED VALIDATION — the plateau is gone (2026-09-10, 23:30 local; `runs/golden_sim_campaign_zwdblk_ancho`)

The user directive reserves the day-long bed for the final validation of a fix
whose event-level behaviour is understood.  One arm: the in-block control
config plus `network.cycle_slip.anchor_slip_handover: true`, cold start on the
slipped campaign fixture, 6 h × 60 stations, exe 204736F4… (the merged card),
1006 s wall, 720/720 emitted.  `scripts/simnet_storm_grade.py`:

| gate quantity | control (`golden_sim_campaign_zwdblk`) | **with the card** | gate |
|---|---|---|---|
| settled ZWD truth rms | 1.188 m | **0.046 m** | ≤ 0.5 **PASS** (the quiet golden's ZWD reads 0.0072 m; [corrected 2026-09-11: the 0.076 quoted here was the quiet bed's CONSUMER rms]) |
| ZWD hourly rms | 1.98 / 1.31 / 0.96 / 0.58 / 1.05 / 1.67 | 0.21 / 0.02 / 0.01 / 0.04 / 0.07 / 0.02 | — |
| per-station ZWD span p50 / stations > 1 m | 0.207 m / 15 | **0.031 m / 0** | — |
| consumer settled p50 / rms | 1.085 / 2.389 m | **0.278 / 0.761 m** | p50 ≤ 0.6 **PASS** |
| consumer hourly rms | 1.59 / 2.19 / 1.56 / 1.03 / 2.55 / 3.09 | 1.12 / 0.34 / 0.33 / 0.22 / 1.04 / 0.78 | — |
| T4 attribution events / erased-arc ratio | 9 871 / 2.17 | **306 / 1.34** | must fall: **PASS** (−97 %) |
| T4 ambient per-band DD error | 1.6 cyc | **0.26 cyc** | — |
| ref-free WL / EWL / L1C at own integer | 86.2 / 99.2 / 44.0 % | **98.1 / 99.8 / 81.1 %** | WL ≥ 83 **PASS** |
| CLK-R drained / max A_c | 41.5 / 25.6 m | 4.2 / 0.3 m | — |
| detector events | 4 545 | 4 545 | unchanged (same slips caught) |
| anchor-slip handovers | — | 261 (211 continuous / 50 cold fallback) | — |

Every pre-registered gate passes, most by an order of magnitude.  The ZWD sink
that opened this campaign (§0 of `project_zwd_sink_census`: 1.2 m in-block,
2.1 m before) is closed to 4.6 cm — below the quiet bed's own 7.6 cm — and the
wide-lane integers, which the storm bed had never held above 86 %, sit at 98 %.
What remains: the consumer's hour-5/6 bump (1.04 / 0.78 m rms against 0.2–0.3
in hours 2–4) and its 0.28 m median against the quiet bed's 0.05 — the §12.12
detector-coverage item (an undetected L5 slip on an anchor arc wrecks a
station) and the 50 cold fallbacks are the two named candidates; both are
harness questions, not storm questions.

**Ledger:** `network.cycle_slip.anchor_slip_handover` → **RECOMMEND_ON**, regime
`on(phase_leveling) and on(phase_datum_prioritized)`, pinned in the sim golden
(`golden_manifest.json`; the field golden is out of regime on its
carry_invariant vehicle — its leveling posture inherits the pin when it lands).
`make_golden_config.py --verify` PASS (51 armed on the sim golden).


## 13. Study 5 — detector coverage: the unflagged GPS L5 slip (2026-09-11)

### 13.1 The question, and what was established before any new leg

§12.12 left one residual path: a slip that no detector catches, landing on a
phase-bias anchor arc, still wrecks its station (S36 to 7.5 m on
`doseall_knob`), because nothing frees the arc, the handover card has nothing
to hand over, and the outlier tier cannot indict a pinned row.  Question: WHY
does that slip escape, how often, and what production change closes it?

**Census first** (`slip_census.py`, scratchpad; every injected slip of
`runs/evleg_doseall` and `runs/evleg_doseall_knob`, 655 each, identical
outcomes on both legs).  Of 1 282 non-inert slips, every band — with or
without a receiver flag — is caught within +0..+2 epochs by MW / LLI / IF-TD,
EXCEPT unflagged L5:

| injected band, flag | caught late or never | of |
|---|---|---|
| L1 / L2 / L7 (Galileo E5b), flag or none | 0 | 852 |
| L5, flag set | 0 | 212 |
| **L5, no flag** | **10 (= the same 5 events on both legs)** | 218 |

The five: S36/G04 −99 cyc at 20.4° (no detector, caught by NOTHING — the
§12.12 wreck), S25/G05 −40 at 61.6°, S15/G20 +110 at 30.9°, S30/G25 +56 at
20.0° (the §12.10 datum-station miss), S33/G19 −40 at 18.1° — the last four
caught post-fit by T4 in the same epoch.  All GPS; Galileo E5a slips have 0
misses (the E5a/E5b pair, λ 9.8 m, catches them).  **Elevation is NOT the
discriminator** (18–62°), so this is not the low-elevation noise story the
§12.12 wording implied.

What catches an unflagged L5 slip when it IS caught: MW L2W;L5Q (80) and MW
L1C;L5Q (42) on GPS, MW L7Q;L5Q (83) on Galileo, IF-TD L1C;L5Q (3 — Galileo
E1/E5a).  Two structural facts from the code:

- **IF-TD forms ONE pair per satellite: the first two distinct bands**
  (`IfPhaseSlipDetector.cpp` ~250) — L1C;L2W on GPS, L1C;L5Q on Galileo.  It
  is blind to a GPS L5-only slip by construction.  (§12.12's "the L5 MW pair
  did not exist" was inferred from `network_slip_raw.csv`, which prints the
  first two bands by construction — the MW detector DOES form (1,5) and (2,5);
  the census shows them firing at S36 on other satellites.)
- **The MW detector reports a slip in two grades and the network consumes one.**
  On the network path (uncombined branch; no satellite MW bias on the sim, so
  no integer locking) `processMwSample` returns (integer, integer_known,
  fraction).  PASS A (`ApplicationNetworkAssembly.cpp:1363`) admits a refloat
  candidate only if `slip_wl_known && slip_wl_cycles != 0 && slip_frac <=
  turbo_edit_frac_max` (0.15).  An UNKNOWN-integer detection (the init-frac
  "suspicious" path, a confirmed TD step, an integer not resolvable from one
  sample) or a known integer with fraction > 0.15 is **dropped with no
  counter and no event row** — invariants #4 and #7 both violated.

### 13.2 Hypotheses — frozen before the reproduction legs are read

Two legs from the quiet seed ep0665, main's `gnss_processor_ancho2.exe`
(HEAD 9935aec), the MW detector's logger armed (it is OFF by default —
`logging.enable_mw_slip_detector`):

- `runs/evleg_l5miss_s36` — the ONE slip (S36000SIM / G04 / L5Q / −99, no
  flag) at +89 (tow 281850 = the dose leg's epoch), 100 epochs, log at debug.
- `runs/evleg_s36only_mwlog` — the dose leg's S36 subset (its 11 slips,
  identical events, 240 ep), log at info: station-local history reproduced.

| hypothesis | prediction in the logs | production change if true |
|---|---|---|
| **H1 dropped detection** — MW fired on (2,5) and/or (1,5) at +89 but returned integer-unknown or fraction > 0.15, and PASS A discarded it | a "MW slip detected for G04 … locked=no, delta_wl=n/a" / "MW init suspicious … unknown integer" line at the slip epoch; NO T0 event row | consume unknown-integer / loose-fraction MW detections as a REFLOAT of the pair's two bands (the network's reset-over-repair), knob `network.cycle_slip.mw_unknown_integer_refloat`, default OFF, counter + event row for every dropped detection regardless of the knob |
| **H2 silent detector** — no G04 detection line at all: the pair's samples were absent (NaN MW, no C5 code) or the arc was inside a warm-up that initialised ON the slipped value | `[DIAG MW skip]` lines for G04, or an arc restart at / just before +89 | depends on which: the warm-up hole needs an init-frac test on the (2,5) lane (λ 5.86 m makes a fresh sample's fraction informative), the pair hole needs the pair |
| H3 below threshold | a G04 line with dev < thresh | impossible at 74 m / 580 m for 99 cycles; listed so the reader sees it was considered |
| H4 state-dependent | the miss does NOT reproduce in the single-event leg but DOES in the S36 subset (station history matters), or in neither (network-level reset) | then the dose leg's own log is the instrument |

### 13.3 Pre-registered gates for the production change (whatever H is true)

| gate | leg | pass |
|---|---|---|
| G1 | null on the card exe, knob OFF | byte-identical to `runs/evleg_null_main_ancho` |
| G2 | `l5miss_s36` + knob ON, vs the matched knob-OFF leg | detection row at +0/+1 naming an L5Q-bearing pair; PHASE_ANCHOR handover row for G04 (the freed anchor); T4 erases at S36 in +0..+10 = 0 (the dose leg had 7 at +0, 6 at +1); S36 ZWD / clock / other-arc levels within 2× the null floor (`event_frame_grade.py`); consumer within the null floor |
| G3 | `doseall_knob` events + knob ON (matched knob-ON null) | S36 no longer wrecked: max per-station ZWD rms ≤ 0.2 m (was 7.5); T4 ≤ 200; wire ≤ 0.05 m; the four T4-only slips become detector catches |
| G4 | quiet 240-ep null + knob ON vs knob OFF | false-alarm price: new refloats ≤ 20 network-wide (≤ 1 % of the campaign slip rate); ZWD / consumer diff within the null-vs-null floor |
| G5 | storm bed `_golden_sim_campaign_zwdblk_ancho` + knob (FINAL) | ZWD ≤ 0.05 m, consumer p50 ≤ 0.25 (was 0.278), T4 ≤ 306, WL ≥ 98 %; hour-5/6 consumer rms must not rise |

The IF-TD second pair (a (1,5) IF pair per GPS satellite) is a SEPARATE item:
it would add a detector to the L5 lane, but the census says the MW (2,5)
lane already covers 96 % and the question is what happens to the last 4 %.

### 13.4 RESULT — H1, with the exact gate named (2026-09-11; legs `l5miss_s36`, `s36only_mwlog`, `l5miss4_mwlog`, main exe `gnss_processor_ancho2.exe`)

**The miss reproduces in the single-event leg** (S36/G04/L5Q −99 at +89: the
outlier tier erases 7 innocent arcs at S36 at +0 and 6 more at +1, G04 stays
anchored at S36 on all three bands, no detector row — exactly the dose leg).
The detector's own log (rotating sink; older lines are in
`mw_slip_detector.N.log`) shows what happened at the slip epoch:

| lane | λ | arc age | \|dev\| | threshold | integer | rounding residual | PASS A |
|---|---|---|---|---|---|---|---|
| L2W/L5Q (bias-free) | 5.861 m | 89 | 581.3 m = 99.2 WL | 7.42 m | **99, known** | **0.183 cyc** (1.07 m) | **dropped: 0.183 > `turbo_edit_frac_max` 0.15** |
| L1C/L5Q (bias-free) | 0.751 m | 87 | 75.4 m = 100.3 WL | 8.46 m | unknown (0.3 cyc off) | — | dropped: integer unknown |
| L1C/L2W (BIA, between-satellite) | 0.862 m | — | no jump (L5-only slip) | — | — | — | — |

Both L5 lanes flagged the slip at 8.9× and 78× their thresholds.  The network
consumed neither: the refloat path re-uses the T0 REPAIR gate — "trust the
integer only if its rounding residual is ≤ 0.15 cycles" — and on the 5.86 m
extra-wide lane 0.15 cycles is 0.88 m, below the lane's own 1σ code noise at
20° (σ = 0.93 m in the log).  A repair-quality gate on a detection whose only
consumer is a refloat, which never uses the integer.

**The other four misses have the same shape** (`l5miss4_mwlog`, the dose leg's
S25/S15/S30/S33 subset, identical events):

| slip | L2W/L5Q lane | L1C/L5Q lane |
|---|---|---|
| S25/G05 −40 (61.6°) | 39.8 WL, integer 40 known, residual ≈ 0.2 → dropped | 39.3 WL, unknown |
| S15/G20 +110 (30.9°) | 109.8 WL, −110 known, ≈ 0.2 → dropped | 109.8 WL, −110 known, ≈ 0.2 → dropped |
| S30/G25 +56 (20.0°) | 56.2 WL, −56 known, ≈ 0.2 → dropped | 55.5 WL, unknown |
| S33/G19 −40 (18.1°) | 40.2 WL, 40 known, ≈ 0.2 → dropped | 39.5 WL, unknown |

So the 4 % is the tail of the rounding-residual distribution on the L5 lanes
(σ_frac ≈ 0.15–0.2 cycles at low elevation on (2,5); larger on (1,5)), not a
missing pair, not elevation, not the detector.  The detector caught 5/5.

**What a refloat gate may cost — the false-alarm census** (`mw_detect_census.py`
over `s36only_mwlog`: 240 ep, 60 stations, S36's 15 real slips, 719 "MW slip
detected" lines):

| population | count | reaches the network today |
|---|---|---|
| warm-up (arc age ≤ 2), all lanes | 680 | 70 admitted (the +1 resume burst: ~80 refloat rows at 279210); the rest dropped |
| arc age 3–9 | 2 | 0 |
| mature (age ≥ 10), bias-free lanes, ratio ≥ 3 | 25 | **every one is one of S36's real slips** — zero false alarms in 59 quiet stations × 2 h |
| mature, BIA lane (L1C/L2W between-satellite), ratio 2.4–7.5 | 12 | 8 of them are ONE event: eight satellites at one station jump by the same 37–40 WL at the same epoch — a between-satellite pivot artifact (the S-transform on a pivot change), dropped today |

Steady-state refloats the network actually performs at the 59 quiet stations
after the burst: **3 in two hours**.  A gate of the form *mature arc AND
evidence ≥ 3× threshold AND bias-free lane* admits all five misses and nothing
else on this bed; the bias-free restriction is what keeps the pivot artifact
out (a detector-side defect worth its own line: that event also ERASES those
eight arcs' MW histories, so the station's L1/L2 lane is blind for the next
few epochs).

**Production card (building, default OFF): `network.cycle_slip.mw_evidence_refloat`**
— the detector exports, with every detection, its evidence ratio (|dev|/thresh),
the arc's age, and the lane's bias class; PASS A admits a refloat candidate on
the legacy rule OR (knob AND bias-free lane AND age ≥ `mw_evidence_min_arc_epochs`
(10) AND ratio ≥ `mw_evidence_ratio_min` (3)); the candidate carries
`integer_known` so the bridge path never consumes a zero integer; dropped
detections are COUNTED always (loose / unknown / would-admit) — invariant #4.
Gates §13.3 G1–G5.  Not built: the IF-TD second pair, and the warm-up
false-alarm limit cycle (a flagged arc is erased and re-initialised on the
next sample; a real slip landing on that sample is absorbed as the new level —
none of the five misses was this, but it is a real hole at low elevation).

### 13.5 The card's gates (worker build 0550b58 on its own worktree, merged to main as 2490087; manager re-grade; 2026-09-11 morning)

`network.cycle_slip.mw_evidence_refloat` (+ `mw_evidence_ratio_min` 3.0,
`mw_evidence_min_arc_epochs` 10), default OFF.  `MwSlipResult` now exports
`evidence_ratio`, `arc_epochs`, `bia_corrected`; PASS A admits on the legacy
rule OR (knob ∧ bias-free lane ∧ age ≥ 10 ∧ ratio ≥ 3); the candidate carries
`integer_known` (the bridge intercepts are skipped for an unknown integer);
rows append `;src=evidence;ratio=…;n=…;int=known|unknown`; run-end clause
always: `MW detect-only admission: legacy N, evidence N [knob on|off;
would-admit N]; dropped loose N / unknown N` (10 new tests, suite 1826 / 0 / 17
re-run by the manager from the worker's Debug build).

| gate | leg(s) | result |
|---|---|---|
| G1 byte-identity OFF | `null_mwevid_off` vs `null_main_ancho` (60 ep) | **PASS** — products / solution / events / ZWD SHA-256 all MATCH (re-hashed by the manager: F3F37BC3…, 30041F28…, 690927F6…, DDC169A2…) |
| G2 single event | 2×2 on S36/G04/L5Q −99 at +89: `l5miss_s36` (both off), `_on` (knob), `_anc` (handover), `_anc_evid` (both) | knob alone: the detection row appears (`L1C;L5Q;src=evidence;ratio=8.9;n=87;int=unknown`; the drain logs "refloated 1 wide-lane slip [S36000SIM/G04]") and the solve does not move — T4 22, clock −17.8 m, ZWD +14.3 m, iono +21.3 m IDENTICAL to both-off; handover alone: identical wreck (nothing freed the arc); **both: T4 0, clock +0.021 m, ZWD −0.013 m, consumer +0.0008 m** (3 families handed over continuously at 281850).  The knob supplies the detection; the handover is the cure on an anchor arc. |
| G3 655-slip dose, knob ON (+ handover) | `doseall_evid` vs `null240_evid`; control `doseall_knob` vs `null240_knob` | **PASS** — T4 200 → **0**; the five formerly-missed slips 0/5 → **5/5** detected at +0 (all `src=evidence`); max per-station ZWD diff 10.6 m (S36) → **0.019 m** (the worker's "0.296" was S36's CLOCK column; ZWD ≤ 0.02 m at every station); wire de-meaned rms 0.041 → **0.009 m**; consumer peak 0.46 → 0.34 m, last bin −0.13 → −0.09 m; MW rows on injected arcs 560 → 705; handovers 120 → 138 (120 continuous / 18 cold); S36 residual after its all-band free: clock +0.30 m, iono +0.53 m at +240 (the §9 all-band re-level of a 20° arc, now a non-anchor) |
| G4 false-alarm price | `null240_evid` vs `null240_knob` | **PASS** — 0 evidence admissions in 240 ep × 60 sta; all four artifacts SHA-256 identical armed vs unarmed; run-end `legacy 286, evidence 0; dropped loose 196 / unknown 1338` (the population the rule declines) |
| G5 storm bed | `runs/golden_sim_campaign_zwdblk_mwevid` (= the §12.13 arm + the knob) | see §13.6 |

**Why the refloat alone is inert on an anchor arc — and a sharper statement of
the §12 engine.**  Two control legs with the handover OFF, injector
`arc_erase mode=free_epoch` on the same pinned anchor arc (S36/G04, no slip):
all three bands freed (`ambs=3;latch=free_ambs`) and two bands freed
(`L1C;L2W`) both read **exactly 0.0000 on every channel** against the null
(clock, ZWD, iono, emitted C and P, consumer) — a freed anchor arc is NOT
re-levelled while it holds the pin; the pin keeps its level.  So a slip on an
anchor arc is a **stale pinned row carrying the whole slip as a misfit** — the
solve distributes −25 m of misfit into the station frame (clock −17.8 m, ZWD
+14 m, iono +21 m at S36) and into the satellite's products, and the outlier
tier, which cannot see a pinned row, erases the station's innocent arcs.
§12.7's phrase "the family is re-realized from the freed arc" is imprecise:
nothing is re-drawn; the pin simply stays on an arc whose observations moved.
The cure is unchanged (move the pin off the arc in the slip epoch), but the
division of labour is now exact: on a NON-anchor arc the detector's refloat
alone repairs the slip (the four T4-only cases in G3); on an ANCHOR arc the
refloat's only job is to put the ambiguity into `free_ambs` so the handover
fires — which is what the knob does for the 4 % the repair gate had hidden.

### 13.6 STORM-BED VALIDATION — PASS on every bar (2026-09-11; `runs/golden_sim_campaign_zwdblk_mwevid` = the §12.13 arm + `mw_evidence_refloat`, worker exe 8A9BEBA1…, 1130 s, 720/720; graded with `scripts/simnet_storm_grade.py`)

| gate quantity | §12.13 arm (anchor handover only) | **+ mw_evidence_refloat** | gate |
|---|---|---|---|
| settled ZWD truth rms | 0.046 m | **0.0071 m** | ≤ 0.05 **PASS** (= the quiet golden's own ZWD floor, 0.0072 m) |
| ZWD hourly rms | 0.21 / 0.02 / 0.01 / 0.04 / 0.07 / 0.02 | 0.17 / 0.01 / 0.007 / 0.006 / 0.007 / 0.008 | — |
| per-station ZWD span p50 / stations > 1 m | 0.031 m / 0 | 0.021 m / 0 | — |
| consumer settled p50 / rms | 0.278 / 0.761 m | **0.174 / 0.365 m** | p50 ≤ 0.25 **PASS** |
| consumer hourly rms | 1.12 / 0.34 / 0.33 / 0.22 / 1.04 / 0.78 | 0.62 / 0.29 / 0.23 / 0.18 / 0.47 / 0.39 | hours 5–6 must not rise: **halved** |
| T4 attribution events / erased-arc ratio | 306 / 1.34 | **78 / 1.51** | ≤ 306 **PASS** (−75 %) |
| T4 ambient per-band DD error | 0.26 cyc | 0.229 cyc | — |
| ref-free WL / EWL / L1C at own integer | 98.1 / 99.8 / 81.1 % | **98.9 / 100.0 / 87.1 %** | WL ≥ 98 **PASS** |
| CLK-R drained / max A_c | 4.2 / 0.3 m | 3.62 / 0.28 m | — |
| detector events | 4 545 | 5 151 | +606 = the admitted detections' rows |
| MW admissions (run-end) | legacy only | legacy 4 570, **evidence 2 278** (rows 1 020: 348 integer-known, 672 unknown; ~170 per hour, flat) | dropped loose 1 010 / unknown 5 660 remain |
| anchor-slip handovers | 261 (211 continuous / 50 cold) | 305 (237 / 68) | cold share 19 → 22 % |

The slipped bed now sits at 7 mm of tropospheric error — EQUAL to the quiet
bed's own floor.  [Corrected 2026-09-11 after the quiet re-run: the "76 mm"
quoted in the first draft of this paragraph was the quiet bed's CONSUMER 3D
rms (0.0762 m, `runs/golden_sim_tri_w6_full`), not its ZWD; the quiet bed's
ZWD truth rms is 0.0072 m.  Quiet 24 h with both slip cards on main's build
(`runs/golden_sim_quiet24_bothcards`, the regenerated golden verbatim, 2880
epochs): ZWD 0.0073 m, hourly 0.007 flat; consumer settled h12–24 rms
0.0895 / p50 0.0593 / UP rms 0.0768 m against the in-block quiet arm's
0.0875 / 0.0525 / 0.0759 (`golden_sim_zwdblk_quiet`, an older golden); T4 66,
6 handovers (all continuous), 0 evidence admissions in 24 h.  The matched
no-card twin of today's golden is owed for the exact neutrality delta.]  On the storm bed roughly one MW admission in three now comes
through the evidence path: the population the repair gate had been hiding is
not a tail at storm density.

**Ledger:** `network.cycle_slip.mw_evidence_refloat` → **RECOMMEND_ON**, regime
`on("network.phase_leveling") and on("network.cycle_slip.mw_detect")`, pinned
in the sim golden; the field golden is out of regime (carry_invariant vehicle,
no phase leveling).  Open after this card: (a) two MW pairs firing on the same
satellite in the same epoch free all three bands although only their COMMON
band slipped — a "lane-intersection" scope (free only the shared band) is the
honest single-band response and is the next detector-side card; (b) the 68
cold fallbacks; (c) the IF-TD second pair; (d) the warm-up erase-and-re-init
limit cycle (priced at ~8e-4 per lane-epoch, §13.4); (e) the BSSD L1/L2
pivot-change artifact (8 arcs' MW histories erased at one station in one
epoch).

## 14. Study 6 — the COMMON-BAND scope: free only the band the evidence names (2026-09-11)

### 14.1 The question, the population, what is already known

A single-band slip on a triple-frequency arc moves every MW pair that contains
the band: an L5 slip fires (1,5) and (2,5) and leaves (1,2) silent.  The drain
refloats each firing pair's TWO bands, so the union is all three — an all-band
free, the §9 floor (0.45–0.65 cyc for 45 epochs at low elevation, slant iono
−0.08 m), for a slip that touched one band.  On the storm bed the run-end
clause reads 2 278 evidence admissions for 1 020 rows and 4 570 legacy
admissions: most slips fire two lanes, so most responses are all-band.  The
band-scope card of Study 3 (`refloat_band_scope: implicated`) tried to pick
the band from the MW/GF DECOMPOSITION and was falsified on the storm bed —
its fallback ran as "keep L1C whatever happened to it" 1 091 times and
collapsed the integers.  This card uses a different, set-theoretic piece of
evidence: **a silent, mature pair exonerates both of its bands**; a band is
implicated when no silent pair contains it.  Narrowing needs POSITIVE
exoneration; without it the response is today's.

### 14.2 The rule (pre-registered)

Per (station, satellite, epoch), tracked phase bands B (2 or 3):

- MW pair p is **silent** iff its arc was evaluated this epoch, is mature
  (arc count ≥ `common_band_min_arc_epochs`, 10) and did not flag — the
  detector's `getEpochCleanPairs()` set, with the maturity guard added if it
  is not already implied.  A flagged pair (admitted or dropped), an immature
  or absent pair is NOT silent.
- If the IF-TD detector evaluated the pair (GPS: L1C;L2W; Galileo: L1C;L5Q)
  and FLAGGED it this epoch, that pair cannot exonerate (closes the
  equal-count trap: L1 and L2 slipping by the same integer leave MW(1,2)
  silent but move the iono-free combination by 0.107 m per cycle).
- A band with a loss-of-lock flag set is implicated regardless.
- **exonerated(b) ⇔ no LLI on b ∧ ∃ silent pair p ∋ b (with IF-TD on p not
  flagged).  implicated = B \ exonerated.**
- Fallbacks to TODAY'S response (free every band any drain would have freed):
  no MW pair fired; implicated = ∅ although a pair fired (inconsistent
  evidence); implicated = B (nothing exonerated).
- The verdict is formed ONCE per epoch before the drains and governs every
  drain for that (station, satellite): the MW refloat, the LLI free and the
  IF-TD pair refloat each free (their bands ∩ implicated); the per-epoch
  union is therefore exactly `implicated` (Study 3's per-epoch invariant).

Worked cases: L5 slip → {L5Q}; L1 slip → {L1C} (IF-TD fires on (1,2) but
(2,5) silent exonerates L2 and L5); L2 slip → {L2W}; two bands by different
counts → all pairs fire → all (today); equal-count L1+L2 → (1,2) silent but
IF-TD(1,2) fires → nothing exonerated → all (today); a pair in warm-up cannot
exonerate → all (today); (1,2) fires alone with (1,5) and (2,5) silent →
implicated = ∅ → inconsistent → today.

### 14.3 Pre-registered legs and gates (knob `network.cycle_slip.common_band_scope`, default OFF)

Quiet seed ep0665, event at +10, 70 epochs, station **S21000SIM** (anchors no
family, so the refloat alone acts and the handover is not in the picture),
satellites from its solved GPS set with DD chords: **G08 (46°)** and **G10
(15°)**.  Every leg arms `anchor_slip_handover`, `mw_evidence_refloat` and the
scope knob explicitly (make-leg inherits the seed's config, which predates
all three).  Each leg vs its own knob-matched null.

| leg | event | expectation with the scope (vs today) |
|---|---|---|
| E1 `cb_l5_g08`, `cb_l5_g10` | L5Q +40, no flag | today all 3 bands freed (MW (1,5)+(2,5)); scope → `implicated=L5Q`; per-band level residual at +14..+45 falls from the §9 floor to ≲ 0.05 cyc, slant-iono step → ≲ 0.01 m |
| E2 `cb_l1_g08` | L1C +40, no flag | today all 3 (MW (1,2)+(1,5), IF-TD (1,2)); scope → {L1C} |
| E3 `cb_l2_g08` | L2W +40, no flag | today all 3; scope → {L2W} |
| E4 control `cb_all_g08` | L1C +40, L2W +31, L5Q +30 in one epoch, no flag | all pairs fire → all implicated → **byte-identical to the knob-OFF leg** |
| E5 control `cb_eq12_g08` | L1C +20 and L2W +20, no flag | MW(1,2) silent, IF-TD(1,2) flags → nothing exonerated → all bands; the verdict row must name `iftd(1,2)=flagged` |
| E6 `cb_lli5_g08` | L5Q +40 WITH the flag | LLI path; scope → {L5Q} (L1/L2 exonerated by silent (1,2)) |

Gates: **G1** knob-OFF null byte-identical to `runs/evleg_null_mwevid_main`;
**G2** E1–E3, E6 narrowed as expected AND per-band level residual at +45
better than the knob-OFF twin by ≥ 3× (event_frame_grade L1_d/L2_d/L5_d),
slant iono within 2× the null floor; **G3** E4 byte-identical, E5 not
narrowed; **G4** quiet 240-ep null with the knob: 0 narrowed verdicts unless
a real detection, artifacts byte-identical to `null240_evid`; **G5** the
655-slip dose (`doseall_evid` events + scope, matched null): levels `all_d`
/ `oth_d` and the per-slip residual table improve, consumer and wire not
worse, T4 ≤ 0 (stays 0), handovers FALL (only the slipped band's family moves);
**G6** storm bed (`_golden_sim_campaign_zwdblk_mwevid` + scope, manager):
consumer p50 ≤ 0.15 (from 0.174), rms ≤ 0.35, ZWD ≤ 0.01, T4 ≤ 78, L1C
ref-free ≥ 90 % (from 87.1), WL ≥ 98.9 %, cold fallbacks < 68.  A T4 count
ABOVE 78 on the storm bed is the falsifier: a wrongly exonerated band shows
up post-fit at that arc.

## 15. Next card, pre-registration stub — the COLD anchor fallbacks (census 2026-09-11 on `golden_sim_campaign_zwdblk_mwevid`)

The anchor-slip handover falls back to a COLD re-realization when the elected
successor has no usable carried value (68 of 305 on the storm arm with both
slip cards; 50 of 261 on the anchor-only arm).  Two censuses, both from the
observation trace (`network_slip_raw.csv`, every station × satellite × epoch,
so an arc's age is the time since the satellite rose at that station; the
residual trace cannot be used — pinned arcs never appear in it):

| population | successor arc age, epochs (p10 / p50 / p90) |
|---|---|
| 68 cold handovers | 12 / **39** / 158 |
| 237 continuous handovers | 65 / 236 / 474 |

- 21 of the 68 fall in the arm's first 30 minutes, where EVERY arc is young
  (cold start): unavoidable, and a warm resume never sees them.
- Of the remaining 47, **43 had three or more mature alternatives** (another
  station holding the same satellite for ≥ 60 epochs) and **in 0 of 43 cases
  was the chosen successor the highest-elevation mature candidate** — the
  successors sat at 16–34° with 10–65-epoch arcs while mature candidates
  stood at 78–80°.  Cold handovers come in whole-satellite groups (13 pairs,
  10 triples: every band of the satellite handed over in one epoch — the
  all-band responses the common-band scope of §14 removes for single-band
  slips).

**Hypothesis for the card:** the successor election does not test carry
usability (maturity) as a CANDIDACY condition; it applies its priority (which
appears to favour a station already holding the satellite's sibling bands, or
list order) and only afterwards discovers the carry is unusable and falls back
cold.  **Pre-registered fix:** candidacy requires a usable carried value
(mature arc, finite value_abs, not itself freed this epoch); the normal
priority then ranks the usable candidates; a family with no usable candidate
keeps today's cold path.  **Gates:** OFF byte-identical; on the storm arm the
cold count falls from 68 to ≤ 25 (the cold-start 21 remain), continuous
handovers land on arcs of age ≥ 60, consumer / ZWD / T4 not worse than §13.6;
the harness leg is the §12.7 single-event anchor slip at a station whose
satellite has a young low successor and a mature high one available.

### 14.4 Iteration 1 — the rule is right, the evidence source was wrong (worker build 5fe64c7 on its worktree; 2026-09-11 ~11:00)

Built as specified: `include/slip/NetworkCommonBandScope.hpp` +
`src/core/slip/NetworkCommonBandScope.cpp` (the verdict seam, called before
`networkSlipDrainMwRefloat`), knob + `common_band_min_arc_epochs`, counters,
`T0_band_scope` rows (`keep` for a narrowed verdict, `all` with a reason for a
fallback when a pair fired), run-end clause with `would-narrow`, 17 tests
(suite 1843 / 0 / 17), `event_leg.py --extra`.  One shared-path edit: the
IF-TD driver's pooled centre / scale / shrinkage were factored into
`ifTdPoolStats` + `ifTdShrunkScale` so the verdict's read-only IF-TD flag
preview cannot drift from the decision it previews — certified by G1.

| gate | result |
|---|---|
| G1 OFF byte-identity vs main's `gnss_processor_mwevid_main.exe` | **PASS** (products / solution / events / ZWD identical) |
| G2 E1–E3, E6 | **no narrowing** — every verdict `all reason=no_exoneration`; ON ≡ OFF digit-for-digit on every graded channel |
| G3 E4 three-band | PASS: products / solution / ZWD identical, events differ only by the new rows.  E5 equal-count: all-band, but `cb_iftd_veto` = 0 — MW(1,2) never entered the clean set, so the trap could not arise |
| G4 quiet 240-ep null ON vs `null240_evid` | DIFFERS: 519 verdicts, 38 narrowed (2 on acting detections), first divergence a 0.2 mm clock at 284100 → knob-on nulls for knob-on legs |
| G5 655-slip dose (`doseall_cb` vs `doseall_evid`, each vs its null) | levels BETTER everywhere: per-slip residual p50 at +1 0.381 → 0.199 cyc, at +45 0.151 → 0.127; other arcs of the dosed station −0.049 → +0.002; iono diff 0.019 → 0.006; handovers 138 → 116 (cold 18 → 15); T4 0 → 0; wire 0.0088 → 0.0086; detector rows identical.  Consumer: mid-leg diff 0.34 → 0.66 m peak, last bin −0.09 → −0.14 m |

**Why nothing narrowed on the harness.**  The verdict took its silence from
`getEpochCleanPairs()`, which admits an arc only under two wavelength-
proportional bars (3σ ≤ 0.7 λ_WL, |dev| ≤ 0.3 λ_WL) — a TurboEdit-grade
"affirmatively clean" test.  On this bed the narrow lanes never meet it:
measured MW scatter (`network_slip_raw.csv`, arcs ≥ 20 samples) L1C+L2W σ_MAD
p50 0.414 m against its bar 0.201 m; L1C+L5Q 0.406 m against 0.175 m.  Only
the extra-wide lanes (L2W+L5Q bar 1.37 m, L7Q+L5Q 2.28 m) ever qualify, so
the only exonerators were (2,5) and (5,7) and the only narrowing shape was
"free L1C, keep L2W and L5Q" (869 of 871 narrowings on the dose; 99.8 % of
exonerations from one lane).  An L5 slip — the case that motivated the card —
cannot be narrowed at all under that source.  **Iteration 2 (sent): silence =
the pair was evaluated this epoch, its arc count ≥ 10, and it did not flag —
the detector's own decision, not the cleanliness bar; the IF-TD veto stays.**
The basis string will carry the silent pair's threshold in WL cycles, because
on this bed the narrow lanes' thresholds run 3–11 WL cycles (the sim's code
noise is ~5× the field's), so a narrow lane's silence excludes only slips
above that; with three lanes plus IF-TD, a single missed lane produces an
INCONSISTENT verdict (every band sits in two lanes) and falls back to all, so
a wrong narrowing needs two lanes to miss the same slip — the storm bed's
T4 count is the guard for that.

**The consumer mid-leg regression is path sensitivity, not quality.**  The
consumer replays COLD from the leg start (n_sats 14–17, no fixes, absolute
error 1.7 m at +0 falling to 0.3–0.4 m by +240 on every leg); its event−null
difference over +20..+120 is two still-converging float solutions on
slightly different wires, and it ends better with the scope (−0.14 vs
−0.09 m).  The storm bed's settled consumer statistics decide the consumer;
the dose leg's honest metric is the producer-side level channels, which
improved.  Iteration-1 storm arm `runs/golden_sim_campaign_zwdblk_cbscope`
launched for the T4 guard and the settled consumer under L1-only narrowing.

### 14.5 Iteration-1 storm arm — producer better, consumer worse, and no producer-side fingerprint (2026-09-11 ~11:40; `runs/golden_sim_campaign_zwdblk_cbscope`, exe 3B40685D…, 1323 s, 720/720)

The L1-only narrowing of iteration 1 (1 160 `keep` verdicts, every one
`implicated=L1C`; 3 048 `all`), against §13.6's arm (both slip cards, no scope):

| quantity | §13.6 | + scope (iter. 1) | bar |
|---|---|---|---|
| settled ZWD truth rms | 0.0071 m | **0.0069 m** | ≤ 0.01 PASS |
| consumer settled p50 / rms | 0.174 / 0.365 m | **0.230 / 0.474 m** | p50 ≤ 0.15 **FAIL** (worse) |
| consumer hourly rms | 0.62 / 0.29 / 0.23 / 0.18 / 0.47 / 0.39 | 0.64 / 0.30 / 0.26 / 0.23 / 0.45 / **0.65** | hours 3, 4, 6 worse; 1, 2, 5 same |
| T4 events / erased ratio | 78 / 1.51 | **83** / 1.65 | ≤ 78 **FAIL** by 5 — all 5 in hour 6 (41 → 46) |
| WL / EWL / L1C ref-free | 98.9 / 100 / 87.1 % | 99.0 / 100 / 87.6 % | WL PASS; L1C ≥ 90 FAIL (87.6) |
| anchor-slip handovers (cold) | 305 (68) | **267 (57)** | cold < 68 PASS |
| re-anchors / continuous S-transforms / cold re-realizations | 476 / 356 / 99 | 442 / 333 / 88 | — |
| detector rows, clock decouples, readmissions | 2391 / 1563 / 1195 / 111 | identical | — |

**Every difference between the arms is fewer gauge moves; the detectors and
the clock tier are identical.**  The falsifier's mechanism (a band kept on an
anchor arc that had slipped, leaving a stale pinned row) is refuted by a
direct census: of the 1 160 keeps, 81 kept a band whose family was anchored
at that station, and **none of the 81 was followed by an outlier erase within
10 epochs or a tropospheric excursion** (p50 3 mm, p95 23 mm over 20 epochs,
same as all keeps); the `all` verdicts on anchor arcs are the ones followed by
excursions (p50 0.17 m) — those are the handovers.  The 5 extra erases sit in
hour 6 at stations (S10, S15, S28, S33, S36, S49) with no keep on their anchor
arcs.  So a strictly smaller, correct repair on the producer leaves the
producer's truth channels equal or better and the single consumer station
worse — with nothing at any producer station to point at.

**Reading, and the test it requires.**  The consumer is one float PPP-RTK
station; its settled median is a single realization whose value depends on
the history of every wire step it absorbed.  Two arms whose wires differ by
38 handovers can differ by tens of centimetres of consumer median without a
quality difference — and §13.6's consumer gate (0.278 → 0.174) was read as
quality.  **The calibration owed before any consumer gate is trusted on this
bed: the same config run under a numerically irrelevant perturbation
(`network.parallel_reduce` on, summation order only; config
`_golden_sim_campaign_zwdblk_mwevid_preduce.json`) — the consumer p50 / rms
difference of that pair is the bed's consumer noise floor.**  Until it is
measured, the card's consumer bar is suspended and the producer-side bars
(ZWD, integers, T4, handovers) carry the verdict; the iteration-2 storm arm
(silence from the detector's own decision) is the one that decides the card,
because iteration 1 could only ever narrow to L1.

**Addendum (2026-09-11 ~12:00) — where the consumer's loss comes from, and what it
says about the rule.**  Two censuses on the same pair of arms:

| census | §13.6 arm | + scope (iter. 1) |
|---|---|---|
| satellite-epochs whose bands are anchored at DIFFERENT stations (of 38 004 with ≥ 2 bands anchored) | 2 500 (6.6 %); hourly 3 / 6 / 9 / 8 / 8 / 7 % | **6 262 (16.5 %)**; hourly 6 / 12 / 20 / 15 / 19 / **27 %** |
| wire inter-frequency bias steps > 5 cm (L1C−L2W / L1C−L5Q / L2W−L5Q / L1C−L7Q) | 438 / 868 / 82 / 347 | 443 / 942 / 146 / 412 (+206 in total; rms of steps lower — fewer huge cold steps) |

The hourly profile of the split-anchoring share follows the consumer's hourly
error (hour 6: 27 % and 0.65 m).  Mechanism: an all-band response on an
anchor station hands over ALL of that satellite's families in one epoch, and
the sticky election tends to place them at one successor; an L1-only response
hands over the L1C family alone and leaves L2W / L5Q pinned where they were.
The satellite's per-frequency biases are then realized at different stations,
whose arc levels walk independently, and the wire carries a slowly changing
INTER-FREQUENCY inconsistency per satellite — invisible to every producer
truth grade (each band is graded separately and each is fine) but exactly what
a float user's dispersive split reads as ionospheric change on that satellite.
Every one of the 81 keeps on anchor arcs leaves its satellite split for tens
of epochs, which is the 2 500 → 6 262.  **Design consequence for iteration 3
if iteration 2 confirms it: the scope must not split a satellite's references —
on an arc that anchors any family of the satellite, do not narrow (free all
bands so the families move together), or narrow but hand over the sibling
families to the same successor.**  The path-sensitivity floor of the consumer
(§14.5's calibration twin) is still owed before the size of the effect is
believed; its direction and its hourly correlate are not in doubt.

**Instruments owed (2026-09-11 ~12:10; from the question "why not form the user's
combinations inside the network?").**  Not as observation rows — every user
combination is a linear mix of rows the uncombined solve already holds, adds
zero rank and only re-weights; falsified twice (the float MW row, the lane
layer).  As EMISSION-TIER MONITORS, yes, and they are missing:
(1) `split_referenced_sats` — per epoch and at run end, the number of
satellites whose families are anchored at different stations (the election
already knows; a few lines; rides with iteration 3's anchor clause);
(2) an inter-frequency consistency monitor on the emitted biases — per
satellite the geometry-free combination of its per-frequency biases, its
epoch-to-epoch step and its drift, MINUS the constellation-common part (a
common shift is absorbed by the user's receiver bias per frequency), with a
run-end line and a harness grader column, so a 70-epoch leg and a storm arm
both read what the user's dispersive split will feel.  The closed-loop user
remains the final arbiter once its noise floor is calibrated (§14.5).

### 14.6 Iteration 2 — silence from the detector's own decision: the rule engages (worker commit 012ff2d on its worktree; exe `gnss_processor_cbscope2.exe` A9F824AF…; 2026-09-11 ~12:00; manager re-grade of the dose pair)

| gate | result |
|---|---|
| G1 OFF byte-identity vs main | **PASS**, hashes identical to iteration 1; the OFF twins of iteration 1 and 2 are byte-identical to each other |
| G2 E1–E3, E6 (G08) | **narrowed to exactly the slipped band** — verdict rows `keep implicated=L5Q / L1C / L2W / L5Q`, exonerators `mw(L1C+L2W)silent(n=10,thr=4.2WL)` and `mw(L2W+L5Q)silent(n=10,thr=0.4WL)`, `mw(L1C+L5Q)silent(n=10,thr=4.2WL)`; per-band level residual at +14 / +45 **24–300× below the OFF twin** (e.g. L5 slip: 0.007 / 0.008 cyc vs −0.644 / −0.300); slant-iono step 0.15 m → 2 mm; other arcs' rms 0.0170 → 0.00013 cyc; station clock 0.0056 → 0.00045 m.  E1 on G10 (15°) did NOT narrow: its (1,2) arc was 8 samples old at +10 (immature, refused and counted) — the maturity guard working as specified |
| G3 controls | three-band slip: `all`, products / solution / ZWD byte-identical to OFF; equal-count L1+L2: `all` with **`IF-TD vetoes 1`** — the trap closed by the veto exactly once, as pre-registered |
| G4 quiet 240-ep null ON vs `null240_evid` | products / solution / ZWD **byte-identical**; 519 verdicts, 3 narrowed, all three on DROPPED flags (no solve effect); `immature pairs 720, weak silences 17` |
| G5 655-slip dose (`doseall_cb2` vs its null; vs `doseall_evid`) | per-slip level residual p50 at +1 / +20 / +45: **0.030 / 0.025 / 0.022 cyc** (iter. 1: 0.199 / 0.139 / 0.127; no scope: 0.381 / 0.201 / 0.151); injected arcs' pool residual 0.511 → **0.033** at +20..+39; handovers **138 → 79** (cold 18 → 8), continuous S-transforms 187 → 151, re-anchors 215 → 167; wire de-meaned rms 0.0088 → **0.0063 m**; T4 0; detector rows identical.  Verdicts 2 138: narrowed 1 151 ({L1C} 458, {L2W} 328, {L5Q} 288, {L7Q} 77 — every band now), all-band 987 (no exoneration 514 / inconsistent 473), IF-TD vetoes 20, immature pairs 1 379, weak silences 440.  Exonerating thresholds p50 3.5 WL, p90 8.3, max 26.9; 22 % above 6 WL. **Consumer:** better through the middle of the leg (diff 0.343 → 0.020 m at +120), worse in the tail — ends 0.446 m vs 0.307 (no scope) |

**The split-reference count scales with the narrowing, exactly as §14.5's
addendum predicts** (per-epoch mean / max of satellites whose families are
anchored at different stations, out of ~53 anchored, on the three dose legs):
no scope **4.8 / 7 (9.1 %)**; iteration 1 **10.6 / 14 (20.1 %)**; iteration 2
**17.0 / 23 (32.2 %)**.  Iteration 3 (sent): the ANCHOR CLAUSE — on an arc
that anchors any family of the satellite, do not narrow (free all bands so the
families move together, reason `anchor_colocate`) — plus the always-on
`split_referenced_sats` counter in the run-end clause; gates: E1 on S21
identical to iteration 2 (S21 anchors nothing), the §13 S36/G04 anchor slip
with v2 (one family handed over) vs v3 (three), and the dose with the split
count expected back at the no-scope level.  Storm arms running: iteration 2
(`runs/golden_sim_campaign_zwdblk_cbscope2`) and the consumer noise-floor
calibration twin (`runs/golden_sim_campaign_zwdblk_mwevid_preduce`).

### 14.7 Iteration-2 storm arm, the consumer instrument calibrated, and the outlier rise located (2026-09-11 ~12:40; `runs/golden_sim_campaign_zwdblk_cbscope2`, exe A9F824AF…, 1457 s; `runs/golden_sim_campaign_zwdblk_mwevid_preduce` = §13.6's arm under `parallel_reduce`)

**The consumer instrument is trustworthy.**  The calibration twin's products
and solution DIFFER from §13.6's arm at the numeric level (summation order),
yet its consumer settled p50 / rms read 0.1742 / 0.3649 against 0.1741 /
0.3649, its hourly rms is identical to three decimals, and every event class
count is identical (detections, handovers, decouples, T4 78 = 78).  A single
user station's settled median is reproducible to a tenth of a millimetre
under numeric perturbation; the changes measured under the scope are real and
come from the discrete differences (which handovers happen).  §14.5's
suspension of the consumer bar is lifted.  What the twin does NOT calibrate is
sensitivity to a different SET of discrete events; that is the mechanism
itself, and it has a census signature.

| quantity | §13.6 (both cards) | iter. 1 (L1-only) | **iter. 2 (all bands)** | bar |
|---|---|---|---|---|
| settled ZWD truth rms | 0.0071 m | 0.0069 | **0.0063 m** | ≤ 0.01 PASS |
| consumer settled p50 / rms | 0.174 / 0.365 | 0.230 / 0.474 | **0.199 / 0.324** | p50 ≤ 0.15 FAIL; rms better |
| consumer hourly rms | 0.62 / 0.29 / 0.23 / 0.18 / 0.47 / 0.39 | 0.64 / 0.30 / 0.26 / 0.23 / 0.45 / 0.65 | 0.70 / 0.25 / 0.22 / 0.19 / 0.41 / 0.34 | hours 2–6 all better than §13.6; hour 1 worse |
| T4 events / erased ratio / ambient | 78 / 1.51 / 0.229 | 83 / 1.65 / 0.226 | **107** / 0.75 / 0.198 | ≤ 78 FAIL (+29, hours 5–6: 0 → 8, 41 → 63) |
| WL / EWL / L1C ref-free | 98.9 / 100 / 87.1 % | 99.0 / 100 / 87.6 | **99.2 / 100 / 90.0 %** | L1C ≥ 90 PASS |
| anchor-slip handovers (cold) | 305 (68) | 267 (57) | **174 (28)** | cold < 68 PASS |
| verdicts / narrowed / vetoes / weak silences | — | 4208 / 1160 / 0 / — | 4208 / 3099 ({L1C} 1190, {L5Q} 850, {L2W} 775, {L7Q} 284) / 116 / 1843 | — |
| split-referenced satellites (share of anchored; hour 6) | 6.6 % (7 %) | 16.5 % (27 %) | **43.8 % (64 %)** | — |

**Where the outlier rise comes from.**  Not from the kept bands: of the 3 099
keeps, 254 kept a band whose family was anchored at that station, and 1 of
those 254 was followed by an outlier erase within 10 epochs (all keeps: 26 of
3 099; ZWD change at noise, p50 2 mm).  It comes from the split itself: **70
of the 107 erases (65 %) land on a satellite that was split-referenced at that
epoch, against a 44 % base rate** — a satellite whose per-frequency gauges
are realized at different stations reads as inter-band inconsistent at the
OTHER stations, and the tier erases their arcs.  The same split that costs the
user (§14.5) costs the producer post-fit, only later and less.

**Verdict on iteration 2:** the narrowing itself is right and cheap (every
producer truth channel improves; L1 integers reach 90 %; cold fallbacks fall
by 60 %); its one defect is that it lets a satellite's references separate.
Iteration 3's anchor clause is the fix for exactly that; its gates, against
THIS arm: split share back to ≤ 10 %, T4 ≤ 78, consumer p50 ≤ 0.174 with the
rms ≤ 0.324 kept, ZWD ≤ 0.0071, L1C ≥ 90 %, handovers below 305.

### 14.8 Iteration 3 — the anchor clause puts the references back together (worker commit 5223dc6; exe `gnss_processor_cbscope3.exe` 776537A2…; 2026-09-11 ~12:35; manager re-grade)

Change: when a verdict would narrow on an arc that anchors ANY family of the
satellite, it does not — reason `anchor_colocate`, every band freed, so all
of the satellite's families move together; plus the always-on
`split_referenced_sats` mean / max in the run-end clause (validated against
the trace-based census: 3.05 / 5 vs 3.0 / 5 on the anchor leg).  27 tests in
three suites; suite 1853 / 0 / 17.

| gate | result |
|---|---|
| G1 OFF byte-identity vs main | PASS, hashes identical to iterations 1–2 |
| S21 leg identity | `cb_l5_g08_v3` ≡ `cb_l5_g08_v2` on all four artifacts (S21 anchors nothing; the clause did not touch it) |
| the anchor leg S36/G04/L5Q −99 | v2: `keep implicated=L5Q` → ONE family handed over (G04_L5Q → S31); v3: `all reason=anchor_colocate` → **all three families handed to S31 in the same epoch**.  Local price of the co-located all-band free: slant iono 0.04 → 0.68 m, ZWD 2 → 13 mm at S36 (the §9 floor); clock quieter (0.030 → 0.021 m); consumer diff sub-mm in both |
| G5 655-slip dose (`doseall_cb3`) | split-referenced satellites **4.79 / 7 per epoch = the no-scope baseline to two decimals** (iter. 1 10.61, iter. 2 17.00; manager re-grade 4.79 / 7, share 9.1 %); handovers back to 138 (124 continuous / 14 cold vs baseline 120 / 18); 36 anchor co-locates; per-slip level residual p50 0.033 / 0.036 / 0.030 cyc at +1 / +20 / +45 (iter. 2: 0.030 / 0.025 / 0.022; baseline 0.381 / 0.201 / 0.151 — still 5× better); **consumer the best of the four arms: ends 0.294 m (baseline 0.307, iter. 2 0.446), peak event−null −0.235 m (the dosed leg beating its null)**; T4 0; detector rows identical |

The trade is now explicit and cheap: co-locating the references costs the
§9 all-band floor at the anchor station for the 36 co-locates on the dose
(~35 % on the per-slip median against iteration 2) and buys a coherent
datum, a quieter clock and the consumer.  Final validation: ONE storm arm
(`runs/golden_sim_campaign_zwdblk_cbscope3`, launched 12:35) against §14.7's
gates — split ≤ 10 %, T4 ≤ 78, consumer p50 ≤ 0.174 with rms ≤ 0.324, ZWD ≤
0.0071, L1C ≥ 90 %, handovers < 305.

### 14.9 FINAL storm arm (iteration 3) — split removed, producer better, the user's median still 3 cm worse: KEEP_OFF on the pre-registered gates (2026-09-11 ~13:00; `runs/golden_sim_campaign_zwdblk_cbscope3`, exe 776537A2…, 1464 s)

| quantity | §13.6 (both cards) | iter. 2 | **iter. 3 (anchor clause)** | §14.7 bar |
|---|---|---|---|---|
| settled ZWD truth rms | 0.0071 m | 0.0063 | **0.0071 m** | ≤ 0.0071 PASS |
| consumer settled p50 / rms | 0.174 / 0.365 | 0.199 / 0.324 | **0.207 / 0.327 m** | p50 ≤ 0.174 **FAIL** (+3.3 cm); rms better (−3.8 cm) |
| consumer hourly rms | 0.62 / 0.29 / 0.23 / 0.18 / 0.47 / 0.39 | 0.70 / 0.25 / 0.22 / 0.19 / 0.41 / 0.34 | 0.65 / 0.29 / 0.23 / 0.18 / **0.35** / 0.41 | hours 1–4 equal, 5 better, 6 slightly worse |
| T4 events / erased ratio / ambient | 78 / 1.51 / 0.229 | 107 / 0.75 / 0.198 | **86** / 0.94 / **0.197** | ≤ 78 **FAIL** (+8: hour 5 0 → 4, hour 6 41 → 46) |
| WL / EWL / L1C ref-free | 98.9 / 100 / 87.1 % | 99.2 / 100 / 90.0 | **99.2 / 100 / 89.6 %** | L1C ≥ 90 FAIL by 0.4 |
| anchor-slip handovers (cold) | 305 (68) | 174 (28) | **305 (59)** | < 305 FAIL by equality; cold better |
| split-referenced satellites | 6.6 % | 43.8 % | **6.6 % — identical to baseline hour for hour** | ≤ 10 % PASS |
| clock-anchor decouples / continuous S-transforms / cold re-realizations | 111 / 356 / 99 | — | 118 / 374 / 81 | +7 / +18 / −18 |
| verdicts / narrowed / anchor co-locates / vetoes | — | 4208 / 3099 / — / 116 | 4208 / 2994 / 105 / 116 | — |
| wire inter-frequency steps > 5 cm (L1C−L2W / L1C−L5Q / L2W−L5Q / L1C−L7Q / L5Q−L7Q) | 438 / 868 / 82 / 347 / 105 | — | 435 / 853 / 70 / 349 / 91 | equal or fewer on every pair |

**Reading.**  The anchor clause does exactly what it was built for: the
split-reference share is the baseline's to the tenth of a percent in every
hour, cold re-realizations fall 99 → 81, and the wire's inter-frequency
consistency is equal or better on every pair.  The producer's own channels
are equal or better (ambient DD error 0.229 → 0.197 cyc, erased-arc ratio
1.51 → 0.94, WL 99.2 %, L1C +2.5 points).  What remains is a small, consistent
consumer cost from narrowing on ORDINARY arcs: the settled median is +3.3 cm
(iteration 2: +2.5 cm; the calibration twin puts numeric noise at 0.1 mm)
with the rms 4 cm better, together with +7 clock-anchor decouples and +8
outlier erases, all in hours 5–6, and no fingerprint in the wire's
inter-frequency census.  **Verdict on the pre-registered gates: not met**
(consumer p50, T4, L1C by 0.4, handovers by equality).  The knob stays merged,
default OFF, byte-identical, with its observability (verdict rows, the
always-on split-reference counter) live; ledger **KEEP_OFF with the
mechanism**, no golden pin.  What would flip it: locate the 3 cm — the seven
extra decouples and eight erases sit in hours 5–6 of the slipped bed, so a
seed near hour 5 and 60-epoch legs at those epochs (with the split counter
and the inter-frequency monitor reading zero, the remaining suspect is the
single-band re-level's interaction with the clock anchor's NIS test), or a
warm-consumer replay on the two wires showing the median difference is the
consumer's own history.  ⚠ Lesson for the harness: the dose leg's cold-replay
consumer ranked iteration 3 BEST of four arms and iteration 2 worst; the
settled storm consumer ranks them the other way on the median — a 2 h
cold-replay consumer is not a consumer gate.

**Quiet-bed neutrality of the two MERGED cards (24 h twins on today's golden,
main's exe, both cards on vs both off):** ZWD 0.0073 m both; consumer settled
h12–24 rms 0.0761 → **0.0895 m (+18 %)**, p50 0.0603 → 0.0593, UP rms 0.0643 →
0.0768; T4 60 → 66.  The evidence card admitted 0 detections in those 24 h,
so the tail cost belongs to the anchor-slip handover's 6 false-alarm family
moves (all continuous).  Recorded in both ledger rows as a measured open
cost; the harness leg at those six epochs is owed (60-epoch legs, not
another 24 h arm).

## 16. Study 7 — the common-band scope's 3 cm is ONE wrong narrowing at 4.99 h (2026-09-11, afternoon; pre-registered before any leg)

### 16.1 What the two storm arms' own logs established (no runs; `scratchpad/cb3_census.py`)

A = `golden_sim_campaign_zwdblk_mwevid` (scope OFF), B = `golden_sim_campaign_zwdblk_cbscope3` (scope ON); hour h = (tow − 302400)/3600.

| hour | p50 A | p50 B | rms A | rms B | p50 up A / B | p50 horiz A / B | p50 (B−A) per epoch | epochs \|B−A\| > 5 cm |
|---|---|---|---|---|---|---|---|---|
| 0 | 0.447 | 0.467 | 0.619 | 0.646 | 0.244 / 0.248 | 0.374 / 0.381 | 0.0000 | 16 |
| 1 | 0.2475 | 0.2455 | 0.285 | 0.287 | 0.206 / 0.213 | 0.114 / 0.103 | +0.0015 | 0 |
| 2 | 0.2286 | 0.2243 | 0.230 | 0.225 | 0.134 / 0.137 | 0.181 / 0.174 | −0.0044 | 0 |
| 3 | 0.1745 | 0.1750 | 0.179 | 0.178 | 0.140 / 0.136 | 0.106 / 0.110 | −0.0006 | 0 |
| 4 | 0.0861 | 0.0925 | 0.468 | 0.354 | 0.077 / 0.081 | 0.039 / 0.046 | +0.0074 | 27 |
| 5 | 0.2798 | 0.3022 | 0.386 | 0.405 | 0.184 / 0.175 | 0.159 / 0.234 | **+0.0949** | **108** |

The settled (h3–h6) p50 0.1741 → 0.2067 of §14.9 is the mixture of an identical hour 3, a near-identical hour 4 (A's 0.468 rms spike is the larger one) and an hour 5 in which B is worse by a median **+9.5 cm on 108 of 120 epochs, all of it horizontal** (up is better).  Per-hour event counts are identical in the two arms for T0_mw_detect, T1_if_slip, PHASE_ANCHOR and READMIT; CLK_ANCHOR differs by exactly +7 disc_bump / +7 refloat in hour 4 and +7 reengage in hour 5; T4: 15 rows only in B (4 at 4.99 h — G14/S11, G11/S16, G11/S21, G20/S37 — and 11 at 5.62 h), 7 only in A, 71 common.

**The 4.99 h row, with the truth beside it (S37000SIM / G20):**

| h | truth (`truth_slips.csv`) | B's verdict |
|---|---|---|
| 4.72 | L2W +33, flagged | 4.74 / 4.75 `keep implicated=L2W` ✓ |
| 4.88 | L2W +38 | 4.88 `keep implicated=L2W` ✓ (iftd(L1C+L2W) flagged) |
| 4.98 | **L5Q −31, flagged** | — (no verdict row) |
| 4.99 | **L1C −29** | `keep implicated=L2W; exonerated=L1C;L5Q; basis=mw(L1C+L5Q)silent(n=99;thr=9.0WL);mw(L1C+L2W)flag;mw(L2W+L5Q)flag;iftd(L1C+L2W)=flagged` — **wrong on both counts** |

The other three 4.99 h T4 rows carry no verdict and no truth slip in the preceding hour: collateral of that epoch's metre-level misfit.  Truth on this bed: 2,486 slips, 2,476 single-band (5 same-epoch two-band coincidences, all with |Δ| ≥ 36 on both bands), and **|Δ| ≥ 10 cycles on every row** — the "weak silence" of §14's interlock (a lane threshold above a small slip) cannot occur here; the trap is two LARGE slips one epoch apart whose difference (2 WL) sits below the lane's 9.0 WL threshold.

### 16.2 The mechanism (the reading, frozen before the workers report)

1. The (L1C+L5Q) lane did not evaluate the LLI epoch against its arc — n was still 99 at 4.99 h, so it was neither flagged nor restarted at 4.98 — and at 4.99 h the detector saw both slips at once; the lane's signature was their DIFFERENCE, +2 WL.
2. The rule's IF-TD veto exists for the first pair only (L1C+L2W on GPS; the IF-TD detector forms one pair per satellite), so the (1,5) silence had no phase-precise check.  IF(1,5)-TD would have read 2.2606·0.1903·(−29) − 1.2606·0.2548·(−31) = **−2.52 m**.
3. L1 and L5 kept levels wrong by 29 and 31 cycles (5.5 m, 7.9 m) under stiff priors and L2 was re-leveled against them; the outlier tier refloated the arc in the same epoch and indicted three healthy arcs; seven satellites' clock anchors decoupled; the user's horizontal error sat +9.5 cm for the following hour.

What carries the HOUR is the open half.  H-clk: the seven decoupled satellites' clocks re-realize with an offset their phase biases do not follow — the fingerprint is a step at 4.99 h in the B−A difference of the consumer-visible clock-minus-bias per satellite (the census worker reports it; §14.9's "no wire fingerprint" compared each wire against itself, never the two wires against each other).  H-coll: the three collateral refloats plus the arc's own all-band re-level (the §9 0.45-cycle floor for ~45 epochs) — too short to carry an hour on their own.  The fix below does not depend on which; the storm arm does.

### 16.3 The fix, pre-registered — IF-TD on EVERY pair (`network.cycle_slip.if_phase_td_pairs`: `first` | `all`)

`first` = today (default, byte-identical).  `all` = the IF-TD driver evaluates every distinct pair present on the satellite at the station (3 on a triple-band satellite) with pools per PAIR KIND, so the first pair's decisions never change and the new pairs are a pure addition; drain semantics for a flagged new pair are today's per-pair semantics (∩ implicated when the verdict is ON); T1_if_slip rows and counters per pair; the verdict vetoes ANY silent MW pair whose IF pair flagged.  With the verdict OFF, `all` changes a response only when every MW pair is silent and only a non-first IF pair flags — the all-bands-equal case (ΔN1 = ΔN2 = ΔN5: today L1/L2 are refloated and L5 keeps a k-cycle error), absent from this bed (0 three-band coincidences), so `all` alone is expected byte-identical on the storm bed.

### 16.4 Pre-registered gates

Harness (worker, before any storm arm; quiet seed ep0665, S21000SIM, event at +10, 70 epochs, knobs explicit on every arm):
- **G1** byte-identity: `first` + scope OFF ≡ main's exe (quiet 240-epoch null and the L2 leg).
- **G2** reproduction: S1 (L5Q −31 with LLI at +10, L1C −29 at +11) and S2 (L1C −29 and L5Q −29 at +10) under ON → `keep implicated=L2W`, L1/L5 residual tens of cycles, T4 and collateral rows; under FIX → `all` naming the (L1C+L5Q) veto, products ≡ OFF.
- **G3** inertness on a genuine single-band slip: the L2 leg under FIX ≡ ON byte-identical (still narrowed to L2W).
- **G4** storm census (after the arm): `keep` verdicts with a truth slip on an exonerated band — today's count (worker census) → 0.

Storm (ONE arm at the end; the §14.3 bars unchanged): consumer settled p50 ≤ 0.174 m, T4 ≤ 78, L1C ≥ 90 %, WL ≥ 98.9 %, ZWD ≤ 0.0071 m, handovers 305, split-referenced 6.6 %.  PASS → both knobs RECOMMEND_ON and pinned together in the sim golden.  FAIL with G2/G3 passing → a second mechanism carries the hour; KEEP_OFF stands and the new fingerprint is recorded.

What falsifies the reading: the divergence epoch is not 4.99 h; or the wrong-narrowing census shows other wrong `keep` rows that cost the consumer nothing (then the collateral / anchor path is the lever, not the narrowing).

### 16.5 The census returns — the reading corrected and the storm gates REVISED before any FIX arm exists (2026-09-11, ~15:30 local; census worker's `task_b..task_h.py`, manager's `cb3_part.py` / `cb3_part2.py` / `cb3_decomp.py`)

**What falsified the §16.2 reading: the consumers do not part at 4.99 h.**  Both take an identical 1.30 m shock at **4.6833 h** — the GPS pivot G01 sets (`PIVOT handover G:G01->G02 datum_bump`, 52 clock anchors `cta_decouple` + `cta_disc_landed`, 157 CB + 156 PB + 52 CLK discontinuities DECLARED) — a full re-reference the user re-absorbs over ~40 min with metre-class excursions.  The two recoveries differ from 4.6917 h on:

| h | arm A (scope OFF) | arm B (scope ON) |
|---|---|---|
| 4.6833 | 1.301 m, identical in both (the pivot change) | 1.301 m |
| 4.80 | E31 L5Q/L7Q handover S26→S54 **COLD** (declared PB re-realization) → vertical spike **1.53 m** at 4.83 h | the same handover **CONTINUOUS** (the intact bands kept S54's arcs carry-usable) → 0.79 m |
| 4.9917 | — (σ̂₀ 0.948, nothing acted) | the wrong narrowing's fallout: 7 clock anchors decoupled with **7 CLK + 27 PB + 21 CB discontinuities declared**, G20's clock re-engaged +2.15 cm off (datum-free +1.96 cm, its phase biases unmoved), T4 acting on G20/S37 (prominence 162) and on three innocent arcs (17–23) while S37's own highest-prominence arcs (G24 884, G30 877, G22 808) were left alone; σ̂₀ 1.356 |
| 5.0–5.4 | East decays to 0 | East sits at −0.2 m, 48 epochs of B−A > 5 cm (mean +0.12 m) |

**Decomposition of the settled median (h ≥ 3), the §14.9 gate:**

| window | A p50 / rms | B p50 / rms | Δp50 |
|---|---|---|---|
| nothing excluded (n = 360) | 0.1741 / 0.3649 | 0.2067 / 0.3273 | **+3.25 cm** |
| pivot-recovery 4.6833–5.40 h excluded (n = 273) | 0.1663 / 0.2730 | 0.1566 / 0.2686 | **−0.97 cm** |
| 4.6833–5.60 excluded (n = 249) | 0.1455 / 0.2032 | 0.1385 / 0.2211 | −0.70 cm |
| 3.0–4.68 alone | 0.1091 / 0.1459 | 0.1127 / 0.1470 | +0.36 cm |
| 4.6833–5.40 alone | 0.2728 / 0.5661 | 0.3332 / 0.4673 | +6.0 cm |
| 5.40–6.0 alone | 0.4031 / 0.4724 | 0.3362 / 0.4627 | −6.7 cm |

The whole +3.3 cm is inside the recovery window of a declared datum change the knob did not cause; outside it the rule's consumer is BETTER, and over the run the rule turned nine cold handovers into continuous ones at identical (tow, family) with zero the other way (2.18 G23 L1C; 3.36 E04 L1C+L5Q; 4.12 G18 L2W+L5Q; 4.80 E31 L5Q+L7Q; 5.58 G12 L2W+L5Q; cold by hour A/B: 27/27, 11/11, 7/6, 2/0, 8/4, 13/11).

**Wrong-narrowing census:** 6 `keep` rows in 2,994 carry a truth slip on an exonerated band within 90 s — and ALL six follow a receiver-flagged slip on the same arc 30 s earlier.  In five, that flagged slip had been answered by the LLI drain at its own epoch and the row's defect is a SPURIOUS implicated band (no truth slip on it); S37/G20 is the only row that exonerated an unanswered slip (the L1C −29 landing the epoch after the flagged L5Q −31).  Truth adjacency: 30 slip pairs ≤ 60 s apart on one arc, 9 with |Δa−Δb| ≤ 12 WL, 6 of those on pairs the IF-TD never forms (L1C+L5Q on GPS, L1C+L7Q, L5Q+L7Q, L2W+L5Q); minimum |Δ| on the bed 12 cycles.  Wire: B−A datum-free clock dispersion 0.9 / 1.5 / 2.2 / 2.7 / 6.5 / 7.5 mm rms by hour — a 7 mm per-satellite difference cannot carry a 12 cm consumer difference; the two arms' emitted clocks agree to mm satellite by satellite (⚠ `simnet_sat_clock_census.py` treats the emitted record as a null-truth error and is not an absolute grade on this bed — its metre-class "tails" G27 3.28 m, G20 2.02 m are the bed's true clocks, identical in both arms).

**The storm gates, revised now (the harness gates G1–G4 of §16.4 stand unchanged):**
- The pooled settled p50 is REPORTED, not gated: its ±3 cm is the recovery path from a declared datum change.
- **W1** consumer, window-excluded: settled p50 with 4.6833–5.40 h excluded ≤ A's 0.1663 (cbscope3 already 0.1566); 3.0–4.68 h within ±5 mm of A.
- **W2** the 4.99 h fallout gone: zero `keep` rows with an UNANSWERED truth slip on an exonerated band (G4); no B-only CLK_ANCHOR decouples at 4.9917 h; no declared CLK discontinuities at 4.9917 h (A: 0); no T4 `acted` rows at 4.9917 h (A: 0).
- **W3** producer and wire not worse than cbscope3: ZWD ≤ 0.0071 m, WL ≥ 99.2 %, L1C ≥ 89.6 %, split-referenced 6.6 %, handovers 305 with cold ≤ 59; T4 ≤ 86 (the 5.62 h epoch is an attribution scatter — A 41 / B 46 rows there, the consumer gap peaks one epoch BEFORE it — so a ±5 there is not a signal).
- **W4** (report only): the pooled p50, the hourly table, the 4.6833–5.40 window alone, the disc_counter declarations by kind at 4.9917.

Verdict rule: W1–W3 PASS → `common_band_scope` and `if_phase_td_pairs: all` RECOMMEND_ON and pinned TOGETHER in the sim golden (the field exception of §14.8 stands for both).  W2 FAIL → the fix is wrong, not the rule.  W1 FAIL with W2/W3 PASS → KEEP_OFF stands and the window's contents are named.

**Added harness leg S3** (LLI-flagged L5Q −31 at +10 and nothing else; arms ON and FIX): expectation — at +10 the LLI implicates L5Q; at +11/+12 NO verdict row.  A `keep` at +11 implicating a band that did not slip reproduces the five spurious rows and names the LLI-epoch handling (the detector withholding the flagged epoch's sample and reading the next epoch as the combined jump) as a second defect, to be closed in the SAME card.

**Next card, stub — the pivot departure.**  A declared full re-reference (52 clocks, 313 biases) costs the user 1.30 m and ~40 min of recovery in BOTH arms: the largest consumer event of the settled window, and a gauge change that ought to be absorbed by transformation, not re-convergence.  Instrument: the 4.6833 h epoch of either arm, consumer-side.  `network.clock_datum_broadcast_anchored` (no pivot, no handover seam) remains the un-validated candidate (CLAUDE.md amendment 5).

### 16.6 The follow-up census — the LLI shadow, a second rule under the same knob, pre-registered (2026-09-11, ~16:00 local; census worker `task_i.py` – `task_iiib.py`)

**Every wrong `keep` sits exactly 30 s after a receiver-flagged slip on its own arc, and the lane it trusted contained the flagged band.**  The six rows re-read with the events at t−60 / t−30 / t:

| # | h | arc | silent lane | flagged band at t−30 | the flagged slip answered at its own epoch by | implicated band has a truth slip | class |
|---|---|---|---|---|---|---|---|
| 1 | 0.9917 | S60/E27 | L1C+L5Q | L7Q | (the exonerated L1C −45 at t−90 was answered at 0.9667 by mw_wl_slip + T1_if_slip) | yes (L7Q −42) | census window artifact — not wrong |
| 2 | 1.5583 | S25/E36 | **L1C+L5Q** | **L5Q** | lli_slip all-band refloat + T1_if_slip L1C;L5Q | **no** | spurious-implicate (L7Q) |
| 3 | 2.1500 | S55/G21 | **L1C+L2W** | **L2W** | lli_slip + T1_if_slip L1C;L2W | **no** | spurious-implicate (L5Q) |
| 4 | 3.0167 | S48/E30 | **L1C+L5Q** | **L5Q** | lli_slip + T1_if_slip L1C;L5Q | **no** | spurious-implicate (L7Q) |
| 5 | 3.3750 | S48/G32 | **L1C+L2W** | **L2W** | lli_slip; (L1C +39 at t−60 by T1_if_slip) | **no** | spurious-implicate (L5Q) |
| 6 | 4.9917 | S37/G20 | **L1C+L5Q** | **L5Q** | lli_slip at 4.9833 — but the L1C −29 lands AT t and T1_if_slip nominated L1C;L2W in the same epoch; the verdict overrode it and discarded the L1C half | no (L2W) | wrong-exonerate (the only one with a wire consequence) |

**How the lane goes silent.**  The traced pair's raw record (`network_slip_raw.csv` / `network_if_slip.csv`, one pair per satellite) shows the flagged epoch EVALUATED and flagging at 12–19 m against a 0.25–0.75 m threshold, then no row at the next epoch (arc reset, sample skipped).  The MW lanes that the verdict reads behave differently: a lane containing the flagged band keeps its arc — n increments straight through the flagged epoch with one sample withheld (S37/G20 L1C+L5Q n 87 → 99 across the LLI; S25/E36 L1C+L5Q 164 → 173; S48/G32 L1C+L2W 129 → 130) — and is then judged "silent, mature" against statistics that have just swallowed a slip, with empirical thresholds of 6–13 WL.  Firing lanes, by contrast, reset to `immature(n=0)` at the next epoch every time.  (⚠ This producer writes NO detector log — `mw_slip_detector.log` is 0 bytes on both storm arms, and the silent lanes have no per-epoch trace anywhere; the basis strings are their only record.  The harness legs enable the detector log as the §13.4 `_mwlog` legs did.)

**Whole-run base rates.**  Of 2,994 `keep` rows, 858 (28.7 %) sit at flagged-slip + 30 s; 853 of those re-implicate the flagged band one epoch late (a cheap single-band re-refloat of an already-answered slip) and 5 are the spurious rows above (0.58 %, against 0.12 % for keeps elsewhere with a slip nearby).  The MW detector itself is byte-identical in the two arms (3,586 refloat rows, identical pair composition): in A the late fire names one real and one innocent band 99.6 % of the time and refloats the whole arc a second time; in B the verdict narrows it and is right 853 / 858.

**Rule F2, pre-registered — under `network.cycle_slip.common_band_scope`, no new knob (OFF byte-identical):** a MW pair is not silent — it is treated as immature — at any epoch where one of its bands carried a receiver flag at the previous epoch on that (station, satellite), or where the detector did not evaluate the pair's previous sample normally (withheld / re-anchored).  Preferred source: a `prev_sample_withheld` flag in the detector's per-epoch pair export; fallback a one-epoch per-(station, sat, band) LLI memory in `NetworkRunContext`.  Basis token `mw(L1C+L5Q)lli_shadow(n=…)`, counter `cb_lli_shadow`.  Consequence: for one epoch after an LLI only the lane NOT containing the flagged band may exonerate; the 853 correct late narrowings are untouched (their silent lane does not contain the flagged band).  F2 alone closes rows 2–6; `if_phase_td_pairs: all` closes row 6 independently and the same-epoch equal-count trap on uncovered pairs (the S2 shape — absent from this bed, the common field shape).  Both are built.

Unit tests pre-registered: (a) the S37/G20 shape with the shadow on (1,5) and IF-TD pairs `first` → all / no_exoneration; (b) LLI on L5Q at t−1, lanes (1,7)/(5,7) firing late, (1,5) silent → all, never `keep implicated=L7Q`; (c) LLI on L5Q at t−1 and a genuine L1C slip at t: (1,2) fires, (1,5) shadowed, (2,5) silent-mature → still narrowed to L1C.

Leg S3 (LLI-flagged L5Q −31 at +10, nothing else), refined expectation: ON without F2 → the census predicts a spurious `keep` at +11 implicating L1C or L2W; FIX (all pairs + F2) → no verdict row at +11, or `all`.  Gate G4 (storm census after the arm) now reads: zero `keep` rows implicating a band with no truth slip within ±60 s AND zero exonerating an unanswered slip — today 5 + 1.
