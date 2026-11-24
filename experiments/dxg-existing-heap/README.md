# DXG guest-memory capability probe (userspace only)

This experiment tests a prerequisite for exploring a D3D12-backed DMA-BUF
on WSL2: can the stock WSL D3D12 runtime copy into **guest-provided, shared
memory**, and can another mapping of the same backing see those bytes
after a GPU fence completes?

It selects a hardware DXCore D3D12 graphics adapter, queries
`D3D12_FEATURE_EXISTING_HEAPS`, tries
`ID3D12Device3::OpenExistingHeapFromAddress` on an aligned 64 KiB
Linux memfd mapping, and submits a patterned GPU buffer copy into the
placed resource. An independent, read-only mapping of the same memfd
checks all bytes after the queue fence. Software adapters are excluded.

## Run on the existing WSL distro

No kernel build, administrator access, kernel config, or WSLg/VHD update
is required. Run in the WSL distro where D3D12 already works. You need
Git, a C++ compiler (GCC tested), and the existing WSL
`/usr/lib/wsl/lib/{libdxcore,libd3d12}.so` runtime libraries.
The helper builds a temporary executable and removes it on exit.

```sh
git clone --depth 1 --filter=blob:none --sparse -b yoi/dxg-existing-heap-probe \
  https://github.com/jihoo12/WSL2-Linux-Kernel dxg-kernel-probe
git -C dxg-kernel-probe sparse-checkout set experiments/dxg-existing-heap
git clone --depth 1 -b v1.619.1 \
  https://github.com/microsoft/DirectX-Headers DirectX-Headers
YDXG_ADAPTER="RTX A4000" \
  sh dxg-kernel-probe/experiments/dxg-existing-heap/build-and-run.sh DirectX-Headers
```

Omit `YDXG_ADAPTER` to try the first suitable hardware adapter.
If C++ or Git is not available on Tumbleweed, one can enter
`nix shell nixpkgs#gcc nixpkgs#git` and run the same commands.

Exit codes: `0` = copy and alias check passed; `3` = driver says existing
heaps are unsupported; `77` = no `/dev/dxg` device; `2` = an observed
runtime, operation, GPU fence, or comparison failure (the stage is
printed). An import rejection or lack of feature support tells us about
this public API route only; it does not disprove internal GPU-PV or
other compatible APIs. This probe was build-checked with stock GCC and
official DirectX-Headers v1.619.1; the cloud test only reached the
expected no-`/dev/dxg` skip, not the GPU path.

Even **PASS is not DRM, PRIME or a display test**. A Linux memfd is not a
DMA-BUF. A follow-up still needs to prove kernel DMA-BUF exporter lifetime
and coherency, DRM/GBM import/export with correct layout and fences, and
Wayland import/presentation. No driver, ioctl, configuration, Mesa,
Weston, or compositor changes are included.

References: [D3D12 existing heaps](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device3-openexistingheapfromaddress),
[WSL DXG guest-sysmem mapping implementation](https://github.com/jihoo12/WSL2-Linux-Kernel/blob/4ee82e228f384c4335f6b2048af72d6956f74c21/drivers/hv/dxgkrnl/dxgvmbus.c#L1659-L1749).
