#include "atmosphere/HigherOrderIonosphere.hpp"

#include "atmosphere/MagneticField.hpp"
#include "common/Constants.hpp"

#include <cmath>

namespace GnssProcessor {

    namespace {
        // Second-order group-delay coefficient (Petrie et al. 2010, Eq. 2;
        // Hernández-Pajares et al. 2007):
        //   I_2^code = (7527 · c / 2) · STEC[el/m²] · B[T] · cos(θ_B) / f^3[Hz]
        // with the result in metres.
        constexpr double kI2CodeCoeff = 7527.0 * Constants::SPEED_OF_LIGHT / 2.0;

        // 1 TECU in electrons per square metre.
        constexpr double kTecuToElectrons = 1.0e16;

        // Third-order group-delay shape factor (Hoque & Jakowski 2008):
        //   I_3^code = (2437 · η) · STEC²[el/m²]² / f^4[Hz]
        // with η ≈ 0.66 a fixed factor capturing the integrated profile
        // of N_e² along the ray (independent of solar activity to within
        // a few tens of percent; sub-mm contribution at GPS L1 in any
        // case, so the simplification is benign).
        constexpr double kI3CodeBase = 2437.0;
        constexpr double kI3ShapeEta = 0.66;
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

        // Code (group delay): both I₂ and I₃ positive, additive to modeled.
        const double i2_code = kI2CodeCoeff * stec_el * B_dot_k / f3;
        const double i3_code = kI3CodeBase * kI3ShapeEta * stec_el * stec_el / f4;

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
