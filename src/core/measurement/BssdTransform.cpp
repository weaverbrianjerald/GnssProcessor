/**
 * @file BssdTransform.cpp
 * @brief Between-Satellite Single-Difference (BSSD) S-transformation.
 *
 * Two methods cover two paths through the pipeline.  @c apply() handles
 * the in-place EpochMeasurement S-transform used by PPP-RTK BSSD;
 * @c differenceBrsd() handles the BRSD-vector → DD-row operation used
 * by baseline DD.  See the header for the full architectural notes.
 */

#include "measurement/BssdTransform.hpp"
#include <Eigen/Dense>
#include <algorithm>
#include <map>
#include <set>

namespace GnssProcessor {

// ===========================================================================
// Construction
// ===========================================================================

BssdTransform::BssdTransform(const GnssConfig& config,
                             const StateManager& state_manager,
                             const PivotManager& pivot_manager,
                             LogManager& log_manager)
    : config_(config),
      state_manager_(state_manager),
      pivot_manager_(pivot_manager),
      log_manager_(log_manager) {
}

// ===========================================================================
// S-transformation (PPP-RTK in-place path)
// ===========================================================================

void BssdTransform::apply(EpochMeasurement& update, const std::string& epoch_prefix) {
const Eigen::Index n = update.innovations.size();
if (n < 2) return;

const bool is_uncombined = config_.isUncombinedEnabled();

// In uncombined mode, group measurements by frequency band (first two
// characters of the signal type, e.g. "L1W" -> "L1") rather than the
// full signal identifier.  Different tracking attributes on the same
// band share geometry, troposphere, and clock � exactly the terms that
// cancel in between-satellite differencing.  This prevents satellites
// that fall back to a different tracking mode (e.g. L1C instead of L1W
// at low elevation) from becoming undifferenced singletons.
auto bandGroupKey = [&](const std::string& constellation,
                        const std::string& signal_type) -> std::string {
    if (is_uncombined && signal_type.size() >= 2)
        return constellation + "|" + signal_type.substr(0, 2);
    return constellation + "|" + signal_type;
};

// ---- Step 1: Classify rows -----------------------------------------
struct RowInfo {
    Eigen::Index row;
    std::string satellite;
    std::string signal_type;
    std::string constellation;
    std::string group_key;   ///< band-normalized group key for SD pairing
    double elevation;
};
std::vector<RowInfo> rows(static_cast<size_t>(n));
std::map<std::string, double> sat_elev;
for (const auto& obs : update.observations)
    sat_elev[obs.satellite] = obs.elevation;

for (Eigen::Index i = 0; i < n; ++i) {
    auto& ri = rows[static_cast<size_t>(i)];
    ri.row = i;
    ri.satellite   = update.satellite_ids[static_cast<size_t>(i)];
    ri.signal_type = update.signal_types[static_cast<size_t>(i)];
    ri.constellation = PppUtils::getSystemString(ri.satellite);
    ri.group_key = bandGroupKey(ri.constellation, ri.signal_type);
    auto it = sat_elev.find(ri.satellite);
    ri.elevation = (it != sat_elev.end()) ? it->second : 0.0;
}

// ---- Step 2: Pivot selection ---------------------------------------
std::map<std::string, std::string> cons_pivot_sat;
    std::map<std::string, double>      cons_pivot_el;
    if (is_uncombined) {
        cons_pivot_sat = pivot_manager_.getPivotSatellites();
        for (const auto& [cons, sat] : cons_pivot_sat) {
            auto it = sat_elev.find(sat);
            cons_pivot_el[cons] = (it != sat_elev.end()) ? it->second : 0.0;
        }
    } else {
        for (const auto& ri : rows) {
            auto it = cons_pivot_el.find(ri.constellation);
            if (it == cons_pivot_el.end() || ri.elevation > it->second) {
                cons_pivot_sat[ri.constellation] = ri.satellite;
                cons_pivot_el[ri.constellation]  = ri.elevation;
            }
        }
    }

    // Build the per-(constellation, band) pivot_map.
    // In uncombined mode, the group key uses the frequency band (e.g. "GPS|L1")
    // so that all tracking attributes on the same band share a single pivot row.
    std::map<std::string, Eigen::Index> pivot_map;
    std::map<std::string, double>       pivot_el;

    if (is_uncombined) {
        for (const auto& ri : rows) {
            if (ri.satellite == cons_pivot_sat[ri.constellation]) {
                pivot_map[ri.group_key] = ri.row;
                pivot_el[ri.group_key]  = ri.elevation;
            }
        }
    } else {
        for (const auto& ri : rows) {
            auto it = pivot_el.find(ri.group_key);
            if (it == pivot_el.end() || ri.elevation > it->second) {
                pivot_map[ri.group_key] = ri.row;
                pivot_el[ri.group_key]  = ri.elevation;
            }
        }
    }

    // ---- Step 3: Partition into SD-eligible rows; reject everything else
    // BSSD purity rule: a row that cannot be paired with a same-group pivot
    // is REJECTED, not passed through.  Letting an undifferenced row
    // survive would silently feed a measurement whose receiver-clock /
    // ISB / code-bias dependence has been zeroed out of H by Step 6
    // (those columns get globally zeroed because *differenced* rows no
    // longer depend on them) — but the row itself was never differenced.
    // The Kalman filter would then absorb the receiver clock and biases
    // into other states.  Drop the row instead so BSSD mode only ever
    // applies properly-differenced observations.
    std::map<std::string, int> grp_cnt;
    for (const auto& ri : rows) {
        if (pivot_map.count(ri.group_key))
            ++grp_cnt[ri.group_key];
    }

    struct SdEntry { Eigen::Index row; std::string gk; };
    std::vector<SdEntry> sd_entries;
    size_t n_rejected_no_pivot = 0;
    size_t n_rejected_singleton = 0;
    size_t n_pivot_consumed = 0;

    for (const auto& ri : rows) {
        if (!pivot_map.count(ri.group_key)) {
            ++n_rejected_no_pivot;
        } else if (grp_cnt[ri.group_key] <= 1) {
            ++n_rejected_singleton;
        } else if (pivot_map[ri.group_key] == ri.row) {
            ++n_pivot_consumed;
        } else {
            sd_entries.push_back({ri.row, ri.group_key});
        }
    }

    const size_t n_sd = sd_entries.size();
    const size_t n_rejected = n_rejected_no_pivot + n_rejected_singleton;
    if (n_sd == 0) {
        log_manager_.getLogger("bssd_transform")->info(
            "{}BSSD: no SD pairs formed ({} rejected: {} no-pivot, "
            "{} singleton; {} pivot consumed) — emitting empty update",
            epoch_prefix, n_rejected,
            n_rejected_no_pivot, n_rejected_singleton, n_pivot_consumed);
        update.innovations.resize(0);
        update.H.resize(0, update.H.cols());
        update.R_full.resize(0, 0);
        update.R_v.resize(0);
        update.satellite_ids.clear();
        update.signal_types.clear();
        return;
    }

    // ---- Step 4-5: Sparse SD transform ---------------------------------
    // The differencing operator D has exactly two non-zeros per SD row:
    // +1 at the non-pivot's input index, -1 at the pivot's input index.
    // The dense D · X family of products is therefore wasteful — each
    // operation collapses to a per-row difference of input rows.  R_undiff
    // is also "diagonal held as full n×n dense" — purely silly.
    //
    // Below we compute new_z, new_H, and new_R directly from the sparse
    // structure.  The new R has a known sparsity pattern: within a group,
    // off-diagonal entries equal the pivot's variance (the shared term);
    // across different groups, entries are zero.
    const Eigen::Index m = static_cast<Eigen::Index>(n_sd);

    struct SdSparse {
        Eigen::Index pos;   ///< Input row that gets the +1 (non-pivot).
        Eigen::Index neg;   ///< Input row that gets the -1 (pivot).
        std::string  gk;    ///< Group key — used for R off-diagonal pattern.
    };
    std::vector<SdSparse> sd_rows;
    sd_rows.reserve(n_sd);
    for (const auto& e : sd_entries) {
        sd_rows.push_back({ e.row, pivot_map[e.gk], e.gk });
    }

    Eigen::VectorXd new_z(m);
    Eigen::MatrixXd new_H(m, update.H.cols());
    for (Eigen::Index k = 0; k < m; ++k) {
        const auto& r = sd_rows[static_cast<size_t>(k)];
        new_z(k)      = update.innovations(r.pos) - update.innovations(r.neg);
        new_H.row(k)  = update.H.row(r.pos)       - update.H.row(r.neg);
    }

    // new_R(i,i) = R_v(pos_i) + R_v(neg_i)
    // new_R(i,j) for i ≠ j:
    //   same group  → R_v(pivot_idx)  (D(i,pivot)·D(j,pivot)·R = (-1)(-1)R)
    //   diff group → 0
    Eigen::MatrixXd new_R = Eigen::MatrixXd::Zero(m, m);
    for (Eigen::Index i = 0; i < m; ++i) {
        const auto& ri = sd_rows[static_cast<size_t>(i)];
        new_R(i, i) = update.R_v(ri.pos) + update.R_v(ri.neg);
        for (Eigen::Index j = i + 1; j < m; ++j) {
            const auto& rj = sd_rows[static_cast<size_t>(j)];
            if (ri.gk == rj.gk) {
                const double r_piv = update.R_v(ri.neg);  // shared pivot
                new_R(i, j) = r_piv;
                new_R(j, i) = r_piv;
            }
        }
    }

    // ---- Step 6: Zero columns that cancel after differencing -----------
    const auto zero_col = [&](size_t idx) {
        if (idx < static_cast<size_t>(new_H.cols()))
            new_H.col(static_cast<Eigen::Index>(idx)).setZero();
    };

    // 6a. Receiver clock, phase clock, ISB
    zero_col(state_manager_.getClockIndex());
    if (state_manager_.hasPhaseClock())
        zero_col(state_manager_.getPhaseClockIndex());
    for (const auto& sys : state_manager_.getSystemsWithTimeOffsets())
        zero_col(state_manager_.getSystemTimeOffsetIndex(sys));

    // 6b. Receiver DCB / IFB columns
    for (const auto& [key, idx] : state_manager_.getCodeBiasStates())
        zero_col(idx);

    // 6c. Reference satellite ionosphere and pivot satellite bias columns
    std::set<Eigen::Index> ref_iono_cols;
    if (is_uncombined) {
        for (const auto& [cons, pivot_sat] : cons_pivot_sat) {
            const int iono_idx = state_manager_.getIonosphereIndex(pivot_sat);
            if (iono_idx >= 0)
                ref_iono_cols.insert(static_cast<Eigen::Index>(iono_idx));

            for (const auto& [sb_key, sb_idx] : state_manager_.getSatBiasStates()) {
                if (sb_key.compare(0, pivot_sat.size() + 1, pivot_sat + "|") == 0)
                    zero_col(sb_idx);
            }
        }
        for (const Eigen::Index col : ref_iono_cols)
            new_H.col(col).setZero();
    }

    // ---- Step 7: Reparameterize phase ambiguities ----------------------
    // When band-normalized grouping is active, the pivot row's actual signal
    // type (e.g. "L1W") may differ from the non-pivot's (e.g. "L1C").
    // The reference ambiguity to zero is always the *pivot's* signal type.
    std::set<Eigen::Index> ref_amb_cols;
    for (size_t k = 0; k < n_sd; ++k) {
        const auto& entry = sd_entries[k];
        const Eigen::Index r_row = pivot_map[entry.gk];
        const std::string& pivot_sig = update.signal_types[static_cast<size_t>(r_row)];
        if (pivot_sig.empty() || pivot_sig[0] != 'L') continue;

        const std::string ref_sat = update.satellite_ids[static_cast<size_t>(r_row)];
        const std::string ref_amb_key = ref_sat + "_" + pivot_sig;
        const int ref_amb_idx = state_manager_.getAmbiguityIndex(ref_amb_key);
        if (ref_amb_idx >= 0)
            ref_amb_cols.insert(static_cast<Eigen::Index>(ref_amb_idx));
    }
    for (const Eigen::Index col : ref_amb_cols)
        new_H.col(col).setZero();

    // ---- Step 8: Build metadata ----------------------------------------
    std::vector<std::string> new_sat_ids;
    std::vector<std::string> new_sig_types;
    new_sat_ids.reserve(static_cast<size_t>(m));
    new_sig_types.reserve(static_cast<size_t>(m));

    for (size_t k = 0; k < n_sd; ++k) {
        const Eigen::Index i = sd_entries[k].row;
        const Eigen::Index r = pivot_map[sd_entries[k].gk];
        new_sat_ids.push_back(
            update.satellite_ids[static_cast<size_t>(i)] + "-" +
            update.satellite_ids[static_cast<size_t>(r)]);
        new_sig_types.push_back(update.signal_types[static_cast<size_t>(i)]);
    }

    // ---- Step 9: Pivot summary -----------------------------------------
    std::string pivot_summary;
    if (is_uncombined) {
        for (const auto& [cons, sat] : cons_pivot_sat)
            pivot_summary += cons + "=" + sat + " ";
    } else {
        for (const auto& [gk, pr] : pivot_map) {
            if (grp_cnt[gk] > 1)
                pivot_summary += gk + "=" +
                    update.satellite_ids[static_cast<size_t>(pr)] + " ";
        }
    }

    // ---- Step 10: Replace update contents ------------------------------
    update.innovations   = new_z;
    update.H             = new_H;
    update.R_full        = new_R;
    update.R_v           = new_R.diagonal();
    update.satellite_ids = std::move(new_sat_ids);
    update.signal_types  = std::move(new_sig_types);

    log_manager_.getLogger("bssd_transform")->info(
        "{}BSSD: {} undiff -> {} SD ({} rejected: {} no-pivot, {} singleton; "
        "{} pivot consumed). Zeroed {} ref-amb, {} ref-iono, {} code-bias cols. "
        "Pivots: {}",
        epoch_prefix, n, n_sd, n_rejected,
        n_rejected_no_pivot, n_rejected_singleton, n_pivot_consumed,
        ref_amb_cols.size(), ref_iono_cols.size(),
        state_manager_.getCodeBiasStates().size(),
        pivot_summary);
}

// ===========================================================================
// BSSD on BRSD (baseline DD path)
//
// The BRSD layer has already cancelled receiver-shared bias terms in
// residual space.  This static method just performs the BSSD step:
// for each non-pivot satellite, subtract the pivot's BRSD residual /
// variance from the non-pivot's, per (band, obs_type) group.
//
// No state required — pivot selection is the caller's responsibility
// (MeasurementModel owns the sticky pivot logic for baseline mode and
// passes the result in via @p pivots).
// ===========================================================================

std::vector<BssdDdRow> BssdTransform::differenceBrsd(
    const std::map<std::string, std::map<std::string, std::vector<BrsdObservation>>>& brsd_by_cons_sat,
    const std::map<std::string, std::string>& pivots,
    const std::set<std::string>& exclude_keys)
{
    std::vector<BssdDdRow> out;

    for (const auto& [cons, pivot_sat] : pivots) {
        auto cons_it = brsd_by_cons_sat.find(cons);
        if (cons_it == brsd_by_cons_sat.end()) continue;
        const auto& sats = cons_it->second;

        auto piv_it = sats.find(pivot_sat);
        if (piv_it == sats.end()) continue;
        const auto& piv_brsd = piv_it->second;

        // Build a (band, obs_type) → pivot BRSD lookup for O(log n)
        // pairing in the inner loop.
        std::map<std::pair<FrequencyBand, char>, const BrsdObservation*> piv_by_band;
        for (const auto& bp : piv_brsd) {
            piv_by_band[{ bp.band, bp.obs_type }] = &bp;
        }

        for (const auto& [sat, info_brsd] : sats) {
            if (sat == pivot_sat) continue;
            for (const auto& bi : info_brsd) {
                if (bi.obs_type != 'C' && bi.obs_type != 'L') continue;
                auto pit = piv_by_band.find({ bi.band, bi.obs_type });
                if (pit == piv_by_band.end()) continue;
                const BrsdObservation& bp = *pit->second;

                // Rebuild-loop exclusion: signal_token mirrors the
                // format MeasurementModel writes into em.signal_types
                // (rov_code when codes agree, "<rov>/<ref>" when not).
                if (!exclude_keys.empty()) {
                    const std::string sig_token = bi.brsd_aliased
                        ? (bi.rov_code + "/" + bi.ref_code)
                        : bi.rov_code;
                    if (exclude_keys.count(sat + "_" + sig_token)) continue;
                }

                BssdDdRow r;
                r.obs_type    = bi.obs_type;
                r.sat_i       = sat;
                r.sat_p       = pivot_sat;
                r.cons        = cons;
                r.rov_code    = bi.rov_code;
                r.ref_code    = bi.ref_code;
                r.band        = bi.band;
                // Strict integer-capability: all four observations
                // (rov_i, rov_p, ref_i, ref_p) must use the same RINEX
                // code.  Expressed via BRSD aliased flags:
                //   - !bi.brsd_aliased: rov and ref agree on sat_i.
                //   - !bp.brsd_aliased: rov and ref agree on pivot.
                //   - bi.rov_code == bp.rov_code: rover uses the same
                //     code on sat_i and pivot (ref consistency follows).
                r.integer_capable = !bi.brsd_aliased
                                 && !bp.brsd_aliased
                                 && (bi.rov_code == bp.rov_code);
                // BSSD step: DD = BRSD_i - BRSD_pivot.
                r.innov_pre   = bi.residual - bp.residual;
                r.var_brsd_i  = bi.variance;
                r.var_brsd_p  = bp.variance;
                r.los_rov_i   = bi.los_rov;
                r.los_rov_p   = bp.los_rov;
                out.push_back(std::move(r));
            }
        }
    }

    return out;
}

} // namespace GnssProcessor
