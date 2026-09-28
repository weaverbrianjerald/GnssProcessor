#pragma once

/**
 * @file PppTypes.hpp
 * @brief Umbrella header that includes the focused PPP type headers, kept for
 *        backward compatibility.
 *
 * The PPP types (constellation enums, the @c PppObservation / @c PppState /
 * @c EpochMeasurement structs, the product-data PODs, the @c ClockRefSignals
 * pair and the @c PppUtils class) live in focused headers, so a consumer
 * need not pull in Eigen, the LogManager and several STL containers at once:
 *   - @c common/GnssTypes.hpp:        System enum, MeasurementType enum, classifyMeasurement()
 *   - @c common/PppObservation.hpp:   PppObservation struct
 *   - @c common/PppState.hpp:         PppState struct
 *   - @c common/EpochMeasurement.hpp: EpochMeasurement struct
 *   - @c products/ProductTypes.hpp:   OrbitData, ClockData, AntennaData, BiasData,
 *                                     ClockRefSignals, ClockRefMap, ErpData,
 *                                     OceanTideData
 *   - @c common/PppUtils.hpp:         PppUtils class
 *
 * New code includes the focused header directly.  This umbrella keeps
 * existing consumers compiling unchanged; migrating a call site to the
 * narrower includes shortens its compile time and states its dependencies.
 */

#include "common/GnssTypes.hpp"
#include "common/PppObservation.hpp"
#include "common/PppState.hpp"
#include "common/EpochMeasurement.hpp"
#include "products/ProductTypes.hpp"
#include "common/PppUtils.hpp"
