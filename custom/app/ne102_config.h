#ifndef __NE102_PRODUCT_CONFIG_H__
#define __NE102_PRODUCT_CONFIG_H__

/*****************************************************************************
 * NE102 low-power capture camera - product configuration (CUSTOMER_ID 10).
 *
 * Content is copied from sdk/demo/ipc_1080p_demo/ipc_1080p_config.h (the
 * verified NE102 bring-up baseline, CUSTOMER_ID 8) with the demo switch
 * replaced by NE102_PRODUCT. All other macros MUST stay identical so that
 * sensor/VPP/codec/heap SDK sources compile exactly as on the bring-up
 * firmware. Re-sync both files when a board-level macro changes.
 *
 * Camera: GC20C3 1080p 1-lane, JPEG capture oriented (no H264 recording in
 * the fast-capture track). Orchestration lives in custom/app/ (see
 * custom/doc/NE102 实现设计.md).
 *****************************************************************************/

/***************************************************************
 * 打开PIN_FROM_PARAM,通过脚本和config.cfg去生成对应的io配置信息
 * 请查看重要的文件:pin_param.h、config.cfg两个文件
 *************************************************************/
#define PIN_FROM_PARAM
/*****************************************
 * NE102 产品方案宏（替代官方 demo 宏）
 ****************************************/
#define NE102_PRODUCT

/**************************************************************
 * NE102_MQTTKA = MQTT keepalive-sleep test firmware:
 *     STA + long-lived MQTT + SRAM_WIFI keepalive sleep with
 *     breakpoint-resume wake, driven by AT+KA* commands
 *     (custom/component/mqttka, per the low-power guide mode 1).
 * This tree builds the keepalive bench only.
 *************************************************************/
#define NE102_MQTTKA                1

/**************************************************************
 * NE102_KA_PARK_HB = park-internal heartbeat feasibility test
 * (low-power-guide option 3): with it compiled in, AT+KAPK arms a
 * mode where timer wakes are suppressed (wakeup_hook returns
 * DSLEEP_ACTION_SLEEP_CONTINUE) and the low-power program itself
 * sends a pre-built UDP heartbeat from dsleep_set_ready_hook() +
 * dsleep_tx_ether() - the application never resumes, so the awake
 * window drops to zero. Runtime switch: AT+KAPK=<ip>[,<port>[,<n>]].
 * 0 = code not compiled (classic wake-and-publish keepalive only).
 *************************************************************/
#define NE102_KA_PARK_HB            1

/**********************************************************************
 * 系统必要信息宏
 * CONFIG_PSRAM_AVHEAP_SIZE:为应用分配的psram宏,需要根据应用场景分配
 * PSRAM_HEAP:如果需要用到psram,需要打开PSRAM_HEAP
 ********************************************************************/
#define DEFAULT_SYS_CLK                 (192*1000000)
#define PSRAM_HEAP          //如果需要psram当作heap,需要打开这个宏
#define AV_PSRAM_HEAP
#define AV_HEAP

/*******************************************************************************************************************************************
空间分配说明与 ipc_1080p 保持一致（拷贝来源）：
H264：  I/P帧:3.1M+0.5M(默认,根据I帧P帧最大值进行调整)
		纯I帧:0.5M
		h264数据缓冲size: 64帧(最大缓冲)*100K(平银每一帧size) = 6M左右

MJPG(1路):  MJPG:  10-30(mjpg节点)*16K = 160K-480K
*********************************************************************************************************************************************/
/* NE102 is a JPEG-capture device (no recording, preview only uses the
 * 720p sub-stream): 7.5MB (copied from the ipc recording profile) starved
 * the general PSRAM heap - the no-SD capture stash (512KB) and capture
 * working set could not allocate, crashing the pipeline (2026-10-16).
 * 4MB covers capture + preview; the freed 3.5MB goes to the general
 * PSRAM heap. */
#define CONFIG_PSRAM_AVHEAP_SIZE        (4*1024*1024)

#define CONFIG_AVHEAP_SIZE              (100*1024 + 50*1024)

/******************************************************************************
 * wifi的必要参数配置
 ******************************************************************************/
#define WIFI_MODE_DEFAULT               WIFI_MODE_STA
#define WIFI_ACS_CHAN_LISTS             0x1FFF      //要扫描的信道。每1bit对应1个信道(bit 0~11 -> chan 1~12)
#define WIFI_PREVENT_PS_MODE_EN         1           //是否尽可能的阻止sta进入休眠
#define WIFI_PS_NO_FRM_LOSS_EN          1           //缓存的TX休眠帧是否不允许丢弃
#define WIFI_TX_AGG_EN                  1           //没时延要求可以开聚合。一般来说连路由就设1，连手机就设0

/*****************************************************************
 * VCAM开关,部分io电源域需要打开才有电
 *****************************************************************/
#define VCAM_EN                         1
/* NE102: VCAM2 LDO powers the 1V8_CSI rail (sensor IOVDD + camera I2C
 * pull-ups); the 1080p demo reference board does not need it, NE102 does. */
#define VCAM2_EN                        1

/*****************************************************************
 * sd使能
 ***************************************************************/
#define FS_EN                           1

/************************************************************************************************************************
 * sensor型号配置（与 ipc_1080p 相同：GC20C3 单独使能）
 ***************************************************************************************************************************/
#define DEV_SENSOR_GC1084               0
#define DEV_SENSOR_GC2053               0
/* NE102: GC20C3 1080p 1-lane sensor (I2C_ID_SEL strap low -> 0x62/0x63) */
#define DEV_SENSOR_GC20C3               1

/***********************************************************
 *默认mjpeg的节点数量,要根据mjpeg启动的分辨率去考虑
 默认节点大小:16*1024
 1080P:算mjpeg大小100-150K,给20个节点足够
 * ********************************************************/
#define JPG_NODE_COUNT 30

/*************************************************************
 * 节省sram内存,将部分模块强制使用psram
 ************************************************************/
#define MORE_SRAM

/*************************************************************
 * 1080P摄像头的h264副码流需要打开VPP_BUF1_EN
 * 副码流供配置模式预览（MJPEG/子码流）使用，快启抓拍轨不启动 gen420
 ************************************************************/
#define SUB_STREAM_EN                   1
#define SUB_STREAM_WIDTH	            1280

/************************************************************************
* 配置MP4录制最大文件的size（NE102 v1 无录像，保留宏值防漂移）
 ************************************************************************/
#define MP4_MAX_SINGLE_SIZE (100 * 1024 * 1024)

/************************************************************************
* 开启文件系统优化
 ************************************************************************/
#define USE_FAT_CACHE 1

/*************************************************************
 * 支持jpeg编码(通过yuv去编码mjpeg):SUPPORT_ENCODER_JPEG
 * 抓拍/预览需要；H264 解码与 LCD/播放器不启用（快启裁剪）
************************************************************ */
#define SUPPORT_DECODER_JPEG
#define SUPPORT_ENCODER_JPEG

/************************************************************
* NE102 v1 has no audio (PRD appendix A: audio = v2). Disabling the
* codec attach also removes the audio coder RPC thread, which crashed
* when CPU1 came up after Codec_init (first board test, 2026-10-10).
* Keep the value in sync when audio lands in v2.
*********************************************************** */
#define AAC_ENC_CTRL AUCODER_NO_RUN

#endif  /* __NE102_PRODUCT_CONFIG_H__ */
