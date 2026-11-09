function(add_tavern_test target)
  # Most tests use internal support; consumer and TCP integration tests below
  # declare their narrower dependencies explicitly.
  add_executable(${target} ${target}.cpp)
  target_link_libraries(${target} PRIVATE tavern_test_support)
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src)
  add_test(NAME ${target} COMMAND ${target})
endfunction()

add_tavern_test(tavern_test)
add_tavern_test(page_store_test)
add_tavern_test(directory_test)
add_tavern_test(adjacency_pages_test)
add_tavern_test(adjacency_tree_test)
add_tavern_test(persistent_relationship_test)
add_tavern_test(commit_test)
add_tavern_test(database_test)
add_tavern_test(recovery_test)
add_tavern_test(recovery_corruption_test)
add_tavern_test(commit_failure_test)
add_tavern_test(process_crash_test)
add_tavern_test(protected_roots_test)
add_tavern_test(reachability_test)
add_tavern_test(free_page_pool_test)
add_tavern_test(reusable_page_test)
add_tavern_test(page_allocation_session_test)
add_tavern_test(follow_reuse_test)
add_tavern_test(unfollow_reuse_test)
add_tavern_test(unfollow_reuse_crash_test)
add_tavern_test(befriend_reuse_test)
add_tavern_test(befriend_reuse_crash_test)
add_tavern_test(unfriend_reuse_test)
add_tavern_test(unfriend_reuse_crash_test)
add_tavern_test(durability_stress_test)
add_tavern_test(database_lock_test)
add_tavern_test(retained_free_page_pool_test)
add_tavern_test(mutual_friends_test)
add_tavern_test(page_liveness_index_test)
add_tavern_test(index_backed_allocation_session_test)
add_tavern_test(incremental_follow_test)
add_tavern_test(production_reclamation_separation_test)
add_tavern_test(public_database_consumer_test)
add_tavern_test(protocol_test)
target_link_libraries(protocol_test PRIVATE tavern_proto)
target_include_directories(protocol_test PRIVATE ${PROJECT_SOURCE_DIR}/server)

add_executable(tcp_server_test tcp_server_test.cpp)
target_link_libraries(tcp_server_test PRIVATE tavern)
target_include_directories(tcp_server_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(tcp_server_test PRIVATE
  TAVERN_SERVER="$<TARGET_FILE:tavern-server>")
add_dependencies(tcp_server_test tavern-server)
add_test(NAME tcp_server_test COMMAND tcp_server_test)
set_tests_properties(tcp_server_test PROPERTIES TIMEOUT 300)

add_executable(cli_test cli_test.cpp)
target_link_libraries(cli_test PRIVATE tavern)
target_compile_definitions(cli_test PRIVATE
  TAVERN_SERVER="$<TARGET_FILE:tavern-server>"
  TAVERN_CLI="$<TARGET_FILE:tavern-cli>")
add_dependencies(cli_test tavern-server tavern-cli)
add_test(NAME cli_test COMMAND cli_test)
set_tests_properties(cli_test PROPERTIES TIMEOUT 300)

option(TAVERN_ENABLE_SLOW_TESTS "Register slow integration tests" OFF)
if(TAVERN_ENABLE_SLOW_TESTS)
  add_test(NAME tcp_server_response_boundary_test
    COMMAND tcp_server_test --slow-response-boundary)
  set_tests_properties(tcp_server_response_boundary_test PROPERTIES
    LABELS slow
    TIMEOUT 300)
endif()

string(TOUPPER "${CMAKE_BUILD_TYPE}" TAVERN_BUILD_TYPE_UPPER)
add_test(NAME installed_consumer_test
  COMMAND ${CMAKE_COMMAND}
    -DTAVERN_BUILD_DIR=${PROJECT_BINARY_DIR}
    -DTAVERN_CONSUMER_SOURCE_DIR=${PROJECT_SOURCE_DIR}/examples/installed_consumer
    -DTAVERN_WORK_DIR=${PROJECT_BINARY_DIR}/installed-consumer-test
    -DTAVERN_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DTAVERN_CONSUMER_CXX_FLAGS=${CMAKE_CXX_FLAGS}
    -DTAVERN_CONSUMER_CXX_FLAGS_CONFIG=${CMAKE_CXX_FLAGS_${TAVERN_BUILD_TYPE_UPPER}}
    -DTAVERN_CONSUMER_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/installed_consumer_test.cmake)
