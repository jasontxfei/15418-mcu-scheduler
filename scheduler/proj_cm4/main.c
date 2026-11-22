/******************************************************************************
* File Name:   main.c
*
* Description: CM4 FIR workload, correctness checks, and LED heartbeat.
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
#include "fir.h"


#define HEARTBEAT_LED_PORT       GPIO_PRT6
#define HEARTBEAT_LED_PIN        (3U)    /* Red LED3 on CY8CPROTO-063-BLE. */
#define HEARTBEAT_TOGGLE_MS      (500U)
#define FIR_STATUS_NOT_RUN       (0U)
#define FIR_STATUS_PASSED        (1U)
#define FIR_STATUS_FAILED        (2U)
#define FIR_EXPECTED_CHECKSUM    (-19014)

/* Local to this core's firmware; useful when inspecting the CM4 image. */
volatile uint32_t cm4_heartbeat_count = 0U;

/* Inspect these symbols with the CM4 debugger after startup. */
int16_t cm4_fir_input[FIR_SAMPLE_COUNT];
int32_t cm4_fir_output[FIR_SAMPLE_COUNT];
volatile uint32_t cm4_fir_status = FIR_STATUS_NOT_RUN;
volatile uint32_t cm4_fir_jobs_completed = 0U;
volatile int32_t cm4_fir_checksum = 0;

int main(void)
{
    cy_rslt_t result = cybsp_init();
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    __enable_irq();

    /* This core owns LED3; CM0+ owns LED4 on a different GPIO port. */
    Cy_GPIO_Pin_FastInit(HEARTBEAT_LED_PORT, HEARTBEAT_LED_PIN,
                        CY_GPIO_DM_STRONG_IN_OFF, 1U, HSIOM_SEL_GPIO);

    if (fir_self_test())
    {
        fir_generate_input(cm4_fir_input, FIR_SAMPLE_COUNT);
        fir_filter_block(cm4_fir_input, cm4_fir_output, FIR_SAMPLE_COUNT);

        int32_t checksum = 0;
        for (size_t i = 0U; i < FIR_SAMPLE_COUNT; ++i)
        {
            checksum += cm4_fir_output[i];
        }
        cm4_fir_checksum = checksum;
        cm4_fir_jobs_completed = 1U;
        cm4_fir_status = (checksum == FIR_EXPECTED_CHECKSUM)
                         ? FIR_STATUS_PASSED : FIR_STATUS_FAILED;
    }
    else
    {
        cm4_fir_status = FIR_STATUS_FAILED;
    }

    for (;;)
    {
        if (cm4_fir_status == FIR_STATUS_PASSED)
        {
            Cy_GPIO_Inv(HEARTBEAT_LED_PORT, HEARTBEAT_LED_PIN);
        }
        else
        {
            /* Active-low red LED stays on if a correctness check fails. */
            Cy_GPIO_Clr(HEARTBEAT_LED_PORT, HEARTBEAT_LED_PIN);
        }
        ++cm4_heartbeat_count;
        Cy_SysLib_Delay(HEARTBEAT_TOGGLE_MS);
    }
}

/* [] END OF FILE */
