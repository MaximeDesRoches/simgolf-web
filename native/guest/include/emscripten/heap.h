// Stand-in for <emscripten/heap.h> in the native desktop build.
#ifndef SIMGOLF_NATIVE_EMSCRIPTEN_HEAP_H
#define SIMGOLF_NATIVE_EMSCRIPTEN_HEAP_H
#include <stddef.h>
static inline size_t emscripten_get_heap_size(void) { return __builtin_wasm_memory_size(0) * 65536; }
#endif
