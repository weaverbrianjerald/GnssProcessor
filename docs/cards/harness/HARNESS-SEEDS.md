# harness seeds: seed hygiene, the ZWD seam reseed and the fingerprint rule

**STATUS**

- **id:** harness seeds
- **topic:** harness
- **status:** reference
- **summary:** Reference: every harness leg arms `network.zwd_seam_reseed`, and each fingerprinted promotion forces a seed re-write; the `_v3` seeds replaced `_v2`, whose snapshots the promoted golden refuses.
- **dates in the text:** 2026-09-11 to 2026-09-12
- **knobs / posture:** `network.zwd_seam_reseed`, `network.scoped_handover_response`, `network.handover_stransform` (both fingerprinted); seed bases `golden_sim_tri_w6` and the campaign `zwdblk` configs
- **hashes named in the text** (commit / exe / byte-cert): `e90086f` (main at the `_v2` exe `gnss_processor_seedv2.exe`, 3FDC5F8D…); seed fingerprints `c5b5df834d185ac8` and `ce8f64cbd3f77ace` (`_v2`), `d76a3d0709aeeaf2` and `542af66dcc9a7198` (`_v3`, exe `gnss_processor_stspromo.exe`, C2F2B297…)
- **run dirs named in the text:** `runs/evleg_zwdseam_null`, `runs/evseed_quiet6h_v2`, `runs/simnet60_quiet_wu93_24h`, `runs/evseed_slipped6h_v2`, `runs/simnet60_campaign_wu93`, `runs/evseed_quiet6h`, `runs/evseed_quiet6h_v3`, `runs/evseed_slipped6h_v3`
- **source:** `docs/record/archive/SPEC_event_harness.md` §18 (18.1, 18.2)

**Outcome.** A standing reference for seed hygiene. Every leg before 2026-09-11 (evening) restarted the ZWD channel at the seam: the null leg's ZWD truth rms read 0.061 to 0.081 m against the seed's 0.0076 m, and 0.0108 m with the reseed armed. Event-minus-null differences are unaffected; absolute ZWD levels read off old legs are superseded. A seed is paired with the exe and golden it was written under. `golden_sim_tri_w6`, the base of the `_v3` seeds, was RETIRED on 2026-09-15 ([RF-3-prep](../../record/refound/RF-3-prep_golden_rebase_2026-09-15.md)).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 18. Harness hygiene — every leg before 2026-09-11 (evening) RESTARTED the ZWD channel at the seam (`zwd_seam_reseed` was never armed by `event_leg.py`)

Found by the ORACLE PIN card (`docs/SPEC_oracle_pin.md` §12): the null leg's ZWD truth rms sits at **0.061–0.081 m for the whole 60-epoch leg** against the seed's own converged **0.0076 m** at the snapshot epoch — the resume restores the ambiguity carries, the frame and the coupling (leg E of the seam card) but not the ZWD, which restarts from its a-priori and does not recover within 60 epochs.  The ledger already carries the remedy: `network.zwd_seam_reseed` (RECOMMEND_SCOPED, regime warm + phase_leveling, "arm on the RESUME leg") restores the snapshot's Z rows as B9-style priors at the seam.  Armed on the same seed/snapshot/exe the null reads **0.0080 m at +1..+5 and 0.0108 m over the leg** (`runs/evleg_zwdseam_null`), i.e. the seed's converged level.

**Applied:** `scripts/event_leg.py` arms `network.zwd_seam_reseed` on every `make-leg` / `null` leg from this commit on; `--no-zwd-seam-reseed` reproduces the old posture for a like-for-like A/B against an old leg.  **What it changes in the record:** event-minus-null DIFFERENCE readings (the `zwd` channel of `event_grade.py`, every Study 1–7 verdict) are unaffected — both legs of a pair share the restart and it cancels; ABSOLUTE ZWD levels quoted off a leg (the pin card's `none` column, any "leg ZWD rms" figure) were the restart, not the producer, and are superseded.  The consumer channel is NOT affected either way (the cold consumer's profile is identical with and without the reseed).  Legs generated in a worktree branched before this commit (the §15 cold-fallback card's) still run the old posture; their OFF/ON pairs remain valid as differences.

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


---

## 2026-10-24: seeds re-written on the refound goldens (card SMX)

Every earlier seed in this record is on a retired base (`golden_sim_tri_w6`, the campaign configs) or GPS only from before card GAD; card [SMX](../../refound/SMX_scenario_matrix_2026-10-24.md) re-wrote them on the current goldens and the clock-level postures of card SCB. Writer: `python scripts/scenario_matrix.py seed <posture>` (through `scripts/event_leg.py write-seed`, whose `DEFAULT_GOLDEN` is now `config/golden/golden_sim_refound.json`), 6 h, a snapshot every 1800 s, exe `gnss_processor_smx_main.exe` (the Release build of main `7bdcebc`, sha256[:16] `10426f27f2e231b8`), goldens at sha256[:16] `887b2dcf84079149` (sim) and `6db4dc5b3e61533b` (field). Each seed passed a 10-epoch smoke first (`evseed_smx_<posture>_smoke`). All under `D:/GnssProcessorSupport/runs_nom/`.

| seed | posture | fixture, start | emitted | wall (s) | fingerprint (`ep0485`) | snapshots |
|---|---|---|---|---|---|---|
| `evseed_smx_g` | the sim golden as armed (GPS + Galileo) | `runs/simnet60_quiet_col_24h`, 00:30:15 | 720 | 1475 | `212dada761590f60` | ep0001, 0062, 0122, 0182, 0243, 0304, 0364, 0424, 0485, 0545, 0605, 0666 |
| `evseed_smx_c` | GPS only + the self-contained regime of card SCL2B | same | 720 | 854 | `4efce5196f2f002d` | same suffixes |
| `evseed_smx_n` | `c` + the native broadcast-clock a priori of card NBP | same, data root `runs/_scl_data_nbp` | 720 | 758 | `63599558801a208f` | same suffixes |
| `evseed_smx_gx` | the sim golden + `network.sat_health` "exclude" (the door removes G01, E14 and E18 for the broadcast's own flags) | same | 720 | 551 | `56eee3dfe8a3f433` | same suffixes |
| `evseed_smx_f` | the field golden as armed (58 stations, held-out `WTZZ00DEU`) | `data/2024-129`, 00:00:00 | 720 | 1216 | `8041b7d719d81620` (`ep0484`, TOW 273690) | ep0001, 0061, 0122, 0182, 0242, 0303, 0363, 0423, 0484, 0544, 0604, 0665 |

Snapshot `ep0485` (TOW 275580, 4 h after the start) is the one the matrix resumes. The snapshot suffix is the emitted count at each 1800 s write, so the same index carries the same tow on every posture of one fixture.

**Two loader traps, both handled by the tools now.** (1) A leg arms `resume_rxcb_lin_restore`, which the loader refuses under `rx_code_bias_datum` (armed in both refound goldens): `event_leg.py` arms the restore only where the datum is off. (2) A fingerprinted knob (for example `network.sat_health`) cannot be added on a warm leg: it needs its own seed, hence `evseed_smx_gx`.

---

## 2026-11-06: the SMX seeds re-written with the hold and latch captures (card CRS2)

Card [CRS2](../../refound/CRS2_resume_hold_and_latches_2026-11-06.md) re-wrote the three sim seeds of card SMX with `network.state_snapshot_hold_capture` and `realtime.state_snapshot_latch_capture` (both armed by `scripts/event_leg.py write-seed` from this card on), so a leg can restore the emit-side phase-bias hold and the consumer's seed latches. Writer: `python scripts/scenario_matrix.py seed <posture>` as on 2026-10-24, exe `gnss_processor_crs2.exe` (the Release build of branch `crs/resume-fixes` on main `284ec29`, sha256[:16] `49e5271dfc4f9e3e`), goldens unchanged. The 2026-10-24 seeds stay on disk as `evseed_smx_<posture>_pre_crs2` for like-for-like re-grades of the legs already on record. All under `D:/GnssProcessorSupport/runs_nom/`.

| seed | posture | emitted | wall (s) | fingerprint (`ep0485`) | `ep0485` hold rows (PH / PHM / PHC / PHF) | consumer `I` rows |
|---|---|---|---|---|---|---|
| `evseed_smx_g` | the sim golden as armed (GPS + Galileo) | 720 | 2759 | `212dada761590f60` | 114 / 2 / 57 / 57 (10 keys in generation 2) | isb 1, phase_clock 0, code_bias 1, clock_seed 1 |
| `evseed_smx_c` | GPS only + the self-contained regime of card SCL2B | 720 | 2309 | `4efce5196f2f002d` | 64 / 1 / 32 / 32 (6 in generation 2, all 64 on a live clone) | same |
| `evseed_smx_n` | `c` + the native broadcast-clock a priori of card NBP | 720 | 2225 | `63599558801a208f` | 62 / 1 / 31 / 31 (6 in generation 2, all on a live clone) | same |

`products.txt`, `closed_loop_trace.csv` and `network_solution.csv` of each seed hash-equal the 2026-10-24 seed's (the captures are write side only); `network_events.csv` differs by the `BLOCKBAARDA` tier card BOF armed on 2026-10-28 (and one `PRIORRES` row on `c`), which card SMX's exe predates. The fingerprints are unchanged, so the snapshot suffixes and tows of the 2026-10-24 table apply. `evseed_smx_gx` was re-written the same way (720 emitted, 1738 s, fingerprint `56eee3dfe8a3f433`, 108 / 2 / 54 / 54 hold rows at `ep0485`, `products.txt` `f7c2603a73e7420f` identical to its 10-24 copy `evseed_smx_gx_pre_crs2`); `evseed_smx_f` was not.
