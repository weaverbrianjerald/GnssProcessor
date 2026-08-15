# RT Network-Producer Service Operations

Operator guide for running the PPP-RTK network producer as a long-lived service: the health surface, the
product-level `H` heartbeat, the emission guards, trace rotation, and restart/supervision.  Consumer-facing product
semantics (URA, discontinuity counters, S contract) are out of scope — see the product docs and
`docs/network_ambiguity_resolution_design.md`.  The RT *service program* record (conventions, brdc_rts cards,
streaming front-end, open RTF cards) is `docs/rt_service_conventions.md`.

## 1. The operator health CSV — `network_service_health.csv`

Armed by `network.health_interval_s > 0` (seconds of **content** time — processed data time, not wall time;
`network.service_mode` arms it at 30 s).  One row per interval, flushed immediately so it can be tailed live,
written to the network log directory alongside `network_trace.csv`.  Each row reports cumulative state **through
the previous epoch** (the current epoch's outcome is unknown when the row is written).

**Naming note:** the pyviz viewer called `network_health` reads `network_trace.csv` — a *different* surface
(per-epoch estimability and convergence).  This file is the *service* heartbeat.

| Column | Semantics |
| --- | --- |
| `week`, `tow` | GPS week / time-of-week of the epoch that triggered the row (content time). |
| `n_emitted` | Cumulative epochs emitted as product. |
| `n_skipped` | Cumulative epochs skipped for any reason (superset of the two withhold counters below plus solve/datum/sync failures). |
| `n_gate_withheld` | Cumulative epochs withheld by the rank/null-space gate. |
| `n_sigma0_withheld` | Cumulative epochs withheld by the post-adaptation σ̂0 ceiling (`network.raim_withhold_sigma0`); subset of `n_skipped`. |
| `n_runaway_epochs` | Cumulative epochs on which the RT0-5 clock-runaway quarantine pruned at least one constellation. |
| `sigma0_ewma` | The network-wide σ̂0 EWMA that scales the emitted URA.  **Dependency caveat:** it updates only inside the warm post-fit RAIM block — it needs `network.emit_ura` **and** `network.raim` **and** `network.phase_leveling` all armed.  With any of them off the column reports the explicit **`-1` "unavailable" sentinel** (never a silently frozen `1.0`) and a startup WARN says so.  During phase-leveling warmup it legitimately reads its `1.0` init until the carry warms. |
| `n_present` | Stations present in the most recent processed epoch. |
| `n_clock_sats` | Registered satellite-clock columns in the most recent processed epoch. |
| `ms_per_epoch_window` | Mean wall-clock ms per loop epoch since the **previous health row** (first row: since loop start).  A windowed load signal — watch it trend up. |
| `last_emit_age_s` | Content-time seconds since the last emitted epoch; `-1` before the first emit.  The primary "is it still publishing" alarm input. |
| `pivot` | Realized per-constellation clock pivots of the last **emitted** product, e.g. `E:E11;G:G05` (semicolon-joined, no commas).  Empty before the first emit.  A change here means the product datum re-referenced — a consumer-visible discontinuity. |
| `cold_start_grade_s` | RTB-7(c): seconds of product time from the first emit until the startup quality ramp lifted (`network.warmup_ura_floor_m`).  `-1` before the lift or when the ramp is off; `0` on a warm resume (a lifted ramp was restored from the state snapshot, so the producer was never degraded). |

Alarming: `last_emit_age_s` above a few product intervals ⇒ alive but not publishing; `sigma0_ewma` trending up ⇒
contamination; `ms_per_epoch_window` approaching the epoch interval ⇒ falling behind real time.

**This file does not cover every withhold tier.**  The SH health gate (§3.2) withholds emission without touching
*any* counter here — `n_emitted` freezes and `last_emit_age_s` climbs while `n_skipped` / `n_gate_withheld` /
`n_sigma0_withheld` stay flat.  Read §3 before wiring a "why is it not publishing" alarm off this file alone.

## 2. Relation to the product-level `H` heartbeat (RTA-10)

Two heartbeats, two questions — they intentionally do not duplicate each other:

- **`network_service_health.csv` (this file, RTB-6)** — *operator* surface.  Aggregate liveness/throughput/trend at
  a slow cadence, on the producer's local disk; never leaves the producer box, not product content.
- **`H` epoch-status records in the product stream (RTA-10)** — *consumer* surface.  One record per epoch inside the
  product file, letting a downstream consumer distinguish "the producer deliberately withheld this
  epoch/constellation" from "my transport dropped it".

### 2.1 `H` record grammar and states

```
H,week,tow,status,sigma0,n_sats_published,withheld
```

Declared in the product header legend, written by `NetworkProductWriter::writeEpochStatus`.  `sigma0` is `0.0000`
when unavailable; `n_sats_published` is 0 on any withhold; `withheld` is `-` (none), a semicolon-joined
constellation list (e.g. `C;R`), or `*` for a whole-epoch withhold.  A withheld epoch emits an `H` line and **no**
`E` line — but only once the deferred product writer has opened, i.e. from the **first successful emit** onward.
Epochs withheld before the producer ever published leave no record in the product file (there is none yet); those
are visible only in the operator surfaces.

| `status` | Meaning | Emitted by |
| --- | --- | --- |
| `ok` | Epoch published in full. | normal emit |
| `partial` | Published, but ≥1 constellation withheld — see the `withheld` field. | per-constellation σ̂0 carve-out |
| `withheld_gate` | Whole epoch withheld by the rank / null-space datum gate. | §3.1 |
| `withheld_sigma0` | Whole epoch withheld by the post-adaptation σ̂0 ceiling (`network.raim_withhold_sigma0`). | §3.1 |
| **`withheld_sh`** | **Whole epoch withheld by the SH health gate — the ionospheric field left its physical band.  NEW; see §3.2.** | §3.2 |
| *anything else* | A **newer** producer's token.  Parsed as `Unknown`, surfaced, never fatal. | — |

Forward-compatible `Unknown` is the contract that lets a producer add a state without breaking deployed consumers —
`withheld_sh` is its first exercise, and a consumer built before it reports `unknown` rather than failing.

**What a consumer does with it today:** `RealTimeEngine` logs producer-status *transitions* edge-triggered (first
sighting and every change; a steady state stays silent), INFO for `ok` and WARN otherwise.  It does **not** yet gate
measurement usage or inflate `R` on the status — that is RTF-9, deliberately out of scope.  An `H` state is
therefore a *diagnostic*, not an integrity action: a consumer riding a `withheld_sh` era coasts on its last
correction set subject to the ordinary staleness limits.

## 3. Emission guards — what withholds, and what the operator sees

Four independent tiers can stop or alter an emission, differing in **epoch semantics** (does the epoch still enter
the carry?) and in **which surface records them**:

| tier | knob | epoch semantics | health CSV | `network_epoch_status.csv` | `H` state | `network_events.csv` |
| --- | --- | --- | --- | --- | --- | --- |
| rank / null-space gate | (always) | skip epoch | `n_skipped`, `n_gate_withheld` | `gate_withheld` | `withheld_gate` | — |
| A4 σ̂0 ceiling | `network.raim_withhold_sigma0` | skip epoch — the contaminated solve never enters the carry | `n_skipped`, `n_sigma0_withheld` | `sigma0_withheld` | `withheld_sigma0` | `SIGMA0` |
| **SH health gate** | `network.sh_health_gate` | **emission only** — solve, carry and every QC tier still run | **nothing** | `sh_gate_withheld` | `withheld_sh` | `SH_GATE` |
| **CB-excursion reinit** | `network.cb_excursion_reinit` | not a withhold — repairs state and signals a discontinuity | — | — | — | `CB_EXC` |

Both bolded tiers are **default OFF and must stay off**; both now carry a DOY-129 field verdict
(`docs/CONFIG_promotion_ledger.md` is the standing record):

- **`network.sh_health_gate` — mechanism VALIDATED, calibration FALSIFIED and NON-TRANSFERABLE.**  **46.3 % of
  epochs withheld (278/600)**, **239 consecutive**, swallowing both event windows *and* the pre-event baseline: its
  a₀₀ band is a **datum-coupled** quantity (standing invariant #5) differing **~25 TECU between two realizations of
  the same day**.  The only KEEP_OFF-because-FALSIFIED knob in its family.  Its degrade-instead-of-withhold path
  also **DEADLOCKS** as wired (`docs/SPEC_day129_frame_guards.md` Part C5(1)): NetworkSolPrep early-returns on
  `sh_gate_engaged` while the URA-ramp lift and `++n_emitted` are both downstream, so arming `warmup_ura_floor_m`
  with this gate means the floor can never lift.  A fix is specified — gate on field SHAPE (rms over degrees ≥ 1 as
  a RATIO to a frozen converged-window median) — but unbuilt.
  > ⚠ **Corpus tension, flagged not resolved:** CLAUDE.md's "standard armed set" still lists `sh_health_gate` among
  > the fingerprint-SAFE arms while the promotion ledger marks it KEEP_OFF/falsified.  Get a ruling before arming it
  > on either document alone.
- **`network.cb_excursion_reinit` — mechanics field-validated over 13.2 h, POSTURE falsified; MUST STAY OFF.**
  5 trips / 5 G01 reinits / 1 disc bump, all seam-coupled, but the D1 pivot-only scope is falsified — **130
  non-pivot (sat, epoch) cases**; the seam ratcheted *every* satellite's code bias.  The disease is real and large
  (+49 m on IF(C1W,C2W) of G01, τ = 3.79 h): a build-the-right-scope block, not a falsified idea.

Read the run-end lines before believing either worked.

### 3.1 The two skip-epoch tiers (existing behaviour, for contrast)

Both increment `n_skipped` **and** their own counter, so a withholding era is visible from the health CSV alone: the
tier counter climbs in lockstep with `last_emit_age_s`.  That is what §1's alarm suggestion assumes — and exactly
what the SH gate does not do.

### 3.2 SH health gate — `network.sh_health_gate`

**What it detects.**  Post-solve, pre-emit, the estimated mean VTEC (the a₀₀ coefficient; the layer SUM under
dual-layer SH) against the physical band `estimation.global_vtec_sh.a00_warn_{low,high}_tecu` (defaults 2.0 / 150.0
TECU — the same band the T1a WARN uses) and, optionally, the coefficient RMS against
`network.sh_gate_rms_max_tecu` (default 0 = off).  Engages after `network.sh_gate_min_epochs` **consecutive**
out-of-band epochs (default 1); releases on the first in-band epoch.  It replaces a throttled per-epoch WARN that
under-reported the reference run 23.8× — the out-of-band count here is per **epoch**, un-throttled.

**Why emission-only.**  The A4 σ̂0 tier skips the whole epoch, right for a 1–3-epoch transient.  An SH era is not a
transient — the reference day carries an 80.5-minute block — and skipping the epoch would starve the carry past
`network.phase_leveling_max_gap_s` (default 120 s; the DOY-129 reference configuration runs 900 s) and **expire
it**, turning a wire-quality problem into a full re-convergence.  The gate withholds only the wire: solve, carry
stash and every QC tier keep running, so the producer stays genuinely healthy and resumes publishing on release with
no re-convergence.  Pending discontinuity bumps survive a withheld era untouched and land with the next actual emit.

**Operator signature — read this before writing an alarm.**  During an engaged era the
health CSV shows `n_emitted` frozen and `last_emit_age_s` climbing while `n_skipped`,
`n_gate_withheld`, `n_sigma0_withheld`, `sigma0_ewma`, `n_present` and `n_clock_sats` all
look normal.  "Not publishing and no counter moved" **is** the SH gate.  Confirm in either:

* `network_events.csv` — `SH_GATE` rows, `engage` / `withhold` (one per withheld epoch) / `release`, each carrying
  a₀₀, the violated bound, `rms_all` and the consecutive out-of-band run;
* the product stream — `H` lines with `withheld_sh` (§2.1), what a downstream consumer sees.

The main log states the era directly: WARN at engage, a count-bearing WARN every 20 consecutive engaged epochs,
INFO at release with running totals.

**Prerequisites (the loader disables the gate and warns if unmet).**  `network.estimate_iono_sh` on, and at least
one live criterion — an active band (`a00_warn_low_tecu < a00_warn_high_tecu`) or a positive
`sh_gate_rms_max_tecu`.  `sh_gate_min_epochs < 1` is clamped to 1.

**Run end (invariant #4).**  One line: out-of-band epochs, engagements, emissions withheld, active thresholds, and
whether the gate was **still engaged at run end** ("the era outlived the data").  If the field never left its band
the line is a WARN saying the gate is untested in that run — do not read a clean run as evidence it works.

**Restart:** the gate is **not** in the resume fingerprint (it belongs to the `raim` class, not the datum class), so
it can be armed, disarmed or retuned across a bounce and still resume **warm**.

### 3.3 CB-excursion reinit — `network.cb_excursion_reinit`

Not a withhold: a **state-repair** guard — the only tier here that alters product *content* rather than its
presence; the stream keeps flowing throughout.

**What it detects.**  The per-epoch **median** over satellites of |Δ clock| against
`network.cb_excursion_median_m` (default 50.0 m).  Datum-change epochs are exempt — a handover moves every clock, a
datum artifact rather than a fault.  The median is the discriminator on purpose: blind to a two-sided excursion
(satellites moving in opposite directions, no lasting harm), saturating on a one-sided one (the whole constellation
moving together — the case that ratchets into the pinned code biases).

**Response.**  The clock-pivot satellite's **code-bias** carry is marked for reinit (folded next epoch at
`network.clock_monitor_reinit_sigma_m`, default 30 m) — and only the code bias: the clock is soft and self-recovers
in 2–4 epochs, phase biases measurably do not move at these events, and the near-static code bias is the one state
that ratchets.  The reinit is queued as a **code-bias discontinuity bump** on the next emit, so the value step is
*signalled* on the wire and a replaying consumer reproduces the counters.  `network.cb_excursion_reinit_all_sats`
(default false) widens the response from the pivot to every satellite.

**Consumer-visible effect.**  A discontinuity-counter increment on the affected satellite's code bias — the
ordinary "this bias jumped, do not carry it across" signal.  No `H`-state change, no gap in the stream.

**Operator surface.**  `CB_EXC` rows in `network_events.csv`: `cb_excursion_reinit` at the trip (carrying the
median, the threshold and the marked satellites) and `cb_excursion_disc_landed` when the queued bump ships.

**Run end (invariant #4).**  Trips, reinits, bumps landed, the largest median observed and its TOW, and the D1
counter for how many non-pivot cases a per-satellite rule would have caught.  **0 trips = the guard was inert**, and
the line says so as a WARN.  A second WARN fires if a queued bump never landed (its satellite went absent from every
later emission) — a value was stepped without the product signalling it; investigate.

**Restart — this one is fingerprinted.**  When armed, `cb_excursion_reinit` contributes the threshold and the D1b
scope to the resume fingerprint.  Arming it, disarming it, **or retuning `cb_excursion_median_m` across a bounce
forces a COLD start** (fingerprint mismatch ⇒ new generation ID ⇒ every consumer resets; §4).  Change it during a
planned outage, not as a live tweak.  While unarmed the fingerprint is byte-identical to one from a build predating
the guard.

## 4. Restart and supervision policy

- **Run under a supervisor** (NSSM/WinSW, a Task-Scheduler restart-on-failure task, or systemd) that restarts the
  producer on crash or exit.  The producer keeps no on-disk state it cannot rebuild; a restart is a cold start
  unless the snapshot below is armed.
- **Windows WER-zombie failure mode (known project gotcha):** a crashed `gnss_processor` can be held open by Windows
  Error Reporting as a zombie process, and that zombie **wedges every later launch** of the executable until
  killed.  The supervisor's restart action MUST pre-kill any lingering `gnss_processor` instances —
  `taskkill /F /IM gnss_processor.exe 2>$null; Start-Sleep 2; <relaunch>`.  A plain "restart on exit" policy without
  this loops forever on a wedged binary.
- **Restart datum semantics (RTB-7):** arm `network.state_snapshot_path` (with
  `network.state_snapshot_interval_s`, default 10 s of product time) and the producer atomically snapshots BOTH
  cross-epoch state layers (driver carries / oscillators / pivots / pending datum bumps, and the emitter's
  discontinuity stream state).  A bounce shorter than the carry-validity horizon (max of
  `phase_leveling_max_gap_s` / `clock_model_max_gap_s`) under an unchanged datum-relevant config resumes **WARM**:
  the RTA-3 generation ID is preserved, consumers see the same product datum and do not reset, and a `WARM RESUME`
  banner reports snapshot age and restored key counts.  Anything else — no/stale/corrupt snapshot, config
  fingerprint mismatch — **cold-starts safely**: a fresh **generation ID** (`network.emit_generation_id`, RTA-3)
  signals the global datum break and the startup quality ramp (`network.warmup_ura_floor_m` /
  `network.warmup_epochs`) advertises degraded URA until the σ̂0 EWMA and carry population stabilize (the lift logs
  the cold-start-to-product-grade metric, also a health-CSV column).

## 5. Trace rotation — `network.trace_rotate_mb` / `network.trace_rotate_keep`

Card **P1**.  Three CSVs grow without bound in a 24/7 run: `network_trace.csv` (per epoch),
`network_solution.csv` (per satellite per epoch — the big one) and `network_service_health.csv` (per heartbeat).
Size-based rotation is armed by `network.trace_rotate_mb > 0` (MiB; checked once per processed epoch, so a file
overshoots by at most one epoch's rows; default 0 = off, byte-identical):

```
file.csv          live file exceeds the threshold
file.csv.1        ← previous live file (newest rolled generation)
file.csv.2        ← shifted from .1
...
file.csv.<keep>   oldest; deleted at the next roll (keep = trace_rotate_keep, default 3)
```

The fresh live file is reopened **with the header row re-written**, so every rolled generation is independently
parseable.

**Viewers do not auto-discover rotated suffixes** — the pyviz viewers read only the live file name.  To inspect
history across a rotation boundary, concatenate the chain oldest-first, stripping every generation's header but the
first (or point a viewer at one renamed generation at a time):

```powershell
Get-Content network_trace.csv.3 | Select-Object -First 1 | Set-Content combined.csv -Encoding utf8
Get-Content network_trace.csv.3, network_trace.csv.2, network_trace.csv.1, network_trace.csv |
    Where-Object { $_ -notmatch '^week,' } | Add-Content combined.csv
```

If a rolled file cannot be renamed because an external tail-reader holds a lock, the producer keeps appending to the
live file and retries at a later epoch — no rows are lost.

Rotation does **not** apply to the product file (`network.product_output_path`) — that is a consumer transport
surface with its own contract (S-line re-broadcast, generation IDs), not an operator log.
