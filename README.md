# FolioForge

A local C++20 / Qt 6 PDF reader and page-tool **preview**, with design decisions recorded in [docs/adr](docs/adr) and progress tracked in [docs/implementation-status.md](docs/implementation-status.md).

This repository now contains a working desktop application, a Qt-free engine API, a PDFium adapter, a CLI, and integration tests. It is the first implementation milestone, not the full editor described in the multi-month plan.

## Build and run

FolioForge builds on Windows (MSVC), macOS and Linux. Dependencies are located through environment variables, not hardcoded paths. See [docs/building.md](docs/building.md) for the full walkthrough.

```sh
python3 scripts/fetch-pdfium.py                 # prebuilt PDFium into third_party/pdfium
export PDFIUM_ROOT=$PWD/third_party/pdfium
export FOLIOFORGE_PREFIX_PATH="/path/to/qt;/path/to/qpdf"   # Qt 6.5+ and QPDF 12+ prefixes
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

On Windows, `./scripts/build.ps1` finds MSVC, configures with the same preset, deploys runtime DLLs beside the executable, and runs the tests; `./scripts/run.ps1 [-Pdf file.pdf]` launches the result. Set `QT_ROOT`, `QPDF_ROOT` and `PDFIUM_ROOT` first (see `scripts/deploy-local.ps1`). Use the `headless` preset (or `-DFOLIOFORGE_DESKTOP=OFF`) to build only the engine, CLI and engine tests without Qt.

Continuous integration (`.github/workflows/ci.yml`) builds and tests on Windows, macOS and Linux for every push and pull request.

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
| Images | Open or drop JPG/PNG files (one PDF per file, or several images into one PDF via File > Images to new PDF), insert images as pages, then use the page tools and Save As. JPEGs are embedded without recompression; PNG transparency is preserved. Large images fit an A4-sized page |
| Read | PDFium rendering, page navigation, zoom/fit commands, on-demand thumbnails for visited pages |
| Search | Case-insensitive text search with a snippet per matching page and result navigation |
| Edit existing text | Click supported text to place a caret and type in place like a text editor; the text can grow (until the page edge) and later text in the same line reflows. Enter or clicking elsewhere applies, Escape cancels; undo/redo and save/reopen supported. Works with standard, embedded and subset fonts and with Unicode text (Latin, Greek, Cyrillic, CJK, ...); characters the run's font lacks are drawn from an embedded subset of a fallback TrueType font. Right-to-left and shaped scripts are not supported yet |
| Accessible text | Read-only selectable extracted text for the current page; not a tagged-PDF accessibility guarantee |
| Organize | Insert blank, duplicate, rotate left/right, move earlier/later, delete; at least one page retained |
| Insert / merge | Insert all pages of another supported basic PDF after the selected page |
| History | Stable page IDs, revision checks, undo/redo, correct dirty state at saved checkpoints |
| Save | Structural reparse, renderer preflight in desktop/CLI, sibling temporary file, flush, byte verification, atomic replacement, external-source conflict detection |
| Export | Current page to PNG at 144 DPI; whole-document text to UTF-8 |
| CLI | New, inspect, text, rotate, merge, JPEG-to-PDF (`image`); output collision protection |

Annotations (highlight, underline, strike-out, rectangle, ellipse, pen, sticky note, text box) can be added, selected and removed from the Annotate menu; each is written with a normal appearance stream, so other viewers show it. Existing passive markup annotations are preserved untouched (only annotations created by FolioForge can be removed). Text-box annotations support printable ASCII. PDFs with encryption, parser repairs, links, signed forms, navigation trees, tagged structures, layers, or selected document actions are conservatively read-only. The UI explains the restriction.

Forms: AcroForm text, check box, radio and drop-down/list fields can be filled with **Annotate → Fill form** (auto-selected when a page has fields). Values are written to the field and every widget gets a regenerated appearance stream, so other viewers show them. Text fields use the field's own font, so non-ASCII text is accepted only when that font has the glyphs (the standard Helvetica form font is ASCII-only). Scripts and calculations are never run, signature fields are untouched, and push buttons, rich-text, multi-select and XFA fields are not editable. PDFs that carry a digital signature, `/SigFlags` append-only, or XFA stay read-only. Pages with form fields cannot be duplicated or deleted, and PDFs with forms cannot be inserted from. Encryption passwords are not saved. No PDF scripting or form-action API is called.

```powershell
./build/gui/pdfeditor-cli.exe new output.pdf
./build/gui/pdfeditor-cli.exe inspect input.pdf
./build/gui/pdfeditor-cli.exe rotate input.pdf 2 rotated.pdf
./build/gui/pdfeditor-cli.exe merge first.pdf second.pdf merged.pdf
./build/gui/pdfeditor-cli.exe text input.pdf
```

CLI page numbers are one-based. CLI outputs must not already exist; it does not overwrite input files.

## Architecture and verification

QPDF is the only writer. Each edit opens a private candidate from an immutable committed snapshot, serializes/reopens it, and publishes it only on success. Undo stores complete snapshots: recent ones in memory (256 MiB), older ones in temporary files (up to 4 GiB / 200 checkpoints), with a 1 GiB per-document cap. Revisions always advance; saved-state identity is tracked separately. The public API exposes no Qt, QPDF, or PDFium handles.

The desktop serializes parsing, mutation, rendering, and export on a dedicated worker pool with one worker. PDFium runs in a separate, restricted `pdfeditor-render` process with timeouts and automatic restart after crashes ([ADR-002](docs/adr/002-render-worker.md)). Bitmap buffers are owned and copied before GUI delivery. Page results are checked against the requested revision/page. Closing a busy document is blocked until its operation completes.

`engine-workflows` creates its own rights-cleared PDFs and verifies save/reopen semantics, text/resource preservation, rendering geometry, immutable snapshot lifetimes, failed operations, history, conflicts, encryption, malformed input, Unicode filenames, and read-only gating. `text-edit-workflows` verifies source spans, escaped/hex text, TJ spacing compensation, standard font variants, shared-stream isolation, overflow/unsupported/stale rejection, undo, and exact raster equality outside the changed run. `desktop-smoke` drives registered actions, clicks a run, edits/cancels/commits with keyboard events, and saves/reopens. Screenshots are written to `build/gui/desktop-text-edit.png` and `build/gui/desktop-smoke.png`.

## In-place text editing

Open a supported PDF, select **Edit text** (Ctrl+E), then click an outlined run. Arrow keys select runs and Enter starts editing for keyboard use. Type your replacement and press Enter to apply it to the PDF; Escape cancels the local edit. Save writes the committed edit. Undo/redo includes text changes. Finish the local edit before saving, navigating, or closing.

The page must have one content stream, zero rotation, default user units, and an uncropped zero-origin media box. Supported: horizontal text in (a) the standard Helvetica/Courier variants, (b) simple Type1/TrueType fonts with a `/Widths` array and either WinAnsiEncoding or a ToUnicode map, and (c) `Type0` fonts with the `Identity-H` encoding and a ToUnicode map (embedded CID/subset fonts). New text reuses glyphs already available in the run's font; other characters are taken from a fallback font (`Document::setFallbackFonts`, `pdfeditor-cli edit ... FALLBACK.ttf`, `FOLIOFORGE_FONT_PATH`, or the installed system fonts) and embedded as a subset `Type0`/`CIDFontType2` font with a ToUnicode map. Only TrueType (glyf) fonts whose license permits embedding and subsetting are used. Right-to-left, Indic, Thai and other shaped scripts, vertical writing, custom Differences encodings without ToUnicode, CFF/OpenType-CFF fallbacks, line breaks inside a run, paragraph reflow, marked content, nonzero character/word spacing, glyph stretch, and clipping are gated. Runs inside form XObjects are not offered. Unsupported pages remain viewable and show a capability explanation.

The replacement must fit the original run's allocated width. It never silently shrinks, wraps, or shifts following text. The engine reparses the expected revision, replaces only the selected Tj/TJ source span, and adds a TJ advance adjustment to retain downstream text positions. It assigns a new content stream to that page to isolate shared resources. No covering rectangle is saved. Deleting all characters removes that run's glyphs while preserving its advance; use Undo to restore the run for later editing.

See [implementation status](docs/implementation-status.md), [ownership ADR](docs/adr/001-document-ownership.md), and [dependency inventory](dependencies/local-inventory.json).

## Limits of this preview

Arbitrary text/paragraph editing, new page text/images, annotation editing (move/resize/recolor), form creation and scripted forms, signing, redaction, OCR, printing, outline navigation, page-range extraction/splitting, recovery, a thumbnail organization grid, and cancellable jobs are not implemented. Rendering uses one page at a time; thumbnails are populated when pages are visited. Search lists matching pages and does not highlight glyphs on the canvas.

The parser and renderer currently run in-process. The supplied PDFium binary has V8/XFA compiled in; hardened builds and isolated workers remain release prerequisites. Input/snapshots are limited to 256 MiB, documents to 10,000 pages, and rasters to 32 megapixels. These bounds do not provide full protection against malicious decoded streams or expensive PDFs. Large-file and cross-platform qualification is outstanding.

Save conflict checking compares complete source bytes immediately before replacement. There is no cross-application locking protocol, so an external writer can still race the final comparison/replacement. Existing signature preservation is not claimed. Direct engine callers should run `Renderer::validate(snapshot)` before `Document::save`, as the desktop and CLI do.

The project includes an Apache-2.0 [LICENSE](LICENSE). See [third-party notices](THIRD_PARTY_NOTICES.md) for the external dependencies included in a distribution.
