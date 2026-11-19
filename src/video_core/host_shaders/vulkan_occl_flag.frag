// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

// FRITCHII_OCCL_FLAG (fritchii_occl_flag.h): the pixel program of an occlusion test draw's flag
// pipeline. With early fragment tests it runs only for fragments that passed the depth and
// stencil tests, so a stored 4096 means "samples passed" for the test's query slot (the 64-bit
// layout of an occlusion query result). The slot's address is pushed at offset 40 of the
// pipeline's push constant range (bytes 40 to 47 are otherwise unused).

#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(early_fragment_tests) in;

layout(buffer_reference, std430, buffer_reference_align = 8) buffer OcclFlagSlot {
    uvec2 value;
};

layout(push_constant) uniform OcclFlagPush {
    layout(offset = 40) uvec2 slot_address;
};

void main() {
    OcclFlagSlot(slot_address).value = uvec2(4096u, 0u);
}
