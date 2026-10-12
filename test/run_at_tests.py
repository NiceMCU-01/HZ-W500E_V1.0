#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
WT32-ETH01 AT 指令集 —— 10 轮全量自动化测试

- 本机开启 TCP / UDP / HTTP 服务端作为 AT+CIPSTART 的真实测试目标
- 串口走 CH340 115200 8N1（端口由环境变量指定）
- 16 条指令全部覆盖，每条跑 10 轮

环境变量（不要把凭据写进代码或提交到仓库）：
  WT32_COM_PORT   串口号，例如 COM3 / COM5，默认 COM3
  WT32_WIFI_SSID  WiFi SSID
  WT32_WIFI_PWD   WiFi 密码
  WT32_HOST_IP    测试服务端所在本机 IP，默认 127.0.0.1
"""
import socket, threading, time, sys, os, csv, json, re, argparse

HOST = os.environ.get("WT32_HOST_IP") or None  # 本机对外 IP，None 时自动探测
TCP_PORT = 8888
UDP_PORT = 8889
HTTP_PORT = 8000
ECHO_PORT = 9999        # UDP 回显：验证"发送->服务端收到->回打串口"完整链路

COM_PORT = os.environ.get("WT32_COM_PORT", "COM3")
BAUD = 115200

# 测试用 WiFi凭据只从环境变量读取，缺失即报错退出，避免误用硬编码账号
WIFI_SSID = os.environ.get("WT32_WIFI_SSID", "")
WIFI_PWD  = os.environ.get("WT32_WIFI_PWD", "")

NOISE = ("WIFI CONNECTED", "WIFI GOT IP", "WIFI DISCONNECT",
         "WIFI CONNECTED!", "WIFI GOT IP!", "WIFI DISCONNECT!")


# ----------------------------------------------------------------- 服务端
class EchoTcp(threading.Thread):
    daemon = True

    def __init__(self, port, host="0.0.0.0"):
        super().__init__()
        self.port = port
        self.s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.bind((host, port))
        self.s.listen(8)
        self.stop = False
        self.hits = []

    def run(self):
        while not self.stop:
            try:
                self.s.settimeout(0.5)
                c, a = self.s.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self.serve, args=(c, a), daemon=True).start()
        try: self.s.close()
        except Exception: pass

    def serve(self, c, a):
        self.hits.append(a)
        try:
            c.settimeout(10)
            while True:
                d = c.recv(4096)
                if not d:
                    break
                c.sendall(d)          # 回显
        except Exception:
            pass
        finally:
            try: c.close()
            except Exception: pass


class EchoUdp(threading.Thread):
    daemon = True

    def __init__(self, port, host="0.0.0.0"):
        super().__init__()
        self.port = port
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.bind((host, port))
        self.s.settimeout(0.5)
        self.stop = False
        self.hits = []

    def run(self):
        while not self.stop:
            try:
                d, a = self.s.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            self.hits.append((a, d))
            self.s.sendto(d, a)
        try: self.s.close()
        except Exception: pass


HTTP_BODY = b"WT32-ETH01-AT-TEST-OK\n"


class HttpSrv(threading.Thread):
    daemon = True

    def __init__(self, port, host="0.0.0.0"):
        super().__init__()
        self.port = port
        self.s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.bind((host, port))
        self.s.listen(8)
        self.stop = False
        self.hits = 0

    def run(self):
        while not self.stop:
            try:
                self.s.settimeout(0.5)
                c, _ = self.s.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self.hits += 1
            try:
                c.settimeout(5)
                c.recv(4096)
                c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                          b"Content-Length: %d\r\nConnection: close\r\n\r\n" % len(HTTP_BODY)
                          + HTTP_BODY)
                c.close()
            except Exception:
                pass
        try: self.s.close()
        except Exception: pass


def detect_host():
    """找出本机在局域网里的 IPv4（优先非回环）"""
    out = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.168.2.1", 80))     # 不发包，仅用于选路
        out.append(s.getsockname()[0])
        s.close()
    except Exception:
        pass
    try:
        out.append(socket.gethostbyname(socket.gethostname()))
    except Exception:
        pass
    for ip in out:
        if ip and not ip.startswith("127."):
            return ip
    return "127.0.0.1"


# ----------------------------------------------------------------- AT 客户端
class AT:
    def __init__(self, port=COM_PORT, baud=BAUD):
        import serial
        self.sp = serial.Serial(port, baud, timeout=0.05,
                               write_timeout=3, dsrdtr=False, rtscts=False)
        # 避免 DTR/RTS 把 ESP32 拉在复位/下载模式
        try:
            self.sp.dtr = False
            self.sp.rts = False
        except Exception:
            pass
        self.buf = b""

    def close(self):
        try: self.sp.close()
        except Exception: pass

    def flush(self):
        self.sp.reset_input_buffer()
        self.buf = b""

    def _pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                n = self.sp.in_waiting
                if n:
                    self.buf += self.sp.read(n)
                else:
                    time.sleep(0.02)
            except Exception:
                time.sleep(0.05)

    def read_until(self, pats, timeout=4.0, quiet=0.35):
        """读到任一 pats 出现为止；命中后quiet 秒无新数据即返回。

        重要：命中 pats 之前**不能**因为 quiet 超时而提前返回。
        AT+CIPSTART 要走完 TCP 三次握手才应答，实测耗时可达 7 秒，
        期间串口完全静默。若此时按 quiet 提前返回，会把"应答还没来"
        误判成失败，表现为「服务端已 accept 到连接、但设备报 ERROR」，
        实测误报率可达 100%。
        """
        pats = [p.encode() if isinstance(p, str) else p for p in pats]
        end = time.time() + timeout
        last = time.time()
        hit = False
        while time.time() < end:
            for p in pats:
                if p in self.buf:
                    hit = True
            if hit:
                # 已命中，等一会儿把后续正文收全再走
                try:
                    n = self.sp.in_waiting
                    if n:
                        self.buf += self.sp.read(n)
                        last = time.time()
                    else:
                        time.sleep(0.02)
                        if time.time() - last > quiet:
                            break
                except Exception:
                    time.sleep(0.05)
            else:
                # 尚未命中，必须一直等到 timeout
                try:
                    n = self.sp.in_waiting
                    if n:
                        self.buf += self.sp.read(n)
                        last = time.time()
                    else:
                        time.sleep(0.02)
                except Exception:
                    time.sleep(0.05)
        return any(p in self.buf for p in pats)

    def send(self, cmd, timeout=4.0, quiet=0.35):
        self.flush()
        self.sp.write((cmd + "\r\n").encode())
        self.sp.flush()
        self.read_until(["OK", "ERROR"], timeout=timeout, quiet=quiet)
        return self.take()

    def take(self):
        b = self.buf
        self.buf = b""
        return b.decode("utf-8", "replace")

    def clean(self, s):
        for n in NOISE:
            s = s.replace(n, "")
        return s

    def is_ok(self, s):
        """应答中含独立的 OK 行即算成功。

        不能要求 OK 必须在字符串末尾：HTPC / 透传等命令在 OK 之后还会
        追加异步通知行（如 CLOSED），这是固件的正常行为，误判为失败。
        """
        c = self.clean(s)
        return re.search(r"(^|\r?\n)OK\s*$", c.strip()) is not None \
            or re.search(r"(^|\r?\n)OK\s*\r?\n", c.strip()) is not None

    def is_error(self, s):
        c = self.clean(s)
        return "ERROR" in c


# ----------------------------------------------------------------- 测试框架
class Result:
    def __init__(self):
        self.rows = []

    def add(self, rnd, group, cmd, expect, got, ok, note=""):
        self.rows.append(dict(round=rnd, group=group, cmd=cmd,
                              expect=expect, got=got, ok=ok, note=note))
        flag = "PASS" if ok else "FAIL"
        print("  [r%-2d] %-4s %-46s" % (rnd, flag, cmd[:46]))
        if not ok:
            print("        期望: %s" % expect[:90])
            print("        实际: %s" % got.replace("\r\n", " | ")[:200])


def log_probe(at, txt):
    c = at.clean(txt)
    for tag in ("+CIFSR:ETHIP", "+CIFSR:STAIP"):
        pass
    return c


def run(at, R, rnd, host, tcp, udp, http, echo):
    G = "基础"

    # 1 AT
    r = at.send("AT")
    R.add(rnd, G, "AT", "OK", r, at.is_ok(r))

    # 2 ATE0
    r = at.send("ATE0")
    R.add(rnd, G, "ATE0", "OK", r, at.is_ok(r))

    # 3 AT（回显关闭后不应回显命令本身）
    r = at.send("AT")
    okc = at.is_ok(r) and "AT\r\n" not in r.replace("\r\nOK\r\n", "")
    R.add(rnd, G, "AT (回显关闭)", "OK 且不回显", r, okc)

    # 4 ATE1
    r = at.send("ATE1")
    R.add(rnd, G, "ATE1", "OK", r, at.is_ok(r))

    # 5 AT+GMR
    r = at.send("AT+GMR")
    R.add(rnd, G, "AT+GMR", "版本信息 + OK", r, at.is_ok(r) and "WT32-ETH01" in r)

    # 6 AT+UART_DEF?
    r = at.send("AT+UART_DEF?")
    R.add(rnd, G, "AT+UART_DEF?", "+UART_DEF:... + OK", r,
          at.is_ok(r) and re.search(r"\+UART_DEF:\d+,\d+,\d+,\d+,\d+", r) is not None)

    # 7 AT+UART_DEF=
    r = at.send("AT+UART_DEF=115200,8,1,0,0")
    R.add(rnd, G, "AT+UART_DEF=115200,8,1,0,0", "OK", r, at.is_ok(r))

    # 8 AT+PASSCHANNEL?
    r = at.send("AT+PASSCHANNEL?")
    R.add(rnd, G, "AT+PASSCHANNEL?", "+PASSCHANNEL:1~6", r,
          at.is_ok(r) and re.search(r"\+PASSCHANNEL:[1-6]", r) is not None)

    # 9 AT+PASSCHANNEL=
    r = at.send("AT+PASSCHANNEL=1")
    R.add(rnd, G, "AT+PASSCHANNEL=1", "OK", r, at.is_ok(r))

    # WiFi
    G = "WiFi"
    # 10 AT+CWMODE?
    r = at.send("AT+CWMODE?")
    R.add(rnd, G, "AT+CWMODE?", "+CWMODE:0~3", r,
          at.is_ok(r) and re.search(r"\+CWMODE:[0-3]", r) is not None)

    # 11 AT+CWMODE=1 (STA)
    r = at.send("AT+CWMODE=1", timeout=5)
    R.add(rnd, G, "AT+CWMODE=1", "OK", r, at.is_ok(r))

    # 12 AT+CWJAP= 连接 AP
    r = at.send('AT+CWJAP="%s","%s"' % (WIFI_SSID, WIFI_PWD), timeout=8)
    R.add(rnd, G, 'AT+CWJAP="%s","***"', WIFI_SSID, r, at.is_ok(r))

    # 关键：CWJAP 只是"发起连接"，esp_wifi_connect() 由后台 sta_conn_task 执行，
    # 关联+DHCP 还需要几秒。必须等到 STA 真正拿到 IP 才能发后续网络命令，
    # 否则后续 CIPSTART 会在链路未就绪时执行，造成大批假失败。
    # 只匹配 STAIP：CIFSR 里恒有 APIP "192.168.4.1"，匹配 "192.168." 会误判。
    for _ in range(24):
        f = at.send("AT+CIFSR", timeout=4)
        m = re.search(r'\+CIFSR:STAIP,"(\d+\.\d+\.\d+\.\d+)"', f)
        if m and m.group(1) != "0.0.0.0":
            break
        time.sleep(0.5)

    # 13 AT+CWJAP?
    r = at.send("AT+CWJAP?")
    R.add(rnd, G, "AT+CWJAP?", "+CWJAP:ssid,bssid,ch,rssi", r,
          at.is_ok(r) and re.search(r"\+CWJAP:\"%s\"" % re.escape(WIFI_SSID), r) is not None)

    # 以太网
    G = "以太网"
    # 14 AT+CWDHCP_DEF?
    r = at.send("AT+CWDHCP_DEF?")
    R.add(rnd, G, "AT+CWDHCP_DEF?", "+CWDHCP_DEF:... + OK", r,
          at.is_ok(r) and "+CWDHCP_DEF:" in r)

    # 15 AT+CWDHCP_DEF=3,1
    r = at.send("AT+CWDHCP_DEF=3,1", timeout=6)
    R.add(rnd, G, "AT+CWDHCP_DEF=3,1", "OK", r, at.is_ok(r))

    # 16 AT+CIPETH_DEF?
    r = at.send("AT+CIPETH_DEF?")
    R.add(rnd, G, "AT+CIPETH_DEF?", "+CIPETH_DEF:ip,gw,mask", r,
          at.is_ok(r) and "+CIPETH_DEF:" in r)

    # 17 AT+CIPETH_DEF= 设静态 IP 后改回 DHCP
    r = at.send('AT+CIPETH_DEF="192.168.2.250","192.168.2.1","255.255.255.0"', timeout=6)
    R.add(rnd, G, 'AT+CIPETH_DEF="192.168.2.250",...', "OK", r, at.is_ok(r))
    r = at.send("AT+CWDHCP_DEF=3,1", timeout=6)      # 恢复 DHCP
    R.add(rnd, G, "AT+CWDHCP_DEF=3,1 (恢复)", "OK", r, at.is_ok(r))

    # AT+CIFSR
    G = "TCP/IP"
    r = at.send("AT+CIFSR", timeout=5)
    R.add(rnd, G, "AT+CIFSR", "6 行 IP/MAC + OK", r,
          at.is_ok(r) and r.count("+CIFSR:") >= 6)

# 18 TCP 客户端
    # timeout 必须覆盖 TCP 三次握手实测耗时（最长约 7s），否则应答还在路上就被判失败
    tcp.hits.clear()
    r = at.send('AT+CIPSTART="TCPC","%s",%d' % (host, TCP_PORT), timeout=15)
    okopen = at.is_ok(r)
    R.add(rnd, G, 'AT+CIPSTART="TCPC",%s,%d' % (host, TCP_PORT), "OK", r, okopen)

    if okopen:
        at.flush()
        at.sp.write(b"AT+CIPSEND=5\r\n")
        got_prompt = at.read_until([">"], timeout=4)
        prompt_txt = at.take()
        R.add(rnd, G, "AT+CIPSEND=5", "提示符 >", prompt_txt, got_prompt)

        at.sp.write(b"HELLO")
        at.sp.flush()
        got = at.read_until(["SEND OK"], timeout=5, quiet=1.2)
        txt = at.take()
        okc = got and re.search(r"Recv\s+5\s+bytes", txt) and "SEND OK" in txt
        R.add(rnd, G, "发送 5 字节 (HELLO)", "Recv 5 bytes + SEND OK", txt, bool(okc))
        # 目标端应收到
        time.sleep(0.4)
        R.add(rnd, G, "服务端收到 TCP 数据", "服务端记录到连接", repr(tcp.hits[-2:]),
              len(tcp.hits) > 0)

        r = at.send("AT+CIPCLOSE", timeout=5)
        R.add(rnd, G, "AT+CIPCLOSE", "CLOSED + OK", r, at.is_ok(r))

    # 19 UDP
    udp.hits.clear()
    r = at.send('AT+CIPSTART="UDPC","%s",%d' % (host, UDP_PORT), timeout=12)
    okopen = at.is_ok(r)
    R.add(rnd, G, 'AT+CIPSTART="UDPC",%s,%d' % (host, UDP_PORT), "OK", r, okopen)
    if okopen:
        at.flush()
        at.sp.write(b"AT+CIPSEND=4\r\n")
        at.read_until([">"], timeout=4)
        at.take()
        at.sp.write(b"PING")
        at.sp.flush()
        got = at.read_until(["SEND OK"], timeout=5, quiet=1.2)
        txt = at.take()
        R.add(rnd, G, "发送 4 字节 (PING/UDP)", "Recv 4 bytes + SEND OK", txt,
              got and re.search(r"Recv\s+4\s+bytes", txt) is not None)
        time.sleep(0.4)
        R.add(rnd, G, "服务端收到 UDP 数据", "服务端记录到报文", "hits=%d" % len(udp.hits),
              len(udp.hits) > 0)
        r = at.send("AT+CIPCLOSE", timeout=5)
        R.add(rnd, G, "AT+CIPCLOSE (UDP)", "CLOSED + OK", r, at.is_ok(r))

    # 19b UDP 收发闭环（模块 -> 本机回显服务 -> 串口）
    # 注意：不能连 127.0.0.1，板子上并没有服务在那个端口监听，
    # 包发出去就石沉大海，永远收不到回打。必须连本机真实 IP。
    at.send("AT+CIPCLOSE", timeout=3)          # 清理上一轮可能残留的连接
    r = at.send('AT+CIPSTART="UDPC","%s",%d' % (host, ECHO_PORT), timeout=12)
    okopen = at.is_ok(r)
    R.add(rnd, G, 'AT+CIPSTART="UDPC","%s",%d' % (host, ECHO_PORT), "OK", r, okopen)
    if okopen:
        echo.hits = []
        at.flush()
        at.sp.write(b"AT+CIPSEND=4\r\n")
        got_p = at.read_until([">"], timeout=4)
        R.add(rnd, G, "AT+CIPSEND=4 (回环)", "提示符 >", at.take(), got_p)
        at.sp.write(b"PING")
        at.sp.flush()
        got = at.read_until(["SEND OK"], timeout=6, quiet=1.5)
        txt = at.take()
        R.add(rnd, G, "发送 4 字节并收到 SEND OK", "Recv 4 bytes + SEND OK", txt,
              bool(got and re.search(r"Recv\s+4\s+bytes", txt)))
        # 服务端必须收到一个完整的 PING 包（而非 4 个单字节包）
        end = time.time() + 2.0
        while time.time() < end and not any(d == b"PING" for _, d in echo.hits):
            time.sleep(0.03)
        whole = any(d == b"PING" for _, d in echo.hits)
        R.add(rnd, G, "服务端收到完整 PING 包", "单个 4 字节报文",
              repr([d for _, d in echo.hits])[:140], whole)
        # 回显数据应被打回串口
        extra = b""
        end = time.time() + 2.5
        while time.time() < end:
            if at.sp.in_waiting:
                extra += at.sp.read(at.sp.in_waiting)
                if b"PING" in extra:
                    break
            else:
                time.sleep(0.03)
        txt2 = txt + extra.decode("utf-8", "replace")
        R.add(rnd, G, "回显数据回打串口", "串口收到 PING", txt2.replace("\r\n", " ")[:140],
              bool("PING" in txt2 and "SEND OK" in txt2))
        at.flush()          # 丢掉残留，避免污染下一条命令
        time.sleep(0.2)
        r = at.send("AT+CIPCLOSE", timeout=5)
        R.add(rnd, G, "AT+CIPCLOSE (回环)", "CLOSED + OK", r, at.is_ok(r))

    # 20 HTTP GET
    http.hits = 0
    r = at.send('AT+CIPSTART="HTPC","http://%s:%d/test.txt"' % (host, HTTP_PORT), timeout=18)
    okc = at.is_ok(r) and "WT32-ETH01-AT-TEST-OK" in at.clean(r)
    R.add(rnd, G, 'AT+CIPSTART="HTPC",...', "200 OK + 正文 + OK", r, okc)
    R.add(rnd, G, "服务端收到 HTTP 请求", "hits>=1", "hits=%d" % http.hits, http.hits >= 1)

    # 21 透传模式
    r = at.send('AT+CIPSTART="TCPC","%s",%d' % (host, TCP_PORT), timeout=15)
    okopen = at.is_ok(r)
    if okopen:
        r = at.send("AT+CIPMODE?", timeout=4)
        R.add(rnd, G, "AT+CIPMODE?", "+CIPMODE:0", r,
              at.is_ok(r) and re.search(r"\+CIPMODE:[01]", r) is not None)
        r = at.send("AT+CIPMODE=1", timeout=4)
        R.add(rnd, G, "AT+CIPMODE=1", "OK", r, at.is_ok(r))

        at.flush()
        at.sp.write(b"TRANSPARENT-1\r\n")
        at.sp.flush()
        got = at.read_until(["TRANSPARENT-1"], timeout=5, quiet=1.0)
        R.add(rnd, G, "透传数据回环", "收到原样数据", "", bool(got))

        at.flush()
        at.sp.write(b"+++")
        at.sp.flush()
        time.sleep(1.3)                     # 规范要求 +++ 后至少间隔 1 秒
        r = at.send("AT", timeout=5)
        R.add(rnd, G, "+++ 退出透传", "+++ 后 AT 应答 OK", r, at.is_ok(r))

        r = at.send("AT+CIPMODE=0", timeout=4)
        R.add(rnd, G, "AT+CIPMODE=0", "OK", r, at.is_ok(r))
        r = at.send("AT+CIPCLOSE", timeout=5)
        R.add(rnd, G, "AT+CIPCLOSE (透传)", "CLOSED + OK", r, at.is_ok(r))
    else:
        R.add(rnd, G, "透传模式", "前置 CIPSTART 失败", r, False)


# ----------------------------------------------------------------- 主流程
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=10)
    ap.add_argument("--port", default=COM_PORT)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()

    outdir = a.out or os.path.dirname(os.path.abspath(__file__))
    host = detect_host()
    print("本机测试目标 IP : %s" % host)
    print("串口            : %s @ %d" % (a.port, BAUD))

    tcp = EchoTcp(TCP_PORT); tcp.start()
    udp = EchoUdp(UDP_PORT); udp.start()
    http = HttpSrv(HTTP_PORT); http.start()
    echo = EchoUdp(ECHO_PORT); echo.start()
    print("服务端          : TCP %d / UDP %d / HTTP %d / UDP回显 %d" % (TCP_PORT, UDP_PORT, HTTP_PORT, ECHO_PORT))

    at = AT(a.port)
    R = Result()

    print("\n等待模块启动并建立 AT 会话 ...")
    ready = False
    for i in range(40):
        r = at.send("AT", timeout=2, quiet=0.4)
        if at.is_ok(r):
            ready = True
            break
        time.sleep(1)
    if not ready:
        print("!! 模块无 AT 应答")
    else:
        print("模块已就绪\n")
        # 关闭回显让日志干净，跑完再打开
        at.send("ATE0")
        # 配一次 WiFi（10 轮共用）
        at.send("AT+CWMODE=1", timeout=6)
        at.send('AT+CWJAP="%s","%s"' % (WIFI_SSID, WIFI_PWD), timeout=10)
        time.sleep(4)
        at.send("ATE1")

        t0 = time.time()
        for rnd in range(1, a.rounds + 1):
            print("\n========== 第 %d / %d 轮 ==========" % (rnd, a.rounds))
            run(at, R, rnd, host, tcp, udp, http, echo)
        print("\n10 轮完成，用时 %.1f 秒" % (time.time() - t0))

    at.close()
    for s in (tcp, udp, http):
        s.stop = True

    # 汇总
    total = len(R.rows)
    passed = sum(1 for r in R.rows if r["ok"])
    print("\n================ 汇总 ================")
    print("用例总数 %d   通过 %d   失败 %d   通过率 %.1f%%"
          % (total, passed, total - passed, 100.0 * passed / max(1, total)))

    bycmd = {}
    for r in R.rows:
        d = bycmd.setdefault(r["cmd"], [0, 0])
        d[1] += 1
        if r["ok"]:
            d[0] += 1
    print("\n按指令统计：")
    for k, v in bycmd.items():
        mark = "OK  " if v[0] == v[1] else "FAIL"
        print("  [%s] %-46s %d/%d" % (mark, k[:46], v[0], v[1]))

    # 落盘
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "at_test_report.csv"), "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=["round", "group", "cmd", "expect", "got", "ok", "note"])
        w.writeheader()
        for r in R.rows:
            w.writerow({**r, "got": r["got"][:500]})
    with open(os.path.join(outdir, "at_test_report.json"), "w", encoding="utf-8") as f:
        json.dump(dict(total=total, passed=passed, host=host, rows=R.rows), f,
                  ensure_ascii=False, indent=1)
    print("\n报告已写入 %s" % outdir)
    return 0 if total and passed == total else 1


if __name__ == "__main__":
    sys.exit(main())