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

include_directories(${CMAKE_SOURCE_DIR}/include ${Python_INCLUDE_DIRS})

# Core library
add_library(gnss_core STATIC
    src/core/common/CycleSlipDetector.cpp
	src/core/common/GnssConfig.cpp
	src/core/common/LogManager.cpp
	src/core/common/ProductManager.cpp
	src/core/common/ProductParser.cpp
	src/core/common/TimeUtils.cpp
	src/core/common/KalmanFilterLogger.cpp
	src/core/common/WlsLogger.cpp
	src/core/corrections/GnssCorrections.cpp
	src/core/filter/KalmanFilter.cpp
	src/core/filter/StateManager.cpp
	src/core/measurement/MeasurementModel.cpp
	src/core/model/PppModel.cpp
	src/core/model/PppRtkModel.cpp
	src/core/rinex/RinexParser.cpp
	src/core/rtcm/RtcmParser.cpp
	src/core/sinex/SinexParser.cpp
	src/core/ssystem/SSystem.cpp)
target_include_directories(gnss_core PUBLIC include)
target_link_libraries(gnss_core PUBLIC Eigen3::Eigen spdlog::spdlog netCDF::netcdf)

# Main executable
set(SOURCES
    src/main.cpp
)
add_executable(gnss_processor ${SOURCES} "src/core/common/GnssConfig.cpp")
target_link_libraries(gnss_processor PRIVATE gnss_core pybind11::pybind11)

# Unit tests
enable_testing()
set(TEST_SOURCES
    tests/test_main.cpp
)
add_executable(gnss_tests ${TEST_SOURCES} "src/core/common/GnssConfig.cpp")
target_link_libraries(gnss_tests PRIVATE GTest::GTest GTest::Main gnss_core)

# Python bindings
add_library(gnss_processor_module MODULE
    src/bindings/rinex_binding.cpp
	#src/core/common/TimeUtils.cpp
 "src/core/common/GnssConfig.cpp")
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

add_custom_command(TARGET gnss_processor_module POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/python/config
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_SOURCE_DIR}/src/core/config/gnss_config.json
            ${CMAKE_SOURCE_DIR}/python/config/gnss_config.json
)