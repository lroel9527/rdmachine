add_executable(rdm_session_tests
  test_storage.cpp
  test_media_e2e.cpp)
target_link_libraries(rdm_session_tests PRIVATE rdmachine::session GTest::gtest_main)
target_include_directories(rdm_session_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests)
include(GoogleTest)
gtest_discover_tests(rdm_session_tests DISCOVERY_TIMEOUT 30)
