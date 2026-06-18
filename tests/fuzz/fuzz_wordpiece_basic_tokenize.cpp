/// @file fuzz_wordpiece_basic_tokenize.cpp
/// @brief libFuzzer harness for WordPieceTokenizer::BasicTokenize (UTF-8 path).
///
/// Target: the BERT normalizer + pre-tokenizer pipeline
///   CleanText -> PadChineseChars -> ToLower -> StripAccents -> whitespace/punct split
/// driven by NextCodePoint, which hand-decodes UTF-8. Malformed/truncated UTF-8,
/// control chars, combining marks, and astral code points all flow through here.
///
/// BasicTokenize() is `const` and reads no vocab, so a default-constructed
/// tokenizer suffices -- no model file, no ONNX, no I/O. The harness compiles the
/// real src/query/wordpiece_tokenizer.cpp; it is the production code, not a copy.
///
/// Build via tests/fuzz/build_fuzzers.sh (homebrew LLVM, -fsanitize=fuzzer,address).
/// NOT part of the main CMake build (keeps the product build off homebrew LLVM).

#include <cstddef>
#include <cstdint>
#include <string>

#include "cortrix/query/wordpiece_tokenizer.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // One default-constructed tokenizer reused across iterations (BasicTokenize is
    // const + stateless w.r.t. the object; cheaper than per-call construction).
    static const cortrix::query::WordPieceTokenizer tok;

    std::string text(reinterpret_cast<const char*>(data), size);

    // The function under test. ASan watches for OOB reads in the UTF-8 walk and the
    // substr() offset arithmetic; libFuzzer maximizes coverage of the code-point
    // classification branches. Result is intentionally ignored -- we want crashes/UB.
    volatile auto words = tok.BasicTokenize(text);
    (void)words;
    return 0;
}
