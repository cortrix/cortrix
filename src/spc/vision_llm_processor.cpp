#include "cortrix/spc/vision_llm_processor.h"
#include "cortrix/spc/subprocess_utils.h"
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <cstdio>
#include <sstream>

using json = nlohmann::json;

namespace cortrix {

VisionLlmProcessor::VisionLlmProcessor(const std::string& python_bin,
                                       const std::string& script_path,
                                       const LlmConfig& llm_config,
                                       int timeout_s)
    : python_bin_(python_bin), script_path_(script_path),
      llm_config_(llm_config), timeout_s_(timeout_s) {
    enabled_ = !script_path_.empty() &&
               !llm_config_.provider.empty() &&
               !llm_config_.api_key.empty();
}

bool VisionLlmProcessor::IsEnabled() const {
    return enabled_;
}

Status VisionLlmProcessor::Process(const std::string& file_path,
                                   const std::string& ocr_text,
                                   OcrResult* result) {
    if (!result) return Status::InvalidArgument("null result pointer");
    if (!enabled_) return Status::Unavailable("VisionLLM not configured");

    result->lines.clear();
    result->merged_text.clear();
    result->total_lines = 0;
    result->filtered_lines = 0;
    result->avg_confidence = 0.0f;

    // Write OCR text to a temp file for the Python script
    std::string ocr_tmp_path;
    if (!ocr_text.empty()) {
        char tmp_template[] = "/tmp/cortrix_vlm_ocr_XXXXXX";
        int fd = mkstemp(tmp_template);
        if (fd >= 0) {
            ocr_tmp_path = tmp_template;
            // Write and close
            FILE* fp = fdopen(fd, "w");
            if (fp) {
                fwrite(ocr_text.data(), 1, ocr_text.size(), fp);
                fclose(fp);
            } else {
                close(fd);
            }
        }
    }

    // Build argv: python3 run_vision_llm.py <pdf_path> [--ocr-text <tmp_path>]
    std::vector<std::string> args = {python_bin_, script_path_, file_path};
    if (!ocr_tmp_path.empty()) {
        args.push_back("--ocr-text");
        args.push_back(ocr_tmp_path);
    }

    // Set LLM config via environment variables
    // (RunSubprocess inherits parent env; we set them before fork)
    setenv("CORTRIX_LLM_PROVIDER", llm_config_.provider.c_str(), 1);
    setenv("CORTRIX_LLM_MODEL", llm_config_.model.c_str(), 1);
    setenv("CORTRIX_LLM_API_KEY", llm_config_.api_key.c_str(), 1);
    if (!llm_config_.base_url.empty()) {
        setenv("CORTRIX_LLM_BASE_URL", llm_config_.base_url.c_str(), 1);
    }

    std::string output;
    Status s = RunSubprocess(args, timeout_s_, &output);

    // Cleanup temp file
    if (!ocr_tmp_path.empty()) {
        std::remove(ocr_tmp_path.c_str());
    }

    if (!s.ok()) {
        return Status::Internal("VisionLLM failed: " + s.message());
    }

    // Parse JSON output (same format as run_ocr.py)
    try {
        json j = json::parse(output);

        if (j.is_object() && j.contains("error")) {
            return Status::Internal("VisionLLM error: " + j["error"].get<std::string>());
        }

        if (!j.is_array()) {
            return Status::Internal("VisionLLM output is not a JSON array");
        }

        // Build merged_text with \f page separators
        std::ostringstream oss;
        int prev_page = -2;

        for (const auto& line_json : j) {
            OcrLine line;
            line.text = line_json.value("text", "");
            line.confidence = line_json.value("confidence", 1.0f);
            line.page = line_json.value("page", -1);
            // box is placeholder for VLM output
            for (int i = 0; i < 4; ++i) {
                line.box[i][0] = 0;
                line.box[i][1] = 0;
            }

            result->lines.push_back(line);
            result->total_lines++;
            result->filtered_lines++;

            // Merge text with \f between pages
            if (prev_page != -2 && line.page >= 0 && line.page != prev_page) {
                oss << "\f";
            } else if (prev_page != -2) {
                oss << "\n\n";
            }
            oss << line.text;
            prev_page = line.page;
        }

        result->merged_text = oss.str();
        result->avg_confidence = 1.0f;

    } catch (const json::exception& e) {
        return Status::Internal("Failed to parse VisionLLM JSON: " + std::string(e.what()));
    }

    return Status::Ok();
}

}  // namespace cortrix
