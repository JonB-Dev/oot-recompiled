// The shaders our interface needs: a textured, vertex colored triangle list, and the mark.
//
// RmlUi hands us premultiplied alpha colors, so the blend state is ONE, INV_SRC_ALPHA and the
// pixel shaders multiply straight through with no unpremultiply step. Geometry with no texture
// is drawn against a one pixel white texture rather than a second pipeline, which keeps the draw
// loop to a single state and one branch fewer per call.

struct UiConstants {
    // Projection and translation already combined on the CPU. Keeping the translation out of the
    // push constants means one range rather than two, and the matrix has to be rebuilt per draw
    // anyway because RmlUi gives the translation per geometry.
    //
    // ROW MAJOR IS NOT THE DEFAULT and saying so here is not decoration. HLSL packs a float4x4
    // column major unless told otherwise, so a row major matrix sent from C++ is read as its own
    // transpose: no error, no warning, and every vertex lands off screen.
    row_major float4x4 transform;
    // x is the time in seconds since the interface came up, for the mark's gleam. The rest is
    // spare and zero.
    float4 mark;
};

[[vk::push_constant]] ConstantBuffer<UiConstants> gConstants : register(b0);

Texture2D<float4> gTexture : register(t0);
SamplerState gSampler : register(s0);

struct VSInput {
    float2 position : POSITION;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD;
};

struct PSInput {
    float4 position : SV_POSITION;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    output.position = mul(gConstants.transform, float4(input.position, 0.0f, 1.0f));
    output.color = input.color;
    output.uv = input.uv;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return input.color * gTexture.Sample(gSampler, input.uv);
}

// ----------------------------------------------------------------------------------------------
// The mark (phase 50): our three stacked triangles, lit by the gleam exactly as the design
// decision's snapshots draw it (9693d652-0b2a-43e6-ae79-09ea2382cad3, the panel header's mark,
// .p-h h4::before): a linear gradient at 100 degrees, three times the element's width, with its
// band of light at 36% to 64%, sliding across the mask from a position of 120% to -20% between
// 66% and 88% of a five second cycle, linear (d-a-mark-gleam), and the mask fitted to the element
// and centered. The mask is the decision's path, three triangles in a box 24 wide with y from 1 to
// 21. The colors are tokens.rcss's brass quartet; a shader cannot read a stylesheet, so they are
// repeated here under that name.
//
// The element's geometry comes from RmlUi's shader decorator, with texcoords 0 to 1 across the
// element's box and a premultiplied white vertex color carrying the element's opacity.
// ----------------------------------------------------------------------------------------------

static const float3 BRASS       = float3(0xB8, 0x91, 0x2F) / 255.0f;
static const float3 BRASS_LIGHT = float3(0xE8, 0xCD, 0x7E) / 255.0f;
static const float3 BRASS_PEAK  = float3(0xFF, 0xF7, 0xD8) / 255.0f;
static const float3 BRASS_DEEP  = float3(0xA8, 0x80, 0x1F) / 255.0f;

static const float2 MARK_BOX = float2(24.0f, 20.0f);
static const float MARK_BOX_TOP = 1.0f;
static const float GLEAM_PERIOD = 5.0f;
static const float GLEAM_FROM = 0.66f;
static const float GLEAM_TO = 0.88f;
static const float GLEAM_ANGLE = radians(100.0f);

// How far outside a triangle a point is, in the box's units: negative inside, the largest of the
// three edges' distances. Exact along the edges, which is where the antialiasing needs it.
float triangle_outside(float2 p, float2 a, float2 b, float2 c) {
    const float winding = sign((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
    float2 e0 = b - a;
    float2 e1 = c - b;
    float2 e2 = a - c;
    float d0 = ((p.x - a.x) * e0.y - (p.y - a.y) * e0.x) / length(e0);
    float d1 = ((p.x - b.x) * e1.y - (p.y - b.y) * e1.x) / length(e1);
    float d2 = ((p.x - c.x) * e2.y - (p.y - c.y) * e2.x) / length(e2);
    return max(max(d0, d1), d2) * winding;
}

float3 gleam_color(float t) {
    if (t <= 0.36f || t >= 0.64f) return BRASS;
    if (t < 0.43f) return lerp(BRASS, BRASS_LIGHT, (t - 0.36f) / 0.07f);
    if (t < 0.47f) return lerp(BRASS_LIGHT, BRASS_PEAK, (t - 0.43f) / 0.04f);
    if (t < 0.51f) return lerp(BRASS_PEAK, BRASS_LIGHT, (t - 0.47f) / 0.04f);
    if (t < 0.57f) return lerp(BRASS_LIGHT, BRASS_DEEP, (t - 0.51f) / 0.06f);
    return lerp(BRASS_DEEP, BRASS, (t - 0.57f) / 0.07f);
}

float4 PSMark(PSInput input) : SV_TARGET {
    // The element's size in pixels, from how fast the texcoords change per pixel.
    const float2 uv_per_pixel = max(fwidth(input.uv), 1e-5f);
    const float2 size = 1.0f / uv_per_pixel;
    const float2 xy = input.uv * size;

    // The mask, fitted and centered: the decision's `center / contain`.
    const float scale = min(size.x / MARK_BOX.x, size.y / MARK_BOX.y);
    const float2 offset = (size - MARK_BOX * scale) * 0.5f;
    const float2 p = (xy - offset) / scale + float2(0.0f, MARK_BOX_TOP);
    const float pixel = 1.0f / scale;   // one pixel, in box units

    const float top   = triangle_outside(p, float2(12.0f, 1.0f),  float2(17.6f, 10.6f), float2(6.4f, 10.6f));
    const float left  = triangle_outside(p, float2(5.9f, 11.6f),  float2(11.5f, 21.0f), float2(0.3f, 21.0f));
    const float right = triangle_outside(p, float2(18.1f, 11.6f), float2(23.7f, 21.0f), float2(12.5f, 21.0f));
    const float outside = min(min(top, left), right);
    const float coverage = saturate(0.5f - outside / pixel);

    // The gradient's position: 120% until 66% of the cycle, then to -20% by 88%, then held.
    const float cycle = frac(gConstants.mark.x / GLEAM_PERIOD);
    const float slide = saturate((cycle - GLEAM_FROM) / (GLEAM_TO - GLEAM_FROM));
    const float position = 1.2f - 1.4f * slide;

    // CSS: a background three times the element's width, placed at `position` (the image's
    // point at that percentage over the element's), and a gradient line at 100 degrees through
    // its center whose length is the box's projection onto that direction.
    const float2 box = float2(3.0f * size.x, size.y);
    const float box_left = (size.x - box.x) * position;
    const float2 center = float2(box_left, 0.0f) + box * 0.5f;
    const float2 direction = float2(sin(GLEAM_ANGLE), -cos(GLEAM_ANGLE));
    const float line_length = abs(box.x * direction.x) + abs(box.y * direction.y);
    const float t = 0.5f + dot(xy - center, direction) / line_length;

    const float3 color = gleam_color(t);
    return float4(color * coverage, coverage) * input.color;
}
