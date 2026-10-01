# I2S: the sign of the second-order ionospheric term

**STATUS**

- **id:** I2S
- **topic:** ionosphere
- **status:** closed
- **summary:** FIXED 2026-10-01. The engine's second-order ionospheric term (`HigherOrderIonosphere::slantHigherOrderComponents`, behind `model_corrections.enable_higher_order_ionosphere`, default off) projected the geomagnetic field on the receiver-to-satellite line of sight with a positive code coefficient, which inverts the term. The correct projection is on the propagation direction (satellite to receiver), as card SOI section 1 derived. Four independent lines agree: a first-principles cold-plasma eigenmode solve (32 of 32 checks, `scripts/higher_order_iono_sign_check.py`); the literature read with its own direction conventions (IERS Conventions 2010 eqs. 9.19 to 9.23 and 9.46, Moore and Morton 2011, Petrie et al. 2011, Hoque and Jakowski 2007); a handedness anchor from the field-validated phase wind-up model, which closes the IEEE-versus-optics naming question; and two independent implementations with a correct field (Ginan, PRIDE PPP-AR). POINT, the engine's reference, carries the same inversion, and nothing in how it applies the term compensates. The engine and the oracle were fixed together: the four I2 goldens flip sign, I3 is unchanged, a direct sign test was added (the old vector fails both tests), and gtest gives 2742 passed with 1 fixture skip. No network result depends on the term, which is off on every posture. PPP output produced with the knob on before this fix carries the inverted term.
- **source:** [SOI](SOI_second_order_ionosphere_leak_2026-10-01.md) section 1; `src/core/atmosphere/HigherOrderIonosphere.cpp`, `include/atmosphere/HigherOrderIonosphere.hpp`, `scripts/higher_order_iono_reference.py`, `tests/test_atmosphere_models.cpp`; POINT `higher_order_iono/Class_Iono.cpp` and `gnss/GnssReceiver.cpp` under `D:\GnssProcessorSupport\reference_software\point\src\trunk\`; [WINDUP-FIELD-LADDER](../cards/network-vehicle/WINDUP-FIELD-LADDER.md) section 9.3
- **knobs / posture:** `model_corrections.enable_higher_order_ionosphere` (default off; off on both goldens; armed only in `config/ppp_rtk_internal_AR.json`)
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** no runs; script `scripts/higher_order_iono_sign_check.py` (32 of 32 checks, exit 0); gtest `HigherOrderIonosphereTest.MatchesCanonicalReference` (goldens regenerated from the corrected oracle) and `HigherOrderIonosphereTest.SignFollowsPropagationAlongField` (new); full Debug suite 2743 cases, 2742 passed, 1 skipped (`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`, fixture-gated)

## 1. The question

The engine formed the unit vector u from the receiver to the satellite (`sat_pos - rx_pos`,
labelled "signal direction"), took B at the pierce point from the dipole of `MagneticField.cpp`,
and modelled

    I2_code = +7527 c STEC (B.u) / f^3,    I2_phase = -1/2 I2_code,

both added to the modelled observable (STEC in electrons per square metre, B in tesla, f in
hertz). The oracle `scripts/higher_order_iono_reference.py` used the same u, so it could not
catch a sign error, and the pinned goldens encoded the engine's sign. Card SOI section 1 derived
the opposite sign from the polarization of the signal. The POINT reference the engine was checked
against also uses u. The magnitude (7527 c) and the phase ratio (-1/2) are not in question.

## 2. The derivation

1. An electron (charge -e) in a field B gyrates right-handedly about B: m dv/dt = -e v x B
   turns v counter-clockwise when viewed with B pointing at the viewer.
2. In a cold magnetized electron plasma, propagation along B has two circular modes. The R mode,
   whose field rotates with the electrons (right-handed about B), has n^2 = 1 - X/(1 - Y). The L
   mode, rotating against them, has n^2 = 1 - X/(1 + Y). Here X = 80.6 N_e / f^2 and
   Y = e B / (2 pi m_e f).
3. GPS signals are right-hand circularly polarized in the IEEE sense: the field rotates clockwise
   when viewed from the source looking along the propagation direction k (satellite to
   receiver), that is, right-handed about k. IS-GPS-200N section 3.3.1.9 states "RHCP" without
   defining a convention. Petrie et al. 2011 (section 3.2, Fig. 5) read the ICD's RHCP as the
   IEEE definition and contrast it with the opposite optics convention. Section 4 below settles
   the handedness without relying on either naming.
4. With k along B, a signal right-handed about k is right-handed about B, so it propagates in
   the R mode. With k against B it propagates in the L mode. In the quasi-longitudinal
   approximation, the index of the mode right-handed about k is, for every angle theta_k between
   k and B,

        n = 1 - X/2 - X Y cos(theta_k) / 2 - X^2 / 8.

   The group index is n_g = d(f n)/df = 1 + X/2 + X Y cos(theta_k) + 3 X^2 / 8. Integrated along
   the path:

        I2_code  = +7527 c STEC B cos(theta_k) / f^3 = +7527 c STEC (B.k) / f^3
                 = -7527 c STEC (B.u) / f^3,                       u = -k,
        I2_phase = -1/2 I2_code.

   The coefficient is 7527 c = e^3 / (8 pi^3 eps0 m_e^2) = 2.2566e12 (equal to e A / (2 pi m_e)
   with A = 80.6).

The code term is a delay when the signal travels along B and an advance when it travels against
it. In the northern hemisphere B points north and down, so a satellite to the south (signal
travelling north and down) gives a positive code term. The engine's form gave a negative one.

## 3. The numerical check

`scripts/higher_order_iono_sign_check.py` assumes no Appleton-Hartree formula and no handedness
naming. It builds the dielectric tensor from m dv/dt = q (E + v x B) with q = -e. It solves the
plane-wave eigenproblem n^2 (k (k.E) - E) + eps E = 0, labels each mode by the rotation sense of
its real time-domain field about k ((E x dE/dt).k), and differentiates f n(f) numerically. GPS L1,
N_e = 1e12 per cubic metre, |B| = 3e-5 T. Results (32 of 32 checks pass, exit 0):

| check | result |
| --- | --- |
| free electron (RK4), two field orientations | right-handed about B |
| circular drive, real time domain, co-rotating / counter-rotating | chi = -0.14274 / -0.07690 against -X/(1 - Y) = -0.14286 / -X/(1 + Y) = -0.07692 |
| mode right-handed about k, theta_k 0, 30, 60, 89, 91, 120, 150, 180 deg | n - (1 - X/2) + X^2/8 equals -X Y cos(theta_k)/2 (relative error at most 6.8e-4 away from 90 deg, 1.5e-2 at 89 and 91 deg); never +X Y cos(theta_k)/2 |
| time conventions exp(-iwt) and exp(+iwt) | identical tables |
| axial ratio of that mode | 1.00000 at 0 and 180 deg, 1.00040 at 60 deg, 1.015 at 89 deg |
| second-order code excess, theta_k 0 / 60 / 120 / 180 deg | +1.7329e-3 / +8.666e-4 / -8.649e-4 / -1.7301e-3 m against +7527 c STEC B cos(theta_k)/f^3 = +1.7313e-3 / +8.657e-4 / -8.657e-4 / -1.7313e-3 m (10 TECU slab) |
| phase excess over code excess | -0.4998 to -0.5001 |
| the gtest's pinned geometry (B points north and down, k mostly down, B.k = +2.673e-5 T, theta_k 43.5 deg) | eigenmode code I2 +7.724e-3 m (100 km slab, 50 TECU); the formula +7.714e-3 m; the engine's former golden -7.714e-3 m, an exact sign flip |

## 4. The handedness anchor

The derivation needs the received field to rotate right-handed about k. The repo's phase wind-up
model (`GnssCorrections::computePhaseWindUp`, Wu et al. 1993 with k = satellite to receiver,
added to the modelled range-like phase) was validated on field data:
[WINDUP-FIELD-LADDER](../cards/network-vehicle/WINDUP-FIELD-LADDER.md) section 9.3 has the
corrected sign beating OFF, and OFF beating the inverted sign, in post-fit carrier-phase RMS in 7
of 7 configurations. The model's response to an antenna rotation about k does not depend on the
antenna frame. A crossed-dipole transmit and receive pair with a matched quarter-period hybrid,
simulated in the real time domain, reproduces that response only when the received field rotates
right-handed about k:

| antenna rotated +5 deg right-handed about k | wind-up model | field right-handed about k | field left-handed about k |
| --- | --- | --- | --- |
| receiver | +5.000 deg | +5.000 deg | -5.000 deg |
| satellite | -5.000 deg | -5.000 deg | +5.000 deg |

RINEX carrier phase has the sign of range, so a larger output delay is a larger modelled phase.
The field data therefore reject a carrier left-handed about k, whichever naming convention the ICD
intends.

## 5. The literature

Read first-hand (2026-10-01):

- **IERS Conventions 2010, chapter 9** (version of 10 June 2013, section 9.4). Eq. 9.19, the
  explicit index for the extraordinary wave, carries a minus sign on the N_e B cos(theta) / f^3
  term. Eqs. 9.20 to 9.22 give the phase delay -s2 / f^3 with s2 = 1.1283e12 times the integral
  of N_e B cos(theta) from transmitter to receiver. Theta is the angle between B and the
  electromagnetic propagation direction k (text after eq. 9.16 and at eq. 9.46). The text
  associates the extraordinary wave with right-hand polarized signals such as GPS. The code
  delay is then +2 s2 / f^3 = +7527 c STEC B cos(theta_k) / f^3, the sign of section 2.
  ⚠ Eq. 9.16 (Y_L = -(omega_g/omega) cos(theta)) combined with the lower sign of eq. 9.18 gives
  the opposite sign if read literally. Eq. 9.19 and every equation after it carry the physical
  sign.
- **Moore and Morton 2011** (Radio Science 46, RS1008). Theta is the angle between the wave
  normal and B. The right-hand polarized GPS signal travels as the X mode for |theta| below
  about 90 deg and as the O mode above. s = 7527 c times the integral of N_e B0 cos(theta) dl.
  They also state, without notation, that near the geomagnetic equator signals arriving from the
  south travel as the X mode with a negative second-order carrier-phase error, and signals from
  the north as the O mode with a positive one. There B is horizontal and northward, so a signal
  from the south travels along B. The physical phase term -1/2 (7527 c STEC B.k / f^3) is
  negative, as they state. The engine's former phase term, -1/2 (7527 c STEC B.u / f^3), was
  positive for that ray.
- **Petrie et al. 2011** (Surveys in Geophysics 32:197-253). Theta is the angle between B and
  the signal propagation direction k (section 3.1). The ICD's RHCP is the IEEE convention
  (section 3.2). For the RHCP signal the lower sign of the Appleton-Lassen expansion applies
  for theta below 90 deg and the upper sign above, giving n = 1 - X/2 - X Y cos(theta)/2 -
  X^2/8 for every theta (section 3.5, eqs. 10 to 11b), the index of section 2. Section 3.6 traces
  the later literature's confusion over which sign applies: Brunner and Gu 1991 assign the lower
  sign to right-hand polarization everywhere, while Hartmann and Leitinger 1984 limit that to the
  geomagnetic northern hemisphere. Kedar et al. 2003 are cited for the finding that all stations
  move southwards when the term is applied.
- **Hoque and Jakowski 2007** (J Geod 81:259-268). Theta is the angle between the wave
  propagation direction and B. The path runs from the satellite to the receiver. Only the
  extraordinary (right-hand) mode is used for GPS.

Not read first-hand (paywalled or blocked when retrieved): Bassiri and Hajj 1993,
Hernandez-Pajares et al. 2007, Kedar et al. 2003 and Datta-Barua et al. 2008 (the latter three
are known here through the IERS chapter and Petrie et al. 2011). The literature sweep reported
that Bassiri and Hajj 1993 and Brunner and Gu 1991 write |cos(theta)|, which is sign-definite and
does not decide the direction question.

## 6. The implementations

| implementation | vector dotted with B | code coefficient | application | net code I2 |
| --- | --- | --- | --- | --- |
| this engine, before the fix | u = sat - rx | +7527 c | added to the model | +K STEC (B.u)/f^3 |
| POINT, IGRF path (`Class_Iono.cpp` `Project_Vector`) | dx = Xs - Xest (u); B from IGRF north, east, down on frame vectors whose third member `Ut` is -r (down), so B is formed correctly | `const1` = e A lambda^3 / (c^3 2 pi m_e) > 0 | code C - I1 - I2 - I3; phase L + (I1 + I2/2 + I3/3)/lambda (GLONASS, Galileo); GPS phase adds (I1 + I2 + I3)/lambda | +K STEC (B.u)/f^3 |
| POINT, dipole path (`BtJ`) | (cos phi_m sin z cos a - 2 sin phi_m cos z) Beq from the receiver-to-satellite geomagnetic azimuth and zenith (u), Beq = +3.12e-5 T | as above | as above | +K STEC (B.u)/f^3 |
| Ginan (`src/cpp/pea/ppp_obs.cpp`, `pppIonStec2`) | `satStat.e = (rSat - rRec)` (u) | `alpha = -1 * alpha_ * rB.dot(satStat.e)`, `alpha_ = 7527e16 lambda / f^2`; code factor +1, phase -1/2 | added like the first-order term | -K STEC (B.u)/f^3 |
| PRIDE PPP-AR (`src/lsq/gpsmod.f90`, `src/lib/calB0Theta.f90`, `src/lib/calstech.f90`) | `rf = xant - xsat` (k); IGRF converted to east, north, up as (IY, IX, -IZ) | `u2 = 7527e16 c B cos(theta) / f^3` | code `obs - range - stec(3)` with +u2; phase `obs - phase + stec(1)` with +u2/2 | +K STEC (B.k)/f^3 = -K STEC (B.u)/f^3 |

K = 7527 c, with STEC in electrons per square metre. POINT's sign agrees with the engine's, but it
is the engine's reference, not an independent check. Nothing in POINT's application compensates
the vector: code is corrected by subtracting I2, and phase by adding it at half size, or at full
size on its GPS path (a factor inconsistency with the same sign). A disabled `#if 0` block in
`Project_Vector` used the satellite-to-receiver vector but mixed a local-frame B with an ECEF
vector, so it decides nothing. Ginan and PRIDE PPP-AR both form a physically correct field and
both use the propagation direction. The sweep also reported that GROOPS and goGPS intend B.k with
k from satellite to receiver but feed the downward IGRF component into their "up" slot. That was
not verified here, and with that defect they match neither convention.

The inverted form probably comes from the POINT lineage (a port of RINEX_HO), together with the
literal reading of IERS eqs. 9.16 and 9.18. The sweep also reported that the printed eqs. 7 and 8
of Kedar et al. 2003 evaluate to B.u, against that paper's own text. This origin is inferred, not
verified.

## 7. The fix (2026-10-01)

- `HigherOrderIonosphere.cpp`: k is formed as `rx_pos_ecef - sat_pos_ecef` (the propagation
  direction). The coefficient, the -1/2 and -1/3 phase factors, and I3 are unchanged. The comment
  that called the line of sight the "signal direction" is corrected, and the sign rationale is
  stated beside the coefficient.
- `HigherOrderIonosphere.hpp`: the Doxygen states theta_k from the propagation direction, and
  that the code term is signed by B.k.
- `scripts/higher_order_iono_reference.py`: the same vector change. The docstring records the
  fix as item 3.
- `tests/test_atmosphere_models.cpp`: the goldens are regenerated from the corrected oracle. I2
  flips sign exactly and I3 is unchanged:

  | case | I2 before (m) | I2 after (m) |
  | --- | --- | --- |
  | L1 code, 50 TECU | -0.007714137670442714 | +0.007714137670442714 |
  | L1 phase, 50 TECU | +0.003857068835221357 | -0.003857068835221357 |
  | L2 code, 50 TECU | -0.016304437097686218 | +0.016304437097686218 |
  | L1 code, 120 TECU | -0.018513930409062514 | +0.018513930409062514 |

  New test `HigherOrderIonosphereTest.SignFollowsPropagationAlongField` checks the sign without
  an oracle. The receiver and the satellite sit on the field line through the pierce point. Code
  I2 must be +7527 c STEC |B| / f^3 along B and the negative of that against B; phase I2 must be
  -1/2 of code; I3 must not depend on the direction.
- Mutation check: with the old vector restored, both tests fail. With the fix, both pass, and the
  full Debug suite gives 2743 cases, 2742 passed, 1 fixture-gated skip (VS 2026 toolchain,
  `scripts/vs2026_build.ps1`).
- `docs/report/atmosphere.tex` (eq. atmos-i2): k is defined as the propagation direction, with
  the sign rationale. `docs/report/main.pdf` was not rebuilt.

## 8. Scope

- Network postures: the term is off on both goldens and on every network arm, and the
  self-contained posture loads no global ionosphere map. No network number changes. Card SOI's
  test computed its prediction with the propagation-direction sign (`scripts/clock_level/soi_test.py`),
  so its reading stands.
- PPP: the only configuration that arms the term is `config/ppp_rtk_internal_AR.json`. PPP or
  PPP-RTK output produced with the term on before this fix modelled the term with the wrong
  sign, an error of twice the term on each signal. That is up to a few centimetres on code at
  high slant TEC, and on DOY-129 twice card SOI's ionosphere-free size (about 2.8 mm at the
  median line of sight, 9.4 mm at p90).
- POINT lives outside the repository and is not modified.
- Owed: nothing on the network path. A field grade of the term's sign is not possible with the
  instruments of card SOI, which could not see the term at all.
