# RDMachine Input: the server's input method for typing with the client's
# input method (docs/ime.md). packaging/macos/package.sh puts the binary into
# "RDMachine Input.app" for /Library/Input Methods.
add_executable(rdmachine-ime main.mm)
set_source_files_properties(main.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
target_link_libraries(rdmachine-ime PRIVATE rdmachine::core "-framework Cocoa" "-framework InputMethodKit")
