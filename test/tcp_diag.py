# 定向复测：连续 CIPSTART TCP 是否稳定？失败时能否复用连接？
import serial, socket, threading, time, sys

# 凭据与端口一律来自环境变量，不写入仓库：
#   WT32_COM_PORT / WT32_WIFI_SSID / WT32_WIFI_PWD / WT32_HOST_IP
import os
sys.stdout.reconfigure(encoding="utf-8")

PORT = os.environ.get("WT32_COM_PORT", "COM3")
HOST = os.environ.get("WT32_HOST_IP", "127.0.0.1")
TCPP = 8888
SSID = os.environ.get("WT32_WIFI_SSID", "")
PWD  = os.environ.get("WT32_WIFI_PWD", "")
N = int(sys.argv[1]) if len(sys.argv) > 1 else 10

hits = []
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((HOST, TCPP)); srv.listen(16); srv.settimeout(0.5)

def acceptor():
    while True:
        try: c, a = srv.accept()
        except Exception: continue
        hits.append(time.time())
        try:
            c.settimeout(1.0)
            d = c.recv(200)
            if d: c.sendall(b"HELLO-FROM-SERVER")
            time.sleep(0.2)
        except Exception: pass
        finally: c.close()

threading.Thread(target=acceptor, daemon=True).start()

def rt(s, cmd, wait):
    s.reset_input_buffer()
    s.write(cmd.encode() + b"\r\n")
    t0 = time.time(); buf = b""
    while time.time() - t0 < wait:
        d = s.read_all()
        if d: buf += d; t0 = time.time()
        else: time.sleep(0.02)
    return buf.decode("utf-8", "ignore")

s = serial.Serial(PORT, 115200, timeout=0.2)
okc = 0
try:
    rt(s, "AT", 1.0)
    # 必须先关联 WiFi：模块刚烧录重启时并未关联，此时直接 CIPSTART 必然失败
    rt(s, 'AT+CWJAP="%s","%s"' % (SSID, PWD), 10)
    # 必须等 STA 真正拿到 IP。注意只能匹配 STAIP：CIFSR 里始终存在
    # APIP "192.168.4.1"，若匹配 "192.168." 会在 STA 未关联时就误判为就绪。
    import re
    for _ in range(20):
        f = rt(s, "AT+CIFSR", 3)
        m = re.search(r'\+CIFSR:STAIP,"(\d+\.\d+\.\d+\.\d+)"', f)
        if m and m.group(1) != "0.0.0.0":
            print("[wifi] STA IP =", m.group(1))
            break
        time.sleep(0.5)
    else:
        print("[wifi] 警告：STA 未获取到 IP，后续 CIPSTART 必然失败")

    for i in range(1, N + 1):
        n0 = len(hits)
        t0 = time.time()
        # 12s：CIPSTART 要走完TCP 握手（实测最长约 7s），等待过短会误判失败
        r = rt(s, 'AT+CIPSTART="TCPC","%s",%d' % (HOST, TCPP), 12)
        dt = time.time() - t0
        ok = r.rstrip().endswith("OK")
        if ok: okc += 1
        print("r%-2d CIPSTART -> %-5s %.1fs  srv_conn=%+d" % (i, "OK" if ok else "FAIL", dt, len(hits) - n0))
        if ok:
            got = rt(s, "AT+CIPSEND=5\r\nHELLO", 3)
            print("      send: %s" % got.replace("\r\n", " ").strip()[:60])
            print("      recv: %s" % rt(s, "", 1.5).replace("\r\n", " ").strip()[:60])
            rt(s, "AT+CIPCLOSE", 3)
        time.sleep(1)
    print("\nCIPSTART 成功 %d/%d，服务端累计接受连接 %d" % (okc, N, len(hits)))
finally:
    s.close(); srv.close()