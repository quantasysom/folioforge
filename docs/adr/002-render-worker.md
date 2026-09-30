# ADR-002: Isolated rendering worker

Status: implemented; hardening is per-platform (see below).

PDFium parses untrusted PDFs and is the most likely component to crash, hang or exhaust memory. All rendering, text extraction and pre-save validation now run in a separate `pdfeditor-render` process, started and supervised by `RenderService` (`engine/include/pdfengine/render_service.h`). The desktop app and CLI use it by default. QPDF parsing and editing stay in the application process; QPDF never hands data to PDFium except as immutable serialized snapshots.

## Protocol
Length-prefixed binary messages over the child's stdin/stdout (`engine/src/render_protocol.h`). A snapshot is sent once (`Load`) and cached by the worker; later render/text/validate requests only carry a page and scale. Responses carry either a payload or an error code and message. The client validates every field of every reply (sizes, strides, magic).

## Failure handling
- Every call has a deadline (30 s render/text, 120 s validate/load by default). A watchdog kills the worker on expiry.
- A crash, hang, oversized or malformed reply kills the worker and raises `ErrorCode::RenderWorkerFailed`; the document state is untouched and the next call starts a fresh worker.
- Ordinary problems (bad page index, zoom limits) are reported as normal errors and do not restart the worker.
- The service is serialized; the desktop's single worker pool already serializes callers.

## Restrictions on the worker
| Platform | Applied restrictions |
| --- | --- |
| macOS | Seatbelt profile (`sandbox_init`): deny by default, read-only file access, no file writes, no network, no exec/fork. Descriptors other than stdio are not inherited (`POSIX_SPAWN_CLOEXEC_DEFAULT`). |
| Linux | `RLIMIT_AS` from the memory budget, `RLIMIT_NOFILE`, `RLIMIT_CPU`, no core dumps, `PR_SET_NO_NEW_PRIVS`, non-dumpable, fds above 2 closed at spawn. No seccomp filter yet. |
| Windows | Job object: kill-on-close, one active process, memory limit, UI restrictions; only stdio pipes inherited. No AppContainer/low-integrity token yet. |

Not yet done: a Linux seccomp/Landlock filter and a Windows low-integrity/AppContainer token. These are the next hardening steps; the process boundary, timeouts and resource limits are in place on all platforms. The `mac` sandbox and the fault-injection tests (`render-service-workflows`: parity with in-process rendering, snapshot caching, error propagation, timeout, crash recovery, and on macOS write denial) were verified on macOS; Windows and Linux run the same tests in CI.

## Opt-out
`FOLIOFORGE_RENDER_INPROCESS=1` runs PDFium in-process. This exists for debugging and provides no isolation. `FOLIOFORGE_RENDER_TEST_HOOKS=1` enables fault-injection requests in the worker and is used only by tests.
