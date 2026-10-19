#pragma once

/**
 * @file ReceiverBias.hpp
 * @brief Which receiver code-bias parameter a code signal maps to, and with which
 *        coefficient: the resolver of the observation equation's receiver code-bias term and
 *        of the receiver code-bias seed.
 *
 * The rule reads the configuration, the receiver's per-band latches in the model and the
 * @c .BIA product's clock-reference observables, and nothing that changes from epoch to
 * epoch.  It is a class of its own because the single-receiver filter and the network
 * producer answer the same question the same way: a code signal on the clock-reference
 * pair resolves to the system's differential code bias with a geometry-free coefficient, a
 * band outside the pair to an inter-frequency bias, a second code of the first band to an
 * inter-signal correction, and a GLONASS FDMA band to a per-slot bias.
 *
 * It also holds the carrier-frequency helper the seeds scale the slant ionosphere by.  The
 * helper's GLONASS channels are the default table, never the receiver's (nothing seeds
 * them), which is the number the seeds have always used.
 */

#include "common/ParamRef.hpp"
#include "common/PppTypes.hpp"
#include "common/PppUtils.hpp"
#include "products/ProductTypes.hpp"   // ClockRefMap

#include <string>

namespace GnssProcessor {

class GnssConfig;
class LogManager;
class PppModel;

/**
 * @brief The receiver code-bias parameter of one code signal.
 */
struct ReceiverBiasEntry {
    std::string state_key;   /**< Text key of the parameter (e.g. "GPS_DCB" or "GPS_IFB5"); empty when the signal has none. */
    double      coefficient; /**< Coefficient of the parameter in the row.  NaN means no bias is modelled. */
    RxCbKey     key{};       /**< The same parameter as a typed key (valid when @ref has_key). */
    bool        has_key{false}; /**< True exactly when @ref state_key is non-empty. */
};

/**
 * @brief The receiver code-bias resolver.
 *
 * Holds two non-owning references and a frequency helper, and no state that changes: it may
 * be shared by every caller of one receiver.  The configuration and the model must outlive
 * it.
 */
class ReceiverBiasResolver {
public:
    /**
     * @brief Builds the resolver of one receiver.
     * @param config      Processing configuration.
     * @param ppp_model   The receiver's model (its band latches and GLONASS channels).
     * @param log_manager Logging, for the frequency helper.
     */
    ReceiverBiasResolver(const GnssConfig& config, const PppModel& ppp_model,
                         LogManager& log_manager);

    /**
     * @brief The receiver code-bias parameter and coefficient for a code signal.
     *
     * @param obs_signal  The code signal ("C1W").
     * @param system      Its constellation.
     * @param satellite   The satellite (selects the GLONASS channel).
     * @param clock_refs  The .BIA product's satellite clock-reference observables per
     *                    constellation letter: the bands the receiver DCB is referenced to.
     *                    A constellation without an entry has no DCB or IFB key.
     * @return The key and coefficient; a NaN coefficient and no key when the signal has no
     *         receiver code-bias parameter.
     */
    ReceiverBiasEntry resolveReceiverBias(const std::string& obs_signal, System system,
                                          const std::string& satellite,
                                          const ClockRefMap& clock_refs) const;

    /**
     * @brief The frequency helper the seeds scale the slant ionosphere by.
     *
     * Its GLONASS channels are the default table, never the receiver's; the receiver's own
     * helper (seeded from the RINEX header) belongs to the observation equation's row.
     *
     * @return The helper.
     */
    const PppUtils& frequencyHelper() const { return ppp_utils_; }

    /**
     * @brief The single-letter key of the .BIA product's clock-reference table for a
     *        constellation.
     * @param system The constellation.
     * @return The RINEX letter; the NUL character for an unknown constellation.
     */
    static char systemToClockRefChar(System system);

private:
    const GnssConfig* config_;
    const PppModel*   ppp_model_;
    PppUtils          ppp_utils_;
};

} // namespace GnssProcessor
