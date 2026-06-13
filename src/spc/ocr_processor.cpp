#include "cortrix/spc/ocr_processor.h"
#include "cortrix/spc/subprocess_utils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <sstream>

using json = nlohmann::json;

namespace cortrix {

OcrProcessor::OcrProcessor(const std::string& python_bin,
                           const std::string& script_path,
                           int timeout_s,
                           float confidence_threshold,
                           int max_retries)
    : python_bin_(python_bin), script_path_(script_path),
      timeout_s_(timeout_s), confidence_threshold_(confidence_threshold),
      max_retries_(max_retries) {}

Status OcrProcessor::Process(const std::string& image_path, OcrResult* result) {
    if (!result) return Status::InvalidArgument("null result pointer");

    // D7 spec: retry 1 time on failure, still fails -> error
    Status last_error;
    for (int attempt = 0; attempt <= max_retries_; ++attempt) {
        last_error = ProcessOnce(image_path, result);
        if (last_error.ok()) {
            return Status::Ok();
        }
    }
    return last_error;
}

Status OcrProcessor::ProcessOnce(const std::string& image_path, OcrResult* result) {
    result->lines.clear();
    result->merged_text.clear();
    result->total_lines = 0;
    result->filtered_lines = 0;
    result->avg_confidence = 0.0f;

    // Call Python subprocess: python3 run_ocr.py <image_path>
    // Use argv-based execution to prevent shell injection via file paths
    std::vector<std::string> args = {python_bin_, script_path_, image_path};
    std::string output;
    Status s = RunSubprocess(args, timeout_s_, &output);
    if (!s.ok()) {
        return Status::Internal("OCR failed: " + s.message());
    }

    // Parse JSON output
    try {
        json j = json::parse(output);

        // Check for error in JSON
        if (j.is_object() && j.contains("error")) {
            return Status::Internal("OCR error: " + j["error"].get<std::string>());
        }

        // Expect an array of OCR lines
        if (!j.is_array()) {
            return Status::Internal("OCR output is not a JSON array");
        }

        // Parse each line
        float confidence_sum = 0.0f;
        for (const auto& line_json : j) {
            OcrLine line;
            line.text = line_json.value("text", "");
            line.confidence = line_json.value("confidence", 0.0f);
            line.page = line_json.value("page", -1);

            // Parse box coordinates
            if (line_json.contains("box")) {
                auto box = line_json["box"];
                if (box.is_array() && box.size() == 4) {
                    for (size_t i = 0; i < 4; ++i) {
                        if (box[i].is_array() && box[i].size() == 2) {
                            line.box[i][0] = box[i][0].get<float>();
                            line.box[i][1] = box[i][1].get<float>();
                        }
                    }
                }
            }

            result->total_lines++;
            confidence_sum += line.confidence;

            // Filter by confidence threshold
            if (line.confidence >= confidence_threshold_) {
                result->lines.push_back(line);
                result->filtered_lines++;
            }
        }

        // Calculate average confidence
        if (result->total_lines > 0) {
            result->avg_confidence = confidence_sum / result->total_lines;
        }

        // Sort lines by reading order (top-to-bottom, left-to-right)
        SortByReadingOrder(&result->lines);

        // Merge text
        result->merged_text = MergeText(result->lines);

    } catch (const json::exception& e) {
        return Status::Internal("Failed to parse OCR JSON output: " + std::string(e.what()));
    }

    return Status::Ok();
}

Status OcrProcessor::CheckAvailability() const {
    return CheckExecutable("paddleocr") ? Status::Ok() : Status::Unavailable("PaddleOCR not installed");
}

void OcrProcessor::SortByReadingOrder(std::vector<OcrLine>* lines) {
    if (!lines || lines->empty()) return;

    // Sort by page first, then within each page by y > x reading order
    std::sort(lines->begin(), lines->end(), [](const OcrLine& a, const OcrLine& b) {
        // Different pages: sort by page number
        if (a.page != b.page) return a.page < b.page;

        float y_a = a.box[0][1];  // top-left y
        float y_b = b.box[0][1];
        float x_a = a.box[0][0];  // top-left x
        float x_b = b.box[0][0];

        // Group by approximate y-coordinate (tolerance for same line)
        const float y_tolerance = 20.0f;
        if (std::abs(y_a - y_b) < y_tolerance) {
            return x_a < x_b;  // Same line, sort by x
        }
        return y_a < y_b;  // Different lines, sort by y
    });
}

std::string OcrProcessor::MergeText(const std::vector<OcrLine>& lines) {
    if (lines.empty()) return "";

    std::ostringstream oss;
    float prev_y = -1000.0f;
    int prev_page = -2;  // impossible initial value
    const float line_break_threshold = 20.0f;
    const float paragraph_break_threshold = 40.0f;

    for (const auto& line : lines) {
        float current_y = line.box[0][1];

        if (prev_page != -2) {
            // Insert \f page separator when page changes
            if (line.page >= 0 && line.page != prev_page) {
                oss << "\f";
                prev_y = -1000.0f;  // reset y tracking for new page
            } else {
                float y_diff = current_y - prev_y;
                if (y_diff > paragraph_break_threshold) {
                    oss << "\n\n";  // Paragraph break
                } else if (y_diff > line_break_threshold) {
                    oss << "\n";  // Line break
                } else {
                    oss << " ";  // Same line, add space
                }
            }
        }

        oss << line.text;
        prev_y = current_y;
        prev_page = line.page;
    }

    return oss.str();
}

}  // namespace cortrix
