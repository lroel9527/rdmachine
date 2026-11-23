# The text field never goes on screen.
add_executable(rdm_ime_tests test_composition_anchor.mm ../composition_anchor.mm)
target_compile_options(rdm_ime_tests PRIVATE -fobjc-arc)
target_include_directories(rdm_ime_tests PRIVATE ${PROJECT_SOURCE_DIR}/ime)
target_link_libraries(rdm_ime_tests PRIVATE GTest::gtest_main
  "-framework AppKit" "-framework Carbon" "-framework InputMethodKit")
include(GoogleTest)
gtest_discover_tests(rdm_ime_tests DISCOVERY_TIMEOUT 30)
