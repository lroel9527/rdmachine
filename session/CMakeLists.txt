# Session library: everything between the protocol core and a user interface
# that needs rdaccel or the OS (PAM login, the rdaccel host and viewer, the
# server identity and the client's known hosts). Plain C++ on macOS and Linux,
# so the Linux CI can test it end to end against an X server (Xvfb).
add_library(rdm_session STATIC
  src/config_store.cpp
  src/known_hosts.cpp
  src/system_auth.cpp
  src/media_host.cpp
  src/viewer.cpp
  src/remote_session.cpp)
add_library(rdmachine::session ALIAS rdm_session)
target_include_directories(rdm_session PUBLIC include)
target_link_libraries(rdm_session PUBLIC rdmachine::core rdaccel::rdaccel PRIVATE pam)
