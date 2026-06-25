#!/usr/bin/env python3
"""Independent reference for the IERS 2010 (Sec. 7.1.5 / Desai 2002) ocean
pole tide site displacement.

This is an *independent* Python reimplementation of the same closed-form
expression that GnssCorrections::computeOceanPoleTideDisplacement evaluates in
C++, used as an external oracle for the OceanPoleTideTest unit test (mirrors the
role of scripts/solid_tide_reference.py and scripts/nmf_reference.py).

Formula (IERS Conventions 2010, eqs. 7.29 with Desai 2002 coefficients; matches
NGS M-PAGES Lib/Station/PoleTide.cpp::computeOceanPoleTide):

    Hp    = sqrt(8*pi/15) * Omega^2 * a^4 / GM                     [m]
    K     = 4*pi*G*a*rho_sw*Hp / (3*g_E)                           [m]
    Kappa = K * AS2RAD          (so m1,m2 may be supplied in arcsec)

    cR = m1*gammaR + m2*gammaI
    cI = m2*gammaR - m1*gammaI

    dE = Kappa * (cR*ueR + cI*ueI)
    dN = Kappa * (cR*unR + cI*unI)
    dU = Kappa * (cR*urR + cI*urI)

Constants are the verbatim M-PAGES values (grs80::OMEGA, iers::A_EARTH,
iers::GM_EARTH, iers::G, iers::G_E) so the C++ port reproduces this to ~1e-12.
"""
import math

# --- physical constants (verbatim M-PAGES values) ---
OMEGA   = 7.292115e-5        # grs80::OMEGA   [rad/s]
A_EARTH = 6378136.6          # iers::A_EARTH  [m]
GM      = 3.986004418e14     # iers::GM_EARTH [m^3/s^2]
G_NEWT  = 6.67428e-11        # iers::G        [m^3/kg/s^2]
G_E     = 9.7803278          # iers::G_E      [m/s^2]  mean equatorial gravity
RHO_SW  = 1025.0             # sea-water density [kg/m^3]
GAMMA_R = 0.6870             # Desai gamma_2^R
GAMMA_I = 0.0036             # Desai gamma_2^I
AS2RAD  = math.pi / 648000.0 # arcsec -> rad


def kappa():
    Hp = math.sqrt(8.0 * math.pi / 15.0) * OMEGA**2 * A_EARTH**4 / GM
    K = 4.0 * math.pi * G_NEWT * A_EARTH * RHO_SW * Hp / (3.0 * G_E)
    return K * AS2RAD, Hp, K


def ocean_pole_tide(m1_arcsec, m2_arcsec, coeffs):
    """coeffs ordering = [nR, nI, eR, eI, uR, uI]  (M-PAGES optCoeffs order).
    Returns (dE, dN, dU) in metres."""
    nR, nI, eR, eI, uR, uI = coeffs
    Kap, _, _ = kappa()
    cR = m1_arcsec * GAMMA_R + m2_arcsec * GAMMA_I
    cI = m2_arcsec * GAMMA_R - m1_arcsec * GAMMA_I
    dE = Kap * (cR * eR + cI * eI)
    dN = Kap * (cR * nR + cI * nI)
    dU = Kap * (cR * uR + cI * uI)
    return dE, dN, dU


if __name__ == "__main__":
    Kap, Hp, K = kappa()
    print(f"Hp    = {Hp:.6f} m")
    print(f"K     = {K:.6f} m")
    print(f"Kappa = {Kap:.12e} m/arcsec")

    # Reference test vector: grid point (lon=0.25, lat=-89.75), first data row
    # of opoleloadcoefcmcor.txt, with file columns u_r^R u_r^I u_n^R u_n^I u_e^R u_e^I:
    #   0.022015 0.040741 -0.021861 -0.029058 -0.025845 -0.055643
    # reordered to optCoeffs [nR, nI, eR, eI, uR, uI]:
    coeffs = [-0.021861, -0.029058, -0.025845, -0.055643, 0.022015, 0.040741]
    m1, m2 = 0.1, 0.2  # arcsec
    dE, dN, dU = ocean_pole_tide(m1, m2, coeffs)
    print(f"\nm1={m1} m2={m2} arcsec, grid-point coeffs:")
    print(f"  dE = {dE:.15e} m")
    print(f"  dN = {dN:.15e} m")
    print(f"  dU = {dU:.15e} m")
