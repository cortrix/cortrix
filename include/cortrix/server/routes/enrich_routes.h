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
///     1. audit (default on): reconcile the namespace with the ingest's index
///        rule in both directions — remove contextual index entries the rule
///        excludes (skip-index chunks, documents below the full processing
///        level), then scan the child blocks against the CONFIGURED chain members
///        and synthesize enrich_state rows for legacy / silently-degraded chunks
///        (f03 ⇔ enriched_score, f35 ⇔ contextualized_status==1 plus the index
///        label where one is expected, f38 ⇔ a hype block with this source child);
///     2. make every 'pending_retry' row due now;
///     3. run one sweep for this namespace → kTaskEnrichBackfill tasks.
///     Response: {namespace, audited, removed, synthesized, enqueued, counts{...}};
///     `removed` is 0 when audit is false.
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
