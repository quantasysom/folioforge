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
| C-01 / C-02 / C-06 | QPDF token/source spans, bounded graphics/text-state interpretation, actual standard-font Tj/TJ replacement, downstream advance preservation, shared-stream isolation, on-page editing | General content IR, custom/subset fonts, multilingual shaping, embedded fonts, form XObjects, rotated/clipped content, paragraph editing |
| Q-01 | Generated fixture ownership and semantic/raster tests | Independent-viewer release corpus and formal manifest |

All other items remain unimplemented. The constrained text-editing subset does not represent the full text/content engine. The conversion suite, annotations, forms, signing, recovery, printing, and installers are not represented as complete.

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

Expand font/source-mapping qualification before enabling custom/subset or multilingual text editing. Implement page-range extraction with source-preservation tests, recovery, and an isolated renderer. Qualify structured-document preservation before relaxing the current read-only gates.
