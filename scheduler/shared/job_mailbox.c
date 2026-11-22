#include "job_mailbox.h"
#include "cy_pdl.h"
#include <string.h>

__attribute__((section(".runtime_shared"), aligned(8), used))
job_mailbox_t runtime_mailbox;
_Static_assert(sizeof(job_mailbox_t) <= 0x4000U, "mailbox exceeds shared SRAM");
_Static_assert(JOB_IPC_CHANNEL >= CY_IPC_CHAN_USER, "reserved IPC channel");

static IPC_STRUCT_Type *channel(void)
{
    return Cy_IPC_Drv_GetIpcBaseAddress(JOB_IPC_CHANNEL);
}

#if defined(CORE_NAME_CM4_0)
uint32_t job_submit(uint32_t operation, uint32_t samples, uint32_t sequence)
{
    if (runtime_mailbox.worker_ready != 1U) return JOB_INVALID;
    if (runtime_mailbox.state != JOB_FREE || Cy_IPC_Drv_IsLockAcquired(channel()))
        return JOB_BUSY;
    if (sequence == 0U || sequence <= runtime_mailbox.sequence ||
        samples > JOB_MAX_SAMPLES || operation > JOB_OP_FIR ||
        (operation == JOB_OP_NOOP && samples != 0U)) return JOB_INVALID;
    runtime_mailbox.sequence = sequence;
    runtime_mailbox.operation = operation;
    runtime_mailbox.samples = samples;
    runtime_mailbox.result = JOB_OK;
    __DMB();
    runtime_mailbox.state = JOB_READY;
    __DMB();
    if (Cy_IPC_Drv_SendMsgPtr(channel(), 1UL << JOB_IPC_INTERRUPT,
                             &runtime_mailbox) != CY_IPC_DRV_SUCCESS)
    {
        runtime_mailbox.state = JOB_FREE;
        return JOB_BUSY;
    }
    return JOB_OK;
}

uint32_t job_try_collect(uint32_t sequence, job_completion_t *completion)
{
    if (completion == NULL) return JOB_INVALID;
    if (Cy_IPC_Drv_IsLockAcquired(channel())) return JOB_BUSY;
    __DMB();
    if (runtime_mailbox.state != JOB_DONE || runtime_mailbox.sequence != sequence ||
        runtime_mailbox.guard_begin != JOB_GUARD || runtime_mailbox.guard_end != JOB_GUARD)
        return JOB_INVALID;
    completion->sequence = runtime_mailbox.sequence;
    completion->result = runtime_mailbox.result;
    completion->worker_ticks = runtime_mailbox.worker_ticks;
    completion->executions = runtime_mailbox.executions;
    const uint32_t result = runtime_mailbox.result;
    __DMB();
    runtime_mailbox.state = JOB_FREE;
    return result == JOB_OK ? JOB_OK : JOB_REMOTE_ERROR;
}
#else
volatile uint32_t rworker_interrupts = 0U;
volatile uint32_t rworker_sleep_entries = 0U;
volatile uint32_t rworker_clock_hz = 0U;
volatile uint32_t rworker_error = 0U;
static volatile uint32_t pending = 0U;
static uint32_t last_sequence = 0U;

static void worker_interrupt(void)
{
    Cy_IPC_Drv_ClearInterrupt(Cy_IPC_Drv_GetIntrBaseAddr(JOB_IPC_INTERRUPT),
                              0U, 1UL << JOB_IPC_CHANNEL);
    ++rworker_interrupts;
    pending = 1U;
    __DSB();
}

bool job_worker_init(void)
{
    memset(&runtime_mailbox, 0, sizeof(runtime_mailbox));
    runtime_mailbox.guard_begin = runtime_mailbox.guard_end = JOB_GUARD;
    rworker_clock_hz = Cy_SysClk_ClkSlowGetFrequency();
    static const cy_stc_sysint_t interrupt_config = {
        .intrSrc = NvicMux2_IRQn,
        .cm0pSrc = cpuss_interrupts_ipc_8_IRQn,
        .intrPriority = 2U
    };
    if (Cy_IPC_Drv_IsLockAcquired(channel()) ||
        Cy_SysInt_Init(&interrupt_config, worker_interrupt) != CY_SYSINT_SUCCESS)
    { rworker_error = 1U; return false; }
    IPC_INTR_STRUCT_Type *interrupt = Cy_IPC_Drv_GetIntrBaseAddr(JOB_IPC_INTERRUPT);
    Cy_IPC_Drv_ClearInterrupt(interrupt, 0U, 1UL << JOB_IPC_CHANNEL);
    Cy_IPC_Drv_SetInterruptMask(interrupt, 0U, 1UL << JOB_IPC_CHANNEL);
    NVIC_ClearPendingIRQ(NvicMux2_IRQn);
    NVIC_EnableIRQ(NvicMux2_IRQn);
    __DMB();
    runtime_mailbox.worker_ready = 1U;
    return true;
}

static void execute_one(void)
{
    void *message = NULL;
    if (Cy_IPC_Drv_ReadMsgPtr(channel(), &message) != CY_IPC_DRV_SUCCESS)
    { rworker_error = 2U; return; }
    __DMB();
    uint32_t result = JOB_OK;
    if (message != &runtime_mailbox || runtime_mailbox.state != JOB_READY ||
        runtime_mailbox.sequence <= last_sequence ||
        runtime_mailbox.guard_begin != JOB_GUARD || runtime_mailbox.guard_end != JOB_GUARD ||
        runtime_mailbox.samples > JOB_MAX_SAMPLES || runtime_mailbox.operation > JOB_OP_FIR)
        result = JOB_INVALID;
    runtime_mailbox.state = JOB_RUNNING;
    if (result == JOB_OK && runtime_mailbox.operation == JOB_OP_FIR)
    {
        uint32_t magnitude = 0U;
        const uint32_t taps = runtime_mailbox.plan.tap_count;
        if (taps == 0U || taps > FIR_MAX_TAPS) result = JOB_INVALID;
        else
        {
            for (uint32_t k = 0U; k < taps; ++k)
            {
                const int32_t c = runtime_mailbox.plan.coefficients[k];
                magnitude += (uint32_t)(c < 0 ? -c : c);
            }
            if (magnitude > FIR_MAX_COEFFICIENT_L1) result = JOB_INVALID;
        }
    }
    const uint32_t start = Cy_TCPWM_Counter_GetCounter(TCPWM0, 0U);
    if (result == JOB_OK && runtime_mailbox.operation == JOB_OP_FIR)
        fir_filter_scalar(&runtime_mailbox.plan, runtime_mailbox.input,
                           runtime_mailbox.output, runtime_mailbox.samples);
    __DSB();
    runtime_mailbox.worker_ticks = Cy_TCPWM_Counter_GetCounter(TCPWM0, 0U) - start;
    if (result == JOB_OK)
    {
        ++runtime_mailbox.executions;
        last_sequence = runtime_mailbox.sequence;
    }
    runtime_mailbox.result = result;
    __DMB();
    runtime_mailbox.state = JOB_DONE;
    __DMB();
    if (Cy_IPC_Drv_LockRelease(channel(), CY_IPC_NO_NOTIFICATION) != CY_IPC_DRV_SUCCESS)
        rworker_error = 3U;
}

void job_worker_loop(void)
{
    for (;;)
    {
        /* Mask-check-WFI-enable avoids a lost wakeup between checking the
         * software flag and sleeping. A pending enabled IRQ wakes WFI even
         * while PRIMASK masks handler entry; enable_irq then runs the handler. */
        __disable_irq();
        if (pending == 0U) { ++rworker_sleep_entries; __DSB(); __WFI(); }
        __enable_irq();
        if (pending != 0U) { pending = 0U; execute_one(); }
    }
}
#endif
