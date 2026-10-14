// Runs the game module's _start, catching traps. In C because the runtime's
// jump buffer is a C11 _Thread_local that C++ code can't name reliably.

#include "golf.h"
#include "wasm-rt-exceptions.h"
#include "wasm-rt-impl.h"

int simgolf_run(w2c_golf *inst) {
  wasm_rt_trap_t trap = wasm_rt_impl_try();
  if (trap != WASM_RT_TRAP_NONE) return (int)trap;
  w2c_golf_0x5Fstart(inst);
  return 0;
}
