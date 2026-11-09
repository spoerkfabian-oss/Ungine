#pragma once
// Test helper: watches single heap allocations on the calling thread. Between Begin(limit) and End(),
// a request larger than `limit` bytes is recorded and fails with std::bad_alloc instead of being
// served, so tests can prove that malformed input never triggers huge allocations.
#include <cstddef>

namespace AllocationProbe {

void        Begin(std::size_t limit);
std::size_t End(); // largest request seen (including refused ones)
bool        Exceeded();

} // namespace AllocationProbe
