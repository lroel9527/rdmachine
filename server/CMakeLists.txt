# Server. On macOS a menu bar app (status, pairing codes, settings, a
# statistics panel for debugging); --headless and Linux: command line only.
add_executable(rdmachine-server main.cpp server_runtime.cpp)
target_link_libraries(rdmachine-server PRIVATE rdmachine::session)
if(APPLE)
  target_sources(rdmachine-server PRIVATE mac_app.mm)
  set_source_files_properties(mac_app.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
  target_link_libraries(rdmachine-server PRIVATE "-framework CoreFoundation" "-framework AppKit"
                        "-framework ApplicationServices" "-framework CoreGraphics")
endif()
