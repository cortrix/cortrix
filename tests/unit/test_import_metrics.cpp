#include <gtest/gtest.h>

#include <string>

#include "cortrix/import/import_metrics.h"

// S6 coverage: DB import observability metrics (§5.5) — counters/gauges + OpenMetrics
// render + the v1.0.2 high-cardinality-label removal (no namespace / tenant_id
// labels). Process-global singleton, so each test resets first.
namespace cortrix::import {
namespace {

TEST(ImportMetricsTest, ImportOutcomeCountersByStatus) {
    auto& m = ImportMetrics::Instance();
    m.ResetForTest();
    m.RecordImport(ImportMetrics::ImportOutcome::kSuccess);
    m.RecordImport(ImportMetrics::ImportOutcome::kSuccess);
    m.RecordImport(ImportMetrics::ImportOutcome::kFailed);
    m.RecordImport(ImportMetrics::ImportOutcome::kCancelled);
    EXPECT_EQ(m.ImportsCount(ImportMetrics::ImportOutcome::kSuccess), 2u);
    EXPECT_EQ(m.ImportsCount(ImportMetrics::ImportOutcome::kFailed), 1u);
    EXPECT_EQ(m.ImportsCount(ImportMetrics::ImportOutcome::kCancelled), 1u);
}

TEST(ImportMetricsTest, RowsImportedAndGauges) {
    auto& m = ImportMetrics::Instance();
    m.ResetForTest();
    m.AddRowsImported(100);
    m.AddRowsImported(50);
    m.AddRowsImported(-5);  // ignored (negative)
    EXPECT_EQ(m.RowsImportedTotal(), 150u);

    m.IncConnectionsActive();
    m.IncConnectionsActive();
    m.DecConnectionsActive();
    EXPECT_EQ(m.ConnectionsActive(), 1);

    m.SetQueueDepth(7);
    EXPECT_EQ(m.QueueDepth(), 7);
}

TEST(ImportMetricsTest, RenderEmitsStableNamesWithoutHighCardinalityLabels) {
    auto& m = ImportMetrics::Instance();
    m.ResetForTest();
    m.RecordImport(ImportMetrics::ImportOutcome::kSuccess);
    m.AddRowsImported(10);
    std::string out = m.Render();

    EXPECT_NE(out.find("cortrix_import_imports_total{status=\"success\"} 1"), std::string::npos);
    EXPECT_NE(out.find("cortrix_import_rows_imported_total 10"), std::string::npos);
    EXPECT_NE(out.find("cortrix_import_connections_active"), std::string::npos);
    EXPECT_NE(out.find("cortrix_import_tasks_queue_depth"), std::string::npos);
    // §5.5 note: the removed high-cardinality labels must NOT appear.
    EXPECT_EQ(out.find("namespace="), std::string::npos);
    EXPECT_EQ(out.find("tenant_id="), std::string::npos);
}

TEST(ImportMetricsTest, DurationHistogramsObserveAndRender) {
    auto& m = ImportMetrics::Instance();
    m.ResetForTest();
    m.ObserveImportDuration("per_row", 0.1);   // → le="0.25" bucket
    m.ObserveImportDuration("merge", 2.0);     // → le="5" bucket
    m.ObserveQueryDuration(ImportMetrics::QueryType::kTableFilter, 0.02);
    m.ObserveQueryDuration(ImportMetrics::QueryType::kCustomSql, 7.5);
    std::string out = m.Render();
    // §5.5 — both histograms present (TYPE + per-label cumulative _bucket/_sum/_count).
    EXPECT_NE(out.find("# TYPE cortrix_import_duration_seconds histogram"), std::string::npos);
    EXPECT_NE(out.find("cortrix_import_duration_seconds_bucket{text_strategy=\"per_row\",le=\"0.25\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_import_duration_seconds_count{text_strategy=\"merge\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("# TYPE cortrix_import_query_duration_seconds histogram"), std::string::npos);
    EXPECT_NE(out.find("cortrix_import_query_duration_seconds_count{query_type=\"table_filter\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("cortrix_import_query_duration_seconds_count{query_type=\"custom_sql\"} 1"),
              std::string::npos);
    EXPECT_NE(out.find("le=\"+Inf\"} 1"), std::string::npos);
    // still no high-cardinality labels.
    EXPECT_EQ(out.find("namespace="), std::string::npos);
    EXPECT_EQ(out.find("tenant_id="), std::string::npos);
}

TEST(ImportMetricsTest, LabelStringsAreStable) {
    EXPECT_STREQ(ToString(ImportMetrics::ImportOutcome::kSuccess), "success");
    EXPECT_STREQ(ToString(ImportMetrics::ImportOutcome::kFailed), "failed");
    EXPECT_STREQ(ToString(ImportMetrics::ImportOutcome::kCancelled), "cancelled");
    EXPECT_STREQ(ToString(ImportMetrics::QueryType::kTableFilter), "table_filter");
    EXPECT_STREQ(ToString(ImportMetrics::QueryType::kCustomSql), "custom_sql");
}

}  // namespace
}  // namespace cortrix::import
