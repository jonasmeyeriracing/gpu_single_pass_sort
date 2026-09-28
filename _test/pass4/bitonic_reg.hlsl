// Register / wave bitonic sort (see bitonic_reg.hlsli) as a size tier: sorts of MIN_COUNT..MAX_COUNT
// elements, one group per sort. GROUP_SIZE must be >= 8192 / BR_ELEMS.
#include "common.hlsli"
#include "bitonic_reg.hlsli"

[numthreads(GROUP_SIZE, 1, 1)]
WAVE_SIZE_ATTR
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

    BitonicRegSort(tid, offset, count);
}
