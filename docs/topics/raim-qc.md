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
| [DOY-129 defects](../cards/raim-qc/DAY129-DEFECTS-ABC.md) | defects A/B/C from the instrumented leg, the five wiring facts, and the measured policy… | closed | 2026-07-28 |
| [DOY-129 phases](../cards/raim-qc/DAY129-PHASES.md) | phases 0–2: the closed-loop replay, the trigger instrumentation, and the gated guards | closed | 2026-07-28 |
| [INC-4](../cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md) | the arc RE-ENTRY vet — there were no mid-arc onsets; `adm_revet_hold` erases the event | closed | 2026-08-02 |
| [Rider A](../cards/raim-qc/MDB-RIDER-A.md) | Baarda reliability numbers (MDB) on the network path — `network.mdb` | closed | 2026-08-04 |
| [Rider B](../cards/raim-qc/BLOCK-BAARDA-RIDER-B.md) | BlockFormal — the (station, sat) LOS block outlier census, `network.block_baarda` | closed | 2026-08-04 |
| [meas admission](../cards/raim-qc/MEAS-ADMISSION.md) | the product-referenced admission screen — the fault class no differential screen can se… | closed | 2026-08-12 |
| [meas admission impl](../cards/raim-qc/MEAS-ADMISSION-IMPL.md) | the 2026-08-01 implementation record — every deviation from the spec, the gates as run,… | closed | 2026-08-12 |
| [DOY-129 anatomy](../cards/raim-qc/DAY129-FRAME-ANATOMY.md) | the answered questions — the pinned pivot's code bias, the a₀₀ collapse anatomy, the qu… | closed | 2026-08-15 |
| [Rider C](../cards/raim-qc/CONGRUENCE-RIDER-C.md) | the station congruence screen, `network.congruence_screen` | closed | 2026-08-15 |
| [T3 null](../cards/raim-qc/T3-NULL-DERIVATION.md) | the statistic as actually computed, the invariance theorem, the falsified redundancy st… | closed | 2026-08-15 |
| [global σ̂₀ rider](../cards/raim-qc/GLOBAL-SIGMA0-RIDER.md) | `network.prior_residual_global_sigma0` — three sums, one ratio, nothing acted on | closed | 2026-08-15 |
| [prires census](../cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) | the prior-residual census — the binding precedent, INC-0..3, the wiring sites, the buil… | closed | 2026-08-15 |
| [redundancy census](../cards/raim-qc/REDUNDANCY-CENSUS.md) | the row-redundancy and unobservable-local censuses — design, cost model, wiring, build … | closed | 2026-08-15 |
| [A4 withhold](../cards/raim-qc/WITHHOLD-ERA-RELEASE.md) | the withhold-era release policy — the defect named exactly, the v2 design, the pre-regi… | closed | 2026-08-21 |
| [T3 field](../cards/raim-qc/T3-FIELD-AND-COLLATERALS.md) | the T3 class and its two collateral defects, the field check, the detection-power gate,… | closed | 2026-08-21 |
| [withhold cliff](../cards/raim-qc/WITHHOLD-CLIFF-RETIREMENT.md) | the retirement census — every G3 number was on a retired binary; the cliff does not rep… | closed | 2026-08-23 |
| [RNEG](../cards/raim-qc/RNEG-REPARTITION.md) | the RNEG re-partition — what a `post_var > fold_var` row is, and the numerator card it … | closed | 2026-09-09 |
| [GL](../cards/raim-qc/GL-gross-los-screen.md) | the gross line-of-sight screen | open | 2026-09-14 |
| [GL-2 + R8](../cards/raim-qc/GL-2-whole-satellite-standdown.md) | the whole-satellite stand-down, and R8 — the bulk drift's channel | open | 2026-09-14 |
| [DEC-day129-gate-policy](../decisions/DEC-day129-gate-policy.md) | the gate-policy ruling — DEGRADE, DO NOT WITHHOLD; the statistic; the CB-excursion resu… | open | — |
| [T3 mechanism](../cards/raim-qc/T3-IONO-MECHANISM.md) | the per-row census, the falsification ledger, and the MECHANISM FOUND — per-LOS ionosph… | closed | — |

**Standing documents** (still at their original paths, indexed not moved):

- [SPEC_flicker_gap_integrity.md](../SPEC_flicker_gap_integrity.md)
- [SPEC_prefit_census_observability.md](../SPEC_prefit_census_observability.md)
- [SPEC_raim_pool_rehost.md](../SPEC_raim_pool_rehost.md)

---

Back to [INDEX.md](../INDEX.md).
