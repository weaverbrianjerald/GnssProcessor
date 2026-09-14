# chain-slant — Odijk rank arithmetic, the global-network chain caveat, the three self-consistent postures, and what not to re-litigate

**STATUS**

- **id:** chain-slant
- **topic:** network-vehicle
- **status:** closed
- **summary:** Odijk rank arithmetic, the global-network chain caveat, the three self-consistent postures, and what not to re-litigate
- **dates in the text:** 2026-08-04 → 2026-08-15
- **knobs / posture:** `network.phase_datum_eq34`, `network.station_pd_pin_aware`, `network.external_sat_code_bias`, `network.code_bias_gf_datum`, `network.sbasis_complete`, `network.trace_sat_bias_states`, `network.trace_rx_states`
- **hashes named in the text** (commit / exe / byte-cert): `6bf857bb`, `521512b6`, `f7fd36ea`, `cbaefe0a`
- **run dirs named in the text:** `runs/simnet300_wsh`, `runs/simnet300_wsh_n50_pin`, `runs/simnet60_tri`, `runs/simnet60_m_c_tri_slant`
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md` — §14, 14.0, 14.1, 14.2, 14.3, 14.4, 14.5, 14.6

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
## §14 THE CHAIN-SLANT BRIEF — Odijk theory, and the T1–T4 disposition
## (absorbed 2026-08-15 from `docs/HANDOFF_wsh_chain_slant.md`, now attic'd)

Reference throughout: **Odijk, Zhang, Khodabandeh, Odolinski, Teunissen (2016), *On the
estimability of parameters in undifferenced, uncombined GNSS network and PPP-RTK user
models by means of S-system theory*, J Geod 90:15–44.**  Every page, table and equation
number below is that paper's.

Theory and trap register of the work order that produced §12.8, §12.9 and §13; the plan
itself is spent (all four tasks executed 2026-08-12, the day it was written).  Its "where
things stand" preamble is not reprinted — those results are §12.2, §12.7 and §12.9.
Fixture: `runs/simnet300_wsh`, graded arm `_simnet300_wsh_n50_pin` (stride-6 = 50 stations),
466/480 emitted, 0 withheld by the observability gate.

### 14.0 T-LABEL MAP — the anchor to re-point to

Seven C++ sites and four configs cite this brief by task label.  The labels are preserved
verbatim; here is where each task's measured record lives:

| label | task | measured record |
|---|---|---|
| **T1** | audit the chain pins (declared → folded → landed per (constellation, signal)) | **§12.8 / §12.8.1** — the chain is COMPLETE; theory in §14.2 |
| **T2** | the one-knob rungs still open (`cb_kaula_prior`, warm resume) | **§12.8.3** — `cb_kaula_prior` FALSIFIED alone AND paired; warm resume MOOT |
| **T3** | the elevation-cutoff A/B with truth (15 vs 25) | **§12.9** — the truth grade INVERTS the field verdict |
| **T4** | posture decision (A / B / C) | **§13** — posture C built and measured; the posture table and its theory are §14.4 |

Code sites naming **T1** as the specification of the instrument they implement:
`include/app/NetworkRunContext.hpp:607` and `:1748`,
`include/filter/NetworkRunTypes.hpp:560`,
`src/core/app/ApplicationNetworkAssembly.cpp:2263` and `:3028`,
`src/core/filter/NetworkChainDatum.cpp:113`, `src/core/filter/NetworkTeardown.cpp:1826`.
Config sites: `_simnet300_wsh_n50_pinaudit.json:3` (T1), `_simnet300_wsh_n50_kaula.json:3`
and `_simnet300_wsh_n50_eq34kaula.json:4` (T2), `_simnet300_wsh_n50_pin_el15.json:3` (T3).
All extend the base config `_simnet300_wsh_n50.json`.

### 14.1 Odijk rank arithmetic — why the near-null is n-invariant BY CONSTRUCTION

The measured direction is **not** a code-bias problem.  It is Odijk's **rank deficiency
type 5** — *between satellite hardware biases and ambiguities*, size **f·m** (p. 23) — with
its receiver-side twin **type 4** of size **f(n−1)**.

**Eq. (24):**  `# rank deficiencies = 1 + 2f + (1 + f)(n − 1 + m)`.

The deficiency **GROWS WITH n**.  Adding receivers adds ambiguities exactly as fast as it
adds data, so **no station count can ever close a structural deficiency.**  That is the
derivation of the n-invariance §12.2 measured (ev 9.78e-06 at n = 50 vs 8.97e-06 at
n = 300, ratio 0.92, against a λmax ratio of 0.89 — 1.03e+05 vs 9.13e+04), and it exists
nowhere else in this corpus.

### 14.2 The global-network chain caveat, and the eq34 implementation it warrants

**Odijk p. 27**, quoting de Jonge (1998): the S-basis removes type 5 by constraining the
pivot receiver's ambiguities `z^s_{1,j}` for **all s and all j** (f·m of them), which a
single pivot receiver can only do if it sees **every** satellite:

> "in case of a global network, in which not all receivers have the same satellites in
> view, it is needed to constrain more ambiguities in the S-basis than those corresponding
> to one receiver and one satellite.  In that case the estimable double-differenced
> ambiguities do not necessarily follow by pivoting a single receiver and a single
> satellite, but then by pivoting a **chain** of receivers and satellites."

That is the theoretical warrant for `network.phase_datum_eq34` — **Card 3** in the loader's
own banner ("Eq-34 SINGLE-RECEIVER-CHAIN phase-bias datum ON (Card 3)",
`GnssConfigLoadNetwork.cpp:6641`) — and for §12.8's audit verdict.  Code anchors:
`src/core/filter/NetworkChainDatum.cpp:88-100,138-144` builds a spanning tree **per
(constellation, signal) component** — `(n_c + m_c − 1)` tree-edge ambiguity pins **+ 1 RPB
pin** per component, i.e. exactly `(n_c + m_c)`, the right shape for the L2W family — with
the election at `:450`.  The folds are `foldAmb` / `foldRpb` in
`ApplicationNetworkAssembly.cpp:2218-2290`, and `foldAmb`'s **silent early-return at
`ApplicationNetworkAssembly.cpp:2249-2253`** (an ambiguity with no live phase row this
epoch) is one of the three silent loss stages the §12.8 audit instrument counts.

⚠️ **THE BRIEF'S OWN PREMISE IS OVERTURNED — carry the landed mechanism, not the design
intent.**  It argued *"we already have one [a chain], and it did not close this"* (eq34
armed still emitted 0/19, rung 2; eq34 + `carry_invariant` also 0/19 cold, rung 6) and
concluded *"therefore task 1 is an AUDIT, not a build."*  **The audit was the right call and
its answer inverted the premise** — §12.8.1/§12.8.2: the chain is COMPLETE, it LIFTS the
near-null ~17× so the rank gate PASSES at default tolerance, and rungs 2 and 6 were SH-gate
withholds (a cold-LEVEL discharge, §12.8.3), never rank-gate withholds.

Two rung-2/6 confounds the brief named, both still worth knowing:
`network.station_pd_pin_aware` was **NOT** armed though the loader recommends it with eq34
("hub stations carry many 1e10 ambiguity pins, and the median-|pivot| PD guard…") — it was
armed in the audit rung; and there was **no counter anywhere** for "pins actually folded per
(constellation, signal) vs the `(n_c + m_c)` the tree needs" — standing invariant #4's exact
shape: *an armed datum that places fewer pins than it thinks reads exactly like a working
one.*  T1 built that counter.

### 14.3 "From the second epoch onwards" — the epoch-1 datum, and its inverse

Every S-basis constraint in Table 2 carries a `(1)`: `dt_1(1)`, `δ_1,j(1)`, `d_r,IF(1)`,
`d^s_IF(1)`, `z^s_{1,j}`.  **The datum is fixed at epoch 1 ONLY.**  The random-walk
constraints (**Eq. 15–19**, the `B` block) carry it forward, so a parameter that *was* the
datum at epoch 1 becomes estimable later as an increment relative to it.  Hence the split
conditions in **Tables 3/4** — CC-R receiver clock `i = 1: r ≥ 2` but `i ≥ 2: r ≥ 1`; CC-S
satellite clock `i = 1: s ≥ 2` but `i ≥ 2: s ≥ 1`, with **Eq. 35** making the first
satellite's clock minus the sum of the rest at epoch 1.  The paper states it plainly
(pp. 25–26): *"the random-walk constraints on the parameters only play a role from the
second epoch onwards."*

**The INVERSE is our risk-register item.**  Odijk **§5.3, p. 32**: remove the temporal
constraint on a parameter type and its rank deficiency **REAPPEARS AT EVERY EPOCH** — the
multi-epoch types 0a(m) / 0b(m) / 0c(m) / 3b(m), size k·… instead of one-off.  Our producer
refloats undifferenced ambiguities every epoch, and under `carry_invariant` the per-station
undifferenced ambiguity carry is **superseded for phase keys**: the DD cycle-basis z-store
supersedes "the PhaseBias shared carry + phase-leveling local carry + RPB carry + eq34
pin-round/shift/reref hygiene for phase keys" (the Card-4 producer banner,
`GnssConfigLoadNetwork.cpp:6705`), and mechanically `ApplicationNetworkAssembly.cpp:2632`'s
`do_amb_carry` requires `!carry_invariant` — restated by the loader at
`GnssConfigLoadNetwork.cpp:7752`: "Card 4's DD carry supersedes the phase-leveling ambiguity
carry".  We carry DD chords instead.  ⚠ An earlier draft cited
`GnssConfigLoadNetwork.cpp:2825-2835`; that range is the `rx_cb_carry_demean_min_stations` /
LV-5 / LV-4 block and never carried the claim.  **Line numbers re-grepped 2026-08-15 against
a working tree carrying uncommitted concurrent edits to all three files — match on the
quoted text, not the integer, if they have drifted.**  **So our type-4/5 family is in exactly
that multi-epoch mode: nothing pins it once and propagates it.**

This explains rung 6's cold failure better than "the carry has not warmed yet", and makes a
**testable prediction, recorded and still untested: a warm resume helps only insofar as it
restores a TIME LINK on the ambiguity family, not merely because state exists.**

### 14.4 The three self-consistent postures, and the f ≥ 3 epoch-1 caveat

Odijk **§5.1 (p. 29)**, after switching from vertical to slant delays: the extra rank
deficiencies **2b** (receiver biases ↔ slant iono, size n−1) and **3b** (satellite biases ↔
slant iono, size m) must be removed by constraining the *geometry-free* code biases
(Table 7), after which code biases are estimable only for a modernized triple- or
higher-frequency constellation and the geometry-free biases lump into the slant ionosphere
(quoted in full at the head of §13).  Under the **vertical** single-layer model instead
(Table 2, rd 2a/3a) only the **iono-free** biases are constrained, so the **geometry-free**
part survives: **Eq. (29)–(32)** give one estimable code-bias parameter shared by frequencies
1 and 2 — the DCB, `d̃^s_2(1) = (μ2/μ1)·d̃^s_1(1)`.

**That is exactly the trade we live in.**  Our producer keeps free per-LOS slant states *and*
ties them to a single-layer SH field softly (`representation_sigma_tecu` 5 + the T5v2 GM
deviation), so we are algebraically in the slant case with an informative prior standing in
for Odijk's hard constraint: the DCB direction is *weakly determined*, neither exactly
determined nor strictly null.  The spectrum agrees — the `CB|*|C2W` common mode sits at
**9.05e-02**, three decades above the cutoff, **never near the gauge.**

**And this is the MECHANISM of vf ≈ 4.9e+05, as theory rather than measurement.**  At two
frequencies essentially all of the satellite code bias's information is the single-layer
assumption plus the `sat_code_bias_q` time link — **not the observation geometry** — and a
parameter whose information is a soft model constraint has a formal σ describing **the
CONSTRAINT, not the error.**  1.14 m of true error against millimetre σ is what that looks
like.

| posture | satellite OSB | ionosphere | evidence |
|---|---|---|---|
| **A — SH-tied + dual (what we run)** | estimable, but its information is the single-layer assumption | biased by the single-layer model error | §12.7: cb vf 4.9e+05; exchange quoted 0.72× of ideal on the then-current script — **superseded, 0.97×**, see §14.6 |
| **B — pure slant + dual** | **none** — GF lumps into the iono; use the external `.BIA` (`network.external_sat_code_bias` already implements this) | unbiased, no field emitted | measured 2026-08-04, 63 sta, **30 ep = IGNITION, directional only**: **2.42× faster**, shared cols **403 → 167**, consumer **4.955 → 10.696 m**.  ⚠️ It also makes `sh_health_gate`, `cb_kaula_prior` and iono-deweight **INERT**.  **RETIRED by §13.5 P2** — the regression does not reproduce once a third band exists |
| **C — slant + TRIPLE** | the *modernized* DCBs `d^s_1 − d^s_j`, j ≥ 3 (**Eq. 33**), estimable **without** the single-layer crutch | unbiased | ⚠️ the brief recorded "not built: the sim writes GPS L1C/L2W only (`src/sim/main.cpp:1963`)".  **BUILT AND MEASURED the same week** — §13.1 (the three opt-in, byte-identity-preserving signal knobs), §13.3.1 (the third band removes the L2W near-null structurally; 20/20 emitted at DEFAULT `rank_gate_tol`), §13.3.2 (`network.code_bias_gf_datum` landed; the slant posture runs 20/20) and §13.5 (the settled matched A/B/C matrix) |

The brief's recommendation — *"C is the theoretically right destination and the only posture
where the OSB is data-determined **and** the ionosphere is unbiased; A is defensible provided
the σ is reported as prior-driven, which today it is not; B is a real option only if we
accept an external `.BIA` and no iono product"* — is borne out by §13.5.

⚠️ **The f ≥ 3 caveat that must not be lost: even "f ≥ 3" is an EPOCH-1 statement.**  Table
8's condition column says the slant-case changes apply *"only for the first epoch, as for
i ≥ 2 their existence is not changed"*, and Table 4 gives per-frequency satellite code biases
for **i ≥ 2, j ≥ 1**.  Random-walk links buy the per-frequency biases back as increments off
the epoch-1 datum.

### 14.5 Reproduction, and the exe-snapshot rule

The SPEC had no reproduction block for the W6+SH arm; this is it.  Runs are Release-only
(invariant #2), one CWD per arm, from a renamed exe copy.  For the build itself see
CLAUDE.md's Build section and `scripts/vs2026_build.ps1`.

```bash
python scripts/simnet_grade.py runs/simnet300_wsh \
       --run runs/simnet300_wsh_n50_pin/run \
       --products runs/simnet300_wsh_n50_pin/products.txt --settle-frac 0.5

python scripts/simnet_exchange.py runs/simnet300_wsh \
       --products runs/simnet300_wsh_n50_pin/products.txt \
       --run runs/simnet300_wsh_n50_pin/run --settle-frac 0.5
```

(The same pair against `runs/simnet300_wsh_n50_pin` / `runs/simnet60_tri` and
`runs/simnet60_m_c_tri_slant` covers the §12 and §13 arms respectively.)

**A renamed exe is a SNAPSHOT, not a symlink — re-copy AND re-hash after any rebuild.**
Identity anchors for this campaign: `gnss_processor_wsh0.exe` sha256 `6bf857bb…` (the §12.2
ladder rungs) · `gnss_processor_wsh.exe` `521512b6…` (the same tree plus the §12.4 traces) ·
`gnss_processor_wsh2.exe` `f7fd36ea…` (this brief's own arms) · `gnss_processor_wsh3.exe`
`cbaefe0a…` (the §12.9 A/B) · `gnss_processor_slant.exe` `33052874…` (§13.3.2) ·
`gnss_processor_matrix.exe` `DA2F9AC0` (§13.5 / §13.6) · `sr.exe` `6B1EF5D6` (§13.7).

### 14.6 Do NOT re-litigate (measured, in §12)

| claim | disposition |
|---|---|
| scale the roster to ~300 | **does not help** — the near-null is n-invariant (§14.1 derives why).  The 300-station fixture is still useful as a superset to stride from |
| the GIM-vs-synthetic mismatch (§11 item 3) | **MOOT** — no GIM is loaded under `estimate_iono_sh`; the two iono constraints are mutually exclusive (`ApplicationNetworkSetup.cpp:386-440`) |
| `network.sbasis_complete` | **passes the gate and DESTROYS the field** — a₀₀ **−6292 TECU** at epoch 2, SH health gate withheld **18/19**.  It constrains the extra direction *at the carried level*, and cold that level is garbage |
| the 25° iono cutoff as the cause of the 50-station withhold | **not the cause** — with the posture bundle 3/3 armed the arm still emitted **0/19** |
| the `cbpin=` S-line fill (`NetworkSolPrep`, at the contract open) | the field, its parser and its contract check **had existed since B6, but nothing ever populated the map**, so EVERY product ever written declared no IF pin.  Without it the §4.4 transform is impossible and the grader refuses to guess.  Fix carried by `docs/SPEC_product_cbpin_contract.md` and `docs/SPEC_sbasis_realization_contract.md` §168; the "since B6, never populated" provenance is recorded here |

⚠️ **TWO BIAS SIGN CONVENTIONS bit in one session.**  (i) The absorbed IF lump in the C
channel is **−d_IF** — measured **slope −1.08 / corr −0.78** — **OPPOSITE to §4.2's
originally-written `+`** (§4.2 is corrected in place).  (ii) The exchange coefficient is
**+7.93 TECU/m**, derived from `StateInitializer::resolveReceiverBias` (μ1/dμ, μ2/dμ with
dμ = 1 − γ = −0.6469), **not** from a verbal argument, which got it backwards (§12.5 P1).
**RULE: never assert a bias sign verbally — read the coefficient off the model or measure the
slope, and say which you did.**  ⚠️ **The exchange-coefficient NUMBER also moved**: this
brief printed **+5.67 TECU/m (0.72× ideal)** in two places, one a posture-comparison table,
**SUPERSEDED ON THE SAME PRODUCTS** by the current `scripts/simnet_exchange.py` at **+7.69
(0.97×)** with correlations essentially unchanged (§12.9's script-version note).  **Quote
0.97×, not 0.72×**, as the 25° settled exchange ratio.

**Instruments built for this arm** (all default-OFF, products byte-identical):
`network.trace_sat_bias_states` → `network_sat_bias_states.csv` (per (sat, code signal) bias
+ σ̂₀-scaled formal σ; **a datum-pinned column gets an EMPTY σ, never its gauge variance**) —
the only channel from which a code-bias vf can be formed, because the B record has no σ in
any configuration; `network.trace_rx_states` → `network_rx_states.csv` (receiver clock +
receiver DCB with datum-conditioned σ); `scripts/simnet_exchange.py` (the 4-channel
decomposition) and the B / receiver channels in `scripts/simnet_grade.py`.

---

