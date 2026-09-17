if(NOT DEFINED READELF OR NOT DEFINED EXECUTABLE OR NOT DEFINED INTERPRETER OR NOT DEFINED RUNTIME_DIR)
  message(FATAL_ERROR "READELF, EXECUTABLE, INTERPRETER, and RUNTIME_DIR are required")
endif()

execute_process(
  COMMAND "${READELF}" -l "${EXECUTABLE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE program_headers
  ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Could not read ELF program headers for ${EXECUTABLE}: ${error}")
endif()
string(FIND "${program_headers}" "Requesting program interpreter: ${INTERPRETER}]" interpreter_index)
if(interpreter_index EQUAL -1)
  message(FATAL_ERROR "${EXECUTABLE} does not select Bootlin interpreter ${INTERPRETER}")
endif()

execute_process(
  COMMAND "${READELF}" -d "${EXECUTABLE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE dynamic_section
  ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Could not read ELF dynamic section for ${EXECUTABLE}: ${error}")
endif()
string(FIND "${dynamic_section}" "(RPATH)" rpath_index)
if(rpath_index EQUAL -1)
  message(FATAL_ERROR "${EXECUTABLE} has no private DT_RPATH")
endif()
string(FIND "${dynamic_section}" "${RUNTIME_DIR}" runtime_index)
if(runtime_index EQUAL -1)
  message(FATAL_ERROR "${EXECUTABLE} DT_RPATH does not include Bootlin runtime ${RUNTIME_DIR}")
endif()
