# gint stress test for emuCasioFxCG: CPU-heavy rendering, DSP repeat loops
# (image blits), timers, and a deliberate exception for the panic screen.
#   fxsdk build-cg        (from this folder; output: build-cg/GintStress.g3a)
# The emuCasioFxCG build also builds it when the fxSDK is installed.

cmake_minimum_required(VERSION 3.15)
project(GintStress)

include(GenerateG3A)
include(Fxconv)
find_package(Gint 2.9 REQUIRED)

fxconv_declare_assets(assets-cg/sprite.png WITH_METADATA)

add_executable(gintstress src/main.c assets-cg/sprite.png)
target_compile_options(gintstress PRIVATE -Wall -Wextra -Os)
target_link_libraries(gintstress Gint::Gint)

generate_g3a(TARGET gintstress OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/GintStress.g3a"
  NAME "GintStress" ICONS assets-cg/icon-uns.png assets-cg/icon-sel.png)
