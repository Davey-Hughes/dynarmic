/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <cstddef>
#include <vector>

namespace Dynarmic::Common {

class Pool {
public:
    /**
     * @param object_size Byte-size of objects to construct
     * @param initial_pool_size Number of objects to have per slab
     */
    Pool(size_t object_size, size_t initial_pool_size);
    ~Pool();

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    /// A moved-from pool holds no slab until its next Alloc().
    Pool(Pool&&) noexcept;
    Pool& operator=(Pool&&) noexcept;

    /// Returns a pointer to an `object_size`-bytes block of memory.
    void* Alloc();

private:
    // Allocates a completely new memory slab.
    // Used when an entirely new slab is needed
    // due the current one running out of usable space.
    void AllocateNewSlab();

    void FreeSlabs();

    size_t object_size;
    size_t slab_size;
    char* current_slab;
    char* current_ptr;
    size_t remaining;
    std::vector<char*> slabs;
};

}  // namespace Dynarmic::Common
