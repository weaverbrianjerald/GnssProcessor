#pragma once

/**
 * @file GnssTypes.hpp
 * @brief Constellation, measurement-type and drop-reason enums shared across
 *        the codebase.
 *
 * @c System identifies a GNSS constellation (GPS / Galileo / GLONASS / etc.),
 * @c MeasurementType classifies a row by observable category (code, phase,
 * Doppler, SNR, inertial), and @c DropReason records why a satellite was
 * dropped before the Kalman update.  The first two are used throughout RINEX
 * parsing, the measurement model, the Kalman filter and trace logging.
 *
 * The header has no dependencies beyond @c <string> (no Eigen, no LogManager),
 * so any header in any subdirectory can include it without heavier transitive
 * dependencies.  The inline @c classifyMeasurement helper dispatches on the
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
     * Classifies measurement rows so that the measurement model, Jacobian
     * builder and noise assignment dispatch on the observable category
     * instead of parsing the first character of RINEX codes.
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
     * @brief Reason a satellite was dropped before reaching the Kalman update.
     *
     * Produced by @c Model::Geometry::computeSatelliteGeometry
     * (model/ModelGeometry.hpp) and aggregated
     * per-satellite by @c PppModel::computeModels; consumed by the
     * @c satellite_status_trace logger so the per-epoch strip chart can
     * separate the failure modes.
     *
     * Ordering: @c None is 0, so a default-constructed reason means "no
     * failure yet".  The other values are ranked roughly by the effort needed
     * to remove the cause:
     *   @c BelowMask  -> change a config knob;
     *   @c NoProduct  -> improve the product chain;
     *   @c NoModel    -> tighten or loosen ATX/BIA/signal-selection rules;
     *   @c Eclipsing  -> unavoidable while eclipse yaw attitude is not modelled.
     */
    enum class DropReason {
        None = 0,    /**< Satellite passed all geometry / model checks. */
        BelowMask,   /**< Elevation below @c processing.elevation_mask_deg. */
        NoProduct,   /**< Missing or invalid orbit / clock / ephemeris at emission. */
        NoModel,     /**< Signal selection or correction failure (no usable
                          code/phase pair, no BIA / ATX coverage, unknown
                          system, configured exclusion list, etc.). */
        Eclipsing    /**< Satellite is in Earth's umbral shadow.  The yaw
                          attitude during eclipse and post-eclipse recovery
                          is block-specific and not modelled here, so the
                          antenna PCO/PCV and phase windup become unreliable.
                          Gated by @c processing.exclude_eclipsing_satellites. */
    };

    /**
     * @brief Human-readable label for a @c DropReason value.
     * @param r Drop reason.
     * @return Stable short label suitable for CSV logging.
     */
    inline const char* dropReasonLabel(DropReason r) {
        switch (r) {
            case DropReason::None:      return "None";
            case DropReason::BelowMask: return "BelowMask";
            case DropReason::NoProduct: return "NoProduct";
            case DropReason::NoModel:   return "NoModel";
            case DropReason::Eclipsing: return "Eclipsing";
        }
        return "None";
    }

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
