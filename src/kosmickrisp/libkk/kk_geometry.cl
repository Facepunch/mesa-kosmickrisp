/*
 * Copyright 2026 LunarG, Inc.
 * Copyright 2026 Google LLC
 * Copyright 2023 Alyssa Rosenzweig
 * Copyright 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "compiler/libcl/libcl_vk.h"
#include "poly/geometry.h"
#include "poly/tessellator.h"

KERNEL(1)
libkk_gs_setup_indirect(
   uint64_t index_buffer, constant uint *draw,
   global struct poly_vertex_params *vp /* output */,
   global struct poly_geometry_params *p /* output */,
   global struct poly_heap *heap,
   uint64_t vs_outputs /* Vertex (TES) output mask */,
   uint32_t index_size_B /* 0 if no index buffer */,
   uint32_t index_buffer_range_el,
   uint32_t prim /* Input primitive type, enum mesa_prim */,
   int is_prefix_summing, uint max_indices, enum poly_gs_shape shape)
{
   uint vertex_count = draw[0];
   uint instance_count = draw[1];

   poly_vertex_params_set_draw(vp, vertex_count, instance_count);
   poly_geometry_params_set_draw(p, prim, shape, max_indices, vertex_count,
                                 instance_count);

   /* TODO_KOSMICKRISP Use poly_index_buffer and implement
    * load_ro_sink_address_poly */
   if (index_size_B) {
      vp->index_buffer = index_buffer + (draw[2] * index_size_B);
      vp->index_buffer_range_el =
         poly_index_buffer_range_el(index_buffer_range_el, draw[2]);
   }

   uint vertex_buffer_size =
      poly_tcs_in_size(vertex_count * instance_count, vs_outputs);

   if (is_prefix_summing) {
      p->count_buffer = poly_heap_alloc(
         heap, p->input_primitives * p->count_buffer_stride);
   }

   vp->output_buffer = (uintptr_t)poly_heap_alloc(heap, vertex_buffer_size);
   vp->outputs = vs_outputs;

   if (shape == POLY_GS_SHAPE_DYNAMIC_INDEXED) {
      const uint32_t index_offset =
         poly_heap_alloc_offs(heap, p->draw.index_count * 4);
      p->draw.first_index = index_offset / 4;
      p->output_index_buffer = (global uint *)(heap->base + index_offset);
   }
}

KERNEL(1024)
libkk_prefix_sum_geom(constant struct poly_geometry_params *p)
{
   local uint scratch[32];
   poly_prefix_sum(scratch, p->count_buffer, p->input_primitives,
                   p->count_buffer_stride / 4, cl_group_id.x, 1024);
}

KERNEL(1024)
libkk_prefix_sum_tess(global struct poly_tess_params *p)
{
   local uint scratch[32];
   poly_prefix_sum(scratch, p->counts, p->nr_patches, 1 /* words */,
                   0 /* word */, 1024);

   /* After prefix summing, we know the total # of indices, so allocate the
    * index buffer now. Elect a thread for the allocation.
    */
   barrier(CLK_LOCAL_MEM_FENCE);
   if (cl_local_id.x != 0)
      return;

   /* The last element of an inclusive prefix sum is the total sum */
   uint total = p->nr_patches > 0 ? p->counts[p->nr_patches - 1] : 0;

   /* Allocate 4-byte indices */
   uint32_t elsize_B = sizeof(uint32_t);
   uint32_t size_B = total * elsize_B;
   uint alloc_B = poly_heap_alloc_offs(p->heap, size_B);
   p->index_buffer = (global uint32_t *)(((uintptr_t)p->heap->base) + alloc_B);

   /* ...and now we can generate the API indexed draw */
   global uint32_t *desc = p->out_draws;

   desc[0] = total;              /* count */
   desc[1] = 1;                  /* instance_count */
   desc[2] = alloc_B / elsize_B; /* start */
   desc[3] = 0;                  /* index_bias */
   desc[4] = 0;                  /* start_instance */
}
