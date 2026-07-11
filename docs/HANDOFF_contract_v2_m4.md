# M4 — Product contract v2: combined-correction quality + age scaling

*Fable-specced 2026-07-11. Foundations: Psychas/Khodabandeh/Teunissen 2024b
(J Geod 98:15) and Zhang/Hou/Zha/Liu 2021 (J Geod 95:91) — see memory
`ppprtk-literature-pack` §7–8. Small card: emitter + IO + consumer stochastic
hook. Independent of Cards 3/4.*

## The findings this encodes

1. Individual products (clock, phase bias, iono) carry cm-level formal STDs
   during satellite-rise reconvergence, yet the COMBINED correction
   (CLK+SPB+ION per key) is mm-level because the components are strongly
   correlated (Zhang 2021 Figs. 2–5; Psychas Eq. 12). Consumers weighting by
   individual sigmas over-deweight fresh-satellite products by 10–100×; with
   the combined σ they can use products DURING reinitialization.
2. Correction latency amplifies combined uncertainty ~an order of magnitude
   at 30 s (Psychas Fig. 7) and drives code/phase-correction correlation to
   0.6–0.9 — both must enter the user stochastic model or the user's reported
   quality is "misleading and incorrect".
3. LICENSE: the corrections' TIME correlation decays and may be neglected by
   consumers (Psychas conclusions) — no consumer-side time-correlation
   machinery needed.

## Wire changes (v2 S/record lines, absent-tolerant like gid=/pdr=)

- Per (sat,σ) phase-bias record P: append optional `sigc=<m>` — the formal
  STD of the COMBINED phase correction (clock + phase bias + iono at a
  reference elevation; document the convention) — and per sat record C:
  optional `sigcc=<m>` (combined code correction) + `rho=<val>` (code/phase
  correction correlation). Producer computes these from the epoch solve's
  shared-block covariance (columns already available at emit; verify the
  reduced solve exposes clock↔PB↔iono cross-terms — if only via
  reconstruction, compute for EMITTED keys only, cost ≈ one back-substitution
  per key).
- Header/S-line: optional `agek=<val>` — the age-scaling coefficient: the
  consumer inflates combined variance as σc²·(1 + agek·Δt_age²) (fit the
  quadratic to the producer's own prediction-step covariance growth; Psychas
  Fig. 7 shape). Replaces/back-fills the ad-hoc URA ramp for phase use.
- Format: no version bump (optional tokens); replay round-trip via
  NetworkProductIO must preserve all three.

## Consumer changes

- `consume_combined_sigma` (default OFF, byte-identical): when ON and tokens
  present, the measurement-noise model for network-corrected observations
  uses σc (age-inflated) + rho instead of the individual-σ RSS. Fallback to
  v1 behavior per key when tokens absent (mixed-stream safe).
- Keep ignoring correction time-correlation (finding 3) — document in the
  consumer profile doc.

## Tests / validation

1. IO round-trip incl. absent-token back-compat (extend
   test_network_product_io patterns).
2. Emitter unit: synthetic covariance with known cross-terms — sigc/sigcc/rho
   reproduce hand-computed values; fresh-satellite case shows
   sigc ≪ individual RSS (the load-bearing inequality).
3. Field: closed-loop consumer A/B (v1 weighting vs consume_combined_sigma)
   on a run with a mid-window rising satellite — ACCEPTANCE: the v2 arm keeps
   using the riser's products (no deweight cliff) with equal-or-better
   position/AR stats; sigma_gate epochs do not increase.
