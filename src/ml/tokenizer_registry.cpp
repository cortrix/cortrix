#include "cortrix/ml/tokenizer_registry.h"

#include <mutex>
#include <unordered_map>

namespace cortrix::ml {

namespace {
std::mutex g_mu;
// Guarded by g_mu for writes; entries, once inserted, are never mutated, so
// borrowers read the shared_ptr lock-free after registration (F02 §2.4-bis).
std::unordered_map<std::string, std::shared_ptr<HfTokenizer>>& Registry() {
    static std::unordered_map<std::string, std::shared_ptr<HfTokenizer>> reg;
    return reg;
}
}  // namespace

Status TokenizerRegistry::LoadAndRegister(const std::string& key,
                                          const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& reg = Registry();
    auto it = reg.find(key);
    if (it != reg.end() && it->second && it->second->loaded()) {
        return Status::Ok();  // already registered (idempotent)
    }
    auto tok = std::make_shared<HfTokenizer>();
    Status s = tok->Load(path);
    if (!s.ok()) {
        return s;
    }
    reg[key] = std::move(tok);
    return Status::Ok();
}

std::shared_ptr<HfTokenizer> TokenizerRegistry::Get(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& reg = Registry();
    auto it = reg.find(key);
    return it != reg.end() ? it->second : nullptr;
}

void TokenizerRegistry::ResetForTest() {
    std::lock_guard<std::mutex> lock(g_mu);
    Registry().clear();
}

}  // namespace cortrix::ml
