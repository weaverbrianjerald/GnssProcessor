/**
 * @file MeasurementModelDd.cpp
 * @brief MeasurementModel's baseline double-difference build: buildDdMeasurement
 *        and its stages.
 *
 * buildDdMeasurement owns the process-wide first-call diagnostic flag and the
 * single exit; ddRunStages runs the stages in order and stops at the first
 * that finds nothing to build.  Three stages write the filter state, and the
 * driver names them: ddSelectPivots (the pivot handover S-transform),
 * ddPruneStaleAmbiguities (removes DD ambiguity states) and ddEnsureAmbiguities
 * (adds them).  The other stages compute rows from the two receivers'
 * observations.  The stages share one working value, DdBuild, and each opens
 * with an alias preamble so its body reads the names the single function used.
 */

#include "measurement/MeasurementModel.hpp"
#include "measurement/BrsdTransform.hpp"
#include "common/SignalBand.hpp"
#include "filter/StateKeySchema.hpp"
#include <cmath>
#include <limits>
#include <tuple>

namespace GnssProcessor {

    // =================================================================
    // Baseline (DD) measurement update (Slice 5e+)
    //
    // DD rows are built from two per-receiver PppModel outputs rather
    // than from a separate geometry layer.  The per-receiver residuals
    // (observed - modeled) already include Sagnac, emission time,
    // satellite clock, Shapiro, antenna PCO/PCV, solid tide, and
    // stochastic-model variances.  DD cancels the parts common to both
    // receivers (sat clock on short baselines, most propagation terms)
    // and leaves the position-dependent geometry + ambiguity signal the
    // filter needs.
    // =================================================================

    namespace {

        /// Build the DD ambiguity key: "DD|cons|sat_i|code|pivot".
        std::string makeDdAmbKey(const std::string& cons,
                                 const std::string& sat_i,
                                 const std::string& code,
                                 const std::string& pivot) {
            return "DD|" + cons + "|" + sat_i + "|" + code + "|" + pivot;
        }

        /// Map first char of a satellite id to constellation name.
        std::string ddConstellationOf(const std::string& sat) {
            if (sat.empty()) return {};
            switch (sat[0]) {
                case 'G': return "GPS";
                case 'E': return "Galileo";
                case 'R': return "GLONASS";
                case 'C': return "BeiDou";
                case 'J': return "QZSS";
                case 'S': return "SBAS";
                case 'I': return "IRNSS";
                default:  return std::string(1, sat[0]);
            }
        }

        /// Constellation string -> System enum.  Used by IF row collapse
        /// to look up frequencies via PppUtils::getFrequency().  Returns
        /// System::Unknown for unrecognised names so the caller can skip.
        System ddSystemFromConsName(const std::string& cons) {
            if (cons == "GPS")     return System::GPS;
            if (cons == "Galileo") return System::Galileo;
            if (cons == "GLONASS") return System::GLONASS;
            if (cons == "BeiDou")  return System::BeiDou;
            if (cons == "QZSS")    return System::QZSS;
            if (cons == "SBAS")    return System::SBAS;
            return System::Unknown;
        }

    } // anonymous namespace

    // ===================================================================
    // The DD build's working types
    // ===================================================================

    /**
     * @brief One satellite tracked by both receivers: its between-receiver
     *        signals (one BRSD per shared band and observation type) and the
     *        inputs of the pivot ranking.
     *
     * Grouping: for each (sat, band, obs_type) tuple shared between the two
     * receivers, the BRSD transform (BrsdTransform::apply) computes the
     * between-receiver residual, variance and rover-side LOS once; BSSD pivot
     * differencing then forms DD rows from the BRSD entries.  BrsdObservation
     * carries the (band, obs_type, rov_code, ref_code, brsd_aliased) metadata
     * with the computed residual, variance and LOS, so the canonical-code
     * selection and the BRSD math run in a single pass.
     */
    struct MeasurementModel::DdSharedSat {
        const PppObservation* rov{ nullptr };
        const PppObservation* ref{ nullptr };
        std::vector<BrsdObservation> brsd_signals;  ///< One BRSD per shared (band, obs_type).
        double avg_snr{ 0.0 };
        size_t num_sigs{ 0 };
    };

    /**
     * @brief One double-differenced row: the non-pivot and pivot satellites, the
     *        rover and reference codes, and its residual, variance pair and
     *        geometry.
     *
     * Each row carries both rover and reference RINEX codes.  When aliasing is
     * off they are equal.  When it is on and the receivers report different
     * tracking attributes on the same band, the row is built cross-attribute:
     * residuals and variances are looked up under each receiver's own code.
     *
     * @c integer_capable is true iff all four observations in the DD (rov_i,
     * rov_p, ref_i, ref_p) use the same RINEX code.  Only those rows have
     * receiver-bias and satellite-bias terms that cancel pairwise; the float
     * ambiguity is then an integer multiple of the carrier wavelength and is
     * eligible for AR.  Any other combination (rov/ref mismatch on this row, a
     * rover code that differs between sat_i and pivot, or a reference code that
     * does) carries a non-integer DCB / IFB residual.
     *
     * The DD row carries the BRSD-combined variance for the non-pivot
     * (var_brsd_i = var_rov_i + var_ref_i) and pivot (var_brsd_p = var_rov_p +
     * var_ref_p).  Diagonal R = var_brsd_i + var_brsd_p; off-diagonal R between
     * rows sharing a pivot = var_brsd_p alone.  Splitting along the BRSD seam
     * (rather than four var_*_* components) makes the variance composition
     * trace cleanly back to the BrsdObservation layer.
     */
    struct MeasurementModel::DdRow {
        char obs_type{ 'C' };
        std::string sat_i, sat_p, cons;
        std::string rov_code, ref_code;
        FrequencyBand band{ FrequencyBand::Unknown };
        bool integer_capable{ true };
        Eigen::RowVector3d h_pos;
        double innov_pre{ 0.0 };
        double var_brsd_i{ 0.0 };  ///< BRSD variance on the non-pivot side (var_rov + var_ref).
        double var_brsd_p{ 0.0 };  ///< BRSD variance on the pivot side (var_rov + var_ref).
        /// Rover-side wet mapping function values for the non-pivot and
        /// pivot satellites.  They populate the H column for the
        /// differential ZWD state when @c estimate_differential_tropo
        /// is on.  The values are nearly band-independent, so the IF
        /// collapse propagates the per-band values from r1.
        double mf_w_rov_i{ 0.0 };
        double mf_w_rov_p{ 0.0 };
        std::string group_key;
        int amb_idx{ -1 };
        double innov{ 0.0 };
    };

    /**
     * @brief The DD build's working value: one call's logger and diagnostic
     *        flag, the shared satellites, the pivots, the rows and the rows kept
     *        for the update.
     *
     * Built by buildDdMeasurement for one call and passed to each stage in
     * order; nothing in it outlives the call.  The observation pointers point
     * into the caller's two vectors.
     */
    struct MeasurementModel::DdBuild {
        /// The "measurement_model" logger.
        std::shared_ptr<spdlog::logger> klog;
        /// True on the first DD call of the process (a function-local static in
        /// buildDdMeasurement, shared by every instance): the stages print the
        /// DD diagnostic.
        bool diag{ false };
        /// The rover's observation per satellite.
        std::map<std::string, const PppObservation*> rov_by_sat;
        /// The reference's observation per satellite.
        std::map<std::string, const PppObservation*> ref_by_sat;
        /// Constellation -> satellite -> its shared signals (at least two).
        std::map<std::string, std::map<std::string, DdSharedSat>> by_cons;
        /// Constellation -> pivot satellite, from DdPivotManager.
        std::map<std::string, std::string> pivots;
        /// The DD rows, per band, or one per satellite pair after the
        /// iono-free collapse.
        std::vector<DdRow> rows;
        /// Indices into rows of the rows that enter the update.
        std::vector<size_t> kept;
    };

    EpochMeasurement MeasurementModel::computeMeasurementUpdate(
        const std::vector<PppObservation>& rov_modeled,
        const std::vector<PppObservation>& ref_modeled,
        const Eigen::VectorXd& current_state,
        const std::set<std::string>& excluded_measurements) {
        // Baseline DD is dispatched through the differencing strategy so the
        // build path is uniform with PPP / PPP-RTK; DdMeasurementBuilder calls
        // back into buildDdMeasurement (below) to assemble the rows.
        return differencing_->buildDoubleDifference(
            *this, rov_modeled, ref_modeled, current_state, excluded_measurements);
    }

    EpochMeasurement MeasurementModel::buildDdMeasurement(
        const std::vector<PppObservation>& rov_modeled,
        const std::vector<PppObservation>& ref_modeled,
        const Eigen::VectorXd& current_state,
        const std::set<std::string>& excluded_measurements) {

        EpochMeasurement em;
        // Seed the output exclusion set from the caller's carry set so
        // the KF outlier handlers (which use insert(), not assign) build
        // a cumulative union across rebuild passes.  Without this, the
        // caller's `carry_excluded = em.excluded_measurements` after the
        // KF returns would replace the previous pass's exclusions with
        // only this pass's additions, and a satellite with two bad
        // signals would oscillate (pass N excludes C1C; pass N+1 starts
        // fresh, excludes C2W, forgets C1C; pass N+2 sees C1C again).
        // The PPP overload of computeMeasurementUpdate does the same.
        em.excluded_measurements = excluded_measurements;

        DdBuild dd;
        dd.klog = log_manager_.getLogger("measurement_model");
        // First-epoch diagnostic: shows whether shared sats / signals exist.
        // A function-local static flag turns it off after the first call, so
        // the log is not flooded.  The flag is process-wide (every instance
        // shares it: under Baseline-Multi only the first rover's first call
        // prints the diagnostic), and it is reset on every normal exit below,
        // not when an exception leaves the build.
        static bool first_dd_call = true;
        dd.diag = first_dd_call;
        ddRunStages(rov_modeled, ref_modeled, current_state, excluded_measurements, dd, em);
        first_dd_call = false;
        return em;
    }

    // ===================================================================
    // The stages, in ddRunStages' order
    // ===================================================================

    void MeasurementModel::ddRunStages(const std::vector<PppObservation>& rov_modeled,
                                       const std::vector<PppObservation>& ref_modeled,
                                       const Eigen::VectorXd& current_state,
                                       const std::set<std::string>& excluded_measurements,
                                       DdBuild& dd, EpochMeasurement& em) {
        ddLogInputDiagnostic(rov_modeled, ref_modeled, dd);
        if (rov_modeled.empty() || ref_modeled.empty()) return;
        em.time = rov_modeled.front().time;

        if (!ddSharedSignals(rov_modeled, ref_modeled, dd)) return;
        // State write 1 of 3: the sticky pivot selection, which on a pivot change
        // applies the S-transform handover to the DD ambiguity states.
        if (!ddSelectPivots(dd)) return;
        ddRowsFromBrsd(excluded_measurements, dd);
        ddCollapseIonoFree(dd);
        if (!ddHasRows(dd)) return;
        // State write 2 of 3: removes the DD ambiguity states no row of this
        // epoch references.
        ddPruneStaleAmbiguities(excluded_measurements, dd);
        ddNonIntegerKeys(dd);
        // State write 3 of 3: adds a DD ambiguity state for each new phase row.
        ddEnsureAmbiguities(dd);
        if (!ddInnovations(current_state, dd)) return;
        ddAssemble(dd, em);
    }

    void MeasurementModel::ddLogInputDiagnostic(const std::vector<PppObservation>& rov_modeled,
                                                const std::vector<PppObservation>& ref_modeled,
                                                const DdBuild& dd) const {
        const auto& klog = dd.klog;
        const bool diag = dd.diag;

        // Printed on the process's first DD call only (see buildDdMeasurement).
        if (!diag) return;
        klog->info("DD first-epoch diagnostic: rov_modeled={} sats, ref_modeled={} sats",
                   rov_modeled.size(), ref_modeled.size());
        if (!rov_modeled.empty()) {
            const auto& r0 = rov_modeled.front();
            std::string codes;
            for (const auto& [code, v] : r0.observed) { codes += code; codes += ' '; }
            klog->info("DD diag: rov[0]={} observed_codes=[{}]", r0.satellite, codes);
        }
        if (!ref_modeled.empty()) {
            const auto& e0 = ref_modeled.front();
            std::string codes;
            for (const auto& [code, v] : e0.observed) { codes += code; codes += ' '; }
            klog->info("DD diag: ref[0]={} observed_codes=[{}]", e0.satellite, codes);
        }
    }

    bool MeasurementModel::ddSharedSignals(const std::vector<PppObservation>& rov_modeled,
                                           const std::vector<PppObservation>& ref_modeled,
                                           DdBuild& dd) const {
        const auto& klog = dd.klog;
        const bool diag = dd.diag;
        auto& rov_by_sat = dd.rov_by_sat;
        auto& ref_by_sat = dd.ref_by_sat;
        auto& by_cons = dd.by_cons;

        // Active-system + excluded-sat filters.
        const auto& active_vec = config_.getActiveSystems();
        const std::set<std::string> active_set(active_vec.begin(), active_vec.end());

        // Index by satellite.
        for (const auto& o : rov_modeled) rov_by_sat[o.satellite] = &o;
        for (const auto& o : ref_modeled) ref_by_sat[o.satellite] = &o;

        // Aliasing policy: when true, codes that share the same (System, Band)
        // but differ in tracking attribute (e.g. rov C2W vs ref C2C) are
        // paired across receivers.  When false, only exact-string code
        // matches are kept (the strict behavior).  The off-band cancellation
        // properties of DD do not depend on identical attributes; a small
        // sat-DCB residual remains on code DDs (absorbed by the float
        // ambiguity on phase DDs in non-code-only mode).  See the Phase 2
        // plan for DCB harmonization.
        const bool signal_aliasing = config_.getBaselineConfig().signal_aliasing;

        // Per-band canonical code chosen at one receiver: pick the
        // tracking attribute with the lowest @c signalAttributePriority().
        // Returns map keyed by (band, obs_type) → (rinex_code, priority).
        auto pickCanonical = [](const PppObservation& obs, const std::string& cons) {
            std::map<std::pair<FrequencyBand, char>,
                     std::pair<std::string, int>> best;
            for (const auto& [code, val] : obs.observed) {
                if (code.size() < 3) continue;
                const auto cls = classifySignal(cons, code);
                if (cls.band == FrequencyBand::Unknown) continue;
                if (cls.type != ObsType::Code && cls.type != ObsType::Phase) continue;
                const auto key = std::make_pair(cls.band, static_cast<char>(cls.type));
                const int prio = signalAttributePriority(cons, cls.band, cls.attribute);
                auto it = best.find(key);
                if (it == best.end() || prio < it->second.second) {
                    best[key] = { code, prio };
                }
            }
            return best;
        };

        // Group shared sats by constellation.  For each (sat, band, obs_type)
        // tuple shared between the two receivers, run the BRSD transform
        // (BrsdTransform::apply) to compute the between-receiver
        // residual + variance + rover-side LOS once.  Downstream BSSD
        // pivot-differencing then forms DD rows from BRSD entries.
        //
        // BrsdObservation carries the (band, obs_type, rov_code, ref_code,
        // brsd_aliased) metadata together with the computed residual /
        // variance / LOS, so the canonical-code selection and the BRSD
        // math run in a single pass.

        for (const auto& [sat, rov_ptr] : rov_by_sat) {
            const std::string cons = ddConstellationOf(sat);
            if (!active_set.empty() && active_set.find(cons) == active_set.end()) continue;
            if (config_.isSatelliteExcluded(sat)) continue;

            auto ref_it = ref_by_sat.find(sat);
            if (ref_it == ref_by_sat.end()) continue;
            const PppObservation* ref_ptr = ref_it->second;

            DdSharedSat info;
            info.rov = rov_ptr;
            info.ref = ref_ptr;

            const auto rov_canon = pickCanonical(*rov_ptr, cons);
            const auto ref_canon = pickCanonical(*ref_ptr, cons);
            for (const auto& [k, rov_pair] : rov_canon) {
                auto rit = ref_canon.find(k);
                if (rit == ref_canon.end()) continue;
                const std::string& rov_code = rov_pair.first;
                const std::string& ref_code = rit->second.first;
                const bool aliased = (rov_code != ref_code);
                if (aliased && !signal_aliasing) continue;  // strict mode drops mismatches

                auto brsd = BrsdTransform::apply(*rov_ptr, *ref_ptr, cons,
                                                 rov_code, ref_code,
                                                 k.first, k.second);
                if (!brsd) continue;  // missing observed/modeled or NaN
                info.brsd_signals.push_back(std::move(*brsd));
            }

            // SNR averaging: any 'S*' RINEX code on the rover side.
            double snr_sum = 0.0; size_t snr_cnt = 0;
            for (const auto& [code, val] : rov_ptr->observed) {
                if (!code.empty() && code[0] == 'S') { snr_sum += val; ++snr_cnt; }
            }
            info.num_sigs = info.brsd_signals.size();
            info.avg_snr  = (snr_cnt > 0) ? (snr_sum / snr_cnt) : 0.0;
            if (info.num_sigs < 2) continue;
            by_cons[cons][sat] = std::move(info);
        }
        if (diag) {
            size_t total_shared = 0;
            size_t total_aliased = 0;
            for (const auto& [cons, sats] : by_cons) {
                klog->info("DD diag: constellation '{}' has {} shared sats with ≥2 signals", cons, sats.size());
                total_shared += sats.size();
                for (const auto& [sat, info] : sats) {
                    std::string codes;
                    for (const auto& b : info.brsd_signals) {
                        codes += b.rov_code;
                        if (b.brsd_aliased) {
                            codes += "->"; codes += b.ref_code; ++total_aliased;
                        }
                        codes += '(';
                        codes += bandLabel(b.band);
                        codes += ") ";
                    }
                    klog->info("DD diag:   {}: {} BRSD signals=[{}]", sat,
                               info.brsd_signals.size(), codes);
                }
            }
            klog->info("DD diag: {} total viable shared sats; signal_aliasing={}, aliased_signals={}",
                       total_shared, signal_aliasing, total_aliased);
        }
        if (by_cons.empty()) {
            if (diag) klog->info("DD diag: EARLY RETURN — by_cons empty");
            return false;
        }
        return true;
    }

    bool MeasurementModel::ddSelectPivots(DdBuild& dd) {
        const auto& klog = dd.klog;
        const bool diag = dd.diag;
        const auto& by_cons = dd.by_cons;
        auto& pivots = dd.pivots;

        // --- Sticky pivot selection per constellation ---
        // Build the candidate map (cons → sat → ranking-info) from the
        // BRSD-populated by_cons structure and hand it to DdPivotManager.
        // The manager's selectPivots applies the sticky-or-rerank logic
        // and, on a pivot change, the S-transform handover (gated by
        // BaselineConfig::enable_pivot_handover), which preserves the DD
        // ambiguity integer cycles across handovers.
        DdPivotManager::CandidateMap pivot_candidates;
        for (const auto& [cons, sats] : by_cons) {
            for (const auto& [sat, info] : sats) {
                pivot_candidates[cons][sat] = { info.avg_snr, info.num_sigs };
            }
        }
        pivots =
            dd_pivot_manager_.selectPivots(pivot_candidates, state_manager_);

        if (diag) {
            for (const auto& [c, p] : pivots) {
                klog->info("DD diag: pivot for '{}' = '{}'", c, p);
            }
        }
        if (pivots.empty()) {
            if (diag) klog->info("DD diag: EARLY RETURN — pivots empty");
            return false;
        }
        return true;
    }

    void MeasurementModel::ddRowsFromBrsd(const std::set<std::string>& excluded_measurements,
                                          DdBuild& dd) const {
        const auto& rov_by_sat = dd.rov_by_sat;
        const auto& by_cons = dd.by_cons;
        const auto& pivots = dd.pivots;
        auto& rows = dd.rows;

        // ---- BSSD step: BRSD entries → DD rows ----
        // The BRSD inputs and the pivot map go to BssdTransform::differenceBrsd,
        // which performs the BSSD differencing per (cons, band, obs_type)
        // group and returns BssdDdRow entries (residual, variance pair,
        // rover-side LOS pair, integer-capability flag, metadata).
        // MeasurementModel then adds its own bookkeeping (group_key for the
        // R off-diagonal correlation, and the H_pos partial derived from the
        // LOS pair) to produce a DdRow.
        //
        // The differencing math lives entirely in BssdTransform; the same
        // class also serves PPP-RTK SD via apply().  See the class header
        // for the architectural notes on the two methods.
        // Build a (cons, sat) → vector<BrsdObservation> view over
        // by_cons.  The static differenceBrsd signature wants this
        // shape so the structural dependency on MeasurementModel's
        // local SharedInfo struct stays decoupled.
        std::map<std::string,
                 std::map<std::string, std::vector<BrsdObservation>>> brsd_by_cons_sat;
        for (const auto& [cons, sats] : by_cons) {
            for (const auto& [sat, info] : sats) {
                brsd_by_cons_sat[cons][sat] = info.brsd_signals;
            }
        }

        const auto bssd_rows = BssdTransform::differenceBrsd(
            brsd_by_cons_sat, pivots, excluded_measurements);

        rows.reserve(bssd_rows.size());
        for (const auto& br : bssd_rows) {
            DdRow r;
            r.obs_type        = br.obs_type;
            r.sat_i           = br.sat_i;
            r.sat_p           = br.sat_p;
            r.cons            = br.cons;
            r.rov_code        = br.rov_code;
            r.ref_code        = br.ref_code;
            r.band            = br.band;
            r.integer_capable = br.integer_capable;
            r.innov_pre       = br.innov_pre;
            r.var_brsd_i      = br.var_brsd_i;
            r.var_brsd_p      = br.var_brsd_p;
            // Geometry partial w.r.t. the rover position state.
            // Reference LOS doesn't appear because the reference is
            // not a state in baseline mode (its contribution already
            // cancelled in the BRSD layer feeding differenceBrsd).
            r.h_pos = -(br.los_rov_i - br.los_rov_p).transpose();
            // Wet mapping function values for the differential-ZWD H
            // column.  PppModel populates @c mf_w on each PppObservation
            // from the GMF/VMF wet mapping function at the rover's
            // elevation; it is nearly band-independent, so any band's
            // per-row value serves.  Defaults to 0.0 if the satellite is
            // not in rov_by_sat, a defensive fallback that is not expected
            // to trigger (both stations agreed on the satellite at the
            // BRSD layer).
            {
                auto it_i = rov_by_sat.find(br.sat_i);
                auto it_p = rov_by_sat.find(br.sat_p);
                r.mf_w_rov_i = (it_i != rov_by_sat.end()) ? it_i->second->mf_w : 0.0;
                r.mf_w_rov_p = (it_p != rov_by_sat.end()) ? it_p->second->mf_w : 0.0;
            }
            // Group key for R off-diagonal correlation: same pivot
            // variance contribution is shared by all non-pivot rows
            // on the same (cons, band, obs_type).
            r.group_key = br.cons + "|" + bandLabel(br.band)
                        + "|" + std::string(1, br.obs_type);
            rows.push_back(std::move(r));
        }
    }

    void MeasurementModel::ddCollapseIonoFree(DdBuild& dd) const {
        const auto& klog = dd.klog;
        const bool diag = dd.diag;
        auto& rows = dd.rows;

        // --- Iono-free row collapse (long-baseline / educational mode) ---
        // When @c BaselineConfig::use_iono_free is on, replace the per-band
        // DD rows for each (sat_i, pivot, obs_type) group with a single
        // row formed from the dual-frequency ionosphere-free combination
        //
        //     y_IF = α·y_1 + β·y_2,   α = f1²/(f1²-f2²),  β = -f2²/(f1²-f2²)
        //
        // where (f1, y_1) is the higher-frequency band and (f2, y_2) is
        // the lower.  This eliminates the first-order ionospheric delay,
        // which is needed on long baselines (>10-20 km), where the DD
        // does not cancel the ionosphere between receivers.  Cost: ~3x
        // amplified code noise on GPS L1+L2 (the variance scales by
        // α²+β² ≈ 8.87), and the ambiguity is not integer-cycle (it is a
        // non-trivial linear combination of N1 and N2 in cycles), so AR
        // is excluded for IF phase rows via @c integer_capable=false.
        // Satellite pairs with fewer than two shared bands are dropped,
        // since IF needs two.  The H_pos block is unchanged: the geometry
        // partial is common to both bands and α + β = 1.
        if (config_.getBaselineConfig().use_iono_free && !rows.empty()) {
            // Group row indices by (cons, sat_i, sat_p, obs_type).
            std::map<std::string, std::vector<size_t>> if_groups;
            for (size_t i = 0; i < rows.size(); ++i) {
                const auto& r = rows[i];
                const std::string gk = r.cons + "|" + r.sat_i + "|" + r.sat_p
                                     + "|" + std::string(1, r.obs_type);
                if_groups[gk].push_back(i);
            }

            std::vector<DdRow> if_rows;
            if_rows.reserve(if_groups.size());
            size_t n_dropped_single_band = 0;

            // Cap on IF noise amplification factor K = sqrt(α²+β²).  Same
            // physics and same threshold as PppModel::selectIonoFreePairs;
            // picking by K rather than by raw frequency rank avoids the
            // BeiDou trap where the two highest frequencies (B1C, B1I) sit
            // ~14 MHz apart and yield K ≈ 77.  Standard wide pairs (GPS
            // L1/L2 K≈3, BeiDou B1I/B3 K≈3.5) pass comfortably; narrow
            // pairs (B1C/B1I, B2x family) are rejected.
            constexpr double kMaxIfNoiseFactor = 10.0;
            size_t n_dropped_high_K = 0;

            for (const auto& [gk, idxs] : if_groups) {
                if (idxs.size() < 2) {
                    // Need two bands to cancel ionosphere; skip.
                    ++n_dropped_single_band;
                    continue;
                }
                // Pick the band pair within this sat-pair group with the
                // smallest noise amplification factor K, subject to K
                // staying under the cap.  For dual-frequency receivers
                // there's only one pair so this reduces to the prior
                // behavior; for triple-/all-frequency it picks the best
                // available combination.
                const System sys = ddSystemFromConsName(rows[idxs.front()].cons);
                double best_K = std::numeric_limits<double>::infinity();
                size_t best_hi = 0, best_lo = 0;
                bool found = false;
                for (size_t i = 0; i < idxs.size(); ++i) {
                    for (size_t j = i + 1; j < idxs.size(); ++j) {
                        const double fa = ppp_utils_.getFrequency(
                            rows[idxs[i]].rov_code, sys, rows[idxs[i]].sat_i);
                        const double fb = ppp_utils_.getFrequency(
                            rows[idxs[j]].rov_code, sys, rows[idxs[j]].sat_i);
                        if (!(std::isfinite(fa) && std::isfinite(fb))
                         || fa <= 0.0 || fb <= 0.0 || fa == fb) continue;
                        const double fas = fa * fa, fbs = fb * fb;
                        const double denom = fas - fbs;
                        if (std::abs(denom) < 1e-3 * fas) continue;
                        const double K = std::sqrt(fas * fas + fbs * fbs) / std::abs(denom);
                        if (K < best_K) {
                            best_K = K;
                            // hi-frequency index first
                            if (fa >= fb) { best_hi = idxs[i]; best_lo = idxs[j]; }
                            else          { best_hi = idxs[j]; best_lo = idxs[i]; }
                            found = true;
                        }
                    }
                }
                if (!found || best_K > kMaxIfNoiseFactor) {
                    ++n_dropped_high_K;
                    continue;
                }

                const DdRow& r1 = rows[best_hi];  // higher f
                const DdRow& r2 = rows[best_lo];  // lower f

                const double f1 = ppp_utils_.getFrequency(r1.rov_code, sys, r1.sat_i);
                const double f2 = ppp_utils_.getFrequency(r2.rov_code, sys, r2.sat_i);
                const double f1s   = f1 * f1;
                const double f2s   = f2 * f2;
                const double denom = f1s - f2s;
                const double alpha =  f1s / denom;
                const double beta  = -f2s / denom;
                const double a2 = alpha * alpha;
                const double b2 = beta  * beta;

                DdRow ifr;
                ifr.obs_type = r1.obs_type;
                ifr.sat_i    = r1.sat_i;
                ifr.sat_p    = r1.sat_p;
                ifr.cons     = r1.cons;
                ifr.band     = r1.band;            // documentation only
                // Concatenated codes serve as both the ambiguity-key code
                // segment and the trace signal_types token.
                ifr.rov_code = r1.rov_code + "+" + r2.rov_code;
                ifr.ref_code = r1.ref_code + "+" + r2.ref_code;
                ifr.h_pos    = r1.h_pos;           // identical for both bands
                // Wet mapping function is essentially frequency-independent;
                // the GMF/VMF wet partial depends on elevation only, so r1
                // and r2 agree to within a few PPM.  Propagate r1's values.
                ifr.mf_w_rov_i = r1.mf_w_rov_i;
                ifr.mf_w_rov_p = r1.mf_w_rov_p;
                ifr.innov_pre = alpha * r1.innov_pre + beta * r2.innov_pre;
                ifr.var_brsd_i = a2 * r1.var_brsd_i + b2 * r2.var_brsd_i;
                ifr.var_brsd_p = a2 * r1.var_brsd_p + b2 * r2.var_brsd_p;
                // Distinct group key so IF rows correlate with each other
                // (same pivot variance) but not with any per-band rows
                // that might appear in mixed scenarios (currently they
                // don't; rows is fully replaced below).
                ifr.group_key = ifr.cons + "|IF|" + std::string(1, ifr.obs_type);
                ifr.amb_idx   = -1;
                // The IF ambiguity is c·(f1·N1 - f2·N2)/(f1²-f2²), never an
                // integer number of cycles.  It is forced float-only so AR's
                // exclusion set catches it at
                // @c getNonIntegerCapableDdAmbiguityKeys().
                ifr.integer_capable = false;
                if_rows.push_back(std::move(ifr));
            }

            if (diag) {
                klog->info("DD diag: IF row collapse: {} per-band rows -> {} IF rows "
                           "({} sat-pair groups dropped for <2 bands, "
                           "{} dropped because best-pair K > {:.1f})",
                           rows.size(), if_rows.size(), n_dropped_single_band,
                           n_dropped_high_K, kMaxIfNoiseFactor);
            }
            rows = std::move(if_rows);
        }
    }

    bool MeasurementModel::ddHasRows(const DdBuild& dd) const {
        const auto& klog = dd.klog;
        const bool diag = dd.diag;
        const auto& rows = dd.rows;

        if (rows.empty()) {
            if (diag) klog->info("DD diag: EARLY RETURN — rows empty (no shared band/obs_type with the pivot)");
            return false;
        }
        return true;
    }

    void MeasurementModel::ddPruneStaleAmbiguities(const std::set<std::string>& excluded_measurements,
                                                   const DdBuild& dd) {
        const auto& pivots = dd.pivots;
        const auto& rows = dd.rows;

        // --- Prune stale DD ambiguity states ---
        // Compute the set of DD ambiguity keys this epoch will reference
        // by reading the final post-collapse @c rows vector.  Each phase
        // row contributes one expected key built from its rov_code, which
        // is either a single RINEX code ("L1C") in per-band mode or the
        // concatenated IF label ("L1C+L2W") after iono-free collapse.
        // Any DD-keyed ambiguity in the state that is not in this set is
        // stale and removed.  This covers every leak path in one pass:
        //   - pivot change for a group (old pivot's keys vanish from the set)
        //   - rover or reference tracker switching codes mid-arc
        //   - satellite leaving view at either receiver
        //   - satellite added to excluded_satellites at runtime
        //   - iono-free toggle changing the code segment of every key
        //   - any other condition that produces no row this epoch
        // This must run before ddAssemble sizes the em.H column indices:
        // the H matrix uses state_manager.getStateSize(), and
        // removeAmbiguity() compacts the state vector and shifts
        // dynamic-state indices.  Building the expected set from @c rows
        // (rather than from the pre-collapse @c by_cons / @c brsd_signals)
        // makes the expected and added keys agree on whether the code
        // segment is per-band or an IF concatenation; otherwise every IF DD
        // ambiguity would be pruned and re-added each epoch, losing its
        // covariance.
        // Build the expected set as parsed (cons, sat_i, code, pivot) tuples
        // so the prune doesn't depend on string-formatting details of
        // makeDdAmbKey matching exactly what's stored.  Then walk the
        // ambiguity map, parse each DD-namespaced key, and remove any
        // tuple not in the expected set.
        using DdTuple = std::tuple<std::string, std::string, std::string, std::string>;
        std::set<DdTuple> expected;
        for (const auto& r : rows) {
            if (r.obs_type != 'L') continue;
            expected.emplace(r.cons, r.sat_i, r.rov_code, r.sat_p);
        }
        // Protect the ambiguities of phase rows excluded this pass by the KF
        // rebuild loop.  An excluded row never enters @c rows, so its
        // (cons,sat_i,code,pivot) tuple is absent from @c expected above, and
        // the prune below would delete the ambiguity state the KF has just
        // soft-reset.  That would undo the outlier-recovery reseed and force a
        // from-scratch reinit (innov_pre, amb_init_var) next epoch, losing the
        // converged float and its covariance.  Each excluded row's expected
        // tuple is therefore reconstructed under this epoch's pivot and kept.
        // The KF exclusion token is "sat_i_<rov_code>" (or "..._<rov>/<ref>"
        // when aliased); the DD ambiguity key uses rov_code.  Genuine leak
        // paths (sat dropout, code switch, pivot change) never appear in
        // @c excluded_measurements, since those sats produce no row to
        // exclude, so this does not weaken the prune.
        for (const auto& ex : excluded_measurements) {
            const auto us = ex.find('_');
            if (us == std::string::npos || us + 1 >= ex.size()) continue;
            const std::string sat_i = ex.substr(0, us);
            std::string code = ex.substr(us + 1);
            const auto slash = code.find('/');
            if (slash != std::string::npos) code = code.substr(0, slash);  // rover code
            const std::string cons = ddConstellationOf(sat_i);
            auto pit = pivots.find(cons);
            if (pit == pivots.end()) continue;
            expected.emplace(cons, sat_i, code, pit->second);
        }
        std::vector<std::string> to_prune;
        for (const auto& [key, idx] : state_manager_.getAmbiguities()) {
            const auto parts = StateKeys::parseDdAmbiguityKey(key);
            if (!parts) continue;
            if (expected.count({ parts->cons, parts->sat_i, parts->code, parts->pivot }))
                continue;
            to_prune.push_back(key);
        }
        for (const auto& k : to_prune) state_manager_.removeAmbiguity(k);
        if (!to_prune.empty()) {
            log_manager_.getLogger("main_log")->info(
                "DD: pruned {} stale ambiguity state(s) (sat dropout, code "
                "switch, or pivot change)", to_prune.size());
        }
    }

    void MeasurementModel::ddNonIntegerKeys(const DdBuild& dd) {
        const auto& rows = dd.rows;

        // --- Refresh the AR-exclusion-key snapshot ---
        // A phase row whose four observations do not all use the same
        // RINEX code carries a residual receiver IFB or satellite DCB
        // term that does not round to an integer (and every IF row is
        // non-integer).  Those keys are exposed to
        // AmbiguityResolver::resolveDd via getNonIntegerCapableDdAmbiguityKeys()
        // so they stay float-only.  The set is rebuilt from scratch each
        // epoch from the current phase rows; code rows are skipped because
        // they do not create ambiguity states.
        non_integer_capable_dd_ambiguity_keys_.clear();
        for (const auto& r : rows) {
            if (r.obs_type != 'L') continue;
            if (r.integer_capable) continue;
            non_integer_capable_dd_ambiguity_keys_.insert(
                makeDdAmbKey(r.cons, r.sat_i, r.rov_code, r.sat_p));
        }
    }

    void MeasurementModel::ddEnsureAmbiguities(DdBuild& dd) {
        auto& rows = dd.rows;

        const bool code_only = config_.getBaselineConfig().code_only;
        const double amb_init_std = config_.getInitialStd().ambiguity;
        const double amb_init_var =
            (amb_init_std > 0.0 ? amb_init_std * amb_init_std : 1e8);

        // --- Ensure DD ambiguity states exist for phase rows ---
        if (code_only) return;
        for (auto& r : rows) {
            if (r.obs_type != 'L') continue;
            // Ambiguity key uses the rover's RINEX code.  When
            // aliasing is on and the rover and reference disagree on
            // tracking attribute, the float ambiguity will absorb the
            // residual sat-DCB; integer fixing requires DCB
            // harmonization (Phase 2 plan) to be valid.
            const std::string key = makeDdAmbKey(r.cons, r.sat_i, r.rov_code, r.sat_p);
            int aidx = state_manager_.getAmbiguityIndex(key);
            if (aidx < 0) {
                state_manager_.addAmbiguity(key, r.innov_pre);
                aidx = state_manager_.getAmbiguityIndex(key);
                if (aidx >= 0) {
                    Eigen::MatrixXd P = state_manager_.getCovariance();
                    P(aidx, aidx) = amb_init_var;
                    state_manager_.setCovariance(P);
                }
            }
            r.amb_idx = aidx;
        }
    }

    bool MeasurementModel::ddInnovations(const Eigen::VectorXd& current_state, DdBuild& dd) const {
        const auto& klog = dd.klog;
        const bool diag = dd.diag;
        auto& rows = dd.rows;
        auto& kept = dd.kept;
        // The same setting ddEnsureAmbiguities reads: code-only builds no phase row.
        const bool code_only = config_.getBaselineConfig().code_only;

        // --- Per-row innov (code = pre; phase = pre - amb_state) ---
        Eigen::VectorXd x;
        try {
            const size_t ss = state_manager_.getStateSize();
            x = (current_state.size() == static_cast<Eigen::Index>(ss))
                ? current_state
                : state_manager_.getStateVector();
        } catch (const std::exception& ex) {
            klog->error("DD state-vector fetch failed: {}", ex.what());
            return false;
        } catch (...) {
            klog->error("DD state-vector fetch failed: unknown exception");
            return false;
        }
        kept.reserve(rows.size());
        try {
            for (size_t i = 0; i < rows.size(); ++i) {
                auto& r = rows[i];
                if (code_only && r.obs_type == 'L') continue;
                if (r.obs_type == 'L') {
                    if (r.amb_idx < 0) continue;
                    if (r.amb_idx >= x.size()) {
                        klog->warn("DD row {} amb_idx={} out of range x.size={}", i, r.amb_idx, x.size());
                        continue;
                    }
                    r.innov = r.innov_pre - x(r.amb_idx);
                } else {
                    r.innov = r.innov_pre;
                }
                kept.push_back(i);
            }
        } catch (const std::exception& ex) {
            klog->error("DD innov loop failed: {}", ex.what());
            return false;
        }
        const Eigen::Index m = static_cast<Eigen::Index>(kept.size());
        if (diag) {
            klog->info("DD diag: {} total DD rows built, {} kept after filter (code_only={})",
                       rows.size(), m, code_only);
        }
        if (m == 0) {
            if (diag) klog->info("DD diag: EARLY RETURN — m==0 (all rows filtered out)");
            return false;
        }
        return true;
    }

    void MeasurementModel::ddAssemble(const DdBuild& dd, EpochMeasurement& em) const {
        const auto& klog = dd.klog;
        const auto& rows = dd.rows;
        const auto& kept = dd.kept;
        // The row count ddInnovations kept (non-zero: it stops the build at zero).
        const Eigen::Index m = static_cast<Eigen::Index>(kept.size());

        try {
            const size_t n_state = state_manager_.getStateSize();
            em.innovations = Eigen::VectorXd(m);
            em.H = Eigen::MatrixXd::Zero(m, static_cast<Eigen::Index>(n_state));
            em.R_full = Eigen::MatrixXd::Zero(m, m);
            em.R_v = Eigen::VectorXd::Zero(m);
            em.satellite_ids.reserve(static_cast<size_t>(m));
            em.signal_types.reserve(static_cast<size_t>(m));

            // Differential ZWD column index, read once outside the row
            // loop; max() if not present (PPP / PPP-RTK, or baseline with
            // estimate_differential_tropo=false).
            const size_t diff_tropo_idx = state_manager_.getDifferentialTropoIndex();
            const bool has_diff_tropo = state_manager_.hasDifferentialTropo()
                && diff_tropo_idx < n_state;

            for (Eigen::Index a = 0; a < m; ++a) {
                const auto& r = rows[kept[static_cast<size_t>(a)]];
                em.H.block(a, 0, 1, 3) = r.h_pos;
                if (r.obs_type == 'L' && r.amb_idx >= 0
                    && static_cast<size_t>(r.amb_idx) < n_state) {
                    em.H(a, r.amb_idx) = 1.0;
                }
                // Differential ZWD partial: phase rows only (the wet delay
                // affects code identically but adding code rows would just
                // double-up the geometry constraint with poorer SNR).  In
                // DD between non-pivot i and pivot p, ∂y/∂Δzwd ≈ mf_w_i −
                // mf_w_p (rover-side, short-baseline approximation that the
                // mapping functions match between rover and reference).
                if (has_diff_tropo && r.obs_type == 'L') {
                    em.H(a, static_cast<Eigen::Index>(diff_tropo_idx)) =
                        r.mf_w_rov_i - r.mf_w_rov_p;
                }
                em.innovations(a) = r.innov;
                em.R_v(a)         = r.var_brsd_i + r.var_brsd_p;
                em.satellite_ids.push_back(r.sat_i + "-" + r.sat_p);
                // Surface the rover code in residual traces.  When aliased
                // with a different reference attribute, append the ref
                // code so log readers can spot mixed-attribute rows.
                em.signal_types.push_back(
                    (r.rov_code == r.ref_code)
                        ? r.rov_code
                        : (r.rov_code + "/" + r.ref_code));
            }
            for (Eigen::Index a = 0; a < m; ++a) {
                const auto& ra = rows[kept[static_cast<size_t>(a)]];
                em.R_full(a, a) = em.R_v(a);
                for (Eigen::Index b = a + 1; b < m; ++b) {
                    const auto& rb = rows[kept[static_cast<size_t>(b)]];
                    if (ra.group_key != rb.group_key) continue;
                    const double cov = ra.var_brsd_p;
                    em.R_full(a, b) = cov;
                    em.R_full(b, a) = cov;
                }
            }
        } catch (const std::exception& ex) {
            klog->error("DD H/R assembly failed: {}", ex.what());
            return;
        } catch (...) {
            klog->error("DD H/R assembly failed: unknown exception");
            return;
        }
    }

} // namespace GnssProcessor
