cmake_minimum_required(VERSION 3.15)
# C is REQUIRED (not optional): the vendored IAU SOFA library (third_party/sofa,
# used by the CIO frame transform — see the sofa target below) is ANSI C and
# will not build without the C language enabled.  Do not revert to "CXX" only.
project(GNSSProcessor CXX C)
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
    src/core/atmosphere/Gpt3.cpp
    src/core/atmosphere/Vmf3.cpp
    src/core/atmosphere/Ionosphere.cpp
    src/core/atmosphere/IonexParser.cpp
    src/core/atmosphere/GlobalIonosphereMap.cpp
    src/core/atmosphere/SphericalHarmonicIonosphere.cpp
    src/core/atmosphere/MagneticField.cpp
    src/core/atmosphere/HigherOrderIonosphere.cpp
    src/core/config/GnssConfig.cpp
    src/core/config/GnssConfigLoad.cpp
    src/core/config/GnssConfigLoadNetwork.cpp
    src/core/logging/LogManager.cpp
    src/core/logging/GatedSink.cpp
    src/core/logging/CsvRotator.cpp
    src/core/logging/NetworkRunTraces.cpp
    src/core/products/ProductManager.cpp
    src/core/products/ProductParser.cpp
    src/core/products/RinexNavParser.cpp
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
    src/core/filter/IlsCore.cpp
    src/core/filter/ResidualHistoryTracker.cpp
    src/core/measurement/MeasurementModel.cpp
    src/core/measurement/MeasurementBuilder.cpp
    src/core/measurement/StateInitializer.cpp
    src/core/measurement/BssdTransform.cpp
    src/core/measurement/BrsdTransform.cpp
    src/core/model/PppModel.cpp
    src/core/model/StochasticModel.cpp
    src/core/model/Antenna.cpp
    src/core/model/SatAttitude.cpp
    src/core/rinex/RinexParser.cpp
    src/core/rinex/MultiRinexSync.cpp
    src/core/rtcm/RtcmParser.cpp
    src/core/rtcm/RtcmSsrDecoder.cpp
    src/core/rtcm/RtcmSignalMap.cpp
    src/core/rtcm/RtcmMsmDecoder.cpp
    src/core/rtcm/RtcmNavDecoder.cpp
    src/core/rtcm/LockTimeTracker.cpp
    src/core/common/CurlGlobalGuard.cpp
    src/core/sbf/SbfSignalMap.cpp
    src/core/sbf/SbfStreamReader.cpp
    src/core/sbf/SbfParser.cpp
    src/core/sbf/Meas3Decoder.cpp
    src/core/sbf/SubframeAssembler.cpp
    src/core/model/ModelGeometry.cpp
    src/core/model/Planetary.cpp
    src/core/app/Application.cpp
    src/core/app/ApplicationBaseline.cpp
    src/core/app/ApplicationRealtime.cpp
    src/core/app/ApplicationNetwork.cpp
    src/core/app/ApplicationNetworkSetup.cpp
    src/core/app/NetworkRunContext.cpp
    src/core/app/EpochUpdateSupport.cpp
	src/core/slip/MwSlipDetector.cpp
	# STAGED (not abandoned) — see header banners in include/slip/.
	#   IfPhaseSlipDetector: DD IF-phase pre-fit residuals; only valid for
	#     static sites with a precise a priori position — the NETWORK producer regime.
	#   GfSlipDetector: 2nd-order time-differenced GF; intended for high-rate
	#     (>=1 Hz) real-time slip *repair* alongside the MW detector, where
	#     ionospheric residual contamination of GF is small enough. Not yet
	#     wired in; the MW detector currently covers slip detection.
	# GfSlipDetector.cpp stays excluded until finished; IfPhaseSlipDetector.cpp is built (network).
	src/core/slip/IfPhaseSlipDetector.cpp
	#src/core/slip/GfSlipDetector.cpp
	src/core/slip/GraphicSlipDetector.cpp
	src/core/slip/EwlSlipDetector.cpp
	src/core/slip/SlipDetectorRegistry.cpp
	src/core/slip/CycleSlipManager.cpp
	src/core/slip/IonoVariabilityMonitor.cpp
	src/core/slip/TurboEditResolver.cpp
	src/core/slip/SoftLliBridge.cpp
	src/core/slip/JumpGapFitter.cpp
	src/core/slip/SlipBridgeKernel.cpp
	src/core/slip/NetworkSlipQc.cpp
    src/core/aiding/AidingSourceRegistry.cpp
    src/core/aiding/ImuAidingSource.cpp
    src/core/aiding/ZuptAidingSource.cpp
    src/core/aiding/AidingSourceManager.cpp
    src/core/products/DataDownloader.cpp
    src/core/products/ProductResolver.cpp
    src/core/products/PathResolver.cpp
    src/core/products/StationInfoFile.cpp
    src/core/products/SsrCorrectionStore.cpp
    src/core/products/NetworkProductEmitter.cpp
    src/core/products/NetworkEmitRebase.cpp
    src/core/products/NetworkPbEmitFilter.cpp
    src/core/products/NetworkProductIO.cpp
    src/core/products/NetworkStateSnapshot.cpp
    src/core/products/LiveEphemerisStore.cpp
    src/core/realtime/NtripClient.cpp
    src/core/realtime/TcpClient.cpp
    src/core/realtime/RtcmStreamReader.cpp
    src/core/realtime/RealTimeEngine.cpp
    src/core/filter/PivotManager.cpp
    src/core/filter/DdPivotManager.cpp
    src/core/filter/CovarianceReReference.cpp
    src/core/filter/NetworkNormalEquations.cpp
    src/core/filter/NetworkEstimator.cpp
    src/core/filter/NetworkInvariantCarryDriver.cpp
    src/core/filter/NetworkMwWlLayer.cpp
    src/core/filter/NetworkSbasis.cpp
    src/core/filter/NeqAccumulator.cpp
    src/core/common/SignalBand.cpp
 "src/core/math/Coordinates.cpp" "src/core/math/Interpolate.cpp")
target_include_directories(gnss_core PUBLIC include)
# KalmanFilter.cpp emits more COFF sections than the default object format
# allows (template-heavy Eigen kernels); /bigobj raises the limit.  Side-effect
# free build flag (MSVC only).  ApplicationNetwork.cpp is the large network-driver
# TU and likewise crossed the section limit as it grew.
if(MSVC)
    set_source_files_properties(src/core/filter/KalmanFilter.cpp
        src/core/app/ApplicationNetwork.cpp
        PROPERTIES COMPILE_FLAGS "/bigobj")
endif()
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

# ---------------------------------------------------------------------------
# IAU SOFA (vendored, third_party/sofa) — standard ANSI-C astronomy library
# providing the CIO-based IAU 2000A/2006 celestial-to-terrestrial frame
# transform used by Planetary::celestialToTerrestrial (gated behind the
# estimation.enable_cio_frame flag).  This is the ONE sanctioned glob in this
# build: SOFA ships ~240 tiny .c files and gnss_core's own list stays explicit.
# Vendored verbatim so the build is self-contained (see third_party/sofa/README.md).
# Built as a separate static lib with warnings suppressed (third-party C).
# ---------------------------------------------------------------------------
file(GLOB SOFA_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sofa/*.c")
add_library(sofa STATIC ${SOFA_SOURCES})
target_include_directories(sofa PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sofa")
if(MSVC)
    target_compile_options(sofa PRIVATE /w)
else()
    target_compile_options(sofa PRIVATE -w)
endif()
target_link_libraries(gnss_core PUBLIC sofa)

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

# SBF capture inspector — block histogram + decoded MeasEpoch observation dump.
add_executable(gnss_sbf_diag src/tools/sbf_diag.cpp)
target_link_libraries(gnss_sbf_diag PRIVATE gnss_core)

# Meas3Ranges (4109) vs MeasEpoch (4027) cross-check on a dual-logged capture.
add_executable(gnss_meas3_xcheck src/tools/meas3_xcheck.cpp)
target_link_libraries(gnss_meas3_xcheck PRIVATE gnss_core)

# ATX coverage inspector — dump parsed PCO/PCV freq keys for a type pattern.
add_executable(gnss_atx_diag src/tools/atx_diag.cpp)
target_link_libraries(gnss_atx_diag PRIVATE gnss_core)

# GNSS exam dataset simulator — synthesizes a multi-GNSS RINEX-3.04 observation
# file (+ truth answer key, in later milestones) for the GNSS/Geodesy qualifying
# exam, then verifies the processor recovers the injected truth.  Links
# gnss_core only; coupling to the verifier is out-of-process (writes RINEX, runs
# gnss_processor, diffs traces).  See C:/GnssSimulator/docs/DESIGN.md.
add_executable(gnss_simulator
    src/sim/main.cpp
    src/sim/SimConfig.cpp
    src/sim/RinexObsWriter.cpp
    src/sim/EphemerisProvider.cpp
    src/sim/TrajectoryGenerator.cpp
    src/sim/IonoDisturbance.cpp
    src/sim/Verifier.cpp)
target_link_libraries(gnss_simulator PRIVATE gnss_core)

# Unit tests
enable_testing()
add_executable(gnss_tests
    tests/test_main.cpp
    tests/test_state_key_schema.cpp
    tests/test_covariance_rereference.cpp
    tests/test_network_emit.cpp
    tests/test_sh_ionosphere.cpp
    tests/test_network_product_io.cpp
    tests/test_network_combined_sigma.cpp
    tests/test_rinex_nav_parser.cpp
    tests/test_network_normal_equations.cpp
    tests/test_network_estimator.cpp
    tests/test_network_carry_vce.cpp
    tests/test_network_iono_deweight_gate.cpp
    tests/test_network_zstore.cpp
    tests/test_network_robust_stats.cpp
    tests/test_network_invariant_carry_driver.cpp
    tests/test_network_mw_wl_layer.cpp
    tests/test_mw_bridge.cpp
    tests/test_card4_emit_rebase.cpp
    tests/test_pb_emit_filter.cpp
    tests/test_eq34_consumer_integer.cpp
    tests/test_glonass_network_config.cpp
    tests/test_codebias_network_config.cpp
    tests/test_ssr_store_injection.cpp
    tests/test_realtime_seams.cpp
    tests/test_transforms.cpp
    tests/test_filter_core.cpp
    tests/test_rtcm.cpp
    tests/test_sbf.cpp
    tests/test_estimation_core.cpp
    tests/test_ils_core.cpp
    tests/test_signal_math.cpp
    tests/test_geometry.cpp
    tests/test_stochastic_model.cpp
    tests/test_turbo_edit_resolver.cpp
    tests/test_soft_lli_bridge.cpp
    tests/test_aiding_source.cpp
    tests/test_smoother_frame.cpp
    tests/test_jump_gap_fitter.cpp
    tests/test_slip_bridge_kernel.cpp
    tests/test_planetary_cio.cpp
    tests/test_orbit_enhancements.cpp
    tests/test_neq_accumulator.cpp
    tests/test_station_info.cpp
    tests/test_antenna_radome_fallback.cpp
    "src/core/math/Coordinates.cpp"
    "src/core/math/Interpolate.cpp")
target_link_libraries(gnss_tests PRIVATE GTest::GTest GTest::Main gnss_core)
# Inject the absolute path to the data/ tree so fixture-backed tests can
# locate real product files (SP3, RINEX, etc.) without hard-coded paths.
target_compile_definitions(gnss_tests PRIVATE
    GNSS_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/data")

# gnss_simulator self-consistency battery (DESIGN §10.6, product-/processor-free
# subset; the end-to-end loop is exercised by harness/gnss_verify_loop.ps1).
# Compiles the standalone sim sources it needs directly (they are not in gnss_core).
add_executable(gnss_simulator_selftest
    tests/sim/test_sim_selfconsistency.cpp
    src/sim/RinexObsWriter.cpp
    src/sim/IonoDisturbance.cpp
    src/sim/Verifier.cpp
    "src/core/math/Coordinates.cpp"
    "src/core/math/Interpolate.cpp")
target_link_libraries(gnss_simulator_selftest PRIVATE GTest::GTest GTest::Main gnss_core)
add_test(NAME gnss_simulator_selftest COMMAND gnss_simulator_selftest)

# Python bindings module (only bindings source; link core)
add_library(gnss_processor_module MODULE
    src/bindings/gnss_module.cpp
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
#   Request the optional `dot` component so doxygen_add_docs() can wire up
#   Graphviz-rendered structure graphs when Graphviz is installed.
find_package(Doxygen OPTIONAL_COMPONENTS dot)
if(DOXYGEN_FOUND)
    set(DOXYGEN_PROJECT_NAME "GNSS Processor")
    set(DOXYGEN_PROJECT_BRIEF "A GNSS processing application for PPP and PPP-RTK.")
    set(DOXYGEN_GENERATE_HTML YES)
    set(DOXYGEN_GENERATE_LATEX NO)
    set(DOXYGEN_HTML_EXTRA_STYLESHEET
        "C:/temp/doxygen/doxygen-awesome-css/doxygen-awesome.css"
        "C:/temp/doxygen/doxygen-awesome-css/doxygen-awesome-sidebar-only.css"
    )

    # --- Tier 1: auto-generated structure graphs (Graphviz / dot) ------------
    # These are *structure* graphs (class collaboration, include/dir trees,
    # inheritance) — a navigation aid, NOT the curated *process* diagrams that
    # live in docs/report.  They require Graphviz `dot`; if it is absent we log
    # a hint and ship the docs without graphs rather than emit broken output.
    # The verbose per-function call/caller graphs are opt-in: they are accurate
    # but turn into hairballs on a state-heavy estimator.
    option(GNSS_DOXYGEN_CALL_GRAPHS
           "Add per-function call/caller graphs to Doxygen output (slow, verbose)" OFF)

    if(DOXYGEN_DOT_FOUND)
        set(DOXYGEN_HAVE_DOT            YES)
        set(DOXYGEN_DOT_IMAGE_FORMAT    svg)   # zoomable, crisp at any scale
        set(DOXYGEN_INTERACTIVE_SVG     YES)
        set(DOXYGEN_DOT_MULTI_TARGETS   YES)
        set(DOXYGEN_DOT_GRAPH_MAX_NODES 100)   # default 50 truncates our graphs
        set(DOXYGEN_MAX_DOT_GRAPH_DEPTH 0)     # 0 = unlimited depth

        # Structural graphs — cheap, high value for onboarding/navigation.
        set(DOXYGEN_CLASS_GRAPH         YES)
        set(DOXYGEN_COLLABORATION_GRAPH YES)
        set(DOXYGEN_GROUP_GRAPHS        YES)
        set(DOXYGEN_INCLUDE_GRAPH       YES)
        set(DOXYGEN_INCLUDED_BY_GRAPH   YES)
        set(DOXYGEN_GRAPHICAL_HIERARCHY YES)
        set(DOXYGEN_DIRECTORY_GRAPH     YES)

        # Call / caller graphs — opt in with -DGNSS_DOXYGEN_CALL_GRAPHS=ON.
        if(GNSS_DOXYGEN_CALL_GRAPHS)
            set(DOXYGEN_CALL_GRAPH   YES)
            set(DOXYGEN_CALLER_GRAPH YES)
        endif()

        message(STATUS
            "Doxygen: Graphviz dot found (${DOXYGEN_DOT_EXECUTABLE}); structure graphs enabled.")
    else()
        message(STATUS
            "Doxygen: Graphviz 'dot' not found -- structure graphs disabled. "
            "Install Graphviz (https://graphviz.org/download/), ensure dot is on "
            "PATH, and re-run CMake to enable class/include/collaboration diagrams.")
    endif()

    doxygen_add_docs(docs
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        COMMENT "Generating Doxygen documentation for GNSS Processor"
    )
endif()
