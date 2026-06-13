#pragma once
#include <string>
#include <vector>
#include "cortrix/common/status.h"

namespace cortrix {

struct OcrLine {
    std::string text;
    float confidence = 0.0f;
    float box[4][2];  // [[x1,y1], [x2,y2], [x3,y3], [x4,y4]]
    int page = -1;    // 0-based page number (-1 = unknown, for backward compat)
};

struct OcrResult {
    std::vector<OcrLine> lines;
    std::string merged_text;
    int total_lines = 0;
    int filtered_lines = 0;
    float avg_confidence = 0.0f;
};

/// PaddleOCR subprocess wrapper
class OcrProcessor {
public:
    OcrProcessor(const std::string& python_bin,
                 const std::string& script_path,
                 int timeout_s = 60,
                 float confidence_threshold = 0.3f,
                 int max_retries = 1);

    /// Run OCR on an image file
    Status Process(const std::string& image_path, OcrResult* result);

    /// Check if PaddleOCR is available
    Status CheckAvailability() const;

private:
    void SortByReadingOrder(std::vector<OcrLine>* lines);
    std::string MergeText(const std::vector<OcrLine>& lines);

    Status ProcessOnce(const std::string& image_path, OcrResult* result);

    std::string python_bin_;
    std::string script_path_;
    int timeout_s_;
    float confidence_threshold_;
    int max_retries_;
};

}  // namespace cortrix
