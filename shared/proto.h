// s2sc wire protocol. Shared verbatim by the device agent and the PC client.
#pragma once
#include <cstdint>

namespace s2sc {

constexpr uint32_t kMagic        = 0x43533253u; // 'S2SC' LE
constexpr uint16_t kProtoVersion = 1;
constexpr uint16_t kDefaultPort  = 31337;

enum Op : uint16_t {
    OpHello    = 1,
    OpListProc = 2,
    OpAttach   = 3,
    OpRegions  = 4,
    OpRead     = 5,
    OpWrite    = 6,
    OpChain    = 7,
    OpPing     = 8,
    OpDetach   = 9,
    // Finds an Il2CppClass by name and every object whose klass pointer is it.
    // Runs on the device: pulling the heap over adb would take minutes.
    OpScanClass = 10,
    // Finds objects by their own fields: a weapon byte and a skin id that is in
    // the catalogue. One pass, no class metadata involved.
    OpScanObjects = 11,
};

#pragma pack(push, 1)

struct MsgHeader {
    uint32_t magic;
    uint16_t op;
    uint16_t flags;
    uint32_t len; // payload length, excluding this header
};

struct HelloReply {
    uint32_t proto;
    uint32_t pid_max;
    uint32_t page_size;
    char     build[64]; // agent build string, informational only
};

struct ProcEntry {
    uint32_t pid;
    char     name[64];
};

struct RegionEntry {
    uint64_t start;
    uint64_t end;
    uint32_t prot;   // kProtRead | kProtWrite | kProtExec
    uint32_t offset; // file offset of the mapping (informational)
    // 128, not 32: APK paths such as
    // /data/app/com.axlebolt.standoff2-.../lib/arm64/libunity.so exceed 32 bytes
    // and the truncated name cannot be matched against a module name.
    char     name[128];
};

struct ChainStep {
    uint64_t base; // only the first step's base is used
    int32_t  off;
    int32_t  pad;
};

struct ChainReply {
    uint64_t addr;
    uint64_t value;
};

struct StatusReply {
    int32_t status; // 0 = ok, otherwise negative errno
};

struct ScanClassReq {
    char     name[64]; // Il2CppClass name, NUL terminated
    uint32_t max_hits; // 0 = agent default
};

struct ScanClassReply {
    int32_t  status;  // 0 = ok, otherwise negative errno
    uint32_t count;   // number of object addresses that follow
    uint64_t klass;   // the resolved Il2CppClass, 0 when not found
};

// Largest id set a single OpScanObjects request may carry.
constexpr uint32_t kMaxScanIds = 8192;

struct ScanObjectsReq {
    uint32_t skin_off;   // offset of the skin id inside the object
    uint32_t weapon_off; // offset of the weapon id byte
    uint32_t mag_off;    // offset of the magazine byte
    uint32_t ammo_off;   // offset of the ammo byte
    uint32_t rest_off;   // offset of the reserve ushort, 0 = absent
    uint32_t id_count;   // number of u32 ids that follow this header
    uint32_t max_hits;
    uint32_t pad;
};

struct ScanObjectsReply {
    int32_t  status;
    uint32_t count; // object addresses follow
};

#pragma pack(pop)

constexpr uint32_t kProtRead  = 1u;
constexpr uint32_t kProtWrite = 2u;
constexpr uint32_t kProtExec  = 4u;

} // namespace s2sc
