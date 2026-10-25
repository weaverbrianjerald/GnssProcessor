# Workbook answer key

One entry per question. **Source** names the card and lines, the file and lines, or the run;
**Check** gives the command that reproduces the answer where one exists, with its runtime on
the development machine (commands run from `C:\GnssProcessor` in Git Bash; `D:` connected).
Numbers marked *checked* were reproduced on 2026-10-24 for this key; the verification log in
[README.md](README.md) lists the main checks. Paths in `results/` are under
`D:\GnssProcessorSupport\presentation\status_update_2026-10-29\`. "Status report lines" are
lines of the draft saved before the qualifications were applied,
`backup_2026-10-24\status_update_2026-10-29.tex` in that folder.

Cards named by their short name are files under `docs/refound/`: ALD
`ALD_anchor_level_datum_2026-10-23.md`, ARL `ARL_integer_ambiguity_ladder_2026-10-09.md`, CTU
`CTU_coupling_time_update_2026-09-28.md`, FCFC `FCFC_fixed_cycle_ledger_2026-10-11.md`, GAD
`GAD_galileo_adoption_2026-10-13.md`, IRD `IRD_integer_recovering_datum_2026-10-13.md`, LAH
`LAH_level_aware_hold_2026-09-29.md`, NBP `NBP_native_broadcast_clock_posture_2026-10-07.md`, R1D
`R1D_station_level_datum_2026-10-23.md`, SCB `SCB_sim_clock_error_bed_2026-10-24.md`, SCL
`SCL_self_contained_clock_level_2026-10-03.md`. `SBASIS-THEORY-E.md` and `SBASIS-CONTRACT.md` are
under `docs/cards/network-vehicle/`.

---

## Module 1 · The observation model and the rank deficiency

### Q1.1 The parameters as armed

**Answer.** The table lists, per class, the columns estimated on the shared border and locally.
Border: satellite clock (GPS 498, Galileo 393), satellite phase bias (GPS L1C, L2W; Galileo L1C,
L5Q) and satellite code bias (GPS C1C, C2W; Galileo C1C, C5Q). The border counts are
station-held instances, one per line of sight (498 GPS, 393 Galileo), not unique columns. Local:
ambiguity per signal, slant ionosphere per system, zenith wet delay (60), receiver clock (60),
Galileo inter-system bias (60), receiver DCB per system (60 each). Receiver position: 180
columns "pinned: held at the roster coordinate".

Mapping: `dt_r` → receiver clock; `δt_r(g)` → inter-system bias, Galileo (GPS is the reference
system); `T` → zenith wet delay on top of the model; `I` → slant ionosphere; `B_r,j` → receiver
DCB; `N` → ambiguity; `dt^s`, `B^s_j`, `b^s_j` → satellite clock, code bias, phase bias. The
receiver phase bias `b_r,j` has no state (parameter-model line `rx_phase_bias    no  no`): the
float ambiguities absorb it, type 4 of the rank table.

Columns at the first epoch: shared (1 + 2f)·m = 5 × 56 = 280 (`nshared=280`); local 3 per line
of sight (two ambiguities, one slant ionosphere) × 891 + 5 per station × 60 = 2,673 + 300 =
2,973. *Checked.*

**Source.** `runs/_rf0hv15_sim6/stdout.txt` lines 380, 639, 658 to 686;
`src/core/network/assembly/ParamInventory.cpp` lines 645 to 661 (the writer);
`include/network/assembly/ParamInventory.hpp` lines 136 to 147.

**Check.** `sed -n '380p;639p;658,686p' runs/_rf0hv15_sim6/stdout.txt` (instant).

### Q1.2 Eq. 24 by hand

**Answer.**
1. 1 + 2f + (n − 1) + m + f(n − 1) + f·m = 1 + 2f + (1 + f)(n − 1) + (1 + f)m
   = 1 + 2f + (1 + f)(n − 1 + m). The slant ionosphere adds (n − 1 + m).
2. f = 2, m = 32: 5 + 3(n + 31) + (n + 31) = 5 + 4(n + 31). n = 60: 369; n = 58: 361;
   n = 305: 1,349. The slope of 4 per station is the figure's "slant ionosphere: 4 per station";
   without the slant term it is 3 (1 + f).
3. n = 60, m = 56, f = 2: 1 + 4 + 3 × 115 = 350, plus 115 = 465. Eq. 24 is a single-system
   count and the record has no two-system form, so 465 is a scale, not a prediction. The rank
   check counts something else: the shared 280 × 280 matrix after every station block is
   eliminated and after the reference rows that act before the gate (ambiguity pins, code-bias
   rows, receiver DCB prior) are folded. The clock pins act in the solve after the gate, so one
   flat clock direction per constellation is left: 2. `Sbasis.hpp` states it: "the reduced
   shared normals have exactly #constellations residual deficiencies, which the
   assertNullSpaceRank gate verifies before the solve". The record calls the gate "the runtime
   assert" of Eq. 24 (`SBASIS-CONTRACT.md` lines 70 to 71); it asserts only this residual.
   *Checked.*

**Source.** `docs/cards/network-vehicle/SBASIS-THEORY-E.md` lines 97 to 114; deck slide 6;
`runs/_rf0hv15_sim6/stdout.txt` line 640 (`declared_dim=2`); `include/network/solve/Sbasis.hpp`
lines 50 to 55; `include/network/solve/SolveGlue.hpp` lines 13 to 27 (stage order).

**Check.** `C:/Python313/python.exe -c "print([1+4+3*(n-1+m)+(n-1+m) for n,m in ((60,32),(58,32),(305,32),(60,56))])"`
prints `[369, 361, 1349, 465]`.

### Q1.3 Which directions are null

**Answer.** First epoch (tow 259230): 280 shared columns, declared 2, two eigenvalues below the
cutoff, +4.036e-11 and −1.803e-10 (a negative value is a rounded zero). Their eigenvectors put
0.2040 on every Galileo clock and 0.1767 on every GPS clock: 1/√24 = 0.2041 and 1/√32 = 0.1768,
the unit vector that shifts all clocks of one system by the same amount. These are the clock
common modes (type 1a, one per system), left for the clock pins. Cutoff = 1e-11 × λ_max =
1e-11 × 1.0459e5 = 1.0459e-6. Cutoff / 1.803e-10 = 5.8e3 (3.8 decades); the weakest real
direction, 1.124e-4 (a GPS L2W phase-bias direction), / cutoff = 107 (2.0 decades). The placement
rule of `RankGate.cpp` ("cutoff >= 2 decades above the gauge depth and >= 2 decades below the
weakest-real layer") is met, the upper margin only just. All 12 dumped sub-cutoff directions (2 on
each of 6 dumped epochs) are clock-flat; the dump covers 6 epochs, the count all 720. Run end
(line 4899): rank gate evaluated on 720 epochs at tol 1e-11, 720 emitted, 0 withheld
under-specified, 0 withheld over-specified, observed near-null count min 2, max 2. *Checked.*

**Source.** `rf_null_space_read.py` output; `runs/_rf0hv15_sim6/logs/network_estimability.csv`
rows 2 to 6; `runs/_rf0hv15_sim6/stdout.txt` lines 640, 4899;
`src/core/network/solve/RankGate.cpp` lines 408 to 410;
`docs/refound/RF-NS_null_space_read_2026-09-15.md` lines 35 to 48.

**Check.** The two commands of the question (9 s).

### Q1.4 One model, and a check every epoch

**Answer.** *May:* the user and the network use the same undifferenced, uncombined code and
phase equations (report equations su-code and su-phase); the user side runs one filter
(`include/filter/KalmanFilter.hpp`) in every step. On the simulated reference run the rank gate
found exactly the declared 2 near-null directions on 720 of 720 epochs, none withheld; the same
counts hold on the real-data run (`runs/_rf0hv15_field6/stdout.txt` line 5432) and on the current
software (`D:/GnssProcessorSupport/runs_nom/_rap_scb_g_q6/stdout.txt` line 4109). The two
directions are the per-system clock common modes.

*May not:*
- "one filter" for steps 6 and 7 without a qualifier: the network processor is a separate
  estimator, an information-form solve over the arrowhead normal equations
  (`include/network/solve/`); "one filter" holds for the user side.
- "a few reference choices": the first simulated epoch folds 2 clock pins, 112 ambiguity pins
  and 112 code-bias rows (226 rows) plus 120 receiver prior entries (Q2.3).
- that the check proves the reference choices correct or harmless: "It counts null
  directions; a row that pins something real removes a direction that was never null and the
  count still matches" (`SBASIS-CONTRACT.md` lines 87 to 88).
- that it covers each station's level direction: it sees only the shared matrix (Q2.5).
- that the count is free of a setting: it depends on `rank_gate_tol` 1e-11; the loader default
  1e-9 withholds 4 real-data epochs (ledger row `rank_gate_tol`).

**Source.** Status report lines 123 to 127 and 546 to 551; the stdout lines above;
`docs/CONFIG_promotion_ledger.json` (row `rank_gate_tol`).

---

## Module 2 · The S-basis and the four reference choices

### Q2.1 The theorem, and what is estimable before any choice

**Answer.**
1. An estimable function `fᵀx` (`f` in the range of `Aᵀ`) is "invariant in value AND variance
   under any admissible datum", that is, under every reference choice that removes exactly the
   null space.
2. Estimable before any choice: position, zenith tropospheric delay and vertical ionosphere
   (their rows of the null-space basis are zero). Not estimable: a satellite clock and a
   satellite phase bias; "The satellite clock does not exist as an estimable number; only a
   datum-dependent lump does."
3. The differential clock wander was estimable, so by the theorem no reference choice could
   move it; every datum comparison came out negative. "The cure must alter ESTIMATION (weights /
   dynamics / model / transmitted covariance), not the gauge." The coverage change that cut the
   wander from 47 to 14 cm was such an estimation-side change (§E5).

**Source.** `docs/cards/network-vehicle/SBASIS-THEORY-E.md` lines 40 to 41, 75 to 83, 116 to 120,
223 to 225.

**Check.** `sed -n '75,83p;116,120p' docs/cards/network-vehicle/SBASIS-THEORY-E.md`.

### Q2.2 Four kinds of reference choice

**Answer.** The verify prints, for each reference configuration, "76 recommended knobs armed in
regime, 2 unarmed (2 documented exceptions), 0 failure(s)" and "golden verify: PASS" (0.4 s).
*Checked.*

- **Pin** (one satellite clock per constellation): unaffected by satellites rising and setting
  until the pinned satellite leaves; then "a constellation-wide datum re-reference (handover),
  which the consumer pays for with an ambiguity reset". Integers are not its concern. Used: no
  key (the default), with `sticky_pivot` true (line 324) and `handover_stransform` true (line
  343); one handover per constellation in the 6 h run (E02 to E03, G01 to G02).
- **Prioritized anchor** (one ambiguity pin per satellite and signal, CC-R-like): an
  integer-ambiguity basis in theory, but a single point of failure; a handover steps the bias
  by whole cycles unless the successor is pinned at its own level. Used:
  `phase_datum_prioritized` (line 297) with `phase_datum_held_basis` (line 925); run end of the
  current software: 308 basis values elected, 22 of them cold (the emitted phase bias stepped,
  declared). The reference configuration's phase biases are nevertheless not integer for a user
  (module 5).
- **Sum-to-zero** (CC-S): set-dependent, "steps at every constellation-membership change",
  ranked worst for time stability; its ambiguities are not directly integer. Used only as
  code-bias sum-to-zero rows, which the simulated case suppresses because the pins already fix
  every channel (2,880 = 4 channels × 720 epochs).
- **Ensemble** (an inner constraint over receivers): anchor-free and handover-free, but needs
  the Eq. 34 integer recovery; its validation verdict was "SUPERSEDED BY MEASUREMENT" and the
  code was deleted on 2026-10-14 (commit 7914263).

Absent from the file: `clock_datum_sum_to_zero`, `clock_datum_broadcast_anchored`,
`pivot_election`; `phase_datum_eq34` is false (line 345).

**Source.** `SBASIS-THEORY-E.md` lines 143 to 150 and 239 to 259;
`include/network/solve/Sbasis.hpp` lines 162 to 166; `config/golden/golden_sim_refound.json` (the
lines named); `D:/GnssProcessorSupport/runs_nom/_rap_scb_g_q6/stdout.txt` lines 4385, 4400, 4845;
`git log -1 7914263`.

**Check.** `C:/Python313/python.exe scripts/make_golden_config.py --verify`;
`grep -n '"sticky_pivot"\|"handover_stransform"\|"phase_datum_prioritized"\|"phase_datum_held_basis"\|"phase_datum_eq34"' config/golden/golden_sim_refound.json`.

### Q2.3 Counting the rows of the first epoch

**Answer.**
1. 56 × (1 clock + 2 phase biases + 2 code biases) = 280 shared columns (line 631, `nshared=280`).
2. Clock pins 2 (E02, G01). Ambiguity pins 112 = 32 × 2 + 24 × 2, one per phase-bias column,
   placed on 18 stations. Code-bias rows 112: 56 ionosphere-free pins and 56 geometry-free pins
   at T_GD (54 at c·T_GD, 2 with a broadcast T_GD of 0), one per code-bias column; sum-to-zero 0.
   Receiver prior entries 120 = 60 stations × 2 constellations, each with information
   1/σ² = 1/(10 m)² = 0.01 m⁻², "INDEPENDENT of station count". Total on the shared state:
   2 + 112 + 112 = 226 rows, plus the 120 prior entries.
3. 280 columns less 112 ambiguity pins and 112 code-bias rows leave the 56 clocks, with one flat
   direction per constellation that no pre-gate row fixes; the clock pins act after the gate, so
   the gate expects 2. On a mismatch the gate throws; the heal soft-anchors near-null phase-bias
   columns (component above 0.3, σ 1 m) and re-gates once; if the count still differs the epoch
   is withheld and counted as under- or over-specified, with a `gate_withheld` event. S-basis
   completion would first fold one eigenvector row (weight 1e3) per extra separable near-null
   direction; it folded 0 directions on 0 of 720 epochs (line 4112). Run end (line 4109): 720
   evaluated at tol 1e-11, 720 emitted, 0 withheld either way, observed min 2, max 2. *Checked.*

**Source.** `D:/GnssProcessorSupport/runs_nom/_rap_scb_g_q6/stdout.txt` lines 631, 679 to 738,
4109, 4112, 4387; `include/network/solve/Sbasis.hpp` lines 50 to 55 and 1584 to 1674;
`src/core/network/solve/Sbasis.cpp` lines 764 to 781; `src/core/network/solve/RankGate.cpp` lines
193 to 305; `include/network/solve/SolveGlue.hpp` lines 13 to 27.

**Check.** `sed -n '631p;679,738p;4109,4112p' D:/GnssProcessorSupport/runs_nom/_rap_scb_g_q6/stdout.txt`.

### Q2.4 Do the published code biases carry an estimate?

**Answer.** First epoch: `B,G05,C1C,-3.2108400641001413` and `B,G05,C2W,-5.288075205569585`.
Ratio 1.646944 = (f1/f2)² = μ2, so the ionosphere-free combination
(f1²·B1 − f2²·B2)/(f1² − f2²) is zero. The G05 navigation record gives T_GD =
−1.071020960808E-08 s, and c·T_GD = −3.2108400641 m, equal to the C1C record. The pinned pair
therefore carries no estimate: its ionosphere-free part is pinned to zero and its geometry-free
part to the broadcast group delay (54 satellites at c·T_GD, 2 with a broadcast group delay of 0,
stdout line 631). With a slant ionosphere the satellite's true geometry-free code bias
is not estimable: it merges into the slant ionosphere of every line of sight to that satellite,
`ι̃ = ι + d_r,GF − d^s_GF`. The corrections carry no ionosphere record, so a user estimates its
own slant ionosphere, which absorbs the same geometry-free lump; code biases and ionosphere act
as a pair (module 3, Q3.1). *Checked.*

**Source.** `D:/GnssProcessorSupport/runs_nom/_rap_scb_g_q6/products.txt` lines 61 to 62;
`data/2024-129/products/BRDC00IGS_R_20241290000_01D_MN.rnx` line 568;
`docs/refound/RF-0c_gf_pin_2026-09-15.md` lines 22 to 23; `SBASIS-THEORY-E.md` lines 122 to 128;
`docs/cards/network-vehicle/SBASIS-CONTRACT.md` lines 114 to 133.

**Check.** The two commands of the question, then
`C:/Python313/python.exe -c "print(299792458*-1.071020960808e-08, -5.288075205569585/-3.2108400641001413, (1575.42/1227.60)**2)"`.

### Q2.5 The direction the shared check cannot see

**Answer.**
1. Information along the unit level from the prior: 0.0074/10² = 7.4e-5 m⁻²; σ = 1/√7.4e-5 =
   116 m.
2. (0.0074 + 0.2392² + 0.1334²)/0.0074 = (0.0074 + 0.0572 + 0.0178)/0.0074 = 11.1, about
   eleven times, σ about 35 m. Card R1D found such a row changes nothing at 10 m and at 1 m
   shrinks the move only by about a fifth while putting 0.95 m into C1W rows: "the datum row is
   not the fix".
3. σ = 1/√110 = 0.095 m. The carried ambiguity coupling supplies 109 of the 110 and the DCB prior
   7e-5: the coupling to the shared state holds the level, not the datum.
4. The direction lives inside one station's block, which is eliminated before the shared matrix
   is formed, so the shared gate never sees it. MAJU00MHL anchored phase biases of five GPS
   satellites (13 pins). With the pins held, the station's level plus a matching shift of its
   anchored phase biases is invisible to its data (residual 2.8e-11), so a move of the level
   reaches the published phase biases. In the GPS L5 event of TOW 260040 the level coordinate
   moved −21.92 m; barring the station from the anchor election removed the event (move 2 %, no
   clock decouples): "The anchor loop is the transmission". The hold card ALD derives is not
   built; GPS L5 stays sequenced.

**Source.** `docs/refound/R1D_station_level_datum_2026-10-23.md` lines 21 to 28, 54, 66 to 89,
99 to 122, 159; `docs/refound/ALD_anchor_level_datum_2026-10-23.md` lines 27, 31 to 41, 54, 58 to
65.

**Check.** `C:/Python313/python.exe -c "import math; print(1/math.sqrt(0.0074/100), (0.0074+0.2392**2+0.1334**2)/0.0074, 1/math.sqrt(110))"`
prints about 116.2, 11.14, 0.095.

### Q2.6 What may be said about reference choices

**Answer.** *May:* the reference choices are declared in the correction file: the S line names
the reference station, the clock pivots, the code-bias pin pairs, `sat_gf_datum=broadcast_tgd`,
the hold and (current software) the wind-up convention, and every change of pivot is marked by an
`E` record with `datum_changed=1`; the rank gate found exactly the declared 2 near-null
directions on 720 of 720 epochs, none withheld, on both reference cases.

*May not:*
- "a few" reference choices: 226 rows on the shared state plus 120 receiver prior entries at the
  first simulated epoch (Q2.3).
- that every choice is declared: the real-data S line carries only `cbpin=G:C1C-C2W`; the
  Galileo pin pairs are "NOT declared (per-satellite split)" (`runs/_rf0hv15_field6/stdout.txt`
  line 686), and no anchor station is named.
- that the check proves the choices correct: it counts null directions only
  (`SBASIS-CONTRACT.md` lines 87 to 91).
- anything about each station's level directions: the gate cannot see them, and they carried
  the L5 event (Q2.5).
- that the choices keep a user's integers: the emitted phase biases "are also not
  integer-recovering on the armed datum", and no integers are fixed on real data.

**Source.** Status report lines 442 to 443 and 546 to 551; `docs/refound/IRD_integer_recovering_datum_2026-10-13.md`
line 49; `docs/refound/FCFC_fixed_cycle_ledger_2026-10-11.md` line 211; Q2.3 to Q2.5.

---

## Module 3 · What a user cannot see

### Q3.1 Three things a float user absorbs

**Answer.**
1. Δ on every clock of system g enters every code and phase row of that system identically; the
   receiver clock (with the inter-system bias for a second system) takes it up. The geometry,
   hence the position, is unchanged.
2. A constant d on the phase bias of satellite s enters only that satellite's phase rows, as a
   constant: the float ambiguity moves, `N_s → N_s + d/λ`. The position is unchanged; only the
   ambiguity's meaning changes, which matters once a user fixes integers.
3. (δ, μ2·δ) adds μ_j·δ to code row j: exactly the ionosphere's signature on code, so
   `I_s → I_s + δ` absorbs it on code. In the phase rows that shift enters as −μ_j·δ, which the
   float ambiguities absorb. The position is unchanged; the user's slant ionosphere is biased by
   δ. The ionosphere-free combination of (δ, μ2·δ) is zero, so an ionosphere-free user sees
   nothing at all.
4. The per-system mean at each epoch (case 1, the receiver clock), then each satellite's mean
   over the session (case 2, the ambiguity). Case 3 does not arise for clocks.

The S-system statement is `SBASIS-THEORY-E.md` §E4: of the correction covariance the user uses
only the block-diagonal part; "the rank-one `[e;e]` (clock/geometry) and `[−μ;μ]` (iono) parts
lie in the user's own design range and are absorbed".

**Source.** `SBASIS-THEORY-E.md` lines 158 to 182; `SBASIS-CONTRACT.md` lines 98 to 102 and 114 to
133; status report lines 311 to 313, 342 to 344, 453 to 458; `results/t3/results.md` lines 64 to
66.

### Q3.2 The header line of reference choices

**Answer.**

| Field | Declares | Where it is defined or written |
| --- | --- | --- |
| `S30000SIM` | the run's reference (clock-datum) station; the pins act on satellite clocks, not on this receiver | `src/core/network/driver/ApplicationNetworkSetup.cpp` line 1485 |
| `1` | bias sign convention | `include/network/solve/Sbasis.hpp` lines 98 to 108 |
| `nominal-wu93` | phase wind-up model | `Sbasis.hpp` line 215 |
| `0` (5th) | ambiguity datum `PivotReceiverZeroAmbiguity`, "legacy single pivot and prioritized anchor" | `Sbasis.hpp` lines 129 to 132 |
| `0` (6th) | clock datum `PerConstellationSatClockPin`, one hard satellite clock pin per constellation | `Sbasis.hpp` lines 162 to 166 |
| `1`, `0` | the network emits its own code biases; code-bias datum id | `Sbasis.hpp` lines 229, 238 |
| `0.05`, `1` | clock discontinuity threshold (m); discontinuities emitted | `include/products/NetworkProductEmitter.hpp` lines 195 to 207 |
| `COD0MGXFIN_..._ORB.SP3+..._CLK.CLK` | the a priori orbit and clock; every C record is a correction to this clock | `ApplicationNetworkSetup.cpp` lines 1524 to 1566 |
| `E:E02`, `G:G01` | realized clock pivots (C = 0 at the first epoch) | `src/core/network/emit/SolPrep.cpp` lines 1176 to 1179 |
| `cbjump=1`, `atx=igs20.atx`, `oref=CoM` | code-bias jump test; antenna file; orbits at the centre of mass | `NetworkProductEmitter.hpp` lines 222 to 228; `ApplicationNetworkSetup.cpp` line 1578; `include/products/NetworkProductIO.hpp` lines 270 to 273 |
| `discfrac=0.25`, `hsig=pc` | phase jump gate of 0.25 cycle; σ0 of the H records from the prefit census | `NetworkProductEmitter.hpp` lines 238 to 248; `NetworkProductIO.hpp` lines 238 to 247 |
| `cbpin=E:C1C-C5Q`, `cbpin=G:C1C-C2W` | the ionosphere-free pin pair per system | `SolPrep.cpp` lines 1118 to 1157 |
| `sat_gf_datum=broadcast_tgd` | the geometry-free datum at c·T_GD | `SolPrep.cpp` lines 1165 to 1172 |
| `pbhold=2` | the phase-clock hold (module 4) | `ApplicationNetworkSetup.cpp` lines 1599 to 1601; `NetworkProductIO.hpp` lines 181 to 199 |

Reference choices of module 2: the two `0` datum ids, the pivots, the `cbpin` pairs and
`sat_gf_datum`; `pbhold` declares how the phase biases are held. A current run appends
`wumode=accumulated` (line 2 of `D:/GnssProcessorSupport/runs_nom/_rap_scb_g_q6/products.txt`).
The real-data S line declares only `cbpin=G:C1C-C2W`; the Galileo pin pairs are not declared
(`runs/_rf0hv15_field6/stdout.txt` line 686). A record such as `C,E04,0.257880637855868` is a
satellite and its clock correction in metres against the a priori clock.

**Source.** `src/core/products/NetworkProductIO.cpp` lines 465 to 602 (the writer) and the lines
in the table.

**Check.** `sed -n '2p' D:/GnssProcessorSupport/runs_nom/_rap_scb_g_q6/products.txt`.

### Q3.3 A declared datum step, seen and absorbed

**Answer.** The awk prints two records: line 134213, `E,2313,271230.00000715256,400,0,0,1,E`
(Galileo, 03:20:30) and line 189631, `E,2313,276149.99999547005,564,0,0,1,G` (GPS, 04:42:30). The
script prints "31 satellites, mean step 1.6278 m, sd after the mean 0.0027 m"; the same script
over an ordinary pair of epochs (276090 to 276120) gives a mean of −0.0001 m and an sd of
0.0008 m. A user's receiver clock absorbs the common 1.63 m (Q3.1, case 1); 3 mm per satellite is
visible, a few times the epoch-to-epoch noise. A grade of raw clock steps sees 31 satellites jump
1.6 m at once and calls it a defect; the datum-invariant step is 3 mm. On real data the GPS group
mean steps by about +154 cm at its pivot change. *Checked.*

**Source.** `runs/_rf0hv15_sim6/products.txt`; `results/t3/results.md` lines 105 to 107;
`include/network/solve/Sbasis.hpp` lines 162 to 166 (a re-reference "the consumer pays for with
an ambiguity reset").

**Check.** The two commands of the question (under 1 s).

### Q3.4 The reference satellite in every chord

**Answer.** L1C, all epochs: as traced, median |e| 0.304 cycle, 65.3 % round to the right
integer; reference-free 0.122 cycle, 81.6 %. Last quarter: 0.260 cycle and 69.5 % against 0.092
cycle and 86.5 %. With a chord `e_i = s_i − s_ref`: Var(e_i) = σ² + σ_ref²; Cov(e_i, e_k) =
σ_ref², so the correlation between two chords is σ_ref²/(σ² + σ_ref²), 0.5 when σ_ref = σ. The
reference satellite's error and its returns enter every chord, so a grade on the traced chords
reports that one element's share; the card's rule is "Quote DD grades ref-free from now on." A
second trap of the same card: subtracting a group's own circular median shrinks uniform noise
from 0.251 to 0.145 cycle, so a wrapped grade is quoted as a gap to a size-matched null.
*Checked.*

**Source.** `docs/cards/sim-bed/DD-TRACE-REF-SAT.md` lines 41 to 48, 89 to 91, 107 to 110, 143 to
159; `CLAUDE.md` ("Grading traps that still apply").

**Check.** The command of the question (12 s).

### Q3.5 May the presentation say that a user cannot see the reference choices?

**Answer.** *May:* a float user cannot see them: the clock common mode, a constant per-satellite
phase-bias offset and a geometry-free code-bias offset are absorbed (Q3.1). Evidence: the GPS
datum step of 1.63 m leaves 3 mm after the per-system mean (Q3.3); cases C and D of the PPP
comparison agree to 0.1 cm in every hour 2 to 6 at WTZZ00DEU, as the theory predicts for
constant biases (`results/t1/results.md` lines 56 to 57; whether case D applied CODE's phase
biases is not established, and the argument does not depend on it).

*May not:*
- that a user is blind to everything the network does: a satellite-specific error that changes
  in time is not absorbed; with the phase-clock hold a wrong a priori clock reaches the phase
  rows one to one (card SCB §8: 0.2 to 0.34 m on the clock data set, module 4).
- that a step is harmless: a float ambiguity absorbs only a constant, so every change of
  reference is declared (`E` records with `datum_changed=1`, hold generations) and a user resets
  across it.
- anything for a user that fixes integers: the integer property depends on the phase-bias datum
  (the Eq. 34 gate, `SBASIS-THEORY-E.md` lines 239 to 249), and no integers are fixed.
- that the simulated 0.9 cm agreement certifies the clock: it "certifies the algebra and the
  biases, not the clock" (card SCB line 19).

**Source.** As named; `docs/refound/SCB_sim_clock_error_bed_2026-10-24.md` lines 19 and 111 to 145.

---

## Module 4 · The satellite clock

### Q4.1 A spectral density read as a step and as a time constant

**Answer.**
1. √(2.9e-8 × 30) = √8.7e-7 = 0.933 mm per epoch; √(2.9e-8 × 21,600) = 2.50 cm over 6 h. The
   value is measured: "q_phys ≈ 2.9e-8 m²/s → 0.94 mm per 30 s" from the structure function.
2. Drift after n epochs: 0.30 mm × n; the walk's 1σ: 0.933 mm × √n. Equal at
   n = (0.933/0.30)² = 9.7 epochs, about 5 min; at 6 h the drift is 21.6 cm, 8.6 times the walk.
   A step standard deviation is compared with a signed, persistent increment: a drift grows as n,
   a random walk's spread as √n, so the anchor cannot follow a sustained rate. Card SCB reads the
   outcome as partial tracking.
3. R = 0.0923²/2 = 4.26e-3 m²; K = √(8.7e-7/4.26e-3) = 0.0143; Δt/K ≈ 2,100 s, about 35 min (the
   exact steady state of the scalar filter gives 2,084 s). A 1e-5 m/s drift leaves
   1e-5 × 2,100 ≈ 2.1 cm. Card SCB measured a coefficient of 2,127 s for the reference
   configuration (2,000 s GPS only, 52 s for the self-contained regime, whose q of 5e-6 m²/s
   gives about 150 s by the same argument): q sets the time constant, within about 2 %. Two
   cautions. The argument is scalar and stationary; the real filter is neither. The grader's
   error is estimate minus truth and the coefficient is positive, which a lag would make
   negative; the card calls it a lag and does not explain the sign, so the safe wording is "an
   error proportional to the drift rate, with a coefficient of about 35 min". *Checked
   (arithmetic).*

**Source.** `config/golden/golden_sim_refound.json` line 323; `include/network/solve/ClockAnchor.hpp`
lines 35 to 36; `docs/cards/clock/Q3-ANCHOR-DESIGN.md` lines 47 to 56 and 100;
`docs/cards/clock/Q3-PHYSICAL-FLOOR.md` lines 29 to 32; card SCB lines 29 to 35, 124, 128;
`docs/cards/raim-qc/DAY129-FRAME-ANATOMY.md` lines 139 to 143; `scripts/simnet_clock_truth_grade.py`
line 151.

**Check.**
`C:/Python313/python.exe -c "import math; q,dt,R=2.9e-8,30,0.0923**2/2; K=math.sqrt(q*dt/R); print(1e3*math.sqrt(q*dt), math.sqrt(q*21600), (math.sqrt(q*dt)/3e-4)**2, K, dt/K, 1e-5*dt/K)"`.

### Q4.2 What the phase-clock hold passes to the user

**Answer.**
1. C − P = c(t) − p(t0) − [c̃(t) − c̃(t0)] = (c − p)(t0) + M(t) − M(t0): constant per satellite up
   to the system mean M, which the receiver clock absorbs. The user applies the clock a(t) + C(t)
   on code, but on phase it sees a(t) + C(t) − P(t) = a(t) + a constant: the a priori clock's
   motion, not the network's estimate. With truth a(t) + err(t), the phase rows are wrong by
   −[err(t) − err(t0)] plus a constant, which the float ambiguity absorbs; the time-varying part
   passes one to one.
2. Mean square over hour 6 (t from 18,000 to 21,600 s after the reset): rate part
   σ_r²·(t_hi³ − t_lo³)/(3·3,600) = 1e-10 × 3.931e8 = 0.0393 m²; walk part
   q·(t_lo + t_hi)/2 = 3e-8 × 19,800 = 0.0006 m²; total 0.0399 m², rms 0.200 m. Card SCB §8
   measured 0.201 m (GPS) and 0.206 m (Galileo). The user carries 0.2 to 0.34 m per hour on the
   clock data set (Q4.4). *Checked (arithmetic).*

**Source.** `include/network/emit/PbHold.hpp` lines 60 to 66; `docs/refound/LAH_level_aware_hold_2026-09-29.md`
lines 30 to 36 and 63 to 64; card SCB lines 29 to 35, 92, 129 to 145.

**Check.** `C:/Python313/python.exe -c "print((1e-10*(21600**3-18000**3)/(3*3600)+3e-8*(18000+21600)/2)**0.5)"`
prints about 0.1998.

### Q4.3 Where the hold's precondition is enforced

**Answer.** Lines 311 to 315 of `GnssConfigLoadNetwork.cpp` throw when `network.apriori_source` is
`sp3_brdc_clock` and `network.emit_pb_hold_phase_clock` is on: "the phase-clock hold is valid only
with a precise a priori clock product". Lines 316 to 319 require, for the same source,
`network.emit_pb_hold` with `network.emit_pb_hold_smoothed` ("the smoothed hold, the hold the
posture was measured with"). With `brdc_rts` the loader only warns (lines 2612 to 2617): the
phase-visible clock motion is then the broadcast clock's, "which drops the network's real clock
corrections (decimetres per hour)". The S-line token is `pbhold=`: 2 in the reference cases (the
phase-clock hold), 3 in the self-contained regime and native cases (the smoothed hold). The cost of
ignoring the precondition, on real data: CODE with the phase-clock hold gave the user 0.108 and
0.024 m in hours 1 and 2; the broadcast clock with the same hold 1.191 and 0.743 m. *Checked
(code lines).*

**Source.** `src/core/config/GnssConfigLoadNetwork.cpp` lines 311 to 319 and 2612 to 2617;
`docs/refound/SCL_self_contained_clock_level_2026-10-03.md` lines 22 to 26 and 76 to 78; card SCB
line 10.

**Check.** `sed -n '311,319p;2612,2617p' src/core/config/GnssConfigLoadNetwork.cpp`.

### Q4.4 Two postures on the clock data set

**Answer.** Reference case `_rap_scb_g_k6`: clock level at hour 6 0.0533 m; per-satellite mean
over hours 2 to 6, rms 0.0426 m over 55 satellites; correlation of the mean error with the drawn
rate +0.524; user median by hour 0.1976, 0.2012, 0.3189, 0.2142, 0.2370, 0.3376 m. Self-contained
regime `_rap_scb_c_k6`: level 0.0232 m; rms 0.0205 m over 31 satellites; correlation +0.026; user
0.2287, 0.1296, 0.1003, 0.0211, 0.0311, 0.0260 m. The regime follows the drift (no correlation
with the rate); the reference case does not. Its user's error is ten times its clock error because
the hold passes the a priori clock's error to the phase rows whatever the estimator tracks (Q4.2).
Not shown: one realization of an error of chosen statistics; the sign question of Q4.1; the regime
case is GPS only (31 satellites against 55), so the two rms values are not like for like; neither
the regime nor the native posture is armed. Card SCB's text says the regime and native users "do
not move" between the two data sets; its own tables show the native user up to 2.1 cm higher on
the clock data set in hours 1 to 3. *Checked.*

**Source.** The two grader outputs; card SCB lines 8, 10, 122 to 145.

**Check.** The two commands of the question (about 2 s each).

### Q4.5 What step 5 may say about the satellite clock

**Answer.**
1. *May not.* Against GFZ the network's corrected clock is 4.3 cm (GPS, 01:00 to 06:00) and 7.6 cm
   (Galileo); the IGS real-time service 4.2 and 3.2 cm over 13:24 to 24:00, other hours; the
   correction c0 is uncorrelated with CODE's error (|r| ≤ 0.18). The report already says the
   network does not beat the real-time clocks.
2. *May not* for the reference configuration: the quiet data set has no clock error (its truth
   clock is the a priori clock), so its 0.9 cm agreement says nothing about the clock; the
   published simulated baseline's clock carries 0.32 m per-satellite constants from the start
   artifact; with injected error the clock level reads 5.3 cm at hour 6 and the user is off by
   0.2 to 0.34 m. *May, scoped:* the self-contained regime and the native broadcast-clock posture
   estimate the clock to 2.3 and 1.7 cm at hour 6 on one realization of an injected error, and
   neither is armed.
3. *May* for the effect, not the wording: the armed hold (`pbhold=2`) keeps clock minus phase
   bias constant per satellite, while the phase-bias record itself moves with the estimated clock
   (`P(t) = p(t0) + [c̃(t) − c̃(t0)]`). The report's wording ("keeps each satellite's published
   phase bias constant", lines 491 to 493) describes the earlier plain hold of card PBH. The
   conclusion, that the user's phase sees CODE's clock, stands.
4. *May only with its scope:* the native broadcast-clock posture against the GPS-only reference
   configuration of 2026-10-07, days 130 to 132, 16 stations, median paired change of the hours 2
   to 6 error, +1.71 cm; 40 % of station-days worse than 2 cm; hour 1 +7.7 cm; day 129 +0.3 cm;
   the clock's own share about 1.0 cm (Q7.4).
5. *May not:* no self-contained posture is armed (ledger UNVALIDATED or KEEP_OFF), and the
   reference configuration's hold requires a precise a priori clock (Q4.3).

**Source.** `results/t3/table_clock_report.tex`; `results/t3/results.md` lines 100 to 113; card
SCB lines 8, 19, 107 to 109, 122 to 145; `docs/refound/NBP_native_broadcast_clock_posture_2026-10-07.md`
lines 170 to 190 and 212 to 219; status report lines 486 to 497; `include/network/emit/PbHold.hpp`
lines 60 to 66.

---

## Module 5 · Float ambiguities and integers

### Q5.1 Why "99.3 % ionosphere"

**Answer.** λ1 = c/f1 = 0.190294 m, λ2 = 0.244210 m, μ2 = (77/60)² = 1.646944.
cos = (λ1 + μ2·λ2)/(|(1, μ2)|·|(λ1, λ2)|) = 0.592494/(1.926765 × 0.309597) = 0.99325; the angle
is 6.66°. The part orthogonal to the ionosphere is 0.3096 m × sin 6.66° = 0.0359 m (3.6 cm) of the
0.31 m step. "99.3 %" is the cosine, a share of amplitude along the ionosphere direction; as a
share of variance (cos²) it is 98.7 %. The metric assumes equal phase noise in metres on both
bands. *Checked.*

**Source.** `docs/refound/ARL_integer_ambiguity_ladder_2026-10-09.md` line 43 (the statement); the
arithmetic above.

**Check.** `C:/Python313/python.exe -c "import math; c=299792458; l1,l2=c/1575.42e6,c/1227.60e6; m=(1575.42/1227.60)**2; k=(l1+m*l2)/math.hypot(1,m)/math.hypot(l1,l2); print(k, math.degrees(math.acos(k)), math.hypot(l1,l2)*math.sin(math.acos(k)), k*k)"`.

### Q5.2 An ionosphere error in cycles

**Answer.** Cycles per metre of L1 ionosphere: L1 1/λ1 = 5.255; L2 μ2/λ2 = 6.744; wide lane
5.255 − 6.744 = −1.489 (`T3-IONO-MECHANISM.md`: L1C 5.255, L2W 6.744). d = 0.4233 m: L1 2.22
cycles, wide lane 0.63 cycle. d = 0.0419 m: L1 0.22 cycle, wide lane 0.062 cycle. 0.1 L1 cycle =
0.0190 m; 1 TECU = 40.3e16/f1² = 0.1624 m of L1 delay, so 0.1 cycle = 0.117 TECU. The no-pin error,
0.4233 m = 2.61 TECU, is 22 times that: "2.61 TECU, 22× the 0.117 TECU that moves a fraction".
0.4233 m is a per-line-of-sight rms of the double-difference-invariant row, so a double difference
of several such terms can be larger. *Checked.*

**Source.** `docs/cards/raim-qc/T3-IONO-MECHANISM.md` lines 326 to 331; `docs/SPEC_oracle_pin.md`
lines 514 to 517 and 541 to 546.

**Check.** `C:/Python313/python.exe -c "l1=299792458/1575.42e6; k1=1/l1; k2=(1575.42/1227.60)**2/(299792458/1227.60e6); print(k1, k2, k1-k2, 0.4233*k1, 0.4233*(k1-k2), 0.1*l1/(40.3e16/1575.42e6**2))"`.

### Q5.3 Complete and partial pins

**Answer.** Row 1b, the double-difference-invariant per-line-of-sight L1 slant ionosphere error,
carries the deficit: 0.4233 m with no pin, 0.0419 m with every ambiguity pinned (10.1 times
smaller; 2.61 to 0.26 TECU). "The `none` column's row 1b IS the integer-information deficit".
The raw per-line-of-sight error (2.73 m) is about 97 % station and satellite lumps, which the
reference choice and the user absorb, so it cannot measure integer information. Partial pins do
not interpolate: L5Q alone 0.2793 m (better); L1C with L2W 0.5632 m (worse than no pin, although
every double difference is then integer); one station 0.4476 m; one satellite 0.4741 m.
"Integer information is worth having only when it is complete on the LOS." The proposed cause
(the bands of a line of sight share one ionosphere state, so exact integers on a subset
over-determine it) is called a consistent reading, not a proof. The numbers come from a 60-epoch
warm resume on `simnet60_quiet_wu93_24h`, a data set whose numbers `CLAUDE.md` marks superseded:
the reading stands, the values are not comparable with today's simulated data. *Checked (lines
514 to 546).*

**Source.** `docs/SPEC_oracle_pin.md` lines 20 to 29, 193 to 195, 514 to 517, 534 to 546, 558 to
568, 611 to 627, 654 to 663; `CLAUDE.md` ("Conventions shared end to end").

### Q5.4 Integers in the float, none in the corrections

**Answer.** First command (4 s): the published GPS phase biases' wide-lane median |fraction| by
hour from 00:30:15 is 0.254, 0.260, 0.259, 0.255, 0.253, 0.252, the narrow lane 0.264 to 0.266,
against a uniform null of 0.250 (band about 0.22 to 0.27); a set of perfect corrections reads
0.000. Second command (4 s): "CORRECT-FIX RATE among kept: 5572/5572 = 100.00%", "WRONG FIXES:
none". The integers are in the network's float solution (a search on its float states and
covariance fixes 42 % of the settled double differences of that case, all correct), but the
published phase biases carry none of it: they sit at the uniform null because each carries an
arbitrary receiver's float ambiguity, and the reference choice is not integer-recovering. A user
cannot fix integers from these corrections. *Checked.*

**Source.** The two outputs; `docs/refound/ARL_integer_ambiguity_ladder_2026-10-09.md` lines 98 to
102; `docs/network_ambiguity_resolution_design.md` lines 335 and 352 to 356;
`docs/refound/IRD_integer_recovering_datum_2026-10-13.md` lines 19 and 49.

**Check.** The two commands of the question.

### Q5.5 What may be said about integer ambiguities

**Answer.** *May:* on one simulated case, a search on the network's float states (written to the
log, never published) fixed 42 % of the settled single-signal double differences (5,572 of
13,138), none wrongly; the wide lane is integer-capable in the float. Its scope: simulation, a
star basis, a 2 h case of card ARL without the reference configuration's phase colour (18.5 % with
it), the settled hour, one realization; "zero wrong fixes are the measurement, not a guarantee".

*May not:*
- attach the 42 % to the reference configuration or its corrections: they are float
  (`ambiguity_vehicle` `float_kf`, `estimation.ambiguity_resolution.mode` 0) and the published
  phase biases sit at the uniform null (Q5.4).
- say that fixing integers improves the corrections: the fixed-cycle fold moved the test station
  by 0.2 %, and the emitted biases are "not integer-recovering on the armed datum".
- present producer-side fixing as built or planned: the standing verdict is DO NOT BUILD.
- claim integers on real data: "No integers are fixed".
- claim that PPP-AR with outside corrections works: the 30 July check fixed wrong integers, "the
  default setting hides the wrong integers", and the 1.3 cm of the ALGO00CAN run "is therefore
  not evidence of correct integers".

The report's "eligible" needs a definition: card ARL's denominator is 13,138 star-basis double
differences.

**Source.** ARL lines 98, 102, 142, 180; `docs/refound/FCFC_fixed_cycle_ledger_2026-10-11.md` lines
208 to 215; `docs/network_ambiguity_resolution_design.md` lines 3 to 5; status report lines 348 to
355, 563 to 565, 653.

---

## Module 6 · The simulated network

### Q6.1 What the quiet data set cannot test

**Answer.** The quiet data set's truth clock is the CODE final clock, and the network takes the
same product as its a priori clock, so the estimable satellite clock truth is identically zero
(card SCB §1). `results/t2/results.md` confirms it: no `sat_clock` key in the generator's
configurations, no ARMED line, no `truth_satclock.csv`. The two `ls` commands print 0 and 61. The
0.9 cm agreement therefore "certifies the algebra and the biases, not the clock": a simulation
grades what it injects, plus contrasts in which every reused term cancels. *Checked (file counts).*

**Source.** Card SCB lines 17 to 25; `results/t2/results.md` lines 123 to 126;
`docs/cards/sim-bed/SIMNET-REF.md` lines 269 to 272.

**Check.** The two commands of the question.

### Q6.2 Where a simulated run starts

**Answer.**
1. 0.504167 h = 1,815.0012 s; epoch k sits 30k s after the first, and the first k with
   30k ≥ 1,815.001 is 61 (1,830 s): tow 259,230 + 1,830 = 261,060, 00:31:00. 0.4958333333 h =
   1,785 s gives k = 60: tow 261,030, 00:30:30 (the T2 restart). Each offset sits 15 s, half an
   epoch, below the epoch it selects, so a rounding of the offset cannot move the start by an
   epoch. The grading origin 00:30:15 (tow 261,015) is not a start epoch: the reference case
   starts at 00:00:30 and is graded from 00:30:15; a case with the offset starts at 00:31:00
   (the first row of its `logs/closed_loop_trace.csv` is tow 261060). A replay re-applies the
   offset from the corrections' first epoch.
2. Output: 259230 367, 259260 366, 259290 366, 259320 367, 259350 309 (warnings per epoch): 5
   epochs logged. The warning needs more than 150 s outside the window, and the window opens at
   tow 259,500, so 259,380 to 259,470 are extrapolated without a warning: 9 epochs, 00:00:30 to
   00:04:30. "Faulty in their first 30 minutes" describes the grading origin chosen inside a
   clean window, not the extent of the fault; what lasts is the effect (the state those epochs
   leave, Q6.3).

**Source.** `include/math/TimeUtils.hpp` lines 416 to 448; `docs/cards/queues/QPR-ITEMS-5-8.md`
lines 239 and 256; `src/core/model/ModelGeometry.cpp` line 1204;
`docs/refound/CTU2_level_recursion_followup_2026-09-29.md` lines 29 to 43;
`docs/refound/SCL2B_shared_history_with_the_hold_2026-10-07.md` lines 188 to 190; status report
lines 611 to 613.

**Check.** The grep pipeline of the question (0.5 s); `head -2 D:/GnssProcessorSupport/runs_nom/_rap_scb_g_q6/logs/closed_loop_trace.csv`.

### Q6.3 The published baseline's clock in the network's basis

**Answer.** Baseline (from 00:00:30): level by hour 0.7087, 0.3487, 0.3303, 0.3157, 0.3096,
0.3108 m; per-satellite mean over hours 2 to 6, rms 0.3179 m over 55 satellites (G06 −1.198,
E05 +0.872, E36 −0.846). Card SCB's quiet case (from 00:31:00): level 0.0809, 0.0291, 0.0226,
0.0202, 0.0193, 0.0185 m; rms 0.0186 m. With `--no-ds` the baseline's rms reads 0.7784 m. The
datum share is the ionosphere-free combination of the truth code biases over the pinned pair
(GPS C1C/C2W, Galileo C1C/C5Q): the network's clock contains it by construction of the code-bias
datum (Q2.4), so it belongs to the truth expressed in the network's basis. Truth is transformed;
differencing the estimates instead would bias the variance factor downward, because the
estimates share the datum. The user of the baseline does not show the 0.32 m: a float user cannot
see a per-satellite constant (module 3). *Checked.*

**Source.** The grader outputs; card SCB lines 87, 102, 107 to 109;
`scripts/simnet_clock_truth_grade.py` lines 138 to 153; `docs/cards/sim-bed/SIMNET-REF.md` lines
276 to 283 and 293.

**Check.** The two commands of the question, the first again with `--no-ds` (about 2 s each).

### Q6.4 The network's cost and the user's floor

**Answer.** Ours minus perfect, m: +0.0030, +0.0026, +0.0085, −0.0007, +0.0005, +0.0002. The
largest cost is 0.85 cm, in hour 3 (the report's "at most 0.9 cm"); hours 4 to 6 show nothing
measurable. The floor (perfect corrections, hours 4 to 6) is 1.73, 1.81 and 1.76 cm, in the
height: median |U| 1.54, 1.54, 1.65 cm against |E| ≤ 0.46 cm and |N| ≤ 0.55 cm. Hour 6 is 30 min
long, because the network run ends at 06:00.

**Source.** `results/t2/results.md` lines 195 to 206; `results/t2/metrics.csv`; status report lines
595 to 605.

### Q6.5 What step 6 may say

**Answer.** *May:* on one quiet 6 h simulation (no slips, no clock error) and one test station, a
user restarted at 00:30:30 differs from a user of perfect corrections by at most 0.9 cm per hour,
and the 1.7 to 1.8 cm left in the height appears with both sets, so it belongs to the user side;
the perfect corrections were checked against the simulation's integers on every signal.

*May not:*
- carry the 0.9 cm to a wrong a priori clock, or say that the network estimates the clock there:
  the quiet data set has none (Q6.1), and with injected error the reference configuration's user
  is 0.2 to 0.34 m off (module 4).
- describe the first 30 minutes as faulty: 9 epochs are extrapolated (Q6.2); and the hour-1
  reversal is not explained (its cause was not tested).
- print the 42 % integer result as a property of this case: it comes from card ARL's 2 h case
  without phase colour, and the published corrections carry no integers (module 5).
- give a spread: one realization, and no spread over seeds exists.
- mix the two gradings: a run started at 00:00:30 and graded from 00:30:15 differs from a run
  started at 00:31:00 (per-satellite clock constants 0.32 against 0.019 m, Q6.3).

**Source.** Status report lines 560 to 614; `results/t2/results.md` lines 186 to 189; Q6.1 to Q6.4.

---

## Module 7 · Real data and the claims

### Q7.1 The test station and the 16 stations

**Answer.** `rapid_arm.py score` (under 1 s): `_rf0hv15_field6` median error by hour 0.06, 0.01,
0.02, 0.01, 0.02, 0.01 m, 688 epochs below 10 cm; `_rf0hv15_sim6` 0.14, 0.07, 0.03, 0.02, 0.03,
0.02 m, 595. `gcensus.py grade` (1 to 14 s): `galp` (the GPS + Galileo corrections), 16 stations,
median by hour 0.136, 0.054, 0.041, 0.036, 0.040, 0.037 m, WTZZ 0.058, 0.011, 0.018, 0.010, 0.015,
0.011 m; paired against `galpc` (GPS only): hours 2 to 6 median −0.0198 m, 10 better, 2 worse
(HARB +0.0158, DAV1 +0.0605), hour 1 −0.2203 m; `galpg` (GPS-only users of the GPS + Galileo
corrections): median −0.0000 m, ARHT +0.1958 m. Better and worse mean a change of more than 1 cm.
The first hour: WTZZ00DEU 5.8 cm against a mean of 1.3 cm over hours 2 to 6 (4.5 times); the
median station 13.6 against 4.2 cm (3.3 times). Mode: `position_q` 100 (moving receiver),
`ar_mode` 0 (no integer fixing). None of the 17 user stations is in the 58-station network, but
WTZZ00DEU shares its site with two network stations, WTZR00DEU and WTZS00DEU ("the colocated
consumer hold-out"; its RINEX header position is about 3 m from WTZR00DEU's); whether that
explains its rank has not been tested. *Checked.*

**Source.** The outputs; `runs/_rf0hv15_field6/arm.json`; `scripts/clock_level/gcensus.py` lines
151 to 153; `docs/refound/GAD_galileo_adoption_2026-10-13.md` lines 70 to 75;
`config/golden/golden_field_refound.json` (`network.stations`);
`docs/refound/RF-PD_station_rejection_2026-09-15.md` line 87.

**Check.** The two commands of the question;
`C:/Python313/python.exe -c "import json; s=json.load(open('config/golden/golden_field_refound.json'))['network']['stations']; n=[x['name'] if isinstance(x,dict) else x for x in s]; print(len(n), [x for x in n if x.startswith('WTZ')])"`
prints `58 ['WTZR00DEU', 'WTZS00DEU']`.

### Q7.2 Five days from the stored grades

**Answer.** Hours 2 to 6 of days 128 to 132: the 25 sixteen-station medians have a median of
0.038 m and a mean of 0.0406 m; the 25 WTZZ00DEU values have a median of 0.018 m. The report's
1.8 cm for WTZZ00DEU reproduces; its 3.9 cm for the 16 stations does not reproduce from the stored
file (3.8 as a median of medians, 4.1 as a mean), so the report should name the statistic it
quotes. Day 130: WTZZ00DEU reads 3.9, 5.9 and 5.3 cm in hours 4 to 6 against the median station's
3.5, 3.2 and 3.8 cm. "The best station" holds on 8 May 2024 (day 129), not on every day. *Checked.*

**Source.** `D:/GnssProcessorSupport/runs_nom/mday_galileo_grades.txt`; status report lines 631 to
633 and 650 to 652.

**Check.**
```
C:/Python313/python.exe - <<'EOF'
import re, statistics as st
cen, wtz = [], []
for l in open('D:/GnssProcessorSupport/runs_nom/mday_galileo_grades.txt'):
    m = re.match(r'== mdg\d+c: .*by hour ([\d. ]+); WTZZ ([\d. ]+)', l)
    if m:
        cen += [float(x) for x in m.group(1).split()[1:]]
        wtz += [float(x) for x in m.group(2).split()[1:]]
print(len(cen), st.median(cen), round(st.mean(cen), 4), st.median(wtz))
EOF
```
prints `25 0.038 0.0406 0.018`.

### Q7.3 The clock comparison with CODE and GFZ

**Answer.** Against the GFZ final clock, per-system epoch mean and per-satellite mean removed
(STD): our network (CODE final plus c0) GPS 4.3 cm and Galileo 7.6 cm over 01:00 to 06:00 (6.4
and 11.6 cm with the first hour; Galileo RMS 15.8 and 25.3 cm because of E12); CODE final 1.4 and
2.6 cm over 00:00 to 06:00, 1.9 and 3.3 cm over 13:24 to 24:00; the IGS real-time service 4.2 and
3.2 cm over 13:24 to 24:00, the only hours archived. GFZ is the reference because the network's a
priori clock is CODE's: against CODE the network would be measured partly against itself. The
report's "4.3 and 7.6 cm, against 1.4 and 2.6 cm" sets 01:00 to 06:00 against CODE over 00:00 to
06:00; like for like, CODE over 01:00 to 06:00 reads 1.20 and 2.23 cm (`clock_stats.csv`).
Correlations of c0 with (final minus CODE): GFZ GPS −0.08, Galileo +0.01; ESA GPS −0.18, Galileo
+0.01. c0 carries no detectable information about CODE's error; relative to its a priori it
behaves as added noise of 6.3 cm (GPS) and 4.6 cm (Galileo without E12). A standalone clock
comparison does not isolate what a user obtains: the user's phase term is phase bias minus clock,
which the hold keeps at CODE's clock. *Checked (`clock_stats.csv`).*

**Source.** `results/t3/table_clock_report.tex`; `results/t3/results.md` lines 9 to 14, 100 to 113
and §3.4; `results/t3/clock_stats.csv` (rows `COD,GFZ,01-06`).

**Check.** `grep "COD,GFZ,01-06" D:/GnssProcessorSupport/presentation/status_update_2026-10-29/results/t3/clock_stats.csv`;
the optional correlation script (20 s).

### Q7.4 What "1.7 cm worse without the final clock" contains

**Answer.** 0.0024 − 0.0007 + 0.0099 = 0.0116 m, not 0.0171 m: the steps add exactly for each
station-day, but the median of a sum is not the sum of the medians. The clock's information is the
broadcast-clock step, about 1.0 cm; the regime costs 0.24 cm and removing G01 gains 0.07 cm. The
1.7 cm is the whole native posture against the GPS-only reference configuration of 2026-10-07 (the
reference configuration is GPS + Galileo since 2026-10-13), over days 130 to 132 (9 to 11 May
2024), 16 stations, as the median paired change of each station's hours 2 to 6 error; 19 of 48
station-days (39.6 %) are worse by more than 2 cm; day 129 is its most favourable day (+0.3 cm);
hour 1 adds +7.65 cm.

**Source.** `docs/refound/NBP_native_broadcast_clock_posture_2026-10-07.md` lines 170 to 177, 184 to
190 and 210 to 219; card GAD; status report line 495.

### Q7.5 The three messages for the meeting

**Answer.**
- **Message 1** (one model and one filter; nothing re-implemented; only inputs and a few settings
  change). *Supported:* one observation model for user and network; one user filter (Q1.4).
  *Qualify:* the network processor of steps 6 and 7 is a separate estimator, and it runs from a
  generated reference configuration (76 recommended settings armed), so "a few settings" holds
  for the user steps.
- **Message 2** (outside corrections fall short on phase biases and atmosphere; GPS real-time
  clocks twice as noisy as final; our network publishes a phase bias for every signal every
  epoch and states its reference choices; within 0.9 cm of perfect corrections; clock precision
  from a final product). *Supported:* real-time GPS 4.2 against CODE's 1.9 cm over the same hours;
  phase biases for 61 to 81 % of satellite-epochs on the live stream, none in the archive, every
  signal every epoch in ours; the final clock as the source of our clock precision. *Qualify:*
  "states its reference choices": the real-data S line omits the Galileo pin pairs (Q2.6); "within
  0.9 cm": one quiet simulation without clock error, user from 00:30:30 (Q6.5). *Add:* the
  correction c0 adds code noise and no clock information, and the user's phase sees CODE's clock
  through the hold (Q4.5, Q7.3).
- **Message 3** (1 to 2 cm at the best station, 3.6 to 5.4 cm at the median of 16 after the first
  hour, no integers; integers, final-free clocks and atmosphere next). *Supported* for 8 May 2024:
  WTZZ00DEU 1.0 to 1.8 cm, median 3.6 to 5.4 cm in hours 2 to 6 (Q7.1). *Qualify:* "best station"
  on that day only (Q7.2), and WTZZ00DEU shares its site with two network stations, so the median
  of the 16 is the typical number, as the report's own limits box says; "our own corrections"
  still take CODE's orbits and final clock. *Do not say:* that the 42 % simulated, log-only
  integer search is progress on a user's integers (Q5.5).

**Source.** Status report lines 123 to 140; the answers named.
