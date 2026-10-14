The wasm2c runtime from [WABT](https://github.com/WebAssembly/wabt) 1.0.42
(`include/wasm-rt*.h`, `wasm2c/wasm-rt-*`), Apache-2.0. The native desktop
build (native/) compiles the C that `wasm2c` 1.0.42 writes against these, so
keep the two versions in step.
