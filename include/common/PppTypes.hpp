#pragma once

/**
 * @file PppTypes.hpp
 * @brief Umbrella header — kept for backward compatibility while the
 *        PPP types are split into focused headers.
 *
 * Historical note: this header was a grab-bag containing constellation
 * enums, the @c PppObservation / @c PppState / @c EpochMeasurement
 * structs, every product-data POD (orbit / clock / antenna / bias /
 * ERP / ocean tide / Sun ephemeris), the @c ClockRefSignals pair, and
 * the @c PppUtils class.  Pulling in @c PppTypes.hpp dragged Eigen,
 * the LogManager, and a half-dozen STL containers into every consumer.
 *
 * The split distributes those types into:
 *   - @c common/GnssTypes.hpp        — System enum, MeasurementType enum, classifyMeasurement()
 *   - @c common/PppObservation.hpp   — PppObservation struct
 *   - @c common/PppState.hpp         — PppState struct
 *   - @c common/EpochMeasurement.hpp — EpochMeasurement struct
 *   - @c common/ProductTypes.hpp     — OrbitData, ClockData, AntennaData, BiasData,
 *                                      ClockRefSignals, ClockRefMap, ErpData,
 *                                      OceanTideData
 *   - @c common/PppUtils.hpp         — PppUtils class
 *
 * New code should @c #include the focused header directly.  This
 * umbrella keeps existing consumers compiling unchanged; over time
 * call sites should migrate to the narrower includes for faster
 * translation-unit compilation and clearer dependency intent.
 */

#include "common/GnssTypes.hpp"
#include "common/PppObservation.hpp"
#include "common/PppState.hpp"
#include "common/EpochMeasurement.hpp"
#include "products/ProductTypes.hpp"
#include "common/PppUtils.hpp"
