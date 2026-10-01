#pragma once

/**
 * @file SimConfig.hpp
 * @brief Tunable knobs for the synthetic-observation generator (runSynth).
 *
 * Collects the measurement-error, multipath, cycle-slip, atmosphere, bias and
 * trajectory parameters of @c runSynth in one struct, so a new exam case (e.g. a
 * smartphone-grade receiver, or a slip-heavy low-elevation scenario) ships as a
 * JSON file instead of a recompile.  See @c loadSimConfig and
 * @c C:/GnssSimulator/scenario/sim_*.json.
 *
 * Every default reproduces the synthesis as it was before the knob existed, so a
 * default-constructed @c SimConfig, and any JSON that omits a field, reproduces
 * the original synthesis byte-for-byte.  The RNG seeds are exposed so a case can
 * be reshuffled without touching the physics; keeping them at the defaults
 * preserves the storm-on/off control and the published answer keys.
 *
 * The header has no JSON dependency; parsing lives in SimConfig.cpp, so
 * translation units that only consume the resolved struct (e.g. main.cpp) do not
 * pull in nlohmann/json.
 */

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "sim/AmbiguityDraw.hpp"

namespace GnssProcessor {
namespace sim {

/**
 * @brief One scripted cycle-slip event, layered on top of the random slip model
 *        without touching its RNG stream (see SPEC_scripted_slips.md).
 *
 * Each event injects a signed integer phase-ambiguity jump into one
 * (satellite, phase-signal) at a deterministic trigger.  The trigger has exactly
 * one of two forms: form A (@c epoch, a 1-based epoch index) or form B
 * (@c elev_lo_deg / @c elev_hi_deg, an elevation window given in JSON as
 * @c elev_deg [lo, hi) with 0 <= lo < hi <= 90, fired at the first visible
 * epoch >= @c not_before_epoch inside the window).  All fields are validated by
 * @c loadSimConfig; this struct is a plain resolved record.
 */
struct ScriptedSlip {
    std::string sat;            ///< "G05"; ^[GE]\d\d$, must be in the signal plan.
    std::string signal;         ///< Phase code, e.g. "L2W": GPS L1C|L2W|L5Q, Galileo L1C|L5Q|L7Q (third band if enabled).
    int    epoch{ 0 };          ///< Form A: 1-based epoch; 0 = unset (use the elevation window).
    double elev_lo_deg{ -1.0 }; ///< Form B: window low bound (deg); <0 = unset.
    double elev_hi_deg{ -1.0 }; ///< Form B: window high bound (deg, exclusive); <0 = unset.
    int    not_before_epoch{ 1 };///< Form B: earliest epoch the window may fire (1-based).
    int    delta_cycles{ 0 };   ///< Signed integer jump; |delta| <= 100000; 0 only with flag_lli.
    int    repeat{ 1 };         ///< Apply the same delta on this many visible epochs; a mask gap pauses the count.
    bool   flag_lli{ false };   ///< Set LLI=1 on each applied repetition.
};

/**
 * @brief A per-observable receiver bias injected as truth
 *        (docs/SPEC_synthetic_minimum_network.md §3, §4.5).
 *
 * Without these records the only receiver-side bias in the simulator is the
 * scalar GPS->Galileo ISB, added identically to Galileo code and phase, so every
 * per-signal receiver code bias is zero.  A network fixture needs non-zero ones:
 * the S-basis contract's Table-8 identity (§B row 4) states that the estimable
 * ionosphere differs from the true ionosphere by the fleet-mean receiver GF code
 * bias minus the satellite GF bias.  With every receiver bias zero, the
 * satellite-mean form of that identity reduces to 0 == 0 and the system-level
 * unit test of it proves nothing, so the network fixture requires the injection.
 *
 * A non-zero receiver code bias makes the dataset unsolvable by a bias-blind
 * single-receiver PPP, which is why the default is an empty list (byte-identical
 * to every existing scenario).
 */
struct RxBias {
    std::string code;        ///< RINEX-3 observation code the bias applies to, e.g. "C1C" / "L2W".
    double value_m{ 0.0 };   ///< Bias added to that observable (m).  Code and phase are independent.
};

/**
 * @brief One peer station in a synthetic network fixture (`synth-network`).
 *
 * The network driver does not synthesize stations in-process: it re-invokes the
 * executable once per station as a child process (the mechanism the `all`
 * subcommand already uses for `spp-reference`).  An in-process loop is unsafe
 * (docs/SPEC_synthetic_minimum_network.md §1) for two reasons: a second
 * @c EphemerisProvider re-registers the spdlog logger name and the catch handler
 * re-throws out of the process, and a shared provider carries three
 * station-dependent quantities with no per-station setter (the phase wind-up
 * accumulator is keyed by satellite ID alone, ocean loading is frozen at one
 * site, and the receiver antenna type is one string).
 *
 * Each station therefore writes to its own output directory, which is why the
 * @c truth_*.csv writers need no station column: the directory is the station
 * dimension.
 */
struct NetworkStation {
    std::string name;            ///< 9-char marker, e.g. "SNA000SIM"; also the RINEX filename prefix.
    double lat_deg{ 0.0 };       ///< Station latitude (deg).
    double lon_deg{ 0.0 };       ///< Station longitude (deg).
    double h_m{ 0.0 };           ///< Station ellipsoidal height (m).
    /// Held out of the network solve and used as the closed-loop consumer
    /// (network.closed_loop.station).  At most one station may set this.
    bool consumer{ false };
    /// Per-station receiver biases.  A non-empty list replaces the base config's
    /// list of the same kind for this station.  The values must differ across the
    /// roster: the Table-8 identity the fixture tests resolves the fleet mean of the
    /// receiver GF code bias, and on a roster where every station carries the same
    /// bias the fleet mean is indistinguishable from each member, so the estimator
    /// could realize any split between them and still grade as correct.  They are
    /// declared per station rather than derived from the salt so that the truth is
    /// stated explicitly.
    std::vector<RxBias> rx_code_bias;
    std::vector<RxBias> rx_phase_bias;
    /// Per-station override of @c SimConfig::PhaseColour::sigma_zen_m (m; JSON roster
    /// key @c phase_colour_sigma_zen_m, finite and >= 0).  NaN (the default) = absent:
    /// the station inherits the network-level @c phase_colour block.  The coloured
    /// phase error is calibrated per station class (tropical, mid, high latitude), so
    /// the class is stated on the roster entry rather than derived from the latitude.
    double phase_colour_sigma_zen_m{ std::numeric_limits<double>::quiet_NaN() };
    /// Per-station override of @c SimConfig::PhaseColour::tau_s (s; JSON roster key
    /// @c phase_colour_tau_s, finite and > 0).  NaN (the default) = inherit.
    double phase_colour_tau_s{ std::numeric_limits<double>::quiet_NaN() };
};

/** @brief All run-time knobs for one synthesized dataset. */
struct SimConfig {
    /**
     * @brief Identity of the receiver this invocation synthesizes.
     *
     * An empty @c name (the default) reproduces the historical single-station
     * behaviour byte-for-byte: marker "SARV", marker number "SARV00FJI", output
     * file "SARV_R_<yyyydoy>0000_01D_30S_MO.rnx".  The `synth-network` driver
     * writes one child config per station with this block filled in.
     *
     * @c seed_salt is mixed into each per-station RNG seed (noise, multipath/slip,
     * troposphere, receiver clock) and into the integer-ambiguity hash.  It must be
     * non-zero and distinct per station; the driver derives it from the station
     * name (@c stationSeedSalt) so it is reproducible.  With a salt of 0 at every
     * station, all stations share one noise, multipath, slip, ZWD and
     * receiver-clock realization: between-station differences cancel the noise
     * exactly, the common receiver clock is absorbed into the satellite-clock datum,
     * and the network looks perfectly conditioned while carrying no information
     * (SPEC §1.1 F1).
     */
    struct StationId {
        std::string name{};             ///< Marker + filename prefix; empty = legacy "SARV".
        std::uint64_t seed_salt{ 0 };   ///< Mixed into the station's RNG seeds and into ambN; 0 = legacy.
    } station;

    /// Peer roster for `synth-network`.  Empty (the default) = single-station
    /// synthesis; the other subcommands ignore it entirely.
    std::vector<NetworkStation> network_stations;

    /// White measurement noise: zenith 1-sigma, scaled by 1/sin(elevation).
    struct Noise {
        double code_sigma_zen_m{ 0.30 };       ///< Code pseudorange sigma at zenith (m).
        double phase_sigma_zen_m{ 0.003 };     ///< Carrier phase sigma at zenith (m).
        double elev_factor_floor_deg{ 5.0 };   ///< Floor on elevation in the 1/sin(el) noise/slip amplification.
        std::uint64_t seed{ 20240315ULL };     ///< RNG seed for the white noise stream (must be >= 0).
    } noise;

    /// Marine multipath: per-(sat,band) Gauss-Markov, elevation-amplified.  Band-
    /// specific code amplitudes do not cancel in the iono-free combination.
    struct Multipath {
        double code_amp_gps_l1_m{ 0.020 };  ///< Zenith code MP amplitude, GPS L1 (m).
        double code_amp_gps_l2_m{ 0.030 };  ///< Zenith code MP amplitude, GPS L2 (m).
        double code_amp_gps_l5_m{ 0.015 };  ///< Zenith code MP amplitude, GPS L5 (m); inert unless @ref Signals::gps_l5.
        double code_amp_gal_e1_m{ 0.015 };  ///< Zenith code MP amplitude, Galileo E1 (m).
        double code_amp_gal_e5a_m{ 0.015 }; ///< Zenith code MP amplitude, Galileo E5a (m).
        double code_amp_gal_e5b_m{ 0.015 }; ///< Zenith code MP amplitude, Galileo E5b (m); inert unless @ref Signals::galileo_e5b.
        double phase_amp_zen_m{ 0.002 };    ///< Zenith phase MP amplitude, all bands (m).
        double tau_s{ 300.0 };              ///< Gauss-Markov correlation time (s).
        double elev_factor_cap_deg{ 20.0 }; ///< 1/sin(el) amplification is clamped below this.
        std::uint64_t seed{ 20240315ULL ^ 0xA17EC0DEull }; ///< RNG seed for the multipath+slip stream (must be >= 0).
    } multipath;

    /// Coloured carrier-phase error (card SFR,
    /// docs/refound/SFR_sim_fixture_coloured_phase_2026-10-01.md section 2): one
    /// unit-variance first-order Gauss-Markov process per line of sight, scaled by
    /// @c sigma_zen_m and the white phase noise's elevation factor
    /// (1/sin(max(el, floor))), with a stationary start and phi = exp(-dt/tau) across
    /// a visibility gap.  BAND-COMMON: the same metres reach the carrier phase of
    /// every band of the satellite and nothing reaches code, so the ionosphere-free
    /// combination keeps all of it and the free slant ionosphere none.  See
    /// @c PhaseColourGenerator (sim/PhaseColour.hpp) for the recursion.
    ///
    /// The field's ionosphere-free post-fit phase residual is about 90 % such a
    /// process (5 to 7 mm, tau 80 to 120 s, strongest in the tropics); the historical
    /// sim's is about 80 % white.  The block is separate from @c multipath (one shared
    /// code-and-phase draw per band, left as it is).
    ///
    /// Default @c sigma_zen_m = 0 = OFF: nothing is drawn, no truth file or summary
    /// field is written, and every existing recipe regenerates byte-for-byte.  The
    /// stream is dedicated and station-salted like the others, so an active block
    /// cannot move the white-noise, multipath/slip, troposphere or receiver-clock
    /// realizations.  A roster entry may override @c sigma_zen_m and @c tau_s per
    /// station (@c NetworkStation), which is how the per-class calibration is stated.
    /// When active, the realization is written to @c truth_phase_colour.csv.
    struct PhaseColour {
        double sigma_zen_m{ 0.0 };   ///< Zenith 1-sigma of the coloured phase error (m); 0 = off.  Must be >= 0.
        double tau_s{ 100.0 };       ///< Gauss-Markov correlation time (s); must be > 0.
        /// Floor on elevation in the 1/sin(el) amplification (deg).  NaN (the default)
        /// = the white noise's @c noise.elev_factor_floor_deg (5 deg by default), so the
        /// two phase errors share one elevation shape unless this is set explicitly.
        /// Resolve with @ref SimConfig::phaseColourElevFloorDeg.
        double elev_factor_floor_deg{ std::numeric_limits<double>::quiet_NaN() };
        std::uint64_t seed{ 20240315ULL ^ 0xC0105EEDull }; ///< Dedicated RNG seed (must be >= 0); distinct from every other stream.
    } phase_colour;

    /**
     * @brief The phase-colour elevation floor in effect (deg).
     *
     * @return @c phase_colour.elev_factor_floor_deg when it is set (finite), else
     *         @c noise.elev_factor_floor_deg.
     */
    double phaseColourElevFloorDeg() const {
        return std::isfinite(phase_colour.elev_factor_floor_deg)
                   ? phase_colour.elev_factor_floor_deg
                   : noise.elev_factor_floor_deg;
    }

    /// Which signals each constellation transmits.  Two bands per system is the
    /// historical fixture (GPS C1C/L1C + C2W/L2W, Galileo C1C/L1C + C5Q/L5Q); a third
    /// band is opt-in.
    ///
    /// The third band is opt-in because noise, multipath and slip draws are taken per
    /// band inside the satellite loop, so appending a band shifts every later draw in
    /// the stream.  With both switches off every existing fixture regenerates
    /// byte-identically; turning one on starts a new lineage rather than perturbing
    /// an old one.
    ///
    /// A third band is what lets a fixture exercise the slant posture's code-bias
    /// channel (Odijk et al. 2016, J Geod 90:15-44, §5.1).  With the ionosphere
    /// parameterized as free slant delays, the geometry-free code biases are absorbed
    /// by the ionosphere and the satellite code biases are not estimable on the first
    /// two frequencies; the paper finds them estimable only for a triple- or
    /// higher-frequency constellation (p. 29).  Its Eq. (33) gives the estimable
    /// quantity: the modernized DCB d^s_1 - d^s_j for j >= 3.  A two-band fixture
    /// cannot exercise that channel.
    struct Signals {
        bool gps_l5{ false };      ///< Add GPS C5Q/L5Q (L5, 1176.45 MHz) as a third GPS band.
        bool galileo_e5b{ false }; ///< Add Galileo C7Q/L7Q (E5b, 1207.14 MHz) as a third Galileo band.
        /// Amplitude (m) of a synthetic satellite OSB, per RINEX code, added on top of
        /// whatever the product `.BIA` supplies.  The shipped COD0MGXFIN OSB product
        /// has no GPS band-5 rows (its GPS inventory is L1C/L1W/L1L/L1X,
        /// L2W/L2S/L2L/L2X, C1C/C1W/C1L/C1X, C2W/C2S/C2L/C2X), so a third band
        /// synthesized from the product alone carries an identically zero code bias,
        /// and a grader cannot tell a correct bias estimate from a channel that is
        /// zero throughout.
        ///
        /// The per-satellite value is a deterministic function of the PRN (no RNG, so
        /// the simulator's RNG streams are untouched and byte-identity is preserved):
        /// value = amp * sin(2*pi*((prn*7) mod 23)/23).  Empty (default) = none.
        /// Keys are 3-character RINEX code or phase observables.  A non-empty map
        /// requires @c corrections.signal_bias, because the synthetic OSB is injected
        /// at the same site as the product OSB; the loader rejects it otherwise.
        std::map<std::string, double> synth_osb_amp_m;
    } signals;

    /// Cycle slips: integer phase jumps, rate ~rate_base/sin^2(el) below max_elev.
    struct CycleSlips {
        double rate_base{ 0.0002 };       ///< Per-epoch slip probability base (scaled by 1/sin^2 el).
        int    min_gap_epochs{ 3 };       ///< Minimum epochs between slips on one (sat,band).
        double max_elev_deg{ 45.0 };      ///< No slips above this elevation.
        double min_cycles_zenith{ 8.0 };  ///< Min slip magnitude at zenith (cycles, before 1/sin el).
        double max_cycles_zenith{ 22.0 }; ///< Max slip magnitude at zenith (cycles, before 1/sin el).
        int    min_magnitude{ 2 };        ///< Floor on |slip| in cycles after scaling.
        double taper_start_frac{ 1.0 };   ///< Arc fraction after which the slip rate cos-ramps to 0 by the end (1.0 = no taper).
        /// Deterministic hand-authored slip events layered on top of the random model
        /// above, with no interaction with its RNG stream.  Empty by default, so an
        /// absent or empty "scripted" key reproduces the random-only dataset
        /// byte-for-byte.  See @c ScriptedSlip and SPEC_scripted_slips.md.
        std::vector<ScriptedSlip> scripted;
    } slips;

    /// Magnitude regime the arc-constant integer ambiguities are drawn from.
    ///
    /// This fixture property decides what an estimator experiment can conclude; it
    /// is not a cosmetic scale.  The historical draw is @f$N\in[10^5,10^6)@f$, about
    /// four decades above what a geodetic receiver produces (measured on eight
    /// bundled IGS stations, DOY-129: median wide-lane @f$|N_1-N_2|@f$ = 28.5 cyc,
    /// median double-difference 29.7 cyc, both Melbourne-Wubbena and so iono-free).
    /// A cold start that linearizes about zero therefore begins far outside its
    /// capture region on the sim and inside it on the field, so a sim result on the
    /// legacy regime does not transfer to the field (on the 60-station bisection
    /// ladder, float beat every eq34 arm on the sim while the matched field arms
    /// passed).
    ///
    /// Default @c Legacy keeps every existing fixture byte-identical on
    /// regeneration; @c Field is required for any experiment touching a zero-target
    /// datum (eq34, carry_invariant, seed-round).  JSON: @c ambiguity.regime
    /// ("legacy" | "field") and @c ambiguity.half_width_cycles (field regime only,
    /// [1, 100000]).  See @ref AmbiguityDraw.hpp for the full derivation and the two
    /// structural properties the draw must preserve in either regime.
    AmbiguityDrawConfig ambiguity;

    /// Truth atmosphere injected on every ray.  ZHD cancels because the processor's
    /// model is reused; ZWD is a state the processor estimates, so it does not cancel
    /// and is recovered instead.
    ///
    /// The wet delay may be made time-varying, as a real troposphere is and as an
    /// external PPP will track.  The per-epoch truth is
    ///     zwd(t) = max(zwd_min_m, zwd_m + diurnal_amp*sin(2*pi*(t-t0+phase)/period) + ou(t))
    /// where ou(t) is a mean-reverting Ornstein-Uhlenbeck / Gauss-Markov term (the same
    /// shape the marine-multipath model uses), stationary with 1-sigma @c zwd_rw_sigma_m
    /// and correlation time @c zwd_rw_tau_s.  A pure random walk is not used: over a
    /// 12 h arc it wanders without bound and drags the arc mean off the configured value
    /// (3 mm/epoch moved the mean from 0.15 to 0.25 m).  The term is drawn once per
    /// epoch from a dedicated RNG stream, so tuning it cannot shift the
    /// measurement-noise or multipath/slip streams, and the storm-on/off control and
    /// byte-identity stay intact.  All variation knobs default to 0, so an omitted key
    /// reproduces the constant-ZWD dataset byte-for-byte.
    ///
    /// When any variation is active the per-epoch truth is written to @c truth_tropo.csv
    /// and @c truth_scalars.csv's @c zwd_m carries the arc mean (the Verifier grades the
    /// series; the single scalar remains for the student-facing grader).
    /// The processor's @c filter.process_noise.troposphere_q must be loose enough to
    /// track the injected variation.  A value tuned for a constant ZWD (e.g. 1e-10)
    /// cannot, and the residual leaks into height through the strong ZWD/height
    /// correlation.
    struct Atmosphere {
        double zwd_m{ 0.15 };                  ///< Truth wet zenith delay (m); the mean when varying.
        double zwd_diurnal_amp_m{ 0.0 };       ///< Diurnal sinusoid amplitude (m); 0 = no diurnal term.
        double zwd_diurnal_period_s{ 86400.0 };///< Diurnal period (s); must be > 0.
        double zwd_diurnal_phase_s{ 0.0 };     ///< Phase offset into the sinusoid (s).
        double zwd_rw_sigma_m{ 0.0 };          ///< Stationary 1-sigma of the Gauss-Markov term (m); 0 = none.
        double zwd_rw_tau_s{ 3600.0 };         ///< Gauss-Markov correlation time (s); must be > 0.
        double zwd_min_m{ 0.01 };              ///< Floor (m); a wet delay cannot go negative.
        std::uint64_t seed{ 20240315ULL ^ 0x7A0D51DEull }; ///< Dedicated tropo RNG seed (must be >= 0).
    } atmosphere;

    /// Inter-system / receiver biases injected as truth states.
    struct Biases {
        double isb_gps_gal_m{ 8.0 }; ///< GPS->Galileo inter-system bias on Galileo rows (m).
        /// Per-observable receiver code biases (m), added to the matching code
        /// observable at this station.  Empty (the default) = none, byte-identical.
        /// See @ref RxBias for why a network fixture cannot leave these at zero.
        /// Truth is written to @c truth_rx_bias.csv whenever either list is non-empty.
        std::vector<RxBias> rx_code_bias;
        /// Per-observable receiver phase biases (m).  Under the producer's S-basis the
        /// receiver phase bias is pure gauge (it is absorbed into the eq34 datum), so a
        /// zero-injected RPB is legitimate truth.  The knob exists so that a zero value
        /// is a stated fact in @c truth_datum.json rather than an unexamined default,
        /// even though the fixture normally leaves it empty.
        std::vector<RxBias> rx_phase_bias;
    } biases;

    /// Full IERS/IGS correction stack (M7).  Each enabled term is injected into the
    /// synthesized observation by reusing the processor's own model, so a
    /// full-correction PPP (or an external service like CSRS-PPP) that applies the
    /// same model recovers the truth (the DESIGN §2.3 "reuse so it cancels" pattern,
    /// as used for the troposphere).  All default to false, so the bare dataset is
    /// byte-identical and the existing corrections-off exam loops keep closing.  The
    /// run config must enable the matching @c model_corrections.enable_* flags.
    /// Ocean tidal loading is supported (see @c ocean_tide below), sourced from the
    /// EOT20 global grid rather than a site BLQ.  Atmospheric pressure loading is
    /// omitted: it needs a site-specific grid that is undefined for a moving
    /// open-ocean vessel (it is 0 on both sides and cancels trivially).
    struct Corrections {
        bool relativity{ false };        ///< Relativistic periodic clock term (-c*dtr).
        bool shapiro{ false };           ///< Shapiro gravitational path delay (+m).
        bool solid_earth_tide{ false };  ///< IERS 2010 solid-earth tide (LOS +m).
        bool pole_tide{ false };         ///< Pole tide (needs an ERP product loaded).
        bool sat_antenna{ false };       ///< Satellite antenna PCO/PCV (needs ATX); wind-up is @c phase_windup.
        /// Ocean tidal loading (IERS 2010 Ch. 7.1.2, 11 BLQ constituents) injected by
        /// reusing the processor's own @c OceanTideModel, so it cancels when the run
        /// config sets @c model_corrections.enable_ocean_tide_loading.  Coefficients are
        /// synthesized from the EOT20 global load grid at @c eot20_nc_path.
        ///
        /// The processor loads OTL coefficients once, at the RINEX @c APPROX
        /// POSITION XYZ, and reuses them for the whole arc.  By default the sim loads
        /// them at the same fixed point, so a moving vessel still cancels exactly;
        /// @c ocean_tide_along_track breaks that symmetry deliberately.
        /// EOT20 supplies the radial component only (the horizontal amplitudes are zero),
        /// so this reproduces the dominant vertical term but not a 3-component FES2014b.
        bool ocean_tide{ false };
        std::string eot20_nc_path{};     ///< EOT20_load.nc path; required when ocean_tide is set.
        /// Sample the EOT20 load grid along the trajectory instead of freezing it at the
        /// APPROX POSITION (requires @c ocean_tide; the loader rejects it otherwise).
        /// Ocean loading is a field, so a vessel under way sees a continuously changing
        /// signal while the processor keeps evaluating the coefficients synthesized at
        /// the start of the arc.  The difference @c d(x(t),t) @c - @c d(x0,t) is then a
        /// real unmodelled error: zero at the first epoch and growing with along-track
        /// distance, common to every satellite but elevation-weighted through the LOS
        /// projection, and therefore partly absorbed by the receiver clock and ZWD
        /// rather than appearing as clean vertical error.  This is the one M7 term that
        /// does not cancel, by design.
        ///
        /// The magnitude is whatever EOT20 gives; it is not tunable, and over a short or
        /// slow arc it may be sub-mm.  Truth is written to @c truth_otl.csv.
        bool ocean_tide_along_track{ false };
        /// Distance between EOT20 samples along the track (km) when
        /// @c ocean_tide_along_track is set.  The grid itself is 0.125 deg (~14 km) and
        /// the load tide is smoother still, so the default is already far finer than the
        /// field's own structure; coefficients are interpolated between anchors in the
        /// complex plane.  Smaller values only cost EOT20 re-reads (~0.3 s each).
        double otl_anchor_spacing_km{ 5.0 };
        /// Receiver antenna PCO/PCV, injected by reusing the same @c Antenna model the
        /// processor runs, and written verbatim into the RINEX "ANT # / TYPE" header so
        /// the injected term and the declared antenna always agree.  The format is the
        /// RINEX field itself, 16-char model + 4-char radome (at most 20 characters),
        /// e.g. @c "TRM59800.00     NONE" (must exist in the ATX).  Needs the ATX.
        ///
        /// Empty (the default) = off: no receiver term is injected and the header keeps
        /// its uncalibrated placeholder, which is what an external service needs in
        /// order to apply nothing (CSRS-PPP "Case 4": null reference PCO).  When set,
        /// the run config must enable the matching
        /// @c model_corrections.enable_receiver_pco / @c _pcv, or the term does not
        /// cancel.
        ///
        /// Caveat: the receiver PCV look-up is azimuth-dependent in the antenna frame.
        /// Both sides assume a level, north-referenced antenna (the processor's
        /// rx-antenna-orientation option defaults off, so the raw topocentric azimuth is
        /// used); a yawing hull is not modelled.  The azimuthal term is mm-level; the
        /// dominant elevation-dependent part cancels exactly.
        std::string rx_antenna_type{};
        /// Observable-specific signal biases (OSB) from the day's Bias-SINEX (.BIA)
        /// product, injected by reusing the processor's own
        /// @c GnssCorrections::getBiasCorrection so they cancel when the run config sets
        /// @c model_corrections.enable_code_phase_bias.  When set, the COD MGX
        /// @c *_OSB.BIA for @c date.{year,doy} is loaded into the provider and its
        /// per-(satellite, observable) code and phase biases are added to every emitted
        /// signal (each RINEX code queried separately: C1C ≠ L1C ≠ C2W).
        ///
        /// This carries hardware physics the bare dataset omits.  The precise satellite
        /// clock is referenced to one ionosphere-free observable pair (COD MGX: GPS
        /// C1W/C2W, Galileo C1C/C5Q), but the receiver logs GPS C1C, so the C1C OSB is a
        /// per-satellite differential (~decimetre in the ionosphere-free code) that an
        /// OSB-ignorant PPP leaves uncorrected.  An OSB-aware external service
        /// (CSRS-PPP) applies it, so for such a tool a dataset without it is the biased
        /// one.  Galileo emits its own clock-reference signals, so its OSB IF
        /// combination is ≈0.  Truth per (satellite, observable) is written to
        /// @c truth_bias.csv.  Needs the .BIA fixture for the date.
        bool signal_bias{ false };
        /// Carrier-phase wind-up (Wu et al. 1993), injected by reusing the processor's
        /// own @c GnssCorrections::computePhaseWindUp in accumulated (unwrapped) mode:
        /// the injected term is continuous within each tracked arc, so the phase
        /// ambiguities stay time-constant integers for any estimator that models
        /// wind-up.  Phase-only, per band (the angle is band-common in cycles; metres
        /// scale with each wavelength; MW/WL cycles are untouched).  Satellite attitude
        /// is yaw-steering from the geometry's cached Sun with a velocity fallback; the
        /// receiver frame is the level, north-referenced local N/E dipole (the same
        /// convention as @c rx_antenna_type's PCV; a yawing hull is not modelled on
        /// either side).  The run config cancels it with
        /// @c model_corrections.enable_phase_windup plus
        /// @c estimation.accumulate_phase_windup (the processor's fractional default
        /// would differ from the data by integer cycles at every ±pi wrap).  An
        /// external full-stack PPP (CSRS-PPP) corrects wind-up unconditionally, so a
        /// dataset without it makes such a service absorb a time-varying term into its
        /// float ambiguities; injecting it is a correctness fix, by the same argument as
        /// the satellite antenna and OSB terms.  Truth per (epoch, satellite) is written
        /// to @c truth_windup.csv.
        bool phase_windup{ false };
    } corrections;

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
        /// Absolute storm-centre anchor (deg).  NaN (the default) keeps the historical
        /// behaviour: the centre is derived from this run's trajectory origin as
        /// @c lat0_deg + center_lat_offset_deg.  That is correct for a single receiver
        /// and wrong for a network synthesized as N independent processes: every
        /// station would re-derive the blob centre from its own origin and therefore sit
        /// exactly at the centre, so a travelling ionospheric disturbance would arrive
        /// simultaneously and identically everywhere and grade as a satellite-side
        /// effect, the opposite of the modelled physics.  The anchor takes effect only
        /// when both values are finite.  The network driver sets them, so every station
        /// samples one field at its own pierce point.
        ///
        /// Caveat: only the SED blob and the legacy continuous TID are IPP-driven.
        /// Explicit @c tid_pulses and @c turb_sats bypass the spatial weight entirely and
        /// are scaled by obliquity alone, so those two overlays have no spatial coherence
        /// even with an absolute anchor.  Use the SED blob for spatially structured
        /// network ionosphere.
        double center_lat_deg{ std::numeric_limits<double>::quiet_NaN() };
        double center_lon_deg{ std::numeric_limits<double>::quiet_NaN() };
        // Travelling-Ionospheric-Disturbance geometry.
        double tid_wavelength_km{ 300.0 };       ///< TID horizontal wavelength (km).
        double tid_period_s{ 1200.0 };           ///< TID period (s).
        double tid_azimuth_deg{ 135.0 };         ///< TID propagation heading from north (deg).
        double tid_period_start_s{ 0.0 };        ///< TID chirp start period (high-rate); <=0 => tid_period_s.
        double tid_period_end_s{ 0.0 };          ///< TID chirp end period (low-rate); <=0 => tid_period_s.
        // --- Optional pulse-train TID (0 defaults keep the legacy continuous chirp) ---
        int    tid_pulse_count{ 0 };             ///< #raised-cosine TID bumps across the window (0 = continuous).
        double tid_pulse_duration_s{ 600.0 };    ///< Each TID bump's width (s); ~600 = 10 min.
        double tid_fast_period_s{ 0.0 };         ///< Rapid carrier period (s) for tid_fast_sat only; ~60 = Nyquist of 30 s data.
        /// Explicit satellites that carry the TID overlay (e.g. {"E13","G18","G23"}).
        /// Non-empty overrides the storm_active_fraction hash: exactly these PRNs are
        /// disturbed, all others stay on the pure GIM/quiet background.  Empty = legacy hash.
        std::vector<std::string> tid_sats{};
        std::string tid_fast_sat{};              ///< One PRN (⊂ tid_sats) given the tid_fast_period_s flicker.

        /// A fully explicit list of TID pulses (the richest overlay control, used by the
        /// hero case).  When non-empty it supersedes tid_pulse_count / tid_sats / round-robin:
        /// each pulse is a positive-definite raised-cosine bump on one satellite, with its
        /// own centre, amplitude, carrier period and width, so satellites can have 1-3 bursts
        /// of different sizes/rates that may overlap and can ramp up as the storm intensifies.
        /// The set of disturbed satellites (for storm_gain / scintillation) is the union of
        /// pulse.sat.  Amplitude is vertical TECU, mapped to slant by the standard obliquity
        /// only (the sed_elev_exp emphasis is not applied to explicit pulses).
        struct TidPulse {
            std::string sat;                     ///< PRN this pulse is on (e.g. "G23").
            double center_tow_s{ 0.0 };          ///< Bump centre, GPS time-of-week (s).
            double amp_tecu{ 0.0 };              ///< Vertical amplitude (TECU).
            double period_s{ 0.0 };              ///< Carrier period inside the bump (s); <=0 => smooth bump.
            double duration_s{ 600.0 };          ///< Bump full width (s); ~600 = 10 min.
        };
        std::vector<TidPulse> tid_pulses{};
        /// Treat the SED blob as a bulk field: every ray sees it according to its own
        /// pierce point, not according to the storm-gain hash (which explicit tid_pulses
        /// pin to the pulse satellites alone).  false = legacy gating, byte-identical.
        bool sed_bulk{ false };
        // Epoch-to-epoch ionospheric turbulence on a few rays (see
        // IonoDisturbance::turbVtec).  It is the only non-smooth term in the model, and
        // therefore the only one a geometry-free QC plot reads as activity: the GIM
        // background and the raised-cosine pulses are differentiable, so a 120 TECU arc
        // without turbulence still looks quiet in gnss-qc-lite.  Calibrated against ADIS
        // (equatorial, Gannon storm): |GF rate| p50 ~0.02, p95 ~0.21, p99 ~0.6 cycles.
        // Windowed by the turb_window_* fractions of the arc so the roughness is
        // episodic (post-sunset), matching that reference rather than running flat all day.
        std::vector<std::string> turb_sats{};    ///< PRNs carrying turbulence ({} = off).
        double turb_sigma_tecu{ 0.0 };           ///< Vertical 1-sigma (TECU) at the window peak; must be >= 0.
        double turb_step_s{ 0.0 };               ///< Epoch spacing (s) for the correlation; <=0 => the epoch interval.
        double turb_window_start_frac{ 0.0 };    ///< Turbulence window start, fraction of the arc.
        double turb_window_duration_frac{ 0.0 }; ///< Turbulence window length, fraction (<=0 => whole arc).
        double sed_elev_exp{ 0.0 };              ///< Extra low-elev emphasis: slant *= obliquity^this (0 = none).
        bool   tid_positive{ false };            ///< TID as a positive-definite pulse (no negative I_L1); false = bipolar sin.
        double storm_active_fraction{ 1.0 };     ///< Fraction of sats that get the storm; the rest stay on quiet background (1 = all).
        // Thin-shell mapping + scintillation low-elevation floor.
        double shell_height_km{ 350.0 };         ///< Ionospheric thin-shell height (km); sets IPP + obliquity.
        double scint_elev_clamp_deg{ 10.0 };     ///< Low-elevation floor for the scintillation envelope (deg).
        /// Source of the quiet background STEC the storm is layered on top of:
        ///   "klobuchar" (default): back the slant STEC out of the reused broadcast
        ///       Klobuchar L1 delay, so a single-frequency student's own Klobuchar
        ///       correction cancels it exactly and only the storm remains;
        ///   "gim": sample the real CODE/IGS global ionosphere map (IONEX) for the
        ///       fixture date.  Klobuchar then removes only part of it, leaving the
        ///       genuine (GIM - Klobuchar) model residual in the single-frequency data;
        ///   "analytic": quiet_vtec_tecu / sin(el) placeholder (no broadcast field).
        /// The dual-frequency ionosphere-free combination cancels the first-order term
        /// for all three (one slant STEC scaled by 1/f^2 per band), so the graded PPP
        /// loop is unaffected by this choice.  The loader rejects any other value.
        std::string background_source{ "klobuchar" };
        /// IONEX path for background_source="gim"; empty => derive from SimConfig::date
        /// as <internal_root>/data/<YYYY-DOY>/products/COD0OPSFIN_<YYYYDOY>0000_01D_01H_GIM.INX.
        std::string gim_path{};
    } iono;

    /**
     * @brief One scripted satellite-clock step at a known epoch (a G-B event).
     *
     * A signed jump in one satellite's clock, held for the remainder of the arc
     * (a step, not an impulse).  This is the event class the Q3 temporal-anchor
     * guard is meant to catch, generated on demand with an exactly known
     * magnitude, which field data cannot provide.  Each event needs a non-zero
     * @c step_m or @c ramp_mps; the loader rejects an event that injects nothing.
     */
    struct ScriptedSatClockStep {
        std::string sat;         ///< "G05"; ^[GE]\d\d$.
        int    epoch{ 0 };       ///< 1-based epoch index the step is applied from (inclusive); [1, n_epochs].
        double step_m{ 0.0 };    ///< Signed jump in metres of range.
        double ramp_mps{ 0.0 };  ///< Optional additional ramp (m/s) accumulating after @c epoch.
    };

    /**
     * @brief Synthetic satellite clock error, added on top of the precise CLK product.
     *
     * Purpose: the sim injects the precise CLK product as the satellite clock, and the
     * network producer emits its clock as a correction to that same resolved product
     * (the S line's @c apriori_id is the SP3+CLK basename pair).  A-priori and truth
     * are therefore the same object, so with no injected error the estimable clock
     * truth is identically zero: a valid null test but not a measurement, and no
     * clock event can be exercised.  This channel supplies both.
     *
     * Determinism: every value is a pure function of (satellite, time), because a
     * network is synthesized as N independent processes that must agree bit-for-bit
     * on the satellite side.  The error is therefore pre-generated on the epoch grid
     * from a per-satellite hash-seeded stream and linearly interpolated to the
     * (station-dependent) emission time, never drawn lazily inside the satellite
     * loop.  A lazy draw would also be sky-dependent, because @c forwardPseudorange is
     * called before the elevation mask, so the draw count would change with
     * visibility and break storm-on/off byte-identity.
     *
     * Injection site: the forward model's clock lookup only, never
     * @c EphemerisProvider::clock() itself.  That shared entry point also feeds the
     * internal SPP and the Part-B answer key; perturbing it would perturb both sides
     * of the 1 mm noise-free self-consistency gate, which would then pass however
     * wrong the arc is.  Truth is written to @c truth_satclock.csv.
     *
     * All defaults are 0/empty, so an omitted section reproduces the existing datasets
     * byte-for-byte.
     */
    struct SatClock {
        /// Per-satellite constant offset, 1-sigma (m).  Drawn once per satellite.
        double bias0_sigma_m{ 0.0 };
        /// Per-satellite linear rate, 1-sigma (m/s).  Drawn once per satellite.
        double rate_sigma_mps{ 0.0 };
        /// Random-walk process-noise spectral density (m^2/s), in the units of the
        /// producer's `filter.process_noise.sat_clock_q`, so injected truth and the
        /// estimator's stochastic model are directly comparable.  The measured physical
        /// value is q_phys ~ 3e-8 (about 0.9 mm per 30 s epoch); the armed carry q sits
        /// far above it (card Q3).
        double rw_q_m2ps{ 0.0 };
        std::uint64_t seed{ 20240315ULL ^ 0x5A7C10CBull }; ///< Dedicated stream (must be >= 0).
        /// Deterministic hand-authored clock steps layered on top (G-B events).
        std::vector<ScriptedSatClockStep> steps;
    } sat_clock;

    /// Fixture date.  Selects which bundled single-day product set (SP3 / CLK / nav /
    /// GIM) the synth runs against, and therefore the GPS week and the day-start tow
    /// that Trajectory::start_tow_s is measured against.  Defaults to DOY 129 so every
    /// pre-existing scenario file reproduces byte-identically.
    ///
    /// This one knob drives the emitted RINEX filename, the fixture directory, the
    /// product-day edge guard and the scenario_summary date together.  They must move
    /// together: if start_tow_s moved to another day while the filename still named
    /// the old one, the processor would resolve the wrong day's orbits against the new
    /// epochs, a multi-day extrapolation that raises no error and looks like a physics
    /// bug.
    struct Date {
        int year{ 2024 };   ///< Calendar year of the fixture products.
        int doy{ 129 };     ///< Day of year (1-366).
    } date;

    /// Truth trajectory (a subset of TrajectoryConfig; kinematic flag comes from
    /// the synth-static/synth-kinematic subcommand, not the file).
    struct Trajectory {
        double interval_s{ 30.0 };          ///< Epoch spacing (s).
        int    n_epochs{ 120 };             ///< Number of epochs (arc length).
        /// Start time-of-week (s) of the first epoch, in [0, 604800).  It is relative to
        /// the GPS week, so it must lie inside the day selected by SimConfig::date: that
        /// day spans tow [dow*86400, (dow+1)*86400] of its GPS week (DOY 129 = week 2313
        /// dow 3 => [259200, 345600]; DOY 131 = week 2313 dow 5 => [432000, 518400]).
        /// The bundled precise products are single-day files, and an arc that reaches
        /// either edge cannot be centred in the Lagrange orbit window and loses its
        /// trailing clock nodes.  The whole arc (start + n_epochs*interval) belongs
        /// comfortably inside that span: runSynth fails when the arc leaves the product
        /// day and warns when it comes within 1800 s of either edge.
        double start_tow_s{ 302400.0 };
        double cruise_speed_mps{ 4.5 };     ///< Cruise speed (kinematic only).
        double lat0_deg{ -18.5 };           ///< Origin latitude (deg).
        double lon0_deg{ -178.2 };          ///< Origin longitude (deg).
        double h0_m{ 0.0 };                 ///< Origin ellipsoidal height (m).
        double heading0_deg{ 35.0 };        ///< Mean heading (deg).
        double heading_amp_deg{ 60.0 };     ///< Heading weave amplitude (deg, kinematic).
        double heading_period_s{ 1200.0 };  ///< Heading weave period (s, kinematic).
        /// Seconds of exact station-keeping before any motion starts (0 = off).
        /// The vessel holds the start position, then ramps up and still reaches
        /// the destination at the final epoch.  Used by the exam hero so the arc
        /// opens with a genuinely static segment students can solve as a fixed
        /// point (Parts B and C), which no other part of the record offers.
        double static_prefix_s{ 0.0 };
        double heave_amp_m{ 1.0 };          ///< Sea-state heave amplitude (m, kinematic).
        double heave_period_s{ 8.0 };       ///< Sea-state heave period (s, kinematic).
        double clock_bias0_s{ 1.0e-4 };     ///< Initial receiver clock bias (s).
        /// Receiver clock drift (s/s): the oscillator's fractional frequency offset.
        /// 0 (the default) = the historical constant bias.  A real receiver clock is
        /// dominated by this linear ramp: a free-running TCXO at ~1e-9 s/s moves the
        /// clock ~0.3 m of range per second, i.e. ~9 m between 30 s epochs, so a batch
        /// model that shares one clock parameter across all epochs is a modelling error
        /// (this is what makes per-epoch clock indexing necessary rather than merely
        /// conventional).  Published per epoch in truth_clock.csv.
        double clock_drift_sps{ 0.0 };
        /// Receiver clock random walk: process-noise spectral density in m^2/s, the
        /// same units a PPP filter's `process_noise.clock_bias_q` uses, so the injected
        /// truth and an estimator's stochastic model can be compared directly.  The
        /// per-epoch increment is drawn N(0, sqrt(q * interval_s)) metres of range and
        /// accumulated (white frequency noise -> random-walk phase, the standard quartz
        /// clock model).  0 (the default) = a perfectly deterministic clock, byte-
        /// identical to the pre-existing datasets.  Must be >= 0.
        ///
        /// A linear drift alone is not enough: a pure ramp is exactly representable by
        /// two parameters (bias + drift), so a student could still avoid indexing the
        /// clock per epoch.  A real oscillator wanders; q = 3e-3 m^2/s is ~0.3 m per 30 s
        /// epoch (sigma_y(30 s) ~ 3e-11, an ordinary geodetic-receiver internal
        /// oscillator), which is above the code noise floor of an epoch's clock estimate
        /// and therefore cannot be absorbed by any low-order polynomial.
        double clock_rw_q_m2ps{ 0.0 };
        /// Dedicated RNG seed for the clock random walk.  A private stream: it cannot
        /// perturb the noise/multipath/slip/tropo draws, so storm-on/off control and the
        /// published answer keys survive a re-seed of the clock alone (must be >= 0).
        std::uint64_t clock_seed{ 20240315ULL ^ 0xC10CC0DEull };
        // Optional "landfall" mode (both dest_* finite => drift start->dest with a
        // terminal grounding; NaN default keeps the analytic model byte-identical).
        double dest_lat_deg{ std::numeric_limits<double>::quiet_NaN() };  ///< Grounding latitude (deg).
        double dest_lon_deg{ std::numeric_limits<double>::quiet_NaN() };  ///< Grounding longitude (deg).
        double landfall_decel_frac{ 0.30 };     ///< Arc fraction spent easing to grounding.
        double landfall_terminal_frac{ 0.0 };   ///< Terminal speed as a fraction of Vmax at grounding (0 = dead stop).
        double approach_weave_amp_m{ 2000.0 };  ///< Lateral meander amplitude (m; vanishes at both ends).
        double approach_weave_period_s{ 2400.0 };///< Lateral meander period (s).
        /// Optional intermediate waypoints (lat_deg, lon_deg): landfall path becomes
        /// a smooth spline start -> waypoints... -> dest.  Empty => straight drift.
        std::vector<std::array<double, 2>> waypoints;
    } trajectory;

    /// RINEX header realism / leak knobs (SPEC_student_handout.md R2).  Every default
    /// reproduces today's header byte-for-byte: an absent @c rinex section, or one whose
    /// keys are all defaults, emits an identical file (the house byte-identity gate).
    ///
    /// APPROX POSITION XYZ is, by default, the exact truth start (row 1 of
    /// truth_position.csv), which discloses the answer in a student handout.
    /// "quantized" mode rounds the disclosed last-known position to a coarse lat/lon
    /// grid and the height to the nearest metre, deterministically and with no RNG draw
    /// (it must never perturb a shared stream).  Because this single value also anchors
    /// the processor's frozen ocean-loading point, @c truth_otl.csv's x0 and the
    /// internal SPP seed (runSynth computes it once and feeds all of them), quantizing
    /// it moves them together.  That is intended: the sim and the processor must freeze
    /// at the same point the RINEX header discloses (R2.2).
    struct Rinex {
        /// "truth" (default, byte-identical) = APPROX POSITION is the exact truth start.
        /// "quantized" = truth start ECEF -> geodetic, round lat/lon to
        /// @c approx_quantize_deg and h to the nearest metre, convert back to ECEF.
        /// (h is not forced to 0: a floating vessel's ellipsoidal height is the geoid
        /// undulation, ~+48 m at the hero LKP.)  Any other value is rejected.
        std::string approx_position_mode{ "truth" };
        double approx_quantize_deg{ 0.001 };   ///< Lat/lon rounding grid (deg); ~110 m at the equator.
        /// Deliberate ENU error (m) added to the header APPROX POSITION only, the value a
        /// receiver or analyst reads as an a priori.  Applied after quantization and after
        /// the ocean-loading reference is frozen, so the injected physics is untouched and
        /// only the disclosed a priori moves.  Deterministic (no RNG).
        /// A header a priori is not a fix, and RINEX does not promise it is close; a
        /// large value here forces a solver to iterate its least squares from the header
        /// seed instead of relying on a near-truth start (the single-iteration residual
        /// scales as |offset|^2 / range, so ~25 km leaves tens of metres).
        /// {0,0,0} = off (byte-identical to the historical behaviour).
        std::array<double, 3> approx_offset_enu_m{ { 0.0, 0.0, 0.0 } };
        /// Header cosmetics.  Empty = keep the writer's current value (byte-identical).
        std::string pgm{};        ///< PGM field (cols 0-19), e.g. "SARV-LOGGER 1.0".
        std::string run_by{};     ///< RUN BY field (cols 20-39).
        std::string observer{};   ///< OBSERVER field (cols 0-19).
        std::string agency{};     ///< AGENCY field (cols 20-59).
        /// "build" (default) = the fixed build stamp today's writer emits.
        /// "collection_end" = UTC datetime of the last epoch (GPS -> UTC via
        /// @c gps_utc_leap_s), formatted "YYYYMMDD HHMMSS UTC" like the DATE field.
        std::string file_date_mode{ "build" };
        int gps_utc_leap_s{ 18 };  ///< GPS-UTC leap offset for "collection_end".
    } rinex;
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
 * @return true on success; false (with @p err set) if the file is missing, is
 *         not parseable, or holds a value that fails validation.
 */
bool loadSimConfig(const std::filesystem::path& path, SimConfig& cfg,
                   std::string* err = nullptr);

/**
 * @brief Derive a station's RNG salt from its name.
 *
 * A pure hash, so the same roster always produces the same per-station
 * realization and a re-run is reproducible without recording the salts.  The
 * value fits in 53 bits, so it survives the round trip through a JSON number.
 * Never returns 0: 0 is the sentinel meaning "legacy single-station, leave every
 * seed untouched", and a station that landed on it would share the unsalted
 * realization with any other station that did the same.
 */
std::uint64_t stationSeedSalt(const std::string& name);

/**
 * @brief Write a per-station child config for the `synth-network` fan-out.
 *
 * Reads @p base_config verbatim (so every knob the user set is preserved
 * exactly), then overrides only what makes this invocation a specific station:
 * the station identity and salt, the trajectory origin, and the absolute storm
 * anchor.  A non-empty per-station @c rx_code_bias / @c rx_phase_bias list
 * replaces the matching @c biases list, a roster entry's @c scripted_slips
 * array replaces @c cycle_slips.scripted, and a finite per-station
 * @c phase_colour_sigma_zen_m / @c phase_colour_tau_s replaces
 * @c phase_colour.sigma_zen_m / @c phase_colour.tau_s (an absent override leaves
 * the network-level value in the child).  It also strips @c network_stations so
 * the child cannot recurse.
 *
 * The absolute anchor is the reason this helper exists rather than the driver
 * hand-editing text: with the legacy relative anchor each child would re-derive
 * the ionospheric blob centre from its own origin and therefore sit exactly at
 * the centre, so a travelling disturbance would arrive simultaneously and
 * identically at every station.
 *
 * @param base_config     Config the network run was launched with; may be empty
 *                        (then the child starts from SimConfig defaults).
 * @param st              The station to write a config for.
 * @param anchor_lat_deg  Absolute storm-centre latitude (deg).
 * @param anchor_lon_deg  Absolute storm-centre longitude (deg).
 * @param out_path        Child config path to write.
 * @param err             Optional: set to a human-readable reason on failure.
 */
bool writeStationConfig(const std::filesystem::path& base_config,
                        const NetworkStation& st,
                        double anchor_lat_deg, double anchor_lon_deg,
                        const std::filesystem::path& out_path,
                        std::string* err = nullptr);

} // namespace sim
} // namespace GnssProcessor
