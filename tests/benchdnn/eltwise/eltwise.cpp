/*******************************************************************************
* Copyright 2019-2020 Intel Corporation
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

#include <math.h>
#include <random>
#include <stdio.h>
#include <stdlib.h>

#include "dnnl.h"

#include "tests/test_thread.hpp"

#include "dnnl_common.hpp"
#include "dnnl_memory.hpp"

#include "eltwise/eltwise.hpp"

namespace eltwise {

static int init_pd(dnnl_engine_t engine, const prb_t *p,
        dnnl_primitive_desc_t &epd, res_t *r, dir_t dir,
        const_dnnl_primitive_desc_t hint) {
    dnnl_eltwise_desc_t ed;
    dnnl_memory_desc_t data_d;

    DNN_SAFE(dnnl_memory_desc_init_by_tag(&data_d, p->ndims, p->dims.data(),
                     p->dt, convert_tag(p->tag, p->ndims)),
            WARN);

    dnnl_alg_kind_t alg = attr_t::post_ops_t::kind2dnnl_kind(p->alg);

    if (p->dir & FLAG_FWD) {
        auto prop = p->dir & FLAG_INF ? dnnl_forward_inference
                                      : dnnl_forward_training;

        DNN_SAFE(dnnl_eltwise_forward_desc_init(
                         &ed, prop, alg, &data_d, p->alpha, p->beta),
                WARN);
    } else {
        dnnl_memory_desc_t diff_data_d;
        DNN_SAFE(dnnl_memory_desc_init_by_tag(&diff_data_d, p->ndims,
                         p->dims.data(), p->dt, dnnl_format_tag_any),
                WARN);
        DNN_SAFE(dnnl_eltwise_backward_desc_init(
                         &ed, alg, &diff_data_d, &data_d, p->alpha, p->beta),
                WARN);
    }

    auto dnnl_attr = create_dnnl_attr(p->attr);

    dnnl_status_t init_status
            = dnnl_primitive_desc_create(&epd, &ed, dnnl_attr, engine, NULL);

    dnnl_primitive_attr_destroy(dnnl_attr);

    if (init_status == dnnl_unimplemented)
        return r->state = UNIMPLEMENTED, OK;
    else
        SAFE(init_status, WARN);

    r->impl_name = query_impl_info(epd);
    if (maybe_skip(r->impl_name)) {
        BENCHDNN_PRINT(2, "SKIPPED: oneDNN implementation: %s\n",
                r->impl_name.c_str());
        DNN_SAFE(dnnl_primitive_desc_destroy(epd), WARN);
        return r->state = SKIPPED, r->reason = SKIP_IMPL_HIT, OK;
    } else {
        BENCHDNN_PRINT(5, "oneDNN implementation: %s\n", r->impl_name.c_str());
    }

    return OK;
}

// Check that on a given input specific alg may return NaN or inf.
// Used in other drivers supporting eltwise post_ops.
bool check_extreme_values(const float &a, const float &b, alg_t alg) {
    switch (alg) {
        case alg_t::EXP:
        case alg_t::EXP_DST:
        case alg_t::LOG:
        case alg_t::POW:
        case alg_t::SQRT:
        case alg_t::SQRT_DST:
            if (std::isnan(a) && std::isnan(b)) return true;
            if (std::isinf(a) && std::isinf(b)
                    && std::signbit(a) == std::signbit(b))
                return true;
        default: break;
    }
    return false;
}

static bool check_abs_err(const prb_t *p, const float &s, const float &trh) {
    const float approx_machine_eps = 2 * epsilon_dt(dnnl_f32);
    const float comp_err = approx_machine_eps / trh;

    switch (p->alg) {
        case alg_t::ELU:
        case alg_t::ELU_DST:
            // catch catastrophic cancellation when (exp(s) - 1), s < 0 and
            // s is close to zero.
            return (p->dir & FLAG_FWD) && std::signbit(s)
                    && (fabsf(expf(s) - 1.f) <= comp_err);
        case alg_t::GELU_TANH: {
            // catch catastrophic cancellation
            // (4.f is magic scale for f32)
            const float sqrt_2_over_pi = 0.797884;
            const float fitting_const = 0.044715;
            float v = tanhf(sqrt_2_over_pi * s * (1 + fitting_const * s * s));
            float dg = sqrt_2_over_pi * (1 + 3 * fitting_const * s * s);
            if (fabsf(1.f + v) <= comp_err) return true;
            return (p->dir & FLAG_BWD) && std::signbit(s)
                    && fabsf(1.f + s * (1.f - v) * dg) <= 4.f * comp_err;
        }
        case alg_t::GELU_ERF: {
            // catch catastrophic cancellation
            // which occurs at large negative s
            const float sqrt_2_over_2 = 0.707106769084930419921875f;
            const float two_over_sqrt_pi = 1.12837922573089599609375f;
            float v = s * sqrt_2_over_2;
            if (p->dir & FLAG_FWD)
                return fabsf(1.f + erff(v)) <= comp_err;
            else
                return fabsf(1.f + erff(v)
                               + v * two_over_sqrt_pi * expf(-v * v))
                        <= comp_err;
        }
        case alg_t::TANH:
            // catch catastrophic cancellation, which occurs when err in tanh(s)
            // is high and tanh(s) is close to 1.
            return (p->dir & FLAG_BWD) && (1.f - tanhf(fabsf(s))) <= comp_err;
        case alg_t::TANH_DST: // sse41 can't do fma
            // catch catastrophic cancellation, which occurs when err in tanh(s)
            // is high and tanh(s) is close to 1.
            return (p->dir & FLAG_BWD) && (1.f - s * s) <= comp_err;
        case alg_t::SRELU:
            // when s is negative, expf(s) -> 0 rapidly
            // which leads to log1pf(expf(s)) -> 0
            // which leads to high relative error,
            // while abs error is still low.
            // (10.f is magic scale for bf16)
            return (p->dir & FLAG_FWD) && std::signbit(s)
                    && log1pf(expf(s)) <= 10.f * comp_err;
        case alg_t::LOGISTIC:
            // when s >= 4, logistic(s) -> 0 rapidly, which leads to high
            // relative error of logistic(s) * (1 - logistic(s)) due to
            // catastrohic cancellation.
            return (p->dir & FLAG_BWD) && !std::signbit(s)
                    && (1.f / (1.f + expf(s))) <= comp_err;
        case alg_t::SWISH:
            // catch cancellation happening when W(s) ~~ -1 in (1 + W(s))
            // formula part on backward.
            return (p->dir & FLAG_BWD)
                    && (p->alpha * s * (1.f - 1.f / (1.f + expf(-p->alpha * s)))
                            <= comp_err);
        default: return false;
    }
}

static int compare(const prb_t *p, const dnn_mem_t &mem_arg_fp,
        const dnn_mem_t &mem_fp, const dnn_mem_t &mem_dt, res_t *r) {
    const bool is_fwd = p->dir & FLAG_FWD;

    const auto nelems = mem_dt.nelems();
    if (nelems == 0) return r->state = PASSED, OK;

    r->total = nelems;

    // Tolerate only rounding error (1 ulp) for other than fp32 precisions.
    float trh = epsilon_dt(p->dt);
    if (p->dt == dnnl_f32) {
        // Tolerate bigger compute errors for complex algorithms.
        if (p->alg == alg_t::GELU_TANH || p->alg == alg_t::ELU
                || p->alg == alg_t::SWISH || p->alg == alg_t::TANH
                || p->alg == alg_t::SRELU || p->alg == alg_t::LOG
                || (is_fwd && p->alg == alg_t::ELU_DST)
                || (is_fwd && p->alg == alg_t::TANH_DST))
            trh = 4e-5;
        else
            trh = 4e-6;
    }

    for (int64_t i = 0; i < nelems; i++) {
        const float dt = mem_dt.get_elem(i);
        const float src = mem_arg_fp.get_elem(i);
        const float fp0 = mem_fp.get_elem(i);
        const float fp = round_to_nearest_representable(p->dt, fp0);

        const float diff = fabsf(fp - dt);
        const float rel_diff = diff / (fabsf(fp) > FLT_MIN ? fabsf(fp) : 1);

        bool ok = (fabsf(fp) > 1e-5 ? rel_diff : diff) <= trh;

        if (!ok) ok = check_extreme_values(fp, dt, p->alg);

        if (!ok && check_abs_err(p, src, trh)) ok = diff <= trh;

        r->errors += !ok;

        const bool dump = false || (!ok && (r->errors < 10 || verbose >= 10))
                || (verbose >= 50 && i < 30) || (verbose >= 99);
        if (dump) {
            std::stringstream ss;
            dims_t dims_idx = off2dims_idx(p->dims, i);
            ss << dims_idx;
            std::string ind_str = ss.str();

            BENCHDNN_PRINT(0,
                    "[%4ld][%s] src:% 9.6g fp0:% 9.6g fp:% 9.6g dt:% 9.6g "
                    "diff:%8.3g rdiff:%8.3g\n",
                    (long)i, ind_str.c_str(), src, fp0, fp, dt, diff, rel_diff);
        }
    }

    if (r->errors) r->state = FAILED;

    if (r->state == UNTESTED) r->state = PASSED; /* optimism */

    return r->state == FAILED ? FAIL : OK;
}

int fill_data(const prb_t *p, data_kind_t kind, dnn_mem_t &mem_dt,
        dnn_mem_t &mem_fp) {
    const auto nelems = mem_fp.nelems();
    if (nelems == 0) return OK;

    /* Do fixed partitioning to have same filling for any number of threads */
    const int64_t n_chunks = 16;
    const int64_t chunk_size = div_up(nelems, n_chunks);

    dnnl::impl::parallel_nd(n_chunks, [&](int idx_chunk) {
        int64_t idx_start = idx_chunk * chunk_size;
        int64_t idx_end = MIN2(idx_start + chunk_size, nelems);
        // Note 1: we use a different seed for each chunk to avoid
        // repeating patterns. We could use discard(idx_start) too but
        // we avoid it for two reasons:
        //   a. it has a complexity in O(idx_start).
        //   b. igen and fgen below might require more than 1 sample
        //   per idx, so the we cannot deterministically compute the
        //   number of states we need to discard
        // Note 2: We also advance the state to avoid having only
        // small values as first chunk input.  The +1 is necessary to
        // avoid generating zeros in first chunk.
        // Note 3: we multiply by kind + 1 to have different values in
        // src/dst and diff_dst. The +1 is to avoid 0 again.
        std::minstd_rand msr((idx_start + 1) * (kind + 1));
        msr.discard(1);
        std::uniform_int_distribution<> igen(0, 10);
        // TODO: 0.09 due to log impl doesn't give good accuracy in 0.99 points
        std::uniform_real_distribution<> fgen(0.f, 0.09f);

        for (int64_t idx = idx_start; idx < idx_end; ++idx) {
            float value = FLT_MAX;
            switch (idx % 8) {
                case 0: value = (float)igen(msr); break; // [0-10] pos
                case 1: value = -(float)igen(msr); break; // [0-10] neg
                case 2: value = fgen(msr); break; // [0.-0.1) pos
                case 3: value = -fgen(msr); break; // [0.-0.1) neg
                case 4: value = 10 * (float)igen(msr); break; // [0-100] pos
                case 5: value = -10 * (float)igen(msr); break; // [0-100] neg
                case 6: value = 10.f * fgen(msr); break; // [0.-1.) pos
                case 7: value = -10.f * fgen(msr); break; // [0.-1.) neg
            }
            value = round_to_nearest_representable(p->dt, value);

            // Hack: -0 may lead to different sign in the answer since input
            // passes through simple reorder which converts -0 into +0.
            if (value == -0.f) value = 0.f;

            mem_fp.set_elem(idx, value);
        }
    });

    SAFE(mem_dt.reorder(mem_fp), WARN);

    return OK;
}

void check_known_skipped_case(const prb_t *p, res_t *r) {
    check_known_skipped_case_common({p->dt}, p->dir, r);
    if (r->state == SKIPPED) return;

    bool is_invalid = false;
    switch (p->alg) {
        case alg_t::CLIP: is_invalid = p->beta < p->alpha; break;
        case alg_t::BRELU:
        case alg_t::ELU_DST:
        case alg_t::RELU_DST: is_invalid = p->alpha < 0; break;
        case alg_t::ROUND:
            is_invalid = p->dt != dnnl_f32 || p->dir & FLAG_BWD;
            break;
        default: break;
    };
    if (is_invalid) r->state = SKIPPED, r->reason = INVALID_CASE;
}

int doit(const prb_t *p, res_t *r) {
    if (bench_mode == LIST) return r->state = LISTED, OK;

    check_known_skipped_case(p, r);
    if (r->state == SKIPPED) return OK;

    dnnl_primitive_t e {};
    SAFE(init_prim(&e, init_pd, p, r), WARN);
    if (r->state == SKIPPED || r->state == UNIMPLEMENTED) return OK;

    const_dnnl_primitive_desc_t const_pd;
    DNN_SAFE(dnnl_primitive_get_primitive_desc(e, &const_pd), CRIT);

    if (dnn_mem_t::check_mem_size(const_pd) != OK) {
        DNN_SAFE_V(dnnl_primitive_destroy(e));
        return r->state = SKIPPED, r->reason = NOT_ENOUGH_RAM, OK;
    }

    const auto q = [&](int index = 0) -> const dnnl_memory_desc_t & {
        return *dnnl_primitive_desc_query_md(
                const_pd, dnnl_query_exec_arg_md, index);
    };

    const auto &data_md = q(DNNL_ARG_SRC);
    const auto &scratchpad_md = q(DNNL_ARG_SCRATCHPAD);

    const auto fp = dnnl_f32;
    const auto tag = get_abx_tag(p->ndims);

    const auto &test_engine = get_test_engine();

    dnn_mem_t src_fp(data_md, fp, tag, test_engine);
    dnn_mem_t src_dt(data_md, test_engine);

    // we need src_fp for proper comparison, => no in-place reference
    dnn_mem_t dst_fp(data_md, fp, tag, test_engine);
    dnn_mem_t placeholder_dst_dt;
    if (!p->inplace) { placeholder_dst_dt = dnn_mem_t(data_md, test_engine); }
    dnn_mem_t &dst_dt = p->inplace ? src_dt : placeholder_dst_dt;

    dnn_mem_t scratchpad_dt(scratchpad_md, test_engine);

    dnn_mem_t d_dst_dt, placeholder_d_src_dt;

    SAFE(fill_data(p, SRC, src_dt, src_fp), WARN);

    args_t args;

    if (p->dir & FLAG_FWD) {
        args.set(DNNL_ARG_SRC, src_dt);
        args.set(DNNL_ARG_DST, dst_dt);
        args.set(DNNL_ARG_SCRATCHPAD, scratchpad_dt);

        SAFE(execute_and_wait(e, args), WARN);

        if (bench_mode & CORR) {
            compute_ref_fwd(p, src_fp, dst_fp);
            dnn_mem_t dst(dst_dt, fp, tag, test_engine);
            SAFE(compare(p, src_fp, dst_fp, dst, r), WARN);
        }
    } else {
        const auto &d_data_md = q(DNNL_ARG_DIFF_DST);

        dnn_mem_t d_dst_fp = dnn_mem_t(d_data_md, fp, tag, test_engine);
        d_dst_dt = dnn_mem_t(d_data_md, test_engine);

        dnn_mem_t &d_src_fp = d_dst_fp; // in-place reference
        if (!p->inplace) {
            placeholder_d_src_dt = dnn_mem_t(d_data_md, test_engine);
        }
        dnn_mem_t &d_src_dt = p->inplace ? d_dst_dt : placeholder_d_src_dt;

        SAFE(fill_data(p, DST, d_dst_dt, d_dst_fp), WARN);

        args.set(DNNL_ARG_DIFF_DST, d_dst_dt);
        args.set(DNNL_ARG_DIFF_SRC, d_src_dt);
        args.set(DNNL_ARG_SCRATCHPAD, scratchpad_dt);

        if (p->use_dst()) {
            if (bench_mode & CORR) compute_ref_fwd(p, src_fp, dst_fp);
            SAFE(dst_dt.reorder(dst_fp), WARN);
            // make dst_fp of same values as for bf16, otherwise there are high
            // relative and absolute errors due to initial difference in source
            // values which become worse particularly when (1 - x) is used.
            SAFE(dst_fp.reorder(dst_dt), WARN);
            args.set(DNNL_ARG_DST, dst_dt);
        } else {
            args.set(DNNL_ARG_SRC, src_dt);
        }
        SAFE(execute_and_wait(e, args), WARN);

        if (bench_mode & CORR) {
            dnn_mem_t &arg_fp = p->use_dst() ? dst_fp : src_fp;
            compute_ref_bwd(p, arg_fp, d_dst_fp, d_src_fp);
            dnn_mem_t d_src(d_src_dt, fp, tag, test_engine);
            SAFE(compare(p, arg_fp, d_src_fp, d_src, r), WARN);
        }
    }

    measure_perf(r->timer, e, args);

    DNN_SAFE_V(dnnl_primitive_destroy(e));

    return OK;
}

} // namespace eltwise
