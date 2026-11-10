find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

add_executable(rdm_core_tests
  test_base64.cpp
  test_frame.cpp
  test_messages.cpp
  test_auth_guards.cpp
  test_server_session.cpp
  test_control.cpp
  test_clipboard_sync.cpp
  test_input_source_sync.cpp
  test_desktop.cpp
  test_picture_mode.cpp
  test_session_stats.cpp)
target_link_libraries(rdm_core_tests PRIVATE rdmachine::core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(rdm_core_tests)
