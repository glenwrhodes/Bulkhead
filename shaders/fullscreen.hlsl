// Round 1 full-screen pass: an animated "tactical display" background (perspective
// deck grid scrolling toward the viewer) with the overhead maze map on a glowing
// panel and a pulsing marker walking the scripted route.

cbuffer FrameConstants : register(b0)
{
    float2 gResolution;
    float  gTime;
    float  gMapOpacity;
    float4 gMapRect;        // x0, y0, x1, y1 (pixels)
    float2 gMarker;         // map UV
    float  gMarkerVisible;
    float  gMarkerRadius;   // pixels
};

Texture2D<float4> gMap : register(t0);
SamplerState gLinear : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
    // One triangle covering the screen.
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

static const float3 kTeal   = float3(0.25, 0.82, 0.88);
static const float3 kOrange = float3(1.00, 0.62, 0.22);

// Signed distance to a rounded box centred at the origin.
float sdRoundBox(float2 p, float2 halfSize, float radius)
{
    float2 q = abs(p) - halfSize + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

float3 background(float2 px)
{
    float2 uv = px / gResolution;
    float aspect = gResolution.x / gResolution.y;
    float3 col = lerp(float3(0.010, 0.016, 0.026), float3(0.028, 0.044, 0.060), uv.y);

    // Deck grid below the horizon, scrolling toward the camera.
    const float horizon = 0.56;
    float below = max(uv.y - horizon, 1e-3);
    float depth = 0.16 / below;
    float2 g = float2((uv.x - 0.5) * aspect * depth * 2.2, depth + gTime * 1.2);
    float2 w = fwidth(g) * 1.3;
    float2 d = min(frac(g), 1.0 - frac(g));
    float2 lines = 1.0 - smoothstep(0.0, w, d);
    float grid = max(lines.x, lines.y) * exp(-depth * 0.09);
    float floorMask = step(horizon, uv.y);
    col += kTeal * grid * 0.35 * floorMask;

    // Horizon glow and faint ceiling ribs above it.
    col += kTeal * exp(-abs(uv.y - horizon) * 38.0) * 0.20;
    float rib = 1.0 - smoothstep(0.0, 0.02, abs(frac(uv.x * 9.0 + 0.5) - 0.5));
    col += kTeal * rib * 0.025 * (1.0 - floorMask) * (1.0 - uv.y / horizon);

    // Scanlines + vignette.
    col *= 0.94 + 0.06 * sin(px.y * 3.14159);
    float2 v = uv - 0.5;
    col *= 1.0 - dot(v, v) * 1.1;
    return col;
}

float4 PSMain(VSOut input) : SV_Target
{
    float2 px = input.pos.xy;
    float3 col = background(px);

    // --- Map panel ---------------------------------------------------------
    float2 rectMin = gMapRect.xy;
    float2 rectMax = gMapRect.zw;
    float2 rectSize = rectMax - rectMin;
    float2 centre = (rectMin + rectMax) * 0.5;
    float pad = 14.0;
    float dPanel = sdRoundBox(px - centre, rectSize * 0.5 + pad, 12.0);

    float panel = (1.0 - smoothstep(-1.0, 1.0, dPanel)) * gMapOpacity;
    col = lerp(col, float3(0.012, 0.020, 0.030), panel * 0.88);

    float border = (1.0 - smoothstep(0.75, 2.25, abs(dPanel))) * gMapOpacity;
    float glow = exp(-max(dPanel, 0.0) / 14.0) * step(0.0, dPanel) * gMapOpacity;
    col += kTeal * (border * 0.9 + glow * 0.18);

    float2 mapUV = (px - rectMin) / rectSize;
    float3 mapColor = gMap.SampleLevel(gLinear, saturate(mapUV), 0).rgb;
    float inside = step(0.0, mapUV.x) * step(mapUV.x, 1.0) * step(0.0, mapUV.y) * step(mapUV.y, 1.0);
    // Slow scan line sweeping down the map.
    float sweep = exp(-abs(frac(gTime * 0.18) - mapUV.y) * 90.0) * 0.12;
    col = lerp(col, mapColor + kTeal * sweep, inside * gMapOpacity);

    // --- Route marker ------------------------------------------------------
    float2 markerPx = rectMin + gMarker * rectSize;
    float md = length(px - markerPx);
    float r = gMarkerRadius;
    float core = 1.0 - smoothstep(r - 1.0, r + 1.0, md);
    float ringPhase = frac(gTime * 0.9);
    float ringRadius = r * (1.4 + 2.2 * ringPhase);
    float ring = (1.0 - smoothstep(0.75, 2.0, abs(md - ringRadius))) * (1.0 - ringPhase);
    float halo = exp(-md / (r * 1.6)) * 0.45;
    float markerAmount = gMarkerVisible * gMapOpacity;
    col = lerp(col, kOrange, core * markerAmount);
    col += kOrange * (ring * 0.9 + halo) * markerAmount * (1.0 - core);

    return float4(saturate(col), 1.0);
}
