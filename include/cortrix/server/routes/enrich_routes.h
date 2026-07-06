#pragma once
#include <string>
#include <vector>

#include "cortrix/auth/auth_middleware.h"
#include "cortrix/resource/namespace_pool.h"

namespace httplib { class Server; }
namespace cortrix::observability { class IOperationLogger; }
namespace cortrix::spc { class EnrichRetrySweeper; }

namespace cortrix {

/// addendum §3.7 G4 — the enrichment-backfill ops surface (Agent self-service):
///
///   POST /api/v1/namespaces/:name/enrich/backfill   (admin)
///     body (optional): {"audit": true}
///     1. audit (default on): scan the namespace's child blocks against the
///        CONFIGURED chain members and synthesize enrich_state rows for legacy /
///        silently-degraded chunks (f03 ⇔ enriched_score, f35 ⇔
///        contextualized_status==1, f38 ⇔ a hype block with this source child);
///     2. make every 'pending_retry' row due now;
///     3. run one sweep for this namespace → kTaskEnrichBackfill tasks.
///     Response: {namespace, synthesized, enqueued, counts{...}}.
///
/// `chain_tokens` = the tokens of the CURRENTLY configured enricher chain
/// ("f03"/"f35"/"f38") — the owed universe for the audit; empty ⇒ the endpoint
/// answers 409 (enrichment not configured, nothing to backfill toward).
void RegisterEnrichRoutes(httplib::Server& server,
                          resource::INamespacePool& pool,
                          ApiKeyAuth& auth,
                          spc::EnrichRetrySweeper* sweeper,
                          std::vector<std::string> chain_tokens,
                          observability::IOperationLogger* oplog);

}  // namespace cortrix
