cmake_minimum_required(VERSION 3.20)
project(tavern VERSION 0.1.0
  DESCRIPTION "A durable relationship graph database"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(TAVERN_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Werror)
add_compile_options(${TAVERN_WARNINGS})

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

enable_testing()
# `tavern` is the installable library; server and CLI remain separate programs.
# Tests and benchmarks are configured here but are not exported or installed.
add_subdirectory(src)
add_subdirectory(server)
add_subdirectory(cli)
add_subdirectory(benchmarks)
add_subdirectory(tests)

install(EXPORT TavernTargets
  FILE TavernTargets.cmake
  NAMESPACE Tavern::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Tavern)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TavernConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/TavernConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Tavern)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/TavernConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/TavernConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/TavernConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Tavern)
