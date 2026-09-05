# The Android build of the engine.
#
# The sources are the engine's own, from the branch this one is merged onto;
# nothing is copied and nothing is written twice. src/main.c is left out because
# it is the desktop window, and jni.c beside this file takes its place: it hands
# Java the buffer of pixels the engine draws into. The engine needs nothing but
# zlib and, for sound, AAudio, both of which Android has.

cmake_minimum_required(VERSION 3.22)
project(catsystem2 C)

set(ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../src)
set(VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party)

file(GLOB ENGINE_SOURCES CONFIGURE_DEPENDS ${ENGINE_DIR}/*.c)
list(REMOVE_ITEM ENGINE_SOURCES ${ENGINE_DIR}/main.c)
if(NOT ENGINE_SOURCES)
    message(FATAL_ERROR
        "No engine sources at ${ENGINE_DIR}. This branch is the wrapper alone; "
        "build a release branch, which merges it onto the engine.")
endif()

add_library(catsystem2 SHARED ${ENGINE_SOURCES} jni.c)
target_include_directories(catsystem2 PRIVATE ${ENGINE_DIR} ${VENDOR_DIR})
target_compile_options(catsystem2 PRIVATE -std=c11 -Wall -Wextra -O2)
target_link_libraries(catsystem2 PRIVATE z log m aaudio)
