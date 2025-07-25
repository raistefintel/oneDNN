/*******************************************************************************
* Copyright 2019-2025 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "gpu/intel/ocl/dispatch.h"
#include "gpu/intel/ocl/ocl_eltwise.h"
#include "gpu/intel/ocl/ocl_io.h"
#include "gpu/intel/ocl/ocl_post_ops.h"
#include "gpu/intel/ocl/types_interop.h"

#define DATA_OFF(x0, x1, x2, x3, x4, x5) OFF_MD(DST, x0, x1, x2, x3, x4, x5)

#define DIFF_DATA_OFF(x0, x1, x2, x3, x4, x5) \
    OFF_MD(DIFF, x0, x1, x2, x3, x4, x5)

#define GWS_GET_THREAD_ID(index) (get_global_id(index) + offset.array[index])

#if IS_FWD
__kernel void ref_eltwise_fwd(__global SRC_DATA_T *src,
        __global DST_DATA_T *dst, float alpha, float beta,
        int64x3_t offset POST_OP_ARGS) {
#if USE_GWS_GET
    dim_t d0 = GWS_GET_D0();
    dim_t d1 = GWS_GET_D1();
    dim_t d2 = GWS_GET_D2();
    dim_t d3 = GWS_GET_D3();
    dim_t d4 = GWS_GET_D4();
    dim_t d5 = GWS_GET_D5();

    const dim_t data_off = DATA_OFF(d0, d1, d2, d3, d4, d5);

    if (d0 >= DST_D0 || d1 >= DST_D1 || d2 >= DST_D2 || d3 >= DST_D3
            || d4 >= DST_D4 || d5 >= DST_D5) {
        write(dst + data_off, 0.f);
        return;
    }
#else
    const dim_t data_off = get_global_id(0)
#if GWS1 > 1
            + get_global_id(1) * GWS0
#endif
#if GWS2 > 1
            + get_global_id(2) * GWS0 * GWS1
#endif
            ;

    const dim_t d0 = 0;
    const dim_t d1 = 0;
    const dim_t d2 = 0;
    const dim_t d3 = 0;
    const dim_t d4 = 0;
    const dim_t d5 = 0;
#endif

#if DT_F16 == 1
    float tmp_s = load(tmp_s, src + data_off);
#else
    float tmp_s = load(tmp_s, src + data_off);
#endif
    tmp_s = fwd_eltwise(tmp_s, alpha, beta, 1.0f);

    float dst_data;
#if WITH_SUM
    load(dst_data, dst + data_off);
#endif

    APPLY_POST_OPS_SERIAL(tmp_s, dst_data, d0, d1, d2, d3, d4, d5);
    write(dst + data_off, tmp_s);
}

#else // #if IS_FWD

#if DT_F64 == 1 || DT_F32 == 1 || DT_BF16 == 1 || DT_F16 == 1

__kernel void ref_eltwise_bwd(__global SRC_DATA_T *src,
        __global DIFF_DATA_T *diff_src, __global DIFF_DATA_T *diff_dst,
        float alpha, float beta, int64x3_t offset) {

    dim_t d0 = GWS_GET_D0();
    dim_t d1 = GWS_GET_D1();
    dim_t d2 = GWS_GET_D2();
    dim_t d3 = GWS_GET_D3();
    dim_t d4 = GWS_GET_D4();
    dim_t d5 = GWS_GET_D5();

    const dim_t data_off = DATA_OFF(d0, d1, d2, d3, d4, d5);
    const dim_t diff_data_off = DIFF_DATA_OFF(d0, d1, d2, d3, d4, d5);

    if (d0 >= DST_D0 || d1 >= DST_D1 || d2 >= DST_D2 || d3 >= DST_D3
            || d4 >= DST_D4 || d5 >= DST_D5) {
        write(diff_src + diff_data_off, 0.f);
        return;
    }

    POST_OP_DATA_T tmp_dd = load(tmp_dd, diff_dst + diff_data_off);
    POST_OP_DATA_T tmp_s = load(tmp_s, src + data_off);

    write(diff_src + diff_data_off, bwd_eltwise(tmp_dd, tmp_s, alpha, beta));
}
#endif

#endif
