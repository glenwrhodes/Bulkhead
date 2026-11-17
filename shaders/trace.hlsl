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

#define MASK_WORLD 0x01
#define MASK_GUN   0x02
#define MASK_GLASS 0x04
#define FLAG_GUN   1u
#define PI 3.14159265

struct Vertex { float3 pos; float3 nrm; };
struct Submesh { uint firstIndex; uint baseVertex; uint material; uint pad; };
struct InstInfo { uint firstSubmesh; uint flags; uint pad0; uint pad1; };
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
    float4 options;     // x RT on, y ambient, z max history, w unused
};

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
            float3 E = light.radianceRange.rgb / max(d2, 0.05) * win * NdotL;
            if (shadows && occluded(P + N * 0.004, L, d - 0.02)) continue;
            sum += E * brdf(N, V, L, s, specular);
        }
    }
    return sum;
}

// Radiance leaving a surface hit by a secondary ray (no shadows, diffuse only: cheap).
float3 bounceRadiance(Hit h, float3 dir, float clampTo, inout uint seed)
{
    if (!h.hit) return options.y;
    if (!h.front) return 0;  // inside the back of a wall
    Surface s = surface(h.material);
    float3 L = s.emissive + directLight(h.P, h.N, -dir, s, false, false, seed) + s.albedo * options.y;
    // Small, very bright emitters (light panels) found by a random secondary ray are
    // fireflies at low sample counts: clamp what one path can carry.
    return min(L, clampTo);
}

float3 shade(Hit h, float3 dir, inout uint seed)
{
    Surface s = surface(h.material);
    float3 V = -dir;
    float3 N = h.N;
    bool rt = options.x > 0.5;
    float3 c = s.emissive;
    c += directLight(h.P, N, V, s, rt, true, seed);
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
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 px = id.xy;
    if (px.x >= uint(resolution.x) || px.y >= uint(resolution.y)) return;
    uint seed = px.x * 1973u + px.y * 9277u + uint(jitter.z) * 26699u;
    seed = pcg(seed);

    float2 uv = (float2(px) + 0.5 + jitter.xy) * resolution.zw;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float3 dir = normalize(camFwd.xyz + camRight.xyz * ndc.x * camPos.w + camUp.xyz * ndc.y * camRight.w);
    float3 origin = camPos.xyz;

    float3 radiance = 0;
    float3 throughput = 1;
    float4 posId = float4(0, 0, 0, 0);
    uint firstFlags = 0;
    for (int layer = 0; layer < 3; ++layer)
    {
        Hit h = traceCulled(origin, dir, 200.0, 0xFF);
        if (!h.hit)
        {
            radiance += throughput * options.y * 0.2;
            break;
        }
        if (layer == 0)
        {
            // instance id as a plain float (exact below 2^24): asfloat() of a small
            // integer is a denormal, which the store flushes to zero
            posId = float4(h.P, float(h.instance + 1u));
            firstFlags = h.flags;
        }
        Surface s = surface(h.material);
        if (s.alpha < 1.0)
        {
            // glass: a little reflected light and tint, then continue through
            float3 V = -dir;
            float3 f0 = float3(0.04, 0.04, 0.04);
            float3 F = fresnel(f0, abs(dot(h.N, V)));
            float3 lit = directLight(h.P, h.N, V, s, false, true, seed);
            radiance += throughput * (s.albedo * lit * s.alpha + F * 0.15);
            throughput *= (1.0 - s.alpha) * lerp(float3(1, 1, 1), s.albedo * 2.0, 0.35);
            origin = h.P + dir * 0.002;
            continue;
        }
        radiance += throughput * shade(h, dir, seed);
        break;
    }
    radiance = min(radiance, 64.0);  // tame fireflies

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
