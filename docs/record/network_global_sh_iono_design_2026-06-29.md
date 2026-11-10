# Co-estimated Global SH-VTEC Ionosphere for the PPP-RTK Network — Design

**Date:** 2026-06-29. **Goal:** make the producer self-sufficient — co-estimate our **own** global ionosphere
(single-layer spherical-harmonic VTEC) jointly with the satellite code biases (W6 OSB) so the system stops
importing an external GIM (`.INX`). Canonical CODE/Schaer recipe (joint SH-VTEC + per-constellation zero-mean
satellite DCB). Secondary win: a *system-shared* VTEC field (TEC is frequency- and constellation-independent)
gives BeiDou the between-satellite iono leverage the per-constellation external GIM under-served — the W6
BeiDou zero-emit.

**Provenance:** 6-agent codebase map → design synthesis → 3 adversarial reviews. Two (estimability + BeiDou)
returned `needs-work` with convergent, load-bearing corrections, folded into §6/§7. Verdict: **sound approach,
not a blocker — but three things must change before it can claim to FIX (not merely shrink) the BeiDou gap.**

**Downstream status (2026-08-15).** This is the field ARCHITECTURE and observability record; the parts §7
names stay unbuilt. The *dual-layer* SH backbone and the per-LOS deviation carry that were subsequently built
live in `docs/SPEC_iono2_mechB_v3.md` (mechanism A, mechanism B v2, the v3 de-mean, the v3k erase split).
**§6.2's rule — keep mean VTEC data-determined, do NOT clamp to an external mean, with its ±5 TECU
DCB-insensitivity test — is the ledger rule that SPEC's "Rejected" list depends on; do not weaken it there
without amending it here.**

---

## 1. Approach — Option A first (co-estimated field as a constraint, slant-iono states kept)

The hinge is one function: `appendGimIonoRows`
([ApplicationNetwork.cpp:701-759](src/core/app/ApplicationNetwork.cpp:701)). Today it injects the external
GIM's between-satellite structure: per GPS/Gal satellite with a usable local slant-iono column and a valid GIM
query it pushes a tight-prior row `H(k,col)=1; z = gim_->slantIonoDelay() - x(iono_idx)` with a
per-constellation **common-mode-free** `R = diag(var) + σ_c²·11ᵀ` (the all-ones block leaves the
per-(rx,const) iono datum level FREE, imposing only between-satellite STEC structure).

**Change:** replace the GIM target with `STEC_SH = M(el)·VTEC_SH(IPP_lat, sun-fixed_lon, t)` synthesized from
a new family of **shared** SH coefficient states. The one substantive new wrinkle: the coefficients are now
*estimated*, so the row gains a Jacobian into the shared SH columns:

```
row:  slant_local(rx,sat)  −  Σ_nm M(el)·Ȳ_nm(IPP) · a_nm   =  innovation
H:    +1 on the local slant-iono column (A_r, unchanged)
      −M(el)·Ȳ_nm(IPP) on each shared SH column (A_s — the ONE new write)
```

Unchanged: the common-mode-free `R`, the per-(const,signal) sum-to-zero DCB datum
([:1326-1335](src/core/app/ApplicationNetwork.cpp:1326)), the per-sat IF clock pin
([:1337-1371](src/core/app/ApplicationNetwork.cpp:1337)), the null-space gate, the D2 carry-forward. The W6
OSB separation that already works for GPS+Gal is reproduced *by construction* the moment `VTEC_SH` regresses
to the GIM — that is the Stage-1 acceptance test.

**Option B** (delete per-(rx,sat) slant states, drive STEC purely from coefficients) is **deferred**: it
removes the local sink for un-modelled small-scale TEC, couples every station's code residual directly into
the global field (blunder propagation), and discards the common-mode-free `R` that shields the DCB datum. Keep
the slant states; let the SH field be an informative prior on them.

---

## 2. New state family — shared SH coefficients (network border, not StateManager core)

The map's "allocate in StateManager core after the ISBs" is right for a *single-receiver* filter and **wrong
here**: the producer's shared parameters live in `NetworkEstimator` columns and reduce via the Schur
arrowhead. A field shared across all stations is structurally identical to a satellite clock (one column,
every station's rows touch it) ⇒ a new **`SharedParamKind::VtecSh`**
([NetworkEstimator.hpp:58-62](include/filter/NetworkEstimator.hpp:58)) with `registerVtecCoeff(n,m,trig)` /
`vtecCoeffColumn(n,m,trig)` mirroring `registerClock`/`registerCodeBias`, and a `ColumnDesc` variant carrying
`(n,m,trig)`.

- **Keying:** `StateKeySchema::makeVtecShKey(n,m,trig)` → `"SH|n|m|C"` / `"SH|n|m|S"` (+ parse helper),
  following the make/parse pattern at [StateKeySchema.hpp:24-38](include/filter/StateKeySchema.hpp:24).
- **Count:** triangular truncation to degree N has (N+1)² coefficients — **N=4 → 25** (Stage 1); N=8 → 81;
  N=12 → 169 (CODE uses 15×15). Start small: these are *dense* border columns (every station's every satellite
  row writes all (N+1)² partials), qualitatively worse for the arrowhead than the sparse clock/bias columns,
  and the per-epoch eigensolve gate already serial-bottlenecks at large n.
- **Temporal model:** random walk via the existing D2 `SharedPrior`
  ([NetworkEstimator.hpp:218-236](include/filter/NetworkEstimator.hpp:218)) — seed each epoch from the prior
  estimate with info inflated by `q·dt`. **Per-degree (Kaula-scaled) q, not a single scalar** (§6.5): in a
  sun-fixed frame a₀₀ is near-constant (slow seasonal/solar drift) while structure lives in higher
  coefficients.
- **No per-station VTEC state in Stage A** — the per-(rx,const) iono level stays free in the common-mode-free
  `R`'s all-ones block and the receiver DCB rides in it unchanged. (This is why the receiver-DCB↔VTEC-offset
  gauge G2 does not appear in Stage A; it is the Option-B coupling we defer.)

---

## 3. Math — reuse the geometry, build only the basis

| Piece | Status | Source |
|---|---|---|
| IPP geometry (el, mapping, lat/lon_ipp) | **reuse** — refactor into a free `ippGeometry()` | [GlobalIonosphereMap.cpp:114-161](src/core/atmosphere/GlobalIonosphereMap.cpp:114) |
| Single-layer mapping `M(el)`, shell ~450 km | **reuse** | same block (`sin_zp = Re/(Re+H)·cos el`) |
| ECEF↔ENU/geodetic, az/el | **reuse** | `Coordinates` |
| Earth-rotation / sun-fixed rotation rate | **reuse** | `kEarthRotDegPerSec`, GlobalIonosphereMap.cpp:91 |
| **mean-sun (sub-solar) longitude** | **build** — standalone `meanSunLongitude(GnssTime)` | extract from `Planetary` Sun-ECEF (atan2, <1° suffices) |
| **associated Legendre `P̄_nm(cosθ)` + normalization** | **build** — new `SphericalHarmonicIonosphere.hpp/.cpp` | confirmed absent (VMF3 terms are unrelated tropo Fourier) |
| **STEC partials d/d a_nm, d/d b_nm** | **build** | `M(el)·P̄_nm·{cos,sin}(m·λ_sf)` |

`VTEC(θ, λ_sf) = Σ_{n=0}^{N} Σ_{m=0}^{n} P̄_nm(cosθ)·(a_nm cos mλ_sf + b_nm sin mλ_sf)`, geographic latitude
for Stage 1 (geomagnetic before BeiDou — §7), sun-fixed longitude `λ_sf = λ_ipp − λ_mean_sun`. Validate
`P̄_nm` + normalization by reproducing a parsed CODE 15×15 IONEX grid to <0.1 TECU and finite-difference
checking the partials **before** any filter wiring (Stage 0).

---

## 4. Integration points (file:line)

- **Row builder:** rewrite `appendGimIonoRows`
  ([ApplicationNetwork.cpp:701-759](src/core/app/ApplicationNetwork.cpp:701)) per §1.
- **Register shared columns:** new `registerVtecCoeff` loop where `registerClock`/`registerCodeBias` are
  called (~[ApplicationNetwork.cpp:1007](src/core/app/ApplicationNetwork.cpp:1007)).
- **Datum rows:** §6 — fold via `est.addSharedConstraintRows`, the same call as the DCB datum
  ([:1382](src/core/app/ApplicationNetwork.cpp:1382)).
- **Reverse-map / trace:** extend `ColumnDesc`/`col_desc` to name SH columns (**hard requirement**, §6.4);
  add `network_vtec_sh.csv` beside the iono trace ([:541-559](src/core/app/ApplicationNetwork.cpp:541)).
- **Config:** `estimation.global_vtec_sh.*` in `loadEstimationSection_` and `network.estimate_iono_sh` in
  `loadNetworkSection_` ([GnssConfigLoad.cpp:618 / 2514](src/core/config/GnssConfigLoad.cpp:618)).
- **Products (Stage 5):** `vtec_sh_coeffs` on `NetworkSolutionEpoch` + `SsrCorrectionStore` push/get +
  consumer apply (closed-loop self-sufficiency proof).

The legacy GIM path stays compiled and selectable, so A/B is a config flip and the OFF path is byte-identical.

---

## 6. Estimability & datum — REVISED per review (the load-bearing section)

`assertNullSpaceRank` ([NetworkSbasis.cpp:90-98](src/core/filter/NetworkSbasis.cpp:90)) throws on **exact
inequality** `near_zero != declared_dim` and skips the epoch wholesale. Everything below is about keeping
`near_zero` deterministic.

### 6.1 CRITICAL — regularize **every** coefficient, not just a₀₀ (Tikhonov/Kaula prior)

Both reviewers independently flagged this as **the single most likely reason a literal implementation emits
nothing.** The original design pinned only a₀₀ and left a_nm (n≥1) "to the gate as a safety net" — but the
gate is **all-or-nothing per epoch**. A 305-station network is land-biased with large ocean/polar/desert gaps;
a sun-fixed SH field has global support, so coefficients whose basis energy concentrates over the gaps are
near-null — and because the sub-solar point moves, **which coefficients are near-null rotates
epoch-to-epoch**. Each un-regularized near-null coefficient adds a near-zero eigenvalue ⇒ `near_zero` exceeds
`declared_dim` ⇒ whole epoch withheld: the zero-emit re-created, now for *all* constellations.

**Fix (what CODE/IGS do):** a **degree-dependent (Kaula-like ~1/n²) a-priori variance on every a_nm, b_nm**,
applied through the D2 `SharedPrior`. Unobserved coefficients are pulled to their prior (≈ climatology);
well-observed ones stay data-dominated. Every potential SH gauge becomes a soft-pinned, Weyl-safe direction
(PSD info adds no near-zero eigenvalue), so **`declared_dimension` stays at #constellations (the clock datums)
regardless of coverage** — cleaner than the original "+1 for a₀₀".

### 6.2 a₀₀ ↔ satellite-DCB level — keep self-sufficiency, do NOT clamp to an external mean

Co-estimating a₀₀ removes the external GIM's implicit mean anchor, so a₀₀ trades against the satellite-DCB
absolute level. The original fix (pin a₀₀ to an external mean at weight 1e3) was flagged: a soft pin to an
external number **re-introduces an external-mean dependency through the back door** (defeating benefit #1) and
**biases every DCB across all constellations** if the prior is off by a few TECU — which a static config
scalar will be, diurnally.

**Resolution:** the per-(const,signal) **zero-mean DCB datum already pins each constellation's DCB level**
([:1326-1335](src/core/app/ApplicationNetwork.cpp:1326)) — that is the pure gauge. Leave the **mean VTEC
data-determined**: a₀₀ carries the weak absolute-STEC information the GIM normally supplements, and §6.1's
Kaula prior on a₀₀ is set **loose** (wide σ — climatology, not a tight IONEX value): enough to keep a₀₀'s
eigenvalue above the gate cutoff, not enough to bias the DCBs. **Verify DCB absolute level is insensitive to a
±5 TECU perturbation of the a₀₀ prior.**

### 6.3 Design-time rank check before wiring (small-synthetic SVD)

The IF-bias↔clock lump and the per-constellation DCB-vs-iono trade are *asserted* "by construction", but the
construction changed (iono is now estimated). **Before wiring**, do the rank algebra on a 2–3 sat / 2–3
station / 2-signal synthetic: write the full `{sat clock, 2 code biases/sat, per-(rx,sat) iono, a₀₀..a_nm}`
Jacobian + all gauge rows {IF pin, sum-to-zero, Kaula priors, common-mode-free R} and confirm by SVD that the
null dimension is exactly `#constellations`. Specifically: (a) the *ionospheric* combination of the code
biases (the part that trades with iono → a₀₀) must be pinned independently of the IF combination — if a
per-constellation DCB-iono combo trades freely you need more than one mean pin; (b) with ≥2 stations each
station's per-(rx,const) iono level must stay individually free (rx-DCB rides it), distinct from the single
global a₀₀ direction.

### 6.4 col_desc must name SH columns (hard requirement, not optional trace)

The one-shot spectrum diagnostic ([:1439-1479](src/core/app/ApplicationNetwork.cpp:1439)) names the columns
spanning each near-zero direction. Without `col_desc` extended to label `SH|n|m|C/S` it prints anonymous
`colNNN` for exactly the new gauge directions, and a missing SH datum becomes indistinguishable from a
clock-datum fault. Prerequisite, not optional.

---

## 7. BeiDou — REVISED, sharper, more honest

The shared field **partially fixes** the BeiDou W6 zero-emit and **changes the form** of the residual gap.

- **BeiDou MEO (sky-sweeping IPPs): a real fix.** A frequency/constellation-independent field determined by
  dense GPS+Gal IPPs constrains the same a_nm that predict BeiDou STEC; MEO DCBs become observable-from-self
  via between-satellite contrast — withhold → emit with genuine observability.
- **BeiDou GEO + most IGSO (quasi-static / analemma IPPs): NOT observable-from-self.** A near-stationary IPP
  samples one field location persistently; its SH partials are near-constant in time and a free-walking local
  slant-iono state absorbs anything. The field makes the GEO DCB **predictable-from-the-field (a model
  extrapolation), not observable** — recovered by *trusting* the GPS/Gal field at one fixed point, so any
  field bias there (truncation, equatorial-anomaly mismodelling) maps **1:1** into the GEO DCB. The coverage
  gap converts from "null → withhold" (gate catches) to "finite-but-unconstrained → silent biased emit"
  (**gate cannot catch**) — more dangerous than the original design admitted.

**Required mitigations before claiming a fix:**

1. **Per-satellite emit gate** keyed on IPP motion (lat/lon spread over a sliding window) **and** local
   GPS/Gal IPP data density within a correlation radius — withhold *individual* geometrically-starved sats,
   never the whole epoch. Emit field-extrapolated DCBs with an **inflated σ** derived from the a_nm formal
   covariance projected through the SH partials.
2. **Exclude static-IPP sats from the a_nm Jacobian** (they READ the field: +1 on local iono, `M·VTEC_SH` as
   a fixed prior). Otherwise a GEO with a biased DCB pushes a_nm at its fixed IPP, the RW carries it forward,
   and a self-reinforcing GEO-DCB/field feedback loop follows — the network-clock-divergence pattern
   ([[project_beidou_network_clock_divergence]]) re-expressed in the iono field, which an 11-epoch smoke and
   even a 1 h run may not surface (Galileo looked clean at 11 ep, diverged at 38 min).
3. **Geomagnetic latitude before enabling BeiDou** (not deferred) — the equatorial anomaly, where many IGSO
   IPPs concentrate, corrupts exactly the DCBs we are trying to recover under geographic latitude.
4. **Differential single-layer mapping:** BeiDou GEO/IGSO view the shell at systematically different
   elevations than co-located MEO; the `M(el)` error is constellation-geometry-correlated and re-confounds
   with the BeiDou DCB at low elevation. Stage-3 acceptance must **stratify BeiDou DCB residual by elevation**
   and flag any elevation-correlated bias (→ MSLM/second layer, not more SH degrees).
5. **Validate against an independent product:** compare emitted BeiDou GEO/IGSO DCBs to CAS/DLR MGEX DCBs at
   the few-tenths-ns level. Emit-rate up with biased values is a false success.

---

## 8. GLONASS — resolve the contradiction (keep it OUT of the SH rows in Stage A)

The synthesis prose claimed GLONASS "injects STEC into the shared field", but the reused row builder skips
`'R'` ([:714](src/core/app/ApplicationNetwork.cpp:714)) and GLONASS carries no satellite OSB (FDMA → per-slot
receiver IFB). Letting GLONASS drive a_nm would leak per-slot IFB error into the global field that then
predicts BeiDou GEO DCBs — a cross-constellation blunder path. **Decision: keep the `'R'` skip in Stage A**;
the "GLONASS injects STEC" premise is *false and intentionally so*. Revisit only behind a separate toggle with
heavy down-weighting + a spectrum check.

---

## 9. Config & products

- `estimation.global_vtec_sh = { enable:false, max_degree:4, layer_height_km:450, kaula_scale, q_per_degree,
  mean_vtec_prior_tecu, mean_vtec_prior_sigma (LOOSE), sun_fixed:true, trace:false }` — new POD beside
  `IonosphereWeightingConfig`, getter `getGlobalVtecShConfig()`.
- `network.estimate_iono_sh` gates the producer feature (forced-enable chain like
  `network.estimate_sat_code_bias`, [:2630-2659](src/core/config/GnssConfigLoad.cpp:2630)); add an **XOR
  validation gate** with external-GIM-loaded so the two iono sources are never double-applied.
- Products (Stage 5): `vtec_sh_coeffs` on `NetworkSolutionEpoch` → `SsrCorrectionStore` push/get → consumer
  apply; wire target IGS-SSR VTEC (RTCM 4076) last.

---

## 10. Staged plan (smoke→1h cadence; each stage independently buildable + testable)

0. **SH kernel, no filter wiring.** `SphericalHarmonicIonosphere.{hpp,cpp}` (P̄_nm + normalization, VTEC
   synthesis, partials, `meanSunLongitude`); refactor `ippGeometry()` out of GlobalIonosphereMap. **Test:**
   reproduce a CODE IONEX grid <0.1 TECU; finite-difference partials. No estimator touch.
1. **Smallest proving slice (GPS+Gal).** Register degree-4 shared columns; rewrite `appendGimIonoRows` to
   target `STEC_SH` with A_s partials; **Kaula prior on every coefficient (§6.1)**; loose a₀₀ prior (§6.2); D2
   RW; `col_desc` + `network_vtec_sh.csv`. **Do the §6.3 synthetic-SVD check first.** **Acceptance:** on
   `_net305_gpsgal_ifb_1h`, co-estimated a_nm reproduce a CODE IONEX low-degree fit AND emitted GPS+Gal OSB
   match the external-GIM W6 OSB to within noise; OFF path byte-identical; spectrum diagnostic shows
   `near_zero == #constellations` with no SH column spanning a near-zero direction.
2. **Smoke→1h, 3-system field** (GPS+Gal driving, BeiDou borrowing read-only): `_net305_3sys_smoke` →
   `_net305_3sys_1h`; watch the gate (no new under-specified withholds), a₀₀ stability, OFF byte-identity.
3. **BeiDou enabled.** Per-satellite emit gate + static-IPP Jacobian exclusion + geomagnetic latitude
   (§7.1–7.3). `_net305_4sys_smoke` → `_4sys_3h`. **Acceptance:** BeiDou MEO/IGSO emit rate up vs the W6
   zero-emit baseline; BeiDou DCBs vs MGEX at few-tenths-ns; no elevation-correlated DCB bias; a_nm + per-GEO
   DCB stable across ALL epochs (catch the slow feedback).
4. **Raise fidelity.** N=4→8→12, tune per-degree q; validate equatorial-anomaly representation. Mind the
   dense-border serial cost (memories `project_timing305_allon_baseline` / `project_net305_attribution_rerun`;
   the previously-cited `project_network_perepoch_serial_bottleneck` does not exist).
5. **Emit + closed loop.** `vtec_sh_coeffs` product + held-out-station consumer apply — own-DCB + own-iono,
   no external IONEX. RTCM-4076 wire last.
6. **Option B evaluation** (only if warranted): pilot direct-coefficient STEC on a subnet; keep local states
   unless B clearly wins.

---

## 11. Top risks (revised)

1. **Un-regularized SH coefficients → whole-epoch withhold** (dominant; §6.1). Kaula prior on every
   coefficient; verify `declared_dimension` unchanged via spectrum dump.
2. **Silent biased GEO/IGSO DCBs** from field extrapolation into coverage holes (§7). Per-sat data-density
   emit gate + inflated σ + MGEX validation; the gate cannot catch this, so it must be explicit.
3. **Static-IPP self-contamination feedback** once BeiDou writes the field (§7.2). Exclude static-IPP sats
   from the a_nm Jacobian.
4. **a₀₀ pin re-introduces external dependency / biases DCBs** (§6.2). Data-determined mean + loose
   climatology prior; ±5 TECU insensitivity test.
5. **SH normalization/Legendre bug** silently scales coefficients (§3). Standalone Stage-0 IONEX-reproduction
   + finite-difference test before wiring.
6. **Dense SH border worsens the per-epoch serial bottleneck.** Keep N=4 until trusted; profile
   reduce()/eigensolve at n=305 before raising N.
7. **Sun-fixed frame / mean-sun error** rotates the field and the RW carry-forward amplifies a slowly-wrong
   frame. Epoch-boundary self-consistency test; cross-check mean-sun longitude two ways.

---

## 12. Bottom line

The architecture is sound, every code-integration claim maps to a real seam, and the approach internalizes the
iono by reusing the entire W6 datum machinery with a single function change + a new shared-column family. The
reviews do not change the architecture — they make it *correct*: **regularize every coefficient (not just
a₀₀), keep mean-VTEC data-determined, and add a per-satellite observability gate so BeiDou GEO/IGSO ship with
honest σ instead of silent bias.** With those, the honest claim is "self-sufficient own iono + own DCB; real
BeiDou MEO fix; BeiDou GEO/IGSO recovered as field-extrapolated products with quantified uncertainty; GEO
coverage-hole residual is a geometric limit, not a modelling artifact."
