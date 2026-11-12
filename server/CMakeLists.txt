# Server. On macOS a menu bar app (status, pairing codes, settings, a
# statistics panel for debugging); --headless and Linux: command line only.
add_executable(rdmachine-server main.cpp server_runtime.cpp)
target_link_libraries(rdmachine-server PRIVATE rdmachine::session)
if(APPLE)
  target_sources(rdmachine-server PRIVATE mac_app.mm)
  set_source_files_properties(mac_app.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
  target_link_libraries(rdmachine-server PRIVATE "-framework CoreFoundation" "-framework AppKit"
                        "-framework ApplicationServices" "-framework CoreGraphics")
  # An empty __CGPreLoginApp,__cgpreloginapp section marks a program as one
  # that runs at the login window (undocumented; Chrome Remote Desktop's
  # login window agent has it too). Without it the window server takes the
  # login window agent's posted events, and the pointer moves, but the login
  # window ignores the clicks and keys (docs/decisions.md, "登录窗口的输入").
  target_link_options(rdmachine-server PRIVATE "LINKER:-sectcreate,__CGPreLoginApp,__cgpreloginapp,/dev/null")
endif()
