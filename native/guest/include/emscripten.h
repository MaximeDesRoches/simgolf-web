// Stand-in for <emscripten.h> in the native desktop build (native/README.md).
// The port's platform code is compiled unchanged to plain wasm with wasi-sdk;
// every EM_JS / EM_ASYNC_JS function becomes an import from the "host"
// module, which native/host implements in place of the page's JavaScript.
// The host runs the game on its own thread of control, so the async ones
// simply block.

#ifndef SIMGOLF_NATIVE_EMSCRIPTEN_H
#define SIMGOLF_NATIVE_EMSCRIPTEN_H

#define HOST_IMPORT(name) __attribute__((import_module("host"), import_name(#name)))

#define EM_JS(ret, name, params, ...) HOST_IMPORT(name) ret name params;
#define EM_ASYNC_JS(ret, name, params, ...) HOST_IMPORT(name) ret name params;

// The few inline snippets are page-only (canvas styling, console stacks).
#define EM_ASM(...) ((void)0)
#define EM_ASM_INT(...) 0

#ifdef __cplusplus
extern "C" {
#endif
HOST_IMPORT(emscripten_get_now) double emscripten_get_now(void);
#ifdef __cplusplus
}
#endif

#endif
