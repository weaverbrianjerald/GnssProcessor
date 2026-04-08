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
    
    src/core/common/GnssConfig.cpp
    src/core/common/LogManager.cpp
    src/core/common/ProductManager.cpp
    src/core/common/ProductParser.cpp
    src/core/common/ProductCache.cpp
    src/core/common/TimeUtils.cpp
    src/core/common/KalmanFilterLogger.cpp
    src/core/common/WlsLogger.cpp
	src/core/common/WlsEstimator.cpp
    src/core/common/PppTypes.cpp
	src/core/common/TraceLogger.cpp
    src/core/corrections/GnssCorrections.cpp
    src/core/corrections/OceanTideModel.cpp
    src/core/filter/KalmanFilter.cpp
    src/core/filter/StateManager.cpp
    src/core/filter/ImuIntegrator.cpp
    src/core/filter/AmbiguityResolver.cpp
    src/core/measurement/MeasurementModel.cpp
    src/core/model/PppModel.cpp
    src/core/model/PppRtkModel.cpp
    src/core/model/StochasticModel.cpp
    src/core/model/Antenna.cpp
    src/core/rinex/RinexParser.cpp
    src/core/rtcm/RtcmParser.cpp
    src/core/sinex/SinexParser.cpp
    src/core/ssystem/SSystem.cpp
    src/core/model/ModelGeometry.cpp
    src/core/model/Planetary.cpp
    src/core/common/Application.cpp
	src/core/common/MwSlipDetector.cpp
	src/core/common/IfPhaseSlipDetector.cpp
    src/core/common/DataDownloader.cpp
    src/core/common/ProductResolver.cpp
    src/core/realtime/NtripClient.cpp
    src/core/realtime/RtcmStreamReader.cpp
    src/core/realtime/RealTimeEngine.cpp
 "src/core/common/Coordinates.cpp" "src/core/common/Interpolate.cpp")
target_include_directories(gnss_core PUBLIC include)
target_link_libraries(gnss_core PUBLIC Eigen3::Eigen spdlog::spdlog netCDF::netcdf CURL::libcurl ZLIB::ZLIB)

# Main executable (link against gnss_core only)
add_executable(gnss_processor src/main.cpp "src/core/common/Coordinates.cpp" "src/core/common/Interpolate.cpp")
target_link_libraries(gnss_processor PRIVATE gnss_core pybind11::pybind11)

# Unit tests
enable_testing()
add_executable(gnss_tests tests/test_main.cpp "src/core/common/Coordinates.cpp" "src/core/common/Interpolate.cpp")
target_link_libraries(gnss_tests PRIVATE GTest::GTest GTest::Main gnss_core)

# Python bindings module (only bindings source; link core)
add_library(gnss_processor_module MODULE
    src/bindings/rinex_binding.cpp
 "src/core/common/Coordinates.cpp" "src/core/common/Interpolate.cpp")
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
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/config
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_SOURCE_DIR}/src/core/config/gnss_config.json
            ${CMAKE_BINARY_DIR}/config/gnss_config.json
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
        COMMENT "Generate HTML documentation"
    )
else()
    message(WARNING "Doxygen not found. Documentation will not be generated.")
endif()