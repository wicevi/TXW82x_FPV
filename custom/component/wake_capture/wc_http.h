#ifndef __WC_HTTP_H__
#define __WC_HTTP_H__

/*
 * wc_http - minimal blocking HTTP/1.1 POST client for the wake-capture
 * test (single file, no SDK http-server/curl dependency). Body is sent
 * straight from the PSRAM JPEG stash; every step is timeout-bounded so
 * a dead receiver can never wedge the capture cycle.
 */

#include <stdint.h>

struct wc_http_result {
    int32_t  status;      /* HTTP status code (200/201/...) on success;
                           * <0 = transport failure (dns/connect/send/
                           * recv/timeout) */
    uint32_t bytes_sent;  /* body bytes handed to the socket */
};

/* POST `body` (image/jpeg) to the given "http://host[:port]/path" URL.
 * Returns wc_http_result.status (>=100 = an HTTP answer arrived).
 * Host may be an IP literal or a name (DNS, bounded). */
int32_t wc_http_post(const char *url, const uint8_t *body, uint32_t len,
                     struct wc_http_result *out);

#endif /* __WC_HTTP_H__ */
