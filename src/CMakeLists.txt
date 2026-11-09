add_library(tavern
  page_store.cpp
  directory.cpp
  adjacency_pages.cpp
  adjacency_tree.cpp
  persistent_relationship.cpp
  commit.cpp
  database.cpp
  protected_roots.cpp
  reachability.cpp
  free_page_pool.cpp
  page_allocation_session.cpp
)
# This is the sole installed library target and exposes only public headers.
target_include_directories(tavern PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(tavern PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(tavern PUBLIC cxx_std_23)

add_library(tavern_test_support STATIC
  tavern.cpp
  database_reclamation_test_support.cpp
  page_liveness_index.cpp
)
# Non-installed support retains reclamation reference code needed by tests and
# benchmarks; it is intentionally outside the consumer-facing Tavern package.
target_include_directories(tavern_test_support PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tavern_test_support PUBLIC tavern)
target_compile_features(tavern_test_support PRIVATE cxx_std_23)

install(TARGETS tavern
  EXPORT TavernTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/tavern
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
