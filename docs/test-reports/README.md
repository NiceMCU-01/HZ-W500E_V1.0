# WT32-ETH01 AT 固件测试报告

本目录存放 WT32-ETH01（ESP32-C3）AT 指令固件的自动化测试产物。

## 目录内容

| 文件 | 说明 |
|---|---|
| `AT指令测试报告.html` | **主要交付物**，浏览器直接打开即可查看完整报告 |
| `at_test_report.csv` | 表格数据，可用Excel / pandas 二次分析 |
| `at_test_report.json` | 结构化数据，含每条用例的命令原文、期望与完整应答 |

三份文件内容一致，仅格式不同。

## 本次测试结论

| 项目 | 数值 |
|---|---|
| 被测固件 | WT32-ETH01_AT_V1.2（ESP-IDF v5.3.2 / ESP32-C3） |
| 覆盖指令 | 16 条 × 10轮 = 420 条用例 |
| 通过率 | **100.0% (420/420)**，零失败 |
| 测试日期 | 2025-10-12 |
| 串口 | `<COM_PORT>` @115200 8N1 |

覆盖的指令：`AT` / `AT+VER` / `AT+RST` / `AT+CIFSR` / `AT+CWJAP` / `AT+CWJAP?` /
`AT+CWMODE?` / `AT+CWQAP?` / `AT+CIPSERVER` / `AT+CIPSEND`（TCP/UDP） /
`AT+CIPSTART`（TCPC / UDPC / TCPS / HTPC）/ 透传模式收发 / UDP 回显链路。

## 数据脱敏说明

报告中的环境相关值已全部替换为占位符：

| 占位符 | 原始含义 |
|---|---|
| `<SSID>` | 测试用WiFi SSID |
| `<PASSWORD>` | 测试用WiFi 密码 |
| `<HOST_IP>` | 测试服务端（测试机）的局域网 IP |
| `<LOCAL_IP>` | 被测设备 STA 获取到的 IP |
| `<MAC>` / `<AP_BSSID>` | 设备 MAC 与 AP BSSID |
| `<EPHEMERAL_PORT>` | TCP 客户端随机源端口 |
| `<COM_PORT>` | 串口号 |

`AT+CWJAP?` 应答中的 RSSI/信道等参数保留原值，便于判断信号质量与信道。
`+CIFSR:APIP,"192.168.4.1"` 为 SoftAP 默认网段，属于公开常量，予以保留。

测试脚本也已改为从环境变量读取凭据（见 `.env.example`），仓库中不含任何真实账号、密码或内网地址。

## 关键修复记录（本次测试发现并解决的固件缺陷）

| # | 缺陷 | 根因 | 修复 |
|---|---|---|---|
| 1 | `esp_wifi_connect()` 阻塞 AT 命令任务 | 在 AT 任务里同步调 WiFi | 独立 `sta_conn_task` |
| 2 | 断线自触发重连死循环 | 单一 busy 标志无法区分触发源 | 拆分 `s_sta_busy` / `s_sta_retry` |
| 3 | STA 从未启动 DHCP 客户端 | 缺 `dhcp_start()` | 在 `WIFI_EVENT_STA_CONNECTED` 中启动 |
| 4 | `CWJAP?` 的 `ap.ssid` 未限长，SSID 输出为空 | memcpy 目标缓冲33 字节 | 按 `sizeof(ap.ssid)` 限长拷贝 |
| 5 | UDP 报文被拆成 N 个单字节 | `CIPSEND` 逐字节调用 `send()` | 缓冲后整块发送 |
| 6 | 重连风暴导致 AP 侧拉黑 MAC | 无退避，失败后立即重试 | 指数退避 1s→2s→4s→8s→15s 封顶 |
| 7 | **连接数上去后全部失败** | 套接字泄漏（对端关闭只置 `fd=-1` 不 `close`；`accept` 覆盖 listen fd）+ `CONFIG_LWIP_MAX_SOCKETS` 仅 8 | 补 `close()`、新增 `lfd` 保存 listen fd、上限提到 16、`TIME_WAIT` 超时降到 3s |
| 8 | `CIPSTART` 成功率低（2/10） | `connect()` 返回 `EINPROGRESS` 就当成功，未等三次握手完成 | 新增 `connect_wait()`：`select` + `FD_ISSET` + `getsockopt(SO_ERROR)` |
| 9 | 报错 errno 为无意义值 | lwIP `connect` 失败时保留上次 errno | 调用前清零 `errno`，失败后以 `SO_ERROR` 为准 |
| 10 | HTPC 命令稳定失败 | connect 前设置 `SO_RCVTIMEO/SO_SNDTIMEO` 干扰握手 | 移除这两项，改用 `select` 控制读取超时 |

> 注意：lwIP 的 errno 数值与 Linux 不同 —— lwIP 中 `EINPROGRESS` 是 **115**、
> `EALREADY` 是 114，而 **118 是 `ENOTNAM`**（无意义码），它出现即说明 errno 是残留值。

同时修复了 3 处测试框架自身缺陷（`read_until()` 过早超时返回、`CWJAP` 后未等 STA 拿到 IP、
`is_ok()` 要求 `OK` 必须在应答末尾），否则会产生大量假失败。

### 诊断手段

AT 固件**不能开启日志**（日志与 AT 应答共用 UART0，会污染应答），因此失败原因通过
`+CIPERR,<stage>,<errno>` 随应答一起输出。`stage` 含义：
`1`=socket、`2`=connect、`3`=bind、`4`=listen、`5`=send。
本次定位到两条决定性线索：`+CIPERR,1,23`（ENFILE，套接字额度耗尽）与
`+CIPERR,2,118`（errno 残留）。

## 在 GitHub 上查看

`docs/test-reports/AT指令测试报告.html` 无法在 GitHub 页面内直接渲染，
可下载后本地打开；或使用 [htmlpreview.github.io](https://htmlpreview.github.io/)
一类工具在线预览。