#pragma once

/**
 * @file SignalBand.hpp
 * @brief RINEX-3 signal classification helpers for pairing observations across receivers.
 *
 * The DD baseline path pairs observations across two receivers whose tracking
 * attributes may differ (for example, the rover reports @c C2W and the reference
 * reports @c C2C: both are GPS L2 code, but the third RINEX-code character differs).
 * The default exact-string intersection drops those rows without a message.
 *
 * This module classifies a RINEX-3 observation code into:
 *  - a @c FrequencyBand (the physical band shared across attributes), and
 *  - an @c ObsType (code, phase, Doppler, SNR), and
 *  - the tracking-attribute character (third position in the RINEX code).
 *
 * It also exposes a deterministic per-band priority order over tracking
 * attributes so a receiver can pick a single canonical code for each
 * (sat, band) pair.
 *
 * The classifier is independent of @c PppUtils, which is stateful and
 * per-instance for GLONASS slot lookup.  Band identity is a property of the RINEX
 * specification alone, so it is provided as stateless free functions behind a
 * header that any module can include.
 */

#include <string>

namespace GnssProcessor {

    /**
     * @brief Identifier for a physical GNSS frequency band.
     *
     * The values follow common RINEX-3 / IGS naming.  Two RINEX codes
     * map to the same @c FrequencyBand iff they observe the same
     * carrier (modulo tracking attribute).
     *
     * New bands are added here as constellations and signals come online; the
     * @c classifySignal() dispatch covers every system and RINEX frequency digit.
     */
    enum class FrequencyBand {
        Unknown = 0,

        // GPS
        GpsL1,    ///< 1575.42 MHz: codes C1C, C1W, C1P, C1S, C1L, C1X (and matching L1*).
        GpsL2,    ///< 1227.60 MHz: codes C2C, C2W, C2P, C2L, C2S, C2X (and matching L2*).
        GpsL5,    ///< 1176.45 MHz: codes C5I, C5Q, C5X (and matching L5*).

        // Galileo
        GalE1,    ///< 1575.42 MHz: codes C1A, C1B, C1C, C1X, C1Z (and matching L1*).
        GalE5a,   ///< 1176.45 MHz: codes C5I, C5Q, C5X (and matching L5*).
        GalE5b,   ///< 1207.14 MHz: codes C7I, C7Q, C7X (and matching L7*).
        GalE5,    ///< 1191.795 MHz wideband: codes C8I, C8Q, C8X.
        GalE6,    ///< 1278.75 MHz: codes C6A, C6B, C6C, C6X, C6Z.

        // GLONASS (FDMA and CDMA bands)
        GloG1,    ///< G1, FDMA: codes C1C, C1P (and matching L1*).
        GloG2,    ///< G2, FDMA: codes C2C, C2P (and matching L2*).
        GloG3,    ///< G3, CDMA: codes C3I, C3Q, C3X.
        GloG1a,   ///< G1a, CDMA: codes C4A, C4B, C4X.
        GloG2a,   ///< G2a, CDMA: codes C6A, C6B, C6X.

        // BeiDou
        BdsB1I,   ///< 1561.098 MHz: codes C2I, C2Q, C2X (also C1I, C1Q).
        BdsB1C,   ///< 1575.42 MHz: codes C1D, C1P, C1X.
        BdsB2I,   ///< 1207.14 MHz B2I (legacy): codes C7I, C7Q, C7X.
        BdsB2a,   ///< 1176.45 MHz: codes C5D, C5P, C5X.
        BdsB2b,   ///< 1207.14 MHz B2b (BDS-3): codes C7D, C7P, C7Z.
        BdsB2ab,  ///< 1191.795 MHz wideband: codes C8D, C8P, C8X.
        BdsB3,    ///< 1268.52 MHz: codes C6I, C6Q, C6X.

        // QZSS
        QzsL1,    ///< codes C1C, C1S, C1L, C1X, C1Z.
        QzsL2,    ///< codes C2L, C2S, C2X.
        QzsL5,    ///< codes C5I, C5Q, C5X.
        QzsL6,    ///< codes C6L, C6S, C6X, C6Z.

        // SBAS
        SbsL1,    ///< codes C1C.
        SbsL5,    ///< codes C5I, C5Q, C5X.

        // IRNSS / NavIC
        IrnL5,    ///< codes C5A, C5B, C5C, C5X.
        IrnS,     ///< codes C9A, C9B, C9C, C9X.
    };

    /**
     * @brief Type of a RINEX observation, taken from the first character.
     */
    enum class ObsType : char {
        Unknown = 0,
        Code    = 'C',  ///< Code/pseudorange (m).
        Phase   = 'L',  ///< Carrier phase (cycles).
        Doppler = 'D',  ///< Doppler (Hz).
        Snr     = 'S',  ///< Signal-to-noise ratio.
    };

    /**
     * @brief Result of classifying a (constellation, RINEX-3-code) pair.
     */
    struct SignalClassification {
        FrequencyBand band{ FrequencyBand::Unknown };  ///< Physical band, or @c Unknown if unrecognized.
        ObsType       type{ ObsType::Unknown };        ///< Observation type taken from the first code character.
        char          attribute{ '\0' };               ///< Tracking attribute (3rd char of code, e.g. 'W' in C2W).
    };

    /**
     * @brief Classify a RINEX-3 3-character observation code.
     *
     * @param constellation Constellation name as used elsewhere in the codebase
     *                      ("GPS","Galileo","GLONASS","BeiDou","QZSS","SBAS","IRNSS").
     *                      The single-letter system codes ("G", "E", ...) are also accepted.
     * @param rinex_code    Three-character RINEX-3 observation code (e.g. "C2W").
     *                      Codes shorter than 3 characters return @c Unknown.
     * @return Classification result; @c band==Unknown when the code is not
     *         recognized for the given constellation.
     */
    SignalClassification classifySignal(const std::string& constellation,
                                        const std::string& rinex_code);

    /**
     * @brief Classify using the leading character of a satellite ID to derive the constellation.
     *
     * Convenience wrapper for call sites that only have the satellite id
     * (e.g. "G02", "E11") rather than the constellation string.
     */
    SignalClassification classifySignalForSat(const std::string& satellite,
                                              const std::string& rinex_code);

    /**
     * @brief Tracking-attribute priority for canonical-code selection.
     *
     * Lower return value = higher priority.  Used when a single receiver
     * reports multiple codes for the same (sat, band) and a single
     * "canonical" code must be picked for cross-receiver pairing.
     *
     * Priority ordering favors:
     *  - semi-codeless P-code and modernized signals on legacy bands
     *    (e.g. C2W > C2P > C2L > C2C > C2S > C2X for GPS L2),
     *  - pilot/data-combined codes over pilot- or data-only on modernized
     *    bands (e.g. Galileo E1 prefers C1X > C1C > C1B > C1A > C1Z).
     *
     * Codes whose attribute is unknown for the given band return a large
     * sentinel value so they sort last.
     *
     * @param constellation Constellation name (see @c classifySignal()).  Not read
     *                      by the current implementation: the band alone selects the order.
     * @param band          Physical band identifier.
     * @param attribute     Third RINEX-code character (e.g. 'W', 'C', 'L').
     * @return Non-negative priority; lower wins.
     */
    int signalAttributePriority(const std::string& constellation,
                                FrequencyBand band,
                                char attribute);

    /**
     * @brief Stable human-readable label for a @c FrequencyBand.
     *
     * Used in logs, diagnostics, and (optionally) DD ambiguity keys
     * when band-level keying is desired over RINEX-code keying.
     */
    const char* bandLabel(FrequencyBand band);

    /**
     * @brief Nominal carrier wavelength (m) for a physical band, from @c Constants.
     *
     * A pure, stateless function of the band identity, so the producer and every
     * replay consumer obtain the same value.  That makes it usable as a fixed
     * producer↔consumer basis for discontinuity thresholds (@c frac·λ).
     *
     * @note For the FDMA GLONASS bands (@c GloG1 / @c GloG2) this returns the
     *       base-frequency (channel k=0) wavelength.  @c SignalBand is slot-stateless
     *       by design (see the file banner), so it cannot apply the per-satellite
     *       channel offset; callers that need the slot-exact FDMA wavelength add
     *       @c k·offset themselves.  The nominal value is deterministic and so
     *       remains a valid contract basis: the slot deviation is at most about 0.25 %
     *       of the wavelength, which is immaterial to a ¼-cycle gate, and network
     *       GLONASS is currently off.
     *
     * @param band Physical band identifier.
     * @return Nominal wavelength in metres, or 0.0 for @c Unknown / unmapped bands.
     */
    double bandNominalWavelengthMeters(FrequencyBand band);

} // namespace GnssProcessor
