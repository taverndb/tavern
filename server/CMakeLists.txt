add_library(tavern_proto STATIC protocol.cpp)
# Protocol code is a build-private library shared by the server and dispatcher
# tests; it is not exported as part of the embedded database package.
target_include_directories(tavern_proto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tavern_proto PUBLIC tavern)
target_compile_features(tavern_proto PRIVATE cxx_std_23)

add_library(tavern_server OBJECT server.cpp)
# Keep event-loop implementation separately compilable for the executable while
# avoiding an additional installed library boundary.
target_include_directories(tavern_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tavern_server PRIVATE tavern_proto tavern)
target_compile_features(tavern_server PRIVATE cxx_std_23)

add_executable(tavern-server main.cpp $<TARGET_OBJECTS:tavern_server>)
target_link_libraries(tavern-server PRIVATE tavern_proto tavern)
install(TARGETS tavern-server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
