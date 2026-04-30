#include "model/Antenna.hpp"
#include "math/Coordinates.hpp"
#include "products/ProductCache.hpp"
#include <Eigen/Dense>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

    namespace {

        // ---- ATX nearest-frequency fallback ---------------------------------
        // For each requested ATX frequency key (e.g. "G05"), the chain lists
        // alternates ordered by *frequency proximity* — closest carrier
        // frequency first.  Used when an antenna entry's pco/pcv map doesn't
        // include the requested key (common for L5 / E5* / B2a on older
        // satellites and many receiver antennas published before triple-freq
        // tracking was widespread).  Cross-frequency PCO drift on the same
        // physical antenna is typically <1 cm; that is much better than the
        // few-cm vertical residual from leaving the correction at zero.
        //
        // Frequency ordering reference (MHz):
        //   GPS    L1=1575.42  L2=1227.60  L5=1176.45
        //   Gal    E1=1575.42  E6=1278.75  E5b=1207.14  E5(a+b)=1191.795  E5a=1176.45
        //   GLO    L1≈1602     L2≈1246     L3=1202.025
        //   BDS    B1C=1575.42 B1I=1561.098 B3I=1268.52 B2b=1207.14 B2(a+b)=1191.795 B2a=1176.45
        //   QZSS   L1=1575.42  L6=1278.75   L2=1227.60                              L5=1176.45
        //   SBAS   L1=1575.42                                                       L5=1176.45
        const std::map<std::string, std::vector<std::string>>& atxFallbackChain() {
            static const std::map<std::string, std::vector<std::string>> chain = {
                // GPS
                { "G01", { "G02", "G05" } },
                { "G02", { "G05", "G01" } },
                { "G05", { "G02", "G01" } },
                // Galileo
                { "E01", { "E06", "E08", "E07", "E05" } },
                { "E05", { "E08", "E07", "E06", "E01" } },
                { "E07", { "E08", "E05", "E06", "E01" } },
                { "E06", { "E07", "E08", "E05", "E01" } },
                { "E08", { "E07", "E05", "E06", "E01" } },
                // GLONASS
                { "R01", { "R02", "R03" } },
                { "R02", { "R03", "R01" } },
                { "R03", { "R02", "R01" } },
                // BeiDou
                { "C01", { "C02", "C06", "C08", "C07", "C05" } },
                { "C02", { "C01", "C06", "C08", "C07", "C05" } },
                { "C05", { "C08", "C07", "C06", "C01", "C02" } },
                { "C06", { "C07", "C08", "C05", "C02", "C01" } },
                { "C07", { "C08", "C05", "C06", "C02", "C01" } },
                { "C08", { "C07", "C05", "C06", "C02", "C01" } },
                // QZSS
                { "J01", { "J06", "J02", "J05" } },
                { "J02", { "J06", "J05", "J01" } },
                { "J05", { "J02", "J06", "J01" } },
                { "J06", { "J02", "J05", "J01" } },
                // SBAS
                { "S01", { "S05" } },
                { "S05", { "S01" } },
            };
            return chain;
        }

        /// Pick an effective ATX frequency key from @p entry_map.  Returns
        /// @p requested when present and the @p good predicate accepts the
        /// stored value; otherwise walks the per-system fallback chain
        /// (when @p enable_fallback is true) and returns the first
        /// alternate that satisfies the predicate.  Empty string when
        /// nothing matches — callers should treat that as "skip the
        /// correction" (legacy behavior).
        template <typename Map, typename GoodPred>
        std::string pickAtxFrequencyKey(const Map& entry_map,
                                         const std::string& requested,
                                         bool enable_fallback,
                                         GoodPred good) {
            if (requested.empty()) return {};
            auto it = entry_map.find(requested);
            if (it != entry_map.end() && good(it->second)) return requested;
            if (!enable_fallback) return {};
            const auto& chain = atxFallbackChain();
            auto cit = chain.find(requested);
            if (cit == chain.end()) return {};
            for (const auto& alt : cit->second) {
                auto ait = entry_map.find(alt);
                if (ait != entry_map.end() && good(ait->second)) return alt;
            }
            return {};
        }

    } // namespace

    /**
     * @file Antenna.cpp
     * @brief Implementation of Antenna corrections: PCO/PCV interpolation, phase wind-up,
     *        and antenna data loading from ATX products.
     */

    /**
     * @brief Construct an Antenna helper.
     *
     * Loads antenna models from products (ATX) via the provided ProductParser and
     * initializes internal helpers used to compute antenna corrections.
     *
     * @param config Configuration reference (read-only).
     * @param product_parser Product parser used to access auxiliary files.
     * @param log_mgr Log manager used for warnings/errors and tracing.
     * @param rinex_parser Reference to the RINEX parser for receiver metadata.
     */
    Antenna::Antenna(const GnssConfig& config, ProductParser& product_parser,
        LogManager& log_mgr, const RinexParser& rinex_parser)
        : log_manager_(log_mgr),
        config_(config),
        gnss_corrections_(log_mgr),
        rinex_parser_(rinex_parser),
        product_parser_(product_parser),
        ppp_utils_(log_mgr),
        planetary_(log_mgr),
        // Initialize reference from cache - NO COPY
        antennas_(ProductCache::getInstance().getAntennas())
    {
        // No need to call loadAntennaData() - we already have reference
        log_manager_.getLogger("antenna_model")->info(
            "Antenna init (zero-copy): {} types, rx={}",
            antennas_.size(), getReceiverAntennaType());

        // Dump every loaded antenna model at trace level for ATX comparison
        dumpAntennaModels();
    }

    /**
     * @brief Compute antenna correction (meters) for a satellite/receiver observation.
     *
     * This function computes and returns the combined antenna correction including:
     * - Satellite phase center offset (PCO) projected onto the satellite-to-receiver line-of-sight.
     * - Satellite phase center variation (PCV) interpolated from the antenna PCV grid.
     * - Receiver PCO/PCV corrections (subtracted from the measured range).
     * - Phase wind-up correction for carrier-phase observations.
     *
     * The implementation:
     * - Maps internal frequency labels to ATX frequency keys based on GNSS system.
     * - Computes a satellite attitude frame using yaw-steering (velocity + Sun direction).
     * - Matches antenna models by satellite identifier and by receiver type (with simple normalization).
     * - Safely handles missing data and logs warnings/traces for diagnostics.
     *
     * Sign convention:
     *   Satellite PCO: APC closer to receiver than CoM → shortens range → subtract
     *   Satellite PCV: sat_apc returns (pco - pcv), caller negates → net +pcv
     *   Receiver  PCO: APC offset from ARP (mostly up) → project onto LOS → add
     *   Receiver  PCV: rec_apc adds pcv to dot(pco,los) → add
     *   total correction = -sat_pco + sat_pcv + rx_pco + rx_pcv + wind_up
     *
     * @param satellite Satellite identifier string (e.g. "G01", "R05").
     * @param freq_type Frequency label used by processing (e.g. "L1", "C1").
     * @param system GNSS system enum for the satellite.
     * @param rx_pos Receiver ECEF position (meters).
     * @param sat_pos Satellite ECEF position (meters).
     * @param sat_vel Satellite ECEF velocity (meters/sec).
     * @param elevation Receiver-measured elevation angle (degrees).
     * @param azimuth Receiver-measured azimuth angle (degrees).
     * @param rx_antenna_type Receiver antenna model/type string (from RINEX header or configuration).
     * @param time Epoch time at which the correction is computed.
     * @return Antenna correction in meters. Positive values follow the convention used by the caller.
     */
    double Antenna::getAntennaCorrection(const std::string& satellite,
        const std::string& freq_type,
        System system,
        const Eigen::Vector3d& rx_pos,
        const Eigen::Vector3d& sat_pos,
        const Eigen::Vector3d& sat_vel,
        double elevation,
        double azimuth,
        const std::string& rx_antenna_type,
        const GnssTime& time,
        const Eigen::Vector3d& sun_pos_hint) const {

        double correction = 0.0;
        bool is_phase = freq_type[0] == 'L';
        double wavelength = is_phase ? ppp_utils_.getWavelength(system, freq_type, satellite) : 0.0;

        if (!rx_pos.allFinite() || !sat_pos.allFinite() || !sat_vel.allFinite() || !std::isfinite(elevation) || !std::isfinite(azimuth)) {
            log_manager_.getLogger("antenna")->warn("Invalid inputs for antenna correction: satellite={}, freq_type={}, "
                "rx_pos=({}, {}, {}), sat_pos=({}, {}, {}), sat_vel=({}, {}, {}), elevation={:.3f}, azimuth={:.3f}",
                satellite, freq_type, rx_pos.x(), rx_pos.y(), rx_pos.z(),
                sat_pos.x(), sat_pos.y(), sat_pos.z(),
                sat_vel.x(), sat_vel.y(), sat_vel.z(),
                elevation, azimuth);
            return 0.0;
        }
        if (elevation < 0.0 || elevation > 90.0 || azimuth < 0.0 || azimuth >= 360.0) {
            log_manager_.getLogger("antenna")->warn("Out-of-range elevation ({:.3f}) or azimuth ({:.3f}) for satellite {}, freq_type={}",
                elevation, azimuth, satellite, freq_type);
            return 0.0;
        }

        std::string atx_freq;
        if (system == System::GPS) {
            if (freq_type.find("C1") == 0 || freq_type.find("L1") == 0) { atx_freq = "G01"; }
            else if (freq_type.find("C2") == 0 || freq_type.find("L2") == 0) { atx_freq = "G02"; }
            else if (freq_type.find("C5") == 0 || freq_type.find("L5") == 0) { atx_freq = "G05"; }
        }
        else if (system == System::Galileo) {
            if (freq_type.find("C1") == 0 || freq_type.find("L1") == 0) { atx_freq = "E01"; }
            else if (freq_type.find("C5") == 0 || freq_type.find("L5") == 0) { atx_freq = "E05"; }
            else if (freq_type.find("C7") == 0 || freq_type.find("L7") == 0) { atx_freq = "E07"; }
            else if (freq_type.find("C6") == 0 || freq_type.find("L6") == 0) { atx_freq = "E06"; }
            else if (freq_type.find("C8") == 0 || freq_type.find("L8") == 0) { atx_freq = "E08"; }
        }
        else if (system == System::GLONASS) {
            if (freq_type.find("C1") == 0 || freq_type.find("L1") == 0) { atx_freq = "R01"; }
            else if (freq_type.find("C2") == 0 || freq_type.find("L2") == 0) { atx_freq = "R02"; }
            else if (freq_type.find("C3") == 0 || freq_type.find("L3") == 0) { atx_freq = "R03"; }
        }
        else if (system == System::BeiDou) {
            if (freq_type.find("C2") == 0 || freq_type.find("L2") == 0) { atx_freq = "C02"; }
            else if (freq_type.find("C7") == 0 || freq_type.find("L7") == 0) { atx_freq = "C07"; }
            else if (freq_type.find("C6") == 0 || freq_type.find("L6") == 0) { atx_freq = "C06"; }
            else if (freq_type.find("C1") == 0 || freq_type.find("L1") == 0) { atx_freq = "C01"; }
            else if (freq_type.find("C5") == 0 || freq_type.find("L5") == 0) { atx_freq = "C05"; }
            else if (freq_type.find("C8") == 0 || freq_type.find("L8") == 0) { atx_freq = "C08"; }
        }
        else if (system == System::QZSS) {
            if (freq_type.find("C1") == 0 || freq_type.find("L1") == 0) { atx_freq = "J01"; }
            else if (freq_type.find("C2") == 0 || freq_type.find("L2") == 0) { atx_freq = "J02"; }
            else if (freq_type.find("C5") == 0 || freq_type.find("L5") == 0) { atx_freq = "J05"; }
            else if (freq_type.find("C6") == 0 || freq_type.find("L6") == 0) { atx_freq = "J06"; }
        }
        else if (system == System::SBAS) {
            if (freq_type.find("C1") == 0 || freq_type.find("L1") == 0) { atx_freq = "S01"; }
            else if (freq_type.find("C5") == 0 || freq_type.find("L5") == 0) { atx_freq = "S05"; }
        }

        if (atx_freq.empty()) {
            log_manager_.getLogger("antenna")->warn("No ATX frequency mapping for satellite {}, freq_type={}, system={}",
                satellite, freq_type, static_cast<int>(system));
            return 0.0;
        }

        // LOS from satellite to receiver (ECEF) - matches reference: l = r_xyz - s_xyz
        Eigen::Vector3d los_sat2rx = (rx_pos - sat_pos);
        double los_norm = los_sat2rx.norm();
        if (los_norm < 1e-3) {
            log_manager_.getLogger("antenna")->warn("Invalid LOS for satellite {}, freq_type={} at TOW {:.3f}", satellite, freq_type, time.tow);
            return 0.0;
        }
        Eigen::Vector3d los = los_sat2rx / los_norm;  // Unit vector from sat to receiver

        // Satellite attitude (yaw-steering) - IGS/ANTEX body frame convention
        // Use the precise cached Sun position if provided by the caller (from the rigorous
        // IAU 1976/1980 precession/nutation model computed in Geometry). Fall back to the
        // rigorous model with empty ERPs only when no cached position is available.
        Eigen::Vector3d sun_pos = sun_pos_hint;
        if (sun_pos.norm() < 1e6) {
            Eigen::Vector3d moon_dummy;
            const std::vector<ErpData> empty_erps;
            planetary_.computeSunMoonPosition(time, empty_erps, sun_pos, moon_dummy);
        }
        if (sun_pos.norm() < 1e6) {
            log_manager_.getLogger("antenna")->warn("Antenna: invalid Sun position at TOW {:.3f}, norm={:.3e}", time.tow, sun_pos.norm());
            // Even with a bad Sun position, attempt to continue with velocity-based fallback
        }
        
        // IGS/ANTEX satellite body frame convention:
        //   Z-axis (k_s) = toward Earth (nadir / boresight)
        //   X-axis (i_s) = toward Sun (in the sat-Sun-Earth plane)
        //   Y-axis (j_s) = completes right-handed frame (cross-track, along solar panel axis)
        //
        // Exception: BeiDou-2 GEO satellites (PRN C01-C05, C59, C60) use orbit-normal
        // (ON) mode permanently. In ON mode the Y-axis is along the orbit normal
        // (cross(r, v)), X-axis is approximately along-track, and Z-axis is nadir.
        //
        // ATX satellite PCO is defined in this frame (Z toward Earth).
        // ATX satellite PCV is indexed by nadir angle = angle from boresight (+Z) to LOS.
        Eigen::Vector3d k_s = -sat_pos.normalized();  // +Z axis: toward Earth (nadir/boresight)
        Eigen::Vector3d i_s, j_s;

        // Detect BeiDou-2 GEO satellites for orbit-normal attitude
        bool use_orbit_normal = false;
        if (system == System::BeiDou && satellite.size() >= 2) {
            int prn = 0;
            try { prn = std::stoi(satellite.substr(1)); } catch (...) {}
            if ((prn >= 1 && prn <= 5) || prn == 59 || prn == 60) {
                use_orbit_normal = true;
            }
        }

        if (use_orbit_normal) {
            // Orbit-normal (ON) mode for BeiDou-2 GEO:
            //   Y-axis = orbit normal = normalize(cross(position, velocity))
            //   X-axis = cross(Y, Z) — approximately along-track
            //   Z-axis = nadir (already computed as k_s)
            Eigen::Vector3d orbit_normal = sat_pos.cross(sat_vel);
            if (orbit_normal.norm() < 1e-8) {
                orbit_normal = Eigen::Vector3d::UnitY();
                log_manager_.getLogger("antenna")->warn(
                    "BeiDou GEO {}: degenerate orbit normal, using fallback", satellite);
            }
            j_s = orbit_normal.normalized();

            i_s = j_s.cross(k_s);
            if (i_s.norm() < 1e-8) {
                i_s = Eigen::Vector3d::UnitX();
            }
            i_s.normalize();

            // Re-orthogonalize Y to ensure a clean right-handed frame
            j_s = k_s.cross(i_s);
            j_s.normalize();

            log_manager_.getLogger("antenna")->trace(
                "BeiDou GEO {} using orbit-normal attitude", satellite);
        }
        else {
            // Standard yaw-steering model (GPS, Galileo, GLONASS, BeiDou-3 MEO, etc.)
            Eigen::Vector3d e_sun = (sun_pos - sat_pos).normalized();

            j_s = k_s.cross(e_sun);
            if (j_s.norm() < 1e-8) {
                // Fallback for eclipse/noon turn
                Eigen::Vector3d vhat = sat_vel.norm() > 1e-8 ? sat_vel.normalized() : Eigen::Vector3d::UnitY();
                j_s = k_s.cross(vhat);
            }
            j_s.normalize();

            i_s = j_s.cross(k_s);
            if (i_s.norm() < 1e-8) {
                i_s = Eigen::Vector3d::UnitX();
            }
            i_s.normalize();
        }

        // Satellite frame rotation matrix: sf = [i; j; k] (rows are basis vectors)
        // sf * vec_ecef -> vec_body; sf^T * vec_body -> vec_ecef
        Eigen::Matrix3d sat_frame;
        sat_frame.row(0) = i_s.transpose();
        sat_frame.row(1) = j_s.transpose();
        sat_frame.row(2) = k_s.transpose();

        // Satellite antenna selection with validity interval.
        //
        // We track id-matched candidates separately from the final selection
        // so a no-match warning can name the exact candidates the matcher
        // tried (with their validity windows) instead of just "no entry
        // found." Past silent failures of this loop have been impossible to
        // diagnose from a one-line warning alone.
        const AntennaData* sat_entry = nullptr;
        int id_matches = 0;
        std::string id_match_summary;  // first 3 candidates' types + windows
        for (const auto& a : antennas_) {
            // Match satellite PRN at the start of the type string.
            // Format: "PRN_BLOCK_SVN" e.g. "G04_BLOCKIIIA_G074"
            const bool id_match =
                (a.type.find(satellite + "_") == 0) ||
                (a.type.find("_" + satellite + "_") != std::string::npos);
            if (!id_match) continue;

            ++id_matches;
            if (id_matches <= 3) {
                if (!id_match_summary.empty()) id_match_summary += "; ";
                id_match_summary += "'" + a.type + "' valid [" +
                    std::to_string(a.valid_from.week) + ":" +
                    std::to_string(static_cast<int64_t>(a.valid_from.tow)) + ", " +
                    std::to_string(a.valid_until.week) + ":" +
                    std::to_string(static_cast<int64_t>(a.valid_until.tow)) + ")";
            }

            const bool has_valid_from = (a.valid_from.week > 0);
            const bool has_valid_until = (a.valid_until.week > 0);

            if (has_valid_from) {
                const bool after_start = !(time < a.valid_from);
                const bool before_end = has_valid_until ? (time < a.valid_until) : true;

                if (after_start && before_end) {
                    sat_entry = &a;
                    log_manager_.getLogger("antenna")->debug(
                        "Matched satellite antenna for {}: type='{}' valid [{:d}:{:.0f}, {:d}:{:.0f})",
                        satellite, a.type,
                        a.valid_from.week, a.valid_from.tow,
                        a.valid_until.week, a.valid_until.tow);
                    break;
                }
            }
            else if (!sat_entry) {
                sat_entry = &a; // fallback if no validity info
                log_manager_.getLogger("antenna")->debug(
                    "Matched satellite antenna for {} (no validity): type='{}'",
                    satellite, a.type);
            }
        }

        if (!sat_entry) {
            if (id_matches == 0) {
                // Type-string match failed entirely — strongly suggests the
                // ATX wasn't loaded or the per-entry type strings don't
                // start with the PRN. Tells the user where to look.
                log_manager_.getLogger("antenna")->warn(
                    "No satellite antenna entry found for {} at {:d}:{:.3f}: "
                    "0 ATX entries had a type string starting with '{}_' (out of {} loaded). "
                    "Check that the ATX file was parsed (product_parser logger at debug) and "
                    "that the entries' PRN field was extracted correctly.",
                    satellite, time.week, time.tow, satellite, antennas_.size());
            } else {
                // ATX has entries for this PRN but none cover the obs epoch —
                // likely a date / validity-parse mismatch.
                log_manager_.getLogger("antenna")->warn(
                    "No satellite antenna entry found for {} at {:d}:{:.3f}: "
                    "{} ATX entries matched the PRN but none had a covering validity window. "
                    "First {} considered: {}",
                    satellite, time.week, time.tow, id_matches,
                    std::min(id_matches, 3), id_match_summary);
            }
        }

        // === Satellite ===
        // The modeled range is computed from CoM to receiver marker. Antenna corrections
        // adjust for the offset between CoM and APC (PCO) and the direction-dependent
        // phase variation (PCV).
        double sat_pco_corr = 0.0, sat_pcv_corr = 0.0;
        // Pick effective ATX keys for PCO and PCV separately — an ATX entry
        // may carry PCO for a frequency without a populated PCV grid (or
        // vice versa), and the fallback chains are walked independently.
        const bool atx_fallback_on =
            config_.getModelCorrections().enable_atx_frequency_fallback;
        const std::string sat_pco_freq = sat_entry
            ? pickAtxFrequencyKey(sat_entry->pco, atx_freq, atx_fallback_on,
                                  [](const Eigen::Vector3d& v) { return v.allFinite(); })
            : std::string{};
        const std::string sat_pcv_freq = sat_entry
            ? pickAtxFrequencyKey(sat_entry->pcv, atx_freq, atx_fallback_on,
                                  [](const std::vector<std::vector<double>>& t) { return !t.empty(); })
            : std::string{};
        if (sat_entry && (!sat_pco_freq.empty() || !sat_pcv_freq.empty())) {
            // === PCO ===
            if (config_.getModelCorrections().enable_satellite_pco
                && !sat_pco_freq.empty()) {
            Eigen::Vector3d pco_body = sat_entry->pco.at(sat_pco_freq) * 1e-3; // mm -> m, in satellite body frame
            if (pco_body.allFinite()) {
                // Since sat_frame is orthonormal: sat_frame.transpose() * pco_body transforms body->ECEF
                Eigen::Vector3d pco_ecef = sat_frame.transpose() * pco_body;
                sat_pco_corr = pco_ecef.dot(los);
                // Satellite APC is closer to receiver than CoM, shortening the range
                correction -= sat_pco_corr;

                if (sat_pco_freq != atx_freq) {
                    log_manager_.getLogger("antenna")->debug(
                        "SAT PCO fallback for {} {}: requested freq={} missing, using {}",
                        satellite, sat_entry->type, atx_freq, sat_pco_freq);
                }
                log_manager_.getLogger("antenna")->trace(
                    "SAT PCO {} {} freq={}: body_mm=({:.4f},{:.4f},{:.4f}) body_m=({:.6f},{:.6f},{:.6f}) "
                    "ecef_m=({:.6f},{:.6f},{:.6f}) dot(pco_ecef,los)={:.6f} m",
                    satellite, sat_entry->type, sat_pco_freq,
                    sat_entry->pco.at(sat_pco_freq).x(), sat_entry->pco.at(sat_pco_freq).y(), sat_entry->pco.at(sat_pco_freq).z(),
                    pco_body.x(), pco_body.y(), pco_body.z(),
                    pco_ecef.x(), pco_ecef.y(), pco_ecef.z(),
                    sat_pco_corr);
            } else {
                log_manager_.getLogger("antenna")->warn("Invalid satellite PCO for {} {} atx_freq={} at TOW {:.3f}",
                    satellite, sat_entry->type, sat_pco_freq, time.tow);
            }
            } // enable_satellite_pco

            // === PCV ===
            if (config_.getModelCorrections().enable_satellite_pcv
                && !sat_pcv_freq.empty()) {
                const auto& pcv_table = sat_entry->pcv.at(sat_pcv_freq);
                if (sat_pcv_freq != atx_freq) {
                    log_manager_.getLogger("antenna")->debug(
                        "SAT PCV fallback for {} {}: requested freq={} missing, using {}",
                        satellite, sat_entry->type, atx_freq, sat_pcv_freq);
                }

                // Nadir angle: angle between boresight (+Z = k_s, toward Earth) and LOS (sat->rx)
                // For visible satellites this is typically 0-14 degrees, matching ATX PCV table range.
                double cos_nadir = k_s.dot(los);
                double nadir = std::acos(std::clamp(cos_nadir, -1.0, 1.0)) * 180.0 / Constants::PI;
                
                // For azimuth-dependent PCV, compute azimuth in satellite body frame
                Eigen::Vector3d los_body = sat_frame * los;
                double az_sat = std::atan2(los_body.y(), los_body.x()) * 180.0 / Constants::PI;
                if (az_sat < 0.0) az_sat += 360.0;
                
                double dazi = sat_entry->dazi > 0.0 ? sat_entry->dazi : 5.0;
                double dzen = sat_entry->dzen > 0.0 ? sat_entry->dzen : 1.0;
                double zen1 = sat_entry->zen1;
                double pcv_raw = interpolatePcv(pcv_table, nadir - zen1, az_sat, dazi, dzen);
                double pcv = pcv_raw * 1e-3;
                if (std::isfinite(pcv)) {
                    sat_pcv_corr = pcv;
                    correction += sat_pcv_corr;

                    log_manager_.getLogger("antenna")->trace(
                        "SAT PCV {} {} freq={}: nadir={:.4f} deg, az_body={:.4f} deg, "
                        "zen1={:.1f}, dzen={:.1f}, dazi={:.1f}, grid={}x{}, "
                        "interp_arg={:.4f}, pcv_raw_mm={:.4f}, pcv_m={:.6f}",
                        satellite, sat_entry->type, sat_pcv_freq,
                        nadir, az_sat, zen1, dzen, dazi,
                        pcv_table.size(), pcv_table[0].size(),
                        nadir - zen1, pcv_raw, pcv);
                } else {
                    log_manager_.getLogger("antenna")->warn("Invalid satellite PCV for {} {} atx_freq={} at TOW {:.3f}",
                        satellite, sat_entry->type, sat_pcv_freq, time.tow);
                }
            }

            if (sat_entry->valid_from.week > 0) {
                log_manager_.getLogger("antenna")->trace(
                    "Satellite antenna {} valid [{:d}:{:.3f},{:d}:{:.3f}) used for {} at {:d}:{:.3f}",
                    sat_entry->type,
                    sat_entry->valid_from.week, sat_entry->valid_from.tow,
                    sat_entry->valid_until.week, sat_entry->valid_until.tow,
                    satellite, time.week, time.tow);
            }
        }

        // Receiver antenna correction (sanitized + safer matching)
        double rx_pco_corr = 0.0, rx_pcv_corr = 0.0;

        // Sanitize input (remove whitespace)
        std::string rx_type_sanitized;
        rx_type_sanitized.reserve(rx_antenna_type.size());
        for (char c : rx_antenna_type) {
            if (!std::isspace(static_cast<unsigned char>(c))) rx_type_sanitized.push_back(c);
        }

        // ATX receiver antenna types are stored as "MODEL_SERIAL" (underscore separator),
        // while RINEX may provide "MODEL SERIAL" (space separator). Build an underscore-normalized
        // variant to improve matching.
        std::string rx_type_underscore;
        rx_type_underscore.reserve(rx_antenna_type.size());
        for (char c : rx_antenna_type) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!rx_type_underscore.empty() && rx_type_underscore.back() != '_')
                    rx_type_underscore.push_back('_');
            } else {
                rx_type_underscore.push_back(c);
            }
        }
        if (!rx_type_underscore.empty() && rx_type_underscore.back() == '_')
            rx_type_underscore.pop_back();

        const AntennaData* rx_entry = nullptr;

        for (const auto& a : antennas_) {
            if (a.type == rx_type_sanitized || a.type == rx_type_underscore) {
                rx_entry = &a;
                break;
            }
        }

        if (!rx_entry) {
            std::vector<const AntennaData*> candidates;
            candidates.reserve(4);

            for (const auto& a : antennas_) {
                if ((!rx_type_sanitized.empty() && a.type.find(rx_type_sanitized) != std::string::npos) ||
                    (!rx_type_underscore.empty() && a.type.find(rx_type_underscore) != std::string::npos)) {
                    candidates.push_back(&a);
                }
            }

            if (candidates.size() == 1) {
                rx_entry = candidates.front();
            } else if (candidates.size() > 1) {
                log_manager_.getLogger("antenna")->warn(
                    "Ambiguous receiver antenna match for '{}' (normalized='{}'), {} candidates: using first '{}'",
                    rx_type_sanitized, rx_type_underscore, candidates.size(), candidates.front()->type);
                rx_entry = candidates.front();
            }
        }

        // === Receiver ===
        // Independent fallback walks for PCO and PCV (same rationale as
        // the satellite side above).
        const std::string rx_pco_freq = rx_entry
            ? pickAtxFrequencyKey(rx_entry->pco, atx_freq, atx_fallback_on,
                                  [](const Eigen::Vector3d& v) { return v.allFinite(); })
            : std::string{};
        const std::string rx_pcv_freq = rx_entry
            ? pickAtxFrequencyKey(rx_entry->pcv, atx_freq, atx_fallback_on,
                                  [](const std::vector<std::vector<double>>& t) { return !t.empty(); })
            : std::string{};
        if (rx_entry && (!rx_pco_freq.empty() || !rx_pcv_freq.empty())) {
            // === PCO ===
            if (config_.getModelCorrections().enable_receiver_pco
                && !rx_pco_freq.empty()) {
            // ATX receiver PCO is stored as North/East/Up (NEU) in mm
            Eigen::Vector3d pco_neu = rx_entry->pco.at(rx_pco_freq) * 1e-3;  // Convert mm to m

            if (pco_neu.allFinite()) {
                // Transform PCO from local NEU to ECEF using centralized coordinate rotation
                double rx_norm = rx_pos.norm();
                if (rx_norm > 1e3) {
                    Coordinates::Llh rx_llh = Coordinates::ecefToLlh(rx_pos);

                    // Get ENU→ECEF rotation matrix
                    Eigen::Matrix3d ori = Coordinates::enuToEcefMatrix(rx_llh.lat_rad, rx_llh.lon_rad);

                    // ATX receiver PCO is stored as North/East/Up (NEU); reorder to ENU
                    Eigen::Vector3d pco_enu(pco_neu(1), pco_neu(0), pco_neu(2));

                    // Transform ENU to ECEF
                    Eigen::Vector3d pco_ecef = ori * pco_enu;

                    // Project onto satellite-to-receiver LOS
                    rx_pco_corr = pco_ecef.dot(los);

                    // Receiver PCO adds to modeled range
                    correction += rx_pco_corr;

                    if (rx_pco_freq != atx_freq) {
                        log_manager_.getLogger("antenna")->debug(
                            "RX PCO fallback for {} '{}': requested freq={} missing, using {}",
                            satellite, rx_entry->type, atx_freq, rx_pco_freq);
                    }
                    log_manager_.getLogger("antenna")->trace(
                        "RX PCO {} freq={} type='{}': neu_mm=({:.4f},{:.4f},{:.4f}) neu_m=({:.6f},{:.6f},{:.6f}) "
                        "enu_m=({:.6f},{:.6f},{:.6f}) ecef_m=({:.6f},{:.6f},{:.6f}) dot(pco_ecef,los)={:.6f} m",
                        satellite, rx_pco_freq, rx_entry->type,
                        rx_entry->pco.at(rx_pco_freq).x(), rx_entry->pco.at(rx_pco_freq).y(), rx_entry->pco.at(rx_pco_freq).z(),
                        pco_neu.x(), pco_neu.y(), pco_neu.z(),
                        pco_enu.x(), pco_enu.y(), pco_enu.z(),
                        pco_ecef.x(), pco_ecef.y(), pco_ecef.z(),
                        rx_pco_corr);
                }
            }
            else {
                log_manager_.getLogger("antenna")->warn("Invalid receiver PCO for {} {} atx_freq={} at TOW {:.3f}",
                    satellite, rx_type_sanitized, rx_pco_freq, time.tow);
            }
            } // enable_receiver_pco

            // === PCV ===
            if (config_.getModelCorrections().enable_receiver_pcv
                && !rx_pcv_freq.empty()) {
                const auto& pcv_table = rx_entry->pcv.at(rx_pcv_freq);
                if (rx_pcv_freq != atx_freq) {
                    log_manager_.getLogger("antenna")->debug(
                        "RX PCV fallback for {} '{}': requested freq={} missing, using {}",
                        satellite, rx_entry->type, atx_freq, rx_pcv_freq);
                }
                double zenith = 90.0 - elevation;
                double dazi = rx_entry->dazi > 0.0 ? rx_entry->dazi : 5.0;
                double dzen = rx_entry->dzen > 0.0 ? rx_entry->dzen : 5.0;
                double zen1 = rx_entry->zen1;
                double pcv_raw = interpolatePcv(pcv_table, zenith - zen1, azimuth, dazi, dzen);
                double pcv = pcv_raw * 1e-3;
                if (std::isfinite(pcv)) {
                    rx_pcv_corr = pcv;
                    correction += rx_pcv_corr;

                    log_manager_.getLogger("antenna")->trace(
                        "RX PCV {} freq={} type='{}': elev={:.4f} zen={:.4f} az={:.4f} "
                        "zen1={:.1f} dzen={:.1f} dazi={:.1f} grid={}x{} "
                        "interp_arg={:.4f} pcv_raw_mm={:.4f} pcv_m={:.6f}",
                        satellite, rx_pcv_freq, rx_entry->type,
                        elevation, zenith, azimuth,
                        zen1, dzen, dazi,
                        pcv_table.size(), pcv_table[0].size(),
                        zenith - zen1, pcv_raw, pcv);
                } else {
                    log_manager_.getLogger("antenna")->warn("Invalid receiver PCV for {} {} atx_freq={} at TOW {:.3f}",
                        satellite, rx_type_sanitized, rx_pcv_freq, time.tow);
                }
            }
        }
        else if (!rx_entry) {
            log_manager_.getLogger("antenna")->trace(
                "RX no antenna model match for '{}' (normalized='{}'), satellite={} freq={}",
                rx_type_sanitized, rx_type_underscore, satellite, atx_freq);
        }
        else {
            log_manager_.getLogger("antenna")->trace(
                "RX antenna '{}' has no PCO for freq={}, satellite={}",
                rx_entry->type, atx_freq, satellite);
        }

        double wind_up_corr = 0.0;
        if (config_.getModelCorrections().enable_phase_windup && is_phase && wavelength > 0.0) {
            double wind_up = gnss_corrections_.computePhaseWindUp(satellite, system, rx_pos, sat_pos, wavelength, time, sun_pos, sat_vel, config_.accumulatePhaseWindUp());
            if (std::isfinite(wind_up)) {
                wind_up_corr = wind_up;
                correction += wind_up_corr;
            } else {
                log_manager_.getLogger("antenna")->warn("Invalid phase wind-up for {} {} at TOW {:.3f}", satellite, freq_type, time.tow);
            }
        }

        if (std::isfinite(correction) && correction != 0.0) {
            log_manager_.getLogger("antenna")->trace("Antenna correction for {} {}: total={:.6f} m (sat_pco={:.6f}, sat_pcv={:.6f}, "
                "rx_pco={:.6f}, rx_pcv={:.6f}, wind_up={:.6f}) at TOW {:.3f}, dazi={:.3f}, dzen={:.3f}",
                satellite, freq_type, correction, sat_pco_corr, sat_pcv_corr,
                rx_pco_corr, rx_pcv_corr, wind_up_corr, time.tow,
                sat_entry ? sat_entry->dazi : 5.0, sat_entry ? sat_entry->dzen : 5.0);
        } else {
            log_manager_.getLogger("antenna")->trace("No antenna correction for {} {} at TOW {:.3f}", satellite, freq_type, time.tow);
        }
        return correction;
    }

    bool Antenna::hasReceiverAtxEntry(const std::string& signal, System system) const {
        // --- Map signal to ATX frequency key (same logic as getAntennaCorrection) ---
        std::string atx_freq;
        if (system == System::GPS) {
            if (signal.find("C1") == 0 || signal.find("L1") == 0) atx_freq = "G01";
            else if (signal.find("C2") == 0 || signal.find("L2") == 0) atx_freq = "G02";
            else if (signal.find("C5") == 0 || signal.find("L5") == 0) atx_freq = "G05";
        } else if (system == System::Galileo) {
            if (signal.find("C1") == 0 || signal.find("L1") == 0) atx_freq = "E01";
            else if (signal.find("C5") == 0 || signal.find("L5") == 0) atx_freq = "E05";
            else if (signal.find("C7") == 0 || signal.find("L7") == 0) atx_freq = "E07";
            else if (signal.find("C6") == 0 || signal.find("L6") == 0) atx_freq = "E06";
            else if (signal.find("C8") == 0 || signal.find("L8") == 0) atx_freq = "E08";
        } else if (system == System::GLONASS) {
            if (signal.find("C1") == 0 || signal.find("L1") == 0) atx_freq = "R01";
            else if (signal.find("C2") == 0 || signal.find("L2") == 0) atx_freq = "R02";
            else if (signal.find("C3") == 0 || signal.find("L3") == 0) atx_freq = "R03";
        } else if (system == System::BeiDou) {
            if (signal.find("C2") == 0 || signal.find("L2") == 0) atx_freq = "C02";
            else if (signal.find("C7") == 0 || signal.find("L7") == 0) atx_freq = "C07";
            else if (signal.find("C6") == 0 || signal.find("L6") == 0) atx_freq = "C06";
            else if (signal.find("C1") == 0 || signal.find("L1") == 0) atx_freq = "C01";
            else if (signal.find("C5") == 0 || signal.find("L5") == 0) atx_freq = "C05";
            else if (signal.find("C8") == 0 || signal.find("L8") == 0) atx_freq = "C08";
        } else if (system == System::QZSS) {
            if (signal.find("C1") == 0 || signal.find("L1") == 0) atx_freq = "J01";
            else if (signal.find("C2") == 0 || signal.find("L2") == 0) atx_freq = "J02";
            else if (signal.find("C5") == 0 || signal.find("L5") == 0) atx_freq = "J05";
            else if (signal.find("C6") == 0 || signal.find("L6") == 0) atx_freq = "J06";
        } else if (system == System::SBAS) {
            if (signal.find("C1") == 0 || signal.find("L1") == 0) atx_freq = "S01";
            else if (signal.find("C5") == 0 || signal.find("L5") == 0) atx_freq = "S05";
        }
        if (atx_freq.empty()) return false;

        // --- Find receiver antenna entry (same matching as getAntennaCorrection) ---
        const std::string rx_type_raw = rinex_parser_.getReceiverAntennaType();
        std::string rx_type_sanitized;
        rx_type_sanitized.reserve(rx_type_raw.size());
        for (char c : rx_type_raw)
            if (!std::isspace(static_cast<unsigned char>(c))) rx_type_sanitized.push_back(c);

        std::string rx_type_underscore;
        rx_type_underscore.reserve(rx_type_raw.size());
        for (char c : rx_type_raw) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!rx_type_underscore.empty() && rx_type_underscore.back() != '_')
                    rx_type_underscore.push_back('_');
            } else {
                rx_type_underscore.push_back(c);
            }
        }
        if (!rx_type_underscore.empty() && rx_type_underscore.back() == '_')
            rx_type_underscore.pop_back();

        const AntennaData* rx_entry = nullptr;
        for (const auto& a : antennas_) {
            if (a.type == rx_type_sanitized || a.type == rx_type_underscore) {
                rx_entry = &a;
                break;
            }
        }
        if (!rx_entry) {
            for (const auto& a : antennas_) {
                if ((!rx_type_sanitized.empty() && a.type.find(rx_type_sanitized) != std::string::npos) ||
                    (!rx_type_underscore.empty() && a.type.find(rx_type_underscore) != std::string::npos)) {
                    rx_entry = &a;
                    break;
                }
            }
        }
        if (!rx_entry) return false;
        return rx_entry->pco.count(atx_freq) > 0;
    }

    /**
     * @brief Return the receiver antenna type string from the RINEX parser.
     *
     * The returned string is sanitized (whitespace removed) to ease matching
     * against ATX entries.
     *
     * @return Sanitized receiver antenna type string (empty if unknown).
     */
    std::string Antenna::getReceiverAntennaType() const {
        std::string antenna_type = rinex_parser_.getReceiverAntennaType();
        if (antenna_type.empty()) {
            log_manager_.getLogger("antenna")->warn("Empty receiver antenna type returned from RINEX parser");
            return antenna_type;
        }
        std::string sanitized;
        sanitized.reserve(antenna_type.size());
        for (char c : antenna_type) {
            if (!std::isspace(static_cast<unsigned char>(c))) sanitized.push_back(c);
        }
        return sanitized;
    }

    /**
     * @brief Load antenna data (ATX) via ProductParser.
     *
     * Attempts to locate the ATX product path via ProductParser and parse
     * antenna entries into the internal `antennas_` vector. Errors and
     * exceptions are logged but do not throw.
     */
    void Antenna::loadAntennaData() {
        // Data accessed via reference to ProductCache - nothing to load
        log_manager_.getLogger("antenna_model")->debug(
            "Using cached antenna reference (zero-copy): {} entries",
            antennas_.size());
    }

    void Antenna::dumpAntennaModels() const {
        auto logger = log_manager_.getLogger("antenna_model");

        // Only dump the receiver antenna model that matches the RINEX header.
        // Satellite antenna entries are already traced per-observation in
        // getAntennaCorrection(), so dumping all of them here would produce
        // thousands of lines for no benefit.
        std::string rx_type = getReceiverAntennaType();
        if (rx_type.empty()) {
            logger->trace("=== ANTENNA MODEL DUMP: no receiver antenna type from RINEX ===");
            return;
        }

        // Build the underscore-normalized variant used for matching
        std::string rx_raw = rinex_parser_.getReceiverAntennaType();
        std::string rx_type_underscore;
        rx_type_underscore.reserve(rx_raw.size());
        for (char c : rx_raw) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!rx_type_underscore.empty() && rx_type_underscore.back() != '_')
                    rx_type_underscore.push_back('_');
            } else {
                rx_type_underscore.push_back(c);
            }
        }
        if (!rx_type_underscore.empty() && rx_type_underscore.back() == '_')
            rx_type_underscore.pop_back();

        // Find the matching entry (same logic as getAntennaCorrection)
        const AntennaData* rx_entry = nullptr;
        size_t rx_index = 0;

        for (size_t i = 0; i < antennas_.size(); ++i) {
            const auto& a = antennas_[i];
            if (a.type == rx_type || a.type == rx_type_underscore) {
                rx_entry = &a;
                rx_index = i;
                break;
            }
        }
        if (!rx_entry) {
            for (size_t i = 0; i < antennas_.size(); ++i) {
                const auto& a = antennas_[i];
                if ((!rx_type.empty() && a.type.find(rx_type) != std::string::npos) ||
                    (!rx_type_underscore.empty() && a.type.find(rx_type_underscore) != std::string::npos)) {
                    rx_entry = &a;
                    rx_index = i;
                    break;
                }
            }
        }

        if (!rx_entry) {
            logger->trace("=== ANTENNA MODEL DUMP: no ATX match for rx='{}' (normalized='{}') ===",
                rx_type, rx_type_underscore);
            return;
        }

        logger->trace("=== BEGIN RECEIVER ANTENNA MODEL DUMP (rx='{}', matched='{}', index={}) ===",
            rx_type, rx_entry->type, rx_index);

        const auto& a = *rx_entry;

        logger->trace(
            "ANT type='{}' valid_from={:d}:{:.0f} valid_until={:d}:{:.0f} "
            "dazi={:.2f} dzen={:.2f} zen1={:.2f}",
            a.type,
            a.valid_from.week, a.valid_from.tow,
            a.valid_until.week, a.valid_until.tow,
            a.dazi, a.dzen, a.zen1);

        // PCO per frequency
        for (const auto& [freq, pco] : a.pco) {
            logger->trace(
                "ANT PCO freq={}: N={:.4f} E={:.4f} U={:.4f} mm",
                freq, pco.x(), pco.y(), pco.z());
        }

        // PCV per frequency
        for (const auto& [freq, table] : a.pcv) {
            if (table.empty()) {
                logger->trace("ANT PCV freq={}: (empty)", freq);
                continue;
            }

            size_t n_rows = table.size();
            size_t n_cols = table[0].size();
            bool az_indep = (n_cols == 1);

            logger->trace(
                "ANT PCV freq={}: {}x{} ({})",
                freq, n_rows, n_cols,
                az_indep ? "NOAZI" : "AZI-dependent");

            // Dump each zenith ring row
            for (size_t r = 0; r < n_rows; ++r) {
                double zen_angle = a.zen1 + r * a.dzen;
                if (az_indep) {
                    logger->trace(
                        "ANT PCV freq={} zen={:.1f}: {:.4f}",
                        freq, zen_angle, table[r][0]);
                }
                else {
                    std::string vals;
                    vals.reserve(n_cols * 10);
                    for (size_t c = 0; c < n_cols; ++c) {
                        if (c > 0) vals += ',';
                        char buf[16];
                        std::snprintf(buf, sizeof(buf), "%.4f", table[r][c]);
                        vals += buf;
                    }
                    logger->trace(
                        "ANT PCV freq={} zen={:.1f}: [{}]",
                        freq, zen_angle, vals);
                }
            }
        }

        logger->trace("=== END RECEIVER ANTENNA MODEL DUMP ===");
    }

    /**
     * @brief Interpolate a PCV grid.
     *
     * Supports both azimuth-independent tables (single column) and full 2D
     * grids. Linear interpolation is used between zenith rings and azimuth
     * sectors; azimuth wraps around circularly.
     *
     * @param pcv_table 2D PCV table: rows=zenith rings, cols=azimuth sectors (or 1 for azimuth-independent).
     * @param zenith Zenith angle in degrees (0..90+ as provided by caller).
     * @param azimuth Azimuth angle in degrees (0..360).
     * @param dazi Azimuth spacing in degrees between PCV columns.
     * @param dzen Zenith spacing in degrees between PCV rows.
     * @return Interpolated PCV value in same units as table (meters if table was in meters; caller scales).
     */
    void Antenna::resetPhaseWindUp(const std::string& satellite) {
        gnss_corrections_.resetPhaseWindUp(satellite);
    }

    void Antenna::resetAllPhaseWindUp() {
        gnss_corrections_.resetAllPhaseWindUp();
    }

    double Antenna::interpolatePcv(const std::vector<std::vector<double>>& pcv_table,
                                   double zenith,
                                   double azimuth,
                                   double dazi,
                                   double dzen) const {
        if (pcv_table.empty() || pcv_table[0].empty()) return 0.0;
        if (dzen <= 0.0) return 0.0;
        size_t cols = pcv_table[0].size();
        bool az_indep = (cols == 1);

        // Clamp zenith to non-negative (zen1 offset already subtracted by caller)
        if (zenith < 0.0) zenith = 0.0;

        size_t zen_idx = static_cast<size_t>(zenith / dzen);
        double zen_frac = (zenith - zen_idx * dzen) / dzen;
        if (zen_idx >= pcv_table.size()) {
            zen_idx = pcv_table.size() - 1;
            zen_frac = 0.0;
        }

        if (az_indep) {
            double pcv_lo = pcv_table[zen_idx][0];
            double pcv_hi = pcv_lo;
            if (zen_idx + 1 < pcv_table.size())
                pcv_hi = pcv_table[zen_idx + 1][0];
            double pcv = pcv_lo + zen_frac * (pcv_hi - pcv_lo);

            log_manager_.getLogger("antenna")->trace(
                "interpolatePcv(NOAZ): zen_in={:.4f} zen_idx={} zen_frac={:.6f} "
                "pcv[{}]={:.4f} pcv[{}]={:.4f} => interp={:.4f}",
                zenith, zen_idx, zen_frac,
                zen_idx, pcv_lo,
                zen_idx + 1, pcv_hi,
                pcv);
            return pcv;
        }

        size_t az_idx = static_cast<size_t>(azimuth / dazi);
        double az_frac = (azimuth - az_idx * dazi) / dazi;
        if (az_idx >= cols) {
            az_idx = cols - 1;
            az_frac = 0.0;
        }
        size_t az_next = (az_idx + 1) % cols;

        double pcv00 = pcv_table[zen_idx][az_idx];
        double pcv01 = pcv_table[zen_idx][az_next];
        double pcv10 = pcv_table[zen_idx + 1][az_idx];
        double pcv11 = pcv_table[zen_idx + 1][az_next];
        if (zen_idx + 1 < pcv_table.size()) {
            pcv10 = pcv_table[zen_idx + 1][az_idx];
            pcv11 = pcv_table[zen_idx + 1][az_next];
        }

        double pcv =
            pcv00 * (1 - zen_frac) * (1 - az_frac) +
            pcv10 * zen_frac * (1 - az_frac) +
            pcv01 * (1 - zen_frac) * az_frac +
            pcv11 * zen_frac * az_frac;

        log_manager_.getLogger("antenna")->trace(
            "interpolatePcv(2D): zen_in={:.4f} az_in={:.4f} zen_idx={} az_idx={} az_next={} "
            "zen_frac={:.6f} az_frac={:.6f} "
            "pcv00={:.4f} pcv01={:.4f} pcv10={:.4f} pcv11={:.4f} => interp={:.4f}",
            zenith, azimuth, zen_idx, az_idx, az_next,
            zen_frac, az_frac,
            pcv00, pcv01, pcv10, pcv11, pcv);
        return pcv;
    }

}