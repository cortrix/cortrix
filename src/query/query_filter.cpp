#include "cortrix/query/query_filter.h"

#include <algorithm>
#include <cctype>

namespace cortrix::query {

namespace {

bool IsIsoDate(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    const int month = std::stoi(s.substr(5, 2));
    const int day = std::stoi(s.substr(8, 2));
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

Status ParseDateBound(const nlohmann::json& range, const char* key,
                      std::optional<std::string>* out) {
    if (!range.contains(key) || range[key].is_null()) return Status::Ok();
    if (!range[key].is_string() || !IsIsoDate(range[key].get<std::string>())) {
        return Status::InvalidArgument(std::string("filter.date_range.") + key +
                                       " must be a date in YYYY-MM-DD format");
    }
    *out = range[key].get<std::string>();
    return Status::Ok();
}

}  // namespace

Status ParseQueryFilter(const nlohmann::json& filter, QueryFilterSpec* out) {
    *out = QueryFilterSpec{};
    if (filter.is_null()) return Status::Ok();
    if (!filter.is_object()) return Status::InvalidArgument("filter must be an object");

    for (auto it = filter.begin(); it != filter.end(); ++it) {
        const std::string& key = it.key();
        const nlohmann::json& value = it.value();
        if (key == "tags") {
            if (value.is_null()) continue;
            if (!value.is_array()) {
                return Status::InvalidArgument("filter.tags must be an array of strings");
            }
            for (const auto& tag : value) {
                if (!tag.is_string()) {
                    return Status::InvalidArgument("filter.tags must be an array of strings");
                }
                out->tags.push_back(tag.get<std::string>());
            }
        } else if (key == "date_range") {
            if (value.is_null()) continue;
            if (!value.is_object()) {
                return Status::InvalidArgument(
                    "filter.date_range must be an object with start and/or end");
            }
            Status s = ParseDateBound(value, "start", &out->date_start);
            if (!s.ok()) return s;
            s = ParseDateBound(value, "end", &out->date_end);
            if (!s.ok()) return s;
            if (out->date_start && out->date_end && *out->date_start > *out->date_end) {
                return Status::InvalidArgument(
                    "filter.date_range.start must not be after filter.date_range.end");
            }
        } else {
            out->ignored_keys.push_back(key);
        }
    }
    return Status::Ok();
}

bool DocumentMatchesFilter(const QueryFilterSpec& spec, const std::string& created_at,
                           const std::string& metadata_json) {
    if (spec.date_start || spec.date_end) {
        if (created_at.size() < 10) return false;
        const std::string day = created_at.substr(0, 10);
        if (spec.date_start && day < *spec.date_start) return false;
        if (spec.date_end && day > *spec.date_end) return false;
    }
    if (!spec.tags.empty()) {
        const nlohmann::json meta =
            nlohmann::json::parse(metadata_json, nullptr, /*allow_exceptions=*/false);
        if (!meta.is_object() || !meta.contains("tags")) return false;
        std::vector<std::string> doc_tags;
        const auto& t = meta["tags"];
        if (t.is_string()) {
            doc_tags.push_back(t.get<std::string>());
        } else if (t.is_array()) {
            for (const auto& v : t) {
                if (v.is_string()) doc_tags.push_back(v.get<std::string>());
            }
        }
        const bool any = std::any_of(spec.tags.begin(), spec.tags.end(), [&](const auto& want) {
            return std::find(doc_tags.begin(), doc_tags.end(), want) != doc_tags.end();
        });
        if (!any) return false;
    }
    return true;
}

}  // namespace cortrix::query
