# SPEC — the carrier-phase wind-up SIGN defect (and the wrapped-mode wrap)

**Status: FIXED AND FIELD-PRICED, 2026-08-21.  `GnssCorrections::computePhaseWindUp`
returned `−φ_windup`; the correction is added to the model with the conventional
sign, so the residual left in every carrier-phase observation was `+2·φ_wu` —
band-common in cycles, per-(receiver, satellite), DD-surviving and MW-invisible.
Found while chasing the `SPEC_o2_carry_invariant_accumulator.md` §7 common-cycle
lead; it is NOT that lead's carrier (§5), and §7 itself does not survive (§6).**

**The fix is FOUR sites, not two** — both quadrature signs, plus a reframe of the
velocity-fallback satellite branch (§3a), plus the comment that caused it.  Gated
by `tests/test_phase_windup_sign.cpp` (10 cases, `PhaseWindUpSignTest`), which
fails 9/10 on the shipped code **and on both half-fixes**.  Priced on real data by
the three-arm ladder in §9: slope ≈ 2 at all three stations, and the arc-demeaned
post-fit carrier-phase residual RMS improves in **7 of 7** configurations.

⚠ **Read §3 and §3a before touching this code again.  Fixing one of the two
quadrature lines is worse than fixing neither, and a blanket `sed` over both
BREAKS the velocity-fallback branch, which was the one place the old `+` was
right.**

⚠ **The yaw-turn response is NOT a sufficient acceptance gate — it PASSES for a
half-fix.  See §3b.**

**Amendments to the original card, all measured:** §1's "exactly `−φ_std`" is not
exact (§1a); `ModelGeometry.cpp:742,748` are not live wind-up sites (§1b); §3's
0.2512/0.2522 are geometry-dependent, not constants (§3c); §4's 79 mm is the
FRACTIONAL sawtooth range, ~3.5× the physical part, and is the wrong unit for the
estimation channel (§4a); §8 item 1's "ON must be worse than OFF" is **not
guaranteed** and was replaced by the ladder (§9).

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
`Antenna`/`GnssCorrections` by value.

### §1a ⚠ "exactly `−φ_std`" is NOT exact — the prose overstated the table

The mirror is exact only for a SPHERICAL receiver vertical.  The satellite triad's
mirror axis is the **geocentric** nadir while the receiver pair's implied third
axis is the **geodetic** down, and the two differ by the vertical deflection (up
to 0.19°), amplified by `1/sin ε_r` in the plane ⊥ k.  Three independent
replications agree:

| population | med \|φ_code + φ_std\| | p90 | max |
|---|---|---|---|
| 20 000–30 000 random geometries, el ≥ 10° | **5.4–5.6e-4 cyc** | 1.3e-3 | 3.4e-2 … 8.6e-2 |
| 1 506 083 real FT63 rows (63 sta × 32 GPS sats × 2880 ep, el ≥ 15°) | 6.4e-4 | — | 0.30 % of rows > 1e-2 |

It is **elevation-structured**: el 10–30° med 4.4e-4, 60–80° med 1.1e-3, 80–90°
med 3.5e-3 — because the shipped `|Dr| = 1 − cos(zenith) → 0` there (measured min
4.3e-8, and the `1e-10` guard at `:1947` **never fires**, so those rows silently
return a value that is neither `+φ` nor `−φ`).  Rows where the deflection is under
0.001° (equator, poles) collapse to med 1.8e-6 cyc — the mirror IS exact there.

The card's own §1 confirmation table already said 5.6e-4 med / 6.4e-2 max, so the
TABLE was right and only the word "exactly" was wrong.  Practical consequence:
**never write an assertion of the form `shipped == −reference`** — assert against
the reference.  And the defect was never a clean, wholly ambiguity-absorbable
inversion; it carried a station-latitude- and elevation-dependent residual on top.

### §1b Correction — `ModelGeometry.cpp:742,748` are NOT live wind-up sites

They call `Antenna::getAntennaCorrection` for the emission-time seed refinement,
but `extractPseudorange` (`ModelGeometry.cpp:237-470`) emits only CODE observables
into `seed_sig1/sig2`, so `is_phase` is false and the wind-up block at
`Antenna.cpp:977` is skipped entirely.  The live call chain is
`Antenna.cpp:978` ← `PppModel.cpp:1456` (base) / `:1528` (IF pair) / `:1765`
(uncombined — the PPP-RTK user AND every network-producer station).

### §1c Two further dead ends found while auditing the consumers

- `SsrPhaseBias.yaw_angle_deg` / `yaw_rate_deg_per_s` (`RtcmSsrDecoder.cpp:365-366`,
  documented at `RtcmTypes.hpp:244` as "needed to compensate for phase wind-up
  consistently between the network and the rover") is **decoded and never read**.
  The one field in the SSR wire format that could have absorbed a wind-up
  convention difference has no consumer anywhere in `src/` or `include/`.
- The closed-loop consumer (`NetworkSetupResult.hpp:122`, a `RealTimeEngine` with
  its own `PppModel`) applied the *same* inverted model, so closed-loop grading
  absorbed the satellite-common part self-consistently and was **partially blind**
  — only the receiver-side `α_r` term survived.

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

⚠ One nuance that looks like it weakens the blindness claim and does not.
`EphemerisProvider.cpp:126` sets `mc.enable_phase_windup = false` — but that
mutates the **simulator's own** `GnssConfig config_` (held BY VALUE,
`include/sim/EphemerisProvider.hpp:408`), never the estimator's.  Its only job is
to stop the sim's internal `antennaMetres()` adding a second, FRACTIONAL wind-up
on top of the dedicated ACCUMULATED `phaseWindupMetres()` path — a double-inject
guard, documented as such at `:113-118`.  The estimator is a separate process
reading `config/test_suite/*.json`, where `enable_phase_windup` defaults **true**.

**And the unit suite gave zero protection.**  Before this card, `computePhaseWindUp`
and `Antenna::getAntennaCorrection` had **zero occurrences under `tests/`** (checked
with two independent tools per the project's absence rule).  The only two files
mentioning wind-up were a JSON-loader boolean
(`tests/sim/test_sim_selfconsistency.cpp:816`) and a legend STRING
(`tests/test_eq34_consumer_integer.cpp:228`).  A proven physical-model sign
inversion on a default-ON path had no unit guard at all — that is what
`tests/test_phase_windup_sign.cpp` now closes.

---

## §3 ⚠ THE FIX IS BOTH LINES, TOGETHER — a half-fix is worse than the defect

(And it is FOUR sites in total once §3a is included — see §8.1.)

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
  happens.  Verified by building RTKLIB's literal form in its own variables: it
  matches the `(−,−)` arm to **max 0.0e+00 / 1.1e-16 cyc** and differs from the
  `(−,+)` half-fix by med 0.2499.

Independent corroboration of `(−,−)` from four vendored reference implementations,
each stated in ITS convention and then translated:

| implementation | receiver `y` | pairing as written | translates to |
|---|---|---|---|
| RTKLIB `rtkcmn.c windupcorr` | `eyr = −east` | `(−, +)` | `(−,−)` at `+east` |
| `reference_software/gnss-qc/matlab/modelling/wind_up.m:29,41-42` | `yr = west` (labelled) | `(−, +)` | `(−,−)` |
| `reference_software/point/.../GnssReceiver.cpp:26925,26944` | `ry = −east` | `(−, +)` | `(−,−)` |
| `reference_software/cssrlib/src/cssrlib/ppp.py:296-302` | `(x,y) = (east, north)` ⇒ third axis **UP ≈ −k** | `(−, +)` | consistent; differs from ours by a rigid +0.25 cyc from choosing `x_r = east` |

`reference_software/mpages/.../Station.cpp:485-486` is the odd one out and must not
be transcribed: its `kvec` is **receiver → satellite**, i.e. `−k`.

The applied sign is also confirmed independently: POINT `GnssReceiver.cpp:700-701`
does `CP1 = (L1 − phaseWindUpL1)·λ` — observation minus `φ_wu`, i.e. the model
carries `+φ_wu`, exactly as `Antenna.cpp:981` → `PppModel.cpp:1470` does here.

### §3a ⚠ THE THIRD SITE — the velocity-fallback branch, where the old `+` was RIGHT

**Absent from the original card, and it inverts the "just flip both lines"
instruction.**  `computePhaseWindUp` builds `x_s`/`y_s` in THREE places, and they
did not agree on handedness:

| branch | lines | `x_s × y_s` | measured `n·k` | sign the `k×y` term needs |
|---|---|---|---|---|
| BeiDou orbit-normal | `:1892-1902` | `+ez` | +0.9888 … +1.0000 (med +0.992) | **−** |
| standard yaw-steering | `:1904-1917` | `+ez` | +0.9716 … +1.0000 (med +0.979) | **−** |
| velocity fallback | `:1918-1933` | **`−ez`** | −1.0000 … −0.9716 (med **−0.979**) | **+** |

The fallback framed about `z_s = -ez` (radially **outward**) and then set
`x_s = y_s.cross(z_s)`, which is the negation of the other two branches' `x_s`.
Consequences, both measured over 20 000 MEO + 5 000 GEO geometries:

1. A blanket `−` at `:1944` would have **degenerated** that branch — `|Ds|` 1.9795 → 0.0206.
2. **The shipped code was ALREADY in the half-fix garbage state there**, because
   the satellite `+` was right while the receiver `+` was wrong: med
   `|φ − φ_correct|` **0.2537** *and* med `|φ + φ_correct|` **0.2518**.  So §1's
   blanket "returns exactly `−φ_windup`" and §4's "largely cancels in SD/DD" are
   **false on any epoch where the Sun position is unavailable**.

The fix reframes the branch about `+ez` so all three share `x_s × y_s = ez`, after
which one unconditional `−` is valid.  **The reframe is a numerical no-op on that
branch**: it negates `y_s` and leaves `x_s` bit-identical, so
`Ds_new(−) ≡ Ds_old(+)` to `0.0e+00`.  *That identity is the review gate* — it is
asserted directly in `PhaseWindUpSignTest.VelocityFallbackBranchSharesTheNadirHandedness`.

Reachability: it is the `else` of `sun_pos.norm() > 1e6`, and `Antenna.cpp:575-577`
warns "invalid Sun position … attempt to continue with velocity-based fallback".
`ModelGeometry.cpp:740` calls `ensureSunMoonCached` immediately before the
correction, so it is rare in practice — a live, warn-logged degraded path, not
dead code.  Latent wart left in place and recorded: its orbit-velocity recovery at
`:1922-1927` iterates `orbits_`, which `GnssCorrections.hpp:514` documents as
"currently unused/empty", so a zero `sat_vel` falls through to a hard-coded
`(0,1,0)` used as a satellite quadrature axis.  Sign-safe under the reframe (the
re-derivation `y_s = ez × x_s` now guarantees handedness there), physically
meaningless; wiring it is a separate item.

`SatAttitude::compute` — which `SatAttitude.hpp:29-33` explicitly documents as a
duplicate of this frame and warns "keep the two sign conventions in sync or they
will silently fork" — has **no** such branch: it keeps `i_s = j_s.cross(k_s)` with
`k_s = -sat_pos.normalized()` throughout and handles a bad Sun *inside* the
yaw-steering branch (`SatAttitude.cpp:118-131`), exactly as wind-up's *inner*
fallback at `:1908-1911` does.  The fork the header warned about had happened.

### §3b ⚠ THE YAW-TURN RESPONSE PASSES A HALF-FIX — it cannot be the acceptance gate

Measured over 200 000 samples, cycles per satellite yaw turn:

| arm | cyc/turn |
|---|---|
| `(+,+)` shipped | **+1.0000** |
| `(−,−)` correct | **−1.0000** |
| `(−,+)` half-fix, satellite only | **−1.0000** ← passes |
| `(+,−)` half-fix, receiver only | +1.0000 |

The sweep rotates only the SATELLITE body frame, so it isolates the satellite
line and is completely blind to the receiver line.  Anyone using "one yaw turn
gives −1 cyc" as the acceptance test signs off on the `(−,+)` half-fix — the exact
failure §3 warns about.  It is kept as case 7 of the gate because it is a genuine
independent handle on `:1944`; it is **not** sufficient alone.

**What DOES separate all four arms** is structural, not statistical.  The
degenerate-dipole guard at `:1947` is an **OR** over the two magnitudes, so a
geometry driving exactly ONE `ε` to zero turns exactly ONE line into an observable:

- `ε_s = 0` ⇔ the receiver sits on the **geocentric** ray through the satellite;
- `ε_r = 0` ⇔ the satellite sits on the receiver's **geodetic** vertical.

These coincide only where the geodetic vertical is parallel to the geocentric
radial — equator and poles — which is why both isolating cases sit at **45 °N**,
where the 0.192° deflection keeps the partner dipole 5.6e4× above the guard.  That
deflection is the entire lever.  Measured (same build, only the two signs differing):

| arm | satellite-boresight case | receiver-zenith case |
|---|---|---|
| oracle | −0.024501 m | −0.024514 m |
| `(+,+)` shipped | 0.0 **FAIL** | 0.0 **FAIL** |
| `(−,+)` half-fix | +0.070646 **FAIL** | 0.0 **FAIL** |
| `(+,−)` half-fix | 0.0 **FAIL** | −0.070633 **FAIL** |
| `(−,−)` fixed | −0.024501 pass | −0.024514 pass |

Case A admits `{(−,+), (−,−)}`; case B admits `{(+,−), (−,−)}`; intersection
`{(−,−)}`.  ⚠ If anyone "tidies" those latitudes toward 0 ° or 90 °, the half-fix
guard silently stops guarding — the premise `ASSERT`s exist to make that loud.

### §3c The 0.2512 / 0.2522 figures are NOT constants

Reproduced qualitatively everywhere and quantitatively nowhere.  Independent
re-measurement, all differences wrapped into (−0.5, +0.5]:

| population | flip `:1944` only | flip `:1945` only |
|---|---|---|
| 30 000 random geometries, el ≥ 10° | 0.2499 / 0.2478 | 0.2478 / 0.2500 |
| 10 099 rows of real GCGO field geometry | **0.2003 / 0.2008** | 0.2010 / 0.2001 |

The load-bearing claim — a half-fix matches NEITHER the model nor its negative,
and sits near the median of `|U(−0.5, 0.5)|` = 0.2500 — holds in every arm on both
populations.  The field figure is 0.20, not 0.25, because the real elevation /
azimuth distribution is not uniform.  **Stop quoting 0.2512 as a fixed number.**

---

## §4 Impact — where it lands, and where it does not

MEASURED on the FT63 roster (63 stations, GPS, DOY-129, el ≥ 15°), per-arc/chord
**range** of `2φ_wu` — the part no constant ambiguity absorbs:

| domain | med (cyc) | p90 | max | at L1 |
|---|---|---|---|---|
| ZD arc (station, satellite) | **0.414** | 1.037 | 1.599 | 79 mm med / 197 mm p90 |
| SD between-station | 0.065 | 0.195 | 0.430 | — |
| DD chord (≥30 min common, n = 3866) | 0.026 | 0.136 | 0.429 | 4.9 mm / 25.9 mm |

### §4a ⚠ The 79 mm figure is right for the WIRE and wrong for the ESTIMATOR

Three separate factors, each independently measured, each pulling the same way.
The table above reproduces (0.4163 vs 0.414 cyc ZD range; 0.0617 vs 0.065 SD — so
both the original measurement and the re-measurement are sound), but its
statistic and its unit are both wrong for the estimation channel:

1. **It is a RANGE, and the float ambiguity absorbs the arc mean.**  Same 3253-arc
   population: range med 0.4163, **RMS about the arc mean med 0.1207**, RMS about
   mean + linear trend (clock/ZTD soak up a ramp) med 0.0423 cyc.  Measured
   range/RMS ratio 3.37 (theory √12 = 3.46 for a linear ramp).
2. **It is measured in FRACTIONAL mode, so it is the range of the sawtooth `2W`,
   not of the physical wind-up.**  The tell is `max 1.599 < 2.0`.  On the archived
   GCGO trace the unwrapped physical per-arc range is med **0.119** / p90 0.400 /
   max 0.594 cyc — ~3.5× smaller at the median.  Most of the 79 mm is the §7 WRAP,
   which is common to all three A/B arms and cancels in every pairwise difference.
3. **"at L1" is the wrong unit on any dual-frequency lineage.**  The error is
   band-common in cycles, so its per-band metre vector decomposes onto
   {geometric, ionospheric}; the geometry-visible coefficient is
   `κ = c/(f₁+f₂) = 0.106953 m/cyc`, not `λ₁ = 0.190294`.  Ratio **1.779**.  The
   rest is absorbed by the free slant-iono state, or cancels in the IF combination
   — the same number either way.

| statistic | cyc | at λ₁ (the card's unit) | geometry-visible (κ) |
|---|---|---|---|
| arc RANGE (§4 headline) | 0.4163 | **79.2 mm** | 44.5 mm |
| RMS about arc mean | 0.1207 | 23.0 mm | **12.9 mm** |
| RMS about mean + trend | 0.0423 | 8.1 mm | 4.5 mm |

So 79 mm → **~13 mm** median for what reaches the estimator; p90 197 → 39 mm.
**Both numbers belong in the card and each must be labelled.**  Two honest bounds
in the other direction: (a) `κ` is a LOWER bound for the network producer — under
an SH constraint or low iono process noise the iono state cannot take its share
and the effective coefficient migrates toward `λ₁`, so the true network figure is
bracketed `0.107 ≤ κ_eff ≤ 0.190 m/cyc`; (b) for a per-band emitted wire product
(satellite phase bias per band) `λ_b` IS the right unit, which is the one place
§4's framing — "sitting directly on the states the producer publishes" — is
exactly correct.

Also worth recording: §1 consequence 1 (the vanishing `|Ds| ≈ 0.02`) is
rhetorically decisive but **numerically inert for the returned angle** — `atan2`
uses the normalised `Ds_hat`/`Dr_hat`, so a uniformly scaled dipole gives an exact
direction until catastrophic cancellation (~5e-15).  The operative damage is
entirely consequence 2, the phase mirror, plus the near-zenith `Dr` conditioning
in §1a.  A "fix" that restored only the magnitude would have changed nothing.

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


## §8 What was landed

### §8.1 The code — FOUR sites, one commit

`src/core/corrections/GnssCorrections.cpp`:

```cpp
// :1944-1945  the two quadrature signs, together
Eigen::Vector3d Ds = x_s - k * (k.dot(x_s)) - k.cross(y_s);   // satellite
Eigen::Vector3d Dr = x_r - k * (k.dot(x_r)) - k.cross(y_r);   // receiver, y_r = +EAST
```

plus the velocity-fallback reframe (§3a: `z_s = -ez` deleted, `ez` substituted into
both cross products, a `y_s = ez.cross(x_s)` re-derivation added so handedness is
structural rather than incidental), plus the `:1872` comment — the documented
origin of the defect — replaced by the **rule** (`sign = −sgn((x×y)·k)`) rather
than one instance of a form, with the RTKLIB trap named inline.

### §8.2 Invariant #6 — this is a product-frame change and CANNOT be byte-identical

Deliberately landed **without a toggle.**  The mechanism is already default-ON
(`GnssConfigTypes.hpp:3383` and `GnssConfigLoad.cpp:2036` both default `true`) and
armed in **624 of 1013** configs explicitly, with exactly **one** setting it false
(`config/test_suite/_simnet3_bare.json:57`); the rest inherit `true`.  A
`windup_sign_fix: false` default would mean byte-identity is "preserved" only in
the sense that THE DEFECT is preserved, on ~1012 of 1013 configs — which inverts
what invariant #6 is for (it exists so a NEW feature cannot perturb a CORRECT
baseline; here the baseline was not correct).  It would also create a THIRD product
frame, where the mismatch detector is merely advisory.

What byte-identity IS preserved, and it is the right control: **the wind-up-OFF
path.**  Verified on real data, 6/6 cells across three stations —
`state_trace.csv` is byte-identical between the shipped and fixed exes wherever
`enable_phase_windup: false`, and differs at every cell where it is true (§9).
That is a stronger statement than a hash of one golden: it proves the two binaries
differ in **nothing but the wind-up path**.

**Gates that trip and must be re-baselined** (all expected; none is a regression):

| tier | artifact | status |
|---|---|---|
| A | `goldens/decomp/*.manifest.json` — 13 checked-in fingerprint files hashing `network_solution.csv` / `network_dd_fractional.csv` | **ALL 13 TRIP.**  Tracked files; re-baseline in a follow-up against a same-toolchain rerun |
| B | `scripts/rt_seam_gate.ps1` + `rt_seam_compare.py`, 4 arms (`sbf`, `sbf_bssd`, `sbf_te`, `cl`) | **ALL 4 TRIP.**  Goldens under `runs/rtseam_goldens/`; re-capture |
| B | `scripts/rt_seam_verify.py`, `rt_seamcut.py` | **exempt** — source-text checks, invariant to numerics |
| C | `test_suite_results/golden.json` via `run_test_suite.py --golden` | trips, and should |
| — | `scripts/p2_make_guard_configs.py` | does NOT trip — a config generator; its "byte-identical" comment is about generated config TEXT |

⚠ Historical fingerprints quoted in `docs/` (e.g. `SPEC_pivot_visibility_gauge_defect.md:502`,
`SPEC_synthetic_minimum_network.md:1406`) should be **annotated, not re-run**: their
baselines live under `D:\GnssProcessor_archive\` and predate the VS 2026 / MSVC
19.51 toolchain, so they are not re-comparable in any case.

**A/B protocol** (used for §9, and the one to reuse): Release only; build the fixed
exe, snapshot to a renamed copy, `git checkout --` the file, rebuild, snapshot the
shipped copy, hash both.  ⚠ `Copy-Item` **preserves the source file's
LastWriteTime**, so restoring a backup over a source can leave ninja with a stale
`.obj` and a silently wrong binary — touch the file after any restore.  This bit
once during this card and was caught only because the unit gate went red.

### §8.3 The product contract — no new legend field; the existing one was INERT

**Recommendation taken: do NOT extend the v2 legend.**  Field 3
(`windup_yaw_model_id`, legended `windup_id`) already exists positionally in every
v2 product ever written, is parsed unconditionally (`NetworkProductIO.cpp:281`),
written unconditionally (`:426`), legended (`:580`), and compared
**unconditionally** in `checkContract` (`:850-852`) — one of only ~8 fields with no
"compare-only-when-declared" guard.  A model-revision id is exactly what an opaque
string compared by equality is for.

The defect was not a missing field.  It was that the field **could never fire**:

- **(a) nothing in production ever wrote it.**  The producer's stamping block
  (`ApplicationNetworkSetup.cpp:993-1122`) explicitly assigns 17 other contract
  members and never touches this one, so it rode the struct default forever.
- **(b) the consumer's expectation was copied out of the file it was checking.**
  `ApplicationRealtime.cpp:507` adopts the first receipt wholesale, and only
  `reference_receiver_id` / `apriori_product_id` / `frame_id` / `atx_id` /
  `orbit_ref` are overridden by config pins.  `windup_yaw_model_id` was not among
  them, so the unconditional compare at `:850` compared a string **to itself**.
  Mismatch probability: exactly zero, for any file.

Landed:

1. `include/filter/NetworkSbasis.hpp` — a compile-time
   `kWindUpYawModelId = "nominal-wu93"`, and the struct default now references it.
   Deliberately **not** a config knob: it declares what the CODE does, and per the
   `project_product_legend_schema_fix` rule a declaration gated on a knob drifts
   away from the thing it declares.  Because the struct default IS what gets
   written, the emitted wire value changes with no producer edit.
2. `src/core/app/ApplicationRealtime.cpp` — the consumer now pins
   `contract_expectation.decl.windup_yaw_model_id` to **its own** constant instead
   of to the adopted receipt.  Every other pin there is an operator choice that
   sensibly falls back to the file; this one is a convention the CODE fixes, so
   falling back to the file is useless.

Back-compat, read from the parser rather than assumed: the S line is **positional
for fields 1–10, `key=`-dispatched for 11+**, with no version token the C++ reader
consumes.  Field 3 is a verbatim `std::string` assignment — no enum, no numeric
conversion — so the field COUNT is unchanged, and an old consumer parses a new file
cleanly and vice versa.

⚠ **The residual asymmetry is inherent and must be stated:** a NEW consumer reading
an OLD (pre-fix, `"nominal"`) product now trips the mismatch warning; an **OLD
binary reading a NEW product still cannot**, because it has no pin and by
definition cannot be given one.  Only the side that knows its own convention can
compare.

⚠ **And a mismatch is still only a WARNING** — `ApplicationRealtime.cpp:537-541`
logs "Continuing anyway", once per contract *change*, with no counter, no epoch
status, no withhold.  That is by design (`:477-479`, "HARD-WARN (never abort)"),
but it means the detector is advisory.  Giving contract mismatch a machine-readable
channel is a separate card.

### §8.4 Warm resume across the fix — invariant #1 + #7, and it was SILENT

**The wind-up accumulator itself is innocent**: `prev_windup_`
(`GnssCorrections.hpp:543`) is a mutable per-instance map, process-local, never
serialized, re-seeded on first sighting.

**The network producer snapshot is not.**  Every carried quantity that ABSORBED the
`+2·φ_wu` residual is serialized (`NetworkStateSnapshot.hpp:138-208`): `local_carry`
ambiguity values, `shared_carry` PB/CB, the phase-leveling zero-q level,
`clock_osc`, the invariant-DD `carry_state` blob.  And the resume guard
(`NetworkRunContext.cpp:559`) hashes the station roster plus ~40 CONFIG knob tokens
and contains **no code version, no binary hash, no git rev** — nor does
`getModelCorrections` appear anywhere in that block.  A code-level sign fix cannot
move a config fingerprint under any circumstance, so a pre-fix snapshot would have
warm-resumed silently into a corrected binary and re-referenced stiff zero-q carries
levelled in a frame that no longer exists.  That is precisely invariant #1's
stale-stiff-prior-past-a-jump and invariant #7's un-revalidated re-coupling edge.

Landed: `NetworkRunContext.cpp` appends `kWindUpYawModelId` to the fingerprint, so
every pre-fix snapshot COLD-starts across the fix.  It is the only token there that
is a code constant rather than a knob, and the comment says why.

### §8.5 Re-grading — what must be annotated

- **Sim-based validations: nothing to retract** — they were blind (§2), so their
  verdicts were computed with the defect cancelled and remain valid *as sim results*.
  They also never tested the thing, which is the §2 finding.
- **DD / relative field grading: largely protected** (§4), the satellite-yaw part
  being station-common.
- **WIRE-quality claims are the exposure** — every satellite-clock and
  satellite-phase-bias grade in the tree was measured with `+2·φ_wu` present on the
  ZD states the producer publishes.  Those need an annotation, not a retraction:
  the defect is a frame offset on the emitted product, and a re-grade is owed
  before any of them is quoted as an absolute wire-quality number.

---

## §9 THE FIELD RESULT — the three-arm ladder, 2026-08-21

Ran per §8.2's protocol.  Manifest `config/test_suite/manifest_windup.json`,
analyzer `scripts/windup_ab_linearity.py`, 28 runs, three known-coordinate stations
on 2024-129 graded against ITRF2020 SINEX.  Exes:
`gnss_processor_shipped.exe` `D50F4C56…`, `gnss_processor_windupfix.exe` `4380A2B6…`.

```
arm1 = wind-up ON,  shipped exe (inverted sign)
arm2 = wind-up OFF
arm3 = wind-up ON,  sign-corrected exe
```

### §9.1 Why the ladder, and not "ON must be worse than OFF"

**The original §8 item 1 criterion is not guaranteed and was replaced.**  With `u`
the ENU shift from one unit of wind-up and `v` all other error vs truth,

```
|v + 2u|² − |v + u|² = 3|u|² + 2 v·û
```

so ON is *better* than OFF whenever `v·û < −1.5|u|`.  It holds on a well-controlled
run only because `|u|` happens to exceed the competing error — not by construction.
**Measured: it fails at JPLM, in both postures** (§9.3).

The ladder is truth-free and sign-free instead.  The arms differ by an additive
per-observation term, so for a linear estimator

```
(I)  second difference  arm1 − 2·arm2 + arm3 ≡ 0
(II) ladder slope       (arm1 − arm3) = 2 · (arm2 − arm3)
```

and under the NULL (shipped sign already right) `arm3 ≡ arm1`, giving slope **0**.
**Slope 2 vs slope 0, with nothing in between.**

### §9.2 Controls — both pass

| control | result |
|---|---|
| wind-up OFF, shipped vs fixed exe, `state_trace.csv` | **BYTE-IDENTICAL, 6/6 cells** (3 stations × frac/acc) |
| wind-up ON, shipped vs fixed exe | **DIFFERS, 3/3** |
| `b_wu_off_frac` vs `d_wu_off_acc` within one exe | identical — `accumulate_phase_windup` is a no-op when the call is gated off |

The first two together prove the binaries differ in nothing but the wind-up path,
and that the runs are deterministic (`eigen_threads: 1` pinned in every cell).

### §9.3 Result — slope lands on 2, never on 0

Accumulated posture (the verdict posture — see §9.5):

| station | slope E | slope N | slope U | R² E/N/U |
|---|---|---|---|---|
| **GCGO** | 2.086 | 2.024 | 2.068 | 0.937 / 0.998 / 0.961 |
| **JPLM** | 2.071 | 1.951 | 1.559 | 0.993 / 0.949 / 0.904 |
| **CIT1** | 1.886 | 1.661 | 1.669 | 0.392 / 0.646 / 0.050 |

Nine of nine components land near 2; none is near 0.  **The null is rejected at all
three stations.**  The shipped model applies the negative of the corrected one, on
real data, inside the solve.

**Arc-demeaned post-fit CARRIER-PHASE residual RMS — the sharpest observable, and
it is unanimous.  7 of 7 configurations, monotone in the predicted direction:**

| configuration | arm3 fixed | arm2 OFF | arm1 shipped | rows/arm |
|---|---|---|---|---|
| JPLM accumulated | **13.042** | 13.521 | 14.609 | ~110 800 |
| JPLM fractional | **13.010** | 13.521 | 14.437 | ~110 800 |
| CIT1 accumulated | **13.122** | 13.936 | 14.851 | ~63 850 |
| CIT1 fractional | **13.175** | 13.936 | 14.945 | ~63 850 |
| GCGO accumulated | **9.771** | 10.816 | 11.993 | ~116 650 |
| GCGO fractional | **10.078** | 10.816 | 12.055 | ~116 650 |
| JPLM kinematic acc | **11.068** | 11.256 | 12.378 | ~110 815 |

mm, arc-demeaned, from `logs/measurement_residuals.csv`.  This is the observation
domain, where the effect is a 5–15 σ signal against `phase_std_m = 0.002`, rather
than the position domain where it is millimetres against a centimetre bias.

**Truth-graded ENU (the price, not the proof)** — `|ENU error|` in mm:

| configuration | arm3 fixed | arm2 OFF | arm1 shipped | ordering |
|---|---|---|---|---|
| CIT1 acc | **46.79** | 53.70 | 59.97 | monotone ✓ |
| CIT1 frac | **46.69** | 53.70 | 58.33 | monotone ✓ |
| GCGO acc | **56.67** | 60.02 | 66.07 | monotone ✓ |
| GCGO frac | **57.25** | 60.02 | 66.27 | monotone ✓ |
| JPLM acc | 49.64 | 45.84 | **45.33** | INVERTED |
| JPLM frac | 49.47 | 45.84 | **46.11** | INVERTED |
| JPLM kin | 73.85 | 60.08 | **57.40** | INVERTED |

**The inversions are predicted a priori and are not evidence against the fix.**
Evaluating `v·û < −1.5|u|` with each configuration's own measured `u` and `v`
predicts the ordering correctly in **7 of 7** cases — monotone where it says
monotone, inverted where it says inverted:

| configuration | \|u\| mm | v·û | threshold −1.5\|u\| | predicted | observed |
|---|---|---|---|---|---|
| JPLM acc | 9.68 | **−23.60** | −14.51 | INVERT | INVERT ✓ |
| JPLM frac | 9.69 | −22.67 | −14.54 | INVERT | INVERT ✓ |
| JPLM kin | 14.41 | −71.18 | −21.61 | INVERT | INVERT ✓ |
| CIT1 acc | 8.98 | +34.15 | −13.48 | monotone | monotone ✓ |
| CIT1 frac | 9.05 | +34.39 | −13.58 | monotone | monotone ✓ |
| GCGO acc | 10.26 | +13.90 | −15.38 | monotone | monotone ✓ |
| GCGO frac | 14.45 | +4.02 | −21.67 | monotone | monotone ✓ |

Both inversions are at JPLM, and both are driven by JPLM's **pre-existing,
unexplained +45 mm (static) to +73 mm (kinematic) standing Up bias** — a defect of
the vertical, not of the wind-up sign, and already on the books
(`project_vertical_instability_solidtide`).  The wind-up shift at JPLM is
Up-weighted (`û_U = −0.37`), so a corrected model pushes the solution *along* that
pre-existing bias.  This is precisely why the truth ordering cannot carry the
verdict, and why the ladder and the residual RMS do.

**The price, in the corrected frame:** `|arm2 − arm3|` = **8.98 – 14.45 mm** of ENU
per unit of wind-up, so the shipped code was carrying roughly **2×** that — ~18–29
mm of position error at these three sites, and 8–20 % of the arc-demeaned phase
residual RMS.

### §9.4 ⚠ Why the second difference is NOT at the numerical floor — arm-dependent editing

Identity (I) predicted micrometres; measured max `|arm1 − 2·arm2 + arm3|` ran from
3.8 mm (GCGO N) to 1195 mm (CIT1 U).  **Diagnosed, not hand-waved: the arms do not
process the same observations.**  Row counts in `measurement_residuals.csv`:

| station | arm1 shipped | arm2 OFF | arm3 fixed |
|---|---|---|---|
| JPLM | 222 207 | 222 226 | **222 228** |
| CIT1 | 127 610 | 127 616 | **127 620** |
| GCGO | 233 325 | 233 338 | **233 339** |

Monotone: the arm carrying the largest residual loses the most observations.  QC is
arm-coupled — a 2× residual trips elevation / variance / innovation gates that a 1×
residual does not — so strict superposition cannot hold and identity (I) is an
approximation, not an identity, on a QC-armed run.  CIT1 is worst because it is
GPS-only RINEX 2 at 15 s with the poorest convergence (17 160 s).

Two consequences worth carrying:

- **Identity (I) is only exact with QC disarmed.**  A future ladder that wants the
  micrometre check must run with snooping / AR / slip repair off; the identity
  closing there and not here would itself be the reportable finding.
- **It makes arm1 look BETTER than it is** — the shipped arm is also having its
  worst observations discarded.  Every arm1 number above is therefore conservative.

### §9.5 ⚠ The sign fix and the §7 WRAP are coupled in the deliverable, though independent in mechanism

Residuals per posture (`W` = wrapped wind-up, `N` = the integer staircase, `φ = W + N`):

| posture | arm1 ON shipped | arm2 OFF | arm3 ON fixed |
|---|---|---|---|
| **accumulated** | `2φ` | `φ` | **0** (per-arc constant, ambiguity-absorbed) |
| **fractional** | `N + 2W` | `φ` | **`N`** — the one-cycle staircase |

In ACCUMULATED mode the corrected arm's residual is fully absorbed.  In FRACTIONAL
mode it is `N`: mostly a per-arc constant, but stepping a **full cycle** (0.19 m at
L1) at each half-cycle crossing — 43 measured jumps/day on one station's trace,
~753/day scaled to 63 stations, mid-pass and in good data (§7).

`estimation.accumulate_phase_windup` defaults **FALSE**
(`GnssConfigLoad.cpp:1135-1136`), so **every field lineage runs fractional.**  The
measured phase residuals above still favour the corrected arm in both postures
(13.010 vs 13.521 vs 14.437 at JPLM fractional), so the fix is a win as landed —
but the fractional corrected arm is trading a smooth systematic for a
discontinuity, which is a standing-invariant #1 shape.

**⇒ Follow-on card, and it should be next: revisit the
`accumulate_phase_windup` loader default.**  The two defects are independent —
arming accumulation does not fix the sign, and the sign fix does not remove the
wrap — but the sign fix is what makes the wrap the dominant remaining wind-up
error on a field lineage.  Do not close §7 by assuming this card covered it.

### §9.6 What this run does and does NOT certify

- ✅ The PPP / PPP-RTK **user** path, three stations, static and kinematic, both
  accumulation postures.
- ❌ The **network producer**, which §4 argues is where this actually lands (the
  emitted satellite clocks and phase biases).  `enableWindUpTracing` has exactly one
  caller — `Application.cpp:1630-1631`, inside `Application::run()` *after* the mode
  branches — so the network, baseline and real-time paths never write
  `windup_trace.csv`, and `ApplicationNetwork.cpp` never calls `logResiduals`, so
  there is no `measurement_residuals.csv` either.  The network analogue is
  `network_station_residuals.csv` (`NetworkRunTraces.cpp:696`, gated on
  `network.trace_station_residuals`); a σ̂₀ / `s_norm` ladder across the three arms
  is the owed network arm.
- ❌ **Absolute wire-quality re-grades** (§8.5).
- ⚠ **GCGO's SINEX-vs-SIF cross-check is 5.45 cm** — flagged by the harness's own
  5 cm tolerance, because its last SSC solution window ends 20:366 and 2024 is an
  extrapolation.  Its *differential* ladder is unaffected (it needs no truth); its
  absolute bias is truth-ambiguous at a scale larger than the entire effect.  Quote
  absolute grades from JPLM (0.50 cm SINEX/SIF agreement) and CIT1 (1.38 cm).

### §9.7 Replication + extension — the 5-station 4-arm matrix (2026-08-21, second session)

Independent re-execution with a different harness (`scripts/windup_ab_slice.py`,
`test_suite_results/windup_ab/`), different exes (shipped `727A9E96…` built at
`3861718` — the last pre-fix commit — in a worktree; fixed `AA472278…` from the
fixed working tree), five stations
(JPLM, ALGO, ALIC, ABPO, ZIMM — three not in §9's set), daily static PPP
GPS+Galileo on 2024-129 vs ITRF2020 SINEX, warm-up 1800 s.  Four arms close the
loop the ladder's three cannot: `shipped_off` vs `fixed_off` is a SAME-SOURCE
identity gate across the two binaries.

- **Identity gate: EXACT.**  `shipped_off ≡ fixed_off` in every metric at every
  station (3D-RMS 4.83/11.67/3.70/5.67/2.37 cm) — the sign is the only mover
  between the two builds on the user path.
- **Horizontal RMS (the channel wind-up actually loads; cm):**

  | arm | JPLM | ALGO | ALIC | ABPO | ZIMM | mean |
  |---|---|---|---|---|---|---|
  | shipped_on  | 2.1 | 1.9 | 4.4 | 6.0 | 1.5 | 3.18 |
  | off (both)  | 1.1 | 1.2 | 3.1 | 4.3 | 1.6 | 2.26 |
  | fixed_on    | 1.0 | 0.7 | 0.8 | 2.7 | 1.3 | 1.30 |

  `fixed_on` beats OFF at **5/5** stations; `shipped_on` is worse than OFF at
  4/5 (ZIMM ~tie) — the §8 item 1 shape, now at five geometries.  The East bias
  collapses under the fix (ALIC −4.1 → −0.7 cm, ABPO −5.7 → −2.1 cm).
- **§9.1's caveat reproduces**: in 3D at JPLM, `fixed_on` (5.16 cm) > OFF
  (4.83 cm) — the `v·û` geometry, not a counter-signal; JPLM's H-RMS still
  improves (1.1 → 1.0 cm).  3D is diluted by arm-invariant vertical biases
  (ALGO carries +11.5 cm U in all four arms).

### §9.8 §8 item 2 CLOSED — the contract now covers every consumer path

The `kWindUpYawModelId = "nominal-wu93"` token (S-record field 3) is pinned by
the BINARY, not adopted from the file, on all three consumer entry points:

- `ApplicationRealtime.cpp` `checkConsumerContract` — pin at the expectation
  seed; the check now runs on EVERY consuming posture (previously gated on the
  `network_consumer` profile, leaving the legacy default-flags tail silent),
  and an S-less (v1) wire draws a one-shot UNVERIFIABLE warn instead of
  default-matching silently.
- `ApplicationConsumerReplay.cpp` — previously read the contract and never
  called `checkContract` at all: the offline replay driver, the path most
  exposed to archived pre-fix products, consumed them with zero trip.  Now
  adopts the file contract pivot-less, pins the wind-up token, hard-warns
  (never aborts) on mismatch, and warns on a v1 wire.
- `networkSnapshotFingerprint` (`NetworkRunContext.cpp`) — the token is in the
  warm-resume fingerprint, so resuming a pre-fix snapshot cold-starts (or, on
  require-armed lineages, hard-errors LOUDLY — intended).

Ops consequences, both intended and both by design LOUD:
- Every stored pre-fix product / golden byte-cert hash (e.g. the
  `_camp60_bytecert*` captures, exe `919bb885`) is now an **expected-fail**
  baseline — the fix is solve-affecting on every path.  Re-capture, do not
  re-grade.
- Live snapshots on disk written pre-fix will not warm-resume across the fix
  (8 present on 2026-08-21).

### §9.9 The sim is frame-consistent again — fixtures regenerated

`runs/simnet60_campaign_wu93` + `runs/simnet60_quiet_wu93` regenerated with the
fixed simulator (`21C2DCD0…`).  A pre-fix fixture consumed by a fixed producer
carries −φ injected vs +φ modelled = a 2φ_wu band-common phase error in every
observation — the same mismatch class the product token guards, one layer down.
Any sim arm that mixes a pre-fix fixture with a post-fix producer (or vice
versa) is confounded; the fixture and the producer must sit on the same side of
the fix.  (Same-side pairs cancel identically — §2 stands: the sim remains
structurally blind to the sign itself.)
