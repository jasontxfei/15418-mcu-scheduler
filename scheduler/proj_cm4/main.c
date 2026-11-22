/******************************************************************************
* File Name:   main.c
*
* Description: M4-only general FIR batch benchmark and debugger-visible results.
*
* Related Document: See README.md
*
*
*******************************************************************************
* Copyright 2020-2024, Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
*
* This software, including source code, documentation and related
* materials ("Software") is owned by Cypress Semiconductor Corporation
* or one of its affiliates ("Cypress") and is protected by and subject to
* worldwide patent protection (United States and foreign),
* United States copyright laws and international treaty provisions.
* Therefore, you may use this Software only as provided in the license
* agreement accompanying the software package from which you
* obtained this Software ("EULA").
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software
* source code solely for use in connection with Cypress's
* integrated circuit products.  Any reproduction, modification, translation,
* compilation, or representation of this Software except as specified
* above is prohibited without the express written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer
* of such system or application assumes all risk of such use and in doing
* so agrees to indemnify Cypress against all liability.
*******************************************************************************/

#if MCU_DISPATCH_BENCHMARK
#include "dispatch_benchmark.h"
int main(void) { return dispatch_benchmark_main(); }
#else
#include "cy_pdl.h"
#include "cybsp.h"
#include "fir.h"

#define BENCH_JOBS 16U
#define BENCH_MAX_SAMPLES 1024U
#define BENCH_TRIALS 31U
#define BENCH_CASES 20U
#define FIR_TIMER_HW TCPWM0
#define FIR_TIMER_COUNTER 0U
#define FIR_TIMER_CLOCK PCLK_TCPWM0_CLOCKS0
#define FIR_TIMER_DIV_TYPE CY_SYSCLK_DIV_16_BIT
#define FIR_TIMER_DIV_NUM 0U

/* Kept as 32-bit words for simple, versioned debugger extraction. */
typedef struct
{
    uint32_t samples, taps, kernel, jobs, trials, verified;
    uint32_t ticks[BENCH_TRIALS];
} bench_result_t;
volatile uint32_t bench_magic = 0x46495231U;
volatile uint32_t bench_version = 1U;
volatile uint32_t bench_record_words = sizeof(bench_result_t) / sizeof(uint32_t);
volatile uint32_t bench_status = 0U; /* 1 running, 2 passed, 3 failed. */
volatile uint32_t bench_error = 0U;
volatile uint32_t bench_error_job = 0U;
volatile uint32_t bench_error_sample = 0U;
volatile uint32_t bench_results_completed = 0U;
volatile uint32_t bench_core_clock_hz = 0U;
volatile uint32_t bench_slow_clock_hz = 0U;
volatile uint32_t bench_timer_clock_hz = 0U;
volatile uint32_t bench_timer_ready = 0U;
volatile uint32_t bench_dsp_available = 0U;
volatile uint32_t bench_timer_overhead[BENCH_TRIALS];
volatile bench_result_t bench_results[BENCH_CASES][2];
static int16_t inputs[BENCH_JOBS][BENCH_MAX_SAMPLES];
static int32_t outputs[BENCH_JOBS][BENCH_MAX_SAMPLES];
static int32_t expected[BENCH_JOBS][BENCH_MAX_SAMPLES];
static fir_plan_t plans[BENCH_JOBS];

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

    bench_core_clock_hz = Cy_SysClk_ClkFastGetFrequency();

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

    bench_timer_clock_hz = Cy_SysClk_PeriphGetFrequency(FIR_TIMER_DIV_TYPE,
                                                    FIR_TIMER_DIV_NUM);
    if ((bench_timer_clock_hz == 0U) ||
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
            bench_timer_ready = 1U;
            return true;
        }
    }
    return false;
}


static uint32_t random_word(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *state = x;
}

static bool prepare_batch(uint32_t samples, uint32_t taps)
{
    for (uint32_t job = 0U; job < BENCH_JOBS; ++job)
    {
        uint32_t seed = 0x15418001U + 7919U * job + 101U * samples + taps;
        int16_t coefficients[FIR_MAX_TAPS];
        for (uint32_t k = 0U; k < taps; ++k)
            coefficients[k] = (int16_t)((int32_t)(random_word(&seed) % 2047U) - 1023);
        if (!fir_plan_init(&plans[job], coefficients, taps)) return false;
        for (uint32_t n = 0U; n < samples; ++n)
            inputs[job][n] = (int16_t)((int32_t)(random_word(&seed) & 65535U) - 32768);
        /* Independent wide reference, outside every measured interval. */
        for (uint32_t n = 0U; n < samples; ++n)
        {
            int64_t sum = 0;
            for (uint32_t k = 0U; k < taps && k <= n; ++k)
                sum += (int64_t)inputs[job][n-k] * coefficients[k];
            if (sum < INT32_MIN || sum > INT32_MAX) return false;
            expected[job][n] = (int32_t)sum;
        }
    }
    return true;
}

static void run_batch(uint32_t kernel, uint32_t samples)
{
    for (uint32_t job = 0U; job < BENCH_JOBS; ++job)
    {
        if (kernel == 0U)
            fir_filter_scalar(&plans[job], inputs[job], outputs[job], samples);
        else
            fir_filter_dsp(&plans[job], inputs[job], outputs[job], samples);
    }
}

static bool verify_batch(uint32_t samples)
{
    for (uint32_t job = 0U; job < BENCH_JOBS; ++job)
        for (uint32_t n = 0U; n < samples; ++n)
            if (outputs[job][n] != expected[job][n])
            {
                bench_error_job = job;
                bench_error_sample = n;
                return false;
            }
    return true;
}

static uint32_t timer_read(void)
{
    __DSB();
    __ISB();
    return Cy_TCPWM_Counter_GetCounter(FIR_TIMER_HW, FIR_TIMER_COUNTER);
}

static bool edge_checks(void)
{
    const int16_t accepted[] = {-32768, 32767};
    const int16_t rejected[] = {-32768, -32768};
    fir_plan_t plan;
    if (!fir_plan_init(&plan, accepted, 2U) ||
        fir_plan_init(&plan, rejected, 2U) || plan.tap_count != 0U ||
        fir_plan_init(&plan, accepted, 0U)) return false;
    static const uint32_t sizes[] = {0U, 1U, 2U, 3U, 7U, 8U, 31U, 32U, 35U};
    static const uint32_t taps[] = {1U, 3U, 7U, 31U, 32U};
    for (uint32_t s = 0U; s < sizeof(sizes)/sizeof(sizes[0]); ++s)
        for (uint32_t t = 0U; t < sizeof(taps)/sizeof(taps[0]); ++t)
        {
            if (!prepare_batch(sizes[s], taps[t])) return false;
            for (uint32_t kernel = 0U; kernel < 2U; ++kernel)
            {
                run_batch(kernel, sizes[s]);
                if (!verify_batch(sizes[s])) return false;
            }
        }
    return true;
}

static bool benchmark(void)
{
    if (!edge_checks()) { bench_error = 3U; return false; }
    for (uint32_t trial = 0U; trial < BENCH_TRIALS; ++trial)
    {
        const uint32_t start = timer_read();
        bench_timer_overhead[trial] = timer_read() - start;
    }
    static const uint32_t sizes[] = {64U, 128U, 256U, 512U, 1024U};
    static const uint32_t taps[] = {4U, 8U, 16U, 32U};
    for (uint32_t s = 0U; s < 5U; ++s)
        for (uint32_t t = 0U; t < 4U; ++t)
        {
            const uint32_t index = s * 4U + t;
            if (!prepare_batch(sizes[s], taps[t])) { bench_error = 4U; return false; }
            for (uint32_t kernel = 0U; kernel < 2U; ++kernel)
            {
                volatile bench_result_t *result = &bench_results[index][kernel];
                result->samples = sizes[s]; result->taps = taps[t];
                result->kernel = kernel; result->jobs = BENCH_JOBS;
                result->trials = BENCH_TRIALS;
                for (uint32_t warmup = 0U; warmup < 3U; ++warmup)
                    run_batch(kernel, sizes[s]);
                for (uint32_t trial = 0U; trial < BENCH_TRIALS; ++trial)
                {
                    const uint32_t start = timer_read();
                    run_batch(kernel, sizes[s]);
                    const uint32_t elapsed = timer_read() - start;
                    result->ticks[trial] = elapsed;
                    if (!verify_batch(sizes[s])) { bench_error = 5U; return false; }
                    ++result->verified;
                }
                ++bench_results_completed;
            }
        }
    return true;
}

int main(void)
{
    bench_status = 1U;
    if (bench_magic != 0x46495231U || bench_version != 1U ||
        bench_record_words != sizeof(bench_result_t) / sizeof(uint32_t))
    { bench_error = 6U; goto failed; }
    if (cybsp_init() != CY_RSLT_SUCCESS) { bench_error = 1U; goto failed; }
    __enable_irq();
    /* Give the boot core time to enter CPU sleep. */
    Cy_SysLib_Delay(20U);
    bench_slow_clock_hz = Cy_SysClk_ClkSlowGetFrequency();
#if defined(__ARM_FEATURE_DSP) && (__ARM_FEATURE_DSP == 1)
    bench_dsp_available = 1U;
#endif
    if (!fir_timer_init()) { bench_error = 2U; goto failed; }
    const uint32_t expected_peripheral_hz = bench_core_clock_hz > 100000000U
                                           ? bench_core_clock_hz / 2U
                                           : bench_core_clock_hz;
    if ((bench_core_clock_hz != 150000000U && bench_core_clock_hz != 100000000U) ||
        bench_slow_clock_hz != expected_peripheral_hz ||
        bench_timer_clock_hz != expected_peripheral_hz ||
        bench_dsp_available == 0U || (SCB->CCR & SCB_CCR_UNALIGN_TRP_Msk) != 0U)
    { bench_error = 2U; goto failed; }
    if (!benchmark()) goto failed;
    bench_status = 2U;
    for (;;) __WFI();
failed:
    bench_status = 3U;
    for (;;) __WFI();
}

#endif
