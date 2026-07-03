# RT Network-Producer Service Operations

Operator guide for running the PPP-RTK network producer as a long-lived
service.  Covers the machine-readable health surface, trace rotation, and the
restart/supervision policy.  Consumer-facing product semantics (URA,
discontinuity counters, S contract) are out of scope here — see the product
docs and `docs/network_ambiguity_resolution_design.md`.

## 1. The operator health CSV — `network_service_health.csv`

Armed by `network.health_interval_s > 0` (seconds of **content** time, i.e.
processed data time, not wall time; `network.service_mode` arms it at 30 s).
One row is appended per interval, flushed immediately so it can be tailed
live.  Written to the network log directory alongside `network_trace.csv`.

**Naming note:** the pyviz viewer called `network_health` reads
`network_trace.csv` — a *different* surface (per-epoch estimability and
convergence).  This file is the *service* heartbeat, hence the name.

Each row reports cumulative state **through the previous epoch** (the current
epoch's outcome is not yet known when the row is written).

| Column | Semantics |
| --- | --- |
| `week`, `tow` | GPS week / time-of-week of the epoch that triggered the row (content time). |
| `n_emitted` | Cumulative epochs emitted as product. |
| `n_skipped` | Cumulative epochs skipped for any reason (superset of the two withhold counters below plus solve/datum/sync failures). |
| `n_gate_withheld` | Cumulative epochs withheld by the rank/null-space gate. |
| `n_sigma0_withheld` | Cumulative epochs withheld by the post-adaptation σ̂0 ceiling (`network.raim_withhold_sigma0`); subset of `n_skipped`. |
| `n_runaway_epochs` | Cumulative epochs on which the RT0-5 clock-runaway quarantine pruned at least one constellation. |
| `sigma0_ewma` | The network-wide σ̂0 EWMA that scales the emitted URA.  **Dependency caveat:** this EWMA updates only inside the warm post-fit RAIM block — it needs `network.emit_ura` **and** `network.raim` **and** `network.phase_leveling` all armed.  When any of them is off the column reports the explicit **`-1` "unavailable" sentinel** (never a silently frozen `1.0`), and a startup WARN says so.  During the phase-leveling warmup it legitimately reads its `1.0` init until the carry warms. |
| `n_present` | Stations present in the most recent processed epoch. |
| `n_clock_sats` | Registered satellite-clock columns in the most recent processed epoch. |
| `ms_per_epoch_window` | Mean wall-clock ms per loop epoch over the window since the **previous health row** (first row: since loop start).  A windowed load signal — watch it trend up. |
| `last_emit_age_s` | Content-time seconds since the last emitted epoch; `-1` before the first emit.  The primary "is it still publishing" alarm input. |
| `pivot` | Realized per-constellation clock pivots of the last **emitted** product, e.g. `E:E11;G:G05` (semicolon-joined, no commas).  Empty before the first emit.  A change here means the product datum re-referenced (a consumer-visible discontinuity). |
| `cold_start_grade_s` | RTB-7(c): cold-start-to-product-grade — seconds of product time from the first emit until the startup quality ramp lifted (`network.warmup_ura_floor_m`).  `-1` before the lift or when the ramp is off; `0` on a warm resume (the producer restored a lifted ramp from its state snapshot and was never degraded). |

Alarming suggestions: `last_emit_age_s` above a few product intervals ⇒ the
service is alive but not publishing (check gate/σ̂0 counters for which tier is
withholding); `sigma0_ewma` trending up ⇒ contamination; `ms_per_epoch_window`
approaching the epoch interval ⇒ falling behind real time.

## 2. Relation to the product-level `H` heartbeat (RTA-10)

Two heartbeats, two questions:

- **`network_service_health.csv` (this file, RTB-6)** — *operator* surface.
  Aggregate liveness, throughput, and trend data at a slow cadence, on the
  producer's local disk.  Answers "is the service healthy and how is it
  trending?"  Never leaves the producer box; not product content.
- **`H` epoch-status records in the product stream (RTA-10)** — *consumer*
  surface.  One record per epoch inside the product file itself
  (`ok`/`partial` + σ̂0 + withheld constellations).  Lets a downstream consumer
  distinguish "the producer deliberately withheld this epoch/constellation"
  from "my transport dropped it".

They intentionally do not duplicate each other: an operator polls the CSV; a
consumer parses the `H` records.

## 3. Restart and supervision policy

- **Run under a supervisor** (Windows service wrapper such as NSSM/WinSW, a
  Task-Scheduler restart-on-failure task, or systemd on Unix) that restarts
  the producer on crash or exit.  The producer keeps no on-disk state it
  cannot rebuild; a restart is a cold start (see the datum note below).
- **Windows WER-zombie failure mode (known project gotcha):** a crashed
  `gnss_processor` can be held open by Windows Error Reporting as a zombie
  process, and that zombie **wedges every later launch** of the executable
  until it is killed.  The supervisor's restart action MUST therefore kill any
  lingering `gnss_processor` instances before relaunching, e.g.

  ```powershell
  taskkill /F /IM gnss_processor.exe 2>$null; Start-Sleep 2; <relaunch>
  ```

  A plain "restart on exit" policy without this pre-kill will loop forever on
  a wedged binary.
- **Restart datum semantics (RTB-7):** arm `network.state_snapshot_path` (with
  `network.state_snapshot_interval_s`, default 10 s of product time) and the
  producer atomically maintains a state snapshot of BOTH cross-epoch state
  layers (driver carries/oscillators/pivots/pending datum bumps + the
  emitter's discontinuity stream state).  A bounce shorter than the
  carry-validity horizon (max of `phase_leveling_max_gap_s` /
  `clock_model_max_gap_s`) under an unchanged datum-relevant config resumes
  **WARM**: the RTA-3 generation ID is preserved, consumers see the same
  product datum and do not reset, and a `WARM RESUME` banner reports the
  snapshot age and restored key counts.  Anything else — no/stale/corrupt
  snapshot, config fingerprint mismatch — **cold-starts safely**: a fresh
  solution **generation ID** (`network.emit_generation_id`, RTA-3) signals the
  global datum break, and the startup quality ramp
  (`network.warmup_ura_floor_m` / `network.warmup_epochs`) advertises degraded
  URA until the σ̂0 EWMA and the carry population have genuinely stabilized
  (the lift logs the cold-start-to-product-grade metric, also a health-CSV
  column).

## 4. Trace rotation — `network.trace_rotate_mb` / `network.trace_rotate_keep`

Three CSVs grow without bound in a 24/7 run: `network_trace.csv` (per epoch),
`network_solution.csv` (per satellite per epoch — the big one), and
`network_service_health.csv` (per heartbeat).  Size-based rotation is armed by
`network.trace_rotate_mb > 0` (MiB; checked once per processed epoch, so a
file overshoots by at most one epoch's rows; default 0 = off, byte-identical):

```
file.csv          live file exceeds the threshold
file.csv.1        ← previous live file (newest rolled generation)
file.csv.2        ← shifted from .1
...
file.csv.<keep>   oldest; deleted at the next roll (keep = trace_rotate_keep, default 3)
```

The fresh live file is reopened **with the header row re-written**, so every
rolled generation is independently parseable.

**Viewers do not auto-discover rotated suffixes.**  The pyviz viewers read
only the live file name.  To inspect history across a rotation boundary,
concatenate the chain oldest-first, stripping the header line of every
generation except the first:

```powershell
Get-Content network_trace.csv.3, network_trace.csv.2, network_trace.csv.1 |
    Where-Object { $_ -notmatch '^week,' } |
    Set-Content combined.csv -Encoding utf8
Get-Content network_trace.csv | Select-Object -Skip 0 | Add-Content combined.csv
```

(or simply point a viewer at one renamed generation at a time).  If a rolled
file cannot be renamed because an external tail-reader holds a lock, the
producer keeps appending to the live file and retries at a later epoch — no
rows are lost.

Note the rotation does **not** apply to the product file
(`network.product_output_path`) — that is a consumer transport surface with
its own contract (S-line re-broadcast, generation IDs), not an operator log.
