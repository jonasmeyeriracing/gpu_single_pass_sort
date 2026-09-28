// Rank sort: one group per sort, for sorts of MIN_COUNT..MAX_COUNT elements.
//
// Every element gets a unique 32-bit sort word (key16 << 16) | localIndex (localIndex < 8192 fits in
// the low 16 bits), stored in groupshared memory. Because all words are unique, an element's output
// position is simply the number of words strictly less than its own: one compare per pair, no
// tie-break term, and the result is stable (equal keys keep input order).
//
// Each thread owns up to ELEMS_PER_THREAD elements (tid, tid + GROUP_SIZE, ...) and loops over all
// words in groupshared memory. All lanes read the same address (broadcast, conflict-free), 8 words
// per iteration (as 8 scalar reads, which the compiler may merge), and every loaded word is compared against all of the thread's
// elements, amortizing the LDS reads. The word array is padded to a multiple of 8 with 0xFFFFFFFF,
// which is strictly greater than any real word (max 0xFFFF1FFF), so padding never adds to a rank.
//
// The output is the original 32-bit input value (key + payload), kept in a register from the load.
#include "common.hlsli"

#ifndef ELEMS_PER_THREAD
#define ELEMS_PER_THREAD ((MAX_COUNT + GROUP_SIZE - 1) / GROUP_SIZE)
#endif

#if MAX_COUNT > MAX_SORT_SIZE
#error "MAX_COUNT exceeds MAX_SORT_SIZE"
#endif
#if ELEMS_PER_THREAD * GROUP_SIZE < MAX_COUNT
#error "ELEMS_PER_THREAD * GROUP_SIZE must cover MAX_COUNT"
#endif

// Words are processed 8 at a time per loop iteration.
#define WORDS_PER_STEP 8
#define PADDED_MAX_COUNT ((MAX_COUNT + WORDS_PER_STEP - 1) / WORDS_PER_STEP * WORDS_PER_STEP)

static const uint kPadWord = 0xFFFFFFFFu;

// At most 8192 * 4 bytes = 32 KB; the tiers used in practice need 0.25-8 KB.
// Plain scalar array: every store is a whole 32-bit word, so no component read-modify-write
// (a uint4 array with dynamic component stores compiles to the same scalar stores, but this way
// it does not depend on the compiler).
groupshared uint gsWords[PADDED_MAX_COUNT];

// Words 4*q .. 4*q+3 as a uint4 (four scalar LDS reads).
uint4 LoadWords4(uint q)
{
    return uint4(gsWords[4 * q + 0], gsWords[4 * q + 1], gsWords[4 * q + 2], gsWords[4 * q + 3]);
}

// Per component: 1 if that word of v is strictly less than w, else 0.
uint4 LessThan4(uint4 v, uint w)
{
    return (uint4)(v < w);
}

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

    const uint paddedCount = (count + WORDS_PER_STEP - 1) / WORDS_PER_STEP * WORDS_PER_STEP;
    const uint numSteps = paddedCount / WORDS_PER_STEP;

    // Load this thread's elements, build their sort words and publish them to groupshared memory.
    uint value[ELEMS_PER_THREAD];
    uint word[ELEMS_PER_THREAD];
    [unroll]
    for (uint e = 0; e < ELEMS_PER_THREAD; ++e)
    {
        const uint i = tid + e * GROUP_SIZE;
        value[e] = 0;
        word[e] = kPadWord;
        if (i < count)
        {
            value[e] = gInput[offset + i];
            word[e] = (value[e] & 0xFFFF0000u) | i;
            gsWords[i] = word[e];
        }
    }
    // Pad the tail up to a multiple of WORDS_PER_STEP (at most 7 words).
    if (tid < paddedCount - count)
    {
        const uint i = count + tid;
        gsWords[i] = kPadWord;
    }
    GroupMemoryBarrierWithGroupSync();

    // Thread owns no element at all (element 0 is the lowest index it could own).
    if (tid >= count)
        return;

#if ELEMS_PER_THREAD > 1
    // Group-uniform: when the sort fits in one element per thread, skip the unused compares.
    if (count <= GROUP_SIZE)
#endif
    {
        uint4 acc = 0;
        for (uint s = 0; s < numSteps; ++s)
        {
            const uint4 a = LoadWords4(2 * s + 0);
            const uint4 b = LoadWords4(2 * s + 1);
            acc += LessThan4(a, word[0]);
            acc += LessThan4(b, word[0]);
        }
        const uint rank = acc.x + acc.y + acc.z + acc.w;
        gOutput[offset + rank] = value[0];
        return;
    }

#if ELEMS_PER_THREAD > 1
    uint4 acc[ELEMS_PER_THREAD];
    [unroll]
    for (uint e = 0; e < ELEMS_PER_THREAD; ++e)
        acc[e] = 0;
    for (uint s = 0; s < numSteps; ++s)
    {
        const uint4 a = LoadWords4(2 * s + 0);
        const uint4 b = LoadWords4(2 * s + 1);
        [unroll]
        for (uint e = 0; e < ELEMS_PER_THREAD; ++e)
        {
            acc[e] += LessThan4(a, word[e]);
            acc[e] += LessThan4(b, word[e]);
        }
    }
    [unroll]
    for (uint e = 0; e < ELEMS_PER_THREAD; ++e)
    {
        // Elements past the end have word == kPadWord and are not written.
        if (tid + e * GROUP_SIZE < count)
        {
            const uint rank = acc[e].x + acc[e].y + acc[e].z + acc[e].w;
            gOutput[offset + rank] = value[e];
        }
    }
#endif
}
