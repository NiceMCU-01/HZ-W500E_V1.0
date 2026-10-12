# 定点诊断：CWJAP 返回 OK 后链路是否真的 up？设备 IP 是否漂移？
import serial, time, sys, re

# 凭据与端口一律来自环境变量，不写入仓库：
#   WT32_COM_PORT / WT32_WIFI_SSID / WT32_WIFI_PWD / WT32_HOST_IP
import os
sys.stdout.reconfigure(encoding="utf-8")

PORT = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("WT32_COM_PORT", "COM3")
ROUNDS = int(sys.argv[2]) if len(sys.argv) > 2 else 6
SSID = os.environ.get("WT32_WIFI_SSID", "")
PWD  = os.environ.get("WT32_WIFI_PWD", "")

def rt(s, cmd, wait):
    s.reset_input_buffer()
    s.write(cmd.encode() + b"\r\n")
    t0 = time.time(); buf = b""
    while time.time() - t0 < wait:
        d = s.read_all()
        if d:
            buf += d; t0 = time.time()
        else:
            time.sleep(0.02)
    return buf.decode("utf-8", "ignore")

s = serial.Serial(PORT, 115200, timeout=0.2)
ips = set()
try:
    rt(s, "AT", 1.0)
    for i in range(1, ROUNDS + 1):
        t0 = time.time()
        r = rt(s, 'AT+CWJAP="%s","%s"' % (SSID, PWD), 8)
        dt = time.time() - t0
        ok = r.rstrip().endswith("OK")
        print("r%-2d CWJAP -> %-5s (%.1fs)" % (i, "OK" if ok else "FAIL", dt))

        # CWJAP 后立刻查状态
        q = rt(s, "AT+CWJAP?", 3)
        st = "?"
        for line in q.splitlines():
            line = line.strip()
            if line.startswith("+CWJAP:"):
                m = re.search(r':(\d+)', line)
                st = m.group(1) if m else "?"
        print("     CWJAP? status=%s %s" % (st, "OK" if st == "3" else "<-- NOT CONNECTED"))

        f = rt(s, "AT+CIFSR", 3)
        dev = re.findall(r"(\d+\.\d+\.\d+\.\d+)", f.replace("STADHCP", ""))
        dh = re.search(r"STADHCP,(\d+),(-?\d+)", f.replace("\r\n", ""))
        print("     CIFSR sta=%s  dhcp=%s" % (dev[0] if dev else "NONE",
                                            dh.groups() if dh else "?"))
        if dev: ips.add(dev[0])
        time.sleep(3)
finally:
    s.close()

print("\n出现过的设备 IP: %s" % (sorted(ips) or "无"))