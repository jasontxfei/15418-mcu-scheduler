/* Single-slot IPC correctness and latency benchmark; not a scheduler. */
#if MCU_DISPATCH_BENCHMARK
#include "dispatch_benchmark.h"
#include "job_mailbox.h"
#include "cy_pdl.h"
#include "cybsp.h"

#define TRIALS 31U
#define RECORDS 14U
#define FIR_TIMER_HW TCPWM0
#define FIR_TIMER_COUNTER 0U
#define FIR_TIMER_CLOCK PCLK_TCPWM0_CLOCKS0
#define FIR_TIMER_DIV_TYPE CY_SYSCLK_DIV_16_BIT
#define FIR_TIMER_DIV_NUM 0U
#define POISON 0x13572468

typedef struct
{
    uint32_t operation, samples, taps, paced, trials, verified;
    uint32_t roundtrip[TRIALS];
    uint32_t worker[TRIALS];
} dispatch_result_t;
volatile uint32_t rbench_status = 0U; /* 1 running, 2 passed, 3 failed. */
volatile uint32_t rbench_error = 0U;
volatile uint32_t rbench_core_clock_hz = 0U;
volatile uint32_t rbench_timer_clock_hz = 0U;
volatile uint32_t rbench_timer_ready = 0U;
volatile uint32_t rbench_records = 0U;
volatile uint32_t rbench_record_words = sizeof(dispatch_result_t)/4U;
volatile uint32_t rbench_jobs_verified = 0U;
volatile uint32_t rbench_protocol_checks = 0U;
volatile uint32_t rbench_timer_overhead[TRIALS];
volatile dispatch_result_t rbench_results[RECORDS];
static int32_t expected[JOB_MAX_SAMPLES];
static uint32_t next_sequence = 0U;

static bool fir_timer_init(void)
{
    // counter timer config struct
    static const cy_stc_tcpwm_counter_config_t timer_config = {
        .period = UINT32_MAX,
        .clockPrescaler = CY_TCPWM_COUNTER_PRESCALER_DIVBY_1,
        .runMode = CY_TCPWM_COUNTER_CONTINUOUS,
        .countDirection = CY_TCPWM_COUNTER_COUNT_UP,
        .compareOrCapture = CY_TCPWM_COUNTER_MODE_COMPARE,
        .compare0 = 0U,
        .compare1 = 0U,
        .enableCompareSwap = false,
        .interruptSources = CY_TCPWM_INT_NONE,
        .captureInputMode = CY_TCPWM_INPUT_RISINGEDGE,
        .captureInput = CY_TCPWM_INPUT_0,
        .reloadInputMode = CY_TCPWM_INPUT_RISINGEDGE,
        .reloadInput = CY_TCPWM_INPUT_0,
        .startInputMode = CY_TCPWM_INPUT_RISINGEDGE,
        .startInput = CY_TCPWM_INPUT_0,
        .stopInputMode = CY_TCPWM_INPUT_RISINGEDGE,
        .stopInput = CY_TCPWM_INPUT_0,
        .countInputMode = CY_TCPWM_INPUT_LEVEL,
        .countInput = CY_TCPWM_INPUT_1
    };

    rbench_core_clock_hz = Cy_SysClk_ClkFastGetFrequency();

    // Assign the dedicated timer divider once on the M4.
    if (Cy_SysClk_PeriphGetDividerEnabled(FIR_TIMER_DIV_TYPE,
                                         FIR_TIMER_DIV_NUM))
    {
        return false;
    }
    if ((Cy_SysClk_PeriphSetDivider(FIR_TIMER_DIV_TYPE,
                                   FIR_TIMER_DIV_NUM, 0U) != CY_SYSCLK_SUCCESS) ||
        (Cy_SysClk_PeriphAssignDivider(FIR_TIMER_CLOCK, FIR_TIMER_DIV_TYPE,
                                      FIR_TIMER_DIV_NUM) != CY_SYSCLK_SUCCESS) ||
        (Cy_SysClk_PeriphEnableDivider(FIR_TIMER_DIV_TYPE,
                                      FIR_TIMER_DIV_NUM) != CY_SYSCLK_SUCCESS))
    {
        return false;
    }

    rbench_timer_clock_hz = Cy_SysClk_PeriphGetFrequency(FIR_TIMER_DIV_TYPE,
                                                    FIR_TIMER_DIV_NUM);
    if ((rbench_timer_clock_hz == 0U) ||
        (Cy_TCPWM_Counter_Init(FIR_TIMER_HW, FIR_TIMER_COUNTER,
                              &timer_config) != CY_TCPWM_SUCCESS))
    {
        return false;
    }

    Cy_TCPWM_Counter_Enable(FIR_TIMER_HW, FIR_TIMER_COUNTER);
    Cy_TCPWM_TriggerStart_Single(FIR_TIMER_HW, FIR_TIMER_COUNTER);
    __DSB(); // data sync barrier

    // bounded polling to make sure timer is configured correctly
    const uint32_t before =
        Cy_TCPWM_Counter_GetCounter(FIR_TIMER_HW, FIR_TIMER_COUNTER);
    for (uint32_t attempt = 0U; attempt < 1024U; ++attempt)
    {
        if (Cy_TCPWM_Counter_GetCounter(FIR_TIMER_HW, FIR_TIMER_COUNTER)
            != before)
        {
            rbench_timer_ready = 1U;
            return true;
        }
    }
    return false;
}


static uint32_t now(void)
{
    __DSB(); __ISB();
    return Cy_TCPWM_Counter_GetCounter(TCPWM0, 0U);
}

static uint32_t random_word(uint32_t *seed)
{
    uint32_t x = *seed;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *seed = x;
}

static bool prepare(uint32_t operation, uint32_t samples, uint32_t taps)
{
    if (runtime_mailbox.state != JOB_FREE) { rbench_error = 3U; return false; }
    uint32_t seed = 0x15418001U + next_sequence * 7919U;
    if (operation == JOB_OP_FIR)
    {
        int16_t coefficients[FIR_MAX_TAPS];
        for (uint32_t k = 0U; k < taps; ++k)
            coefficients[k] = (int16_t)((int32_t)(random_word(&seed)%2047U)-1023);
        if (!fir_plan_init(&runtime_mailbox.plan, coefficients, taps))
        { rbench_error = 4U; return false; }
        for (uint32_t n = 0U; n < samples; ++n)
            runtime_mailbox.input[n] = (int16_t)((int32_t)(random_word(&seed)&65535U)-32768);
        for (uint32_t n = 0U; n < samples; ++n)
        {
            int64_t value = 0;
            for (uint32_t k = 0U; k < taps && k <= n; ++k)
                value += (int64_t)runtime_mailbox.input[n-k] * coefficients[k];
            if (value < INT32_MIN || value > INT32_MAX) { rbench_error = 5U; return false; }
            expected[n] = (int32_t)value;
            runtime_mailbox.output[n] = POISON;
        }
    }
    if (samples < JOB_MAX_SAMPLES) runtime_mailbox.output[samples] = POISON;
    return true;
}

static bool execute(uint32_t operation, uint32_t samples, bool probe,
                     uint32_t *roundtrip, uint32_t *worker)
{
    const uint32_t id = ++next_sequence;
    const uint32_t before = runtime_mailbox.executions;
    const uint32_t start = now();
    if (job_submit(operation, samples, id) != JOB_OK)
    { rbench_error = 6U; return false; }
    if (probe)
    {
        /* A second submit cannot overwrite READY, RUNNING, or uncollected DONE. */
        if (job_submit(operation, samples, id+1U) != JOB_BUSY)
        { rbench_error = 7U; return false; }
        ++rbench_protocol_checks;
    }
    job_completion_t completion;
    uint32_t status;
    do
    {
        status = job_try_collect(id, &completion);
        if ((uint32_t)(now()-start) > rbench_timer_clock_hz)
        { rbench_error = 8U; return false; } /* Never reclaim a timed-out slot. */
    } while (status == JOB_BUSY);
    *roundtrip = now()-start;
    if (status != JOB_OK || completion.sequence != id || completion.executions != before+1U)
    { rbench_error = 9U; return false; }
    *worker = completion.worker_ticks;
    if (*worker > *roundtrip) { rbench_error = 10U; return false; }
    for (uint32_t n = 0U; n < samples; ++n)
        if (runtime_mailbox.output[n] != expected[n])
        { rbench_error = 11U; return false; }
    if (samples < JOB_MAX_SAMPLES && runtime_mailbox.output[samples] != POISON)
    { rbench_error = 12U; return false; }
    if (probe)
    {
        if (job_try_collect(id, &completion) != JOB_INVALID ||
            job_submit(operation, samples, id) != JOB_INVALID)
        { rbench_error = 13U; return false; }
        rbench_protocol_checks += 2U;
    }
    ++rbench_jobs_verified;
    return true;
}

static bool run_checks(void)
{
    job_completion_t completion;
    if (job_submit(99U, 0U, 1U) != JOB_INVALID ||
        job_submit(JOB_OP_FIR, JOB_MAX_SAMPLES+1U, 1U) != JOB_INVALID ||
        job_submit(JOB_OP_NOOP, 0U, 0U) != JOB_INVALID ||
        job_try_collect(0U, &completion) != JOB_INVALID)
    { rbench_error = 14U; return false; }
    rbench_protocol_checks += 4U;
    static const uint32_t sizes[] = {0U,1U,7U,8U,9U,31U,64U,128U,256U,512U,1024U};
    static const uint32_t taps[] = {1U,3U,8U,31U,32U};
    for (uint32_t i = 0U; i < 128U; ++i)
    {
        const uint32_t n = sizes[i%11U], k = taps[(i/11U)%5U];
        uint32_t elapsed, worker;
        if (!prepare(JOB_OP_FIR, n, k) ||
            !execute(JOB_OP_FIR, n, true, &elapsed, &worker)) return false;
    }
    return true;
}

static bool measure(void)
{
    if (!run_checks()) return false;
    for (uint32_t i = 0U; i < TRIALS; ++i)
    {
        const uint32_t start = now();
        rbench_timer_overhead[i] = now()-start;
    }
    for (uint32_t paced = 0U; paced < 2U; ++paced)
        for (uint32_t test = 0U; test < 7U; ++test)
        {
            const uint32_t op = test == 0U ? JOB_OP_NOOP : JOB_OP_FIR;
            const uint32_t sizes[] = {64U,256U,1024U};
            const uint32_t n = test == 0U ? 0U : sizes[(test-1U)/2U];
            const uint32_t k = test == 0U ? 0U : ((test-1U)%2U ? 32U : 8U);
            volatile dispatch_result_t *result = &rbench_results[paced*7U+test];
            result->operation = op; result->samples = n; result->taps = k;
            result->paced = paced; result->trials = TRIALS;
            for (uint32_t trial = 0U; trial < TRIALS+3U; ++trial)
            {
                if (paced != 0U) Cy_SysLib_DelayUs(100U); /* Outside timer. */
                uint32_t elapsed, worker;
                if (!prepare(op,n,k) || !execute(op,n,false,&elapsed,&worker)) return false;
                if (trial >= 3U)
                {
                    result->roundtrip[trial-3U] = elapsed;
                    result->worker[trial-3U] = worker;
                    ++result->verified;
                }
            }
            ++rbench_records;
        }
    return runtime_mailbox.executions == rbench_jobs_verified;
}

int dispatch_benchmark_main(void)
{
    rbench_status = 1U;
    if (cybsp_init() != CY_RSLT_SUCCESS) { rbench_error = 1U; goto failed; }
    __enable_irq();
    Cy_SysLib_Delay(20U);
    if (rbench_record_words != sizeof(dispatch_result_t)/4U ||
        !fir_timer_init() || runtime_mailbox.worker_ready != 1U ||
        runtime_mailbox.state != JOB_FREE ||
        (rbench_core_clock_hz != 150000000U && rbench_core_clock_hz != 100000000U))
    { rbench_error = 2U; goto failed; }
    if (!measure()) goto failed;
    rbench_status = 2U;
    for (;;) __WFI();
failed:
    rbench_status = 3U;
    for (;;) __WFI();
}
#endif
