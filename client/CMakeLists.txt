# Mac client (phase 1): native AppKit app built on rdaccel's viewer.
# M0 PR 1 only proves the build links; the connection window and the canvas
# (extended from rdaccel's examples/remote_mac) come next.
add_executable(rdmachine-client main.mm)
target_link_libraries(rdmachine-client PRIVATE rdmachine::core rdaccel::rdaccel "-framework AppKit")
