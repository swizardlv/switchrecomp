#version 450
// SwitchRecomp present pass (hdr.c): the finished frame (the game's own tone mapping, UI included), letterboxed into
// an image the size of the swapchain, written to the swapchain with
//  - FXAA (aa = 1): FXAA 3.11 quality on the display-encoded values, as FXAA expects;
//  - HDR (mode 1 scRGB, 2 HDR10 PQ): normal content at paper white, only the top of the range expanded smoothly toward
//    the display's peak, keeping each pixel's hue (the RGB ratio); mode 0 = SDR as it was.
// The image is read through a UNORM view: always the display-encoded bytes.
// SWR: binding 1 = the "Hold (-) to skip" prompt (premultiplied, composed on the CPU by gpu_gl.c skip_prompt_rgba),
// blended into the display-encoded picture before any HDR step, with its hold bar drawn here.
layout(set = 0, binding = 0) uniform sampler2D img;
layout(set = 0, binding = 1) uniform sampler2D ovl;
layout(push_constant) uniform PC {
    float paper;    // nits for SDR white
    float peak;     // nits the brightest highlight reaches
    float mode;     // 0 SDR, 1 scRGB, 2 HDR10 PQ
    float aa;       // 1: FXAA
    float out_srgb; // SDR to an sRGB swapchain: the hardware encodes, so hand it linear values
    float knee;     // where HDR expansion starts, in SDR linear (0..1)
    float ox, oy, ow, oh; // prompt rectangle in swapchain pixels (top left origin)
    float oa;             // prompt opacity, 0 = none
    float hold;           // hold progress 0..1
    float bx, by, bw, bh; // hold bar rectangle
    float sharp;          // SWR: sharpening 0 off .. 1 (contrast-adaptive, like AMD CAS)
} pc;
layout(location = 0) out vec4 o;

float L(vec2 p) { return dot(texture(img, p).rgb, vec3(0.299, 0.587, 0.114)); }
vec3 fxaa(vec2 uv) {
    vec2 rc = 1.0 / vec2(textureSize(img, 0));
    vec3 rgbM = texture(img, uv).rgb;
    float M = dot(rgbM, vec3(0.299, 0.587, 0.114));
    float N = L(uv + vec2(0, -rc.y)), S = L(uv + vec2(0, rc.y)), E = L(uv + vec2(rc.x, 0)), W = L(uv + vec2(-rc.x, 0));
    float mx = max(max(N, W), max(E, max(S, M))), mn = min(min(N, W), min(E, min(S, M))), range = mx - mn;
    if (range < max(0.0312, mx * 0.125)) return rgbM;
    float NW = L(uv + vec2(-rc.x, -rc.y)), NE = L(uv + vec2(rc.x, -rc.y)), SW = L(uv + vec2(-rc.x, rc.y)),
          SE = L(uv + vec2(rc.x, rc.y));
    float NS = N + S, WE = W + E, subNSWE = NS + WE, eH1 = -2.0 * M + NS, eV1 = -2.0 * M + WE, NESE = NE + SE,
          NWNE = NW + NE, eH2 = -2.0 * E + NESE, eV2 = -2.0 * N + NWNE;
    float NWSW = NW + SW, SWSE = SW + SE, eH4 = abs(eH1) * 2.0 + abs(eH2), eV4 = abs(eV1) * 2.0 + abs(eV2),
          eH3 = -2.0 * W + NWSW, eV3 = -2.0 * S + SWSE;
    float eH = abs(eH3) + eH4, eV = abs(eV3) + eV4;
    bool horz = eH >= eV;
    float len = horz ? rc.y : rc.x;
    float subA = subNSWE * 2.0 + NWSW + NESE;
    if (!horz) { N = W; S = E; }
    float subB = subA * (1.0 / 12.0) - M;
    float gN = N - M, gS = S - M, lNN = N + M, lSS = S + M;
    bool pairN = abs(gN) >= abs(gS);
    float grad = max(abs(gN), abs(gS));
    if (pairN) len = -len;
    float subC = clamp(abs(subB) / range, 0.0, 1.0);
    vec2 posB = uv;
    vec2 off = horz ? vec2(rc.x, 0.0) : vec2(0.0, rc.y);
    if (!horz) posB.x += len * 0.5; else posB.y += len * 0.5;
    vec2 pN = posB - off, pP = posB + off;
    float subD = -2.0 * subC + 3.0, subE = subC * subC;
    if (!pairN) lNN = lSS;
    float gsc = grad * 0.25, MM = M - lNN * 0.5, subF = subD * subE;
    bool mLT0 = MM < 0.0;
    float eN = L(pN) - lNN * 0.5, eP = L(pP) - lNN * 0.5;
    bool dN = abs(eN) >= gsc, dP = abs(eP) >= gsc;
    const float st[11] = float[](1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0, 8.0, 8.0, 8.0, 8.0);
    for (int i = 0; i < 11 && !(dN && dP); i++) {
        if (!dN) { pN -= off * st[i]; eN = L(pN) - lNN * 0.5; dN = abs(eN) >= gsc; }
        if (!dP) { pP += off * st[i]; eP = L(pP) - lNN * 0.5; dP = abs(eP) >= gsc; }
    }
    float dstN = horz ? uv.x - pN.x : uv.y - pN.y, dstP = horz ? pP.x - uv.x : pP.y - uv.y;
    bool goodN = (eN < 0.0) != mLT0, goodP = (eP < 0.0) != mLT0;
    float spanLen = dstP + dstN;
    bool dirN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool good = dirN ? goodN : goodP;
    float subG = subF * subF, pxOff = dst * (-1.0 / spanLen) + 0.5, subH = subG * 0.75;
    float o2 = max(good ? pxOff : 0.0, subH);
    vec2 p = uv;
    if (!horz) p.x += o2 * len; else p.y += o2 * len;
    return texture(img, p).rgb;
}

// each pixel minus a share of its four neighbours, scaled down where the neighbourhood is near black or white
vec3 cas(vec3 c, vec3 n, vec3 s, vec3 e, vec3 w, float k) {
    vec3 mn = min(c, min(min(n, s), min(e, w))), mx = max(c, max(max(n, s), max(e, w)));
    vec3 a = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
    vec3 wt = -a * mix(0.125, 0.2, k);
    return clamp((c + (n + s + e + w) * wt) / (1.0 + 4.0 * wt), 0.0, 1.0);
}

vec3 srgb_to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}
vec3 pq(vec3 nits) {
    vec3 l = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(0.1593017578125));
    return pow((0.8359375 + 18.8515625 * l) / (1.0 + 18.6875 * l), vec3(78.84375));
}

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(img, 0));
    vec3 c = pc.aa > 0.5 ? fxaa(uv) : texelFetch(img, ivec2(gl_FragCoord.xy), 0).rgb;
    if (pc.sharp > 0.0) {
        ivec2 q = ivec2(gl_FragCoord.xy), m = textureSize(img, 0) - 1;
        c = cas(c, texelFetch(img, clamp(q + ivec2(0, -1), ivec2(0), m), 0).rgb, texelFetch(img, clamp(q + ivec2(0, 1), ivec2(0), m), 0).rgb,
                texelFetch(img, clamp(q + ivec2(1, 0), ivec2(0), m), 0).rgb, texelFetch(img, clamp(q + ivec2(-1, 0), ivec2(0), m), 0).rgb, pc.sharp);
    }
    if (pc.oa > 0.0) {
        vec2 f = gl_FragCoord.xy;
        if (f.x >= pc.ox && f.y >= pc.oy && f.x < pc.ox + pc.ow && f.y < pc.oy + pc.oh) {
            vec4 v = texture(ovl, (f - vec2(pc.ox, pc.oy)) / vec2(pc.ow, pc.oh)) * pc.oa;
            c = c * (1.0 - v.a) + v.rgb;
        }
        if (pc.hold > 0.0 && f.x >= pc.bx && f.y >= pc.by && f.x < pc.bx + pc.bw && f.y < pc.by + pc.bh)
            c = f.x < pc.bx + pc.bw * pc.hold ? mix(c, vec3(1.0), pc.oa) : mix(c, vec3(0.0), 0.5 * pc.oa);
    }
    if (pc.mode < 0.5) { // SDR
        o = vec4(pc.out_srgb > 0.5 ? srgb_to_linear(c) : c, 1.0);
        return;
    }
    c = max(srgb_to_linear(c), vec3(0.0));
    // expansion on the brightest channel: f(y) = y below the knee, then y + (r-1)((y-k)/(1-k))^2, so f(k) = k with a
    // matching slope and f(1) = r = peak/paper; monotonic for r >= 1
    float y = max(c.r, max(c.g, c.b));
    float r = max(pc.peak / pc.paper, 1.0), k = clamp(pc.knee, 0.0, 0.99);
    float t = clamp((y - k) / (1.0 - k), 0.0, 1.0);
    float f = y + (r - 1.0) * t * t;
    vec3 nits = c * (y > 1e-5 ? f / y : 1.0) * pc.paper;
    if (pc.mode > 1.5) {
        const mat3 to2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956);
        o = vec4(pq(to2020 * nits), 1.0);
    } else
        o = vec4(nits / 80.0, 1.0);
}
