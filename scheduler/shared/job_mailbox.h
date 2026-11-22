#ifndef MCU_JOB_MAILBOX_H
#define MCU_JOB_MAILBOX_H

#include "fir.h"

#define JOB_MAX_SAMPLES 1024U
#define JOB_IPC_CHANNEL 8U
#define JOB_IPC_INTERRUPT 8U
#define JOB_GUARD 0xA15418B6U

enum { JOB_FREE, JOB_READY, JOB_RUNNING, JOB_DONE };
enum { JOB_OP_NOOP, JOB_OP_FIR };
enum { JOB_OK, JOB_BUSY, JOB_INVALID, JOB_REMOTE_ERROR };

/* One producer (M4), one worker (M0+), one outstanding request. Both firmware
 * linkers place this exact ABI in the same dedicated NOLOAD SRAM region.
 * Ownership, hardware IPC lock publication, and DMB barriers protect buffers;
 * volatile control fields alone would not establish publication ordering. */
typedef struct
{
    uint32_t guard_begin;
    volatile uint32_t state;
    volatile uint32_t worker_ready;
    uint32_t sequence;
    uint32_t operation;
    uint32_t samples;
    uint32_t result;
    uint32_t worker_ticks;
    volatile uint32_t executions;
    fir_plan_t plan;
    int16_t input[JOB_MAX_SAMPLES];
    int32_t output[JOB_MAX_SAMPLES];
    uint32_t guard_end;
} job_mailbox_t;

typedef struct
{
    uint32_t sequence, result, worker_ticks, executions;
} job_completion_t;

extern job_mailbox_t runtime_mailbox;

/* M4 fills plan/input only while FREE. Submit publishes those buffers without
 * copying; sequence must be nonzero and strictly increasing within a boot.
 * The caller cannot mutate/reuse any slot data until collection succeeds.
 * collect is nonblocking; BUSY means keep waiting. A timeout must not free or
 * reuse an in-flight slot. Collected output remains valid until next submit. */
uint32_t job_submit(uint32_t operation, uint32_t samples, uint32_t sequence);
uint32_t job_try_collect(uint32_t sequence, job_completion_t *completion);

/* M0+: initialization happens once before launching M4. IRQ only wakes the
 * worker; execution occurs in foreground, never in the interrupt handler. */
bool job_worker_init(void);
void job_worker_loop(void);

#endif
