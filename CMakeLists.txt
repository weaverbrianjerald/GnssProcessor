cmake_minimum_required(VERSION 3.15)
# vcpkg stays in CLASSIC mode (packages under C:/vcpkg/installed, shared by both
# build trees and the IDE).  The repo-root vcpkg.json only pins the versions for the
# one-time per-machine install (docs/onboarding/01_setup.md); without this line its
# mere presence would flip the toolchain to manifest mode and rebuild every
# dependency inside each build tree.  Must precede project().
set(VCPKG_MANIFEST_MODE OFF CACHE BOOL "vcpkg classic mode; vcpkg.json is an install list only")
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
    src/core/config/GnssConfigNetworkPosture.cpp
    src/core/logging/LogManager.cpp
    src/core/logging/GatedSink.cpp
    src/core/logging/CsvRotator.cpp
    src/core/network/diag/RunTraces.cpp
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
    src/core/corrections/WindUpGapBridge.cpp
    src/core/filter/KalmanFilter.cpp
    src/core/filter/KalmanKernels.cpp
    src/core/filter/KalmanResidualQc.cpp
    src/core/filter/KalmanPostFitQc.cpp
    src/core/filter/KalmanOutlierResponse.cpp
    src/core/filter/Smoother.cpp
    src/core/filter/StateManager.cpp
    src/core/filter/StateManagerProcessNoise.cpp
    src/core/filter/StateManagerLayout.cpp
    src/core/filter/StateManagerFamilies.cpp
    src/core/filter/ImuIntegrator.cpp
    src/core/filter/AmbiguityResolver.cpp
    src/core/filter/IlsCore.cpp
    src/core/filter/ResidualHistoryTracker.cpp
    src/core/measurement/MeasurementModel.cpp
    src/core/measurement/MeasurementModelStates.cpp
    src/core/measurement/MeasurementModelRow.cpp
    src/core/measurement/MeasurementModelVariance.cpp
    src/core/measurement/MeasurementModelDd.cpp
    src/core/measurement/MeasurementModelNetwork.cpp
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
    src/core/app/PppSetup.cpp
    src/core/app/PppEpochSteps.cpp
    src/core/app/PppEpochUpdate.cpp
    src/core/app/PppEpochRecords.cpp
    src/core/app/PppRunEnd.cpp
    src/core/app/ApplicationBaseline.cpp
    src/core/app/ApplicationRealtime.cpp
    src/core/network/driver/ApplicationNetwork.cpp
    src/core/network/driver/ApplicationNetworkEpoch.cpp
    src/core/network/driver/Run.cpp
    src/core/network/driver/EpochAdmission.cpp
    src/core/network/driver/ApplicationNetworkSetup.cpp
    src/core/app/ApplicationConsumerReplay.cpp
    src/core/network/driver/ApplicationNetworkAssembly.cpp
    src/core/network/driver/Knobs.cpp
    src/core/network/state/SnapshotFingerprint.cpp
    src/core/network/state/WarmResume.cpp
    src/core/network/state/Entities.cpp
    src/core/network/state/RunFields.cpp
    src/core/network/state/StateSnapshotWrite.cpp
    src/core/network/state/BorderPrior.cpp
    src/core/network/state/BorderStash.cpp
    src/core/network/carry/LocalCarryHarvest.cpp
    src/core/network/assembly/SignalRoster.cpp
    src/core/network/assembly/SignalRosterSites.cpp
    src/core/app/EpochUpdateSupport.cpp
	src/core/slip/MwSlipDetector.cpp
	src/core/slip/MwSlipDetectorEpoch.cpp
	src/core/slip/MwSlipDetectorSample.cpp
	src/core/slip/MwSlipDetectorPairs.cpp
	src/core/slip/MwSlipDetectorStats.cpp
	src/core/slip/MwSlipDetectorQuery.cpp
	src/core/slip/MwSlipDetectorReset.cpp
	src/core/slip/MwSlipDetectorTrace.cpp
	src/core/slip/MwSlipDetectorZd.cpp
	# STAGED (not abandoned) — see header banners in include/slip/.
	#   IfPhaseSlipDetector: DD IF-phase pre-fit residuals; only valid for
	#     static sites with a precise a priori position — the NETWORK producer regime.
	#   GfSlipDetector: 2nd-order time-differenced GF; intended for high-rate
	#     (>=1 Hz) real-time slip *repair* alongside the MW detector, where
	#     ionospheric residual contamination of GF is small enough. Not yet
	#     wired in; the MW detector currently covers slip detection.
	# GfSlipDetector.cpp stays excluded until finished; IfPhaseSlipDetector.cpp is built (network).
	src/core/slip/IfPhaseSlipDetector.cpp
	src/core/slip/IfPhaseStagedUserPath.cpp
	#src/core/slip/GfSlipDetector.cpp
	src/core/slip/GraphicSlipDetector.cpp
	src/core/slip/EwlSlipDetector.cpp
	src/core/slip/SlipDetectorRegistry.cpp
	src/core/slip/CycleSlipManager.cpp
	src/core/slip/CycleSlipDetection.cpp
	src/core/slip/CycleSlipAdjudication.cpp
	src/core/slip/CycleSlipPivot.cpp
	src/core/slip/CycleSlipNonMwPasses.cpp
	src/core/slip/CycleSlipRepair.cpp
	src/core/slip/CycleSlipCounters.cpp
	src/core/slip/IonoVariabilityMonitor.cpp
	src/core/slip/TurboEditResolver.cpp
	src/core/slip/TurboEditChain.cpp
	src/core/slip/TurboEditAdjudication.cpp
	src/core/slip/TurboEditBridge.cpp
	src/core/slip/SoftLliBridge.cpp
	src/core/slip/JumpGapFitter.cpp
	src/core/slip/SlipBridgeKernel.cpp
	src/core/network/slip/RawObservables.cpp
	src/core/network/slip/SlipQc.cpp
	src/core/network/slip/GapResumeCensus.cpp
	src/core/network/slip/SlipOracle.cpp
	src/core/network/slip/CommonBandScope.cpp
	src/core/network/slip/GfArcJump.cpp
	src/core/network/slip/PassASlipScan.cpp
	src/core/network/slip/PassASlipDrains.cpp
    src/core/aiding/AidingSourceRegistry.cpp
    src/core/aiding/ImuAidingSource.cpp
    src/core/aiding/ZuptAidingSource.cpp
    src/core/aiding/AidingSourceManager.cpp
    src/core/products/DataDownloader.cpp
    src/core/products/ProductResolver.cpp
    src/core/products/PathResolver.cpp
    src/core/products/StationInfoFile.cpp
    src/core/products/SsrCorrectionStore.cpp
    src/core/products/SignalDeclaration.cpp
    src/core/products/NetworkProductEmitter.cpp
    src/core/network/emit/EmitRebase.cpp
    src/core/network/emit/ClockEmitRebase.cpp
    src/core/products/NetworkProductIO.cpp
    src/core/products/NetworkStateSnapshot.cpp
    src/core/products/LiveEphemerisStore.cpp
    src/core/products/BrdcClockApriori.cpp
    src/core/realtime/NtripClient.cpp
    src/core/realtime/TcpClient.cpp
    src/core/realtime/RtcmStreamReader.cpp
    src/core/network/driver/StreamAligner.cpp
    src/core/network/driver/StreamFrontEnd.cpp
    src/core/realtime/RealTimeEngine.cpp
    src/core/realtime/RealTimeEngineAdmit.cpp
    src/core/realtime/RealTimeEngineSsrDiag.cpp
    src/core/realtime/RealTimeEnginePrepare.cpp
    src/core/realtime/RealTimeEngineSlipQc.cpp
    src/core/realtime/RealTimeEngineSsrBreaks.cpp
    src/core/realtime/RealTimeEngineEstimate.cpp
    src/core/realtime/RealTimeEngineInit.cpp
    src/core/realtime/RealTimeEngineIngest.cpp
    src/core/realtime/RealTimeEngineStateSnap.cpp
    src/core/realtime/RtStateSnapshot.cpp
    src/core/realtime/PbDiscCarryLedger.cpp
    src/core/realtime/PbStepTranslateLedger.cpp
    src/core/realtime/AmbLevelCheck.cpp
    src/core/realtime/InnovationScreen.cpp
    src/core/realtime/RealTimeEngineScreen.cpp
    src/core/realtime/ConsumerSatTrace.cpp
    src/core/filter/PivotManager.cpp
    src/core/filter/DdPivotManager.cpp
    src/core/filter/CovarianceReReference.cpp
    src/core/network/solve/NormalEquations.cpp
    src/core/network/solve/NormalEquationsDetached.cpp
    src/core/network/solve/Estimator.cpp
    src/core/network/carry/InvariantCarryDriver.cpp
    src/core/network/solve/Sbasis.cpp
    src/core/network/assembly/ExtraBandScope.cpp
    src/core/network/assembly/SecondCodeScope.cpp
    src/core/network/assembly/SecondPhaseScope.cpp
    src/core/network/assembly/ChannelAll.cpp
    src/core/network/assembly/MultiGnss.cpp
    src/core/network/solve/PhaseDatum.cpp
    src/core/network/solve/PhaseAnchorEpoch.cpp
    src/core/network/slip/SlipFunnel.cpp
    src/core/network/solve/ChainDatum.cpp
    src/core/network/solve/AnchorHygiene.cpp
    src/core/network/assembly/SignalIntegrity.cpp
    src/core/network/assembly/MeasurementAdmission.cpp
    src/core/network/qc/Raim.cpp
    src/core/network/qc/ClockQc.cpp
    src/core/network/solve/ClockAnchor.cpp
    src/core/network/state/CarryDisposition.cpp
    src/core/network/state/CarrySites.cpp
    src/core/network/assembly/Responses.cpp
    src/core/network/solve/ClockLevel.cpp
    src/core/network/diag/FrameDiag.cpp
    src/core/network/qc/GrossLos.cpp
    src/core/network/diag/StationBlock.cpp
    src/core/network/solve/HandoverStransform.cpp
    src/core/network/assembly/RowClassify.cpp
    src/core/network/qc/RhsAttrib.cpp
    src/core/network/qc/PrefitCensus.cpp
    src/core/network/qc/RedundancyCensus.cpp
    src/core/network/qc/Mdb.cpp
    src/core/network/solve/DatumGraph.cpp
    src/core/network/qc/BlockBaarda.cpp
    src/core/network/qc/Congruence.cpp
    src/core/network/qc/CongruenceSetup.cpp
    src/core/network/qc/PriorResidual.cpp
    src/core/network/emit/SolPrep.cpp
    src/core/network/emit/EmitStore.cpp
    src/core/network/emit/PbHold.cpp
    src/core/network/emit/BrdcWire.cpp
    src/core/network/emit/LedgerEmit.cpp
    src/core/network/diag/IonoDiag.cpp
    src/core/network/diag/DdFrac.cpp
    src/core/network/ar/SdEwlAr.cpp
    src/core/network/ar/SdEwlRows.cpp
    src/core/network/diag/ShDevVtec.cpp
    src/core/network/solve/SolveStations.cpp
    src/core/network/assembly/StationBuild.cpp
    src/core/network/assembly/IonoConstraintRows.cpp
    src/core/network/carry/LevelingCarry.cpp
    src/core/network/carry/PhaseColourCarry.cpp
    src/core/network/carry/RxClockCarry.cpp
    src/core/network/carry/RxCodeBiasCarry.cpp
    src/core/network/carry/SlantIonoCarry.cpp
    src/core/network/assembly/StationAnchors.cpp
    src/core/network/assembly/StationCarry.cpp
    src/core/network/assembly/StationSplit.cpp
    src/core/network/carry/ZwdCarry.cpp
    src/core/network/diag/ShHealth.cpp
    src/core/network/diag/Leveling.cpp
    src/core/network/diag/StashStatus.cpp
    src/core/network/ar/ProducerAr.cpp
    src/core/network/ar/ArSuspect.cpp
    src/core/network/ar/ArcGraph.cpp
    src/core/network/ar/ArcLife.cpp
    src/core/network/ar/ArCycleBasis.cpp
    src/core/network/ar/ArLattice.cpp
    src/core/network/ar/FixedCycles.cpp
    src/core/network/carry/InstallCarry.cpp
    src/core/network/carry/DdPriorBuild.cpp
    src/core/network/solve/SolveGlue.cpp
    src/core/network/solve/CodeBiasDatum.cpp
    src/core/network/solve/ClockDatum.cpp
    src/core/network/solve/RankGate.cpp
    src/core/network/solve/SbasisCompletion.cpp
    src/core/network/qc/RaimWithinEpoch.cpp
    src/core/network/qc/WithinEpochCulprits.cpp
    src/core/network/carry/LevelAuthority.cpp
    src/core/network/qc/Relevel.cpp
    src/core/network/runend/Teardown.cpp
    src/core/network/runend/RunTimingRunEnd.cpp
    src/core/network/runend/RunArtifactsRunEnd.cpp
    src/core/network/runend/SolveStationsRunEnd.cpp
    src/core/network/runend/ResumeRunEnd.cpp
    src/core/network/runend/ReadmitRunEnd.cpp
    src/core/network/runend/ClosedLoopRunEnd.cpp
    src/core/network/carry/LevelingCarryRunEnd.cpp
    src/core/network/carry/RxCodeBiasCarryRunEnd.cpp
    src/core/network/carry/CarryVceRunEnd.cpp
    src/core/network/carry/CarryVceApply.cpp
    src/core/network/slip/SlipQcRunEnd.cpp
    src/core/network/runend/RunEnd.cpp
    src/core/network/driver/EpochObservers.cpp
    src/core/network/carry/Inject.cpp
    src/core/network/assembly/CanonicalPairAdmission.cpp
    src/core/network/state/ArcLedger.cpp
    src/core/network/solve/AnchorReentry.cpp
    src/core/network/slip/OraclePin.cpp
    src/core/network/slip/OraclePinSetup.cpp
    src/core/network/carry/Coupling.cpp
    src/core/network/carry/CouplingExact.cpp
    src/core/network/carry/ClockClone.cpp
    src/core/network/carry/SharedHistory.cpp
    src/core/network/diag/PhaseVisibleDiag.cpp
    src/core/network/emit/PbHoldClone.cpp
    src/core/network/assembly/ParamRegistry.cpp
    src/core/network/assembly/ParamInventory.cpp
    src/core/network/assembly/Contributions.cpp
    src/core/network/diag/AmbStateTrace.cpp
    src/core/network/ar/AmbFloat.cpp
    src/core/network/assembly/BiasAdmission.cpp
    src/core/filter/NeqAccumulator.cpp
    src/core/common/SignalBand.cpp
    src/core/common/GnssIdentity.cpp
 "src/core/math/Coordinates.cpp" "src/core/math/Interpolate.cpp")
target_include_directories(gnss_core PUBLIC include)
# KalmanFilter.cpp emits more COFF sections than the default object format
# allows (template-heavy Eigen kernels); /bigobj raises the limit.  Side-effect
# free build flag (MSVC only).  ApplicationNetwork.cpp no longer needs it: the
# decomposition (INC-0..12) relocated the template-heavy epoch-loop body into the
# filter/Network* seams, shrinking the driver TU from ~14.7k to ~1.6k lines, back
# under the section limit.  ApplicationNetworkAssembly.cpp keeps it (the PASS-B
# per-station Eigen assembly still crosses the limit).
if(MSVC)
    set_source_files_properties(src/core/filter/KalmanFilter.cpp
        src/core/network/driver/ApplicationNetworkAssembly.cpp
        PROPERTIES COMPILE_FLAGS "/bigobj")
    # tests/network/test_block_baarda.cpp sits at the Debug section limit through the network
    # run-context headers it includes (card CTU section 14 added a few template members
    # there and tipped it over, C1128).
    set_source_files_properties(tests/network/test_block_baarda.cpp
        PROPERTIES COMPILE_FLAGS "/bigobj")
    # tests/network/test_block_baarda_estimable.cpp includes the same run-context headers.
    set_source_files_properties(tests/network/test_block_baarda_estimable.cpp
        PROPERTIES COMPILE_FLAGS "/bigobj")
    # tests/network/test_coupling_exact_update.cpp: card WSX's generalized eigen-solve of the border
    # covariance against the batch tipped it over the Debug section limit (C1128).
    set_source_files_properties(tests/network/test_coupling_exact_update.cpp
        PROPERTIES COMPILE_FLAGS "/bigobj")
    # tests/network/test_handover_stransform_dense.cpp (card SCL B2): the run-context headers plus the
    # explicit eigen-solve and inverse references cross the Debug section limit (C1128).
    set_source_files_properties(tests/network/test_handover_stransform_dense.cpp
        PROPERTIES COMPILE_FLAGS "/bigobj")
    # tests/network/test_network_shared_history.cpp (card SCL2 X2b, gate U5): the smoothed hold's
    # clone types and the projection cases cross the Debug section limit (C1128).
    set_source_files_properties(tests/network/test_network_shared_history.cpp
        PROPERTIES COMPILE_FLAGS "/bigobj")
    # tests/network/test_network_detached_fold.cpp (card FCF, FCF-1): the dense oracles (LDLT,
    # pivoted LU, the rank and condition-number references) cross the Debug section
    # limit (C1128).
    set_source_files_properties(tests/network/test_network_detached_fold.cpp
        PROPERTIES COMPILE_FLAGS "/bigobj")
    # tests/test_pb_hold_block.cpp and tests/test_pb_hold_smoothed.cpp (card NOM3 step EP2): the
    # run fixture (tests/support/NetworkTestRun.hpp) brings the run's engines into the
    # translation unit and crosses the Debug section limit (C1128).
    set_source_files_properties(tests/test_pb_hold_block.cpp tests/test_pb_hold_smoothed.cpp
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

# Ocean tide loading inspector - SIF lookup, coefficient table and ENU displacement
# series from the production OceanTideModel (input of scripts/otl_compare/otl_gate.py).
add_executable(gnss_otl_diag src/tools/otl_diag.cpp)
target_link_libraries(gnss_otl_diag PRIVATE gnss_core)

# GNSS exam dataset simulator — synthesizes a multi-GNSS RINEX-3.04 observation
# file (+ truth answer key, in later milestones) for the GNSS/Geodesy qualifying
# exam, then verifies the processor recovers the injected truth.  Links
# gnss_core only; coupling to the verifier is out-of-process (writes RINEX, runs
# gnss_processor, diffs traces).  See C:/GnssSimulator/docs/DESIGN.md.
add_executable(gnss_simulator
    src/sim/main.cpp
    src/sim/SimConfig.cpp
    src/sim/SatClockError.cpp
    src/sim/ScriptedSlips.cpp
    src/sim/RinexObsWriter.cpp
    src/sim/EphemerisProvider.cpp
    src/sim/TrajectoryGenerator.cpp
    src/sim/IonoDisturbance.cpp
    src/sim/OtlField.cpp
    src/sim/Verifier.cpp)
target_link_libraries(gnss_simulator PRIVATE gnss_core)

# Unit tests
enable_testing()
add_executable(gnss_tests
    tests/test_math_core.cpp
    tests/test_epoch_seek.cpp
    tests/test_leap_second_scale.cpp
    tests/test_atmosphere_models.cpp
    tests/test_gpt3_load_path.cpp
    tests/test_site_displacement.cpp
    tests/test_phase_windup_sign.cpp
    tests/test_antenna_model.cpp
    tests/test_state_key_schema.cpp
    tests/test_covariance_rereference.cpp
    tests/network/test_network_emit.cpp
    tests/test_sh_ionosphere.cpp
    tests/network/test_network_product_io.cpp
    tests/network/test_network_combined_sigma.cpp
    tests/test_rinex_nav_parser.cpp
    tests/test_brdc_clock_apriori.cpp
    tests/test_rinex_zero_sentinel.cpp
    tests/test_rinex3_blank_prn.cpp
    tests/test_rinex3_phase_shift.cpp
    tests/network/test_network_normal_equations.cpp
    tests/network/test_network_detached_fold.cpp
    tests/network/test_neq_station_prior_before_pd.cpp
    tests/network/test_pd_safe_fold.cpp
    tests/network/test_carry_aware_covariance.cpp
    tests/test_w6_iono_constraint.cpp
    tests/network/test_sat_gf_extra_band_free.cpp
    tests/network/test_network_estimator.cpp
    tests/network/test_network_carry_vce.cpp
    tests/network/test_carry_vce_apply.cpp
    tests/test_residual_qc_wtest.cpp
    tests/network/test_pivot_frame_q.cpp
    tests/network/test_border_clone.cpp
    tests/network/test_network_zstore.cpp
    tests/network/test_network_arc_graph.cpp
    tests/network/test_network_arc_graph_oracles.cpp
    tests/network/test_network_arc_life.cpp
    tests/network/test_producer_ar_cycle_basis.cpp
    tests/network/test_ar_cycle_basis_config.cpp
    tests/network/test_network_ar_lattice.cpp
    tests/network/test_network_fixed_cycles.cpp
    tests/network/test_network_fixed_cycle_release.cpp
    tests/test_fixed_cycle_config.cpp
    tests/network/test_network_robust_stats.cpp
    tests/network/test_network_slip_qc.cpp
    tests/test_gap_reentry_relevel.cpp
    tests/network/test_gap_reentry_per_signal.cpp
    tests/test_gap_reentry_coresumed.cpp
    tests/test_tracker_modeled_first.cpp
    tests/network/test_signal_roster.cpp
    tests/network/test_signal_roster_plan.cpp
    tests/network/test_signal_roster_always_on.cpp
    tests/network/test_n3c_switches.cpp
    tests/network/test_net_small_defects.cpp
    tests/network/test_signal_roster_sites.cpp
    tests/network/test_arc_ledger.cpp
    tests/network/test_ledger_anchor.cpp
    tests/test_ledger_reentry.cpp
    tests/test_ledger_emit.cpp
    tests/network/test_slip_oracle.cpp
    tests/test_if_phase_slip_detector.cpp
    tests/test_if_phase_ref_gate_defaults.cpp
    tests/test_if_td_pair_width.cpp
    tests/test_if_td_reseed_on_flag.cpp
    tests/test_iftd_ref_arm_all.cpp
    tests/network/test_network_coupling.cpp
    tests/network/test_network_invariant_carry_driver.cpp
    tests/network/test_shdev_carry.cpp
    tests/network/test_rx_cb_carry.cpp
    tests/network/test_network_phase_datum.cpp
    tests/test_eq34_reattach_hygiene.cpp
    tests/test_signal_integrity.cpp
    tests/test_measurement_admission.cpp
    tests/test_measurement_model_prepass.cpp
    tests/network/test_redundancy_census.cpp
    tests/network/test_network_mdb.cpp
    tests/network/test_block_baarda.cpp
    tests/network/test_block_baarda_estimable.cpp
    tests/network/test_network_gross_los.cpp
    tests/network/test_network_gross_los_cold_bound.cpp
    tests/network/test_station_congruence.cpp
    tests/network/test_prior_residual_census.cpp
    tests/network/test_producer_ar_trace.cpp
    tests/test_prefit_census.cpp
    tests/network/test_network_bias_admission.cpp
    tests/network/test_network_rank_gate.cpp
    tests/network/test_network_sbasis_complete.cpp
    tests/test_datum_graph_census.cpp
    tests/network/test_station_block_audit.cpp
    tests/test_card4_emit_rebase.cpp
    tests/network/test_clock_emit_rebase.cpp
    tests/network/test_clock_anchor.cpp
    tests/network/test_network_solprep_disc.cpp
    tests/test_eq34_consumer_integer.cpp
    tests/network/test_glonass_network_config.cpp
    tests/network/test_codebias_network_config.cpp
    tests/network/test_rx_cb_datum_gauge.cpp
    tests/test_raim_carry_invariant_pool.cpp
    tests/test_phase_datum_handover.cpp
    tests/test_datum_station_slip_config.cpp
    tests/test_d5_corroborate_autoarm.cpp
    tests/test_h_heartbeat_rider.cpp
    tests/test_bia_receiver_records.cpp
    tests/network/test_iono_carry_config.cpp
    tests/network/test_network_process_noise_config.cpp
    tests/test_withhold_release_config.cpp
    tests/network/test_station_factor_config.cpp
    tests/network/test_level_authority.cpp
    tests/test_config_extends.cpp
    tests/test_sd_ewl_prereduce.cpp
    tests/network/test_sd_ewl_integer.cpp
    tests/test_ssr_store_injection.cpp
    tests/test_realtime_seams.cpp
    tests/test_consumer_disc_carry.cpp
    tests/test_pb_step_translate.cpp
    tests/test_pb_emit_hold.cpp
    tests/test_pb_hold_phase_clock.cpp
    tests/test_pb_hold_smoothed.cpp
    tests/test_pb_hold_block.cpp
    tests/network/test_wsx_within_station_cross.cpp
    tests/test_phase_visible_monitor.cpp
    tests/test_pb_hold_presence.cpp
    tests/test_pb_translation_premise.cpp
    tests/test_amb_level_check.cpp
    tests/test_amb_level_arc.cpp
    tests/test_amb_level_statistics.cpp
    tests/test_amb_level_band_scope.cpp
    tests/test_innovation_screen.cpp
    tests/test_innovation_screen_engine.cpp
    tests/test_disc_mark_scope.cpp
    tests/test_consumer_sat_trace.cpp
    tests/test_transforms.cpp
    tests/test_filter_core.cpp
    tests/test_rtcm.cpp
    tests/test_rtcm_stream_dating.cpp
    tests/test_epoch_cadence.cpp
    tests/test_realtime_defects.cpp
    tests/test_tcp_client_silence.cpp
    tests/network/test_network_streaming.cpp
    tests/test_rtcm_logger_lifetime.cpp
    tests/test_safe_logger.cpp
    tests/test_sbf.cpp
    tests/test_estimation_core.cpp
    tests/test_ils_core.cpp
    tests/test_ils_decorrelated_partial.cpp
    tests/test_wl_only_ffr.cpp
    tests/test_nl_search_row.cpp
    tests/test_fix_and_hold.cpp
    tests/test_signal_math.cpp
    tests/test_geometry.cpp
    tests/test_stochastic_model.cpp
    tests/test_turbo_edit_resolver.cpp
    tests/test_soft_lli_bridge.cpp
    tests/test_cycle_slip_counters.cpp
    tests/test_aiding_source.cpp
    tests/test_smoother_frame.cpp
    tests/test_jump_gap_fitter.cpp
    tests/test_slip_bridge_kernel.cpp
    tests/network/test_relevel_bridge_v3.cpp
    tests/network/test_relevel_delta_bridge.cpp
    tests/test_planetary_cio.cpp
    tests/test_planetary_sun_frame.cpp
    tests/test_orbit_enhancements.cpp
    tests/test_noon_turn_attitude.cpp
    tests/test_neq_accumulator.cpp
    tests/test_station_info.cpp
    tests/test_station_info_guards.cpp
    tests/test_station_info_seasonal.cpp
    tests/test_otl_sif_source.cpp
    tests/test_otl_translation.cpp
    tests/test_antenna_radome_fallback.cpp
    tests/test_antenna_eccentricity.cpp
    tests/test_station_marker_to_arp.cpp
    tests/test_sif_apriori_arp.cpp
    tests/test_log_manager_init.cpp
    tests/test_product_cache_load_failure.cpp
    tests/test_windup_census_config.cpp
    tests/test_windup_gap_bridge.cpp
    tests/test_windup_restart_response_config.cpp
    tests/test_windup_restart_targets.cpp
    tests/network/test_network_param_registry.cpp
    tests/test_gnss_identity.cpp
    tests/network/test_network_param_inventory.cpp
    tests/network/test_network_rx_extra_band_delay.cpp
    tests/network/test_network_contributions.cpp
    tests/network/test_service_mode_clock_refusals.cpp
    tests/network/test_loader_message_conditions.cpp
    tests/network/test_run_manifest_toggles.cpp
    tests/network/test_network_knobs_lane_emit.cpp
    tests/network/test_network_knobs_epoch_driver.cpp
    tests/network/test_network_knobs_station_assembly.cpp
    tests/network/test_network_epoch_contract.cpp
    tests/network/test_network_station_build.cpp
    tests/network/test_network_harvest_layout.cpp
    tests/network/test_network_engine_spec.cpp
    tests/network/test_network_row_classify.cpp
    tests/network/test_raim_pool_data_rows_only.cpp
    tests/network/test_network_rhs_attrib.cpp
    tests/test_rhs_attrib_columns.cpp
    tests/test_rhs_attrib_local.cpp
    tests/test_phase_colour_state.cpp
    tests/network/test_exl_switch_on_resume.cpp
    tests/network/test_carry_disposition.cpp
    tests/network/test_carry_sites.cpp
    tests/test_param_responses.cpp
    tests/test_zwd_seam_reseed_config.cpp
    tests/network/test_zwd_seam_reseed_q.cpp
    tests/network/test_carry_zwd_in_block.cpp
    tests/network/test_carry_iono_in_block.cpp
    tests/network/test_carry_rx_clock_in_block.cpp
    tests/test_resume_frame_restore.cpp
    tests/test_resume_pb_seam_mark.cpp
    tests/test_resume_border_restore.cpp
    tests/test_resume_coupling_restore.cpp
    tests/test_resume_datum_restore.cpp
    tests/test_rt_state_snapshot.cpp
    tests/test_state_family.cpp
    tests/test_state_manager_process_noise.cpp
    tests/test_state_manager_families.cpp
    tests/test_slant_ionosphere_presence.cpp
    tests/test_kalman_complete_reset.cpp
    tests/test_kalman_pivot_detectors.cpp
    tests/test_kalman_qc_counters.cpp
    tests/test_ppp_epoch_work.cpp
    tests/network/test_network_inject.cpp
    tests/network/test_network_run_end.cpp
    tests/network/test_network_entities_records.cpp
    tests/network/test_network_epoch_observers.cpp
    tests/network/test_network_solve_skip.cpp
    tests/network/test_network_we_choice.cpp
    tests/network/test_network_within_epoch_culprits.cpp
    tests/network/test_canonical_pair_admission.cpp
    tests/network/test_oracle_pin.cpp
    tests/test_warm_resume_require.cpp
    tests/test_anchor_slip_handover.cpp
    tests/network/test_phase_gate_drop_edge.cpp
    tests/network/test_handover_stransform.cpp
    tests/network/test_handover_stransform_dense.cpp
    tests/test_mw_evidence_refloat.cpp
    tests/test_mw_pivot_reset.cpp
    tests/network/test_network_gf_arc_jump.cpp
    tests/network/test_network_ar_suspect.cpp
    tests/network/test_common_band_scope.cpp
    tests/test_candidacy_usable.cpp
    tests/test_restored_arc_age.cpp
    tests/network/test_resume_carry_rebase.cpp
    tests/test_resume_emit_restore.cpp
    tests/test_resume_station_restore.cpp
    tests/test_resume_signal_canon_restore.cpp
    tests/test_resume_rxcb_lin_restore.cpp
    tests/network/test_amb_float_vehicle.cpp
    tests/test_phase_held_basis.cpp
    tests/test_canonical_signal_latch.cpp
    tests/test_canonical_signal_station_scope.cpp
    tests/test_canonical_signal_code_follow.cpp
    tests/network/test_mw_detect_pair_scope.cpp
    tests/test_mw_detector_steps.cpp
    tests/network/test_extra_band_code_sigma.cpp
    tests/network/test_network_second_code_scope.cpp
    tests/network/test_second_phase_channel.cpp
    tests/network/test_channel_mode_all.cpp
    tests/network/test_galileo_network.cpp
    tests/test_code_datum_family.cpp
    tests/test_clock_level_carry.cpp
    tests/network/test_coupling_exact_update.cpp
    tests/network/test_coupling_long_horizon.cpp
    tests/network/test_network_clock_clone.cpp
    tests/network/test_network_shared_history.cpp
    tests/network/test_network_column_family.cpp
    tests/network/test_ctu_level_guard.cpp
    tests/test_owd_wire_contract.cpp
    tests/test_product_contract_gaps.cpp
    tests/test_reinflate_never_shrinks.cpp
    tests/test_active_system_admission.cpp
    tests/test_group_delay_latch.cpp
    tests/test_residual_history_tracker.cpp
    tests/test_sat_health.cpp
    tests/test_clock_level_batch2.cpp
    tests/test_graphic_slip_detector.cpp
    tests/test_minor_defect_fixes.cpp
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
    tests/sim/test_sim_network_fixture.cpp
    tests/sim/test_sim_ambiguity_regime.cpp
    tests/sim/test_sim_phase_colour.cpp
    src/sim/RinexObsWriter.cpp
    src/sim/IonoDisturbance.cpp
    src/sim/SimConfig.cpp
    src/sim/SatClockError.cpp
    src/sim/ScriptedSlips.cpp
    src/sim/TrajectoryGenerator.cpp
    src/sim/OtlField.cpp
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
