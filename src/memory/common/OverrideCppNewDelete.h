#pragma once

#include <cstdalign>
#include <cstdlib>

#include "GlobalMemoryAllocator.h"

#ifdef OVERRIDE_CXX_NEW_DELETE

// Override global new/delete with custom memory allocator.
void *operator new(size_t size) { return hf3fs::memory::allocate(size); }

void operator delete(void *mem) noexcept { hf3fs::memory::deallocate(mem); }

void *operator new[](size_t size) { return hf3fs::memory::allocate(size); }

void operator delete[](void *mem) noexcept { hf3fs::memory::deallocate(mem); }

#endif
