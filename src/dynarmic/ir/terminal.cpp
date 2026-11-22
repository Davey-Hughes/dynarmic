/* This file is part of the dynarmic project.
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/ir/terminal.h"

#include <algorithm>
#include <cstdlib>
#include <new>

#include <mcl/assert.hpp>

namespace Dynarmic::IR::Term::detail {

namespace {

constexpr std::size_t node_size = std::max({sizeof(If), sizeof(CheckBit), sizeof(CheckHalt)});
// A block's terminal is copied a few times between translation and emission, so only a few nodes are
// live at once. Freed nodes past this many go back to the heap.
constexpr std::size_t max_free_nodes = 64;

// Trivially destructible on purpose: a thread_local with a destructor keeps a dlopen'ed library loaded
// while the thread lives. A thread that exits leaks its free nodes (at most max_free_nodes).
struct FreeList {
    void* head = nullptr;
    std::size_t count = 0;
};

thread_local FreeList free_list;

}  // namespace

void* AllocateNestedTerminal(std::size_t size) {
    ASSERT(size <= node_size);
    if (void* const node = free_list.head) {
        free_list.head = *static_cast<void**>(node);
        --free_list.count;
        return node;
    }
    if (void* const node = std::malloc(node_size)) {
        return node;
    }
    throw std::bad_alloc{};
}

void FreeNestedTerminal(void* p) noexcept {
    if (!p) {
        return;
    }
    if (free_list.count == max_free_nodes) {
        std::free(p);
        return;
    }
    *static_cast<void**>(p) = free_list.head;
    free_list.head = p;
    ++free_list.count;
}

}  // namespace Dynarmic::IR::Term::detail
