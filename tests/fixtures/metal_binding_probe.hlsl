ByteAddressBuffer Input : register(t1, space0);
RWByteAddressBuffer Output : register(u0, space0);

[numthreads(1, 1, 1)]
void CSMain(uint3 threadID : SV_DispatchThreadID) {
    Output.Store(0, Input.Load(0) + 7);
}
