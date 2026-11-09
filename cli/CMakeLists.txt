add_executable(tavern-cli main.cpp)
# The CLI intentionally has no Tavern library link dependency: it speaks TCP
# Text Protocol v1 only.
target_compile_features(tavern-cli PRIVATE cxx_std_23)
install(TARGETS tavern-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
