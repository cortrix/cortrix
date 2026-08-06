#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cortrix/auth/auth_context.h"
#include "cortrix/common/result.h"
#include "cortrix/import/import_types.h"
#include "cortrix/observability/operation_logger.h"

namespace cortrix::import {

/// Non-sensitive view of a registered connection (list_connections —
/// never carries the DSN). Returned by ConnectionManager::ListConnections.
struct ConnectionInfo {
    ConnectionRefId ref_id;
    std::string name;
    TenantId tenant_id;
    std::string host;
    std::string db_name;
    std::string registered_by;
    int64_t registered_at_ms = 0;
    int64_t expires_at_ms = 0;
    bool revoked = false;
};

/// Encrypted credential store (AES-256 secret store). The raw DSN is
/// handed in once at register time, stored encrypted, and only ever returned to the
/// ImportManager via the ConnectionManager. cortrix/ owns this CE interface; a Vault
/// / AWS-SM backed impl is a Phase-2 swap (L1 evolution). For standalone the in-memory
/// AES impl (InMemorySecretStore) is the test double; a real KMS/Vault store → integration.
class ISecretStore {
public:
    virtual ~ISecretStore() = default;

    /// Encrypt + store `secret`, returning an opaque key id (persisted in
    /// db_connections.secret_key_id). Never logs the plaintext.
    virtual std::string Put(const std::string& secret) = 0;

    /// Decrypt the secret behind `key_id`. nullopt if the key is unknown / revoked.
    virtual std::optional<std::string> Get(const std::string& key_id) = 0;

    /// Forget the secret behind `key_id` (called on revoke).
    virtual void Erase(const std::string& key_id) = 0;
};

/// In-memory AES-256-GCM secret store. Standalone default + the unit
/// test double. Encrypts with a process-ephemeral key so the plaintext never sits
/// in the map; a persistent KMS/Vault store is the integration / Phase-2 swap. Thread-safe.
class InMemorySecretStore : public ISecretStore {
public:
    InMemorySecretStore();

    std::string Put(const std::string& secret) override;
    std::optional<std::string> Get(const std::string& key_id) override;
    void Erase(const std::string& key_id) override;

private:
    std::mutex mu_;
    unsigned char key_[32];                          // AES-256 key, process-ephemeral
    std::map<std::string, std::string> blobs_;       // key_id -> iv||tag||ciphertext (hex)
    uint64_t next_id_ = 1;
};

/// Persistent metadata access for the db_connections table. cortrix/
/// owns this CE interface; the SQLite-backed impl is SqliteConnectionStore. The
/// secret itself lives in ISecretStore, NOT here (this row holds only secret_key_id
/// + non-sensitive metadata). Mockable for standalone unit tests.
class IConnectionStore {
public:
    virtual ~IConnectionStore() = default;

    struct Record {
        ConnectionRefId ref_id;
        std::string name;
        TenantId tenant_id;
        std::string secret_key_id;
        std::string host;
        std::string db_name;
        std::string registered_by;
        int64_t registered_at_ms = 0;
        int64_t expires_at_ms = 0;
        std::optional<int64_t> revoked_at_ms;
        std::optional<std::string> revoked_by;
        std::optional<std::string> revoke_reason;
    };

    virtual Status Insert(const Record& rec) = 0;
    virtual std::optional<Record> Find(const ConnectionRefId& ref_id) = 0;
    virtual std::vector<Record> ListByTenant(const TenantId& tenant_id) = 0;
    virtual Status MarkRevoked(const ConnectionRefId& ref_id,
                               int64_t revoked_at_ms,
                               const std::string& revoked_by,
                               const std::string& reason) = 0;
};

/// In-memory IConnectionStore — standalone default + unit test double. The
/// SQLite-backed SqliteConnectionStore (real catalog.db) → integration. Thread-safe.
class InMemoryConnectionStore : public IConnectionStore {
public:
    Status Insert(const Record& rec) override;
    std::optional<Record> Find(const ConnectionRefId& ref_id) override;
    std::vector<Record> ListByTenant(const TenantId& tenant_id) override;
    Status MarkRevoked(const ConnectionRefId& ref_id, int64_t revoked_at_ms,
                       const std::string& revoked_by, const std::string& reason) override;

private:
    std::mutex mu_;
    std::map<ConnectionRefId, Record> records_;
};

struct ConnectionManagerConfig {
    int expire_default_days = 30;    // connection.expire_default_days
    int expire_max_days = 365;       // connection.expire_max_days
};

/// Pure-virtual interface for the credential-ref lifecycle, so the
/// ImportManager / QueryExecutor seam can be mocked. resolve_dsn is the only path
/// that ever materializes a plaintext DSN.
class IConnectionManager {
public:
    virtual ~IConnectionManager() = default;

    virtual Result<ConnectionRefId> Register(const std::string& name,
                                             const std::string& dsn,
                                             const TenantId& tenant_id,
                                             const std::string& registered_by,
                                             std::optional<int> expire_days = std::nullopt) = 0;

    /// Resolve a ref → plaintext DSN for the ImportManager. Enforces the tenant
    /// boundary (cross-tenant ref → CX_ERR_IMPORT_CROSS_TENANT_REF) and expiry/
    /// revocation (→ CX_ERR_IMPORT_AUTH_DENIED). The auth_ctx.tenant_id is the gate.
    virtual Result<std::string> ResolveDsn(const ConnectionRefId& ref_id,
                                           const AuthContext& auth_ctx) = 0;

    virtual std::vector<ConnectionInfo> ListConnections(const TenantId& tenant_id) = 0;

    virtual Status Revoke(const ConnectionRefId& ref_id,
                          const AuthContext& auth_ctx,
                          const std::string& reason) = 0;
};

/// connection-ref management: encrypted secret store + ref expiry +
/// revoke + the cross-tenant guard. Holds only refs + metadata; the DSN is
/// encrypted in the ISecretStore. The 3 admin endpoints (register/list/revoke,
///) bind to these methods.
class ConnectionManager : public IConnectionManager {
public:
    /// `op_logger` (optional, may be null) receives the import-rev-6 operation_log
    /// entries `db_connection_register` / `db_connection_revoke` (S6). Null = no
    /// audit (observability is strictly additive to the business path).
    ConnectionManager(std::shared_ptr<ISecretStore> secret_store,
                      std::shared_ptr<IConnectionStore> conn_store,
                      std::shared_ptr<observability::IOperationLogger> op_logger = nullptr,
                      ConnectionManagerConfig config = {});

    Result<ConnectionRefId> Register(const std::string& name,
                                     const std::string& dsn,
                                     const TenantId& tenant_id,
                                     const std::string& registered_by,
                                     std::optional<int> expire_days = std::nullopt) override;

    Result<std::string> ResolveDsn(const ConnectionRefId& ref_id,
                                   const AuthContext& auth_ctx) override;

    std::vector<ConnectionInfo> ListConnections(const TenantId& tenant_id) override;

    Status Revoke(const ConnectionRefId& ref_id,
                  const AuthContext& auth_ctx,
                  const std::string& reason) override;

private:
    std::shared_ptr<ISecretStore> secret_store_;
    std::shared_ptr<IConnectionStore> conn_store_;
    std::shared_ptr<observability::IOperationLogger> op_logger_;
    ConnectionManagerConfig config_;
};

}  // namespace cortrix::import
