# Build and package FolioForge

Use `scripts/release.ps1` (Windows PowerShell 5.1 or PowerShell 7) or
`bash scripts/release.sh` (Bash 3.2+, including macOS's bundled Bash).
Both run the same CMake pipeline: configure Release, compile, run the three
test suites, deploy runtime dependencies, create packages, and write SHA256SUMS.
They do not install dependencies, publish packages, or require administrator access.

Build each package **on its target OS**. A Windows build does not produce Linux
or macOS binaries. Use matching compiler/architecture builds of all dependencies;
build Intel and Apple Silicon packages separately on matching macOS environments.

## Prerequisites and formats

All hosts need CMake 3.25+, Ninja (or `--generator` / `-Generator`), a C++20
compiler, Qt 6.5+ with Core/Gui/Widgets/Concurrent and deployment tools,
QPDF 12+ with its CMake package, and PDFium headers and shared library.
Set `QT_ROOT`, `QPDF_ROOT`, and `PDFIUM_ROOT`, or pass their command-line options.
Prefixes must contain their installed `bin`, `lib`, `include`, and CMake files
as applicable. PDFium's shared library must be in `bin` on Windows or `lib` on Unix.

| Host | Formats | Additional tools |
| --- | --- | --- |
| Windows | ZIP (default), MSI, EXE | MSVC C++ build tools; WiX 3 (`candle`, `light`) or WiX 4 for MSI; NSIS (`makensis`) for EXE installer |
| Linux | TGZ (default), DEB, RPM | GCC/Clang; `dpkg-shlibdeps` for DEB; `rpmbuild` for RPM; Qt's platform/plugin dependencies |
| macOS | DMG (default), PKG, ZIP, TGZ | Xcode command-line tools, Qt's `macdeployqt`; native `hdiutil` / `productbuild` |

`EXE` means an NSIS installer. Windows application `.exe` files are built for
every Windows format. macOS packages contain `FolioForge.app`, with the CLI at
`FolioForge.app/Contents/MacOS/pdfeditor-cli`. Windows/Linux packages contain
both executables in `bin`.

WiX 4 needs CMake 3.30+ and its matching `WixToolset.UI.wixext` extension installed.
For example, install WiX 4.0.4 and `WixToolset.UI.wixext/4.0.4` together; select
`-WixVersion 4`. The default is WiX 3. Missing requested packaging tools fail the
run before compilation; formats are never silently skipped.

## Windows PowerShell

```powershell
./scripts/release.ps1 `
  -QtRoot C:/Qt/6.11.1/msvc2022_64 `
  -QpdfRoot C:/projects/vPDF/vcpkg_installed/x64-windows `
  -PdfiumRoot C:/projects/vPDF/third_party/pdfium `
  -Formats ZIP

# With WiX 4 and NSIS installed and available on PATH:
./scripts/release.ps1 `
  -QtRoot C:/Qt/6.11.1/msvc2022_64 `
  -QpdfRoot C:/projects/vPDF/vcpkg_installed/x64-windows `
  -PdfiumRoot C:/projects/vPDF/third_party/pdfium `
  -Formats ZIP,MSI,EXE -WixVersion 4
```

PowerShell initializes an x64 MSVC shell when `cl` is not already available.
For a different architecture, start the matching developer shell first and use
matching dependencies and a separate build directory. Git Bash can also run the
shell script from an initialized MSVC developer environment. WSL builds Linux
packages, not Windows packages.

## Linux shell

```bash
export QT_ROOT=/opt/Qt/6.8.3/gcc_64
export QPDF_ROOT=/opt/qpdf
export PDFIUM_ROOT=/opt/pdfium
bash scripts/release.sh --formats TGZ,DEB
# On an RPM build host:
bash scripts/release.sh --formats RPM
```

Use the oldest Linux distribution you intend to support as the build host.
System libraries remain system dependencies; these packages are not universal
static binaries. DEB/RPM dependencies are calculated by their native packaging
tools. A TGZ user must supply the corresponding system libraries. Qt plugins
and non-system libraries are deployed using Qt's deployment support.

## macOS shell

```bash
export QT_ROOT="$HOME/Qt/6.8.3/macos"
export QPDF_ROOT="$(brew --prefix qpdf)"
export PDFIUM_ROOT="$HOME/deps/pdfium"
bash scripts/release.sh --formats DMG,PKG,ZIP
```

The bundle includes the desktop app, CLI and runtime dependencies. PKG targets
`/Applications`; DMG supports copying the app to Applications. These scripts
produce unsigned development packages. Developer ID signing/notarization and
Windows Authenticode signing require your certificates and are separate release
steps; no signing identity or credential is embedded in these scripts.

## Options and output

Run `bash scripts/release.sh --help` or `Get-Help ./scripts/release.ps1 -Full`.
Useful paired options:

| Shell | PowerShell | Purpose |
| --- | --- | --- |
| `--build-dir PATH` | `-BuildDirectory PATH` | Default `build/release-<OS>` |
| `--output-dir PATH` | `-OutputDirectory PATH` | Default `dist/<OS>` |
| `--generator NAME` | `-Generator NAME` | Default Ninja; supports multi-config generators |
| `--toolchain PATH` | `-Toolchain PATH` | Optional native dependency toolchain file |
| `--notices-dir PATH` | `-NoticesDirectory PATH` | Include an assembled third-party notices directory |
| `--skip-tests` | `-SkipTests` | Explicitly opt out of running tests |

Relative build/output paths are resolved against the repository root. Reuse a
build directory only with the same generator, compiler and dependency versions.
Builds do not delete existing directories. CPack may replace same-named output
packages. `SHA256SUMS` covers package files in the selected output directory.

The project's Apache-2.0 LICENSE and THIRD_PARTY_NOTICES.md are included in each
package. Pass `--notices-dir` with the notices for the exact third-party binaries
you distribute. The source repository's existing dependency inventory describes
the original development installation.

## Verification and references

The Windows ZIP pipeline is exercised locally, including the existing tests.
MSI/NSIS require their optional tools; Linux and macOS need validation on their
respective hosts before release.

- [Qt runtime deployment](https://doc.qt.io/qt-6/qt-deploy-runtime-dependencies.html)
- [Qt native-host deployment support](https://doc.qt.io/qt-6/qt-generate-deploy-app-script.html)
- [CPack WiX requirements](https://cmake.org/cmake/help/latest/cpack_gen/wix.html)
