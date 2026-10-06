---
title: NVIDIA CUDA
description: Run embedding and reranking on an NVIDIA GPU on Linux.
weight: 40
---

Build and run the Cortrix CUDA image, then confirm that the models are really running on the GPU.

The default `deploy/Dockerfile` and `deploy/docker-compose.yml` are a CPU deployment. CUDA is turned on only when you build `deploy/Dockerfile.cuda` and layer `deploy/docker-compose.cuda.yml` on top.

> [!NOTE]
> This page is adapted from the [CUDA execution provider runbook](https://github.com/cortrix/cortrix/blob/release/1.0/docs/operations/cuda-execution-provider.md) and hasn't been tested on a GPU machine. It's a deployment guide and contains no performance data. Complete the verification steps on your target machine before you treat CUDA as verified.

## Prerequisites {#prerequisites}

- A Linux x86_64 host and container architecture.
- An NVIDIA GPU and a compatible driver.
- Docker, Docker Compose, and NVIDIA Container Toolkit configured for Docker.

The image runtime is CUDA 11.8 and cuDNN 8 on Ubuntu 22.04, with the repository-pinned ONNX Runtime 1.17.1 GPU build.

Host driver requirements:

| Host NVIDIA driver | Notes |
|---|---|
| `>= 520.61.05` | The required baseline, and the recommended version for deployment |
| `>= 450.80.02` and `< 520.61.05` | NVIDIA documents compatibility in this range, with limitations. Cortrix doesn't treat this range as validated |
| `< 450.80.02` | Below the compatibility floor for CUDA 11.8. Don't use it |

Check the host before you build:

```bash
uname -m
nvidia-smi
nvidia-smi --query-gpu=name,uuid,driver_version --format=csv,noheader
docker version
docker compose version
```

`uname -m` must print `x86_64`, and `nvidia-smi` must list the GPU you intend to use.

## Execution provider values {#execution-provider-values}

Embedding and reranking each choose an execution provider independently, from the same set of values:

| Value | Behavior in the CUDA image |
|---|---|
| `auto` | Try CUDA first. If it fails, record the reason and create a fresh CPU-only session |
| `cpu` | Use the CPU without trying CUDA |
| `cuda` | Require CUDA. A failure aborts startup, with no CPU fallback |
| `coreml` | For builds on Apple platforms. Not available in the Linux CUDA image |

Environment variables take precedence over the YAML configuration. Values aren't case-sensitive, but lowercase is recommended.

## Steps {#steps}

Run every command from the root of the repository.

{{% steps %}}

### Prepare the environment file {#env-file}

```bash
cp deploy/.env.example deploy/.env
```

At a minimum, confirm these settings:

```dotenv {title="deploy/.env"}
CORTRIX_PROFILE=full
CORTRIX_EMBEDDING_EXECUTION_PROVIDER=auto
CORTRIX_RERANKER_EXECUTION_PROVIDER=auto
NVIDIA_VISIBLE_DEVICES=all
NVIDIA_DRIVER_CAPABILITIES=compute,utility
```

> [!WARNING]
> In `v1.0.0-rc.2`, `deploy/docker-compose.yml` sets `CORTRIX_PROFILE` to `quickstart` and both execution providers to `cpu`, and it doesn't load `deploy/.env`. The CUDA override file changes only the image, platform, build, and device reservation. As written, the values above don't reach the container, and the stack starts on the CPU. Check the provider with the verification steps below, and expect to edit the Compose environment yourself until this is fixed in the repository.

To use only one GPU, set `NVIDIA_VISIBLE_DEVICES` to its index or UUID. Cortrix needs only the `compute` and `utility` capabilities.

### Build the image {#build}

```bash
docker compose \
  -f deploy/docker-compose.yml \
  -f deploy/docker-compose.cuda.yml \
  build cortrix
```

The build is restricted to `linux/amd64`, and model files aren't baked into the image.

### Start the stack {#start}

```bash
docker compose \
  -f deploy/docker-compose.yml \
  -f deploy/docker-compose.cuda.yml \
  up -d
```

Follow the startup log until the service is healthy:

```bash
docker compose \
  -f deploy/docker-compose.yml \
  -f deploy/docker-compose.cuda.yml \
  logs -f cortrix
```

{{% /steps %}}

## Verify {#verify}

Don't conclude that CUDA is in use from the image name or from GPU visibility alone. Check each of these.

### Check that the container sees the GPU {#verify-gpu}

```bash
docker compose \
  -f deploy/docker-compose.yml \
  -f deploy/docker-compose.cuda.yml \
  exec cortrix nvidia-smi -L
```

Only the GPUs selected by `NVIDIA_VISIBLE_DEVICES` should be listed.

### Check the runtime libraries {#verify-libraries}

```bash
docker compose \
  -f deploy/docker-compose.yml \
  -f deploy/docker-compose.cuda.yml \
  exec cortrix sh -c \
  'out=$(ldd /usr/local/lib/libonnxruntime_providers_cuda.so) && printf "%s\n" "$out" && ! printf "%s\n" "$out" | grep -q "not found"'
```

This command succeeds only when there are no missing dependencies.

### Check the active execution provider {#verify-active-ep}

```bash
curl -fsS http://localhost:8420/api/v1/system/health/ready
```

Look at `components.embedding_execution_provider` and `components.reranker_execution_provider`:

| Situation | What you see |
|---|---|
| CUDA is working | `active_ep` is `cuda` |
| `auto` fell back to the CPU | The service is still ready, but `fallback` is `true`, `active_ep` is `cpu`, and `preferred_ep` is `cuda` |
| The explicit setting and reality don't match | The service isn't ready, and `policy_mismatch` is `true` |
| No model is configured | `model_configured` is `false` and `active_ep` is `stub` |

### Check the metrics {#verify-metrics}

The metrics port, 9091, is open only inside the container, so query it from there:

```bash
docker compose \
  -f deploy/docker-compose.yml \
  -f deploy/docker-compose.cuda.yml \
  exec -T cortrix sh -c \
  'curl -fsS http://127.0.0.1:9091/metrics | grep -E "cortrix_onnx_build_info|embedding_(configured|active)_ep|reranker_(configured|active)_ep"'
```

A CUDA build includes this line:

```text
cortrix_onnx_build_info{runtime_flavor="cuda"} 1
```

## Require the GPU {#require-gpu}

A typical CUDA deployment keeps both components on `auto`. If you'd rather have startup fail than fall back to the CPU when GPU initialization fails, change that component to `cuda` and recreate the service:

```bash
docker compose \
  -f deploy/docker-compose.yml \
  -f deploy/docker-compose.cuda.yml \
  up -d --force-recreate cortrix
```

## Roll back to CPU {#roll-back}

To switch back to the default CPU image and keep the data volume:

```bash
docker compose \
  -f deploy/docker-compose.yml \
  -f deploy/docker-compose.cuda.yml \
  down
docker compose -f deploy/docker-compose.yml up -d --build
```

> [!CAUTION]
> Don't add `--volumes` to `down` unless you really mean to delete your data.

To keep the CUDA image and just use the CPU for now, set both execution provider variables to `cpu` and recreate the service.

## Troubleshooting {#troubleshooting}

| Symptom | What to do |
|---|---|
| Docker reports that no GPU driver is available | Confirm `nvidia-smi` works on the host, and that NVIDIA Container Toolkit is installed and configured for Docker |
| The container is rejected before the entrypoint runs | Check the Docker daemon log for an `NVIDIA_REQUIRE_CUDA` or driver-version constraint error, and upgrade the host driver. Don't set `NVIDIA_DISABLE_REQUIRE=true` to bypass the check |
| The pre-flight check can't see a GPU | Check the value of `NVIDIA_VISIBLE_DEVICES`, and that `NVIDIA_DRIVER_CAPABILITIES` includes `compute,utility` |
| `ldd` reports CUDA or cuDNN libraries as `not found` | Rebuild from `deploy/Dockerfile.cuda` and confirm the runtime stage is the CUDA 11.8 and cuDNN 8 image |

## Next steps {#next-steps}

- [Performance tuning](/docs/deploy/performance-tuning/)
- [Models and ONNX](/docs/deploy/models/)
- [Compatibility and status](/docs/resources/compatibility/)
