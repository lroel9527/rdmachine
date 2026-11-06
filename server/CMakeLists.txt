# Server (phase 1: command line on macOS; also builds on Linux for tests).
# The menu bar app around it comes with M1's login-window work.
add_executable(rdmachine-server main.cpp)
target_link_libraries(rdmachine-server PRIVATE rdmachine::session)
if(APPLE)
  target_link_libraries(rdmachine-server PRIVATE "-framework CoreFoundation")
endif()
