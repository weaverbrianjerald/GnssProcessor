# W6 Satellite-OSB Producer Slice — Adversarial Review

**Date:** 2026-06-29
**Target:** `ProcessingMode::PPP_RTK_Network`, flag `network.estimate_sat_code_bias`
(`estimate_network_sat_code_bias_` / `estimateNetworkSatCodeBias()`).
**Method:** 8-dimension adversarial review (28 agents: per-dimension reviewers → per-finding
independent refute pass → lead synthesis). Findings below all **survived** the refute pass and were
re-checked against the live tree. Line numbers are hints — the file is concurrently edited; anchor by
symbol.

> **Provenance note.** This review was produced by a session tasked with *receiver* code bias; it
> discovered the *satellite* OSB slice (W6) is being implemented concurrently and already forces the
> receiver DCB + GIM as a coupled dependency. Rather than duplicate, this is a de-risking review of that
> in-flight slice.

---

## 1. Verdict

**Not yet shippable.** The estimability *theory* is sound — GIM-weighted iono + per-constellation
sum-to-zero + per-satellite IF pin is the right S-basis-invariant recipe, the whitening math is correct,
and the null-space gate is a genuine safety net. But the **datum implementation only pins the two lowest
distinct bands**, while a code-bias column is allocated for **every** band/attribute. Under the shipped
`frequency_mode:"all"` / `channel_mode:"all"` config, 3rd+ band (and second same-band attribute) common
modes are **unpinned exact gauges** → `assertNullSpaceRank` trips → **every such epoch is withheld → zero
products**. Fix the datum gap and the slice becomes viable; the rest is de-risking.

**Live corroboration (firsthand).** A concurrent W6 run's estimability spectrum (observed in
`logs/main_log.log`, 2024-129) showed:
```
Network[estimability]: sv=3.771e-12 dir spanned by [CB|G26|C5Q=0.45, CB|G10|C5Q=0.45,
                                                     CB|G27|C5Q=0.45, CB|G23|C5Q=0.45, CB|G18|C5Q=0.45]
```
— a machine-zero singular value in the **L5 (`C5Q`)** satellite code-bias direction. That is C1 firing in
a real run: the L5 band (the 3rd GPS band under `frequency_mode:"all"`) is an unpinned constellation-common
gauge. The bundled `config/ppp_rtk_network_codebias.json` ships `frequency_mode:"all"`, so **the slice
zero-emits out of the box.**

---

## 2. Confirmed issues (ranked)

### CRITICAL

**C1 — 3rd+ band / second-attribute OSB columns get no datum pin → gate trips → zero-emit under the
shipped config.**
`ApplicationNetwork.cpp` W6 datum block (`~1259-1305`): per-sat IF pin uses `sigs.front()` (f1) + the
*first distinct-frequency* signal (f2) only; the sum-to-zero uses `sigs.front()` (lowest band) only. But
`MeasurementModel::ensureNetworkSharedStates` allocates a column for **every** code signal. Every band-≥3
column (GPS L5, Galileo E5b/E6/E5) and every non-front same-band attribute (e.g. `C1C` vs `C1W`) is in **no**
gauge row → its constellation-common mode is a real exact gauge (the forced per-band receiver `*_IFB<band>`
Schur-eliminates the per-station mean, so it is not data-pinned) → `near_zero > declared_dimension` → epoch
withheld.
**Fix:** generate datum rows for **all** registered bands — per satellite, one inter-band IF/GF pin per
*extra* band tying it to the primary pair, **plus** a per-(constellation, band/attribute) sum-to-zero for
every non-reference column. **Or** restrict registered OSB columns to a fixed 2-band reference pair per
constellation and hard-`assert nband<=2` so it **fails closed** instead of silently zero-emitting. Until
fixed, drop `frequency_mode:"all"`/`channel_mode:"all"` from the bundled config.

### HIGH

**H1 — Satellite OSB datum re-reference is an UNSIGNALLED step on every consumer code residual.**
`NetworkProductEmitter::emitToStore` code-bias branch (`~64-71`) has **no** discontinuity bookkeeping —
contrast the phase branch (`~86-100`): no `disc_counter_`/`last_*`/`force_datum_bump_`, `SsrCodeBias` has no
`discontinuity_counter`, and the soln trace hardcodes `0`. The per-constellation sum-to-zero membership
changes as sats rise/set and the IF pin re-balances band-2, so **every OSB steps non-uniformly at each
handover** and the consumer applies it as a known correction with no reset hook → a jump on every code
residual. **Fix:** mirror the phase-bias bookkeeping for code (add `discontinuity_counter`, track per-signal
`last_code_bias_` + `force_datum_bump_`, wire a consumer code-bias discontinuity hook, stamp the real
counter). **Or** anchor the DCB datum to a fixed reference satellite per constellation (broadcast TGD / IGS
DCB) so the absolute level stops re-referencing.

### MEDIUM

**M1 — Min-2-GIM-valid-sats-per-constellation precondition documented but not enforced** → per-constellation
zero-emit. `appendGimIonoRows` folds even a 1-row (`mc==1`) block (a loose 1×1 prior with no between-sat
structure); the only `>=2` guard (`~916`) is `>=2 *stations*`, not `>=2 GIM-valid sats`. A constellation
momentarily showing one GIM-valid sat (realistic for Galileo on a 2-station baseline at marginal cutoff)
leaves its DCB a near-null → epoch withheld. **Fix:** count GIM-valid sats per constellation across
contributing stations; require `>=2` before registering/datuming that constellation's OSB columns, else
exclude them this epoch + one-shot warn. (This also retires U1.)

**M2 — W6 omits the GPS/Galileo `signal_priorities` fail-loud guard its GLONASS sibling enforces.**
`signal_priorities_` has no defaults; a W6 config missing GPS/Galileo entries loads OK, drops every sat at
signal selection (WARN, not error), emits an empty product. **Fix:** mirror the GLONASS guard in the W6
block — for each OSB-covered GNSS in `active_systems_`, require a matching `signal_priorities_` entry,
`log->error` + `return false` otherwise.

**M3 — Production `data_source` forces `iono_weighting` but not `data_download.download_gim`** → spurious
hard-error. In production the IONEX resolves only when `download_gim` (default false); the W6 block forces
five flags but not this one, so a production W6 run hard-errors at startup even when the AC publishes a GIM.
(Loud/actionable; internal mode unaffected.) **Fix:** force `download_config_.download_gim = true` in the W6
block; keep the `runNetwork` hard-error as backstop.

### LOW

- **L1 — Data race on one-shot `gim_diag_done`** (`appendGimIonoRows ~662-667`): non-atomic test-then-set on
  a `[&]`-captured bool inside the `#pragma omp parallel for` produce region — UB; the "one-shot" warn may
  print twice with a nondeterministic station name (it is the *only* shared write in the parallel region, so
  the per-station `A_s/A_r/w/y` blocks are unaffected). Fix: `std::call_once` / `omp critical`, or drop the
  cross-station one-shot.
- **L2 — Extreme `common_mode_sigma_m` ill-conditions `R`** (unchecked LLT, no `info()` check); `cond(R) ≈
  σ_c²·mc/var_min` → ~1e16 at config-reachable `σ_c≈1e4+` (default 50 is safe, ~7 digits). Fix: whiten the
  rank-1 structure analytically via Sherman-Morrison — exact, cheaper, removes the cliff and the need for an
  `info()` check.
- **L3 — `registered_code_keys` allocation block (`~902-926`) runs unguarded in flag-OFF mode** (verified
  behaviorally byte-identical no-op today). Fix: wrap in `if (estimateNetworkSatCodeBias())` for consistency
  + future-proofing.
- **L4 — Consumer inherits `iono_weighting_.enabled=true` into `RealTimeEngine`** (which warns "unsupported")
  — cosmetic, no numerical effect. Fix: `c.iono_weighting_.enabled = false` in `deriveClosedLoopConsumerConfig`.

### UNCERTAIN

- **U1 — `mc==1` GIM prior "fights" the sum-to-zero datum**, biasing the emitted DCB. Structurally true but
  negligible at defaults (datum weight 1e3 vs GIM info ~1/2500 → ~6 decades; datum-dominated to sub-mm).
  **Moot if M1 is implemented** (the `mc==1` path is then excluded). Only matters at small non-default
  `common_mode_sigma_m`.

---

## 3. What is sound (verified correct — do not re-litigate)

- **Estimability recipe** (GIM-weighted iono + per-const sum-to-zero + per-sat IF pin) is the correct,
  S-basis-invariant design; the `σ_c²·11ᵀ` common-mode-free structure correctly frees the per-constellation
  absolute iono level for multi-sat blocks.
- **IF pin & sum-to-zero are pure gauge directions** for the two-band case; `kCodeBiasGaugeWeight=1e3` is
  correctly placed above the gate cutoff and below σmax (no spectral inflation). The IF-lump → estimable
  clock double-count guard is correct.
- **Whitening math** (`A_r=L⁻¹H`, `y=L⁻¹z`, `w=1` from `LLᵀ=R`) contributes exactly `HᵀR⁻¹H` — correct to
  ~7 digits at the shipped `common_mode_sigma_m=50`; parallel-safe (const GIM query, station-own block).
- **Null-space gate** (`assertNullSpaceRank`, tol 1e-9) genuinely catches C1/M1 as *withheld epochs*, not
  silent corruption. D2 carry-forward prior is added **after** the gate (gate sees data-only structure).
- **Flag-OFF is behaviorally byte-identical** to committed V1.
- **Sign trace producer→store→consumer is consistent** (no sign error); consumer OSB application
  (`ssr_bias_term`) is numerically correct — the only consumer gap is the missing discontinuity *signal*
  (H1), not the math.
- **Phase-bias emit path** has full correct discontinuity bookkeeping (the model H1 should copy).

---

## 4. Top 3 next actions

1. **Fix C1 (the blocker).** Extend the datum to all bands/attributes, **or** restrict OSB columns to a
   fixed 2-band pair + hard `assert nband<=2`. Until then, change the bundled config off
   `frequency_mode:"all"`. *Without this the producer emits nothing under its own config.*
2. **Fix H1 (consumer correctness).** Add code-bias discontinuity bookkeeping (mirror phase) or anchor the
   DCB datum to a fixed reference satellite. The only confirmed downstream *correctness* gap.
3. **Land M1+M2+M3 together** (min-2-GIM guard, signal_priorities fail-loud, `download_gim` force) — convert
   silent zero-emits / spurious hard-errors into graceful degradation / actionable errors. Sweep L1-L4 while
   in the files.

**Key anchors:** `ApplicationNetwork.cpp` {`appendGimIonoRows` ~633-697; W6 datum ~1214-1318; GIM load
~277-287; `registered_code_keys` ~902-926; gate ~1350+}; `NetworkProductEmitter.cpp::emitToStore` {code
~64-71, phase ~86-100}; `GnssConfigLoad.cpp` {W6 block ~2611-2649, GLONASS guard ~2660-2699,
`deriveClosedLoopConsumerConfig` ~2784-2860}; `MeasurementModel.cpp::ensureNetworkSharedStates` ~1088-1095;
`StateInitializer.cpp::resolveReceiverBias` ~153-173; `NetworkSbasis.cpp` {`assertNullSpaceRank` ~86,
`declared_dimension` ~53}.
