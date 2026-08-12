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

### 2.4 The 50-station globally-distributed variant

Added 2026-08-11.  The 3/5-station fixtures cannot grade the QC stack at all —
all seven tiers report **zero opportunities** at n = 3 (§7, measured).  The
50-station variant exists to give them real opportunity counts, and to extend the
vf-vs-station-count trend far enough that it can be read.

| property | value | why |
|---|---|---|
| n | **50 solve + 1 held-out consumer** | above `rx_cb_carry_demean_min_stations` = 8, the highest tier threshold in the standard set |
| distribution | Fibonacci (golden-angle) lattice, \|lat\| ≤ 72° | near-uniform on the sphere; a lat/lon grid clusters at the poles |
| generator | `scripts/simnet_make_roster.py` | 50 hand-written coordinate triples is exactly where a silent error lives, and a HELD coordinate is never estimated so nothing downstream can catch one |
| posture | the **full standard armed set** (CLAUDE.md) | including `phase_leveling`, `raim_within_epoch`, `estimate_iono_sh` + `sh_health_gate`, `cold_clock_seed`, `mw_detect`, `code_bias_disc_jump_test`, and `emit_clock_disc` + `clock_disc_jump_test` |

Two deliberate deviations from a real IGS roster, stated rather than left as
unexamined properties:

- **Latitude is capped at ~72°.**  GPS is inclined 55°, so a polar station sees
  every satellite at low elevation and contributes mostly noise.  Left
  uncapped, the station-count trend would confound "more stations" with "worse
  stations".
- **Coverage is even, not land-biased.**  Real networks cluster (Europe, North
  America) and that clustering is itself a conditioning property.  Even coverage
  is the cleaner instrument for grading QC tiers; a clustered variant is a
  separate arm, not a correction to this one.

`sh_health_gate` **requires `estimate_iono_sh`** — without it the loader disables
the gate with a warning, and a disabled gate reads exactly like a clean one.

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

**MEASURED 2026-08-11, not inferred.**  The run-end flag-rate summary on the
n = 3 G-A arm reports, for all seven tiers:

| tier | opp | flag | implied α |
|---|---|---|---|
| IF-phase slip (T1) | **0** | 0 | 1.24e-15 |
| within-ep trigger (T2) | **0** | 0 | 2.70e-03 |
| post-fit RAIM cell (T3) | **0** | 0 | 1.97e-09 |
| RAIM consensus (T3) | **0** | 0 | 1.97e-09 |
| attribution SAT (T4) | **0** | 0 | 1.97e-09 |
| clock-model NIS (T5) | **0** | 0 | 1.97e-09 |
| clock-monitor (T6) | **0** | 0 | — |

**Zero OPPORTUNITIES, not zero flags.**  That is the invariant-#4 distinction and
it is the whole reason for the 50-station variant (§2.4): a tier that never got
to look at anything reads in every log exactly like a tier that looked and found
nothing.  `scripts/simnet_grade.py` now reports `opp` alongside `flag` and labels
a 0-opportunity tier INERT rather than clean.

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

## §9.7 The 50-STATION GLOBAL arm — the vf trend and the QC tiers (2026-08-11)

Fixture `config/sim/simnet50_ga.json` (roster from `scripts/simnet_make_roster.py`,
50 solve + 1 consumer, Fibonacci lattice, \|lat\| ≤ 72°), 240 epochs = 2 h,
GPS-only, quiet.

**Geometry certified before any arm ran** (§2.1): bipartite components **k = 1 at
every epoch**, 50/50 stations present, all **32** GPS satellites visible each
epoch, **minimum 12 stations per satellite** against a hardcoded
`kMinStationsPerSat` = 2, and **zero** satellite-epochs dropped.  A global roster
is where fragmentation would be expected, and it does not occur at this size.

Two arms, because one cannot answer both questions:

- **matched** (`_simnet50_matched.json`) — the SAME minimal posture as the n = 3
  and n = 5 arms, so the vf trend is one knob (station count).
- **armed** (`_simnet50_ga.json`) — the full standard armed set, so the QC tiers
  have opportunities.  Its posture is NOT comparable to the n = 3/5 arms.

### 9.7.1 The vf trend — and the two channels diverge

Matched posture throughout, so this is station count alone:

| channel | n = 3 | n = 5 | **n = 50** |
|---|---|---|---|
| C residual rms | 5.517 m | 2.183 m | **1.486 m** |
| C **vf** | 768.2 | 392.3 | **455.2** |
| ZWD residual mean | +4.307 m | +0.029 m | **+0.007 m** |
| ZWD residual rms | 4.558 m | 1.056 m | **0.896 m** |
| ZWD **vf** | 87.5 | 6.5 | **1.63** |
| consumer 3D rms | 3.645 m | 3.098 m | — |

> ⚠️ **AMENDED 2026-08-11 by adversarial review — the clock row's SHAPE is not
> readable.**  The n = 50 clock vf is **78 % of its variance in ONE common
> direction**.  Decomposing the settled C residual into fleet-common (vs pivot) ⊕
> per-satellite: n=3 5.517 = 0.974 ⊕ 5.428, n=5 2.183 = 1.342 ⊕ 1.714, n=50
> 1.486 = **1.315 ⊕ 0.685** m rms.  So the **per-satellite error DOES converge**
> with n (5.43 → 0.69); what does not is a pivot-relative common level that wanders
> ~1.3 m rms at every n (ties to the CB-Q "wire drifts in unpinned directions"
> finding).  Median row vf at n=50 is **15.5**, not the mean 455 — the mean is a
> three-satellite tail (G31/G28/G03 drifting −0.2 → −2.4 m *together*).  Effective
> sample size for the common direction is ≈ 1, so **"455 vs 392 = a plateau" is
> realization noise and must not be read as a shape.**  vf is still GROWING at 2 h
> at every n.  The honest statement is the decomposition, not the scalar.
>
> ⚠️ **And the ZWD "vf 1.63 = calibrated" row needs its asterisk**: truth is a
> 0.12 m near-constant (±0.055 m fleet spread) while `std_m` ≈ 0.79 m, so the
> channel is honestly UNINFORMATIVE — about 30× worse than just predicting the
> constant.  The 87.5 → 6.5 → 1.63 trend is numerator-driven (the error shrinking),
> not a σ that became trustworthy.

**The two channels behave completely differently, and that is the finding.**

- **ZWD converges to CALIBRATED**: vf 87.5 → 6.5 → **1.63**.  Its n = 3
  mis-calibration was a determinability artifact and it disappears with scale.
- **The satellite clock does NOT**: vf 768 → 392 → **455**, a plateau, not a
  descent.  Accuracy improves (5.52 → 1.49 m rms) but the formal σ shrinks just as
  fast, so it stays **~21× too optimistic in σ** at every scale tested.

This is directly relevant to the field's cb vf ≈ 10 question, without answering
it: a channel whose σ is structurally optimistic *independent of network size* is
an estimator property, not a small-network artifact.  The clock channel is such a
channel.  Whether the code-bias channel is one too still requires the W6 arm.

### 9.7.2 The QC tiers — 0/7 live at n = 3, 4/7 at n = 50

Armed arm, quiet data (no injected faults), 232/240 epochs emitted, **0 withheld
by the rank gate**, 8 by the A4 σ̂₀ ceiling.  78.5 ms/epoch, 50 stations,
PARALLEL/20 threads.

| tier | opp | flag | observed rate | implied α | verdict |
|---|---|---|---|---|---|
| IF-phase slip (T1) | **0** | 0 | — | 1.24e-15 | INERT — needs `cycle_slip.if_phase_td` |
| within-ep trigger (T2) | 226 | 7 | 3.10e-02 | 2.70e-03 | **live, 11× its implied α** |
| post-fit RAIM cell (T3) | 103 347 | 747 | 7.23e-03 | 1.97e-09 | **live, 3.7 MILLION× its implied α** |
| RAIM consensus (T3) | 724 | 0 | 0 | 1.97e-09 | live, silent (correct on a quiet arm) |
| attribution SAT (T4) | 7 135 | 0 | 0 | 1.97e-09 | live, silent (correct) |
| clock-model NIS (T5) | **0** | 0 | — | 1.97e-09 | INERT — needs `network.clock_model` |
| clock-monitor (T6) | **0** | 0 | — | — | INERT — and GPS-only degenerates its magnitude test |

Events on the same quiet arm: `T0_mw_detect / mw_wl_slip` **44**,
`T2_within_epoch / we_refloat_arc` 13 + `we_refloat_sat` 9, `T4_attrib /
attrib_arc` 13, `SIGMA0 / withhold` 8, plus 326 topology `row_kinds`.

**Findings:**

1. **Station count is what makes the QC stack live.**  0 of 7 tiers had a single
   opportunity at n = 3; 4 of 7 do at n = 50.  T2/T3/T4 need cross-station
   populations that a minimum network cannot supply.
2. **The post-fit RAIM cell tier fires 3.7 × 10⁶ times its own implied Gaussian
   α on clean synthetic data.**  This is the SAME finding as the clock vf, measured
   a second way: vf ≈ 455 means σ is ~21× too small, and a residual 21× larger
   than its σ crosses a k = 6 threshold constantly.  The two instruments agree,
   which is what makes it a result rather than an artifact of either.
3. **44 false MW slip detections** on an arm with `rate_base = 0` — no slip was
   injected anywhere.  A false-alarm rate of ~1.5e-4 per station-satellite-epoch.
4. **Three tiers remain inert even at 50 stations**, and each for a *named*
   reason rather than for want of data: T1 needs `cycle_slip.if_phase_td`, T5/T6
   need `network.clock_model`.  Arming those is the next arm, not a re-run.
5. **`estimate_iono_sh` cannot simply be added.**  It co-estimates the SH-VTEC
   field JOINTLY WITH THE SATELLITE CODE BIASES — the whole coupled W6 slice — and
   the realized datum then no longer matches `declared_dimension`: the rank gate
   withheld **all 240 epochs** and the producer emitted nothing.  Therefore
   **`sh_health_gate` is not gradeable by this fixture** until the W6 slice is
   armed as a whole.

⚠️ **The armed arm is NOT posture-comparable to the matched one**, for two
independent reasons.

*Posture.*  Its ZWD vf is **3466** against the matched arm's **1.63** — while its
ZWD rms is *better* (0.659 vs 0.896 m).  The armed set makes the ZWD estimate more
accurate and its σ roughly three orders of magnitude too small.  `phase_leveling`
is the prime suspect but the arms differ by many knobs; **a bisect is owed before
that is stated as a fact.**

*The URA σ-scale is not 1 in the armed arm.*  The published URA is
`s0 · sqrt(scov)` with `s0 = max(1.0, σ̂₀_EWMA)` (`NetworkSolPrep.cpp:430,443`).
That EWMA is structurally unavailable unless `emit_ura` + `raim` +
`phase_leveling` are ALL armed (the RTB-6 sentinel), so:

| arm | RTB-6 sentinel | σ̂₀ EWMA | **s0** | published URA |
|---|---|---|---|---|
| n = 3 / 5 / 50 **matched** | present | unavailable | **1.000** | the pure marginal formal σ |
| n = 50 **armed** | absent | **1.674** | **1.674** | formal σ inflated 1.674× |

⇒ **the whole matched trend (768 → 392 → 455) is on the pure formal σ and is
directly comparable.**  The armed arm's clock vf of 7866 is *deflated* by
s0² = 2.80 relative to the same measure — on the pure formal σ it would be
≈ 2.2 × 10⁴.  The reported figure is therefore CONSERVATIVE, and the direction of
every conclusion is unaffected, but the two must never be pooled.  Any future arm
that flips `phase_leveling` moves this scale silently.

---

### 9.7.3 QC DETECTION — quiet vs fault, one knob apart

`config/sim/simnet50_qcfault.json`, identical roster and identical producer
posture to the armed quiet arm; the only difference is the injected faults.

Faults, each verified present in the emitted RINEX before grading:

| fault | where | magnitude IN THE DATA |
|---|---|---|
| cycle slip, unflagged | S01 / G26 / L1C, ep 60, 19° elev | **+12.26 widelane cycles** (MW computed from the RINEX) |
| cycle slip, unflagged | S02 / G06 / L2W, ep 90, 44° elev | **+6.70 widelane cycles** |
| cycle slip, **LLI-flagged** | S03 / G19 / L1C, ep 160, 22° elev | +45 cyc, LLI = 1 |
| satellite clock step | G14, ep 120, all stations | +10 m |

Per-tier response (flag counts, quiet → fault):

| tier | quiet | fault | Δ |
|---|---|---|---|
| IF-phase slip (T1) | 0 opp | 0 opp | inert |
| within-ep trigger (T2) | 7 | 7 | 0 |
| post-fit RAIM cell (T3) | 747 | 603 | −144 |
| **RAIM consensus (T3)** | 0 | **29** | **+29 — responded** |
| attribution SAT (T4) | 0 | 0 | 0 |
| clock-model NIS (T5) / clock-monitor (T6) | 0 opp | 0 opp | inert |

Per-fault, from `network_events.csv` within ±10 epochs of each injection:

| fault | detected? | by what |
|---|---|---|
| G14 clock step +10 m | **YES, and MARKED** | `CLK_STEP mark` + `T4_attrib attrib_arc` |
| S03 slip, LLI-flagged | **YES** | `T0_mw_detect lli_slip` |
| S02 slip, −7 cyc unflagged | partial | `T4_attrib attrib_arc` only — no slip event |
| S01 slip, +12 cyc unflagged | **MISSED** | nothing |

**Findings:**

1. **The clock-step marking gap from §9.6 is CLOSED.**  With `emit_clock_disc` +
   `clock_disc_jump_test` armed, the 10 m step produces a `CLK_STEP mark`.  §9.6's
   zero marks were an unarmed marker, exactly as that section said — confirmed
   rather than assumed.
2. **RAIM consensus is the tier that works.**  0 → 29 flags, and it is the only
   tier whose flag count responds to the faults at all.  It needs the cross-station
   population that n = 3/5 cannot supply, which is the case for the 50-station
   variant in one line.
3. ⚠️ **`mw_detect` missed both unflagged slips — and they were not subtle.**
   The MW widelane jumps are **+12.26** and **+6.70 cycles**, computed directly
   from the emitted RINEX; MW noise is sub-cycle, so these are the easiest targets
   a widelane detector has.  The detector was demonstrably LIVE (44 `mw_wl_slip`
   events elsewhere on the same data).  The one slip it did catch was caught by the
   receiver's own **LLI flag** (`lli_slip`), not by the MW statistic.  So on this
   arm: **44 false alarms, 0 of 2 true detections, and the only true catch came
   from a flag the receiver supplied.**
   *This is one arm and one configuration — it is a finding to chase, not a verdict
   on the detector.*  The obvious next bisects: the elevation-scaled threshold at
   19°, and whether the network-path MW detector needs an arc maturity the fault
   epochs did not reach.
4. **T4 attribution never flags.**  7135 opportunities, 0 flags in both arms,
   including for a 10 m satellite clock step seen by all 50 stations — the exact
   fault class it exists to attribute.  It emits `attrib_arc` events but no flags.
5. **The post-fit RAIM cell tier's flag count goes DOWN with faults added**
   (747 → 603).  With a false-alarm population that large, adding a real fault just
   reshuffles which cells exceed.  That is what a tier dominated by false alarms
   looks like, and it is consistent with finding 2 of §9.7.2.

### 9.7.4 CHASING THE mw_detect MISS — one defect PROVEN, one still open

The §9.7.3 miss was chased to ground.  Three hypotheses, each a one-knob A/B on
the same fixture data.

| # | hypothesis | test | result |
|---|---|---|---|
| 1 | the rounding-cleanliness gate is too tight (`turbo_edit_frac_max` = 0.15, default) | 0.15 → **0.49** | **FALSIFIED** — 44 → 66 events, but still only at epochs **2 and 4**; nothing at 60/90/160 |
| 2 | no BIA ⇒ no BSSD integer WL lock ⇒ `slip_wl_known` never true (`has_bia ? "BSSD-pass2" : "direct-no-BIA"`, MwSlipDetector.cpp:2696) | `enable_code_phase_bias` false → **true** | **FALSIFIED** — bit-for-bit the same 44 events at epoch 2, same misses |
| 3 | **station index 0 is structurally excluded from slip detection** | roster reordered | **PROVEN** — below |

#### The proven defect: the datum station gets NO slip detection at all

```cpp
const int i_lo = phase_datum_prioritized ? 0 : 1;   // ApplicationNetworkAssembly.cpp:1281
```

The knob is **`network.phase_datum_prioritized`**, default **false**
(`GnssConfigLoadNetwork.cpp:4240`) — a knob in its own right, NOT
`phase_datum_eq34` (an earlier draft of this section misattributed it; the
`i_lo` proof is unaffected, the named remedy was wrong).  With it false,
`i_lo = 1`, and every slip-detection loop in `NetworkSlipQc.cpp` (lines 69, 169,
385 — MW **and** LLI) runs `for (int i = i_lo; ...)`, so **`stations[0]` is
skipped entirely.**

*Controlled proof.*  Identical fixture data; the ONLY change is roster order,
moving `S03000SIM` (whose LLI-flagged slip was detected at index 2) to index 0:

| roster position of S03000SIM | its G19 LLI slip | total `lli_slip` events in the run |
|---|---|---|
| index 2 | **detected** (`T0_mw_detect / lli_slip`) | 1 |
| **index 0** | **MISSED** | **0** |

The in-code rationale is sound as far as it goes — "Pivot (stations[0]) is never
touched (its ambiguities are gauge-pinned)", and refloating a pinned ambiguity is
meaningless.  **But the consequence is not benign.**  The datum station is
precisely the receiver whose ambiguity rides inside every emitted phase bias
(contract §B.2: "the network datum receiver's ambiguity rides inside it, which is
what makes the user's between-satellite DD ambiguities integer").  An undetected
slip there does not stay local — it re-references the P record for that satellite
across the whole network, and **nothing in the run records that it happened**:
not the flag-rate summary, not the event log, not a counter.

This is standing invariant #1 in its exact form — a stiff time-linked quantity
(the gauge-pinned datum ambiguity) with no wired slip response — and it is the
one station where the invariant matters most.

#### Still open: MW produces no mid-arc candidates at ANY station

`i_lo` explains S01 (index 0) completely.  It does **not** explain S02
(index 1, −7 cyc unflagged, 44° elevation), which sat inside the loop and was
still missed by MW; only `T4_attrib / attrib_arc` fired.  Across both fault arms
every one of the 44 `mw_wl_slip` events is at **epoch 2**, and the frac_max arm
adds epoch 4 — i.e. the MW path emits candidates only during ignition and then
nothing for 238 epochs.  Hypotheses 1 and 2 are eliminated; the remaining
candidates are the arc-maturity / Welford noise-floor state and the BSSD MW
pivot's own resolution (`slip_wl_known` is documented false "if no pivot could be
made — MW pivot slipped with an unresolved integer").

**Recommended next step, and why it is not this card's:** arm
`network.trace_slip_raw` (`network_slip_raw.csv` publishes the per-(station,sat)
raw MW every epoch, *independently of whether any detector is enabled*) and read
what the producer's own MW does at S02/G06/epoch 90.  That distinguishes "the
detector never saw the jump" from "it saw it and rejected it" in one arm.

### 9.7.5 RESOLVED — the detector SAW the jump and formed no candidate

`network.trace_slip_raw` armed (`network_slip_raw.csv` publishes per-(station,sat)
raw MW / GF / phase-minus-code / LLI every epoch, computed from `raw_observed`
**independently of whether any detector is enabled**).  At the injected epochs,
against each arc's own robust epoch-to-epoch scatter (1.4826·MAD):

| station / sat | MW jump | cycles | **vs arc scatter** | LLI | detected |
|---|---|---|---|---|---|
| **S02 / G06** (idx 1) | +5.760 m | **+6.68** | **13.3×** | 0/0 | **MISSED** |
| S03 / G19 (idx 2) | +38.821 m | +45.04 | 51.9× | **1**/0 | caught — by **LLI only** |
| S01 / G26 (idx 0) | +10.421 m | +12.09 | 7.7× | 0/0 | MISSED (`i_lo` blind spot) |

**This closes the open question of §9.7.4.**  The jump is present in the
producer's own trace, at the exact epoch, at 13× the arc's own scatter, with a
clean LLI — and the detector emitted no candidate.  The failure is in candidate
FORMATION, not in the observable.  Combined with the two falsified hypotheses
(frac gate, BIA), the remaining surface is the BSSD/integer-lock path itself.

### 9.7.6 The MW NULL SPACE — measured, and RAIM does not currently cover it

The MW widelane is N₁ − N₂, so a slip with **ΔN₁ = ΔN₂ changes it by exactly
zero** — structurally invisible to any MW-only detector, at any threshold.  A
paired arm (`config/sim/simnet50_mwblind.json`) puts a (+2,+2) and a (+2,0) slip
on the **same satellite, same epoch, same magnitude**, so the only difference is
whether the pair lies in MW's null space.  Verified in the emitted RINEX:

| slip | ΔWL measured | ΔWL predicted | ΔGF measured | ΔGF predicted |
|---|---|---|---|---|
| (+2,+2) | **−0.019 cyc** | **0 exactly** | −0.142 m | n(λ₁−λ₂) = −0.108 m |
| (+2, 0) | +2.199 cyc | +2 | +0.397 m | nλ₁ = +0.381 m |

The pair IS observable — ΔGF = −10.8 cm and the iono-free phase moves
n·c/(f₁+f₂) = **+21.4 cm** — just not by MW.  That is the argument for a
geometry-based backstop rather than a better MW gate.

**But the backstop does not currently fire.**  QC response to the pair, against
the quiet baseline:

| tier | quiet flag | mw-blind arm flag | Δ |
|---|---|---|---|
| IF-phase slip (T1) | 0 (0 opp) | 0 (0 opp) | 0 |
| within-ep trigger (T2) | 7 | 7 | 0 |
| post-fit RAIM cell (T3) | 747 | 743 | −4 (noise) |
| **RAIM consensus (T3)** | 0 | **0** | **0** |
| attribution SAT (T4) | 0 | 0 | 0 |
| clock-model NIS / clock-monitor | 0 (0 opp) | 0 (0 opp) | 0 |

**A 2-cycle slip pair — 38 cm on L1, 21.4 cm in the iono-free phase — passes
through all seven tiers untouched.**  Neither the MW-blind (+2,+2) nor the
MW-visible (+2,0) control produced a slip event; the visible one raised only a
`T4_attrib / attrib_arc`.  RAIM consensus, which DID respond to the 10 m clock
step (§9.7.3, 0 → 29), does not respond at this magnitude.

So "MW has a null space, therefore we need RAIM" is correct as a **requirement**
and is not yet a description of the built system: the geometry-based tier that
should cover MW's blind subspace does not cover it at 2 cycles.

### 9.7.7 The remedy is real but NOT free — do not make it standard yet

`network.phase_datum_prioritized: true` sets `i_lo = 0`.  One-knob arm off the
fault fixture:

| | default (`i_lo`=1) | **armed** (`i_lo`=0) |
|---|---|---|
| station-0 T0 events | **0** | **5** |
| run `mw_wl_slip` total | 44 | 49 |
| S01/G26 slip | MISSED | **still MISSED** |
| **epochs emitted** | **232 / 240** | **143 / 240** |
| withheld by A4 σ̂₀ ceiling | 8 | **97** |

Two conclusions, and they point opposite ways:

1. **It confirms the `i_lo` mechanism a second, independent way** — station 0
   goes from 0 to 5 T0 events purely by flipping this knob.
2. **It costs 97 of 240 epochs** (40 % of the product) to the A4 σ̂₀ ceiling,
   against 8 at default.  And it does NOT recover the S01 slip anyway, because
   the §9.7.5 candidate-formation failure applies at station 0 too — `i_lo` was
   necessary, not sufficient.

⇒ **Arming it as a standing default is NOT justified by this evidence.**  It
belongs in the SCOPED ledger as a validated-but-not-promoted knob with its cost
recorded, per the house rule that loader defaults stay off and each new config
arms its set explicitly.  The blind spot is real and must be documented wherever
the datum station's slip exposure matters; the fix for it is not this knob alone.

### 9.7.8 ⚠️ CORRECTION — RAIM was NOT blind.  The consensus rule vetoed it.

**§9.7.3 and §9.7.6 graded against the wrong instrument.**  `network_events.csv`
does not record RAIM refloats, so "MISSED" there meant "no event row", not "not
detected".  `network_raim.csv` (`network.trace_slip_events`) carries the actual
decision chain, and it says the opposite:

| slip | `s_max_norm` | `raim_k` | `spiked` | `n_sta_spiked` | `min_sta` | `refloated` |
|---|---|---|---|---|---|---|
| S02/G06, −7 cyc | **58.2** | 6 | **1** | **3** | 3 | **1 — acted** |
| S03/G11, +2 cyc | **9.4** | 6 | **1** | **1** | 3 | **0 — vetoed** |

RAIM **detected both, comfortably.**  The failure is in the ACTION rule:

```cpp
if (kv.second >= raim_min_sta) flagged.push_back(kv.first);   // NetworkRaim.cpp:266
```

The count is keyed by **satellite**, counting how many *stations* spiked on it.
The tier is built to attribute **satellite** faults.  **A cycle slip is a receiver
event, so exactly one station can ever spike** — `n_sta_spiked = 1 < min_sta = 3`,
at any magnitude.  On the MW-blind arm the run totals are **743 spiked, 0
refloated**: a 100 % detection rate and a 0 % action rate.

The −7 cyc slip got through only because it reached `n_sta_spiked` = 3 = exactly
the minimum — the slip perturbed the SHARED satellite parameters enough to push
two other stations' G06 residuals over k.  That is contamination masquerading as
consensus, not the tier working as intended.

**`raim_min_stations` is not a knob to lower** — it exists to stop the tier
attributing every station's noise to a satellite.  The fix is a separate
station-local attribution path (same statistic, different consensus logic).
Note `raim_attribution` (T4) reports **7135 opportunities and 0 flags in every
arm**, which is suspicious in the same direction and is the next thing to check.

### 9.7.9 HOW MUCH DO SLIPS CONTAMINATE?  Measured — and they are ORTHOGONAL to the level problem

One knob off the quiet 50-station arm: `cycle_slips.rate_base` 0 → 0.0002 (the
sim default), identical fixture, identical per-station seeds.  **465 slips across
all 51 stations, |Δ| 13 … 52 (median) … 123 cycles.**

At realistic magnitudes the QC stack DOES act — RAIM 2707 spiked / **2751
refloated** (vs 0 at 2 cycles), consensus flags 0 → 191, `mw_wl_slip` 44 → 121,
`lli_slip` 0 → 132.  Large slips hit many stations at once, so the consensus rule
is satisfied.  **The 2-cycle case is below the effective ACTION threshold, not
below the detection threshold.**

Truth grade, settled half:

| channel | 0 slips | **465 slips** | ratio |
|---|---|---|---|
| C rms | 1.102 m | **4.630 m** | **4.2×** |
| C median \|·\| | 0.804 m | 0.996 m | 1.24× |
| C max \|·\| | 3.569 m | **19.452 m** | 5.4× |
| ZWD rms | 0.659 m | 2.158 m | 3.3× |
| consumer 3D rms | 1.417 m | 1.719 m | 1.2× |

Median barely moves while rms goes 4.2× and max 5.4× ⇒ slips act through a heavy
**TAIL**, not a uniform shift: most epochs fine, some badly poisoned.  Also
`CLK_STEP mark` **0 → 17** — station-local slips being marked on the wire as
SATELLITE CLOCK discontinuities — and `T2_within_epoch / emitted_contaminated`
**0 → 11**.

#### The decomposition — and the negative result that matters

Splitting the settled C residual into fleet-common (vs pivot) ⊕ per-satellite:

| arm | C rms | = common | ⊕ per-sat |
|---|---|---|---|
| 0 slips | 1.102 | **0.920** | 0.608 |
| 465 slips | 4.630 | **0.466** | **4.606** |

**Slips inflate the PER-SATELLITE error 7.6× (0.61 → 4.61 m) and leave the COMMON
direction untouched — it does not grow, it shrinks slightly.**

So the two problems are ORTHOGONAL:

- **Slip contamination is real and first-order** on the per-satellite channel, and
  it is a genuine, fixable defect (the §9.7.8 action rule).
- **The pivot-relative common-mode level wander (~0.9–1.3 m rms at every station
  count) is NOT slip contamination.**  It is identical with 0 and with 465 slips.
  That is the channel the field campaign has been chasing (the CB-Q "wire drifts
  in unpinned directions" finding), and this arm rules slips out as its cause.

Fixing the slip response would clean up the per-satellite tail and the 17 spurious
clock marks.  It would not move the level.

#### What this does and does not license

- The `i_lo` finding is **proven** by a controlled single-variable A/B and is
  actionable: either extend detection to station 0 in the legacy datum posture, or
  document that the datum station is a slip-detection blind spot and make the
  eq34 posture mandatory where slips matter.
- The MW mid-arc silence is **measured but unexplained**.  It is one fixture, one
  data rate (30 s), one elevation range.  It is a lead, not a verdict on the
  detector.
- Caveat carried from §9.7.3: the sim's slips are injected on ONE band, which is
  the classic widelane target; a two-band slip is a different test.

---

## §9.6 G-B — satellite-clock step, FIRST CLASS ONLY (measured 2026-08-11)

> ⚠️ **RETRACTED 2026-08-11 by adversarial review — do NOT quote the table below.**
> The "recovery floor between 0.5 and 2.0 m" is a **WINDOW ARTIFACT**.  Pre-step,
> the G-B arms are bit-identical to the null arm (max |ΔC| = 0.000000 over 118
> epochs), and the **null arm itself drifts −0.464 m between the pre- and
> post-windows** — so differencing pre-vs-post inside one arm charges that drift
> to the injected step.  Graded DIFFERENTIALLY against the null arm's own
> post-window (the seeds make this exact), recovery is **92.8 / 92.8 / 92.4 %** at
> 0.5 / 2 / 10 m: a uniform ~7.5 % linear absorption, **no floor, and 0.5 m is NOT
> "undetected"**.  G-B must be graded differentially against the null arm.
> Related debt: §3.1's promised "re-worded 1 mm gate" was never built — the SPP is
> fed unperturbed rows instead, so nothing asserts the injection reached the
> observables.  (§9.7.5 now does that job for the slip classes, via
> `trace_slip_raw`.)

Bench demonstration, not the full matrix.  Arms `config/sim/simnet5_gb{05,2,10}.json`
— 5 solve stations, one step on **G14** (40–44° elevation, well observed, so the
result is not confounded with the low-elevation noise that dominates G02/G13) at
epoch 120 of 240.  Recovered magnitude = median C(G14) over epochs 125–150 minus
median over 95–118 (C is already the estimable contrast; the pivot is pinned to 0).

| injected | pre-step | post-step | **recovered** | error | latency |
|---|---|---|---|---|---|
| 0.5 m | −0.347 | −0.361 | **−0.014 m** | −0.514 | **not detected** |
| 2.0 m | −0.347 | +1.030 | **+1.377 m** | −0.623 | 0 epochs |
| 10.0 m | −0.347 | +8.415 | **+8.762 m** | −1.238 | 0 epochs |

**Empirical recovery floor is between 0.5 m and 2.0 m** at this configuration — a
0.5 m step is entirely absorbed (recovered 3 % of it), while 2 m and 10 m are
tracked within one epoch.  Recovery is **biased LOW by 12–31 %**, the balance
going into the other estimable channels.

Latency 0 is expected rather than impressive: `sat_clock_q = 0.01` gives
√(q·Δt) = 0.55 m of process noise per 30 s epoch, so the clock state is nearly
free and simply re-estimates.  That is the same posture the Q3 card exists to
change, and it means **this arm measures the absorption floor, not a guard.**

⚠️ **Marking is NOT TESTED by these arms.**  Zero RAIM flags and zero R records
were observed at every magnitude including 10 m — but `emit_clock_disc` and
`clock_disc_jump_test` were **not armed**, so the marker channel was never
populated.  Per standing invariant #4, that is an inert instrument, not a failing
guard, and it must not be read as one.  Arming them is the first item of the real
G-B matrix.

**Still owed for G-B:** the marked re-run of this class; and the CB step/ramp,
iono ramp, and station outage/return classes (cycle slips already exist via
`ScriptedSlip` but are not yet wired into an event matrix).  Empirical MDBs per
guard, and the event ↔ R ↔ counter ↔ consumer tie-out, follow from those.

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
