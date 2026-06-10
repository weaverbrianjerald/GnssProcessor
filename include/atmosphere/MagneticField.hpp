#pragma once

/**
 * @file MagneticField.hpp
 * @brief Centered tilted dipole model of the Earth's geomagnetic field.
 *
 * Provides @c B(r) evaluated at an arbitrary ECEF position.  Used by
 * @c HigherOrderIonosphere to compute the @c B·cos(θ) factor in the
 * second-order ionospheric delay term.  The dipole approximation is
 * accurate to ~5–10% in magnitude and ~10° in direction at typical
 * ionospheric pierce-point altitudes (300–500 km) versus a full IGRF
 * spherical-harmonic synthesis — adequate for sub-mm error on the I₂
 * correction itself, since I₂ peaks around 1–2 cm at GPS L1 during
 * high TEC and the dipole error projects through linearly.
 *
 * No external data file is required; the dipole moment and pole
 * position are baked-in constants at a fixed reference epoch
 * (IGRF-13 2020.0 dipole term).  Replace with a full IGRF synthesis
 * later if mm-level higher-order accuracy is ever needed.
 */

#include <Eigen/Dense>

namespace GnssProcessor {

    /**
     * @class MagneticField
     * @brief Closed-form geomagnetic field evaluation via centered tilted dipole.
     *
     * Stateless; all methods are static.  No dependency on the LogManager
     * or the configuration — the dipole is a physics constant.
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
         * geographic location of the geomagnetic NORTH pole (where
         * compass N-tips point — magnetically the south pole of the
         * dipole), @c r̂ is the unit position vector, @c R_E is a
         * reference Earth radius, and @c B0 is the surface field
         * magnitude at the magnetic equator.
         *
         * Constants are baked in to the IGRF-13 2020.0 dipole term:
         *   geomagnetic north pole: 80.65°N, 287.32°E (geographic)
         *   B0 at magnetic equator (surface): 30,000 nT
         *   reference Earth radius: 6371.2 km
         *
         * @param pos_ecef Position at which to evaluate the field (m).
         * @return Magnetic field vector in ECEF, units of Tesla.
         *         Returns a zero vector if @c pos_ecef is at the origin
         *         (degenerate).
         */
        static Eigen::Vector3d fieldAtEcef(const Eigen::Vector3d& pos_ecef);

        /**
         * @brief Unit vector toward geomagnetic north pole in ECEF.
         *
         * Exposed for testing and for callers that want to compute the
         * dot product @c m̂·k̂ themselves (e.g. for diagnostics).
         */
        static Eigen::Vector3d dipoleAxisEcef();
    };

} // namespace GnssProcessor
