/******************************************************************************
* File Name:   main.c
*
* Description: CM0+ FIR benchmark, TCPWM timing, and LED heartbeat.
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

#include "cy_pdl.h"
#include "cycfg.h"
#include "cybsp.h"
#include "fir.h"

#define HEARTBEAT_LED_PORT       GPIO_PRT7
#define HEARTBEAT_LED_PIN        (1U)    /* Green LED4. */
#define HEARTBEAT_TOGGLE_MS      (250U)
#define OTHER_CORE_SETTLE_MS     (20U)
#define FIR_STATUS_NOT_RUN       (0U)
#define FIR_STATUS_PASSED        (1U)
#define FIR_STATUS_FAILED        (2U)
#define FIR_EXPECTED_CHECKSUM    (-19014)

/* Dedicated benchmark resources; keep these unused by other peripherals.
 * TCPWM0 counters are 32-bit on this device. Divider value 0 means /1. */
#define FIR_TIMER_HW            TCPWM0
#define FIR_TIMER_COUNTER       (0U)
#define FIR_TIMER_CLOCK         PCLK_TCPWM0_CLOCKS0
#define FIR_TIMER_DIV_TYPE      CY_SYSCLK_DIV_16_BIT
#define FIR_TIMER_DIV_NUM       (0U)

volatile uint32_t cm0p_heartbeat_count = 0U;

/* Inspect these symbols with the CM0+ debugger after startup. */
int16_t cm0p_fir_input[FIR_SAMPLE_COUNT];
int32_t cm0p_fir_output[FIR_SAMPLE_COUNT];
volatile uint32_t cm0p_fir_status = FIR_STATUS_NOT_RUN;
volatile uint32_t cm0p_fir_jobs_completed = 0U;
volatile int32_t cm0p_fir_checksum = 0;
volatile uint32_t cm0p_fir_timer_ready = 0U;
volatile uint32_t cm0p_core_clock_hz = 0U;
volatile uint32_t cm0p_timer_clock_hz = 0U;
volatile uint32_t cm0p_fir_ticks = 0U;
volatile uint64_t cm0p_fir_time_ns = 0U;
volatile uint32_t cm0p_fir_self_test_passed = 0U;

// Initialization function for on-board TCPWM0 counter 0, used for timing
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

    cm0p_core_clock_hz = Cy_SysClk_ClkSlowGetFrequency();

    // assign clock divider; M0+ configures this once
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

    cm0p_timer_clock_hz = Cy_SysClk_PeriphGetFrequency(FIR_TIMER_DIV_TYPE,
                                                    FIR_TIMER_DIV_NUM);
    if ((cm0p_timer_clock_hz == 0U) ||
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
            cm0p_fir_timer_ready = 1U;
            return true;
        }
    }
    return false;
}


int main(void)
{
    __enable_irq();
    cy_rslt_t result = cybsp_init();
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    Cy_GPIO_Pin_FastInit(HEARTBEAT_LED_PORT, HEARTBEAT_LED_PIN,
                        CY_GPIO_DM_STRONG_IN_OFF, 1U, HSIOM_SEL_GPIO);

    /* Keep separate results so a failed self-test is not mistaken for a
     * timer initialization failure. Both checks are outside the measurement. */
    cm0p_fir_self_test_passed = fir_self_test() ? 1U : 0U;
    const bool timer_ready = fir_timer_init();

    /* Start the heartbeat-only M4 image after configuring the timebase + settle time settle before measuring */
    Cy_SysEnableCM4(CY_CORTEX_M4_APPL_ADDR);
    Cy_SysLib_Delay(OTHER_CORE_SETTLE_MS);

    if ((cm0p_fir_self_test_passed != 0U) && timer_ready)
    {
        fir_generate_input(cm0p_fir_input, FIR_SAMPLE_COUNT);
        __DSB(); // data sync barrier
        __ISB(); // instruction sync barrier
        const uint32_t start_ticks =
            Cy_TCPWM_Counter_GetCounter(FIR_TIMER_HW, FIR_TIMER_COUNTER);
        fir_filter_block(cm0p_fir_input, cm0p_fir_output, FIR_SAMPLE_COUNT);
        __DSB(); // data sync barrier
        __ISB(); // instruction sync barrier
        const uint32_t elapsed_ticks =
            Cy_TCPWM_Counter_GetCounter(FIR_TIMER_HW, FIR_TIMER_COUNTER)
            - start_ticks;

        // conversion/checksum not included in timer
        cm0p_fir_ticks = elapsed_ticks; // for debugger variable inspection
        cm0p_fir_time_ns = ((uint64_t)elapsed_ticks * 1000000000ULL)
                          / cm0p_timer_clock_hz;

        int32_t checksum = 0;
        for (size_t i = 0U; i < FIR_SAMPLE_COUNT; ++i)
        {
            checksum += cm0p_fir_output[i];
        }
        cm0p_fir_checksum = checksum;
        cm0p_fir_jobs_completed = 1U;
        cm0p_fir_status = (checksum == FIR_EXPECTED_CHECKSUM)
                         ? FIR_STATUS_PASSED : FIR_STATUS_FAILED;
    }
    else
    {
        cm0p_fir_status = FIR_STATUS_FAILED;
    }

    for (;;)
    {
        if (cm0p_fir_status == FIR_STATUS_PASSED)
        {
            Cy_GPIO_Inv(HEARTBEAT_LED_PORT, HEARTBEAT_LED_PIN);
        }
        else
        {
            Cy_GPIO_Clr(HEARTBEAT_LED_PORT, HEARTBEAT_LED_PIN);
        }
        ++cm0p_heartbeat_count;
        Cy_SysLib_Delay(HEARTBEAT_TOGGLE_MS);
    }
}

/* [] END OF FILE */
