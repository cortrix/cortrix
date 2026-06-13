#pragma once
#include <string>
#include <cstdint>

namespace cortrix {

/// Compute SHA-256 content hash (hex string) for a file
/// @param file_path: path to file
/// @param hash: [out] 64-char lowercase hex string
/// @return 0 = success, -1 = file not found, -2 = read error
int ComputeFileHash(const std::string& file_path, std::string& hash);

/// Detect MIME type from file extension
/// @param filename: file name or path
/// @return MIME type string (e.g. "application/pdf"), "application/octet-stream" if unknown
std::string DetectMimeType(const std::string& filename);

/// Get file size in bytes
/// @param file_path: path to file
/// @return file size, -1 on error
int64_t GetFileSize(const std::string& file_path);

/// Extract filename from path
std::string GetBasename(const std::string& path);

/// Extract file extension (lowercase, without dot)
std::string GetExtension(const std::string& filename);

/// Resolve path to absolute (resolve symlinks)
/// @param path: relative or absolute path
/// @return absolute path, empty string on error
std::string ResolvePath(const std::string& path);

/// Check if path is a directory
/// @param path: path to check
/// @return true if directory, false otherwise
bool IsDirectory(const std::string& path);

/// Check if path is a regular file
/// @param path: path to check
/// @return true if regular file, false otherwise
bool IsRegularFile(const std::string& path);

}  // namespace cortrix
