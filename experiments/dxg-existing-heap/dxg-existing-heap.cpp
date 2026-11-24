// SPDX-License-Identifier: MIT
#include <wsl/winadapter.h>
#include <directx/d3d12.h>
#include <directx/dxcore.h>
#include <dxguids/dxguids.h>
#include <wrl/client.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using Microsoft::WRL::ComPtr;
namespace {
constexpr size_t bytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

struct Mapping {
    void *data = MAP_FAILED;
    size_t size = bytes;
    ~Mapping() { if (data != MAP_FAILED) munmap(data, size); }
};

struct Fd {
    int fd = -1;
    ~Fd() { if (fd >= 0) close(fd); }
};

struct Library {
    void *handle;
    explicit Library(const char *name) : handle(dlopen(name, RTLD_NOW | RTLD_LOCAL)) {}
    ~Library() { if (handle) dlclose(handle); }
};

D3D12_RESOURCE_DESC buffer_desc() {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

int fail(const char *stage, HRESULT hr) {
    std::fprintf(stderr, "%s failed (HRESULT 0x%08x)\n", stage, static_cast<unsigned>(hr));
    return 2;
}

unsigned char pattern(size_t i) {
    return static_cast<unsigned char>((i ^ (i >> 8) ^ (i >> 12)) * 17 + 61);
}
}

int main() {
    struct stat dxg{};
    if (stat("/dev/dxg", &dxg) || !S_ISCHR(dxg.st_mode)) {
        std::fputs("SKIP: no /dev/dxg character device\n", stderr);
        return 77;
    }

    Library dxcore("libdxcore.so");
    Library d3d12("libd3d12.so");
    if (!dxcore.handle || !d3d12.handle) {
        std::fputs("WSL DXCore/D3D12 libraries not loadable; check /usr/lib/wsl/lib\n", stderr);
        return 2;
    }

    auto make_factory = reinterpret_cast<HRESULT (WINAPI *)(REFIID, void **)>(
        dlsym(dxcore.handle, "DXCoreCreateAdapterFactory"));
    auto make_device = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
        dlsym(d3d12.handle, "D3D12CreateDevice"));
    if (!make_factory || !make_device) {
        std::fputs("DXCoreCreateAdapterFactory or D3D12CreateDevice missing\n", stderr);
        return 2;
    }

    ComPtr<IDXCoreAdapterFactory> factory;
    HRESULT hr = make_factory(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return fail("DXCoreCreateAdapterFactory", hr);
    const GUID graphics[] = {DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS};
    ComPtr<IDXCoreAdapterList> adapters;
    hr = factory->CreateAdapterList(1, graphics, IID_PPV_ARGS(&adapters));
    if (FAILED(hr)) return fail("CreateAdapterList", hr);

    ComPtr<ID3D12Device3> device;
    for (uint32_t i = 0; i < adapters->GetAdapterCount(); ++i) {
        ComPtr<IDXCoreAdapter> adapter;
        bool hardware = false;
        if (FAILED(adapters->GetAdapter(i, IID_PPV_ARGS(&adapter))) ||
            FAILED(adapter->GetProperty(DXCoreAdapterProperty::IsHardware, sizeof(hardware), &hardware)) ||
            !hardware)
            continue;
        char name[1024]{};
        size_t description_size = 0;
        if (SUCCEEDED(adapter->GetPropertySize(DXCoreAdapterProperty::DriverDescription,
                &description_size)) && description_size > 0 && description_size < sizeof(name))
            adapter->GetProperty(DXCoreAdapterProperty::DriverDescription, description_size, name);
        // Match the known test adapter if one was explicitly requested.
        const char *wanted = std::getenv("YDXG_ADAPTER");
        if (wanted && !std::strstr(name, wanted))
            continue;
        hr = make_device(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr)) {
            std::printf("Hardware adapter: %s\n", *name ? name : "<unnamed>");
            break;
        }
        fail("D3D12CreateDevice(ID3D12Device3)", hr);
    }
    if (!device) {
        std::fputs("No matching hardware D3D12 Device3\n", stderr);
        return 2;
    }

    D3D12_FEATURE_DATA_EXISTING_HEAPS existing{};
    hr = device->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &existing, sizeof(existing));
    if (FAILED(hr)) return fail("CheckFeatureSupport(EXISTING_HEAPS)", hr);
    std::printf("D3D12_FEATURE_EXISTING_HEAPS: %s\n", existing.Supported ? "yes" : "no");
    if (!existing.Supported) return 3; // Unsupported test path, not a test pass.

    Fd backing;
    backing.fd = memfd_create("dxg-existing-heap-probe", MFD_CLOEXEC);
    if (backing.fd < 0 || ftruncate(backing.fd, bytes)) {
        perror("memfd_create/ftruncate");
        return 2;
    }
    // Reserve first; MAP_FIXED replaces only our own inaccessible reservation.
    Mapping reservation{mmap(nullptr, bytes * 2, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), bytes * 2};
    if (reservation.data == MAP_FAILED) { perror("reserve"); return 2; }
    const auto addr = reinterpret_cast<uintptr_t>(reservation.data);
    auto *aligned = reinterpret_cast<void *>((addr + bytes - 1) & ~(bytes - 1));
    if (mmap(aligned, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
            backing.fd, 0) == MAP_FAILED) {
        perror("mmap existing heap");
        return 2;
    }
    Mapping alias{mmap(nullptr, bytes, PROT_READ, MAP_SHARED, backing.fd, 0), bytes};
    if (alias.data == MAP_FAILED) { perror("mmap observer"); return 2; }
    std::memset(aligned, 0, bytes);
    if (static_cast<const unsigned char *>(alias.data)[0] != 0) {
        std::fputs("Independent guest mapping baseline did not match backing\n", stderr);
        return 2;
    }
    // Declaration order keeps the input mapping valid until after the GPU
    // resources wrapping it have been released.
    ComPtr<ID3D12Heap> heap;
    hr = device->OpenExistingHeapFromAddress(aligned, IID_PPV_ARGS(&heap));
    if (FAILED(hr)) return fail("OpenExistingHeapFromAddress(memfd)", hr);
    std::puts("GPU runtime accepted guest-backed heap");

    const auto desc = buffer_desc();
    ComPtr<ID3D12Resource> target, upload;
    hr = device->CreatePlacedResource(heap.Get(), 0, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&target));
    if (FAILED(hr)) return fail("CreatePlacedResource(COPY_DEST)", hr);

    D3D12_HEAP_PROPERTIES upload_props{};
    upload_props.Type = D3D12_HEAP_TYPE_UPLOAD;
    upload_props.CreationNodeMask = upload_props.VisibleNodeMask = 1;
    hr = device->CreateCommittedResource(&upload_props, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    if (FAILED(hr)) return fail("CreateCommittedResource(UPLOAD)", hr);
    const D3D12_RANGE empty{0, 0}, full{0, bytes};
    void *upload_mem = nullptr;
    hr = upload->Map(0, &empty, &upload_mem);
    if (FAILED(hr)) return fail("Map(upload)", hr);
    for (size_t i = 0; i < bytes; ++i)
        static_cast<unsigned char *>(upload_mem)[i] = pattern(i);
    upload->Unmap(0, &full);

    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) return fail("CreateCommandQueue", hr);
    ComPtr<ID3D12CommandAllocator> allocator;
    hr = device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator));
    if (FAILED(hr)) return fail("CreateCommandAllocator", hr);
    ComPtr<ID3D12GraphicsCommandList> cmd;
    hr = device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&cmd));
    if (FAILED(hr)) return fail("CreateCommandList", hr);
    ComPtr<ID3D12Fence> fence;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) return fail("CreateFence", hr);

    cmd->CopyBufferRegion(target.Get(), 0, upload.Get(), 0, bytes);
    hr = cmd->Close();
    if (FAILED(hr)) return fail("Close command list", hr);
    ID3D12CommandList *lists[]{cmd.Get()};
    // Drain progress before a possible _Exit on a device error.
    std::fflush(stdout);
    queue->ExecuteCommandLists(1, lists);
    hr = queue->Signal(fence.Get(), 1);
    if (FAILED(hr)) std::_Exit(fail("Signal after submit", hr));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        const auto value = fence->GetCompletedValue();
        if (value == UINT64_MAX) std::_Exit(fail("Device removed after submit", device->GetDeviceRemovedReason()));
        if (value >= 1) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fputs("Fence timed out; terminating probe without unmapping an in-flight resource\n", stderr);
            std::_Exit(2);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto *observed = static_cast<const unsigned char *>(alias.data);
    for (size_t i = 0; i < bytes; ++i) {
        if (observed[i] != pattern(i)) {
            std::fprintf(stderr, "GPU copy not visible in independent mapping at byte %zu\n", i);
            return 2;
        }
    }
    std::puts("PASS: D3D12 GPU copy/fence into 64 KiB memfd-backed guest mapping, verified through independent mmap");
    return 0;
}
