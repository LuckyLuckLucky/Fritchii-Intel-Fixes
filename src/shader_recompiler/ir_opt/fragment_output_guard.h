// SPDX-License-Identifier: GPL-3.0-or-later

// Adapted from MagicBeanDev/Longshot gpu/vk/eden/shader_recompiler/ir_opt/fragment_output_guard.h
// (GPL-3.0): the NaN guard on float colour outputs.

#pragma once

#include <utility>
#include <vector>

#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader::Optimization {

/// Keeps every float colour output of a fragment program finite: NaN becomes 0, +-Inf (and
/// anything beyond +-1e30) is clamped to +-1e30. One NaN in OoT's scene target goes through the
/// bloom's chain of averages to the whole picture: a one-frame white flash with the HUD on top
/// (Longshot docs/design.md, findings 2026-11-06). Integer targets are left as they are (their
/// outputs carry integer bits).
/// float_targets: bit i set when colour target i is a float (or unorm/snorm) one.
/// Returns the number of guarded writes. INTEL_FORK_NO_NAN_GUARD=1 skips it (vk_pipeline_cache).
inline size_t GuardFragmentOutputs(IR::Program& program, u32 float_targets) {
    if (program.stage != Stage::Fragment) {
        return 0;
    }
    std::vector<std::pair<IR::Block*, IR::Inst*>> writes;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::SetFragColor && inst.Arg(0).IsImmediate() &&
                ((float_targets >> inst.Arg(0).U32()) & 1) != 0 && !inst.Arg(2).IsImmediate()) {
                writes.emplace_back(block, &inst);
            }
        }
    }
    for (const auto& [block, inst] : writes) {
        IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(*inst)};
        const IR::F32 value{inst->Arg(2)};
        const IR::F32 limited{ir.FPClamp(value, ir.Imm32(-1e30f), ir.Imm32(1e30f))};
        const IR::F32 safe{ir.Select(ir.FPIsNan(value), ir.Imm32(0.0f), limited)};
        inst->SetArg(2, safe);
    }
    return writes.size();
}

} // namespace Shader::Optimization
