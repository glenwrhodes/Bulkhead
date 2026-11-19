// Bulkhead path tracer (inline ray tracing, DXR 1.1 RayQuery, cs_6_5).
//
// Per pixel and frame:
//   primary ray (jittered)  -> visibility, glass passes through with a tint
//   direct light            -> every fixture light in the 3x3 surrounding cells, with a
//                              shadow ray to a random point on the light (soft shadows)
//   one diffuse bounce      -> cosine-sampled ray, lit by the hit's direct light and emission
//   glossy reflection       -> one ray around the mirror direction, rough surfaces skip it
// Results accumulate over frames: temporal reprojection in the window, a fixed-camera
// average in captures. With RT off: unshadowed direct light plus a flat ambient term.
//
// Round 5 (mood):
//   lights          radiance comes per frame (flicker, dead, emergency); emissive surfaces
//                   are scaled per instance by the power of their cell
//   wear            procedural grime, rust streaks and dull patches, from each cell's decay
//   flashlight      a shadowed spot light under the gun barrel
//   haze            single scattering along the primary ray: one fixture sample (uniform
//                   distance) and one flashlight sample (equiangular), both shadowed

// Round 6-7: short-lived lights (spark bursts, the muzzle flash, bolts, impacts: every
// pixel considers them, shadowed on primary hits) and steam plumes (ray-marched cones
// with animated turbulence, lit by the fixtures around them, the flashlight and the
// short-lived lights). Sparks and bolts have their own mask: seen directly, ignored by
// shadow and bounce rays.

#define MASK_WORLD 0x01
#define MASK_GUN   0x02
#define MASK_GLASS 0x04
#define MASK_FX    0x08
#define FLAG_GUN   1u
#define PI 3.14159265

struct Vertex { float3 pos; float3 nrm; };
struct Submesh { uint firstIndex; uint baseVertex; uint material; uint pad; };
struct InstInfo { uint firstSubmesh; uint flags; uint pad0; uint pad1; float4 emissive; };
struct Material { float4 baseAlpha; float4 emissiveRough; float4 metalPad; };
struct Light { float4 posRadius; float4 radianceRange; };

cbuffer Frame : register(b0)
{
    float4 camPos;      // xyz, w = tan(fovX/2)
    float4 camRight;    // w = tan(fovY/2)
    float4 camUp;
    float4 camFwd;
    float4 prevPos;     // previous frame camera, same layout
    float4 prevRight;
    float4 prevUp;
    float4 prevFwd;
    float4 resolution;  // w, h, 1/w, 1/h
    float4 jitter;      // xy subpixel jitter (pixels), z frame seed, w history mode
    float4 grid;        // grid w, grid h, cell size, light count
    float4 options;     // x RT on, y ambient, z max history, w short-lived light count
    float4 flashPos;    // xyz position, w intensity (W/sr, 0 = off)
    float4 flashDir;    // xyz direction, w cos(outer cone)
    float4 flashCone;   // x cos(inner cone), y cos(spill edge), z range, w steam plume count
    float4 hazeParams;  // x density multiplier, y HG g, z time, w lights picked per pixel (0: all)
};

struct Steam { float4 originLength; float4 dirStrength; float4 lightSeed; };

RaytracingAccelerationStructure gScene : register(t0);
StructuredBuffer<Vertex>   gVerts      : register(t1);
StructuredBuffer<uint>     gIndices    : register(t2);
StructuredBuffer<Submesh>  gSubmeshes  : register(t3);
StructuredBuffer<InstInfo> gInst       : register(t4);
StructuredBuffer<Material> gMaterials  : register(t5);
StructuredBuffer<Light>    gLights     : register(t6);
StructuredBuffer<uint2>    gCells      : register(t7);
StructuredBuffer<uint>     gCellLights : register(t8);
Texture2D<float4>          gPrevAccum  : register(t9);
Texture2D<float4>          gPrevPos    : register(t10);
RWTexture2D<float4>        gAccum      : register(u0);
RWTexture2D<float4>        gPos        : register(u1);
StructuredBuffer<float4>   gCellInfo   : register(t11);  // per cell: decay, haze, mood
StructuredBuffer<Light>    gDynLights  : register(t12);  // short-lived lights
StructuredBuffer<Steam>    gSteam      : register(t13);  // steam plumes

// ---------------------------------------------------------------------------
// Random numbers
// ---------------------------------------------------------------------------
uint pcg(inout uint s)
{
    s = s * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
float rnd(inout uint s) { return float(pcg(s) >> 8) * (1.0 / 16777216.0); }

void onb(float3 n, out float3 t, out float3 b)
{
    float s = n.z >= 0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}
float3 cosineSample(float3 n, inout uint s)
{
    float u = rnd(s), v = rnd(s);
    float r = sqrt(u), phi = 2.0 * PI * v;
    float3 t, b;
    onb(n, t, b);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u)));
}
float3 sphereSample(inout uint s)
{
    float z = rnd(s) * 2.0 - 1.0, phi = 2.0 * PI * rnd(s);
    float r = sqrt(max(0.0, 1.0 - z * z));
    return float3(r * cos(phi), r * sin(phi), z) * pow(rnd(s), 0.3333);
}

// ---------------------------------------------------------------------------
// Ray queries
// ---------------------------------------------------------------------------
struct Hit
{
    bool hit;
    bool front;
    float t;
    float3 P;
    float3 N;
    uint material;
    uint instance;
    uint flags;
    float3 emis;  // instance emissive multiplier
};

Hit makeHit(float3 o, float3 d, float t, uint inst, uint geom, uint prim, float2 bary, float3x4 o2w)
{
    Hit h;
    h.hit = true;
    h.t = t;
    h.P = o + d * t;
    h.instance = inst;
    InstInfo ii = gInst[inst];
    h.flags = ii.flags;
    h.emis = ii.emissive.rgb;
    Submesh sm = gSubmeshes[ii.firstSubmesh + geom];
    h.material = sm.material;
    uint base = sm.firstIndex + prim * 3;
    Vertex v0 = gVerts[sm.baseVertex + gIndices[base]];
    Vertex v1 = gVerts[sm.baseVertex + gIndices[base + 1]];
    Vertex v2 = gVerts[sm.baseVertex + gIndices[base + 2]];
    float3 n = v0.nrm * (1.0 - bary.x - bary.y) + v1.nrm * bary.x + v2.nrm * bary.y;
    float3 ng = cross(v1.pos - v0.pos, v2.pos - v0.pos);
    float3x3 r = (float3x3)o2w;
    h.N = normalize(mul(r, n));
    ng = normalize(mul(r, ng));
    h.front = dot(ng, d) < 0.0;
    if (dot(h.N, d) > 0.0) h.N = -h.N;  // shade the side the ray sees
    return h;
}

Hit missHit()
{
    Hit h = (Hit)0;
    h.hit = false;
    return h;
}

// Primary-style query: closest hit, back faces culled (like the Blender kit renders).
Hit traceCulled(float3 o, float3 d, float tmax, uint mask)
{
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 0.0005;
    ray.TMax = tmax;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE, mask, ray);
    q.Proceed();
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return missHit();
    return makeHit(o, d, q.CommittedRayT(), q.CommittedInstanceID(), q.CommittedGeometryIndex(),
                   q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(), q.CommittedObjectToWorld3x4());
}

// Secondary rays see both faces, so the backs of walls still block.
Hit traceAny(float3 o, float3 d, float tmax, uint mask)
{
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 0.0005;
    ray.TMax = tmax;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE, mask, ray);
    q.Proceed();
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return missHit();
    return makeHit(o, d, q.CommittedRayT(), q.CommittedInstanceID(), q.CommittedGeometryIndex(),
                   q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(), q.CommittedObjectToWorld3x4());
}

bool occluded(float3 o, float3 d, float tmax)
{
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 0.0005;
    ray.TMax = tmax;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE, MASK_WORLD, ray);
    q.Proceed();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// ---------------------------------------------------------------------------
// Shading
// ---------------------------------------------------------------------------
struct Surface
{
    float3 albedo;
    float alpha;
    float3 emissive;
    float roughness;
    float metallic;
};

Surface surface(uint m)
{
    Material mat = gMaterials[m];
    Surface s;
    s.albedo = mat.baseAlpha.rgb;
    s.alpha = mat.baseAlpha.a;
    s.emissive = mat.emissiveRough.rgb;
    s.roughness = clamp(mat.emissiveRough.a, 0.06, 1.0);
    s.metallic = mat.metalPad.x;
    return s;
}

// ---------------------------------------------------------------------------
// Cells: wear and haze, blended bilinearly between cell centres
// ---------------------------------------------------------------------------
float4 cellAt(int2 c)
{
    c = clamp(c, int2(0, 0), int2(grid.xy) - 1);
    return gCellInfo[c.y * int(grid.x) + c.x];
}

float4 cellInfo(float3 P)
{
    float2 g = P.xz / grid.z;   // cell centres sit on integer coordinates
    float2 b = floor(g);
    float2 t = g - b;
    int2 i = int2(b);
    float4 a = lerp(cellAt(i), cellAt(i + int2(1, 0)), t.x);
    float4 c = lerp(cellAt(i + int2(0, 1)), cellAt(i + int2(1, 1)), t.x);
    return lerp(a, c, t.y);
}

uint hash3(int3 p)
{
    uint h = uint(p.x) * 73856093u ^ uint(p.y) * 19349663u ^ uint(p.z) * 83492791u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
float h01(int3 p) { return float(hash3(p) >> 8) * (1.0 / 16777216.0); }

float vnoise(float3 p)
{
    float3 b = floor(p);
    float3 f = p - b;
    f = f * f * (3.0 - 2.0 * f);
    int3 i = int3(b);
    float x00 = lerp(h01(i), h01(i + int3(1, 0, 0)), f.x);
    float x10 = lerp(h01(i + int3(0, 1, 0)), h01(i + int3(1, 1, 0)), f.x);
    float x01 = lerp(h01(i + int3(0, 0, 1)), h01(i + int3(1, 0, 1)), f.x);
    float x11 = lerp(h01(i + int3(0, 1, 1)), h01(i + int3(1, 1, 1)), f.x);
    return lerp(lerp(x00, x10, f.y), lerp(x01, x11, f.y), f.z);
}

float fbm(float3 p)
{
    return 0.5 * vnoise(p) + 0.3 * vnoise(p * 2.03 + 17.1) + 0.2 * vnoise(p * 4.11 + 3.7);
}

// Grime patches, rust streaks running down walls, soot on ceilings, dirt on floors.
// decay 0 = factory clean, 1 = derelict.
void applyWear(inout Surface s, float3 P, float3 N, float decay)
{
    if (decay < 0.02 || s.alpha < 1.0) return;
    float big = fbm(P * 0.9);
    float fine = vnoise(P * 3.7);
    float cover = saturate(((big * 0.75 + fine * 0.25) - (1.0 - decay) * 0.72) * 2.4);
    if (N.y > 0.6) cover = saturate(cover * 1.35 + decay * 0.15);   // floors collect dirt
    if (N.y < -0.6) cover = saturate(cover * 1.15);                  // soot on ceilings
    float streak = 0.0;
    if (abs(N.y) < 0.5)
    {
        // vertically stretched noise: narrow drips and rust runs, broken up along their length
        float3 q = float3((P.x + P.z) * 13.0, P.y * 0.55, (P.x - P.z) * 1.3);
        float runs = saturate((vnoise(q) - 0.6) * 5.0);
        float breakup = smoothstep(0.3, 0.7, vnoise(float3(P.x * 2.1, P.y * 2.6, P.z * 2.1) + 5.3));
        streak = runs * breakup * saturate(decay * 1.5 - 0.25) * (0.6 + 0.4 * fine);
    }
    const float3 grime = float3(0.05, 0.045, 0.038);
    const float3 rust = float3(0.21, 0.09, 0.035);
    s.albedo *= lerp(1.0, 0.7, decay);  // everything goes dingy
    s.albedo = lerp(s.albedo, grime * (0.7 + 0.6 * fine), cover * 0.85);
    s.albedo = lerp(s.albedo, rust * (0.7 + 0.6 * fine), streak * 0.85);
    float dull = saturate(cover + streak * 0.7);
    s.roughness = lerp(s.roughness, 0.92, dull);
    s.metallic *= 1.0 - dull * 0.8;
    s.emissive *= lerp(1.0, 0.7, decay);  // dirty lenses
}

// The surface at a hit: material, the instance's power, and wear (not on the gun).
Surface surfaceAt(Hit h)
{
    Surface s = surface(h.material);
    s.emissive *= h.emis;
    if (!(h.flags & FLAG_GUN))
        applyWear(s, h.P, h.N, cellInfo(h.P).x);
    return s;
}

float3 fresnel(float3 f0, float c) { return f0 + (1.0 - f0) * pow(1.0 - saturate(c), 5.0); }

float3 brdf(float3 N, float3 V, float3 L, Surface s, bool specular)
{
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), s.albedo, s.metallic);
    float3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L)), NdotV = saturate(dot(N, V)) + 1e-4;
    float3 F = fresnel(f0, dot(H, V));
    float3 diffuse = (1.0 - F) * (1.0 - s.metallic) * s.albedo / PI;
    if (!specular) return diffuse;
    float a = s.roughness * s.roughness;
    float a2 = a * a;
    float NdotH = saturate(dot(N, H));
    float dd = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D = a2 / (PI * dd * dd);
    float k = (s.roughness + 1.0) * (s.roughness + 1.0) / 8.0;
    float G = (NdotL / (NdotL * (1.0 - k) + k)) * (NdotV / (NdotV * (1.0 - k) + k));
    return diffuse + D * G * F / (4.0 * NdotL * NdotV + 1e-4);
}

float3 directLight(float3 P, float3 N, float3 V, Surface s, bool shadows, bool specular, inout uint seed)
{
    float cs = grid.z;
    int2 cell = int2(floor(P.x / cs + 0.5), floor(P.z / cs + 0.5));
    int gw = int(grid.x), gh = int(grid.y);
    float3 sum = 0;
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
    {
        int2 c = cell + int2(dx, dy);
        if (c.x < 0 || c.y < 0 || c.x >= gw || c.y >= gh) continue;
        uint2 range = gCells[c.y * gw + c.x];
        for (uint i = 0; i < range.y; ++i)
        {
            Light light = gLights[gCellLights[range.x + i]];
            if (dot(light.radianceRange.rgb, float3(1, 1, 1)) <= 1e-4) continue;  // dead fixture
            float3 lp = light.posRadius.xyz;
            if (shadows) lp += sphereSample(seed) * light.posRadius.w;
            float3 toL = lp - P;
            float d2 = dot(toL, toL);
            float d = sqrt(d2);
            float3 L = toL / d;
            float NdotL = dot(N, L);
            if (NdotL <= 0.0) continue;
            float x = d / light.radianceRange.w;
            float win = saturate(1.0 - x * x * x * x);
            win *= win;
            if (win <= 0.0) continue;
            // (+0.12: a surface brushing past a fixture, like the creature on the ceiling,
            // must not blow out to white)
            float3 E = light.radianceRange.rgb / (d2 + 0.12) * win * NdotL;
            if (shadows && occluded(P + N * 0.004, L, d - 0.02)) continue;
            sum += E * brdf(N, V, L, s, specular);
        }
    }
    // short-lived lights: few, so every one is considered
    uint nd = uint(options.w);
    for (uint i = 0; i < nd; ++i)
    {
        Light light = gDynLights[i];
        float3 lp = light.posRadius.xyz;
        if (shadows) lp += sphereSample(seed) * light.posRadius.w;
        float3 toL = lp - P;
        float d2 = dot(toL, toL);
        float d = sqrt(d2);
        float3 L = toL / d;
        float NdotL = dot(N, L);
        if (NdotL <= 0.0) continue;
        float x = d / light.radianceRange.w;
        float win = saturate(1.0 - x * x * x * x);
        win *= win;
        if (win <= 0.0) continue;
        float3 E = light.radianceRange.rgb / max(d2, 0.02) * win * NdotL;
        if (shadows && occluded(P + N * 0.004, L, d - 0.02)) continue;
        sum += E * brdf(N, V, L, s, specular);
    }
    return sum;
}

// Round 18: the same light, sampled. Every light in range is still looked at, but without a
// ray: what it would add here if nothing were in the way (its power, distance, range, angle
// and the surface's BRDF, at its centre). Those add up to the unshadowed light S, exactly.
// Each of nPicks picks then chooses one light in proportion to what it adds, and only the
// picked lights get a shadow ray (to a random point on the light: soft shadows).
//   ratio (default): the picks only estimate how much of the light gets through, and that
//     fraction scales S. Where every light is visible, or none is, there is no noise at all;
//     there is noise only where some lights are blocked and others not. The fraction is
//     weighted toward the lights that matter, which bends the answer a little in partial
//     shadow (the converged references show how much).
//   plain: each pick's light divided by its chance of being picked. On average exactly
//     what testing every light gives, with noise wherever there are several lights.
// The shadow rays per pixel are nPicks however many lights there are.
// Past this many short-lived lights reaching one point, that point weighs this many of them
// chosen at random (each standing for count / kDynCandidates).
#define kDynCandidates 32u
#define kMaxPicks 4u
#define kBounceDynCandidates 32u

float lightIrradiance(Light light, float soft, float3 lp, float3 P, float3 N, out float3 L, out float d)
{
    float3 toL = lp - P;
    float d2 = dot(toL, toL);
    d = sqrt(d2);
    L = toL / d;
    float NdotL = dot(N, L);
    float x = d / light.radianceRange.w;
    float win = saturate(1.0 - x * x * x * x);
    win *= win;
    if (NdotL <= 0.0 || win <= 0.0) return 0.0;
    // (soft: +0.12 for fixtures, as directLight; short-lived lights clamp at 0.02 instead)
    return win * NdotL / (soft > 0.0 ? d2 + soft : max(d2, 0.02));
}

// Round 19: the picks are spread evenly, not drawn independently. The lights' weights are
// laid end to end (kept in a list while they are added up), and pick k takes the light at
// (u + k / nPicks) of the way along. u is this pixel's own number, moved on by the golden
// ratio every frame or sub-frame, so over a capture's 12 sub-frames the 24 picks land
// evenly along the lights: one adding a quarter of the light gets 6 of them, not "6 on
// average". Drawn independently, the films' walls were visibly grainier than with every
// light tested (captures/film_grain_*); the window's history gains the same way while the
// camera holds still.
// (Tried instead of the list: a second pass over the lights, weighing them again up to the
// last pick. It measured about 0.5 ms a frame slower at 1080p; the list measured the same
// as independent picks.)
#define kMaxCandidates 32u
static float gPickU = 0.0;  // set per pixel in main

float3 unshadowed(Light light, float soft, float scale, float3 lp, float3 P, float3 N, float3 V, Surface s,
                  out float3 L, out float d)
{
    float e = lightIrradiance(light, soft, lp, P, N, L, d);
    if (e <= 0.0) return 0;
    return light.radianceRange.rgb * e * brdf(N, V, L, s, true) * scale;
}

// id: the light's index in gLights, or in gDynLights with the top bit set
void considerLight(Light light, uint id, float soft, float scale, float3 P, float3 N, float3 V, Surface s,
                   inout float3 S, inout float wsum, inout float candW[kMaxCandidates],
                   inout uint candId[kMaxCandidates], inout uint n, inout uint seed)
{
    float3 L;
    float d;
    float3 f = unshadowed(light, soft, scale, light.posRadius.xyz, P, N, V, s, L, d);
    float w = dot(f, float3(0.2126, 0.7152, 0.0722));
    if (w <= 1e-8) return;
    S += f;
    wsum += w;
    if (n < kMaxCandidates)
    {
        candW[n] = w;
        candId[n] = id;
        ++n;
    }
    else
    {
        // the list is full: the last place holds one of several lights, kept in proportion
        // to what each adds, and carries the weight of them all (none is ever dropped)
        const uint last = kMaxCandidates - 1u;
        candW[last] += w;
        if (rnd(seed) * candW[last] < w) candId[last] = id;
    }
}

float3 directLightSampled(float3 P, float3 N, float3 V, Surface s, uint nPicks, bool plain, inout uint seed)
{
    nPicks = clamp(nPicks, 1u, kMaxPicks);
    float candW[kMaxCandidates];
    uint candId[kMaxCandidates];
    uint n = 0;
    float3 S = 0;
    float wsum = 0.0;
    float cs = grid.z;
    int2 cell = int2(floor(P.x / cs + 0.5), floor(P.z / cs + 0.5));
    int gw = int(grid.x), gh = int(grid.y);
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
    {
        int2 c = cell + int2(dx, dy);
        if (c.x < 0 || c.y < 0 || c.x >= gw || c.y >= gh) continue;
        uint2 range = gCells[c.y * gw + c.x];
        for (uint i = 0; i < range.y; ++i)
        {
            uint li = gCellLights[range.x + i];
            Light light = gLights[li];
            if (dot(light.radianceRange.rgb, float3(1, 1, 1)) <= 1e-4) continue;  // dead fixture
            considerLight(light, li, 0.12, 1.0, P, N, V, s, S, wsum, candW, candId, n, seed);
        }
    }
    // Short-lived lights. First a cheap pass over all of them: which can reach this point at
    // all (in range, on the lit side)? Those are weighed, every one, so S stays exact. Only
    // when more than kDynCandidates reach one point are that many of them kept at random
    // (without repeats), each standing for its share.
    // (Until round 19 a random 16 of ALL the lights were weighed whenever there were more
    // than 16 in the scene, with repeats. With ten creatures (20+ lights) that made S itself
    // a dice roll on every surface: grain on walls, pipes and the gun, not just in shadows.)
    uint nd = uint(options.w);
    uint reach[kDynCandidates];
    uint m = 0;
    for (uint i = 0; i < nd; ++i)
    {
        Light light = gDynLights[i];
        float3 toL = light.posRadius.xyz - P;
        float r = light.radianceRange.w;
        if (dot(toL, toL) >= r * r || dot(toL, N) <= 0.0) continue;
        if (m < kDynCandidates) reach[m] = i;
        else
        {
            uint j = uint(rnd(seed) * float(m + 1u));
            if (j < kDynCandidates) reach[j] = i;
        }
        ++m;
    }
    uint cnt = min(m, kDynCandidates);
    float sc = cnt > 0u ? float(m) / float(cnt) : 1.0;
    for (uint k2 = 0; k2 < cnt; ++k2)
        considerLight(gDynLights[reach[k2]], reach[k2] | 0x80000000u, 0.0, sc, P, N, V, s, S, wsum, candW, candId, n,
                      seed);
    if (wsum <= 0.0 || n == 0u) return 0;
    // the picks: evenly spaced along the weights from this pixel's u (see above)
    const float u0 = gPickU;
    gPickU = frac(gPickU + 0.381966);  // (another surface shaded for this pixel: move on)
    uint pick[kMaxPicks];
    float at[kMaxPicks];
    [unroll] for (uint k = 0; k < kMaxPicks; ++k)
    {
        pick[k] = n - 1u;
        at[k] = frac(u0 + float(k) / float(nPicks)) * wsum;
    }
    float run = 0.0;
    for (uint ci = 0; ci < n; ++ci)
    {
        float next = run + candW[ci];
        [unroll] for (uint k = 0; k < kMaxPicks; ++k)
            if (at[k] >= run && at[k] < next) pick[k] = ci;
        run = next;
    }
    float3 lit = 0, all = 0;  // the picks' estimate with and without their shadow rays
    [unroll] for (uint k = 0; k < kMaxPicks; ++k)
    {
        if (k >= nPicks) continue;
        const uint id = candId[pick[k]];
        const bool dyn = (id & 0x80000000u) != 0u;
        Light light;
        if (dyn) light = gDynLights[id & 0x7FFFFFFFu];
        else light = gLights[id];
        const float soft = dyn ? 0.0 : 0.12;
        const float scale = dyn ? sc : 1.0;
        float3 L;
        float d;
        // its own weight again (the list's last place may carry several lights' weight)
        float w = dot(unshadowed(light, soft, scale, light.posRadius.xyz, P, N, V, s, L, d),
                      float3(0.2126, 0.7152, 0.0722));
        if (w <= 1e-8) continue;
        float3 lp = light.posRadius.xyz + sphereSample(seed) * light.posRadius.w;
        float3 f = unshadowed(light, soft, scale, lp, P, N, V, s, L, d) * (wsum / w);
        if (dot(f, float3(1, 1, 1)) <= 0.0) continue;
        all += f;
        if (!occluded(P + N * 0.004, L, d - 0.02)) lit += f;
    }
    if (plain || dot(all, float3(1, 1, 1)) <= 1e-7) return lit / float(nPicks);
    // ratio: the share that gets through (per colour: lights of different colours can be
    // blocked differently), times the unshadowed light added up exactly
    // (a colour the picks hardly carry takes the overall share instead)
    float allL = dot(all, float3(0.2126, 0.7152, 0.0722));
    float share = saturate(dot(lit, float3(0.2126, 0.7152, 0.0722)) / max(allL, 1e-7));
    float3 perColour = saturate(lit / max(all, 1e-7));
    float3 ratio = lerp(float3(share, share, share), perColour, step(0.1 * allL, all));
    return S * ratio;
}

// What a surface hit by a bounce or reflection ray gets from the lights (no shadows, diffuse
// only, as directLight with shadows off). The fixtures (a dozen or so in the 3x3 cells) are
// all added up as before, and so are the short-lived lights (see below).
float3 bounceDirectSampled(float3 P, float3 N, float3 V, Surface s, inout uint seed)
{
    float3 sum = 0;
    float cs = grid.z;
    int2 cell = int2(floor(P.x / cs + 0.5), floor(P.z / cs + 0.5));
    int gw = int(grid.x), gh = int(grid.y);
    float3 L;
    float d;
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
    {
        int2 c = cell + int2(dx, dy);
        if (c.x < 0 || c.y < 0 || c.x >= gw || c.y >= gh) continue;
        uint2 range = gCells[c.y * gw + c.x];
        for (uint i = 0; i < range.y; ++i)
        {
            Light light = gLights[gCellLights[range.x + i]];
            float e = lightIrradiance(light, 0.12, light.posRadius.xyz, P, N, L, d);
            if (e > 0.0) sum += light.radianceRange.rgb * e * brdf(N, V, L, s, false);
        }
    }
    // Short-lived lights: what every one in range sends here is added up (no ray), and the
    // diffuse BRDF is applied once, at their average direction (it depends on the direction
    // only through a small Fresnel term), so a light costs a few multiplies.
    // (Until round 19 this picked one of a random 6 and divided by its chance, which put
    // grain into the bounced light on every surface once there were more than a few lights.)
    // Up to kBounceDynCandidates lights in the scene (the game has 20-odd with ten creatures
    // close), that is all of them, with no noise. Past that, every (nd / that many)th from a
    // random start, each standing for its share: no repeats, and the work stops growing.
    // (Looking through all 120 of the cost test's lights here cost 3-4 ms a frame.)
    uint nd = uint(options.w);
    uint cnt = min(nd, kBounceDynCandidates);
    float stride = cnt > 0u ? float(nd) / float(cnt) : 1.0;
    float first = nd > cnt ? rnd(seed) : 0.0;
    float3 E = 0, Lsum = 0;
    for (uint k = 0; k < cnt; ++k)
    {
        Light light = gDynLights[min(nd - 1u, uint((float(k) + first) * stride))];
        float3 toL = light.posRadius.xyz - P;
        float r = light.radianceRange.w;
        if (dot(toL, toL) >= r * r || dot(toL, N) <= 0.0) continue;
        float e = lightIrradiance(light, 0.0, light.posRadius.xyz, P, N, L, d);
        float3 c = light.radianceRange.rgb * e;
        E += c;
        Lsum += L * dot(c, float3(0.2126, 0.7152, 0.0722));
    }
    if (dot(Lsum, Lsum) > 1e-12) sum += E * stride * brdf(N, V, normalize(Lsum), s, false);
    return sum;
}

// Spot cone weight of the flashlight toward direction -L (from the light to the point).
float flashSpot(float3 L)
{
    float c = dot(-L, flashDir.xyz);
    return smoothstep(flashDir.w, flashCone.x, c) * 0.93 + 0.07 * smoothstep(flashCone.y, flashDir.w, c);
}

static const float3 kFlashColor = float3(1.0, 0.95, 0.88);

float3 flashLight(float3 P, float3 N, float3 V, Surface s, bool shadows, bool specular)
{
    if (flashPos.w <= 0.0) return 0;
    float3 toL = flashPos.xyz - P;
    float d2 = dot(toL, toL);
    float d = sqrt(d2);
    float3 L = toL / d;
    float NdotL = dot(N, L);
    if (NdotL <= 0.0) return 0;
    float spot = flashSpot(L);
    if (spot <= 0.0) return 0;
    float x = d / flashCone.z;
    float win = saturate(1.0 - x * x * x * x);
    win *= win;
    if (win <= 0.0) return 0;
    if (shadows && occluded(P + N * 0.004, L, d - 0.02)) return 0;
    // (+0.35: things right in front of the lens, like a hanging panel, stay readable)
    float3 E = kFlashColor * flashPos.w * spot * win / (d2 + 0.35) * NdotL;
    return E * brdf(N, V, L, s, specular);
}

// Radiance leaving a surface hit by a secondary ray (no shadows from fixtures, diffuse
// only: cheap). The flashlight is shadowed, or its light would leak through walls.
float3 bounceRadiance(Hit h, float3 dir, float clampTo, inout uint seed)
{
    if (!h.hit) return options.y;
    if (!h.front) return 0;  // inside the back of a wall
    Surface s = surfaceAt(h);
    float3 lit = hazeParams.w >= 1.0 ? bounceDirectSampled(h.P, h.N, -dir, s, seed)
                                     : directLight(h.P, h.N, -dir, s, false, false, seed);
    float3 L = s.emissive + lit + s.albedo * options.y;
    L += flashLight(h.P, h.N, -dir, s, true, false);
    // Small, very bright emitters (light panels) found by a random secondary ray are
    // fireflies at low sample counts: clamp what one path can carry.
    return min(L, clampTo);
}

float3 shade(Hit h, float3 dir, inout uint seed)
{
    Surface s = surfaceAt(h);
    float3 V = -dir;
    float3 N = h.N;
    bool rt = options.x > 0.5;
    float3 c = s.emissive;
    // round 18: sampled (hazeParams.w lights picked per pixel), or every light in range
    // (hazeParams.w: picks, plus 8 for the plain estimator)
    if (rt && hazeParams.w >= 1.0)
        c += directLightSampled(h.P, N, V, s, uint(hazeParams.w) & 7u, uint(hazeParams.w) >= 8u, seed);
    else
        c += directLight(h.P, N, V, s, rt, true, seed);
    c += flashLight(h.P, N, V, s, rt, true);
    if (!rt)
        return c + s.albedo * 0.035 * (1.0 - s.metallic) + options.y;

    float3 o = h.P + N * 0.003;
    // one diffuse bounce
    if (s.metallic < 0.99)
    {
        float3 gd = cosineSample(N, seed);
        Hit g = traceAny(o, gd, 10.0, MASK_WORLD);
        c += s.albedo * (1.0 - s.metallic) * bounceRadiance(g, gd, 1.5, seed);
    }
    // glossy reflection
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), s.albedo, s.metallic);
    float NdotV = saturate(dot(N, V));
    if (s.roughness < 0.62)
    {
        float3 r = reflect(dir, N);
        float spread = s.roughness * s.roughness;
        r = normalize(r + sphereSample(seed) * spread * 1.5);
        if (dot(r, N) < 0.02) r = normalize(r + N * (0.02 - dot(r, N)));
        Hit rh = traceAny(o, r, 40.0, MASK_WORLD);
        float gloss = saturate(1.0 - s.roughness * 1.4);
        // sharp reflections may show the light panels; blurry ones must not sparkle
        float3 Lr = bounceRadiance(rh, r, lerp(1.0, 8.0, gloss * gloss), seed);
        c += fresnel(f0, NdotV) * Lr * gloss;
    }
    return c + options.y * 0.02;
}

// ---------------------------------------------------------------------------
// Space (part 3): what a ray sees once it has left the ship through a viewport.
// Stars on a cube-face grid (one jittered candidate per cell, the 3x3 cells around the
// ray checked), each a soft disc ~0.8 px wide at 1080p, most faint, a few bright, tinted
// from orange to blue-white; a faint galactic band and a dim violet nebula. A pure
// function of direction, so captures repeat and the window can reproject it.
// ---------------------------------------------------------------------------
// (hash3, vnoise and fbm are the wear section's)
float hashF(inout uint h) { return rnd(h); }

float3 sky(float3 d)
{
    // cube face and face coordinates
    float3 a = abs(d);
    int face;
    float2 uv;
    if (a.x >= a.y && a.x >= a.z) { face = d.x > 0 ? 0 : 1; uv = d.yz / a.x; }
    else if (a.y >= a.z)          { face = d.y > 0 ? 2 : 3; uv = d.xz / a.y; }
    else                          { face = d.z > 0 ? 4 : 5; uv = d.xy / a.z; }
    const float N = 300.0;                    // cells across a face (~5 px at 1080p)
    float2 g = (uv * 0.5 + 0.5) * N;
    int2 c0 = int2(floor(g));
    float3 col = 0;
    const float sigma = 0.2;                  // star radius in cells (~1 px)
    [unroll] for (int k = 0; k < 9; ++k)
    {
        int2 c = c0 + int2(k % 3 - 1, k / 3 - 1);
        uint h = hash3(int3(c, face));
        if (hashF(h) > 0.11) continue;         // about one cell in nine has a star
        float2 p = float2(c) + float2(0.15, 0.15) + 0.7 * float2(hashF(h), hashF(h));
        float2 q = g - p;
        float r2 = dot(q, q);
        float m = hashF(h);
        float b = 0.05 + 1.6 * pow(m, 8.0) + (m > 0.985 ? 22.0 : 0.0);  // magnitude: mostly faint
        float t = hashF(h);
        float3 tint = t < 0.15 ? float3(1.0, 0.72, 0.48) : t < 0.35 ? float3(1.0, 0.9, 0.75)
                     : t < 0.8 ? float3(0.92, 0.95, 1.0) : float3(0.7, 0.8, 1.0);
        col += tint * b * exp(-r2 / (2.0 * sigma * sigma));
    }
    // galactic band round one great circle, and a nebula off to one side
    float3 nG = normalize(float3(0.35, 0.82, -0.45));
    float bandW = exp(-pow(dot(d, nG) / 0.2, 2.0));
    float dust = fbm(d * 5.0 + 3.1);
    col += float3(0.16, 0.18, 0.3) * 0.05 * bandW * (0.4 + dust);
    float3 nN = normalize(float3(-0.6, 0.25, 0.75));
    float neb = smoothstep(0.75, 1.0, dot(d, nN)) * fbm(d * 7.0 + 11.0);
    col += float3(0.32, 0.12, 0.38) * 0.07 * neb;
    return col;
}

// ---------------------------------------------------------------------------
// Haze: single scattering along the primary ray
// ---------------------------------------------------------------------------
float hgPhase(float c)
{
    float g = hazeParams.y;
    float d = 1.0 + g * g - 2.0 * g * c;
    return (1.0 - g * g) / (4.0 * PI * d * sqrt(d));
}

float lightWindow(float d, float range)
{
    float x = d / range;
    float w = saturate(1.0 - x * x * x * x);
    return w * w;
}

// Light scattered toward the camera by the haze between o and o + d * tEnd, and the
// transmittance over that distance (for the surface behind it).
float3 hazeInscatter(float3 o, float3 d, float tEnd, out float transmittance, inout uint seed)
{
    transmittance = 1.0;
    if (hazeParams.x <= 0.0) return 0;
    float sigma = 0.5 * (cellInfo(o).y + cellInfo(o + d * tEnd).y) * hazeParams.x;
    transmittance = exp(-sigma * tEnd);
    if (sigma <= 1e-6) return 0;
    float3 L = 0;

    // fixtures: two stratified distances along the ray; at each, one light from that
    // point's 3x3 cells, chosen in proportion to its unshadowed contribution (weighted
    // reservoir sampling), then one shadow ray to it
    const int kHazeSteps = 2;
    int gw = int(grid.x), gh = int(grid.y);
    for (int k = 0; k < kHazeSteps; ++k)
    {
        float ts = (float(k) + rnd(seed)) / float(kHazeSteps) * tEnd;
        float3 P = o + d * ts;
        float sig = cellInfo(P).y * hazeParams.x;
        if (sig <= 0.0) continue;
        int2 cell = int2(floor(P.x / grid.z + 0.5), floor(P.z / grid.z + 0.5));
        float wsum = 0.0, wPick = 0.0;
        float3 cPick = 0, lPick = 0;
        for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 c = cell + int2(dx, dy);
            if (c.x < 0 || c.y < 0 || c.x >= gw || c.y >= gh) continue;
            uint2 range = gCells[c.y * gw + c.x];
            for (uint i = 0; i < range.y; ++i)
            {
                Light light = gLights[gCellLights[range.x + i]];
                float3 toL = light.posRadius.xyz - P;
                float dd2 = dot(toL, toL);
                float dl = sqrt(dd2);
                float3 contrib = light.radianceRange.rgb * lightWindow(dl, light.radianceRange.w) / max(dd2, 0.3) *
                                 hgPhase(dot(toL / dl, d));
                float w = dot(contrib, float3(0.2126, 0.7152, 0.0722));
                if (w <= 1e-6) continue;
                wsum += w;
                if (rnd(seed) * wsum < w)
                {
                    wPick = w;
                    cPick = contrib;
                    lPick = light.posRadius.xyz;
                }
            }
        }
        if (wsum <= 0.0) continue;
        float3 toL = lPick - P;
        float dl = length(toL);
        if (occluded(P, toL / dl, dl - 0.05)) continue;
        float3 s = sig * exp(-sigma * ts) * cPick * (wsum / wPick) * (tEnd / float(kHazeSteps));
        L += min(s, 1.2);
    }

    // flashlight: equiangular sampling around the beam's origin
    if (flashPos.w > 0.0)
    {
        float3 lp = flashPos.xyz;
        float delta = dot(lp - o, d);
        float D = max(length(o + d * delta - lp), 0.03);
        // the ray only enters the cone a little past the light: start sampling there
        float tA = min(max(0.0, delta + 0.4), tEnd);
        float thA = atan2(tA - delta, D), thB = atan2(tEnd - delta, D);
        float t = D * tan(lerp(thA, thB, rnd(seed)));
        float ts = clamp(delta + t, tA, tEnd);
        float3 P = o + d * ts;
        float3 toL = lp - P;
        float dl = max(length(toL), 0.01);
        float3 Ld = toL / dl;
        float spot = flashSpot(Ld);
        float sig = cellInfo(P).y * hazeParams.x;
        float win = lightWindow(dl, flashCone.z);
        if (spot > 0.0 && sig > 0.0 && win > 0.0 && !occluded(P, Ld, dl - 0.02))
        {
            // the 1/d^2 falloff cancels against the equiangular pdf
            float w = (thB - thA) / D;
            L += min(sig * exp(-sigma * ts) * hgPhase(dot(Ld, d)) * kFlashColor * flashPos.w * spot * win * w, 1.5);
        }
    }

    // the brightest short-lived light (muzzle flash, spark burst): equiangular, shadowed
    if (options.w >= 1.0)
    {
        Light light = gDynLights[0];
        float3 lp = light.posRadius.xyz;
        float delta = dot(lp - o, d);
        // (the muzzle flash sits almost on the camera ray: keep its glow from blowing up)
        float D = max(length(o + d * delta - lp), 0.3);
        float thA = atan2(-delta, D), thB = atan2(tEnd - delta, D);
        float t = D * tan(lerp(thA, thB, rnd(seed)));
        float ts = clamp(delta + t, 0.0, tEnd);
        float3 P = o + d * ts;
        float3 toL = lp - P;
        float dl = max(length(toL), 0.01);
        float3 Ld = toL / dl;
        float sig = max(cellInfo(P).y, 0.004) * hazeParams.x;
        float win = lightWindow(dl, light.radianceRange.w);
        if (win > 0.0 && !occluded(P, Ld, dl - 0.02))
            L += min(0.35 * sig * exp(-sigma * ts) * hgPhase(dot(Ld, d)) * light.radianceRange.rgb * win * (thB - thA) / D,
                     0.4);
    }
    return L;
}

// ---------------------------------------------------------------------------
// Steam: plumes from torn pipes, ray-marched along the primary ray up to tMax.
// T is the transmittance so far (in/out); returns the light the steam adds.
// ---------------------------------------------------------------------------
float3 steamLight(float3 P)
{
    float3 E = 0;
    // flashlight (unshadowed: the plume is small and near)
    if (flashPos.w > 0.0)
    {
        float3 toL = flashPos.xyz - P;
        float d2 = dot(toL, toL);
        E += kFlashColor * flashPos.w * flashSpot(toL * rsqrt(d2)) / max(d2, 0.1);
    }
    uint nd = uint(options.w);
    for (uint i = 0; i < nd; ++i)
    {
        Light light = gDynLights[i];
        float3 toL = light.posRadius.xyz - P;
        E += light.radianceRange.rgb / max(dot(toL, toL), 0.1);
    }
    return E;
}

float3 marchSteam(float3 o, float3 d, float tMax, inout float T, inout uint seed)
{
    float3 L = 0;
    uint n = uint(flashCone.w);
    for (uint i = 0; i < n; ++i)
    {
        Steam s = gSteam[i];
        float3 so = s.originLength.xyz;
        float len = s.originLength.w;
        float3 sd = s.dirStrength.xyz;
        float strength = s.dirStrength.w;
        // bounding sphere round the bent cone
        float3 c = so + sd * (len * 0.5) + float3(0, 0.12 * len, 0);
        float R = len * 0.8;
        float3 oc = o - c;
        float b = dot(oc, d);
        float disc = b * b - (dot(oc, oc) - R * R);
        if (disc <= 0.0) continue;
        float sq = sqrt(disc);
        float t0 = max(-b - sq, 0.0), t1 = min(-b + sq, tMax);
        if (t1 <= t0) continue;
        const int N = 14;
        float dt = (t1 - t0) / N;
        float t = t0 + dt * rnd(seed);
        for (int k = 0; k < N; ++k, t += dt)
        {
            float3 p = o + d * t;
            float u = dot(p - so, sd);
            if (u < 0.0 || u > len) continue;
            float3 axisP = so + sd * u + float3(0, 0.3 * u * u / len, 0);  // buoyant: bends up
            float r = length(p - axisP);
            float rad = 0.035 + 0.3 * u;                                    // widens
            float prof = exp(-2.2 * r * r / (rad * rad));
            if (prof < 0.01) continue;
            float fade = pow(saturate(1.0 - u / len), 1.5) * smoothstep(0.0, 0.06, u);
            float turb = fbm(p * 4.5 - sd * (hazeParams.z * 3.2) - float3(0, hazeParams.z * 0.8, 0) + s.lightSeed.w);
            float dens = prof * fade * saturate(turb * 1.7 - 0.3) * strength * 9.0;
            if (dens <= 1e-3) continue;
            float a = 1.0 - exp(-dens * dt);
            // single scattering, roughly isotropic, albedo ~0.9; a little ambient so it reads
            float3 Lp = (s.lightSeed.xyz + steamLight(p)) * 0.07 + 0.004;
            L += T * a * Lp;
            T *= 1.0 - a;
        }
    }
    return L;
}

// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 px = id.xy;
    if (px.x >= uint(resolution.x) || px.y >= uint(resolution.y)) return;
    uint seed = px.x * 1973u + px.y * 9277u + uint(jitter.z) * 26699u;
    seed = pcg(seed);
    // where this pixel's light picks start: its own number, moved on by the golden ratio
    // (2654435769 / 2^32) each frame or sub-frame
    uint pickSeed = px.x * 7919u + px.y * 104729u + 12345u;
    gPickU = float((pcg(pickSeed) + uint(jitter.z) * 2654435769u) >> 8) * (1.0 / 16777216.0);

    float2 uv = (float2(px) + 0.5 + jitter.xy) * resolution.zw;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float3 dir = normalize(camFwd.xyz + camRight.xyz * ndc.x * camPos.w + camUp.xyz * ndc.y * camRight.w);
    float3 origin = camPos.xyz;

    float3 radiance = 0;
    float3 throughput = 1;
    float4 posId = float4(0, 0, 0, 0);
    uint firstFlags = 0;
    float firstT = 60.0;
    bool toSpace = false;  // the ray left the ship through glass: the stars
    for (int layer = 0; layer < 3; ++layer)
    {
        Hit h = traceCulled(origin, dir, 200.0, 0xFF);
        if (!h.hit)
        {
            radiance += throughput * (sky(dir) + options.y * 0.2);
            toSpace = layer > 0;
            break;
        }
        if (layer == 0)
        {
            // instance id as a plain float (exact below 2^24): asfloat() of a small
            // integer is a denormal, which the store flushes to zero
            posId = float4(h.P, float(h.instance + 1u));
            firstFlags = h.flags;
            firstT = min(h.t, 60.0);
        }
        Surface s = surfaceAt(h);
        if (s.alpha < 1.0)
        {
            // glass (part 3): it mirrors what is behind you with Fresnel's weight (a
            // fully shaded reflection ray), catches highlights from the lights and the
            // flashlight, keeps a faint film of its own colour (alpha), and lets the rest
            // through tinted by its hue
            float3 V = -dir;
            float3 N = h.N;
            bool rt = options.x > 0.5;
            // a pane has two faces, ~4% each head-on: 9% for the pair
            float3 F = fresnel(float3(0.09, 0.09, 0.09), abs(dot(N, V)));
            if (rt)
            {
                float3 r = reflect(dir, N);
                Hit rh = traceAny(h.P + N * 0.002, r, 60.0, MASK_WORLD);
                float3 Lr = !rh.hit ? sky(r) : rh.front ? shade(rh, r, seed) : float3(0, 0, 0);
                radiance += throughput * F * min(Lr, 24.0);
            }
            Surface gs = s;
            gs.albedo = 0;  // highlights only: glass has no diffuse
            gs.metallic = 0;
            gs.roughness = 0.015;  // polished: the flashlight shows as a tight spot, not a blob
            radiance += throughput * (directLight(h.P, N, V, gs, rt, true, seed) + flashLight(h.P, N, V, gs, rt, true));
            float3 lit = directLight(h.P, N, V, s, false, false, seed);
            radiance += throughput * s.albedo * lit * s.alpha;
            float3 hue = s.albedo / max(max(s.albedo.r, s.albedo.g), max(s.albedo.b, 1e-3));
            throughput *= (1.0 - F) * (1.0 - s.alpha) * lerp(float3(1, 1, 1), hue, 0.35);
            origin = h.P + dir * 0.002;
            continue;
        }
        radiance += throughput * shade(h, dir, seed);
        break;
    }
    if (options.x > 0.5 && !(firstFlags & FLAG_GUN))
    {
        float T;
        float3 ins = hazeInscatter(camPos.xyz, dir, firstT, T, seed);
        radiance = radiance * T + ins;
        if (flashCone.w >= 1.0)
        {
            float Ts = 1.0;
            float3 steam = marchSteam(camPos.xyz, dir, firstT, Ts, seed);
            radiance = radiance * Ts + steam;
        }
    }
    radiance = min(radiance, 64.0);  // tame fireflies
    // seen through glass, space reprojects as if infinitely far (a point 500 m along the
    // ray, a reserved id), so the stars don't smear when the camera moves
    if (toSpace && !(firstFlags & FLAG_GUN)) posId = float4(camPos.xyz + dir * 500.0, 16000000.0);

    float mode = jitter.w;
    float4 result = float4(radiance, 1.0);
    if (mode > 0.0 && mode < 1.0)
    {
        // capture: average with the previous sub-frames of the same still camera
        float4 prev = gPrevAccum.Load(int3(px, 0));
        result = float4(lerp(prev.rgb, radiance, mode), prev.w + 1.0);
    }
    else if (mode < 0.0 && posId.w != 0.0)
    {
        // window: reproject into the previous frame and blend if it saw the same surface
        // previous-frame position of this surface, in pixels (centre convention)
        float2 prevF = float2(-10, -10);
        if (firstFlags & FLAG_GUN)
            prevF = float2(px);  // the gun is fixed to the camera
        else
        {
            float3 v = posId.xyz - prevPos.xyz;
            float z = dot(v, prevFwd.xyz);
            if (z > 0.01)
            {
                float sx = dot(v, prevRight.xyz) / (z * prevPos.w);
                float sy = dot(v, prevUp.xyz) / (z * prevRight.w);
                prevF = float2((sx * 0.5 + 0.5) * resolution.x, (0.5 - sy * 0.5) * resolution.y) - 0.5;
            }
        }
        // bilinear history from the 4 nearest previous pixels that saw the same surface
        int2 base = int2(floor(prevF));
        float2 f = prevF - float2(base);
        float dist = length(posId.xyz - camPos.xyz);
        float tol = 0.03 + 0.01 * dist;
        float4 hist = 0;
        float wsum = 0;
        [unroll] for (int k = 0; k < 4; ++k)
        {
            int2 o = int2(k & 1, k >> 1);
            int2 q = base + o;
            if (any(q < 0) || q.x >= int(resolution.x) || q.y >= int(resolution.y)) continue;
            float w = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
            float4 pp = gPrevPos.Load(int3(q, 0));
            if (abs(pp.w - posId.w) > 0.5 || length(pp.xyz - posId.xyz) > tol) continue;
            hist += gPrevAccum.Load(int3(q, 0)) * w;
            wsum += w;
        }
        if (wsum > 0.05)
        {
            hist /= wsum;
            float n = min(hist.w + 1.0, options.z);
            result = float4(lerp(hist.rgb, radiance, 1.0 / n), n);
        }
    }
    gAccum[px] = result;
    gPos[px] = posId;
}
