cmake_minimum_required(VERSION 3.20)
project(emuCasioFxCG VERSION 1.0 LANGUAGES C)

# Shown in Help > About: the version, and (build_info.c, written at each
# build) the build's date and time and git commit.
add_compile_definitions(EMU_VERSION="${PROJECT_VERSION}")

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

# The emulator's UI font (assets/fonts/Inter-Medium.ttf), embedded as C.
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/ui_font_ttf.c
  COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_SOURCE_DIR}/assets/fonts/Inter-Medium.ttf
          -DOUTPUT=${CMAKE_BINARY_DIR}/ui_font_ttf.c -DSYMBOL=UI_FONT_TTF -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS assets/fonts/Inter-Medium.ttf cmake/embed.cmake
  COMMENT "Embedding the UI font")

add_custom_target(build_info
  COMMAND ${CMAKE_COMMAND} -DOUTPUT=${CMAKE_BINARY_DIR}/build_info.c -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
          -P ${CMAKE_SOURCE_DIR}/cmake/build_info.cmake
  BYPRODUCTS ${CMAKE_BINARY_DIR}/build_info.c
  COMMENT "Recording the build's date and commit")

add_executable(emucasiofxcg
  src/main.c
  ${CMAKE_BINARY_DIR}/build_info.c
  src/views/ui_font.c ${CMAKE_BINARY_DIR}/ui_font_ttf.c
  src/engine/emu.c
  src/engine/os/syscalls.c src/engine/os/syscalls_text.c src/engine/os/syscalls_keys.c
  src/engine/os/syscalls_mcs.c src/engine/os/syscalls_bfile.c src/engine/os/syscalls_dialogs.c src/engine/os/syscalls_draw.c src/engine/os/syscalls_status.c src/engine/os/syscalls_time.c src/engine/os/messages.c src/engine/os/syscalls_system.c src/engine/os/font_data.c
  src/engine/os/display.c src/engine/os/casio_font.c
  src/views/window.c src/views/menubar.c src/views/info_window.c src/views/send_addin.c src/views/keypad.c src/views/about_window.c src/views/desktop.c src/views/font.c
  src/views/app_icon.c
  src/utils/g3a.c src/utils/keys.c src/utils/png.c)
target_include_directories(emucasiofxcg PRIVATE src/engine/os src/views src/utils)
target_link_libraries(emucasiofxcg PRIVATE emucore SDL3::SDL3 m)
add_dependencies(emucasiofxcg build_info)

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

  # OS syscall test add-ins (tests/os-apps): one fxSDK project, one .g3a per
  # group of syscalls. Each run starts with empty storage and must leave
  # "ok" in its file of storage/fls0.
  set(os_src ${CMAKE_SOURCE_DIR}/tests/os-apps)
  set(os_bin ${CMAKE_BINARY_DIR}/os-apps)
  set(os_apps OsSyscalls DrawSyscalls KeyModifiers StatusBar FileSyscalls TimeKeys Language TextSyscalls McsSystem)
  set(os_outputs)
  foreach(app ${os_apps})
    list(APPEND os_outputs ${os_bin}/${app}.g3a)
  endforeach()
  file(GLOB os_sources ${os_src}/src/*.c ${os_src}/src/*.h)
  add_custom_command(
    OUTPUT ${os_outputs}
    COMMAND ${FXSDK_ENV} ${CMAKE_COMMAND} -S ${os_src} -B ${os_bin}
            -DCMAKE_MODULE_PATH=${FXSDK_CMAKE}
            -DCMAKE_TOOLCHAIN_FILE=${FXSDK_CMAKE}/FXCG50.cmake
            -DFXSDK_CMAKE_MODULE_PATH=${FXSDK_CMAKE}
    COMMAND ${FXSDK_ENV} ${CMAKE_COMMAND} --build ${os_bin}
    DEPENDS ${os_src}/CMakeLists.txt ${os_sources}
    COMMENT "Building the OS syscall test add-ins with the fxSDK")
  add_custom_target(os-apps ALL DEPENDS ${os_outputs})

  # os_test(<name> <g3a> <file left in fls0> [emulator options...])
  function(os_test name g3a marker)
    set(dir ${CMAKE_BINARY_DIR}/os-apps/run-${name})
    string(REPLACE ";" " " opts "${ARGN}")
    add_test(NAME ${name}
      COMMAND sh -c "rm -rf '${dir}' && mkdir -p '${dir}' && cd '${dir}' && \
                     '$<TARGET_FILE:emucasiofxcg>' --headless ${opts} '${CMAKE_BINARY_DIR}/os-apps/${g3a}.g3a' && \
                     cat storage/fls0/${marker} && grep -q '^ok' storage/fls0/${marker}")
  endfunction()
  os_test(os_system_dialogs OsSyscalls OSTEST.txt --keys DOWN,DOWN,EXE,DOWN,EXE,EXIT,EXIT)
  os_test(os_draw DrawSyscalls DRAWTEST.txt)
  os_test(os_shift_alpha KeyModifiers KEYTEST.txt
          --keys ALPHA,7,SHIFT,EXIT,SHIFT,ALPHA,XOT,LOG,DP,ALPHA,7,SHIFT,SHIFT,7,ALPHA,SQUARE,SHIFT,LOG,ALPHA,EXE,7,SHIFT,DEL,SHIFT,AC,1,NEG,FD,VARS)
  os_test(os_status_bar StatusBar STATTEST.txt --battery low --keys ALPHA,7)
  os_test(os_files FileSyscalls FILETEST.txt)
  os_test(os_files_cg20 FileSyscalls FILETEST.txt --model cg20)
  os_test(os_timers_keyboard TimeKeys TIMETEST.txt --keys 7,MENU)
  os_test(os_timers_keyboard_cg20 TimeKeys TIMETEST.txt --model cg20 --keys 7,MENU)
  os_test(os_language Language LANGTEST.txt --language de --keys DOWN,EXE,EXIT,UP,EXE,F6)
  os_test(os_text TextSyscalls TEXTTEST.txt --keys RIGHT,RIGHT,EXE)
  os_test(os_mcs_system McsSystem MCSTEST.txt)
else()
  message(STATUS "fxSDK not found: gint test add-ins will not be built")
endif()
