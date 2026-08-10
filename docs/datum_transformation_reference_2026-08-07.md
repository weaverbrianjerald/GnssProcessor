# Datum transformation reference — ITRF2020 / ITRF2014 / NAD83(2011)

**Status:** reference. Captured 2026-08-07 from primary sources (IGN/IERS, NGS).
**Scope note:** GnssProcessor itself is **pure ITRF end-to-end** — the SIF
(`data/static/sif/*.json`, ITRF2020 @ 2020.0) and the IGS SSC
(`data/static/sinex/ITRF2020-IGS-TRF.SSC`) are the only frame realizations the
engine touches, and there is no NAD83 anywhere in the codebase. This document
exists for *external* work that has to cross into a national datum — control
network re-adjustment, OPUS/RTN comparisons, NSRS-modernization demonstrations.
Nothing here is consumed by the C++ engine.

---

## 1. The transformation, and the two sign conventions

Both conventions below describe the *same physical transformation*. They differ
only in the sign of the rotation parameters. **Getting this backwards does not
leave the rotation unapplied — it applies it twice, in the wrong direction.**
Verified numerically: the penalty is exactly `2|θ|·L` (§6).

### 1a. Coordinate-frame / CCW-axes convention — used by NGS

Soler, Weston & Han, eq. (1). NGS states the convention verbatim as:

> "Differential anticlockwise (counterclockwise) rotations (expressed in
> radians) at epoch t₀, respectively, around the x_ITRF, y_ITRF, z_ITRF axes to
> establish parallelism with the x_NAD83, y_NAD83, z_NAD83 axes."

and on the NGS Helmert page simply:

> "Counterclockwise rotation of axes are positive"

```
                                  ┌                    ┐
⎧x⎫         ⎧Tx⎫                  │ 1+s    εz    −εy   │ ⎧x⎫
⎨y⎬       = ⎨Ty⎬              +   │ −εz   1+s     εx   │ ⎨y⎬
⎩z⎭_NAD83   ⎩Tz⎭                  │  εy   −εx    1+s   │ ⎩z⎭_ITRF
                                  └                    ┘
```

### 1b. Position-vector convention — used by IERS / IGN

From the ITRF2020 transformation file header, verbatim:

> ```
> XS :    : X :   : Tx :   :  D   -Rz   Ry : : X :
> YS :  = : Y : + : Ty : + :  Rz   D   -Rx : : Y :
> ZS :    : Z :   : Tz :   : -Ry   Rx   D  : : Z :
>
> Where X,Y,Z are the coordinates in ITRF2020 and XS,YS,ZS are the
> coordinates in the other frames.
> ```

### 1c. The relationship

Matching the two matrices element-by-element:

```
R_IERS  =  −ε_NGS      (componentwise)          D_IERS ≡ s_NGS
```

**Verified numerically** (`max |M_ccw(ε) − M_pv(−ε)| = 0.000e+00` exactly).
Applying the NGS parameters in the IERS matrix without flipping the sign gives a
matrix that differs by `2.597e-07` = `2|εx|` in radians.

Time evolution, both conventions (Soler eq. 1 continued):

```
T(t) = T(t₀) + Ṫ·(t − t₀)
ε(t) = [ε(t₀) + ε̇·(t − t₀)] · m_r          m_r = 4.84813681e-9   (mas → rad)
s(t) = s(t₀) + ṡ·(t − t₀)
```

---

## 2. ITRF2020 → ITRF2014

Source: IGN, `Transfo-ITRF2020_TRFs.txt`. **Position-vector convention** (§1b).
Reference epoch **2015.0**.

| | Tx | Ty | Tz | D | Rx | Ry | Rz |
|---|---|---|---|---|---|---|---|
| units | mm | mm | mm | ppb | mas | mas | mas |
| value | −1.4 | −0.9 | 1.4 | −0.42 | 0.00 | 0.00 | 0.00 |
| rate /yr | 0.0 | −0.1 | 0.2 | 0.00 | 0.00 | 0.00 | 0.00 |

Rotations are identically zero — ITRF2020 and ITRF2014 are aligned in
orientation. **For baseline vectors this transformation is therefore scale-only**
(−0.42 ppb ⇒ 0.0042 mm on a 10 km baseline). It is negligible for vector work and
matters only for absolute positions.

---

## 3. ITRF2014 → NAD83(2011)

Source: NGS MYCS2; independently corroborated by **EPSG:8970** (identical values:
`rx = 0.02678138″ = 26.78138 mas`, etc.). **CCW-axes convention** (§1a).
Reference epoch **2010.0**.

| parameter | value | unit | rate | unit |
|---|---|---|---|---|
| Tx | +1.00530 | m | +0.00079 | m/yr |
| Ty | −1.90921 | m | −0.00060 | m/yr |
| Tz | −0.54157 | m | −0.00144 | m/yr |
| εx | +26.78138 | mas | +0.06667 | mas/yr |
| εy | −0.42027 | mas | −0.75744 | mas/yr |
| εz | +10.93206 | mas | −0.05133 | mas/yr |
| s | +0.36891 | ppb | −0.07201 | ppb/yr |

At t₀: **|ε| = 28.9297 mas = 1.402553e-07 rad ⇒ 0.1403 mm/km** for vectors.

**Cross-validation of the rotation rates.** `ε̇ = (+0.06667, −0.75744, −0.05133)`
mas/yr matches Soler et al.'s independently published 1997.0 ITRF00→NAD83 set
`(0.067, −0.757, −0.051)` to every printed digit. These rates *are* the North
America Euler pole and must be identical across realizations — a strong check
that both extractions are correct.

---

## 4. ITRF00 → NAD83, epoch 1997.0 — historical, for provenance

Soler, Weston & Han (2004), read directly from the paper. Same CCW-axes
convention. Retained because it documents where the modern set descends from and
because the rate agreement in §3 is checked against it.

```
t₀ = 1997.0
Tx = 0.9956 m      Ty = −1.9013 m     Tz = −0.5215 m
εx = 25.915 mas    εy = 9.426 mas     εz = 11.599 mas
s  = 0.62e-9
Ṫ  = (0.0007, −0.0007, 0.0005) m/yr
ε̇  = (0.067, −0.757, −0.051) mas/yr
ṡ  = −0.18e-9 /yr
```

---

## 5. Epoch handling — the parameters are epoch-*matched*, not epoch-*bridging*

The Helmert maps **ITRF at epoch t → NAD83 at the same epoch t**. It does not
move a coordinate between epochs. NGS states this explicitly:

> "using the Helmert parameters by themselves generally will not yield correct
> results if the input and output epochs are not the same, and these parameters
> alone are not guaranteed to correctly transform to NAD83 at epoch 2010.00 if
> the input ITRF2014 coordinates are at any epoch other than 2010.00."

Crossing epochs requires a velocity model (HTDP's crustal motion model, or
station-specific velocities from the SIF / CORS).

The time-dependent rotation is what makes NAD83 co-rotate with the North America
plate, so ε(t) grows substantially over a decade and a half:

| t | εx | εy | εz | \|ε\| | D |
|---|---|---|---|---|---|
| 2010.0 | +26.78138 | −0.42027 | +10.93206 | 28.9297 mas | +0.36891 ppb |
| 2026.0 | +27.84810 | −12.53931 | +10.11078 | 32.1711 mas | −0.78325 ppb |

**To compare against published NAD83(2011) @ 2010.00, evaluate the parameters at
t = 2010.0 and supply ITRF coordinates propagated to 2010.0.** Evaluating at
2026.0 yields NAD83 at 2026.0, which is a different (plate-rotated) coordinate.

---

## 6. Baseline vectors — translations cancel, rotation and scale do not

Soler et al. derive this explicitly as their eq. (3):

> "Notice that because the vector components defined by equation (2) involve the
> subtraction of two sets of coordinates, the shifts and shift rates in equation
> (1) will cancel."

```
⎧Δx⎫          ┌ 1+s    εz   −εy ┐ ⎧Δx⎫
⎨Δy⎬       =  │ −εz   1+s    εx │ ⎨Δy⎬
⎩Δz⎭_NAD83    └  εy   −εx   1+s ┘ ⎩Δz⎭_ITRF
```

Consequences:

- **Translations are irrelevant to vector work.** Bursa-Wolf vs.
  Molodensky-Badekas is therefore moot for vectors — both give the same `(1+s)R`.
- **Length changes only by scale** (rotation is length-preserving), so the
  −0.42 ppb + 0.37 ppb terms are sub-0.01 mm at 10 km.
- **Orientation changes by `|θ| ≈ 28.93 mas`**, i.e. `0.1403 mm/km` as an upper
  bound. Realized magnitude is `|θ × Δx| = |θ||Δx| sin∠`, so it depends on
  baseline azimuth relative to the rotation axis — measured 63–98 % of the bound
  on real Oregon baselines (§7).

### 6a. OPUS applies eq. (3) — it *does* rotate the vector

This is the operationally decisive fact. Soler et al., on the OPUS procedure:

> "Equation (3), with the NAD 83 coordinates of the CORS stations, is used
> internally by NGS software OPUS … to obtain the final NAD 83 coordinates of the
> unknown point P."

OPUS transforms the ITRF vector into NAD83 via eq. (3), **then** adds it to the
NAD83 CORS coordinate via eq. (4). It is *not* the "ITRF vector + NAD83 origin"
hybrid.

Also relevant to determining what frame a given set of stored vectors is in:

> "NGS typically stores processed vectors in its data base in some ITRF frame."

> "…the input G-file which contains all vector components of each session
> referred to a topocentric frame parallel to ITRFxx, located at the origin of
> the vector."

So an **ADJUST G-file carries ITRF-parallel vectors**, while **OPUS output
coordinates are built from rotated vectors**. Whether a particular vector set has
been rotated is therefore *software-dependent and must be read from the file's
frame declaration* (GVX carries a per-vector reference-frame attribute) rather
than assumed.

### 6b. Why OPUS propagates from control instead of Helmert-ing coordinates

> "OPUS could have used program HTDP to convert ITRF00 coordinates into NAD 83
> coordinates, however this approach would have assumed that the adopted 14
> transformation parameters between ITRF00 and NAD 83 fit equally well in all
> areas of the country. This may not be the case due to the effect of small
> localized distortions."

Soler et al. quantify that localized distortion against the FBN adjustment as
**up to 2 cm horizontal and 4 cm vertical**. This is the standing argument for
propagating from nearby control via observed vectors rather than applying a
global similarity transform — and the magnitude to expect when the two disagree.

---

## 7. Worked example — verified

ITRF2020 → NAD83(2011), epoch-matched at **t = 2010.0**, chained
ITRF2020→ITRF2014→NAD83(2011). ITRF2020 coordinates from
`tools/sif_propagate/sif_propagate.py` (NCN SIF, MYCS3).

| station | | X | Y | Z |
|---|---|---|---|---|
| CORV00USA | ITRF2020@2010.0 | −2498423.3257 | −3802819.9379 | 4454736.7360 |
| (Corvallis OR) | NAD83(2011) | −2498422.5142 | −3802821.1365 | 4454736.6934 |
| | shift | +0.8116 | −1.1986 | −0.0425 |
| NEWP00USA | ITRF2020@2010.0 | −2548454.1031 | −3769449.6344 | 4454636.5241 |
| (Newport OR) | NAD83(2011) | −2548453.2898 | −3769450.8303 | 4454636.4773 |
| | shift | +0.8133 | −1.1960 | −0.0468 |
| CHZZ00USA | ITRF2020@2010.0 | −2503336.5035 | −3714407.3769 | 4525454.7919 |
| (Cape Mears OR) | NAD83(2011) | −2503335.6872 | −3714408.5660 | 4525454.7379 |
| | shift | +0.8164 | −1.1892 | −0.0540 |

Absolute shift ≈ **1.44–1.45 m**, dominated by the translation.

**Vector term** — the same three sites as baselines, showing that the metre-level
translation vanishes and only the millimetre-level rotation survives:

| baseline | L (km) | (M−I)·Δx (mm) | mm/km | bound \|θ\|L (mm) | realized/bound | wrong-sign penalty |
|---|---|---|---|---|---|---|
| CORV–NEWP | 60.139 | 5.291 | 0.0880 | 8.435 | 0.627 | 10.581 mm |
| CORV–CHZZ | 113.322 | 15.622 | 0.1379 | 15.894 | 0.983 | 31.243 mm |
| NEWP–CHZZ | 100.402 | 10.395 | 0.1035 | 14.082 | 0.738 | 20.791 mm |

The "wrong-sign penalty" column is the cost of applying NGS parameters in the
IERS matrix without negating: exactly **2×** the rotation term, in the wrong
direction. On CORV–CHZZ that is 31 mm of self-inflicted error on a transformation
whose correct magnitude is 16 mm.

---

## 8. Sources

- IGN / IERS, ITRF2020 transformation parameters —
  `https://itrf.ign.fr/docs/solutions/itrf2020/Transfo-ITRF2020_TRFs.txt`
  (equation and convention quoted verbatim in §1b)
- NGS, Multi-Year CORS Solution 2 (MYCS2) —
  `https://geodesy.noaa.gov/CORS/news/mycs2/mycs2.shtml` (§3)
- EPSG:8970, *ITRF2014 to NAD83(2011) (1)* — `https://epsg.io/8970`
  (independent corroboration of §3)
- NGS, *Helmert Transformation Parameters Used at NGS* —
  `https://www.ngs.noaa.gov/CORS/news/historical_helmert.shtml`
  (convention statement; historical sets)
- Soler, T., N.D. Weston, H. Han, *Computing NAD 83 Coordinates Using
  ITRF-Derived Vector Components*, NGS/NOAA —
  `https://geodesy.noaa.gov/CORS/Articles/FIGSoler.pdf`
  (eqs. 1–4, the OPUS procedure, §4, §6)
- Soler, T. (2001), *Densifying 3D networks by accurate transformation of
  GPS-determined vector components*, GPS Solutions 4(3): 27–33 — the rigorous
  vector + covariance transformation at differing epochs (not reproduced here)

**Not used:** HTDP source (`htdp.f`) was not retrieved; the ITRF2014→NAD83(2011)
set in §3 comes from MYCS2 + EPSG and is the same set HTDP implements. If a
future need requires frames beyond those tabulated here, the HTDP source is the
authority.

## 9. Verification

Numbers in §1c, §5, §6, §7 were produced by a standalone verification script that
implements both conventions independently and checks them against each other.
Checks performed:

1. `M_ccw_axes(ε) == M_pos_vector(−ε)` → max difference **0.000e+00**
2. Same-sign (deliberately wrong) → differs by **2|εx|**, confirming the
   failure mode is a doubling, not an omission
3. Rotation-rate agreement between the MYCS2 set and Soler's 1997.0 set
4. `(M−I)·Δx` against the analytic bound `|θ|·L` on three real baselines

The script is not currently in the repo. Promote it to `tools/` if these
transformations become a recurring need.
