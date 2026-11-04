# The same consumer must compile with complete handlers and fail for each
# missing handler / unsafe lifetime / invalid target. No runtime gates.
foreach(case RANGE 0 4)
  execute_process(COMMAND "${COMPILER}" -std=c++17 "-I${INCLUDE_DIR}"
    "-DCONTRACT_CASE=${case}" -c "${SOURCE}" -o "${BINARY_DIR}/adt-contract-${case}.o"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE error)
  file(WRITE "${BINARY_DIR}/adt-contract-${case}.log" "${out}${error}")
  if(case EQUAL 0 AND NOT result EQUAL 0)
    message(FATAL_ERROR "Valid ADT consumer failed: ${error}")
  elseif(NOT case EQUAL 0 AND result EQUAL 0)
    message(FATAL_ERROR "Invalid ADT consumer ${case} compiled")
  endif()
endforeach()
