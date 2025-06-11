cmake_minimum_required(VERSION 3.15)
project(GNSSProcessor CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake")
set(PYBIND11_FINDPYTHON ON)

find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
find_package(Eigen3 REQUIRED)
find_package(spdlog REQUIRED)
find_package(pybind11 REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/include)

set(SOURCES
    ${CMAKE_SOURCE_DIR}/src/main.cpp
)
add_executable(gnss_processor ${SOURCES})
target_link_libraries(gnss_processor PRIVATE Eigen3::Eigen spdlog::spdlog pybind11::pybind11)

enable_testing()
find_package(GTest REQUIRED)
set(TEST_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/test_main.cpp
)
add_executable(gnss_tests ${TEST_SOURCES})
target_link_libraries(gnss_tests PRIVATE GTest::GTest GTest::Main Eigen3::Eigen spdlog::spdlog)

pybind11_add_module(example_module ${CMAKE_SOURCE_DIR}/python/example.cpp)
target_include_directories(example_module PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(example_module PRIVATE pybind11::pybind11 Python::Module)

add_custom_command(TARGET example_module POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:example_module>
            ${CMAKE_SOURCE_DIR}/python/example_module.pyd
)