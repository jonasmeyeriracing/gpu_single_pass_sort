// pass0 baseline: one group per sort, whole sort in groupshared memory, classic bitonic sort
// with a group barrier after every compare-exchange stage. Sorts are padded to the next power
// of two with 0xFFFFFFFF (sorts last). Compares full 32-bit values (key in the high 16 bits).
#include "common.hlsli"

// 8192 * 4 bytes = 32 KB = the entire D3D12 groupshared limit; no other groupshared allowed.
groupshared uint gsData[MAX_SORT_SIZE];

[numthreads(GROUP_SIZE, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint tid : SV_GroupIndex)
{
    const uint sortIndex = groupId.x;
    if (sortIndex >= gNumSorts)
        return;

    const uint2 desc = gSortDescs[sortIndex];
    const uint offset = desc.x;
    const uint count = desc.y;
    if (count == 0 || count < MIN_COUNT || count > MAX_COUNT)
        return;

    // Next power of two >= count.
    const uint n = (count <= 1) ? 1 : (1u << firstbithigh(count - 1)) << 1;

    for (uint i = tid; i < n; i += GROUP_SIZE)
        gsData[i] = (i < count) ? gInput[offset + i] : 0xFFFFFFFFu;
    GroupMemoryBarrierWithGroupSync();

    const uint halfN = n >> 1;
    for (uint k = 2; k <= n; k <<= 1)
    {
        for (uint j = k >> 1; j > 0; j >>= 1)
        {
            for (uint p = tid; p < halfN; p += GROUP_SIZE)
            {
                // p-th pair of this stage: 'lo' has bit j clear, 'hi' = lo + j.
                const uint lo = ((p & ~(j - 1)) << 1) | (p & (j - 1));
                const uint hi = lo | j;
                const bool ascending = (lo & k) == 0;
                const uint a = gsData[lo];
                const uint b = gsData[hi];
                if ((a > b) == ascending)
                {
                    gsData[lo] = b;
                    gsData[hi] = a;
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }

    for (uint i = tid; i < count; i += GROUP_SIZE)
        gOutput[offset + i] = gsData[i];
}
