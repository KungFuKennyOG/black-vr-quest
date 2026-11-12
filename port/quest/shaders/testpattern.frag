#version 450
// BLACK VR smoke-test scene, ray-cast per pixel in OpenXR space (metres,
// +y up). Shows head tracking (world-locked floor grid + pillars), stereo
// (a cube 1.5 m ahead at eye height and the pillars' parallax) and both
// controllers (spheres + beams; a beam turns white when its trigger is held).
// Output values are display-referred (gamma); written through a UNORM view
// of the sRGB swapchain image.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(push_constant) uniform PC {
    vec4 c0;   // xyz: world ray dir at the bottom-left corner, w: time (s)
    vec4 cx;   // xyz: dir delta across x,                     w: mode (0 scene, 1 scene + red sky)
    vec4 cy;   // xyz: dir delta across y (bottom to top),     w: floor y
    vec4 eye;  // xyz: eye position,                            w: pad buttons (bits)
    vec4 h0o;  // left hand aim origin,  w: valid
    vec4 h0d;  // left hand aim dir,     w: trigger 0..1
    vec4 h1o;  // right hand aim origin, w: valid
    vec4 h1d;  // right hand aim dir,    w: trigger 0..1
} pc;

const float FAR = 1e9;

float plane_y(vec3 ro, vec3 rd, float y) { float t = (y - ro.y) / rd.y; return (t > 0.0) ? t : FAR; }

float sphere(vec3 ro, vec3 rd, vec3 c, float r)
{
    vec3 oc = ro - c; float b = dot(oc, rd); float h = b * b - dot(oc, oc) + r * r;
    if (h < 0.0) return FAR;
    float t = -b - sqrt(h);
    return t > 0.0 ? t : FAR;
}

// axis-aligned box, returns t and normal
float box(vec3 ro, vec3 rd, vec3 lo, vec3 hi, out vec3 n)
{
    vec3 inv = 1.0 / rd;
    vec3 t0 = (lo - ro) * inv, t1 = (hi - ro) * inv;
    vec3 tmin = min(t0, t1), tmax = max(t0, t1);
    float tn = max(max(tmin.x, tmin.y), tmin.z), tf = min(min(tmax.x, tmax.y), tmax.z);
    if (tn > tf || tf < 0.0 || tn < 0.0) return FAR;
    n = -sign(rd) * step(tmin.yzx, tmin.xyz) * step(tmin.zxy, tmin.xyz);
    return tn;
}

// capsule-ish beam: distance from ray to segment, approximated by sampling
float beam(vec3 ro, vec3 rd, vec3 a, vec3 d, float len, float r)
{
    vec3 w = ro - a;
    float b = dot(rd, d), dd = dot(rd, w), e = dot(d, w);
    float den = 1.0 - b * b;
    if (den < 1e-5) return FAR;
    float s = (b * e - dd) / den, u = (e - b * dd) / den;
    u = clamp(u, 0.0, len);
    s = max(dot(a + d * u - ro, rd), 0.0);
    vec3 p = ro + rd * s, q = a + d * u;
    return length(p - q) < r ? s : FAR;
}

vec3 hsv(float h) { return clamp(abs(fract(h + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0); }

void main()
{
    vec3 rd = normalize(pc.c0.xyz + pc.cx.xyz * uv.x + pc.cy.xyz * (1.0 - uv.y));
    vec3 ro = pc.eye.xyz;
    float floor_y = pc.cy.w;
    uint buttons = uint(pc.eye.w + 0.5);

    // sky
    vec3 col = mix(vec3(0.55, 0.62, 0.70), vec3(0.10, 0.16, 0.32), clamp(rd.y * 1.5 + 0.2, 0.0, 1.0));
    if (pc.cx.w > 0.5) col = mix(col, vec3(0.45, 0.05, 0.05), 0.6);   // game files missing
    float best = FAR;

    // floor with a 1 m grid and coloured axes
    float tf = plane_y(ro, rd, floor_y);
    if (tf < best && tf < 200.0) {
        best = tf;
        vec3 p = ro + rd * tf;
        vec2 g = abs(fract(p.xz + 0.5) - 0.5);
        float lw = 0.012 + 0.002 * tf;
        float line = 1.0 - smoothstep(lw, lw * 1.8, min(g.x, g.y));
        col = mix(vec3(0.18, 0.20, 0.22), vec3(0.85), line * 0.8);
        if (abs(p.z) < lw * 2.0) col = vec3(0.9, 0.15, 0.1);   // x axis
        if (abs(p.x) < lw * 2.0) col = vec3(0.1, 0.35, 0.95);  // z axis (forward is -z)
        col = mix(col, vec3(0.55, 0.62, 0.70), smoothstep(20.0, 120.0, tf));
    }

    // ring of 12 pillars, radius 3 m
    for (int i = 0; i < 12; i++) {
        float a = float(i) * 6.2831853 / 12.0;
        vec3 c = vec3(sin(a) * 3.0, floor_y, -cos(a) * 3.0);
        vec3 n;
        float t = box(ro, rd, c + vec3(-0.15, 0.0, -0.15), c + vec3(0.15, 2.2, 0.15), n);
        if (t < best) {
            best = t;
            vec3 base = (i == 0) ? vec3(1.0, 0.85, 0.1) : hsv(float(i) / 12.0) * 0.8 + 0.1;
            col = base * (0.45 + 0.55 * max(dot(n, normalize(vec3(0.4, 0.8, 0.3))), 0.0));
        }
    }

    // stereo cube 1.5 m ahead at eye height (0.25 m), spinning slowly
    {
        float s = sin(pc.c0.w * 0.5), co = cos(pc.c0.w * 0.5);
        vec3 c = vec3(0.0, floor_y + 1.5, -1.5);
        vec3 lo = ro - c, ld = rd;
        lo.xz = mat2(co, -s, s, co) * lo.xz; ld.xz = mat2(co, -s, s, co) * ld.xz;
        vec3 n;
        float t = box(lo, ld, vec3(-0.125), vec3(0.125), n);
        if (t < best) {
            best = t;
            vec3 base = vec3(0.2, 0.9, 0.4);
            if ((buttons & 0x1000u) != 0u) base = vec3(0.1, 0.9, 0.1);  // A
            if ((buttons & 0x2000u) != 0u) base = vec3(0.9, 0.1, 0.1);  // B
            if ((buttons & 0x4000u) != 0u) base = vec3(0.1, 0.3, 0.9);  // X
            if ((buttons & 0x8000u) != 0u) base = vec3(0.9, 0.9, 0.1);  // Y
            col = base * (0.4 + 0.6 * abs(n.x * 0.5 + n.y * 0.8 + n.z * 0.3));
        }
    }

    // controllers
    for (int h = 0; h < 2; h++) {
        vec4 o = (h == 0) ? pc.h0o : pc.h1o;
        vec4 d = (h == 0) ? pc.h0d : pc.h1d;
        if (o.w < 0.5) continue;
        float t = sphere(ro, rd, o.xyz, 0.035);
        if (t < best) { best = t; col = (h == 0) ? vec3(0.2, 0.5, 1.0) : vec3(1.0, 0.5, 0.2); }
        t = beam(ro, rd, o.xyz, normalize(d.xyz), 2.0, 0.006);
        if (t < best) { best = t; col = mix((h == 0) ? vec3(0.2, 0.5, 1.0) : vec3(1.0, 0.5, 0.2), vec3(1.0), d.w); }
    }
    color = vec4(col, 1.0);
}
