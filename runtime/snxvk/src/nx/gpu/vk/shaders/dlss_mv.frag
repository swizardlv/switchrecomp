#version 450
// SWR (DLSS motion vectors): where this pixel's surface was last frame, in render pixels, minus where it is now
// (DLSS's convention: current to previous). Both from unjittered viewports, so the vectors carry no jitter.
layout(location = 0) in vec4 c;
layout(location = 1) in vec4 p;
layout(location = 0) out vec2 mv;
layout(push_constant) uniform Viewports {
    vec4 cur;  // x, y, width, height of this frame's viewport (height negative when flipped)
    vec4 prev; // the same draw's viewport last frame
} vp;
void main() {
    vec2 nc = c.xy / c.w, np = p.xy / p.w;
    vec2 sc = vp.cur.xy + (nc * 0.5 + 0.5) * vp.cur.zw;
    vec2 sp = vp.prev.xy + (np * 0.5 + 0.5) * vp.prev.zw;
    mv = sp - sc;
}
