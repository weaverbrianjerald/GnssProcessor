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

## §9.5 G-A RESULTS — measured 2026-08-11 (the first absolute grade)

Arms: `config/sim/simnet3_ga.json` / `_simnet5_ga.json` → `synth-network` →
`config/test_suite/_simnet3_ga.json` / `_simnet5_ga.json`, Release
(`gnss_processor_simnet.exe`), 240 epochs = 2 h @ 30 s, GPS-only, INT posture, quiet
(`storm_scale` 0), graded by `scripts/simnet_grade.py` over the settled second half.
Producer: 240/240 epochs emitted, **0 skipped, 0 withheld**, rank gate passed every epoch,
`declared_dimension` = 1 as predicted (§5).

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

**1. The n = 3 network is estimable but not determinable, and nothing internal says so.**
ZWD carries a **+4.31 m systematic** against a 0.12 m truth, dragging the consumer's height
(+3.50 m of the 3.55 m error is UP).  Two more stations remove the bias entirely (+0.029 m)
⇒ a determinability deficiency of the minimum network, **not** a modeling error.

**2. The formal σ collapses while the true error grows.**  Median URA vs median |C − truth|
over the n = 3 arc:

| epoch | 0 | 1 | 5 | 20 | 60 | 120 | 239 |
|---|---|---|---|---|---|---|---|
| median URA | 6.63 m | 2.34 m | 0.343 m | 0.242 m | 0.178 m | 0.122 m | 0.176 m |
| median \|C − truth\| | 1.44 m | 0.77 m | 1.13 m | 1.53 m | 1.95 m | 1.95 m | 3.38 m |
| implied vf | 0.05 | 0.11 | 10.7 | 15.0 | 74.3 | 257 | 371 |

The covariance says *converging*; truth says *diverging*.  **Every internal instrument
called the run clean**: σ̂₀ = 0.0419, zero RAIM flags on all six tiers, zero withholds, rank
gate passing, no discontinuities.  A confidently wrong estimator satisfies every internal
grade this project has ever applied.

**3. Not a correction mismatch, and not the injected biases.**  A bisect arm
(`_simnet3_bare`) with the ENTIRE optional correction stack off on both sides (relativity,
Shapiro, solid tide, pole tide, sat + rx antenna, wind-up) and the receiver code biases
removed reproduces the result to three decimals — C rms 5.5151 vs 5.5171 m, ZWD 4.3055 vs
4.3073 m, vf 87.46 vs 87.53.  One arm, many knobs, but the *direction* is unambiguous: the
residual does not live in the correction stack.

**4. vf remains ≫ 1 even at n = 5** (392 clock, 6.5 ZWD) — the formal σ understates the
true error by ~20× (clock) and ~2.5× (ZWD) in σ.

### What this does NOT establish

- **Not the field cb vf ≈ 10 question yet** — no B channel in the INT posture, so the
  code-bias vf is unmeasured; the W6 arm (§2.2) is required and not yet run.
- **A small-network vf does not extrapolate** — ZWD vf fell 87.5 → 6.5 for n = 3 → 5; the
  field runs 63–305 stations.  Any "the estimator's σ is off by X" must name its n.
- **The P channel is not yet a sharp test** — SD fractional residuals 0.21–0.35 cyc, i.e.
  the ambiguities have not resolved in 2 h at this scale, so §4.3's "exactly integer × λ" is
  untested rather than falsified.
- Ignition is excluded by grading the settled half, but 2 h is the MINIMUM (§2).

## §9.7 The 50-STATION GLOBAL arm — the vf trend and the QC tiers (2026-08-11)

Fixture `config/sim/simnet50_ga.json` (roster from `scripts/simnet_make_roster.py`, 50 solve
+ 1 consumer, Fibonacci lattice, \|lat\| ≤ 72°), 240 epochs = 2 h, GPS-only, quiet.

**Geometry certified before any arm ran** (§2.1): bipartite components **k = 1 at every
epoch**, 50/50 stations present, all **32** GPS satellites visible each epoch, **minimum 12
stations per satellite** against a hardcoded `kMinStationsPerSat` = 2, **zero**
satellite-epochs dropped.  A global roster is where fragmentation would be expected; it does
not occur at this size.

Two arms: **matched** (`_simnet50_matched.json`) — the SAME minimal posture as n = 3 / 5, so
the vf trend is one knob (station count); **armed** (`_simnet50_ga.json`) — the full
standard armed set, so the QC tiers have opportunities.  Their postures are NOT comparable.

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

> ⚠️ **AMENDED 2026-08-11 by adversarial review — the clock row's SHAPE is not readable.**
> The n = 50 clock vf is **78 % of its variance in ONE common direction**.  Settled C
> residual as fleet-common (vs pivot) ⊕ per-satellite: n=3 5.517 = 0.974 ⊕ 5.428, n=5
> 2.183 = 1.342 ⊕ 1.714, n=50 1.486 = **1.315 ⊕ 0.685** m rms.  The **per-satellite error
> DOES converge** with n (5.43 → 0.69); what does not is a pivot-relative common level
> wandering ~1.3 m rms at every n (ties to the CB-Q "wire drifts in unpinned directions"
> finding).  Median row vf at n=50 is **15.5**, not the mean 455 — the mean is a
> three-satellite tail (G31/G28/G03 drifting −0.2 → −2.4 m *together*).  Effective sample
> size for the common direction is ≈ 1, so **"455 vs 392 = a plateau" is realization noise
> and must not be read as a shape.**  vf is still GROWING at 2 h at every n.
>
> ⚠️ **The ZWD "vf 1.63 = calibrated" row needs its asterisk**: truth is a 0.12 m
> near-constant (±0.055 m fleet spread) while `std_m` ≈ 0.79 m, so the channel is honestly
> UNINFORMATIVE — ~30× worse than just predicting the constant.  The 87.5 → 6.5 → 1.63 trend
> is numerator-driven (the error shrinking), not a σ that became trustworthy.

**The two channels behave completely differently, and that is the finding.**  ZWD converges
to CALIBRATED (87.5 → 6.5 → **1.63**): its n = 3 mis-calibration was a determinability
artifact that disappears with scale.  The satellite clock does NOT (768 → 392 → **455**):
accuracy improves (5.52 → 1.49 m rms) but the formal σ shrinks just as fast, so it stays
**~21× too optimistic in σ** at every scale tested.  A channel whose σ is structurally
optimistic *independent of network size* is an estimator property, not a small-network
artifact; the clock channel is such a channel, and whether the code-bias channel is one too
still requires the W6 arm.

### 9.7.2 The QC tiers — 0/7 live at n = 3, 4/7 at n = 50

Armed arm, quiet data (no injected faults), 232/240 epochs emitted, **0 withheld by the rank
gate**, 8 by the A4 σ̂₀ ceiling.  78.5 ms/epoch, 50 stations, PARALLEL/20 threads.

| tier | opp | flag | observed rate | implied α | verdict |
|---|---|---|---|---|---|
| IF-phase slip (T1) | **0** | 0 | — | 1.24e-15 | INERT — needs `cycle_slip.if_phase_td` |
| within-ep trigger (T2) | 226 | 7 | 3.10e-02 | 2.70e-03 | **live, 11× its implied α** |
| post-fit RAIM cell (T3) | 103 347 | 747 | 7.23e-03 | 1.97e-09 | **live, 3.7 MILLION× its implied α** |
| RAIM consensus (T3) | 724 | 0 | 0 | 1.97e-09 | live, silent (correct on a quiet arm) |
| attribution SAT (T4) | 7 135 | 0 | 0 | 1.97e-09 | live, silent (correct) |
| clock-model NIS (T5) | **0** | 0 | — | 1.97e-09 | INERT — needs `network.clock_model` |
| clock-monitor (T6) | **0** | 0 | — | — | INERT — GPS-only degenerates its magnitude test |

Events on the same quiet arm: `T0_mw_detect / mw_wl_slip` **44**, `T2_within_epoch /
we_refloat_arc` 13 + `we_refloat_sat` 9, `T4_attrib / attrib_arc` 13, `SIGMA0 / withhold` 8,
plus 326 topology `row_kinds`.

| # | finding |
|---|---|
| 1 | **Station count is what makes the QC stack live.**  0 of 7 tiers had a single opportunity at n = 3; 4 of 7 do at n = 50.  T2/T3/T4 need cross-station populations a minimum network cannot supply |
| 2 | **The post-fit RAIM cell tier fires 3.7 × 10⁶ times its own implied Gaussian α on clean synthetic data** — the SAME finding as the clock vf, a second way: vf ≈ 455 means σ is ~21× too small, and a residual 21× larger than its σ crosses a k = 6 threshold constantly.  Two instruments agreeing is what makes it a result |
| 3 | **44 false MW slip detections** on an arm with `rate_base = 0` — no slip injected anywhere.  ~1.5e-4 false alarms per station-satellite-epoch |
| 4 | **Three tiers remain inert even at 50 stations**, each for a *named* reason: T1 needs `cycle_slip.if_phase_td`, T5/T6 need `network.clock_model`.  Arming those is the next arm, not a re-run |
| 5 | **`estimate_iono_sh` cannot simply be added.**  It co-estimates the SH-VTEC field JOINTLY WITH THE SATELLITE CODE BIASES — the whole coupled W6 slice — and the realized datum then no longer matches `declared_dimension`: the rank gate withheld **all 240 epochs** and the producer emitted nothing.  **`sh_health_gate` is not gradeable by this fixture** until the W6 slice is armed as a whole |

⚠️ **The armed arm is NOT posture-comparable to the matched one**, for two independent
reasons.  *Posture*: its ZWD vf is **3466** against the matched arm's **1.63** while its ZWD
rms is *better* (0.659 vs 0.896 m) — the armed set makes the estimate more accurate and its σ
three orders of magnitude too small; `phase_leveling` is the prime suspect but the arms
differ by many knobs, so **a bisect is owed before that is stated as a fact.**  *σ-scale*:
the published URA is `s0 · sqrt(scov)` with `s0 = max(1.0, σ̂₀_EWMA)`
(`NetworkSolPrep.cpp:430,443`), and that EWMA is structurally unavailable unless `emit_ura`
+ `raim` + `phase_leveling` are ALL armed (the RTB-6 sentinel):

| arm | RTB-6 sentinel | σ̂₀ EWMA | **s0** | published URA |
|---|---|---|---|---|
| n = 3 / 5 / 50 **matched** | present | unavailable | **1.000** | the pure marginal formal σ |
| n = 50 **armed** | absent | **1.674** | **1.674** | formal σ inflated 1.674× |

⇒ **the whole matched trend (768 → 392 → 455) is on the pure formal σ and is directly
comparable.**  The armed arm's clock vf of 7866 is *deflated* by s0² = 2.80 relative to the
same measure — on the pure formal σ it would be ≈ 2.2 × 10⁴.  The reported figure is
CONSERVATIVE and no conclusion's direction changes, but the two must never be pooled.  Any
future arm that flips `phase_leveling` moves this scale silently.

---

### 9.7.3 QC DETECTION — quiet vs fault, one knob apart

`config/sim/simnet50_qcfault.json`, identical roster and producer posture to the armed quiet
arm; the only difference is the injected faults, each verified present in the emitted RINEX
before grading:

| fault | where | magnitude IN THE DATA | detected? | by what |
|---|---|---|---|---|
| cycle slip, unflagged | S01 / G26 / L1C, ep 60, 19° elev | **+12.26 widelane cycles** (MW from the RINEX) | **MISSED** | nothing |
| cycle slip, unflagged | S02 / G06 / L2W, ep 90, 44° elev | **+6.70 widelane cycles** | partial | `T4_attrib attrib_arc` only — no slip event |
| cycle slip, **LLI-flagged** | S03 / G19 / L1C, ep 160, 22° elev | +45 cyc, LLI = 1 | **YES** | `T0_mw_detect lli_slip` |
| satellite clock step | G14, ep 120, all stations | +10 m | **YES, and MARKED** | `CLK_STEP mark` + `T4_attrib attrib_arc` |

(detection columns from `network_events.csv` within ±10 epochs of each injection.)
Per-tier flag counts, quiet → fault: T1 inert (0 opp both); T2 7 → 7 (Δ 0); **T3 cell
747 → 603 (Δ −144)**; **T3 RAIM consensus 0 → 29 (+29 — responded)**; T4 0 → 0; T5/T6 inert.

| # | finding |
|---|---|
| 1 | **The clock-step marking gap from §9.6 is CLOSED.**  With `emit_clock_disc` + `clock_disc_jump_test` armed the 10 m step produces a `CLK_STEP mark`; §9.6's zero marks were an unarmed marker, exactly as that section said |
| 2 | **RAIM consensus is the tier that works** — 0 → 29 flags, the only tier whose flag count responds to the faults at all.  It needs the cross-station population n = 3/5 cannot supply: the case for the 50-station variant in one line |
| 3 | ⚠️ **`mw_detect` missed both unflagged slips — and they were not subtle.**  The MW widelane jumps are **+12.26** and **+6.70 cycles** from the emitted RINEX and MW noise is sub-cycle, so these are the easiest targets a widelane detector has.  The detector was demonstrably LIVE (44 `mw_wl_slip` events elsewhere on the same data) and the one slip it did catch was caught by the receiver's own **LLI flag**, not the MW statistic.  On this arm: **44 false alarms, 0 of 2 true detections, and the only true catch came from a flag the receiver supplied.**  Chased in §9.7.4–§9.7.5 |
| 4 | **T4 attribution never flags.**  7135 opportunities, 0 flags in both arms, including for a 10 m satellite clock step seen by all 50 stations — the exact fault class it exists to attribute.  It emits `attrib_arc` events but no flags.  (⚠️ CORRECTED by §9.7.10: an INSTRUMENT gap, not a dead tier) |
| 5 | **The post-fit RAIM cell tier's flag count goes DOWN with faults added** (747 → 603): with a false-alarm population that large a real fault just reshuffles which cells exceed — consistent with finding 2 of §9.7.2 |

### 9.7.4 CHASING THE mw_detect MISS — one defect PROVEN, one still open

Three hypotheses, each a one-knob A/B on the same fixture data:

| # | hypothesis | test | result |
|---|---|---|---|
| 1 | the rounding-cleanliness gate is too tight (`turbo_edit_frac_max` = 0.15, default) | 0.15 → **0.49** | **FALSIFIED** — 44 → 66 events, still only at epochs **2 and 4**; nothing at 60/90/160 |
| 2 | no BIA ⇒ no BSSD integer WL lock ⇒ `slip_wl_known` never true (`has_bia ? "BSSD-pass2" : "direct-no-BIA"`, MwSlipDetector.cpp:2696) | `enable_code_phase_bias` false → **true** | **FALSIFIED** — bit-for-bit the same 44 events at epoch 2, same misses |
| 3 | **station index 0 is structurally excluded from slip detection** | roster reordered | **PROVEN** — below |

#### The proven defect: the datum station gets NO slip detection at all

```cpp
const int i_lo = phase_datum_prioritized ? 0 : 1;   // ApplicationNetworkAssembly.cpp:1281
```

The knob is **`network.phase_datum_prioritized`**, default **false**
(`GnssConfigLoadNetwork.cpp:4240`) — a knob in its own right, NOT `phase_datum_eq34` (an
earlier draft misattributed it; the `i_lo` proof is unaffected, the named remedy was wrong).
With it false `i_lo = 1`, and every slip-detection loop in `NetworkSlipQc.cpp` (lines 69,
169, 385 — MW **and** LLI) runs `for (int i = i_lo; ...)`, so **`stations[0]` is skipped
entirely.**  *Controlled proof*, identical fixture data, ONLY roster order changed: with
`S03000SIM` at index 2 its G19 LLI slip is **detected** (`T0_mw_detect / lli_slip`, 1 event
in the run); moved to **index 0** it is **MISSED** (**0** events).

The in-code rationale — "Pivot (stations[0]) is never touched (its ambiguities are
gauge-pinned)" — is sound as far as it goes, but the consequence is not benign: the datum
station is the receiver whose ambiguity rides inside every emitted phase bias (contract
§B.2), so an undetected slip there re-references the P record for that satellite **across
the whole network**, and nothing in the run records it — not the flag-rate summary, not the
event log, not a counter.  Standing invariant #1 in its exact form, at the one station where
it matters most.  Fixed in §9.7.10 (`cycle_slip.datum_station_detect`).

#### Still open: MW produces no mid-arc candidates at ANY station

`i_lo` explains S01 (index 0) completely.  It does **not** explain S02 (index 1, −7 cyc
unflagged, 44° elevation), which sat inside the loop and was still missed by MW; only
`T4_attrib / attrib_arc` fired.  Across both fault arms every one of the 44 `mw_wl_slip`
events is at **epoch 2**, and the frac_max arm adds epoch 4 — the MW path emits candidates
only during ignition, then nothing for 238 epochs.  Hypotheses 1 and 2 are eliminated; the
remaining candidates are the arc-maturity / Welford noise-floor state and the BSSD MW
pivot's own resolution (`slip_wl_known` is documented false "if no pivot could be made — MW
pivot slipped with an unresolved integer").  Discriminating instrument:
`network.trace_slip_raw`, run, answered in §9.7.5.

### 9.7.5 RESOLVED — the detector SAW the jump and formed no candidate

`network.trace_slip_raw` armed (`network_slip_raw.csv` publishes per-(station,sat) raw MW /
GF / phase-minus-code / LLI every epoch from `raw_observed`, **independently of whether any
detector is enabled**).  At the injected epochs, against each arc's own robust
epoch-to-epoch scatter (1.4826·MAD):

| station / sat | MW jump | cycles | **vs arc scatter** | LLI | detected |
|---|---|---|---|---|---|
| **S02 / G06** (idx 1) | +5.760 m | **+6.68** | **13.3×** | 0/0 | **MISSED** |
| S03 / G19 (idx 2) | +38.821 m | +45.04 | 51.9× | **1**/0 | caught — by **LLI only** |
| S01 / G26 (idx 0) | +10.421 m | +12.09 | 7.7× | 0/0 | MISSED (`i_lo` blind spot) |

**This closes §9.7.4's open question.**  The jump is in the producer's own trace, at the
exact epoch, at 13× the arc's own scatter, with a clean LLI — and the detector emitted no
candidate.  The failure is in candidate FORMATION, not the observable.  With the two
falsified hypotheses (frac gate, BIA), the remaining surface is the BSSD/integer-lock path.

### 9.7.6 The MW NULL SPACE — measured, and RAIM does not currently cover it

The MW widelane is N₁ − N₂, so a slip with **ΔN₁ = ΔN₂ changes it by exactly zero** —
structurally invisible to any MW-only detector at any threshold.  A paired arm
(`config/sim/simnet50_mwblind.json`) puts a (+2,+2) and a (+2,0) slip on the **same
satellite, same epoch, same magnitude**, so the only difference is whether the pair lies in
MW's null space.  Verified in the emitted RINEX:

| slip | ΔWL measured | ΔWL predicted | ΔGF measured | ΔGF predicted |
|---|---|---|---|---|
| (+2,+2) | **−0.019 cyc** | **0 exactly** | −0.142 m | n(λ₁−λ₂) = −0.108 m |
| (+2, 0) | +2.199 cyc | +2 | +0.397 m | nλ₁ = +0.381 m |

The pair IS observable — ΔGF = −10.8 cm, and the iono-free phase moves
n·c/(f₁+f₂) = **+21.4 cm** — just not by MW: the argument for a geometry-based backstop
rather than a better MW gate.  **But the backstop does not currently fire.**  QC response to
the pair vs the quiet baseline: T1 0 (0 opp) both; T2 7 → 7; **T3 cell 747 → 743** (−4,
noise); **RAIM consensus 0 → 0**; T4 0 → 0; T5/T6 0 opp both.  **A 2-cycle slip pair — 38 cm
on L1, 21.4 cm in the iono-free phase — passes through all seven tiers untouched.**  Neither
the MW-blind (+2,+2) nor the MW-visible (+2,0) control produced a slip event; the visible one
raised only a `T4_attrib / attrib_arc`.  RAIM consensus, which DID respond to the 10 m clock
step (§9.7.3, 0 → 29), does not respond at this magnitude.  So "MW has a null space,
therefore we need RAIM" is a **requirement**, not a description of the built system.

⚠️ **CORRECTED by §9.7.10 correction 3: this was an UNARMED-TIER reading.**  T1
(`network.cycle_slip.if_phase_td`) was off in every §9.7 arm.  Armed, it catches both G11
slips at their exact injection epochs with zero false alarms.  §15.3's `--signal-mode
mw-blind` generator makes this discriminator reproducible on demand.

### 9.7.7 The remedy is real but NOT free — do not make it standard yet

`network.phase_datum_prioritized: true` sets `i_lo = 0`.  One-knob arm off the fault fixture:

| | default (`i_lo`=1) | **armed** (`i_lo`=0) |
|---|---|---|
| station-0 T0 events | **0** | **5** |
| run `mw_wl_slip` total | 44 | 49 |
| S01/G26 slip | MISSED | **still MISSED** |
| **epochs emitted** | **232 / 240** | **143 / 240** |
| withheld by A4 σ̂₀ ceiling | 8 | **97** |

Two conclusions pointing opposite ways: it **confirms the `i_lo` mechanism a second,
independent way** (station 0 goes 0 → 5 T0 events on this knob alone), and it **costs 97 of
240 epochs** (40 % of the product) to the A4 σ̂₀ ceiling against 8 at default without
recovering the S01 slip — the §9.7.5 candidate-formation failure applies at station 0 too,
so **`i_lo` was necessary, not sufficient.**

⇒ **Arming it as a standing default is NOT justified by this evidence.**  SCOPED ledger,
validated-but-not-promoted, cost recorded (loader defaults stay off; each new config arms its
set explicitly).  The blind spot is fixed instead by §9.7.10's
`cycle_slip.datum_station_detect`, at **zero availability cost**.

### 9.7.8 ⚠️ CORRECTION — RAIM was NOT blind.  The consensus rule vetoed it.

**§9.7.3 and §9.7.6 graded against the wrong instrument.**  `network_events.csv` does not
record RAIM refloats, so "MISSED" there meant "no event row".  `network_raim.csv`
(`network.trace_slip_events`) carries the actual decision chain and says the opposite:

| slip | `s_max_norm` | `raim_k` | `spiked` | `n_sta_spiked` | `min_sta` | `refloated` |
|---|---|---|---|---|---|---|
| S02/G06, −7 cyc | **58.2** | 6 | **1** | **3** | 3 | **1 — acted** |
| S03/G11, +2 cyc | **9.4** | 6 | **1** | **1** | 3 | **0 — vetoed** |

RAIM **detected both, comfortably.**  The failure is in the ACTION rule:

```cpp
if (kv.second >= raim_min_sta) flagged.push_back(kv.first);   // NetworkRaim.cpp:266
```

The count is keyed by **satellite**, counting how many *stations* spiked on it, because the
tier attributes **satellite** faults.  **A cycle slip is a receiver event, so exactly one
station can ever spike** — `n_sta_spiked = 1 < min_sta = 3`, at any magnitude.  MW-blind arm
run totals: **743 spiked, 0 refloated**.  The −7 cyc slip got through only by reaching
`n_sta_spiked` = 3 = exactly the minimum, the slip having perturbed the SHARED satellite
parameters enough to push two other stations' G06 residuals over k — contamination
masquerading as consensus.

**`raim_min_stations` is not a knob to lower** — it exists to stop the tier attributing every
station's noise to a satellite.  The fix is a separate station-local attribution path (same
statistic, different consensus logic).  `raim_attribution` (T4) reporting **7135
opportunities and 0 flags in every arm** is suspicious in the same direction — checked in
§9.7.10, which corrects both the "100 % detection / 0 % action" framing and the T4 reading.

### 9.7.9 HOW MUCH DO SLIPS CONTAMINATE?  Measured — and they are ORTHOGONAL to the level problem

One knob off the quiet 50-station arm: `cycle_slips.rate_base` 0 → 0.0002 (the sim default),
identical fixture, identical per-station seeds.  **465 slips across all 51 stations, |Δ| 13 …
52 (median) … 123 cycles.**  At realistic magnitudes the QC stack DOES act — RAIM 2707 spiked
/ **2751 refloated** (vs 0 at 2 cycles), consensus flags 0 → 191, `mw_wl_slip` 44 → 121,
`lli_slip` 0 → 132: large slips hit many stations at once, so the consensus rule is
satisfied.  **The 2-cycle case is below the effective ACTION threshold, not below the
detection threshold.**

| channel, settled half | 0 slips | **465 slips** | ratio |
|---|---|---|---|
| C rms | 1.102 m | **4.630 m** | **4.2×** |
| C median \|·\| | 0.804 m | 0.996 m | 1.24× |
| C max \|·\| | 3.569 m | **19.452 m** | 5.4× |
| ZWD rms | 0.659 m | 2.158 m | 3.3× |
| consumer 3D rms | 1.417 m | 1.719 m | 1.2× |

Median barely moves while rms goes 4.2× and max 5.4× ⇒ slips act through a heavy **TAIL**,
not a uniform shift.  Also `CLK_STEP mark` **0 → 17** — station-local slips marked on the
wire as SATELLITE CLOCK discontinuities — and `T2_within_epoch / emitted_contaminated`
**0 → 11**.

#### The decomposition — and the negative result that matters

Settled C residual as fleet-common (vs pivot) ⊕ per-satellite: 0 slips 1.102 = **0.920** ⊕
0.608 m; 465 slips 4.630 = **0.466** ⊕ **4.606** m.  **Slips inflate the PER-SATELLITE error
7.6× (0.61 → 4.61 m) and leave the COMMON direction untouched — it does not grow, it shrinks
slightly.**  So the two problems are ORTHOGONAL: slip contamination is real, first-order on
the per-satellite channel, and a fixable defect (the §9.7.8 action rule); the pivot-relative
common-mode level wander (~0.9–1.3 m rms at every station count) is **NOT slip
contamination** — identical with 0 and with 465 slips.  That is the channel the field
campaign has been chasing, and this arm rules slips out as its cause.  Fixing the slip
response would clean up the per-satellite tail and the 17 spurious clock marks; it would not
move the level.

### 9.7.10 The §9.7.8 trail CLOSED — corrections, fixes LANDED, and their measurements (2026-08-11)

Follow-up session working from the retained `runs/simnet50_mwblind2/run/logs` decision
traces.

#### Corrections to §9.7.3 / §9.7.6 / §9.7.8

| # | correction |
|---|---|
| 1 | **T4's 7135 opp / 0 flag is an INSTRUMENT gap, not a dead tier.**  The flag-rate counter (`fr_attrib`, NetworkRaim.cpp) counts only the whole-SAT criterion, deliberately conjoined with `σ̂0 > raim_sigma0_trigger` (the 305-24 h lock-in guard: at a robust-clean epoch a high column effect is a median-polish artifact, and wholesale refloats destabilize).  A single-satellite fault cannot move a network-wide robust MEDIAN (one sat is ~1/31 of the row pool), so the SAT flag is **structurally unreachable at a clean epoch — for exactly the fault class the tier is named after.**  It CAN fire in globally-contaminated postures: the `phase_datum_prioritized` arm (16 flags amid 37 within-epoch triggers) proves the counter wiring.  Meanwhile the tier's ARC sub-path (isolated-arc interaction, gated at any σ̂0) **acted 14 times on the mwblind2 arm — including the real (+2,0) slip at S03/G11 at its exact injection epoch** (interaction 12.88 = 7.2×σ̂0, `attrib_arc` event, refloat applied), and none of those actions appeared in any flag-rate line |
| 2 | **§9.7.8's "100 % detection / 0 % action" over-claimed both halves.**  `network_raim.csv`'s `refloated` records the CONSENSUS action only (§0 error class, instance 4); and most of the 743 "detections" are the σ-miscalibration false-alarm population of §9.7.2 — on a clean arm, NOT acting on them is correct.  Measured true-event record: 2 injected slips → 1 with any post-fit signature → 1 acted (same epoch, arc path).  The consensus rule's receiver-event veto stands as designed; the station-local action path it calls for **already exists and works** — it is the T4 arc path |
| 3 | **The MW-blind (+2,+2) pair produced NO post-fit signature at all** — no cell spike at any epoch (its GF signature is absorbed by the free per-LOS iono, the remainder redistributed below the cell threshold), so no action rule could have caught it post-fit.  §9.7.6's "the geometry-based backstop does not fire" was an UNARMED-TIER reading: T1 (`network.cycle_slip.if_phase_td`) was off in every §9.7 arm (0 opportunities in every flag-rate table).  Armed (`_simnet50_mwblind_ifp`, one knob): **both G11 slips caught at the exact injection epoch** — S02 ΔΔφ_IF 0.205 m > thr 0.135 (the MW-null pair; prediction 2·λ_IF(1,1) = 0.214 m) and S03 0.989 m > thr 0.257 — with **2 flags in 109 137 samples = zero false alarms.**  The MW null space is covered by the tier that already existed; the standing 30/60 s doctrine (relaxed MW + LLI/IF-TD) simply was not applied to the sim posture.  (The loader's posture auditor already nags: "POSTURE BUNDLE 'zero-q phase memory + slip response': PARTIAL 1/2 — MISSING: network.cycle_slip.if_phase_td") |

#### Fix 1 — T4a flag-rate line (landed)

`fr_attrib_arc` counts the ARC sub-path's criterion (`interaction/σ̂0 > raim_k`, pre
persistence/maturity — mirroring how the SAT line mirrors `bad_sats`), emitted as
`T4a,attribution ARC (T4)` in the summary and CSV.  On the mwblind2 rerun: **103 347 opp / 14
flag — matching the 14 `attrib_arc` actions in the retained events one-for-one.**  The
summary now separates "SAT criterion structurally quiet" from "ARC sub-path live and acting".

#### Fix 2 — `network.cycle_slip.datum_station_detect` (landed, default OFF)

The §9.7.4 blind spot, closed with the mechanism-specific response instead of the §9.7.7
knob.  Setup builds the pivot's MW detector under the toggle; a serial PASS-A block rolls it
(`ApplicationNetworkAssembly`); the MW drain routes station-0 MW candidates and
(Card-L-qualified) LLI assertions to a two-edge network re-reference (invariant #7).  **Queue
edge** at detection: `datum_slip` event + the sat queued for a HARD skip-stash of its shared
PhaseBias carry at this epoch's stash (before the pin-bridge harvest, both stash branches).
**Apply edge** at the first drain AFTER that skip-stash consumed (per-sat guard, so a
withheld epoch cannot decouple the edges): `datum_reref` event, pending-reseed of the sat's
ambiguities at EVERY non-pivot station (present or absent — an absent station's mark waits
for its own re-coupling edge), local-carry erase, Card-4 C3c feed, per-sat disc mark via the
Edge-1 mark-and-serialize queue (whose SolPrep drain condition gains the third producer — its
first arm queued marks into a run with `ura_retro` and `mark_refsat` both off and the mark
silently never landed, the exact leak the drain comment predicts), and run-end counters
(`datum_slip_detections` / `datum_slip_rerefs`, invariant #4 line in the teardown).  Loader:
requires `mw_detect` (parsed after the invariant-#1 auto-arm so leveling counts); inert under
any no-pivot datum.  Unit suite: `DatumStationSlipDetectConfig` (4 cases, incl. the auto-arm
ordering).

**Validation, OFF (byte-identity).**  The mwblind2 arm rerun with the new build, toggles off:
`products.txt` and every decision trace (`network_raim.csv`, `network_events.csv`,
`network_raim_attrib.csv`, `network_slip_raw.csv`, `network_epoch_status.csv`)
**bit-identical to the retained run**; `network_flag_rates.csv` differs by exactly the added
T4a row.

**Validation, ON (`_simnet50_pivotswap_datum` vs `_simnet50_pivotswap_b`, one knob, identical
qcfault fixture data, settled half).**  The index-0 G19 LLI slip — MISSED in §9.7.4 — is now
caught end-to-end: `datum_slip` at the LLI epoch, `datum_reref` one epoch later (28
ambiguities network-wide), `R,G19` disc mark on the wire, counters 2/2.

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

| edge | triage |
|---|---|
| one ignition false positive — the pivot's MW detector emitted a G12 candidate at epoch 2 (the §9.7.4 ignition-candidate class) → one spurious re-reference, marked honestly (`R,G12`) | **field-inert** (ignition is pre-convergence); a maturity gate on the datum path is a cheap follow-up once §9.7.5's mid-arc candidate-formation defect — which bounds MW's usefulness at the pivot exactly as everywhere else — is chased.  LLI is the demonstrated live catcher for the pivot |
| the pivot remains OUTSIDE post-fit RAIM cells and has NO IF-TD detector (setup gate) | coverage is MW + LLI + this response.  A pivot slip in MW's null space with no LLI stays uncovered; extending IF-TD to the pivot is the named follow-up |
| warm resume drops pending re-reference queues | same transient class as `refsat_mark_sats`; the slip then surfaces to post-fit RAIM at non-pivot stations |
| §9.7.9 orthogonality stands | this cleans the per-satellite tail and the wire hygiene.  **It does not move the pivot-relative common-mode LEVEL** — the datum-wander channel remains the separate (CB-Q) campaign |

#### What this does and does not license

- The `i_lo` finding is **proven** (controlled single-variable A/B) and actionable: extend
  detection to station 0 in the legacy datum posture, or document the blind spot and make the
  eq34 posture mandatory where slips matter.
- The **MW mid-arc silence is measured but unexplained** — one fixture, one data rate (30 s),
  one elevation range.  A lead, not a verdict on the detector.  OPEN.
- Caveat from §9.7.3: the sim's slips here are injected on ONE band (the classic widelane
  target); a two-band slip is a different test — reproducible via §15.3's `--signal-mode
  mw-blind`.

---

## §9.8 The DATUM-WANDER arm — the common level FOLLOWS THE DATUM STATION, and both remedies measured (2026-08-11)

The §9.7.1/§9.7.9 channel: a pivot-relative fleet-common C level that does not converge with
n and is slip-independent.  On the QUIET fixture `truth_C ≡ 0`, so the fleet-common of the
emitted C records IS the wander, exactly.  Tool: `scripts/simnet_common_mode.py`
(median-over-sats series; settled, demeaned).

**Design.**  Same quiet 50-sta data, same armed posture, ONE key varied:
`network.clock_datum_station` — which the setup makes `stations[0]`, so the clock datum AND
the phase pivot AND the sync reference move together (not separable in the legacy family).
S01 is near-polar (lat −68.6°, the §2.4 "sees everything at low elevation" class); S26 is
equatorial (+1.1°).  Then `network.clock_datum_sum_to_zero` — the CLAUDE.md amendment-5
"needs its first validation run" candidate — on each.  Pre-registered: P1 (wander owned by
the datum station) ⇒ series decorrelate under datum change; P2 (shared-state) ⇒ corr ≈ +1.
Arms `_simnet50_ga_cd{A,B,C,D}.json`.

| arm | pivot / clock datum | C resid rms | C contrast rms (datum-free) | common wander std | consumer 3D rms |
|---|---|---|---|---|---|
| cdA | S01 polar, pivot-sat emit | 1.102 m | 0.966 m | **0.193 m** | 1.417 m |
| cdB | S26 equatorial, pivot-sat emit | 0.384 m | **0.443 m** | 0.024 m | 0.315 m |
| cdC | S01 + sum-to-zero | 0.636 m | 0.991 m | 0.036 m | 1.589 m |
| cdD | **S26 + sum-to-zero** | **0.286 m** | **0.434 m** | **0.014 m** | **0.208 m** |

### Findings

| # | finding |
|---|---|
| 1 | **P1 in substance: the wander follows the datum station, and its amplitude is set by that station's geometry** — 0.193 m (polar) vs 0.024 m (equatorial), 8×, on identical data; first-difference corr(A,B) = +0.15 ≈ 0 (increments unrelated).  The level-corr (−0.885) is two smooth 1 h curves trending oppositely — per the §9.7.1 amendment, effective n ≈ few for slow series; do not read that shape |
| 2 | **The §9.7.9 "0.920 m common" figure decomposes as ~0.82 m CONSTANT lump** (d_IF/anchor-class offset, reproducible, not a stability problem) **⊕ ~0.19 m true WANDER**: √(0.824² + 0.193²) = 0.846 ≈ the 0.920 measured there |
| 3 | **The datum-free contrast ALSO moved (0.966 → 0.443, 2.2×)** — the pivot is not just a gauge: its phase pins gauge the PB chain through its own (noisy, low-elevation) data, its QC exposure differs, and the estimable per-sat content degrades with pivot quality.  **Pivot election is a PRODUCT-QUALITY decision, not a labeling decision** |
| 4 | **`clock_datum_sum_to_zero` first validation: PASS, doing exactly what it claims.**  Wire mean-over-sats ≡ 0.0000 exactly every epoch (constraint holds bit-for-bit); wander 0.193 → 0.036 at fixed (bad) pivot; availability unchanged (232/240 emitted, 8 A4 withholds, same as cdA); rank gate passes (`declared_dimension` 1 → 0, self-consistently — §5's caveat applies).  The loader AUTO-ARMS `clock_disc_on_set_change` (a clock-satellite set change re-references the Σ=0 gauge and marks the disc counters — invariant #7 already wired).  My pre-registered "< 0.02 m" missed: with Σ=0 enforced the MEDIAN wander (0.036) measures distribution asymmetry, not the level; the MEAN is pinned identically |
| 5 | **The consumer feels the PIVOT, not the wander**: consumer 3D rms ranks with the contrast column (0.21/0.32 m for good-pivot arms vs 1.42/1.59 m for polar-pivot arms — a 6.8× end-to-end swing from datum election alone), and sum-to-zero alone (cdC) does NOT help the consumer — a PPP consumer estimates its own clock, so the common level was never its problem.  The wander matters to ABSOLUTE-clock consumers (timing, inter-provider comparison) and to every internal vf/URA channel |
| 6 | **The §9.7.1 vf plateau needs an asterisk**: clock vf 7866 (cdA) vs 741 (cdB) — the σ-optimism largely followed the pivot artifact too.  The matched-trend arms (n = 3/5/50) all used the first-roster-station datum, so "vf ~455 plateau at every n" conflates estimator σ-optimism with the datum-election artifact.  Owed: a re-read of that trend against cdB-class arms before quoting the plateau as an estimator property |

### What this does and does not say about the FIELD wander

The FT63 lineage's `clock_datum_station` is MBAR00UGA (equatorial — the field already has
good datum geometry by luck or design).  What transfers is the MECHANISM: under the legacy
emit, the datum receiver's own clock-estimation error goes 1:1 onto the wire's common level.
On real data that error budget is real multipath + environment + hardware, not the sim's
clean noise — and the field lineage estimates CB jointly (W6 posture), so the CB-Q
"metres/pass in unpinned inter-channel dirs" is this channel COUPLED into the bias states,
not this channel alone.  The sim numbers bound the geometry term only.

**Remedy stack, validated here:** (a) elect a good-geometry pivot (helps EVERYTHING
including the consumer, 2.2× on per-sat content); (b) arm `clock_datum_sum_to_zero` for the
common level (5×, exact mean pin; costs a set-change re-reference class, auto-marked).  Both
are datum toggles — INSIDE the warm-resume fingerprint, fresh convergence only.  Caveats: one
fixture, 1 h settled window, GPS-only INT posture (no CB states), float consumer.

### 9.8.1 The 25° mask arms — the mechanism CONFIRMED, the remedy REJECTED

Two more arms (`_simnet50_ga_cd{E,F}.json`): the SAME two pivots with
`processing.elevation_mask_deg` 10 → 25 (producer-side mask on the same data).

| arm | pivot | mask | contrast rms | wander std | consumer 3D rms |
|---|---|---|---|---|---|
| cdA | polar | 10° | 0.966 m | 0.193 m | 1.417 m |
| **cdE** | polar | **25°** | **0.403 m** | 0.067 m | **4.839 m** |
| cdB | equatorial | 10° | 0.443 m | 0.024 m | 0.315 m |
| **cdF** | equatorial | **25°** | 0.447 m | 0.078 m | **4.968 m** |

| reading | evidence |
|---|---|
| **Mechanism confirmed** | masking the POLAR pivot's low-elevation data recovers the datum-free contrast to the equatorial level (0.966 → 0.403 ≈ 0.443), while the same mask on the equatorial pivot moves it not at all (0.443 → 0.447).  The penalty is specifically THE PIVOT'S low-elevation data gauging the network — not low-elevation data in general |
| **Remedy REJECTED** | the global mask costs a ~+4.8–5.0 m SYSTEMATIC consumer position error in BOTH arms (p50 ≈ 5.1 m — a bias, not noise: the tropo/height separation lives in the low-elevation geometry the mask removed), and the wander worsens on the good pivot (0.024 → 0.078, less averaging).  A 25° mask amputates the observability to hide the artifact |
| the surgical version — scope the GAUGE, not the data | elect a good pivot (validated, free), or a future pivot-PIN elevation floor (pin only the pivot's high-elevation ambiguities; every observation stays in the solve).  The sub-network/common-pivot intuition reduces to exactly this: the gauge should ride on high-quality observations; partitioning the SOLVE would add tie-seam datum churn (invariant #7's edge class) for no observability gain over per-satellite anchor election (`phase_datum_prioritized` / eq34), which already exists |
| pivot cycle slips are NOT a factor in any cd arm | the quiet fixture injects zero slips (`rate_base` 0, no scripted slips) — the 2.2×/8× effects are slip-free by construction.  (Field-side pivot-slip exposure is §9.7.10's separate, now-closable channel) |

## §9.9 The FIELD probe — the wire wander is NOT receiver-code-driven (2026-08-11)

`scripts/field_wire_datum_probe.py` on the retained CB-Q 12 h control run
(`runs/cbq_12h_ctl`: 61 stations, datum MBAR00UGA, the FT63 lineage, W6+SH posture).  Method:
wire common level w(t) = per-epoch median of the emitted C records (increments per-satellite
→ median; pairs across the run's single `datum_changed` epoch dropped); per-station iono-null
multipath proxy from `network_slip_raw.csv` (`l1c1 + 2/(γ−1)·gf` — increments track
−Δmp_code; LLI rows excluded); corr(dw, dmp_i) per station over the settled 10 h.  TOW keys
quantized to the second (the E rows carry float dust: 302370.00000190735 vs the trace's
%.3f — exact-key joins silently die).

**Result: the §9.8 receiver-side driver is ABSENT on the field.**  Field wire wander
**1.735 m** demeaned std over the longest settled segment (875 epochs) — the CB-Q "metres per
pass" scale, quantified.  corr(dw, dmp): **MBAR ranks 48 of 61** (r = +0.011); controls
median |r| = 0.018, max 0.053 — no station's code observables track the wire, the whole table
is at the noise floor.  Tropo at MBAR is excludable by magnitude (1.7 m of clock common would
need ~0.6–1 m of ZWD error), though the mp proxy is structurally blind to it (tropo cancels
in L1−P1).

**Interpretation.**  The sim's geometry-driven datum-receiver term exists but is SMALL on the
field (MBAR is equatorial; the sim bound for a good datum is ~0.02–0.03 m, two orders below
the 1.735 m observed).  The field wander is dominated by the SHARED/product common
directions — the CB-Q "unpinned inter-channel drift" (clock ↔ CB ↔ iono exchanges, plus any
a-priori product common drift, which enters w(t) directly as −(apriori common) and which no
station observable can track).  This probe eliminates the receiver-local hypothesis and
returns the hunt to the LEVEL information channel (`SPEC_level_information_budget`, the CB-Q
card).  **Caveat**: the increment probe covers the FAST channel (30 s); a driver acting only
at hours-scale with negligible 30 s increments would evade it — the level-domain candidates
need a level-domain instrument (the CB-Q structure-function attack, not this one).

### 9.9.1 The a-priori CLK common-drift probe — the PRODUCT FRAME EXONERATED

`scripts/field_apriori_clk_probe.py`: A(t) = c·median_s dt_apriori_s(t) from the cached COD
30 s CLK, against w(t) on the joint 874 settled datum-stable epochs (7.3 h).  The C records
are corrections about this product, so a product fleet-common drift enters w(t) at slope −1
and no station observable can track it — the exact §9.9 signature.

- A(t) is a nearly pure LINEAR timescale drift: −4.089 m/h, residual std **0.094 m** after
  detrending (range 29.8 m raw).
- The wire does NOT inherit it: w's own trend is −0.548 m/h (slope-−1 inheritance would read
  +4.089), detrended corr(w,A) = −0.19 at slope **−0.014**, increment corr +0.04.  The
  detrended product common (9 cm) could at most explain ~7 % of the wire's 1.298 m detrended
  wander even at slope −1; measured slope says it explains ~none.
- Linear timescale offsets are frame-conventional (a datum-invariant consumer never sees
  them).

**With §9.9 this corners the field wander: slips (§9.7.9), every station's receiver-code
errors (§9.9), datum-station geometry (§9.8, bounded 2–3 cm at an equatorial datum), tropo
(magnitude), and the a-priori frame (here) are all eliminated by measurement.  The 1.3 m
detrended wander is ESTIMATOR-INTERNAL — the clock ↔ CB ↔ iono common-direction exchanges,
i.e. the LEVEL information channel.**

## §9.10 Cross-station IONO-FIELD agreement — coherent where mapped, BROKEN at low–high (2026-08-11)

`scripts/field_iono_pair_agreement.py`, raw observables only (an INPUT property, not a fit
property): per station-pair-satellite, 300 s GF increments mapped to vertical by thin-shell
obliquity (h = 450 km), LLI-clean, settled 10 h; 65 pairs in [300, 2000] km, 1406 (pair, sat)
series.

| baseline | elev class | series | med corr | med rel-rms |
|---|---|---|---|---|
| 300–700 km | both-high | 349 | **0.981** | **0.173** |
| 300–700 km | mid | 426 | 0.947 | 0.260 |
| 1200–2000 km | both-high | 284 | 0.957 | 0.397 |
| 1200–2000 km | mid | 344 | 0.918 | 0.451 |
| 1200–2000 km | **low–high** | 3 | **0.381** | **1.130** |

(rel-rms: 0 = identical fields, √2 ≈ 1.41 = uncorrelated.)

- **The co-estimated field's INPUT is genuinely coherent** at the scales the SH model claims:
  two stations' vertical iono rates agree to ~17 % at 300–700 km and ~40 % at 1200–2000 km
  when both view high — this coherence is what lets the network separate iono from clock.
- **The "same field" assumption BREAKS in the low–high configuration** (one station
  low-elevation, one high, ~1000+ km apart — the classic thin-shell + horizontal-gradient
  failure regime): agreement collapses to near-uncorrelated.  Low-elevation GF content
  disagrees with the field the rest of the network sees MORE than the signal itself —
  whatever weight the estimator gives it is absorbed by the per-LOS/SH iono states and,
  through the coupled solve, by the clock/CB commons.
- **Not yet shown**: that this channel DRIVES w(t).  Attribution needs a causal arm (e.g.
  `ionosphere_weighting.elevation_cutoff_deg` / iono-side elevation policy A/B graded on the
  wire common — NOT a global observation mask, which §9.8.1 already rejected).  The low–high
  series count is 3 (long joint low-elevation arcs are rare); widen the window or pool days
  before quoting 0.381 as a precise number.

---

## §9.6 G-B — satellite-clock step, FIRST CLASS ONLY (measured 2026-08-11)

> ⚠️ **RETRACTED 2026-08-11 by adversarial review — do NOT quote the table below.**  The
> "recovery floor between 0.5 and 2.0 m" is a **WINDOW ARTIFACT**.  Pre-step, the G-B arms
> are bit-identical to the null arm (max |ΔC| = 0.000000 over 118 epochs), and the **null arm
> itself drifts −0.464 m between the pre- and post-windows** — so differencing pre-vs-post
> inside one arm charges that drift to the injected step.  Graded DIFFERENTIALLY against the
> null arm's own post-window (the seeds make this exact), recovery is **92.8 / 92.8 / 92.4 %**
> at 0.5 / 2 / 10 m: a uniform ~7.5 % linear absorption, **no floor, and 0.5 m is NOT
> "undetected"**.  G-B must be graded differentially against the null arm.  Related debt:
> §3.1's promised "re-worded 1 mm gate" was never built — the SPP is fed unperturbed rows, so
> nothing asserts the injection reached the observables.  (§9.7.5 now does that job for the
> slip classes, via `trace_slip_raw`.)

Bench demonstration, not the full matrix.  Arms `config/sim/simnet5_gb{05,2,10}.json` — 5
solve stations, one step on **G14** (40–44° elevation, well observed, so the result is not
confounded with the low-elevation noise that dominates G02/G13) at epoch 120 of 240.
Recovered magnitude = median C(G14) over epochs 125–150 minus median over 95–118 (C is
already the estimable contrast; the pivot is pinned to 0).

| injected | pre-step | post-step | **recovered** | error | latency |
|---|---|---|---|---|---|
| 0.5 m | −0.347 | −0.361 | **−0.014 m** | −0.514 | **not detected** |
| 2.0 m | −0.347 | +1.030 | **+1.377 m** | −0.623 | 0 epochs |
| 10.0 m | −0.347 | +8.415 | **+8.762 m** | −1.238 | 0 epochs |

**Empirical recovery floor is between 0.5 m and 2.0 m** at this configuration — a 0.5 m step
is entirely absorbed (recovered 3 % of it), while 2 m and 10 m are tracked within one epoch.
Recovery is **biased LOW by 12–31 %**, the balance going into the other estimable channels.
Latency 0 is expected rather than impressive: `sat_clock_q = 0.01` gives √(q·Δt) = 0.55 m of
process noise per 30 s epoch, so the clock state is nearly free and simply re-estimates —
the posture the Q3 card exists to change, and it means **this arm measures the absorption
floor, not a guard.**

⚠️ **Marking is NOT TESTED by these arms.**  Zero RAIM flags and zero R records at every
magnitude including 10 m — but `emit_clock_disc` and `clock_disc_jump_test`
were **not armed**, so the marker channel was never populated.  Per invariant #4 that is an
inert instrument, not a failing guard.  Arming them is the first item of the real G-B matrix.

**Still owed for G-B — ⚠ SUPERSEDED 2026-08-15 by §15.7, the single owed-list.**  Two items
this paragraph named are DISCHARGED: the marked re-run of the clock-step class happened in
§9.7.3, and cycle slips ARE now wired into an event matrix — the §15 bench, with a
±1/2/3/5/10/50-cycle magnitude ladder and an empirical MDB of **2 cycles**.  §15.7 also
carries a fact this paragraph did not know: **CB step/ramp is currently NOT INJECTABLE AT
ALL**, because both bias channels are epoch-constant by construction.  Read §15.7, not this
paragraph.

---

## §11 NEXT — the W6+SH synthetic arm (design brief for the next session)

> ⚠️ **SUPERSEDED IN PART, 2026-08-12 — the arm is BUILT and its first ladder is
> measured; see §12.**  **Items 1 and 3 did not survive contact with the data and must
> not be re-used as written** — item 1's framing is wrong about WHICH wall comes first
> (the wall at 50 stations is not a code-bias/ionosphere separability floor but ONE
> uncounted near-null, the **L2W satellite phase-bias COMMON MODE**, ev **9.78e-06**
> against a 6.9e-12 clock gauge, §12.2; the elevation-cutoff suspicion is settled too —
> with the 25° cutoff armed the posture bundle is 3/3 and the arm still withholds every
> epoch, so the 50-station withhold is NOT a cutoff artefact), and item 3 is MOOT rather
> than a design decision.  **Items 2, 4 and 5 stand and are implemented as written.**

The §9.9/§9.9.1 elimination campaign cornered the field wire wander (1.3 m detrended) as
ESTIMATOR-INTERNAL: the clock ↔ CB ↔ iono common exchanges.  Those states exist only in
the coupled W6+SH posture — **which the synthetic fixture had never run** (every §9 arm is
INT posture; sim-internal wander there is 0.024 m).  This arm runs the field's
wander-bearing posture on synthetic truth so the wander decomposes exactly.

*(Item numbering below is FROZEN — `_simnet50_wsh_probe.json:3` cites "§11 item 1" and
`scripts/simnet_exchange.py:4` cites "§11 item 4".  Items 1–4 are compressed to their
disposition; item 5 is the standing-rules list, carried in full.)*

| item | disposition |
|---|---|
| **1** "the observability floor is STATION SUPPORT; scale the roster to ~300" | **FALSIFIED, do not follow** (banner above; §12.2 rung 5 and §12.3).  The wall is one uncounted near-null (the `PB\|*\|L2W` common mode), n-INVARIANT, and 50 stations run the slice.  What survives: the fixture is dual-frequency (L1C/L2W); the ledger's `estimate_sat_code_bias` interlock text ("dual-frequency or single-channel sets ⇒ the null-space rank gate WITHHOLDS EVERY EPOCH"); `project_dual_freq_osb_rank_deficiency` (dual+OSB solves at 305, so it is not a frequency wall); §9.7.2 finding 5's all-240-withheld reading was a PARTIAL slice.  The fallback this item listed — add a third sim band — is what actually worked (§13.3.1) |
| **2** arm the FULL slice, never a partial one | **implemented as written**: `estimate_sat_code_bias: true` force-arms the coherent set (GIM constraint, iono weighting, IFB) exactly like the field; `estimate_iono_sh` rides on top.  W6 bias posture per §2.2: `corrections.signal_bias: true` (the injected OSB **is** the B-record truth), `enable_code_phase_bias` forced off by the loader |
| **3** "the GIM constraint vs synthetic truth is a DESIGN DECISION" | **MOOT, not a decision.**  Under `estimate_iono_sh` the external GIM is **never loaded**; the two iono constraints are mutually exclusive by construction (`ApplicationNetworkSetup.cpp:386-440`).  What IS load-bearing here is the elevation CUTOFF, which gates which lines of sight get an SH tie row |
| **4** grading = per-family common-mode decomposition against truth | **implemented**: `scripts/simnet_common_mode.py` extended to clock w(t) (C records), CB common (B vs injected OSB) and iono a₀₀ (`scripts/sh_vtec.py` over `network_vtec_sh.csv` vs the injected field), plus their cross-correlations — the exchange signature itself, landed as `scripts/simnet_exchange.py`.  The elevation-cutoff 15↔25 A/B WITH truth (ties to the field A/B in `runs/cbq_elcut25`) ran: §12.9 |
| **5** standing rules that bite here | fresh convergence only (datum toggles are fingerprinted); ≥ 2 h settled for any level claim; arm `cycle_slip.if_phase_td` and `cycle_slip.datum_station_detect` (both validated §9.7.10); Release renamed exe; one CWD per arm; pre-register predictions per arm |

### 9.10.1 The elevation-cutoff FIELD A/B — direction CONFIRMED, magnitude small (2026-08-12)

`_cbq_12h_elcut25.json` (= the retained control + GIM
`ionosphere_weighting.elevation_cutoff_deg` 15 → 25, the 305-validated value the FT63
lineage forked below), 12 h, cold, same settled 875-epoch segment, availability parity
(1440/1440 emitted, 0 withholds both):

| | control 15° | **25°** | Δ |
|---|---|---|---|
| wire common raw std | 1.735 m | 1.527 m | −12 % |
| wire common detrended std | 1.298 m | **1.191 m** | −8 % (~16 % of variance) |
| linear trend | −0.548 m/h | −0.454 m/h | −17 % |
| station-attribution table | flat (max 0.053) | flat (max 0.090) | no station driver either arm |

The pre-registered direction holds — the §9.10 low-elevation iono incoherence DOES feed
the clock common — but it is a MINOR contributor: ~84 % of the wander variance survives.
The FT63 lineage at 15° was never in the 305-run's divergence regime (that record was 10°
vs 25° with an actual a00 collapse); raising to 25° is a cheap, availability-free trim
worth adopting on the lineage, not the cure.  **The dominant wander source remains inside
the clock ↔ CB ↔ iono exchanges — the §11 W6+SH synthetic arm is unchanged as the decisive
experiment.**

## §12 The W6+SH ARM — built, and its minimum-n ladder measured (2026-08-12)

Everything here was run; each claim names the arm that produced it.  Release only,
`gnss_processor_wsh0.exe` (sha256 `6bf857bb…`) for the ladder rungs and
`gnss_processor_wsh.exe` (sha256 `521512b6…`, the same tree plus the §12.4 traces) for the
trace A/B, one CWD per arm.

### 12.1 What was built

| piece | file | what it is |
|---|---|---|
| producer template | `config/test_suite/_simnet_wsh_template.json` | the W6+SH posture: `estimate_iono_sh` (which FORCES the whole coupled slice), 25° iono cutoff, SH degree 15, `sh_health_gate`, `if_phase_td` + `datum_station_detect`, `trace_estimability`, the two new σ traces, equatorial clock datum |
| 300-station fixture | `config/sim/simnet300_wsh.json` → `runs/simnet300_wsh/` | 300 solve + 1 consumer, 480 epochs = 4 h @ 30 s, **`corrections.signal_bias: true`** (the injected OSB IS the B-record truth), quiet, pinned storm anchor |
| roster generator fix | `scripts/simnet_make_roster.py` | 9-char markers past n = 99 (`S00101SIM`, scheme `wide3`); the old scheme emits a TEN-character name at n = 100, not a RINEX long name at all.  Legacy rosters regenerate byte-identically |
| per-arm products / prefix | `scripts/simnet_make_producer_config.py` | `--products`, `--epochs`; plus a LOUD warning when the clock datum falls back to `stations[0]` (on a Fibonacci roster that is the most POLAR station, and §9.8 measured ~8× more common-level wander from a polar datum) |
| B + receiver channels | `scripts/simnet_grade.py` | §4.4's two-stage S-transform applied to TRUTH (IF lump over the S line's own `cbpin` pair, then zero-mean per channel over the epoch's registered set), vf from the new σ trace; receiver DCB graded as a CONTRAST with the level reported as the absorbed Table-8 gauge |
| exchange decomposition | `scripts/simnet_exchange.py` | §11 item 4: the clock / receiver-DCB / a₀₀ common-mode series, their correlation matrix (series AND first differences), and the measured-vs-predicted exchange coefficient |

### 12.2 The ladder — four rungs at n = 50, 20 epochs each (~90 s per rung)

The 50-station G-A fixture is a valid EMISSION probe for the W6+SH slice even though it
was synthesized in the INT posture: rank structure does not depend on the injected bias
VALUES.  The whole ladder cost about six minutes.

| rung | one knob | epochs emitted | what it establishes |
|---|---|---|---|
| 1 `_simnet50_wsh_probe` | — (25° cutoff, deg 4) | **0 / 19** | posture bundle **3/3 armed**, so the §9.7.2 finding-5 withhold is **not** the elevation-cutoff artefact |
| 2 `_simnet50_wsh_eq34` | `phase_datum_eq34` | **0 / 19** | ~~the field's Eq-34 phase datum does **not** lift it — the datum is not the lever~~ **SUPERSEDED §12.8.2**: the datum DOES lift it (rank gate passes); all 19 withholds were the SH gate — a₀₀ −15 965 TECU at its first epoch |
| 3 `_simnet50_wsh_sbc` | `sbasis_complete` | 1 / 19 | gate PASSES and the **ionosphere is destroyed** — see below |
| 4 `_simnet50_wsh_dgtol` | `network.rank_gate_tol: 1e-12` | **19 / 19** | emits, and the solve is healthy |
| 5 `_simnet300_wsh_smoke` | **300 stations** (deg 15) | **0 / 20** | **station support is NOT the wall** — the n-invariance below |
| 6 `_simnet300_wsh_n50_carry` | `phase_datum_eq34` + `carry_invariant` | **0 / 19** | the carry cannot lift it COLD, and cannot warm through a withhold — the withholding gate is the SH gate, not the rank gate (§12.8.2) |

**The direction, named.**  Rung 1's dump (`network_estimability.csv`, `reduced_N_ss`
143×143, `declared_dim` 1, λmax 1.0283e+05 ≈ 1.03e+05, cutoff 1.0283e-04) shows exactly
two directions below the cutoff:

| ev | composition | reading |
|---|---|---|
| 6.94e-12 | `CLK\|G08…` uniform 0.1768 = 1/√32 | the declared clock gauge ✓ |
| **9.78e-06** | `PB\|*\|L2W` uniform ≈ 0.258 | **the L2W satellite phase-bias common mode** — stable to four digits across every epoch |
| 2.48e-02 | `PB\|G13\|L2W` + `PB\|G13\|L1C` | the weakest REAL layer |
| 2.30e-01 | `SH\|1\|0\|C`, `SH\|0\|0\|C`, … | the SH block, healthy |

With float ambiguities a satellite-common L2W phase bias is absorbable by the L2W
ambiguities, so this is a **gauge the declared dimension (= #constellations = 1) does not
count**, 5.9 decades above the numerical gauge and 3.4 below the weakest real layer.  The
standing record predicted it in words — *"'dual'/'single' leaves the secondary-band
(L2W/L5Q) phase-bias common mode under-determined at low station count"* — and this is it,
isolated and quantified.

**★ It is n-INVARIANT, and that reframes the whole brief.**  The 300-station arm (rung 5)
reproduces the identical spectrum: λmax 1.0283e+05 (≈1.03e+05) → 9.1255e+04 (≈9.13e+04),
ratio 0.89; clock gauge 6.94e-12 → −3.44e-10 (numerical zero); **the extra direction
9.78e-06 → 8.97e-06, ratio 0.92**; weakest real layer 2.48e-02 → 1.69e-02, ratio 0.68 —
every real layer moves with the data and the extra direction does not.  **Six times the
stations added no information to that direction**, which is what distinguishes a structural
gauge from a weakly-determined parameter and confirms the mechanism: more stations bring
more *ambiguities* as well as more data, so the absorbability is untouched.  The record's
closing clause *"at low station count"* is therefore wrong — it is not about station count
at all.

Rung 6 closes the remaining cheap hypothesis.  `carry_invariant` (the DD-chord time link,
the one mechanism that could stop the ambiguities absorbing the bias) is **refused by the
loader without `phase_datum_eq34`** (the cycle basis is defined only on the eq34 chain
datum), so the pair is the correct test — and the pair still emits nothing cold.  It also
cannot bootstrap: the carry needs emitted epochs to warm and the gate withholds them, the
degrade-during-withhold deadlock this project has already met once.  ⚠️ **The withholding
gate is the SH gate, not the rank gate — §12.8.2.**

**Rung 3 is the most important rung, and it is a WARNING.**  `sbasis_complete` constrains
the extra direction along its own eigenvector *at the carried level*; at epoch 2 that level
is cold and the tension went straight into the field — **a₀₀ = −6292 TECU, rms_all 3541
TECU**, whereupon the SH health gate withheld 18 of 19 epochs.  The rank gate was satisfied
and the answer was garbage: the fixture's whole reason for existing, reproduced on a knob.
The datum-station slip at TOW 302430 is not the cause — rung 4 sees the same slip (G09 at
S26000SIM, 28 ambiguities reseeded across 49 stations) and stays clean.  **Do not arm
`sbasis_complete` on a cold start in this posture.**

**Rung 4 is healthy, not merely emitting**: a₀₀ 31.55 → 31.59 TECU across the arc (σ 1.9 →
0.50), in band, per-degree rms stable; post-fit σ̂₀ **0.962 code** / 0.024 phase (the phase
pool is structurally ~0 with free ambiguities — expected and documented); consumer
converging.  Its tolerance was placed from this config's OWN dump as the BeiDou card's rule
requires — cutoff 1.03e-07, 4.8 decades above the gauge depth and 2.0 below the weakest
real layer — **never globally**, since `assertNullSpaceRank` throws in both directions.

### 12.3 What the ladder does and does not settle

- **50 stations are enough to run the W6+SH slice** on this fixture, making the graded arm
  ~6× cheaper than §11's recommendation assumed — and scaling to 300 buys nothing here, so
  §11 item 1's recommendation should not be followed.
- It did **not** settle *why the field emits at 303 with the default tolerance* — three
  candidates remained, each a ~90 s one-knob rung: **(a)** warm resume
  (`state_snapshot_carry`, so the rung-6 deadlock may never be entered); **(b)**
  `network.cb_kaula_prior` (σ 3 m), which the BeiDou card measured LIFTING near-null
  directions, armed in the field and absent here; **(c)** channel diversity the two-signal
  sim does not have.  **All three resolved in §12.8.3** (and the premise itself corrected:
  rungs 2/6 were SH-gate, not rank-gate, withholds — §12.8.2).
- **A rung that emits is not a rung that is right.**  Rung 4 estimates a direction that may
  be a genuine gauge; if so its emitted L2W phase-bias common mode will wander with an
  honest large σ — a graded question for the settled arm (the P channel +
  `emit_phase_bias_sigma`), not something the emission count says.

### 12.4 The §4.8 export gap is CLOSED (both halves)

`network.trace_sat_bias_states` → `network_sat_bias_states.csv`
(`week,tow,sat,signal,code_bias_m,sigma_m,s0,pinned`) and `network.trace_rx_states` →
`network_rx_states.csv` (`week,tow,station,kind,key,value_m,std_m`).  Both trace-only,
default-OFF, **no product grammar change** — the B record still carries no σ, so no
consumer fingerprint keyed on the header legend moves.

The satellite trace reuses the URA/combined-σ covariance treatment verbatim (gauge nulling,
datum-pin skip, positive-variance guard, σ̂₀ scaling) so a vf from it is comparable with a vf
from the U record; a datum-pinned column is written with an EMPTY σ and `pinned=1` rather
than its gauge-artefact variance.  The receiver trace follows the `network_zwd.csv` pattern
(`reconstructStation` + `conditionalLocalVarianceDiag`).  **Invariant #6, same-build A/B**:
`_simnet50_wsh_tron` vs `_simnet50_wsh_troff`, one knob pair, 19 epochs — `products.txt`
**byte-identical** (sha256 `7aaa21eb…` both), no files created when OFF.

First measurements (rung-4 posture, 19 epochs — **ignition, not verdicts**): 64 satellite
rows/epoch with σ 0.52 m → 4.5 mm across the arc; the receiver DCB is one `GPS_DCB` state per
station (not per code, so the comparable truth is the injected GF difference) with a
conditional σ of **~32 m** against a ±0.5 m injected spread — almost pure gauge, precisely
what Table-8 says.

### 12.5 Pre-registered predictions for the settled arm

Written before the graded run, per §6.  **All three resolved in §12.7** — this stub is
kept for the pre-registration record and because P1's wrong sign is a load-bearing lesson.

1. **P1 — the exchange.**  Pre-registered **−7.93 TECU per metre** of fleet-mean
   receiver-DCB error (reasoning: absorbing a positive receiver GF bias requires a negative
   ionosphere; γ = 1.6469, 0.1624 m/TECU, obliquity 1.2), predicting strong NEGATIVE
   correlation of `rxdcb_common` with `a00_err`.  ⚠️ **THE SIGN IS WRONG — do not re-adopt
   this number as written.**  Derived from the model's own coefficients the value is
   **+7.93** (§12.7, §14.6).  The correlation half was confirmed.
2. **P2 — the CB variance factor.**  Pre-registered **vf > 2** (optimistic σ), because
   every other channel on this fixture is optimistic (C vf 455, ZWD 1.63 at n = 50
   settled).  Answered §12.7: **≈ 4.9e+05**.
3. **P3 — the level.**  a₀₀ ran ~3.7 TECU BELOW the network-sampled truth mean in rung 4's
   ignition window, the same direction as the field's standing DEFECT 2.  Pre-registered:
   the settled offset stays negative and shrinks, graded time-matched against the truth
   field's own moving mean, never a single number.  Confirmed §12.7 (−1.42 TECU settled).

### 12.7 THE GRADED ARM — 466/480 epochs, and all three predictions resolved

`_simnet300_wsh_n50_pin` — 50 stations (stride-6 of the 300-station W6 fixture), 480
epochs = 4 h, `rank_gate_tol` 1e-12 placed from this arm's own dump, graded on the settled
half (233 epochs).  (The T2/T3 rungs of §12.8.3/§12.9 all extend the same base config
`_simnet300_wsh_n50.json`.)  **466 emitted, 14 skipped — all 14 by the A4 σ̂₀ ceiling, ZERO
by the observability gate.**  The SH field never left its band, so the health gate is
untested here and the run-end line says so.

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

**P2 — ANSWERED, and not subtly.**  The emitted satellite code bias carries **1.14 m rms of
true error** while its formal σ (median ~4.5 mm settled) claims millimetres: vf **~4.9e+05**,
σ optimistic by ~700× *in σ*.  So the field's "cb vf ≈ 10" is nowhere near the estimator's
actual calibration on truth — whatever the field census measures, it is not the code-bias
channel's true error budget.  §9.5 finding 2's formal-σ-collapses-while-true-error-grows
pattern holds on the code-bias channel too, and harder.

**P1 — the exchange is CONFIRMED, and my pre-registered SIGN was wrong.**  Settled
correlations (226 joint epochs): clk_common ↔ rxdcb_common **−0.939** (first difference
−0.009); clk_common ↔ a00_err **−0.848** (+0.149); rxdcb_common ↔ a00_err **+0.853**
(−0.467); cb_rms ↔ a00_err **−0.832** (+0.010).  **The exchange lives entirely in the slow
drift** — every level correlation 0.83 to 0.94, every first-difference correlation ≈ 0.  The
four channels do not share epoch-to-epoch noise; they share a wandering common direction,
exactly the shape the field campaign inferred and could not watch.

Coefficient: measured **+5.67 TECU per metre** of fleet-mean rx-DCB error (corr +0.853)
against a **derived +7.93** — 0.72× the ideal-absorption value.  ⚠️ **SUPERSEDED ON THE SAME
PRODUCTS by the current `simnet_exchange.py`: +7.69 (0.97×)** — quote 0.97×, not 0.72×
(§12.9 script-version note, §14.6).  Either way most but not all of the receiver-DCB common
error is traded against the field (the ionosphere is also held by its own data and its
Kaula prior).

⚠️ The **sign** in §12.5's P1 was wrong, and the correction matters more than the number: it
must be *derived from the model's own coefficients*, not argued.  The receiver DCB enters
the two reference-band code rows with `mu1/dmu` and `mu2/dmu`
(`StateInitializer::resolveReceiverBias`, mu1 = 1, mu2 = γ = 1.6469, dmu = −0.6469), so on
P1 − P2 its signature is exactly **+1 · x_DCB** against the ionosphere's
**(1 − γ) = −0.6469**, giving Δι = **+1.546** m of L1 per metre of DCB error — POSITIVE.
The **second** bias sign convention to bite in one session (the first: the absorbed IF lump
in the C channel, slope −1.08 / corr −0.78, opposite to §4.2's written `+`).  **Rule for
this fixture: never assert a bias sign from a verbal argument — read the coefficient off the
model, or measure the slope and say which you did.**

**P3 — directionally confirmed.**  a₀₀ runs **−1.42 TECU** below the network-sampled truth
mean on the settled window (σ 0.77) against −3.68 TECU in the 19-epoch ignition window:
negative and shrinking with settling, exactly as predicted.  Graded against the truth
field's own per-epoch coverage-weighted mean, never a single number — the trap §6 of the
level card names.

### 12.6 What is still owed

DISCHARGED (pointers, not re-argued): the 300-station arm at DEFAULT tolerance → rung 5,
**0/20**; a ≥ 2 h settled graded run of rung 4's posture → **§12.7**; the 15↔25
elevation-cutoff A/B WITH truth (§11 item 4) → **§12.9** — the truth grade INVERTS the
field verdict on this coherent fixture (15° wins every clock-adjacent channel 1.3–4×; the
cut FEEDS the exchange, 0.50× → 0.97× of ideal absorption; the field's 25° adoption stands
on field evidence, its separability cost is now measured); "why the FIELD emits at 303
with the default tolerance" → **§12.8.3** (the eq34 chain lifts the near-null, audited
complete; the sim's non-reproduction is its phase−code alignment convention, not rank;
`cb_kaula_prior` **FALSIFIED** alone AND paired with eq34).

STILL OWED:

- The `sbasis_complete` failure deserves its own note in the SBC card: a cold-start engage
  can inject a carried level that no rank instrument sees.
- The exchange decomposition should be re-run on a **datum-station A/B** (§9.8's strongest
  lever): if the clock↔rx-DCB −0.94 is really the datum station's own error propagating,
  moving the datum should move the whole signature.

## §12.8 The CHAIN-PIN AUDIT — rungs 2/6 re-attributed, and the field's emission
## explained (2026-08-12, HANDOFF_wsh_chain_slant T1/T2)

*(That handoff is now `docs/attic/HANDOFF_wsh_chain_slant.md`; its content is **§14**, and
**§14.0** is the T1–T4 label map.  T1's theory is §14.1/§14.2.)*

T1 asked: per (constellation, signal), do the eq34 chain pins the election declares
actually reach the solve?  Three silent loss stages existed — `foldAmb`/`foldRpb`
early-return without a trace (no state index / no live phase row), and a station whose
local block `est.addStation` rejects is dropped WITH every folded pin it carried.  Standing
invariant #4: an armed datum that places fewer pins than it thinks reads exactly like a
working one.

**Instrument (built, always-on under eq34, log-only).**  The election records per-group
declared gauge dimension Σ(n_c + m_c) (`NetworkChainDatum.cpp`, the `dr_key` map); the
produce phase tallies each fold outcome per station (`StationAssembly::pin_audit`, summed
serially — thread-count independent); the post-consume comparison accumulates declared /
folded / landed / miss-reason / station-loss counters
(`NetworkRunContext::Eq34PinAudit`); `NetworkTeardown` prints one line per group.  Solve
inputs untouched; products unaffected.

### 12.8.1 The audit answer: the chain is COMPLETE — and the decision point flips

`_simnet300_wsh_n50_pinaudit` (rung-2 posture + `station_pd_pin_aware`, default tolerance,
19 audited epochs):

```
G|L1C: COMPLETE all 19 epochs — declared 1558 (−0 bridged), folded 1558, landed 1558
G|L2W: COMPLETE all 19 epochs — declared 1558 (−0 bridged), folded 1558, landed 1558
```

1558 = 19 × 82 = 19 × (50 stations + 32 satellites): every component's exact gauge
dimension, zero fold refusals, zero pins lost to dropped stations, both signal groups,
every epoch.  **The chain datum is not under-placed** — the handoff's "if complete, the
near-null is elsewhere and the tolerance stays" branch is the one taken, with one large
correction below.

### 12.8.2 Rungs 2/6 were NEVER rank-gate withholds — §12.2's attribution corrected

The audit rung's estimability dump shows the rank gate **PASSING** under eq34:
`n_below = 1 = declared_dim` at the default cutoff.  The L2W common mode is **lifted
9.78e-06 → 1.67e-04** (~17×, from below the 1.19e-04 cutoff to above it) — the de Jonge /
Odijk chain prescription works exactly as §14.2 reads it.  The 0/19 emission is the **SH
health gate**: a₀₀ −18 780 TECU at the first epoch, −32 633 at the second.  Re-reading rung
2's own record confirms the same signature was there all along: every epoch
`sh_gate_withheld` (a₀₀ −15 965 TECU at its first epoch), and its run-end line says **"0 of
those withheld by the observability gate"**.  So:

§12.2 rung 2's "the field's Eq-34 phase datum does not lift it — the datum is not the
lever" is **WRONG in mechanism**: the datum lifts the near-null; what fails cold is the
*level* the constrained family is realized at.  Rung 6's "the carry cannot lift it COLD"
inherits the same correction — the deadlock is real, but the gate it cannot warm through is
the SH gate.  Rung 3 (`sbasis_complete`, a₀₀ −6292) and rungs 2/6 are the **same failure
class**: constrain the under-determined family at a cold garbage level and the tension
discharges into the ionosphere.

### 12.8.3 The discharge is a SIM-ONLY phase-alignment artifact — and it answers
### "why does the field emit at 303"

The sim draws integer ambiguities **N ∈ [100 000, 1 000 000) cycles, independently per
(satellite, band, station)** (`src/sim/main.cpp` `ambN`, deliberately unguessable for the
MW/NL exercise).  Phase−code lumps are therefore 19–250 km, and the eq34 cold pins drive
tree-edge ambiguities to **absolute zero on both bands independently** — each doubly-pinned
edge imposes a random geometry-free pseudo-iono of order (λ₁N₁ − λ₂N₂)/(μ₁−μ₂) ≈ tens of
km.  The per-LOS iono states are the softest GF sink, the SH ties drag the field, a₀₀ lands
at −19 000 TECU (≈ −3.1 km of L1 delay — the observed scale), and the SH gate withholds.
**Real receivers keep phase aligned to code within metres**, so the same pins cost the
field metres of absorbable phase-bias warp: the field cold-starts this exact posture
healthy.

§12.3's three candidates, resolved:

| candidate | verdict |
|---|---|
| (b) `cb_kaula_prior` σ 3 m, alone | **FALSIFIED** — `_simnet300_wsh_n50_kaula`: 0/19, rank-gate withheld.  A code-side prior does not lift the phase-side type-5 direction |
| (b) with eq34 (the field's actual pair) | **FALSIFIED as the protection** — `_simnet300_wsh_n50_eq34kaula`: 0/19 `sh_gate_withheld`, a₀₀ −19k/−33k unchanged; pin audit again 1558/1558/1558 both groups |
| (a) warm resume | **MOOT** — the field's 12 h arms emit **cold** (§9.10.1: 1440/1440), so warm resume cannot be the explanation; and arming a fingerprinted datum toggle against a foreign snapshot is illegal anyway |
| (c) channel diversity | **NOT NEEDED** for the emission question (it IS the §13 third-band story, separately measured) |

**The answer: the field emits at 303 because its eq34 chain lifts the L2W near-null above
the default cutoff — the audited, complete chain doing exactly what Odijk's global-network
caveat prescribes.  The sim could not reproduce it for a reason that has nothing to do
with rank: the zero-target cold pins fight the sim's km-scale integer draws.**  Consistent
with §13.3.1, where the third band removes the near-null structurally and the sim emits at
default tolerance with no datum armed at all.

### 12.8.4 Consequences

| consequence | detail |
|---|---|
| **the graded sim posture stays rung 4** (per-config `rank_gate_tol`, direction FREE) | on this fixture the free-gauge posture is the healthy one; the eq34 posture is field-correct but sim-hostile at f = 2 |
| **identified structural fix (NOT BUILT): first-sighting pin-to-round** | `eq34_round` pins re-sighted arcs to λ·round(carry/λ) but first sightings to 0, and cold there is no carry — while the carry cannot warm because the SH gate withholds (the degrade-during-withhold shape again).  Extending the round target to the CURRENT float at first sighting would make eq34 cold-startable on any receiver with large phase−code lumps, and is field-inert for aligned receivers (round of a metre-scale lump ≈ the same small integer).  Default-OFF, byte-identity A/B, per invariant #6 — **SPEC'd as its own card, `docs/SPEC_eq34_seed_round.md`** (knob `network.phase_datum_eq34_seed_round`; covers the incumbent-without-carry RE-ZERO trap and the carry_invariant hole, not just the first sighting) |
| the `sbasis_complete` SBC-card note owed in §12.6 now covers three knobs, not one | any mechanism that hard-constrains this family cold (sbasis_complete, eq34 zero-target pins, and by extension any future datum "completion") must either seed its targets from the current state or be barred from cold starts in this posture |

## §12.9 The elevation-cutoff A/B WITH TRUTH — the field verdict INVERTS on a
## coherent field (2026-08-12, HANDOFF_wsh_chain_slant T3)

*(T3's label map is §14.0; that handoff is now `docs/attic/`.)*

`_simnet300_wsh_n50_pin_el15` (15°) vs `_simnet300_wsh_n50_pin_el25b` (25°), 480 epochs
each, settled half graded, **same build** (`gnss_processor_wsh3.exe`, sha256 `cbaefe0a…`).
The 25° control re-run reproduces the stored §12.7 arm to four digits on every channel (C
1.0251/0.8891 vf 5683.6; B 1.1442 vf 489 286; ZWD 0.5413 vf 2301; P 0.2662/0.3178) — which
simultaneously certifies the A/B as single-knob AND that the §12.8 audit instrumentation
plus the day's concurrent tree edits are producer-inert on this posture (invariant #6
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
| **clk_common wander std** | **0.0598 m** (≈ 0.060) | 0.2478 m (≈ 0.248) | **4.1×** |
| a₀₀ error (mean ± std, TECU) | −0.12 ± 0.52 | −1.57 ± 1.03 | 3× |
| exchange coefficient (of +7.93 ideal) | +3.96 (**0.50×**) | +7.69 (**0.97×**) | — |
| clk↔rxdcb / rxdcb↔a₀₀ corr | −0.848 / +0.667 | −0.939 / +0.859 | tighter at 25° |

**P-A/B-2 is FALSIFIED in the pre-registered direction — the truth grade INVERTS the field
verdict.**  The 15° arm is better on every clock-adjacent channel, and the exchange numbers
say why mechanically: the low-elevation rays carry the OBLIQUITY DIVERSITY that separates
clock ↔ receiver-DCB ↔ ionosphere.  Cut them and the family collapses onto the
ideal-absorption manifold — the exchange coefficient rises 0.50× → 0.97× of the derived
ideal, the level correlations tighten toward ±0.9, the common-mode wander quadruples, and
the absorbed error surfaces as 2× clock truth error.  **The cutoff does not suppress the
exchange; it FEEDS it.**

**Reconciliation with the field A/B (§9.10.1: 25° trims wander 12 %).**  No contradiction —
the two arms measure two sides of one trade.  The synthetic field is COHERENT BY
CONSTRUCTION (the injected iono maps through the same single-layer geometry the estimator
assumes), so its low-elevation rows carry information and zero pathology: the cut is pure
separability loss, priced here.  The field's low-elevation rows additionally carry the REAL
§9.10 low–high incoherence, and removing that contamination nets −12 % wander *despite*
paying the same separability tax — which also explains why the field trim was so small
("~84 % of the variance survives"): the cut is partially self-defeating, weakening the very
containment that bounds the exchange.  The FT63 adoption of 25° stands on the field
evidence, but its cost now has a number, and any future "raise the cutoff further" proposal
must count BOTH sides (30° would likely lose in the field too).

**Script-version note.**  §12.7's exchange coefficient "+5.67 (0.72×)" is superseded ON
THE SAME PRODUCTS by the current `simnet_exchange.py`: +7.69 (0.97×), correlations
essentially unchanged (−0.939/−0.841/+0.859/−0.847).  The A/B above is internally
consistent (one script version for both arms); quote 0.97×, not 0.72×, as the 25° settled
exchange ratio.

## §13 The THIRD BAND, and the slant-ionosphere posture (2026-08-12)

Odijk et al. 2016 (J Geod 90:15-44) §5.1, p. 29, on the model with free SLANT ionospheric
delays:

> "the receiver and satellite code biases are **not estimable anymore for the first two
> frequencies**… code biases are only estimable for a (modernized) triple- or
> higher-frequency constellation… the geometry-free receiver/satellite code biases become
> lumped to the ionospheric slant delay."

Under the **vertical** single-layer model instead (their §3.2), only the iono-free biases
are constrained (Table 2, rd 2a/3a), so the geometry-free part survives as one estimable
parameter shared by frequencies 1 and 2 — the DCB (Eq. 29–32,
`d̃^s_2(1) = (μ2/μ1)·d̃^s_1(1)`).  **That is the posture we run**, with the single-layer tie
imposed *softly* (SH rows + `representation_sigma_tecu`), which is why the dual-frequency
code bias is estimable here at all — and why its information is a model assumption rather
than the data.  §12.7's **cb vf ≈ 4.9e+05** is the price, measured.

### 13.1 What was built (sim side, done)

`SimConfig::Signals` — `signals.gps_l5`, `signals.galileo_e5b`, `signals.synth_osb_amp_m`.
Both bands **opt-in**: the band plan is already a table (`src/sim/main.cpp:1618-1643`) and
the noise/multipath/slip draws are taken per band inside the satellite loop, so appending
one shifts the whole stream — default-off keeps every existing fixture byte-identical, and
arming it is a new lineage by construction.  Verified: a two-band regeneration is
byte-identical on the RINEX and on `truth_atmo.csv`; `truth_bias.csv` gains three
provenance columns (`product_m,synth_m,source`) with the first four columns unchanged
row-for-row.

**The synthetic OSB is not optional for this experiment.**  The shipped `COD0MGXFIN` OSB
product carries **no GPS band-5 rows at all** — its GPS inventory is L1C/L1W/L1L/L1X,
L2W/L2S/L2L/L2X, C1C/C1W/C1L/C1X, C2W/C2S/C2L/C2X (verified by inventory).  A third band
synthesized from the product alone would carry an identically-zero code bias, and no grader
could separate "estimated it correctly" from "the channel is zero".  `synth_osb_amp_m`
injects a deterministic per-PRN delta (no RNG) recorded in the truth file with its
provenance.

Fixture: `config/sim/simnet60_tri.json` → `runs/simnet60_tri` (60 solve + 1 consumer, 480
epochs, GPS triple, `synth_osb_amp_m` C5Q 0.60 m / L5Q 0.02 m).  The roster now also
injects a **C5Q receiver bias** (inert on two-band fixtures, since the sim only applies a
bias to a code it emits).

### 13.2 The slant posture is NOT reachable in the producer today — two changes

| piece | state |
|---|---|
| loader path | **blocked**: `estimate_sat_code_bias` with neither SH nor GIM hard-errors (`ApplicationNetworkSetup.cpp:414-437`); `estimate_iono_sh` force-arms the SH field (`GnssConfigLoadNetwork.cpp:2060-2066`) |
| satellite GF datum | **missing**: `buildCodeBiasDatumRows` (`NetworkSbasis.cpp:415`) builds (1) a per-satellite **IF** pin and (2) per-(constellation, signal) sum-to-zero.  There is no **GF** pin |
| receiver GF datum | **already exists**: the receiver DCB is one `GPS_DCB` state per station whose GF signature is exactly `+1·x_DCB` (`StateInitializer::resolveReceiverBias`, μ1/dμ and μ2/dμ), so `network.rx_code_bias_datum` (sum-to-zero over those states) IS Odijk's receiver-side constraint |

Rank accounting to satisfy (Odijk §5.1 + Table 7).  Dropping the iono constraint opens two
deficiency types: **2b** — receiver hardware biases ↔ slant iono, size **n − 1**, removed by
constraining the geometry-free RECEIVER code biases (`d_r,GF(1)`; CC-R needs n − 1, CC-S
needs n); **3b** — satellite hardware biases ↔ slant iono, size **m**, removed by
constraining the geometry-free SATELLITE code biases (`d^s_GF(1)`; CC-R needs m, CC-S needs
m − 1).  So the work is a loader path allowing the combination **gated on f ≥ 3** (below
that Odijk says there is nothing to estimate and the current hard error is *correct*), plus
GF datum rows on the satellite side shaped like the existing IF pin —
`μ_GF = (−1, 1, 0, …)/(μ2 − μ1)` (their Eq. 30) instead of `μ_IF`.  Both default-OFF with a
byte-identity A/B.

**State the product consequence rather than discovering it later.**  Under Table 8 the
emitted quantities change meaning: `ι̃^s_r = ι^s_r + d_r,GF − d^s_GF` — the geometry-free
biases move INTO the ionosphere, and the B record then carries only modernized-DCB content
(`d^s_1 − d^s_j`, j ≥ 3).  A consumer must apply the network's ionosphere and its code biases
**as a pair**; applying one without the other double-counts the GF lump.  A product-contract
change, not a tuning knob.

### 13.3 What can be measured before that work

The triple-frequency fixture is useful immediately, with no producer change, for the
chain-datum question: **does the `PB|*|L2W` gauge survive when a third band exists?**  Run
the existing W6+SH posture (§12) on `runs/simnet60_tri` and read `network_estimability.csv`.
Three informative outcomes: (1) the near-null disappears ⇒ a two-band artefact of the phase
datum, and the chain audit has its answer; (2) it persists on L2W only ⇒ per-band, the datum
pins one band and not the other — the sharpest possible statement of the pinning defect;
(3) it appears on L5Q too (two near-nulls) ⇒ "one per non-reference band", exactly Odijk's
type-5 (size f·m) arithmetic, so the pin count scales with f rather than a band being
special.

### 13.3.1 MEASURED — the third band closes the observability wall (outcome 1)

`_simnet60_tri_wsh` (60 stations, W6+SH, SH degree 4, **default** `rank_gate_tol`, 20
epochs): **20/20 epochs emitted, 0 withheld.**  The two-band posture could not emit a
single epoch without a hand-placed `rank_gate_tol: 1e-12` (§12.2); the three-band posture
runs on the shipped default.  The spectrum says why — `n_below` = **1** = `declared_dim`:

| | dual (n = 50 and n = 300) | **triple (n = 60)** |
|---|---|---|
| directions below cutoff | **2** | **1** |
| clock gauge | 6.9e-12 / −3.4e-10 | −1.3e-10 |
| `PB\|*\|L2W` common mode | **9.78e-06 / 8.97e-06** | **gone** |
| weakest real direction | 2.48e-02 | 1.13e-02 (`PB\|G30\|L5Q+L2W+L1C`, per-satellite) |

The satellite-common secondary-band phase-bias gauge **does not exist at three
frequencies**.  The arithmetic is ordinary: each extra band adds 2·n·m observations
against n·m new ambiguities plus ~2m + 2n new biases, so at network scale a third band is
net-informative, and the surplus lands on exactly the bias/ambiguity family (Odijk types
4/5) the two-band datum could not pin.

Health: post-fit σ̂₀ **1.064 code**, a₀₀ 35.7 → 41.6 TECU in band, SH gate never engaged.
B records now carry **C5Q** (640 rows each for C1C/C2W/C5Q over 20 epochs), and the
third-band bias grades against its injected synthetic truth — ignition residual (20 ep):
C1C 0.274 m rms, C2W 0.451 m rms, **C5Q 0.450 m rms** (injected amplitude 0.60 m), datum
check emitted per-channel mean **+1.1e-05 m**, vf **1 998**.

⚠️ **That vf is NOT comparable to §12.7's 4.9e+05.**  Different station count, different
SH degree, and — decisively — 20 ignition epochs versus 233 settled ones.  The direction is
encouraging and the matched comparison is owed: same n, same degree, ≥ 2 h settled, dual vs
triple, one knob.

### 13.3.2 BUILT AND MEASURED — the slant posture runs, and its code bias is CALIBRATED

> ⚠️ **THE "CALIBRATED" VERDICT IN THIS HEADING IS SUPERSEDED BY §13.5 — read that first.**
> Settled and matched, the same posture gives **vf 8 756 published / 34 900 s0-corrected,
> not 1.886.**  The 20-epoch probe read a converging formal σ against a not-yet-grown true
> error (the §9.5 finding-2 signature) as calibration, and the write-up read it as "the
> §13.4 prediction confirmed by five and a half orders of magnitude" (vf ≈ 4.9e+05 dual
> single-layer §12.7 vs ≈ 1.9 here).  Its own caveat — 20 ignition epochs vs the dual arm's
> 233 settled, 60 stations vs 50, different iono posture, matched settled comparison still
> owed — was the right one and it fired.  **Surviving: the STRUCTURAL result** (pinned pair
> datum-fixed, all content on the third band, gate clean at default tolerance).  **Not
> surviving: the "CALIBRATED" verdict**, re-derived on settled data in §13.5 P1.

Built 2026-08-12 (`gnss_processor_slant.exe`, sha256 `33052874…`):

- **`network.code_bias_gf_datum`** — the per-satellite geometry-free pin,
  `(d_2 − d_1)/(μ2 − μ1) = 0` on the IF-pinned pair (`NetworkSbasis.cpp`, folded at
  `kCodeBiasGaugeWeight` beside the IF pin).  Counters `n_gf_pin_sats` and `n_gf_only_pair`
  (satellites with no third band, whose code bias is then fully datum-fixed) ride the
  existing `Network[W6-cb]` line.
- **The loader gate**: the W6 chain force-enables `ionosphere_weighting` only when the GF
  datum is OFF, so `estimate_sat_code_bias` + no SH + no GIM is reachable;
  `ApplicationNetworkSetup` gained the matching no-GIM branch.  Interlocks, all hard
  errors: GF datum without `estimate_sat_code_bias` (nothing to gauge); GF datum with
  `estimate_iono_sh` **or** `ionosphere_weighting` (rd 3b only exists when the ionosphere is
  unconstrained, and under a single-layer constraint the GF bias IS the estimable DCB —
  pinning it deletes the product); GF datum with `frequency_mode: dual` (IF + GF fix both
  pinned channels, so every emitted B record would be the datum itself).

**Arm `_simnet60_tri_slant`** (60 stations, triple, free slant iono, no SH/GIM, default
rank-gate tolerance, 20 epochs): **20/20 emitted, 0 withheld**, `declared_dim` 1 =
`n_below` 1, post-fit σ̂₀ **1.073 code**.  Gauge rows folded: 32 IF + 32 GF + 3 sum-to-zero,
`n_gf_only_pair` = 0.  The structural prediction holds to the millimetre — the pinned pair
is datum-fixed and the third band carries everything:

| channel | emitted rms | residual vs transformed truth |
|---|---|---|
| C1C | 0.0022 m | **0.0022 m** |
| C2W | 0.0032 m | **0.0032 m** |
| **C5Q** | 3.889 m | **0.0760 m** |
| **variance factor** | | **1.886** ⚠️ IGNITION ONLY — SUPERSEDED, see the banner above and §13.5 |

⚠️ **The grader needs `--gf-datum` (auto-detected from the console).**  Under the slant
datum the truth projection removes a SECOND direction:

    t̃_j = t_j − (μ_IF·t) − μ_j·(μ_GF·t)

derived from the paper's own orthogonality properties (μ_IF·e = 1, μ_IF·μ = 0, Eq. 26;
μ_GF·e = 0, μ_GF·μ = 1, Eq. 31), which is why it forces t̃ = 0 on both pinned channels
rather than being told to.  Grading this arm with the single-layer transform reports
**2.3 m / 3.7 m phantom residuals** on two channels that are datum-fixed to zero — exactly
what happened on the first attempt, because the loader had no "armed" log line for the
auto-detect to find.  That silent-arming gap is fixed (invariant #4), and it is a reminder
that a datum change is also a GRADER change.

### 13.5 THE MATCHED MATRIX — settled, and it corrects §13.3.2

One fixture (`runs/simnet60_tri`), same 60 stations, same seeds, 480 epochs = 4 h, graded
on the settled half.  `rank_gate_tol` and `rx_code_bias_datum` matched across all three
arms so each adjacent pair is ONE knob.  Dual+slant is structurally illegal, so frequency
and posture separate in two steps, not one.

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

**⚠️ CORRECTION to §13.3.2: "vf 1.886 → CALIBRATED" was an IGNITION ARTIFACT** — the
20-epoch probe measured a converging formal σ against a not-yet-grown true error, precisely
the §9.5 finding-2 signature this fixture exists to expose, reproduced on a new channel by
the very arm supposed to demonstrate the cure.

**P1 — the discriminator answers POSTURE, not frequency.**  On the s0-corrected scale (vf
is deflated by s0², and s0 differs 1.16 → 2.00 across arms, so the raw numbers are NOT
poolable — the grader says so and it matters here): A 541 000 ≈ B 570 600 ≫ C 34 900.
**The third frequency alone buys nothing for code-bias calibration; the slant datum buys
~16×.**  And 34 900 is still σ optimistic by ~190× *in σ* — the slant posture improves the
channel by more than an order of magnitude and does **not** make it calibrated.

**P2 — CONFIRMED, and it retires a standing objection.**  The recorded 2026-08-04 "slant is
2.2× WORSE" result was dual-frequency and 30 epochs (§14.4 posture B) and does **not**
reproduce here: the third frequency improves the consumer **3.9×** (1.126 → 0.290 m), and
the slant posture gives 1.8× of that back while still landing **2.2× better than the field's
own dual+SH posture**.  Slant does not cost position once the code biases have a third band
to live in.

**P3 — and the surprise.**  The clock and ZWD channels go the OTHER way: A → B degrades the
clock contrast 0.773 → 1.721 m (2.2×) and ZWD 0.549 → 0.873 m, and the posture does not
recover either.  So the third band helps the code bias and the consumer while hurting the
clock and troposphere.  Hypotheses and the blocked discriminator: §13.8.4.

### 13.4 Pre-registered, for when the posture runs

| prediction | reasoning |
|---|---|
| **the code-bias vf should come DOWN by orders of magnitude** | at f = 2 the channel's information is the soft single-layer tie (hence vf ≈ 4.9e+05); at f = 3 under slant the modernized DCB is determined by *data*.  If it does not, the problem was never the ionosphere model |
| **the exchange coefficient should collapse** | §12.7 measured **+5.67 TECU per metre** of fleet-mean rx-DCB error (0.72× ideal absorption) — ⚠ **both figures SUPERSEDED on the same products by +7.69 / 0.97×**; see §12.9's script-version note.  With the GF explicitly pinned and lumped into the iono that trade is closed by construction — the remaining clock↔iono correlation is then whatever is left after removing it, the actual quantity the field campaign wants |
| **the ionosphere should get BETTER and the position WORSE** | recorded measurement, 2026-08-04, 63 sta, 30 ep, ignition-only: dropping SH+W6 for pure slant ran faster but cost the consumer — full figures §14.4 posture B.  A ≥ 2 h triple-frequency arm is the first honest test of whether that regression survives when the third band gives the code biases somewhere to live |

### 13.6 THE AMBIGUITY-INTEGER GRADE — the first absolute integer test, and the
### lane ladder measured (2026-08-12, HANDOFF_slant_triple_next §3)

*(That handoff is now `docs/attic/HANDOFF_slant_triple_next.md`; **its §3 IS this
section**, and its method rules and traps are **§13.8**.)*

`_simnet60_tri_slant_1h_ddint` — posture C + `trace_dd_fractional` (stride 1, max_pairs
2000; note the real knob is `trace_dd_fractional_epoch_stride`), 120 epochs = 1 h,
`gnss_processor_matrix.exe` (DA2F9AC0), 116 emitted (0 observability, 4 A4).  Grader:
`scripts/simnet_amb_integer.py` (new; `--selftest` covers the slip-accumulation join and
the lane matching).  The graded quantity is **e = dd_cyc − DD_true in FULL cycles** against
the fixture's known integers — truth differenced in Python ints (N is km-scale by
construction), the trace's own `frac_cyc` used as a per-row join check (229 961 rows, **0
dropped**).

Integer-structure questions are decided per-row and are 1-h-LEGAL (§13.8.1); the per-band
absolute magnitudes below shrink with settling and must not be quoted as settled levels.

| channel | n | med(e) | med\|e\| | p90\|e\| | ≤0.10 cyc | ≤0.25 cyc | **round(e)=0** |
|---|---|---|---|---|---|---|---|
| L1C (per-band) | 76 485 | +0.055 | 4.956 | 20.39 | 1.9 % | 4.4 % | **8.3 %** |
| L2W (per-band) | 76 911 | +0.072 | 5.034 | 20.35 | 1.5 % | 3.3 % | **8.3 %** |
| L5Q (per-band) | 76 565 | +0.092 | 5.076 | 20.40 | 1.6 % | 3.6 % | **8.1 %** |
| **WL(1,2)** | 64 642 | +0.027 | **0.386** | 1.22 | 15.6 % | 34.9 % | **60.2 %** |
| **EWL(2,5)** | 64 690 | −0.001 | **0.081** | 0.27 | 57.7 % | 88.2 % | **98.2 %** |

Two competing predictions were registered before the run.  **Pessimistic (the standing
record):** the NL blocker is iono REPRESENTATION — per-LOS slant iono scatters 0.26–0.37 m
against a WL→NL bridge requirement of 0.198 m, so `med|frac|` sits at 0.25 (uniform), and
the slant posture removes the SH constraint, leaving the per-LOS iono freer still ⇒ the NL
should get *worse*.  **Optimistic (what the third band opens):** at f = 3 the extra-wide-lane
(L2,L5) has λ ≈ 5.86 m, which 0.3 m of iono scatter cannot break, so the EWL → WL → NL
cascade becomes available on this fixture for the first time.  The honest expected shape
was **EWL near-integer, WL plausible, NL still not**, with the value in measuring *which
rung breaks* — which no field arm can do.

**That shape is confirmed exactly, and now with absolute numbers.**

| # | finding |
|---|---|
| 1 | **The EWL is at its own TRUE integer 98.2 % of the time** (med\|e\| 0.081 cyc).  Not "near an integer" — near the *right* integer, verified against truth.  The lane cascade's first rung exists on this fixture |
| 2 | **The field's fractional instrument was hiding the error scale.**  The raw per-band DDs are not "0.25 cyc off" — they are **~5 cycles off with a ±20-cycle p90 tail**, and only ~8 % sit at their true integer.  med\|frac\| 0.25 uniform (the field reads a uniform **0.248**) was the "off by 47.3" class, not the "off by 0.3" class — the distinction §13.8.2 names, measured for the first time |
| 3 | **The WL:EWL ratio is pure DD-iono arithmetic**: measured med\|e\| ratio 4.8× vs 5.1× predicted from the iono coefficients (WL 1.49 vs EWL 0.29 cyc per metre of L1 DD-iono).  The two lanes share one driver and it is the ionosphere, quantitatively |
| 4 | **The per-band bands carry an EXTRA error component equal-in-cycles across bands** — per-band:WL measured 12.8× vs 3.5× iono-predicted, and all three bands sit at the same ~5.0 cyc med\|e\|, i.e. it cancels EXACTLY in every between-band difference.  Leading candidate (open, not asserted): residual carrier **phase-windup** content — identical in cycles on every band, non-cancelling in global-baseline DDs, and capable of full-turn integer wraps, which would also explain the low per-band round(e)=0 share.  The discriminator would regress e against the modeled windup DD; owed |

Sweep items (the absorbed handoff's §4 sweep, 1-h-legal parts — anchor map §13.8): datum
inventory 67 = 32 IF + 3 STZ + 32 GF with `n_gf_only_pair` 0; `declared_dim = n_below = 1`;
`cbpin=` on the S line; the pinned pair datum-fixed to millimetres on truth (**C1C 0.0023 m
= 2.3 mm, C2W 0.0032 m = 3.2 mm** rms — the "holds to the millimetre" claim, now
attributed: it is the DATUM channels that are exact; C5Q, the information-carrying
modernized-DCB channel, is 0.086 m = **8.6 cm** at 1 h and still converging).  Consumer at
1 h: 3D rms 2.64 m, **almost entirely +UP (mean +2.59 m ≈ +2.6 m)** — the classic slant-iono
ignition signature (per-LOS iono converging from prior pushes UP); §13.5's settled 0.514 m
says it decays; report-only here.  The exchange was NOT run at 1 h — its coefficient is
barred by the 1-h rule and its home is the settled arm, where §13.4's collapse prediction
lives.

**What this changes upstream:** the standing "MW/WL as conditioning — do NOT arm;
net-harmful" verdict was measured at dual frequency, where GPS has no EWL band.  §13.6 does
not overturn it — it bounds it: at f = 3 the EWL DDs are already 98 % correct-integer as
FLOATS, so an EWL-first fixing cascade has a real substrate here.  Whether ARMING the lane
machinery helps the solve is a separate, unanswered question — and the eq34+carry+EWL trio
the handoff's §3.3 named (`network.mw_wl.ewl_enable` requires `carry_invariant`, which
requires `phase_datum_eq34`) was BLOCKED on this sim until seed-round existed (the
zero-target cold pins would inject the §12.8 km-scale artifact into a posture with no SH
gate to catch it; `docs/SPEC_eq34_seed_round.md`).  Since unblocked and RUN: **§13.7, and
it DIVERGES.**

### 13.7 THE SETTLED TRIO ARM — unblocked, and it DIVERGES (2026-08-12)

Same-build (sr.exe 6B1EF5D6) 480-epoch A/B, settled half: `_simnet60_tri_slant_480_ddint`
(float, 474/480) vs `_simnet60_tri_slant_trio_480` (eq34+carry_invariant+ewl+seed_round,
**480/480 — the seed-round card holds; no withhold, max |y| in bound**).

| settled half | float | trio |
|---|---|---|
| per-band med\|e\| | 8.0–9.4 cyc | **~1 720 cyc (p90 ~7 350)** |
| EWL at true integer | **74.7 %** (0.256 cyc) | 5.0 % |
| WL at true integer | 26.2 % | 0.7 % |

**The trio arm has a SLOW RUNAWAY the 20-epoch smoke could not see** (smoke EWL 94 %;
settling degrades monotonically — full-window trio EWL 32 % → settled 5 %).  Attribution so
far: the **WL-2 lane is exonerated** — its run line reads "0 rows decided, rows NOT folded
into the solve" (statistic-only over 226 421 cycles).  The runaway therefore lives in
eq34 + carry_invariant + the every-epoch seed re-round: 130 290 re-rounds (271/epoch —
under carry_invariant no carry ever exists, so every pin re-rounds every epoch), 591
(sat,σ) re-references, 0 carries shifted/erased — the invariant DD carry refolding across a
stepping pin frame, invariant #1/#7's ratchet shape.  **Do NOT arm the trio posture for
settled work** pending a mechanism card; the FLOAT posture stays the graded track.  Two
float-side notes: the lanes DEGRADE with settling (1 h EWL 96 % → settled 74.7 % — the
§13.6 1 h numbers were the optimistic window), and the field's frac-only instrument would
read this runaway as a NORMAL uniform fractional distribution — only the truth grade can
see it.

#### 13.7.1 The D5 hypothesis, TESTED and REJECTED (2026-08-21)

A config audit raised the objection that this arm runs `carry_invariant` with `q_z = 0`
and `carry_invariant_innov_gate` OFF — a stiff zero-q carry — and that the runaway might
therefore be an artifact of an unguarded posture rather than a property of the estimator.
**The objection's premise is half right and its conclusion is wrong.  Tested; §13.7 stands.**

*What the audit got right.*  The arm does run `q_z = 0` with D5 off, and this fixture
injects **zero** cycle slips (`runs/simnet60_tri/*/truth_slips.csv`: 61 files, 0 data rows),
so the arm's `mw_detect` + `if_phase_td` are armed but idle **by construction**.  D5 is the
only response that fires on MODEL ERROR rather than on a detected raw slip, so on this arm
the effective event response really is nil.

*What it got wrong.*  Not "unguarded" in the invariant-#1 sense — the C3c funnel
`carry_slipped_ambs` is gated on `if (carry_invariant)` alone and is live with D5 off; the
config simply has nothing to trigger it.  (Corpus-wide: 633 configs arm `carry_invariant`
and **zero** lack a response entirely.  Loader interlock now at
`GnssConfigLoadNetwork.cpp:6759`.)

*The test.*  Three same-build 480-epoch arms on the CURRENT Release build, single knob,
matched populations (n = 18 980/band settled, 57 272 full).  Configs
`_simnet60_tri_slant_trio_480_ab_{base,d5,d5cal}.json`; the historical
`runs/simnet60_tri_slant_trio_480/` is left untouched as the §13.7 record.

| settled half | control | D5 (k=4, uncal) | D5 CALIBRATED | float twin |
|---|---|---|---|---|
| per-band med\|e\| L1C/L2W/L5Q | 1780 / 1735 / 1745 | 1522 / 1490 / 1492 | **1503 / 1481 / 1483** | 8.0–9.4 |
| EWL med\|e\| | 13.58 | 3.24 | **10.49** | — |
| EWL at true integer | 3.3 % | 11.2 % | **3.6 %** | 74.7 % |
| WL at true integer | 0.3 % | 0.8 % | **0.6 %** | 26.2 % |
| D5 flags | — | 140 126 | **25 709** | — |
| chord births / slip refloats | 3 984 / 1 956 | 143 405 / 141 377 | 29 701 / 27 673 | — |

**The control reproduces §13.7 on the current build** (settled per-band 1735–1780 vs the
recorded ~1720; full-window EWL 33.1 % → settled 3.3 %, against the recorded 32 % → 5 %),
so the contrast is sound and the earlier worry that a newer build had moved the headline
does not apply here.

**Read the counters before the grade (invariant #4).**  The uncalibrated arm flagged
140 126 times over 480 epochs against 960 carried DDs — ~292/epoch, a ~30 % turnover of the
whole carry every epoch, driving births 3 984 → 143 405.  That is not a gate screening a
runaway; it is the carry being dismantled and rebuilt continuously, so its apparent EWL win
(3.3 % → 11.2 %) is the carry getting OUT OF THE WAY — the arm regressing toward the float
twin that already beats it ~200×.  The loader's own posture ledger predicts exactly this:
raw formal Cov(v) is 10–30× optimistic (M2), so `innov_k = 4` is correct ONLY with
`innov_calibrate`.

**Calibrated, the win evaporates.**  5.4× fewer flags (25 709), and settled EWL integer
recovery falls back to control — **3.6 % vs 3.3 %** — while per-band med\|e\| improves only
15.6 % (1780 → 1503) and stays **~160× worse than the float twin**.  The gate is live, not
dead wiring (27 673 refloats, 14× the control), and it still does not touch the runaway.

⇒ **The runaway is a property of the estimator on this posture, not an artifact of the
missing D5 gate.**  §13.7's "do NOT arm the trio posture for settled work" and the §13.8
rule-5 injection freeze both stand unchanged.  What this does retire is one candidate
mechanism: the mechanism card owed by §13.7 should not spend another arm on the innovation
gate.  Note also that arming D5 here would have looked like a large win to anyone who read
the grade without the counters.

### 13.8 GRADING RULES AND TRAPS for the triple/slant posture
### (absorbed 2026-08-15 from `docs/HANDOFF_slant_triple_next.md`, now attic'd)

That handoff was the work order for §13.6 and §13.4 ("§3 + §4 EXECUTED 2026-08-12 — the
measured record is SPEC §13.6"); the plan is dropped and the method rules, which existed
nowhere else, are carried here.  Anchor map for its referrers: **its §1 → §13.8.0** ·
**§2 (the 1 h rule) → §13.8.1** · **§3 → §13.6** (the executed record) · **§3.2 → §13.8.2**
· **§3.3 → §13.7** (the trio arm ran and DIVERGES) · **§4 (the sweep) → §13.6**, closing
paragraph · **§5 → §13.8.4** · **§7 → §13.8.3**.

**13.8.0 The posture-C build inventory** (the table `project_family_gf_datum_evidence`
cites as "§1 lists every existing piece").  Posture C = triple frequency + free slant
ionosphere + geometry-free code-bias datum; config
`config/test_suite/_simnet60_m_c_tri_slant.json`, fixture `runs/simnet60_tri` (60 solve + 1
consumer, 480 epochs @ 30 s, GPS C1C/L1C + C2W/L2W + C5Q/L5Q, injected synthetic L5 OSB).

| piece | where |
|---|---|
| third sim band (opt-in, byte-identical off) | `signals.gps_l5` / `signals.galileo_e5b` / `signals.synth_osb_amp_m` (§13.1) |
| GF code-bias datum (Odijk rd 3b) | `network.code_bias_gf_datum` → `NetworkSbasis::buildCodeBiasDatumRows` (§13.3.2) |
| loader path for slant + W6 | the W6 chain force-enables `ionosphere_weighting` **only when the GF datum is OFF**; `ApplicationNetworkSetup` has the matching no-GIM branch |
| satellite bias σ | `network.trace_sat_bias_states` → `network_sat_bias_states.csv` |
| receiver clock + DCB σ | `network.trace_rx_states` → `network_rx_states.csv` |
| `cbpin=` S-line fill | `NetworkSolPrep` — declared but never populated since B6 (§14.6) |
| truth grader, slant-aware | `scripts/simnet_grade.py --gf-datum` (auto-detects) |
| exchange decomposition | `scripts/simnet_exchange.py` |

Release exe for the matrix: `build/x64-Release/gnss_processor_matrix.exe` sha256
`da2f9ac0…` — **re-copy AND re-hash after any rebuild; a renamed exe is a snapshot, not a
symlink.**

#### 13.8.1 ⚠️ THE 1 h RULE — the legal / not-legal lists

A 120-epoch arm is **1 hour, which is inside ignition for every level and calibration
channel on this fixture.**  §13.6 cites this list as **"1-h-LEGAL (§13.8.1)"** — the
absorbed handoff called it its §2.  The cautionary case is §13.3.2 → §13.5 (the 20-epoch
probe's "vf 1.886 = CALIBRATED" against the settled 8 756).

| | list |
|---|---|
| **LEGAL at 1 h** — structural / wiring questions decided in the first epochs and not converging | rank / gate behaviour; datum realization (are the pinned channels fixed?); which instruments fire; counter sanity; **the ambiguity-integer question**; QC tier opportunity counts; **any ratio between arms run on the same window** |
| **NOT LEGAL at 1 h** | variance factors; the ionosphere level; the exchange coefficient; "is it calibrated"; consumer verdicts; anything compared against the §13.5 settled numbers |

**Enforcement:** a 1 h number quoted must carry the window **in the same sentence**.  There
is no cost argument for grading a level channel on 1 h — the 4 h fixture exists and the
whole 480-epoch matrix ran in under five minutes (~3 min/arm at 60 stations).

#### 13.8.2 The truth-DD recipe, and the km-scale integer trap

Estimability first: Odijk Table 3 (CC-R) makes the estimable ambiguity a **double
difference**, `z̃^s_{r,j} = [z^s_{r,j} − z^1_{r,j}] − [z^s_{1,j} − z^1_{1,j}]` (r ≥ 2,
s ≥ 2), an integer **BY CONSTRUCTION IN EVERY POSTURE** — the iono parameterization
(Table 8) changes the biases and the iono, not the ambiguity's integer nature.  So "are the
ambiguities near integer" is a question about the **ESTIMATES**, not the model.  Under the
eq34 chain datum the realized basis is DD chords on a spanning tree rather than a single
pivot pair (`NetworkChainDatum.cpp`) — the same object.

| step | recipe |
|---|---|
| **estimates** | arm `network.trace_dd_fractional` (+ `trace_dd_fractional_stride: 1`, `trace_dd_fractional_max_pairs: 2000`; the real stride knob is `trace_dd_fractional_epoch_stride`).  Header `week,tow,cons,signal,sat_i,ref_sat,sta_a,sta_b,…` carries all four identifiers needed to reconstruct the truth DD |
| **truth** | `runs/simnet60_tri/<station>/truth_ambiguities.csv` (`satellite,signal,N_int,wavelength_m`; **90 rows = 30 sats × 3 signals per station**) plus `truth_slips.csv` deltas.  The true value at epoch k is `N_int + Σ truth_slips.delta_cycles` up to k — the file holds the **PRE-SLIP** integer (§8 trap 8).  This fixture is quiet (`rate_base` 0) so the sum is empty, **but write the code to add it anyway** |
| **the DD truth**, per (signal, sat_i, ref_sat, sta_a, sta_b) | `DD_true = [N(a,i) − N(a,ref)] − [N(b,i) − N(b,ref)]`, and the graded quantity is `DD_est − DD_true` **in cycles**, not its fractional part.  A fractional-part test cannot separate "off by 0.3 cycles" from "off by 47.3", and the field has only ever had the former |

⚠️ **THE TRAP: the integers are km-scale by construction.**  Under the legacy regime the
sim draws `N ∈ [1e5, 1e6)` per (station, band), so `N_int` ≈ 905 116 in the sample row.
**Do the differencing in Python ints BEFORE converting to float**, or you grade
floating-point cancellation.  The same km-scale draw is what makes zero-target cold
ambiguity pins inject km-scale tension (§12.8.1) — the defect §15.1's field-N regime fixes
at the fixture level.

#### 13.8.3 Trap register for this posture (do not re-litigate)

| trap | statement |
|---|---|
| **a datum change is also a GRADER change** | `simnet_grade.py` needs `--gf-datum` for the slant posture (auto-detected from the console line the loader now prints).  Without it the truth projection omits a direction and reports **2.3 / 3.7 m PHANTOM residuals** on channels datum-fixed to zero (§13.3.2) |
| **vf is deflated by s0²** | s0 varies **1.16 → 2.00** across postures.  **Never pool or compare raw vf across arms** without the correction; §13.5 quotes both |
| **never assert a bias SIGN verbally** | read the coefficient off the model or measure the slope, and say which.  Two conventions bit in one session: the absorbed IF lump is **−d_IF** (measured slope −1.08) and the exchange coefficient is **+**, derived from `resolveReceiverBias`.  Full statement §14.6 |
| **the COD product has NO GPS band-5 rows at all** | the third band's OSB truth is entirely the injected synthetic delta (§13.1) |
| **already measured, do not repeat** | station count is not the observability lever (the L2W gauge was n-invariant and is GONE at f = 3, §12.2/§13.3.1); `sbasis_complete` passes the gate and DESTROYS the ionosphere on a cold start (§12.2 rung 3); `cb_kaula_prior` **FALSIFIED** alone and paired (§12.8.3); the elevation-cutoff truth A/B **INVERTS** the field verdict — 15° beats 25° on the clock channels because the cutoff FEEDS the exchange (§12.9) |

#### 13.8.4 OWED — why the third band HURTS the clock and ZWD

**Open question, no hypothesis tested.**  §13.5 measured it in BOTH postures: A → B
degrades the clock contrast **0.773 → 1.721 m** (2.2×) and ZWD **0.549 → 0.873 m**, and
posture C does not recover either.  Three candidates:

1. the extra band adds **32 satellite phase biases + n·m ambiguities** against FLOAT
   ambiguities with no AR — more nuisance parameters than geometry-side information;
2. the synthetic L5 carries the same `code_sigma_zen_m` (**0.30 m**) as L1/L2 plus a
   hand-set `code_amp_gps_l5_m` (**0.015 m**) multipath amplitude, while real L5 is *quieter*
   than L1 C/A — the fixture may be **under-weighting its own third band**;
3. the synthetic L5 OSB (**0.60 m** amplitude) is larger than the product's L1/L2 OSBs on
   some satellites, so the third band may be dominating code-bias estimation.

The cheapest discriminator is a **third-band noise-realism sweep** (vary
`noise.code_sigma_zen_m` per band).  ⚠️ **It is BLOCKED on a sim change**: the `noise` block
is **band-common today**, so a per-band σ must be added first, following the same opt-in
byte-identity-off pattern the third band used.  **Do that before concluding anything about
f = 3 and clocks.**

#### 13.8.5 Reproduction (posture C)

```bash
python scripts/simnet_grade.py runs/simnet60_tri \
       --run runs/simnet60_m_c_tri_slant/run \
       --products runs/simnet60_m_c_tri_slant/products.txt --settle-frac 0.5

python scripts/simnet_exchange.py runs/simnet60_tri \
       --products runs/simnet60_m_c_tri_slant/products.txt \
       --run runs/simnet60_m_c_tri_slant/run --settle-frac 0.5
```

⚠️ In the slant posture the **a₀₀ channel DOES NOT EXIST** (no SH field), so
`simnet_exchange.py` has only the clock / rx-DCB / cb_rms channels.

**The 1 h arm recipe:** copy `_simnet60_m_c_tri_slant.json`, set
`processing.max_duration_s: 3570.0` (= 120 epochs), give it **its own
`product_output_path` and its own CWD**, and add the `trace_dd_fractional` block — the real
stride knob is `trace_dd_fractional_epoch_stride`.  Then grade in CYCLES with
`scripts/simnet_amb_integer.py` (§13.6), and obey §13.8.1.

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

## §15 THE EVENT-INJECTION BENCH — the G-B harness §6 called for
## (absorbed 2026-08-15 from `docs/SPEC_sim_injection_bench.md`, now attic'd)

**Status, 2026-08-13.**  Fixture N-regime LANDED + unit-tested + validated on a generated
fixture.  Slip-injection bench BUILT (arm generator, pre-flight, grader) and exercised
end-to-end on a 24-station / 120-epoch fixture.  Grader hardening (`simnet_grade.py`
product-record census) LANDED.  **RAIM re-host field gates 1–3 remain OWED — this bench
does not discharge them (§15.7).**

Companions: `docs/SPEC_raim_pool_rehost.md` (the gates), `docs/SPEC_family_gf_datum.md`,
`docs/SPEC_pivot_visibility_gauge_defect.md` (the defect §15.6's null arm discovered).
§15 is deliberately self-contained: a LIVE workstream (24 `_simnet24_inj_*` configs exist
against a record of the first arms only), so it can be split back out cheaply if it doubles.

**Anchor map for the absorbed doc's referrers** — its §1 and **§1.1/§1.2/§1.3 → §15.1**
(landed / draw properties / regime declaration, folded into one section) · **§1.4 →
§15.1's "The regime A/B" subsection** · §2 → §15.2 · §2.1 → §15.2.1 · §3 → §15.3 · §4 and
§4.1 → §15.4 · §5 → §15.5 · **§5.5 → §15.6** · §6 → §15.7 · §7 → §15.8.

### 15.1 The ambiguity N-REGIME — a fixture property that changes what an experiment can conclude

The simulator drew arc-constant integer ambiguities from `N ∈ [1e5, 1e6)`
(`src/sim/main.cpp`, the `ambN` lambda).  Not a cosmetic scale choice: it decides whether a
cold start that linearizes about zero begins **inside or outside its capture region**, and
every posture-C datum mechanism (eq34, `carry_invariant`, the seed round) targets zero.

**Measured, not assumed** — eight bundled IGS stations, DOY-129 2024, 1 h arcs,
Melbourne-Wubbena (so the estimate is iono-free and code noise averages down):

| quantity | field (IGS) | legacy sim | ratio |
|---|---|---|---|
| median \|N_WL\| = \|N1−N2\| | **28.5 cyc** | 268 127 | **9 400×** |
| median \|∇ΔN_WL\| (double difference) | **29.7 cyc** | 358 244 | **12 100×** |
| median \|N1\| (iono-biased, order only) | ~49 cyc | 505 000 | ~10 000× |

A geodetic receiver aligns its phase register to the pseudorange at lock, so the raw
ambiguity is O(10¹–10²) cycles.  **The sim was four decades wide.**

**Consequence, already paid for.**  On the 60-station bisection ladder **float beat every
eq34 arm by 34–852×** while the matched *field* arms passed structurally (480/480, consumer
float ~2.97 m).  The fixture could not grade the datum stack it was built for
(`project_eq34_bisection_ladder`: *"the 60-sta rung can only grade things eq34 does not
touch.  A field-realistic-N fixture is the fix."*)  The same km-scale draw is what makes
zero-target cold pins inject km-scale tension (§12.8.3) and what §13.8.2's Python-int rule
defends against.

**What landed.**  `include/sim/AmbiguityDraw.hpp` — the draw is now a unit-testable **free
function**, not a lambda inside a 3.5k-line synthesis.

```json
"ambiguity": { "regime": "field", "half_width_cycles": 48 }
```

| property | behaviour |
|---|---|
| `regime: "legacy"` (the **DEFAULT**) | `N ∈ [1e5,1e6)`, **byte-identical to every archived fixture**, pinned by `SimAmbiguityRegime.LegacyReproducesTheHistoricalExpression`, which re-implements the pre-extraction expression and compares over a **2 × 32 × 3 × 4** sweep |
| `regime: "field"` | draws `N ∈ [−W, +W]`, `W = half_width_cycles` |
| `half_width_cycles` WITHOUT `regime: "field"` | ⚠️ **HARD LOAD ERROR** — armed-but-inert is the failure this codebase pays for repeatedly, and here it would produce a config reading field-realistic beside a fixture still 1e5 cycles wide |
| **W = 48 calibration** | predicted i.i.d. medians ZD 24 / WL 29 / DD 38 vs measured field ~49 / 28.5 / 29.7.  **Verified on `runs/simnet24_inject_null` (25 stations): ZD 25.0, WL 30.0, DD 42.0, range exactly [−48, +48]** |

**Two draw properties must survive in EITHER regime** (both test-pinned; both easy to destroy
by "simplifying" the function).  **The station salt is load-bearing**: without it every
station receives numerically identical integers, so every between-station SD and DD is
**EXACTLY ZERO** — AR reports a **100 % fix rate on perfect integers while measuring
nothing**, invisible to every run gate; this matters *more* under the field regime, whose
narrow range makes accidental collisions look plausible (same defect class as §1.1 F2, from
the other side).  **Per-band draws are independent**: the pre-2026 formula made the wide-lane
`N1 − N2` the **constant −137 on every satellite**, so one MW solve generalized to the whole
constellation.

The hash is a **self-contained splitmix64 finalizer, not a shared-RNG draw**, so switching
regime moves the ambiguities **and nothing else** — noise, multipath and slip realizations
are untouched, which is what makes a regime A/B legitimate.  **The fixture declares its
regime**: `scenario_summary.json` gains an `"ambiguity"` block **only when the regime is not
legacy**, so archived fixtures regenerate byte-identically (the same rule `truth_slips.csv`'s
`source` column follows).  Without it a grader would have to infer the regime from
`truth_ambiguities.csv`, and an accidental mixed-regime A/B would read as an estimator result
rather than as two different datasets.

#### ⚠️ The regime A/B — measured, single-knob, and NOT yet explained

Same roster, same producer config, same seeds; the `ambiguity` block is the only difference
(its absence *is* the legacy knob).  24 solve stations, 120 epochs, float posture,
`scripts/simnet_amb_integer.py`, all-epoch window:

| channel | field med\|e\| | legacy med\|e\| | field round(e)=0 | legacy round(e)=0 |
|---|---|---|---|---|
| EWL(2,5) | 0.162 | **0.096** | 83.8 % | **94.1 %** |
| WL(1,2) | 0.942 | **0.452** | 30.0 % | **54.0 %** |
| L1C | 7.50 | 6.03 | 3.3 % | 7.8 % |
| L2W | 7.35 | 6.08 | 7.3 % | 7.0 % |
| L5Q | 6.94 | 5.99 | 5.9 % | 6.4 % |

**FIELD-REALISTIC INTEGERS GRADE WORSE ON THE LANE CHANNELS.**  Not a wrapped trace and not
a float-cancellation artifact: both arms genuinely track their own scale (legacy med
\|dd_cyc\| **4.2e5**, max 1.57e6; field **46.9**) and both dropped **0 rows** on the
grader's own `frac_cyc` consistency check.

**This does not weaken the fix and must not be read as one.**  The field regime exists
because it is *representative*, not because it is *easier*.  If field-realistic integers are
harder for the lane cascade, that is a fact about the estimator the old fixture was hiding —
the same category as §13.6's finding that the fractional-only instrument was concealing an
"off by 47.3".  Note \|frac_cyc\| median is **0.2583 (field) / 0.2489 (legacy) / 0.254 (real
IGS MW)**: **the fractional instrument reads the same in every regime while the whole-cycle
instrument separates them by 24 points.**

⚠️ **MECHANISM OWED — do not guess it into the record.**  Two untested candidates: **(a)
sampling** — ~9000 rows but only a few hundred independent arcs, so part of a 10-point gap
could be draw noise; **(b) a genuine narrow-range effect** — W = 48 gives only **97 slots
per band** and produces near-ties between arcs a 9e5-wide draw never produces.  **A matched
multi-seed repeat separates them cheaply** (vary the roster salt, hold everything else).
Until then this is a measurement, not an explanation.

### 15.2 The PRE-FLIGHT — do not inject at a dead mechanism

`scripts/simnet_injection_preflight.py <producer_config> [--target ...]` — offline,
config-only, flattened through the `extends` chain exactly as the loader does.  **Exit 0 =
every requested target LIVE; exit 1 = at least one INERT/DEGRADED.**

**Why it is mandatory.**  Under `network.carry_invariant` (Card 4) a documented set of QC
mechanisms is inert **BY CONSTRUCTION** — the producer banner says so (supersession clause
`GnssConfigLoadNetwork.cpp:6705`, §14.3): the invariant DD carry supersedes the PhaseBias
shared carry, the phase-leveling local carry, the RPB carry, and the eq34 pin-round / shift /
reref hygiene.  (⚠ An earlier draft cited `GnssConfigLoadNetwork.cpp:6548` for this banner;
that neighbourhood is the Eq-34 Card 3 single-receiver-chain banner instead — re-grepped
2026-08-15.)  Injecting
a fault at a superseded mechanism gives the worst outcome available: the run completes,
counters read zero, and the result is written up as *"the guard failed to catch it"* when
the guard was never wired.  **Paid for twice on post-fit RAIM and once on eq34 INC-2 (537
FRAMED re-hosts, byte-identical output, +419 s wall).**  ⚠️ **This is WORSE than standing
invariant #4**: there the counters are zero; here they can be **NON-ZERO AND SPECIFIC while
the actuation is inert.**

**Validated against the two known cases.**  On `_ft63_24h_conv.json` — the config
`SPEC_raim_pool_rehost.md` calls "the trap" — the checker independently reproduces the
diagnosis: `raim_postfit` **INERT** (two kills), `raim_within_epoch` **INERT**,
`slip_refloat_legacy` **DEGRADED**, `a4_withhold` **INERT**.  On the trio posture:
`raim_postfit` **INERT**, `slip_datum_station` **INERT** under eq34.

#### 15.2.1 A finding the pre-flight surfaced: A4 CANNOT FIRE on the gate-1/2 posture

`SPEC_raim_pool_rehost.md` **R1** warns that a "measure-only" arm is not emission-neutral
because σ̂₀ feeds **(a)** the A4 withhold and **(b)** the URA EWMA, and instructs a decision
before gate 2.

But A4 consumes `sigma0_adapt` (declared `NetworkSolveGlue.cpp:1103`, published at `:1777`),
computed **inside** `if (within_epoch_armed && carry_warm)` (`NetworkSolveGlue.cpp:1124` —
⚠ an earlier draft cited `NetworkSolveGlue.cpp:1102`, which was never the `if`; re-grepped
2026-08-15, same caveat about concurrent edits as §14.3) — **not** the post-fit seam's σ̂₀.
On the posture gates 1/2 prescribe (`carry_invariant` ON, `phase_leveling` OFF) `carry_warm`
is **false**, so **A4 CANNOT FIRE regardless of the re-host.**  R1 fires on
`raim_carry_invariant && raim_withhold_sigma0 > 0` without consulting `carry_warm`, so **it
warns on a posture where its (a) half is structurally unreachable.**

**Consequence for gate 2:** "does A4 start firing" is conditional on which posture the gate
runs in — with leveling OFF the emission-affecting surface reduces to the **URA EWMA
alone**.  Read `n_sigma0_withheld` and the URA column **SEPARATELY** rather than assuming
R1's (a) and (b) move together.  The pre-flight reports this explicitly rather than leaving
it to be rediscovered.

### 15.3 The ARMS

`scripts/simnet_inject_arms.py <null_fixture> <base_sim> <out_sim>`

**The null arm is synthesized FIRST and is an INPUT to the fault arm.**  A form-A scripted
slip fires only if that satellite is visible at that station at that exact epoch, and an
event that never fires is a **hard run-end failure** (`unfinished()`); on a global roster
most (station, satellite) pairs are below the horizon at any epoch — **measured 47 of 51 on
an earlier attempt.**  The generator reads the null arm's own `truth_visibility.csv` and
`truth_ambiguities.csv` and plans every event inside that intersection, so the null arm does
double duty: **false-alarm calibration AND the visibility oracle.**  Measured: **24 planned,
24 applied, zero `unfinished` failures.**

**Scripted slips touch NO RNG stream**, so the fault fixture is identical to the null
everywhere except at the injected `(station, sat, signal, epoch)` — what makes a differential
grade legitimate, and §9.6 is emphatic that G-B classes **must** be graded differentially,
because a headline recovery-floor table was RETRACTED when the null arm itself drifted
−0.464 m across the same window.

**The magnitude ladder.**  Default **±1, 2, 3, 5, 10, 50 cycles** spread across distinct
stations / satellites / epochs, so one fault run measures a **DETECTION FLOOR — the empirical
MDB — rather than a single yes/no.**  Plan-time constraints: one event per
`(station, sat, signal)`; **no two events sharing `(sat, epoch)`** (an attribution tier is
never asked to separate two simultaneous faults on one satellite); injections confined to
**`[0.35, 0.85]` of the arc**, past ignition and leaving a post-event detection window.

`--signal-mode mw-blind` injects the **same** delta on both bands of a wide-lane pair, which
cancels in the Melbourne-Wübbena combination and is **invisible to `mw_detect`**.  §9.7.6
measured exactly this null space and §9.7.10 showed `network.cycle_slip.if_phase_td`
catches it; the mode makes that discriminator reproducible on demand.

### 15.4 The GRADE, and the mandatory negative control

`scripts/simnet_inject_grade.py --null-run … --fault-run … --fault-fixture …`

**TRUTH COMES FROM THE FIXTURE, NOT THE PLAN.**  The join key is the fault fixture's
per-station `truth_slips.csv` rows with `source=scripted` — what the simulator actually
applied, at its resolved tow.  The injection manifest is read only as a cross-check; **a
planned event missing from truth is a BENCH DEFECT, reported as one and KEPT IN THE
DENOMINATOR, never silently dropped.**

| reported | detail |
|---|---|
| null-arm false alarms per tier | plus the `network_flag_rates.csv` observed-rate ÷ implied-α ratio — the null calibration the T3 anomaly needs (2.61e7× at 60 sta, 2.7e7× at 300 sta, both on truth-clean data) |
| fault-arm detection | per-injection match on `(station, sat)` within a latency window, latency in epochs.  **The signal is REPORTED but NOT REQUIRED** — a wide-lane detector legitimately flags the band pair (`detail = "L1C;L5Q"`) rather than the single band that jumped, and requiring a signal match would score a correct MW detection as a miss |
| differential tier counts | fault − null |
| the empirical MDB | detection rate per magnitude rung, and the smallest magnitude caught on every instance |
| run-end counters | scraped from the run log.  ⚠️ **NO CSV SINK** — both `NetworkRaimCiTallies` and `SlipQcCounters` are spdlog-only, and `write_run_manifest` carries neither `raim_carry_invariant` nor `_refloat`, so **an A/B harness currently cannot tell the two RAIM re-host arms apart from the manifest.**  Owed item in §15.7 |

**The mandatory negative control** (§6, G-B: *"an injected event that produces zero guard
trips is a WIRING FAILURE, not a quiet day.  The grader asserts this explicitly."*) is
implemented as a hard verdict with **its own EXIT CODE 4**, distinct from a detector that
ran and missed (**exit 0, reported PARTIAL** — a legitimate, gradeable power result).  On a
wiring failure the grader points at the pre-flight and **names the supersession class**.

### 15.5 Grader hardening — the EMPTY-PRODUCT trap

`scripts/simnet_grade.py` now censuses `products.txt` record types **BEFORE any channel
grade** and **REFUSES** a consumer verdict when **B, C and P are all zero**.  Records were
bucketed into per-epoch dicts and never tallied, so a product carrying header / epoch / S
lines and nothing else parsed cleanly, produced the expected epoch count, and reached every
channel as empty dicts that skipped silently.  On **2026-08-13 two arms of the eq34 bisection
ladder were in exactly that state**, their consumer read a flattering **0.037 m with nothing
applied**, and the conclusion *"seed_round breaks the consumer 0.037 → 585 m"* was
**PUBLISHED AND RETRACTED.**

| behaviour | detail |
|---|---|
| **refusal** when B=C=P=0 | **the numbers are WITHHELD DELIBERATELY** — a reader who can see them will quote them, and the failure mode is that they look *good*.  The `--json` payload **drops `err3d` / `up`** and carries `consumer_void_reason` instead, so a batch harness cannot bank the number either |
| **EXIT CODE 3** | the second condition in the file to move the exit code, after a missing `products.txt`.  A voided verdict is an input defect, not a grading opinion |
| ⚠️ **CAVEAT, not refusal,** when only *some* of B/C/P are absent | a clock-only product is a legitimate posture, but it changes what the consumer number means and an A/B spanning that difference must be labelled |
| two incidental finds | the parser silently dropped **480 `H` (heartbeat) records per run** (the docstring promised a `heartbeat` key that was never built), and orphan C/U/B/R/P records arriving before the first `E` are now counted |

**Verified both ways on archived arms:** `_e_eq34` (0 B/C/P) → **REFUSED, exit 3**;
`_ecis_rehost` → **unchanged, consumer 358.5474 m**, matching the recorded value exactly.

### 15.6 MEASURED — the first bench run

Fixture `runs/simnet24_inject_{null,fault}`, **24 solve + 1 consumer, 120 epochs (1 h @
30 s), field-N regime, float posture** (`_simnet60_m_c_tri_slant` template,
`phase_leveling` ON, `carry_invariant` OFF — **deliberately a posture the pre-flight clears
as fully LIVE**, so the bench itself was validated against a known-live mechanism before
being pointed at a contested one).  Release only, renamed exe.  **29 s per producer arm** —
the whole loop is minutes, as intended.

**Detection.**

- **22 / 24 attributable = 91.7 %**, and **0 of those 22 coincide with a null-arm flag** at
  the same (station, sat, window), so the rate needs no false-alarm discount.
- **Latency 0 epochs on all 22**; every match carried `signal_match = True` and
  `action = refloat` — the detector named the exact injected signal and acted on it.
- **Empirical MDB = 2 cycles**, the smallest magnitude caught on every instance; **±1 was
  caught 3/4.**  Differential tier counts fault − null: **T0_mw_detect +19, T1_if_slip
  +14**; run-end counters agree — null **32 detections / 0 refloats**, fault **51 / 19**.
- **The 2 misses**: a **+50 cyc on G20 L2W at 50° elevation** and a **+1 cyc on G17 L5Q at
  22°**.  The missed +50 is the interesting one and is a **POWER question, not a wiring
  one** — the bench's job is to hand that over cleanly, not to explain it.

**Truth recovery in whole cycles, null vs fault.**

| channel | null round(e)=0 | fault round(e)=0 | null med\|e\| | fault med\|e\| |
|---|---|---|---|---|
| EWL(2,5) | 83.8 % | 84.3 % | 0.162 | 0.151 |
| WL(1,2) | 30.0 % | 31.1 % | 0.942 | 0.888 |
| L1C | 3.3 % | 3.9 % | 7.50 | 6.88 |

**24 injected slips of ±1…±50 cycles produced NO MEASURABLE DEGRADATION in DD integer
recovery** — the fault arm is marginally *better* on every channel.  That is the response
mechanism working, graded against truth **in cycles rather than fractionally**, which the
field's own instrument (\|frac_cyc\| ≈ 0.25 uniform) structurally cannot do.

**Null calibration — the T3 anomaly reproduces a THIRD time.**  The null arm is **not
quiet**: **100 flags on truth-clean data** (`cycle_slips.rate_base = 0.0`, so there is not
one random slip in the fixture).

| tier | opportunities | flags | observed rate | implied α | ratio |
|---|---|---|---|---|---|
| T3 post-fit RAIM cell | 24 238 | 1 810 | 7.47e-2 | 1.973e-9 | **3.78e7×** |
| T3c RAIM consensus | 1 306 | 74 | 5.67e-2 | 1.973e-9 | **2.87e7×** |
| T4a attribution ARC | 24 238 | 50 | 2.06e-3 | 1.973e-9 | 1.05e6× |
| T2 within-epoch | 114 | 1 | 8.77e-3 | 2.700e-3 | **3.25×** |
| T1 IF-phase slip | 25 269 | 0 | 0 | 1.244e-15 | — |

This **independently reproduces** the T3 implied-α anomaly at a third scale (**2.61e7× at
60 sta, 2.7e7× at 300 sta, 3.78e7× here at 24 sta**) and, newly, **under the FIELD ambiguity
regime** — so the anomaly is **neither a station-count effect nor an artifact of the
unrealistic integer draw.**  T2 at 3.25× is the only tier whose threshold is anywhere near
its own data.  **`T0_mw_detect` fired 36 times on a fixture with ZERO slips in it** — a
false-alarm population nothing else in the harness was counting.

#### ★ The null arm's anomaly was a REAL PRODUCER DEFECT — the discovery record

The T3 tail contained one coherent excursion: **G15 at tow 303510–303570, `s_raw` up to
2214 simultaneously across six stations, on a fixture with ZERO injected faults.**  Chasing
it produced `docs/SPEC_pivot_visibility_gauge_defect.md` — which owns the root-cause record;
**this is the DISCOVERY record (the null arm found it), and the two are different facts.**

**Root cause, proven:** with `phase_datum_prioritized` OFF the network's shared phase-bias
set is `stations.front()`'s — and `stations.front()` **IS** the clock datum station.  When
the **PIVOT ALONE** loses a satellite, that satellite's phase-bias columns are removed
**NETWORK-WIDE** and the clock absorbs the released direction.  The sim truth is clean
through the whole event (windup smooth, satellite continuously visible), so it is
**entirely producer-side.**  **Causally confirmed** by a two-sided prediction test over
`clock_datum_station` — **3/3 at 24 stations including a clean negative control, then 6/6
at 60 stations** — with a **TRUTH-FREE DISCRIMINATOR: PB-only removal = avoidable defect;
PB+CLK+CB = the satellite genuinely left the network.**

Consequences for this bench: **electing a datum station that never loses a satellite cuts
the T3 false-alarm rate 7.47 % → 4.49 %** and the datum-free clock contrast **3.073 →
0.518 m**, so **~40 % of the null arm's T3 flags were downstream of an avoidable gauge
event** and the remaining 4.5 % is the threshold/population mismatch above.  **The bench's
own null arm was MIS-ELECTED**: `scripts/simnet_make_producer_config.py` warns only about
the *polar* fallback, and **visibility continuity is a second, independent criterion it does
not check** — `scripts/datum_gauge_audit.py` now does, and its `--run` mode works on field
runs.  This is the **first sim reproduction, with truth**, of a defect previously seen only
in the field as "G29 −44 m step, JPLM masked" — precisely the "does the response mechanism
work" question this bench exists to answer, arrived at from the **null arm** rather than
from an injection.

#### An error the bench caught in ITSELF

The first run planned **10 of 24 events on Galileo**.  The producer config solves **GPS
only** (its own POSTURE line reads "1 constellation(s)"), so those events landed in
observations nothing reads and **scored as detection misses** — dragging the headline to
**58.3 %** and making detection look magnitude-independent (a ±50 cyc "miss" that was never
observed).  Corrected, **GPS was 14/14 at every magnitude.**  **This is the supersession
error one layer out: INJECTING WHERE NOTHING IS LOOKING.**  Both halves are fixed —
`simnet_inject_arms.py --constellations` prevents it, and the grader detects it from the
emitted product's own satellite records and reports those events as **UNOBSERVED / excluded
from the rate**, never as misses.

### 15.7 What this bench does NOT discharge — THE single owed-list for G-B

**AN INJECTION PASS IS NECESSARY, NOT SUFFICIENT.**  The bench answers *"does the response
mechanism work"* on exact truth.  It does **not** answer *"does the event express on this
lineage"* — the field half, and the known **seeded-chain trap** where a mid-lineage
warm-resume seam measurably ERASES events under guards.  The field arms remain the
graduation gate for every sim-validated knob: `network.clock_datum_sum_to_zero`,
`raim_we_code_iono` first ON, `network.clock_datum_broadcast_anchored` + `emit_clock_disc`.

*(This list supersedes both §6's G-B brief and §9.6's closing paragraph.)*

| # | owed |
|---|---|
| 1 | **RAIM re-host field gates 1–3** (`SPEC_raim_pool_rehost.md` §7).  This bench exercises the **LEGACY** posture where the pool is `pl_amb`; the re-host onto `qc_amb` is what gates 1–3 test, and **gate 1 requires a 10-epoch `_ft63`-derived Release run with an OFF twin for byte-identity** |
| 2 | **A `carry_invariant` injection arm.**  Everything here ran on a posture the pre-flight cleared as fully LIVE, deliberately.  The interesting arm — slip injection under `carry_invariant` + `raim_carry_invariant`, where **detection is ungated but the ACTUATOR is the C3c funnel rather than the legacy refloat** — is the next card |
| 3 | **The other G-B classes.**  ⚠️ **CB step/ramp (the DOY-129 ratchet class) is currently NOT INJECTABLE AT ALL** — both bias channels are epoch-constant by construction.  Also not injectable: **receiver clock step**, **station outage**, **single-observation blunder**.  **Iono ramp IS injectable** via `iono.turb_*` / `tid_pulses`.  **Only the satellite-clock class and slips exist today** |
| 4 | **A CSV sink for the run-end counters**, plus `raim_carry_invariant` / `_refloat` in `write_run_manifest`, so an A/B harness can identify arms without scraping spdlog (§15.4) |
| 5 | **Trio-divergence stays OUT.**  Per standing instruction, **nothing is injected into the EWL-trio posture until the ~1720-cycle runaway (§13.7) is explained**; anything injected there is confounded.  `_simnet60_tri_slant_trio_480_noewl` is the existing control |

### 15.8 Reproduction

Release-only (invariant #2), renamed exe (concurrent-build lock); build per CLAUDE.md's
Build section / `scripts/vs2026_build.ps1`.

```bash
# 1. null fixture (also the visibility oracle)
python scripts/simnet_make_roster.py config/sim/simnet60_tri.json \
       config/sim/simnet24_inject.json --n 24 --consumer
#    then set trajectory.n_epochs and ambiguity.regime = "field"
build/x64-Release/gnss_simulator.exe synth-network runs/simnet24_inject_null \
       --sim-config config/sim/simnet24_inject.json

# 2. fault arm, planned against the null arm's visibility
python scripts/simnet_inject_arms.py runs/simnet24_inject_null \
       config/sim/simnet24_inject.json config/sim/simnet24_inject_fault.json \
       --ladder 1 2 3 5 10 50 --repeat-per-rung 2 --seed 7
build/x64-Release/gnss_simulator.exe synth-network runs/simnet24_inject_fault \
       --sim-config config/sim/simnet24_inject_fault.json

# 3. matched producer configs
python scripts/simnet_make_producer_config.py runs/simnet24_inject_null \
       config/test_suite/_simnet60_m_c_tri_slant.json \
       config/test_suite/_simnet24_inj_null.json \
       --products C:/GnssProcessor/runs/inj_null/products.txt --datum-station S12000SIM
#    (repeat for fault)

# 4. PRE-FLIGHT before spending run time
python scripts/simnet_injection_preflight.py config/test_suite/_simnet24_inj_fault.json \
       --target slip_detect raim_postfit raim_within_epoch

# 5. Release runs only, renamed exe, one CWD per arm
build/x64-Release/gnss_processor_inject.exe config/test_suite/_simnet24_inj_null.json
build/x64-Release/gnss_processor_inject.exe config/test_suite/_simnet24_inj_fault.json

# 6. grade
python scripts/simnet_inject_grade.py --null-run runs/inj_null --fault-run runs/inj_fault \
       --fault-fixture runs/simnet24_inject_fault \
       --manifest config/sim/simnet24_inject_fault.json.injection.json
# truth recovery in CYCLES (not fractional) is the existing integer grader:
python scripts/simnet_amb_integer.py runs/simnet24_inject_fault --run runs/inj_fault
```

---

## §16 THE 60-STA CAMPAIGN — the first long / multi-GNSS / realistic-slip grade
## (2026-08-20; full record `docs/SPEC_slip_relevel_walk.md`)

Fixtures `runs/simnet60_campaign` (2 486 natural slips) + `runs/simnet60_quiet`
(quiet twin), 60 sta + consumer, GPS+GAL **triple+triple**, field-N, 720 ep,
post-`1b9e157`.  Headlines, recorded here so §-local readers see them:

- **The §9.5 "confidently wrong" class now has a MECHANISM**: at a realistic
  slip rate every refloat re-levels from one epoch of code and freezes the
  result — the level channels random-walk √t to ~6.5 m ZWD / 14 m clock
  contrast over 6 h while σ̂₀, the rank gate, A4, and invariant-#7's R all stay
  silent.  Six-arm elimination ladder + the quiet twin (ZWD 0.15 m,
  CONVERGING) pin the cause to slips; iono posture / arc persistence /
  coupling stack / Galileo exonerated.  QUEUE item 9 (ranks first).
- **Slip DETECTION passes at scale**: 94.1 % detected + 3.5 % responded of
  2 486 (new `scripts/simnet_slip_census.py`; silent 2.4 % are mask-edge
  arc-ends).  The response's re-coupling edge is the defect, not detection.
- **`raim_arc_persist_epochs` degenerates to ÷K** on this error class (the
  per-LOS iono error is a LEVEL, so arcs re-trip) — its spike premise is
  falsified here.
- **QUEUE 8(a) verdict (PERSISTS)** and its Table-8 split collateral live in
  `QUEUE_principled_replacements.md` §8; grader
  `scripts/simnet_iono_interaction.py` (the §7.8 decomposition, promoted).
- Posture facts at 60 sta: GF pin holds the code channel at p50 4.9 cm;
  GPS-only is WORSE than GPS+GAL; the coupling stack ATTENUATES refloat churn.
- Wire hygiene: 720/720 with 720 H lines (the RTA-10 pre-open H-buffer fix
  landed this session); Card-1f orphan attribution live (88 orphans, all PB).

---

## §17 THE DASHBOARD — one-command HTML report over a graded simnet run (2026-08-31)

The workshop/handover artifact: everything §4 grades and §9/§16 narrate, in one
self-contained offline HTML (the exam-dashboard shape, network edition).

```powershell
venv\Scripts\python.exe scripts\simnet_report.py <fixture_dir> --run <run_dir>
# -> <run_dir>\simnet_dashboard.html      (defaults; --skip-grade reuses artifacts)
```

Two steps, chained: (1) `simnet_grade.py` writes its verdict INTO the run's
`logs/` — `simnet_grade.json` (machine-readable), `simnet_grade.txt` (the full
console readout, verbatim — the caveat text is part of the verdict), and
`simnet_grade_series.csv` (`--series`: per-epoch aggregates of the SAME
residuals the grader grades, emitted from the same transform code path so the
plots cannot drift from the numbers; clock/phase/ZWD/rx cover the full run with
a `_settle` marker row, code-bias covers the settled window only, and the
collector provably perturbs nothing — console+JSON byte-identical with and
without it).  (2) `pyviz/run_report.py` renders; three `simnet_*` viewers key
on those artifacts (auto-skip on every other run) ahead of the standard
network viewers:

- **simnet_scorecard** — the grader's verdict displayed, never re-derived,
  with the verbatim readout appended (census-first, void-consumer, REFUSED
  channels all surface exactly as the grader printed them).
- **simnet_theory** — the formulations behind the numbers, offline mathtext:
  the uncombined model + rank defect (Odijk Eq. 24), the realized S-basis wire
  contracts (C/B/P estimable lumps, pivots/cbpin/gfpin quoted from THIS run's
  S line via the JSON), and truth-projection grading incl. why contrasts
  answer the calibration question backwards (§4.1).
- **simnet_truth_series** — per-epoch rms truth error beside the published σ
  per channel (the §9.5 formal-σ-collapse exhibit, on a time axis), per-station
  ZWD error (the §9 determinability exhibit), rx DCB contrast vs level (the
  Table-8 gauge labelled as such), and phase SD med|frac| with the 0.25-uniform
  reference.  The settle boundary is drawn on every axis; left of it is shown,
  never graded.

Validated 2026-08-31 on `camp60wu_quiet` (quiet twin: reads healthy — ZWD p50
2.5 cm reproduced), `camp60wu_f1` (campaign free-epoch champion), and
`camp60wu_f5c` (stale-arm demo: the exhibit shows the metre-class walk loudly).
Viewer build-tests: `pyviz/tests/test_simnet_viewers.py`.

**FLAGSHIP (2026-08-31): `runs/camp60wu_quiet12h`** — the 12 h quiet arm built
for the workshop.  Fixture `runs/simnet60_quiet_wu93_12h`
(`config/sim/simnet60_quiet_12h.json`: the quiet twin's recipe with n_epochs
1440; 12:00 → 23:59:30 of DOY 129, so observations stay inside the day and the
SP3/CLK tail interpolates from the day-130 neighbours already in `data/`).
Producer config `config/test_suite/_camp60wu_quiet12h.json`, generated by
`simnet_make_producer_config.py` from the `_camp60wu_quiet` template
(`--datum-station S30000SIM`; only `max_duration_s` and the product path differ
from the validated posture; rinex paths absolutized — the generator copies the
manifest's RELATIVE paths and the producer runs from its own CWD).  Run:
1440/1440 emitted, 0 skipped/withheld, 26.4 min loop.  Settled half (6 h):
ZWD rms 0.058 m (p50 2.1 cm, vf 64), code bias p50 1.4 cm (vf 2275), consumer
3D rms 0.49 m; phase SD med|frac| ~0.25 (the known per-band no-integer-info
state); clock level wander ~1.9 m rms against cm-class σ — the exhibit.
⚠ Renamed-exe copies must live IN `build/x64-Release/` (the exe resolves
netcdf.dll etc. beside itself; a copy in the run dir dies on load).

**FLAGSHIP UPDATE (2026-08-31, same day):** the churn investigation the first
flagship triggered landed R2 (SPEC_pivot_visibility_gauge_defect §5.09-§5.09.2).
`runs/camp60wu_quiet12h_r2` is the new workshop flagship — same fixture, single
mechanism vs the baseline (composed continuous handover, border-exact 1e-4
ceiling): churn −98.5 %, consumer settled 2-2.5× better (3D med 0.16-0.19 m),
clock walk / AR wall unchanged, cb/level lock-in tax named (LV-6).  Present the
baseline beside it — the pair is a controlled exhibit of response-inflicted
churn vs frame survival.  Both dashboards: `<run>/simnet_dashboard.html`.

**S0 ARM (2026-08-31, `runs/camp60wu_quiet12h_r2q3s0` = r2q3 + clock_datum_sum_to_zero —
its SECOND sim validation, 60 sta / 12 h):** the producer clock channel is
transformed — residual rms 2.06 → **0.377 m** (−82 %), datum-free contrast 1.92 →
0.518 m (−73 %), vf −94 %, hourly level FLAT at ~0.3 m from h1 (no h0-4 walk, no
h4-6 pivot staircase — zero pivot events, anchor datum-decouples 214 → 15, the
first-generation pivot seam is structurally gone).  Grader support landed: the
S line's positional `isb_datum` field is read (1 = sum-to-zero) and the clock
truth transform zero-means per (constellation, epoch set); byte-inert on
pivot-datum products; live-tail torn-row reads hardened.
**⚠ VERDICT: NOT PROMOTED — the level bill moved to the consumer's channel.**
The unobservable common level must park SOMEWHERE until an external authority
exists (LV-6): the pivot datum parks it in the satellite clock, which a
consumer ABSORBS into its own receiver clock (r2q3 held a ~2 m clock level and
still positioned at 0.13 m); sum-to-zero evicts it into the code bias (rms
+38 %, p50 2.8 → 4.6 cm, vf ×6, ZWD +21 %), which the consumer APPLIES — 3D
rms 0.20 → 0.87 m, convergence snap h5.1 → h10.0.  **The datum choice decides
WHO PAYS for the level, not whether it exists** — the sharpest exhibit this
fixture has produced, and the concrete producer-side case for the LV-6
per-epoch level authority.  Keep S0 for clock-product/exhibit postures; the
consumer flagship remains `_r2q3`.

**BCA ARM (2026-08-31, `runs/camp60wu_quiet12h_r2q3bca` = r2q3 +
clock_datum_broadcast_anchored + apriori_clock_anchor_sigma_m 3.0 — the
loader-required pair under this bed's apriori_source=sp3, i.e. the
EXTERNAL-CLOCK HOLD; on this fixture the a-priori IS truth (C-channel truth
identically 0), so the anchors park the level in the truth frame and the
clock channel is σ-bounded partly BY CONSTRUCTION — the graded channels are
the consumer and the applied products, per the loader's own warning):**
1440/1440 emitted, 0 withheld; smoke 120/120.  Datum realized as per-sat soft
anchors (declared_dim=0, rank gate clean, pivot rows skipped 1396 → 0), and the
pivot seam is structurally gone exactly as under S0 (cta_decouple 214 → 15,
clock-pivot handovers 4 → 0).
**The finding is a clean TIME-SPLIT, and both halves matter:**
- **Ignition (h0-h6): the biggest first-half consumer win of the campaign.**
  Snap (sustained fwd-1h-median < 0.5 m — definition re-derived this session;
  ladder column base 8.78 / r2 4.46 / r2q3 4.37 / s0 8.44 h) drops 4.37 →
  **1.01 h**; hourly consumer 3D rms h0-h5 {0.72, 0.98, 1.31, 1.25, 1.04,
  0.47} → {0.55, 0.49, 0.30, 0.31, 0.40, 0.13} (2-4×); worst excursion 1.06 →
  0.51 m.  ZWD converges faster too (h3: 0.31 → 0.17 rms).  Mechanism: the
  anchor supplies the clock frame during exactly the window in which the
  network's own datum is still converging — the r2q3 ignition plateau IS
  producer frame wander, and an external hold deletes it.
- **Settled (h6+): NOT PROMOTED as flagship — the S0 lesson, third instance.**
  The per-epoch anchor is an information FLOOR the settled network cannot
  average past: consumer 3D rms 0.204 → 0.280 m (p50 0.128 → 0.253, floor
  wandering 0.17-0.38 by hour, never reaching r2q3's 0.11-0.13); the level
  bill lands in the APPLIED channels — cb p50 2.8 → 4.6 cm (+67 %, the same
  bill as S0's 4.6), cb vf ×6.8, and ZWD stalls at a 0.12-0.18 m rms floor
  (h11: 0.026 → 0.138, ×5) instead of converging.  Datum-free clock contrast
  p50 0.195 → 0.383 (rms +7 %).  Evicting the level from the sat-clock
  channel — even into a TRUTH-quality external frame — sends it to channels
  the consumer applies.
Side findings: (a) per-sat settled clock means carry the same metre-class
tails in BOTH arms (G25/G26/G12; sd over sats 1.90 vs 1.70 m — a fixture/
posture property, NOT a bca defect; anchor-vs-IF-lump tension measured nil,
corr +0.06).  (b) Post-fit QC volume collapses under bca — T3 flags 11110 →
17, T4 arc 699 → 2 on identical opportunity counts (1.3 M) — consistent with
the pivot-frame re-referencing churn being the flag source (the chord-churn
result), but it moves the tier's effective operating point ~650×: re-verify
MDB on a FAULTED bed before trusting T3/T4 under any anchor-free datum.
Derived follow-up (NOT built): a σ-SCHEDULE (bind in ignition, release as data
information overtakes) — equivalently, warm-start-shaped: the snapshot-resume
candidate should buy the same first half with no settled tax.  Ledger row
updated (`network.clock_datum_broadcast_anchored`): scoped regime now includes
ignition/bootstrap service windows; stale brdc_rts-only interlock corrected.

**Q3 q-LADDER ARMS (2026-09-01, `runs/camp60wu_quiet12h_r2q35` q 3e-5 /
`_r2q37` q 3e-7):** full record in SPEC_clock_temporal_anchor §9.1.  Headline:
**3e-6 is a consumer LOCAL OPTIMUM confirmed from both sides** (rms 0.209 /
0.204 / 0.240 across the rungs); the cb bill is stiffness-independent (prices
ARMING, not q); 3e-7 mints the first quiet-bed nis trips (5) exactly at the
§8.1 floor-crossover rung and doubles ZWD rms on their shock tails; clk-σ
overconfidence grows monotonically with stiffness.  The q choice is CLOSED on
this bed.

**WARM-START ARMS (2026-09-01, the bca-derived first-half candidate —
`runs/camp60wu_q6wsnap` (w) / `_q6cold` (b) / `_q6warm` (c), flagship posture,
one build):** (w) = flagship stopped at 5h59m31s + RTB-7 snapshot writer —
**products PREFIX-BYTE-MATCH the 12 h flagship** (snapshot write proven
outputs-only; the snapshot IS the flagship at hour 6).  (b)/(c) start at hour
6 exactly (`start_offset_hours 5.9958333`, the 15 s half-epoch back-off); (c)
warm-resumes a COPY of (w)'s snapshot under `state_snapshot_require` (age
30 s vs the 120 s horizon; fingerprint accepted; generation id PRESERVED;
restored 369 D2 shared-carries, 2751 leveling ambiguity carries, 2/2 pivots,
emitter state, n_emitted 720 ⇒ carry stays warm — and **0 ZWD carries**: with
`network.carry_zwd` false this lineage's ZWD memory lives only in the
in-process station filters, which the snapshot structurally cannot carry).
**The verdict decomposes the first-half problem into three parts:**
1. **Hour 1 is CONSUMER-self-convergence-bound** — warm ≡ cold in the first
   mission hour (3D rms 1.145 both, max ~4.3 both) while the continuous
   flagship's consumer sat at 0.41: no producer-side fix can buy hour 1; the
   consumer-side warm start (the RT filter-state snapshot card) is that half.
2. **The producer frame IS the plateau, and the warm resume buys it** —
   mission-start snap 3.15 h (cold) → **0.38 h (warm, −88 %)**; h7 rms 0.374 →
   0.211; seam continuity: warm's first-epoch same-TOW clock delta vs the
   flagship is ~40 cm median vs cold's ~95-120 cm (3× — not carry-precision,
   because station states and cross-covariances do not travel).
3. **As-built it is PARTIAL, and the stiff half actively drags the soft
   half** — ZWD re-converges from scratch in BOTH arms, and the warm arm is
   SLOWER late (absolute-h11 ZWD rms: flagship 0.026, cold 0.095, warm
   0.173): the restored zero-q ambiguity carries were leveled under the old
   ZWD trajectory and retard the fresh station filters' re-convergence — the
   invariant-#1 drag shape, measured.  The arms CROSS: by h10-11 cold ties or
   beats warm on the consumer (0.207/0.335 vs 0.280/0.260), and the common
   settled window h9-12 reads flagship 0.143 / warm 0.278 / cold 0.379 m rms
   — the warm resume recovers under half of the settled gap.
**VERDICT: scoped to the designed RTB-7 use (service bounce: generation
continuity, no consumer reset, snap −88 %) — NOT yet the full first-half fix.**
Completion path, in order of leverage: (i) arm `network.carry_zwd` (+
`carry_iono`) in BOTH writer and resume postures so tropo/iono memory moves
into the snapshotable carry seam — a solve-affecting posture change with its
own A/B owed (it alters the flagship, so it re-bases the ladder); (ii) the
consumer-side warm start for hour 1; (iii) evaluate `state_snapshot_resume_shock_m`
(RE-2, default 0) against the measured 40 cm seam.

**RELEVEL-FREE-EPOCH OFF-CHECK (2026-09-01, `runs/camp60wu_quiet12h_r2q3rfoff`
= r2q3 with `cycle_slip.relevel_free_epoch` FALSE, single knob):** the
pre-registered near-null FAILED in the informative direction — **OFF costs
+29 % settled consumer (3D rms 0.204 → 0.263, p50 0.128 → 0.166) and +17 %
ZWD rms on the ZERO-slip bed.**  The detection layer is identical in both
arms (159 MW detections, 16 refloats; event census flat; clock channel flat),
so the delta is pure RESPONSE policy: the ON arm's run-end counters show 1608
detector-marked free-epoch latches with **1514 inject-site skips taken** —
stale carried priors NOT folded at arc re-latches — and the OFF arm folded
them.  ⚠ The latch population (1608) is 10× the slip count (159): the knob is
NOT a slip response, it is the guard against stale-carry injection at EVERY
arc re-latch, and the quiet bed pays for its absence through the same
invariant-#1 channel the slip campaign named.  Verdict: the template pin is
retro-validated with an absolute grade and its scope WIDENS from "slipped
postures" to this producer posture generally (ledger table row updated).

**RT FILTER-STATE SNAPSHOT (CONSUMER WARM START) — BUILT; THE SIM CO-RESTART
VENUE IS MEASURED ZERO-WIN, AND HOUR 1 IS RE-ATTRIBUTED (2026-09-01, the
"consumer-side warm start" completion candidate; exe `gnss_processor_fable_cw`,
runs `camp60wu_q6wsnap_cw` (w'') + `camp60wu_q6warm_cw{,3,4,5}`, configs
`_camp60wu_q6w*_cw*.json`):**
**Built.**  The RTB-7 complement: `realtime.state_snapshot_path` arms a
periodic atomic snapshot of the RT/consumer engine's OWN Kalman memory —
core block + every dynamic state (exact-layout replay via the new
mutation-free `StateManager::restoreDynamicState`; `getStateSize()` is
map-derived, so refusals must not append) + full covariance + the SSR
continuity baselines (generation id, `noteSsrDiscontinuity` memory, A5
applied maps, epoch counter) — and a staged warm RESUME (parse at
initialize, age+fingerprint gates at the first processed epoch; the restored
prev-epoch time makes the first predict inflate over the true gap).  New
seam `RealTimeEngineStateSnap.cpp` + `RtStateSnapshot.{hpp,cpp}`; knobs
`state_snapshot_{path,interval_s,horizon_s,seam_refloat,restore_scope}`;
unit suite `RtStateSnapshotTest` (3 cases: exact round-trip, six
corruption rejections, layout-replay contract).  The closed-loop consumer
inherits the knobs through `deriveClosedLoopConsumerConfig`'s full copy.
Byte gates all PASS: (w'') writer arm — producer products + closed-loop
trace + network_zwd all BYTE-IDENTICAL to `runs/camp60wu_q6wsnap` with both
snapshot writers live; every resume arm — producer products byte-identical
to (c') (the consumer is passive).
**The five-arm ablation (consumer hourly 3D rms, h6 | h7 | h8-h11), against
a producer that ALSO warm-resumed (the (c') posture):**
cold (c') 1.068|0.258|0.33/0.15/0.13/0.12; full-STIFF (cw)
7.443|0.942|0.20/0.12/0.13/0.13; +amb-refloat (BIT-IDENTICAL to cw — this
consumer re-seeds ambiguities per epoch, so ambiguity stiffness was never
its channel); +ZWD-freed (cw3) 10.721|0.917 (the freed ZWD absorbed 2.9 m
before snapping back at the ~20-min DCB relax); core-scope-ISB-stiff (cw4)
9.237|0.907 (epoch-0 10.95 vs cold 4.46 — the ISB, q 1e-12, the stiffest
core state, isolated as the last liar); core-scope + ISB freed (cw5)
**≡ COLD to 3.4 mm max / 0.0 mm p50 per-epoch over all 720 epochs**.
**Three invariant-#1 instances in one card** (ambiguities → ZWD → ISB), each
isolated by its own arm: a producer that warm-resumes emits a deliberately
UNSIGNALLED product transient (gen id preserved, no disc bumps — the
seamlessness that protects cold consumers), and every stiff restored state
fights it — while every free-q state (position q 100 m²/s ⇒ σ≈55 m after
one 30 s predict; receiver clock) is forgotten before the first update.
**Memory either evaporates or fights; on this venue the best warm start IS
the cold start, and the card's default now guarantees exactly that**
(`restore_scope: "core"` + `seam_refloat: true` ⇒ measured cold-equivalent).
**Hour 1 re-attributed.**  The reference ladder's "hour 1 is
CONSUMER-self-convergence-bound" conflated two things: the consumer's own
bootstrap is ~4 epochs on this bed (free-q position; cold epoch-0 4.5 m →
t+90 s 0.7 m), and the h6-h7 rms (1.07/0.26) is the PRODUCER'S OWN
post-resume product transient passing through the applied products — which
no consumer-side state can absorb better than fresh states do.  The h6
lever is therefore PRODUCER-side (bind the resumed leg's first minutes,
bca-σ-schedule-shaped, or extend the seam-scoped re-seed family to the
producer's station filters) — future cards, not this one.
**Venue scoping (the card's real regime, validation OWED):** engines with
PERSISTENT cross-epoch memory facing a CONTINUOUS product stream — the
covl145-class rooftop replay (2.8 min of ambiguity/ZWD/bias convergence,
`project_rt_report_surveyed_reference`), turbo_edit/AR lineages, static
consumers with tight position q, and consumer-only restarts against a live
producer.  There `restore_scope: "full"` (+`seam_refloat` per continuity)
is the designed posture; arm it on that venue's own A/B before quoting any
benefit.  Ledger row `realtime.state_snapshot_path` carries the scoped
verdict.

**SEAM-SCOPED ZWD SERIALIZATION — BUILT + VALIDATED (2026-09-01, the card the
carry_zwd re-base arms derived (finding 3 below); `runs/camp60wu_quiet12h_r2q3_szv`
(a) / `_q6wsnap_szv` (w') / `_q6warm_szv` (c'), configs `_camp60wu_*_szv.json`,
exe `gnss_processor_fable_szv.exe` = sha256 `87d8629c…` beside its DLLs, runs
sequential each from its own CWD):**
**Design as built.**  (1) WRITE side, knob-free: any snapshot-armed run
(`state_snapshot_path` set ⇒ `NetworkKnobs::zwd_capture`) records the tropo
column per station (its own assembly block OUTSIDE `legacy_local_inject`, the
RAIM-pool-re-host pattern — live through warmup and on no-leveling lineages;
`ApplicationNetworkAssembly.cpp` capture block) and runs the existing B9
harvest/stash every processed epoch (`networkHarvestLocalCarries`, gate now
`zwd_on || zwd_capture || zwd_seam_live`) — `est.reconstructStation` +
`stationRegression`/`conditionalLocalVarianceDiag` are READ-ONLY, so every
RTB-7 snapshot serializes fresh Z rows with zero solve effect.  (2) RESUME
side, default-OFF knob `network.zwd_seam_reseed` (+`_epochs`, default 480 =
4 h): a warm resume that restores ≥1 Z row arms a countdown
(`NetworkCarryState::zwd_seam_ep_left`); while it runs, the UNCHANGED B9 fold
injects (gate `zwd_on || zwd_seam_live`, pivot included via the
`legacy_local_inject` OR) and the rolling stash refreshes — (cz)'s validated
mechanism with a retirement clock.  Both invariant-#7 edges are logged (ARMED
at resume / RETIRED at exhaustion) + run-end fold counters (invariant #4) +
manifest fields.  (3) Fingerprint: NO new token — the EIGHTH forward-only
response-policy exemption (`networkSnapshotFingerprint` comment): Z rows mean
the same thing armed or not, and a token would outlaw the same-snapshot
cold/warm A/B this validation ran; `zwd_on`/`zwd_q` stay fingerprinted.  The
horizon default sits past the ~3.1 h measured cold re-convergence and under
the ≥6-8 h double-memory onset, inside (cz)'s validated 720.  Unit contract:
`tests/test_zwd_seam_reseed_config.cpp` (suite ZwdSeamReseedConfig, 7 cases;
suite total 1554+1 skip).
**Gates, all PASS.**  (a) CONTINUOUS BYTE-IDENTITY: 12 h flagship-verbatim run
→ products.txt (34 524 993 B, sha256 `09da28d2…`) + closed_loop_trace.csv +
network_zwd.csv all BYTE-IDENTICAL to `runs/camp60wu_quiet12h_r2q3` —
cross-BUILD, so the golden stands un-rebuilt.  (w') CAPTURE-LIVE IDENTITY: the
q6wsnap pattern re-run with capture live (snapshots every 10 s, 60-station
stash every epoch) → the same three artifacts BYTE-IDENTICAL to the old-build
`runs/camp60wu_q6wsnap`; final snapshot = same epoch (tow 323970), SAME
fingerprint `35860c5f45b8a004`, now with **60 Z rows** (σ 3.8-8.2 mm — the
honest single-epoch datum-conditioned formal σ; (z)'s 0.9-1.5 mm was the
double-count-tightened one).  (c') SEAM: resume ACCEPTED (age 30 s, gen id
preserved, 369 D2 + 2751 amb carries as the reference (c) + the 60 Z rows);
window ran **28 800 folds = exactly 60 sta × 480 ep** (zero stale skips),
RETIRED on schedule at tow 338370.
**Verdict (hourly 3D consumer rms | ZWD truth rms, T0 302400):**
h6 1.068|0.124, h7 0.258|0.119, h8 0.329|0.119, h9 0.150|0.108,
h10 0.132|0.141, h11 0.119|0.199.  Mission-start snap (the campaign's pinned
forward-120-ep-median-<0.5 m metric, which reproduces every published ladder
value exactly): **0.11 h** vs (cz) 0.00 / no-Z warm 0.38 / cold 3.15 /
flagship-from-own-start 4.37.  ZWD does NOT restart — h6 0.124 beats
(cz)'s 0.172, the (z) ceiling's own 0.169, and cold's 0.390.  Settled h9-12
consumer **0.134 m rms (p50 0.125)** vs flagship 0.143 / (cz) 0.112 / no-Z
warm (c) 0.278 / cold 0.379 — **the reference ladder's "recovers under half
the settled gap" is now ALL of it, at or past the continuous flagship's own
level**, with the continuous run byte-identical instead of (z)-divergent.
Retirement costs the CONSUMER nothing (h11 0.119 vs (cz)'s still-injecting
0.134); the ZWD truth error relaxes h10→h11 (0.141→0.199) toward the
filter-own plateau — that late drift is the restored zero-q AMBIGUITY
carries' invariant-#1 drag (this section's part-3 finding, unchanged by this
card: (c) paid 0.173 at h11 with no seam at all), not a seam defect.  σ
honesty moves the right way: ZWD vf 0.206 (conservative) vs (cz) 1.232
(optimistic).  (c')'s own end-of-run snapshot carries 60 fresh Z rows — a
resume chain re-seeds at every seam.  **Still open, unchanged: hour 1 is
consumer-self-convergence-bound (h6 1.068 ≈ (cz) 1.039) — the consumer-side
warm start remains the other half of the first-half fix.**  Ledger:
`network.zwd_seam_reseed` row added; `network.carry_zwd` row + JSON twin
re-synced (stale duplicate absorbed).

**CARRY_ZWD RE-BASE ARMS (2026-09-01, `runs/camp60wu_quiet12h_r2q3z` (z) /
`_q6coldz` (bz) / `_q6warmz` (cz) — the warm-start completion candidate; NOTE
`carry_iono` was deliberately NOT armed, the 8(b) campaign falsified it):**
1. **The re-base is FALSIFIED for continuous runs.**  `carry_zwd: true` on
   the flagship (B9 at the resolved default q 1e-9 m²/s, mirroring the
   filter's own troposphere_q) is net-harmful on every applied channel:
   consumer 3D rms 0.204 → **0.723 m** (p50 0.128 → 0.660, snap NEVER, and
   DIVERGING at run end — h8-h11: 0.72, 0.64, 0.79, 1.09); ZWD converges
   monotonically but ~3× slower (h11 0.077 vs 0.026); cb p50 +32 % / vf
   +66 %.  Mechanism: the carry re-injects last epoch's ZWD posterior as a
   prior ON TOP of the station filter's own persistence — a structural
   information double-count that halves the effective q and cannot forget
   ignition errors at the filter's natural rate.  Not a q-tuning issue; a
   compensating retune would tune around the defect.  **KEEP_OFF.**
2. **The seam mechanics are PROVEN on the same arms.**  (z)'s hour-6 history
   snapshot carries 60 Z rows; (cz) restored all 60 (vs 0 on the r2q3
   posture) and its ZWD does NOT restart — absolute-h6 rms 0.172 vs the
   ceiling's own 0.169 vs cold's 0.390.  Mission-start snap **0.00 h** (vs
   coldz 3.12, and 0.38 on the r2q3-posture warm resume), and h8-h11
   consumer **0.088-0.134 m — true-r2q3-flagship-class, the best settled
   hours any arm has produced on this bed**.  (cz) even beats its own
   continuous ceiling: the resume rebuilds fresh station filters against
   h6-vintage carries, RESETTING the double-memory pathology clock — the
   resumed leg's success independently confirms finding 1's mechanism.
   Hour 1 stays consumer-bound (1.04/1.08 both arms — the r2q3 experiment's
   irreducible hour reproduced).  Seam frame continuity ~50 cm (vs cold's
   ~90-110), same class as the r2q3-posture resume.
3. **The derived card — BUILT + VALIDATED 2026-09-01, see the SEAM-SCOPED
   ZWD SERIALIZATION entry above: SEAM-SCOPED ZWD serialization.**  Write
   the station ZWD posterior into the snapshot UNCONDITIONALLY (read from
   the station filter at write time — no continuous-run carry needed) and
   inject it as a B9-style prior ONLY on post-resume epochs, retiring as the
   filter re-establishes its own memory.  Gets (cz)'s seam behaviour with
   the continuous run byte-identical to r2q3.  That card, plus the
   consumer-side warm start for hour 1, is the complete first-half fix.

**WE-CI ARM (2026-08-31, `runs/camp60wu_quiet12h_r2q3weci` = r2q3 +
raim_we_code_iono, + 1 h smoke):** the SAFETY half of this knob's first ON
validation, and it is maximal — armed and LIVE (code pool populated all day,
max σ̂₀ 1.85 vs trigger 10), **zero trips over 1440 epochs, and products.txt +
closed_loop_trace.csv + network_zwd.csv BYTE-IDENTICAL to the `_r2q3`
flagship under the same build** — the armed screen costs nothing when quiet.
Scoping discovery: the IONO pool max σ̂₀ read 0.00 — structurally EMPTY on any
`estimate_iono_sh: false` lineage (this bed included), so only the CODE half
is quiet-validated; the iono half needs an SH-armed lineage.  The DETECTION
half (the DOY-129 19:26 detonation warm-start) remains the blocker before
WE-CI joins the standard armed set (ledger row updated).

## §18 THE STABILITY CAMPAIGN — the settled state must stop moving (2026-09-01 →)

Successor phase to the §17 deep on/off campaign (7 verdicts, fd1d123→c799a8b,
plus the seam-ZWD/RT-snapshot session ed170b2/8da1de6).  The standardized base
is `_camp60wu_quiet12h_r2q3` (R2 sticky continuous handover + Q3 anchor q 3e-6
+ relevel_free_epoch + pivot clock datum + zwd_seam_reseed on resume postures);
targets, in priority order: (1) settled late drift, (2) producer post-resume
transient, (3) per-sat settled clock tails, (4) T3/T4 MDB on a faulted bed,
(5) LV-6 design.  One mechanism per arm; verdicts land here + ledger rows.

### 18.1 TARGET 3 — per-sat metre-class settled clock tails: ROOT-CAUSED
### (2026-09-01, census instrument on the 12 h bed; fix cards derived, not built)

Instrument: `scripts/simnet_sat_clock_census.py` — per-sat settled clock
residual, median-demeaned per constellation per epoch (datum-free, robust to
level wander), joined against fixture `truth_visibility` coverage, wire
presence (emitted vs truth-visible), and the producer's own URA.  Run over
`_r2q3`, `_r2q3s0`, `_r2q3bca`.

**The tails are not a fixture property and not "clock quality" — they are a
producer RE-COUPLING defect acting on the fixture's eclipse schedule:**

1. **Every metre-class tail sat has a ~55-min visible(≥10 sta)-but-NOT-emitted
   window**, and the 15 windows over 12 h form the classic eclipse staircase
   (sequential per orbital plane, ~1.8 h spacing).  The excluder is
   `exclude_eclipsing_satellites` (armed in the standard set; it logs no
   per-event line — the census infers the windows from wire-vs-visibility).
2. **The tail is a STEP exactly at the re-admission epoch, then constant for
   hours** (p50 ≈ rms ≈ max): G25 d −0.43 → −7.13 at h3.22 (±3 cm for the
   following 8.7 h), G12 +6.33 at h2.35, G26 −9.4 (own frame) at h5.9, G22
   +1.81 at h11.30 (mid-settled, the directly-measured case).
3. **Mechanism: the clock ↔ C5Q-code-bias split RE-REALIZES at re-admission.**
   Pre-eclipse G25 carried clock −0.90 m / b_C5Q −7.53 m — the ~7 m is a
   COLD-START third-frequency misrealization parked in the weakly-estimable
   C5Q channel (7 m from the injected ±0.6 m OSB truth) where it is
   consumer-inert.  Post-readmission: clock −7.62 / b_C5Q −0.42 — the
   misrealization RELOCATED into the consumer-visible clock, and the stiff
   states hold the new split for the rest of the run.  G12 shows the exchange
   with BOTH channels landing badly (clock +0.06→+6.42, C5Q +1.91→−4.89):
   the re-seed lands wherever the readmission epoch's estimability puts it,
   then freezes.  The applied C1C combination (clk + b_C1C) steps ~6.6 m.
4. **Silent on every armed instrument.**  URA/sigcc stays 4–7 mm through the
   step.  NO clock-disc mark despite `emit_clock_disc` +
   `clock_disc_jump_test` armed — the jump test compares successive emitted
   values and a 110-epoch absence leaves it nothing to compare: **GAP-BLIND**,
   the same shape as the wind-up accumulator's 300/900 s horizon defect.  The
   only event at the edge is `cta_recouple` (statistic 0.057 vs threshold 6):
   the Q3 anchor re-couples against the post-re-solve value — invariant #7's
   re-coupling edge validating the wrong thing, measured.
5. **Datum posture changes WHO PAYS, not whether.**  r2q3 and bca reproduce
   the same top-3 (G25 7.2/7.6, G26 6.9/7.0, G12 6.7/6.4 — the "fixture-
   common, both datums" reading from the bca session, explained).  Under bca
   the r2q3 pivot G01 itself shows a 4.1 m tail (it eclipses h4.7–5.6; under
   the pivot datum its own re-realization redistributes network-wide through
   the gauge).  Under s0 the per-sat tails collapse ~10× (G25 0.71, G12
   0.28): the per-epoch Σclk=0 constraint SMEARS the misrealization across
   the ~18-sat constellation (7 m/18 ≈ 0.4 m each) — the S0 lesson again;
   the bill moves to cb.
6. **Bridge to target 1 (hypothesis, 24 h bed will test):** GPS sats eclipse
   twice daily; the 12 h bed's rising h10–h11 contains the G14 (h10.69), G22
   (h11.29), E21 (h11.70) readmissions.  The settled consumer level-wander
   may be partly the superposition of these split re-realizations passing
   through the applied products.

Derived fix cards (NOT built; each is invariant-#7 work on the eclipse valve —
row added to SPEC_prefit_census_observability §13):
- **Gap-aware clock-disc marking**: the disc test must span an emission gap
  (compare against the pre-gap value with a gap-scaled band), so a
  re-admission step is MARKED and the consumer re-references instead of
  silently absorbing ~7 m.
- **Re-coupling-edge split re-validation**: on re-admission, re-validate the
  returning sat's clock+bias SPLIT against its pre-gap realization (or
  deliberately re-seed and mark), not merely the post-re-solve innovation.
- **The C5Q cold-start misrealization itself** (why 7 m parks in the third
  frequency at ignition) — LV-6 / level-authority family; without it the
  re-coupling fix merely keeps the 7 m where it is inert, which may be the
  correct product decision anyway.

### 18.2 TARGET 1 — settled late drift: instrument + 12 h re-analysis
### (24 h verdict PENDING — the arm is running as this lands)

Instrument: `scripts/simnet_drift_age.py` (commit 2306134) — hourly
consumer/channel/event decomposition + DD-truth-error-vs-chord-age join
(the simnet_amb_integer join; stations from the FIXTURE dir).  Join gate:
reproduces the flagship's published hourly consumer values exactly.

12 h re-analysis, before the 24 h bed:
- **h11 is partly product-day-edge artifact**: all 79 h11 `attrib_arc` events
  sit in the final 5 epochs (tow 345420–345540, many-sat = product-quality);
  h11 decomposes pre-zone 0.157 / edge-zone 0.200 / final-6-ep 0.269.  The
  real drift is h7 0.107 → h10 0.132 → h11-clean ~0.157.
- **The drift is a wandering UP-dominated BIAS** (hourly up-mean −0.073 →
  −0.111 → +0.008 → +0.108 → +0.145 m h7→h11, zero-crossing at h9 where 3D
  dropped back to 0.107; rms ≈ |mean|) — level-wander signature, not noise
  growth and not monotone aging.
- **No graded channel moves h7→h10** (clk residual/contrast flat, cb falls,
  network-median ZWD flat); settled per-epoch consumer 3D correlates +0.53
  rxdcb_contrast, +0.45 clk_contrast, −0.67 rxclk_contrast.
- **DD ambiguity truth error does NOT age**: med|e| by chord-age bucket
  3.57 → 3.45 → 3.32 → 3.13 → 2.96 → 3.16 cyc (0-10 → 481-960 ep) —
  flat-to-improving; traced population constant (~320–400k rows/h).

The 24 h bed (`runs/simnet60_quiet_wu93_24h`, `config/sim/simnet60_quiet_24h.
json`: 00:00:30 → 23:59:30 of DOY 129, single product day, final epoch = the
12 h fixture's exact trailing epoch) decides monotone-aging vs bounded-wander
with 15 clean settled hours, decorrelates run-hour from time-of-day (the
00:00 anchor), and carries the RTB-7 snapshot writer + per-epoch history so
the scheduled mid-run seam-reseed arm can branch from any hour.
