# ONNX Runtime Upgrade & Rollback SOP

> Operational runbook for upgrading the ONNX Runtime that `cortrix-server`
> links. Covers the CPU flavor's common same-major `.so` swap, rollback, the
> rare cross-major rebuild, CUDA provider bundles, and rolling upgrades.
>
> Source of truth for the mechanism: `design/features/F22-onnx-runtime-upgrade.md`.

## Background — what an upgrade actually changes

`cortrix-server` links the ONNX Runtime library (`libonnxruntime.so` on Linux,
`libonnxruntime.<version>.dylib` on macOS). Cortrix locks the **ABI major
version** at build time via the CMake flag `ONNXRT_MAJOR_VERSION` (default `1`).

- **Same-major CPU-flavor upgrade (e.g. 1.17 -> 1.19).** ONNX Runtime is
  ABI-compatible within a major version, so a CPU deployment can replace the
  core library file and restart the process. **No rebuild.**
- **CUDA-flavor upgrade.** Treat the core runtime and execution-provider shared
  objects as one versioned bundle. Rebuild the CUDA image from a pinned ORT
  archive and compatible CUDA/cuDNN base; never replace only
  `libonnxruntime.so` while retaining provider libraries from another release.
- **Cross-major upgrade (e.g. 1.x -> 2.x) — every few years.** ABI breaks across
  majors, so you rebuild `cortrix-server` with `ONNXRT_MAJOR_VERSION` bumped,
  then deploy the new binary.

At startup, `cortrix::onnx::StartupValidator` runs a fail-fast check before the
server begins serving:

1. the loaded runtime's major version matches the build-time
   `ONNXRT_MAJOR_VERSION` (else `CX_ERR_ONNXRT_VERSION_MISMATCH`);
2. every registered model's ONNX opset is within the loaded runtime's supported
   opset range (else `CX_ERR_ONNX_OPSET_INCOMPATIBLE`).

If either check fails the server aborts with a clear error rather than starting
in a broken state. There is no automatic downgrade or fallback — upgrades and
rollbacks are operator-driven (symmetric with "manual upgrade").

Always run the dry-run check **before** restarting, so an incompatibility is
caught without taking the service down:

```bash
cortrix-server --check-onnx
```

It validates the loaded runtime + registered models and exits 0 (safe) or 1
(do not restart — see the printed `CX_ERR_*` code).

`--check-onnx` does not prove CUDA driver visibility or CUDA provider dynamic
dependencies. CUDA deployments must also follow the `ldd`, active-provider,
and capability-smoke checks in the
[CUDA operations runbook](cuda-execution-provider.md).

---

## Scenario A — same-major CPU-flavor upgrade (no rebuild)

Do not use this single-library procedure for the CUDA flavor. For CUDA, update
the pinned ORT version, URL/hash, and compatible CUDA/cuDNN image together,
rebuild `deploy/Dockerfile.cuda`, then run the CUDA capability smoke.

```bash
# 1. Back up the current library (tag it with the version you are replacing).
sudo cp /usr/local/lib/libonnxruntime.so \
        /usr/local/lib/libonnxruntime.so.$(cortrix-server --runtime-version).bak

# 2. Install the new library (download from the ONNX Runtime releases page).
sudo cp ~/Downloads/onnxruntime-1.19.0/lib/libonnxruntime.so /usr/local/lib/

# 3. Dry-run the validation WITHOUT starting the server.
cortrix-server --check-onnx
#    exit 0 + "[OK] All checks passed" -> go to step 4
#    exit 1 -> roll back the .so (step 2 of Scenario B) and read the error code

# 4. Restart the server.
sudo systemctl restart cortrix-server

# 5. Verify with a real query.
curl -X POST http://localhost:8420/v1/search -d '{...}'
```

Expected `--check-onnx` output on a compatible upgrade:

```
[OK] ONNX Runtime: 1.19.0 (major=1, matches CMake expectation)
[OK] Model /models/bge-m3/model.onnx: opset 17 in [7, 21]
[OK] All checks passed. Safe to restart cortrix-server.
```

---

## Scenario B — upgrade failed, roll back

```bash
# 1. Read the ONNX error code from the logs.
journalctl -u cortrix-server -n 100 | grep CX_ERR_ONNX

# 2. Decide based on the code:
#    CX_ERR_ONNXRT_VERSION_MISMATCH  -> wrong major; this is a cross-major change
#                                       and needs a rebuild (Scenario C), not a
#                                       same-major swap. Roll back the .so.
#    CX_ERR_ONNX_OPSET_INCOMPATIBLE  -> a model's opset is not supported by the
#                                       new runtime. Roll back the .so, or
#                                       re-export the model at a supported opset.
#    CX_ERR_ONNX_INFERENCE_FAILED    -> a run-time failure (input data / memory /
#                                       a corrupt model), not necessarily caused
#                                       by the upgrade — investigate the input.

# 3. Restore the backed-up library.
sudo cp /usr/local/lib/libonnxruntime.so.OLD_VERSION.bak \
        /usr/local/lib/libonnxruntime.so

# 4. Restart and re-verify.
sudo systemctl restart cortrix-server
cortrix-server --check-onnx
```

The `--check-onnx` failure surface carries machine-readable detail an operator
(or an Agent reading the log) can act on directly, e.g.:

```
[FAIL] CX_ERR_ONNX_OPSET_INCOMPATIBLE
  model_path: /models/bge-reranker-v2-m3.onnx
  model_opset: 20
  supported_opset_range: [7, 19]
  action: downgrade_model_or_upgrade_runtime
```

---

## Scenario C — cross-major upgrade (rare, requires rebuild)

A cross-major bump (e.g. `1.x` -> `2.x`) breaks ABI, so the library swap alone is
not enough — the binary must be rebuilt against the new major.

```bash
# 1. Bump the locked major in the build.
#    CMakeLists / Dependencies.cmake:  ONNXRT_MAJOR_VERSION "1" -> "2"
#    (also point ONNXRUNTIME_VERSION at the new 2.x release used to build/link).

# 2. Rebuild cortrix-server.
cmake -S . -B build -DONNXRT_MAJOR_VERSION=2
cmake --build build --target cortrix-server -j2

# 3. Deploy the new binary AND the matching 2.x library
#    (follow Scenario A steps 1-2 to place the 2.x .so).

# 4. Dry-run, then restart and verify.
cortrix-server --check-onnx
sudo systemctl restart cortrix-server
```

If you swap to a 2.x library without rebuilding, startup aborts with
`CX_ERR_ONNXRT_VERSION_MISMATCH` (loaded major 2 != compiled-for major 1) — by
design, so a mismatched deployment can never serve traffic.

---

## Scenario D — multi-instance rolling (zero-downtime) upgrade

Phase 1 V1.0 OSS ships as a **single self-hosted instance**; a restart is a
short (~5s) window of unavailability. Multi-instance / Kubernetes deployment is
a Phase 2 concern.

If you have already fronted several `cortrix-server` processes (or K8s pods)
with a load balancer, you can upgrade them one at a time for zero downtime:

1. Take instance 1 out of rotation -> swap its `.so` (Scenario A) -> `--check-onnx`
   -> restart -> add back to rotation. The other instances keep serving.
2. Verify instance 1 is healthy, then roll the same steps across instances 2..N.
3. Use the LB / K8s Service to isolate the instance currently being upgraded.

ONNX runtime does not block this pattern, but the orchestration itself (LB draining,
readiness probes, rollout ordering) belongs to your deployment layer (systemd /
K8s / custom LB), not to `cortrix-server`.

---

## Observability — what to watch after an upgrade

`cortrix-server` exposes ONNX runtime, inference, and execution-provider metrics (OpenMetrics, prefix
`cortrix_onnx_*`). After an upgrade, an operator or ops Agent can confirm health
by querying:

```promql
# Startup validation failures since the upgrade (should be 0).
sum(rate(cortrix_onnx_startup_validation_total{result!="ok"}[5m]))

# The ONNX Runtime version the running process actually loaded.
cortrix_onnx_runtime_version_info

# Inference failure rate — watch for a regression after the swap.
rate(cortrix_onnx_inference_failed_total[5m])
```

A spike in `cortrix_onnx_*` errors right after an upgrade points squarely at the
runtime swap — roll back per Scenario B.
