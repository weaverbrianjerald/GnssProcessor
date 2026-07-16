# HANDOFF — C1 clock-RATE producer (network.emit_clock_rate)

*Written 2026-07-14 (Opus). Status: IMPLEMENTED, default-OFF, **UNBUILT** (user is
running concurrent sessions — build deferred). This is the pivot target from the
clock-model revival Phase A.5 no-go — see `docs/CLOCK_HADAMARD_CALIB_2026-07-14.md`
and `docs/HANDOFF_clock_model_revival.md`.*

## 1. Why this exists (the A.5 finding, one line)

The two-state `network.clock_model` CARRY is a no-go (the emitted clock is white-FM,
not RWFM ⇒ the carry buys ~1× yet risks a NIS storm). But the exploitable
satellite-clock stability is the **deterministic RATE** (intrinsic two-state drift
advantage 16–17× Galileo/BeiDou, 1.7× GPS). This card emits that rate as the product
C1 **without** the carry, so a low-latency / real-time consumer extrapolates
`c0 + c1·dt` instead of holding C0 constant (the SSR clock saw-tooth fix; consumer
already applies it at `SsrCorrectionStore::applyClockToBias`).

## 2. Design (what and why)

`network.emit_clock_rate` (bool, default false). When on **and `clock_model` is off**,
a decoupled per-satellite estimator finite-differences THIS satellite's *posterior
solved clock* over its true elapsed time and stamps the emitted `clock_c1_m_s`:

- **Decoupled from the solve** — injects NO clock prior and drives NO refloat, so the
  network solution is **byte-identical** to the off state; only the (otherwise-zero)
  C1 field changes. This is the key difference from `clock_model`, whose carry prior
  (`addSharedPrior`) + NIS refloat are what make it a no-go.
- **Derives C1 from C0** ⇒ inherits C0's datum. No S-basis rate-datum needed — A.5
  showed the in-estimator 2-state buys ~1× on the residual, so the datum/normal-eq
  surgery the old `project_network_clock_rate_c1` design #1 proposed is not worth it.
- Mirrors `clock_model`'s gap / contiguity / datum-wipe handling: past `R11`
  (`clock_model_max_gap_s`) the drift is untrustworthy ⇒ C1 ZOH for one epoch
  (`T5_clock_rate` event); a datum re-reference reseeds (no step-driven spurious rate);
  a warmup gate skips the cold-start float epochs.
- **Inert when `clock_model` is on** (which already supplies C1 from its two-state
  drift, the B3b path). The RT0-5 rate ceiling (`clock_runaway_rate_m_s`) policies this
  C1 too (it reads `clock_c1_m_s` regardless of source).

The emit path (`NetworkProductEmitter` `rec.c1`, `NetworkProductIO` optional C-field 4)
and the consumer (`c0 + c1·dt`) were already built by the B3b work — no product-format
or consumer change. The stale `project_network_clock_rate_c1` memory ("producer
hardcodes c1=0, NOT built") predates B3b.

## 3. Files changed (all default-OFF / byte-identical when off)

- `include/config/GnssConfig.hpp` — getter `networkEmitClockRate()` + member `network_emit_clock_rate_{false}` + Doxygen.
- `src/core/config/GnssConfigLoad.cpp` — parse `network.emit_clock_rate` + interaction log (clock_model wins if both).
- `src/core/app/ApplicationNetwork.cpp` — `emit_clock_rate_on` / `emit_rate_warm` locals; `clock_rate_est` map; emit-stamp `else if` branch; the decoupled update pass; config-summary line; verification-integer field; warm-resume save/restore.
- `include/products/NetworkStateSnapshot.hpp` + `src/core/products/NetworkStateSnapshot.cpp` — `clock_rate_est` field + `O,` wire record (mirrors the `K,` clock_osc record) for exact ON-path warm-resume.
- `tests/test_codebias_network_config.cpp` — default-off assertion + `NetworkEmitClockRate.TogglesAndDefaults` round-trip test.

## 4. Build + validation plan (run when free — cadence: smoke first)

1. **Build** (vcvars pattern): Debug `gnss_tests` + Release. Watch for the two new
   passes compiling; no new warnings expected.
2. **Unit**: `gnss_tests --gtest_filter=NetworkEmitClockRate.*:NetworkServiceProfile.*`
   then the full suite (expect green; the only schema change is the added toggle).
3. **OFF-path byte-identity (the invariant-6 gate)**: same-build golden A/B —
   `emit_clock_rate` off vs the committed baseline on a 10-ep 30 s config ⇒
   `network_solution.csv` **byte-identical** (also confirms the verification integer
   shifted only because the toggle joined the fingerprint, not the solution).
4. **ON smoke (the decoupling proof)** — turnkey A/B pair (created; differ in exactly
   one field, `network.emit_clock_rate`, both `parallel_reduce:false` for deterministic
   byte-id, GPS-only 30 s / decim 1 / 60 ep):
   `config/test_suite/_emit_c1_{off,on}.json`. Run both (Release, renamed exe, zombie
   pre-kill) and assert:
   - `_on`'s C records carry a nonzero field-4 C1 (the product's C-line), rate
     magnitude ~cm/s (the residual orbit-error drift, per A.5);
   - the solution columns of `_on` vs `_off` (c0, PB, CB, iono) are **byte-identical**
     — the decoupling holds, only c1 differs (this IS the OFF-path byte-id gate too,
     since `_off` = the toggle inert);
   - `T5_clock_rate` `c1_zoh_suppress` events fire only on genuine vanish-returns.
   Force-fire check (invariant #1): confirm a datum re-reference epoch reseeds the
   rate (no spurious C1 spike) and a >max_gap return ships C1=0 for one epoch.
5. **Field payoff (the real test)**: the sp3-apriori residual rate is small (~cm/s), so
   the C1 value shows most in **brdc_rts / real-time** mode, where the correction rate
   vs the broadcast apriori is large. The decisive A/B is a CONSUMER run: ZOH (c1=0)
   vs c1-extrapolation across an SSR-latency gap (down-sampled / delayed product),
   graded on the consumer vertical / saw-tooth amplitude. Ties to the M4
   "FULL = clock-wander" hypothesis and `project_closedloop_consumer_vertical_bug`.

## 5. Known follow-ups (not blocking)

- **Smoothing**: C1 is currently a raw finite-difference of the posterior clock (matches
  the B3b `clock_model` c1) — noisy (A.5: ~0.1–0.2 m/epoch c0 noise on a ~cm/s signal).
  A 2-state KF smoother (the unwired `clockOscUpdate` + `clockOscPredict` machinery is
  right there, decoupled) would trade a little lag for a cleaner rate; add as a tunable
  once the raw version is field-validated. Don't smooth blind — validate raw first.
- **brdc_rts sizing**: confirm the larger correction-vs-broadcast rate in RT mode is
  captured well by the finite-difference (bigger signal, better SNR — likely fine).
- The A/B smoke configs `config/test_suite/_emit_c1_{off,on}.json` derive from the 30 s
  GPS 3-card lineage (`_net305_3card_30s_gps_d8_em10_60ep_allpar`) with `parallel_reduce`
  forced OFF for deterministic byte-id (pcarry + seam stay on). For the Galileo asset (the
  biggest intrinsic rate, ~16×) re-run with a GPS+Gal `active_systems` — GPS-only is the
  faster, cleaner decoupling proof.
