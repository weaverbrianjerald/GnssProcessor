# NOM5: the network's station parameter set, finished (card NOM step N6, steps N6f to N6j)

**STATUS**

- **id:** NOM5
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-10-30. Card NOM's step N6 landed its body (steps N6.0 to N6e, merge `f64f916`, 2026-10-20: the network owns its station parameter set, and one observation equation serves the producer, the consumer and the seeds); the merge named N6f to N6j as remaining. This card builds them on branch `nom/n6-tail`: N6f, direct assembly (no dense Jacobian on the network; byte-identical; cost expected lower); N6g, the seeds on the whole equation behind `estimation.seed_full_equation` (default off, fingerprinted; the consumer moves at births); N6h, a user-side layout flag that reaches no parameter of a network run refused at load, and `network.estimate_tropo_gradients` (default off, fingerprinted) as the first station parameter added through the set alone; N6j, the close (tests, onboarding pages 7 and 8, `CLAUDE.md` routing). N6i (a canonical column order) is not taken (decision D2's default).
- **source:** `scripts/nom3_designs/N6_design.md` (sections 3 to 7; decisions D1 to D10, defaults taken); the merge `f64f916` ("N6f to N6j remain"); `docs/onboarding/08_adding_a_network_parameter.md` (the recipe and its open rough edge on gradients); `docs/refound/UOM_n6_user_side_edits_2026-10-17.md` (the user-side notices of N6a and N6b)
- **knobs / posture:** N6f and N6j: no key. N6g: `estimation.seed_full_equation` (new, default false, fingerprinted, ledger UNVALIDATED). N6h: `network.estimate_tropo_gradients` (new, default false, fingerprinted, ledger UNVALIDATED); the load-time refusal of section 2.3 (no key). Base: the goldens of main `5127429` (card BOF's posture; baseline pair `runs/_rf0hv17_{field,sim}6`).
- **dates in the text:** 2026-10-30
- **hashes / run dirs:** branch `nom/n6-tail` from main `5127429`; worktree `D:/GnssProcessorSupport/worktrees/n6tail`; control executable `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_n6tctl.exe` (the worktree's Release build of `5127429`, before any change); gate records `D:/GnssProcessorSupport/gates/n6t/`; run root `D:/GnssProcessorSupport/runs_nom`
- **gates (frozen at pre-registration):** section 3, written before any run of a changed executable

**Outcome.** Pre-registered 2026-10-30; no result yet.

## 1. Question

Can the station parameter set of card NOM step N6 be finished as designed? Four parts: the producer's data rows
assembled straight into the shared border and the local block with no output bit moved (N6f); the seeds read the whole
observation equation behind a key, with the consumer's births graded (N6g); a user-side layout flag that reaches no
parameter refused, and a new station parameter added through the set alone (N6h); the tests and the reader's pages
brought to the finished structure (N6j).

## 2. The steps

### 2.1 N6f: direct assembly (no key)

At `5127429`, `networkStationRows` (`network/assembly/StationRows.cpp`) fills a dense matrix H of m rows by one column per
slot of the station's set (zeroed, and one zeroed vector of the set's length per row), and `networkSplitStation`
(`StationSplit.cpp`) copies its columns into `A_s` (the border) and `A_r` (the local block). The congruence step
(`network/qc/Congruence.cpp`) reads the three position columns of the same H, and `rxIfbExcludedRows` reads its
receiver IFB columns.

After N6f the row loop keeps each row's partials as (slot, value) pairs in the order `scatterDense` writes them; a slot
written twice in one row keeps the last value, as the dense assignment does. The split decides each slot's
disposition by the same tests in the same order (position pinned; a shared entry with a border column; the receiver
IFB rule; a slot whose every partial is plus or minus zero is unobserved, the `maxCoeff() == 0.0` meaning for an
explicit zero partial; a slot whose nonzero partials are all on excluded rows is unobserved) and writes every pair,
zeros included, into `A_s` or `A_r`. When two shared entries name one border column, the later entry of the split's
loop order owns it, as the column copy does today. The congruence step builds its m by 3 position block from the
pairs; `rxIfbExcludedRows` reads the pairs.

Expected: no output bit moves; the field golden's loop is faster (no dense H, no column copies).

### 2.2 N6g: the seeds on the whole equation (`estimation.seed_full_equation`)

`kLegacySeedMask` (`measurement/Seeds.hpp`) reproduces the two pre-N6 copies of the equation, which leave out the range
correction, the satellite phase bias, the satellite clock, the SSR bias, the phase colour and (for the residual seeds)
the phase clock. With the key on, a seed subtracts every known term of its row (the mask `kFullSeedMask`, its exact
term list stated in this card when built, with the order of subtraction extended at the end of `kSeedOrder` so the
legacy terms keep their order), and every seed takes the carrier frequency of the satellite's own GLONASS channel (the
open frequency item of `Seeds.hpp`). The key is fingerprinted: a resumed run never crosses it.

Expected: the producer is unchanged with the key on (at a producer birth the omitted terms are zero: shared states at
0, no SSR term, range correction 0; the goldens carry no GLONASS); the consumer moves at births, because its SSR code
and phase biases (metres) leave its inter-system bias, clock, code-bias and ambiguity seeds.

### 2.3 N6h: the layout flags and the gradient (`network.estimate_tropo_gradients`)

**The flags.** Decision D7's default is a load error, in network mode, for `estimation.estimate_velocity`,
`estimation.estimate_phase_clock`, `estimation.estimate_tropo_gradients` and `imu.enabled`, naming the network key. Its
premise (design risk 7: such a flag gives a network station extra columns) was removed by N6e, and a finding of this
pre-registration narrows it: the closed-loop and replay consumers are configured by a copy of the network
configuration (`GnssConfig::deriveClosedLoopConsumerConfig`, `GnssConfigLoadNetwork.cpp`), which keeps these flags, so
in a network run with a consumer they set the consumer's layout. The setup's warning ("a network station has no such
parameter; it is ignored", `ApplicationNetworkSetup.cpp`) is therefore wrong for a run with a consumer. N6h refuses a
flag only where it reaches no parameter of the run (network mode with no consumer), naming the network key where one
exists, and states the flag's scope (the consumer, not the stations) otherwise. The six flags of today's warning
(the four above, `estimation.state_ambiguity_basis nl_wl` and `estimation.between_satellite_sd.enabled`) take one rule.
The configurations under `config/` are scanned for the flags in network mode before the rule lands.

**The gradient.** `network.estimate_tropo_gradients` gives each station two local columns, north and east, born at 0 on
its first epoch with the key on, through the recipe of onboarding page 8 (the `ParamRef` kind, the set's field, the
registry row, the inventory class, the contribution sets, the labels, the loader line, `NetworkKnobs`, the ledger row
UNVALIDATED, a run-end line). The observation equation already holds the term
(`mf_grad * (cos(az) * G_N + sin(az) * G_E)`). A first version is uncarried: estimated afresh every epoch, as the
golden posture's wet delay is. Page 8 leaves one decision open: whether uncarried gradient columns keep the station
blocks well conditioned or need a prior. That decision is graded here (gate G4); a prior
(`network.tropo_gradient_prior_sigma_m`, a zero-mean pseudo-observation per column per epoch) is built only if the
uncarried columns fail it.

### 2.4 N6j: the close (no key)

The 11 files of `tests/network` that still name `StateManager` or `StateKeys::` at `5127429` are re-pointed where the
name stands for the network's own set (a test of a user-side helper keeps it); onboarding page 8 takes the gradient as
its worked example and updates its rough edges, page 7 (the network tour) and the code tour follow; the `CLAUDE.md`
network routing names `StationParams` and the equation for a new station parameter; `docs/status/OPEN_ITEMS.md` and
`nom_epoch_lint.py` are brought up to date.

## 3. Gates (frozen 2026-10-30, before any run of a changed executable)

Executables: Release builds of the worktree, renamed copies under `D:/GnssProcessorSupport/gates/exe/x64-Release/`;
the control is the build of `5127429` before any change. Every difference a gate shows is a finding to trace, not to
explain away; nothing lands with an untraced difference.

- **G0, identity (N6f; N6g and N6h with their keys absent).** The 23 G5 arms and the snapshot bytes
  (`gates/g7/gate_pair.sh`): 24 of 24 IDENTICAL. The 16 user arms (`scripts/uom_gate.py`, quick set): 16 of 16
  IDENTICAL. Real time (`sbf`, `sbf_bssd`, `sbf_te`; `cl`): identical apart from wall-clock text. For N6h the declared
  text moves are the setup's flag lines and the run-start tables' rows that name the new kind; nothing else.
- **G1, checks.** The Debug suite of the worktree passes (fixture skips only); `nom_epoch_lint.py`,
  `nom_n4_verify.py --whole`, `make_golden_config.py --verify`, `config_posture_check.py --verify-loader`,
  `ledger_render.py --check` and `docs_index_check.py` pass.
- **G2, cost (N6f).** Two timing pairs per golden bed on 2 h arms (control and candidate at the same time, the launch
  order swapped in the second pair, `scripts/cost_attrib/abba.sh`): the loop per epoch is not slower than the control by
  more than 1 % on any pair (the design allowed +3 % per body step and expects N6f below zero); the station-add section
  is reported.
- **G3, full-equation seeds (N6g, key on, against the same executable with the key off).** The producer's products and
  traces on both 2 h goldens are byte-identical (the prediction of section 2.2; a move is a finding traced before any
  grading). The consumer: `WTZZ00DEU` by hour on the 6 h field golden, `SZZ000SIM` from 00:30:15 on the 6 h sim golden,
  and the 16-site census (`gcensus.py`; `simcensus6h.py` with `SIMCENSUS_BASE`). Recommendation bar (decision D6):
  hours 1 to 6 not worse at the held-out consumers by more than 1 mm in any hour, the census median site not worse,
  and no census site worse by more than 1 cm (card NSP: a neutral change moves no site by more than 0.19 mm, so a site
  past 1 cm is a response). The arming decision is the author's.
- **G4, the gradient (N6h, key on, against the same executable with the key off).** The three run-start tables list
  the two columns per station (estimated-local counts, integrity lines at 0); the run-end line counts the columns
  estimated. Conditioning, on 2 h field and sim arms: no station block refused or dropped beyond the control's count
  (the kernel's positive-definiteness refusals and `sta_pd_dropped` events); the per-station robust spread of the
  epoch estimates reported (the sim truth has no gradient, so its estimates are noise); the held-out consumers and the
  wire against the control reported, with no adoption claim. A failed conditioning clause builds the prior of
  section 2.3 and repeats G4 with it.
- **G5, the refusal (N6h).** Unit tests: each flag in a network configuration with no consumer fails to load and the
  message names the network key where one exists; with a consumer the configuration loads and the setup states the
  flag's scope; outside network mode nothing changes. The configuration scan of section 2.3 lists every file the rule
  refuses.

### 3.1 Amendment A1 (2026-10-30, before any run of an N6g executable)

The configuration toggle convention (`CLAUDE.md`, "Config toggles") puts a user-visible key on the run-start
`Toggles:` line. `estimation.seed_full_equation` takes the field `seed_full=` at the end of that line, so G0 for N6g
reads identical apart from that one line of `stdout.txt` and `main_log` (`seed_full=off`) in every arm. No other text
moves are declared for N6g.
