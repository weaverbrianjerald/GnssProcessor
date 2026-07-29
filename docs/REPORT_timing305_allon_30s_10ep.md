# REPORT — 305-station "everything-on" timing baseline (30 s tier, 10-ep smoke)

*2026-07-28 (Fable). Run: `_net305_timing_gps_allon_10ep.json` (clone of the validated
`_val305_gps_allbias_sh_10ep.json`, only `product_output_path` changed), Release
`gnss_processor_fable_timing.exe` (build of 07:25 same day, newer than every source),
fresh cwd `val_out/fable_timing305/`, 20 threads. Concurrent load: the s2z A/B arm at
~1 core (~5% of box) for the whole run — inside the ±5 % single-run noise floor
(HANDOFF_perlos_parallel_sweeps §1); ratios are load-bearing, absolutes ±5 %.*

Shape: 305 stations (303 active), GPS-only, 30 s, freq/channel = all, OSB
(`estimate_sat_code_bias`), SH deg-15 + trace, full Card-4 invariant carry + VCE +
coupling trio + full-cov shared carry, D5 innovation gate, IF-TD slip, H1 adaptive
deweight, PB-emit filter, dd-frac trace (500 pairs), closed-loop KOKV consumer,
`parallel_stations` + `parallel_seam` + `parallel_carry` all confirmed armed in-log.
Border 710 shared cols; 32 sats; carried DDs peak z-dim **7 938**, max component
K_c **2 405**; 912 slip refloats, 9 083 births over the run.

## Headline

| | |
|---|---|
| Epoch-loop wall | **304.88 s / 11 epochs = 27.72 s/epoch** |
| Real-time duty @ 30 s cadence | **0.92× — already marginal** |
| Startup (ingest+products+init) | 58 s one-time (RINEX 306 sta = **0.52 s** parallel; product cache 104 544 orbits + 1 045 243 clocks dominates) |
| Total wall (launch→exit) | 363.0 s |

## Where the epoch goes (run totals ÷ 11)

| Block | ms/ep | % | Notes |
|---|---:|---:|---|
| **Card-4 carry pipeline, total** | **15 742** | **56.8** | dense-K physics (C2 proof: no bounded exact factored form) |
| — fold `foldReducedDdPriors` (in solve-glue) | 9 558 | 34.5 | per-sta-loop 5 822 (21.0 %) + **M-inverse 3 036 (11.0 %, serial K×K Cholesky)** + GMGᵀ 700 |
| — installPosterior | 3 787 | 13.7 | post-cov 2 871 (10.4 %) + D5-gate 876 + post-mean 28 |
| — dd-build (arc graph + basis-project W·Pᶻ·Wᵀ) | 2 200 | 7.9 | driver basis-project 1 773 (6.4 %) |
| **cov-consumers (608 reconstructStation/ep)** | **6 945** | **25.1** | dd-frac trace 3 585 (304 calls) + T1a/H1 sweep 3 358 (303 calls) — two full per-station recon sweeps doing the same reconstruction |
| assembly wall (per-sta OMP; addStation 3 135 inside) | 3 166 | 11.4 | |
| reduce (border Schur, 1/ep) | 1 368 | 4.9 | |
| solve (dense border, 710 cols) | **41** | 0.1 | the actual solve is FREE |
| cov-inverse | 93 | 0.3 | |
| slip-scans + pre-asm + emit + rest | ~370 | 1.4 | emit-store only 65 ms/ep |
| unattributed | ~13 | 0.0 | accounting closes |

Cross-check: dd-frac tracer self-report "pool 3 381 ms (305 sta)" matches its bracket.
K_c histogram (emitted, log2 buckets): 0 0 0 0 0 0 11 11 11 **77** — the fold cost lives
in ~7 giant (≥256-dim) components per epoch. Mean fundamental-cycle length 4.9 / max 10
over 86 853 folded chords.

## Savings ladder (measured bases; closed doors respected)

Closed by prior cards — do NOT re-propose: exact factored carry (C2 impossibility,
dense Σ_ss global net; only remaining lead = Σ_ss sparsity/rank probe), parallel
Cholesky inside current Eigen (L1: 1.06×), config-stripping of "other" (≤8 %,
structural), marginalization as K-reducer (LF-1 INC-3: K rebuilt per epoch).

1. **P1 sweep fusion** (perlos §INC-P1, unbuilt; was the "next measured card" when the
   carry card closed at "sufficient perf"). dd-frac + T1a/H1 run one recon sweep instead
   of two: 608→~304 calls/ep. Expected ≈ −3.4 s/ep (−12 %). Byte-id contract.
2. **RT profile drops dd-frac entirely** (it is B5 diagnostics): bracket says −3.6 s/ep
   (−13 %). Caveat: Run-D on the 300 s tier saw the bracketed time vanish but wall move
   only −1.1 % (±5 % noise) — A/B before claiming. Fusion (1) subsumes most of this.
3. **Threaded-BLAS (MKL) Eigen backend for the fold M-inverse** — the serial K×K
   Cholesky factor is now 11.0 % of the epoch (it was the L1 residual when L1 was
   judged not-worth-it; parallel_carry's relaxed-equivalence license already covers this
   path). 4–8× on the factor ⇒ ≈ −2.3…−2.7 s/ep (−9 %).
4. **D1 selected inverse / per-station recon batching** (planned, unbuilt): attacks the
   per-call cost of the remaining ~304 recon/ep after fusion.
5. **Forest partitioning / component cap** (approx, field-graded — the only true
   K-reducer): all K² terms (fold loop, M-inv, post-cov, basis-project ≈ 49 % of epoch)
   scale with component size; 7 giant components ≥256 dominate. The one big structural
   door still open; quality card, not a perf tweak.

Realistic near-term stack (1+3, keeping all estimation identical): 27.7 → ~22 s/ep.
With (5) landing well, the carry block could plausibly halve again — but that is a
quality-graded card, not free.

## 1 s real-time verdict

At 30 s cadence the producer is at 0.92× real time; a 1 s cadence has a 1 s budget —
**28× off, and the carry pipeline alone (15.7 s/ep) busts it 16×**. Its cost is
per-epoch dense-K algebra, independent of how little the state moved in 1 s, and C2
proved the dense form is intrinsic under the current carry-every-epoch design. No
optimization ladder reaches 1 s at 305 stations.

The architecture that does (matches IGS-RTS/commercial SSR practice, and our own C1
finding that emitted c0 is a random walk at 30 s ⇒ ZOH is MMSE ⇒ clocks must be
*measured* at the served rate while everything else is ZOH-fine):

- **Split cadence.** Full solve + carry + QC at 5–30 s (today's path, plus ladder).
  Per-second: a clocks-only update — freeze geometry/ambiguities/iono/biases from the
  slow loop, solve ~340 clock states. The full 710-col border solve is 41 ms today;
  a clocks-only pass with cached geometry is well under the 1 s budget including
  measurement build (assembly is per-station-parallel and mostly addStation normal
  forming, which shrinks to the clock columns).
- Prerequisite: the latent **R11 emission-continuity defect** (clock-rate C1 card)
  surfaces in any high-rate clock emission — fix before serving 1 s clocks.
- A **regional (~50-sta) 1 s net** is plausible even without split cadence: sweeps and
  assembly scale ~linearly with stations, and K ~ linearly ⇒ K² fold cost ÷ ~35.

## Artifacts

- Config: `config/test_suite/_net305_timing_gps_allon_10ep.json` (reusable timing bench)
- Run dir: `val_out/fable_timing305/` (logs/main_log.log holds D0/D0b/INC-C0 tables;
  products.txt 215 KB, 11/11 epochs emitted; solution/vtec/dd-frac CSVs)
- Exe: `build/x64-Release/gnss_processor_fable_timing.exe` (copy of 07:25 build)
