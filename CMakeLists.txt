cmake_minimum_required(VERSION 3.0.2)
project(state_machine)

find_package(catkin REQUIRED)

catkin_package(
  INCLUDE_DIRS include
  LIBRARIES ${PROJECT_NAME}
)

include_directories(
  include
  ${catkin_INCLUDE_DIRS}
)

add_library(${PROJECT_NAME} SHARED
  src/state_machine.cpp
)

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)

target_include_directories(${PROJECT_NAME} PUBLIC
  include
)

target_link_libraries(${PROJECT_NAME}
  ${catkin_LIBRARIES}
)

if(CATKIN_ENABLE_TESTING)
  catkin_add_gtest(${PROJECT_NAME}_runtime_test
    test/state_machine_runtime_test.cpp
  )
  if(TARGET ${PROJECT_NAME}_runtime_test)
    target_link_libraries(${PROJECT_NAME}_runtime_test
      ${PROJECT_NAME}
      ${catkin_LIBRARIES}
    )
    target_include_directories(${PROJECT_NAME}_runtime_test PRIVATE
      include
    )
  endif()

  find_program(LCOV_EXECUTABLE lcov)
  if(LCOV_EXECUTABLE)
    set(COVERAGE_DIR ${CMAKE_BINARY_DIR}/${PROJECT_NAME}_coverage)
    add_custom_target(${PROJECT_NAME}_coverage
      COMMAND ${CMAKE_COMMAND} -E rm -rf ${COVERAGE_DIR}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${COVERAGE_DIR}
      COMMAND ${CMAKE_COMMAND} -E chdir ${COVERAGE_DIR}
        ${CMAKE_CXX_COMPILER}
        -std=c++17 --coverage -O0 -g -pthread
        -I${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/state_machine.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/test/state_machine_runtime_test.cpp
        -lgtest
        -o ${PROJECT_NAME}_runtime_test_coverage
      COMMAND ${CMAKE_COMMAND} -E chdir ${COVERAGE_DIR}
        ./${PROJECT_NAME}_runtime_test_coverage
      COMMAND ${LCOV_EXECUTABLE}
        --capture
        --directory ${COVERAGE_DIR}
        --output-file ${COVERAGE_DIR}/coverage.info
        --include "*/src/common/state_machine/src/state_machine.cpp"
      COMMAND ${LCOV_EXECUTABLE}
        --summary ${COVERAGE_DIR}/coverage.info
      COMMENT "Generating state_machine runtime coverage"
    )
  endif()
endif()
