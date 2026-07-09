# LTEScope

LTEScope is a desktop SDR analysis app for spectrum viewing, waterfall display,
and protocol decode workflows.

Current direction:

- Native desktop first, with macOS now supported as a real build target.
- Optional SDR hardware backends for RTL-SDR, HackRF, and Airspy.
- LTE decoding stays vendored and optional through `HAS_LTE`.
- On macOS, LTE uses a direct srsRAN 4G backend. Falcon/LTESniffer stays on
  the non-macOS path for now.
- Emscripten/WebAssembly is still experimental and not a supported target yet.

The build is CMake-based and uses Ninja.

## What You Need

### macOS

Recommended packages:

```bash
brew install llvm glfw zstd fftw pkg-config
```

LTEScope will fetch GLFW 3.4, ImGui, and ImPlot automatically if those sources
or packages are not available locally.

If Apple Clang on your machine is missing the full C++ standard library headers,
use Homebrew LLVM for the build.

### Windows

Use the MSYS2 MINGW64 shell and install the toolchain packages listed in
[`COMPILE.md`](COMPILE.md).

## Build

### macOS

From the project root:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
cmake --build build -j4
```

If your local Apple toolchain is complete, you can omit the `CMAKE_*_COMPILER`
flags and let CMake use the system compiler.

### Windows

From the MSYS2 MINGW64 shell:

```bash
cmake -S . -B build -G Ninja
ninja -C build
```

## Run

### macOS

```bash
./build/LTEScope
```

### Windows

```bash
./build/LTEScope.exe
```

## Build Options

- `HAS_LTE=ON|OFF`
- `HAS_AIRSPY` is enabled automatically when libairspy is found

Examples:

```bash
cmake -S . -B build -G Ninja -DHAS_LTE=OFF
cmake -S . -B build -G Ninja -DHAS_LTE=ON
```

## Notes

- RTL-SDR and HackRF support are optional. If the libraries are not present, the
  app still builds and the UI reports that hardware support is unavailable.
- On macOS, the OpenGL path is still desktop OpenGL. The build silences the
  platform deprecation warnings, but it is not using Metal.
- On macOS, the LTE path is srsRAN-only so the app can build and run without
  the Falcon/LTESniffer stack.
- If the repository checkout is missing vendor trees for GLFW, ImGui, or
  ImPlot, CMake will fetch them during configure.

## More Detail

For the longer platform-specific dependency list and troubleshooting notes, see
[`COMPILE.md`](COMPILE.md).
