#include "cortrix/resource/namespace_facade.h"

#include "cortrix/store/iindex.h"             // IIndex (operator* on the unique_ptr)
#include "cortrix/store/write_coordinator.h"  // WriteCoordinator

namespace cortrix::resource {

NamespaceFacade::NamespaceFacade(INamespacePool& pool, std::string namespace_id)
    : pool_(pool), namespace_id_(std::move(namespace_id)) {}

NamespaceFacade::~NamespaceFacade() {
    // Tear down self-held windows before releasing the bundle. Order matters:
    // memory_ borrows store_ (CortrixStore&) — so memory_ first, then store_,
    // then blob_. store_ owns its private connection (D-I1.bis) and closes it;
    // memory_ closes its own db.
    memory_.reset();
    store_.reset();
    blob_.reset();
    if (acquired_) {
        pool_.Release(namespace_id_);  // Phase 1: no-op placeholder, pairs Acquire
    }
}

Status NamespaceFacade::Acquire() {
    auto res = pool_.Acquire(namespace_id_);
    if (!res.ok()) {
        return res.status();  // CX_ERR_NS_* token surfaced by the pool
    }
    bundle_ = res.value();
    acquired_ = true;

    UnitResourceBundle& unit = bundle_->primary();

    // store(): per-facade PRIVATE store.db connection (D-I1.bis). Each facade
    // (HTTP request / SPC task / async worker) opens its own WAL connection —
    // thread isolation lives in the object lifetime, no cross-instance sharing
    // (the former shared bundle-conn view segfaulted under concurrency). WAL
    // gives N readers + 1 writer; writer contention queues on busy_timeout.
    // {false, false}: schema was migrated and doc recovery already ran once at
    // pool load time — neither one-shot startup job may run per-request.
    store_ = std::make_unique<cortrix::CortrixStoreSqlite>(
        unit.data_dir + "/store.db",
        cortrix::CortrixStoreSqlite::OpenOptions{false, false});
    if (store_->Open() != 0) {
        return Status::Internal("namespace_facade: store Open() failed (ns=" +
                                namespace_id_ + ")");
    }

    // blob(): self-held under <unit_dir>/blob (D-I4 light resource).
    blob_ = std::make_unique<cortrix::CortrixBlobLocal>(unit.data_dir + "/blob");

    // memory(): self-held independent db at <unit_dir>/memory.db (D-I4). Its ctor
    // borrows the CortrixStore& above; its tables live in that separate sqlite file.
    memory_ = std::make_unique<cortrix::MemoryStore>(*store_);
    Status mem = memory_->Init(unit.data_dir + "/memory.db");
    if (!mem.ok()) {
        return Status::Internal("namespace_facade: memory Init() failed (ns=" +
                                namespace_id_ + "): " + mem.message());
    }

    return Status::Ok();
}

cortrix::store::IIndex& NamespaceFacade::vec_index() {
    return *bundle_->primary().index;
}
cortrix::store::WriteCoordinator& NamespaceFacade::write_coordinator() {
    return *bundle_->primary().pwl;
}
cortrix::CortrixStore& NamespaceFacade::store() { return *store_; }
cortrix::CortrixBlobStore& NamespaceFacade::blob() { return *blob_; }
cortrix::MemoryStore& NamespaceFacade::memory() { return *memory_; }

}  // namespace cortrix::resource
