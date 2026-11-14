# The File Provider extension (docs/file-paste.md): packaging/macos/package.sh
# puts it into RDMachine.app and RDMachine Server.app as
# Contents/PlugIns/RDMachine Files.appex and signs it sandboxed.
add_executable(rdmachine-files-ext files_extension.m)
target_compile_options(rdmachine-files-ext PRIVATE -fobjc-arc -fapplication-extension)
target_link_options(rdmachine-files-ext PRIVATE -fapplication-extension "LINKER:-e,_NSExtensionMain")
target_link_libraries(rdmachine-files-ext PRIVATE "-framework Foundation" "-framework FileProvider"
  "-framework UniformTypeIdentifiers")
