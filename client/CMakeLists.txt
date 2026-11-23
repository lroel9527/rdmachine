# Command-line test client: log in, receive pictures, quit.
add_executable(rdmachine-connect connect_main.cpp input_source_icon.mm)
set_source_files_properties(input_source_icon.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
target_link_libraries(rdmachine-connect PRIVATE rdmachine::session "-framework AppKit" "-framework Carbon")

# Enlarging a 1x picture 2x for a Retina screen (docs/upscaler.md): the text
# model trained by tools/upscaler/train.py, and MetalFX.
add_library(rdm_upscaler STATIC picture_upscaler.mm upscaler_model.cpp)
set_source_files_properties(picture_upscaler.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
target_include_directories(rdm_upscaler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rdm_upscaler PUBLIC rdaccel::rdaccel
  "-framework Foundation" "-framework IOSurface" "-framework Metal" "-framework MetalFX")

# Mac client: native AppKit app built on rdaccel's viewer.
add_executable(rdmachine-client main.mm app_delegate.mm remote_window.mm client_support.mm input_source_icon.mm)
target_compile_options(rdmachine-client PRIVATE -fobjc-arc)
target_link_libraries(rdmachine-client PRIVATE rdmachine::session rdm_upscaler
  "-framework AppKit" "-framework IOSurface" "-framework QuartzCore")
