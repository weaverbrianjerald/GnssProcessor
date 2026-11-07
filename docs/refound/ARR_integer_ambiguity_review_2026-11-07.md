# ARR: a review of the network's integer ambiguity resolution capabilities

**STATUS**

- **id:** ARR
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-07, after card SRN: an independent review of what the network producer and its user can and cannot do for integer ambiguity recovery on the golden posture (float vehicle, held S-basis with pins at zero, per-signal phase biases held at emission, no ionosphere product, user AR off), along five dimensions: the theory of integer recoverability under the realized S-basis, the producer-side machinery as built and its evidence, the integer information on the baseline wire and at the user, the user-side resolution as built, and the route to integer products with its sequence. Results land below the pre-registration.
- **source:** main `adef08b` (worktree `D:/GnssProcessorSupport/worktrees/scirev`); the unmerged integer-datum branch `claude/distracted-germain-d93ac2` (cards IRDB to IRDE, read without checkout); the baseline pair `runs/_rf0hv17_{field,sim}6`; the grades of card SRN section 3.6 and `D:/GnssProcessorSupport/srn/grades/`
- **dates in the text:** 2026-11-07
- **knobs / posture:** the goldens as generated; `network.ambiguity_resolution.*` (log-only probe), `network.phase_datum_eq34*` (off), `closed_loop.ar_mode` 0; nothing armed or changed by this card
- **hashes / run dirs:** main `adef08b`; `runs/_rf0hv17_field6`, `runs/_rf0hv17_sim6`; `runs_nom/_rap_srn_*`

**Outcome.** Question: under the realized S-basis and today's emission, which integers can a user recover from the products, which cannot and why; what does the integer information on the wire look like by lane, pair and age; what has the producer-side machinery of cards ARL to IRD established and what is reusable; what does the user's own resolution do when armed; and which route leads from float products to integer-recoverable ones within the author's sequence and the record's verdicts? The review reads code, cards and the baseline telemetry, runs no new arm of its own, and respects the standing verdicts (producer AR not built as a product mechanism in its cascade and re-pin forms, the FFR gate, the datum do-not-retry list). Pending: results below.

## 1. Pre-registration (2026-11-07)

### 1.1 The five dimensions

| # | dimension | the questions |
| --- | --- | --- |
| E1 | theory of integer recoverability | the user's between-satellite single-difference ambiguity as a function of the true integers, the datum receivers' integers and the realized pins; the condition for integerness (Odijk 2016 Eq. 34, the record's eq34 gate); which of today's mechanisms preserve it: the pin at zero, the hold at emission across a generation, the channel families, the two constellations, the receiver phase bias, one clock for code and phase |
| E2 | the producer-side machinery | the log-only search, the arc graph and cycle basis, the lattice, the suspect mask, the detached fix-and-hold, the single-difference extra-wide-lane rows, the float vehicle; the honest-covariance question across the two eras; the integer-datum event rows of card IRD and what remains |
| E3 | the integer information on the wire | reference-free double-difference fractions by lane, pair and arc age on both beds; the wire's biases against the sim truth before and after the hold; what limits the narrow lane |
| E4 | the user-side resolution | the routes, the validation, the fail-unsafe history and what remains fixed or open; what a user needs from this wire to fix the narrow lane within minutes; the signal-family blind spot |
| E5 | the route to integer products | candidate stages (an integer-recovering datum through events; a wide-lane float-state constraint; a network partial resolution on the honest covariance as constraints in the solve; an ionosphere product; user-side resolution with the consumer veto), each with its theoretical standing, prerequisite, bed, gate and cost, ordered within the author's sequence |

### 1.2 Method

One independent reviewer per dimension (the first wave on Fable lost all five agents to the model limit at 11 minutes; the wave was relaunched on Opus), reading the worktree, the integer-datum branch and the baseline telemetry, with card SRN's grades and its verified facts supplied as premises to test (the hold, the anchor frames, the Melbourne-Wubbena test of the wire against truth, the user's AR off, the a priori weights, the clock anchor's nature). The findings are deduplicated and ranked by hand; the material ones are attacked by two referees each (the code truth; the theory and prior coverage) on the same model. Deliverables: the conditions for integer recovery with each marked met or not met on today's posture; the evidence table of the wire's integer information; the standing of each piece of built machinery; the staged route with its first build.

### 1.3 What is not re-litigated

The ensemble phase datum and the other entries of the datum do-not-retry list; the bootstrap success-rate gate with ADOP as diagnostic only; never searching pivot ambiguities; GLONASS float; the producer-side wide-lane cascade and Phase-3 re-pin as a product mechanism on the leveling vehicle (a finding that the premise changed under the float vehicle is stated as such, not acted on); the author's sequence (clock level, Galileo done, L5, lanes, seams, all channels, SH ionosphere last).
## 2. Findings (2026-11-07)

Wave 1 ran on Opus after the Fable limit: five reviewers, 54 findings, 1.46 million tokens, 352 tool uses, 28 minutes, no errors; the reports are kept at `D:/GnssProcessorSupport/srn/arr_wave1.json`. The findings below are the deduplicated result, with the reviewer ids in brackets. Fourteen of them were selected for two-referee verification (section 3), launched at 04:35; until the verdicts land, every claim below is a reviewer's reading or measurement, the numbers of section 2.3 recomputed by hand where stated.

### 2.1 The conditions for integer recovery on this wire (E1)

For a user applying the C, P and B records on signal j, the estimable between-satellite single-difference ambiguity is, in cycles,

a_u^{sp} = (N_u^s − N_u^p) − (N_{a(s)}^s − N_{a(p)}^p) + [δ_{a(p),j} − δ_{a(s),j}]/λ_j + (t_s − t_p)/λ_j + [ε_s(t0_s) − ε_p(t0_p)]/λ_j + [D_s(t) − D_p(t)]/λ_j,

where a(s) is the anchor receiver whose frame satellite s carries (a continuous handover keeps the first anchor's frame), δ is that receiver's effective phase offset on the signal (its phase bias minus what its code-defined clock, inter-system bias and code bias absorb; real-valued), t_s the pin target, ε_s(t0_s) the estimation error of the phase-visible bias at the generation start, and D_s(t) the change of the a priori clock product's phase-clock error since t0_s. The user's own receiver bias cancels. The integer condition (Odijk et al. 2016 Eq. 34; the record's eq34 gate) requires all of:

| condition | statement | on the goldens |
| --- | --- | --- |
| C1 | one datum receiver per family: δ_{a(s)} − δ_{a(p)} integer, in practice a(s) = a(p) | not met: 5 to 16 anchors per (constellation, signal) family on the field, 21 to 30 receivers carrying pins per epoch; GPS L1C and L2W anchored at different stations in 41.8 % of satellite-epochs |
| C2 | integer pin targets across handovers | not met: a continuous handover pins the successor at its float estimate (253 of 272 field handovers; successor formal sigma p50 0.11 cycle); the cold target zero is an integer and harmless while constant |
| C3 | a converged value at the generation start, ε below 0.1 cycle | not met: 175 of 219 field and 112 of 128 sim generations start at the run's first epoch |
| C4 | the a priori clock phase-consistent with truth to 0.1 cycle differentially over hours | unmeasured: the sim's clock truth is the a priori product, so D is zero there by construction |
| C5 | the same signal at user and wire for both satellites | met: the consumer applies P only to the identical signal and never a sibling |

Two of the record's statements are corrected by this derivation: the contract's section B item 2 (the P record makes user double differences integer) was written for the August eq34 posture and does not describe the prioritized held basis, and the S line declares a single-pivot-receiver datum (`amb_datum` 0, `ref_rx` the clock datum station) that the goldens do not realize [E1-F1, E1-F2, E1-F3, E1-F10].

### 2.2 The two breakers, measured on the sim against truth

**The forest mixes receiver frames.** Measured on the sim estimator (`network_amb_state.csv` against the truth integers, arcs at least 120 epochs old, the single difference taken at a non-anchor station): satellite pairs that share one anchor and never changed it read a median fraction of 0.136 cycle (L1C) and 0.143 (L5Q), 36 to 38 % within 0.1; pairs on different anchors, also unchanged, read 0.225 and 0.278, the uniform null. Cohorts with at least one continuous handover read the null even with the same current anchor. Inside the forest no target rule can meet C1 and C2 at once: rounding a successor's target steps the frame by the receivers' offset difference [E1-F2, E1-F3].

**The hold freezes the cold estimate.** Inside a generation the wire's Melbourne-Wubbena bias minus truth equals the generation-start value plus the de-commoned clock transfer divided by the wide-lane wavelength and nothing else: on the sim the per-satellite regression has slope 1.0000 and R² 0.99999996, and with the transfer removed the Melbourne-Wubbena combination is constant to 0.0007 cycle. The transfer is a clock motion written into the phase biases and not into the code biases (which are constant to the millimetre, two codes and two pins per satellite), so it moves the wide-lane combination by 5.6 (GPS) and 3.4 (Galileo) cycles peak to peak over the run. The frozen first-epoch values between satellites sit at the null (Galileo median 0.233, GPS 0.17 to 0.21 at group sizes whose null is 0.20). The producer's own double-difference wide lanes are 97 to 98 % round-correct over the same hours; the held-out user's own float wide lanes sit at the null (reference-free median 0.228 on the sim, 0.221 on the field, against 0.25) [E5-1, E1-F4, E3-4, E3-5, E4-2].

The two breakers are independent: with the hold removed the datum still fails C1 and C2 on the field, and the sim bed cannot show that, because its fixture simulates no receiver phase biases (`truth_rx_bias.csv` holds code rows only) [E5-2]. The measurement that decides which binds on each bed needs the pre-hold estimate, which main does not write; card IRD-1 built that trace default off on its branch, and the measurement is started as card SZE (section 5).

**The wire's declaration is false.** Every P record carries `signal_integer` 1 (128,610 field, 80,330 sim records), set for every non-GLONASS bias with a comment asserting that the prioritized datum keeps the integer property; the consumer's eligibility keys on the presence of a COD bias entry, so every ambiguity is eligible, uncovered signals included. A consumer that trusts the wire and arms its resolver gets wrong integers with nothing to warn it: on the golden wire with the gate off, card IRDB fixed 678 of 906 gradeable wide lanes wrong. Inert today only because `closed_loop.ar_mode` is 0 on both goldens [E5-3, E4-3, E3-6].

### 2.3 The integer information inside the producer (E3, recomputed in card SRN section 3.6)

| quantity | sim (against truth) | field (reference-free, against a size-matched null) |
| --- | --- | --- |
| wide lane, arcs over 30 min | 92 % within 0.15 cycle, 0.3 % wrong rounding; 98.4 % round-correct in the last quarter | GPS L1C−L2W 92 to 94 % within 0.15 by hour, 97 % at arcs over 2 h; Galileo L1C−L5Q 95 to 97 %, 99 % over 2 h |
| narrow lane, arcs over 2 h | 72 to 84 % within 0.15, 3 to 8 % wrong rounding, a heavy tail | Galileo E1 converges as on the sim (0.073 to 0.078 after 2 h); GPS at the null on mixed-family pairs |
| convergence law | median error falls as one over the square root of arc age (slope −0.52) | the same shape on Galileo; flat on GPS mixed pairs |

The field wide lane supersedes card AB3-0's 57.1 % (an older baseline): the frozen decision rule of AB3 passes on both lanes, reference-free [E3-2]. The GPS narrow-lane deficit on the field follows the station's GPS code family, not convergence: every GPS L2W double difference between a C1C-latched and a C1W-latched station (46 % of L2W pairs) sits at the null at every arc age, the signature of a frozen bias, while within Europe same-family pairs reach 64 % within 0.15 cycle against 50 % for cross-family pairs (Galileo 90 against 85). Galileo pairs band 1 with band 5 within one tracking family, so no Galileo double difference mixes code families; the GPS L2W double difference is the one in the posture that spans two band-1 code families, whose satellite inter-signal code bias enters the ionosphere separation of the narrow lane [E3-3]. This refines card SRN's reading of the asymmetry as pair sampling: the sampling is real, and under it a GPS code-family effect remains on common pairs. Events do not limit the producer's double differences (re-binning by the age since the last drop moves 5 % of rows and no bin); the growth of the formal narrow-lane sigma over the run in card SRN's table is the trace's reference artifact (by leg age the formal sigma falls monotonically, about 1.0 cycle under 10 min to 0.14 at 2 to 4 h, pessimistic in the core by a factor two and exceeded in the tails) [E3-7, E3-8].

The covariance: the 2026-09-10 NO-GO's premise, a carry-blind double-difference covariance, no longer holds on the float vehicle: the covariance is the exact bordered inverse of the normals the solve inverts, the carried priors enter as conditional rows, and the settled ratio of error to formal sigma on the sim is 0.7 to 0.9 [E2-1]. On the field, though, the ambiguity covariance is optimistic, not conservative: the per-unit integer census reads a chi-square(2) statistic with a 95th percentile of 9.6 to 12.0 against 5.99, so a ratio, success rate or fixed-failure-rate gate read from it overstates confidence by about 1.7 in variance with a heavy tail; the post-fit code sigma0 of card SRN says nothing about the accumulated ambiguity precision [E2-2].

### 2.4 The machinery and the user side (E2, E4)

| piece | standing |
| --- | --- |
| log-only search (star, tree, cycle basis; decorrelated partial fix) | 0 wrong fixes on the float vehicle across the ARL rungs; fixes about half of settled L1/L2 pairs on the sim; a certification layer, not a product mechanism |
| detached fix-and-hold (FCF family) | failed structurally: the hold replaces the estimate at emission, the datum is not integer-recovering, and the zero-q float already integrates the arc |
| eq34 chain with the IRD-4 event rows | built default off on the branch; the remaining blockers are the zero-mean receiver phase-bias floor (the author's D0 ruling), a truth-free rounding rule for cold pins and D-N's trigger, not event algebra |
| datum events | the golden's cold and first-sighting re-references erase every station's carry of the affected satellite (350 sim, 186 field erases; the run-end line still logs them at error level as retired); the IRD re-host cut release erases 2,944 carries per 2 h |
| blind slips | a (4, 5)-class slip the detectors miss leaves the float near a wrong lattice point at an unchanged sigma; the GF arc-jump trigger that catches it is unvalidated and triples field re-levels: a binding prerequisite of any integer build |
| single-difference extra-wide-lane rows | never reach the solve on either golden (no third band); their Route B is the template for a per-station lane row |
| a Melbourne-Wubbena float pseudo-observation as a lane add-back | rejected: it re-uses data the uncombined filter holds and double-counts them; card ARL measured the arc-averaged wide lane no better than the float's own |
| the user's armed route | a correct joint per-band integer least squares with the full filter covariance, accepted on ratio 3, bootstrapped success 0.999 and a 0.15 cycle cap; the fix is a soft pseudo-observation fed back every epoch (mode 1 is fix-and-hold, mode 2 equals mode 1, mode 3 doubles it); the real-time builder drops `enable_partial_ar` and `min_arc_info` (fixed only on the IRD branch); no per-integer record and no formal sigma at the user on main |
| time to fix with a correct wire | the truth-wire ceiling on the GPS-only ARL beds: the search opens at about 18 minutes, a sustained 5 cm at 26 minutes fixed against 44 float; minutes need an ionosphere correction in the same S-basis; the consumer inherits a kinematic position process noise from the golden |

### 2.5 The route (E5)

| stage | content | standing |
| --- | --- | --- |
| S0 | an integer-consistent emission: the wire's Melbourne-Wubbena and band combinations equal the producer's estimate in an integer-consistent frame at every emitted epoch, declared per key, with the float user protected by declared generations; first, the measurement of which breaker binds on each bed (pre-hold against held, same-anchor against cross-anchor, holds off) | the gate on every other stage; the measurement started as card SZE on the IRD-1 traces |
| S1 | the integer-recovering datum (the de Jonge chain of card IRD, every event row, on a bed with receiver phase biases) | a golden migration, blocked on the author's D0 ruling |
| S2 | a wide-lane float pseudo-observation from Melbourne-Wubbena averages | rejected (double counting) |
| S3 | network partial resolution in the solve as integer constraints | buys little precision on a zero-q float (card FCF: 6 to 7 % within-arc phase gain, the user worse); a certification layer; the standing do-not-build is respected, its premise changed but no gain shown; lanes after L5 as its wide-lane rung |
| S4 | an ionosphere correction for the user, undifferenced in the same S-basis, as a user-side weighted pseudo-observation | the largest time-to-fix lever (the producer's zero-ionosphere instrument fixes every epoch at 0.006 cycle), gated behind S0 and S1, outside the self-contained posture, the author's decision |
| S5 | user-side resolution on the closed-loop consumer with a truthful per-key declaration and the innovation screen as the frame-mismatch guard | last; on truth wires 2,506 settled integers per bed with 0 wrong |

Two cheaper options beside the stages: one band-1 GPS code family per double difference (a latch rule where both codes are tracked, or a per-family satellite code bias; a field-only 6 h arm graded by the mixed-latch pair grade) [E3-10]; and an emission-side transform of the forest's per-satellite references into one receiver's frame from the producer's own integer double differences (Odijk's S-transformation applied to estimable outputs, the integers from the lattice), which touches the standing verdict and is a ruling for the author [E1-O2].

## 3. Second-model verification (2026-11-07)

Fourteen findings (E5-1, E1-F2, E1-F3, E3-3, E2-2, E2-1, E4-4, E4-5, E5-3, E3-2, E3-7, E5-7, E5-5, E1-O2) are under two-referee verification on Opus, launched at 04:35; the verdicts are appended here as they land.

## 4. Verdict

The network holds integer information and the user never receives it. Inside the producer the float vehicle's double-difference wide lanes are integer on both beds (97 to 98 % round-correct on the sim, 92 to 99 % within 0.15 cycle on the field) and its narrow lanes converge at the code-averaging rate on the sim and on field Galileo, with a frozen bias on GPS pairs that mix the C1C and C1W code families. On the wire two independent mechanisms destroy that information: each satellite's phase bias is realized in the frame of its own anchor receiver, with float pin targets at handovers, so between-satellite differences carry two receivers' phase offsets; and the emission hold freezes each bias at its cold first-epoch value and moves it by a phase-only clock transfer, so the wire's wide-lane combination sits at the uniform null in every hour while the estimate behind it is integer. The wire nevertheless declares every bias integer. The first thing to build is not a resolver but a measurement, started tonight: which breaker binds on which bed. After it, the sequence is an integer-consistent emission, the integer-recovering datum under the author's floor ruling, lanes after L5, user-side resolution with a truthful declaration, and an ionosphere correction only by the author's decision.

## 5. Builds and measurements started

- **SZE** (started 2026-11-07 04:36 as an Opus worktree agent): the pre-hold against held grade on the golden sim, the field and the ARL bed with receiver phase biases, with the holds-off arm; its card lands under `docs/refound/`.
- Owed, not started: the GPS code-family latch arm (field, 6 h); the truth-wire and golden-wire consumer replays with the resolver armed (the positive and negative controls of the user's resolution, card IRD-1's design); the truthful `signal_integer` declaration from the datum (card IRD-7's `declare_integer_by_datum`); the shared resolver configuration builder on main (partial resolution reaches the consumer); a per-integer trace and the formal sigma at the user (card VIN of the scientific review carries the sigma).
