#ifndef MCU_GREEDY_BATCH_H
#define MCU_GREEDY_BATCH_H

#include "fir.h"

#define BATCH_MAX_JOBS 32U
#define BATCH_POOL_SAMPLES 2048U
#define BATCH_OUTPUT_SAMPLES (BATCH_POOL_SAMPLES + BATCH_MAX_JOBS)
#define BATCH_GUARD 0xB15418A7U
#define BATCH_NOTIFY_CHANNEL 8U
#define BATCH_CLAIM_CHANNEL 9U
#define BATCH_INTERRUPT 8U
#define BATCH_NO_JOB UINT32_MAX
enum { BATCH_M4_ONLY, BATCH_M0_ONLY, BATCH_GREEDY };
enum { BATCH_UNCLAIMED, BATCH_RUNNING, BATCH_DONE };

typedef struct
{
    fir_plan_t plan;
    uint32_t samples, input_offset, output_offset;
    volatile uint32_t state, owner, claims, start_tick, end_tick;
} batch_job_t;

typedef struct
{
    uint32_t guard_begin;
    volatile uint32_t ready, epoch;
    uint32_t mode, job_count, total_samples, kernel[2], trace;
    volatile uint32_t next_job, done_epoch[2], completed[2], error[2];
    batch_job_t jobs[BATCH_MAX_JOBS];
    int16_t input[BATCH_POOL_SAMPLES];
    int32_t output[BATCH_OUTPUT_SAMPLES];
    uint32_t guard_end;
} batch_shared_t;

extern batch_shared_t greedy_shared;

/* The controller prepares a bounded immutable batch only after the previous
 * wake channel is released. Kernels/offsets/plans stay fixed until both workers
 * finish. Claim channel 9 protects only next_job and ownership, never a kernel.
 * Each output range is exclusive to the worker that claims its descriptor. */
bool batch_worker_init(void);
void batch_worker_loop(void);
void batch_run_core(uint32_t core);
bool batch_notify_worker(uint32_t epoch);
bool batch_worker_finished(uint32_t epoch);
int greedy_benchmark_main(void);

#endif
