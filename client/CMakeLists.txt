# Command-line test client: log in, receive pictures, quit.
add_executable(rdmachine-connect connect_main.cpp input_source_icon.mm)
set_source_files_properties(input_source_icon.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
target_link_libraries(rdmachine-connect PRIVATE rdmachine::session "-framework AppKit" "-framework Carbon")

# Mac client: native AppKit app built on rdaccel's viewer.
add_executable(rdmachine-client main.mm app_delegate.mm remote_window.mm client_support.mm input_source_icon.mm)
target_compile_options(rdmachine-client PRIVATE -fobjc-arc)
target_link_libraries(rdmachine-client PRIVATE rdmachine::session
  "-framework AppKit" "-framework IOSurface" "-framework QuartzCore")
