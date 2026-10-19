# gint file system test for emuCasioCalc (Bfile_* syscalls through the world switch). Build with the fxSDK:
#   fxsdk build-cg        (from this folder; output: build-cg/GintFiles.g3a)
#   fxsdk build-fx        (the same for the fx-9860G: build-fx/GintFiles.g1a)
# The emuCasioCalc build also builds it when the fxSDK is installed.

cmake_minimum_required(VERSION 3.15)
project(GintFiles)

include(GenerateG1A)
include(GenerateG3A)
find_package(Gint 2.9 REQUIRED)

add_executable(gintfiles src/main.c)
target_compile_options(gintfiles PRIVATE -Wall -Wextra -Os)
target_link_libraries(gintfiles Gint::Gint)

if("${FXSDK_PLATFORM_LONG}" STREQUAL fx9860G)
  generate_g1a(TARGET gintfiles OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/GintFiles.g1a"
    NAME "GintFiles" ICON assets-fx/icon.png)
else()
  generate_g3a(TARGET gintfiles OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/GintFiles.g3a"
    NAME "GintFiles" ICONS assets-cg/icon-uns.png assets-cg/icon-sel.png)
endif()
