#pragma once

/**
 * @file StationInfo.hpp
 * @brief Plain-data model for a single station's metadata, as carried by an
 *        NGS Station Information File (SIF).
 *
 * A SIF (e.g. @c igs20_SIF.json, @c ncn20_SIF.json) is a JSON bundle of
 * per-station GNSS metadata produced by NGS in the same spirit as the
 * M-PAGES @c StationInfoFile.  Each station record carries:
 *
 *  - Identity: 9-char ID (@c SSSS00CCC), DOMES number, description.
 *  - Ocean tidal loading (OTL): full FES2014b 11-constituent
 *    amplitude/phase set in the SIF @b Up/East/North convention (all three
 *    components, unlike the radial-only EOT20 grid path).
 *  - Ocean pole tide (OPT): the six Desai (2002) load coefficients
 *    (real/imag for North, East, Up), stored here in the order
 *    @c GnssCorrections expects for @c opt_coeffs_.
 *  - Time-windowed antenna history (type, serial, ARP-minus-GRP ENU
 *    eccentricity, mounting orientation, @c DO_NOT_USE).
 *  - Time-windowed receiver history.
 *  - Time-windowed @c OCCUPATION solutions: ITRF coordinates + velocity +
 *    sigmas + the 6x6 coordinate/velocity covariance, each referred to its
 *    own record's @c REFERENCE_EPOCH (typically 2020.0; a handful of NCN
 *    records carry 2010.0), falling back to the SIF @c METADATA epoch when a
 *    record has none, in the SIF @c REFERENCE_FRAME (typically ITRF2020).
 *  - Nonlinear station motion: periodic (annual/semiannual XYZ cos/sin)
 *    terms and post-seismic deformation (log/exp relaxation) blocks.
 *
 * @c StationInfo is a passive container.  Parsing lives in
 * @c StationInfoFile; consumers (ocean-loading models, the a-priori
 * coordinate path, the PPP-RTK network datum) read from it.
 *
 * All units are SI as stored here: amplitudes/coordinates/displacements in
 * metres, phases/orientation in degrees, velocities in metres/year,
 * covariances in metre^2 / metre^2-per-year / metre^2-per-year^2,
 * decay times and periods in years.  The loader performs the mm->m and
 * mm/y->m/y conversions declared in the SIF @c METADATA block, so downstream
 * code sees SI units only.
 */

#include <Eigen/Dense>
#include <array>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief One ocean-tide constituent in the SIF Up/East/North convention.
     *
     * Amplitudes in metres, Greenwich phase lags in degrees.  The SIF stores
     * Up/East/North; @c StationInfoFile keeps them verbatim here and the
     * conversion to the @c OceanTideModel Up/West/South lag convention is done
     * at the point of use.
     */
    struct SifOtlConstituent {
        double up_amp{ 0.0 };      ///< Radial (Up) amplitude [m].
        double east_amp{ 0.0 };    ///< East amplitude [m].
        double north_amp{ 0.0 };   ///< North amplitude [m].
        double up_phase{ 0.0 };    ///< Radial (Up) Greenwich phase lag [deg].
        double east_phase{ 0.0 };  ///< East Greenwich phase lag [deg].
        double north_phase{ 0.0 }; ///< North Greenwich phase lag [deg].
    };

    /**
     * @brief Ocean pole tide (Desai 2002) per-station load coefficients.
     *
     * Field order intentionally mirrors @c GnssCorrections::opt_coeffs_
     * (@c [u_n^R, u_n^I, u_e^R, u_e^I, u_r^R, u_r^I], radial == Up): this is the
     * same convention as the @c opoleloadcoefcmcor.txt grid the SIF values were
     * sampled from, verified to agree to interpolation precision.  Units: metres.
     */
    struct SifOceanPoleTide {
        double north_real{ 0.0 }; ///< u_n^R [m].
        double north_imag{ 0.0 }; ///< u_n^I [m].
        double east_real{ 0.0 };  ///< u_e^R [m].
        double east_imag{ 0.0 };  ///< u_e^I [m].
        double up_real{ 0.0 };    ///< u_r^R (radial) [m].
        double up_imag{ 0.0 };    ///< u_r^I (radial) [m].
        bool valid{ false };      ///< True once populated from a SIF record.
    };

    /**
     * @brief Time-windowed antenna installation.
     *
     * Window bounds are decimal years (e.g. 2012.07).  An open-ended SIF END of
     * "9999-..." is stored as the sentinel 9999.0 (the loader maps any year
     * >= 9000 to it), so it compares later than any real epoch.
     */
    struct SifAntennaWindow {
        double start_year{ 0.0 };  ///< Window start [decimal year].
        double end_year{ 0.0 };    ///< Window end   [decimal year].
        std::string type;          ///< IGS antenna+radome code, e.g. "TRM57971.00     NONE".
        std::string serial;        ///< Antenna serial number.
        Eigen::Vector3d arp_minus_grp_enu{ 0.0, 0.0, 0.0 }; ///< ARP - GRP eccentricity, ENU (E,N,U) [m]; the RINEX ANTENNA: DELTA H/E/N in another order (see @c reduceSifCoordinateToArp).
        double orientation_deg{ 0.0 }; ///< Antenna mounting orientation, clockwise from true north [deg].
        bool do_not_use{ false };  ///< True if this window is flagged unusable.
    };

    /**
     * @brief Time-windowed receiver installation.
     */
    struct SifReceiverWindow {
        double start_year{ 0.0 };       ///< Window start [decimal year].
        double end_year{ 0.0 };         ///< Window end   [decimal year].
        std::string type;               ///< Receiver hardware type string.
        std::string firmware;           ///< Firmware version string.
        std::string satellite_systems;  ///< Tracked systems, e.g. "GREC".
        bool do_not_use{ false };       ///< True if this window is flagged unusable.
    };

    /**
     * @brief Time-windowed ITRF coordinate solution (one @c OCCUPATION record).
     *
     * Coordinates and velocity are referred to the SIF @c REFERENCE_FRAME at
     * this record's own @c REFERENCE_EPOCH (@c reference_epoch_year).  The
     * loader takes that from the OCCUPATION record and falls back to the SIF
     * @c METADATA epoch (@c StationInfo::reference_epoch_year) only when the
     * record has no numeric @c REFERENCE_EPOCH.  The epochs differ in
     * practice: four NCN records (AZAZ00USA, AZOR00USA, AZPL00USA, FLSI00USA)
     * are referred to 2010.0 while the METADATA epoch is 2020.0, so
     * propagating them from the METADATA epoch misplaces them by ten years of
     * velocity (11 to 15 cm).  All four are flagged DO_NOT_USE in the shipped
     * NCN SIF, so occupation selection skips them and no resolved coordinate
     * changes today; the per-record epoch applies once such a record is
     * usable.  The 6x6 covariance is ordered
     * [X, Y, Z, Vx, Vy, Vz] and is referred to the same epoch.
     */
    struct SifOccupation {
        double start_year{ 0.0 };  ///< Window start [decimal year].
        double end_year{ 0.0 };    ///< Window end   [decimal year].
        double reference_epoch_year{ 2020.0 };           ///< Epoch of @c coordinates, @c velocity and @c covariance [decimal year].
        Eigen::Vector3d coordinates{ 0.0, 0.0, 0.0 };    ///< ECEF coordinates of the ground reference point (monument, not the ARP) at @c reference_epoch_year [m].
        Eigen::Vector3d coord_sigmas{ 0.0, 0.0, 0.0 };   ///< Coordinate sigmas [m].
        Eigen::Vector3d velocity{ 0.0, 0.0, 0.0 };       ///< ECEF velocity [m/yr].
        Eigen::Vector3d velocity_sigmas{ 0.0, 0.0, 0.0 };///< Velocity sigmas [m/yr].
        Eigen::Matrix<double, 6, 6> covariance =
            Eigen::Matrix<double, 6, 6>::Zero();         ///< [X Y Z Vx Vy Vz] covariance [m^2, m^2/yr, m^2/yr^2].
        bool has_covariance{ false };                    ///< True if @c covariance was populated.
        bool do_not_use{ false };                        ///< True if this solution is flagged unusable.
    };

    /**
     * @brief One periodic (seasonal) frequency for the nonlinear motion model.
     *
     * Displacement contribution per component:
     *   d(t) = COS * cos(2*pi*(t - t_ref)/PERIOD) + SIN * sin(2*pi*(t - t_ref)/PERIOD)
     * Amplitudes in metres, period in years.
     */
    struct SifPeriodicFreq {
        double period_yr{ 1.0 }; ///< Period [yr] (1.0 = annual, 0.5 = semiannual).
        double x_cos{ 0.0 };     ///< X cosine amplitude [m].
        double x_sin{ 0.0 };     ///< X sine amplitude [m].
        double y_cos{ 0.0 };     ///< Y cosine amplitude [m].
        double y_sin{ 0.0 };     ///< Y sine amplitude [m].
        double z_cos{ 0.0 };     ///< Z cosine amplitude [m].
        double z_sin{ 0.0 };     ///< Z sine amplitude [m].
    };

    /**
     * @brief Time-windowed block of periodic frequencies.
     *
     * The window says where the block was fitted, not where it may be
     * evaluated: @c resolveStationCoordinate continues the first block before
     * its window and the last block after it, and carries the block that ended
     * last across a gap (see @c SifPeriodicReach).  The blocks of a station
     * are kept in file order, which is not guaranteed to be chronological.
     */
    struct SifPeriodicBlock {
        double start_year{ 0.0 };           ///< Window start [decimal year].
        double end_year{ 0.0 };             ///< Window end   [decimal year].
        double reference_epoch_year{ 0.0 }; ///< Phase reference epoch [decimal year].
        std::vector<SifPeriodicFreq> freqs; ///< Frequencies (typically annual + semiannual).
    };

    /**
     * @brief Post-seismic deformation parameters for one local component (E/N/U).
     *
     * Displacement = sum_i ALOG_i * log(1 + dt/TLOG_i) + sum_k AEXP_k * (1 - exp(-dt/TEXP_k)),
     * with dt = (t - event) in years.  Amplitudes in metres, decay times in years.
     * This is the displacement accumulated since the event; the coordinate
     * function applies its change since the occupation's reference epoch (see
     * @c resolveStationCoordinate), because the reference coordinate already
     * holds the displacement accumulated by then.
     */
    struct SifPsdComponent {
        std::vector<double> aexp;  ///< Exponential amplitudes [m].
        std::vector<double> texp;  ///< Exponential decay times [yr].
        std::vector<double> alog;  ///< Logarithmic amplitudes [m].
        std::vector<double> tlog;  ///< Logarithmic decay times [yr].
    };

    /**
     * @brief Time-anchored post-seismic deformation block (one earthquake).
     */
    struct SifPsdBlock {
        double event_year{ 0.0 }; ///< Earthquake epoch [decimal year].
        SifPsdComponent east;     ///< East component.
        SifPsdComponent north;    ///< North component.
        SifPsdComponent up;       ///< Up component.
    };

    /**
     * @brief Resolved per-station record extracted from a SIF.
     */
    struct StationInfo {
        std::string id;          ///< 9-char IGS long name, e.g. "ABMF00GLP".
        std::string domes;       ///< DOMES number, e.g. "97103M001".
        std::string description; ///< Free-text site description.
        Eigen::Vector3d approximate_coordinates{ 0.0, 0.0, 0.0 }; ///< A-priori ECEF [m].
        std::string reference_frame;       ///< e.g. "ITRF2020" (from SIF METADATA).
        double reference_epoch_year{ 2020.0 }; ///< SIF METADATA reference epoch [decimal year]; the fallback for an occupation with no REFERENCE_EPOCH of its own (see @c SifOccupation::reference_epoch_year).

        std::array<SifOtlConstituent, 11> otl{}; ///< OTL, BLQ order: M2 S2 N2 K2 K1 O1 P1 Q1 Mf Mm Ssa.
        bool has_otl{ false };                    ///< True if @c otl was populated.
        SifOceanPoleTide opt;                     ///< Ocean pole tide load coefficients.

        std::vector<SifAntennaWindow> antennas;   ///< Antenna history (chronological).
        std::vector<SifReceiverWindow> receivers; ///< Receiver history (chronological).
        std::vector<SifOccupation> occupations;   ///< ITRF coordinate solutions (chronological).
        std::vector<SifPeriodicBlock> periodic;   ///< Periodic (seasonal) motion blocks.
        std::vector<SifPsdBlock> psd;             ///< Post-seismic deformation blocks.
    };

    /**
     * @brief Names of the 11 OTL constituents in BLQ order.
     *
     * Same order as @c OceanTideModel::OTL_CONSTITUENT_NAMES, but spelled as the
     * SIF keys (upper-case MF, MM, SSA where OceanTideModel uses Mf, Mm, Ssa).
     * The loader looks up each SIF constituent by these names and stores it in
     * the matching slot of @c StationInfo::otl.
     */
    static constexpr std::array<const char*, 11> SIF_OTL_CONSTITUENTS = {
        "M2", "S2", "N2", "K2", "K1", "O1", "P1", "Q1", "MF", "MM", "SSA"
    };

} // namespace GnssProcessor
