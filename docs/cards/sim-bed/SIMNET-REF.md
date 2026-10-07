# SIMNET: the synthetic minimum network, design and grading reference

**STATUS**

- **id:** SIMNET
- **topic:** sim-bed
- **status:** reference
- **summary:** Standing design reference for the truth-graded 3/5/50-station sim network: one process per station, truth channels, truth graded in the producer's basis, gates G-A to G-C, the trap register.
- **dates in the text:** 2026-08-11 to 2026-08-15
- **knobs / posture:** `processing.signal_priorities`, `network.closed_loop.replay_products_path`, `network.health_interval_s`, `network.product_flush_per_epoch`, `network.trace_sat_bias_states`, `network.trace_rx_states`, `network.apriori_source`
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** `runs/simnet300_wsh`, `runs/simnet50_mwblind2`, `runs/simnet60_tri`, `runs/cbq_12h_ctl`, `runs/simnet24_inject_` (the §0 provenance note places every cited `runs/` path under `D:\GnssProcessor_archive\2026-08-12\runs\`)
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md` (preamble), §0 to §8, the G-A / G-B / G-C gates of §6, §9, §10

**Outcome.** The document specifies the sim truth bed: N processes with per-station seeds and a station-aware ambiguity hash, truth transformed into the producer's realized basis before any vf, a rank check beyond the self-consistent gate, the QC tiers inert at small n, and a 16-entry trap register. Its §0 error class stands: an instrument's silence is not a negative result until it was armed, had opportunities and records that outcome. The measured record (§9.5 to §15.8) lives in the cards the [routing stub](../../SPEC_synthetic_minimum_network.md) names; the sim golden now runs a 60-station fixture ([SFR](../../refound/SFR_sim_fixture_coloured_phase_2026-10-01.md)).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
# SPEC — the SYNTHETIC MINIMUM NETWORK (the sim half of G4)
### (a truth-graded 3/5-station fixture: the first ABSOLUTE grade the network producer has ever had, and a cheap exact bench for event response)

2026-08-11.  Companions: `docs/SPEC_sbasis_realization_contract.md` (what is gradeable),
`docs/SPEC_clock_temporal_anchor.md` (Q3, whose guard population this fixture manufactures
on demand), `docs/SPEC_cb_admission_card.md` / the CB-carry card (whose guard G-C rebases
on).

---

## §0 READ THIS FIRST — document status, and the error class it exposed

Written incrementally while the arms ran, so **later sections retract earlier ones**.

| section | status |
|---|---|
| §1–§8 (design, method, traps) | **live** |
| §9.5 G-A calibration table | live; see §9.7.1 on how to read a vf |
| **§9.6 G-B recovery floor** | **RETRACTED** — window artifact; banner in place |
| §9.7.1 clock-vf "plateau" | **AMENDED** — not a readable shape; use the decomposition |
| §9.7.2 QC tier table | live (its "44 false alarms" framing corrected in §9.7.4) |
| **§9.7.3 / §9.7.6 "MISSED"** | **CORRECTED by §9.7.8** — wrong instrument |
| §9.7.4 / §9.7.5 MW chase · §9.7.7 `phase_datum_prioritized` cost | live |
| **§9.7.8 / §9.7.9** | live, but §9.7.8's "100 % detection / 0 % action" is **CORRECTED by §9.7.10**: `refloated` records only the CONSENSUS action; arc-path actions are in `network_events.csv` |
| **§9.7.10 fixes + corrections** | live — current conclusions; blind-spot fix and T4a counter LANDED and sim-validated |
| **§9.8 datum-wander arm** | live — the common level FOLLOWS the datum station; `clock_datum_sum_to_zero` first validation PASS; ⚠ asterisks §9.7.1's vf plateau (pivot-election artifact mixed in) |
| **§9.8.1 mask arms / §9.9 field probe** | live — 25° mask confirms the pivot mechanism but is REJECTED as a remedy (~5 m consumer bias); the FIELD wander (1.735 m) is NOT receiver-code-driven (MBAR rank 48/61) |
| **§9.9.1 / §9.10 product + iono probes** | live — a-priori frame EXONERATED (detrended slope −0.014; product resid common 9 cm); wander is ESTIMATOR-INTERNAL.  Iono input coherent (0.98 @ 300–700 km both-high), BROKEN at low–high ~1000 km (0.38, rel-rms 1.13); causal arm owed |
| §11 W6+SH design brief | **SUPERSEDED IN PART** — items 1 and 3 falsified by §12; 2/4/5 implemented.  Item numbering FROZEN (cited from two configs and `simnet_exchange.py`) |
| §12 / §12.8 / §12.9 W6+SH arm | live — chain-pin audit COMPLETE, rungs 2/6 re-attributed, elevation A/B INVERTS the field verdict |
| §13 third band + slant | live; **§13.3.2's "vf 1.886 = CALIBRATED" SUPERSEDED by §13.5**; §13.7 trio posture is do-NOT-arm for settled work |
| **§13.8 slant/triple grading rules** | live — absorbed 2026-08-15 from `HANDOFF_slant_triple_next.md` (attic'd): the 1 h rule, truth-DD recipe, trap register, owed third-band clock/ZWD regression |
| **§14 chain-slant brief (Odijk theory + T1–T4)** | live — absorbed 2026-08-15 from `HANDOFF_wsh_chain_slant.md` (attic'd).  §14.0 is the T-label map the seven C++ sites and four configs re-point to; §2 of the original ("we have a chain and it did not close this") is CORRECTED — §14.2 |
| **§15 event-injection bench** | live — absorbed 2026-08-15 from `SPEC_sim_injection_bench.md` (attic'd).  The G-B harness §6 called for; **§15.7 is the single owed-list for G-B**, superseding §6's and §9.6's |

**Provenance, one for the whole document** (2026-08-12 archive move).  Every `runs/<name>`
path cited below — `runs/simnet300_wsh`, `runs/simnet50_mwblind2`, `runs/simnet60_tri`,
`runs/cbq_12h_ctl`, `runs/simnet24_inject_{null,fault}`, the rest — resolves under
**`D:\GnssProcessor_archive\2026-08-12\runs\`**, not the working tree.  Do not "fix" them.

### The error class, stated generally

Three headline readings failed the same way — measurement hygiene, not GNSS:

> **A conclusion was drawn from an instrument that does not record the thing being looked
> for, and its silence was read as a negative result.**

| # | instance |
|---|---|
| 1 | **`network_events.csv` does not record RAIM refloats.**  "No event row" read as "not detected"; RAIM had detected at 58σ and acted.  The decision chain is `network_raim.csv`, behind `trace_slip_events` |
| 2 | **A tier reporting `0 flag` with `0 opp` is INERT, not clean** — indistinguishable in every log line |
| 3 | **A guard whose MARKER is unarmed emits nothing.**  §9.6 read zero `CLK_STEP` marks as "not marked" when `emit_clock_disc` was off |
| 4 | **A column recording ONE action path read as THE action record** (§9.7.10).  §9.7.8's "743 spiked, 0 refloated = 0 % action rate" read `network_raim.csv`'s `refloated`, which records only the CONSENSUS refloat, while the T4 arc path's 14 refloats — including the real injected slip at its exact epoch — were in `network_events.csv` all along |

**The check:** before reading absence as evidence, establish the instrument (a) was armed,
(b) had opportunities, (c) records the specific outcome, not a neighbouring one.  Standing
invariant #4 states (b) only.  Treat "the counter stayed at zero" and "the event log is
empty" in prior campaign conclusions as **unverified** until the recording path is
confirmed — RAIM refloat is not the only tier that ACTS without emitting.

**Why this exists.**  Every grade the network producer has ever received is INTERNAL
(self-consistency, datum invariance, cross-instrument agreement, census counts), and all
are satisfiable by a confidently wrong estimator.  The simulator's synthetic data is
externally cross-validated (CSRS-PPP grades it well), so a sim-fed network yields
truth-graded ABSOLUTE error on every estimable quantity for the first time, plus a bench
where the injected event is known exactly — what invariant #1 has never been able to
demonstrate on a real lineage.  **It does not replace the field G4**: event *expression* on
a real lineage is a separate question (the seeded-chain trap).  This certifies the
MECHANISM so the one expensive field replay is confirmation, not exploration.

---

## §1 The architecture decision — N processes, one per station

**Decision: N invocations of `runSynth`, one OS process per station**, derived per-station
seeds, shared realization enforced by *pure functions* not shared state.  Forced: an
in-process station loop needs either N `EphemerisProvider`s or one shared provider, both
structurally blocked.

- **N providers terminate the process.**  Each owns a `LogManager` by value and calls
  `initialize()`; the second call hits `spdlog::register_logger("gnss_processor")` on an
  already-registered name and throws, and **the catch handler re-issues `register_logger`
  unguarded, so the second throw escapes** (`src/core/logging/LogManager.cpp:145-168`,
  re-issue at :167; verified 2026-08-11).  Each constructor also calls
  `ProductCache::getInstance().clearCache()` (`EphemerisProvider.cpp:88-92`), and
  `Antenna::antennas_` is a raw reference into that singleton's vector (`Antenna.hpp:321`).
- **One shared provider silently corrupts three station-dependent terms.**
  `GnssCorrections::prev_windup_` is keyed by **satellite ID alone**
  (`GnssCorrections.hpp:543`) and its same-epoch branch *returns the stored value instead
  of recomputing* (`GnssCorrections.cpp:1969-2002`) — interleaved stations hand each other
  their wind-up accumulator.  Ocean loading is installed at ONE `site_ecef`
  (`EphemerisProvider.cpp:284-306`); `rx_antenna_type_` is one constructor-set string.
  Neither has a per-station setter.

`main.cpp:3024-3042` already establishes the escape hatch: `all` re-invokes the exe as a
**child process** for `spp-reference`, naming LogManager/spdlog and the product caches as
process-global singletons.  Cost is N product parses — negligible at 3–5 stations.

**The contract option (a) must satisfy.**  Correct only if the shared realization is a
deterministic pure function of `(satellite, time, receiver position)` with **zero RNG
consumption**, so N processes reproduce it bit-identically.  Already holds:
`IonoDisturbance` consumes no stream (`turbVtec` = splitmix64 over PRN+epoch-ms,
`stormGain` = FNV-1a over sat id, `IonoDisturbance.cpp:173-211`); orbit / clock / OSB are
product lookups.  **Every new shared channel (§3: satellite clock error, satellite phase
bias) MUST be built the same way** — a pre-generated grid table keyed on (sat, epoch
index), or a self-contained hash like `ambN`.  Never a lazy draw.

### 1.1 The two failure modes this decision creates, and their fixes

Both are *silent* and both produce a beautiful result.

**F1 — shared seeds.**  All four seeds are fixed constants (`noise.seed` 20240315,
`multipath.seed` 20240315^0xA17EC0DE, `atmosphere.seed` 20240315^0x7A0D51DE, `clock_seed`
20240315^0xC10CC0DE).  N stations would share one noise / multipath / slip / ZWD /
receiver-clock realization: between-station differences cancel the noise exactly, the
network looks perfectly conditioned, the common receiver clock is absorbed into the
satellite-clock datum so the clock channel grades flawless while carrying no information,
and every σ̂₀ / formal-σ calibration is meaningless *and beautiful*.  Subtler variant: with
shared seeds but a differing sky, `rng`/`aux_rng` consumption is sky-dependent (3 draws per
visible (sat,band), `main.cpp:2264-2267`), so an equality A/B reads "fine" while all
stations walk overlapping stretches of ONE mt19937_64 sequence.  **`tropo_rng` is worse**:
drawn at most once per epoch *outside* the satellite loop (`main.cpp:2083-2100`), so its
consumption is sky-INdependent and N stations get a bit-identical ZWD series — the one
nuisance the network exists to separate from clock becomes exactly degenerate with it.
*Fix (S10):* derive all four seeds as `hash(base_seed, station_id)`.

**F2 — station-blind `ambN`.**  Verified 2026-08-11: `ambN` hashes only
`(sat[0], prn, band_idx)` (`main.cpp:1954-1975`), no station term.  Every between-station
SD/DD ambiguity would be **exactly zero** — AR reports 100 % fix on perfect integers while
measuring nothing, and no run gate flags it (`worst_clean`, `worst_isb_clean`,
`worst_if_resid` all pass).  *Fix (S5):* station enters the hash and the truth file.

---

## §2 The fixture

| property | value | why |
|---|---|---|
| date | **2024-129**, pinned | reuses the bundled SP3/CLK/BIA/nav; a novel date needs SP3/CLK writers (large) |
| data root | **`data/_simnet/2024-129/`**, NOT `data/2024-129/` | trap #29: synthetic obs in the real date dir are picked up by PathResolver's station-name glob and mixed with real observations, no error |
| station names | `SNA000SIM` … `SNE000SIM` (9-char, `SIM` country) | cannot collide with a real IGS 9-char id |
| constellation | GPS-only | one clock gauge; hand-auditable rank |
| signals | L1C/L2W (2 signals) | matches the IF-pin pair class |
| rate | 30 s | matches the field lineage |
| kinematic | `synth-static` | CORS geometry — the producer holds coordinates fixed, so `position_ecef` == truth exactly |
| arc | **≥ 2 h** for any LEVEL claim; 20-epoch smokes for wiring only | the level channel's convergence budget is ~2 h, and this project has already had a sign reversal between 20-epoch and settled arms |
| n | **3** (rank tier) and **5** (first redundancy tier) | §8 |
| geometry | continental spread — the LONGEST baselines keeping full common visibility at the mask, **certified offline** | §2.1 |
| mask | 10° (sim default) | |

### 2.1 Geometry must be CERTIFIED, not assumed

Short baselines (< 50 km) make iono and tropo near-common-mode and DD ambiguities trivially
resolvable, leaving the network's reason for existing unexercised.  Long baselines
decorrelate them but risk bipartite fragmentation of the (station × satellite) graph, and
`constexpr int kMinStationsPerSat = 2` (`ApplicationNetworkAssembly.cpp:1494`, verified)
**erases a satellite's observations from EVERY station**, not just its shared columns.

*Procedure:* generate, then run `netDatumGraphComponents` offline over the sim's own
`truth_visibility.csv`; require **k = 1 component at every epoch** before any arm runs.  A
fragmented epoch makes `observed` = k while `declared` stays 1 and the gate throws
"under-specified" (`NetworkSbasis.cpp:400-411`), withholding the epoch — which reads as a
producer defect.

### 2.4 The 50-station globally-distributed variant

Added 2026-08-11.  The 3/5-station fixtures cannot grade the QC stack at all — all seven
tiers report **zero opportunities** at n = 3 (§7, measured).  This variant gives them real
opportunity counts and extends the vf-vs-station-count trend far enough to be readable.

| property | value | why |
|---|---|---|
| n | **50 solve + 1 held-out consumer** | above `rx_cb_carry_demean_min_stations` = 8, the highest tier threshold in the standard set |
| distribution | Fibonacci (golden-angle) lattice, \|lat\| ≤ 72° | near-uniform on the sphere; a lat/lon grid clusters at the poles.  GPS is inclined 55°, so an uncapped polar station sees everything at low elevation and would confound "more stations" with "worse stations" |
| coverage | even, not land-biased | real networks cluster (Europe, N. America) and that clustering is itself a conditioning property; even coverage is the cleaner instrument for grading QC tiers.  A clustered variant is a separate arm, not a correction to this one |
| generator | `scripts/simnet_make_roster.py` | 50 hand-written coordinate triples is exactly where a silent error lives, and a HELD coordinate is never estimated so nothing downstream can catch one |
| posture | the **full standard armed set** (CLAUDE.md) | `phase_leveling`, `raim_within_epoch`, `estimate_iono_sh` + `sh_health_gate`, `cold_clock_seed`, `mw_detect`, `code_bias_disc_jump_test`, `emit_clock_disc` + `clock_disc_jump_test` |

`sh_health_gate` **requires `estimate_iono_sh`** — without it the loader disables the gate
with a warning, and a disabled gate reads exactly like a clean one.

### 2.2 The two bias postures are separate arms, one knob apart

Mutually exclusive in the loader; they grade different channels.

| arm | `estimate_sat_code_bias` | `enable_code_phase_bias` | `corrections.signal_bias` | grades |
|---|---|---|---|---|
| **INT** (integer arm) | false | true | **false** | P record as exact integers × λ (§4.3); clean ambiguity truth |
| **W6** (bias arm) | **true** (forces `enable_code_phase_bias` OFF, `GnssConfigLoadNetwork.cpp:2015-2038`) | false | **true** | B record vs injected OSB truth (§4.4) |

Under the default posture the producer *removes* the same `.BIA` the sim injected, so **no
B records exist at all**; under W6 the injected OSB **is** the truth.  Conversely the sharp
integer test needs `signal_bias` OFF, because the shipped 2024-129 `COD0MGXFIN` OSB carries
**377 L-signal rows** (~0.19 m ≈ 1 L1 cycle) — so the in-code comment at
`main.cpp:1964-1966` claiming *"No separate satellite phase bias is injected, so the
integers remain RESOLVABLE via the MW WL→NL cascade"* is **FALSE whenever
`signal_bias=true`**.  Fix that comment as part of S7.

### 2.3 Mandatory config hygiene (each of these silently produces a null run)

| rule | why |
|---|---|
| `processing.signal_priorities` **must be present** | absent ⇒ `PppModel::selectObservationTypes` returns empty for the missing constellation key (`PppModel.cpp:1994-2000`) and **every station contributes zero observations**, no error.  Copy the block from `config/ppp_rtk_network_internal.json:29-32`.  (Config arrays REPLACE, they do not merge — `GnssConfigJsonHelpers.hpp:63-74`) |
| no `network.closed_loop.replay_products_path` key, even empty | its mere PRESENCE short-circuits `runNetwork` to the consumer-only replay driver *before* input validation (`ApplicationNetwork.cpp:185-204`) |
| arm `network.health_interval_s` + `network.product_flush_per_epoch` | only `health_trace` flushes mid-run; every other stream can lose its buffered tail |
| arm the standard set per CLAUDE.md | minus what is inert at this scale (§8) |
| identical epoch grid at every station | `MultiRinexSync` builds the schedule from the datum station's epoch list and matches others into it (`MultiRinexSync.cpp:307-366`); a rate mismatch silently discards epochs |
| Release-only, renamed exe copy, no overlap with another session's field arm | check `Get-Process gnss*` first |

---

## §3 Truth channels

| quantity | today | action |
|---|---|---|
| **coordinates** | `truth_position.csv` | + `station`.  Its `clock_bias_s` is `setprecision(10)` ≈ 3 cm quantized — **never grade the clock from it** |
| **receiver clock** | `truth_clock.csv` (`setprecision(12)`) | + `station` |
| **ZWD** | `truth_tropo.csv` per epoch | + `station`; **per-station `tropo_rng`** (S6).  `truth_scalars.csv`'s `zwd_m` is the arc MEAN, not instantaneous |
| **slant iono per LOS** | `truth_atmo.csv` — `stec_quiet/sed/tid/turb/total` (**already SLANT** TECU), `I_L1_m`, `obliquity`, IPP | + `station`.  `obliquity` is published so double-mapping is *auditable*, not so it can be applied |
| **ambiguities** | `truth_ambiguities.csv` — station-blind, pre-slip only | **S5**: station in the hash + file; arc-start/end tow; explicit arc break at re-rise (today the integer never resets when a satellite sets and re-rises).  True value = `N_int + Σ truth_slips.delta_cycles` |
| **satellite clock** | **absent** | **S4**: `truth_satclock.csv` (week,tow,sat,clk_s,clk_err_s) + a synthetic error channel.  Without an injected error `truth_C ≡ 0` (§4.2) — a valid null test, not a measurement |
| **satellite code bias** | `truth_bias.csv`, OSB, gated on `signal_bias` | + `source` column; + `synth_m` if a synthetic delta is injected |
| **satellite phase bias** | present but **mislabelled** (§2.2) | fix the comment; **S7** adds a configurable synthetic bias so the P channel is controllable rather than "whatever COD shipped" |
| **receiver code bias** | **absent** — the only receiver-side bias in the model is one scalar GPS→Gal ISB | **S-rxcb**: per-(station, RINEX code) injection + `truth_rx_bias.csv`.  **NON-OPTIONAL** — §4.5 |
| **receiver phase bias** | absent | pure gauge under the producer's basis, so zero-injected is *legitimate truth* — but it must be **stated** in `truth_datum.json` (S13), not left implicit |

### 3.1 Where the satellite clock error may and may not be injected

Adding the error inside `EphemerisProvider::clock()` disables the safety net: it moves
*both* `forwardPseudorange` and `solveSpp`, so the **1 mm noise-free self-consistency gate**
(`main.cpp:2830`) passes no matter how wrong the arc is; it also perturbs `runSppReference`
(the Part-B answer key) and trips `probe-orbit`'s 1 µs CLK-agreement gate.  Injecting into
`forwardPseudorange` only fails the opposite way: the 1 mm gate blows up at metre level and
reads as a bug in the injection.

**Correct:** a new `clockTruth()` used at `main.cpp:425,429` only, `solveSpp` and
`spp-reference` left on `clock()`, **plus a re-worded gate asserting the SPP residual now
equals the injected error rather than 0.**  Second hazard: `|sat_clk| < 1e-12` is overloaded
as the clock-*coverage* test (`main.cpp:430`, `return false` ⇒ satellite silently dropped) —
test coverage on the product clock, add the error **after**.  Third: `forwardPseudorange`
runs **before** the elevation-mask test (`main.cpp:2148-2150`), so any lazily-drawn RNG
there fires for satellites that are then discarded — draw count becomes sky-dependent and
storm-on/off byte-identity dies.  Pre-generate on the epoch grid.

---

## §4 The estimable-function grading map

**The governing structural fact:** the sim reuses the processor's own correction chain, so
every reused term cancels and its residual truth is *structurally zero*.  Gradeable is
exactly (i) what the sim injects that the producer does not model, and (ii) the
datum-invariant contrasts of what both model.

### 4.1 The method correction that makes the χ²/vf audit valid

Transforming the ESTIMATE into a between-satellite contrast to kill the datum **breaks the
variance factor**: σ²_e ≠ σ²_s + σ²_s′, because the two estimates share the datum and are
strongly correlated, so the published marginal σ OVERSTATES the contrast σ and biases vf
**downward** — making a mis-calibrated estimator look calibrated.  G-A exists to ask *"is
the field cb vf ≈ 10 the estimator or the data?"*, and a downward-biased vf answers it
**backwards**.

**Therefore: transform TRUTH into the producer's realized basis, not the estimate into
contrasts.**

```
e_s   = estimate_s  −  S[truth]_s
vf    = mean( e_s² / σ_s² )          σ_s = the marginal, datum-CONDITIONED formal σ
```

Both sides then live in the same basis and σ_s is the marginal variance of exactly that
quantity.  Requires knowing the realized datum exactly — realized pivot, `cbpin` pair,
zero-mean membership set — all of which the **S line publishes** and the grader must READ,
never assume.  The contrast form is retained as a **datum-free cross-check** (it must
agree), which catches a misread datum; only the basis-transformed form carries a vf.
Membership caveat: the zero-mean set is per-epoch (a code column registers only when ≥ 2
present stations hold it, `ApplicationNetworkAssembly.cpp:1613-1620`), so **apply the
transform per epoch with that epoch's registered set.**

### 4.2 C record — satellite clock

Wire meaning (contract §B.1): `dt̃ˢ = [dtˢ + d_IFˢ] − dt_pivˢ − A_c`, a correction to the
a-priori named in the S line's `apriori_id`, which `ApplicationNetworkSetup.cpp:1016-1031`
sets to the **resolved SP3+CLK basenames**.  The sim injects that same CLK product, so
a-priori and truth are the same object:

```
truth_C(s,t) = Δdtˢ(t) − Δdt_piv(t) − [d_IFˢ − d_IF_piv] − A_c
```

⚠️ **SIGN CORRECTED IN PLACE 2026-08-12** (was written `+ [d_IFˢ − d_IF_piv]`).  The
absorbed IF lump in the C channel is **−d_IF**, measured: slope **−1.08**, corr **−0.78**
(§12.7, §14.6).  A grader copying the `+` form graded the lump backwards — the first of two
bias-sign conventions that bit in one session; rule in §14.6.

`Δdtˢ` is the **injected** error (S4).  With no injected error and `enable_code_phase_bias`
on, **truth_C ≡ 0 for every satellite** — the G-A null.  Cross-check (datum-free):
`C(s) − C(s′) == Δdtˢ − Δdtˢ′`.  Never difference raw `C` against a raw absolute simulated
clock: that returns the datum as an error.

### 4.3 P record — satellite phase bias

Under the default legacy datum the pivot station's ambiguities are hard-pinned to zero at
w = 1e10 (`ApplicationNetworkAssembly.cpp:2220-2246`), so

```
truth_P(s,j) = −λ_j·N_piv(s,j) + b_φ^{s,j}(unmodelled) + κ_j
```

with `N_piv` the **pivot station's** injected integer at that epoch (`N_int + Σ slips`) and
`κ_j` a per-signal constant absorbed by the receiver clock.  Grading rule,
between-satellite at fixed signal:

```
P(s,j) − P(s0,j)  ==  −λ_j·[ N_piv(s,j) − N_piv(s0,j) ]  +  [b_φ^{s,j} − b_φ^{s0,j}]
```

which in the **INT arm** is *exactly* integer × λ_j — the sharpest single test in the
fixture, and the reason S5 is non-optional (§1.1 F2).

### 4.4 B record — satellite code bias (W6 arm only)

Apply the producer's own two-stage S-transform **to truth** before comparing.  **(1) IF
lumping**: per satellite form `a₁·b(s,j₁) + a₂·b(s,j₂)` over the pinned pair
(`a₁ = f₁²/(f₁²−f₂²)`, `a₂ = −f₂²/(f₁²−f₂²)`, `a₁+a₂ = 1`) and subtract it from both of
that satellite's channel values; the pinned pair is `selectIfPinPair`'s **lexical** choice —
read `cbpin=<const>:<s1>-<s2>` off the S line, and note it is **not** necessarily the
consumer's pair (`project_day129_pivot_cb_ratchet`).  **(2) Zero-mean per channel** over
that epoch's registered set.

### 4.5 Ionosphere — the Table-8 identity, and why rx-CB injection is mandatory

Contract §B.4: `ι̃ˢ(i) = ιˢ(i) + d̄_r,GF(datum) − d_GFˢ(datum)`.  Two terms, two independent
assertions:

```
(a) between-satellite at one station  →  kills the receiver term:
    (ι̃_r^s − ι_r^s) − (ι̃_r^{s'} − ι_r^{s'})  ==  −( d_GF^s − d_GF^{s'} )

(b) satellite-mean at one station     →  leaves the receiver term:
    mean_s( ι̃_r^s − ι_r^s )  ==  d̄_r,GF − mean_s( d_GF^s )
```

**(b) is the headline system-level unit test of the S-basis contract**, and it **requires
non-zero injected receiver code biases**: with `d_r,j ≡ 0` — the state of the sim today — it
degenerates to `0 == 0`.  Hence the rx-CB channel in §3 is NON-OPTIONAL.  A between-station
SD at fixed satellite is the third form (cancels `d_GFˢ` exactly and `d̄_r,GF` as a fleet
scalar) and grades the iono FIELD directly against `truth_atmo`.

**Do not read `network_ionosphere.csv` for any of this.**  It writes `x_r` alone — the
reconstructed *correction* about the seeded linearization point, not absolute slant iono
(`NetworkIonoDiag.cpp:89-94`).  The ZWD writer at `NetworkIonoDiag.cpp:105-131` is the
correct template: it *does* add `x_lin`.

### 4.6 ZWD — the one clean vf channel available today

`network_zwd.csv` is the only per-station state channel already emitted with a
datum-conditioned formal σ (`zwd_m = x_lin + x_r`, `std_m =
sqrt(conditionalLocalVarianceDiag)`, `NetworkIonoDiag.cpp:105-131`).  ZWD is not
gauge-bearing — grade it **absolutely** per station against `truth_tropo.csv[station]`, and
use its `std_m` for the χ² calibration the C/P/B channels cannot supply as cleanly.

### 4.7 Consumer position — the only fully datum-free absolute output

`logs/closed_loop_trace.csv` logs the error vector directly
(`pos_err_3d_m,east_m,north_m,up_m`, `NetworkEmitStore.cpp:649-703`) against the held-out
station's marker→ARP-reduced truth; a synthetic station with zero eccentricity grades
identically to its configured ECEF.  AR-confounded, so an **end-to-end verdict, not a
channel diagnostic**.

### 4.8 The one export gap, and how it is closed WITHOUT touching the wire

There is **no formal σ on the B record in any configuration** — verified 2026-08-11, the
legend is literally `# B,sat,signal,code_bias_m[,code_disc]` (`NetworkProductIO.cpp:549`,
writer at :649).  So the CB variance factor — the question G-A exists to answer — is
structurally unobtainable from the product.  Adding a `sigma=` token would change the
**product grammar** and move any consumer fingerprint keyed on the header legend.
Rejected.

**Instead:** a default-OFF trace CSV `network_sat_bias_states.csv` (per-(sat,signal)
estimate + conditional σ) on the same `reconstructStation` + `conditionalLocalVarianceDiag`
pattern `network_zwd.csv` already uses, behind `network.trace_sat_bias_states`.  Trace-only,
default-OFF, structurally byte-identical when OFF, **no product grammar change**.  A sibling
`network_rx_states.csv` (`network.trace_rx_states`) does the same for the receiver clock and
receiver code bias, which today appear only as census *labels* carrying standardized prior
residuals, never as estimates with σ.

---

## §5 The rank assertion — and why the obvious one is worthless

**`declared_dimension` is NOT the Odijk Eq.-24 count.**  It is the *residual* shared
null-space expected AT THE GATE: `plan.declared_dimension = plan.shared_datum.size()` = one
pin per constellation (`NetworkSbasis.cpp:159-188`), so GPS-only it is **1**, with **zero
dependence on m, n, or f**.  Worse, **the gate is self-consistent by construction**: it
compares `observed` against `declared`, which the same code path produced.  Flipping
`clock_datum_sum_to_zero` moves *both* sides 1 → 0 and the gate still passes.  "The gate did
not throw" says nothing about which datum was realized.

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

| # | assertion |
|---|---|
| 1 | `plan.declared_dimension == 1`, **and** `observed == 1` via `assertNullSpaceRank(N, 1, &observed, est.rankGateTol())`.  ⚠️ **Pass `est.rankGateTol()` explicitly** — the `tol` default in the *signature* is 1e-6 (`NetworkSbasis.hpp:690-692`) while production always passes `NetworkEstimator::kNullSpaceTol` = **1e-9**; a test omitting the argument exercises a 1000× looser gate than any real run |
| 2 | **Independently of the gate**, the *installed datum row inventory* at a NAMED epoch with a KNOWN visible set (published in `truth_visibility.csv`) — pins per class, matched against the contract §A table.  The eq34/pivot pins are **elected per epoch** and installed only when the state exists, so this is only meaningful at a pinned epoch |
| 3 | **k = 1** bipartite component at every epoch (§2.1) |
| 4 | Do NOT arm `sbasis_complete`: it makes the gate read `observed == declared` unconditionally by inspecting the *completed* system (`NetworkSolveGlue.cpp:575-597`), so any fixture measuring realized dimension through the gate reports the completion's success instead of the deficiency |

---

## §6 The gates

Arms are one knob apart with **pre-registered predictions**.  Sim runs are seconds; that is
a reason for more arms, not looser method.

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

**Any systematic residual vs truth here is a MODELING FINDING — report it, do not tune it
away.**

### G-B — event injection, one class at a time, magnitude sweeps at known epochs

> ✅ **BUILT — the harness this subsection specifies is §15** (arm generator, pre-flight,
> differential grader, mandatory negative control with its own exit code 4).  First bench
> run measured in §15.6; **the single owed-list for G-B is §15.7**, superseding the "still
> owed" paragraph at the end of §9.6.

Classes: satellite clock step (Q3's guard population) · CB step/ramp (the DOY-129 ratchet
class) · cycle slips (slip funnel) · iono ramp (turbulence) · station outage/return
(set-change / staleness).  Per event, grade: **detection epoch (latency)** · **estimated vs
injected magnitude** · **wire mark tie-out** (event ↔ R/counter ↔ consumer reaction, the
`scripts/q3_mark_tieout.py` pattern) · **consumer transient vs truth**.  Report the
**smallest reliably caught step per guard** — an empirical MDB.  **Thresholds stay
FIELD-derived; the sim measures detection floors, it never tunes them.**

Mandatory negative control: **an injected event that produces zero guard trips is a WIRING
FAILURE, not a quiet day.**  The grader asserts this explicitly (the
`q3_ladder_grade.py:126-127` INERT-line pattern).  This fixture is the one place where that
distinction is provable.

### G-C — the G4 bench

Q3 anchor at rung-2 posture (q = 3e-7) plus the CB guard armed **in sim**; injected events
at DOY-129-like magnitudes must **trip, re-solve, mark**, and the consumer must **ride
through** — invariant #1 demonstrated against known truth.  *Coordination:* the CB-carry
card is in flight and **lands first**; G-C rebases on its guard rather than inventing one.

---

## §7 What this fixture CANNOT certify (read before quoting it)

Several QC tiers are **structurally inert** at this scale, so "armed and never fired" here
means nothing:

| tier | default | at n=3 | at n=5 |
|---|---|---|---|
| `raim_min_stations` | 3 | requires **unanimity** — inert | real 3-of-5 ✅ |
| `cb_admission_min_stations` | 4 | folds to `max(2,min(4,n))` = 3 | 4 ✅ |
| `rx_cb_carry_demean_min_stations` | 8 | inert | **still inert** |
| `fast_clock.min_stations_per_sat` | 3 | marginal | ✅ |
| `parallel_min_stations` | 10 (strictly-greater) | serial | serial |
| `kMinStationsPerSat` | 2 (hardcoded) | **one station from binding** | comfortable |

⇒ **the 3-station fixture is a RANK / estimability instrument; the 5-station is the first
tier where QC response is gradeable at all; the rx-CB de-mean tier needs ≥ 8 and is out of
reach of both.**  Reasoning that quotes the raw defaults (e.g. "cb admission needs 4")
mispredicts what actually happens.

**MEASURED 2026-08-11, not inferred.**  Run-end flag-rate summary, n = 3 G-A arm, all seven
tiers at **0 opp / 0 flag**: IF-phase slip (T1) implied α 1.24e-15; within-ep trigger (T2)
2.70e-03; post-fit RAIM cell (T3), RAIM consensus (T3), attribution SAT (T4) and clock-model
NIS (T5) all 1.97e-09; clock-monitor (T6) —.  **Zero OPPORTUNITIES, not zero flags** — the
invariant-#4 distinction, and the whole reason for the 50-station variant (§2.4).
`scripts/simnet_grade.py` reports `opp` alongside `flag` and labels a 0-opportunity tier
INERT rather than clean.

Also outside scope: ignition (any arm < 2 h grades the transient, not the level);
`network.apriori_source: "brdc_rts"` (hard-refuses when the cache holds SP3/CLK, which the
sim cannot synthesize without — `ApplicationNetworkSetup.cpp:355-372`); and `sim::Verifier`,
whose tolerances are calibrated to single-receiver float PPP and which prints an
authoritative-looking `=== VERIFIER PASS ===` with silently-skipped gates
(`Verifier.cpp:294-329`).  **The network grader is a new Python script; the Verifier's
user-side exam contract is left intact.**

---

## §8 Trap register (grader must defend against each)

Beyond §1.1, §2.1, §3.1, §4.1 and §4.8:

| # | trap |
|---|---|
| 1 | **Row-index joins.** `truth_atmo/tropo/otl/slips/ambiguities` are written *before* `if (clean.size() < 5) continue;` (`main.cpp:2409`); `truth_position/clock/visibility` and the RINEX epoch *after*.  Different row counts.  **Join on tow, never row index.** |
| 2 | **`stec_*` are already SLANT.**  Multiplying by the published `obliquity` gives a wrong-but-plausible value that **still cancels in IF**, leaving no residual signature |
| 3 | **Datum-pinned columns get no σ.**  The clock pivot satellite gets no U record and no `P sigma=` (`NetworkSolPrep.cpp:427-492`) — a vf audit iterating emitted satellites silently excludes the satellite the datum lives on |
| 4 | **Shape-dispatched, config-dependent field counts.**  C writes `clock_c1_m_s` unconditionally under `serialize_disc_counters`, conditionally otherwise; P trailing fields dispatch by shape (bare int = disc counter; `sigma=`/`sigc=` in any order).  **Stop the positional parse at the first field containing `=`; read the `#` legend** (grammar-complete, enforced by `HeaderLegendDeclaresEveryEmittedField`) |
| 5 | **`P sigma=` changes meaning under `phase_datum_ensemble`** (becomes a between-satellite SD sigma, reference sat gets the group median).  Never pool σ across datum modes |
| 6 | **A withheld epoch writes an H heartbeat with NO E line** (`NetworkEmitStore.cpp:121-143`) — a grader iterating E records reads a withheld era as a data gap |
| 7 | **Blank means "did not run", never 0** (`sigma0_pf`, `pc_*`, `ddfold_*`, `adm_*`, `prires_*`).  Coercing blanks to 0 turns "never ran" into "measured zero" (invariant #4).  `network_epoch_status.csv`'s row **width** changes with three independent toggles — read the header |
| 8 | **`truth_scalars.csv`'s key set is config-dependent**, and `truth_slips.csv` gains a trailing `source` column only when scripted slips exist.  A fixed-schema parser reports 0.0 defaults and a passing verdict on an unarmed channel |
| 9 | **Silent overwrite.**  All 11 truth writers and `obs_path` are fixed names in one `outdir`; two stations into one dir leaves one set and **PASSes** |
| 10 | **Every station self-identifies as `SARV`** — `hdr.marker_name` is never assigned (`RinexObsWriter.hpp:49-51`) |
| 11 | **A typo'd path exits 0.**  `ProductCache::loadProducts` only WARNS on empty orbits/clocks; a non-pivot station's RINEX parse failure is non-fatal |
| 12 | **Antenna mismatch is a warn**, not an error — receiver PCO/PCV silently zeroed (`ApplicationNetworkSetup.cpp:586-641`) |
| 13 | **`mp_code` and `mp_phase` come from the SAME Gauss-Markov draw** (`main.cpp:2277-2278`), so MW and GF see a *deterministic* multipath relationship |
| 14 | **Shared `logs/`.**  `Application::log_dir_` is hardcoded `"logs"` relative to CWD; two arms from one directory overwrite each other.  Replicate `q3_ladder_run.ps1`'s refuse-if-`gnss_processor*`-lives check |
| 15 | **The sim writes no nav file into `outdir`** — outside `data/<YYYY-DOY>/`, every nav-dependent path (Klobuchar, broadcast anchoring, `cold_clock_seed`) goes silently empty while the run completes |
| 16 | **Warm-resume fingerprinting**: an arm chain reusing `state_snapshot_path` can silently cold-start and read as a clean resume |

---

## §9 Build manifest

**Sim (all default-OFF / byte-identical when unarmed):** `SimConfig` gains a `stations[]`
shape + absolute storm anchor + per-station seed derivation; `main.cpp` gains a
multi-station driver subcommand, per-station identity (marker, filename, approx pos),
per-station truth fan-out, per-station ZWD stream, station in `ambN`; new
`SatClockError.{hpp,cpp}`, **`ScriptedSlips.{hpp,cpp}`** (⚠ CORRECTED 2026-08-15 — drafted
here as `ScriptedEvents.{hpp,cpp}`; it landed as `include/sim/ScriptedSlips.hpp` /
`src/sim/ScriptedSlips.cpp`, both present, both in `CMakeLists.txt`, used by
`tests/sim/test_sim_selfconsistency.cpp`.  No `ScriptedEvents` file exists), rx-bias
injection, `truth_satclock.csv`, `truth_rx_bias.csv`, `truth_datum.json`.  Later additions
on the same rule: `include/sim/AmbiguityDraw.hpp` (§15.1).  **Both** CMake source lists
(`CMakeLists.txt:295-305` and `:409-421` — maintained independently and already differing:
`EphemerisProvider.cpp` is in `gnss_simulator` but not in `gnss_simulator_selftest`).

**Producer (trace-only, default-OFF, no grammar change):** `network.trace_sat_bias_states`,
`network.trace_rx_states`.

**Tooling:** `scripts/simnet_grade.py` (the truth grader),
`config/test_suite/_simnet3_*.json` / `_simnet5_*.json`.  ⚠ **CORRECTED 2026-08-15:** this
manifest also listed `scripts/simnet_run.ps1` ("runner with the zombie check").  **It was
never built and no file of that name exists anywhere in the tree.**  The zombie-check /
refuse-if-`gnss_processor*`-lives pattern trap #14 tells the reader to replicate lives in
**`scripts/q3_ladder_run.ps1`**, which does exist.  Later tooling built against this SPEC:
`scripts/simnet_make_roster.py`, `simnet_make_producer_config.py`, `simnet_common_mode.py`,
`simnet_exchange.py`, `simnet_amb_integer.py`, `simnet_inject_arms.py`,
`simnet_inject_grade.py`, `simnet_injection_preflight.py`, `datum_gauge_audit.py`.

**Tests:** a NEW file under `tests/sim/` (target `gnss_simulator_selftest`, **separate**
from `gnss_tests`), new suite name — suite names are one-file-owned and must stay that way.

---

## §10 Verification status

**This is the CODE-ANCHOR verification log for §1–§8, not the run status.**  ⚠️ Its
original text read *"Nothing in this SPEC has been run yet"* — written 2026-08-11, before
the arms.  **That is false and has been corrected here (2026-08-15).  The measured record
runs from §9.5 to §15.8**; §0's status table is the index to it.

Code anchors for the design sections were verified 2026-08-11 against the working tree
(which then carried uncommitted sim work on `EphemerisProvider`, `TrajectoryGenerator`,
`SimConfig`, `test_sim_selfconsistency`).  Four load-bearing claims were independently
re-verified with a second tool (symbol-absence needs two tools): `ambN`'s missing station
term — `main.cpp:1954-1975` (§1.1 F2); the `LogManager` unguarded re-`register_logger` in
the catch handler — `LogManager.cpp:167` (§1); the absence of any σ field in the B-record
legend — `NetworkProductIO.cpp:549` (§4.8; the gap is closed trace-side in §12.4 and the
legend itself is deliberately unchanged); and `kMinStationsPerSat = 2` —
`ApplicationNetworkAssembly.cpp:1494` (§2.1).

---

