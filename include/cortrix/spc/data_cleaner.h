#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cortrix/common/status.h"
#include "cortrix/spc/cleaning_errors.h"
#include "cortrix/spc/cleaning_types.h"

// Forward declarations — data cleaning does not hard-depend on those headers.
//  - TraceContext (GEN-Observability): Phase 1 noop pointer, Phase 2 OTel.
//  - IOperationLogger (CE operation log): audit sink. NOT in the L0 base
//    yet — taken as a *nullable* shared_ptr so data cleaning
//    builds + tests standalone; the live operation-log wiring lands later.
namespace cortrix::observability { struct TraceContext; }
namespace cortrix::operation_log { class IOperationLogger; }

namespace cortrix::spc {

/// SPC Pipeline data-cleaning module. Runs *before* Block assembly to
/// drop duplicate chunks (exact hash + semantic cosine, D2/D3) and to mark
/// anomalous chunks (5 reasons, D4) so Storage Write keeps them out of P-HNSW
/// (D5). chunk-level; orthogonal to file-level dedup (tenants.dedup_scope).
///
/// D3 standalone: Dedup / DetectAnomaly / config validation are fully
/// implemented + tested against the `Block` contract view. The
/// SpcPipeline wiring (ChunkResult/EmbeddingResult ↔ Block, Storage-Write
/// skip-index) is cross-Feature → integration. The op_logger audit sink is
/// optional/nullable for the same reason.
class DataCleaner {
public:
    /// @param config     resolved cleaning config (post ConfigResolver merge)
    /// @param op_logger  operation-log audit sink; may be nullptr. When
    ///                   present, B2 path stats are logged there (not in the API
    ///                   response). Default nullptr keeps data cleaning standalone.
    explicit DataCleaner(
        const CleaningConfig& config,
        std::shared_ptr<operation_log::IOperationLogger> op_logger = nullptr);

    /// Validate the config (threshold ∈ [0,1], max_chunk_chars > 0). Returns the
    /// matching CX_ERR_CLEANING_* Status on violation. Called by the ctor's consumers
    /// before use; exposed for the pipeline to fail fast / surface to the Agent.
    Status ValidateConfig() const;

    /// Replace the active cleaning config (D3.5 NS-override wiring). The SPCPipeline
    /// owns one DataCleaner and re-resolves the effective CleaningConfig per task
    /// (global ← NS cleaning_config, §3.4) before Dedup/DetectAnomaly, so the owned
    /// instance must be reconfigurable. Dedup/DetectAnomaly read config_ on each
    /// call, so this takes effect on the next call. Not thread-safe with concurrent
    /// Dedup/DetectAnomaly on the same instance (the pipeline runs one task at a
    /// time per worker over its own DataCleaner — wire⑤ per-task façade model).
    void SetConfig(const CleaningConfig& config) { config_ = config; }

    /// The active cleaning config (post any SetConfig). Exposed for tests / the
    /// pipeline to confirm the resolved values took effect.
    const CleaningConfig& config() const { return config_; }

    /// ★ Main entry 1: dedup (in-place; removes duplicates). D2 chaining: exact SHA-256 of
    /// chunk_text, then semantic cosine over the upstream embeddings (D3). No-op
    /// (returns 0 removed) when dedup_enabled=false. TraceContext: Phase-1 noop.
    DedupResult Dedup(std::vector<Block>& blocks,
                      const observability::TraceContext* ctx = nullptr);

    /// ★ Main entry 2: anomaly detection (in-place mark). Sets flags_ext bit2 kFlagExtIsAnomalous
    /// + metadata "cleaning.anomaly_reason" / "cleaning.skip_index". Anomalous
    /// blocks are NOT removed — Storage Write reads the flag to skip P-HNSW (D5).
    /// No-op when anomaly_detection_enabled=false.
    AnomalyResult DetectAnomaly(std::vector<Block>& blocks,
                                const observability::TraceContext* ctx = nullptr);

    /// Convenience: the §5.2 A-class summary from a dedup + anomaly run over an
    /// `input_count`-sized batch (chunks_input / chunks_indexed /
    /// chunks_skipped_dedup / chunks_marked_anomalous).
    static CleaningResult Summarize(int input_count, const DedupResult& dedup,
                                    const AnomalyResult& anomaly);

    /// ★ Cleaning-plugin seam — data cleaning ships the stub; the
    /// full plugin ecosystem lands with the plugin API.
    void RegisterPlugin(std::function<void(std::vector<Block>&)> plugin);
    void ApplyPlugins(std::vector<Block>& blocks,
                      const observability::TraceContext* ctx = nullptr);

private:
    CleaningConfig config_;
    std::shared_ptr<operation_log::IOperationLogger> op_logger_;

    std::vector<std::function<void(std::vector<Block>&)>> plugins_;
};

/// ICleaningPlugin stub abstract base. Data cleaning gives the
/// signature; the plugin API implements the full ecosystem (interface + sample plugins +
/// loader + docs).
class ICleaningPlugin {
public:
    virtual ~ICleaningPlugin() = default;
    virtual const char* Name() const = 0;          ///< plugin name (log/metric label)
    virtual void Apply(std::vector<Block>& blocks) = 0;  ///< in-place transform
};

/// Anomaly flag — the frozen bit2 of BlockFlagsExt. Mirrors
/// cortrix::kFlagExtIsAnomalous so the cleaning code reads self-documenting; the SoT is
/// common/block_types.h (V14 J1 whole-bitmap lock).
constexpr uint8_t kBlockFlagExtAnomalous = 0x04;

/// True iff `block` was marked anomalous (Storage Write reads this to skip
/// P-HNSW — D5). Equivalent to the metadata "cleaning.skip_index" == true.
bool ShouldSkipIndex(const Block& block);

}  // namespace cortrix::spc
