#include "cortrix/spc_enricher/prompt_template.h"

namespace cortrix::spc {

namespace {
constexpr char kDefaultZh[] = "default-zh";
constexpr char kDefaultEn[] = "default-en";

// The combined NER + Summary instruction (topic 3.2 combined prompt). The model must
// return ONE JSON object whose keys are the chunk indices (as strings), each
// value an object {entities:[{text,type,start_offset,end_offset}], summary,
// score}. Indices match the `===CHUNK i===` markers Render() appends. This
// per-index keying is what topic 3.3 L1/L2/L3 fault tolerance keys off of
// (L3 = whole-object parse fail; L2 = a chunk index missing; L1 = a field
// missing inside one chunk's object).
// Chinese-language enrichment prompt — sent to the LLM when enriching Chinese
// documents (multilingual content support; this is functional engine data, NOT a
// stray translatable string, so the Chinese body is intentionally preserved). The
// English counterpart is kEnBody below; PromptTemplate selects between them by id.
const std::string kZhBody =
    "你是一个文本分析助手。下面有若干文本块，每个块以 ===CHUNK i=== 开头（i 为从 0 "
    "开始的序号）。请对每个块同时完成：(1) 命名实体识别，实体类型取值范围 "
    "PERSON/ORG/DATE/MONEY/LOC/EVENT/PRODUCT；(2) 一句话摘要；(3) 给出一个 0.0 到 "
    "1.0 之间的语义质量分。\n"
    "只返回一个 JSON 对象，键为块序号的字符串形式，值为对象 "
    "{\"entities\":[{\"text\":\"\",\"type\":\"\",\"start_offset\":0,\"end_offset\":0}],"
    "\"summary\":\"\",\"score\":0.0}。不要输出 JSON 以外的任何内容。\n";

const std::string kEnBody =
    "You are a text analysis assistant. Below are several text chunks, each "
    "starting with ===CHUNK i=== (i is a 0-based index). For each chunk, do all "
    "of: (1) named entity recognition, with entity type in "
    "PERSON/ORG/DATE/MONEY/LOC/EVENT/PRODUCT; (2) a one-sentence summary; (3) a "
    "semantic quality score between 0.0 and 1.0.\n"
    "Return ONLY a single JSON object whose keys are the chunk indices as "
    "strings, each value an object "
    "{\"entities\":[{\"text\":\"\",\"type\":\"\",\"start_offset\":0,\"end_offset\":0}],"
    "\"summary\":\"\",\"score\":0.0}. Output nothing other than the JSON.\n";
}  // namespace

const std::map<std::string, std::string>& PromptTemplate::Templates() {
    static const std::map<std::string, std::string> kTemplates = {
        {kDefaultZh, kZhBody},
        {kDefaultEn, kEnBody},
    };
    return kTemplates;
}

bool PromptTemplate::IsKnown(const std::string& template_id) {
    return Templates().count(template_id) > 0;
}

PromptTemplate::PromptTemplate(const std::string& template_id) {
    const auto& templates = Templates();
    auto it = templates.find(template_id);
    if (it == templates.end()) {
        it = templates.find(kDefaultZh);  // unknown → default-zh fallback
    }
    id_ = it->first;
    instruction_ = &it->second;
}

std::string PromptTemplate::Render(const std::vector<ChunkContext>& contexts) const {
    std::string out = *instruction_;
    for (size_t i = 0; i < contexts.size(); ++i) {
        out += "\n===CHUNK ";
        out += std::to_string(i);
        out += "===\n";
        out += contexts[i].chunk_text;
    }
    return out;
}

}  // namespace cortrix::spc
