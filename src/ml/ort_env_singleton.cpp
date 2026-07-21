#include "cortrix/ml/ort_env_singleton.h"

#include <mutex>

#ifdef CORTRIX_HAS_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define CORTRIX_ADDRESS_SANITIZER_ENABLED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CORTRIX_ADDRESS_SANITIZER_ENABLED 1
#endif
#endif

#if defined(__linux__) && defined(CORTRIX_ADDRESS_SANITIZER_ENABLED) && defined(__has_include)
#if __has_include(<sanitizer/lsan_interface.h>)
#include <sanitizer/lsan_interface.h>
#define CORTRIX_HAS_LSAN_INTERFACE 1
#endif
#endif

namespace cortrix::ml {

namespace {
std::once_flag g_once;
bool g_initialized = false;
#ifdef CORTRIX_HAS_ONNXRUNTIME
// Non-owning pointer to the function-local environment created by Init(). The
// local static is initialized before any consumer can construct a Session and is
// destroyed at process shutdown in reverse construction order, after those
// consumer-owned Sessions. This preserves the single-Env lifetime contract
// without intentionally leaking the runtime allocation.
Ort::Env* g_env = nullptr;
#endif

#if defined(CORTRIX_HAS_LSAN_INTERFACE)
// The pinned ONNX Runtime 1.17.1 Linux binary retains two 64-byte process
// allocations after a balanced CreateEnv/ReleaseEnv pair. Limit the LSan
// exclusion to the Env constructor so unrelated allocations and all later
// inference work remain fully checked. The owned Env itself is still destroyed
// normally at shutdown.
class ScopedOrtEnvLsanExclusion {
public:
    ScopedOrtEnvLsanExclusion() { __lsan_disable(); }
    ~ScopedOrtEnvLsanExclusion() { __lsan_enable(); }

    ScopedOrtEnvLsanExclusion(const ScopedOrtEnvLsanExclusion&) = delete;
    ScopedOrtEnvLsanExclusion& operator=(const ScopedOrtEnvLsanExclusion&) = delete;
};
#endif
}  // namespace

bool OrtEnvSingleton::Init() {
#ifdef CORTRIX_HAS_ONNXRUNTIME
    std::call_once(g_once, []() {
#if defined(CORTRIX_HAS_LSAN_INTERFACE)
        ScopedOrtEnvLsanExclusion lsan_exclusion;
#endif
        static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "cortrix_ml");
        g_env = &env;
        g_initialized = true;
    });
    return true;
#else
    // No ONNX Runtime compiled in: nothing to create. Mark "initialized" so the
    // idempotency contract holds, but EnvHandle() stays null.
    std::call_once(g_once, []() { g_initialized = true; });
    return false;
#endif
}

bool OrtEnvSingleton::Initialized() { return g_initialized; }

void* OrtEnvSingleton::EnvHandle() {
    Init();
#ifdef CORTRIX_HAS_ONNXRUNTIME
    return static_cast<void*>(g_env);
#else
    return nullptr;
#endif
}

}  // namespace cortrix::ml
