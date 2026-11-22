#ifndef MCU_SCHEDULER_FIR_H
#define MCU_SCHEDULER_FIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIR_MAX_TAPS 32U
#define FIR_MAX_COEFFICIENT_L1 65535U

typedef struct
{
    uint32_t tap_count;
    int16_t coefficients[FIR_MAX_TAPS];
} fir_plan_t;

/* coefficients[0] multiplies the current sample. Preparation copies arbitrary
 * signed coefficients, rejecting zero/too many taps or sum(abs(c)) > 65535.
 * This bound makes every partial sum fit int32_t for every int16_t input.
 * On failure the plan is invalidated (tap_count == 0). Preparation is not part
 * of a filtering job; the immutable plan can be reused by either core. */
bool fir_plan_init(fir_plan_t *plan, const int16_t *coefficients, size_t taps);

/* Both kernels implement exactly the same unscaled integer convolution:
 * output[n] = sum(coefficients[k] * input[n-k]), with input[n<0] = 0.
 * Each call has independent zero history. Buffers and plan must not overlap;
 * input/output hold sample_count elements. A valid initialized plan is required.
 * sample_count == 0 touches neither input nor output. No allocation or IPC.
 * The DSP kernel uses M4 dual 16-bit MACs; other targets use a scalar fallback. */
void fir_filter_scalar(const fir_plan_t *plan, const int16_t *input,
                       int32_t *output, size_t sample_count);
void fir_filter_dsp(const fir_plan_t *plan, const int16_t *input,
                    int32_t *output, size_t sample_count);

#endif
