# Platform-independent product core: control protocol, TLS, auth and session
# logic. No rdaccel or OS framework dependency, so Linux can reuse it and it
# builds and tests on Linux too.
add_library(rdm_core STATIC
  src/base64.cpp
  src/frame.cpp
  src/messages.cpp
  src/random.cpp
  src/auth_guards.cpp
  src/tls.cpp
  src/channel.cpp
  src/server_session.cpp
  src/control_server.cpp
  src/control_client.cpp
  src/clipboard_sync.cpp
  src/clipboard_wire.cpp
  src/clipboard_files.cpp
  src/transfer_pacer.cpp
  src/input_source_sync.cpp
  src/desktop.cpp
  src/picture_mode.cpp
  src/session_stats.cpp)
add_library(rdmachine::core ALIAS rdm_core)
target_include_directories(rdm_core PUBLIC include)
target_link_libraries(rdm_core
  PUBLIC nlohmann_json::nlohmann_json
  PRIVATE mbedtls mbedx509 mbedcrypto)
find_package(Threads REQUIRED)
target_link_libraries(rdm_core PUBLIC Threads::Threads)
target_compile_definitions(rdm_core PUBLIC RDM_VERSION_STRING="${PROJECT_VERSION}")
