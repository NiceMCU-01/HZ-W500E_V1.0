# WiFi 连接稳定性验证脚本
# 目的：验证指数退避是否解决"连测几轮后彻底连不上"的问题。
# 做法：连续 N 轮 AT+CWJAP -> 查 AT+CIFSR，静置若干秒观察是否掉线。
import serial, time, sys

# 凭据与端口一律来自环境变量，不写入仓库：
#   WT32_COM_PORT / WT32_WIFI_SSID / WT32_WIFI_PWD / WT32_HOST_IP
import os

PORT = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("WT32_COM_PORT", "COM3")
ROUNDS = int(sys.argv[2]) if len(sys.argv) > 2 else 8
SSID = os.environ.get("WT32_WIFI_SSID", "")
PWD  = os.environ.get("WT32_WIFI_PWD", "")

def drain(s, wait=0.4):
    t0 = time.time(); buf = b""
    while time.time() - t0 < wait:
        try:
            d = s.read_all()
            if d: buf += d; t0 = time.time()
        except Exception:
            pass
    return buf.decode("utf-8", "ignore")

s = serial.Serial(PORT, 115200, timeout=0.2)
try:
    s.reset_input_buffer()
    s.write(b"AT\r\n"); time.sleep(0.5)
    print("[boot]", drain(s).replace("\r\n", " ").strip()[:80])

    s.write(b'AT+CWJAP="%s","%s"\r\n' % (SSID.encode(), PWD.encode()))
    time.sleep(0.6)
    print("[join]", drain(s, 6).replace("\r\n", " ").strip()[:120])

    ok = 0
    for i in range(1, ROUNDS + 1):
        s.write(b"AT+CIFSR\r\n"); time.sleep(0.3)
        r = drain(s)
        compact = r.replace("\r\n", " ").strip()
        got_ip = "192.168." in compact
        dhcp = [t for t in compact.split() if t.startswith("STADHCP")]
        print("  round %2d  ip=%-5s  %s" % (i, "YES" if got_ip else "NO", dhcp[0] if dhcp else "(no dhcp line)"))
        if got_ip: ok += 1
        if i < ROUNDS:
            print("           idle 10s ...")
            time.sleep(10)

    print("\n=== 汇总：%d/%d 轮拿到 IP ===" % (ok, ROUNDS))
finally:
    s.close()