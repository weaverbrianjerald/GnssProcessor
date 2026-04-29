#pragma once

/**
 * @file GnssTypes.hpp
 * @brief Constellation and measurement-type enums shared across the codebase.
 *
 * The two enums here are the most basic categorical types in the project:
 * @c System identifies a GNSS constellation (GPS / Galileo / GLONASS / etc.),
 * and @c MeasurementType classifies a row by observable category (code,
 * phase, Doppler, SNR, inertial).  These are used throughout RINEX
 * parsing, the measurement model, the Kalman filter, and trace logging.
 *
 * Kept dependency-free (no Eigen, no LogManager) so any header in any
 * subdirectory can include this without dragging in heavier transitive
 * deps.  The inline @c classifyMeasurement helper dispatches on the
 * leading character of a RINEX-style observable key.
 */

#include <string>

namespace GnssProcessor {

    /**
     * @brief Enumeration of supported GNSS constellations/systems.
     */
    enum class System {
        GPS,      /**< Global Positioning System (USA) */
        Galileo,  /**< Galileo (EU) */
        GLONASS,  /**< GLONASS (Russia) */
        BeiDou,   /**< BeiDou (China) */
        QZSS,     /**< QZSS (Japan) */
        SBAS,     /**< Satellite-Based Augmentation Systems (regional) */
        Unknown   /**< Unknown or unsupported system */
    };

    /**
     * @brief Enumeration of GNSS and auxiliary measurement types.
     *
     * Used to classify measurement rows so that the measurement model,
     * Jacobian builder and noise assignment can dispatch on the observable
     * category rather than parsing the first character of RINEX codes.
     */
    enum class MeasurementType {
        Code,       /**< Pseudorange (code) measurement (RINEX 'C' observables). */
        Phase,      /**< Carrier-phase measurement (RINEX 'L' observables). */
        Doppler,    /**< Doppler-shift measurement (RINEX 'D' observables, units: Hz). */
        SNR,        /**< Signal-to-noise ratio (RINEX 'S' observables). */
        Inertial,   /**< Inertial measurement (e.g. accelerometer / gyroscope). */
        Unknown     /**< Unrecognised or unsupported measurement type. */
    };

    /**
     * @brief Classify a RINEX-style observable key into a MeasurementType.
     *
     * The classification is based on the first character of @p key:
     *   'C' -> Code, 'L' -> Phase, 'D' -> Doppler, 'S' -> SNR.
     *
     * @param key Observable key (e.g. "C1W", "L1C", "D1C", "S1C").
     * @return Corresponding MeasurementType, or MeasurementType::Unknown.
     */
    inline MeasurementType classifyMeasurement(const std::string& key) {
        if (key.empty()) return MeasurementType::Unknown;
        switch (key[0]) {
        case 'C': return MeasurementType::Code;
        case 'L': return MeasurementType::Phase;
        case 'D': return MeasurementType::Doppler;
        case 'S': return MeasurementType::SNR;
        default:  return MeasurementType::Unknown;
        }
    }

} // namespace GnssProcessor
