# MGT: the telemetry of the isolated-arc refloat maturity gate

**STATUS**

- **id:** MGT
- **topic:** raim-qc
- **status:** closed
- **summary:** CLOSED 2026-10-30 on branch `claude/confident-elgamal-6b01b9` (committed, not merged, nothing armed). BUILT and GATED, default off. Card RCG found `network.raim_refloat_maturity_gate` silent. The gate now reports what it holds and when it lifts: a run-scope tally (flags, arc-epochs and epochs held), one `RAIM_MATURITY` event row per held flag and one at the lift, a run-end section and an arming line; the `acted` column of the attribution trace reads a refloat taken. Gate off, the final executable is byte-identical to the base on a 2 h field arm and a 2 h sim arm (products, closed-loop trace, events, attribution trace, solution, epoch status; only wall-clock and provenance text differs). Gate on at 120 epochs, 2 h field: the same files identical apart from 28 added event rows and 27 `acted` values (1 to 0); 27 flags held on 11 epochs (19 arcs, 3 held more than once), the lift at 01:02:30 (62.5 min, epoch 126, 126 emitted epochs, 120 EWMA updates), and 3 of the 4 flags that pass after it are arcs held before it. The persistence gate had the same `acted` defect and the same test fixes it. Pre-registered 2026-10-30 (`7abddb9`).
- **source:** card RCG sections 1.2, 4.3 and 4.5 (the gate, its measured behaviour and the telemetry it owes); `src/core/network/qc/Raim.cpp`, the T4 attribution block of `networkPostFitRaim`
- **knobs / posture:** read, not changed: `network.raim_refloat_maturity_gate` (UNVALIDATED) with `network.raim_refloat_maturity_epochs` (PARAM). No new key. Base: `config/golden/golden_field_refound.json` and `config/golden/golden_sim_refound.json` of main `1527cf9`.
- **dates in the text:** 2026-10-30
- **hashes / run dirs:** base exe `D:/GnssProcessorSupport/worktrees/mgsi/build/x64-Release/gnss_processor_rcg.exe` (sha256[:16] `fab88a924e9189fd`, the Release build of main `1527cf9`); changed exes `gnss_processor_mgt.exe` (`440b6a8772d6e172`, first build) and `gnss_processor_mgt2.exe` (`00a0cad2728733c6`, final build), copies of `build/x64-Release/gnss_processor.exe` of the worktree `C:/GnssProcessor/.claude/worktrees/confident-elgamal-6b01b9` (its build tree is a junction to `D:/GnssProcessorSupport/worktrees/confident-elgamal-6b01b9_build`); run root `D:/GnssProcessorSupport/runs_nom`: first-build arms `_rap_mgt_{boff,noff,bon,non}_f2` and `_rap_mgt_{boff,noff}_s2`, final-build arms `_rap_mgt_{noff2,non2}_f2` and `_rap_mgt_noff2_s2` (b = base exe, n = changed exe, off and on = the gate, f2 and s2 = 2 h field and sim)
- **gates (frozen at pre-registration):** section 3, written before any run of the changed executable

**Outcome.** Question: can the maturity gate report what it holds and when it lifts without changing what it does? Result: yes. With the gate off, a 2 h field arm and a 2 h sim arm of the changed executable match the base executable in every product and trace. With it on at 120 epochs the 2 h field arm differs only by 28 event rows and 27 `acted` values (1 to 0). The telemetry reads 27 flags held on 11 epochs and the lift at 62.5 min, and names the three arcs held five, three and three times that the lift then refloats. All gates pass. Standing: nothing armed; arming remains the author's decision (section 5).

## 1. Question

Can the maturity gate report what it does, without changing what it does? The gate holds an isolated-arc T4 refloat
until the run has emitted max(`raim_refloat_maturity_epochs`, `phase_leveling_warmup_epochs`) epochs and the sigma0
EWMA has received `raim_refloat_maturity_epochs` updates (`networkWarmupRampLifted`). It has two edges (standing
invariant 7): each held flag leaves a stiff carry in the solve that T4 would have dropped, and the lift re-couples
every later flag to the refloat. Card RCG measured both edges from the outside (27 held flags and a lift near 63 min at
120 epochs). The producer reported neither.

## 2. What is built

| piece | where | what it reports |
| --- | --- | --- |
| the gate rules as a pure function | `raimSelectIsolatedArcs` (`network/qc/Raim.hpp`) | the arcs T4 refloats, the flags the maturity gate holds (with station, satellite and statistic) and the persistence streaks to carry; the order of the tests is the T4 block's order, so the unit tests pin the seam |
| the run-scope tally | `RaimMaturityGateTally`, `RaimCounters::maturity_gate` | scans (the opportunity count), flags that reached the gate, flags held (arc-epochs), epochs with a held flag, the first and last held epoch, and the lift (processed epoch, GPS time, `n_emitted`, sigma0 EWMA update count) |
| one event row per held flag | tier `RAIM_MATURITY`, event `arc_held`, action `hold` | station, satellite, the flag statistic (interaction over sigma0), the bar, and the point of the run (`n_emitted`, EWMA updates, the need) |
| one event row at the lift | tier `RAIM_MATURITY`, event `lift`, action `lift` | `n_emitted` as the statistic, the epoch need as the threshold, and the EWMA update count, the held arcs and epochs in the detail; one info line as well |
| a run-end section | `networkRaimMaturityGateRunEnd`, in the RAIM subsystem's report | the counts above, and the four outcomes the denominators tell apart: never consulted, never lifted, held then lifted, open from the first scan (held nothing) |
| an arming line | loader, `validateNetworkCarryPosture_` | ARMED with the rule the gate applies at run time, or INERT (no `network.raim_attribution`), plus a warning when the horizon does not exceed the carry warm-up |
| the `acted` column | `network_raim_attrib.csv`, ARC rows | 1 only when T4 refloated the arc or its satellite |

The held and lift rows carry their own tier, not `T4_attrib`: every grader that counts the `T4_attrib` rows as
refloats (`scripts/rapid_arm.py`, `scripts/ev7_event_read.py` and others) keeps its meaning on a gate-on arm.

The persistence gate (`network.raim_arc_persist_epochs`, K) had the same `acted` defect: an arc over the bar that has
not yet persisted K epochs is not refloated and read `acted=1`. The fix is the same test (membership in the refloat
set), so it is made here. The goldens run K = 1, where the two readings coincide.

## 3. Gates (frozen 2026-10-30, before any run of the changed executable)

The base executable is the Release build of main `1527cf9` that card RCG used
(`D:/GnssProcessorSupport/worktrees/mgsi/build/x64-Release/gnss_processor_rcg.exe`, sha256[:16] `fab88a924e9189fd`).
The changed executable is the Release build of this branch. All arms are golden copies with the one change named, made
with `scripts/rapid_arm.py make` under `RAPID_RUNS_ROOT=D:/GnssProcessorSupport/runs_nom`.

| gate | arms | bar |
| --- | --- | --- |
| G0 unit | `gnss_tests` Debug | the new suites pass; the full suite is green apart from the one fixture-gated skip |
| G1 gate off, field | 2 h field golden, base exe and changed exe | `products.txt`, `logs/closed_loop_trace.csv`, `logs/network_events.csv`, `logs/network_raim_attrib.csv` and the solution trace byte-identical |
| G2 gate off, sim | 2 h sim golden from 00:30:15, same pair | the same four files byte-identical |
| G3 gate on, field | 2 h field golden with `raim_refloat_maturity_gate=true`, `raim_refloat_maturity_epochs=120`, base exe and changed exe | products, solution and closed-loop trace byte-identical; the events file identical apart from the added `RAIM_MATURITY` rows; the attribution trace identical apart from the `acted` values of held arcs; the run-end text gains the gate section |
| G4 reading | the G3 changed-exe arm | the held flags are about 27 in the first hour and the lift falls near 63 min (card RCG section 4.3); the `arc_held` rows equal the held count of the run-end line; the lift row and the run-end line agree; every ARC row with `acted=1` before the lift names a refloat in the events file |

A gate that fails is reported with the difference; no tolerance is applied to a byte comparison.

## 4. Results (2026-10-30)

### 4.1 Executables, arms and one amendment

| role | executable | sha256[:16] | note |
| --- | --- | --- | --- |
| base | `gnss_processor_rcg.exe` | `fab88a924e9189fd` | Release build of main `1527cf9`, the executable of card RCG |
| first changed build | `gnss_processor_mgt.exe` | `440b6a8772d6e172` | compiled 2026-10-30 09:04 |
| final changed build | `gnss_processor_mgt2.exe` | `00a0cad2728733c6` | the executable this branch commits |

The six arms of the first build ran at once (field about 500 s, sim about 335 s each, on a shared box), then the three
arms of the final build (field 420 s, sim 265 s). Every arm exits 0. The comparisons are byte hashes of each file of
`products.txt` and `logs/*.csv` plus the run manifest, and, for the two traces the gate touches, a row-wise comparison.

**Amendment A1** (2026-10-30, after the first-build runs, before the final-build runs). The loader's warning for a
horizon that does not exceed the carry warm-up said the gate holds nothing whether or not the sigma0 EWMA is fed.
With the EWMA fed its update count binds (it starts at the first post-fit pass, after the carry warm-up), so the gate
holds for about `horizon` passes whatever the horizon; the warning is now given for the unfed EWMA only. The change
is log text. The gate-on arms of the two builds are identical in every product and trace, events and attribution trace
included, and the gates were repeated on the final build.

### 4.2 Gates

| gate | result |
| --- | --- |
| G0 unit | Debug `gnss_tests`: the five new suites (21 cases) pass. The full suite on the preceding Debug build (before amendment A1 and the loader test it changed): 3 652 tests in 497 suites, 3 619 passed, 33 skipped, none failed; every skip reads "fixture not present" (the SIF, GPT3, SP3, IONEX and BRDC fixtures; this worktree has no populated `data/`). On the final build the 427 tests of the suites matching `*Raim*`, `*Loader*`, `*Config*`, `*Posture*`, `*RunEnd*` and `*GrossLos*` pass (1 fixture-gated skip). PASS |
| G1 gate off, field | final build against base: `products.txt`, `logs/closed_loop_trace.csv`, `logs/network_events.csv`, `logs/network_raim_attrib.csv`, `logs/network_solution.csv`, `logs/network_epoch_status.csv` and 26 other files byte-identical; four files differ in wall-clock or provenance only (below). The first build gave the same. PASS |
| G2 gate off, sim | the same, from 00:30:15. PASS |
| G3 gate on, field | final build against base: products, solution, closed-loop trace and every other file identical apart from the four wall-clock files; `network_events.csv` is the base file plus 28 `RAIM_MATURITY` rows (13 770 old-tier rows identical, in order); `network_raim_attrib.csv` has the same 5 475 rows with 27 ARC `acted` values 1 to 0 and no other difference. PASS |
| G4 reading | section 4.3: 27 flags held (24 in the first hour), the lift at 62.5 min; the `arc_held` rows (27) equal the run-end held count; the lift row and the run-end line agree; every ARC row with `acted=1` has a `T4_attrib` refloat event at its epoch (0 without). PASS |

The four files that differ in every pair are `network_datum_graph.csv` (column `ms`), `network_run_summary.csv`
(`loop_ms`, `ms_per_epoch`), `network_service_health.csv` (`ms_per_epoch_window`) and `network_run_manifest.json`
(`written_utc`, `config_path`, `config_hash_fnv1a64`, the build stamp, `product_output_path`, `loop_ms`). A pair of
runs of the same executable differs in the same four files.

### 4.3 What the telemetry reads (2 h field, gate at 120 epochs, final build)

- **Decoupling edge.** 27 isolated-arc flags held on 11 epochs, between processed epochs 8 and 121: 12, 12 and 3 by half
  hour (24 in the first hour), the last at +60.0 min. They are 19 arcs on 6 stations; three arcs were held more than
  once: HKSL00HKG E04 five times (+51.5, +56.5, +57.5, +59.5, +60.0 min), HKSL00HKG E13 and E19 three times each (+57.5,
  +59.5, +60.0 min). The flag statistic (interaction over sigma0) runs from 6.6 to 269, median 10.8, against the bar of 6.
- **Re-coupling edge.** One `lift` row at TOW 262950.0 (01:02:30, +62.5 min), processed epoch 126: `n_emitted=126`
  against a need of 120, 120 sigma0 EWMA updates (the EWMA starts counting at the first post-fit pass, after the 6-epoch
  carry warm-up, so the epoch count is 6 above the horizon at the lift). The run-end line reports 234 scans, 31 flags
  at the gate, 27 held, 4 passed.
- **After the lift.** 4 isolated-arc refloats: HKSL00HKG E04, E13 and E19 at +64.0 min (the three arcs held before the
  lift) and KIT300UZB G21 at +75.0 min. The `T4_attrib` rows by half hour (0, 0, 4, 0) equal the base arm's and the
  first four half hours of card RCG's 6 h arm. So the lift refloats the persistent misfits and nothing else catches up.
- **Attribution trace.** ARC rows with `acted=1`: 31 in the base arm (12, 12, 7, 0 by half hour), 4 in the changed arm
  (0, 0, 4, 0). The 27 held arcs that appear in the trace read 0.
- **Consumer.** WTZZ00DEU 3D p50 for hours 1 and 2 (mm): 58.80 and 12.30 with the gate off, 58.65 and 12.30 with it on, the
  same in both executables and equal to card RCG's values.

### 4.4 Verdict

Every gate passes. The telemetry reproduces card RCG's measurements (27 held flags, the lift near 63 min, the three
persistent HKSL00HKG arcs) from the producer's own rows. Nothing is armed and the ledger verdicts of
`network.raim_refloat_maturity_gate` (UNVALIDATED) and `network.raim_refloat_maturity_epochs` (PARAM) are unchanged.

## 5. Owed and not done

Triage per standing invariant 3: field-blocking would make the key not field-runnable; none of these does.

- **The seam's wiring is certified by the data arm, not by a unit test** (field-inert). `networkPostFitRaim` needs a
  solved network. The gate logic (`raimSelectIsolatedArcs`), the tally, the event rows, the run-end text and the loader
  lines are unit-pinned; the call sites are read by section 4.3.
- **The persistence gate (K above 1) has unit tests only** (field-inert). The goldens run K = 1 (ledger row PARAM, default
  1), where the old and new `acted` coincide. Earlier runs with K above 1 (the sim ladder of that ledger row, the K = 1e6
  legs of `scripts/ev_dose_legs.py`) recorded `acted=1` for an arc the persistence gate held; this build records 0.
- **No counter pairs a held arc with its later refloat** (field-inert, measure-only). The join of the `arc_held` rows
  with the later `T4_attrib` `attrib_arc` rows gives it: 3 of the 4 refloats after the lift here.
- **`scripts/rapid_arm.py score` counts a fixed list of event tiers** and `RAIM_MATURITY` is not in it. The held count is
  in the run-end line and in the tier's rows.
- **The 6 h grade stands as card RCG made it.** This card repeats no 6 h arm. Arming is the author's decision. The
  register line of `docs/status/OPEN_ITEMS_BUILT_OFF.md` that lists the telemetry as owed is edited on merge, not here:
  this branch is cut from main `1527cf9`, before card RCG's merge.
