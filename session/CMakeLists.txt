# Session library: everything between the protocol core and a user interface
# that needs rdaccel or macOS (PAM login, the rdaccel host and viewer, the
# server identity and the client's known hosts).
add_library(rdm_session STATIC
  src/config_store.cpp
  src/live_stats.cpp
  src/server_settings.cpp
  src/known_hosts.cpp
  src/system_auth.cpp
  src/media_host.cpp
  src/viewer.cpp
  src/remote_session.cpp
  src/daemon.cpp
  src/agent_host.cpp
  src/ime_bridge.cpp src/ime_state_tracker.cpp src/ime_key_judge.cpp src/input_icon_cache.cpp)
target_sources(rdm_session PRIVATE src/system_clipboard_mac.mm src/virtual_desktop_mac.mm src/login_screen_mac.mm src/code_identity_mac.mm
  src/file_target_mac.mm src/ime_bridge_mac.mm src/client_input_icon_mac.mm)
set_source_files_properties(src/system_clipboard_mac.mm src/virtual_desktop_mac.mm src/login_screen_mac.mm src/code_identity_mac.mm
  src/file_target_mac.mm src/ime_bridge_mac.mm src/client_input_icon_mac.mm
  PROPERTIES COMPILE_OPTIONS -fobjc-arc)
# The XPC protocol shared with the File Provider extension.
target_include_directories(rdm_session PRIVATE ${PROJECT_SOURCE_DIR}/fileprovider)
target_link_libraries(rdm_session PUBLIC "-framework AppKit" "-framework CoreGraphics" "-framework Security"
  "-framework FileProvider" "-framework IOKit" "-framework Carbon")
add_library(rdmachine::session ALIAS rdm_session)
target_include_directories(rdm_session PUBLIC include)
target_link_libraries(rdm_session PUBLIC rdmachine::core rdaccel::rdaccel PRIVATE pam)
