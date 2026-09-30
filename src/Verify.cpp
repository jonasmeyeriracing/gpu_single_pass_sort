#include "Verify.h"

#include "Common.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
// Sorts v ascending (full 32-bit values): LSD radix sort, 4 passes of 8 bits, one histogram pass for
// all digits, and a pass is skipped if every value has the same digit there (e.g. a constant key
// digit of sparse_keys). std::sort below 64 values. tmp: scratch of the same size.
void SortValues(std::vector<uint32_t>& v, std::vector<uint32_t>& tmp)
{
    const size_t n = v.size();
    if (n < 64)
    {
        std::sort(v.begin(), v.end());
        return;
    }
    uint32_t hist[4][256];
    memset(hist, 0, sizeof(hist));
    for (uint32_t x : v)
    {
        ++hist[0][x & 0xFF];
        ++hist[1][(x >> 8) & 0xFF];
        ++hist[2][(x >> 16) & 0xFF];
        ++hist[3][x >> 24];
    }
    tmp.resize(n);
    uint32_t* src = v.data();
    uint32_t* dst = tmp.data();
    for (uint32_t pass = 0; pass < 4; ++pass)
    {
        const uint32_t shift = 8 * pass;
        uint32_t* h = hist[pass];
        if (h[(src[0] >> shift) & 0xFF] == n)
            continue; // every value has this digit
        uint32_t sum = 0;
        for (uint32_t d = 0; d < 256; ++d)
        {
            const uint32_t c = h[d];
            h[d] = sum;
            sum += c;
        }
        for (size_t i = 0; i < n; ++i)
        {
            const uint32_t x = src[i];
            dst[h[(x >> shift) & 0xFF]++] = x;
        }
        std::swap(src, dst);
    }
    if (src != v.data())
        memcpy(v.data(), src, n * sizeof(uint32_t));
}
} // namespace

bool VerifySorts(const IterationData& input, const uint32_t* output, uint32_t firstSort, uint32_t endSort,
                 std::string* message)
{
    thread_local std::vector<uint32_t> expected;
    thread_local std::vector<uint32_t> actual;
    thread_local std::vector<uint32_t> scratch;

    for (uint32_t s = firstSort; s < endSort; ++s)
    {
        const uint32_t offset = input.sorts[s].offset;
        const uint32_t count = input.sorts[s].count;
        const uint32_t end = offset + count;
        const uint32_t paddedEnd = (end + kOffsetAlignment - 1) / kOffsetAlignment * kOffsetAlignment;

        for (uint32_t i = end; i < paddedEnd; ++i)
        {
            if (output[i] != kPoisonValue)
            {
                if (message)
                    *message = Format("sort %u (count %u): padding element %u was overwritten (0x%08X)", s, count,
                                      i - offset, output[i]);
                return false;
            }
        }

        for (uint32_t i = offset + 1; i < end; ++i)
        {
            if ((output[i] >> 16) < (output[i - 1] >> 16))
            {
                if (message)
                    *message = Format("sort %u (count %u): key order violated at index %u (0x%04X after 0x%04X)", s,
                                      count, i - offset, output[i] >> 16, output[i - 1] >> 16);
                return false;
            }
        }

        expected.assign(input.elements.begin() + offset, input.elements.begin() + end);
        actual.assign(output + offset, output + end);
        SortValues(expected, scratch);
        SortValues(actual, scratch);
        const auto mismatch = std::mismatch(expected.begin(), expected.end(), actual.begin());
        if (mismatch.first != expected.end())
        {
            if (message)
                *message = Format("sort %u (count %u): output is not a permutation of the input "
                                  "(sorted multisets differ at rank %u: expected 0x%08X, got 0x%08X)",
                                  s, count, static_cast<uint32_t>(mismatch.first - expected.begin()), *mismatch.first,
                                  *mismatch.second);
            return false;
        }
    }
    return true;
}

bool VerifyTail(const IterationData& input, const uint32_t* output, uint32_t begin, uint32_t end,
                std::string* message)
{
    // Stray writes past the last sort (e.g. a rank >= count) land here.
    for (uint32_t i = begin; i < end; ++i)
    {
        if (output[i] != kPoisonValue)
        {
            if (message)
                *message = Format("element %u past the end of the last sort (end %zu) was overwritten (0x%08X)", i,
                                  input.elements.size(), output[i]);
            return false;
        }
    }
    return true;
}

bool VerifyIteration(const IterationData& input, const uint32_t* output, uint32_t bufferElements,
                     std::string* message)
{
    return VerifySorts(input, output, 0, static_cast<uint32_t>(input.sorts.size()), message) &&
           VerifyTail(input, output, static_cast<uint32_t>(input.elements.size()), bufferElements, message);
}
