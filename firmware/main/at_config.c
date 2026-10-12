#include "at_config.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <string.h>
#include <stdio.h>

#define NS      "atcfg"
#define K_BAUD  "baudrate"
#define K_DATA  "databits"
#define K_STOP  "stopbits"
#define K_PAR   "parity"
#define K_FLOW  "flow"
#define K_PASS  "pass_channel"
#define K_CWM   "cwmode"
#define K_DM    "dhcp_mode"
#define K_DE    "dhcp_en"
#define K_IP    "eth_ip"
#define K_GW    "eth_gw"
#define K_MASK  "eth_mask"
#define K_SSID  "sta_ssid"
#define K_PWD   "sta_pwd"

static at_config_t s_cfg;

void cfg_defaults(at_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->baudrate     = 115200;
    c->databits     = 8;
    c->stopbits     = 1;
    c->parity       = 0;
    c->flow_control = 0;
    c->pass_channel = 1;
    c->cwmode       = 3;
    c->dhcp_mode    = 3;
    c->dhcp_en      = 1;
    snprintf(c->eth_ip,   sizeof(c->eth_ip),   "192.168.1.200");
    snprintf(c->eth_gw,   sizeof(c->eth_gw),   "192.168.1.1");
    snprintf(c->eth_mask, sizeof(c->eth_mask), "255.255.255.0");
    c->sta_ssid[0] = 0;
    c->sta_pwd[0]  = 0;
}

static void rd_i(nvs_handle_t h, const char *k, int *v, int def)
{
    int32_t t = def;
    if (nvs_get_i32(h, k, &t) == ESP_OK) *v = (int)t;
}

static void rd_s(nvs_handle_t h, const char *k, char *v, size_t n)
{
    size_t len = n;
    if (nvs_get_str(h, k, v, &len) != ESP_OK) v[0] = 0;
}

esp_err_t cfg_load(at_config_t *c)
{
    cfg_defaults(c);
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
    if (e != ESP_OK) return ESP_OK; /* 首次运行：用默认值 */

    rd_i(h, K_BAUD, &c->baudrate,     c->baudrate);
    rd_i(h, K_DATA, &c->databits,     c->databits);
    rd_i(h, K_STOP, &c->stopbits,     c->stopbits);
    rd_i(h, K_PAR,  &c->parity,       c->parity);
    rd_i(h, K_FLOW, &c->flow_control, c->flow_control);
    rd_i(h, K_PASS, &c->pass_channel, c->pass_channel);
    rd_i(h, K_CWM,  &c->cwmode,       c->cwmode);
    rd_i(h, K_DM,   &c->dhcp_mode,    c->dhcp_mode);
    rd_i(h, K_DE,   &c->dhcp_en,      c->dhcp_en);
    rd_s(h, K_IP,   c->eth_ip,   sizeof(c->eth_ip));
    rd_s(h, K_GW,   c->eth_gw,   sizeof(c->eth_gw));
    rd_s(h, K_MASK, c->eth_mask, sizeof(c->eth_mask));
    rd_s(h, K_SSID, c->sta_ssid, sizeof(c->sta_ssid));
    rd_s(h, K_PWD,  c->sta_pwd,  sizeof(c->sta_pwd));

    nvs_close(h);
    return ESP_OK;
}

esp_err_t cfg_save(const at_config_t *c)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_i32(h, K_BAUD, c->baudrate);
    nvs_set_i32(h, K_DATA, c->databits);
    nvs_set_i32(h, K_STOP, c->stopbits);
    nvs_set_i32(h, K_PAR,  c->parity);
    nvs_set_i32(h, K_FLOW, c->flow_control);
    nvs_set_i32(h, K_PASS, c->pass_channel);
    nvs_set_i32(h, K_CWM,  c->cwmode);
    nvs_set_i32(h, K_DM,   c->dhcp_mode);
    nvs_set_i32(h, K_DE,   c->dhcp_en);
    nvs_set_str(h, K_IP,   c->eth_ip);
    nvs_set_str(h, K_GW,   c->eth_gw);
    nvs_set_str(h, K_MASK, c->eth_mask);
    nvs_set_str(h, K_SSID, c->sta_ssid);
    nvs_set_str(h, K_PWD,  c->sta_pwd);
    e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t cfg_erase(void)
{
    esp_err_t e = nvs_erase_all(NS);
    if (e != ESP_OK) return e;
    return nvs_commit(NS);
}

at_config_t *cfg_get(void)
{
    return &s_cfg;
}

void cfg_store(at_config_t *c)
{
    s_cfg = *c;
}