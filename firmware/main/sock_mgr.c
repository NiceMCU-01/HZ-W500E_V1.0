#include "sock_mgr.h"

#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <esp_netif.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define RX_BUF 2048

static struct {
    int            fd;
    int            lfd;             /* 服务端 listen 套接字，accept 后需单独关闭 */
    sock_type_t    type;
    int            transparent;
    struct sockaddr_in peer;      /* 对端（客户端/服务端均使用） */
    struct sockaddr_in last_udp;  /* UDP 最近一次对端 */
    int            connected;     /* TCPS 已 accept */
    bool           in_use;
} s = { .fd = -1, .lfd = -1, .type = SOCK_NONE };

static sock_rx_sink_t s_sink = NULL;
static TaskHandle_t    s_task = NULL;
static volatile bool   s_run  = false;

/* 诊断记录：AT 固件不能开日志，失败原因只能随 AT 应答输出 */
static volatile sock_diag_t s_diag = { .stage = 0, .err = 0, .last_ok = 0 };

static inline void diag_set(int stage, int err)
{
    s_diag.stage = stage;
    s_diag.err   = err;
}

const sock_diag_t *sock_diag(void) { return &s_diag; }

void sock_set_sink(sock_rx_sink_t fn) { s_sink = fn; }

/* ---------------- 接收任务 ---------------- */
static void rx_task(void *arg)
{
    uint8_t buf[RX_BUF];
    (void)arg;

    while (s_run) {
        if (!s.in_use || s.fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* 服务端模式：等待 accept */
        if (s.type == SOCK_TCPS && !s.connected) {
            struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
            setsockopt(s.lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            int c = accept(s.lfd, (struct sockaddr *)&s.peer, NULL);
            if (c >= 0) {
                /* listen 套接字另存到 s.lfd：不能覆盖掉，否则它永远不会被关闭，
                   又是一个socket 泄漏（上限只有 8 个）。 */
                s.fd = c;
                s.connected = true;
                if (s_sink) s_sink((const uint8_t *)"\r\nCONNECTED\r\n", 12);
            }
            continue;
        }

        if (s.type == SOCK_UDPS) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int n = recvfrom(s.fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            if (n > 0) {
                s.last_udp = from;
                if (s_sink) s_sink(buf, n);
            }
            continue;
        }

        int n = recv(s.fd, buf, sizeof(buf), 0);
        if (n > 0) {
            if (s_sink) s_sink(buf, n);
        } else if (n == 0) {
            /* 对端关闭 */
            if (s_sink) s_sink((const uint8_t *)"\r\nCLOSED\r\n", 10);
            if (s.type == SOCK_TCPS) {
                sock_close();
            } else {
                /* 必须真正 close：只置 s.fd = -1 会泄漏套接字。
                   CONFIG_LWIP_MAX_SOCKETS 仅 8，泄漏几次后 socket() 就返回
                   ENFILE(23)，表现为「一开始能连上、连测几轮后全部失败」。 */
                close(s.fd);
                s.fd = -1;
                s.in_use = false;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t sock_init(void)
{
    if (s_task) return ESP_OK;
    s_run = true;
    if (xTaskCreate(rx_task, "sock_rx", 4096, NULL, 5, &s_task) != pdPASS) {
        s_run = false;
        s_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---------------- 打开 ---------------- */
static void reset(void)
{
    if (s.fd >= 0) close(s.fd);
    s.fd = -1;
    s.type = SOCK_NONE;
    s.in_use = false;
    s.connected = false;
    s.transparent = 0;
}

/* 等待式 connect。
   lwIP 在非阻塞套接字上connect() 会立刻返回 EINPROGRESS(118)，
   表示"正在握手"而非失败。原代码只判断 rc != 0 就直接判失败，
   于是握手其实成功、服务端也已accept，AT 层却回ERROR。
   实测这是 CIPSTART 偶发失败的头号原因（10 次失败全部是 118）。 */
/* lwIP 的 TCP 套接字是非阻塞的：connect() 立刻返回 EINPROGRESS(115)
       表示"正在握手"，不是失败。原代码只判断 rc != 0 就判失败，
       于是握手其实成功、服务端也已 accept，AT 层却回 ERROR。

       注意：lwIP 在connect() 失败时可能把 errno 留成上一次的残留值
       （实测出现过 118 = ENOTNAM这种无意义码），所以必须先清零 errno，
       失败后再用 getsockopt(SO_ERROR) 取回真实原因。 */
static int connect_wait(int fd, const struct sockaddr *a, socklen_t alen,
                        int timeout_ms)
{
    errno = 0;                      /* 清除残留 errno，避免误报无意义错误码 */
    if (connect(fd, a, alen) == 0) return 0;

    /* 握手还在进行：等它完成 */
    if (errno == EINPROGRESS || errno == EALREADY) {
        for (;;) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(fd, &wset);
            struct timeval tv = { .tv_sec = timeout_ms / 1000,
                                  .tv_usec = (timeout_ms % 1000) * 1000 };
            int sel = select(fd + 1, NULL, &wset, NULL, &tv);
            if (sel < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (sel == 0) { errno = ETIMEDOUT; return -1; }

            int soerr = 0;
            socklen_t slen = sizeof(soerr);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) != 0) return -1;
            if (soerr == 0) return 0;                       /* 握手成功 */
            if (soerr == EINPROGRESS || soerr == EALREADY) continue;
            errno = soerr;
            return -1;
        }
    }

    /* connect() 直接失败：errno 可能不可信，用 SO_ERROR 取真实原因。
       仍取不到就归为 ETIMEDOUT，避免把无意义码（如 118）报给上层。 */
    {
        int soerr = 0;
        socklen_t slen = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) == 0 && soerr != 0) {
            errno = soerr;
        } else if (errno == 0) {
            errno = ETIMEDOUT;
        }
    }
    return -1;
}

esp_err_t sock_open(sock_type_t type, const char *ip, int rport, int lport)
{
    sock_close();

    int is_tcp = (type == SOCK_TCPC || type == SOCK_TCPS || type == SOCK_HTPC);

    /* 同sock_http_get：TIME_WAIT 会占用套接字额度，重试几次即可拿到 fd */
    int fd = -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        fd = socket(AF_INET, is_tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
        if (fd >= 0) break;
        diag_set(1, errno);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (fd < 0) return ESP_FAIL;

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons((uint16_t)rport);
    a.sin_addr.s_addr = ipaddr_addr(ip);

    switch (type) {
    case SOCK_TCPC:
        if (connect_wait(fd, (struct sockaddr *)&a, sizeof(a), 10000) != 0) {
            diag_set(2, errno); close(fd); return ESP_FAIL;
        }
        s.peer = a;
        break;

    case SOCK_UDPC:
        if (lport > 0) {
            struct sockaddr_in l = { 0 };
            l.sin_family = AF_INET;
            l.sin_port = htons((uint16_t)lport);
            l.sin_addr.s_addr = IPADDR_ANY;
            if (bind(fd, (struct sockaddr *)&l, sizeof(l)) != 0) {
                diag_set(3, errno); close(fd); return ESP_FAIL;
            }
        }
        if (connect_wait(fd, (struct sockaddr *)&a, sizeof(a), 5000) != 0) {
            diag_set(2, errno); close(fd); return ESP_FAIL;
        }
        s.peer = a;
        s.last_udp = a;
        break;

    case SOCK_TCPS:
    case SOCK_UDPS: {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in l = { 0 };
        l.sin_family = AF_INET;
        l.sin_port = htons((uint16_t)(lport > 0 ? lport : 0));
        l.sin_addr.s_addr = IPADDR_ANY;
        if (bind(fd, (struct sockaddr *)&l, sizeof(l)) != 0) {
            diag_set(3, errno); close(fd); return ESP_FAIL;
        }
        if (type == SOCK_TCPS) {
            if (listen(fd, 1) != 0) { diag_set(4, errno); close(fd); return ESP_FAIL; }
            s.connected = false;
        }
        if (rport > 0) { s.last_udp = a; }
        break;
    }
    default:
        close(fd);
        diag_set(1, 0);
        return ESP_FAIL;
    }

    s.fd = fd;
    s.lfd = -1;
    if (type == SOCK_TCPS) s.lfd = fd;   /* listen 与连接套接字同源，由 rx_task 接管 */
    s.type = type;
    s.in_use = true;
    s.connected = (type != SOCK_TCPS);
    diag_set(0, 0);          /* 成功则清空 */
    return ESP_OK;
}

/* ---------------- HTTP GET ---------------- */
esp_err_t sock_http_get(const char *url)
{
    char host[64] = { 0 }, path[256] = "/";
    int  port = 80;

    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;

    const char *slash = strchr(p, '/');
    size_t hl = slash ? (size_t)(slash - p) : strlen(p);
    if (hl >= sizeof(host)) return ESP_ERR_INVALID_ARG;
    memcpy(host, p, hl);
    if (slash) snprintf(path, sizeof(path), "%s", slash);

    char *colon = strrchr(host, ':');
    if (colon) {
        port = atoi(colon + 1);
        *colon = 0;
    }

    sock_close();

    /* 重试：紧跟在 CIPCLOSE 之后时，上一个连接还占着 TIME_WAIT 额度，
       socket() 可能返回 ENFILE/EMFILE。等待额度回收后重试即可。 */
    int fd = -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) break;
        diag_set(1, errno);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (fd < 0) return ESP_FAIL;

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = ipaddr_addr(host);
    if (a.sin_addr.s_addr == 0) { close(fd); return ESP_ERR_INVALID_ARG; }

    /* 注意：不要在 connect 之前设置 SO_RCVTIMEO/SO_SNDTIMEO。
       实测设置后 HTPC 会以 +CIPERR,2,118 稳定失败（stage=connect），
       而同为 TCP 客户端的 TCPC 不设这两项则 10/10 正常。
       超时改由 connect_wait 自身的超时参数控制，更可控。 */
    /* HTPC 紧跟在 CIPCLOSE 之后执行，上一个连接的 TIME_WAIT 还占着套接字额度，
       容易一连 connect 失败。重试整段连接流程。 */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (connect_wait(fd, (struct sockaddr *)&a, sizeof(a), 10000) == 0) break;
        diag_set(2, errno);
        if (attempt == 2) { close(fd); return ESP_FAIL; }
        close(fd);
        vTaskDelay(pdMS_TO_TICKS(400));
        for (int k = 0; k < 3; k++) {
            fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd >= 0) break;
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if (fd < 0) { diag_set(1, errno); return ESP_FAIL; }
    }

    char req[512];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: WT32-ETH01\r\n\r\n",
        path, host);
    if (send(fd, req, rl, 0) < 0) { diag_set(5, errno); close(fd); return ESP_FAIL; }

    s.fd = fd;
    s.type = SOCK_HTPC;
    s.in_use = true;

    /* 同步读完整个响应后关闭。用 select 限时读，避免对端不回包时永久阻塞。 */
    uint8_t buf[1024];
    for (;;) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(fd, &rset);
        struct timeval rtv = { .tv_sec = 5, .tv_usec = 0 };
        if (select(fd + 1, &rset, NULL, NULL, &rtv) <= 0) break;

        int n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (s_sink) s_sink(buf, n);
    }
    sock_close();
    return ESP_OK;
}

/* ---------------- 收发 ---------------- */
esp_err_t sock_send(const uint8_t *d, int len)
{
    if (!s.in_use || s.fd < 0) return ESP_FAIL;
    int sent = 0;
    while (sent < len) {
        int n;
        if (s.type == SOCK_UDPS) {
            n = sendto(s.fd, d + sent, len - sent, 0,
                       (struct sockaddr *)&s.last_udp, sizeof(s.last_udp));
        } else {
            n = send(s.fd, d + sent, len - sent, 0);
        }
        if (n <= 0) return ESP_FAIL;
        sent += n;
    }
    return ESP_OK;
}

void sock_close(void)
{
    if (s.fd >= 0) close(s.fd);
    if (s.lfd >= 0) close(s.lfd);   /* listen 套接字必须单独关闭 */
    s.fd = -1;
    s.lfd = -1;
    s.type = SOCK_NONE;
    s.in_use = false;
    s.connected = false;
    s.transparent = 0;
}

bool sock_is_open(void)         { return s.in_use && s.fd >= 0; }
sock_type_t sock_get_type(void) { return s.type; }
void sock_set_transparent(int on) { s.transparent = on; }
int  sock_is_transparent(void)    { return s.transparent; }