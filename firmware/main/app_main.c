#include <stdio.h>
#include <nvs_flash.h>
#include <esp_err.h>

#include "at_config.h"
#include "net_mgr.h"
#include "at_cmd.h"

void app_main(void)
{
    /* NVS */
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }

    /* 读取保存的参数（首次运行用默认值） */
    cfg_load(cfg_get());

    /* W5500 以太网 + WiFi */
    net_init();

    /* AT 指令任务 */
    at_init();
}