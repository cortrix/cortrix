// Watcher Fan-out (TD-WATCHER-001) — DirWatcherRegistry unit tests (S1).
//
// Verifies the fan-out refactor: one OS FileWatcher per directory whose events
// are dispatched to every subscribed namespace's DirectoryImporter. Covers the
// S1 DoD test matrix: Subscribe (single / multi / idempotent /
// not-found / empty), Unsubscribe (single / last / not-found), RemoveWatch,
// FanOut (all-NS / one-importer-fail), ListWatches, TriggerScan, and the
// watchers.json v1->v2 SaveLoad round-trip.
//
// FileWatcher ownership lives in the registry; importer file-processing is
// exercised with local mocks matching the real CortrixStore/SPCManager vtables
// (same approach as tests/integration/test_directory_import.cpp).
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "cortrix/catalog/catalog_types.h"  // NSMetadata (router CreateNamespace)
#include "cortrix/connector/dir_watcher_registry.h"
#include "cortrix/spc/spc_pipeline.h"  // complete type for MockSPCManager dtor
#include "mock_spc_manager.h"
#include "ns_pool_test_helper.h"  // integration wiring: NsPoolHarness (namespace pool INamespacePool)

using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;

namespace cortrix {

// Test-only accessor friend (declared in dir_watcher_registry.h). Drives the
// private FanOutEvents deterministically without OS-watcher event timing.
class DirWatcherRegistryPrivateTest {
public:
    static int SlotCount(const DirWatcherRegistry& r) {
        std::lock_guard<std::mutex> lock(r.mu_);
        return static_cast<int>(r.slots_.size());
    }
    static int ImporterCount(const DirWatcherRegistry& r, const std::string& dir) {
        std::lock_guard<std::mutex> lock(r.mu_);
        for (const auto& s : r.slots_) {
            if (s->canonical_dir == dir) {
                return static_cast<int>(s->importers.size());
            }
        }
        return -1;
    }
    static bool HasOsWatcher(const DirWatcherRegistry& r, const std::string& dir) {
        std::lock_guard<std::mutex> lock(r.mu_);
        for (const auto& s : r.slots_) {
            if (s->canonical_dir == dir) return s->watcher != nullptr;
        }
        return false;
    }
    // Fan out a synthetic event batch to the slot for `dir` (canonical).
    static void FanOut(DirWatcherRegistry& r, const std::string& dir,
                       const std::vector<FileEvent>& events) {
        DirWatcherRegistry::WatcherSlot* slot = nullptr;
        {
            std::lock_guard<std::mutex> lock(r.mu_);
            for (auto& s : r.slots_) {
                if (s->canonical_dir == dir) { slot = s.get(); break; }
            }
        }
        ASSERT_NE(slot, nullptr) << "no slot for " << dir;
        r.FanOutEvents(*slot, events);
    }
};

namespace {

using cortrix::testing::MockSPCManager;

// ---- Fixture ----
//
// integration wiring: DirWatcherRegistry takes a namespace pool resource::INamespacePool (the importers
// it builds acquire a per-request NamespaceFacade from it) plus a catalog INSRouter
// it creates subscribed namespaces through (catalog INSERT + namespace pool AdmitCreate). The
// pool is stood up by the shared NsPoolHarness (real DefaultNamespacePool + mocked
// Catalog routers + capturing FakeIndex); the router here is a standalone MockNSRouter
// whose CreateNamespace is wired to harness_->Admit() so the registry's create path
// genuinely admits into the pool (matching production — this is what makes a fresh
// subscriber's facade.Acquire() resolve).
//
// Tests whose assertions depend on an importer actually processing files (the
// fan-out / TriggerScan SPC-submit counts) end up admitted via that CreateNamespace
// wiring; the explicit AdmitNs() calls are now redundant but kept (idempotent).
// Tests that only assert registry bookkeeping (slot/importer/subscriber counts)
// pass regardless: an importer scan that cannot acquire fails non-fatally and
// Subscribe still returns Ok.
class DirWatcherRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        namespace fs = std::filesystem;
        test_dir_ = fs::temp_directory_path() /
                    ("cortrix_dirwatch_reg_" + std::to_string(::getpid()));
        fs::remove_all(test_dir_);
        fs::create_directories(test_dir_);
        CreateFile("a.txt", "alpha");
        CreateFile("b.md", "# beta");
        canonical_dir_ = fs::canonical(test_dir_).string();

        // Namespace pool lives OUTSIDE the watched test_dir_ (sibling temp dir) so its
        // store.db / catalog files are never seen by the importers' recursive scans.
        harness_ = std::make_unique<cortrix::test::NsPoolHarness>(
            fs::temp_directory_path() /
            ("cortrix_dirwatch_pool_" + std::to_string(::getpid())));

        // The registry now creates namespaces through an INSRouter (catalog INSERT
        // + namespace pool AdmitCreate). Wire the mock router's CreateNamespace to actually
        // admit into the harness pool — mirroring production, where CreateNamespace
        // is what makes a freshly-subscribed NS resolvable by facade.Acquire(). This
        // also means tests no longer need to AdmitNs() by hand (kept idempotent).
        using ::testing::_;
        using ::testing::Invoke;
        using ::testing::Return;
        ON_CALL(ns_router_, CreateNamespace(_))
            .WillByDefault(Invoke([this](const cortrix::catalog::NSMetadata& m) {
                return harness_->Admit(m.namespace_id);  // Ok / AlreadyExists-ish
            }));
    }
    void TearDown() override {
        harness_.reset();  // its dtor removes the pool data_root
        std::error_code ec;
        std::filesystem::remove_all(test_dir_, ec);
    }
    void CreateFile(const std::string& name, const std::string& content) {
        auto p = test_dir_ / name;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out << content;
    }
    // Admit a namespace into the pool so its importer's facade.Acquire() hits.
    // (Now redundant with the CreateNamespace wiring above, but harmless/idempotent
    // and kept so the existing tests read unchanged.)
    void AdmitNs(const std::string& ns) {
        ASSERT_TRUE(harness_->Admit(ns).ok());
    }
    // The namespace pool the registry consumes.
    cortrix::resource::INamespacePool& pool() { return harness_->ipool(); }
    // The catalog router the registry creates namespaces through.
    cortrix::catalog::INSRouter& router() { return ns_router_; }

    std::filesystem::path test_dir_;
    std::string canonical_dir_;
    std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
    ::testing::NiceMock<cortrix::test::MockNSRouter> ns_router_;
    MockSPCManager spc_;
};

// ---- Subscribe ----

TEST_F(DirWatcherRegistryTest, Subscribe_SingleNS) {
    EXPECT_CALL(spc_, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    DirWatcherRegistry reg(pool(), router(), spc_);

    Status s = reg.Subscribe(test_dir_.string(), {"team_a"}, true, /*autostart=*/false);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(1, DirWatcherRegistryPrivateTest::SlotCount(reg));
    EXPECT_EQ(1, DirWatcherRegistryPrivateTest::ImporterCount(reg, canonical_dir_));
    auto watches = reg.ListWatches();
    ASSERT_EQ(1u, watches.size());
    EXPECT_EQ(canonical_dir_, watches[0].directory);
    EXPECT_EQ(1, watches[0].subscriber_count);
}

TEST_F(DirWatcherRegistryTest, Subscribe_MultiNS_OneWatcher) {
    EXPECT_CALL(spc_, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    DirWatcherRegistry reg(pool(), router(), spc_);

    Status s = reg.Subscribe(test_dir_.string(), {"team_a", "team_b", "archive"},
                             true, /*autostart=*/true);
    ASSERT_TRUE(s.ok()) << s.message();

    // 1 slot (=> 1 OS watcher), 3 importers.
    EXPECT_EQ(1, DirWatcherRegistryPrivateTest::SlotCount(reg));
    EXPECT_EQ(3, DirWatcherRegistryPrivateTest::ImporterCount(reg, canonical_dir_));
    EXPECT_TRUE(DirWatcherRegistryPrivateTest::HasOsWatcher(reg, canonical_dir_));
    auto watches = reg.ListWatches();
    ASSERT_EQ(1u, watches.size());
    EXPECT_EQ(3, watches[0].subscriber_count);
    EXPECT_EQ(3u, watches[0].target_namespaces.size());
    EXPECT_TRUE(watches[0].watching);
    reg.StopAll();
}

TEST_F(DirWatcherRegistryTest, Subscribe_Idempotent) {
    EXPECT_CALL(spc_, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    DirWatcherRegistry reg(pool(), router(), spc_);

    ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"team_a"}, true, false).ok());
    // Re-subscribe the same NS: no new importer.
    ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"team_a"}, true, false).ok());
    EXPECT_EQ(1, DirWatcherRegistryPrivateTest::ImporterCount(reg, canonical_dir_));

    // Adding a second NS to the same dir reuses the slot.
    ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"team_a", "team_b"}, true, false).ok());
    EXPECT_EQ(1, DirWatcherRegistryPrivateTest::SlotCount(reg));
    EXPECT_EQ(2, DirWatcherRegistryPrivateTest::ImporterCount(reg, canonical_dir_));
}

TEST_F(DirWatcherRegistryTest, Subscribe_DirNotFound) {
    DirWatcherRegistry reg(pool(), router(), spc_);
    Status s = reg.Subscribe("/no/such/directory/anywhere", {"team_a"}, true, false);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(StatusCode::kNotFound, s.code());
}

TEST_F(DirWatcherRegistryTest, Subscribe_EmptyNS) {
    DirWatcherRegistry reg(pool(), router(), spc_);
    Status s = reg.Subscribe(test_dir_.string(), {}, true, false);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(StatusCode::kInvalidArgument, s.code());
}

// ---- Unsubscribe / RemoveWatch ----

TEST_F(DirWatcherRegistryTest, Unsubscribe_SingleNS) {
    EXPECT_CALL(spc_, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    DirWatcherRegistry reg(pool(), router(), spc_);
    ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"team_a", "team_b", "archive"},
                              true, false).ok());

    Status s = reg.Unsubscribe(test_dir_.string(), "team_b");
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(1, DirWatcherRegistryPrivateTest::SlotCount(reg));  // watcher remains
    EXPECT_EQ(2, DirWatcherRegistryPrivateTest::ImporterCount(reg, canonical_dir_));
    auto watches = reg.ListWatches();
    ASSERT_EQ(1u, watches.size());
    EXPECT_EQ(2, watches[0].subscriber_count);
    // team_b gone, others remain.
    const auto& nss = watches[0].target_namespaces;
    EXPECT_EQ(nss.end(), std::find(nss.begin(), nss.end(), "team_b"));
}

TEST_F(DirWatcherRegistryTest, Unsubscribe_LastNS_DestroysWatcher) {
    EXPECT_CALL(spc_, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    DirWatcherRegistry reg(pool(), router(), spc_);
    ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"only_ns"}, true, false).ok());

    Status s = reg.Unsubscribe(test_dir_.string(), "only_ns");
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(0, DirWatcherRegistryPrivateTest::SlotCount(reg));  // slot destroyed
    EXPECT_TRUE(reg.ListWatches().empty());
}

TEST_F(DirWatcherRegistryTest, Unsubscribe_NotFound) {
    EXPECT_CALL(spc_, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    DirWatcherRegistry reg(pool(), router(), spc_);
    ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"team_a"}, true, false).ok());

    // NS not subscribed -> NotFound.
    EXPECT_EQ(StatusCode::kNotFound,
              reg.Unsubscribe(test_dir_.string(), "ghost").code());
    // Directory not watched -> NotFound.
    EXPECT_EQ(StatusCode::kNotFound,
              reg.Unsubscribe(std::filesystem::temp_directory_path().string(),
                              "team_a").code());
}

TEST_F(DirWatcherRegistryTest, RemoveWatch_All) {
    EXPECT_CALL(spc_, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    DirWatcherRegistry reg(pool(), router(), spc_);
    ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"team_a", "team_b"}, true, false).ok());

    Status s = reg.RemoveWatch(test_dir_.string());
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(0, DirWatcherRegistryPrivateTest::SlotCount(reg));

    EXPECT_EQ(StatusCode::kNotFound, reg.RemoveWatch(test_dir_.string()).code());
}

// ---- Fan-out ----

TEST_F(DirWatcherRegistryTest, FanOut_EventsToAllNS) {
    // Every NS importer must process every (non-filtered) event => one
    // SPC Submit per (file, NS). 3 NS x 2 new files = 6 submits, and the NS
    // labels must include all three subscribers.
    std::map<std::string, int> submits_per_ns;
    EXPECT_CALL(spc_, Submit(_))
        .WillRepeatedly(Invoke([&](std::shared_ptr<SPCTask> t) -> Status {
            submits_per_ns[t->namespace_name]++;
            return Status::Ok();
        }));
    // Each subscriber's importer must acquire its namespace to process a file.
    AdmitNs("team_a");
    AdmitNs("team_b");
    AdmitNs("archive");
    DirWatcherRegistry reg(pool(), router(), spc_);
    ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"team_a", "team_b", "archive"},
                              true, /*autostart=*/false).ok());

    std::vector<FileEvent> events;
    for (const char* f : {"a.txt", "b.md"}) {
        FileEvent ev;
        ev.path = (test_dir_ / f).string();
        ev.type = FileEventType::kCreated;
        ev.is_directory = false;
        events.push_back(ev);
    }
    DirWatcherRegistryPrivateTest::FanOut(reg, canonical_dir_, events);

    EXPECT_EQ(2, submits_per_ns["team_a"]);
    EXPECT_EQ(2, submits_per_ns["team_b"]);
    EXPECT_EQ(2, submits_per_ns["archive"]);
}

TEST_F(DirWatcherRegistryTest, FanOut_FiltersIgnoredFiles) {
    int submits = 0;
    EXPECT_CALL(spc_, Submit(_))
        .WillRepeatedly(Invoke([&](std::shared_ptr<SPCTask>) -> Status {
            submits++;
            return Status::Ok();
        }));
    DirWatcherRegistry reg(pool(), router(), spc_);
    ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"team_a"}, true, false).ok());

    // A directory event and ignored patterns must be dropped at fan-out.
    std::vector<FileEvent> events;
    FileEvent dir_ev;
    dir_ev.path = (test_dir_ / "sub").string();
    dir_ev.type = FileEventType::kCreated;
    dir_ev.is_directory = true;
    events.push_back(dir_ev);

    for (const char* f : {".hidden", "scratch.tmp", "x.swp"}) {
        FileEvent ev;
        ev.path = (test_dir_ / f).string();
        ev.type = FileEventType::kCreated;
        ev.is_directory = false;
        events.push_back(ev);
    }
    DirWatcherRegistryPrivateTest::FanOut(reg, canonical_dir_, events);
    EXPECT_EQ(0, submits);  // all events filtered out
}

TEST_F(DirWatcherRegistryTest, FanOut_OneImporterFailDoesNotBlockOthers) {
    // team_fail's SPC submit fails; team_ok's must still succeed (per-NS
    // isolation, watcher part. success semantics).
    std::map<std::string, int> ok_submits;
    EXPECT_CALL(spc_, Submit(_))
        .WillRepeatedly(Invoke([&](std::shared_ptr<SPCTask> t) -> Status {
            if (t->namespace_name == "team_fail") {
                return Status::Internal("simulated SPC failure");
            }
            ok_submits[t->namespace_name]++;
            return Status::Ok();
        }));
    // Both NS are admitted so each importer reaches the SPC submit; the isolation
    // under test is an SPC-submit failure (team_fail), not an acquire failure.
    AdmitNs("team_fail");
    AdmitNs("team_ok");
    DirWatcherRegistry reg(pool(), router(), spc_);
    ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"team_fail", "team_ok"},
                              true, false).ok());

    std::vector<FileEvent> events;
    FileEvent ev;
    ev.path = (test_dir_ / "a.txt").string();
    ev.type = FileEventType::kCreated;
    ev.is_directory = false;
    events.push_back(ev);
    DirWatcherRegistryPrivateTest::FanOut(reg, canonical_dir_, events);

    EXPECT_EQ(1, ok_submits["team_ok"]);  // healthy NS unaffected
}

// ---- List / GetWatch / TriggerScan ----

TEST_F(DirWatcherRegistryTest, ListWatches_MultiDir) {
    EXPECT_CALL(spc_, Submit(_)).WillRepeatedly(Return(Status::Ok()));

    namespace fs = std::filesystem;
    auto dir2 = fs::temp_directory_path() /
                ("cortrix_dirwatch_dir2_" + std::to_string(::getpid()));
    fs::create_directories(dir2);
    { std::ofstream(dir2 / "c.txt") << "gamma"; }
    std::string canon2 = fs::canonical(dir2).string();

    DirWatcherRegistry reg(pool(), router(), spc_);
    ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"a1", "a2"}, true, false).ok());
    ASSERT_TRUE(reg.Subscribe(dir2.string(), {"b1", "b2"}, true, false).ok());

    auto watches = reg.ListWatches();
    EXPECT_EQ(2u, watches.size());

    // GetWatch by id resolves to the right directory.
    std::string id1 = DirWatcherRegistry::MakeId(canonical_dir_);
    auto got = reg.GetWatch(id1);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(canonical_dir_, got->directory);
    EXPECT_EQ(2, got->subscriber_count);
    EXPECT_FALSE(reg.GetWatch("deadbeef").has_value());

    fs::remove_all(dir2);
    (void)canon2;
}

TEST_F(DirWatcherRegistryTest, TriggerScan_FanOut) {
    // TriggerScan re-scans the directory for every subscriber => each NS imports
    // both files (2 files x 2 NS = 4 submits).
    std::map<std::string, int> submits_per_ns;
    EXPECT_CALL(spc_, Submit(_))
        .WillRepeatedly(Invoke([&](std::shared_ptr<SPCTask> t) -> Status {
            submits_per_ns[t->namespace_name]++;
            return Status::Ok();
        }));
    AdmitNs("team_a");
    AdmitNs("team_b");
    DirWatcherRegistry reg(pool(), router(), spc_);
    ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"team_a", "team_b"},
                              true, /*autostart=*/false).ok());

    ASSERT_TRUE(reg.TriggerScan(test_dir_.string()).ok());
    EXPECT_EQ(2, submits_per_ns["team_a"]);
    EXPECT_EQ(2, submits_per_ns["team_b"]);

    EXPECT_EQ(StatusCode::kNotFound,
              reg.TriggerScan("/no/such/dir").code());
}

// ---- Persistence (SaveState / LoadState v2 + v1 migration) ----

TEST_F(DirWatcherRegistryTest, SaveLoad_RoundTrip_V2) {
    EXPECT_CALL(spc_, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    namespace fs = std::filesystem;
    auto dir2 = fs::temp_directory_path() /
                ("cortrix_dirwatch_rt2_" + std::to_string(::getpid()));
    fs::create_directories(dir2);
    { std::ofstream(dir2 / "c.txt") << "gamma"; }

    auto state_dir = test_dir_ / "state";
    fs::create_directories(state_dir);

    {
        DirWatcherRegistry reg(pool(), router(), spc_);
        ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"team_a", "team_b", "archive"},
                                  true, false).ok());
        ASSERT_TRUE(reg.Subscribe(dir2.string(), {"team_x", "team_y"}, true, false).ok());
        ASSERT_TRUE(reg.SaveState(state_dir.string()).ok());
    }

    // Persisted file is v2.
    nlohmann::json j;
    { std::ifstream in(state_dir / "watchers.json"); in >> j; }
    EXPECT_EQ(2, j.value("version", -1));
    ASSERT_EQ(2u, j["watchers"].size());

    // Restore into a fresh registry.
    DirWatcherRegistry reg2(pool(), router(), spc_);
    ASSERT_TRUE(reg2.LoadState(state_dir.string()).ok());
    auto watches = reg2.ListWatches();
    EXPECT_EQ(2u, watches.size());
    int total_subs = 0;
    for (const auto& w : watches) total_subs += w.subscriber_count;
    EXPECT_EQ(5, total_subs);  // 3 + 2
    // Restored with autostart=false: no OS watcher yet.
    std::string canon_dir = fs::canonical(test_dir_).string();
    EXPECT_FALSE(DirWatcherRegistryPrivateTest::HasOsWatcher(reg2, canon_dir));

    fs::remove_all(dir2);
}

TEST_F(DirWatcherRegistryTest, LoadState_MigratesV1ToV2) {
    EXPECT_CALL(spc_, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    namespace fs = std::filesystem;
    auto state_dir = test_dir_ / "state_v1";
    fs::create_directories(state_dir);

    // Hand-write a v1 watchers.json: same data_dir, two namespaces (two rows)
    // must merge into ONE watcher with target_namespaces = [team_a, team_b].
    nlohmann::json v1;
    v1["version"] = 1;
    v1["watchers"] = nlohmann::json::array();
    for (const char* ns : {"team_a", "team_b"}) {
        nlohmann::json w;
        w["data_dir"] = test_dir_.string();
        w["namespace_name"] = ns;
        v1["watchers"].push_back(w);
    }
    { std::ofstream(state_dir / "watchers.json") << v1.dump(2); }

    DirWatcherRegistry reg(pool(), router(), spc_);
    ASSERT_TRUE(reg.LoadState(state_dir.string()).ok());

    auto watches = reg.ListWatches();
    ASSERT_EQ(1u, watches.size()) << "v1 rows for same dir must merge into one watcher";
    EXPECT_EQ(2, watches[0].subscriber_count);
    EXPECT_EQ(canonical_dir_, watches[0].directory);

    // Re-saving emits v2.
    auto out_dir = test_dir_ / "state_v1_out";
    fs::create_directories(out_dir);
    ASSERT_TRUE(reg.SaveState(out_dir.string()).ok());
    nlohmann::json j;
    { std::ifstream in(out_dir / "watchers.json"); in >> j; }
    EXPECT_EQ(2, j.value("version", -1));
}

TEST_F(DirWatcherRegistryTest, LoadState_MissingFileIsOk) {
    DirWatcherRegistry reg(pool(), router(), spc_);
    auto empty = test_dir_ / "no_state_here";
    std::filesystem::create_directories(empty);
    Status s = reg.LoadState(empty.string());
    EXPECT_TRUE(s.ok());  // first run: nothing to restore
    EXPECT_TRUE(reg.ListWatches().empty());
}

TEST_F(DirWatcherRegistryTest, LoadState_CorruptFileIsNonFatal) {
    namespace fs = std::filesystem;
    auto state_dir = test_dir_ / "state_corrupt";
    fs::create_directories(state_dir);
    { std::ofstream(state_dir / "watchers.json") << "{ this is not valid json "; }

    DirWatcherRegistry reg(pool(), router(), spc_);
    Status s = reg.LoadState(state_dir.string());
    EXPECT_TRUE(s.ok());  // WARN + skip, must not block startup
    EXPECT_TRUE(reg.ListWatches().empty());
}

// ---- id derivation ----

TEST_F(DirWatcherRegistryTest, MakeId_DeterministicAnd8Hex) {
    std::string id = DirWatcherRegistry::MakeId("/data/shared/engineering");
    EXPECT_EQ(8u, id.size());
    for (char c : id) EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c)));
    // Deterministic + path-sensitive.
    EXPECT_EQ(id, DirWatcherRegistry::MakeId("/data/shared/engineering"));
    EXPECT_NE(id, DirWatcherRegistry::MakeId("/data/shared/other"));
}

// ---- S2 route-support API: DirectoryForId / GetStats / AggregateStats ----

TEST_F(DirWatcherRegistryTest, DirectoryForId_ResolvesAndRejects) {
    EXPECT_CALL(spc_, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    DirWatcherRegistry reg(pool(), router(), spc_);
    ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"team_a"}, true, false).ok());

    std::string id = DirWatcherRegistry::MakeId(canonical_dir_);
    auto dir = reg.DirectoryForId(id);
    ASSERT_TRUE(dir.has_value());
    EXPECT_EQ(canonical_dir_, *dir);
    EXPECT_FALSE(reg.DirectoryForId("deadbeef").has_value());
}

TEST_F(DirWatcherRegistryTest, GetStats_PerNamespaceInSubscriberOrder) {
    // Two NS subscribed -> GetStats returns one entry per NS, namespace-keyed.
    EXPECT_CALL(spc_, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    AdmitNs("team_a");
    AdmitNs("team_b");
    DirWatcherRegistry reg(pool(), router(), spc_);
    ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"team_a", "team_b"}, true,
                              /*autostart=*/true).ok());
    reg.StopAll();

    auto stats = reg.GetStats(test_dir_.string());
    ASSERT_EQ(2u, stats.size());
    EXPECT_EQ("team_a", stats[0].first);
    EXPECT_EQ("team_b", stats[1].first);
    // The initial scan saw the two files created in SetUp (a.txt, b.md).
    EXPECT_GE(stats[0].second.total_files, 2);

    // Unknown directory -> empty.
    EXPECT_TRUE(reg.GetStats("/no/such/dir").empty());
}

TEST_F(DirWatcherRegistryTest, AggregateStats_SumsAllImporters) {
    EXPECT_CALL(spc_, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    AdmitNs("team_a");
    AdmitNs("team_b");
    DirWatcherRegistry reg(pool(), router(), spc_);
    ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"team_a", "team_b"}, true,
                              /*autostart=*/true).ok());
    reg.StopAll();

    auto per_ns = reg.GetStats(test_dir_.string());
    ASSERT_EQ(2u, per_ns.size());
    int64_t expected = per_ns[0].second.total_files + per_ns[1].second.total_files;
    ImportStats agg = reg.AggregateStats();
    EXPECT_EQ(expected, agg.total_files);
}

TEST_F(DirWatcherRegistryTest, RemoveWatch_DeleteDocsCountsAndReports) {
    // delete_docs=true must purge each importer's docs and report the count via
    // the out-param. With the FakeIndex harness the count is >= 0 (no crash); the
    // contract under test is the API wiring, not the storage layer.
    EXPECT_CALL(spc_, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    DirWatcherRegistry reg(pool(), router(), spc_);
    ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"team_a", "team_b"}, true, false).ok());

    int deleted = -123;  // sentinel; RemoveWatch must overwrite it.
    Status s = reg.RemoveWatch(test_dir_.string(), /*delete_docs=*/true, &deleted);
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_GE(deleted, 0);
    EXPECT_EQ(0, DirWatcherRegistryPrivateTest::SlotCount(reg));

    // delete_docs=false default keeps the old behavior (out-param stays 0).
    ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"team_a"}, true, false).ok());
    int deleted2 = 7;
    ASSERT_TRUE(reg.RemoveWatch(test_dir_.string(), /*delete_docs=*/false, &deleted2).ok());
    EXPECT_EQ(0, deleted2);
}

// ---- rev-deploy M-4: FanOut concurrency safety (UAF fix) ----

// A concurrent Unsubscribe (mutating slot.importers under mu_) must not race the
// fan-out path that snapshots the importers under mu_ before dispatching. With
// the unique_ptr->shared_ptr + snapshot fix this runs clean under TSAN/ASAN; the
// assertion here is "no crash / no hang", and the registry stays consistent.
TEST_F(DirWatcherRegistryTest, FanOut_ConcurrentUnsubscribeNoUAF) {
    EXPECT_CALL(spc_, Submit(_)).WillRepeatedly(Return(Status::Ok()));
    for (const char* ns : {"a", "b", "c", "d"}) AdmitNs(ns);
    DirWatcherRegistry reg(pool(), router(), spc_);
    ASSERT_TRUE(reg.Subscribe(test_dir_.string(), {"a", "b", "c", "d"},
                              true, /*autostart=*/false).ok());

    std::vector<FileEvent> events;
    {
        FileEvent ev;
        ev.path = (test_dir_ / "a.txt").string();
        ev.type = FileEventType::kCreated;
        ev.is_directory = false;
        events.push_back(ev);
    }

    std::atomic<bool> stop{false};
    // Hammer fan-out from a background thread while the main thread unsubscribes
    // namespaces one by one (each erases an importer from the live vector).
    std::thread fanner([&]() {
        while (!stop.load()) {
            DirWatcherRegistryPrivateTest::FanOut(reg, canonical_dir_, events);
        }
    });
    for (const char* ns : {"a", "b", "c"}) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        reg.Unsubscribe(test_dir_.string(), ns);
    }
    stop.store(true);
    fanner.join();

    // One subscriber ("d") survives; the registry is still consistent.
    auto watches = reg.ListWatches();
    ASSERT_EQ(1u, watches.size());
    EXPECT_EQ(1, watches[0].subscriber_count);
    EXPECT_EQ("d", watches[0].target_namespaces.front());
}

}  // namespace
}  // namespace cortrix
