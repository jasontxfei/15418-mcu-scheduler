#include "fir.h"
#include <string.h>

#if defined(__ARM_FEATURE_DSP) && (__ARM_FEATURE_DSP == 1)
#include "cmsis_compiler.h"
#endif

bool fir_plan_init(fir_plan_t *plan, const int16_t *coefficients, size_t taps)
{
    if (plan == NULL) return false;
    plan->tap_count = 0U;
    if (coefficients == NULL || taps == 0U || taps > FIR_MAX_TAPS) return false;
    uint32_t magnitude = 0U;
    for (size_t k = 0U; k < taps; ++k)
    {
        const int32_t c = coefficients[k];
        magnitude += (uint32_t)(c < 0 ? -c : c);
    }
    if (magnitude > FIR_MAX_COEFFICIENT_L1) return false;
    memcpy(plan->coefficients, coefficients, taps * sizeof(*coefficients));
    plan->tap_count = (uint32_t)taps;
    return true;
}

static int32_t dot_scalar(const int16_t *input, const int16_t *coefficients,
                          size_t n, size_t taps)
{
    int32_t sum = 0;
    for (size_t k = 0U; k < taps; ++k)
        sum += (int32_t)input[n - k] * coefficients[k];
    return sum;
}

void fir_filter_scalar(const fir_plan_t *plan, const int16_t *restrict input,
                       int32_t *restrict output, size_t sample_count)
{
    const size_t taps = plan->tap_count;
    const size_t startup = sample_count < taps - 1U ? sample_count : taps - 1U;
    size_t n = 0U;
    for (; n < startup; ++n)
        output[n] = dot_scalar(input, plan->coefficients, n, n + 1U);
    for (; n < sample_count; ++n)
        output[n] = dot_scalar(input, plan->coefficients, n, taps);
}

/* memcpy permits unaligned halfword buffers without violating C aliasing rules.
 * M4 permits unaligned word loads in normal SRAM (UNALIGN_TRP must remain clear).
 * The two input samples have the opposite order to the two coefficients. */
static int32_t mac_pair(const int16_t *input, const int16_t *coefficients,
                        int32_t sum)
{
#if defined(__ARM_FEATURE_DSP) && (__ARM_FEATURE_DSP == 1)
    uint32_t x, c;
    memcpy(&x, input, sizeof(x));
    memcpy(&c, coefficients, sizeof(c));
    return (int32_t)__SMLADX(x, c, (uint32_t)sum);
#else
    return sum + (int32_t)input[1] * coefficients[0]
               + (int32_t)input[0] * coefficients[1];
#endif
}

void fir_filter_dsp(const fir_plan_t *plan, const int16_t *restrict input,
                    int32_t *restrict output, size_t sample_count)
{
    const size_t taps = plan->tap_count;
    const size_t startup = sample_count < taps - 1U ? sample_count : taps - 1U;
    const int16_t *c = plan->coefficients;
    size_t n = 0U;
    for (; n < startup; ++n)
        output[n] = dot_scalar(input, c, n, n + 1U);
    for (; sample_count - n >= 4U; n += 4U)
    {
        int32_t a = 0, b = 0, d = 0, e = 0;
        size_t k = 0U;
        for (; k + 1U < taps; k += 2U)
        {
            a = mac_pair(input + n - k - 1U, c + k, a);
            b = mac_pair(input + n - k, c + k, b);
            d = mac_pair(input + n - k + 1U, c + k, d);
            e = mac_pair(input + n - k + 2U, c + k, e);
        }
        if (k < taps)
        {
            a += (int32_t)input[n - k] * c[k];
            b += (int32_t)input[n - k + 1U] * c[k];
            d += (int32_t)input[n - k + 2U] * c[k];
            e += (int32_t)input[n - k + 3U] * c[k];
        }
        output[n] = a;
        output[n + 1U] = b;
        output[n + 2U] = d;
        output[n + 3U] = e;
    }
    for (; n < sample_count; ++n)
    {
        int32_t sum = 0;
        size_t k = 0U;
        for (; k + 1U < taps; k += 2U)
            sum = mac_pair(input + n - k - 1U, c + k, sum);
        if (k < taps) sum += (int32_t)input[n - k] * c[k];
        output[n] = sum;
    }
}
