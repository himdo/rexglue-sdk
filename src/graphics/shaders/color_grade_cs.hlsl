// Swap colour grade: runs in place on the gamma-corrected guest output, after
// the gamma ramp and before FXAA / presentation / screenshot capture. Driven by
// rex_set_swap_color_grade (the game's photo mode filters).
//
// Compile (from this directory):
//   fxc /nologo /T cs_5_1 /E main /O3 /Vn color_grade_cs
//       /Fh bytecode/d3d12_5_1/color_grade_cs.h color_grade_cs.hlsl

cbuffer XeColorGradeConstants : register(b0) {
  uint2 xe_grade_size;
  float xe_grade_contrast;   // power curve around the pivot (shadows.w)
  float xe_grade_gamma;      // applied after contrast (> 1 darkens mid tones)
  float4 xe_grade_row_r;     // colour matrix rows: rgb weights, w = offset
  float4 xe_grade_row_g;
  float4 xe_grade_row_b;
  float4 xe_grade_shadows;   // rgb offset weighted towards the shadows,
                             // w = contrast pivot (mid grey, e.g. 0.4)
  float4 xe_grade_highlights;// rgb offset weighted towards the highlights
  float4 xe_grade_fade;      // rgb colour, w = amount (lifted / faded blacks)
};

RWTexture2D<unorm float4> xe_grade_dest : register(u0);

[numthreads(16, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  uint2 pixel = xe_thread_id.xy;
  if (any(pixel >= xe_grade_size)) {
    return;
  }
  float4 texel = xe_grade_dest[pixel];
  float3 c = texel.rgb;
  c = float3(dot(xe_grade_row_r.rgb, c) + xe_grade_row_r.w,
             dot(xe_grade_row_g.rgb, c) + xe_grade_row_g.w,
             dot(xe_grade_row_b.rgb, c) + xe_grade_row_b.w);
  c = saturate(c);
  // Contrast that keeps black, white and the pivot fixed (a plain stretch
  // around 0.5 crushes the game's mostly dark frames).
  float p = clamp(xe_grade_shadows.w, 0.05f, 0.95f);
  float3 lo = p * pow(max(c / p, 1.0e-6f), xe_grade_contrast);
  float3 hi = 1.0f - (1.0f - p) * pow(max((1.0f - c) / (1.0f - p), 1.0e-6f),
                                       xe_grade_contrast);
  c = (c < p) ? lo : hi;
  c = pow(max(c, 1.0e-5f), xe_grade_gamma);
  float luma = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
  float shadow = (1.0f - luma) * (1.0f - luma);
  c += xe_grade_shadows.rgb * shadow + xe_grade_highlights.rgb * (luma * luma);
  c = lerp(c, xe_grade_fade.rgb, xe_grade_fade.w);
  xe_grade_dest[pixel] = float4(saturate(c), texel.a);
}
