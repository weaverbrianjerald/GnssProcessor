# HANDOFF — the W6+SH synthetic arm: chain datum, and the slant-vs-single-layer decision

> ✅ **EXECUTED 2026-08-12 (same day).**  The measured record lives in SPEC
> §12.8 (T1 audit + T2 rungs) and §12.6/§12.7 updates; §13 (written
> concurrently) carries the third-band measurements this file's §3/T4 asked for.
> Headline: **T1's decision point took the "chain is complete" branch, and then
> overturned §2's premise** — the audit proves every (n_c+m_c) pin lands (1558 =
> 19×82 per signal, zero silent losses), the chain LIFTS the L2W near-null
> 9.78e-06 → 1.67e-04 (rank gate PASSES at default tolerance), and rungs 2/6
> were never rank-gate withholds at all: they were SH-gate withholds from a₀₀
> ≈ −16k…−33k TECU.  The discharge is a sim-only artifact (the sim draws
> N ∈ [1e5,1e6) cycles per band/station, so zero-target cold pins inject km-scale
> GF tension; real receivers keep phase−code aligned to metres).  That answers
> §12.3's open question: **the field emits at 303 because the chain works
> there.**  T2: `cb_kaula_prior` falsified alone AND paired; warm resume moot
> (the field's arms emit cold).  T3 (§12.9): the truth grade **INVERTS the
> field verdict** — on the coherent synthetic field 15° beats 25° on every
> clock-adjacent channel (C rms 0.49 vs 1.03 m; clk-common wander std 0.060 vs
> 0.248 m), because the cutoff FEEDS the exchange (0.50× → 0.97× of ideal
> absorption): low-elevation obliquity diversity is the separability.  The
> field's −12 % at 25° is the contamination side of the same trade; both stand.
> T4: posture A stays for graded sim arms (rung-4 tolerance); C's third band is
> already built and measured in §13 — the near-null is GONE at f = 3 and the
> sim emits at default tolerance.*

*Written 2026-08-12 for a fresh session with no prior context.  Read this file, then
`docs/SPEC_synthetic_minimum_network.md` §12 (the measured record) and §11 (the
original brief, two of whose items are now falsified — the §11 header says which).*

Reference: Odijk, Zhang, Khodabandeh, Odolinski, Teunissen (2016), *On the
estimability of parameters in undifferenced, uncombined GNSS network and PPP-RTK
user models by means of S-system theory*, J Geod 90:15–44.  Page/table/equation
numbers below are that paper's.

---

## 1  Where things stand

The W6+SH synthetic arm exists and is graded.  The fixture is
`runs/simnet300_wsh` (300 solve + 1 consumer, 480 epochs = 4 h @ 30 s, DOY 2024-129,
quiet, `corrections.signal_bias: true` so the injected COD OSB **is** the B-record
truth).  The graded arm is `_simnet300_wsh_n50_pin` (stride-6 = 50 stations),
466/480 epochs emitted, 0 withheld by the observability gate.

Three results matter here:

1. **The slice will not solve cold at the default rank-gate tolerance, at ANY
   station count.**  One extra near-null beyond the declared clock gauge: the
   **L2W satellite phase-bias common mode**, uniform ≈ 1/√N over satellites.  It is
   **n-invariant** — ev 9.78e-06 at n = 50 and 8.97e-06 at n = 300 (λmax 1.03e+05 /
   9.13e+04).  Six times the stations added no information to it.  The arm runs only
   with a per-config `network.rank_gate_tol: 1e-12`, placed from that config's own
   `network_estimability.csv`.
2. **The code-bias channel is wildly over-confident on truth**: B-record residual
   **1.144 m rms** with a variance factor of **≈ 4.9e+05** (σ optimistic ~700× *in
   σ*).  Section 3 below explains why that number is not a surprise.
3. **The clock ↔ receiver-DCB ↔ ionosphere exchange is real and lives entirely in
   the slow drift**: settled correlations −0.939 / −0.848 / +0.853 / −0.832 with
   *every* first-difference correlation ≈ 0.  Measured exchange +5.67 TECU per metre
   of fleet-mean rx-DCB error against +7.93 derived from the model's own
   coefficients (0.72×).

---

## 2  What Odijk actually says about the near-null (this is the new part)

The measured direction is **not** a code-bias problem.  It is Odijk's **rank
deficiency type 5** — *between satellite hardware biases and ambiguities*, of size
**f·m** (p. 23) — with its receiver-side twin type 4 of size *f(n−1)*.

Two consequences follow directly and both match what we measured:

- **Eq. (24):** `# rank deficiencies = 1 + 2f + (1+f)(n − 1 + m)`.  The deficiency
  **grows with n**.  Adding receivers adds ambiguities exactly as fast as it adds
  data, so no station count can ever close a structural deficiency.  That is the
  n-invariance, explained.
- **The global-network caveat (p. 27):** the S-basis removes type 5 by constraining
  the pivot receiver's ambiguities `z^s_{1,j}` for **all s and all j** (f·m of
  them).  A single pivot receiver can only do that if it sees every satellite —
  and Odijk says so explicitly: *"in case of a global network, in which not all
  receivers have the same satellites in view, it is needed to constrain more
  ambiguities in the S-basis than those corresponding to one receiver and one
  satellite.  In that case the estimable double-differenced ambiguities do not
  necessarily follow by pivoting a single receiver and a single satellite, but then
  by pivoting a **chain** of receivers and satellites"* (de Jonge 1998).

So the instinct is right: a global network needs the chain.  **But we already have
one, and it did not close this.**  `network.phase_datum_eq34` is that chain (Card 3,
`SingleReceiverChain`), and `src/core/filter/NetworkChainDatum.cpp:88-100,138-144`
shows it builds a spanning tree **per (constellation, signal) component** —
`(n_c + m_c − 1)` tree-edge ambiguity pins + 1 RPB pin per component, which is the
right shape for the L2W family.  Yet the arm with eq34 armed still emitted 0/19
(rung 2), and eq34 + `carry_invariant` also emitted 0/19 cold (rung 6).

**Therefore task 1 is an AUDIT, not a build** (see §5).

Two confounds in those rungs to clear first, both cheap:

- `network.station_pd_pin_aware` was **not** armed, and the loader explicitly
  recommends it with eq34 ("hub stations carry many 1e10 ambiguity pins, and the
  median-|pivot| PD guard…").
- There is **no counter anywhere** for "pins actually folded per
  (constellation, signal) vs the (n_c + m_c) the tree needs".  `foldAmb`
  (`ApplicationNetworkAssembly.cpp:2249-2253`) silently early-returns when an
  ambiguity has no phase row this epoch.  Standing invariant #4: an armed datum
  that places fewer pins than it thinks reads exactly like a working one.

---

## 3  The slant-vs-single-layer question, answered with its price tag

**Yes, slant is the more honest parameterization — and at two frequencies it is
fatal to the product we are trying to emit.**  Both halves are in the paper.

Odijk §5.1 (p. 29), after switching from vertical to slant delays: the extra rank
deficiencies **2b** (receiver biases ↔ slant iono, size n−1) and **3b** (satellite
biases ↔ slant iono, size m) must be removed by constraining the *geometry-free*
code biases (Table 7).  Then:

> "the receiver and satellite code biases are **not estimable anymore for the first
> two frequencies**… code biases are only estimable for a (modernized) triple- or
> higher-frequency constellation… the geometry-free receiver/satellite code biases
> become lumped to the ionospheric slant delay."

Under the vertical single-layer model instead (Table 2, rd 2a/3a), only the
**iono-free** biases are constrained, so the **geometry-free** part survives:
Eq. (29)–(32) give one estimable code-bias parameter shared by frequencies 1 and 2 —
the DCB, `d̃^s_2(1) = (μ2/μ1)·d̃^s_1(1)`.

**That is exactly the trade we are living in.**  Our producer keeps free per-LOS
slant states *and* ties them to a single-layer SH field softly
(`representation_sigma_tecu` 5 + the T5v2 GM deviation).  So we are algebraically in
the slant case with an informative prior standing in for Odijk's hard constraint:
the DCB direction is *weakly determined* rather than either exactly determined or
strictly null.  The spectrum agrees — the `CB|*|C2W` common mode sits at 9.05e-02,
three decades above the cutoff, never near the gauge.

**And this is where the vf ≈ 4.9e+05 comes from.**  At two frequencies, essentially
all of the satellite code bias's information is the single-layer assumption plus the
`sat_code_bias_q` time link — not the observation geometry.  A parameter whose
information is a soft model constraint will have a formal σ that describes the
constraint, not the error.  1.14 m of true error against millimetre σ is what that
looks like.

Three self-consistent postures, pick deliberately:

| posture | satellite OSB | ionosphere | evidence |
|---|---|---|---|
| **A — SH-tied + dual (today)** | estimable, but its information is the single-layer assumption | biased by the single-layer model error | running; cb vf 4.9e+05, exchange 0.72× ideal |
| **B — pure slant + dual** | **none** — GF lumps into the iono; use the external `.BIA` (`network.external_sat_code_bias` already implements this) | unbiased, no field emitted | measured 2026-08-04: 2.42× faster (shared cols 403→167) but consumer 4.955 → **10.696 m**, and it makes `sh_health_gate`/`cb_kaula_prior`/iono-deweight inert (30 ep = ignition, directional only) |
| **C — slant + TRIPLE** | the *modernized* DCBs `d^s_1 − d^s_j`, j ≥ 3 (Eq. 33), estimable **without** the single-layer crutch | unbiased | not built: the sim writes GPS L1C/L2W only (`src/sim/main.cpp:1963`) |

**Recommendation.**  C is the theoretically right destination and the only posture
where the OSB is data-determined *and* the ionosphere is unbiased.  A is what we
run and is defensible provided the σ is reported as prior-driven — which today it is
not.  B is a real option only if we accept an external `.BIA` and no iono product.
None of this blocks task 1, which is posture-independent.

**One caveat that must not be lost:** even the "f ≥ 3" rule is an *epoch-1*
statement.  Table 8's condition column says the slant-case changes apply "only for
the first epoch, as for i ≥ 2 their existence is not changed", and Table 4 gives
per-frequency satellite code biases for **i ≥ 2, j ≥ 1**.  Random-walk links buy the
per-frequency biases back as increments off the epoch-1 datum.  Which leads to the
second half of the theory:

## 4  "From the second epoch onwards", and why it is a risk register item for us

Every S-basis constraint in Table 2 carries a `(1)`: `dt_1(1)`, `δ_1,j(1)`,
`d_r,IF(1)`, `d^s_IF(1)`, `z^s_{1,j}`.  **The datum is fixed at epoch 1 only.**  The
random-walk constraints (Eq. 15–19, the `B` block) then carry it forward, so a
parameter that *was* the datum at epoch 1 becomes estimable later as an increment
relative to it.  Hence the split conditions in Tables 3/4 — CC-R receiver clock
`i = 1: r ≥ 2` but `i ≥ 2: r ≥ 1`; CC-S satellite clock `i = 1: s ≥ 2` but
`i ≥ 2: s ≥ 1` (with Eq. 35 making the first satellite's clock minus the sum of the
rest at epoch 1).  The paper states the mechanism plainly (pp. 25–26): *"the
random-walk constraints on the parameters only play a role from the second epoch
onwards."*

**The inverse is the part that should worry us.**  §5.3 (p. 32): remove the temporal
constraint on a parameter type and its rank deficiency **reappears at every epoch**
(the multi-epoch types 0a(m)/0b(m)/0c(m)/3b(m), size k·… instead of one-off).  Our
producer refloats undifferenced ambiguities every epoch — under `carry_invariant`
the per-station undifferenced ambiguity carry is *superseded* for phase keys
(`GnssConfigLoadNetwork.cpp:2825-2835`); we carry DD chords instead.  So for us the
type-4/5 family is in exactly that multi-epoch mode: **nothing pins it once and
propagates it.**

This is a better explanation of rung 6's cold failure than "the carry has not warmed
yet", and it makes a testable prediction: a warm resume helps only insofar as it
restores a *time link on the ambiguity family*, not merely because state exists.

---

## 5  The work, in order

**T1 — audit the chain pins (do this first; it is the cheapest and it decides the
rest).**  Per (constellation, signal) component, count pins actually folded vs the
`(n_c + m_c)` the tree declares, and check the **L2W** group specifically.  Sites:
`NetworkChainDatum.cpp:88-100,138-144,450` (election) and
`ApplicationNetworkAssembly.cpp:2218-2290` (`foldAmb` / `foldRpb`, note the silent
early-returns).  Emit it as a run-end counter — invariant #4 requires the number,
not the absence of a complaint.  Arm `network.station_pd_pin_aware` at the same
time (the loader recommends it with eq34 and rungs 2/6 lacked it).
*Decision point:* if the L2W component is short of `(n_c + m_c)` pins, the chain is
incomplete and fixing it is the correct cure for the near-null — no tolerance knob
needed.  If it is complete, the near-null is elsewhere and the tolerance stays.

**T2 — the one-knob rungs that are still open.**  Each ~90 s at n = 50 on the
existing fixture; configs extend `_simnet300_wsh_n50.json`:
- `network.cb_kaula_prior` (σ 3 m) — armed in the field, absent here; the BeiDou
  card measured it lifting near-null directions by construction.  Most likely
  single explanation for why the field's 303-station arm emits at default tolerance.
- warm resume (`state_snapshot_carry`) — the field lineage resumes rather than cold
  starts, so it may simply never enter the deadlock rung 6 hits.

**T3 — the elevation-cutoff A/B with truth** (SPEC §11 item 4, still owed): 15 vs 25
on the graded arm, directly comparable to the field A/B in `runs/cbq_elcut25`
(graded: raw wander 1.735 → 1.527 m, ~16 % of variance; ~84 % stays in the
clock↔CB↔iono exchanges).

**T4 — posture decision (§3).**  If C is wanted, the sim needs a third GPS band; the
band plan is already a table (`src/sim/main.cpp:1618-1643`) and the RNG draws are
per-band inside the satellite loop, so a third band must be **opt-in** or every
existing fixture's noise realization changes.

### Commands

```bash
python scripts/simnet_grade.py runs/simnet300_wsh --run runs/simnet300_wsh_n50_pin/run --products runs/simnet300_wsh_n50_pin/products.txt --settle-frac 0.5
```

```bash
python scripts/simnet_exchange.py runs/simnet300_wsh --products runs/simnet300_wsh_n50_pin/products.txt --run runs/simnet300_wsh_n50_pin/run --settle-frac 0.5
```

Runs are Release-only, one CWD per arm, from a renamed exe copy
(`build/x64-Release/gnss_processor_wsh2.exe`, sha256 `f7fd36ea…`; re-copy and
re-hash after any rebuild — a renamed exe is a snapshot, not a symlink).

---

## 6  Do not re-litigate (measured, in §12)

- **Scaling the roster to ~300 does not help.**  The near-null is n-invariant.  The
  300-station fixture is still useful as a superset to stride from.
- **No GIM is loaded** under `estimate_iono_sh` — the two iono constraints are
  mutually exclusive (`ApplicationNetworkSetup.cpp:386-440`).  §11 item 3's
  "grade the GIM-vs-synthetic mismatch" is moot.
- **`network.sbasis_complete` passes the gate and destroys the field** (a₀₀ −6292
  TECU at epoch 2; SH health gate withheld 18/19).  It constrains the extra
  direction *at the carried level*, and cold that level is garbage.
- **The 25° iono cutoff is not the cause** of the 50-station withhold: with the
  posture bundle 3/3 armed the arm still emitted 0/19.
- **Two bias SIGN conventions bit in one session.**  The absorbed IF lump in the C
  channel is **−d_IF** (measured slope −1.08 / corr −0.78), opposite to SPEC §4.2's
  written `+`; and the exchange coefficient is **+**7.93 TECU/m, derived from
  `StateInitializer::resolveReceiverBias` (μ1/dμ, μ2/dμ with dμ = 1 − γ = −0.6469),
  not from a verbal argument — which got it backwards.  **Never assert a bias sign
  verbally; read the coefficient off the model or measure the slope, and say which.**

## 7  Instruments built for this arm (all default-OFF, products byte-identical)

- `network.trace_sat_bias_states` → `network_sat_bias_states.csv` (per (sat, code
  signal) bias + σ̂₀-scaled formal σ; a datum-pinned column gets an EMPTY σ, never
  its gauge variance) — the only channel from which a code-bias vf can be formed,
  because the B record has no σ in any configuration.
- `network.trace_rx_states` → `network_rx_states.csv` (receiver clock + receiver DCB
  with datum-conditioned σ).
- **The `cbpin=` S-line fill** (`NetworkSolPrep.cpp`, at the contract open): the
  field, its parser and its contract check had existed since B6 but nothing ever
  populated the map, so every product ever written declared no IF pin.  Without it
  the §4.4 transform is impossible and the grader refuses to guess.
- `scripts/simnet_exchange.py` (the 4-channel decomposition) and the B/receiver
  channels in `scripts/simnet_grade.py`.
