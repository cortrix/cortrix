#include "cortrix/ml/ort_env_singleton.h"

#include <mutex>

#ifdef CORTRIX_HAS_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace cortrix::ml {

namespace {
std::once_flag g_once;
bool g_initialized = false;
#ifdef CORTRIX_HAS_ONNXRUNTIME
// Leaked-on-purpose process singleton: the Ort::Env must outlive every Session
// in the process, so it is never destroyed (avoids static-destruction-order
// crashes when other singletons tear down). One per process (F02 §2.4-bis).
Ort::Env* g_env = nullptr;
#endif
}  // namespace

bool OrtEnvSingleton::Init() {
#ifdef CORTRIX_HAS_ONNXRUNTIME
    std::call_once(g_once, []() {
        g_env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "cortrix_ml");
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
