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

# CPU + memory: no SDL, so the tests stay headless.
add_library(emucore STATIC src/cpu.c src/mem.c src/sh7305.c src/r61524.c)
target_include_directories(emucore PUBLIC src)

add_executable(emucasiofxcg
  src/main.c src/emu.c src/g3a.c src/keys.c src/syscalls.c src/syscalls_text.c
  src/syscalls_keys.c src/syscalls_mcs.c src/display.c src/platform_sdl.c src/app_icon.c)
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

  add_test(NAME gint_hello
    COMMAND emucasiofxcg --headless --keys EXE,MENU,7,EXIT --screenshot gint-hello.bmp gint-hello/GintHello.g3a
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(gint_hello PROPERTIES PASS_REGULAR_EXPRESSION "add-in exited")
  add_test(NAME gint_stress
    COMMAND emucasiofxcg --headless --keys EXE --screenshot gint-stress.bmp gint-stress/GintStress.g3a
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(gint_stress PROPERTIES PASS_REGULAR_EXPRESSION "add-in exited")
  # F1 makes a misaligned write: gint must catch it and show its panic screen.
  add_test(NAME gint_panic
    COMMAND emucasiofxcg --headless --keys F1 --screenshot gint-panic.bmp gint-stress/GintStress.g3a
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(gint_panic PROPERTIES PASS_REGULAR_EXPRESSION "misaligned write at 0x08100001.*key script finished")
else()
  message(STATUS "fxSDK not found: gint test add-ins will not be built")
endif()
