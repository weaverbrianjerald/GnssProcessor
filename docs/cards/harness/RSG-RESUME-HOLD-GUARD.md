# RSG: the resume's hold guard (the chain restore tied to the station memory)

**STATUS**

- **id:** RSG
- **topic:** harness
- **status:** open
- **summary:** Pre-registered 2026-11-23 on the managing session's order of that day, after card WRS's dated result of 2026-11-23: the first simulator leg of card WRS (`evleg_wr_s_L`, before the station memory) held 4,692 link-epochs on 134 links one cycle off on both bands, because the chain restore brought back the ring fix's held integers while every wind-up accumulator restarted at the seam. That combination of keys loads on `main` `f3c82c3`. This card ties the two: a loader refusal, and a release of the restored holds at run time when the station memory section is absent or refused.
- **dates in the text:** 2026-11-23
- **knobs / posture:** `network.resume_chain_restore`, `network.resume_station_memory_restore` (both default off; both off in the reference files, unchanged); no new key.
- **code:** branch `net/resume-guard-2026-11-23` from `main` `f3c82c3`, worktree `D:/GnssProcessorSupport/worktrees/rsg`; reaches `main` only on the author's word.
- **run dirs:** `D:/GnssProcessorSupport/runs_nom/` (`RAPID_RUNS_ROOT`); tools and readings `D:/GnssProcessorSupport/gates/warm_resume/rsg/`.
- **source:** the managing session's order of 2026-11-23 (23:17); card [WRS](WRS-WARM-RESUME-REFERENCE.md), section "the first simulator leg held integers the phase contradicts"; card WRC (branch `net/whole-solve-resume-2026-11-23`), section "the simulator's truth audit after a restart".

**Outcome.** Read 2026-11-24 (commit `fd17676`). (a) The loader refuses `network.resume_chain_restore` under accumulated wind-up without `network.resume_station_memory_restore`; the tools still load. (b) A resume whose station memory section is absent releases every restored ring-fix hold (681 of 681 on the simulator leg, one RESUME event). With every warm-start key absent both reference files are identical to `main`'s engine. The leg's truth bar is missed: the restored float carries stay in the writer's frame, the gate decides the writer's integers again, and 2,914 link-epochs on 122 links stay one turn off (3,708 plus 984 before). The release alone does not clear the state.

> Results are appended TO THIS FILE, below the text, under dated headings.

---

## 1. The state and why it is wrong

Under accumulated wind-up the producer's integers absorb each arc's whole wind-up turns since the arc began. The chain memory section (`network.state_snapshot_chain_capture`, `network.resume_chain_restore`) carries the ring fix's links, held integers included, in the writer's frame. The station memory section (`network.state_snapshot_station_memory_capture`, `network.resume_station_memory_restore`) carries every station's wind-up accumulator. With the first restored and the second not, every accumulator starts again at the raw angle of the resumed run's first epoch, and a restored hold on an arc whose accumulator had passed half a cycle sits one whole turn off. On `main` `f3c82c3` the loader requires only `resume_datum_restore` for the chain restore. The station memory restore, when armed, warns and goes on when its section is absent or refused. The ledger rows of both keys already describe the whole-turn offset. `scripts/event_leg.py` and `scripts/warm_start.py` arm the station memory by default.

## 2. What is built (exactly these two)

- **(a) Loader.** `src/core/config/GnssConfigLoadNetwork.cpp`: `network.resume_chain_restore` with `estimation.accumulate_phase_windup` and without `network.resume_station_memory_restore` is a thrown refusal with its reason.
- **(b) Seam.** `network/state/ResumeHoldGuard.{hpp,cpp}`:
  - `networkResumeHoldGuardRelease` runs in the driver after the chain and station memory restores, before the first epoch. It acts when the run keeps a ring fix, the wind-up is accumulated, the chain restore restored its section and the station memory restore did not.
  - Each held link is un-held as a release at the snapshot's epoch (`carry.resume.snap_s`, set by the warm resume), so the ring fix's own window after a release applies from the seam.
  - Counters: `resume_hold_guard_released` (GPS, Galileo) and the reason code (1 refused, 2 absent, 3 not restored).
  - Declared by the observer `networkResumeHoldGuardSeamEvent` (one `RESUME,hold_guard_release` row at the first resumed epoch) and a run-end line.
- The ledger rows of the two keys take a three-part note. No key is armed anywhere.

## 3. Proofs, registered before the first run

1. **Identity with every warm-start key absent.** Both reference files at 2 h (the simulator with its start, 4 threads), this branch's engine against `main`'s engine `gnss_processor_int1123k.exe` (sha256[:16] a638ca112925ed1e, the sources of `main`'s code): `products.txt` byte-identical, and every trace identical apart from wall-time, path and timing fields.
2. **The loader's refusal** as a unit test (refused under accumulated wind-up without the station memory; accepted with it, and under fractional wind-up). The tools are unaffected: a leg made by `scripts/event_leg.py` (which `scripts/warm_start.py` calls) arms the station memory with the chain restore and loads.
3. **(b) as a unit test** on a chain section written with held links and restored while the station memory section is absent (every restored hold released, counted by constellation, stamped at the snapshot's epoch; nothing released with the station memory restored, without a chain section, or under fractional wind-up). **And one simulator leg in the state of `evleg_wr_s_L`, reached the run-time way:** the leg configuration of `evleg_wr_s_L` with the station memory restore armed, resumed from its own seed `evseed_wr_s`, which carries no station memory section. Bars:
   - the holds are released at the seam, and the release counter equals the holds the chain section restored;
   - the frame-free audit against the fixture truth (`gates/warm_resume/wrc/wrc_truth_frames.py`, branch at the producer's arc start) shows 0 off from the seam on, where `evleg_wr_s_L` shows 3,708 attributed and 984 not attributable link-epochs.

## 2026-11-24: proofs read (commit `fd17676`, engine `gnss_processor_rsg1.exe`, sha256[:16] 56c518cc7fad0bc8)

Every run exit 0 except the first try of the tool leg (exit 1: it was given a snapshot written by the WRC branch, whose sections this branch's parser does not know; the loader had accepted its configuration; rerun on a snapshot of `main`'s engine, exit 0).

1. **Identity, met.** `runs_nom/_rap_rsg_{m,g}_{field,sim}` (2 h, 4 threads, the simulator with its start 0.504167 h; `m` on `gnss_processor_int1123k.exe`, `g` on this branch): `products.txt` byte-identical on both beds. Of 71 (field) and 69 (simulator) trace files, 7 differ: `main_log.log`, `rt_engine.log` and `network_run_manifest.json` in run times, paths and the build stamp; `network_datum_graph.csv`, `network_live.jsonl`, `network_service_health.csv` and `network_run_summary.csv` only in their wall-time columns (checked column by column).
2. **The loader's refusal, met.** Unit case `ResumeHoldGuardConfig.TheChainRestoreUnderAccumulatedWindUpRequiresTheStationMemory`. A leg configuration made by `scripts/event_leg.py` (chain restore with the station memory restore) loads and resumes on this engine: `runs_nom/evleg_rsg_tool_s` (10 epochs from `evseed_wrc0_s4` at 03:00), both sections restored, the guard inert (no event).
3. **(b), the unit cases met; the leg's release met; the leg's truth bar MISSED.** The leg `runs_nom/evleg_rsg_wrs_L` is the configuration of `evleg_wr_s_L` with the station memory restore armed, resumed from the snapshot of `evseed_wr_s`, which carries no station memory section.
   - At the seam the guard released 681 restored holds (GPS 381, Galileo 300), with one `RESUME,hold_guard_release` event. The chain restore reports 1,246 links restored, 681 of them held, so the counter equals the holds restored.
   - The frame-free audit against the fixture truth (`gates/warm_resume/rsg/truth_frames_rsg_wrs_L.txt`) finds 171,206 held link-epochs, of which 2,030 are attributed off and 884 not attributable, on 122 links, from 03:47 on (the old leg: 3,708 and 984 on 134 links).
   - The off links are holds decided again after the release, at the writer's integers. Example: S52000SIM G21 is held again at 272910 with (105, -110), as in the seed; S10000SIM E14 at 273510 with (-66, 98).
   - The restored float carries (the leveling carries of the frame and coupling restores) are in the writer's frame, a whole turn off under the restarted accumulators, so the gate decides the writer's integers again. A release of the holds alone does not clear the state. What would clear it is a decision for the managing session: re-float the restored ambiguity carries as wind-up restarts when the station memory section is absent or refused, or refuse such a resume.
