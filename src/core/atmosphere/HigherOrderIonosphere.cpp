#include "atmosphere/HigherOrderIonosphere.hpp"

#include "atmosphere/MagneticField.hpp"
#include "common/Constants.hpp"

#include <cmath>

namespace GnssProcessor {

    namespace {
        // Second-order GROUP-delay (code) coefficient.  Derived from the
        // quasi-longitudinal Appleton-Hartree phase index
        //   n_ph = 1 − X/2 − (1/2)·X·Y_L − …,   X = A·N_e/f²,  Y_L = (e·B·cosθ)/(2π·m_e·f)
        // whose 2nd-order phase delay is −(A·e/(4π·m_e·f³))·B·cosθ·STEC.
        // The group delay is (1−k)=−2× the phase term (k=3, the f⁻³ power),
        // giving the code coefficient
        //   I_2^code = (A·e/(2π·m_e)) · STEC[el/m²] · B[T]·cosθ / f³[Hz]
        //            = (7527 · c) · STEC · B·cosθ / f³           [metres].
        // NOTE: it is 7527·c for the code (group) delay, NOT 7527·c/2 — the
        // latter is the *phase* coefficient (the phase term is −½ of code, see
        // slantHigherOrderComponents).  Matches POINT Iono_Second_Order
        // (const1 = e·A/(2π·m_e·f³), no factor of ½) and Hernández-Pajares
        // et al. 2007 / Petrie et al. 2010.  e·A/(2π·m_e) = 2.2564e12 ≈ 7527·c.
        constexpr double kI2CodeCoeff = 7527.0 * Constants::SPEED_OF_LIGHT;

        // 1 TECU in electrons per square metre.
        constexpr double kTecuToElectrons = 1.0e16;

        // Third-order GROUP-delay (code) coefficient (Hoque & Jakowski 2008;
        // POINT Iono_Third_Order):
        //   I_3^code = (2437 · η) · ∫N_e² dl / f⁴ ,   ∫N_e² dl = η·N_max·STEC
        // i.e. the third-order term depends on ∫N_e² dl (units el²·m⁻⁵), which
        // the shape factor η≈0.66 relates to N_max·STEC — it is NOT proportional
        // to STEC².  N_max (the F2-layer peak electron density, el/m³) is taken
        // from the Hoque-Jakowski linear fit N_max = (14e12 / 3.17e18)·STEC
        // (the two anchor points TEC=1.38e18↔6e12 and TEC=4.55e18↔20e12).
        // 2437 = 3·A²/8.  Sub-mm at GPS L1; the STEC-vs-VTEC choice inside the
        // N_max fit is negligible at that level, so slant STEC is used to match
        // the POINT reference exactly.
        constexpr double kI3CodeBase  = 2437.0;
        constexpr double kI3ShapeEta  = 0.66;
        constexpr double kNmaxPerStec = (20.0 - 6.0) * 1.0e12 / ((4.55 - 1.38) * 1.0e18);
    } // namespace

    HigherOrderIonosphere::Components
    HigherOrderIonosphere::slantHigherOrderComponents(
            const Eigen::Vector3d& rx_pos_ecef,
            const Eigen::Vector3d& sat_pos_ecef,
            double ipp_lat_rad,
            double ipp_lon_rad,
            double ipp_radius_m,
            double stec_tecu,
            double frequency_hz,
            char meas_type) const {
        Components out;

        if (frequency_hz <= 0.0 || !std::isfinite(stec_tecu)) return out;
        if (meas_type != 'C' && meas_type != 'L') return out;
        if (ipp_radius_m <= 0.0 || !std::isfinite(ipp_radius_m)) return out;

        // Reconstruct the IPP ECEF position from the geocentric IPP
        // coordinates returned by the GIM.  IONEX is a thin-shell
        // spherical model, so geocentric → ECEF is just spherical →
        // Cartesian at the IONEX base+shell radius.
        const double cos_lat = std::cos(ipp_lat_rad);
        const Eigen::Vector3d ipp_ecef(
            ipp_radius_m * cos_lat * std::cos(ipp_lon_rad),
            ipp_radius_m * cos_lat * std::sin(ipp_lon_rad),
            ipp_radius_m * std::sin(ipp_lat_rad));

        const Eigen::Vector3d B = MagneticField::fieldAtEcef(ipp_ecef);  // Tesla

        // Signal direction k̂: from receiver to satellite, unit vector.
        const Eigen::Vector3d los = sat_pos_ecef - rx_pos_ecef;
        const double los_norm = los.norm();
        if (los_norm < 1.0) return out;
        const Eigen::Vector3d k_hat = los / los_norm;

        const double B_dot_k = B.dot(k_hat);              // Tesla
        const double stec_el = stec_tecu * kTecuToElectrons;  // el/m²

        const double f2 = frequency_hz * frequency_hz;
        const double f3 = f2 * frequency_hz;
        const double f4 = f2 * f2;

        // Code (group delay): I₂ signed by B·cosθ; I₃ positive; additive to modeled.
        const double ne_max  = kNmaxPerStec * stec_el;        // F2-peak density, el/m³
        const double i2_code = kI2CodeCoeff * stec_el * B_dot_k / f3;
        const double i3_code = kI3CodeBase * kI3ShapeEta * ne_max * stec_el / f4;

        if (meas_type == 'C') {
            out.i2_m = i2_code;
            out.i3_m = i3_code;
        } else {
            // Phase delay (advance): opposite sign with the standard
            // factor-of-2 and factor-of-3 scalings from the Appleton-
            // Hartree expansion (group vs phase velocity at each order).
            out.i2_m = -0.5 * i2_code;
            out.i3_m = -(1.0 / 3.0) * i3_code;
        }
        out.valid = std::isfinite(out.i2_m) && std::isfinite(out.i3_m);
        return out;
    }

    double HigherOrderIonosphere::slantHigherOrderDelay(
            const Eigen::Vector3d& rx_pos_ecef,
            const Eigen::Vector3d& sat_pos_ecef,
            double ipp_lat_rad,
            double ipp_lon_rad,
            double ipp_radius_m,
            double stec_tecu,
            double frequency_hz,
            char meas_type) const {
        const Components c = slantHigherOrderComponents(
            rx_pos_ecef, sat_pos_ecef, ipp_lat_rad, ipp_lon_rad,
            ipp_radius_m, stec_tecu, frequency_hz, meas_type);
        return c.valid ? (c.i2_m + c.i3_m) : 0.0;
    }

} // namespace GnssProcessor
