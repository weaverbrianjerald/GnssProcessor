/**
 * @file NetworkRunEnd.cpp
 * @brief The run-end table, kNetworkRunEndReports: every run-end report of the network
 *        producer in print order, with what it reports and its home file (card NOM step
 *        TD1b).  filter/NetworkRunEnd.hpp states how to add a row and the order constraints.
 *
 * The table is empty until the run end is cut into its reports.
 */

#include "filter/NetworkRunEnd.hpp"

namespace GnssProcessor {

const std::array<NetworkRunEndRow, kNetworkRunEndReportCount> kNetworkRunEndReports = {};

} // namespace GnssProcessor
