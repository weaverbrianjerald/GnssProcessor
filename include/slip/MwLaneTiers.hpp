#pragma once
/**
 * @file MwLaneTiers.hpp
 * @brief M5 card UWL-1 — the per-constellation MW-WL lane-tier band table
 *        (docs/SPEC_uwl_card.md §2/§3.1).
 *
 * The WL-2/WL-3 cascade (statistic → FFR → bridge → lane carry) is generic in
 * the pair wavelengths everywhere EXCEPT three hardcoded band digits that made
 * it structurally GPS-only, silently: the @c band5_of scan ('5'), the
 * @c updateBridgeArc band pick ({1,2,5}), and the carry third-band exclusion.
 * For Galileo the "EWL" pair degenerated to (L5Q,L5Q) — no detector arc, every
 * cycle dead as @c skip_missing_mw — and no bridge arc ever formed (no band
 * '2').  This table replaces those digits with ONE per-constellation triple:
 *
 * | cons | primary (WL) pair | top tier (statistic/bridge seed)      | top-tier λ |
 * |------|-------------------|---------------------------------------|------------|
 * | G    | (1,2) L1/L2       | EWL (2,5) = (L2,L5), Δf 51.15 MHz     |  5.861 m   |
 * | E    | (1,5) E1/E5a      | UWL (7,5) = (E5b,E5a), Δf 30.69 MHz   |  9.768 m   |
 * | E'8' | (1,5) E1/E5a      | UWL (8,5) = (E5 AltBOC,E5a), Δf 15.345| 19.537 m   |
 *
 * The Galileo top band is selectable (@c network.mw_wl.uwl_gal_top_band):
 * '7' (default) uses E5b; '8' uses the E5 AltBOC wideband — twice the
 * wavelength AND cm-level code noise, at the cost of C8/L8 tracking sparsity
 * (measure, don't assume — SPEC §7.5 study promoted to a config option).
 *
 * BDS-3 has the same (B2b,B2a)/(B2ab,B2a) pairs but stays OUT of the table —
 * BLOCKED on the open BeiDou network clock divergence (memory
 * `beidou-network-clock-divergence`); adding 'C' here is the whole of
 * increment 5's arming.  GLONASS is excluded (FDMA — no common λ integer DD).
 *
 * Conventions:
 *  - @c b1/@c b2 are the PRIMARY (WL) pair band digits in descending-frequency
 *    order (f(b1) > f(b2)) — they match what the estimator's two-lowest-band
 *    primary-pair election and the WL-3 census already produce.
 *  - @c b_top is the top-tier band digit that pairs with @c b2.  The (hi,lo)
 *    frequency order WITHIN the top pair is NOT encoded here — GPS has
 *    f(b2)&gt;f(b_top) (L2&gt;L5) while Galileo has f(b_top)&gt;f(b2)
 *    (E5b/E5&gt;E5a); consumers order by actual frequency (the MW statistic and
 *    the bridge λ must be positive).
 *  - A constellation absent from the table contributes NO top-tier lane; the
 *    wiring warns once instead of skipping silently (SPEC §3.1 — fact 3's
 *    silent death becomes loud).
 */

#include <optional>

namespace GnssProcessor {

/// One constellation's lane-tier band digits (see file header for the table).
struct MwLaneTier {
    char b1;          ///< primary (WL) pair high-frequency band digit ('1').
    char b2;          ///< primary (WL) pair low-frequency band digit — also the top pair's shared band.
    char b_top;       ///< top-tier partner band digit (GPS '5'; Galileo '7' or '8').
    char b_mid{'\0'}; ///< OPTIONAL mid-ladder rung band ('\0' = none).  Galileo '6' (E6):
                      ///< the (E6,E5a) λ 2.931 m rung between the 9.768/19.537 m top tier
                      ///< and the 0.751 m WL — the all-pairs census (Jul-16, 309 sta)
                      ///< measured it circR 0.91–0.95 with 161 stations tracking L6C.
                      ///< A mid hop cuts each bridge hop's λ amplification ~3.3× (the
                      ///< direct 19.5→0.75 hop scales phase+iono noise by λ_top; chaining
                      ///< through λ_mid re-rounds at 2.9 m first).  GPS has no candidate
                      ///< frequency between L2/L5 and the WL — always '\0'.
    char b_top2{'\0'};///< OPTIONAL fallback top rung (increment 5, @c uwl_dual_top; '\0' =
                      ///< single-top).  Galileo: preferred b_top '8' (E5 AltBOC, λ 19.537 m)
                      ///< where band-8 is tracked ACROSS a cycle, else the b_top2 '7'
                      ///< (E5b, λ 9.768 m) rung decides — resolved per CYCLE (λ_top must
                      ///< be uniform across the cycle's arcs: the bridge identity sums ONE
                      ///< λ_top).  The untaken sibling is a CLOSURE gate, never a lane row
                      ///< (§1b — only F−1 independent rungs may re-pin; a dependent third
                      ///< lane row is the F-A refold class): f_E5 = (f_E5a+f_E5b)/2 exactly
                      ///< ⇒ N(7,5) = 2·N(8,5) per coherent arc, so the sibling statistic
                      ///< must sit on fx_integer·λ_top/λ_sib.  Both {b_top, b_top2} are
                      ///< uncarried under the dual tier.
};

/**
 * @brief The per-constellation lane-tier table (M5 card UWL-1, SPEC §3.1).
 *
 * @param cons          Constellation letter ('G', 'E', ...).
 * @param gal_top_band  Galileo top-tier band digit ('7' = E5b λ 9.768 m,
 *                      '8' = E5 AltBOC λ 19.537 m); ignored for other
 *                      constellations.
 * @param gal_mid_rung  Arm the Galileo E6 mid-ladder rung (@ref MwLaneTier::b_mid
 *                      = '6'; @c network.mw_wl.uwl_mid_enable).  Ignored for
 *                      other constellations (GPS has no mid frequency).
 * @param gal_dual_top  Increment 5 (@c network.mw_wl.uwl_dual_top): arm the
 *                      per-cycle preferred/fallback top rung for Galileo —
 *                      @ref MwLaneTier::b_top = '8' (preferred, λ 19.537 m),
 *                      @ref MwLaneTier::b_top2 = '7' (fallback, λ 9.768 m),
 *                      both uncarried, the untaken sibling a closure gate.
 *                      Supersedes @p gal_top_band (ignored).  GPS has no
 *                      sibling pair — always single-top.
 * @return The tier digits, or @c std::nullopt for constellations without an
 *         armed tier (BDS-3 deferred/blocked, GLONASS FDMA-excluded, others
 *         unmodeled).  GPS reproduces the legacy hardcoded digits exactly
 *         (the §6.1 bit-identity requirement).
 */
inline std::optional<MwLaneTier> mwLaneTierOf(char cons, char gal_top_band = '7',
                                              bool gal_mid_rung = false,
                                              bool gal_dual_top = false) {
    switch (cons) {
        case 'G': return MwLaneTier{ '1', '2', '5' };
        case 'E':
            if (gal_dual_top)
                return MwLaneTier{ '1', '5', '8', gal_mid_rung ? '6' : '\0', '7' };
            return MwLaneTier{ '1', '5', gal_top_band,
                               gal_mid_rung ? '6' : '\0' };
        default:  return std::nullopt;   // C deferred (clock divergence), R excluded (FDMA)
    }
}

}  // namespace GnssProcessor
