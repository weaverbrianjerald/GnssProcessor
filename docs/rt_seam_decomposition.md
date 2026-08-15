# RealTimeEngine seam decomposition — record and gate report

Cut 2026-08-01.  `RealTimeEngine::processEpoch` was one ~1,440-line epoch-loop body accreting SSR sync, ephemeris
reaction, measurement build, filter update, AR and emit — structurally what `Application::runNetwork` looked like
before INC-0..12.  The RTF-4..9 service queue (`docs/rt_service_conventions.md` §10) will feed the same body
further, so the cut was taken first; the backlog's #6 `IByteStreamSource` port covers the transport side only.

**Provenance.** The frozen golden gate 1 verifies against lives under
`D:\GnssProcessor_archive\2026-08-12\runs\rtseam_goldens`, not in the working tree (`runs/` was archived off C: on
2026-08-12).  `scripts/rt_seam_verify.py` cannot re-run from a clean tree without pointing at it.

## Result

| | before | after |
|---|---|---|
| `RealTimeEngine.cpp` | 2,366 | 625 |
| `processEpoch` | 1,438 | 135 |
| `initialize` | 343 | 19 |
| `feedRtcmFrame` | 256 | 78 |

30 seams across 8 new TUs: `RealTimeEngine{Admit,SsrDiag,Prepare,SlipQc,SsrBreaks,Estimate,Init,Ingest}.cpp`.
Placement rules live in CLAUDE.md ("Real-time engine — where new code goes").

## Why member functions, not a context struct of the network kind

`Application::runNetwork` kept its state in function locals, forcing `NetworkRunContext` and the 40-47-param
verbatim seams.  Every persistent value in `RealTimeEngine` is already a private member, so a **private member
function** reaches `state_manager_` / `config_` / `ssrStore()` with zero plumbing.  Only eleven locals ever crossed
a block boundary; those travel in `RtEpochContext`, a stack-local owning nothing that outlives the epoch.

> **Hazard.** A value that must survive to the next epoch is a MEMBER.  Moving one into `RtEpochContext` compiles
> and silently cold-starts it every epoch — the RT analogue of the network's Tier-1 carry two-place rule.

## Why this cut was safer than `NetworkSolveGlue`

Every early exit in the pre-cut `processEpoch` was `return false`, all nine sat in the front half, and there was
exactly one `return true` (the last line).  So **26 seams are `void`**, moved with *zero* control-flow
transformation, and **4 seams are `bool`** (the admission gates) whose bodies are still verbatim — the entire
transformation is an appended `return true`, because the existing `return false` statements keep their exact meaning
under `if (!seam(ctx)) return false`.  `NetworkSolveGlue` by contrast had to convert 4 epoch-skip `continue`s into
an 18-field result struct — the network decomposition's one non-verbatim cut.  Nothing of that class was needed here.

## Gate 1 — verbatim relocation (PRIMARY): 30/30

`python scripts/rt_seam_verify.py`.  Per seam: take the relocated body out of its new `.cpp` by brace matching, drop
the declared alias preamble and trailing statement, apply the inverse of any declared substitution, restore the
measured dedent, and assert a **character-exact** match against
`runs/rtseam_goldens/pre/src/RealTimeEngine.cpp.frozen` (captured before any edit, sha256
`9fb3dc00883fbb0e37fbc35ecbca2e7e466ffe098bf212ef3e93e6c1c16944b7`; now under the archive root above).  PRIMARY
**because it is the only gate covering 100% of the moved text, including branches no fixture executes** — 1,058
relocated lines in the `processEpoch` batch, 407 in the secondary batch.

Exactly two declared substitutions exist, both in `stepTimeAndDetectGap`, where two driver-local *declarations*
became context writes:

```
const GnssTime aiding_t_prev = prev_epoch_time_;  ->  ctx.aiding_t_prev = prev_epoch_time_;
const bool     aiding_first  = first_epoch_;      ->  ctx.aiding_first  = first_epoch_;
```

Everything else is verbatim with an EMPTY substitution map — what the alias-preamble contract buys.

## Gate 2 — same-build byte-identity A/B (CORROBORATING): 4/4 PASS

```bash
Copy-Item build\x64-Release\gnss_processor.exe build\x64-Release\gnss_rtseam.exe
scripts\rt_seam_gate.ps1 -Exe build\x64-Release\gnss_rtseam.exe -Label cut2
python scripts/rt_seam_compare.py pre cut2
```

| arm | fixture | what it uniquely arms |
|---|---|---|
| `sbf` | `sbf_capture.sbf`, PPP, 76 ep | backlog warn, max-duration latch, WLS clock seed, MW slip path |
| `sbf_bssd` | same + BSSD | `regaugeBssd` (the `seedClockFromWls` twin), `dropBroadcastGrade` |
| `sbf_te` | same + TurboEdit + bridge-LLI | the TurboEdit / soft-LLI two thirds of `detectAndRepairSlips` |
| `cl` | closed-loop network consumer, 2024-129 05:00-06:00, 3 sta + CIT100USA, 121 ep | every SSR seam, `traceSsrConsumer`, ZWD constraint, network iono prior, real AR |

Byte-identical across every numeric artifact, including `network_solution.csv` (546 KB),
`satellite_status_trace.csv` (177 KB), `ssr_consumer_trace.csv` (49 KB), `measurement_residuals.csv` (89 KB),
`closed_loop_trace.csv`, `state_trace.csv`, `dop_trace.csv`.

**Maskspec** (wall-clock-derived only; ported from the network's frozen INC-0 maskspec where it overlaps).  CSV
masking is **column-scoped by header name** — blanket-masking a file would hide a real regression in a neighbouring
column.  Logs: spdlog `[%E]` prefix; `lag_s`; `frame rx→process`; `held N/Ns`; `hold expired after Ns`;
`ms/epoch`; `N ms wall`; `epoch-loop wall N ms`.  `network_service_health.csv`: columns `ms_per_epoch_window`,
`last_emit_age_s`.  `network_run_manifest.json`: keys `written_utc`, `compiled`, `loop_ms`, `ms_per_epoch`.

**Determinism pins in the arm configs** (an unpinned run reds falsely): `enable_obs_skip_to_latest=false` — it reads
`nowContinuousGpsSeconds()`, so on a historical replay it drops *every* epoch and the arm compares two empty runs;
`enable_obs_ssr_sync=false` — the hold age is measured against wall clock.  `scripts/rt_seam_gate.ps1` takes `-Exe`
as a **required** parameter with no default, deliberately: `scripts/run_inc4_gate.ps1:5` hardcodes
`gnss_processor_dcmp.exe` with no override, which under the renamed-exe directive yields a stale-binary false green.

## Gate 3 — unit suite

**1044 passed / 1 skipped** (`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`, the known bootstrap-gated fixture) —
**the 2026-08-01 gate-time measurement, and the evidence for this specific cut.**  Not the current suite size:
CLAUDE.md re-counted it at 1360 cases / 196 suites / 89 files on 2026-08-12.  Clean Debug and Release builds; no new
warnings beyond the pre-existing C4828 codepage class.

## ⚠ What gate 2 does NOT certify (invariant #4, stated rather than glossed)

The A/B certifies only what a fixture executes.  Measured on the `cl` arm:

- **0** phase-bias, **0** code-bias and **0** clock discontinuities, **0** producer-status transitions over 121
  epochs.  All five `RealTimeEngineSsrBreaks.cpp` seams therefore ran their *traversal* (per-observation /
  per-signal loops, guards, raw/combined-key filters) but **not their reaction bodies** — no `resetAmbiguity`, no
  `reinflateCodeBiasVariance`, no `reinflateSystemTimeOffsetVariance` was driven.  A clean 1-hour window with a
  stable satellite set produces no datum steps.
- `sbf_te` came out byte-identical to `sbf`.  TurboEdit **is** constructed and `resolve()` **is** called (confirmed
  in the log), but on this clean 76-epoch fixture it returns empty sets, so `te.repairs` / `reset_sats` /
  `code_events` / `moot_signals` / `pending_sats` all iterate zero times.
- `reinflateOnCodeBiasDisc`'s **else** branch (the `estimate_ifb=false` warn-once path) is executed by no arm — arm
  `cl` sets `estimate_ifb=true` to reach the larger branch.

Acceptable for a pure relocation *because gate 1 covers those lines mechanically* — but **owed debt the moment
anyone edits a seam body rather than moving it.**  Closing it needs SSR content that actually steps a datum (a
synthesized producer restart / pivot handover, or a window chosen for rise-set churn): a card in its own right.

## Reusable mechanic

`scripts/rt_seamcut.py` holds ONE canonical seam table generating both the `.cpp` bodies and the verifier manifest,
so the two cannot disagree about what moved.  Run `--survey` first: it prints each block's measured dedent and
first/last line, and a range crossing a scope boundary shows up as a min-indent below 4 and is refused.

Traps that fired during this cut, recorded so they are not re-paid:

1. The generator emits `"    " + original[dedent:]`; the verifier must strip that 4-space function indent *before*
   restoring the dedent.  Getting this wrong reports all 20 seams as failures, uniformly off by exactly 4 — a
   signature worth recognizing, since it looks like catastrophe and is a harness bug.
2. Declared `subs` are applied by the generator with plain `str.replace`, so the verifier's inverse must also be a
   plain replace.  Treating them as identifier renames (`\b`-anchored regex) silently fails to invert
   whole-statement subs.
3. The frozen-source directory under the golden root gets picked up as a run arm unless excluded.
