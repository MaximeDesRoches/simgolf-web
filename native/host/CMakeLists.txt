# SimGolf desktop app (native/README.md): the game module translated to C by
# wasm2c, plus a host over SDL2. Configure with the game module from
# native/guest (or a release download):
#
#   cmake -B build/native -S native/host -DSIMGOLF_WASM=build/guest/golf-host.wasm
#   cmake --build build/native
#
# Needs SDL2, zlib and wasm2c (WABT 1.0.42, matching third_party/wasm2c-rt).
cmake_minimum_required(VERSION 3.20)
project(simgolf_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(SIMGOLF_WASM "" CACHE FILEPATH "golf-host.wasm (from native/guest)")
set(SIMGOLF_VERSION "0.1.0" CACHE STRING "App version")
if(NOT SIMGOLF_WASM OR NOT EXISTS "${SIMGOLF_WASM}")
  message(FATAL_ERROR "Set SIMGOLF_WASM to golf-host.wasm (build native/guest first)")
endif()
get_filename_component(SIMGOLF_WASM "${SIMGOLF_WASM}" ABSOLUTE)

option(SIMGOLF_BUNDLED_SDL "Build SDL2 from source and link it statically (release builds)" OFF)
option(SIMGOLF_BUNDLED_ZLIB "Build zlib from source (cross builds)" OFF)

find_program(WASM2C wasm2c REQUIRED)
include(FetchContent)
if(SIMGOLF_BUNDLED_ZLIB)
  set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(zlib URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz)
  FetchContent_MakeAvailable(zlib)
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
  target_include_directories(zlibstatic INTERFACE "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
else()
  find_package(ZLIB REQUIRED)
endif()
if(SIMGOLF_BUNDLED_SDL)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz)
  FetchContent_MakeAvailable(SDL2)
  set(SDL2_TARGET SDL2::SDL2-static)
else()
  find_package(SDL2 REQUIRED)
  set(SDL2_TARGET SDL2::SDL2)
endif()

# The game module as C, split so the compiler can work in parallel.
set(W2C_DIR "${CMAKE_BINARY_DIR}/w2c")
set(W2C_N 16)
set(W2C_SRC "")
foreach(i RANGE 0 15)
  list(APPEND W2C_SRC "${W2C_DIR}/golf_${i}.c")
endforeach()
add_custom_command(
  OUTPUT ${W2C_SRC} "${W2C_DIR}/golf.h"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${W2C_DIR}"
  COMMAND ${WASM2C} "${SIMGOLF_WASM}" -o "${W2C_DIR}/golf.c" --num-outputs=${W2C_N} -n golf
  DEPENDS "${SIMGOLF_WASM}"
  COMMENT "wasm2c: translating the game module")

set(RT "${ROOT}/third_party/wasm2c-rt")
add_library(w2c_golf STATIC ${W2C_SRC} "${RT}/wasm-rt-impl.c" "${RT}/wasm-rt-mem-impl.c"
  "${RT}/wasm-rt-exceptions-impl.c")
target_include_directories(w2c_golf PUBLIC "${W2C_DIR}" "${RT}")
target_compile_options(w2c_golf PRIVATE -O2 -w -fno-strict-aliasing)

set(U "${ROOT}/third_party/unshield/lib")
add_library(unshield STATIC
  ${U}/component.c ${U}/converter.c ${U}/directory.c ${U}/file.c ${U}/file_group.c
  ${U}/helper.c ${U}/libunshield.c ${U}/log.c ${U}/md5/md5c.c)
target_include_directories(unshield PUBLIC "${U}" PRIVATE "${U}/.." "${U}/md5")
target_compile_options(unshield PRIVATE -w)
target_link_libraries(unshield PUBLIC ZLIB::ZLIB)

set(L "${ROOT}/third_party/lzma-sdk")
add_library(lzma7z STATIC
  ${L}/7zAlloc.c ${L}/7zArcIn.c ${L}/7zBuf.c ${L}/7zCrc.c ${L}/7zCrcOpt.c ${L}/7zDec.c ${L}/7zFile.c
  ${L}/7zStream.c ${L}/Alloc.c ${L}/Bcj2.c ${L}/Bra.c ${L}/Bra86.c ${L}/BraIA64.c ${L}/CpuArch.c
  ${L}/Delta.c ${L}/Lzma2Dec.c ${L}/LzmaDec.c ${L}/Ppmd7.c ${L}/Ppmd7Dec.c)
target_include_directories(lzma7z PUBLIC "${L}")
target_compile_options(lzma7z PRIVATE -w)
if(WIN32)
  target_compile_definitions(lzma7z PUBLIC UNICODE _UNICODE)
endif()

set(HOST_SRC main.cpp run.c wasi.cpp imports.cpp audio.cpp importer.cpp setup.cpp)
set(RESOURCES "${ROOT}/third_party/fonts/LiberationSans-Regular.ttf" "${ROOT}/third_party/fonts/LiberationSans-Bold.ttf"
  "${ROOT}/third_party/fonts/LiberationSerif-Bold.ttf" "${ROOT}/third_party/fonts/ComicNeue-Bold.ttf"
  "${ROOT}/third_party/fonts/LICENSE" "${ROOT}/third_party/fonts/README.md")

if(APPLE)
  set(ICON "${CMAKE_CURRENT_SOURCE_DIR}/macos/SimGolf.icns")
  if(NOT EXISTS "${ICON}")
    set(ICON "")
  endif()
  add_executable(SimGolf MACOSX_BUNDLE ${HOST_SRC} ${ICON})
  if(ICON)
    set_source_files_properties(${ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  endif()
  set_source_files_properties(${RESOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/fonts)
  set_source_files_properties("${ROOT}/web/pages/jgld-nops.json" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  target_sources(SimGolf PRIVATE ${RESOURCES} "${ROOT}/web/pages/jgld-nops.json")
  set_target_properties(SimGolf PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_VERSION "${SIMGOLF_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${SIMGOLF_VERSION}")
  # The translated game nests calls deeply; give the main thread room.
  target_link_options(SimGolf PRIVATE -Wl,-stack_size,0x4000000)
else()
  add_executable(SimGolf WIN32 ${HOST_SRC})
  if(WIN32)
    target_sources(SimGolf PRIVATE windows/simgolf.rc)
    target_link_options(SimGolf PRIVATE -Wl,--stack,67108864 -static -s)
    target_link_libraries(SimGolf PRIVATE comdlg32)
  endif()
  # resources/ next to the executable (main.cpp looks there)
  add_custom_command(TARGET SimGolf POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SimGolf>/resources/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy ${RESOURCES} "$<TARGET_FILE_DIR:SimGolf>/resources/fonts/"
    COMMAND ${CMAKE_COMMAND} -E copy "${ROOT}/web/pages/jgld-nops.json" "$<TARGET_FILE_DIR:SimGolf>/resources/")
endif()
target_include_directories(SimGolf PRIVATE "${ROOT}/third_party/stb")
if(TARGET SDL2::SDL2main)
  target_link_libraries(SimGolf PRIVATE SDL2::SDL2main)
endif()
target_link_libraries(SimGolf PRIVATE w2c_golf unshield lzma7z ${SDL2_TARGET} ZLIB::ZLIB)
