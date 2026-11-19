// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zstd_errors.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/logging.h"
#include "core/arm/dynarmic/host_routines.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"

namespace Core::HostRoutines {
namespace {

// ------------------------------------------------------------------ sites

constexpr u64 OotProgramId = 0x0400486024DEC000ULL;
// main E0F3CCFA... (2.0.0, v131072), the build the offsets below were read from.
constexpr std::array<u8, 20> OotBuildId{0xE0, 0xF3, 0xCC, 0xFA, 0xF5, 0x00, 0x65, 0xAE, 0x4E, 0xA0,
                                        0x1E, 0xB5, 0xAE, 0xB1, 0x3B, 0x1A, 0x45, 0xDA, 0x33, 0x48};
// main is loaded right after rtld's 0x20000 bytes (the process's entry point is rtld's).
constexpr u64 MainOffset = 0x20000;

enum class Routine : u32 {
    ZstdFrames = 0,      ///< whole-buffer decode (the resource loaders, ARes_Loading threads)
    ZstdStreamEntry = 1, ///< the stream decode wrapper's entry (ParallelDecompThread)
    ZstdStreamLoop = 2,  ///< the same function's decode loop, after its own frame set-up
    Count = 3,
};

/// One word the hook needs to see in the game's code, relative to the site.
struct Check {
    s32 offset;
    u32 word;
};

struct Site {
    Routine routine;
    u64 main_offset;
    /// checks[0] is the word at the site itself (the one the BRK stands for).
    std::array<Check, 16> checks;
};

constexpr std::array<Check, 16> WordsFrom(const std::array<u32, 16>& words) {
    std::array<Check, 16> checks{};
    for (size_t i = 0; i < words.size(); ++i) {
        checks[i] = Check{static_cast<s32>(i * 4), words[i]};
    }
    return checks;
}

constexpr u32 Pacibsp = 0xD503237F;   // a hint: a no-op in the JIT, so a declined entry skips it
constexpr u32 CmpW23One = 0x710006FF; // cmp w23, #1 (declining the loop site sets its flags)

// The stream function (main+0xF5C80): x0 the stream object, x1/w2 one chunk of input, x3 the
// game's DDict or 0; returns (w0) the bytes still to come. Its prologue saves x19..x30 and keeps
// x19 = the object, x20 = the chunk, w23 = its size; a new frame (state 1) runs the game's own
// set-up (header, the context in the object's workspace, the dictionary, x22 = the DDict) and
// branches to the loop at +0x50; a frame under way loads x21 = the context at +0x4C and runs into
// it. The loop's result goes back through the epilogue at +0x404 with w0 set (the game's own
// error paths branch there too).
constexpr u64 StreamFunction = 0xF5C80;
constexpr u64 StreamLoop = 0xF5CD0;
constexpr u64 StreamEpilogue = 0xF6084;

constexpr std::array<Site, 3> OotSites{
    // zstd's decompressMultiFrame inlined: x0 the game's context, x1/x2 the output and its room,
    // x3/x4 the frames, x5/x6 a raw dictionary, x7 the game's DDict or 0. Its three callers set
    // up a static context in place right before each call and read only the result.
    Site{Routine::ZstdFrames, 0xEB380,
         WordsFrom({0xD503237F, 0xA9BA7BFD, 0xA9016FFC, 0xA90267FA, 0x910003FD, 0xA9035FF8,
                    0xA90457F6, 0xA9054FF4, 0xD11A43FF, 0xF9005BE2, 0xF90023E1, 0xAA0003EE,
                    0xF90037E7, 0xB5024E27, 0xF100149F, 0x5401EBC3})},
    Site{Routine::ZstdStreamEntry, StreamFunction,
         WordsFrom({0xD503237F, 0xD10243FF, 0xA9037BFD, 0xA9046FFC, 0xA90567FA, 0xA9065FF8,
                    0xA90757F6, 0xA9084FF4, 0x9100C3FD, 0x39400008, 0x2A0203F7, 0xAA0103F4,
                    0xAA0003F3, 0x7100051F, 0x54001F60, 0x71000D1F})},
    Site{Routine::ZstdStreamLoop, StreamLoop,
         {{
             {0x0, CmpW23One},    // cmp w23, #1
             {0x4, 0x54001D2B},   // b.lt +0x3A8 (no input: the bytes still to come)
             {-0x4, 0xF9401A75},  // ldr x21, [x19, #0x30] (a frame under way)
             {0x518, 0x17FFFEBA}, // b StreamLoop (the end of the game's frame set-up)
             // The epilogue at StreamEpilogue: ldp x20, x19 / x22, x21 / x24, x23 / x26, x25 /
             // x28, x27 / x29, x30 from [sp, #0x80..#0x30]; add sp, sp, #0x90; retab.
             {0x3B4, 0xA9484FF4},
             {0x3B8, 0xA94757F6},
             {0x3BC, 0xA9465FF8},
             {0x3C0, 0xA94567FA},
             {0x3C4, 0xA9446FFC},
             {0x3C8, 0xA9437BFD},
             {0x3CC, 0x910243FF},
             {0x3D0, 0xD65F0FFF},
         }}},
};

constexpr u32 BrkBase = 0xD4200000;
constexpr u32 BrkImmBase = 0xF7A0; // BRK #0xF7A0 + routine

constexpr u32 BrkFor(Routine routine) {
    return BrkBase | ((BrkImmBase + static_cast<u32>(routine)) << 5);
}

/// FRITCHII_HOST_ZSTD (boot): bit 0 = whole-buffer decodes, bit 1 = stream decodes, bit 2 =
/// stream decodes in verify mode (the host decodes a shadow copy and compares; the game's code
/// does the real work and writes everything).
u32 ZstdMode() {
    static const u32 mode = [] {
        const char* text = std::getenv("FRITCHII_HOST_ZSTD");
        const u32 value = text != nullptr ? static_cast<u32>(std::strtoul(text, nullptr, 10)) & 7 : 0;
        if (value != 0) {
            LOG_INFO(Core_ARM,
                     "FRITCHII_HOST_ZSTD={}: whole-buffer decodes {}, stream decodes {} (the "
                     "host's libzstd {})",
                     value, (value & 1) != 0 ? "on the host" : "as the game's code",
                     (value & 4) != 0   ? "verified (shadow decode, the game's code writes)"
                     : (value & 2) != 0 ? "on the host"
                                        : "as the game's code",
                     ZSTD_versionString());
        }
        return value;
    }();
    return mode;
}

bool StreamVerify() {
    return (ZstdMode() & 4) != 0;
}

bool RoutineOn(Routine routine) {
    switch (routine) {
    case Routine::ZstdFrames:
        return (ZstdMode() & 1) != 0;
    case Routine::ZstdStreamEntry:
    case Routine::ZstdStreamLoop:
        return (ZstdMode() & 6) != 0;
    case Routine::Count:
        break;
    }
    return false;
}

/// Per process: each site's verdict (checked once, at its first compile).
struct ProcessSites {
    std::mutex mutex;
    u64 process_id = ~u64{0};
    u64 main_base = 0;
    std::array<int, static_cast<size_t>(Routine::Count)> state{}; ///< 0 unchecked, 1 on, 2 off
};

ProcessSites& Verdicts() {
    static ProcessSites sites;
    return sites;
}

bool IsOot(Core::System& system, const Kernel::KProcess& process) {
    if (process.GetProgramId() != OotProgramId) {
        return false;
    }
    const auto& build_id = system.GetApplicationProcessBuildID();
    return std::equal(OotBuildId.begin(), OotBuildId.end(), build_id.begin());
}

// ------------------------------------------------------------------ counters and log line

enum class Group : u32 { Frames = 0, Stream = 1, Count = 2 };

struct Stats {
    std::atomic<u64> calls{};    ///< answered on the host
    std::atomic<u64> declined{}; ///< left to the game's code
    std::atomic<u64> out_bytes{};
    std::atomic<u64> host_ns{};
    std::atomic<u64> max_ns{};
    std::atomic<u64> errors{};   ///< answered with the decoder's error (as the game's would)
    std::atomic<u64> verified{}; ///< verify mode: chunks compared with what the game's code did
    std::atomic<u64> mismatches{}; ///< of them different (must be 0)
};
std::array<Stats, static_cast<size_t>(Group::Count)> g_stats;
std::atomic<s64> g_last_log_ns{};

Stats& StatsOf(Group group) {
    return g_stats[static_cast<size_t>(group)];
}

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Add(std::atomic<u64>& counter, u64 value) {
    counter.fetch_add(value, std::memory_order_relaxed);
}

void NoteTime(Stats& stats, u64 ns) {
    Add(stats.host_ns, ns);
    u64 max = stats.max_ns.load(std::memory_order_relaxed);
    while (ns > max && !stats.max_ns.compare_exchange_weak(max, ns, std::memory_order_relaxed)) {
    }
}

/// "Host zstd" every 10 s at most, from whichever thread gets there, while anything ran.
void MaybeLog() {
    const s64 now = NowNs();
    s64 last = g_last_log_ns.load(std::memory_order_relaxed);
    if (now - last < 10'000'000'000LL ||
        !g_last_log_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        return;
    }
    const auto line = [](const char* name, Stats& s) {
        const u64 calls = s.calls.exchange(0, std::memory_order_relaxed);
        const u64 declined = s.declined.exchange(0, std::memory_order_relaxed);
        const u64 bytes = s.out_bytes.exchange(0, std::memory_order_relaxed);
        const u64 ns = s.host_ns.exchange(0, std::memory_order_relaxed);
        const u64 max_ns = s.max_ns.exchange(0, std::memory_order_relaxed);
        const u64 errors = s.errors.exchange(0, std::memory_order_relaxed);
        const u64 verified = s.verified.exchange(0, std::memory_order_relaxed);
        const u64 mismatches = s.mismatches.load(std::memory_order_relaxed);
        if (calls == 0 && declined == 0 && verified == 0) {
            return;
        }
        LOG_INFO(Core_ARM,
                 "Host zstd (FRITCHII_HOST_ZSTD={}) {}: {} calls on the host, {:.1f} MiB out in "
                 "{:.1f} ms (longest call {:.2f} ms), {} errors as the game's, {} left to the "
                 "game's code, verify: {} chunks compared, {} mismatches in all (must be 0) "
                 "(since the last line)",
                 ZstdMode(), name, calls, static_cast<double>(bytes) / (1024.0 * 1024.0),
                 static_cast<double>(ns) / 1e6, static_cast<double>(max_ns) / 1e6, errors,
                 declined, verified, mismatches);
    };
    line("whole-buffer", StatsOf(Group::Frames));
    line("stream", StatsOf(Group::Stream));
}

/// Why a decode went to the game's code: each reason logged once.
void NoteDecline(Group group, const char* why) {
    Add(StatsOf(group).declined, 1);
    static std::mutex mutex;
    static std::array<const char*, 24> seen{};
    {
        std::scoped_lock lock{mutex};
        for (auto& entry : seen) {
            if (entry == why) {
                return;
            }
            if (entry == nullptr) {
                entry = why;
                break;
            }
        }
    }
    LOG_INFO(Core_ARM, "Host zstd: a {} decode runs as the game's code: {} (logged once)",
             group == Group::Frames ? "whole-buffer" : "stream", why);
}

// ------------------------------------------------------------------ the game's DDicts

// The game's zstd 1.4 DDict: +8 its content, +16 the size, +0x18..+0x6840 the entropy tables it
// built, +0x6840 the dictID, +0x6844 whether the tables are present (the code reads both).
constexpr u64 DictContent = 0x8;
constexpr u64 DictSize = 0x10;
constexpr u64 DictTables = 0x18;
constexpr u64 DictTablesEnd = 0x6840;
constexpr u64 DictId = 0x6840;
constexpr u64 DictEntropy = 0x6844;
constexpr u64 MaxDictBytes = 64ULL << 20;
constexpr size_t MaxDicts = 32;

struct HostDict {
    u64 guest = 0;
    u64 content = 0;
    u64 size = 0;
    u32 id = 0;
    u32 entropy = 0;
    std::vector<u8> tables; ///< the game's tables when it was made: another DDict built in place
                            ///< at the same address (same size and ID) has other tables
    ZSTD_DDict* dd = nullptr;

    HostDict() = default;
    HostDict(const HostDict&) = delete;
    HostDict& operator=(const HostDict&) = delete;
    ~HostDict() {
        ZSTD_freeDDict(dd);
    }
};

std::mutex g_dict_mutex;
std::vector<std::shared_ptr<HostDict>> g_dicts; ///< newest last

/// The game's DDict at `guest` as a libzstd one (its content copied), or null when libzstd does
/// not take it as the same dictionary.
std::shared_ptr<HostDict> DictFor(Core::Memory::Memory& memory, u64 guest) {
    const u64 content = memory.Read64(guest + DictContent);
    const u64 size = memory.Read64(guest + DictSize);
    const u32 id = memory.Read32(guest + DictId);
    const u32 entropy = memory.Read32(guest + DictEntropy);
    thread_local std::vector<u8> tables;
    tables.resize(DictTablesEnd - DictTables);
    memory.ReadBlock(guest + DictTables, tables.data(), tables.size());

    std::scoped_lock lock{g_dict_mutex};
    for (auto it = g_dicts.begin(); it != g_dicts.end(); ++it) {
        HostDict& dict = **it;
        if (dict.guest != guest) {
            continue;
        }
        if (dict.content == content && dict.size == size && dict.id == id &&
            dict.entropy == entropy && dict.tables == tables) {
            return *it;
        }
        // The game made another DDict here: streams still using the old one keep it.
        g_dicts.erase(it);
        break;
    }
    if (size == 0 || size > MaxDictBytes) {
        return nullptr;
    }
    std::vector<u8> bytes(size);
    memory.ReadBlock(content, bytes.data(), bytes.size());
    auto dict = std::make_shared<HostDict>();
    dict->dd = ZSTD_createDDict_advanced(bytes.data(), bytes.size(), ZSTD_dlm_byCopy,
                                         entropy != 0 ? ZSTD_dct_fullDict : ZSTD_dct_rawContent,
                                         ZSTD_defaultCMem);
    if (dict->dd == nullptr || ZSTD_getDictID_fromDDict(dict->dd) != id) {
        return nullptr;
    }
    dict->guest = guest;
    dict->content = content;
    dict->size = size;
    dict->id = id;
    dict->entropy = entropy;
    dict->tables = tables;
    if (g_dicts.size() >= MaxDicts) {
        g_dicts.erase(g_dicts.begin());
    }
    g_dicts.push_back(dict);
    LOG_INFO(Core_ARM, "Host zstd: the game's dictionary {:#x} (ID {:#x}, {} bytes{}) taken",
             guest, id, size, entropy != 0 ? ", with entropy tables" : ", raw content");
    return dict;
}

// ------------------------------------------------------------------ buffers

/// A per-thread buffer that grows without zero filling; given back when it got very large.
struct Scratch {
    std::unique_ptr<u8[]> data;
    size_t capacity = 0;

    u8* Get(size_t size) {
        if (size > capacity || data == nullptr) {
            data = std::make_unique_for_overwrite<u8[]>((std::max)(size, size_t{1}));
            capacity = (std::max)(size, size_t{1});
        }
        return data.get();
    }
    void Trim() {
        if (capacity > (64ULL << 20)) {
            data.reset();
            capacity = 0;
        }
    }
};

ZSTD_DCtx* ThreadDctx() {
    struct Holder {
        ZSTD_DCtx* z = ZSTD_createDCtx();
        ~Holder() {
            ZSTD_freeDCtx(z);
        }
    };
    thread_local Holder holder;
    return holder.z;
}

// ------------------------------------------------------------------ whole-buffer decode

constexpr u64 DctxFormat = 0x7110; // the game's context: frame format (0 zstd1, 1 magicless)
constexpr u64 MaxFrameInput = 256ULL << 20;
constexpr u64 MaxFrameOutput = 512ULL << 20;

/// main+0xEB380. The three callers set up a static context in place right before the call
/// (ZSTD_initStaticDCtx inlined), pass no raw dictionary and read only the result: the content
/// size, or zstd's error code.
bool ZstdFrames(Core::Memory::Memory& memory, Registers& regs, Stats& stats) {
    const u64 dctx = regs.Get(0);
    const u64 dst = regs.Get(1);
    const u64 room = regs.Get(2);
    const u64 src = regs.Get(3);
    const u64 size = regs.Get(4);
    const u64 ddict = regs.Get(7);
    if (regs.Get(5) != 0 || regs.Get(6) != 0) {
        NoteDecline(Group::Frames, "a raw dictionary");
        return false;
    }
    if (memory.Read32(dctx + DctxFormat) != 0) {
        NoteDecline(Group::Frames, "the context asks for another frame format");
        return false;
    }
    if (size > MaxFrameInput || room > MaxFrameOutput) {
        NoteDecline(Group::Frames, "over 256 MiB in or 512 MiB out");
        return false;
    }
    std::shared_ptr<HostDict> dict;
    if (ddict != 0 && (dict = DictFor(memory, ddict)) == nullptr) {
        NoteDecline(Group::Frames, "a dictionary libzstd does not take as the game's");
        return false;
    }
    ZSTD_DCtx* const z = ThreadDctx();
    if (z == nullptr) {
        NoteDecline(Group::Frames, "no libzstd context");
        return false;
    }
    thread_local Scratch in_buffer;
    thread_local Scratch out_buffer;
    u8* const in = in_buffer.Get(size);
    u8* const out = out_buffer.Get(room);
    memory.ReadBlock(src, in, size);
    const size_t result =
        dict != nullptr ? ZSTD_decompress_usingDDict(z, out, room, in, size, dict->dd)
                        : ZSTD_decompress_usingDict(z, out, room, in, size, nullptr, 0);
    if (ZSTD_isError(result)) {
        // The game's decoder has written part of the output by then; its callers only test
        // the result.
        Add(stats.errors, 1);
    } else if (result != 0) {
        // WriteBlock marks the pages for the GPU caches as the game's stores would.
        memory.WriteBlock(dst, out, result);
        Add(stats.out_bytes, result);
    }
    in_buffer.Trim();
    out_buffer.Trim();
    regs.Set(0, result);
    return true;
}

// ------------------------------------------------------------------ stream decode

// The game's stream object: its fields as the game's code reads and writes them.
constexpr u64 StState = 0;     // u8: 1 start, 2 decoding, 3 failed, 4 done
constexpr u64 StDst = 8;       // u64: where the next output goes
constexpr u64 StRoom = 16;     // s32: output room left
constexpr u64 StLeft = 20;     // u32: content still to come (the result)
constexpr u64 StStageCap = 40; // s32: the staging buffer's size
constexpr u64 StStaged = 44;   // s32: bytes in the staging buffer
constexpr u64 MaxStreamContent = 512ULL << 20;
constexpr u64 MaxStreamBytes = 1ULL << 30; // all host streams' content buffers together
constexpr s32 MaxChunk = 64 << 20;
constexpr size_t MaxStreams = 64;

/// The stream object's fields the loop reads and writes: the game's own (in guest memory), or
/// a shadow copy of them (verify mode, where the game's code writes the real ones).
class StreamFields {
public:
    /// The game's fields.
    StreamFields(Core::Memory::Memory& memory, u64 guest) : m_memory{&memory}, m_guest{guest} {}
    /// A shadow copy of the game's fields as they are now.
    static StreamFields Shadow(Core::Memory::Memory& memory, u64 guest) {
        StreamFields fields{memory, guest};
        fields.m_state = memory.Read8(guest + StState);
        fields.m_dst = memory.Read64(guest + StDst);
        fields.m_room = memory.Read32(guest + StRoom);
        fields.m_left = memory.Read32(guest + StLeft);
        fields.m_shadow = true;
        return fields;
    }

    bool IsShadow() const {
        return m_shadow;
    }
    u8 State() const {
        return m_shadow ? m_state : m_memory->Read8(m_guest + StState);
    }
    void SetState(u8 value) {
        m_shadow ? void(m_state = value) : m_memory->Write8(m_guest + StState, value);
    }
    u64 Dst() const {
        return m_shadow ? m_dst : m_memory->Read64(m_guest + StDst);
    }
    void SetDst(u64 value) {
        m_shadow ? void(m_dst = value) : m_memory->Write64(m_guest + StDst, value);
    }
    u32 Room() const {
        return m_shadow ? m_room : m_memory->Read32(m_guest + StRoom);
    }
    void SetRoom(u32 value) {
        m_shadow ? void(m_room = value) : m_memory->Write32(m_guest + StRoom, value);
    }
    u32 Left() const {
        return m_shadow ? m_left : m_memory->Read32(m_guest + StLeft);
    }
    void SetLeft(u32 value) {
        m_shadow ? void(m_left = value) : m_memory->Write32(m_guest + StLeft, value);
    }
    s32 StageCap() const {
        return static_cast<s32>(m_memory->Read32(m_guest + StStageCap));
    }
    /// Output: the game's memory, or nothing for a shadow.
    void Output(u64 dst, const u8* data, size_t size) {
        if (!m_shadow) {
            m_memory->WriteBlock(dst, data, size);
        }
    }

private:
    Core::Memory::Memory* m_memory;
    u64 m_guest;
    bool m_shadow = false;
    u8 m_state = 0;
    u64 m_dst = 0;
    u32 m_room = 0;
    u32 m_left = 0;
};

struct HostStream {
    ZSTD_DCtx* z = nullptr;
    std::shared_ptr<HostDict> dict;
    /// The frame's content as decoded so far: the window the next blocks copy from (the game's
    /// decoder reads it from its output; the bytes are the same).
    std::unique_ptr<u8[]> content;
    size_t content_size = 0;
    size_t produced = 0;
    /// The start of a piece that goes on in the next chunk (the game keeps it in its staging
    /// buffer; only this function reads that buffer, so the host keeps it here).
    std::vector<u8> staged;

    /// Verify mode: what the game's code should have left after the last chunk.
    bool verify = false;
    bool check_pending = false;
    u8 expect_state = 0;
    u64 expect_dst = 0;
    u32 expect_room = 0;
    u32 expect_left = 0;
    u64 check_dst = 0;      ///< where the chunk's output went
    size_t check_from = 0;  ///< and where it is in `content`
    size_t check_size = 0;

    HostStream() = default;
    HostStream(const HostStream&) = delete;
    HostStream& operator=(const HostStream&) = delete;
    ~HostStream() {
        ZSTD_freeDCtx(z);
    }
};

std::mutex g_stream_mutex;
std::unordered_map<u64, std::unique_ptr<HostStream>> g_streams; ///< by the stream object's address
std::unordered_set<u64> g_stream_starts; ///< objects whose call is in the game's frame set-up
u64 g_stream_bytes = 0;
size_t g_stream_peak = 0;

/// One guest thread drives a stream object at a time (the game's job loop), so an entry is
/// used without the lock; only that stream's own calls make or free it.
HostStream* FindStream(u64 guest) {
    std::scoped_lock lock{g_stream_mutex};
    const auto it = g_streams.find(guest);
    return it != g_streams.end() ? it->second.get() : nullptr;
}

void DropStreamLocked(u64 guest) {
    const auto it = g_streams.find(guest);
    if (it != g_streams.end()) {
        g_stream_bytes -= it->second->content_size;
        g_streams.erase(it);
    }
}

void DropStream(u64 guest) {
    std::scoped_lock lock{g_stream_mutex};
    DropStreamLocked(guest);
}

/// The entry of a call: a new frame (state 1) is marked so the loop site knows the game's
/// set-up just ran; any host stream of the object ends.
void NoteStreamEntry(u64 guest, bool new_frame) {
    std::scoped_lock lock{g_stream_mutex};
    DropStreamLocked(guest);
    if (new_frame) {
        g_stream_starts.insert(guest);
    } else {
        g_stream_starts.erase(guest);
    }
}

bool TakeStreamStart(u64 guest) {
    std::scoped_lock lock{g_stream_mutex};
    return g_stream_starts.erase(guest) != 0;
}

/// A new frame on stream object `guest`: its entry, or null (the reason in *why).
HostStream* StartStream(u64 guest, u64 content_size, std::shared_ptr<HostDict> dict,
                        const char** why) {
    std::scoped_lock lock{g_stream_mutex};
    DropStreamLocked(guest);
    if (g_streams.size() >= MaxStreams) {
        *why = "64 host streams in use";
        return nullptr;
    }
    if (g_stream_bytes + content_size > MaxStreamBytes) {
        *why = "the host streams' buffers would pass 1 GiB";
        return nullptr;
    }
    auto stream = std::make_unique<HostStream>();
    stream->z = ZSTD_createDCtx();
    if (stream->z == nullptr) {
        *why = "no libzstd context";
        return nullptr;
    }
    stream->content =
        std::make_unique_for_overwrite<u8[]>((std::max)(content_size, u64{1}));
    stream->content_size = content_size;
    stream->dict = std::move(dict);
    HostStream* const result = stream.get();
    g_streams.emplace(guest, std::move(stream));
    g_stream_bytes += content_size;
    if (g_streams.size() > g_stream_peak) {
        g_stream_peak = g_streams.size();
        LOG_INFO(Core_ARM, "Host zstd: {} stream(s) decoding on the host at once", g_stream_peak);
    }
    return result;
}

/// One decompressContinue of `size` bytes, its output stored and counted as the game does.
size_t StreamContinue(StreamFields& fields, HostStream& stream, const u8* src, size_t size,
                      Stats& stats) {
    const u64 dst = fields.Dst();
    const u32 room32 = fields.Room();
    const size_t room = static_cast<size_t>(static_cast<s64>(static_cast<s32>(room32)));
    const size_t capacity = (std::min)(room, stream.content_size - stream.produced);
    u8* const out = stream.content.get() + stream.produced;
    const size_t result = ZSTD_decompressContinue(stream.z, out, capacity, src, size);
    if (ZSTD_isError(result)) {
        return result;
    }
    if (result != 0) {
        fields.Output(dst, out, result);
        stream.produced += result;
        if (!fields.IsShadow()) {
            Add(stats.out_bytes, result);
        }
    }
    fields.SetDst(dst + static_cast<u64>(static_cast<s64>(static_cast<s32>(result))));
    fields.SetRoom(room32 - static_cast<u32>(result));
    fields.SetLeft(fields.Left() - static_cast<u32>(result));
    return result;
}

/// The game's loop over one chunk of a started stream: its result.
s32 StreamChunk(StreamFields& fields, HostStream& stream, const u8* src, s32 n, Stats& stats) {
    const auto fail = [&](size_t result) -> s32 {
        fields.SetState(3);
        Add(stats.errors, 1);
        return ZSTD_getErrorCode(result) == ZSTD_error_dstSize_tooSmall ? -2 : -1;
    };
    while (n >= 1) {
        const size_t want = ZSTD_nextSrcSizeToDecompress(stream.z);
        if (want == 0) {
            // Input after the frame's end.
            fields.SetState(3);
            Add(stats.errors, 1);
            return -3;
        }
        const s32 w = static_cast<s32>(want);
        if (!stream.staged.empty()) {
            // A piece begun in an earlier chunk: filled up to its size.
            const s32 staged = static_cast<s32>(stream.staged.size());
            if (w > staged) {
                const s32 take = (std::min)(w - staged, n);
                stream.staged.insert(stream.staged.end(), src, src + take);
                src += take;
                n -= take;
            }
            if (static_cast<s32>(stream.staged.size()) < w) {
                continue;
            }
            const size_t result = StreamContinue(fields, stream, stream.staged.data(), want, stats);
            if (ZSTD_isError(result)) {
                return fail(result);
            }
            stream.staged.erase(stream.staged.begin(), stream.staged.begin() + want);
            continue;
        }
        if (n < w) {
            // The piece goes on in the next chunk.
            if (fields.StageCap() < w) {
                fields.SetState(3);
                Add(stats.errors, 1);
                return -1;
            }
            stream.staged.assign(src, src + n);
            break;
        }
        const size_t result = StreamContinue(fields, stream, src, want, stats);
        if (ZSTD_isError(result)) {
            return fail(result);
        }
        src += want;
        n -= w;
    }
    if (ZSTD_nextSrcSizeToDecompress(stream.z) == 0) {
        fields.SetState(4);
    }
    return static_cast<s32>(fields.Left());
}

void NoteMismatch(u64 guest, const char* what, u64 game, u64 host) {
    Stats& stats = StatsOf(Group::Stream);
    const u64 n = stats.mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16) {
        LOG_WARNING(Core_ARM,
                    "Host zstd verify mismatch #{} (stream object {:#x}): {}: the game's code "
                    "{:#x}, the host {:#x}",
                    n, guest, what, game, host);
    }
}

/// Verify mode: the game's code ran the last chunk; compare what it left with the shadow.
/// `bytes`: compare the output too (only while the frame is under way: once it is done, its
/// output may already belong to someone else).
void CheckShadow(Core::Memory::Memory& memory, u64 guest, HostStream& stream, bool bytes) {
    if (!stream.check_pending) {
        return;
    }
    stream.check_pending = false;
    Stats& stats = StatsOf(Group::Stream);
    Add(stats.verified, 1);
    const u64 before = stats.mismatches.load(std::memory_order_relaxed);
    const u8 state = memory.Read8(guest + StState);
    // A done frame (4) may be set to 1 for the next one before this check.
    if (state != stream.expect_state && !(stream.expect_state == 4 && state == 1)) {
        NoteMismatch(guest, "state", state, stream.expect_state);
    }
    if (state == stream.expect_state) {
        const u64 dst = memory.Read64(guest + StDst);
        const u32 room = memory.Read32(guest + StRoom);
        const u32 left = memory.Read32(guest + StLeft);
        if (dst != stream.expect_dst) {
            NoteMismatch(guest, "output pointer", dst, stream.expect_dst);
        }
        if (room != stream.expect_room) {
            NoteMismatch(guest, "output room", room, stream.expect_room);
        }
        if (left != stream.expect_left) {
            NoteMismatch(guest, "bytes still to come", left, stream.expect_left);
        }
    }
    if (bytes && stream.check_size != 0 &&
        stats.mismatches.load(std::memory_order_relaxed) == before) {
        thread_local std::vector<u8> game;
        game.resize(stream.check_size);
        memory.ReadBlock(stream.check_dst, game.data(), game.size());
        const u8* const host = stream.content.get() + stream.check_from;
        if (std::memcmp(game.data(), host, game.size()) != 0) {
            size_t at = 0;
            while (at < game.size() && game[at] == host[at]) {
                ++at;
            }
            NoteMismatch(guest, "output byte", stream.check_dst + at, host[at]);
        }
    }
}

enum class Outcome { Answered, Declined };

/// main+0xF5C80, the entry: always the game's code (its frame set-up included); notes a new
/// frame for the loop site and ends any host stream of an object that starts or ends a frame.
Outcome ZstdStreamEntry(Core::Memory::Memory& memory, Registers& regs) {
    const u64 guest = regs.Get(0);
    const u8 state = memory.Read8(guest + StState);
    if (StreamVerify()) {
        if (HostStream* const stream = FindStream(guest); stream != nullptr) {
            CheckShadow(memory, guest, *stream, state == 2);
        }
    }
    if (state == 1 || state == 3 || state == 4) {
        NoteStreamEntry(guest, state == 1);
    }
    return Outcome::Declined;
}

/// main+0xF5CD0, the loop: x19 the object, x20 the chunk, w23 its size, x21 the game's context
/// (set up by the game's code), x22 the DDict right after a frame set-up.
Outcome ZstdStreamLoop(Core::Memory::Memory& memory, Registers& regs, Stats& stats) {
    const u64 guest = regs.Get(19);
    const u64 src = regs.Get(20);
    const s32 n = static_cast<s32>(regs.Get(23));
    const bool verify = StreamVerify();
    HostStream* stream = nullptr;
    if (TakeStreamStart(guest)) {
        // The game's code just set the frame up: the header read, the context ready, the
        // content size in the object, state 2. The host takes the frame from its first byte.
        if (memory.Read8(guest + StState) != 2) {
            NoteDecline(Group::Stream, "the frame set-up did not leave state 2");
            return Outcome::Declined;
        }
        if (memory.Read32(guest + StStaged) != 0) {
            // The game would begin this frame with those bytes: only its code does that.
            NoteDecline(Group::Stream, "staged bytes left from an earlier frame");
            return Outcome::Declined;
        }
        const u64 content = memory.Read32(guest + StLeft);
        if (content > MaxStreamContent) {
            NoteDecline(Group::Stream, "content over 512 MiB");
            return Outcome::Declined;
        }
        if (n > MaxChunk) {
            NoteDecline(Group::Stream, "a chunk over 64 MiB");
            return Outcome::Declined;
        }
        const u64 ddict = regs.Get(22);
        std::shared_ptr<HostDict> dict;
        if (ddict != 0 && (dict = DictFor(memory, ddict)) == nullptr) {
            NoteDecline(Group::Stream, "a dictionary libzstd does not take as the game's");
            return Outcome::Declined;
        }
        const char* why = "";
        stream = StartStream(guest, content, std::move(dict), &why);
        if (stream == nullptr) {
            NoteDecline(Group::Stream, why);
            return Outcome::Declined;
        }
        const size_t begin = ZSTD_decompressBegin_usingDDict(
            stream->z, stream->dict != nullptr ? stream->dict->dd : nullptr);
        if (ZSTD_isError(begin)) {
            DropStream(guest);
            NoteDecline(Group::Stream, "libzstd refused to begin");
            return Outcome::Declined;
        }
        stream->verify = verify;
    } else {
        stream = FindStream(guest);
        if (stream == nullptr) {
            return Outcome::Declined; // a frame the game's code runs from its start
        }
        if (n > MaxChunk) {
            // Cannot go back to the game's code in the middle of a frame.
            DropStream(guest);
            memory.Write8(guest + StState, 3);
            regs.Set(0, static_cast<u32>(-1));
            NoteDecline(Group::Stream, "a chunk over 64 MiB in a host frame (failed)");
            return Outcome::Answered;
        }
    }
    thread_local Scratch in_buffer;
    const u8* in = nullptr;
    if (n >= 1) {
        u8* const bytes = in_buffer.Get(static_cast<size_t>(n));
        memory.ReadBlock(src, bytes, static_cast<size_t>(n));
        in = bytes;
    }
    if (stream->verify) {
        // The host decodes a shadow; the game's code decodes the chunk for real, and the next
        // entry compares the two.
        StreamFields fields = StreamFields::Shadow(memory, guest);
        const u64 dst = fields.Dst();
        const size_t from = stream->produced;
        StreamChunk(fields, *stream, in, n, stats);
        stream->check_pending = true;
        stream->expect_state = fields.State();
        stream->expect_dst = fields.Dst();
        stream->expect_room = fields.Room();
        stream->expect_left = fields.Left();
        stream->check_dst = dst;
        stream->check_from = from;
        stream->check_size = stream->produced - from;
        in_buffer.Trim();
        return Outcome::Declined;
    }
    StreamFields fields{memory, guest};
    const s32 result = StreamChunk(fields, *stream, in, n, stats);
    const u8 after = memory.Read8(guest + StState);
    if (after == 3 || after == 4) {
        DropStream(guest);
    }
    in_buffer.Trim();
    regs.Set(0, static_cast<u32>(result));
    return Outcome::Answered;
}

/// Declining the loop site: the `cmp w23, #1` the BRK stands for, then the next instruction.
void DeclineLoop(Registers& regs, u64 pc) {
    const u32 a = static_cast<u32>(regs.Get(23));
    const u32 r = a - 1;
    const u32 n = r >> 31;
    const u32 z = r == 0 ? 1 : 0;
    const u32 c = a >= 1 ? 1 : 0;
    const u32 v = ((a ^ 1U) & (a ^ r)) >> 31;
    const u32 nzcv = (n << 3) | (z << 2) | (c << 1) | v;
    regs.SetPstate((regs.GetPstate() & 0x0FFFFFFFU) | (nzcv << 28));
    regs.SetPC(pc + 4);
}

const Site* SiteAt(u64 main_base, u64 address) {
    for (const Site& site : OotSites) {
        if (address == main_base + site.main_offset) {
            return &site;
        }
    }
    return nullptr;
}

const char* SiteName(Routine routine) {
    switch (routine) {
    case Routine::ZstdFrames:
        return "whole-buffer decode";
    case Routine::ZstdStreamEntry:
        return "stream decode entry";
    case Routine::ZstdStreamLoop:
        return "stream decode loop";
    case Routine::Count:
        break;
    }
    return "?";
}

} // Anonymous namespace

Sites SitesFor(Core::System& system, const Kernel::KProcess& process) {
    if (ZstdMode() == 0 || !IsOot(system, process)) {
        return {};
    }
    const u64 main_base = GetInteger(process.GetEntryPoint()) + MainOffset;
    Sites sites{~u64{0}, 0};
    for (const Site& site : OotSites) {
        if (!RoutineOn(site.routine)) {
            continue;
        }
        sites.first = (std::min)(sites.first, main_base + site.main_offset);
        sites.last = (std::max)(sites.last, main_base + site.main_offset);
    }
    return sites.last != 0 ? sites : Sites{};
}

u32 InstructionFor(Core::System& system, Kernel::KProcess& process, Core::Memory::Memory& memory,
                   u64 vaddr, u32 instruction) {
    const u64 main_base = GetInteger(process.GetEntryPoint()) + MainOffset;
    const Site* const site = SiteAt(main_base, vaddr);
    if (site == nullptr || instruction != site->checks[0].word || !RoutineOn(site->routine)) {
        return instruction;
    }
    ProcessSites& verdicts = Verdicts();
    std::scoped_lock lock{verdicts.mutex};
    if (verdicts.process_id != process.GetProcessId() || verdicts.main_base != main_base) {
        verdicts.process_id = process.GetProcessId();
        verdicts.main_base = main_base;
        verdicts.state.fill(0);
    }
    int& state = verdicts.state[static_cast<size_t>(site->routine)];
    if (state == 0) {
        bool match = IsOot(system, process);
        for (const Check& check : site->checks) {
            if (!match || check.word == 0) {
                break;
            }
            match = memory.Read32(vaddr + static_cast<u64>(static_cast<s64>(check.offset))) ==
                    check.word;
        }
        state = match ? 1 : 2;
        LOG_INFO(Core_ARM, "Host zstd: main+{:#x} ({}) {}", site->main_offset,
                 SiteName(site->routine),
                 match ? "hooked" : "does not match this build's code: off");
    }
    return state == 1 ? BrkFor(site->routine) : instruction;
}

bool OnBreakpoint([[maybe_unused]] Core::System& system, Kernel::KProcess& process,
                  Core::Memory::Memory& memory, Registers& regs, u64 pc) {
    const u64 main_base = GetInteger(process.GetEntryPoint()) + MainOffset;
    const Site* const site = SiteAt(main_base, pc);
    if (site == nullptr) {
        return false;
    }
    {
        // Only a BRK this file put there (the guest's own word is the checked one).
        ProcessSites& verdicts = Verdicts();
        std::scoped_lock lock{verdicts.mutex};
        if (verdicts.process_id != process.GetProcessId() ||
            verdicts.state[static_cast<size_t>(site->routine)] != 1) {
            return false;
        }
    }
    const s64 start = NowNs();
    Outcome outcome = Outcome::Declined;
    switch (site->routine) {
    case Routine::ZstdFrames: {
        Stats& stats = StatsOf(Group::Frames);
        const u64 return_address = regs.Get(30);
        try {
            outcome = ZstdFrames(memory, regs, stats) ? Outcome::Answered : Outcome::Declined;
        } catch (const std::bad_alloc&) {
            // Before anything was written: the game's code does the call.
            NoteDecline(Group::Frames, "host memory ran out");
        }
        if (outcome == Outcome::Answered) {
            regs.SetPC(return_address);
            Add(stats.calls, 1);
            NoteTime(stats, static_cast<u64>(NowNs() - start));
        } else {
            // The pacibsp the BRK stands for is a no-op here: the game's code goes on after it.
            regs.SetPC(pc + 4);
        }
        break;
    }
    case Routine::ZstdStreamEntry:
        ZstdStreamEntry(memory, regs);
        regs.SetPC(pc + 4); // (the pacibsp)
        break;
    case Routine::ZstdStreamLoop: {
        Stats& stats = StatsOf(Group::Stream);
        try {
            outcome = ZstdStreamLoop(memory, regs, stats);
        } catch (const std::bad_alloc&) {
            // A frame the host took cannot go back to the game's code: it fails as a broken
            // frame would. Otherwise nothing was written and the game's code goes on.
            const u64 guest = regs.Get(19);
            if (FindStream(guest) != nullptr) {
                DropStream(guest);
                memory.Write8(guest + StState, 3);
                regs.Set(0, static_cast<u32>(-1));
                outcome = Outcome::Answered;
            }
            NoteDecline(Group::Stream, "host memory ran out");
        }
        if (outcome == Outcome::Answered) {
            // The function's own epilogue: restores x19..x30 and the stack, returns w0.
            regs.SetPC(main_base + StreamEpilogue);
            Add(stats.calls, 1);
            NoteTime(stats, static_cast<u64>(NowNs() - start));
        } else {
            DeclineLoop(regs, pc);
        }
        break;
    }
    case Routine::Count:
        return false;
    }
    MaybeLog();
    return true;
}

u64 StreamVerifyMismatches() {
    return StatsOf(Group::Stream).mismatches.load(std::memory_order_relaxed);
}

} // namespace Core::HostRoutines
