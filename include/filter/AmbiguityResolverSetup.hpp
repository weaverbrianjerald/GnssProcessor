/**
 * @file AmbiguityResolverSetup.hpp
 * @brief The one builder of an @ref GnssProcessor::AmbiguityResolverConfig from a
 *        @ref GnssProcessor::GnssConfig.
 *
 * Three call sites construct an ambiguity resolver: the post-processing PPP / PPP-RTK
 * driver, each baseline rover, and the real-time engine (and with it the closed-loop
 * consumer).  Each used to copy the configuration field by field, and the copies
 * drifted: the baseline copy dropped @c min_arc_info and the real-time copy dropped
 * @c min_arc_info and @c enable_partial_ar, so a setting that the post-processing
 * resolver honoured was ignored without a word in the other two.  The builder below is
 * the only place that copies, so a field added to the resolver's configuration is
 * wired to every resolver at once.
 *
 * It lives beside, not inside, the resolver's header: @c AmbiguityResolver.hpp owns the
 * configuration struct, and this header is the only place that reads a
 * @c GnssConfig to fill it.
 */
#pragma once

#include "filter/AmbiguityResolver.hpp"

namespace GnssProcessor {

class GnssConfig;

/**
 * @brief Fill an @ref AmbiguityResolverConfig from the ambiguity-resolution settings of a
 *        configuration.
 *
 * Copies every field of the resolver's configuration: the mode, the ratio test and its
 * upper bound, the bootstrap success rate, the per-entry fraction cap, the elevation
 * mask, the hold variance, the convergence guards (minimum epochs, position sigma), the
 * WL-only settings (pseudo-observation variance, fraction cap, FFR gate, minimum arc
 * count, innovation cap), the per-candidate maturity gate (@c min_arc_info), the
 * narrowlane functional, partial AR, the active constellations and the state ambiguity
 * basis.  A field left at its default in the configuration yields the struct's own
 * default, so a run that sets none of the optional settings builds the resolver it
 * always built.
 *
 * Pure: reads @p config, logs nothing, owns nothing.
 *
 * @param config The loaded configuration (any run mode).
 * @return The resolver configuration.
 */
AmbiguityResolverConfig makeAmbiguityResolverConfig(const GnssConfig& config);

} // namespace GnssProcessor
