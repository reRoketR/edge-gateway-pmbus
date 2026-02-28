/***************************************************************************//**
* \file mtb_pmbus_conf.h
* \version 1.0
*
* Custom compile time options configuration file.
*
********************************************************************************
* \copyright
* (c) (2025), Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation. All rights reserved.
********************************************************************************
* This software, including source code, documentation and related materials
* ("Software") is owned by Cypress Semiconductor Corporation or one of its
* affiliates ("Cypress") and is protected by and subject to worldwide patent
* protection (United States and foreign), United States copyright laws and
* international treaty provisions. Therefore, you may use this Software only
* as provided in the license agreement accompanying the software package from
* which you obtained this Software ("EULA").
*
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software source
* code solely for use in connection with Cypress's integrated circuit products.
* Any reproduction, modification, translation, compilation, or representation
* of this Software except as specified above is prohibited without the express
* written permission of Cypress.
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
* including Cypress's product in a High Risk Product, the manufacturer of such
* system or application assumes all risk of such use and in doing so agrees to
* indemnify Cypress against all liability.
*******************************************************************************/

#ifndef MTB_PMBUS_CONF_H
#define MTB_PMBUS_CONF_H

#include "mtb_pmbus_log_level.h"

/* Modify the compile time options to reflect your configuration */

/* Debug-level logging so we can see all PMBus events on UART */
#define MTB_PMBUS_LOG_LEVEL                     (MTB_PMBUS_LOG_LEVEL_WARNING)

/* Disable timeout detection to simplify debugging */
#define MTB_PMBUS_ENABLE_TIMEOUT                (0U)

/* We don't need zone, SMBALERT, or general-call for this test */
#define MTB_PMBUS_SUPPORT_ZONE                  (0U)
#define MTB_PMBUS_SUPPORT_SMBALERT              (0U)
#define MTB_PMBUS_SUPPORT_GEN_CALL_ADDR         (0U)

/* We need PEC support (gateway sends PEC) */
#define MTB_PMBUS_SUPPORT_PEC                   (1U)

/* PMBus mode (not just SMBus) */
#define MTB_PMBUS_SUPPORT_PMBUS                 (1U)

/* Pages: we don't need pages for this simple test */
#define MTB_PMBUS_PAGES_NUM                     (0U)

/* Phases: not needed */
#define MTB_PMBUS_PHASES_NUM                    (0U)

/* Enable implemented commands: CAPABILITY and REVISION */
#define MTB_PMBUS_IMPL_CMD_CAPABILITY           (1U)
#define MTB_PMBUS_IMPL_CMD_REVISION             (1U)

/* Disable PAGE/PHASE/ZONE pre-implemented commands */
#define MTB_PMBUS_IMPL_CMD_PAGE                 (0U)
#define MTB_PMBUS_IMPL_CMD_PHASE                (0U)
#define MTB_PMBUS_IMPL_CMD_ZONE_CONFIG          (0U)
#define MTB_PMBUS_IMPL_CMD_ZONE_ACTIVE          (0U)
#define MTB_PMBUS_IMPL_CMD_QUERY                (0U)

#endif /* MTB_PMBUS_CONF_H */
