// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/builtins/ukernel/pack.h"

static iree_ukernel_status_t iree_ukernel_pack_validate(
    const iree_ukernel_pack_params_t* params) {
  const uint32_t allflags = IREE_UKERNEL_FLAG_PACK_TRANSPOSE_INNER |
                            IREE_UKERNEL_FLAG_PACK_TRANSPOSE_OUTER;
  if (params->flags & ~allflags) {
    return iree_ukernel_status_bad_flags;
  }
  switch (params->type) {
    case iree_ukernel_pack_type_f32f32:
    case iree_ukernel_pack_type_i8i8:
    case iree_ukernel_pack_type_i32i32:
      break;
    default:
      return iree_ukernel_status_bad_type;
  }
  if (params->in_stride0 < 0 || params->out_stride0 < 0 ||
      params->in_size0 < 0 || params->in_size1 < 0 || params->out_size0 < 0 ||
      params->out_size1 < 0 || params->out_size2 < 0 || params->out_size3 < 0) {
    return iree_ukernel_status_unsupported_huge_or_negative_dimension;
  }
  return iree_ukernel_status_ok;
}

static inline void iree_ukernel_ssize_swap(iree_ukernel_ssize_t* a,
                                           iree_ukernel_ssize_t* b) {
  iree_ukernel_ssize_t t = *a;
  *a = *b;
  *b = t;
}

static inline void iree_ukernel_memcpy(char* dst, const char* src,
                                       iree_ukernel_ssize_t size) {
  for (iree_ukernel_ssize_t i = 0; i < size; ++i) dst[i] = src[i];
}

iree_ukernel_status_t iree_ukernel_pack(
    const iree_ukernel_pack_params_t* params) {
  IREE_UKERNEL_RETURN_IF_ERROR(iree_ukernel_pack_validate(params));
  if (params->out_size0 == 0 || params->out_size1 == 0 ||
      params->out_size2 == 0 || params->out_size3 == 0) {
    return iree_ukernel_status_ok;
  }
  iree_ukernel_ssize_t elem_size = iree_ukernel_pack_elem_size(params->type);
  iree_ukernel_ssize_t lsize0 = params->out_size0;
  iree_ukernel_ssize_t lsize1 = params->out_size1;
  iree_ukernel_ssize_t lsize2 = params->out_size2;
  iree_ukernel_ssize_t lsize3 = params->out_size3;
  iree_ukernel_ssize_t out_stride_l0 = params->out_stride0;
  iree_ukernel_ssize_t out_stride_l1 = params->out_size3 * params->out_size2;
  iree_ukernel_ssize_t out_stride_l2 = params->out_size3;
  iree_ukernel_ssize_t out_stride_l3 = 1;
  if (params->flags & IREE_UKERNEL_FLAG_PACK_TRANSPOSE_OUTER) {
    iree_ukernel_ssize_swap(&lsize0, &lsize1);
    iree_ukernel_ssize_swap(&out_stride_l0, &out_stride_l1);
  }
  if (params->flags & IREE_UKERNEL_FLAG_PACK_TRANSPOSE_INNER) {
    iree_ukernel_ssize_swap(&lsize2, &lsize3);
    iree_ukernel_ssize_swap(&out_stride_l2, &out_stride_l3);
  }
  for (iree_ukernel_ssize_t l0 = 0; l0 < lsize0; ++l0) {
    for (iree_ukernel_ssize_t l2 = 0; l2 < lsize2; ++l2) {
      for (iree_ukernel_ssize_t l1 = 0; l1 < lsize1; ++l1) {
        for (iree_ukernel_ssize_t l3 = 0; l3 < lsize3; ++l3) {
          iree_ukernel_ssize_t out_offset =
              l0 * out_stride_l0 + l2 * out_stride_l2 + l1 * out_stride_l1 +
              l3 * out_stride_l3;
          iree_ukernel_ssize_t i0 = l0 * lsize2 + l2;
          iree_ukernel_ssize_t i1 = l1 * lsize3 + l3;
          char* out_ptr = ((char*)params->out_buffer) + out_offset * elem_size;
          if (i0 >= params->in_size0 || i1 >= params->in_size1) {
            iree_ukernel_memcpy(out_ptr, params->padding_value, elem_size);
          } else {
            iree_ukernel_ssize_t in_offset = i1 + i0 * params->in_stride0;
            const char* in_ptr =
                ((char*)params->in_buffer) + in_offset * elem_size;
            iree_ukernel_memcpy(out_ptr, in_ptr, elem_size);
          }
        }
      }
    }
  }
  return iree_ukernel_status_ok;
}