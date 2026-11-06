// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The on-screen controls and the game's menu (gpu/vk/ui.c): quads in the picture's pixels
#version 450
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color; // R G B A bytes, as the menu's colours are written
layout(push_constant) uniform Picture {
    vec2 size;
    uint srgb;
} pc;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
void main() {
    gl_Position = vec4(pos / pc.size * 2.0 - 1.0, 0.0, 1.0);
    v_uv = uv;
    v_color = color;
}
