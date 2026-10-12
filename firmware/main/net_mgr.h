#ifndef NET_MGR_H
#define NET_MGR_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_netif.h"

esp_err_t   net_init(void);

/* 以太网状态 */
bool        net_eth_started(void);
bool        net_eth_connected(void);
esp_netif_t *net_eth_netif(void);

/* WiFi 状态 */
bool        net_wifi_started(void);
esp_netif_t *net_sta_netif(void);
esp_err_t    net_dhcp_err(void);   /* 最近一次 dhcpc_start 的返回码（诊断用） */
esp_netif_t *net_ap_netif(void);
bool        net_sta_connected(void);

/* 取 IP（成功 true，ip 为 "a.b.c.d"） */
bool        net_get_ip(esp_netif_t *ni, char *ip, size_t ip_len);

/* 取 MAC（大写冒号分隔，失败输出空串） */
void        net_get_eth_mac(char *mac, size_t len);
void        net_get_sta_mac(char *mac, size_t len);
void        net_get_ap_mac(char *mac, size_t len);

/* WiFi 控制 */
esp_err_t   net_wifi_start_sta(void);
esp_err_t   net_wifi_start_ap(void);
esp_err_t   net_wifi_stop_sta(void);
esp_err_t   net_wifi_stop_ap(void);
esp_err_t   net_sta_connect(const char *ssid, const char *pwd);
/* 后台重试连接（AT 指令立即返回 OK，不阻塞）；失败时打印扫描结果 */
esp_err_t   net_sta_connect_async(const char *ssid, const char *pwd, int retries);

/* 以太网重配（DHCP / 静态） */
esp_err_t   net_eth_reconfigure(void);

#endif /* NET_MGR_H */