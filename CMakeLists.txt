cmake_minimum_required(VERSION 3.15)
project(GNSSProcessor CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Python 3.13 paths
set(Python_EXECUTABLE "C:/Python313/python.exe")
set(Python_INCLUDE_DIRS "C:/Python313/include")
set(Python_LIBRARY_DIRS "C:/Python313/libs")
set(Python_LIBRARIES "C:/Python313/libs/python313.lib")
set(Python_ROOT_DIR "C:/Python313")
set(PYBIND11_FINDPYTHON ON)
set(Python_FIND_STRATEGY LOCATION)
set(Python_FIND_REGISTRY NEVER)

set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake")

find_package(Python REQUIRED)
find_package(Eigen3 REQUIRED)
find_package(spdlog REQUIRED)
find_package(pybind11 CONFIG REQUIRED)
find_package(GTest REQUIRED)
find_package(netCDF REQUIRED)
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)

# OpenMP for Eigen matmul / solve parallelism.  Eigen detects the
# _OPENMP macro automatically and threads its dense BLAS-equivalent
# kernels.  Optional — linked only when the toolchain provides it.
find_package(OpenMP)

include_directories(${CMAKE_SOURCE_DIR}/include ${Python_INCLUDE_DIRS})

# Ensure optimized builds also emit debug info/PDBs for profiling
# Put near the top of your top-level CMakeLists.txt
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} /O2 /Zi")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /Zi")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /DEBUG")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /DEBUG")

# Core library
add_library(gnss_core STATIC
    src/core/atmosphere/Troposphere.cpp
    src/core/atmosphere/Ionosphere.cpp
    src/core/config/GnssConfig.cpp
    src/core/config/GnssConfigLoad.cpp
    src/core/logging/LogManager.cpp
    src/core/logging/GatedSink.cpp
    src/core/products/ProductManager.cpp
    src/core/products/ProductParser.cpp
    src/core/products/ProductCache.cpp
    src/core/math/TimeUtils.cpp
    src/core/logging/KalmanFilterLogger.cpp
    src/core/logging/WlsLogger.cpp
	src/core/math/WlsEstimator.cpp
    src/core/common/PppUtils.cpp
	src/core/logging/TraceLogger.cpp
    src/core/corrections/GnssCorrections.cpp
    src/core/corrections/OceanTideModel.cpp
    src/core/filter/KalmanFilter.cpp
    src/core/filter/Smoother.cpp
    src/core/filter/StateManager.cpp
    src/core/filter/ImuIntegrator.cpp
    src/core/filter/AmbiguityResolver.cpp
    src/core/filter/ResidualHistoryTracker.cpp
    src/core/measurement/MeasurementModel.cpp
    src/core/measurement/StateInitializer.cpp
    src/core/measurement/BssdTransform.cpp
    src/core/measurement/BrsdTransform.cpp
    src/core/model/PppModel.cpp
    src/core/model/PppRtkModel.cpp
    src/core/model/StochasticModel.cpp
    src/core/model/Antenna.cpp
    src/core/rinex/RinexParser.cpp
    src/core/rinex/MultiRinexSync.cpp
    src/core/rtcm/RtcmParser.cpp
    src/core/rtcm/RtcmSsrDecoder.cpp
    src/core/rtcm/RtcmSignalMap.cpp
    src/core/rtcm/RtcmMsmDecoder.cpp
    #src/core/sinex/SinexParser.cpp
    #src/core/ssystem/SSystem.cpp
    src/core/model/ModelGeometry.cpp
    src/core/model/Planetary.cpp
    src/core/app/Application.cpp
    src/core/app/ApplicationBaseline.cpp
    src/core/app/ApplicationRealtime.cpp
	src/core/slip/MwSlipDetector.cpp
	#src/core/slip/IfPhaseSlipDetector.cpp
	#src/core/slip/GfSlipDetector.cpp
	src/core/slip/GraphicSlipDetector.cpp
	src/core/slip/CycleSlipManager.cpp
    src/core/products/DataDownloader.cpp
    src/core/products/ProductResolver.cpp
    src/core/products/PathResolver.cpp
    src/core/products/SsrCorrectionStore.cpp
    src/core/realtime/NtripClient.cpp
    src/core/realtime/TcpClient.cpp
    src/core/realtime/RtcmStreamReader.cpp
    src/core/realtime/RealTimeEngine.cpp
    src/core/filter/PivotManager.cpp
    src/core/filter/DdPivotManager.cpp
    src/core/common/SignalBand.cpp
 "src/core/math/Coordinates.cpp" "src/core/math/Interpolate.cpp")
target_include_directories(gnss_core PUBLIC include)
target_link_libraries(gnss_core PUBLIC Eigen3::Eigen spdlog::spdlog netCDF::netcdf CURL::libcurl ZLIB::ZLIB)
if(WIN32)
    # ws2_32 is needed by the raw TCP client (winsock2).  No-op elsewhere.
    target_link_libraries(gnss_core PUBLIC ws2_32)
endif()
if(OpenMP_CXX_FOUND)
    target_link_libraries(gnss_core PUBLIC OpenMP::OpenMP_CXX)
    message(STATUS "OpenMP enabled — Eigen will parallelize dense matmul / solve.")
else()
    message(STATUS "OpenMP not found — Eigen kernels will run single-threaded.")
endif()

# Bake the project root into the binary so PathResolver / GnssConfig
# can default files.internal_data_root to <project_root>/data without
# needing the user to set anything.  Public so dependents (gnss_tests,
# gnss_processor_module) inherit the same default.
target_compile_definitions(gnss_core PUBLIC GNSS_PROJECT_ROOT="${CMAKE_SOURCE_DIR}")

# Main executable (link against gnss_core only)
add_executable(gnss_processor src/main.cpp "src/core/math/Coordinates.cpp" "src/core/math/Interpolate.cpp")
target_link_libraries(gnss_processor PRIVATE gnss_core pybind11::pybind11)

# Standalone RTCM3 file diagnostic tool — decodes a captured dump and
# prints message-type counts, station coords, latest SSR per satellite,
# and MSM7 observation summary.  Useful for validating real Septentrio
# / IGS-RTS captures against the parser before going live.
add_executable(gnss_rtcm_diag src/tools/rtcm_diag.cpp)
target_link_libraries(gnss_rtcm_diag PRIVATE gnss_core)

# Unit tests
enable_testing()
add_executable(gnss_tests
    tests/test_main.cpp
    tests/test_state_key_schema.cpp
    tests/test_transforms.cpp
    tests/test_filter_core.cpp
    tests/test_rtcm.cpp
    "src/core/math/Coordinates.cpp"
    "src/core/math/Interpolate.cpp")
target_link_libraries(gnss_tests PRIVATE GTest::GTest GTest::Main gnss_core)
# Inject the absolute path to the data/ tree so fixture-backed tests can
# locate real product files (SP3, RINEX, etc.) without hard-coded paths.
target_compile_definitions(gnss_tests PRIVATE
    GNSS_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/data")

# Python bindings module (only bindings source; link core)
add_library(gnss_processor_module MODULE
    src/bindings/rinex_binding.cpp
 "src/core/math/Coordinates.cpp" "src/core/math/Interpolate.cpp")
target_include_directories(gnss_processor_module PRIVATE ${CMAKE_SOURCE_DIR}/include ${Python_INCLUDE_DIRS})
target_link_directories(gnss_processor_module PRIVATE ${Python_LIBRARY_DIRS})
target_link_libraries(gnss_processor_module PRIVATE gnss_core pybind11::embed pybind11::module ${Python_LIBRARIES})
set_target_properties(gnss_processor_module PROPERTIES
    PREFIX ""
    SUFFIX ".pyd"
)

add_custom_command(TARGET gnss_processor_module POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:gnss_processor_module>
            ${CMAKE_SOURCE_DIR}/python/gnss_processor.pyd
)

add_custom_command(TARGET gnss_processor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/config
            ${CMAKE_BINARY_DIR}/config
)

# Disable manifests globally for MSVC (Windows 11 fix)
if(MSVC)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} /MANIFEST:NO")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /MANIFEST:NO")
endif()

# For specific targets (e.g., gnss_processor)
target_link_options(gnss_processor PRIVATE /MANIFEST:NO)
target_link_options(gnss_tests PRIVATE /MANIFEST:NO)
target_link_options(gnss_processor_module PRIVATE /MANIFEST:NO)

# Doxygen
find_package(Doxygen)
if(DOXYGEN_FOUND)
    set(DOXYGEN_PROJECT_NAME "GNSS Processor")
    set(DOXYGEN_PROJECT_BRIEF "A GNSS processing application for PPP and PPP-RTK.")
    set(DOXYGEN_GENERATE_HTML YES)
    set(DOXYGEN_GENERATE_LATEX NO)
    set(DOXYGEN_HTML_EXTRA_STYLESHEET
        "C:/temp/doxygen/doxygen-awesome-css/doxygen-awesome.css"
        "C:/temp/doxygen/doxygen-awesome-css/doxygen-awesome-sidebar-only.css"
    )
    doxygen_add_docs(docs
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        COMMENT "Generating Doxygen documentation for GNSS Processor"
    )
endif()
