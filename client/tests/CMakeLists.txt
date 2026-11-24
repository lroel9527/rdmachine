add_executable(rdm_client_tests test_upscaler.mm test_window_upscale.mm test_stats_fields.mm
  ../remote_window.mm ../client_support.mm ../input_source_icon.mm)
target_compile_options(rdm_client_tests PRIVATE -fobjc-arc)
target_include_directories(rdm_client_tests PRIVATE ${PROJECT_SOURCE_DIR}/client)
target_link_libraries(rdm_client_tests PRIVATE rdm_upscaler rdmachine::session GTest::gtest_main
  "-framework AppKit" "-framework Carbon" "-framework CoreGraphics" "-framework CoreText" "-framework IOSurface"
  "-framework QuartzCore")
include(GoogleTest)
# Needs a Metal device (any Mac this builds on has one); the tests skip without.
# The window tests make a window but never put it on screen.
gtest_discover_tests(rdm_client_tests DISCOVERY_TIMEOUT 30)
