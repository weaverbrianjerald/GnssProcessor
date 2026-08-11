# SPEC — the SYNTHETIC MINIMUM NETWORK (the sim half of G4)
### (a truth-graded 3/5-station fixture: the first ABSOLUTE grade the network producer has ever had, and a cheap exact bench for event response)

2026-08-11.  Companion to `docs/SPEC_sbasis_realization_contract.md` (which
defines *what* is gradeable), `docs/SPEC_clock_temporal_anchor.md` (Q3 — whose
guard population this fixture manufactures on demand) and
`docs/SPEC_cb_admission_card.md` / the CB-carry card in flight (whose guard
G-C rebases on).

**Why this exists.**  Every grade the network producer has ever received is
INTERNAL: self-consistency, datum invariance, cross-instrument agreement,
census counts.  All of them are satisfiable by a confidently wrong estimator.
The simulator's synthetic data is externally cross-validated (CSRS-PPP grades
it well), so a sim-fed network yields truth-graded ABSOLUTE error on every
estimable quantity for the first time — plus a bench where the injected event
is known exactly, which is what invariant #1 has never been able to
demonstrate on a real lineage.

**What it is not.**  It does not replace the field G4.  Event *expression* on a
real lineage is a separate question (the seeded-chain trap).  This certifies
the MECHANISM so the one expensive field replay is confirmation, not
exploration.

---

## §1 The architecture decision — N processes, one per station

**Decision: N invocations of `runSynth`, one OS process per station**, with
derived per-station seeds and the shared realization enforced by *pure
functions*, not shared state.

This is forced, not preferred.  An in-process station loop needs either N
`EphemerisProvider`s or one shared provider, and **both are structurally
blocked**:

- **N providers terminate the process.**  Each provider owns a `LogManager` by
  value and calls `initialize()`.  The second call hits
  `spdlog::register_logger("gnss_processor")` on an already-registered name and
  throws; **the catch handler calls `register_logger` again, unguarded, and the
  second throw escapes** (`src/core/logging/LogManager.cpp:145-168` — verified
  2026-08-11: the `catch (const spdlog_ex&)` block re-issues
  `spdlog::register_logger(logger)` at :167 with no guard).  Each provider
  constructor also calls `ProductCache::getInstance().clearCache()`
  (`EphemerisProvider.cpp:88-92`), and `Antenna::antennas_` is a raw reference
  into that singleton's vector (`Antenna.hpp:321`).
- **One shared provider silently corrupts three station-dependent terms.**
  `GnssCorrections::prev_windup_` is keyed by **satellite ID alone**
  (`GnssCorrections.hpp:543`) and its same-epoch branch *returns the stored
  value instead of recomputing* (`GnssCorrections.cpp:1969-2002`) — interleaved
  stations hand each other their wind-up accumulator.  Ocean loading is
  installed at ONE `site_ecef` (`EphemerisProvider.cpp:284-306`).
  `rx_antenna_type_` is one constructor-set string.  None has a per-station
  setter.

`main.cpp:3024-3042` already establishes the escape hatch: `all` re-invokes the
exe as a **child process** for `spp-reference`, with a comment naming
LogManager/spdlog and the product caches as process-global singletons.  The
fixture reuses that pattern.  Cost is N product parses; at 3-5 stations,
negligible.

**The contract option (a) must satisfy.**  Correct only if the shared
realization is a deterministic pure function of `(satellite, time, receiver
position)` with **zero RNG consumption**, so N independent processes reproduce
it bit-identically.  This already holds: `IonoDisturbance` consumes no stream
(`turbVtec` = splitmix64 over PRN+epoch-ms, `stormGain` = FNV-1a over sat id,
`IonoDisturbance.cpp:173-211`); orbit / clock / OSB are product lookups.
**Every new shared channel (§3: satellite clock error, satellite phase bias)
MUST be built the same way** — a pre-generated grid table keyed on
(sat, epoch index), or a self-contained hash like `ambN`.  Never a lazy draw.

### 1.1 The two failure modes this decision creates, and their fixes

Both are *silent* and both produce a beautiful result.

**F1 — shared seeds.**  All four seeds are fixed constants (`noise.seed`
20240315, `multipath.seed` 20240315^0xA17EC0DE, `atmosphere.seed`
20240315^0x7A0D51DE, `clock_seed` 20240315^0xC10CC0DE).  N stations would share
an identical noise / multipath / slip / ZWD / receiver-clock realization.
Between-station differences then cancel the noise exactly; the network looks
perfectly conditioned; the common receiver clock is absorbed into the
satellite-clock datum so the clock channel grades flawless while carrying no
information; σ̂₀ and every formal-σ calibration is meaningless *and beautiful*.

A subtler variant: if seeds are shared but the sky differs, `rng`/`aux_rng`
consumption is sky-dependent (3 draws per visible (sat,band),
`main.cpp:2264-2267`), so the streams are not bit-identical and an equality
A/B reads "fine" — while all stations walk overlapping stretches of ONE
mt19937_64 sequence.  **`tropo_rng` is worse**: drawn at most once per epoch
*outside* the satellite loop (`main.cpp:2083-2100`), so its consumption is
sky-INdependent and N stations get a bit-identical ZWD series — the one
nuisance the network exists to separate from clock becomes exactly degenerate
with it.

*Fix (S10):* derive all four seeds as `hash(base_seed, station_id)`.

**F2 — station-blind `ambN`.**  Verified 2026-08-11: `ambN` hashes only
`(sat[0], prn, band_idx)` (`main.cpp:1954-1975`), no station term.  Every
between-station SD/DD ambiguity would be **exactly zero** — AR reports 100 %
fix on perfect integers while measuring nothing, and no run gate flags it
(`worst_clean`, `worst_isb_clean`, `worst_if_resid` all pass).

*Fix (S5):* station enters the hash and the truth file.

---

## §2 The fixture

| property | value | why |
|---|---|---|
| date | **2024-129**, pinned | reuses the bundled SP3/CLK/BIA/nav with zero product work; a novel date needs SP3/CLK writers (large) |
| data root | **`data/_simnet/2024-129/`**, NOT `data/2024-129/` | trap #29: synthetic obs in the real date dir are picked up by PathResolver's station-name glob and mixed with real observations, with no error |
| station names | `SNA000SIM` … `SNE000SIM` (9-char, `SIM` country) | cannot collide with a real IGS 9-char id |
| constellation | GPS-only | one clock gauge; hand-auditable rank |
| signals | L1C/L2W (2 signals) | matches the IF-pin pair class |
| rate | 30 s | matches the field lineage |
| kinematic | `synth-static` | CORS geometry — the producer holds coordinates fixed, so `position_ecef` == truth exactly |
| arc | **≥ 2 h** for any LEVEL claim; 20-epoch smokes for wiring only | invariant: the level channel's convergence budget is ~2 h and this project has already had a sign reversal between 20-epoch and settled arms |
| n | **3** (rank tier) and **5** (first redundancy tier) | see §8 for what each can and cannot certify |
| geometry | continental spread, chosen as the LONGEST baselines that keep full common visibility at the mask, **certified offline** | see §2.1 |
| mask | 10° (sim default) | |

### 2.1 Geometry must be CERTIFIED, not assumed

Short baselines (< 50 km) make iono and tropo near-common-mode, DD ambiguities
trivially resolvable, and the network's entire reason for existing
unexercised.  Long baselines decorrelate them properly but risk bipartite
fragmentation of the (station × satellite) graph, and
`constexpr int kMinStationsPerSat = 2`
(`ApplicationNetworkAssembly.cpp:1494`, verified) **erases a satellite's
observations from EVERY station**, not just its shared columns.

*Procedure:* generate, then run `netDatumGraphComponents` offline over the
sim's own `truth_visibility.csv`; require **k = 1 component at every epoch**
before any arm runs.  A fragmented epoch makes `observed` = k while `declared`
stays 1 and the gate throws "under-specified" (`NetworkSbasis.cpp:400-411`),
withholding the epoch — which reads as a producer defect.

### 2.2 The two bias postures are separate arms, one knob apart

They are mutually exclusive in the loader and they grade different channels.

| arm | `estimate_sat_code_bias` | `enable_code_phase_bias` | `corrections.signal_bias` | grades |
|---|---|---|---|---|
| **INT** (integer arm) | false | true | **false** | P record as exact integers × λ (§4.3); clean ambiguity truth |
| **W6** (bias arm) | **true** (forces `enable_code_phase_bias` OFF, `GnssConfigLoadNetwork.cpp:2015-2038`) | false | **true** | B record vs injected OSB truth (§4.4) |

Rationale: under the default posture the producer *removes* the same `.BIA` the
sim injected, so **no B records exist at all** and the code-bias channel is
untestable.  Under W6 the injected OSB **is** the truth.  Conversely the sharp
integer test needs `signal_bias` OFF, because the shipped 2024-129
`COD0MGXFIN` OSB carries **377 L-signal rows** (~0.19 m ≈ 1 L1 cycle) — so the
in-code comment at `main.cpp:1964-1966` claiming *"No separate satellite phase
bias is injected, so the integers remain RESOLVABLE via the MW WL→NL cascade"*
is **FALSE whenever `signal_bias=true`**.  Fix that comment as part of S7.

### 2.3 Mandatory config hygiene (each of these silently produces a null run)

- `processing.signal_priorities` **must be present**.  Absent ⇒
  `PppModel::selectObservationTypes` returns empty for the missing
  constellation key (`PppModel.cpp:1994-2000`) and **every station contributes
  zero observations**, with no error.  Copy the block from
  `config/ppp_rtk_network_internal.json:29-32`.  (Config arrays REPLACE, they
  do not merge — `GnssConfigJsonHelpers.hpp:63-74`.)
- No `network.closed_loop.replay_products_path` key, even empty: its mere
  PRESENCE short-circuits `runNetwork` to the consumer-only replay driver
  *before* input validation (`ApplicationNetwork.cpp:185-204`).
- Arm `network.health_interval_s` and `network.product_flush_per_epoch`: only
  `health_trace` flushes mid-run; every other stream can lose its buffered tail.
- Arm the standard set per CLAUDE.md, minus what is inert at this scale (§8).
- Give every station an **identical epoch grid**: `MultiRinexSync` builds the
  schedule from the datum station's epoch list and matches others into it
  (`MultiRinexSync.cpp:307-366`); a rate mismatch silently discards epochs.
- Runs are Release-only, from a renamed exe copy, and must not overlap another
  session's field arm (check `Get-Process gnss*` first).

---

## §3 Truth channels

| quantity | today | action |
|---|---|---|
| **coordinates** | `truth_position.csv` | + `station` column.  Note its `clock_bias_s` is `setprecision(10)` ≈ 3 cm quantized — **never grade the clock from it** |
| **receiver clock** | `truth_clock.csv` (`setprecision(12)`) | + `station` |
| **ZWD** | `truth_tropo.csv` per epoch | + `station`; **per-station `tropo_rng`** (S6).  `truth_scalars.csv`'s `zwd_m` is the arc MEAN, not instantaneous |
| **slant iono per LOS** | `truth_atmo.csv` — `stec_quiet/sed/tid/turb/total` (**already SLANT** TECU), `I_L1_m`, `obliquity`, IPP | + `station`.  `obliquity` is published so double-mapping is *auditable*, not so it can be applied |
| **ambiguities** | `truth_ambiguities.csv` — station-blind, pre-slip only | **S5**: station in the hash + file; arc-start/end tow; explicit arc break at re-rise (today the integer never resets when a satellite sets and re-rises).  True value = `N_int + Σ truth_slips.delta_cycles` |
| **satellite clock** | **absent** | **S4**: `truth_satclock.csv` (week,tow,sat,clk_s,clk_err_s) + a synthetic error channel.  Without an injected error, `truth_C ≡ 0` (§4.2) — a valid null test, not a measurement |
| **satellite code bias** | `truth_bias.csv`, OSB, gated on `signal_bias` | + `source` column; + `synth_m` if a synthetic delta is injected |
| **satellite phase bias** | present but **mislabelled** (see §2.2) | fix the comment; **S7** adds a configurable synthetic bias so the P channel is controllable rather than "whatever COD shipped" |
| **receiver code bias** | **absent** — the only receiver-side bias in the model is one scalar GPS→Gal ISB | **S-rxcb**: per-(station, RINEX code) injection + `truth_rx_bias.csv`.  **NON-OPTIONAL** — see §4.5 |
| **receiver phase bias** | absent | pure gauge under the producer's basis, so zero-injected is *legitimate truth* — but it must be **stated** in `truth_datum.json` (S13), not left implicit |

### 3.1 Where the satellite clock error may and may not be injected

The tempting one-liner — adding the error inside `EphemerisProvider::clock()` —
is wrong in a way that disables the safety net.  It moves *both*
`forwardPseudorange` and `solveSpp`, so the **1 mm noise-free self-consistency
gate** (`main.cpp:2830`) passes no matter how wrong the arc is.  It also
perturbs `runSppReference` (the Part-B answer key) and trips `probe-orbit`'s
1 µs CLK-agreement gate.

Injecting into `forwardPseudorange` only fails the opposite way: the 1 mm gate
blows up at metre level and reads as a bug in the injection.

**Correct:** a new `clockTruth()` used at `main.cpp:425,429` only, with
`solveSpp` and `spp-reference` left on `clock()`, **plus a re-worded gate that
asserts the SPP residual now equals the injected error rather than 0.**

Second hazard: `|sat_clk| < 1e-12` is overloaded as the clock-*coverage* test
(`main.cpp:430`, `return false` ⇒ satellite silently dropped).  Test coverage
on the product clock; add the error **after**.

Third: `forwardPseudorange` is called **before** the elevation-mask test
(`main.cpp:2148-2150`), so any lazily-drawn RNG there fires for satellites that
are then discarded — draw count becomes sky-dependent and storm-on/off
byte-identity dies.  Pre-generate on the epoch grid.

---

## §4 The estimable-function grading map

**The governing structural fact:** the sim reuses the processor's own
correction chain, so every reused term cancels and its residual truth is
*structurally zero*.  What is gradeable is exactly (i) what the sim injects
that the producer does not model, and (ii) the datum-invariant contrasts of
what both model.

### 4.1 The method correction that makes the χ²/vf audit valid

The obvious grading move — transform the ESTIMATE into a between-satellite
contrast to kill the datum — **breaks the variance factor**.  For a contrast,
σ²_e ≠ σ²_s + σ²_s′: the two estimates share the datum and are strongly
correlated, so the marginal σ the producer publishes OVERSTATES the contrast σ
and biases vf **downward** — i.e. it makes a mis-calibrated estimator look
calibrated.  Since G-A exists precisely to ask *"is the field cb vf ≈ 10 the
estimator or the data?"*, a downward-biased vf answers it **backwards**.

**Therefore: transform TRUTH into the producer's realized basis, not the
estimate into contrasts.**

```
e_s   = estimate_s  −  S[truth]_s
vf    = mean( e_s² / σ_s² )          σ_s = the marginal, datum-CONDITIONED formal σ
```

This is valid because both sides then live in the same basis and σ_s is the
marginal variance of exactly that quantity.  It requires knowing the realized
datum exactly — realized pivot, `cbpin` pair, zero-mean membership set — all of
which the **S line publishes** and all of which the grader must READ, never
assume.

The contrast form is retained as a **datum-free cross-check** (it must agree),
which is what catches a misread datum.  Only the basis-transformed form carries
a vf.

Membership caveat: the zero-mean set is per-epoch (a code column registers only
when ≥ 2 present stations hold it, `ApplicationNetworkAssembly.cpp:1613-1620`),
so **the transform is applied per epoch with that epoch's registered set.**

### 4.2 C record — satellite clock

Wire meaning (contract §B.1): `dt̃ˢ = [dtˢ + d_IFˢ] − dt_pivˢ − A_c`, a
correction to the a-priori named in the S line's `apriori_id` — which
`ApplicationNetworkSetup.cpp:1016-1031` sets to the **resolved SP3+CLK
basenames**.  Because the sim injects that same CLK product, a-priori and truth
are the same object:

```
truth_C(s,t) = Δdtˢ(t) − Δdt_piv(t) + [d_IFˢ − d_IF_piv] − A_c
```

with `Δdtˢ` the **injected** error (S4).  With no injected error and
`enable_code_phase_bias` on, **truth_C ≡ 0 for every satellite** — the G-A null.

Cross-check (datum-free): `C(s) − C(s′) == Δdtˢ − Δdtˢ′`.  Never difference raw
`C` against a raw absolute simulated clock: that returns the datum as an error.

### 4.3 P record — satellite phase bias

Under the default legacy datum the pivot station's ambiguities are hard-pinned
to zero at w = 1e10 (`ApplicationNetworkAssembly.cpp:2220-2246`), so

```
truth_P(s,j) = −λ_j·N_piv(s,j) + b_φ^{s,j}(unmodelled) + κ_j
```

with `N_piv` the **pivot station's** injected integer at that epoch
(`N_int + Σ slips`) and `κ_j` a per-signal constant absorbed by the receiver
clock.  Grading rule, between-satellite at fixed signal:

```
P(s,j) − P(s0,j)  ==  −λ_j·[ N_piv(s,j) − N_piv(s0,j) ]  +  [b_φ^{s,j} − b_φ^{s0,j}]
```

which in the **INT arm** is *exactly* integer × λ_j.  This is the sharpest
single test in the fixture, and the reason S5 is non-optional (§1.1 F2).

### 4.4 B record — satellite code bias (W6 arm only)

Apply the producer's own two-stage S-transform **to truth** before comparing:

1. **IF lumping.** Per satellite form `a₁·b(s,j₁) + a₂·b(s,j₂)` over the pinned
   pair (`a₁ = f₁²/(f₁²−f₂²)`, `a₂ = −f₂²/(f₁²−f₂²)`, `a₁+a₂ = 1`) and subtract
   it from both of that satellite's channel values.  The pinned pair is
   `selectIfPinPair`'s **lexical** choice — read `cbpin=<const>:<s1>-<s2>` off
   the S line.  It is **not** necessarily the consumer's pair
   (`project_day129_pivot_cb_ratchet`).
2. **Zero-mean per channel** over that epoch's registered set.

### 4.5 Ionosphere — the Table-8 identity, and why rx-CB injection is mandatory

Contract §B.4: `ι̃ˢ(i) = ιˢ(i) + d̄_r,GF(datum) − d_GFˢ(datum)`.  Two terms, so
two independent assertions:

```
(a) between-satellite at one station  →  kills the receiver term:
    (ι̃_r^s − ι_r^s) − (ι̃_r^{s'} − ι_r^{s'})  ==  −( d_GF^s − d_GF^{s'} )

(b) satellite-mean at one station     →  leaves the receiver term:
    mean_s( ι̃_r^s − ι_r^s )  ==  d̄_r,GF − mean_s( d_GF^s )
```

**(b) is the headline system-level unit test of the S-basis contract.**  And it
**requires non-zero injected receiver code biases**: with `d_r,j ≡ 0` — which is
the state of the sim today — it degenerates to `0 == 0` and proves nothing.
That is why the rx-CB channel in §3 is marked NON-OPTIONAL.

A between-station SD at fixed satellite is the third form (cancels `d_GFˢ`
exactly and `d̄_r,GF` as a fleet scalar) and grades the iono FIELD directly
against `truth_atmo`.

**Do not read `network_ionosphere.csv` for any of this.**  It writes `x_r`
alone — the reconstructed *correction* about the seeded linearization point,
not absolute slant iono (`NetworkIonoDiag.cpp:89-94`).  The ZWD writer at
`NetworkIonoDiag.cpp:105-131` is the correct template: it *does* add `x_lin`.

### 4.6 ZWD — the one clean vf channel available today

`network_zwd.csv` is the only per-station state channel already emitted with a
datum-conditioned formal σ (`zwd_m = x_lin + x_r`, `std_m =
sqrt(conditionalLocalVarianceDiag)`, `NetworkIonoDiag.cpp:105-131`).  ZWD is
not gauge-bearing — grade it **absolutely** per station against
`truth_tropo.csv[station]`, and use its `std_m` for the χ² calibration the
C/P/B channels cannot supply as cleanly.

### 4.7 Consumer position — the only fully datum-free absolute output

`logs/closed_loop_trace.csv` logs the error vector directly
(`pos_err_3d_m,east_m,north_m,up_m`, `NetworkEmitStore.cpp:649-703`) against
the held-out station's marker→ARP-reduced truth.  A synthetic station with zero
eccentricity grades identically to its configured ECEF.  It is AR-confounded,
so it is an **end-to-end verdict, not a channel diagnostic**.

### 4.8 The one export gap, and how it is closed WITHOUT touching the wire

There is **no formal σ on the B record in any configuration** — verified
2026-08-11, the legend is literally
`# B,sat,signal,code_bias_m[,code_disc]` (`NetworkProductIO.cpp:549`, writer at
:649).  So the CB variance factor — the question G-A exists to answer — is
structurally unobtainable from the product.

Adding a `sigma=` token to the B record would change the **product grammar**
and move any consumer fingerprint keyed on the header legend.  Rejected.

**Instead:** a new default-OFF trace CSV `network_sat_bias_states.csv`
(per-(sat,signal) estimate + conditional σ), built on the same
`reconstructStation` + `conditionalLocalVarianceDiag` pattern
`network_zwd.csv` already uses, behind `network.trace_sat_bias_states`.
Trace-only, default-OFF, structurally byte-identical when OFF, **no product
grammar change**.  A sibling `network_rx_states.csv`
(`network.trace_rx_states`) does the same for the receiver clock and receiver
code bias, which today appear only as census *labels* carrying standardized
prior residuals, never as estimates with σ.

---

## §5 The rank assertion — and why the obvious one is worthless

**`declared_dimension` is NOT the Odijk Eq.-24 count.**  It is the *residual*
shared null-space expected AT THE GATE: `plan.declared_dimension =
plan.shared_datum.size()` = one pin per constellation
(`NetworkSbasis.cpp:159-188`), so GPS-only it is **1**, with **zero dependence
on m, n, or f**.  Asserting it alone certifies nothing.

Worse, **the gate is self-consistent by construction**: it compares `observed`
against `declared`, which the same code path produced.  Flipping
`clock_datum_sum_to_zero` moves *both* sides 1 → 0 and the gate still passes.
"The gate did not throw" says nothing about which datum was realized.

Where the other Eq.-24 classes go — none reach the gate:

| Odijk class | size here | closed pre-gate, where |
|---|---|---|
| 1a rx↔sat clocks | 1×#const = **1** | the PIN — this is the 1 the gate sees |
| 4/5 phase bias ↔ ambiguity | f·m = **2m** pins | inside the pivot station's block at w=1e10, then **Schur-eliminated** (`ApplicationNetworkAssembly.cpp:2220-2246, 2326-2343`) — never reaches the shared border |
| 1b/3a sat CB IF pin | m | **absent** in the INT arm (`estimate_sat_code_bias` false); folded **before** the gate in W6 (`NetworkSolveGlue.cpp:251-305`) |
| 1b/3a channel gauge | f = 2 | same |
| 2a/2b rx clock / rx CB | local | rx clock white; `rx_code_bias_datum` is a per-station **loose diagonal prior**, not a shared row — both Schur-eliminated |
| 2b/3b iono ↔ GF | — | not constrained by design (contract §A row 5) |

**So `declared_dimension == 1` in BOTH arms and at BOTH n = 3 and n = 5.**

### 5.1 What to actually assert

1. `plan.declared_dimension == 1`, **and** `observed == 1` via
   `assertNullSpaceRank(N, 1, &observed, est.rankGateTol())`.
   ⚠️ **Pass `est.rankGateTol()` explicitly.**  The `tol` default in the
   *signature* is 1e-6 (`NetworkSbasis.hpp:690-692`) while production always
   passes `NetworkEstimator::kNullSpaceTol` = **1e-9**.  A test that omits the
   argument exercises a 1000× looser gate than any real run.
2. **Independently of the gate**, assert the *installed datum row inventory* at
   a NAMED epoch with a KNOWN visible set (the sim publishes it in
   `truth_visibility.csv`) — pins per class, matched against the contract §A
   table.  The eq34/pivot pins are **elected per epoch** and installed only when
   the state exists, so this is only meaningful at a pinned epoch.
3. Assert **k = 1** bipartite component at every epoch (§2.1).
4. Do NOT arm `sbasis_complete`: it makes the gate read `observed == declared`
   unconditionally by inspecting the *completed* system
   (`NetworkSolveGlue.cpp:575-597`), so any fixture measuring realized dimension
   through the gate reports the completion's success instead of the deficiency.

---

## §6 The gates

Arms are one knob apart with **pre-registered predictions**.  Sim runs are
seconds; that is a reason for more arms, not looser method.

### G-A — the NULL TEST (no events)

Quiet sim day, both arms, n = 3 and n = 5, ≥ 2 h.

| channel | statistic | PASS | pre-registered prediction |
|---|---|---|---|
| C (clock) | max abs, basis-transformed | ≡ 0 to numerical precision | exactly 0 — a-priori *is* truth (§4.2) |
| C contrast | max abs | 0 | must agree with the above; disagreement ⇒ datum misread |
| P (phase bias) | frac cycle of the SD | < 0.01 cyc | integer-exact in the INT arm |
| B (code bias) | RMS after S-transform, W6 arm | — | **unknown — this is the measurement** |
| B | **vf** vs `network_sat_bias_states.csv` σ | — | **THE headline number.** Field cb vf ≈ 10.  If sim vf ≈ 1 with known quasi-static truth, the field mis-calibration is DATA / model-of-data, **not** estimator |
| iono (a) | between-sat identity | residual < mm | structural |
| iono (b) | **Table-8 fleet-mean identity** | holds EXACTLY | the system-level unit test of contract §B row 4 |
| ZWD | vf vs `network_zwd.csv` `std_m` | — | the cleanest available vf; a second opinion on the CB number |
| consumer | 3D error | noise floor | |
| wire census | discontinuities | ~0 | |
| rank | §5.1 items 1-3 | all pass | |

**Any systematic residual vs truth here is a MODELING FINDING — report it, do
not tune it away.**

### G-B — event injection, one class at a time, magnitude sweeps at known epochs

Classes: satellite clock step (Q3's guard population) · CB step/ramp (the
DOY-129 ratchet class) · cycle slips (slip funnel) · iono ramp (turbulence) ·
station outage/return (set-change / staleness).

Per event, grade: **detection epoch (latency)** · **estimated vs injected
magnitude** · **wire mark tie-out** (event ↔ R/counter ↔ consumer reaction, the
`scripts/q3_mark_tieout.py` pattern) · **consumer transient vs truth**.

Report the **smallest reliably caught step per guard** — an empirical MDB.
**Thresholds stay FIELD-derived; the sim measures detection floors, it never
tunes them.**

Mandatory negative control: **an injected event that produces zero guard trips
is a WIRING FAILURE, not a quiet day.**  The grader asserts this explicitly
(the `q3_ladder_grade.py:126-127` INERT-line pattern).  This fixture is the one
place where that distinction is provable.

### G-C — the G4 bench

Q3 anchor at rung-2 posture (q = 3e-7) plus the CB guard armed **in sim**;
injected events at DOY-129-like magnitudes must **trip, re-solve, mark**, and
the consumer must **ride through** — invariant #1 demonstrated against known
truth.

*Coordination:* the CB-carry card is in flight and **lands first**; G-C rebases
on its guard rather than inventing one.

---

## §7 What this fixture CANNOT certify (read before quoting it)

Honesty section.  Several QC tiers are **structurally inert** at this scale, so
"armed and never fired" here means nothing:

| tier | default | at n=3 | at n=5 |
|---|---|---|---|
| `raim_min_stations` | 3 | requires **unanimity** — inert | real 3-of-5 ✅ |
| `cb_admission_min_stations` | 4 | folds to `max(2,min(4,n))` = 3 | 4 ✅ |
| `rx_cb_carry_demean_min_stations` | 8 | inert | **still inert** |
| `fast_clock.min_stations_per_sat` | 3 | marginal | ✅ |
| `parallel_min_stations` | 10 (strictly-greater) | serial | serial |
| `kMinStationsPerSat` | 2 (hardcoded) | **one station from binding** | comfortable |

⇒ **the 3-station fixture is a RANK / estimability instrument; the 5-station is
the first tier where QC response is gradeable at all; the rx-CB de-mean tier
needs ≥ 8 and is out of reach of both.**  Reasoning that quotes the raw
defaults (e.g. "cb admission needs 4") mispredicts what actually happens.

Also outside scope: ignition (any arm < 2 h grades the transient, not the
level); `network.apriori_source: "brdc_rts"` (hard-refuses when the cache holds
SP3/CLK, which the sim cannot synthesize without —
`ApplicationNetworkSetup.cpp:355-372`); and `sim::Verifier`, whose tolerances
are calibrated to single-receiver float PPP and which prints an
authoritative-looking `=== VERIFIER PASS ===` with silently-skipped gates
(`Verifier.cpp:294-329`).  **The network grader is a new Python script; the
Verifier's user-side exam contract is left intact.**

---

## §8 Trap register (grader must defend against each)

Beyond §1.1, §2.1, §3.1, §4.1 and §4.8:

1. **Row-index joins.** `truth_atmo/tropo/otl/slips/ambiguities` are written
   *before* `if (clean.size() < 5) continue;` (`main.cpp:2409`);
   `truth_position/clock/visibility` and the RINEX epoch *after*.  Different row
   counts. **Join on tow, never row index.**
2. **`stec_*` are already SLANT.** Multiplying by the published `obliquity`
   gives a wrong-but-plausible value that **still cancels in IF**, leaving no
   residual signature.
3. **Datum-pinned columns get no σ.** The clock pivot satellite gets no U record
   and no `P sigma=` (`NetworkSolPrep.cpp:427-492`) — a vf audit iterating
   emitted satellites silently excludes the satellite the datum lives on.
4. **Shape-dispatched, config-dependent field counts.** C writes `clock_c1_m_s`
   unconditionally under `serialize_disc_counters`, conditionally otherwise;
   P trailing fields dispatch by shape (bare int = disc counter; `sigma=`/`sigc=`
   in any order).  **Stop the positional parse at the first field containing
   `=`; read the `#` legend** (grammar-complete, enforced by
   `HeaderLegendDeclaresEveryEmittedField`).
5. **`P sigma=` changes meaning under `phase_datum_ensemble`** (becomes a
   between-satellite SD sigma, reference sat gets the group median).  Never pool
   σ across datum modes.
6. **A withheld epoch writes an H heartbeat with NO E line**
   (`NetworkEmitStore.cpp:121-143`) — a grader iterating E records reads a
   withheld era as a data gap.
7. **Blank means "did not run", never 0** (`sigma0_pf`, `pc_*`, `ddfold_*`,
   `adm_*`, `prires_*`).  Coercing blanks to 0 turns "never ran" into "measured
   zero" (invariant #4).  `network_epoch_status.csv`'s row **width** changes
   with three independent toggles — read the header.
8. **`truth_scalars.csv`'s key set is config-dependent**, and `truth_slips.csv`
   gains a trailing `source` column only when scripted slips exist.  A
   fixed-schema parser reports 0.0 defaults and a passing verdict on an unarmed
   channel.
9. **Silent overwrite.** All 11 truth writers and `obs_path` are fixed names in
   one `outdir`; two stations into one dir leaves one set and **PASSes**.
10. **Every station self-identifies as `SARV`** — `hdr.marker_name` is never
    assigned (`RinexObsWriter.hpp:49-51`).
11. **A typo'd path exits 0.** `ProductCache::loadProducts` only WARNS on empty
    orbits/clocks; a non-pivot station's RINEX parse failure is non-fatal.
12. **Antenna mismatch is a warn**, not an error — receiver PCO/PCV silently
    zeroed (`ApplicationNetworkSetup.cpp:586-641`).
13. **`mp_code` and `mp_phase` come from the SAME Gauss-Markov draw**
    (`main.cpp:2277-2278`), so MW and GF see a *deterministic* multipath
    relationship, not a random one.
14. **Shared `logs/`.** `Application::log_dir_` is hardcoded `"logs"` relative
    to CWD; two arms from one directory overwrite each other.  Replicate
    `q3_ladder_run.ps1`'s refuse-if-`gnss_processor*`-lives check.
15. **The sim writes no nav file into `outdir`** — outside `data/<YYYY-DOY>/`,
    every nav-dependent path (Klobuchar, broadcast anchoring, `cold_clock_seed`)
    goes silently empty while the run completes.
16. **Warm-resume fingerprinting**: an arm chain reusing `state_snapshot_path`
    can silently cold-start and read as a clean resume.

---

## §9 Build manifest

**Sim (all default-OFF / byte-identical when unarmed):** `SimConfig` gains a
`stations[]` shape + absolute storm anchor + per-station seed derivation;
`main.cpp` gains a multi-station driver subcommand, per-station identity
(marker, filename, approx pos), per-station truth fan-out, per-station ZWD
stream, station in `ambN`; new `SatClockError.{hpp,cpp}`,
`ScriptedEvents.{hpp,cpp}`, rx-bias injection, `truth_satclock.csv`,
`truth_rx_bias.csv`, `truth_datum.json`.  **Both** CMake source lists
(`CMakeLists.txt:295-305` and `:409-421` — they are maintained independently
and already differ: `EphemerisProvider.cpp` is in `gnss_simulator` but not in
`gnss_simulator_selftest`).

**Producer (trace-only, default-OFF, no grammar change):**
`network.trace_sat_bias_states`, `network.trace_rx_states`.

**Tooling:** `scripts/simnet_grade.py` (the truth grader),
`scripts/simnet_run.ps1` (runner with the zombie check),
`config/test_suite/_simnet3_*.json` / `_simnet5_*.json`.

**Tests:** a NEW file under `tests/sim/` (target `gnss_simulator_selftest`,
**separate** from `gnss_tests`), new suite name — suite names are one-file-owned
and must stay that way.

---

## §9.5 G-A RESULTS — measured 2026-08-11 (the first absolute grade)

Arms: `config/sim/simnet3_ga.json` / `_simnet5_ga.json` → `synth-network` →
`config/test_suite/_simnet3_ga.json` / `_simnet5_ga.json`, Release
(`gnss_processor_simnet.exe`), 240 epochs = 2 h @ 30 s, GPS-only, INT posture,
quiet (`storm_scale` 0), graded by `scripts/simnet_grade.py` over the settled
second half.  Producer: 240/240 epochs emitted, **0 skipped, 0 withheld**, rank
gate passed every epoch, `declared_dimension` = 1 as predicted (§5).

| channel | statistic | **n = 3** | **n = 5** |
|---|---|---|---|
| C (sat clock) | residual rms | 5.517 m | **2.183 m** |
| C | residual mean | +0.651 m | −1.318 m |
| C | contrast rms (datum-free) | 7.980 m | 2.494 m |
| C | **vf** | **768.2** | **392.3** |
| P (phase bias) | SD frac, L1C rms | 0.213 cyc | 0.350 cyc |
| P | SD frac, L2W rms | 0.213 cyc | 0.346 cyc |
| ZWD | residual **mean** | **+4.307 m** | **+0.029 m** |
| ZWD | residual rms | 4.558 m | 1.056 m |
| ZWD | **vf** | **87.5** | **6.5** |
| consumer | 3D rms | 3.645 m | 3.098 m |
| consumer | UP mean | +3.495 m | −2.701 m |

### The finding

**1. The n = 3 network is estimable but not determinable, and nothing internal
says so.**  ZWD carries a **+4.31 m systematic** against a 0.12 m truth, which
drags the consumer's height (+3.50 m of the 3.55 m error is UP).  Adding two
stations removes the bias entirely (+0.029 m).  So it is a determinability
deficiency of the minimum network, **not** a modeling error.

**2. The formal σ collapses while the true error grows.**  Median URA against
median |C − truth| over the n = 3 arc:

| epoch | 0 | 1 | 5 | 20 | 60 | 120 | 239 |
|---|---|---|---|---|---|---|---|
| median URA | 6.63 m | 2.34 m | 0.343 m | 0.242 m | 0.178 m | 0.122 m | 0.176 m |
| median \|C − truth\| | 1.44 m | 0.77 m | 1.13 m | 1.53 m | 1.95 m | 1.95 m | 3.38 m |
| implied vf | 0.05 | 0.11 | 10.7 | 15.0 | 74.3 | 257 | 371 |

The covariance says *converging*; truth says *diverging*.  This is the whole
argument for the fixture in one table — and **every internal instrument called
the run clean**: σ̂₀ = 0.0419, zero RAIM flags on all six tiers, zero withholds,
rank gate passing, no discontinuities.  A confidently wrong estimator satisfies
every internal grade this project has ever applied.

**3. It is not a correction mismatch, and not the injected biases.**  A
bisect arm (`_simnet3_bare`) with the ENTIRE optional correction stack off on
both sides (relativity, Shapiro, solid tide, pole tide, sat + rx antenna,
wind-up) and the receiver code biases removed reproduces the result to three
decimal places — C rms 5.5151 vs 5.5171 m, ZWD 4.3055 vs 4.3073 m, vf 87.46 vs
87.53.  One arm, many knobs, but the *direction* is unambiguous: the residual
does not live in the correction stack.

**4. vf remains ≫ 1 even at n = 5** (392 clock, 6.5 ZWD) — the formal σ
understates the true error by ~20× (clock) and ~2.5× (ZWD) in σ.

### What this does NOT establish

- **It does not answer the field cb vf ≈ 10 question yet.**  There is no B
  channel in the INT posture (no CB records exist), so the code-bias vf is
  unmeasured.  The W6 arm (§2.2) is required and is not yet run.
- **A small-network vf does not extrapolate.**  ZWD vf fell 87.5 → 6.5 for
  n = 3 → 5.  The field runs 63–305 stations.  Any statement of the form "the
  estimator's σ is off by X" must name its station count.
- **The P channel is not yet a sharp test.**  SD fractional residuals are
  0.21–0.35 cyc, i.e. the ambiguities have not resolved in 2 h at this scale, so
  the "exactly integer × λ" prediction of §4.3 is untested rather than falsified.
- Ignition is excluded by grading the settled half, but 2 h is the MINIMUM
  (§2); the level channel's own budget is ~2 h.

---

## §10 Verification status

Nothing in this SPEC has been run yet.  Code anchors verified 2026-08-11
against the working tree (which carries uncommitted sim work on
`EphemerisProvider`, `TrajectoryGenerator`, `SimConfig`,
`test_sim_selfconsistency`).  The four load-bearing claims were independently
re-verified with a second tool: `ambN`'s missing station term
(`main.cpp:1954-1975`), the `LogManager` unguarded re-`register_logger` in the
catch handler (`LogManager.cpp:167`), the absence of any σ field in the B-record
legend (`NetworkProductIO.cpp:549`), and `kMinStationsPerSat = 2`
(`ApplicationNetworkAssembly.cpp:1494`).
