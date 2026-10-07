# iono-field agreement: cross-station iono coherence and the field cutoff A/B

**STATUS**

- **id:** iono-field agreement
- **topic:** ionosphere
- **status:** closed
- **summary:** The GF iono input is coherent between stations both viewing high (median corr 0.981 at 300 to 700 km), BROKEN at low-high; a 25° iono cutoff trims the detrended field wander 8 %.
- **dates in the text:** 2026-08-11 to 2026-08-12
- **knobs / posture:** `estimation.ionosphere_weighting.elevation_cutoff_deg` 15 vs 25 (arm config `_cbq_12h_elcut25.json` = the retained control + GIM, 12 h, cold); instrument `scripts/field_iono_pair_agreement.py` (raw observables, settled 10 h)
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** none
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md` §9.10, §9.10.1

**Outcome.** Two questions: is the field's GF iono input coherent between stations at the scales the SH model assumes, and does low-elevation incoherence drive the wire's common wander? Coherent when both stations view high (median rel-rms 0.173 at 300 to 700 km); in the low-high configuration it collapses to corr 0.381 and rel-rms 1.130, on only 3 series. The 12 h field A/B at 25° confirmed the direction with availability parity, but about 84 % of the wander variance survived: a cheap trim adopted on the FT63 lineage, not the cure. The truth-graded sim A/B later priced the cut's separability cost ([elev-cutoff A/B](../ionosphere/ELEV-CUTOFF-AB.md)).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
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

