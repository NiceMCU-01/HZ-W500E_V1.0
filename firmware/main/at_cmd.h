#ifndef AT_CMD_H
#define AT_CMD_H

#include <stdint.h>
#include <esp_err.h>

esp_err_t at_init(void);

/* 供 socket 层把收到的网络数据写到串口 */
void at_out_raw(const uint8_t *d, int len);

#endif /* AT_CMD_H */