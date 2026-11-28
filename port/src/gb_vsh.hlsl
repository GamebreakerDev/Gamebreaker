// The NV2A vertex program interpreter as a Direct3D 11 vertex shader.
// A transcription of the toolkit's CPU interpreter (nv2a_vsh_interp.c,
// vsh_run_fields) and of what the executor and the back end do to a vertex
// after it (transform_vertex, backend_from_xf, gb_draw): same reads, same
// writes, same order of arithmetic. Embedded in the program by CMake.
cbuffer VP : register(b1) { uint4 prog[136]; };     // y, z, w: dwords 1, 2, 3 of an instruction
cbuffer VK : register(b2) { float4 kc[192]; };      // c[0..191]
cbuffer VB : register(b3) {
    float4 imm[16];       // current values of attributes without an array
    float4 tscale[4];     // x, y: scale of each stage's s and t
    float4 pscale;        // surface scale x, y; 2 / target width, 2 / target height
    float4 pz;            // 1 / 16777215; near plane (c[59].z); fog parameters 0, 1
    uint4  vf;            // start slot; mask of attributes that are arrays; fog enable; fog mode
};
struct VI {
    float4 a0 : ATTR0;   float4 a1 : ATTR1;   float4 a2 : ATTR2;   float4 a3 : ATTR3;
    float4 a4 : ATTR4;   float4 a5 : ATTR5;   float4 a6 : ATTR6;   float4 a7 : ATTR7;
    float4 a8 : ATTR8;   float4 a9 : ATTR9;   float4 a10 : ATTR10; float4 a11 : ATTR11;
    float4 a12 : ATTR12; float4 a13 : ATTR13; float4 a14 : ATTR14; float4 a15 : ATTR15;
};
struct VO {
    float4 pos : SV_Position; float4 col : COLOR0; float4 spec : COLOR1; float fog : FOG;
    float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1; float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3;
    float2 clip : SV_ClipDistance0;
};

#define F_INF  asfloat(0x7F800000u)
#define RCC_LO 5.42101e-20
#define RCC_HI 1.884467e19

float4 swz(float4 s, uint sw, uint neg)
{
    float4 r = float4(s[(sw >> 6) & 3u], s[(sw >> 4) & 3u], s[(sw >> 2) & 3u], s[sw & 3u]);
    return neg != 0u ? -r : r;
}

float4 wm(float4 d, float4 s, uint m)
{
    return float4((m & 8u) != 0u ? s.x : d.x, (m & 4u) != 0u ? s.y : d.y,
                  (m & 2u) != 0u ? s.z : d.z, (m & 1u) != 0u ? s.w : d.w);
}

float4 clamp01(float4 c)
{
    return float4(c.x > 1.0 ? 1.0 : (c.x >= 0.0 ? c.x : 0.0), c.y > 1.0 ? 1.0 : (c.y >= 0.0 ? c.y : 0.0),
                  c.z > 1.0 ? 1.0 : (c.z >= 0.0 ? c.z : 0.0), c.w > 1.0 ? 1.0 : (c.w >= 0.0 ? c.w : 0.0));
}

float fogf(float d)
{
    if (vf.z == 0u)
        return 1.0;
    uint mode = vf.w;
    float px = pz.z, py = pz.w;
    precise float f;
    if (mode == 0x800u || mode == 0x802u) {
        precise float e = d * py; e = e * 16.0;
        f = px + exp2(e); f = f - 1.5;
    } else if (mode == 0x801u || mode == 0x803u) {
        precise float e = -d * d; e = e * py; e = e * py; e = e * 32.0;
        f = px + exp2(e); f = f - 1.5;
    } else {
        precise float e = d * py;
        f = px + e; f = f - 1.0;
    }
    if (mode == 0x802u || mode == 0x803u || mode == 0x804u)
        f = abs(f);
    if (f != f)
        f = 1.0;
    return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
}

VO vsp(VI i)
{
    float4 vin[16] = { i.a0, i.a1, i.a2, i.a3, i.a4, i.a5, i.a6, i.a7,
                       i.a8, i.a9, i.a10, i.a11, i.a12, i.a13, i.a14, i.a15 };
    float4 R[13];             // R0..R11, then oPos
    float4 O[13];             // the output registers, by address
    uint q;
    for (q = 0u; q < 16u; q++)
        if (((vf.y >> q) & 1u) == 0u)
            vin[q] = imm[q];
    for (q = 0u; q < 13u; q++) {
        R[q] = float4(0.0, 0.0, 0.0, 0.0);
        O[q] = float4(0.0, 0.0, 0.0, 0.0);
    }
    O[3].w = 1.0; O[4].w = 1.0;
    O[9].w = 1.0; O[10].w = 1.0; O[11].w = 1.0; O[12].w = 1.0;
    int a0 = 0;

    [loop] for (uint s = vf.x; s < 136u; s++) {
        uint w1 = prog[s].y, w2 = prog[s].z, w3 = prog[s].w;
        uint mac = (w1 >> 21) & 15u, ilu = (w1 >> 25) & 7u;
        int ci = (int)((w1 >> 13) & 255u);
        if (((w3 >> 1) & 1u) != 0u)
            ci += a0;
        if (ci < 0 || ci >= 192)
            ci = 0;
        float4 kv = kc[ci];
        float4 iv = vin[(w1 >> 9) & 15u];
        uint ma = (w2 >> 26) & 3u, ra = (w2 >> 28) & 15u;
        uint mb = (w2 >> 11) & 3u, rb = (w2 >> 13) & 15u;
        uint mc = (w3 >> 28) & 3u, rc = ((w2 & 3u) << 2) | ((w3 >> 30) & 3u);
        float4 a = swz(ma == 1u ? R[ra <= 12u ? ra : 0u] : (ma == 2u ? iv : kv), w1 & 255u, (w1 >> 8) & 1u);
        float4 b = swz(mb == 1u ? R[rb <= 12u ? rb : 0u] : (mb == 2u ? iv : kv), (w2 >> 17) & 255u, (w2 >> 25) & 1u);
        float4 c = swz(mc == 1u ? R[rc <= 12u ? rc : 0u] : (mc == 2u ? iv : kv), (w2 >> 2) & 255u, (w2 >> 10) & 1u);
        precise float4 mres = float4(0.0, 0.0, 0.0, 0.0);
        precise float4 ires = float4(0.0, 0.0, 0.0, 0.0);

        if (mac != 0u) {
            mres = a;
            if (mac == 2u) {
                mres = a * b;
            } else if (mac == 3u) {
                mres = a + c;
            } else if (mac == 4u) {
                precise float4 m = a * b;
                mres = m + c;
            } else if (mac >= 5u && mac <= 7u) {
                precise float p0 = a.x * b.x;
                precise float p1 = a.y * b.y;
                precise float p2 = a.z * b.z;
                precise float d = p0 + p1;
                d = d + p2;
                if (mac == 6u) {
                    d = d + b.w;
                } else if (mac == 7u) {
                    precise float p3 = a.w * b.w;
                    d = d + p3;
                }
                mres = float4(d, d, d, d);
            } else if (mac == 8u) {
                precise float y = a.y * b.y;
                mres = float4(1.0, y, a.z, b.w);
            } else if (mac == 9u) {
                mres = float4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w);
            } else if (mac == 10u) {
                mres = float4(a.x >= b.x ? a.x : b.x, a.y >= b.y ? a.y : b.y, a.z >= b.z ? a.z : b.z, a.w >= b.w ? a.w : b.w);
            } else if (mac == 11u) {
                mres = float4(a.x < b.x ? 1.0 : 0.0, a.y < b.y ? 1.0 : 0.0, a.z < b.z ? 1.0 : 0.0, a.w < b.w ? 1.0 : 0.0);
            } else if (mac == 12u) {
                mres = float4(a.x >= b.x ? 1.0 : 0.0, a.y >= b.y ? 1.0 : 0.0, a.z >= b.z ? 1.0 : 0.0, a.w >= b.w ? 1.0 : 0.0);
            } else if (mac == 13u) {
                precise float t = a.x + 0.001;
                a0 = (int)floor(t);
            }
        }
        if (ilu != 0u) {
            float x = c.x;
            ires = c;
            if (ilu == 2u) {
                precise float r = 1.0 / x;
                if (x == 0.0)
                    r = F_INF;
                ires = float4(r, r, r, r);
            } else if (ilu == 3u) {
                precise float f = 1.0 / x;
                if (x == 0.0) {
                    f = (asuint(x) >> 31) != 0u ? -RCC_HI : RCC_HI;
                } else {
                    if (abs(f) < RCC_LO) f = f < 0.0 ? -RCC_LO : RCC_LO;
                    if (abs(f) > RCC_HI) f = f < 0.0 ? -RCC_HI : RCC_HI;
                }
                ires = float4(f, f, f, f);
            } else if (ilu == 4u) {
                precise float sq = sqrt(abs(x));
                precise float r = 1.0 / sq;
                ires = float4(r, r, r, r);
            } else if (ilu == 5u) {
                float fl = floor(x);
                ires = float4(exp2(fl), x - fl, exp2(x), 1.0);
            } else if (ilu == 6u) {
                float ax = abs(x);
                if (ax == 0.0) {
                    ires = float4(-F_INF, 1.0, -F_INF, 1.0);
                } else {
                    float e = floor(log2(ax));
                    ires = float4(e, ax / exp2(e), log2(ax), 1.0);
                }
            } else if (ilu == 7u) {
                float nl = c.x > 0.0 ? c.x : 0.0;
                float nh = c.y > 0.0 ? c.y : 0.0;
                float p = c.w < -127.9961 ? -127.9961 : (c.w > 127.9961 ? 127.9961 : c.w);
                float pw;
                if (nh == 0.0)
                    pw = p > 0.0 ? 0.0 : (p == 0.0 ? 1.0 : F_INF);
                else
                    pw = exp2(p * log2(nh));
                ires = float4(1.0, nl, c.x > 0.0 ? pw : 0.0, 1.0);
            }
        }

        uint tdst = (w3 >> 20) & 15u;
        uint mmask = (w3 >> 24) & 15u, imask = (w3 >> 16) & 15u;
        if (mac != 0u && mac != 13u && mmask != 0u && tdst <= 12u)
            R[tdst] = wm(R[tdst], mres, mmask);
        if (ilu != 0u && imask != 0u) {
            uint it = mac != 0u ? 1u : tdst;      // paired with a MAC op the ILU writes R1
            if (it <= 12u)
                R[it] = wm(R[it], ires, imask);
        }
        uint omask = (w3 >> 12) & 15u;
        if (omask != 0u && ((w3 >> 11) & 1u) != 0u) {
            float4 src = ((w3 >> 2) & 1u) != 0u ? ires : mres;
            uint oa = (w3 >> 3) & 255u;
            if (oa == 0u)
                R[12] = wm(R[12], src, omask);
            else if (oa < 13u)
                O[oa] = wm(O[oa], src, omask);
        }
        if ((w3 & 1u) != 0u)
            break;
    }

    VO o;
    float4 p = R[12];
    if (asuint(p.w) == 0u)        // +0: a program that never divided by w
        p.w = 1.0;
    float w = p.w;
    precise float px = p.x * pscale.x;
    precise float py = p.y * pscale.y;
    precise float tx = px * pscale.z;
    tx = tx - 1.0;
    precise float ty = py * pscale.w;
    ty = 1.0 - ty;
    precise float tz = p.z * pz.x;
    precise float cx = tx * w;
    precise float cy = ty * w;
    precise float cz = tz * w;
    o.pos = float4(cx, cy, cz, w);
    // The two planes the executor clips at: w = 0.001 and the near plane.
    o.clip = float2(w - 0.001, (p.z - pz.y) * w);

    float4 d0 = clamp01(O[3]);
    o.col = floor(d0 * 255.0 + 0.5) / 255.0;        // the 8 bits the executor packs diffuse into
    o.spec = clamp01(O[4]);
    o.fog = fogf(O[5].x);
    o.tc0 = float4(O[9].x * tscale[0].x, O[9].y * tscale[0].y, O[9].z, O[9].w);
    o.tc1 = float4(O[10].x * tscale[1].x, O[10].y * tscale[1].y, O[10].z, O[10].w);
    o.tc2 = float4(O[11].x * tscale[2].x, O[11].y * tscale[2].y, O[11].z, O[11].w);
    o.tc3 = float4(O[12].x * tscale[3].x, O[12].y * tscale[3].y, O[12].z, O[12].w);
    return o;
}
