# gint test add-in for the monochrome fx-9860G models (emuCasioFxCG). Build
# with the fxSDK:
#   fxsdk build-fx        (from this folder; output: build-fx/GintMono.g1a)
# The emuCasioFxCG build also builds it when the fxSDK has gint for fx-9860G.

cmake_minimum_required(VERSION 3.15)
project(GintMono)

include(GenerateG1A)
find_package(Gint 2.9 REQUIRED)

add_executable(gintmono src/main.c)
target_compile_options(gintmono PRIVATE -Wall -Wextra -Os)
target_link_libraries(gintmono Gint::Gint)

generate_g1a(TARGET gintmono OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/GintMono.g1a"
  NAME "GintMono" ICON assets-fx/icon.png)
