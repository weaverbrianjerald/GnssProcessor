# SPEC — the SYNTHETIC MINIMUM NETWORK (the sim half of G4)
### (a truth-graded 3/5-station fixture: the first ABSOLUTE grade the network producer has ever had, and a cheap exact bench for event response)

2026-08-11.  Companion to `docs/SPEC_sbasis_realization_contract.md` (which
defines *what* is gradeable), `docs/SPEC_clock_temporal_anchor.md` (Q3 — whose
guard population this fixture manufactures on demand) and
`docs/SPEC_cb_admission_card.md` / the CB-carry card in flight (whose guard
G-C rebases on).

---

## §0 READ THIS FIRST — document status, and the error class it exposed

This SPEC was written incrementally while the arms ran, so **later sections
retract earlier ones**.  Read the status table before quoting anything.

| section | status |
|---|---|
| §1–§8 (design, method, traps) | **live** |
| §9.5 G-A calibration table | live, but see the §9.7.1 amendment on how to read a vf |
| **§9.6 G-B recovery floor** | **RETRACTED** — window artifact; banner in place |
| §9.7.1 clock-vf "plateau" | **AMENDED** — the scalar is not a readable shape; use the decomposition |
| §9.7.2 QC tier table | live (the "44 false alarms" framing is corrected in §9.7.4) |
| **§9.7.3 / §9.7.6 "MISSED"** | **CORRECTED by §9.7.8** — graded against the wrong instrument |
| §9.7.4 / §9.7.5 MW chase | live |
| §9.7.7 `phase_datum_prioritized` cost | live |
| **§9.7.8 / §9.7.9** | live, but §9.7.8's "100 % detection / 0 % action" framing is **CORRECTED by §9.7.10** (the `refloated` column records only the CONSENSUS action; the arc-path actions live in `network_events.csv`) |
| **§9.7.10 fixes + corrections** | live — the current conclusions; blind-spot fix and T4a counter LANDED and sim-validated |
| **§9.8 datum-wander arm** | live — the common level FOLLOWS the datum station; `clock_datum_sum_to_zero` first validation PASS; ⚠ puts an asterisk on §9.7.1's vf plateau (pivot-election artifact mixed in) |
| **§9.8.1 mask arms / §9.9 field probe** | live — 25° mask confirms the pivot mechanism but is REJECTED as a remedy (~5 m consumer bias); the FIELD wander (1.735 m) is NOT receiver-code-driven (MBAR rank 48/61) — the hunt returns to the LEVEL/inter-channel directions |
| **§9.9.1 / §9.10 product + iono probes** | live — a-priori frame EXONERATED (detrended slope −0.014; product resid common 9 cm); wander is ESTIMATOR-INTERNAL.  Iono input field coherent (0.98 @ 300–700 km both-high) but BROKEN at low–high ~1000 km (0.38, rel-rms 1.13) — candidate contamination channel, causal arm owed |

### The error class, stated generally

Three of this card's headline readings failed the same way, and it is **not** a
GNSS error — it is a measurement-hygiene error that any part of this codebase's
analysis history could share:

> **A conclusion was drawn from an instrument that does not record the thing
> being looked for, and its silence was read as a negative result.**

The three instances:

1. **`network_events.csv` does not record RAIM refloats.**  "No event row" was
   read as "not detected".  RAIM had in fact detected at 58σ and acted.  The
   decision chain lives in `network_raim.csv`, behind `trace_slip_events`.
2. **A tier reporting `0 flag` with `0 opp` is INERT, not clean.**  Zero
   opportunities and zero findings are indistinguishable in every log line.
3. **A guard whose MARKER is unarmed emits nothing.**  §9.6 read zero
   `CLK_STEP` marks as "the event was not marked", when `emit_clock_disc` was
   simply off.
4. **A column that records ONE action path was read as THE action record**
   (found by the §9.7.10 follow-up).  §9.7.8's "743 spiked, 0 refloated = 0 %
   action rate" read `network_raim.csv`'s `refloated` column — which records
   only the CONSENSUS refloat — while the T4 arc path's 14 refloats (including
   the real injected slip, at its exact epoch) were in `network_events.csv`
   all along.  Same class from the other side: an instrument that does not
   record the acting sub-path makes the sub-path read as dead.

**The generalizable check:** before reading any absence as evidence, establish
that the instrument (a) was armed, (b) had opportunities, and (c) records the
specific outcome in question — not merely a neighbouring one.  Standing
invariant #4 states (b); (a) and (c) are the same mistake wearing different
clothes.

Anyone auditing prior campaign conclusions should treat "the counter stayed at
zero" and "the event log is empty" as **unverified** until the recording path is
confirmed, particularly where a tier ACTS without emitting (RAIM refloat is not
the only such path).

---

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

### 9.7.10 The §9.7.8 trail CLOSED — corrections, fixes LANDED, and their measurements (2026-08-11)

Follow-up session, working from the retained `runs/simnet50_mwblind2/run/logs`
decision traces (the §9.7.8 note "T4 reports 7135 opportunities and 0 flags …
is the next thing to check" is what this section checks).  Three corrections to
the record, then two landed fixes with their validation.

#### Corrections to §9.7.3 / §9.7.6 / §9.7.8

1. **T4's 7135 opp / 0 flag is an INSTRUMENT gap, not a dead tier.**  The
   flag-rate counter (`fr_attrib`, NetworkRaim.cpp) counts only the whole-SAT
   criterion, which is deliberately conjoined with `σ̂0 > raim_sigma0_trigger`
   (the 305-24 h lock-in guard: at a robust-clean epoch a high column effect is
   a median-polish artifact, and wholesale refloats destabilize).  A
   single-satellite fault cannot move a network-wide robust MEDIAN (one sat is
   ~1/31 of the row pool), so the SAT flag is **structurally unreachable at a
   clean epoch — for exactly the fault class the tier is named after.**  It CAN
   fire in globally-contaminated postures: the `phase_datum_prioritized` arm
   (16 flags amid 37 within-epoch triggers) proves the counter wiring itself.
   Meanwhile the tier's ARC sub-path (isolated-arc interaction, gated at any
   σ̂0) **acted 14 times on the mwblind2 arm — including the real (+2,0) slip
   at S03/G11 at its exact injection epoch** (interaction 12.88 = 7.2×σ̂0,
   `attrib_arc` event, refloat applied), and none of those actions appeared in
   any flag-rate line.
2. **§9.7.8's "100 % detection / 0 % action" over-claimed both halves.**  The
   `refloated` column of `network_raim.csv` records the CONSENSUS action only
   (§0 error class, instance 4); and most of the 743 "detections" are the
   σ-miscalibration false-alarm population of §9.7.2 — on a clean arm, NOT
   acting on them is correct.  Measured true-event record on this arm: 2
   injected slips → 1 with any post-fit signature → 1 acted (same epoch, arc
   path).  The consensus rule's receiver-event veto stands as designed; the
   station-local action path it calls for **already exists and works** — it is
   the T4 arc path.
3. **The MW-blind (+2,+2) pair produced NO post-fit signature at all** — no
   cell spike at any epoch (its GF signature is absorbed by the free per-LOS
   iono, the remainder redistributed below the cell threshold).  No action
   rule, existing or proposed, could have caught it post-fit.  §9.7.6's "the
   geometry-based backstop does not fire" was an UNARMED-TIER reading: T1
   (`network.cycle_slip.if_phase_td`) was off in every §9.7 arm (0
   opportunities in every flag-rate table).  Armed (`_simnet50_mwblind_ifp`,
   one knob): **both G11 slips caught at the exact injection epoch** —
   S02 ΔΔφ_IF 0.205 m > thr 0.135 (the MW-null pair; prediction 2·λ_IF(1,1) =
   0.214 m) and S03 0.989 m > thr 0.257 — with **2 flags in 109 137 samples =
   zero false alarms.**  The MW null space is covered by the tier that already
   existed; the standing 30/60 s doctrine (relaxed MW + LLI/IF-TD) simply was
   not applied to the sim posture.  (The loader's posture auditor already
   nags: "POSTURE BUNDLE 'zero-q phase memory + slip response': PARTIAL 1/2 —
   MISSING: network.cycle_slip.if_phase_td".)

#### Fix 1 — T4a flag-rate line (landed)

`fr_attrib_arc` counts the ARC sub-path's criterion (`interaction/σ̂0 >
raim_k`, pre persistence/maturity — mirroring how the SAT line mirrors
`bad_sats`), emitted as `T4a,attribution ARC (T4)` in the summary and CSV.  On
the mwblind2 rerun: **103 347 opp / 14 flag — matching the 14 `attrib_arc`
actions in the retained events one-for-one.**  The summary now separates "SAT
criterion structurally quiet" from "ARC sub-path live and acting".

#### Fix 2 — `network.cycle_slip.datum_station_detect` (landed, default OFF)

The §9.7.4 blind spot, closed with the mechanism-specific response instead of
the §9.7.7 knob.  Wiring: setup builds the pivot's MW detector under the
toggle; a serial PASS-A block rolls it (`ApplicationNetworkAssembly`); the MW
drain routes station-0 MW candidates and (Card-L-qualified) LLI assertions to
a two-edge network re-reference (invariant #7): **queue edge** at detection —
`datum_slip` event + the sat queued for a HARD skip-stash of its shared
PhaseBias carry at this epoch's stash (before the pin-bridge harvest, both
stash branches); **apply edge** at the first drain AFTER that skip-stash
consumed (per-sat guard, so a withheld epoch cannot decouple the edges) —
`datum_reref` event, pending-reseed of the sat's ambiguities at EVERY
non-pivot station (present or absent — an absent station's mark waits for its
own re-coupling edge), local-carry erase, Card-4 C3c feed, per-sat disc mark
via the Edge-1 mark-and-serialize queue (whose SolPrep drain condition gains
the third producer — its first arm queued marks into a run with `ura_retro`
and `mark_refsat` both off and the mark silently never landed, the exact leak
the drain comment predicts), and run-end counters (`datum_slip_detections` /
`datum_slip_rerefs`, invariant #4 line in the teardown).  Loader: requires
`mw_detect` (parsed after the invariant-#1 auto-arm so leveling counts);
inert under any no-pivot datum.  Unit suite: `DatumStationSlipDetectConfig`
(4 cases, incl. the auto-arm ordering).

**Validation, OFF (byte-identity).**  The mwblind2 arm rerun with the new
build, toggles off: `products.txt` and every decision trace
(`network_raim.csv`, `network_events.csv`, `network_raim_attrib.csv`,
`network_slip_raw.csv`, `network_epoch_status.csv`) **bit-identical to the
retained run**; `network_flag_rates.csv` differs by exactly the added T4a row.

**Validation, ON (`_simnet50_pivotswap_datum` vs `_simnet50_pivotswap_b`, one
knob, identical qcfault fixture data, settled half).**  The index-0 G19 LLI
slip — MISSED in §9.7.4 — is now caught end-to-end: `datum_slip` at the LLI
epoch, `datum_reref` one epoch later (28 ambiguities network-wide), `R,G19`
disc mark on the wire, counters 2/2.

| | OFF (blind spot) | **ON (datum detect)** | |
|---|---|---|---|
| C residual rms | 1.657 m | **0.485 m** | 3.4× |
| C residual max | **18.14 m** | **1.44 m** | 12.6× — the §9.7.9 heavy tail |
| C contrast rms (datum-free) | 2.245 m | 0.468 m | cross-check agrees |
| ZWD rms | 1.181 m | 0.685 m | 1.7× |
| `CLK_STEP` marks | **12** | **1** (the injected G14 step) | the unhandled pivot slip was being marked as ELEVEN spurious per-sat CLOCK discontinuities — poison for every consumer |
| `emitted_contaminated` | 27 | 0 | |
| A4 σ̂0 withholds | 14 | **14** | **zero availability cost** — vs 97/240 for `phase_datum_prioritized` (§9.7.7) |
| consumer 3D rms | 1.362 m | 1.227 m | AR-confounded, modest |

**Known costs / open edges (triaged):**
- One ignition false positive: the pivot's MW detector emitted a G12 candidate
  at epoch 2 (the §9.7.4 ignition-candidate class) → one spurious
  re-reference during ignition, marked honestly (`R,G12`).  Field-inert here
  (ignition is pre-convergence); a maturity gate on the datum path is a cheap
  follow-up once §9.7.5's mid-arc candidate-formation defect — which bounds
  MW's usefulness at the pivot exactly as everywhere else — is chased.  LLI is
  the demonstrated live catcher for the pivot.
- The pivot remains OUTSIDE post-fit RAIM cells and has NO IF-TD detector
  (setup gate) — its coverage is MW + LLI + this response.  A pivot slip in
  MW's null space with no LLI stays uncovered; extending IF-TD to the pivot
  is the named follow-up.
- Warm resume drops pending re-reference queues (same transient class as
  `refsat_mark_sats`; the slip then surfaces to post-fit RAIM at non-pivot
  stations instead).
- §9.7.9 orthogonality stands: this cleans the per-satellite tail and the
  wire hygiene.  **It does not move the pivot-relative common-mode LEVEL** —
  the datum-wander channel remains the separate (CB-Q) campaign.

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

## §9.8 The DATUM-WANDER arm — the common level FOLLOWS THE DATUM STATION, and both remedies measured (2026-08-11)

The §9.7.1/§9.7.9 channel: a pivot-relative fleet-common C level that does not
converge with n and is slip-independent.  On the QUIET fixture `truth_C ≡ 0`,
so the fleet-common of the emitted C records IS the wander, exactly.  Tool:
`scripts/simnet_common_mode.py` (median-over-sats series; settled, demeaned).

**Design.**  Same quiet 50-sta data, same armed posture, ONE key varied:
`network.clock_datum_station` — which the setup makes `stations[0]`, i.e. the
clock datum AND the phase pivot AND the sync reference move together (they are
not separable in the legacy family).  S01 is near-polar (lat −68.6°, the §2.4
"sees everything at low elevation" class); S26 is equatorial (+1.1°).  Then
`network.clock_datum_sum_to_zero` — the CLAUDE.md amendment-5 "needs its first
validation run" candidate — on each.  Pre-registered: P1 (wander owned by the
datum station) ⇒ series decorrelate under datum change; P2 (shared-state) ⇒
corr ≈ +1.  Arms `_simnet50_ga_cd{A,B,C,D}.json`.

| arm | pivot / clock datum | C resid rms | C contrast rms (datum-free) | common wander std | consumer 3D rms |
|---|---|---|---|---|---|
| cdA | S01 polar, pivot-sat emit | 1.102 m | 0.966 m | **0.193 m** | 1.417 m |
| cdB | S26 equatorial, pivot-sat emit | 0.384 m | **0.443 m** | 0.024 m | 0.315 m |
| cdC | S01 + sum-to-zero | 0.636 m | 0.991 m | 0.036 m | 1.589 m |
| cdD | **S26 + sum-to-zero** | **0.286 m** | **0.434 m** | **0.014 m** | **0.208 m** |

### Findings

1. **P1 in substance: the wander follows the datum station, and its amplitude
   is set by that station's geometry** — 0.193 m (polar) vs 0.024 m
   (equatorial), 8×, on identical data; first-difference corr(A,B) = +0.15 ≈ 0
   (increments unrelated).  The level-corr (−0.885) is two smooth 1 h curves
   trending oppositely — per the §9.7.1 amendment, effective n ≈ few for slow
   series; do not read that shape.
2. **The §9.7.9 "0.920 m common" figure decomposes as ~0.82 m CONSTANT lump
   (d_IF/anchor-class offset, reproducible, not a stability problem) ⊕ ~0.19 m
   true WANDER.**  √(0.824² + 0.193²) = 0.846 ≈ the 0.920 measured there.
3. **The datum-free contrast ALSO moved (0.966 → 0.443, 2.2×)** — the pivot is
   not just a gauge: its phase pins gauge the PB chain through its own (noisy,
   low-elevation) data, its QC exposure differs, and the estimable per-sat
   content degrades with pivot quality.  **Pivot election is a PRODUCT-QUALITY
   decision, not a labeling decision.**
4. **`clock_datum_sum_to_zero` first validation: PASS, doing exactly what it
   claims.**  Wire mean-over-sats ≡ 0.0000 exactly every epoch (constraint
   holds bit-for-bit); wander 0.193 → 0.036 at fixed (bad) pivot; availability
   unchanged (232/240 emitted, 8 A4 withholds, same as cdA); rank gate passes
   (`declared_dimension` 1 → 0, self-consistently — §5's caveat applies).  The
   loader AUTO-ARMS `clock_disc_on_set_change` (a clock-satellite set change
   re-references the Σ=0 gauge and marks the disc counters — invariant #7 was
   already wired).  My pre-registered "< 0.02 m" missed: with Σ=0 enforced the
   MEDIAN wander (0.036) measures distribution asymmetry, not the level; the
   MEAN is pinned identically.
5. **The consumer feels the PIVOT, not the wander**: consumer 3D rms ranks with
   the contrast column (0.21/0.32 m for good-pivot arms vs 1.42/1.59 m for
   polar-pivot arms — a 6.8× end-to-end swing from datum election alone), and
   sum-to-zero alone (cdC) does NOT help the consumer — a PPP consumer
   estimates its own clock, so the common level was never its problem.  The
   wander matters to ABSOLUTE-clock consumers (timing, inter-provider
   comparison) and to every internal vf/URA channel.
6. **The §9.7.1 vf plateau needs an asterisk**: clock vf 7866 (cdA) vs 741
   (cdB) — the σ-optimism largely followed the pivot artifact too.  The
   matched-trend arms (n = 3/5/50) all used the first-roster-station datum, so
   the "vf ~455 plateau at every n" conflates estimator σ-optimism with the
   datum-election artifact.  Owed: a re-read of that trend against cdB-class
   arms before quoting the plateau as an estimator property.

### What this does and does not say about the FIELD wander

The FT63 lineage's `clock_datum_station` is MBAR00UGA (equatorial — the field
already has good datum geometry by luck or design).  What transfers is the
MECHANISM: under the legacy emit, the datum receiver's own clock-estimation
error goes 1:1 onto the wire's common level.  On real data that error budget is
real multipath + environment + hardware, not the sim's clean noise — and the
field lineage estimates CB jointly (W6 posture), so the CB-Q "metres/pass in
unpinned inter-channel dirs" is this channel COUPLED into the bias states, not
this channel alone.  The sim numbers bound the geometry term only.

**Remedy stack, validated here:** (a) elect a good-geometry pivot (helps
EVERYTHING including the consumer, 2.2× on per-sat content); (b) arm
`clock_datum_sum_to_zero` for the common level (5×, exact mean pin; costs a
set-change re-reference class, auto-marked).  Both are datum toggles — INSIDE
the warm-resume fingerprint, fresh convergence only.  Caveats: one fixture,
1 h settled window, GPS-only INT posture (no CB states), float consumer.

### 9.8.1 The 25° mask arms — the mechanism CONFIRMED, the remedy REJECTED

Two more arms (`_simnet50_ga_cd{E,F}.json`): the SAME two pivots with
`processing.elevation_mask_deg` 10 → 25 (producer-side mask on the same data).

| arm | pivot | mask | contrast rms | wander std | consumer 3D rms |
|---|---|---|---|---|---|
| cdA | polar | 10° | 0.966 m | 0.193 m | 1.417 m |
| **cdE** | polar | **25°** | **0.403 m** | 0.067 m | **4.839 m** |
| cdB | equatorial | 10° | 0.443 m | 0.024 m | 0.315 m |
| **cdF** | equatorial | **25°** | 0.447 m | 0.078 m | **4.968 m** |

- **Mechanism confirmed**: masking the POLAR pivot's low-elevation data
  recovers the datum-free contrast to the equatorial level (0.966 → 0.403 ≈
  0.443), while the same mask on the equatorial pivot moves it not at all
  (0.443 → 0.447).  The pivot penalty is specifically THE PIVOT'S low-elevation
  data gauging the network — not low-elevation data in general.
- **Remedy rejected**: the global mask costs a ~+4.8–5.0 m SYSTEMATIC consumer
  position error in BOTH arms (p50 ≈ 5.1 m — a bias, not noise: the
  tropo/height separation lives in the low-elevation geometry the mask
  removed), and the wander worsens on the good pivot (0.024 → 0.078, less
  averaging).  A 25° mask amputates the observability to hide the artifact.
- The surgical version — scope the GAUGE, not the data: elect a good pivot
  (validated, free), or a future pivot-PIN elevation floor (pin only the
  pivot's high-elevation ambiguities; every observation stays in the solve).
  The sub-network/common-pivot intuition reduces to exactly this: the gauge
  should ride on high-quality observations; partitioning the SOLVE would add
  tie-seam datum churn (invariant #7's edge class) for no observability gain
  over per-satellite anchor election (`phase_datum_prioritized` / eq34), which
  already exists.
- Pivot cycle slips are NOT a factor in any cd arm: the quiet fixture injects
  zero slips (`rate_base` 0, no scripted slips) — the 2.2×/8× effects are
  slip-free by construction.  (Field-side pivot-slip exposure is §9.7.10's
  separate, now-closable channel.)

## §9.9 The FIELD probe — the wire wander is NOT receiver-code-driven (2026-08-11)

`scripts/field_wire_datum_probe.py` on the retained CB-Q 12 h control run
(`runs/cbq_12h_ctl`: 61 stations, datum MBAR00UGA, the FT63 lineage, W6+SH
posture).  Method: wire common level w(t) = per-epoch median of the emitted C
records (increments per-satellite → median; pairs across the run's single
`datum_changed` epoch dropped); per-station iono-null multipath proxy from
`network_slip_raw.csv` (`l1c1 + 2/(γ−1)·gf` — increments track −Δmp_code;
LLI rows excluded); corr(dw, dmp_i) per station over the settled 10 h.
TOW keys quantized to the second (the E rows carry float dust:
302370.00000190735 vs the trace's %.3f — exact-key joins silently die).

**Result: the §9.8 receiver-side driver is ABSENT on the field.**

- Field wire wander: **1.735 m** demeaned std over the longest settled
  segment (875 epochs) — the CB-Q "metres per pass" scale, quantified.
- corr(dw, dmp): **MBAR ranks 48 of 61** (r = +0.011); controls median
  |r| = 0.018, max 0.053.  No station's code observables track the wire —
  the whole table is at the noise floor.
- Tropo at MBAR is excludable by magnitude (1.7 m of clock common would need
  ~0.6–1 m of ZWD error), though the mp proxy is structurally blind to it
  (tropo cancels in L1−P1).

**Interpretation.**  The sim's geometry-driven datum-receiver term exists but
is SMALL on the field (MBAR is equatorial; the sim bound for a good datum is
~0.02–0.03 m — two orders below the 1.735 m observed).  The field wander is
dominated by the SHARED/product common directions — the CB-Q "unpinned
inter-channel drift" (clock ↔ CB ↔ iono exchanges, plus any a-priori product
common drift, which enters w(t) directly as −(apriori common) and which no
station observable can track).  This probe eliminates the receiver-local
hypothesis the §9.8 mechanism raised, and returns the hunt to the LEVEL
information channel (`SPEC_level_information_budget`, the CB-Q card).

**Caveat**: the increment probe covers the FAST channel (30 s).  A driver
acting only at hours-scale with negligible 30 s increments would evade it;
the level-domain candidates (a-priori clock common drift, CB/iono common
exchanges) need a level-domain instrument — the CB-Q structure-function
line of attack, not this one.

### 9.9.1 The a-priori CLK common-drift probe — the PRODUCT FRAME EXONERATED

`scripts/field_apriori_clk_probe.py`: A(t) = c·median_s dt_apriori_s(t) from
the cached COD 30 s CLK, against w(t) on the joint 874 settled datum-stable
epochs (7.3 h).  The C records are corrections about this product, so a
product fleet-common drift enters w(t) at slope −1 and no station observable
can track it — the exact §9.9 signature.

- A(t) is a nearly pure LINEAR timescale drift: −4.089 m/h, residual std
  **0.094 m** after detrending (range 29.8 m raw).
- The wire does NOT inherit it: w's own trend is −0.548 m/h (slope-−1
  inheritance would read +4.089), detrended corr(w,A) = −0.19 at slope
  **−0.014**, increment corr +0.04.  The detrended product common (9 cm)
  could at most explain ~7 % of the wire's 1.298 m detrended wander even at
  slope −1; measured slope says it explains ~none.
- Linear timescale offsets are frame-conventional (a datum-invariant consumer
  never sees them).

**With §9.9 this corners the field wander: slips (§9.7.9), every station's
receiver-code errors (§9.9), datum-station geometry (§9.8, bounded 2–3 cm at
an equatorial datum), tropo (magnitude), and the a-priori frame (here) are
all eliminated by measurement.  The 1.3 m detrended wander is
ESTIMATOR-INTERNAL — the clock ↔ CB ↔ iono common-direction exchanges, i.e.
the LEVEL information channel.**

## §9.10 Cross-station IONO-FIELD agreement — coherent where mapped, BROKEN at low–high (2026-08-11)

`scripts/field_iono_pair_agreement.py`, raw observables only (an INPUT
property, not a fit property): per station-pair-satellite, 300 s GF increments
mapped to vertical by thin-shell obliquity (h = 450 km), LLI-clean, settled
10 h; 65 pairs in [300, 2000] km, 1406 (pair, sat) series.

| baseline | elev class | series | med corr | med rel-rms |
|---|---|---|---|---|
| 300–700 km | both-high | 349 | **0.981** | **0.173** |
| 300–700 km | mid | 426 | 0.947 | 0.260 |
| 1200–2000 km | both-high | 284 | 0.957 | 0.397 |
| 1200–2000 km | mid | 344 | 0.918 | 0.451 |
| 1200–2000 km | **low–high** | 3 | **0.381** | **1.130** |

(rel-rms: 0 = identical fields, √2 ≈ 1.41 = uncorrelated.)

- **The co-estimated field's INPUT is genuinely coherent** at the scales the
  SH model claims: two stations' vertical iono rates agree to ~17 % at
  300–700 km and ~40 % at 1200–2000 km when both view high — this coherence
  is what lets the network separate iono from clock at all.
- **The "same field" assumption BREAKS in the low–high configuration** (one
  station low-elevation, one high, ~1000+ km apart — the classic thin-shell
  + horizontal-gradient failure regime): agreement collapses to
  near-uncorrelated.  Low-elevation GF content disagrees with the field the
  rest of the network sees MORE than the signal itself — whatever weight the
  estimator gives it is absorbed by the per-LOS/SH iono states and, through
  the coupled solve, by the clock/CB commons.
- **Not yet shown**: that this channel DRIVES w(t) — today's result
  characterizes the input; attribution needs a causal arm (e.g.
  `ionosphere_weighting.elevation_cutoff_deg` / iono-side elevation policy
  A/B graded on the wire common — NOT a global observation mask, which
  §9.8.1 already rejected).  Note the low–high series count is 3 (long
  joint low-elevation arcs are rare); widen the window or pool days before
  quoting the 0.381 as a precise number.

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

## §11 NEXT — the W6+SH synthetic arm (design brief for the next session)

> ⚠️ **SUPERSEDED IN PART, 2026-08-12 — the arm is BUILT and its first ladder is
> measured; see §12.**  Two of the five build notes below did not survive contact
> with the data and must not be re-used as written:
> - **Item 1's framing is wrong about WHICH wall comes first.**  The wall at 50
>   stations is not a code-bias/ionosphere separability floor.  It is ONE uncounted
>   near-null direction, and the estimability dump names it: the **L2W (secondary
>   band) satellite phase-bias COMMON MODE**, ev 9.78e-06 against a 6.9e-12 clock
>   gauge (§12.2).  The elevation-cutoff suspicion in item 1 is also settled —
>   arming the 25° cutoff completes the posture bundle 3/3 and the arm still
>   withholds every epoch, so the documented 50-station withhold is NOT a
>   cutoff artefact.
> - **Item 3 is MOOT, not a design decision.**  Under `estimate_iono_sh` the
>   external GIM is **never loaded** — the two iono constraints are mutually
>   exclusive by construction (`ApplicationNetworkSetup.cpp:386-440`), so there is
>   no real-GIM-vs-synthetic-truth mismatch to design around.  What IS load-bearing
>   from that block is the elevation CUTOFF, which gates which lines of sight get an
>   SH tie row.
>
> Items 2, 4 and 5 stand and are implemented as written.

The §9.9/§9.9.1 elimination campaign cornered the field wire wander (1.3 m
detrended) as ESTIMATOR-INTERNAL: the clock ↔ CB ↔ iono common exchanges.
Those states exist only in the coupled W6+SH posture — **which the synthetic
fixture has never run** (every §9 arm is INT posture; sim-internal wander
there is 0.024 m).  This arm runs the field's wander-bearing posture on
synthetic truth so the wander decomposes exactly.  Build notes, in order of
how they will bite:

1. **The observability floor is the first wall, and it is STATION SUPPORT.**
   The ledger's `estimate_sat_code_bias` interlock records: at 50 stations,
   "dual-frequency or single-channel sets ⇒ the null-space rank gate WITHHOLDS
   EVERY EPOCH"; `project_dual_freq_osb_rank_deficiency` proved it is NOT a
   frequency wall (dual+OSB solves at 305).  The simnet fixture is exactly
   dual-frequency (L1C/L2W) at 50.  §9.7.2 finding 5 ("estimate_iono_sh ⇒ all
   240 epochs withheld") was measured on a PARTIAL slice, but the floor
   likely holds for the full slice too at n = 50.  **Recommended: scale the
   synthetic roster to ~300** (`scripts/simnet_make_roster.py` takes n; sim
   generation is cheap; producer ≈ 27 s/ep lane-off at 305 ⇒ ~1.8 h per
   240-ep arm) — this also matches the lineage that measured the 77×
   elevation-cutoff effect.  Alternatives: add a third sim band (check the
   RINEX writer), or first re-run the 7-config observability smoke ON the
   fixture to find the minimum n.
2. **Arm the FULL slice, never a partial one**: `estimate_sat_code_bias: true`
   force-arms the coherent set (GIM constraint, iono weighting, IFB) exactly
   like the field; `estimate_iono_sh` rides on top.  W6 bias posture per
   §2.2: `corrections.signal_bias: true` (the injected OSB **is** the B-record
   truth), `enable_code_phase_bias` is forced off by the loader.
3. **The GIM constraint vs synthetic truth is a DESIGN DECISION, not a
   detail.**  `ionosphere_weighting` is load-bearing for W6 rank, but it
   anchors the estimated field toward the REAL DOY-129 GIM while the injected
   iono truth is synthetic — a knowable, gradeable mismatch (both sides are
   in hand; grade it, do not tune it away), or synthesize an IONEX from the
   sim's own truth field (heavier, cleaner).  Either way, first PROVE the
   constraint is armed and fed on the sim data root (§0 discipline: the GIM
   file must exist under `data/2024-129`, else the constraint silently
   degrades and the arm measures nothing).
4. **Grading = per-family common-mode decomposition against truth**, the
   thing the field can only infer: extend `scripts/simnet_common_mode.py` to
   clock w(t) (C records), CB common (B records vs injected OSB), and iono
   a00 (`scripts/sh_vtec.py` over `network_vtec_sh.csv` vs the injected
   field), plus the cross-correlations among the three — the exchange
   signature itself.  Then replicate the elevation-cutoff 15↔25 A/B WITH
   truth (ties to the in-flight field A/B in `runs/cbq_elcut25`).
5. **Standing rules that bite here**: fresh convergence only (datum toggles
   are fingerprinted); ≥ 2 h settled for any level claim; arm
   `cycle_slip.if_phase_td` and `cycle_slip.datum_station_detect` (both
   validated §9.7.10); Release renamed exe; one CWD per arm; pre-register
   predictions per arm.

### 9.10.1 The elevation-cutoff FIELD A/B — direction CONFIRMED, magnitude small (2026-08-12)

`_cbq_12h_elcut25.json` (= the retained control + GIM
`ionosphere_weighting.elevation_cutoff_deg` 15 → 25, the 305-validated value
the FT63 lineage forked below), 12 h, cold, same settled 875-epoch segment,
availability parity (1440/1440 emitted, 0 withholds both):

| | control 15° | **25°** | Δ |
|---|---|---|---|
| wire common raw std | 1.735 m | 1.527 m | −12 % |
| wire common detrended std | 1.298 m | **1.191 m** | −8 % (~16 % of variance) |
| linear trend | −0.548 m/h | −0.454 m/h | −17 % |
| station-attribution table | flat (max 0.053) | flat (max 0.090) | no station driver either arm |

The pre-registered direction holds — the §9.10 low-elevation iono incoherence
DOES feed the clock common — but it is a MINOR contributor here: ~84 % of the
wander variance survives.  The FT63 lineage at 15° was never in the 305-run's
divergence regime (that record was 10° vs 25° with an actual a00 collapse);
raising to 25° is a cheap, availability-free trim worth adopting on the
lineage, not the cure.  **The dominant wander source remains inside the
clock ↔ CB ↔ iono exchanges — the §11 W6+SH synthetic arm is unchanged as the
decisive experiment.**

## §12 The W6+SH ARM — built, and its minimum-n ladder measured (2026-08-12)

Everything in this section was run; each claim names the arm that produced it.
Release only, `gnss_processor_wsh0.exe` (sha256 `6bf857bb…`) for the ladder rungs
and `gnss_processor_wsh.exe` (sha256 `521512b6…`, the same tree plus the §12.4
traces) for the trace A/B, one CWD per arm.

### 12.1 What was built

| piece | file | what it is |
|---|---|---|
| producer template | `config/test_suite/_simnet_wsh_template.json` | the W6+SH posture: `estimate_iono_sh` (which FORCES the whole coupled slice), 25° iono cutoff, SH degree 15, `sh_health_gate`, `if_phase_td` + `datum_station_detect`, `trace_estimability`, the two new σ traces, equatorial clock datum |
| 300-station fixture | `config/sim/simnet300_wsh.json` → `runs/simnet300_wsh/` | 300 solve + 1 consumer, 480 epochs = 4 h @ 30 s, **`corrections.signal_bias: true`** (the injected OSB IS the B-record truth), quiet, pinned storm anchor |
| roster generator fix | `scripts/simnet_make_roster.py` | 9-char markers past n = 99 (`S00101SIM`, scheme `wide3`); the old scheme emits a TEN-character name at n = 100, which is not a RINEX long name at all.  Legacy rosters regenerate byte-identically |
| per-arm products / prefix | `scripts/simnet_make_producer_config.py` | `--products`, `--epochs`; plus a LOUD warning when the clock datum falls back to `stations[0]` (on a Fibonacci roster that is the most POLAR station, and §9.8 measured ~8× more common-level wander from a polar datum) |
| B + receiver channels | `scripts/simnet_grade.py` | §4.4's two-stage S-transform applied to TRUTH (IF lump over the S line's own `cbpin` pair, then zero-mean per channel over the epoch's registered set), vf from the new σ trace; receiver DCB graded as a CONTRAST with the level reported as the absorbed Table-8 gauge |
| exchange decomposition | `scripts/simnet_exchange.py` | §11 item 4: the clock / receiver-DCB / a₀₀ common-mode series, their correlation matrix (series AND first differences), and the measured-vs-predicted exchange coefficient |

### 12.2 The ladder — four rungs at n = 50, 20 epochs each (~90 s per rung)

The 50-station G-A fixture is a valid EMISSION probe for the W6+SH slice even
though it was synthesized in the INT posture: rank structure does not depend on the
injected bias VALUES.  That made the whole ladder cost about six minutes.

| rung | one knob | epochs emitted | what it establishes |
|---|---|---|---|
| 1 `_simnet50_wsh_probe` | — (25° cutoff, deg 4) | **0 / 19** | posture bundle **3/3 armed**, so the §9.7.2 finding-5 withhold is **not** the elevation-cutoff artefact |
| 2 `_simnet50_wsh_eq34` | `phase_datum_eq34` | **0 / 19** | ~~the field's Eq-34 phase datum does **not** lift it — the datum is not the lever~~ **SUPERSEDED §12.8.2**: the datum DOES lift it (rank gate passes); all 19 withholds were the SH gate — a₀₀ −15 965 TECU at its first epoch |
| 3 `_simnet50_wsh_sbc` | `sbasis_complete` | 1 / 19 | gate PASSES and the **ionosphere is destroyed** — see below |
| 4 `_simnet50_wsh_dgtol` | `rank_gate_tol: 1e-12` | **19 / 19** | emits, and the solve is healthy |
| 5 `_simnet300_wsh_smoke` | **300 stations** (deg 15) | **0 / 20** | **station support is NOT the wall** — see the n-invariance below |
| 6 `_simnet300_wsh_n50_carry` | `phase_datum_eq34` + `carry_invariant` | **0 / 19** | the carry cannot lift it COLD, and it cannot warm through a withhold — the withholding gate is the SH gate, not the rank gate (§12.8.2) |

**The direction, named.**  Rung 1's dump (`network_estimability.csv`,
`reduced_N_ss` 143×143, `declared_dim` 1, λmax 1.0283e+05, cutoff 1.0283e-04) shows
exactly two directions below the cutoff:

| ev | composition | reading |
|---|---|---|
| 6.94e-12 | `CLK\|G08…` uniform 0.1768 = 1/√32 | the declared clock gauge ✓ |
| **9.78e-06** | `PB\|*\|L2W` uniform ≈ 0.258 | **the L2W satellite phase-bias common mode** — stable to four digits across every epoch |
| 2.48e-02 | `PB\|G13\|L2W` + `PB\|G13\|L1C` | the weakest REAL layer |
| 2.30e-01 | `SH\|1\|0\|C`, `SH\|0\|0\|C`, … | the SH block, healthy |

With float ambiguities a satellite-common L2W phase bias is absorbable by the L2W
ambiguities, so this is a **gauge the declared dimension (= #constellations = 1)
does not count**, sitting 5.9 decades above the numerical gauge and 3.4 below the
weakest real layer.  The standing record already predicted this direction in words
— *"'dual'/'single' leaves the secondary-band (L2W/L5Q) phase-bias common mode
under-determined at low station count"* — and this is it, isolated and quantified.

**★ It is n-INVARIANT, and that is the finding that reframes the whole brief.**
The 300-station arm (rung 5) reproduces the identical spectrum:

| | n = 50 | n = 300 | ratio |
|---|---|---|---|
| λmax | 1.0283e+05 | 9.1255e+04 | 0.89 |
| clock gauge | 6.94e-12 | −3.44e-10 | (numerical zero) |
| **the extra direction** | **9.78e-06** | **8.97e-06** | **0.92** |
| weakest real layer | 2.48e-02 | 1.69e-02 | 0.68 |

**Six times the stations added no information to that direction.**  Every real
layer scales with the data; this one does not — which is what distinguishes a
structural gauge from a weakly-determined parameter, and it confirms the mechanism
directly: more stations bring more *ambiguities* as well as more data, so the
absorbability is untouched.  The last clause of the record's own wording — *"at low
station count"* — is therefore wrong: it is not about station count at all.

Rung 6 closes the remaining cheap hypothesis.  `carry_invariant` (the DD-chord time
link, which is the one mechanism that could stop the ambiguities from absorbing the
bias) is **refused by the loader without `phase_datum_eq34`** — the cycle basis is
defined only on the eq34 chain datum — so the pair is the correct test, and the
pair still emits nothing on a cold start.  It also cannot bootstrap: the carry needs
emitted epochs to warm, and the gate withholds them.  That is the
degrade-during-withhold deadlock shape this project has already met once.

**Rung 3 is the most important rung, and it is a WARNING.**  `sbasis_complete`
constrains the extra direction along its own eigenvector *at the carried level*; at
epoch 2 that carried level is cold, and the tension went straight into the field:
**a₀₀ = −6292 TECU, rms_all 3541 TECU**, whereupon the SH health gate withheld 18 of
19 epochs.  The rank gate was satisfied and the answer was garbage — the fixture's
whole reason for existing, reproduced on a knob.  The datum-station slip at TOW
302430 is not the cause: rung 4 sees the same slip (G09 at S26000SIM, 28
ambiguities reseeded across 49 stations) and stays clean.  **Do not arm
`sbasis_complete` on a cold start in this posture.**

**Rung 4 is healthy, not merely emitting**: a₀₀ 31.55 → 31.59 TECU across the arc
(σ 1.9 → 0.50), in band, per-degree rms stable; post-fit σ̂₀ **0.962 code** / 0.024
phase (the phase pool is structurally ~0 with free ambiguities — that zero is
expected and documented); consumer converging.  Its tolerance was placed from this
config's OWN dump exactly as the BeiDou card's rule requires — cutoff 1.03e-07 is
4.8 decades above the gauge depth and 2.0 below the weakest real layer — **never
globally**, since `assertNullSpaceRank` throws in both directions.

### 12.3 What the ladder does and does not settle

- It settles that **50 stations are enough to run the W6+SH slice** on this fixture,
  which makes the graded arm ~6× cheaper than §11's recommendation assumed — and
  that scaling to 300 buys nothing for this problem, so §11 item 1's recommendation
  should not be followed.
- It does **not** settle *why the field emits at 303 with the default tolerance*,
  but it narrows the candidates sharply, because station count, the phase datum and
  the carry are now all eliminated on a cold start.  What remains, in order of
  suspicion: (a) the field lineage runs `state_snapshot_carry` — it RESUMES with a
  warm carry rather than cold-starting, so the deadlock rung 6 hits may simply never
  be entered; (b) `cb_kaula_prior` (σ 3 m), a loose prior that the BeiDou card
  measured LIFTING near-null directions, is armed in the field and absent here;
  (c) real IGS observables carry channel diversity the two-signal sim does not.
  Each is a one-knob rung on this fixture and costs ~90 s.
- **A rung that emits is not a rung that is right.**  Rung 4 estimates a direction
  that may be a genuine gauge; if so its emitted L2W phase-bias common mode will
  wander with an honest large σ.  That is a graded question for the settled arm
  (the P channel + `emit_phase_bias_sigma`), not something the emission count says.

### 12.4 The §4.8 export gap is CLOSED (both halves)

`network.trace_sat_bias_states` → `network_sat_bias_states.csv`
(`week,tow,sat,signal,code_bias_m,sigma_m,s0,pinned`) and `network.trace_rx_states`
→ `network_rx_states.csv` (`week,tow,station,kind,key,value_m,std_m`).  Both
trace-only, default-OFF, **no product grammar change** — the B record still carries
no σ, so no consumer fingerprint keyed on the header legend moves.

The satellite trace reuses the URA/combined-σ covariance treatment verbatim (gauge
nulling, datum-pin skip, positive-variance guard, σ̂₀ scaling) so a vf from it is
comparable with a vf from the U record; a datum-pinned column is written with an
EMPTY σ and `pinned=1` rather than its gauge-artefact variance.  The receiver trace
follows the `network_zwd.csv` pattern (`reconstructStation` +
`conditionalLocalVarianceDiag`).

**Invariant #6, same-build A/B**: `_simnet50_wsh_tron` vs `_simnet50_wsh_troff`,
one knob pair, 19 epochs — `products.txt` **byte-identical**
(sha256 `7aaa21eb…` both), no files created when OFF.

First measurements (rung-4 posture, 19 epochs — **ignition, not verdicts**): 64
satellite rows/epoch with σ 0.52 m → 4.5 mm across the arc; the receiver DCB is one
`GPS_DCB` state per station (not per code, so the comparable truth is the injected
GF difference) with a conditional σ of **~32 m** against a ±0.5 m injected spread —
i.e. the receiver DCB is almost pure gauge, which is precisely what Table-8 says.

### 12.5 Pre-registered predictions for the settled arm

Written before the graded run, per §6:

1. **P1 — the exchange.** `simnet_exchange.py` predicts **−7.93 TECU per metre** of
   fleet-mean receiver-DCB error (absorbing a positive receiver GF bias requires a
   negative ionosphere; γ = 1.6469, 0.1624 m/TECU, obliquity 1.2).  If the wander is
   the clock↔CB↔iono exchange the §9.9 campaign cornered, the settled
   `rxdcb_common` and `a00_err` series correlate strongly NEGATIVE with a slope
   within 2× of that.  A wrong sign or a null correlation says the wander enters
   somewhere else and names the next arm.
2. **P2 — the CB variance factor.** The field's cb vf ≈ 10 is either the estimator
   or the data.  On truth, with the σ channel now existing, the settled B-record vf
   answers it.  Prediction: **vf > 2** (optimistic σ), because every other channel
   on this fixture is optimistic (C vf 455, ZWD 1.63 at n = 50 settled).
3. **P3 — the level.** a₀₀ ran ~3.7 TECU BELOW the network-sampled truth mean in
   rung 4's ignition window, the same direction as the field's standing DEFECT 2.
   Prediction: the settled offset stays negative and shrinks — and it must be graded
   time-matched against the truth field's own moving mean, never a single number.

### 12.7 THE GRADED ARM — 466/480 epochs, and all three predictions resolved

`_simnet300_wsh_n50_pin` — 50 stations (stride-6 of the 300-station W6 fixture),
480 epochs = 4 h, `rank_gate_tol` 1e-12 placed from this arm's own dump, graded on
the settled half (233 epochs).  **466 emitted, 14 skipped — all 14 by the A4 σ̂₀
ceiling, ZERO by the observability gate.**  The SH field never left its band (the
health gate is therefore untested here, and the run-end line says so).

| channel | settled statistic |
|---|---|
| **B (satellite code bias)** | residual rms **1.144 m** (C1C 0.841, C2W 1.382), mean −0.004 m |
| B datum realization | emitted per-channel mean **−3.6e-03 m** — the sum-to-zero datum is real |
| **B variance factor** | **489 286** over 14 614 rows |
| C (satellite clock) | residual rms 1.025 m, datum-free contrast 0.889 m, vf 5 684 (s0 = 2.075) |
| P (phase bias) | SD frac L1C 0.266 cyc, L2W 0.318 cyc |
| rx DCB contrast | rms 1.540 m (level −0.240 m = the absorbed Table-8 gauge) |
| rx clock contrast | rms 1.928 m |
| ZWD | residual rms 0.541 m, vf 2 301 |

**P2 — ANSWERED, and not subtly.**  The emitted satellite code bias carries **1.14 m
rms of true error** while its formal σ (median ~4.5 mm settled) claims millimetres:
a variance factor of **~4.9e+05**, i.e. the σ is optimistic by ~700× *in σ*.  So the
field's "cb vf ≈ 10" is nowhere near the estimator's actual calibration on truth —
whatever the field census measures, it is not the code-bias channel's true error
budget.  The formal-σ-collapses-while-true-error-grows pattern of §9.5 finding 2
holds on the code-bias channel too, and harder.

**P1 — the exchange is CONFIRMED, and my pre-registered SIGN was wrong.**
Settled correlations (226 joint epochs):

| pair | series | first difference |
|---|---|---|
| clk_common ↔ rxdcb_common | **−0.939** | −0.009 |
| clk_common ↔ a00_err | **−0.848** | +0.149 |
| rxdcb_common ↔ a00_err | **+0.853** | −0.467 |
| cb_rms ↔ a00_err | **−0.832** | +0.010 |

**The exchange lives entirely in the slow drift.**  Every level correlation is 0.83
to 0.94; every first-difference correlation is ≈ 0.  The four channels do not share
epoch-to-epoch noise — they share a wandering common direction, which is exactly the
shape the field campaign inferred and could not watch.

Coefficient: measured **+5.67 TECU per metre** of fleet-mean rx-DCB error
(corr +0.853) against a **derived +7.93** — 0.72× the ideal-absorption value, i.e.
most but not all of the receiver-DCB common error is being traded against the field
(the ionosphere is also held by its own data and its Kaula prior).

⚠️ The **sign** in §12.5's P1 was wrong, and the correction matters more than the
number: it must be *derived from the model's own coefficients*, not argued.  The
receiver DCB enters the two reference-band code rows with `mu1/dmu` and `mu2/dmu`
(`StateInitializer::resolveReceiverBias`, mu1 = 1, mu2 = γ = 1.6469,
dmu = −0.6469), so on P1 − P2 its signature is exactly **+1 · x_DCB** against the
ionosphere's **(1 − γ) = −0.6469**, giving Δι = **+1.546** m of L1 per metre of DCB
error — POSITIVE.  This is the **second** bias sign convention to bite in one
session (the first: the absorbed IF lump in the C channel, measured at slope −1.08 /
corr −0.78, opposite to §4.2's written `+`).  **Rule for this fixture: never assert
a bias sign from a verbal argument — read the coefficient off the model, or measure
the slope and say which you did.**

**P3 — directionally confirmed.**  a₀₀ runs **−1.42 TECU** below the network-sampled
truth mean on the settled window (σ 0.77), against −3.68 TECU in the 19-epoch
ignition window: negative, and shrinking with settling, exactly as predicted.  It is
graded against the truth field's own per-epoch coverage-weighted mean, never a
single number — the trap §6 of the level card names.

### 12.6 What is still owed

- ~~The 300-station arm at DEFAULT tolerance~~ — run: 0/20, see rung 5.
- ~~A ≥ 2 h settled graded run of rung 4's posture~~ — run: §12.7.
- ~~The 15↔25 elevation-cutoff A/B WITH truth (§11 item 4)~~ — **run and graded,
  §12.9**: the truth grade INVERTS the field verdict on this coherent fixture
  (15° wins every clock-adjacent channel 1.3–4×; the cut FEEDS the exchange,
  0.50× → 0.97× of ideal absorption).  The field's 25° adoption stands on field
  evidence; its separability cost is now measured.
- ~~**Why the FIELD emits at 303 with the default tolerance**~~ — **ANSWERED,
  §12.8.3**: the eq34 chain lifts the near-null (audited complete); the sim's
  non-reproduction is its phase−code alignment convention, not rank.
  `cb_kaula_prior` falsified both alone and paired with eq34.
- The `sbasis_complete` failure deserves its own note in the SBC card: a
  cold-start engage can inject a carried level that no rank instrument sees.
- The exchange decomposition should be re-run on a **datum-station A/B** (§9.8's
  strongest lever): if the clock↔rx-DCB −0.94 is really the datum station's own
  error propagating, moving the datum should move the whole signature.

## §12.8 The CHAIN-PIN AUDIT — rungs 2/6 re-attributed, and the field's emission
## explained (2026-08-12, HANDOFF_wsh_chain_slant T1/T2)

The handoff's T1 asked: per (constellation, signal), do the eq34 chain pins the
election declares actually reach the solve?  Three silent loss stages existed —
`foldAmb`/`foldRpb` early-return without a trace (no state index / no live phase
row), and a station whose local block `est.addStation` rejects is dropped WITH
every folded pin it carried.  Standing invariant #4: an armed datum that places
fewer pins than it thinks reads exactly like a working one.

**Instrument (built, always-on under eq34, log-only).**  The election records
per-group declared gauge dimension Σ(n_c + m_c) (`NetworkChainDatum.cpp`, the
`dr_key` map); the produce phase tallies each fold outcome per station
(`StationAssembly::pin_audit`, summed serially — thread-count independent); the
post-consume comparison accumulates declared / folded / landed / miss-reason /
station-loss counters (`NetworkRunContext::Eq34PinAudit`); `NetworkTeardown`
prints one line per group.  Solve inputs untouched; products unaffected.

### 12.8.1 The audit answer: the chain is COMPLETE — and the decision point flips

`_simnet300_wsh_n50_pinaudit` (rung-2 posture + `station_pd_pin_aware`, default
tolerance, 19 audited epochs):

```
G|L1C: COMPLETE all 19 epochs — declared 1558 (−0 bridged), folded 1558, landed 1558
G|L2W: COMPLETE all 19 epochs — declared 1558 (−0 bridged), folded 1558, landed 1558
```

1558 = 19 × 82 = 19 × (50 stations + 32 satellites): every component's exact
gauge dimension, zero fold refusals, zero pins lost to dropped stations, both
signal groups, every epoch.  **The chain datum is not under-placed** — the
handoff's "if complete, the near-null is elsewhere and the tolerance stays"
branch is the one taken, with one large correction below.

### 12.8.2 Rungs 2/6 were NEVER rank-gate withholds — §12.2's attribution corrected

The audit rung's estimability dump shows the rank gate **PASSING** under eq34:
`n_below = 1 = declared_dim` at the default cutoff.  The L2W common mode is
**lifted 9.78e-06 → 1.67e-04** (~17×, from below the 1.19e-04 cutoff to above
it) — the de Jonge / Odijk chain prescription works exactly as §2 of the
handoff reads it.  The 0/19 emission is the **SH health gate**: a₀₀ −18 780
TECU at the first epoch, −32 633 at the second.

Re-reading rung 2's own record confirms the same signature was there all along:
every epoch `sh_gate_withheld` (a₀₀ −15 965 TECU at its first epoch), and its
run-end line says **"0 of those withheld by the observability gate"**.  So:

- §12.2 rung 2 "the field's Eq-34 phase datum does not lift it — the datum is
  not the lever" is **WRONG in mechanism**: the datum lifts the near-null; what
  fails cold is the *level* the constrained family is realized at.
- Rung 6's "the carry cannot lift it COLD" inherits the same correction: the
  deadlock is real, but the withholding gate it cannot warm through is the SH
  gate, not the rank gate.
- Rung 3 (`sbasis_complete`, a₀₀ −6292) and rungs 2/6 are the **same failure
  class**: constrain the under-determined family at a cold garbage level and
  the tension discharges into the ionosphere.

### 12.8.3 The discharge is a SIM-ONLY phase-alignment artifact — and it answers
### "why does the field emit at 303"

The sim draws integer ambiguities **N ∈ [100 000, 1 000 000) cycles,
independently per (satellite, band, station)** (`src/sim/main.cpp` `ambN`,
deliberately unguessable for the MW/NL exercise).  Phase−code lumps are
therefore 19–250 km, and the eq34 cold pins drive tree-edge ambiguities to
**absolute zero on both bands independently** — each doubly-pinned edge imposes
a random geometry-free pseudo-iono of order (λ₁N₁ − λ₂N₂)/(μ₁−μ₂) ≈ tens of
km.  The per-LOS iono states are the softest GF sink, the SH ties drag the
field, a₀₀ lands at −19 000 TECU (≈ −3.1 km of L1 delay — the observed scale),
and the SH gate withholds.  **Real receivers keep phase aligned to code within
metres**, so the same pins cost the field metres of absorbable phase-bias warp:
the field cold-starts this exact posture healthy.

§12.3's three candidates, resolved:

| candidate | verdict |
|---|---|
| (b) `cb_kaula_prior` σ 3 m, alone | **FALSIFIED** — `_simnet300_wsh_n50_kaula`: 0/19, rank-gate withheld.  A code-side prior does not lift the phase-side type-5 direction |
| (b) with eq34 (the field's actual pair) | **FALSIFIED as the protection** — `_simnet300_wsh_n50_eq34kaula`: 0/19 `sh_gate_withheld`, a₀₀ −19k/−33k unchanged; pin audit again 1558/1558/1558 both groups |
| (a) warm resume | **MOOT** — the field's 12 h arms emit **cold** (§9.10.1: 1440/1440), so warm resume cannot be the explanation; and arming a fingerprinted datum toggle against a foreign snapshot is illegal anyway |
| (c) channel diversity | **NOT NEEDED** for the emission question (it IS the §13 third-band story, separately measured) |

**The answer: the field emits at 303 because its eq34 chain lifts the L2W
near-null above the default cutoff — the audited, complete chain doing exactly
what Odijk's global-network caveat prescribes.  The sim could not reproduce it
for a reason that has nothing to do with rank: the zero-target cold pins fight
the sim's km-scale integer draws.**  Consistent with §13.3.1, where the third
band removes the near-null structurally and the sim emits at default tolerance
with no datum armed at all.

### 12.8.4 Consequences

- **The graded sim posture stays rung 4** (per-config `rank_gate_tol`, direction
  FREE).  On this fixture the free-gauge posture is the healthy one; the eq34
  posture is field-correct but sim-hostile at f = 2.
- **Identified structural fix (not built): first-sighting pin-to-round.**
  `eq34_round` pins re-sighted arcs to λ·round(carry/λ) but first sightings to
  0, and cold there is no carry — while the carry cannot warm because the SH
  gate withholds (the degrade-during-withhold shape again).  Extending the
  round target to the CURRENT float at first sighting would make eq34
  cold-startable on any receiver with large phase−code lumps, and is field-inert
  for aligned receivers (round of a metre-scale lump ≈ the same small integer).
  Default-OFF, byte-identity A/B, per invariant #6 — **SPEC'd as its own card,
  `docs/SPEC_eq34_seed_round.md`** (knob `network.phase_datum_eq34_seed_round`;
  covers the incumbent-without-carry RE-ZERO trap and the carry_invariant hole,
  not just the first sighting).
- The `sbasis_complete` SBC-card note owed in §12.6 now covers three knobs, not
  one: any mechanism that hard-constrains this family cold (sbasis_complete,
  eq34 zero-target pins, and by extension any future datum "completion") must
  either seed its targets from the current state or be barred from cold starts
  in this posture.

## §12.9 The elevation-cutoff A/B WITH TRUTH — the field verdict INVERTS on a
## coherent field (2026-08-12, HANDOFF_wsh_chain_slant T3)

`_simnet300_wsh_n50_pin_el15` (15°) vs `_simnet300_wsh_n50_pin_el25b` (25°),
480 epochs each, settled half graded, **same build** (`gnss_processor_wsh3.exe`,
sha256 `cbaefe0a…`).  The 25° control re-run reproduces the stored §12.7 arm to
four digits on every channel (C 1.0251/0.8891 vf 5683.6; B 1.1442 vf 489 286;
ZWD 0.5413 vf 2301; P 0.2662/0.3178) — which simultaneously certifies the A/B
as single-knob AND that the §12.8 audit instrumentation plus the day's
concurrent tree edits are producer-inert on this posture (invariant #6
discharged by measurement, not inspection).

| settled channel | **15°** | 25° | 25/15 |
|---|---|---|---|
| emitted | 468 (12 A4) | 466 (14 A4) | parity (P-A/B-1 ✓) |
| C residual rms | **0.4908 m** | 1.0251 m | **2.09×** |
| C datum-free contrast rms | **0.5198 m** | 0.8891 m | 1.71× |
| C vf | 1 267 | 5 684 | 4.5× |
| B residual rms / vf | 1.1311 m / 503 478 | 1.1442 m / 489 286 | ≈ flat (P-A/B-3 ✓) |
| P frac L1C / L2W | 0.268 / 0.272 cyc | 0.266 / 0.318 cyc | L2W +17 % at 25° |
| rx clock contrast rms | **1.5023 m** | 1.9277 m | 1.28× |
| rx DCB contrast rms | 1.5052 m | 1.5403 m | ≈ flat |
| ZWD rms / vf | 0.4750 m / 1 653 | 0.5413 m / 2 301 | 1.14× |
| consumer 3D rms | 0.7463 m | 0.8372 m | 1.12× |
| **clk_common wander std** | **0.0598 m** | 0.2478 m | **4.1×** |
| a₀₀ error (mean ± std, TECU) | −0.12 ± 0.52 | −1.57 ± 1.03 | 3× |
| exchange coefficient (of +7.93 ideal) | +3.96 (**0.50×**) | +7.69 (**0.97×**) | — |
| clk↔rxdcb / rxdcb↔a₀₀ corr | −0.848 / +0.667 | −0.939 / +0.859 | tighter at 25° |

**P-A/B-2 is FALSIFIED in the pre-registered direction — the truth grade
INVERTS the field verdict.**  On this fixture the 15° arm is better on every
clock-adjacent channel, and the exchange numbers say why mechanically: the
low-elevation rays carry the OBLIQUITY DIVERSITY that separates clock ↔
receiver-DCB ↔ ionosphere.  Cut them and the family collapses onto the
ideal-absorption manifold — the measured exchange coefficient rises from 0.50×
to 0.97× of the derived ideal, the level correlations tighten toward ±0.9, the
common-mode wander quadruples, and the absorbed error surfaces as 2× clock
truth error.  **The cutoff does not suppress the exchange; it FEEDS it.**

**Reconciliation with the field A/B (§9.10.1: 25° trims wander 12 %).**  No
contradiction — the two arms measure the two sides of one trade.  The synthetic
field is COHERENT BY CONSTRUCTION (the injected iono maps through the same
single-layer geometry the estimator assumes), so its low-elevation rows carry
information and zero pathology: the cut is pure separability loss, price
measured here.  The field's low-elevation rows additionally carry the REAL §9.10
low–high incoherence, and removing that contamination nets −12 % wander
*despite* paying the same separability tax.  This also explains why the field
trim was so small ("~84 % of the variance survives"): the cut is partially
self-defeating, weakening the very containment that bounds the exchange.  The
FT63 adoption of 25° stands on the field evidence — but its cost now has a
number, and any future "raise the cutoff further" proposal must count BOTH
sides (30° would likely lose in the field too).

**Script-version note.**  §12.7's exchange coefficient "+5.67 (0.72×)" is
superseded ON THE SAME PRODUCTS by the current `simnet_exchange.py`: +7.69
(0.97×), correlations essentially unchanged (−0.939/−0.841/+0.859/−0.847).
The A/B above is internally consistent (one script version for both arms);
quote 0.97×, not 0.72×, as the 25° settled exchange ratio.

## §13 The THIRD BAND, and the slant-ionosphere posture (2026-08-12)

Odijk et al. 2016 (J Geod 90:15-44) §5.1, p. 29, on the model with free SLANT
ionospheric delays:

> "the receiver and satellite code biases are **not estimable anymore for the first
> two frequencies**… code biases are only estimable for a (modernized) triple- or
> higher-frequency constellation… the geometry-free receiver/satellite code biases
> become lumped to the ionospheric slant delay."

Under the **vertical** single-layer model instead (their §3.2), only the
iono-free biases are constrained (Table 2, rd 2a/3a), so the geometry-free part
survives as one estimable parameter shared by frequencies 1 and 2 — the DCB
(Eq. 29–32, `d̃^s_2(1) = (μ2/μ1)·d̃^s_1(1)`).  **That is the posture we run**, with
the single-layer tie imposed *softly* (SH rows + `representation_sigma_tecu`), which
is why the dual-frequency code bias is estimable here at all — and why its
information is a model assumption rather than the data.  §12.7's **cb vf ≈ 4.9e+05**
is the price of that, measured.

### 13.1 What was built (sim side, done)

`SimConfig::Signals` — `signals.gps_l5`, `signals.galileo_e5b`,
`signals.synth_osb_amp_m`.  Both bands **opt-in**: noise/multipath/slip draws are
taken per band inside the satellite loop, so appending one shifts the whole stream —
default-off keeps every existing fixture byte-identical, and arming it is a new
lineage by construction.  Verified: a two-band regeneration is byte-identical on
the RINEX and on `truth_atmo.csv`; `truth_bias.csv` gains three provenance columns
(`product_m,synth_m,source`) with the first four columns unchanged row-for-row.

**The synthetic OSB is not optional for this experiment.**  The shipped
`COD0MGXFIN` OSB product carries **no GPS band-5 rows at all** — its GPS inventory
is L1C/L1W/L1L/L1X, L2W/L2S/L2L/L2X, C1C/C1W/C1L/C1X, C2W/C2S/C2L/C2X (verified by
inventory).  A third band synthesized from the product alone would carry an
identically-zero code bias, and no grader could separate "estimated it correctly"
from "the channel is zero".  `synth_osb_amp_m` injects a deterministic per-PRN delta
(no RNG) recorded in the truth file with its provenance.

Fixture: `config/sim/simnet60_tri.json` → `runs/simnet60_tri` (60 solve + 1
consumer, 480 epochs, GPS triple, `synth_osb_amp_m` C5Q 0.60 m / L5Q 0.02 m).  The
roster now also injects a **C5Q receiver bias** (inert on two-band fixtures, since
the sim only applies a bias to a code it emits).

### 13.2 The slant posture is NOT reachable in the producer today — two changes

| piece | state |
|---|---|
| loader path | **blocked**: `estimate_sat_code_bias` with neither SH nor GIM hard-errors (`ApplicationNetworkSetup.cpp:414-437`); `estimate_iono_sh` force-arms the SH field (`GnssConfigLoadNetwork.cpp:2060-2066`) |
| satellite GF datum | **missing**: `buildCodeBiasDatumRows` (`NetworkSbasis.cpp:415`) builds (1) a per-satellite **IF** pin and (2) per-(constellation, signal) sum-to-zero.  There is no **GF** pin |
| receiver GF datum | **already exists**: the receiver DCB is one `GPS_DCB` state per station whose GF signature is exactly `+1·x_DCB` (`StateInitializer::resolveReceiverBias`, μ1/dμ and μ2/dμ), so `network.rx_code_bias_datum` (sum-to-zero over those states) IS Odijk's receiver-side constraint |

Rank accounting to satisfy (Odijk §5.1 + Table 7).  Dropping the iono constraint
opens two additional deficiency types:

- **2b** — receiver hardware biases ↔ slant iono, size **n − 1**; removed by
  constraining the geometry-free RECEIVER code biases (`d_r,GF(1)`; CC-R needs
  n − 1, CC-S needs n).
- **3b** — satellite hardware biases ↔ slant iono, size **m**; removed by
  constraining the geometry-free SATELLITE code biases (`d^s_GF(1)`; CC-R needs m,
  CC-S needs m − 1).

So the work is: a loader path allowing the combination **gated on f ≥ 3** (below
that Odijk says there is nothing to estimate, and the current hard error is
*correct*), plus GF datum rows on the satellite side with the same shape as the
existing IF pin — `μ_GF = (−1, 1, 0, …)/(μ2 − μ1)` (their Eq. 30) instead of
`μ_IF`.  Both must be default-OFF with a byte-identity A/B.

**State the product consequence rather than discovering it later.**  Under Table 8
the emitted quantities change meaning: `ι̃^s_r = ι^s_r + d_r,GF − d^s_GF`, i.e. the
geometry-free biases move INTO the ionosphere, and the B record then carries only
modernized-DCB content (`d^s_1 − d^s_j`, j ≥ 3).  A consumer must apply the network's
ionosphere and its code biases **as a pair**; applying one without the other
double-counts the GF lump.  That is a product-contract change, not a tuning knob.

### 13.3 What can be measured before that work

The triple-frequency fixture is useful immediately, with no producer change, for the
question Fable's chain-datum task is chasing: **does the `PB|*|L2W` gauge survive
when a third band exists?**  Run the existing W6+SH posture (§12) on
`runs/simnet60_tri` and read `network_estimability.csv`.  Three outcomes, all
informative:

1. the near-null disappears ⇒ it was a two-band artefact of the phase datum, and the
   chain audit has its answer;
2. it persists on L2W only ⇒ it is per-band and the datum pins one band and not the
   other — the sharpest possible statement of the pinning defect;
3. it appears on L5Q too (two near-nulls) ⇒ it is "one per non-reference band", which
   is exactly Odijk's type-5 (size f·m) arithmetic and says the pin count scales with
   f, not that a band is special.

### 13.3.1 MEASURED — the third band closes the observability wall (outcome 1)

`_simnet60_tri_wsh` (60 stations, W6+SH, SH degree 4, **default** `rank_gate_tol`,
20 epochs): **20/20 epochs emitted, 0 withheld.**  The two-band posture could not
emit a single epoch without a hand-placed `rank_gate_tol: 1e-12` (§12.2); the
three-band posture runs on the shipped default.

The spectrum says why — `n_below` = **1** = `declared_dim`:

| | dual (n = 50 and n = 300) | **triple (n = 60)** |
|---|---|---|
| directions below cutoff | **2** | **1** |
| clock gauge | 6.9e-12 / −3.4e-10 | −1.3e-10 |
| `PB\|*\|L2W` common mode | **9.78e-06 / 8.97e-06** | **gone** |
| weakest real direction | 2.48e-02 | 1.13e-02 (`PB\|G30\|L5Q+L2W+L1C`, per-satellite) |

The satellite-common secondary-band phase-bias gauge **does not exist at three
frequencies**.  The arithmetic is the ordinary one: each extra band adds 2·n·m
observations against n·m new ambiguities plus ~2m + 2n new biases, so at network
scale a third band is net-informative, and the surplus lands on exactly the
bias/ambiguity family (Odijk types 4/5) that the two-band datum could not pin.

Health: post-fit σ̂₀ **1.064 code**, a₀₀ 35.7 → 41.6 TECU in band, SH gate never
engaged.  B records now carry **C5Q** (640 rows each for C1C/C2W/C5Q over 20
epochs), and the third-band bias grades against its injected synthetic truth:

| channel | ignition residual (20 ep) |
|---|---|
| C1C | 0.274 m rms |
| C2W | 0.451 m rms |
| **C5Q** | **0.450 m rms** (injected amplitude 0.60 m) |
| datum check | emitted per-channel mean **+1.1e-05 m** |
| vf | 1 998 |

⚠️ **That vf is NOT comparable to §12.7's 4.9e+05.**  Different station count,
different SH degree, and — decisively — 20 ignition epochs versus 233 settled ones.
The direction is encouraging and the matched comparison is owed: same n, same
degree, ≥ 2 h settled, dual vs triple, one knob.

### 13.3.2 BUILT AND MEASURED — the slant posture runs, and its code bias is CALIBRATED

Built 2026-08-12 (`gnss_processor_slant.exe`, sha256 `33052874…`):

- **`network.code_bias_gf_datum`** — the per-satellite geometry-free pin,
  `(d_2 − d_1)/(μ2 − μ1) = 0` on the IF-pinned pair (`NetworkSbasis.cpp`, folded at
  `kCodeBiasGaugeWeight` beside the IF pin).  Counters `n_gf_pin_sats` and
  `n_gf_only_pair` (satellites with no third band, whose code bias is then fully
  datum-fixed) ride the existing `Network[W6-cb]` line.
- **The loader gate**: the W6 chain now force-enables `ionosphere_weighting` only
  when the GF datum is OFF, so `estimate_sat_code_bias` + no SH + no GIM is
  reachable; `ApplicationNetworkSetup` gained the matching branch that loads no GIM
  and says so.  Interlocks, all hard errors: GF datum without `estimate_sat_code_bias`
  (nothing to gauge); GF datum with `estimate_iono_sh` **or** `ionosphere_weighting`
  (rd 3b only exists when the ionosphere is unconstrained, and under a single-layer
  constraint the GF bias IS the estimable DCB — pinning it deletes the product);
  GF datum with `frequency_mode: dual` (IF + GF fix both pinned channels, so every
  emitted B record would be the datum itself).

**Arm `_simnet60_tri_slant`** (60 stations, triple, free slant iono, no SH/GIM,
default rank-gate tolerance, 20 epochs): **20/20 emitted, 0 withheld**,
`declared_dim` 1 = `n_below` 1, post-fit σ̂₀ **1.073 code**.  Gauge rows folded:
32 IF + 32 GF + 3 sum-to-zero, `n_gf_only_pair` = 0.

The structural prediction holds to the millimetre — the pinned pair is datum-fixed
and the third band carries everything:

| channel | emitted rms | residual vs transformed truth |
|---|---|---|
| C1C | 0.0022 m | **0.0022 m** |
| C2W | 0.0032 m | **0.0032 m** |
| **C5Q** | 3.889 m | **0.0760 m** |
| **variance factor** | | **1.886** ⚠️ IGNITION ONLY — see §13.5 |

⚠️ **SUPERSEDED BY §13.5 — the settled matched matrix says 8 756, not 1.9.**  The
paragraph below was written from a 20-epoch probe and read the ignition transient as
calibration.  What survives: the STRUCTURAL result (pinned pair datum-fixed, all
content on the third band, gate clean at default tolerance).  What does not: the
"calibrated" verdict.

**That was read as the §13.4 prediction confirmed by five and a half orders of magnitude.**
The same producer's code-bias channel graded **vf ≈ 4.9e+05** in the dual-frequency
single-layer posture (§12.7) and **vf ≈ 1.9** here.  The field's catastrophic
code-bias calibration is therefore a property of the *two-frequency single-layer
posture*, not an estimator defect: at f = 2 the bias is determined by the SH tie (a
model assumption), at f = 3 under Odijk's slant datum it is determined by data.

⚠️ **Caveats, stated because the numbers are dramatic.**  20 ignition epochs against
the dual arm's 233 settled ones; 60 stations vs 50; entirely different iono
posture.  The gap is far too large for ignition to explain, but the matched settled
comparison is still owed before this is quoted as a ratio.

⚠️ **The grader needs `--gf-datum` (auto-detected from the console).**  Under the
slant datum the truth projection removes a SECOND direction:

    t̃_j = t_j − (μ_IF·t) − μ_j·(μ_GF·t)

derived from the paper's own orthogonality properties (μ_IF·e = 1, μ_IF·μ = 0,
Eq. 26; μ_GF·e = 0, μ_GF·μ = 1, Eq. 31), which is why it forces t̃ = 0 on both pinned
channels rather than being told to.  Grading this arm with the single-layer
transform reports **2.3 m / 3.7 m phantom residuals** on two channels that are
datum-fixed to zero — which is exactly what happened on the first attempt, because
the loader had no "armed" log line for the auto-detect to find.  That silent-arming
gap is fixed (invariant #4), and it is a reminder that a datum change is also a
GRADER change.

### 13.5 THE MATCHED MATRIX — settled, and it corrects §13.3.2

One fixture (`runs/simnet60_tri`), same 60 stations, same seeds, 480 epochs = 4 h,
graded on the settled half.  `rank_gate_tol` and `rx_code_bias_datum` matched across
all three arms so each adjacent pair is ONE knob.  Dual+slant is structurally
illegal, so frequency and posture separate in two steps, not one.

| | **A** dual + SH | **B** triple + SH | **C** triple + slant + GF |
|---|---|---|---|
| epochs emitted | 456/480 | 476/480 | 474/480 |
| σ̂₀ scale s0 | 1.160 | 1.909 | 1.997 |
| **cb residual rms** | 1.131 m | 0.962 m | **0.568 m** |
| — pinned pair | C1C 0.832 / C2W 1.366 | C1C 0.821 / C2W 1.342 | **C1C 0.014 / C2W 0.023** |
| — third band | — | C5Q 0.548 | C5Q 0.983 |
| cb vf (published) | 400 776 | 156 743 | 8 756 |
| **cb vf (s0-corrected)** | **541 000** | **570 600** | **34 900** |
| clock residual / contrast | 0.592 / 0.773 m | 1.211 / 1.721 m | 1.119 / 1.590 m |
| clock vf (s0-corrected) | 5 010 | 55 970 | 47 000 |
| ZWD residual / vf | 0.549 m / 2 843 | 0.873 m / 11 828 | 0.872 m / 11 661 |
| **consumer 3D rms** | 1.126 m | **0.290 m** | 0.514 m |

**⚠️ CORRECTION to §13.3.2: "vf 1.886 → CALIBRATED" was an IGNITION ARTIFACT.**
The same posture graded settled gives **8 756** published / **34 900** s0-corrected.
The 20-epoch probe measured a converging formal σ against a not-yet-grown true
error — which is precisely the §9.5 finding-2 signature this fixture exists to
expose ("the formal σ collapses while the true error grows"), reproduced on a new
channel by the very arm that was supposed to demonstrate the cure.  The caveat
attached to that number was the right one and it fired.

**P1 — the discriminator answers POSTURE, not frequency.**  On the s0-corrected
scale (vf is deflated by s0², and s0 differs 1.16 → 2.00 across arms, so the raw
numbers are NOT poolable — the grader says so and it matters here):
A 541 000 ≈ B 570 600 ≫ C 34 900.  **The third frequency alone buys nothing for
code-bias calibration; the slant datum buys ~16×.**  And 34 900 is still σ
optimistic by ~190× *in σ* — the slant posture improves the channel by more than an
order of magnitude and does **not** make it calibrated.

**P2 — CONFIRMED, and it retires a standing objection.**  The recorded 2026-08-04
result (slant 2.2× WORSE, consumer 4.955 → 10.696 m) was dual-frequency and 30
epochs.  It does **not** reproduce here: the third frequency improves the consumer
**3.9×** (1.126 → 0.290 m), and the slant posture gives 1.8× of that back while
still landing **2.2× better than the field's own dual+SH posture**.  Slant does not
cost position once the code biases have a third band to live in.

**P3 — and the surprise.**  The clock and ZWD channels go the OTHER way: A → B
degrades the clock contrast 0.773 → 1.721 m (2.2×) and ZWD 0.549 → 0.873 m, and the
posture does not recover either.  So the third band helps the code bias and the
consumer while hurting the clock and troposphere.  Hypotheses, untested: the extra
band adds 32 satellite phase biases and n·m ambiguities against float ambiguities
with no AR; and the synthetic L5 carries the same code σ as L1/L2, so it may be
adding nuisance parameters faster than geometry-side information.  The next arm is a
noise-realism sweep on the third band, not another posture.

### 13.4 Pre-registered, for when the posture runs

- **The code-bias vf should come DOWN by orders of magnitude.**  At f = 2 the
  channel's information is the soft single-layer tie (hence vf ≈ 4.9e+05); at f = 3
  under slant the modernized DCB is determined by *data*.  If it does not, the
  problem was never the ionosphere model.
- **The exchange coefficient should collapse.**  §12.7 measured +5.67 TECU per metre
  of fleet-mean rx-DCB error (0.72× ideal absorption).  With the GF explicitly
  pinned and lumped into the iono, that trade is closed by construction — the
  remaining clock↔iono correlation is then whatever is left after removing it, which
  is the actual quantity the field campaign wants.
- **The ionosphere should get BETTER and the position should get WORSE.**  Recorded
  measurement (2026-08-04, 63 sta, 30 ep, ignition-only): dropping SH+W6 for pure
  slant ran 2.42× faster but the consumer went 4.955 → 10.696 m.  A ≥ 2 h
  triple-frequency arm is the first honest test of whether that regression survives
  when the third band gives the code biases somewhere to live.

### 13.6 THE AMBIGUITY-INTEGER GRADE — the first absolute integer test, and the
### lane ladder measured (2026-08-12, HANDOFF_slant_triple_next §3)

`_simnet60_tri_slant_1h_ddint` — posture C + `trace_dd_fractional` (stride 1,
max_pairs 2000; note the real knob is `trace_dd_fractional_epoch_stride`), 120
epochs = 1 h, `gnss_processor_matrix.exe` (DA2F9AC0), 116 emitted (0
observability, 4 A4).  Grader: `scripts/simnet_amb_integer.py` (new; `--selftest`
covers the slip-accumulation join and the lane matching).  The graded quantity is
**e = dd_cyc − DD_true in FULL cycles** against the fixture's known integers —
truth differenced in Python ints (N is km-scale by construction), the trace's own
`frac_cyc` used as a per-row join check (229 961 rows, **0 dropped**).

Integer-structure questions are decided per-row and are 1-h-LEGAL (handoff §2);
the per-band absolute magnitudes below shrink with settling (last-quarter columns)
and must not be quoted as settled levels.

| channel | n | med(e) | med\|e\| | p90\|e\| | ≤0.10 cyc | ≤0.25 cyc | **round(e)=0** |
|---|---|---|---|---|---|---|---|
| L1C (per-band) | 76 485 | +0.055 | 4.956 | 20.39 | 1.9 % | 4.4 % | **8.3 %** |
| L2W (per-band) | 76 911 | +0.072 | 5.034 | 20.35 | 1.5 % | 3.3 % | **8.3 %** |
| L5Q (per-band) | 76 565 | +0.092 | 5.076 | 20.40 | 1.6 % | 3.6 % | **8.1 %** |
| **WL(1,2)** | 64 642 | +0.027 | **0.386** | 1.22 | 15.6 % | 34.9 % | **60.2 %** |
| **EWL(2,5)** | 64 690 | −0.001 | **0.081** | 0.27 | 57.7 % | 88.2 % | **98.2 %** |

**The pre-registered "honest expected shape" is confirmed exactly: EWL
near-integer, WL plausible, NL not — and now with absolute numbers.**

1. **The EWL is at its own TRUE integer 98.2 % of the time** (med|e| 0.081 cyc).
   Not "near an integer" — near the *right* integer, verified against truth.  The
   lane cascade's first rung exists on this fixture.
2. **The field's fractional instrument was hiding the error scale.**  The raw
   per-band DDs are not "0.25 cyc off" — they are **~5 cycles off with a
   ±20-cycle p90 tail**, and only ~8 % sit at their true integer.  med|frac| 0.25
   uniform was the "off by 47.3" class, not the "off by 0.3" class.  This is the
   distinction §3.2 of the handoff names, measured for the first time.
3. **The WL:EWL ratio is pure DD-iono arithmetic**: measured med|e| ratio 4.8×
   vs 5.1× predicted from the iono coefficients (WL 1.49 vs EWL 0.29 cyc per
   metre of L1 DD-iono).  The two lanes share one driver and it is the
   ionosphere, quantitatively.
4. **The per-band bands carry an EXTRA error component that is equal-in-cycles
   across bands** — per-band:WL measured 12.8× vs 3.5× iono-predicted, and all
   three bands sit at the same ~5.0 cyc med|e| — i.e. it cancels EXACTLY in
   every between-band difference.  Leading candidate (open, not asserted):
   residual carrier **phase-windup** content — identical in cycles on every
   band, non-cancelling in global-baseline DDs, and capable of full-turn integer
   wraps, which would also explain the low per-band round(e)=0 share.  The
   discriminator would regress e against the modeled windup DD; owed.

Sweep items (handoff §4, 1-h-legal parts): datum inventory 67 = 32 IF + 3 STZ +
32 GF with `n_gf_only_pair` 0; `declared_dim = n_below = 1`; `cbpin=` on the S
line; the pinned pair datum-fixed to millimetres on truth (**C1C 0.0023 m, C2W
0.0032 m rms** — the "holds to the millimetre" claim, now attributed: it is the
DATUM channels that are exact; C5Q, the information-carrying modernized-DCB
channel, is 0.086 m at 1 h and still converging).  Consumer at 1 h: 3D rms
2.64 m, **almost entirely +UP (mean +2.59 m)** — the classic slant-iono
ignition signature (per-LOS iono converging from prior pushes UP); §13.5's
settled 0.514 m says it decays; report-only here.  The exchange was NOT run at
1 h (its coefficient is barred by the 1-h rule; its home is the settled arm,
where §13.4's collapse prediction lives).

**What this changes upstream:** the standing "MW/WL as conditioning — do NOT
arm; net-harmful" verdict was measured at dual frequency, where GPS has no EWL
band.  §13.6 does not overturn it — it bounds it: at f = 3 the EWL DDs are
already 98 % correct-integer as FLOATS, so an EWL-first fixing cascade has a
real substrate on this fixture.  Whether ARMING the lane machinery helps the
solve remains a separate, unanswered question (and the eq34+carry trio the
handoff §3.3 names is BLOCKED on this sim until seed-round exists — the
zero-target cold pins would inject the §12.8 km-scale artifact into a posture
with no SH gate to catch it; `docs/SPEC_eq34_seed_round.md`).

### 13.7 THE SETTLED TRIO ARM — unblocked, and it DIVERGES (2026-08-12)

Same-build (sr.exe 6B1EF5D6) 480-epoch A/B, settled half: `_simnet60_tri_slant_480_ddint`
(float, 474/480) vs `_simnet60_tri_slant_trio_480` (eq34+carry_invariant+ewl+seed_round,
**480/480 — the seed-round card holds; no withhold, max |y| in bound**).

| settled half | float | trio |
|---|---|---|
| per-band med\|e\| | 8.0–9.4 cyc | **~1 720 cyc (p90 ~7 350)** |
| EWL at true integer | **74.7 %** (0.256 cyc) | 5.0 % |
| WL at true integer | 26.2 % | 0.7 % |

**The trio arm has a SLOW RUNAWAY the 20-epoch smoke could not see** (smoke EWL
94 %; settling degrades monotonically — full-window trio EWL 32 % → settled 5 %).
Attribution so far: the **WL-2 lane is exonerated** — its run line reads "0 rows
decided, rows NOT folded into the solve" (statistic-only over 226 421 cycles).
The runaway therefore lives in eq34 + carry_invariant + the every-epoch seed
re-round: 130 290 re-rounds (271/epoch — under carry_invariant no carry ever
exists, so every pin re-rounds every epoch), 591 (sat,σ) re-references, 0
carries shifted/erased — the invariant DD carry refolding across a stepping pin
frame, invariant #1/#7's ratchet shape.  **Do NOT arm the trio posture for
settled work** pending a mechanism card; the FLOAT posture stays the graded
track.  Two float-side notes: the lanes DEGRADE with settling (1 h EWL 96 % →
settled 74.7 % — the §13.6 1 h numbers were the optimistic window), and the
field's frac-only instrument would read this runaway as a NORMAL uniform
fractional distribution — only the truth grade can see it.

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
