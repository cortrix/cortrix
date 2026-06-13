#include "cortrix/onnx/startup_validator.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <spdlog/spdlog.h>

#include "cortrix/config/config.h"
#include "cortrix/onnx/onnx_error.h"
#include "cortrix/onnx/runtime.h"

namespace cortrix::onnx {

namespace {

// ---------------------------------------------------------------------------
// Minimal protobuf wire-format reader for the ONNX ModelProto opset.
//
// We only need one number out of the model header — the ONNX opset version —
// so we scan the protobuf wire format directly instead of linking the full
// onnx protobuf library (which is not a Cortrix dependency). Relevant schema
// (onnx.proto3):
//   ModelProto.opset_import      = field 8, repeated OperatorSetIdProto (LEN)
//   OperatorSetIdProto.domain    = field 1, string (LEN)
//   OperatorSetIdProto.version   = field 2, int64  (VARINT)
// The default ONNX operator set has domain "" (empty) or "ai.onnx"; that is the
// opset a runtime's supported range applies to.
//
// Wire types: 0=VARINT, 1=I64, 2=LEN(length-delimited), 5=I32. We handle 0/2
// and skip 1/5 by fixed width so a malformed/extended field can't desync us.
// ---------------------------------------------------------------------------

class WireReader {
 public:
    WireReader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}

    bool AtEnd() const { return p_ >= end_; }
    bool Ok() const { return ok_; }

    // Read a base-128 varint. Sets failure (and returns 0) on truncation.
    uint64_t ReadVarint() {
        uint64_t value = 0;
        int shift = 0;
        while (p_ < end_) {
            uint8_t byte = *p_++;
            value |= static_cast<uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) return value;
            shift += 7;
            if (shift >= 64) break;  // overlong varint
        }
        ok_ = false;
        return 0;
    }

    // Read a field tag → (field_number, wire_type). Caller checks Ok()/AtEnd().
    void ReadTag(uint32_t* field_number, uint32_t* wire_type) {
        uint64_t tag = ReadVarint();
        *field_number = static_cast<uint32_t>(tag >> 3);
        *wire_type = static_cast<uint32_t>(tag & 0x7);
    }

    // Return a sub-reader over the next LEN-delimited chunk and advance past it.
    WireReader ReadLengthDelimited() {
        uint64_t len = ReadVarint();
        if (!ok_ || static_cast<size_t>(end_ - p_) < len) {
            ok_ = false;
            return WireReader(p_, 0);
        }
        WireReader sub(p_, static_cast<size_t>(len));
        p_ += len;
        return sub;
    }

    // Skip a field of the given wire type (for fields we don't care about).
    void SkipField(uint32_t wire_type) {
        switch (wire_type) {
            case 0:  // VARINT
                ReadVarint();
                break;
            case 1:  // I64
                Advance(8);
                break;
            case 2:  // LEN
                ReadLengthDelimited();
                break;
            case 5:  // I32
                Advance(4);
                break;
            default:
                ok_ = false;  // unknown wire type → stop, don't desync
                break;
        }
    }

    const uint8_t* cur() const { return p_; }
    const uint8_t* end() const { return end_; }

 private:
    void Advance(size_t n) {
        if (static_cast<size_t>(end_ - p_) < n) {
            ok_ = false;
            p_ = end_;
        } else {
            p_ += n;
        }
    }

    const uint8_t* p_;
    const uint8_t* end_;
    bool ok_ = true;
};

// Parse one OperatorSetIdProto sub-message → (domain, version). Returns true if
// a version was read.
bool ParseOpsetEntry(WireReader sub, std::string* domain, int64_t* version) {
    bool got_version = false;
    while (!sub.AtEnd() && sub.Ok()) {
        uint32_t field = 0, wt = 0;
        sub.ReadTag(&field, &wt);
        if (!sub.Ok()) break;
        if (field == 1 && wt == 2) {  // domain (string)
            WireReader s = sub.ReadLengthDelimited();
            if (!sub.Ok()) break;
            domain->assign(reinterpret_cast<const char*>(s.cur()),
                           static_cast<size_t>(s.end() - s.cur()));
        } else if (field == 2 && wt == 0) {  // version (int64 varint)
            *version = static_cast<int64_t>(sub.ReadVarint());
            got_version = sub.Ok();
        } else {
            sub.SkipField(wt);
        }
    }
    return got_version;
}

bool IsDefaultOnnxDomain(const std::string& domain) {
    return domain.empty() || domain == "ai.onnx";
}

}  // namespace

Result<int> StartupValidator::ReadModelOpset(const std::string& model_path) {
    std::ifstream f(model_path, std::ios::binary);
    if (!f) {
        return OnnxStatus(OnnxErrorCode::kOpsetIncompatible,
                          "cannot open model file: " + model_path);
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    if (buf.empty()) {
        return OnnxStatus(OnnxErrorCode::kOpsetIncompatible,
                          "empty model file: " + model_path);
    }

    WireReader r(buf.data(), buf.size());
    int64_t default_domain_version = -1;
    int64_t max_version = -1;

    // Top-level ModelProto: scan for field 8 (opset_import). Everything else is
    // skipped by wire type.
    while (!r.AtEnd() && r.Ok()) {
        uint32_t field = 0, wt = 0;
        r.ReadTag(&field, &wt);
        if (!r.Ok()) break;
        if (field == 8 && wt == 2) {  // opset_import (repeated message)
            WireReader sub = r.ReadLengthDelimited();
            if (!r.Ok()) break;
            std::string domain;
            int64_t version = -1;
            if (ParseOpsetEntry(sub, &domain, &version) && version >= 0) {
                if (IsDefaultOnnxDomain(domain)) default_domain_version = version;
                if (version > max_version) max_version = version;
            }
        } else {
            r.SkipField(wt);
        }
    }

    // Prefer the default-domain opset; fall back to the max opset seen (a model
    // may declare only a custom domain, but the default is what the runtime
    // range applies to — if absent, the highest declared opset is the safest
    // proxy for the compatibility check).
    int64_t chosen = default_domain_version >= 0 ? default_domain_version : max_version;
    if (chosen < 0) {
        return OnnxStatus(OnnxErrorCode::kOpsetIncompatible,
                          "no opset_import found in model: " + model_path);
    }
    return static_cast<int>(chosen);
}

StartupValidator::ValidationReport StartupValidator::ValidateVerbose(
    const ValidationConfig& cfg, const observability::TraceContext* /*ctx*/) {
    ValidationReport report;
    report.runtime_version = Runtime::GetRuntimeVersion();
    report.runtime_major = Runtime::GetRuntimeMajorVersion();
    report.supported_opset_range = Runtime::GetSupportedOpsetRange();

    // --- Check 1: runtime ABI major matches the CMake-compiled expectation ---
    const std::string compiled_major = Runtime::GetCompiledMajorVersion();
    const int compiled_major_int = std::stoi(compiled_major.empty() ? "0" : compiled_major);

    if (!Runtime::IsRuntimeLinked()) {
        // No runtime linked → nothing to validate against. This is a valid
        // build configuration (CORTRIX_USE_ONNX=OFF, stub embeddings only), so
        // we pass: there are no real ONNX models to run.
        spdlog::info("ONNX startup validation skipped: runtime not linked (stub-only build)");
        report.status = Status::Ok();
        return report;
    }

    if (report.runtime_major != compiled_major_int) {
        report.status = OnnxStatus(
            OnnxErrorCode::kRuntimeVersionMismatch,
            "loaded ONNX Runtime major " + std::to_string(report.runtime_major) +
                " != compiled-for major " + compiled_major);
        return report;
    }

    // --- Check 2: every registered model's opset is within the range ---
    const auto [min_opset, max_opset] = report.supported_opset_range;
    for (const std::string& path : cfg.registered_model_paths) {
        ModelOpsetInfo info;
        info.model_path = path;

        std::ifstream probe(path, std::ios::binary);
        if (!probe) {
            if (cfg.skip_missing_models) {
                info.checked = false;
                report.models.push_back(info);
                spdlog::info("ONNX startup validation: model not present, skipped: {}", path);
                continue;
            }
            report.status = OnnxStatus(OnnxErrorCode::kOpsetIncompatible,
                                       "registered model not found: " + path);
            report.models.push_back(info);
            return report;
        }
        probe.close();

        Result<int> opset = ReadModelOpset(path);
        if (!opset.ok()) {
            report.status = opset.status();
            report.models.push_back(info);
            return report;
        }
        info.model_opset = opset.value();
        info.checked = true;
        report.models.push_back(info);

        if (info.model_opset < min_opset || info.model_opset > max_opset) {
            report.status = OnnxStatus(
                OnnxErrorCode::kOpsetIncompatible,
                "model " + path + " opset " + std::to_string(info.model_opset) +
                    " outside runtime range [" + std::to_string(min_opset) + ", " +
                    std::to_string(max_opset) + "]");
            return report;
        }
    }

    report.status = Status::Ok();
    return report;
}

Status StartupValidator::Validate(const ValidationConfig& cfg,
                                  const observability::TraceContext* ctx) {
    return ValidateVerbose(cfg, ctx).status;
}

StartupValidator::ValidationConfig StartupValidator::CollectRegisteredOnnxModels(
    const CortrixConfig& config) {
    ValidationConfig cfg;
    // OnnxEmbedder (bge-m3) — the one ONNX consumer with a model_path in the
    // current frozen config. Empty path = stub-only deployment → register
    // nothing for it.
    if (!config.embedding.model_path.empty()) {
        cfg.registered_model_paths.push_back(config.embedding.model_path);
    }
    // [Q3/#26 · F39-rev-1/F22-2 D3.5 wiring] The F39 query-complexity DistilBERT
    // model (models/query-complexity/model.onnx, env override
    // CORTRIX_QUERY_COMPLEXITY_MODEL_DIR). Registered ONLY when the file is present,
    // so an offline / model-less deployment validates nothing for it and the F39
    // backend takes its heuristic fallback (OnnxComplexityBackend Init() leaves it
    // unavailable → QueryComplexityClassifier L2 default-Complex). When the model IS
    // present, this lets StartupValidator catch an opset/ABI mismatch up front.
    {
        std::string dir = "models/query-complexity";
        if (const char* env = std::getenv("CORTRIX_QUERY_COMPLEXITY_MODEL_DIR");
            env != nullptr && env[0] != '\0') {
            dir = env;
        }
        const std::string model = dir + "/model.onnx";
        std::error_code ec;
        if (std::filesystem::exists(model, ec) && !ec) {
            cfg.registered_model_paths.push_back(model);
        }
    }
    // --- D3.5 wiring (do NOT add here during D3 standalone) ---
    // When F02 lands its reranker model_path config field, append it here
    // (bge-reranker-v2-m3); same for the F40 Wave C sparse model. Both keep
    // ZERO code changes to F02 / OnnxEmbedder themselves (D5=A static binding) —
    // this collector is the only place that learns about them.
    return cfg;
}

}  // namespace cortrix::onnx
