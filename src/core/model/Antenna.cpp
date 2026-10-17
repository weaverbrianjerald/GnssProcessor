#include "model/Antenna.hpp"
#include "model/SatAttitude.hpp"
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
        // alternates ordered by carrier-frequency proximity, closest first.
        // The order is approximate where two bands lie within about 15 MHz of
        // each other: E08 precedes E07 for E01, C08 precedes C07 for C01/C02,
        // and C02 precedes C01 for C06/C07/C08.  Used when an antenna entry's
        // pco/pcv map does not include the requested key (common for L5 / E5* /
        // B2a on older satellites and on many receiver antennas calibrated
        // before triple-frequency tracking was widespread).  Cross-frequency
        // PCO drift on the same physical antenna is typically <1 cm, much
        // smaller than the few-cm vertical residual left by a zero correction.
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

        // Receiver-only cross-system fallback (nearest GPS band by carrier
        // frequency), walked after atxFallbackChain() is exhausted.  The
        // receiver phase pattern is a function of frequency on shared
        // hardware, so substituting the nearest GPS band for a missing
        // non-GPS key is physically valid for the receiver antenna.  It is
        // never used for the satellite antenna: E01 and G01 are different
        // spacecraft, so this chain is kept separate from atxFallbackChain()
        // (which the satellite path shares).
        //
        // The main mappings are exact carrier-frequency matches
        // (E1->G01, E5a->G05, B1C->G01, B2a->G05 = 1575.42 / 1176.45 MHz);
        // the others are approximations 14-51 MHz apart.
        const std::map<std::string, std::vector<std::string>>& atxReceiverCrossSystemChain() {
            static const std::map<std::string, std::vector<std::string>> chain = {
                { "E01", { "G01" } },          // E1  1575.42 == L1 (exact)
                { "E05", { "G05", "G02" } },   // E5a 1176.45 == L5 (exact)
                { "E07", { "G02", "G05" } },   // E5b 1207.14 ~ L2 (approx)
                { "E06", { "G02" } },          // E6  1278.75 ~ L2 (approx)
                { "C01", { "G01" } },          // B1I 1561.098 ~ L1 (~14 MHz)
                { "C02", { "G01" } },          // B1C 1575.42 == L1 (exact)
                { "C05", { "G05", "G02" } },   // B2a 1176.45 == L5 (exact)
                { "C06", { "G02" } },          // B3I 1268.52 ~ L2 (approx)
                // QZSS/SBAS already share GPS frequencies; no entry needed.
            };
            return chain;
        }

        /// Pick an effective ATX frequency key from @p entry_map.  Returns
        /// @p requested when present and the @p good predicate accepts the
        /// stored value; otherwise walks the per-system fallback chain
        /// (when @p enable_fallback is true) and returns the first
        /// alternate that satisfies the predicate.  Empty string when
        /// nothing matches; callers then skip the correction (it stays 0).
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

        /// Receiver-only variant of @c pickAtxFrequencyKey.  First runs the
        /// same exact-then-in-system-fallback resolution (so the receiver
        /// path matches the satellite path when the requested key exists on
        /// the antenna).  Only when that yields nothing and @p cross_system is
        /// true does it walk @c atxReceiverCrossSystemChain(), the
        /// nearest-GPS-band substitution.  Returns the first alternate
        /// present and accepted by @p good, else empty (the caller then skips
        /// the correction).
        ///
        /// Do not call it for the satellite antenna: the cross-system chain
        /// would borrow another constellation's spacecraft entry.
        template <typename Map, typename GoodPred>
        std::string pickReceiverAtxFrequencyKey(const Map& entry_map,
                                                const std::string& requested,
                                                bool enable_fallback,
                                                bool cross_system,
                                                GoodPred good) {
            // 1) exact key + in-system frequency-proximity chain.
            const std::string in_system =
                pickAtxFrequencyKey(entry_map, requested, enable_fallback, good);
            if (!in_system.empty()) return in_system;
            // 2) receiver-only cross-system nearest-GPS-band fallback.
            if (!cross_system || requested.empty()) return {};
            const auto& chain = atxReceiverCrossSystemChain();
            auto cit = chain.find(requested);
            if (cit == chain.end()) return {};
            for (const auto& alt : cit->second) {
                auto ait = entry_map.find(alt);
                if (ait != entry_map.end() && good(ait->second)) return alt;
            }
            return {};
        }

    /// Map a RINEX-3 signal code (e.g. "L5Q", "C7Q") and its system to
    /// the ATX frequency key (e.g. "G05", "E07").  Returns the empty
    /// string when the signal is not recognised for that system.  The one
    /// mapping used by @c getAntennaCorrection and by the coverage queries
    /// (@c hasReceiverAtxEntry, @c receiverAtxFrequencySource,
    /// @c hasFullAtxCoverage, @c wasAtxSubstituted), so they stay in sync.
    std::string signalToAtxFrequencyKey(const std::string& signal, System system) {
        if (signal.size() < 2) return {};
        const std::string two = signal.substr(0, 2);
        switch (system) {
        case System::GPS:
            if (two == "C1" || two == "L1") return "G01";
            if (two == "C2" || two == "L2") return "G02";
            if (two == "C5" || two == "L5") return "G05";
            return {};
        case System::Galileo:
            if (two == "C1" || two == "L1") return "E01";
            if (two == "C5" || two == "L5") return "E05";
            if (two == "C7" || two == "L7") return "E07";
            if (two == "C6" || two == "L6") return "E06";
            if (two == "C8" || two == "L8") return "E08";
            return {};
        case System::GLONASS:
            if (two == "C1" || two == "L1") return "R01";
            if (two == "C2" || two == "L2") return "R02";
            if (two == "C3" || two == "L3") return "R03";
            return {};
        case System::BeiDou:
            if (two == "C2" || two == "L2") return "C02";
            if (two == "C7" || two == "L7") return "C07";
            if (two == "C6" || two == "L6") return "C06";
            if (two == "C1" || two == "L1") return "C01";
            if (two == "C5" || two == "L5") return "C05";
            if (two == "C8" || two == "L8") return "C08";
            return {};
        case System::QZSS:
            if (two == "C1" || two == "L1") return "J01";
            if (two == "C2" || two == "L2") return "J02";
            if (two == "C5" || two == "L5") return "J05";
            if (two == "C6" || two == "L6") return "J06";
            return {};
        case System::SBAS:
            if (two == "C1" || two == "L1") return "S01";
            if (two == "C5" || two == "L5") return "S05";
            return {};
        default:
            return {};
        }
    }

    /// Whitespace-normalize a receiver antenna type for ATX matching.
    /// @p sanitized has all whitespace removed; @p underscore collapses
    /// internal whitespace runs to a single '_' (with no trailing '_').
    /// dumpAntennaModels() builds the same two forms inline.
    void normalizeAntennaType(const std::string& raw,
                              std::string& sanitized,
                              std::string& underscore) {
        sanitized.clear();
        sanitized.reserve(raw.size());
        for (char c : raw)
            if (!std::isspace(static_cast<unsigned char>(c))) sanitized.push_back(c);

        underscore.clear();
        underscore.reserve(raw.size());
        for (char c : raw) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!underscore.empty() && underscore.back() != '_')
                    underscore.push_back('_');
            } else {
                underscore.push_back(c);
            }
        }
        if (!underscore.empty() && underscore.back() == '_')
            underscore.pop_back();
    }

    // correctAzimuth() is a public static member of Antenna (exposed for unit
    // testing alongside interpolatePcv); see Antenna.hpp and the definition
    // below the constructor.

    /// Type-preserving radome→NONE variant of a 20-char ANTEX/RINEX antenna
    /// field ("<16-char model><4-char radome>"): the same field with its
    /// radome overwritten by the radome-agnostic "NONE" calibration, or {}
    /// when the radome is already "NONE" or the field is shorter than 20
    /// chars.  A blank radome is not skipped: it is replaced by "NONE" like any
    /// other code.  Standard IGS/EPN fallback: an uncalibrated radome code
    /// borrows the same physical antenna's radome-agnostic NONE PCO/PCV
    /// (mm-level).  Never touches the antenna type (chars 0-15).
    std::string radomeNoneVariantOf(const std::string& s) {
        if (s.size() >= 20 && s.substr(16, 4) != "NONE") {
            std::string v = s;
            v.replace(16, 4, "NONE");
            return v;
        }
        return {};
    }

    /// Build receiver antenna TYPE-remap substitution candidates (priority
    /// order) for a raw RINEX antenna type, modelled on M-PAGES AntennaSubs +
    /// AntennaDatabase::findSubRxAnt: an explicit deprecated/legacy-code table
    /// (an extension point; see RNXCMP/NGS deprecatedAnts.json), each table
    /// candidate also offered with its own radome→NONE fallback.
    ///
    /// The generic IGS radome→NONE fallback on the original type is not built
    /// here: Antenna::matchReceiverAntenna() (called by
    /// findReceiverAntennaEntry()) applies it unconditionally as the always-on
    /// Tier 1 step (@ref radomeNoneVariantOf), before consulting this opt-in
    /// TYPE-remap table.  Duplicates are removed while preserving priority
    /// order.  Returns an empty list when no table rule applies.
    std::vector<std::string> receiverAntennaSubstitutes(const std::string& raw) {
        // Explicit deprecated/legacy-code TYPE-remap table.  Empty by default:
        // the always-on Tier 1 radome→NONE step already covers the common
        // "specific radome uncalibrated" case for every antenna, so entries
        // belong here only for real model-code remaps, e.g.
        //   { "OLD_MODEL_CODE   NONE", { "NEW_MODEL_CODE   NONE" } },
        static const std::map<std::string, std::vector<std::string>> kSubTable = {};

        std::vector<std::string> out;
        auto push_unique = [&out](const std::string& c) {
            if (c.empty()) return;
            for (const auto& e : out) if (e == c) return;
            out.push_back(c);
        };

        const auto it = kSubTable.find(raw);
        if (it != kSubTable.end()) {
            for (const auto& cand : it->second) {
                push_unique(cand);
                // A remapped type may itself carry an uncalibrated radome.
                push_unique(radomeNoneVariantOf(cand));
            }
        }
        return out;
    }

    /// Pure exact-then-substring receiver-antenna match against @p antennas (no
    /// logging).  An exact match on either normalized form wins; otherwise the
    /// first substring-containment match in @p antennas order is returned.
    /// @p out_count (optional) receives the number of substring candidates so
    /// the caller can warn on ambiguity (1 is reported for an exact hit, which
    /// never trips the >1 ambiguity check).  The one implementation of this
    /// order-sensitive, first-match primitive, shared by the production path
    /// and the unit tests.
    const AntennaData* matchReceiverEntryInList(const std::vector<AntennaData>& antennas,
                                                const std::string& sanitized,
                                                const std::string& underscore,
                                                int* out_count = nullptr) {
        if (out_count) *out_count = 0;
        // (1) Exact match on either normalized form.
        for (const auto& a : antennas) {
            if (a.type == sanitized || a.type == underscore) {
                if (out_count) *out_count = 1;
                return &a;
            }
        }
        // (2) Substring-containment fallback.  The first candidate (in list
        // order) is always the one used; the count lets the caller report an
        // ambiguous match.
        const AntennaData* first = nullptr;
        int n = 0;
        for (const auto& a : antennas) {
            if ((!sanitized.empty()  && a.type.find(sanitized)  != std::string::npos) ||
                (!underscore.empty() && a.type.find(underscore) != std::string::npos)) {
                if (!first) first = &a;
                ++n;
            }
        }
        if (out_count) *out_count = n;
        return first;
    }

    } // namespace

    /**
     * @file Antenna.cpp
     * @brief Implementation of Antenna corrections: PCO/PCV interpolation, phase wind-up,
     *        and access to the antenna models loaded from ATX products.
     */

    /**
     * @brief Construct an Antenna helper.
     *
     * Binds `antennas_` to the antenna list already loaded into the ProductCache
     * (no copy, no ATX parsing here), initializes the internal helpers used to
     * compute antenna corrections, and dumps the matched receiver antenna model at
     * trace level.
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
        planetary_(log_mgr, config.enableCioFrame(), config.enableSunFrameCorrection()),
        // Reference into the cache; no copy.
        antennas_(ProductCache::getInstance().getAntennas())
    {
        // loadAntennaData() is not needed: antennas_ already refers to the cache.
        log_manager_.getLogger("antenna_model")->info(
            "Antenna init (zero-copy): {} types, rx={}",
            antennas_.size(), getReceiverAntennaType());

        // Dump the matched receiver antenna model at trace level for ATX comparison.
        dumpAntennaModels();
    }

    double Antenna::correctAzimuth(double azimuth_deg, double orientation_deg) {
        // Ported verbatim from M-PAGES NGS::RxAntenna::correctAzimuth:
        // subtract the orientation, then wrap once in each direction by 360
        // (inputs in [0,360) give a result in [0,360)).
        // e.g. correctAzimuth(10,90) == 280.
        double az = azimuth_deg - orientation_deg;
        if (az < 0.0)   az += 360.0;
        if (az > 360.0) az -= 360.0;
        return az;
    }

    Antenna::ReceiverMatch Antenna::matchReceiverAntenna(const std::vector<AntennaData>& antennas,
                                                         const std::string& rx_antenna_type,
                                                         bool enable_type_substitution) {
        ReceiverMatch m;
        normalizeAntennaType(rx_antenna_type, m.sanitized, m.underscore);

        // (1) Primary exact/substring match on the raw type.
        if (const AntennaData* e = matchReceiverEntryInList(
                antennas, m.sanitized, m.underscore, &m.ambiguous_candidates)) {
            m.entry = e;
            return m;
        }

        // (2) Tier 1 (always-on): type-preserving radome→NONE fallback.  Retry
        // the same antenna type with its radome set to "NONE", the documented
        // IGS/EPN practice of borrowing a physical antenna's radome-agnostic
        // calibration when its specific radome code is uncalibrated (mm-level).
        // Runs before the opt-in TYPE-remap table and never remaps the antenna
        // type, so an unknown model still resolves to nothing (zero receiver
        // correction).  A primary match returns at (1), so matched lookups are
        // unaffected; the fallback only supplies an entry where the primary
        // match found none.
        if (const std::string none_variant = radomeNoneVariantOf(rx_antenna_type);
            !none_variant.empty()) {
            std::string ns, nu;
            normalizeAntennaType(none_variant, ns, nu);
            if (const AntennaData* e = matchReceiverEntryInList(antennas, ns, nu,
                                                                &m.ambiguous_candidates)) {
                m.entry = e;
                m.via_radome_none = true;
                return m;
            }
        }

        // (3) Tier 1b (opt-in): rule-driven TYPE-remap substitution.  Off by
        // default, so an unknown model still yields a zero receiver correction.
        if (enable_type_substitution) {
            for (const std::string& cand : receiverAntennaSubstitutes(rx_antenna_type)) {
                std::string cs, cu;
                normalizeAntennaType(cand, cs, cu);
                if (const AntennaData* e = matchReceiverEntryInList(antennas, cs, cu,
                                                                    &m.ambiguous_candidates)) {
                    m.entry = e;
                    m.type_substitution = cand;
                    return m;
                }
            }
        }
        return m;  // no match (entry == nullptr)
    }

    const AntennaData* Antenna::findReceiverAntennaEntry(const std::string& rx_antenna_type,
                                                         bool warn_on_ambiguity,
                                                         std::string* out_sanitized,
                                                         std::string* out_underscore,
                                                         bool* out_via_radome_none) const {
        // All matching lives in the pure static matchReceiverAntenna(); this
        // member wrapper only forwards the diagnostics and emits the warnings:
        // the ambiguity warning only when warn_on_ambiguity is true (the
        // correction path), the TYPE-substitution warning whenever the opt-in
        // table supplied the entry.
        const ReceiverMatch m = matchReceiverAntenna(
            antennas_, rx_antenna_type, config_.enableRxAntennaSubstitution());
        if (out_sanitized)       *out_sanitized       = m.sanitized;
        if (out_underscore)      *out_underscore      = m.underscore;
        if (out_via_radome_none) *out_via_radome_none = m.via_radome_none;

        if (m.entry) {
            if (warn_on_ambiguity && m.ambiguous_candidates > 1) {
                log_manager_.getLogger("antenna")->warn(
                    "Ambiguous receiver antenna match for '{}' (normalized='{}'), {} candidates: using first '{}'",
                    m.sanitized, m.underscore, m.ambiguous_candidates, m.entry->type);
            }
            if (!m.type_substitution.empty()) {
                log_manager_.getLogger("antenna")->warn(
                    "Receiver antenna substitution: '{}' has no ATX match; using '{}' (ATX entry '{}')",
                    rx_antenna_type, m.type_substitution, m.entry->type);
            }
        }
        return m.entry;
    }

    bool Antenna::receiverAntennaViaRadomeFallback(std::string* out_detail) const {
        const std::string raw = rinex_parser_.getReceiverAntennaType();
        bool via = false;
        const AntennaData* e =
            findReceiverAntennaEntry(raw, /*warn_on_ambiguity=*/false,
                                     nullptr, nullptr, &via);
        if (!via || !e) return false;
        if (out_detail) {
            // Trailing-trim the raw RINEX field for display; the radome is the
            // 4-char code at [16,20) of the ANTEX "<16 model><4 radome>" field.
            std::string shown = raw;
            const auto last = shown.find_last_not_of(" \t\r\n");
            shown = (last == std::string::npos) ? std::string{} : shown.substr(0, last + 1);
            const std::string radome =
                (raw.size() >= 20) ? raw.substr(16, 4) : std::string{"????"};
            *out_detail = fmt::format(
                "'{}' radome '{}' uncalibrated — using radome-agnostic NONE "
                "calibration (ATX entry '{}'; type-preserving, mm-level)",
                shown, radome, e->type);
        }
        return true;
    }

    /**
     * @brief Compute antenna correction (meters) for a satellite/receiver observation.
     *
     * This function computes and returns the combined antenna correction including:
     * - Satellite phase center offset (PCO) projected onto the satellite-to-receiver line-of-sight.
     * - Satellite phase center variation (PCV) interpolated from the antenna PCV grid.
     * - Receiver PCO/PCV corrections (subtracted from the measured range).
     * - Phase wind-up correction for carrier-phase observations (signal code
     *   starting with 'L', when enable_phase_windup is set).
     *
     * The implementation:
     * - Maps the RINEX signal code to the ATX frequency key for the GNSS system.
     * - Computes the satellite body frame with SatAttitude::compute (yaw-steering,
     *   orbit-normal for BeiDou-2 GEO PRNs, beta-driven modes when orbit-normal
     *   attitude is enabled).  With enableNoonTurnAttitude() a GPS Block IIF or
     *   IIR satellite near its orbit noon at small |beta| gets the yaw-rate-limited
     *   noon-turn frame instead (SatAttitude::applyNoonTurn), for the PCO and for
     *   the wind-up alike.
     * - Matches antenna models by satellite identifier (with validity window) and by
     *   receiver type (normalization, radome→NONE fallback, optional TYPE remap).
     * - Returns 0 for invalid inputs and skips missing terms, logging warnings/traces
     *   for diagnostics.
     *
     * Sign convention:
     *   Satellite PCO: APC closer to receiver than CoM → shortens range → subtract
     *   Satellite PCV: sat_apc returns (pco - pcv), caller negates → net +pcv
     *   Receiver  PCO: APC offset from ARP (mostly up) → project onto LOS → add
     *   Receiver  PCV: rec_apc adds pcv to dot(pco,los) → add
     *   total correction = -sat_pco + sat_pcv + rx_pco + rx_pcv + wind_up
     *
     * @param satellite Satellite identifier string (e.g. "G01", "R05").
     * @param freq_type RINEX signal code used by processing (e.g. "L1C", "C1C"); only
     *        the first two characters select the frequency.
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
        const Eigen::Vector3d& sun_pos_hint,
        bool skip_satellite_pco) const {

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

        // Map the RINEX signal code + system to the ATX frequency key
        // (e.g. "L5Q"/Galileo -> "E05").  Single source of truth shared with
        // the coverage queries; see signalToAtxFrequencyKey() above.
        const std::string atx_freq = signalToAtxFrequencyKey(freq_type, system);

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

        // Satellite attitude, IGS/ANTEX body frame convention.
        // Use the cached Sun position if the caller provided one (from the rigorous
        // IAU 1976/1980 precession/nutation model computed in Geometry).  Only when no
        // cached position is available (norm below 1e6 m), compute it here with the
        // same model and empty ERPs.
        Eigen::Vector3d sun_pos = sun_pos_hint;
        if (sun_pos.norm() < 1e6) {
            Eigen::Vector3d moon_dummy;
            const std::vector<ErpData> empty_erps;
            planetary_.computeSunMoonPosition(time, empty_erps, sun_pos, moon_dummy);
        }
        if (sun_pos.norm() < 1e6) {
            log_manager_.getLogger("antenna")->warn("Antenna: invalid Sun position at TOW {:.3f}, norm={:.3e}", time.tow, sun_pos.norm());
            // Continue anyway; the attitude helper uses a velocity-based frame when the
            // Sun direction is degenerate (e.g. a zero Sun position).
        }
        
        // IGS/ANTEX satellite body frame convention:
        //   Z-axis (k_s) = toward Earth (nadir / boresight)
        //   X-axis (i_s) = toward Sun (in the sat-Sun-Earth plane)
        //   Y-axis (j_s) = completes right-handed frame (cross-track, along solar panel axis)
        //
        // Exception: BeiDou-2 GEO satellites (PRN C01-C05, C59, C60) use orbit-normal
        // (ON) mode permanently (with orbit-normal attitude disabled; see below).
        // In ON mode the Y-axis is along the orbit normal
        // (cross(r, v)), X-axis is approximately along-track, and Z-axis is nadir.
        //
        // ATX satellite PCO is defined in this frame (Z toward Earth).
        // ATX satellite PCV is indexed by nadir angle = angle from boresight (+Z) to LOS.
        // The body frame (X toward Sun, Z nadir) comes from the shared attitude helper
        // SatAttitude::compute.  With enableOrbitNormalAttitude() false it gives
        // yaw-steering for every satellite and orbit-normal for the BeiDou-2 GEO PRNs
        // above (the default, byte-identical path, including the trace/warn stream
        // below); with it true the helper applies beta-driven mode selection to every
        // other BeiDou and QZSS satellite and still forces the GEO PRNs to
        // orbit-normal, whatever beta is.
        const int sat_prn = SatAttitude::prnOf(satellite);
        const bool legacy_geo = SatAttitude::usesLegacyOrbitNormal(system, sat_prn);

        // BeiDou-2 GEO degenerate-orbit-normal warning and orbit-normal trace, emitted
        // here because the stateless SatAttitude helper cannot log.  They are emitted
        // whenever legacy_geo, which keeps the flag-off trace stream byte-identical and
        // matches the orbit-normal frame the helper produces for these PRNs with the
        // flag either off or on (with it on, the helper overrides the beta table for
        // these PRNs).
        if (legacy_geo) {
            const Eigen::Vector3d on_check = sat_pos.cross(sat_vel);
            if (on_check.norm() < 1e-8) {
                log_manager_.getLogger("antenna")->warn(
                    "BeiDou GEO {}: degenerate orbit normal, using fallback", satellite);
            }
            log_manager_.getLogger("antenna")->trace(
                "BeiDou GEO {} using orbit-normal attitude", satellite);
        }

        SatAttitude::Frame att = SatAttitude::compute(
            system, sat_prn, sat_pos, sat_vel, sun_pos,
            config_.enableOrbitNormalAttitude(),
            config_.enableSunFrameCorrection());
        // att is a function-scope local that outlives every use below, so the
        // const-ref bindings are safe and copy nothing.  They follow att if the
        // noon-turn step below replaces it.
        const Eigen::Vector3d& k_s = att.k_s;  // +Z axis: toward Earth (nadir/boresight)
        const Eigen::Vector3d& i_s = att.i_s;  // +X axis: toward Sun (in sat-Sun-Earth plane)
        const Eigen::Vector3d& j_s = att.j_s;  // +Y axis: completes the right-handed frame

        // Attitude-mode trace, only with orbit-normal attitude enabled, so the
        // flag-off trace stream stays byte-identical; GEO PRNs are traced above.
        if (config_.enableOrbitNormalAttitude() &&
            att.mode != SatAttitude::Mode::YawSteering && !legacy_geo) {
            log_manager_.getLogger("antenna")->trace(
                "{} attitude mode={} beta={:.2f} deg", satellite,
                static_cast<int>(att.mode), att.beta);
        }

        // Satellite frame rotation matrix: sf = [i; j; k] (rows are basis vectors)
        // sf * vec_ecef -> vec_body; sf^T * vec_body -> vec_ecef
        Eigen::Matrix3d sat_frame;
        sat_frame.row(0) = i_s.transpose();
        sat_frame.row(1) = j_s.transpose();
        sat_frame.row(2) = k_s.transpose();

        // Satellite antenna selection with validity interval.
        //
        // The first PRN-matched entry whose validity window covers the epoch is
        // used; an entry without a validity start is used only when no covering
        // entry is found.  PRN-matched candidates are counted separately from the
        // final selection so a no-match warning can name the candidates tried
        // (with their validity windows), not only "no entry found".
        const AntennaData* sat_entry = nullptr;
        int id_matches = 0;
        std::string id_match_summary;  // first 3 candidates' types + windows
        for (const auto& a : antennas_) {
            // Match the satellite PRN at the start of the type string, or
            // delimited by '_' inside it.
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

        // Noon-turn attitude (default off).  The block, and with it the yaw-rate
        // limit, comes from the satellite's ATX type, so this follows the selection.
        // The wind-up below takes the same law, keeping PCO and wind-up on one
        // attitude.  Off, or a satellite without a law: the nominal frame stands.
        std::optional<SatAttitude::NoonTurnLaw> noon_law;
        if (config_.enableNoonTurnAttitude() && sat_entry) {
            noon_law = SatAttitude::noonTurnLaw(system, sat_entry->type);
        }
        if (noon_law) {
            att = SatAttitude::applyNoonTurn(att, *noon_law, sat_pos, sat_vel, sun_pos);
            sat_frame.row(0) = i_s.transpose();
            sat_frame.row(1) = j_s.transpose();
            if (att.mode == SatAttitude::Mode::NoonTurn) {
                log_manager_.getLogger("antenna")->trace(
                    "{} noon turn: beta={:.2f} deg, yaw {:+.1f} deg from nominal",
                    satellite, att.beta, att.yaw_offset_deg);
            }
        }

        if (!sat_entry) {
            if (id_matches == 0) {
                // No type string matched the PRN: most likely the ATX was not
                // loaded or its type strings do not carry the PRN.  The warning
                // says where to look.
                log_manager_.getLogger("antenna")->warn(
                    "No satellite antenna entry found for {} at {:d}:{:.3f}: "
                    "0 ATX entries had a type string starting with '{}_' (out of {} loaded). "
                    "Check that the ATX file was parsed (product_parser logger at debug) and "
                    "that the entries' PRN field was extracted correctly.",
                    satellite, time.week, time.tow, satellite, antennas_.size());
            } else {
                // The ATX has entries for this PRN but none covers the epoch,
                // most likely a date or validity-parse mismatch.
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
        // Pick effective ATX keys for PCO and PCV separately: an ATX entry
        // may carry PCO for a frequency without a populated PCV grid (or
        // vice versa), and the fallback chains are walked independently.
        const bool atx_fallback_on =
            config_.getModelCorrections().enable_atx_frequency_fallback;
        // Satellite antenna: intra-system fallback only; another
        // constellation's entry is never borrowed (E01 and G01 are different
        // spacecraft).  A missing satellite key falls back along the same
        // satellite's frequency chain (atxFallbackChain), else the term is 0.
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
            // skip_satellite_pco suppresses this term alone: an APC-referenced
            // (legacy-SSR) orbit already places the satellite at the phase
            // center, so the CoM->APC offset must not be applied a second time.
            // PCV (defined about the APC boresight) and all receiver-side terms
            // below are unaffected (RTB-5(c)).
            if (config_.getModelCorrections().enable_satellite_pco
                && !skip_satellite_pco
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

        // Match the receiver antenna model (sanitize, exact/substring,
        // radome→NONE fallback, optional rule-driven substitution) via the
        // shared helper.  The
        // normalized forms are echoed back for the diagnostic log lines below.
        std::string rx_type_sanitized, rx_type_underscore;
        const AntennaData* rx_entry = findReceiverAntennaEntry(
            rx_antenna_type, /*warn_on_ambiguity=*/true,
            &rx_type_sanitized, &rx_type_underscore);

        // === Receiver ===
        // Independent fallback walks for PCO and PCV (same rationale as
        // the satellite side above).  Unlike the satellite, the receiver
        // may additionally cross to the nearest GPS band (E1->G01, E5a->G05,
        // ...) when enable_receiver_cross_system_atx_fallback is set: the
        // receiver phase pattern is a function of frequency on shared
        // hardware, so the substitution is physical.
        const bool rx_xsys =
            config_.getModelCorrections().enable_receiver_cross_system_atx_fallback;
        const std::string rx_pco_freq = rx_entry
            ? pickReceiverAtxFrequencyKey(rx_entry->pco, atx_freq, atx_fallback_on, rx_xsys,
                                  [](const Eigen::Vector3d& v) { return v.allFinite(); })
            : std::string{};
        const std::string rx_pcv_freq = rx_entry
            ? pickReceiverAtxFrequencyKey(rx_entry->pcv, atx_freq, atx_fallback_on, rx_xsys,
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
                        const bool xsys = !atx_freq.empty() && atx_freq[0] != 'G'
                                          && !rx_pco_freq.empty() && rx_pco_freq[0] == 'G';
                        log_manager_.getLogger("antenna")->debug(
                            "RX PCO fallback for {} '{}': requested freq={} missing, using {}{}",
                            satellite, rx_entry->type, atx_freq, rx_pco_freq,
                            xsys ? " [cross-system]" : "");
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
                    const bool xsys = !atx_freq.empty() && atx_freq[0] != 'G'
                                      && !rx_pcv_freq.empty() && rx_pcv_freq[0] == 'G';
                    log_manager_.getLogger("antenna")->debug(
                        "RX PCV fallback for {} '{}': requested freq={} missing, using {}{}",
                        satellite, rx_entry->type, atx_freq, rx_pcv_freq,
                        xsys ? " [cross-system]" : "");
                }
                double zenith = 90.0 - elevation;
                double dazi = rx_entry->dazi > 0.0 ? rx_entry->dazi : 5.0;
                double dzen = rx_entry->dzen > 0.0 ? rx_entry->dzen : 5.0;
                double zen1 = rx_entry->zen1;
                // Receiver antenna mounting orientation: rotate the measured
                // azimuth into the antenna frame before the PCV look-up.  Only
                // the receiver PCV uses the measured azimuth: the receiver PCO is
                // a LOS projection and the satellite PCV uses the satellite
                // body-frame azimuth (az_sat above), so both are deliberately
                // left unrotated.  With the option off, pcv_azimuth == azimuth
                // exactly and the output is byte-identical.
                double pcv_azimuth = azimuth;
                if (config_.enableRxAntennaOrientation()) {
                    const double ori_az = config_.getReceiverAntennaOrientation().azimuth_deg;
                    pcv_azimuth = correctAzimuth(azimuth, ori_az);
                    log_manager_.getLogger("antenna")->trace(
                        "RX PCV orientation for {} freq={}: az_meas={:.4f} - ori={:.4f} -> az_pcv={:.4f} deg",
                        satellite, rx_pcv_freq, azimuth, ori_az, pcv_azimuth);
                }
                double pcv_raw = interpolatePcv(pcv_table, zenith - zen1, pcv_azimuth, dazi, dzen);
                double pcv = pcv_raw * 1e-3;
                if (std::isfinite(pcv)) {
                    rx_pcv_corr = pcv;
                    correction += rx_pcv_corr;

                    log_manager_.getLogger("antenna")->trace(
                        "RX PCV {} freq={} type='{}': elev={:.4f} zen={:.4f} az={:.4f} "
                        "zen1={:.1f} dzen={:.1f} dazi={:.1f} grid={}x{} "
                        "interp_arg={:.4f} pcv_raw_mm={:.4f} pcv_m={:.6f}",
                        satellite, rx_pcv_freq, rx_entry->type,
                        elevation, zenith, pcv_azimuth,
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
            double wind_up = gnss_corrections_.computePhaseWindUp(satellite, system, rx_pos, sat_pos, wavelength, time, sun_pos, sat_vel, config_.accumulatePhaseWindUp(), config_.phaseWindUpMaxGapS(), config_.enableSunFrameCorrection(),
                noon_law ? &*noon_law : nullptr);
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
        const std::string atx_freq = signalToAtxFrequencyKey(signal, system);
        if (atx_freq.empty()) return false;

        // --- Find receiver antenna entry (shared matching) ---
        const AntennaData* rx_entry = findReceiverAntennaEntry(
            rinex_parser_.getReceiverAntennaType(), /*warn_on_ambiguity=*/false);
        if (!rx_entry) return false;
        if (rx_entry->pco.count(atx_freq) > 0) return true;
        // Receiver-only cross-system fallback: the nearest GPS band stands in
        // for a missing non-GPS key (E1->G01, E5a->G05, ...).  Gated on the
        // opt-in toggle; the satellite antenna is never loosened this way.
        if (config_.getModelCorrections().enable_receiver_cross_system_atx_fallback) {
            const auto& xs = atxReceiverCrossSystemChain();
            auto it = xs.find(atx_freq);
            if (it != xs.end()) {
                for (const auto& alt : it->second)
                    if (rx_entry->pco.count(alt) > 0) return true;
            }
        }
        return false;
    }

    std::string Antenna::receiverAtxFrequencySource(const std::string& signal, System system,
                                                    std::string* out_key) const {
        if (out_key) out_key->clear();
        const std::string atx_freq = signalToAtxFrequencyKey(signal, system);
        if (atx_freq.empty()) return "none";
        const AntennaData* rx_entry = findReceiverAntennaEntry(
            rinex_parser_.getReceiverAntennaType(), /*warn_on_ambiguity=*/false);
        if (!rx_entry) return "none";
        const auto& mc = config_.getModelCorrections();
        const std::string key = pickReceiverAtxFrequencyKey(
            rx_entry->pco, atx_freq, mc.enable_atx_frequency_fallback,
            mc.enable_receiver_cross_system_atx_fallback,
            [](const Eigen::Vector3d& v) { return v.allFinite(); });
        if (out_key) *out_key = key;
        if (key.empty()) return "none";
        if (key == atx_freq) return "exact";
        // Same constellation letter: the in-system frequency chain supplied it.
        if (key.front() == atx_freq.front()) return "same_system";
        return "cross_system";
    }

    bool Antenna::hasReceiverAntennaMatch() const {
        // Same matching the correction path runs (rinex_parser_.getReceiverAntennaType()
        // → findReceiverAntennaEntry, with the radome→NONE and opt-in substitution
        // fallbacks), but with warn_on_ambiguity=false so this query emits no ambiguity
        // warning and does not mutate state.  The substitution warning still fires when
        // the opt-in table supplies the entry.
        return findReceiverAntennaEntry(
            rinex_parser_.getReceiverAntennaType(), /*warn_on_ambiguity=*/false) != nullptr;
    }

    bool Antenna::wasAtxSubstituted(const std::string& satellite,
                                     const std::string& signal,
                                     System system) const {
        // Substituted := strict coverage absent, fallback enabled, and at
        // least one of the four entries (sat PCO, sat PCV, rx PCO, rx PCV)
        // resolved via the in-system fallback chain to a non-empty alternate
        // frequency.  The receiver cross-system chain is not walked here.
        if (hasFullAtxCoverage(satellite, signal, system)) return false;
        if (!config_.getModelCorrections().enable_atx_frequency_fallback) return false;

        const std::string atx_freq = signalToAtxFrequencyKey(signal, system);
        if (atx_freq.empty()) return false;

        // Locate satellite entry (same matching logic as hasFullAtxCoverage:
        // first PRN match, validity window not checked).
        const AntennaData* sat_entry = nullptr;
        for (const auto& a : antennas_) {
            if (a.type.find(satellite + "_") == 0 ||
                a.type.find("_" + satellite + "_") != std::string::npos) {
                sat_entry = &a;
                break;
            }
        }

        // Locate receiver entry (shared matching logic).
        const AntennaData* rx_entry = findReceiverAntennaEntry(
            rinex_parser_.getReceiverAntennaType(), /*warn_on_ambiguity=*/false);

        // Helper: given an entry map and a "good" predicate, return the
        // resolved frequency key (exact or fallback) from pickAtxFrequencyKey
        // with the fallback forced on; comparing it with atx_freq separates an
        // exact key from a substituted one.
        auto pcoGood = [](const Eigen::Vector3d& v) { return v.allFinite(); };
        auto pcvGood = [](const std::vector<std::vector<double>>& v) { return !v.empty(); };

        auto picked = [&](const auto& map, auto good) -> std::string {
            return pickAtxFrequencyKey(map, atx_freq, true, good);
        };

        bool any_substituted = false;
        if (sat_entry) {
            const std::string k1 = picked(sat_entry->pco, pcoGood);
            const std::string k2 = picked(sat_entry->pcv, pcvGood);
            if ((!k1.empty() && k1 != atx_freq) ||
                (!k2.empty() && k2 != atx_freq)) any_substituted = true;
        }
        if (rx_entry) {
            const std::string k3 = picked(rx_entry->pco, pcoGood);
            const std::string k4 = picked(rx_entry->pcv, pcvGood);
            if ((!k3.empty() && k3 != atx_freq) ||
                (!k4.empty() && k4 != atx_freq)) any_substituted = true;
        }
        return any_substituted;
    }

    bool Antenna::hasFullAtxCoverage(const std::string& satellite,
                                      const std::string& signal,
                                      System system) const {
        // Strict coverage: the (sat, signal) is "in the ATX" only when
        // both the satellite and receiver entries supply finite PCO and
        // populated PCV grids for the *exact* requested frequency, never
        // via the @c enable_atx_frequency_fallback chain (the receiver
        // cross-system exception is below).  This is the gate the
        // @c require_atx_coverage flag opts into.
        const std::string atx_freq = signalToAtxFrequencyKey(signal, system);
        if (atx_freq.empty()) return false;

        auto entryHasPcoAndPcvFor = [](const AntennaData& a,
                                       const std::string& key) -> bool {
            const auto pco_it = a.pco.find(key);
            if (pco_it == a.pco.end()) return false;
            if (!pco_it->second.allFinite()) return false;
            const auto pcv_it = a.pcv.find(key);
            if (pcv_it == a.pcv.end()) return false;
            if (pcv_it->second.empty()) return false;
            return true;
        };
        auto entryHasPcoAndPcv = [&atx_freq, &entryHasPcoAndPcvFor](
                                     const AntennaData& a) -> bool {
            return entryHasPcoAndPcvFor(a, atx_freq);
        };

        // --- Satellite entry: ATX type strings are typically of the
        // form "<PRN>_BLOCK..._<SVN>", so a substring match on the
        // PRN works for both leading and middle positions.  The first match
        // is used; its validity window is not checked.
        const AntennaData* sat_entry = nullptr;
        for (const auto& a : antennas_) {
            if (a.type.find(satellite + "_") == 0 ||
                a.type.find("_" + satellite + "_") != std::string::npos) {
                sat_entry = &a;
                break;
            }
        }
        if (!sat_entry || !entryHasPcoAndPcv(*sat_entry)) return false;

        // --- Receiver entry (shared matching logic) ---
        const AntennaData* rx_entry = findReceiverAntennaEntry(
            rinex_parser_.getReceiverAntennaType(), /*warn_on_ambiguity=*/false);
        if (!rx_entry) return false;
        if (entryHasPcoAndPcv(*rx_entry)) return true;   // strict, exact freq
        // Receiver-only cross-system fallback: the nearest GPS band supplies
        // the missing non-GPS key when the opt-in toggle is set.  The
        // satellite half above stays strict and is never loosened this way,
        // so a missing satellite entry still fails coverage.
        if (config_.getModelCorrections().enable_receiver_cross_system_atx_fallback) {
            const auto& xs = atxReceiverCrossSystemChain();
            auto it = xs.find(atx_freq);
            if (it != xs.end()) {
                for (const auto& alt : it->second)
                    if (entryHasPcoAndPcvFor(*rx_entry, alt)) return true;
            }
        }
        return false;
    }

    /**
     * @brief Return the receiver antenna type string from the RINEX parser.
     *
     * The returned string is sanitized (whitespace removed) to ease matching
     * against ATX entries.  An empty type from the parser is logged as a warning.
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
     * @brief Report the cached antenna model count.
     *
     * `antennas_` is a reference into the ProductCache, which holds the parsed
     * ATX entries, so nothing is loaded here; the method only logs the entry
     * count at debug level.
     */
    void Antenna::loadAntennaData() {
        // Data accessed via reference to ProductCache; nothing to load.
        log_manager_.getLogger("antenna_model")->debug(
            "Using cached antenna reference (zero-copy): {} entries",
            antennas_.size());
    }

    void Antenna::dumpAntennaModels() const {
        auto logger = log_manager_.getLogger("antenna_model");

        // Only dump the receiver antenna model that matches the RINEX header.
        // Satellite antenna entries are already traced per-observation in
        // getAntennaCorrection(); dumping all of them here would add
        // thousands of lines.
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

        // Find the matching entry: exact, then first substring, match as in the
        // primary step of getAntennaCorrection(); the radome→NONE fallback and the
        // TYPE substitution are not applied, so a station matched only through
        // them logs "no ATX match" here.
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

    void Antenna::enableWindUpCensus(double wu_gap_s, double carry_gap_s) {
        gnss_corrections_.enableWindUpCensus(wu_gap_s, carry_gap_s);
    }

    const GnssCorrections::WindUpCensus& Antenna::windUpCensus() const {
        return gnss_corrections_.windUpCensus();
    }

    const std::map<std::string, GnssCorrections::WuRestart>& Antenna::wuRestarts() const {
        return gnss_corrections_.wuRestarts();
    }

    void Antenna::clearWuRestarts() {
        gnss_corrections_.clearWuRestarts();
    }

    bool Antenna::enableWindUpTracing(const std::filesystem::path& log_dir) {
        return gnss_corrections_.enableWindUpTracing(log_dir);
    }

    /**
     * @brief Interpolate a PCV grid.
     *
     * Supports both azimuth-independent tables (single column) and full 2D
     * grids. Linear interpolation is used between zenith rings and azimuth
     * sectors; azimuth wraps around circularly.  At or beyond the last zenith
     * ring the value is held flat.
     *
     * @param pcv_table 2D PCV table: rows=zenith rings, cols=azimuth sectors (or 1 for azimuth-independent).
     * @param zenith Zenith (or nadir) angle in degrees, relative to ZEN1 (the caller
     *        subtracts ZEN1); negative values are clamped to 0.
     * @param azimuth Azimuth angle in degrees (0..360).
     * @param dazi Azimuth spacing in degrees between PCV columns.
     * @param dzen Zenith spacing in degrees between PCV rows; <= 0 returns 0.
     * @return Interpolated PCV value in the table's units (mm for ATX tables; the
     *         caller scales by 1e-3).  0 for an empty table.
     */
    double Antenna::interpolatePcv(const std::vector<std::vector<double>>& pcv_table,
                                   double zenith,
                                   double azimuth,
                                   double dazi,
                                   double dzen) {
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
            return pcv;
        }

        size_t az_idx = static_cast<size_t>(azimuth / dazi);
        double az_frac = (azimuth - az_idx * dazi) / dazi;
        if (az_idx >= cols) {
            az_idx = cols - 1;
            az_frac = 0.0;
        }
        size_t az_next = (az_idx + 1) % cols;

        // Bilinear corners.  The next-zenith-ring rows default to the
        // current ring so that a query in the last ring (zen_idx + 1 ==
        // size, i.e. zenith >= the table's largest angle) holds the value
        // flat instead of reading past the table.  The next-ring rows are
        // read only when that ring exists; the bounds check must precede the
        // access.
        double pcv00 = pcv_table[zen_idx][az_idx];
        double pcv01 = pcv_table[zen_idx][az_next];
        double pcv10 = pcv00;
        double pcv11 = pcv01;
        if (zen_idx + 1 < pcv_table.size()) {
            pcv10 = pcv_table[zen_idx + 1][az_idx];
            pcv11 = pcv_table[zen_idx + 1][az_next];
        }

        double pcv =
            pcv00 * (1 - zen_frac) * (1 - az_frac) +
            pcv10 * zen_frac * (1 - az_frac) +
            pcv01 * (1 - zen_frac) * az_frac +
            pcv11 * zen_frac * az_frac;
        return pcv;
    }

}