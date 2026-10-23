/*
 * mqttka.h - WiFi keepalive-sleep MQTT test firmware component.
 *
 * Bench firmware per the low-power guide ("TXW82x 低功耗开发指南" mode 1):
 *   - STA boot, router credentials over AT
 *   - long-lived MQTT session (kawaii client, default broker.emqx.io:1883,
 *     client id / pub / sub topics derived from the board MAC)
 *   - SRAM_WIFI keepalive sleep entered over AT or an MQTT command; the
 *     device wakes from the sleep breakpoint (NO reboot wake), services
 *     the MQTT heartbeat, processes subscribed commands and re-sleeps
 *   - LED blinks while awake, dark while sleeping; SD and camera stay
 *     unpowered for the whole test
 *
 * Built only when NE102_MQTTKA is 1 (custom/app/ne102_config.h); the
 * product boot flow is bypassed in that build.
 */
#ifndef __NE102_MQTTKA_H__
#define __NE102_MQTTKA_H__

#include <stdint.h>

/* Pull the product config so NE102_MQTTKA is visible in EVERY translation
 * unit that includes this header (same reason as pwrtest.h: files outside
 * project/txw82xApp do not include sys_config.h). */
#include "../../app/ne102_config.h"

/* Idle-path entry (app_lowpwr_camera_init, config-mode boot). */
void ne102_mqttka_init(void);

/* AT command handlers (registered in project/txw82xApp/atcmd.c). */
int32_t ne102_atcmd_kassid(const char *cmd, char *argv[], uint32_t argc);
int32_t ne102_atcmd_kamq(const char *cmd, char *argv[], uint32_t argc);
int32_t ne102_atcmd_kago(const char *cmd, char *argv[], uint32_t argc);
int32_t ne102_atcmd_kaslp(const char *cmd, char *argv[], uint32_t argc);
int32_t ne102_atcmd_kastop(const char *cmd, char *argv[], uint32_t argc);
int32_t ne102_atcmd_kapub(const char *cmd, char *argv[], uint32_t argc);
int32_t ne102_atcmd_kapk(const char *cmd, char *argv[], uint32_t argc);
int32_t ne102_atcmd_kapkt(const char *cmd, char *argv[], uint32_t argc);
int32_t ne102_atcmd_kadbg(const char *cmd, char *argv[], uint32_t argc);
int32_t ne102_atcmd_kashot(const char *cmd, char *argv[], uint32_t argc);
int32_t ne102_atcmd_kacap(const char *cmd, char *argv[], uint32_t argc);
int32_t ne102_atcmd_kaq(const char *cmd, char *argv[], uint32_t argc);

#endif /* __NE102_MQTTKA_H__ */
