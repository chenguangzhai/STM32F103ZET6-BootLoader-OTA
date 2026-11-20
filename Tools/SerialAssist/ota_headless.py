#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""无界面 OTA 发送端（调试 / CI 用）

和 GUI 的关系：**协议打包不重复实现**，直接 import serial_assist 里的
build_fwup_header / build_ota_tail。这样只有一处字节定义，不会漂移。

两种模式：

  boot-ota <bin>   老协议，目标 = BootLoader
                   SWD 复位 → 裸字节流 → 12 字节 OTAE 尾 → Boot 自己校验跳转
                   适用：把"带 bug 的 App"换成新的，因为旧 App 还不会收新协议

  app-ota  <bin>   新协议，目标 = 正在运行的 App
                   32 字节 FWUP 头 → 等 300ms → 裸字节流 → App 自己写槽+复位
                   不碰内部 Flash，下载全程旧固件完好

  flash-boot <bin> 直接经 SWD 把 bin 烧到 0x08000000（BootLoader 自己）。
                   Boot 不能自己升级自己，改 Boot 只能走这条路。
                   会先开串口再去烧，这样上电日志一条都不漏。

  watch            不发送任何数据，只经 SWD 复位一次并把上电日志看满。
                   查"上电到底发生了什么"用它，比 flash-boot 轻。

用法：
  python ota_headless.py app-ota ../../Application/build/Debug/Application.bin
  python ota_headless.py boot-ota xxx.bin --port COM25 --chunk 256 --delay 1

退出码：0 = 看到成功标志；1 = 超时或无成功标志；2 = 参数/环境错误
"""
from __future__ import print_function

import argparse
import os
import struct
import sys
import threading
import time

try:
    import serial
except ImportError:
    print("缺 pyserial：C:/Python311/python.exe -m pip install pyserial")
    sys.exit(2)

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

# 协议定义复用 GUI 那份，不在这里另写
from serial_assist import (          # noqa: E402
    build_fwup_header, build_ota_tail, find_openocd, openocd_reset,
)


class Reader(threading.Thread):
    """后台收串口并按行打印，带相对时间戳。"""

    def __init__(self, ser):
        threading.Thread.__init__(self)
        self.daemon = True
        self.ser = ser
        self.stop_flag = False
        self.lines = []
        self.last_rx = time.time()
        self._buf = b""

    def run(self):
        while not self.stop_flag:
            try:
                chunk = self.ser.read(256)
            except Exception:
                break
            if not chunk:
                continue
            self.last_rx = time.time()
            self._buf += chunk
            while b"\n" in self._buf:
                line, self._buf = self._buf.split(b"\n", 1)
                self._emit(line)
        if self._buf:
            self._emit(self._buf)

    def _emit(self, raw):
        text = raw.decode("utf-8", "replace").rstrip("\r")
        self.lines.append(text)
        print("  | " + text)

    @staticmethod
    def _is_history(line):
        """黑匣子回放的历史行，格式 "[log] #12 拒绝降级 v=5 ..."。

        判定设备状态时必须跳过它们：历史里"上次被拒绝降级"这件事，
        和"这一次被拒绝"完全是两回事，混在一起就会误判。
        """
        return line.startswith("[log]")

    def wait_for(self, needle, timeout):
        """等某行出现，返回是否等到。"""
        end = time.time() + timeout
        while time.time() < end:
            if any(needle in l for l in self.lines if not self._is_history(l)):
                return True
            time.sleep(0.02)
        return any(needle in l for l in self.lines if not self._is_history(l))

    def wait_quiet(self, quiet_s, hard_s):
        """等到串口安静 quiet_s 秒，或总时长超过 hard_s。"""
        end = time.time() + hard_s
        while time.time() < end:
            if time.time() - self.last_rx > quiet_s:
                return
            time.sleep(0.05)

    def saw(self, needle):
        return any(needle in l for l in self.lines if not self._is_history(l))


def send_stream(ser, data, chunk, delay_ms, label):
    """按块发送裸字节流，和 GUI 的节奏一致。"""
    total = len(data)
    sent = 0
    t0 = time.time()
    while sent < total:
        block = data[sent:sent + chunk]
        ser.write(block)
        sent += len(block)
        if delay_ms > 0:
            time.sleep(delay_ms / 1000.0)
    dt = time.time() - t0
    print("  [tx] {} {:,} B / {:.2f}s ({:.1f} KB/s)".format(
        label, total, dt, (total / 1024.0) / max(dt, 1e-6)))


def flash_boot(rd, path, base=0x08000000, timeout=60):
    """经 SWD 把 bin 烧到内部 Flash。Boot 只能这样更新。"""
    import subprocess
    exe, scr = find_openocd()
    if not exe:
        print("找不到 openocd，无法烧录")
        return False
    icfg = os.path.join(scr, "interface", "cmsis-dap.cfg")
    tcfg = os.path.join(scr, "target", "stm32f1x.cfg")
    # openocd 走 Tcl 解析，路径里的反斜杠会被当转义，统一换成正斜杠
    binpath = path.replace("\\", "/")
    cmd = [exe, "-f", icfg, "-f", tcfg,
           "-c", "program {} 0x{:08X} verify reset exit".format(binpath, base)]
    flags = 0x08000000 if os.name == "nt" else 0
    print("  [swd] program {} -> 0x{:08X}".format(os.path.basename(path), base))
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, creationflags=flags)
    except Exception as e:
        print("  [swd] 失败：{}".format(e))
        return False
    out = (r.stdout or "") + (r.stderr or "")
    for line in out.splitlines():
        if any(k in line for k in ("Written", "Verified", "Error", "error",
                                   "** Programming", "** Verified")):
            print("  [swd] " + line.strip())
    return r.returncode == 0 and "Verified" in out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["boot-ota", "app-ota", "flash-boot", "watch"])
    ap.add_argument("binfile", nargs="?", help="watch 模式不需要")
    ap.add_argument("--port", default="COM25")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--chunk", type=int, default=256)
    ap.add_argument("--delay", type=float, default=1.0, help="块间延时 ms")
    ap.add_argument("--ver", type=int, default=1,
                    help="固件版本号 1..65535，写进 FWUP 头，Boot 用它拦降级")
    args = ap.parse_args()

    if not (1 <= args.ver <= 65535):
        print("版本号必须在 1..65535 之间，当前 {}".format(args.ver))
        return 2

    data = b""
    crc = 0
    path = None
    if args.mode != "watch":
        if not args.binfile:
            print("{} 模式必须指定 bin 文件".format(args.mode))
            return 2
        path = os.path.abspath(args.binfile)
        if not os.path.isfile(path):
            print("文件不存在：{}".format(path))
            return 2
        with open(path, "rb") as f:
            data = f.read()
        import zlib
        crc = zlib.crc32(data) & 0xFFFFFFFF
        print("固件 {}  {:,} B  crc=0x{:08X}".format(
            os.path.basename(path), len(data), crc))
    print("模式 {}  端口 {} @ {}".format(args.mode, args.port, args.baud))
    print("-" * 60)

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.05,
                            write_timeout=5)
    except Exception as e:
        print("打开串口失败：{}".format(e))
        print(">>> 串口可能被串口助手占着，先把助手里的「关闭串口」点掉 <<<")
        return 2
    # 显式放开 DTR/RTS：有些板子把 DTR 接到 NRST，默认断言会把板子按在复位里
    try:
        ser.dtr = False
        ser.rts = False
    except Exception:
        pass

    rd = Reader(ser)
    rd.start()
    ok = False

    try:
        if args.mode == "watch":
            exe, scr = find_openocd()
            if not exe:
                print("找不到 openocd，无法自动复位；请手动按复位键")
                return 2
            print("SWD 复位，观察上电日志 ...")
            openocd_reset(exe, scr)
            # 复位后序列：打印 → 搬运（静默几百 ms）→ 3 s 上电窗口（静默）
            # → 跳转 → App 打印。静默阈值必须大于 3 s 那个窗口，
            # 否则会在窗口期被判"结束"，把 jump 和 App 的输出全漏掉
            rd.wait_quiet(5.0, 30.0)
            return 0

        if args.mode == "flash-boot":
            print("[1/2] 经 SWD 烧 BootLoader 到 0x08000000 ...")
            if not flash_boot(rd, path):
                print("烧录失败，板子仍是原来那份 Boot")
                return 2
            print("[2/2] 等上电日志（reset run 已让它重跑）...")
            rd.wait_for("[ota] 就绪", 8.0)
            rd.wait_quiet(0.5, 5.0)
            return 0 if rd.saw("[ota] 就绪") else 1

        if args.mode == "boot-ota":
            exe, scr = find_openocd()
            if not exe:
                print("找不到 openocd，无法自动复位。请手动按一下复位键，"
                      "并在 3 秒窗口内重跑本命令。")
                return 2
            print("[1/3] SWD 复位，让板子进入 Boot 的 3 秒接收窗口 ...")
            good, info = openocd_reset(exe, scr)
            print("      {}".format(info))
            if not good:
                return 2
            # 等 Boot 打印 ready，说明它已经开好 DMA 在等数据了
            if rd.wait_for("[ota] 就绪", 3.0):
                print("[2/3] Boot 就绪，开始发送")
            else:
                print("[2/3] 没看到 '[ota] ready'，仍然尝试发送（可能日志被截）")
            send_stream(ser, data, args.chunk, args.delay, "裸字节流")
            time.sleep(0.05)
            ser.write(build_ota_tail(len(data), crc))
            print("  [tx] OTAE 尾 12 B")
            print("[3/3] 等 Boot 回读校验 ...")
            rd.wait_for("校验通过", 8.0)
            rd.wait_quiet(0.4, 3.0)
            ok = rd.saw("校验通过") and rd.saw("跳转到 App")

        else:  # app-ota
            hdr = build_fwup_header(args.ver, len(data), crc)
            print("[1/3] 发 32 字节 FWUP 头（版本 {}）：{}".format(
                args.ver,
                " ".join("{:02X}".format(b) for b in hdr[:8]) + " ..."))
            ser.write(hdr)
            # 等 App 擦完头部扇区再发数据（W25Q 扇区擦除典型 45ms）
            time.sleep(0.3)
            print("[2/3] 开始发送固件")
            send_stream(ser, data, args.chunk, args.delay, "固件数据")
            print("[3/3] 等 App 校验、写头、自复位 ...")
            rd.wait_for("复位回 BootLoader", 6.0)
            # App 复位后会走 Boot：读槽 → 搬运（静默几百 ms）→ 3 s 窗口 → 跳转 → App 打印。
            # 静默阈值给到 5 s，是为了跨过那 3 s 上电窗口；给 0.5 s 会在
            # 搬运/窗口期间就判定结束，把 [apply] done 和 jump 全漏掉（踩过一次）
            rd.wait_quiet(5.0, 30.0)
            ok = (rd.saw("头部已写入") and rd.saw("[slot] A：")
                  and (rd.saw("跳转到 App") or rd.saw("编译时间")))
            if rd.saw("拒绝降级"):
                print("!! Boot 拒绝了这次升级：版本 {} 低于板子当前的版本，"
                      "固件已收进槽 A 但没有搬运。换个更高的版本号重发即可"
                      .format(args.ver))
                ok = False

    finally:
        rd.stop_flag = True
        time.sleep(0.1)
        try:
            ser.close()
        except Exception:
            pass

    print("-" * 60)
    if ok:
        print("结果：成功（看到了预期标志）")
        return 0
    print("结果：没看到预期标志，请看上面的设备输出")
    return 1


if __name__ == "__main__":
    sys.exit(main())
