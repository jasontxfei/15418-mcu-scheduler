/******************************************************************************
* File Name:   main.c
*
* Description: CM4 heartbeat while CM0+ runs the FIR benchmark.
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
#include "cybsp.h"

#define HEARTBEAT_LED_PORT       GPIO_PRT6
#define HEARTBEAT_LED_PIN        (3U)    /* Red LED3. */
#define HEARTBEAT_TOGGLE_MS      (500U)

volatile uint32_t cm4_heartbeat_count = 0U;

// helper function for printing results to serial terminal
static void serial_print_results(void)
{
    printf("M4 FIR: samples=%u taps=%u status=%lu "
           "timer_ready=%lu checksum=%ld\r\n",
           (unsigned int)FIR_SAMPLE_COUNT,
           (unsigned int)FIR_TAP_COUNT,
           (unsigned long)cm4_fir_status,
           (unsigned long)cm4_fir_timer_ready,
           (long)cm4_fir_checksum);

    printf("core_hz=%lu timer_hz=%lu ticks=%lu time_ns=",
           (unsigned long)cm4_core_clock_hz,
           (unsigned long)cm4_timer_clock_hz,
           (unsigned long)cm4_fir_ticks);

    /* Avoid %llu, which newlib-nano doesn't support. */
    const uint64_t time_ns = cm4_fir_time_ns;
    if (time_ns >= 1000000000ULL)
    {
        printf("%lu%09lu",
               (unsigned long)(time_ns / 1000000000ULL),
               (unsigned long)(time_ns % 1000000000ULL));
    }
    else
    {
        printf("%lu", (unsigned long)time_ns);
    }

    printf("\r\n");
    (void)fflush(stdout);
}

int main(void)
{
    cy_rslt_t result = cybsp_init();
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }
    __enable_irq();

    Cy_GPIO_Pin_FastInit(HEARTBEAT_LED_PORT, HEARTBEAT_LED_PIN,
                        CY_GPIO_DM_STRONG_IN_OFF, 1U, HSIOM_SEL_GPIO);
    for (;;)
    {
        Cy_GPIO_Inv(HEARTBEAT_LED_PORT, HEARTBEAT_LED_PIN);
        ++cm4_heartbeat_count;
        Cy_SysLib_Delay(HEARTBEAT_TOGGLE_MS);
    }
}

/* [] END OF FILE */
