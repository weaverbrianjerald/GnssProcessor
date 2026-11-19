# XBD: the datum of a third frequency band in the network (2026-11-19)

**STATUS**

- **id:** XBD
- **topic:** network-vehicle
- **status:** open
- **summary:** Derivation and exact-rank proof of the minimal hard rows that complete the datum of the triple-frequency network posture, an attribution of the third-band drift in the DOY 144 and DOY 145 runs, and a default-off key with its pre-registered bars (a second key withdrawn before its build, section 11). Under the engine's hard rows the triple posture leaves one free direction per (receiver, system), the receiver geometry-free direction, plus one per extra code signal and connected component beyond the first; one DCB row per (receiver, system) at every receiver, chain roots included, and one level row per component close them as a pure datum, on a toy network in rational arithmetic and on the real `_rap_k1t_n144` roster over two prime fields. The attribution does not support the reading that the decimetre drift of the receivers' third-band delays is a datum defect: it lies in the estimable receiver quantity, and the satellites' metre-scale sums move by membership only. The datum defects that remain are the repeated DCB prior (false precision of every receiver DCB and extra-band sigma) and the receiver direction at the chain roots, which couples to the phase datum, both on dual frequency too. An independent check (section 11) confirms the attribution, narrows three numbers, withdraws the split C5Q graph (it consisted of zero-weight rows) and finds one more direction, of the two separate Galileo families of the `f13` setup, outside both keys. The third GPS band stays SEQUENCED.
- **source:** day program 2026-11-19, package systems-extra-band-datum (`D:/GnssProcessorSupport/gates/day_program_2026-11-19/program.json`); [JNM](JNM_joint_network_mathematics_2026-11-18.md) sections 2, 5, 6, 8 and its corrections 12 and 15; [NSF](NSF_native_signal_family_2026-11-18.md); [NRF part 10](NRF_network_ring_fixing_2026-11-14_part10.md) section 79.4; NRF parts 16 and 17 sections 117 and 117.1; [L5B](L5B_band5_receiver_delay_2026-10-10.md); [ALD](../record/refound/ALD_anchor_level_datum_2026-10-23.md). Code read at `cf983dc` (branch `int/2026-11-19`). Analysis files: `D:/GnssProcessorSupport/gates/day_program_2026-11-19/results/xband/` (`rank/`, `receiver/`, `satellite/`, `code/HOLDERS.md`).

**Outcome.** The minimal completion of the triple posture's datum is proven: one hard DCB row per (receiver, system), roots included, and one level row per extra-signal component; both are pure datum rows. The run evidence narrows what they can repair. The receivers' decimetre third-band drift lies in the estimable receiver quantity, and the satellites' sums move by membership. The receiver direction moves only at the chain roots, where it is tied to the phase datum, and on observed data the root choice shifts the estimable GPS quantity by 0.5 to 1.5 cm before any integer differs. Whether the DCB rows remove that and DOY 144's disagreement is the day runs' question.

## 1. The question and the principle

The question is whether a third frequency band makes the network's datum incomplete, which rows complete it, and
whether the incomplete datum explains DOY 144's disagreement between the runs from the two datum receivers (NRF 79.4:
5 179 failing checks with the third band at full weight, 0 with its code rows deweighted 100 times). The principle
served is one joint network for all systems, frequencies and channels: a form that keeps bands or signal types in
separate networks is at most an interim step. The third GPS band is not closed by this record; it stays SEQUENCED
(section 9). The integer work belongs to step IRD-3 of [IRD](IRD_integer_recovering_datum_2026-10-13.md) section 11 (the third band's tree grown on
the pair's forest) under IRD-4's invariants; its bars are reused from NRF 79.1 and 79.4 and IRD-8 K-a on the
simulator.

## 2. The model in the engine's parameterization

Notation as JNM section 1, with mu_j = (f_L1 / f_j)^2. The receiver code biases are parameterized as the engine builds
them (src/core/measurement/ReceiverBias.cpp:60-229):

- the two bands of the clock-reference pair carry one column `<sys>_DCB` per (receiver, system), coefficients
  c1 = mu1/dmu and c2 = mu2/dmu with dmu = mu1 - mu2 (the ionosphere-free combination of the pair carries none);
- a third band carries one column `<sys>_IFB<band>` per (receiver, system) with coefficient 1 (written E);
- a second band-1 code that is not the receiver's canonical band-1 code carries `<sys>_ISC_<signal>`, coefficient 1.

Satellites carry one code column per signal and one phase column per phase signal; receivers one phase bias per phase
signal (the chain form); ambiguities one per arc. Per epoch: receiver clock, the Galileo inter-system offset, satellite
clocks, wet delay, slant ionosphere per line of sight.

| | GPS L1/L2 with L5 | Galileo E1/E5a with E5b |
| --- | --- | --- |
| mu2 | 1.646944 | 1.793270 |
| mu3 | 1.793270 | 1.703246 |
| dmu = 1 - mu2 | -0.646944 | -0.793270 |
| mu3 / dmu | -2.771908 | -2.147120 |

## 3. The null directions and what the engine fixes

| Direction | Count | Engine (cf983dc) |
| --- | --- | --- |
| N1 clock | one per system and epoch | hard: one satellite clock pin |
| N2 receiver clock against receiver biases | one per (receiver, system) | parameterization (the DCB has no ionosphere-free part) |
| N3 satellite clock against satellite biases | one per satellite | hard: ionosphere-free pin of the satellite's pair, time-linked |
| N6 satellite geometry-free | one per (satellite, class) | hard: geometry-free pin of the pair at the broadcast group delay, time-linked; the Galileo band-2 companion (C5X) pinned against band 1 |
| N4 level of an extra code signal | one per (code signal, connected component of its receiver-satellite graph) | hard: ONE level row per (system, code signal) (a sum over its satellites, time-linked over the veterans, or for a Galileo secondary channel the family difference); a split graph leaves one direction per extra component |
| N7 + N8 phase biases against ambiguities | n_c + m_c per (phase signal, component) | hard: chain pins (receiver phase-bias pin at the root, a spanning tree of ambiguities) |
| **N5 receiver geometry-free** | **one per (receiver, system)** | **soft only**: the 10 m DCB prior refolded every epoch, the DCB and IFB carries, the receiver phase-bias time link |

The N5 generator in the engine's parameterization: every slant ionosphere of the system at the receiver += iota,
DCB -= dmu iota, IFB -= mu3 iota, ISC -= iota, receiver phase bias of signal j += mu_j iota. Its ratio dE/dD = mu3/dmu
is the ratio NRF 79.4 measured at some stations (-2.44 and -1.78 against -2.77 and -2.15).

The estimable receiver quantity of a third band (JNM correction 15) is, under the engine's columns,

    GIF = E - (mu3 / dmu) * D        (GPS: E + 2.7719 D; Galileo: E + 2.1471 D)

invariant under N5 and determined as a difference between receivers of one component of the extra signal.

## 4. The minimal rows and their proof

**Statement.** Under the engine's hard rows, the triple posture's design has exactly one free direction per
(receiver, system) and one per extra code signal and component beyond the first. A hard row DCB = target per
(receiver, system), at every receiver including the chain roots, and one level row per component of each extra code
signal (receivers sharing an IFB column merged across the signals of that band) close the null space. Both sets are a
pure datum: rank([A; H]) = rank(A) + rank(H), so they constrain no estimable function. An IFB row per (receiver, system)
in place of the DCB row is an equally valid datum of N5 (one of the two, never both); the DCB row is the choice that is
the same on dual and on triple frequency.

**Method.** `results/xband/rank/xband_rank.py`: the observation rows and the engine's hard rows as listed in section 3,
built in the parameterization of section 2; ranks by exact elimination in rational arithmetic (toy networks) and over
GF(2^61 - 1) and GF(2^31 - 1) with random residues for the mapping function and wavelengths (the real roster); equal
ranks on both primes. The builder first reproduces the recorded chain case of JNM section 6 exactly (defect K + 24,
hard rows K + 20, nullity r = 4; DCB rows at every receiver close it, at every receiver but the root leave 1).

**Toy networks** (rational arithmetic, `toy_result.txt`):

| case | nullity after the engine rows | + DCB rows at every receiver | + DCB rows except the roots | + IFB rows instead |
| --- | --- | --- | --- | --- |
| GPS triple, C5Q at 3 receivers, C5X at 2, C1W at 1 (K = 2, 3) | 5 = receivers | 0, pure datum | 2 | 0, pure datum |
| GPS + Galileo triple, Galileo families on disjoint receivers | 8 = receiver-system pairs | 0, pure datum | 4 | 0, pure datum |
| C5Q graph in two components, one level row per signal | 5 = 4 + 1 | 1 | 3 | 2 |
| the same with one level row per component | 4 | 0, pure datum | 2 | 1 |
| GPS dual frequency | 4 | 0, pure datum | 1 | n/a |

The N5 generator lies in the null space of the data and of the engine rows at every non-root receiver tested; the
level generator of each component is null for the data and moves only its signal's level row.

**The real roster** (`_rap_k1t_n144`, three epochs of 30 s, 6 876 and 6 940 columns, `roster_*.txt`,
`families_*.txt`):

| window | rank(A) | nullity after the engine rows | DCB columns | + DCB rows | + DCB rows except the roots |
| --- | --- | --- | --- | --- | --- |
| TOW 3600 to 3660 | 5 988 | 110 | 110 | 0, pure datum | 4 |
| TOW 7200 to 7260, one level row per signal | 6 062 | 111 | 110 | 1 | 7 |
| TOW 7200 to 7260, one level row per component | 6 062 | 110 | 110 | 0, pure datum | 6 |

At TOW 7200 the GPS C5Q graph has two components: PALM00ATA with G18 and G26, and 26 receivers with 18 satellites.
Every engine row family is a pure datum on its own (additivity deficit 0 for clock pins, satellite pins, companion
pins, carried extra-band rows, level rows, root pins and tree pins). Over the whole run the C5Q graph splits at
TOW 4140 to 7860 and 21 270 to 21 570; C5X, C7Q and C7X stay one component at all 720 epochs; the Q and X receiver sets
are disjoint and no receiver tracks both codes of one band (`satellite/S2_*`).

**What the rows do not do.** A pure datum row moves no estimable quantity of a batch solution. In the sequential
solution the engine's soft holders can make realized estimable quantities depend on the datum (section 5); the hard
rows remove that path for N5 and the extra components, nothing more.

## 5. The attribution of the third-band drift

Read from the existing run folders, no engine run (`receiver/`, `satellite/`; scripts beside the tables).

**5.1 The receivers' third-band delays drift in the estimable quantity, not along N5.** In `_rap_k1t_n144`, hours 0.5
to 6, the median net change of the DCB is 1.5 mm (GPS) and 3.8 mm (Galileo), against 5.1 cm and 10.2 cm for GIF; the
decimetre drifts named in NRF 79.4 are GIF drifts (Galileo DJIG00DJI dE -0.80 m with dGIF -0.86 m, SEYG00SYC -0.76 with
-0.80, GOP600CZE -0.50 with -0.55; GPS SVTL00RUS +0.29 with +0.31). Epoch-to-epoch, the median share of the increment
variance along N5 is 0.1 % (GPS) and 0.7 % (Galileo); the median slope of dE on dD is +17 and +13, not the N5 ratio.
The N5 ratio of NRF 79.4 holds at a few stations only: GPS BOR100POL (-2.57, correlation -0.81), Galileo PALM00ATA,
BOGT00COL and COCO00AUS. `k1r` and `k1b` give the same medians. Section 79.4's reading ("the delay moves with the DCB at
the ratio of a common shift of the station's slant ionosphere") generalised from those stations.

**5.2 The reported sigmas are the refolded prior.** The DCB and IFB standard deviations equal 10/sqrt(n) and
|mu3/dmu| * 10/sqrt(n) within 0.4 % at 52 of 55 stations in both systems (n = epochs since the column's birth): the
posterior variance of both columns is the N5 direction held by the prior folded again at every epoch on top of the
carried posterior (RxCodeBiasCarry.cpp:313-334, folded through `inj_prior` as `PriorRxCbDatumPrePd` while the in-block
carry folds the posterior; `code/HOLDERS.md` C1, C3). GIF's own formal uncertainty is negligible beside it, so a GIF
drift of decimetres is far outside its formal sigma: the drift is in the data's third-band content or in the model of
it, not a loose datum. The stations held below the prior (BOR100POL 0.044 of 10/sqrt(n); BOGT00COL 0.045, COCO00AUS
0.052, PALM00ATA 0.062) are the stations where D moves along N5.

**5.3 Code deweighting stops the N5 motion, not the GIF drift.** `_rap_k1a_n144` (third-band code rows deweighted 100
times, the run that passes the integer bar): median |dD| 0.2 mm (GPS) and 0.1 mm (Galileo), Galileo stations with
|dD| above 1 cm from 17 to 2; median |dGIF| 6.3 cm and 6.6 cm. `_rap_k1q_n144` (receiver extra-band delay noise
1e-5 m2/s): GIF drifts more (8.5 cm and 13.3 cm, range 0.26 m against 0.10 m), D unchanged, and the two-run audit
unchanged at 5 179 failing checks (NRF Q3 arms against their same-exe controls `k1t45`, `k1r45`). Neither the GIF drift
nor the delay's process noise is the path to the integer disagreement; the third band's code rows at full weight are
(NRF 79.4), through the slant ionosphere on the clusters' lines of sight (4 to 5 cm above `f13` at the decisions in
`k1t`, within 1 cm in `k1a`).

**5.4 The satellites' extra-band sums move by membership.** In `k1t` the satellites present at two consecutive
epochs keep their summed C7Q, C7X, C5Q and C5X columns within 1e-3 m over the run (veterans -0.09, +0.33, 0.00,
0.00 m); the sums' moves are entries and exits: C7Q +1.78 m by entries and +12.97 m by exits (departing satellites with
large values, E11 -7.3 m, E12 -5.3 m, set in the first epoch). C5X keeps 0.00 m because it gains no new satellite (three
satellites leave and return at their old value within 0.5 mm). The pair signals move the same way (Galileo C5Q by
+16.2 m). A satellite returning after about 4 000 s or more comes back at a new value (E16 +2.70 m, E10 +1.29 m): the
satellite code-bias carry's horizon, not the level. `k1a` shows the same walk (C7Q ends at 13.41 m). The level rows hold
the level exactly; nothing in the satellite side calls for a datum change.

**5.5 Dual frequency.** On the five adopted days (`_rap_ad_n143` to `_rap_ad_n147`, 24 h) GPS_DCB ranges 0.7 to 0.9 mm
at the median (BOR100POL 0.08 to 0.21 m every day). Galileo_DCB moves more than 5 cm at MAJU00MHL (DOY 145, 0.51 m),
MBAR00UGA (DOY 146, 0.27 m), WIND00NAM (0.16 m) and BOGT00COL (DOY 147, 0.087 m), in excursions of 5 to 20 minutes
that return (MBAR00UGA +0.11 to -0.17, ending at -0.004 m), at epochs of rehosts, slip latches, re-entries and cut
releases of the chain. These are events moving a null direction (standing invariant 7) that the refolded prior pulls
back; the std there falls to 0.48 to 0.78 of 10/sqrt(n) and rises up to 2.65 times after an excursion.

**5.6 The simulator.** `_rap_t3t_sim` shows centimetre GIF drift after the first hour (median 2.0 cm, largest 0.12 m)
with no drifting delay in its truth, and no N5 drift except at two stations. Its runs from both roots hold the same
integers.

**5.7 Root dependence.** Between `k1t` and `k1r` the DCB differs by 0.2 mm or less at the median and GIF by 7.6 mm
(GPS) and 4.2 mm (Galileo), 90th percentile 3.6 cm and 1.8 cm. GIF is estimable, so a difference between two datum
choices is either the datum leaking into estimable quantities through the soft holders or the consequence of different
held integers. Split by time from TOW 1800 (`receiver/R7_*`, `R7b_level_removed.txt`, the common level of the Q and the
X stations removed, since that level is a datum direction):

| TOW | GPS GIF, k1t - k1r, between stations: median / 90th pct (m) | Galileo GIF (m) |
| --- | --- | --- |
| 2700 | 0.0156 / 0.0379 | 0.0001 / 0.0012 |
| 4500 | 0.0110 / 0.0451 | 0.0005 / 0.0044 |
| 9000 | 0.0074 / 0.0326 | 0.0022 / 0.0129 |
| 21000 | 0.0078 / 0.0230 | 0.0034 / 0.0210 |

The GPS difference is in place at TOW 2700, before the first failing check (TOW 3630, Galileo X/X) and before the GPS
cluster (TOW 6400); the Galileo difference grows only after the X/X decisions. On the simulator pair (`t3t_sim`
against `t3t_sim_r`, both roots holding the same integers) the GIF difference is 0.0000 m at the median and at most
0.2 mm at the 90th percentile at every time from 00:45, apart from the two roots' own stations at ignition: with clean
data the estimator is root-invariant in GIF; with DOY 144's data it is not, by 1.6 cm, before any integer differs.
Different hold timing of integers that do not fail the check is not excluded by this read.

**The roots.** The stations whose DCB moves along N5 are the chain roots of the phase families: in `k1t` COCO00AUS
(Galileo L1C, L5Q, L7Q), BOGT00COL (Galileo L1X, L5X, L7X), BOR100POL (GPS L1C, L2W, L5X), PALM00ATA (Galileo and GPS
L5Q second components) and GOP600CZE; in `k1r` WTZR00DEU takes eight families. At a root the receiver phase bias is
pinned, so the station's receiver direction moves every satellite phase bias of the family and every other receiver's
phase bias with it; it is held there by those carries against the refolded prior, which is why the roots' DCB sigma
sits at 0.04 to 0.06 of 10/sqrt(n) and their DCB walks by decimetres (`k1t` PALM00ATA -0.40 m; `k1b` BOGT00COL +2.25 m).
The DCB of the stations whose root status differs between `k1t` and `k1r` differs by 2 to 20 cm before TOW 3600
(BOR100POL, GOP600CZE, WTZR00DEU, BOGT00COL, COCO00AUS). The dual pair `f13_n144` against `f13r_n144` shows the same
root stations differing (BOR100POL 7.9 cm, COCO00AUS 7.6, PALM00ATA 8.5, WTZR00DEU 4.1 before TOW 3600; median 0,
90th percentile at most 1.5 mm): the roots' coupling is a property of the chain datum on any number of bands, not of
the third band. The coincidence of the lists (moving stations and roots) is shown, not a causal chain. The simulator
pair was read from its first epoch (TOW 261060, 00:31:00).

**Reading.** The scout's reading (the refolded prior and the extra-band delay carry hold one direction against each
other, so the third band is a datum problem) holds at the chain roots and not for the network: the decimetre drifts
are in the estimable receiver quantity, and the satellite walk is membership. Three datum defects are confirmed and
are what the keys address: the repeated prior, which reports every receiver DCB and extra-band sigma as 10/sqrt(n) and
pulls the time variation of the receiver direction; the receiver direction at the roots, which is tied through the
root's phase-bias pin to every satellite phase bias and every other receiver's phase bias and so changes with the root;
and the split C5Q graph, whose second level is held only by carries. On observed data the root choice reaches the
estimable GPS GIF by 1.6 cm before any integer differs, on the simulator not at all: the hard rows make the receiver
direction the same in both runs whatever the root, and D3 measures whether that leak and the X/X window's failing
checks shrink with it.

## 6. The holders today (code at cf983dc, `code/HOLDERS.md`)

- The DCB prior: `networkRxCbPriors` folds lam = 1/sigma^2 (sigma `network.rx_code_bias_datum_sigma_m`, default
  10 m) on every station block every epoch with no test of carry state (RxCodeBiasCarry.cpp:313-334), through
  `inj_prior` (pre-PD) in the adopted configuration; counted `pl_cb_datum_rows`, run-end line RXCB-DATUM
  (Sbasis.cpp:2512-2534).
- The DCB and IFB carry in block: a whitened block with border regression and cross terms (RxCodeBiasCarry.cpp:145-233,
  380-505), q 1e-9 m2/s (`rx_extra_band_delay_q` for IFB when set), horizon 900 s, a 10-sigma step reset and the family
  re-seed erase, gap prune at the previous epoch's time.
- The receiver phase-bias time link, q 3e-6 m2/s, and at the root the 1e10 pin (PhaseDatum.cpp:1563-1577).
- No prior on the slant ionosphere per line of sight in the adopted posture; the ionosphere seed carries no receiver
  DCB (the DCB is born at 0 after the seed).
- The anchor level hold of card ALD (1e10 on the data-null level direction restricted to carried columns, at anchors
  or at every station; default off) failed its event gate on three bands and is closed; it is a different row (the
  carried part of the level, numerically extracted) from the DCB row proposed here.

## 7. The keys (built default off, byte-identical off)

**`network.rx_gf_datum`**, values:

- `prior_refold` (default, today): the 10 m prior folded on every epoch.
- `birth_prior`: the prior folded once, when the DCB column has no carried block row this epoch (no carried entry, an
  entry past the 900 s horizon, or a restored entry without its block), which covers a station's birth, its return
  after the gap prune and every erase; the posterior carried after. Removes the double count.
- `hard`: a hard row (weight 1e10, the chain pins' weight) per (receiver, system) on the DCB, at every receiver
  including the roots: target 0 at a birth or return, the carried value otherwise; an event at both edges (the row
  placed at a birth or return, and released when the station leaves), station and system named; counters of rows
  placed and seeds by cause; a run-end line.

**`network.extra_band_level_datum`** (bool): the level row of each extra-band code signal split by connected component
of the receiver-satellite graph (receivers sharing an IFB column merged across the band's signals), each time-linked
over its veterans, a component with no veteran re-seeded at zero over its present satellites with an event; counters of
components, splits and re-seeds. Its Sbasis.cpp hunk lands after the pin-walk package's Sbasis commit.

Each key has a ledger row UNVALIDATED and a fingerprint token when armed. The comment at RxCodeBiasCarry.cpp:299-300
("The between-station differences are estimable") is corrected.

## 8. Pre-registered bars (written before any run of the new engine)

Engine: the Release build of branch `math/extra-band-datum-2026-11-19`, run as a renamed copy. Order: the identity
set D1 first (nothing else is read before it passes), then D1b, D0, D2, D3. `network.extra_band_level_datum` joins
the keyed runs in a second build, after the pin-walk package's Sbasis.cpp commit; the first keyed runs arm
`network.rx_gf_datum` alone (one change per run).

- **D1, off identity** (keys absent; `scripts/nom_td_gate.py --whole --mask-time`; bar: identical). Against the frozen
  engine `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_int1119.exe`, through its own 2 h runs: the
  configurations of `_rap_int19_n146` (observed, integer fixing), `_rap_int19_f40mc` (simulator, integer fixing) and
  `_rap_int19_gf` (the field reference), run on the new engine as `xb_id_n146`, `xb_id_f40mc`, `xb_id_gf`. These are
  dual frequency and never enter the third-band rows, so a fourth pair: the triple-frequency control `xb_k1c_n144`
  (`k1t_n144` at 2 h) on the new engine against `xb_k1f_n144` (the same configuration) on the frozen engine.
- **D1b, the receiver datum on the adopted dual-frequency control** (the managing session's addition): the
  configuration of `_rap_int19_n146` with the keys off (`xb_id_n146`), with `network.rx_gf_datum` birth_prior
  (`xb_idb_n146`) and hard (`xb_idh_n146`), 2 h each. Read: which outputs move at all (products, users, the held
  integers and their decision epochs, the receiver code-bias sigmas) and by how much, graded on between-satellite or
  datum-invariant quantities only; a pure datum change should move no estimable quantity, so every move is named with
  its cause. Stop rule: if the held integers differ between the three, stop and report before any further run.
- **D0, 10-epoch smoke** (`xb_smoke`: `k1t_n144`'s configuration with `network.rx_gf_datum` hard): exit 0; the
  RX_GF_DATUM run-end line with rows and placements non-zero; place events in `network_events.csv`.
- **D2, simulator first**: `t3t_sim`'s configuration, 2 h from 00:30:15, both keys on against the same-exe control:
  0 wrong held link-epochs and 0 wrong decisions against truth in the pairs and cascades; the pairs' held link-epochs
  within 1 % of the control.
- **D3, DOY 144 at 2 h** (TOW 0 to 7200, the Galileo X/X window TOW 3600 to 6200 inside): `k1t_n144`'s configuration on
  the new engine as the control (`xb_k1c_n144`); the keys on (`xb_k1h_n144`); the keys on from WTZR00DEU
  (`xb_k1hr_n144`, from `k1r_n144`). The base is the `f13` setup plus the third band, not the adopted setup
  (`_rap_ad_*`, the main reference configuration since 2026-11-19): the evidence of section 5 and the two-root
  disagreement live in that configuration, and the adopted setup merges the Galileo X family into the pilot names
  (`network.phase_families`), so the X/X window where the two roots first part does not exist there. The adopted
  setup's triple runs are on the night list (section 9, rank 0). Graded: the two-root check of the two keyed runs in the X/X window (bar 0 failing; reference `k1t` against
  `k1r`, exe nrf32, 1 182 failing in that window); the keyed runs' held integers against `f13_n144`'s over the 2 h
  (reported against the control's count); the GIF difference between the two keyed runs before TOW 3600 (section 5.7's
  measure, bar: smaller than `k1t` against `k1r`); the DCB, IFB and GIF ranges and the slant ionosphere on the cluster
  lines of sight beside them.
- **Expectation, stated before the run.** The attribution makes a full pass of D3 unlikely from the datum alone: the
  third band's code content reaches the integers through the slant ionosphere and the estimable GIF. The decisive
  reading is whether the keyed runs are closer to datum invariance (D3's third measure) and whether the window's failing
  count falls.

## 9. Night runs (specified, none run)

**Rank 0, the adopted setup with the third band (specified in full: no such run exists).** Base
`D:/GnssProcessorSupport/runs_nom/_rap_ad_n144/arm.json` (the adopted integer-fixing configuration on DOY 144), cut to
6 h (`processing.max_duration_s`, `network.max_duration_s` 21 600), with the third band added as `k1t_n144` adds it to
`f13_n144`, adapted to the phase families:

- `processing.frequency_mode` triple; `processing.signal_priorities` with GPS C5Q, C5X, C5I, L5Q, L5X, L5I (and their D
  and S types) and Galileo C7Q, C7X, L7Q, L7X appended, as in `k1t_n144`;
- `network.estimate_rx_extra_band_code_delay` true; `network.sat_gf_extra_band_free` true;
- `network.phase_families` ["E:L1X>L1C", "E:L5X>L5Q", "E:L7X>L7Q"] with `network.phase_family_codes` true: the
  combined Galileo E5b signals are read under the pilot names, phase and code, as E1 and E5a already are, so Galileo
  has one family on all three bands (C7Q/L7Q) and the X/X family of `k1t` does not exist; GPS L5 keeps its two
  tracking kinds (C5Q/L5Q and C5X/L5X), as in the adopted setup GPS has no families;
- `network.ring_fix.pairs` as adopted (["E:L1C/L5Q", "G:L1C/L2W"]); `network.ring_fix.cascade`
  ["G:L1C/L2W>L5Q", "G:L1C/L2W>L5X", "E:L1C/L5Q>L7Q"] with `cascade_log` and `cascade_tree` true (the third-band
  integers decided in the log only, never held: no third-signal integer is held on observed data before these bars
  pass).

Runs: keys off and keyed (`network.rx_gf_datum` hard plus `network.extra_band_level_datum` true), each from the
elected root and from WTZR00DEU (`network.phase_datum_eq34_receiver`): 4 runs of 6 h; if slots allow only two, the
keyed and keys-off runs from the elected root. Read: the two-root check of each pair (held and would-hold, the pairs
and the cascades' log), the pairs' held integers against `_rap_ad_n144`'s (the dual adopted run of the same day, hours
1 to 6), the GPS and Galileo GIF root difference between stations before the first failing check, the receivers' DCB
and IFB ranges and sigmas at the roots, the C5Q graph's components and the level rows' re-seeds. Bar: 0 failing
two-root checks in the pairs; the pairs' held integers equal `_rap_ad_n144`'s; the keyed GIF root difference below
the keys-off pair's. This configuration decides more than `k1t`'s: it is the setup that goes forward.

| rank | base | change | hours | runs | bar |
| --- | --- | --- | --- | --- | --- |
| 1 | `_rap_k1t_n144/arm.json` on the new engine, the k1t root and WTZR00DEU | `network.rx_gf_datum` hard + `network.extra_band_level_datum` true | 6 | 2 | two-root check 0 failing in both DOY 144 clusters (GPS TOW 6400 to 9700 lies beyond 2 h); held integers equal `f13_n144`'s; extra-band delay range within 3 times the carry's 1 sigma at the roots; GIF root difference below the keys-off pair's |
| 2 | the same two roots, keys off, new engine | none (the same-exe reference for rank 1) | 6 | 2 | reproduces the recorded 5 179 failing checks within the exe change |
| 3 | `_rap_t3t_sim/arm.json` and `_rap_t3t_sim_r/arm.json`, from 00:30:15 | as rank 1 | 3 | 2 | 0 wrong held link-epochs and decisions against truth in pairs and cascades; held link-epochs within 1 % of `_rap_t3t45_sim` |
| 4 | `_rap_k1t_n145/arm.json`, two roots | as rank 1 | 6 | 2 | 0 failing two-root checks; held integers equal `f13_n145`'s |
| 5 | `_rap_ad_n146/arm.json` (dual, adopted), new engine | `network.rx_gf_datum` birth_prior, and a same-exe control | 6 | 2 | clocks and biases within solve noise of the control; held shares within 0.5 point by hour; 0 new failing two-run checks; Galileo DCB excursions at MBAR00UGA and WIND00NAM reported |

## 10. Decisions for the author

1. **JNM decision 1, the holder of the receiver direction.** Recommended: the hard DCB row at every receiver, roots
   included (section 4; the same datum on dual and triple). Alternatives proven equivalent as a datum: an IFB row per
   (receiver, system) on triple; removing the DCB column. `birth_prior` is the minimal repair if a soft datum is kept.
2. **JNM decision 4, estimability-tested extra-band rows.** Recommended: the level row per component
   (`extra_band_level_datum`), needed wherever an extra signal's weighted receiver-satellite graph splits (on DOY 144
   none does once the zero-weight rows are left out, section 11).
3. **The third band's path to the integers** is its code content through the slant ionosphere and the estimable GIF
   (sections 5.1 to 5.3), not the datum. The next lever, after the day runs, is a model question (a receiver-satellite
   or make-dependent term on the third band, or its code weight in the integer-fixing posture), to be measured on the
   simulator first. The third GPS band stays SEQUENCED: the order is the datum keys (this record), their night runs,
   then the third-band code model, then third-signal integers held on observed data.

## 11. 2026-11-19: the independent check, the zero-weight rows and a direction of the two Galileo families

An independent reviewer recomputed the claims of sections 4 and 5 from the raw traces with scripts of its own
(`results/xband/review/`, `REVIEW_SUMMARY.md`), without reading the team's scripts first. Verdicts and corrections:

- **5.1 (the drift is in GIF): confirmed** (median |dD| 1.5 and 3.8 mm, |dGIF| 5.08 and 10.21 cm; DJIG -0.864 m,
  SEYG -0.801, GOP600CZE -0.547, SVTL +0.314). The Galileo drift sits in the C7Q receiver set (median net 0.38 m over 27
  stations); with the set's common level removed 0.31 m remains: differential, estimable.
- **5.2 (the refolded prior): narrowed.** The DCB sigma is 10/sqrt(n - 6), not 10/sqrt(n): it stays at 10 m for the
  first seven epochs (the carry's warm-up, when no carried row exists); with n - 6, 52 of 55 stations per system sit
  within 0.4 %. The exceptions are three per system and all are chain roots: GPS BOR100POL (0.044), GOP600CZE (0.62),
  PALM00ATA (0.62); Galileo BOGT00COL, COCO00AUS, PALM00ATA. A root of a pair band is held to 0.04 to 0.06 of the prior,
  a root of GPS L5Q only to 0.62.
- **The roots: confirmed** (the `r1` column of `network_chain_edges.csv`).
- **5.4 (the satellite sums): narrowed.** The cumulated veterans' sums are reproduced (-0.086, +0.332, -0.004,
  +0.002 m) and the walk is membership, but C7X's veterans' sum moves by up to 0.15 m between epochs in the first three
  hours (198 of 720 pairs above 1 mm, mostly in hour 1): C7X is a Galileo secondary-family channel, its level held by
  the family-difference row against C7Q, not by a sum over its own satellites. After about three hours every veterans'
  sum holds within 1 mm.
- **5.7 (the root dependence): narrowed.** By the reviewer's computation the GPS between-station GIF difference at
  TOW 2700 is 1.10 cm at the median (90th percentile 3.4 cm), not 1.56 cm; over TOW 1800 to 3600 it ranges from 0.5 to
  1.45 cm, and 0.6 cm at TOW 2700 with the four root stations left out. The effect is real, 0.5 to 1.5 cm, and partly
  the roots. The simulator pair is confirmed (median 0.03 mm, 90th percentile 0.2 mm).
- **Section 4 on the real roster: narrowed.** The rank script counted rows the engine weights at zero. At TOW 7200
  four such rows exist, among them PALM00ATA's G18 and G26 C5Q rows, which the engine excludes because only that
  station reaches those satellite columns (`rxIfbExcludedRows`, StationSplit.cpp). The "split C5Q graph" consisted of
  those rows: with weighted rows only, every extra-band graph of `k1t_n144` is one component at all 720 epochs (the
  reviewer's `K6_components__rap_k1t_n144.csv`; recomputed here, `roster_*_w.txt`). The third datum defect of section 5
  is therefore withdrawn for DOY 144: `network.extra_band_level_datum` remains the correct completion of a split graph
  (the toy case of section 4), but on this day it would place no extra row.
- **A direction of the two Galileo families.** With the zero-weight rows dropped, TOW 3600 to 3660 closes exactly as
  stated (nullity 110 after the engine rows, 0 after the DCB rows). TOW 7200 to 7260 leaves one more direction after
  the 110 DCB rows (`rank/xband_nullvec.py`): it moves the C1X satellite biases of 29 satellites, the X-family satellite
  phase biases (L1X, L5X, L7X), the inter-system offsets, E5b IFBs and slant ionospheres of the 28 combined-mode
  receivers, and the clock of E12. It involves no DCB and no GPS column: it is a level of the combined-mode Galileo
  family, open while a satellite (E12) is reached by that family only (inferred from the vector), which the
  family-difference row over the common satellites does not hold. The engine's S-basis completion did not engage at
  that time (it engaged at TOW 2850 and 13950 only, on phase-bias directions), so the direction rests on carries. It
  belongs to the `f13` setup's two separate Galileo families; the adopted setup merges them (`network.phase_families`)
  and cannot carry it. It is recorded here as a finding of the rank check, not addressed by either key.
- **The derivation's rows: no engine hard row found modelled wrongly** (IF and GF pins, the band-2 companion, carried
  and held extra-band rows, the family-difference and level rows, the clock pin, root and tree pins). Not modelled: the
  degenerate single-frequency satellite pin (not triggered), the soft S-basis completion, the ring-fix holds, the rank
  floor and the anchor-level hold (inert or soft in `k1t`).
- **The alternative tested by the reviewer** (the receiver drift follows the mean of the satellites' extra-band columns
  over its visible set): not supported (increments correlate at 0.02); receiver multipath on the third band cannot be
  tested from these traces.

Consequences: D3 measures the receiver datum alone (`network.rx_gf_datum` hard). **`network.extra_band_level_datum`
is withdrawn before its build** (2026-11-19, 13:30): with the zero-weight rows left out no extra-band graph of DOY 144
splits, and the program's exact rank tool (package systems-rank-reference, `HANDOFF_k1t_posture_for_extra_band_datum.md`,
`_rap_k1t_n144` at TOW 10800 and 10830) finds every extra-band satellite code level estimable after the armed hard rows
(E7Q 26 of 26, E7X 27 of 29, E5X 30 of 30, G5Q 18 of 18, G5X 21 of 21), so the key would place no row and be inert
wiring. The derivation of section 4 stands for a graph that does split; the component code and its unit cases are kept
outside the repository (`results/xband/withdrawn/`). The same tool confirms the minimal set of section 4: one row per
(receiver, constellation), 110 rows on `k1t_n144`, none redundant, DCB and IFB rows interchangeable and redundant
together; and it finds the receiver direction held below the prior-only value at most stations in the anchor form
(`_rap_srn_b6`, `_rap_srn_f6`), so "nothing hard holds it" is a statement about the chain configuration.

<!-- Results are appended below this line under dated headings. -->
