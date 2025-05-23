/*******************************************************************************
* Copyright 2017-2025 Intel Corporation
* Copyright 2018 YANDEX LLC
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

#include <bitset>

#include "common/dnnl_thread.hpp"

#include "cpu/cpu_pooling_pd.hpp"
#include "cpu/x64/jit_avx512_core_bf16cvt.hpp"
#include "cpu/x64/jit_avx512_core_fp8cvt.hpp"
#include "cpu/x64/jit_uni_pool_kernel.hpp"

namespace dnnl {
namespace impl {
namespace cpu {
namespace x64 {

using namespace Xbyak;
using namespace alg_kind;

#define GET_OFF(field) offsetof(jit_uni_pooling_args_t, field)

constexpr int sse41_single_block_size
        = cpu_isa_traits_t<sse41>::vlen / sizeof(float);

static bcast_set_t get_supported_bcast_strategies() {
    return {broadcasting_strategy_t::scalar, broadcasting_strategy_t::per_oc,
            broadcasting_strategy_t::no_broadcast};
}

template <cpu_isa_t isa>
jit_uni_pool_kernel_t<isa>::~jit_uni_pool_kernel_t() = default;

template <cpu_isa_t isa>
jit_uni_pool_kernel_t<isa>::jit_uni_pool_kernel_t(
        const jit_pool_conf_t &ajpp, const memory_desc_t *dst_md)
    : jit_generator_t(jit_name(), isa), jpp(ajpp) {

    bool has_f8_e5m2_binary_postops = false;
    bool has_f8_e4m3_binary_postops = false;
    if (jpp.with_binary) {
        const auto &post_ops = jpp.post_ops;
        for (int i = 0; i < post_ops.len(); i++) {
            const auto &entry = post_ops.entry_[i];
            if (!entry.is_binary()) continue;
            has_f8_e5m2_binary_postops
                    = entry.binary.src1_desc.data_type == data_type::f8_e5m2
                    || has_f8_e5m2_binary_postops;
            has_f8_e4m3_binary_postops
                    = entry.binary.src1_desc.data_type == data_type::f8_e4m3
                    || has_f8_e4m3_binary_postops;
        }
    }

    if (use_fp8_emulation() || has_f8_e5m2_binary_postops
            || has_f8_e4m3_binary_postops) {
        if (utils::one_of(data_type::f8_e5m2, ajpp.src_dt, ajpp.dst_dt)
                || has_f8_e5m2_binary_postops)
            f8_e5m2_cvt_ = utils::make_unique<fp8_conversion_e5m2_t>(this,
                    fp8_emu_reserv_1, fp8_emu_reserv_2, fp8_emu_reserv_3,
                    fp8_tmp_mask, fp8_emu_reg64);
        if (utils::one_of(data_type::f8_e4m3, ajpp.src_dt, ajpp.dst_dt)
                || has_f8_e4m3_binary_postops)
            f8_e4m3_cvt_ = utils::make_unique<fp8_conversion_e4m3_t>(this,
                    fp8_emu_reserv_1, fp8_emu_reserv_2, fp8_emu_reserv_3,
                    fp8_emu_reserv_4, fp8_emu_reserv_5, fp8_emu_reg64);
    }

    const auto tail_size
            = isa == sse41 ? jpp.c_tail % sse41_single_block_size : jpp.c_tail;

    if (jpp.with_postops) {
        static constexpr bool preserve_gpr = true;
        static constexpr bool preserve_vmm = true;
        static constexpr bool use_exact_tail_scalar_bcast = false;

        const binary_injector::rhs_arg_static_params_t rhs_sp {
                static_cast<std::size_t>(this->xmm4.getIdx()), this->r14,
                this->r15, this->r13, preserve_gpr, preserve_vmm,
                GET_OFF(post_ops_binary_rhs_arg_vec), GET_OFF(dst_orig),
                memory_desc_wrapper(jpp.tag_kind == jit_memory_tag_kind_t::ncsp
                                ? jpp.tmp_md
                                : *dst_md),
                static_cast<size_t>(tail_size), k_c_tail_mask,
                use_exact_tail_scalar_bcast};

        const binary_injector::static_params_t bsp {reg_param,
                get_supported_bcast_strategies(), rhs_sp, f8_e5m2_cvt_.get(),
                f8_e4m3_cvt_.get()};

        postops_injector_
                = utils::make_unique<injector::jit_uni_postops_injector_t<isa>>(
                        this, jpp.post_ops, bsp);
    }

    io::io_tail_conf_t io_tail_conf(jpp.c_block, tail_size,
            k_c_tail_mask.getIdx(), vmm_c_tail_mask.getIdx(), tmp_gpr);

    utils::optional_t<io::io_emu_bf16_conf_t> io_bf16_conf;
    if (use_bf16_emulation())
        io_bf16_conf = io::io_emu_bf16_conf_t(bf16_emu_reserv_1,
                bf16_emu_reserv_2, bf16_emu_reserv_3, bf16_emu_reserv_4,
                bf16_emu_reserv_5);

    utils::optional_t<io::io_emu_fp8_conf_t> io_fp8_conf;
    if (use_fp8_emulation() || has_f8_e5m2_binary_postops
            || has_f8_e4m3_binary_postops)
        io_fp8_conf = io::io_emu_fp8_conf_t(fp8_emu_reserv_1, fp8_emu_reserv_2,
                fp8_emu_reserv_3, fp8_emu_reserv_4, fp8_emu_reserv_5,
                fp8_tmp_mask, fp8_emu_reg64);

    using io_mdt_helper = io::jit_io_multi_dt_helper_t<Vmm>;

    typename io_mdt_helper::data_types_t dtypes = {jpp.src_dt, jpp.dst_dt};
    // Indices of type s32 will be stored/loaded as f32 as jit_io_helper_t does not
    // support integers but stores/loads f32 without additional conversions of those
    // 4 bytes. jit_io_helper_t is not used for processing indices of type u8.
    if (jpp.ind_dt == data_type::s32) dtypes.insert(data_type::f32);
    if (jpp.needs_f32_accum_for_bf16) dtypes.insert(data_type::f32);

    io_ = io_mdt_helper(this, jpp.isa, dtypes, {}, io_tail_conf, io_bf16_conf,
            {}, utils::nullopt, io_fp8_conf);
}

static status_t set_binary_postops_formats(
        post_ops_t &post_ops, const memory_desc_t *dst_md) {
    for (int idx = 0; idx < post_ops.len(); ++idx) {
        if (!post_ops.contain(primitive_kind::binary, idx)) continue;

        auto &src1_md = post_ops.entry_[idx].binary.src1_desc;
        const memory_desc_wrapper src1_mdw(src1_md);
        if (!src1_mdw.format_any()) {
            if (src1_mdw.is_blocking_desc())
                continue;
            else
                return status::unimplemented;
        }

        const memory_desc_wrapper dst_mdw(dst_md);
        assert(!dst_mdw.format_any());

        CHECK(memory_desc_init_by_blocking_desc(
                src1_md, dst_mdw.blocking_desc()));
    }

    return status::success;
}

template <cpu_isa_t isa>
bool jit_uni_pool_kernel_t<isa>::has_large_buffers(const pooling_pd_t *ppd) {
    auto is_large = [](const memory_desc_t &md) {
        memory_desc_wrapper mdw(md);
        return mdw.size()
                > static_cast<size_t>(std::numeric_limits<int32_t>::max());
    };

    if (is_large(*ppd->invariant_src_md())) return true;
    auto &post_ops = ppd->attr()->post_ops_;
    for (int i = 0; i < post_ops.len(); i++) {
        auto &e = post_ops.entry_[i];
        if (e.is_binary()) {
            if (is_large(e.binary.src1_desc)) return true;
        }
    }
    return false;
}

template <cpu_isa_t isa>
status_t jit_uni_pool_kernel_t<isa>::init_conf(
        jit_pool_conf_t &jpp, primitive_attr_t &attr, const pooling_pd_t *ppd) {

    const auto &pd = *ppd->desc();
    const memory_desc_wrapper src_d(
            ppd->is_fwd() ? ppd->src_md() : ppd->diff_src_md());
    const memory_desc_wrapper dst_d(
            ppd->is_fwd() ? ppd->dst_md() : ppd->diff_dst_md());

    VDISPATCH_POOLING_IC(!has_large_buffers(ppd), VERBOSE_SHAPE_RESTRICTION);

    const int ndims = src_d.ndims();

    jpp.nthr = dnnl_get_max_threads();
    jpp.is_training = pd.prop_kind == prop_kind::forward_training;
    jpp.is_backward = pd.prop_kind == prop_kind::backward_data;

    jpp.id = (ndims == 5) ? src_d.dims()[2] : 1;
    jpp.ih = (ndims == 3) ? 1 : src_d.dims()[ndims - 2];
    jpp.iw = src_d.dims()[ndims - 1];
    jpp.od = (ndims == 5) ? dst_d.dims()[2] : 1;
    jpp.ow = dst_d.dims()[ndims - 1];
    jpp.oh = (ndims == 3) ? 1 : dst_d.dims()[ndims - 2];

    const bool is_avx512 = is_superset(isa, avx512_core);
    jpp.ndims = ndims;
    jpp.mb = src_d.dims()[0];
    jpp.c_without_padding = src_d.dims()[1];
    jpp.c_block = is_avx512 ? 16 : 8;

    jpp.alg = pd.alg_kind;

    jpp.src_dt = jpp.is_backward ? pd.diff_src_desc.data_type
                                 : pd.src_desc.data_type;
    jpp.dst_dt = jpp.is_backward ? pd.diff_dst_desc.data_type
                                 : pd.dst_desc.data_type;

    jpp.tmp_md = memory_desc_t();

    jpp.is_bf16 = (src_d.data_type() == data_type::bf16
            && dst_d.data_type() == data_type::bf16);
    jpp.is_f16 = (src_d.data_type() == data_type::f16
            && dst_d.data_type() == data_type::f16);
    jpp.is_fp8 = utils::one_of(src_d.data_type(), data_type::f8_e5m2,
                         data_type::f8_e4m3)
            && utils::one_of(
                    dst_d.data_type(), data_type::f8_e5m2, data_type::f8_e4m3);

    using namespace format_tag;

    const auto blocked_fmt_tag = is_avx512
            ? utils::pick(ndims - 3, nCw16c, nChw16c, nCdhw16c)
            : utils::pick(ndims - 3, nCw8c, nChw8c, nCdhw8c);

    // src_d.data_type() is equal to dst_d.data_type(). This is checked in init
    auto ncsp_fmt_tag = format_tag::undef;

    const unsigned int L3_cache_size_per_core
            = platform::get_per_core_cache_size(3);
    const size_t block_size
            = ((size_t)jpp.id * jpp.ih * jpp.iw + jpp.od * jpp.oh * jpp.ow)
            * jpp.c_block * types::data_type_size(src_d.data_type());

    const bool forward_ncsp_allowed = !jpp.is_backward
            && jpp.c_without_padding > 3
            && ((jpp.ih > 1 && jpp.iw > 1
                        && block_size <= L3_cache_size_per_core)
                    || utils::one_of(src_d.data_type(), data_type::bf16,
                            data_type::f16, data_type::f8_e5m2,
                            data_type::f8_e4m3));

    const bool backward_ncsp_allowed = jpp.is_backward
            && ((jpp.ih > 1 && jpp.iw > 1 && jpp.c_without_padding > 1
                        && block_size <= L3_cache_size_per_core)
                    || (utils::one_of(src_d.data_type(), data_type::bf16,
                                data_type::f16)
                            && !(jpp.alg == pooling_max
                                    && block_size > L3_cache_size_per_core)));

    ncsp_fmt_tag = ((forward_ncsp_allowed || backward_ncsp_allowed) && is_avx512
                           && ndims <= 5)
            ? utils::pick(ndims - 3, ncw, nchw, ncdhw)
            : format_tag::undef;

    const auto nspc_fmt_tag = (ndims <= 5)
            ? utils::pick(ndims - 3, nwc, nhwc, ndhwc)
            : format_tag::undef;

    const auto fmt_tag = src_d.matches_one_of_tag(
            blocked_fmt_tag, ncsp_fmt_tag, nspc_fmt_tag);

    VDISPATCH_POOLING_IC(
            dst_d.matches_tag(fmt_tag), VERBOSE_UNSUPPORTED_TAG_S, "dst");

    const bool post_ops_with_binary
            = attr.post_ops_.find(primitive_kind::binary) != -1;

    if (fmt_tag == ncsp_fmt_tag) {
        // transform input to blocked f32, call f32 jit, transform result to
        // plain output
        jpp.is_bf16 = false;
        jpp.is_f16 = false;
        jpp.is_fp8 = false;
        jpp.src_dt = jpp.dst_dt = data_type::f32;
        jpp.dt_size = types::data_type_size(jpp.src_dt);
        jpp.tag_kind = jit_memory_tag_kind_t::ncsp;

        // used to initialize binary post-ops
        if (ppd->is_fwd() && post_ops_with_binary) {
            CHECK(memory_desc_init_by_tag(jpp.tmp_md, ndims, dst_d.md_->dims,
                    data_type::f32, blocked_fmt_tag));
        }
    } else {
        jpp.dt_size = types::data_type_size(src_d.data_type());
        jpp.tag_kind = (fmt_tag == nspc_fmt_tag)
                ? jit_memory_tag_kind_t::nspc
                : jit_memory_tag_kind_t::blocked;
    }

    if (ppd->is_fwd() && post_ops_with_binary) {
        CHECK(set_binary_postops_formats(attr.post_ops_,
                jpp.tag_kind == jit_memory_tag_kind_t::ncsp ? &jpp.tmp_md
                                                            : dst_d.md_));
    }

    VDISPATCH_POOLING_IC(
            init_post_ops_conf(jpp, attr, dst_d), VERBOSE_UNSUPPORTED_POSTOP);

    jpp.isa = (jpp.is_bf16 && mayiuse(avx512_core_bf16))
            ? avx512_core_bf16
            : ((jpp.is_fp8 && mayiuse(avx512_core_fp16)) ? avx512_core_fp16
                                                         : isa);

    // disabling verbose dispatch messages for unsupported isa for
    // better readability
    if (!mayiuse(isa)) return status::unimplemented;

    VDISPATCH_POOLING_IC(
            (fmt_tag != format_tag::undef), VERBOSE_UNSUPPORTED_TAG);
    VDISPATCH_POOLING_IC(IMPLICATION(jpp.is_bf16,
                                 utils::one_of(jpp.isa, avx512_core_bf16,
                                         avx512_core, avx2_vnni_2)),
            VERBOSE_ISA_DT_MISMATCH);
    VDISPATCH_POOLING_IC(
            IMPLICATION(jpp.is_f16,
                    utils::one_of(jpp.isa, avx512_core_fp16, avx2_vnni_2)),
            VERBOSE_ISA_DT_MISMATCH);
    VDISPATCH_POOLING_IC(
            IMPLICATION(jpp.is_fp8, utils::one_of(jpp.isa, avx512_core_fp16)),
            VERBOSE_ISA_DT_MISMATCH);
    VDISPATCH_POOLING_IC(
            utils::one_of(pd.alg_kind, pooling_max, pooling_avg_include_padding,
                    pooling_avg_exclude_padding),
            VERBOSE_BAD_ALGORITHM);

    const bool is_xf16_avx2_vnni_2
            = (jpp.is_bf16 || jpp.is_f16) && isa == avx2_vnni_2;
    // note: avx2_vnni_2 only supports nxc format
    VDISPATCH_POOLING_IC(IMPLICATION(is_xf16_avx2_vnni_2,
                                 jpp.tag_kind == jit_memory_tag_kind_t::nspc),
            "isa, format tag mismatch");

    // note: avx2_vnni_2 only supports FWD direction
    VDISPATCH_POOLING_IC(IMPLICATION(is_xf16_avx2_vnni_2, !jpp.is_backward),
            "isa, propagation kind mismatch");

    jpp.c = jpp.tag_kind == jit_memory_tag_kind_t::blocked
            ? utils::rnd_up(jpp.c_without_padding, jpp.c_block)
            : jpp.c_without_padding;
    if (jpp.tag_kind == jit_memory_tag_kind_t::blocked)
        assert(src_d.padded_dims()[1] == jpp.c);
    jpp.nb_c = utils::div_up(jpp.c, jpp.c_block);
    jpp.c_tail = jpp.c_without_padding % jpp.c_block;
    jpp.is_c_padded = jpp.tag_kind == jit_memory_tag_kind_t::blocked
            && src_d.padded_dims()[1] != jpp.c_without_padding;

    jpp.stride_d = (ndims == 5) ? pd.strides[0] : 1;
    jpp.stride_h = (ndims == 3) ? 1 : pd.strides[ndims - 4];
    jpp.stride_w = pd.strides[ndims - 3];
    jpp.kd = (ndims == 5) ? pd.kernel[0] : 1;
    jpp.kh = (ndims == 3) ? 1 : pd.kernel[ndims - 4];
    jpp.kw = pd.kernel[ndims - 3];

    jpp.f_pad = (ndims == 5) ? pd.padding[0][0] : 0;
    jpp.t_pad = (ndims == 3) ? 0 : pd.padding[0][ndims - 4];
    jpp.l_pad = pd.padding[0][ndims - 3];

    const int back_pad = calculate_end_padding(
            jpp.f_pad, jpp.od, jpp.id, jpp.stride_d, jpp.kd);
    const int bottom_pad = calculate_end_padding(
            jpp.t_pad, jpp.oh, jpp.ih, jpp.stride_h, jpp.kh);
    const int right_pad = calculate_end_padding(
            jpp.l_pad, jpp.ow, jpp.iw, jpp.stride_w, jpp.kw);

    VDISPATCH_POOLING_IC(
            !(jpp.f_pad >= jpp.kd || jpp.t_pad >= jpp.kh || jpp.l_pad >= jpp.kw
                    || back_pad >= jpp.kd || bottom_pad >= jpp.kh
                    || right_pad >= jpp.kw),
            VERBOSE_UNSUPPORTED_PAD_FEATURE, "");

    jpp.ind_dt = ppd->workspace_md() ? ppd->workspace_md()->data_type
                                     : data_type::undef;

    jpp.simple_alg = jpp.is_training
            || IMPLICATION(jpp.is_backward, jpp.kd <= jpp.stride_d);

    jpp.ur = 0;
    if (jpp.alg == pooling_max) {
        jpp.ur = is_avx512 ? 16 : 4;

        if (utils::one_of(isa, avx, avx2, avx2_vnni_2) && jpp.c_tail > 0)
            // Additional register needed for tail mask
            jpp.ur -= 1;

        if (jpp.is_training)
            jpp.ur = is_avx512 ? 9 : 3;
        else if (jpp.is_backward)
            jpp.ur = is_avx512 ? 6 : 3;
    } else {
        if (jpp.is_backward)
            jpp.ur = is_avx512 ? 12 : 6;
        else
            jpp.ur = is_avx512 ? 24 : 12;
    }
    if ((jpp.is_bf16 || jpp.is_f16) && isa != avx2_vnni_2) {
        jpp.ur = (!isa_has_bf16(jpp.isa))
                ? jpp.ur - 4 // Free registers for AVX512 emulation
                : jpp.ur - 1; // Free register for cvt from bf16/f16 to f32
    }

    if (jpp.is_fp8) {
        // TODO: Optimize the ur if native FP8 support is available
        jpp.ur = jpp.ur - 4;
    }
    assert(jpp.ur > 0);

    const bool is_relaxed_acc = utils::one_of(
            attr.acc_mode_, accumulation_mode::relaxed, accumulation_mode::any);
    jpp.needs_f32_accum_for_bf16 = !is_relaxed_acc && jpp.is_bf16
            && jpp.alg == alg_kind::pooling_max && jpp.is_backward
            && (jpp.stride_d < jpp.kd || jpp.stride_h < jpp.kh
                    || jpp.stride_w < jpp.kw);

    // select jpp.ur_bc
    if (jpp.tag_kind == jit_memory_tag_kind_t::nspc) {
        auto min_ur_w = nstl::max(1, utils::div_up(jpp.l_pad, jpp.stride_w));
        int min_ur_w1 = utils::div_up(right_pad, jpp.stride_w);
        if (min_ur_w < min_ur_w1) { min_ur_w = min_ur_w1; }
        jpp.ur_bc = nstl::min(jpp.nb_c, nstl::max(1, jpp.ur / min_ur_w));
        //take into account threading - to have enough work for parallelization
        float best_eff = 0;
        for (int ur_bc = jpp.ur_bc; ur_bc > 0; ur_bc--) {

            const auto nb2_c = utils::div_up(jpp.nb_c, ur_bc);
            auto work = jpp.is_backward
                    ? (ndims == 5 && jpp.simple_alg ? jpp.od : 1)
                    : (ndims == 5 ? jpp.od : jpp.oh);
            work *= jpp.mb * nb2_c;
            auto eff = (float)work / utils::rnd_up(work, jpp.nthr);
            if (eff > best_eff) {

                best_eff = eff;
                jpp.ur_bc = ur_bc;
            }
            if (eff > 0.9f) break; // Heuristic threshold
        }

        //take into account cache re-usage after zeroing on backward
        if (jpp.is_backward && ndims < 5 && !jpp.needs_f32_accum_for_bf16) {
            const int L2 = platform::get_per_core_cache_size(2) / jpp.dt_size;
            int ur_bc = nstl::max(1, L2 / (jpp.kh * jpp.iw * jpp.c_block));
            jpp.ur_bc = nstl::min(jpp.ur_bc, ur_bc);
        }

        jpp.ur_bc_tail = jpp.nb_c % jpp.ur_bc;
    } else {
        jpp.ur_bc = 1;
        jpp.ur_bc_tail = 0;
    }

    jpp.f32_accum_block_size = jpp.ur_bc * jpp.c_block;
    if (jpp.needs_f32_accum_for_bf16) {
        assert(memory_desc_wrapper(jpp.tmp_md).is_zero()
                && (fmt_tag == nspc_fmt_tag || fmt_tag == blocked_fmt_tag));

        dims_t dims {};
        utils::array_copy(dims, src_d.dims(), ndims);

        const auto nb2_c = utils::div_up(jpp.nb_c, jpp.ur_bc);
        dims[0] = nstl::min(dnnl_get_max_threads(), jpp.mb * nb2_c);
        dims[1] = jpp.f32_accum_block_size;

        CHECK(memory_desc_init_by_tag(
                jpp.tmp_md, ndims, dims, data_type::f32, fmt_tag));
    }

    return status::success;
}

template <cpu_isa_t isa>
void jit_uni_pool_kernel_t<isa>::init_scratchpad(
        const jit_pool_conf_t &jpp, memory_tracking::registrar_t &scratchpad) {

    // scratchpad for c_block slice of input and/or output
    using namespace memory_tracking::names;
    const int nscr = nstl::min(dnnl_get_max_threads(), jpp.mb * jpp.nb_c);
    if (jpp.tag_kind == jit_memory_tag_kind_t::ncsp) {
        scratchpad.book(key_pool_src_plain2blocked_cvt,
                static_cast<size_t>(jpp.c_block) * jpp.id * jpp.ih * jpp.iw
                        * nscr,
                jpp.dt_size);
        scratchpad.book(key_pool_dst_plain2blocked_cvt,
                static_cast<size_t>(jpp.c_block) * jpp.od * jpp.oh * jpp.ow
                        * nscr,
                jpp.dt_size);
        scratchpad.book<uint32_t>(key_pool_ind_plain2blocked_cvt,
                static_cast<size_t>(jpp.c_block) * jpp.od * jpp.oh * jpp.ow
                        * nscr);
    }

    if (jpp.needs_f32_accum_for_bf16) {
        auto tmp_d = memory_desc_wrapper(jpp.tmp_md);
        scratchpad.book<char>(key_pool_src_f32_accum, tmp_d.size());
    }
}

static int reg_ind(int shift, int bc, int j, int ur_bc, int ur_w) noexcept {
    return shift * ur_bc * ur_w + bc * ur_w + j;
};

template <cpu_isa_t isa>
inline void jit_uni_pool_kernel_t<isa>::put_one_in_vmm() {
    mov(tmp_gpr, 1);
    uni_broadcast_reg_val(tmp_gpr.getIdx(), vmm_one.getIdx());
}

template <cpu_isa_t isa>
inline void jit_uni_pool_kernel_t<isa>::uni_broadcast_reg_val(
        const int reg_idx, const int vmm_idx) {
    uni_vmovq(Xmm(vmm_idx), reg64_t(reg_idx));
    uni_vpbroadcastd(Vmm(vmm_idx), Xmm(vmm_idx));
}

template <cpu_isa_t isa>
inline void jit_uni_pool_kernel_t<isa>::push_vmm_val(const int idx) {
    Vmm val_to_store(idx);
    sub(rsp, val_to_store.getBit());
    uni_vmovups(ptr[rsp], val_to_store);
}

template <cpu_isa_t isa>
inline void jit_uni_pool_kernel_t<isa>::pop_vmm_val(const int idx) {
    Vmm val_to_load(idx);
    uni_vmovups(val_to_load, ptr[rsp]);
    add(rsp, val_to_load.getBit());
}

template <cpu_isa_t isa>
inline void jit_uni_pool_kernel_t<isa>::load(const data_type_t dt,
        const int idx, const reg64_t &reg_ptr, const int offset,
        const bool is_c_tail_proccessing) {
    io_[dt]->load(vmmword[reg_ptr + offset], Vmm(idx),
            is_c_tail_proccessing && !jpp.is_c_padded);
}

template <cpu_isa_t isa>
inline void jit_uni_pool_kernel_t<isa>::store(const data_type_t dt,
        const int idx, const reg64_t &reg_ptr, const int offset,
        const bool is_c_tail_proccessing) {
    if (is_c_tail_proccessing && jpp.is_c_padded && jpp.with_postops)
        pad_with_zeros(idx);
    io_[dt]->store(Vmm(idx), vmmword[reg_ptr + offset],
            is_c_tail_proccessing && !jpp.is_c_padded);
}

template <cpu_isa_t isa>
inline void jit_uni_pool_kernel_t<isa>::pad_with_zeros(const int idx) {
    if (isa == sse41) {
        uni_vxorps(xmm_tmp_1, xmm_tmp_1, xmm_tmp_1);
        if (jpp.c_tail <= sse41_single_block_size && sse_high_half) {
            uni_vmovups(Vmm(idx), xmm_tmp_1);
        } else if ((jpp.c_tail < sse41_single_block_size && !sse_high_half)
                || (jpp.c_tail > sse41_single_block_size && sse_high_half)) {
            const auto c_tail = jpp.c_tail % sse41_single_block_size;
            std::bitset<8> tail_mask((1 << c_tail) - 1);
            tail_mask.flip();
            uni_vblendps(Vmm(idx), Vmm(idx), xmm_tmp_1, tail_mask.to_ulong());
        }
    } else if (isa == avx || isa == avx2) {
        uni_vxorps(ymm_tmp_1, ymm_tmp_1, ymm_tmp_1);
        uni_vblendvps(Vmm(idx), ymm_tmp_1, Vmm(idx), vmm_c_tail_mask);
    } else
        uni_vmovups(Vmm(idx) | k_c_tail_mask | T_z, Vmm(idx));
}

template <cpu_isa_t isa>
inline void jit_uni_pool_kernel_t<isa>::load_indices(
        const int indr_i, const int step_index, bool is_c_tail_processing) {
    if (jpp.ind_dt == data_type::u8) {
        auto indvr = vreg(indr_i);
        auto indxr = xreg(indr_i);
        if (isa == sse41) {
            if (is_c_tail_processing && !jpp.is_c_padded) {
                for (int i = 0; i < jpp.c_tail % (jpp.c_block / 2); i++)
                    pinsrb(indxr, ptr[reg_index + step_index + i], i);
            } else {
                movd(indxr, ptr[reg_index + step_index]);
            }
            pmovzxbd(indvr, indxr);
        } else if (isa == avx || isa == avx2) {
            if (is_c_tail_processing && !jpp.is_c_padded) {
                for (int i = 0; i < jpp.c_tail; i++)
                    vpinsrb(indxr, indxr, ptr[reg_index + step_index + i], i);
            } else {
                vmovq(indxr, ptr[reg_index + step_index]);
            }
            if (!mayiuse(avx2)) {
                avx_pmovzxbd(indvr, indxr, xmm_tmp);
            } else {
                vpmovzxbd(indvr, indxr);
            }
        } else {
            if (is_c_tail_processing && !jpp.is_c_padded) {
                vpmovzxbd(indvr | k_c_tail_mask | T_z,
                        ptr[reg_index + step_index]);
            } else {
                vpmovzxbd(indvr, ptr[reg_index + step_index]);
            }
        }
    } else {
        assert(jpp.ind_dt == data_type::s32);

        // Load 4-byte values without conversion. The values are actually integers.
        auto indvr = vreg(indr_i);
        io_[data_type::f32]->load(vmmword[reg_index + step_index], indvr,
                is_c_tail_processing && !jpp.is_c_padded);
    }
}

template <cpu_isa_t isa>
inline void jit_uni_pool_kernel_t<isa>::store_indices(const int indr_i,
        const int step_index, const bool is_c_tail_processing,
        const bool is_first_w_block) {
    if (jpp.ind_dt == data_type::u8) {
        auto xr = xreg(indr_i);
        if (isa == sse41) {
            for (int i = 0; i < (jpp.c_block / 2); ++i) {
                if (is_c_tail_processing
                        && i + (sse_high_half ? (jpp.c_block / 2) : 0)
                                >= jpp.c_tail) {
                    if (jpp.is_c_padded)
                        mov(ptr[reg_index + step_index + i],
                                tmp_gpr.cvt8()); // fill padded tail with zeros
                    else
                        break; // tail end
                } else {
                    // bytes which should be stored are located in
                    // least significant bits(8 to be precise) of 32 bits parts
                    // of xmm thus we need to store 0, 4, 8 and 12 byte of xmm
                    pextrb(ptr[reg_index + step_index + i], xr, 4 * i);
                }
            }
        } else if (utils::one_of(isa, avx, avx2, avx2_vnni_2)) {
            auto yr = yreg(indr_i);
            if (is_c_tail_processing && !jpp.is_c_padded) {
                const int max_nr_of_vals = jpp.c_tail > (jpp.c_block / 2)
                        ? (jpp.c_block / 2)
                        : jpp.c_tail;
                for (int i = 0; i < max_nr_of_vals; ++i) {
                    // bytes which should be stored are located in
                    // least significant bits(8 to be precise) of 32 bits parts
                    // of xmm thus we need to store 0, 4, 8 and 12 byte of xmm
                    vpextrb(ptr[reg_index + step_index + i], xr, 4 * i);
                }

                if (jpp.c_tail > (jpp.c_block / 2)) {
                    Xmm higher_128bits(vmm_mask.getIdx());
                    vextractf128(higher_128bits, yr, 1);
                    for (int i = 0; i < jpp.c_tail - (jpp.c_block / 2); ++i) {
                        // bytes which should be stored are located in
                        // least significant bits(8 to be precise) of 32 bits parts
                        // of xmm thus we need to store 0, 4, 8 and 12 byte of xmm
                        vpextrb(ptr[reg_index + step_index + (jpp.c_block / 2)
                                        + i],
                                higher_128bits, 4 * i);
                    }
                }
            } else {
                if (is_c_tail_processing) {
                    assert(jpp.is_c_padded);
                    vandps(yr, yr, vmm_c_tail_mask);
                }
                if (is_first_w_block) {
                    vmovd(xmm_tmp, reg_shuf_mask);
                    uni_vpbroadcastd(vmm_tmp, xmm_tmp);
                }
                if (mayiuse(avx2)) {
                    vpshufb(yr, yr, vmm_tmp);
                    vmovd(ptr[reg_index + step_index], xr);
                    vperm2i128(yr, yr, yr, 0x1u);
                    vmovd(ptr[reg_index + step_index + (jpp.c_block / 2)], xr);
                } else {
                    Xmm t(vmm_mask.getIdx());
                    vextractf128(t, yr, 0);
                    vpshufb(t, t, xmm_tmp);
                    vmovd(ptr[reg_index + step_index], t);
                    vextractf128(t, yr, 1);
                    vpshufb(t, t,
                            xmm_tmp); // ymm_tmp[:128]==ymm_tmp[127:0]
                    vmovd(ptr[reg_index + step_index + (jpp.c_block / 2)], t);
                }
            }
        } else {
            auto vr = vreg(indr_i);
            if (is_c_tail_processing) {
                if (jpp.is_c_padded) {
                    knotw(k_c_tail_mask, k_c_tail_mask);
                    vpxord(vr | k_c_tail_mask, vr, vr);
                    knotw(k_c_tail_mask, k_c_tail_mask);
                    vpmovusdb(ptr[reg_index + step_index], vr);
                } else
                    vpmovusdb(ptr[reg_index + step_index], vr | k_c_tail_mask);
            } else {
                vpmovusdb(ptr[reg_index + step_index], vr);
            }
        }
    } else {
        assert(jpp.ind_dt == data_type::s32);

        // Store 4-byte values without conversion. The values are actually integers.
        auto idx = reg_idx(indr_i);
        if (is_c_tail_processing && jpp.is_c_padded) pad_with_zeros(idx);
        io_[data_type::f32]->store(Vmm(idx), vmmword[reg_index + step_index],
                is_c_tail_processing && !jpp.is_c_padded);
    }
}

template <cpu_isa_t isa>
bool jit_uni_pool_kernel_t<isa>::init_post_ops_conf(jit_pool_conf_t &jpp,
        const primitive_attr_t &attr, const memory_desc_wrapper &dst_d) {
    const auto &post_ops = attr.post_ops_;
    jpp.with_postops = false;
    jpp.with_eltwise = false;
    jpp.with_binary = false;

    if (post_ops.len() == 0) return true;

    if (jpp.is_backward) { return false; }

    jpp.with_eltwise = post_ops.find(primitive_kind::eltwise) != -1;
    jpp.with_binary = post_ops.find(primitive_kind::binary) != -1;
    jpp.with_postops = jpp.with_eltwise || jpp.with_binary;

    if (!jpp.with_postops) return false;

    jpp.post_ops = post_ops;

    using namespace injector;
    return post_ops_ok(post_ops_ok_args_t(isa, {binary, eltwise},
            attr.post_ops_, &dst_d, false /*sum_at_pos_0_only*/,
            false /*sum_requires_scale_one*/, false /*sum_requires_zp_zero*/,
            false /*sum_requires_same_params*/,
            get_supported_bcast_strategies()));
}

template <cpu_isa_t isa>
void jit_uni_pool_kernel_t<isa>::apply_postops(int ur_bc, int ur_w, int c_block,
        const std::function<bool(int, bool)> &is_tail_predicate) {
    binary_injector::rhs_arg_dynamic_params_t rhs_arg_params;
    const int end_idx = vmm_idx_upper_bound() + 1;
    const int start_idx = end_idx - (ur_bc * ur_w);
    const bool sse41_postops_disabled
            = isa == sse41 && disable_postops_when_sse_high_half_processed_;
    if (end_idx - start_idx == 0) return;
    if (jpp.with_binary && !sse41_postops_disabled) {

        const int c_off = (jpp.tag_kind == jit_memory_tag_kind_t::nspc)
                ? jpp.c
                : c_block;

        if (jpp.tag_kind == jit_memory_tag_kind_t::ncsp) {
            mov(tmp_gpr, reg_output);
            sub(tmp_gpr, ptr[reg_param + GET_OFF(dst)]);
            add(tmp_gpr, ptr[reg_param + GET_OFF(dst_po_helper)]);
        }

        for (int jj = 0; jj < ur_w; jj++) {
            for (int bci = 0; bci < ur_bc; bci++) {
                const auto vmm_idx
                        = vreg(reg_ind(0, bci, jj, ur_bc, ur_w)).getIdx();

                const size_t output_offset
                        = jpp.dt_size * (jj * c_off + bci * c_block);

                rhs_arg_params.vmm_idx_to_out_reg.emplace(vmm_idx,
                        jpp.tag_kind == jit_memory_tag_kind_t::ncsp
                                ? tmp_gpr
                                : reg_output);
                rhs_arg_params.vmm_idx_to_out_elem_off_val.emplace(
                        vmm_idx, output_offset);
                if (is_tail_predicate
                        && is_tail_predicate(
                                bci, true /*process_with_postops*/)) {
                    rhs_arg_params.vmm_tail_idx_.emplace(vmm_idx);
                }
            }
        }
    }
    postops_injector_->compute_vector_range(start_idx, end_idx, rhs_arg_params);
}

template <cpu_isa_t isa>
inline void jit_uni_pool_kernel_t<isa>::maybe_recalculate_divisor(
        int jj, int ur_w, int pad_l, int pad_r, bool with_c_tail_proccessing) {
    if (jpp.alg == pooling_avg_exclude_padding) {
        int kw = jpp.kw;
        int stride_w = jpp.stride_w;

        int non_zero_kw = kw;
        non_zero_kw -= nstl::max(0, pad_l - jj * stride_w);
        non_zero_kw -= nstl::max(0, pad_r - (ur_w - 1 - jj) * stride_w);

        if (non_zero_kw != prev_kw) {
            mov(tmp_gpr, float2int((float)non_zero_kw));
            uni_vmovq(xmm_tmp, tmp_gpr);
            uni_vbroadcastss(vmm_tmp, xmm_tmp);
            if (with_c_tail_proccessing
                    && (utils::one_of(isa, avx, avx2, avx2_vnni_2))) {
                push_vmm_val(vmm_c_tail_mask.getIdx());
                uni_broadcast_reg_val(
                        reg_ker_area_h.getIdx(), vmm_ker_area_h.getIdx());
            }
            uni_vmulps(vmm_tmp, vmm_tmp, vmm_ker_area_h);
            if (with_c_tail_proccessing
                    && (utils::one_of(isa, avx, avx2, avx2_vnni_2))) {
                pop_vmm_val(vmm_c_tail_mask.getIdx());
            }
            prev_kw = non_zero_kw;
        }
    }
}

template <cpu_isa_t isa>
inline void jit_uni_pool_kernel_t<isa>::avg_step(int ur_w, int ur_bc, int pad_l,
        int pad_r, bool with_c_tail_proccessing) {

    auto iw = jpp.iw;
    auto kw = jpp.kw;
    auto stride_w = jpp.stride_w;
    auto c_block = jpp.c_block;
    auto dt_size = jpp.dt_size;
    const int c_off
            = (jpp.tag_kind == jit_memory_tag_kind_t::nspc) ? jpp.c : c_block;
    Label kd_label, kh_label;

    const auto is_tail_processing = [&](int bc,
                                            bool process_with_postops = false) {
        if (isa == sse41 && (!jpp.is_c_padded || process_with_postops)) {
            return with_c_tail_proccessing && bc == (ur_bc - 1)
                    && ((jpp.c_tail > (jpp.c_block / 2) && sse_high_half)
                            || (jpp.c_tail < (jpp.c_block / 2)
                                    && !sse_high_half));
        } else
            return with_c_tail_proccessing && bc == (ur_bc - 1);
    };

    for (int jj = 0; jj < ur_w; jj++) {
        if (jpp.is_backward)
            maybe_recalculate_divisor(
                    jj, ur_w, pad_l, pad_r, with_c_tail_proccessing);
        for (int bci = 0; bci < ur_bc; bci++) {
            const auto accr_i = reg_ind(0, bci, jj, ur_bc, ur_w);
            auto accvr = vreg(accr_i);
            if (jpp.is_backward) {
                auto output_offset = dt_size * (jj * c_off + bci * c_block);
                load(jpp.dst_dt, accvr.getIdx(), reg_output, output_offset,
                        is_tail_processing(bci));
                uni_vdivps(accvr, accvr, vmm_tmp);
            } else {
                uni_vpxor(accvr, accvr, accvr);
            }
        }
    }

    if (jpp.simple_alg && jpp.ndims == 5) {
        push(reg_input);
        push(reg_output);
        mov(aux_reg_input_d, reg_input);
        mov(ki, ptr[reg_param + GET_OFF(kd_padding)]);
        L(kd_label);
        mov(aux_reg_input, aux_reg_input_d);
    } else {
        mov(aux_reg_input, reg_input);
    }

    xor_(kj, kj);
    L(kh_label);
    {
        for (int ki = 0; ki < kw; ki++) {
            int jj_start = nstl::max(0, utils::div_up(pad_l - ki, stride_w));
            int jj_end = ur_w
                    - utils::div_up(
                            nstl::max(0, ki + pad_r - (kw - 1)), stride_w);

            for_(int jj = jj_start; jj < jj_end; jj++)
            for (int bci = 0; bci < ur_bc; bci++) {
                const auto accvr = vreg(reg_ind(0, bci, jj, ur_bc, ur_w));
                const auto inpr_i = reg_ind(1, bci, jj, ur_bc, ur_w);
                auto inpvr = vreg(inpr_i);
                int aux_input_offset
                        = (ki + jj * stride_w - pad_l) * c_off + bci * c_block;
                if (aux_input_offset >= iw * c_off) continue;
                int input_offset = dt_size * aux_input_offset;
                if (jpp.is_backward) {
                    load(jpp.src_dt, reg_idx(inpr_i), aux_reg_input,
                            input_offset, is_tail_processing(bci));
                    uni_vaddps(inpvr, inpvr, accvr);
                    store(jpp.src_dt, reg_idx(inpr_i), aux_reg_input,
                            input_offset, is_tail_processing(bci));
                } else {
                    load(jpp.src_dt, vmm_tmp_1.getIdx(), aux_reg_input,
                            input_offset, is_tail_processing(bci));
                    uni_vaddps(accvr, accvr, vmm_tmp_1);
                }
            }
        }
        add(aux_reg_input, jpp.dt_size * iw * c_off);
        inc(kj);
        cmp(kj, reg_kh);
        jl(kh_label, T_NEAR);
    }

    if (jpp.simple_alg && jpp.ndims == 5) {
        add(aux_reg_input_d, jpp.dt_size * jpp.ih * iw * c_off);
        dec(ki);
        cmp(ki, 0);
        jg(kd_label, T_NEAR);
        pop(reg_output);
        pop(reg_input);
    }

    if (!jpp.is_backward) {
        for (int jj = 0; jj < ur_w; jj++) {
            maybe_recalculate_divisor(
                    jj, ur_w, pad_l, pad_r, with_c_tail_proccessing);
            for (int bci = 0; bci < ur_bc; bci++) {
                const auto accr_i = reg_ind(0, bci, jj, ur_bc, ur_w);
                const auto accvr = vreg(accr_i);
                uni_vdivps(accvr, accvr, vmm_tmp);
            }
        }

        if (jpp.with_postops)
            apply_postops(ur_bc, ur_w, c_block, is_tail_processing);

        for (int jj = 0; jj < ur_w; jj++) {
            for (int bci = 0; bci < ur_bc; bci++) {
                const auto accr_i = reg_ind(0, bci, jj, ur_bc, ur_w);
                const auto output_offset
                        = dt_size * (jj * c_off + bci * c_block);
                store(jpp.dst_dt, reg_idx(accr_i), reg_output, output_offset,
                        is_tail_processing(bci));
            }
        }
    }
}

template <cpu_isa_t isa>
inline void jit_uni_pool_kernel_t<isa>::max_step_fwd(int ur_w, int ur_bc,
        int pad_l, int pad_r, bool with_c_tail_proccessing) {
    int iw = jpp.iw;
    int kw = jpp.kw;
    int stride_w = jpp.stride_w;
    int c_block = jpp.c_block;
    const int c_off
            = (jpp.tag_kind == jit_memory_tag_kind_t::nspc) ? jpp.c : c_block;
    Label kd_label, kh_label;

    auto is_tail_processing = [&](int bc, bool process_with_postops = false) {
        if (isa == sse41 && (!jpp.is_c_padded || process_with_postops)) {
            return with_c_tail_proccessing && bc == (ur_bc - 1)
                    && ((jpp.c_tail > (jpp.c_block / 2) && sse_high_half)
                            || (jpp.c_tail < (jpp.c_block / 2)
                                    && !sse_high_half));
        } else
            return with_c_tail_proccessing && bc == (ur_bc - 1);
    };

    mov(tmp_gpr, float2int(nstl::numeric_limits<float>::lowest()));
    uni_vmovq(xmm_tmp, tmp_gpr);
    uni_vbroadcastss(vmm_tmp, xmm_tmp);

    for_(int jj = 0; jj < ur_w; jj++)
    for (int bci = 0; bci < ur_bc; bci++) {
        const auto accvr = vreg(reg_ind(0, bci, jj, ur_bc, ur_w));
        uni_vmovups(accvr, vmm_tmp);
        if (jpp.is_training) {
            const auto indvr = vreg(reg_ind(2, bci, jj, ur_bc, ur_w));
            uni_vpxor(indvr, indvr, indvr);
        }
    }
    if (jpp.is_training) {
        uni_vmovq(xmm_tmp, reg_k_shift);
        uni_vpbroadcastd(vmm_k_offset, xmm_tmp);
    }
    if (jpp.ndims == 5) {
        push(reg_input);
        push(reg_output);
        mov(aux_reg_input_d, reg_input);
        mov(ki, ptr[reg_param + GET_OFF(kd_padding)]);
        L(kd_label);
        mov(aux_reg_input, aux_reg_input_d);
    } else {
        mov(aux_reg_input, reg_input);
    }
    xor_(kj, kj);
    L(kh_label);
    {
        for (int ki = 0; ki < kw; ki++) {
            int jj_start = nstl::max(0, utils::div_up(pad_l - ki, stride_w));
            int jj_end = ur_w
                    - utils::div_up(
                            nstl::max(0, ki + pad_r - (kw - 1)), stride_w);
            for_(int jj = jj_start; jj < jj_end; jj++)
            for (int bci = 0; bci < ur_bc; bci++) {
                const auto accvr = vreg(reg_ind(0, bci, jj, ur_bc, ur_w));
                const auto inpr_i = reg_ind(1, bci, jj, ur_bc, ur_w);
                const auto inpvr = vreg(inpr_i);
                const auto indvr = vreg(reg_ind(2, bci, jj, ur_bc, ur_w));
                const auto cvtvr = vreg(reg_ind(3, bci, jj, ur_bc, ur_w));
                int aux_input_offset
                        = (ki + jj * stride_w - pad_l) * c_off + bci * c_block;
                if (aux_input_offset >= iw * c_off) continue;
                int input_offset = jpp.dt_size * aux_input_offset;
                load(jpp.src_dt, reg_idx(inpr_i), aux_reg_input, input_offset,
                        is_tail_processing(bci));
                if (isa == sse41) {
                    movups(vmm_mask, accvr);
                    cmpps(vmm_mask, inpvr, _cmp_lt_os);
                    blendvps(accvr, inpvr);
                    if (jpp.is_training) blendvps(indvr, vmm_k_offset);
                } else if (utils::one_of(isa, avx, avx2, avx2_vnni_2)) {
                    vcmpps(cvtvr, accvr, inpvr, _cmp_lt_os);
                    vblendvps(accvr, accvr, inpvr, cvtvr);
                    if (jpp.is_training)
                        vblendvps(indvr, indvr, vmm_k_offset, cvtvr);
                } else {
                    vcmpps(k_store_mask, accvr, inpvr, _cmp_lt_os);
                    vblendmps(accvr | k_store_mask, accvr, inpvr);
                    if (jpp.is_training)
                        vblendmps(indvr | k_store_mask, indvr, vmm_k_offset);
                }
            }
            if (jpp.is_training) {
                if (with_c_tail_proccessing
                        && (utils::one_of(isa, avx, avx2, avx2_vnni_2))) {
                    push_vmm_val(vmm_c_tail_mask.getIdx());
                    put_one_in_vmm();
                }

                if (isa == avx && !mayiuse(avx2))
                    avx_vpadd1(vmm_k_offset, vmm_one, xmm_tmp);
                else
                    uni_vpaddd(vmm_k_offset, vmm_k_offset, vmm_one);

                if (with_c_tail_proccessing
                        && (utils::one_of(isa, avx, avx2, avx2_vnni_2)))
                    pop_vmm_val(vmm_c_tail_mask.getIdx());
            }
        }
        add(aux_reg_input, jpp.dt_size * iw * c_off);
        inc(kj);
        cmp(kj, reg_kh);
        jl(kh_label, T_NEAR);
    }

    if (jpp.ndims == 5) {
        add(aux_reg_input_d, jpp.dt_size * jpp.ih * iw * c_off);
        if (jpp.is_training) {
            mov(tmp_gpr, ptr[reg_param + GET_OFF(kd_padding_shift)]);
            uni_vmovq(xmm_tmp, tmp_gpr);
            uni_vpbroadcastd(vmm_tmp, xmm_tmp);
            if (isa == avx && !mayiuse(avx2)) {
                Xmm t(vmm_mask.getIdx());
                avx_vpadd1(vmm_k_offset, xmm_tmp, t);
            } else {
                uni_vpaddd(vmm_k_offset, vmm_k_offset, vmm_tmp);
            }
        }

        dec(ki);
        cmp(ki, 0);
        jg(kd_label, T_NEAR);
        pop(reg_output);
        pop(reg_input);
    }

    if (with_c_tail_proccessing && jpp.is_c_padded && isa == sse41)
        mov(tmp_gpr, 0); // needed zero to fill padded tail

    if (jpp.with_postops)
        apply_postops(ur_bc, ur_w, c_block, is_tail_processing);

    for_(int jj = 0; jj < ur_w; jj++)
    for (int bci = 0; bci < ur_bc; bci++) {
        const auto accr_i = reg_ind(0, bci, jj, ur_bc, ur_w);
        const auto output_offset = jpp.dt_size * (jj * c_off + bci * c_block);
        const bool is_c_tail_processing = is_tail_processing(bci);
        store(jpp.dst_dt, reg_idx(accr_i), reg_output, output_offset,
                is_c_tail_processing);

        if (jpp.is_training) {
            const size_t step_index = (jj * c_off + bci * c_block)
                    * types::data_type_size(jpp.ind_dt);

            const auto indr_i = reg_ind(2, bci, jj, ur_bc, ur_w);
            const bool is_first_w_block = jj == 0;
            store_indices(
                    indr_i, step_index, is_c_tail_processing, is_first_w_block);
        }
    }
}

template <cpu_isa_t isa>
inline void jit_uni_pool_kernel_t<isa>::max_step_bwd(int ur_w, int ur_bc,
        int pad_l, int pad_r, bool with_c_tail_proccessing) {

    int iw = jpp.iw;
    int kw = jpp.kw;
    int stride_w = jpp.stride_w;
    int c_block = jpp.c_block;
    const int output_c_off
            = (jpp.tag_kind == jit_memory_tag_kind_t::nspc) ? jpp.c : c_block;
    const int input_c_off = jpp.needs_f32_accum_for_bf16
            ? jpp.f32_accum_block_size
            : output_c_off;
    const auto input_dt
            = jpp.needs_f32_accum_for_bf16 ? data_type::f32 : jpp.src_dt;
    const size_t input_dt_size = types::data_type_size(input_dt);
    const size_t output_dt_size = jpp.dt_size;
    assert(output_dt_size == types::data_type_size(jpp.dst_dt));

    Label kd_label, kh_label;

    const auto is_tail_processing = [&](int bc) {
        if (isa == sse41) {
            return with_c_tail_proccessing && bc == (ur_bc - 1)
                    && ((jpp.c_tail > (jpp.c_block / 2) && sse_high_half)
                            || (jpp.c_tail < (jpp.c_block / 2)
                                    && !sse_high_half)
                            || (jpp.c_tail == (jpp.c_block / 2) && sse_high_half
                                    && jpp.is_c_padded));
        } else
            return with_c_tail_proccessing && bc == (ur_bc - 1);
    };

    for_(int jj = 0; jj < ur_w; jj++)
    for (int bci = 0; bci < ur_bc; bci++) {
        const auto outr_i = reg_ind(0, bci, jj, ur_bc, ur_w);
        auto out_offset = output_dt_size * (jj * output_c_off + bci * c_block);
        const bool is_c_tail_processing = is_tail_processing(bci);
        load(jpp.dst_dt, reg_idx(outr_i), reg_output, out_offset,
                is_c_tail_processing);
        const size_t step_index = (jj * output_c_off + bci * c_block)
                * types::data_type_size(jpp.ind_dt);

        const auto indr_i = reg_ind(1, bci, jj, ur_bc, ur_w);
        load_indices(indr_i, step_index, is_c_tail_processing);
    }
    uni_vmovq(xmm_tmp, reg_k_shift);
    uni_vpbroadcastd(vmm_k_offset, xmm_tmp);

    if (jpp.simple_alg && jpp.ndims == 5) {
        push(reg_input);
        push(reg_output);
        mov(aux_reg_input_d, reg_input);
        mov(ki, ptr[reg_param + GET_OFF(kd_padding)]);
        mov(reg_kd_pad_shift, ptr[reg_param + GET_OFF(kd_padding_shift)]);
        L(kd_label);
        mov(aux_reg_input, aux_reg_input_d);
    } else {
        mov(aux_reg_input, reg_input);
    }

    xor_(kj, kj);
    L(kh_label);
    {
        for (int ki = 0; ki < kw; ki++) {
            int jj_start = nstl::max(0, utils::div_up(pad_l - ki, stride_w));
            int jj_end = ur_w
                    - utils::div_up(
                            nstl::max(0, ki + pad_r - (kw - 1)), stride_w);
            for_(int jj = jj_start; jj < jj_end; jj++)
            for (int bci = 0; bci < ur_bc; bci++) {
                const auto outvr = vreg(reg_ind(0, bci, jj, ur_bc, ur_w));
                const auto indvr = vreg(reg_ind(1, bci, jj, ur_bc, ur_w));
                const auto inpr_i = reg_ind(2, bci, jj, ur_bc, ur_w);
                const auto inpvr = vreg(inpr_i);
                const auto cvtvr = vreg(reg_ind(3, bci, jj, ur_bc, ur_w));
                int aux_inp_offset = (ki + jj * stride_w - pad_l) * input_c_off
                        + bci * c_block;
                if (aux_inp_offset >= iw * input_c_off) continue;
                int inp_offset = input_dt_size * aux_inp_offset;
                load(input_dt, reg_idx(inpr_i), aux_reg_input, inp_offset,
                        is_tail_processing(bci));
                if (isa == sse41) {
                    movups(cvtvr, indvr);
                    pcmpeqd(cvtvr, vmm_k_offset);
                    vandps(cvtvr, cvtvr, outvr);
                    addps(inpvr, cvtvr);
                    store(input_dt, inpvr.getIdx(), aux_reg_input, inp_offset,
                            is_tail_processing(bci));
                } else if (isa == avx || isa == avx2) {
                    if (mayiuse(avx2)) {
                        vpcmpeqd(cvtvr, indvr, vmm_k_offset);
                    } else {
                        avx_pcmpeqd(cvtvr, indvr, vmm_k_offset, xmm_tmp);
                    }
                    uni_vpxor(vmm_tmp, vmm_tmp, vmm_tmp);
                    vblendvps(vmm_tmp, vmm_tmp, outvr, cvtvr);
                    vaddps(inpvr, inpvr, vmm_tmp);
                    store(input_dt, inpvr.getIdx(), aux_reg_input, inp_offset,
                            is_tail_processing(bci));
                } else {
                    vpcmpeqd(k_store_mask, indvr, vmm_k_offset);
                    vblendmps(vmm_tmp | k_store_mask | T_z, outvr, outvr);
                    vaddps(inpvr, inpvr, vmm_tmp);
                    store(input_dt, inpvr.getIdx(), aux_reg_input, inp_offset,
                            is_tail_processing(bci));
                }
            }

            if (with_c_tail_proccessing && (isa == avx || isa == avx2)) {
                push_vmm_val(vmm_c_tail_mask.getIdx());
                put_one_in_vmm();
            }

            if (isa == avx && !mayiuse(avx2)) {
                avx_vpadd1(vmm_k_offset, vmm_one, xmm_tmp);
            } else {
                uni_vpaddd(vmm_k_offset, vmm_k_offset, vmm_one);
            }

            if (with_c_tail_proccessing && (isa == avx || isa == avx2))
                pop_vmm_val(vmm_c_tail_mask.getIdx());
        }
        add(aux_reg_input, input_dt_size * iw * input_c_off);
        inc(kj);
        cmp(kj, reg_kh);
        jl(kh_label, T_NEAR);
    }
    if (jpp.simple_alg && jpp.ndims == 5) {
        add(aux_reg_input_d, input_dt_size * jpp.ih * iw * input_c_off);

        mov(tmp_gpr, reg_kd_pad_shift);
        uni_vmovq(xmm_tmp, tmp_gpr);
        uni_vpbroadcastd(vmm_tmp, xmm_tmp);
        if (isa == avx && !mayiuse(avx2)) {
            Xmm t(vmm_mask.getIdx());
            avx_vpadd1(vmm_k_offset, vmm_tmp, t);
        } else {
            uni_vpaddd(vmm_k_offset, vmm_k_offset, vmm_tmp);
        }

        dec(ki);
        cmp(ki, 0);
        jg(kd_label, T_NEAR);
        pop(reg_output);
        pop(reg_input);
    }
}

template <cpu_isa_t isa>
void jit_uni_pool_kernel_t<isa>::zero_diff_src(
        int ur_bc, bool with_c_tail_proccessing) {
    const int c_off = jpp.needs_f32_accum_for_bf16
            ? jpp.f32_accum_block_size
            : ((jpp.tag_kind == jit_memory_tag_kind_t::nspc) ? jpp.c
                                                             : jpp.c_block);

    Label l_skip, l_ih_loop, l_id_loop;

    auto is_tail_processing = [&](int bc) {
        return with_c_tail_proccessing && bc == (ur_bc - 1);
    };

    mov(reg_zero_id, ptr[reg_param + GET_OFF(zero_id)]);
    cmp(reg_zero_id, 0);
    jz(l_skip, T_NEAR);

    mov(reg_zero_ih, ptr[reg_param + GET_OFF(zero_ih)]);
    cmp(reg_zero_ih, 0);
    jz(l_skip, T_NEAR);

    mov(reg_zero_ptr, ptr[reg_param + GET_OFF(zero_ptr)]);

    Vmm vzero = vmm_tmp;
    uni_vpxor(vzero, vzero, vzero);

    const auto src_dt
            = jpp.needs_f32_accum_for_bf16 ? data_type::f32 : jpp.src_dt;
    const auto dt_size = types::data_type_size(src_dt);
    const int width_size = jpp.iw * c_off * dt_size;

    auto aux_reg_zero_ptr = tmp_gpr;

    L(l_id_loop);
    {
        mov(aux_reg_zero_ptr, reg_zero_ptr);
        mov(aux_reg_zero_ih, reg_zero_ih);
        L(l_ih_loop);
        {
            const auto vlen = cpu_isa_traits_t<isa>::vlen;
            const int step = c_off * dt_size;

            // TODO: maybe a big code generated here
            for_(int i = 0; i < width_size; i += step)
            for (int bci = 0; bci < ur_bc; bci++) {
                const int offs = i + bci * jpp.c_block * dt_size;
                if (isa == sse41) {
                    bool is_needed_c_tail_processing = false;
                    if (is_tail_processing(bci)
                            && jpp.c_tail < (jpp.c_block / 2))
                        is_needed_c_tail_processing = true;
                    store(src_dt, vzero.getIdx(), reg_zero_ptr, offs,
                            is_needed_c_tail_processing);
                    if (!is_tail_processing(bci)
                            || (is_tail_processing(bci)
                                    && (jpp.is_c_padded
                                            || jpp.c_tail
                                                    > (jpp.c_block / 2)))) {
                        store(src_dt, vzero.getIdx(), reg_zero_ptr, offs + vlen,
                                is_tail_processing(bci));
                    }

                } else {
                    store(src_dt, vzero.getIdx(), reg_zero_ptr, offs,
                            is_tail_processing(bci));
                }
            }
            add(reg_zero_ptr, width_size);
            dec(aux_reg_zero_ih);
            jnz(l_ih_loop, T_NEAR);
        }
        mov(reg_zero_ptr, aux_reg_zero_ptr);
        add(reg_zero_ptr, width_size * jpp.ih);
        dec(reg_zero_id);
        jnz(l_id_loop, T_NEAR);
    }

    L(l_skip);
}

template <cpu_isa_t isa>
void jit_uni_pool_kernel_t<isa>::generate() {

    this->preamble();

    int ow = jpp.ow;
    int iw = jpp.iw;
    int kw = jpp.kw;
    int kh = jpp.kh;
    int c_block = jpp.c_block;
    int stride_w = jpp.stride_w;
    int l_pad = jpp.l_pad;
    const int output_c_off
            = (jpp.tag_kind == jit_memory_tag_kind_t::nspc) ? jpp.c : c_block;
    const int input_c_off = jpp.needs_f32_accum_for_bf16
            ? jpp.f32_accum_block_size
            : output_c_off;

    int vlen = cpu_isa_traits_t<isa>::vlen;

    const size_t input_dt_size
            = jpp.needs_f32_accum_for_bf16 ? sizeof(float) : jpp.dt_size;

    if (use_bf16_emulation()) io_.init_bf16();

    mov(reg_input, ptr[reg_param + GET_OFF(src)]);
    mov(reg_output, ptr[reg_param + GET_OFF(dst)]);
    if (jpp.alg == pooling_max && (jpp.is_training || jpp.is_backward))
        mov(reg_index, ptr[reg_param + GET_OFF(indices)]);
    mov(reg_kh, ptr[reg_param + GET_OFF(kh_padding)]);
    mov(reg_k_shift, ptr[reg_param + GET_OFF(kh_padding_shift)]);
    mov(reg_ker_area_h, ptr[reg_param + GET_OFF(ker_area_h)]);
    mov(reg_nbc, ptr[reg_param + GET_OFF(ur_bc)]);

    auto process_oi = [&](int ur_w, int ur_bc, int lpad, int rpad,
                              bool with_c_tail_proccessing,
                              bool inc_reg = true) {
        step(ur_w, ur_bc, lpad, rpad, with_c_tail_proccessing);

        if (isa == sse41) {
            if (with_c_tail_proccessing && jpp.c_tail <= (jpp.c_block / 2)) {

                // In nspc format in case of c tail processing if c tail is
                // equal or lower than 4 we don't have to process
                // last high half block, because it doesn't exist
                if (!jpp.is_c_padded) ur_bc -= 1;
                /*
                 * In case of c_tail_processing if c_tail is equal or lower than 4
                 * applying postops never make sense. In case of blocked format it
                 * can cause overwriting zero padding or segfault because the element
                 * corresponding to the piece with padded zeros doesn't exist in binary
                 * postops arg1 tensor (nchw format) in per_oc bcast strategy.
                 */
                disable_postops_when_sse_high_half_processed_
                        = jpp.tag_kind == jit_memory_tag_kind_t::blocked;
            }
            sse_high_half = true;
            step_high_half(ur_w, ur_bc, lpad, rpad, with_c_tail_proccessing);
            sse_high_half = false;
            disable_postops_when_sse_high_half_processed_ = false;
        }

        if (!inc_reg) return;

        auto output_dt_size = jpp.dt_size;
        auto shift = (isa == sse41) ? vlen : 0;
        add(reg_input,
                input_dt_size * nstl::max(0, ur_w * stride_w - lpad)
                                * input_c_off
                        - shift);
        add(reg_output, output_dt_size * ur_w * output_c_off - shift);
        if (jpp.alg == pooling_max && (jpp.is_training || jpp.is_backward)) {
            auto ishift = (isa == sse41) ? jpp.c_block / 2 : 0;
            auto ind_dt_size = types::data_type_size(jpp.ind_dt);
            add(reg_index, (ur_w * output_c_off - ishift) * ind_dt_size);
        }
    };

    auto perform_ker = [&](int ur_bc, bool with_c_tail_processing) {
        prev_kw = 0; // re-initialize this value for avg steps

        if (jpp.is_backward && jpp.simple_alg)
            zero_diff_src(ur_bc, with_c_tail_processing);

        if (jpp.alg == pooling_avg_exclude_padding
                && (!with_c_tail_processing
                        || (!utils::one_of(isa, avx, avx2, avx2_vnni_2)))) {
            // vmm_ker_area_h and vmm_c_tail_mask are stored in one register
            // so when vmm_c_tail_mask is used we need to load vmm_ker_area_h
            // exactly where this information is needed with the
            // vmm_c_tail_mask information being saved first
            uni_broadcast_reg_val(
                    reg_ker_area_h.getIdx(), vmm_ker_area_h.getIdx());
        }

        if (jpp.alg == pooling_avg_include_padding) {
            mov(tmp_gpr, float2int((float)(kw * kh * jpp.kd)));
            uni_vmovq(xmm_tmp, tmp_gpr);
            uni_vpbroadcastd(vmm_tmp, xmm_tmp);
        }

        if (jpp.alg == pooling_max && (jpp.is_training || jpp.is_backward)) {
            if (!with_c_tail_processing
                    || (!utils::one_of(isa, avx, avx2, avx2_vnni_2))) {
                // The same situation as above(vmm_ker_area_h).
                put_one_in_vmm();
            }

            if (utils::one_of(isa, avx, avx2, avx2_vnni_2)) {
                mov(reg_shuf_mask, 0x0c080400);
            }
        }

        const int ur_w = nstl::min(jpp.ow, jpp.ur / jpp.ur_bc);
        const int n_oi_iterations = utils::div_up(ow, ur_w);
        const int ur_stride_w = ur_w * stride_w;
        const int l_pad_iterations
                = nstl::min(n_oi_iterations, utils::div_up(l_pad, ur_stride_w));

        for (int i = 0; i < l_pad_iterations; ++i) {
            const int ow_s = i * ur_w;
            const int ow_e = nstl::min(ow, ow_s + ur_w);
            const int cur_l_pad = l_pad - i * ur_stride_w;
            const int cur_r_pad = nstl::max(
                    0, calculate_end_padding(l_pad, ow_e, iw, stride_w, kw));
            const int cur_ur_w = ow_e - ow_s;
            process_oi(cur_ur_w, ur_bc, cur_l_pad, cur_r_pad,
                    with_c_tail_processing);
        }

        const int rem_n_oi_iters = n_oi_iterations - l_pad_iterations;
        const int cur_iw = l_pad_iterations * ur_stride_w - l_pad;
        const int cur_iw_rightmost_idx = cur_iw + kw - 1;
        const int no_pad_full_n_oi_iters = utils::saturate<int>(
                0, rem_n_oi_iters, (iw - cur_iw_rightmost_idx) / ur_stride_w);

        if (no_pad_full_n_oi_iters > 0) {
            Label ow_loop;
            if (no_pad_full_n_oi_iters > 1) xor_(oi_iter, oi_iter);
            L(ow_loop);
            {
                process_oi(ur_w, ur_bc, 0, 0, with_c_tail_processing);
                if (no_pad_full_n_oi_iters > 1) {
                    inc(oi_iter);
                    cmp(oi_iter, no_pad_full_n_oi_iters);
                    jl(ow_loop, T_NEAR);
                }
            }
        }

        for (int i = l_pad_iterations + no_pad_full_n_oi_iters;
                i < n_oi_iterations; ++i) {
            const int ow_s = i * ur_w;
            const int ow_e = nstl::min(ow, ow_s + ur_w);
            const int cur_r_pad = nstl::max(
                    0, calculate_end_padding(l_pad, ow_e, iw, stride_w, kw));
            const int cur_ur_w = ow_e - ow_s;
            process_oi(cur_ur_w, ur_bc, 0, cur_r_pad, with_c_tail_processing);
        }
    };
    Label ur_bc_tail_label, c_tail_processing_label, finish_label;

    if (jpp.ur_bc_tail > 0) {
        cmp(reg_nbc, jpp.ur_bc);
        jne(ur_bc_tail_label, T_NEAR);
    } else if (jpp.c_tail != 0) {
        // ur_bc contains number of channel blocks to processing
        // b_c contains number of channel blocks already processed
        // If reg_nbc + tmp_gpr == jpp.nb_c then this is
        // information that probably channel tail processing will be needed.
        mov(tmp_gpr, ptr[reg_param + GET_OFF(b_c)]);
        add(tmp_gpr, reg_nbc);
        cmp(tmp_gpr, jpp.nb_c);
        je(c_tail_processing_label, T_NEAR);
    }

    perform_ker(jpp.ur_bc, false);

    if (jpp.ur_bc_tail > 0) {
        jmp(finish_label, T_NEAR);

        // If ur_bc_tail exists then we know that this is
        // last set of blocks to process and we need
        // care of c tail processing if number of channels
        // is not divided by number of channels in block
        L(ur_bc_tail_label);
        if (jpp.c_tail != 0) io_.prepare_tail_mask();
        perform_ker(jpp.ur_bc_tail, jpp.c_tail != 0);

        L(finish_label);
    } else if (jpp.c_tail != 0) {
        jmp(finish_label, T_NEAR);

        L(c_tail_processing_label);
        io_.prepare_tail_mask();
        perform_ker(jpp.ur_bc, true);

        L(finish_label);
    }

    this->postamble();

    if (jpp.with_eltwise && postops_injector_)
        postops_injector_->prepare_table(/* generate = */ true);
    if (f8_e5m2_cvt_) f8_e5m2_cvt_->prepare_table();
    if (f8_e4m3_cvt_) f8_e4m3_cvt_->prepare_table();
    io_.prepare_table_fp8();
}

template struct jit_uni_pool_kernel_t<sse41>;
template struct jit_uni_pool_kernel_t<avx>;
template struct jit_uni_pool_kernel_t<avx2>;
template struct jit_uni_pool_kernel_t<avx2_vnni_2>;
template struct jit_uni_pool_kernel_t<avx512_core>;
template struct jit_uni_pool_kernel_t<avx512_core_fp16>;

} // namespace x64
} // namespace cpu
} // namespace impl
} // namespace dnnl

// vim: et ts=4 sw=4 cindent cino+=l0,\:4,N-s
