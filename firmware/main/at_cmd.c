/*
 * AT 指令解析与实现 —— 对应 WT32-ETH01 V1.1 AT 指令集
 *
 * 覆盖 16 条指令：
 *   AT / ATE / AT+RST / AT+GMR / AT+RESTORE / AT+UART_DEF / AT+PASSCHANNEL
 *   AT+CWMODE / AT+CWJAP
 *   AT+CWDHCP_DEF / AT+CIPETH_DEF
 *   AT+CIPSTART / AT+CIPSEND / AT+CIPMODE / AT+CIPCLOSE / AT+CIFSR
 */
#include "at_cmd.h"
#include "at_config.h"
#include "net_mgr.h"
#include "sock_mgr.h"
#include "board_pins.h"

#include <driver/uart.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

#define FW_VER   "WT32-ETH01_AT_V1.1"
#define IDF_VER  "ESP-IDF v5.3.2"

#define LINE_MAX 1024
#define MAX_ARGC 8

static int s_echo = 1;
static int s_cipmode = 0;

static volatile int s_send_remaining = 0;
/* 数据阶段结束后的命令间隔保护：丢弃驱动里晚到的残留字节 */
static volatile TickType_t s_guard_until = 0;
static volatile int s_send_total     = 0;
/* CIPSEND 数据阶段缓冲：收满 len 字节后整块 sock_send，避免 UDP 被拆成单字节包 */
static uint8_t s_send_buf[2048];
static volatile int s_send_filled = 0;

static char s_line[LINE_MAX];
static int  s_len = 0;

static SemaphoreHandle_t s_tx_mutex = NULL;

/* +++ 转义检测 */
static uint8_t    s_plus_buf[3];
static int        s_plus_n = 0;
static int        s_plus_armed = 0;
static TickType_t s_plus_deadline = 0;

/* ---------------- 输出 ---------------- */
void at_out_raw(const uint8_t *d, int len)
{
    if (len <= 0) return;
    if (!s_tx_mutex) { uart_write_bytes(AT_UART_NUM, d, len); return; }
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_write_bytes(AT_UART_NUM, d, len);
    uart_wait_tx_done(AT_UART_NUM, pdMS_TO_TICKS(200));
    xSemaphoreGive(s_tx_mutex);
}

static void at_out(const char *s) { at_out_raw((const uint8_t *)s, (int)strlen(s)); }
static void ok(void) { at_out("\r\nOK\r\n"); }
static void er(void) { at_out("\r\nERROR\r\n"); }

/* ---------------- 参数解析 ---------------- */
static int split_args(char *s, char **argv, int max)
{
    int n = 0;
    char *p = s;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0) break;
        if (*p == '"') {
            p++;
            argv[n++] = p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = 0;
        } else {
            argv[n++] = p;
            while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
        }
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    return n;
}

static int arg_int(const char *s, int def)
{
    if (!s || !*s) return def;
    return atoi(s);
}

static void cfg_commit(void)
{
    cfg_store(cfg_get());
    cfg_save(cfg_get());
}

/* ---------------- 基础指令 ---------------- */
static void h_gmr(void)
{
    char b[128];
    snprintf(b, sizeof(b), "\r\n%s\r\n%s", FW_VER, IDF_VER);
    at_out(b);
    ok();
}

static void h_uart_def(char **argv, int n)
{
    at_config_t *c = cfg_get();
    if (n < 1) {
        char b[96];
        snprintf(b, sizeof(b), "+UART_DEF:%d,%d,%d,%d,%d",
                 c->baudrate, c->databits, c->stopbits, c->parity, c->flow_control);
        at_out(b); ok(); return;
    }
    if (n < 5) { er(); return; }
    int baud = arg_int(argv[0], 0);
    int db   = arg_int(argv[1], 0);
    int sb   = arg_int(argv[2], 0);
    int par  = arg_int(argv[3], 0);
    int fl   = arg_int(argv[4], 0);
    if (baud < 300 || baud > 4000000) { er(); return; }
    if (db < 5 || db > 8)   { er(); return; }
    if (sb < 1 || sb > 3)   { er(); return; }
    if (par < 0 || par > 2) { er(); return; }
    if (fl  < 0 || fl  > 3) { er(); return; }

    int old = c->baudrate;
    c->baudrate = baud; c->databits = db; c->stopbits = sb;
    c->parity   = par;   c->flow_control = fl;
    cfg_commit();
    if (baud != old) uart_set_baudrate(AT_UART_NUM, baud);
    ok();
}

static void h_passchannel(char **argv, int n)
{
    at_config_t *c = cfg_get();
    if (n < 1) {
        char b[48];
        snprintf(b, sizeof(b), "+PASSCHANNEL:%d", c->pass_channel);
        at_out(b); ok(); return;
    }
    int v = arg_int(argv[0], 0);
    if (v < 1 || v > 6) { er(); return; }
    c->pass_channel = v;
    cfg_commit();
    ok();
}

/* ---------------- WiFi ---------------- */
static void h_cwmode(char **argv, int n)
{
    at_config_t *c = cfg_get();
    if (n < 1) {
        char b[32];
        snprintf(b, sizeof(b), "+CWMODE:%d", c->cwmode);
        at_out(b); ok(); return;
    }
    int v = arg_int(argv[0], -1);
    if (v < 0 || v > 3) { er(); return; }

    net_wifi_stop_ap();
    net_wifi_stop_sta();
    c->cwmode = v;
    cfg_commit();
    switch (v) {
    case 0: break;
    case 1: net_wifi_start_sta(); break;
    case 2: net_wifi_start_ap();  break;
    default: net_wifi_start_sta(); net_wifi_start_ap(); break;
    }
    ok();
}

static void h_cwjap(char **argv, int n)
{
    if (n < 1) {
        wifi_ap_record_t ap;
        char b[192];
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            unsigned char *m = ap.bssid;
            /* ap.ssid 是固定 32 字节数组、不保证以 '\0' 结尾，必须限制长度输出，
               否则会带出后面的乱码（曾表现为 SSID 为空） */
            char ssid[33];
            memcpy(ssid, ap.ssid, 32);
            ssid[32] = 0;
            snprintf(b, sizeof(b), "+CWJAP:\"%s\",%02X:%02X:%02X:%02X:%02X:%02X,%d,%d",
                     ssid, m[0], m[1], m[2], m[3], m[4], m[5],
                     ap.primary, ap.rssi);
            at_out(b); ok();
        } else {
            at_out("+CWJAP:3");
            er();
        }
        return;
    }
    at_config_t *c = cfg_get();
    const char *ssid = argv[0] ? argv[0] : "";
    const char *pwd  = (n >= 2 && argv[1]) ? argv[1] : "";
    if (!*ssid) { er(); return; }

    strncpy(c->sta_ssid, ssid, sizeof(c->sta_ssid) - 1);
    strncpy(c->sta_pwd,  pwd,  sizeof(c->sta_pwd) - 1);
    c->sta_ssid[sizeof(c->sta_ssid) - 1] = 0;
    c->sta_pwd[sizeof(c->sta_pwd) - 1]   = 0;
    cfg_commit();

    if (net_sta_connect_async(c->sta_ssid, c->sta_pwd, 4) != ESP_OK) { er(); return; }
    ok();
}

/* ---------------- 以太网 ---------------- */
static void h_cwdhcp_def(char **argv, int n)
{
    at_config_t *c = cfg_get();
    if (n < 1) {
        char b[80];
        snprintf(b, sizeof(b), "+CWDHCP_DEF:%d,%d\r\nDHCP:%d", c->dhcp_mode, c->dhcp_en, c->dhcp_en);
        at_out(b); ok(); return;
    }
    if (n < 2) { er(); return; }
    int mode = arg_int(argv[0], 0);
    int en   = arg_int(argv[1], 0);
    if (mode < 0 || mode > 3) { er(); return; }
    if (en   < 0 || en   > 1) { er(); return; }
    c->dhcp_mode = mode;
    c->dhcp_en   = en;
    cfg_commit();
    net_eth_reconfigure();
    ok();
}

static void h_cipe_def(char **argv, int n)
{
    at_config_t *c = cfg_get();
    if (n < 1) {
        char b[96];
        snprintf(b, sizeof(b), "+CIPETH_DEF:\"%s\",\"%s\",\"%s\"", c->eth_ip, c->eth_gw, c->eth_mask);
        at_out(b); ok(); return;
    }
    const char *ip = argv[0] ? argv[0] : "";
    const char *gw = (n >= 2 && argv[1] && *argv[1]) ? argv[1] : "192.168.1.1";
    const char *mk = (n >= 3 && argv[2] && *argv[2]) ? argv[2] : "255.255.255.0";
    if (!*ip) { er(); return; }

    strncpy(c->eth_ip,   ip, sizeof(c->eth_ip) - 1);
    strncpy(c->eth_gw,   gw, sizeof(c->eth_gw) - 1);
    strncpy(c->eth_mask, mk, sizeof(c->eth_mask) - 1);
    c->eth_ip[sizeof(c->eth_ip) - 1] = 0;
    c->eth_gw[sizeof(c->eth_gw) - 1] = 0;
    c->eth_mask[sizeof(c->eth_mask) - 1] = 0;
    c->dhcp_en = 0;                 /* 静态 IP 生效则关闭 DHCP */
    cfg_commit();
    net_eth_reconfigure();
    ok();
}

/* ---------------- TCP/IP ---------------- */
/* 失败时带上errno 诊断行。AT 固件不能开日志（日志会混进应答），
   所以诊断码必须随 AT 应答一起输出。stage: 1=socket 2=connect 3=bind 4=listen 5=send */
static void er_diag(const char *tag)
{
    const sock_diag_t *d = sock_diag();
    char b[48];
    if (d->stage) {
        snprintf(b, sizeof(b), "+CIPERR,%d,%d\r\n", d->stage, d->err);
        at_out(b);
    }
    er();
    (void)tag;
}

static void h_cipstart(char **argv, int n)
{
    sock_close();
    if (n < 1) { er(); return; }
    const char *t = argv[0] ? argv[0] : "";

    if (!strcasecmp(t, "HTPC")) {
        if (n < 2 || !argv[1]) { er(); return; }
        if (sock_http_get(argv[1]) != ESP_OK) { er_diag("HTPC"); return; }
        ok(); return;
    }
    if (!strcasecmp(t, "TCPC") || !strcasecmp(t, "UDPC")) {
        if (n < 3) { er(); return; }
        const char *ip = argv[1] ? argv[1] : "";
        int rport = arg_int(argv[2], 0);
        int lport = (n >= 4) ? arg_int(argv[3], 0) : 0;
        if (!*ip || rport <= 0) { er(); return; }
        sock_type_t st = !strcasecmp(t, "TCPC") ? SOCK_TCPC : SOCK_UDPC;
        if (sock_open(st, ip, rport, lport) != ESP_OK) { er_diag(t); return; }
        ok(); return;
    }
    if (!strcasecmp(t, "TCPS") || !strcasecmp(t, "UDPS")) {
        int lport = (n >= 2) ? arg_int(argv[1], 0) : 0;
        sock_type_t st = !strcasecmp(t, "TCPS") ? SOCK_TCPS : SOCK_UDPS;
        if (sock_open(st, "0.0.0.0", 0, lport) != ESP_OK) { er(); return; }
        ok(); return;
    }
    er();
}

static void h_cipsend(char **argv, int n)
{
    /* 普通模式：AT+CIPSEND=<len> */
    if (n >= 1) {
        int len = arg_int(argv[0], -1);
        if (len <= 0 || len > (int)sizeof(s_send_buf)) { er(); return; }
        if (!sock_is_open()) { er(); return; }
        /* 不在这里 flush：会与 uart_read_bytes 抢 FIFO，
           残留字节交给数据阶段结束后的命令间隔保护处理 */
        at_out("\r\n> ");
        s_send_total     = len;
        s_send_remaining = len;
        s_send_filled    = 0;
        return;
    }
    /* 透传模式：AT+CIPSEND */
    if (!sock_is_open()) { er(); return; }
    s_cipmode = 1;
    sock_set_transparent(1);
    ok();
}

static void h_cipmode(char **argv, int n)
{
    if (n < 1) {
        char b[32];
        snprintf(b, sizeof(b), "+CIPMODE:%d", s_cipmode);
        at_out(b); ok(); return;
    }
    int v = arg_int(argv[0], -1);
    if (v != 0 && v != 1) { er(); return; }
    s_cipmode = v;
    sock_set_transparent(v == 1);
    ok();
}

static void h_cipclose(char **argv, int n)
{
    (void)argv; (void)n;
    if (!sock_is_open()) { er(); return; }
    sock_close();
    at_out("\r\nCLOSED\r\n");
    ok();
}

static void h_cifsr(char **argv, int n)
{
    (void)argv; (void)n;
    char ip[24], mac[24], b[160];

    net_get_ip(net_ap_netif(), ip, sizeof(ip));
    net_get_ap_mac(mac, sizeof(mac));
    snprintf(b, sizeof(b), "\r\n+CIFSR:APIP,\"%s\"\r\n+CIFSR:APMAC,\"%s\"", ip, mac);
    at_out(b);

    net_get_ip(net_sta_netif(), ip, sizeof(ip));
    net_get_sta_mac(mac, sizeof(mac));
    /* 附带 STA 的 DHCP 状态，便于现场判断"连上了但没 IP"是 DHCP 未启动还是被拒绝 */
    esp_netif_dhcp_status_t dstat;
    int dh = -1;
    if (esp_netif_dhcpc_get_status(net_sta_netif(), &dstat) == ESP_OK) dh = (int)dstat;
    snprintf(b, sizeof(b), "\r\n+CIFSR:STAIP,\"%s\"\r\n+CIFSR:STAMAC,\"%s\"\r\n+CIFSR:STADHCP,%d,%d",
             ip, mac, dh, (int)net_dhcp_err());
    at_out(b);

    net_get_ip(net_eth_netif(), ip, sizeof(ip));
    net_get_eth_mac(mac, sizeof(mac));
    snprintf(b, sizeof(b), "\r\n+CIFSR:ETHIP,\"%s\"\r\n+CIFSR:ETHMAC,\"%s\"", ip, mac);
    at_out(b);

    ok();
}

/* ---------------- 命令分发 ---------------- */
static void handle_line(char *cmd)
{
    char  work[LINE_MAX];
    char *argv[MAX_ARGC];
    int   argc = 0;

    strncpy(work, cmd, LINE_MAX - 1);
    work[LINE_MAX - 1] = 0;

    /* 尾部 '?' 表示查询型；"=?" 表示测试型，两者要区分 */
    size_t l = strlen(work);
    int is_query = 0, is_test = 0;
    if (l && work[l - 1] == '?') {
        work[l - 1] = 0;
        is_query = 1;
        if (l >= 2 && work[l - 2] == '=') {
            work[l - 2] = 0;
            is_test = 1;
        }
    }

    /* 回显 */
    if (s_echo) { at_out(cmd); at_out("\r\n"); }

    /* --- 执行型 --- */
    if (!strcasecmp(work, "AT"))   { if (is_query) { er(); return; } ok(); return; }
    if (!strcasecmp(work, "ATE0")) { s_echo = 0; if (is_query) { er(); return; } ok(); return; }
    if (!strcasecmp(work, "ATE1")) { s_echo = 1; if (is_query) { er(); return; } ok(); return; }
    if (!strcasecmp(work, "AT+GMR"))     { if (is_query) { er(); return; } h_gmr(); return; }
    if (!strcasecmp(work, "AT+CIFSR"))   { if (is_query) { er(); return; } h_cifsr(NULL, 0); return; }
    if (!strcasecmp(work, "AT+CIPCLOSE")) { if (is_query) { er(); return; } h_cipclose(NULL, 0); return; }
    if (!strcasecmp(work, "AT+CIPSEND"))  { if (is_query) { er(); return; } h_cipsend(NULL, 0); return; }
    if (!strcasecmp(work, "AT+RST")) {
        if (is_query) { er(); return; }
        ok(); vTaskDelay(pdMS_TO_TICKS(300)); esp_restart(); return;
    }
    if (!strcasecmp(work, "AT+RESTORE")) {
        if (is_query) { er(); return; }
        cfg_defaults(cfg_get());
        cfg_commit();
        cfg_erase();
        ok();
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
        return;
    }

    /* --- 测试型 AT+xxx=? --- */
    if (is_query) {
        if (is_test && !strcasecmp(work, "AT+CWMODE")) {
            at_out("+CWMODE:<0..3> (0:null,1:Station,2:SoftAP,3:SoftAP+Station)");
            ok(); return;
        }
        if (is_test) { er(); return; }
        /* 查询型 */
        if (!strcasecmp(work, "AT+UART_DEF"))    { h_uart_def(NULL, 0); return; }
        if (!strcasecmp(work, "AT+PASSCHANNEL")) { h_passchannel(NULL, 0); return; }
        if (!strcasecmp(work, "AT+CWMODE"))      { h_cwmode(NULL, 0); return; }
        if (!strcasecmp(work, "AT+CWJAP"))       { h_cwjap(NULL, 0); return; }
        if (!strcasecmp(work, "AT+CWDHCP_DEF"))  { h_cwdhcp_def(NULL, 0); return; }
        if (!strcasecmp(work, "AT+CIPETH_DEF"))  { h_cipe_def(NULL, 0); return; }
        if (!strcasecmp(work, "AT+CIPMODE"))     { h_cipmode(NULL, 0); return; }
        er(); return;
    }

    /* --- 设置型：需要 '=' --- */
    char *eq = strchr(work, '=');
    if (!eq) { er(); return; }
    *eq = 0;
    argc = split_args(eq + 1, argv, MAX_ARGC);

    if (!strcasecmp(work, "AT+UART_DEF"))    { h_uart_def(argv, argc); return; }
    if (!strcasecmp(work, "AT+PASSCHANNEL")) { h_passchannel(argv, argc); return; }
    if (!strcasecmp(work, "AT+CWMODE"))      { h_cwmode(argv, argc); return; }
    if (!strcasecmp(work, "AT+CWJAP"))       { h_cwjap(argv, argc); return; }
    if (!strcasecmp(work, "AT+CWDHCP_DEF"))  { h_cwdhcp_def(argv, argc); return; }
    if (!strcasecmp(work, "AT+CIPETH_DEF"))  { h_cipe_def(argv, argc); return; }
    if (!strcasecmp(work, "AT+CIPSTART"))    { h_cipstart(argv, argc); return; }
    if (!strcasecmp(work, "AT+CIPSEND"))     { h_cipsend(argv, argc); return; }
    if (!strcasecmp(work, "AT+CIPMODE"))     { h_cipmode(argv, argc); return; }

    er();
}

/* ---------------- AT 任务 ---------------- */
static void at_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];

    for (;;) {
        /* 必须清零：驱动返回的字节数一旦大于实际填充量，
           残留的旧字节会被当成新命令/数据重复处理（表现为
           CIPSEND 发送后最后一个字节泄漏到下一条命令）。 */
        memset(buf, 0, sizeof(buf));
        int n = uart_read_bytes(AT_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n <= 0) {
            if (s_plus_armed && xTaskGetTickCount() > s_plus_deadline) {
                s_plus_n = 0;
                s_plus_armed = 0;
                s_cipmode = 0;
                sock_set_transparent(0);
                at_out("\r\n");
            }
            continue;
        }

        for (int i = 0; i < n; i++) {
            uint8_t c = buf[i];
            /* 命令间隔保护：窗口内丢弃晚到的残留字节；窗口一过立即失效 */
            if (s_guard_until && !sock_is_transparent()) {
                if ((int32_t)(xTaskGetTickCount() - s_guard_until) >= 0) {
                    s_guard_until = 0;          /* 窗口已过，正常处理 */
                } else if (c == '\r' || c == '\n') {
                    s_guard_until = 0;          /* 残留结束 */
                } else {
                    continue;                   /* 丢弃残留字节 */
                }
            }

/* --- 透传模式：裸数据直发 socket ---
   同样按块发送（收到什么就整块发），UDP 才不会被拆成单字节包。*/
if (sock_is_transparent() && sock_is_open() && s_send_remaining == 0) {
                if (c == '+') {
                    if (s_plus_n < 3) s_plus_buf[s_plus_n++] = '+';
                    if (s_plus_n == 3) {
                        s_plus_armed = 1;
                        s_plus_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(100);
                    }
                } else {
                    if (s_plus_n) {
                        sock_send(s_plus_buf, s_plus_n);
                        s_plus_n = 0;
                        s_plus_armed = 0;
                    }
                    uint8_t one = c;
                    sock_send(&one, 1);
                }
                continue;
            }

/* --- AT+CIPSEND=<len> 的数据阶段 ---
   必须先把 len 个字节缓冲起来再一次性 sock_send。
   早期实现每收到一个字节就 sock_send(&c,1)，对 TCP 尚可，
   但 UDP 会被拆成 N 个单字节报文（服务端收到 ['H','E','L','L','O']
   而不是一个 HELLO），且每包一次系统调用，效率也差。 */
if (s_send_remaining > 0) {
    if (s_send_filled < (int)sizeof(s_send_buf)) s_send_buf[s_send_filled++] = c;
    s_send_remaining--;
    if (s_send_remaining == 0) {
        sock_send(s_send_buf, s_send_filled);
        char b[64];
        snprintf(b, sizeof(b), "\r\nRecv %d bytes\r\nSEND OK\r\n", s_send_total);
        at_out(b);
        s_send_filled = 0;
        /* 命令间隔保护：吸收晚到的残留字节 */
        s_guard_until = xTaskGetTickCount() + pdMS_TO_TICKS(250);
    }
    continue;
}

            /* --- 行组装 --- */
            if (c == '\r' || c == '\n') {
                if (s_len > 0) {
                    s_line[s_len] = 0;
                    handle_line(s_line);
                    s_len = 0;
                    /* 一条命令到行尾即止。必须 break：否则同一批里的
                       后续字节（如 AT+CIPSEND=n\\r\\n 的 \\n）会被
                       当成 CIPSEND 的 payload 吃掉，导致真实数据被挤出。 */
                    break;
                }
            } else if (c == '\b' || c == 0x7F) {
                if (s_len > 0) s_len--;
            } else if (s_len < LINE_MAX - 1) {
                s_line[s_len++] = (char)c;
            }
        }
    }
    vTaskDelete(NULL);
}

/* ---------------- 初始化 ---------------- */
static void sink(const uint8_t *d, int len) { at_out_raw(d, len); }

esp_err_t at_init(void)
{
    uart_config_t uc = {
        .baud_rate  = AT_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t e = uart_driver_install(AT_UART_NUM, 4096, 4096, 0, NULL, 0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    e = uart_param_config(AT_UART_NUM, &uc);
    if (e != ESP_OK) return e;

    s_tx_mutex = xSemaphoreCreateMutex();
    if (!s_tx_mutex) return ESP_FAIL;

    sock_set_sink(sink);
    e = sock_init();
    if (e != ESP_OK) return e;

    if (xTaskCreate(at_task, "at_task", 6144, NULL, 6, NULL) != pdPASS) return ESP_FAIL;
    return ESP_OK;
}