/*
 * wc_http.c - minimal blocking HTTP/1.1 POST client (see wc_http.h).
 *
 * Bound strategy (every number is a hard ceiling):
 *   DNS/connect : 6s   non-blocking connect + select (lwip ioctlsocket
 *                     FIONBIO; plain blocking connect has no usable
 *                     deadline if the peer silently drops SYN)
 *   send/recv   : 6s   SO_SNDTIMEO/SO_RCVTIMEO per call, looped until
 *                     the whole request is out and the status line is in
 * No retries: the caller (wake_capture) reports and moves on - the
 * cycle must reach its re-sleep regardless of the network state.
 */
#include <basic_include.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/inet.h>
#include "wc_http.h"

#define WC_HTTP_CONNECT_MS  6000
#define WC_HTTP_IO_MS       6000
/* response wait: generous for slow links - bench WiFi measured as low
 * as ~25KB/s (48KB body in ~2-4s on the wire AFTER send() completed,
 * board finding 2026-10-23), 6s was not always enough */
#define WC_HTTP_RESP_MS     15000

/* Parse "http://host[:port]/path". Returns RET_OK and fills
 * host/port/path (in caller-owned buffers) on success. */
static int32_t wc_url_parse(const char *url, char *host, uint32_t hsz,
                            uint16_t *port, const char **path)
{
    const char *p, *slash, *colon;
    uint32_t hostlen;
    uint32_t pval;

    if (os_strncmp(url, "http://", 7) != 0) {
        return RET_ERR;
    }
    p = url + 7;
    slash = os_strchr(p, '/');
    colon = os_strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        hostlen = (uint32_t)(colon - p);
        pval = 0;
        for (p = colon + 1; p != slash && *p >= '0' && *p <= '9'; p++) {
            pval = pval * 10 + (uint32_t)(*p - '0');
        }
        /* accept host:port without a trailing path (p at NUL, no slash) */
        if ((slash && p != slash) || (!slash && *p != 0)
            || pval == 0 || pval > 65535) {
            return RET_ERR;
        }
        *port = (uint16_t)pval;
    } else {
        hostlen = slash ? (uint32_t)(slash - p) : os_strlen(p);
        *port = 80;
    }
    if (hostlen == 0 || hostlen >= hsz) {
        return RET_ERR;
    }
    os_memcpy(host, url + 7, hostlen);
    host[hostlen] = 0;
    *path = slash ? slash : "/";
    return RET_OK;
}

int32_t wc_http_post(const char *url, const uint8_t *body, uint32_t len,
                     struct wc_http_result *out)
{
    char host[48];
    const char *path;
    uint16_t port = 80;
    struct sockaddr_in dst;
    struct hostent *he;
    uint32_t ip;
    int fd = -1, one = 1, res, i;
    int32_t status = -1;
    char hdr[192];
    char resp[160];
    fd_set wset;
    struct timeval tv;
    uint32_t sent = 0;
    int soerr = 0;
    int soerr_len = sizeof(soerr);

    os_memset(out, 0, sizeof(*out));
    if (wc_url_parse(url, host, sizeof(host), &port, &path) != RET_OK) {
        os_printf(KERN_ERR "WC: bad url %s (need http://host[:port]/path)\n",
                  url);
        return -1;
    }
    ip = inet_addr(host);   /* IP literal first: no DNS on the bench */
    if (ip == INADDR_NONE) {
        he = gethostbyname(host);
        if (!he || !he->h_addr_list[0]) {
            os_printf(KERN_ERR "WC: dns %s failed\n", host);
            return -2;
        }
        ip = *(uint32_t *)he->h_addr_list[0];
    }
    /* connect stage, up to 3 attempts on a fresh socket, NO delays
     * between (user ruling: no hard delays in the capture path - the
     * keepalive path shares this code): a cold-boot connect can race
     * the freshly-DHCP-bound interface (half-mounted mask/gw window
     * aborts the attempt, soerr=103 ECONNABORTED - board finding
     * 2026-10-23); an immediate retry lands past the bind. The
     * keepalive path (netif bound long before) connects first try. */
    {
        uint8_t attempt;

        for (attempt = 0; attempt < 3; attempt++) {
            fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                return -3;
            }
            dst.sin_family = AF_INET;
            dst.sin_port = PP_HTONS(port);
            dst.sin_addr.s_addr = ip;

            /* non-blocking connect + select: bounded dial */
            ioctlsocket(fd, FIONBIO, &one);
            res = connect(fd, (struct sockaddr *)&dst, sizeof(dst));
            if (res < 0) {
                fd_set wset2;

                FD_ZERO(&wset2);
                FD_SET(fd, &wset2);
                tv.tv_sec = WC_HTTP_CONNECT_MS / 1000;
                tv.tv_usec = (WC_HTTP_CONNECT_MS % 1000) * 1000;
                res = select(fd + 1, NULL, &wset2, NULL, &tv);
                if (res <= 0) {
                    os_printf(KERN_ERR "WC: connect %s:%u timeout/err"
                              "%s\n", host, port,
                              attempt < 2 ? " (retrying)" : "");
                    close(fd);
                    fd = -1;
                    continue;
                }
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr,
                           (socklen_t *)&soerr_len);
                if (soerr) {
                    os_printf(KERN_ERR "WC: connect %s:%u soerr=%d%s\n",
                              host, port, soerr,
                              attempt < 2 ? " (retrying)" : "");
                    close(fd);
                    fd = -1;
                    continue;
                }
            }
            break;   /* connected (or instantly ok) */
        }
        if (fd < 0) {
            goto out;   /* all attempts failed */
        }
    }
    /* back to blocking-with-select: this lwip build does NOT honor
     * SO_SNDTIMEO/SO_RCVTIMEO (board finding 2026-10-23: a mid-body
     * TX stall blocked send() FOREVER with the peer's window open and
     * recv() waited equally forever - both with 6s sockopts set).
     * select() works (the connect phase proves it), so every send and
     * the response wait are select-bounded wall-clock deadlines. */
    one = 0;
    ioctlsocket(fd, FIONBIO, &one);

    os_snprintf(hdr, sizeof(hdr),
                "POST %s HTTP/1.1\r\n"
                "Host: %s:%u\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %u\r\n"
                "Connection: close\r\n\r\n",
                path, host, port, len);
    {
        const uint8_t *p = (const uint8_t *)hdr;
        uint32_t left = (uint32_t)os_strlen(hdr);

        while (left) {
            int32_t n;
            fd_set wset;
            struct timeval tv;

            FD_ZERO(&wset);
            FD_SET(fd, &wset);
            tv.tv_sec = WC_HTTP_IO_MS / 1000;
            tv.tv_usec = (WC_HTTP_IO_MS % 1000) * 1000;
            if (select(fd + 1, NULL, &wset, NULL, &tv) <= 0) {
                os_printf(KERN_ERR "WC: hdr send select timeout\n");
                goto out;
            }
            n = send(fd, (const char *)p, left, 0);
            if (n <= 0) {
                os_printf(KERN_ERR "WC: send hdr failed\n");
                goto out;
            }
            p += n;
            left -= (uint32_t)n;
        }
    }
    while (sent < len) {
        int32_t n;
        fd_set wset;
        struct timeval tv;

        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        tv.tv_sec = WC_HTTP_IO_MS / 1000;
        tv.tv_usec = (WC_HTTP_IO_MS % 1000) * 1000;
        if (select(fd + 1, NULL, &wset, NULL, &tv) <= 0) {
            os_printf(KERN_ERR "WC: body send stall at %u/%u "
                      "(select timeout, TX path wedged?)\n", sent, len);
            goto out;
        }
        n = send(fd, (const char *)(body + sent), len - sent, 0);
        if (n <= 0) {
            os_printf(KERN_ERR "WC: send body failed at %u/%u\n", sent, len);
            goto out;
        }
        sent += (uint32_t)n;
    }
    out->bytes_sent = sent;

    /* status line only: "HTTP/1.x NNN ..." - select-bounded wait */
    {
        uint32_t got = 0;
        const uint32_t rsp_to = WC_HTTP_RESP_MS;

        os_memset(resp, 0, sizeof(resp));
        while (got < sizeof(resp) - 1) {
            int32_t n;
            fd_set rset;
            struct timeval tv;

            FD_ZERO(&rset);
            FD_SET(fd, &rset);
            tv.tv_sec = rsp_to / 1000;
            tv.tv_usec = (rsp_to % 1000) * 1000;
            n = select(fd + 1, &rset, NULL, NULL, &tv);
            if (n < 0) {
                os_printf(KERN_ERR "WC: resp select err\n");
                goto out;
            }
            if (n == 0) {
                os_printf(KERN_ERR "WC: no response in %ums (recv "
                          "timeout)\n", rsp_to);
                goto out;
            }
            n = recv(fd, resp + got, sizeof(resp) - 1 - got, 0);
            if (n <= 0) {
                os_printf(KERN_ERR "WC: connection closed before "
                          "status (%u/%u)\n", sent, len);
                goto out;
            }
            got += (uint32_t)n;
            /* stop at end of headers once the status line is in */
            if (got >= 12 && os_strstr(resp, "\r\n")) {
                break;
            }
        }
    }
    for (i = 0; resp[i] && resp[i] != ' '; i++) {
        /* skip the protocol token */
    }
    if (resp[i] == ' ') {
        status = os_atoi(&resp[i + 1]);
    }
    if (status < 100) {
        os_printf(KERN_ERR "WC: bad status line: %.32s\n", resp);
        status = -7;
    }
    os_printf(KERN_INFO "WC: http %s:%u%s -> %d \"%s\"\n", host, port, path,
              status, resp);
out:
    if (fd >= 0) {
        close(fd);
    }
    out->status = status;
    return status;
}
