// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <unordered_map>
#include <unordered_set>

#include "shader_recompiler/backend/spirv/vulkan_memory_model_pass.h"

namespace Shader::Backend::SPIRV {
namespace {
using u32 = std::uint32_t;

// Opcodes, storage classes and mask bits from the SPIR-V specification.
constexpr u32 OpMemoryModel = 14;
constexpr u32 OpFunctionParameter = 55;
constexpr u32 OpTypePointer = 32;
constexpr u32 OpConstant = 43;
constexpr u32 OpVariable = 59;
constexpr u32 OpImageTexelPointer = 60;
constexpr u32 OpLoad = 61;
constexpr u32 OpStore = 62;
constexpr u32 OpAccessChain = 65;
constexpr u32 OpInBoundsAccessChain = 66;
constexpr u32 OpPtrAccessChain = 67;
constexpr u32 OpInBoundsPtrAccessChain = 70;
constexpr u32 OpCopyObject = 83;
constexpr u32 OpImageRead = 98;
constexpr u32 OpImageWrite = 99;
constexpr u32 OpBitcast = 124;
constexpr u32 OpSelect = 169;
constexpr u32 OpControlBarrier = 224;
constexpr u32 OpMemoryBarrier = 225;
constexpr u32 OpAtomicLoad = 227;
constexpr u32 OpAtomicStore = 228;
constexpr u32 OpAtomicExchange = 229;
constexpr u32 OpAtomicCompareExchange = 230;
constexpr u32 OpAtomicCompareExchangeWeak = 231;
constexpr u32 OpAtomicIIncrement = 232;
constexpr u32 OpAtomicXor = 242;
constexpr u32 OpPhi = 245;
constexpr u32 OpCooperativeMatrixLoadKHR = 4457;
constexpr u32 OpCooperativeMatrixStoreKHR = 4458;
constexpr u32 OpAtomicFMinEXT = 5614;
constexpr u32 OpAtomicFMaxEXT = 5615;
constexpr u32 OpAtomicFAddEXT = 6035;

constexpr u32 MemoryModelVulkan = 3;
constexpr u32 StorageClassWorkgroup = 4;
constexpr u32 StorageClassStorageBuffer = 12;
constexpr u32 StorageClassPhysicalStorageBuffer = 5349;

constexpr u32 MemoryAccessNonPrivatePointer = 0x20;
constexpr u32 ImageOperandsNonPrivateTexel = 0x400;
constexpr u32 SemanticsAcquire = 0x2;
constexpr u32 SemanticsRelease = 0x4;
constexpr u32 SemanticsAcquireRelease = 0x8;
constexpr u32 SemanticsMakeAvailable = 0x2000;
constexpr u32 SemanticsMakeVisible = 0x4000;

bool Shared(u32 storage_class) {
    return storage_class == StorageClassWorkgroup || storage_class == StorageClassStorageBuffer ||
           storage_class == StorageClassPhysicalStorageBuffer;
}

u32 WithAvailability(u32 semantics) {
    u32 result = semantics;
    if (semantics & (SemanticsRelease | SemanticsAcquireRelease)) {
        result |= SemanticsMakeAvailable;
    }
    if (semantics & (SemanticsAcquire | SemanticsAcquireRelease)) {
        result |= SemanticsMakeVisible;
    }
    return result;
}

/// Word offsets (from the instruction start) of the memory semantics ids an instruction has.
std::vector<u32> SemanticsOperands(u32 opcode) {
    switch (opcode) {
    case OpControlBarrier:
        return {3};
    case OpMemoryBarrier:
        return {2};
    case OpAtomicStore:
        return {3};
    case OpAtomicCompareExchange:
    case OpAtomicCompareExchangeWeak:
        return {5, 6};
    case OpAtomicLoad:
    case OpAtomicExchange:
    case OpAtomicFMinEXT:
    case OpAtomicFMaxEXT:
    case OpAtomicFAddEXT:
        return {5};
    default:
        if (opcode >= OpAtomicIIncrement && opcode <= OpAtomicXor) {
            return {5};
        }
        return {};
    }
}

bool MakesPointer(u32 opcode) {
    switch (opcode) {
    case OpVariable:
    case OpImageTexelPointer:
    case OpAccessChain:
    case OpInBoundsAccessChain:
    case OpPtrAccessChain:
    case OpInBoundsPtrAccessChain:
    case OpCopyObject:
    case OpBitcast:
    case OpSelect:
    case OpPhi:
    case OpFunctionParameter:
    case OpLoad:
        return true;
    default:
        return false;
    }
}
} // Anonymous namespace

VulkanMemoryModelPassStats MakeVulkanMemoryModelCoherent(std::vector<u32>& code) {
    VulkanMemoryModelPassStats stats;
    if (code.size() < 5) {
        return stats;
    }
    // Pass 1: the memory model, pointer storage classes, constants and the semantics in use.
    bool vulkan_model = false;
    std::unordered_map<u32, u32> pointer_type_class;
    std::unordered_map<u32, u32> pointer_class;
    struct ConstantDef {
        u32 type;
        u32 value;
    };
    std::unordered_map<u32, ConstantDef> constants;
    std::unordered_set<u32> semantics_ids;
    for (size_t offset = 5; offset < code.size();) {
        const u32 count = code[offset] >> 16;
        const u32 opcode = code[offset] & 0xFFFFu;
        if (count == 0 || offset + count > code.size()) {
            return stats;
        }
        if (opcode == OpMemoryModel && count >= 3) {
            vulkan_model = code[offset + 2] == MemoryModelVulkan;
        } else if (opcode == OpTypePointer && count >= 3) {
            pointer_type_class[code[offset + 1]] = code[offset + 2];
        } else if (opcode == OpConstant && count == 4) {
            constants[code[offset + 2]] = {code[offset + 1], code[offset + 3]};
        } else if (MakesPointer(opcode) && count >= 3) {
            if (const auto it = pointer_type_class.find(code[offset + 1]);
                it != pointer_type_class.end()) {
                pointer_class[code[offset + 2]] = it->second;
            }
        }
        for (const u32 operand : SemanticsOperands(opcode)) {
            if (operand < count) {
                semantics_ids.insert(code[offset + operand]);
            }
        }
        offset += count;
    }
    if (!vulkan_model) {
        return stats;
    }
    // New constants for the semantics that need availability or visibility, defined right after
    // the constants they replace (before any function uses them).
    u32 bound = code[3];
    std::unordered_map<u32, u32> replaced_semantics;
    for (const u32 id : semantics_ids) {
        const auto it = constants.find(id);
        if (it == constants.end()) {
            continue;
        }
        const u32 value = WithAvailability(it->second.value);
        if (value != it->second.value) {
            replaced_semantics[id] = bound++;
        }
    }
    const auto storage_class_of = [&](u32 pointer) -> u32 {
        const auto it = pointer_class.find(pointer);
        return it != pointer_class.end() ? it->second : ~0u;
    };
    // Pass 2: rewrite.
    std::vector<u32> out;
    out.reserve(code.size() + code.size() / 16);
    out.insert(out.end(), code.begin(), code.begin() + 5);
    for (size_t offset = 5; offset < code.size();) {
        const u32 count = code[offset] >> 16;
        const u32 opcode = code[offset] & 0xFFFFu;
        std::vector<u32> inst(code.begin() + offset, code.begin() + offset + count);
        const auto add_mask = [&inst](size_t mask_index, u32 bit) {
            if (inst.size() > mask_index) {
                inst[mask_index] |= bit;
            } else {
                inst.push_back(bit);
            }
            inst[0] = (static_cast<u32>(inst.size()) << 16) | (inst[0] & 0xFFFFu);
        };
        switch (opcode) {
        case OpLoad:
            // %type %result %pointer [mask]
            if (count >= 4 && Shared(storage_class_of(inst[3]))) {
                add_mask(4, MemoryAccessNonPrivatePointer);
                ++stats.loads;
            }
            break;
        case OpStore:
            // %pointer %object [mask]
            if (count >= 3 && Shared(storage_class_of(inst[1]))) {
                add_mask(3, MemoryAccessNonPrivatePointer);
                ++stats.stores;
            }
            break;
        case OpCooperativeMatrixLoadKHR:
            // %type %result %pointer %layout %stride [mask]
            if (count >= 6 && Shared(storage_class_of(inst[3]))) {
                add_mask(6, MemoryAccessNonPrivatePointer);
                ++stats.loads;
            }
            break;
        case OpCooperativeMatrixStoreKHR:
            // %pointer %object %layout %stride [mask]
            if (count >= 5 && Shared(storage_class_of(inst[1]))) {
                add_mask(5, MemoryAccessNonPrivatePointer);
                ++stats.stores;
            }
            break;
        case OpImageRead:
            // %type %result %image %coordinate [operands]
            if (count >= 5) {
                add_mask(5, ImageOperandsNonPrivateTexel);
                ++stats.images;
            }
            break;
        case OpImageWrite:
            // %image %coordinate %texel [operands]
            if (count >= 4) {
                add_mask(4, ImageOperandsNonPrivateTexel);
                ++stats.images;
            }
            break;
        default:
            break;
        }
        for (const u32 operand : SemanticsOperands(opcode)) {
            if (operand < inst.size()) {
                if (const auto it = replaced_semantics.find(inst[operand]);
                    it != replaced_semantics.end()) {
                    inst[operand] = it->second;
                    ++stats.semantics;
                }
            }
        }
        out.insert(out.end(), inst.begin(), inst.end());
        if (opcode == OpConstant && count == 4) {
            if (const auto it = replaced_semantics.find(code[offset + 2]);
                it != replaced_semantics.end()) {
                out.insert(out.end(), {(4u << 16) | OpConstant, code[offset + 1], it->second,
                                       WithAvailability(code[offset + 3])});
                ++stats.constants;
            }
        }
        offset += count;
    }
    out[3] = bound;
    code = std::move(out);
    return stats;
}

} // namespace Shader::Backend::SPIRV
