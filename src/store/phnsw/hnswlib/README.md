# Vendored hnswlib (P-HNSW shallow fork)

This directory is a **vendored copy** of [hnswlib](https://github.com/nmslib/hnswlib),
the header-only HNSW graph index used by Cortrix's P-HNSW vector index (F01).

| | |
|---|---|
| Upstream | https://github.com/nmslib/hnswlib |
| Version | v0.8.0 |
| Commit | `3f3429661187e4c24a490a0f148fc6bc89042b3d` |
| License | Apache-2.0 (see `LICENSE`) |
| Vendored | 2026-05-30 (F01 S1) |

## Why vendored instead of FetchContent

F01 (Persistent HNSW) is a **shallow fork**: we keep hnswlib's graph algorithm
untouched and add persistence (WAL + snapshot) *around* it in `../`. Vendoring the
source in-tree (rather than pulling it via CMake `FetchContent` at configure time)
gives us:

- A stable, auditable pin of the exact graph code we ship (Cortrix CE is open
  source; downstream contributors see precisely what is built).
- A place to land the few persistence-oriented patches the shallow fork needs,
  without forking upstream's whole repo.
- Hermetic, offline builds (no network fetch for the core index).

## Modification policy

**Do not modify the graph algorithm** (`hnswalg.h`, `space_*.h`,
`visited_list_pool.h`, `bruteforce.h`, `stop_condition.h`). P-HNSW's persistence
layer (WAL writer, group commit, snapshot manager) lives **outside** this folder
in `src/store/phnsw/`. Any persistence hook that genuinely requires touching
upstream code must be recorded here with a `// CORTRIX-PATCH:` comment and a note
in this README, so re-syncing with upstream stays mechanical.

As of F01 S1 there are **no local modifications** — this is a clean copy of v0.8.0.
