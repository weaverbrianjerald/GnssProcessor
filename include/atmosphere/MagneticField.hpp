#pragma once

/**
 * @file MagneticField.hpp
 * @brief Centered tilted dipole model of the Earth's geomagnetic field.
 *
 * Provides @c B(r) evaluated at an arbitrary ECEF position.  Used by
 * @c HigherOrderIonosphere to compute the @c B·cos(θ) factor in the
 * second-order ionospheric delay term.  Against a full IGRF
 * spherical-harmonic synthesis, the dipole approximation is accurate to
 * about 5–10 % in magnitude and about 10° in direction at typical
 * ionospheric pierce-point altitudes (300–500 km).  The dipole error
 * propagates linearly into I₂, which peaks around 1–2 cm at GPS L1 under
 * high TEC, so the resulting error on the I₂ correction is at most about
 * 1–2 mm.
 *
 * No external data file is required; the dipole moment and pole
 * position are fixed constants at a reference epoch (IGRF-13 2020.0
 * dipole term).  Mm-level higher-order accuracy would require a full
 * IGRF synthesis in place of this model.
 */

#include <Eigen/Dense>

namespace GnssProcessor {

    /**
     * @class MagneticField
     * @brief Closed-form geomagnetic field evaluation via centered tilted dipole.
     *
     * Stateless; all methods are static.  No dependency on the LogManager
     * or the configuration, since the dipole parameters are fixed constants.
     */
    class MagneticField {
    public:
        /**
         * @brief Evaluate the geomagnetic field at an ECEF position.
         *
         * Uses the standard centered dipole formula
         *
         *   @c B(r) = (B0 · (R_E/r)^3) · [ m̂ − 3·(m̂·r̂)·r̂ ]
         *
         * where @c m̂ is the unit vector from the geocentre toward the
         * geographic location of the geomagnetic north pole (where
         * compass north tips point; magnetically the south pole of the
         * dipole), @c r̂ is the unit position vector, @c R_E is a
         * reference Earth radius, and @c B0 is the surface field
         * magnitude at the magnetic equator.
         *
         * Constants are fixed to the IGRF-13 2020.0 dipole term:
         *   geomagnetic north pole: 80.65°N, 287.32°E (geographic)
         *   B0 at magnetic equator (surface): 30,000 nT
         *   reference Earth radius: 6371.2 km
         *
         * @param pos_ecef Position at which to evaluate the field (m).
         * @return Magnetic field vector in ECEF, units of Tesla.
         *         Returns a zero vector if @c pos_ecef is within 1 m of the
         *         origin (degenerate).
         */
        static Eigen::Vector3d fieldAtEcef(const Eigen::Vector3d& pos_ecef);

        /**
         * @brief Unit vector toward the geomagnetic north pole in ECEF.
         *
         * Exposed for testing and for callers that compute the dot
         * product @c m̂·k̂ themselves (for example, for diagnostics).
         */
        static Eigen::Vector3d dipoleAxisEcef();
    };

} // namespace GnssProcessor
