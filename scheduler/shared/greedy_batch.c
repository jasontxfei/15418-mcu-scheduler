#include "greedy_batch.h"
#include "cy_pdl.h"
#include <string.h>

__attribute__((section(".runtime_shared"), aligned(8), used))
batch_shared_t greedy_shared;
_Static_assert(sizeof(batch_shared_t) <= 0x4000U, "batch exceeds shared SRAM");
_Static_assert(BATCH_NOTIFY_CHANNEL >= CY_IPC_CHAN_USER, "reserved notify channel");
_Static_assert(BATCH_CLAIM_CHANNEL >= CY_IPC_CHAN_USER, "reserved claim channel");

static uint32_t claim(uint32_t core)
{
    IPC_STRUCT_Type *lock = Cy_IPC_Drv_GetIpcBaseAddress(BATCH_CLAIM_CHANNEL);
    uint32_t attempt = 0U;
    while (Cy_IPC_Drv_LockAcquire(lock) != CY_IPC_DRV_SUCCESS)
        if (++attempt == 100000U) { greedy_shared.error[core] = 1U; return BATCH_NO_JOB; }
    __DMB();
    uint32_t index = greedy_shared.next_job;
    if (index < greedy_shared.job_count)
    {
        greedy_shared.next_job = index + 1U;
        batch_job_t *job = &greedy_shared.jobs[index];
        if (job->state != BATCH_UNCLAIMED || job->claims != 0U)
            greedy_shared.error[core] = 2U;
        job->owner = core;
        ++job->claims;
        job->state = BATCH_RUNNING;
    }
    else index = BATCH_NO_JOB;
    __DMB();
    if (Cy_IPC_Drv_LockRelease(lock, CY_IPC_NO_NOTIFICATION) != CY_IPC_DRV_SUCCESS)
        greedy_shared.error[core] = 3U;
    return index;
}

static void execute_job(uint32_t index, uint32_t core)
{
    batch_job_t *job = &greedy_shared.jobs[index];
    if (job->samples > 1024U || job->input_offset > BATCH_POOL_SAMPLES ||
        job->samples > BATCH_POOL_SAMPLES-job->input_offset ||
        job->output_offset >= BATCH_OUTPUT_SAMPLES ||
        job->samples >= BATCH_OUTPUT_SAMPLES-job->output_offset ||
        job->plan.tap_count == 0U || job->plan.tap_count > FIR_MAX_TAPS)
    { greedy_shared.error[core] = 4U; return; }
    if (greedy_shared.trace != 0U)
    {
        __DSB();
        job->start_tick = Cy_TCPWM_Counter_GetCounter(TCPWM0, 0U);
    }
    if (greedy_shared.kernel[core] == 0U)
        fir_filter_scalar(&job->plan, greedy_shared.input+job->input_offset,
                           greedy_shared.output+job->output_offset, job->samples);
    else
        fir_filter_dsp(&job->plan, greedy_shared.input+job->input_offset,
                        greedy_shared.output+job->output_offset, job->samples);
    if (greedy_shared.trace != 0U)
    {
        __DSB();
        job->end_tick = Cy_TCPWM_Counter_GetCounter(TCPWM0, 0U);
    }
    __DMB();
    job->state = BATCH_DONE;
    ++greedy_shared.completed[core];
}

void batch_run_core(uint32_t core)
{
    if (greedy_shared.mode == BATCH_GREEDY)
    {
        for (;;)
        {
            const uint32_t index = claim(core);
            if (index == BATCH_NO_JOB || greedy_shared.error[core] != 0U) break;
            execute_job(index,core);
        }
    }
    else /* Solo worker calibration; no competing producer or claimant. */
    {
        for (uint32_t index = 0U; index < greedy_shared.job_count; ++index)
        {
            batch_job_t *job = &greedy_shared.jobs[index];
            job->owner = core; job->claims = 1U; job->state = BATCH_RUNNING;
            execute_job(index,core);
        }
        greedy_shared.next_job = greedy_shared.job_count;
    }
    __DMB();
    greedy_shared.done_epoch[core] = greedy_shared.epoch;
    __DMB();
}

#if defined(CORE_NAME_CM4_0)
bool batch_notify_worker(uint32_t epoch)
{
    __DMB();
    return Cy_IPC_Drv_SendMsgWord(Cy_IPC_Drv_GetIpcBaseAddress(BATCH_NOTIFY_CHANNEL),
                                  1UL << BATCH_INTERRUPT,epoch) == CY_IPC_DRV_SUCCESS;
}

bool batch_worker_finished(uint32_t epoch)
{
    if (Cy_IPC_Drv_IsLockAcquired(Cy_IPC_Drv_GetIpcBaseAddress(BATCH_NOTIFY_CHANNEL)))
        return false;
    __DMB();
    return greedy_shared.done_epoch[1] == epoch;
}
#else
volatile uint32_t gworker_interrupts = 0U;
volatile uint32_t gworker_clock_hz = 0U;
volatile uint32_t gworker_error = 0U;
static volatile uint32_t pending = 0U;
static uint32_t last_epoch = 0U;

static void worker_interrupt(void)
{
    Cy_IPC_Drv_ClearInterrupt(Cy_IPC_Drv_GetIntrBaseAddr(BATCH_INTERRUPT),
                              0U,1UL << BATCH_NOTIFY_CHANNEL);
    ++gworker_interrupts;
    pending = 1U;
    __DSB();
}

bool batch_worker_init(void)
{
    memset(&greedy_shared,0,sizeof(greedy_shared));
    greedy_shared.guard_begin = greedy_shared.guard_end = BATCH_GUARD;
    gworker_clock_hz = Cy_SysClk_ClkSlowGetFrequency();
    static const cy_stc_sysint_t config = {
        .intrSrc=NvicMux2_IRQn, .cm0pSrc=cpuss_interrupts_ipc_8_IRQn, .intrPriority=2U
    };
    if (Cy_IPC_Drv_IsLockAcquired(Cy_IPC_Drv_GetIpcBaseAddress(BATCH_NOTIFY_CHANNEL)) ||
        Cy_IPC_Drv_IsLockAcquired(Cy_IPC_Drv_GetIpcBaseAddress(BATCH_CLAIM_CHANNEL)) ||
        Cy_SysInt_Init(&config,worker_interrupt) != CY_SYSINT_SUCCESS)
    { gworker_error=1U; return false; }
    IPC_INTR_STRUCT_Type *irq = Cy_IPC_Drv_GetIntrBaseAddr(BATCH_INTERRUPT);
    Cy_IPC_Drv_ClearInterrupt(irq,0U,1UL << BATCH_NOTIFY_CHANNEL);
    Cy_IPC_Drv_SetInterruptMask(irq,0U,1UL << BATCH_NOTIFY_CHANNEL);
    NVIC_ClearPendingIRQ(NvicMux2_IRQn);
    NVIC_EnableIRQ(NvicMux2_IRQn);
    __DMB();
    greedy_shared.ready = 1U;
    return true;
}

static void run_message(void)
{
    uint32_t epoch=0U;
    IPC_STRUCT_Type *notify = Cy_IPC_Drv_GetIpcBaseAddress(BATCH_NOTIFY_CHANNEL);
    if (Cy_IPC_Drv_ReadMsgWord(notify,&epoch) != CY_IPC_DRV_SUCCESS)
    { gworker_error=2U; return; }
    __DMB();
    if (epoch <= last_epoch || epoch != greedy_shared.epoch ||
        greedy_shared.job_count > BATCH_MAX_JOBS ||
        greedy_shared.mode == BATCH_M4_ONLY || greedy_shared.mode > BATCH_GREEDY ||
        greedy_shared.kernel[1] > 1U)
    {
        gworker_error=3U;
        greedy_shared.error[1]=5U;
        __DMB(); greedy_shared.done_epoch[1]=epoch;
    }
    else { last_epoch=epoch; batch_run_core(1U); }
    __DMB();
    if (Cy_IPC_Drv_LockRelease(notify,CY_IPC_NO_NOTIFICATION) != CY_IPC_DRV_SUCCESS)
        gworker_error=4U;
}

void batch_worker_loop(void)
{
    for (;;)
    {
        __disable_irq();
        if (pending==0U) { __DSB(); __WFI(); }
        __enable_irq();
        if (pending!=0U) { pending=0U; run_message(); }
    }
}
#endif
