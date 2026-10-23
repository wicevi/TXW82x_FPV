/*
 * netctl.h - NE102 network track lifecycle (the second, on-demand track).
 *
 * With NE102_DEFER_CPU1 the network stack (CPU1 + RF calibration + wifi
 * + lwip) is NOT started at boot in the fast-capture mode. This module
 * starts it on demand (upload policy / AT+NENET):
 *   netctl_start(): sys_start_cpu1() (blocks until cpu1_ready) ->
 *                   sys_wifi_init() -> sys_network_init()
 * Config mode and wifi test mode call ne102_cpu1_ensure() instead:
 * main() then continues with the regular sys_wifi_init() path.
 */
#ifndef __NE102_NETCTL_H__
#define __NE102_NETCTL_H__

#include <stdint.h>

/* Capture-track network policy (PRD 3.2 / user review 2026-10-10):
 * STA ONLY - wifi_mode is forced to STA at runtime (not persisted);
 * if the user never configured a router the whole track is skipped
 * (store-only fallback) and CPU1 is not started for it. "Configured"
 * means sys_cfgs.ssid holds a real user SSID - the auto-generated AP
 * name the wifi lib writes there on config boots does NOT count. */
enum ne102_netctl_state {
    NE102_NETCTL_OFF = 0,    /* not started yet */
    NE102_NETCTL_NO_CRED = 1,/* skipped: no user STA credentials */
    NE102_NETCTL_SYS = 2,    /* served by the config-mode system path */
    NE102_NETCTL_STA = 3,    /* capture track brought STA up */
};

#define NE102_NETCTL_NO_CRED_RET (1) /* netctl_start() return for NO_CRED */

/* Start the network track for the capture flow (idempotent).
 * Returns RET_OK, or NE102_NETCTL_NO_CRED_RET when no router credentials
 * are configured - the caller must then take the store-only path.
 * Wifi mode is forced to STA at runtime (NOT persisted); connection and
 * DHCP complete asynchronously (phase 3 waits on wifi/dhcpc events). */
int32_t netctl_start(void);

/* Credential-free bring-up (CPU1 + AP-mode wifi/ble/lwip) for sleep-only
 * tests: system_sleep() needs bgn_dsleep_init() inside sys_wifi_init().
 * A later netctl_start() upgrades to APSTA without re-running wifi init. */
int32_t netctl_start_infra(void);

/* Current track state (see enum). */
enum ne102_netctl_state netctl_state(void);

/* 1 = the user has actually configured a router (real STA credentials in
 * sys_cfgs.ssid, not the auto AP name). Also used by the config-mode
 * AP+STA switch in main.c. */
uint8_t netctl_sta_cred_present(void);

/* 1 once sys_start_cpu1() returned (CPU1 firmware running). */
uint8_t netctl_cpu1_started(void);

/* Boot-time helper for config / wifi-test paths: bring CPU1 up (once). */
void ne102_cpu1_ensure(void);

/* Test-firmware override: when set, netctl_start() brings the STA track
 * up even though the boot routed to config mode (the pwrtest build boots
 * the idle path but still needs the capture-style connect sequence). */
void netctl_set_force_track(uint8_t en);

/* Test-firmware helper: mark the network track as already up (wifi was
 * initialized at boot in the regular main() branch). */
void netctl_set_boot_sta(void);

#endif /* __NE102_NETCTL_H__ */
