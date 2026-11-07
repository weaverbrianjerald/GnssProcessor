/**
 * @file AmbiguityResolverSetup.cpp
 * @brief The shared builder of the ambiguity resolver's configuration.
 *
 * See AmbiguityResolverSetup.hpp.
 */

#include "filter/AmbiguityResolverSetup.hpp"

#include "config/GnssConfig.hpp"

namespace GnssProcessor {

AmbiguityResolverConfig makeAmbiguityResolverConfig(const GnssConfig& config) {
    AmbiguityResolverConfig ar_config;
    ar_config.ar_mode               = config.getArMode();
    ar_config.ratio_threshold       = config.getArRatioThreshold();
    ar_config.elevation_mask_ar     = config.getArElevationMask();
    ar_config.hold_variance         = config.getArHoldVariance();
    ar_config.min_epochs            = config.getArMinEpochs();
    ar_config.max_position_sigma_m  = config.getArMaxPositionSigma();
    ar_config.ratio_upper_bound     = config.getArRatioUpperBound();
    ar_config.wl_only               = config.getArWlOnly();
    ar_config.wl_only_pseudo_var    = config.getArWlOnlyPseudoVar();
    ar_config.wl_max_frac           = config.getArWlMaxFrac();
    ar_config.min_success_rate      = config.getArMinSuccessRate();
    ar_config.wl_only_ffr           = config.getArWlOnlyFfr();
    ar_config.wl_only_min_arc_count = config.getArWlOnlyMinArcCount();
    ar_config.wl_only_max_innov_cyc = config.getArWlOnlyMaxInnovCyc();
    ar_config.min_arc_info          = config.getArMinArcInfo();
    ar_config.nl_max_per_pair_frac  = config.getArNlMaxPerPairFrac();
    ar_config.nl_iono_free_row      = config.getArNlIonoFreeRow();
    ar_config.enable_partial_ar     = config.getArEnablePartialAr();
    ar_config.ar_active_systems     = config.getArActiveSystems();
    ar_config.state_ambiguity_basis = config.getStateAmbiguityBasis();
    return ar_config;
}

} // namespace GnssProcessor
