// SPDX-License-Identifier: MIT
#include <wsl/winadapter.h>
#include <directx/d3d12.h>
#include <directx/dxcore.h>
#include <dxguids/dxguids.h>
#include <wrl/client.h>

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <vector>
#include <unistd.h>

using Microsoft::WRL::ComPtr;
namespace {
constexpr size_t bytes = 64 * 1024;
constexpr unsigned width = 71, height = 43;
constexpr auto texture_format = DXGI_FORMAT_R8G8B8A8_UNORM;
struct ClearRegion {
    D3D12_RECT rect;
    unsigned color;
};
constexpr ClearRegion regions[] = {
    {{3, 4, 32, 22}, 1},
    {{24, 13, 57, 36}, 2},
    {{51, 1, 69, 9}, 4},
    {{5, 29, 13, 42}, 7},
};

unsigned region_color(unsigned color, uint32_t seed) {
    return color ^ (seed & 15u);
}

void rgba_bytes(unsigned color, unsigned char (&rgba)[4]) {
    for (unsigned k = 0; k < 4; ++k)
        rgba[k] = (color & (1u << k)) ? 255 : 0;
}

bool texture_matches(const unsigned char *pixels, size_t pitch, uint32_t seed,
                     bool report = true) {
    if (pitch < width * 4) return false;
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            unsigned color = 8;
            for (const auto &r : regions)
                if (x >= static_cast<unsigned>(r.rect.left) && x < static_cast<unsigned>(r.rect.right) &&
                    y >= static_cast<unsigned>(r.rect.top) && y < static_cast<unsigned>(r.rect.bottom))
                    color = r.color;
            unsigned char rgba[4];
            rgba_bytes(region_color(color, seed), rgba);
            const auto *got = pixels + y * pitch + x * 4;
            if (std::memcmp(got, rgba, sizeof(rgba)) != 0) {
                if (report)
                    std::fprintf(stderr, "Texture mismatch at (%u,%u): expected %u,%u,%u,%u; got %u,%u,%u,%u\n",
                        x, y, rgba[0], rgba[1], rgba[2], rgba[3], got[0], got[1], got[2], got[3]);
                return false;
            }
        }
    }
    return true;
}

bool test_texture_verifier() {
    constexpr size_t pitch = 512;
    constexpr uint32_t seed = 0;
    std::vector<unsigned char> data(height * pitch, 0xca);
    // Independent fixture construction: paint literal RGBA bytes, including
    // the overlap and deliberately opaque background.
    auto paint = [&](unsigned left, unsigned top, unsigned right, unsigned bottom,
                     unsigned char red, unsigned char green, unsigned char blue, unsigned char alpha) {
        const unsigned char rgba[]{red, green, blue, alpha};
        for (unsigned y = top; y < bottom; ++y)
            for (unsigned x = left; x < right; ++x)
                std::memcpy(data.data() + y * pitch + x * 4, rgba, sizeof(rgba));
    };
    paint(0, 0, 71, 43, 0, 0, 0, 255);
    paint(3, 4, 32, 22, 255, 0, 0, 0);
    paint(24, 13, 57, 36, 0, 255, 0, 0);
    paint(51, 1, 69, 9, 0, 0, 255, 0);
    paint(5, 29, 13, 42, 255, 255, 255, 0);
    if (!texture_matches(data.data(), pitch, seed) ||
        texture_matches(data.data(), width * 4, seed, false) ||
        texture_matches(data.data(), pitch, seed + 1, false))
        return false;
    std::vector<unsigned char> flipped(height * pitch);
    for (unsigned y = 0; y < height; ++y)
        std::memcpy(flipped.data() + y * pitch, data.data() + (height - 1 - y) * pitch, pitch);
    if (texture_matches(flipped.data(), pitch, seed, false)) return false;
    const auto idx = 4 * pitch + 3 * 4;
    data[idx] = 0;
    data[idx + 2] = 255;
    return !texture_matches(data.data(), pitch, seed, false);
}
struct Fd {
    int fd = -1;
    Fd() = default;
    explicit Fd(int f) : fd(f) {}
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    ~Fd() { reset(); }
    void reset() { if (fd >= 0) close(fd); fd = -1; }
};

struct Packet {
    uint32_t low = 0;
    int32_t high = 0;
    uint32_t seed = 0;
    uint32_t texture = 0;
};

bool transfer(int sock, Packet &payload, Fd &fd, bool send) {
    alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int))]{};
    iovec iov{&payload, sizeof(payload)};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);
    if (send) {
        auto *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &fd.fd, sizeof(fd.fd));
        return sendmsg(sock, &msg, MSG_NOSIGNAL) == sizeof(payload);
    }
    const auto n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    if (n < 0) return false;
    // Even on a malformed message, close any actually delivered descriptors.
    bool valid = n == sizeof(payload) && !(msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
    size_t count = 0;
    for (auto *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
            c->cmsg_len < CMSG_LEN(0)) { valid = false; continue; }
        const size_t fds = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < fds; ++i) {
            int received;
            std::memcpy(&received, CMSG_DATA(c) + i * sizeof(received), sizeof(received));
            if (++count == 1) fd.fd = received;
            else close(received);
        }
    }
    return valid && count == 1 && fd.fd >= 0;
}

bool ok(const char *where, HRESULT hr) {
    if (SUCCEEDED(hr)) return true;
    std::fprintf(stderr, "%s failed (HRESULT 0x%08x)\n", where, static_cast<unsigned>(hr));
    return false;
}

unsigned char pattern(size_t i, uint32_t seed) {
    uint32_t v = static_cast<uint32_t>(i) ^ seed;
    v ^= v >> 16;
    v *= UINT32_C(0x7feb352d);
    v ^= v >> 15;
    return static_cast<unsigned char>(v ^ (v >> 8) ^ (v >> 24));
}

struct Runtime {
    void *dxcore = nullptr, *d3d12 = nullptr;
    ~Runtime() { if (d3d12) dlclose(d3d12); if (dxcore) dlclose(dxcore); }
    bool load() {
        dxcore = dlopen("libdxcore.so", RTLD_NOW | RTLD_LOCAL);
        d3d12 = dlopen("libd3d12.so", RTLD_NOW | RTLD_LOCAL);
        if (!dxcore || !d3d12)
            std::fputs("Unable to load WSL libdxcore.so/libd3d12.so (check /usr/lib/wsl/lib)\n", stderr);
        return dxcore && d3d12;
    }
};

bool make_device(Runtime &r, Packet &packet, bool consumer, ComPtr<ID3D12Device> &dev) {
    auto factory_fn = reinterpret_cast<HRESULT (WINAPI *)(REFIID, void **)>(
        dlsym(r.dxcore, "DXCoreCreateAdapterFactory"));
    auto device_fn = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(dlsym(r.d3d12, "D3D12CreateDevice"));
    if (!factory_fn || !device_fn) {
        std::fputs("WSL DXCore/D3D12 entry points not available\n", stderr);
        return false;
    }
    ComPtr<IDXCoreAdapterFactory> factory;
    if (!ok("DXCoreCreateAdapterFactory", factory_fn(IID_PPV_ARGS(&factory)))) return false;
    const GUID filters[] = {DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS};
    ComPtr<IDXCoreAdapterList> adapters;
    if (!ok("CreateAdapterList", factory->CreateAdapterList(1, filters, IID_PPV_ARGS(&adapters))))
        return false;

    for (uint32_t i = 0; i < adapters->GetAdapterCount(); ++i) {
        ComPtr<IDXCoreAdapter> adapter;
        bool hw = false;
        LUID luid{};
        if (FAILED(adapters->GetAdapter(i, IID_PPV_ARGS(&adapter))) ||
            FAILED(adapter->GetProperty(DXCoreAdapterProperty::IsHardware, sizeof(hw), &hw)) || !hw ||
            FAILED(adapter->GetProperty(DXCoreAdapterProperty::InstanceLuid, sizeof(luid), &luid)))
            continue;
        if (consumer && (luid.LowPart != packet.low || luid.HighPart != packet.high)) continue;
        char name[1024]{};
        size_t length = 0;
        if (SUCCEEDED(adapter->GetPropertySize(DXCoreAdapterProperty::DriverDescription, &length)) &&
            length && length < sizeof(name))
            adapter->GetProperty(DXCoreAdapterProperty::DriverDescription, length, name);
        if (!consumer) {
            const char *want = std::getenv("YDXG_ADAPTER");
            if (want && !std::strstr(name, want)) continue;
        }
        if (!ok("D3D12CreateDevice", device_fn(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))))
            continue;
        packet.low = luid.LowPart;
        packet.high = luid.HighPart;
        std::printf("%s PID %ld, hardware: %s, LUID %08x:%08x\n", consumer ? "Importer" : "Producer",
            static_cast<long>(getpid()), *name ? name : "<unnamed>",
            static_cast<unsigned>(packet.high), static_cast<unsigned>(packet.low));
        return true;
    }
    std::fputs("No matching hardware D3D12 adapter\n", stderr);
    return false;
}

bool make_buffer(ID3D12Device &dev, D3D12_HEAP_TYPE type, D3D12_HEAP_FLAGS flags,
                 D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource> &resource,
                 UINT64 length = bytes) {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = type;
    props.CreationNodeMask = props.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = length;
    d.Height = d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return ok("CreateCommittedResource", dev.CreateCommittedResource(&props, flags, &d, state,
                                 nullptr, IID_PPV_ARGS(&resource)));
}

enum class Operation { WriteBuffer, ReadBuffer, PaintTexture, ReadTexture };

bool execute_and_wait(ID3D12Device &dev, ID3D12Resource &shared, ID3D12Resource *staging,
                      Operation operation, uint32_t seed,
                      const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp = nullptr) {
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    if (!ok("CreateCommandQueue", dev.CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))) ||
        !ok("CreateCommandAllocator", dev.CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&alloc))) ||
        !ok("CreateCommandList", dev.CreateCommandList(0, desc.Type, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd))) ||
        !ok("CreateFence", dev.CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = &shared;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    const bool paint = operation == Operation::PaintTexture;
    const bool read = operation == Operation::ReadBuffer || operation == Operation::ReadTexture;
    barrier.Transition.StateAfter = paint ? D3D12_RESOURCE_STATE_RENDER_TARGET :
        read ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
    ComPtr<ID3D12DescriptorHeap> rtv;
    cmd->ResourceBarrier(1, &barrier);
    if (paint) {
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 1;
        if (!ok("CreateDescriptorHeap(RTV)", dev.CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv))))
            return false;
        const auto target = rtv->GetCPUDescriptorHandleForHeapStart();
        dev.CreateRenderTargetView(&shared, nullptr, target);
        auto clear = [&](unsigned idx, const D3D12_RECT *rect) {
            const unsigned color = region_color(idx, seed);
            const FLOAT rgba[] = {FLOAT(bool(color & 1)), FLOAT(bool(color & 2)),
                                  FLOAT(bool(color & 4)), FLOAT(bool(color & 8))};
            cmd->ClearRenderTargetView(target, rgba, rect ? 1 : 0, rect);
        };
        clear(8, nullptr);
        for (const auto &r : regions) clear(r.color, &r.rect);
    } else if (operation == Operation::ReadTexture) {
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = &shared;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = staging;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = *fp;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    } else {
        cmd->CopyBufferRegion(read ? staging : &shared, 0, read ? &shared : staging, 0, bytes);
    }
    const auto transfer_state = barrier.Transition.StateAfter;
    barrier.Transition.StateAfter = barrier.Transition.StateBefore;
    barrier.Transition.StateBefore = transfer_state;
    cmd->ResourceBarrier(1, &barrier);
    if (!ok("Command list close", cmd->Close())) return false;
    ID3D12CommandList *lists[]{cmd.Get()};
    std::fflush(stdout);
    queue->ExecuteCommandLists(1, lists);
    if (!ok("Queue signal", queue->Signal(fence.Get(), 1))) std::_Exit(2);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        const auto value = fence->GetCompletedValue();
        if (value == UINT64_MAX) {
            ok("Device removed", dev.GetDeviceRemovedReason());
            std::_Exit(2);
        }
        if (value >= 1) return true;
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fputs("GPU fence timeout\n", stderr);
            std::_Exit(2);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

D3D12_RESOURCE_DESC texture_desc() {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
    d.Format = texture_format;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return d;
}

bool make_texture(ID3D12Device &dev, ComPtr<ID3D12Resource> &resource) {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    props.CreationNodeMask = props.VisibleNodeMask = 1;
    const auto d = texture_desc();
    return ok("CreateCommittedResource(shared GPU render-target texture)",
        dev.CreateCommittedResource(&props, D3D12_HEAP_FLAG_SHARED, &d,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)));
}

bool produce(int sock, ComPtr<ID3D12Resource> &shared, Runtime &r, ComPtr<ID3D12Device> &dev,
             bool texture) {
    Packet packet{};
    packet.texture = texture;
    if (getrandom(&packet.seed, sizeof(packet.seed), 0) != sizeof(packet.seed)) {
        perror("getrandom");
        return false;
    }
    if (!make_device(r, packet, false, dev)) return false;
    if (texture) {
        if (!make_texture(*dev.Get(), shared) ||
            !execute_and_wait(*dev.Get(), *shared.Get(), nullptr, Operation::PaintTexture, packet.seed))
            return false;
        std::printf("Producer GPU rendered %ux%u RGBA8 via RTV clears (no CPU upload)\n", width, height);
    } else {
        ComPtr<ID3D12Resource> upload;
        if (!make_buffer(*dev.Get(), D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_SHARED,
                D3D12_RESOURCE_STATE_COMMON, shared) ||
            !make_buffer(*dev.Get(), D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_FLAG_NONE,
                D3D12_RESOURCE_STATE_GENERIC_READ, upload)) return false;

        const D3D12_RANGE none{0, 0}, full{0, bytes};
        void *mapping = nullptr;
        if (!ok("Upload map", upload->Map(0, &none, &mapping))) return false;
        for (size_t i = 0; i < bytes; ++i)
            static_cast<unsigned char *>(mapping)[i] = pattern(i, packet.seed);
        upload->Unmap(0, &full);
        if (!execute_and_wait(*dev.Get(), *shared.Get(), upload.Get(), Operation::WriteBuffer, packet.seed))
            return false;
    }

    HANDLE handle{};
    if (!ok("CreateSharedHandle", dev->CreateSharedHandle(shared.Get(), nullptr, GENERIC_ALL, nullptr, &handle)))
        return false;
    // Linux Mesa's D3D12 backend uses the same HANDLE -> intptr_t -> fd mapping.
    const auto raw = reinterpret_cast<intptr_t>(handle);
    if (raw < 3 || raw > INT_MAX || fcntl(static_cast<int>(raw), F_GETFD) < 0) {
        std::fputs("CreateSharedHandle did not return a valid Linux FD\n", stderr);
        return false;
    }
    Fd fd(static_cast<int>(raw));
    char fd_path[64]{}, fd_type[160]{};
    std::snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd.fd);
    if (readlink(fd_path, fd_type, sizeof(fd_type) - 1) >= 0)
        std::printf("D3D12 export descriptor: %s\n", fd_type);
    std::fflush(stdout);
    if (!transfer(sock, packet, fd, true)) { perror("sendmsg(SCM_RIGHTS)"); return false; }
    return true;
}

int consume(int sock, bool fixture) {
    Packet packet{};
    Fd fd;
    if (!transfer(sock, packet, fd, false)) {
        std::fputs("Importer did not receive exactly one FD and metadata\n", stderr);
        return 2;
    }
    if (fixture) {
        char x{};
        if (read(fd.fd, &x, 1) != 1 || x != 'X' || packet.seed != 0x415)
            return 2;
        std::puts("PASS: independent exec process and SCM_RIGHTS fixture (not GPU)");
        return 0;
    }
    Runtime r;
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12Resource> shared, readback;
    if (packet.texture > 1 || !r.load() || !make_device(r, packet, true, dev) ||
        !ok("OpenSharedHandle in importer", dev->OpenSharedHandle(
           reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd.fd)), IID_PPV_ARGS(&shared)))) return 2;
    if (packet.texture) {
        const auto d = shared->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            d.Width != width || d.Height != height || d.Format != texture_format ||
            d.DepthOrArraySize != 1 || d.MipLevels != 1 || d.SampleDesc.Count != 1 ||
            !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
            std::fputs("Imported resource does not match expected RGBA8 render target\n", stderr);
            return 2;
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows = 0;
        UINT64 row_size = 0, size = 0;
        dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &row_size, &size);
        if (!size || size > 1024 * 1024 || fp.Offset >= size || rows != height ||
            row_size != width * 4 || fp.Footprint.RowPitch < row_size ||
            fp.Footprint.RowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT ||
            fp.Offset + (height - 1) * UINT64(fp.Footprint.RowPitch) + row_size > size) {
            std::fputs("Unexpected readback footprint\n", stderr);
            return 2;
        }
        if (!make_buffer(*dev.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_HEAP_FLAG_NONE,
                D3D12_RESOURCE_STATE_COPY_DEST, readback, size) ||
            !execute_and_wait(*dev.Get(), *shared.Get(), readback.Get(),
                Operation::ReadTexture, packet.seed, &fp)) return 2;
        const D3D12_RANGE full{size_t(fp.Offset), size_t(size)}, none{0, 0};
        void *mapping = nullptr;
        if (!ok("Texture readback map", readback->Map(0, &full, &mapping))) return 2;
        const auto *pixels = static_cast<const unsigned char *>(mapping) + fp.Offset;
        std::printf("Importer copied %ux%u RGBA8, row pitch %u\n", width, height, fp.Footprint.RowPitch);
        const bool same = texture_matches(pixels, fp.Footprint.RowPitch, packet.seed);
        readback->Unmap(0, &none);
        if (!same) return 2;
        std::puts("PASS: independent process imported the GPU-rendered D3D12 texture; exact asymmetric RGBA pixels match (not DMA-BUF)");
        return 0;
    }
    if (!make_buffer(*dev.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_HEAP_FLAG_NONE,
            D3D12_RESOURCE_STATE_COPY_DEST, readback) ||
        !execute_and_wait(*dev.Get(), *shared.Get(), readback.Get(), Operation::ReadBuffer, packet.seed))
        return 2;
    const D3D12_RANGE full{0, bytes}, none{0, 0};
    void *mapping = nullptr;
    if (!ok("Readback map", readback->Map(0, &full, &mapping))) return 2;
    bool same = true;
    for (size_t i = 0; i < bytes; ++i) {
        if (static_cast<const unsigned char *>(mapping)[i] != pattern(i, packet.seed)) {
            std::fprintf(stderr, "Importer readback mismatch at byte %zu\n", i);
            same = false;
            break;
        }
    }
    readback->Unmap(0, &none);
    if (!same) return 2;
    std::puts("PASS: separate WSL process opened DXG resource and verified D3D12 GPU bytes after both fences (not DMA-BUF)");
    return 0;
}
}

int main(int argc, char **argv) {
    if (argc == 3 && (!std::strcmp(argv[1], "--receive") || !std::strcmp(argv[1], "--receive-fixture"))) {
        char *end = nullptr;
        const long f = std::strtol(argv[2], &end, 10);
        if (!*argv[2] || *end || f < 3 || f > INT_MAX) return 64;
        Fd socket(static_cast<int>(f));
        return consume(socket.fd, !std::strcmp(argv[1], "--receive-fixture"));
    }

    if (argc == 2 && !std::strcmp(argv[1], "--test-texture")) {
        if (!test_texture_verifier()) return 2;
        std::puts("PASS: padded RGBA texture validator rejects wrong pitch, seed, vertical flip and channel swap (not GPU)");
        return 0;
    }
    const bool fixture = argc == 2 && !std::strcmp(argv[1], "--test-ipc");
    const bool texture = argc == 2 && !std::strcmp(argv[1], "--texture");
    if (argc != 1 && !fixture && !texture) {
        std::fprintf(stderr, "usage: %s [--texture|--test-texture|--test-ipc]\n", argv[0]);
        return 64;
    }
    struct stat st{};
    if (!fixture && (stat("/dev/dxg", &st) || !S_ISCHR(st.st_mode))) {
        std::fputs("SKIP: no /dev/dxg character device\n", stderr);
        return 77;
    }

    int pair[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) < 0) { perror("socketpair"); return 2; }
    Fd to_child(pair[0]), to_parent(pair[1]);
    timeval limit{30, 0};
    if (setsockopt(to_parent.fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit)) < 0) {
        perror("setsockopt(SO_RCVTIMEO)");
        return 2;
    }
    char number[32]{};
    std::snprintf(number, sizeof(number), "%d", to_parent.fd);
    const pid_t child = fork();
    if (child < 0) { perror("fork"); return 2; }
    if (child == 0) {
        // Exec before any DXCore/D3D runtime is loaded; inherit only our socket.
        if (fcntl(to_parent.fd, F_SETFD, 0) < 0) _exit(2);
        execl("/proc/self/exe", argv[0], fixture ? "--receive-fixture" : "--receive", number, nullptr);
        _exit(2);
    }
    to_parent.reset();

    Runtime r;
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12Resource> shared;
    bool sent = false;
    if (fixture) {
        int stream[2];
        if (pipe2(stream, O_CLOEXEC) == 0) {
            Fd input(stream[0]), output(stream[1]);
            Packet packet{0, 0, 0x415};
            sent = write(output.fd, "X", 1) == 1 && transfer(to_child.fd, packet, input, true);
        }
    } else if (r.load()) {
        sent = produce(to_child.fd, shared, r, dev, texture);
    }
    to_child.reset();
    // Keep the source device and resource alive until the importer exits.
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    return sent && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 2;
}
