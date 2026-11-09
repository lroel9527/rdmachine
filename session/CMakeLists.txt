# Session library: everything between the protocol core and a user interface
# that needs rdaccel or the OS (PAM login, the rdaccel host and viewer, the
# server identity and the client's known hosts). Plain C++ on macOS and Linux,
# so it can also be tested end to end on Linux against an X server (Xvfb).
add_library(rdm_session STATIC
  src/config_store.cpp
  src/live_stats.cpp
  src/server_settings.cpp
  src/known_hosts.cpp
  src/system_auth.cpp
  src/media_host.cpp
  src/viewer.cpp
  src/remote_session.cpp)
if(APPLE)
  target_sources(rdm_session PRIVATE src/system_clipboard_mac.mm)
  set_source_files_properties(src/system_clipboard_mac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
  target_link_libraries(rdm_session PUBLIC "-framework AppKit")
else()
  target_sources(rdm_session PRIVATE src/system_clipboard.cpp)
endif()
add_library(rdmachine::session ALIAS rdm_session)
target_include_directories(rdm_session PUBLIC include)
target_link_libraries(rdm_session PUBLIC rdmachine::core rdaccel::rdaccel PRIVATE pam)
