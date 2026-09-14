# W6+SH brief — the design brief for the W6+SH synthetic arm — superseded by the arm itself

**STATUS**

- **id:** W6+SH brief
- **topic:** sim-bed
- **status:** superseded
- **summary:** the design brief for the W6+SH synthetic arm — superseded by the arm itself
- **dates in the text:** 2026-08-12
- **knobs / posture:** —
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** `runs/cbq_elcut25`
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md` — §11

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
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

