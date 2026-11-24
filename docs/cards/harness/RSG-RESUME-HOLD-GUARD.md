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

## 2026-11-24: step (b'), registered before its first run (on the managing session's word of 02:47)

**What is built.** When the station memory restore is armed under accumulated wind-up and its section is absent or refused at a resume, every station's model records its first wind-up seed of each satellite in its restart ledger (`GnssCorrections::setSeedAsRestart`, entry with gap 0 and `resume_seed` set), and the existing restart response (`network.cycle_slip.windup_restart_refloat`, `networkSlipDrainWindupRestart`) refloats the arcs' carries as it does for every restart: one rule for one event, not a second mechanism. The resumed run holds no accumulator, so that seed stands where the uninterrupted run continued its unwrapped angle, the step of a restart. The holds of (b) are released as before. The clocks, biases, ionosphere and wet delays stay restored. Counted (`resume_seed_restart_stations`, `resume_seed_amb_carries`; the response's `wu_restart_resume_seeds` and `wu_restart_resume_arcs`), declared by one `RESUME,seed_restart_arm` event at the first resumed epoch, each refloat by the response's own `T0_windup` row (detail "resume seed without the accumulators"), and a run-end line. No new key.

**Readings, fixed before the first run:**

1. The simulator leg of reading 3 (station memory armed, its section absent), on the (b') engine: the frame-free audit against the fixture truth shows 0 off from the seam on, attributed and not attributable alike. Counts: the carries refloated equal the ambiguity carries restored (a carry not refloated is listed by reason: its satellite not in view at a present station before it left the carry horizon), holds released 681.
2. What the resume still gains: the held share by hour and the built-in user by hour of that leg, against a cold start at the same epoch and against the full warm start with the station memory (three columns).
3. Identity with every warm-start key absent on both reference files (2 h, against `main`'s engine), and the loader refusal of (a) unchanged.
4. The unit cases of (b'): a first seed enters the ledger only when set, once per satellite; the guard sets the rule only when the section is absent or refused under accumulated wind-up.

If reading 1 misses again, the fallback (b'') is stated before it is built: such a resume is refused under `state_snapshot_require` and cold-starts otherwise, with a counter and an event.

## 2026-11-24: step (b') read: reading 1 missed on two seeds

Commit `17add12`, engine `gnss_processor_rsg2.exe` (sha256[:16] 6a4d738d36c7d4c2). Every run exit 0.

- **Reading 3, met.** `runs_nom/_rap_rsg_g2_{field,sim}` against `_rap_rsg_m_*`: `products.txt` identical, and the 7 differing traces differ only in run times, paths, the build stamp and wall-time columns. The loader refusal of (a) is unchanged (unit case).
- **Reading 4, met.** `ResumeSeedRestart.*`, 2 cases; filtered suites 160 run, 147 passed, 13 designed skips, 0 failed.
- **Reading 1, missed twice.**
  - On the seed of `evleg_wr_s_L` (`runs_nom/evleg_rsg_bp_leg`, section absent): 681 holds released, 60 stations armed, 1,526 ambiguity carries restored, and 1,415 resume seeds refloated 1,440 arcs. The frame-free audit against the truth still finds 1,034 attributed and 526 not attributable link-epochs off, on 49 links (with the holds released alone: 2,914 on 122).
  - That seed was written on 2026-11-22. Its "full warm" column (`evleg_rsg_bp_warm`, from `evseed_wr1_s`) had its station memory refused too (format v1) and ran the same path, giving the same counts. So the three columns were repeated from `main`'s own seed `evseed_wrc0_s4` at 03:00, with the station memory section refused by a changed fingerprint (`evleg_rsg_c0leg`):
    - 697 holds released, and 1,466 resume seeds refloated 1,446 arcs;
    - off: 3,480 attributed and 1,157 not attributable link-epochs on 76 links, from 03:17 on (E11 at five stations, G02 and G14 among them).
- **Reading 2, the three columns from `evseed_wrc0_s4`** (held share GPS / Galileo, built-in user 3D median by hour from the seam; frame-free audit over the leg):

| hour | degraded resume (b') | full warm start | cold start at the same epoch |
|---|---|---|---|
| 1 | 37.1 / 41.7 %, 0.552 m | 90.2 / 91.1 %, 0.064 m | 26.6 / 22.4 %, 0.809 m |
| 2 | 70.0 / 80.1 %, 0.054 m | 92.1 / 90.6 %, 0.017 m | 90.3 / 89.3 %, 0.154 m |
| 3 | 69.1 / 83.4 %, 0.035 m | 90.2 / 92.2 %, 0.022 m | 89.9 / 92.1 %, 0.039 m |
| 4 | 71.0 / 77.9 %, 0.024 m | 88.1 / 86.8 %, 0.013 m | 82.5 / 86.8 %, 0.028 m |
| off against the truth | 4,637 link-epochs on 76 links | 0 of 252,017 | 0 of 192,777 |

**Reading.** The degraded resume keeps a quicker first hour than a cold start, but from the second hour it holds fewer links than the cold start and holds integers the truth contradicts.

The refloat reaches the ambiguity carries. The off links are decided after the gate's window, and several share one satellite (E11 at five stations), which points to a satellite-side or tree state restored in the writer's frame that the refloat does not reach (the chain's restored tree pins and pinned lives, the emitter's held values, the satellite phase-bias carries). This is not shown.

**The fallback (b''), stated before it is built:** when the station memory restore is armed under accumulated wind-up and its section is absent or refused, the resume is refused under `network.state_snapshot_require` (a run error before the first epoch, as for every other refusal), and otherwise the run cold-starts, with a counter and a RESUME event. The release of (b) and the seed rule of (b') would then no longer be reached and are withdrawn.

## 2026-11-24: step (b''), registered before its first run (on the managing session's word of that morning)

Steps (b) and (b') are withdrawn: their misses stand as recorded above (the release alone left 2,914 link-epochs off on 122 links; the refloat left 1,560 and 4,637 off on two seeds and held fewer links than a cold start from the second hour). The code of (b') is reverted (`36fbb78`) and that of (b) removed with this step.

**What is built.**
- When `network.resume_station_memory_restore` is armed under `estimation.accumulate_phase_windup` and the snapshot's station memory section is absent or refused (version or fingerprint), the warm resume is refused whole in its gate, before anything is restored (`resumeCheckStationMemory`, network/state/WarmResume.cpp; the rule in `networkStationMemoryResumeRefusal`, network/state/StationMemory.hpp).
- Under `network.state_snapshot_require` the run stops before its first epoch with the refusal `WarmResumeRefusal::StationMemory` and its reason. Otherwise it starts cold at that epoch, with the counter `station_memory_cold_starts` (reason code 1 refused, 2 absent), one `RESUME,station_memory_cold_start` event at its first epoch, and a run-end line. Nothing of the half-restored state is used.
- The loader refusal (a) stays. The ledger rows of the two keys take the three-part note.

**Proofs, fixed before the first run:**
1. Identity with every warm-start key absent on both reference files (2 h, 4 threads, the simulator with its start) against `main`'s engine `gnss_processor_int1123k.exe`.
2. The cold start is a cold start. The simulator leg from `evseed_wrc0_s4` at 03:00 with its station memory section refused (the fingerprint changed, as in `evleg_rsg_c0leg`) equals a run started cold at the same epoch with no snapshot at all (`evleg_rsg_c0cold`'s configuration on this engine). The comparison covers products and every trace, apart from the wall-clock fields and the RESUME event and counter. That identity is the bar, and the truth count follows from it (0 off, as `evleg_rsg_c0cold`).
3. Under `network.state_snapshot_require` the same leg does not start: its exit code and message, as a unit case and as one run.
4. The unit cases of (a) and of the refusal. The tools are unaffected: a leg made by `scripts/event_leg.py` (which `scripts/warm_start.py` calls) arms the station memory with its capture and resumes.
