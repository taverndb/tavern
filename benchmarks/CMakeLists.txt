add_executable(tavern_benchmark relationship_benchmark.cpp)
# Benchmarks use non-installed test support and are neither product binaries nor
# part of the exported package.
target_link_libraries(tavern_benchmark PRIVATE tavern_test_support)
target_include_directories(tavern_benchmark PRIVATE ${PROJECT_SOURCE_DIR}/src)

add_executable(reclamation_benchmark reclamation_benchmark.cpp)
target_link_libraries(reclamation_benchmark PRIVATE tavern_test_support)
target_include_directories(reclamation_benchmark PRIVATE ${PROJECT_SOURCE_DIR}/src)
