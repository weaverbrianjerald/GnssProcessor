/**
 * @file gnss_module.cpp
 * @brief pybind11 entry point for the `gnss_processor` Python module.
 *
 * Built by the `gnss_processor_module` CMake target and copied to
 * python/gnss_processor.pyd on every build (see CMakeLists.txt POST_BUILD).
 * The module name in PYBIND11_MODULE has to remain `gnss_processor`: Python
 * locates the init symbol (PyInit_gnss_processor) by the .pyd filename.
 *
 * The surface is deliberately minimal and grows as needs appear:
 *   - gnss_processor.__version__
 *   - gnss_processor.Llh                       geodetic lat/lon/height container
 *   - ecef_to_llh / llh_to_ecef                WGS84 geodetic <-> ECEF
 *   - ecef_to_enu / enu_to_ecef / enu_to_ecef_matrix   local topocentric ENU
 *
 * Pure, stateless helpers (such as Coordinates) bind directly.  Stateful
 * classes need more work: RinexParser's constructor takes (const GnssConfig&,
 * LogManager&, ProductParser&), so those types need bindings, or a convenience
 * factory is required, before RinexParser can be exposed here.
 */

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/eigen.h>

#include <string>

#include "math/Coordinates.hpp"
#include "common/Constants.hpp"

namespace py = pybind11;
namespace Coord = GnssProcessor::Coordinates;
using GnssProcessor::Coordinates::Llh;

namespace {
constexpr double kWgs84A = GnssProcessor::Constants::Ellipsoid::WGS84::a;
constexpr double kWgs84F = GnssProcessor::Constants::Ellipsoid::WGS84::f;
}  // namespace

PYBIND11_MODULE(gnss_processor, m) {
    m.doc() =
        "GnssProcessor — minimal Python bindings.\n\n"
        "Currently exposes WGS84 geodetic coordinate conversions. All angles\n"
        "are radians and all lengths meters. ECEF vectors are accepted as and\n"
        "returned as length-3 NumPy arrays.";
    m.attr("__version__") = "0.1.0";

    py::class_<Llh>(m, "Llh",
                    "Geodetic latitude/longitude/height (radians, radians, meters).")
        .def(py::init<>())
        .def(py::init([](double lat_rad, double lon_rad, double h_m) {
                 return Llh{lat_rad, lon_rad, h_m};
             }),
             py::arg("lat_rad"), py::arg("lon_rad"), py::arg("h_m"))
        .def_readwrite("lat_rad", &Llh::lat_rad)
        .def_readwrite("lon_rad", &Llh::lon_rad)
        .def_readwrite("h_m", &Llh::h_m)
        .def("__repr__", [](const Llh& p) {
            return "Llh(lat_rad=" + std::to_string(p.lat_rad) +
                   ", lon_rad=" + std::to_string(p.lon_rad) +
                   ", h_m=" + std::to_string(p.h_m) + ")";
        });

    m.def("ecef_to_llh", &Coord::ecefToLlh,
          py::arg("ecef"), py::arg("a") = kWgs84A, py::arg("f") = kWgs84F,
          "Convert an ECEF [x, y, z] position (meters) to geodetic Llh "
          "(Bowring iteration; defaults to the WGS84 ellipsoid).");

    m.def("llh_to_ecef", &Coord::llhToEcef,
          py::arg("lat_rad"), py::arg("lon_rad"), py::arg("h_m"),
          py::arg("a") = kWgs84A, py::arg("f") = kWgs84F,
          "Convert geodetic (lat_rad, lon_rad, h_m) to an ECEF [x, y, z] "
          "vector (meters).");

    m.def("enu_to_ecef_matrix", &Coord::enuToEcefMatrix,
          py::arg("lat_rad"), py::arg("lon_rad"),
          "3x3 rotation mapping local ENU -> ECEF at the given geodetic "
          "lat/lon (columns are the E, N, U unit vectors in ECEF).");

    m.def("ecef_to_enu", &Coord::ecefToEnu,
          py::arg("vec_ecef"), py::arg("ref_llh"),
          "Convert an ECEF vector (e.g. sat - ref, meters) to local ENU at "
          "the reference location ref_llh.");

    m.def("enu_to_ecef", &Coord::enuToEcef,
          py::arg("enu"), py::arg("ref_llh"),
          "Convert a local ENU vector (meters) at ref_llh to an ECEF vector.");
}
