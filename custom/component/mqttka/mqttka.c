/*
 * mqttka.c - MQTT keepalive-sleep test firmware (bench build, NE102_MQTTKA).
 *
 * Exercises the low-power guide's mode 1 (SYSTEM_SLEEP_TYPE_SRAM_WIFI):
 * the WiFi association is parked by the dsleep low-power program while the
 * application checkpoints; on wake the code RESUMES at the system_sleep()
 * call site - a reboot-style wake is a bug here, not a design option.
 *
 * Test flow (all states observable on the LED, PC0):
 *   idle awake     1 Hz blink, STA auto-associated at boot (router
 *                  credentials via AT+KASSID, persisted in syscfg)
 *   AT+KAGO        wait for the STA IP, connect MQTT, subscribe the
 *                  command topic, publish an "online" event
 *   AT+KASLP       enter the keepalive sleep loop (MQTT must be up):
 *                    - sleep segment = keepalive * 3/4 by default (the
 *                      broker drops a silent client after 1.5x keepalive)
 *                    - wake (timer / inbound TCP via wkdet) resumes at the
 *                      breakpoint; LED fast-blinks during the awake window
 *                    - each wake: print the message that arrived (if any),
 *                      publish a heartbeat event (also proves the TCP
 *                      session survived the sleep), then re-sleep
 *                    - MQTT payload "wake" exits the loop and publishes a
 *                      "wake" event; "sleep" keeps sleeping; anything else
 *                      is printed only
 *                    - heartbeat failure -> link wait + full MQTT
 *                      reconnect + retry; 3 consecutive failed cycles end
 *                      the loop with the error LED
 *
 * Time bookkeeping: systick freezes during sleep and the framework
 * compensates systime on resume. Every wake verifies that by comparing
 * the jiffies delta against the requested sleep ("tchk" in AT+KAQ); no
 * code in this component depends on jiffies advancing across a sleep.
 *
 * Camera and SD stay unpowered the whole test (rails cut at init, and the
 * lowPower module table re-asserts the cut on every suspend).
 */
#include <basic_include.h>
#include <osal/sleep.h>
#include <osal/semaphore.h>
#include <osal/task.h>
#include <lib/atcmd/libatcmd.h>
#include <lib/umac/ieee80211.h>
#include <lib/lmac/lmac_dsleep.h>
#include <lib/common/dsleepdata.h>
#include <lowPower_app.h>
#include <pin_param.h>
#include <chip/txw82x/pmu.h>
#include <lwip/netif.h>
#include <lwip/sockets.h>
#include <lwip/etharp.h>
#include <mqttclient.h>
#include <fdb_syscfg.h>
#include "syscfg.h"
#include "mqttka.h"
#include "../netctl/netctl.h"
#include "../led/led.h"
#include <lib/heap/sysheap.h>
#include "../capture/capture.h"

#define KA_CFG_KEY      "ne102_mqttka"
#define KA_TRACE_KEY    "ne102_katrace"
#define KA_DBG_KEY      "ne102_kadbg"
#define KA_TASK_STACK   16384 /* mqtt_connect runs on this task (proven size) */
#define KA_IP_WAIT_MS   50000 /* router (re)association + DHCP tolerance */
#define KA_SETTLE_DATA_MS 2000  /* data-wake only: dispatch grace for the
                                 * inbound packet that woke us (it is still
                                 * in the socket buffer; the kawaii handler
                                 * needs scheduler time to latch ka_cmd).
                                 * 2026-10-16 board trace: a capture cmd
                                 * took >400ms to dispatch, the settle gave
                                 * up, an hb leaked into the capture wake
                                 * and the cmd only fired pre-park */
#define KA_QUIET_MS     3000  /* pre-park drain: consume in-flight broker
                               * traffic while awake instead of paying a
                               * spurious data wake ~2s after parking */
#define KA_ACK_WAIT_MS  2000  /* QoS1 PUBACK window - session liveness probe */
#define KA_MAX_FAILS    3     /* consecutive failed heartbeat cycles -> exit */

/* ---- persisted MQTT test config (FlashDB KV) ---- */

#define KA_CFG_MAGIC 0x4B413101 /* "KA" v1 */
#define KA_CFG_VER   1

struct ka_cfg {
    uint32_t magic;
    uint8_t  ver;
    char     host[64];
    uint16_t port;
    char     client_id[40];
    char     sub_topic[64];
    char     pub_topic[64];
    char     photo_topic[64]; /* NE301-format photo stream topic */
    uint16_t keepalive_s; /* MQTT keepalive, default 120 */
    uint32_t sleep_ms;    /* sleep segment, 0 = auto (keepalive * 3/4) */
};

static struct ka_cfg ka;
static uint8_t ka_cfg_loaded;

/* ---- runtime state ---- */

enum ka_req {
    KA_REQ_NONE = 0,
    KA_REQ_GO,   /* bring the MQTT session up */
    KA_REQ_SLP,  /* enter the keepalive sleep loop */
    KA_REQ_CAP,  /* one capture + photo publish cycle */
};

enum ka_cmd {
    KA_CMD_NONE = 0,
    KA_CMD_SLEEP,
    KA_CMD_WAKE,
    KA_CMD_CAPTURE,
};

static os_semaphore_t ka_sema;
static os_semaphore_t ka_rx_sema; /* rx-dispatched signal (handler -> loop) */
static volatile uint8_t ka_req;
static volatile uint8_t ka_cmd;       /* set by the MQTT message handler */
static volatile uint8_t stop_req;
static volatile uint8_t in_sleep_loop;
static volatile uint8_t mq_connected;
static mqtt_client_t *mq;

static uint16_t ka_cycles_req;        /* transient: KASLP <cycles> arg */
static uint8_t ka_sleep_type = SYSTEM_SLEEP_TYPE_SRAM_WIFI; /* KASLP <type> */

/* counters for AT+KAQ */
static uint32_t cnt_cycles, cnt_hb_ok, cnt_hb_fail, cnt_msgs, cnt_reconn;
static uint32_t last_wake_rsn;
static int32_t  last_hb_rc = -999;
static int32_t  last_tchk_ms;         /* measured sleep - requested sleep */
/* awake-window telemetry: resume -> hb PUBACK confirmed (avg/max), the
 * direct measurement of "is there a TX black window after resume" */
static uint32_t w2a_sum, w2a_max, w2a_cnt;
/* first-wake reason capture: the reason latch lags on the first park of a
 * session; the raw code is preserved here before later wakes overwrite it
 * (KAQ "fw=") and shown per-event as "rsn" */
static uint8_t first_wake_rsn;
static uint8_t first_wake_valid;

static uint32_t ms_now(void)
{
    return (uint32_t)os_jiffies_to_msecs(os_jiffies());
}

static const char *wk_name(uint8_t r)
{
    switch (r) {
    case DSLEEP_WK_REASON_TIMER:
    case DSLEEP_WK_REASON_TIM:
    case DSLEEP_WK_REASON_BC_TIM:
    case DSLEEP_WK_REASON_RTC_ALARM:
        return "timer";
    case DSLEEP_WK_REASON_STA_DATA:
    case DSLEEP_WK_REASON_WK_DATA:
        return "remote";
    case DSLEEP_WK_REASON_IO0:
    case DSLEEP_WK_REASON_IO1:
    case DSLEEP_WK_REASON_IO2:
    case DSLEEP_WK_REASON_IO3:
    case DSLEEP_WK_REASON_IO4:
    case DSLEEP_WK_REASON_IO5:
    case DSLEEP_WK_REASON_PIR:
        return "io";
    case DSLEEP_WK_REASON_BEACON_LOST:
    case DSLEEP_WK_REASON_AP_ERROR:
    case DSLEEP_WK_REASON_HB_TIMEOUT:
    case DSLEEP_WK_REASON_PS_DISCONNECT:
    case DSLEEP_WK_REASON_STA_ERROR:
    case DSLEEP_WK_REASON_SLEEPED_STA_ERROR:
    case DSLEEP_WK_REASON_WEAK_SIGNAL:
        return "link-lost";
    case DSLEEP_WK_REASON_DHCP_RENEW:
        return "dhcp-renew";
    case DSLEEP_WK_REASON_KEY_UPDATE:
        return "key-update";
    case DSLEEP_WK_REASON_MCLR:
    case DSLEEP_WK_REASON_WDT:
        return "reset";
    default:
        return "other";
    }
}

/* ---- always-on sleep trace (diagnoses a reboot-style wake) ----
 *
 * Lives in the SYSTEM_SLEEPDATA_ID_USER always-on block: survives the
 * sleep AND survives a chip reset (a forbidden reboot-style wake shows up
 * as a leftover phase at boot). Boot prints right after a dsleep-wake
 * reset are UNRELIABLE on this board (UART print loss), so the leftover
 * is also copied to RAM (AT+KAQ) and persisted to KV (survives power
 * cycles) - the evidence outlives the UART.
 */
#define KA_TRACE_MAGIC 0x5A4C
#define KA_TRACE_VER   2
struct ka_trace {
    uint16_t magic;
    uint8_t  ver;
    uint8_t  phase;     /* 1=entering sleep 2=resumed 3=hb ok 4=reconnected
                         * 5=loop exited 6=hb failed 7=loop aborted */
    uint8_t  wake_rsn;  /* raw wake reason of the last wake */
    uint8_t  rsv;
    uint32_t seq;       /* sleep cycle index */
    uint32_t exp_ms;    /* requested segment */
    uint32_t ms_before; /* jiffies around the sleep call */
    uint32_t ms_after;
    int32_t  hb_rc;
    int32_t  sr;        /* last system_sleep() return code */
};
static struct ka_trace *ka_trace;
static struct ka_trace ka_boot_trace; /* leftover from the previous boot */
static uint8_t ka_boot_trace_valid;

static void ka_trace_set(uint8_t phase)
{
    if (ka_trace) {
        ka_trace->phase = phase;
        /* KV persistence at session-boundary milestones only: 1=entering
         * a park (survives even a power cut mid-park), 5=clean loop exit,
         * 6/7=failures. The healthy per-cycle phases stay in the
         * always-on RAM copy only - no flash writes inside the awake
         * window (w2a telemetry, 2026-10-15). */
        if (phase == 1 || phase == 5 || phase == 6 || phase == 7) {
            syscfg_set_binary(KA_TRACE_KEY, ka_trace, sizeof(*ka_trace));
        }
    }
}

/* BOARD FINDING (2026-10-15/16, board 433ba8): sys_wakeup_reason() reports
 * the PREVIOUS park's wake reason - the current wake's code is written back
 * only later in the resume path (dataset: data-wake cycle1 reported rsn=0,
 * timer cycle2 reported rsn=8=WK_DATA; 2026-10-16: a capture data-wake
 * directly after a timer park still read rsn=1=TIMER, which mis-gated the
 * dispatch grace and leaked an hb ahead of the capture). rsn==0 therefore
 * means "no earlier wake to report", labeled "na". Do NOT branch on the
 * raw rsn for protocol decisions; kept raw+honest for logs, vendor lag
 * recorded for FAE. */
static const char *wk_label(uint8_t r, uint32_t slept_ms, uint32_t seg)
{
    (void)slept_ms;
    (void)seg;
    if (r == 0) {
        return "na";
    }
    return wk_name(r);
}

/* ---- config persistence ---- */

static void ka_cfg_defaults(void)
{
    char mac6[8];

    os_memset(&ka, 0, sizeof(ka));
    ka.magic = KA_CFG_MAGIC;
    ka.ver = KA_CFG_VER;
    os_strcpy(ka.host, "broker.emqx.io");
    ka.port = 1883;
    ka.keepalive_s = 120;
    ka.sleep_ms = 0; /* auto segment */

    /* MAC-derived identity: ne102_<last3>, sub ne102/<last3>/cmd,
     * pub ne102/<last3>/evt (sys_cfgs.mac is filled before usr_app_init) */
    os_snprintf(mac6, sizeof(mac6), "%02x%02x%02x",
                sys_cfgs.mac[3], sys_cfgs.mac[4], sys_cfgs.mac[5]);
    os_snprintf(ka.client_id, sizeof(ka.client_id), "ne102_%s", mac6);
    os_snprintf(ka.sub_topic, sizeof(ka.sub_topic), "ne102/%s/cmd", mac6);
    os_snprintf(ka.pub_topic, sizeof(ka.pub_topic), "ne102/%s/evt", mac6);
    os_snprintf(ka.photo_topic, sizeof(ka.photo_topic), "ne102/%s/photo", mac6);
}

static void ka_cfg_load(void)
{
    struct ka_cfg blob;

    if (ka_cfg_loaded) {
        return;
    }
    ka_cfg_defaults();
    if (syscfg_get_binary(KA_CFG_KEY, &blob, sizeof(blob))
            >= (int32_t)sizeof(struct ka_cfg)
        && blob.magic == KA_CFG_MAGIC) {
        /* field-by-field copy: a new default (e.g. MAC-derived topic)
         * must survive a cfg written by an older build missing it */
        if (blob.host[0])       os_strcpy(ka.host, blob.host);
        if (blob.port)          ka.port = blob.port;
        if (blob.client_id[0])  os_strcpy(ka.client_id, blob.client_id);
        if (blob.sub_topic[0])  os_strcpy(ka.sub_topic, blob.sub_topic);
        if (blob.pub_topic[0])  os_strcpy(ka.pub_topic, blob.pub_topic);
        if (blob.keepalive_s)   ka.keepalive_s = blob.keepalive_s;
        ka.sleep_ms = blob.sleep_ms;
    }
    ka_cfg_loaded = 1;
}

static void ka_cfg_save(void)
{
    if (!ka_cfg_loaded) {
        return;
    }
    syscfg_set_binary(KA_CFG_KEY, &ka, sizeof(ka));
}

/* ---- link helpers ---- */

static uint8_t ip_ready(struct netif *nif)
{
    /* 192.168.1.1 (0x0101A8C0) is our own AP-mode address; any other
     * non-zero lease means the STA side is connected (pwrtest helper) */
    return nif && nif->ip_addr.addr != 0 && nif->ip_addr.addr != 0x0101A8C0;
}

static uint8_t ka_wait_ip(uint32_t tmo_ms)
{
    uint32_t waited = 0;

    while (waited < tmo_ms) {
        mcu_watchdog_feed();
        if (ip_ready(netif_find("w0"))) {
            return 1;
        }
        os_sleep_ms(100);
        waited += 100;
    }
    return ip_ready(netif_find("w0"));
}

/* ---- MQTT ---- */

static int32_t ka_pub_evt(const char *event, const char *extra_kv)
{
    char buf[176];
    mqtt_message_t msg;

    if (!mq || !mq_connected) {
        return RET_ERR;
    }
    os_memset(&msg, 0, sizeof(msg));
    if (extra_kv) {
        os_snprintf(buf, sizeof(buf),
                    "{\"event\":\"%s\",\"seq\":%u,\"up_s\":%u,%s}",
                    event, cnt_hb_ok + 1, ms_now() / 1000, extra_kv);
    } else {
        os_snprintf(buf, sizeof(buf),
                    "{\"event\":\"%s\",\"seq\":%u,\"up_s\":%u}",
                    event, cnt_hb_ok + 1, ms_now() / 1000);
    }
    msg.payload = buf;
    msg.payloadlen = os_strlen(buf);
    msg.qos = QOS0;
    last_hb_rc = mqtt_publish(mq, ka.pub_topic, &msg);
    return last_hb_rc == 0 ? RET_OK : RET_ERR;
}

/* heartbeat variant: QoS1 publish carrying the wake reason (the UART wake
 * prints are lost on this board, so the evt stream is the observability) */
static int32_t ka_pub_evt_wk(const char *event, const char *extra_kv,
                             const char *wk, uint8_t rsn)
{
    char buf[192];
    mqtt_message_t msg;

    if (!mq || !mq_connected) {
        return RET_ERR;
    }
    os_memset(&msg, 0, sizeof(msg));
    if (extra_kv) {
        os_snprintf(buf, sizeof(buf),
                    "{\"event\":\"%s\",\"seq\":%u,\"up_s\":%u,\"wk\":\"%s\",\"rsn\":%u,%s}",
                    event, cnt_hb_ok + 1, ms_now() / 1000, wk, rsn, extra_kv);
    } else {
        os_snprintf(buf, sizeof(buf),
                    "{\"event\":\"%s\",\"seq\":%u,\"up_s\":%u,\"wk\":\"%s\",\"rsn\":%u}",
                    event, cnt_hb_ok + 1, ms_now() / 1000, wk, rsn);
    }
    msg.payload = buf;
    msg.payloadlen = os_strlen(buf);
    msg.qos = QOS1;
    last_hb_rc = mqtt_publish(mq, ka.pub_topic, &msg);
    return last_hb_rc == 0 ? RET_OK : RET_ERR;
}

/* wait until the QoS1 PUBACK clears the pending-ack list (the kawaii yield
 * thread processes it; a dead session never acks and kawaii just resends
 * into the void) */
static int32_t ka_wait_puback(uint32_t wait_ms)
{
    uint32_t w = 0;

    if (!mq) {
        return RET_ERR;
    }
    while (w < wait_ms) {
        mcu_watchdog_feed();
        if (mq->mqtt_ack_handler_number == 0) {
            return RET_OK;
        }
        os_sleep_ms(50);
        w += 50;
    }
    return mq->mqtt_ack_handler_number == 0 ? RET_OK : RET_ERR;
}

/* MQTT message handler - runs on the kawaii yield thread. Keep it minimal:
 * print the payload, latch a one-word command. Long work happens on the
 * ka task (the handler posts REQ_SLP via the semaphore for the awake
 * "sleep" command). */
static void ka_msg_handler(void *client, message_data_t *msg)
{
    char payload[40];
    uint32_t len = msg->message->payloadlen;
    uint32_t i;

    (void)client;
    if (len >= sizeof(payload)) {
        len = sizeof(payload) - 1;
    }
    os_memcpy(payload, msg->message->payload, len);
    payload[len] = 0;
    /* trim trailing whitespace/CR/LF so raw terminal input also works */
    for (i = len; i > 0; i--) {
        char c = payload[i - 1];
        if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
            break;
        }
        payload[i - 1] = 0;
    }

    cnt_msgs++;
    os_printf(KERN_INFO "MQTTKA: rx topic=%s payload=%s\n", msg->topic_name, payload);

    if (os_strcasecmp(payload, "sleep") == 0) {
        ka_cmd = KA_CMD_SLEEP;
        if (!in_sleep_loop && !ka_req) {
            ka_req = KA_REQ_SLP;
            os_sema_up(&ka_sema);
        }
    } else if (os_strcasecmp(payload, "wake") == 0
               || os_strcasecmp(payload, "work") == 0) {
        ka_cmd = KA_CMD_WAKE;
    } else if (os_strcasecmp(payload, "capture") == 0
               || os_strcasecmp(payload, "shot") == 0) {
        ka_cmd = KA_CMD_CAPTURE;
        if (!in_sleep_loop && !ka_req) {
            ka_req = KA_REQ_CAP;
            os_sema_up(&ka_sema);
        }
    }
    /* anything else: printed only */
    /* signal the sleep loop: ka_cmd is latched - a data wake waiting in
     * the dispatch grace can decide NOW instead of burning the timeout */
    os_sema_up(&ka_rx_sema);
}

static int32_t ka_mqtt_connect(void)
{
    char portstr[8];

    if (mq) {
        /* deterministic teardown before a fresh lease: stops the yield
         * thread so it cannot race the reconnect */
        mq_connected = 0;
        mqtt_disconnect(mq);
        mqtt_release(mq);
        mq = NULL;
    }

    mq = mqtt_lease();
    if (!mq) {
        os_printf(KERN_INFO "MQTTKA: mqtt_lease failed\n");
        return RET_ERR;
    }
    mqtt_set_host(mq, ka.host);
    os_snprintf(portstr, sizeof(portstr), "%u", ka.port);
    mqtt_set_port(mq, portstr);
    mqtt_set_client_id(mq, ka.client_id);
    mqtt_set_clean_session(mq, 1);
    mqtt_set_keep_alive_interval(mq, ka.keepalive_s);
    mqtt_set_cmd_timeout(mq, 8000);
    mqtt_set_read_buf_size(mq, 1024);
    /* one client carries everything: 96KB write buffer (PSRAM via the
     * platform_memory >=16KB rule) holds a full NE301 photo JSON
     * (~48KB base64 for a 36KB jpeg) - no second client needed */
    mqtt_set_write_buf_size(mq, 96 * 1024);
    mqtt_set_thread_stack_size(mq, 4096);

    if (mqtt_connect(mq) != 0) {
        os_printf(KERN_INFO "MQTTKA: connect %s:%u failed\n",
                  ka.host, (uint32_t)ka.port);
        mqtt_release(mq);
        mq = NULL;
        return RET_ERR;
    }
    if (mqtt_subscribe(mq, ka.sub_topic, QOS0, ka_msg_handler) != 0) {
        os_printf(KERN_INFO "MQTTKA: subscribe %s failed\n", ka.sub_topic);
        mqtt_disconnect(mq);
        mqtt_release(mq);
        mq = NULL;
        return RET_ERR;
    }
    mq_connected = 1;
    os_printf(KERN_INFO "MQTTKA: mqtt up %s:%u id=%s sub=%s pub=%s ka=%us\n",
              ka.host, (uint32_t)ka.port, ka.client_id,
              ka.sub_topic, ka.pub_topic, (uint32_t)ka.keepalive_s);
    ka_pub_evt("online", "\"via\":\"at\"");
    return RET_OK;
}

/* heartbeat failed: the TCP session died. The SRAM_WIFI association should
 * still be alive, so normally a plain MQTT reconnect is enough; if the
 * link itself dropped (link-lost wake) wait for the stack to re-associate
 * first. */
static int32_t ka_recover(void)
{
    mq_connected = 0;
    cnt_reconn++;
    if (!ka_wait_ip(30000)) {
        os_printf(KERN_INFO "MQTTKA: no IP during recovery, giving up\n");
        return RET_ERR;
    }
    return ka_mqtt_connect();
}

/* ---- FAE diagnostics (2026-10-17): wake-reason sources ----
 * Disassembly: sys_wakeup_reason() returns *(bgn_dsleep + 0x3a9) - the
 * SAME byte the (never-executed) wakeup_hook receives as wk_reason.
 * The observed one-park lag therefore means the current wake's code is
 * written into that byte LATE in the resume path; these probes build
 * the timeline (pre-park / +0ms / +300ms / end-of-settle) for FAE. */
/* bgn_dsleep (struct dsleep_priv *, lmac_dsleep.h) is the dcfg struct
 * the park program reads; offsets verified by disassembly of the final
 * image (2026-10-16): +0x3a9 wk_reason byte, +0x41c wakeup_hook, +0x414
 * usr_wkdet_cb, +0x428 dsleep_ready_hook */
static int32_t ka_park_wakeup_hook(uint8_t wk_reason);
static int32_t ka_wkdet(void *priv, uint8_t *data, uint32_t len);

static uint8_t ka_dcfg_wkbyte(void)
{
    return bgn_dsleep
        ? *(volatile uint8_t *)((uint8_t *)bgn_dsleep + 0x3a9) : 0xFF;
}

/* LMAC wake flags at dcfg+0x3a0 (disassembly of dsleep_wakeup_pending_check,
 * libdsleep.a): bit 1 = WK_DATA, set by the WiFi MAC hardware DURING the
 * park - readable the instant system_sleep() returns, well before the
 * library's pending_check runs (~150ms later on this silicon) and writes
 * the decoded reason into +0x3a9. This is our zero-lag data-wake oracle. */
static uint8_t ka_dcfg_wkflags(void)
{
    return bgn_dsleep
        ? *(volatile uint8_t *)((uint8_t *)bgn_dsleep + 0x3a0) : 0;
}

/* framework sleep callback: RESUME actions deliver the LIVE per-wake
 * reason (board-verified 2026-10-19: timer cycles report 1, the capture
 * data-wake cycle reported 8=WK_DATA, matching command arrival exactly;
 * the official demo has this very field commented out -
 * lowPower_app.c:38). Latched here for telemetry + FAE evidence. */
static volatile uint8_t ka_cb_wkreason;
static volatile uint8_t ka_cb_fresh;

/* Resume semaphore (2026-10-22): system_sleep() is an asynchronous
 * dispatch, and the wake side used to bridge the async gap with timed
 * polls (see ka_sleep_loop). ka_sleepcb RESUME posts this; the loop
 * blocks on it instead - counting semantics digest the "cb fires
 * before/after the task regains control" race both ways. */
static os_semaphore_t ka_resume_sema;

static int32_t ka_sleepcb(uint16_t type, struct sys_sleepcb_param *args,
                          void *priv)
{
    (void)type;
    (void)priv;
    if (args->action == SYS_SLEEPCB_ACTION_RESUME) {
        ka_cb_wkreason = args->resume.wkreason;
        ka_cb_fresh = 1;
        os_sema_up(&ka_resume_sema);
    }
    return RET_OK;
}

/* one-shot registration proof: the hook pointers really landed in the
 * dcfg struct the park program reads (rules out a usage error on our
 * side - FAE package evidence) */
static void ka_dump_hook_reg(void)
{
    static uint8_t dumped;

    if (dumped || !bgn_dsleep) {
        return;
    }
    dumped = 1;
    /* offsets disassembly-verified: +0x410 usr_wkdet_cb, +0x414 priv,
     * +0x41c dsleep_wakeup_hook, +0x428 dsleep_ready_hook */
    os_printf(KERN_INFO "MQTTKA: dcfg=%p hook@0x41c=%p (want %p) "
              "wkdet@0x410=%p (want %p) ready@0x428=%p\n",
              bgn_dsleep,
              *(void **)((uint8_t *)bgn_dsleep + 0x41c),
              (void *)ka_park_wakeup_hook,
              *(void **)((uint8_t *)bgn_dsleep + 0x410),
              (void *)ka_wkdet,
              *(void **)((uint8_t *)bgn_dsleep + 0x428));
}

/* ---- wake-packet filter (runs in the low-power program context) ---- */

static uint32_t ka_local_ip; /* cached net-order w0 address, 0 = unknown */

/* THIS-wake data-pending latch: wkdet writes WK_DATA when it APPROVES a
 * packet (that approval IS the wake), the park-entry sentinel 0xFF
 * surviving means no data was approved = timer/spurious wake. Written
 * from the low-power program context: one byte store only. Must live in
 * the always-on dsleep data section to survive parks. Note the
 * dsleep_set_wakeup_hook callback never fires on plain mode-1 parks
 * (board finding 2026-10-16: sentinel survived every wake) - the wkdet
 * path is the only proven live signal. */
__dsleep_data static uint8_t ka_wk_latch; /* 0xFF = no wkdet-approved data */

static int32_t ka_wkdet(void *priv, uint8_t *data, uint32_t len)
{
    /* data = IPv4 header (same convention as lowPower_app user_check_data).
     * Wake ONLY on payload-bearing TCP addressed to us: a broker publish
     * (or its retransmission) carries bytes. Bare ACKs - TCP keepalive
     * probes from the broker/NAT - carry no payload and must NOT burn a
     * wake (board finding 2026-10-15: ~4 spurious data wakes / 6 min at
     * 11-21s spacing with zero inbound MQTT messages). */
    uint32_t dst, ihl, tot, thl;

    (void)priv;
    if (len >= 24 && (data[0] >> 4) == 4 && data[9] == 0x06) {
        if (ka_local_ip == 0) {
            ka_wk_latch = DSLEEP_WK_REASON_WK_DATA;
            return 1;
        }
        os_memcpy(&dst, data + 16, 4);
        if (dst != ka_local_ip) {
            return 0;
        }
        ihl = (data[0] & 0x0F) * 4;
        if (ihl < 20 || len < ihl + 20) {
            return 0;
        }
        tot = ((uint32_t)data[2] << 8) | data[3];
        thl = ((uint32_t)data[ihl + 12] >> 4) * 4;
        if (tot > ihl + thl) {
            ka_wk_latch = DSLEEP_WK_REASON_WK_DATA;
            return 1; /* TCP segment with payload */
        }
    }
    return 0;
}

static uint32_t ka_segment_ms(void);
static uint8_t ka_park_active(void);

/* ---- park-internal heartbeat (feasibility of guide option 3) ----
 *
 * Classic keepalive wakes the application every segment (window =
 * broker RTT + scheduling, measured 465ms avg to broker.emqx.io). This
 * mode keeps the application PARKED across timer expiries:
 *   - dsleep_set_wakeup_hook: TIMER -> DSLEEP_ACTION_SLEEP_CONTINUE
 *     (re-park, no app resume; data/IO wakes fall through untouched)
 *   - dsleep_set_ready_hook: called by the low-power program when the
 *     suppressed wake's RF is ready - sends a pre-built UDP heartbeat
 *     via dsleep_tx_ether(). No libc in that context: manual payload
 *     formatting and checksum math only.
 * The MQTT session itself is NOT kept alive by a UDP probe (that needs
 * a crafted TCP PINGREQ with live seq/ack - follow-up); this test
 * proves the park-internal TX channel, its timing and its stability,
 * observable on a PC listener while the LED stays dark.
 */
static uint32_t cnt_park_hb;   /* total park heartbeats (RAM, AT+KAQ) */
static uint16_t last_park_n;   /* sent in the last park stretch */
static uint8_t ka_cap_skip = 3; /* AE-settle frames dropped before the shot */
static uint16_t ka_cap_w;       /* 0,0 = sensor native resolution */
static uint16_t ka_cap_h;
static uint32_t ka_cap_len;     /* JPEG governor target bytes, 0 = default */
static volatile uint8_t cap_pending;   /* capture cmd arrived during the loop */
static uint32_t cnt_photos;     /* photos published (AT+KAQ) */
static int32_t last_cap_rc = -999; /* last capture-cycle result */
static int8_t last_pub_stage;        /* photo publish: 0=ok 1=alloc 2=connect 3=publish */
static uint32_t last_cap_len;      /* stash length at the last cycle (0 = stash empty) */
static int32_t last_sram_free;       /* SRAM heap free AFTER the last cycle */
static int32_t cap_sram_delta;       /* free(before) - free(after) of the last cycle */

#if NE102_KA_PARK_HB

#define KA_PARK_CFG_KEY "ne102_mqttka_pk"
#define KA_PARK_MAGIC   0x50414B31 /* "PAK1" */
#define KA_PARK_PL_MAX  24         /* "ne102_park_hb seq=65535\r\n" */
#define KA_PARK_FRAME   (14 + 20 + 8 + KA_PARK_PL_MAX)

struct ka_park_cfg {
    uint32_t magic;
    char     ip[16];   /* target IP, "255.255.255.255" = L2 broadcast */
    uint16_t port;
    uint8_t  budget;   /* suppressed timer wakes before a full resume */
    uint8_t  en;       /* runtime arm switch (AT+KAPK) */
};

static struct ka_park_cfg pk = {
    .magic = KA_PARK_MAGIC,
    .ip = "255.255.255.255",
    .port = 9999,
    .budget = 20,
    .en = 0,
};

/* state touched from the low-power program: must live in the
 * always-on dsleep data section to survive parks */
/* full park frame: [0..21] lmac_bgn_hw_frm_info, [22..45] 802.11 data
 * header (PROTECTED), [46..53] LLC/SNAP+ethertype, [54..] IP payload.
 * Built like dsleep_tx_ether's internal layout BUT with fc bit14 set so
 * lmac_bgn_non_os_tx takes its CCMP path (reverse-engineered 2026-10-15:
 * tx_ether hardcodes fc=0x0108 -> cleartext -> AP drops it on WPA2) */
#define KA_PARK_WIFI_FRAME (22 + 26 + 8 + 20 + 8 + KA_PARK_PL_MAX)
__dsleep_data static struct {
    uint8_t  frame[KA_PARK_WIFI_FRAME];
    uint16_t len;
    uint16_t ip_id;
    uint16_t sent;
    uint8_t  budget;    /* heartbeats before the watchdog segment ends */
    uint8_t  active;
    uint64_t hb_ticks;  /* RC ticks between park heartbeats */
    uint64_t tmr_last;  /* RC counter at the last park heartbeat */
    int32_t  tx_rc;     /* last dsleep_tx_ether return code (diagnostics:
                         * the park program prints its own TX errors only
                         * with dsleep debug enabled) */
    uint32_t tx_calls;  /* total tx_ether invocations this loop */
} ka_park;

/* ones-complement internet checksum over the 20-byte IPv4 header */
/* internal low-power-program TX entry (global symbol in libdsleep.a):
 * takes the 22B hw_frm_info + 802.11 header frame and runs the real TX
 * path INCLUDING per-STA CCMP when fc bit14 (protected) is set.
 * bgn_dsleep comes from lmac_dsleep.h; +1084 = the associated BSSID
 * (proven by the tx_ether disassembly's addr1 source). */
extern int32_t lmac_bgn_non_os_tx(void *frm, uint32_t len);
#define KA_PARK_BSSID_OFF 1084
#define KA_PARK_DW_FRM 22   /* lmac_bgn_hw_frm_info size */
#define KA_PARK_DW_IP  (KA_PARK_DW_FRM + 26 + 8)  /* QoS hdr(26) + LLC */
#define KA_PARK_DW_PL  (KA_PARK_DW_IP + 20 + 8)   /* payload at 82 */

static uint16_t ka_park_ip_csum(const uint8_t *ip)
{
    uint32_t sum = 0;
    int i;

    for (i = 0; i < 20; i += 2) {
        if (i != 10) { /* skip the checksum field itself */
            sum += ((uint32_t)ip[i] << 8) | ip[i + 1];
        }
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFF);
}

/* decimal formatter without libc (low-power program context) */
static int ka_park_put_dec(char *p, uint32_t v)
{
    char tmp[10];
    int n = 0, i;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 10);
    for (i = 0; i < n; i++) {
        p[i] = tmp[n - 1 - i];
    }
    return n;
}

/* build the frame template (application context, once per loop) */
static int32_t ka_park_frame_build(void)
{
    struct netif *nif = netif_find("w0");
    ip4_addr_t dst;
    uint8_t *f = ka_park.frame;         /* [0..21] hw_frm_info        */
    uint8_t *wh = f + KA_PARK_DW_FRM;   /* [22..45] 802.11 data hdr   */
    uint8_t *llc = wh + 26;             /* [48..55] LLC/SNAP + etype  */
    uint8_t *ip = f + KA_PARK_DW_IP;    /* [54..73] IPv4              */
    uint8_t *udp = ip + 20;
    uint8_t *pl = f + KA_PARK_DW_PL;
    uint8_t dstmac[6];

    if (!nif || !ip_ready(nif) || !bgn_dsleep) {
        return RET_ERR;
    }
    if (!ip4addr_aton(pk.ip, &dst)) {
        return RET_ERR;
    }

    /* final L3 next hop MAC (broadcast / ARP neighbour / gateway) -
     * it becomes 802.11 addr3; the router routes + NATs onward */
    os_memset(dstmac, 0xFF, 6);
    if (dst.addr != 0xFFFFFFFF) {
        struct eth_addr *mac = NULL;
        ip4_addr_t *dip = NULL;
        ip4_addr_t *arp_ip = &dst;

        if (!ip4_addr_netcmp(&dst, &nif->ip_addr, &nif->netmask)
            && nif->gw.addr) {
            arp_ip = &nif->gw; /* off-subnet: ARP the default gateway */
        }
        if (etharp_find_addr(nif, arp_ip, &mac, &dip) >= 0 && mac) {
            os_memcpy(dstmac, mac, 6);
        } else {
            os_printf(KERN_INFO "MQTTKA: park-hb: no ARP entry, "
                      "using broadcast\n");
        }
    }

    /* hw_frm_info: zeroed except byte6 bits[9:6]=2 (copied from the
     * tx_ether prologue - the frame class the park TX engine expects) */
    os_memset(f, 0, KA_PARK_DW_FRM);
    f[6] = 2 << 6;

    /* 802.11 data header - THE FIX: fc bit14 (Protected) set so the
     * lmac CCMP path encrypts with the per-STA key (tx_ether hard-
     * codes this bit to 0, sending cleartext that WPA2 APs discard) */
    /* fc = 0x4188: QoS Data(subtype 8) + ToDS + Protected - copied from
     * the vendor's own park TX template "tcp_br_frm" in libdsleep.a
     * (plain-Data 0x4108 leaves the QoS Control field missing -> 2-byte
     * shift -> AP cannot parse/decrypt -> discard) */
    wh[0] = 0x88;          /* fc low: QoS data */
    wh[1] = 0x41;          /* fc high: ToDS(bit8) + Protected(bit14) */
    wh[2] = wh[3] = 0;     /* duration */
    os_memcpy(wh + 4, (uint8_t *)bgn_dsleep + KA_PARK_BSSID_OFF, 6);
    os_memcpy(wh + 10, sys_cfgs.mac, 6);   /* addr2 = us */
    os_memcpy(wh + 16, dstmac, 6);         /* addr3 = L3 next hop */
    wh[22] = wh[23] = 0;   /* seq: assigned by non_os_tx from HW regs */
    wh[24] = wh[25] = 0;   /* QoS Control (tid 0, no A-MSDU) */

    /* LLC/SNAP (rfc1042) + ethertype IPv4 */
    llc[0] = llc[1] = llc[2] = 0xAA;
    llc[3] = llc[4] = llc[5] = 0x00;
    llc[6] = 0x08;
    llc[7] = 0x00;

    /* IPv4 + UDP (checksum patched per-send in the ready hook) */
    os_memset(ip, 0, 20);
    ip[0] = 0x45;
    ip[2] = (20 + 8 + KA_PARK_PL_MAX) >> 8;
    ip[3] = (20 + 8 + KA_PARK_PL_MAX) & 0xFF;
    ip[4] = 0x12;
    ip[5] = 0x34;
    ip[8] = 64;  /* ttl */
    ip[9] = 17;  /* UDP */
    os_memcpy(ip + 12, &nif->ip_addr.addr, 4);
    os_memcpy(ip + 16, &dst.addr, 4);

    udp[0] = 0xC3; udp[1] = 0x50;          /* sport 50000 */
    udp[2] = pk.port >> 8;
    udp[3] = pk.port & 0xFF;
    udp[4] = (8 + KA_PARK_PL_MAX) >> 8;
    udp[5] = (8 + KA_PARK_PL_MAX) & 0xFF;
    udp[6] = udp[7] = 0;                   /* checksum 0 (IPv4-legal) */

    ka_park.len = KA_PARK_DW_PL + KA_PARK_PL_MAX;
    ka_park.ip_id = 0x1234;
    os_memset(pl, 0, KA_PARK_PL_MAX);
    return RET_OK;
}

/* low-power program context: RF ready after a suppressed timer wake */
static void ka_park_ready_hook(void)
{
    uint8_t *ip = ka_park.frame + KA_PARK_DW_IP;
    uint8_t *pl = ka_park.frame + KA_PARK_DW_PL;
    char *s = (char *)pl;
    uint16_t csum;
    int i = 0;

    if (!ka_park.active || ka_park.len == 0) {
        return;
    }
    /* the lib calls this hook on EVERY internal wake window (~1s DTIM
     * cadence: 85 calls per 90s park, measured 2026-10-15) - throttle
     * with the RC timer so one heartbeat leaves per interval */
    if (ka_park.hb_ticks
        && (pmu_tmrao_get() - ka_park.tmr_last) < ka_park.hb_ticks) {
        return;
    }
    ka_park.tmr_last = pmu_tmrao_get();
    /* payload "ne102_park_hb seq=N" written without libc */
    const char tag[] = "ne102_park_hb seq=";
    for (i = 0; tag[i] && i < KA_PARK_PL_MAX - 6; i++) {
        s[i] = tag[i];
    }
    i += ka_park_put_dec(s + i, ka_park.sent + 1);

    /* fresh IP id + checksum */
    ip[4] = ka_park.ip_id >> 8;
    ip[5] = ka_park.ip_id & 0xFF;
    ka_park.ip_id++;
    ip[10] = ip[11] = 0;
    csum = ka_park_ip_csum(ip);
    ip[10] = csum >> 8;
    ip[11] = csum & 0xFF;

    /* direct internal TX with the PROTECTED 802.11 header: goes through
     * the per-STA CCMP path (tx_ether would send it cleartext) */
    ka_park.tx_rc = lmac_bgn_non_os_tx(ka_park.frame, ka_park.len);
    ka_park.tx_calls++;
    ka_park.sent++;
}

/* low-power program context: also latch THIS wake's reason (if this hook
 * ever fires - on plain mode-1 parks it does NOT, board finding
 * 2026-10-16), then pass the wake through untouched.
 *
 * Disassembly (2026-10-16, linked image): the wakeup_hook's SOLE call
 * site is dsleep_wakeup (.ds_text @0x04003914), entered from
 * wakeup_Handler (@0x04003bd4) which the park program installs into the
 * AON wake vector (0x20000004) before its only doze. Our mode-1 parks
 * never execute that path (sentinel survives; arming a no-op ready_hook
 * and/or the vendor heartbeat_int does NOT route it). enter/exit hooks
 * have NO call sites at all (dead code); rx_hook lives in dsleep0_rx_hdl
 * and wkdet in dsleep_wkdata_check (app context - the path that DOES
 * run for us). Kept registered for telemetry in case a future lib/FAE
 * enables the park wake path.
 *
 * Written from lmac context: one byte store only, no libc, no PMU
 * register access (BOARD RULE 2026-10-15: never touch the PMU RC counter
 * around sleeps). */
static int32_t ka_park_wakeup_hook(uint8_t wk_reason)
{
    ka_wk_latch = wk_reason;
    if (!ka_park.active) {
        return DSLEEP_ACTION_DO_NOTHING;
    }
    /* board finding 2026-10-15: SLEEP_CONTINUE does NOT suppress the
     * mode-1 segment-timer expiry (the app resumed every 90s anyway).
     * The park segment itself is the watchdog (interval * budget, see
     * ka_sleep_loop) and heartbeats are RC-throttled in the ready hook -
     * any wake reaching here is a real one, let it through. */
    return DSLEEP_ACTION_DO_NOTHING;
}

static void ka_park_arm(void)
{
    if (!pk.en || !pk.port) {
        return;
    }
    if (ka_park_frame_build() != RET_OK) {
        os_printf(KERN_INFO "MQTTKA: park-hb frame build failed, "
                  "classic mode\n");
        return;
    }
    ka_park.sent = 0;
    ka_park.budget = pk.budget;
    ka_park.tmr_last = pmu_tmrao_get();
    {
        int32_t rc_hz = dsleep_get_rc_hz();

        /* heartbeat interval = one CLASSIC segment (ka_segment_ms);
         * the park segment itself becomes interval * budget = the
         * watchdog that forces a full app refresh */
        ka_park.hb_ticks = rc_hz > 0
            ? ((uint64_t)rc_hz * ka_segment_ms() / 1000) : 0;
    }
    ka_park.active = 1;
    dsleep_set_ready_hook(ka_park_ready_hook);
    /* also arm the VENDOR park heartbeat (RPC-registered official API,
     * pwrtest precedent): with AT+KADBG=1 its park-side prints reveal
     * whether the lib's own heartbeat path (tcp_br_frm/UALIVE machinery
     * seen in libdsleep strings) actually transmits, and what it needs */
    ieee80211_conf_set_heartbeat_int(WIFI_MODE_STA, ka_segment_ms() / 1000);
    os_printf(KERN_INFO "MQTTKA: park-hb armed: udp %s:%u every %ums (RC), "
              "watchdog wake after %u hbs + vendor hb_int=%us\n",
              pk.ip, (uint32_t)pk.port, ka_segment_ms(),
              (uint32_t)pk.budget, ka_segment_ms() / 1000);
}

static void ka_park_disarm(void)
{
    if (ka_park.active) {
        ka_park.active = 0;
        dsleep_set_ready_hook(NULL);
        cnt_park_hb += ka_park.sent;
        last_park_n = ka_park.sent;
        os_printf(KERN_INFO "MQTTKA: park-hb disarmed (sent %u this stretch, "
                  "%u total, tx_ether calls=%u rc=%d)\n",
                  (uint32_t)last_park_n, (uint32_t)cnt_park_hb,
                  ka_park.tx_calls, (int)ka_park.tx_rc);
        ka_park.sent = 0;
        ka_park.tx_calls = 0;
    }
}

#else /* !NE102_KA_PARK_HB */

static void ka_park_arm(void) {}
static void ka_park_disarm(void) {}

#endif /* NE102_KA_PARK_HB */

static uint8_t ka_park_active(void)
{
#if NE102_KA_PARK_HB
    return ka_park.active;
#else
    return 0;
#endif
}

/* ---- keepalive sleep loop (runs on the ka task) ---- */

static uint32_t ka_segment_ms(void)
{
    uint32_t seg = ka.sleep_ms
                   ? ka.sleep_ms
                   : (uint32_t)ka.keepalive_s * 1000 * 3 / 4;

    if (seg < 5000) {
        seg = 5000;
    }
    return seg;
}

static int32_t ka_enter_sleep(uint32_t seg_ms, uint32_t seq)
{
    struct system_sleep_param sp;

    os_memset(&sp, 0, sizeof(sp));
    sp.sleep_ms = seg_ms; /* timer wake always armed - never sleeps forever */

    dsleep_set_usr_wkdet_cb(NULL, ka_wkdet);
    /* reason latch: wkdet writes WK_DATA when it approves the wake packet;
     * the wakeup_hook (if it ever runs) overwrites with the live reason.
     * 0xFF surviving the park = unlabeled wake (timer/spurious OR a data
     * wake that took the DTIM buffered-delivery path - board finding
     * 2026-10-16: a capture data wake arrived unlabeled, rsn misread as
     * TIMER and wkdet was never consulted) */
    ka_wk_latch = 0xFF;
    ka_cb_fresh = 0; /* resume cbs may fire before the loop regains control */
    os_sema_eat(&ka_resume_sema);   /* drain a stale up() (e.g. a resume
                                     * that landed after an abandoned
                                     * wait) so the post-wake down() only
                                     * sees THIS park's resume */
    dsleep_set_wakeup_hook(ka_park_wakeup_hook);
    ka_dump_hook_reg(); /* one-shot registration proof (FAE evidence) */
    os_printf(KERN_INFO "MQTTKA: park: wkbyte_pre=%u\n", ka_dcfg_wkbyte());
    ne102_led_set(NE102_LED_OFF);

    if (ka_trace) {
        ka_trace->seq = seq;
        ka_trace->exp_ms = seg_ms;
        ka_trace->ms_before = ms_now();
        ka_trace->wake_rsn = 0;
        ka_trace->hb_rc = 0;
        ka_trace->sr = 0;
    }
    ka_trace_set(1);

    os_sleep_ms(5); /* let the log leave the UART FIFO (30ms was waste:
                     * 921600 baud drains 100 chars in ~1ms; 5ms margin) */
    {
        int32_t sr = system_sleep((SYSTEM_SLEEP_TYPE)ka_sleep_type, &sp);

        if (ka_trace) {
            ka_trace->sr = sr;
        }
        return sr;
    }
}

static void ka_sleep_loop(uint16_t cycles)
{
    uint32_t seg = ka_segment_ms();
    uint32_t done = 0, fails = 0;
    struct netif *nif;

    if (!mq_connected) {
        os_printf(KERN_INFO "MQTTKA: sleep refused, MQTT not connected "
                  "(AT+KAGO first)\n");
        ne102_led_set(NE102_LED_ERROR);
        return;
    }

    nif = netif_find("w0");
    ka_local_ip = nif ? nif->ip_addr.addr : 0;

    in_sleep_loop = 1;
    stop_req = 0;
    ka_cmd = KA_CMD_NONE;
    first_wake_valid = 0;
    os_printf(KERN_INFO "MQTTKA: sleep loop start (seg=%ums cycles=%u hb_ka=%us)\n",
              seg, (uint32_t)cycles, (uint32_t)ka.keepalive_s);
    ka_pub_evt("sleep", NULL);

    /* pre-park drain (board finding: the QoS1 command that started this
     * loop can straddle the park entry and data-wake the chip ~2s later;
     * consume it while awake instead) */
    {
        uint32_t drain = KA_QUIET_MS;
        while (drain && !stop_req && ka_cmd != KA_CMD_WAKE) {
            mcu_watchdog_feed();
            os_sleep_ms(drain > 100 ? 100 : drain);
            drain -= drain > 100 ? 100 : drain;
        }
    }

    ka_park_arm(); /* park-internal heartbeat when AT+KAPK armed it */
#if NE102_KA_PARK_HB
    if (ka_park.active) {
        /* park mode: the segment is only a watchdog - the app parks
         * through interval*budget while the ready hook sends hbs */
        seg *= (uint32_t)pk.budget;
        os_printf(KERN_INFO "MQTTKA: park-hb: watchdog segment %ums\n", seg);
    }
#endif
    /* drain stale rx signals (messages consumed while awake must not
     * short-circuit the first park's dispatch wait) */
    {
        uint8_t guard = 16;

        while (guard-- && os_sema_down(&ka_rx_sema, 1) > 0) {
        }
    }

    while (!stop_req && (cycles == 0 || done < cycles)) {
        /* fast exit: a wake command consumed while awake must not wait
         * behind a full park */
        if (ka_cmd == KA_CMD_WAKE) {
            os_printf(KERN_INFO "MQTTKA: remote wake cmd (pre-park) exit\n");
            ka_cmd = KA_CMD_NONE;
            ka_pub_evt("wake", "\"by\":\"remote\"");
            break;
        }
        /* keep a latched CAPTURE alive here: it may have arrived during
         * the hb-publish window (after the settle check); wiping it here
         * silently dropped remote captures (board finding 2026-10-16) */
        if (ka_cmd != KA_CMD_CAPTURE) {
            ka_cmd = KA_CMD_NONE;
        }
        /* pre-park CAPTURE check: a command latched LATE in the previous
         * wake window (handler dispatch > settle window) must fire NOW,
         * not after another full park (2026-10-16: alternating miss/fire
         * pattern - the missed command waited 90s for the next wake) */
        if (ka_cmd == KA_CMD_CAPTURE) {
            /* latched late (dispatch outlived the settle window): still
             * publish the capture event - the photo must be preceded by
             * its response event on every path (2026-10-16 board trace:
             * this branch fired bare, the observer only saw an hb) */
            os_printf(KERN_INFO "MQTTKA: latched capture - firing pre-park\n");
            ka_cmd = KA_CMD_NONE;
            cap_pending = 1;
            ka_pub_evt("capture", "\"by\":\"remote\",\"via\":\"prepark\"");
            break;
        }

        uint32_t t0 = ms_now();
        uint32_t measured;
        uint32_t tw;   /* resume timestamp: awake-window telemetry origin */
        uint8_t rsn;
        uint8_t live;       /* hook-latched reason (THIS wake) */
        uint8_t live_valid; /* 0 = hook did not run, live fell back to rsn */
        const char *wkl;
        int32_t sr;

        /* BOARD RULE (2026-10-15 lockup): do NOT touch the PMU RC counter
         * (pmu_tmrao_get) around system_sleep - reading TMRAOCNTL from the
         * app context corrupts the park program's own RC timing baseline
         * and the wake timer never fires (device parks forever; v3 board
         * lockup, classic mode, KV park cfg was off). */
        sr = ka_enter_sleep(seg, cnt_cycles + 1);

        /* ---- execution RESUMES here on wake (no reboot) ---- */
        tw = ms_now();
        ne102_led_set(NE102_LED_WEB_ACTIVE); /* fast blink: awake window */
        measured = tw - t0; /* jiffies: counts AWAKE time only (frozen in
                             * park) - NOT the true sleep duration */
        /* Authoritative wake reason: block on the resume semaphore
         * (ka_sleepcb RESUME posts it) instead of the old timed poll.
         * system_sleep() only dispatches the park RPC, so this spot
         * used to bridge the async gap with a 10ms/200ms window AND a
         * second ka_cb_fresh=0 that could discard an early cb - both
         * hazards vanish with counting semantics (an early up() leaves
         * the count at 1 for this down(); a late up() wakes us here).
         * The sleepcb remains the only wake signal that has never been
         * wrong (board finding 2026-10-20); the TIMER fallback stays
         * for the park-refused / no-resume corner. */
        {
            int32_t got = (sr == RET_OK)
                          && os_sema_down(&ka_resume_sema,
                                          (int32_t)seg + 60000) > 0;

            live = got ? ka_cb_wkreason : DSLEEP_WK_REASON_TIMER;
        }
        live_valid = 1;
        rsn = sys_wakeup_reason(); /* telemetry only */
        /* first-cycle reason lag: the raw code of THIS loop's first wake
         * is captured before anything can overwrite it (KAQ "fw=") */
        if (!first_wake_valid) {
            first_wake_rsn = rsn;
            first_wake_valid = 1;
        }
        last_wake_rsn = rsn;
        cnt_cycles++;
        done++;
        if (ka_trace) {
            ka_trace->ms_after = ms_now();
            ka_trace->wake_rsn = rsn;
        }
        ka_trace_set(2);
        wkl = wk_label(rsn, 0, seg);
        os_printf(KERN_INFO "MQTTKA: wake #%u rsn=%u(%s) live=%u(%s) "
                  "cb=%u +%ums sr=%d\n",
                  cnt_cycles, rsn, wkl, live, wk_label(live, 0, seg),
                  ka_cb_wkreason, measured, (int)sr);

        /* dispatch grace: WK_DATA waits for the command dispatch (sema-
         * driven, typically <1.4s); TIMER goes straight to hb - the
         * sleepcb has already confirmed the type with zero ambiguity */
        if (live > DSLEEP_WK_REASON_BC_TIM) {
            uint32_t t_left = KA_SETTLE_DATA_MS;

            while (t_left && !stop_req && ka_cmd != KA_CMD_WAKE) {
                uint32_t step = t_left > 50 ? 50 : t_left;

                /* os_sema_down: 1 = acquired, 0 = timeout (osal/semaphore.h)
                 * - this test was INVERTED until 2026-10-16: timeouts broke
                 * the grace after one 200ms step and real dispatches were
                 * ignored, so every slow-dispatch capture leaked an hb and
                 * fell through to the pre-park path */
                if (os_sema_down(&ka_rx_sema, (int32_t)step) > 0) {
                    break; /* message dispatched - ka_cmd is fresh */
                }
                t_left -= step;
                if (live_valid
                    && live >= DSLEEP_WK_REASON_TIMER
                    && live <= DSLEEP_WK_REASON_BC_TIM) {
                    break; /* fresh timer confirmed: nothing inbound */
                }
            }
        }

        if (ka_cmd == KA_CMD_CAPTURE) {
            /* capture exit: publish the capture event (NOT hb - the photo
             * itself is the response), run the cycle, re-sleep */
            os_printf(KERN_INFO "MQTTKA: remote capture cmd - exiting loop\n");
            ka_cmd = KA_CMD_NONE;
            cap_pending = 1; /* cycle + re-sleep after ka_sleep_loop */
            ka_pub_evt("capture", "\"by\":\"remote\"");
            break;
        }
        if (stop_req || ka_cmd == KA_CMD_WAKE) {
            os_printf(KERN_INFO "MQTTKA: %s requested exit\n",
                      stop_req ? "AT+KASTOP" : "remote wake cmd");
            ka_cmd = KA_CMD_NONE;
            ka_pub_evt("wake", "\"by\":\"remote\"");
            break;
        }
        ka_cmd = KA_CMD_NONE;

        /* heartbeat: NON-data wakes - it is the keepalive itself. Only a
         * wkdet-confirmed data wake skips it (its response is the command
         * output, or silence on stray traffic); every-wake hb polluted
         * the event stream (user ruling 2026-10-16). The sentinel case
         * (no data approved) MUST hb: it covers timer wakes whatever the
         * stale global reports - gating on the lagging rsn here once
         * skipped a timer keepalive and stretched the broker-side gap
         * past keepalive. QoS1 + PUBACK wait: a fire-and-forget publish
         * "succeeds" even into a dead session (board finding: outbound
         * data silently swallowed after a >=90s park), so only a broker
         * ACK proves the session survived */
        if (!live_valid || live <= DSLEEP_WK_REASON_BC_TIM) {
            const char *wkn = wk_label(live, 0, seg);
            int32_t rc = ka_pub_evt_wk("hb", NULL, wkn, live);
            if (rc == RET_OK && ka_wait_puback(KA_ACK_WAIT_MS) == RET_OK) {
                uint32_t d = ms_now() - tw;

                cnt_hb_ok++;
                fails = 0;
                w2a_sum += d;
                if (d > w2a_max) {
                    w2a_max = d;
                }
                w2a_cnt++;
                if (ka_trace) {
                    ka_trace->hb_rc = 0;
                }
                ka_trace_set(3);
                os_printf(KERN_INFO "MQTTKA: hb acked in %ums - session survived\n", d);
            } else {
                cnt_hb_fail++;
                fails++;
                ka_trace_set(6);
                os_printf(KERN_INFO "MQTTKA: hb unacked (%s), "
                          "reconnecting...\n", wkn);
                os_sleep_ms(300);
                if (ka_recover() == RET_OK
                    && ka_pub_evt_wk("hb", "\"rc\":1", wkn, live) == RET_OK
                    && ka_wait_puback(KA_ACK_WAIT_MS) == RET_OK) {
                    cnt_hb_ok++;
                    fails = 0;
                    ka_trace_set(4);
                }
            }
        }
        if (fails >= KA_MAX_FAILS) {
            os_printf(KERN_INFO "MQTTKA: %u failed cycles, aborting loop\n",
                      (uint32_t)KA_MAX_FAILS);
            ka_trace_set(7);
            ne102_led_set(NE102_LED_ERROR);
            ka_park_disarm();
            in_sleep_loop = 0;
            return;
        }

#if NE102_KA_PARK_HB
        /* full wake after a park stretch: report + re-arm the budget */
        if (ka_park.active && ka_park.sent) {
            char xb[32];

            cnt_park_hb += ka_park.sent;
            last_park_n = ka_park.sent;
            ka_park.sent = 0;
            ka_park.budget = pk.budget;
            os_snprintf(xb, sizeof(xb), "\"n\":%u,\"tot\":%u",
                        (uint32_t)last_park_n, (uint32_t)cnt_park_hb);
            os_printf(KERN_INFO "MQTTKA: park-hb stretch: %u this wake, %u total\n",
                      (uint32_t)last_park_n, (uint32_t)cnt_park_hb);
            ka_pub_evt("phb", xb);
        }
#endif
    }

    ka_park_disarm();
    in_sleep_loop = 0;
    ka_trace_set(5);
    if (cap_pending) {
        /* capture exit re-sleeps right after the photo cycle: the capture
         * event published at the break IS the wake response - an "awake"
         * here would be redundant (board ruling 2026-10-16) */
        os_printf(KERN_INFO "MQTTKA: sleep loop end (%u cycles, hb ok=%u fail=%u "
                  "msgs=%u reconn=%u) - capture pending\n",
                  done, cnt_hb_ok, cnt_hb_fail, cnt_msgs, cnt_reconn);
    } else {
        ne102_led_set(NE102_LED_AP_READY);
        ka_pub_evt("awake", NULL);
        os_printf(KERN_INFO "MQTTKA: sleep loop end (%u cycles, hb ok=%u fail=%u "
                  "msgs=%u reconn=%u)\n",
                  done, cnt_hb_ok, cnt_hb_fail, cnt_msgs, cnt_reconn);
    }
}

/* ---- ka task ---- */

/* ---- remote capture: NE301-format photo over MQTT ---- */

static const char KA_B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* NE301-compatible single publish (same shape as upload.c up_single):
 * {"metadata":{...},"ai_result":null,"image":"data:image/jpeg;base64,.."}
 * Buffers come from PSRAM; a 400KB jpeg becomes ~540KB base64. */
static int32_t ka_pub_photo(const uint8_t *jpg, uint32_t len)
{
    /* trim encoder padding after the EOI marker (up to 4 bytes) so the
     * NE301 receiver's strict SOI/EOI check passes */
    {
        uint32_t guard = 4;

        while (guard-- && len > 2
               && !(jpg[len - 2] == 0xFF && jpg[len - 1] == 0xD9)) {
            len--;
        }
    }
    uint32_t b64_max = ((len + 2) / 3) * 4 + 1;
    char *b64 = os_malloc_psram(b64_max + 16);
    /* NEW-schema envelope is ~700B (metadata + device_info + image_data +
     * encoding keys) - the old +320 sizing overflowed into neighbouring
     * PSRAM blocks (semaphore handles -> tcpip assert loop -> reset) */
    char *json = os_malloc_psram(b64_max + 1536);
    mqtt_message_t msg;
    uint32_t i, o = 0;
    int32_t rc = RET_ERR;

    last_pub_stage = 0;
    if (!b64 || !json) {
        last_pub_stage = 1;
        goto out;
    }
    for (i = 0; i + 2 < len; i += 3) {
        uint32_t v = ((uint32_t)jpg[i] << 16) | ((uint32_t)jpg[i + 1] << 8)
                     | jpg[i + 2];

        b64[o++] = KA_B64[(v >> 18) & 0x3f];
        b64[o++] = KA_B64[(v >> 12) & 0x3f];
        b64[o++] = KA_B64[(v >> 6) & 0x3f];
        b64[o++] = KA_B64[v & 0x3f];
    }
    if (i < len) {
        uint32_t v = (uint32_t)jpg[i] << 16;

        b64[o++] = KA_B64[(v >> 18) & 0x3f];
        b64[o++] = KA_B64[(v >> 12) & 0x3f];
        b64[o++] = (i + 1 < len) ? KA_B64[(jpg[i + 1] >> 4) & 0x30] : '=';
        b64[o++] = '=';
    }
    b64[o] = 0;

    /* EXACT NE301 schema (real capture sample, 2026-10-16): top-level
     * keys metadata/device_info/ai_result/image_data/encoding; the image
     * rides in image_data as a data URI and encoding is its own field.
     * The earlier "image" field name hung the NE301 receiver parser. */
    os_sprintf(json,
               "{\"metadata\":{\"image_id\":\"cam%02d_%u\","
               "\"timestamp\":%u,\"format\":\"jpeg\","
               "\"width\":1920,\"height\":1080,\"size\":%u,"
               "\"quality\":90,\"trigger_type\":\"remote\"},"
               "\"device_info\":{"
               "\"device_name\":\"NE102-%s\","
               "\"mac_address\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
               "\"serial_number\":\"SN00000000\","
               "\"hardware_version\":\"1.0.0\","
               "\"software_version\":\"0.1.0\","
               "\"power_supply_type\":\"full-power\","
               "\"battery_percent\":0,"
               "\"communication_type\":\"wifi\"},"
               "\"ai_result\":null,"
               "\"image_data\":\"data:image/jpeg;base64,%s\","
               "\"encoding\":\"base64\"}",
               1, (uint32_t)os_jiffies(), ms_now() / 1000, len,
               ka.client_id + 6,
               sys_cfgs.mac[0], sys_cfgs.mac[1], sys_cfgs.mac[2],
               sys_cfgs.mac[3], sys_cfgs.mac[4], sys_cfgs.mac[5],
               b64);

    if (!mq || !mq_connected) {
        last_pub_stage = 2;
        goto out;
    }
    os_memset(&msg, 0, sizeof(msg));
    msg.payload = json;
    msg.payloadlen = os_strlen(json);
    msg.qos = QOS0;
    rc = mqtt_publish(mq, ka.photo_topic, &msg) == 0 ? RET_OK : RET_ERR;
    if (rc != RET_OK) {
        last_pub_stage = 3;
    } else {
        cnt_photos++;
    }
out:
    if (b64) {
        os_free_psram(b64);
    }
    if (json) {
        os_free_psram(json);
    }
    return rc;
}

/* rails up -> init -> shot(1) -> NE301 publish -> deinit -> rails down.
 * Runs on the ka task (same envelope pwrtest cycles proved). */
static void ka_capture_cycle(void)
{
    struct ne102_capture_cfg cc = {
        .skip_frames = ka_cap_skip, .burst = 1,
        .width = ka_cap_w, .height = ka_cap_h,
        .target_len = ka_cap_len,
    };
    uint8_t *jpg = NULL;
    uint32_t len = 0;
    uint32_t t0 = ms_now();
    extern void app_camera_rails_power(unsigned char on);
    int32_t f0 = sysheap_freesize(&sram_heap);
    int32_t rc;

    ne102_led_set(NE102_LED_WEB_ACTIVE);
    os_printf(KERN_INFO "MQTTKA: capture cycle (skip=%u, photo topic %s)\n",
              (uint32_t)ka_cap_skip, ka.photo_topic);
    app_camera_rails_power(1);
    ne102_capture_set_cfg(&cc);
    rc = ne102_capture_init();
    if (rc == RET_OK) {
        rc = ne102_capture_shot(1);
    }
    if (rc == RET_OK) {
        rc = ne102_capture_wait_done(12000);
    }
    ne102_capture_get_last_jpg(&jpg, &len);
    last_cap_len = len;
    if (rc == RET_OK && jpg && len) {
        os_printf(KERN_INFO "MQTTKA: captured %uB in %ums\n", len, ms_now() - t0);
        if (ka_pub_photo(jpg, len) == RET_OK) {
            os_printf(KERN_INFO "MQTTKA: photo published in %ums total\n",
                      ms_now() - t0);
        } else {
            os_printf(KERN_INFO "MQTTKA: photo publish failed\n");
        }
    } else {
        os_printf(KERN_INFO "MQTTKA: capture failed (rc=%d len=%u)\n",
                  (int)rc, len);
    }
    if (rc != RET_OK || !jpg || !len) {
        last_cap_rc = rc; /* observable via AT+KASHOT? */
    }
    ne102_capture_deinit();   /* tears the pipeline + sensor rails down */
    app_camera_rails_power(0); /* belt&braces: bench stays dark after */
    last_sram_free = sysheap_freesize(&sram_heap);
    cap_sram_delta = f0 - last_sram_free;
    os_printf(KERN_INFO "MQTTKA: sram free %d -> %d (cycle cost %d)\n",
              (int)f0, (int)last_sram_free, (int)cap_sram_delta);
    ne102_led_set(NE102_LED_AP_READY);
}

static void ka_task(void *arg)
{
    for (;;) {
        uint8_t r;

        os_sema_down(&ka_sema, osWaitForever);
        r = ka_req;
        ka_req = KA_REQ_NONE;
        if (r == KA_REQ_GO) {
            ne102_led_set(NE102_LED_WEB_ACTIVE);
            if (!netctl_sta_cred_present()) {
                os_printf(KERN_INFO "MQTTKA: no router credentials "
                          "(AT+KASSID first)\n");
                ne102_led_set(NE102_LED_ERROR);
                continue;
            }
            if (!ka_wait_ip(KA_IP_WAIT_MS)) {
                os_printf(KERN_INFO "MQTTKA: no IP after %ums "
                          "(check router / AT+KASSID)\n", (uint32_t)KA_IP_WAIT_MS);
                ne102_led_set(NE102_LED_ERROR);
                continue;
            }
            if (mq_connected) {
                os_printf(KERN_INFO "MQTTKA: already connected\n");
            } else if (ka_mqtt_connect() != RET_OK) {
                ne102_led_set(NE102_LED_ERROR);
                continue;
            }
            ne102_led_set(NE102_LED_AP_READY); /* awake: 1 Hz blink */
        } else if (r == KA_REQ_SLP) {
            for (;;) {
                ka_sleep_loop(ka_cycles_req);
                if (!cap_pending) {
                    break;
                }
                /* remote capture during the loop: run one cycle, then
                 * re-enter the SAME sleep loop (product behavior) */
                cap_pending = 0;
                ka_capture_cycle();
            }
        } else if (r == KA_REQ_CAP) {
            if (!mq_connected) {
                os_printf(KERN_INFO "MQTTKA: capture needs MQTT "
                          "(AT+KAGO first)\n");
                continue;
            }
            ka_capture_cycle();
        }
    }
}

static void ka_post(uint8_t req)
{
    ka_req = req;
    os_sema_up(&ka_sema);
}


/* ---- lowPower module table: keep the bench rails dark ---- */

static int ka_rails_suspend(void *p1, void *p2, void *p3, void *p4)
{
    extern void app_camera_rails_power(unsigned char on);
    extern void ne102_board_tf_power(unsigned char on);
    uint32_t mask[6] = {0};
    uint8_t cam_en, tf_pwr;

    (void)p1; (void)p2; (void)p3; (void)p4;
    app_camera_rails_power(0);
    ne102_board_tf_power(0);
    /* hold the enable pins low through the sleep (unheld IOs get reset) */
    cam_en = MACRO_PIN(PIN_CAM_EN);
    tf_pwr = MACRO_PIN(PIN_TF_PWR_EN);
    if (cam_en != 0xFF) mask[cam_en / 16] |= BIT(cam_en % 16);
    if (tf_pwr != 0xFF) mask[tf_pwr / 16] |= BIT(tf_pwr % 16);
    dsleep_set_user_gioa(mask[0]);
    dsleep_set_user_giob(mask[1]);
    dsleep_set_user_gioc(mask[2]);
    dsleep_set_user_giod(mask[3]);
    return RET_OK;
}

static const struct lowPower_module_ops ka_modules[] = {
    { "ka_rails", ka_rails_suspend, NULL, {NULL, NULL, NULL, NULL} },
};

/* ---- init (idle path of the MQTTKA bench build) ---- */

void ne102_mqttka_init(void)
{
    /* FlashDB KV up (2026-10-22 board finding): nothing in any build
     * called flashdb_init(), so every syscfg_* KV access silently
     * failed "not inited" - KA cfg / park cfg / trace / dbg never
     * actually persisted. The KVDB partition is the fixed 0x7F0000
     * slot (fal_cfg.h), clear of the tail parameter sectors. */
    {
        extern int flashdb_init(void);

        if (!syscfg_get_dev() && flashdb_init() != 0) {
            os_printf(KERN_ERR "MQTTKA: flashdb_init failed - KV "
                      "persistence unavailable\n");
        }
    }
    os_sema_init(&ka_resume_sema, 0);
    ka_cfg_load();
#if NE102_KA_PARK_HB
    {
        struct ka_park_cfg kvcfg;

        if (syscfg_get_binary(KA_PARK_CFG_KEY, &kvcfg, sizeof(kvcfg))
                >= (int32_t)sizeof(kvcfg)
            && kvcfg.magic == KA_PARK_MAGIC) {
            os_memcpy(&pk, &kvcfg, sizeof(pk));
        }
    }
#endif

    /* silence the periodic dumps unless the tester enabled them via
     * AT+KADBG (persisted in KV - diagnostics must survive reboots) */
    {
        int32_t dbgv = 0;

        syscfg_get_binary(KA_DBG_KEY, &dbgv, sizeof(dbgv));
        dsleep_set_dbg_cfg(dbgv ? DSLEEP_DBG_EN_PRINT : 0);
    }
    /* bench diagnostics: gate at INFO so normal-flow prints pass without
     * abusing KERN_ERR; flip back to 3 when the debug phase ends */
    print_level(6);
    CoreSetting->print_level = 6;
    sys_status.dbg_umac = 0;
    sys_status.dbg_top = 0;
    sys_status.dbg_heap = 0;
    sys_status.dbg_cache = 0;
    sys_status.dbg_net = 0;
    sys_status.dbg_irq = 0;
    sys_status.dbg_lmac = 0;

    /* camera stays OFF, but pre-reserve the no-SD capture stash NOW:
     * the general PSRAM heap is fresh at boot; after capture_init carves
     * the 7.5MB AV pool a 512KB request can fail (stash stayed empty) */
    ne102_capture_stash_reserve(512 * 1024);
    /* this test never touches camera or SD: cut their rails now and keep
     * them cut across sleeps (module table above re-asserts on suspend) */
    {
        extern void app_camera_rails_power(unsigned char on);
        extern void ne102_board_tf_power(unsigned char on);
        app_camera_rails_power(0);
        ne102_board_tf_power(0);
    }

    ne102_led_init();
    ne102_led_set(NE102_LED_AP_READY); /* awake idle: 1 Hz blink */

    /* always-on trace: a leftover phase (other than a clean loop exit)
     * means the previous session ended by RESET - the wake path this
     * build must avoid. Boot prints are unreliable right after such a
     * reset, so keep the evidence in RAM (AT+KAQ) and in KV. */
    ka_trace = (struct ka_trace *)sys_sleepdata_request(
        SYSTEM_SLEEPDATA_ID_USER, sizeof(struct ka_trace));
    if (ka_trace) {
        if (ka_trace->magic == KA_TRACE_MAGIC && ka_trace->ver == KA_TRACE_VER
            && ka_trace->phase != 0 && ka_trace->phase != 5) {
            os_memcpy(&ka_boot_trace, ka_trace, sizeof(ka_boot_trace));
            ka_boot_trace_valid = 1;
            syscfg_set_binary(KA_TRACE_KEY, &ka_boot_trace,
                              sizeof(ka_boot_trace));
            os_printf(KERN_INFO "MQTTKA: last session ended by RESET: "
                      "phase=%u rsn=%u(%s) seq=%u exp=%ums hb_rc=%d sr=%d\n",
                      ka_boot_trace.phase, ka_boot_trace.wake_rsn,
                      wk_name(ka_boot_trace.wake_rsn), ka_boot_trace.seq,
                      ka_boot_trace.exp_ms, (int)ka_boot_trace.hb_rc,
                      (int)ka_boot_trace.sr);
        }
        os_memset(ka_trace, 0, sizeof(*ka_trace));
        ka_trace->magic = KA_TRACE_MAGIC;
        ka_trace->ver = KA_TRACE_VER;
    }
    /* KV fallback: the flash copy survives a POWER CYCLE (the always-on
     * copy does not - a silent reboot with wiped sleepdata is a power
     * event). phase 1 there = the previous session ended inside a park;
     * 6/7 = failure states; 5 = clean exit, not an incident. */
    if (!ka_boot_trace_valid) {
        struct ka_trace kv;

        if (syscfg_get_binary(KA_TRACE_KEY, &kv, sizeof(kv))
                >= (int32_t)sizeof(kv)
            && kv.magic == KA_TRACE_MAGIC && kv.ver == KA_TRACE_VER
            && (kv.phase == 1 || kv.phase == 6 || kv.phase == 7)) {
            os_memcpy(&ka_boot_trace, &kv, sizeof(ka_boot_trace));
            ka_boot_trace_valid = 1;
            os_printf(KERN_INFO "MQTTKA: KV trace: prior session ended "
                      "in phase %u, seq=%u sr=%d%s\n",
                      kv.phase, kv.seq, (int)kv.sr,
                      kv.phase == 1 ? " (park-time death or power cut)" : "");
        }
    }

    /* register the (minimal) lowpower module table so every sleep entry
     * runs the standard APP suspend/resume chain */
    lowpower_app_init(ka_modules, ARRAY_SIZE(ka_modules));
    /* FAE diag: capture what the framework's resume steps report */
    sys_register_sleepcb(ka_sleepcb, NULL);

    /* wifi came up on the regular main() boot branch (pure STA): mark the
     * netctl track so nothing re-runs sys_wifi_init (double init faults) */
    netctl_set_boot_sta();
    netctl_set_force_track(1);

    os_sema_init(&ka_sema, 0);
    os_sema_init(&ka_rx_sema, 0);
    os_task_create("mqttka", ka_task, NULL,
                   OS_TASK_PRIORITY_NORMAL, 0, NULL, KA_TASK_STACK);

    /* AT command registration (board finding 2026-10-22: the handlers
     * have always lived here as exported ne102_atcmd_* functions, but
     * the registration block was lost with the old orchestrator file -
     * every AT+KA* command was silently dead. Rebuilt from mqttka.h.) */
    atcmd_register("AT+KASSID", ne102_atcmd_kassid);
    atcmd_register("AT+KAMQ", ne102_atcmd_kamq);
    atcmd_register("AT+KAGO", ne102_atcmd_kago);
    atcmd_register("AT+KASLP", ne102_atcmd_kaslp);
    atcmd_register("AT+KASTOP", ne102_atcmd_kastop);
    atcmd_register("AT+KAPUB", ne102_atcmd_kapub);
    atcmd_register("AT+KASHOT", ne102_atcmd_kashot);
    atcmd_register("AT+KACAP", ne102_atcmd_kacap);
    atcmd_register("AT+KAPK", ne102_atcmd_kapk);
    atcmd_register("AT+KAPKT", ne102_atcmd_kapkt);
    atcmd_register("AT+KADBG", ne102_atcmd_kadbg);
    atcmd_register("AT+KAQ", ne102_atcmd_kaq);

    {
        struct netif *nif = netif_find("w0");
        os_printf(KERN_INFO "MQTTKA: idle. mac-derived id=%s sub=%s pub=%s\n"
                  "  AT+KASSID=<ssid>[,<key>]    router credentials\n"
                  "  AT+KAMQ=<host>[,<port>[,<id>[,<sub>[,<pub>[,<ka_s>]]]]]\n"
                  "  AT+KAGO                     connect MQTT (needs IP)\n"
                  "  AT+KASLP=<sleep_s>[,<n>]    keepalive sleep (0=auto)\n"
                  "  AT+KASTOP | AT+KAPUB=<text> | AT+KAQ\n"
                  "  cfg: %s:%u ka=%us seg=%ums cred=%u ip=%s\n",
                  ka.client_id, ka.sub_topic, ka.pub_topic,
                  ka.host, (uint32_t)ka.port, (uint32_t)ka.keepalive_s,
                  ka_segment_ms(), netctl_sta_cred_present(),
                  ip_ready(nif) ? ipaddr_ntoa(&nif->ip_addr) : "none");
    }
}

/* ---- AT handlers ---- */

int32_t ne102_atcmd_kassid(const char *cmd, char *argv[], uint32_t argc)
{
    if (argc == 1 && argv[0][0] == '?') {
        atcmd_resp("ssid=%s key=%s cred=%u", (const char *)sys_cfgs.ssid,
                   sys_cfgs.passwd[0] ? "<set>" : "<open>",
                   netctl_sta_cred_present());
        return 0;
    }
    if (argc < 1 || !argv[0][0]) {
        atcmd_resp("usage: AT+KASSID=<ssid>[,<key>] (empty key = open net)");
        return -1;
    }
    os_memset(sys_cfgs.bssid, 0, 6);
    os_strncpy((char *)sys_cfgs.ssid, argv[0], SSID_MAX_LEN);
    sys_cfgs.ssid[SSID_MAX_LEN] = 0;
    if (argc >= 2 && argv[1][0]) {
        os_strncpy((char *)sys_cfgs.passwd, argv[1], PASSWD_MAX_LEN);
        sys_cfgs.passwd[PASSWD_MAX_LEN] = 0;
    } else {
        sys_cfgs.passwd[0] = 0;
    }
    /* derive + push the PSK now (CPU1 is up on this boot path): a stale
     * psk makes the 4-way handshake fail in a deauth loop (board finding
     * 2026-10-13) */
    if (sys_cfgs.ssid[0] && sys_cfgs.passwd[0] && netctl_cpu1_started()) {
        uint8_t psk[32];
        wpa_passphrase(sys_cfgs.ssid, (char *)sys_cfgs.passwd, psk);
        os_memcpy(sys_cfgs.psk, psk, 32);
        ieee80211_conf_set_psk(WIFI_MODE_STA, psk);
    }
    syscfg_save();
    /* re-apply the STA config so the new credentials associate without a
     * reboot (same flush the proven APSTA switch uses) */
    {
        extern void syscfg_flush(int32 reset);
        syscfg_flush(1);
    }
    atcmd_resp("OK: ssid=%s %s, associating (AT+KAGO waits for the IP)",
               (const char *)sys_cfgs.ssid,
               sys_cfgs.passwd[0] ? "key set" : "open");
    return 0;
}

int32_t ne102_atcmd_kamq(const char *cmd, char *argv[], uint32_t argc)
{
    if (argc == 1 && argv[0][0] == '?') {
        atcmd_resp("host=%s port=%u id=%s sub=%s pub=%s ka=%us seg=%ums",
                   ka.host, (uint32_t)ka.port, ka.client_id, ka.sub_topic,
                   ka.pub_topic, (uint32_t)ka.keepalive_s, ka_segment_ms());
        return 0;
    }
    ka_cfg_load();
    if (argc >= 1 && argv[0][0]) {
        os_strncpy(ka.host, argv[0], sizeof(ka.host) - 1);
        ka.host[sizeof(ka.host) - 1] = 0;
    }
    if (argc >= 2 && argv[1][0]) {
        int32_t p = os_atoi(argv[1]);
        if (p < 1 || p > 65535) {
            atcmd_resp("ERR: bad port");
            return -1;
        }
        ka.port = (uint16_t)p;
    }
    if (argc >= 3 && argv[2][0]) {
        os_strncpy(ka.client_id, argv[2], sizeof(ka.client_id) - 1);
        ka.client_id[sizeof(ka.client_id) - 1] = 0;
    }
    if (argc >= 4 && argv[3][0]) {
        os_strncpy(ka.sub_topic, argv[3], sizeof(ka.sub_topic) - 1);
        ka.sub_topic[sizeof(ka.sub_topic) - 1] = 0;
    }
    if (argc >= 5 && argv[4][0]) {
        os_strncpy(ka.pub_topic, argv[4], sizeof(ka.pub_topic) - 1);
        ka.pub_topic[sizeof(ka.pub_topic) - 1] = 0;
    }
    if (argc >= 6 && argv[5][0]) {
        int32_t k = os_atoi(argv[5]);
        if (k < 10 || k > 600) {
            atcmd_resp("ERR: keepalive 10..600s");
            return -1;
        }
        ka.keepalive_s = (uint16_t)k;
    }
    ka_cfg_save();
    atcmd_resp("OK: %s:%u id=%s sub=%s pub=%s ka=%us (AT+KAGO to connect)",
               ka.host, (uint32_t)ka.port, ka.client_id, ka.sub_topic,
               ka.pub_topic, (uint32_t)ka.keepalive_s);
    return 0;
}

int32_t ne102_atcmd_kago(const char *cmd, char *argv[], uint32_t argc)
{
    (void)cmd;
    if (argc == 1 && argv[0][0] == '?') {
        atcmd_resp("mqtt=%u ip=%s", mq_connected,
                   ip_ready(netif_find("w0"))
                       ? ipaddr_ntoa(&netif_find("w0")->ip_addr) : "none");
        return 0;
    }
    if (in_sleep_loop) {
        atcmd_resp("ERR: sleep loop active (AT+KASTOP first)");
        return -1;
    }
    ka_post(KA_REQ_GO);
    atcmd_resp("OK: connect scheduled (watch LED / AT+KAQ)");
    return 0;
}

int32_t ne102_atcmd_kaslp(const char *cmd, char *argv[], uint32_t argc)
{
    int32_t secs, cycles, type;

    (void)cmd;
    if (argc == 1 && argv[0][0] == '?') {
        atcmd_resp("in_loop=%u mqtt=%u seg=%ums cycles_req=%u type=%u",
                   in_sleep_loop, mq_connected, ka_segment_ms(),
                   (uint32_t)ka_cycles_req, (uint32_t)ka_sleep_type);
        return 0;
    }
    if (!mq_connected) {
        atcmd_resp("ERR: MQTT not connected (AT+KAGO first)");
        return -1;
    }
    if (in_sleep_loop) {
        atcmd_resp("ERR: already sleeping (AT+KASTOP to exit)");
        return -1;
    }
    secs = (argc >= 1 && argv[0][0]) ? os_atoi(argv[0]) : 0;
    cycles = (argc >= 2 && argv[1][0]) ? os_atoi(argv[1]) : 0;
    type = (argc >= 3 && argv[2][0]) ? os_atoi(argv[2])
                                     : SYSTEM_SLEEP_TYPE_SRAM_WIFI;
    if (secs < 0 || secs > 3600 || cycles < 0 || cycles > 9999
        || type < SYSTEM_SLEEP_TYPE_SRAM_WIFI
        || type >= SYSTEM_SLEEP_TYPE_MAX) {
        atcmd_resp("ERR: bad args (sleep_s 0..3600, cycles 0=unlimited, "
                   "type 1=SRAM_WIFI 2=WIFI_DSLEEP 3=SRAM_ONLY "
                   "4=LIGHT 5=RTCC)");
        return -1;
    }
    ka_cfg_load();
    ka.sleep_ms = (uint32_t)secs * 1000; /* 0 = auto (keepalive * 3/4) */
    ka_cfg_save();
    ka_cycles_req = (uint16_t)cycles;
    ka_sleep_type = (uint8_t)type;
    ka_post(KA_REQ_SLP);
    atcmd_resp("OK: keepalive sleep seg=%ums cycles=%u type=%u%s "
               "(hb every wake)",
               ka_segment_ms(), (uint32_t)ka_cycles_req,
               (uint32_t)ka_sleep_type,
               (type == SYSTEM_SLEEP_TYPE_SRAM_WIFI) ? "" :
               " NOTE: non-default level - wake may REBOOT (RTCC) or "
               "drop the session; observe sr= and reconn counters");
    return 0;
}

int32_t ne102_atcmd_kastop(const char *cmd, char *argv[], uint32_t argc)
{
    (void)cmd; (void)argc;
    if (!in_sleep_loop) {
        atcmd_resp("OK: not sleeping");
        return 0;
    }
    stop_req = 1;
    atcmd_resp("OK: exit requested (takes effect in the next awake window, "
               "<= one sleep segment)");
    return 0;
}

/* AT+KAPK=<ip>[,<port>[,<budget>]] - park-internal heartbeat target.
 * ip "0" disarms the mode. port default 9999, budget default 20
 * (suppressed timer wakes before a full resume refreshes the app). */
int32_t ne102_atcmd_kapk(const char *cmd, char *argv[], uint32_t argc)
{
#if NE102_KA_PARK_HB
    (void)cmd;
    if (argc == 1 && argv[0][0] == '?') {
        /* tx_rc/tx_calls live in the always-on park struct: they survive
         * reboots (not power cycles), so this also reads the LAST park
         * session's dsleep_tx_ether result even after a reflash */
        atcmd_resp("en=%u ip=%s port=%u budget=%u sent=%u/%u",
                   pk.en, pk.ip, (uint32_t)pk.port, (uint32_t)pk.budget,
                   (uint32_t)cnt_park_hb, (uint32_t)last_park_n);
#if NE102_KA_PARK_HB
        atcmd_resp("park tx: calls=%u rc=%d (rev: -19=tx_op NULL, "
                   "-12=malloc, else lmac code)", ka_park.tx_calls,
                   (int)ka_park.tx_rc);
#endif
        return 0;
    }
    if (argc < 1 || !argv[0][0]) {
        atcmd_resp("usage: AT+KAPK=<ip>[,<port>[,<budget>]] (ip 0 = off)");
        return -1;
    }
    if (os_strcmp(argv[0], "0") == 0) {
        pk.en = 0;
    } else {
        os_strncpy(pk.ip, argv[0], sizeof(pk.ip) - 1);
        pk.ip[sizeof(pk.ip) - 1] = 0;
        if (argc >= 2 && argv[1][0]) {
            int32_t p = os_atoi(argv[1]);
            if (p < 1 || p > 65535) {
                atcmd_resp("ERR: bad port");
                return -1;
            }
            pk.port = (uint16_t)p;
        }
        if (argc >= 3 && argv[2][0]) {
            int32_t b = os_atoi(argv[2]);
            if (b < 1 || b > 255) {
                atcmd_resp("ERR: budget 1..255");
                return -1;
            }
            pk.budget = (uint8_t)b;
        }
        pk.en = 1;
    }
    syscfg_set_binary(KA_PARK_CFG_KEY, &pk, sizeof(pk));
    atcmd_resp("OK: park-hb %s (udp %s:%u budget %u; AT+KASLP parks, "
               "AT+KAPKT probes)", pk.en ? "armed" : "off",
               pk.ip, (uint32_t)pk.port, (uint32_t)pk.budget);
    return 0;
#else
    (void)cmd; (void)argv; (void)argc;
    atcmd_resp("ERR: NE102_KA_PARK_HB not compiled (ne102_config.h)");
    return -1;
#endif
}

/* AT+KAPKT - one app-context UDP probe to the configured park target
 * (lwip sockets). Separates "L3 path to the listener works" from the
 * park-internal TX machinery: probe arrives + park-hb not = dsleep TX
 * side fault; neither arrives = network path problem. */
int32_t ne102_atcmd_kapkt(const char *cmd, char *argv[], uint32_t argc)
{
#if NE102_KA_PARK_HB
    int fd, rc;
    struct sockaddr_in sa;

    (void)cmd; (void)argv; (void)argc;
    os_memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(pk.port);
    sa.sin_addr.s_addr = inet_addr(pk.ip);
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        atcmd_resp("ERR: no socket");
        return -1;
    }
    rc = sendto(fd, "ne102_park_probe", 16, 0,
                (struct sockaddr *)&sa, sizeof(sa));
    close(fd);
    atcmd_resp("%s: udp probe to %s:%u rc=%d (watch the PC listener)",
               rc == 16 ? "OK" : "ERR", pk.ip, (uint32_t)pk.port, rc);
    return rc == 16 ? 0 : -1;
#else
    (void)cmd; (void)argv; (void)argc;
    atcmd_resp("ERR: NE102_KA_PARK_HB not compiled");
    return -1;
#endif
}

/* AT+KADBG=<0|1> - low-power program debug prints (bit0 = print enable).
 * The park program prints its OWN diagnostics ("tx: type=...", "null_tx:
 * fail", "ARP: fail", "alloc_fail", BKTO/ulp lines) - enable around a
 * park-hb test to see exactly why a tx_ether frame was dropped. */
int32_t ne102_atcmd_kadbg(const char *cmd, char *argv[], uint32_t argc)
{
    int32_t v;

    (void)cmd;
    if (argc == 1 && argv[0][0] == '?') {
        atcmd_resp("dsleep_dbg=0x%x", dsleep_get_dbg_cfg());
        return 0;
    }
    if (argc < 1 || !argv[0][0]) {
        atcmd_resp("usage: AT+KADBG=<0|1>");
        return -1;
    }
    v = os_atoi(argv[0]);
    dsleep_set_dbg_cfg(v ? DSLEEP_DBG_EN_PRINT : 0);
    {
        int32_t dbgv = v ? 1 : 0;

        syscfg_set_binary(KA_DBG_KEY, &dbgv, sizeof(dbgv));
    }
    atcmd_resp("OK: dsleep dbg %s (park prints tx/null/ARP/BKTO errors)",
               v ? "ON (noisy)" : "off");
    return 0;
}

int32_t ne102_atcmd_kapub(const char *cmd, char *argv[], uint32_t argc)
{
    (void)cmd;
    if (argc < 1 || !argv[0][0]) {
        atcmd_resp("usage: AT+KAPUB=<text> (published on %s)", ka.pub_topic);
        return -1;
    }
    if (!mq_connected) {
        atcmd_resp("ERR: MQTT not connected");
        return -1;
    }
    {
        mqtt_message_t msg;
        os_memset(&msg, 0, sizeof(msg));
        msg.payload = argv[0];
        msg.payloadlen = os_strlen(argv[0]);
        msg.qos = QOS0;
        if (mqtt_publish(mq, ka.pub_topic, &msg) != 0) {
            atcmd_resp("ERR: publish failed");
            return -1;
        }
    }
    atcmd_resp("OK: published");
    return 0;
}

/* AT+KASHOT - one capture + NE301 photo publish now (needs MQTT up) */
int32_t ne102_atcmd_kashot(const char *cmd, char *argv[], uint32_t argc)
{
    (void)cmd;
    if (argc == 1 && argv[0][0] == '?') {
        atcmd_resp("photos=%u skip=%u rc=%d pub=%d len=%u sram=%d(-%d) topic=%s", cnt_photos,
                   (uint32_t)ka_cap_skip, (int)last_cap_rc,
                   (int)last_pub_stage, last_cap_len,
                   (int)last_sram_free, (int)cap_sram_delta, ka.photo_topic);
        return 0;
    }
    if (!mq_connected) {
        atcmd_resp("ERR: MQTT not connected (AT+KAGO first)");
        return -1;
    }
    if (in_sleep_loop || ka_req) {
        atcmd_resp("ERR: sleeper busy");
        return -1;
    }
    ka_req = KA_REQ_CAP;
    os_sema_up(&ka_sema);
    atcmd_resp("OK: capture scheduled (photo -> %s)", ka.photo_topic);
    return 0;
}

/* AT+KACAP=<skip>[,<w>,<h>[,<target_len>]] - AE-settle skip frames,
 * optional resolution (0,0 = sensor native) and JPEG size-governor
 * target bytes (0 = SDK default 50KB) */
int32_t ne102_atcmd_kacap(const char *cmd, char *argv[], uint32_t argc)
{
    int32_t v;

    (void)cmd;
    if (argc == 1 && argv[0][0] == '?') {
        atcmd_resp("skip=%u w=%u h=%u len=%u photos=%u",
                   (uint32_t)ka_cap_skip, ka_cap_w, ka_cap_h, ka_cap_len,
                   cnt_photos);
        return 0;
    }
    if (argc < 1 || !argv[0][0]) {
        atcmd_resp("usage: AT+KACAP=<skip 0..10>[,<w>,<h>[,<target_len>]]");
        return -1;
    }
    v = os_atoi(argv[0]);
    if (v < 0 || v > 10) {
        atcmd_resp("ERR: skip 0..10");
        return -1;
    }
    ka_cap_skip = (uint8_t)v;
    if (argc >= 3 && argv[1][0] && argv[2][0]) {
        int32_t w = os_atoi(argv[1]);
        int32_t h = os_atoi(argv[2]);

        if (w < 0 || h < 0) {
            atcmd_resp("ERR: bad w/h");
            return -1;
        }
        ka_cap_w = (uint16_t)w;
        ka_cap_h = (uint16_t)h;
    }
    if (argc >= 4 && argv[3][0]) {
        int32_t l = os_atoi(argv[3]);

        if (l < 0 || l > 400 * 1024) {
            atcmd_resp("ERR: len 0..409600");
            return -1;
        }
        ka_cap_len = (uint32_t)l;
    }
    atcmd_resp("OK: skip=%u w=%u h=%u len=%u", (uint32_t)ka_cap_skip,
               ka_cap_w, ka_cap_h, ka_cap_len);
    return 0;
}

int32_t ne102_atcmd_kaq(const char *cmd, char *argv[], uint32_t argc)
{
    struct netif *nif = netif_find("w0");

    (void)cmd; (void)argc;
    /* two lines: the AT print buffer is only 256B (typesdef.h) */
    atcmd_resp("loop=%u mqtt=%u ip=%s cycles=%u hb_ok=%u hb_fail=%u "
               "reconn=%u msgs=%u seg=%ums ka=%us",
               in_sleep_loop, mq_connected,
               ip_ready(nif) ? ipaddr_ntoa(&nif->ip_addr) : "none",
               cnt_cycles, cnt_hb_ok, cnt_hb_fail, cnt_reconn, cnt_msgs,
               ka_segment_ms(), (uint32_t)ka.keepalive_s);
    atcmd_resp("wake=%u(%s) tchk=%+dms w2a=%u/%ums btrc=%u ph=%u(%s,c%u,sr%d) "
               "cur=%u fw=%u(%s) pk=%u/%u photos=%u",
               last_wake_rsn, wk_name(last_wake_rsn), last_tchk_ms,
               w2a_cnt ? w2a_sum / w2a_cnt : 0, w2a_max,
               ka_boot_trace_valid, ka_boot_trace.phase,
               wk_name(ka_boot_trace.wake_rsn), ka_boot_trace.seq,
               (int)ka_boot_trace.sr,
               ka_trace ? ka_trace->phase : 0,
               first_wake_rsn, wk_name(first_wake_rsn),
               (uint32_t)cnt_park_hb, (uint32_t)last_park_n, cnt_photos);
    return 0;
}
