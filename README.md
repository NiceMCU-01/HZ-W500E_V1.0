# WT32-ETH01 AT 固件

WT32-ETH01（ESP32-C3）AT 指令集固件的源码与自动化测试报告。

## 目录结构

```
├── docs/test-reports/       测试报告产物（HTML / CSV / JSON）+ 说明文档
├── firmware/                ESP-IDF v5.3.2 固件工程
│   ├── main/                    应用源码（AT 命令、WiFi 管理、套接字管理）
│   ├── CMakeLists.txt
│   ├── partitions.csv
│   └── sdkconfig.defaults
└── test/                    测试与诊断脚本（Python）
```

## 测试结论

10 轮全量测试，16 条 AT 指令共 **420 条用例全部通过（100.0%）**，
过程中定位并修复 10 个固件缺陷 + 3 个测试框架缺陷。
完整数据与修复记录见 [`docs/test-reports/README.md`](docs/test-reports/README.md)。

查看报告（下载后本地打开）：
[`docs/test-reports/AT指令测试报告.html`](docs/test-reports/AT指令测试报告.html)

## 固件构建

需要 ESP-IDF v5.3.2：

```bash
cd firmware
idf.py set-target esp32c3
idf.py build
idf.py -p <COM_PORT> flash monitor
```

> 修改 `sdkconfig.defaults` 后需删除 `sdkconfig` 全量重编才会生效。

### 关键配置

| 配置 | 值 | 说明 |
|---|---|---|
| `CONFIG_LWIP_MAX_SOCKETS` | 16 | AT 固件频繁开关 TCP 连接，lwIP 还会为 TIME_WAIT 保留额度，默认 8 不够 |
| `CONFIG_LWIP_TCP_CLOSE_TIMEOUT_MS` | 3000 | 加快 TIME_WAIT 回收，减少额度占用 |

### 日志说明

**AT 固件不要开启日志。** 日志与 AT 应答共用 UART0，会污染应答。
失败诊断通过 `+CIPERR,<stage>,<errno>` 随应答输出：
`stage` = 1 socket / 2 connect / 3 bind / 4 listen / 5 send。

## 运行测试

测试脚本从环境变量读取配置，不含任何硬编码凭据：

```bash
pip install pyserial

export WT32_COM_PORT=COM3        # 串口号
export WT32_WIFI_SSID=<你的SSID>
export WT32_WIFI_PWD="<你的密码>"
export WT32_HOST_IP=             # 留空则自动探测本机局域网地址

python test/run_at_tests.py      # 10 轮全量测试
python test/make_report.py       # 生成报告到 test/
```

单项诊断脚本：

| 脚本 | 用途 |
|---|---|
| `wifi_stability.py` | 连续多轮 CWJAP 验证指数退避是否解决「连测几轮后彻底连不上」 |
| `wifi_diag.py` | 定点诊断：CWJAP 返回 OK 后链路是否真的 up、设备 IP 是否漂移 |
| `tcp_diag.py` | 连续 CIPSTART TCP 稳定性复测 |

## 数据脱敏

仓库内所有数据均已脱敏，环境相关的值以占位符表示
（`<SSID>`、`<PASSWORD>`、`<HOST_IP>`、`<LOCAL_IP>`、`<MAC>`、`<AP_BSSID>`、
`<EPHEMERAL_PORT>`、`<COM_PORT>`），对照表见
[`docs/test-reports/README.md`](docs/test-reports/README.md)。