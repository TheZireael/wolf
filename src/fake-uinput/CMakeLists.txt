cmake_minimum_required(VERSION 3.16)

# Broker daemon: listens on a Unix socket for MKNOD/REMOVE commands from the
# LD_PRELOAD interceptor library and creates /dev/input/ device nodes.
# Runs as root inside app containers (started via docker exec).
add_executable(fake-uinput-broker fake-uinput-broker.cpp)
target_compile_features(fake-uinput-broker PRIVATE cxx_std_17)
target_link_libraries(fake-uinput-broker -static)
install(TARGETS fake-uinput-broker RUNTIME DESTINATION bin)

# LD_PRELOAD interceptor library: intercepts ioctl(UI_DEV_CREATE / UI_DEV_DESTROY)
# and delegates device node creation to the broker via the Unix socket.
add_library(fake_uinput SHARED fake-uinput.cpp)
target_compile_features(fake_uinput PRIVATE cxx_std_17)
target_link_libraries(fake_uinput PRIVATE dl)
set_target_properties(fake_uinput PROPERTIES OUTPUT_NAME "fake-uinput")
install(TARGETS fake_uinput LIBRARY DESTINATION lib)

# 32-bit interceptor library for Steam's 32-bit processes.
# Static-links libstdc++/libgcc so it works without 32-bit runtime libs in the container.
add_library(fake_uinput_32 SHARED fake-uinput.cpp)
target_compile_features(fake_uinput_32 PRIVATE cxx_std_17)
target_compile_options(fake_uinput_32 PRIVATE -m32)
target_link_options(fake_uinput_32 PRIVATE -m32 -static-libstdc++ -static-libgcc)
target_link_libraries(fake_uinput_32 PRIVATE dl)
set_target_properties(fake_uinput_32 PROPERTIES OUTPUT_NAME "fake-uinput32")
install(TARGETS fake_uinput_32 LIBRARY DESTINATION lib)
