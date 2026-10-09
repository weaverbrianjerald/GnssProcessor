/**
 * @file NetworkRowClassify.cpp
 * @brief Implementation of the shared network row classifier and the shared-column
 *        family pass (see NetworkRowClassify.hpp; INC-0 of
 *        docs/SPEC_prefit_census_observability.md).
 *
 * Card NOM step N0b: a column's family, its satellite and the two SH mean columns
 * are read from the estimator's column identity (@ref sharedColumnFamily,
 * @c NetworkEstimator::vtecCoeffColumn) instead of being parsed from the label; the
 * label is still built, for the traces.
 *
 * Card NOM step N2b: a row's class is read from its contribution tag
 * (@ref rowClassOfKind), so @ref classifyRow no longer reads the shared columns, the
 * ambiguity list @c pl_amb or the coupling region's position.  @ref rowClassName maps
 * the class to the same strings as before, so every dumped class name is unchanged.
 */

#include "filter/NetworkRowClassify.hpp"

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

bool isCodeObservationKind(ContributionKind k) {
    return k == ContributionKind::DataCode || k == ContributionKind::DataCodeCompanion;
}

bool isPhaseObservationKind(ContributionKind k) {
    return k == ContributionKind::DataPhase || k == ContributionKind::DataPhaseSecond;
}

NetRowClass rowClassOfKind(ContributionKind k, double w) {
    using CK = ContributionKind;
    if (isCodeObservationKind(k))  return NetRowClass::ObsCode;
    if (isPhaseObservationKind(k)) return NetRowClass::ObsPhase;
    if (contributionInCouplingRegion(k)) return NetRowClass::CplRow;
    switch (k) {
        case CK::ShIonoTie:
            return NetRowClass::IonoShRow;
        // The station constraint rows and the phase-colour prior rows: no observation,
        // no carry statement.  A hard gauge pin by its weight, a soft prior row otherwise.
        case CK::AmbiguityPin:
        case CK::Eq34RpbPin:
        case CK::Eq34RpbRankFloor:
        case CK::Eq34RpbTimeLink:
        case CK::GloIfbAnchor:
        case CK::GloIonoAnchor:
        case CK::GimIono:
        case CK::SdEwl:
        case CK::PhaseColourBirth:
        case CK::PhaseColourBackstop:
            return (w >= kNetPivotPinWeight) ? NetRowClass::PivotPin : NetRowClass::LocalPinRow;
        default:
            // Unset, a diagonal prior kind or a shared kind: no station row should carry
            // one.  Unclassified, and counted by the contributions table's integrity lines.
            return NetRowClass::ObsOther;
    }
}

const RowTag& rowTagAt(const StationAssembly& R, Eigen::Index row) {
    static const RowTag kNoTag{};
    if (row < 0 || static_cast<std::size_t>(row) >= R.row_tags.size()) return kNoTag;
    return R.row_tags[static_cast<std::size_t>(row)];
}

std::string rowObservationCode(const RowTag& t) {
    if (t.signal.sys == System::Unknown) return {};
    if (isCodeObservationKind(t.kind))  return t.signal.obsCode(ObsType::Code);
    if (isPhaseObservationKind(t.kind)) return t.signal.obsCode(ObsType::Phase);
    return {};
}

NetRowClass classifyRow(const StationAssembly& R, Eigen::Index row, std::string* row_sat) {
    const RowTag& t = rowTagAt(R, row);
    if (row_sat && row_sat->empty() && t.sat.valid()) *row_sat = t.sat.str();
    const double w = (row < R.w.size()) ? R.w(row) : 1.0;
    return rowClassOfKind(t.kind, w);
}

bool raimPoolAdmitsKind(ContributionKind k) {
    return isPhaseObservationKind(k) || contributionInCouplingRegion(k)
           || k == ContributionKind::SdEwl;
}

RowTag raimPoolColumnSubject(std::string_view sat, std::string_view signal) {
    return makeRowTag(ContributionKind::DataPhase, sat, signal);
}

bool rowTagNamesColumn(const RowTag& row, const RowTag& column) {
    return column.sat.valid() && column.signal.sys != System::Unknown
           && row.sat == column.sat && row.signal == column.signal;
}

bool raimPoolAdmitsRow(const RowTag& row, bool clears, const RowTag& column,
                       bool data_rows_only) {
    if (!data_rows_only) return clears && raimPoolAdmitsKind(row.kind);
    return isPhaseObservationKind(row.kind) && rowTagNamesColumn(row, column);
}

} // namespace GnssProcessor
