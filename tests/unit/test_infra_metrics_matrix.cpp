// Infra/standalone metrics — exhaustive matrix coverage for RerankerMetrics,
// NsPoolMetrics, ParentChunkStoreMetrics, ImportMetrics, MetadataMetrics,
// OplogMetrics, DeployMetrics (enum ToString, label-combo counters, gauges,
// histogram boundaries, render, reset). Separate suite namespaces from the
// existing per-feature metric tests.
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <tuple>

#include "cortrix/deploy/deploy_metrics.h"
#include "cortrix/import/import_metrics.h"
#include "cortrix/metadata/metadata_metrics.h"
#include "cortrix/observability/oplog_metrics.h"
#include "cortrix/reranker/reranker_metrics.h"
#include "cortrix/resource/ns_pool_metrics.h"
#include "cortrix/store/parent_chunk_store_metrics.h"

// ============================ Reranker =================================
namespace cortrix::reranker {
namespace rr_matrix {

using CF = RerankerMetrics::CoremlFallbackReason;
using FT = RerankerMetrics::FailedTaskReason;

class RerankerFx : public ::testing::Test {
protected:
    void SetUp() override { RerankerMetrics::Instance().ResetForTest(); }
    void TearDown() override { RerankerMetrics::Instance().ResetForTest(); }
    RerankerMetrics& m() { return RerankerMetrics::Instance(); }
};

TEST_F(RerankerFx, ToStringAll) {
    EXPECT_STREQ(ToString(CF::kEpInitFailed), "ep_init_failed");
    EXPECT_STREQ(ToString(CF::kUnsupportedPlatform), "unsupported_platform");
    EXPECT_STREQ(ToString(FT::kOnnxException), "onnx_exception");
    EXPECT_STREQ(ToString(FT::kOom), "oom");
    EXPECT_STREQ(ToString(FT::kTimeout), "timeout");
}

class RerankerCoremlMatrix : public RerankerFx, public ::testing::WithParamInterface<CF> {};
TEST_P(RerankerCoremlMatrix, Increments) {
    m().RecordCoremlFallback(GetParam());
    EXPECT_EQ(m().CoremlFallbackCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, RerankerCoremlMatrix,
                         ::testing::Values(CF::kEpInitFailed,
                                           CF::kUnsupportedPlatform));

class RerankerFailedMatrix : public RerankerFx, public ::testing::WithParamInterface<FT> {};
TEST_P(RerankerFailedMatrix, Increments) {
    m().RecordFailedTask(GetParam());
    EXPECT_EQ(m().FailedTasksCount(GetParam()), 1u);
    for (FT f : {FT::kOnnxException, FT::kOom, FT::kTimeout})
        if (f != GetParam()) EXPECT_EQ(m().FailedTasksCount(f), 0u);
}
INSTANTIATE_TEST_SUITE_P(All, RerankerFailedMatrix,
                         ::testing::Values(FT::kOnnxException, FT::kOom,
                                           FT::kTimeout));

TEST_F(RerankerFx, ActiveEpGauge) {
    m().SetActiveEp("coreml");
    EXPECT_EQ(m().ActiveEp(), "coreml");
    m().SetActiveEp("cpu");
    EXPECT_EQ(m().ActiveEp(), "cpu");
}

TEST_F(RerankerFx, CircuitBreakerGaugeAndTrips) {
    m().SetCircuitBreakerState(1);
    EXPECT_EQ(m().CircuitBreakerState(), 1);
    m().RecordCircuitBreakerTrip();
    m().RecordCircuitBreakerTrip();
    EXPECT_EQ(m().CircuitBreakerTripsCount(), 2u);
}

TEST_F(RerankerFx, TruncatedQueueAndExtremelyLong) {
    m().RecordTruncated();
    m().RecordExtremelyLong();
    m().SetQueueDepth(7);
    EXPECT_EQ(m().TruncatedCount(), 1u);
    EXPECT_EQ(m().ExtremelyLongCount(), 1u);
    EXPECT_EQ(m().QueueDepth(), 7);
}

// score_duration histogram bounds {0.05,0.1,0.2,0.5,1,1.5,2,5}s.
TEST_F(RerankerFx, ScoreDurationHistogram) {
    m().ObserveScoreDuration(0.05);  // exactly first bound
    m().ObserveScoreDuration(0.01);  // under
    m().ObserveScoreDuration(99.0);  // above largest -> +Inf
    EXPECT_EQ(m().ScoreDurationCount(), 3u);
    EXPECT_NEAR(m().ScoreDurationSum(), 0.05 + 0.01 + 99.0, 1e-6);
}

TEST_F(RerankerFx, RenderHelpType) {
    m().RecordCoremlFallback(CF::kEpInitFailed);
    m().SetActiveEp("cpu");
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_reranker_coreml_fallback_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_reranker_active_ep gauge"),
              std::string::npos);
}

TEST_F(RerankerFx, Reset) {
    m().RecordCoremlFallback(CF::kEpInitFailed);
    m().RecordFailedTask(FT::kOom);
    m().SetCircuitBreakerState(2);
    m().RecordCircuitBreakerTrip();
    m().RecordTruncated();
    m().SetQueueDepth(3);
    m().ObserveScoreDuration(0.05);
    m().ResetForTest();
    EXPECT_EQ(m().CoremlFallbackCount(CF::kEpInitFailed), 0u);
    EXPECT_EQ(m().FailedTasksCount(FT::kOom), 0u);
    EXPECT_EQ(m().CircuitBreakerState(), 0);
    EXPECT_EQ(m().CircuitBreakerTripsCount(), 0u);
    EXPECT_EQ(m().TruncatedCount(), 0u);
    EXPECT_EQ(m().QueueDepth(), 0);
    EXPECT_EQ(m().ScoreDurationCount(), 0u);
}

}  // namespace rr_matrix
}  // namespace cortrix::reranker

// ============================ NsPool ===================================
namespace cortrix::resource {
namespace nspool_matrix {

using RR = NsPoolMetrics::RejectReason;

class NsPoolFx : public ::testing::Test {
protected:
    void SetUp() override { NsPoolMetrics::Instance().ResetForTest(); }
    void TearDown() override { NsPoolMetrics::Instance().ResetForTest(); }
    NsPoolMetrics& m() { return NsPoolMetrics::Instance(); }
};

TEST_F(NsPoolFx, ToStringAll) {
    EXPECT_STREQ(ToString(RR::kNsCountExceeded), "ns_count_exceeded");
    EXPECT_STREQ(ToString(RR::kMemoryExceeded), "memory_exceeded");
}

class NsPoolRejectMatrix : public NsPoolFx, public ::testing::WithParamInterface<RR> {};
TEST_P(NsPoolRejectMatrix, Increments) {
    m().RecordRejectedCreate(GetParam());
    EXPECT_EQ(m().RejectedCreateCount(GetParam()), 1u);
    for (RR r : {RR::kNsCountExceeded, RR::kMemoryExceeded})
        if (r != GetParam()) EXPECT_EQ(m().RejectedCreateCount(r), 0u);
}
INSTANTIATE_TEST_SUITE_P(All, NsPoolRejectMatrix,
                         ::testing::Values(RR::kNsCountExceeded,
                                           RR::kMemoryExceeded));

TEST_F(NsPoolFx, GaugesAndStartupFailures) {
    m().SetSize(42);
    EXPECT_EQ(m().Size(), 42);
    m().SetMemoryBudgetUsedBytes(1024);
    EXPECT_EQ(m().MemoryBudgetUsedBytes(), 1024);
    m().AddStartupLoadFailures(2);
    m().AddStartupLoadFailures(1);
    EXPECT_EQ(m().StartupLoadFailuresCount(), 3u);
}

// duration histograms bounds {0.1,0.5,1,3,5,10,30,60}s.
TEST_F(NsPoolFx, DurationHistogramsRender) {
    m().ObserveStartupLoadDuration(0.1);   // exactly first bound
    m().ObserveStartupLoadDuration(999.0);  // above largest -> +Inf
    m().ObserveNsLoadDuration(0.05);       // under first bound
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_ns_pool_startup_load_duration_seconds histogram"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_ns_pool_startup_load_duration_seconds_count 2"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_ns_pool_ns_load_duration_seconds_count 1"),
              std::string::npos);
}

TEST_F(NsPoolFx, RenderHelpTypeNoHighCardinality) {
    m().SetSize(1);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_ns_pool_size gauge"), std::string::npos);
    EXPECT_EQ(out.find("namespace_id="), std::string::npos);
    EXPECT_EQ(out.find("ns_id="), std::string::npos);
}

TEST_F(NsPoolFx, Reset) {
    m().SetSize(5);
    m().SetMemoryBudgetUsedBytes(100);
    m().RecordRejectedCreate(RR::kMemoryExceeded);
    m().AddStartupLoadFailures(3);
    m().ObserveStartupLoadDuration(0.1);
    m().ResetForTest();
    EXPECT_EQ(m().Size(), 0);
    EXPECT_EQ(m().MemoryBudgetUsedBytes(), 0);
    EXPECT_EQ(m().RejectedCreateCount(RR::kMemoryExceeded), 0u);
    EXPECT_EQ(m().StartupLoadFailuresCount(), 0u);
}

}  // namespace nspool_matrix
}  // namespace cortrix::resource

// ====================== ParentChunkStore ===============================
namespace cortrix::store {
namespace pcs_matrix {

using LR = ParentChunkStoreMetrics::LookupResult;

class ParentChunkStoreFx : public ::testing::Test {
protected:
    void SetUp() override { ParentChunkStoreMetrics::Instance().ResetForTest(); }
    void TearDown() override {
        ParentChunkStoreMetrics::Instance().ResetForTest();
    }
    ParentChunkStoreMetrics& m() { return ParentChunkStoreMetrics::Instance(); }
};

TEST_F(ParentChunkStoreFx, ToStringAll) {
    EXPECT_STREQ(ToString(LR::kHit), "hit");
    EXPECT_STREQ(ToString(LR::kMiss), "miss");
}

class ParentChunkStoreLookupMatrix : public ParentChunkStoreFx, public ::testing::WithParamInterface<LR> {};
TEST_P(ParentChunkStoreLookupMatrix, Increments) {
    m().RecordLookup(GetParam());
    m().RecordLookup(GetParam());
    EXPECT_EQ(m().LookupCount(GetParam()), 2u);
    for (LR r : {LR::kHit, LR::kMiss})
        if (r != GetParam()) EXPECT_EQ(m().LookupCount(r), 0u);
}
INSTANTIATE_TEST_SUITE_P(All, ParentChunkStoreLookupMatrix,
                         ::testing::Values(LR::kHit, LR::kMiss));

TEST_F(ParentChunkStoreFx, IsolatedInstanceIndependentOfSingleton) {
    ParentChunkStoreMetrics local;
    local.RecordLookup(LR::kHit);
    EXPECT_EQ(local.LookupCount(LR::kHit), 1u);
    // the process-wide singleton is untouched.
    EXPECT_EQ(m().LookupCount(LR::kHit), 0u);
}

// lookup_latency histogram bounds {1e-4,...,0.05}s.
TEST_F(ParentChunkStoreFx, LookupLatencyHistogram) {
    m().ObserveLookupLatency(0.0001);  // exactly first bound
    m().ObserveLookupLatency(0.00001);  // under
    m().ObserveLookupLatency(9.0);     // above largest -> +Inf
    EXPECT_EQ(m().LookupLatencyCount(), 3u);
    EXPECT_NEAR(m().LookupLatencySum(), 0.0001 + 0.00001 + 9.0, 1e-6);
}

TEST_F(ParentChunkStoreFx, RenderHelpType) {
    m().RecordLookup(LR::kHit);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_parent_chunk_store_lookup_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_parent_chunk_store_lookup_total{result=\"hit\"} 1"),
              std::string::npos);
    EXPECT_EQ(out.find("parent_id="), std::string::npos);
}

TEST_F(ParentChunkStoreFx, Reset) {
    m().RecordLookup(LR::kHit);
    m().ObserveLookupLatency(0.0001);
    m().ResetForTest();
    EXPECT_EQ(m().LookupCount(LR::kHit), 0u);
    EXPECT_EQ(m().LookupLatencyCount(), 0u);
}

}  // namespace pcs_matrix
}  // namespace cortrix::store

// ============================ Import ==================================
namespace cortrix::import {
namespace import_matrix {

using IO = ImportMetrics::ImportOutcome;
using QT = ImportMetrics::QueryType;

class ImportFx : public ::testing::Test {
protected:
    void SetUp() override { ImportMetrics::Instance().ResetForTest(); }
    void TearDown() override { ImportMetrics::Instance().ResetForTest(); }
    ImportMetrics& m() { return ImportMetrics::Instance(); }
};

TEST_F(ImportFx, ToStringAll) {
    EXPECT_STREQ(ToString(IO::kSuccess), "success");
    EXPECT_STREQ(ToString(IO::kFailed), "failed");
    EXPECT_STREQ(ToString(IO::kCancelled), "cancelled");
    EXPECT_STREQ(ToString(QT::kTableFilter), "table_filter");
    EXPECT_STREQ(ToString(QT::kCustomSql), "custom_sql");
}

class ImportOutcomeMatrix : public ImportFx, public ::testing::WithParamInterface<IO> {};
TEST_P(ImportOutcomeMatrix, Increments) {
    m().RecordImport(GetParam());
    EXPECT_EQ(m().ImportsCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, ImportOutcomeMatrix,
                         ::testing::Values(IO::kSuccess, IO::kFailed,
                                           IO::kCancelled));

TEST_F(ImportFx, RowsAndConnectionsAndQueueGauges) {
    m().AddRowsImported(100);
    m().AddRowsImported(50);
    EXPECT_EQ(m().RowsImportedTotal(), 150u);
    m().IncConnectionsActive();
    m().IncConnectionsActive();
    m().DecConnectionsActive();
    EXPECT_EQ(m().ConnectionsActive(), 1);
    m().SetQueueDepth(8);
    EXPECT_EQ(m().QueueDepth(), 8);
}

TEST_F(ImportFx, ImportAndQueryDurationHistogramsRender) {
    m().ObserveImportDuration("per_row", 0.1);
    m().ObserveImportDuration("merge", 999.0);  // above largest -> +Inf
    m().ObserveQueryDuration(QT::kCustomSql, 0.05);
    std::string out = m().Render();
    EXPECT_NE(out.find("# TYPE cortrix_import_duration_seconds histogram"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_import_query_duration_seconds histogram"),
              std::string::npos);
}

TEST_F(ImportFx, RenderHelpTypeNoHighCardinality) {
    m().RecordImport(IO::kSuccess);
    std::string out = m().Render();
    EXPECT_NE(out.find("# TYPE cortrix_import_imports_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_import_imports_total{status=\"success\"} 1"),
              std::string::npos);
    EXPECT_EQ(out.find("namespace="), std::string::npos);
    EXPECT_EQ(out.find("tenant_id="), std::string::npos);
}

TEST_F(ImportFx, Reset) {
    m().RecordImport(IO::kSuccess);
    m().AddRowsImported(10);
    m().IncConnectionsActive();
    m().SetQueueDepth(3);
    m().ResetForTest();
    EXPECT_EQ(m().ImportsCount(IO::kSuccess), 0u);
    EXPECT_EQ(m().RowsImportedTotal(), 0u);
    EXPECT_EQ(m().ConnectionsActive(), 0);
    EXPECT_EQ(m().QueueDepth(), 0);
}

}  // namespace import_matrix
}  // namespace cortrix::import

// ============================ Metadata =================================
namespace cortrix::metadata {
namespace metadata_matrix {

using GS = MetadataMetrics::GenStatus;
using GSrc = MetadataMetrics::GenSource;

constexpr std::array<GS, 3> kStatuses{GS::kSuccess, GS::kPartialWarning,
                                      GS::kFailed};
constexpr std::array<GSrc, 3> kSources{GSrc::kApiUpload, GSrc::kBatchImport,
                                       GSrc::kReUpload};

class MetadataFx : public ::testing::Test {
protected:
    void SetUp() override { MetadataMetrics::Instance().ResetForTest(); }
    void TearDown() override { MetadataMetrics::Instance().ResetForTest(); }
    MetadataMetrics& m() { return MetadataMetrics::Instance(); }
};

TEST_F(MetadataFx, ToStringAll) {
    EXPECT_STREQ(ToString(GS::kSuccess), "success");
    EXPECT_STREQ(ToString(GS::kPartialWarning), "partial_warning");
    EXPECT_STREQ(ToString(GS::kFailed), "failed");
    EXPECT_STREQ(ToString(GSrc::kApiUpload), "api_upload");
    EXPECT_STREQ(ToString(GSrc::kBatchImport), "batch_import");
    EXPECT_STREQ(ToString(GSrc::kReUpload), "re_upload");
}

using GenParam = std::tuple<GS, GSrc>;
class MetadataGenMatrix : public MetadataFx, public ::testing::WithParamInterface<GenParam> {};
TEST_P(MetadataGenMatrix, IncrementsOnlyTargetCell) {
    auto [st, src] = GetParam();
    m().RecordGenerated(st, src);
    EXPECT_EQ(m().GeneratedCount(st, src), 1u);
    for (GS s : kStatuses)
        for (GSrc sr : kSources)
            if (!(s == st && sr == src)) EXPECT_EQ(m().GeneratedCount(s, sr), 0u);
}
INSTANTIATE_TEST_SUITE_P(AllCells, MetadataGenMatrix,
                         ::testing::Combine(::testing::ValuesIn(kStatuses),
                                            ::testing::ValuesIn(kSources)));

TEST_F(MetadataFx, BlockCountGaugeAndFieldMissing) {
    m().SetBlockCount(99);
    EXPECT_EQ(m().BlockCount(), 99);
    m().RecordFieldMissing("title");
    m().RecordFieldMissing("title");
    m().RecordFieldMissing("author");
    EXPECT_EQ(m().FieldMissingCount("title"), 2u);
    EXPECT_EQ(m().FieldMissingCount("author"), 1u);
    EXPECT_EQ(m().FieldMissingCount("unseen"), 0u);
}

TEST_F(MetadataFx, SizeAndDurationHistogramsRender) {
    m().ObserveMetadataSize(1024.0);
    m().ObserveGenerateDuration(0.01);
    m().ObserveGenerateDuration(999.0);  // above largest -> +Inf
    std::string out = m().Render();
    EXPECT_NE(out.find("# TYPE cortrix_metadata_size_bytes histogram"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_metadata_block_generate_duration_seconds histogram"),
              std::string::npos);
}

TEST_F(MetadataFx, RenderHelpTypeNoHighCardinality) {
    m().RecordGenerated(GS::kSuccess, GSrc::kApiUpload);
    std::string out = m().Render();
    EXPECT_NE(out.find("# TYPE cortrix_metadata_block_generated_total counter"),
              std::string::npos);
    EXPECT_EQ(out.find("doc_id="), std::string::npos);
    EXPECT_EQ(out.find("ns_id="), std::string::npos);
}

TEST_F(MetadataFx, Reset) {
    m().RecordGenerated(GS::kSuccess, GSrc::kApiUpload);
    m().SetBlockCount(10);
    m().RecordFieldMissing("title");
    m().ResetForTest();
    EXPECT_EQ(m().GeneratedCount(GS::kSuccess, GSrc::kApiUpload), 0u);
    EXPECT_EQ(m().BlockCount(), 0);
    EXPECT_EQ(m().FieldMissingCount("title"), 0u);
}

}  // namespace metadata_matrix
}  // namespace cortrix::metadata

// ============================ Oplog ===================================
namespace cortrix::observability {
namespace oplog_matrix {

using CR = OplogMetrics::CleanupReason;

class OplogFx : public ::testing::Test {
protected:
    void SetUp() override { OplogMetrics::Instance().ResetForTest(); }
    void TearDown() override { OplogMetrics::Instance().ResetForTest(); }
    OplogMetrics& m() { return OplogMetrics::Instance(); }
};

TEST_F(OplogFx, ToStringAll) {
    EXPECT_STREQ(ToString(CR::kAge), "age");
    EXPECT_STREQ(ToString(CR::kQuota), "quota");
}

TEST_F(OplogFx, WritesByActionResourceType) {
    m().RecordWrite("memory_create", "memory");
    m().RecordWrite("memory_create", "memory");
    m().RecordWrite("memory_edit", "memory");
    EXPECT_EQ(m().WriteCount("memory_create", "memory"), 2u);
    EXPECT_EQ(m().WriteCount("memory_edit", "memory"), 1u);
    EXPECT_EQ(m().WriteCount("nonexistent", "memory"), 0u);
}

class OplogCleanupMatrix : public OplogFx, public ::testing::WithParamInterface<CR> {};
TEST_P(OplogCleanupMatrix, Increments) {
    m().RecordCleanupDeleted(GetParam(), 5);
    m().RecordCleanupDeleted(GetParam(), 3);
    EXPECT_EQ(m().CleanupDeletedCount(GetParam()), 8u);
}
INSTANTIATE_TEST_SUITE_P(All, OplogCleanupMatrix,
                         ::testing::Values(CR::kAge, CR::kQuota));

TEST_F(OplogFx, CleanupFailedAndSizeGauge) {
    m().RecordCleanupFailed();
    m().RecordCleanupFailed();
    EXPECT_EQ(m().CleanupFailedCount(), 2u);
    m().SetSizeRows(1234);
    EXPECT_EQ(m().SizeRows(), 1234);
}

// query_latency histogram keyed by filter_dimensions 0..8 (clamped).
class OplogFilterDimMatrix : public OplogFx, public ::testing::WithParamInterface<int> {};
TEST_P(OplogFilterDimMatrix, IndependentSeries) {
    m().ObserveQueryLatency(GetParam(), 10);
    EXPECT_EQ(m().QueryLatencyCount(GetParam()), 1u);
}
INSTANTIATE_TEST_SUITE_P(All, OplogFilterDimMatrix,
                         ::testing::Values(0, 1, 2, 3, 4, 5, 6, 7, 8));

TEST_F(OplogFx, FilterDimensionsClampedAboveMax) {
    m().ObserveQueryLatency(50, 10);  // clamps into the dim=8 series
    EXPECT_EQ(m().QueryLatencyCount(8), 1u);
}

// cleanup_duration histogram bounds {0.01,0.05,0.1,0.5,1,5,10,30}s (passed ms).
TEST_F(OplogFx, CleanupDurationHistogram) {
    m().ObserveCleanupDuration(10);     // 0.01s exactly first bound
    m().ObserveCleanupDuration(5);      // under
    m().ObserveCleanupDuration(999999);  // above largest -> +Inf
    EXPECT_EQ(m().CleanupDurationCount(), 3u);
}

TEST_F(OplogFx, RenderHelpTypeNoHighCardinality) {
    m().RecordWrite("memory_create", "memory");
    m().SetSizeRows(1);
    std::string out = m().RenderOpenMetrics();
    EXPECT_NE(out.find("# TYPE cortrix_oplog_writes_total counter"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_oplog_size_rows gauge"),
              std::string::npos);
    EXPECT_EQ(out.find("user_id="), std::string::npos);
    EXPECT_EQ(out.find("namespace_id="), std::string::npos);
    EXPECT_EQ(out.find("trace_id="), std::string::npos);
}

TEST_F(OplogFx, Reset) {
    m().RecordWrite("memory_create", "memory");
    m().RecordCleanupDeleted(CR::kAge, 4);
    m().RecordCleanupFailed();
    m().SetSizeRows(99);
    m().ObserveQueryLatency(2, 10);
    m().ObserveCleanupDuration(10);
    m().ResetForTest();
    EXPECT_EQ(m().WriteCount("memory_create", "memory"), 0u);
    EXPECT_EQ(m().CleanupDeletedCount(CR::kAge), 0u);
    EXPECT_EQ(m().CleanupFailedCount(), 0u);
    EXPECT_EQ(m().SizeRows(), 0);
    EXPECT_EQ(m().QueryLatencyCount(2), 0u);
    EXPECT_EQ(m().CleanupDurationCount(), 0u);
}

}  // namespace oplog_matrix
}  // namespace cortrix::observability

// ============================ Deploy ===================================
namespace cortrix::deploy {
namespace deploy_matrix {

class DeployFx : public ::testing::Test {
protected:
    void SetUp() override { DeployMetrics::Instance().ResetForTest(); }
    void TearDown() override { DeployMetrics::Instance().ResetForTest(); }
    DeployMetrics& m() { return DeployMetrics::Instance(); }
};

TEST_F(DeployFx, DiskUsageGaugeLastWriteWins) {
    m().SetDiskUsageRatio(0.42);
    EXPECT_NEAR(m().DiskUsageRatio(), 0.42, 1e-9);
    m().SetDiskUsageRatio(0.9);
    EXPECT_NEAR(m().DiskUsageRatio(), 0.9, 1e-9);
}

TEST_F(DeployFx, ShutdownStatusGauge) {
    m().SetShutdownStatus(0);
    EXPECT_EQ(m().ShutdownStatus(), 0);
    m().SetShutdownStatus(2);
    EXPECT_EQ(m().ShutdownStatus(), 2);
}

TEST_F(DeployFx, RenderEmitsOwnedGaugesAndBuildInfo) {
    m().SetDiskUsageRatio(0.5);
    m().SetShutdownStatus(1);
    m().SetBuildInfo("1.0.0-rc.1", "abc123", "2026-06-21");
    m().MarkStart();
    std::string out = m().Render();
    EXPECT_NE(out.find("# TYPE cortrix_disk_usage_ratio gauge"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_shutdown_status gauge"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_build_info"), std::string::npos);
    EXPECT_NE(out.find("version=\"1.0.0-rc.1\""), std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_uptime_seconds gauge"),
              std::string::npos);
    // bloom filter gauges are NOT part of DeployMetrics::Render().
    EXPECT_EQ(out.find("cortrix_bloom_filter_estimated_count"),
              std::string::npos);
}

TEST_F(DeployFx, RenderNoHighCardinalityLabels) {
    m().SetDiskUsageRatio(0.5);
    std::string out = m().Render();
    EXPECT_EQ(out.find("ns_id="), std::string::npos);
    EXPECT_EQ(out.find("doc_id="), std::string::npos);
}

TEST_F(DeployFx, BloomFilterRenderFromLambdaSources) {
    BloomFilterMetricSource src;
    src.estimated_count = [] { return uint64_t{1000}; };
    src.false_positive_rate = [] { return 0.01; };
    src.last_rebuild_epoch_sec = [] { return int64_t{1700000000}; };
    src.ready = [] { return true; };
    std::string out = RenderBloomFilterMetrics(src);
    EXPECT_NE(out.find("cortrix_bloom_filter_estimated_count{subsystem=\"catalog\"} 1000"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_bloom_filter_ready{subsystem=\"catalog\"} 1"),
              std::string::npos);
}

TEST_F(DeployFx, BloomFilterNeverRebuiltEmitsZeroTimestamp) {
    BloomFilterMetricSource src;
    src.estimated_count = [] { return uint64_t{0}; };
    src.false_positive_rate = [] { return 0.0; };
    src.last_rebuild_epoch_sec = [] { return int64_t{-1}; };  // never -> 0
    src.ready = [] { return false; };
    std::string out = RenderBloomFilterMetrics(src);
    EXPECT_NE(out.find("cortrix_bloom_filter_last_rebuild_ts{subsystem=\"catalog\"} 0"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_bloom_filter_ready{subsystem=\"catalog\"} 0"),
              std::string::npos);
}

TEST_F(DeployFx, Reset) {
    m().SetDiskUsageRatio(0.7);
    m().SetShutdownStatus(2);
    m().ResetForTest();
    EXPECT_NEAR(m().DiskUsageRatio(), 0.0, 1e-9);
    EXPECT_EQ(m().ShutdownStatus(), 0);
}

}  // namespace deploy_matrix
}  // namespace cortrix::deploy
