#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace cortrix {

/// Raw file storage vtable interface.
/// Return value convention: 0 = success, -1 = error, -2 = not found
class CortrixBlobStore {
public:
    virtual ~CortrixBlobStore() = default;

    virtual int store(const std::string& ns, const std::string& doc_id,
                      const void* data, size_t len) = 0;
    virtual int load(const std::string& ns, const std::string& doc_id,
                     std::vector<uint8_t>& out) = 0;
    virtual int del(const std::string& ns, const std::string& doc_id) = 0;
};

}  // namespace cortrix
