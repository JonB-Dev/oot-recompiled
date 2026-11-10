RaytracingAccelerationStructure Scene : register(t0, space0);
RWByteAddressBuffer Output : register(u1, space0);

[shader("raygeneration")]
void TestRayGen() {
    RayDesc ray;
    ray.Origin = float3(0.0, 0.0, 1.0);
    ray.Direction = float3(0.0, 0.0, -1.0);
    ray.TMin = 0.0;
    ray.TMax = 10.0;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(Scene, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) {}
    Output.Store(0, query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? asuint(query.CommittedRayT()) : 0);
}
