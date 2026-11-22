#ifndef MCU_SCHEDULER_FIR_H
#define MCU_SCHEDULER_FIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIR_TAP_COUNT    (8U)
#define FIR_SAMPLE_COUNT (256U)

/* Fill a block with repeatable samples in the range [-128, 127]. */
void fir_generate_input(int16_t *input, size_t sample_count);

/*
 * y[n] = x[n] + 2*x[n-1] + ... + 8*x[n-7], with x[n] = 0 for n < 0.
 * Each call starts with zero history and writes exactly sample_count outputs.
 * The input/output buffers must not overlap. Outputs are unnormalized int32_t
 * weighted sums; all int16_t input values fit safely with these fixed weights.
 */
void fir_filter_block(const int16_t *input, int32_t *output,
                      size_t sample_count);

/* Check hand-calculated outputs, signed inputs, and independent repeated calls. */
bool fir_self_test(void);

#endif /* MCU_SCHEDULER_FIR_H */
