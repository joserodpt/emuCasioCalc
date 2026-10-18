cmake_minimum_required(VERSION 3.20)
project(emuCasioFxCG C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra)

find_package(SDL3 REQUIRED CONFIG)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Source layout:
#   src/engine     the emulated machine (CPU, memory, SH7305, display controller)
#   src/engine/os  high-level emulation of the Casio OS (syscalls)
#   src/views      SDL window, menu bar, add-in info window
#   src/utils      g3a loader, calculator key table

# The hardware core has no SDL, so the CPU tests stay headless.
add_library(emucore STATIC
  src/engine/cpu.c src/engine/mem.c src/engine/sh7305.c src/engine/r61524.c)
target_include_directories(emucore PUBLIC src/engine)

add_executable(emucasiofxcg
  src/main.c
  src/engine/emu.c
  src/engine/os/syscalls.c src/engine/os/syscalls_text.c src/engine/os/syscalls_keys.c
  src/engine/os/syscalls_mcs.c src/engine/os/syscalls_bfile.c src/engine/os/syscalls_dialogs.c src/engine/os/syscalls_draw.c
  src/engine/os/display.c
  src/views/window.c src/views/menubar.c src/views/info_window.c src/views/font.c
  src/views/app_icon.c
  src/utils/g3a.c src/utils/keys.c)
target_include_directories(emucasiofxcg PRIVATE src/engine/os src/views src/utils)
target_link_libraries(emucasiofxcg PRIVATE emucore SDL3::SDL3 m)

# Hand-assembled smoke-test add-in.
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/hello.g3a
  COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/make_hello_g3a.py ${CMAKE_BINARY_DIR}/hello.g3a
  DEPENDS tools/make_hello_g3a.py)
add_custom_target(hello_g3a ALL DEPENDS ${CMAKE_BINARY_DIR}/hello.g3a)

enable_testing()
add_executable(test_cpu tests/test_cpu.c)
target_link_libraries(test_cpu PRIVATE emucore)
add_test(NAME cpu COMMAND test_cpu)
add_test(NAME hello
  COMMAND emucasiofxcg --headless --keys EXE,EXIT --screenshot hello.bmp hello.g3a
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# No build date, no old addresses: detected as an fx-CG50 add-in; also runs as fx-CG10/20.
add_test(NAME hello_model
  COMMAND emucasiofxcg --headless --keys EXIT hello.g3a
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(hello_model PROPERTIES PASS_REGULAR_EXPRESSION "running as fx-CG50 \\(no build date")
add_test(NAME hello_cg20
  COMMAND emucasiofxcg --headless --model cg20 --keys EXE,EXIT hello.g3a
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(hello_cg20 PROPERTIES PASS_REGULAR_EXPRESSION "running as fx-CG10/20.*add-in exited")

# gint add-ins (tests/gint-*), built with the fxSDK when it is installed
# (fxsdk on the PATH, or -DFXSDK=/path/to/bin/fxsdk).
find_program(FXSDK fxsdk HINTS $ENV{HOME}/.local/bin)
if(FXSDK)
  get_filename_component(FXSDK_BIN "${FXSDK}" DIRECTORY)
  get_filename_component(FXSDK_CMAKE "${FXSDK_BIN}/../lib/cmake/fxsdk" ABSOLUTE)
  set(FXSDK_ENV ${CMAKE_COMMAND} -E env "PATH=${FXSDK_BIN}:$ENV{PATH}")

  function(gint_addin name g3a)
    set(src ${CMAKE_SOURCE_DIR}/tests/${name})
    set(bin ${CMAKE_BINARY_DIR}/${name})
    add_custom_command(
      OUTPUT ${bin}/${g3a}
      COMMAND ${FXSDK_ENV} ${CMAKE_COMMAND} -S ${src} -B ${bin}
              -DCMAKE_MODULE_PATH=${FXSDK_CMAKE}
              -DCMAKE_TOOLCHAIN_FILE=${FXSDK_CMAKE}/FXCG50.cmake
              -DFXSDK_CMAKE_MODULE_PATH=${FXSDK_CMAKE}
      COMMAND ${FXSDK_ENV} ${CMAKE_COMMAND} --build ${bin}
      DEPENDS ${src}/CMakeLists.txt ${src}/src/main.c
      COMMENT "Building ${g3a} with the fxSDK")
    add_custom_target(${name} ALL DEPENDS ${bin}/${g3a})
  endfunction()
  gint_addin(gint-hello GintHello.g3a)
  gint_addin(gint-stress GintStress.g3a)
  gint_addin(gint-files GintFiles.g3a)

  add_test(NAME gint_hello
    COMMAND emucasiofxcg --headless --keys EXE,MENU,7,EXIT --screenshot gint-hello.bmp gint-hello/GintHello.g3a
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(gint_hello PROPERTIES PASS_REGULAR_EXPRESSION "running as fx-CG50.*add-in exited")
  # gint's fx-CG10/20 code path (other RAM layout, half the clock).
  add_test(NAME gint_hello_cg20
    COMMAND emucasiofxcg --headless --model cg20 --keys EXE,MENU,7,EXIT gint-hello/GintHello.g3a
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(gint_hello_cg20 PROPERTIES PASS_REGULAR_EXPRESSION "running as fx-CG10/20.*add-in exited")
  add_test(NAME gint_stress
    COMMAND emucasiofxcg --headless --keys EXE --screenshot gint-stress.bmp gint-stress/GintStress.g3a
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(gint_stress PROPERTIES PASS_REGULAR_EXPRESSION "add-in exited")
  # F1 makes a misaligned write: gint must catch it and show its panic screen.
  add_test(NAME gint_panic
    COMMAND emucasiofxcg --headless --keys SHIFT,SHIFT,SHIFT,SHIFT,SHIFT,SHIFT,SHIFT,SHIFT,F1
            --screenshot gint-panic.bmp gint-stress/GintStress.g3a
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  # Bfile_* syscalls through gint's world switch; leaves storage/fls0/EMUFILES.txt.
  add_test(NAME gint_files
    COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_BINARY_DIR}/gint-files
            sh -c "rm -rf storage && $<TARGET_FILE:emucasiofxcg> --headless --keys EXE GintFiles.g3a && grep -q ok storage/fls0/EMUFILES.txt")
  set_tests_properties(gint_panic PROPERTIES PASS_REGULAR_EXPRESSION "misaligned write at 0x08100001.*key script finished")
else()
  message(STATUS "fxSDK not found: gint test add-ins will not be built")
endif()
