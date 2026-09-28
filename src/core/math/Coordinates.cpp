#include "common/Constants.hpp"
#include "math/Coordinates.hpp"
#include <cmath>
#include <limits>

namespace GnssProcessor {
    namespace Coordinates {

        Llh ecefToLlh(const Eigen::Vector3d& ecef, double a, double f) {
            // Derived constants
            const double e2 = f * (2.0 - f);

            const double x = ecef.x();
            const double y = ecef.y();
            const double z = ecef.z();

            Llh out;
            const double p = std::sqrt(x * x + y * y);
            out.lon_rad = std::atan2(y, x);

            if (p < 1e-12) {
                // On the polar axis: latitude is ±π/2 and height follows directly;
                // longitude keeps the atan2(y, x) value computed above
                out.lat_rad = (z >= 0.0) ? Constants::PI / 2.0 : -Constants::PI / 2.0;
                const double sin_lat = std::sin(out.lat_rad);
                const double N = a / std::sqrt(1.0 - e2 * sin_lat * sin_lat);
                out.h_m = std::abs(z) - N * (1.0 - e2);
                return out;
            }

            // Initial guess (Bowring)
            double lat = std::atan2(z, p * (1.0 - e2));
            double h = 0.0;

            // Iterate to refine latitude/height (converges quickly)
            for (int i = 0; i < 10; ++i) {
                const double sin_lat = std::sin(lat);
                const double N = a / std::sqrt(1.0 - e2 * sin_lat * sin_lat);
                h = p / std::cos(lat) - N;
                const double denom = p * (1.0 - e2 * (N / (N + h)));
                if (std::abs(denom) < 1e-15) break;
                const double lat_next = std::atan2(z, denom);
                if (std::abs(lat_next - lat) < 1e-13) {
                    lat = lat_next;
                    break;
                }
                lat = lat_next;
            }

            out.lat_rad = lat;
            out.h_m = h;
            return out;
        }

        Eigen::Vector3d llhToEcef(double lat_rad, double lon_rad, double h_m, double a, double f) {
            const double e2 = f * (2.0 - f);
            const double sin_lat = std::sin(lat_rad);
            const double cos_lat = std::cos(lat_rad);
            const double cos_lon = std::cos(lon_rad);
            const double sin_lon = std::sin(lon_rad);

            const double N = a / std::sqrt(1.0 - e2 * sin_lat * sin_lat);

            double x = (N + h_m) * cos_lat * cos_lon;
            double y = (N + h_m) * cos_lat * sin_lon;
            double z = ((1.0 - e2) * N + h_m) * sin_lat;

            return Eigen::Vector3d(x, y, z);
        }

        Eigen::Matrix3d enuToEcefMatrix(double lat_rad, double lon_rad) {
            double slat = std::sin(lat_rad), clat = std::cos(lat_rad);
            double slon = std::sin(lon_rad), clon = std::cos(lon_rad);

            Eigen::Matrix3d R;
            // Columns are E, N, U unit vectors in ECEF
            R.col(0) = Eigen::Vector3d(-slon, clon, 0.0);                    // East
            R.col(1) = Eigen::Vector3d(-slat * clon, -slat * slon, clat);    // North
            R.col(2) = Eigen::Vector3d(clat * clon, clat * slon, slat);      // Up

            return R;
        }

        Eigen::Vector3d ecefToEnu(const Eigen::Vector3d& vec_ecef, const Llh& ref_llh) {
            Eigen::Matrix3d R = enuToEcefMatrix(ref_llh.lat_rad, ref_llh.lon_rad);
            // R maps ENU -> ECEF, so R.transpose() maps an ECEF vector to ENU
            return R.transpose() * vec_ecef;
        }

        Eigen::Vector3d enuToEcef(const Eigen::Vector3d& enu, const Llh& ref_llh) {
            Eigen::Matrix3d R = enuToEcefMatrix(ref_llh.lat_rad, ref_llh.lon_rad);
            return R * enu;
        }

        Eigen::Vector3d markerToArp(const Eigen::Vector3d& marker_ecef,
                                    const Eigen::Vector3d& delta_hen) {
            // Exact no-op on a zero eccentricity: skip the LLH iteration so a
            // header-less stream (or a 0/0/0 station) is bit-for-bit unchanged.
            if (!(delta_hen.norm() > 1e-6) || !marker_ecef.allFinite()) return marker_ecef;
            // RINEX (H, E, N) = (Up, East, North) -> ENU order (E, N, U).
            const Eigen::Vector3d enu(delta_hen(1), delta_hen(2), delta_hen(0));
            return marker_ecef + enuToEcef(enu, ecefToLlh(marker_ecef));
        }

        Eigen::Vector3d arpToMarker(const Eigen::Vector3d& arp_ecef,
                                    const Eigen::Vector3d& delta_hen) {
            if (!(delta_hen.norm() > 1e-6) || !arp_ecef.allFinite()) return arp_ecef;
            const Eigen::Vector3d enu(delta_hen(1), delta_hen(2), delta_hen(0));
            // The rotation is defined at the marker, which is the unknown, so one
            // fixed-point step is taken: rotate at the ARP for a first guess, then
            // rotate again at that guess.  The first guess is off by O(|ecc|²/R),
            // driven by the horizontal components: zero for a height-only
            // eccentricity (geodetic latitude and longitude do not change along
            // the ellipsoid normal), about 8 µm for a 6.2 m horizontal offset.
            // One iteration brings the closure below a picometre.
            const Eigen::Vector3d guess = arp_ecef - enuToEcef(enu, ecefToLlh(arp_ecef));
            return arp_ecef - enuToEcef(enu, ecefToLlh(guess));
        }

    } // namespace Coordinates
} // namespace GnssProcessor