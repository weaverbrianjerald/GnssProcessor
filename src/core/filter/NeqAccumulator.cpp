/**
 * @file NeqAccumulator.cpp
 * @brief Implementation of the cross-session normal-equation accumulator (item #13B).
 */

#include "filter/NeqAccumulator.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>

namespace GnssProcessor {

    bool NeqAccumulator::isEpochLocal_(const std::string& l) {
        // Genuinely per-epoch dynamic states: the receiver clock family, the
        // kinematic velocity / IMU error states, and the fast per-satellite
        // ionosphere.  These are Schur-eliminated each epoch so the retained
        // system spans only the (approximately) time-constant parameters.
        return l == "CLK" || l == "CLK_RATE" || l == "PHASE_CLK"
            || l == "VEL_X" || l == "VEL_Y" || l == "VEL_Z"
            || l.rfind("ATT_", 0) == 0
            || l.rfind("BACC_", 0) == 0
            || l.rfind("BGYR_", 0) == 0
            || l.rfind("IONO:", 0) == 0
            // A state StateManager::getStateLabels() leaves unnamed
            // ("STATE_<i>") is keyed by raw index, which changes across
            // prune and grow.  It is eliminated per epoch so that one physical
            // parameter is not split into several global parameters.
            || l.rfind("STATE_", 0) == 0;
    }

    bool NeqAccumulator::isArcLabel_(const std::string& l) {
        return l.rfind("AMB:", 0) == 0;
    }

    bool NeqAccumulator::resetMatchesArc_(const std::string& arc_base,
                                          const std::set<std::string>& reset_satellites) {
        if (reset_satellites.empty()) return false;
        // arc_base is "AMB:<sat>_<signal>"; extract the satellite token.
        if (arc_base.size() <= 4) return false;
        const std::string body = arc_base.substr(4);   // after "AMB:"
        const auto us = body.find('_');
        const std::string sat = (us == std::string::npos) ? body : body.substr(0, us);
        return reset_satellites.count(sat) > 0;
    }

    Eigen::Index NeqAccumulator::globalIndexFor_(const std::string& label) {
        auto it = label_to_global_.find(label);
        if (it != label_to_global_.end()) return it->second;
        const Eigen::Index g = N_.rows();
        if (g == 0) {
            N_ = Eigen::MatrixXd::Zero(1, 1);
            u_ = Eigen::VectorXd::Zero(1);
        } else {
            N_.conservativeResize(g + 1, g + 1);
            N_.row(g).setZero();
            N_.col(g).setZero();
            u_.conservativeResize(g + 1);
            u_(g) = 0.0;
        }
        label_to_global_[label] = g;
        global_labels_.push_back(label);
        return g;
    }

    void NeqAccumulator::accumulate(const EpochMeasurement& meas,
                                    const std::vector<std::string>& labels,
                                    const std::set<std::string>& reset_satellites) {
        const Eigen::Index m = meas.innovations.size();
        const Eigen::Index n = meas.H.cols();
        // An epoch that cannot be folded into the system is a gap in the
        // accumulated NEQ.  The arc-presence set is cleared so the next usable
        // epoch starts new arcs for the affected ambiguities.  This is
        // conservative: it avoids merging two physically distinct arcs across
        // the skipped epoch.
        if (m == 0 || meas.H.rows() != m ||
            static_cast<Eigen::Index>(labels.size()) != n) {
            prev_epoch_arcs_.clear();
            return;   // empty or mismatched dimensions: skip this epoch (the caller checks and logs a label-count mismatch before calling)
        }

        // --- Active columns: those with any nonzero design-matrix entry. ---
        std::vector<Eigen::Index> active;
        active.reserve(static_cast<std::size_t>(n));
        for (Eigen::Index c = 0; c < n; ++c) {
            if (meas.H.col(c).cwiseAbs().maxCoeff() > 0.0) active.push_back(c);
        }
        const Eigen::Index a = static_cast<Eigen::Index>(active.size());
        if (a == 0) { prev_epoch_arcs_.clear(); return; }

        // --- Epoch normal contribution Ne = Haᵀ R⁻¹ Ha, ue = Haᵀ R⁻¹ Δz,
        //     ltple = Δzᵀ R⁻¹ Δz, over the active columns. ---
        Eigen::MatrixXd Ha(m, a);
        for (Eigen::Index k = 0; k < a; ++k) Ha.col(k) = meas.H.col(active[k]);
        const Eigen::VectorXd& dz = meas.innovations;

        Eigen::MatrixXd Ne;
        Eigen::VectorXd ue;
        double ltple = 0.0;
        const bool dense_r = (meas.R_full.rows() == m && meas.R_full.cols() == m);
        if (dense_r) {
            Eigen::LDLT<Eigen::MatrixXd> rldlt(meas.R_full);
            // Skip an ill-posed R: LDLT.info() catches NaN and structural
            // failure, and isPositive() rejects an indefinite (negative-pivot)
            // block.  A dense R has to be SPD to give a valid weight.
            if (rldlt.info() != Eigen::Success || !rldlt.isPositive()) {
                prev_epoch_arcs_.clear();
                return;
            }
            const Eigen::MatrixXd RinvHa = rldlt.solve(Ha);    // m×a
            const Eigen::VectorXd Rinvdz = rldlt.solve(dz);    // m
            Ne = Ha.transpose() * RinvHa;
            ue = Ha.transpose() * Rinvdz;
            ltple = dz.dot(Rinvdz);
        } else {
            if (meas.R_v.size() != m) { prev_epoch_arcs_.clear(); return; }
            Eigen::VectorXd w(m);
            for (Eigen::Index i = 0; i < m; ++i) {
                const double r = meas.R_v(i);
                w(i) = (std::isfinite(r) && r > 0.0) ? (1.0 / r) : 0.0;
            }
            const Eigen::MatrixXd WHa = w.asDiagonal() * Ha;   // m×a
            Ne = Ha.transpose() * WHa;
            ue = Ha.transpose() * (w.asDiagonal() * dz);
            ltple = (w.array() * dz.array().square()).sum();
        }

        // --- Arc-stamp the carrier-ambiguity labels: a re-acquisition gap or a
        //     flagged slip-reset starts a fresh segment. ---
        std::set<std::string> cur_arcs;
        for (Eigen::Index k = 0; k < a; ++k) {
            const std::string& base = labels[active[k]];
            if (isArcLabel_(base)) cur_arcs.insert(base);
        }
        for (const auto& base : cur_arcs) {
            const bool first_time = (arc_counter_.find(base) == arc_counter_.end());
            const bool gap = (!first_time &&
                              prev_epoch_arcs_.find(base) == prev_epoch_arcs_.end());
            const bool reset_now = resetMatchesArc_(base, reset_satellites);
            if (first_time)               arc_counter_[base] = 0;
            else if (gap || reset_now)    arc_counter_[base] += 1;
        }
        std::vector<std::string> col_label(static_cast<std::size_t>(a));
        for (Eigen::Index k = 0; k < a; ++k) {
            const std::string& base = labels[active[k]];
            col_label[static_cast<std::size_t>(k)] = isArcLabel_(base)
                ? base + "#" + std::to_string(arc_counter_[base])
                : base;
        }
        prev_epoch_arcs_ = std::move(cur_arcs);

        // --- Partition active columns into global (retained) and epoch-local. ---
        std::vector<Eigen::Index> gidx, lidx;
        for (Eigen::Index k = 0; k < a; ++k) {
            if (isEpochLocal_(labels[active[k]])) lidx.push_back(k);
            else                                  gidx.push_back(k);
        }
        const Eigen::Index ng = static_cast<Eigen::Index>(gidx.size());
        const Eigen::Index nl = static_cast<Eigen::Index>(lidx.size());
        if (ng == 0) {
            // No retained parameter is touched this epoch, so its rows
            // constrain only eliminated locals and contribute nothing to the
            // retained system.  They are not counted in n_obs_: dof, n_local_
            // and ltpl_ have to be drawn from the same epochs, or σ̂₀²
            // (= WSSR/dof) is biased.  In practice this branch does not run,
            // because position is always retained; it keeps the accounting exact.
            return;
        }

        auto sub = [&Ne](const std::vector<Eigen::Index>& ri,
                         const std::vector<Eigen::Index>& ci) {
            Eigen::MatrixXd out(static_cast<Eigen::Index>(ri.size()),
                                static_cast<Eigen::Index>(ci.size()));
            for (std::size_t i = 0; i < ri.size(); ++i)
                for (std::size_t j = 0; j < ci.size(); ++j)
                    out(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) =
                        Ne(ri[i], ci[j]);
            return out;
        };
        auto subv = [&ue](const std::vector<Eigen::Index>& ri) {
            Eigen::VectorXd out(static_cast<Eigen::Index>(ri.size()));
            for (std::size_t i = 0; i < ri.size(); ++i)
                out(static_cast<Eigen::Index>(i)) = ue(ri[i]);
            return out;
        };

        Eigen::MatrixXd Ngg = sub(gidx, gidx);
        Eigen::VectorXd ug  = subv(gidx);

        Eigen::MatrixXd Ncontrib;
        Eigen::VectorXd ucontrib;
        double ltpl_contrib;
        Eigen::Index local_elim = 0;

        if (nl > 0) {
            Eigen::MatrixXd Ngl = sub(gidx, lidx);   // ng×nl
            Eigen::MatrixXd Nll = sub(lidx, lidx);   // nl×nl
            Eigen::VectorXd ul  = subv(lidx);
            Eigen::LDLT<Eigen::MatrixXd> lldlt(Nll);
            if (lldlt.info() != Eigen::Success) {
                Nll.diagonal().array() += 1e-9;      // tiny ridge, then retry
                lldlt.compute(Nll);
            }
            if (lldlt.info() != Eigen::Success) {
                // The local block is singular even after the ridge.  The epoch
                // is dropped rather than folded without its local cross terms.
                // Its rows are not counted in n_obs_ (nothing was added to
                // ltpl_ or N_), so dof and WSSR stay drawn from the same epochs.
                return;
            }
            const Eigen::MatrixXd Nll_inv_Nlg = lldlt.solve(Ngl.transpose());  // nl×ng
            const Eigen::VectorXd Nll_inv_ul  = lldlt.solve(ul);               // nl
            Ncontrib    = Ngg - Ngl * Nll_inv_Nlg;
            ucontrib    = ug  - Ngl * Nll_inv_ul;
            ltpl_contrib = ltple - ul.dot(Nll_inv_ul);
            local_elim  = nl;
        } else {
            Ncontrib = Ngg;
            ucontrib = ug;
            ltpl_contrib = ltple;
        }

        // --- Map the retained labels into the growing global system and
        //     accumulate (resize-all-first, then add). ---
        std::vector<Eigen::Index> gmap(static_cast<std::size_t>(ng));
        for (Eigen::Index k = 0; k < ng; ++k)
            gmap[static_cast<std::size_t>(k)] =
                globalIndexFor_(col_label[static_cast<std::size_t>(gidx[static_cast<std::size_t>(k)])]);

        for (Eigen::Index i = 0; i < ng; ++i) {
            const Eigen::Index gi = gmap[static_cast<std::size_t>(i)];
            u_(gi) += ucontrib(i);
            for (Eigen::Index j = 0; j < ng; ++j)
                N_(gi, gmap[static_cast<std::size_t>(j)]) += Ncontrib(i, j);
        }

        ltpl_   += ltpl_contrib;
        n_obs_  += static_cast<std::size_t>(m);
        n_local_ += static_cast<std::size_t>(local_elim);
    }

    NeqAccumulator::Solution NeqAccumulator::solve() const {
        Solution s;
        s.labels   = global_labels_;
        s.n_obs    = n_obs_;
        s.n_local  = n_local_;
        s.n_params = static_cast<std::size_t>(N_.rows());
        s.wssr     = ltpl_;
        if (N_.rows() == 0) return s;

        Eigen::LDLT<Eigen::MatrixXd> ldlt(N_);
        if (ldlt.info() != Eigen::Success) return s;   // singular → ok == false

        s.x = ldlt.solve(u_);
        s.wssr = ltpl_ - u_.dot(s.x);
        const double params = static_cast<double>(N_.rows()) +
                              static_cast<double>(n_local_);
        s.dof = static_cast<double>(n_obs_) - params;
        s.sigma0_sq = (s.dof > 0.0)
            ? s.wssr / s.dof
            : std::numeric_limits<double>::quiet_NaN();

        // Per-parameter formal σ = √(σ̂₀²·(N⁻¹)ᵢᵢ).  N is small (retained
        // parameters only), so the dense inverse is cheap.
        const Eigen::MatrixXd Ninv =
            ldlt.solve(Eigen::MatrixXd::Identity(N_.rows(), N_.rows()));
        const double vf = (std::isfinite(s.sigma0_sq) && s.sigma0_sq > 0.0)
            ? s.sigma0_sq : 1.0;
        s.sigma.resize(N_.rows());
        for (Eigen::Index i = 0; i < N_.rows(); ++i)
            s.sigma(i) = std::sqrt(std::max(vf * Ninv(i, i), 0.0));

        s.ok = true;
        return s;
    }

    bool NeqAccumulator::writeFiles(const std::filesystem::path& stem,
                                    const Solution& sol) const {
        if (N_.rows() == 0) return false;

        const Eigen::Index P = N_.rows();
        const double seuw = (std::isfinite(sol.sigma0_sq) && sol.sigma0_sq > 0.0)
            ? std::sqrt(sol.sigma0_sq) : std::numeric_limits<double>::quiet_NaN();

        // -------- Human-readable .asc (modelled on the M-PAGES .nrm ASCII dump) --------
        {
            std::ofstream asc(stem.string() + ".asc", std::ios::out | std::ios::trunc);
            if (!asc.is_open()) return false;
            asc << std::scientific << std::setprecision(10);
            asc << "# GnssProcessor normal-equation export (item #13B)\n";
            asc << "# Retained parameters are time-constant / per-arc; epoch-local\n";
            asc << "# states (clock family, velocity, IMU, fast iono) were Schur-eliminated.\n";
            asc << "# NOTE: ZWD / gradients are retained as session-constant (the filter\n";
            asc << "#   random-walks them), so sigma0_sq is the variance factor of THIS\n";
            asc << "#   constant-troposphere batch reparameterization, not of the KF run;\n";
            asc << "#   a value >> 1 on long sessions is expected from tropo time-variation.\n";
            asc << "=================================================================\n";
            asc << "n_params:        " << P << "\n";
            asc << "n_obs:           " << sol.n_obs << "\n";
            asc << "n_local_elim:    " << sol.n_local << "\n";
            asc << "dof:             " << sol.dof << "\n";
            asc << "sigma0_sq:       " << sol.sigma0_sq << "\n";
            asc << "seuw:            " << seuw << "\n";
            asc << "wssr:            " << sol.wssr << "\n";
            asc << "solved:          " << (sol.ok ? "yes" : "no") << "\n";
            asc << "=================================================================\n";
            asc << "# parameters: index  label  xhat  sigma\n";
            for (Eigen::Index i = 0; i < P; ++i) {
                const double xi = (sol.ok && i < sol.x.size()) ? sol.x(i)
                    : std::numeric_limits<double>::quiet_NaN();
                const double si = (sol.ok && i < sol.sigma.size()) ? sol.sigma(i)
                    : std::numeric_limits<double>::quiet_NaN();
                asc << "Param[" << std::setw(5) << i << "] == "
                    << std::left << std::setw(28)
                    << (i < static_cast<Eigen::Index>(global_labels_.size())
                            ? global_labels_[static_cast<std::size_t>(i)] : std::string("?"))
                    << std::right << "  xhat=" << xi << "  sigma=" << si << "\n";
            }
            asc << "=================================================================\n";
            asc << "# normal matrix: index  N(i,i)  u(i)  rowMax  rowMin\n";
            for (Eigen::Index i = 0; i < P; ++i) {
                double rmax = -std::numeric_limits<double>::infinity();
                double rmin =  std::numeric_limits<double>::infinity();
                for (Eigen::Index j = 0; j < P; ++j) {
                    rmax = std::max(rmax, N_(i, j));
                    rmin = std::min(rmin, N_(i, j));
                }
                asc << std::setw(5) << i << "  " << N_(i, i) << "  " << u_(i)
                    << "  " << rmax << "  " << rmin << "\n";
            }
            asc << "Basement Window (post-fit WSSR): " << sol.wssr << "\n";
            if (!asc.good()) return false;
        }

        // -------- Compact, round-trippable .neq (upper-triangular N + u) --------
        {
            std::ofstream neq(stem.string() + ".neq", std::ios::out | std::ios::trunc);
            if (!neq.is_open()) return false;
            neq << std::scientific << std::setprecision(16);
            neq << "# GnssProcessor .neq v1\n";
            neq << "NPARAMS " << P << "\n";
            neq << "NOBS " << sol.n_obs << "\n";
            neq << "NLOCAL " << sol.n_local << "\n";
            neq << "LTPL " << ltpl_ << "\n";
            neq << "SIGMA0SQ " << sol.sigma0_sq << "\n";
            neq << "LABELS\n";
            for (Eigen::Index i = 0; i < P; ++i)
                neq << (i < static_cast<Eigen::Index>(global_labels_.size())
                            ? global_labels_[static_cast<std::size_t>(i)] : std::string("?"))
                    << "\n";
            neq << "N\n";   // upper triangle, "i j value"
            for (Eigen::Index i = 0; i < P; ++i)
                for (Eigen::Index j = i; j < P; ++j)
                    neq << i << " " << j << " " << N_(i, j) << "\n";
            neq << "U\n";
            for (Eigen::Index i = 0; i < P; ++i)
                neq << i << " " << u_(i) << "\n";
            neq << "END\n";
            if (!neq.good()) return false;
        }

        return true;
    }

} // namespace GnssProcessor
