// RerankerThreadPool is now a class template (see reranker_thread_pool.h) so each
// caller can return its result BY VALUE — the R02-C2 use-after-free / data-race fix
// (a timed-out task's worker must never write back into the caller's stack). All
// member definitions therefore live in the header; this translation unit is kept as
// an (empty) compilation anchor so the existing CMake source list is unchanged.
#include "cortrix/reranker/reranker_thread_pool.h"

namespace cortrix::reranker {

// Intentionally empty — RerankerThreadPool<R> is fully defined in the header.

}  // namespace cortrix::reranker
