// Present pass: exposure + AgX tone mapping (the same view transform the Blender kit
// renders use), then the HUD: crosshair, minimap with the player, optional big map.

cbuffer Present : register(b0)
{
    float4 p0;  // x exposure, y hud (1 = on), z time, w exit flash 0..1
    float4 p1;  // x width, y height, z map aspect (w / h), w big map opacity
    float4 p2;  // x player map u, y player map v, z player yaw, w minimap on
    float4 p3;  // x despeckle k (0 = off)
    float4 p4;  // map grid: x,y margin (uv), z,w one cell (uv)
    float4 p5;  // x grid w, y grid h, z hide unexplored (1 = on), w minimap half-width in cells (0 = whole map)
};

Texture2D<float4> gAccum : register(t0);
Texture2D<float4> gMap   : register(t1);
StructuredBuffer<uint> gExplored : register(t2);  // per cell, non-zero = seen
SamplerState gLinear : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

// Minimal AgX (Troy Sobotka's AgX, polynomial fit by Benjamin Wrensch).
float3 agxContrast(float3 x)
{
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

float3 agx(float3 v)
{
    const float3x3 m = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                0.0784335999999992, 0.878468636469772, 0.0784336,
                                0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 mi = float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                                 -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                                 -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    v = mul(max(v, 1e-10), m);
    v = clamp(log2(v), minEv, maxEv);
    v = (v - minEv) / (maxEv - minEv);
    v = agxContrast(v);
    // a touch more contrast, like Blender's "Medium High Contrast" look
    v = lerp(v, smoothstep(0.0, 1.0, v), 0.25);
    return saturate(mul(v, mi));
}

float sdBox(float2 p, float2 b)
{
    float2 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}

bool seen(float2 uv)
{
    if (p5.z < 0.5) return true;
    int2 c = int2(floor((uv - p4.xy) / p4.zw));
    c = clamp(c, int2(0, 0), int2(p5.xy) - 1);
    return gExplored[c.y * int(p5.x) + c.x] != 0;
}

float3 mapColor(float2 uv, float2 duv)
{
    if (any(uv < 0.0) || any(uv > 1.0)) return float3(0.01, 0.015, 0.02);
    if (!seen(uv)) return float3(0.018, 0.022, 0.028);
    // 4 taps: the big map shrinks the image, and it has no mip levels
    float3 m = gMap.SampleLevel(gLinear, uv + duv * float2(-0.25, -0.25), 0).rgb;
    m += gMap.SampleLevel(gLinear, uv + duv * float2(0.25, -0.25), 0).rgb;
    m += gMap.SampleLevel(gLinear, uv + duv * float2(-0.25, 0.25), 0).rgb;
    m += gMap.SampleLevel(gLinear, uv + duv * float2(0.25, 0.25), 0).rgb;
    return m * 0.25;
}

// Draws the map into rect (origin, size in pixels), showing the map image around uvC
// (uvHalf either side); returns colour, alpha.
float4 drawMap(float2 px, float2 org, float2 size, float opacity, float2 uvC, float2 uvHalf)
{
    float2 local = (px - org) / size;
    float2 uv = uvC + (local - 0.5) * 2.0 * uvHalf;
    float4 o = 0;
    float2 c = org + size * 0.5;
    float d = sdBox(px - c, size * 0.5 + 6.0);
    if (d < 0.0)
    {
        o = float4(0.01, 0.015, 0.02, 0.8);
        if (all(local >= 0.0) && all(local <= 1.0))
            o = float4(mapColor(uv, 2.0 * uvHalf / size), 0.95);
    }
    float border = 1.0 - smoothstep(0.5, 1.8, abs(d));
    o.rgb = lerp(o.rgb, float3(0.25, 0.82, 0.88), border);
    o.a = max(o.a, border);
    // player: dot plus heading tick
    float2 pp = org + ((p2.xy - uvC) / (2.0 * uvHalf) + 0.5) * size;
    float2 h = float2(-sin(p2.z), -cos(p2.z));
    float r = max(4.0, size.y * 0.012);
    float dd = length(px - pp);
    float2 rel = px - pp;
    float along = dot(rel, h);
    float side = abs(dot(rel, float2(-h.y, h.x)));
    float tick = (along > 0.0 && along < r * 3.2 && side < r * 0.45) ? 1.0 : 0.0;
    float dotA = 1.0 - smoothstep(r - 1.0, r + 1.0, dd);
    float m = max(dotA, tick);
    o.rgb = lerp(o.rgb, float3(1.0, 0.62, 0.22), m);
    o.a = max(o.a, m);
    o.a *= opacity;
    return o;
}

float4 PSMain(VSOut input) : SV_Target
{
    float2 px = input.pos.xy;
    float2 res = p1.xy;
    int2 ip = int2(px);
    float3 hdr = gAccum.Load(int3(ip, 0)).rgb;
    if (p3.x > 0.0)
    {
        // despeckle: clamp this pixel's luminance to its neighbours' mean +- k sigma, which
        // removes isolated noisy dots but leaves real edges (high sigma there) alone
        float sum = 0.0, sum2 = 0.0;
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0) continue;
            int2 q = clamp(ip + int2(x, y), int2(0, 0), int2(res) - 1);
            float l = dot(gAccum.Load(int3(q, 0)).rgb, float3(0.2126, 0.7152, 0.0722));
            sum += l;
            sum2 += l * l;
        }
        float mean = sum / 8.0;
        float sigma = sqrt(max(sum2 / 8.0 - mean * mean, 0.0));
        float l = dot(hdr, float3(0.2126, 0.7152, 0.0722));
        float lc = clamp(l, mean - p3.x * sigma, mean + p3.x * sigma);
        if (l > 1e-5) hdr *= lc / l;
        else hdr = lc;
    }
    hdr *= p0.x;
    float3 col = agx(hdr);

    // exit reached: fade toward white
    col = lerp(col, float3(1.0, 1.0, 1.0), p0.w * 0.55);

    if (p0.y > 0.5)
    {
        // crosshair
        float2 c = px - res * 0.5;
        float s = max(1.0, res.y / 720.0);
        float ring = abs(length(c) - 3.0 * s);
        float cross_ = (abs(c.x) < 0.75 * s && abs(c.y) > 6.0 * s && abs(c.y) < 13.0 * s) ||
                       (abs(c.y) < 0.75 * s && abs(c.x) > 6.0 * s && abs(c.x) < 13.0 * s) ? 1.0 : 0.0;
        float a = max(cross_, 1.0 - smoothstep(0.4 * s, 1.2 * s, ring));
        col = lerp(col, float3(0.85, 0.97, 1.0), a * 0.85);

        // minimap, top right
        if (p2.w > 0.5)
        {
            float h = res.y * 0.26;
            float4 m;
            if (p5.w > 0.0)
            {
                // zoomed: a square of cells around the player, north up
                float2 size = float2(h, h);
                float2 org = float2(res.x - size.x - res.y * 0.03, res.y * 0.03);
                m = drawMap(px, org, size, 0.92, p2.xy, p5.w * p4.zw);
            }
            else
            {
                float2 size = float2(h * p1.z, h);
                float2 org = float2(res.x - size.x - res.y * 0.03, res.y * 0.03);
                m = drawMap(px, org, size, 0.92, float2(0.5, 0.5), float2(0.5, 0.5));
            }
            col = lerp(col, m.rgb, m.a);
        }
    }
    if (p1.w > 0.0)
    {
        float h = res.y * 0.82;
        float2 size = float2(h * p1.z, h);
        float2 org = (res - size) * 0.5;
        float4 m = drawMap(px, org, size, p1.w, float2(0.5, 0.5), float2(0.5, 0.5));
        col = lerp(col * (1.0 - 0.5 * p1.w), m.rgb, m.a);
    }
    return float4(col, 1.0);
}
