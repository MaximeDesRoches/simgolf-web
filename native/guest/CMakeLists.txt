# The game as a plain wasm32 module for the native desktop host
# (native/README.md). Same sources as the browser build's golf target, built
# with wasi-sdk instead of Emscripten: <emscripten.h> is replaced by
# include/emscripten.h, which turns the page's JavaScript functions into
# imports the host implements, and SDL calls become imports too (SDL's headers
# are used for types only).
#
#   cmake -B build/guest -S native/guest -DCMAKE_TOOLCHAIN_FILE=$WASI_SDK/share/cmake/wasi-sdk-p1.cmake
#   cmake --build build/guest
#
# Needs the translated game in build/gen (tools/port/rebuild.sh), like the
# browser build. The output, golf-host.wasm, is what native/host runs.
cmake_minimum_required(VERSION 3.20)
project(simgolf_guest LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(SIMGOLF_GEN "${ROOT}/build/gen" CACHE PATH "Translator output")
set(SDL2_INCLUDE "$ENV{HOME}/projects/emsdk/upstream/emscripten/cache/sysroot/include/SDL2" CACHE PATH "SDL2 headers (types only)")

# FFmpeg's Bink decoder for the videos (tools/native/build-ffmpeg-wasi.sh)
set(FFMPEG "${ROOT}/build/ffmpeg-wasi/out" CACHE PATH "FFmpeg built for wasi")
if(NOT EXISTS "${FFMPEG}/lib/libavcodec.a")
  message(FATAL_ERROR "No FFmpeg in ${FFMPEG}: run tools/native/build-ffmpeg-wasi.sh first")
endif()

if(NOT EXISTS "${SIMGOLF_GEN}/golf/golf_protos.h")
  message(FATAL_ERROR "No translated game in ${SIMGOLF_GEN}: run tools/port/rebuild.sh first")
endif()

file(GLOB GEN_SRC CONFIGURE_DEPENDS "${SIMGOLF_GEN}/*/*.c")
file(GLOB GOLF_OVERRIDES CONFIGURE_DEPENDS "${ROOT}/src/port/golf/*.c")
file(GLOB JGLD_OVERRIDES CONFIGURE_DEPENDS "${ROOT}/src/port/jgld/*.c")

add_executable(golf-host
  ${ROOT}/src/port/port_main.c ${ROOT}/src/port/runtime.c ${ROOT}/src/port/crt.c ${ROOT}/src/port/fs.c
  ${ROOT}/src/port/win32.c ${ROOT}/src/port/gdi.c ${ROOT}/src/port/imports.c video.c
  ${ROOT}/src/port/hd.c
  ${ROOT}/src/platform/terrain_dll.cpp ${ROOT}/src/platform/terrain_gl.cpp ${ROOT}/src/platform/terrain_bridge.cpp
  ${ROOT}/src/platform/terrain_hd.cpp ${ROOT}/src/platform/sound.cpp
  ${GEN_SRC} ${GOLF_OVERRIDES} ${JGLD_OVERRIDES})
set_target_properties(golf-host PROPERTIES SUFFIX ".wasm")
target_compile_definitions(golf-host PRIVATE SIMGOLF_PORT_FS SIMGOLF_HOST _WASI_EMULATED_SIGNAL _WASI_EMULATED_PROCESS_CLOCKS)
target_include_directories(golf-host PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include" "${ROOT}/src" "${ROOT}/third_party/stb" "${SDL2_INCLUDE}" "${FFMPEG}/include")
foreach(f IN LISTS GEN_SRC)
  get_filename_component(d "${f}" DIRECTORY)
  set_property(SOURCE "${f}" APPEND PROPERTY INCLUDE_DIRECTORIES "${d}")
endforeach()
set_property(SOURCE ${GOLF_OVERRIDES} APPEND PROPERTY INCLUDE_DIRECTORIES "${SIMGOLF_GEN}/golf")
set_property(SOURCE ${JGLD_OVERRIDES} APPEND PROPERTY INCLUDE_DIRECTORIES "${SIMGOLF_GEN}/jgld")
set_source_files_properties(${GEN_SRC} ${GOLF_OVERRIDES} ${JGLD_OVERRIDES} PROPERTIES COMPILE_OPTIONS
  "-w;-std=gnu11;-Wno-error=int-conversion;-Wno-error=incompatible-pointer-types;-Wno-error=implicit-function-declaration;-Wno-error=incompatible-function-pointer-types;-Wno-error=return-type")
target_compile_options(golf-host PRIVATE -O1 -fno-strict-aliasing -fwrapv -fwrapv-pointer
  -fno-delete-null-pointer-checks -fno-finite-loops -fno-exceptions)
target_link_options(golf-host PRIVATE
  -Wl,--allow-undefined -Wl,--no-stack-first -Wl,--global-base=285212672 -Wl,-z,stack-size=8388608
  -Wl,--initial-memory=402653184 -Wl,--max-memory=2147483648
  -lwasi-emulated-signal -lwasi-emulated-process-clocks)
target_link_libraries(golf-host PRIVATE "${FFMPEG}/lib/libavformat.a" "${FFMPEG}/lib/libavcodec.a" "${FFMPEG}/lib/libavutil.a")
