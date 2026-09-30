# Building FolioForge

Requirements: a C++20 compiler (MSVC 2022+, Clang 15+, GCC 12+), CMake 3.25+, Ninja, Qt 6.5+ (desktop only),
QPDF 12+ and a PDFium build. Nothing is hardcoded: CMake finds dependencies through `CMAKE_PREFIX_PATH`
(Qt, QPDF) and `PDFIUM_ROOT`.

## 1. Get the dependencies

| Dependency | Windows | macOS | Linux |
| --- | --- | --- | --- |
| Qt 6.5+ | Qt online installer or `aqt` | `brew install qt` or `aqt` | distribution `qt6-base-dev` or `aqt` |
| QPDF 12+ | `vcpkg install qpdf` | `brew install qpdf` | build from source or `vcpkg install qpdf` (distribution packages are often 11.x) |
| PDFium | `python scripts/fetch-pdfium.py` | `python3 scripts/fetch-pdfium.py` | `python3 scripts/fetch-pdfium.py` |

`vcpkg.json` declares QPDF, so with `VCPKG_ROOT` set the `ci` preset installs it automatically.
`scripts/fetch-pdfium.py` downloads [pdfium-binaries](https://github.com/bblanchon/pdfium-binaries) for the current OS/architecture.

## 2. Configure, build, test

```sh
export PDFIUM_ROOT=$PWD/third_party/pdfium
export FOLIOFORGE_PREFIX_PATH="/path/to/qt;/path/to/qpdf"
cmake --preset dev            # or: headless (no Qt), ci (vcpkg toolchain)
cmake --build --preset dev
ctest --preset dev
```

PowerShell uses `$env:PDFIUM_ROOT = ...`. To keep machine-specific values, copy a preset into an untracked
`CMakeUserPresets.json`. CTest automatically adds the PDFium/QPDF/Qt runtime directories to the library search
path, so tests run without manual `PATH`/`LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH` setup. The desktop smoke test runs
with `QT_QPA_PLATFORM=offscreen`, so no display is required. On Linux install a font package (for example
`fonts-dejavu-core fonts-liberation`) so PDFium can render text.

## 3. Rendering worker

PDF rendering, text extraction and validation run in a separate `pdfeditor-render` process (see ADR-002). It must
sit beside the desktop app / CLI; the build and packaging steps place it there. Set
`FOLIOFORGE_RENDER_INPROCESS=1` to opt out of isolation for debugging only.

## Platform status

CI builds and tests all three platforms. Locally verified: macOS arm64 (Clang). Windows and Linux are exercised by CI.
