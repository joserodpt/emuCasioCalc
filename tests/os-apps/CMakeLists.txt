# OS syscall test add-ins for emuCasioFxCG: each calls a group of syscalls
# from the OS world (gint_world_switch), like a libfxcg add-in, checks the
# results and leaves "ok" (or the first failing check) in a file of
# \\fls0\. Build with the fxSDK:
#   fxsdk build-cg        (from this folder; output: build-cg/*.g3a)
# The emuCasioFxCG build also builds them when the fxSDK is installed, and
# CTest runs them (see ../../CMakeLists.txt for the keys each one needs).

cmake_minimum_required(VERSION 3.15)
project(OsApps)

include(GenerateG3A)
find_package(Gint 2.9 REQUIRED)

# os_app(<source name> <g3a name>)
function(os_app src name)
  add_executable(${src} src/${src}.c src/rows.c)
  target_compile_options(${src} PRIVATE -Wall -Wextra -Os)
  target_link_libraries(${src} Gint::Gint)
  generate_g3a(TARGET ${src} OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${name}.g3a"
    NAME "${name}" ICONS assets-cg/icon-uns.png assets-cg/icon-sel.png)
endfunction()

os_app(os-syscalls OsSyscalls)
os_app(draw-syscalls DrawSyscalls)
os_app(key-modifiers KeyModifiers)
os_app(status-bar StatusBar)
os_app(file-syscalls FileSyscalls)
os_app(time-keys TimeKeys)
os_app(language Language)
os_app(text-syscalls TextSyscalls)
os_app(mcs-system McsSystem)
