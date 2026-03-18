/*
 * Master of Puppets — Vulkan Backend
 * vulkan_suballoc.c — Memory suballocator (pool + linear allocators)
 *
 * Reduces vkAllocateMemory calls from hundreds to single digits by
 * grouping allocations into large blocks per memory type.
 *
 * Two allocation strategies:
 *   - Linear: for per-frame transient buffers (UBO, staging). Bump pointer,
 *     reset at frame start.  O(1) alloc, O(1) reset.
 *   - Free-list: for persistent resources (mesh VBOs, textures).
 *     First-fit with coalescing on free.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#if defined(MOP_HAS_VULKAN)

#include "vulkan_internal.h"
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

#define MOP_SUBALLOC_BLOCK_SIZE_PERSISTENT (64 * 1024 * 1024) /* 64 MB */
#define MOP_SUBALLOC_BLOCK_SIZE_TRANSIENT (16 * 1024 * 1024)  /* 16 MB */
#define MOP_SUBALLOC_MAX_BLOCKS 64
#define MOP_SUBALLOC_MAX_FREE_NODES 4096

/* -------------------------------------------------------------------------
 * Free-list node (intrusive linked list within a block)
 * ------------------------------------------------------------------------- */

typedef struct MopSuballocFreeNode {
  VkDeviceSize offset;
  VkDeviceSize size;
  struct MopSuballocFreeNode *next;
} MopSuballocFreeNode;

/* -------------------------------------------------------------------------
 * Memory block — one large vkAllocateMemory
 * ------------------------------------------------------------------------- */

typedef struct MopSuballocBlock {
  VkDeviceMemory memory;
  VkDeviceSize size;
  uint32_t memory_type_index;
  void *mapped; /* non-NULL if host-visible */

  /* Linear allocator state */
  VkDeviceSize linear_offset; /* current bump pointer */

  /* Free-list allocator state */
  MopSuballocFreeNode *free_list;
  uint32_t alloc_count; /* outstanding allocations (for block reclaim) */
} MopSuballocBlock;

/* -------------------------------------------------------------------------
 * MopSuballocator — top-level allocator
 * ------------------------------------------------------------------------- */

struct MopSuballocator {
  VkDevice device;
  VkPhysicalDeviceMemoryProperties mem_props;
  VkDeviceSize min_alignment; /* nonCoherentAtomSize or similar */

  MopSuballocBlock blocks[MOP_SUBALLOC_MAX_BLOCKS];
  uint32_t block_count;

  /* Free node pool (avoid malloc per node) */
  MopSuballocFreeNode node_pool[MOP_SUBALLOC_MAX_FREE_NODES];
  uint32_t node_pool_next;

  /* Stats */
  uint32_t total_vk_allocs;
  uint64_t total_bytes_allocated;
  uint64_t total_bytes_used;
};

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

static VkDeviceSize suballoc_align(VkDeviceSize value, VkDeviceSize alignment) {
  if (alignment == 0)
    return value;
  return (value + alignment - 1) & ~(alignment - 1);
}

static MopSuballocFreeNode *suballoc_alloc_node(MopSuballocator *sa) {
  if (sa->node_pool_next >= MOP_SUBALLOC_MAX_FREE_NODES)
    return NULL;
  MopSuballocFreeNode *n = &sa->node_pool[sa->node_pool_next++];
  memset(n, 0, sizeof(*n));
  return n;
}

/* Find or create a block for the given memory type with enough space */
static MopSuballocBlock *
suballoc_find_or_create_block(MopSuballocator *sa, uint32_t memory_type_index,
                              VkDeviceSize required_size, bool persistent,
                              VkMemoryPropertyFlags mem_flags) {

  VkDeviceSize block_size = persistent ? MOP_SUBALLOC_BLOCK_SIZE_PERSISTENT
                                       : MOP_SUBALLOC_BLOCK_SIZE_TRANSIENT;

  /* Ensure block is large enough for the allocation */
  if (required_size > block_size)
    block_size = required_size;

  /* Search existing blocks of the same memory type */
  for (uint32_t i = 0; i < sa->block_count; i++) {
    MopSuballocBlock *b = &sa->blocks[i];
    if (b->memory_type_index != memory_type_index)
      continue;

    if (!persistent) {
      /* Linear: check if there's space at the bump pointer */
      VkDeviceSize aligned =
          suballoc_align(b->linear_offset, sa->min_alignment);
      if (aligned + required_size <= b->size)
        return b;
    } else {
      /* Free-list: search for a fitting free node */
      MopSuballocFreeNode *node = b->free_list;
      while (node) {
        VkDeviceSize aligned_off =
            suballoc_align(node->offset, sa->min_alignment);
        VkDeviceSize padding = aligned_off - node->offset;
        if (node->size >= required_size + padding)
          return b;
        node = node->next;
      }
    }
  }

  /* No suitable block found — allocate a new one */
  if (sa->block_count >= MOP_SUBALLOC_MAX_BLOCKS) {
    MOP_WARN("[VK suballoc] max blocks reached (%u)", MOP_SUBALLOC_MAX_BLOCKS);
    return NULL;
  }

  VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = block_size,
      .memoryTypeIndex = memory_type_index,
  };

  MopSuballocBlock *b = &sa->blocks[sa->block_count];
  memset(b, 0, sizeof(*b));
  b->memory_type_index = memory_type_index;
  b->size = block_size;

  VkResult r = vkAllocateMemory(sa->device, &ai, NULL, &b->memory);
  if (r != VK_SUCCESS) {
    MOP_ERROR("[VK suballoc] vkAllocateMemory failed: %d (size=%llu)", r,
              (unsigned long long)block_size);
    return NULL;
  }

  sa->total_vk_allocs++;
  sa->total_bytes_allocated += block_size;

  /* Map if host-visible */
  if (mem_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
    vkMapMemory(sa->device, b->memory, 0, block_size, 0, &b->mapped);
  }

  /* Initialize free list with the entire block */
  if (persistent) {
    MopSuballocFreeNode *node = suballoc_alloc_node(sa);
    if (node) {
      node->offset = 0;
      node->size = block_size;
      node->next = NULL;
      b->free_list = node;
    }
  }

  sa->block_count++;
  return b;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

MopSuballocator *
mop_suballoc_create(VkDevice device,
                    const VkPhysicalDeviceMemoryProperties *props,
                    VkDeviceSize min_alignment) {
  MopSuballocator *sa = calloc(1, sizeof(MopSuballocator));
  if (!sa)
    return NULL;
  sa->device = device;
  sa->mem_props = *props;
  sa->min_alignment = min_alignment > 0 ? min_alignment : 256;
  return sa;
}

void mop_suballoc_destroy(MopSuballocator *sa) {
  if (!sa)
    return;
  for (uint32_t i = 0; i < sa->block_count; i++) {
    if (sa->blocks[i].mapped)
      vkUnmapMemory(sa->device, sa->blocks[i].memory);
    vkFreeMemory(sa->device, sa->blocks[i].memory, NULL);
  }
  MOP_INFO(
      "[VK suballoc] destroyed: %u VK allocs, %llu bytes allocated, %llu used",
      sa->total_vk_allocs, (unsigned long long)sa->total_bytes_allocated,
      (unsigned long long)sa->total_bytes_used);
  free(sa);
}

MopSuballocResult mop_suballoc_alloc(MopSuballocator *sa, VkDeviceSize size,
                                     VkDeviceSize alignment,
                                     uint32_t memory_type_index,
                                     VkMemoryPropertyFlags mem_flags,
                                     bool persistent) {
  MopSuballocResult result = {0};
  if (!sa || size == 0)
    return result;

  if (alignment < sa->min_alignment)
    alignment = sa->min_alignment;

  MopSuballocBlock *block = suballoc_find_or_create_block(
      sa, memory_type_index, size, persistent, mem_flags);
  if (!block)
    return result;

  if (!persistent) {
    /* Linear allocation: bump pointer */
    VkDeviceSize aligned = suballoc_align(block->linear_offset, alignment);
    if (aligned + size > block->size)
      return result; /* shouldn't happen — we checked in find_or_create */

    result.memory = block->memory;
    result.offset = aligned;
    result.size = size;
    result.mapped = block->mapped ? (uint8_t *)block->mapped + aligned : NULL;
    result.success = true;

    block->linear_offset = aligned + size;
    block->alloc_count++;
    sa->total_bytes_used += size;
  } else {
    /* Free-list allocation: first-fit */
    MopSuballocFreeNode **prev_ptr = &block->free_list;
    MopSuballocFreeNode *node = block->free_list;
    while (node) {
      VkDeviceSize aligned_off = suballoc_align(node->offset, alignment);
      VkDeviceSize padding = aligned_off - node->offset;
      if (node->size >= size + padding) {
        result.memory = block->memory;
        result.offset = aligned_off;
        result.size = size;
        result.mapped =
            block->mapped ? (uint8_t *)block->mapped + aligned_off : NULL;
        result.success = true;

        /* Shrink or remove the free node */
        VkDeviceSize remaining = node->size - size - padding;
        if (remaining > sa->min_alignment) {
          node->offset = aligned_off + size;
          node->size = remaining;
          /* If there was padding at the front, create a new free node */
          if (padding > 0) {
            MopSuballocFreeNode *pad_node = suballoc_alloc_node(sa);
            if (pad_node) {
              pad_node->offset = node->offset - padding - size;
              pad_node->size = padding;
              pad_node->next = node;
              *prev_ptr = pad_node;
            }
          }
        } else {
          /* Remove node entirely (padding + remaining are small) */
          *prev_ptr = node->next;
        }

        block->alloc_count++;
        sa->total_bytes_used += size;
        return result;
      }
      prev_ptr = &node->next;
      node = node->next;
    }
  }

  return result;
}

void mop_suballoc_free(MopSuballocator *sa, VkDeviceMemory memory,
                       VkDeviceSize offset, VkDeviceSize size) {
  if (!sa || !memory)
    return;

  /* Find the block */
  for (uint32_t i = 0; i < sa->block_count; i++) {
    MopSuballocBlock *b = &sa->blocks[i];
    if (b->memory != memory)
      continue;

    /* Insert into free list (sorted by offset for coalescing) */
    MopSuballocFreeNode *new_node = suballoc_alloc_node(sa);
    if (!new_node)
      return;
    new_node->offset = offset;
    new_node->size = size;

    /* Insert sorted */
    MopSuballocFreeNode **prev_ptr = &b->free_list;
    while (*prev_ptr && (*prev_ptr)->offset < offset)
      prev_ptr = &(*prev_ptr)->next;
    new_node->next = *prev_ptr;
    *prev_ptr = new_node;

    /* Coalesce with next */
    if (new_node->next &&
        new_node->offset + new_node->size >= new_node->next->offset) {
      MopSuballocFreeNode *merged = new_node->next;
      new_node->size = (merged->offset + merged->size) - new_node->offset;
      new_node->next = merged->next;
    }

    /* Coalesce with prev */
    if (prev_ptr != &b->free_list) {
      /* Walk from head to find actual prev */
      MopSuballocFreeNode *prev = b->free_list;
      while (prev && prev->next != new_node)
        prev = prev->next;
      if (prev && prev->offset + prev->size >= new_node->offset) {
        prev->size = (new_node->offset + new_node->size) - prev->offset;
        prev->next = new_node->next;
      }
    }

    b->alloc_count--;
    sa->total_bytes_used -= size;
    return;
  }
}

void mop_suballoc_reset_linear(MopSuballocator *sa,
                               uint32_t memory_type_index) {
  if (!sa)
    return;
  for (uint32_t i = 0; i < sa->block_count; i++) {
    MopSuballocBlock *b = &sa->blocks[i];
    if (b->memory_type_index == memory_type_index && !b->free_list) {
      b->linear_offset = 0;
      b->alloc_count = 0;
    }
  }
}

void mop_suballoc_get_stats(const MopSuballocator *sa, uint32_t *out_vk_allocs,
                            uint64_t *out_bytes_allocated,
                            uint64_t *out_bytes_used) {
  if (!sa)
    return;
  if (out_vk_allocs)
    *out_vk_allocs = sa->total_vk_allocs;
  if (out_bytes_allocated)
    *out_bytes_allocated = sa->total_bytes_allocated;
  if (out_bytes_used)
    *out_bytes_used = sa->total_bytes_used;
}

#endif /* MOP_HAS_VULKAN */
