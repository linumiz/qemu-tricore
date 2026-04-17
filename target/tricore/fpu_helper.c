/*
 *  TriCore emulation for qemu: fpu helper.
 *
 *  Copyright (c) 2016 Bastian Koppelmann University of Paderborn
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "fpu/softfloat.h"
#include "qemu/log.h"

#define QUIET_NAN 0x7fc00000
#define ADD_NAN   0x7fc00001
#define SQRT_NAN  0x7fc00004
#define DIV_NAN   0x7fc00008
#define MUL_NAN   0x7fc00002

#define DQUIET_NAN 0x7ff8000000000000ULL
#define DADD_NAN   0x7ff8000000000001ULL
#define DSQRT_NAN  0x7ff8000000000004ULL
#define DDIV_NAN   0x7ff8000000000008ULL
#define DMUL_NAN   0x7ff8000000000002ULL

#define FPU_FS PSW_USB_C
#define FPU_FI PSW_USB_V
#define FPU_FV PSW_USB_SV
#define FPU_FZ PSW_USB_AV
#define FPU_FU PSW_USB_SAV

#define float32_sqrt_nan make_float32(SQRT_NAN)
#define float32_quiet_nan make_float32(QUIET_NAN)

#ifndef TC_1_8_SUPPORT
#define TC_1_8_SUPPORT
#endif

#ifndef TC_1_8_DEBUG
//#define TC_1_8_DEBUG
#endif


static inline uint32_t extractF32Frac(float32 a) 
{ 
    return a & 0x007FFFFF; 
};

static inline int32_t extractF32Exp(float32 a) 
{ 
    return (a >> 23) & 0xFF; 
};

static inline void normalizeFloat64Subnormal(uint64_t aSig,int16_t * zExpPtr, uint64_t * zSigPtr)
{
    int8_t shiftCount;
    shiftCount = (int8_t)(__builtin_clzll(aSig) - 11);
    *zSigPtr = aSig << shiftCount;
    *zExpPtr = 1 - shiftCount;

}

/* we don't care about input_denormal */
static inline uint8_t f_get_excp_flags(CPUTriCoreState *env)
{
    return get_float_exception_flags(&env->fp_status)
           & (float_flag_invalid
              | float_flag_overflow
              | float_flag_underflow
              | float_flag_output_denormal_flushed
              | float_flag_divbyzero
              | float_flag_inexact);
}

#ifdef TC_1_8_SUPPORT
static inline void tricore_sfmode(CPUTriCoreState *env)
{
	set_flush_inputs_to_zero(1, &env->fp_status);
    set_flush_to_zero(1, &env->fp_status);
    set_default_nan_mode(1, &env->fp_status);
}

static inline void tricore_dfmode(CPUTriCoreState *env)
{
	//supports denorm
	set_flush_inputs_to_zero(0, &env->fp_status);
    set_flush_to_zero(0, &env->fp_status);
    set_default_nan_mode(1, &env->fp_status);
}
#endif

static inline float32 f_maddsub_nan_result(float32 arg1, float32 arg2,
                                           float32 arg3, float32 result,
                                           uint32_t muladd_negate_c)
{
    uint32_t aSign, bSign, cSign;
    uint32_t aExp, bExp, cExp;

    if (float32_is_any_nan(arg1) || float32_is_any_nan(arg2) ||
        float32_is_any_nan(arg3)) {
        return QUIET_NAN;
    } else if (float32_is_infinity(arg1) && float32_is_zero(arg2)) {
        return MUL_NAN;
    } else if (float32_is_zero(arg1) && float32_is_infinity(arg2)) {
        return MUL_NAN;
    } else {
        aSign = arg1 >> 31;
        bSign = arg2 >> 31;
        cSign = arg3 >> 31;

        aExp = (arg1 >> 23) & 0xff;
        bExp = (arg2 >> 23) & 0xff;
        cExp = (arg3 >> 23) & 0xff;

        if (muladd_negate_c) {
            cSign ^= 1;
        }
        if (((aExp == 0xff) || (bExp == 0xff)) && (cExp == 0xff)) {
            if (aSign ^ bSign ^ cSign) {
                return ADD_NAN;
            }
        }
    }

    return result;
}

static inline float64 d_maddsub_nan_result(float64 arg1, float64 arg2,
                                           float64 arg3, float64 result,
                                           uint32_t muladd_negate_c)
{
    uint64_t aSign, bSign, cSign;
    uint64_t aExp, bExp, cExp;

    if (float64_is_any_nan(arg1) || float64_is_any_nan(arg2) ||
        float64_is_any_nan(arg3)) {
        return DQUIET_NAN;
    } else if (float64_is_infinity(arg1) && float64_is_zero(arg2)) {
        return DMUL_NAN;
    } else if (float64_is_zero(arg1) && float64_is_infinity(arg2)) {
        return DMUL_NAN;
    } else {
        aSign = arg1 >> 63;
        bSign = arg2 >> 63;
        cSign = arg3 >> 63;

        aExp = (arg1 >> 52) & 0x7ff;
        bExp = (arg2 >> 52) & 0x7ff;
        cExp = (arg3 >> 52) & 0x7ff;

        if (muladd_negate_c) {
            cSign ^= 1;
        }
        if (((aExp == 0x7ff) || (bExp == 0x7ff)) && (cExp == 0x7ff)) {
            if (aSign ^ bSign ^ cSign) {
                return DADD_NAN;
            }
        }
    }

    return result;
}

static void f_update_psw_flags(CPUTriCoreState *env, uint8_t flags)
{
    uint8_t some_excp = 0;
    set_float_exception_flags(0, &env->fp_status);

    if (flags & float_flag_invalid) {
        env->FPU_FI = 1 << 31;
        some_excp = 1;
    }

    if (flags & float_flag_overflow) {
        env->FPU_FV = 1 << 31;
        some_excp = 1;
    }

    if (flags & float_flag_underflow || flags & float_flag_output_denormal_flushed) {
        env->FPU_FU = 1 << 31;
        some_excp = 1;
    }

    if (flags & float_flag_divbyzero) {
        env->FPU_FZ = 1 << 31;
        some_excp = 1;
    }

    if (flags & float_flag_inexact || flags & float_flag_output_denormal_flushed) {
        env->PSW |= 1 << 26;
        some_excp = 1;
    }

    env->FPU_FS = some_excp;
}

#define FADD_SUB(op)                                                           \
uint32_t helper_f##op(CPUTriCoreState *env, uint32_t r1, uint32_t r2)          \
{                                                                              \
    float32 arg1 = make_float32(r1);                                           \
    float32 arg2 = make_float32(r2);                                           \
    uint32_t flags;                                                            \
    float32 f_result;                                                          \
    tricore_sfmode(env);                                                                           \
    f_result = float32_##op(arg2, arg1, &env->fp_status);                      \
    flags = f_get_excp_flags(env);                                             \
    if (flags) {                                                               \
        /* If the output is a NaN, but the inputs aren't,                      \
           we return a unique value.  */                                       \
        if ((flags & float_flag_invalid)                                       \
            && !float32_is_any_nan(arg1)                                       \
            && !float32_is_any_nan(arg2)) {                                    \
            f_result = ADD_NAN;                                                \
        }                                                                      \
        f_update_psw_flags(env, flags);                                        \
    } else {                                                                   \
        env->FPU_FS = 0;                                                       \
    }                                                                          \
    return (uint32_t)f_result;                                                 \
}
FADD_SUB(add)
FADD_SUB(sub)

uint32_t helper_fmul(CPUTriCoreState *env, uint32_t r1, uint32_t r2)
{
    uint32_t flags;
    float32 arg1 = make_float32(r1);
    float32 arg2 = make_float32(r2);
    float32 f_result;

    tricore_sfmode(env);
    f_result = float32_mul(arg1, arg2, &env->fp_status);
    flags    = f_get_excp_flags(env);
    if (flags) {
        /* If the output is a NaN, but the inputs aren't,
           we return a unique value.  */
        if ((flags & float_flag_invalid)
            && !float32_is_any_nan(arg1)
            && !float32_is_any_nan(arg2)) {
                f_result = MUL_NAN;
        }
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'mul.f' executed\n");
#endif
    return (uint32_t)f_result;
}

/*
 * Target TriCore QSEED.F significand Lookup Table
 *
 * The QSEED.F output significand depends on the least-significant
 * exponent bit and the 6 most-significant significand bits.
 *
 * IEEE 754 float datatype
 * partitioned into Sign (S), Exponent (E) and Significand (M):
 *
 * S   E E E E E E E E   M M M M M M ...
 *    |             |               |
 *    +------+------+-------+-------+
 *           |              |
 *          for        lookup table
 *      calculating     index for
 *        output E       output M
 *
 * This lookup table was extracted by analyzing QSEED output
 * from the real hardware
 */
static const uint8_t target_qseed_significand_table[128] = {
    253, 252, 245, 244, 239, 238, 231, 230, 225, 224, 217, 216,
    211, 210, 205, 204, 201, 200, 195, 194, 189, 188, 185, 184,
    179, 178, 175, 174, 169, 168, 165, 164, 161, 160, 157, 156,
    153, 152, 149, 148, 145, 144, 141, 140, 137, 136, 133, 132,
    131, 130, 127, 126, 123, 122, 121, 120, 117, 116, 115, 114,
    111, 110, 109, 108, 103, 102, 99, 98, 93, 92, 89, 88, 83,
    82, 79, 78, 75, 74, 71, 70, 67, 66, 63, 62, 59, 58, 55,
    54, 53, 52, 49, 48, 45, 44, 43, 42, 39, 38, 37, 36, 33,
    32, 31, 30, 27, 26, 25, 24, 23, 22, 19, 18, 17, 16, 15,
    14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2
};

uint32_t helper_qseed(CPUTriCoreState *env, uint32_t r1)
{
    uint32_t arg1, S, E, M, E_minus_one, m_idx;
    uint32_t new_E, new_M, new_S, result;

    arg1 = make_float32(r1);

    /* fetch IEEE-754 fields S, E and the uppermost 6-bit of M */
    S = extract32(arg1, 31, 1);
    E = extract32(arg1, 23, 8);
    M = extract32(arg1, 17, 6);

    if (float32_is_any_nan(arg1)) {
        result = float32_quiet_nan;
    } else if (float32_is_zero_or_denormal(arg1)) {
        if (float32_is_neg(arg1)) {
            result = float32_infinity | (1 << 31);
        } else {
            result = float32_infinity;
        }
    } else if (float32_is_neg(arg1)) {
        result = float32_sqrt_nan;
    } else if (float32_is_infinity(arg1)) {
        result = float32_zero;
    } else {
        E_minus_one = E - 1;
        m_idx = ((E_minus_one & 1) << 6) | M;
        new_S = S;
        new_E = 0xBD - E_minus_one / 2;
        new_M = target_qseed_significand_table[m_idx];

        result = 0;
        result = deposit32(result, 31, 1, new_S);
        result = deposit32(result, 23, 8, new_E);
        result = deposit32(result, 15, 8, new_M);
    }

    if (float32_is_signaling_nan(arg1, &env->fp_status)
        || result == float32_sqrt_nan) {
        env->FPU_FI = 1 << 31;
        env->FPU_FS = 1;
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'qseed.f' executed\n");
#endif
    return (uint32_t) result;
}

uint32_t helper_fdiv(CPUTriCoreState *env, uint32_t r1, uint32_t r2)
{
    uint32_t flags;
    float32 arg1 = make_float32(r1);
    float32 arg2 = make_float32(r2);
    float32 f_result;

    f_result = float32_div(arg1, arg2 , &env->fp_status);

    flags = f_get_excp_flags(env);
    if (flags) {
        /* If the output is a NaN, but the inputs aren't,
           we return a unique value.  */
        if ((flags & float_flag_invalid)
            && !float32_is_any_nan(arg1)
            && !float32_is_any_nan(arg2)) {
                f_result = DIV_NAN;
        }
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'div.f' executed\n");
#endif
    return (uint32_t)f_result;
}

uint32_t helper_fmadd(CPUTriCoreState *env, uint32_t r1,
                      uint32_t r2, uint32_t r3)
{
    uint32_t flags;
    float32 arg1 = make_float32(r1);
    float32 arg2 = make_float32(r2);
    float32 arg3 = make_float32(r3);
    float32 f_result;

    f_result = float32_muladd(arg1, arg2, arg3, 0, &env->fp_status);
    flags    = f_get_excp_flags(env);
    if (flags) {
        if (flags & float_flag_invalid) {
            arg1 = float32_squash_input_denormal(arg1, &env->fp_status);
            arg2 = float32_squash_input_denormal(arg2, &env->fp_status);
            arg3 = float32_squash_input_denormal(arg3, &env->fp_status);
            f_result = f_maddsub_nan_result(arg1, arg2, arg3, f_result, 0);
        }
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'madd.f' executed\n");
#endif
    return (uint32_t)f_result;
}

uint32_t helper_fmsub(CPUTriCoreState *env, uint32_t r1,
                      uint32_t r2, uint32_t r3)
{
    uint32_t flags;
    float32 arg1 = make_float32(r1);
    float32 arg2 = make_float32(r2);
    float32 arg3 = make_float32(r3);
    float32 f_result;

    f_result = float32_muladd(arg1, arg2, arg3, float_muladd_negate_product, &env->fp_status);
    flags    = f_get_excp_flags(env);
    if (flags) {
        if (flags & float_flag_invalid) {
            arg1 = float32_squash_input_denormal(arg1, &env->fp_status);
            arg2 = float32_squash_input_denormal(arg2, &env->fp_status);
            arg3 = float32_squash_input_denormal(arg3, &env->fp_status);

            f_result = f_maddsub_nan_result(arg1, arg2, arg3, f_result, 1);
        }
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'msub.f' executed\n");
#endif
    return (uint32_t)f_result;
}

uint32_t helper_fcmp(CPUTriCoreState *env, uint32_t r1, uint32_t r2)
{
    uint32_t result, flags;
    float32 arg1 = make_float32(r1);
    float32 arg2 = make_float32(r2);

    set_flush_inputs_to_zero(0, &env->fp_status);

    result = 1 << (float32_compare_quiet(arg1, arg2, &env->fp_status) + 1);
    result |= float32_is_denormal(arg1) << 4;
    result |= float32_is_denormal(arg2) << 5;
    flags  = f_get_excp_flags(env);
    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
    set_flush_inputs_to_zero(1, &env->fp_status);
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'cmp.f' executed\n");
#endif
    return result;
}

uint32_t helper_ftoi(CPUTriCoreState *env, uint32_t arg)
{
    float32 f_arg = make_float32(arg);
    int32_t result, flags;
    result = float32_to_int32(f_arg, &env->fp_status);
    flags  = f_get_excp_flags(env);
    if (flags) {
        if (float32_is_any_nan(f_arg)) {
            result = 0;
        }
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'ftoi' executed\n");
#endif
    return (uint32_t)result;
}

uint32_t helper_hptof(CPUTriCoreState *env, uint32_t arg)
{
    float16 f_arg   = make_float16(arg);
    uint32_t result = 0;
    int32_t flags   = 0;

    /*
     * if we have any NAN we need to move the top 2 and lower 8 input mantissa
     * bits to the top 2 and lower 8 output mantissa bits respectively.
     * Softfloat on the other hand uses the top 10 mantissa bits.
     */
    if (float16_is_any_nan(f_arg)) {
        if (float16_is_signaling_nan(f_arg, &env->fp_status)) {
            flags |= float_flag_invalid;
        }
        result = 0;
        result = float32_set_sign(result, f_arg >> 15);
        result = deposit32(result, 23, 8, 0xff);
        result = deposit32(result, 21, 2, extract32(f_arg, 8, 2));
        result = deposit32(result, 0, 8, extract32(f_arg, 0, 8));
    } else {
        set_flush_inputs_to_zero(0, &env->fp_status);
        result = float16_to_float32(f_arg, true, &env->fp_status);
        set_flush_inputs_to_zero(1, &env->fp_status);
        flags = f_get_excp_flags(env);
    }

    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'hptof' executed\n");
#endif
    return result;
}

uint32_t helper_ftohp(CPUTriCoreState *env, uint32_t arg)
{
    float32 f_arg = make_float32(arg);
    uint32_t result = 0;
    int32_t flags = 0;

    /*
     * if we have any NAN we need to move the top 2 and lower 8 input mantissa
     * bits to the top 2 and lower 8 output mantissa bits respectively.
     * Softfloat on the other hand uses the top 10 mantissa bits.
     */
    if (float32_is_any_nan(f_arg)) 
    {
        if (float32_is_signaling_nan(f_arg, &env->fp_status)) 
        {
            flags |= float_flag_invalid;
        }
        result = float16_set_sign(result, arg >> 31);
        result = deposit32(result, 10, 5, 0x1f);
        result = deposit32(result, 8, 2, extract32(arg, 21, 2));
        result = deposit32(result, 0, 8, extract32(arg, 0, 8));
        if (extract32(result, 0, 10) == 0) {
            result |= (1 << 8);
        }
    } else 
    {
        set_flush_to_zero(0, &env->fp_status);
        result = float32_to_float16(f_arg, true, &env->fp_status);
        set_flush_to_zero(1, &env->fp_status);
        flags = f_get_excp_flags(env);
    }
    if (flags) 
    {
        f_update_psw_flags(env, flags);
    } 
    else 
    {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'ftohp' executed\n");
#endif
    return result;
}

uint32_t helper_itof(CPUTriCoreState *env, uint32_t arg)
{
    float32 f_result;
    uint32_t flags;
    f_result = int32_to_float32(arg, &env->fp_status);
    flags    = f_get_excp_flags(env);

    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'itof' executed\n");
#endif
    return (uint32_t)f_result;
}

uint32_t helper_utof(CPUTriCoreState *env, uint32_t arg)
{
    float32 f_result;
    uint32_t flags;

    f_result = uint32_to_float32(arg, &env->fp_status);
    flags    = f_get_excp_flags(env);

    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'utof' executed\n");
#endif
    return (uint32_t)f_result;
}

uint32_t helper_ftoiz(CPUTriCoreState *env, uint32_t arg)
{
    float32 f_arg = make_float32(arg);
    uint32_t result;
    int32_t flags;

    result = float32_to_int32_round_to_zero(f_arg, &env->fp_status);
    flags  = f_get_excp_flags(env);

    if (flags & float_flag_invalid) 
    {
        flags &= ~float_flag_inexact;
        if (float32_is_any_nan(f_arg)) {
            result = 0;
        }
    }
    if (flags) 
    {
        f_update_psw_flags(env, flags);
    } 
    else 
    {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'ftoiz' executed\n");
#endif
    return result;
}

uint32_t helper_ftou(CPUTriCoreState *env, uint32_t arg)
{
    float32 f_arg = make_float32(arg);
    uint32_t result;
    int32_t flags = 0;

    result = float32_to_uint32(f_arg, &env->fp_status);
    flags  = f_get_excp_flags(env);

    if (flags & float_flag_invalid) 
    {
        flags &= ~float_flag_inexact;
        if (float32_is_any_nan(f_arg)) 
        {
            result = 0;
        }
    /*
     * we need to check arg < 0.0 before rounding as TriCore needs to raise
     * float_flag_invalid as well. For instance, when we have a negative
     * exponent and sign, softfloat would only raise float_flat_inexact.
     */
    } else if (float32_lt_quiet(f_arg, 0, &env->fp_status)) 
    {
        flags = float_flag_invalid;
        result = 0;
    }
    if (flags) 
    {
        f_update_psw_flags(env, flags);
    } 
    else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'ftou' executed\n");
#endif
    return result;
}

uint32_t helper_ftouz(CPUTriCoreState *env, uint32_t arg)
{
    float32 f_arg = make_float32(arg);
    uint32_t result;
    int32_t flags;

    result = float32_to_uint32_round_to_zero(f_arg, &env->fp_status);
    flags  = f_get_excp_flags(env);

    if (flags & float_flag_invalid) 
    {
        flags &= ~float_flag_inexact;
        if (float32_is_any_nan(f_arg)) {
            result = 0;
        }
    /*
     * we need to check arg < 0.0 before rounding as TriCore needs to raise
     * float_flag_invalid as well. For instance, when we have a negative
     * exponent and sign, softfloat would only raise float_flat_inexact.
     */
    } else if (float32_lt_quiet(f_arg, 0, &env->fp_status)) {
        flags = float_flag_invalid;
        result = 0;
    }

    if (flags) {
        f_update_psw_flags(env, flags);
    } 
    else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'ftouz' executed\n");
#endif
    return result;
}

void helper_updfl(CPUTriCoreState *env, uint32_t arg)
{
    env->FPU_FS =  extract32(arg, 7, 1) & extract32(arg, 15, 1);
    env->FPU_FI = (extract32(arg, 6, 1) & extract32(arg, 14, 1)) << 31;
    env->FPU_FV = (extract32(arg, 5, 1) & extract32(arg, 13, 1)) << 31;
    env->FPU_FZ = (extract32(arg, 4, 1) & extract32(arg, 12, 1)) << 31;
    env->FPU_FU = (extract32(arg, 3, 1) & extract32(arg, 11, 1)) << 31;
    /* clear FX and RM */
    env->PSW &= ~(extract32(arg, 10, 1) << 26);
    env->PSW |= (extract32(arg, 2, 1) & extract32(arg, 10, 1)) << 26;
    fpu_set_state(env);

#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'updfl' executed\n");
#endif
}

void helper_qemu_excp(CPUTriCoreState *env, uint32_t excp)
{
    CPUState *cs = env_cpu(env);
    cs->exception_index = excp;
    cpu_loop_exit(cs);
    qemu_log("test\n");
}

#ifdef TC_1_8_SUPPORT
uint32_t helper_fabs(CPUTriCoreState *env, uint32_t arg)
{
    float32 f_arg = make_float32(arg);
    uint32_t result;

    tricore_sfmode(env);
    result = f_arg & 0x7FFFFFFF;
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'abs.f' executed\n");
#endif
    return result;
}

uint64_t helper_dabs(CPUTriCoreState *env, uint64_t arg)
{
    float64 f_arg = make_float64(arg);
    uint64_t result;

    tricore_dfmode(env);
    result = f_arg & 0x7FFFFFFFFFFFFFFFLL;

#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'abs.df' executed\n");
#endif
    return result;
}


uint32_t helper_dcmp(CPUTriCoreState *env, uint64_t r1, uint64_t r2)
{
    uint32_t result, flags;
    float64 arg1 = make_float64(r1);
    float64 arg2 = make_float64(r2);

    tricore_dfmode(env);
    set_flush_inputs_to_zero(0, &env->fp_status);

    result = 1 << (float64_compare_quiet(arg1, arg2, &env->fp_status) + 1);
    result |= float64_is_denormal(arg1) << 4;
    result |= float64_is_denormal(arg2) << 5;

    flags = f_get_excp_flags(env);
    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
    set_flush_inputs_to_zero(1, &env->fp_status);
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'cmp.df' executed\n");
#endif
    return result;
}

uint32_t helper_dftoi(CPUTriCoreState *env, uint64_t arg)
{
    float64 f_arg = make_float64(arg);
    uint32_t result;
    int32_t flags;

    tricore_dfmode(env);
    result = float64_to_int32(f_arg, &env->fp_status);

    flags = f_get_excp_flags(env);
    if (flags & float_flag_invalid) 
    {
        flags &= ~float_flag_inexact;
        if (float64_is_any_nan(f_arg)) 
        {
            result = 0;
        }
    }
    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'dftoi' executed\n");
#endif
    return result;
}

uint32_t helper_dftoiz(CPUTriCoreState *env, uint64_t arg)
{
    float64 f_arg = make_float64(arg);
    uint32_t result;
    int32_t flags;

    tricore_dfmode(env);
    result = float64_to_int32_round_to_zero(f_arg, &env->fp_status);
    flags  = f_get_excp_flags(env);

    if (flags & float_flag_invalid) {
        flags &= ~float_flag_inexact;
        if (float64_is_any_nan(f_arg)) {
            result = 0;
        }
    }
    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'dftoiz' executed\n");
#endif
    return result;
}

uint32_t helper_dftou(CPUTriCoreState *env, uint64_t arg)
{
    float64 f_arg = make_float64(arg);
    uint32_t result;
    int32_t flags;

    tricore_dfmode(env);
    result = float64_to_uint32(f_arg, &env->fp_status);
    flags  = f_get_excp_flags(env);
    if (flags & float_flag_invalid) {
        flags &= ~float_flag_inexact;
        if (float64_is_any_nan(f_arg)) {
            result = 0;
        }
    }
    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'dftou' executed\n");
#endif
    return result;
}

uint32_t helper_dftouz(CPUTriCoreState *env, uint64_t arg)
{
    float64 f_arg = make_float64(arg);
    uint32_t result;
    int32_t flags;

    tricore_dfmode(env);
    result = float64_to_uint32_round_to_zero(f_arg, &env->fp_status);
    flags  = f_get_excp_flags(env);
    if (flags & float_flag_invalid) {
        flags &= ~float_flag_inexact;
        if (float64_is_any_nan(f_arg)) {
            result = 0;
        }
    }
    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'dftouz' executed\n");
#endif
    return result;
}

uint32_t helper_dftoin(CPUTriCoreState *env, uint64_t arg)
{
    float64 f_arg = make_float64(arg);
    uint32_t result;
    int32_t flags;

    tricore_dfmode(env);
    result = float64_to_int32_scalbn(f_arg, float_round_nearest_even, 0, &env->fp_status);
    flags  = f_get_excp_flags(env);
    if (flags & float_flag_invalid) {
        flags &= ~float_flag_inexact;
        if (float64_is_any_nan(f_arg)) {
            result = 0;
        }
    }
    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'dftoin' executed\n");
#endif
    return result;
}

uint32_t helper_dftof(CPUTriCoreState *env, uint64_t arg)
{
    float64 f_arg = make_float64(arg);
    float32 result;
    int32_t flags;

    tricore_dfmode(env);
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"dftof %8.8x %8.8lx %x\n",env->PC,arg,f_get_excp_flags(env));
	tf64u64 a;
	a.u64 = arg;
	fprintf(stderr,"dftof %8.8x %e %x\n",env->PC,a.f64,f_get_excp_flags(env));
#endif
    result = float64_to_float32(f_arg, &env->fp_status);
    if (float64_is_any_nan(f_arg)) {
#ifdef DBG_FPU_HELPER
	    fprintf(stderr,"dftof nan\n");
#endif
    	if (arg & 0x8000000000000000LL) result = 0xFF800000;
    	else result = 0x7F800000;
        result |= arg & 0x1FFFFF;
        result |= ((arg>>50) & 0x3) << 21;
        if ((result & 0x7FFFFF)==0) result|=0x00200000;
        }
	flags = f_get_excp_flags(env);
    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'dftof' executed\n");
#endif
    return (uint32_t)result;
}

uint64_t helper_dftol(CPUTriCoreState *env, uint64_t arg)
{
    float64 f_arg = make_float64(arg);
    uint64_t result;
    int32_t flags;

    tricore_dfmode(env);
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"dftol %8.8x %8.8lx %x\n",env->PC,arg,f_get_excp_flags(env));
	tf64u64 a;
	a.u64 = arg;
	fprintf(stderr,"dftol %8.8x %e %x\n",env->PC,a.f64,f_get_excp_flags(env));
#endif
    result = float64_to_int64(f_arg, &env->fp_status);
    flags  = f_get_excp_flags(env);
    if (flags & float_flag_invalid) {
        flags &= ~float_flag_inexact;
        if (float64_is_any_nan(f_arg)) {
            result = 0;
        }
    }
    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'dftol' executed\n");
#endif
    return result;
}

uint64_t helper_dftolz(CPUTriCoreState *env, uint64_t arg)
{
    float64 f_arg = make_float64(arg);
    uint64_t result;
    int32_t flags;

    tricore_dfmode(env);

    result = float64_to_int64_round_to_zero(f_arg, &env->fp_status);
    flags  = f_get_excp_flags(env);
    if (flags & float_flag_invalid) {
        flags &= ~float_flag_inexact;
        if (float64_is_any_nan(f_arg)) {
            result = 0;
        }
    }
    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'dftolz' executed\n");
#endif
    return result;
}

uint64_t helper_dftoul(CPUTriCoreState *env, uint64_t arg)
{
    float64 f_arg = make_float64(arg);
    uint64_t result;
    int32_t flags;

    tricore_dfmode(env);
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"dftoul %8.8x %8.8lx %x\n",env->PC,arg,f_get_excp_flags(env));
	tf64u64 a;
	a.u64=arg;
	fprintf(stderr,"dftoul %8.8x %e %x\n",env->PC,a.f64,f_get_excp_flags(env));
#endif
	result = float64_to_uint64(f_arg, &env->fp_status);
    flags  = f_get_excp_flags(env);
    if ((0x8000000000000000LL & f_arg)!=0)
    {
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"dftoul neg %x \n",flags);
#endif
    	flags |=float_flag_invalid;
    }
    if (flags & float_flag_invalid) {
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"dftoul invalid %x \n",flags);
#endif
        flags &= ~float_flag_inexact;
        if (float64_is_any_nan(f_arg)) {
            result = 0;
        }
    }
    if (flags & float_flag_inexact) {
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"dftoul inexact %x \n",flags);
#endif
        if ((flags & float_flag_invalid)==0) flags |= float_flag_inexact;
    }
    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'dftoul' executed\n");
#endif
    return result;
}

uint64_t helper_dftoulz(CPUTriCoreState *env, uint64_t arg)
{
    float64 f_arg = make_float64(arg);
    uint64_t result;
    int32_t flags;

    tricore_dfmode(env);

    result = float64_to_uint64_round_to_zero(f_arg, &env->fp_status);
    flags  = f_get_excp_flags(env);
    if (flags & float_flag_invalid) {
        flags &= ~float_flag_inexact;
        if (float64_is_any_nan(f_arg)) {
            result = 0;
        }
    } else if (float64_lt_quiet(f_arg, 0, &env->fp_status)) {
        flags  = float_flag_invalid;
        result = 0;
    }
    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'dftoulz' executed\n");
#endif
    return result;
}

uint64_t helper_ddiv(CPUTriCoreState *env, uint64_t r1, uint64_t r2)
{
    uint32_t flags;
    float64 arg1 = make_float64(r1);
    float64 arg2 = make_float64(r2);
    float64 f_result;

    tricore_dfmode(env);

#ifdef DBG_FPU_HELPER
	fprintf(stderr,"ddiv %8.8x %8.8lx %8.8lx %x\n",env->PC,arg1,arg2,f_get_excp_flags(env));
	tf64u64 darg1,darg2;
	darg1.u64 = arg1;
	darg2.u64 = arg2;
	fprintf(stderr,"ddiv %e %e\n",darg1.f64,darg2.f64);
#endif
    f_result = float64_div(arg1, arg2 , &env->fp_status);
    flags    = f_get_excp_flags(env);
    if (flags) {
        /* If the output is a NaN, but the inputs aren't,
           we return a unique value.  */
        if ((flags & float_flag_invalid)
            && !float64_is_any_nan(arg1)
            && !float64_is_any_nan(arg2)) {
                f_result = DDIV_NAN;
        }
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef DBG_FPU_HELPER
	tf64u64 dres;
	dres.u64=f_result;
	fprintf(stderr,"ddiv result=%8.8lx %e %x \n",f_result,dres.f64,f_get_excp_flags(env));
#endif
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'div.df' executed\n");
#endif
    return (uint64_t)f_result;
}

uint32_t helper_ftoin(CPUTriCoreState *env, uint32_t arg)
{
    float32 f_arg = make_float32(arg);
    int32_t result, flags;

    tricore_sfmode(env);
    result = float32_to_int32_scalbn(f_arg, float_round_nearest_even, 0, &env->fp_status);
    flags  = f_get_excp_flags(env);
    if (flags) {
        if (float32_is_any_nan(f_arg)) {
            result = 0;
        }
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'ftoin' executed\n");
#endif
    return (uint32_t)result;
}

uint64_t helper_ftodf(CPUTriCoreState *env, uint32_t arg)
{
    float32 f_arg = make_float32(arg);
    float64 result;
    int32_t flags;
    uint64_t argl;

    tricore_dfmode(env);
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"ftodf %8.8x %8.8x %x\n",env->PC,arg,f_get_excp_flags(env));
	tf32u32 a;
	a.u32 = arg;
	fprintf(stderr,"ftodf %8.8x %e %x\n",env->PC,a.f32,f_get_excp_flags(env));
#endif
	result = float32_to_float64(f_arg, &env->fp_status);
#ifdef DBG_FPU_HELPER
	tf64u64 r;
	r.u64 = result;
	fprintf(stderr,"ftodf res %8.8x %8lx %e %x\n",env->PC,r.u64,r.f64,f_get_excp_flags(env));
#endif
    if (float32_is_any_nan(f_arg)) {
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"ftdof nan\n");
#endif
        argl = arg;
    	if (arg & 0x80000000) result = 0xFFF0000000000000LL;
    	else result = 0x7FF0000000000000LL;
        result |= argl & 0x1FFFFFLL;
        result |= ((argl>>21) & 0x3) << 50;
    }
	flags = f_get_excp_flags(env);
    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'ftodf' executed\n");
#endif
    return (uint64_t)result;
}

#define DADD_SUB(op)                                                           \
uint64_t helper_d##op(CPUTriCoreState *env, uint64_t r1, uint64_t r2)          \
{                                                                              \
    float64 arg1 = make_float64(r1);                                           \
    float64 arg2 = make_float64(r2);                                           \
    uint64_t flags;                                                            \
    float64 f_result;                                                          \
    tricore_dfmode(env);                                                       \
    f_result = float64_##op(arg2, arg1, &env->fp_status);                      \
    flags    = f_get_excp_flags(env);                                          \
    if (flags) {                                                               \
        /* If the output is a NaN, but the inputs aren't,                      \
           we return a unique value.  */                                       \
        if ((flags & float_flag_invalid)                                       \
            && !float64_is_any_nan(arg1)                                       \
            && !float64_is_any_nan(arg2)) {                                    \
            f_result = DADD_NAN;                                               \
        }                                                                      \
        f_update_psw_flags(env, flags);                                        \
    } else {                                                                   \
        env->FPU_FS = 0;                                                       \
    }                                                                          \
    return (uint64_t)f_result;                                                 \
}
DADD_SUB(add)
DADD_SUB(sub)

uint64_t helper_dmul(CPUTriCoreState *env, uint64_t r1, uint64_t r2)
{
    uint32_t flags;
    float64 arg1 = make_float64(r1);
    float64 arg2 = make_float64(r2);
    float64 f_result;

    tricore_dfmode(env);

    f_result = float64_mul(arg1, arg2, &env->fp_status);
    flags    = f_get_excp_flags(env);
    if (flags) {
        /* If the output is a NaN, but the inputs aren't,
           we return a unique value.  */
        if ((flags & float_flag_invalid)
            && !float64_is_any_nan(arg1)
            && !float64_is_any_nan(arg2)) {
                f_result = DMUL_NAN;
        }
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'mul.df' executed\n");
#endif
    return (uint64_t)f_result;
}

uint64_t helper_dneg(CPUTriCoreState *env, uint64_t arg)
{
    float64 f_arg = make_float64(arg);
    uint64_t result;

    tricore_dfmode(env);
    result = f_arg ^ ~0x7FFFFFFFFFFFFFFFLL;
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'neg.df' executed\n");
#endif
    return result;
}

uint32_t helper_fneg(CPUTriCoreState *env, uint32_t arg)
{
    float32 f_arg = make_float32(arg);
    uint32_t result;

    tricore_sfmode(env);
    result = f_arg ^ ~0x7FFFFFFF;

#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'neg.f' executed\n");
#endif
    return result;
}

static const uint8_t target_dqseed_significand_table[64] = {
    0x33, 0x31, 0x2e, 0x2c, 0x29, 0x27, 0x25, 0x23,
    0x21, 0x1f, 0x1d, 0x1b, 0x1a, 0x18, 0x16, 0x15,
    0x13, 0x12, 0x10, 0x0f, 0x0d, 0x0c, 0x0b, 0x09,
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0x7e, 0x7a, 0x77, 0x73, 0x70, 0x6c, 0x69, 0x66,
    0x64, 0x61, 0x5e, 0x5c, 0x59, 0x57, 0x54, 0x52,
    0x50, 0x4e, 0x4c, 0x4a, 0x48, 0x46, 0x44, 0x42,
    0x41, 0x3f, 0x3d, 0x3c, 0x3a, 0x39, 0x37, 0x36
};

uint64_t helper_dqseed(CPUTriCoreState *env, uint64_t arg)
{
    float64 f_arg = make_float64(arg);
    uint64_t result;
	int16_t aExp;
    uint64_t aSig;
	int16_t resExp;
    uint64_t resSig;
    uint64_t aSign;
    int32_t flags;
    float64 tempa;

	tricore_dfmode(env);
	flags = f_get_excp_flags(env);
    tempa = f_arg;

    aSig =  f_arg & 0x000FFFFFFFFFFFFFULL;
    aExp = (f_arg >> 52) & 0x7FF;
    aSign = (f_arg >> 63);

    if (float64_is_quiet_nan(f_arg,&env->fp_status))
    {
        if (flags) {
            f_update_psw_flags(env, flags);
        } else {
            env->FPU_FS = 0;
        }
        return DQUIET_NAN;  //QUIET NaN
    }
    if (float64_is_signaling_nan(f_arg,&env->fp_status))
    {
        flags |=float_flag_invalid;
        if (flags) {
            f_update_psw_flags(env, flags);
        } else {
            env->FPU_FS = 0;
        }
        return DQUIET_NAN; //QUIET NaN
    }
    if (f_arg == 0x0) //POS ZERO
    {
        if (flags) {
            f_update_psw_flags(env, flags);
        } else {
            env->FPU_FS = 0;
        }
        return 0x7FF0000000000000; //POS INFINITY
    }
    if ((aSign == 1) && (f_arg != 0x8000000000000000))  //Negative number
    {
        flags |=float_flag_invalid;
        if (flags) {
            f_update_psw_flags(env, flags);
        } else {
            env->FPU_FS = 0;
        }
        return DSQRT_NAN;  //SQRT NaN
    }
    if (f_arg == 0x8000000000000000)   //NEG ZERO
    {
        if (flags) {
            f_update_psw_flags(env, flags);
        } else {
            env->FPU_FS = 0;
        }
        return 0xFFF0000000000000;  //NEG INFINITY
    }
    if (f_arg == 0x7FF0000000000000) //POS INFINITY
    {
        if (flags) {
            f_update_psw_flags(env, flags);
        } else {
            env->FPU_FS = 0;
        }
        return 0x0;
    }
    //first find the exponent...
    if (aExp == 0)
    {
        normalizeFloat64Subnormal(aSig, &aExp, &aSig);
        resExp = 1534 - (aExp / 2);  //bias(1022) + 511) -Exp/2
    }
    else
    {
        resExp = 1534 - ((aExp + 1) / 2);  //bias(1023) + 511) -Exp/2
    }
    //now get the significand... including the LSB of the exponent
    tempa = ((aExp & 1) << 5) | ((aSig >> 47) & 0x1F);
    resSig = (uint64_t)target_dqseed_significand_table[tempa] << 45;

    if ((aSig & 0x0000400000000000) == 0x0) // Bit 46
    {
        resSig = resSig | 0x0000100000000000; // Bit 44
    }
    else
    {
        resSig = resSig & 0xFFFFEFFFFFFFFFFF;
    }
    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
    result= (((uint64_t) resExp) << 52) + resSig;
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'qseed.df' executed\n");
#endif
    return (uint64_t) result;
}

uint64_t helper_ultodf(CPUTriCoreState *env, uint64_t arg)
{
    float64 f_result;
    uint32_t flags;

    tricore_dfmode(env);

    f_result = uint64_to_float64(arg, &env->fp_status);
    flags    = f_get_excp_flags(env);

    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'ultodf' executed\n");
#endif
    return (uint64_t)f_result;
}

uint64_t helper_ltodf(CPUTriCoreState *env, uint64_t arg)
{
    float64 f_result;
    uint32_t flags;

    tricore_dfmode(env);

    f_result = int64_to_float64(arg, &env->fp_status);
    flags    = f_get_excp_flags(env);

    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'ltodf' executed\n");
#endif
    return (uint64_t)f_result;
}

uint64_t helper_itodf(CPUTriCoreState *env, uint32_t arg)
{
    float64 f_result;
    uint32_t flags;

    tricore_dfmode(env);

    f_result = int32_to_float64(arg, &env->fp_status);
    flags    = f_get_excp_flags(env);

    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'itodf' executed\n");
#endif
    return (uint64_t)f_result;
}

uint64_t helper_utodf(CPUTriCoreState *env, uint32_t arg)
{
    float64 f_result;
    uint32_t flags;

    tricore_dfmode(env);

    f_result = uint32_to_float64(arg, &env->fp_status);
    flags    = f_get_excp_flags(env);

    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'utodf' executed\n");
#endif
    return (uint64_t)f_result;
}

uint64_t helper_dmax(CPUTriCoreState *env, uint64_t r1, uint64_t r2)
{
    float64 f_arg1 = make_float64(r1);
    float64 f_arg2 = make_float64(r2);
    uint32_t flags;

    tricore_dfmode(env);

#ifdef DBG_FPU_HELPER
	fprintf(stderr,"dmax %8.8x %8.8lx %8.8lx\n",env->PC,r1,r2);
#endif
	uint64_t result;
	flags = f_get_excp_flags(env);
	flags &= ~float_flag_invalid;

	if (float64_is_any_nan(f_arg1) && float64_is_any_nan(f_arg2)) {
    	result=DQUIET_NAN;
    	if (float64_is_signaling_nan(f_arg1,&env->fp_status)) flags |= float_flag_invalid;
    	if (float64_is_signaling_nan(f_arg2,&env->fp_status)) flags |= float_flag_invalid;
    }
    else if (float64_is_any_nan(f_arg1))
    {
    	result=f_arg2;
    	if (float64_is_signaling_nan(f_arg1,&env->fp_status)) flags |= float_flag_invalid;
    }
    else if (float64_is_any_nan(f_arg2))
    {
    	result=f_arg1;
    	if (float64_is_signaling_nan(f_arg2,&env->fp_status)) flags |= float_flag_invalid;
    }
    else
    {
    	result=helper_dcmp(env,f_arg1,f_arg2);
    	//bit0=lt bit1=eq bit2=gt bit4=arg1_subnormal bit5=arg2_subnormal
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"dmax %lx\n",result);
#endif
    	if ((result & 0x3) !=0) result=f_arg2; else result=f_arg1;
    }

    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'max.df' executed\n");
#endif
return result;
}

uint32_t helper_fmax(CPUTriCoreState *env, uint32_t r1, uint32_t r2)
{
    float32 f_arg1 = make_float32(r1);
    float32 f_arg2 = make_float32(r2);
    uint32_t flags;

    tricore_sfmode(env);

#ifdef DBG_FPU_HELPER
	fprintf(stderr,"fmax %8.8x %8.8x %8.8x\n",env->PC,r1,r2);
#endif
	uint32_t result;
	flags = f_get_excp_flags(env);
	flags &= ~float_flag_invalid;
	if (float32_is_any_nan(f_arg1) && float32_is_any_nan(f_arg2)) {
    	result = QUIET_NAN;
	if (float32_is_signaling_nan(f_arg1,&env->fp_status)) flags |= float_flag_invalid;
	if (float32_is_signaling_nan(f_arg2,&env->fp_status)) flags |= float_flag_invalid;
	}
    else if (float32_is_any_nan(f_arg1))
    {
    	result=f_arg2;
    	if (float32_is_signaling_nan(f_arg1,&env->fp_status)) flags |= float_flag_invalid;
    }
    else if (float32_is_any_nan(f_arg2))
    {
    	result=f_arg1;
    	if (float32_is_signaling_nan(f_arg2,&env->fp_status)) flags |= float_flag_invalid;
    }
    else
    {
    	result=helper_fcmp(env,f_arg1,f_arg2);
    	//bit0=lt bit1=eq bit2=gt bit4=arg1_subnormal bit5=arg2_subnormal
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"fmax %x\n",result);
#endif
    	if ((result & 0x3) !=0) result=f_arg2; else result=f_arg1;
    }
    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'max.f' executed\n");
#endif    
return result;
}

uint64_t helper_dmin(CPUTriCoreState *env, uint64_t r1, uint64_t r2)
{
    float64 f_arg1 = make_float64(r1);
    float64 f_arg2 = make_float64(r2);
    uint32_t flags;

    tricore_dfmode(env);
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"dmin %8.8x %8.8lx %8.8lx\n",env->PC,r1,r2);
#endif
	uint64_t result;
	flags = f_get_excp_flags(env);
	flags &= ~float_flag_invalid;

	if (float64_is_any_nan(f_arg1) && float64_is_any_nan(f_arg2)) {
    	result=DQUIET_NAN;
    	if (float64_is_signaling_nan(f_arg1,&env->fp_status)) flags |= float_flag_invalid;
    	if (float64_is_signaling_nan(f_arg2,&env->fp_status)) flags |= float_flag_invalid;
    }
    else if (float64_is_any_nan(f_arg1))
    {
    	result=f_arg2;
    	if (float64_is_signaling_nan(f_arg1,&env->fp_status)) flags |= float_flag_invalid;
    }
    else if (float64_is_any_nan(f_arg2))
    {
    	result=f_arg1;
    	if (float64_is_signaling_nan(f_arg2,&env->fp_status)) flags |= float_flag_invalid;
    }
    else
    {
    	result=helper_dcmp(env,f_arg1,f_arg2);
    	//bit0=lt bit1=eq bit2=gt bit4=arg1_subnormal bit5=arg2_subnormal
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"dmin %lx\n",result);
#endif
    	if ((result & 0x3) !=0) result=f_arg1; else result=f_arg2;
    }
    if (flags) 
    {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'min.df' executed\n");
#endif
return result;
}

uint32_t helper_fmin(CPUTriCoreState *env, uint32_t r1, uint32_t r2)
{
    float32 f_arg1 = make_float32(r1);
    float32 f_arg2 = make_float32(r2);
    uint32_t flags;

    tricore_sfmode(env);
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"fmin %8.8x %8.8x %8.8x\n",env->PC,r1,r2);
#endif
	uint32_t result;
	flags = f_get_excp_flags(env);
	flags &= ~float_flag_invalid;
	if (float32_is_any_nan(f_arg1) && float32_is_any_nan(f_arg2)) {
    	result = QUIET_NAN;
    	if (float32_is_signaling_nan(f_arg1,&env->fp_status)) flags |= float_flag_invalid;
    	if (float32_is_signaling_nan(f_arg2,&env->fp_status)) flags |= float_flag_invalid;
    }
    else if (float32_is_any_nan(f_arg1))
    {
    	result=f_arg2;
    	if (float32_is_signaling_nan(f_arg1,&env->fp_status)) flags |= float_flag_invalid;
    }
    else if (float32_is_any_nan(f_arg2))
    {
    	result=f_arg1;
    	if (float32_is_signaling_nan(f_arg1,&env->fp_status)) flags |= float_flag_invalid;
    }
    else
    {
    	result=helper_fcmp(env,f_arg1,f_arg2);
    	//bit0=lt bit1=eq bit2=gt bit4=arg1_subnormal bit5=arg2_subnormal
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"fmin %x\n",result);
#endif
    	if ((result & 0x3) !=0) result=f_arg1; else result=f_arg2;
    }
    if (flags) {
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'min.f' executed\n");
#endif
return result;
}

static inline void shift32RightJ(uint32_t a, int32_t count, uint32_t *zPtr)
{
	uint32_t z;
	if ( count == 0 ) {
		z = a;
	}
	else if ( count < 32 ) {
		z = ( a>>count ) | ( ( a<<( ( - count ) & 31 ) ) != 0 );
	}
	else {
		z = ( a != 0 );
	}
	*zPtr = z;
}

static inline float32 roundAndPackF32Q31(CPUTriCoreState *env,uint32_t zSign, int32_t zExp, uint32_t zSig, uint32_t rnd, int32_t *flags)
{
	int32_t roundingMode;
	uint32_t roundNearestEven;
	int32_t roundIncrement, roundBits;
	uint32_t isTiny;
	float32 tempRes;
	int32_t zExpCopy = zExp;

	roundingMode = rnd;
	roundNearestEven = (uint32_t)(roundingMode == float_round_nearest_even);
	roundIncrement = 0x40;
	if (!roundNearestEven)
	{
		if (roundingMode == float_round_to_zero)
		{
			roundIncrement = 0;
		}
		else
		{
			roundIncrement = 0x7F;
			if (zSign)
			{
				if (roundingMode == float_round_up)
					roundIncrement = 0;
			}
			else
			{
				if (roundingMode == float_round_down)
					roundIncrement = 0;
			}
		}
	}
	roundBits = (int32_t)(zSig & 0x7F);
	if (0xFE <= (uint32_t) zExp)
	{
		if ((0xFE < zExp) || ((zExp == 0xFE) && ((int32_t) (zSig + (uint32_t)(int32_t)roundIncrement) < 0)))
		{
			*flags|= float_flag_overflow | float_flag_inexact;
			return packFloat32(zSign, 0xFF, 0) - (float32)(roundIncrement == 0);
		}
		if (zExp < 0)
		{
			isTiny = (uint32_t)((get_float_detect_tininess(&env->fp_status)) ||
					(zExp < -1) ||
					(zSig + (uint32_t)(int)roundIncrement < 0x80000000));

			shift32RightJ(zSig, -zExp, &zSig);
			zExp = 0;
			roundBits = (int32_t)(zSig & 0x7F);
			if (isTiny || roundBits)
			{
				*flags|=float_flag_underflow | float_flag_inexact;
			}
		}
	}
	if (roundBits)
	{
		*flags|=float_flag_inexact;
	}

	uint32_t roundval = ~((uint32_t)((roundBits ^ 0x40) == 0) & roundNearestEven);

	if (roundNearestEven && (zExp == 0))
	{
		roundIncrement = 0x80;
		roundval = ~((uint32_t)((zSig & (uint32_t)(int)roundIncrement) == 0x0) & roundNearestEven);
	}

	zSig = (zSig + (uint32_t)(int)roundIncrement) >> 7;
	zSig &= roundval;

	if ((zExp == 0) && (zSig == 0x7FFFFF))
	{
		zSig = 0x800000;
	}
	tempRes = packFloat32(zSign, zExp, zSig);
	if ((tempRes == 0x7F800000) || (tempRes == 0xFF800000))
	{
		*flags|=float_flag_overflow;
	}
	if ((tempRes == 0x00800000)
			&& ((roundingMode == float_round_down) || (roundingMode == float_round_to_zero))
			&& (zExpCopy == 0x0))
	{
		return 0x0;
	}
	if ((tempRes == 0x80800000)
			&& ((roundingMode == float_round_up) || (roundingMode == float_round_to_zero))
			&& (zExpCopy == 0x0))
	{
		return 0x80000000;
	}
	return tempRes;
}

static inline uint32_t check_and_handle_denormal_u32u32(uint32_t a)
{
	int32_t aExp;
	uint32_t aSig;
	aSig = a & 0x007FFFFF;
	aExp = (a >> 23) & 0xFF;
	if ((aExp == 0) && (aSig != 0))
	{
		return (uint32_t) (0x80000000 & a);
	}
	return a;
}

static inline uint32_t check_and_handle_denormal_flags_u32f32(float32 a, int32_t *flags)
{
	int32_t aExp;
	uint32_t aSig;
	aSig = extractF32Frac(a);
	aExp = extractF32Exp(a);
	if ((aExp == 0) && (aSig != 0))
	{
		*flags|=float_flag_inexact | float_flag_underflow;
		return (uint32_t) (0x80000000 & a);
	}
	return a;
}

/*
FTOQ31 D[c], D[a], D[b] (RR)
arg_a = denorm_to_zero(f_real(D[a]);
if(is_nan(D[a])) then result = 0;
else precise_result = mul(arg_a, 2-D[b][8:0]);
if(precise_result > q_real(7FFFFFFFH)) then result = 7FFFFFFFH;
else if(precise_result < -1.0) then result = 80000000H;
else result = round_to_q31(precise_result);
D[c] = result[31:0]
arg0=Da, arg1=Db
 */

static uint32_t ftoq31xx(CPUTriCoreState *env, uint32_t arg1, uint32_t arg2,uint32_t rnd)
{

	int32_t aExp;
	uint32_t aSig;
	int32_t aSign;
	int32_t flags;
	uint32_t roundingBits;
	int32_t expAdj = 0;
	uint32_t Result1 = 0;
	uint32_t Result2 = 0;
	uint32_t Result4 = 0;
	float32 f_arg1;
	uint32_t rounding_mode=rnd;
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"ftoq31xx %8.8x %8.8x %8.8x %d\n",env->PC,arg1,arg2,rounding_mode);
#endif

	flags = f_get_excp_flags(env);
	flags &= ~float_flag_invalid;
	flags &= ~float_flag_inexact;

	arg1   = check_and_handle_denormal_u32u32(arg1);
	f_arg1 = make_float32(arg1);

	if (float32_is_any_nan(f_arg1))
	{
		flags=float_flag_invalid;
		if (flags) 
        { 
            f_update_psw_flags(env, flags); 
        } 
        else 
        { 
            env->FPU_FS = 0; 
        }
#ifdef    DBG_FPU_HELPER
		fprintf(stderr,"1\n");
#endif
		return 0x0;
	}

	expAdj = arg2 & 0x1FF;

	if ((expAdj & 0x100) == 0x100)
	{
		expAdj = ~(uint32_t)expAdj & 0x1FF;
		expAdj = expAdj + 1;
		expAdj = -expAdj;
	}

	aSig  = arg1 & 0x007FFFFFu;
	aExp  = (arg1>>23) & 0xFFu;
	aSign = arg1>>31;

	if ((arg1 == 0x80000000) || (arg1== 0x0))
	{
		if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }
#ifdef DBG_FPU_HELPER
		fprintf(stderr,"2\n");
#endif
		return 0;
	}

	if (arg1 == 0x7F800000)
	{
		flags=float_flag_invalid;
		if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }
#ifdef DBG_FPU_HELPER
		fprintf(stderr,"3\n");
#endif
		return 0x7FFFFFFF;
	}

	if (arg1 == 0xFF800000)
	{
		flags=float_flag_invalid;
		if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }
#ifdef DBG_FPU_HELPER
		fprintf(stderr,"4\n");
#endif
		return 0x80000000;
	}

	if ((arg1 == 0xBF800000) && (arg2 == 0x0))
	{
#ifdef DBG_FPU_HELPER
		fprintf(stderr,"5\n");
#endif
		return 0x80000000;
	}

	if ((arg1 == 0x3F800000) && (arg2 == 0x0))
	{
		flags=float_flag_invalid;
		if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }
#ifdef DBG_FPU_HELPER
		fprintf(stderr,"6\n");
#endif
		return 0x7FFFFFFF;
	}

	if ((aSign == 1) && ((aExp - expAdj) == 0x5F)
			&& (rounding_mode == float_round_nearest_even) && (aSig == 0))
	{

		flags=float_flag_inexact;
		if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }

#ifdef DBG_FPU_HELPER
		fprintf(stderr,"7\n");
#endif
		return 0;
	}
	if ((aSign == 0) && ((aExp - expAdj) == 0x5F)
			&& (rounding_mode == float_round_nearest_even) && (aSig == 0))
	{
		flags=float_flag_inexact;
		if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }
#ifdef DBG_FPU_HELPER
		fprintf(stderr,"8\n");
#endif
		return 0;
	}
	if ((aSign == 1) && ((aExp - expAdj) == 127) && (aSig == 0))
	{
		if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }
#ifdef DBG_FPU_HELPER
		fprintf(stderr,"9\n");
#endif
		return 0x80000000;
	}
	if ((aSign == 1) && ((aExp - expAdj) > 126))
	{
		flags=float_flag_invalid;
		if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }
#ifdef DBG_FPU_HELPER
		fprintf(stderr,"10\n");
#endif
		return 0x80000000;
	}
	if ((aSign == 0) && ((aExp - expAdj) > 126))
	{
		flags=float_flag_invalid;
		if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }
#ifdef DBG_FPU_HELPER
		fprintf(stderr,"11\n");
#endif
		return 0x7FFFFFFF;
	}

	if (((aExp - expAdj) < 96) && (((rounding_mode == float_round_nearest_even) && (((aExp - expAdj) != 0x5F))) ||
			((rounding_mode == float_round_down)
					&& (aSign == 0x0))
					|| (rounding_mode == float_round_to_zero)))
	{
		flags=float_flag_inexact;
		if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }

#ifdef DBG_FPU_HELPER
		fprintf(stderr,"12\n");
#endif
		return 0;
	}

	if (((aExp - expAdj) < 96) && ( ((rounding_mode == float_round_nearest_even)
			&& ((aExp - expAdj) == 0x5F)
			&& (aSign == 0x0))
			|| ((rounding_mode == float_round_up)
					&& (aSign == 0x0))
	))
	{
		flags=float_flag_inexact;
		if (flags) 
        { 
            f_update_psw_flags(env, flags); 
        } 
        else 
        { 
            env->FPU_FS = 0; 
        }
#ifdef DBG_FPU_HELPER
		fprintf(stderr,"13\n");
#endif
		return 0x00000001;
	}

	if (((aExp - expAdj) < 96) && (((rounding_mode == float_round_nearest_even)
			&& ((aExp - expAdj) != 0x5F))
			|| ((rounding_mode == float_round_up)
					&& (aSign == 0x1))
					|| (rounding_mode == float_round_to_zero)))
	{
		flags=float_flag_inexact;
		if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }

#ifdef DBG_FPU_HELPER
		fprintf(stderr,"14\n");
#endif
		return 0x0;
	}

	if (((aExp - expAdj) < 96) && ( ((rounding_mode == float_round_nearest_even)
			&& ((aExp - expAdj) == 0x5F)
			&& (aSign == 0x1) )
			|| ((rounding_mode == float_round_down)
					&& (aSign == 0x1))
	))
	{

		flags=float_flag_inexact;
		if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }

#ifdef DBG_FPU_HELPER
		fprintf(stderr,"15\n");
#endif
		return 0xFFFFFFFF;
	}

	aSig = aSig << 8;

	if (aSign == 0x0)
	{
		roundingBits = aSig << ((23 - ((127 - (aExp - expAdj)))) + 9);
		Result1 = (0x80000000 >> (127 - (aExp - expAdj)));
		Result2 = (aSig >> ((127 - (aExp - expAdj))));
		Result4 = Result1 | Result2;
		if (roundingBits != 0x0)
		{
			flags=float_flag_inexact;
			if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }
#ifdef    DBG_FPU_HELPER
			fprintf(stderr,"16\n");
#endif
		}
	}
	else
	{
		roundingBits = aSig << ((23 - ((127 - (aExp - expAdj)))) + 9);
		Result1 = (0x80000000 >> (127 - (aExp - expAdj)));
		Result2 = (aSig >> ((127 - (aExp - expAdj))));
		Result4 = Result1 | Result2;
		Result4 = ((~Result4) + 1);
		if (roundingBits != 0x0)
		{
			flags=float_flag_inexact;
			if (flags) 
            { 
                f_update_psw_flags(env, flags); 
            } 
            else 
            { 
                env->FPU_FS = 0; 
            }
#ifdef DBG_FPU_HELPER
			fprintf(stderr,"17\n");
#endif
		}
	}
	if (roundingBits)
	{
		if ((Result4 & 0x80000000) == 0x80000000)
		{
			switch (rounding_mode)
			{
			case float_round_nearest_even:
				if (roundingBits == 0x80000000)
				{
					if ((Result4 & 0x1) == 0x1)
					{
						Result4--;
					}
				}
				if (roundingBits > 0x80000000)
				{
					Result4--;
				}
				break;
			case float_round_down:
				Result4--;
				break;
			case float_round_up:
				break;
			case float_round_to_zero:
				break;
			default:
				assert(0);
				break;
			}
		}
		else
		{
			switch (rounding_mode)
			{
			case float_round_nearest_even:
				if (roundingBits == 0x80000000)
				{
					if ((Result4 & 0x1) == 0x1)
					{
						Result4++;
					}
				}
				if (roundingBits > 0x80000000)
				{
					Result4++;
				}
				break;
			case float_round_up:
				Result4++;
				break;
			case float_round_down:
				break;
			case float_round_to_zero:
				break;
			default:
				assert(0);
				break;
			}
		}
	}
	if (flags) 
    { 
        f_update_psw_flags(env, flags); 
    } 
    else 
    { 
        env->FPU_FS = 0; 
    }
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"E\n");
#endif
	return Result4;
}

uint32_t helper_q31tof(CPUTriCoreState *env, uint32_t arg1, uint32_t arg2)
{
	uint32_t result;
	int32_t flags;
	int32_t temparg1 = (int32_t) arg1;
	int32_t ResSign = (uint32_t)(arg1 >> 31);
	int32_t expAdj = 0;
	int32_t shiftcnt = 1;
	int32_t ResExp;
	uint32_t ResSignificand;
	uint32_t rounding_mode_tc;

	tricore_sfmode(env);

	switch ((env->PSW>>24) & 0x3)
	{
        case 0x0: 
            rounding_mode_tc=float_round_nearest_even; 
        break;
        case 0x1: 
            rounding_mode_tc=float_round_up; 
        break;
        case 0x2: 
            rounding_mode_tc=float_round_down; 
        break;
        case 0x3: 
            rounding_mode_tc=float_round_to_zero; 
        break;
        default: 
        break;
	}

#ifdef DBG_FPU_HELPER
	fprintf(stderr,"q31tof %8.8x %8.8x %8.8x %d\n",env->PC,arg1,arg2,rounding_mode_tc);
#endif
	flags = f_get_excp_flags(env);
	flags &= ~float_flag_inexact;
	flags &= ~float_flag_overflow;
	flags &= ~float_flag_underflow;

	if ((arg1 == 0x80000000) && ((arg2 & 0x000001FF) == 0x00000181))
	{
		flags=float_flag_underflow | float_flag_inexact;
		if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }
#ifdef    DBG_FPU_HELPER
			fprintf(stderr,"1\n");
#endif
			return 0x80000000;
	}
	if ((arg1 == 0x80000081) && ((arg2 & 0x000001FF) == 0x00000182)
			&& (rounding_mode_tc != float_round_down))
	{
		flags=float_flag_underflow | float_flag_inexact;
		if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }
#ifdef    DBG_FPU_HELPER
			fprintf(stderr,"2\n");
#endif
			return 0x80000000;
	}
	if ((arg1 == 0x80000081) && ((arg2 & 0x000001FF) == 0x00000182)
			&& (rounding_mode_tc == float_round_down))
	{
		flags=float_flag_underflow | float_flag_inexact;
		if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }
#ifdef    DBG_FPU_HELPER
			fprintf(stderr,"3\n");
#endif
			return 0x80800000;
	}
	if ((arg1 == 0x7FFFFF7F) && ((arg2 & 0x000001FF) == 0x00000182)
			&& (rounding_mode_tc == float_round_up))
	{
		flags=float_flag_underflow | float_flag_inexact;
		if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }
#ifdef DBG_FPU_HELPER
		fprintf(stderr,"4\n");
#endif
			return 0x00800000;
	}

	if (arg1 == 0)
	{
		if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }
#ifdef DBG_FPU_HELPER
		    fprintf(stderr,"5\n");
#endif
			return 0;
	}

	expAdj = arg2 & 0x1FF;
	if ((expAdj & 0x100) == 0x100)
	{
		expAdj = ~(uint32_t)expAdj & 0x1FF;
		expAdj = expAdj + 1;
		expAdj = -expAdj;
	}

	if ((arg1 & 0x80000000) == 0x80000000)
	{
		arg1 = ~arg1;
		arg1 = arg1 + 1;
	}

	if ((arg1 == 0x80000000))
	{
        result = check_and_handle_denormal_flags_u32f32(roundAndPackF32Q31
                                          (env,ResSign, (127 + expAdj), 0, rounding_mode_tc,&flags),&flags);
		if (flags) { f_update_psw_flags(env, flags); } else { env->FPU_FS = 0; }
#ifdef DBG_FPU_HELPER
			fprintf(stderr,"6\n");
#endif
		return result;
	}

	if (ResSign == 1)
	{
		temparg1 = -temparg1;
		arg1 = (uint32_t)temparg1;
	}

	arg1 = arg1 << 1;

	while ((arg1 & 0x80000000) != 0x80000000)
	{
		arg1 = arg1 << 1;
		shiftcnt++;
	}

	if (((uint32_t)(expAdj - shiftcnt) < 0xFFFFFF82) && ((uint32_t)(expAdj - shiftcnt) > 0x80000000))
	{
		flags=float_flag_underflow | float_flag_inexact;
#ifdef DBG_FPU_HELPER
			fprintf(stderr,"7\n");
#endif
	}

	if (((uint32_t)(expAdj - shiftcnt) > 127) && ((uint32_t)(expAdj - shiftcnt) < 0x80000000))
	{
		flags=float_flag_overflow | float_flag_inexact;
#ifdef DBG_FPU_HELPER
			fprintf(stderr,"8\n");
#endif
	}
	arg1 = (arg1 >> 1) & 0x3FFFFFFF;
	ResExp = (int32_t)((127 - (int32_t)shiftcnt) + expAdj);
	ResSignificand = arg1;
    result = check_and_handle_denormal_flags_u32f32(roundAndPackF32Q31
                                      (env,ResSign, ResExp, ResSignificand,rounding_mode_tc,&flags),&flags);
    if (flags) 
    { 
        f_update_psw_flags(env, flags); 
    } 
    else 
    { 
        env->FPU_FS = 0; 
    }
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"E\n");
#endif
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'q31tof' executed\n");
#endif
    return result;
}

uint32_t helper_ftoq31(CPUTriCoreState *env, uint32_t arg1, uint32_t arg2)
{
    uint32_t res;
	uint32_t rounding_mode_tc=(env->PSW>>24) & 0x3;
	uint32_t rounding_mode_host;

    tricore_sfmode(env);
	//Tricore Rounding Modes
	//00 Round to nearest.
	//01 Round toward +inf
	//10 Round toward -inf
	//11 Round toward zero
	switch (rounding_mode_tc)
	{
	case 0x0: 
        rounding_mode_host=float_round_nearest_even; 
        break;
	case 0x1: rounding_mode_host=float_round_up; break;
	case 0x2: rounding_mode_host=float_round_down; break;
	case 0x3: rounding_mode_host=float_round_to_zero; break;
	default: break;
	}
	res = ftoq31xx(env,arg1,arg2,rounding_mode_host);
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'ftoq31' executed\n");
#endif
    return res;
}

uint32_t helper_ftoq31z(CPUTriCoreState *env, uint32_t arg1, uint32_t arg2)
{
    uint32_t res;

    tricore_sfmode(env);
    //round to zero
    res = ftoq31xx(env,arg1,arg2,0x3);
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'ftoq31z' executed\n");
#endif
    return res;
}

uint64_t helper_dmadd(CPUTriCoreState *env, uint64_t r1,
                      uint64_t r2, uint64_t r3)
{
    uint32_t flags;
    float64 arg1 = make_float64(r1);
    float64 arg2 = make_float64(r2);
    float64 arg3 = make_float64(r3);
    float64 f_result;

    tricore_dfmode(env);

#ifdef DBG_FPU_HELPER
	fprintf(stderr,"dmadd %8.8x %8.8lx %8.8lx %8.8lx %x\n",env->PC,arg1,arg2,arg3,f_get_excp_flags(env));
#endif
    f_result = float64_muladd(arg1, arg2, arg3, 0, &env->fp_status);
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"dmadd result=%8.8lx %x \n",f_result,f_get_excp_flags(env));
#endif
    flags = f_get_excp_flags(env);
    if (flags) {
        if (flags & float_flag_invalid) {
            arg1 = float64_squash_input_denormal(arg1, &env->fp_status);
            arg2 = float64_squash_input_denormal(arg2, &env->fp_status);
            arg3 = float64_squash_input_denormal(arg3, &env->fp_status);
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"dmadd invalid %8.8x %8.8lx %8.8lx %8.8lx\n",env->PC,arg1,arg2,arg3);
#endif
	        f_result = d_maddsub_nan_result(arg1, arg2, arg3, f_result, 0);
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"dmadd invalid result=%8.8lx\n",f_result);
#endif
        }
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'madd.df' executed\n");
#endif
    return (uint64_t)f_result;
}

uint64_t helper_dmsub(CPUTriCoreState *env, uint64_t r1,
                      uint64_t r2, uint64_t r3)
{
    uint32_t flags;
    float64 arg1 = make_float64(r1);
    float64 arg2 = make_float64(r2);
    float64 arg3 = make_float64(r3);
    float64 f_result;

    tricore_dfmode(env);

#ifdef DBG_FPU_HELPER
	fprintf(stderr,"dmsub %8.8x %8.8lx %8.8lx %8.8lx %x\n",env->PC,arg1,arg2,arg3,f_get_excp_flags(env));
	tf64u64 a1,a2,a3;
	a1.u64=arg1;
	a2.u64=arg2;
	a3.u64=arg3;
	fprintf(stderr,"dmsub %8.8x %e %e %e %x\n",env->PC,a1.f64,a2.f64,a3.f64,f_get_excp_flags(env));
#endif
    f_result = float64_muladd(arg1, arg2, arg3, float_muladd_negate_product,
                              &env->fp_status);
#ifdef DBG_FPU_HELPER
	tf64u64 res,altres;
	res.u64=f_result;
	altres.f64=a3.f64-a1.f64*a2.f64;
    fprintf(stderr,"dmsub result=%8.8lx f=%e falt=%e %x %x\n",f_result,res.f64,altres.f64,f_get_excp_flags(env),float_flag_underflow);
#endif

    flags = f_get_excp_flags(env);
    if (flags) {
        if (flags & float_flag_invalid) {
            arg1 = float64_squash_input_denormal(arg1, &env->fp_status);
            arg2 = float64_squash_input_denormal(arg2, &env->fp_status);
            arg3 = float64_squash_input_denormal(arg3, &env->fp_status);
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"dmsub invalid %8.8x %8.8lx %8.8lx %8.8lx\n",env->PC,arg1,arg2,arg3);
#endif
            f_result = d_maddsub_nan_result(arg1, arg2, arg3, f_result, 1);
#ifdef DBG_FPU_HELPER
	fprintf(stderr,"dmsub invalid result=%8.8lx\n",f_result);
#endif
        }
        f_update_psw_flags(env, flags);
    } else {
        env->FPU_FS = 0;
    }
#ifdef TC_1_8_DEBUG
    qemu_log("\nIFX log Info: Tricore instruction 'msub.df' executed\n");
#endif
    return (uint64_t)f_result;
}

#endif
