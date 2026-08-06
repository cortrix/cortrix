#include "cortrix/chunker/chunker_config.h"

namespace cortrix::chunker {

const char* ToString(ChunkStrategy s) {
    switch (s) {
        case ChunkStrategy::kParentChild: return "parent-child";
        case ChunkStrategy::kFlat:        return "flat";
        case ChunkStrategy::kSemantic:    return "semantic";
    }
    return "parent-child";
}

const char* ToString(UnitLevel u) {
    switch (u) {
        case UnitLevel::kPage:      return "page";
        case UnitLevel::kParagraph: return "paragraph";
        case UnitLevel::kSentence:  return "sentence";
    }
    return "paragraph";
}

ChunkStrategy ChunkStrategyFromString(const std::string& s) {
    if (s == "flat") return ChunkStrategy::kFlat;
    if (s == "semantic") return ChunkStrategy::kSemantic;
    return ChunkStrategy::kParentChild;  // default
}

UnitLevel UnitLevelFromString(const std::string& s) {
    if (s == "page") return UnitLevel::kPage;
    if (s == "sentence") return UnitLevel::kSentence;
    return UnitLevel::kParagraph;  // default (lock)
}

}  // namespace cortrix::chunker
