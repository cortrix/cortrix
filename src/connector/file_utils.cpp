#include <cstdint>
#include "cortrix/connector/file_utils.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <unordered_map>
#include <openssl/evp.h>

namespace cortrix {

static const std::unordered_map<std::string, std::string> kMimeMap = {
    {".pdf",  "application/pdf"},
    {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".doc",  "application/msword"},
    {".txt",  "text/plain"},
    {".md",   "text/markdown"},
    {".csv",  "text/csv"},
    {".json", "application/json"},
    {".xml",  "text/xml"},
    {".html", "text/html"},
    {".htm",  "text/html"},
    {".png",  "image/png"},
    {".jpg",  "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif",  "image/gif"},
    {".bmp",  "image/bmp"},
    {".tiff", "image/tiff"},
    {".wav",  "audio/wav"},
    {".mp3",  "audio/mpeg"},
    {".mp4",  "video/mp4"},
    {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
};

int ComputeFileHash(const std::string& file_path, std::string& hash) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return -1;
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return -1;

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return -1;
    }

    char buf[8192];
    while (file.read(buf, sizeof(buf))) {
        EVP_DigestUpdate(ctx, buf, file.gcount());
    }
    if (file.gcount() > 0) {
        EVP_DigestUpdate(ctx, buf, file.gcount());
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);

    static const char hex_chars[] = "0123456789abcdef";
    hash.clear();
    hash.reserve(digest_len * 2);
    for (unsigned int i = 0; i < digest_len; ++i) {
        hash.push_back(hex_chars[(digest[i] >> 4) & 0x0F]);
        hash.push_back(hex_chars[digest[i] & 0x0F]);
    }

    return 0;
}

std::string DetectMimeType(const std::string& filename) {
    namespace fs = std::filesystem;
    std::string ext = fs::path(filename).extension().string();
    // Lowercase for case-insensitive matching
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    auto it = kMimeMap.find(ext);
    if (it != kMimeMap.end()) {
        return it->second;
    }
    return "application/octet-stream";
}

int64_t GetFileSize(const std::string& file_path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto size = fs::file_size(file_path, ec);
    if (ec) return -1;
    return static_cast<int64_t>(size);
}

std::string GetBasename(const std::string& path) {
    namespace fs = std::filesystem;
    return fs::path(path).filename().string();
}

std::string GetExtension(const std::string& filename) {
    namespace fs = std::filesystem;
    std::string ext = fs::path(filename).extension().string();
    // Remove leading dot
    if (!ext.empty() && ext[0] == '.') {
        ext = ext.substr(1);
    }
    // Lowercase
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

std::string ResolvePath(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto resolved = fs::canonical(path, ec);
    if (ec) return "";
    return resolved.string();
}

bool IsDirectory(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    return fs::is_directory(path, ec);
}

bool IsRegularFile(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

}  // namespace cortrix
