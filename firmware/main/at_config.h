#ifndef AT_CONFIG_H
#define AT_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

typedef struct {
    int baudrate;        /* 9600..921600，默认 115200 */
    int databits;        /* 5..8，默认 8 */
    int stopbits;        /* 1/2/3 => 1bit / 1.5bit / 2bit（按文档编码） */
    int parity;          /* 0=None 1=Odd 2=Even */
    int flow_control;    /* 0=None 1=RTS 2=CTS 3=RTS+CTS */
    int pass_channel;    /* 1..6 */
    int cwmode;          /* 0=关闭 1=STA 2=AP 3=AP+STA */
    int dhcp_mode;       /* 0=SoftAP 1=Station 2=AP+STA 3=ETH */
    int dhcp_en;         /* 0/1 */
    char eth_ip[16];
    char eth_gw[16];
    char eth_mask[16];
    char sta_ssid[33];
    char sta_pwd[65];
} at_config_t;

void        cfg_defaults(at_config_t *c);
esp_err_t   cfg_load(at_config_t *c);
esp_err_t   cfg_save(const at_config_t *c);
esp_err_t   cfg_erase(void);
void        cfg_store(at_config_t *c);

/* 单例访问（cfg_load 后有效） */
at_config_t *cfg_get(void);

#endif /* AT_CONFIG_H */