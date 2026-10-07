// A triangle covering the whole viewport, from SV_VertexID 0..2.

float4 main(uint vertex_id : SV_VertexID) : SV_Position {
  float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
  return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
