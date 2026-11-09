// Replaces the global operator new/delete of EngineTests (forwarding to malloc/free) to observe
// allocation sizes on one thread; see AllocationProbe.h.
#include "AllocationProbe.h"

#include <cstdlib>
#include <new>

namespace {
thread_local bool        t_active   = false;
thread_local bool        t_exceeded = false;
thread_local std::size_t t_limit    = 0;
thread_local std::size_t t_largest  = 0;
} // namespace

namespace AllocationProbe {

void Begin(std::size_t limit)
{
    t_limit    = limit;
    t_largest  = 0;
    t_exceeded = false;
    t_active   = true;
}

std::size_t End()
{
    t_active = false;
    return t_largest;
}

bool Exceeded() { return t_exceeded; }

} // namespace AllocationProbe

void* operator new(std::size_t size)
{
    if (t_active) {
        if (size > t_largest)
            t_largest = size;
        if (size > t_limit) {
            t_exceeded = true;
            throw std::bad_alloc();
        }
    }
    if (void* p = std::malloc(size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
