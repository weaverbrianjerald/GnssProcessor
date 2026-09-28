/**
 * @file BrsdTransform.cpp
 * @brief Implementation of the between-receiver single-difference transform.
 *
 * The operation is one subtraction in residual space plus a variance
 * sum.  It has its own translation unit so the downstream
 * measurement-model code composes BRSD and BSSD operators instead of
 * an inline four-way subtraction whose variance bookkeeping is hard
 * to audit.
 */

#include "measurement/BrsdTransform.hpp"

#include <cmath>

namespace GnssProcessor {

    namespace {

        /// Per-receiver phase/code residual: @c observed - @c modeled with
        /// missing-key and NaN safety.  Returns @c std::nullopt when either
        /// map lookup fails or the difference is non-finite.
        inline std::optional<double> singleReceiverResidual(
            const PppObservation& obs, const std::string& code)
        {
            const auto io = obs.observed.find(code);
            const auto im = obs.modeled.find(code);
            if (io == obs.observed.end() || im == obs.modeled.end()) return std::nullopt;
            const double r = io->second - im->second;
            if (!std::isfinite(r)) return std::nullopt;
            return r;
        }

        /// Variance lookup that rejects non-positive and non-finite values.
        /// Returns 0.0 for a missing or invalid entry; the caller decides
        /// whether a zero-variance row is acceptable (usually not, since
        /// the diagonal entries of R must be positive).
        inline double varianceOrZero(
            const std::map<std::string, double>& variances,
            const std::string& code)
        {
            const auto it = variances.find(code);
            return (it != variances.end()
                 && std::isfinite(it->second)
                 && it->second > 0.0) ? it->second : 0.0;
        }

    } // anonymous namespace

    std::optional<BrsdObservation> BrsdTransform::apply(
        const PppObservation& rov,
        const PppObservation& ref,
        const std::string& constellation,
        const std::string& rov_code,
        const std::string& ref_code,
        FrequencyBand band,
        char obs_type)
    {
        const auto r_rov = singleReceiverResidual(rov, rov_code);
        const auto r_ref = singleReceiverResidual(ref, ref_code);
        if (!r_rov || !r_ref) return std::nullopt;

        BrsdObservation b;
        b.satellite     = rov.satellite;
        b.constellation = constellation;
        b.band          = band;
        b.obs_type      = obs_type;
        b.rov_code      = rov_code;
        b.ref_code      = ref_code;
        b.brsd_aliased  = (rov_code != ref_code);

        // The BRSD residual is the rover-minus-reference difference of the
        // per-receiver (observed - modeled).  Sign convention: a positive
        // residual means the rover's observation exceeds its model (longer
        // pseudorange or phase) by more than the reference's does, after
        // both stations apply their full PppModel correction stack.
        b.residual = *r_rov - *r_ref;

        // Variance sum: per-receiver observation noise is assumed
        // independent.  This holds for thermal noise; multipath
        // correlation would violate it and is not modeled.
        b.var_rov  = varianceOrZero(rov.variances, rov_code);
        b.var_ref  = varianceOrZero(ref.variances, ref_code);
        b.variance = b.var_rov + b.var_ref;

        // Rover-side geometry: only the rover position is a state in
        // baseline mode, so the downstream geometry partial is built
        // from the rover LOS alone.  The reference LOS is not copied
        // into the output.
        b.los_rov       = rov.los;
        b.elevation_rov = rov.elevation;
        b.time          = rov.time;
        return b;
    }

} // namespace GnssProcessor
