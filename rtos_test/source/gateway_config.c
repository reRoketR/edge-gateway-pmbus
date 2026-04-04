/*******************************************************************************
 * File Name:   gateway_config.c
 *
 * Description: Instantiates the global configuration from the active profile.
 *              The profile is selected by the GW_PROFILE build define:
 *
 *                make build GW_PROFILE=exp1_single
 *
 *              If GW_PROFILE is not set, profile_default.h is used.
 *
 * Related Document: agent.md §5
 *
 ******************************************************************************/

#include "gateway_config.h"
#include <stdio.h>

/*******************************************************************************
 * Profile selection via preprocessor
 *
 * GW_PROFILE is set as a -D define from the Makefile.
 * We map it to the correct profile_*.h include.
 ******************************************************************************/

/*
 * Profile selection:
 *   make build GW_PROFILE=exp1_fast        → includes profile_exp1_fast.h
 *   make build GW_PROFILE=exp4_pec_off     → includes profile_exp4_pec_off.h
 *   make build                              → includes profile_default.h
 *
 * The Makefile passes GW_PROFILE_HEADER="profiles/profile_<name>.h"
 * so we can use it directly in #include.
 */
#ifdef GW_PROFILE_HEADER
  #include GW_PROFILE_HEADER
#else
  #include "profiles/profile_default.h"
#endif

/*******************************************************************************
 * Global config instance — defined exactly once
 ******************************************************************************/
const char   *g_profile_name = PROFILE_NAME;
#ifdef INTEGRATION_TEST
config_t       g_config      = PROFILE_CONFIG;   /* writable for test config */
#else
const config_t g_config      = PROFILE_CONFIG;
#endif

/*******************************************************************************
 * Function Name: config_print_boot_banner
 *******************************************************************************
 * Summary:
 *   Prints the active configuration to UART at boot.
 *   Required by agent.md §5.4 for thesis reproducibility.
 *
 *   Example output:
 *     [SYS] profile=default pec=1 mqtt=192.168.1.10:1883 q_telem=0 q_ctrl=1 q_metrics=0
 *     [SYS] i2c: speed=100000 timeout=20ms retries=2 recovery=1 settle=5ms
 *     [SYS] buffer: ram=256 flash=0 batch=50 drop_oldest=1
 *     [SYS] devices: 2
 *     [SYS]   [0] 0x58 "psu_a" poll=200ms status=1000ms
 *     [SYS]   [1] 0x59 "psu_b" poll=200ms status=1000ms
 ******************************************************************************/
void config_print_boot_banner(void)
{
    const config_t *c = &g_config;

    printf("\n");
    printf("[SYS] profile=%s  pec=%d  mqtt=%s:%u  q_telem=%u  q_ctrl=%u  q_metrics=%u\n",
           g_profile_name,
           (int)c->i2c.pec_enabled,
           c->mqtt.host,
           (unsigned)c->mqtt.port,
           (unsigned)c->mqtt.qos_telemetry,
           (unsigned)c->mqtt.qos_control,
           (unsigned)c->mqtt.qos_metrics);

    printf("[SYS] i2c: speed=%lu  transaction_timeout=%lums  retries=%u  recovery=%d  "
           "settle=%lums\n",
           (unsigned long)c->i2c.speed_hz,
           (unsigned long)c->i2c.transaction_timeout_ms,
           (unsigned)c->i2c.retries,
           (int)c->i2c.bus_recovery,
           (unsigned long)c->i2c.recovery_settle_ms);

    printf("[SYS] buffer: enabled=%d  ram=%u  flash=%lu  batch=%u  "
           "flush=%lums  drop_oldest=%d\n",
           (int)c->buffer.enabled,
           (unsigned)c->buffer.ram_max_records,
           (unsigned long)c->buffer.flash_max_records,
           (unsigned)c->buffer.flush_batch_size,
           (unsigned long)c->buffer.flush_interval_ms,
           (int)c->buffer.drop_oldest);

    printf("[SYS] metrics_period=%lums\n",
           (unsigned long)c->metrics_period_ms);

    /* ---- Reporting / publish filtering ---- */
    {
        printf("[SYS] filter: telem=%s",
               c->reporting.telemetry_filter_enabled ? "ON" : "OFF");
        if (c->reporting.telemetry_filter_enabled)
        {
            printf(" db=vin:%lu/vout:%lu/iin:%lu/iout:%lu/temp:%lu/pout:%lu hb=%lums",
                   (unsigned long)c->reporting.deadband_vin_mV,
                   (unsigned long)c->reporting.deadband_vout_mV,
                   (unsigned long)c->reporting.deadband_iin_mA,
                   (unsigned long)c->reporting.deadband_iout_mA,
                   (unsigned long)c->reporting.deadband_temp1_mC,
                   (unsigned long)c->reporting.deadband_pout_mW,
                   (unsigned long)c->reporting.telemetry_heartbeat_ms);
        }
        printf(" | status=%s",
               c->reporting.status_filter_enabled ? "ON" : "OFF");
        if (c->reporting.status_filter_enabled)
        {
            printf(" on-change init=%s hb=%lums",
                   c->reporting.status_emit_initial ? "emit" : "suppress",
                   (unsigned long)c->reporting.status_heartbeat_ms);
        }
        printf("\n");
    }

    printf("[SYS] devices: %u\n", (unsigned)c->num_devices);
    for (uint8_t i = 0; i < c->num_devices; i++) {
        const device_cfg_t *d = &c->devices[i];
        printf("[SYS]   [%u] 0x%02X \"%s\"  poll=%lums  status=%lums",
               (unsigned)i,
               (unsigned)d->addr_7bit,
               d->label,
               (unsigned long)d->poll_period_ms,
               (unsigned long)d->status_period_ms);
        /* Print per-device overrides only when they deviate from globals */
        bool any_ovr = d->telemetry_heartbeat_ms | d->status_heartbeat_ms |
                       d->deadband_vin_mV       | d->deadband_vout_mV    |
                       d->deadband_iin_mA       | d->deadband_iout_mA    |
                       d->deadband_temp1_mC     | d->deadband_pout_mW;
        if (any_ovr)
        {
            printf("  ovr:");
            if (d->deadband_vin_mV)   printf(" vin:%lu", (unsigned long)d->deadband_vin_mV);
            if (d->deadband_vout_mV)  printf(" vout:%lu", (unsigned long)d->deadband_vout_mV);
            if (d->deadband_iin_mA)   printf(" iin:%lu", (unsigned long)d->deadband_iin_mA);
            if (d->deadband_iout_mA)  printf(" iout:%lu", (unsigned long)d->deadband_iout_mA);
            if (d->deadband_temp1_mC) printf(" temp:%lu", (unsigned long)d->deadband_temp1_mC);
            if (d->deadband_pout_mW)  printf(" pout:%lu", (unsigned long)d->deadband_pout_mW);
            if (d->telemetry_heartbeat_ms) printf(" thb:%lu", (unsigned long)d->telemetry_heartbeat_ms);
            if (d->status_heartbeat_ms)    printf(" shb:%lu", (unsigned long)d->status_heartbeat_ms);
        }
        printf("\n");
    }
    printf("\n");
}
