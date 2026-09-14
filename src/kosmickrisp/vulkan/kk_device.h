/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * Copyright 2025 LunarG, Inc.
 * Copyright 2025 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_DEVICE_H
#define KK_DEVICE_H 1

#include "kk_private.h"

#include "kk_query_table.h"
#include "kk_queue.h"

#include "kosmickrisp/bridge/mtl_types.h"

#include "kosmickrisp/clc/kk_precompiled_shader.h"
#include "libkk_shaders.h"

#include "util/list.h"
#include "util/u_dynarray.h"

#include "vk_device.h"
#include "vk_meta.h"
#include "vk_queue.h"

struct kk_bo;
struct kk_physical_device;
struct vk_pipeline_cache;

struct kk_residency_set {
   simple_mtx_t mutex;
   mtl_residency_set *handle;
   /* Committed and immutable; invalidated on any membership change. */
   mtl_residency_set *snapshot;
};

struct mtl_sampler_packed {
   enum mtl_sampler_address_mode mode_u;
   enum mtl_sampler_address_mode mode_v;
   enum mtl_sampler_address_mode mode_w;
   enum mtl_sampler_border_color border_color;

   enum mtl_sampler_min_mag_filter min_filter;
   enum mtl_sampler_min_mag_filter mag_filter;
   enum mtl_sampler_mip_filter mip_filter;
   enum mtl_sampler_reduction_mode reduction_mode;

   enum mtl_compare_function compare_func;
   float min_lod;
   float max_lod;
   uint32_t max_anisotropy;
   bool normalized_coordinates;
};

struct kk_rc_sampler {
   struct mtl_sampler_packed key;

   mtl_sampler *handle;

   /* Reference count for this hardware sampler, protected by the heap mutex */
   uint16_t refcount;

   /* Index of this hardware sampler in the hardware sampler heap */
   uint16_t index;
};

struct kk_sampler_heap {
   simple_mtx_t lock;

   struct kk_query_table table;

   /* Map of mtl_sampler_packed to kk_rc_sampler */
   struct hash_table *ht;
};

/* Stand-in textures for VK_EXT_robustness2 nullDescriptor. Metal makes every
 * texture member function on a null texture undefined, so instead of branching
 * around each sample, kk_nir_lower_null_images redirects a read through a
 * descriptor whose resource id is 0 to the stand-in matching the shader's
 * texture type and zeroes the result. Contents are never observed, so all
 * stand-ins alias one region. GPU resource ids live in `bo` at offset 0 as
 * uint64_t[KK_NULL_TEX_COUNT], indexed by kk_null_tex_index(), bound at
 * MSL_NULL_TEXTURES_BUFFER. */
enum kk_null_tex_shape {
   KK_NULL_TEX_2D,
   KK_NULL_TEX_2D_ARRAY,
   KK_NULL_TEX_3D,
   KK_NULL_TEX_CUBE,
   KK_NULL_TEX_CUBE_ARRAY,
   KK_NULL_TEX_MS,
   KK_NULL_TEX_MS_ARRAY,
   KK_NULL_TEX_BUF,
   KK_NULL_TEX_SHAPES,
};

enum kk_null_tex_type {
   KK_NULL_TEX_FLOAT,
   KK_NULL_TEX_INT,
   KK_NULL_TEX_UINT,
   KK_NULL_TEX_DEPTH,
   KK_NULL_TEX_TYPES,
};

#define KK_NULL_TEX_COUNT (KK_NULL_TEX_SHAPES * KK_NULL_TEX_TYPES)

static inline unsigned
kk_null_tex_index(enum kk_null_tex_type type, enum kk_null_tex_shape shape)
{
   return type * KK_NULL_TEX_SHAPES + shape;
}

struct kk_null_textures {
   struct kk_bo *bo;
   mtl_texture *textures[KK_NULL_TEX_COUNT];
};

struct kk_precompiled_cache {
   struct kk_precompiled_shader shaders[LIBKK_NUM_PROGRAMS];
};

/* Metal 4 command allocators keep a per-allocator pool of IOGPU command
 * storage sized to the largest recording they ever served and never shrink it.
 * Owning one per VkCommandBuffer (s&box keeps thousands alive) pinned ~27GB.
 * Allocators are instead pooled per device and handed to a command buffer only
 * while it records; the commit feedback returns them once the GPU is done. */
struct kk_alloc_set {
   struct list_head link;
   struct kk_device *dev;
   /* pre_gfx / gfx / post_gfx. Concurrently open Metal command buffers need
    * their own allocator (sharing one crashes in AGX endComputePass). */
   mtl_command_allocator *allocators[3];
   /* Metal command buffers of the submission that last used the set. */
   uint32_t cmd_bufs_used;
   /* Immutable after recording; retained through GPU completion. */
   mtl_residency_set *recording_residency;
   /* Owned references, one per ended Metal command buffer. */
   struct util_dynarray residency_snapshots;
};

struct kk_device {
   struct vk_device vk;

   mtl_device *mtl_handle;
   mtl_compiler *mtl_compiler_handle;

   /* Dispatch table exposed to the user. Required since we need to record all
    * commands due to Metal limitations */
   struct vk_device_dispatch_table exposed_dispatch_table;

   struct kk_sampler_heap samplers;
   struct kk_null_textures null_textures;
   struct kk_query_table occlusion_queries;

   /* Track all heaps the user allocated so we can set them all as resident when
    * recording as required by Metal. */
   struct kk_residency_set residency_set;

   struct {
      simple_mtx_t mutex;
      struct list_head free;
      unsigned free_count;
   } alloc_sets;

   struct kk_precompiled_cache precompiled_cache;

   bool has_queue;
   struct kk_queue queue;

   struct vk_meta_device meta;

   /* Geomtry heap */
   struct kk_bo *heap;
   util_once_flag heap_init_once;
};

VK_DEFINE_HANDLE_CASTS(kk_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)

static inline struct kk_physical_device *
kk_device_physical(const struct kk_device *dev)
{
   return (struct kk_physical_device *)dev->vk.physical;
}

VkResult kk_device_init_meta(struct kk_device *dev);
void kk_device_finish_meta(struct kk_device *dev);
VkResult kk_device_init_lib(struct kk_device *dev);
void kk_device_finish_lib(struct kk_device *dev);
mtl_residency_set *kk_device_acquire_residency_snapshot(struct kk_device *dev);
struct kk_alloc_set *kk_device_acquire_alloc_set(struct kk_device *dev);
/* GPU must be done with the set. Resets it for reuse or frees it if the last
 * recording was large enough to have bloated its pools. */
void kk_device_recycle_alloc_set(struct kk_alloc_set *set);

void kk_device_add_heap_to_residency_set(struct kk_device *dev, mtl_heap *heap);
void kk_device_remove_heap_from_residency_set(struct kk_device *dev,
                                              mtl_heap *heap);
void kk_device_add_texture_to_residency_set(struct kk_device *dev,
                                            mtl_texture *texture);
void kk_device_remove_texture_from_residency_set(struct kk_device *dev,
                                                 mtl_texture *texture);
void kk_device_add_buffer_to_residency_set(struct kk_device *dev,
                                           mtl_buffer *buffer);
void kk_device_remove_buffer_from_residency_set(struct kk_device *dev,
                                                mtl_buffer *buffer);
void kk_device_make_resources_resident(struct kk_device *dev);

/* Required to create a sampler */
mtl_sampler *kk_sampler_create(struct kk_device *dev,
                               const struct mtl_sampler_packed *packed);
VkResult kk_sampler_heap_add(struct kk_device *dev,
                             struct mtl_sampler_packed desc,
                             struct kk_rc_sampler **out);
void kk_sampler_heap_remove(struct kk_device *dev, struct kk_rc_sampler *rc);

#endif // KK_DEVICE_H
