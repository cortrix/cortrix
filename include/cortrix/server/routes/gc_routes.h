#pragma once

namespace httplib { class Server; }

namespace cortrix {

class ApiKeyAuth;

namespace catalog::gc {
class GcManager;
class DocumentGcSweeper;
}

/// Register the OPEN-2 GC + maintenance ops routes (ARCH, api/paths/ops.yaml).
/// All endpoints are mounted under /api/v1 and require the ops:admin scope
/// (kPermAdmin); the destructive ones additionally require X-Ops-Confirm: true.
///
///   GET  /api/v1/gc/status            -> GcStatus (read-only)
///   POST /api/v1/gc/run               -> 202 GcStatus  (X-Ops-Confirm)
///   POST /api/v1/gc/restore           -> 200 PartialSuccessById  ({document_ids:[...]})
///   POST /api/v1/gc/purge             -> 202 GcStatus  (X-Ops-Confirm, irreversible)
///   POST /api/v1/maintenance/vacuum   -> 202  (SQLite VACUUM over catalog.db)
///   POST /api/v1/maintenance/reindex  -> 202  (REINDEX; optional {namespace})
///
/// The document-lifecycle ops (status soft_deleted_count / run / restore / purge)
/// delegate to the DocumentGcSweeper — the layer users see (soft-deleted documents,
/// not catalog blob_uris). GcManager backs the maintenance vacuum/reindex over
/// catalog.db. GC responses follow the GcStatus contract (status /
/// soft_deleted_count / reclaimable_bytes / last_gc_at); errors use the GEN-Agent
/// 4-field envelope.
void RegisterGcRoutes(httplib::Server& server,
                      catalog::gc::GcManager& gc,
                      catalog::gc::DocumentGcSweeper& sweeper,
                      ApiKeyAuth& auth);

}  // namespace cortrix
