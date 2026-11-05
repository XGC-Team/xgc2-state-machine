cmake_minimum_required(VERSION 3.10)
project(xgc2_state_machine VERSION 0.2.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(XGC2_STATE_MACHINE_BUILD_TESTING "Build xgc2_state_machine tests" ON)

add_library(xgc2_state_machine INTERFACE)
add_library(xgc2_state_machine::state_machine ALIAS xgc2_state_machine)
set_target_properties(xgc2_state_machine PROPERTIES EXPORT_NAME state_machine)
target_compile_features(xgc2_state_machine INTERFACE cxx_std_17)
target_include_directories(xgc2_state_machine INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

install(TARGETS xgc2_state_machine EXPORT xgc2_state_machineTargets)
install(FILES include/state_machine/adt_machine.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/state_machine)
configure_package_config_file(cmake/xgc2_state_machineConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/xgc2_state_machineConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/xgc2_state_machine
  PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/xgc2_state_machineConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
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
    add_test(NAME adt_compile_contract COMMAND ${CMAKE_COMMAND}
      -DCOMPILER=${CMAKE_CXX_COMPILER}
      -DINCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/include
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/test/adt_compile_contract.cpp
      -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/test/adt_compile_contract.cmake)
  endif()
endif()
