#pragma once
#include <gmock/gmock.h>
#include "cortrix/spc/onnx_embedder.h"

namespace cortrix {
namespace testing {

// OnnxEmbedder is not a virtual interface, so we provide a test double.
// For unit tests that need embedding, create a FakeEmbedder that returns
// deterministic vectors.
class FakeEmbedder {
public:
    explicit FakeEmbedder(int dim = 1024) : dim_(dim) {}

    /// Return a deterministic embedding (all zeros + text hash seed)
    Status Embed(const std::string& text, EmbeddingResult* result) {
        result->vector.resize(dim_, 0.0f);
        // Simple hash-based seed for deterministic but varied vectors
        float seed = 0.0f;
        for (char c : text) seed += static_cast<float>(c);
        if (dim_ > 0) result->vector[0] = seed / 1000.0f;
        result->dim = dim_;
        result->inference_time_ms = 1.0f;
        return Status::Ok();
    }

    int dimension() const { return dim_; }

private:
    int dim_;
};

}  // namespace testing
}  // namespace cortrix
