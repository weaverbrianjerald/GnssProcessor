# SPEC — the carrier-phase wind-up SIGN defect (and the wrapped-mode wrap)

**Status: DEFECT PROVEN, UNFIXED, NOT YET FIELD-PRICED.  Analytic derivation plus
three independent numerical confirmations.  `GnssCorrections::computePhaseWindUp`
returns exactly `−φ_windup`; the correction is added to the model with the
conventional sign, so the residual left in every carrier-phase observation is
`+2·φ_wu` — band-common in cycles, per-(receiver, satellite), DD-surviving and
MW-invisible.  Found 2026-08-21 while chasing the `SPEC_o2_carry_invariant_accumulator.md`
§7 common-cycle lead; it is NOT that lead's carrier (§5), and §7 itself does not
survive (§6).**

⚠ **Read §3 before touching the code.  Fixing one of the two lines is worse than
the defect.**

---

## §1 The defect

`src/core/corrections/GnssCorrections.cpp:1944-1945`:

```cpp
Eigen::Vector3d Ds = x_s - k * (k.dot(x_s)) + k.cross(y_s);   // satellite
Eigen::Vector3d Dr = x_r - k * (k.dot(x_r)) + k.cross(y_r);   // receiver
```

with `k = (rx_pos − sat_pos).normalized()` (satellite → receiver, `:1873`),
`ez = −sat_pos.normalized()` (geocentric nadir, `:1882`), the satellite triad
`ey = ez × e_sun`, `ex = ey × ez` — right-handed `(ex, ey, ez)` (`:1907,1913`) —
and the receiver pair `x_r = NORTH`, `y_r = EAST` (`:1940-1941`), whose implied
third axis is `north × east = DOWN`.

**Both triads are right-handed with their third axis pointing along +k**, and both
quadrature terms carry `+k×ŷ`.  For a right-handed triad `(x, y, z)` with `z ≈ k`,
`k×y ≈ −x` — so the `+` sign *cancels* the leading term instead of reinforcing it.

**Analytic result (exact, no small-angle step).**  In the plane ⊥ k with a
right-handed basis (p̂, q̂, k), for any antenna triad right-handed with
`ε = ∠(z_ant, k)` and `α` the rotation of x about `z_ant`:

```
x_⊥k − k×y = (1 + cos ε) · ( cos α  p̂ + sin α  q̂)      <- the correct effective dipole
x_⊥k + k×y = (1 − cos ε) · (−cos α  p̂ + sin α  q̂)      <- what the code builds
```

Two consequences, either one fatal:

1. **Magnitude `(1 − cos ε)`: the effective dipole VANISHES on boresight.**  For the
   satellite `ε` is the nadir angle ⇒ `|Ds| ≈ 0.02`; for the receiver `ε` is the
   zenith angle ⇒ `|Dr| → 0` for a satellite overhead.  **An effective dipole that
   vanishes where the antenna gain is maximal is not an effective dipole.**  This is
   decisive without reference to any external implementation.
2. **Phase `π − α`: the mirror of the correct phase.**  `p̂_s` and `p̂_r` are both the
   ⊥k projection of the LOS-vertical plane, so they are collinear and the basis
   offsets cancel — leaving `φ_code ≡ −φ_std` exactly.

Origin: the comment at `:1872` — *"Wu et al. (1993): k = sat->rx, D = x − k(k.x) +
k x y"* — one form, applied to **both** dipoles, with `y_r = +east`.

### Three independent confirmations

| route | result |
|---|---|
| analytic, 20 000 random geometries at el ≥ 10° | med \|Ds\| **0.02052** vs `1−cos(nadir)` = 0.02052; med \|Dr\| **0.45833** vs `1−cos(zenith)` = 0.45835; \|φ_code + φ_std\| med **5.6e-4 cyc**, max 6.4e-2 |
| independent replication of the shipped function, realistic geometry + yaw sweep | \|Ds\| **1.7e-4** and \|Dr\| **3.1e-3** for `(+,+)` against **1.99983 / 1.99695** for `(−,−)`; windup `+0.029710` vs `−0.029710`; **one satellite yaw turn ⇒ +1.0000 cyc (code) vs −1.0000 cyc (standard)** |
| replication against the archived C++ trace | reproduces `windup_trace.csv` to **3 µcyc median / 1.05e-4 cyc max** over 10 075 matched (satellite, epoch) rows, and `φ_code + φ_wu` is constant per arc to ~1 mcyc (sd 0.0007, within-arc range median 0.00054) |

**No compensating sign downstream** (verified): `Antenna.cpp:981 correction +=
wind_up_corr` → `PppModel.cpp:1470 + antenna_corr` into the modelled range.  The
network producer is exposed too — each station's `PppModel` carries its own
`Antenna`/`GnssCorrections` by value, and `ModelGeometry.cpp:742,748` /
`PppModel.cpp:1456` are the live call sites.

---

## §2 Why nothing caught it

**Every simulator validation in this project is structurally blind to it.**
`src/sim/EphemerisProvider.cpp:414` injects wind-up by calling **the same**
`computePhaseWindUp` (with `accumulate=true` hard-coded at `:413`), so the sim
injects `−φ` and the producer models `−φ` and the defect **cancels identically** on
every truth-graded arm.  A sim A/B can never see this; only real data can.

The doc `docs/report/observation_physics.tex:349-354` prints the same `(+,+)` pair
and calls it "the standard Wu construction" — documentation transcribed from the
code, not independent corroboration.

---

## §3 ⚠ THE FIX IS BOTH LINES, TOGETHER — a half-fix is worse than the defect

Measured over 20 000 geometries:

| arm | \|φ − φ_std\| med / p90 | \|φ + φ_std\| med / p90 |
|---|---|---|
| flip ONLY `:1944` (satellite) | 0.2512 / 0.4493 | 0.2522 / 0.4521 |
| flip ONLY `:1945` (receiver) | 0.2524 / 0.4522 | 0.2510 / 0.4493 |

0.25 cyc is the median of \|U(−0.5, 0.5)\| — **a half-fix matches neither the model
nor its negative.**  It converts a clean, largely ambiguity-absorbable sign
inversion into a uniformly distributed band-common phase error spanning a full
cycle.  Today's state is recoverable; a half-fix is not.

Two further corrections worth carrying:

- `ez` at `:1882` is the **geocentric nadir**, not "toward the receiver".  If it
  pointed exactly at the receiver, `ε_s ≡ 0` and `|Ds|` would trip the `1e-10` guard
  at `:1947`, returning 0.0 every epoch.  The nadir angle is precisely what keeps
  the degenerate construction numerically alive at ~2 % of the correct amplitude.
- The standard pairing stated **in the code's own variables** (`y_r = +east`) is
  `(−,−)`.  RTKLIB's familiar `(−,+)` is stated with `eyr = −east` — numerically
  identical, but quoting it in RTKLIB's convention is exactly how a half-fix
  happens.

---

## §4 Impact — where it lands, and where it does not

MEASURED on the FT63 roster (63 stations, GPS, DOY-129, el ≥ 15°), per-arc/chord
**range** of `2φ_wu` — the part no constant ambiguity absorbs:

| domain | med (cyc) | p90 | max | at L1 |
|---|---|---|---|---|
| ZD arc (station, satellite) | **0.414** | 1.037 | 1.599 | 79 mm med / 197 mm p90 |
| SD between-station | 0.065 | 0.195 | 0.430 | — |
| DD chord (≥30 min common, n = 3866) | 0.026 | 0.136 | 0.429 | 4.9 mm / 25.9 mm |

The satellite-yaw term is station-common, so the error **largely cancels in SD/DD**
— datum-invariant grading (the Psychas single-realization mandate this project
grades by) is partly protected, which is a second reason it survived.  **The wire is
not protected**: this is a ~0.4 cyc/arc ZD-domain systematic sitting directly on the
states the producer publishes (satellite clocks, satellite phase biases) and on the
phase-leveling zero-q carry.

---

## §5 It is NOT the §7 common-cycle carrier

Its DD-domain footprint is ≤ 1.9 % of the σ_eff variance at a 5000 km baseline —
against `SPEC_o2_carry_invariant_accumulator.md` §3's measured σ_eff floor of
0.35–0.45 cyc, and against the ~5 cyc per-band DD error on the sim.  It is a
**wire / product-contract defect, not an estimation-residual defect.**

And it is **structurally absent from the arm where the §13.6 statistic lives**:
that arm resolves `accumulate_phase_windup = true` and the sim injects through the
same function, so both the sign defect and the wrap cancel there.

---

## §6 The §7 lead does not survive — "equal in cycles" is not a separate direction

The correction that matters most for the record.  In per-band **cycle** coordinates:

```
cc   (equal in cycles) = (1, 1        , 1        )
geo  (equal in metres) = (1, 0.779221 , 0.746753 )     [ λ₁/λ_b ]
iono                   = (1, 1.283333 , 1.339130 )     [ α_b λ₁/λ_b ]
```

and `cc = 0.5691·geo + 0.4313·iono` with residual **0.230 % of ‖cc‖ (0.132°)**.
The phase observation equation confines the DD ambiguity error to exactly
`span{geo, iono}` — any component outside it appears directly in the phase residual
at mm level — and `cc` lies inside that plane.  Consequences:

- **On GPS L1/L2 — every FIELD lineage — the separation is formally undetermined**
  (2 observations, 2 basis vectors span ℝ²).  Widening the band set does not rescue
  it: 0.230 % at GPS L1/L2/L5, 0.305 % across seven GPS+Galileo bands, 0.297 % across
  the widest GNSS set.
- Both target statistics are **one coordinate of one 2-parameter mixture**: "equal in
  cycles" appears whenever the geo : iono split is near 0.57 : 0.43.  Measured splits
  — field arms **59 : 41**, §13.6's 1 h fixture **56 : 44** (which reproduces "equal
  in cycles" exactly), and the settled 480-epoch arm of the same fixture **33 : 67**,
  at which point the per-band medians read 1 : 1.124 : 1.153 **and the signature is
  gone**.  Independently reproduced here by grading absolute integers against truth
  on `runs/simnet60_tri_slant_480_ddint`: 1 : 1.104 : 1.129 full window,
  1 : 1.123 : 1.148 settled.
- The data **prefers `geo + iono` over `cc + iono`** — better on 63.7 % of rows
  (binomial z = 64.5), and the empirically optimal second direction sits
  **0.03–0.09° from `geo`** and ~1° from `cc`.
- The off-plane residual is flat at **3–6 mm(L1) across a 400× range of |e₁|**, which
  is the fixture's own measurement-noise floor (phase σ 3 mm + multipath 2 mm; a
  four-leg DD ≈ 7 mm) — so there is no third direction to find.

⇒ **`SPEC_o2_carry_invariant_accumulator.md` §7's claim that "the ionospheric
direction has never been observed" must be withdrawn.**  The 1.262–1.265 metre
ratio is not evidence of a non-ionospheric carrier; it is the mixture coordinate,
and the ionospheric component is present throughout — it is the *dominant* component
on the settled arm (67 %).  §7 should be split into two independent leads, because
the wind-up mechanisms exist only where the FIELD statistic lives and are absent
where the SIM statistic lives.

---

## §7 The second, separate defect — fractional-mode wrapping on every field lineage

`estimation.accumulate_phase_windup` resolves **absent ⇒ false** on
`_ft63_24h_conv`, `_ft63_pdfix_v3`, `_agu63`, `_ext63_gps_dual_12h_s2h_on`,
`_golden305_gps`, `_golden63_gps_1h_laneoff` (loader default,
`GnssConfigLoad.cpp:1135-1136`), and **true** on the sim lineages `_camp60_f4`,
`_simnet60_tri_slant_*`.  In fractional mode the applied wind-up is the raw
`atan2` angle, confined to (−0.5, +0.5] cycles, while the physical wind-up is
continuous — so every half-cycle crossing steps the applied correction by exactly
one cycle.

MEASURED on `windup_trace.csv` (24.0 h, GCGO, 115 satellites, 100 983 rows, `mode`
= `fractional` on every row): **43 consecutive-epoch jumps of one cycle**
(0.9901–0.9999, median 0.9990), at elevations 10.1 / 36.1 / 88.2° (min/med/max) —
**mid-pass, in good data**.  Scaled to the roster that is ~**753/day at 63
stations**.  Each is band-common, so `ΔWL = 0` exactly and **MW is structurally
blind**; `|ΔGF| = 0.054 m`, `|ΔIF| = 0.107 m`.

Scoped honestly: 753/day is **0.1 % of the measured 525 009 chord refloats/day** and
~16 % of the 4 646 raw slip events/day.  It is a genuine standing-invariant #1/#7
instance — a one-cycle discontinuity entering a time-linked ambiguity — but it is
not a σ_eff mover.

⚠ Provenance note: the trace file found under `runs/sdewl_byteid_off/logs/` is a
**stale artifact pooled from `build/x64-Release/logs`** (byte-identical to the copy
in `runs/sdewl_on30_RAWFRAME_INVALID/`, four constellations over 24 h, while that run
was GPS+Galileo for 900 s).  It matches `config/ppp_rtk_internal.json`.
`enableWindUpTracing` has exactly one caller — `Application.cpp:1630-1631`, inside
`Application::run()` **after** the mode branches — so **the network, baseline and
real-time paths never write `windup_trace.csv`.**

---

## §8 What is owed before any fix lands

1. **The decisive empirical test, which the sim cannot provide** (§2): a real-data
   A/B on a station with known coordinates — wind-up ON (as shipped) vs OFF vs
   sign-corrected — graded against SINEX/SiteTruth.  If the sign is inverted, ON must
   be *worse than OFF*, and the corrected arm better than both.  That is the arm that
   converts this from proven-in-algebra to priced-in-the-field.
2. **Invariant #6**: the fix is solve-affecting on every lineage and every path.  It
   changes emitted satellite clocks and phase biases, so it is a product-frame change
   — check whether it needs a legend/contract field, since a producer-fixed /
   consumer-unfixed pairing is an undetectable frame mismatch (the recorded precedent
   is the phase-shift convention, which is likewise absent from the v2 legend).
3. **Re-grading**: every archived validation that used the sim is blind to this (§2);
   every field validation carries it.  Nothing needs retracting on relative/DD
   grading (§4), but wire-quality claims (satellite clock and phase-bias grades) were
   all measured with it present.
4. `accumulate_phase_windup` should be considered at the same time (§7), and whether
   the loader default is right — but note the two are independent, and arming
   accumulation does **not** fix the sign.
