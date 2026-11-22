/* Matched sequential/first-available-worker comparison, with separate traces. */
#if MCU_GREEDY_BENCHMARK
#include "greedy_batch.h"
#include "cy_pdl.h"
#include "cybsp.h"

#define CASES 25U
#define MODES 5U
#define TRIALS 31U
#define POISON 0x13572468
#define FIR_TIMER_HW TCPWM0
#define FIR_TIMER_COUNTER 0U
#define FIR_TIMER_CLOCK PCLK_TCPWM0_CLOCKS0
#define FIR_TIMER_DIV_TYPE CY_SYSCLK_DIV_16_BIT
#define FIR_TIMER_DIV_NUM 0U

typedef struct
{
    uint32_t case_id, mode, jobs, total_samples, samples, taps;
    uint32_t m4_kernel, m0_kernel, trials, verified;
    uint32_t ticks[TRIALS], m4_jobs[TRIALS], m0_jobs[TRIALS];
} greedy_result_t;
typedef struct
{
    uint32_t case_id, jobs, elapsed, overlap, m4_kernel, m0_kernel, m4_jobs, m0_jobs;
    /* samples, taps, owner, claims, start relative to batch, end relative */
    uint32_t timeline[BATCH_MAX_JOBS][6];
} greedy_trace_t;
volatile uint32_t gbench_status=0U, gbench_error=0U;
volatile uint32_t gbench_core_clock_hz=0U, gbench_timer_clock_hz=0U, gbench_timer_ready=0U;
volatile uint32_t gbench_record_words=sizeof(greedy_result_t)/4U;
volatile uint32_t gbench_trace_words=sizeof(greedy_trace_t)/4U;
volatile uint32_t gbench_records=0U, gbench_traces=0U, gbench_verified_batches=0U;
volatile uint32_t gbench_verified_jobs=0U, gbench_overlap_cases=0U, gbench_prechecks=0U;
volatile greedy_result_t gbench_results[CASES][MODES];
volatile greedy_trace_t gbench_trace[CASES];
static int32_t expected[BATCH_OUTPUT_SAMPLES];
static uint32_t epoch=0U, batch_start=0U, batch_elapsed=0U;

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

    gbench_core_clock_hz = Cy_SysClk_ClkFastGetFrequency();

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

    gbench_timer_clock_hz = Cy_SysClk_PeriphGetFrequency(FIR_TIMER_DIV_TYPE,
                                                    FIR_TIMER_DIV_NUM);
    if ((gbench_timer_clock_hz == 0U) ||
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
            gbench_timer_ready = 1U;
            return true;
        }
    }
    return false;
}


static uint32_t now(void)
{
    __DSB(); __ISB();
    return Cy_TCPWM_Counter_GetCounter(TCPWM0,0U);
}

static uint32_t random_word(uint32_t *seed)
{
    uint32_t x=*seed;
    x^=x<<13; x^=x>>17; x^=x<<5;
    return *seed=x;
}

static bool set_job(uint32_t id,uint32_t samples,uint32_t taps,uint32_t *offset,uint32_t seed)
{
    if (*offset+samples>BATCH_POOL_SAMPLES || samples>1024U) return false;
    batch_job_t *job=&greedy_shared.jobs[id];
    job->samples=samples; job->input_offset=*offset; job->output_offset=*offset+id;
    int16_t coefficients[FIR_MAX_TAPS];
    for (uint32_t k=0U;k<taps;++k)
        coefficients[k]=(int16_t)((int32_t)(random_word(&seed)%2047U)-1023);
    if (!fir_plan_init(&job->plan,coefficients,taps)) return false;
    for (uint32_t n=0U;n<samples;++n)
        greedy_shared.input[*offset+n]=(int16_t)((int32_t)(random_word(&seed)&65535U)-32768);
    for (uint32_t n=0U;n<samples;++n)
    {
        int64_t value=0;
        for (uint32_t k=0U;k<taps && k<=n;++k)
            value+=(int64_t)greedy_shared.input[*offset+n-k]*coefficients[k];
        if (value<INT32_MIN || value>INT32_MAX) return false;
        expected[job->output_offset+n]=(int32_t)value;
    }
    *offset+=samples;
    return true;
}

static bool prepare_case(uint32_t id)
{
    static const uint32_t sizes[]={64U,128U,256U,512U,1024U};
    static const uint32_t taps[]={4U,8U,16U,32U};
    static const uint32_t mixed[]={16U,32U,64U,128U};
    static const uint32_t mixed_taps[]={32U,4U,16U,8U};
    uint32_t jobs=32U, n=0U, k=8U, offset=0U;
    if (id<20U) { n=sizes[id/4U]; k=taps[id%4U]; jobs=BATCH_POOL_SAMPLES/n; if (jobs>32U) jobs=32U; }
    else if (id==20U) n=1U;
    else if (id==21U) n=16U;
    else if (id==23U) k=32U;
    for (uint32_t j=0U;j<jobs;++j)
        if (!set_job(j,n==0U?mixed[j%4U]:n,id==24U?mixed_taps[j%4U]:k,
                     &offset,0x15418001U+id*104729U+j*7919U)) return false;
    greedy_shared.job_count=jobs;
    greedy_shared.total_samples=offset;
    return true;
}

static void reset_batch(uint32_t mode,uint32_t kernel4,uint32_t kernel0,uint32_t trace)
{
    greedy_shared.mode=mode<2U?BATCH_M4_ONLY:(mode<4U?BATCH_M0_ONLY:BATCH_GREEDY);
    greedy_shared.kernel[0]=kernel4; greedy_shared.kernel[1]=kernel0;
    greedy_shared.trace=trace;
    greedy_shared.next_job=0U;
    for (uint32_t c=0U;c<2U;++c)
    { greedy_shared.done_epoch[c]=0U; greedy_shared.completed[c]=0U; greedy_shared.error[c]=0U; }
    for (uint32_t j=0U;j<greedy_shared.job_count;++j)
    {
        batch_job_t *job=&greedy_shared.jobs[j];
        job->state=BATCH_UNCLAIMED; job->owner=UINT32_MAX; job->claims=0U;
        if (trace!=0U) { job->start_tick=0U; job->end_tick=0U; }
    }
}

static bool run_batch(uint32_t mode,uint32_t kernel4,uint32_t kernel0,uint32_t trace)
{
    /* Output poisoning is correctness instrumentation, common to both paths. */
    for (uint32_t n=0U;n<BATCH_OUTPUT_SAMPLES;++n) greedy_shared.output[n]=POISON;
    if (mode<2U) reset_batch(mode,kernel4,kernel0,trace);
    const uint32_t generation=++epoch;
    batch_start=now();
    /* Queue/control initialization is actual parallel runtime work. */
    if (mode>=2U) reset_batch(mode,kernel4,kernel0,trace);
    __DMB(); greedy_shared.epoch=generation; __DMB();
    if (mode<2U)
    {
        /* Strong sequential reference: direct calls, no queue locks, per-job
         * state bookkeeping, timestamps, wakeup or completion polling. */
        for (uint32_t j=0U;j<greedy_shared.job_count;++j)
        {
            const batch_job_t *job=&greedy_shared.jobs[j];
            if (kernel4==0U)
                fir_filter_scalar(&job->plan,greedy_shared.input+job->input_offset,
                                   greedy_shared.output+job->output_offset,job->samples);
            else
                fir_filter_dsp(&job->plan,greedy_shared.input+job->input_offset,
                                greedy_shared.output+job->output_offset,job->samples);
        }
    }
    else
    {
        if (!batch_notify_worker(generation)) { gbench_error=3U; return false; }
        if (mode==4U) batch_run_core(0U);
        while (!batch_worker_finished(generation))
            if ((uint32_t)(now()-batch_start)>gbench_timer_clock_hz)
            { gbench_error=4U; return false; } /* Never reuse a timed-out pool. */
    }
    batch_elapsed=now()-batch_start;
    if (mode<2U)
    {
        /* Serial bookkeeping is excluded from its primary timing. */
        greedy_shared.completed[0]=greedy_shared.job_count;
        for (uint32_t j=0U;j<greedy_shared.job_count;++j)
        { greedy_shared.jobs[j].owner=0U; greedy_shared.jobs[j].claims=1U; greedy_shared.jobs[j].state=BATCH_DONE; }
    }
    return true;
}

static bool verify(void)
{
    __DMB();
    if (greedy_shared.guard_begin!=BATCH_GUARD || greedy_shared.guard_end!=BATCH_GUARD ||
        greedy_shared.error[0]!=0U || greedy_shared.error[1]!=0U ||
        greedy_shared.completed[0]+greedy_shared.completed[1]!=greedy_shared.job_count)
    { gbench_error=5U; return false; }
    uint32_t counts[2]={0U,0U};
    for (uint32_t j=0U;j<greedy_shared.job_count;++j)
    {
        const batch_job_t *job=&greedy_shared.jobs[j];
        if (job->state!=BATCH_DONE || job->owner>1U || job->claims!=1U)
        { gbench_error=6U; return false; }
        ++counts[job->owner];
        for (uint32_t n=0U;n<job->samples;++n)
            if (greedy_shared.output[job->output_offset+n]!=expected[job->output_offset+n])
            { gbench_error=7U; return false; }
        if (greedy_shared.output[job->output_offset+job->samples]!=POISON)
        { gbench_error=8U; return false; }
    }
    if (counts[0]!=greedy_shared.completed[0] || counts[1]!=greedy_shared.completed[1])
    { gbench_error=9U; return false; }
    ++gbench_verified_batches;
    gbench_verified_jobs+=greedy_shared.job_count;
    return true;
}

static uint32_t median(volatile greedy_result_t *result)
{
    uint32_t values[TRIALS];
    for (uint32_t i=0U;i<TRIALS;++i)
    {
        uint32_t j=i;
        while (j>0U && values[j-1U]>result->ticks[i]) { values[j]=values[j-1U]; --j; }
        values[j]=result->ticks[i];
    }
    return values[TRIALS/2U];
}

static bool save_trace(uint32_t id,uint32_t kernel4,uint32_t kernel0)
{
    if (!run_batch(4U,kernel4,kernel0,1U) || !verify()) return false;
    volatile greedy_trace_t *trace=&gbench_trace[id];
    trace->case_id=id; trace->jobs=greedy_shared.job_count; trace->elapsed=batch_elapsed;
    trace->m4_kernel=kernel4; trace->m0_kernel=kernel0;
    trace->m4_jobs=greedy_shared.completed[0]; trace->m0_jobs=greedy_shared.completed[1];
    uint32_t overlap=0U;
    for (uint32_t j=0U;j<greedy_shared.job_count;++j)
    {
        const batch_job_t *job=&greedy_shared.jobs[j];
        const uint32_t start=job->start_tick-batch_start, end=job->end_tick-batch_start;
        if (end<start || end>batch_elapsed) { gbench_error=10U; return false; }
        trace->timeline[j][0]=job->samples; trace->timeline[j][1]=job->plan.tap_count;
        trace->timeline[j][2]=job->owner; trace->timeline[j][3]=job->claims;
        trace->timeline[j][4]=start; trace->timeline[j][5]=end;
        if (job->owner!=0U) continue;
        for (uint32_t k=0U;k<greedy_shared.job_count;++k)
        {
            const batch_job_t *other=&greedy_shared.jobs[k];
            if (other->owner!=1U) continue;
            const uint32_t s2=other->start_tick-batch_start, e2=other->end_tick-batch_start;
            const uint32_t lo=start>s2?start:s2, hi=end<e2?end:e2;
            if (hi>lo) overlap+=hi-lo;
        }
    }
    trace->overlap=overlap;
    if (overlap>0U) ++gbench_overlap_cases;
    ++gbench_traces;
    return true;
}

static bool prechecks(void)
{
    static const uint32_t jobs[]={0U,1U,32U,1U,32U};
    static const uint32_t sizes[]={0U,0U,0U,1024U,1U};
    static const uint32_t taps[]={1U,1U,32U,31U,3U};
    for (uint32_t i=0U;i<5U;++i)
    {
        uint32_t offset=0U;
        for (uint32_t j=0U;j<jobs[i];++j)
            if (!set_job(j,sizes[i],taps[i],&offset,15418U+j)) return false;
        greedy_shared.job_count=jobs[i]; greedy_shared.total_samples=offset;
        if (!run_batch(4U,1U,0U,0U) || !verify()) return false;
        ++gbench_prechecks;
    }
    return true;
}

static bool measure(void)
{
    if (!prechecks()) return false;
    for (uint32_t id=0U;id<CASES;++id)
    {
        if (!prepare_case(id)) { gbench_error=11U; return false; }
        uint32_t kernel4=1U,kernel0=0U;
        for (uint32_t mode=0U;mode<MODES;++mode)
        {
            if (mode==4U)
            {
                kernel4=median(&gbench_results[id][0])<=median(&gbench_results[id][1])?0U:1U;
                kernel0=median(&gbench_results[id][2])<=median(&gbench_results[id][3])?0U:1U;
            }
            const uint32_t k4=mode<2U?mode:kernel4, k0=(mode==2U||mode==3U)?mode-2U:kernel0;
            volatile greedy_result_t *result=&gbench_results[id][mode];
            result->case_id=id; result->mode=mode; result->jobs=greedy_shared.job_count;
            result->total_samples=greedy_shared.total_samples;
            result->samples=id<20U?greedy_shared.jobs[0].samples:(id<22U?(id==20U?1U:16U):0U);
            result->taps=id==24U?0U:greedy_shared.jobs[0].plan.tap_count;
            result->m4_kernel=k4; result->m0_kernel=k0; result->trials=TRIALS;
            for (uint32_t trial=0U;trial<TRIALS+3U;++trial)
            {
                if (!run_batch(mode,k4,k0,0U) || !verify()) return false;
                if (trial>=3U)
                {
                    const uint32_t index=trial-3U;
                    result->ticks[index]=batch_elapsed;
                    result->m4_jobs[index]=greedy_shared.completed[0];
                    result->m0_jobs[index]=greedy_shared.completed[1];
                    ++result->verified;
                }
            }
            ++gbench_records;
        }
        if (!save_trace(id,kernel4,kernel0)) return false;
    }
    return gbench_overlap_cases>0U;
}

int greedy_benchmark_main(void)
{
    gbench_status=1U;
    if (cybsp_init()!=CY_RSLT_SUCCESS) { gbench_error=1U; goto failed; }
    __enable_irq(); Cy_SysLib_Delay(20U);
    if (gbench_record_words!=sizeof(greedy_result_t)/4U ||
        gbench_trace_words!=sizeof(greedy_trace_t)/4U || !fir_timer_init() ||
        greedy_shared.ready!=1U ||
        (gbench_core_clock_hz!=150000000U && gbench_core_clock_hz!=100000000U))
    { gbench_error=2U; goto failed; }
    if (!measure()) goto failed;
    gbench_status=2U;
    for (;;) __WFI();
failed:
    gbench_status=3U;
    for (;;) __WFI();
}
#endif
