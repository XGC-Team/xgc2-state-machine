cmake_minimum_required(VERSION 3.10)
project(xgc2_state_machine VERSION 0.2.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(XGC2_STATE_MACHINE_BUILD_TESTING "Build xgc2_state_machine tests" ON)
option(XGC2_STATE_MACHINE_BUILD_ROS1_TESTING "Build the optional ROS1 edge tests" OFF)

add_library(xgc2_state_machine INTERFACE)
add_library(xgc2_state_machine::state_machine ALIAS xgc2_state_machine)
set_target_properties(xgc2_state_machine PROPERTIES EXPORT_NAME state_machine)
target_compile_features(xgc2_state_machine INTERFACE cxx_std_17)
target_include_directories(xgc2_state_machine INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

install(TARGETS xgc2_state_machine EXPORT xgc2_state_machineTargets)
install(FILES include/state_machine/adt_machine.hpp
  include/state_machine/signal_slot.hpp include/state_machine/next_wake.hpp
  include/state_machine/ordered_worker.hpp include/state_machine/async_worker.hpp
  include/state_machine/sync_runtime_tick_adapter.hpp include/state_machine/ros1_adaptive_loop.hpp
  include/state_machine/ros1_queued_owner.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/state_machine)

find_package(Threads REQUIRED)
add_library(xgc2_state_machine_workers INTERFACE)
add_library(xgc2_state_machine::workers ALIAS xgc2_state_machine_workers)
set_target_properties(xgc2_state_machine_workers PROPERTIES EXPORT_NAME workers)
target_link_libraries(xgc2_state_machine_workers INTERFACE
  xgc2_state_machine::state_machine Threads::Threads)
install(TARGETS xgc2_state_machine_workers EXPORT xgc2_state_machineWorkersTargets)
install(EXPORT xgc2_state_machineWorkersTargets NAMESPACE xgc2_state_machine::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xgc2_state_machine)
add_library(xgc2_state_machine_ros1 INTERFACE)
add_library(xgc2_state_machine::ros1 ALIAS xgc2_state_machine_ros1)
set_target_properties(xgc2_state_machine_ros1 PROPERTIES EXPORT_NAME ros1)
target_link_libraries(xgc2_state_machine_ros1 INTERFACE
  xgc2_state_machine::state_machine Threads::Threads)
install(TARGETS xgc2_state_machine_ros1 EXPORT xgc2_state_machineRos1Targets)
install(EXPORT xgc2_state_machineRos1Targets NAMESPACE xgc2_state_machine::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xgc2_state_machine)
add_library(xgc2_state_machine_ros1_loop INTERFACE)
add_library(xgc2_state_machine::ros1_loop ALIAS xgc2_state_machine_ros1_loop)
set_target_properties(xgc2_state_machine_ros1_loop PROPERTIES EXPORT_NAME ros1_loop)
target_link_libraries(xgc2_state_machine_ros1_loop INTERFACE xgc2_state_machine::ros1)
install(TARGETS xgc2_state_machine_ros1_loop EXPORT xgc2_state_machineRos1LoopTargets)
install(EXPORT xgc2_state_machineRos1LoopTargets NAMESPACE xgc2_state_machine::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xgc2_state_machine)
if(XGC2_STATE_MACHINE_BUILD_ROS1_TESTING)
  find_package(roscpp REQUIRED)
  find_package(rosgraph_msgs REQUIRED)
  target_include_directories(xgc2_state_machine_ros1 SYSTEM INTERFACE
    "$<BUILD_INTERFACE:${roscpp_INCLUDE_DIRS}>")
  target_include_directories(xgc2_state_machine_ros1_loop SYSTEM INTERFACE
    "$<BUILD_INTERFACE:${rosgraph_msgs_INCLUDE_DIRS}>")
  target_link_libraries(xgc2_state_machine_ros1 INTERFACE
    "$<BUILD_INTERFACE:${roscpp_LIBRARIES}>")
endif()
configure_package_config_file(cmake/xgc2_state_machineConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/xgc2_state_machineConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xgc2_state_machine
  PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/xgc2_state_machineConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/xgc2_state_machineConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/xgc2_state_machineConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xgc2_state_machine)
install(EXPORT xgc2_state_machineTargets NAMESPACE xgc2_state_machine::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xgc2_state_machine)

if(XGC2_STATE_MACHINE_BUILD_TESTING)
  include(CTest)
  if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    if(TARGET GTest::gtest)
      set(_xgc2_state_machine_gtest GTest::gtest)
    else()
      set(_xgc2_state_machine_gtest GTest::GTest)
    endif()
    if(TARGET GTest::gtest_main)
      set(_xgc2_state_machine_gtest_main GTest::gtest_main)
    else()
      set(_xgc2_state_machine_gtest_main GTest::Main)
    endif()
    add_executable(adt_machine_test test/adt_machine_test.cpp)
    target_link_libraries(adt_machine_test PRIVATE xgc2_state_machine::state_machine
      ${_xgc2_state_machine_gtest} ${_xgc2_state_machine_gtest_main})
    add_test(NAME adt_machine_test COMMAND adt_machine_test)
    add_executable(foundation_test test/foundation_test.cpp)
    target_link_libraries(foundation_test PRIVATE xgc2_state_machine::workers
      ${_xgc2_state_machine_gtest} ${_xgc2_state_machine_gtest_main})
    add_test(NAME foundation_test COMMAND foundation_test)
    set_tests_properties(foundation_test PROPERTIES TIMEOUT 30)
    add_executable(async_worker_test test/async_worker_test.cpp)
    target_link_libraries(async_worker_test PRIVATE xgc2_state_machine::workers
      ${_xgc2_state_machine_gtest} ${_xgc2_state_machine_gtest_main})
    add_test(NAME async_worker_test COMMAND async_worker_test)
    set_tests_properties(async_worker_test PROPERTIES TIMEOUT 30)
    add_executable(edge_schedule_test test/edge_schedule_test.cpp)
    target_link_libraries(edge_schedule_test PRIVATE xgc2_state_machine::state_machine
      ${_xgc2_state_machine_gtest} ${_xgc2_state_machine_gtest_main})
    add_test(NAME edge_schedule_test COMMAND edge_schedule_test)
    if(XGC2_STATE_MACHINE_BUILD_ROS1_TESTING)
      add_executable(ros1_adaptive_loop_test test/ros1_adaptive_loop_test.cpp)
      target_link_libraries(ros1_adaptive_loop_test PRIVATE xgc2_state_machine::ros1_loop
        ${_xgc2_state_machine_gtest})
      # A dedicated offline ROS master/use_sim_time fixture runs this executable.
    endif()
    add_test(NAME adt_compile_contract COMMAND ${CMAKE_COMMAND}
      -DCOMPILER=${CMAKE_CXX_COMPILER}
      -DINCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/include
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/test/adt_compile_contract.cpp
      -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/test/adt_compile_contract.cmake)
  endif()
endif()
