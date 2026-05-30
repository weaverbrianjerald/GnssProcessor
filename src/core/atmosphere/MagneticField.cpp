#include "atmosphere/MagneticField.hpp"

#include "common/Constants.hpp"

#include <cmath>

namespace GnssProcessor {

    namespace {
        // IGRF-13 2020.0 centered dipole position (geographic coordinates of
        // the geomagnetic north pole).  The dipole moves ~50 km/yr; for the
        // post-2020 era this is accurate to a few tenths of a degree, which
        // translates to <1% error on |B·cos θ| at typical IPP altitudes.
        constexpr double kPoleLatDeg = 80.65;
        constexpr double kPoleLonDeg = -72.68;  // = 287.32°E expressed as -72.68°E

        // Surface field magnitude at the magnetic equator (Tesla).  The
        // canonical IGRF mean-radius value of 30,000 nT.
        constexpr double kB0Tesla = 30.0e-6;

        // IGRF reference Earth radius (km → m).
        constexpr double kRefEarthRadiusM = 6371.2e3;
    } // namespace

    Eigen::Vector3d MagneticField::dipoleAxisEcef() {
        const double lat = kPoleLatDeg * Constants::PI / 180.0;
        const double lon = kPoleLonDeg * Constants::PI / 180.0;
        return Eigen::Vector3d(std::cos(lat) * std::cos(lon),
                               std::cos(lat) * std::sin(lon),
                               std::sin(lat));
    }

    Eigen::Vector3d MagneticField::fieldAtEcef(const Eigen::Vector3d& pos_ecef) {
        const double r = pos_ecef.norm();
        if (r < 1.0) return Eigen::Vector3d::Zero();

        const Eigen::Vector3d r_hat = pos_ecef / r;
        // m_hat points to the geographic location of the geomagnetic NORTH
        // pole (Arctic), which is magnetically the south pole of the dipole.
        // With that convention the field is
        //   B = (B0·(R_E/r)^3) · ( m̂ − 3·(m̂·r̂)·r̂ )
        // so that at the magnetic equator B points horizontally toward the
        // magnetic north pole — matching observed compass behaviour.
        const Eigen::Vector3d m_hat = dipoleAxisEcef();
        const double dot = m_hat.dot(r_hat);

        const double scale = kB0Tesla * std::pow(kRefEarthRadiusM / r, 3);
        return scale * (m_hat - 3.0 * dot * r_hat);
    }

} // namespace GnssProcessor
