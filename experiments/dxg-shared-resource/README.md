# WSL DXG cross-process shared D3D12 resource probe

This is a narrow **userspace capability test**, not a kernel patch or
DRM/PRIME implementation. It tests the opposite direction from importing
guest-allocated memory: D3D12 itself allocates a shared default-heap GPU
buffer, and a second WSL Linux process tries to use it.

The producer selects a DXCore hardware adapter, creates a 64 KiB shared
D3D12 resource, and copies a per-run generated pattern into it using the
GPU queue. It waits for its fence and returns the resource to the COMMON
state before exporting via D3D12 `CreateSharedHandle`. The Linux file
descriptor is passed over a private Unix socket with `SCM_RIGHTS`. The
second process is started with a fresh `exec`, does not inherit the GPU
device or memory, selects the same adapter LUID, calls `OpenSharedHandle`,
copies the resource to its own readback buffer on its GPU queue, waits
for its own fence, and checks every byte. The exporter keeps its resource
alive through the importer's exit.

## Build and run in the existing WSL distro

Requirements: Git, a C++ compiler, and the existing
`/usr/lib/wsl/lib/libdxcore.so` and `libd3d12.so` WSL runtime. No sudo,
custom kernel, GPU settings change, or WSLg system image is needed. On
Tumbleweed, enter `nix shell nixpkgs#gcc nixpkgs#git` first if the tools
aren't available. The build helper makes a temporary binary.

```sh
git clone --depth 1 --filter=blob:none --sparse -b yoi/dxg-shared-resource-probe \
  https://github.com/jihoo12/WSL2-Linux-Kernel dxg-share-probe
git -C dxg-share-probe sparse-checkout set experiments/dxg-shared-resource
git clone --depth 1 -b v1.619.1 \
  https://github.com/microsoft/DirectX-Headers DirectX-Headers
YDXG_ADAPTER="RTX A4000" \
  sh dxg-share-probe/experiments/dxg-shared-resource/build-and-run.sh DirectX-Headers
```

If you already cloned DirectX-Headers from the preceding probe, reuse it.
Omit the adapter filter to allow the first working hardware adapter.
Append `--test-ipc` to the build helper to test only Unix FD transfer and
process isolation on any Linux host. A fixture `PASS` is not GPU success.
GPU run returns 77 if no `/dev/dxg` is found and 2 on a stage-specific
failure. Only the full byte-comparison `PASS` verifies GPU resource
sharing through this D3D12 pathway.

A DXG resource descriptor is **not DMA-BUF**. A positive result proves
cross-process D3D12 interoperability only; it does not verify DRM PRIME,
Mesa GBM import, Wayland transport, or working Shady. A negative result
identifies the first failing public-API step, not a conclusion about all
WSL graphics paths.

The pinned headers build with GCC and strict warnings. Cloud verification
covered the fresh-process FD transfer fixture and the expected no-GPU
skip; actual DXG GPU handle import still needs a WSL hardware run.
