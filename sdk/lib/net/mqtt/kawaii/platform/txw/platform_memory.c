/*
 * @Author: jiejie
 * @Github: https://github.com/jiejieTop
 * @Date: 2019-12-14 22:02:07
 * @LastEditTime: 2020-04-27 16:32:58
 * @Description: the code belongs to jiejie, please keep the author information and source code according to the license.
 */
#include "platform_memory.h"
#include "chip/txw82x/txw82x.h"

/* NE102: large buffers (the ~50KB-500KB MQTT write buffer) go to the
 * 8MB PSRAM heap; small ones stay in SRAM. The original 16KB threshold
 * was only wrong because frees were not routed by address - fixed
 * 2026-10-14 (IS_PSRAM_ADDR -> os_free_psram), which was the real bug
 * behind "publish rc=-1" (SRAM mmpool corruption on every free). */
#define PSRAM_THRESHOLD (16 * 1024)

void *platform_memory_alloc(size_t size)
{
    if (size >= PSRAM_THRESHOLD) {
        return os_zalloc_psram(size);
    }
    return os_zalloc(size);
}

void *platform_memory_calloc(size_t num, size_t size)
{
    size_t total = num * size;
    if (total >= PSRAM_THRESHOLD) {
        void *p = os_malloc_psram(total);
        if (p) os_memset(p, 0, total);
        return p;
    }
    return os_calloc(num, size);
}

void platform_memory_free(void *ptr)
{
    /* free MUST route by address: os_free() frees into the SRAM mmpool,
     * and a PSRAM pointer there corrupts the pool list ("sram: free
     * error, ptr=0x28xxx" + ERROR FREE cascade, board 2026-10-13) */
    if (ptr) {
        if (IS_PSRAM_ADDR(ptr)) {
            os_free_psram(ptr);
        } else {
            os_free(ptr);
        }
    }
}
