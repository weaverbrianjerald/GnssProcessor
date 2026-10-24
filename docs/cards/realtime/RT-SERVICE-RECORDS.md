# RT-SERVICE: real-time service work queue, the constant-velocity slant ionosphere and the RT report decisions

**STATUS**

- **id:** RT-SERVICE
- **topic:** realtime
- **status:** reference
- **summary:** State of the real-time service work queue (RT0, RTA and RTB cards closed; the RTF-3 remainder and RTF-4 to RTF-9 open), the user-side constant-velocity slant-ionosphere block (default off) and the decisions of the surveyed-reference RT report.
- **dates in the text:** 2026-07-02 to 2026-10-07
- **source:** agent memory notes `project_rt_service_work_queue`, `project_cv_slant_iono` and `project_rt_report_surveyed_reference`, copied 2026-10-24

**Outcome.** The goal of the queue is a live PPP-RTK service on the project's own products with only the real-time orbits external. The notes record the lessons of the closed cards (conventions in `docs/rt_service_conventions.md`), the open remainder, the constant-velocity slant-iono model and its falsified diagonal sibling, and the decisions of the real-time scorecard (suffix-stable convergence, the surveyed reference). Status notes mark what changed after 2026-10-07.

> Results are appended TO THIS FILE, below the existing text.

---

## 2026-10-24: real-time service work queue: closed cards, the open remainder and the lessons

These results were recorded only in the agent memory note `project_rt_service_work_queue` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

Status note, 2026-10-24: `network.service_mode` is KEEP_OFF in the ledger, so the call below for an armed service_mode smoke followed by a 1 h run is stale; the test file named below is `tests/network/test_network_product_io.cpp`. The RTF-3 remainder and RTF-4 to RTF-9 match docs/status/OPEN_ITEMS.md.

Goal: a live PPP-RTK service on our own products, only RT orbits external. Conventions as in
memory note `project_network_enhancement_queue` (default OFF, byte-identical OFF, Release runs, smoke then 1 h, one card
per session).

Status (tree-verified 2026-07-28, unchanged in the docs 2026-10-07): CLOSED + COMMITTED at or before `dee2824`:
RT0-1..5, RTA-1..13, RTB-1..8 (26 OPUS cards) and brdc_rts Cards 1-8; RTF-3 increment 1 landed 2026-08-01.
OPEN: the RTF-3 remainder (end-to-end ON validation and the header-free station factory, both FIELD-BLOCKING)
and RTF-4..RTF-9.

Record (read the doc, not a paraphrase): `docs/rt_service_conventions.md` (§1-5 frozen conventions, §6 D1-D10 +
knobs + the four load refusals, §7 Cards 1-8 with the field-forced corrections, §8 Card-8 runbook and
2026-07-05 results incl. the 05:00 UTC run-window trap, §9 RTF-3, §10 RTF-4..9, §12 owed items + meta-rules);
`docs/rt_service_operations.md` (health CSV, `H` heartbeat, emission-guard tiers, restart / WER supervision,
trace rotation). Correction 2026-10-07: lessons 1-4 below, once memory-only, are now in that doc (§6.2 the
`iode{0}` to `iode{-1}` sweep, §7 Corrections #1 and #2, Card 6 deviation #2).

Landed knobs in no doc:
- RT0-5 `network.clock_runaway_check` + `clock_runaway_mag_m` (1000) + `clock_runaway_rate_m_s` (10): per-const
  median |c0 - global median| and median |c1| ceilings drop that constellation from the epoch's emission
  (health CSV `n_runaway_epochs`).
- RTB-4 `realtime.use_precise_orbits` (default false), getter `realtimeUsesLiveOrbitSource()`: ULT orbits +
  network clocks; default makes the orbit gate equal the clock gate, byte-identical.

Open residuals the docs do not carry:
- The armed `service_mode` smoke then 1 h data run is the highest-value validation hour (the author's standing call).
- CsvRotator (as of 2026-08-15): a persistent `.1` LOCK still erodes one rolled generation per retry cycle (delete
  and shift precede the `.1` rename); a stranded `.rotating` is discarded, not recovered (operations §5 "no rows
  are lost" covers only the live-file retry). Both lock branches still have zero test coverage on 2026-10-07
  (`tests/test_network_product_io.cpp` covers only the success path).
- RTA-10 H-record gaps: solve_failed / sync_skip / datum_failed epochs write no H; `partial` lumps RT0-5
  runaway prunes with σ̂0 carve-outs; old readers flag `healthy()=false` on H files.

Lessons paid for in field time:
1. 0 is a VALID IODE: unknown must be -1 (permissive); an unset `iode{0}` dropped every live network clock.
2. LiveEphemerisStore keeps current + previous per sat: feed BRDC progressively sorted by GPS-scale toc (BDS
   +1356 w +14 s); a 23:00-only test hid a total 05:00 outage.
3. brdc_rts refuses ANY precise product (`getOrbitCount()>0 || getClockCount()>0`); CLK without SP3 splits
   the orbit/clock frame silently.
4. The network is per-epoch GLS: an eph-cutover re-level shifts the injected PRIOR before the solve (a post-solve
   shift over-corrects); advance the baseline AT the shift or a withheld roll double-shifts.
5. DF407 MSM lock time n = 512..703 are valid long locks; saturate at 2^26 ms for n >= 704 (a 0.0 read as a
   reset gave perpetual spurious LLI on the healthiest sats).
6. ARP / position blunder gates key on `(position - initial_position_).norm() > 1e3`, never `position.norm()`
   (the pole fallback passes the naive form and rejects the first 1005/1006 seed).
7. Signal-key prefix matching must reject combined IF keys (guard on `'_'`): `findBandSibling` matched
   `C1W_C5X` to `C1C`.
8. `network.service_mode` bundle-wins overrides an explicit `false`; within-epoch RAIM without base `raim` +
   `phase_leveling` breaks the σ̂0-EWMA to URA chain; validation clamps that run before arming are bypassed by
   armed values, so re-run them post-arm.
9. ULT orbits are `02D` with 6-hourly starts (05M COD, 15M IGS); `01D_05M` always 404s; the ULT-ERP long name
   is unverifiable, so route ULT to RAP ERP.
10. About 60 new lines in ApplicationNetwork.cpp tripped MSVC C1128: the TU needs a `/bigobj` entry.
11. CSV rotation: temp-stage live to `.rotating` FIRST, then to `.1`, with rollback (shift-before-rename strands
    rows when `.1` is locked).
12. Haiku discovery agents were unreliable here (4/9 broke the schema, 3/5 proposals wrong, two would have lost
    data): hand-verify every agent-proposed edit.

memory note `project_closedloop_consumer_vertical_bug` (Card-8 criterion 4: the ~2.8 m vertical float / 0 % fix is
mode-independent; 3-station reproducer), memory note `feedback_wer_zombie_wedge`, memory note `project_network_atmosphere_readiness`.

## 2026-10-24: constant-velocity slant ionosphere block [I, Idot] (default off, committed 2026-07-02)

These results were recorded only in the agent memory note `project_cv_slant_iono` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

Status note, 2026-10-24: the diagonal `network.carry_iono` key named below was deleted by card RKD; its FALSIFIED verdict stands.

User-side PPP-RTK per-sat slant iono as a constant-velocity block `[I_s, Idot_s]` (F level-rate = dt, CWNA Q
`q*[[dt^3/3, dt^2/2],[dt^2/2, dt]]`, the clock CWNA pattern). `estimation.enable_iono_rate` /
`enableIonoRate()`, default FALSE, OFF path byte-identical, uncombined PPP-RTK only. Committed (`f44b3c6`, 2026-07-02).

- PSD fitted EMPIRICALLY from second differences of a 24 h GPS+Galileo `ionosphere_trace.csv`:
  `process_noise.iono_rate_psd = 1e-10 m^2/s^3` at zenith, scaled `/sin^2 el` like the level;
  `initial_std.ionosphere_rate = 0.1 m/s`. Reproduces slant σ(D²I) 8 mm at 12° to 2 mm at zenith.
- `adaptive_iono_process_noise` inflates the RATE PSD as a ratio (override / static level q): no unit mixing.
- A/B smoke (1 h GCGO GPS+Gal BSSD, RW vs CV): level σ 6.4 % tighter at 0-15° el, 3.5 % at 15-30°, equal high;
  |rate| median 0.55 mm/s; 3D tail-30 0.177 vs 0.181 m. Modest because the config carries GIM weighting + tight RW
  `ionosphere_q=1e-3`; 3 h / 24 h runs still owed (memory note `feedback_test_cadence_smoke_first`). ZWD stays RW.

⚠ Multi-row state blocks: `removeStateAtIndex` sizes the shrunk matrix from `getStateSize()` (map counts), so
erase each row's map entry immediately after removing that row (removing both rows first gave an Eigen OOB
abort). Also: the Smoother snapshot must carry the rate map or `F_CC` drops the coupling, and every reinflate
site must reinflate BOTH rows (the iono value is kept across slips).

OPEN: the author wanted CV for the NETWORK. Settled mapping: NOT for the sun-fixed SH-VTEC (quasi-static by
design, RW correct; memory note `project_global_sh_iono`); NOT for a per-epoch code-snapshot LSQ slant path (no temporal
state). It applies to a CARRIED per-station slant-STEC Kalman state, i.e. the unbuilt atmosphere product (RTF-8,
`docs/rt_service_conventions.md` §10.5; memory note `project_network_atmosphere_readiness`). Portable pieces: the CWNA
2x2 block, the second-difference PSD fit, pivot/frame handling into the S-basis. Build on the STEC producer,
do not port this state plumbing; the user-side implementation stands on its own (keep it).
Context as of 2026-10-07 (ledger rows in `docs/CONFIG_promotion_ledger.md`): no carried-iono vehicle is armed.
`network.carry_iono` (diagonal) is FALSIFIED x4 (queue 8b); `network.carry_iono_in_block` (per-LOS column in the
F3 block, 2026-09-10) failed its slipped-sim consumer gate (p50 +47 %, integers -26 % med|e|) and was NEUTRAL on
the quiet bed, which showed the four falsifications were the diagonal row's station-common over-count, not
carrying iono as such. It needs F3, which the re-founding cut; the goldens run free per-LOS slant iono (no SH).

## 2026-10-24: real-time run report graded against the surveyed reference: decisions and the covl145 numbers

These results were recorded only in the agent memory note `project_rt_report_surveyed_reference` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

The real-time run report (built 2026-08-30, committed in `bd6fbaa`): `pyviz/gnssviz/viewers/rt_scorecard.py`
grades `state_trace` against the SURVEYED `receiver.reference_position_ecef` (ENU + 3D error, log
convergence against 3·σ3D, horizontal scatter with 50/95 % circles, steady-state CDFs; `rt_banner_html()`
is the banner and chip row). Run with `pyviz/run_report.py --run-config <cfg> [--ref-ecef X Y Z]`.
Formulation: `docs/report/realtime_engine.tex` §`sec:rt-assess`. Tests: `pyviz/tests/test_rt_scorecard.py`.

Decisions (do not re-litigate):
- Convergence is SUFFIX-STABLE: `k* = min{k : max_{j>=k} err3d_j <= θ}`. First crossing measures the noise, not
  the solution. Non-finite epochs break the hold. Ladder 1.0/0.5/0.2/0.1 m; steady-state statistics run from
  the coarsest achieved rung and the summary names it.
- σ3D = sqrt(tr P^xx) (rotation-invariant; never build a per-axis horizon σ from a diagonal-only trace).
- Explicit `--ref-ecef` beats the config block. No reference: viewer skipped, banner `""`, every existing
  report byte-unchanged.
- `RT_DASHBOARD_OFF` is a SIZE rule, not a judgement: at 1 Hz each per-(sat, signal) slip/residual trace is
  24-36 MB of CSV (report 72.8 MB to 35.5 MB curated). `--all` / `--only` override.
- `pyviz/gnssviz/runconfig.py` strips jsonc with `strip_json_comments` (ported from
  `scripts/gnss_config_chain.py`); a regex cannot, because config paths and URLs contain `//`.

Traps:
- The Browser pane refuses `file://`: serve the report with `python -m http.server` on 127.0.0.1.
- plotly backend: `Scatter.c` was ignored (flat colour) until mirrored from the `PolarScatter` branch on
  2026-08-30; a coloured-Scatter viewer that looks monochrome under `--backend plotly` hits this class.
- VLine labels are top-anchored annotations in both backends: at 1 Hz label only the finest achieved rung.
- A renamed exe copy must sit beside its DLLs or it dies 0xC0000135: memory note `feedback_concurrent_build_locks`.

covl145 replay (1 h, 3600 epochs at 1 Hz, GPS+Galileo, BSSD, COD FIN): within 1 m in 51 s, 0.5 m in 2.8 min,
never held 0.2 m. Steady state from the 1 m rung, bias/RMS: E -15.3/16.9, N +15.7/16.4, U +17.6/19.2 cm;
H p50/p95 20.9/32.1 cm; 3D p50/p95 26.5/45.4 cm. Wall 61 s for the hour (about 60x real time), so replay
convergence is not a demo problem; a live rooftop session is where a warm start would pay.

Warm start:
- The position half already exists: `ppp_model_->initializeState()` seeds from `reference_position_ecef`
  (log "Using user-supplied reference position"), so the 2.8 min is ambiguity / ZWD / bias convergence.
- RT filter-state snapshot (`realtime.state_snapshot_path` + `_restore_scope`, `RtStateSnapshot.{hpp,cpp}`,
  `RealTimeEngineStateSnap.cpp`, `8da1de6`, 2026-09-01): ZERO-WIN on the sim co-restart venue (default scope
  `core` is cold to mm; `full` is KEEP_OFF there, its arm walked U to -9 m). Its three invariant-#1/#7 responses (pending ambiguity reseed, ZWD variance freed,
  `reinflateSystemTimeOffsetVariance`) were each isolated by an arm.
- Card CC (2026-09-14, event-harness closed-loop consumer): scope `full` + `state_snapshot_seam_refloat: false`
  resumed at 0.63 m at k = 1 (max 1.67 m over 60 epochs) and became the harness leg default; `core` tracked the
  cold trajectory (14.4 m at k = 1); `full` with the refloat ON re-opens every ambiguity (6.9 m at k = 11).
- ⚠ OWED as of 2026-10-07 (ledger row `realtime.state_snapshot_path`): the covl145-class A/B with scope
  `full`, the venue with persistent ambiguity/bias memory and a product stream continuous across the consumer
  restart (snapshot mid-replay, restart, compare against the 2.8 min re-convergence). Quote no benefit before it.

Record: `docs/CONFIG_promotion_ledger.md` (row `realtime.state_snapshot_path`),
`docs/cards/consumer/CC-declared-step-carry.md`, `docs/cards/sim-bed/SIMNET-DASHBOARD.md`.
memory note `project_concurrent_run_log_collision`.
