cmake_minimum_required(VERSION 3.20)
project(cocaine LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_executable(cocaine
  src/main.cpp
  src/log.cpp
  src/mem/guest_memory.cpp
  src/macho/macho.cpp
  src/cpu/ppc/ppc_interp.cpp
  src/cpu/ppc/ppc_jit.cpp
  src/cpu/ppc/altivec.cpp
  src/cpu/x86/x86_interp.cpp
  src/cpu/x86/x86_jit.cpp
  src/cpu/x86/f80.cpp
  src/cpu/x86/x87.cpp
  src/cpu/x86/sse.cpp
  src/kernel/process.cpp
  src/kernel/syscalls.cpp
  src/kernel/sysctl.cpp
  src/kernel/mach.cpp
  src/kernel/commpage.cpp
  src/kernel/paths.cpp
  src/kernel/files.cpp
  src/kernel/sockets.cpp
  src/kernel/ipc.cpp
  src/kernel/iokit.cpp
  src/session/session.cpp
  src/session/art.cpp
  src/viewer/viewer.mm
  src/kernel/threads.cpp
  src/kernel/processes.cpp
  src/kernel/signals.cpp
  src/kernel/protect.cpp
  src/kernel/hooks.cpp
  src/kernel/wallpaper.cpp
  src/gl/gl.cpp
  src/al/al.cpp
  src/audio/coreaudio.cpp
  src/audio/midi.cpp
)

target_include_directories(cocaine PRIVATE src)
target_compile_options(cocaine PRIVATE
  -Wall -Wextra -Wno-unused-parameter
  -fno-exceptions -fno-rtti -fno-strict-aliasing
)

# The CPU cores do guest floating point with host floating point, so the
# compiler must not fuse multiply-adds or assume the default rounding mode.
set_source_files_properties(
  src/cpu/ppc/ppc_interp.cpp
  src/cpu/ppc/altivec.cpp
  src/cpu/x86/x86_interp.cpp
  src/cpu/x86/x87.cpp
  src/cpu/x86/sse.cpp
  PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-frounding-math")

set_source_files_properties(src/viewer/viewer.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(cocaine PRIVATE "-framework CoreFoundation" "-framework IOKit" "-framework Cocoa" "-framework QuartzCore" "-framework CoreGraphics" "-framework ImageIO" "-framework OpenGL" "-framework OpenAL" "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreAudio")

enable_testing()
add_executable(display-pixels-test tests/display-pixels.cpp)
target_include_directories(display-pixels-test PRIVATE src)
add_test(NAME display-pixels COMMAND display-pixels-test)
add_executable(viewer-resize-test tests/viewer-resize.mm)
target_include_directories(viewer-resize-test PRIVATE src)
set_source_files_properties(tests/viewer-resize.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(viewer-resize-test PRIVATE "-framework Cocoa" "-framework QuartzCore" "-framework CoreGraphics")
add_test(NAME viewer-resize COMMAND viewer-resize-test)
set_tests_properties(viewer-resize PROPERTIES SKIP_RETURN_CODE 77)
# The ARM64 instruction encodings the PowerPC translator emits, against the
# system's assembler.
add_test(NAME arm64-encodings
  COMMAND sh ${CMAKE_SOURCE_DIR}/tests/a64-check.sh ${CMAKE_BINARY_DIR}/a64-work)
add_test(NAME guest-programs
  COMMAND sh ${CMAKE_SOURCE_DIR}/tests/run.sh $<TARGET_FILE:cocaine> ${CMAKE_BINARY_DIR}/test-work)
