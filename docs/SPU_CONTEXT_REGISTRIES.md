# Per-context DMA and reservation ownership

The SPURS HLE retains a context for each workload/lane across scheduling
quanta. That count can exceed the physical SPU count even when only one policy
executes at a time. The MFC engine is now embedded in `spu_context`, so each
virtual owner keeps its queue, tag completion and stalled-list state without a
process-wide lookup. The reservation-holder registry remains separate and
continues to cover paused policy contexts.

`mfc_for(ctx)` lazily initializes `ctx->mfc` and returns that address directly.
The context's single execution owner protects engine state; it must stop before
`spu_mfc_release` or context reset/free. Persistent policy contexts keep their
engine across scheduling quanta and release it only when the context is reset
or destroyed. Stack-local job contexts release after execution.

`spu_mfc_has_engine` reads an atomic per-context presence marker, so served and
shadow contexts still prove that they did not create an engine. The only MFC
context list is the optional `SPU_PCSAMPLE` diagnostic registry. It is populated
only when sampling is enabled and uses its lock only for diagnostic iteration,
registration and cleanup; ordinary MFC channel lookup, initialization and
release do not visit it.

The engine itself needs no heap allocation. A diagnostic list node is retired
by normal release or by `spu_mfc_forget_range` when a stack unwinds; range
cleanup compares saved addresses and never reads the abandoned context. Worker
exit obtains stack bounds on Windows, macOS and Linux so sampling cannot retain
a dangling stack pointer.

The coherence registry keeps its existing lock-line ownership and allocation
failure behavior. This change does not alter GETLLAR/PUTLLC ordering, stalled
list semantics or reservation invalidation.

`python3 tools/test_spu_context_registries.py` runs POSIX C fixtures for
independent contexts, real stalled-list DMA, cross-thread engine-presence
reads, `SPU_PCSAMPLE` initialization and reservation holders. The dedicated
MFC context-engine fixtures count lookup locks, copy contexts to prove inline
engines do not alias, and protect an abandoned range before cleanup.
