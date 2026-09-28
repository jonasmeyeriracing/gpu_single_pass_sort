#pragma once

#include "Workloads.h"

#include <string>

// The output buffer is filled with this before every sort; padding between sorts must keep it.
constexpr uint32_t kPoisonValue = 0xDEADBEEFu;

// Checks one iteration's GPU output: the whole output buffer (kMaxElementsPerIteration values, same
// layout as input.elements):
//  - keys (high 16 bits) are non-decreasing within each sort
//  - each sort's output multiset of full 32-bit values equals its input multiset
//  - the padding after each sort (up to the next 64-element boundary) was not written
//  - nothing past the end of the last sort (up to the end of the buffer) was written
// Order among equal keys is unspecified. On failure returns false and describes the first problem.
bool VerifyIteration(const IterationData& input, const uint32_t* output, std::string* message);
