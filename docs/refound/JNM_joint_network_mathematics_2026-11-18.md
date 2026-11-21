# JNM: rank defects and datum of one joint network across systems, bands and channels (2026-11-18)

**STATUS**

- **id:** JNM
- **topic:** network-vehicle
- **status:** reference
- **summary:** Reference text for one undifferenced, uncombined network over several systems, bands and tracking channels: the null-space generators, the defect count, the S-basis rows, small cases checked by a rank computation, the estimable and integer combinations, and a table of what the network code fixes against what the model needs. The lead holds after the check against the code: nothing hard holds the receiver geometry-free direction, one per receiver and system. Ten statements were wrong in scope or wording (the class definition, the minimality of the S-basis table, the wet-delay condition, the new-arc rule, two notes on the code among them) and are corrected in the last section; nothing in the code is changed.
- **source:** plan package 11, "Mathematics of one joint network across systems, bands and channels"; docs/report/*.tex, CHA part 3 and NRF parts 14 and 18 as cited in the body; the commits read: 03dfcbc of branch chain/audit by the draft, 7fb0737 for network code by the check (of the cited code files only src/core/config/GnssConfigLoadNetwork.cpp differs between the two), main 5ddc422 for user-engine code. Written by an agent team in the night of 2026-11-18 and checked against the code by an independent agent; the corrections are the last section.

**Outcome.** The joint model has eight families of null directions; an independent exact rank computation reproduces them and the counts of the small cases. The code fixes seven families once each, by hard rows or by parameterization. The eighth, the receiver geometry-free code bias against that receiver's slant ionosphere, one direction per receiver and system, has no hard row: it rests on a 10 m prior on the receiver DCB and on carries, and the receiver phase-bias floor is off in the integer-fixing configuration. A per-make code term with zero mean per band and class leaves s-1 free directions for a class that differs on both bands and shares no other band. The mathematical reference lacks a general rank count, the chain count and the ring fix.

Read at commit 03dfcbc. Every path:line below is at 03dfcbc. Files that carry uncommitted edits in the worktree were read with `git show 03dfcbc:<path>`. This text merges three first-stage results (what the code fixes, a derivation, a check of the reference) with an independent review; where the review refuted a first-stage claim, the claim is corrected here and the disagreement is stated in section 10. The small cases of section 6 were also checked by a rank computation of the design matrix (pure Python Gaussian elimination, random mapping-function values, L1 and E5a frequencies), run outside the repository; no engine or test was run.

## 1. Model and notation

Notation follows docs/report/observation_equations.tex:27-55 and docs/report/notation.tex:82-107.

```
P(r,s,j,k) = rho + dt_r(k) - dt^s(k) + dt_{r,g}(k) + m(r,s,k) T_r(k) + mu_j I(r,s,k) + B_{r,j} - B^s_j
L(r,s,j,k) = rho + dt_r(k) - dt^s(k) + dt_{r,g}(k) + m(r,s,k) T_r(k) - mu_j I(r,s,k) + lambda_j N(r,s,j) + b_{r,j} - b^s_j
```

- r receiver, s satellite of system g, j signal (system, band, channel), k epoch; `mu_j = f_L1^2 / f_j^2` (the slant ionosphere is referred to GPS L1 for every system: src/core/measurement/ObservationEquation.cpp:196-200, include/common/GnssIdentity.hpp:1111).
- Free every epoch: receiver clock `dt_r`, inter-system offset `dt_{r,g}` (systems other than the time reference only), satellite clock `dt^s`, zenith wet delay `T_r`, slant ionosphere `I`.
- Constant over an arc: code biases `B`, phase biases `b`, ambiguities `N`. A slow random walk on them does not change the null space, because every bias direction below is constant in time.
- Orbits and coordinates are known (held at the roster value).
- G systems; `s_g` satellites in system g, `s = sum s_g`; `f_g` signals in system g, `F = sum f_g`.

[corrected 2026-11-19: see Corrections, item 1] A "class" is a set of receivers that track a band with the same channel (Galileo pilot against combined tracking, or one receiver make within the combined mode, NRF section 107.8).

## 2. Null-space generators

Each generator leaves every observation unchanged.

| Name | Direction | Count |
| --- | --- | --- |
| N1 clock datum | `dt^s(k) += a_g(k)` for all s in g, and the receivers' clock of g `+= a_g(k)` | K G |
| N2 receiver clock against receiver biases | `dt_r(k)` (or `dt_{r,g}(k)`) `+= beta` for all k; `B_{r,j} -= beta`, `b_{r,j} -= beta` for every j of g | one per (receiver, system) |
| N3 satellite clock against satellite biases | `dt^s += gamma^s`; `B^s_j -= gamma^s`, `b^s_j -= gamma^s` for every j | s |
| N4 code-bias level per signal | `B_{r,j} += delta_j` at every receiver tracking j; `B^s_j += delta_j` | [corrected 2026-11-19: see Corrections, item 2] one per signal |
| N5 receiver geometry-free | `I(r,s) += iota` for every s of g at receiver r; `B_{r,j} -= mu_j iota`; `b_{r,j} += mu_j iota` | one per (receiver, system) |
| N6 satellite geometry-free | `I(r,s) += kappa` for the receivers of one class; `B^s_j += mu_j kappa`, `b^s_j -= mu_j kappa` on that class's signals | one per (satellite, class) |
| N7 receiver phase bias against ambiguity | `N(r,s,j) -= a` for all s; `b_{r,j} += lambda_j a` | one per tracked (receiver, signal) |
| N8 satellite phase bias against ambiguity | `N(r,s,j) += a` for all r of one arc-graph component; `b^s_j += lambda_j a` | one per (satellite, signal, component) |

Dependencies among them:
- (i) a constant N1 is a combination of N2, N3, N4, N7 and N8: one per system;
- (ii) N5 with `iota = c` over all receivers of one class combined with N6 with `kappa = -c` over the same class lies in N4 + N7 + N8: one per class.

N5 is per (receiver, system) because the receiver code biases are keyed by system (`key.sys = system`, src/core/measurement/ReceiverBias.cpp:224-226): the ionosphere offset between systems at one receiver is therefore not separable from the biases. This settles the derivation's open question on the keying.

N7 and N8 together span `n_c + m_c` directions per component of the receiver-satellite arc graph of each signal (n_c receivers, m_c satellites). Equal `a` in both is a common shift of receiver and satellite phase bias, a true null direction, not a dependency.

[corrected 2026-11-19: see Corrections, item 3] The zenith wet delay adds no defect when the mapping function is not additively separable: if `m(r,s) = f_r + g_s` for all lines of sight, the shift `T_r += t` (one t for every receiver) with `dt_r -= f_r t` and `dt^s += g_s t` leaves every row unchanged: one direction per epoch. Co-located receivers are one instance; the review notes the condition is wider than co-location. For a network of separated stations it holds only approximately (a near-defect whose size was not measured).

## 3. Defect count

With R_G the number of (receiver, system) pairs, `c_s` the number of classes in which satellite s is observed, `C_g` the number of classes of system g, and the phase components counted per signal:

```
D = K G + 2 R_G + s + sum_s c_s + F + sum_j sum_components (n_c + m_c) - G - sum_g C_g
```

When every receiver tracks every signal of its system (one class, one component per signal) this reduces to the derivation's form

```
D = K G + 2 r G + 2 s + F + r F + sum_g s_g f_g - 2 G
```

The reduced form does not hold under partial tracking: on the small case of section 6 it gives K + 44, while the true defect is K + 38 (review, confirmed by the rank computation).

## 4. The channel term

On band b of system g, let the receivers fall into C classes on disjoint receiver sets. Write the satellite code bias of the channel used by class c as `B^s_{j(c)} = B^s_b + d^s_{b,c}`.

**4a. Levels.** `d^s_{b,c} += beta^s` for all c with `B^s_b -= beta^s` is s directions per band; `d^s_{b,c} += alpha_c` for all s, absorbed by the class's receiver biases, is C directions per band, one of which is N4. The class term adds `s + C - 1` per band.

[corrected 2026-11-19: see Corrections, item 6] **4b. Ionosphere.** When a class differs on both bands of a pair (combined tracking of E1 and E5a), N6 applies per (satellite, class): the per-satellite geometry-free part of `d` is collinear with that class's slant ionosphere. The invariant part of the class difference is then its ionosphere-free combination differenced between satellites: s-1 values per class and band pair, not 2(s-1). When the class differs on one band only, the s-1 values sit on that band.

**4c. Phase.** A phase class term per (satellite, band, class) is collinear with the ambiguities of that class's arcs (N8 restricted to the class). It is never estimable in a float solution and appears only through the integer property (section 7).

**4d. Separability of a bias between two channels of one band.**

| Situation | What the data determine |
| --- | --- |
| Common part of a channel | Never separable from the receiver biases; one level row per channel (the sum-to-zero row) |
| Some receiver tracks both channels of a satellite | The channel difference is free of clock, geometry and ionosphere (equal frequency); the satellite-differenced bias is directly estimable. This is the overlap test `isc_bridged` (src/core/network/solve/Sbasis.cpp:1689-1692) |
| Disjoint receiver sets, other band shared | [corrected 2026-11-19: see Corrections, item 6] Through the shared satellite clock only, at code precision: s-1 values on the differing band |
| Disjoint receiver sets, both bands differ | [corrected 2026-11-19: see Corrections, item 6] Only the ionosphere-free combination (s-1 values); the geometry-free part needs a datum row |
| Phase | Never in a float solution; only through integer double differences |

**4e. Joint against separate.** With a satellite bias per signal, the class term is free, and the classes are joined only through the per-epoch satellite clock. One satellite phase bias per band with a constrained class term is what makes the classes one integer network.

## 5. Minimal S-basis

| Row | Content | Count | Generator removed |
| --- | --- | --- | --- |
| S1 | one satellite clock pin per system per epoch | K G | N1 |
| S2 | receiver ionosphere-free code combination of its two clock-pair signals = 0 | one per (receiver, system) | N2 |
| S5 | receiver geometry-free code combination = 0 (the ionosphere then carries the receiver inter-frequency code bias) | one per (receiver, system) | N5 |
| S3 | satellite ionosphere-free code pin | s | N3 |
| [corrected 2026-11-19: see Corrections, item 4] S6 | satellite geometry-free code pin at a target (broadcast group delay or zero) | one per (satellite, class) | N6 |
| S4 | sum-to-zero over satellites, one per extra signal beyond the pair | F - 2G (full tracking) | N4 |
| S7 | receiver phase bias lumped into the ambiguities (`b_{r,j} := 0`), or one pin per component plus a tree (chain form) | per tracked (receiver, signal) | N7 |
| S8 | [corrected 2026-11-19: see Corrections, item 5] one anchor ambiguity per (satellite, signal) per component: a spanning-forest condition, not one receiver (inferred from N7, N8) | per (satellite, signal, component) | N8 |
| S9 | `d^s_{b,ref} := 0` for one reference class per band | s per band | 4a |
| [corrected 2026-11-19: see Corrections, item 4] S10 | sum over s of `d^s_{b,c}` = 0 for each other class | C - 1 per band | 4a |
| [corrected 2026-11-19: see Corrections, item 4] S11 | where a class differs on two bands, the geometry-free part of `d` = 0 per satellite | s per class and band pair | 4b |

The repository's rows map onto S1, S3, S4, S6 and S8 (docs/report/network.tex:597-602, 640-652, 811-866). S2 is realized by the receiver DCB parameterization. S5 has no hard counterpart (section 8). S11 is the same rule the code already applies to extra signals, one geometry-free pin per extra signal and satellite (network.tex:858-860).

## 6. Small case

One system, s = 3 satellites, two bands, r = 4 receivers: A1 and A2 track the pilot signals P1, P2 of both bands; B1 and B2 track the combined signals X1, X2. Satellite biases per signal (the same estimable space as band biases plus a class term without S11).

- Per epoch: `dt_r` 4, `dt^s` 3, `T_r` 4, `I` 12, total 23.
- Constant: `B_{r,j}` 8, `B^s_j` 12, `b_{r,j}` 8, `b^s_j` 12, `N` 24, total 64.
- Observations: 4 x 3 x 2 x 2 = 48 per epoch.
- Defect: N1 K, N2 4, N3 3, N4 4, N5 4, N6 6, N7 8, N8 12, minus (i) 1, minus (ii) 2: D = K + 38. Rank 22K + 26.

| K | unknowns | observations | rank | defect | redundancy |
| --- | --- | --- | --- | --- | --- |
| 1 | 87 | 48 | 48 | 39 | 0 |
| 2 | 110 | 96 | 70 | 40 | 26 |
| 3 | 133 | 144 | 92 | 41 | 52 |

The rank computation reproduces every row of this table. An independent split by the review (geometry-free part: rank K+1 per line of sight; ionosphere-free code: 11K+6 unknowns with defect K+4; ionosphere-free phase: one constant per line of sight) gives the same 22K + 26.

Hand checks at K = 1: 24 phase rows hold 24 ambiguities; 43 code unknowns minus 19 code defects give 24, equal to the 24 code rows.

S-basis for the case, 38 constant rows plus K clock pins: S2 4, S5 4, S3 3, S6 6 (pilot pair and combined pair per satellite), one class level for the combined class (S10, on the combined pair's ionosphere-free combination), S7 8, S8 12 (anchor A1 for the pilot signals, B1 for the combined signals). With K = 2 the 40 rows leave nullity 0 in the rank computation.

**The recorded design of the per-make term.** In the band-plus-class form with `d` on the combined class, satellite pins only on the band biases and the recorded constraint "zero mean over the satellites of each (band, class)" (CHA part3 line 91), the rank computation leaves nullity 2 = s-1. Adding the per-satellite geometry-free row on `d` (S11) brings it to 0. A combined-make class that differs on both bands therefore needs S11, or one ionosphere-free value per satellite, or the term on one band only. (The derivation's sentence on this count was garbled; it is replaced by this statement.)

**The chain-datum case.** One system, two signals, four receivers all tracking both, three satellites, receiver code bias as one DCB column per receiver with the code's coefficients. The rank computation gives defect K + 24. The code's hard rows (one clock pin per epoch, satellite ionosphere-free and geometry-free pins, one receiver phase-bias pin at the datum receiver per signal, six tree ambiguity pins per signal) number K + 20 and leave nullity 4 = r. A hard DCB = 0 row at every receiver closes it; the same row at every receiver except the datum receiver leaves nullity 1. The softly held space is r directions per system, not r-1.

## 7. What is estimable and what is integer

Under S1 to S11 the estimable quantities are:
- satellite clocks relative to the pivot, carrying the satellite ionosphere-free code bias;
- receiver clocks and inter-system offsets per epoch, carrying the receiver ionosphere-free code bias;
- slant ionosphere plus the receiver and satellite geometry-free code bias (relative to the S6 target), per class where 4b applies;
- zenith wet delay, for a mapping function that is not additively separable;
- receiver code biases of extra signals, as differences between receivers;
- the lumped satellite phase bias `b^s_j - lambda_j N(r0,s,j)` plus code-bias terms (network.tex:528-535);
- [corrected 2026-11-19: see Corrections, item 6] the class code term: s-1 values per class (ionosphere-free where the class differs on both bands).

The receiver geometry-free code bias differences between stations are not estimable while the slant ionosphere is free per line of sight and unconstrained (N5 per receiver).

Estimable ambiguities are `Ntilde(r,s,j) = N(r,s,j) - N(r0,s,j) + c_{r,j}` with `c_{r,j}` real, per receiver and signal.
- **Integer:** differences between satellites of one system and one phase signal family, `Ntilde(r,s,j) - Ntilde(r,p,j)` (double differences), inside one arc-graph component.
- **Inside one class:** integer.
- **Across classes, with one satellite phase bias per band:** integer only when the class phase term, differenced between satellites, is a whole number of cycles. A fraction biases every double difference across classes. Section 107.8 measured a per-satellite wide-lane term of the Trimble class of 0.026 cycle rms, at most 0.07 cycle (CHA part3 line 91).
- **With a satellite phase bias per signal:** no double difference across classes exists; the classes are two integer networks.
- [corrected 2026-11-19: see Corrections, item 7] **New arcs** add an ambiguity each and add a defect only when they split a component of the arc graph.
- **Across systems:** none. Graphs are per (system, signal) (docs/report/network_carry.tex:415; ambiguity_resolution.tex:734). The inter-system offset enters phase rows only to be absorbed by the ambiguities (and by the receiver phase bias under the chain), so GPS L1 and Galileo E1, which share a carrier, form no double difference in the present model.

## 8. What the code fixes against what the model needs

Configurations. The integer-fixing configuration requires the chain phase datum (`network.ring_fix.enable` is refused without `network.phase_datum_eq34`, src/core/config/GnssConfigLoadNetwork.cpp:11197-11201). The main reference configuration uses the prioritized anchor election (config/golden/golden_field_refound.json:354, `phase_datum_eq34` false). Both arm `rx_code_bias_datum` (line 301), `w6_iono_constraint` none (line 912), `sat_gf_datum` broadcast_tgd with time link, `second_code_channel` and `phase_colour_state` (pins list, line 8). The integer-fixing configuration sets `network.phase_datum_eq34_rpb_prior_sigma_m` to 0 (CHA part3, the list of adopted keys, lines 113-118).

Adopted signals: GPS L1C/L2W phase with codes C1C, C1W, C2W; Galileo L1C/L5Q phase with codes C1C/C5Q after the rename of the combined channels (src/core/network/driver/ApplicationNetworkSetup.cpp:245-282).

| Model direction | Code at 03dfcbc | Verdict |
| --- | --- | --- |
| N1 clock (K G) | One hard pin on one satellite clock per system per epoch; declared dimension = number of systems, checked by the rank gate (Sbasis.cpp:524-530) | Fixed once (hard) |
| N2 receiver clock against receiver biases | Inter-system offset only for systems other than the time reference (include/measurement/ObservationEquation.hpp:422-424); receiver DCB with coefficients `mu_j / (mu_1 - mu_2)`, zero ionosphere-free combination (ReceiverBias.cpp:219-228) | Fixed once (parameterization) |
| N3 satellite clock against satellite biases | Satellite ionosphere-free pin, time-linked, re-seeded to 0 at a pair change (Sbasis.cpp:1425-1468) | Fixed once |
| N6 satellite geometry-free, clock pair | Geometry-free pin at `c T_GD` (GPS) or BGD(E1,E5a) (Galileo) (Sbasis.cpp:1543-1549), target latched once per run | Fixed once; target depends on the processing window (NRF section 124) |
| N6 for BeiDou B1I/B3I | `broadcastGroupDelayDescribesPair` accepts only band digits 1/2 (1/5 for Galileo) (Sbasis.cpp:94-96); C2I/C6I falls to the zero target (Sbasis.cpp:102-105) | Fixed once; zero is a legal but conventional target |
| N4 per code signal | Sum-to-zero per (system, code signal), time-linked; suppressed when every column of the channel is pinned (Sbasis.cpp:1859-1867) | Fixed once; minimal, F - 2G rows realized (review) |
| Receiver second code (GPS C1W) | Receiver ISC, unit coefficient (ReceiverBias.cpp:108-131); satellite C1W column left free (Sbasis.cpp:1651-1656); level by the sum-to-zero row | Fixed once |
| N5 receiver geometry-free (one per receiver and system) | 10 m zero-mean prior on the receiver DCB, folded on every station block (src/core/network/carry/RxCodeBiasCarry.cpp:313-332; default sigma at GnssConfigLoadNetwork.cpp:4183); 1 m receiver phase-bias floor (src/core/network/solve/PhaseDatum.cpp:1606-1645), set to 0 in the integer-fixing configuration; carries | **Gap: no hard row.** Held by soft terms only, r directions per system (section 6) |
| N7 + N8, chain datum | Receiver phase-bias pin at the datum receiver per component, tree pins `n_c + m_c - 1` per component, w = 1e10 (PhaseDatum.cpp:1550-1575; src/core/network/solve/ChainDatum.cpp:2326) | Fixed once; `n_c + m_c` per component is correct |
| N7 + N8, anchor election | One ambiguity pin per (satellite, signal), no receiver phase bias (Sbasis.hpp:38-48) | Fixed once (S7 + S8) |
| Phase colour per line of sight | Gauss-Markov prior (birth variance, decay) | Held by its prior only; collinear with the line of sight's band-pair ambiguities (inferred) |
| Coverage-driven extra defects | S-basis completion rows on measured near-null directions (Sbasis.cpp:582); near-null phase-bias soft anchor | Per epoch; arbitrary value on whatever direction is near-null |
| Class term, Galileo before the rename (main reference configuration) | C5X takes the band-2 companion geometry-free pin against C1C; C1X keeps its sum-to-zero or family-difference row: s+1 rows | Fixed once (review) |
| Class term, adopted rename | The combined channels are renamed to the pilot names for phase and, with `phase_family_codes`, code (ApplicationNetworkSetup.cpp:245-282) | Not a defect removal: an assertion that the class term is zero |
| Class term, planned per-make design | Zero mean over satellites per (band, class) (CHA part3 line 91) | **Gap:** s-1 directions per class that differs on both bands (section 6) |
| Third band, satellite side (not in the adopted setup) | Geometry-free pin against band 1 at the same target, which the code itself says asserts an inter-signal correction of zero, metres across bands (Sbasis.cpp:1696-1720); freed where determined only under `network.sat_gf_extra_band_free` (default false) | **Over-constraint:** a value asserted where the data determine the column |
| Band-2 same-band companion (C2X beside C2W) | Pinned at `c T_GD`, asserting ISC = 0, unless `network.sat_isc_state` (default false) and an overlap receiver (Sbasis.cpp:1670-1694) | Over-constraint by default |
| Third band, receiver side, with the satellite pin | [corrected 2026-11-19: see Corrections, item 9] Receiver extra-band code delay held at its seed unless `network.estimate_rx_extra_band_code_delay`; the code notes that the geometry-free pin "repeats the same defect on both sides" (ReceiverBias.cpp:100-103) | **Double fix** of one level direction, on both sides (count inferred) |
| Frame | Coordinates held, orbits fixed | Constant |

Notes:
1. **No double fix in the dual-frequency datum.** The ionosphere-free pin, the geometry-free pin and the suppressed sum-to-zero rows remove N3, N6 and N4 once each (review, from Sbasis.cpp:1425-1431, 1543-1549, 1859-1867).
2. **The two soft holders of N5.** The comment at RxCodeBiasCarry.cpp:300 ("The between-station differences are estimable") does not hold with a free slant ionosphere and no ionosphere constraint. The code elsewhere already names the floor "the datum of the station's geometry-free direction" (PhaseDatum.cpp:1626-1632), and calls the separation of receiver phase bias, ambiguity and ionosphere "nearly rank-deficient" (PhaseDatum.cpp:1608-1611).
3. [corrected 2026-11-19: see Corrections, item 10] **The chain cross-check is bookkeeping.** `checkChainGauge` compares two counters filled in the same routine (ChainDatum.cpp:2643-2659); it presumes `n_c + m_c` rather than testing it. The presumption is correct.
4. [corrected 2026-11-19: see Corrections, item 8] **The same-band claim needs an overlap receiver.** Sbasis.cpp:1651-1656 says a same-band extra is "estimable from the data at any coverage". That holds only where some receiver tracks both codes of the satellite; with the classes on disjoint receivers the column is reached only through the satellite clock (section 4d).
5. **A possible repeated prior.** If the receiver DCB carry propagates the posterior and the 10 m prior is folded again every epoch, the prior's information grows with the epoch count (after 720 epochs an effective sigma near 0.37 m) (inferred, not checked; the record notes a double apply "would not show here", docs/record/refound/RF-PD-fix_station_prior_before_pd_2026-09-15.md:84).
6. **The receiver DCB column exists** on the main reference configuration: the record counts the DCB datum rows rising from 17 809 to 42 463 (RF-PD-fix:82), and those rows are folded only where `cb.form == ParamForm::Dcb` (RxCodeBiasCarry.cpp:313). This settles a first-stage open item from the record, not from a code trace of the .BIA path.

## 9. Gaps in the mathematical reference (docs/report)

1. **No general rank count.** The gauges are named in words (network.tex:536-555) and the rank gate checks only the clock dimension (network.tex:652). No derivation gives the null space as a function of the roster (systems, bands per system, channels per band, chain or anchor). Every multi-band and multi-channel datum row rests on code comments.
2. **The receiver prior text repeats the N5 error.** network.tex:870-872 says the prior "leaves the between-station differences to the data"; with a free slant ionosphere those differences are not estimable.
3. **The chain datum has no gauge count.** network.tex:801-809 states the pins (one receiver phase-bias pin plus a spanning tree per component) without the `n_c + m_c` count or the collinearity of the receiver phase bias with the inter-system offset and the ambiguities.
4. **The ring fix is absent** from every docs/report/*.tex.
5. **The extra-band geometry-free pin is presented as a datum choice** (network.tex:858-860); where a receiver tracks both pair bands and the extra band, the column is estimable and the pin asserts a value. The receiver band-5 delay held at its seed (network.tex:1198-1217) asserts the receiver side the same way; the reference does not join the two.
6. **The geometry-free target is a run latch,** not the broadcast value the reference states (network.tex:847-857): the first epoch depends on the processing window and the target cannot be formed from a stream (NRF section 124). The reference does state the zero fallback for other pairs (network.tex:855-857).
7. **The geometry-free scale.** The code takes mu relative to the pin pair's first signal (Sbasis.cpp:1543); the reference uses f_L1 (network.tex:839-846). Equal for GPS and Galileo; for BeiDou B1I the time-linked value differs by `f_B1^2 / f_L1^2` (harmless at the zero target).
8. **Signal families.** The rename of combined channels and its assertion (equal satellite phase and code bias across tracking modes) are not in the reference; Galileo is described with both code families as separate columns (network.tex:1138-1158), and under `phase_family_codes` the family-difference row (network.tex:891-925) has no combined column to act on.
9. **No cross-system phase tie** is derived (section 7).
10. **Not derived:** the receiver ISC under `processing.channel_mode: all` and its closure (ReceiverBias.cpp:97-131); the band-2 companion under `sat_isc_state`; the phase colour's collinearity; the ionosphere term dropped when a carrier frequency is unknown (ObservationEquation.cpp:194-195).

## 10. Disagreements and how they are resolved

1. **How N5 is held.** The first stage said soft terms hold it (including the 1 m floor) and that tree pins reach it at the datum receiver. The derivation said the soft prior fixes the common mode and leaves the station differences to the data. The review said both are partly wrong. The rank computation of section 6 sides with the review: the hard rows leave r directions per system, and the floor is 0 in the integer-fixing configuration.
2. **The defect formula.** The derivation's closed form holds only under full tracking; section 3 gives the general form (review).
3. **The wet-delay condition.** The derivation states additive separability as the condition and co-location as an instance; the review states it covers any `m(r,s) = f_r + g_s`. Both are kept; the size of the near-defect on the test networks is not measured.
4. **The DCB column.** The first stage could not settle whether it exists; the review settles it from the record (section 8, note 6).
5. **The receiver phase bias in code.** The derivation read only the reference; the first stage shows the chain form estimates one receiver phase bias per (receiver, system, phase signal) with a pin at the datum receiver (ObservationEquation.cpp:250-256), and the anchor form has none.
6. **Code or phase reading of section 107.8.** The derivation and the recorded design assume a code term; the review holds this decisive and untested. A Melbourne-Wubbena value cannot separate the two. A code term reaches the solve only through the ionosphere-free clock rows at code noise; a phase wide-lane fraction is collinear with the class's ambiguities and shows only in double differences across classes. If the term is phase, a code-term build would estimate about zero and change no integer.

## 11. Runs this work would need later (none requested)

1. A hard N5 row (or removal of the DCB column), default off: a same-build identity run with the key off; 2 h comparisons on the m1 and h40 beds with the key on and off (receiver DCB sd, rank-gate withholds, user error by hour).
2. Before the per-make term: on m1, the per-class mean of the satellite-differenced code ionosphere-free residual, and the per-class phase double-difference fractions.
3. A trace of the receiver DCB posterior sigma over epochs against a single prior application (the repeated-prior question).
4. For any third-band posture: `sat_gf_extra_band_free` and `sat_isc_state` on against off.

## Decisions for the author

1. **N5:** hold the receiver geometry-free direction with a hard row per (receiver, system) (DCB = 0, or its carried value with a time link), or remove the receiver DCB column so the receiver geometry-free code bias goes into the slant ionosphere (the same estimable space with fewer columns). Either is default off, with counters and a re-seed when a receiver arrives or returns (invariant 7).
2. **The per-make term:** one ionosphere-free value per (satellite, class), or the term on one band only, or band terms with a per-satellite geometry-free row; zero mean per (band, class) alone leaves s-1 free directions.
3. **The reading of section 107.8:** measure code against phase (section 11, item 2) before building the term.
4. **Multi-band default:** whether the extra-band geometry-free pin and the band-2 companion pin become estimability-tested by default when a third band joins the joint network.
5. **Cross-system integers:** whether to derive and build a receiver inter-system phase bias for systems that share a carrier, which the one-network principle needs for double differences across systems.
6. **BeiDou target:** keep the zero geometry-free target for B1I/B3I or latch the matching broadcast group delays.
7. **The reference:** whether to add a chapter with the general rank count, the chain count and the ring fix, and to correct network.tex:870-872 and the comment at RxCodeBiasCarry.cpp:300.

## Not checked

- Whether the integer-fixing configuration file sets `phase_colour_state` and `sbasis_complete` (not opened; CHA part3 lists the pair family and the adopted keys).
- Whether the 10 m DCB prior is counted more than once over a carried state.
- The ionosphere-free pin pair under the second code channel without a clock-reference pair (the lexical rule C1C/C2W, Sbasis.cpp:62-78), on a run.
- Which code form section 123 adopted beyond `phase_family_codes` true (NSF memo, part18), and whether the BGD target matches the renamed pair C1C/C5Q (inferred yes, matched by band digit).
- The wet-delay near-defect size on h40 and m1.
- GLONASS frequency division and the dropped ionosphere term at ObservationEquation.cpp:194-195.
- Whether the network path forces the per-band ambiguity basis (NetView, MeasurementBuilder not read).
- The rank computations use a toy geometry with random mapping values, not the test networks.
- Re-opened at 03dfcbc for this text: Sbasis.cpp:90-106, 520-532, 1640-1720, 1855-1870; ReceiverBias.cpp:90-131, 212-228; RxCodeBiasCarry.cpp:296-332; PhaseDatum.cpp:1603-1645; ChainDatum.cpp:2643-2659; GnssConfigLoadNetwork.cpp:4180-4185, 8440-8445, 11195-11201; CHA part3 lines 85-118; network.tex:801-810, 855-880. Every other path:line is taken from the first-stage results as reported.

## 2026-11-19: corrections from the check against the code

Two independent checks read this text against the code at 7fb0737 (D:/GnssProcessorSupport/worktrees/read_7fb0737, the tree meant by "7fb0737" below) and the adopted configuration D:/GnssProcessorSupport/runs_nom/_rap_ad_n146/arm.json, and repeated the rank results in exact arithmetic modulo a prime, with a float SVD rank alongside (equal everywhere). The scripts and outputs of that computation are under D:/GnssProcessorSupport/gates/post_reset_program_2026-11-18/results/_critical_read/_scratch/JNM-math/ (jnm_rank.py, jnm_rank_more.py, jnm_chain_null.py); "case" below names one of its test cases. Items 1 to 10 are statements found wrong and carry a marker in the body; items 11 to 20 complete or narrow statements that stand. The full errata are JNM-code_errata.md and JNM-math_errata.md under D:/GnssProcessorSupport/gates/post_reset_program_2026-11-18/results/_critical_read/.

1. **Section 1, the class.** The text defines a class as the receivers that track a band with the same channel. With that wording the general formula of section 3 overcounts by s-1 per split on one band (cases 5, 6). The formula holds with this definition: a class is a group of receivers of one system linked by shared signals; receivers that differ on one band and share the other form one class.
2. **Section 2, N4.** The text gives one direction per signal. Under partial visibility the count is one per (code signal, connected component of its receiver-satellite graph) (case 9e).
3. **Section 2, wet delay.** The text gives no defect unless the mapping function is additively separable, with co-location as an instance. Affine separability (`m = a_r + b_r g_s`) adds one direction per epoch (case 10b), and so does a receiver with two satellites (cases 9b, 13); a co-located pair among separated receivers adds none (case 10c), a fully co-located network one per epoch (case 10d). The condition: shifts t_r, not all zero, with `m(r,s) t_r = x_r + y_s` on every line of sight.
4. **Section 5, minimality.** S1 to S8 are minimal for one class, with S4 = F - 2G (cases 1 to 3). The table as a set (S6 per (satellite, class), S10 C - 1 per band, S11 s per class and band pair) is redundant by s + 1 rows per class (case 8: 4, 5 and 6 rows at s = 3, 4, 5). Minimal forms: per-signal form, S6 per (satellite, class) and one S10 row per extra class on its ionosphere-free combination (as in section 6); band-plus-class form, S6 on the band biases, S11 per class differing on both bands and one S10 row per such class.
5. **Section 5, S8.** The text calls S8 a spanning-forest condition. With S7 lumped, one anchor per (satellite, signal) at any receiver observing it is minimal (case 13b); the forest condition belongs to the chain form.
6. **Sections 4b, 4d rows 3 and 4, and 7, the class term.** The statement that only the ionosphere-free part is estimable (s-1 values) and that the geometry-free part needs a datum row holds when the classes share no other band (case 4: dimension 2). With a shared third band (case 6, Galileo classes split on E1/E5a sharing E5b: dimension 4) the geometry-free part is estimable too, and a datum row on it asserts a value.
7. **Section 7, new arcs.** The text says new arcs add a defect only when they split a component. A new arc between nodes already in the graph adds an ambiguity and no defect (case 9g); an arc bringing a new satellite or receiver adds that node's directions, +4 in case 9f, without a split. Adding arcs never splits a component.
8. **Section 8, note 4.** The text says the same-band satellite column is estimable only where a receiver tracks both codes. Without an overlap receiver the s-1 satellite differences are estimable through the satellite clock at code precision (case 12), as section 4d row 3 itself states; the comment at Sbasis.cpp:1651-1656 holds for the satellite-differenced column. By the same argument the band-2 companion pin over-constrains s-1 values per channel without an overlap receiver too, against the comment at Sbasis.cpp:1683-1684 (7fb0737).
9. **Section 8, row "Third band, receiver side".** The quotation "repeats the same defect on both sides" and its citation ReceiverBias.cpp:100-103 do not describe the third-band receiver delay. The comment at ReceiverBias.cpp:97-107 concerns same-band second codes under `processing.channel_mode` all; the band-3 receiver column is an IFB with unit coefficient (ReceiverBias.cpp:200-209, 7fb0737).
10. **Section 8, note 3.** The text says `checkChainGauge` compares two counters filled in the same routine. `n_pins` counts the placed pins (ChainDatum.cpp:1999, 2329, 2354) and `gauge_dim` sums the component sizes (ChainDatum.cpp:2330, 2410, 7fb0737): the check tests that each forest spans its component, and presumes n_c + m_c as the gauge dimension.
11. **Section 3, reduced form.** It holds when every receiver tracks every system and every signal of it; with a Galileo-only receiver the reduced form gives 50 against 46 true (case 11).
12. **Outcome and section 8, N5 row, the holders.** The 10 m prior is `network.rx_code_bias_datum_sigma_m`, default 10.0, set by neither the reference configurations nor the adopted configuration; `rx_code_bias_datum` is true in all (arm.json:304, golden_field_refound.json:301). It is folded on every station block before the carry (RxCodeBiasCarry.cpp:313-334). The carries are the receiver DCB carry in block and, in the integer-fixing configuration, the receiver phase-bias time link (q 3e-6 m2/s by default, GnssConfigTypes.hpp:1620, Knobs.cpp:375); no slant-ionosphere carry is armed (`carry_iono_in_block` default false, GnssConfigLoadNetwork.cpp:5173). The 1 m floor acts on the receiver phase bias, not on the DCB, and reaches r-1 of the r directions: the datum receiver's pinned bias is skipped, so that receiver's direction rests on the DCB prior alone (jnm_chain_null.py). The floor is 0 in arm.json:956 (Knobs.cpp:374, weight 0). No other row checked reaches N5: `w6_iono_constraint` none, `ionosphere_weighting` off, `estimate_iono_sh` false, `sim_zero_iono` 0 (StationAnchors.cpp:172); the S-basis completion acts on the reduced shared normals (Sbasis.cpp:582-589), which cannot see a station-local direction.
13. **Section 7 or 10.1, anchor election against chain.** In the anchor election the phase part of N5 falls on the receiver's ambiguities, held by the zero-q leveling carry; in the chain it falls on the receiver phase bias, held by its time link, the softer holder (inferred from the parameterization, not run).
14. **Outcome, section 6 and decision 2, the per-make term.** The s-1 free directions arise for a class that differs on both bands and shares no other band; a class differing on one band leaves none (cases 8, 8b to 8d). S11 with both zero-mean rows keeps one redundant row; S11 with one ionosphere-free zero-mean row is minimal. The parenthetical last sentence of the paragraph "The recorded design of the per-make term" is drafting text, not a result.
15. **Section 7, extra-signal receiver code biases.** The estimable quantity is the extra-signal receiver code bias minus its interpolation from the pair, differenced between receivers (under S2 and S5); the raw difference between receivers is not estimable (case 3).
16. **Section 8, row "Third band, satellite side".** The pin sits at the same target at first sighting only; with `sat_gf_datum_time_linked` the row then carries its own previous geometry-free combination (Sbasis.cpp:1795-1800).
17. **Section 8, N3 row.** The re-seed to 0 at a pair change follows from `network.code_datum_pair_continuity` off, its default (GnssConfigLoadNetwork.cpp:8387-8388; absent in the adopted configuration).
18. **Section 8, note 6.** The DCB column exists only where the bias product's clock-reference pairs name the system (ReceiverBias.cpp:179-180); not traced for the adopted configuration.
19. **Not checked, first item.** Settled: the adopted configuration sets `network.phase_colour_state` and `network.sbasis_complete` true.
20. **Not checked, "the lexical rule C1C/C2W".** The pair rule is band digit, then name: C1C, then the first code on another frequency by name, so C2L before C2W where both columns exist.

### Line numbers that moved

Of the code files cited, only src/core/config/GnssConfigLoadNetwork.cpp differs between 03dfcbc and 7fb0737; nineteen other citations hold at 7fb0737 within one line.

- GnssConfigLoadNetwork.cpp:4182-4183 (03dfcbc, default of the DCB prior sigma) is at 4201-4202 (7fb0737).
- GnssConfigLoadNetwork.cpp:11197-11201 (03dfcbc, ring fix refused without `phase_datum_eq34`) is at 11229-11233 (7fb0737; the mathematics check reads 11228-11232).
- The receiver phase-bias floor default 1.0 is at GnssConfigLoadNetwork.cpp:8470-8472 (7fb0737).
- ChainDatum.cpp:2643-2659 (`checkChainGauge`) is at 2645-2659 (7fb0737).
- The tree-pin weight w = 1e10 is at PhaseDatum.cpp:1551 and 1574, not at ChainDatum.cpp:2326.

### What stands

- The eight families, their counts and the dependencies (i) and (ii); the counts of section 6 (D = K + 38, rank 22K + 26, the table, the hand check 43 - 19 = 24, nullity 0 with the K + 38 rows); the chain case (defect K + 24, K + 20 hard rows, nullity r; a DCB row at every receiver closes it, every receiver but the datum receiver leaves 1). All reproduced exactly.
- The lead: in the adopted configuration nothing hard holds the receiver geometry-free direction. It rests on `network.rx_code_bias_datum` with sigma 10 m (default, unset); the receiver phase-bias floor is off (arm.json:956); no ionosphere constraint, slant-ionosphere carry or S-basis completion reaches it. The receiver geometry-free differences between stations are not estimable, so the comment at RxCodeBiasCarry.cpp:299-300 and network.tex:871-872 are wrong.
- The over-constraint of the third-band pin and of the band-2 companion pin, and the single fix of the pre-rename Galileo class rows (s + 1), hold mathematically; the code half confirms the rows of section 8 except items 9 and 10.
- Decision 1 (N5) stands; item 13 sharpens why it matters more in the integer-fixing configuration. A hard row at every receiver, not every receiver but the datum receiver, closes N5 in the rank computation.
- Decision 2 (the per-make term) stands, restricted to a class differing on both bands and sharing no other band; minimal forms: S11 plus one ionosphere-free zero-mean row, one value per satellite, or the term on one band.
- Decision 3 (code against phase reading of section 107.8) stands, not tested numerically.
- Decision 4 (multi-band default) stands, strengthened: the companion pin over-constrains without an overlap receiver (item 8), and a shared third band turns a class geometry-free pin into an assertion (item 6); items 9 and 16 do not affect it.
- Decisions 5 to 7 (cross-system integers, BeiDou target, the reference chapter) stand as choices.

### Left unconfirmed

- The F - 2G row count of N4 on a run; the statement of section 4a that the class term adds s + C - 1 per band.
- Whether the adopted run loads a bias product that creates the receiver DCB column.
- The repeated 10 m prior: supported but not confirmed. The prior is refolded every epoch (RxCodeBiasCarry.cpp:313-334) while the in-block harvest stores the posterior variance (RxCodeBiasCarry.cpp:399-427); 10/sqrt(720) = 0.37 m is right as arithmetic but ignores the carry's random walk. The DCB carry's q and the steady-state sigma were not read.
- The collinearity of the phase colour state with the ambiguities.
- The engine's own rows on a real roster (the rank computation uses toy rows); any absolute slant-ionosphere prior, which would weakly hold N5; troposphere gradients; GLONASS.
- The 11K + 6 sub-split of the review (its total is confirmed); section 9 items 3 to 10; section 10.6; the network.tex and CHA citations beyond network.tex:839-880 and CHA part 3 lines 85-118.

## 2026-11-20: status set to reference

Set to `reference` by the managing session's choice of 2026-11-20, under the author's deferral of the consolidation decisions ("I will defer to your choices on the consolidation 'decisions for the author'"); applied by the docs consolidation on branch `docs/consolidate-2026-11-20`: the card is reference text for the joint network model; nothing is owed by it.
