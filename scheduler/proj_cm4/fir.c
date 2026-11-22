#include "fir.h"

static const int16_t coefficients[FIR_TAP_COUNT] = {
    1, 2, 3, 4, 5, 6, 7, 8
};

void fir_generate_input(int16_t *input, size_t sample_count)
{
    for (size_t i = 0U; i < sample_count; ++i)
    {
        input[i] = (int16_t)((int32_t)((17U * i + 13U) % 256U) - 128);
    }
}

void fir_filter_block(const int16_t *input, int32_t *output,
                      size_t sample_count)
{
    for (size_t n = 0U; n < sample_count; ++n)
    {
        int32_t sum = 0;
        const size_t available_taps =
            (n < FIR_TAP_COUNT) ? n + 1U : FIR_TAP_COUNT;

        /* Missing samples before the block are zero, so skip those terms */
        for (size_t tap = 0U; tap < available_taps; ++tap)
        {
            sum += (int32_t)input[n - tap] * coefficients[tap];
        }

        output[n] = sum;
    }
}

bool fir_self_test(void)
{
    static const int16_t input[] = {
        1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0
    };
    static const int32_t expected[] = {
        1, 4, 10, 20, 30, 40, 50, 60, 61, 52, 32, 0
    };
    static const int16_t zero_input[sizeof(input) / sizeof(input[0])] = {0};
    static const int16_t negative_input[] = {-1, -2, -3, -4};
    static const int32_t negative_expected[] = {-1, -4, -10, -20};
    const size_t count = sizeof(input) / sizeof(input[0]);
    int32_t actual[sizeof(input) / sizeof(input[0])];

    /* Run twice to check that earlier calls do not leave filter history. */
    for (size_t run = 0U; run < 2U; ++run)
    {
        fir_filter_block(input, actual, count);
        for (size_t i = 0U; i < count; ++i)
        {
            if (actual[i] != expected[i])
            {
                return false;
            }
        }
    }

    /* Check that every output is overwritten, including after a nonzero job. */
    fir_filter_block(zero_input, actual, count);
    for (size_t i = 0U; i < count; ++i)
    {
        if (actual[i] != 0)
        {
            return false;
        }
    }

    fir_filter_block(negative_input, actual,
                     sizeof(negative_input) / sizeof(negative_input[0]));
    for (size_t i = 0U;
         i < sizeof(negative_expected) / sizeof(negative_expected[0]); ++i)
    {
        if (actual[i] != negative_expected[i])
        {
            return false;
        }
    }

    return true;
}
