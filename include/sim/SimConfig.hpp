#pragma once

/**
 * @file SimConfig.hpp
 * @brief Tunable knobs for the synthetic-observation generator (runSynth).
 *
 * Collects the measurement-error, multipath, cycle-slip, atmosphere, bias and
 * trajectory parameters that were previously hardcoded inside @c runSynth into
 * one struct, so a new exam case (e.g. a smartphone-grade receiver, or a
 * slip-heavy low-elevation scenario) can be shipped as a JSON file instead of a
 * recompile.  See @c loadSimConfig and @c C:/GnssSimulator/scenario/sim_*.json.
 *
 * EVERY default below equals the value that was hardcoded at the time this
 * struct was introduced, so a default-constructed @c SimConfig — and any JSON
 * that omits a field — reproduces the original synthesis byte-for-byte.  The
 * two RNG seeds are exposed so a case can be reshuffled without touching the
 * physics; keeping them at the defaults preserves the storm-on/off control and
 * the published answer keys.
 *
 * This header is intentionally free of any JSON dependency; parsing lives in
 * SimConfig.cpp so translation units that only consume the resolved struct
 * (e.g. main.cpp) do not pull in nlohmann/json.
 */

#include <cstdint>
#include <filesystem>
#include <string>

namespace GnssProcessor {
namespace sim {

/** @brief All run-time knobs for one synthesized dataset. */
struct SimConfig {
    /// White measurement noise: zenith 1-sigma, scaled by 1/sin(elevation).
    struct Noise {
        double code_sigma_zen_m{ 0.30 };       ///< Code pseudorange sigma at zenith (m).
        double phase_sigma_zen_m{ 0.003 };     ///< Carrier phase sigma at zenith (m).
        double elev_factor_floor_deg{ 5.0 };   ///< Floor on elevation in the 1/sin(el) noise/slip amplification.
        std::uint64_t seed{ 20240315ULL };     ///< RNG seed for the white noise stream (must be >= 0).
    } noise;

    /// Marine multipath: per-(sat,band) Gauss-Markov, elevation-amplified.  Band-
    /// specific code amplitudes do NOT cancel in the iono-free combination.
    struct Multipath {
        double code_amp_gps_l1_m{ 0.020 };  ///< Zenith code MP amplitude, GPS L1 (m).
        double code_amp_gps_l2_m{ 0.030 };  ///< Zenith code MP amplitude, GPS L2 (m).
        double code_amp_gal_e1_m{ 0.015 };  ///< Zenith code MP amplitude, Galileo E1 (m).
        double code_amp_gal_e5a_m{ 0.015 }; ///< Zenith code MP amplitude, Galileo E5a (m).
        double phase_amp_zen_m{ 0.002 };    ///< Zenith phase MP amplitude, all bands (m).
        double tau_s{ 300.0 };              ///< Gauss-Markov correlation time (s).
        double elev_factor_cap_deg{ 20.0 }; ///< 1/sin(el) amplification is clamped below this.
        std::uint64_t seed{ 20240315ULL ^ 0xA17EC0DEull }; ///< RNG seed for the multipath+slip stream (must be >= 0).
    } multipath;

    /// Cycle slips: integer phase jumps, rate ~rate_base/sin^2(el) below max_elev.
    struct CycleSlips {
        double rate_base{ 0.0002 };       ///< Per-epoch slip probability base (scaled by 1/sin^2 el).
        int    min_gap_epochs{ 3 };       ///< Minimum epochs between slips on one (sat,band).
        double max_elev_deg{ 45.0 };      ///< No slips above this elevation.
        double min_cycles_zenith{ 8.0 };  ///< Min slip magnitude at zenith (cycles, before 1/sin el).
        double max_cycles_zenith{ 22.0 }; ///< Max slip magnitude at zenith (cycles, before 1/sin el).
        int    min_magnitude{ 2 };        ///< Floor on |slip| in cycles after scaling.
    } slips;

    /// Truth atmosphere injected on every ray (ZHD cancels via reuse; ZWD is estimated).
    struct Atmosphere {
        double zwd_m{ 0.15 }; ///< Truth wet zenith delay (m).
    } atmosphere;

    /// Inter-system / receiver biases injected as truth states.
    struct Biases {
        double isb_gps_gal_m{ 8.0 }; ///< GPS->Galileo inter-system bias on Galileo rows (m).
    } biases;

    /// Visibility screen.
    struct Geometry {
        double elevation_mask_deg{ 10.0 }; ///< Satellites below this elevation are dropped.
    } geometry;

    /// Ionospheric storm.  Amplitudes are pre-storm_scale; the window is placed as
    /// a fraction of the arc so it tracks trajectory length automatically.
    struct Iono {
        double storm_scale{ 1.0 };               ///< Multiplies SED/TID/scintillation (0 = storm off, same RNG).
        double quiet_vtec_tecu{ 25.0 };          ///< Quiet background VTEC (analytic fallback only).
        double sed_amp_tecu{ 15.0 };             ///< Storm-Enhanced-Density blob amplitude (vertical TECU).
        double tid_amp_tecu{ 4.0 };              ///< Travelling-Ionospheric-Disturbance amplitude (vertical TECU).
        double scint_sigma_phi_max_rad{ 0.6 };   ///< Peak scintillation phase 1-sigma (rad).
        double window_start_frac{ 0.25 };        ///< Storm window start as a fraction of the arc.
        double window_duration_frac{ 0.50 };     ///< Storm window duration as a fraction of the arc.
        // SED blob shape + motion (decides which sats see the storm + the gradient).
        double sed_sigma_lat_deg{ 6.0 };         ///< SED Gaussian sigma in IPP latitude (deg).
        double sed_sigma_lon_deg{ 8.0 };         ///< SED Gaussian sigma in IPP longitude (deg).
        double drift_lat_deg_per_min{ -0.15 };   ///< Storm-centre latitude drift (deg/min).
        double drift_lon_deg_per_min{ 0.30 };    ///< Storm-centre longitude drift (deg/min).
        double center_lat_offset_deg{ 0.0 };     ///< Storm centre latitude offset from the trajectory origin (deg).
        double center_lon_offset_deg{ 0.20 };    ///< Storm centre longitude offset from the trajectory origin (deg).
        // Travelling-Ionospheric-Disturbance geometry.
        double tid_wavelength_km{ 300.0 };       ///< TID horizontal wavelength (km).
        double tid_period_s{ 1200.0 };           ///< TID period (s).
        double tid_azimuth_deg{ 135.0 };         ///< TID propagation heading from north (deg).
        // Thin-shell mapping + scintillation low-elevation floor.
        double shell_height_km{ 350.0 };         ///< Ionospheric thin-shell height (km); sets IPP + obliquity.
        double scint_elev_clamp_deg{ 10.0 };     ///< Low-elevation floor for the scintillation envelope (deg).
    } iono;

    /// Truth trajectory (a subset of TrajectoryConfig; kinematic flag comes from
    /// the synth-static/synth-kinematic subcommand, not the file.  The start epoch
    /// t0/date is NOT exposed — it is tied to the bundled SP3/CLK/nav fixtures and
    /// cannot be changed by config alone).
    struct Trajectory {
        double interval_s{ 30.0 };          ///< Epoch spacing (s).
        int    n_epochs{ 120 };             ///< Number of epochs (arc length).
        double cruise_speed_mps{ 4.5 };     ///< Cruise speed (kinematic only).
        double lat0_deg{ -18.5 };           ///< Origin latitude (deg).
        double lon0_deg{ -178.2 };          ///< Origin longitude (deg).
        double h0_m{ 0.0 };                 ///< Origin ellipsoidal height (m).
        double heading0_deg{ 35.0 };        ///< Mean heading (deg).
        double heading_amp_deg{ 60.0 };     ///< Heading weave amplitude (deg, kinematic).
        double heading_period_s{ 1200.0 };  ///< Heading weave period (s, kinematic).
        double heave_amp_m{ 1.0 };          ///< Sea-state heave amplitude (m, kinematic).
        double heave_period_s{ 8.0 };       ///< Sea-state heave period (s, kinematic).
        double clock_bias0_s{ 1.0e-4 };     ///< Initial receiver clock bias (s).
        double clock_drift_sps{ 0.0 };      ///< Receiver clock drift (s/s); 0 = constant bias.
    } trajectory;
};

/**
 * @brief Load a SimConfig from a JSON file (nlohmann, `//` comments allowed).
 *
 * Fields may sit at the document root or under a top-level @c "simulator"
 * object (so a sim config can later be bundled into a processor config).  Any
 * omitted field keeps @p cfg's current value, so pass a default-constructed
 * SimConfig to get "defaults except what the file overrides".
 *
 * @param path  JSON file to read.
 * @param cfg   In/out: pre-seeded with defaults, overwritten per present key.
 * @param err   Optional: set to a human-readable message on failure.
 * @return true on success; false (with @p err set) if the file is missing or
 *         not parseable.
 */
bool loadSimConfig(const std::filesystem::path& path, SimConfig& cfg,
                   std::string* err = nullptr);

} // namespace sim
} // namespace GnssProcessor
