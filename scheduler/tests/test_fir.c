#include "fir.h"
#include <assert.h>
#include <stdio.h>

static uint32_t seed = 15418U;
static uint32_t random_word(void)
{
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}

int main(void)
{
    fir_plan_t plan;
    const int16_t rejected[] = {-32768, -32768};
    const int16_t accepted[] = {-32768, 32767};
    assert(!fir_plan_init(NULL, accepted, 2U));
    assert(!fir_plan_init(&plan, NULL, 2U));
    assert(!fir_plan_init(&plan, accepted, 0U));
    assert(!fir_plan_init(&plan, accepted, FIR_MAX_TAPS + 1U));
    assert(!fir_plan_init(&plan, rejected, 2U));
    assert(plan.tap_count == 0U);
    assert(fir_plan_init(&plan, accepted, 2U));
    unsigned cases = 0U;
    for (size_t taps = 1U; taps <= FIR_MAX_TAPS; ++taps)
        for (size_t size = 0U; size <= 103U; ++size)
            for (unsigned pattern = 0U; pattern < 5U; ++pattern)
            {
                int16_t coefficients[FIR_MAX_TAPS];
                int16_t storage[106];
                int16_t *input = storage + 1; /* May be only halfword aligned. */
                int32_t scalar[106], dsp[106];
                for (size_t k = 0U; k < taps; ++k)
                    coefficients[k] = (int16_t)((int32_t)(random_word() % 2047U) - 1023);
                if (pattern == 4U)
                {
                    for (size_t k = 0U; k < taps; ++k) coefficients[k] = 0;
                    coefficients[0] = -32768;
                    if (taps > 1U) coefficients[taps-1U] = 32767;
                }
                for (size_t n = 0U; n < size; ++n)
                    input[n] = pattern == 0U ? INT16_MIN :
                               pattern == 1U ? INT16_MAX :
                               pattern == 2U ? (n % 2U ? INT16_MAX : INT16_MIN) :
                               (int16_t)((int32_t)(random_word() & 65535U) - 32768);
                assert(fir_plan_init(&plan, coefficients, taps));
                scalar[0] = dsp[0] = 1234567;
                scalar[size+1U] = dsp[size+1U] = 7654321;
                fir_filter_scalar(&plan, input, scalar+1U, size);
                fir_filter_dsp(&plan, input, dsp+1U, size);
                for (size_t n = 0U; n < size; ++n)
                {
                    int64_t expected = 0;
                    for (size_t k = 0U; k < taps && k <= n; ++k)
                        expected += (int64_t)input[n-k] * coefficients[k];
                    assert(scalar[n+1U] == expected);
                    assert(dsp[n+1U] == expected);
                }
                assert(scalar[0] == 1234567 && dsp[0] == 1234567);
                assert(scalar[size+1U] == 7654321 && dsp[size+1U] == 7654321);
                ++cases;
            }
    assert(fir_plan_init(&plan, accepted, 2U));
    fir_filter_scalar(&plan, NULL, NULL, 0U);
    fir_filter_dsp(&plan, NULL, NULL, 0U);
    printf("PASS: %u full-output cases, coefficient bounds, and output guards\n", cases);
    return 0;
}
