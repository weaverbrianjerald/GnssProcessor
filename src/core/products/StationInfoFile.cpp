/**
 * @file StationInfoFile.cpp
 * @brief Implementation of the SIF loader / registry (see StationInfoFile.hpp).
 */

#include "products/StationInfoFile.hpp"
#include "math/Coordinates.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <utility>
#include <vector>

namespace GnssProcessor {

namespace {

    /// Read a numeric field, returning @p def if missing/non-numeric.
    double jnum(const nlohmann::json& j, const char* key, double def = 0.0) {
        auto it = j.find(key);
        return (it != j.end() && it->is_number()) ? it->get<double>() : def;
    }

    /// Read a string field, returning empty if missing/non-string.
    std::string jstr(const nlohmann::json& j, const char* key) {
        auto it = j.find(key);
        return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
    }

    /// Read a boolean field, returning @p def if missing/non-boolean.
    bool jbool(const nlohmann::json& j, const char* key, bool def = false) {
        auto it = j.find(key);
        return (it != j.end() && it->is_boolean()) ? it->get<bool>() : def;
    }

    /// Read a 3-element numeric array into a vector (zeros if missing/short).
    Eigen::Vector3d jvec3(const nlohmann::json& j, const char* key) {
        Eigen::Vector3d v = Eigen::Vector3d::Zero();
        auto it = j.find(key);
        if (it != j.end() && it->is_array() && it->size() >= 3) {
            for (int k = 0; k < 3; ++k)
                if ((*it)[k].is_number()) v[k] = (*it)[k].get<double>();
        }
        return v;
    }

    /// Read a numeric array of arbitrary length (empty if missing).
    std::vector<double> jarr(const nlohmann::json& j, const char* key) {
        std::vector<double> out;
        auto it = j.find(key);
        if (it != j.end() && it->is_array()) {
            out.reserve(it->size());
            for (const auto& e : *it)
                if (e.is_number()) out.push_back(e.get<double>());
        }
        return out;
    }

    /// Trim ASCII whitespace, keeping case (for equipment TYPE / serial compares).
    std::string trimCopy(const std::string& s) {
        const auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return {};
        const auto e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    /// Trim ASCII whitespace and upper-case (for case-insensitive key matching).
    std::string upperTrim(std::string s) {
        s = trimCopy(s);
        for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    /// Convert a SIF timestamp ("YYYY-MM-DDTHH:MM:SS TZ") to a decimal year.
    ///
    /// The timezone suffix (UTC / GPS) is ignored: the GPS-UTC offset (18 s or
    /// less) is far below the resolution that decimal-year window selection and
    /// velocity propagation need.  Any year >= 9000 (an open-ended SIF END such as
    /// "9999-...") maps to 9999.0, so it compares as later than any real epoch.
    /// A string with fewer than three parsable date fields returns 0.0.
    double decimalYearFromSif(const std::string& s) {
        int Y = 0, M = 1, D = 1, h = 0, mi = 0, se = 0;
        if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &mi, &se) < 3)
            return 0.0;
        if (Y >= 9000) return 9999.0;  // open-ended END sentinel
        if (M < 1 || M > 12) M = 1;
        if (D < 1) D = 1;
        static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
        const bool leap = (Y % 4 == 0 && (Y % 100 != 0 || Y % 400 == 0));
        const int doy = cum[M - 1] + (D - 1) + ((leap && M > 2) ? 1 : 0);
        const double diy = leap ? 366.0 : 365.0;
        return Y + (doy + (h * 3600.0 + mi * 60.0 + se) / 86400.0) / diy;
    }

} // namespace

StationInfoFile::StationInfoFile(LogManager& log_manager)
    : log_manager_(log_manager) {}

std::size_t StationInfoFile::load(const std::filesystem::path& sif_json) {
    auto log = log_manager_.getLogger("station_info");

    std::ifstream f(sif_json);
    if (!f.is_open()) {
        log->error("SIF: cannot open '{}'", sif_json.string());
        return 0;
    }

    nlohmann::json doc;
    try {
        f >> doc;
    } catch (const std::exception& e) {
        log->error("SIF: JSON parse error in '{}': {}", sif_json.string(), e.what());
        return 0;
    }

    double meta_epoch = 2020.0;
    std::string meta_frame;
    if (auto mit = doc.find("METADATA"); mit != doc.end() && mit->is_object()) {
        meta_epoch = jnum(*mit, "REFERENCE_EPOCH", 2020.0);
        meta_frame = jstr(*mit, "REFERENCE_FRAME");
    }

    auto sit = doc.find("STATIONS");
    if (sit == doc.end() || !sit->is_array()) {
        log->error("SIF: '{}' has no STATIONS array", sif_json.string());
        return 0;
    }

    std::size_t added = 0;
    for (const auto& sj : *sit) {
        StationInfo st;
        st.id = jstr(sj, "ID");
        if (st.id.empty()) continue;  // a station with no ID is unusable
        st.domes = jstr(sj, "DOMES");
        st.description = jstr(sj, "DESCRIPTION");
        st.reference_frame = meta_frame;
        st.reference_epoch_year = meta_epoch;
        st.approximate_coordinates = jvec3(sj, "APPROXIMATE_COORDINATES");

        // --- Ocean tidal loading (FES2014b, Up/East/North amplitudes & phases) ---
        if (auto otlit = sj.find("OCEAN_TIDE_LOADING"); otlit != sj.end() && otlit->is_object()) {
            bool any = false;
            for (std::size_t k = 0; k < 11; ++k) {
                auto cit = otlit->find(SIF_OTL_CONSTITUENTS[k]);
                if (cit != otlit->end() && cit->is_object()) {
                    SifOtlConstituent c;
                    c.up_amp    = jnum(*cit, "UP_AMPLITUDE");
                    c.east_amp  = jnum(*cit, "EAST_AMPLITUDE");
                    c.north_amp = jnum(*cit, "NORTH_AMPLITUDE");
                    c.up_phase    = jnum(*cit, "UP_PHASE");
                    c.east_phase  = jnum(*cit, "EAST_PHASE");
                    c.north_phase = jnum(*cit, "NORTH_PHASE");
                    st.otl[k] = c;
                    any = true;
                }
            }
            st.has_otl = any;
        }

        // --- Ocean pole tide (Desai load coefficients, NEU real/imag) ---
        if (auto optit = sj.find("OCEAN_POLE_TIDE"); optit != sj.end() && optit->is_object()) {
            st.opt.north_real = jnum(*optit, "NORTH_REAL");
            st.opt.north_imag = jnum(*optit, "NORTH_IMAGINARY");
            st.opt.east_real  = jnum(*optit, "EAST_REAL");
            st.opt.east_imag  = jnum(*optit, "EAST_IMAGINARY");
            st.opt.up_real    = jnum(*optit, "UP_REAL");
            st.opt.up_imag    = jnum(*optit, "UP_IMAGINARY");
            st.opt.valid = true;
        }

        // --- Antenna history ---
        if (auto ant = sj.find("ANTENNA"); ant != sj.end() && ant->is_array()) {
            st.antennas.reserve(ant->size());
            for (const auto& aj : *ant) {
                SifAntennaWindow w;
                w.start_year = decimalYearFromSif(jstr(aj, "START"));
                w.end_year   = decimalYearFromSif(jstr(aj, "END"));
                w.type   = jstr(aj, "TYPE");
                w.serial = jstr(aj, "SERIAL_NUMBER");
                w.arp_minus_grp_enu = jvec3(aj, "ARP_MINUS_GRP_ENU");
                w.orientation_deg = jnum(aj, "ORIENTATION");
                w.do_not_use = jbool(aj, "DO_NOT_USE");
                st.antennas.push_back(std::move(w));
            }
        }

        // --- Receiver history ---
        if (auto rec = sj.find("RECEIVER"); rec != sj.end() && rec->is_array()) {
            st.receivers.reserve(rec->size());
            for (const auto& rj : *rec) {
                SifReceiverWindow w;
                w.start_year = decimalYearFromSif(jstr(rj, "START"));
                w.end_year   = decimalYearFromSif(jstr(rj, "END"));
                w.type     = jstr(rj, "TYPE");
                w.firmware = jstr(rj, "FIRMWARE");
                w.satellite_systems = jstr(rj, "SATELLITE_SYSTEMS");
                w.do_not_use = jbool(rj, "DO_NOT_USE");
                st.receivers.push_back(std::move(w));
            }
        }

        // --- Occupation (ITRF coordinate solutions) ---
        if (auto occ = sj.find("OCCUPATION"); occ != sj.end() && occ->is_array()) {
            st.occupations.reserve(occ->size());
            for (const auto& oj : *occ) {
                SifOccupation o;
                o.start_year = decimalYearFromSif(jstr(oj, "START"));
                o.end_year   = decimalYearFromSif(jstr(oj, "END"));
                // Each record's own epoch (the NCN cors_ex records carry 2010.0
                // under a 2020.0 METADATA epoch); METADATA only when absent.
                o.reference_epoch_year = jnum(oj, "REFERENCE_EPOCH", meta_epoch);
                o.coordinates  = jvec3(oj, "COORDINATES");
                o.coord_sigmas = jvec3(oj, "COORDINATE_SIGMAS");          // metres (COORDINATE_UNITS=m)
                o.velocity         = jvec3(oj, "VELOCITY") * 1.0e-3;      // mm/y -> m/y
                o.velocity_sigmas  = jvec3(oj, "VELOCITY_SIGMAS") * 1.0e-3;
                o.do_not_use = jbool(oj, "DO_NOT_USE");

                // Assemble the full symmetric 6x6 [X Y Z Vx Vy Vz] covariance.  The
                //   diagonal comes from the sigma arrays (already SI above); the
                //   off-diagonal comes from the 15-element 6X6_COVARIANCE_MATRIX
                //   (mm^2 -> m^2, one 1e-6 factor for every cross term, position and
                //   velocity alike), in row-major upper-triangle order, which is the
                //   SIF/M-PAGES convention.
                const std::vector<double> c = jarr(oj, "6X6_COVARIANCE_MATRIX");
                if (c.size() >= 15) {
                    Eigen::Matrix<double, 6, 6>& C = o.covariance;
                    C.setZero();
                    C(0, 0) = o.coord_sigmas[0] * o.coord_sigmas[0];
                    C(1, 1) = o.coord_sigmas[1] * o.coord_sigmas[1];
                    C(2, 2) = o.coord_sigmas[2] * o.coord_sigmas[2];
                    C(3, 3) = o.velocity_sigmas[0] * o.velocity_sigmas[0];
                    C(4, 4) = o.velocity_sigmas[1] * o.velocity_sigmas[1];
                    C(5, 5) = o.velocity_sigmas[2] * o.velocity_sigmas[2];
                    constexpr double s = 1.0e-6;  // mm^2 -> m^2
                    C(0, 1) = s * c[0];  C(0, 2) = s * c[1];  C(0, 3) = s * c[2];
                    C(0, 4) = s * c[3];  C(0, 5) = s * c[4];
                    C(1, 2) = s * c[5];  C(1, 3) = s * c[6];  C(1, 4) = s * c[7];
                    C(1, 5) = s * c[8];
                    C(2, 3) = s * c[9];  C(2, 4) = s * c[10]; C(2, 5) = s * c[11];
                    C(3, 4) = s * c[12]; C(3, 5) = s * c[13];
                    C(4, 5) = s * c[14];
                    for (int i = 0; i < 6; ++i)
                        for (int j = i + 1; j < 6; ++j) C(j, i) = C(i, j);
                    o.has_covariance = true;
                }
                st.occupations.push_back(std::move(o));
            }
        }

        // --- Nonlinear terms: periodic (seasonal) + post-seismic deformation ---
        if (auto nlt = sj.find("NONLINEAR_TERMS"); nlt != sj.end() && nlt->is_object()) {
            if (auto per = nlt->find("PERIODIC_TERMS"); per != nlt->end() && per->is_array()) {
                st.periodic.reserve(per->size());
                for (const auto& bj : *per) {
                    SifPeriodicBlock b;
                    b.start_year = decimalYearFromSif(jstr(bj, "START"));
                    b.end_year   = decimalYearFromSif(jstr(bj, "END"));
                    b.reference_epoch_year = meta_epoch;
                    if (auto fr = bj.find("FREQUENCIES"); fr != bj.end() && fr->is_array()) {
                        b.freqs.reserve(fr->size());
                        for (const auto& fj : *fr) {
                            SifPeriodicFreq pf;
                            pf.period_yr = jnum(fj, "PERIOD", 1.0);
                            constexpr double mm = 1.0e-3;  // mm -> m (PERIODIC_COEFF_UNITS=mm)
                            pf.x_cos = mm * jnum(fj, "X_COS");
                            pf.x_sin = mm * jnum(fj, "X_SIN");
                            pf.y_cos = mm * jnum(fj, "Y_COS");
                            pf.y_sin = mm * jnum(fj, "Y_SIN");
                            pf.z_cos = mm * jnum(fj, "Z_COS");
                            pf.z_sin = mm * jnum(fj, "Z_SIN");
                            b.freqs.push_back(pf);
                        }
                    }
                    st.periodic.push_back(std::move(b));
                }
            }

            if (auto psd = nlt->find("POST_SEISMIC"); psd != nlt->end() && psd->is_array()) {
                st.psd.reserve(psd->size());
                for (const auto& pj : *psd) {
                    SifPsdBlock pb;
                    pb.event_year = decimalYearFromSif(jstr(pj, "EVENT_EPOCH"));
                    const auto readComp = [&](const char* key, SifPsdComponent& comp) {
                        if (auto cit = pj.find(key); cit != pj.end() && cit->is_object()) {
                            comp.aexp = jarr(*cit, "AEXP");  // PSD_COEFF_UNITS=m, PSD_TIME_UNITS=y
                            comp.texp = jarr(*cit, "TEXP");
                            comp.alog = jarr(*cit, "ALOG");
                            comp.tlog = jarr(*cit, "TLOG");
                        }
                    };
                    readComp("EAST",  pb.east);
                    readComp("NORTH", pb.north);
                    readComp("UP",    pb.up);
                    st.psd.push_back(std::move(pb));
                }
            }
        }

        // --- Index and store ---
        const std::size_t idx = stations_.size();
        const std::string id_u = upperTrim(st.id);
        const std::string dm   = upperTrim(st.domes);
        stations_.push_back(std::move(st));
        if (!id_u.empty()) {
            by_id_.emplace(id_u, idx);
            if (id_u.size() >= 4) by_id4_.emplace(id_u.substr(0, 4), idx);  // first wins
        }
        if (!dm.empty()) by_domes_.emplace(dm, idx);
        ++added;
    }

    log->info("SIF: ingested {} stations from '{}'", added, sif_json.string());
    return added;
}

namespace {

    /// A position the prefix guard can use: all finite and off the origin by more
    /// than 1e6 m (rejects the zero vector a missing RINEX APPROX header leaves).
    bool usableEcef(const Eigen::Vector3d& p) {
        return p.allFinite() && p.norm() > 1e6;
    }

    /// Reference position of a prefix candidate for the distance test:
    /// APPROXIMATE_COORDINATES, else the first non-DO_NOT_USE occupation's
    /// coordinates, else the first occupation's; the first usable one wins.
    /// Empty when none is usable.
    std::optional<Eigen::Vector3d> prefixReferencePosition(const StationInfo& s) {
        if (usableEcef(s.approximate_coordinates)) return s.approximate_coordinates;
        for (const auto& o : s.occupations) {
            if (o.do_not_use) continue;
            if (usableEcef(o.coordinates)) return o.coordinates;
            break;  // only the FIRST non-flagged occupation is consulted
        }
        if (!s.occupations.empty() && usableEcef(s.occupations.front().coordinates))
            return s.occupations.front().coordinates;
        return std::nullopt;
    }

} // namespace

const StationInfo* StationInfoFile::find(const std::string& station_key,
                                         const Eigen::Vector3d* approx_ecef,
                                         double max_prefix_distance_m) const {
    const std::string k = upperTrim(station_key);
    if (k.empty()) return nullptr;
    // Exact matches are authoritative: no position test.
    if (auto it = by_id_.find(k); it != by_id_.end()) return &stations_[it->second];
    if (auto it = by_domes_.find(k); it != by_domes_.end()) return &stations_[it->second];
    if (k.size() < 4) return nullptr;
    const auto it4 = by_id4_.find(k.substr(0, 4));
    if (it4 == by_id4_.end()) return nullptr;

    // A 4-char prefix match is only a candidate: a 9-char ID missing from the
    // registry can share its code with an unrelated station (GAMB00PYF vs
    // GAMB00USA, 7,688 km apart).  Confirm it by position when the caller has
    // one, else accept only a bare 4-char key.
    const StationInfo& cand = stations_[it4->second];
    if (approx_ecef != nullptr && usableEcef(*approx_ecef)) {
        const std::optional<Eigen::Vector3d> ref = prefixReferencePosition(cand);
        if (!ref) {
            log_manager_.getLogger("station_info")->warn(
                "SIF: '{}' is not a station ID or DOMES; 4-char prefix candidate {} "
                "rejected (the candidate has no usable reference position to compare "
                "with the approximate position)", trimCopy(station_key), cand.id);
            return nullptr;
        }
        const double dist_m = (*ref - *approx_ecef).norm();
        if (dist_m <= max_prefix_distance_m) return &cand;
        log_manager_.getLogger("station_info")->warn(
            "SIF: '{}' is not a station ID or DOMES; 4-char prefix candidate {} "
            "rejected: {:.1f} m from the approximate position (limit {:.1f} m)",
            trimCopy(station_key), cand.id, dist_m, max_prefix_distance_m);
        return nullptr;
    }
    if (k.size() == 4) return &cand;  // a bare code carries no conflicting information
    log_manager_.getLogger("station_info")->warn(
        "SIF: '{}' is not a station ID or DOMES; 4-char prefix candidate {} rejected "
        "(no approximate position to confirm it; without one only a bare 4-char "
        "code is accepted)", trimCopy(station_key), cand.id);
    return nullptr;
}

namespace {

    /// Pick the first non-flagged OCCUPATION record covering @p year
    /// (start <= year < end); otherwise the non-flagged record whose window
    /// midpoint is nearest @p year (an open END is capped at 9000 for the
    /// midpoint).  Returns nullptr when every record is flagged DO_NOT_USE.
    const SifOccupation* selectOccupation(const StationInfo& s, double year) {
        const SifOccupation* best = nullptr;
        double best_dist = 1e30;
        for (const auto& o : s.occupations) {
            if (o.do_not_use) continue;
            if (year >= o.start_year && year < o.end_year) return &o;  // exact window
            const double mid = 0.5 * (o.start_year + std::min(o.end_year, 9000.0));
            const double dist = std::abs(year - mid);
            if (dist < best_dist) { best_dist = dist; best = &o; }
        }
        return best;
    }

    /// True iff some window of @p wins other than the first, in start order,
    /// starts inside [lo, hi] (inclusive) and @p differs from the window before
    /// it.  Windows sharing a start keep their file order (stable sort).
    template <class Window, class Differs>
    bool equipmentChangeInGap(const std::vector<Window>& wins, double lo, double hi,
                              Differs differs) {
        std::vector<const Window*> by_start;
        by_start.reserve(wins.size());
        for (const auto& w : wins) by_start.push_back(&w);
        std::stable_sort(by_start.begin(), by_start.end(),
                         [](const Window* a, const Window* b) { return a->start_year < b->start_year; });
        for (std::size_t i = 1; i < by_start.size(); ++i) {
            const double st = by_start[i]->start_year;
            if (st < lo || st > hi) continue;
            if (differs(*by_start[i - 1], *by_start[i])) return true;
        }
        return false;
    }

    /// The periodic block that supplies the seasonal term at one epoch, and the
    /// selection case that chose it.  @c block is null (and @c reach
    /// NotEvaluated) when the station has no periodic block.
    struct PeriodicChoice {
        const SifPeriodicBlock* block{ nullptr };
        SifPeriodicReach reach{ SifPeriodicReach::NotEvaluated };
    };

    /// Choose the periodic block for @p year, following NGS M-PAGES v1.2
    /// (PeriodicMotion::computeDisplacement).  The blocks are taken in start
    /// order (file order for equal starts; the file order itself is not
    /// chronological for every station).  The first block whose window covers
    /// the year (start <= year < end) wins, so overlapping windows are not
    /// summed.  With none covering: before the first window the first block,
    /// at or after the end of the last window the last block, and inside a gap
    /// the last block, in start order, that ended at or before the year.
    PeriodicChoice selectPeriodicBlock(const StationInfo& s, double year) {
        PeriodicChoice out;
        if (s.periodic.empty()) return out;

        std::vector<const SifPeriodicBlock*> by_start;
        by_start.reserve(s.periodic.size());
        for (const auto& b : s.periodic) by_start.push_back(&b);
        std::stable_sort(by_start.begin(), by_start.end(),
                         [](const SifPeriodicBlock* a, const SifPeriodicBlock* b) {
                             return a->start_year < b->start_year;
                         });

        const SifPeriodicBlock* last_ended = nullptr;  // last block, in start order, with end <= year
        for (const SifPeriodicBlock* b : by_start) {
            if (year >= b->start_year && year < b->end_year) {
                out.block = b;
                out.reach = SifPeriodicReach::Covered;
                return out;
            }
            if (b->end_year <= year) last_ended = b;
        }

        if (year < by_start.front()->start_year) {
            out.block = by_start.front();
            out.reach = SifPeriodicReach::BeforeFirst;
        } else if (year >= by_start.back()->end_year) {
            out.block = by_start.back();
            out.reach = SifPeriodicReach::AfterLast;
        } else {
            // Past the first start and covered by nothing: the first block has
            // ended, so last_ended is set.
            out.block = last_ended;
            out.reach = SifPeriodicReach::InGap;
        }
        return out;
    }

} // namespace

std::string describePeriodicExtrapolation(const SifPeriodicEvaluation& p) {
    if (!p.extrapolated()) return {};
    const double yrs = std::abs(p.extrapolation_years);
    switch (p.reach) {
    case SifPeriodicReach::BeforeFirst:
        return fmt::format("epoch is {:.3f} yr before the first periodic block [{:.4f}, {:.4f}); "
                           "that block's seasonal term is applied",
                           yrs, p.block_start_year, p.block_end_year);
    case SifPeriodicReach::AfterLast:
        return fmt::format("epoch is {:.3f} yr after the last periodic block [{:.4f}, {:.4f}); "
                           "that block's seasonal term is applied",
                           yrs, p.block_start_year, p.block_end_year);
    case SifPeriodicReach::InGap:
        return fmt::format("epoch is {:.3f} yr after the periodic block [{:.4f}, {:.4f}) and "
                           "before the next one; the earlier block's seasonal term is applied",
                           yrs, p.block_start_year, p.block_end_year);
    default:
        return {};
    }
}

ResolvedStationCoord resolveStationCoordinate(const StationInfo& s,
                                              double decimal_year,
                                              bool with_periodic,
                                              bool with_psd) {
    ResolvedStationCoord out;
    const SifOccupation* occ = selectOccupation(s, decimal_year);
    if (occ == nullptr) return out;  // no usable solution; caller falls back

    // Extrapolation diagnostics (no effect on the coordinate).  selectOccupation
    // returns a covering window whenever one exists, so the selected record not
    // covering the epoch means none does.
    out.segment_start_year = occ->start_year;
    out.segment_end_year   = occ->end_year;
    if (decimal_year < occ->start_year || decimal_year >= occ->end_year) {
        out.extrapolated = true;
        double gap_lo = 0.0, gap_hi = 0.0;
        if (decimal_year >= occ->end_year) {
            out.extrapolation_years = decimal_year - occ->end_year;
            gap_lo = occ->end_year - SIF_GAP_CHANGE_TOLERANCE_YR;
            gap_hi = decimal_year;
        } else {
            out.extrapolation_years = decimal_year - occ->start_year;  // negative
            gap_lo = decimal_year;
            gap_hi = occ->start_year + SIF_GAP_CHANGE_TOLERANCE_YR;
        }
        // Antenna: model + radome (whole TYPE) or serial; a same-equipment
        // re-log does not count.  Receiver: TYPE only; firmware-only and
        // serial-only updates do not count.
        out.antenna_change_in_gap = equipmentChangeInGap(
            s.antennas, gap_lo, gap_hi,
            [](const SifAntennaWindow& prev, const SifAntennaWindow& cur) {
                return trimCopy(prev.type) != trimCopy(cur.type)
                    || trimCopy(prev.serial) != trimCopy(cur.serial);
            });
        out.receiver_change_in_gap = equipmentChangeInGap(
            s.receivers, gap_lo, gap_hi,
            [](const SifReceiverWindow& prev, const SifReceiverWindow& cur) {
                return trimCopy(prev.type) != trimCopy(cur.type);
            });
    }

    // Linear term: reference coordinate + velocity * (t - t_ref), with t_ref the
    // occupation's own reference epoch (the METADATA epoch only when the record
    // has none; see the loader).
    const double dt = decimal_year - occ->reference_epoch_year;
    out.ecef = occ->coordinates + occ->velocity * dt;

    // Periodic (seasonal) term: one block, chosen by the epoch (the covering
    // block, else the nearest by the M-PAGES rule; see selectPeriodicBlock), so
    // the term does not vanish where the linear term goes on.
    // d = A_cos*cos(w*dT) + A_sin*sin(w*dT), w = 2*pi/period,
    // dT = (t - block ref epoch) in years (NGS / M-PAGES convention).
    if (with_periodic) {
        const PeriodicChoice pc = selectPeriodicBlock(s, decimal_year);
        if (pc.block != nullptr) {
            const SifPeriodicBlock& b = *pc.block;
            out.periodic.reach            = pc.reach;
            out.periodic.block_start_year = b.start_year;
            out.periodic.block_end_year   = b.end_year;
            if (pc.reach == SifPeriodicReach::BeforeFirst)
                out.periodic.extrapolation_years = decimal_year - b.start_year;  // negative
            else if (pc.reach == SifPeriodicReach::AfterLast || pc.reach == SifPeriodicReach::InGap)
                out.periodic.extrapolation_years = decimal_year - b.end_year;

            const double dT = decimal_year - b.reference_epoch_year;
            for (const auto& f : b.freqs) {
                if (f.period_yr == 0.0) continue;
                const double w = 2.0 * 3.14159265358979323846 / f.period_yr;
                const double c = std::cos(w * dT), sn = std::sin(w * dT);
                out.ecef.x() += f.x_cos * c + f.x_sin * sn;
                out.ecef.y() += f.y_cos * c + f.y_sin * sn;
                out.ecef.z() += f.z_cos * c + f.z_sin * sn;
            }
        }
    }

    // Post-seismic deformation: sum of log/exp relaxation per ENU component,
    // for events before the epoch.  d = sum A_log*log(1 + dT/T_log)
    //                                 + sum A_exp*(1 - exp(-dT/T_exp)).
    if (with_psd && !s.psd.empty()) {
        const auto evalComp = [](const SifPsdComponent& comp, double dT) {
            double d = 0.0;
            const std::size_t nlog = std::min(comp.alog.size(), comp.tlog.size());
            for (std::size_t i = 0; i < nlog; ++i)
                if (comp.tlog[i] > 0.0) d += comp.alog[i] * std::log(1.0 + dT / comp.tlog[i]);
            const std::size_t nexp = std::min(comp.aexp.size(), comp.texp.size());
            for (std::size_t i = 0; i < nexp; ++i)
                if (comp.texp[i] > 0.0) d += comp.aexp[i] * (1.0 - std::exp(-dT / comp.texp[i]));
            return d;
        };
        Eigen::Vector3d enu = Eigen::Vector3d::Zero();  // (E, N, U)
        for (const auto& pb : s.psd) {
            const double dT = decimal_year - pb.event_year;
            if (dT <= 0.0) continue;  // event has not happened yet at this epoch
            enu.x() += evalComp(pb.east, dT);
            enu.y() += evalComp(pb.north, dT);
            enu.z() += evalComp(pb.up, dT);
        }
        if (enu.squaredNorm() > 0.0) {
            const Coordinates::Llh llh = Coordinates::ecefToLlh(out.ecef);
            out.ecef += Coordinates::enuToEcef(enu, llh);
        }
    }

    // Propagate the position covariance to the epoch over the same dt as the
    // linear term (the covariance is referred to the occupation's own epoch):
    // with A = [I3 | dt*I3],
    // Cov3(t) = Cxx + dt*(Cxv + Cxv^T) + dt^2*Cvv.  (The periodic/PSD sigma
    // contributions are sub-mm and intentionally omitted from the datum prior.)
    if (occ->has_covariance) {
        const Eigen::Matrix3d Cxx = occ->covariance.topLeftCorner<3, 3>();
        const Eigen::Matrix3d Cxv = occ->covariance.topRightCorner<3, 3>();
        const Eigen::Matrix3d Cvv = occ->covariance.bottomRightCorner<3, 3>();
        out.covariance = Cxx + dt * (Cxv + Cxv.transpose()) + (dt * dt) * Cvv;
        out.has_covariance = true;
    }

    out.valid = true;
    return out;
}

std::optional<SifRosterCoordinate> resolveRosterCoordinate(const StationInfoFile& registry,
                                                           const std::string& station_name,
                                                           double decimal_year) {
    // A roster station without position_ecef has no position to confirm a
    // prefix candidate with: find() then accepts an exact ID, a DOMES number or
    // a bare 4-char code, and rejects (and logs) a longer prefix-only name.
    const StationInfo* s = registry.find(station_name);
    if (s == nullptr) return std::nullopt;
    // Full SIF coordinate function at the run date (see resolveStationCoordinate):
    //   x(t) = x_ref + v * (t - t_ref) + periodic(t) + post-seismic(t)
    // with x_ref/v the occupation solution at that occupation's own reference
    // epoch (typically ITRF2020 at 2020.0), so the returned ECEF is the
    // survey-grade coordinate propagated to decimal_year, as a fixed-coordinate
    // network roster requires.
    const ResolvedStationCoord rc = resolveStationCoordinate(*s, decimal_year,
                                                             /*with_periodic=*/true,
                                                             /*with_psd=*/true);
    if (!rc.valid) return std::nullopt;
    SifRosterCoordinate out;
    out.ecef       = rc.ecef;
    out.epoch_year = decimal_year;
    out.extrapolated           = rc.extrapolated;
    out.extrapolation_years    = rc.extrapolation_years;
    out.segment_start_year     = rc.segment_start_year;
    out.segment_end_year       = rc.segment_end_year;
    out.antenna_change_in_gap  = rc.antenna_change_in_gap;
    out.receiver_change_in_gap = rc.receiver_change_in_gap;
    out.periodic               = rc.periodic;
    return out;
}

SifArpReduction reduceSifCoordinateToArp(const StationInfo& s,
                                         const Eigen::Vector3d& grp_ecef,
                                         double decimal_year,
                                         const Eigen::Vector3d* rinex_delta_hen) {
    SifArpReduction out;
    out.arp_ecef = grp_ecef;

    // The covering SIF antenna window, the same one the a-priori antenna QC
    // reports on.  ARP_MINUS_GRP_ENU is stored (E, N, U); markerToArp takes the
    // RINEX (H, E, N) order.
    for (const auto& aw : s.antennas) {
        if (decimal_year < aw.start_year || decimal_year >= aw.end_year) continue;
        out.has_sif_eccentricity = true;
        out.sif_delta_hen = Eigen::Vector3d(aw.arp_minus_grp_enu(2),
                                            aw.arp_minus_grp_enu(0),
                                            aw.arp_minus_grp_enu(1));
        break;
    }

    if (rinex_delta_hen != nullptr) {
        out.delta_hen = *rinex_delta_hen;
        out.source = SifEccentricitySource::RinexHeader;
        if (out.has_sif_eccentricity)
            out.eccentricity_disagreement_m = (out.delta_hen - out.sif_delta_hen).norm();
    } else if (out.has_sif_eccentricity) {
        out.delta_hen = out.sif_delta_hen;
        out.source = SifEccentricitySource::SifAntennaWindow;
    } else {
        return out;
    }

    out.arp_ecef = Coordinates::markerToArp(grp_ecef, out.delta_hen);
    return out;
}

} // namespace GnssProcessor
