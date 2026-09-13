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

## 15. The COLD anchor fallbacks — KEEP_OFF after TWO iterations: the rule works, the per-family scope was wrong, and fixing it proved the SPLIT was not the consumer's cost (`network.phase_datum_candidacy_usable` + `…_satellite`; census 2026-09-11 on `golden_sim_campaign_zwdblk_mwevid`, built + graded on `coldfb`)

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
keeps today's cold path.  **Gates (as pre-registered; §15.3–§15.6 report each one, and §15.5 replaces the leg population the last sentence names — the census proxy did not predict carry usability):** OFF byte-identical; on the storm arm the
cold count falls from 68 to ≤ 25 (the cold-start 21 remain), continuous
handovers land on arcs of age ≥ 60, consumer / ZWD / T4 not worse than §13.6;
the harness leg is the §12.7 single-event anchor slip at a station whose
satellite has a young low successor and a mature high one available.

### 15.1 The card as built — pre-registration, frozen before any leg (2026-09-11 evening; worktree `coldfb` / branch `worktree-coldfb` off main `cc689d5`)

`network.phase_datum_candidacy_usable` (bool, default **false**) +
`network.phase_datum_candidacy_min_age_ep` (int, default **60**; 0 = no age test).

When the knob is ON **and a handover is about to happen** — an incumbent EXISTS
for the key and the sticky branch did not keep it (absent, lost the key, or
banned by an injector / anchor-slip directive) — the candidate sweep gains a
FIRST pass over the SAME coverage-ranked list that accepts only candidates whose
carried value is USABLE:

```
usable(i, amb) := the continuous branch's OWN predicate on the ledger entry
                    (finite value, 0 < var <= phase_datum_handover_max_var_m2,
                     the ratio ceiling when armed, and NOT a forced-cold directive)
                ∧ (i, amb) not in this epoch's freed sets (relevel.free_ambs, carry_slipped_ambs)
                ∧ (min_age_ep == 0 ∨ age unknown ∨ n_loop_epochs − LocalCarry::birth_ep ≥ min_age_ep)
```

The predicate is ONE lambda with two readers — the candidacy pass and the
continuous branch that follows it — because a screen that drifted from the
decision it previews would elect a station the branch then refuses, which is the
defect with extra steps.  The existing coverage order ranks the usable
candidates and the FIRST is elected; with none usable the election falls through
to today's two sweeps unchanged, counted.  Two deliberate non-entries: a kept
sticky incumbent (no handover), and an incumbent-LESS key — a first sighting or
bridge exit has no outgoing frame, the continuous branch cannot run for it, and
every pin there is a 0-pin whichever station takes it.  The hygiene eligibility
test rides inside the pass, so the elected station is always one today's FIRST
sweep would also have accepted — just, possibly, a later-ranked one.

**Property, asserted in a test and stated in the header:** with the knob ON a
handover goes COLD only if no tracking candidate satisfies the continuous
branch's own predicate — `handover_cold` then counts exactly the unavoidable
ones.

Counters on `NetworkAnchorTallies` (invariant #4): `candidacy_usable_elected`,
`candidacy_rank_changed` (the elected station differs from the one today's sweep
would pick — the number that proves the pass did something), `candidacy_no_usable_fallback`,
`candidacy_skipped_unusable` split four ways (`no_entry` / `var_refused` /
`freed` / `young`); a run-end clause reports all of them and WARNS when the pass
never ran or never changed an election.  Every handover row gains
`cand_skipped=<n>;cand_first_unusable=<station|->` when armed — the PBGAUGE
per-family row and the PHASE_ANCHOR anchor-slip row both — so a cold-handover
census re-runs from the trace alone.  Tests: `tests/test_candidacy_usable.cpp`,
17 cases in two new suites (`CandidacyUsableConfig`, `CandidacyUsableElection`).

### 15.2 Step 1 — the family census, from the seed's own traces (no runs)

`scripts/ev_coldfb_census.py` reproduces the election's coverage rank from
`network_phase_anchor.csv` (the incumbent of every family) and
`network_slip_raw.csv` (age = consecutive 30 s epochs the satellite has been
seen at that station; elevation at the epoch), and lists, per anchored family,
the top-ranked non-incumbent candidate against the best MATURE (≥ 60 ep) HIGH
(> 60°) one below it.  Seed `evseed_quiet6h` ep0665 (tow 279150), 60 stations,
`phase_datum_pref_elev_deg` 15, `phase_datum_min_stations` 2.

153 families are anchored; **102** can be assessed (GPS L5Q and Galileo L7Q are
absent from `network_slip_raw.csv`, which carries only the L1C+L2W and L1C+L5Q
pairs — a signal-coverage limit of that trace, not of the election), and **70 of
the 102 qualify**: the top-ranked non-incumbent candidate is young or low while a
mature high one sits below it.  The four chosen (three by rank, plus the §12.7
family G10/S29000SIM, which qualifies on its own):

| family | incumbent (age/el) | trk | top candidate (rank/age/el) | mature alternative (rank/age/el) |
|---|---|---|---|---|
| E09 L1C+L5Q | S14000SIM 402 ep / 48.5° | 20 | S02000SIM 0 / 35 ep / 15.7° | S04000SIM 11 / 275 ep / 60.1° |
| E10 L1C+L5Q | S09000SIM 665 ep / 12.8° | 19 | S56000SIM 0 / 37 ep / 16.0° | S25000SIM 2 / 654 ep / 63.4° |
| G25 L1C+L2W | S59000SIM 114 ep / 34.3° | 16 | S60000SIM 0 / 38 ep / 14.7° | S56000SIM 1 / 256 ep / 67.0° |
| G10 L1C+L2W (§12.7) | S29000SIM 665 ep / 35.5° | 21 | S06000SIM 0 / 56 ep / 21.7° | S24000SIM 3 / 312 ep / 72.3° |

### 15.3 G1 — OFF byte-identity: PASS, on both halves

Quiet seed `evseed_quiet6h` ep0665 (tow 279150), 60-epoch legs, event at +10.

| comparison | result |
|---|---|
| base exe (unmodified worktree, `90566ebf…`) OFF-null vs ON exe (`c763c168…`) OFF-null | **IDENTICAL** — `products.txt` `f3f37bc3…`, `network_dd_fractional.csv` `852c4dc3…`, `network_events.csv` `690927f6…`, `network_solution.csv`, `network_zwd.csv`, and the run-end counter block character-for-character |
| ON exe, knob OFF vs knob ON **at the default `min_age_ep` 60** (`coldfb_null_off` vs `coldfb_null_on`) | **SOLVE IDENTICAL** — `products.txt`, `network_dd_fractional.csv`, `network_solution.csv`, `network_zwd.csv` byte-for-byte; `network_events.csv` differs ONLY by the two trace fields the armed knob appends to each handover row (273 rows both, `…from=S46000SIM` vs `…from=S46000SIM;cand_skipped=0;cand_first_unusable=-`).  But see §15.4: that identity is the maturity bar being INERT on a warm-resume leg, not the pass agreeing with today's sweep (FIXED 2026-09-12, §15.4.1 — re-measured on the v2 seed the same arm gives 36/36 elected, 11 rank-changed, `handover_cold` 3 → 0) |
| ON exe, knob OFF vs knob ON at `min_age_ep` 0 (`coldfb_null_off` vs `coldfb_null_on0`) | DIFFERS, and attributes exactly: 3 of 24 handovers changed successor (`candidacy_rank_changed` 3), `handover_cold` 3 → **0** |

The third row is the §14.4 precedent, not a failure: a knob that acts must be
graded against a **knob-ON null**, which is why the leg set carries three nulls.

### 15.4 The first finding, and it is about the HARNESS: the maturity bar is structurally inert on a warm-resume leg

`coldfb_null_on` (knob ON, `min_age_ep` at its 60 default) is byte-identical to
the OFF null, and its run-end clause says why:

```
the usable-first candidacy pass ran on 24 handover election(s) — 0 elected a candidate
whose carry is USABLE … and 24 found NO usable candidate and fell through to today's
sweeps.  414 (candidate, family) pair(s) were skipped: 0 no ledger entry, 24 refused by
the variance/ratio ceiling, 0 freed this epoch, 390 younger than 60 epochs.
```

**390 of 414 skips are `young`, and every one of them is an artifact of the
resume.**  `LocalCarry::birth_ep` is deliberately NOT snapshotted (the documented
restored-entry convention: a restored entry reads 0 ⇒ mature).  But the harvest's
own first-stash-wins rule treats `birth_ep == 0` as *not yet stamped* and stamps
it with the CURRENT `n_loop_epochs` — so the first post-resume harvest re-brands
every restored arc as **born at the resume**, and on a leg shorter than the bar
no arc can ever clear it.  The pass degrades gracefully (it falls through to
today's election, which is exactly the honest cold path), but it is measuring
nothing.

Consequences, and they are not only the harness's:

* every harness leg of this card runs the ON arm at `min_age_ep: 0`, which
  exercises the LEDGER half of the predicate — the half a 60-epoch leg can see.
  The legs are named `_on0`; `coldfb_null_on` and `coldfb_e09_on` are kept as the
  documented inert controls (27/27 fall-through, 445 young skips).
* on a production **warm-resume lineage** the bar is inert for its first
  `min_age_ep` epochs after every resume, in the safe direction.  The G3 storm
  arm is a COLD 6 h run, where `birth_ep` is the true first stash and the bar is
  live from epoch 60 on — the regime the default was sized for.
* ~~**owed:** either snapshot `birth_ep` (and `birth_s` with it), or give the
  candidacy bar a resume-aware clock.  Until then any card whose predicate reads
  `birth_ep` must state which side of a resume it was measured on.~~  **FIXED
  2026-09-12 — see §15.4.1.**  A card whose predicate reads `birth_ep` no longer
  has to say which side of a resume it was measured on; it does have to be
  built against an exe carrying the fix (everything before `worktree-birthfix`
  measured the inert bar).

#### 15.4.1 FIXED (2026-09-12) — the RESTORED sentinel, and the bar is live on a resume leg at its DEFAULT

**Mechanism: a sentinel, not a format bump.**  `LocalCarry::kBirthEpRestored`
(−1) / `kBirthSRestored` (−1.0) — the warm resume stamps every restored entry
with them (`makeRestoredLocalCarry`, `NetworkRunContext.cpp:1250` for the `L`
rows and `:1279` for `Z`), and the harvest's first-stash-wins rule now tests
`!= 0` instead of `> 0` (`localCarryStampBirth`, `NetworkRunTypes.hpp:531`,
called at all three stash sites — `NetworkRunContext.cpp:3636` ambiguity,
`:4056` in-block iono, `:4242` in-block rx clock).  `0` still means "never
stashed"; `−1` means "restored, true age not snapshotted ⇒ MATURE"; the two are
now DISTINGUISHABLE, which is the entire defect.  **Nothing is added to the
snapshot record or to the warm-resume fingerprint**, so the fix applies to every
seed already on disk — which is also why the alternative (snapshot `birth_ep` at
a format bump) was NOT taken: it could not have been A/B'd on
`evseed_quiet6h_v2` at all, because that seed was written before it.

**The reader audit came back clean: all three readers already tested `> 0` /
`<= 0`, so none needed a logic change.**  The two `birth_ep` readers — the F3
block admission gate (`ApplicationNetworkAssembly.cpp:3499` / `:3525`) and the
candidacy maturity bar (`NetworkPhaseDatum.cpp:325`) — were merged onto ONE
predicate, `localCarryYoung` (`NetworkRunTypes.hpp:505`), so they cannot drift
apart.  The `birth_s` reader (the sim-only slip oracle's window floor,
`NetworkSlipOracle.cpp:267`) keeps LOSING the window's `max` for the arc's whole
restored life instead of being floored at the resume second from the first
post-resume harvest on — the behaviour its own header always documented.
`relevel_s` was never affected and needs no sentinel: it is stamped only on
CREATE and on FREE (`NetworkYoungInflate::stamp`), never on an ordinary
re-stash, so a restored entry's 0 survives every harvest and reads mature by
`varAdd`'s own `relevel_s > 0` test.  Unit suite:
`tests/test_restored_arc_age.cpp` (`RestoredArcAge`, 10 cases).

**Measured.**  `runs/evseed_quiet6h_v2` ep0665 (fingerprint `c5b5df834d185ac8`,
WARM RESUME confirmed: 2637 ambiguity carries + 60 Z rows + 355 D2 shared
carries restored, `state_snapshot_require` on), 60-epoch NULL legs, `--extra`
arming `phase_datum_candidacy_usable` + `phase_datum_candidacy_min_age_ep`
(QC/election class, NOT fingerprinted — the loader accepted it on the resume
leg, as §18.1 rule (2) allows).  Two exes differing ONLY by this fix: OLD
`1B37E94BB6DA82FE…` (main `6a83728`), FIXED `06580E71AA86A37F…`.

| channel (run-end clause) | knob OFF (either exe) | ON @ 60, OLD exe | ON @ 60, FIXED exe |
|---|---|---|---|
| elections the pass entered | — | 39 | 36 |
| elected a candidate whose carry is USABLE | — | **0** | **36** |
| of which NOT today's pick (`candidacy_rank_changed`) | — | 0 | **11** |
| found NO usable candidate, fell through | — | **39** | **0** |
| (candidate, family) pairs skipped | — | 679 | 10 |
| … no ledger entry | — | 0 | 0 |
| … refused by the variance/ratio ceiling | — | 46 | 3 |
| … freed this epoch | — | 31 | 7 |
| … **younger than 60 epochs** | — | **602** | **0** |
| PB-GAUGE anchor handovers | 39 | 39 | 36 |
| … CONTINUOUS / **COLD** | 36 / **3** | 36 / **3** | 36 / **0** |
| handover rows carrying a screen decision (`cand_skipped > 0`) | — | 0 of 39 | **9 of 36** (`cand_first_unusable` = S31000SIM ×3, S02000SIM ×2, S29000SIM ×2, S56000SIM ×2) |
| consumer 3D, final epoch (held-out SZZ000SIM) | 0.833 m | 0.833 m | 0.760 m |

The OLD-exe ON leg is the §15.3 inert control reproduced on the v2 seed: same
39/36/3 handovers as the OFF null, `cand_skipped=0` on every traced row, and
**602 of 679 skips are `young`** (the SPEC's original 390 of 414 was the same
phenomenon on the pre-promotion `evseed_quiet6h` seed).

**OFF-path byte-identity (invariant #6).**  Same legdir, same `leg.json`, the
two exes: `products.txt` `f6588a96d157dad836214f3eafde379133d4fc2c8dffe3d7fc774364a86952b6`,
`network_dd_fractional.csv` `5842740049dd86f0…`, `network_events.csv`
`0272423d6276ab2f…`, plus `network_solution.csv`, `network_zwd.csv`,
`network_epoch_status.csv`, `network_phase_anchor.csv` — all IDENTICAL, and the
run-end block differs only in four wall-clock lines (RINEX parse seconds, three
`[RT] obs backlog` wall-clock deltas).  Expected: the three readers are all
knob-gated (`phase_leveling_local_block_min_age_ep` 0, `candidacy_usable` off,
`oracle_relevel` off in the golden), so a resume leg with the age knobs unarmed
cannot see the sentinel.

**The bar is now TRANSPARENT, not BLIND — and that is the test.**  On the fixed
exe `min_age_ep: 60` is **byte-identical to `min_age_ep: 0`** (`products.txt`
`903DFAAC0E7A5896…` both, plus all six traces; the skip partition identical at
0/3/7/0) — the restored-entry contract delivered: on a leg whose candidates all
predate the snapshot, the maturity test correctly refuses nobody, so the LEDGER
half decides.  Before the fix those two bars were completely different runs
(§15.3's third row: 3 rank changes, `handover_cold` 3 → 0 only at bar 0).  The
bar's BITE is pinned by `RestoredArcAge.AGenuinelyYoungCandidateIsStillRefused`
and not by this leg: with the fix the sweep finds a usable candidate at rank 0–2
every time, so it never walks down to an in-leg-born arc, and `skip_young` stays
0 for a reason a longer or slipped leg would change.

### 15.5 G2 — the census proxy named the wrong families, and the production path answered anyway

**The four chosen families all hand over CONTINUOUSLY with the knob OFF.**  Each
`anchor_handover` directive at +10 retires the incumbent and lets the production
rules choose; on this bed the top-ranked candidate's carried value was usable
every time:

| leg | injected family | OFF successor (elev) | OFF branch | ON0 successor | ON0 branch |
|---|---|---|---|---|---|
| `coldfb_e09_*` | E09 L1C/L5Q/L7Q | S02000SIM (17.6°) | continuous | S02000SIM | continuous |
| `coldfb_e10_*` | E10 ×3 | S56000SIM (17.7°) | continuous | S56000SIM | continuous |
| `coldfb_g25_*` | G25 ×3 | S60000SIM (15.9°) | continuous | S60000SIM | continuous |
| `coldfb_g10_*` (§12.7 family) | G10 ×3 | S06000SIM (24.1°) | continuous | S06000SIM | continuous |
| `coldfb_g30_*` (replacement, see below) | G30 ×3 | S02000SIM (68.5°) | continuous | S02000SIM | continuous |

So **arc age and elevation at the snapshot epoch do not predict carry
usability** on this bed — the §15 census's proxy is the wrong screen, and the
only oracle is the branch the election actually takes.  (The census remains the
right *shape* test: it found families whose top-ranked candidate is young or low
with a mature high one below, 70 of 102 assessable.)

**The cold population on this bed is NATURAL, not forced.**  Every leg that does
not inject on G30 carries the same event at +22 (tow 280140): the G30 family's
sticky incumbent S25000SIM loses the key and the election hands all three bands
over.  With the knob OFF it lands on the top-ranked candidate **S31000SIM at
12.59°**, whose ledger value is refused by the 0.01 m² ceiling, and all three
families are pinned at 0 — a COLD re-realization.  With the knob ON (`min_age_ep`
0) the pass skips exactly that one candidate and elects **S12000SIM at 39.81°**,
whose value is usable, and all three go CONTINUOUS at targets −13.65 / −11.86 /
+3.97 m.  The trace says so in one row: `cand_skipped=1;cand_first_unusable=S31000SIM`.

**Six independent pairs, one result.**  (`cand_*` from the run-end clause;
consumer = the closed-loop 3D error at +40 and at the leg's last epoch; ZWD =
pooled error against `truth_tropo.csv` over all 60 stations × 62 epochs, n = 3720.)

| pair | cand entered / usable-elected / rank-changed | skips (var / freed) | cold OFF → ON | consumer +40 (OFF → ON) | consumer end | ZWD rms (OFF → ON) | ZWD max abs |
|---|---|---|---|---|---|---|---|
| null | 24 / 24 / 3 | 3 / 0 | **3 → 0** | 1.711 → 1.665 (−46 mm) | 1.546 → 1.490 | 0.07211 → 0.07180 | 0.363 → 0.335 |
| e09 | 27 / 27 / 3 | 3 / 0 | **3 → 0** | 2.622 → 2.512 (−110 mm) | 2.538 → 2.413 | 0.07238 → 0.07208 | 0.363 → 0.335 |
| e10 | 27 / 27 / 3 | 3 / 0 | **3 → 0** | 1.707 → 1.661 (−46 mm) | 1.539 → 1.484 | 0.07213 → 0.07182 | 0.363 → 0.335 |
| g25 | 27 / 27 / 3 | 3 / 0 | **3 → 0** | 1.920 → 1.857 (−62 mm) | 1.711 → 1.633 | 0.07211 → 0.07180 | 0.364 → 0.335 |
| g10 | 27 / 27 / 3 | 3 / 0 | **3 → 0** | 1.767 → 1.756 (−10 mm) | 1.621 → 1.610 | 0.07212 → 0.07181 | 0.364 → 0.335 |
| slip (§12.7, 40 cyc L1C at S29000SIM/G10, `anchor_slip_handover` armed both sides) | 39 / 39 / **9** | 3 / **7** | **3 → 0** | 1.113 → 1.241 (**+128 mm**) | 1.061 → 1.172 | 0.07262 → 0.07184 | 0.363 → 0.335 |

T4 attribution rows: **0 in every leg, both arms** (a quiet bed; the injected slip
is taken by the MW drain, never by T4), so the T4 half of the gate is vacuous
here and is reported as such.

Three things in that table are worth naming:

1. **The slip leg is the only one where the `freed` clause fires** — 7 candidates
   refused because a slip response had freed their own arc that epoch — and it is
   the only leg where the pass moves more than the three G30 elections (9 rank
   changes).  Invariant #1 at candidacy time, on the production path, with a
   counter.
2. **The slip leg is also the only channel that went the wrong way**: its
   consumer is 11–13 cm worse with the knob ON while its ZWD is better (rms
   0.07262 → 0.07184) and its cold count still falls 3 → 0.  The consumer replays
   COLD from the leg start and is path-sensitive over 60 epochs (§14.4's standing
   caveat), and the six extra elections this leg moves are its own; it is
   recorded, not explained away.
3. The emitted G30 phase-bias level differs between the arms by 0.87 m (L1C) and
   8.73 m (L2W) from the handover epoch on — that is the *datum*, not an error
   (invariant #5): the cold arm re-realized the family on S31000SIM's ambiguity
   while the ON arm preserved the outgoing frame.  The satellite clock difference
   is 3–6 mm.

**G2 verdict: PASS on the mechanism, with the population replaced.**  In every
pair the cold handovers are exactly the avoidable ones and the pass removes all
of them (3 → 0, six times), the successor moves from a 12.6° unconverged arc to a
39.8° converged one, ZWD is equal-or-better everywhere and the consumer is better
in five pairs of six.  The pre-registered form of G2 ("OFF = cold on each chosen
family") cannot be met on this bed by injection at all, and that is a property of
the bed, not of the card.

### 15.6 G3 — the storm arm: every producer channel the card targets improves, and the USER'S PRODUCT does not

`config/test_suite/_golden_sim_campaign_zwdblk_coldfb.json` = the cbscope5 arm plus
`network.phase_datum_candidacy_usable` and the run path, nothing else (verified by a
flattened key-by-key diff).  Exe `gnss_processor_coldfb_storm.exe` = `c763c168…`, alone
on the box, 720/720 epochs, exit 0, **1226 s**.  `min_age_ep` at its 60 default — this
is a COLD run, so the bar is live from epoch 60 on.  A = scope OFF (`mwevid`), C4 =
cbscope4, **C5 = cbscope5 (the base this card starts from)**, B = this arm.

| channel | A (OFF) | C4 | C5 | **B (coldfb)** | bar | verdict |
|---|---|---|---|---|---|---|
| **PHASE_ANCHOR handovers / cold** | 305 / 68 | 291 / 51 | 315 / 78 | 309 / **46** | cold ≤ 30 | MISS by 16 |
| ...cold-start (first 30 min) / settled | — | — | 27 / 51 | **27 / 19** | (reported apart) | settled −63 % |
| ...cold by hour h0…h5 | 27 11 7 2 8 13 | — | 41 12 7 0 5 13 | **38 8 0 0 0 0** | | **zero from hour 2** |
| **PB-GAUGE elected / continuous / cold re-realizations** | — | 442/369/73 | 462/362/100 | 473/421/**52** | cold ≤ 40 | MISS by 12 (settled 67 → 19) |
| **continuous successor arc age p10 / p50 / p90 (ep)** | — | — | 91 / 240 / 459 | **99 / 231 / 475** | p10 ≥ 60 | **PASS** |
| candidacy pass: entered / usable-elected / **rank-changed** / fell through | — | — | — | 473 / 404 / **53** / 69 | | the pass moved 53 elections |
| candidacy skips: no_entry / var-refused / freed / young | — | — | — | 0 / 1074 / 2 / 161 | | |
| **consumer settled p50 (pooled) / rms** | .1741 / .365 | .1673 / .331 | **.1204 / .2215** | **.1794 / .2823** | ≤ .1204 | **FAIL** (+5.9 cm, worse than A) |
| W1 window-excluded p50 (4.6833–5.40 out) | .1663 | .1584 | .1028 | **.1456** | ≤ .1028 | **FAIL** (better than A) |
| consumer hourly p50 h0…h5 | .447 .247 .229 .174 .086 .280 | .469 .283 .268 .165 .099 .347 | .524 .113 .114 .101 .090 .236 | **.610 .263 .240 .186 .109 .204** | | h1–h3 roughly doubled; h5 best of every arm |
| consumer hourly rms | .62 .29 .23 .18 .47 .39 | .61 .27 .26 .16 .36 .42 | .65 .11 .12 .10 .24 .28 | **.69 .26 .236 .18 .327 .315** | | |
| pivot-recovery window 4.68–5.40 alone / last 36 min | .273 / .403 | .212 / .431 | .221 / .267 | **.314 / .160** | | window worse, tail **best of every arm** |
| **ZWD settled truth rms / span p50** | .0071 / .031 | .0064 / .020 | .0069 / .021 | **.0061 / .0199** | ≤ .0069 | **PASS — best of every arm** |
| **ref-free WL / EWL / L1C** | 98.9/100.0/87.1 | 99.2/99.9/90.1 | 99.2/100.0/89.9 | **99.3/100.0/90.9** | ≥ 99.2 / ≥ 89.9 | **PASS — best of every arm** |
| T4 events / ambient | 78 / .229 | 11 / .196 | 11 / .197 | **11 / .196** | ≤ 11 | PASS |
| W2 4.9917 h: T4 rows / anchor decouples / declared CLK breaks | 0/0/0 | 0/0/0 | 0/0/0 | **0/0/0** | absent | PASS |
| G4 `keep` exonerating an unanswered slip / spurious-implicate | — | 0 / 2 | 0 / 2 | **0 / 2** | 0 / (residual) | PASS / unchanged residual |
| **split-referenced satellites, mean / max of 52.8** | 3.5 / 7 | 7.6 / 13 | **0.0 / 0** | **0.9 / 3** | 0 | **FAIL** |
| anchor stations holding the datum, per epoch (GPS / Gal) | — | — | 17.26 / 11.42 | **17.76 / 11.63** | | +0.5 / +0.2 |
| common-band scope verdicts / narrowed / LLI-epoch (narrowed, co-located) | — | — | 4208 / 3147 / 1205 (1153, 52) | 4208 / 3148 / 1205 (1151, 54) | | unchanged |
| run time | 1130 s | 983 s | 1050 s | **1226 s** | | +17 % |

**The mechanism is proven and the cost has a name.**  The pass did exactly what §15
said it would: 53 of 473 elections went to a station today's sweep would not have
taken, the settled cold handovers fell 51 → 19 and are **zero from hour 2 onward**
(the 27 that remain are the cold-start 30 minutes, where every arc is young by
construction and no candidacy rule can help), the successors it picks are OLDER
(p10 91 → 99 epochs), and every producer-side channel it could plausibly move
improved — ZWD settled rms 0.0069 → **0.0061** and ref-free L1C 89.9 → **90.9**,
both the best of any arm on this bed, with WL, EWL and T4 equal-or-better.

**But the user's product is materially worse**, and worse than the scope-OFF
baseline: pooled settled p50 0.1204 → 0.1794 m (+49 %), hours 1–3 roughly doubled,
and the window-excluded median 0.1028 → 0.1456.  The named cause is visible in the
same run: **split-referenced satellites 0.0 → 0.9 mean / 3 max**, and the datum
spread over +0.5 GPS / +0.2 Galileo anchor stations per epoch.  The screen is
**per-FAMILY** — each (sat, signal) independently elects the best-carried host — so
when a satellite's three bands hand over in one epoch they can land on three
different stations, each correct for its own band and jointly an inter-frequency
inconsistency the per-band producer instruments cannot see and the user pays for.
That is exactly the shape §16.7/§16.8 measured for the LLI-epoch narrowing (producer
channels clean, consumer −3.5 to −7 cm/hour) and exactly the defect the **anchor
clause** cured there by moving a satellite's families together.

### 15.7 Verdict and what is owed

**`network.phase_datum_candidacy_usable` is KEEP_OFF (2026-09-11), merged default-OFF
and byte-identical OFF.**  Three of the pre-registered G3 bars fail — consumer pooled
p50, consumer window-excluded p50, and split-referenced = 0 — and two more miss
because the cold-start 30 minutes are irreducible (cold 46 vs ≤ 30, cold
re-realizations 52 vs ≤ 40, both with their settled halves down 63–72 %).  The rule
itself is not refuted: it is the ONLY arm on this bed with zero settled cold
handovers from hour 2, the best ZWD and the best L1C, and the invariant-#1 `freed`
clause fires on the production path (2 refusals on the storm arm, 7 on the harness
slip leg).  What is refuted is the **per-family scope**.

Pre-registered next iteration, before it exists: **the SATELLITE clause.**  When a
satellite's families are handed over in the same epoch, elect ONE successor for all
of them — the highest coverage-ranked station that is usable for the most of the
satellite's bands (ties to the coverage rank), with the per-family fall-through kept
for a family that hands over alone.  Gates: split-referenced back to 0.0 / 0, the
settled cold count no worse than this arm's 19, and consumer pooled p50 ≤ 0.1204
(cbscope5) — i.e. the producer wins of this arm kept while the inter-frequency
inconsistency that bought them is removed.  The §15.4 `birth_ep` resume defect is a
separate, smaller card and is independent of this one.

⚠ **§15.8 reports that iteration: it met its own primary gate (splits 0.0 / 0) and FALSIFIED this paragraph's mechanism — the consumer got 1.5 cm WORSE with every split removed.  Read §15.8 before quoting the split hypothesis from here.**

**Lessons for the memory file, both paid for here:**

1. **A candidacy screen that acts per-BAND on a multi-band satellite buys producer
   quality with a SPLIT REFERENCE, and the producer's own per-band instruments say
   only the good half.**  ZWD, ref-free integer rates and the cold-handover counters
   all improved while the user's median went up 49 %.  Any election-side rule must
   report the split-referenced census next to its own counters.
2. **Arc age and elevation do not predict carry usability.**  Five injected
   anchor-handover families chosen on that proxy all handed over CONTINUOUSLY with
   the knob off; the only cold population on the quiet bed was one unforced handover
   nobody had selected.  For a card about the ledger, screen on the LEDGER — or on
   the branch the election actually takes.

### 15.8 ITERATION 2 — the SATELLITE clause: it removes every split, it fixes the slip leg, and on the storm arm it REFUTES the reading that motivated it

`network.phase_datum_candidacy_satellite` (bool, default **TRUE** under
`phase_datum_candidacy_usable`; OFF reproduces iteration 1 exactly, which is what
makes the A/B a single knob and what makes the two storm arms differ only in the
code default).  Built on the §15.7 reading: the per-family screen is right about
WHICH carry is usable and wrong about WHOSE satellite it belongs to.

**The rule** (locked before any leg).  When the candidacy pass is armed:

* **(a) JOINT resolution** — every family of satellite *s* handing over THIS epoch
  takes ONE successor: the first coverage-ranked candidate usable for the LARGEST
  number of them, ties to coverage order.  A family that host is not usable for is
  still pinned THERE, through the honest cold path (`sat_clause_cold_colocated`):
  a cold pin on the same host is a frame step, not a split.  No candidate usable
  for any mover ⇒ today's sweeps unchanged, counted.
* **(b) PARTIAL handover** — when only some of *s*'s families move, the station
  already anchoring the others is tried FIRST and, if it tracks and is usable for
  at least one mover, takes them all (`sat_clause_colocated`).  ⚠ *Reading, not
  transcription:* taking the held host on "usable for ≥ 1" rather than only on a
  joint-maximum tie is the only reading under which (c) can hold — letting a
  higher-scoring stranger win re-creates exactly the split this iteration removes,
  and a family the held host cannot serve takes a co-located cold pin instead.
* **(c) the census** — the run-end clause reports the per-satellite distinct-anchor
  numbers from the SAME always-on counters the storm grade reads
  (`networkSplitReferencedSats`), so the clause and the grade agree by
  construction rather than by agreement.

One acceptance lambda now has three readers (the per-family pass, the joint
scoring, the co-located cold decision).  The per-family pass still runs under the
clause — it owns the four skip counters and the row's `cand_skipped` /
`cand_first_unusable`, and it is the FAMILY-ONLY pick `sat_clause_split_avoided`
is measured against.  A family the joint host does not TRACK cannot be pinned
there and falls back per-family: the only residual split, counted and warned.
Tests: +7 cases in the same two suites (24 total), each asserting the
per-satellite distinct-station count; full Debug suite **1924 / 261 suites, 1907
passed, 17 skipped, 0 failures**.

**Harness** (quiet seed ep0665, 60-epoch legs, ON arms at `min_age_ep` 0 for the
§15.4 warm-resume reason, which is unchanged).

| leg | exit / wall | split-ref mean / max | handovers / continuous (cold) | consumer +40 | consumer end | ZWD pooled rms |
|---|---|---|---|---|---|---|
| `null_off` | 0 / 188.0 s | 0.0 / 0 | 24 / 21 (**3**) | 1.7108 | 1.5464 | 0.07211 |
| `null_on0` (iter 1) | 0 / 144.1 s | 0.0 / 0 | 24 / 24 (0) | 1.6645 | 1.4904 | 0.07180 |
| `null_on2` (iter 2) | 0 / 151.5 s | 0.0 / 0 | 24 / 24 (0) | 1.6645 | 1.4904 | 0.07180 |
| `g30_off` / `g30_on0` / `g30_on2` | 0 / 174.3, 182.6, 145.5 s | 0.0 / 0 | 24 / 24 (0) | 1.8664 (all three) | 1.6769 (all three) | — |
| `slip_off` (§12.7, 40 cyc L1C at S29000SIM/G10) | 0 / 138.1 s | **4.3 / 6** | 41 / 38 (**3**) | 1.1132 | 1.0614 | 0.07262 |
| `slip_on` (iter 1) | 0 / 142.5 s | **6.0 / 7** | 39 / 39 (0) | 1.2412 | **1.1718** (+110 mm) | 0.07184 |
| `slip_on2` (iter 2) | 0 / 151.1 s | **4.3 / 6** (= OFF) | 38 / 38 (0) | 1.0046 | **1.0240** (−37 mm vs OFF) | 0.07209 |

`null_on2` and `g30_on2` are **byte-identical** to their iteration-1 twins
(products, DD and solution all match) — those windows had no split to avoid, and
the clause's own counters say so (`sat_clause_split_avoided` = 0 there).  The slip
leg is the one that moved, and it is the leg iteration 1 hurt: 16 satellite-epochs
resolved jointly over 38 families, **2 by the held-host preference and 3 splits
avoided**, `handover_cold` still 3 → 0.

**The attribution is clean.**  On the slip leg the emitted G10 wire is essentially
the same in both iterations (clock Δ −0.0007 m, L1C Δ −0.039 m, L2W Δ −0.046 m at
the leg end), while the consumer difference against OFF flips **+0.110 m → −0.037
m**.  The consumer gain is therefore NOT the injected family's own wire — it is
the satellites whose references iteration 1 split and iteration 2 did not.  That
is the §14.7 channel, measured directly: an inter-frequency inconsistency the
per-band producer instruments cannot see and the user pays for.

**G3 storm arm `coldfb2`** (`config/test_suite/_golden_sim_campaign_zwdblk_coldfb2.json`
= the `coldfb` arm on iteration-2 code, where the clause is the code default, so the two
arms differ by exactly that; exe `gnss_processor_coldfb2_storm.exe` = `1b80ce25…`,
720/720, exit 0, 1406 s).  **B** = iteration 1 (`coldfb`), **B2** = this arm.

| channel | A (OFF) | C5 (cbscope5) | B (iter 1) | **B2 (iter 2)** | pre-registered bar | verdict |
|---|---|---|---|---|---|---|
| **split-referenced satellites, mean / max of 52.8** | 3.5 / 7 | 0.0 / 0 | 0.9 / 3 | **0.0 / 0** | 0.0 / 0 | **PASS** |
| PHASE_ANCHOR handovers / cold | 305 / 68 | 315 / 78 | 309 / 46 | 312 / **46** | ≤ 46 | **PASS** |
| ...cold-start / settled | — | 27 / 51 | 27 / 19 | **27 / 19** | settled ≤ 19 | **PASS** |
| ...cold by hour h0…h5 | 27 11 7 2 8 13 | 41 12 7 0 5 13 | 38 8 0 0 0 0 | **38 8 0 0 0 0** | | zero from hour 2 |
| PB-GAUGE elected / continuous / cold re-realizations | — | 462/362/100 | 473/421/52 | 468/416/**52** | ≤ 52 | **PASS** |
| continuous successor arc age p10 / p50 / p90 | — | 91 / 240 / 459 | 99 / 231 / 475 | **116 / 236 / 478** | p10 ≥ 60 | **PASS** |
| candidacy entered / usable-elected / rank-changed / fell through | — | — | 473/404/53/69 | 468/399/**60**/69 | | |
| SATELLITE clause: sat-epochs / families / co-located / **splits avoided** / cold co-located / untracked / no-usable | — | — | — | 133 / 399 / 0 / **13** / 0 / 0 / 23 | | the clause fired |
| **consumer settled p50 (pooled) / rms** | .1741 / .365 | .1204 / .2215 | .1794 / .2823 | **.1941 / .3091** | ≤ .1204 | **FAIL** |
| W1 window-excluded p50 | .1663 | .1028 | .1456 | **.1751** | ≤ .1028 | **FAIL** |
| consumer hourly p50 h0…h5 | .447 .247 .229 .174 .086 .280 | .524 .113 .114 .101 .090 .236 | .610 .263 .240 .186 .109 .204 | **.610 .264 .243 .189 .110 .232** | h1–h3 within +2 cm of C5 | **FAIL** (+15 / +13 / +9 cm) |
| consumer hourly rms | .62 .29 .23 .18 .47 .39 | .65 .11 .12 .10 .24 .28 | .69 .26 .236 .18 .327 .315 | **.69 .26 .24 .182 .31 .397** | | |
| pivot window 4.68–5.40 / last 36 min | .273 / .403 | .221 / .267 | .314 / .160 | **.367 / .219** | | |
| **ZWD settled truth rms / span p50** | .0071 / .031 | .0069 / .021 | .0061 / .0199 | **.0061 / .0197** | ≤ .0069 | **PASS — best, tied with B** |
| **ref-free WL / EWL / L1C** | 98.9/100/87.1 | 99.2/100/89.9 | 99.3/100/90.9 | **99.3/100/90.8** | ≥ 99.2 / ≥ 89.9 | **PASS** |
| T4 events / ambient | 78 / .229 | 11 / .197 | 11 / .196 | **11 / .196** | ≤ 11 | **PASS** |
| W2 4.9917 h T4 / decouples / declared CLK breaks | 0/0/0 | 0/0/0 | 0/0/0 | **0/0/0** | absent | **PASS** |
| G4 unanswered-exonerate / spurious-implicate | — | 0 / 2 | 0 / 2 | **0 / 2** | 0 / (residual) | **PASS** |
| anchor stations holding the datum / epoch (GPS / Gal) | — | 17.26 / 11.42 | 17.76 / 11.63 | **17.16 / 11.56** | | back to C5's spread |
| run time | 1130 s | 1050 s | 1226 s | **1406 s** | | +34 % vs C5 |

**The clause did its job and the consumer did not follow.**  Split-referenced
satellites are **0.0 / 0 all run** — the primary pre-registered gate, met exactly, with
13 splits avoided and the datum spread back to C5's (17.16 vs 17.26 GPS stations per
epoch).  Every producer gate passes and three are the best of any arm.  And the
consumer is **worse than iteration 1** (pooled p50 .1794 → .1941), not better.

**So the §15.7 reading is FALSIFIED: the split reference was a correlate, not the
mechanism.**  Removing every split moved the consumer the wrong way by 1.5 cm.

**What the consumer difference actually is — measured, and it is a grading trap.**
Decomposing the settled consumer error into its mean ENU components:

| arm | Ē (m) | N̄ (m) | Ū (m) | \|U\| p50 | 3D p50 |
|---|---|---|---|---|---|
| A (scope OFF) | **+0.0771** | +0.0309 | −0.1615 | 0.1186 | 0.1741 |
| C5 (cbscope5) | **+0.0381** | −0.0255 | −0.0437 | 0.0739 | 0.1204 |
| B (iter 1) | **+0.0776** | −0.0295 | −0.0072 | 0.1036 | 0.1794 |
| B2 (iter 2) | **+0.0786** | −0.0452 | −0.0411 | 0.1124 | 0.1941 |

The EAST bias is +7.7 to +7.9 cm on **A, B and B2 alike** and +3.8 cm on C5 — C5 is the
outlier, not the candidacy arms.  The static bias-vector magnitude is 6.3 cm for C5 and
9.9 cm for B2, which is ~3.6 cm of the 7.4 cm median gap; and the vertical, which
iteration 1 disturbed (Ū −0.007 against C5's −0.044), iteration 2 puts back (−0.041).
Against the scope-OFF baseline the card costs **+0.5 cm (B) / +2.0 cm (B2)** on the 3D
median, not the +49 % the C5 comparison reads.

That does not rescue the gates — they were set against C5 and they fail — but it names
what the failure is measuring: **a single-realization horizontal datum bias that differs
between C5 and every other arm on this bed, including the arm the card was built on.**
Standing invariant #5 says exactly this about consumer position as a grading channel, and
the correct instrument here is a datum-invariant one.

### 15.9 Verdict on iteration 2, and what iteration 3 must test

**`network.phase_datum_candidacy_usable` stays KEEP_OFF**, now with both iterations
merged default-OFF and the satellite clause default-ON beneath it.  The pre-registered
gates: **11 pass** (split 0.0/0, cold 46 total and 19 settled, PB-GAUGE cold 52,
successor age p10 116, ZWD 0.0061, WL 99.3, L1C 90.8, T4 11, W2 absent, G4 unchanged) and
**3 fail**, all three consumer: pooled p50 .1941 vs ≤ .1204, window-excluded .1751 vs
≤ .1028, hourly h1–h3 +9 to +15 cm vs the +2 cm allowance.

What iteration 2 SETTLED, and it is worth the run on its own:

1. **The split reference is not the consumer cost.**  Zero splits, consumer 1.5 cm
   worse.  The §15.7 mechanism is refuted by its own fix.
2. **The clause is nonetheless right and cheap** — it costs 13 election changes in 6 h,
   restores the datum spread, restores the vertical, and keeps every producer gain.  It
   is the form the knob should keep if it is ever armed.
3. **The consumer comparison against cbscope5 is confounded** by a 4 cm east-bias
   difference that the candidacy arms share with the scope-OFF baseline.

**Iteration 3, pre-registered before it exists.**  The open question is no longer "which
station" but "what does moving the datum host at all cost the user", and the C5
comparison cannot answer it because C5's own realization differs.  Two things, in order:

⚠ **§15.11 reports iteration 4 on the NEW golden (scoped_handover_response promoted): 12 of 15 gates pass — every wire instrument beats the new reference — and the de-biased consumer misses by 4 mm while one pivot edge regresses.  KEEP_OFF stands.**

⚠ **§15.10 reports iteration 3.  The first item below is DONE and it inverted the A-comparison: per-hour-de-biased, B2 is 47 % BETTER than the scope-OFF arm, not worse — but cbscope5's advantage SURVIVES de-biasing, so the §15.8 gate failures stand.**

* **A datum-invariant consumer instrument.**  Re-grade every arm on a quantity the
  station's own coordinate bias cannot enter — the between-satellite emitted-bias
  consistency, or the consumer's ambiguity-fixing / residual channel — and re-run the
  §15.6 and §15.8 verdicts against it.  Until that exists, no consumer gate on this bed
  distinguishes a card from a realization.
* **The COVERAGE cost of a usable-carry host.**  The pass by construction elects a
  later-ranked station (60 rank changes here).  Measure the elected successors' coverage
  rank distribution and test the cheapest restraint: accept a later-ranked candidate only
  while its coverage is within k of the top-ranked one's (k a knob, 0 = today), so the
  card buys carry continuity without ever moving the datum to a thin host.  Gate: the
  producer channels of B2 kept, and the consumer within the A-arm's own spread.

### 15.10 ITERATION 3 — the DATUM-INVARIANT consumer census: the raw median INVERTS the ranking, and the wire instruments are the ones that move with the cold count (no runs; `scripts/ev_consumer_invariant_grade.py` over the four existing storm arms)

§15.8 ended on a suspicion: the consumer gates were read against `cbscope5`, whose
settled mean EAST error is +3.8 cm while the scope-OFF baseline and BOTH candidacy
arms sit at +7.7…+7.9 cm.  Iteration 3 is the instrument that settles it — no new
runs, the four arms already on disk (A = `mwevid`, C5 = `cbscope5`, B = `coldfb`,
B2 = `coldfb2`), graded on quantities a static station-coordinate bias cannot enter.

**The instruments** (all over the settled window h ≥ 3 unless stated):

* **bias** — the settled mean ENU and its magnitude, reported so the confound is
  visible instead of hiding inside every other number.
* **debias / debiasH** — `|error − the arm's OWN settled mean ENU|`, p50 and rms;
  and the same with a **per-hour** mean removed, so a slowly MOVING bias cannot
  hide inside the static removal.  If the two disagree, the bias is drifting.
* **consumer re-convergence** — from the consumer's own `logs/rt_engine.log`: SSR
  phase-bias resets, code-bias discontinuities, clock discontinuities.  This is
  what a producer datum re-realization COSTS the user, and it is position-free.
* **shocks** — epochs after the first hour with 3D error > 0.5 m.  An 8 cm bias
  cannot manufacture one.
* **wire** — the emitted-bias step series from `products.txt`: every (sat, signal)
  phase bias against the previous emitted epoch, summarised as the fraction of
  epochs carrying ANY |step| > 0.05 m and the p90 of the per-epoch maximum.  A
  CONTINUOUS handover is an S-transform and makes no step; a COLD one does.

| arm | cold handovers | Ē | N̄ | Ū | \|bias\| | raw p50 / rms | **debias p50 / rms** | **debiasH p50 / rms** |
|---|---|---|---|---|---|---|---|---|
| A (scope OFF) | 68 | +0.0771 | +0.0309 | −0.1615 | 0.1816 | 0.1741 / 0.365 | 0.1323 / 0.317 | **0.1532 / 0.309** |
| C5 (cbscope5) | 78 | +0.0381 | −0.0255 | −0.0437 | **0.0633** | **0.1204 / 0.222** | **0.0646 / 0.212** | **0.0521 / 0.206** |
| B (iter 1) | 46 | +0.0776 | −0.0295 | −0.0072 | 0.0833 | 0.1794 / 0.282 | 0.1363 / 0.270 | **0.0943 / 0.256** |
| B2 (iter 2) | 46 | +0.0786 | −0.0452 | −0.0411 | 0.0996 | 0.1941 / 0.309 | 0.1150 / 0.293 | **0.0815 / 0.284** |

| arm | PB resets | CB discs | clock discs | shocks (> 0.5 m, after h1) | AR | wire: frac epochs \|step\| > 5 cm | wire: p90 max step |
|---|---|---|---|---|---|---|---|
| A | 1867 | 599 | 40 | 46 | never ran | 0.3708 | 0.5280 m |
| C5 | **1938** | 597 | 39 | **13** | never ran | 0.3694 | 0.4000 m |
| B | **1772** | 593 | 39 | 21 | never ran | **0.3083** | **0.2682 m** |
| B2 | 1794 | 593 | 39 | 26 | never ran | **0.3056** | **0.2698 m** |

Hourly (raw p50 / per-hour-de-biased p50 / PB resets / shocks):

| arm | h0 | h1 | h2 | h3 | h4 | h5 |
|---|---|---|---|---|---|---|
| A | .447/.317/905/45 | .247/.097/3/0 | .229/.032/8/0 | .174/.054/0/0 | .086/.174/468/24 | .280/.238/483/22 |
| C5 | .524/.284/962/63 | .113/.056/0/0 | .114/.023/32/0 | .101/.031/0/0 | .090/.047/450/7 | .236/.217/494/6 |
| B | .610/.302/953/83 | .263/.052/0/0 | .240/.035/0/0 | .186/.040/0/0 | .109/.098/432/10 | .204/.205/387/11 |
| B2 | .610/.302/953/83 | .264/.054/0/0 | .243/.033/0/0 | .189/.040/0/0 | .110/.080/444/13 | .232/.239/397/13 |

**1. The raw pooled median INVERTS the ranking against the OFF baseline.**  Raw, the
order is A 0.1741 < B 0.1794 < B2 0.1941 — the card "costs" the user.  With each
arm's own per-hour mean removed the order is **B2 0.0815 < B 0.0943 < A 0.1532** —
the card is 47 % BETTER than OFF and iteration 2 better than iteration 1.  The two
readings differ because A carries a −16 cm vertical bias that the raw median
charges to nobody and the de-biased one removes.  Everything §15.6 and §15.8 said
about "the consumer" on the A-vs-card comparison was reading that.

**2. The `cbscope5` advantage SURVIVES de-biasing — plainly, and it is not the
bias.**  C5 is best of the four on every position instrument after removal (debias
p50 0.0646, debiasH 0.0521, rms 0.206–0.212).  Its favourable centring is real but
secondary: it is also genuinely the quietest arm within the hour.  So the §15.8
gate failures are NOT an artifact of C5's centring, and the honest statement is
that **B2 is better than A and worse than C5 on the position channel, both before
and after de-biasing.**

**3. The instruments that move with the cold-handover count are the WIRE ones.**
PB resets are monotone in the cold count — 78 colds → 1938 resets (C5), 68 → 1867
(A), 46 → 1772 / 1794 (B / B2) — so **B2 beats BOTH A and C5 on the consumer's own
re-convergence work**, which is the channel a cold gauge re-realization actually
acts through.  The emitted-step instrument separates card from no-card sharply:
31 % of epochs carry a > 5 cm step on B/B2 against 37 % on A and C5, and the p90
per-epoch maximum step falls 0.528 / 0.400 m → **0.268 / 0.270 m**.  That is the
card doing exactly what it was built to do, measured on the product rather than on
a station's position.

**4. The instruments that do NOT move with the cold count**: code-bias
discontinuities (599 / 597 / 593 / 593) and clock discontinuities (40 / 39 / 39 /
39) are flat across all four arms — the cold PHASE-anchor handover does not touch
them, which is a useful negative control.  Shocks are *anti*-correlated (C5 has the
most colds and the fewest shocks, 13 against B2's 26 and A's 46) and live in h0 and
h4–h5, i.e. in ignition and the §19 pivot-departure window, not in the handover
population at all.  De-biased position sits with C5, not with the cold count.

**5. What could not be computed: the AR channel and therefore the wrong-fix rate.**
`closed_loop.ar_mode = 0` on this lineage, so `ar_ran` is 0 on all 720 epochs of all
four arms, `ar_gate` reads `not_run` everywhere, `p_succ` is 0 and
`logs/ambiguity_resolver.log` is empty; there is no `ambiguity_trace.csv` for the
consumer.  The consumer is FLOAT-ONLY by configuration, so there are no fixes to
grade and no wrong fixes to count.  Re-grading that channel needs an arm with
`closed_loop.ar_mode` armed — a new run, not a new instrument.

**Verdict.**  `network.phase_datum_candidacy_usable` **stays KEEP_OFF.**  The
promotion condition was a datum-invariant channel where B2 beats BOTH A and C5 AND
no invariant channel worse.  The first half is met — PB resets (1794 vs 1867 / 1938)
and both wire-step instruments (0.306 vs 0.371 / 0.369; 0.270 m vs 0.528 / 0.400 m)
— and the second half is not: de-biased position p50 and rms and the shock count are
all worse than C5.  What iteration 3 changes is the CHARACTER of the open question.
It is no longer "does the card cost the user 49 %" (it does not; that number was a
vertical bias on the OFF arm) but "the card makes the WIRE quieter and the position
channel still prefers `cbscope5`", and the next thing to measure is why — the
candidate is the §19 pivot-departure window, where every arm's shocks and half its
PB resets live and where the cold-handover population is not.

### 15.11 ITERATION 4 — the card on the NEW golden, graded on iteration 3's invariant instruments: the wire gates pass by wide margins, the de-biased consumer misses by 4 mm, and one pivot edge regresses (2026-09-11; `runs/golden_sim_campaign_zwdblk_coldfb3`, 720/720, exit 0, 1361 s)

The scoped-handover promotion (§19.5) moved the reference: `S =
runs/golden_sim_campaign_zwdblk_scoped` is the new golden's posture on the campaign
bed, and its consumer is a different animal from cbscope5's (settled p50 0.1204 →
0.0825, shocks 13 → 0).  Iteration 4 re-asks §15.9's question against THAT baseline,
on the datum-invariant instruments §15.10 built.

**The arm.**  `config/test_suite/_golden_sim_campaign_zwdblk_coldfb3.json` = the
tracked `_golden_sim_campaign_zwdblk_scoped.json` + `network.phase_datum_candidacy_usable:
true` (the satellite clause is the code default beneath it) + the run path.  Nothing
else; the flattened key-by-key diff is exactly those two lines.  Exe
`gnss_processor_coldfb3.exe` = `1B37E94B…`, built from main `e90086f`; the reference
was run on `F4FF8EF0…`.  **Those two exes differ only in the posture table** — a
log-only reporter (`logNetworkPosture_` reads already-parsed state and changes
nothing) — and in the cold-fallback code, which is default-OFF and was certified
byte-identical OFF at §15.3.  The reference is therefore a valid same-code baseline,
and it is stated here rather than assumed.

| channel | **S (scoped)** | **B3 (S + candidacy)** | gate | verdict |
|---|---|---|---|---|
| **per-hour-de-biased consumer p50 / rms** | **0.0347 / 0.0466** | **0.0387 / 0.0516** | ≤ S | **FAIL** (+4.0 / +5.0 mm) |
| statically de-biased p50 / rms | 0.0563 / 0.0688 | 0.0815 / 0.0890 | (reported) | worse |
| raw settled p50 / rms | 0.0825 / 0.0899 | 0.1111 / 0.1348 | (reported) | worse |
| settled mean ENU / \|bias\| | +0.0335 / −0.0104 / −0.0460, 0.0578 | +0.0830 / −0.0334 / −0.0473, 0.1012 | (reported) | the east bias returns to the candidacy arms' +0.08 |
| **consumer PB resets** | 1475 | **1337** | < S | **PASS** (−9.4 %) |
| CB discs / clock discs | 517 / 21 | 516 / 21 | (control) | flat |
| shocks (> 0.5 m after h1) | 0 | **0** | | tie, both clean |
| **wire: frac epochs \|step\| > 5 cm** | 0.3611 | **0.2944** | ≤ S | **PASS** (−18 %) |
| **wire: p90 per-epoch max step** | 0.4000 m | **0.2651 m** | ≤ S | **PASS** (−34 %) |
| **PHASE_ANCHOR handovers / cold** | 315 / 78 | 312 / **46** | settled ≤ 19 | **PASS** |
| ...cold-start / settled | 27 / 51 | **27 / 19** | | −63 % settled |
| ...cold by hour h0…h5 | 41 12 7 0 5 13 | **38 8 0 0 0 0** | | zero from hour 2 |
| PB-GAUGE elected / continuous / cold | 462 / 362 / 100 | 468 / 416 / **52** | | |
| continuous successor age p10 / p50 / p90 | 91 / 240 / 459 | **116 / 236 / 478** | | older |
| candidacy: entered / elected / rank-changed / fell through | — | 468 / 399 / **60** / 69 | | |
| satellite clause: sat-ep / families / **splits avoided** / cold-coloc / untracked | — | 133 / 399 / **13** / 0 / 0 | | |
| **split-referenced mean / max** | 0.0 / 0 | **0.0 / 0** | 0.0 / 0 | **PASS** |
| **ZWD settled truth rms / span p50** | 0.0066 / 0.0205 | **0.0060 / 0.0178** | ≤ 0.0066 | **PASS** |
| **ref-free WL / EWL / L1C** | 99.2 / 100 / 90.2 | **99.3 / 100 / 91.0** | ≥ 99.2 / ≥ 90.2 | **PASS** |
| **T4 events / ambient** | 11 / 0.206 | **11 / 0.198** | ≤ 11 | **PASS** |
| **W2 4.9917 h: T4 / decouples / CLK breaks** | 0/0/0 | **0/0/0** | absent | **PASS** |
| **sat-clock datum-free contrast rms** | 0.7657 | 0.7713 (**+0.73 %**) | ±3 % | **PASS** |
| whole-run residual rms | 1.0096 | 1.0144 (+0.48 %) | | |
| **consumer 3D at the GPS pivot edge (319260)** | 0.126 | **0.104** | ≤ 0.126 | **PASS** |
| **consumer 3D at the Galileo pivot edge (321900)** | 0.031 | **0.108** | ≤ 0.031 | **FAIL** (+77 mm) |
| consumer hourly p50 h0…h5 | .524 .113 .114 .101 .089 .047 | .610 .264 .243 .189 .102 .088 | | |
| per-hour-de-biased hourly p50 | .284 .056 .023 .031 .035 .046 | .302 .054 .033 .040 **.033 .040** | | h1/h4/h5 equal-or-better, h0/h2/h3 worse |
| PB resets by hour | 962 0 32 0 256 225 | 953 0 **0 0 232 152** | | better in h2, h4, h5 |
| run time | 1242 s | 1361 s | | +10 % |

**Verdict: 12 of 15 gates pass, 3 fail, and the knob stays KEEP_OFF.**  The three
failures are two instruments: the per-hour-de-biased consumer (p50 +4.0 mm, rms
+5.0 mm — a miss, not a collapse) and the Galileo pivot edge (0.031 → 0.108 m).

**What iteration 4 settles, and it is not nothing:**

1. **On the WIRE the card is now unambiguously better than the best arm this bed
   has.**  It beats S on every wire instrument at once — PB resets 1475 → 1337,
   the fraction of epochs carrying a > 5 cm emitted step 0.361 → 0.294, and the p90
   per-epoch maximum step 0.400 → 0.265 m — while removing 32 settled cold
   handovers and every settled one after hour 1.  That is the third independent
   arm on which the wire instruments move with the cold-handover count.
2. **The producer's truth-graded channels are all better**: ZWD 0.0066 → 0.0060 (the
   best on this bed), ref-free L1C 90.2 → 91.0 (likewise), WL 99.2 → 99.3, T4 equal,
   contrasts +0.73 %.
3. **And the consumer still does not follow.**  Its per-hour-de-biased median is 4 mm
   worse than S's with the wire measurably quieter — the same shape §15.10 found, now
   against a reference that itself fixed the largest consumer defect on the bed.  The
   settled mean east error returns to +0.083 m (the value every candidacy arm carries,
   and the scope-OFF arm too) from S's +0.034, so a third of B3's raw gap to S is
   again a static horizontal realization the card should not be able to control —
   yet the de-biased gap survives, exactly as C5's did.
4. **The Galileo edge is the one real regression**, and it is worth naming precisely:
   at 321900 the scoped response alone reaches 0.031 m and the card takes it to
   0.108 m — still **7× better than the pre-scoped 0.777 m**, but a step back from
   the reference.  The GPS edge moves the other way (0.126 → 0.104).  Both edges are
   epochs where the candidacy pass is re-choosing hosts while a clock datum moves;
   the interaction of the two mechanisms at a pivot edge is untested and is the
   obvious next thing to look at.

**Iteration 5, pre-registered.**  The open question is now narrow and local: *why does
a quieter wire leave a noisier consumer, and what happens at a pivot edge when the
candidacy pass and the scoped handover act in the same epoch?*  Two steps, in order:
(i) a per-edge census of the anchor set across 321900 in S and B3 — which families
moved, whether any moved BECAUSE of the candidacy pass, and whether the consumer's
resets at that epoch are the ones the card added; (ii) only then a knob, if the census
names one: an interlock that defers a candidacy-driven handover by one epoch when the
same epoch carries a clock-datum change, gated on the Galileo edge returning to ≤ 0.031
with the wire instruments kept.

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

## 14.10 The IF-TD detector watched ONE lane per satellite — `network.cycle_slip.if_phase_td_pairs` (2026-09-11)

### 14.10.1 The defect

The common-band scope's IF-TD VETO (§14.2) is what closes the EQUAL-COUNT trap:
two bands slipping by the same integer leave their Melbourne-Wübbena lane silent
while the iono-free combination moves 0.107 m per cycle, so a silent MW pair with
no IF-TD pair behind it exonerates two bands that both moved.  But
`IfPhaseSlipDetector` forms **exactly one pair per satellite** — the first two
distinct phase bands it finds (L1C+L2W on GPS, L1C+L5Q on Galileo) — so the veto
reaches one lane of three and the trap is open on every other one.

Measured on the storm bed, station **S37000SIM / G20 at 4.99 h**: truth L5Q −31
cycles (receiver-flagged) at 4.98 h and L1C −29 cycles at 4.99 h.  The (L1C+L5Q)
lane moved by 2 wide-lane cycles and stayed silent, and the verdict read

```
keep implicated=L2W; exonerated=L1C;L5Q;
basis=mw(L1C+L5Q)silent(n=99;thr=9.0WL);mw(L1C+L2W)flag;
      mw(L2W+L5Q)flag;iftd(L1C+L2W)=flagged
```

— it freed the one band that had **not** slipped, the epoch's misfit was metres,
and the user paid +9.5 cm for the following hour.

### 14.10.2 The knob

`network.cycle_slip.if_phase_td_pairs` = `first` (default, byte-identical) |
`all`.  Under `all` the detector evaluates **every distinct pair** of a
satellite's tracked phase bands (3 on a triple-band arc, 1 on a dual-band one).
Each pair rides its own GRADIENT CHAIN: `constellation_state_` is keyed by the
bare system string for a satellite's FIRST pair — so that pair's population,
double differences and log lines are the pre-knob ones byte for byte — and by
`"<system>|<sig1>_<sig2>"` for every additional pair.  The driver's pools
(centre / scale / shrinkage) were already per pair KIND, and the verdict's
read-only preview reads the same source, so the extra pairs are a pure addition
on both sides.

A flag resets **that chain only** (`resetSatelliteInState`); the whole-satellite
`resetSatellite` would discard the other pairs' clean gradient histories and make
one pair's decision depend on another's.  `T1_if_slip` rows carry a
`pair=L1C+L5Q` token under `all` (omitted under `first`, so those rows stay the
pre-knob text), reset keys are per pair (`G01_L1C_L5Q`), and the run-end clause
reports evaluated / flagged / refloated per pair kind, knob-independently.

### 14.10.3 ⚠ 14.10.4 first — ONE reference arm per (station, constellation)

Running the reference-satellite slip test (tier T1r) per CHAIN is **not** a pure
addition: it is three tests on three different iono-free combinations of the same
arcs, and the extra-wide ones are ill-conditioned.  (L2W,L5Q) has IF coefficients
12.26 / −11.26 — noise amplification 16.6 against 2.98 on (L1C,L2W) — while its
λ_IF is 0.1248 m, so the legacy candidate condition |median(non-ref ΔΔ)| >
0.5·λ_IF is met by NOISE.  Measured on a 70-epoch quiet 60-station null before
this clause:

| quantity | `first` | `all`, per-chain ref arm | `all`, shared ref arm |
|---|---|---|---|
| IF-TD ref-gate legacy candidates | 25 | **5 351** | **25** |
| …of which FIRED | 1 | **64** (52 marked incoherent STATION EVENTS) | **1** |
| pooled-arm flags | 0 | 0 | 0 |
| products / solution / ZWD / DD-frac vs `first` | — | all DIFFER | **all IDENTICAL** |

A false-alarm cost entirely invisible in the per-pair evaluated/flagged tally, and
the reason the first `all` arms were not byte-identical to `first` on a quiet bed.
Additional chains now ADOPT the first chain's reference (so every pair of a
satellite sits in one between-satellite frame) and skip its test; the fallback to
their own election covers only a satellite set that does not contain that
reference at all.

### 14.10.4 Harness legs (quiet seed ep0665, S21000SIM / G08, event at +10, 70 epochs)

Arms: **OFF** (`common_band_scope` false, `if_phase_td_pairs` first), **ON**
(true, first), **FIX** (true, all).  Arms of a leg differ in exactly those two
knobs (verified by config diff).  G08's arc at the seed is 367 epochs old, but the
MW detector's state is not in the snapshot, so the in-leg maturity reads n = 10 at
the event — exactly the `common_band_min_arc_epochs` bar, which is what makes
these legs reproduce the field shape at all.

| leg | event(s) | ON verdict | FIX verdict |
|---|---|---|---|
| **S1** storm shape | L5Q −31 **with LLI** @+10, L1C −29 @+11 | +1 `keep implicated=L2W` — **the field defect, verbatim** | +1 `keep implicated=L2W` — **all-pairs alone does NOT close it** |
| **S2** pure equal-count | L1C −29 and L5Q −29 both @+10, no flag | +0 `keep implicated=L2W` — wrong | +0 `all reason=no_exoneration`, `iftd(L1C+L5Q)=flagged` — **veto fires** |
| **L2** single-band control | L2W −33 @+10, no flag | +0 `keep implicated=L2W` | identical verdict; basis gains `iftd(L1C+L5Q)silent` |
| **S3** LLI only | L5Q −31 with LLI @+10 | +1 `keep implicated=L5Q` — the CORRECT late narrowing | identical verdict |

Per-band DD level residual of the injected arc (event − null, cycles; the
`sig` band has its injected integer removed, the others do not — read across
arms):

| leg / arm | L2_d@+1 | L5_d@+1 | L2_d@+45 | L5_d@+45 | oth_d rms +14..45 | ZWD_d peak | iono_d peak | T4 rows (ids) |
|---|---|---|---|---|---|---|---|---|
| S1 OFF | −2.214 | −2.280 | −0.367 | −0.378 | 0.0214 | 0.0026 m | −0.331 m | 0 |
| **S1 ON (pre-F2, the defect)** | **+13.822** | **+9.078** | +0.290 | +0.289 | **0.0668** | **0.729 m** | 0.523 m | **15 (15, all at S21000SIM)** |
| S1 FIX (pre-F2) | +5.935 | +4.434 | +0.236 | +0.251 | 0.0215 | 0.237 m | 0.470 m | **0** |
| S1 ON / FIX with F2 (§14.11) | −2.214 / −2.228 | −2.280 / −2.294 | −0.367 | −0.378 | 0.0214 / 0.0192 | 0.0026 m | −0.330 m | 0 |
| S2 OFF | −0.587 | −29.584 | −0.326 | −29.335 | 0.0193 | 0.0027 m | 0.143 m | 0 |
| **S2 ON (the defect)** | −2.428 | −31.484 | −0.256 | −29.264 | 0.0459 | 0.247 m | **3.846 m** | 0 |
| **S2 FIX** | −0.587 | −29.584 | −0.326 | −29.335 | 0.0193 | 0.0027 m | 0.143 m | 0 |
| L2 OFF | −0.587 | −0.584 | −0.326 | −0.335 | 0.0193 | 0.0027 m | 0.143 m | 0 |
| L2 ON | −0.031 | −0.002 | −0.013 | −0.008 | 0.00014 | 0.0003 m | −0.003 m | 0 |
| **L2 FIX** | −0.031 | −0.002 | −0.013 | −0.008 | 0.00014 | 0.0003 m | −0.003 m | 0 |

Readings: **S2 FIX is OFF digit for digit** — the veto returns the leg to today's
response.  **L2 FIX is ON digit for digit** on every channel, and products /
solution / ZWD / DD-fractional are byte-identical between them, so the widening is
exactly inert on a genuine single-band slip.  **S1 ON pre-F2 is what the defect
costs**: 13.8 and 9.1 cycles of level error at +1, ZWD 0.73 m, the station's other
arcs 3.1× worse, and the post-fit tier erasing **15 innocent arcs at that one
station**.  All-pairs alone removes all 15 erases and halves the level error (the
new pairs catch the L5Q slip at +0), but it does not remove the wrong narrowing:
a flag resets its own chain, so at +1 — the epoch of the second slip and of the
verdict — the (1,5) and (2,5) pairs have no previous gradient, produce no sample,
and cannot veto.  That is §14.11.

**⚠ The one measured COST of `all`: S3.**  With the widening the S3 leg's level
residual goes 0.005 → 0.335 cyc at +45 and its slant-iono peak 0.004 → 0.143 m,
i.e. from near-perfect to the §9 all-band floor, *with the +1 verdict unchanged at
`keep implicated=L5Q`*.  Mechanism: at the LLI epoch itself **no verdict exists** —
every MW sample is withheld (§14.11), so `flagged_pairs` is empty and the rule
returns FALLBACK 1, and the drains run UNSCOPED.  Under `first` that was harmless
because the only actor there is the LLI drain, which is already band-scoped by the
receiver's own bits (it freed L5Q alone).  Under `all` the (1,5) and (2,5) IF-TD
pairs also flag at +0 and, unscoped, free both of their bands each — all three.
**Named fix, not built here: form a verdict at an LLI epoch too** (the LLI bits are
themselves evidence: implicated = flagged bands, provided a silent mature
un-shadowed lane free of flagged bands exonerates the rest), so FALLBACK 1 stops
meaning "unscoped" on the one epoch class where a receiver flag has already named
the band.

## 14.11 RULE F2 — the LLI shadow: a lane that did not see the flagged epoch is not silent

### 14.11.1 The mechanism, from the detector's own decision lines

`MwSlipDetector::detect`'s uncombined branch, on any epoch where a satellite
carries a receiver loss-of-lock bit, schedules the ambiguity resets and
`continue`s — **deliberately preserving the MW arc** (the MW combination is a
function of raw observations alone, so a short lock-loss does not invalidate the
EWMA).  So the arc count runs straight through the flagged epoch and the first
sample after it reads MATURE while the lane saw nothing at the epoch the receiver
declared broken — and a band-common slip does not move the wide lane in any case.

Storm-bed census: all five spurious `keep` verdicts in 2 994 sit exactly one epoch
after a receiver-flagged slip on the same arc, and in every one the exonerating
lane CONTAINED the flagged band (S37/G20 (L1C+L5Q) n 87 → 99 across the LLI, thr
9.0 WL; S25/E36 (L1C+L5Q) n 164 → 173).

Reproduced on the S1 leg, from the new `MWDEC` / `MWSKIP` lines
(`runs/evleg_m_s1_fix`, S21000SIM / G08):

```
  -1 L1C+L2W     dev  +0.5668 m (+0.658 WL)  thr 3.4777 (4.035 WL)  n= 9  flag=0  shadow=0
  +0 ALL PAIRS WITHHELD (LLI on L5Q)          <- no sample evaluated; n does NOT advance
  +1 L1C+L2W     dev -24.5936 m (-28.534 WL)  thr 3.5786 (4.152 WL)  n=10  flag=1  shadow=0
  +1 L1C+L5Q     dev  +1.6025 m ( +2.133 WL)  thr 3.1228 (4.156 WL)  n=10  flag=0  shadow=1
  +1 L2W+L5Q     dev +181.82 m (+31.023 WL)   thr 2.0700 (0.353 WL)  n=10  flag=1  shadow=1
  +2 L1C+L5Q     dev  +1.1801 m ( +1.571 WL)  thr 4.8701 (6.481 WL)  n=11  flag=0  shadow=0
```

The answer to "does the detector evaluate the LLI epoch's sample or withhold it":
**it withholds it, for the whole satellite, and keeps the arc** — so the next
epoch's sample is compared against the pre-LLI mean and reads the COMBINED jump.
Line 4 is the damage: +2.13 WL against a 4.16 WL gate, silent and "mature", and
it exonerated L1C and L5Q.  The S3 leg's +1 shows the other half — `L1C+L2W dev
+0.466 WL, shadow=0` next to `L1C+L5Q +31.133 WL flag=1` and `L2W+L5Q +31.023 WL
flag=1` — the lane carrying no flagged band, which is why the correct late
narrowing survives F2.

### 14.11.2 The rule

Gated by `network.cycle_slip.common_band_scope`; no new knob, OFF stays
byte-identical.  **A MW pair is NOT silent — it is refused exactly as an immature
pair is — at any epoch where one of its BANDS carried an LLI at the previous epoch
on that (station, satellite).**  Exported by the detector itself:
`EpochPairStatus::lli_shadow`, from a one-epoch per-satellite band memory
(`prev_lli_bands_`, rotated at the top of `detect`), so no run-context fallback is
needed and the (station) scope is the per-station detector instance.  Band-scoped
on purpose: a lane made only of unflagged bands is untouched by the receiver's
report and may still exonerate.  Basis token `mw(L1C+L5Q)lli_shadow(n=99)`,
counter `cb_lli_shadow`, run-end clause extended.

| leg | with F2 |
|---|---|
| **S1 ON** | +1 `all reason=no_exoneration; basis=mw(L1C+L5Q)lli_shadow(n=10);…` — **F2 alone closes the storm shape**; levels, ZWD, other arcs and T4 all return to the OFF baseline |
| **S1 FIX** | same verdict; the +0 IF-TD catches remain |
| **S2** | unchanged (no LLI on that leg) — the two mechanisms are complementary, each closing its own shape |
| **L2** | unchanged `keep implicated=L2W`, level residual 0.008–0.013 cyc |
| **S3** | unchanged `keep implicated=L5Q` at +1 — the 853-of-858 correct late narrowing is not costed |

### 14.11.3 Detector readout

Two DEBUG lines in `MwSlipDetector` (silent at the shipped level; an emission
cannot change a decision, and products are byte-identical):

- `MWDEC sta=… <sat>_<sig1>_<sig2> t=… dev=… thr=… n=… lam_wl=… flag=… lli=… lli_shadow=…`
  at the flag decision, for **every evaluated pair** — the per-sample record was
  previously readable only through a verdict, and a verdict exists only on epochs
  where some pair FIRED, so the silent lane either side of a slip was unreadable;
- `MWSKIP sta=… <sat> t=… lli_bands=[…] — EVERY pair's sample WITHHELD this epoch`
  in the LLI branch, so a missing MWDEC line reads as a withheld sample rather
  than as missing data.

`setStationLabel` because all 60 stations share the one `mw_slip_detector` logger
and its records are keyed only by `<sat>_<sig1>_<sig2>` — without the label the
log cannot be read per station at all.  Arm with
`logging.enable_mw_slip_detector: true` + `logging.levels.mw_slip_detector:
"debug"` (the §13.4 recipe) and read the rotated `mw_slip_detector.N.log` files
too.  ⚠ The sink is 4 × 5 MB and a 60-station leg writes ~15 MB per 25 epochs, so
a 70-epoch leg rotates its own event out: use a **25-epoch leg with `--at-ep 10`**
when the log is the instrument (`runs/evleg_m_s1_*`, `m_s3_*`).

### 14.11.4 Gates

| gate | result |
|---|---|
| **G1** both knobs at their defaults, vs main's own Release exe (`420634f`) | **PASS** on the L2 control leg and on the quiet 240-epoch null: products, solution, events, ZWD, DD-fractional, rx-states and the consumer trace all byte-identical |
| `all` vs `first`, quiet 70-epoch null | products / solution / ZWD / DD-fractional **byte-identical**; `network_events.csv` differs only by the new rows and `pair=` tokens |
| `all` vs `first`, L2 single-band control | same four artifacts **byte-identical** |
| `all` vs `first`, S1 / S3 | differ, as they must — the new pairs catch the L5 slip at +0 |
| unit suite | 1892 cases / 259 suites, 1875 passed, 17 fixture-gated skips, 0 failures (+22 over main's 1870) |

Run-end clause from a FIX leg (`runs/evleg_r_s1_fix`):

```
Network (common-band scope): verdicts 480, narrowed 3 (bands kept 6), all-band 477
  (no exoneration 471 / inconsistent 6 / anchor co-locate 0), IF-TD vetoes 0,
  immature pairs 720, LLI shadows 1, weak silences 3; split-referenced satellites
  mean 3.4 / max 5 of 52.3 anchored [knob on].
Network (IF-TD pairs [all]): 5 pair kind(s), eval 202014 / flag 3 / refloat 3 total —
  L1C_L2W eval 39813 / flag 1 / refloat 1, L1C_L5Q eval 67338 / flag 1 / refloat 1,
  L1C_L7Q eval 27525 / flag 0 / refloat 0, L2W_L5Q eval 39813 / flag 1 / refloat 1,
  L5Q_L7Q eval 27525 / flag 0 / refloat 0.
```

Both knobs stay default (`first` / OFF).  Owed before any promotion: the storm-bed
arm, and the §14.10.4 LLI-epoch verdict named above.

## 14.12 RULE F3 — the LLI-EPOCH verdict: the epoch the rule could not see

### 14.12.1 The gap §14.10.4 found

At an epoch where the receiver flags a band, `MwSlipDetector::detect`'s
uncombined branch withholds **every** MW sample of that satellite (§14.11).  So
`flagged_pairs` is empty, the §14.2 rule returns FALLBACK 1 — "no MW pair fired,
not a verdict at all" — and **the drains run UNSCOPED**.

That was harmless for as long as the LLI drain was the only actor at such an
epoch: it frees exactly the bands the receiver flagged, i.e. it is already
band-scoped by its own evidence.  It stops being harmless the moment a SIBLING
drain can fire in the same epoch.  Under `if_phase_td_pairs = all` the IF-TD
pairs that contain the flagged band do fire there — the IF-TD detector does not
skip LLI epochs — and unscoped, each frees BOTH of its bands.  On a triple-band
arc with an L5 flag that is (1,5) ∪ (2,5) = all three, so every LLI epoch becomes
an all-band free.  Measured on the §14.10 **S3** leg (L5Q −31 with the flag, the
LLI path alone):

| S3 leg | level residual @+45 | slant-iono peak | +1 verdict |
|---|---|---|---|
| `first` (only the LLI drain acts) | **0.005 cyc** | 0.004 m | `keep implicated=L5Q` |
| `all`, no F3 | **0.335 cyc** (the §9 all-band floor) | 0.143 m | `keep implicated=L5Q`, unchanged |

The verdict at +1 was right the whole time; the damage was done at +0, where
there was no verdict to obey.

### 14.12.2 The rule

Gated by `network.cycle_slip.common_band_scope`; no new knob, OFF stays
byte-identical.  When LLI bits are set for a (station, satellite) at this epoch
**and the normal path formed no verdict** (no MW pair flagged — on the uncombined
path, all withheld), form one:

- `action = keep`, `implicated = lli_signals ∩ live_bands`;
- **no exoneration step.**  The receiver's bits ARE the scope — today's LLI drain
  already trusts them that far, and there is no MW evidence at this epoch to
  weigh anything against.  `exonerated` stays empty so the `bands_kept` census
  keeps meaning "spared by positive exoneration";
- basis `lli=L5Q;mw=withheld`, outcome @ref CommonBandOutcome::LliOnly, counters
  `cb_lli_verdicts` / `cb_lli_narrowed` (knob-independent), run-end clause
  extended;
- every drain at that epoch — the LLI free and every IF-TD pair refloat — then
  frees (its bands ∩ implicated), through the one existing hook, exactly as the
  drains already do under a verdict.

**If any MW pair DID flag at an LLI epoch the normal path runs, unchanged**: F3's
guard is `flagged_pairs.empty()`, and the §14.2 rule already implicates every
band carrying a loss-of-lock bit, so there is nothing for F3 to add there.

Two degenerate cases are explicit: a flagged band that is not tracked leaves
`implicated = live_bands` and `narrowed = false`, so a caller cannot restrict a
drain by accident; and every live band flagged is today's response, also
`narrowed = false`.

**⚠ Under `first` + scope ON this also scopes the (L1C+L2W) IF-TD drain** when the
LLI is on L1C or L2W — today that drain frees both of its bands at an LLI epoch.
F3 is therefore not an `all`-only rule, and its first effect on the existing ON
posture is a narrowing the ON lineage has never run.

**Deliberately NOT applied: the §14.5 ANCHOR CLAUSE.**  An F3 verdict is not
refused on an arc that anchors a family of the satellite, because the LLI drain
ALREADY frees exactly the flagged bands there today — so F3 changes only the
sibling drains, and refusing to scope them would restore the very all-band free
the rule exists to remove.  Whether that trade costs reference co-location is
measurable rather than assumed: the always-on split-referenced-satellite census
(§14.5) reads it on every arm.

### 14.12.3 Legs and gates

See §14.12.4 for the measured table.  Pre-registered, same seed / station /
satellite / arms as §14.10.4:

| leg | pre-registered |
|---|---|
| **S3 FIX** | +0 `keep implicated=L5Q` with the IF-TD refloat rows scoped to L5Q; +1 `keep implicated=L5Q` as before; level residual @+45 and iono peak equal to the `first` values (0.005 cyc / 0.004 m) |
| **S1 FIX** | +0 `keep implicated=L5Q`; +1 `all … lli_shadow` unchanged; residual table unchanged from the S1+F2 row |
| **S2 FIX / L2 FIX** | products byte-identical to the pre-F3 `all` runs (`fd217f07…` / `4e0b5606…`) — neither leg has an LLI, so F3 cannot reach them |
| **null 70 / null 240** | `all` ≡ `first` on products / solution / ZWD / DD-fractional; OFF ≡ main's `420634f` exe |

### 14.12.4 RESULT — the gap closes, and `all` becomes a pure addition on every leg

Final exe `gnss_processor_iftdpairs_v6.exe` (SHA-256 `B94E7600D4159…`), same seed /
station / satellite / arms; every knob explicit; all three arms of every leg
rebuilt from FRESH event dicts and their schedules verified before a single run.

| leg | ON (scope, first) | FIX (scope, all) |
|---|---|---|
| **S3** LLI only | +0 `keep rule=lli_epoch;implicated=L5Q;narrowed=1;basis=lli=L5Q;mw=withheld`; +1 `keep implicated=L5Q` | **identical rows**, plus the two scoped drains: `T1_if_slip … pair=L1C+L5Q;cb_kept=L1C` and `… pair=L2W+L5Q;cb_kept=L2W` |
| **S1** storm shape | +0 the same `lli_epoch` row; +1 `all … mw(L1C+L5Q)lli_shadow(n=10)` | identical rows + the same two scoped drains at +0 |
| **S2** equal-count | +0 `keep implicated=L2W` (the defect) | +0 `all reason=no_exoneration`, `iftd(L1C+L5Q)=flagged` |
| **L2** single band | +0 `keep implicated=L2W` | identical verdict |

Per-band DD level residual of the injected arc (event − null, cycles):

| leg / arm | L2_d@+1 | L5_d@+1 | L2_d@+45 | L5_d@+45 | oth_d rms +14..45 | ZWD_d peak | iono_d peak | T4 |
|---|---|---|---|---|---|---|---|---|
| S3 ON | 0.001 | 0.032 | 0.005 | 0.008 | 0.000182 | −0.0006 m | 0.0037 m | 0 |
| **S3 FIX** | **0.001** | **0.032** | **0.005** | **0.008** | **0.000182** | **−0.0006 m** | **0.0037 m** | 0 |
| S1 ON | −2.214 | −2.280 | −0.367 | −0.378 | 0.021447 | −0.0026 m | −0.331 m | 0 |
| **S1 FIX** | **−2.214** | **−2.280** | **−0.367** | **−0.378** | **0.021447** | **−0.0026 m** | **−0.331 m** | 0 |

**S3 FIX is now S3 ON digit for digit** — the §14.12.1 cost (0.335 cyc / 0.143 m)
is gone, and the pre-registered `first` values (0.005 cyc / 0.004 m) are met
exactly.  **S1 FIX is S1 ON digit for digit** too.

Byte-identity:

| comparison | products / solution / ZWD / DD-fractional |
|---|---|
| **OFF vs main's `420634f` exe**, L2 control leg | **IDENTICAL** (events too) |
| **OFF vs main's exe**, quiet 240-epoch null | **IDENTICAL** (events, rx-states and the consumer trace too; products `4c96e34a4aebc4a9`) |
| `all` vs `first`, quiet 70-epoch null | **IDENTICAL** |
| `all` vs `first`, **S3** and **S1** | **IDENTICAL** — with F3 the widening no longer changes the solve on an LLI leg at all |
| `all` vs `first`, L2 control | **IDENTICAL** (`4e0b56068e44a862`) |
| S2 FIX vs the pre-F3 `all` run | **IDENTICAL**, events included (`fd217f072a2eaf98`) — no LLI on that leg, so F3 cannot reach it |

So after F3 the only legs on which `all` changes the solve at all are the ones
where a *new pair catches a slip the first pair could not see* — S2.  That is the
contract the card was aiming at.

Run-end clause (`runs/evleg_t_s3_fix`):

```
Network (common-band scope): verdicts 480, narrowed 4 (bands kept 8), all-band 476
  (no exoneration 470 / inconsistent 6 / anchor co-locate 0), IF-TD vetoes 0,
  immature pairs 720, LLI shadows 0, LLI-epoch verdicts 1 (1 narrowed),
  weak silences 3; split-referenced satellites mean 3.4 / max 5 of 52.3 anchored
  [knob on].
Network (IF-TD pairs [all]): 5 pair kind(s), eval 202015 / flag 2 / refloat 2 total —
  L1C_L2W eval 39814 / flag 0 / refloat 0, L1C_L5Q eval 67338 / flag 1 / refloat 1,
  L1C_L7Q eval 27525 / flag 0 / refloat 0, L2W_L5Q eval 39813 / flag 1 / refloat 1,
  L5Q_L7Q eval 27525 / flag 0 / refloat 0.
```

The **split-referenced satellite census reads 3.4 / max 5 on both arms**, i.e. the
`first` value — so scoping the LLI epoch did not separate a satellite's references,
which is the measurement the deliberate omission of the anchor clause (§14.12.2)
was left open for.

Unit suite 1897 cases / 259 suites, 1880 passed, 17 fixture-gated skips, 0
failures (+27 over main's 1870; F3 adds 5 rule/hook cases to
`tests/test_common_band_scope.cpp`).

⚠ **A harness-builder trap, recorded because it cost a leg.**  `event_leg.py
make-leg` arms ONE event; a multi-event leg patches `leg.json` afterwards.  A
builder that hoists the event list out of the arm loop and then `pop()`s the
per-event offset out of it mutates the shared dicts, so the SECOND arm built gets
a different schedule — here S2's second slip moved from +0 to +1, turning the
equal-count trap into a two-epoch slip and producing a plausible-looking but
different verdict.  Build the schedule fresh per arm, and **print every leg's
`(signal, cycles, lli, tow − t0)` before running any of them** (the batch-6
builder does).

Still owed before any promotion: the storm-bed arm.  Both knobs stay default
(`first` / OFF).

### 14.12.5 The ANCHOR CLAUSE inside F3 — iteration 2's defect, re-entered through the LLI door

The Study-7 storm arm (`runs/golden_sim_campaign_zwdblk_cbscope4`) cleared every
§14.10/§14.11 gate — the 4.99 h fallout gone (0 T4 rows / 0 clock decouples / 0
declared clock breaks at 4.9917), unanswered exonerations 1 → 0, T4 86 → 11, ZWD
0.0071 → 0.0064, L1C ref-free 89.6 → 90.1 %, cold handovers 59 → 51, consumer
pooled p50 0.2067 → 0.1673 (better than OFF's 0.1741) — with ONE defect, and it
was F3's:

| quantity | §14.9 (cbscope3) | cbscope4, F3 without the clause |
|---|---|---|
| split-referenced satellites, mean of 52.8 | 3.5 (6.6 %) | **7.6 (14.4 %)** |
| single-family anchor handovers | 57 | **103** |
| …of the new ones, on an arc carrying an F3 verdict that epoch | — | **49** |
| single-family handovers from the NORMAL path's `keep` rows | 0 | **0** |

§14.12.2 had left the clause off deliberately, on the argument that the LLI drain
already frees exactly the flagged bands so F3 changes only the sibling drains.
That argument is sound for the DRAIN and wrong for the DATUM: narrowing on an arc
that anchors a family still frees only the flagged band's family, so the election
hands THAT family over alone and the satellite's per-frequency references
separate.  The §14.10 S3 leg could not see it — **S21000SIM anchors nothing**,
which is exactly why §14.3 chose it.

**The fix** is the §14.5 clause applied inside F3, identically: if the arc anchors
ANY family of the satellite the LLI-epoch verdict is `all` with reason
`anchor_colocate`, so every family moves to one successor.  Counted in the shared
`cb_anchor_colocate` (which stays the honest total) AND in its own
`cb_lli_anchor_colocate`, because the storm arm needed exactly that split to
attribute the regression to the F3 path.  The clause fires only on a verdict that
WOULD have narrowed, so an LLI on an untracked band is not charged a co-locate it
never earned.  The event row keeps `rule=lli_epoch` on the co-locate row too —
without it the two paths' refusals are indistinguishable in the trace.

### 14.12.6 RESULT — the anchor-arc leg (S36000SIM / G04, L5Q −31 with LLI at +10)

Exe `gnss_processor_iftdpairs_v7.exe` (SHA-256 `35D7DDF8BDFD5…`), quiet seed
ep0665, 70 epochs, knobs explicit.  G04's families anchor at S36000SIM (§14.8).

| arm | +0 verdict | +0 PHASE_ANCHOR handovers | later | split-ref mean / max |
|---|---|---|---|---|
| null (any arm) | — | — | — | **3.4 / 5** |
| **OFF** | (no verdict) | 1 → S60000SIM | +1: 1 → S31000SIM | 4.3 / 6 |
| ON `first`, pre-clause | `keep rule=lli_epoch;implicated=L5Q` | 1 → S60000SIM | +1: 1 → S31000SIM | 4.3 / 6 |
| ON `first`, **clause** | `all rule=lli_epoch;reason=anchor_colocate` | 1 → S60000SIM | +1: 1 → S31000SIM | 4.3 / 6 |
| **FIX `all`, pre-clause** | `keep rule=lli_epoch;implicated=L5Q`, drains scoped (`cb_kept=L1C`, `cb_kept=L2W`) | **1 → S60000SIM** | +1 normal path fires its own clause, 1 → S31000SIM | **4.3 / 6** |
| **FIX `all`, clause** | `all rule=lli_epoch;reason=anchor_colocate`, drains unscoped | **3 → S60000SIM, all one successor** | +1 `keep implicated=L5Q`, no further handover | **3.4 / 5 = the null** |

Both pre-registered predictions hold: pre-clause **one** family hands over and the
split census rises above the null; with the clause **all three** families hand over
to the **same** successor at +10 and the census equals the null's.

**The OFF control is what makes the attribution exact.**  OFF — knob entirely off —
gives the same 1 handover and the same 4.3 / 6 as the ON arm with or without the
clause.  So the single-family handover at an LLI epoch on the `first` lineage is
**today's behaviour**, not a card effect: the LLI drain is band-scoped by the
receiver's own bits and always was.  F3's leak was `all`-SPECIFIC — it scoped the
IF-TD pair drains that would otherwise have freed all bands and co-located the
families — and the clause removes exactly that.  With the clause the `all` arm is
the **only** posture on this leg that keeps a satellite's references together
(3.4 / 5 against OFF's 4.3 / 6).

Regression set, all byte-identical including `network_events.csv`:

| comparison | products / solution / ZWD / DD-frac / events |
|---|---|
| S3 at S21, ON and FIX, vs §14.12.4 | **IDENTICAL** (S21 anchors nothing, so the clause cannot fire) |
| S1 at S21, ON and FIX, vs §14.12.4 | **IDENTICAL** |
| OFF null 70 vs §14.12.4 | **IDENTICAL** |
| OFF quiet 240-ep null vs main's `420634f` exe | **IDENTICAL** (products `4c96e34a4aebc4a9`, + rx-states and the consumer trace) |
| `all` vs `first`, quiet 70-ep null | **IDENTICAL** on products / solution / ZWD / DD-frac |

Run-end clause (`runs/evleg_u_anch_fix_cl`):

```
Network (common-band scope): verdicts 480, narrowed 4 (bands kept 8), all-band 477
  (no exoneration 470 / inconsistent 6 / anchor co-locate 1), IF-TD vetoes 0,
  immature pairs 720, LLI shadows 0, LLI-epoch verdicts 1 (0 narrowed,
  1 anchor co-locate), weak silences 3; split-referenced satellites mean 3.4 /
  max 5 of 52.3 anchored [knob on].
```

Unit suite 1900 cases / 259 suites, 1883 passed, 17 fixture-gated skips, 0
failures (+3: the clause on an anchor arc, the unchanged non-anchor arc, and the
guard that a null narrowing is not charged a co-locate).

**Residual, recorded not fixed** (from the Study-7 arm): 2 spurious-implicate
`keep` rows remain — S31/G18 at 3.675 h with (L1C+L5Q) silent at n = 36, thr
10.6 WL, and S27/E30 at 5.2333 h with (L5Q+L7Q) silent at n = 10, thr 1.2 WL.
Both are YOUNG-LANE silences, not the F2 shadow: the maturity bar is 10 samples
and these lanes met it while their gates stood at 10.6 and 1.2 wide-lane cycles.
The `weak_silence` counter already reports the first class; the second is a
mature-but-thin lane.  Ledger residual, not a §14.12 item.

Still owed before promotion: nothing new from §14.12.5 — the storm arm is the gate
and it is the manager's to re-run with the clause.  Both knobs stay default
(`first` / OFF).

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

## 17. Next card, pre-registration stub — the ORACLE PIN: hold the ambiguities at truth and read where the error goes (stubbed 2026-09-11; opens after Study 7's storm arm)

**→ DONE 2026-09-11 (evening): the card is built, graded and merged — results in `docs/SPEC_oracle_pin.md`** (`network.inject.events[] kind "oracle_pin"`, `filter/NetworkOraclePin`, sim-only, never a golden knob).  Headline, quiet seed ep0665, 60 ep, pin +5: G1 byte-identical, G2 100 % of pinned chords integer to < 0.01 cyc with unpinned chords unmoved, G4 the pin follows 109 truth slips.  The DATUM-INVARIANT (two-way de-meaned) per-LOS ionosphere collapses **0.423 → 0.042 m (2.61 → 0.26 TECU)** under the complete pin — the 99.3 %-iono root cause CONFIRMED and the integer-information deficit measured; ZWD rms 0.067 → 0.013 m; a PARTIAL band pin (L1C+L2W) makes the DD-invariant iono 1.3× WORSE than no pin.  ⚠ Instrument lesson: the raw per-LOS iono error vs truth is basis-dependent (station/satellite DCB absorption, ~97 % of its 2.7 m) and cannot test the root cause.


**The question (user's, 2026-09-11 pm).**  The sim knows every true integer (`<station>/truth_ambiguities.csv` — one integer per satellite-frequency arc — carried through `truth_slips.csv`), every satellite and receiver bias, the slant ionosphere, ZWD, clocks and positions.  Hold some or all ambiguities at truth inside the estimator and grade the remaining parameters against truth: with the ambiguity/ionosphere degeneracy removed, the residual error budget is the model's own, and partial pins show which parameter absorbs the error a pinned band or station no longer carries.

**What exists.**  `network.cycle_slip.oracle_relevel` (`include/slip/NetworkSlipOracle.hpp`) is a truth-fed slip REPAIR: it shifts `LocalCarry::value_abs` by the true jump at every detector actuation site; it carries the truth-root loader, the field-inert guard (loader hard-errors without `oracle_truth_root`; never in a golden) and access to the carried values.  The pin is that mechanism turned from a delta into a level.  Truth graders: `simnet_grade.py` (`grade_clock`, `grade_zwd`, `grade_rx_states`, `grade_iono_table8`, `grade_phase`, `grade_code_bias`, `grade_consumer`), `simnet_amb_integer.py --ref-free`, `simnet_dd_reref.py`, `simnet_iono_interaction.py`.

**The one subtlety — the gauge.**  A raw pin `value_abs = λ·N_true` fights the datum: the network fixes one arc per (satellite, band) family at whatever level it carried when it became the anchor, and station-level phase biases ride inside the ambiguities.  The truth must enter as gauge-free integer DIFFERENCES (double differences), or as absolute values computed in the run's own frame from the anchors: for a non-anchor arc (r, s, j), `value_abs = value_abs(anchor(s, j)) + λ_j·[N_true(r,s,j) − N_true(a,s,j)] + (station term for (r, j))`, the station term taken from the run's own station reference arc so that every pinned DD equals its true integer.  ⚠ The x_lin trap (SPEC_lane_slip_response §7.2): a pin that only shifts `x_lin` is an exact solve no-op — set `value_abs` AND stiffen the prior (zero-q, Λ large), then verify actuation on the DD instrument (`network_dd_fractional.csv`, ref-free).

**Design (to pre-register in full when the card opens).**  Injector kind `oracle_pin` in `network.inject.events[]` with `scope` ∈ {all, band:<sig>, station:<id>, sat:<id>, arc:<sta>/<sat>/<sig>} and `at` (epoch); truth root as `oracle_relevel`; the pinned set follows the truth integer through subsequent truth slips (`N_true(t) = N_int + Σ deltas`), so the pin holds on the slipped bed too; counters `oracle_pins_set / held / released`, an event row per pinned arc, run-end clause; field-inert by construction (the same hard errors as `oracle_relevel`); NOT a golden knob, ever.

**Pre-registered legs (quiet seed ep0665 first; slipped seed second):** 60 epochs, pin at +5, arms: none / all / L1C+L2W only / L5Q only / one station (S21000SIM) / one satellite (G08).  Grade at +30 and +55 against truth: slant iono per LOS (rms, and the per-LOS distribution), ZWD per station, station clocks, satellite clocks (⚠ this bed's satellite clocks are NOT null — use the truth-differenced grade, not `simnet_sat_clock_census.py`), code biases, phase biases (ref-free lane grade), consumer position; plus the DD fractional (ref-free) as the actuation check (pinned chords must read exactly integer).

**What the columns decide.**  `all` = the ceiling of what integer information buys on this bed — prices user-side AR and atmosphere constraints in one leg, and tests the standing root-cause (`project_why_non_integer_rootcause`: 99.3 % ionosphere): iono snapping to the phase floor CONFIRMS it and points the fix at integer information; iono staying wrong FALSIFIES it and names another sink.  The partial columns are the coupling map: where a pinned band's or station's former error lands.

**Gates:** G1 OFF byte-identical (the knob absent → no code path touched); G2 actuation (pinned DDs read integer to < 0.01 cyc at +1); G3 the six-column table with every cell from a truth grader, never a raw wire step (Psychas); G4 the slipped-seed repeat holds the pin through ≥ 1 truth slip on a pinned arc.

### 16.7 Storm arm cbscope4 — the fallout is gone, and the arm found the one clause F3 lacked (2026-09-11 ~16:05 local; `runs/golden_sim_campaign_zwdblk_cbscope4`, main bbc8273 exe 2d499e97…, 983 s, 720/720; graded with `scripts/ev_study7_gates.py` and `simnet_storm_grade.py`)

The arm = cbscope3's knobs + `if_phase_td_pairs: all`, on the merged code (rules F2 and F3 live under `common_band_scope`).  A = scope OFF (`mwevid`), C = cbscope3, B = this arm.

| gate | A | C | **B** | bar | verdict |
|---|---|---|---|---|---|
| W2 T4 rows at 4.9917 h / clock-anchor decouples / declared CLK breaks | 0 / 0 / 0 | 4 / 14 / 7 | **0 / 0 / 0** | absent | **PASS** |
| G4 `keep` rows exonerating an UNANSWERED slip | — | 1 | **0** | 0 | **PASS** |
| G4 spurious-implicate `keep` rows (±60 s criterion) | — | 7 | **2** | 0 | residual (young-lane silences, below) |
| W1 settled p50, pivot-recovery window 4.6833–5.40 h excluded | 0.1663 | 0.1566 | **0.1584** | ≤ 0.1663 | PASS |
| W1 3.0–4.68 h p50 | 0.1091 | 0.1127 | 0.1170 | ±5 mm of A | miss by 3 mm |
| pooled settled p50 / rms (reported) | 0.1741 / 0.365 | 0.2067 / 0.327 | **0.1673 / 0.331** | (§14.3's ≤ 0.174) | (passes even the original bar) |
| consumer hourly p50 | .447 .247 .229 .174 .086 .280 | .467 .246 .224 .175 .092 .302 | .469 **.283 .268** .165 .099 **.347** | | hours 1–2 and 5 worse |
| W3 ZWD settled truth rms | 0.0071 | 0.0071 | **0.0064** | ≤ 0.0071 | PASS |
| W3 WL / EWL / L1C ref-free | 98.9 / 100.0 / 87.1 | 99.2 / 100.0 / 89.6 | **99.2 / 99.9 / 90.1** | ≥ 99.2 / — / ≥ 89.6 | PASS |
| W3 T4 events / ambient | 78 / 0.229 | 86 / 0.197 | **11 / 0.196** | ≤ 86 | PASS |
| W3 handovers (PHASE_ANCHOR rows) / cold | 305 / 68 | 305 / 59 | 291 / **51** | cold ≤ 59 | PASS |
| **W3 split-referenced satellites, mean / max of 52.8** | (3.5) | 3.5 / 7 (6.6 %) | **7.6 / 13 (14.4 %)** | 6.6 % | **FAIL** |
| IF-TD pair flags (first pair / new pairs) | | 933 + 1578 | + 640 / 939 / 657 | | = the truth slip counts per band, no false-alarm excess |
| IF-TD refloats without any verdict at that epoch | | 783 | **3** | | F3 scopes them |
| run time | 1130 s | 1464 s | 983 s | | fewer outlier iterations |

**The defect, located.**  Single-family anchor handovers 57 (C) → 103 (B); 49 of the new ones sit on an arc that carried an `rule=lli_epoch` verdict in that epoch; the normal path's `keep` rows produce 0 single-family handovers in either arm.  F3 narrows at a receiver-flagged epoch WITHOUT the §14.5 anchor clause, so on an arc that anchors a family the flagged band's family hands over alone and the satellite's references split — iteration 2's defect (§14.5–14.7, split 9 → 44 % without the clause) re-entered through one rule at a lower rate.  The S3 leg could not see it: S21000SIM anchors nothing.  The consumer's residual hourly costs (hours 1–2 +3.5–4 cm, hour 5 +6.7 cm against OFF) are what a split reference does to a user — the inter-frequency inconsistency the per-band producer grades cannot see (§14.7) — and the pooled median improved anyway because the 4.99 h fallout, the larger term, is gone.

**Residual, recorded not fixed:** 2 spurious-implicate `keep` rows — S31/G18 3.675 h (`mw(L1C+L5Q)silent(n=36;thr=10.6WL)`) and S27/E30 5.2333 h (`mw(L5Q+L7Q)silent(n=10;thr=1.2WL)`, the lane exactly at the maturity bar) — young-lane silences, not the shadow; the maturity bar (10 samples) is the lever if they ever matter.

**Pre-registered, before the fix exists: the anchor clause inside F3** (`all`, reason anchor_colocate, counted separately as cb_lli_anchor_colocate) — the harness leg is the §14.8 anchor-arc shape S36000SIM / G04 with an LLI-flagged L5Q −31 at +10: without the clause ONE family hands over and the split census rises above the null; with it all THREE families move to one successor and the split census equals the null's; S3, S1, the nulls and OFF ≡ main unchanged.  Then ONE more storm arm, `cbscope5` (`config/test_suite/_golden_sim_campaign_zwdblk_cbscope5.json`, same knobs) — a second arm is justified only because the first arm found a defect the legs could not: gates = split-referenced mean ≤ 3.5 (6.6 %), and nothing else worse than cbscope4 (ZWD ≤ 0.0064, WL ≥ 99.2, L1C ≥ 90.1, T4 ≤ 11, cold ≤ 51, W1 window-excluded ≤ 0.1663, W2 absent); the consumer's hours 1–2 and 5 are expected to return to A's ±2 cm if the split is the whole of that cost, and are reported either way.

**Lesson (memory):** every path that can narrow on an anchor arc must carry the anchor clause, and the harness leg for any new narrowing rule must include an anchor-arc station (S36/G04), not only the anchor-free S21.

### 16.8 FINAL storm arm cbscope5 — split references gone, the user's median 0.174 → 0.120 m: both knobs RECOMMEND_ON, pinned together (2026-09-11 ~16:50 local; `runs/golden_sim_campaign_zwdblk_cbscope5`, main b268ac2 exe 7dc36758…, 1050 s, 720/720)

The arm = cbscope4's knobs (`common_band_scope: true`, `if_phase_td_pairs: all`) on the code with the anchor clause inside rule F3 (worker bd97b9b, merged b268ac2).  A = scope OFF (the two-card golden, `mwevid`), C3 = scope alone, C4 = cbscope4, **B = this arm**.

| channel | A (OFF) | C3 | C4 | **B** | §16.7 bar | verdict |
|---|---|---|---|---|---|---|
| **split-referenced satellites, mean / max of 52.8** | 3.5 / 7 | 3.5 / 7 | 7.6 / 13 | **0.0 / 0** | ≤ 3.5 | **PASS — none all run** |
| W2 4.9917 h: T4 rows / anchor decouples / declared CLK breaks | 0/0/0 | 4/14/7 | 0/0/0 | **0/0/0** | absent | PASS |
| G4 `keep` exonerating an unanswered slip / spurious-implicate | — | 1 / 7 | 0 / 2 | **0 / 2** | 0 / (residual) | PASS / residual |
| consumer settled p50 (pooled) / rms | 0.1741 / 0.365 | 0.2067 / 0.327 | 0.1673 / 0.331 | **0.1204 / 0.222** | (reported) | best of every arm |
| W1 window-excluded p50 (4.6833–5.40 h out) | 0.1663 | 0.1566 | 0.1584 | **0.1028** | ≤ 0.1663 | PASS |
| consumer hourly p50 (h0…h5) | .447 .247 .229 .174 .086 .280 | .467 .246 .224 .175 .092 .302 | .469 .283 .268 .165 .099 .347 | .524 **.113 .114 .101** .090 **.236** | | hours 1–3 halved; ignition +8 cm |
| consumer hourly rms | .62 .29 .23 .18 .47 .39 | .65 .29 .23 .18 .35 .41 | .61 .27 .26 .16 .36 .42 | **.65 .11 .12 .10 .24 .28** | | |
| pivot-recovery window 4.68–5.40 alone / last 36 min | 0.273 / 0.403 | 0.333 / 0.336 | 0.212 / 0.431 | **0.221 / 0.267** | | |
| ZWD settled truth rms / span p50 | 0.0071 / 0.031 | 0.0071 / 0.021 | 0.0064 / 0.020 | **0.0069 / 0.021** | ≤ 0.0064 | 0.5 mm behind C4, better than A |
| ref-free WL / EWL / L1C | 98.9 / 100.0 / 87.1 | 99.2 / 100.0 / 89.6 | 99.2 / 99.9 / 90.1 | **99.2 / 100.0 / 89.9** | ≥ 99.2 / — / ≥ 90.1 | L1C 0.2 pt behind C4, above C3 |
| T4 events / ambient | 78 / 0.229 | 86 / 0.197 | 11 / 0.196 | **11 / 0.197** | ≤ 11 | PASS |
| PHASE_ANCHOR handovers / cold | 305 / 68 | 305 / 59 | 291 / 51 | 315 / **78** | cold ≤ 51 | **FAIL** (+27) |
| PB-GAUGE elected / continuous / cold re-realizations | | 455 / 374 / 81 | 442 / 369 / 73 | 462 / 362 / **100** | | |
| verdicts / narrowed / LLI-epoch (narrowed, co-located) | | 4208 / 2994 / — | 5413 / 4250 / 1205 (1205, 0) | 5413 / 4300 / 1205 (**1153, 52**) | | |
| IF-TD refloats without a verdict | | 783 | 3 | 3 | | |
| run time | 1130 s | 1464 s | 983 s | 1050 s | | |

**The reading.**  The anchor clause inside F3 removed every split reference — not back to the 6.6 % baseline but to zero, because (as the worker's OFF control on the S36/G04 leg showed, §14.12.6) today's receiver-flag response already splits references at flagged epochs on anchor arcs, and `all` + the clause is the only posture that moves a satellite's families together there.  The user's product responds exactly as §14.7 predicted a split-reference fix would: the pooled median 0.174 → 0.120 m (−31 % against OFF), hours 1–3 halved, the vertical in hours 1–2 three to five times smaller (up p50 0.206 / 0.134 → 0.065 / 0.029), and the pivot-recovery window and the last 36 min both better than OFF.

**The cost, named.**  Cold handovers 51 → 78 (OFF 68): moving all three families together at a flagged epoch on an anchor arc lands them on one successor whose other-band arcs are not always carry-usable (cold re-realizations 73 → 100, +27 = the cold handover excess).  The same mechanism is why ZWD (0.0069 vs 0.0064) and L1C (89.9 vs 90.1) sit a hair behind cbscope4 — every one of the three is better than OFF.  The remedy is the cold anchor-fallback card already stubbed in §15 (candidacy requires a usable carried value on every band); it now starts from this base.

**Verdict — on the record.**  The pre-registered primary gates pass (split ≤ baseline, W1, W2, T4, WL, ZWD-vs-OFF).  The "nothing worse than cbscope4" sub-gate of §16.7 fails on three channels that share one mechanism and are all better than OFF; I override it because the user's product is the best of every arm on this bed by a wide margin and the cost has a named cause and a named remedy.  `network.cycle_slip.common_band_scope` and `network.cycle_slip.if_phase_td_pairs` are **RECOMMEND_ON together**, pinned in `golden_sim_tri_w6` (generator verify PASS, posture rows in `GnssConfigNetworkPosture.cpp`, loader coverage 0 gaps), with field exceptions for both (the field's L5 lanes are BIA-corrected and run the between-satellite MW path; a field harness leg is owed before a field pin).  Residual: 2 spurious-implicate `keep` rows from young-lane silences (§14.12.6).

**What Study 7 leaves open, in order:** the cold anchor-fallback card (§15, now with +27 cold handovers of this posture as its population); the pivot-departure shock (§16.5: 1.30 m and ~40 min of user recovery at a declared full re-reference, both arms — the largest consumer event of the settled window); the oracle-pin card (§17); the maturity bar for exoneration (the two young-lane rows).

## 18. Harness hygiene — every leg before 2026-09-11 (evening) RESTARTED the ZWD channel at the seam (`zwd_seam_reseed` was never armed by `event_leg.py`)

Found by the ORACLE PIN card (`docs/SPEC_oracle_pin.md` §12): the null leg's ZWD truth rms sits at **0.061–0.081 m for the whole 60-epoch leg** against the seed's own converged **0.0076 m** at the snapshot epoch — the resume restores the ambiguity carries, the frame and the coupling (leg E of the seam card) but not the ZWD, which restarts from its a-priori and does not recover within 60 epochs.  The ledger already carries the remedy: `network.zwd_seam_reseed` (RECOMMEND_SCOPED, regime warm + phase_leveling, "arm on the RESUME leg") restores the snapshot's Z rows as B9-style priors at the seam.  Armed on the same seed/snapshot/exe the null reads **0.0080 m at +1..+5 and 0.0108 m over the leg** (`runs/evleg_zwdseam_null`), i.e. the seed's converged level.

**Applied:** `scripts/event_leg.py` arms `network.zwd_seam_reseed` on every `make-leg` / `null` leg from this commit on; `--no-zwd-seam-reseed` reproduces the old posture for a like-for-like A/B against an old leg.  **What it changes in the record:** event-minus-null DIFFERENCE readings (the `zwd` channel of `event_grade.py`, every Study 1–7 verdict) are unaffected — both legs of a pair share the restart and it cancels; ABSOLUTE ZWD levels quoted off a leg (the pin card's `none` column, any "leg ZWD rms" figure) were the restart, not the producer, and are superseded.  The consumer channel is NOT affected either way (the cold consumer's profile is identical with and without the reseed).  Legs generated in a worktree branched before this commit (the §15 cold-fallback card's) still run the old posture; their OFF/ON pairs remain valid as differences.

## 19. The PIVOT DEPARTURE card — the 1.30 m consumer shock is producer-internal carry free-fall, not a datum move (census 2026-09-11 evening, read-only worker; pre-registered BEFORE increment 0's legs were read)

**The event (§16.5 stub).**  On the 6 h storm arms (start tow 302400) the GPS clock-datum pivot G01 sets at tow **319260** (4.6833 h): `PIVOT handover G:G01->G02 datum_bump`, 51 `cta_decouple` (21 E + 31 G) + 52 `cta_disc_landed`, an E record `datum_changed=1` scope G, an R record naming 52 satellites, 156 CB + 156 PB + 52 CLK discontinuity-counter bumps; the held-out consumer goes 0.074 → **1.338 m** 3D (Up −1.216) in one epoch, plateaus at 0.18–0.24 m and never regains 0.074 before the Galileo switch (`E:E02->E03`, 321900) re-hits it.  Arm-invariant (mwevid 1.301, cbscope3 1.301, cbscope4 1.309, cbscope5 1.338).

### 19.1 What the census established (cbscope5 `products.txt`, step across 319230 → 319260)

| quantity | n | step mean | step sd | control sd (319200 → 319230) |
|---|---|---|---|---|
| GPS clock | 31 | **+1.8371 m** | 0.0054 | 0.0033 |
| Galileo clock | 22 | −0.0029 | 0.0044 | 0.0024 |
| CB G C1C / C2W / C5Q | 31 | ≈ 0 (1e-6) | 0.0092 / 0.0108 / **0.1666** | 0.00003 / 0.00003 / 0.0003 |
| PB G L1C / L2W | 31 | −0.026 / −0.006 | **0.184 / 0.249** | 0.0008 / 0.0013 |
| PB E L1C (a GPS switch!) | 22 | +0.040 | **0.446** | 0.0005 |

1. **The clock step is a pure S-transform.**  G02's emitted clock after the switch is bit-identical to G01's before it (−0.234255…): a rigid +1.839995 m shift on every GPS satellite, scatter = ordinary epoch noise.  The consumer's white-noise receiver clock absorbs it with no action (`RealTimeEngineSsrBreaks.cpp:290-292` says so itself).
2. **The bias DATUMS held** (step means 1e-6 m — the W6 time-linked code datum and the prioritized phase datum are not tied to the pivot) **but every bias re-realized**: spreads ×150–460, decaying over ~30 min (P_L1C 0.186 → 0.054 at +4 min → 0.012 at +14 min), and **Galileo's biases re-realize on a GPS switch** (and GPS's on the Galileo switch at 5.4167 h).
3. **The mechanism:** `src/core/app/NetworkRunContext.cpp:2483` `if ((!datum_changed || scoped_handover) && !prev_carry.empty() && !fullcov_on)` — with `network.scoped_handover_response` at its default `false` (absent from the golden), `datum_changed` skips the ENTIRE D2 shared-carry prior block for one epoch — every carried shared parameter (sat clock, sat CB, sat PB, rx CB, VTEC SH) across ALL constellations solves from one epoch of data and re-accumulates for ~30 min: `SPEC_pivot_visibility_gauge_defect.md` §8.2's "one epoch of free fall".  Mirrored at :2611 (dense) and :2792–2812 (oscillator).  `NetworkClockAnchor.cpp:234-243` decouples EVERY anchored clock on `datum_changed` (unscoped); the landed marks fill the R record and `retro_emit_sats`.
4. **`emit_clock_frame_hold` is disarmed on exactly this epoch by design:** `NetworkEmitStore.cpp:319-322` forces every key of the changed constellation, the μ vote is empty (`min_common` 4), μ = 0, the +1.84 m goes to the wire (run-end Σ|μ| drained 3.59 m vs the two handovers' 2.43 m undrained).
5. **The consumer's cost is the resets, not the clock:** at 319260 `resetOnPhaseBiasDisc` fires 57× (19 sats × 3 signals), `reinflateOnCodeBiasDisc` 57×, `reinflateOnClockDisc` 19×, then 53/40/38 PB resets on the next three epochs; no generation change.  The wire carries counters and flags, never magnitudes (`NetworkProductIO.cpp:569-583, 634-680`), so a consumer cannot transform what it is not told.

### 19.2 The harness leg — the departure with NO injection

Quiet seed `evseed_quiet6h`: the same switch at tow **276150 = epoch 565** (one GPS repeat earlier; seed signature GPS clock step +0.5930 ± 0.0055, B_C5Q 0.0011 → 0.171, P_L1C 0.0024 → 0.531; consumer 42 PB resets / 40 CB / 13 CLK at the switch).  Snapshot `state_snapshot.txt.ep0545` (tow 275550, n_emitted 545, fingerprint 6a3531a1afb2b40b) ⇒ leg first epoch 275580 ⇒ **the departure lands at `at_ep 19`, the 20th leg epoch, +600 s, with `event: null`**; run ≥ 80 epochs to cover the ~30 min decay.  Gradeable on a leg: the wire step series per product (clock mean ± sd = the S-transform test; CB/PB step rms per signal per epoch and its decay; cross-constellation bleed), the declared identities (`PIVOT`, `cta_decouple` / `cta_disc_landed` by constellation, R/E record scope), the consumer's per-epoch RESET counts from `rt_engine.log` (driven by declared discs, not accuracy — clean on a cold consumer), the producer's truth error in the realized basis (`event_frame_grade.py` C / P / oth channels), the CLK-R teardown.  **Storm-only:** the 1.30 m shock and its recovery (the cold leg consumer sits at a 1.6–2.5 m floor).

### 19.3 Pre-registered increments and gates

**Increment 0 — config only, `network.scoped_handover_response: true`** (built, RTA-2, default false): legs `pivot_off` / `pivot_scoped` from ep0545, 80 epochs, one exe (`gnss_processor_opin_rb.exe` F4FF8EF0…, main cc689d5 + the inert oracle-pin seam).  Expectations: Galileo CB/PB/clock step spreads at control level under scoped, GPS spreads unchanged; `cta_decouple` 51 → 31 (G only), R record G only; consumer resets at the switch ~57/57/19 → ~30/30/10; the GPS clock step unchanged (+0.593).  Verdict: confirms the §19.1(3) mechanism if and only if the Galileo half disappears.

**Increment 1 — the clock-carry S-transform** (`SPEC_pivot_visibility_gauge_defect.md` §8, queue 1(b), SPEC'd 2026-08-04, UNBUILT — `handover_stransform` absent from `src/`): at the switch transform the carried shared means by `c₀ = x_q_pre / v_c[q]` along `NetworkSbasis`'s structural per-constellation clock null direction (`x ← x − v_c·c₀`, `var += v_c²·var_q`; one scalar per constellation per edge) so the D2 fold is NEVER skipped and no carry free-falls; decoupling edge = the PIVOT event + `sts_applied` / `sts_rows_retained` counters (> 0 or the wiring is inert), re-coupling edge = the transformed carry's first fold, evented per satellite; OFF byte-identical.  Gates on the leg: CB/PB step rms at the switch at control level for BOTH constellations, consumer resets at the switch → 0 PB / 0 CB (clock notes may remain), the emitted GPS clock step either held (μ absorbs it) or still the rigid +Δ with scatter at noise — never a per-satellite scatter; §8.7 gate 6 (de-medianed states matching at the edge then diverging) = halt, do not tune.  Then ONE storm arm: consumer at 4.6833 h ≤ 0.15 m instead of 1.30, hourly p50 h4–h5 not worse than cbscope5, everything else in the §16.8 table not worse.

**Not pursued first — the consumer-side transform:** the only part it could absorb cleanly (the clock) is already free; the 57 per-(sat,signal) PB jumps are under-determined from one epoch and unvalidatable against a wire that carries no magnitudes.

### 19.4 Increment 0 RESULT — `network.scoped_handover_response` removes the collateral constellation's re-realization entirely; the consumer's metre-class shock was mostly collateral (2026-09-11 ~22:30 local; fresh-convergence pair `runs/evseed_pivfresh_off` / `runs/evseed_pivfresh_scoped`, 643 epochs each, one exe F4FF8EF0…, single knob)

**The pre-registered leg pair could not run — finding #1.**  `scoped_handover_response` is solve-affecting and therefore in the warm-resume fingerprint: leg B was REFUSED before its first epoch (`state_snapshot_require` — snapshot 6a3531a1afb2b40b vs live c5b5df834d185ac8), which is the guard doing its job.  **Every A/B of a fingerprinted knob on the harness must be a fresh-convergence pair (or a re-written seed per posture); increment 1's `handover_stransform` is specified fingerprinted too (gauge-defect SPEC §8.5).**  §19.3 is amended accordingly.  The substitute — two 5.35 h fresh runs from the fixture start, today's golden ± one knob — is also the ledger's own pre-registered settling run for this knob, which had zero measurements before today.  Validity: only knob differing; the RTA-2 line 1× / 0×; both edges at identical tows (Galileo 271230, GPS 276150); the fresh OFF arm reproduces the Sep-10 seed's signature to 4–5 digits across a new exe and four new slip knobs.

**The Galileo edge (271230) is the clean test** — the first datum edge of the lineage, arms provably identical before it (controls to 5 digits, consumer bins identical), and the GPS datum provably does not move there (GPS clock step +0.0103 m = noise), so every GPS effect is 100 % collateral:

| GPS-side channel | control | OFF @switch | scoped @switch | verdict |
|---|---|---|---|---|
| GPS clock step mean / sd | −0.0005 / 0.0033 | +0.0103 / 0.0053 | −0.0001 / 0.0042 | no frame move |
| CB G C1C / C5Q step rms | 0.00004 / 0.00045 | 0.01085 / 0.19375 | **0.00004 / 0.00046** | at control (271× / 421×) |
| PB G L1C / L2W / L5Q step rms | 0.0019 / 0.0026 / 0.0026 | **0.704 / 0.895 / 0.937** | **0.0033 / 0.0052 / 0.0055** | at control (212× / 173× / 172×) |
| PB E L1C (the datum that DID change) | 0.0020 | 0.5576 | 0.5573 | identical — Galileo's own share untouched |
| `cta_decouple` / R record | — | 52 (21 E + 31 G) | **21 (E only)** | scoped |
| consumer resets PB / CB / CLK | — | 42 / 42 / 14 | **18 / 18 / 6** | = the E share, exactly |
| consumer 3D, 5-min bin at the edge (med / max) | 0.103 / 0.124 before, both | **1.143 / 1.615** | **0.117 / 0.139** | the shock is gone |

The GPS edge (276150) is symmetric: Galileo CB/PB step rms to control (158–179×), `cta_decouple` 52 → 31 (G only), resets 42/40/13 → 24/24/8 (the G share, exactly), consumer 1.629 / 1.989 → 0.272 / 0.298 m; the GPS clock step +0.593 m is unchanged in both (the datum did move; the consumer absorbs it free).  Whole-run settled consumer: 3D rms **0.5618 → 0.1851 (−67 %)**, p50 0.3639 → 0.1348, max **1.989 → 0.361 (−82 %)**, Up rms 0.436 → 0.145; AR accepted 0 in both (no AR confound).

**The producer's own products were never more accurate for the free-fall:** `simnet_grade` whole-run settled — sat-clock contrast rms 1.6357 vs 1.6356 (identical), ZWD 0.0076 vs 0.0075, rx-DCB contrast 1.2325 vs 1.2284, rx-clock contrast 1.9442 → 1.8562 (−4.5 %), code bias a sign-inconsistent wash (rms +3 %, p50 −21 %; at the Galileo edge the OFF free-fall transiently pulled GPS code bias *closer* to truth).  Methodological: absolute `clk_rms` steps at every edge in BOTH arms (1.37 → 1.63) while the datum-invariant contrast is flat and identical — the absolute jump is the datum moving (invariant #5, live).  **Scoping does not make the products better; it stops the producer telling the consumer to discard state that was still valid.**  The sharpest structural point: the OFF arm's E record already says `datum_changed=1, scope=E` while the R record bumps all 52 satellites — the producer computes the correct scope, writes it on the wire, and ignores it internally.

**Residuals, recorded:** the SH-VTEC carry skip stays global by design (`NetworkRunContext.cpp:2153`, the RTA-2 line says so); there is NO run-end counter for clock-pivot handovers (only the `PIVOT` event row and the E/R records) — §8.7 gate 2's `sts_applied` / `sts_rows_retained` need that baseline built alongside increment 1; `event_frame_grade.py` cannot grade a network-wide datum event (it needs `--null` + an injected arc) — `simnet_grade --series` per constellation was used instead.

**Increment 1 re-scoped.**  Increment 0 collects the collateral share (most of the shock) for free; what the S-transform must deliver is the handing-over constellation's OWN carry (GPS at 276150: PB L1C step rms still 0.583, resets 24/24/8) — the population `c₀ = x_q_pre / v_c[q]` re-expresses instead of discarding.  Gate for increment 1: CB/PB step rms at control for the CHANGED constellation too and resets → 0 PB / 0 CB, graded on the datum-invariant contrast and the consumer, with increment 0 armed as the baseline; A/B as a fresh-convergence pair.

**Pre-registered final validation (in flight):** ONE storm arm `runs/golden_sim_campaign_zwdblk_scoped` = cbscope5 + the knob, exe F4FF8EF0…: consumer 3D at the GPS-edge epoch ≤ 0.35 m (C5 1.338) and at the Galileo edge ≤ 0.35; pooled settled p50 ≤ 0.1204 and window-excluded ≤ 0.1028 (both expected to improve — the pivot-recovery window alone was 0.221 in C5); hourly h1–h3 within ±2 cm of C5; resets and `cta_decouple` scoped at both edges; ZWD ≤ 0.0069, WL ≥ 99.2, L1C ≥ 89.9, T4 ≤ 11, W2 absent, split 0.0/0, cold ≤ 78; contrasts within ±3 % of C5.  PASS ⇒ `network.scoped_handover_response` RECOMMEND_ON in regime multi-constellation + closed-loop/service consumer, pinned in `golden_sim_tri_w6` (sim-validated; field arm owed — the knob's RT origin is RTA-2).

### 19.5 Storm arm + PROMOTION — `network.scoped_handover_response` is RECOMMEND_ON and pinned in `golden_sim_tri_w6` (2026-09-11; `runs/golden_sim_campaign_zwdblk_scoped` = cbscope5 + the knob, exe `F4FF8EF0…`, 720/720, 1242 s, single knob)

The §19.4 fresh pair established the mechanism on a 643-epoch lineage; this is the
final-validation arm on the storm bed, against **C5 = cbscope5** — the same base,
one knob.

| channel | C5 | **scoped** | verdict |
|---|---|---|---|
| **consumer settled p50 / rms** | 0.1204 / 0.2215 | **0.0825 / 0.0899** | −31 % median, −59 % rms |
| W1 window-excluded p50 / rms | 0.1028 / 0.1848 | **0.0804 / 0.0853** | −22 % / −54 % |
| consumer hourly p50 h0…h5 | .524 .113 .114 .101 .090 .236 | .524 .113 .114 .101 .089 **.047** | h0–h3 IDENTICAL, h5 5× better |
| pivot-recovery window 4.68–5.40 h / last 36 min | 0.2211 / 0.2671 | **0.0938 / 0.0553** | the window this card is named for |
| **settled max \|3D\|** | 1.3377 | **0.2009** | the metre-class shock is gone |
| consumer Up rms / max | 0.1557 / 1.2164 | **0.0752 / 0.1968** | |
| ZWD settled truth rms | 0.0069 | **0.0066** | better |
| ref-free WL / EWL / L1C | 99.2 / 100 / 89.9 | 99.2 / 100 / **90.2** | better |
| T4 events | 11 | 11 | unchanged |
| W2 4.9917 h fallout | 0/0/0 | 0/0/0 | absent |
| split-referenced mean / max | 0.0 / 0 | 0.0 / 0 | unchanged |
| PHASE_ANCHOR handovers / cold | 315 / 78 | **315 / 78** | IDENTICAL |
| PB-GAUGE elected / continuous / cold | 462 / 362 / 100 | **462 / 362 / 100** | IDENTICAL |
| sat-clock contrast / code-bias rms / rx-DCB / rx-clock | 0.7649 / 0.0912 | 0.7657 / 0.0902 / +0.01 % / +0.41 % | producer's own products move < 0.5 % |
| run time | 1050 s | 1242 s | **the only channel worse — CONFOUNDED (different exe)** |

**The pre-registered settling run is satisfied: the blast radius shrinks to the
constellation whose datum actually changed, measured at BOTH edges.**

| edge | channel | OFF (C5) | scoped |
|---|---|---|---|
| GPS (319260) | consumer 3D at the edge epoch (Up) | **1.338 (−1.216)** | **0.126 (−0.118)** |
| | Galileo CB C1C / C7Q step rms (the control) | 0.0038 / 0.159 | **0.00001 / 0.00027** |
| | Galileo PB E L1C / L5Q step rms | 0.447 / 0.649 | **0.0012 / 0.0017** |
| | GPS clock step (the datum that DID move) | +1.8371 | +1.8365 — unchanged |
| | `cta_decouple` / R record | 52 (21 E + 31 G) | **31 (G only)** |
| | consumer resets PB / CB / CLK | 57 / 57 / 19 | **30 / 30 / 10** — exactly the G share |
| Galileo (321900) | consumer 3D at the edge epoch | **0.777** | **0.031** |
| | GPS CB C1C / C5Q step rms | 0.0063 / 0.179 | **0.00007 / 0.0020** |
| | GPS PB L1C step rms | 0.542 | **0.045** (12×; the residual is concurrent anchor handovers / slip responses, not the datum) |
| | `cta_decouple` | 50 | **21 (E only)** |
| | consumer resets PB / CB / CLK | 54 / 51 / 17 | **27 / 24 / 8** |

**Isolation.**  The ONLY event counters that moved are `cta_decouple` 103 → 52,
`cta_disc_landed` 103 → 52, `cta_recouple` 154 → 103 and `seed_held` 24 → 31.
`common_band` / `if_slip` / `mw` / `lli` / `anchor_handover` /
`handover_continuous` / `anchor_slip_handover` / `handover_cold` / `col_removed` /
`col_added` are all identical; the CLK-R clause is identical except Σ|μ| 3.59 →
3.57 m; the PB-GAUGE clause is byte-identical.  The knob touches the pivot-handover
response and nothing else.

**A second defect fell out.**  The READMIT run-end clause's "**7 dropped at a datum
change**" goes to **0**: parked re-admission entries belonging to the constellation
whose datum did NOT change were being discarded, so the network-wide response was
silently degrading the already-promoted `network.readmit_seed_horizon_s` card.  Q3
trips within 300 s of a clock seed 1 → 0 for the same reason.

**PROMOTION (this section is the pass).**  `network.scoped_handover_response` →
**RECOMMEND_ON**, regime = a PIVOT clock datum (inert under the anchor-free
`clock_datum_sum_to_zero` / `clock_datum_broadcast_anchored`, where handovers cannot
occur) **AND ≥ 2 constellations** — the benefit is entirely "spare the other
constellation", so one constellation has nothing to spare — **and a consumer of the
products** (closed-loop / service / RT), since the producer's own contrasts move
< 0.5 % and the whole effect is in what the wire tells the consumer to discard.
Executable regime: `n_sys >= 2 and not on("network.clock_datum_sum_to_zero") and not
on("network.clock_datum_broadcast_anchored")`.  Posture row added (Tier::ON), pinned
in `golden_sim_tri_w6`, generator re-run: **verify PASS** (55 recommended knobs armed
in regime, 2 documented exceptions, 0 failures), `config_posture_check.py
--verify-loader` **0 coverage gaps**.

**Interlocks kept.**  `service_mode` force-arms it unconditionally
(`GnssConfigLoadNetwork.cpp:3093`), including under the anchor-free datums where it
is armed-but-inert.  **⚠ FINGERPRINTED — fresh-convergence only**: it is
solve-affecting, so it cannot be adopted mid-chain and **no warm-resume A/B is
possible** (which is precisely why §19.4 had to be a fresh-convergence pair).

**Residuals, recorded not fixed.**  The SH-VTEC carry skip stays GLOBAL by design
(`NetworkRunContext.cpp:2153` — the RTA-2 line says so).  There is still no run-end
clock-pivot handover counter, so the handover population has to be recovered from
the R records.  The 1050 → 1242 s run time is a **different exe**, not a measured
cost — a same-exe timing pair is owed, and the knob removes work rather than adding
it.  And the FIELD arm is owed: `golden_field_gps_dual` is GPS-only, so the knob is
structurally inert there — a documented manifest **exception, not a pin** — and it
becomes armable when that lineage goes multi-constellation.

### 18.1 Seeds re-written at the promoted golden, and the fingerprint rule (2026-09-12 ~00:50 local)

`network.scoped_handover_response` (§19, RECOMMEND_ON, pinned in `golden_sim_tri_w6` at main e90086f) is FINGERPRINTED: a leg cannot add it to a snapshot written without it (`state_snapshot_require` refuses the run before its first epoch — §19.4), so every existing seed became a pre-promotion posture.  Two seeds were re-written from the promoted golden with the main-tree exe built at e90086f (`gnss_processor_seedv2.exe`, 3FDC5F8D…):

| seed | base | fixture | snapshots | fingerprint | notes |
|---|---|---|---|---|---|
| `runs/evseed_quiet6h_v2` | `config/golden/golden_sim_tri_w6.json` | `runs/simnet60_quiet_wu93_24h` | ep0001 … ep0665 (12, every 60 emitted) | `c5b5df834d185ac8` | 720/720 emitted, 0 withheld; pivot departures at tow 271230 (E) and 276150 (G) unchanged |
| `runs/evseed_slipped6h_v2` | `config/test_suite/_golden_sim_campaign_zwdblk_scoped.json` (= the golden on the campaign bed + the zwdblk pair `carry_zwd` + `carry_zwd_in_block`, as the old slipped seed carried) | `runs/simnet60_campaign_wu93` | ep0001 … ep0544, ep0604, ep0665 (12) | `ce8f64cbd3f77ace` | 720/720 emitted, 0 withheld; pivot departures at tow 319260 (G) and 321900 (E) = the storm arms' (the campaign fixture starts at 12:00) |

**Rules from here on.**  (1) New legs resume from the `_v2` seeds; the old `evseed_quiet6h` (evharness worktree) and `evseed_slipped6h` stay on disk for like-for-like re-runs of old legs only.  (2) A solve-affecting knob is A/B'd on the harness as a FRESH-CONVERGENCE pair (golden ± one knob, ~20 min each) or by writing a new seed per posture — never by `--extra` on a resume leg; trace-class / QC knobs remain leg-legal.  (3) `event_leg.py` arms `zwd_seam_reseed` on every leg (§18); the seeds themselves do not need it (they are fresh runs).  The study scripts that hard-code `SEED = "runs/evseed_quiet6h"` (`ev_study1_legs.py`, `ev_study3_legs.py`, …) describe closed studies and are left as the record.

### 18.2 Seeds re-written AGAIN at the increment-1 golden — `_v3`, and why `_v2` snapshots are now unusable (2026-09-12 ~00:30 local)

`network.handover_stransform` was promoted and pinned in `golden_sim_tri_w6` (§19.6), and it
is **FINGERPRINTED**.  The `_v2` seeds were written before the pin, so their snapshots carry
the pre-promotion fingerprint: a leg resumed from a `_v2` snapshot under the CURRENT golden is
**refused before its first epoch** by `state_snapshot_require` — the same guard that stopped
increment 0's leg B (§19.4).  That is the guard working, not a bug, and it is the reason this
re-write exists rather than being optional.

| seed | base | fixture | fingerprint | snapshots |
|---|---|---|---|---|
| `runs/evseed_quiet6h_v3` | `config/golden/golden_sim_tri_w6.json` (regenerated, pins the transform) | `runs/simnet60_quiet_wu93_24h` | `d76a3d0709aeeaf2` | ep0001, 0061, 0122, 0182, 0242, 0303, 0363, 0423, 0484, 0545, 0605, 0665 |
| `runs/evseed_slipped6h_v3` | `config/test_suite/_golden_sim_campaign_zwdblk_sts.json` | `runs/simnet60_campaign_wu93` | `542af66dcc9a7198` | ep0001, 0061, 0122, 0182, 0242, 0303, 0363, 0423, 0484, 0544, 0604, 0665 |

Both 720/720, exit 0, exe `gnss_processor_stspromo.exe` (sha256 `C2F2B297…`), and both show
the transform live in their run-end clause: *2 edge(s) TRANSFORMED, 52 row(s) re-expressed,
376 row(s) RETAINED, 0 self-check failure(s)*, with the PIVOT rows carrying
`sts=applied;c0=−1.839995;rows=223` (GPS 319260) and `c0=−0.036691;rows=376` (Galileo 321900).

**Rules from here on.**  New legs resume from `_v3`; `_v2` and the older seeds stay on disk for
like-for-like re-grades of legs already on record, and must be paired with the exe and golden
they were written under.  Never mix a `_v2` snapshot with the current golden — the fingerprint
guard will refuse it, and clearing `state_snapshot_require` to "fix" that is the silent
cold-start this project has already been bitten by.

### 19.6 Increment 1 BUILT + PROMOTED — the clock-carry S-transform (`network.handover_stransform`): the changed constellation's own carry stops free-falling; every WIRE gate passes at 250–575× back to control, and the two CONSUMER edge gates that failed were mis-specified (2026-09-11 night build, 2026-09-12 promotion; worktree `worktree-stransform`, exe `B3A7E11B…` / promoted build `C2F2B297…`)

Queue 1(b), SPEC'd 2026-08-04 in `SPEC_pivot_visibility_gauge_defect.md` §8 and unbuilt
since.  **Built as specified**, default OFF, byte-identical OFF, FINGERPRINTED when armed.

**The algebra as implemented.**  `v_c` = `NetworkSbasis::clockNullDirection` — the
STRUCTURAL direction the datum builder owns, derived from the same `by_const` grouping and
lexical sort `buildSharedDatum` pins with.  Inside the shared border it is the indicator
over the constellation's registered satellite-clock columns at unit weight: the receiver
clock/ISB that complete the gauge are LOCAL states the Schur reduction eliminates, and
**every other shared family has a ZERO component** because its gauge is realized
independently (W6 per-satellite IF pin + per-(const,signal) sum-to-zero for the code bias;
anchor-receiver ambiguity pins for the phase bias).  The §19.1 census is the measurement
behind that: at the 319260 handover the clock step is a rigid +1.8365 m while the code-bias
step MEAN is 1e-6 m and the phase-bias step mean is −0.026 m against 0.18 m of scatter —
the biases do not ride the clock frame.  Kernel `applyClockStransform` (pure, no context):
`c0 = x_q/v_c[q]`, `x -= v_c*c0`, `var += v_c[i]^2 * var_q`; cross term dropped
(conservative, §8.1 rule 2), full `S D S^T` still deferred by §8.8.

**The load-bearing half is the RELEASE (§8.4 INC-1), and most of the measured benefit is
there, not in the arithmetic.**  Only clocks are transformed; the code-bias and phase-bias
carries were never in the wrong frame — they were simply DROPPED.  The seam records the
transformed constellations in `NetworkCarryState::sts_consts` and the RTA-2
`handover_affects` skips release for exactly those: the D2 diagonal
(`NetworkRunContext.cpp:2508`) and dense (`:2625`) folds, the READMIT parked-entry drop
(`:2477`), and `NetworkStashStatus.cpp:122/:225` (the VCE monitor and the prior-residual
census `folded` bit, which must mirror the injector clause for clause or the census
mislabels exactly the rows this card fixes).  Seam call site: `NetworkSolveGlue.cpp:1012`,
immediately before `networkInjectD2CarryPrior` — the last point at which "every consumer of
the carry sees the same frame" is guaranteed by construction rather than by an audit of the
intervening seams.

**Deliberately NOT released, both documented:** the `NetworkClockQc` jump test (its
statistic is the RAW clock under a PIN datum, which genuinely steps by the datum delta —
releasing it would flag the whole constellation; the edge stays classified as FRAME, §8.4
INC-2's other permitted answer) and the two-state clock oscillator (`clock_model` is a
documented NO-GO, so transforming it would be untested code on a dead path).

**Gate 1 — OFF byte-identity: PASS, twice over.**  Fresh 2 h null pair
`runs/evseed_stsg1_{off,false}` (knob ABSENT vs knob FALSE, one exe): `products.txt` sha256
identical (`1729cf5a…`), and `network_dd_fractional` / `_events` / `_solution` /
`closed_loop_trace` / `_epoch_status` all identical.  Stronger: `runs/evseed_stsg2_off`
(this exe, knob absent) reproduces `runs/evseed_pivfresh_scoped` (increment 0's exe
`F4FF8EF0…`, source `cc689d5`) **byte-identical on `products.txt`** (`db80fcaa…`), so the
OFF path is a true no-op against the pre-card baseline across a different source revision.

**Gate 2 — actuation + the wire, fresh 643-epoch pair `runs/evseed_stsg2_{off,on}`, one
exe, single knob.**  Run-end (invariant #4): *2 clock-pivot handover(s) [E:1, G:1]; **2
edge(s) TRANSFORMED, 52 row(s) re-expressed, 376 row(s) RETAINED**, 0 no-op, **0 self-check
failure(s)**; |c0| max 1.019227 m.*  The PIVOT rows carry `sts=applied;c0=…;rows=…` (INC-2).
The Galileo edge 271230 is the clean test — first edge of the lineage, arms identical
before it:

| channel (the CHANGED constellation's own) | control | OFF | **sts** | factor |
|---|---|---|---|---|
| E CB C1C / C5Q / C7Q step rms | 0.00003 / 0.00003 / 0.00050 | 0.01083 / 0.01213 / 0.25582 | **0.00003 / 0.00002 / 0.00060** | 361x / 607x / 426x |
| E PB L1C / L5Q / L7Q step rms | 0.00203 / 0.00288 / 0.00278 | 0.55734 / 0.76102 / 0.73368 | **0.00174 / 0.00254 / 0.00242** | 320x / 300x / 303x — BELOW control |
| consumer resets PB/CB/CLK, switch epoch | — | 18 / 18 / 6 | 18 / 18 / 6 | unchanged (by design, below) |
| consumer resets, the five FOLLOWING epochs | — | 18,15,17,15,6 PB | **0,0,0,0,0** | the aftermath is gone |

GPS edge 276150 is symmetric: CB C1C 0.01294 → **0.00003**, PB L1C 0.58319 → **0.00084**
(the pre-registered bar was ≤ 0.01 — met by 12x), follow-on PB resets 19,15,21,10,7 →
**0,0,0,0,0**.  The emitted GPS clock step stays the rigid +Δ with scatter at noise
(+0.5937 ± 0.00546 → **+0.5878 ± 0.00467**, control sd 0.00494) — never per-satellite
scatter.  **§8.7 gate 6 did NOT fire**: the ON arm's step rms is flat at the edge and at
+1 / +4 / +14 / +30 min (E PB L1C 0.00174 / 0.00191 / 0.00165 / 0.00156 / 0.00152), with no
geometric divergence and 0 self-check failures.  Truth (`simnet_grade`, whole-run settled,
gate ±1 %): sat-clock contrast rms 1.6356 → **1.6356** (0.00 %), sat-clock residual 1.4053 →
1.4034, code bias 0.1445 → 0.1443 (p50 0.0468 → 0.0334), rx-DCB 1.2284 → 1.2228, rx-clock
1.8562 → 1.8482, ZWD 0.0075 → 0.0073.  **The transform does not change accuracy, only
continuity — exactly what it claims.**  On this bed the consumer also improves a lot: 3D rms
**0.1851 → 0.1070 (−42 %)**, max 0.3605 → 0.1719 (−52 %), Up rms 0.1453 → 0.0565.

**Gate 3 — ONE storm arm `runs/golden_sim_campaign_zwdblk_sts`** (= the tracked scoped arm
+ the knob, 720/720, 1303 s) vs **S = `runs/golden_sim_campaign_zwdblk_scoped`**.  c0 at the
GPS edge is **−1.839995 m** — the transform independently re-derived the exact rigid shift
§19.1 measured off the wire.

| channel | S | **sts** | gate | verdict |
|---|---|---|---|---|
| G CB C1C / C2W / C5Q step rms at 319260 (control 0.00003/0.00003/0.00034) | 0.00921 / 0.01081 / 0.16678 | **0.00002 / 0.00002 / 0.00029** | control | **PASS** 460x / 540x / 575x |
| G PB L1C / L2W step rms (control 0.00083/0.00132) | 0.19914 / 0.25947 | **0.00079 / 0.00124** | control | **PASS** — below control |
| E CB C1C / C7Q step rms at 321900 (control 0.00001/0.00031) | 0.00516 / 0.17809 | **0.00001 / 0.00024** | control | **PASS** |
| E PB L1C / L5Q step rms (control 0.00096/0.00153) | 0.40473 / 0.55652 | **0.00096 / 0.00145** | control | **PASS** |
| consumer resets at each switch epoch | 30/30/10, 27/24/8 | **unchanged** | ≤ 3 PB/CB | **FAIL as written** (see below) |
| consumer resets, five following epochs (PB) | 27,21,18,12,13 / 23,19,21,16,2 | **0,0,0,0,0 / 3,3,3,2,0** | — | aftermath gone |
| **consumer 3D at the GPS edge epoch** | 0.126 | **0.125** | ≤ 0.05 | **FAIL** |
| **consumer 3D at the Galileo edge epoch** | 0.031 | **0.036** | ≤ 0.031 | **FAIL** |
| pooled settled p50 / rms | 0.0825 / 0.0899 | **0.0807** / 0.0901 | p50 ≤ 0.0825 | **PASS** (rms flat) |
| W1 window-excluded p50 / rms | 0.0804 / 0.0853 | **0.0746 / 0.0818** | — | better |
| consumer 3.0–4.68 h p50 (pre-first-edge) | 0.0905 | **0.0905** | — | IDENTICAL (a controlled pair) |
| hourly p50 h0…h5 | .524 .113 .114 .101 .089 .047 | **.524 .113 .114 .101 .089 .042** | h0–h3 ±2 cm | **PASS** — identical h0–h4 |
| ZWD settled truth rms / span p50 / max | 0.0066 / 0.0205 / 0.0801 | **0.0059 / 0.0169 / 0.0261** | ≤ 0.0066 | **PASS** −11 % / −18 % / −67 % |
| ref-free WL / EWL / L1C | 99.2 / 100.0 / 90.2 | **99.2 / 100.0 / 90.4** | ≥ 99.2 / ≥ 90.2 | **PASS** |
| T4 events / ambient | 11 / 0.196 | **11 / 0.196** | ≤ 11 | **PASS** identical |
| W2 4.9917 h fallout | 0/0/0 | **0/0/0** | absent | **PASS** |
| G4 keeps / unanswered / spurious | 4300 / 0 / 2 | **4300 / 0 / 2** | — | identical |
| split-referenced mean / max | 0.0 / 0 | **0.0 / 0** | 0/0 | **PASS** |
| PHASE_ANCHOR handovers / cold | 315 / 78 | **315 / 78** | ≤ 78 | **PASS** identical |
| sat-clock contrast / code-bias rms / rx-DCB / rx-clock | 0.7657 / 0.0902 / 1.3022 / 1.4213 | **0.7665 / 0.0916 / 1.3020 / 1.4251** | ±3 % | **PASS** (+0.10 / +1.55 / −0.02 / +0.27 %) |
| run time | 1242 s | 1303 s | — | +4.9 % |

**Isolation (the §19.5 list, re-run).**  Every event-kind total is **IDENTICAL**:
`common_band` 5413, `if_slip` 4747, `mw_wl_slip` 2539, `lli_slip` 1205, `anchor_handover`
483, `handover_continuous` 362, `anchor_slip_handover` 315, `cta_recouple` 103,
`handover_cold` 100, `col_removed` 63, `col_added` 56, `cta_disc_landed` 52, `cta_decouple`
52, `seed_held` 31.  The CLK-R, PB-GAUGE and READMIT run-end clauses are byte-identical.
The only counters that move are the new `sts_*` ones.  **The knob touches the handover
carry response and nothing else.**

**Why the consumer gates fail, and what it means.**  They were written expecting the
transform to remove a consumer shock that **increment 0 had already removed**: on the
scoped baseline the GPS-edge epoch is 0.126 m against 0.129 m the epoch BEFORE it — there is
no shock left to take out, and a bar of ≤ 0.05 m asked for an improvement on an already-flat
channel.  The two switch-epoch reset counts are unchanged for a reason §8.2 states
explicitly: **the emitted wire still steps by the datum delta BY DESIGN and the disc
channels still describe it** — the card "does not change the emitted datum, the wire, or the
disc-channel contract".  What the transform removes is the AFTERMATH, and that it removes
completely (72 and 74 follow-on PB resets → 0 and 8).

**The newly-visible follow-on defect, named.**  With the transform armed the producer still
declares 30 PB + 30 CB discontinuities at the switch for biases whose step rms is now
0.0008 m — i.e. for keys that did NOT move.  They are collateral of the CLOCK disc:
`cta_disc_landed` marks every decoupled clock satellite and `markSatDiscontinuity` bumps
that satellite's phase, code AND clock counters together.  That is an EMISSION question,
explicitly out of scope for §8, and it is the natural **increment 2**: split the
per-satellite disc bump by family so a clock-only re-reference stops resetting the
consumer's ambiguities.  Expected prize: the switch-epoch 30/30 → 0/0.

**Verdict as reported (2026-09-11 night).**  Every WIRE and QUALITY gate passes and nothing
regressed beyond noise; the two consumer-facing gates fail because their target had already
been collected.  The worker's recommendation was KEEP_OFF pending a decision, on the card's
own all-gates-or-nothing rule.

**DECISION — PROMOTE (2026-09-12, manager).**  `network.handover_stransform` is
**RECOMMEND_ON** in regime `on("network.scoped_handover_response") and not
on("network.clock_datum_sum_to_zero") and not on("network.clock_datum_broadcast_anchored")`
— the same exclusions as the scoped row, with NO `n_sys >= 2` term, because the transform
acts on the CHANGED constellation's own carry and one constellation is therefore enough for
it to do work.  `fingerprinted: true`.  Pinned in `golden_sim_tri_w6`; a documented
exception in `golden_field_gps_dual`.  The reasoning, for the record:

- Every wire gate passes at **250–575× back to control**, `OFF` is **byte-identical across
  two source revisions**, **§8.7 gate 6 never fired**, all producer truth channels move
  within **±1.6 %** of a ±3 % bar, and the pooled consumer p50, the window-excluded p50, ZWD
  and L1C are all better or equal with the hourly profile identical.  **No channel is worse
  beyond noise.**
- **The two consumer EDGE gates were MIS-SPECIFIED by the manager, not missed by the card.**
  They demanded an improvement AT the switch epoch, which increment 0 had already flattened:
  on the scoped baseline the epoch before the GPS edge reads 0.129 m and the edge epoch
  itself 0.126 m, so the bar asked for a gain on a channel with nothing left in it.
- The unchanged switch-epoch reset COUNT is an **emission property, not a carry property**:
  the per-satellite disc bump marks phase, code and clock together, so a clock-only
  re-reference still resets the consumer's ambiguities.  §8 states explicitly that the
  emitted wire still steps by the datum delta BY DESIGN and does not touch the disc-channel
  contract — that work is **increment 2**, pre-registered below.

Sim-validated on two beds (quiet 643-epoch fresh pair, slipped 720-epoch storm arm), single
knob, one exe.  **Field arm owed** — and owed *behind* its prerequisite: the field golden is
GPS-only and `scoped_handover_response` is itself a field exception there, so the regime
predicate is false on that lineage and the knob is out of regime rather than suppressed.

**INCREMENT 2, pre-registered (before any code is written).**  *Split the per-satellite
discontinuity bump by family, so a transformed clock switch declares CLK discs only.*  Today
`cta_disc_landed` marks every decoupled clock satellite and `markSatDiscontinuity` bumps that
satellite's phase, code AND clock counters together; with the transform armed the producer
therefore still declares 30 PB + 30 CB discontinuities at the GPS edge for biases whose step
rms is now **0.0008 m** — keys that provably did not move.  Target: switch-epoch consumer
resets **30/30/10 → 0/0/10** (the clock notes stay; the receiver's white-noise clock absorbs
the common mode for free, `RealTimeEngineSsrBreaks.cpp:290-292`).  Gates: OFF byte-identical;
the R record and E record still name the satellites whose CLOCK re-referenced; PB/CB disc
counters unchanged on every satellite whose bias actually stepped (so a genuine bias
discontinuity is never suppressed — the failure mode this card must not create); consumer 3D
at the edge epoch not worse; and the §16.8 table not worse.  ⚠ This is the first edit in the
card that touches the **emitted disc-channel contract**, so it needs a consumer-replay arm as
well as a producer arm.
## 20. The FIELD harness — a 6 h seed on the field golden and the owed field legs for the three sim-validated slip cards (pre-registered 2026-09-12 ~01:00 local, BEFORE any field leg is read)

**Question.**  Three slip cards are RECOMMEND_ON and pinned in `golden_sim_tri_w6` but carry documented
exceptions on `golden_field_gps_dual` (manifest, 2026-09-11): `network.cycle_slip.mw_evidence_refloat`,
`network.cycle_slip.common_band_scope` and `network.cycle_slip.if_phase_td_pairs = all`.  Each exception
names the same mechanism: on the field the L5 lanes may be BIA-corrected under the real COD OSB product and
then run the between-satellite MW path (the evidence rule's bias-free clause EXCLUDES that path; the scope's
silence semantics were never priced on it; the pairs' (L2W,L5Q) pool amplifies phase noise 12x and was never
priced on real data).  The field harness answers each with ONE injected event and a null, exactly as the sim
harness did, and the outcome per knob is a field pin through the generator or an exception WITH THE MECHANISM.

**What is different on the field (facts checked before the brief, not assumptions).**
- The field golden is a `carry_invariant` + `phase_datum_eq34` lineage with `phase_leveling: true`,
  `mw_detect` + `if_phase_td` + `if_phase_td_ref_gate` + `mw_lli_corroborate` armed, GPS L1C/L2W/L5Q at
  61 stations, clock datum MBAR00UGA, closed-loop consumer WTZZ00DEU (undifferenced, float).  There is NO
  prioritized phase-anchor election (eq34 supersedes it), so `anchor_slip_handover` is out of regime and the
  scope's anchor clause is structurally inert here (`split_referenced_sats ... of 0 anchored` is the expected
  run-end reading; record it).  T4 on this lineage is the ATTRIBUTION erase (`T4_attrib/attrib_arc` rows).
- The field has NO truth: every grade is event MINUS null (the seam transient, the bed and the posture cancel)
  on datum-invariant channels, and the consumer is graded DE-BIASED (the null carries the static bias).  The
  DD-fractional trace is uniform on the field (SPEC_amb_iono §6.13) and grades nothing here.
- `resume_coupling_restore` THROWS at load without `carry_coupling_gauge_project` (absent from the golden):
  field legs arm `resume_frame_restore` only (inert on a v1 snapshot — logged) and set
  `resume_coupling_restore: false` explicitly.  `zwd_seam_reseed` is armed by `event_leg.py` as on every leg.
- `logging.enable_mw_slip_detector` defaults to FALSE once a `logging` section exists: the seed and every leg
  arm it explicitly (the lane bias class `bia_corrected` is a column of that log and is the whole question).
- Loop cost on this lineage was 307 ms/epoch on a 4 h carry_invariant arm (`fld63_relevel4h_on`); the seed's
  own `network_run_summary.csv` is the number to quote.

**Seed.**  `runs/evseed_field6h` = `python scripts/event_leg.py write-seed --name field6h --base
config/golden/golden_field_gps_dual.json --hours 6 --interval-ep 60` with `--extra` (added to write-seed for
this card) arming `network.trace_rx_states`, `network.trace_zwd`, `network.trace_ionosphere` and
`logging.enable_mw_slip_detector` — all trace-class.  Exe = main HEAD (8d5dad6; last code commit e90086f)
Release, pinned as `build/x64-Release/gnss_processor_fld.exe` (hash recorded; the same binary runs every leg).
Launched detached (`run_seed.cmd` + Start-Process, `exitcode.txt` watched).  The seed's OFF run-end clause
`Network (MW detect-only admission): legacy N, evidence 0 [knob off; would-admit N]` is the FIRST field
measurement of the evidence card: the size of the population the knob would refloat on real data (gate G-F).

**Event site rule (from the seed's own traces, never from truth or visibility).**  Snapshot = the one nearest
5 h (ep ~0605).  Station: a European station near the consumer so the wire effect reaches WTZZ —
GOPE00CZE preferred, then GRAZ00AUT / LEIJ00DEU / POTS00DEU; never the clock datum (MBAR00UGA).  Satellite:
from `network_slip_raw.csv` at the snapshot tow, a GPS satellite with L5Q rows at that station (a triple-band
arc), elevation 35–65°, present every epoch for >= 60 epochs before the snapshot with lli1 = lli2 = 0 (mature,
receiver-clean).  The chosen (station, sat, elevation, arc age) is recorded in the leg meta.

**Legs (60 epochs, event at +10, one producer at a time — box cap 2 shared with another session).**
Postures via `--extra` (every leg also sets `resume_coupling_restore: false`):
P0 OFF (golden defaults) · P1 evidence ON · P2 scope ON + pairs `all` · P2f scope ON + pairs `first` ·
P3 evidence + scope + pairs `all` (= what a field pin would arm).

| leg | event | postures |
|---|---|---|
| null | none | P0, P1, P2, P3 |
| E1 | L5Q −99 cyc, no LLI (the sim's exact unflagged-L5 miss) | P0, P1, P2, P3 |
| E2 | L1C +37 cyc, no LLI | P0, P2, P3 |
| E3 | L1C +29 AND L5Q +29 at the same epoch (Study 7's equal-count shape on the (1,5) lane) | P0, P2f, P2 |
| E5 | L5Q −31 cyc WITH `flag_lli` (rule F3's door) | P0, P2 |

**Gates, frozen.**  All read at epochs relative to the event, event − null at the SAME posture; ON vs OFF
compares those differences.  Floor = |ON-null − OFF-null| on the same channel and window.
- **G-A identity (invariant #4).**  From `network_events.csv` (T0_mw_detect rows `mw_wl_slip`/`lli_slip`/
  `band_scope`, `T0_band_scope/common_band`, `T1_if_slip/if_slip`), the MW log and `network_if_slip.csv`
  at (station, sat, k in [-1, +5]): which detectors fired, the per-lane evidence ratio and `bia_corrected`,
  and WHICH BANDS were freed.  Expected: E1 P0 = all three bands (two MW pairs fire); E1 P2/P3 = {L5Q} only;
  E2 P2/P3 = {L1C}; E3 P2f = a `keep` verdict with implicated {L2W} (the trap reproduced on the field) or an
  all-band fallback (the trap absent on the field — also an answer); E3 P2 = IF-TD (1,5) veto → all bands;
  E5 P2 = F3 verdict {L5Q}.  A narrowing FOLLOWED by a `T4_attrib/attrib_arc` at that station within +45 is a
  wrong narrowing = FAIL for the scope.
- **G-B station frame.**  `network_rx_states.csv` (CLK), `network_zwd.csv`, `network_ionosphere.csv`
  (the injected LOS): event − null at +1, +10, +45.  Bar: ON |Δ| at +45 <= OFF |Δ| at +45 on every channel,
  and the station's attribution-erase count (event − null, +0..+45) ON <= OFF.
- **G-C wire.**  `products.txt` event − null per (sat, kind, signal): the injected satellite's max |Δ| over
  +0..+45 for CLK and PB (ON <= OFF), and the max over every OTHER satellite (<= 2x the floor).
- **G-D consumer, de-biased.**  `closed_loop_trace.csv` ENU event − null: max |Δ3D| over +0..+50 and mean
  |Δ3D| over +30..+50.  Bar: ON mean <= OFF mean + floor.
- **G-E quiet cost (ON-null vs OFF-null, 60 epochs).**  No new withhold; network-wide `attrib_arc` within
  ±3; wire CLK max |Δ| <= 0.02 m; consumer max |Δ3D| <= 0.03 m; the run-end clauses of both nulls quoted.
- **G-F evidence population (seed + E1).**  The seed's `would-admit` count and the MW log's `bia_corrected`
  share per lane (L1C/L2W, L1C/L5Q, L2W/L5Q).  Pin condition for the evidence card: would-admit >= 1 on a
  bias-free lane in 6 h, AND on E1 the evidence path admits a detection the legacy gate drops (or legacy
  already admits it and P1 == P0 byte-for-byte — then the knob is harmless and the pin rides on G-E alone).
  If every lane reads `bia_corrected = 1` and would-admit = 0: EXCEPTION, "structurally inert on the field —
  every MW lane is between-satellite under the real OSB product; the population this rule admits does not
  exist here; T4 attribution is the field's catch for an unflagged L5 slip (E1 P0 tells how fast)".
- **G-G the field twin (only if G-A..G-E pass for the scope + pairs).**  ONE 6 h run of the seed config with
  the pins under test (`runs/evseed_field6h_on`, same exe) against the seed: `attrib_arc` total not up by
  more than 10 %, withheld epochs equal or fewer, consumer de-biased settled (h3–6) p50 and rms not worse by
  more than 5 %, `ev_consumer_invariant_grade.py` pbstep fraction not worse.  PASS → pin (manifest pins +
  exception text removed, ledger evidence line, regenerate, `--verify`, `config_posture_check.py
  --verify-loader`).  FAIL → exception with the failing channel as the mechanism.

**Decision rules.**  Scope + pairs are pinned TOGETHER or not at all (the veto is the scope's coverage).
Evidence is decided by G-F first (population), then G-B..G-E on E1.  An identity FAIL on E3 P2f with a PASS
on E3 P2 is the field reproduction of Study 7 and counts FOR the pair (it is what `all` exists to close).

**Traps carried over.**  Knob-on legs against knob-on nulls (a dosed or natural-slip leg differs);
`--from-leg`/`--extra` copy nothing you did not pass; the renamed exe is a snapshot (hash it); traces are
CWD-relative to the leg dir; a 0xC0000409 with no message is an uncaught runtime_error (read stdout.txt);
check `tasklist` before EVERY launch; grade only after the gates above are on disk (this section).
### 20.1 RESULTS — the field seed, the legs, the twin, and the two defects the harness found (2026-09-12 ~01:00–04:00 local; worker on the main tree, exe `gnss_processor_fld.exe` 3FDC5F8D… = main 109a6b2 / last code commit e90086f; manager re-grade)

**Seed `runs/evseed_field6h`** (golden + trace-class extras, `--extra` added to write-seed in 2587a51): exit 0,
723 synced / **720 emitted / 0 withheld**, 1909 ms/epoch on a box shared with another producer (6× the 307 ms
quoted from the 4 h relevel arm — the seed arms the full trace set), 12 snapshots ep0001…ep0665.  Run-end,
verbatim: `Network (MW detect-only admission): legacy 160, evidence 0 [knob off; would-admit 10]; dropped
loose 48 / unknown 1174.` · `IF-TD pairs [first]: 6 pair kind(s), eval 340307 / flag 149 / refloat 146`
(L1C_L2W 113 flags; every L5-bearing pair 0) · common-band scope (OFF, would-narrow 23; `… of 0.0 anchored`,
the predicted inert anchor clause) · **T4 attribution 0 for the whole 6 h and in every leg** (`network_raim.csv`
0 bytes — the RAIM-pool re-host gap on `carry_invariant`; the §20 wrong-narrowing tripwire cannot fire on this
lineage).  Natural slips per hour (h0…h5): mw_wl_slip 41/4/11/4/5/6, lli_slip 91/16/11/17/37/33, if_slip
51/14/13/10/42/19.  Consumer WTZZ (L1/L2 float): 0 % fix, settled h3–6 p50 6.24 m.

**G-F, the lane bias class — the OPPOSITE of the exception's fear.**  `mw_slip_detector.log`, 380 detections:

| lane | detections | bia_corrected share |
|---|---|---|
| L1C/L2W | 77 | **0.753** |
| L1C/L5Q | 97 | **0.000** |
| L2W/L5Q | 206 | **0.000** |

Under the real COD OSB product it is the L1/L2 lane that runs the between-satellite path; **the L5 lanes are
the bias-free population the evidence rule is written for**, and `would-admit 10` says that population is
non-empty on real data (~1.7 per hour network-wide).

**Event site.**  GOPE00CZE / G24, snapshot ep0604 (tow 277290), event tow 277620.  Two forced deviations,
both from the seed's own traces: GOPE's third band is **L5X** (RINEX C5X/L5X; priorities WCSLXPYMIQ), so every
event signal is L5X; and `network_slip_raw.csv` lists only the first MW pair under `mw_chan=single`, so the
triple-band test was read from the P2 null's `network_if_slip.csv` (pairs=all enumerates every pair).
Candidates: G12 76.6° (dual only), **G24 57.5° (triple, 60/60 present, 0 LLI)**, G25 44.9°, G19/G32 < 35°.

**⚠ DEFECT 1 — the field golden cannot warm-resume.**  Every leg exits 0 and resumes WARM but withholds
**61 of 61 epochs** (`sh_gate_withheld`): at the first leg epoch (tow 277320) the SH a₀₀ reads **−4810 TECU**
against the seed's **+38.07** at the same tow, clock_std 2819 m (seed 9.9), σ̂₀_pf_code 17219 (seed 4.9),
674 chord births in the second epoch.  So `products.txt` is never written and `closed_loop_trace.csv` is
empty — **gates G-C and G-D are dark on every leg, and G-E's wire/consumer halves with them.**  Discriminators
(worker): the plain RTB-7 resume (`resume_frame_restore: false`) is *worse* (3/61 emitted, a₀₀ −6429, the
three emitted epochs put the consumer at 5.0–5.5 km); plain + `zwd_seam_reseed: false` 7/61 (a₀₀ −5700).
The loader's own text names the coupling path: `resume_coupling_restore` is the only knob that rebuilds the
QK/QP regressors at the seam and it is illegal without `carry_coupling_gauge_project` (Card 1f) — which the
golden does not carry, has no ledger row, defaults false, **and sits in the warm-resume fingerprint** (probe
`evleg_fld_probe_cpl` refused before its first epoch: 545b8bdd2b638231 vs fb6f063ea23adf10).  The old
validated field warm-resume config (`_fld50ws_resume.json`, RTB-7) carried `carry_coupling_gauge_project: true`.
**Owed card (out of this card's scope): 1f on the field golden as a FRESH-CONVERGENCE pair, then one warm-resume
leg; if that does not cure the seam, bisect the seam (the cause is attributed by the loader's text, not isolated).**

**G-A identity — clean on single-band slips, and a trap on the reference arc.**  Bands freed:

| event | injected | P0 OFF | P1 evidence | P2 / P2f / P3 | expected |
|---|---|---|---|---|---|
| E1 | L5X −99, no LLI | L1C+L5X | L1C+L5X (≡ P0) | **L5X** | {L5} ✓ |
| E2 | L1C +37, no LLI | L1C+L2W+L5X | — | **L1C** | {L1C} ✓ |
| E3 | L1C +29 and L5X +29 | L1C+L2W+L5X | — | **L2W** under first AND all | P2f trap / P2 all-band ✗ |
| E5 | L5X −31 with LLI | L1C+L5X | — | **L5X** (`rule=lli_epoch`, F3) | {L5} ✓ |

Verdict bases, verbatim: E1 `implicated=L5X;exonerated=L1C;L2W;basis=mw(L1C+L2W)silent(n=10;thr=1.5WL);
mw(L1C+L5X)flag;mw(L2W+L5X)flag;iftd(…)silent×3;lli=none`; E2 `implicated=L1C;exonerated=L2W;L5X;basis=
mw(L2W+L5X)silent(n=10;thr=0.2WL);mw(L1C+L2W)flag;mw(L1C+L5X)flag;iftd(L1C+L2W)=flagged`; E3
`implicated=L2W;exonerated=L1C;L5X;basis=mw(L1C+L5X)silent(n=10;thr=1.4WL);mw(L1C+L2W)flag;mw(L2W+L5X)flag;
iftd(L1C+L2W)=flagged;iftd(L1C+L5X)silent;iftd(L2W+L5X)silent;lli=none` — the equal-count trap reproduced on
real data: the scope freed the one band that never moved and left 29 cycles standing on both guilty bands,
where OFF freed all three.  P1 ≡ P0 bit-for-bit on the null and on E1 (the legacy gate admits a −99 L5 slip
on the field: the lane's σ is ~5× smaller than the sim's); P3 ≡ P2 everywhere.

**⚠ DEFECT 2 — why `pairs: all` did not veto E3: the IF-TD reference arc is UNTESTED in the additional
chains.**  `network_if_slip.csv` (P2 null, 60 epochs): the L5-bearing pairs at GOPE/G24 read
`abs_dev_m = 0.000000` at every epoch of the window while their pools fill normally; network-wide, 41
(station, sat) arcs are zero on every row — **all of them on the (L2W, L5·) pair, exactly one per station,
21 L5Q + 20 L5X (8.4 % / 7.4 % of arcs, so not a tracking-code effect)**, and none of them zero in the
companion L1/L2 chain.  Per (station, pair, epoch) group there is exactly one zero-deviation satellite in
92.7 % of groups; pooling both L5 chains, one satellite in 97.6 %.  It is the chain's REFERENCE: the statistic
is ΔOMC(sat) − ΔOMC(ref), zero for the reference itself.  The first chain tests its reference through the
reference arm; the additional chains do NOT re-run it (§14.10.4, the flood guard).  Correction to the naive
reading: the L5 chains adopt the (1,2) chain's reference only when it tracks L5 (46.7 % of station-epochs
shared; of the 53.3 % that differ, 98 % are a (1,2) reference with no L5), otherwise they elect the highest
L5-capable satellite (rank 1 in 80.6 %, rank ≤ 2 in 99.0 %).  At GOPE: (1,2) reference G12 (76.6°, Block
IIR-M, no L5); both L5 chains reference **G24** for k ∈ [−9, +41], then G25.  **So E3 at G24 was confounded —
the (1,5) veto was structurally unavailable, not merely silent — and the dead zone is every station's
highest L5-capable satellite: the best-placed arc of the constellation.**  G25 at GOPE has live L5 deviations
at k = 0 (0.0086 / 0.0044 m against thresholds 0.109 / 0.170 m; an equal +29-cycle L1+L5 slip gives ~3.1 m
of IF deviation, 28× the bar) and becomes the reference only at k = +43.

**The equal-count leg OFF the reference arc (GOPE00CZE / G25, same tow, L1C +29 and L5X +29) — the trap
is CLOSED there by `pairs: all` and OPEN under `pairs: first`.**  P0 freed L1C+L2W+L5X (MW (2,5) + IF-TD (1,2)
drains).  P2f: `keep implicated=L2W;exonerated=L1C;L5X;basis=mw(L1C+L5X)silent(n=10;thr=1.5WL);mw(L1C+L2W)flag;
mw(L2W+L5X)flag;iftd(L1C+L2W)=flagged;lli=none` → **L2W only** (the innocent band; 29 cycles left standing on
both injected bands).  P2: `all reason=no_exoneration;…;iftd(L1C+L5X)=flagged;…;iftd(L2W+L5X)=flagged` → all
three bands freed, three `T1_if_slip` refloat rows (one per pair).  `network_if_slip.csv` GOPE/G25 at k = 0:
L1C/L2W 14.044 m (thr 0.107, flagged), **L1C/L5X 3.1508 m (thr 0.1089, flagged — 29×; the prediction
29 cyc × 0.107 m/cyc = 3.10 m holds to 2 %)**, L2W/L5X 83.17 m (thr 0.171, flagged); under `first` the two
L5 rows do not exist.  The MW (1,5) lane is silent (n=10, thr 1.5 WL) exactly as the trap requires.  The
close-spaced pools are healthy on real data (thresholds 0.109 / 0.171 m — the 12× amplification is absorbed).
Caveat: G25 takes the L5 reference at k = +43, so a future B/C/D grade on this arm stops at +40.

**G-G, the 6 h field twin for the evidence knob (`runs/evseed_field6h_ev` = seed config + `mw_evidence_refloat`,
same exe; manager-launched and graded).**  Exit 0, **720/720 emitted, 0 withheld (= the seed)**; run-end
`legacy 160, evidence 10 [knob on; would-admit 0]; dropped loose 44 / unknown 1168`; every other detector
count identical (lli_slip 205, if_slip 149, eq34 reref 746 / reattach 337; mw_wl_slip 71 → 73; attribution 0
both).  The admissions (`src=evidence` rows): BOGT/G32 L1C;L5X ratio 7.0 n=67 int=unknown · BSHM/G30 L1C;L5Q
4.5 n=349 known · KIT3/G06 L1C;L5Q 5.0 n=56 unknown · SYOG/G26 L2W;L5X 7.7 n=185 known · CHUR/G03 L2W;L5Q
5.1 n=25 unknown — **all on the bias-free L5 lanes, all mature, three with an integer the legacy gate can
never admit.**  Wire (products ON − OFF): satellite clocks ≤ 3 mm at every epoch; four admissions move the
satellite's emitted phase bias ≤ 0.14 m within 30 min; the SYOG/G26 admission **re-realizes the (G26, L5X)
phase-bias datum by 9.2 m in the admission epoch and it is DECLARED** (`disc_counter` 0 → 1 — invariant #7
holds).  Consumer (`ev_consumer_invariant_grade.py`, t0 259200, settled h3–6): pb_reset / cb_disc / clk_disc
**29 / 590 / 6 identical**; stepping-epoch fraction **0.1181 identical**; p90 of the per-epoch max emitted
step 0.127 → 0.257 m (recorded — the consumer runs L1/L2 and did not move); raw p50 6.24 → 6.42 m; de-biased
p50 **5.28 → 5.49 (+4.0 %)**, rms **6.48 → 6.16 (−5.0 %)**; per-hour de-biased p50 3.53 → 3.21, rms
4.42 → 3.90.  **Every G-G bar passes** (withheld equal, consumer within ±5 %, step fraction equal, admissions
identified and on the lanes the rule targets).

**VERDICTS (per the §20 decision rules).**
- `network.cycle_slip.mw_evidence_refloat` — **FIELD PIN.**  G-F: the L5 lanes are 0 % bias-corrected and
  would-admit = 10 in 6 h; G-E: P1 ≡ P0 bit-for-bit on the quiet leg and on E1; G-G: pass on every bar.  The
  fear in the exception text was inverted by measurement.  Field cost signature to watch: an admission on the
  arc that realizes a (sat, signal) phase-bias datum re-realizes the family (declared) — on this lineage there
  is no anchor election to hand it over, so the consumer pays one declared discontinuity per such admission.
- `network.cycle_slip.common_band_scope` + `if_phase_td_pairs = all` — **EXCEPTION, with the mechanism.**
  Single-band narrowing is correct on real data (E1, E2, E5 by the receiver's bits), but the equal-count trap is
  OPEN on every station's IF-TD reference satellite (the highest L5-capable one), where the additional pairs
  cannot veto because the reference arc is untested (DEFECT 2), and on this lineage the wrong narrowing has
  no backstop (T4 attribution structurally 0).  The wire/consumer cost of the trap could not be measured
  (DEFECT 1).  Fix card (stub, not built here): a pair whose IF-TD chain holds the satellite as its reference
  is NOT silent for the verdict (untested ⇒ immature ⇒ all-band on that arc — one clause in
  `NetworkCommonBandScope`, no detector change, no re-opening of the §14.10.4 flood), re-priced on G24 and G25;
  then the field pin rides on a seed that can warm-resume (DEFECT 1's card).
- Both defects are HARNESS findings of the kind the field seed exists for: neither is visible on the sim (the
  sim's S2 leg injected on a non-reference satellite; the sim golden resumes through the F3 coupling path).

### 20.2 RESULTS — FP-1, the §20 matrix on the warm-resuming seed: defect 1 is cured, every identity row reproduces, and the pin is REFUSED by a defect the gates could not see — the reference-slip fan-out (2026-09-13 ~11:30 local; worker on worktree-agent-a8ac220a59ca83a04, exe `gnss_processor_rs7.exe` 23C628B3… = main's producer after RS-7, seed `evseed_field6h_v5` ep0604, legs `runs/evleg_fp1_*`; manager grade)

**Legs.**  {null, E1, E2, E3/G24, E3/G25} × {OFF, PIN = scope + pairs `all` + the §22 clause}; the E5 pair was not
run (slot contention; F3's door is not what decides this card).  Every leg 61/61 emitted, 0 withheld, no cold start,
products + 61 consumer rows — **§20.1's defect 1 is cured on v5**, and G-B / G-C / G-D are readable on the field for
the first time.

**G-A PASS on every pre-registered row.**  E1 PIN → `{L5X}` (`iftd(L1C+L5X)=ref_arc;iftd(L2W+L5X)=ref_arc`); E2 PIN →
`all` (`no_exoneration`, the clause's named cost); E3/G24 PIN → `all reason=no_exoneration` with both L5 pairs
`ref_arc` (§22.4 B2 verbatim — the trap closed); E3/G25 PIN → `all` via `iftd(L1C+L5X)=flagged` (the (1,5) veto).
PIN null run-end `narrowed 7 (bands kept 14), all-band 44 (no exoneration 34 / inconsistent 9 / anchor co-locate 1),
IF-TD vetoes 0, ref-arc refusals 6 [L1C_L5Q 1, L1C_L5X 1, L1W_L5X 2, L2W_L5Q 2]` = §22.4's null to the pair; OFF null
`narrowed 1, inconsistent 15, ref-arc refusals 0 [would-narrow 1]`.  T4 attribution 0 on every leg (structural on
`carry_invariant`), so the wrong-narrowing tripwire cannot fire on this lineage.

**The blocker — the reference-slip fan-out (§22.6).**  GOPE's chains from `is_ref`: (L1C,L2W) → G12 all leg; both
L5 chains → **G24 for k ∈ [−9, +41]**, handing to G25 at +42.  An IF-TD sample is `dOMC(sat) − dOMC(ref)`, so the
injected slip reads `abs_dev = 0.000000` on G24's own L5 rows and full-size on everyone else's:

| leg | G24's own L5 rows | other satellites flagged → refloats | E3/G25 control (G25 not a reference) |
|---|---|---|---|
| E1 PIN (L5X −99) | 0.000000 ×2 | **6** (G11 / G25 / G32 × both L5 chains; 31.8 m at 253–292×, 283.9 m at 605–1580× the threshold) → 6 | — |
| E2 PIN (L1C +37) | 0.000000 ×2 | **3** ((1,5) only — an L1C slip leaves IF(L2W,L5X) alone) → 3 | — |
| E3/G24 PIN | 0.000000 ×2 | **6** (3.16 m at 25–29×, 83.2 m at 177–463×) → 6 | G25's own rows 3.15 / 83.2 m flagged, **0** collateral |
| every OFF arm | — | 0 | 0 |

No backstop: card R's reference gate reads `3600 opp, 1 flag` identically on all four legs (it tests the first chain
only), T4 is 0, and the collateral satellites have no `T0_band_scope` row to narrow.  Cost: blast radius at k = 0 OFF
1 product key (`P/G24/L5X`), PIN 2 — the extra is `P/G32/L1C`, an innocent satellite's phase bias re-realized
network-wide by **13.71 / 13.73 / 13.73 m** on E1 / E2 / E3g24 (declared — disc 1 → 2, σ 0.039 → 0.449 m — so the
declaration edge holds, but the response is wrong); GOPE ZWD at +45 2.02 m PIN vs 0.25 OFF on E1; consumer mean
|Δ3D| over +30..+50 E1 4.71 vs 0.98, E2 3.16 vs 1.18, E3g24 2.80 vs 2.43 — OFF recovers by +15, PIN does not.  Field
exposure: 12.7 % of IF-TD samples sit on a reference arc, 25.9 % of (station, satellite) arcs are a chain reference
at least once over 6 h, and of 149 flagged rows zero are on a reference arc (the detector cannot flag there); under
`all` the exposure roughly triples (§22.4: 107 of 507 arcs).

| event | G-A | G-B | G-C | G-D | fan-out PIN / OFF |
|---|---|---|---|---|---|
| E1 | PASS | FAIL (clk, zwd) | FAIL (PB L2X) | FAIL (4.71 vs 0.98 + 1.99) | 2 / 0 |
| E2 | PASS | FAIL (zwd) | PASS | PASS (3.160 vs bar 3.168) | 1 / 0 |
| E3/G24 | PASS | FAIL (zwd) | FAIL (PB L1C / L1W / L5Q) | PASS | 2 / 0 |
| E3/G25 | PASS | PASS | PASS | PASS | 0 / 0 |

**G-E fails its numeric bars for a defensible reason — the counter-weight.**  Withheld 0 / 0, `attrib_arc` 0 / 0,
but wire CLK max |Δ| 0.088 m (bar 0.02) and consumer max |Δ3D| 4.43 m (bar 0.03).  The floor series is EXACTLY zero
on every channel through k = +38; at +39 (tow 278790) JPLM00USA/G10 shows `L1C/L5Q dd = 5.046 m` against a 0.109 m
threshold on a 189-sample mature pool → flagged and refloated in PIN, while in OFF the same dd appears with
`count = 0, med = dd, abs_dev = 0, flagged = 0`: G10 lost L2W that epoch, its "first pair" rotated to (L1C,L5Q), and
the new chain restarted with the slipped sample as its own origin.  Every other satellite at JPLM is quiet (≤ 0.004 m)
and the reference G28 is quiet: **a real 46σ slip that `all` catches and `first` structurally cannot** — a
pool-restart blind spot at pair rotation, the second defect this card names (under `first` the additional chains'
histories do not exist; under `all` they are warm).

**Decision.**  Scope + pairs `all` + the clause stay FIELD EXCEPTIONS with the fan-out as the mechanism (the manifest
texts are rewritten through the generator; the ledger rows carry this section); G-G not run.  The pin is re-run
(§20's bars on the fixed posture) once REF-FAN (§22.6) passes F1–F4 — and the pair-rotation blind spot is the
argument FOR `all` once the fan-out is closed.  Graders merged: `scripts/fp1_gate_table.py`,
`scripts/fp1_refslip_census.py`.

### 20.3 RESULTS — FP-2, the matrix on the FIXED posture: the 60-epoch matrix is a clean sweep, the 6 h twin refuses the pin on the response cost (2026-09-13 ~23:20 local; worker on worktree-agent-a5c519cce79cf30c2 `5800768`, merged; exe = the certified `AF418A9F…`; seed `evseed_field6h_v8` ep0604; harness `e6071f43…` unmoved for the batch; manager grade)

**Legs.**  {null, E1, E2, E3/G24, E3/G25, E5} × {OFF, PIN = scope + pairs `all` + the clause + `if_phase_td_ref_arm_all`},
all 12 clean (61/61, no cold start, RS-8 / RS-9 restores on, the four ARMED lines present on every PIN leg, every pair's
config diff = the four knobs + paths).

**G-A PASS on every row** (E1 `keep implicated=L5X` with both L5 pairs `ref_arc`; E2 `all reason=no_exoneration`, the
clause's cost; E3/G24 `all` via `ref_arc`; E3/G25 `all` via the (1,5) veto, its three detections bit-identical to §22.7's;
E5 `keep implicated=L5X` via `lli=L5X`), T4 attribution 0 everywhere.  **The fan-out census: 0 collateral on every G24
event** (FP-1 read 6 / 3 / 6), the reference's own arcs reset instead (2 / 2 / 3 / 2), the innocent 13.7 m re-realization
absent; blast radius at k = 0 **1 key in both arms on every event**; the per-chain counter prices it (`fan-out suppressed`
3 → 9 on `e1_pin`, FP-1's six).  **G-B PASS 5/5** (every channel within 9 mm at a zero floor; E1 better on clock and iono);
**G-C PASS 5/5** (≤ 0.017 of the 0.123 m CLK floor); **G-D PASS 5/5** with the floor (cons_mean floor 4.29 m) — at a ZERO
floor E1 would be 6.5× better (2.34 → 0.36), E3/G24 better, E3/G25 a tie, **E2 and E5 worse (+0.65, +1.39 m mean)**.  The
two re-reads: the E3/G24 station clock at +45 is +1.4917 OFF / +1.4918 PIN (REF-FAN's miss does not reproduce as an arm
difference); the E5 consumer max reproduces in direction, smaller (2.75 → 3.61, +31 %).  **G-E:** withheld 0 / 0 and
`attrib_arc` 0 / 0 PASS; the numeric bars are not gradeable — the two nulls are exactly zero on every channel through
k = −8 and diverge at **k = −7** on §22.7's natural `G27/SCTB00ATA` reference fire, then PIN catches what OFF structurally
cannot (MIZU/G06 at +21; **JPLM/G10's 46σ (1,5) slip at +39, which OFF carries on the wire for 8 epochs** before its own
156 m detection at +47; VOIM/G18 at +47).  Every phase-bias step > 0.5 m is DECLARED in all 12 legs (a new wire-step
census, `scripts/`); the undeclared steps are the known clock-channel gap (`emit_clock_disc` off), identical in both arms.

**G-G the twin (`evseed_field6h_v8fp2`, 6 h, the four knobs) — FAIL.**  PASS: 720 / 720 emitted = v8, `attrib_arc` 0 = 0,
**fingerprint `545b8bdd2b638231` = v8's (none of the four knobs is fingerprinted)**, the F1 flood bar at scale
(additional-chain fires 12 own + **20 inherited** = 32 vs the first chains' 21; `L2W_L5X` 704 candidates → 4 fires) — **the
adopted-reference path's first field validation**, the half §22.7 left open.  Work done: scope narrowings 23 → 55,
ref-arc refusals 0 → 143 across 10 pair kinds, pairs evaluated 340 k → 689 k.  FAIL: consumer de-biased settled h3–6 p50
**+18 % (mean-debiased) / +22 % (median)**, rms **+31 / +33 %** (bar 5 %); `pbstep` epochs carrying |step| > 0.05 m 0.118 →
**0.151 (+28 %)**, p90 max step 0.257 → **0.733 m** (bar: not worse); per-hour the PIN consumer is BETTER in h0–2 and worse in
h3–5.  Mechanism: **+107 refloats over 6 h**, each a declared phase-bias re-realization on the wire that the float,
undifferenced WTZZ consumer re-converges through — the detector side is right (it catches real slips `first` cannot see)
and the RESPONSE is what costs.  Neutral: `pb_reset` 29 = 29, `clk_disc` 6 = 6.  Anomaly worth its own leg: the first
chain's OWN decisions drift under `all` at 6 h (`L1C_L2W` 113/110 → 107/104, `L1C_L2X` 15/15 → 8/8) though identical over 60
epochs — §14.10.4's "pure addition" holds structurally, not after slow state feedback; a single-knob 6 h leg would
attribute it.  Also: a PIN-only ≈ 1.9 m declared `P/G30/L2W` re-realization at a leg-dependent epoch (a marginal trigger,
not an event response); the 0.5 m consumer shock instrument is saturated (600 / 600) on this consumer.

**Decision.**  The four stay FIELD EXCEPTIONS, now with the measured mechanism (the manifest texts rewritten through
the generator; the four ledger rows carry this section).  What would change it: a cheaper slip response — the
lane-preserving refloat (SPEC_lane_slip_response's open build) or an emitted-level-preserving re-tap — or a consumer that
carries its state across DECLARED steps; when either lands, the G-G twin is re-run on that posture and all four are
pinned together.  The fan-out fix itself (REF-FAN) stands validated on both beds and pinned on the sim golden.

## 21. THE FIELD WARM-RESUME SEAM (card RS-2) — the restored invariant DD branch is folded in the WRITER's linearization frame; §20.1's "Card 1f is the prerequisite" is WITHDRAWN (pre-registered 2026-09-12 ~07:30 local, manager; before any new leg is read)

### 21.1 What §20.1 called DEFECT 1, re-read from the instruments the legs already wrote

§20.1 attributed the field golden's inability to warm-resume (every leg from `runs/evseed_field6h`
ep0604 withheld 61/61) to the missing Card 1f `carry_coupling_gauge_project`, on the strength of the
loader's own text at `GnssConfigLoadNetwork.cpp:3602–3609`.  That attribution is withdrawn:

* **1f is OUT OF REGIME on this lineage.**  Ledger row `network.carry_coupling_gauge_project`:
  `regime: on("network.carry_conditional_coupling") and not on("network.carry_invariant")`; the
  field golden runs `carry_invariant: true`, and `ApplicationNetworkAssembly.cpp:3117` gates the
  whole leveling inject on `do_amb_carry = (…) && !carry_invariant` — the Card-1 coupling rows the
  loader text is about are never built here (the 1g ledger row says the same for every
  `carry_invariant` lineage: "structurally INERT").  Arming 1f would append a fingerprint token and
  change nothing.  The `resume_coupling_restore` interlock that names 1f is therefore not a
  diagnosis of this seam; it is an interlock for a different vehicle.

* **The first post-resume solve is torn apart by ONE prior class.**  The rhs-attrib flight
  recorder fired on the SH-gate trip at the leg's first epoch (`runs/evleg_fld_null_off/logs/
  rhs_attrib/attrib_277320.csv`, TOTAL rows, `norm_total`):

  | class | norm_total | c00_l1 / c00_l2 |
  |---|---|---|
  | **dd_fold** (the restored invariant DD carry's prior) | **7.82e7** | **−1377.05 / −3471.85** |
  | pivot_pin | 2.44e6 | −1.56 / −1.11 |
  | cpl_row (the 61 ZWD coupling rows) | 5.14e5 | 1.01 / 0.58 |
  | shared:d2_leftover | 8.51e3 | 0 / 0 |
  | obs_code — every code observation of 61 stations | 616.0 | 0.72 / 0.46 |
  | obs_other | 233.8 | |
  | shared:d2_sh / iono_sh_row | 190.3 / 121.1 | |
  | obs_phase | 0.0008 | |

  Five orders of magnitude above the data, and it drives a₀₀ directly (the c00 columns).  Epoch
  status at 277320 (leg vs the seed at the same tow): a₀₀ **−11109.1** vs **+38.33 ± 0.80** TECU,
  `clock_std_m` **2819.5** vs 9.88, `sigma0_pf_code` **17218.8** vs 4.886, `sigma0_pf` 1564.7 vs
  0.137, worst sat G20 6868.7 m; `n_d5_flags` **676 / 676 re-taps** (seed: 0), `n_chord_births` 64;
  at the second epoch **674 births / 672 slip refloats** (the raw detectors, now with two samples,
  erase the branch wholesale), `clock_std` still 2681 m.  The SH gate never releases: a₀₀ −1350 at
  +20, −1162 at +40, −1198 at +60.  The prior-residual census over the leg: `zwd` σ̂₀ **12515**
  (worst |ê| **4177 m** at SGOC00LKA against `fold_var` 7.1e-5), `cb` 1604, `rxcb` 84.6; the rx-CB
  carry withheld 3624 of 3660 priors at its 10σ gate.  The plain RTB-7 leg (`evleg_fld_null_plain`,
  no frame restore) is the same failure: a₀₀ −14113.6 at the first epoch, one-epoch releases at
  +40/+42 (145.2 / 82.6 TECU), re-engaged (352.6, −94.2), 6 epochs emitted.

* **Restore mechanics were textbook in every leg** (`stdout.txt:414–418`): 256 D2 shared-carry
  keys (restored DIAGONAL — "the full-covariance carry re-seeds at the first emitted epoch"), 0
  ambiguity carries (none exist under `carry_invariant`), 61 ZWD Z rows, **701 carried DD restored
  branch-faithfully** (RTB-7c, σ̂₀ seeded 2.8259, Pᶻ dense v2 blob, binding validated), 126
  phase-anchor incumbents + 32 CLK-R held-frame keys, snapshot age 30 s, generation id preserved.
  The blob at ep0604 is `CARRY,v2,4` (4 blocks, 701 chords).

### 21.2 The mechanism (read from the code; the pre-registered legs test it)

`NetworkInvariantCarryDriver::serializeCarry` (`NetworkInvariantCarryDriver.cpp:182`) writes per
block the `Z` row (chord means ẑ), the `P` rows (Pᶻ), and the `H/X/A` topology rows — **nothing
about the linearization**.  The chord mean is the CORRECTION-frame double difference:
`SPEC_warm_resume_event_card.md` §4 already states "the serialized `z` is the CORRECTION-frame DD
(λ·N minus Σc·x_lin), hence run-specific" — and drew from it only the conclusion that the
branch-agreement METRIC was invalid across runs.  The stronger consequence was never drawn: **a
resumed run re-seeds every station's ambiguity `x_lin` from code at its first epoch
(`cold_clock_seed` + the code-minus-phase seed; the station layer is not snapshotted), the DD prior
fold reads THAT `x_lin` per arc (`NetworkDdPriorBuild.cpp:254–266`, `lin_of`) and forms
`v = z_free − Gᵀx_s − ẑ` (driver `:1770`) — so every restored ẑ is wrong by Σc·(x_lin,writer −
x_lin,now)**: the DD of code-seeded ambiguity values taken hours apart, metres per chord, folded at
q_z = 1e-4 m²/s stiffness on 701 chords at once.  The D5 innovation gate is POST-solve, so it can
only flag the damage (676 flags), not prevent the fold; and `carry_invariant_innov_corroborate`'s
"no evidence still re-taps" keeps the levels for the next epoch anyway.  By the second epoch the
raw detectors erase the branch — but the shared memory (the SH carry, the D2 clocks/biases, the
rx-CB carry) has already been stashed from the garbage solve, which is why nothing heals: the
withhold era is self-sustaining.

**Why this was invisible on 2026-08-02 (`ft63_warm12`, "resume was textbook, the two arms are the
same run after ~3 epochs").**  `config/test_suite/_ft63_warm12.json` arms `carry_invariant` +
`state_snapshot_carry` + the D5 gate but **not `carry_invariant_pd_safe`** — it ran the OLD fold,
behind which "78 % of the carried state was measured diffusing" under a silent M:=0 freeze
(`SPEC_pd_safe_dd_fold.md`).  A frame-wrong branch that mostly cannot bite produces exactly what was
recorded: a 91 m re-entry spike that dies in 2–3 epochs, 587 of 769 restored chords reborn in the
first two epochs, and a "benefit window" of ~3 epochs.  The golden pins the PD-safe fold, so the
restored branch now bites at full stiffness.  **Interpretation, not yet a measurement** — it is
consistent with every number above and is the reason the RS card's "branch-faithful" PASS cannot be
carried over to the golden posture; it is NOT graded here (pd_safe is fingerprinted and cannot be
A/B'd on a leg).

### 21.3 Pre-registered legs — Phase 1, on the EXISTING seed (no new seed, no code change)

All legs: `runs/evseed_field6h` ep0604 (tow 277290; fingerprint 545b8bdd2b638231), 60 epochs
(tows 277320…279090), NULL (no injected event), one exe built from main at or after eb37b24,
`--extra` always carrying `"resume_coupling_restore": false` (the loader throws otherwise on this
lineage), run one at a time.  Every discriminator below is fingerprint-EXEMPT (checked against the
token list `NetworkRunContext.cpp:644–978`): `state_snapshot_carry` is the read-side gate of the
blob restore (`:1724`) and not a token; `sh_health_gate` and `zwd_seam_reseed` and
`resume_frame_restore` are QC / forward-only resume policy.

| leg | knobs beyond the standard leg | question |
|---|---|---|
| L0 `rs_null_ctl` | none | same-exe control: must reproduce §21.1 (61/61 withheld, a₀₀ < −5000 at +1, `dd_fold` TOTAL > 1e7) |
| **L1 `rs_null_nocarry`** | `state_snapshot_carry: false` | **the blob is the cause**: shared / Z / frame restored warm, the DD branch cold-starts |
| L2 `rs_null_plain_nocarry` | `state_snapshot_carry: false`, `resume_frame_restore: false` | is the frame restore needed for availability on the field, or only for the seam's PB continuity |
| L4 `rs_null_shgateoff` | `sh_health_gate: false` (blob restored) | the deadlock question: with the wire allowed to emit, does a₀₀ return to the seed's band by +60 (the era is the gate's) or not (the pollution is in the carried memory) |

**Gates (bars frozen now).**  The seed's own traces at the matched tows are the continuous
reference (`network_epoch_status.csv`, `network_vtec_sh.csv` layer-0 (0,0) = a₀₀, `network_zwd.csv`,
`products.txt`, `closed_loop_trace.csv` — 61 consumer rows exist in the window).

* **G1 (control, L0):** as above.  A control that does not reproduce voids the phase.
* **G2 (availability, L1):** emitted ≥ 55 of 61; any withheld epoch lies in +1…+5.
* **G3 (frame, L1), every epoch from +5 on:** |a₀₀,leg − a₀₀,seed| ≤ 5 TECU; `clock_std_m` ≤ 2×
  the seed's; `sigma0_pf_code` ≤ 2× the seed's.
* **G4 (branch cost, L1):** chord births per epoch from +5 on ≤ 5× the seed's rate at the same
  tows; the D5 flag count at +1 is recorded (structurally ~0 without a restored branch — the number
  that matters is Phase 2's).
* **G5 (wire + ZWD continuity, L1), from +5 on, leg − seed at matched tows:** satellite clock
  (per-epoch median over satellites removed, the datum-invariant contrast) p50 |Δ| ≤ 0.05 m and
  p95 ≤ 0.20 m; W6 code bias p50 |Δ| ≤ 0.02 m; ZWD p50 |Δ| ≤ 5 mm, p95 ≤ 20 mm; phase-bias
  re-realization at the seam either DECLARED (the emitter's disc counters bump) or absent — a
  silent |ΔPB| > 0.05 m on more than 10 % of keys at any epoch FAILS (invariant #7).
* **G6 (L2 vs L1):** recorded, no bar: whether the plain resume passes G2/G3 too, and the PB-side
  seam step with vs without the frame restore.
* **G7 (L4):** recorded: a₀₀ at +60 within 5 TECU of the seed ⇒ the era was the gate's; otherwise
  the carried memory's.
* **Consumer:** recorded, not gated in Phase 1 — the closed-loop consumer is not snapshotted and
  its own cold start dominates a 30-minute window (§18.14: grade seams with a cold-consumer replay).

**Decision rule.**  L1 passes G2–G5 ⇒ the blob is the whole seam defect; the INTERIM harness
posture for field legs becomes `state_snapshot_carry: false` in `--extra` (shared/Z/frame warm, DD
branch cold — "RTB-7(a)"), which unblocks the owed field price of the §20.1 wrong narrowing and the
scope+pairs pin work; Phase 2 builds the real fix.  L1 fails G2 or G3 ⇒ a second class is torn:
re-read the L1 first-epoch rhs-attrib TOTAL table (the recorder fires on the trip) and bisect the
remaining restored classes (Z rows via `zwd_seam_reseed: false`; frame rows via L2) before any build.

### 21.4 Phase 2 — the fix, pre-registered (built only after Phase 1 is graded by the manager)

**Design: re-base the restored branch, do not re-seed the stations.**  The carry must live in the
absolute value (the standing `x_lin`-shift-is-a-solve-no-op principle: the prior's RHS is formed
against the CURRENT linearization).  Two additions, both default-OFF and byte-identical when off:

1. **Write side, `network.state_snapshot_carry_lin` (default false).**  At every harvest the driver
   also stores per chord `zl = Σc·x_lin` (the same incidence that forms `z_free`, applied to the
   arcs' current `x_lin` — `NetworkDdPriorBuild` already has `lin_of` per arc), and
   `serializeCarry` writes one `ZL` row per block beside `Z` (format-forward: a reader without the
   row behaves exactly as today; snapshot bytes change only when armed).  Products and every trace
   byte-identical (write-side only, the frame-capture precedent).
2. **Read side, `network.resume_carry_rebase` (default false; forward-only resume response policy,
   fingerprint-EXEMPT like `resume_frame_restore`).**  A restored block whose `ZL` row is present is
   marked UNBASED; at the FIRST fold after the resume the prior build computes `zl_now` for every
   restored chord from the epoch's `lin_of` and the driver re-bases `ẑ ← ẑ + (zl_writer − zl_now)`
   (Pᶻ unchanged — an exact frame translation).  A chord with ANY arc absent at that epoch cannot be
   re-based and is DROPPED (marginalized) rather than folded frame-wrong — counted.  Armed without
   `ZL` rows in the blob ⇒ loud "branch NOT re-based" warning and today's behaviour; run-end line
   `n_resume_carry_rebased / n_resume_carry_dropped_unbased / max |Δẑ|`; the D5 flag count at the
   first post-resume epoch is the invariant-#4 readout.

`scripts/event_leg.py`: write-seed arms `state_snapshot_carry_lin`, make-leg/null arm
`resume_carry_rebase` (beside `resume_frame_restore` / `resume_coupling_restore`), both recorded in
the meta files.  Unit tests: a two-block driver round-trip with a shifted `x_lin` restores the
identical absolute DD; a chord missing an arc is dropped and counted; a blob without `ZL` restores
today's ẑ untouched.

**Gates (frozen now; graded on a NEW 6 h field seed written by the fixed exe with the write knob —
the new field harness seed `runs/evseed_field6h_v2` — against its own traces and against the
no-blob leg L1′ from the same seed):**

* **P1 byte-identity:** the fixed exe with both knobs OFF reproduces the seed-writer path and the
  L0/L1 legs byte-for-byte on `products.txt` (and the snapshot bytes without the `ZL` rows); write
  knob ON: products and every trace byte-identical to OFF, snapshot differs only by `ZL` rows.
* **P2 the seam is closed, from +1 on (not +5):** D5 flags at +1 ≤ 10 (was 676); `dd_fold` TOTAL
  norm at +1 within 10× of the same leg's +10 value (was ≥ 1e5×); emitted ≥ 58/61; |a₀₀ − seed|
  ≤ 2 TECU, `clock_std` ≤ 1.5× seed, `sigma0_pf_code` ≤ 1.5× seed; G5's wire/ZWD bars from +1.
* **P3 the branch is worth restoring (rebased leg vs L1′ no-blob leg, same seed):** chord births
  over +1…+5 ≤ 20 % of L1′'s; the satellite-clock contrast and code-bias |Δ| vs the seed at +1…+5
  ≤ L1′'s; silent PB steps at the seam ≤ 1 % of keys; consumer p50 over +5…+60 ≤ L1′'s.
  P3 failing with P2 passing = the branch restore is correct but worthless on this lineage — then
  the harness posture stays `state_snapshot_carry: false` and the ledger row says so.

**Sequel (not this card):** with a field seed that warm-resumes, the §20.1 wrong-narrowing price
(gates G-C/G-D, dark in §20) becomes measurable, and the reference-arc clause + the scope/pairs
field pin follow on that seed.
### 21.5 RESULTS — Phase 1 (the blob is the seam), the Phase 2 build merged, and the P2/P3 bars re-calibrated BEFORE the Phase 2 legs are read (2026-09-12 ~10:00 local; worker on worktree-agent-a2e7ed83282f533a8, manager re-grade with `scripts/ev_seam_grade.py`)

**Exe** `gnss_processor_rs2.exe` = main `cf08909` (sha256 `a35d2254…`; the worker's worktree was 147 commits
behind and was fast-forwarded first — the stale-branch-exe trap, caught).  Legs `runs/evleg_rs_null_{ctl,
nocarry,plain_nocarry,shgateoff}`, 60 epochs from ep0604, all `exit 0` with the same RTB-7 restore line
(256 D2 / 0 ambiguity / 61 Z / 1 pivot baseline; n_emitted 604).  Grader: `python scripts/ev_seam_grade.py
<legdir> --seed runs/evseed_field6h` (absolute paths — it resolves relative ones against its own repo).

| gate | L0 ctl (blob) | **L1 no-blob** | L2 plain, no-blob | L4 blob, SH gate off |
|---|---|---|---|---|
| G1 control | reproduces §21.1: 60/60 withheld, `dd_fold` 7.823e7, D5 676 — a₀₀(+1) **−4810.41** on `network_vtec_sh.csv` vs **−11109.10** on the SH gate's own log line (§21.1 quoted the gate; the grader prints both) | — | — | same first epoch as L0 |
| G2 availability | FAIL 0/60 | **PASS 60/60** | PASS 60/60 | PASS 60/60 (gate off) |
| G3 frame, from +5 | FAIL: \|Δa₀₀\| 1095.7 TECU, clock_std ×330.8, σ̂₀_code ×397.5 | **PASS: 2.25 TECU, ×0.876, ×1.053** | PASS: 1.64, ×0.804, ×0.880 | FAIL (as L0) |
| G4 births +5..+60 | 321 vs seed 296 (1.08×); D5 at +1 = 676 | **262 (0.89×); D5 at +1 = 0** | 302 (1.02×); 0 | 321; 676 |
| G5 wire + ZWD | NA | FAIL — see the calibration below | FAIL | FAIL: clock contrast p50 763 m, code bias 385 m |
| G7 a₀₀(+60) vs seed 37.47 | −471.49 | **36.56 (Δ −0.91)** | 38.58 (+1.11) | **−471.49 (Δ −508.96)** |
| consumer p50 from +5 (report) | no rows | 5.07 m (seed 8.83) | 10.51 m | 3487 m |

**What the four legs establish.**
1. **The blob is the seam defect** (L0 vs L1): dropping only the invariant DD carry restore turns 0/60 into
   60/60, a₀₀ tracks the seed to ≤ 2.25 TECU from +5 on, clock scatter and σ̂₀ sit at the seed's, D5 flags at
   +1 go 676 → 0, and the branch rebuilds cold (701 births at +1 — exactly the chords it declined to restore).
2. **The era is the carried memory's, not the gate's** (L4): with the SH gate off and the blob restored the
   wire emits garbage to +60 (clock contrast p50 763 m, consumer 3487 m, a₀₀ −471 at +60).  The withhold is
   protective — it is the only thing between the consumer and 61 epochs of poisoned product.
3. **The frame restore owns the PB-continuity half, not availability** (L2 vs L1): G2/G3 identical without
   it, but the PB seam step doubles (median ΔPB +5.41 m vs +1.79 m) and the consumer is worse (10.5 vs 5.1 m).

**G5 read correctly — two of its sub-bars were mis-calibrated, one is a real finding.**
* The clock sub-bar (p50 0.05 / p95 0.20 m) is TIGHTER than the seed's own epoch-to-epoch datum-invariant
  clock contrast (p50 **0.0907** / p95 **0.4034** m): no leg can pass it on this lineage.  The code-bias bar
  (0.02 m vs own step 0.0097) and the 5 cm PB threshold (own step 0.0025) are calibrated.
* `network_zwd.csv`'s `zwd_m` is not a physical ZWD on the field golden — the seed alone spans −30.5 … +13.5 m
  at one station over 6 h and −24.5 … +23.8 m across stations at one settled epoch (the field ZWD sink,
  `project_zwd_sink_census`): the 5 mm / 20 mm bars are sim-calibrated and mean nothing here.
* The real finding: **the no-blob resume re-realizes the phase wire ONCE, declared, and then does not converge
  to a well-defined wire.**  Each emitting leg writes exactly one `R` record at +1 naming 31 satellites
  (invariant #7 satisfied); the offset persists (per-satellite-median-removed |ΔPB| vs the seed p50 **9.4 m**
  at +2 — a genuinely different branch per (sat, signal), not a datum shift); the two no-blob legs disagree with
  EACH OTHER by 8.4 m p50; and the leg−seed clock contrast GROWS 0.89 → 3.41 m over the 60 epochs while each
  leg's own epoch-to-epoch continuity matches the seed's (L1 0.12/0.92 m).  The literal §21.3 clause reads
  FAIL because that declared step persists past +5; a cold branch is not a branch restore.

**Decision (per §21.3).**  L1 passes G2–G4 and G5's calibrated halves; the interim harness posture for field
IDENTITY legs is `state_snapshot_carry: false` in `--extra` (shared / Z / frame warm, DD branch cold), used by
§22's legs.  It is NOT a posture for grading absolute wire continuity across the seam — that is Phase 2's job.

**Phase 2 — built and MERGED (main `2c62724`; worker commits `62303d2` + grader `b903b66`).**  As pre-registered
in §21.4 with three deviations the worker flagged and the manager accepted: (i) `lin_of` in
`NetworkDdPriorBuild.cpp` lives inside the lane machinery (`mw_mach_on`, off on both goldens) — the arc's
`x_lin` is taken from the station state vector on the path that runs (`ArcInput::x_lin/x_lin2`, read only under
the two knobs); (ii) the writer's spanning tree is not serialized, so the re-base cannot recompute the writer's
cycles per chord — it rides the cycle-basis projection `W` the build already forms, `ẑ_new = W·ẑ_old +
(W·zl_writer − zl_now)`, which is frame-blind (`Z_new = W·Z_old` in arc space) and reduces to the §21.4 rule
exactly when the tree is unchanged; applied to the EMITTED block, never to `carried_`, so a RAIM rebuild is
idempotent; (iii) "a chord with an absent arc is dropped" is the existing birth path — `n_resume_carry_dropped_
unbased` counts the case with no exact re-base (a restored contributor with no `ZL` row).  Knobs
`network.state_snapshot_carry_lin` (write; disarmed without `state_snapshot_carry`) and
`network.resume_carry_rebase` (read; forward-only resume policy, NOT fingerprinted); one `ZL` row per block
after `Z`; run-end `Network (RS-2 linearization carry): …`; a "Linearization frame: …" token on the RTB-7c
restore line; unit suite `ResumeCarryRebaseTest` (4: per-arc-shift round trip with the identity control,
un-basable ⇒ diffuse birth + counted, legacy blob untouched, write-off ⇒ no `ZL`).  Debug suite 1968 / 0 fail.
**P1 on the worktree build: PASS** — 20-epoch field-golden smoke, `products.txt` `b9da8cc1…` in both arms,
the snapshot differs only by the four `ZL` rows (+ the write timestamp), and the warm-resume fingerprint is
**unchanged, `545b8bdd2b638231` = the existing seed's** (a `ZL`-bearing snapshot is chain-compatible).
Main's own rebuild must reproduce the same hashes before the Phase 2 legs count (owed to the worker now).

**P2 / P3 bars, RE-CALIBRATED and frozen now (the Phase 2 seed `runs/evseed_field6h_v2` is being written;
legs `rs2_null_{rebase,norebase,nocarry}` from its ep0604, one exe).**  The yardstick is the seed's OWN
epoch-to-epoch step over the leg window, computed by the grader for each channel (own-step p50 / p95):
* **P2a seam closed, from +1 on (not +5):** emitted ≥ 58/61; |a₀₀ − seed| ≤ 2 TECU; `clock_std` ≤ 1.5× and
  `sigma0_pf_code` ≤ 1.5× the seed's at every epoch; D5 flags at +1 ≤ 10 (was 676); `dd_fold` TOTAL at +1
  within 10× of the same leg's +10 value (the recorder must be forced on if it does not trip — otherwise this
  sub-bar is read from the D5 count alone).
* **P2b wire, leg − seed at matched tows, pooled +2..+60:** clock contrast p50 ≤ 3× own-step p50 and p95 ≤
  3× own-step p95; W6 code bias p50 ≤ 3× own-step p50; ZWD graded by the same own-step rule, not the mm bars.
* **P2c phase wire at the seam (the discriminator):** the `R` record at +1 names ≤ 3 satellites (no-blob: 31);
  per-satellite-median-removed |ΔPB| vs the seed at +2 p50 ≤ 0.05 m (no-blob: 9.4 m); no DRIFT after +2:
  |Δ(k) − Δ(+2)| ≤ 0.05 m on ≥ 90 % of PB keys at every epoch to +60.
* **P3 the branch is worth restoring (rebased vs `rs2_null_nocarry`, same seed):** births over +1..+5 ≤ 20 %
  of the no-blob leg's; leg−seed clock contrast p50 at +60 ≤ the no-blob leg's at +5 (no growth); consumer p50
  over +5..+60 ≤ the no-blob leg's.  `rs2_null_norebase` (blob, re-base off) must reproduce the L0 failure on
  the new seed — the control that proves the fix is the re-base and not the seed.
P2a passing with P2c failing ⇒ the frame is right but the branch content is not (a second-order seam);
P3 failing with P2 passing ⇒ correct but worthless on this lineage — then the harness stays no-blob and the
ledger row says so.

### 21.6 RESULTS — Phase 2: the re-base is exact and total, and it is NECESSARY BUT NOT SUFFICIENT; RS-2b pre-registered (2026-09-12 ~12:30 local; seed `runs/evseed_field6h_v2`, exe `gnss_processor_rs2seed.exe` 0fe4b64e… = main 2c62724's tree; legs `runs/evleg_rs2_null_{rebase,norebase,nocarry}` from ep0604, one exe; manager re-grade with `scripts/ev_seam_grade.py`)

**P1 on main's own rebuild: PASS** — `gnss_processor_rs2main.exe` (520c1106…, main after the RS-2 merge)
reproduces `products.txt` `b9da8cc1…` in BOTH smoke arms; the snapshot differs only by the four `ZL` rows;
fingerprint `545b8bdd2b638231` in both.  **The v2 seed:** 723 synced / 720 emitted / 0 withheld, 12 snapshots,
ep0604 at the same tow as v1's (277290) with 4 `ZL` rows and the SAME fingerprint — the write knob is
non-fingerprinted on a real 6 h lineage.  The only substantive config delta from v1 is
`cycle_slip.mw_evidence_refloat: true` (the §20.1 field pin, promoted after v1 was written), so v1 and v2
legs are not cross-comparable; every comparison below is within v2.

| | **rebase** (blob + re-base) | **norebase** (blob, re-base off) | **nocarry** (no blob) |
|---|---|---|---|
| disposition | **61/61 emitted** | **61/61 `sh_gate_withheld`** | 61/61 emitted |
| G3 from +5: worst \|Δa₀₀\|; clock_std ×; σ̂₀_code × | 2.73 TECU @+7; 0.902; 1.126 | 1095.7; 330.8; 397.5 | 2.25 @+43; 0.876; 1.053 |
| a₀₀(+60) − seed | −0.61 | −508.96 | −0.91 |
| rhs-attrib at +1 | no trip | `dd_fold` 7.823e7 (v1's table to 4 s.f.) | no trip |
| RS-2 run-end | write ARMED / read ARMED; ZL on 4 blocks; **re-based 637 rows**, max \|Δẑ\| **411 715 m**, 0 un-basable | read off; re-based 0 | no blob |
| chord births +1 / +2 / +3 / +4 / +5 | 64 / **305** / 4 / 8 / 0 | 64 / 674 / 8 / 16 / 6 | 701 / 7 / 5 / 8 / 0 |
| D5 flags / re-taps at +1; +2 | **301 / 301**; 0 / 0 | 676 / 676; 4 / 4 | 0; 1 |
| R records | +1: 30 sats (the only one) | none (nothing emitted) | +1: 30 sats (the only one) |
| per-sat-median-removed \|ΔPB\| p50 at +2 / +60 | 14.05 / 13.79 m | NA | 9.98 / 9.65 m |
| clock contrast p50/p95 at +5; at +60 | 2.95/7.49; 4.00/10.92 m | NA | 1.68/6.91; 3.41/9.03 m |
| pooled +5..+60: clock p50/p95; CB p50; ZWD p50/p95 | 3.64/11.75; 0.669; 2.14/9.43 | NA | 2.50/10.49; 0.675; 4.12/14.36 |
| consumer p50 from +5 (seed 8.94) | 5.18 m | no rows | 5.05 m |

Seed yardsticks (own epoch-to-epoch step, tows 277320–279090): clock contrast 0.0906 / 0.4030, W6 code bias
0.0097 / 0.0390, phase bias 0.0027 / 0.0076, station ZWD 0.107 / 0.569 m (p50 / p95) — within 2 % of v1's.

**What is established.**
1. **The re-base is the fix for the catastrophe, single-knob and total:** same seed, same exe,
   `resume_carry_rebase` alone takes the resume from 0/61 to 61/61 with a₀₀, clock scatter and σ̂₀ at the
   seed's.  `norebase` reproduces §21.1 to four significant figures on a freshly written seed.
2. **The wiring is complete, not silently partial:** 637 re-based + 64 not carried = 701 restored chords; the
   64 are exactly the chords that fail to carry at +1 in BOTH blob-restoring legs (arc churn at the seam); 0
   un-basable.  The correction scale — max |Δẑ| 412 km — is the code-seeded linearization's distance from the
   writer's converged one, the same order as the 256 km cold-start leak `cold_clock_seed` addresses.
3. **P2a / P2c / P3 FAIL as pre-registered.**  D5 flags at +1 are 301, not ≤ 10 (676 halved, not removed);
   305 chords are reborn at +2; the emitter still re-realizes 30 satellites at +1; per-sat-median-removed
   |ΔPB| at +2 is 14.05 m, not ≤ 0.05; and on every pooled wire channel except ZWD the re-based leg ends
   FURTHER from the seed than the no-blob leg (clock 3.64 vs 2.50 m, PB 13.8 vs 9.7 m), the two emitting legs
   differing from each other by 1.5 m (clock) and 4–5 m (PB).  The interim harness posture stays
   `state_snapshot_carry: false`.
4. **The residual is a first-epoch DISAGREEMENT between the restored branch and the cold station layer, not a
   frame error the re-base missed.**  The 301 flagged chords (`network_d5_flags.csv` at +1): |v| p10 4.8 /
   p50 11.0 / p90 27.1 / max 104 m against a modelled `var_v` p50 0.33 m² (σ 0.58 m) — a 19σ disagreement, on
   L2W (171) AND L1C (113) AND L1W (17), across 51 stations.  For comparison the seed's own 52 D5 flags in the
   same window have |v| p50 91 660 m with diffuse `var_v`: they are the born-chord artefact the D5 v5 note
   describes (`carry_invariant_innov_carried_only` is NOT armed on the field golden — hygiene item), so the
   leg's +1 flags are the first carried-chord innovations this lineage has produced at a seam.  Two readings
   remain open and they are opposite: (a) the branch is right and the single-epoch data-only DD is wrong by
   metres because the station layer's memory (rx-CB carry, the T5 iono deviation, the PB realization) is not
   in the snapshot — then holding the branch through the first epochs lets the station layer converge around
   it; (b) the branch is inconsistent with this run's data for a deeper reason (a shared-border frame the
   local re-base cannot reach) — then holding it degrades the solve.  §21.5's "P2a passing with P2c failing ⇒
   second-order seam" is the case we are in; the D5 gate erases the branch before either reading can be told
   apart, which is why the next increment is a measurement, not a fix.

**RS-2b pre-registered — the D5 resume warm-up (`network.resume_d5_warmup_ep`, int, default 0).**  For the
first K epochs after a WARM resume the D5 innovation gate is diagnostic-only: trips are counted
(`d5_warmup_suppressed`, and the trace row keeps its columns plus a `warmup_suppressed` flag) but neither
re-tapped nor mark the arc slipped; the MW / LLI / IF-TD drains stay LIVE (a real slip still refloats), the
corroboration gate is untouched, cold starts are unaffected (the countdown is set only at a warm resume).
Forward-only resume response policy, fingerprint-EXEMPT (the `resume_slip_warmup_ep` class).  Legs from the
v2 seed ep0604, blob + re-base, `--extra` `resume_d5_warmup_ep` 5 (`rs2_null_rebase_d5w5`) and 1
(`rs2_null_rebase_d5w1`, the dose control):

* **P4a the branch survives:** births at +2 ≤ 20 (was 305); D5 suppressed count reported per epoch +1..+K;
  D5 flags at +K+1..+K+5 (the first live epochs) ≤ 10 per epoch — if they return at hundreds the branch was
  wrong (reading b).
* **P4b the solve stays sane with the branch held:** emitted 60/60; |a₀₀ − seed| ≤ 2 TECU at every epoch from
  +1; clock_std and σ̂₀_code ≤ 1.5× the seed's; no SH-gate or σ̂₀ withhold.
* **P4c the branch carries information (reading a):** pooled +5..+60 clock contrast p50 ≤ the no-blob leg's
  2.50 m AND ≤ 3× the seed's own step (0.27 m) for a full pass; per-sat-median-removed |ΔPB| at +2 ≤ 1 m
  (no-blob 9.98); consumer p50 ≤ 5.05 m.  The R record at +1 is recorded — 30 satellites again would mean
  the PB re-realization at a warm resume is structural to the emitter on this lineage (a separate item),
  not a branch property.
* P4a+P4b pass with P4c failing ⇒ the branch is right but the wire's PB/clock realization is set elsewhere
  (the station layer / emitter memory: RS-3, snapshot the rx-CB carry, the T5 deviation and the PB
  realization).  P4b failing ⇒ reading (b): the restore is unfaithful beyond the local frame; the harness
  stays no-blob and the ledger row says so.

**Ledger (this commit, `scripts/_rs2_refarc_ledger_oneoff.py rs2 PIN …`):** `network.state_snapshot_carry_lin`
RECOMMEND_SCOPED, PINNED on the field golden (write side, products byte-identical — every snapshot-writing
run of this lineage now carries its frame); `network.resume_carry_rebase` RECOMMEND_SCOPED with the
necessary-not-sufficient record above, armed by the harness on every resume leg, pinned nowhere.

### 21.7 RESULTS — RS-2b: the branch was right; the wire's realization is set by two datum memories the snapshot never carried; RS-3 pre-registered (2026-09-12 ~13:45 local; legs `runs/evleg_rs2_null_rebase_d5w{5,1}`, seed v2 ep0604, exe `gnss_processor_rs2b.exe` a31248d0…; manager re-grade)

**Build (merged `b6afe8c`; registered `58c4088`).**  `network.resume_d5_warmup_ep` (int, default 0): armed only in
`networkTryWarmResume`, counted down at the install seam (the D5 gate's single site), trips counted
(`d5_warmup_suppressed`, a `warmup_suppressed` column on `network_d5_flags.csv`) but neither re-tapped nor
funnelled; σ̂₀ feed, corroboration gate and the MW/LLI/IF-TD drains untouched; not fingerprinted.  Cold smoke
with K=5 hashes `b9da8cc1…` (inert without a resume); Debug suite 1969 / 0 failed; one log-order defect the
worker found in its own retire line was fixed before the legs were re-run (`c96182a`).

| | base (blob + re-base) | **K = 5** | **K = 1** |
|---|---|---|---|
| D5 flags / re-taps / suppressed at +1 | 301 / 301 / 0 | 301 / **0** / **301** | 301 / **0** / **301** |
| chord births +1 / +2 / +3 / +4 / +5 | 64 / **305** / 4 / 8 / 0 | 64 / **7** / 4 / 8 / 0 | 64 / **7** / 5 / 10 / 2 |
| live D5 trips after the window | (+7: 1) | +6..+10: 0, **1**, 0, 0, 0 | +2..+6: **1, 2, 2**, 0, 0 |
| D5 rows over the leg / run re-taps | 332 / 332 | 332 / 27 | 332 / 31 |
| G2 / G3 (worst \|Δa₀₀\| from +5; clock_std ×; σ̂₀ ×) | PASS / 2.73; 0.902; 1.126 | PASS / 2.56 @+9; 0.903; 1.178 | PASS / 2.56 @+9; 0.903; 1.179 |
| Δa₀₀ at +1 … +5 (all legs alike) | +4.95, +2.50, +2.83, +2.41, +2.48 | +4.95, +2.27, +2.82, +2.26, +2.34 | +4.95, +2.27, +2.92, +2.55, +2.31 |
| R record at +1 | 30 sats | **30 sats** | **30 sats** |
| per-sat-median-removed \|ΔPB\| p50 at +2 / +60 | 14.05 / 13.79 m | 14.35 / 13.64 | 14.35 / 13.78 |
| clock contrast pooled +5..+60 p50 / p95 | 3.64 / 11.75 m | 3.59 / 11.91 | 3.61 / 11.94 |
| W6 code bias pooled p50 | 0.669 | 0.651 | 0.656 |
| ZWD pooled p50 / p95 | 2.14 / 9.43 | **1.66 / 8.58** | **1.65 / 8.62** |
| consumer p50 from +5 (seed 8.94) | 5.18 | 8.91 | 5.24 |

**P4a PASS, P4b PASS, P4c FAIL — the pre-registered reading (a): the branch is right, the wire's realization
is set elsewhere.**  Holding the branch through ONE epoch removes the +2 birth wave (305 → 7); the 301 trips
at +1 are identical in every arm and never return — the gate then runs at the seed's own rate (0–2 per
epoch, the same 332 D5 rows everywhere).  So the +1 disagreement was one epoch of cold station layer, not a
branch inconsistency, and K = 1 suffices.  The Δa₀₀ sub-bar (≤ 2 TECU from +1) is failed identically by
every resume posture including the no-blob one (+2.3 … +2.9 TECU over +2..+10, decaying to < 1 by +60) — a
common ~30-min SH-level transient at the seam, recorded, not a branch property; P4b is judged on the G3
5-TECU bar.  P4c: the wire does not move at all — clock contrast, code bias and the per-satellite PB offset
are unchanged to within 3 %, the emitter still declares 30 satellites at +1, and only ZWD is quieter (23 %).
The consumer split (K=5 8.91 vs K=1 5.24 m on wires that differ by < 1 %) is the in-process consumer's own
state, not the wire — §18.14 stands: grade seams with a cold-consumer replay.

**Decision.**  `network.resume_d5_warmup_ep` RECOMMEND_SCOPED at K = 1 (`58c4088`); `scripts/event_leg.py`
arms it on every resume leg beside `resume_carry_rebase`, so the harness posture for field legs is now
**blob + re-base + one held epoch** (branch continuity is what the slip cards grade; the datum re-draw is
common to every posture and cancels in event − null).  The no-blob posture remains available by `--extra`.

**The two datum memories (read from the code, confirmed by the leg's own events).**  At +1 the leg writes
**110 `PHASE_DATUM / eq34_reref` events** (the seed: 2 per epoch) and **30 `CB_DATUM / reseed_disc_landed`**
(the seed: 0):
1. The eq34 chain datum's spanning tree is re-elected from scratch at the seam.  The election
   (`NetworkChainDatum.cpp`, STICKY-tree seed at `:230–270`) continues last epoch's tree through
   `eq34_datum_rx` (the sticky datum receiver per (constellation, signal)), `eq34_rx_parent` (each
   receiver's parent-edge satellite), `eq34_sat_root` and `eq34_prev_target` — documented at
   `NetworkRunContext.hpp:794–815` and NEVER written to the snapshot — plus `anchor_incumbent` (the sat-side
   parents, which the frame restore DOES bring back: 126).  With the receiver-side maps empty the tree is
   rebuilt from the cold arc registry, the pinned arcs change, and every phase-bias family re-realizes by the
   integer between the old and new pinned arcs: 110 rerefs, the R record naming 30 satellites, per-satellite
   PB offsets of 14 m.  The DD branch cannot prevent this — it is invariant to the datum by construction.
2. The time-linked code-bias datum's previous-value map (`code_bias_prev_` / `code_bias_sat_seen_`,
   `NetworkRunContext.hpp:1128–1142`) is "NOT persisted", and the resume block sets
   `code_bias_datum_resumed_ = true` so that every first post-resume re-seed disc-bumps
   (`NetworkRunContext.cpp:1655–1665`, landed at `NetworkSolPrep.cpp:~880–900`): the W6 code-bias / clock
   split is re-drawn for 30 satellites — the 3.6 m clock-contrast and 0.65 m code-bias offsets.

**RS-3 pre-registered — datum continuity across the seam.**  Write knob `network.state_snapshot_datum_capture`
(default false; beside `state_snapshot_frame_capture`): the snapshot carries the eq34 tree maps
(`eq34_datum_rx`, `eq34_rx_parent`, `eq34_sat_root`, `eq34_prev_target`, `eq34_prev_el`,
`eq34_retained_edges`) and the time-linked code-bias datum memory (`code_bias_prev_`,
`code_bias_sat_seen_`) as format-forward rows; write side only, products byte-identical.  Read knob
`network.resume_datum_restore` (default false; forward-only resume response policy, fingerprint-EXEMPT;
requires `resume_frame_restore`): restore the maps before the first election so the sticky-tree seed
continues the writer's tree, and restore the code-bias memory so `code_bias_datum_resumed_` stays false
and a re-seed bumps only where it genuinely re-seeds; rows absent ⇒ today's behaviour, loudly.  Counters and
a run-end line (`eq34 edges / datum rx / prev targets restored; CB prev values / seen restored; first-epoch
eq34 rerefs, reattaches, CB re-seed discs`).  New seed `runs/evseed_field6h_v3` (write knob via `--extra`),
legs from its ep0604 on the harness posture, one exe: `rs3_null_restore`, `rs3_null_norestore` (the
control — must reproduce K = 1's 110 / 30 / 30-sat / 14 m), `rs3_null_nocarry_restore` (the datum half
without the branch).
* **P5a the datums continue:** `eq34_reref` at +1 ≤ 5 (was 110); `reseed_disc_landed` at +1 ≤ 3 (was 30);
  the R record at +1 names ≤ 3 satellites (was 30).
* **P5b the wire is continuous, pooled +5..+60 and at +2:** per-sat-median-removed |ΔPB| at +2 ≤ 0.05 m
  (was 14.35); clock contrast p50 ≤ 3× the seed's own step (0.27 m; was 3.6) and p95 ≤ 3× (1.2 m); W6 code
  bias p50 ≤ 0.03 m (was 0.65); ZWD p50 ≤ 3× own step (0.32 m; was 1.65); no silent PB step > 0.05 m on
  > 10 % of keys at any epoch from +2 on.
* **P5c availability and branch unchanged:** 60/60 emitted; a₀₀ on the G3 bar; births +2 ≤ 20; D5 rows ≈ 332.
* **Consumer:** a cold-consumer replay of the leg's wire against a replay of the seed's wire over the same
  window (`network.closed_loop.replay_products_path`, §18.14), reported; the in-process consumer is not graded.
* P5a passing with P5b failing names a THIRD memory (the rx-CB carry, the T5 iono deviation) — then the
  first-epoch offsets per channel say which; P5a failing means the sticky seed does not engage on restored
  maps under `carry_invariant` (its carry-value test), and the fix moves into the election.

### 21.8 MAIN CERTIFICATION after the day's merges (2026-09-12 ~14:20 local; worker on main's own tree)

main `58c4088` (RS-2 `2c62724`, reference-arc clause `4b88ca2`, RS-2b `b6afe8c`, the three ledger/posture
passes): `src`/`include`/`scripts` clean; Debug and Release rebuilt with the wrapper, first attempt.  **Full
Debug suite on main: 1975 cases / 269 suites, 1974 passed, 1 skipped (the documented fixture gate
`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`), 0 failed** (the 17 skips seen in worktrees are the
`data/`-fixture gates that resolve on main's baked data root).  Smoke pair on `gnss_processor_main_close.exe`
(74f9ed0b…): `products.txt` **`b9da8cc1…` in both arms** (the golden as it ships with `state_snapshot_carry_lin`
pinned vs the same with it false), the snapshot differing only by the four `ZL` rows, fingerprint
`545b8bdd2b638231` both; the POSTURE block lists `state_snapshot_carry_lin` (armed ⇒ no advisory in the ON arm,
`POSTURE OFF [SCOPED]` in the OFF arm), `resume_carry_rebase` and `resume_d5_warmup_ep` (off on a seed
writer, correctly), and omits `common_band_ref_arc` because its parent scope is the field exception (out of
regime, row present at `GnssConfigNetworkPosture.cpp:201`); zero unknown-knob warnings.  **Closing null leg
`runs/evleg_close_null_main`** from `evseed_field6h_v2` ep0604 with `event_leg.py`'s registered defaults
(frame restore, re-base, `resume_d5_warmup_ep` 1, coupling restore false): 61/61 emitted, 301 suppressed /
637 re-based, and `products.txt` `d75d12a6…` **byte-identical to `rs2_null_rebase_d5w1`'s** on a different
binary — the harness posture reproduces the hand-armed leg exactly.

### 21.9 RESULTS — RS-3: both datums continue at the seam; the wire still re-realizes ~10 m per phase-bias family, without the branch too, and UNDECLARED; RS-4 pre-registered (2026-09-12 ~16:30 local; seed `runs/evseed_field6h_v3`, exe `gnss_processor_rs3.exe` D233ADBB…; manager re-grade)

**Build (merged `794d10e`).**  As §21.7 plus one deviation the worker found by measurement: the code-bias half has a
THIRD container — the IF-pin reference pair `code_bias_ifpin_pair_`, against which `NetworkSbasis` decides
`pair_same`; restored empty it reads "pair changed" for every satellite, the split re-seeds to 0 and (with the
seen-set now restored) every one bumps — restoring the values without the pair was strictly worse than today
(probe: 32 of 32 still re-seeding).  Row `CBI` added; discs 32 → 0.  Rows ride the existing header (the
optional-`var_first` precedent).  Debug suite 1982 / 0 failed, `ResumeDatumRestore` (7); cold smoke products
`b9da8cc1…` both arms, snapshots differing only by the 824 new rows; seed v3 720/720, ep0604 at the same tow
with 793 datum rows (6 EQD / 176 EQP / 126 EQS / 302 EQT / 121 CBP / 32 CBS / 30 CBI), fingerprint
`545b8bdd2b638231` unchanged.

| first post-resume epoch | restore | norestore (control) | nocarry_restore |
|---|---|---|---|
| `eq34_reref` / `eq34_reattach` (seed 2 / 1) | **8 / 0** | 110 / 0 | 8 / 0 |
| `CB_DATUM/reseed_disc_landed` (seed 0) | **0** | 30 | 0 |
| R record satellites | **0** | 30 | 0 |
| D5 flags at +1 / births +1 / +2 | 40 / 64 / 7 | 301 / 64 / 7 | 0 / 701 / 7 |
| G2 / G3 / G4 | PASS / PASS / PASS | PASS / PASS / PASS | PASS / PASS / PASS |
| per-sat-median-removed \|ΔPB\| p50 at +2 / +60 | **11.20 / 11.30 m** | 14.35 / 13.78 | 9.33 / 10.52 |
| clock contrast pooled p50 / p95 | **1.74 / 9.28 m** | 3.61 / 11.94 | 1.82 / 8.85 |
| W6 code bias pooled p50 | 0.470 | 0.656 | 0.464 |
| ZWD pooled p50 / p95 | 0.64 / 4.99 | 1.65 / 8.62 | 1.24 / 7.84 |
| a₀₀(+60) − seed | −1.19 | −0.84 | −1.22 |
| cold-consumer replay p50 from +5 (§18.14) | **6.66 m, falling** | 10.04 m, flat | — |

The control reproduces §21.7's K = 1 leg exactly (110 / 30 / 30 sats / 14.35 m / 3.61 / 0.656 / 1.65), so v3
is a faithful replacement for v2.  The restored tree at +1 (the leg's own first-epoch snapshot — see the trap
below) is 93 % the writer's: EQP 164/176 parent edges, EQS 115/126 roots, EQT 282/302 pins (all targets 0),
CBS/CBI identical.

**Verdicts.**  P5a: the two hard bars pass (re-seed discs 0 ≤ 3, R record 0 ≤ 3); `eq34_reref` 8 against a
bar of 5 and a seed rate of 2 — a near-miss that reads as the 7 % of the tree that genuinely could not be
kept (arcs that changed across the seam).  P5c PASS.  **P5b FAIL on every channel**, all moving the right way.
Cold-consumer replay: the restore leg's wire converges (6.66 m and falling) where the control's does not (10.0
flat) — a real product improvement, not yet continuity.

**The residual, measured (`products.txt` at +2, leg vs seed).**  Satellite clocks agree to a common −0.118 m
with a datum-free contrast of 0.54 m p50.  Phase biases do NOT: raw |ΔPB| p50 **17.6 m** over 120 keys,
**not whole wavelengths** (|Δ/λ − round| p50 0.258 — uniform noise), **not common to a satellite** (spread
across a satellite's signals 48.7 m p50; G02 L1C +11.5 / L1W +37.5 / L2W +13.8 m).  The emitter's own per-key
baselines (`epb`, restored from the writer) moved by 16.5 m p50 at the first emit.  The same order of offset
is present WITHOUT the DD branch (`nocarry_restore` 9.3 m) and with the branch held (§21.7), and the tree and
targets are the writer's.  So the phase-bias LEVEL per family is set by something that restarts cold in every
posture — the station layer (the rx-CB carry: 26 % of priors withheld at its 10σ gate on the leg vs 13 % on
the seed; the T5 per-LOS iono deviation carry: 400 cold seeds at +1; the SH a₀₀ +4.6 TECU at +1 decaying in
five epochs; the receiver phase-bias states) — or a definition of the family's level the pins do not fix.
That is RS-4's measurement.

**The seam is UNDECLARED on the wire — an invariant-#7 gap in its own right.**  The first emitted E record
carries `datum_changed 0`, no R record is written, and this lineage does not serialize the optional per-key
`phase_disc` field (legend: `P,sat,signal,bias_m,signal_integer,wide_lane_integer[,phase_disc]`), so a file
consumer sees a 17 m phase-bias step with no signal.  Why: the CLK-R `resume_seam` datum mark fires only when
the held-frame engine reports `epochs == 0` (`NetworkEmitStore.cpp:338`), and the frame restore brings that
engine back with its 604 epochs — the mark was designed for the clock frame and is suppressed exactly when the
clock IS continuous; the emitter's per-key jump test (`NetworkProductEmitter.cpp:383`) does bump the
in-process counters on a fresh baseline, which is what the in-process consumer reacts to, but nothing reaches
the file.  ⚠ Harness trap (worker, verified on `rs2_null_rebase_d5w1` too): a leg's first emitted epoch
overwrites the `seed.txt` it resumed from — `event_leg.py` sets `state_snapshot_interval_s = 1e9`, but
`NetworkRunContext.cpp` gates the write on `last_snapshot_write_s < 0.0 || …`, and the initial −1 fires
unconditionally.  Every leg today copied a fresh snapshot and ran once, so no result is affected; `event_leg.py
rerun` exists for exactly this; a re-run in place would silently resume from the leg's own +1 snapshot.

**RS-4 pre-registered — the phase-bias level at the seam: measure, then declare.**
* **Measurement (no solve change):** arm the raw satellite-bias state trace (`network.trace_sat_bias_states`,
  trace-class) on a `rs3_null_restore`-posture leg and on a 60-epoch re-run window of the seed, and attribute
  the raw estimator PB difference leg − seed at +1 per (satellite, signal) to: (a) the family's pinned arc kept
  vs changed (the EQT diff), (b) the pinning station's rx-CB carry restart and its receiver phase-bias state,
  (c) the signal and its per-LOS iono deviation at the pinning station, (d) emitted vs raw (whether the
  emitter's re-base/filter moves the level).  Deliverable: the table, and a one-sentence mechanism the next
  memory card can be built on.  Also the rx-CB carry and T5 deviation as candidate rows for a capture increment
  if (b)/(c) dominate.
* **Declaration (small build, default-OFF, byte-identical when off):** `network.resume_pb_seam_mark` — at the
  first emit after a warm resume, if the emitted phase bias differs from the restored per-key baseline by more
  than `discThresholdM` on more than 10 % of a constellation's keys, mark `datum_changed` for that
  constellation exactly as the CLK-R `resume_seam` rule does (counted, evented `PB_FRAME/resume_seam`); and
  register the per-key discontinuity serialization for the field golden as its own row (product bytes change
  — a documented product-contract change, certified by a smoke pair).  Gates: on the restore leg the first E
  record carries `datum_changed 1` (or an R record) whenever P5b's PB bar fails; byte-identical products when
  no key exceeds the threshold (the seed-window control); the cold-consumer replay unchanged (the mark reaches
  the file, the replay driver's reaction is measured, not assumed).

### 21.10 RESULTS — RS-4: the seam's phase-bias level is the EMIT LAYER, and it closes exactly; the seam is now declared; RS-5 pre-registered (2026-09-12 ~19:00 local; worker on worktree-agent-ac918e1c0b5f083e4, merged `83a48de`; manager grade)

**The instrument.**  `network.trace_sat_bias_states` does not carry the phase-bias state (its writer at
`NetworkSolPrep.cpp:1253` emits the satellite CODE bias), and no trace carries it — `network_solution.csv` is
written downstream of the emit transforms and mirrors `products.txt`.  The worker found the exact
instrument in existing knobs: `NetworkEstimator.cpp:1050` sets `npb.bias_m = x_s(c)` (the raw state) and two
OUTPUTS-ONLY transforms then overwrite it in `NetworkEmitStore.cpp` — the Card-4 eq34 emit re-base (`:256`,
`carry_invariant_emit_rebase`, default true) and the PB emission filter (`:295`, `emit_pb_filter`, armed on
the golden) — so a twin with both off (`runs/evseed_field6h_v3raw`) emits the raw state verbatim.  Proof it is
outputs-only: the v3t and v3raw ep0604 snapshots differ in 0 lines outside the emitter `epb`/`ep` rows, and the
two restore legs have max |ΔCLK| = 0.0 over 1830 clock keys.  The trace-only twin `runs/evseed_field6h_v3t`
hashes `products.txt` `5f671fba…` = v3's (trace-class confirmed).

| p50 \|Δ\| per (sat, signal), leg − seed | +1 | +2 | +30 |
|---|---|---|---|
| **EMITTED** | **17.666 m** | **17.611** | **17.652** |
| **RAW** (transforms off on both sides) | **1.695 m** | **1.862** | **3.263** |
| emit layer on the LEG (emitted − raw) | **0.000 (0 of 129 keys nonzero)** | 0.628 | 5.714 |
| emit layer on the SEED at the same tow | 18.410 | 18.766 | 23.062 |
| closure `emitted − (raw + legLayer − seedLayer)` | — | **0.000 over 120 keys** | — |

In §21.9's own units (per-sat-median-removed |ΔPB| p50 at +2): restore 11.20 (§21.9: 11.20), nocarry 9.33
(§21.9: 9.33), **raw 0.43 m**.  Clocks reproduce §21.9 (common −0.1177 m, contrast p50 0.537).

**Mechanism.**  The eq34 emit re-base engine (`NetworkEmitRebase`, a bare run-local at
`ApplicationNetwork.cpp:915`; memory `last_emit_` = one double per key, the previous emission frame) and the
PB emission filter (`NetworkPbEmitFilter`, `:930`; `state_` mean/var/last_seen per key + per-constellation
step accumulators) are in no snapshot and cold-start at the seam: the leg's first epoch passes the raw
solve-frame value through untouched while the seed's wire at the same tow carries 604 epochs of accumulated
re-base (18.4 m p50).  The estimator's own level moves **1.70 m p50 at +1** — 1.68 m excluding the six L2X
keys (24.7 m), whose eq34 component datum receiver GAMB00PYF vanished at the seam (`→ None/CPVG00CPV`; 19 of
134 families changed root); grouped by pinned-arc kept vs changed 1.59 vs 2.44 m, by signal L1C 2.10 / L1W
1.10 / L2W 2.41 / L5Q 2.37 / L5X 0.82, L2W/L1W ratio −0.55 (not iono-like: +1.65).  Receiver clocks leg −
seed at +1 p50 1.21 / p95 5.11 m over 61 stations; receiver code bias 0.57 / 1.68.  **Cold-consumer replay**
(p50 3D from +5 / +30): restore 6.66 / 5.54, nocarry 15.41 / 15.44, the seed's own continuous wire over the
same window 9.30 / 8.08 — a constant per-key level costs a cold consumer nothing; the consumers the seam
hurts are the ones that carried state across it.

**Declaration built and measured (`network.resume_pb_seam_mark`, default OFF, one-shot, outputs-only, not
fingerprinted).**  Marked leg: run-end `keys tested 129, over threshold 129 (100.0 %), constellations marked
1`, one `PB_FRAME/resume_seam` event (`keys_over=129/129, max|dPB|=119.3 m`), first E record `datum_changed 1`
(`…,604,0,0,1,G`) vs the control's 0 — **the whole 61-epoch products file differs by that one line** (P
records max |Δ| 0); cold smoke on and off both `b9da8cc1…`; Debug suite 1988 / 271 suites / 0 failed;
`ResumePbSeamMark` (6).  The replay of the marked wire is byte-identical to the unmarked one (`NetworkProductIO
.cpp:773` re-applies the flag, but every key is its own first sighting at the first replayed epoch).  The
complementary half for a stateful file consumer is **`network.serialize_disc_counters`** (RTA-4): no ledger row
until now; its smoke pair changes only the P (2700) / B (2595) / C (640) records (`b9da8cc1 → 87a33bf0`, one
integer inserted before the `sigma=` token of every P line, legend unchanged).  Registered this commit:
the mark RECOMMEND_SCOPED and armed on every harness leg; the serialization RECOMMEND_SCOPED with the golden
pins DEFERRED to the RS-5 close-out, where the smoke baselines of all of today's cards are re-recorded once.

**RS-5 pre-registered — emit-layer continuity across the seam.**  Write knob
`network.state_snapshot_emit_capture` (default false): the snapshot carries `NetworkEmitRebase::last_emit_`
(row `ER,<sat|signal>,<last_emit_m>`) and `NetworkPbEmitFilter`'s `state_` (`PF,<sat|signal>,<mean>,<var>,
<last_seen_s>`) and `step_` (`PS,<const>,<sum_log_e2>,<n_eff>`) as format-forward rows under the existing
header; both engines become parameters of `networkWriteStateSnapshot` / `networkTryWarmResume` exactly as the
CLK-R engine was made one (§18.10), with `exportState` / `restoreState` on each.  Read knob
`network.resume_emit_restore` (default false; forward-only resume response policy, fingerprint-EXEMPT; requires
`resume_frame_restore`): restore both engines before the first emit; rows absent ⇒ today's behaviour, loudly;
counters + run-end line (`emit re-base keys restored N, filter keys N, step accumulators N; first-epoch emit
layer p50 vs the writer's`).  Legs from a new seed `runs/evseed_field6h_v4` (write knob via `--extra`; the
harness will arm both knobs by default once merged): `rs5_null_restore`, `rs5_null_norestore` (control — must
reproduce RS-4's 17.6 m emitted / 0.000 layer), and the raw-transform twin is not needed (the closure is the
instrument).
* **P7a the emit layer continues:** emitted − seed at +1 p50 ≤ raw − seed p50 + 0.05 m (i.e. ≈ 1.7 m, the
  estimator's own residual); the leg's emit layer at +1 within 0.05 m of the seed's on ≥ 95 % of keys; the
  seam mark's `keys over threshold` fraction reads the RAW residual's (recorded — with a 1.7 m raw step it is
  expected still to fire, honestly).
* **P7b nothing else moves:** availability, a₀₀, clocks, D5, births, eq34 rerefs and CB discs as RS-3's restore
  leg; cold-consumer replay unchanged (a level is invisible to a cold consumer).
* **P7c the stateful consumer (the point of the card):** a consumer replay STARTED on the seed's wire and
  continued across the seam onto the leg's (splice the seed's products up to ep0604 with the leg's; §18.14's
  `splice_wire.py`) — its ambiguity resets at the seam and its 3D p50 over +5..+30 vs the same consumer on the
  seed's continuous wire; recorded for the control and the restore leg, with the mark's declaration on.
* Then RS-6 — the estimator's own 1.7 m level at the seam (the station layer: receiver clocks 1.2 m p50 at +1,
  the rx-CB carry, the T5 deviation, the vanished L2X component datum receiver) — is pre-registered from
  RS-5's residual table, not before.

### 21.11 RESULTS — RS-5 closes the seam on the phase-bias wire; the day's close-out; RS-6 pre-registered (2026-09-12 ~21:00 local; worker on worktree-agent-a1a4a2dd4a4bbb596 `265d122`; manager grade)

**Build.**  `NetworkEmitRebase::exportState/restoreState` (the `last_emit_` map) and
`NetworkPbEmitFilter::exportState/restoreState` (`state_` + `step_`; counters and config stay run-local);
write knob `network.state_snapshot_emit_capture` (rows `ER` / `PF` / `PS`, format-forward under the existing
header; the pooled `PS` bucket's key is legitimately blank), read knob `network.resume_emit_restore` (throws
without `resume_frame_restore`); both engines threaded through `networkWriteStateSnapshot` /
`networkTryWarmResume` as the CLK-R engine is.  Debug suite **2001 / 273 suites / 0 failed** (17 fixture
skips in the worktree); `ResumeEmitRestore` + `ResumeEmitRestoreConfig` 13/13; cold smoke `products.txt`
**`b9da8cc1…` in both arms**, snapshots differing by 135 ER + 135 PF + 1 PS rows; seed
`runs/evseed_field6h_v4`: 720/720, `products.txt` **byte-identical to v3t's** (`5f671fba…`), ep0604 with 147
ER / 147 PF / 1 PS, fingerprint `545b8bdd2b638231` unchanged.

| emitted PB, leg − seed, p50 / p95 / max \|Δ\| | +1 | +2 | +30 |
|---|---|---|---|
| `rs5_null_norestore` (control) | 17.666 / 82.80 / 119.34 m | 17.633 / 82.82 | 17.652 / 82.84 |
| **`rs5_null_restore`** | **0.000 / 0.003 / 0.053 m** | 0.001 / 0.011 | 0.038 / 0.153 |

Keys within 0.05 m at +1: restore **120/121**, control 0/121; per-sat-median-removed p50 at +2: 0.0005 m vs
11.20.  **P7a PASS — and past its own bar:** the restored re-base aligns each key to the writer's last emitted
value, so it absorbs the estimator's 1.7 m raw level as integer + common-fractional; the PB seam mark correctly
stays silent (`keys tested 129, over threshold 6 (4.7 %), marked 0`, first E `datum_changed 0`) where the
control fires it (129/129, marked 1).  **P7b PASS, byte-exactly:** the two legs' `products.txt` (19 049 lines)
differ ONLY in 7 840 P rows and the one E flag — every C / B / U / H record identical, and a₀₀ (worst |Δ| 4.39
TECU at +39; −1.19 at +60), clocks (contrast pooled p50 1.743), code bias (0.470), ZWD (0.636), D5 flags at +1
40 / re-taps 0, births 64 / 7, eq34 rerefs 8, CB re-seed discs 0, `637 re-based` are the same numbers on both.
**P7c — the stateful consumer** (`scripts/splice_wire.py`, re-written and verified to reproduce the seed's
bytes up to the seam; the same WTZZ00DEU consumer replayed over the same 665-epoch window):

| wire | PB resets at the seam | code re-refs | clock re-refs | PB resets over the window |
|---|---|---|---|---|
| seed, continuous | 0 | 0 | 0 | 29 |
| spliced control | **19** | 24 | 6 | 59 |
| **spliced restore** | **0** | 21 | **0** | 30 |

The restore wire is indistinguishable from the continuous wire on the phase-bias channel; the 6 clock
re-references in the control are downstream of its `datum_changed 1`; the code-bias re-references (21 / 24) are
in BOTH legs whose B records are byte-identical — the seam's CODE-BIAS channel, the estimator's own 0.47 m
level, declared per key by the code jump test.  (The replay's 3D column is not an instrument here: the file
replay floats its own iono and reads ~15 m on the continuous wire.)

**Decision.**  Both knobs RECOMMEND_SCOPED and registered; the goldens pin the write knob; the harness arms
both by default (seeds and legs).  **The deferred product-contract pin lands now:** `network.serialize_disc_
counters` is pinned on both goldens and the smoke baselines are re-recorded once in the close-out
certification below (the field golden's canonical 20-epoch hash moves from `b9da8cc1…` to the serialized
one measured in §21.10's pair, `87a33bf0…`, if nothing else in the day moved products — the certification
says).  The harness posture for a field resume leg is therefore: frame + branch (re-based, one held D5 epoch) +
both datums + the emit layer + the PB seam mark, every one of them default-OFF, byte-identical when off, and
outside the warm-resume fingerprint.

**What the seam still costs, and where (RS-6, pre-registered as a MEASUREMENT first).**  The estimator's own
level at the first post-resume epoch: phase bias 1.70 m p50 raw (1.68 excluding the six L2X keys whose eq34
component datum receiver vanished), code bias 0.47 m p50 (the 21 code re-references), clock contrast 0.54 m at
+2 growing to 1.7 m pooled, ZWD 0.64 m, receiver clocks 1.21 m p50 — all set by the station layer that
restarts cold (the rx-CB carry: 26 % of its priors withheld at the 10σ gate on a leg vs 13 % on the seed; the
T5 per-LOS iono deviation carry: 400 cold seeds at +1; the SH a₀₀ +4.6 TECU decaying over five epochs; the
receiver phase-bias states).  With RS-5 armed none of this is visible on the PB wire any more (the re-base
absorbs it), so RS-6 needs a STATE-side instrument: extend `network_sat_bias_states.csv` (today CODE bias only)
with the per-(sat, signal) phase-bias state (trace-class, byte-identical products), and a trace of the rx-CB
carry values per (station, signal).  Legs from v4 ep0604 on today's posture, each with ONE station-layer memory
restored in turn once captured (rx-CB carry; T5 deviation; RPB) against the control — the channel that moves
names the memory.  Bars, frozen now: raw PB residual at +1 p50 ≤ 0.2 m; code bias ≤ 0.05 m p50 (and the
stateful consumer's code re-references at the seam ≤ 3); clock contrast pooled p50 ≤ 3× the seed's own step
(0.27 m); ZWD ≤ 3× own step (0.32 m); SH a₀₀ at +1 within 2 TECU.  Also owed from the day: the six L2X families
whose component datum receiver GAMB00PYF is dropped at the seam (a re-election edge, not a memory).

⚠ **Harness trap, confirmed on every leg today and to be fixed in the harness, not the producer:** a leg's
first emitted epoch overwrites the `seed.txt` it resumed from (`NetworkRunContext.cpp` gates the first write on
`last_snapshot_write_s < 0.0`, which the initial −1 satisfies unconditionally, whatever
`state_snapshot_interval_s` says).  The `.epNNNN` library is intact; `event_leg.py rerun` restores the pristine
copy; never re-run a leg in place.  **FIXED in the harness 2026-09-13 (main `dcf04dd`, worker commit `a207193`):**
`make-leg` / `null` now lay down TWO copies — `seed.txt` (pristine, never named by any config; its sha256 recorded in
`leg_meta.json`) and `state_snapshot.txt` (what `state_snapshot_path` names, disposable) — so the exe never reads and
writes the same file; `rerun` restores only the disposable copy after verifying a pristine source.  Validated on
`runs/evleg_hfix_trap`: first run and in-place rerun both WARM RESUME, products byte-identical (`95cda87d…`),
`seed.txt` unchanged.  The same commit arms `resume_coupling_restore` on a leg only when the seed's lineage carries
the loader's Card-1f interlocks (the field golden does not), so field legs no longer need `--extra` for it.

### 21.12 CLOSE-OUT CERTIFICATION (2026-09-12 ~22:30 local; worker on main's own tree at `17f854f`)

Debug and Release rebuilt with the wrapper, first attempt.  **Full Debug suite: 2001 cases / 273 suites,
2000 passed, 1 skipped (the documented fixture gate), 0 failed.**  Three-way 20-epoch smoke of the shipped
field golden on `gnss_processor_close2.exe` (106D0205…): `base` (as it ships) **`87a33bf0…`**; `noser`
(`serialize_disc_counters` false) **`b9da8cc1…`** — the previous canonical hash, so nothing but the
serialization moved products today; `nowrite` (the three write knobs `state_snapshot_carry_lin` /
`_datum_capture` / `_emit_capture` false) **== `base`** — the write side is byte-identical.  The base/noser
diff is exactly P 2700 / B 2595 / C 640 records, one appended or inserted integer each, S/E/H/U untouched
(RS-4's census reproduced).  **The field golden's canonical 20-epoch hash is `87a33bf0…` from here on.**  All
nine knobs registered today read correctly in the POSTURE block (the four write/contract knobs armed; the
resume knobs `POSTURE OFF [SCOPED]` on a cold seed writer; `resume_datum_restore` and `common_band_ref_arc`
out of regime), no unknown-knob warnings.  Closing leg `runs/evleg_close2_null_main` (v4 ep0604, harness
defaults): 60/60, every seam and run-end line as §21.11's restore leg, and its `products.txt`
**byte-identical to `rs5_null_restore`'s** (`5fca04e2…`) on main's merged exe.

Two notes for the next session.  (1) **A leg inherits its SEED's config:** `runs/evseed_field6h_v4` was
written before the `serialize_disc_counters` pin, so every v4 leg keeps the unserialized product contract —
write a v5 seed from the current golden before grading anything that needs the per-key counters on the wire.
(2) Two pre-existing golden properties, not today's: the loader's regime label reads "WARM-RESUME start" on a
cold smoke (it keys on `state_snapshot_path` being set), and the POSTURE CHAIN advisory about
`emit_generation_id` under `state_snapshot_require: false` fires on every cold seed writer.

### 21.13 RS-6a pre-registered — the station layer's own memory across the seam: the rx-CB carry and the T5 iono deviation carry (2026-09-12 ~23:00 local, manager; before any leg is read)

**What is left at the seam after RS-5, and where it lives.**  With the emit layer restored the phase-bias WIRE
is continuous, but the estimator's own level at the first post-resume epoch still moves: raw phase bias 1.70 m
p50 (§21.10; per-station medians MAJU00MHL 2.3 / MBAR00UGA 0.96 m — a per-station component), W6 code bias
0.47 m p50 (the 21 code-bias re-references a stateful consumer takes at the seam, §21.11), receiver clocks
1.21 m p50 / 5.11 p95, clock contrast 0.54 m at +2 growing to 1.7 m pooled, ZWD 0.64 m, SH a₀₀ +4.6 TECU
decaying over five epochs.  Two station-layer memories restart cold at the seam and neither is in the
snapshot: the **rx-CB carry** (`ctx.carry.rx_cb_carry`, `std::map<std::pair<int,std::string>, LocalCarry>` —
the per-(station, code key) receiver code-bias memory with RW q 1e-9 m²/s; on a leg its 10σ inject gate
withholds 26 % of priors vs 13 % on the seed) and the **T5 per-LOS iono deviation carry**
(`ctx.carry.sh_dev_carry`, `std::map<std::string, ShDevCarry>` keyed "STATION|SAT"; 400 cold seeds at +1).
The rx-CB restart re-splits code bias / iono / clock per station and signal, which is exactly the shape of the
raw residual (per-station, not iono-ratio'd); the deviation restart is the a₀₀ transient.

**Build (default-OFF, byte-identical when off, forward-only, not fingerprinted — the Z-row precedent).**
Write knob `network.state_snapshot_station_capture`: rows `RC,<station index>,<cb key>,<value_abs>,<var>,
<last_seen_s>` (the same `LocalCarry` fields the Z rows serialize, one per rx-CB carry entry) and
`SD,<STATION|SAT>,<every ShDevCarry field>` under the existing header.  Read knob
`network.resume_station_restore` (requires `resume_frame_restore`): restore the rx-CB entries through
`makeRestoredLocalCarry` (the restored-arc sentinel, §15.4.1) and the deviation entries as they were, before
the first epoch; either half absent ⇒ inert and logged; counters + run-end line.  Two sub-knobs are NOT wanted
— the halves are gated by which rows the snapshot carries and by two leg-side switches
`resume_station_restore_rxcb` / `_shdev` (default true; diagnostic only) so one leg can restore one half.

**Instruments.**  The raw estimator level is visible only with the two outputs-only emit transforms off
(`carry_invariant_emit_rebase: false`, `emit_pb_filter: false` — not fingerprinted, §21.10), so the legs come
in two seeds written by the same exe from the current golden: `runs/evseed_field6h_v5` (the golden + the write
knob; carries every row kind of the day — the harness seed from now on) and `runs/evseed_field6h_v5raw` (v5
+ the two transforms off; the estimator-level instrument).  Legs, 60 epochs from ep0604, one exe:
from v5raw — `rs6_raw_ctl`, `rs6_raw_both`, `rs6_raw_rxcb` (rx-CB half only), `rs6_raw_shdev`;
from v5 — `rs6_null_ctl`, `rs6_null_both` (+ the stateful-consumer splice on both).

* **P8a the raw level (v5raw legs, products = raw state):** raw PB leg − seed at +1 p50 ≤ 0.5 m (from 1.70;
  excluding the six L2X keys whose datum receiver drops); receiver clocks leg − seed at +1 p50 ≤ 0.3 m
  (from 1.21); SH a₀₀ at +1 within 2 TECU (from +4.6); the rx-CB gate's withheld share on the leg within 3
  points of the seed's 13 %.
* **P8b the wire (v5 legs):** W6 code bias (B records) leg − seed pooled p50 ≤ 0.10 m (from 0.47); clock
  contrast pooled p50 ≤ 0.5 m (from 1.74) and at +2 ≤ 0.2 (from 0.54); ZWD p50 ≤ 0.32 m (from 0.64); PB wire
  unchanged (0.000 at +1, RS-5); the stateful consumer's code-bias re-references at the seam ≤ 5 (from 21)
  and its phase-bias resets 0.
* **P8c attribution:** the rxcb-only and shdev-only legs partition the P8a moves; the half that moves a
  channel owns it.  Both halves passing their share ⇒ RS-6 closes; a residual left with both restored names
  the next memory (the receiver phase-bias states, the six L2X families' datum receiver).
* Byte-identity: write knob on/off products identical; a cold run with the read knob inert; suite green.

### 21.14 RESULTS — RS-6a: the two station-layer memories partition cleanly, the T5 half removes the a₀₀ seam transient; the persistent residual is not `zwd_seam_reseed`; RS-7 pre-registered (2026-09-13 ~03:00 local; worker on worktree-agent-adf16ebd10c1b98dc `ba6be47`, merged `a726206`, registered `cb4fca4`; manager grade)

**Build.**  `RC` rows (the Z rows' `LocalCarry` field set — complete for this carry: its harvest stashes exactly
`value_abs / var / pl_now_s` and its inject reads only those) and `SD` rows (the four `ShDevCarry` scalars);
restore after the RS-5 emit restore; the rx-CB entries through the restored-arc sentinel (inert for this carry
today — no reader tests an age — locked by a test; the load-bearing field is `last_seen_s`, which the gap
inflation `var + q·Δt` and the 900 s prune key on) and the deviation entries as written (their harvest stamp
`t_s > 0` is what makes them read WARM; the aging pre-pass ages them over the true gap and drops them at 5τ);
two diagnostic sub-switches partition the halves.  Cold smoke `products.txt` **`87a33bf0…` both arms**,
snapshot +61 RC / +376 SD rows; Debug suite 2013 / 275 suites / 0 failed; `ResumeStationRestore` (12).
Seeds `runs/evseed_field6h_v5` (the harness seed from now on: every row kind of the day; its products are v4's
with the serialized counters, 0 differing values) and `v5raw` (emit transforms off — the estimator instrument);
ep0604 at the same tow, fingerprint `545b8bdd2b638231` unchanged.

| leg − seed, p50 (p95) | raw ctl | raw both | raw rx-CB only | raw T5 only |
|---|---|---|---|---|
| raw PB at +1, ex L2X | 1.68 (4.06) | **1.50 (2.83)** | 1.46 (3.82) | 1.72 (3.03) |
| raw PB at +2, ex L2X | 1.75 (4.66) | 1.51 (3.34) | 1.56 (4.55) | 1.63 (3.66) |
| receiver clocks at +1 (61 sta) | 1.21 (5.11) | 0.93 (5.70) | **0.91 (5.10)** | 1.12 (5.31) |
| SH a₀₀ at +1 / +6 (TECU) | **+4.62 / +0.46** | **+0.43 / +0.11** | +4.71 / +0.44 | **+0.32 / +0.11** |
| T5 cold seeds over the leg (seed: ambient) | 400 | **50** | 400 | 50 |
| rx-CB gate withheld share (seed, same window: **11.0 %**) | 26.2 % | 26.3 % | 26.3 % | 26.2 % |

| wire (v5 legs), leg − seed | null ctl | null both |
|---|---|---|
| PB wire at +1 / +2 / +30 p50 | 0.0004 / 0.0008 / 0.038 | 0.0004 / 0.0007 / 0.048 |
| W6 code bias at +2 / pooled p50 | 0.128 / 0.472 | 0.152 / 0.463 |
| clock contrast at +2 / pooled p50 | 0.441 / 1.593 | **0.291** / 1.503 |
| ZWD pooled p50 / p95 | 0.625 / 4.97 | 0.642 / 5.00 |
| stateful consumer at the seam: PB / code / clock re-refs | 0 / 21 / 0 | 0 / 22 / 0 |

**Verdicts.**  P8c the partition is clean: the T5 deviation half OWNS the SH a₀₀ seam transient (+4.62 → +0.43
with both, +0.32 alone; rx-CB alone +4.71; cold seeds 400 → 50 = the seed's own ambient rate) and halves the +2
clock contrast; the rx-CB half owns the receiver-clock move at +1 (1.21 → 0.91 m) and the raw PB tail (p95
4.06 → 2.83 m).  P8a: a₀₀ PASS (≤ 2 TECU); raw PB p50 1.50 (bar 0.5) and receiver clocks 0.93 (bar 0.3) FAIL;
the rx-CB gate share does not move.  P8b: PB wire unchanged at 0.000 (PASS); code bias 0.46 (bar 0.10), clock
contrast pooled 1.50 (bar 0.5), ZWD 0.64 (bar 0.32), consumer code re-references 22 (bar 5) FAIL.  Both knobs
RECOMMEND_SCOPED, the write knob pinned on the goldens, the harness arms both (`cb4fca4`) — the T5 half is a
clean win and the rx-CB half a modest one; neither is the residual.

**The residual is structural, and `zwd_seam_reseed` is not it (probe `rs6_{raw,null}_both_nozwd`).**  The
rx-CB gate withholds 26 % of its priors on EVERY leg — the whole 60 epochs, not a transient — against 11.0 %
on the continuous seed over the identical window; the code-bias level (0.46 m), ZWD (0.6 m) and the pooled
clock contrast (1.5 m) sit with it.  The one thing every leg had that the seed never has, the harness's ZWD
seam re-seed (61 Z rows as B9 priors at q 1e-9 for 480 epochs — validated on the sim, §17, never on the
field, where the ZWD channel is the datum sink), was switched off on both postures: ZWD 0.64 → 0.41 m (p95
5.0 → 2.6), receiver clocks 0.93 → 0.75, the raw PB tail at +30 15.3 → 9.1 m and the pooled clock-contrast
p95 9.0 → 5.1 improve; a₀₀ at +1 +0.43 → +0.73 TECU and no longer decays (+0.36 at +6), the +2 clock contrast
returns to the control's (0.29 → 0.44), the code bias worsens (0.46 → 0.50) and D5 trips 53 at +1 instead of
38; the rx-CB gate reads 26.2 % either way and the consumer 22 code re-references either way.  Mixed on the
field, and NOT the residual's cause: the harness default stays armed and the `zwd_seam_reseed` ledger row gets
this field measurement as a scope note, with the decision deferred to RS-7 (the ZWD / DCB / clock split is
what the shared border carries, and that border is the one memory every leg still rebuilds from one epoch).

**RS-7 pre-registered — the dense shared border across the seam.**  Every leg reads `Network (C1): warm
resume restored 256 diagonal carries — the full-covariance carry re-seeds at the first emitted epoch`: the
D2 shared carry (clocks, W6 code biases, SH coefficients) comes back DIAGONAL although the seed's snapshot
carries its dense covariance (the 150 QK/QP rows of `state_snapshot_coupling_capture`), because the dense-border
restore (§18.12 (2a), `NetworkRunContext.cpp` ~:1572–1614) lives behind `resume_coupling_restore`, which the
loader refuses on this lineage for an unrelated reason (it hard-requires Card 1f, out of regime under
`carry_invariant`).  A shared border that restarts with one epoch's information behaves exactly like the
residual: a noisier per-epoch DCB / clock / SH split for as long as the border's cross-covariance is thinner
than the seed's, more 10σ withholds at the rx-CB inject, a metre-class ZWD sink and a code-bias level that a
stateful consumer re-references.  Build: split the dense-border restore out of `resume_coupling_restore` into
its own read knob `network.resume_border_restore` (default false; forward-only, fingerprint-exempt; requires
`resume_frame_restore`; restores the QK/QP dense D2 covariance for the restored D rows — the coupling ROWS
(QL/QZ regressors) stay behind the 1f interlock), counters + run-end line; write side unchanged (the rows
already exist).  Legs from `v5raw` / `v5` ep0604, one exe: `rs7_raw_ctl` / `rs7_raw_border`, `rs7_null_ctl`
/ `rs7_null_border` (+ the stateful splice).
* **P9a the border is dense from +1:** the C1 line reports the dense restore (no "diagonal" clause); first-fold
  clock trips (the §18.11 counter) at the seed's level.
* **P9b the residual closes:** rx-CB gate withheld share within 3 points of the seed's 11.0 %; W6 code bias
  pooled p50 ≤ 0.10 m (from 0.46); clock contrast pooled p50 ≤ 0.5 m (from 1.50); ZWD pooled p50 ≤ 0.32 m
  (from 0.64); raw PB at +1 p50 ≤ 0.5 m ex L2X (from 1.50); receiver clocks at +1 ≤ 0.3 m (from 0.93); the
  stateful consumer's code re-references at the seam ≤ 5 (from 22) with phase resets 0.
* **P9c** PB wire unchanged at 0.000 (RS-5); a₀₀ within 2 TECU; the `zwd_seam_reseed` pair re-run on the
  border-restored posture decides that knob's field scope.

### 21.15 FINAL CERTIFICATION of the day's tree (2026-09-13 ~04:30 local; worker on main at `cb4fca4`; docs commits after it only)

Debug and Release rebuilt with the wrapper, first attempt.  **Full Debug suite on main: 2013 cases / 275 suites,
2012 passed, 1 skipped (the documented fixture gate), 0 failed.**  Smokes on `gnss_processor_close3.exe`
(3E83D30A…): the golden as it ships and the same with all four write knobs (`state_snapshot_carry_lin` /
`_datum_capture` / `_emit_capture` / `_station_capture`) false both hash `products.txt` **`87a33bf0…`** — the
write side of every card is byte-identical; the snapshots separate exactly by knob (`base` writes RC 61 / SD
376 / ER 135 / PF 135 / ZL 4 / EQD 6 / CBP 129; `nowrite` none of them); posture rows correct, no unknown-knob
warnings.  Closing leg `runs/evleg_close3_null_main` (v5 ep0604, `event_leg.py` defaults, which now arm every
resume knob of the day): 60/60, every seam and run-end line as §21.14's `rs6_null_both`, and its
`products.txt` **byte-identical to `rs6_null_both`'s** (`c45e52a0…`) across two independently linked binaries.

The seam card's state at close: the field golden warm-resumes with the DD branch, both datums, the emit layer
and the station layer continuous; the phase-bias wire is continuous to 0.000 m at the first epoch and the
a₀₀ seam transient is gone; a stateful consumer sees 0 phase-bias resets at the seam.  What remains is the dense
shared border, restored DIAGONAL on this lineage (RS-7, §21.14) — the code-bias level, the ZWD sink offset, the
pooled clock contrast and the rx-CB gate's 26 % share all sit with it.

### 21.16 RS-8 pre-registered — the six L2X families' "datum drop" is the per-station CANONICAL-SIGNAL latch re-deciding at the resume epoch, not a datum memory (2026-09-13 ~06:40 local; read-only investigation on the existing legs, manager pre-registration BEFORE any leg is read)

**The finding (no new run; Opus investigation on `evseed_field6h_v5` / `v5raw` and the RS-6 legs).**  §21.10–§21.14 graded
raw phase bias "ex L2X" because the six GPS L2X families step ~25 m at +1 and their eq34 component datum
receiver GAMB00PYF "vanishes" (→ CPVG00CPV).  The datum memory is complete and IS restored: the ep0604 snapshot
carries `EQD,G|L2X,GAMB00PYF`, eleven `EQS,G*_L2X,GAMB00PYF` roots, the `PA` pins and the parent edges
INEG00MEX←G31 / OAFA00ARG←G27, and the leg's RS-3 line restores all of it (6 sticky receivers, 176 parent edges,
126 roots, 302 pin targets).  The seed does not re-elect at the seam (its PHASE_DATUM rows at 277290–277350 carry
zero L2X; its L2X tracker set is `{GAMB, INEG, OAFA}` for all 720 epochs).  The leg's election falls through the
sticky and incumbent tests (`NetworkChainDatum.cpp:306–353`, then the reref stamp at `:507–510`) for one reason:
GAMB00PYF is not in the L2X `rx_list` because it contributes **no L2X arc** — its L2X tracker set at +1 is
`CPVG|G10 G12 G15 G23 G24 G25 G29 G32, INEG|G10 G28 G31 G32` (12 arcs) against the seed's
`GAMB|G04 G08 G09 G27 G31, INEG|…, OAFA|…` (15 arcs); the unions are exactly the emitted L2X key sets (seed 11
satellites, leg 10, intersection = the six that re-reference; the other five have no L2X column at all ⇒ "None").

The cause is `PppModel::decideConstellationCanonicals` (`src/core/model/PppModel.cpp:414`, guard `:427`, lock
`:479`; member `include/model/PppModel.hpp:513–523`): per station, on that station's FIRST processed epoch, it picks
the per-(band, obs-type) signal with the highest satellite count and locks it for the run.  The latch is in no
snapshot and its "Canonical signals locked for GPS …" line goes to the `ppp_model` logger, which is 0 bytes in every
run — the swap is silent.  RINEX tallies at the two run starts reproduce every observed tracker set:

| station | 00:00 (seed ep 1) | 05:02 (leg ep 1) | seed canonical | leg canonical |
|---|---|---|---|---|
| GAMB00PYF | L2W 9 / **L2X 10** | **L2W 10** / L2X 8 | L2X | **L2W** |
| CPVG00CPV | **L2W 10** / L2X 7 | L2W 8 / **L2X 11** | L2W | **L2X** |
| OAFA00ARG | L2W 7 / **L2X 8** | **L2W 9** / L2X 8 | L2X | **L2W** |
| INEG00MEX | L2W 1 / **L2X 7** | L2W 2 / **L2X 7** | L2X | L2X |

On the matched raw pair (`v5raw` vs `rs6_raw_both`) the six L2X keys read p50 |Δ| **25.6 m** at +1 against 1.50 m
for the other 123 — §21.10's 24.7 m to the digit.  (A latch that decides a station's canonical L2 signal from ONE
epoch's satellite count is a design smell in its own right — GAMB's L2W/L2X counts differ by one or two satellites
at both instants — but that is not this card: the continuity defect is that a resume RE-decides it.)

**Build (RS-8; the RS-3/RS-5/RS-6 pattern — default-OFF, byte-identical when off, forward-only, NOT fingerprinted).**
Write knob `network.state_snapshot_signal_capture`: one row `CN,<station index>,<constellation>,<band>,<C|L>,<signal>`
per locked canonical (from `PppModel::exportCanonicalSignals()` over `constellation_canonical_` +
`_decided_`), format-forward under the existing header; the field golden pins it (write side only, products
byte-identical) and `event_leg.py write-seed` arms it.  Read knob `network.resume_signal_canon_restore` (requires
`resume_frame_restore` — the arcs the restored realization frame names are the writer's canonical signals'):
at a warm resume, after the RS-3 datum restore, restore each station's map (`PppModel::restoreCanonicalSignals()`)
so the `:427` guard treats the station as decided; a snapshot without `CN` rows is inert and WARNED (the rider that
makes this defect visible from now on: the "Canonical signals locked" line is re-emitted on `main_log` at every
decision AND every restore, with the counts); counters + run-end line.  Two leg-side diagnostics are NOT wanted.
Seeds: `runs/evseed_field6h_v6` and `v6raw` = the v5 recipes + the write knob (their products must be byte-identical
to v5's / v5raw's: 0 differing values — the RS-5→RS-6 precedent), ep0604 at the same tow, fingerprint unchanged.

**Gates, frozen (legs `rs8_raw_ctl` / `rs8_raw_canon` from `v6raw` ep0604 and `rs8_null_ctl` / `rs8_null_canon`
from `v6`, 60 epochs, one exe; every other RS restore at the harness default; `resume_coupling_restore: false`):**
* **Q1 the tracker set:** at +1 the leg's L2X tracker set in `network_if_slip.csv` is the seed's `{GAMB00PYF,
  INEG00MEX, OAFA00ARG}` and the emitted L2X satellite set is the seed's 11; every station's canonical map at +1
  equals the writer's (the restore line counts 0 disagreements).
* **Q2 the datum:** the first post-resume epoch's eq34 re-references drop from 8 to ≤ 2 (the seed's own two L5Q
  re-references at 277320), with zero L2X rows; the RS-3 restore line unchanged.
* **Q3 the level:** on the raw pair the six L2X keys' |Δ| at +1 p50 ≤ 2.0 m (from 25.6), i.e. at the other keys'
  level, so raw PB at +1 is graded over all 129 keys from here on; the other 123 keys within 0.05 m of the control
  leg's (the restore touches L2X only).
* **Q4 byte-identity:** cold smoke knob absent vs armed `products.txt` identical (`87a33bf0…` on the field golden);
  `rs8_raw_ctl` reproduces `evleg_rs6_raw_both`'s `products.txt` byte-for-byte (same posture, new binary — the
  resume path certified too); `v6` / `v6raw` products byte-identical to `v5` / `v5raw`; suite green.
* **Q5 the rider:** the canonical line appears on `main_log` for every station on the cold seed and, on the legs,
  reads "restored" (canon) / "decided" (ctl); a resume against a `CN`-less snapshot (a v5 seed) warns once.

### 21.17 RESULTS — RS-7: the dense border restores exactly, and it is NOT the residual (KEEP_OFF with the mechanism); RS-7b census pre-registered (2026-09-13 ~08:20 local; worker on worktree-agent-abc0bf1feef082b22 `0a7efa8`, merged; manager grade)

**Build.**  `network.resume_border_restore` (default false; throws without `resume_frame_restore`; warn-DISARMS
without `shared_carry_full_cov`; does NOT require Card 1f or `carry_conditional_coupling` — both locked by tests);
the §18.12 (2a) dense-border block gated on either knob and naming the knob that armed it; the C1 seam line made
truthful (the "diagonal carries" clause prints only when no dense restore follows); the first-fold diagonal census
armed on EVERY warm resume with the dense carry on (control legs read it too); run-end `Network (RS-7 border
restore)` line + a plain-warm-resume readout; `tests/test_resume_border_restore.cpp` (10).  Cold smoke `87a33bf0…`
knob absent AND armed; Debug suite 2023 / 276 suites / 2022 passed / 1 skip / 0 failed; exe `23C628B3…`.
The border under test: the 150 QK/QP keys are **29 CLK + 121 CB** (C1W 29, C2W 30, C1C 30, C5X 16, C5Q 14, C2X 2)
— no PhaseBias keys on this lineage (§project_network_fullcov_carry's regime caveat, confirmed) — and all 150 of
the 256 D rows' non-VtecSh keys restore (0 dropped).  `rs7_null_ctl` `products.txt` = `c45e52a0…`, §21.15's
certified hash, on a third independently linked binary.

| leg − seed (raw = v5raw, estimator; null = v5, wire) | raw ctl | raw border | null ctl | null border |
|---|---|---|---|---|
| C1 "diagonal carries" clause / RS-7 keys restored | YES / – | **no / 150 (0 dropped)** | YES / – | no / 150 |
| border keys folded DIAGONALLY at the first fold | 150 | **0** | 150 | 0 |
| raw PB +1 p50 (p95) ex L2X | 1.496 (2.825) | 1.457 (2.629) | 0.000 (0.002) | 0.000 (0.002) |
| receiver clocks +1 p50 (p95), 61 sta | 0.934 (5.70) | **1.124** (5.82) | idem | idem |
| SH a₀₀ +1 / +6 (TECU) | +0.43 / +0.11 | +0.45 / +0.51 | idem | idem |
| rx-CB gate withheld share (seed, same window: **11.01 %** = 403/3660) | **26.31 %** | **26.28 %** | idem | idem |
| W6 code bias +2 / pooled p50 | 0.152 / 0.460 | 0.140 / **0.804** | idem | idem |
| clock contrast +2 / pooled p50 (p95) | 0.291 / 1.651 (9.11) | 0.361 / 1.563 (**7.32**) | idem | idem |
| ZWD pooled p50 / p95 | 0.659 / 5.03 | 0.637 / 5.03 | idem | idem |
| D5 flags +1; births +1 / +2; eq34 rerefs | 38; 64 / 7; 64 | 39; 64 / 7; 64 | idem | idem |
| PB wire p50 +1 / +2 / +30 | – | – | 0.0004 / 0.0007 / 0.048 | 0.0005 / 0.0011 / 0.035 |
| stateful consumer at the seam: PB / code / clock re-refs (±30 s) | – | – | 0 / 22 / 0 | 0 / **25** / 0 |

**P9a PASS** — the border is dense from the first fold, 150/0, the diagonal count 150 → 0, zero dense-fold
fallbacks.  ⚠ Its second instrument is vacuous here: the §18.11 "first-fold clock trips" counter is the Card-Q3
decouple total and `network.clock_anchor` is not armed on the field golden, so it reads 0 structurally (the run-end
line now says so instead of letting the 0 read as health).  **P9b FAIL on every bar, with the border demonstrably
dense** — so §21.14's attribution is FALSIFIED.  What the border moves: clock contrast p95 9.11 → 7.32 (at +60
2.85 → 1.87), raw PB p95 2.83 → 2.63, ZWD 0.659 → 0.637.  What it moves the wrong way: the W6 code-bias level
0.460 → **0.804** pooled (every signal: C1C 0.29 → 0.48, C1W 0.88 → 1.07, C2W 0.47 → 0.78, C5Q 0.37 → 0.79, C5X
0.33 → 0.77), receiver clocks at +1 0.93 → 1.12 m, consumer code re-references 22 → 25.  The code-bias trajectory
INVERTS: the border arm is better at the seam (+1 0.070 vs 0.086, +2 0.140 vs 0.152) and then walks ~1.7× faster
(+10 0.495 vs 0.333, +30 0.844 vs 0.440, +60 1.139 vs 0.666) — restoring the writer's cross-covariance fixes the
first fold and then stiffens the code-bias block against a station layer that is NOT the writer's (invariant #1's
shape: a stiff prior across a state the run does not share).  What does not move at all: the rx-CB gate share,
**flat at 26 % on every epoch of both arms** against the seed's flat 10–11 % over the identical window.  **P9c
PASS** (PB wire 0.000 unchanged; a₀₀ Δ 0.02 TECU); the `zwd_seam_reseed` pair on the border posture reproduces
§21.14's mixed pattern unchanged (ZWD 0.637 → 0.430, clock contrast 1.563 → 1.168, rx clocks 1.12 → 0.82; CB 0.804
→ 0.918, a₀₀ +0.45 → +0.74 and no decay, D5 39 → 53), so that knob's field verdict does not change either.

**Decision.**  `network.resume_border_restore` = **KEEP_OFF on the field** with the mechanism above (the knob is
correct, tested, default-OFF and stays in the tree; re-test it after the station-layer residual is closed — the
sim's leg-E win (§18.14) was measured WITH the coupling rows and with a station layer that continued).  Not armed
by the harness; no golden change.  Harness notes: `ev_launch.ps1` wrote an empty exit-code line (`$p.ExitCode`
unpopulated; fixed by touching `$p.Handle`); a `CMakeLists.txt` edit landing during a configure leaves `build.ninja`
newer than it and the new test file silently uncompiled — count suites, touch `CMakeLists.txt`.

**RS-7b pre-registered — a CENSUS, no new run: WHERE the flat 26 % lives.**  Everything §21.14 called "the
residual" (rx-CB withheld share 26 % vs 11 %, code-bias level walk, ZWD 0.64 m, pooled clock contrast 1.5 m, 22
consumer code re-references) is persistent over the whole leg, so it is a per-epoch structural difference between
the leg's and the seed's state, not a transient.  Read from the existing `rs7_raw_{ctl,border}` legs and `v5raw`
over the identical window (epochs 605–664): (a) the rx-CB withholds per (station, code key, epoch) with the
innovation's magnitude, sign and σ — concentrated on a station/signal subset withholding EVERY epoch (a per-station
split) or diffuse (a gate calibration); constant per-key offset (a frame) or scatter (instability); which term of
"10σ + 0.5 m" fails; (b) what the inject innovation is measured AGAINST in the code (`NetworkRunContext.cpp` ~5231,
`RxCbInjectAction::SkipInnov`) — the writer's `x_lin`, the previous posterior, or the epoch's code-only estimate —
and what a withhold does to the carry (the fingerprint note at ~214 says this gate reads innovations against
`x_lin`, and an `x_lin` shift is a solve no-op that a screen still sees, SPEC_lane_slip_response §7.2); (c) receiver
clocks and ZWD per station, leg − seed as 60-epoch series: persistent per-station offsets, and whether they sit on
the withholding stations; (d) W6 code bias per (sat, signal) and clock contrast per satellite, leg − seed series:
common-mode per signal (a datum realization) or per satellite, and whether clock and code bias re-split per
satellite (CLK + CB per (sat, signal) leg − seed — the consumer-invariant sum — against each alone).  Candidate
mechanisms to weigh, none assumed: a station-side `x_lin` re-seeded at the resume that the gate reads (H1); a
per-satellite clock↔code-bias re-split invisible to the consumer (then the instruments overstate the residual and
the 22 re-references are its whole cost) (H2); a station subset with a persistently different clock/CB/ZWD split —
the field's ZWD datum sink (H3); a restored carry variance so tight that 10σ sits below the leg's own scatter (H4).
Deliverable: the mechanism with the evidence, the smallest increment, and its harness grade.

### 21.18 RESULTS — RS-7b census: the flat 26 % is the rx-CB inject gate reading a FROZEN, un-snapshotted linearization point re-seeded at the resume; RS-9 pre-registered (2026-09-13 ~08:50 local; read-only Opus census on the RS-7 legs, script `scratchpad/rs7b_census.py`; manager pre-registration BEFORE any leg is read)

**(a) Where the 26 % lives.**  One rx-CB key per station (`GPS_DCB`, 61 per epoch), so "by key" is "by station".
Over the identical window (tow 277320–279090, k = 1..60): seed 403/3660 = **11.01 %**, `rs7_raw_ctl` 963/3660 =
26.31 %, `rs7_raw_border` 962/3660, `rs6_raw_ctl` (no station-layer restore at all) 942/3599 = 26.17 %.  Per-epoch
counts are flat and deterministic — seed `6 6 6 … 7 7 7` (one step at k = 14), leg `17 17 … 16 16` — and the set is
**17 stations withholding 45–58 of 60 epochs** (KRGG 58, GUAM 57, SGOC 56, CAS1 55, GAMB 55, SOLO 54, OAFA 54, BOR1
54, KOKV 53, MAJU 52, LCK4 51, MRO1 51, COCO 50, CUSV 49, GOPE 49, LMMF 48, HKSL 45) against the seed's **7** (KOKV
57, CPVG 56, MRO1 54, LMMF 53, GOPE 52, GAMB 49, LCK4 46): 6 shared, 11 leg-only, 1 seed-only.  Concentrated,
per-station, fixed from k = 1.  **No guard term fails — the innovation does.**  `rxCbInjectGate`
(`include/filter/NetworkRunTypes.hpp:1535–1549`) withholds on `innov² > k²·(var_eff + guard_var)`, k = 10,
guard_var = 0.25 m², `var_eff = var + q·Δt` with q = 1e-9: measured `var_eff` p50 1.30e-2 m², so the 0.5 m guard is
95 % of the σ² and the bar is a fixed **5.13 m** — on a quantity whose epoch-to-epoch motion is 9.4e-4 m p50.  H4
(gate calibration) is dead: `var_eff` would have to grow 400× to move the bar 20 %.

**(b) What the innovation is measured against — the proof.**  `ApplicationNetworkAssembly.cpp:3954`:
`cb_innov_raw = value_abs − x_lin(cb_idx)`; `x_lin` is the station StateManager's code-bias state, seeded ONCE by
`StateInitializer::initializeCodeBiases` (`StateInitializer.cpp:656`, the median over satellites of a code-residual
DCB, written at `:749/:754`) on the station's FIRST processed epoch behind the one-shot `isCodeBiasInitialized()`
(`MeasurementModel.cpp:1799–1802`), never written back; `rx_cb_carry_demean` / `_harvest_demean` /
`rx_cb_carry_step_gate` are all off here, so the raw innovation is the tested one.  At k = 1 the leg has restored
all 61 RC entries, so `value_abs`, `var` and `last_seen_s` are bit-identical to the seed's at the same tow, the gap is
30 s in both, k / q / guard identical — same numerator, same threshold, **6 withheld in the seed, 17 in the leg**: the
only input left is `x_lin`.  `rs6_raw_ctl` (carry NOT restored) trips the same ~16 from k = 2 — the set is a property
of the leg's `x_lin`, not of the carry's provenance.  `x_lin` is in no snapshot row (RC = `value_abs / var /
last_seen_s`, `NetworkStateSnapshot.cpp:257–259 / :686`) and nothing restores it.  **What a withhold does:** the fold
is skipped (`:4006–4009`, zero force) but the harvest overwrites the entry unconditionally
(`NetworkRunContext.cpp:5328`) and the prune only drops > 900 s — a withholding station is refreshed and refused
every epoch, forever, with no rx-CB memory, while the run-end line reads "61 carried" (invariant #4 blind spot).
Its carry variance is 1.3e-2 m² against a folding station's 3e-5.

**(c)(d) The consequences sit on the withholding stations, and the clock error is real to the consumer.**  Leg −
seed with the per-epoch across-station median removed (invariant #5): the 18 withholding stations carry CB
`value_abs` p50 **0.510 m** vs 0.019 on the 44 folding ones (26×, essentially exclusive), receiver clock 1.94 vs 1.03,
ZWD 0.95 vs 0.62 (worst SOLO CLK 16.8 / ZWD 7.8, CPVG 8.2 / 4.1, CAS1 5.3 / 3.5).  H3 is inverted — the per-station
split is the CONSEQUENCE of the withhold (the set is fixed at k = 1 before any split can develop).  Products leg −
seed over 60 epochs: clock contrast p50 1.497 (p95 8.99), code bias 0.427, and the consumer-invariant sums |CLK + CB|
1.566 / |CLK − CB| 1.397 against |CLK| 1.497 alone, per-satellite corr(ΔCLK, ΔCB) = +0.59 (a re-split would be ≈ −1):
**H2 falsified**, the 22 code re-references are not the whole cost.  Structure: per-signal means ≈ 0 (only C2X +0.378,
the §21.16 L2X key) — not a datum realization; a per-(sat, signal) CONSTANT explains 85.9 % of the ΔCB variance (per
satellite alone 29 %) — a frozen level offset per OSB key, the "level walk" shape; clock contrast per satellite
persistent (G16 +9.2 ± 5.2, G21 −8.6 ± 2.8).

**Mechanism (H1).**  RS-6 restored the rx-CB carry's numerator and left its reference cold: `x_lin(cb)` is a frozen,
arbitrary linearization point re-derived at the leg's first epoch from one epoch of code at 05:02 instead of 00:00;
the gate's only statistic is the distance from it, so 11 extra stations sit outside a 5.13 m bar for the whole leg,
lose their receiver-DCB memory permanently, and the receiver-DCB / satellite-OSB / clock / ZWD split re-realizes on
them.  Invariant #7 exactly: the re-coupling edge re-validated the value it reconnected but not the frame it is
judged against — RS-8's defect family (a per-station, first-epoch, un-snapshotted, silent decision; GAMB / OAFA /
CPVG sit in both sets, `resolveReceiverBias` feeds `initializeCodeBiases`).  ⚠ **An open defect in its own right,
NOT this card:** the gate is frame-blind on the continuous run too — 7 of 61 stations are refused for the whole 6 h
on the seed (no rx-CB memory all day).  The frame-invariant screen (`rx_cb_carry_step_gate`, LV-4) is the correct
statistic but was graded net-harmful as built (SPEC_level_information_budget §11 / §23.3: +64 / +35 / +16 % post-fit
phase rms, needs LV-5) — a solve-affecting card with the level-channel instruments, owed, not tonight.

**RS-9 pre-registered — snapshot and restore the code-bias linearization point (the RS-3/5/6 pattern).**  Write knob
`network.state_snapshot_rxcb_lin_capture` (requires `state_snapshot_station_capture`): row `RL,<station idx>,<cb
key>,<x_lin>` beside the RC row (`NetworkStateSnapshot.cpp:258`, read at `:686`), format-forward.  Read knob
`network.resume_rxcb_lin_restore` (requires `resume_station_restore` with its rxcb half): the RL map is stashed at
the RS-6 restore (`NetworkRunContext.cpp:1689–1707`) and consumed at the station's first-epoch code-bias
initialization (`StateInitializer::setCodeBiasSeedOverride` checked inside the `samples` loop at
`StateInitializer.cpp:745–753`, `markCodeBiasInitialized()` for covered keys; uncovered keys fall back to today's
seed and are counted); a snapshot without RL rows is inert and warned; counters + run-end line.  Solve-invariant by
construction under this posture (a code bias enters linearly, an `x_lin` shift is an exact solve no-op —
project_network_xlin_invariance; the only consumer is this gate; `rx_code_bias_datum`, whose RHS reads `x_lin`, is
off here — the loader refuses the pair).  Not fingerprinted (forward-only resume response policy).  Rider (every
run, trace-class): the rx-CB run-end line names the stations that withheld > 90 % of their epochs ("carried NOTHING")
so the seed's 7 become visible.  Rejected variants: seeding `x_lin := value_abs` (over-corrects the leg to ~0 % against
the seed's 11 % and breaks the leg↔seed comparability every RS gate uses); arming LV-4.  Seeds `v7` / `v7raw` = the
v5 recipes + the write knob (products byte-identical to v5 / v5raw), ep0604 at the same tow, fingerprint unchanged.

**Gates, frozen (legs `rs9_raw_ctl` / `rs9_raw_lin` from `v7raw`, `rs9_null_ctl` / `rs9_null_lin` from `v7`, 60
epochs, one exe, every other RS restore at the harness default; `resume_border_restore` off):**
* **R1 the closing bar:** rx-CB withheld share over k = 1..60 = the seed's 11.0 ± 1 % (from 26.3), and the leg's
  withholding set = the seed's 7 stations (KOKV, CPVG, MRO1, LMMF, GOPE, GAMB, LCK4) ± 1; restored RL entries 61,
  uncovered 0.
* **R2 the consequences, the RS-7 P9b bars re-used unchanged:** W6 code bias pooled p50 ≤ 0.10 m (from 0.46); ZWD
  pooled p50 ≤ 0.32 (from 0.66); clock contrast pooled p50 ≤ 0.5 (from 1.65); receiver clocks at +1 p50 ≤ 0.3 (from
  0.93); raw PB at +1 p50 ≤ 0.5 ex L2X (from 1.50; the L2X exclusion stays until RS-8 lands); the stateful consumer's
  code re-references at the seam ≤ 5 (from 22) with phase resets 0; PB wire 0.000 and a₀₀ within 2 TECU unchanged.
  Each bar reported PASS/FAIL; the card closes on R1 even if R2 leaves a remainder — R2's remainder then names the
  next memory.
* **R3 byte-identity:** cold smoke knob absent vs armed `87a33bf0…`; `v7` / `v7raw` products = `v5` / `v5raw`;
  `rs9_raw_ctl` `products.txt` = `evleg_rs7_raw_ctl`'s (same posture, new binary); suite green.
* **R4 the rider:** the seed's run-end line names its 7 permanently-refused stations; the lin leg names the same 7;
  the ctl leg names 17.

### 21.19 RESULTS — RS-9 closes the rx-CB frame (withheld share 26.3 % → 9.8 %, the seed's 11.0 %); the pooled 60-epoch leg − seed bars are RETIRED as seam instruments (they measure realization divergence); RS-10 pre-registered (2026-09-13 ~13:20 local; worker on worktree-agent-aac1e6222abdfe75f `3b6459f`+, merged; manager grade)

**Build.**  `RL,<station idx>,<cb key>,<x_lin>` rows paired 1:1 with the RC rows (61 on this lineage — the carried
`GPS_DCB`; the uncarried `GPS_IFB5` never reaches the gate) under `network.state_snapshot_rxcb_lin_capture`;
`network.resume_rxcb_lin_restore` stashes the map at the RS-6 restore and seeds each station's first-epoch code-bias
linearization point to the writer's value (`StateInitializer::resolveCodeBiasSeedOverride`, `markCodeBiasInitialized`
for covered keys); counters + run-end line; the rider on the always-on rx-CB line names the stations that withheld
> 90 % of their epochs ("carried NOTHING").  Cold smoke `87a33bf0…` knobs absent AND armed (the ON snapshot gains
exactly 61 RL rows); seeds `v7` / `v7raw` = the v5 recipes + the write knob, products byte-identical to `v5` /
`v5raw`, ep0604 at the same tow, fingerprint `545b8bdd2b638231`; `rs9_raw_ctl` / `rs9_null_ctl` `products.txt`
byte-identical to `rs7_raw_ctl` / `rs7_null_ctl`; Debug suite 2035 / 277 / 0 failed.  ⚠ The per-key "seeded from the
RESTORED linearization point" line goes to the `state_initializer` logger, 0 bytes on this posture (the §21.16
family); the counters are the instrument.

**R1 PASS — the frame was the mechanism.**  rx-CB withheld share over k = 1..60 (`scripts/rs9_census.py`, which
reproduces §21.18's 403/3660): seed **11.01 %**; ctl **26.31 %** (= `rs7_raw_ctl` to the row); **lin 9.78 %** — 94 % of
the 15.3-point gap closed, 1.2 points BELOW the seed (the ±1 band missed on the favourable side).  Withholding sets
(≥ 75 % of the window): seed 7, ctl 17, **lin 6** — every one of the 11 stations the defect created (KRGG GUAM SGOC
BOR1 SOLO MAJU COCO CUSV HKSL SCTB, CAS1 55 → 7) is back to 0/60; shared with the seed 5, only-leg 1 (OAFA00ARG),
only-seed 2 (CPVG00CPV, LCK400IND) — OAFA and CPVG are exactly §21.16's canonical-signal stations (RS-8, another
branch), LCK4 sits at 46/60 on the seed and 13/60 on the leg.  **R4 PASS:** the rider names 6 on the seeds (CPVG GAMB
GOPE KOKV LMMF MRO1 — LCK4 is 77 % over the window, under 90 % over 6 h), 16 on the ctl legs, 6 on the lin legs (GAMB
GOPE KOKV LMMF MRO1 OAFA).  Both arms restore identically at the RS-6 layer (61 rx-CB entries, 467 deviation
entries): the frame is the only difference.

**R2 — every pooled bar fails again, and the way it fails retires the bars.**  With 11 stations returned to folding:
W6 code bias pooled p50 0.460 → **0.545** (worse), clock contrast pooled 1.651 → 1.697 (worse; p95 9.11 → 8.93), ZWD
0.659 → 0.653 (flat), receiver clocks at +1 0.934 → 0.819 (12 % better), raw PB at +1 1.496 → 1.373 (8 % better), PB
wire 0.000 unchanged, a₀₀ +0.43 → +0.38, births / D5 / eq34 rerefs unchanged; the stateful consumer's code
re-references at the seam 22 → 24, phase resets 0 on both, cold-consumer settled 3D 11.79 vs 11.75 m.  The rx-CB
withheld population was therefore NOT what drives the pooled code-bias / clock / ZWD residual, and the RS-7 control's
own series says what does (`rs7_null_ctl/seam_grade.json`, leg − seed p50 by epoch):

| leg − seed p50 | +1 | +2 | +5 | +30 | +60 |
|---|---|---|---|---|---|
| clock contrast | 0.31 | 0.29 | 0.53 | 1.42 | 2.85 m |
| W6 code bias | 0.086 | 0.152 | 0.268 | 0.44 | 0.67 m |
| ZWD | 0.30 | 0.30 | 0.40 | 0.56 | 1.35 m |
| PB wire (DD-invariant) | 0.000 | 0.001 | 0.003 | 0.048 | 0.118 m |
| a₀₀ | +0.43 | +0.18 | +0.13 | −1.53 | −1.19 TECU |

Every shared channel — including the DD-invariant phase-bias wire and the SH a₀₀ — diverges roughly linearly from a
near-identical state on identical data: two realizations of a weak-datum, random-walk shared parameterization walking
apart as their gate decisions differ, the chain-datum churn the literature leaves unsolved (project_ppprtk_literature
_pack).  The 60-epoch pooled bars of §21.13/§21.14 (CB ≤ 0.10, clock contrast ≤ 0.5, ZWD ≤ 0.32) therefore graded
DIVERGENCE, not the seam — which is why RS-7 and RS-9 could each close their own mechanism exactly and leave those bars
untouched.  **From here the seam is graded at the SEAM EPOCH (+1 / +2 against the seed's own per-epoch step) and by
consumer instruments (the stateful splice's re-references at the seam; the cold-consumer replay), never by pooled
leg − seed levels over a window.**  On those instruments the seam's remaining cost is two items: the ZWD step at +1
(0.30 m p50 with `zwd_seam_reseed` ARMED, which should have pinned it to the writer's value) and the 22–24 code-bias
re-references the stateful consumer takes at the seam.

**Decision.**  Both RS-9 knobs RECOMMEND_SCOPED: the write knob pinned on both goldens (write side, byte-identical;
requires `state_snapshot_station_capture`, pinned), the read knob harness-armed on every leg.  `n_resume_rxcb_lin_
uncovered` counts only keys that have a carry (0 here).  The next harness seed (v8) carries CN + RL rows once RS-8
merges.

**RS-10 pre-registered — a CENSUS, no new run, on `rs9_null_lin` / `rs9_raw_lin` vs `v7` / `v7raw`.**  (a) The ZWD
seam step: per station at +1 and +2, the leg's ZWD vs the seed's, the snapshot's Z-row values vs the seed's ZWD trace
at ep0604/0605, and the leg's ZWD linearization point / a-priori vs the writer's (`zwd_seam_reseed` injects the Z rows
as B9 priors at q 1e-9 — in which frame, correction or absolute?); the same for the receiver clocks at +1 (0.82 m p50
— per-epoch white, so 0.82 m is the seam's per-station re-split at +1); is the 0.30 m the RS-9 family again (a
re-seeded `x_lin` for ZWD), a datum-sink restart, or the reseed acting in the wrong frame?  (b) The seam
re-references: which 22 (sat, signal) keys the consumer re-references, whether each is DECLARED (B-record disc
counter / code jump test) and its |Δ| at +1 leg − seed, against the seed's own per-epoch code-bias step; whether they
are the time-linked code-bias datum's per-key re-seed (RS-3 restored the CBP map with the latch false — did those keys
re-seed?), the W6 set-mean realization, or the receiver side.  Deliverable: mechanism, the smallest increment, and
its seam-epoch bar (ZWD at +1 within 3× the seed's own step; consumer code re-references at the seam ≤ 5).

### 21.20 RESULTS — RS-8: the canonical-signal latch flips on 23 of 61 stations at a resume, not three; restoring it takes chord births at the seam 64 → 1 and every phase-bias key's level to the seed's (2026-09-13 ~14:30 local; worker on worktree-agent-a533ac4a027ca01be `d4a56fa` + merge; manager grade)

**Build.**  `PppModel::exportCanonicalSignals / restoreCanonicalSignals`; row `CN,<station idx>,<constellation>,<band>,
<C|L>,<signal>` under `network.state_snapshot_signal_capture` (61 stations × 6 entries = 366 rows on this lineage);
`network.resume_signal_canon_restore` (requires `resume_frame_restore`; INERT-warned on a CN-less snapshot) restores
each station's map before its first epoch so the `:427` guard treats it as decided; counters (stations / entries /
disagreements with what the first epoch would have decided) + run-end line; the rider re-emits the "Canonical
signals" line on `main_log` at every decision AND restore (`decided` / `restored` / `audited`); one call in
`ApplicationNetwork.cpp` after `networkTryWarmResume`, body in the seam; `tests/test_resume_signal_canon_restore.cpp`
(13).  Cold smoke `87a33bf0…` knobs absent AND armed (the ON snapshot gains exactly the 366 CN rows); seeds `v6` /
`v6raw` = the v5 recipes + the write knob, products byte-identical to `v5` / `v5raw`, ep0604 at the same tow,
fingerprint `545b8bdd2b638231`; `rs8_raw_ctl` `products.txt` byte-identical to `rs6_raw_both`'s (the ctl reading a
CN-bearing snapshot it ignores); Debug suite 2036 / 278 / 0 failed on the merged tree.  ⚠ The worker's worktree was
created 285 files behind main and reset to main's tip before any edit (feedback_agent_worktree_stale_base).

**Q1 PASS.**  At the first IF-TD-traced epoch (277350) the canon leg's L2X tracker set is the seed's — 15 arcs on
{GAMB00PYF, INEG00MEX, OAFA00ARG}, arc for arc — against the ctl's 12 on {CPVG00CPV, INEG00MEX} (§21.16 reproduced);
emitted L2X satellites at +1 11 = the seed's (ctl 10).  The restore line's disagreement count: **34 of 366 entries
across 23 of 61 stations** — the latch does not flip on three stations, it flips on twenty-three: mostly L1 code /
phase ties (`C1C` vs `C1W`, `L1C` vs `L1W`, decided by one or two satellites), plus the three L2 flips §21.16 named
(GAMB L2X→L2W, OAFA L2X/C2X→L2W/C2W, CPVG L2W→L2X) and a `C2W`↔`C2L` at CEBR00ESP.  The cold seed's rider caught the
defect in the act: `GAMB00PYF (10 sats): … L2X(L2:10)` at 00:00 against `L2L keeps L2X vs L2W(10)` at 05:02.

**Q2 PASS.**  eq34 re-references at 277320: ctl 8 (2 L5Q + 6 L2X), **canon 2** (G09 and G27 on L5Q — the seed's own
two), zero L2X; the RS-3 restore line identical on both.

**Q3 — the level.**  Raw pair, per-key |Δ| at +1 vs `v6raw`: the six L2X keys **25.63 → 0.62 m** p50 (max 70.6 →
1.53; bar ≤ 2.0, PASS with 41× margin) — and the OTHER 115 keys **1.4957 → 0.6009 m** p50 (max 4.66 → 2.25).  The
pre-registered "within 0.05 m of the control" bar assumed the restore touched L2X only; it touches the channel
23 stations contribute, so the whole realization moves — toward the seed, 2.5× better.  The bar is rewritten as "no
worse than the control" and PASSES; the 1.50 m "raw PB at +1 ex L2X" figure that RS-6/RS-7/RS-9 carried was the
latch's own cost on the other 20 stations.  Raw PB at +1 is graded over all 129 keys from here on.

| wire pair (`rs8_null_*` vs `v6`), leg − seed | ctl | **canon** | seed's own |
|---|---|---|---|
| chord births at +1 | 64 | **1** | 2 |
| D5 flags at +1 | 38 | **29** | — |
| clock contrast p50 at +1 / +60 | 0.306 / 2.846 | **0.160 / 0.886** | — |
| W6 code bias p50 at +1 | 0.086 | **0.046** | — |
| ZWD p50 / p95 (window) | 0.659 / 5.03 | 0.446 / 4.24 | — |
| PB wire p50 at +1 | 0.0004 | 0.0004 | — |
| a₀₀ at +1; worst from +5 | +0.43; 4.49 @ +39 | +0.88; 2.80 @ +56 | — |
| σ̂₀ code ratio (G3) | 1.561 | **1.136** | — |
| consumer p50 from +5 (report-only) | 7.86 | 9.26 m | — |

**The structural headline: 64 → 1 chord births at the seam** — the ctl leg was re-birthing the ambiguities of every
station whose canonical channel had just flipped; the canon leg births 1 against the continuous run's 2.  (Even the
pooled channels §21.19 retired move: clock contrast window p50 1.65 → 1.06, at +60 2.85 → 0.89 — the latch was also
the largest seed of the two realizations' divergence.)

**Q4 PASS** on every clause (above).  **Q5 PASS:** ctl 62 `decided` per leg; canon 61 `restored` + 61 `audited` + 1
`decided` — the stray `decided` is the closed-loop CONSUMER's own `PppModel`, built outside `buildStation`, not in the
producer roster and not covered by the snapshot: the consumer re-decides its canonical channels at every warm resume
too (scope note; the consumer is not snapshotted at all and its own cold start dominates a 30-minute window, §18.14).
The probe `rs8_probe_v5` (read knob armed against the CN-less v5 snapshot): one INERT warn, the run-end
`0 restored under an ARMED read` clause, 62 `decided` lines, exit 0.

**Decision.**  Both knobs RECOMMEND_SCOPED: the write knob pinned on both goldens (write side, byte-identical), the
read knob harness-armed on every leg; the harness seed becomes `v8` (CN + RL rows) at the certification.  Two owed
items named, not built: the latch's tie-break itself (34 flips on one-satellite ties over a 5 h gap is a design
smell of `decideConstellationCanonicals` on the CONTINUOUS run too — a per-station canonical channel decided by one
epoch's count; owed as its own card with a field A/B), and the consumer's uncovered latch.

### 21.21 RESULTS — RS-10 census: the ZWD "step" is `zwd_seam_reseed` itself on a lineage that carries no ZWD, and the consumer's code re-references are all declared (2026-09-13 ~15:00 local; read-only Opus census on `rs9_null_lin` / `rs9_raw_lin` vs `v7` / `v7raw`, scripts `scratchpad/rs10_*.py`; manager grade; the single-knob confirmation leg pre-registered below)

**(a) The frame is clean, and the sink has no memory to restore.**  The Z row holds the ABSOLUTE ZWD (`NetworkRunContext
.cpp:5055`, `value_abs = x_lin + x_r`) and the B9 inject forms `η = λ(value_abs − x_lin(tropo))` with `x_lin(tropo) ≡ 0` on
both sides (the producer never writes its solution back into the station StateManager) — measured |Z row − seed
ZWD(ep0604)| = **0.0000 m on all 61 stations**; RS-9's `x_lin` family is falsified for the ZWD (lin 0.288 vs ctl 0.300 m
at +1).  What the field lineage lacks is a ZWD MEMORY at all: `carry_zwd` off, no F3 station block ⇒ the seed's
`carried` flag is 0 on all 43 889 ZWD rows — the ZWD is a per-epoch white datum sink on every continuous field epoch,
with its own epoch-to-epoch step **0.107 m p50 (rms 0.72)**.  The reseed pins that sink to one draw at q = 1e-9 m²/s
(σ 1.7e-4 m over 30 s — the measured increment implies q ≈ 1.7e-2, ~4000× in σ): the leg's `zwd` prior family runs
σ̂₀ p50 **11.2** / max|t| 23 (cb 3.2, rxcb 4.1 in the same file; the seed has no `zwd` family), folds every epoch and
is refused by the data — and from +2 the harvest re-stashes the LEG's own posterior (fold variance 2.3e-2 → 2.3e-5 m²):
the diagonal ZWD carry documented FALSIFIED on continuous runs (consumer 0.204 → 0.723 m rms), running for 480 epochs.
The receiver-clock "re-split" at +1 is the same event: across stations r(ΔZWD, ΔCLK) = **−0.897**, slope −1.71 (= −⟨mf_w⟩),
R² 0.80; the residual after the trade is 1.36 m rms — and 1.39 m in the reseed-OFF arm, flat over +1..+30 while the
totals grow (rms ΔCLK 3.08 → 6.62).  Against the estimator's own formal σ (seed σ(ZWD) p50 0.31, σ(CLK) 0.57 m):
reseed ON |ΔZWD|/σ 1.05, |ΔCLK|/σ 1.61; reseed OFF **0.68 and 0.99** — two realizations of the same estimator.  The
movers are weak-geometry stations (Spearman with the seed's own per-station step +0.77: MAC1, VOIM, PARK, KIT3, CEBR,
CAS1), not the rx-CB set (R² 0.015).  Over the hour the double count leaks to the wire: ZWD leg − seed p50 **1.338 vs
0.578 m** at +60, satellite-clock contrast p95 **10.29 vs 5.44** (the RS-6-era nozwd contrast, not single-knob; D5 trips
at +1 37 → 53, diagnostic-only under the RS-2b hold).  **Verdict: a datum-sink restart — and the reseed is the
perturbation, not the cure.**  `network.zwd_seam_reseed` is OFF on any lineage that does not carry the ZWD: its ledger
regime is narrowed from `phase_leveling` to `phase_leveling_local_block or carry_zwd` (the two mechanisms that give
the ZWD a carried flag; the sim golden has the block, the field golden neither), the posture row follows, and
`event_leg.py` arms it only on such lineages (`--extra` overrides).  Rejected: raising `zwd_carry_q` to soften the
prior — it would still run a ZWD carry for 480 epochs on the one lineage where that carry is falsified.

**(b) The seam re-references — 14 keys at +1 (24 over +1/+2), all DECLARED, all GPS code bias.**  G06 C1W/C2W/C5X, G12
C1W, G19 C1W/C2W, G24 C1W/C2W, G25 C1W/C2W/C5X, G32 C1W/C2W/C5X (ctl = the same minus G06 C2W); the phase-bias (29)
and clock (6) discontinuity totals are identical to the continuous reference — the consumer's whole seam cost is code
bias.  Declared 14/14: each leg's `code_disc` counter = the seed's + 1 at 277320 while the seed's own is unchanged;
producer-wide the leg bumps 81 of 120 keys at +1 against the seed's 7.  The consumer's test IS the declared counter,
no threshold (`RealTimeEngineSsrBreaks.cpp:169–175` → `SsrCorrectionStore.cpp:268–276` → `StateManager::noteSsr
Discontinuity`, any change in either direction), and its response is ONE re-inflation of the constellation's receiver
code-bias states — no ambiguity reset, no re-reference; 14 fires = one GPS-wide rx-CB re-inflation.  Magnitude: |leg −
seed| at +1 for those keys p50 **0.099 m** (wire arm bit-identical to the raw arm ⇒ estimator level), 14× the seed's own
per-epoch step for the same keys (0.007 m).  Mechanism: the producer's emit jump test (`NetworkProductEmitter.cpp:368–
381`, threshold 0.25 λ = 48–64 mm) tripped by a genuine realization move — 71 of the 81 bumps exceed it (0 false
negatives) — plus 10 `changed_channels` triggers from a channel-set change at the leg's first epoch (G|C1W 29 → 30
gaining G18; G|C2X G31 → G32) that the seed never had, i.e. RS-8's latch, retired with it.  Not the RS-3 datum (121
CBP + 32 CBS + 30 CBI restored, zero re-seed rows), not the receiver side (RS-9 moves it 12 mm the wrong way), not a
per-signal datum shift (signed channel means ≤ 1.2 mm against sd 0.09–0.25 m).  What remains after RS-8 is a
~5–10 cm per-key OSB realization move at the first post-resume fold, sitting at the jump test's own threshold and
answered by a benign consumer re-inflation — the seam's residual floor, read at the certification's closing splice.

**The confirmation leg, pre-registered (running):** `rs10_null_nozwd` = `rs9_null_lin`'s posture with
`zwd_seam_reseed` off (the one differing key, verified by config diff; same exe B3E6D744), graded against
`rs9_null_lin` and `v7`: the `zwd` prior family absent (n_folded 0); at +1 ZWD p50 ≤ 0.22 / p95 ≤ 1.5 m (from 0.288 /
2.32), receiver-clock rms ≤ 1.8 m (from 3.08), the ZWD-trade-removed residual 1.37 ± 0.10 m; the wire at +1 unchanged
(satellite-clock contrast p50 ≤ 0.34 / p95 ≤ 0.85, cb p50 ≤ 0.10, pb p50 ≤ 0.001); at +60 ZWD p50 ≤ 0.60 and
satellite-clock contrast p95 ≤ 6.0; D5 trips at +1 reported with re-taps 0; |ΔZWD|/σ and |ΔCLK|/σ at +1 ≈ 0.68 / 0.99.

**Confirmation leg RESULT (`rs10_null_nozwd`, single knob verified by config diff — `resume_signal_canon_restore` pinned false
in both arms because RS-8's harness default landed between the two legs; the v7 seed has no CN rows anyway): 11 of 12 bars
PASS.**  The `zwd` prior family is absent (0 rows, 0 folds; ON σ̂₀ 11.2); at +1 ZWD p50 **0.180** (ON 0.288) / p95 1.26 (2.32),
receiver-clock rms **1.65** (3.08), the trade-removed residual 1.356 (1.354); wire at +1 unchanged within the six-leg scatter —
code bias 0.092 (0.097), phase bias 0.0004 (0.0004), satellite-clock contrast p95 0.817 (0.816) and p50 0.368 vs the 0.34 bar
(FAIL as written: the bar was another leg's observed value with no margin; six legs span 0.275–0.368, sd 0.029, and the OFF − ON
gap is 0.46× the seed's own per-epoch step) ; at +60 ZWD p50 **0.584** (ON 1.338) and satellite-clock contrast p95 **5.36**
(10.29); |ΔZWD|/σ 0.65 and |ΔCLK|/σ 0.93 at +1 (the predicted 0.68 / 0.99); D5 trips at +1 51 (37), re-taps 0.  Removal is
confirmed on the producer's state channel and on the wire over the hour.  **And one unregistered channel goes the other way,
reproducibly:** the grader's report-only in-run consumer (a cold consumer started at the seam in every leg) reads 3D p50 over
k 5–32 **14.7 m OFF vs 8.8 ON** and |up| 12.1 vs 6.4 (p95 19.4 vs 7.9), crossing over by k 33–60 (7.3 vs 7.6) — on two
independent OFF arms (`rs10_null_nozwd`, `rs6_null_both_nozwd`) and two ON arms (`rs9_null_lin`, `rs9_null_ctl`), the
continuous consumer at 8.9.  That is the benefit `event_leg.py` armed the knob for ("the consumer vertical it feeds"), which
the census had no channel to see: the stiff prior double-counts the sink for the producer but hands a COLD consumer a
~15-minute smoother start.  The stateful-consumer splice (§21.14's RS-6 probe) saw no difference either way.  The two ends of
the stiffness axis each win one side — a genuine trade, not a defect — so the decision (OFF on non-carrying lineages) stands on
the producer evidence with the cold-consumer transient recorded as its named cost, and the bracket is pre-registered as one
config-only pair from the same seed: `network.zwd_carry_q` 3.8e-4 (p50-matched to the sink's own step) and 1.7e-2
(rms-matched), reseed ARMED, each a single knob vs `rs9_null_lin`; bars R1' (prior present, n_folded 3721, σ̂₀ p50 ≤ 2.0,
max|t| p50 ≤ 5 — an inert q FAILS), R2' / R4' at the OFF arm's levels (ZWD +1 ≤ 0.22 / p95 ≤ 1.5, rx-clock rms ≤ 1.8; +60
ZWD ≤ 0.60, contrast p95 ≤ 6.0), R3' wire within the six-leg scatter, C' the in-run consumer k 5–32 |up| p50 ≤ 7 m and 3D
p50 ≤ 9.5 m (the ON arm's levels) with k 33–60 not worse than either arm.  A q that holds both ends re-arms the reseed on
this lineage at that q; if neither does, the trade is decided by the stateful splice, not by the in-run consumer.

**The bracket could not run, and why is the finding (RS-10b pre-registered).**  `rs10_null_q4` was REFUSED at the resume:
`network.zwd_carry_q` is IN the warm-resume fingerprint (`NetworkRunContext.cpp:743`, under "carry q's … must COLD-start"),
while `zwd_seam_reseed` / `_epochs` are deliberately not — the seam response's only weight is borrowed from a continuous-run
carry parameter, so the one knob a resume response exists to tune is the one that cannot be tuned on a resume leg, which is
why 1e-9 was never questioned on this lineage.  RS-10b: `network.zwd_seam_reseed_q` (default −1 = use `zwd_carry_q`,
byte-identical when unset; read only inside the seam window; NOT fingerprinted, like every RS-x response knob), then the
bracket exactly as pre-registered above on the v7 seed (`rs10b_null_{on,q4,q2}`; the ON leg must reproduce `rs9_null_lin`
byte-for-byte on the new binary).

**RS-10b RESULTS — the bracket ran; the reseed's damage was its stiffness, and no single q holds both ends (2026-09-13 ~20:40
local; worker on worktree-agent-aaa93a7f642ccfdb7 `e2bf262`, merged `9cf83b3`; manager grade).**  `network.zwd_seam_reseed_q`
(default −1 = inherit `zwd_carry_q`, byte-identical; must be > 0 when set; one resolved `zwdQEff(seam_live)` read by the B9
diagonal fold, the B9-in-block fold and the harvest's prior-residual `fold_var`; NOT fingerprinted) — cold smoke `87a33bf0…`
both arms, suite 2060 / 280 / 0 failed, the ON reference reproduces `rs9_null_lin` byte-for-byte, and all four legs
warm-resume under the unchanged fingerprint.  The five arms on `v7` ep0604 (the six-leg scatter of a +1 p50 is ±0.03 m;
R2′'s bars were set at the OFF arm's own values without margin and are restated as multiples of the seed's own per-epoch
step, 0.107 m ZWD / 0.331 m receiver clock):

| leg − seed | ON 1e-9 | q 3.8e-4 | q 2.5e-3 | q 1.7e-2 | reseed OFF |
|---|---|---|---|---|---|
| `zwd` prior σ̂₀ p50; folds dropped by the var guard | 11.2; 215 | 1.32; 6 | 0.60; 1 | **0.24; 0** | absent |
| ZWD +1 p50 (× the seed's step) | 2.68× | 3.00× | 2.82× | 2.07× | 1.68× |
| rx-clock +1 rms (× step); max | 9.3×; 17.1 m | 9.0×; 16.3 | 7.9×; 12.6 | **5.9×; 5.9** | 5.0×; 5.9 |
| ZWD +1 max | 9.88 m | 9.44 | 7.31 | **3.25** | 2.48 |
| ZWD +60 p50 / contrast +60 p95 | 1.338 / 10.29 | 0.895 / **4.01** | 0.618 / 4.05 | **0.471** / 4.94 | 0.584 / 5.36 |
| wire at +1 (clock contrast p50 / cb / pb) | 0.327 / 0.097 / 0.0004 | 0.324 / 0.101 / – | 0.330 / 0.100 / – | 0.345 / 0.094 / – | 0.368 / 0.092 / 0.0004 |
| cold in-run consumer k 5–32 3D / \|up\| p50 | 8.8 / 6.4 | **6.4 / 2.3** | 7.3 / 3.1 | 12.2 / 9.8 | 14.7 / 12.1 |
| cold consumer k 33–60 3D / \|up\| | 7.6 / 3.9 | 6.3 / 1.5 | 7.1 / **0.97** | 7.0 / 1.9 | 7.3 / 3.3 |

The over-stiffness signature is monotone in q (tails, σ̂₀, dropped folds) and at 1.7e-2 the seam tails equal the OFF arm's
while the producer at +60 is BETTER than OFF (ZWD 0.471 vs 0.584) — §21.21's "the reseed is the perturbation, not the cure"
was a statement about q = 1e-9, not about the mechanism.  The cold consumer runs the other way: the best arms are the soft
ones (3.8e-4, 2.5e-3) and 1.7e-2 breaks it.  The crossover lies in (2.5e-3, 1.7e-2): q 2.5e-3 holds every consumer bar and
misses the producer end on exactly one, ZWD at +60 0.618 vs the 0.60 bar (3 %; the OFF arm itself 0.584), with σ̂₀ 0.60,
contrast p95 4.05 and the tails half-way down.  **Decision.**  No q holds both pre-registered ends, so by the
pre-registration the tie-break is the stateful-consumer splice, which is indifferent to the reseed (§21.14) — the reseed
stays OFF on lineages that carry no ZWD; `network.zwd_seam_reseed_q` is registered RECOMMEND_SCOPED (validated,
byte-identical, the only way the response can ever be tuned) and q 2.5e-3 is the re-arm candidate on record, re-tested
on the v8 seed as a second realization (the one 3 % miss sits inside the pooled p50's own scatter).  Two instrument
notes for the record: R1′d (max|t| p50) never passes at any q and is non-monotonic — it is not on the stiffness axis; and
the census counts 3720 of 3721 folds at 2.5e-3 because its own `var > 0` guard drops one station-epoch (the ON-path
run-end counter reads 3721 on every armed leg).

**RS-10b v8 RE-TEST — the re-arm candidate q = 2.5e-3 is REFUSED on the second realization (2026-09-14 ~01:30 local; manager-run
leg `evleg_rs10b_v8_q3` = `cert_null_v8` + `zwd_seam_reseed: true, zwd_seam_reseed_q: 0.0025`, single-knob pair by config
diff, both from `evseed_field6h_v8` ep0604; `scripts/ev_seam_grade.py` against the v8 seed; consumer channel REPORTED only,
§24.1).**  On the v8 seed — RS-8 + RS-9 in the snapshot — the reseed-OFF seam is far cleaner than the v7 OFF arm the bracket
was bracketed against (ZWD leg − seed +1 p50 0.086 vs 0.180; +60 0.092 vs 0.584), and the reseed at the crossover q is now
worse than OFF on every ZWD number and neutral on the wire:

| leg − seed | q 2.5e-3 (v8) | reseed OFF (v8, `cert_null_v8`) |
|---|---|---|
| ZWD +1 p50 / p95 | 0.169 / **2.17** | 0.086 / 0.42 |
| ZWD +2 p50 / p95 | 0.164 / 1.08 | 0.105 / 0.36 |
| ZWD +60 p50 / p95 | **0.296 / 4.70** | 0.092 / 0.60 |
| ZWD pooled +5..+60 p50 / p95 | 0.210 / 2.63 | 0.087 / 0.42 |
| sat-clock contrast +1 p50; +60 p50 / p95 | 0.140; 0.268 / 0.945 | 0.152; 0.255 / 0.929 |
| W6 code bias +1 / +60 p50 | 0.052 / 0.461 | 0.052 / 0.415 |
| phase bias +1 / +60 p50 | 0.0004 / 0.097 | 0.0004 / 0.109 |
| D5 flags at +1 (re-taps 0) | 37 | 45 |
| `zwd` prior family | 3720 folds, Σw·ê² 85.8 % of the prior side | absent (0 rows) |
| a₀₀ +1 / +60 (TECU) | +0.95 / +0.12 | +1.08 / −0.07 |
| in-run cold consumer p50 from +5 (reported) | 4.28 m | 8.94 m |

The reseed's prior is a perturbation of at least the size of the seam it means to smooth: at +60 it leaves the ZWD 3× further
from the continuous run with a 4.7 m p95 tail, while the wire channels are inside the six-leg scatter either way.  The
cold-consumer number runs the other way again (4.3 vs 8.9 m) and, per §24, cannot be read.  **Decision.**  No re-arm: the
reseed stays OFF on lineages that carry no ZWD memory; `network.zwd_seam_reseed_q` stays RECOMMEND_SCOPED (validated,
byte-identical, the only way the response can be tuned) with this re-test on its row; the crossover of §21.21 was a property
of the v7 seed's seam, not of the mechanism.

### 21.22 CERTIFICATION — the night's merges on main's own binaries; the v8 harness seeds; the closing legs graded at the seam epoch (2026-09-13 ~21:30 local; certification worker on main, parts 1–2 at `1bb0039`; part 3 re-certifies the final HEAD; manager grade)

**Part 1 (main `1bb0039`; `c1d94e8` binary-identical).**  Debug and Release rebuilt first attempt (both new test TUs compiled in
without a `CMakeLists.txt` touch); Debug suite **2049 cases / 279 suites / 2048 passed / 1 documented skip / 0 failed**
(the six resume suites 48/48); Release exe `29952251…`; smokes `_cert1_smoke_base` and `_cert1_smoke_nowrite` (every write
knob false) BOTH `87a33bf0…` — fourteen write-row families go exactly to zero under the write knobs and every other tag is
count-identical (the census delta vs `_rs6_smoke_off` is CN 366, RC 61, SD 376, RL 61 only); no unknown-knob warning;
the canonical-signal rider on `main_log` 61 + the consumer's 1.

**Part 2 — the v8 harness seeds.**  `runs/evseed_field6h_v8` / `v8raw` = the v5 / v5raw recipes under today's harness
(config diff = the two new write knobs + paths): `products.txt` **byte-identical to v5 / v5raw** (`0f510024…` / `c7f63437…`),
ep0604 at tow 277290.00000810623, fingerprint `545b8bdd2b638231`, the ep0604 census = v5's in every tag plus **RL 61 and
CN 366** (D 256 · ZL 4 · EQD 6 · EQP 176 · EQS 126 · EQT 302 · CBP 121 · CBS 32 · CBI 30 · ER 147 · PF 147 · PS 1 · RC 61 ·
SD 467 · QK/QP 150 · QS 383 · QZ 61); v8raw the same minus the emit layer.  **v8 is the harness seed from here.**

**The closing legs (`cert_null_v8` / `cert_raw_v8`, 60 epochs, the harness defaults and nothing else: every RS restore
armed, `resume_coupling_restore` and `zwd_seam_reseed` set false by the harness on this lineage, `resume_border_restore`
absent).**  Both 61/61, no cold start, riders 61 `restored` + 61 `audited` + 1 `decided` (the consumer's).  Restore counts:
RS-2 4 blocks / 699 chord rows re-based / 0 un-basable; RS-2b 45 trips held, retired at 277320; RS-3 6 / 176 / 126 / 302
and CB 121 / 32 / 30, **eq34 re-references at +1 = 2**, re-seed discs 0; RS-5 147 / 147 / 1 (raw arm 0 / 0 / 0 with the
"0 under an ARMED read" clause — its seed writes no emit layer); RS-6 61 + 467; **RS-8 366 restored, 34 would have been
decided differently** (§21.20 to the count); **RS-9 61 restored / 61 seeded / 0 carried-but-unseeded**; RS-4 null arm 126
tested / 0 over threshold / 0 marked, raw arm 113 of 126 over (89.7 %) / 1 marked — the mark reads the raw level exactly
where it is meant to.

| seam epoch, leg − seed | +1 | +2 | reference (the card that set it) |
|---|---|---|---|
| PB wire p50 | 0.0004 | 0.0008 | 0.000 (RS-5) |
| clock contrast p50 | 0.152 | 0.190 | 0.160 (RS-8 canon) |
| W6 code bias p50 | 0.052 | 0.084 | 0.046 (RS-8 canon) |
| ZWD p50 | **0.086** | 0.105 | 0.180 (RS-10 nozwd) — halved again by RS-8 |
| Δa₀₀ (TECU) | +1.08 | +1.49 | within 2 |
| chord births leg / seed | **1 / 2** | 5 / 2 | 1 (RS-8 canon; the control read 64) |
| D5 flags (held, 0 re-taps) | 45 | 1 | posture (RS-9 in, reseed out) |

The raw arm's solve channels are bit-for-bit the null arm's (only the emit layer differs).  rx-CB withheld share over the
leg **11.39 %** (417/3660) vs the seed's 11.01 % — on the seed's own number (RS-9's lin leg read 9.78); the withholding set 7
on both, ±1 (OAFA00ARG in, LCK400IND out).  L2X tracker set at the first IF-TD epoch **= the seed's, arc for arc**
({GAMB00PYF 5, OAFA00ARG 6, INEG00MEX 4}).  **Raw PB at +1 over ALL keys** (126 present at the epoch): p50 **0.736 m**, p95
1.79, max 2.22 — the 11 L2X keys read 0.30 (no longer the outlier family; the "ex L2X" grading is retired), the other 115
0.78 against RS-8's 0.60 on the canon leg (+30 %; posture delta — RS-9 in, the reseed out — worth a second look, not a
regression).  **Stateful splice** (`_cert_rp_splice_v8`, 665 epochs; the instrument first reproduced RS-9's 24 on
`_rs9_rp_splice_lin`): at the seam epoch **PB resets 0, code re-references 15, clock 0** — the code channel down from
22–24 (−37 %) with RS-8's 10 channel-set bumps retired; the window's PB count (29) equals the continuous wire's.

**The D5-v5 re-test (§23.2's named pair, `d5v5_v8_{off,on}`, 120 epochs, one differing key).**  V2 every clause PASS
(born flags 74 → 0, born-skipped 1824, carried 16 → 17, births 1873 → 1805, **re-taps 90 → 17**, carry-side lifetime 49.1 →
51.0 ep); wire: clock contrast 0.0020 and ZWD 0.0018 PASS, PB wire |ON − OFF| p50 **0.0137 m vs the 0.01 bar** (FAIL, 1.37×;
p95 0.18, max 6.3); withheld 0 = 0; a₀₀ 0.002 TECU.  **The consumer bar is indeterminate:** de-biased by the window MEDIAN
ON/OFF = −3.9 % (PASS), by the window MEAN +3.0 to +4.1 % — every reading inside ±5.4 % and the SIGN flips with an
analyst choice, while the direct arm-to-arm 3D separation (mean 1.22 m) is an order of magnitude larger than the
dispersion difference.  Not resolved by same-realization pairing either: the knob reshapes the chord population and with
it the realization, and this float consumer cannot rank two realizations.  **KEEP_OFF stands** (§23.2); the mechanism's
win is real (re-taps −80 %) and a promotion needs a consumer-side instrument that is not a single realization's
dispersion — a truth-referenced multi-realization grade, owed.

**Part 3 — the final HEAD (`1542441` for binaries and configs; `574d1a4` docs-only past it).**  Debug and Release rebuilt
first attempt; Debug suite **2073 cases / 281 suites / 2072 passed / 1 documented skip / 0 failed** (`IfTdRefArmAll` 13/13,
`ZwdSeamReseedQ` 11/11); Release exe `AF418A9F…`; the smoke pair from the REGENERATED field golden (now pinning
`raim_carry_invariant`) `_cert3_smoke_base` and `_cert3_smoke_nowrite` BOTH `87a33bf0…` with the re-host live (20 seam
passes, 18 163 cells, σ̂₀ 1.000, zero opportunity) — the posture summary moved SCOPED 39/46 → 40/47 and the loader's old
"post-fit RAIM is INERT" warning is gone; no unknown-knob warnings.  **The REF-FAN sim storm arm** (`runs/golden_sim_campaign_
zwdblk_refarm` vs `_refarc`, the §22.5 protocol; config tracked as `config/test_suite/_golden_sim_campaign_zwdblk_refarm.json`)
is neutral or better on every printed field — ZWD 0.0059 both, consumer p50 0.0807 → 0.0806, T4 events 11 = 11, refloats
8315 → 8313, the whole band-scope census byte-identical — and the additional chains now report opportunities (2 each on
the Galileo (1,7) and (5,7) chains, 4 inherited fires against the first chains' 2, the F1 bar exactly); the knob is pinned
on `golden_sim_tri_w6` and its exception lifted (§22.7).  ⚠ `simnet_storm_grade.py` needs the elapsed-seconds line in
`exitcode.txt` that `ev_launch.ps1` writes and a plain `.cmd` launcher does not.

## 22. THE REFERENCE-ARC CLAUSE for the common-band scope (`network.cycle_slip.common_band_ref_arc`) — the §20.1 fix stub, pre-registered (2026-09-12 ~08:00 local, manager; before any leg is read)

### 22.1 The defect, restated from the code

The scope's IF-TD veto (`NetworkCommonBandScope.cpp:138–144`) refuses a silent MW pair as an
exonerator only when the pooled IF-TD preview FLAGGED that pair.  The preview (`:277–356`) forms
`flagged = |dd − med| > thr` per sample, and a sample on the chain's REFERENCE satellite has
`dd ≡ 0` by construction (`IfPhaseSlipDetector.hpp:195–206`: `is_reference`) — so on a reference
arc the veto is structurally unavailable and the pair lands in `iftd_silent` instead.  The
per-station reference GATE (card R) tests the FIRST chain's reference through a different statistic,
but its decision never reaches the preview, and the additional chains under `if_phase_td_pairs =
all` have no reference test at all (§14.10.4).  §20.1 measured the consequence on real data: an
equal-count L1C+L5X slip on GOPE/G24 (the L5 chains' reference) narrowed to the innocent band under
`first` AND `all`, while on G25 (not the reference) `all` vetoed at 29× the threshold.  Network-wide
the dead zone is exactly one arc per station per additional chain — the highest L5-capable
satellite in 80.6 % of epochs.

### 22.2 The clause (built default-OFF behind `network.cycle_slip.common_band_ref_arc`)

A pair whose IF-TD sample at (station, satellite) is the chain's reference is **UNTESTED for the
verdict**: the preview records it as such (`CommonBandEvidence::iftd_untested`), the rule refuses
the corresponding silent MW pair as an exonerator — the same shape as `immature` and `lli_shadow`
(counted `iftd_ref_arc`, basis token `iftd(<pair>)=ref_arc`, never listed as `silent`) — and the
verdict falls through exactly as today when nothing else exonerates (`no_exoneration ⇒ all-band`).
No detector change, no re-opening of the §14.10.4 flood, no new reference election.  Applied to
EVERY chain's reference (first chain included): the first chain's reference gate is a separate
instrument whose decision the preview does not see, so treating its arc as tested would be an
assumption, not a measurement — wiring the gate's fire into the preview is a possible second
increment, not this card.  Byte-identical when the knob is off (one branch in the rule).  Also
(trace-only, products byte-identical): `network_if_slip.csv` gains an `is_ref` column so a
reference arc can be identified by the detector's own flag instead of by `abs_dev_m == 0`.

**Named cost, accepted in advance.**  On a reference arc of the L5 chains, a single-band L1 (or L2)
slip loses its narrowing: its only silent pair is (2,5) (or (1,5)), now untested ⇒ all-band = the
pre-card response.  An L5 slip there still narrows (its silent pair is (1,2), tested).  On an arc
that is the reference of all three chains (the (1,2) reference tracks L5 in 46.7 % of
station-epochs) nothing narrows.  The census in 22.3 (B3) prices this from the seed's own traces.

### 22.3 Pre-registered legs and gates

Legs on `runs/evseed_field6h` ep0604 (GOPE00CZE, event at +10, 60 epochs; the resume posture is
whatever §21 Phase 1 decides — `state_snapshot_carry: false` if L1 passes — identical across every
leg of this card), scope ON + `if_phase_td_pairs: all` (the §20.1 `sp` extra), knob ON vs OFF:

| leg | site | expected ON | expected OFF |
|---|---|---|---|
| null on/off | — | byte-identical to OFF unless a natural slip lands on a reference arc; every differing event row must carry `ref_arc` | — |
| E3 L1C +29 & L5X +29 | G24 (reference of both L5 chains, k ∈ [−9, +41]) | **all-band**, basis carries `iftd(L1C+L5X)=ref_arc;iftd(L2W+L5X)=ref_arc`, `reason=no_exoneration` | {L2W} — the §20.1 trap reproduced |
| E1 L5X −99 | G24 | {L5X}, products byte-identical to OFF | {L5X} |
| E2 L1C +37 | G24 | all-band (the named cost) | {L1C} |
| E3 L1C +29 & L5X +29 | G25 (not a reference until +43) | byte-identical to OFF (all-band via the (1,5) veto) | all-band |

* **B1 OFF byte-identity:** knob absent vs `false`, and this exe vs main's, on the null leg —
  `products.txt` and `network_events.csv` identical.
* **B2 identity:** the table above, read from each leg's `network_events.csv` verdict row
  (`implicated=…;exonerated=…;basis=…`) and the run-end clause's new `ref-arc refusals N` field
  (invariant #4: an ON leg at G24 reporting 0 refusals is dead wiring).
* **B3 cost census (no run):** over the seed's 6 h `network_events.csv` (scope OFF, `would-narrow
  23`), the number of would-narrow verdicts whose exonerating pair sits on a reference arc of its
  chain (reference identified from `network_if_slip.csv` by `abs_dev_m == 0` on every row of the
  arc, cross-checked against the new `is_ref` column on a 20-epoch smoke) — the fraction of
  narrowings the clause forfeits.  Recorded; the pre-registered expectation is ≤ 15 % (one
  reference arc per station per chain among ~12 tracked).
* **B4 the sim bed (owed for the sim-golden pin, not for the merge):** the increment-1 storm arm
  (`config/test_suite/_golden_sim_campaign_zwdblk_sts.json` + the knob) graded with
  `scripts/simnet_storm_grade.py` against the Study-7 final arm: consumer settled p50 ≤ 0.13 m
  (was 0.1204), T4 attribution ≤ 15 (was 11), split-referenced satellites ≤ 0.5/epoch (was 0.0),
  ZWD settled rms ≤ 0.0075 m (was 0.0069), IF-TD vetoes not fewer.  Pass ⇒ pin in
  `golden_sim_tri_w6` through the generator; fail ⇒ the clause stays a field-only recommendation
  with the sim cost named.
* **Then (sequel, on a field seed that warm-resumes — §21 Phase 2):** the wire/consumer price of
  the E3 narrowing at G24 with the clause OFF vs ON (gates G-C/G-D of §20, dark until now), and the
  scope+pairs+clause FIELD pin through the generator if the §20 bars pass.
### 22.4 RESULTS — B1–B3 PASS on the field seed; an unregistered effect; B4 running (2026-09-12 ~10:10 local; worker on worktree-agent-a02f192d1479c9bc2 @ `c8096ee`, exe `gnss_processor_cbref.exe` sha256 `5BD7D2DC…`, legs `runs/evleg_cbref_*` on the §21.5 interim posture `state_snapshot_carry: false`)

**Built as §22.2:** `network.cycle_slip.common_band_ref_arc` (default OFF, warn+disarm without the scope);
`CommonBandEvidence::iftd_untested`; the preview carries `is_ref` (an `IfTdPreview` element replaces the
`{pair, flagged}` pair) and folds three ways only when the knob is armed; the rule refuses an untested pair
before the veto check (`iftd_ref_arc` + `iftd_ref_arc_pairs`, token `iftd(<pair>)=ref_arc`); counters
`n_cb_iftd_ref_arc[_by_pair]`; run-end field `ref-arc refusals N [pair: n …]`; `network_if_slip.csv` gains
`is_ref`.  One addition the worker made from its own null A/B (`c8096ee`): the basis tail names untested
pairs the refusal branch never reached (MW-flagged / immature / unprocessed), exactly as the flagged set is
named — without it 6 of the 12 differing null rows differed only by a vanished token.  Basis-string only.
Debug suite 1970 / 0 fail; `CommonBand*` 50/50.

**B1 PASS.**  Null on/off: `products.txt` identical (`1FF0BA20…`); `network_events.csv` differs in 12 of
1992 rows, **12/12 carry `ref_arc`**, all `T0_band_scope`, all natural L5-chain reference arcs (e.g.
WTZR00DEU/G24 at 277410, `iftd(L1C+L5Q)=ref_arc;iftd(L2W+L5Q)=ref_arc`).  Smoke, knob absent vs `false`:
`products.txt` `B9DA8CC1…` identical.

**B2 PASS — every pre-registered row.**

| leg (GOPE00CZE, +10) | ON | OFF |
|---|---|---|
| E3 L1C +29 & L5X +29, **G24** | `all reason=no_exoneration;implicated=L1C;L2W;L5X;basis=mw(L1C+L5X)silent(n=10;thr=1.4WL);iftd(L1C+L5X)=ref_arc;mw(L1C+L2W)flag;mw(L2W+L5X)flag;iftd(L1C+L2W)=flagged;iftd(L2W+L5X)=ref_arc;lli=none` | `keep implicated=L2W;exonerated=L1C;L5X` — the §20.1 trap |
| E1 L5X −99, G24 | `keep implicated=L5X` | same; products `DEF230D7…` identical |
| E2 L1C +37, G24 | `all` (the named cost) | `keep implicated=L1C` |
| E3, G25 | `all`, products `44D461B0…` identical | `all` via `iftd(L1C+L5X)=flagged` |

The clause reaches the drain: on E3/G24 the OFF IF-TD refloat row reads `L1C;L2W;pair=L1C+L2W;cb_kept=L1C`
(the scope held back the band that really slipped); ON it reads `L1C;L2W;pair=L1C+L2W`.  Run-end
`ref-arc refusals`: null 6 `[L1C_L5Q 1, L1C_L5X 1, L1W_L5X 2, L2W_L5Q 2]`, E3/G24 8 (`L1C_L5X` 1 → 3, its two
verdict epochs), E2/G24 8 (`L2W_L5X` +2), E1 and E3/G25 6 (= null) — invariant #4 in both directions.

**B3 — the census could not be run as pre-registered, and the substitute is exact.**  The OFF run writes no
`T0_band_scope` rows (`NetworkCommonBandScope.cpp:521` returns before the emit loop when unarmed; the seed's
20 484 event rows hold 0), so its 23 would-narrow verdicts exist only as a counter.  Census on the five leg
pairs instead (both arms scope ON + `all`, the clause the only difference): **4 forfeited narrowings of 106
(3.8 %, bar ≤ 15 %), all four injected — E3 ×2 (the fix) and E2 ×2 (the cost); natural forfeitures 0/20.**
Reference exposure under `all`: `is_ref` on 18.3 % of IF-TD samples; 107 of 507 (station, satellite) arcs are
a reference of ≥ 1 chain (multiplicity 1/2/3 = 32/47/28).  ⚠ The `abs_dev_m == 0` proxy §22.3 named is
UNUSABLE: 28.5 % false negatives on the smoke (`abs_dev = |dd − med|` is zero only where the pool median is),
0 of 71 hits at a flag epoch on the seed — the `is_ref` column is the instrument.

**Unregistered effect, recorded, not banked.**  On every leg the clause also resolves **six `inconsistent`
fallbacks into narrowings** (null: narrowed 1 → 7, inconsistent 15 → 9): a pair fired while every band was
"exonerated" — a contradiction — and refusing the untestable exonerator leaves consistent evidence that names
one band.  Products are unchanged because those six MW flags were dropped at admission, so the drain acted on
none of them.  The principle is right (an exonerator that cannot be cross-checked is not an exonerator); the
wire effect is nil on the null and is to be read on the storm arm, not assumed.

**B4 running:** `config/test_suite/_golden_sim_campaign_zwdblk_refarc.json` = the increment-1 `sts` storm arm +
the clause, `gnss_processor_cbref.exe`, against `runs/golden_sim_campaign_zwdblk_sts` (exe certification by a
20-epoch smoke pair `stspromo` vs `cbref` on the `sts` config, or a same-exe OFF rerun).  Bars as §22.3.
### 22.5 B4 PASS — the sim storm bed is neutral on every bar; the clause is PROMOTED (RECOMMEND_ON with the scope) and pinned in `golden_sim_tri_w6` (2026-09-12 ~11:30 local; manager grade with `scripts/simnet_storm_grade.py`)

**Arms.**  `runs/golden_sim_campaign_zwdblk_refarc` = `config/test_suite/_golden_sim_campaign_zwdblk_refarc.json`
(the increment-1 `sts` arm + `common_band_ref_arc: true`), exe `gnss_processor_cbref.exe` (main `904b89e` + the
clause), exit 0, 720/720, 1507 s on a shared box; against `runs/golden_sim_campaign_zwdblk_sts` (exe
`stspromo`, 1303 s).  **Exe certification without a smoke:** `products.txt` is byte-identical between the two arms
for the first **283 epochs** — the first differing byte is the clock record after the E record for tow 310890
(2.35 h in), where the clause first actuates — so the OFF path across the two builds is certified on the storm
bed itself.  The 286/286 symmetric event-row differences are the same events with different numbers after that
epoch (PBGAUGE 224, CLK_ANCHOR 19, READMIT 17, STRANSFORM 2, PIVOT 2) plus 22 `T0_band_scope` rows carrying
`ref_arc`; the first three differing rows (tow 302430) are `all → all` with the basis tail only.

| channel (§22.3 bar) | sts | refarc | bar |
|---|---|---|---|
| ZWD settled truth rms | 0.0059 | **0.0059** | ≤ 0.0075 |
| ZWD hourly | 0.17, 0.007, 0.007, 0.006, 0.006, 0.006 | identical | — |
| consumer settled p50 / rms | 0.0807 / 0.09006 | **0.0807 / 0.09004** | p50 ≤ 0.13 |
| consumer hourly rms | 0.645, 0.109, 0.116, 0.101, 0.103, 0.059 | identical | — |
| T4 attribution events / ambient | 11 / 0.193 | **11** / 0.190 | ≤ 15 |
| split-referenced satellites mean / max | 0.0 / 0 | **0.0 / 0** | ≤ 0.5 |
| WL / EWL / L1C ref-free settled | 99.2 / 100.0 / 90.4 | identical | — |
| IF-TD vetoes (by pair) | 116 (L1C_L2W 79, L1C_L5Q 37) | **116** (identical) | not fewer |
| IF-TD pairs eval / flag / refloat | 2055853 / 4747 / 4722 | identical | — |
| PB-GAUGE handovers continuous / cold | 362 / 100 | identical | — |
| S-transform edges / re-expressed / retained | 2 / 52 / 376 | identical | — |
| scope: verdicts / narrowed / no-exon / inconsistent | 4208 / 3147 / 807 / 202 | 4208 / **3150** / **820** / **186** | — |
| **ref-arc refusals** | — | **29 [L1C_L5Q 12, L1C_L7Q 5, L2W_L5Q 12]** | > 0 (invariant #4) |

Every pre-registered B4 bar passes with the two arms equal to the digit; the only movement is inside the scope's
bookkeeping — 16 `inconsistent` fallbacks become 13 `no_exoneration` (both all-band, no action change) and 3
narrowings — and the solve diverges only from the first of those narrowings on.  The sim bed never carries the
reference-arc trap (its injected and campaign slips do not land equal-count on a reference), so the clause is
neutral there and fixing on the field (§22.4).

**Decision.**  RECOMMEND_ON together with the scope: pinned in `golden_sim_tri_w6` through the generator
(`scripts/_rs2_refarc_ledger_oneoff.py refarc` → ledger row + md + posture row + manifest pin; `--verify` PASS;
`config_posture_check.py --verify-loader` 0 gaps).  The field golden needs no exception — the scope itself is a
field exception there, so the clause is out of regime rather than suppressed; the field pin of scope + pairs +
clause rides on the RS-2 seed (§21.5) once the §20 wire/consumer bars can be read.  Grader hygiene noted, not
fixed here: `simnet_storm_grade.py`'s `bscope_clause` reads `None` on both
arms.  **Resolved 2026-09-13 (main `dcf04dd`, worker commit `ee33550`): the regex was not stale — the `§Z`
response-scope clause (`raim_erase_band_scope` / `refloat_band_scope`) is out of regime on the campaign lineage, which
emits the detector-side `Network (common-band scope): verdicts …` census instead; the grader now tries both, records
`bscope_clause_kind`, and the captured census reproduces B4's table to the digit (4208 / 3150 vs 3147 / 820 vs 807 /
186 vs 202 / 116 / ref-arc refusals 29).**

### 22.6 THE REFERENCE-SLIP FAN-OUT under `if_phase_td_pairs = all` — found by FP-1 on the warm-resuming field seed; the REF-FAN card pre-registered (2026-09-13 ~09:45 local, manager; FP-1's legs still running, no fix leg read)

**The finding (FP-1 worker, interim report at 7 of 12 legs; `runs/evleg_fp1_*` from `evseed_field6h_v5` ep0604,
exe `gnss_processor_rs7.exe` 23C628B3…).**  With the field seed warm-resuming cleanly (61/61 emitted on every
leg, 0 withheld — defect 1 of §20.1 is cured on v5) the §20 identity rows all reproduce (E1 PIN → `{L5X}`; E2 PIN
→ `all` via `iftd(L2W+L5X)=ref_arc`, the clause's named cost; the PIN null's `ref-arc refusals 6 [L1C_L5Q 1,
L1C_L5X 1, L1W_L5X 2, L2W_L5Q 2]` = §22.4 exactly).  But the pre-registered G-A cannot see what the additional
chains DO with a slip on their own reference: at GOPE00CZE, G24 is the reference of both L5 chains for k ∈
[−9, +41] (§20.1), so E1's −99 cyc L5X slip reads `abs_dev = 0.000000` on G24's own rows and **−31.8 m / −283.9 m
on G11, G25 and G32** — six false IF-TD refloats on innocent arcs, 0 in the OFF arm; one of them (G32/L1C)
re-realizes a satellite phase-bias datum network-wide by **13.7 m** (declared: disc 1 → 2, σ 0.039 → 0.449).  No
backstop: card R's counters are identical in all four legs, T4 is structurally 0 on `carry_invariant`, and the
collateral satellites have no scope verdict to narrow.  The mirror image of §22.1: the clause made an UNTESTED
reference refuse to exonerate; nothing makes a SLIPPED reference stop implicating its chain.

**Why it exists (the §14.10.3 flood guard).**  The reference arm (tier T1r, `IfPhaseSlipDetector.cpp` ~689–800:
the median of the non-reference between-epoch DDs is the reference's own step; on a fire the reference's IF arc
is reset, `ref_slip` traced, and — for a coherent step — the chain's `prev_grad` re-based so the pooled outlier
test sees only per-satellite residuals) runs on the FIRST chain only (`~977: run_ref_arm = first_pair_chain`),
because per-chain re-runs through the LEGACY candidate condition |median| > 0.5·λ_IF flooded on the wide
combinations ((L2W,L5Q): noise amplification 16.6, λ_IF 0.125 m — 5 351 candidates / 64 fires on a quiet null,
§14.10.3).  An additional chain therefore ADOPTS the first chain's reference when that satellite is in its set
(`~934–957`) and otherwise ELECTS its own (`selectReference`) — and in neither case is a reference slip ever
attributed: an adopted reference's fire in the first chain does not reach the additional chain (whose fan-out then
goes to the pooled test), and an own-elected reference is never tested at all.  On DOY-129 the L5 chains
own-elect at 53 % of station-epochs (the (1,2) reference is a no-L5 IIR-M in 98 % of those), G24 at GOPE among
them.  Card R's EMPIRICAL gate (`if_phase_td_ref_gate`, armed on both goldens: per-chain EWMA dispersion scale,
k = 6, coherence, warm-up abstain, thin-widen) postdates that flood measurement and scales with each chain's own
noise — the legacy threshold was the flood, not the arm.

**Build (default-OFF, byte-identical when off; the `first` posture untouched by construction).**  Knob
`network.cycle_slip.if_phase_td_ref_arm_all` (requires `if_phase_td_pairs = all` AND `if_phase_td_ref_gate` —
warn + disarm otherwise; the legacy candidate condition must never gate a wide chain again): on an additional
chain, (i) OWN-ELECTED reference ⇒ run the reference arm on that chain through the empirical gate with the chain's
own dispersion state (`cs.ref_scale_*` are per chain already); (ii) ADOPTED reference ⇒ inherit the first chain's
verdict for that satellite this epoch: a first-chain `ref_slip` fire on the adopted satellite is a `ref_slip` on
this chain too (reset THIS pair's reference arc `<sat>_<this chain's phase_if_key>`, re-base this chain's
`prev_grad` by this chain's own median when the verdict is FireRebase, skip the pooled attribution of the fan-out);
no fire ⇒ exactly today.  Counters per chain: candidates / fires (own) / inherited fires / fan-out suppressed;
run-end line; `network_if_slip.csv` rows for the additional chains' reference arm carry the chain key and
`ref_slip`.  Nothing changes on the first chain, on `pairs = first`, or when the knob is off.

**Gates, frozen (legs 60 epochs from `evseed_field6h_v5` ep0604, PIN posture = scope + pairs `all` + the clause
[the FP-1 arm], fix off vs on, one exe; events as §20 at GOPE00CZE, +10):**
* **F1 the flood (null off/on):** additional-chain reference-arm fires network-wide over 60 epochs ≤ 2× the first
  chain's own count over the same window (report both, and the candidates); products byte-identical when nothing
  fires, otherwise differing only at attributed epochs (list them).
* **F2 the fan-out (E1, E5, E3/G24 off/on):** collateral IF-TD refloats on the non-reference satellites of the L5
  chains at k ∈ [+9, +12] → 0 (from 6 on E1); the reference's own arc flagged `ref_slip` on each L5 chain at +10
  (E1 and E5: the L5X arc; E3: both bands, and the band-scope verdict for G24 stays all-band or narrows to the
  slipped bands, never to L2W); phase-bias datum re-realizations caused by collateral refloats → 0 (the 13.7 m
  G32/L1C one); consumer event − null max |Δ3D| over +0..+50 and mean over +30..+50 on ≤ OFF; the station frame
  (CLK / ZWD / the injected LOS) at +45 on ≤ off.
* **F3 the control (E3/G25 off/on, G25 not a reference):** products byte-identical, or differing only where an
  inherited first-chain fire acted (explain each).
* **F4 byte-identity:** cold smoke on the field golden (pairs `first`) knob absent vs armed `87a33bf0…`; the
  fix-off legs reproduce FP-1's PIN legs' `products.txt` byte-for-byte (same posture, new binary); suite green.
Passing F1–F4 re-opens the FP-1 pin (G-A..G-G re-run on the fixed posture, §20's bars); failing F1 names the
flood's true owner (the empirical gate on wide chains) and the pin stays an exception with this mechanism.

### 22.7 RESULTS — REF-FAN: the reference arm on the additional chains holds the flood and removes the fan-out; RECOMMEND_SCOPED, not yet pinned; FP-2 (the §20 matrix on the fixed posture) pre-registered (2026-09-13 ~19:10 local; worker on worktree-agent-a789fceb19843b4fb `9bd2d51`, merged `e6b04a4`; manager grade)

**Build.**  `network.cycle_slip.if_phase_td_ref_arm_all` (default OFF; loader disarms it without `if_phase_td_pairs = all`
AND `if_phase_td_ref_gate`, and the detector refuses the legacy candidate condition on a `|`-keyed chain — two locks
against re-opening the §14.10.3 flood): an additional chain with an OWN-elected reference runs the reference arm
through card R's empirical gate with that chain's own dispersion state; an ADOPTED reference inherits the first
chain's verdict for the epoch (this pair's reference arc reset, the pooled fan-out suppressed); per-chain counters
(opportunities / candidates / own fires / inherited fires / fan-out rows suppressed) + a run-end line;
`tests/test_iftd_ref_arm_all.cpp` (13).  Cold smoke on the field golden (pairs `first`, knob inert) `87a33bf0…` both
arms; the fix-off legs reproduce FP-1's PIN legs byte-for-byte (`null_off` `bea5df33…`, `e3g25_off` `1c217798…`); Debug
suite 2036 / 277 / 0 failed.  ⚠ Process: the ten legs were first generated across three harness versions (RS-9, RS-8
and the RS-10 conditional landed on `event_leg.py` mid-batch — feedback_harness_defaults_shared_state) and an orphaned
driver tree re-ran four legs in place (killed name-scoped); every graded leg was regenerated onto one harness
(`e6071f43…`), diff-verified single-knob per pair.  A free determinism control: `e3g25_on` run twice (once with a
second producer live) is byte-identical.

**F1 PASS — the flood is held by the empirical gate.**  Null legs, 60 epochs × 60 stations: additional-chain fires
**1** (own; `G27/SCTB00ATA` on `L2W;L5X` at k = −7, a natural reference slip the OFF arm never tested) against the
first chain's 1 — bar ≤ 2×.  The counters price §14.10.3 exactly: `GPS|L2W_L5Q` 9 candidates → 0 fires, `GPS|L2W_L5X`
85 → 1 — 93 of 94 legacy candidates on the wide combinations suppressed.  Products differ from that fire on (59
epochs, the first differing `H` record stamped at k = −7; nothing differs at −10 / −9).

**F2 PASS on every primary bar — the fan-out is gone.**  Collateral IF-TD refloats at GOPE00CZE, k = +0:

| event | OFF collateral | ON collateral | ON: the reference's own arcs reset | the innocent 13.3 m `G32/L1C` PB re-realization |
|---|---|---|---|---|
| E1 L5X −99 | 6 (31.8 m ×3, 283.9 m ×3) | **0** | 2 (`G24 L1C;L5X`, `G24 L2W;L5X`) | OFF present, **ON absent** |
| E5 L5X −31 + LLI | 6 (9.96 ×3, 88.9 ×3) | **0** | 2 | OFF present, ON absent |
| E3/G24 +29 / +29 | 6 + 1 legitimate (`G24 L1C+L2W` 14.05 m) | **0** + the same legitimate one, value identical | 2 | OFF present, ON absent |

The band-scope verdict for G24 is unchanged (E1 both arms `implicated=L5X; exonerated=L1C;L2W; … ref_arc`; E3/G24
all-band in both — the clause's named cost untouched).  Event − null within each arm: blast radius collapses 7.5× on
every event (worst other satellite 13.34 → 1.79 m); the station frame at +45 improves on E1 (clk −8.17 → −5.07, ZWD
3.48 → 1.16) and E5 (−4.48 → −1.59, 2.50 → 0.36); consumer mean |Δ3D| over +30..+50 **1.94 → 0.72 (E1), 2.20 → 1.01
(E5), 1.34 → 0.99 (E3/G24)**.  Two honest misses, reported not smoothed: consumer MAX |Δ3D| over +0..+50 E1 2.44 →
2.52 (+3 %) and **E5 2.93 → 4.47 (+53 %)**; the E3/G24 station clock at +45 −1.19 → +1.50 m.

**F3 PASS in substance.**  The E3/G25 detections are bit-identical between arms (`L1C+L2W` 14.0445, `L1C+L5X` 3.1508,
`L2W+L5X` 83.1726 m) and the station frame identical to four decimals; the products differ from k = −7 on — the same
natural `G27` fire as in every pair including the null, not the G25 event — so the pre-registered "byte-identical"
wording cannot hold for a mechanism that acts on natural reference slips network-wide; the bar is read event − null
within each arm.  **F4 PASS** (above).

**Invariant #4, both ways.**  `inherited` fires = **0 in every leg**: the adopted-reference path is built and
unit-tested but never fired on this lineage — the L5 chains own-elect (the (1,2) reference is a no-L5 IIR-M at 98 % of
those station-epochs), so the whole field result comes from the own-elected arm; the inheritance half is validated
on a lineage where an L5-capable satellite is the first chain's reference, or not at all.

**Decision.**  RECOMMEND_SCOPED (regime: `if_phase_td_pairs = all` with `if_phase_td_ref_gate`); pinned nowhere yet —
the field golden runs pairs `first` (out of regime), the sim golden pins pairs `all` + the clause — its storm arm ran in the certification (§21.22 part 3): neutral or
better on every field, the additional chains now with opportunities, so the knob is PINNED there and the exception lifted.  **FP-2 pre-registered:**
the §20 matrix on the fixed field posture — scope + pairs `all` + the clause + `ref_arm_all` — from the v8 harness
seed (every RS restore in both arms; the harness held still for the batch): {null, E1, E2, E3/G24, E3/G25, E5} ×
{OFF, PIN}, §20's G-A..G-E bars verbatim with the G25 +40 window rule, the fan-out census of §20.2 (collateral must
read 0 on every G24 leg), the E5 consumer-max and the E3/G24 clock-at-+45 misses above re-read on the second
realization; G-A..G-E passing ⇒ the G-G twin (6 h, the four knobs) and, on its bars, the field pin of all four
together.  A second event bed (another station / satellite where the injected satellite is a chain reference) is the
named guard against pinning on one site.

## 23. D5 v5 CARRIED-ONLY on the field (`network.carry_invariant_innov_carried_only`) — the born-chord flags are the churn engine and removing them costs nothing on the wire (2026-09-13; pre-registered in the worker brief before any leg was read, results ~12:30 local; worker on worktree-agent-abad026e4a52148da, exe `gnss_processor_rs7.exe` 23C628B3…; manager grade)

**Why.**  The Card-4 D5 innovation gate standardizes a chord's innovation against Cov(v), the H0 covariance about
the CARRIED prior; a chord BORN this epoch has the diffuse fresh-chord prior (√var = 1000 m exactly), so there is no
hypothesis to test, its "reset" is a no-op and the flag's only lasting effect is the funnel entry that re-borns it
next epoch — the loader's 2026-08-02 measurement (95.3 % of all D5 flags were born chords).  v5 applies v2's
accumulator exclusion to the DECISION.  Built default-OFF, never armed on the field golden, no ledger row (§20.1's
hygiene item).  NOT a warm-resume fingerprint token (the carry family contributes `carry:inv` and `carry:pdsafe`
only; both ON legs warm-resumed with the carry restored; the ON twin's ep0604 fingerprint is v5's
`545b8bdd2b638231`), so the knob is A/B-able on legs from one snapshot.

**Pre-registered gates (the brief).**  Smoke pair; legs `d5v5_{null,e3}_{off,on}` (60 epochs from `v5` ep0604, E3 =
the equal-count slip at GOPE/G24): V1 identity on E3 (the injected chords flagged / re-tapped identically, the T0/T1
rows identical, bands freed identical); V2 the mechanism over +6..+60 (born-chord flags ON = 0 with the born-skipped
counter > 0, carried flags ±10 %, births and re-taps ON ≤ OFF, chord lifetime ON ≥ OFF); V3 no harm (clock contrast
±0.02 m, PB wire ±0.01, ZWD ±0.02, consumer de-biased 3D mean +30..+60 ±0.03, withheld equal, a₀₀ ±0.5 TECU); V4 the
6 h twin `evseed_field6h_v5d5` vs `v5` (flags down by the born share, births, carried DDs, `attrib_arc` not up > 10 %,
withheld equal or fewer, consumer de-biased settled h3–6 p50 / rms not worse than 5 %, `pbstep` not worse).

**Smoke.**  OFF `87a33bf0…` (the certified hash); ON `6439e2bb…` — every payload kind moves (B 1690/2595 rows, worst
0.058 m; C 415/640, 0.011 m; P 1755/2700, 0.57 m in the 20-epoch ignition; U 1.4e-5 m), E and S identical, no record
added or dropped; D5 flags 22 → 10 = 12 born + 10 carried → 0 born + 10 carried exactly.

**Legs — V1 / V2 / V3 PASS on every bar.**  All four 61/61 emitted, 0 withheld, warm-resumed; `evleg_d5v5_null_off`
byte-identical (`c45e52a0…`) to another worker's `evleg_fp1_null_off`.  V1: the five injected-event rows verbatim
identical between arms (INJECT ×2; `T0_mw_detect refloat L2W;L5X`; `T1_if_slip refloat L1C;L2W`; MDB); event − null =
+2 births / +2 slip refloats on both arms — and D5 never flags the injected chord on either arm, the raw layer catches
it in-epoch.  V2 (null, +6..+60): born flags **28 → 0**, carried **10 → 10**, births 294 → 268, re-taps 38 → 10,
born-skipped 353 (= the ON arm's births — the counter counts every born row before the threshold test, ~7–8 % of
which would have flagged), carried DDs at run end 759 = 759, carry-side mean chord lifetime (Little's law) 122 → 131
epochs.  The two populations separate perfectly: the 28 born flags at √var = 1000.000 m with median |v| 69.4 km; the
10 carried at √var 0.30–1.06 m, median |v| 8.9 m.  ⚠ `network_dd_fractional.csv`'s chord set is visibility-determined
(set-identical across arms), so a lifetime read from it passes by construction — the carry-side substitute is the
instrument.  V3 (null): clock contrast +0.0035 m, PB wire −0.0007, ZWD −0.0009, consumer de-biased +0.0021, a₀₀
+0.0012 TECU, withheld 0 = 0; the E3 pair the same to three digits (the event does not amplify the knob's wire
effect).  Direct arm-to-arm consumer 3D separation mean 0.39 m / max 2.12 m on a 7–9 m float-only consumer —
reported, not gated.

**V4 the twin (720/720 both; fingerprint unchanged).**

| | v5 (OFF) | twin (ON) | |
|---|---|---|---|
| D5 flags / re-taps | 730 / 730 | **143 / 143** | −80.4 %; 730 − 589 = 141 predicted, 143 observed |
| born / carried flags | 589 / 141 | **0 / 143** | +1.4 % carried |
| chord births; slip refloats | 9932; 1902 | 9362; **1332** | −5.7 %; −30.0 % |
| carried DD at run end; marginalizations | 758; 5710 | 758; 5710 | identical |
| withheld epochs; a₀₀ median | 0; 31.281 | 0; 31.250 | equal; −0.03 TECU |
| `pbstep` frac > thr; p90 max | 0.1181; 0.2570 m | 0.1125; **0.0571 m** | better (4.5×) |
| `cb_disc` / `clk_disc` / shocks / `pb_reset` | 590 / 6 / 600 / 29 | 590 / 6 / 600 / 30 | R, E, S records identical |
| consumer de-biased settled h3–6 p50 / rms | 5.495 / 6.163 | 5.785 / 6.980 | **+5.3 % / +13.3 % — FAIL (bar 5 %)** |

The twin's wire: clock contrast |ON − OFF| p50 0.014 / p95 0.123 m; phase bias p50 0.31 m with a tail that is ONE
satellite — every one of the 731 rows > 20 m is G11 (L1C 620, L2W 111; 389.2 L1 cycles, not an integer): an
ambiguity LEVEL realized differently between two independent cold starts, invisible to the datum-invariant
between-satellite reading (`pbstep`, better on ON) and absent on the warm legs, which share a realization (PB p50
0.0007 m).  The consumer's hourly series moves in both directions (ON better in h1 −29 % and h3 −36 %, worse in
h4–h6) on a float-only consumer at ~6.5 m raw error — single-realization wander, exactly the confound standing
invariant #5 names, and the reason the V4 consumer bar is not read as harm.

**Decision.**  RECOMMEND_SCOPED (regime: `carry_invariant` with the D5 gate); the mechanism is proven on the field
and every wire-side instrument is flat or better.  The field golden pin is DEFERRED to a same-realization consumer
grade, pre-registered here: two 240-epoch null legs from `v5` ep0604 (`d5v5_long_{off,on}`; both arms share the
seed's realization up to the seam, so the consumer difference isolates the knob) — consumer de-biased 3D mean over
+120..+240 ON within 5 % of OFF (each arm de-biased by its own window median), the direct arm-to-arm separation
reported, `pbstep` on the leg products not worse, withheld equal, clock contrast / PB wire / ZWD p50 over +120..+240
within the V3 bars.  PASS ⇒ pin on `golden_field_gps_dual` (its certified smoke hash moves — every payload kind, no
structural change — and is re-recorded at that certification); FAIL ⇒ the exception stands with the consumer number.
Graders merged: `scripts/d5v5_grade.py`, `d5v5_prodiff.py`, `d5v5_seam_ab.py`.

### 23.2 RESULTS — the same-realization pair refuses the pin: KEEP_OFF on the field, re-test named (2026-09-13 ~16:20 local; worker on worktree-agent-abad026e4a52148da `76861c1`, merged; manager grade)

**Legs.**  `d5v5_long_off` / `d5v5_long_on`: 240 epochs from `v5` ep0604 (exe 23C628B3…, config diff = the knob only;
harness defaults of ~12:45 — before RS-8 / RS-9 were harness-armed, so both arms carry those seam defects equally).
**The pre-registered window is not measurable:** a 30-epoch contiguous producer outage sits at ep122–151 on BOTH arms
(`solve_failed` at 122, `gate_withheld` 123–150, `sh_gate_withheld` 151; 6 scattered SH-gate withholds to 215 — the
DOY-129 sick-field era, 06:03–06:18 UT; the SH gate still has no release policy) with dispositions epoch-for-epoch
identical between arms, and the consumer collapses on both (3D 100–330 m after the outage, never recovering) — so
every +120..+240 bar fails on both arms' wreckage and says nothing about the knob (consumer de-biased +14 %, wire
p50 0.18 m, `pbstep` +6 % / +10 %; withheld 37 = 37 PASS).

**Pre-outage (+6..+120, the producer emitting every epoch) — the read that counts:** consumer de-biased 3D mean
**3.069 → 4.267 m (+39 %)**, raw 3D at +120 3.3 vs 14.6 m; PB wire |ON − OFF| p50 **0.033 m** (bar 0.01; spread across
many satellites — G10 107 rows, G04 91, G11 86 above 0.2 m — a broad slow divergence, not the twin's single-satellite
level); clock contrast 0.0055 m and ZWD 0.0022 m PASS.  Three horizons of the same statistic: +6..+60 clock 0.0018 /
PB 0.0032 / ZWD 0.0007 / consumer separation 0.39 m; +6..+120 0.0055 / 0.033 / 0.0022 / 2.61 m; post-outage 0.18 /
0.18 / 0.079 / 169 m — every channel grows with time since the seam (§21.19's realization divergence, here driven by
the knob's own change of the chord population), and the outage multiplies it ~6×.  V2 holds over the whole leg: born
flags 86 → 0, carried 97 → 100, D5 flags 183 → 100, re-taps 145 → 62, births −2.7 %, carried DDs 739 = 739,
marginalizations 1976 = 1976, `clk_disc` 0 = 0, shocks 84 = 84.

**Decision.**  Two truth-referenced consumer reads in the wrong direction (the twin's settled h3–6 +5.3 % p50 /
+13 % rms; this pair's +39 % over +6..+120) against one neutral 60-epoch read: **KEEP_OFF on the field** (ledger
verdict changed; the deferred-pin exception and the posture row removed).  The mechanism is real and cheap — born
chords never carried information to test — but a knob that reshapes the chord population reshapes the realization,
and on this float-only consumer the two realizations the field has been dealt read worse.  Re-test named, not
run: a 120-epoch pair from the v8 harness seed (every RS restore incl. RS-8 / RS-9 in both arms; the pair above
pre-dates them), graded on the consumer de-biased 3D mean over +6..+120 within 5 % and the PB wire within 0.01 m — a
second realization on a seam that no longer re-births 64 chords.  (Instrument note for every future 2 h field leg:
the DOY-129 sick-field outage at 06:03–06:18 UT lands at ep122–151 of any leg from ep0604 — grade before it or
around it, never through it.)

### 23.3 The v8 re-test (2026-09-13 ~21:20; the pair §23.2 named, run in the certification, §21.22)

`d5v5_v8_{off,on}`, 120 epochs from `v8` ep0604, one differing key: V2 every clause PASS (born flags 74 → 0, born-skipped 1824,
carried 16 → 17, births 1873 → 1805, re-taps 90 → 17, carry-side lifetime 49 → 51 ep); clock contrast 0.0020 and ZWD 0.0018
PASS; PB wire |ON − OFF| p50 0.0137 m against the 0.01 bar (FAIL, 1.37×); withheld 0 = 0.  The consumer bar is INDETERMINATE:
de-biased by the window median ON/OFF −3.9 %, by the window mean +3.0 to +4.1 % — every reading inside ±5.4 % with the sign
set by an analyst choice, while the direct arm-to-arm separation (1.22 m mean) dwarfs the dispersion difference.  A knob that
reshapes the chord population reshapes the realization, and a single float consumer cannot rank two realizations even from
one seed.  **KEEP_OFF stands.**  A promotion needs a consumer instrument that is not one realization's dispersion — a
truth-referenced grade over several seam epochs / stations, owed.

## 24. THE CLOSED-LOOP CONSUMER AS AN INSTRUMENT (card CI) — the field consumer wanders by metres, resets on every declared step it observes, and starts cold at every seam; the consumer-side bars of §20.3, §21.21 and §23.2 are SUSPENDED until it converges (pre-registered 2026-09-14 ~00:40 local, manager; the attribution legs launched before this was written, none read)

### 24.1 What the consumer bars have actually been reading

§20.3 refused the field pin of the four slip knobs on a consumer bar: the settled (h3–6) de-biased 3D error of the held-out
WTZZ00DEU float consumer, +18–33 % on the twin.  The same consumer decided §23.2/§23.3 (D5-v5 "indeterminate — the sign
flips with the de-bias centre") and the consumer end of §21.21 (RS-10b's cold in-run consumer).  Read on its own, the
continuous consumer of the harness seed `evseed_field6h_v8` is this (per-hour p50 over 120 epochs, ENU against the
surveyed WTZZ coordinates):

| hour | 3D | up | east | north | n_sats |
|---|---|---|---|---|---|
| 0 | 3.42 | −2.55 | +1.79 | +1.11 | 47 |
| 1 | 6.13 | −0.58 | −0.18 | +6.12 | 48 |
| 2 | 8.54 | +2.83 | −0.22 | +7.92 | 48 |
| 3 | 4.76 | −0.73 | −1.20 | +4.62 | 51 |
| 4 | 5.04 | −3.99 | −2.21 | −0.06 | 50 |
| 5 | 7.16 | −4.75 | +0.42 | −4.99 | 49 |

A float PPP-RTK user with precise orbits, a 61-station clock and phase-bias wire and a co-estimated ionosphere should sit
at decimetres after the first hour; this one walks the NORTH axis from +1 to +8 to −5 m and the vertical from −4 to +3 to
−5 m over six hours, and its first five epochs go 4.2 → 4.7 → 6.2 → 8.0 → 12.4 m — AWAY from its code start.  A consumer
that cold-starts at the seam (`cert_null_v8`, §21.22) reads 3.96 m at k = 1 and 16.1 m at k = 11.  Its hour-to-hour p50
moves by 2–4 m; a 5 % bar on the de-biased dispersion of that series (§20.3's G-G: 5.49 → 6.50 m) is inside the
instrument's own swing, and so is every consumer reading in §21.21 and §23.2.  The `n_sats` column says why the number
is not even a GPS-only reading: the consumer tracks four constellations, the wire covers GPS, and the other three ride the
precise CLK product in a different clock frame through the consumer's inter-system offsets.

**Ruling (retroactive, stated once).**  The WIRE-side bars of those sections stand (§20.3's `pbstep` +28 % epochs / p90
step 3×; §23.3's PB wire 1.37× its bar; §21.21's producer end); their CONSUMER-side bars are SUSPENDED — neither PASS nor
FAIL — until the instrument in §24.4 converges.  No consumer bar decides a pin while the consumer's own hour-to-hour p50
spread exceeds the bar.  (The `feedback_event_harness_over_daylong_runs` rule already said it: a gate that needs a
settled 6 h window on the single closed-loop station is a sign the instrument is wrong.)

### 24.2 The reset channel, measured — the four slip knobs did not reach the consumer through it

`RealTimeEngineSsrBreaks.cpp:94` `resetOnPhaseBiasDisc`: for every phase signal the consumer models on a satellite it
observes, a change of the wire's phase-bias discontinuity counter (`StateManager::noteSsrDiscontinuity`) calls
`resetAmbiguity` — the covariance row and column are zeroed (every cross-covariance to the clock, ZWD, iono and position
thrown away), the diagonal set to `ambiguity_std²`, and the ambiguity marked pending re-seed from ONE epoch of code.
Twins, resets per hour from `logs/rt_engine.log`: v8 **[8, 1, 0, 1, 17, 2] = 29**, v8fp2 **[7, 1, 0, 1, 17, 3] = 29** (code
590 / 586, clock 6 / 6).  The +107 refloats of §20.3 produced ZERO additional consumer resets: the extra declared steps
were on satellites WTZZ does not observe (the JPLM/G10 catch is over California) or below the declaration threshold, and
the wire's σ exposure barely moved (P rows with σ > 0.1 m: 2.32 % → 2.42 %).  On the 60-epoch E3/G24 legs both arms show
the same two resets (G24 L5X at the event epoch — WTZZ observes G24 — and G06 L2W at +31).  So the twin's consumer
difference is not attributable to the four knobs through any channel the consumer is known to react to; it is the wander
of an unconverged filter under a different realization of the same wire.

### 24.3 Pre-registered attribution legs (launched ~00:20, before this section; none read)

From v8 ep0604, 60 epochs, no event, consumer-only knobs under `network.closed_loop` (the producer is byte-identical to
`cert_null_v8` — assert `products.txt` sha `…` equal); the consumer is COLD at the seam in every arm, which is the
regime these legs grade — the DIRECTION of a cold float PPP over 30 minutes:

| leg | consumer knob | question |
|---|---|---|
| L0 `cert_null_v8` | as the golden (FULL: network clock + PB + SH-VTEC iono) | the reference: 3.96 m → 16.1 m by k = 11 |
| L1 `ci_null_noiono` | `consume_network_iono: false` | is the supplied slant ionosphere the vertical driver? |
| L2 `ci_null_noclock` | `consume_network_clock: false` (bias-only: precise CLK + network PB) | is the network clock frame the driver (the Aug FULL-mode divergence, `project_closedloop_consumer_vertical_bug`)? |
| L3 `ci_null_neither` | both false | the two together |
| L4 `ci_null_noproducts` | `consume_network_products: false` (plain PPP on SP3/CLK/.BIA) | does the consumer's OWN modeling converge at all on this station? |

Read per leg: 3D / up / north / east at k = 1, 11, 30, 60, the p50 over k 30–60 and the sign of the k 30–60 slope.
Frozen reading: a healthy float PPP with precise products crosses 1 m 3D within 20–30 min and keeps falling.  If L4
converges and L0 diverges, the network products break the consumer and L1/L2/L3 partition the damage between the iono
supply and the clock frame; if L4 also diverges, the consumer's own modeling on WTZZ (four constellations through the
inter-system offsets, the 25° `ionosphere_weighting.elevation_cutoff_deg`, the 15° mask, antenna handling) is the defect
and the network is not implicated — a consumer-side card before anything else.  Whichever arm converges becomes the
consumer posture of the field golden's `closed_loop` block for grading (a consumer knob is not a producer knob: the goldens
pin it in the manifest, no ledger row, no fingerprint).

### 24.4 Card CC — the continuity half, built regardless of §24.3's answer (worker brief `brief_cc.md`)

**(A) Carry the float ambiguity across a DECLARED phase-bias step — `network.closed_loop.pb_disc_carry`** (derived into the
consumer engine as `processing.ssr_phase_bias_disc_carry`; default false ⇒ byte-identical).  A declared step is a
re-realization common to every consumer of that satellite: the phase row is `predicted = … + ssr_bias_term + amb_term`
with both signs +1 (`MeasurementModel.cpp:1608 / 1642 / 1655`), so when the applied bias moves by Δ = PB_new − PB_old the
float ambiguity in the new datum is EXACTLY `amb − Δ` — a translation of one state, covariance untouched, no re-seed.
Mechanism at `resetOnPhaseBiasDisc`: the engine records the bias value it APPLIED at the last update per key (the same
gated/max-age read the model uses); on a counter change with a recorded value it shifts (`StateManager::shiftAmbiguity`,
new, value-only) and logs `carried {} {} by {:+.3f} m`; first sighting, no recorded value, NL/WL basis, or a BSSD consumer ⇒
the existing reset, each counted; the A5 freshness flip (applied ↔ not applied, Δ = applied_now − applied_prev with 0 for
"not applied") is carried by the same bookkeeping and counted separately.  Counters `pb_disc_carried / pb_disc_reset_*`
at run end; the RT snapshot does not carry the last-applied map (v1 format unchanged), so after a restore the first bump
per key falls back to reset — the conservative direction, stated.  The consumer's own slip QC resets are untouched.

**(B) The consumer's WARM START in the harness.**  The engine already has the seam (`RealTimeEngineStateSnap.cpp`:
`realtime.state_snapshot_path`, RtStateSnapshot v1 = x, P, dynamic layout, disc-counter memory, the A5 applied maps;
age horizon 300 s; a fingerprint over the state-meaning config facts) — the closed-loop harness never armed it, which is
why every leg's consumer is cold.  Build: `network.closed_loop.state_snapshot: true` makes the harness write the consumer's
snapshot beside each network history copy with the SAME `.epNNNN` suffix and the same tow (`state_snapshot.txt.consumer.
epNNNN`); `scripts/event_leg.py` copies it into the leg (pristine + disposable, the seed.txt pattern) and sets
`realtime.state_snapshot_path` on the leg config with the write cadence parked, so the leg's consumer resumes at the seam;
a refused restore (age / fingerprint / layout) is logged and the leg's consumer cold-starts — a rider line says which.
The seed is re-written as **v9** (products must hash v8's `0f510024…`; the consumer snapshots present at every history
epoch).  Then the continuous consumer at the matched tows is the reference for every leg (consumer leg − seed at k, the
channel `ev_seam_grade.py` has always REPORTED and never gated because the consumer was cold).

**(C) The combined-σ contract** (M4, `network.emit_combined_sigma` + `closed_loop.consume_combined_sigma`, built and
validated on the 50-station golden in August, NOT armed on the field golden) is a config-only rung on the same ladder:
a refloated satellite's fresh phase-bias σ (0.47 m at the event epoch against 0.039 m) deweights its phase row 150× for
the filter window under the v1 URA + σ RSS the field consumer runs; the combined σ supersedes it.

**Gates (frozen).**  G0 unit: the shift preserves P exactly and the post-update position is unchanged to 1e-9 with vs
without a synthetic declared step; every reset-fallback branch; counter closure.  G1 byte-identity OFF: the certification
smoke (`_cert3_smoke_base`) reproduces `87a33bf0…` AND its `closed_loop_trace.csv` / `rt_engine.log` byte-for-byte; the
v9 seed's products hash v8's.  G2 (B): a null leg from v9 ep0604 whose consumer rider reads RESTORED, no consumer cold
start, consumer leg − seed at k = 1 within the snapshot's own re-linearization (report; cm expected) and flat to k = 60.
G3 (A) on the E3/G24 PIN wire from v9 (the declared G24 L5X re-realization at +10 that WTZZ observes): OFF resets G24 L5X
at the event, ON carries it (the log line with Δ), products byte-identical between the pair; consumer event − null over
+10..+40: ON's max |Δ3D| ≤ OFF's and ON's mean ≤ OFF's.  G4 (the twin, only after §24.3 has named a converging posture and
G2/G3 pass): the v8 recipe with that posture + (A) vs without — declared-step resets 29 → ≤ 2 (the first sightings), settled
p50 / rms not worse, and the instrument's own hour-to-hour p50 spread reported; then and only then §20.3's G-G is re-run on
the fixed slip posture with this consumer, and the four knobs are pinned or not on a bar the instrument can carry.

**§24.4 RESULTS — card CC: the carry is exact and neutral, the warm start works once the restore scope is right, and neither can be
graded on this wire's consumer (2026-09-14 ~04:40 local; worker on worktree-agent-a3344694b164c2e9f `ef2b134`, merged; exe
`F27E9CF9…`; harness `3f421edb…` (the consumer-snapshot additions only); seed v9; manager grade).**

**Build.**  (A) `network.closed_loop.pb_disc_carry` → `processing.ssr_phase_bias_disc_carry`; `StateManager::shiftAmbiguity`
(value-only); `PbDiscCarryLedger` (per-key last-applied value, counters `pb_disc_carried / pb_flip_carried / resets first_sight
/ basis / bssd / unknown_key`, closure at run end); the RtStateSnapshot v1 format unchanged (no value map — after a restore
the first bump per key resets, stated in the header).  (B) `network.closed_loop.state_snapshot`: the consumer's snapshot
written beside every network history copy with the copy's tow; `RealTimeEngine::writeStateSnapshotTo`, a RESTORED rider;
`event_leg.py` copies `state_snapshot.txt.consumer.epNNNN` into the leg (pristine `seed_consumer.txt` + disposable), sets
`realtime.state_snapshot_path` with the cadence parked, records both in leg_meta; `write-seed` arms the knob.  Suite 2083 /
282 / 0 failed (10 new `ConsumerDiscCarryTest` cases; 16 fixture-gated skips on the worker's empty `data/`).  **G1 PASS**:
`_cc_smoke_base` products `87a33bf0…`, consumer trace byte-identical (`7476f0a0…`), `rt_engine.log` identical.  **Seed v9**:
products `0f510024…` = v8, consumer trace = v8's, ep0604 fingerprint `545b8bdd2b638231`, consumer snapshots beside all 12
history copies (tow equal 12 / 12; consumer fingerprint `8ba709ddf9191c8a`).

**G2 — the restore takes, and the loader's default scope makes it useless.**  `cc_null_warm` (RESTORED at tow 277320, age
30 s, 29 states / 24 dynamic; products `e0d234ee…` = `cert_null_v8`).  Consumer leg − seed at matched tows, |Δ ENU|:

| restore posture | k = 1 | k = 11 | k = 30 | k = 60 | max / mean k 1..60 |
|---|---|---|---|---|---|
| `core` (loader default) | **14.43** | 4.28 | 2.12 | 1.21 | 14.43 / 2.61 — the leg then tracks §24.3's COLD trajectory to the digit |
| `full`, seam refloat ON | 0.83 | 6.89 | 3.31 | 2.53 | 6.89 / 2.99 — the refloat re-opens every ambiguity: worse than cold at k = 11 (18.8 m) |
| **`full`, seam refloat OFF** | **0.63** | 1.31 | 1.56 | 1.12 | **1.67 / 1.23** |

`core` restores the five core states and re-creates the 24 dynamic ones, so 48 code rows pull the position 9 m in one epoch;
`full` + `state_snapshot_seam_refloat: false` is the posture that resumes — and the 0.6–1.7 m it still sits from the seed's
consumer is the wire's own seam (clock contrast 0.15 m p50 and nine declared code-bias re-references at the seam epoch,
identical in every posture).  **Decision: the harness leg default becomes `full` + refloat OFF** (a harness default, not a
golden knob; recorded in the harness's default list).

**G3 — the carry does what it says; its consumer benefit is inside the instrument's noise.**  E3/G24 pin pair on v9:
products `adb75dc5…` byte-identical across OFF / CARRY / the fp2 leg.  OFF: `reset G24 L5X` at 277620 and `reset G06 L2W`
at 278250; CARRY: `carried G24 L5X by +0.776 m` and `carried G06 L2W by −12.795 m` (a 12.8 m declared step carried exactly),
no reset for G24, counters `events 2 → carried 2, resets 0, closure HOLDS`.  Consumer event − null over k 11..41: default
posture OFF max 3.380 / mean 2.400, CARRY 3.332 / 2.347 (PASS both bars; carry − off max 0.225, mean 0.093 m); warm posture
OFF 1.050 / 0.514, CARRY 1.054 / 0.513 (mean PASS, max by 4 mm the other way) — mm to cm, because the consumer's error is
wire-driven (§24.3 / §24.5) and the event − null curves are the pin posture's producer-side response common to both arms.

**Decision.**  (A) VALIDATED as exact and neutral (byte-identical OFF, counters closed, the shift lands to the millimetre);
**default OFF, not pinned** — an exact translation is strictly better than a code re-seed, but its benefit cannot be
measured on a wire that leaves a float user at metres, and the standing rule is validation before a pin; it is re-graded on
the §24.5 wire instrument's successor (a consumer-continuity read on a consistent wire) once §24.6's fix lands.  (B) LANDED
as the harness's consumer instrument: every leg from a v9-class seed carries a warm consumer; the `full` + refloat-OFF
default is set on main.  The G4 twin is NOT run: the §24.3 attribution has named no converging posture, because none exists
on this wire — the twin waits on the producer fix.  The pooled "consumer" bars of §20.3 / §21.21 / §23.2 stay suspended.

**§24.7 CERTIFICATION of main after card CC (2026-09-14 ~05:10 local; certification worker on main's own tree at `73cbbab`).**  Release and
Debug rebuilt first attempt (596 s / 274 s); Debug suite **2083 cases / 282 suites / 2082 passed / 0 failed / 1 documented skip**
(`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`; 84.6 s); Release exe `3E768AD2…` (copy `gnss_processor_cert2.exe`); smoke
`_cert4_smoke_base` (knobs OFF) `products.txt` `87a33bf0…` = cert3 and `closed_loop_trace.csv` byte-identical, no unknown-knob line, no
CC-B counter line; the warm null leg on main's exe (`cert4_null_warm`, v9 ep0604) products `e0d234ee…` = `cert_null_v8`, consumer
RESTORED (scope full, 29 states / 24 dynamic, age 30 s), leg − seed 0.6305 / 1.311 / 1.564 / 1.119 m at k = 1 / 11 / 30 / 60 (its consumer
trace byte-identical to the worker's `cc_null_warm_full_norefloat`).  One cross-exe note: the v8 and v9 `ep0604` snapshots differ by nine
bytes (the header write time and the URA quality-ramp counter 598 → 604, read only under `warmup_ura_floor_m > 0`; both seeds' products
`0f510024…`).  The per-row residual trace (§24.6) merged after this certification is trace-only; its own smoke pair (`_rows_smoke_base` /
`_rows_smoke_on`, both `87a33bf0…`) was run by its worker on the same code.

**§24.3 RESULTS (2026-09-14 ~01:20 local; the five legs, products byte-identical `e0d234ee…` in every arm; manager read with
`scratchpad/ci_consumer_read.py`).**  The consumer's own modeling is NOT the defect, and neither the iono supply nor the
network clock alone is:

| arm | k = 1 | k = 11 | k = 30 | k = 60 | p50 3D k 30–60 | \|up\| | north | east | slope m/ep |
|---|---|---|---|---|---|---|---|---|---|
| L0 as golden (FULL) | 3.96 | 16.13 | 11.37 | 7.18 | 8.44 | 5.98 | −5.92 | +0.79 | −0.137 |
| L1 no iono supply | 4.25 | 15.34 | 10.74 | 7.60 | 8.37 | 4.49 | −6.90 | +1.62 | −0.102 |
| L2 no network clock (bias-only) | 9.39 | 4.93 | 6.13 | 6.44 | 6.52 | 5.30 | −2.08 | +3.26 | +0.005 |
| L3 neither (CODE clock + network biases) | 6.21 | 6.65 | 8.16 | 7.47 | 8.24 | 6.61 | −2.81 | +3.88 | −0.029 |
| **L4 no products (plain PPP on CODE SP3/CLK/.BIA)** | **1.48** | **1.32** | **0.28** | **0.25** | **0.25** | **0.17** | −0.17 | +0.08 | 0.000 |

The same 6–8 GPS satellites (15° mask), the same filter, the same 30 minutes: on CODE's products the cold float converges to
0.25 m 3D / 0.17 m up by k = 30 and stays there; on ANY combination of the network's products it sits at 6–8 m at k = 60
(FULL mode first walks to 16 m by k = 10 — its ZWD state runs 0.07 → −1.37 → +3.41 m over eleven epochs while the frozen
`troposphere_q 1e-9` then holds whatever it latched; the continuous seed's consumer holds ZWD ≈ 0.6 m for six hours
against a physical ~0.15 m, which is the height bias of §24.1 in another coordinate).  L3 is the decisive row: CODE's clock
with the network's phase and code biases is as bad as FULL — so the damage is in the BIAS wire or in how the consumer
applies it, and a per-satellite constant cannot be the reason (a float ambiguity absorbs any constant).  Either the
network's biases, taken with a clock, differ from CODE's by something that MOVES within an arc at the metre level, or the
consumer's application of the network products is defective at the metre level — `project_closedloop_consumer_vertical_bug`
concluded the latter in August ("the defect is CONSUMER-side application; products are sound in isolation") and it was never
fixed.  The two are separated without a consumer, by the products themselves (§24.5).

**Corrections to §24.1's reading.**  The consumer is a healthy float PPP user (L4); its wander on the network wire is a
TRUE statement about the wire AS APPLIED, not an instrument artifact — but the bar problem stands as written: a wire that
leaves a float user at 6–8 m makes every consumer figure a random walk, and the resets are not the channel.  The
consumer-side bars stay suspended; the replacement instrument is §24.5.

### 24.5 The wire-vs-CODE consistency census — truth-referenced, datum-invariant, consumer-free, readable on any leg (pre-registered before it is written; a read-only Opus census, no producer run)

What the consumer applies per satellite s and phase signal L at epoch t is `A_net(s,L,t) = c·δt_net(s,t) + PB_net(s,L,t)`
(FULL) against what the CODE user applies, `A_code(s,L,t) = c·δt_CODE(s,t) + OSB_CODE(s,L)`; for code signals the same with
the network W6 code bias against CODE's code OSB.  A float user is indifferent to any per-satellite CONSTANT difference and
to any per-epoch common-mode difference (its ambiguities and receiver clock absorb them), so the quantity that reaches the
position is `D(s,L,t) = [A_net − A_code](s,L,t) − median_s[A_net − A_code](L,t)`, and its WITHIN-ARC variation is the
consumer's damage.  Census on `runs/evseed_field6h_v8` (6 h) and `runs/evseed_field6h_v8fp2` (the four-knob twin), the
products.txt `C` / `P` / `B` rows against the CODE 30 s CLK and OSB BIA the seed's config names (the S line's a-priori
identity; the consumer used the same files in L4): per satellite and signal, D's mean over each arc (the datum offset — a
sanity read: it should be the same constant for every signal of a satellite up to the OSB), its rms ABOUT the arc mean, and
the per-hour p50 / p95 of |D − arc mean| pooled over satellites; the same for the code side; the raw clock contrast
`c·δt_net − c·δt_CODE` de-medianed per epoch, p50 / p95 per hour.  Also the 60-epoch windows the legs use (k = 1..60 from
tow 277320) on both seeds and on `evleg_cert_null_v8`.

**Bars.**  W1 (consistency): pooled within-arc rms of D_phase ≤ 0.05 m per hour after hour 1 on v8 ⇒ the products are
consumer-consistent and the metre-level consumer failure is the consumer's APPLICATION (a consumer build card follows,
instrumented by the applied-correction dump); rms ≥ 0.5 m ⇒ the wire itself moves within arcs at the metre level and the
consumer is reading it right (then the between-satellite clock / phase-bias wander is the producer defect to attack, and
the pin question is moot until it is).  W2 (the pin, re-graded on this instrument): v8fp2's pooled within-arc rms of
D_phase and D_code not worse than v8's by more than 10 % in hours 3–6; the count of arcs with a within-arc step > 0.25 m
reported per twin.  W3 (the code side): pooled within-arc rms of D_code ≤ 0.3 m; a per-satellite code-side CONSTANT
offset larger than 1 m is reported by satellite (a datum inconsistency the consumer cannot absorb because it carries no
per-satellite code-bias state — `estimate_sat_biases false`).

**§24.5 RESULTS — the wire is NOT consumer-consistent, and the satellite CLOCK carries it (2026-09-14 ~02:50 local; read-only Opus
census, `scripts/wire_vs_code_census.py` merged `20377f2`; manager re-read).**  Conventions taken from the code, not from memory:
the `C` record is a DELTA on the a-priori CODE clock (`clock_corr = interpolateClock(CLK) + applyClockToBias(c0)/c`), the
consumer's applied phase term is `−C_net + PB_net` against the CODE user's `−0 + OSB` (the common `−c·δt_CODE` cancels), the
S-line's a-priori identity names the FIRST product path of its extension (the 128 file) while every epoch was served by the
129 CLK; GPS L5 / C5 are absent from CODE's MGEX OSB, so 24 k of 95 k P rows are outside the census.  Per hour, pooled
within-arc |D − arc mean| on v8 (declared arcs = split at gaps, `R` marks and counter bumps):

| h | D_phase rms, declared / tracking arcs | D_code rms, declared / tracking | raw clock contrast LEVEL p50 / p95; within-arc rms |
|---|---|---|---|
| 0 | 5.00 / 9.85 | 1.43 / 6.53 | 5.71 / 12.79; 5.64 |
| 1 | 4.48 / 6.37 | 1.34 / 5.19 | 4.09 / 12.90; 3.97 |
| 2 | 3.71 / 4.94 | 1.52 / 3.59 | 2.69 / 9.21; 2.90 |
| 3 | 4.88 / 6.17 | 1.92 / 3.95 | 2.87 / 11.56; 4.54 |
| 4 | 4.96 / 6.41 | 1.45 / 5.12 | 5.62 / 14.48; 5.00 |
| 5 | 2.76 / 9.23 | 1.49 / 6.44 | 5.84 / 16.79; 2.68 |

**W1 FAIL by ×10 on the "wire moves" branch**: 2.8–5.0 m rms within declared arcs (bar 0.05; the ≥ 0.5 m branch).  The
decomposition is the finding: `PB_net − OSB` is flat within arcs (0.12–0.37 m rms, ZERO steps > 0.25 m), `B_net − OSB` is
flat (0.13–0.22 m, zero steps), and `C_net` alone wanders 2.7–5.6 m rms within declared arcs with a 12–31 m between-satellite
spread (the producer's own `clock_std_m` 6.7 m at epoch 1, `CLK_STEP` 0 — in the estimator, not at emission); worst arc G11
L1C mean +60.7 m / rms 30.4 m, worst clock G05 drifting +9.7 → −11.7 m over hours 0–4.7.  **W2 — the four knobs did not change
it** (declared-arc phase rms within 3 %, code identical; 24–27 % worse on tracking arcs in hours 4–5 only because 312 vs 301
declared re-realizations put more steps inside tracking arcs) — the pin question is moot until W1 is fixed.  **W3 FAIL both
halves**: code within-arc rms 1.3–1.9 m (declared), and per-satellite CONSTANT code offsets > 1 m on 90 of 106 keys (up to
10.7 m; ±3–7 m in the pinned C1C/C2W IF direction, 13 m on the clock pivot G01) — which a consumer without per-satellite
code-bias states cannot absorb.  The 60-epoch window on `cert_null_v8` reproduces the continuous seed to a few % on every
kind: the inconsistency is the wire's, not a seam artefact.  So §24.3's consumer was right, and the response-cost story of
§20.3 was a reading of a wire that leaves a float user at metres regardless of any slip knob.

**Two things ruled in and out immediately (manager, 03:00).**  (i) The 61 held station coordinates are ITRF2020 propagated to
DOY 129 to < 1 mm (62 / 62 in `data/static/sinex/ITRF2020-IGS-TRF.SSC`) and the per-station phase residual rms is 0.01–0.17 m
(`network_pf_keyed.csv`, kind STA, hours 2–5) — not a coordinate or geometry blunder.  (ii) **The producer's own settled CODE
residuals are 1.8–9.8 m rms per station, median 4.5 m** (`s0_code` 4.5 against the 0.3 m assumed; the epoch σ̂₀ code ≈ 5 the
seam grader has printed all along), rising with isolation — GAMB 9.8, JPLM 9.6, MAJU 8.7, CAS1 8.1, SAVO 7.7, KOKV 7.6, PALM
7.6 … WROC 1.8, GOP6 2.2, FFMJ 2.5; corr(rms, log mean distance to the 3 nearest stations) = +0.51; per signal C1C 6.7, C2W
5.5, C5Q 5.4, C1W 5.3, C5X 5.3, C2X 4.3 — the L2 / L5 residuals are SMALLER than L1's, the opposite of a dispersive (ionospheric)
error (which would read ×1.65 / ×1.79); per satellite G16 8.8 … G19 3.7.  The satellite clock is what the code determines: a
code model inconsistent by metres, station-dependently, IS the clock's metre-level wander, and the phase side inherits it
through the clock.  The seven stations the frame-blind rx-CB gate refuses all day (§21.18) sit high (GAMB, KOKV, CPVG, LCK4,
LMMF, MRO1, GOPE) but are not the whole list (JPLM, MAJU, CAS1, SAVO, PALM are not refused).

### 24.6 The CODE-RESIDUAL attribution — per-LOS, from the producer's own post-fit residuals (pre-registered before the trace is read)

A null leg from v8 ep0604 (`ci_null_resid`: `trace_station_residuals: true` + `raim_carry_invariant: true`, trace-only + the
pinned screen; products must hash `e0d234ee…`) supplies per-LOS post-fit code AND phase residuals per epoch if the writer
rides the re-hosted pool (if it still writes zero rows on this lineage — the known gap — the writer is re-pointed at
`qc_amb` as a trace-only build item and the leg re-run).  Reads over the settled window k = 10..60, per (station, satellite,
signal): the residual MEAN (a constant no state absorbed), its rms about the mean, and its elevation dependence (bins
15–25°, 25–45°, > 45°).  Pre-registered discriminators, each with the mechanism it names:

| signature | mechanism it names |
|---|---|
| rms(C2W)/rms(C1C) ≈ 1.65, rms(C5)/rms(C1C) ≈ 1.79, same sign per LOS | ionosphere representation (the SH field / per-LOS deviation cannot follow the slant) |
| ratios ≈ 1, per-(station, signal) CONSTANT mean of metres, flat in elevation | a receiver code bias not absorbed — the rx-CB state's gate / withhold / canonical-signal keying (§21.18's frame-blind gate is the named suspect) |
| ratios ≈ 1, mean ≈ 0, rms ∝ 1/sin(el), phase clean | code multipath / observation noise model (then the 0.3 m code σ is wrong, not the model) |
| per-satellite constant common to all stations | a satellite code-bias / clock datum term (the W3 constants) |
| the phase residual of the same LOS shares the signature | geometry / troposphere / mapping (excluded so far by the cm phase rms) |

Bars: A1 the per-station code rms of the leg reproduces `network_pf_keyed.csv`'s ranking (the trace is the same statistic);
A2 the dominant signature is named by the table with > 70 % of the pooled code residual variance in ONE row; A3 the named
mechanism's fix is pre-registered as the next producer card WITH a wire-vs-CODE bar (W1's within-arc rms halved on a 60-epoch
leg, then on a seed) — the first time this project has had a truth-referenced wire bar to build against.

**§24.6 RESULTS — the code residual is a persistent per-LINE-OF-SIGHT level of metres, non-dispersive, with no home in the model
(2026-09-14 ~05:30 local; worker on worktree-agent-ad8d3f6516c27591c `7ece11f`, merged `0dac1cc`: `network.trace_postfit_rows` → `logs/
network_pf_rows.csv` (signed `resid_m = y − A_s·x_s − A_r·x_r`, `w`, `elev_deg` per row, written inside the P1-4 pass; smoke ON/OFF both
`87a33bf0…`) and `scripts/pf_rows_attrib.py`; leg `ci_null_rows` from v8 ep0604 with the pinned screen, products `e0d234ee…`, 145 587 rows
over 60 epochs; manager grade + the LOS forensic below).**

**A1 PASS**: the dump reproduces the leg's own `pf_keyed` STA statistic at all 3 111 (tow, station) cells (max |Δ| 5e-5) and its
per-station ranking at the same epochs (Spearman 0.996); the ranking is NOT stationary over the day (Spearman 0.53 against
hours 2–5: GOPE 3.6 → 9.1, HOLB 8.0 → 13.0, JPLM 10.8 → 16.3 m) — a per-LOS population, not fixed station constants.

**The shape (window k = 10..60, 1 303 LOS-signal series, pooled code rms 4.33 m; phase rms 0.3 mm — numerically zero under
free ambiguities).**  The per-LOS MEAN carries **92.3 %** of the sum of squares (|mean| p50 1.76 / p90 6.62 / max 29.8 m; rms
about the mean p50 0.56 m), persists across window halves (r 0.91, slope 0.87), and is ×3.2 larger below 25° than above 45°.
It is NOT dispersive: rms(C2W)/rms(C1C) = 0.87 and rms(C5)/rms(C1C) = 0.70 against the 1.65 / 1.79 an ionospheric error
carries; the between-signal energy is 12× what an ionospheric pattern allows; the C1C and C2W per-LOS means are uncorrelated
(r 0.03).  Nested variance decomposition: per-(station, signal) constants **20.2 %**, per-(satellite, signal) constants 8.3 %,
station-epoch and satellite-epoch common modes 0.5 % each, **per-LOS signal-common 51.9 %**, per-LOS dispersive 16.4 %,
remainder 2.2 %.  The seven stations the rx-CB gate refuses all day carry the DCB signature the (station, signal) constants
predict — C1C mean −2.86 m vs +0.40 for the rest, C2W +1.95 vs −0.49, opposite signs at 6 of 7 (GAMB −6.1 / +4.1, CPVG −4.8 /
+2.1, LCK4 −3.4 / +2.7 …) — §21.18's frame-blind gate priced at a fifth of the code residual variance.  A2 by the letter: the
largest row is "the per-LOS residual after every state row" at 70.6 %; the ionosphere row is shape-refuted.

**The LOS forensic (manager, 05:20).**  JPLM/G10 (C1W / C2W / C5Q ≈ −16 / −15 / −19 m at k = 10, el 23.4°, sliding to −27 / −27 /
−30 m by k = 45 at 17.8° — signal-common, growing as the satellite sets) is the worst of 493 LOS with ≥ 30 rows; the levels
correlate with NEITHER the Sagnac term (r −0.015; Sagnac rms 15.7 m across the LOS — an omitted or mis-signed Earth-rotation
correction is excluded) NOR the receiver-clock light-time term ρ̇·δt_r (r −0.13; JPLM's receiver clock is +260 km ≈ 0.87 ms, the
term 0.43 m); |level| vs 1/sin(el) r +0.33, and a per-station `a + b/sin(el)` fit explains only **8.2 %** of the level SS — the
levels are not a zenith-referenced station pattern either.  What the network's own zenith channel shows is the finding of the
hour: **the per-station ZWD states are unphysical** — SCTB −16.2 m (min −34.7), JPLM +21.7, MAJU −12.2, GOP6 −10.7, OAFA −8.0,
KRGG −4.9, WROC −4.4 m against a physical 0–0.4 m — the white-sink ZWD absorbs tens of metres of station-common, mapping-shaped
error every epoch, and the per-LOS levels are what the mapping function cannot fit.  The iono states for the worst LOS are
ordinary (JPLM/G10 slant 10–12 m L1 at 17–23°).  A non-dispersive, station-common, mapping-shaped error of tens of metres has
no physical source at an IGS site with ITRF coordinates; the discriminating test is a plain standalone PPP of the same
stations on CODE products (JPLM, SCTB, MAJU, and WROC as the control), which decides between the observations at those sites
and the network's per-station modeling — running as this is written.

**The standalone-PPP forensic (manager, 05:45) — the observations and the shared single-station model are FINE; the defect is the
network's own estimation.**  Four network stations run as plain PPP on CODE SP3 / CLK / .BIA (`config/ppp_internal.json`, GPS-only,
15° mask, 2 h from 00:00, `runs/_ci_ppp_*`), graded on the last 30 min against the ITRF2020 roster:

| station (network code rms, network ZWD) | 3D p50 | E / N / U | 3D at 30 / 60 min | tropo state | IF code resid rms | phase rms |
|---|---|---|---|---|---|---|
| WROC00POL (1.8 m, −4.4 m) | **0.032** | −0.026 / +0.002 / +0.013 | 0.17 / 0.07 | 0.090 | 0.42 | 0.006 |
| JPLM00USA (9.6 m, +21.7 m) | **0.118** | +0.018 / −0.021 / −0.116 | 11.2 / 3.97 | 0.107 | 0.47 | 0.007 |
| SCTB00ATA (7.2 m, −16.2 m) | **0.186** | +0.069 / +0.013 / +0.172 | 1.60 / 0.24 | −0.122 | 1.41 | 0.009 |
| MAJU00MHL (8.7 m, −12.2 m) | **0.160** | +0.089 / +0.055 / +0.122 | 4.41 / 1.88 | 0.386 | 0.94 | 0.018 |

JPLM/G10 — the network's worst LOS at −25 m — reads **+0.04 m mean / 0.24 m rms** standalone.  Every geometric explanation
of the network's per-LOS levels is excluded by direct test on the 493 LOS of `ci_null_rows`: the Sagnac term (r −0.015),
the receiver-clock light-time term (r −0.13), a light-time or transmit-time error (corr with the range rate +0.125, slope
+1 ms), and a per-station position-error vector (a 4-parameter fit per station explains 42.8 % of the level SS with fitted
|d| of 3–20 m — the roster is ITRF2020 to 1 mm, so these are not coordinates).  The levels are metres, per LOS, non-dispersive,
persistent, present only in the network's uncombined multi-station solve, whose per-station ZWD sits at ±5–35 m to
accommodate them.  **Conclusion: the network producer's code model — the uncombined per-signal code rows with estimated
satellite code biases (W6), carried receiver code biases (frozen at their first-epoch seed, q 1e-9), the per-LOS ionosphere
tied to the SH field, and the per-epoch ZWD sink — is inconsistent with the data by metres at every station, worst at the
isolated ones; the satellite clock, which the code determines, inherits it (§24.5), and the wire's metre-level inconsistency
with CODE, the consumer's failure to converge (§24.3), and every "level channel" finding since July are downstream of it.**

### 24.8 Card CM — CODE-MODEL CONSISTENCY: single-knob COLD arms that name the mechanism (pre-registered before any arm is read)

Six arms from the field golden, 2 h cold from 00:00 (240 epochs; the fingerprinted knobs must cold-start), one knob each
against the control, every arm with `network.trace_postfit_rows: true` (trace-only):

| arm | knob | what it removes |
|---|---|---|
| A0 | control (the golden + the trace) | — |
| A1 | `network.carry_rx_code_bias: false` | the receiver code-bias carry (the frozen first-epoch memory; §21.18's gate) |
| A2 | `network.estimate_sat_code_bias: false` + `model_corrections.enable_code_phase_bias: true` | W6 — CODE's OSB replaces the estimated satellite code biases (the consumer L4's satellite side) |
| A3 | `network.estimate_iono_sh: false` | the SH-VTEC tie — the per-LOS ionosphere floats at `ionosphere_q` |
| A4 | `filter.process_noise.sat_code_bias_q: 1e-4` | the frozen satellite code-bias states (q 1e-10 → 1e-4) |
| A5 | `network.cb_kaula_prior: false` | the 3 m Kaula prior on the code biases |

A knob the loader refuses on this lineage is reported with the loader's line and the arm dropped, never worked around; a knob
that forces others (W6's forcing of `estimate_sat_biases`, `promote_sat_code_bias`, `estimate_ifb`) is recorded with the
effective posture from `gnss_config.log`.  **Reads per arm, last hour (k 121..240):** (i) per-station code residual rms p50
across stations (`network_pf_keyed.csv` STA; control ≈ 4.5 m); (ii) per-station ZWD absolute p50 and the count of stations with
|ZWD| > 0.5 m (`network_zwd.csv`; control 7 stations beyond 5 m); (iii) the wire-vs-CODE census on the arm's `products.txt`
(`scripts/wire_vs_code_census.py`, hour 1: within-arc rms of D_phase and D_code, the raw clock contrast LEVEL p50; control
2.8–5 m / 1.3–1.9 m / 3–6 m); (iv) the per-LOS level SS share and its dispersive fraction (`scripts/pf_rows_attrib.py`); (v)
availability (emitted / withheld) and the run-end counters.  **Bars (frozen):** an arm NAMES the mechanism when it takes (i)
below 1.0 m AND (ii) to zero stations beyond 0.5 m AND (iii) D_phase within-arc rms below 0.5 m in hour 1 — all three; a
partial mover (any bar halved) is reported as a contributor; no bar moved ⇒ that mechanism is exonerated.  Two or more arms
crossing the bars ⇒ the fix card combines them and is graded on the same three plus the consumer's convergence (§24.3's
instrument, now with a warm consumer).  Availability must not fall below the control's by more than 5 %; an arm that holds the
bars by withholding is not a pass.
