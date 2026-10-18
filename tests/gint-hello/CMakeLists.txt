# gint smoke test for emuCasioFxCG. Build with the fxSDK:
#   fxsdk build-cg        (from this folder; output: build-cg/GintHello.g3a)
# The emuCasioFxCG build also builds it when the fxSDK is installed.

cmake_minimum_required(VERSION 3.15)
project(GintHello)

include(GenerateG3A)
find_package(Gint 2.9 REQUIRED)

add_executable(ginthello src/main.c)
target_compile_options(ginthello PRIVATE -Wall -Wextra -Os)
target_link_libraries(ginthello Gint::Gint)

generate_g3a(TARGET ginthello OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/GintHello.g3a"
  NAME "GintHello" ICONS assets-cg/icon-uns.png assets-cg/icon-sel.png)
