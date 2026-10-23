/*
 * netctl.c - on-demand network track for the NE102 capture mode.
 *
 * Capture-track policy (PRD 3.2 / user review 2026-10-10):
 *  - STA ONLY: the track exists to reach the user's router and upload;
 *    AP mode makes no sense here, so wifi_mode is forced to STA at runtime
 *    (not persisted - config mode keeps whatever the user set).
 *  - No credentials -> no network: if the user never configured a router
 *    (sys_cfgs.ssid empty) the whole track is skipped and the capture flow
 *    falls back to the store-only path; CPU1 is never started for it.
 *
 * pre_main no longer starts CPU1 (system0.c, NE102_DEFER_CPU1): its boot
 * (incl. RF calibration) plus sys_wifi_init()/sys_network_init() are only
 * paid when a policy actually needs the network. __init is an empty macro
 * here, so the sys_* init functions stay callable at runtime.
 */
#include <basic_include.h>
#include <lib/atcmd/libatcmd.h>
#include "syscfg.h"
#include "netctl.h"
#include "../../app/app_lowpwr_camera.h"

/* project/txw82xApp: started in main()/wifi.c/network.c (__init = no-op) */
extern void sys_start_cpu1(uint32 run_addr);
extern void sys_wifi_init(void);
extern void sys_network_init(void);

static uint8_t cpu1_started = 0;
static uint8_t track_state = NE102_NETCTL_OFF;
static uint8_t force_track = 0; /* pwrtest override: STA track from idle */

void netctl_set_force_track(uint8_t en)
{
    force_track = en;
}

void netctl_set_boot_sta(void)
{
    /* pwrtest build: the idle path boots through the regular main()
     * branch (wifi/ble/network at __init, the manual's reference flow).
     * Mark the track as up so netctl_start() never re-runs sys_wifi_init
     * (double init hard-faults). */
    cpu1_started = 1;
    track_state = NE102_NETCTL_STA;
}

uint8_t netctl_cpu1_started(void)
{
    return cpu1_started;
}

enum ne102_netctl_state netctl_state(void)
{
    return (enum ne102_netctl_state)track_state;
}

void ne102_cpu1_ensure(void)
{
    if (!cpu1_started) {
        os_printf(KERN_INFO "NE102: starting CPU1 (deferred)...\n");
        sys_start_cpu1(0x10001000); /* blocks until CoreSetting->cpu1_ready */
        cpu1_started = 1;
        os_printf(KERN_INFO "NE102: CPU1 ready @%ums\n", ne102_boot_elapsed_ms());
    }
}

/* 1 = the user has actually configured a router (STA credentials present).
 *
 * Caveat found on board (2026-10-10): the precompiled wifi lib writes the
 * auto-generated AP name ("82X_" + last 3 MAC bytes, uppercase hex) into
 * sys_cfgs.ssid and persists it on every config-mode boot, so a plain
 * emptiness check would see "configured" on a factory-fresh device. Treat
 * the auto AP name (prefix match + our own MAC when known) as absent. */
uint8_t netctl_sta_cred_present(void)
{
    const char *ssid = (const char *)sys_cfgs.ssid;
    uint32_t len = os_strlen(ssid);

    if (len == 0) {
        return 0;
    }
    if (os_strncmp(ssid, "82X_", 4) == 0) {
        if (len == 10 && (sys_cfgs.mac[3] | sys_cfgs.mac[4] | sys_cfgs.mac[5])) {
            char auto_name[11];
            os_snprintf(auto_name, sizeof(auto_name), "82X_%02X%02X%02X",
                        sys_cfgs.mac[3], sys_cfgs.mac[4], sys_cfgs.mac[5]);
            if (os_strcmp(ssid, auto_name) == 0) {
                return 0; /* our own auto AP name, not a user router */
            }
        } else if (len == 8 || len == 10) {
            return 0; /* auto-name length without a usable MAC to compare */
        }
    }
    /* psk is derived from ssid+passwd by the AT/web setters; a set ssid
     * with an empty passwd is still tried (open network). */
    return 1;
}

int32_t netctl_start(void)
{
    if (track_state == NE102_NETCTL_STA) {
        return RET_OK; /* idempotent */
    }

    /* No credentials -> skip the network entirely. */
    if (!netctl_sta_cred_present()) {
        track_state = NE102_NETCTL_NO_CRED;
        os_printf(KERN_WARNING "NE102: no router configured (no user STA "
                  "credentials), skip network track\n");
        return NE102_NETCTL_NO_CRED_RET;
    }

    ne102_cpu1_ensure();

    if (track_state == NE102_NETCTL_SYS) {
        /* wifi stack already up via netctl_start_infra() (pwrtest sleep
         * path): only switch the ifaces to APSTA - re-running
         * sys_wifi_init() a second time is untested and crashes. */
        extern void syscfg_flush(int32 reset);
        extern void dhcpd_stop(void);
        sys_cfgs.wifi_mode = WIFI_MODE_APSTA;
        syscfg_flush(1);
        sys_cfgs.dhcpc_en = 1;
        dhcpd_stop();
        track_state = NE102_NETCTL_STA;
        os_printf(KERN_INFO "NE102: network track up (APSTA upgrade from "
                  "infra, ssid=%s)\n", (const char *)sys_cfgs.ssid);
        return RET_OK;
    }

    /* Replicate the CONFIG mode WiFi init sequence exactly (the only
     * sequence proven to work on this platform). Pure STA mode has a
     * WiFi TX failure (WPA_AUTHENTICATING loop); CONFIG mode's
     * AP→APSTA switch works. We boot WiFi in AP mode first, then do
     * the APSTA switch, exactly like CONFIG mode's main.c path. */
    {
        extern void sys_wifi_init(void);
        extern void sys_ble_init(void);
        extern void sys_network_init(void);
        extern void syscfg_flush(int32 reset);
        extern void dhcpd_stop(void);

        /* Step 1: start as AP mode (same as CONFIG boot default) */
        sys_cfgs.wifi_mode = WIFI_MODE_AP;
        sys_wifi_init();
        sys_ble_init();
        sys_network_init();
        os_sleep_ms(100); /* let the AP start beaconing */

        /* Step 2: APSTA switch (same as CONFIG mode's usr_app_init) */
        sys_cfgs.wifi_mode = WIFI_MODE_APSTA;
        syscfg_flush(1);

        /* Step 3: stop dhcpd (AP DHCP server conflicts with STA DHCP
         * client on the single netif) and start DHCP on the STA */
        sys_cfgs.dhcpc_en = 1;
        dhcpd_stop();
    }

    track_state = NE102_NETCTL_STA;
    os_printf(KERN_INFO "NE102: network track up @%ums (APSTA, ssid=%s)\n",
              ne102_boot_elapsed_ms(), (const char *)sys_cfgs.ssid);
    return RET_OK;
}

int32_t netctl_start_infra(void)
{
    /*
     * Credential-free network bring-up for the pwrtest sleep path:
     * system_sleep() needs the dsleep context created by
     * bgn_dsleep_init(), which runs inside sys_wifi_init() - so CPU1 +
     * the wifi stack must be up even when the test sleeps WITHOUT any
     * upload. AP mode with no associated clients is the cheapest proven
     * entry (netctl_start's dance step 1). A later netctl_start() with
     * credentials upgrades to APSTA without re-running sys_wifi_init().
     */
    if (track_state == NE102_NETCTL_STA || track_state == NE102_NETCTL_SYS) {
        return RET_OK;
    }

    ne102_cpu1_ensure();
    {
        extern void sys_wifi_init(void);
        extern void sys_ble_init(void);
        extern void sys_network_init(void);
        sys_cfgs.wifi_mode = WIFI_MODE_AP;
        sys_wifi_init();
        sys_ble_init();
        sys_network_init();
    }
    track_state = NE102_NETCTL_SYS;
    os_printf(KERN_INFO "NE102: net infra up (AP mode, no-cred sleep path)\n");
    return RET_OK;
}

