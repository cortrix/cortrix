# Fuzzing -- parse entry points

libFuzzer harnesses for Cortrix's externally-reachable parse entry points. These are
**standalone** -- deliberately NOT wired into the main CMake build, because libFuzzer
requires the homebrew LLVM clang (Apple clang ships no `libclang_rt.fuzzer_osx.a`) and
the product build must not depend on it.

## Toolchain

- `clang++` from homebrew LLVM (`/opt/homebrew/opt/llvm/bin/clang++`, v22+), which
  bundles `libclang_rt.fuzzer_osx.a`. Override with `CLANGXX=...`.
- Sanitizers: `-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all`.
- All outputs go to `build-fuzz/` (gitignored; isolated from `build/`, `build-cov/`,
  `build-llvmcov/`).

## Build & run

```bash
tests/fuzz/build_fuzzers.sh                 # build all
tests/fuzz/build_fuzzers.sh wordpiece       # build one (substring match)

# run (seeds/ = tracked starting corpus; corpus/ = gitignored growth):
mkdir -p tests/fuzz/corpus/flatten_metadata
build-fuzz/fuzz_flatten_metadata -max_total_time=60 -max_len=8192 \
    tests/fuzz/corpus/flatten_metadata tests/fuzz/seeds/flatten_metadata
```

## Targets

| Harness | Entry point under test | Real TU or copy |
|---|---|---|
| `fuzz_wordpiece_basic_tokenize` | `WordPieceTokenizer::BasicTokenize` (BERT normalizer + UTF-8 decode) | real `src/query/wordpiece_tokenizer.cpp` |
| `fuzz_query_request_fromjson` | `json::parse` + `QueryRequest::FromJson` + `Normalize` + `Validate` (mirrors `query_routes.cpp`) | real `src/query/query_request.cpp` |
| `fuzz_cross_ns_parse_request` | cross-NS `ParseRequest` (F04 Sec.2.4) | verbatim copy (real TU pulls scatter-gather); drift-guard in header |
| `fuzz_flatten_metadata` | `FlattenMetadataIntoMap` (`json::parse` + `.dump()` of nested values) | verbatim copy; drift-guard in header |

The two "verbatim copy" harnesses carry the function body copied byte-for-byte from
production (their TUs transitively link heavy subsystems). If you change the real
`ParseRequest` / `FlattenMetadataIntoMap`, update the copy (drift guard noted in each
harness header).

## Results (last run)

Random fuzzing: all four clean.
`wordpiece` 3.5M execs - `query_request` 2.0M - `cross_ns` 2.18M - `flatten_metadata`
2.13M (~9.8M total exec, 0 random crashes). WordPiece is robust against malformed /
truncated UTF-8, control chars, combining marks, and astral code points.

### Fixed: deep-nested-JSON stack overflow (DoS)

Directed deep-nesting input used to crash `FlattenMetadataIntoMap` (and the ingest
routes that `.dump()` user `metadata`) with `AddressSanitizer: stack-overflow` in the
nlohmann serializer: `nlohmann::json::dump()` recurses per nesting level with no depth
bound, and a stack overflow is a SIGSEGV that the surrounding `catch (...)` cannot stop.

**Fixed.** Ingest now rejects over-deep metadata at the boundary, and
`FlattenMetadataIntoMap` has a defensive `JsonExceedsMaxDepth` guard
(`include/cortrix/common/json_depth.h`) -- an iterative, stack-based, overflow-proof
depth check -- for any data stored before that guard existed. This harness mirrors that
guard so it stays in sync with production.

The former reproducer is retained as a corpus seed, not a live crash:
`seeds/flatten_metadata/deep_nesting_20k.json`. It no longer crashes the guarded code;
it exercises the depth-limit path.
