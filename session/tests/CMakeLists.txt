add_executable(rdm_session_tests
  test_storage.cpp
  test_live_stats.cpp
  test_media_e2e.cpp)
if(APPLE)
  # The real NSPasteboard; needs a logged-in session (a pasteboard server).
  # The real virtual display (RDM_TEST_VIRTUAL_DESKTOP=1: it rearranges the displays).
  target_sources(rdm_session_tests PRIVATE test_system_clipboard_mac.mm test_virtual_desktop_mac.mm)
  set_source_files_properties(test_system_clipboard_mac.mm test_virtual_desktop_mac.mm
    PROPERTIES COMPILE_OPTIONS -fobjc-arc)
endif()
target_link_libraries(rdm_session_tests PRIVATE rdmachine::session GTest::gtest_main)
target_include_directories(rdm_session_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests)
include(GoogleTest)
# One at a time: the end-to-end tests capture the real screen (two
# ScreenCaptureKit streams starting at once can hang on macOS) and the Mac
# clipboard tests share the system pasteboard.
gtest_discover_tests(rdm_session_tests DISCOVERY_TIMEOUT 30 PROPERTIES RESOURCE_LOCK rdm_screen_and_pasteboard)
