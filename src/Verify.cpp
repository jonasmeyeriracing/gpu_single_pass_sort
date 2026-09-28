#include "Verify.h"

#include "Common.h"

#include <algorithm>
#include <vector>

bool VerifyIteration(const IterationData& input, const uint32_t* output, std::string* message)
{
    thread_local std::vector<uint32_t> expected;
    thread_local std::vector<uint32_t> actual;

    for (uint32_t s = 0; s < kSortsPerIteration; ++s)
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
        std::sort(expected.begin(), expected.end());
        std::sort(actual.begin(), actual.end());
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

    // Stray writes past the last sort (e.g. a rank >= count) land here.
    for (uint32_t i = static_cast<uint32_t>(input.elements.size()); i < kMaxElementsPerIteration; ++i)
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
