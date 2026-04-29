#pragma once

/**
 * @file BaselineFixtures.hpp
 * @brief Named test-fixture presets for baseline / DD processing.
 *
 * The baseline pipeline needs four pieces of input that are tedious
 * to type into a JSON config: reference station name, reference ECEF
 * (mm-accurate so the rover absolute solution doesn't drift), rover
 * name, and the date the obs files cover.  This header bundles those
 * pieces into named presets that the user picks via the
 * @c baseline_fixture top-level config field:
 *
 *   "baseline_fixture": "IGS_Pasadena"   // or "OSU_Campus", or "Custom"
 *
 * Recognised preset names are case-insensitive.  When a known preset
 * is selected, @c GnssConfig::loadBaselineSection_ fills in any field
 * the user left blank.  User-typed values in the @c baseline JSON
 * block always override preset defaults — the preset is purely a
 * convenience for "I want the standard X test."
 *
 * To add a preset:
 *   1. Append a row to @c baselineFixturePresets() below.
 *   2. If the obs files are on CDDIS, add a matching entry to
 *      @c FIXTURE_OBSERVATIONS in @c scripts/bootstrap_internal_data.py
 *      so internal-mode users can fetch the data.
 *   3. If the date is new, add it to @c FIXTURE_DATES in the same
 *      script so the bootstrap pulls SP3/CLK/BIA/ERP/BRDC for it.
 *
 * Coordinates are ITRF2014 epoch 2010.0 unless noted.  IGS stations
 * are accurate to ~mm; verify against the current site log if you
 * need sub-cm absolute positioning out of the rover.
 */

#include <array>
#include <map>
#include <string>

namespace GnssProcessor {

/**
 * @brief One named baseline test-fixture bundle.
 *
 * Empty strings in @c rover_name signal a single-station fixture
 * (rare; the baseline pipeline needs at least one rover, but we keep
 * the field nullable so future "PPP-only" presets could share this
 * catalog if the design ever extends that way).
 */
struct BaselineFixturePreset {
    std::string ref_name;                  ///< Reference station ID (e.g. "CIT100USA" or "NARY").
    std::array<double, 3> ref_ecef;        ///< Reference ECEF [X, Y, Z] in metres (ITRF2014 epoch 2010.0 unless noted).
    std::string rover_name;                ///< Rover station ID; empty for single-station fixtures.
    int year{ 0 };                         ///< Observation year (4-digit).
    int doy{ 0 };                          ///< Day of year (1-366).
    std::string description;               ///< One-line note for log output.
};

/**
 * @brief Lookup table of all known baseline fixtures.
 *
 * Keys are lower-case so case-insensitive lookup is a single
 * @c toLower + @c find pair.  Returned by const reference; the
 * underlying static is initialised on first call (Meyers singleton).
 */
inline const std::map<std::string, BaselineFixturePreset>& baselineFixturePresets() {
    static const std::map<std::string, BaselineFixturePreset> kPresets = {
        // -----------------------------------------------------------
        // OSU_Campus — short local survey.  Not on any public archive,
        // so internal-mode users must drop the obs files in manually
        // (the bootstrap script prints instructions).  Reference ECEF
        // is the "NARY" mark surveyed by the OSU geomatics group;
        // verify against your local survey records before trusting
        // mm-level rover positioning.
        // -----------------------------------------------------------
        {"osu_campus", BaselineFixturePreset{
            "NARY",
            { -2497077.4917, -3805547.6556, 4453089.1921 },
            "233",
            2023, 321,
            "OSU campus baseline (~340 m, NARY ref + 233 rover, 2023-DOY321). "
            "Local research data; not on CDDIS."
        }},

        // -----------------------------------------------------------
        // IGS_Pasadena — Caltech (CIT1) vs JPL (JPLM), ~10 km baseline,
        // both stations on the IGS daily archive at CDDIS.  Recommended
        // default for repeatable testing — anyone can re-fetch the
        // data and the coordinates are published on igs.org.
        //
        // ECEF below is from the IGS station coordinate file
        // (igs14.snx) at epoch 2010.0; current epoch values differ by
        // a few cm from plate motion but that's well below the
        // discrimination level of a development run.
        // -----------------------------------------------------------
        {"igs_pasadena", BaselineFixturePreset{
            "CIT100USA",
            { -2492006.9389, -4659170.3174, 3559273.6829 },
            "JPLM00USA",
            2024, 129,
            "IGS Caltech vs JPL baseline (~10 km, CIT1 ref + JPLM rover, 2024-DOY129). "
            "Both stations on CDDIS daily archive."
        }},
    };
    return kPresets;
}

} // namespace GnssProcessor
