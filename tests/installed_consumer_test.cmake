if(NOT DEFINED TAVERN_BUILD_DIR OR NOT DEFINED TAVERN_CONSUMER_SOURCE_DIR OR
   NOT DEFINED TAVERN_WORK_DIR)
  message(FATAL_ERROR "Missing installed-consumer test paths")
endif()

file(REMOVE_RECURSE "${TAVERN_WORK_DIR}")
file(MAKE_DIRECTORY "${TAVERN_WORK_DIR}")
set(prefix "${TAVERN_WORK_DIR}/prefix")
set(source "${TAVERN_WORK_DIR}/consumer-source")
set(build "${TAVERN_WORK_DIR}/consumer-build")
string(TOUPPER "${TAVERN_BUILD_TYPE}" build_type_upper)
file(COPY "${TAVERN_CONSUMER_SOURCE_DIR}/" DESTINATION "${source}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${TAVERN_BUILD_DIR}" --prefix "${prefix}"
  RESULT_VARIABLE result)
if(result)
  message(FATAL_ERROR "Tavern installation failed: ${result}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${source}" -B "${build}"
          "-DCMAKE_PREFIX_PATH=${prefix}" "-DCMAKE_BUILD_TYPE=${TAVERN_BUILD_TYPE}"
          "-DCMAKE_CXX_FLAGS=${TAVERN_CONSUMER_CXX_FLAGS}"
          "-DCMAKE_CXX_FLAGS_${build_type_upper}=${TAVERN_CONSUMER_CXX_FLAGS_CONFIG}"
          "-DCMAKE_EXE_LINKER_FLAGS=${TAVERN_CONSUMER_EXE_LINKER_FLAGS}"
  RESULT_VARIABLE result)
if(result)
  message(FATAL_ERROR "Installed consumer configure failed: ${result}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" RESULT_VARIABLE result)
if(result)
  message(FATAL_ERROR "Installed consumer build failed: ${result}")
endif()
execute_process(COMMAND "${build}/tavern_installed_consumer"
                        "${TAVERN_WORK_DIR}/consumer.db"
                RESULT_VARIABLE result)
if(result)
  message(FATAL_ERROR "Installed consumer execution failed: ${result}")
endif()
