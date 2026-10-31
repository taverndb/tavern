cmake_minimum_required(VERSION 3.20)
project(tavern_installed_consumer LANGUAGES CXX)

find_package(Tavern CONFIG REQUIRED)

add_executable(tavern_installed_consumer main.cpp)
target_link_libraries(tavern_installed_consumer PRIVATE Tavern::tavern)
target_compile_features(tavern_installed_consumer PRIVATE cxx_std_23)
