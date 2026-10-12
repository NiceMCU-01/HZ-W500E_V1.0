#include "net_mgr.h"
#include "at_config.h"
#include "board_pins.h"

#include <esp_eth.h>
#include <esp_eth_netif_glue.h>
#include <esp_eth_mac.h>
#include <esp_eth_phy.h>
#include <esp_eth_driver.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_log.h>
#include <freertos/semphr.h>
#include <lwip/ip4_addr.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* ---------------- 状态 ---------------- */
static esp_eth_handle_t s_eth_h      = NULL;
static esp_netif_t     *s_eth_ni     = NULL;
static esp_netif_t     *s_sta_ni     = NULL;
static esp_netif_t     *s_ap_ni      = NULL;
static bool s_eth_started   = false;
static bool s_eth_link      = false;
static bool s_wifi_started  = false;
static bool s_sta_connected = false;
static bool s_sta_got_ip    = false;
static bool s_sta_busy      = false;   /* 显式请求连接（AT+CWJAP / 启动） */
static bool s_sta_retry     = false;   /* 掉线后请求重连 */
static esp_err_t s_dhcp_err = ESP_OK;  /* dhcpc_start 的返回码，供诊断 */

/* ---------------- WiFi 连接任务 ----------------
   esp_wifi_connect() 在 C3 上是阻塞调用（内部等认证结果，最长数十秒，
   期间芯片 ROM 会打印 "busy p..."）。若直接在 AT 命令任务里调用，会把整个
   串口命令处理卡住，表现为 CWJAP 无响应、后续命令返回 busy。
   因此把连接动作放进这个低优先级任务，AT 层只负责发起。

   注意 s_sta_busy 与 s_sta_retry 必须分开：任务自己调用的 esp_wifi_disconnect()
   也会触发 STA_DISCONNECTED 事件，若事件把同一个标志置位，任务循环就会
   立刻再次断线重连，形成永远连不上的抖动。 */
static void sta_conn_task(void *arg)
{
    at_config_t *c = (at_config_t *)arg;
    int backoff_ms = 0;                /* 自动重连的指数退避时长 */
    for (;;) {
        /* 等待有人发起连接 */
        while (!s_sta_busy && !s_sta_retry) {
            backoff_ms = 0;            /* 网络稳定下来后退避复位 */
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        /* 先清标志，再做任何会触发断线事件的操作 */
        bool explicit_req = s_sta_busy; /* 用户主动 AT+CWJAP：不受退避影响 */
        s_sta_busy  = false;
        s_sta_retry = false;

        /* 退避只作用于"掉线后自动重连"。没有退避时，本任务每轮循环都会
           disconnect + connect（间隔仅 200~300ms），AP 侧会把它判定为连接
           洪泛并拉黑该 MAC，表现为"一开始能连上、连测几轮后彻底连不上"。 */
        if (!explicit_req && backoff_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        }

        if (!s_wifi_started) {
            if (net_wifi_start_sta() != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        }
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));      /* 给 AP 侧留出重连间隔 */

        wifi_config_t wc;
        memset(&wc, 0, sizeof(wc));
        strncpy((char *)wc.sta.ssid, c->sta_ssid, sizeof(wc.sta.ssid) - 1);
        strncpy((char *)wc.sta.password, c->sta_pwd, sizeof(wc.sta.password) - 1);
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;

        if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) continue;
        esp_wifi_connect();   /* 阻塞在这里是安全的：不在 AT 任务里 */

        /* 等到连上为止；若中途断线，DISCONNECTED 事件会置 s_sta_retry，
           由下一轮循环接手重连 */
        for (int t = 0; t < 300 && !s_sta_connected && !s_sta_retry; t++) {
            /* 关联成功后再确保一次 DHCP 客户端在跑。
               esp_netif_create_default_wifi_sta() 只创建 netif，不会自动启动
               dhcpc；之前在 net_wifi_start_sta() 里启动过，但实测状态仍是
               STOPPED(0)，这里在链路真正 up 之后再拉一次更可靠。 */
            if (s_sta_connected && s_sta_ni && cfg_get()->dhcp_en) {
                esp_netif_dhcp_status_t st;
                if (esp_netif_dhcpc_get_status(s_sta_ni, &st) == ESP_OK &&
                    st == ESP_NETIF_DHCP_STOPPED) {
                    esp_netif_dhcpc_start(s_sta_ni);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        /* 指数退避：1s → 2s → 4s → 8s → 15s（封顶）。连接成功即复位。 */
        if (s_sta_connected) {
            backoff_ms = 0;
        } else {
            backoff_ms = backoff_ms ? (backoff_ms * 2) : 1000;
            if (backoff_ms > 15000) backoff_ms = 15000;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ---------------- 事件 ---------------- */
static void eth_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == ETHERNET_EVENT_CONNECTED)         s_eth_link = true;
    else if (id == ETHERNET_EVENT_DISCONNECTED) s_eth_link = false;
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            s_sta_connected = false;
            if (cfg_get()->sta_ssid[0]) s_sta_busy = true;   /* 交给 sta_conn_task */
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            s_sta_connected = true;
            /* 关联成功的标准时机：在这里启动 STA 的 DHCP 客户端。
               esp_netif_create_default_wifi_sta() 只创建 netif，不会自动启动
               dhcpc，漏掉这一步就会出现"连上 AP 但 STAIP 为空"。
               放在事件里最稳妥，涵盖 AT 发起 / 自动重连 / 开机自连三条路径。 */
            if (s_sta_ni && cfg_get()->dhcp_en) {
                s_dhcp_err = esp_netif_dhcpc_start(s_sta_ni);
            }
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_sta_connected = false;
            s_sta_got_ip    = false;
            /* 自动重连：用独立的 s_sta_retry，避免与"自己触发的断线"互相打架 */
            if (cfg_get()->sta_ssid[0]) s_sta_retry = true;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            s_sta_got_ip = true;
            /* 注意：不要在这里无条件 esp_netif_set_default_netif(s_sta_ni)。
               实测这么做反而让 CIPSTART 成功率从 2/10 降到 0/10（AP 网卡拿不到
               默认路由后，套接字走W5500 静态地址 192.168.2.250 反而全失败）。
               默认路由归属留待进一步定位，暂不改动。 */
        } else if (id == IP_EVENT_ETH_GOT_IP) {
            /* 以太网插线并拿到 IP 时才切默认路由，这是官方推荐的优先级做法 */
            if (s_eth_ni) esp_netif_set_default_netif(s_eth_ni);
        }
    }
}

/* ---------------- SPI + W5500 ---------------- */
static esp_err_t spi_eth_bus_init(void)
{
    /* 用中断脚，必须先装 GPIO ISR 服务 */
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;

    spi_bus_config_t buscfg = {
        .mosi_io_num     = PIN_W5500_MOSI,
        .miso_io_num     = PIN_W5500_MISO,
        .sclk_io_num     = PIN_W5500_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .data4_io_num    = -1,
        .data5_io_num    = -1,
        .data6_io_num    = -1,
        .data7_io_num    = -1,
        .max_transfer_sz = 0,
        .flags           = SPICOMMON_BUSFLAG_MASTER,
    };
    e = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    return ESP_OK;
}

static esp_err_t eth_start(void)
{
    if (s_eth_h) return ESP_OK;

    esp_err_t e = spi_eth_bus_init();
    if (e != ESP_OK) return e;

    spi_device_interface_config_t spi_devcfg = {
        .mode           = 0,
        .clock_speed_hz = PIN_W5500_SPI_MHZ * 1000 * 1000,
        .spics_io_num   = PIN_W5500_CS,
        .queue_size     = 20,
    };

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr       = PIN_W5500_PHY_ADDR;
    phy_config.reset_gpio_num = PIN_W5500_RST;   /* 低有效，驱动自己拉复位 */

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &spi_devcfg);
    w5500_config.int_gpio_num   = PIN_W5500_INT;
    w5500_config.poll_period_ms = 0;              /* 用中断，不轮询 */

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (!mac) return ESP_FAIL;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (!phy) { mac->del(mac); return ESP_FAIL; }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    e = esp_eth_driver_install(&eth_config, &s_eth_h);
    if (e != ESP_OK) {
        mac->del(mac);
        phy->del(phy);
        s_eth_h = NULL;
        return e;
    }

    /* ESP-IDF 5.x：ESP_NETIF_DEFAULT_ETH() 自带 ETH 相关配置，无需再设 flags */
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_ni = esp_netif_new(&cfg);
    if (!s_eth_ni) {
        esp_eth_driver_uninstall(s_eth_h);
        s_eth_h = NULL;
        return ESP_FAIL;
    }
    /* 把网卡和协议栈 glue 关联起来 */
    esp_netif_attach(s_eth_ni, esp_eth_new_netif_glue(s_eth_h));

    s_eth_started = true;
    return ESP_OK;
}

/* DHCP / 静态 IP 下发 */
esp_err_t net_eth_reconfigure(void)
{
    if (!s_eth_ni) return ESP_FAIL;
    at_config_t *c = cfg_get();

    if (c->dhcp_en) {
        esp_netif_dhcpc_start(s_eth_ni);
    } else {
        esp_netif_dhcpc_stop(s_eth_ni);
        esp_netif_ip_info_t ip;
        memset(&ip, 0, sizeof(ip));
        ip.ip.addr     = ipaddr_addr(c->eth_ip);
        ip.gw.addr     = ipaddr_addr(c->eth_gw);
        ip.netmask.addr = ipaddr_addr(c->eth_mask);
        if (ip.ip.addr == 0) return ESP_ERR_INVALID_ARG;
        return esp_netif_set_ip_info(s_eth_ni, &ip);
    }
    return ESP_OK;
}

/* ---------------- WiFi ---------------- */
esp_err_t net_wifi_start_sta(void)
{
    if (s_sta_ni) return ESP_OK;

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&ic);
    if (e != ESP_OK) return e;

    e = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            wifi_event_handler, NULL, NULL);
    if (e != ESP_OK) return e;
    e = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                            wifi_event_handler, NULL, NULL);
    if (e != ESP_OK) return e;

    s_sta_ni = esp_netif_create_default_wifi_sta();
    s_ap_ni  = esp_netif_create_default_wifi_ap();

    at_config_t *c = cfg_get();
    e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e != ESP_OK) return e;

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strncpy((char *)wc.sta.ssid, c->sta_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, c->sta_pwd, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;

    e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (e != ESP_OK) return e;

    e = esp_wifi_start();
    if (e != ESP_OK) return e;
    s_wifi_started = true;

    /* STA 的 DHCP 客户端：esp_netif_create_default_wifi_sta() 只建 netif，
       不会自动启动 dhcpc，必须显式start，否则连上 AP 也拿不到 IP。
       （以太网那边已经这样做了，WiFi 之前漏了） */
    at_config_t *c2 = cfg_get();
    if (c2->dhcp_en) esp_netif_dhcpc_start(s_sta_ni);

    if (c->sta_ssid[0]) s_sta_busy = true;
    return ESP_OK;
}

esp_err_t net_wifi_start_ap(void)
{
    if (!s_wifi_started) {
        esp_err_t e = net_wifi_start_sta();
        if (e != ESP_OK) return e;
    }
    return esp_wifi_set_mode(s_ap_ni ? WIFI_MODE_APSTA : WIFI_MODE_AP);
}

esp_err_t net_wifi_stop_sta(void)
{
    if (!s_wifi_started) return ESP_OK;
    s_sta_connected = false;
    s_sta_got_ip    = false;
    return esp_wifi_disconnect();
}

esp_err_t net_wifi_stop_ap(void)
{
    if (!s_wifi_started) return ESP_OK;
    return esp_wifi_set_mode(WIFI_MODE_STA);
}

esp_err_t net_sta_connect(const char *ssid, const char *pwd)
{
    (void)ssid; (void)pwd;
    if (!s_wifi_started) {
        esp_err_t e = net_wifi_start_sta();
        if (e != ESP_OK) return e;
    }
    /* 只置标志，真正的阻塞连接交给 sta_conn_task */
    s_sta_busy = true;
    return ESP_OK;
}

bool net_sta_connected(void) { return s_sta_connected && s_sta_got_ip; }

/* ---------------- 查询 ---------------- */
bool net_eth_started(void)   { return s_eth_started; }
bool net_eth_connected(void) { return s_eth_link; }
bool net_wifi_started(void)  { return s_wifi_started; }
esp_netif_t *net_eth_netif(void) { return s_eth_ni; }
esp_netif_t *net_sta_netif(void) { return s_sta_ni; }

esp_err_t net_dhcp_err(void) { return s_dhcp_err; }
esp_netif_t *net_ap_netif(void)  { return s_ap_ni; }

bool net_get_ip(esp_netif_t *ni, char *ip, size_t ip_len)
{
    if (ip && ip_len) ip[0] = 0;
    if (!ni) return false;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(ni, &info) != ESP_OK) return false;
    if (info.ip.addr == 0) return false;
    if (ip && ip_len) snprintf(ip, ip_len, IPSTR, IP2STR(&info.ip));
    return true;
}

static void fmt_mac(const uint8_t *b, char *mac, size_t len)
{
    snprintf(mac, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             b[0], b[1], b[2], b[3], b[4], b[5]);
}

void net_get_eth_mac(char *mac, size_t len)
{
    uint8_t b[6] = { 0 };
    if (mac && len) mac[0] = 0;
    if (!s_eth_h) return;
    if (esp_eth_ioctl(s_eth_h, ETH_CMD_G_MAC_ADDR, b) == ESP_OK) fmt_mac(b, mac, len);
}

void net_get_sta_mac(char *mac, size_t len)
{
    uint8_t b[6] = { 0 };
    if (mac && len) mac[0] = 0;
    if (esp_wifi_get_mac(WIFI_IF_STA, b) == ESP_OK) fmt_mac(b, mac, len);
}

void net_get_ap_mac(char *mac, size_t len)
{
    uint8_t b[6] = { 0 };
    if (mac && len) mac[0] = 0;
    if (esp_wifi_get_mac(WIFI_IF_AP, b) == ESP_OK) fmt_mac(b, mac, len);
}

/* W5500 没有出厂 MAC（不像普通网卡），必须软件写入一个，否则读回全 0。
   放在 WiFi 初始化之后调用，用 STA MAC 末字节 +1 派生，保证每台设备唯一。 */
static void eth_fixup_mac(void)
{
    if (!s_eth_h) return;
    uint8_t m[6] = { 0 };
    if (esp_eth_ioctl(s_eth_h, ETH_CMD_G_MAC_ADDR, m) != ESP_OK) return;
    for (int i = 0; i < 6; i++) if (m[i]) return;      /* 已有有效 MAC */

    uint8_t base[6] = { 0 };
    if (esp_wifi_get_mac(WIFI_IF_STA, base) == ESP_OK) {
        /* 末字节 +1 会和 SoftAP 的 MAC 撞车（AP/STA 仅末字节相差 1）。
           改为把首字节置本地管理位(bit1)，既唯一又不会与 AP/STA 冲突。 */
        m[0] = (uint8_t)(base[0] | 0x02);
        m[1] = base[1]; m[2] = base[2]; m[3] = base[3]; m[4] = base[4];
        m[5] = base[5];
        esp_eth_ioctl(s_eth_h, ETH_CMD_S_MAC_ADDR, m);
    }
}

/* AT+CWJAP 按规范只需"响应 OK"，不做后台重试。
   早期版本用后台任务反复 esp_wifi_disconnect/connect，在 WiFi 一直连不上时
   会触发 FreeRTOS 断言 xTaskPriorityDisinherit（连不上多半是没接天线）。 */

esp_err_t net_sta_connect_async(const char *ssid, const char *pwd, int retries)
{
    (void)retries;
    return net_sta_connect(ssid, pwd);
}

/* ---------------- 初始化 ---------------- */
esp_err_t net_init(void)
{
    esp_err_t e;

    e = esp_netif_init();
    if (e != ESP_OK) return e;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;

    /* 以太网（模块主功能，始终启动） */
    e = eth_start();
    if (e != ESP_OK) return e;
    e = esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                            eth_event_handler, NULL, NULL);
    if (e != ESP_OK) return e;
    net_eth_reconfigure();

    /* STA 连接任务（承载阻塞的 esp_wifi_connect） */
    xTaskCreate(sta_conn_task, "sta_conn", 4096, cfg_get(), 3, NULL);

    /* WiFi 按 cwmode */
    switch (cfg_get()->cwmode) {
    case 0: break;
    case 1: net_wifi_start_sta(); break;
    case 2: net_wifi_start_ap();  break;
    default: net_wifi_start_sta(); net_wifi_start_ap(); break;
    }

    eth_fixup_mac();
    return ESP_OK;
}