#pragma once

/**
 * @file BaselineFixtures.hpp
 * @brief Named test-fixture presets for baseline / DD processing.
 *
 * The baseline pipeline needs four inputs that are tedious to type into a
 * JSON config: reference station name, reference ECEF (mm-accurate, so the
 * rover absolute solution does not inherit a reference error), rover name,
 * and the date the obs files cover.  This header bundles them into named
 * presets selected by the @c baseline_fixture top-level config field:
 *
 *   "baseline_fixture": "IGS_Pasadena"   // or "OSU_Campus", or "Custom"
 *
 * Preset names are matched case-insensitively (the loader also maps '-' and
 * ' ' to '_'); an absent or empty field means "Custom".  When a known preset
 * is selected, @c GnssConfig::loadBaselineSection_ fills in any field left
 * blank.  Values given in the @c baseline JSON block always override the
 * preset, which only supplies defaults for a standard test.
 *
 * To add a preset:
 *   1. Append a row to @c baselineFixturePresets() below.
 *   2. If the obs files are on CDDIS, add a matching entry to
 *      @c FIXTURE_OBSERVATIONS in @c scripts/bootstrap_internal_data.py
 *      so internal-mode runs can fetch the data.
 *   3. If the date is new, add it to @c FIXTURE_DATES in the same
 *      script so the bootstrap pulls SP3/CLK/BIA/ERP/BRDC for it.
 *
 * Coordinates are ITRF2014 epoch 2010.0 unless noted.  IGS stations are
 * accurate to about 1 mm; sub-cm absolute positioning of the rover requires
 * checking the reference against the current site log.
 */

#include <array>
#include <map>
#include <string>

namespace GnssProcessor {

/**
 * @brief One named baseline test-fixture bundle.
 *
 * An empty @c rover_name denotes a single-station fixture.  The baseline
 * pipeline needs at least one rover, so no current preset uses this; the
 * field is nullable so that PPP-only presets could share this catalog.
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
        // OSU_Campus: short local survey.  Not on any public archive,
        // so for internal-mode runs the obs files are placed manually
        // (the bootstrap script prints instructions).  The reference
        // ECEF is the "NARY" mark surveyed by the OSU geomatics group;
        // mm-level rover positioning requires checking it against the
        // local survey records.
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
        // IGS_Pasadena: Caltech (CIT1) vs JPL (JPLM), ~10 km baseline,
        // both stations on the IGS daily archive at CDDIS.  Recommended
        // for repeatable testing: the data can be re-fetched and the
        // coordinates are published on igs.org.
        //
        // The ECEF below is the CIT100USA (Caltech) monument in ITRF2020,
        // from the NGS Station Info File (data/static/sif/igs20_SIF.json),
        // propagated with the station velocity to the fixture's
        // observation epoch (2024-DOY129 ≈ 2024.35), so the held-fixed DD
        // reference matches the antenna position on the observation day.
        // It converts to lat 34.1367°, lon -118.1273°, h 215 m (Caltech).
        // The earlier value { -2492006.9389, -4659170.3174, 3559273.6829 }
        // was off by about 1.7 km (it converts to a -694 m height), and
        // the rover absolute solution inherited that error.  See
        // docs/test_suite.md, "Findings".
        // -----------------------------------------------------------
        {"igs_pasadena", BaselineFixturePreset{
            "CIT100USA",
            { -2491490.7288, -4660802.8538, 3559129.1818 },
            "JPLM00USA",
            2024, 129,
            "IGS Caltech vs JPL baseline (~10 km, CIT1 ref + JPLM rover, 2024-DOY129). "
            "Both stations on CDDIS daily archive."
        }},
    };
    return kPresets;
}

} // namespace GnssProcessor
