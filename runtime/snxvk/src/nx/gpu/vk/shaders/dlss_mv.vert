#version 450
// SWR (DLSS motion vectors, gpu/vk/dlss.c): a depth-prepass draw again, from its transform-feedback capture. The
// position is the one the game's own vertex shader produced this frame (so the depth test EQUAL against the scene
// depth matches exactly); the previous frame's position of the same vertex rides along.
layout(location = 0) in vec4 cur;
layout(location = 1) in vec4 prev;
layout(location = 0) out vec4 o_cur;
layout(location = 1) out vec4 o_prev;
invariant gl_Position;
void main() {
    o_cur = cur;
    o_prev = prev;
    gl_Position = cur;
}
