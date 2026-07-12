# CUDA Execution Provider Operations

This runbook covers the Linux x86_64 CUDA image for Cortrix. The default
[`deploy/Dockerfile`](../../deploy/Dockerfile) and
[`deploy/docker-compose.yml`](../../deploy/docker-compose.yml) remain the CPU
deployment. CUDA is enabled only by building
[`deploy/Dockerfile.cuda`](../../deploy/Dockerfile.cuda) and applying
[`deploy/docker-compose.cuda.yml`](../../deploy/docker-compose.cuda.yml) as a
Compose override.

This is an operational deployment guide, not a performance benchmark.

## Compatibility contract

- Host operating system and container architecture: Linux x86_64
  (`linux/amd64`).
- Host: an NVIDIA GPU, a compatible NVIDIA driver, Docker, Docker Compose, and
  NVIDIA Container Toolkit configured for Docker.
- Image runtime: CUDA 11.8 with cuDNN 8 on Ubuntu 22.04.
- ONNX Runtime: the repository-locked official 1.17.1 Linux x86_64 GPU asset.
  CMake verifies its locked SHA-256 while building.
- The locked ONNX Runtime 1.17.1 GPU archive links against CUDA 11.x and
  cuDNN 8.x (`libcudart.so.11.0`, `libcublas.so.11`, and `libcudnn.so.8`).
- The NVIDIA CUDA base image supplies `NVIDIA_REQUIRE_CUDA`; NVIDIA Container
  Toolkit checks that constraint against the host driver before starting the
  container. Do not bypass that check.

References:

- [ONNX Runtime CUDA Execution Provider requirements](https://onnxruntime.ai/docs/execution-providers/CUDA-ExecutionProvider.html)
- [NVIDIA Container Toolkit installation](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/install-guide.html)
- [NVIDIA GPU enumeration and driver capabilities](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/docker-specialized.html)

Confirm the host before building:

```bash
uname -m
nvidia-smi
docker version
docker compose version
```

`uname -m` must report `x86_64`, and `nvidia-smi` must list the intended GPU.
Install and configure NVIDIA Container Toolkit by following NVIDIA's guide if
Docker cannot request a GPU.

## Execution-provider behavior

Embedding and reranking are selected independently with the same four values:

| Value | CUDA image behavior |
| --- | --- |
| `auto` | Try CUDA first. If provider attachment or session creation fails, record the reason and create a fresh CPU-only session. |
| `cpu` | Create a CPU session without attempting CUDA. |
| `cuda` | Require CUDA. Provider or session failure aborts startup; there is no CPU fallback. |
| `coreml` | Intended for a CoreML-enabled Apple build. It is unavailable in the Linux CUDA image and fails configuration validation. |

The canonical YAML keys are:

```yaml
embedding:
  execution_provider: auto

reranker:
  execution_provider: auto
```

The canonical environment variables are:

```dotenv
CORTRIX_EMBEDDING_EXECUTION_PROVIDER=auto
CORTRIX_RERANKER_EXECUTION_PROVIDER=auto
```

Environment variables take precedence over YAML. Values are case-insensitive,
but lowercase values are recommended.

### Legacy aliases

The following aliases remain accepted for migration only:

- `embedding.gpu_provider` and `CORTRIX_EMBEDDING_DEVICE`
- `reranker.use_coreml: true` and `CORTRIX_RERANKER_USE_COREML=true`

Do not set a canonical key and a legacy alias to different values within the
same YAML or environment layer. Cortrix treats that as a configuration error
and refuses to start. New deployments should use only the canonical names.

## Model-profile startup contract

`CORTRIX_PROFILE=full` is the default. The bundled container configuration has
a non-empty embedding model and tokenizer path. On first start, the entrypoint
provisions `model.onnx`, `model.onnx_data`, and `tokenizer.json` into the data
volume. If those required assets remain missing or empty, the entrypoint exits
with an error. A configured invalid model or tokenizer also causes the server
to fail during initialization.

The reranker remains optional in the bundled configuration: an empty
`reranker.model_dir` is its explicit stub mode. If an operator supplies a
non-empty directory, its model and tokenizer must be valid or server startup
fails.

`CORTRIX_PROFILE=lite` is the explicit development stub. Before startup, the
entrypoint rewrites the runtime copy of `cortrix.yaml` so
`embedding.model_path` and `embedding.tokenizer_path` are empty, then skips
model and parser provisioning. If a custom configuration is mounted read-only,
lite startup fails because it cannot make this explicit change.

An empty embedding model path is the only embedding stub mode. Missing assets
at a non-empty path never silently degrade to a stub.

## Build

Run all commands from the repository root.

Create the local environment file and review it before starting:

```bash
cp deploy/.env.example deploy/.env
```

At minimum, choose the profile and providers:

```dotenv
CORTRIX_PROFILE=full
CORTRIX_EMBEDDING_EXECUTION_PROVIDER=auto
CORTRIX_RERANKER_EXECUTION_PROVIDER=auto
NVIDIA_VISIBLE_DEVICES=all
NVIDIA_DRIVER_CAPABILITIES=compute,utility
```

To expose only one GPU, set `NVIDIA_VISIBLE_DEVICES` to a GPU index or UUID. Do
not add graphics or display capabilities; Cortrix needs `compute` and `utility`.

Build the CUDA image through the base Compose file plus its override:

```bash
docker compose \
  -f deploy/docker-compose.yml \
  -f deploy/docker-compose.cuda.yml \
  build cortrix
```

The build is restricted to `linux/amd64`. It does not bake model files into the
image and must not add generated packages, model assets, or binaries to Git.

## Deploy and use

Start the CUDA deployment:

```bash
docker compose \
  -f deploy/docker-compose.yml \
  -f deploy/docker-compose.cuda.yml \
  up -d
```

Follow startup until the service is healthy:

```bash
docker compose \
  -f deploy/docker-compose.yml \
  -f deploy/docker-compose.cuda.yml \
  logs -f cortrix
```

For a normal CUDA-capable deployment, keep both providers on `auto`. To require
GPU initialization for either component, change that component to `cuda` and
recreate the service:

```bash
docker compose \
  -f deploy/docker-compose.yml \
  -f deploy/docker-compose.cuda.yml \
  up -d --force-recreate cortrix
```

The entrypoint checks GPU visibility and resolves the CUDA provider's dynamic
library dependencies. A failed pre-flight is fatal when either configured
provider is explicitly `cuda`. With `auto`, the pre-flight is reported and the
server is allowed to exercise its fresh CPU-session fallback.

When both components are configured with `cpu`, the entrypoint skips
`nvidia-smi` and CUDA-library pre-flight entirely. If one component uses
`auto` or `cuda`, GPU pre-flight still runs for that component.

## Verify

### 1. Container and GPU

```bash
docker compose \
  -f deploy/docker-compose.yml \
  -f deploy/docker-compose.cuda.yml \
  ps

docker compose \
  -f deploy/docker-compose.yml \
  -f deploy/docker-compose.cuda.yml \
  exec cortrix nvidia-smi -L
```

The service must be running or healthy, and only the GPUs selected by
`NVIDIA_VISIBLE_DEVICES` should appear.

### 2. Runtime libraries

```bash
docker compose \
  -f deploy/docker-compose.yml \
  -f deploy/docker-compose.cuda.yml \
  exec cortrix sh -c \
  'out=$(ldd /usr/local/lib/libonnxruntime_providers_cuda.so) && printf "%s\n" "$out" && ! printf "%s\n" "$out" | grep -q "not found"'
```

This command succeeds only when `ldd` reports no unresolved dependencies.

### 3. API health

```bash
curl --fail --silent --show-error \
  http://localhost:${CORTRIX_HTTP_PORT:-8420}/api/v1/health
```

### 4. Build flavor, active providers, and fallback counters

The OpenMetrics listener is internal on port 9091, so query it through the
container:

```bash
docker compose \
  -f deploy/docker-compose.yml \
  -f deploy/docker-compose.cuda.yml \
  exec -T cortrix sh -c \
  'curl -fsS http://127.0.0.1:9091/metrics | grep -E "cortrix_onnx_build_info|embedding_(configured|active)_ep|embedding_ep_(fallback|init_failure)_total|reranker_(configured|active)_ep|reranker_ep_(fallback|init_failure)_total"'
```

Expected CUDA build marker:

```text
cortrix_onnx_build_info{runtime_flavor="cuda"} 1
```

With a valid model and working GPU, `configured_ep="auto"` should pair with
`active_ep="cuda"`. If an `auto` CUDA attempt fails, `active_ep="cpu"` and the
matching fallback counter increases with `reason="ep_init_failed"` or
`reason="session_init_failed"`. A component with no configured model reports
`active_ep="stub"`.

The matching `ep_init_failure_total` counter records failed CPU, CoreML, or
CUDA provider/session attempts even when no fallback is allowed.

Do not infer CUDA use from image name or GPU visibility alone; verify the active
provider metric.

## Roll back

To switch back to the default CPU image while preserving the named data volume:

```bash
docker compose \
  -f deploy/docker-compose.yml \
  -f deploy/docker-compose.cuda.yml \
  down

docker compose -f deploy/docker-compose.yml up -d --build
```

Do not add `--volumes` to the `down` command unless data deletion is explicitly
intended.

To keep the CUDA image but temporarily force CPU sessions, set both canonical
provider variables to `cpu` and recreate the service. This is an operational
mitigation, not an image rollback; the CUDA runtime and GPU reservation remain.

For a CPU-isolation check that does not reserve or expose a GPU, run the CUDA
image without `--gpus` and set both providers to `cpu`. Use an existing data
volume for `full`, or `CORTRIX_PROFILE=lite` for the explicit stub smoke. The
entrypoint must report that GPU pre-flight was skipped.

## Troubleshooting

### Docker reports that no compatible GPU driver is available

Confirm `nvidia-smi` works on the host, then confirm NVIDIA Container Toolkit is
installed and configured for Docker. The Compose override requests a device
with `driver: nvidia` and `capabilities: [gpu]`; plain Docker without the
toolkit cannot satisfy it.

### The container is rejected before the entrypoint starts

Check the Docker daemon logs for an `NVIDIA_REQUIRE_CUDA` or driver-version
constraint failure. Upgrade the host driver to one compatible with the image's
CUDA 11.8 runtime. Do not set `NVIDIA_DISABLE_REQUIRE=true` to bypass the
compatibility gate.

### CUDA pre-flight cannot see a GPU

Check the selected `NVIDIA_VISIBLE_DEVICES` value, GPU UUID/index, and
`NVIDIA_DRIVER_CAPABILITIES`. The latter must include `compute,utility`. Compare
host `nvidia-smi -L` with the same command executed inside the container.

### `ldd` reports CUDA or cuDNN libraries as `not found`

Rebuild from `deploy/Dockerfile.cuda`, confirm the runtime stage is the CUDA
11.8/cuDNN 8 image, and confirm the NVIDIA runtime injected `libcuda.so.1` from
the host. Do not copy host CUDA libraries into the repository or image build
context.

### `auto` starts on CPU

Read the startup warning and inspect the component's fallback counter. An
`ep_init_failed` reason points to provider attachment or dynamic dependencies;
`session_init_failed` points to CUDA session/model initialization. Fix the
cause and recreate the service. `auto` fallback uses a fresh CPU-only session;
it does not reuse partially configured CUDA session options.

### Explicit `cuda` repeatedly exits

This is the intended fail-closed behavior. Check GPU visibility, `ldd`, model
and tokenizer paths, and startup logs. Use `auto` only if CPU fallback is an
accepted availability policy; otherwise keep `cuda` and repair the deployment.

### CUDA out of memory

A session-creation OOM is a `session_init_failed` event. `auto` may create a
fresh CPU session; explicit `cuda` exits. An inference-time OOM does not rebind
the live session to another provider: the request fails and the inference
failure metric increases. Reduce model concurrency, batch size, or sequence
length, or move the deployment to a GPU with sufficient memory before retrying.

### Full profile exits after model provisioning

Confirm all three files are present and non-empty in the data volume:

```text
/data/models/bge-m3/model.onnx
/data/models/bge-m3/model.onnx_data
/data/models/bge-m3/tokenizer.json
```

Fix network or volume permissions and restart. Full mode does not turn a failed
download into a stub.

### Lite profile says the config is not writable

The lite contract requires the entrypoint to clear both embedding paths. Remove
the read-only config mount, provide a writable runtime copy, or use a custom
configuration whose embedding paths are already empty with an entrypoint that
preserves that explicit state.

### Startup reports a canonical/legacy conflict

Remove the deprecated alias and keep only
`CORTRIX_EMBEDDING_EXECUTION_PROVIDER` or
`CORTRIX_RERANKER_EXECUTION_PROVIDER`. The entrypoint does not synthesize a
canonical default when a legacy alias is explicitly supplied, so matching
legacy-only deployments can still migrate cleanly.
