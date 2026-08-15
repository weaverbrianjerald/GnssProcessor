# SPEC — FAMILY-GF DATUM: per-(satellite, secondary-family) geometry-free pin

*Drafted + revised 2026-08-13 after a full anchor + adversarial recon pass against the
working tree.* **Status: BUILT (uncommitted), UNVALIDATED on data.**

| piece | state |
|---|---|
| §D.5 product-token half (independent session) | BUILT — unconditional realized-pin record (`this_epoch_if_pin_real` / `this_epoch_gf_pin_real`), per-emit `cbpin=`/`gfpin=` fill, wire writer/parser/legend/`checkContract`, grader per-CONSTELLATION transform + `multifamily_gf` refusal, round-trip gtests |
| §3.1 `channelFamilyOf` + §3.2 `runFamilyCensus`, §2 row block, §3.4 knob + interlock + banner, §2.1 time-link warning, run-latched census plumb, `'+'`-joined `gfpin=` producer fill + "partitioned but not pinned ⇒ declare NOTHING", §D.5 `row_ok` repair | BUILT — `tests/test_family_gf_datum.cpp` 13 cases; full suite **1384 pass / 1 skip / 0 fail** |
| §5 grader (per-FAMILY transform, `multifamily_emitted_no_token` screen, pinned-channel assertion, `MULTIFAMILY`-sentinel fail-open repair) | BUILT + numerically verified on a synthetic Q+X Galileo product carrying the full producer datum (IF + GF + per-family GF + s2z): **truth recovered to rms 5.9e−17 m over both families**, pinned-channel residual 0, single-family GPS unchanged at **6.4e−17 m**; the same product with a single-family token now REFUSES (`multifamily_emitted_no_token:P,X`) |
| §6 V0 (OFF byte-identity) | ✅ PASSED 2026-08-13 (§6.1) |
| §6 V1 (sim family-partition fixture), V2 (field arm) | ⚠ **NOT BUILT / NOT RUN.** The card has **NEVER emitted a family-GF row on data** — the ON path is unit-tests only, so every claim about what the rows DO is a PREDICTION. §6.0 governs acceptance: **gate closure alone will not settle it** |

*Mechanism measured 2026-08-12/13 (GPS+Galileo posture-C field campaign). **All code anchors
are WORKING-TREE lines** — the uncommitted token half drifted every `NetworkSbasis.cpp`
anchor off HEAD `8e5b04b` by +2 / +11 / +20 by region. ⚠ `runs/` was archived off `C:` on
2026-08-12 to `D:\GnssProcessor_archive\2026-08-12\runs\`. This card unblocks the single
largest coverage loss in the posture-C track: **113 of the 310-station DOY-129 roster (36 %)
contribute NO Galileo observations**, because their receivers track the X channel family and
the datum never reaches it.*

---

## §0 Context in one paragraph

Posture C (triple-frequency + free slant iono + GF code-bias datum) closes Odijk rank
deficiency 3b with one per-satellite GF pin on the IF-pinned pair
(`NetworkSbasis::buildCodeBiasDatumRows`, NetworkSbasis.cpp:620-674). The pair is selected
deterministically as the two lowest distinct-frequency code signals sorted by band digit then
full signal name (`selectIfPinPair`, def. NetworkSbasis.cpp:23-50, call :493) — for Galileo
with both families registered that is **(C1C, C5Q), the Q family**. The DOY-129 roster
partitions perfectly by receiver family — **187 stations pure Q (C1C/C5Q/C7Q), 102 pure X
(C1X/C5X/C7X), 11 X-without-E5b, 9 no-Galileo, ZERO bridging receivers** — so no X-station
observation touches a pinned channel, and rank deficiency 3b re-derives PER FAMILY: one null
direction per (satellite, X-family), μ-patterned, absorbed by the X stations' free slant iono.
The fix is **one GF row per (satellite, secondary family) and deliberately NO IF row** (§2).
The hard design question is whether the two families' emitted biases share one S-system (§D):
they do in the clock direction and do NOT in the iono direction, forcing a per-family product
token and a consumer mixing rule.

## §1 The defect, measured (2026-08-12, exe 44D7D965/02076F44)

| arm | result |
|---|---|
| `_pc305_gg_tri_slant_20m` (305-sta field, GPS+GAL both families) | **every epoch withheld**: gate "observed 4-7 (near-)zero ev below ~2.87e-04 but declared S-basis dimension is 2"; drop-and-heal tried 152 columns, healed 0 (no droppable column spans a ~20-dim null space). Durable record: the `_comment` of `config/test_suite/_pc305_ggq_tri_slant_20m.json` (committed c91a6ba) |
| `_pc305_gg_tri_slant_5ep_estdump` (+`trace_estimability`) | names the space: `n_shared 501, declared 2, n_below 22` = 2 per-constellation clock gauges (`CLK\|G*` uniform 1/√33, `CLK\|E*` 1/√23) + **~20 per-satellite directions with `CB\|E##\|C5X = PB\|E##\|L5X` and `CB\|E##\|C7X = PB\|E##\|L7X` moving together, band weights 1 : 1.793 : 1.703**. Trace `logs/network_estimability.csv` (2026-08-12 16:54) + copies in `runs/pc305_ggq_*/traces/` |
| roster scan, **OBSERVED** data, all 310 RINEX (re-measured 2026-08-13) | **185 pure-Q / 114 pure-X / 11 no-Galileo, ZERO observed bridges** (the draft's header scan was right by luck; see below) |
| `_pc305_ggq_tri_slant_*` (Q-only rerun ladder) | 40/40, 120/120, 480/480 at default tolerance — removing the X states removes the null space; the posture itself is healthy |

**The band weights are exactly μ.** μ_j = (f_E1/f_j)²: **E5a 1.7933, E5b 1.7032** — measured
1.793 / 1.703. ⚠ **E5a is 1.7933, NOT 1.7947** — (1575.42/1176.45)² = 1.79330; a reader
checking against 1.7947 finds a 0.08 % gap that is the document's, not the filter's. The
direction is Δd_jx = μ_j·δ (code AND phase together) compensated by ι → ι − δ on every
X-station LOS: Odijk rd 3b confined to the X subsystem.

> ### ⚠ DECLARED ≠ OBSERVED — the census substrate is load-bearing (measured)
> A **HEADER** scan reports **2 bridging stations** (`KOUC00NCL`, `NRMD00NCL`), which under
> §3.2's rule ("PARTITIONED when ZERO stations carry both") means **NOT partitioned, no rows
> emitted, and the 305-station arm keeps withholding every epoch** — the card inert on the
> exact arm it exists to fix. Both are **PHANTOM declarations**: on
> `data/2024-129/obs/kouc00ncl/KOUC00NCL_R_20241290000_01D_30S_MO.rnx` the header declares
> C1C/C5Q/C7Q alongside C1X/C5X/C7X, and across **22 564 Galileo satellite lines the
> non-blank counts are C1C 0, C5Q 0, C7Q 0** against C1X 22 564, C5X 22 469, C7X 22 441.
> `NRMD00NCL` is the same shape. Repo-wide over 310 files, exactly **2** stations declare
> both families and observe only one.
>
> **The census MUST derive from observation-derived state, never from RINEX headers or
> `signal_priorities`.** The implementation uses `StateManager::getSatBiasStates()`,
> allocated only from measurements passing `MeasurementModel::isUsableMeasurement`, and a
> blank RINEX field never becomes an observation (`RinexParser.cpp:672-674` skips empty value
> fields). On that substrate the observed bridge count is **0** and the card fires.
>
> **⚠ Residual fragility:** the blank-field skip is what protects this. A station writing a
> literal `0.0` instead of a blank WOULD be ingested under the default config
> (`rinex_skip_zero_values` defaults **false**, `RinexParser.cpp:685`), creating a phantom
> state and a phantom bridge that **silently disables the card**. The census line is the
> instrument — it prints the bridge count and station tallies every run, so a surprise "NOT
> partitioned" verdict names itself rather than presenting as an inert knob.

**The count closes, with a stated precondition.** 22 = 1 (`CLK|G*`, uniform 0.1731 ⇒
1/0.1731² = 33.4 ⇒ 33 GPS satellites) + 20 (X-family per-satellite directions) + 1
(`CLK|E*`, uniform 0.2072 ⇒ 23.3 ⇒ 23 Galileo satellites).

> **⚠ Population.** The constellation had **23** Galileo satellites but only **21** carried
> ≥ 2 distinct-frequency X channels — and it is that number, **not `n_E_sats`**, that sets
> the count. Acceptance formula: **`n_below = declared + n_X_sats − rank{1_{S_j}}`**, where
> `n_X_sats` counts satellites with a usable secondary-family pair this epoch and `S_j` is
> channel *j*'s satellite support. Measured: 21 − 1 = 20 ✓. Using `n_E_sats − 1` predicts 22
> undeclared directions where the dump shows 20, i.e. `n_below` = 24, not 22.

The per-(constellation, signal) sum-to-zero rows (span NetworkSbasis.cpp:475-482, rows built
:676-712) each impose Σ_s d_{s,j} = 0 over the satellites carrying channel j. On the null
family {δ_s}, channel j gives Σ_s μ_j δ_s = 0 ⇒ **Σ_s δ_s = 0 — the SAME single constraint
for every j**, so the three X-channel s2z rows collapse to ONE and precisely **21 − 1 = 20**
per-satellite directions survive: the dump's count.

> **⚠ Precondition — do not drop it.** That collapse yields n_sats − 1 **only when all of the
> family's channels span the SAME satellite set.** In general s2z removes `rank{indicator
> vectors of each family channel's satellite set}` directions, and the **11 XX− stations are
> exactly the population that can break it** (a satellite seen on C1X/C5X but never C7X gives
> C7X a different support). **§6's acceptance arithmetic must be computed from the run's OWN
> dump, never assumed to be n_sats − 1.**

**Why only the GF direction is null.** An X-station code row on channel jx reads
`P = ρ + dt_r − dt^s + μ_j·ι_r^s + d_jx^s + d_r,jx`. dt^s is SHARED with the Q side and
already realized by the Q-family IF pin (the clock-common gauge, NetworkSbasis.cpp:484) plus
the per-constellation clock datum, so the X-family IF combination has a fixed reference and is
**estimable** — the per-satellite inter-family ISC, a real product quantity. Only the GF
combination trades against the X stations' per-LOS iono. Two confirmations: (1) **empirical**
— every per-satellite null direction in the dump is μ-patterned, none flat-IF-patterned;
(2) **analytic** — with a₁ = f₁²/(f₁²−f₂²), a₂ = −f₂²/(f₁²−f₂²) (NetworkSbasis.cpp:585-587),
`a₁·μ₁ + a₂·μ₂ = a₁ + a₂·(f₁²/f₂²) = (f₁² − f₂²·f₁²/f₂²)/(f₁² − f₂²) = 0`, so an IF row is
identically blind to the μ direction and could never have closed it.

## §2 The rule this card adds

For every (satellite s, admitted channel family F) of a constellation where F is **not** the
family of s's IF-pinned pair AND F's observing-station set has no bridge to the pinned pair
(§3 census):

```
emit ONE row:   ( d_{s,F2} − d_{s,F1} ) / ( μ_{F2} − μ_{F1} ) = 0
```

on F's two lowest distinct-frequency code signals (F1, F2).

**Sign convention:** **−1/(μ₂−μ₁) on F1** (low band, μ₁ ≡ 1) and **+1/(μ₂−μ₁) on F2** — the
existing row at NetworkSbasis.cpp:646-647 is specific (the draft's "∓1/(μ₂−μ₁)" was
ambiguous). μ is **pair-relative**: the code computes `mu2_minus_mu1 = (f1*f1)/(f2*f2) - 1.0`
(:641), i.e. μ_j = (f_F1/f_j)² with μ₁ ≡ 1, so a family row is the *identical* formula on
different columns and for Galileo the X pair's weights equal the Q pair's numerically.

**Never an IF row for F** — the clock datum has already spent the IF direction, and a family
IF pin would pin the estimable ISC to zero: an over-pin on a data-determined quantity, which
injects tension that must discharge somewhere (the §12.8 failure class of
`SPEC_synthetic_minimum_network` — "constrain the family cold at a garbage level"; §6.0 has
the measured instance on this very posture).

Row targets are **always 0 and never time-linked**, matching the existing GF row and for the
same reason (NetworkSbasis.cpp:633-639): d_GF per family is a datum quantity the estimable
parameters absorb (Table 8 per family, §D) — metre-scale gauge, not an integer-truth-bearing
state. Structurally guaranteed: `cb_time_linked` is never read inside the GF block and the
push is a bare `zs.push_back(0.0)` (:652).

### §2.1 The s2z redundancy — and why `code_datum_time_linked` needs an interlock

The family rows are **exactly rank-1 dependent** with the family's s2z rows:
`Σ_s (family-GF row_s) ≡ ( s2z(F2) − s2z(F1) ) / (μ₂ − μ₁)`. With every RHS at 0 this is a
consistent redundant constraint and is PSD-safe (`addSharedConstraintRows` folds w·HᵀH at
`kCodeBiasGaugeWeight` = 1.0e3, NetworkSolveGlue.cpp:269 / NetworkEstimator.cpp:882; redundant
PSD information cannot create a new near-zero). Stated in the run log rather than "fixed":
dropping one satellite's row would make *which* satellite is unpinned depend on the
rising/setting set, which is worse.

> **Anchor correction.** The draft cited "the Weyl argument the RXCB-DATUM **log line** cites
> (GnssConfigLoadNetwork.cpp:2314-2323)". The Weyl sentence is in the **source comment** at
> :2318-2322; the emitted `log->info` spans :2323-2327 and says "PSD-safe", not "Weyl". Line
> :2314 is the branch head.

**Under `network.code_datum_time_linked` the s2z RHS becomes Σ b̂_prev (:701-703), generally
nonzero and DIFFERENT per channel — and the dependency turns INCONSISTENT:** the GF rows
demand Σd_{F2} − Σd_{F1} = 0 while s2z demands Σd_{F2} = rhs_{F2} and Σd_{F1} = rhs_{F1}.
Folded as heavily weighted pseudo-observations rather than exact constraints, this does not
throw — it silently least-squares, injecting tension of order (rhs_{F2} − rhs_{F1})/(μ₂−μ₁).
**PRE-EXISTING for the primary GF pin** (identical algebra on C1C/C5Q), not introduced here.

> ### ⚠ CORRECTION — this is LIVE on every posture-C FIELD arm, not latent
> An earlier revision called the combination "latent, not live" because no
> `code_bias_gf_datum` config in `config/test_suite` sets `code_datum_time_linked`. **That
> grep ignored `extends`.** Resolving the chain of `_pc305_ggq_tri_slant_20m` gives
> `code_datum_time_linked = True` **and** `code_bias_gf_datum = True` — the former inherited
> from `_ft63_24h_conv.json:124` five levels down, the latter entering at
> `_pc305_tri_slant_10ep`. Only the three *sim* arms are clean. So every posture-C field arm
> to date has run the primary GF pin against s2z rows whose targets are inconsistent with it.
> This card does **not** fix that, but it must stop being invisible — and this card doubles
> the affected row count. **Magnitude caveat:** under time-linking the s2z rows span the
> channel's VETERANS (`NetworkSbasis::codeS2zTimeLink`), not all present satellites, so the
> dependency is *exactly* rank-1 only when the veteran set matches the pinned set; otherwise
> it is approximate and the tension correspondingly smaller. Quantifying it on a real arm is
> owed work, not a settled number.

Implemented as a **WARN, deliberately not a hard error** (`GnssConfigLoadNetwork.cpp`, at the
`code_datum_time_linked` validation site): a hard error would reject the entire established
field lineage over a defect this card did not introduce and has not measured. Escalating it is
a separate decision with its own evidence.

### §2.2 What becomes estimable

For family F: the per-satellite IF_F combination (the inter-family ISC, referenced to the
constellation's one realized clock) and — when F carries a third band — the modernized DCB
d_{s,F1} − d_{s,Fj≥3} in F's own GF convention. For a two-band family (the 11 XX− stations'
world) the ISC is the entire estimable content; correct, not a defect, mirroring the existing
`n_gf_only_pair` accounting (NetworkSbasis.cpp:663-671).

> **Gotcha for the per-family counter.** `sigs` at that site is the satellite's WHOLE
> registered signal list *across both families*, so a Q-pinned Galileo satellite with X
> channels registered already scores `has_third = true`. A per-family `n_gf_only_pair` must
> scan the FAMILY's signal subset, not `sigs`.

### §2.3 The phase side needs no new rows

The measured direction is a single 1-D per-satellite direction carrying BOTH code and phase
(`CB|E##|C5X` and `PB|E##|L5X` moving together), and the family-GF row evaluates to exactly
δ_s on it ((μ₂δ − μ₁δ)/(μ₂−μ₁) = δ), so a code-only row is not orthogonal to it and kills it
whole. A *pure*-PB direction cannot arise where a same-signal code co-anchor exists, and the
CB-ADM phase co-gate guarantees that co-anchor by refusing to allocate phase state for a
signal whose code was quarantined (`MeasurementModel::ensureNetworkSharedStates`; rationale
ApplicationNetworkAssembly.cpp:1629-1637, which names the measured **89 pure-PB near-null
gauges** that motivated it). The §6 gate acceptance is the check; a residual PB-only direction
would be a finding, not a tuning knob.

## §3 Design

### §3.0 What the census may NOT assume — CORRECTED

**The census may NOT derive from the CB-ADM run-latched admission census**, for two measured
reasons:

1. **CB-ADM does not retain per-(station, channel) support.** `pair_station_epochs`
   (NetworkBiasAdmission.cpp:40, filled :62) is collapsed to an `int support` count at :89-92
   and station identity discarded; the latched POD (NetworkBiasAdmission.hpp:85-103) keeps
   `Pair{key, satellite, signal, support, Verdict}` only. `bridges(F)` is **not** derivable
   from `ctx.admission`.
2. **CB-ADM does not run on this lineage at all.** It is gated on `config_.networkCbAdmission()`
   (ApplicationNetwork.cpp:1192-1194), and `network.cb_admission` appears in exactly two
   configs repo-wide (`_cbadm_50sta_15ep_gpsgalbds.json`,
   `_maxnet_305_gpsgalbds_d15_5ep_datum.json`) — neither in the
   `_pc305_ggq_tri_slant_20m → … → _ft63_24h_conv` chain. Arming it would switch code-bias
   registration to the structurally different never-prune branch
   (ApplicationNetworkAssembly.cpp:1745-1778): **a posture change, not a free ride.**

**The census is an independent scan** over the live per-(station, channel) substrate already
at the call site: `setup.stations[i].sm->getSatBiasStates()` (StateManager.hpp:871,
`const std::map<std::string, size_t>&` keyed `"SAT|SIGNAL"`, StateKeySchema.hpp:608-616),
gated by `present[i]`, station named by `stations[i].name` (NetworkRunTypes.hpp:47). Both
`ctx` and `setup.stations` are in scope at NetworkSolveGlue.cpp:257-266, where
`buildCodeBiasDatumRows` is called.

### §3.1 Family key — CORRECTED: a per-constellation relation, not a character

⚠ **The family key is NOT the RINEX attribute character** (C1**C** vs C1**X**). §1's roster
line disproves it: the Q family is **C1C / C5Q / C7Q** — characters `C`, `Q`, `Q`, not one
character — and Galileo E1 has no `Q` attribute at all (SignalBand.hpp:52 — GalE1 codes are
C1A, C1B, C1C, C1X, C1Z). Keying on the raw character would split the Q family in two and, on
GPS, declare `C2W` and `C2L` different families **on a posture that works today — a live
regression risk for a knob advertised as byte-identical where nothing partitions.**

| constellation | primary (discrete-component tracking) | secondary (combined I+Q tracking) |
|---|---|---|
| E (Galileo) | `A B C I Q Z` | `X` |
| G (GPS) | `C W P Y S L I Q M` | `X` |
| C (BeiDou) | `I Q D P A Z` | `X` |

The organizing physical fact: an `X` channel is the **combined** I+Q correlator output and
carries its own hardware bias, while the discrete components share the receiver's component
chain. v1 ships the Galileo row only (§7), the table being the extension point. Raw material
exists — `SignalClassification::attribute` (SignalBand.hpp:106) via `classifySignalForSat`
(SignalBand.hpp:128), already called at NetworkBiasAdmission.cpp:112 which uses `.band` and
drops `.attribute`. The signal string is never canonicalized on the way in: it is the raw
RINEX-3 3-char code from `obs.observed` (MeasurementModel.cpp:1252, raw-signals-only filter
:1185), so `signal[2]` is the attribute everywhere downstream. The codebase already states the
correct relation in prose — NetworkSbasis.hpp:597-599, "the Galileo C/Q-vs-X tracking split
partitions the station set" — it simply has no type for it. This card adds one.

### §3.2 Census (detection) — run-latched, always-on under gf_datum

The partition question is a property of the receiver population, not of an epoch.

- a station's family set per constellation = the families (§3.1) of the code signals in its
  `getSatBiasStates()` keys;
- a constellation's secondary family F is **PARTITIONED** when zero stations carry both an F
  channel and a pinned-pair channel; `bridges(F)` is counted and printed;
- the decision is LATCHED for the run on the first epoch with a usable station set. Mid-run
  bridge loss is NOT re-detected in v1 (§7); the null-space gate remains the backstop and any
  resulting withhold is attributed by the census line already printed.

> ### v1 LIMITATION — the bridge test is per STATION, the physics is per SATELLITE
> An independent rank probe (3 Q + 3 X stations, 3 satellites, full uncombined
> parameterisation with the producer's whole datum stack) reproduced the defect exactly —
> `null = 2 = n_sats − 1` on a split roster, `null = 0` once one family-GF row per satellite
> is added — and established a sharper rule than the one implemented: the secondary family is
> partitioned **for satellite s** unless some station observes *s* on both a primary-pinned
> and a secondary channel, and the network-wide reduction for *k* jointly-observed satellites
> is **max(k−1, 0)**. So `bridges ≥ 1` is **not** sufficient to suppress rows: a single
> bridging station seeing one satellite removes zero directions (k = 1 ⇒ max(0,0) = 0), yet
> the implemented per-station boolean withdraws the card for the whole constellation.
> **v1 ships the conservative per-station test on purpose:** it can only ever *under*-emit
> rows (fail safe, back to today's withhold-and-diagnose), never over-pin an observable
> quantity, and on the measured roster it is exactly equivalent (observed bridges = 0). The
> refinement — emit per-satellite over `SATS \ bridged`, treat k ≤ 1 as partitioned in effect
> — is owed the moment a genuinely bridging receiver enters the roster, and the data is in
> hand (`runFamilyCensus` parses the satellite out of each key and currently discards it).

**The census prints unconditionally whenever `code_bias_gf_datum` is armed**, knob on or off
(invariant #4). Off + partitioned prints the diagnosis missing on 2026-08-12:

```
Network (FAMILY-GF census): constellation E family X — 113 station(s),
0 bridge(s): PARTITIONED.  network.code_bias_gf_datum_per_family is OFF;
expect n_sats−1 undeclared near-null directions and gate withholds.
```

Because it needs a live station set the census **cannot live in the loader** (no `synced`
vector at config-load time); it belongs beside the solve-glue call site.

### §3.3 Row construction

`buildCodeBiasDatumRows` (NetworkSbasis.cpp:415 — unchanged) gains **one** input: the
partition verdict, as `const std::map<std::string, std::set<char>>& gf_secondary_families`
(constellation → partitioned family tags), slotted immediately after `bool gf_datum` (:420).

> **Deliberately NOT `family_gf_pairs: map<sat, vector<pair<sig1,sig2>>>`** as the draft
> proposed. Precomputing pairs outside the builder forks the selection logic and the frequency
> source. Passing only the verdict lets the builder run `selectIfPinPair` itself on the
> family-filtered `sigs` with the same `freqOf` the IF pin uses — **one selection path, one
> frequency source, determinism preserved by construction.**

Rows emit inside the existing per-satellite loop **after line 674** (the primary GF block's
close), through the same gauge-row stack. `plan.declared_dimension` is untouched — verified
structurally: it is a pure function of `buildSharedDatum` (NetworkSbasis.cpp:124, :149, :188)
and is never written by any code-bias path.

> **Correction to the acceptance arithmetic.** "the declared count remains the
> per-constellation clock gauges" is true only in PIN mode (:188). Under
> `clock_datum_sum_to_zero` (:149) or `clock_datum_broadcast_anchored` (:124),
> `declared_dimension` is **0**. §6's "`n_below` returns to declared" must be read against the
> run's actual declared value — 2 on the measured arm, 0 on a sum-to-zero clock posture.

Counters (all run-end, invariant #4): `n_family_gf_sats` per (const, family), census verdict
per family, per-epoch folded-row count in the W6-cb line (extend "IF pin for N sats + s2z for
M levels + GF pin for K sats" with "+ family-GF for J (sat,family)").

### §3.4 Knob and interlocks — CORRECTED anchors

```cpp
// include/config/GnssConfig.hpp — private members, after :5660
// (NOT GnssConfigTypes.hpp, which holds only enums and POD sub-structs and has
//  zero network_code_bias_* members — the original draft misfiled this)
bool network_code_bias_gf_datum_per_family_{false};
// getter after :1577, matching the sibling's doxygen:
bool networkCodeBiasGfDatumPerFamily() const { return network_code_bias_gf_datum_per_family_; }
// network.code_bias_gf_datum_per_family                     // JSON, network section
```

Loader rules: **parse** immediately after :2125, beside its parent, above the W6 chain;
**interlock at :2287** — after the existing GF block's closing `}` (:2286) and before the
RXCB-DATUM comment (:2288), which is precisely what delivers "inherits every existing GF
interlock by construction" (the four hard errors — `!estimate_sat_code_bias` :2219, `iono_sh`
:2226, `iono_weighting` :2241, `frequency_mode: dual` :2257 — have already `return false`d by
then); hard error without `network.code_bias_gf_datum` (no primary GF frame to be secondary
TO); legal with a family that never partitions (the census emits no rows ⇒ byte-identical), so
arming it network-wide is safe — but it stays **default OFF** per the no-promotion rule.

**There is no central network toggle-summary line to add to.** `GnssConfigLoad.cpp:1644`
("Toggles: tropo_grad=…") is a processing/estimation line carrying no network datum knobs. The
established pattern is an inline tagged banner at the validation site — `(CB-GF-DATUM)` :2273,
`(RXCB-DATUM)` :2323, `(CB-ADM)` :2378 — so this card adds
`("Network producer (FAMILY-GF): …")`. `isLoggerEnabled` is **not applicable** (a named-logger
sink registry, not a toggle registry). `NetworkKnobs` is **not needed** either — the GF datum
is read directly at the fold site (`config_.networkCodeBiasGfDatum()`, NetworkSolveGlue.cpp:260).

## §D The S-system question — are Q and X biases in the same frame?

**Answer: one S-system in the clock/IF direction; per-family S-systems in the GF/iono
direction. The product must say so.**

- **Clock direction — SHARED.** dt̃^s is realized once, by the primary family's IF pin; the
  X-family ISC (IF_X combination) is estimated AGAINST that clock. A consumer applies one
  clock per satellite for both families.

  > **Mechanism correction.** Earlier revisions wrote "by the primary family's IF pin **plus
  > the per-constellation clock datum**". The rank probe falsifies the second conjunct: with
  > the clock datum removed the inter-family contrast stays determined, because dt^s CANCELS
  > between the two blocks' own observations. The primary IF pin LABELS the clock↔bias split;
  > the secondary family's fleet-common level is fixed separately by the per-(constellation,
  > signal) s2z rows on F's own channels. **This sharpens what §5 may grade:** the ISC is
  > data-determined in its **n_sats − 1 contrasts** and datum-determined (zero-mean) in its
  > **one fleet dimension**, so the ISC channel must be graded as a zero-mean-across-satellites
  > contrast, never as an absolute — grading the fleet mean against injected truth would report
  > a datum choice as an error.
- **Iono direction — PER FAMILY.** Table 8 applies family-wise:
  ι̃_r^s = ι_r^s + d_{r,GF(fam(r))} − d^s_{GF,fam(r)}. A station's slant-iono estimate absorbs
  ITS OWN family's satellite GF lump. The two families' conventions differ per satellite by
  the true inter-family GF difference ΔGF^s = GF(d_X^s) − GF(d_Q^s) — physically real
  (ns-scale hardware), unobservable within the network by construction, NOT emitted anywhere.
- **The PHASE bias rides the same per-family lump** and the original §D omitted it:
  δ̃_j^s = δ_j^s + μ_j·GF_fam(d^s). Same Table-8 bookkeeping (iono enters phase with
  coefficient −μ_j). It matters because §D.1's "safe", §D.2's mixing rule and §D.3's consumer
  contract apply to the phase-bias records too — a consumer keeping families separate on code
  but mixing on phase has the same μ-patterned inconsistency, in the least visible channel.

**Verified numerically, not asserted** (§5): on a 6-channel Galileo fixture (C1C/C5Q/C7Q +
C1X/C5X/C7X) the per-family null space {e over all channels, μ|_F per family} against the
per-family row set {μ_IF of the primary pair, μ_GF of each family's own pair} gives R·N = I to
4e−16 — block-wise consistent because μ_GF^(F) has support only inside F (so
μ_GF^(F)·μ|_F' = δ_FF' and μ_GF^(F)·e = 0) and μ_IF^(prim) only in the primary family. After
projection the Q pinned pair is exactly 0, the X GF combination exactly 0, and the X IF
combination recovers the injected ISC exactly.

1. **Single-family consumer (every receiver in the current roster): safe.** It applies its
   family's B records and estimates its own iono; the lump cancels family-consistently — the
   already-validated case (the Q-family field ladder and the sim graded arms).
2. **Bridging consumer: MUST NOT mix.** One per-LOS iono state cannot absorb two conventions;
   applying B records of both families injects a per-satellite μ-patterned inconsistency of
   size ΔGF^s. v1 contract: a consumer restricts each constellation to ONE family's
   iono-coupled content (the other family's channels may be used only in iono-free
   combinations). Estimating ΔGF^s consumer-side is a legal future extension, out of scope.
3. **Product token.** Extend the §1.5 fill (NetworkSolPrep.cpp:956-1049; the line to
   generalize is :1023, `gf_decl[...] = p.first + "-" + p.second`) with the `'+'`-joined
   per-family form, e.g. `gfpin=E:C1C-C5Q+C1X-C5X G:C1C-C2W`. **The wire already carries this
   form**: the parser stores the value verbatim (NetworkProductIO.cpp:326-335, with an
   explicit comment that a future multi-family value must survive a round-trip through an
   older reader) and the writer is symmetric (:496-497). Only the PRODUCER side is missing.
4. **⚠ BLOCKING — THE DECLARATION IS CURRENTLY FALSE ON A MULTI-FAMILY PRODUCT.** §D.3's rule
   "absence of the token on a multi-family product is a REFUSE" is **unimplementable on
   today's wire, and worse, the token is not absent — it is wrong.** `selectIfPinPair` orders
   by (band digit, full name) and takes the first distinct frequency, so on a Q+X Galileo
   roster **every** E satellite resolves to (C1C, C5Q) whether or not X channels are
   registered. The unanimity rule at NetworkSolPrep.cpp:1006-1027 therefore sees perfect
   agreement and emits a confident `gfpin=E:C1C-C5Q`, and the grader refuses only on a literal
   `'+'` in the value (simnet_grade.py:656). Net effect **on this card's own target posture**:
   a correct-looking single-family token is accepted and the X channels are graded through the
   Q family's GF transform — exactly the silent mis-framing the token exists to prevent.
   Nothing on the S line names the observed families. **Required with this card** (either,
   preferably both): (a) **producer-side** — when a constellation has >1 admitted family,
   either declare the `'+'`-joined per-family value (knob ON) or **decline to declare `gfpin=`
   at all and log why** (knob OFF); silence is honest, a single-family pair on a multi-family
   product is not. (b) **grader-side** — §5's multi-family screen, which does not depend on
   the producer being fixed. *Sort caveat:* (C1C, C5Q) is the Galileo pair only while no C5I
   is registered — the key is (band digit, full name), so C5I would sort ahead of C5Q.
5. **Pre-existing fill gap — ⚠ NO LONGER OWED BY THIS CARD: it LANDED separately 2026-08-13**
   (independent session, same root cause independently reproduced). `this_epoch_if_pin_real` /
   `this_epoch_gf_pin_real` are in the tree, `cbpin=` now declares on every posture, and a v1
   single-family `gfpin=<const>:<s1>-<s2>` is written, parsed, legend-declared, compared by
   `checkContract` and round-trip tested — including the `'+'`-joined per-family form, which
   the parser accepts verbatim so this card only adds the PRODUCER side. **Full narrative:
   `docs/SPEC_product_cbpin_contract.md` §6. Wire semantics:
   `docs/SPEC_sbasis_realization_contract.md` §A/§B/§D**, which also records the defect class —
   *a product-contract declaration accidentally gated on a QC/trace-class knob*
   (`emit_clock_disc`, absent from the whole ft63 chain, so every pc305 field arm structurally
   could not declare `cbpin=` while every sim posture-C config declared fine). Two
   implementation notes binding THIS card: the declaration rule is **unanimity, not modal** (a
   modal declaration is false for the minority satellites — precisely the family case), and it
   is **rebuilt at every emit**, not stamped once.

   **One residual asymmetry, owed to THIS card:** the unconditional IF record — the card's
   `NetworkSbasis.cpp:569`, now **`:724`** — fires before row construction and is guarded only
   on `f1,f2 > 0`, **not on `row_ok`**, whereas the GF record (the card's `:662`, now `:769`)
   sits correctly inside `if (gf_ok && …)`. So `tally.second` (NetworkSolPrep.cpp:1002) can
   count a satellite whose IF row never folded while `tally.first` cannot, which can
   **suppress a legitimate `gfpin=` as a phantom "partial split"**. Practically unreachable
   (it needs `codeBiasColumn` to throw), but the fix is one line: move the record below the
   push at :616-619, or add `&& row_ok`. *(For contrast, the hold-OFF disc-compare scratch
   `this_epoch_if_pin_sel` is gated `!clock_signal_basis_hold && emit_clock_disc && f1>0 &&
   f2>0` — the card's `NetworkSbasis.cpp:559`, now `:654`.)*

## §5 Grader consequences (a datum change is a GRADER change — standing rule)

**Already built** (uncommitted): `simnet_grade.py` derives the transform **per constellation**
rather than applying GPS's pin and GPS frequencies to Galileo satellites — a genuine
correctness fix, since the old code had no E5b entry at all, the band that under the GF datum
carries the ONLY estimable content. The projector is t̃ = t − e·(μ_IF·t) − μ·(μ_GF·t), built at
:707-715 / :729-738, with the GF basis vector byte-for-byte matching the producer's row. All
four orthogonality identities hold to machine precision as coded (μ_IF·e = 1 exactly,
μ_IF·μ = −4.4e−16, μ_GF·e = 0 exactly, μ_GF·μ = 1+2e−16); the projector is idempotent to
6.7e−16 and annihilates both pinned channels exactly. Odijk Eqs. 26/31 as claimed.

> ⚠ **The identity is asymmetric, and getting it wrong is easy.** Writing the assertion as
> "every pinned channel projects to 0" is WRONG for a secondary family and was caught by the
> assertion itself during implementation: the primary family loses both its IF and its GF
> direction so each pinned channel vanishes identically; a secondary family loses only its GF
> direction, so its pinned channels DO NOT vanish — they still carry the inter-family ISC,
> which is the whole point. What must vanish there is the **GF combination of the pair**.
> Asserting |t| = 0 on a secondary channel is asserting exactly the over-pin §2 refuses.

**What this card added — all four BUILT and numerically verified:**

1. **The per-family transform.** Remove, per family: that family's GF direction (from the
   `gfpin=` token), and for the PRIMARY family only, additionally the IF direction. Secondary
   families keep their IF content — graded as the ISC channel against injected truth. A strict
   generalization: the projection formula at :737 is unchanged; only `gflump` needs a family key.
2. **The multi-family screen §D.3 describes.** After `emitted` is computed (~:636), per
   constellation: `fams = {family(j) for (s,j) in ep["code_bias"] if s[0]==c}`; if
   `len(fams) > 1` and `gfpin.get(c)` is absent or single-family, refuse with
   `multifamily_emitted_no_token`. **This is the screen that would have caught the 2026-08-12
   `_pc305_gg_tri_slant_20m` posture**, and unlike §D.4(a) it does not depend on the producer.
3. **The assertion the code promises.** The comment at :700-702 says the pinned-channel
   self-check is "asserted below rather than assumed" — there is no assertion in the file
   (`grep -c '^\s*assert'` = 0). Add it (after :737, when `d["gf"]` and `j in d["pin"]`,
   require `abs(t) < 1e-9` and count violations), or correct the comment. Under a per-family
   transform this is the one identity that catches a wrong family pairing silently.
4. **Fix the fail-open sentinel.** `parse_s_line` turns a `'+'` value into
   `("MULTIFAMILY", raw)` and `grade_code_bias` refuses it cleanly (:656-657) — but
   `if_lump_by_sat` tests `len(pair) != 2` (:313) and the sentinel **is** length 2, so it
   unpacks the sentinel as signals and silently produces no matches.

> ⚠ `scripts/simnet_grade.py` has a **live concurrent writer** — it changed twice during this
> card's recon (most recently adding a `gf_forced` parameter that fixes a real precedence bug:
> an explicit `--gf-datum` was a no-op for every constellation as soon as ANY `gfpin` appeared
> on the wire). Re-diff before patching.

## §6 Validation ladder

### §6.0 Why gate closure is NOT acceptance — the `sbasis_complete` counter-example

**The most important paragraph in the card**, omitted entirely by the original draft.

`network.sbasis_complete` (SBC) is an **existing, generic mechanism that already closes
exactly this null space**: "one minimum-constraint row per realized extra direction (PB↔iono
channel-family trades)" — `docs/SPEC_sbasis_realization_contract.md` **§A** (the "GPS+Gal
channel-family extras" row), `NetworkSbasis::completeSbasisDimension`. Its posture-ledger line
even describes this defect. **And on this exact posture it passes the rank gate and destroys
the field:**

- `SPEC_synthetic_minimum_network.md` **§13.8.3** (the "Already measured, do not repeat" trap
  register) on the slant/triple posture: "`sbasis_complete` passes the gate and destroys the
  ionosphere on a cold start". *(Was `HANDOFF_slant_triple_next.md` §7, absorbed 2026-08-15;
  now `docs/attic/HANDOFF_slant_triple_next.md`.)*
- `docs/SPEC_synthetic_minimum_network.md` **§12.2**, ladder rung 3 `_simnet50_wsh_sbc`
  (1 / 19 emitted): it "constrains the extra direction along its own eigenvector *at the
  carried level*; at epoch 2 that carried level is cold, and the tension went straight into
  the field: **a₀₀ = −6292 TECU, rms_all 3541 TECU**", after which the SH health gate withheld
  18 of 19 epochs. **"The rank gate was satisfied and the answer was garbage."**

So the original acceptance criteria — "`n_below` returns to declared" and "emits at default
tolerance" — are a **known false positive**: the documented failure mode satisfies both.

**Why family-GF should succeed where SBC fails** — the card's central argument: SBC constrains
a *numerically discovered* direction at the **carried level**, garbage on a cold start — a hard
constraint at a wrong value, injecting real tension. Family-GF constrains a *physically
identified* direction at **RHS 0**, a legitimate S-basis choice because d_GF is a true gauge
quantity the estimable parameters absorb (Odijk Table 8, per family). **The distinguishing
evidence is the ionosphere level, not the rank count. Every rung below must report an
ionosphere-level check, and an SBC contrast arm is mandatory, not optional.**

### §6.1 Rungs

**V0 — OFF byte-identity. ✅ RUN AND PASSED 2026-08-13.** Same-build A/B, Release, two arms.
The CONTROL binary reverts exactly this card's two product-visible shared-path edits (the
`this_epoch_if_pin_real` relocation and the `partitioned ⇒ suppress gfpin` branch); everything
else is identical source, isolating the surface that could break invariant #6. Live tree
restored byte-exact afterwards (md5-verified, zero markers left).

| arm | binary md5 | products md5 | verdict |
|---|---|---|---|
| `_simnet60_tri_slant` (60-sta sim, 20 ep) control | `38f18e14…` | `92cd4335…` | — |
| `_simnet60_tri_slant` arm | `196d7ddf…` | `92cd4335…` | **IDENTICAL** |
| `_pc305_tri_slant_10ep` (303-sta GPS field, 10 ep) control | `38f18e14…` | `3fd0675c…` | — |
| `_pc305_tri_slant_10ep` arm | `196d7ddf…` | `3fd0675c…` | **IDENTICAL** |

Both arms 20/20 and 10/10 epochs emitted, all `ok`, zero withholds; B/C record counts match
exactly (1920/640 and 980/320). Fold telemetry carries **no** family-GF segment ("68 code-bias
gauge rows: IF pin for 32 sats + s2z for 4 levels + GF pin for 32 sats"), i.e. zero extra rows
folded. S-line tokens unchanged where emitted (`cbpin=G:C1C-C2W gfpin=G:C1C-C2W`, identical in
both sim arms; the field arm emits no S record at all on this config).

**The GPS regression guard fired, and it was not a formality.** `_pc305_tri_slant_10ep` sets
`signal_priorities.GPS` to include **both C5Q and C5X**, so GPS genuinely HAS a secondary
family on this roster. The census reports `constellation G family X — 0 station(s), 110
BRIDGE(s) to the primary family: NOT partitioned, no extra rows` — **110 stations track GPS
C5X and primary channels, so the bridge test, not the family key, is what keeps the card off a
posture that works today.** This is the concrete case §3.1 was rewritten for: a naive attribute
key would have found the same X family. The sim arm is the other half: `constellation G — no
secondary (X) channel family observed on 60 station(s): SINGLE-FAMILY`. **§2.1 confirmed on
live data:** the field arm emitted the new `code_datum_time_linked` + `code_bias_gf_datum`
warning — the inheritance finding reproduced at runtime rather than by config inspection.
**What V0 does NOT cover:** neither roster partitions, so the *emitting* path and the
"partitioned ⇒ suppress `gfpin=`" branch are still exercised only by unit tests. That is V1's
job, and §6.0 governs its acceptance.

**V1 — sim, family-partition fixture.** Sim change (scoped), **in this order** — the emission
path is the open question, not the truth source:

1. a per-station roster field `family: "x"` in `NetworkStation` (`include/sim/SimConfig.hpp:98-115`
   — verified, no such field today), plumbed through `writeStationConfig`, switching that
   station's Galileo band plan to C1X/L1X + C5X/L5X (E5b optional via `galileo_e5b` for a
   triple-X arm). The band plans are hard-coded Q-family-only at `src/sim/main.cpp:1644-1656`,
   and `synthOsb` (:1671-1677) is only ever called with `band.code`/`band.phase` from that plan
   (:2544-2545) — so **adding `"C5X"` to `synth_osb_amp_m` before this step is inert**. No
   C1X/C5X/C7X exists anywhere in `src/sim` or `include/sim` today.
2. `simnet_make_roster.py --family-x-fraction 0.38`, deterministic (index mod), matching the
   field's 187/113 split.
3. X truth biases via `synth_osb_amp_m` entries for the X codes, flowing into `truth_bias.csv`
   (`src/sim/main.cpp:2157`).

Acceptance, all measured not asserted: (1) control (knob OFF): gate withholds, census prints
PARTITIONED, dump shows `n_below = declared + (n_E_sats − 1)` **computed from the run's own
dump** per §1's precondition, with `declared` read from the run not assumed to be 2 (§3.3);
(2) knob ON: emits at DEFAULT tolerance, `n_below = declared`, dump clean; (3) **iono level in
band** — the §6.0 discriminator, compared against the control-family arm AND against an **SBC
arm** (`sbasis_complete` on the same fixture) expected to reproduce the rung-3 collapse; a
family-GF arm that closes the gate but moves the iono level like SBC is a FAILURE, not a pass;
(4) truth grade: X pinned-GF combination datum-exact, ISC channel (IF_X) graded against
injected truth through the §5 transform, Q channels byte-comparable to the control-family arm;
(5) single-family consumers (one Q, one X) both self-consistent — the X consumer is the FIRST
X-family consumer ever run.

**V2 — field.** The `_pc305_gg_tri_slant_20m` posture + knob (fresh lineage): expect emission
at default tolerance with **all 113 X stations admitted**, the W6-cb line growing by the
family-GF row count, eq34 chains complete for L1X/L5X/L7X, and E X-channel B records carrying
ISC + C7X content with the GF combination ≡ 0. Grade on datum-invariant quantities only (no
truth in the field); level questions ride the standard ladder (20 m → 1 h → 4 h). **Report the
ionosphere level at every rung** — §6.0's discriminator is the only guard against reproducing
SBC's failure silently.

## §7 Deferrals (triaged per invariant #3)

- **Mid-run bridge appearance/loss**: NOT handled in v1 — the census is run-latched.
  Field-inert on the evidence (the partition is a receiver-hardware property and the
  310-station scan shows 0 bridges to lose); the rank gate is the backstop and the census line
  makes any such withhold self-attributing. Handling it properly is an invariant-#7 two-edge
  event (datum topology change) and deserves its own card if a mixed-family receiver appears.
- **Bridging-consumer ΔGF estimation**: contract-level exclusion in v1 (§D.2).
- **Non-Galileo constellations — TIGHTENED.** v1 ships the Galileo row of §3.1's table only.
  The mechanism is constellation-generic and the census runs for every constellation, but **v1
  gates row emission to an explicit constellation allow-list** rather than the draft's "rows
  emit wherever partition is detected" — that open-ended rule combined with a wrong family key
  is precisely how a GPS posture that works today would start emitting rows. BDS-3 B1C/B2a is
  the expected next case and gets its own validation arm.
- **`code_datum_time_linked` + `code_bias_gf_datum`**: §2.1. This card adds the interlock but
  does not resolve the underlying inconsistency for the primary pin.

## §8 Standing-invariants pre-flight

1. n/a — hard datum rows, no carried state, no slip coupling; rows are re-derived every epoch
   from the latched census. 2. Release-only field runs. 3. Deferrals triaged — §7.
4. Counters/events — §3.2 census line (always-on under gf_datum), extended W6-cb fold line,
   run-end per-family audit, `gfpin=` declaration or a logged reason; no silent inertness path
   (knob-off + partitioned prints the expected-withhold diagnosis). 5. Datum-invariant
   grading — §5/§6; the ISC channel is graded THROUGH the S-transform, never raw.
6. Default-OFF + structural byte-identity — §6 V0; same-build A/B mandatory. 7. The census is
   run-latched precisely to avoid mid-run datum-topology edges (§7); the one v1 edge (cold
   start with the census verdict) is logged at startup with identities and counts.

## §9 Owed with the build

`tests/test_family_gf_datum.cpp`: row weights **−1/(μ₂−μ₁) on F1, +1/(μ₂−μ₁) on F2** and target
0 (never time-linked); NO IF row for a secondary family; **the §3.1 family mapping, including
the GPS non-partition case — C2W/C2L must NOT be two families**; census partition/bridge/latch
logic on synthetic support sets; the §1 count closure on a toy normal-equation system
**including the unequal-support case** the §1 precondition names; `gfpin=` per-family
round-trip through the S-line writer/parser; the §D.5 `row_ok` asymmetry. Plus the V1 sim
fixture pair, **the SBC contrast arm**, and the same-build V0 hashes, recorded in this file's
status header when run.
