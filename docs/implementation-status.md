# Implementation status — 2026-09-08

The supplied plan is a multi-phase product specification. The initial workspace contained only that plan and its mockup. This implementation delivers an executable foundation/page-tool preview without advertising unsupported editor features.

| Backlog | Evidence delivered | Remaining acceptance work |
| --- | --- | --- |
| F-01 | CMake targets, local preset, installed version/hash inventory, build/run/deploy scripts | Source/revision locks, dependency rebuilds, complete license inventory, three-platform build matrix |
| F-02 / F-04 | Owned PDFium snapshots/bitmaps, serialized calls, revision/page checks | Render process, IPC, cancellation, large-document benchmarks |
| F-03 | QPDF create/open/serialize, page metadata and stable IDs | Broader structural corpus |
| F-05 | Ownership ADR and functioning round trips | Font/source-mapping feasibility, latency budgets, full P0 decision |
| E-01 / E-02 | Qt-free interface, useful typed errors, candidate-isolated commands | Feature-specific capabilities and more fault injection |
| E-03 | Undo/redo, monotonic revisions, saved identity, bounded history | Recovery-backed history and presentation selection history |
| E-04 | Safe temporary write, byte verification, replace, source conflict detection | Disk-full/interruption injection, competing-writer race policy, platform QA |
| U-01 / U-02 | Tabs, native menus/toolbars, page list, one-page canvas, zoom, inspector, text panel | Virtualized tiled canvas, full thumbnail grid, accessibility/IME qualification |
| U-03 | Search results and page navigation | Outline navigation and match geometry/highlights |
| P-01 / P-02 | Blank/insert/delete/rotate/duplicate/reorder with history | Multi-selection and drag interactions, broader geometry fixtures |
| P-03 | Whole-document page insertion for basic supported PDFs | Page ranges and supported structured-document relationship remapping |
| O-01 | PNG current-page export and document text export | Batch/range/image import, JPEG options, cancellation |
| C-01 / C-02 / C-06 | QPDF token/source spans, bounded graphics/text-state interpretation, Tj/TJ replacement for standard, simple, embedded/subset and Identity-H CID fonts, Unicode text with automatic fallback-font subset embedding, downstream advance preservation, shared-stream isolation, on-page editing | General content IR, shaping/RTL scripts, non-Identity CMaps, CFF fallback fonts, font reuse across edits, form XObjects, rotated/clipped content, paragraph editing |
| Q-01 | Generated fixture ownership and semantic/raster tests | Independent-viewer release corpus and formal manifest |

All other items remain unimplemented. The constrained text-editing subset does not represent the full text/content engine. Annotations: add/list/remove of highlight, underline, strike-out, rectangle, ellipse, ink, note and ASCII free-text with generated appearance streams (`annotation-workflows`). Forms: fill text/checkbox/radio/choice fields with regenerated appearances; signed, XFA and tagged forms stay read-only (`form-workflows`). The conversion suite, annotation editing, form creation, scripted forms, signing, recovery, printing, and installers are not represented as complete.

## Verification

- Windows x64, MSVC 19.51, Qt 6.11.1, QPDF 12.3.2, PDFium 150.0.7869.0.
- CTest engine workflows and desktop action workflows pass.
- The text-edit regression suite covers source/geometry mapping, escaped and hexadecimal strings, TJ arrays, all eight supported standard font variants, exact outside-region raster equality, shared-stream isolation, malformed/unsupported cases, overflow, stale commands, and saved extraction. The desktop test exercises actual canvas hit testing, the inline editor, Escape/Enter, text undo/redo, and save/reopen.
- The desktop target explicitly uses the Windows GUI subsystem; `build/gui` is now the default output/launch directory. The earlier `build/dev` executable may remain open and is not automatically replaced or terminated.
- The tests also pass with development dependency directories removed from PATH, using deployed DLLs beside the executable.
- A separate `FOLIOFORGE_DESKTOP=OFF` configure/build and engine test pass verifies the headless targets without finding Qt. The Windows CLI also creates and reopens a Telugu-named PDF successfully.
- Desktop screenshot inspected: page raster, readable UI text, extracted text, page navigation and properties render correctly.
- Startup initially failed because runtime DLLs were missing from the launch environment. Local deployment is now part of the build script.
- This does not establish clean-machine installation, independent-viewer interoperability, malicious-file safety, or performance on large real-world PDFs.

## Next bounded milestone

Add a shaping engine (RTL/Indic) and paragraph reflow to text editing. Implement page-range extraction with source-preservation tests, recovery, and an isolated renderer. Qualify structured-document preservation before relaxing the current read-only gates.

## Update — portability, isolation and history

- Builds are configured through `FOLIOFORGE_PREFIX_PATH` / `PDFIUM_ROOT` (no hardcoded machine paths); `scripts/fetch-pdfium.py` downloads PDFium; `vcpkg.json` provides QPDF for CI; CTest sets library search paths itself. See `docs/building.md`.
- GitHub Actions builds and runs all suites on Windows, Linux and macOS. Locally verified so far on macOS arm64 only; the Windows and Linux legs are unverified until CI runs.
- Rendering, text extraction and validation run in the `pdfeditor-render` worker with timeouts, crash recovery and per-platform restrictions (ADR-002).
- Undo history spills to disk; snapshot/file cap raised from 256 MiB to 1 GiB. Whole-snapshot storage remains, so very large PDFs still cost a full re-serialization per edit.
- Still open: Linux seccomp and Windows AppContainer for the worker, delta/incremental history, custom/subset/multilingual text editing, paragraph editing, annotation editing/non-ASCII text boxes, form creation/scripts, signing, crash recovery.

## Additions (forms, merge, annotations, signing, redaction, recovery)

- **Forms:** AcroForm text/checkbox/radio/choice filling.
- **Merge:** page-range insert (`1-3,5`), multi-file merge dialog (PDF, images, DOCX/XLSX/PPTX). Office files need LibreOffice (`soffice`, or set `FOLIOFORGE_SOFFICE`).
- **Annotations:** create, move, recolor, edit note/free-text content, delete (FolioForge-created annotations only).
- **Signing:** PKCS#12 CMS detached signatures as an incremental update, plus verification. macOS only (Security.framework); other platforms report Unsupported. Signatures are invisible and trust is not evaluated.
- **Redaction:** draw boxes, then "Apply redactions…" flattens the page to a ~200 dpi raster with the boxes burned in. Text, vectors and annotations on the page are removed, so the page is no longer selectable. Not available on pages with form fields.
- **Recovery:** unsaved documents are snapshotted every 20 s to the app-data `recovery` folder; on next launch FolioForge offers to restore them as a new file. Cleared on save or close.

## Right-hand panel rail

A vertical icon rail at the right edge switches between four panels (click the active icon to collapse) plus page properties:

- **Comments** – lists comments with replies nested; reply, edit and delete any annotation (replies and popups are removed with their parent). Edits to FolioForge text boxes rebuild their appearance; other annotations only change `/Contents`.
- **Bookmarks** – add (current page), rename, delete, click to go. Bookmarks of deleted pages are pruned; redaction retargets them. Named destinations (`/Names`, `/Dests`) still make a file read-only.
- **Pages** – lazily rendered thumbnails; insert blank, duplicate, rotate, delete, click to go.
- **Layers** – check boxes show/hide optional-content groups (writes `/OCProperties /D /ON,/OFF`, undoable); double-click renames.

The rail bottom holds the page box, previous/next, rotate, fit and zoom −/+. Search opens as a left pane with Ctrl+F.
