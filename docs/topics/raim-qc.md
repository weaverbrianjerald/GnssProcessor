# `raim-qc` — Quality control — RAIM tiers, reliability, the censuses

Everything that screens data or state: the RAIM tiers (post-fit, within-epoch, the
carry-invariant re-host), Baarda reliability (MDB), block-Baarda and the station
congruence screen, the redundancy / prior-residual / prefit censuses, product-referenced
measurement admission, the T3-class null, and the withhold-era policy.

**Conventions and invariants**

- **RAIM is always on**, and the response doctrine is **adapt-and-EMIT within the detected
  epoch**. A withhold is the backstop and is graded as an adaptation failure unless the
  epoch is provably unrecoverable.
- Run-end counters are the ON-path telemetry (invariant #4): "0 driven / 0 folded" means
  the wiring is inert no matter how clean the log looks.
- A screen that is armed is not a screen that acts — the loader logging "ENABLED" while
  the tier screens nothing has happened twice.
- `trace_prefit_census` is the only always-on PRE-solve screen; post-fit σ̂₀ launders a
  parked cell through the free ambiguities.
- Every deferral is triaged field-blocking vs field-inert, with DATA for "inert"
  (invariant #3).

**Current posture (2026-09-14)** — RF-3 makes QC act on CODE rows, with the gross-LOS
screen made consumer-safe by landing the satellite stand-down BEFORE emission. See
[the network re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md).

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [DOY-129 defects](../cards/raim-qc/DAY129-DEFECTS-ABC.md) | guarded leg: seam withhold CORRECT, a₀₀ the wrong statistic, one-epoch CB resume window | closed | 2026-07-28 |
| [DOY-129 phases](../cards/raim-qc/DAY129-PHASES.md) | phase 0/1 results unrecorded; CB guard KEEP_OFF (pivot-only FALSIFIED), gate (iii) UNMET | closed | 2026-07-28 |
| [INC-4](../cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md) | re-entry vet: 16 of 17 onsets were re-entries; 200 m vet bound closes class B; default OFF | closed | 2026-08-02 |
| [Rider A](../cards/raim-qc/MDB-RIDER-A.md) | network MDB: byte-identical; faults under 9.4 m hide in the median code row; gates OWED | closed | 2026-08-04 |
| [Rider B](../cards/raim-qc/BLOCK-BAARDA-RIDER-B.md) | block-Baarda LOS census: BUILT, unit-gated, measure-only; every §12.8 field gate owed | closed | 2026-08-04 |
| [meas admission](../cards/raim-qc/MEAS-ADMISSION.md) | admission screen: corrupt channels refused at birth, era-2 withhold gone; ARMED on goldens | closed | 2026-08-12 |
| [meas admission impl](../cards/raim-qc/MEAS-ADMISSION-IMPL.md) | admission build: gates 1 to 4 PASS, BSHM births refused; 1b cannot pass; 5 to 7 ungraded | closed | 2026-08-12 |
| [DOY-129 anatomy](../cards/raim-qc/DAY129-FRAME-ANATOMY.md) | pivot CB ratchet (+49 m, no event response), clock wander, datum-only disc counter | closed | 2026-08-15 |
| [Rider C](../cards/raim-qc/CONGRUENCE-RIDER-C.md) | congruence screen: smoke PASS; detects held-coordinate errors, cannot measure them | closed | 2026-08-15 |
| [T3 null](../cards/raim-qc/T3-NULL-DERIVATION.md) | scale-blind statistic; station strata cut 4.46 % to 0.97 %; redundancy fix FALSIFIED (sim) | closed | 2026-08-15 |
| [global σ̂₀ rider](../cards/raim-qc/GLOBAL-SIGMA0-RIDER.md) | global σ̂₀ rider: measure-only, OFF identity and wiring PASS; triangle and cliff OWED | closed | 2026-08-15 |
| [prires census](../cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) | prior-residual census: smoke gates PASS, power gates 3 to 6 ungraded; armed on goldens | closed | 2026-08-15 |
| [redundancy census](../cards/raim-qc/REDUNDANCY-CENSUS.md) | testability census built and smoke-gated; the leg-19 known-answer gates stay OWED (§15) | closed | 2026-08-15 |
| [A4 withhold](../cards/raim-qc/WITHHOLD-ERA-RELEASE.md) | withhold-era escalation: era 29 to 1, VALIDATED; G3 FAIL later a cross-build artifact | closed | 2026-08-21 |
| [T3 field](../cards/raim-qc/T3-FIELD-AND-COLLATERALS.md) | field check: σ̂₀ floor-pinned, stratification 1.5×; detection gate PASSED; R2 + R4 BUILT | closed | 2026-08-21 |
| [withhold cliff](../cards/raim-qc/WITHHOLD-CLIFF-RETIREMENT.md) | cliff is a timer, not therapy; G3 FAIL did NOT REPRODUCE; promotion waits on level health | closed | 2026-08-23 |
| [RNEG](../cards/raim-qc/RNEG-REPARTITION.md) | r-negative re-partition: worst \|t\| 2.48e8 was a floor artifact; RNEG read in metres | closed | 2026-09-09 |
| [GL](../cards/raim-qc/GL-gross-los-screen.md) | gross-LOS screen: tail 13 to 16× smaller but bars FAILED; completed and ARMED by RF-3 | closed | 2026-09-14 |
| [GL-2 + R8](../cards/raim-qc/GL-2-whole-satellite-standdown.md) | stand-down FAILED (consumer 22.25 m in hour 4), left OFF; rx-CB carry exonerated | closed | 2026-09-14 |
| [DEC-day129-gate-policy](../decisions/DEC-day129-gate-policy.md) | gate rulings (degrade, 120-epoch cap, rms_ho ratio) unbuilt; superseded, SH and seams out of the re-founding | superseded | — |
| [T3 mechanism](../cards/raim-qc/T3-IONO-MECHANISM.md) | per-LOS ionosphere error drives the T3 heterogeneity (corr +0.926) and the DD integers | closed | — |

**Standing documents** (still at their original paths, indexed not moved):

- [SPEC_flicker_gap_integrity.md](../SPEC_flicker_gap_integrity.md)
- [SPEC_prefit_census_observability.md](../SPEC_prefit_census_observability.md)
- [SPEC_raim_pool_rehost.md](../SPEC_raim_pool_rehost.md)

---

Back to [INDEX.md](../INDEX.md).
