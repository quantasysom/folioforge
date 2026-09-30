# ADR-001: QPDF ownership and checkpoint transactions

Status: implemented for the page-tool preview; full P0 feasibility remains open.

QPDF is the sole mutable document writer. PDFium receives immutable serialized bytes for rendering and extraction. No PDFium edit APIs are used. Each command reparses a private candidate, applies the edit, serializes it, and checks page structure before publishing a new revision. Failure leaves the committed snapshot and history untouched.

Page IDs are monotonically allocated within a session. Duplicates/imports get new IDs; reorder preserves IDs. Undo and redo restore state identities and page IDs while advancing revision numbers. Saved-state identity is separate, so undoing to the saved state clears dirty state. History pruning is surfaced in the UI.

This favors correctness and clear ownership over large-file performance. Small generated-document workflows pass on the installed stack. No broad source-mapped text editing, multilingual font writing, large-file latency target, or full P0 exit criterion has been qualified.

Rendering now runs in an isolated worker process; see [ADR-002](002-render-worker.md). Undo history keeps recent snapshots in memory and spills older ones to temporary files (`HistoryLimits`: 256 MiB memory, 4 GiB disk, 200 entries by default); snapshots are capped at 1 GiB.

The QPDF dependency boundary follows its [document/page helper model](https://qpdf.readthedocs.io/en/stable/design.html). Exact implementation calls were checked against the locally installed 12.3.2 headers. PDFium APIs and ownership rules were checked against the installed public headers.
