# gint test add-in built for both families from one source (emuCasioFxCG):
#   fxsdk build-fx        -> build-fx/GintDual.g1a (fx-9860G, monochrome)
#   fxsdk build-cg        -> build-cg/GintDual.g3a (fx-CG, colour)
# The emuCasioFxCG build builds both when the fxSDK has gint for them.

cmake_minimum_required(VERSION 3.15)
project(GintDual)

include(GenerateG1A)
include(GenerateG3A)
find_package(Gint 2.9 REQUIRED)

add_executable(gintdual src/main.c)
target_compile_options(gintdual PRIVATE -Wall -Wextra -Os)
target_link_libraries(gintdual Gint::Gint)

if("${FXSDK_PLATFORM_LONG}" STREQUAL fx9860G)
  generate_g1a(TARGET gintdual OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/GintDual.g1a"
    NAME "GintDual" ICON assets-fx/icon.png)
else()
  generate_g3a(TARGET gintdual OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/GintDual.g3a"
    NAME "GintDual" ICONS assets-cg/icon-uns.png assets-cg/icon-sel.png)
endif()
