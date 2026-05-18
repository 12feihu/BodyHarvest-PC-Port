// Phase 16: multiplayer proof-of-concept.
//
// Symmetric UDP-loopback "see each other" demo. Each running
// instance binds a local UDP port and sends one short packet per
// frame to the peer's port containing its current X/Y/Z position
// and Y rotation. The receive side writes the latest remote pos
// into a pre-allocated "ghost" slot in BH's vehicleInstances[]
// table, so each instance sees the other player as a visible
// vehicle moving in real time through its own copy of the world.
//
// Out of scope for the POC:
//   - Game state sync (missions, AI, terrain damage)
//   - Authoritative host model
//   - Smoothing / dead reckoning
//   - More than 2 players
//   - Configurable peers (hardcoded loopback ports)
//
// Two-instance test recipe:
//   Instance A:  bh_app.exe                    (listen 12345 -> send 12346)
//   Instance B:  bh_app.exe --port 12346 --peer-port 12345
//
// The toggle on the Options tab gates both send and receive; when
// off, this module is a no-op every tick.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

// Defined in bh_cheats.cpp — wrapper around the static
// direct_spawn_alien() that allocates a free alienInstances slot
// and runs the standard template copy + flag init for the given type.
namespace bh::spawn { int alien_simple(uint8_t* rdram, uint8_t alien_type); }

namespace bh::net {

namespace {

constexpr uint32_t kPacketMagic = 0x504D4842;  // "BHMP" little-endian
constexpr uint8_t  kPacketVersion = 1;

#pragma pack(push, 1)
struct Packet {
    uint32_t magic;       // kPacketMagic
    uint8_t  version;     // kPacketVersion
    uint8_t  pad[3];      // alignment
    float    x, y, z;     // player world position (f32 cache values)
    int16_t  rot_y;       // facing direction (s16, BH's 0x10000 = 360°)
    uint16_t seq;         // increments per send; useful for stats
};
#pragma pack(pop)
static_assert(sizeof(Packet) == 24, "Packet must stay tightly packed");

// Configuration captured at init() time.
SOCKET     g_sock = INVALID_SOCKET;
sockaddr_in g_peer_addr{};
uint16_t   g_my_port   = 0;
uint16_t   g_peer_port = 0;

// Runtime flag — toggled from the Options-tab checkbox.
std::atomic<bool> g_enabled{false};

// Latest received remote-player state. Updated by tick()'s recv loop,
// applied to the ghost slot in apply_remote_pos_to_ghost().
std::atomic<float>   g_remote_x{0.0f};
std::atomic<float>   g_remote_y{0.0f};
std::atomic<float>   g_remote_z{0.0f};
std::atomic<int16_t> g_remote_rot{0};
std::atomic<bool>    g_remote_valid{false};
std::atomic<uint64_t> g_packets_sent{0};
std::atomic<uint64_t> g_packets_recv{0};

// Ghost lives in an alienInstances[] slot instead of a vehicle slot
// (per user feedback — vehicle Alpha 1 is tank-shaped, not "second
// player"-shaped). Bad Adam (type 0x12) is the perfect ghost: it's
// humanoid-shaped AND tagged "doesn't move, can't die" so its AI
// won't fight our per-frame position writes the way a normal alien
// or vehicle's AI does. Allocation goes through bh::spawn::alien_simple
// in bh_cheats.cpp which already runs the full free-slot + template
// copy + flag init dance.
constexpr uint32_t kAlienInstancesBase   = 0x80048198;
constexpr uint32_t kAlienInstanceSize    = 0x50;
constexpr uint32_t kVirtBase             = 0x80000000u;
constexpr uint8_t  kGhostAlienType       = 0x12;  // Bad Adam — humanoid, stationary

// Read-helpers (duplicated minimal versions from bh_cheats.cpp — kept
// local so this module has no dependency on the cheats menu).
inline uint8_t read_n64_u8(const uint8_t* rdram, uint32_t virt) {
    return rdram[(virt - kVirtBase) ^ 3u];
}
inline void write_n64_u8(uint8_t* rdram, uint32_t virt, uint8_t v) {
    rdram[(virt - kVirtBase) ^ 3u] = v;
}
inline void write_n64_s16(uint8_t* rdram, uint32_t virt, int16_t v) {
    std::memcpy(rdram + ((virt - kVirtBase) ^ 2u), &v, sizeof(v));
}
inline void write_n64_u16(uint8_t* rdram, uint32_t virt, uint16_t v) {
    std::memcpy(rdram + ((virt - kVirtBase) ^ 2u), &v, sizeof(v));
}
inline uint16_t read_n64_u16(const uint8_t* rdram, uint32_t virt) {
    uint16_t v;
    std::memcpy(&v, rdram + ((virt - kVirtBase) ^ 2u), sizeof(v));
    return v;
}
inline void write_n64_f32(uint8_t* rdram, uint32_t virt, float v) {
    std::memcpy(rdram + (virt - kVirtBase), &v, sizeof(v));
}
inline uint32_t read_n64_u32(const uint8_t* rdram, uint32_t virt) {
    uint32_t v;
    std::memcpy(&v, rdram + (virt - kVirtBase), sizeof(v));
    return v;
}
inline float read_n64_f32(const uint8_t* rdram, uint32_t virt) {
    float v;
    std::memcpy(&v, rdram + (virt - kVirtBase), sizeof(v));
    return v;
}
inline bool ram_ptr_valid(uint32_t virt) {
    return virt >= kVirtBase && virt < 0x80800000u;
}

// Same player-entity pointer the cheats menu uses.
constexpr uint32_t kCurrentEntityPtr     = 0x80052B34u;
constexpr uint32_t kEntityOffsetPosX     = 0x00u;
constexpr uint32_t kEntityOffsetPosY     = 0x02u;
constexpr uint32_t kEntityOffsetPosZ     = 0x04u;
constexpr uint32_t kEntityOffsetDir      = 0x0Eu;
constexpr uint32_t kEntityOffsetCacheX   = 0x4Cu;
constexpr uint32_t kEntityOffsetCacheY   = 0x50u;
constexpr uint32_t kEntityOffsetCacheZ   = 0x54u;

// Ghost-slot tracking. -1 = not yet allocated. Set on first received
// packet to whatever slot bh::spawn::alien_simple gave us.
int g_ghost_slot = -1;

// Validate our cached ghost slot still holds Bad Adam. BH may have
// repopulated alienInstances[] (level reload, save load, despawn
// pass, etc.) — in which case our index now points at a legit
// level alien and our position writes would hijack it. Reset to -1
// so the next ensure_ghost_exists re-spawns into a fresh free slot.
void verify_ghost_slot(uint8_t* rdram) {
    if (g_ghost_slot < 0) return;
    if (rdram == nullptr) return;
    const uint32_t slot_virt = kAlienInstancesBase
                             + uint32_t(g_ghost_slot) * kAlienInstanceSize;
    const uint8_t spec = read_n64_u8(rdram, slot_virt + 0x1A);
    if (spec != kGhostAlienType) {
        std::fprintf(stderr,
            "[net] ghost: slot %d now holds type 0x%02X (expected 0x%02X) — "
            "reallocating on next tick\n",
            g_ghost_slot, unsigned(spec), unsigned(kGhostAlienType));
        g_ghost_slot = -1;
    }
}

// Lazy-allocate the ghost via the existing alien spawner. The
// spawner finds a free alienInstances slot, copies the template,
// sets HP + flags from alienSpecs[type], and increments the active
// counter — exactly the same code path BH uses for legitimate
// alien spawns, so the ghost integrates cleanly into the render list.
void ensure_ghost_exists(uint8_t* rdram) {
    if (g_ghost_slot >= 0) return;
    if (rdram == nullptr) return;
    const int slot = bh::spawn::alien_simple(rdram, kGhostAlienType);
    if (slot < 0) {
        std::fprintf(stderr,
            "[net] ghost: spawner returned -1 (alien table full?) — ghost invisible\n");
        return;
    }
    g_ghost_slot = slot;
    std::fprintf(stderr, "[net] ghost: allocated alien slot %d, type 0x%02X (Bad Adam)\n",
                 g_ghost_slot, unsigned(kGhostAlienType));
}

// Push the latest received remote position into the ghost slot's
// position fields. Aliens use s16 positions only (no f32 cache like
// VehicleInstance has), so writes are simpler than the vehicle ghost.
// AlienInstance offsets:
//   0x00 X, 0x02 Y, 0x04 Z (s16)
//   0x0E Direction (s16)
void apply_remote_pos_to_ghost(uint8_t* rdram) {
    if (g_ghost_slot < 0) return;
    if (!g_remote_valid.load(std::memory_order_acquire)) return;

    const float fx = g_remote_x.load(std::memory_order_acquire);
    const float fy = g_remote_y.load(std::memory_order_acquire);
    const float fz = g_remote_z.load(std::memory_order_acquire);
    const int16_t rot = g_remote_rot.load(std::memory_order_acquire);

    const uint32_t a = kAlienInstancesBase
                     + uint32_t(g_ghost_slot) * kAlienInstanceSize;
    write_n64_s16(rdram, a + kEntityOffsetPosX, int16_t(fx));
    write_n64_s16(rdram, a + kEntityOffsetPosY, int16_t(fy));
    write_n64_s16(rdram, a + kEntityOffsetPosZ, int16_t(fz));
    write_n64_s16(rdram, a + kEntityOffsetDir,  rot);
}

} // namespace

// Initialize Winsock + bind a non-blocking UDP socket on `my_port`.
// All packets get sent to 127.0.0.1:`peer_port`. Returns false on
// any failure (already-bound port, Winsock startup failure, etc.).
bool init(uint16_t my_port, uint16_t peer_port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "[net] WSAStartup failed\n");
        return false;
    }

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) {
        std::fprintf(stderr, "[net] socket() failed: %d\n", WSAGetLastError());
        return false;
    }

    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port        = htons(my_port);
    if (bind(g_sock, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        std::fprintf(stderr,
            "[net] bind(%u) failed: %d "
            "(another bh_app instance probably owns this port)\n",
            unsigned(my_port), WSAGetLastError());
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return false;
    }

    // Non-blocking — we'll drain whatever's available each tick.
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);

    g_peer_addr.sin_family      = AF_INET;
    g_peer_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    g_peer_addr.sin_port        = htons(peer_port);
    g_my_port   = my_port;
    g_peer_port = peer_port;

    std::fprintf(stderr,
        "[net] ready: listening on 127.0.0.1:%u, sending to 127.0.0.1:%u "
        "(toggle from Options tab to start exchanging)\n",
        unsigned(my_port), unsigned(peer_port));
    return true;
}

// Per-frame: send our position, drain incoming packets, apply latest
// remote position to the ghost slot. Cheap; safe to call every tick
// even when disabled (early-outs on the flag).
void tick(uint8_t* rdram) {
    if (!g_enabled.load(std::memory_order_acquire)) return;
    if (g_sock == INVALID_SOCKET) return;
    if (rdram == nullptr) return;

    // ---- Send our own position ----
    const uint32_t entity = read_n64_u32(rdram, kCurrentEntityPtr);
    if (ram_ptr_valid(entity)) {
        Packet pkt{};
        pkt.magic   = kPacketMagic;
        pkt.version = kPacketVersion;
        pkt.x       = read_n64_f32(rdram, entity + kEntityOffsetCacheX);
        pkt.y       = read_n64_f32(rdram, entity + kEntityOffsetCacheY);
        pkt.z       = read_n64_f32(rdram, entity + kEntityOffsetCacheZ);
        // Direction (yaw) — s16 at unk0E.
        uint16_t rot_u;
        std::memcpy(&rot_u,
            rdram + ((entity + kEntityOffsetDir - kVirtBase) ^ 2u),
            sizeof(rot_u));
        pkt.rot_y = int16_t(rot_u);
        pkt.seq   = uint16_t(g_packets_sent.fetch_add(1, std::memory_order_relaxed));
        sendto(g_sock, (const char*)&pkt, sizeof(pkt), 0,
               (sockaddr*)&g_peer_addr, sizeof(g_peer_addr));
    }

    // ---- Drain receive queue ----
    for (int i = 0; i < 16; ++i) {  // cap per-tick drain to avoid starvation
        Packet pkt{};
        sockaddr_in from{};
        int from_len = sizeof(from);
        int n = recvfrom(g_sock, (char*)&pkt, sizeof(pkt), 0,
                         (sockaddr*)&from, &from_len);
        if (n != sizeof(pkt)) break;  // would-block or short read
        if (pkt.magic != kPacketMagic) continue;
        if (pkt.version != kPacketVersion) continue;
        g_remote_x.store(pkt.x, std::memory_order_release);
        g_remote_y.store(pkt.y, std::memory_order_release);
        g_remote_z.store(pkt.z, std::memory_order_release);
        g_remote_rot.store(pkt.rot_y, std::memory_order_release);
        g_remote_valid.store(true, std::memory_order_release);
        g_packets_recv.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- Apply to ghost ----
    // Validate first so a stolen / overwritten slot triggers a
    // re-spawn instead of us continuing to puppet the wrong entity.
    verify_ghost_slot(rdram);
    ensure_ghost_exists(rdram);
    apply_remote_pos_to_ghost(rdram);
}

void set_enabled(bool on) {
    const bool was = g_enabled.exchange(on, std::memory_order_acq_rel);
    if (was != on) {
        std::fprintf(stderr, "[net] multiplayer %s "
            "(sent=%llu received=%llu)\n",
            on ? "ENABLED" : "disabled",
            (unsigned long long)g_packets_sent.load(),
            (unsigned long long)g_packets_recv.load());
    }
}

bool enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

} // namespace bh::net
