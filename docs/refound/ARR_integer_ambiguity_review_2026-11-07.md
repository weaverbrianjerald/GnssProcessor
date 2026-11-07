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
