# 7 · Network tour: reading one network run

**Goal:** follow the multi-station network producer from what a run prints, not from its
files. After this page, a network run's tables, its stages and the place a given
quantity lives can be found without reading the driver.

Pages 1 and 2 come first. This page grows with the reorganization of the network
producer (card NOM, `docs\refound\NOM_network_object_model_2026-10-08.md`): each step of
that card adds a table or an object here, and the sections marked *in progress* say what
is not yet organized.

---

## What the network producer does

Many reference stations observe the same satellites. The producer estimates, every 30 s,
the corrections a single user needs to position itself with phase precision: a clock per
satellite, a code bias and a phase bias per satellite and signal. It writes them to a
product file. A held-out station, the closed-loop **consumer**, reads that product and
positions itself; its error against a known coordinate is the main quality number.

```
 58 stations' RINEX ──►  runNetwork()  ──►  products.txt  ──►  consumer (WTZZ00DEU)
                         one joint solve      clocks, biases       3D error per epoch
                         per epoch                                 logs\closed_loop_trace.csv
```

The joint solve splits its unknowns in two:

| part | what it holds | who sees it |
| --- | --- | --- |
| the **border** (shared) | satellite clocks, satellite phase biases, satellite code biases | every station; this is what the product carries |
| each station's **local block** | receiver clock, receiver code biases, wet delay, slant ionosphere per satellite, float ambiguities | that station only; eliminated before the border is solved |

## Running one network arm

Every network experiment starts from a **golden** configuration, a generated file that
carries the agreed posture: `config\golden\golden_field_refound.json` (58 real stations,
2024 day 129) and `config\golden\golden_sim_refound.json` (60 simulated stations with known
truth). An arm is a copy with one change. `scripts\rapid_arm.py` makes and runs arms:

```
python scripts\rapid_arm.py make --base config\golden\golden_field_refound.json --tag my_first_f2 --hours 2
python scripts\rapid_arm.py run --tags my_first_f2 --exe C:\GnssProcessor\build\x64-Release\gnss_processor.exe
python scripts\rapid_arm.py score my_first_f2
```

The run directory is `runs\_rap_my_first_f2\`: `products.txt` (the product),
`stdout.txt`, and `logs\` (every trace, `main_log.log` included). Network runs use the
Release build only: Debug is 50 to 100 times slower. A 2 h field arm takes about two
minutes on an idle machine. Simulated arms start at 00:30:15
(`--set network.start_offset_hours=0.504167`), because the simulated data's first minutes
are an artifact.

## Start with the tables

Four tables at the top of `logs\main_log.log` say what the run estimates, before any
residual or plot is read.

**The model as armed** (`Network (param model)`, card PRM): one line per kind of
parameter, with the process noise, its rule, the floors, the gap horizon, the route by
which it is carried from one epoch to the next, and the configuration key each number
comes from.

```
kind            est car          q units     q_rule         q_anchored ... route          source
sat_clock       yes yes      0.001 m^2/s     anchor_elapsed    2.9e-08 ... dense_border   filter.process_noise.sa...
sat_phase_bias  yes yes      1e-12 m^2/s     rw                      - ... dense_border   ...
ambiguity       yes yes          0 m^2/s     rw                      - ... local_block    network.phase_leveling_...
```

**The parameters as armed** (`Network (parameters as armed)`, card NOM step N1): every
column of every station's state, after the first emitted epoch and again at run end, one
line per class, counted by what the network did with it.

```
class                          kind              estimated_border estimated_local held_at_seed absorbed pinned ... stations  note
satellite clock, GPS           sat_clock                      480               0            0        0      0 ...       58
satellite phase bias, GPS L1W  sat_phase_bias                 143               0            0        5      0 ...       18  absorbed: anchored-union coverage 5
ambiguity, GPS L1W             ambiguity                        0             148            0        0      0 ...       18
receiver DCB, GPS              rx_code_bias.dcb                 0              58            0        0      0 ...       58
receiver position              rx_position                      0               0            0        0    174 ...       58  pinned: held at the roster coordinate
absorbed, anchored-union coverage: 5 instance(s), withheld when fewer than network.phase_datum_min_stations
stations hold the key this epoch (networkBuildPhaseDatumCandidates); satellite phase bias taken up by the
station's float ambiguity ...
```

The L1W lines show how to read a class against its neighbour: 148 L1W ambiguity columns
(one per station and satellite, at 18 stations), 143 of them sharing their satellite's L1W
phase bias on the border, 5 whose phase bias is withheld (a satellite tracked on L1W by a
single station cannot share a bias) and taken up by that station's own ambiguity. Every withheld instance names its rule, the configuration
key and the parameter that absorbs it.

The columns are the possible dispositions of one parameter column on one epoch:

| column | meaning |
| --- | --- |
| `estimated_border` | a column of the shared border |
| `estimated_local` | a column of the station's local block |
| `held_at_seed` | in the station's model at a fixed value, outside the solve |
| `absorbed` | left out because another parameter takes it up |
| `pinned` | held by the datum (the station coordinates) |
| `dropped` | removed from the model with a stated reason |
| `unobserved` | in the layout, but no observation reaches it this epoch |

The integrity lines that close the table should read 0: stations rejected after the
split, withheld states without their absorbing column, columns without a disposition, and
columns the classifier does not know. A line also counts the stations that did not reach
the split, by cause (for example one station-epoch with no usable observations). A class
held out of the solve by a rule whose premise does not hold on the run prints a WARNING.
On a three-frequency arm, for example:

```
WARNING: receiver IFB, GPS L5: held at its first-epoch seed at 58 station(s); dropped because the
satellite code biases are estimated (network.estimate_sat_code_bias); premise (GIM weighting ...)
does not hold on this run
```

That one line is the finding card L5R needed a residual study to make.

**The contributions as armed** (`Network (contributions as armed)`, card NOM step N2a):
every row and every prior that enters the joint solve, by kind. A station's block holds
observation rows and constraint rows side by side; this table says which is which, how
many there are, where each comes from and how it is weighted.

```
kind                      channel                stage        unit     count_epoch folds_epoch ... weight | builder
data_code                 station_rows           before_gate  rows             956          58 ... 1/sigma^2 of the stochastic model ...
data_phase                station_rows           before_gate  rows             954          58 ... 1/sigma^2 of the stochastic model ...
ambiguity_pin             station_rows           before_gate  rows              88          13 ... 1e10 (literal, an exact gauge) ...
block_rx_cb_carry         station_rows           before_gate  rows               0           0 ... unit (whitened local block) ...
prior_rx_cb_datum_pre_pd  station_prior_pre_pd   before_gate  entries           58          58 ... 1/network.rx_code_bias_datum_sigma^2 ...
cb_if_pin                 shared_rows            before_gate  rows              32           1 ... NetworkSbasis::kCodeBiasGaugeWeight ...
```

The excerpt is the cold first epoch, before any carry exists, so the carried-prior kinds
read 0; the run-end table counts them over the run. Every declared kind is listed, including
those this posture never uses (they read 0), so an armed family that produces nothing is
visible. The closing integrity lines (rows without a tag, tags left unset, tags out of place,
counts that disagree with the builders' own counters, shared folds without a kind) read 0.

Every check that asks what a row is (the post-fit outlier tests, the censuses, the
residual traces, the right-hand-side attribution) reads the row's tag. The row classifier
(`include\filter\NetworkRowClassify.hpp`) maps each kind to its class in one table, and
the outlier pool's admission rule is one declared predicate there (`raimPoolAdmitsKind`);
`NetworkRaim` counts each kind the pool admits.

**The signals as planned** (`Network (signals as planned)`, card NOM step N3a): for each
constellation and frequency band, which signal every station models, the companions, the
datum signals and the signals left out, with a reason and a station count. The roster
behind it (`include\app\SignalRoster.hpp`) is built on every run.

```
system   band role          signals
GPS      1    phase         L1C 40, L1W 18
GPS      1    code          C1C 40, C1W 18
GPS      1    datum         code: C1C 58 | pinned by: C1C 32 | phase: L1C 40
GPS      5    stations      58 report the band, 0 model it
GPS      5    excluded      no_latch: C5X 30, L5X 30, C5Q 28, L5Q 28
decision site      compared_epoch disagree_epoch compared_run disagree_run  group | where it is decided ...
if_pin_pair                   116             18          116           18  signal | the S-basis ionosphere-free pin pair ...
```

The excerpt is the 2 h field golden: 40 stations model GPS L1 on L1C/C1C and 18 on
L1W/C1W, and nobody models L5 on this posture. The decision-site rows below the band rows
compare the roster with the other places in the code that decide the same question, one
row per place: the epoch's count, the run's count, and how many disagree. A disagreement
is not an error by itself; it marks where two rules choose differently (here, the
ionosphere-free pin uses C1C on band 1, while 18 stations model C1W). Card NOM step N3b
turns each such row into a reader of the roster or a decision for the author.

All four tables are also written to the run manifest as JSON records (`param_model`,
`param_inventory`, `contributions`, `signals_as_planned`).

## The identities

Every network quantity belongs to a constellation, a satellite, a signal, a station, a
line of sight (station and satellite) or an arc (station, satellite and signal).
`include\common\GnssIdentity.hpp` defines each one once: a table of constellation
properties (`traitsOf`: letter, name, frequency bands), and the value types `SatId`,
`SignalId`, `ObservableId`, `SignalPair`, `StationId`, `LosId` and `ArcId`, each with one
parser, one rendering and an order that matches the string keys still in use. New network
code reads these types instead of testing `sat[0] == 'G'` or `signal[1] == '5'`.

The network code reads identities through these types (card NOM step N0b): a satellite
string's constellation comes from `systemOfSat` or `letterOfSat` and nowhere else, band
digits from `SignalId`, carrier frequencies from the traits table, and an estimator
column's family from `sharedColumnFamily` (`include\filter\NetworkRowClassify.hpp`).

*In progress:* many network containers are still keyed by composite strings such as
`"G05|L1C"`; card NOM step N4 re-keys them by the types above. Five estimator-label parses
remain and are listed in the card.

## One epoch, in eight stages

One epoch is `Application::networkProcessEpoch_` (`src\core\app\ApplicationNetworkEpoch.cpp`),
called by `runNetwork()` (`src\core\app\ApplicationNetwork.cpp`) for each synced epoch. Each
stage is a function in its own file, called in this order with the run (`NetworkRun`,
`app\NetworkRun.hpp`: the context, configuration, traces and engines) and the epoch
(`NetworkEpoch`, `app\NetworkEpoch.hpp`: this epoch's values, grouped by the stage that
writes them, const to every reader).

| # | stage | main functions (file) |
| --- | --- | --- |
| 1 | **signals:** which signals each station tracks and models | `networkBuildSignalRoster` (`app\SignalRoster.cpp`) |
| 2 | **per-station models and slips** (PASS A): geometry, corrections, cycle-slip detection, allocation of the satellite states | `networkPassAHead_`, `networkPassASlipScan_` (`app\ApplicationNetworkAssembly.cpp`) |
| 3 | **datum:** the reference satellite clock and phase anchors that fix what the data cannot | `networkElectPhaseAnchors` (`filter\NetworkPhaseDatum.cpp`) and the chain and ensemble elections beside it |
| 4 | **the carry decided:** how each carried value enters this epoch (process noise, routes) | `networkCarryDecide` (`filter\NetworkCarryDisposition.cpp`) |
| 5 | **per-station rows** (PASS B): each station's design matrix, split into border and local columns, folded into the joint system | `networkPassB_`, `networkSplitStation_` (`app\ApplicationNetworkAssembly.cpp`); the parameter inventory is recorded here |
| 6 | **the solve:** datum rows, carried priors, the joint solve, the within-epoch quality checks | `networkSolveGlue` (`filter\NetworkSolveGlue.cpp`), `networkInstallCarry` (`filter\NetworkInstallCarry.cpp`) |
| 7 | **the product:** the satellite corrections prepared and written | `networkSolPrep` (`filter\NetworkSolPrep.cpp`), `networkEmitStore` (`filter\NetworkEmitStore.cpp`) |
| 8 | **after the product:** post-fit checks, the carries for the next epoch, diagnostics, the state snapshot | `networkLeveling`, `networkPostFitRaim` (`filter\NetworkRaim.cpp`), `networkClockQc`, `networkStashStatus`, `networkWriteStateSnapshot` |

The order of stages 7 and 8 matters: the product is written before the post-fit checks
run, so a check's response reaches the product one epoch later.

The state the stages share has three homes. What belongs to one constellation, satellite,
satellite signal, station, line of sight or arc is a row of an entity table
(`net::NetworkEntities`, `include\app\NetworkEntities.hpp`), keyed by its typed identity, so
everything about one satellite sits in one record; each field's declaration row states how it
survives a restart. What one stage hands later stages of the same epoch is an epoch event
(`EpochEvents`, `include\app\NetworkEpoch.hpp`, read as `ep.events`): it is built empty with
the epoch, one group per writing stage, so nothing is cleared by hand and nothing from a
skipped epoch reaches the next one. What stays run-wide (the carried priors, latches set at
one epoch and read at the next, hand-offs a later stage drains, run counters) is
`NetworkCarryState` and `NetworkServiceCounters` in `NetworkRunContext`
(`include\app\NetworkRunContext.hpp`), each member with its row in `net::kRunFields`. A stage
names what it writes: an events group arrives as an output parameter, and an entity table it
binds for writing has its row in `net::kEntityWriters`.

## The run end

After the last epoch, every mechanism prints what it did over the run: counters, verdicts,
INERT clauses for armed knobs that never acted. These lines are the run's own telemetry
(an armed knob reading "0 driven" is dead wiring), and grading scripts parse them.

The run end is a table. `include\filter\NetworkRunEnd.hpp` declares
`kNetworkRunEndReports`: 107 reports in print order, each row naming what it reports and
the file it lives in. `networkTeardown` (`src\core\filter\NetworkTeardown.cpp`, 68 lines)
walks the table. A report is one function, `int networkXxxRunEnd(const NetworkRunEndView&)`,
in the file of the subsystem it describes: the RAIM pool's report is in `NetworkRaim.cpp`,
the gross line-of-sight screen's in `NetworkGrossLos.cpp`. It reads the run through the
view, prints its lines when its subsystem is armed, and returns 0, or a verdict for the
exit code. To find what a mechanism reports at run end, open its file and search for
`RunEnd`; to see the whole run end in order, read the table.

## Where new code goes

`CLAUDE.md` (section "Network producer: where new code goes") maps each kind of change to
its file: a new per-epoch step to a `Network*` stage file, a new per-epoch event to the
writing stage's group of `EpochEvents`, new run state to `NetworkRunContext`, new emission behaviour to `NetworkSolPrep` or `NetworkEmitStore`, a
new run-end readout to a report in the subsystem's own file plus one row of
`kNetworkRunEndReports` (the table's header lists the order rules). The loop itself stays a
list of calls.

## The record

The network is a research program, and its decisions are recorded:

| where | what |
| --- | --- |
| `docs\refound\CARDS.md` | one line per design card of the current program, in order |
| `docs\refound\<ID>_*.md` | one card: the question, the frozen gates, the results under dated headings |
| `docs\CONFIG_promotion_ledger.md` | why each switch in the goldens is on |
| `config\golden\golden_manifest.json` | what the goldens pin (they are generated by `scripts\make_golden_config.py`, never edited by hand) |

A change to network behaviour starts as a card with its gates written before the first
run, is graded on both the simulated and the real bed against the baseline, and reaches
the goldens only by the author's decision.

Next: [8 · Adding a network parameter](08_adding_a_network_parameter.md) when a task adds a
per-station state; otherwise back to [6 · First tasks](06_first_tasks.md), or the network
card a task names.
