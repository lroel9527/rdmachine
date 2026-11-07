add_executable(rdm_session_tests
  test_storage.cpp
  test_media_e2e.cpp)
if(APPLE)
  # The real NSPasteboard; the macOS CI runner has a pasteboard server.
  target_sources(rdm_session_tests PRIVATE test_system_clipboard_mac.mm)
  set_source_files_properties(test_system_clipboard_mac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
endif()
target_link_libraries(rdm_session_tests PRIVATE rdmachine::session GTest::gtest_main)
target_include_directories(rdm_session_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests)
include(GoogleTest)
gtest_discover_tests(rdm_session_tests DISCOVERY_TIMEOUT 30)
