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

**Outcome.** Pre-registered; not yet read. Two edges guard one state. (a) The loader refuses `network.resume_chain_restore` under `estimation.accumulate_phase_windup` without `network.resume_station_memory_restore`, in the form of the existing "requires" rules. (b) A warm resume that restores the chain section while the station memory section is absent or refused releases every restored ring-fix hold before the first epoch, stamped at the snapshot's epoch, counted by constellation, with one `RESUME,hold_guard_release` event at the first resumed epoch and a run-end line; the links re-enter through the ring fix's ordinary gate.

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
