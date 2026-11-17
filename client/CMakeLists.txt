# Command-line test client: log in, receive pictures, quit.
add_executable(rdmachine-connect connect_main.cpp)
target_link_libraries(rdmachine-connect PRIVATE rdmachine::session)

# Mac client: native AppKit app built on rdaccel's viewer.
add_executable(rdmachine-client main.mm)
target_compile_options(rdmachine-client PRIVATE -fobjc-arc)
target_link_libraries(rdmachine-client PRIVATE rdmachine::session
  "-framework AppKit" "-framework IOSurface" "-framework QuartzCore")
