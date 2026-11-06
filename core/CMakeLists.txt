# Platform-independent product core: control protocol, (later) auth and
# session logic. No rdaccel or OS dependency, so Linux can reuse it.
add_library(rdm_core STATIC
  src/base64.cpp
  src/frame.cpp
  src/messages.cpp)
add_library(rdmachine::core ALIAS rdm_core)
target_include_directories(rdm_core PUBLIC include)
target_link_libraries(rdm_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_definitions(rdm_core PUBLIC RDM_VERSION_STRING="${PROJECT_VERSION}")
