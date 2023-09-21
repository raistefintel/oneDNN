/*******************************************************************************
* Copyright 2023 Intel Corporation
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

#include <array>

#include "common/bit_cast.hpp"
#include "common/float16.hpp"
#include "common/float8.hpp"
#include "common/utils.hpp"

namespace dnnl {
namespace impl {

float8_e5m2_t &float8_e5m2_t::operator=(float16_t f) {
    // we just need to apply rounding
    uint16_t fraw = f.raw;
    uint16_t naninf_mask = 0x7c00;

    bool is_special = (fraw & naninf_mask) == naninf_mask;
    bool is_nan = is_special && (fraw & 0x03ff); // one of the lsb is non zero

    // we always set quiet bit for NaN
    if (is_nan) {
        raw_bits_ = (fraw >> 8) | 0x02;
        return *this;
    }

    // if infinity, we just return it as is
    if (is_special) {
        raw_bits_ = fraw >> 8;
        return *this;
    }

    // otherwise we just round and return
    int16_t rounding_nudge = 0x007f + ((fraw & 0x0100) >> 8);
    fraw = fraw + rounding_nudge;
    raw_bits_ = fraw >> 8;
    return *this;
}

float8_e5m2_t &float8_e5m2_t::operator=(float f) {
    float16_t f16 = f;
    float8_e5m2_t f8 = f16;
    raw_bits_ = f8.raw_bits_;
    return *this;
}

float8_e5m2_t::operator float() const {
    std::array<uint8_t, 2> iraw = {{0, raw_bits_}};
    auto f16 = utils::bit_cast<float16_t>(iraw);
    return static_cast<float>(f16);
}

float8_e4m3_t &float8_e4m3_t::operator=(float16_t f) {
    using namespace utils;
    // Here the idea is to add a large constant to the float16_t to force the
    // proper rounding to f8_e4m3 accuracy.
    int fraw = f.raw;

    // first we extract the sign and make the input positive
    unsigned int s8 = (fraw & 0x8000) >> 8;
    fraw = fraw & 0x7fff;

    // we filter out overflow, nan
    // Note: values in [448;464] round to 448, which is representable
    // So we overflow above 464
    if (fraw > 0x5f40) {
        raw_bits_ = s8 | 0x7f;
        return *this;
    }
    // we filter out underflow when f <= 2^-10
    if (fraw <= 0x1400) {
        raw_bits_ = s8;
        return *this;
    }

    // compute the rounding shifter by taking its exponent + 0x1p7
    // Lucky us, it does not overflow as fraw <= 448.
    float16_t shifter = (fraw & 0x7c00) + 0x1c00;
    int is_denorm = shifter < 0x4000; // 0x1.0p1f
    if (is_denorm) shifter = 0x4000; // 0x1.0p1f

    float16_t rounded = (float16_t(fraw) + shifter) - shifter;

    int e8 = ((rounded.raw & 0x7c00) >> 10) - 8;
    uint8_t m8 = (rounded.raw & 0x03ff) >> 7;

    // we need to make the implicit f32 mantissa bit explicit for
    // denorm f8_e4m3
    if (is_denorm) {
        m8 = (m8 | 0x00000008) >> (-e8 + 1);
        e8 = 0;
    }

    raw_bits_ = s8 | (e8 << 3) | m8;
    return *this;
}

float8_e4m3_t &float8_e4m3_t::operator=(float f) {
    using namespace utils;
    // Here the idea is to add a large constant to the float to force the
    // proper rounding to f8_e4m3 accuracy.
    int fraw = float2int(f);

    // first we extract the sign and make the input positive
    unsigned int s8 = (fraw & 0x80000000) >> 24;
    fraw = fraw & 0x7fffffff;

    // we filter out overflow, nan and underflow
    // Note: values in [448;464] round to 448, which is representable
    // So we overflow above 464
    if (fraw > 0x43e80000) {
        raw_bits_ = s8 | 0x7f;
        return *this;
    }
    if (fraw <= 0x3a800000) {
        raw_bits_ = s8;
        return *this;
    }

    // compute the rounding shifter by taking its exponent + 0x1p20
    float shifter = int2float((fraw & 0x7f800000) + 0x0a000000);
    int is_denorm = shifter < 16384.f; //0x1.0p14f
    if (is_denorm) shifter = 16384.f; //0x1.0p14f

    float rounded = (int2float(fraw) + shifter) - shifter;

    int e8 = ((float2int(rounded) & 0x7f800000) >> 23) - 120;
    uint8_t m8 = (float2int(rounded) & 0x007fffff) >> 20;

    // we need to make the implicit f32 mantissa bit explicit for
    // denorm f8_e4m3
    if (is_denorm) {
        m8 = (m8 | 0x00000008) >> (-e8 + 1);
        e8 = 0;
    }

    raw_bits_ = s8 | (e8 << 3) | m8;
    return *this;
}

float8_e4m3_t::operator float() const {
    uint16_t s16 = (raw_bits_ & 0x80) << 8;
    uint16_t e8 = (raw_bits_ & 0x78) >> 3;
    uint16_t e16 = (e8 + 8 /* 15 - 7 = e16_bias - e8_bias */) << 10;
    uint16_t m8 = (raw_bits_ & 0x7);
    uint16_t m16 = m8 << 7;

    // Need to convert f8_e4m3 denormal into f16 normal.
    if (e8 == 0 && m8 != 0) {
        uint16_t count = 2;
        count = m8 > 0x1 ? 1 : count;
        count = m8 > 0x3 ? 0 : count;
        e16 -= count;
        m16 = (m16 << (count + 1) & 0x7);
    } else if (e8 == 0 && m8 == 0) {
        e16 = 0;
    } /* e8 == 0xf && m == 0x7, when? */

    uint16_t u16 = s16 | e16 | m16;
    auto f16 = utils::bit_cast<float16_t>(u16);
    return static_cast<float>(f16);
}

} // namespace impl
} // namespace dnnl