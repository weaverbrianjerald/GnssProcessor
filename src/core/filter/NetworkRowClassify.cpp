/**
 * @file NetworkRowClassify.cpp
 * @brief Implementation of the shared network row/column classifier
 *        (see NetworkRowClassify.hpp; INC-0 of
 *        docs/SPEC_prefit_census_observability.md).
 *
 * Both bodies carry the logic of the label pass and the `classifyRow` lambda
 * from `NetworkRhsAttrib.cpp` unchanged.  The captured `fam` / `col_sat` /
 * `c00_*` locals are the @ref SharedColumnMap fields here, and the class is
 * returned as the enum instead of a `const char*`; @ref rowClassName maps it
 * back to the same strings, so every dumped class name is unchanged.  The
 * relocation was gated on a byte-identical attribution dump.
 *
 * Card NOM step N0b: a column's family, its satellite and the two SH mean columns
 * are read from the estimator's column identity (@ref sharedColumnFamily,
 * @c NetworkEstimator::vtecCoeffColumn) instead of being parsed from the label; the
 * label is still built, for the traces.
 */

#include "filter/NetworkRowClassify.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkRunTypes.hpp"

namespace GnssProcessor {

const char* rowClassName(NetRowClass c) {
    switch (c) {
        case NetRowClass::ObsCode:     return "obs_code";
        case NetRowClass::ObsPhase:    return "obs_phase";
        case NetRowClass::ObsOther:    return "obs_other";
        case NetRowClass::IonoShRow:   return "iono_sh_row";
        case NetRowClass::PivotPin:    return "pivot_pin";
        case NetRowClass::LocalPinRow: return "local_pin_row";
        case NetRowClass::CplRow:      return "cpl_row";
    }
    return "obs_other";   // unreachable; keeps every compiler quiet
}

NetColFam sharedColumnFamily(const NetworkEstimator& est, Eigen::Index col, std::string* sat) {
    if (col < 0 || col >= est.numShared()) return NetColFam::Other;
    SharedParamKind kind = SharedParamKind::Clock;
    std::string     col_sat, signal;
    if (est.sharedColumnIdentity(col, kind, col_sat, signal)) {
        if (sat) *sat = col_sat;
        switch (kind) {
            case SharedParamKind::Clock:     return NetColFam::Clk;
            case SharedParamKind::CodeBias:  return NetColFam::Cb;
            case SharedParamKind::PhaseBias: return NetColFam::Pb;
            case SharedParamKind::VtecSh:    return NetColFam::Sh;   // not a satellite identity
        }
        return NetColFam::Other;
    }
    // No satellite identity: a clone of any kind (card SCL's clocks, card BCL's biases), a
    // shared-history column or an SH coefficient.
    if (est.isClockClone(col) || est.isHistoryColumn(col)) return NetColFam::Other;
    return NetColFam::Sh;
}

SharedColumnMap buildSharedColumnMap(const NetworkEstimator& est, Eigen::Index n) {
    SharedColumnMap cm;
    if (n <= 0) return cm;
    cm.n = n;
    cm.label.assign(static_cast<std::size_t>(n), {});
    cm.fam.assign(static_cast<std::size_t>(n), NetColFam::Other);
    cm.col_sat.assign(static_cast<std::size_t>(n), {});
    bool any_sh = false;
    for (Eigen::Index c = 0; c < n; ++c) {
        const std::size_t u = static_cast<std::size_t>(c);
        try { cm.label[u] = est.sharedColumnLabel(c); }
        catch (...) { cm.label[u] = "col" + std::to_string(c); }
        cm.fam[u] = sharedColumnFamily(est, c, &cm.col_sat[u]);
        if (cm.fam[u] == NetColFam::Sh) any_sh = true;
    }
    // The SH mean column of one shell: its (n, m, trig) = (0, 0, cos) coefficient, or -1.
    const auto meanColumn = [&](int layer) -> Eigen::Index {
        try {
            const Eigen::Index c = est.vtecCoeffColumn(0, 0, 'C', layer);
            return (c >= 0 && c < n) ? c : -1;
        } catch (...) { return -1; }   // the shell is not registered
    };
    if (any_sh) {
        cm.c00_l1 = meanColumn(0);   // "SH:C:0:0"
        cm.c00_l2 = meanColumn(1);   // "SH2:C:0:0"
    }
    return cm;
}

NetRowClass classifyRow(const SharedColumnMap& cm, const StationAssembly& R,
                        Eigen::Index row, std::string* row_sat) {
    if (R.cpl_row_start >= 0 && row >= R.cpl_row_start) return NetRowClass::CplRow;
    const double w = (row < R.w.size()) ? R.w(row) : 1.0;
    bool sh = false, pb = false, cb = false, any_shared = false;
    const Eigen::Index ns = std::min<Eigen::Index>(R.A_s.cols(), cm.n);
    for (Eigen::Index c = 0; c < ns; ++c) {
        if (R.A_s(row, c) == 0.0) continue;
        any_shared = true;
        const std::size_t u = static_cast<std::size_t>(c);
        if (row_sat && row_sat->empty() && !cm.col_sat[u].empty()) *row_sat = cm.col_sat[u];
        if (cm.fam[u] == NetColFam::Sh) { sh = true; break; }
        if (cm.fam[u] == NetColFam::Pb) pb = true;
        else if (cm.fam[u] == NetColFam::Cb) cb = true;
    }
    if (sh) return NetRowClass::IonoShRow;
    if (!any_shared) return (w >= 1e9) ? NetRowClass::PivotPin : NetRowClass::LocalPinRow;
    if (pb) return NetRowClass::ObsPhase;
    // Amb-column touch = secondary phase signal (frame-diag convention).
    for (Eigen::Index c = 0; c < R.A_r.cols(); ++c) {
        if (std::abs(R.A_r(row, c)) > 0.5) {
            for (const auto& pa : R.pl_amb)
                if (pa.col == c) return NetRowClass::ObsPhase;
        }
    }
    if (cb) return NetRowClass::ObsCode;
    return NetRowClass::ObsOther;
}

} // namespace GnssProcessor
