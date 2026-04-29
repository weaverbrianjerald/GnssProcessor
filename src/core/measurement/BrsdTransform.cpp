/**
 * @file BrsdTransform.cpp
 * @brief Implementation of the between-receiver single-difference transform.
 *
 * The math here is a single subtraction in residual space plus a
 * variance composition.  The reason it deserves its own translation
 * unit is so the downstream measurement-model code can be a clean
 * composition of BRSD + BSSD operators rather than an inline four-way
 * subtraction whose variance bookkeeping is hard to audit.
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

        /// Variance lookup with non-positive / non-finite filtering.
        /// Returns 0.0 on missing or invalid entries — the caller can then
        /// decide whether a zero-variance row is acceptable (it usually
        /// isn't, since R diagonal entries must be positive).
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

        // The BRSD residual is the rov-vs-ref difference of the
        // per-receiver (observed - modeled).  Sign convention: positive
        // residual = the rover sees the satellite "later" (longer
        // pseudorange / phase) than the reference, after both stations
        // have applied their full PppModel correction stack.
        b.residual = *r_rov - *r_ref;

        // Variance composition: assume per-receiver observation noise
        // is independent.  Holds for thermal noise; multipath
        // correlations would invalidate this but aren't modeled here.
        b.var_rov  = varianceOrZero(rov.variances, rov_code);
        b.var_ref  = varianceOrZero(ref.variances, ref_code);
        b.variance = b.var_rov + b.var_ref;

        // Rover-side geometry: only the rover position is a state in
        // baseline mode, so the geometry partial downstream is built
        // from the rover LOS alone.  The reference LOS is informational
        // — preserved here only because future network-mode work might
        // estimate the reference position too.
        b.los_rov       = rov.los;
        b.elevation_rov = rov.elevation;
        b.time          = rov.time;
        return b;
    }

} // namespace GnssProcessor
