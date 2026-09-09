# FolioForge

A local C++20 / Qt 6 PDF reader and page-tool **preview**, implemented from the [architecture plan](architecture/pdf-editor-plan/PDF-Editor-Implementation-Plan.md).

This repository now contains a working desktop application, a Qt-free engine API, a PDFium adapter, a CLI, and integration tests. It is the first implementation milestone, not the full editor described in the multi-month plan.

## Run on this Windows installation

```powershell
./scripts/build.ps1
./scripts/run.ps1
# Or open a PDF immediately:
./scripts/run.ps1 -Pdf C:\path\document.pdf
```

After building, `build/gui/pdfeditor-desktop.exe` can also be launched directly. This is a Windows GUI executable and does not create a command terminal. The build script discovers MSVC, configures CMake, builds, deploys local runtime DLLs/plugins beside the executable, and runs all three test suites. It does not install dependencies or change the existing vPDF project. `build/dev` contains the older preview and is no longer the launch target.

The local `dev` preset uses Qt `C:/Qt/6.11.1/msvc2022_64`, QPDF `C:/projects/vPDF/vcpkg_installed/x64-windows`, and PDFium `C:/projects/vPDF/third_party/pdfium`. Override these paths in `CMakeUserPresets.json` or use a custom configure command on another installation. Deployment scripts currently target these local Windows dependency locations.

```powershell
# From an MSVC developer shell:
cmake --preset dev
cmake --build --preset dev
./scripts/deploy-local.ps1
ctest --preset dev
```

Generic CMake interface (requires matching installed headers/libraries):

```sh
cmake -S . -B build/custom -DCMAKE_PREFIX_PATH="/path/to/qt;/path/to/qpdf" -DPDFIUM_ROOT=/path/to/pdfium
cmake --build build/custom
ctest --test-dir build/custom --output-on-failure
```

Use `-DFOLIOFORGE_DESKTOP=OFF` to build the engine, CLI, and engine tests without Qt. Only Windows x64 / MSVC has been built and tested so far. Runtime discovery on other platforms is the caller's responsibility.

## Build installers and portable packages

Use [scripts/release.ps1](scripts/release.ps1) or [scripts/release.sh](scripts/release.sh)
to build, test, and package on Windows, Linux, or macOS. Supported outputs are
Windows ZIP/MSI/EXE, Linux TGZ/DEB/RPM, and macOS APP bundles in DMG/PKG/ZIP/TGZ.
See [packaging instructions](docs/packaging.md) for dependencies, examples and
platform validation limits. Outputs go to `dist/<OS>` with SHA-256 checksums.

## Implemented workflows

| Feature | Current behavior |
| --- | --- |
| Open / new | Local PDFs, password prompts, blank A4 PDFs, separate document tabs, drag-and-drop |
| Read | PDFium rendering, page navigation, zoom/fit commands, on-demand thumbnails for visited pages |
| Search | Case-insensitive text search with a snippet per matching page and result navigation |
| Edit existing text | Click an outlined supported run, type in place, Enter to commit, Escape to cancel; undo/redo and save/reopen supported |
| Accessible text | Read-only selectable extracted text for the current page; not a tagged-PDF accessibility guarantee |
| Organize | Insert blank, duplicate, rotate left/right, move earlier/later, delete; at least one page retained |
| Insert / merge | Insert all pages of another supported basic PDF after the selected page |
| History | Stable page IDs, revision checks, undo/redo, correct dirty state at saved checkpoints |
| Save | Structural reparse, renderer preflight in desktop/CLI, sibling temporary file, flush, byte verification, atomic replacement, external-source conflict detection |
| Export | Current page to PNG at 144 DPI; whole-document text to UTF-8 |
| CLI | New, inspect, text, rotate, merge; output collision protection |

PDFs with encryption, parser repairs, annotations, forms, signatures, navigation trees, tagged structures, layers, or selected document actions are conservatively read-only. The UI explains the restriction. Encryption passwords are not saved. No PDF scripting or form-action API is called.

```powershell
./build/gui/pdfeditor-cli.exe new output.pdf
./build/gui/pdfeditor-cli.exe inspect input.pdf
./build/gui/pdfeditor-cli.exe rotate input.pdf 2 rotated.pdf
./build/gui/pdfeditor-cli.exe merge first.pdf second.pdf merged.pdf
./build/gui/pdfeditor-cli.exe text input.pdf
```

CLI page numbers are one-based. CLI outputs must not already exist; it does not overwrite input files.

## Architecture and verification

QPDF is the only writer. Each edit opens a private candidate from an immutable committed snapshot, serializes/reopens it, and publishes it only on success. Undo stores complete snapshots, bounded to 100 checkpoints and a 128 MiB history budget. Revisions always advance; saved-state identity is tracked separately. The public API exposes no Qt, QPDF, or PDFium handles.

The desktop serializes parsing, mutation, rendering, and export on a dedicated worker pool with one worker. PDFium additionally has a process-wide mutex. Bitmap buffers are owned and copied before GUI delivery. Page results are checked against the requested revision/page. Closing a busy document is blocked until its operation completes.

`engine-workflows` creates its own rights-cleared PDFs and verifies save/reopen semantics, text/resource preservation, rendering geometry, immutable snapshot lifetimes, failed operations, history, conflicts, encryption, malformed input, Unicode filenames, and read-only gating. `text-edit-workflows` verifies source spans, escaped/hex text, TJ spacing compensation, standard font variants, shared-stream isolation, overflow/unsupported/stale rejection, undo, and exact raster equality outside the changed run. `desktop-smoke` drives registered actions, clicks a run, edits/cancels/commits with keyboard events, and saves/reopens. Screenshots are written to `build/gui/desktop-text-edit.png` and `build/gui/desktop-smoke.png`.

## In-place text editing

Open a supported PDF, select **Edit text** (Ctrl+E), then click an outlined run. Arrow keys select runs and Enter starts editing for keyboard use. Type your replacement and press Enter to apply it to the PDF; Escape cancels the local edit. Save writes the committed edit. Undo/redo includes text changes. Finish the local edit before saving, navigating, or closing.

The first supported category is horizontal printable ASCII in standard Type 1 Helvetica/Courier (regular, bold, oblique, bold-oblique), with WinAnsiEncoding or a restricted StandardEncoding subset. The page must have one content stream, zero rotation, default user units, and an uncropped zero-origin media box. Complex/custom/embedded fonts, non-ASCII text, marked content, nonzero character/word spacing, glyph stretch, and clipping are gated. Runs inside form XObjects are not offered. Unsupported pages remain viewable and show a capability explanation.

The replacement must fit the original run's allocated width. It never silently shrinks, wraps, or shifts following text. The engine reparses the expected revision, replaces only the selected Tj/TJ source span, and adds a TJ advance adjustment to retain downstream text positions. It assigns a new content stream to that page to isolate shared resources. No covering rectangle is saved. Deleting all characters removes that run's glyphs while preserving its advance; use Undo to restore the run for later editing.

See [implementation status](docs/implementation-status.md), [ownership ADR](docs/adr/001-document-ownership.md), and [dependency inventory](dependencies/local-inventory.json).

## Limits of this preview

Arbitrary text/paragraph editing, new text/images/shapes, annotations, forms, signing, redaction, OCR, printing, outline navigation, page-range extraction/splitting, recovery, a thumbnail organization grid, and cancellable jobs are not implemented. Rendering uses one page at a time; thumbnails are populated when pages are visited. Search lists matching pages and does not highlight glyphs on the canvas.

The parser and renderer currently run in-process. The supplied PDFium binary has V8/XFA compiled in; hardened builds and isolated workers remain release prerequisites. Input/snapshots are limited to 256 MiB, documents to 10,000 pages, and rasters to 32 megapixels. These bounds do not provide full protection against malicious decoded streams or expensive PDFs. Large-file and cross-platform qualification is outstanding.

Save conflict checking compares complete source bytes immediately before replacement. There is no cross-application locking protocol, so an external writer can still race the final comparison/replacement. Existing signature preservation is not claimed. Direct engine callers should run `Renderer::validate(snapshot)` before `Document::save`, as the desktop and CLI do.

The project includes an Apache-2.0 [LICENSE](LICENSE). See [third-party notices](THIRD_PARTY_NOTICES.md) for the external dependencies included in a distribution.
