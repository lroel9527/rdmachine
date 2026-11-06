# Mac server (phase 1): Objective-C++ shell around rdmachine::core and rdaccel.
# M0 PR 1 only proves the build links; the TLS listener and rdaccel host come next.
add_executable(rdmachine-server main.mm)
target_link_libraries(rdmachine-server PRIVATE rdmachine::core rdaccel::rdaccel "-framework Foundation")
