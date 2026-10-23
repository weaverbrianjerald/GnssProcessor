# EPH-JUMP: re-seed a satellite when its broadcast baseline jumps

**STATUS**

- **id:** EPH-JUMP
- **topic:** realtime
- **status:** closed
- **summary:** BUILT, default OFF: a change of the broadcast record a satellite uses is tested for its modelled-range jump, and above 10 m the satellite's ionosphere, satellite-bias and ambiguity states are re-seeded; byte-identical off and armed-but-quiet, healthy changes stay under 0.83 m over 6 h; arming awaits a graded live jump.
- **dates:** 2026-10-23
- **knobs / posture:** `processing.ephemeris_jump_reseed` (default false), `processing.ephemeris_jump_threshold_m` (default 10 m); real-time engine only (no network-loader key, so no ledger row)
- **code:** `realtime/EphemerisJumpLedger` (the last-used record and the range-jump test), `RealTimeEngine::reactToEphemerisJumps_` and `reseedAfterEphemerisJump_` (`RealTimeEngineSsrBreaks.cpp`), `LiveEphemerisStore::selectedRecord` / `evaluateRecord` / `withinFitWindow`, `Geometry::liveOrbitUsed` and `PppObservation::live_orbit`; tests `tests/test_ephemeris_jump.cpp`
- **run dirs:** `D:\GnssProcessorSupport\runs\rt_e23_diag\ejr_covl_{off,on}` (6 h May replay pair), `ejr_smoke`; gate captures `C:\GnssProcessor\runs\rtseam_goldens\{e23_post,ejr_off}`
- **exe:** `gnss_ejr.exe` md5 `87ccafee9ac7d0588268c0e5e4ec5f33` (Release, D: worktree build of this change)
- **source:** the first follow-up of card [INAV-ODD](INAV-ODD-PAGE.md) section 4

**Outcome.** After E23's record changed on 2026-10-23 its satellite-bias states kept a 266 m error at 0.025 m sigma for half an hour: the engine had no response to a satellite's broadcast baseline moving (standing invariants 1 and 7). The engine now remembers the record each satellite's orbit came from, measures the line-of-sight range change when it changes, and re-seeds the satellite above 10 m (a pivot's partners for a pivot). Default off and byte-identical; armed on a 6 h real replay it saw 377 changes, the largest 0.83 m, and wrote nothing. Not yet graded on a real jump: that needs a recorder or a live event.

> Results are appended TO THIS FILE, below the text.

---

## 1. The defect being answered

In `live_ge_kin` (card INAV-ODD section 1) E23 was admitted on a corrupted receiver record, the filter
absorbed the 270 m range error into its slant ionosphere (-266 m) and satellite-bias states, and at
01:30:36 the record changed to a correct one. Every E23 row stepped by +265.7 m; the phase rows
re-absorbed the step, but the satellite-bias states held the old value at 0.025 m standard deviation,
so every E23 code row for the rest of the session was a large innovation and an outlier (8,036 lines).
Nothing in the engine treats a change of the broadcast record under a satellite as an event.

## 2. Design

**Detection** (`EphemerisJumpLedger`, pure). Each epoch, for each satellite whose orbit this epoch came
from the live broadcast store (`PppObservation::live_orbit`, recorded by `Geometry::getSatellitePV`
only while the key is armed; a satellite modelled from an SP3 is not tested), the engine asks the store
for the record the geometry selects (`LiveEphemerisStore::selectedRecord`: the version the SSR orbit IOD
names when held, else the closest toe, with the same fit-window guard) and its source (IGS stream or
receiver). The ledger compares it with the record the satellite used last. When the orbit or clock
elements differ, both records are evaluated at the current epoch and

    Δρ = u · (p_new − p_old) − c · (dt_new − dt_old),

u the unit line of sight from the receiver. Each change is exactly one of: undecided (the old record is
beyond its fit window, after a gap), below the threshold, or a jump. Source switches between the IGS
stream and the receiver are counted and logged at info (the store's own stall-fallback line is debug).

**Response** (`reactToEphemerisJumps_`, in the SSR-breaks seam after the declared level-step reaction,
before the measurement update). For a jump of satellite s:

- s not a pivot: s is re-seeded.
- s the between-satellite pivot of its constellation: every other satellite of that constellation that
  holds a state is re-seeded, because each single-differenced state carries the pivot's range.

Re-seeding keeps the state values and breaks their covariance: ionosphere (and its rate) and every
satellite-bias variance raised to at least Δρ², and every ambiguity reset at a standard deviation of at
least |Δρ| (pending a geometry seed). The epoch's rows then re-determine what the states absorbed. This
drops the prior, as invariant 1 asks, rather than re-seeding a stiff value from one epoch of code.

Every jump is a warn line (`EPH-JUMP`) naming both records, their sources, Δρ and the re-seeded
satellites; the consumer satellite trace records `eph_jump_reseed` per satellite; the run-end line
reports changes, switches, undecided, jumps, pivot jumps, re-seeds, ambiguity resets, the largest change
and the counter closure.

Limits: the record selection repeats Geometry's rule from the engine's SSR store, which is the store the
geometry reads in a live run; a configuration that routes the orbit SSR to another store could select a
different version at an IOD cutover (healthy cutovers stay far below the threshold). A configuration with
precise orbits and live clocks (`realtime.use_precise_orbits`) is not covered: its satellites are not
tested because their orbit is not from the broadcast store. The ledger is not part of the RT snapshot.

## 3. Verification

**Unit tests** (`tests/test_ephemeris_jump.cpp`, 11 cases): the range jump of a 0.02 m^1/2 sqrt(A) error
is the semi-major-axis change (about 218 m) within 0.2 m from below the satellite, and of a 1 µs clock
change is -c·1 µs; each change takes exactly one outcome (first sighting, identical record and a
same-element source switch report nothing; a 0.3 m clock refresh is below the threshold; the corrupted
receiver record is a jump and a source switch); an old record beyond its window is undecided; the store
reports the selected version and its source for current and previous records and nothing beyond the fit
window; `evaluateRecord` reproduces `computeSatellitePV` and `computeSatelliteClock` exactly. Engine
(friend shim over a real `StateManager`): a jump raises the satellite's ionosphere and satellite-bias
variances above (200 m)² and resets its ambiguity while a second satellite is untouched; a 0.3 m change
is counted and writes nothing; an SP3-modelled satellite is not tested; unarmed, nothing is measured or
written; a pivot's jump targets its constellation partners only. Full Debug suite: 3,602 tests,
0 failed, 1 skipped.

**Seam gate, knob off** (`ejr_off` against `e23_post`, the previous commit's build from the same D: tree):
`sbf`, `sbf_bssd`, `sbf_te` PASS byte-identical; `cl` all CSVs identical, its only flagged lines the
wall-clock timing fields (the ARC LEDGER `COST` field the comparator does not mask; with every `ms` / `s`
value masked the logs are identical).

**Armed on real data** (`ejr_covl_{off,on}`: the 6 h May rooftop SBF capture `covl1452.26_`, PPP-RTK,
GPS + Galileo, no precise products for the date so every orbit is broadcast; the pair differs only in the
key). All 10 CSV outputs are byte-identical between the two runs: armed, the response wrote nothing. The
run-end line:

```
[2420, 129599.000 (2026-05-25 11:59:59.000)] [rt_engine] [info] [RT] broadcast-baseline jumps (end of run): 377 record change(s), 0 source switch(es); 375 below 10.0 m, 2 undecided, 0 jump(s) (0 of a pivot); 0 satellite re-seed(s), 0 ambiguity reset(s); largest |range change| 0.826 m on G07; closure holds.
```

Healthy record changes, |Δρ| (from the debug `EPH-CHANGE` lines in `stdout.txt`):

| changes | n | median | p99 | max |
| --- | --- | --- | --- | --- |
| GPS, new IODE | 26 | 0.063 m | 0.826 m | 0.826 m |
| Galileo, same IODnav (the receiver's I/NAV and F/NAV decodes alternating) | 179 | 0.332 m | 0.666 m | 0.668 m |
| Galileo, new IODnav | 170 | 0.341 m | 0.670 m | 0.696 m |

The two undecided changes followed gaps (E10 IODnav 52 to 82, G06 IODE 54 to 68). The 10 m threshold
sits 12 times above the largest healthy change and 9 times below the median corrupted-record error of
card INAV-ODD (89 m semi-major axis; the E23 event moved 266 m).

**Not done.** No real jump has been replayed: the E23 event was live and unrecorded, and the decoder
defect that produced it is fixed. A graded A/B of the response needs a paced obs / SSR / nav recorder or
a live session that meets a stale IGS record again. Arming the key in the live configs is the author's
decision after that grade.
