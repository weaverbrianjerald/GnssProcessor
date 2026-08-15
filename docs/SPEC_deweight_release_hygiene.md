# SPEC — iono-deweight RELEASE hygiene (the FT63 era-2 owner)

**Status**: SPEC'd 2026-08-01 (the session that root-caused era-2); INC-3b BUILT + graded 2026-08-01
(§7.5). **Scope discipline**: default-OFF knobs, OFF structurally byte-identical, NOT fingerprinted
(forward-only release-time policy — the eq34-hygiene precedent), so shared-snapshot replay legs stay
A/B-legal.
**Memory context**: `project_ft63_fast_tier` (root cause + instrument), `project_adaptive_iono_q_network`
(predicted this fix class: "gate-exit reset/relevel"), `project_iono_deweight_card` (the gate hardened here).
**⚠ Provenance**: every `runs/<name>` here — `ft63_leg19_{off,rhsat,dwr,dwr_k10,dwr_k30,dwr_nd,dwrat,dwroff}`,
`ft63_dwr_smoke` — is absent from the working tree; `runs/` was archived to
`D:\GnssProcessor_archive\2026-08-12\runs\` on 2026-08-12 (CLAUDE.md) and the paths resolve there.
**⚠ Anchor drift** (re-grepped at HEAD 2026-08-15): the 2026-08-01 text cited
`ApplicationNetworkAssembly.cpp:263` for `y = em.innovations` (now `:264`) and `:1000`/`:1030` for the tie
row's `r.innov = -x(iono_idx)` (now `:1078` / `:1108`); `ApplicationNetworkSetup.cpp:559` for the single
network `setStateVector` (now `:574`); `NetworkShHealth.cpp:230` for the frame-broken slant residual (now
`:242`, `resid = x_r(...) − sh_slant`, no `x_lin`). `NetworkShDevVtec.cpp:115` is unchanged. As-built
seam: `filter/NetworkIonoDwRelease.{hpp,cpp}` (`NetworkIonoDwRelease.cpp:4`), driver INC-1 call site
`ApplicationNetwork.cpp:1470`, run-end block `NetworkTeardown.cpp:730` (invariant #4), §7.2's proof cited
back by `NetworkRaim.cpp:574`.

---

> ## ⛔ §1's CAUSAL CHAIN IS FALSIFIED (2026-08-01, implementing session)
>
> Read §7 before §1. The defect CLASS below is right; the VALVE is not. BSHM was never deweighted — it
> sat at `w = 1.0` throughout and its only `IONO_DW` event in the whole control leg is an **engage** at
> the break. INC-1 and INC-2 are both keyed on the gate weight and therefore have **zero opportunity** at
> this event. INC-3b (§7) is the increment that can fire, and is what got built and graded.

## 1. Root cause this card fixes (evidence on disk; do not re-derive)

FT63 DOY-129 era-2 (19:26:00, TOW 329160, a00 38.7 → 5739 TECU, 76-min SH-gate era, 181 m never-healed
exit scar). Named mechanically by the reduced-RHS class-attribution instrument (`network.rhs_attrib_tow`,
closure ≤ 5e-11) on `runs/ft63_leg19_rhsat` (replay from `ft63_24h_allon` snap_20):

- **The parked garbage**: (BSHM00ISR, G07) — C1C+L1C-only LOS, raw RINEX clean, LLI clean, tracking
  continuous — carries a STEADY prefit code innovation of **+1.88·10⁵ m** (G16@BSHM: +7.1·10⁴ m) against
  a garbage-parked slant-iono / ambiguity pair: phase self-cancels via the float ambiguity (y_phase
  −6.4 km), the code row stays naked at w≈2. Parked through at least 329040–329160, station σ̂₀ clean (the
  local states absorb it — BSHM's reduced C00 load +0.002).
- **The valve**: BSHM's adaptive iono-constraint deweight (station-scalar) sat at weight **0**
  (n_dw_zero=3, dw_min 0.0) through 19:17–19:25:30 — its SH-tie rows for the affected slants were **not
  folded**, so the garbage slant was decoupled from the field (benign local absorption).
- **The discharge**: at 329160 the gate **RELEASES** (n_dw_zero 3→0, dw_min 0.0→0.25 — the rate_limit
  0.25 slew; a fifth tie row appears in BSHM's stack). The re-folded tie couples the slant column to the
  SH block in the Schur reduction and the code row's 188-km load leaks into a00: BSHM obs_code C00
  contribution 0.002 → **+6493** (layer-2 +3623), the obs_code class balance flips −5716 → +1395 (net
  imbalance ≈ +6500), the field discharges +5.7k TECU, the SH gate engages. The row itself is IDENTICAL
  across the break (same w, same y) — only the coupling changed.
- All seven prior falsification arms (hygiene, dwfast, ablate4, sion, noslip, aiq, code/sat scans) are
  explained: none touched the release re-coupling or the parked cells. dwfast (rate_limit 1.0) releases
  0→1 in one epoch — same discharge, break unchanged ✓. Era-1 (07:32, a00 1666, 48 min) is the same class
  (22 engage / 15 release events on the day; grade observationally in §6, do not over-claim).

Two co-defects surfaced (SEPARATE cards, pre-declared here): the **seed defect** — what seeded
(BSHM,G07)'s slant/ambiguity 188 km wrong; an L1-only LOS admitted during its station's zero-weight era
has NO iono observability, so the seed source for single-band fresh arcs needs its own probe (open); and
the **band-loss policy** (flicker INC-0) — BSHM/G07 being L1-only-processed at all vs LOS-dropped is the
standing inconsistency, unchanged here.

## 2. Design — release is an EVENT with a wired response (standing invariant #1)

The deweight gate's zero-weight era makes a station's slant states carry UNVALIDATED values (decoupled
from the field precisely while the storm made them untrustworthy). Re-coupling them silently is the
defect. Three increments, independently knobbed:

### INC-1 (core): release-time state hygiene
`network.iono_dw_release_reseed` (bool, default false). When a station's gate weight RISES from ≤
`release_floor_eps` (detect in the gate's `advance()` slew, not by comparing to exactly 0.0):
| # | INC-1 action |
|---|---|
| 1 | **Reseed** every (station, sat) slant-iono deviation state of that station from the CURRENT SH field evaluation (the fresh-LOS init path's field-evaluate) — ⛔ **but see §7.2: a bare state reseed is a solve NO-OP**, retained as counted BOOKKEEPING only; |
| 2 | **Refloat** the station's float ambiguities (the phase side re-seeds on the reseeded slant — the same funnel the slip response uses; count them); |
| 3 | **Erase** the station's carried per-LOS deviation stash entries (d5_keep must not resurrect the parked value — the v3k keep-policy explicitly yields to an event-driven erase; count separately). |


The tie rows then re-engage against FRESH states: the release folds information, not garbage.

### INC-2: while-decoupled park bound
`network.iono_dw_park_bound_tecu` (double, default 0 = off). While a station's gate weight is ≤ eps, any
slant deviation state whose |value| exceeds the bound is reset to the field evaluation immediately
(counted). Belt-and-suspenders: bounds the garbage a zero-weight era can accumulate, so even a missed
release path (e.g. a warm resume INTO a released state) has a bounded shock. A physical bound of O(300
TECU) is 3 orders below the observed 1.15e6-TECU-equivalent park.

### INC-3: release ramp gate on the tie rows
`network.iono_dw_release_validate_k` (double, default 0 = off). On release, fold each re-engaged tie row
only if its standardized innovation |slant − field| / σ passes k; a failing slant stays decoupled
(counted, one line per epoch) and re-tests next epoch. The row-level screen for anything INC-1's reseed
cannot reach (e.g. a code-bias-side park).

## 3. Wiring sites (as spec'd Aug-1; the as-built seam is `filter/NetworkIonoDwRelease.{hpp,cpp}`)

| # | site |
|---|---|
| 1 | Gate state + slew: `include/filter/NetworkIonoDeweightGate.hpp` (header-only; per-station `advance()` — the release edge is detectable there; expose an epoch release-list). |
| 2 | Tie-row build (where the deweight scalar applies / rows are omitted): the SH↔slant constraint fold in `ApplicationNetworkAssembly` (the `iono_sh_row` class of the attribution instrument; the row that appeared at 329160). |
| 3 | Reseed/refloat plumbing: the slip-funnel + fresh-LOS init paths (`StateManager::isAmbiguityPendingReseed`, the T5v2 erase projection in `ApplicationNetworkAssembly`); deviation stash = the rep_error carry (v3k demean/d5_keep) in `NetworkRunContext`. |
| 4 | Counters: `NetworkServiceCounters` (aiq_*/hyg patterns); run-end line in `NetworkTeardown.cpp`; PHASE/IONO tier events `dw_release_reseed` (station, n_slants, n_refloats), `dw_park_bound_reset`, `dw_release_row_held`. |
| 5 | Fingerprint: NOT appended (release-time policy; stored-state meaning unchanged) — exemption recorded at `networkSnapshotFingerprint` like the hygiene card did. |


## 4. Unit tests

`tests/test_iono_dw_release_hygiene.cpp` (one suite): (1) release edge detected exactly once per era end;
(2) INC-1 reseeds slants + refloats ambiguities + erases stash for the released station only; (3) INC-2
bound resets a parked state mid-era; (4) INC-3 holds a failing tie row and re-tests; (5) OFF ⇒
structurally byte-identical (no release list allocated).

## 5. Field validation (standing FT63 replay harness)

⛔ **SUPERSEDED IN PART by §7.4** — gate 3 as written is ungradeable on this event (it contains no
release). Grade INC-3b instead, per §7.4; what was actually run is §7.5. The gate list is kept for the
INC-1/INC-2 legs that still need a run containing a genuine release.

Controls: `runs/ft63_leg19_rhsat` (attribution baseline, break at 5738.6), **`runs/ft63_leg19_off`** (the
OFF leg — §7.1/§7.5 and `SPEC_flicker_gap_integrity` §6B use this name; an earlier draft said
`runs/ft63_leg19_base`, which is **wrong**), snap_20.

| gate | content |
|---|---|
| 1 | 10-ep smoke: knobs parse + arming lines + OFF twin byte-identical. |
| 2 | OFF leg from snap_20 (new build): a00 5738.6 at 329160 (bit-check). |
| 3 | **INC-1 leg**: expect `dw_release_reseed` for the 3 zero-weight stations at 19:26:00; **no SH-gate engagement at 329160; a00 stays < 150 through 19:40**; the (BSHM,G07) code row's post-reseed innovation collapses from 1.88e5 to O(code noise) — verify via a `rhs_attrib_tow: 329160` dump (obs_code class total back to O(±few hundred), BSHM station row O(1)). |
| 4 | QC flat vs OFF: D5 flags ±few %, rx-CB withholds ~14-15%, σ̂₀ 1.00; the reseed's own refloat count small (≈ n_slants of 3 stations). |
| 5 | 24h rerun: era-2 absent or reduced to a non-gate transient; era-1 graded observationally (same-class hypothesis, not claimed); **exit-scar grade**: across-gap total_IF span ≪ 181 m; day-end quiescent trend NO-DRIFT. |
| 6 | Product claims on total_IF only (promotion-ledger convention). |

## 6. Deferral triage (pre-declared)

| # | deferral |
|---|---|
| 1 | The 188-km seed-source probe: OPEN, separate card (this card makes it field-inert by resetting it at the valve; the seed defect remains latent until probed — say so in the report). |
| 2 | Warm resume INTO a zero-weight era whose release happened while down: covered by INC-2 only (bounded); full coverage deferred until a real case. |
| 3 | The gate's own policy (release thresholds/hysteresis): unchanged — this card adds a response to release, it does not retune the gate. |

---

## 7. CORRECTION — the valve is the ADMISSION gate, not the release (2026-08-01)

Written by the implementing session, from the control run's own traces. §1's defect CLASS survives; its
causal chain does not.

### 7.1 What the evidence says

`runs/ft63_leg19_off/logs/` (the OFF control, break at TOW 329160, a00 39.5 → 5738.6):

| # | evidence |
|---|---|
| 1 | **BSHM was never deweighted.** Its only `IONO_DW` row in the entire leg is `329160, engage, BSHM00ISR, z=48.746569, w=0.7500`. It sat at `w = 1.0` — fully coupled, every tie row folding — through the whole pre-break window. Its z at the break is the gate REACTING to the break it was blamed for. |
| 2 | **`n_dw_zero = 3 / dw_min_w = 0.0` are network AGGREGATES** (`aggregate()`), not BSHM's weight. §1 read them as station-specific. The stations actually at zero were LCK400IND / LMMF00MTQ / OAFA00ARG / POVE00BRA / SAVO00BRA — the five that released at 329160. |
| 3 | **The release wave lands one epoch too late anyway.** The `ep_dw_*` harvest is POST-`advance()` (`NetworkShHealth.cpp:326-329`), so the epoch-status row stamped 329160 describes the field epoch **329190** will use. The field that fed the 329160 solve is the row stamped 329130 — aggregate-identical to the four epochs before it. |
| 4 | **The fifth tie row is real.** `runs/ft63_leg19_rhsat/logs/rhs_attrib/rows_*.csv`: BSHM goes 4 → 5 `iono_sh_row` across the break, and the parked `obs_code G07 y = +1.88e5 m` row is identical either side (w 1.9658 → 1.9977). But with `w_s = 1.0` on both sides, that row appeared because a satellite crossed the `elevation >= iw.elevation_cutoff_deg` / `h2local` gate and entered the tie stack for the FIRST time. |

⇒ A slant column carrying an unvalidated parked value WAS tied to the SH field and DID discharge into a00
— exactly as §1 says. It was admitted, not re-coupled.

### 7.2 A bare state reseed is a solve NO-OP

Independently, and fatal to INC-1's first bullet as written. The producer forms every innovation against
the per-station linearization point (data rows through `em.innovations`,
`ApplicationNetworkAssembly.cpp:264`; the tie row through `r.innov = -x(iono_idx)`, `:1078` / `:1108`) and
never writes the solve back (`st.sm->setStateVector` appears once on the network path, at
`ApplicationNetworkSetup.cpp:574`). Shifting `x_lin` on local column c by δ therefore sends
`y → y − A_r e_c δ`, hence `u_s → u_s − N_sr e_c δ` and `u_r → u_r − N_rr e_c δ`, and

    b_red = u_s − N_sr N_rr⁻¹ u_r  →  b_red    (exactly)

so the shared solution — and the absolute estimate `x_lin + x_r` — is unchanged. **`|x_lin − field|` is a
frame-arbitrary number and must not be screened on.**

The three genuinely non-invariant actuators in this card are: the **stash erase** (sets the tie row's
`dev_pred_slant` / `var_pred_slant`, uncompensated), the **ambiguity refloat** (drops the arc's DD chord
from `A_old` under `carry_invariant`), and the **row hold** (a row-set change). The slant state write is
retained as BOOKKEEPING — it makes the frame-broken readouts honest — and is counted separately so it can
never be mistaken for an effect.

### 7.3 INC-3b — the increment with opportunity

`network.iono_dw_admit_validate_k` (double, 0 = off). The §2 INC-3 screen applied at a LOS's **first** tie
row. A LOS the seam has not admitted is held for one epoch — during which `networkShDevVtec` still
harvests its deviation (that harvest is gated on the deweight weight, not on whether the row folded) —
and admitted at the end of that epoch iff the harvested deviation passes k. Quarantine, admission and
rejection are counted apart: every rising satellite serves one quarantine epoch, but only a REJECT is the
guarded event.

The screen statistic is the harvested deviation standardized by `representation_sigma_tecu`, per §7.2. All
of INC-2/INC-3/INC-3b therefore require the T5 deviation channel (`rep_error_tau_s > 0`) and disarm with a
warning without it, rather than falling back to the linearization point.

A one-shot **start/resume SEED** admits every present LOS unscreened on the first seam pass
(`ctx.carry.dw_tie_seeded`). Nothing here is serialized, so without it a resumed leg would hold EVERY tie
row for one epoch — a self-inflicted shock far larger than the event guarded against — and there is
nothing to guard at start anyway, since the guard is about coupling to an ALREADY-ESTABLISHED field.

**k calibration** (from the OFF control's `network_vtec_sh_health.csv`): pre-break the worst standardized
LOS misfit is ~20–40 (`slant_resid_max_m` 27–34 m against a per-LOS σ of 0.8–2.0 m); at the break it is
~87,000 (`slant_resid_max_m` 130,108 m). k = 100 is ~3× above the storm tail and ~3 orders below the park.
The health trace's `slant_resid_*` omits `x_lin` (`NetworkShHealth.cpp:242`) and is one of the
frame-broken readouts — used here only for an order-of-magnitude bracket, not as the statistic.

### 7.5 VALIDATION RECORD (2026-08-01, `gnss_processor_dwr.exe`, Release)

Unit: 1054/1054 Debug cases pass (1 pre-existing fixture skip); new suite `IonoDwReleaseHygiene`, 8 cases.

| gate 1 record |
|---|
| **Smoke** `runs/ft63_dwr_smoke` (cold, 10 ep, all four increments armed): all four arm and log; the start seed admitted 390 LOS; INC-3b showed 2 quarantines → 11 admissions → 0 rejects; INC-1/INC-2 correctly reported ZERO OPPORTUNITY. |
| **Gate 1 — OFF byte-identity: PASS (re-verified on the FINAL build).** `runs/ft63_leg19_dwroff` (same build, nothing armed) vs the pre-existing `runs/ft63_leg19_off`: `network_vtec_sh_health.csv`, `network_solution.csv` and `network_events.csv` are md5-IDENTICAL. `network_epoch_status.csv` differs only by columns a CONCURRENT session is adding live (`n_uncheckable_rows`, `pc_code_*`, …; 29 → 48 → 49 over the session); the 29 pre-existing columns are identical on all 121 rows. The OFF twin reproduces the documented break exactly — a00 SUM **5738.62** at 329160 against the recorded 5738.6. |
| ⚠️ **Unexplained crash, NOT this card's:** one run exited 0xC0000409 (STATUS_STACK_BUFFER_OVERRUN) after COLD-STARTING — it was pointed at a stale snapshot path (`age 0.000 s ⇒ COLD START`) by operator error. It used the **OFF** config, where this seam returns on its first line and the assembly's admission check is skipped entirely, so the fault is not in this card; the exe also carried the concurrent session's in-flight `NetworkRhsAttrib` / epoch-status work. Not reproduced, not diagnosed. Worth a look before anything here is committed. |

**Gate 2 — the event: the primary break is PREVENTED.** `runs/ft63_leg19_dwr` (INC-3b only, k = 100), same
snapshot (`snap_20`), `state_snapshot_require: true` (so the shared-snapshot A/B is proven legal — a
fingerprinted knob would have hard-failed):

| TOW | OFF a00 SUM | ON a00 SUM | ON disposition |
|---|---|---|---|
| 329130 | 39.47 | 39.48 | emitted |
| **329160** | **5738.62** | **38.85** | **emitted** |
| 329190 | 4929.72 | 521.42 | sh_gate_withheld |
| 330000 | 1393.05 | −43.43 | sh_gate_withheld |

Window peak |a00 SUM| 5738.6 → **830.6 (6.9×)**; by 19:40 the armed leg is back in band (−43) while OFF is
still at 1393. **The guard fired on exactly the predicted cell, at exactly the predicted epoch:**

    2313,329160.000,IONO_DW,dw_admit_reject,G07,BSHM00ISR,123374.94,100.0,
      hold_tie_row,slant 1.878e+05 m vs field 6.938 m (dev 616874.70 TECU; sigma_rep 5.00)

i.e. (BSHM00ISR, G07) presented its FIRST tie row carrying the +1.88e5 m park and was withheld. The
corrected valve confirmed mechanically, and an independent re-confirmation of §7.1: a
deweight-release-keyed guard could not have touched it.

**k calibration verdict:** 1492 of 1582 quarantines were rejected, but **zero rejections occurred before
the break epoch** — every one is aftermath (once a00 is wrong, the field evaluation is wrong for every new
LOS). So k = 100 is not over-tight in healthy operation. Reject-z distribution: p50 181, p90 378, p95 696,
p99 1.11e4, max 1.24e5 (BSHM/G07, 17 consecutive epochs).

⚠️ **Gate 3 — NOT a clean pass; era-2 is NOT closed.** A residual excursion appears one epoch LATER
(329190, 521 TECU) and the SH gate still engages, so "a00 < 150 through 19:40" fails even though the break
it was aimed at is gone. Three hypotheses tested, all three FALSIFIED — recorded so they are not retried:

| # | hypothesis, all FALSIFIED |
|---|---|
| 1 | **A second, smaller parked column admitted just under k.** k sweep (`runs/ft63_leg19_dwr_k10` / `_k30` / `ft63_leg19_dwr`): a00 at 329190 = 525.40 / 521.42 / 521.42, window peak 858 / 816 / 831. **FLAT** — tightening k tenfold changes nothing, so the residual does not come through the admission path. |
| 2 | **A deweight RELEASE re-coupling (the SPEC's original mechanism, second-order).** The armed leg's `network_epoch_status.csv` still shows `n_dw_zero = 3`, `dw_min_w = 0.0` at 329160; with the post-advance harvest skew those releases do not reach an assembly until 329220, after the residual. |
| 3 | **Row starvation by INC-3b itself** (the defect below). Bounding it (`runs/ft63_leg19_dwr_nd`) leaves 329190 at **521.42, bit-for-bit unchanged**. |

The event streams are decisive on what it is NOT: OFF and ON carry IDENTICAL non-`IONO_DW` events across
329130–329220 (same `T1_if_slip`, `T0_gap`, `CB_DATUM`, `T0_mw_detect` rows) and differ ONLY in the
`SH_GATE` line. Same class of event, one epoch later and ~11× smaller — not a different injector. Naming
it needs the `rhs_attrib` instrument, UNAVAILABLE this session: it armed (`ARMED at TOW 329160.0 for 2
epoch(s)`) but emitted no dumps, because a concurrent session was mid-rewrite of `NetworkRhsAttrib` (it now
references `counters.pc`, `knobs.rec_on`, `carry.rhs_trip_pending`). `runs/ft63_leg19_dwrat` is the empty
probe; re-run it once that instrument is whole.

| post-gate corrections |
|---|
| **⚠️ Defect found in INC-3b by its own counters, FIXED:** `networkShDevVtec` skips its harvest for a station at weight ≤ 0 and for a LOS with no local column (`NetworkShDevVtec.cpp:115`), so those LOS never produce the statistic the screen needs — and the first cut held them FOREVER (measured: `CPVG00CPV\|G30` quarantined 58 consecutive epochs, `CPVG00CPV\|G11` 17, `OAFA00ARG\|G19` 10). A guard must reject on EVIDENCE, not on its absence. Fixed with a bounded barren wait (`kAdmitNoDevMaxEp = 2`) then admit, counted apart as `admit_nodev` (4 on this leg; quarantines fell 1582 → 1199). Found only because the quarantine counter was emitted per LOS — invariant #4 earning its keep. |
| **⚠️ Calibration trap paid for here:** k was first set from the health trace's `slant_resid_max_m` (~30 m ⇒ z ~20), which OMITS `x_lin` and is one of the frame-broken readouts §7.2 names. The real deviations run 2–4 orders higher. Calibrate k from the harvested stash deviation, never from `network_vtec_sh_health.csv`. |

### 7.4 Consequence for §5's gates

Gate 3 as written is ungradeable: INC-1 fires on releases, and this event contains none. Grade INC-3b
instead — `dw_admit_reject` for (BSHM00ISR, the entering satellite) at 329160, no SH-gate engagement, a00
< 150 through 19:40 — and grade INC-1/INC-2 on a run that contains a genuine release (the DOY-129 day had
22 engage / 15 release events; 19:26 was not one of them). Their run-end block says so explicitly rather
than letting a zero-opportunity run read as a pass.
