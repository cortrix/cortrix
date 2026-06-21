#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "cortrix/common/status.h"
#include "cortrix/resource/namespace_pool.h"
#include "cortrix/resource/pool_error.h"
#include "ns_pool_test_helper.h"

// Extra deterministic acquire/release/evict/idempotency edges for the F05
// DefaultNamespacePool, built on the shared NsPoolHarness (real WriteCoordinator
// over a temp dir + mocked F12 routers). The basic admission/budget/startup paths
// live in test_namespace_pool*.cpp; this adds the resident-lifecycle edges in a
// NEW unique fixture (NsPoolExtra) so there is no suite-name clash.

namespace cortrix {
namespace {

class NsPoolExtra : public ::testing::Test {
 protected:
  void SetUp() override {
    auto dir = std::filesystem::temp_directory_path() /
               ("nspool_extra_" +
                std::to_string(::testing::UnitTest::GetInstance()
                                   ->current_test_info()
                                   ->line()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    harness_ = std::make_unique<cortrix::test::NsPoolHarness>(dir);
  }

  std::unique_ptr<cortrix::test::NsPoolHarness> harness_;
};

// Acquire an unknown NS -> CX_ERR_NS_NOT_FOUND token + kNotFound code.
TEST_F(NsPoolExtra, AcquireUnknownIsNotFoundWithToken) {
  auto& pool = harness_->pool();
  Result<resource::NamespaceResourceBundle*> acq = pool.Acquire("ghost");
  ASSERT_FALSE(acq.ok());
  EXPECT_EQ(acq.status().code(), StatusCode::kNotFound);
  EXPECT_NE(acq.status().message().find("CX_ERR_NS_NOT_FOUND"), std::string::npos);
}

// Idempotent re-admit of a resident NS returns Ok and does not grow the pool.
TEST_F(NsPoolExtra, ReAdmitResidentIsIdempotentNoGrowth) {
  ASSERT_TRUE(harness_->Admit("ns-a", 100).ok());
  const size_t size_after_first = harness_->pool().GetPoolStats().pool_size_current;
  EXPECT_EQ(size_after_first, 1u);
  // Second admit of the same NS: Ok, size unchanged.
  Status s = harness_->pool().AdmitCreate("ns-a", 999);
  EXPECT_TRUE(s.ok()) << s.message();
  EXPECT_EQ(harness_->pool().GetPoolStats().pool_size_current, 1u);
}

// Acquire + Release pair, then re-acquire succeeds (refcount returns to 0).
TEST_F(NsPoolExtra, AcquireReleaseReacquire) {
  ASSERT_TRUE(harness_->Admit("ns-b", 100).ok());
  auto& pool = harness_->pool();

  Result<resource::NamespaceResourceBundle*> a1 = pool.Acquire("ns-b");
  ASSERT_TRUE(a1.ok());
  ASSERT_NE(a1.value(), nullptr);
  EXPECT_EQ(a1.value()->namespace_id, "ns-b");
  pool.Release("ns-b");

  Result<resource::NamespaceResourceBundle*> a2 = pool.Acquire("ns-b");
  ASSERT_TRUE(a2.ok());
  EXPECT_EQ(a2.value(), a1.value());  // same node-stable slot address
  pool.Release("ns-b");
}

// Release on a non-resident NS is a safe no-op (no crash, idempotent).
TEST_F(NsPoolExtra, ReleaseUnknownIsNoOp) {
  harness_->pool().Release("never-existed");  // must not crash
  harness_->pool().Release("never-existed");
  SUCCEED();
}

// EvictForDelete with no outstanding borrow drops the NS; later Acquire fails.
TEST_F(NsPoolExtra, EvictThenAcquireFails) {
  ASSERT_TRUE(harness_->Admit("ns-c", 100).ok());
  auto& pool = harness_->pool();
  EXPECT_EQ(pool.GetPoolStats().pool_size_current, 1u);

  Status ev = pool.EvictForDelete("ns-c");
  EXPECT_TRUE(ev.ok()) << ev.message();
  EXPECT_EQ(pool.GetPoolStats().pool_size_current, 0u);

  Result<resource::NamespaceResourceBundle*> acq = pool.Acquire("ns-c");
  EXPECT_FALSE(acq.ok());
  EXPECT_EQ(acq.status().code(), StatusCode::kNotFound);
}

// EvictForDelete is idempotent: a second evict of an absent NS returns Ok.
TEST_F(NsPoolExtra, EvictIsIdempotent) {
  ASSERT_TRUE(harness_->Admit("ns-d", 100).ok());
  EXPECT_TRUE(harness_->pool().EvictForDelete("ns-d").ok());
  EXPECT_TRUE(harness_->pool().EvictForDelete("ns-d").ok());  // absent -> Ok
  EXPECT_TRUE(harness_->pool().EvictForDelete("never").ok());
}

// EvictForDelete deferred while a borrow is outstanding: the last Release reaps.
TEST_F(NsPoolExtra, EvictDeferredUntilLastRelease) {
  ASSERT_TRUE(harness_->Admit("ns-e", 100).ok());
  auto& pool = harness_->pool();

  Result<resource::NamespaceResourceBundle*> borrow = pool.Acquire("ns-e");
  ASSERT_TRUE(borrow.ok());

  // Evict while borrowed: marked pending_delete, NOT yet reaped.
  EXPECT_TRUE(pool.EvictForDelete("ns-e").ok());
  // A new Acquire is refused once pending_delete is set.
  Result<resource::NamespaceResourceBundle*> mid = pool.Acquire("ns-e");
  EXPECT_FALSE(mid.ok());
  EXPECT_EQ(mid.status().code(), StatusCode::kNotFound);

  // Last Release reaps the slot.
  pool.Release("ns-e");
  EXPECT_EQ(pool.GetPoolStats().pool_size_current, 0u);
}

// Multiple distinct namespaces are independently resident.
TEST_F(NsPoolExtra, MultipleNamespacesIndependent) {
  ASSERT_TRUE(harness_->Admit("ns-1", 100).ok());
  ASSERT_TRUE(harness_->Admit("ns-2", 100).ok());
  ASSERT_TRUE(harness_->Admit("ns-3", 100).ok());
  auto& pool = harness_->pool();
  EXPECT_EQ(pool.GetPoolStats().pool_size_current, 3u);

  EXPECT_TRUE(pool.Acquire("ns-1").ok());
  pool.Release("ns-1");
  EXPECT_TRUE(pool.EvictForDelete("ns-2").ok());
  EXPECT_EQ(pool.GetPoolStats().pool_size_current, 2u);
  EXPECT_TRUE(pool.Acquire("ns-3").ok());
  pool.Release("ns-3");
}

}  // namespace
}  // namespace cortrix
