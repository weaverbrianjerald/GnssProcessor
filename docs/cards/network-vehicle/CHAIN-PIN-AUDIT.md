# chain-pin audit: the eq34 chain is complete, and rungs 2 and 6 re-attributed

**STATUS**

- **id:** chain-pin audit
- **topic:** network-vehicle
- **status:** closed
- **summary:** The eq34 chain is COMPLETE (every declared pin folded and landed); rungs 2 and 6 were SH-gate withholds from cold zero-target pins, a sim-only artifact; `cb_kaula_prior` FALSIFIED as the protection.
- **dates in the text:** 2026-08-12
- **knobs / posture:** `network.phase_datum_eq34`, `network.station_pd_pin_aware`; the fix it names, `network.phase_datum_eq34_seed_round`
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** none as `runs/` paths; arms `_simnet300_wsh_n50_pinaudit`, `_simnet300_wsh_n50_kaula`, `_simnet300_wsh_n50_eq34kaula`
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md` §12.8, 12.8.1 to 12.8.4

**Outcome.** The question: do the eq34 chain pins the election declares reach the solve? A log-only audit counts 1558 pins declared, folded and landed per signal group over 19 epochs; the rank gate passes, the L2W common mode lifted ~17×. The 0/19 emission was the SH health gate: cold zero-target pins against the sim's km-scale integer draws drove a₀₀ to −19 000 TECU. Field phase sits within metres of code, so the same chain emits at 303 stations. The named fix was then BUILT and VALIDATED default OFF, not proposed for field lineages ([SPEC_eq34_seed_round](../../SPEC_eq34_seed_round.md)); the re-founding goldens run `phase_datum_eq34` off.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
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

