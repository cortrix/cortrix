// Coverage-gap unit tests for src/auth/. Targets the schema migration failure
// arm, platform_db open/already-open guards, AdminUsersService "not initialized"
// guards + validation/CRUD branches, AuthConfigService null-db guards + the SMTP
// admin setter/redaction path, and Pbkdf2PasswordHasher malformed-hash arms.
// Runs against a real in-memory / temp-file platform.db. Deterministic.
//
// SKIPPED (need fault injection / OpenSSL): the prepare/step/pragma SQLite ERROR
// arms inside platform_db / auth_config / admin_users (sqlite3_exec mid-op
// failures), and RAND_bytes-failed / PBKDF2-derivation-failed in the hasher
// (OpenSSL CSPRNG). Noted in the agent report.
#include <gtest/gtest.h>

#include <sqlite3.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <unistd.h>

#include "cortrix/auth/admin_users_service.h"
#include "cortrix/auth/auth_config_service.h"
#include "cortrix/auth/auth_schema.h"
#include "cortrix/auth/pbkdf2_password_hasher.h"
#include "cortrix/auth/platform_db.h"

namespace cortrix::auth {
namespace {

bool HasCxAuth(const Status& st, const std::string& cx) {
    return !st.ok() && st.message().rfind(cx, 0) == 0;  // starts_with
}

// Unique temp-file path per process + call (getpid + atomic counter) so parallel
// (-j2) runs never share / corrupt a fixed-name db file.
std::string UniqueDbPath(const char* tag) {
    static std::atomic<int> counter{0};
    return std::string(::testing::TempDir()) + "auth_cov_" + tag + "_" +
           std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1)) + ".db";
}

// ===========================================================================
// auth_schema -- the Migrate() sqlite3_exec failure arm (50% gap).
// ===========================================================================

// Pre-create a TABLE whose name collides with one of the schema's indexes
// (idx_users_email). CREATE INDEX IF NOT EXISTS then fails ("there is already a
// table named idx_users_email"), driving AuthSchemaProvider::Migrate's error
// branch (the std::string error-message build).
TEST(AuthSchemaGapTest, MigrateFailsOnNameCollision) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "CREATE TABLE idx_users_email(x INTEGER)", nullptr, nullptr,
                           nullptr),
              SQLITE_OK);

    AuthSchemaProvider provider;
    Status st = provider.Migrate(db, 0, kAuthSchemaVersion);
    EXPECT_FALSE(st.ok());
    EXPECT_NE(st.message().find("auth schema migration failed"), std::string::npos)
        << st.message();
    sqlite3_close(db);
}

// ===========================================================================
// platform_db -- already-open guard + open-failure arm.
// ===========================================================================

// A second Open on an already-open handle returns the InvalidArgument guard.
TEST(PlatformDbGapTest, SecondOpenIsInvalidArgument) {
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    Status st = pdb.Open(":memory:");
    EXPECT_FALSE(st.ok());
    EXPECT_EQ(st.code(), StatusCode::kInvalidArgument);
    EXPECT_NE(st.message().find("already open"), std::string::npos) << st.message();
}

// Opening an un-creatable path (a directory component that is actually a file)
// drives the sqlite3_open failure arm -> "platform.db open failed".
TEST(PlatformDbGapTest, OpenBadPathFails) {
    // ":memory:/sub.db" is not a valid creatable path; sqlite cannot open it.
    const std::string bad = std::string(::testing::TempDir()) + "no_such_dir_xyz/inner/db.sqlite";
    PlatformDb pdb;
    Status st = pdb.Open(bad);
    EXPECT_FALSE(st.ok());
    EXPECT_EQ(pdb.db(), nullptr);  // handle reset to null on failure
}

// Close() is safe to call twice (idempotent) and after a failed/never Open.
TEST(PlatformDbGapTest, DoubleCloseSafe) {
    PlatformDb pdb;
    pdb.Close();  // never opened
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    pdb.Close();
    pdb.Close();  // second close is a no-op
    EXPECT_EQ(pdb.db(), nullptr);
}

// ===========================================================================
// AdminUsersService -- "not initialized" guards + validation/CRUD branches.
// ===========================================================================

class AdminUsersGapTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(pdb_.Open(":memory:").ok());
        svc_ = std::make_unique<AdminUsersService>(pdb_.db(), &hasher_);
    }
    PlatformDb pdb_;
    Pbkdf2PasswordHasher hasher_{1000};  // low iters -> fast
    std::unique_ptr<AdminUsersService> svc_;
};

// --- A null-db service "not initialized"-guards Get / List / Update / Disable. ---
// (Disable/Enable exercise the private SetStatus path through the public API.)
TEST(AdminUsersGapNullDbTest, NotInitializedGuards) {
    Pbkdf2PasswordHasher hasher(1000);
    AdminUsersService svc(nullptr, &hasher);
    EXPECT_TRUE(HasCxAuth(svc.Get("usr_x").status(), "CX_ERR_INTERNAL_ERROR"));
    EXPECT_TRUE(HasCxAuth(svc.List({}).status(), "CX_ERR_INTERNAL_ERROR"));
    EXPECT_TRUE(
        HasCxAuth(svc.Update("usr_x", "n", std::nullopt, std::nullopt).status(),
                  "CX_ERR_INTERNAL_ERROR"));
    EXPECT_TRUE(HasCxAuth(svc.Disable("usr_x").status(), "CX_ERR_INTERNAL_ERROR"));
    EXPECT_TRUE(HasCxAuth(svc.Enable("usr_x").status(), "CX_ERR_INTERNAL_ERROR"));
    EXPECT_TRUE(HasCxAuth(svc.Create("a@b.com", "Secure123!", "user", "").status(),
                          "CX_ERR_INTERNAL_ERROR"));
}

// --- A null hasher also fails Create's "not initialized" guard (db ok, hasher null). ---
TEST_F(AdminUsersGapTest, CreateNullHasherNotInitialized) {
    AdminUsersService svc(pdb_.db(), /*hasher=*/nullptr);
    auto r = svc.Create("a@b.com", "Secure123!", "user", "");
    EXPECT_TRUE(HasCxAuth(r.status(), "CX_ERR_INTERNAL_ERROR"));
}

// --- Create validation: bad email + bad role + over-long display_name + weak
//     password all collapse into one CX_ERR_USER_VALIDATION with a fields_invalid list. ---
TEST_F(AdminUsersGapTest, CreateValidationCollectsInvalidFields) {
    const std::string long_dn(100, 'x');  // > 64
    auto r = svc_->Create("not-an-email", "weak", "superuser", long_dn);
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(HasCxAuth(r.status(), "CX_ERR_USER_VALIDATION")) << r.status().message();
    EXPECT_NE(r.status().message().find("email"), std::string::npos);
    EXPECT_NE(r.status().message().find("role"), std::string::npos);
    EXPECT_NE(r.status().message().find("password"), std::string::npos);
    EXPECT_NE(r.status().message().find("display_name"), std::string::npos);
}

// --- Create success + Get round-trip + duplicate-email constraint arm. ---
TEST_F(AdminUsersGapTest, CreateSuccessGetAndDuplicate) {
    auto created = svc_->Create("dev@example.com", "Secure123!", "user", "Dev");
    ASSERT_TRUE(created.ok()) << created.status().message();
    EXPECT_EQ(created->email, "dev@example.com");
    EXPECT_EQ(created->role, "user");
    EXPECT_EQ(created->status, "active");
    EXPECT_TRUE(created->email_verified);  // invite mode -> verified

    auto got = svc_->Get(created->id);
    ASSERT_TRUE(got.ok()) << got.status().message();
    EXPECT_EQ(got->email, "dev@example.com");
    EXPECT_EQ(got->display_name, "Dev");

    // A duplicate email hits the UNIQUE constraint -> USER_EMAIL_EXISTS.
    auto dup = svc_->Create("dev@example.com", "Secure123!", "user", "Dev2");
    ASSERT_FALSE(dup.ok());
    EXPECT_TRUE(HasCxAuth(dup.status(), "CX_ERR_USER_EMAIL_EXISTS")) << dup.status().message();
}

// --- Create with empty display_name defaults it to the email. ---
TEST_F(AdminUsersGapTest, CreateEmptyDisplayNameDefaultsToEmail) {
    auto created = svc_->Create("noname@example.com", "Secure123!", "admin", "");
    ASSERT_TRUE(created.ok()) << created.status().message();
    EXPECT_EQ(created->display_name, "noname@example.com");
    EXPECT_EQ(created->role, "admin");
}

// --- Get on a missing user -> USER_NOT_FOUND. ---
TEST_F(AdminUsersGapTest, GetNotFound) {
    auto r = svc_->Get("usr_ghost");
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(HasCxAuth(r.status(), "CX_ERR_USER_NOT_FOUND")) << r.status().message();
}

// --- Update on a missing user -> USER_NOT_FOUND (existence checked first). ---
TEST_F(AdminUsersGapTest, UpdateNotFound) {
    auto r = svc_->Update("usr_ghost", std::string("New"), std::nullopt, std::nullopt);
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(HasCxAuth(r.status(), "CX_ERR_USER_NOT_FOUND")) << r.status().message();
}

// --- Update validation: bad role + empty display_name -> USER_VALIDATION. ---
TEST_F(AdminUsersGapTest, UpdateValidationRejected) {
    auto created = svc_->Create("u1@example.com", "Secure123!", "user", "U1");
    ASSERT_TRUE(created.ok());
    auto r = svc_->Update(created->id, std::string(""), std::string("notarole"), std::nullopt);
    ASSERT_FALSE(r.ok());
    EXPECT_TRUE(HasCxAuth(r.status(), "CX_ERR_USER_VALIDATION")) << r.status().message();
}

// --- Update with no fields is an idempotent no-op that returns the row as-is. ---
TEST_F(AdminUsersGapTest, UpdateNoFieldsIsNoOp) {
    auto created = svc_->Create("u2@example.com", "Secure123!", "user", "U2");
    ASSERT_TRUE(created.ok());
    auto r = svc_->Update(created->id, std::nullopt, std::nullopt, std::nullopt);
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r->display_name, "U2");  // unchanged
    EXPECT_EQ(r->role, "user");
}

// --- Update success: change role + email_verified, persisted and re-read. ---
TEST_F(AdminUsersGapTest, UpdateSuccess) {
    auto created = svc_->Create("u3@example.com", "Secure123!", "user", "U3");
    ASSERT_TRUE(created.ok());
    auto r = svc_->Update(created->id, std::string("Renamed"), std::string("admin"),
                          std::optional<bool>(false));
    ASSERT_TRUE(r.ok()) << r.status().message();
    EXPECT_EQ(r->display_name, "Renamed");
    EXPECT_EQ(r->role, "admin");
    EXPECT_FALSE(r->email_verified);
}

// --- Disable/Enable flip status via SetStatus; status change on a ghost -> NOT_FOUND. ---
TEST_F(AdminUsersGapTest, DisableEnableAndSetStatusNotFound) {
    auto created = svc_->Create("u4@example.com", "Secure123!", "user", "U4");
    ASSERT_TRUE(created.ok());

    auto dis = svc_->Disable(created->id);
    ASSERT_TRUE(dis.ok()) << dis.status().message();
    EXPECT_EQ(dis->status, "disabled");

    auto en = svc_->Enable(created->id);
    ASSERT_TRUE(en.ok()) << en.status().message();
    EXPECT_EQ(en->status, "active");

    auto miss = svc_->Disable("usr_ghost");
    ASSERT_FALSE(miss.ok());
    EXPECT_TRUE(HasCxAuth(miss.status(), "CX_ERR_USER_NOT_FOUND")) << miss.status().message();
}

// --- List filters: q/status/role + pagination clamps (page<1 -> 1, limit cap). ---
TEST_F(AdminUsersGapTest, ListFiltersAndPaginationClamp) {
    ASSERT_TRUE(svc_->Create("alice@example.com", "Secure123!", "admin", "Alice").ok());
    auto bob = svc_->Create("bob@example.com", "Secure123!", "user", "Bob");
    ASSERT_TRUE(bob.ok());
    ASSERT_TRUE(svc_->Create("carol@example.com", "Secure123!", "user", "Carol").ok());
    ASSERT_TRUE(svc_->Disable(bob->id).ok());  // one disabled row for the status filter

    // role filter -> only admins.
    AdminUserListFilter role_f;
    role_f.role = "admin";
    auto admins = svc_->List(role_f);
    ASSERT_TRUE(admins.ok()) << admins.status().message();
    for (const auto& u : admins->users) EXPECT_EQ(u.role, "admin");

    // q substring filter on email/display_name.
    AdminUserListFilter q_f;
    q_f.q = "bob";
    auto qres = svc_->List(q_f);
    ASSERT_TRUE(qres.ok());
    EXPECT_GE(qres->total, 1);

    // status=disabled filter returns only disabled rows.
    AdminUserListFilter dis_f;
    dis_f.status = "disabled";
    auto disres = svc_->List(dis_f);
    ASSERT_TRUE(disres.ok());
    for (const auto& u : disres->users) EXPECT_EQ(u.status, "disabled");

    // pagination clamps: page<1 -> 1, limit>cap -> 200.
    AdminUserListFilter clamp_f;
    clamp_f.page = -5;
    clamp_f.limit = 9999;
    auto clamped = svc_->List(clamp_f);
    ASSERT_TRUE(clamped.ok());
    EXPECT_EQ(clamped->page, 1);
    EXPECT_EQ(clamped->limit, 200);
}

// ===========================================================================
// AuthConfigService -- null-db guards + SMTP setter/redaction success path.
// ===========================================================================

// --- A null-db service guards LoadOrInitDefaults / ReloadKey / SetSmtp. ---
TEST(AuthConfigGapNullDbTest, NullDbGuards) {
    AuthConfigService svc(nullptr);
    EXPECT_TRUE(HasCxAuth(svc.LoadOrInitDefaults(), "CX_ERR_INTERNAL_ERROR"));
    EXPECT_TRUE(HasCxAuth(svc.ReloadKey("smtp.host"), "CX_ERR_INTERNAL_ERROR"));
    AuthConfigService::SmtpSettings s;
    EXPECT_TRUE(HasCxAuth(svc.SetSmtp(s), "CX_ERR_INTERNAL_ERROR"));
}

// --- SetSmtp writes + hot-reloads the snapshot; GetSmtpRedacted masks the pass. ---
TEST(AuthConfigGapTest, SetSmtpThenGetRedacted) {
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    AuthConfigService svc(pdb.db());
    ASSERT_TRUE(svc.LoadOrInitDefaults().ok());

    AuthConfigService::SmtpSettings in;
    in.host = "smtp.example.com";
    in.port = 2525;
    in.user = "mailer";
    in.pass = "s3cr3t";
    in.tls = false;
    in.enable_email_verification = true;
    ASSERT_TRUE(svc.SetSmtp(in).ok());

    // Snapshot reflects the written values (hot-reload fired on each key).
    config::AuthConfig c = svc.Get();
    EXPECT_EQ(c.smtp_host, "smtp.example.com");
    EXPECT_EQ(c.smtp_port, 2525);
    EXPECT_EQ(c.smtp_user, "mailer");
    EXPECT_FALSE(c.smtp_tls);
    EXPECT_TRUE(c.email_verification);

    // The redacted view masks the password but keeps the rest.
    AuthConfigService::SmtpSettings out = svc.GetSmtpRedacted();
    EXPECT_EQ(out.host, "smtp.example.com");
    EXPECT_EQ(out.port, 2525);
    EXPECT_EQ(out.pass, "***");  // never returns the stored password
    EXPECT_TRUE(out.enable_email_verification);
}

// --- An empty stored password redacts to empty (not "***"). ---
TEST(AuthConfigGapTest, GetRedactedEmptyPassStaysEmpty) {
    PlatformDb pdb;
    ASSERT_TRUE(pdb.Open(":memory:").ok());
    AuthConfigService svc(pdb.db());
    ASSERT_TRUE(svc.LoadOrInitDefaults().ok());
    AuthConfigService::SmtpSettings out = svc.GetSmtpRedacted();
    EXPECT_TRUE(out.pass.empty());  // default pass is empty -> stays empty
}

// ===========================================================================
// Pbkdf2PasswordHasher -- the remaining malformed-hash error arms.
// ===========================================================================

// --- iterations == 0 in the stored hash -> "bad pbkdf2 iteration count" error. ---
TEST(Pbkdf2GapTest, ZeroIterationsIsError) {
    Pbkdf2PasswordHasher h(1000);
    auto r = h.Verify("pw", "pbkdf2$sha256$0$c2FsdA$ZGs");
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(HasCxAuth(r.status(), "CX_ERR_INTERNAL_ERROR")) << r.status().message();
}

// --- A non-base64url salt segment -> "bad pbkdf2 salt" decode error. ---
TEST(Pbkdf2GapTest, BadSaltDecodeIsError) {
    Pbkdf2PasswordHasher h(1000);
    // '!' is not a base64url character -> Base64UrlDecode fails on the salt.
    auto r = h.Verify("pw", "pbkdf2$sha256$1000$!!!!$ZGs");
    EXPECT_FALSE(r.ok());
}

// --- A valid salt but a non-base64url dk segment -> "bad pbkdf2 dk" decode error. ---
TEST(Pbkdf2GapTest, BadDkDecodeIsError) {
    Pbkdf2PasswordHasher h(1000);
    auto r = h.Verify("pw", "pbkdf2$sha256$1000$c2FsdA$!!!!");
    EXPECT_FALSE(r.ok());
}

// --- cost() reports the bcrypt-equivalent factor 12 regardless of iteration config. ---
TEST(Pbkdf2GapTest, CostIsBcryptEquivalent) {
    Pbkdf2PasswordHasher a(1000);
    Pbkdf2PasswordHasher b(99);
    EXPECT_EQ(a.cost(), 12);
    EXPECT_EQ(b.cost(), 12);
}

}  // namespace
}  // namespace cortrix::auth
