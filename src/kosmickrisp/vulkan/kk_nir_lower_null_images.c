/*
 * Copyright © 2025 Imagination Technologies Ltd.
 * Copyright (C) 2020-2021 Collabora, Ltd.
 * Copyright © 2020 Valve Corporation
 * Copyright 2026 LunarG, Inc.
 * Copyright 2026 Google LLC
 * SPDX-License-Identifier: MIT
 */
#include "kk_device.h"
#include "kk_shader.h"

#include "kosmickrisp/compiler/nir_to_msl.h"

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"

/* VK_EXT_robustness2 nullDescriptor: reads through a null image descriptor
 * return zero, writes are discarded. Metal leaves every texture member
 * function on a null texture undefined, and a branch around each sample keeps
 * the Metal compiler from hoisting and batching texture fetches (Fishport's
 * material shader has 69 of them). So reads are made branch-free: the handle
 * load takes the address of the device's stand-in texture of the same MSL
 * type when the descriptor's resource id is 0, and the result is selected to
 * zero. Stores and atomics keep the branch: the stand-ins are shared and read
 * only.
 */

static unsigned
null_tex_index(const nir_intrinsic_instr *handle)
{
   /* kk_nir_lower_textures has already run: 1D is 2D, storage cubes are 2D
    * arrays. */
   bool array = nir_intrinsic_image_array(handle);
   enum kk_null_tex_shape shape;
   switch (nir_intrinsic_image_dim(handle)) {
   case GLSL_SAMPLER_DIM_3D:
      shape = KK_NULL_TEX_3D;
      break;
   case GLSL_SAMPLER_DIM_CUBE:
      shape = array ? KK_NULL_TEX_CUBE_ARRAY : KK_NULL_TEX_CUBE;
      break;
   case GLSL_SAMPLER_DIM_MS:
      shape = array ? KK_NULL_TEX_MS_ARRAY : KK_NULL_TEX_MS;
      break;
   case GLSL_SAMPLER_DIM_BUF:
      shape = KK_NULL_TEX_BUF;
      break;
   default:
      assert(nir_intrinsic_image_dim(handle) == GLSL_SAMPLER_DIM_2D);
      shape = array ? KK_NULL_TEX_2D_ARRAY : KK_NULL_TEX_2D;
      break;
   }

   enum kk_null_tex_type type = KK_NULL_TEX_DEPTH;
   if (handle->intrinsic == nir_intrinsic_load_texture_handle_kk) {
      switch (nir_alu_type_get_base_type(nir_intrinsic_dest_type(handle))) {
      case nir_type_int:
         type = KK_NULL_TEX_INT;
         break;
      case nir_type_uint:
         type = KK_NULL_TEX_UINT;
         break;
      default:
         type = KK_NULL_TEX_FLOAT;
         break;
      }
   }

   return kk_null_tex_index(type, shape);
}

/* handle = load_texture_handle_kk(bcsel(is_null, stand_in_addr, desc_addr)) */
static bool
lower_handle_load(nir_builder *b, nir_intrinsic_instr *intr, UNUSED void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_texture_handle_kk &&
       intr->intrinsic != nir_intrinsic_load_depth_texture_kk)
      return false;

   b->cursor = nir_before_instr(&intr->instr);
   nir_def *addr = intr->src[0].ssa;
   nir_def *is_null =
      nir_ieq_imm(b, nir_load_global_constant(b, 1, 64, addr), 0);
   nir_def *table =
      nir_load_buffer_ptr_kk(b, 1, 64, .binding = MSL_NULL_TEXTURES_BUFFER);
   nir_def *stand_in =
      nir_iadd_imm(b, table, null_tex_index(intr) * sizeof(uint64_t));
   nir_src_rewrite(&intr->src[0], nir_bcsel(b, is_null, stand_in, addr));
   return true;
}

static nir_def *
is_null_of(nir_def *handle)
{
   nir_alu_instr *sel =
      nir_def_as_alu(nir_def_as_intrinsic(handle)->src[0].ssa);
   assert(sel->op == nir_op_bcsel);
   return sel->src[0].src.ssa;
}

static bool
lower_use(nir_builder *b, nir_instr *instr, UNUSED void *data)
{
   nir_def *def = NULL;
   nir_def *handle = NULL;
   bool is_write = false;

   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      switch (intr->intrinsic) {
      case nir_intrinsic_bindless_image_store:
      case nir_intrinsic_bindless_image_atomic:
      case nir_intrinsic_bindless_image_atomic_swap:
         is_write = true;
         FALLTHROUGH;
      case nir_intrinsic_bindless_image_load:
      case nir_intrinsic_bindless_image_sparse_load:
      case nir_intrinsic_bindless_image_size:
      case nir_intrinsic_bindless_image_samples:
      case nir_intrinsic_bindless_image_levels:
         if (nir_intrinsic_infos[intr->intrinsic].has_dest)
            def = &intr->def;
         handle = intr->src[0].ssa;
         break;
      default:
         return false;
      }
   } else if (instr->type == nir_instr_type_tex) {
      nir_tex_instr *tex = nir_instr_as_tex(instr);
      def = &tex->def;
      handle = nir_get_tex_src(tex, nir_tex_src_texture_handle);
      if (!handle)
         return false;
   } else {
      return false;
   }

   nir_def *is_null = is_null_of(handle);

   if (!is_write) {
      b->cursor = nir_after_instr(instr);
      nir_def *zero = nir_imm_zero(b, def->num_components, def->bit_size);
      nir_def_rewrite_uses_after(def, nir_bcsel(b, is_null, zero, def));
      return true;
   }

   b->cursor = nir_before_instr(instr);
   nir_def *zero = NULL;
   nir_if *nif = nir_push_if(b, nir_inot(b, is_null));
   nir_instr_remove(instr);
   nir_builder_instr_insert(b, instr);
   if (def) {
      nir_push_else(b, nif);
      zero = nir_imm_zero(b, def->num_components, def->bit_size);
   }
   nir_pop_if(b, nif);

   if (def) {
      nir_def *phi = nir_if_phi(b, def, zero);

      /* We can't use nir_def_rewrite_uses_after on phis, so use the global
       * version and fixup the phi manually
       */
      nir_def_rewrite_uses(def, phi);

      nir_phi_instr *phi_as_phi = nir_def_as_phi(phi);
      nir_phi_src *phi_src =
         nir_phi_get_src_from_block(phi_as_phi, instr->block);
      nir_src_rewrite(&phi_src->src, def);
   }

   return true;
}

bool
kk_nir_lower_null_images(nir_shader *shader)
{
   bool progress = nir_shader_intrinsics_pass(shader, lower_handle_load,
                                              nir_metadata_control_flow, NULL);
   if (progress)
      progress |= nir_shader_instructions_pass(shader, lower_use,
                                               nir_metadata_none, NULL);
   return progress;
}
