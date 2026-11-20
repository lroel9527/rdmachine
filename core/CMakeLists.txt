# Product core: control protocol, TLS, auth and session logic. No rdaccel or
# OS framework dependency, so it is tested without a screen.
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
  src/file_transfer.cpp
  src/transfer_pacer.cpp
  src/input_source_sync.cpp
  src/desktop.cpp
  src/picture_mode.cpp
  src/ime_wire.cpp
  src/audio_control.cpp
  src/session_stats.cpp
  src/login_keys.cpp
  src/agent_link.cpp)
add_library(rdmachine::core ALIAS rdm_core)
target_include_directories(rdm_core PUBLIC include)
target_link_libraries(rdm_core
  PUBLIC nlohmann_json::nlohmann_json
  PRIVATE mbedtls mbedx509 mbedcrypto libzstd_static)
target_include_directories(rdm_core PRIVATE ${zstd_SOURCE_DIR}/lib)
find_package(Threads REQUIRED)
target_link_libraries(rdm_core PUBLIC Threads::Threads)
target_compile_definitions(rdm_core PUBLIC RDM_VERSION_STRING="${PROJECT_VERSION}")
