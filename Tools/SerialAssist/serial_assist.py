#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
================================================================================
 STM32 OTA 串口助手
--------------------------------------------------------------------------------
 专为 STM32F103ZET6 OTA BootLoader 工程写的串口调试工具。

 相比通用串口助手（SSCOM 等），它多做了五件事：

   1. 固定 UTF-8 解码     —— 日志里的中文不会再变 "寮曡剼"
   2. 固件分块发送        —— 块大小 / 块间延时 / 实时进度 / 随时中止
   3. 自动解析 BootLoader —— 抓 [ota] page / tail 的 sum 算真实落盘进度
   4. 字节数自动对账      —— 文件大小 vs BootLoader 回报的 sum，一眼看出丢没丢
   5. 日志按来源着色      —— [boot] 蓝 / [ota] 青 / [rst] 橙 / ok 绿 / FAIL 红

 依赖：pyserial（C:\Python311 已自带 3.5）
 运行：双击 run_serial_assist.bat
       或  C:\Python311\python.exe serial_assist.py
================================================================================
"""

import codecs
import glob
import json
import os
import queue
import re
import struct
import subprocess
import sys
import threading
import time
import tkinter as tk
import zlib
from tkinter import filedialog, messagebox, ttk

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    # --noconsole 打包后 sys.stderr 是 None，直接 write 会抛
    # AttributeError 把真正的报错盖掉，所以先判空。
    _err = ("缺少 pyserial。请执行：\n"
            "    C:\\Python311\\python.exe -m pip install pyserial\n")
    if sys.stderr is not None:
        sys.stderr.write(_err)
    raise


# ==============================================================================
# 常量
# ==============================================================================

APP_TITLE = "STM32 OTA 串口助手"
APP_VER = "1.0"

BAUD_LIST = ["9600", "19200", "38400", "57600", "115200",
             "230400", "460800", "921600"]

# 配色（浅色系，长时间看不刺眼）
C_BG      = "#F4F6F9"   # 窗口底色
C_PANEL   = "#FFFFFF"   # 面板底色
C_BORDER  = "#D9DEE6"
C_TEXT    = "#1F2733"
C_SUB     = "#6C7683"
C_ACCENT  = "#2563EB"
C_ACCENT2 = "#1D4ED8"
C_OK      = "#0F9D58"
C_WARN    = "#C77700"
C_BAD     = "#D93025"
C_BOOT    = "#7C3AED"
C_OTA     = "#0E7490"

FONT_UI   = "Microsoft YaHei UI"
FONT_MONO = "Consolas"

# 配置（串口、波特率、块大小这些记下来，下次打开不用重选）
#
# 打包成 exe 后 __file__ 指向 PyInstaller 的临时解包目录
# （%TEMP%\_MEIxxxx），程序一退出整个目录就被删掉，配置文件跟着丢。
# 所以这里要区分「源码运行」和「exe 运行」两种情形：
#   - 源码：脚本所在目录
#   - exe ：exe 自身所在目录（用户双击的那个文件夹）
def app_dir():
    if getattr(sys, "frozen", False):
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))


CONFIG_PATH = os.path.join(app_dir(), "assist_config.json")


def load_config():
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def save_config(cfg):
    try:
        with open(CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)
    except Exception:
        pass


# ==============================================================================
# BootLoader 日志解析规则
#   全部用 search（不是 match），因为一行前面可能带别的东西，
#   例如 "发送完毕！[ota] page 0x0800B000 ok, sum: 14336"
# ==============================================================================

RE_READY = re.compile(rb"\[ota\]\s+ready,\s*app=(0x[0-9A-Fa-f]+),\s*page=(\d+)B"
                      rb"(?:\s*,\s*proto=(\d+))?")
RE_PAGE  = re.compile(rb"\[ota\]\s+page\s+(0x[0-9A-Fa-f]+)\s+ok,\s*sum:\s*(\d+)")
RE_TAIL  = re.compile(rb"\[ota\]\s+tail\s+(0x[0-9A-Fa-f]+)\s+(\d+)B\s+ok,\s*sum:\s*(\d+)")
RE_ENTER = re.compile(rb"\[boot\]\s+enter\s+OTA")
RE_JUMP  = re.compile(rb"\[boot\]\s+jump\s+to\s+app\s+(0x[0-9A-Fa-f]+)")
RE_NOAPP = re.compile(rb"\[boot\]\s+no\s+valid\s+app")
RE_RST   = re.compile(rb"\[rst\]\s+(\w+)")
RE_OTAERR= re.compile(rb"\[ota\].*(?:FAIL|fail|error|ERR)")

# proto=2 才开始有"尾部记录 + CRC 校验"，老版本没有这一段
RE_TAILREC = re.compile(rb"\[ota\]\s+tail:\s*len=(\d+)\s+crc=0x([0-9A-Fa-f]+)")
RE_VOK     = re.compile(rb"\[ota\]\s+verify\s+OK\s*(\d+)B")
RE_VBAD    = re.compile(rb"\[ota\]\s+verify\s+FAIL")
RE_SIZEBAD = re.compile(rb"\[ota\]\s+size\s+mismatch")
RE_LEGACY  = re.compile(rb"\[boot\]\s+(?:no\s+param|legacy\s+mode)")
RE_CRCBAD  = re.compile(rb"\[boot\]\s+crc\s+mismatch")


def list_serial_ports():
    """返回 [(device, 显示文本, 是否真实USB串口), ...]

    要判断"哪个是真的插着的设备"，只能看 hwid：
    蓝牙串口的 hwid 形如 BTHENUM\\{...}，是系统虚拟出来的，
    一台机器上能有七八个，而且往往按 COM 号排在前面。
    真实 USB 转串口的 hwid 里带 "USB VID:PID=xxxx:xxxx"。
    只按描述里的 CH340 / CP210 关键字判断是不可靠的 ——
    很多板子用的是通用的 "USB 串行设备" 描述。
    """
    out = []
    for p in serial.tools.list_ports.comports():
        desc = (p.description or "").strip()
        hwid = p.hwid or ""
        low = desc.lower()
        is_bluetooth = ("BTHENUM" in hwid.upper() or "蓝牙" in desc
                        or "bluetooth" in low)
        is_usb = ("USB VID" in hwid.upper() or "USB\\VID" in hwid.upper())

        if desc and low not in ("n/a", "none"):
            label = "{}  {}".format(p.device, desc)
        else:
            label = p.device

        out.append((p.device, label, is_usb and not is_bluetooth))

    # 真实 USB 串口排最前，蓝牙虚拟口排最后
    out.sort(key=lambda x: (not x[2], x[0]))
    return out


def human_bytes(n):
    """1024 -> '1.0 KB'"""
    n = float(n)
    if n < 1024:
        return "{:.0f} B".format(n)
    if n < 1024 * 1024:
        return "{:.1f} KB".format(n / 1024.0)
    return "{:.2f} MB".format(n / (1024.0 * 1024.0))


# ------------------------------------------------------------------ 调试器复位
# 跑 SWD 复位的不一定是 DAP-Link：ST-Link、J-Link 都能干同一件事，
# 区别只在 OpenOCD 要加载哪个 interface 脚本。下面这张表按 USB 的
# VID/PID 认设备，顺序就是优先级（同时插多个时用排在前面的）。
DEBUGGERS = [
    ("VID_1FC9&PID_5601", "cmsis-dap", "DAP-Link"),
    ("VID_1FC9&PID_0132", "cmsis-dap", "DAP-Link"),
    ("VID_0D28&PID_0204", "cmsis-dap", "DAPLink"),
    ("VID_0483&PID_3744", "stlink",    "ST-Link V1"),
    ("VID_0483&PID_3748", "stlink",    "ST-Link V2"),
    ("VID_0483&PID_374B", "stlink",    "ST-Link V2-1"),
    ("VID_0483&PID_374D", "stlink",    "ST-Link V3E"),
    ("VID_0483&PID_374E", "stlink",    "ST-Link V3"),
    ("VID_0483&PID_374F", "stlink",    "ST-Link V3"),
    ("VID_1366&PID_0101", "jlink",     "J-Link"),
    ("VID_1366&PID_0105", "jlink",     "J-Link"),
]


def detect_debugger():
    """认当前插着的调试器，返回 (接口脚本名, 显示名)：('stlink', 'ST-Link V2')。

    只认「此刻真的连着」的设备。不能用注册表 Enum\\USB 判断 —— 拔掉的
    设备键还留在里面，换调试器时会认成旧的那个，然后复位必然失败。
    CFGMGR32 的 PRESENT 过滤才是准的。
    """
    if os.name != "nt":
        return None, None
    try:
        import ctypes
        from ctypes import wintypes

        cm = ctypes.windll.cfgmgr32
        CM_GETIDLIST_FILTER_PRESENT = 0x00000100

        size = wintypes.ULONG(0)
        if cm.CM_Get_Device_ID_List_SizeW(
                ctypes.byref(size), None,
                CM_GETIDLIST_FILTER_PRESENT) != 0:
            return None, None
        buf = ctypes.create_unicode_buffer(size.value)
        if cm.CM_Get_Device_ID_ListW(
                None, buf, size.value,
                CM_GETIDLIST_FILTER_PRESENT) != 0:
            return None, None
        ids = bytes(buf).decode("utf-16-le", "ignore").upper()
    except Exception:
        return None, None

    for vidpid, iface, label in DEBUGGERS:
        if vidpid in ids:
            return iface, label
    return None, None


def find_openocd():
    """返回 (openocd.exe 绝对路径, scripts 目录)；找不到返回 (None, None)"""
    pats = [
        os.path.expanduser("~/.eide/tools/openocd*/bin/openocd.exe"),
        os.path.expanduser("~/.eide/tools/openocd*/bin/openocd"),
        "C:/Program Files/OpenOCD*/bin/openocd.exe",
        "C:/Program Files (x86)/OpenOCD*/bin/openocd.exe",
        "/usr/bin/openocd",
    ]
    for pat in pats:
        for exe in sorted(glob.glob(pat), reverse=True):
            if not os.path.isfile(exe):
                continue
            root = os.path.dirname(os.path.dirname(exe))
            for sub in ("share/openocd/scripts", "share/openocd/scripts/"):
                scr = os.path.join(root, *sub.split("/"))
                if os.path.isdir(os.path.join(scr, "target")):
                    return exe, scr
            # Linux 包管理装的位置
            if os.path.isdir("/usr/share/openocd/scripts/target"):
                return exe, "/usr/share/openocd/scripts"
    return None, None


def openocd_reset(exe, scr, interface="cmsis-dap", target="stm32f1x",
                  timeout=25):
    """让调试器复位目标板并放开运行。返回 (成功?, 说明文本)"""
    icfg = os.path.join(scr, "interface", interface + ".cfg")
    tcfg = os.path.join(scr, "target", target + ".cfg")
    if not os.path.isfile(icfg):
        return False, "找不到接口配置：{}".format(icfg)
    if not os.path.isfile(tcfg):
        return False, "找不到芯片配置：{}".format(tcfg)

    cmd = [exe, "-f", icfg, "-f", tcfg,
           "-c", "init; reset run; shutdown"]
    # Windows 下别弹出黑色控制台窗口
    flags = 0x08000000 if os.name == "nt" else 0
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, creationflags=flags)
    except subprocess.TimeoutExpired:
        return False, "OpenOCD 超时（{}s），调试器可能没插或正被占用".format(timeout)
    except FileNotFoundError:
        return False, "OpenOCD 可执行文件不存在"

    out = (r.stdout or "") + (r.stderr or "")
    if r.returncode != 0:
        brief = [l for l in out.splitlines()
                 if "Error" in l or "error" in l][-3:]
        return False, "OpenOCD 失败（exit {}）：{}".format(
            r.returncode, " | ".join(brief) or out.splitlines()[-1:] or "")
    if "SWD" not in out and "JTAG" not in out and "DPIDR" not in out:
        return False, "调试器未响应：{}".format(
            (out.splitlines() or ["无输出"])[-1])
    return True, "调试器复位完成"


# ==============================================================================
# 协议打包（GUI 与无界面调试端 ota_headless.py 共用）
# ==============================================================================
# 抽成模块级函数，是因为这两个结构的字节定义必须和 C 端逐字节一致：
#   OtaSlotHeader_t（32 字节，见 ota_slot.h）
#   老协议尾部（12 字节，'OTAE' + len + crc32）
# 一旦 GUI 里写一份、调试脚本里再写一份，两边漂移的症状是"发送说成功、
# 板子报 crc mismatch"，而且不报任何错 —— 这类坑排查成本极高。

OTA_HDR_MAGIC = 0x50555746      # 'F','W','U','P' 小端形式 = OTA_SLOT_HDR_MAGIC
OTA_HDR_VER = 1                 # 必须等于 C 端 OTA_SLOT_HDR_VER
OTA_TAIL_MAGIC = b"OTAE"


def build_fwup_header(fw_ver, fw_len, fw_crc, build_time=0, flags=0):
    """构造 32 字节槽头。

    布局（必须和 OtaSlotHeader_t 一致）：
      magic(I) fw_ver(H) hdr_ver(H) fw_len(I) fw_crc(I) build_time(I) flags(I)
      = 24 字节；hdr_crc(I) 覆盖前 24 字节；reserved(I) 必须为 0

    注意 CRC 覆盖的是 24 而不是 28 —— 28 是 hdr_crc 之后再算 reserved 的偏移，
    很容易记混。C 端用 offsetof(hdr_crc) 算，这里必须跟着对上。
    """
    body = struct.pack("<IHHIIII", OTA_HDR_MAGIC, fw_ver, OTA_HDR_VER,
                       fw_len, fw_crc, build_time, flags)
    return body + struct.pack("<II", zlib.crc32(body) & 0xFFFFFFFF, 0)


def build_ota_tail(fw_len, fw_crc):
    """老协议（BootLoader 串口收固件）的 12 字节尾部。"""
    return struct.pack("<4sII", OTA_TAIL_MAGIC, fw_len, fw_crc)


# ==============================================================================
# 主窗口
# ==============================================================================

class OtaSerialApp(object):

    # ------------------------------------------------------------------ 初始化
    def __init__(self, root):
        self.root = root
        self.cfg = load_config()

        root.title("{}  v{}".format(APP_TITLE, APP_VER))
        root.geometry(self.cfg.get("geometry") or "1280x900")
        root.minsize(1040, 840)
        root.configure(bg=C_BG)

        # ---- 串口对象与线程状态 ----
        self.ser = None
        self.reader_alive = False
        self.reader_thread = None
        self.rx_queue = queue.Queue()        # (kind, payload) 主线程消费

        # 注：这里曾经有个 rx_epoch 分代号，用来在"清空缓冲"时丢弃
        # reader 手上正在读的那一块。后来确认需求只是清屏 —— 清屏不该
        # 影响接收，丢掉的是板子刚发来的数据 —— 所以连同代号一起撤了。

        # ---- 发送线程 ----
        self.tx_thread = None
        self.abort_evt = threading.Event()

        # ---- 解码器（增量式，解决多字节字符被切开变乱码）----
        self.dec_utf8 = codecs.getincrementaldecoder("utf-8")("replace")
        self.dec_gbk = codecs.getincrementaldecoder("gbk")("replace")

        # ---- HEX 显示缓冲 ----
        self.hex_buf = b""
        self.hex_off = 0
        self.hex_last_rx = 0.0

        # ---- 解析行缓冲 ----
        self.line_buf = b""

        # ---- OTA 状态 ----
        self.ota_base = ""          # App 基址，如 0x08008000
        self.ota_page = ""          # 页大小，如 2048
        self.ota_saved = 0          # BootLoader 已确认落盘的字节数（sum）
        self.ota_proto = 1          # 协议版本：1 = 老版（无校验），2 = 支持尾部 + CRC
        self.ota_verify = None      # None 未出结果 / ("ok", 长度) / ("fail", 0)
        self.ota_declared = 0       # 尾部记录里声明的固件长度
        self.ota_crc = ""           # 尾部记录里带的 CRC（字符串，仅显示用）
        self.fw_size = 0            # 当前选中的固件文件大小
        self.fw_verified = None     # None=未判定 True=一致 False=不一致
        self.ota_done_ts = 0.0      # 最近一次升级完成的时刻（0 = 还没跑过）

        # ---- 调试器（DAP-Link 经 OpenOCD 复位）----
        self.ocd_exe, self.ocd_scr = find_openocd()

        # 调试器类型：配置里显式指定 > 现场探测 > 默认按 DAP-Link 猜。
        # 换了 ST-Link / J-Link 不用改配置，探测会自己认出来。
        _probed_iface, _probed_name = detect_debugger()
        self.ocd_iface = (self.cfg.get("ocd_iface") or _probed_iface
                          or "cmsis-dap")
        self.ocd_target = self.cfg.get("ocd_target", "stm32f1x")
        self.ocd_name = _probed_name or {
            "cmsis-dap": "DAP-Link",
            "stlink":    "ST-Link",
            "jlink":     "J-Link",
        }.get(self.ocd_iface, self.ocd_iface.upper())

        # 接口脚本必须真的存在，否则 OpenOCD 一启动就报错，不如提前置灰
        self.ocd_iface_ok = bool(
            self.ocd_scr
            and os.path.isfile(os.path.join(self.ocd_scr, "interface",
                                            self.ocd_iface + ".cfg")))
        self.ocd_ready = bool(self.ocd_exe) and self.ocd_iface_ok

        # ---- 收发计数 ----
        self.rx_count = 0
        self.tx_count = 0

        # ---- 接收区最大保留行数 ----
        self.rx_max_lines = 5000

        self._init_style()
        self._build_ui()
        self.refresh_ports()

        # Ctrl+L 清空接收区，和终端里清屏的习惯一致。
        # 绑在 root 上而不是 Text 上：焦点在输入框时也想能按。
        # 末尾返回 "break" 阻止 Tk 继续往上冒泡（否则某些主题会吃掉按键）。
        root.bind("<Control-l>",
                  lambda e: (self.clear_rx(), "break")[1])

        # 关闭窗口时先收拾线程，否则 python 会卡在 join
        root.protocol("WM_DELETE_WINDOW", self.on_close)

        self._poll()

    # ---------------------------------------------------------------- ttk 主题
    def _init_style(self):
        st = ttk.Style()
        try:
            st.theme_use("clam")            # clam 才允许改背景色
        except tk.TclError:
            pass

        st.configure("TFrame", background=C_BG)
        st.configure("Card.TFrame", background=C_PANEL,
                     relief="solid", borderwidth=1)
        st.configure("TLabel", background=C_BG, foreground=C_TEXT,
                     font=(FONT_UI, 9))
        st.configure("Card.TLabel", background=C_PANEL, foreground=C_TEXT,
                     font=(FONT_UI, 9))
        st.configure("Hint.TLabel", background=C_PANEL, foreground=C_SUB,
                     font=(FONT_UI, 8))
        st.configure("Val.TLabel", background=C_PANEL, foreground=C_TEXT,
                     font=(FONT_MONO, 10))
        st.configure("Big.TLabel", background=C_PANEL, foreground=C_TEXT,
                     font=(FONT_MONO, 13, "bold"))
        st.configure("TLabelframe", background=C_BG, bordercolor=C_BORDER,
                     relief="solid", borderwidth=1)
        st.configure("TLabelframe.Label", background=C_BG, foreground=C_SUB,
                     font=(FONT_UI, 9, "bold"))
        st.configure("TCheckbutton", background=C_BG, foreground=C_TEXT,
                     font=(FONT_UI, 9))
        st.configure("Card.TCheckbutton", background=C_PANEL, foreground=C_TEXT,
                     font=(FONT_UI, 9))
        st.configure("TButton", font=(FONT_UI, 9), padding=(9, 4))
        st.configure("Accent.TButton", background=C_ACCENT, foreground="#FFFFFF",
                     font=(FONT_UI, 9, "bold"), padding=(12, 5), borderwidth=0)
        st.map("Accent.TButton",
               background=[("active", C_ACCENT2), ("disabled", "#A9C0F2")])
        st.configure("Go.TButton", background=C_OK, foreground="#FFFFFF",
                     font=(FONT_UI, 9, "bold"), padding=(10, 6), borderwidth=0)
        st.map("Go.TButton",
               background=[("active", "#0B8049"), ("disabled", "#A8D5BE")])
        st.configure("Stop.TButton", background="#F1F3F6", foreground=C_BAD,
                     font=(FONT_UI, 9, "bold"), padding=(10, 6), borderwidth=1)
        st.map("Stop.TButton",
               background=[("active", "#FBE4E2"), ("disabled", "#F0F0F0")])
        st.configure("Ota.Horizontal.TProgressbar",
                     troughcolor="#E7EBF1", background=C_OK,
                     thickness=16, borderwidth=0)

    # ------------------------------------------------------------------- 布局
    def _build_ui(self):
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(1, weight=1)

        self._build_topbar()      # row 0
        self._build_middle()      # row 1  接收区 + 右侧面板
        self._build_sendbar()     # row 2
        self._build_statusbar()   # row 3

    # ---- 顶部：串口配置 ----
    def _build_topbar(self):
        bar = ttk.Frame(self.root)
        bar.grid(row=0, column=0, sticky="ew", padx=12, pady=(10, 6))

        ttk.Label(bar, text="端口").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_cb = ttk.Combobox(bar, textvariable=self.port_var,
                                    width=26, state="readonly",
                                    font=(FONT_MONO, 9))
        self.port_cb.pack(side="left", padx=(6, 4))

        ttk.Button(bar, text="刷新", width=6,
                   command=self.refresh_ports).pack(side="left", padx=(0, 14))

        ttk.Label(bar, text="波特率").pack(side="left")
        self.baud_var = tk.StringVar(value=str(self.cfg.get("baud", "115200")))
        ttk.Combobox(bar, textvariable=self.baud_var, width=9,
                     values=BAUD_LIST, state="readonly",
                     font=(FONT_MONO, 9)).pack(side="left", padx=(6, 14))

        self.btn_open = ttk.Button(bar, text="打开串口", width=11,
                                   style="Accent.TButton",
                                   command=self.toggle_port)
        self.btn_open.pack(side="left")

        # ---- 右侧显示选项（pack side=right 是从右往左排，注意顺序反着写）----
        self.disp_mode = tk.StringVar(value=self.cfg.get("disp_mode", "UTF-8"))
        cb_mode = ttk.Combobox(bar, textvariable=self.disp_mode, width=7,
                               values=["UTF-8", "GBK", "HEX"], state="readonly",
                               font=(FONT_MONO, 9))
        cb_mode.pack(side="right")
        cb_mode.bind("<<ComboboxSelected>>", lambda e: self.on_disp_mode_change())
        ttk.Label(bar, text="显示").pack(side="right", padx=(0, 6))

        self.autoscroll = tk.BooleanVar(value=self.cfg.get("autoscroll", True))
        self._chk(bar, "自动滚动", self.autoscroll, bg=C_BG).pack(
            side="right", padx=(0, 14))

        self.timestamp = tk.BooleanVar(value=self.cfg.get("timestamp", False))
        self._chk(bar, "时间戳", self.timestamp, bg=C_BG).pack(
            side="right", padx=(0, 14))

    # ---- 中部：接收区 + 右侧 OTA 面板 ----
    def _build_middle(self):
        mid = ttk.Frame(self.root)
        mid.grid(row=1, column=0, sticky="nsew", padx=12, pady=(0, 6))
        mid.columnconfigure(0, weight=1)
        mid.rowconfigure(0, weight=1)

        # ------- 左：接收区 -------
        left = ttk.Frame(mid)
        left.grid(row=0, column=0, sticky="nsew")
        left.columnconfigure(0, weight=1)
        left.rowconfigure(0, weight=1)

        self.rx = tk.Text(left, wrap="char", font=(FONT_MONO, 10),
                          bg="#FBFCFE", fg=C_TEXT, insertbackground=C_TEXT,
                          relief="solid", borderwidth=1,
                          padx=8, pady=6, spacing1=1, spacing3=1,
                          undo=False, state="disabled")
        self.rx.grid(row=0, column=0, sticky="nsew")

        sb = ttk.Scrollbar(left, orient="vertical", command=self.rx.yview)
        sb.grid(row=0, column=1, sticky="ns")
        self.rx.configure(yscrollcommand=sb.set)

        # 着色规则
        self.rx.tag_configure("boot", foreground=C_BOOT)
        self.rx.tag_configure("ota", foreground=C_OTA)
        self.rx.tag_configure("rst", foreground=C_WARN)
        self.rx.tag_configure("good", foreground=C_OK)
        self.rx.tag_configure("bad", background="#FDECEA", foreground=C_BAD)
        self.rx.tag_configure("sys", foreground=C_SUB)

        # ------- 右：OTA 面板 -------
        # 不写死宽度，让右栏按内容自身需求占位。
        # 之前锁死 344px + grid_propagate(False)，一旦内容最小宽度超过它，
        # 控件就会被画到边界外、被窗口裁掉，而且极难察觉。
        right = ttk.Frame(mid)
        right.grid(row=0, column=1, sticky="ns", padx=(10, 0))
        self._build_ota_card(right)
        self._build_fw_card(right)

    def _chk(self, parent, text, var, bg=None):
        """普通 Tk 复选框。

        不用 ttk 的 Card.TCheckbutton：clam 主题下选中标记会渲染成一个 ✕，
        看着像「关闭」，容易让人误判当前状态。
        """
        bg = bg or C_PANEL
        return tk.Checkbutton(
            parent, text=text, variable=var,
            bg=bg, fg=C_TEXT, activebackground=bg,
            activeforeground=C_TEXT, selectcolor="#FFFFFF",
            font=(FONT_UI, 9), bd=0, highlightthickness=0,
            anchor="w", cursor="hand2")

    def _card(self, parent, title, row):
        """带标题的卡片外壳，返回内部 content frame"""
        outer = tk.Frame(parent, bg=C_BG)
        outer.grid(row=row, column=0, sticky="ew", pady=(0, 6))
        outer.columnconfigure(0, weight=1)

        head = tk.Frame(outer, bg=C_BG)
        head.grid(row=0, column=0, sticky="ew", pady=(0, 3))
        tk.Label(head, text=title, bg=C_BG, fg=C_SUB,
                 font=(FONT_UI, 9, "bold")).pack(side="left")

        box = tk.Frame(outer, bg=C_PANEL, highlightbackground=C_BORDER,
                       highlightthickness=1)
        box.grid(row=1, column=0, sticky="ew")
        box.columnconfigure(1, weight=1)

        content = tk.Frame(box, bg=C_PANEL)
        content.grid(row=0, column=0, sticky="nsew", padx=10, pady=8)
        content.columnconfigure(1, weight=1)
        return content

    def _build_ota_card(self, parent):
        c = self._card(parent, "OTA 状态", 0)

        # 状态灯 + 文字
        line0 = tk.Frame(c, bg=C_PANEL)
        line0.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 6))
        self.lamp = tk.Canvas(line0, width=10, height=10, bg=C_PANEL,
                              highlightthickness=0)
        self.lamp.pack(side="left", padx=(0, 6))
        self.lamp_id = self.lamp.create_oval(1, 1, 9, 9,
                                             fill="#C9CFD8", outline="")
        self.var_state = tk.StringVar(value="未连接")
        tk.Label(line0, textvariable=self.var_state, bg=C_PANEL, fg=C_SUB,
                 font=(FONT_UI, 9, "bold")).pack(side="left")

        def kv_row(r, key, var, mono=True):
            tk.Label(c, text=key, bg=C_PANEL, fg=C_SUB,
                     font=(FONT_UI, 8)).grid(row=r, column=0, sticky="w")
            tk.Label(c, textvariable=var, bg=C_PANEL, fg=C_TEXT,
                     font=(FONT_MONO, 9) if mono else (FONT_UI, 9),
                     anchor="e").grid(row=r, column=1, sticky="e")

        self.var_app = tk.StringVar(value="--")
        self.var_page = tk.StringVar(value="--")
        self.var_saved = tk.StringVar(value="0 B")
        self.var_target = tk.StringVar(value="未选择固件")
        kv_row(1, "App 基址", self.var_app)
        kv_row(2, "页大小", self.var_page)
        kv_row(3, "已落盘", self.var_saved)
        kv_row(4, "目标大小", self.var_target, mono=False)

        self.pb = ttk.Progressbar(c, style="Ota.Horizontal.TProgressbar",
                                  maximum=100, value=0)
        self.pb.grid(row=5, column=0, columnspan=2, sticky="ew", pady=(6, 3))

        self.var_verify = tk.StringVar(value="")
        self.lb_verify = tk.Label(c, textvariable=self.var_verify, bg=C_PANEL,
                                  fg=C_SUB, font=(FONT_UI, 9, "bold"),
                                  anchor="w")
        self.lb_verify.grid(row=6, column=0, columnspan=2, sticky="ew")

    def _build_fw_card(self, parent):
        c = self._card(parent, "固件发送", 1)

        self.fw_path = tk.StringVar(value="")
        self.fw_name = tk.StringVar(value="（未选择文件）")

        ttk.Button(c, text="选择 .bin 文件", command=self.pick_file).grid(
            row=0, column=0, columnspan=2, sticky="ew", pady=(0, 5))

        tk.Label(c, textvariable=self.fw_name, bg=C_PANEL, fg=C_SUB,
                 font=(FONT_UI, 8), wraplength=288, justify="left",
                 anchor="w").grid(row=1, column=0, columnspan=2,
                                  sticky="ew", pady=(0, 7))

        # 块大小
        tk.Label(c, text="块大小", bg=C_PANEL, fg=C_SUB,
                 font=(FONT_UI, 8)).grid(row=2, column=0, sticky="w", pady=1)
        self.chunk_var = tk.StringVar(value=str(self.cfg.get("chunk", "256")))
        ttk.Combobox(c, textvariable=self.chunk_var, width=7, state="readonly",
                     values=["64", "128", "256", "512", "1024", "2048"],
                     font=(FONT_MONO, 9)).grid(row=2, column=1, sticky="e", pady=1)

        # 块延时
        tk.Label(c, text="块延时 (ms)", bg=C_PANEL, fg=C_SUB,
                 font=(FONT_UI, 8)).grid(row=3, column=0, sticky="w", pady=1)
        self.delay_var = tk.StringVar(value=str(self.cfg.get("delay", "1")))
        ttk.Combobox(c, textvariable=self.delay_var, width=7, state="readonly",
                     values=["0", "1", "2", "5", "10", "20"],
                     font=(FONT_MONO, 9)).grid(row=3, column=1, sticky="e", pady=1)

        self.auto_reset = tk.BooleanVar(
            value=self.cfg.get("auto_reset", self.ocd_ready))
        self.chk_reset = self._chk(c, "发送前调试器复位（抢 OTA 窗口）",
                                   self.auto_reset)
        self.chk_reset.grid(row=4, column=0, columnspan=2, sticky="w",
                            pady=(6, 2))

        # 新架构开关：固件不再由 BootLoader 收，而是直接发给正在运行的 App。
        # App 自己写 W25Q 槽 A、收完写头、然后软复位回 BootLoader 搬运。
        # 默认关：老工具配老流程仍然可用，切换是显式动作，不做隐式猜测。
        # 放在"发送前复位"紧下方，因为这两项是互斥的两条发送路径 ——
        # 勾了新架构就不需要抢 OTA 窗口，所以联动置灰，避免误以为还要复位。
        self.fwup_mode = tk.BooleanVar(value=self.cfg.get("fwup_mode", False))
        self.chk_fwup = self._chk(c, "新架构：直接发给 App（FWUP 头，免复位）",
                                  self.fwup_mode)
        self.chk_fwup.grid(row=5, column=0, columnspan=2, sticky="w",
                           pady=(0, 2))

        # 固件版本号：写进 FWUP 头 → App 原样存进槽头 → Boot 搬进参数页。
        # Boot 拿它和"板子当前版本"比，低于就拒绝搬运 —— 这是拦误刷旧版的唯一手段
        # （CRC 不行：同一份源码重编一次，__TIME__ 变了 CRC 就变，分不出新旧）。
        # 只在新架构下有意义：老协议没有头部，Boot 那边也读不到版本。
        vf = tk.Frame(c, bg=C_PANEL)
        vf.grid(row=6, column=0, columnspan=2, sticky="ew", pady=(0, 5))
        vf.columnconfigure(0, weight=1)
        tk.Label(vf, text="固件版本（升序，降级会被拒绝）", bg=C_PANEL, fg=C_SUB,
                 font=(FONT_UI, 8)).grid(row=0, column=0, sticky="w")
        self.fw_ver_var = tk.StringVar(value=str(self.cfg.get("fw_ver", "1")))
        ttk.Spinbox(vf, textvariable=self.fw_ver_var, from_=1, to=65535,
                    width=7, font=(FONT_MONO, 9)).grid(row=0, column=1, sticky="e")

        # 发送 / 中止
        bf = tk.Frame(c, bg=C_PANEL)
        bf.grid(row=7, column=0, columnspan=2, sticky="ew")
        bf.columnconfigure(0, weight=2)
        bf.columnconfigure(1, weight=1)

        self.btn_go = ttk.Button(bf, text="▶ 开始发送", style="Go.TButton",
                                 command=self.start_file_send)
        self.btn_go.grid(row=0, column=0, sticky="ew", padx=(0, 4))

        self.btn_stop = ttk.Button(bf, text="■ 中止", style="Stop.TButton",
                                   command=self.abort_file_send,
                                   state="disabled")
        self.btn_stop.grid(row=0, column=1, sticky="ew")

        # 进度细节：已发 / 速度
        self.var_tx = tk.StringVar(value="")
        tk.Label(c, textvariable=self.var_tx, bg=C_PANEL, fg=C_SUB,
                 font=(FONT_MONO, 8), anchor="w").grid(
            row=8, column=0, columnspan=2, sticky="ew", pady=(5, 0))

        # 复位：DAP-Link 的串口 DTR 不接 NRST，必须走 SWD。
        # width 单位是字符，必须限死，否则 ttk 按钮按文字撑出 147px 最小宽度、
        # 把卡片顶宽，右对齐的控件会被画到窗口外且 winfo 量不出来。
        rf = tk.Frame(c, bg=C_PANEL)
        rf.grid(row=9, column=0, columnspan=2, sticky="ew", pady=(6, 0))
        rf.columnconfigure(0, weight=2)
        rf.columnconfigure(1, weight=1)

        self.btn_rst = ttk.Button(rf, text="SWD 复位", width=9,
                                  command=self.reset_via_debugger)
        self.btn_rst.grid(row=0, column=0, sticky="ew", padx=(0, 3))
        if not self.ocd_ready:
            self.btn_rst.configure(state="disabled")

        ttk.Button(rf, text="唤醒", width=5, command=self.wake_ota).grid(
            row=0, column=1, sticky="ew")

        # 调试器状态一行说明，找不到时告诉用户原因
        self.ocd_hint = tk.StringVar()
        if not self.ocd_exe:
            self.ocd_hint.set("未找到 OpenOCD，请手动按板子复位")
        elif not self.ocd_iface_ok:
            self.ocd_hint.set("OpenOCD 缺 interface/{}.cfg，请手动复位".format(
                self.ocd_iface))
        else:
            self.ocd_hint.set("SWD 复位就绪 · " + self.ocd_name)
        tk.Label(c, textvariable=self.ocd_hint, bg=C_PANEL,
                 fg=C_OK if self.ocd_ready else C_BAD,
                 font=(FONT_UI, 7), anchor="w").grid(
            row=10, column=0, columnspan=2, sticky="ew", pady=(3, 0))

        # 两条路径互斥：新架构下"发送前复位"没意义（App 本来就活着），
        # 置灰比留着一个勾更容易让人看懂当前走哪条。取消勾选后恢复可用。
        self.fwup_mode.trace_add("write", lambda *_: self._sync_send_mode())
        self._sync_send_mode()

    def _sync_send_mode(self):
        """勾了「新架构」就把「发送前复位」置灰，反之恢复。

        只是让界面说真话：新架构的发送函数根本不读 auto_reset，
        留着可勾选框会让人以为复位还在起作用。
        """
        if self.fwup_mode.get():
            self.chk_reset.configure(state="disabled")
        else:
            self.chk_reset.configure(state="normal")

    # ---- 底部：发送区 ----
    def _build_sendbar(self):
        wrap = ttk.Frame(self.root)
        wrap.grid(row=2, column=0, sticky="ew", padx=12, pady=(0, 6))
        wrap.columnconfigure(0, weight=1)

        box = tk.Frame(wrap, bg=C_PANEL, highlightbackground=C_BORDER,
                       highlightthickness=1)
        box.grid(row=0, column=0, sticky="ew")
        box.columnconfigure(0, weight=1)

        self.tx = tk.Text(box, height=3, wrap="char", font=(FONT_MONO, 10),
                          bg=C_PANEL, fg=C_TEXT, insertbackground=C_TEXT,
                          relief="flat", borderwidth=0,
                          padx=8, pady=6, undo=True)
        self.tx.grid(row=0, column=0, sticky="ew", padx=(0, 0), pady=0)
        self.tx.bind("<Control-Return>", lambda e: (self.send_text(), "break")[1])

        side = tk.Frame(box, bg=C_PANEL)
        side.grid(row=0, column=1, sticky="ns", padx=8, pady=6)

        # HEX / \r\n 横排，否则这一列会比文本框还高，把发送框顶高
        opt = tk.Frame(side, bg=C_PANEL)
        opt.pack(anchor="w", pady=(0, 4))

        self.hex_tx = tk.BooleanVar(value=False)
        self._chk(opt, "HEX", self.hex_tx).pack(side="left")

        self.crlf = tk.BooleanVar(value=True)
        self._chk(opt, "\\r\\n", self.crlf).pack(side="left", padx=(10, 0))

        ttk.Button(side, text="发送", style="Accent.TButton",
                   command=self.send_text).pack(fill="x")

        tk.Label(wrap, text="Ctrl+Enter 快捷发送", bg=C_BG, fg=C_SUB,
                 font=(FONT_UI, 8)).grid(row=1, column=0, sticky="w", pady=(2, 0))

    # ---- 最底：状态栏 ----
    def _build_statusbar(self):
        bar = tk.Frame(self.root, bg="#E9EDF2", height=24)
        bar.grid(row=3, column=0, sticky="ew")
        bar.grid_propagate(False)

        self.var_conn = tk.StringVar(value="未连接")
        tk.Label(bar, textvariable=self.var_conn, bg="#E9EDF2", fg=C_SUB,
                 font=(FONT_UI, 8)).pack(side="left", padx=12)

        self.var_count = tk.StringVar(value="RX 0 B   |   TX 0 B")
        tk.Label(bar, textvariable=self.var_count, bg="#E9EDF2", fg=C_SUB,
                 font=(FONT_MONO, 8)).pack(side="right", padx=12)

        # 文案回到"清空接收"：它清的是接收区这一屏字，
        # 串口缓冲、待渲染队列、解析状态全都原样保留，名字得说清楚
        tk.Button(bar, text="清空接收", relief="flat", bg="#E9EDF2", fg=C_SUB,
                  font=(FONT_UI, 8), bd=0, activebackground="#DCE2E9",
                  cursor="hand2", command=self.clear_rx).pack(side="right", padx=4)

        tk.Button(bar, text="保存日志", relief="flat", bg="#E9EDF2", fg=C_SUB,
                  font=(FONT_UI, 8), bd=0, activebackground="#DCE2E9",
                  cursor="hand2", command=self.save_log).pack(side="right", padx=4)

    # ==========================================================================
    # 串口开关
    # ==========================================================================

    def refresh_ports(self):
        ports = list_serial_ports()
        values = [d for _, d, _ in ports]
        self.port_cb["values"] = values
        if not values:
            self.port_var.set("")
            return

        cur = self.port_var.get()
        if cur in values:
            return
        # 上次用的那个口还插着，就接着用它
        saved = self.cfg.get("port")
        if saved in values:
            self.port_var.set(saved)
            return
        # 列表已经按"真实 USB 串口优先、蓝牙虚拟口靠后"排好序，取第一个即可
        self.port_var.set(values[0])

    def toggle_port(self):
        if self.ser and self.ser.is_open:
            self.close_port()
        else:
            self.open_port()

    def open_port(self):
        sel = self.port_var.get().strip()
        if not sel:
            messagebox.showwarning("没有可用串口", "请先插好 USB 转串口，再点刷新。")
            return
        dev = sel.split()[0]

        try:
            baud = int(self.baud_var.get())
        except ValueError:
            messagebox.showwarning("波特率无效", "请选择或输入一个数字波特率。")
            return

        # 先构造但不 open，把 DTR/RTS 置 False 再 open
        # 否则 open 瞬间会拉一次 DTR —— 很多板子 DTR 接 NRST，会白复位一次
        ser = serial.Serial()
        ser.port = dev
        ser.baudrate = baud
        ser.bytesize = serial.EIGHTBITS
        ser.parity = serial.PARITY_NONE
        ser.stopbits = serial.STOPBITS_ONE
        ser.timeout = 0.05
        ser.write_timeout = None        # 大块写时阻塞等待，不丢数据
        ser.dtr = False
        ser.rts = False

        try:
            ser.open()
        except serial.SerialException as e:
            msg = str(e)
            if "PermissionError" in msg or "拒绝访问" in msg or "Access is denied" in msg:
                messagebox.showerror(
                    "串口被占用",
                    "{} 已被其他程序占用。\n\n"
                    "常见原因：SSCOM / XCOM / VSCode 串口监视器 还开着。\n"
                    "请先关掉它们再试。".format(dev))
            else:
                messagebox.showerror("打开失败", msg)
            return

        self.ser = ser
        self.reader_alive = True
        self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader_thread.start()

        # 复位解码器与 HEX 偏移
        self.dec_utf8 = codecs.getincrementaldecoder("utf-8")("replace")
        self.dec_gbk = codecs.getincrementaldecoder("gbk")("replace")
        self.hex_buf = b""
        self.hex_off = 0
        self.line_buf = b""

        self.btn_open.configure(text="关闭串口")
        self.var_conn.set("已连接  {}  {}".format(dev, baud))
        self._set_state("已连接", C_ACCENT)
        self._sys_line("--- 已连接 {} @ {} ---".format(dev, baud))

    def close_port(self):
        # 先让发送线程停下
        self.abort_evt.set()
        self.reader_alive = False

        if self.reader_thread and self.reader_thread.is_alive():
            self.reader_thread.join(timeout=0.6)

        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None

        self.btn_open.configure(text="打开串口")
        self.var_conn.set("未连接")
        self._set_state("未连接", "#C9CFD8")
        self._sys_line("--- 已断开 ---")

    def _reader_loop(self):
        """后台读串口，只做一件事：把字节丢进队列。UI 一律主线程改。"""
        while self.reader_alive:
            ser = self.ser
            if ser is None:
                break
            try:
                n = ser.in_waiting
                data = ser.read(n if n > 0 else 1)
            except Exception as e:
                self.rx_queue.put(("error", str(e)))
                break
            if data:
                self.rx_queue.put(("data", data))

    # ==========================================================================
    # 主线程轮询：把队列里的东西刷到界面
    # ==========================================================================

    def _poll(self):
        try:
            while True:
                kind, payload = self.rx_queue.get_nowait()
                if kind == "data":
                    self._on_rx(payload)
                elif kind == "error":
                    self._on_serial_error(payload)
                elif kind == "tx":
                    self._on_tx_progress(*payload)
        except queue.Empty:
            pass

        # HEX 模式下，不足 16 字节的尾巴要定时冲出来，否则要等到下一批数据
        if (self.disp_mode.get() == "HEX" and self.hex_buf
                and time.time() - self.hex_last_rx > 0.15):
            self._flush_hex()

        self.root.after(30, self._poll)

    def _on_serial_error(self, msg):
        self._sys_line("--- 串口异常: {} ---".format(msg))
        self.close_port()

    def _on_rx(self, data):
        self.rx_count += len(data)
        self._update_counts()
        self._feed_parser(data)         # 解析永远基于原始字节，与显示模式无关
        self._render(data)

    def _update_counts(self):
        self.var_count.set("RX {}   |   TX {}".format(
            human_bytes(self.rx_count), human_bytes(self.tx_count)))

    # ==========================================================================
    # 渲染
    # ==========================================================================

    def _render(self, data):
        mode = self.disp_mode.get()

        if mode == "HEX":
            self.hex_last_rx = time.time()
            self.hex_buf += data
            while len(self.hex_buf) >= 16:
                chunk, self.hex_buf = self.hex_buf[:16], self.hex_buf[16:]
                self._insert("{:08X}  {}\n".format(
                    self.hex_off, " ".join("{:02X}".format(b) for b in chunk)), "sys")
                self.hex_off += 16
            return

        dec = self.dec_utf8 if mode == "UTF-8" else self.dec_gbk
        text = dec.decode(data)
        if not text:
            return

        if self.timestamp.get():
            now = time.time()
            ms = int((now - int(now)) * 1000)
            text = time.strftime("[%H:%M:%S") + ".{:03d}] ".format(ms) + text

        start_line = int(self.rx.index("end-1c").split(".")[0])
        self._insert(text)
        end_line = int(self.rx.index("end-1c").split(".")[0])

        # 逐行着色
        for ln in range(start_line, end_line + 1):
            try:
                line = self.rx.get("{}.0".format(ln), "{}.end".format(ln))
            except tk.TclError:
                continue
            tag = self._classify(line)
            if tag:
                self.rx.tag_add(tag, "{}.0".format(ln), "{}.end".format(ln))

    def _flush_hex(self):
        if not self.hex_buf:
            return
        chunk = self.hex_buf
        self.hex_buf = b""
        self._insert("{:08X}  {}\n".format(
            self.hex_off, " ".join("{:02X}".format(b) for b in chunk)), "sys")
        self.hex_off += len(chunk)

    def _insert(self, s, tag=None):
        at_end = self.rx.yview()[1] > 0.999
        self.rx.configure(state="normal")
        self.rx.insert("end", s, tag or ())
        # 超量裁剪，保证长时间开着也不吃内存
        total = int(self.rx.index("end-1c").split(".")[0])
        if total > self.rx_max_lines:
            self.rx.delete("1.0", "{}.0".format(total - self.rx_max_lines + 500))
        self.rx.configure(state="disabled")
        if self.autoscroll.get() or at_end:
            self.rx.see("end")

    def _sys_line(self, s):
        self._insert(s + "\n", "sys")

    @staticmethod
    def _classify(line):
        s = line.strip()
        if not s:
            return None
        if "FAIL" in s or "ERROR" in s or "error" in s:
            return "bad"
        if s.startswith("[boot]"):
            return "boot"
        if s.startswith("[ota]"):
            return "ota"
        if s.startswith("[rst]"):
            return "rst"
        if " ok" in s or s.endswith("ok") or "OK" in s:
            return "good"
        return None

    def on_disp_mode_change(self):
        self.dec_utf8 = codecs.getincrementaldecoder("utf-8")("replace")
        self.dec_gbk = codecs.getincrementaldecoder("gbk")("replace")
        self.hex_buf = b""
        self.hex_off = 0
        self.clear_rx()
        self._sys_line("--- 显示模式: {} ---".format(self.disp_mode.get()))

    def clear_rx(self):
        """清空接收区显示 —— 只动界面，不碰串口一个字节

        做的事只有三件：清 Text 内容、丢掉 HEX 模式下攒着不满 16 字节的
        尾巴、偏移归零。

        明确不清的东西（这是刻意留着的）：
          · 驱动层 in_waiting —— 板子刚发来、驱动已收到但还没读的字节
          · rx_queue —— 已读到、还没渲染的块
          · line_buf —— 跨块的半行，OTA 面板靠它拼整行
          · 增量解码器 —— 可能存着半个多字节字符
          · rx_count / tx_count —— 累计收发统计
        这些都属于"数据"，清屏不该把它们扔掉。所以清完之后只要板子还在
        发，屏幕会接着往下滚 —— 这是对的，清屏不等于停收。

        副作用：HEX 模式下偏移从 00000000 重新起算。不归零的话清屏后
        第一行会接着上一屏的地址，看起来像丢了一段，反而更迷惑。
        """
        self.rx.configure(state="normal")
        self.rx.delete("1.0", "end")
        self.rx.configure(state="disabled")
        self.hex_buf = b""
        self.hex_off = 0

    def save_log(self):
        txt = self.rx.get("1.0", "end-1c")
        if not txt.strip():
            messagebox.showinfo("没有内容", "接收区还是空的。")
            return
        path = filedialog.asksaveasfilename(
            title="保存接收日志",
            defaultextension=".txt",
            filetypes=[("文本文件", "*.txt"), ("所有文件", "*.*")],
            initialfile=time.strftime("serial_%Y%m%d_%H%M%S.txt"))
        if not path:
            return
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(txt)
            messagebox.showinfo("已保存", path)
        except Exception as e:
            messagebox.showerror("保存失败", str(e))

    # ==========================================================================
    # BootLoader 日志解析 → 更新 OTA 面板
    # ==========================================================================

    def _feed_parser(self, data):
        """按行切分后交给 _handle_line。跨数据块的行用 line_buf 拼起来。"""
        self.line_buf += data
        while b"\n" in self.line_buf:
            line, self.line_buf = self.line_buf.split(b"\n", 1)
            self._handle_line(line.strip(b"\r"))
        # 长时间没有换行的异常数据（比如二进制）不要无限累积
        if len(self.line_buf) > 4096:
            self.line_buf = self.line_buf[-1024:]

    def _handle_line(self, line):
        if not line:
            return

        m = RE_READY.search(line)
        if m:
            # 升级完成后会自动复位，重启的 BootLoader 又打一遍 [ota] ready。
            # 这一次不能把统计清零 —— 否则界面上会出现
            # 「已落盘 14,868 B → 0 B」，看着像升级失败。
            just_done = (time.time() - self.ota_done_ts) < 15.0
            self.ota_base = m.group(1).decode()
            self.ota_page = m.group(2).decode()
            # proto 是可选组：老 BootLoader 不打这个字段，就是 1
            self.ota_proto = int(m.group(3)) if m.group(3) else 1
            self.var_app.set(self.ota_base)
            self.var_page.set(self.ota_page + " B")
            if not just_done:
                self.ota_saved = 0
                self.var_saved.set("0 B")
                self._set_state("等待固件", C_ACCENT)
            else:
                self._set_state("升级完成，重启中…", C_OK)
            self._update_progress()
            self._update_verify()
            return

        m = RE_ENTER.search(line)
        if m:
            self._set_state("已进入 OTA，接收中…", C_WARN)
            return

        m = RE_PAGE.search(line)
        if m:
            self.ota_saved = int(m.group(2))
            self.var_saved.set("{:,} B".format(self.ota_saved))
            self._set_state("写入中…", C_WARN)
            self._update_progress()
            self._update_verify()
            return

        m = RE_TAIL.search(line)
        if m:
            self.ota_saved = int(m.group(3))
            self.ota_done_ts = time.time()      # 记时刻，后面重启的 ready 不会清统计
            self.var_saved.set("{:,} B".format(self.ota_saved))
            self.pb.configure(value=100)
            self._update_verify()
            self._set_state("写入完成", C_OK)
            self._flash_lamp(C_OK)
            return

        m = RE_TAILREC.search(line)
        if m:
            self.ota_declared = int(m.group(1))
            self.ota_crc = m.group(2).decode().upper()
            self._set_state("收到尾部记录，校验中…", C_ACCENT)
            return

        m = RE_VOK.search(line)
        if m:
            n = int(m.group(1))
            self.ota_verify = ("ok", n)
            self.ota_saved = n
            self.ota_done_ts = time.time()
            self.var_saved.set("{:,} B".format(n))
            self.pb.configure(value=100)
            self._update_verify()
            self._set_state("CRC 校验通过，跳转中…", C_OK)
            self._flash_lamp(C_OK)
            return

        if RE_VBAD.search(line) or RE_SIZEBAD.search(line):
            # 校验没过就别指望跳转了，板子还停在 OTA 里等重传
            self.ota_verify = ("fail", 0)
            self._update_verify()
            self._set_state("校验失败，未跳转", C_BAD)
            self._flash_lamp(C_BAD)
            return

        if RE_CRCBAD.search(line):
            self._set_state("固件 CRC 不匹配，停在 OTA", C_BAD)
            return

        if RE_LEGACY.search(line):
            # 参数页空的：这份 App 是加校验之前刷的，能用但没验证过
            self._set_state("老固件：未校验", C_WARN)
            return

        m = RE_JUMP.search(line)
        if m:
            self.var_app.set(m.group(1).decode())
            self._set_state("已跳转 App", C_OK)
            return

        m = RE_NOAPP.search(line)
        if m:
            self._set_state("无有效 App，等待固件", C_WARN)
            return

        if RE_OTAERR.search(line):
            self._set_state("写入出错", C_BAD)
            self._flash_lamp(C_BAD)
            return

        m = RE_RST.search(line)
        if m:
            cause = m.group(1).decode()
            if cause in ("IWDG", "WWDG"):
                self._set_state("看门狗复位！", C_BAD)
            return

    def _update_progress(self):
        if self.fw_size > 0:
            pct = min(100.0, self.ota_saved * 100.0 / self.fw_size)
            self.pb.configure(value=pct)
        elif self.ota_saved > 0:
            # 不知道目标大小，就按 100KB 做相对刻度（仅示意）
            self.pb.configure(value=min(100.0, self.ota_saved / 1024.0))

    def _update_verify(self):
        """把 BootLoader 报的 sum 和文件大小对一下账 —— OTA 最容易出问题的地方"""
        # 有 CRC 结论时以它为准：字节数对上不代表内容对
        if self.ota_verify is not None:
            kind, n = self.ota_verify
            if kind == "ok":
                self.var_verify.set("✓ CRC 校验通过 ({:,} B)".format(n))
                self.lb_verify.configure(fg=C_OK)
            else:
                self.var_verify.set("✗ CRC 校验失败，未跳转")
                self.lb_verify.configure(fg=C_BAD)
            return

        if self.fw_size <= 0 or self.ota_saved <= 0:
            self.var_verify.set("")
            self.lb_verify.configure(fg=C_SUB)
            return

        if self.ota_saved == self.fw_size:
            self.var_verify.set("✓ 字节数一致 ({:,} B)".format(self.ota_saved))
            self.lb_verify.configure(fg=C_OK)
        elif self.ota_saved < self.fw_size:
            self.var_verify.set("✗ 少 {} B".format(self.fw_size - self.ota_saved))
            self.lb_verify.configure(fg=C_BAD)
        else:
            self.var_verify.set("✗ 多出 {} B".format(self.ota_saved - self.fw_size))
            self.lb_verify.configure(fg=C_BAD)

    def _set_state(self, text, color):
        self.var_state.set(text)
        self.lamp.itemconfigure(self.lamp_id, fill=color)

    def _flash_lamp(self, color):
        """完成/出错时闪三下，眼睛容易注意到"""
        def blink(n):
            if n <= 0:
                self.lamp.itemconfigure(self.lamp_id, fill=color)
                return
            cur = self.lamp.itemcget(self.lamp_id, "fill")
            self.lamp.itemconfigure(self.lamp_id,
                                    fill="#FFFFFF" if cur == color else color)
            self.root.after(180, lambda: blink(n - 1))
        blink(6)

    # ==========================================================================
    # 普通发送
    # ==========================================================================

    def send_text(self):
        if not (self.ser and self.ser.is_open):
            messagebox.showwarning("串口未打开", "请先点「打开串口」。")
            return

        raw = self.tx.get("1.0", "end-1c")
        if not raw:
            return

        if self.hex_tx.get():
            try:
                payload = bytes.fromhex(
                    raw.replace(",", " ").replace("0x", " ").replace("0X", " "))
            except ValueError:
                messagebox.showerror("HEX 格式错误",
                                     "HEX 模式只能填十六进制数，例如：01 A0 FF")
                return
        else:
            payload = raw.encode("utf-8")
            if self.crlf.get():
                payload += b"\r\n"

        try:
            self.ser.write(payload)
            self.tx_count += len(payload)
            self._update_counts()
            self._sys_line(">>> {} B: {}".format(
                len(payload),
                payload[:48].hex(" ").upper() + (" ..." if len(payload) > 48 else "")))
        except Exception as e:
            messagebox.showerror("发送失败", str(e))

    def wake_ota(self):
        if not (self.ser and self.ser.is_open):
            messagebox.showwarning("串口未打开", "请先点「打开串口」。")
            return
        try:
            self.ser.write(b"\x00")
            self.tx_count += 1
            self._update_counts()
            self._sys_line(">>> 唤醒字节 0x00（触发 OTA 窗口）")
        except Exception as e:
            messagebox.showerror("发送失败", str(e))

    def reset_via_debugger(self, silent=False, done_cb=None):
        """经 SWD 让 DAP-Link 复位目标板，MCU 从头跑、进入 OTA 窗口。

        silent=True 时（发送流程内调用）不弹框，只写日志。
        done_cb(成功?) 在后台线程结束后回调，用于串起"复位完立刻发数据"。
        """
        if not self.ocd_ready:
            if not self.ocd_exe:
                msg = "未找到 OpenOCD，无法通过调试器复位。请手动按板子复位键。"
            else:
                msg = ("OpenOCD 里缺 interface/{}.cfg，认不出当前调试器。"
                       "请手动按板子复位键。".format(self.ocd_iface))
            if silent:
                self._sys_line("!!! " + msg)
            else:
                messagebox.showwarning("无调试器", msg)
            if done_cb:
                done_cb(False)
            return

        def work():
            ok, info = openocd_reset(self.ocd_exe, self.ocd_scr,
                                     self.ocd_iface, self.ocd_target)
            self.rx_queue.put(("sys",
                               "--- SWD 复位 {}：{} ---".format(
                                   "成功" if ok else "失败", info)))
            if done_cb:
                done_cb(ok)

        if not silent:
            self._sys_line("--- 正在通过 SWD 复位 … ---")
        threading.Thread(target=work, daemon=True).start()

    # ==========================================================================
    # 固件发送
    # ==========================================================================

    def pick_file(self):
        path = filedialog.askopenfilename(
            title="选择固件",
            filetypes=[("固件 bin", "*.bin"), ("所有文件", "*.*")],
            initialdir=self._guess_fw_dir())
        if not path:
            return
        self.fw_path.set(path)
        try:
            self.fw_size = os.path.getsize(path)
        except OSError as e:
            messagebox.showerror("读取失败", str(e))
            return

        self.fw_name.set("{}\n{:,} B  ({})".format(
            os.path.basename(path), self.fw_size, human_bytes(self.fw_size)))
        self.var_target.set("{:,} B".format(self.fw_size))
        self.ota_saved = 0
        self.var_saved.set("0 B")
        self.pb.configure(value=0)
        self.var_verify.set("")
        self.var_tx.set("")

    def _guess_fw_dir(self):
        """自动去 build/Debug 找，省得每次翻目录"""
        last = self.cfg.get("last_file")
        if last and os.path.isfile(last):
            return os.path.dirname(last)
        here = app_dir()
        cand = os.path.abspath(os.path.join(here, "..", "..",
                                            "Application", "build", "Debug"))
        return cand if os.path.isdir(cand) else here

    def start_file_send(self):
        if not (self.ser and self.ser.is_open):
            messagebox.showwarning("串口未打开", "请先点「打开串口」。")
            return
        path = self.fw_path.get()
        if not path or not os.path.isfile(path):
            messagebox.showwarning("没有固件", "请先点「选择 .bin 文件」。")
            return
        if self.tx_thread and self.tx_thread.is_alive():
            return

        self.abort_evt.clear()
        self.ota_saved = 0
        self.ota_done_ts = 0.0
        self.ota_verify = None       # 清掉上一轮的校验结论，否则会误判这次成功
        self.ota_declared = 0
        self.ota_crc = ""
        self.var_saved.set("0 B")
        self.pb.configure(value=0)
        self.var_verify.set("")

        self.btn_go.configure(state="disabled")
        self.btn_stop.configure(state="normal")
        self._set_state("发送中…", C_WARN)

        chunk = int(self.chunk_var.get())
        delay = float(self.delay_var.get())

        self._sys_line("--- 开始发送 {} （{} B，块 {}，延时 {}ms）{} ---".format(
            os.path.basename(path), self.fw_size, chunk, delay,
            " · 新架构 FWUP" if self.fwup_mode.get() else ""))

        # 版本号只在主线程读一次再传进线程：Tk 变量不属于线程安全对象，
        # 发送线程里现读现用，偶尔会拿到空串或半截值
        if self.fwup_mode.get():
            self.tx_thread = threading.Thread(
                target=self._tx_worker_fwup,
                args=(path, chunk, delay, self._read_fw_ver()),
                daemon=True)
        else:
            self.tx_thread = threading.Thread(
                target=self._tx_worker, args=(path, chunk, delay), daemon=True)
        self.tx_thread.start()

    def _read_fw_ver(self):
        """取版本号输入框的值。非法就回落 1 并明说，不静默改用户的输入。"""
        raw = str(self.fw_ver_var.get()).strip()
        try:
            v = int(raw)
        except Exception:
            v = 0
        if not (1 <= v <= 65535):
            self._sys_line("--- 版本号「{}」不合法，本次按 1 发送 ---".format(raw))
            return 1
        return v

    def abort_file_send(self):
        if self.tx_thread and self.tx_thread.is_alive():
            self.abort_evt.set()
            self._sys_line("--- 用户中止发送 ---")

    def _tx_worker_fwup(self, path, chunk, delay_ms, fw_ver):
        """新架构：把「32 字节槽头 + 固件」直接发给正在运行的 App。

        和老 _tx_worker 的四处本质区别：
          1. 不做 SWD 复位 —— 目标是 App。复位会把 App 踢回 BootLoader，
             接收状态机还没建立就被打断了
          2. 头部先发 —— App 靠 'FWUP' 认出"这是固件"，业务数据不会误触发
          3. 不发 OTAE 尾部 —— 长度和 CRC 已经在头部里。再发 12 字节只会被
             App 当成固件数据，反而让 CRC 永远对不上
          4. 发完不复位 —— App 写完头会自己 NVIC_SystemReset()。
             外部复位若抢在前面，可能打断"写头"这一步，留下半成品
        """
        try:
            with open(path, "rb") as f:
                data = f.read()
        except Exception as e:
            self.rx_queue.put(("error", "读取文件失败: {}".format(e)))
            return

        total = len(data)
        if total <= 0:
            self.rx_queue.put(("error", "固件是空文件"))
            return

        crc = zlib.crc32(data) & 0xFFFFFFFF
        # 版本号由界面传入（_read_fw_ver 已做范围兜底）。它一路原样走到
        # 槽头 fw_ver → Boot 搬运时写进参数页 version，降级拦截就靠它比较

        # 头部字节定义统一走 build_fwup_header，别在这里另写一份 pack
        hdr = build_fwup_header(fw_ver, total, crc)

        try:
            self.ser.write(hdr)
        except Exception as e:
            self.rx_queue.put(("error", "发送头部失败: {}".format(e)))
            return

        self.rx_queue.put(("sys",
            "--- FWUP 头已发：ver={} len={:,} crc=0x{:08X} ---".format(
                fw_ver, total, crc)))

        # 等 App 把「头部扇区」擦掉再开始发数据。
        # 为什么必须等：App 收到 32 字节头后要先擦掉旧头所在的 4KB 扇区
        # （W25Q 典型 45ms），这期间它不能取串口数据。115200 下 45ms 会进来
        # 约 520 字节，而 App 的 DMA 环形缓冲只有 1024 字节 —— 头部一擦完
        # 紧接着还要擦第一个数据扇区，两段阻塞叠起来正好逼近溢出边界。
        # 这里先静一下，让缓冲从空开始，后面的"边收边擦"就宽松了。
        time.sleep(0.3)

        sent = 0
        t0 = time.time()
        last_ui = 0.0

        try:
            while sent < total:
                if self.abort_evt.is_set():
                    # App 那边会静默 3 秒后自行放弃，不需要再补什么命令
                    self.rx_queue.put(("sys",
                        "--- 已中止，App 会在 3 秒静默后放弃本次接收 ---"))
                    self.rx_queue.put(("tx", (sent, total, 0.0, True, True)))
                    return

                block = data[sent:sent + chunk]
                self.ser.write(block)
                sent += len(block)

                now = time.time()
                if now - last_ui > 0.08 or sent >= total:
                    self.rx_queue.put(("tx", (sent, total,
                                              sent / max(now - t0, 1e-6), False)))
                    last_ui = now

                if delay_ms > 0:
                    time.sleep(delay_ms / 1000.0)
        except Exception as e:
            self.rx_queue.put(("error", "发送中断: {}".format(e)))
            self.rx_queue.put(("tx", (sent, total, 0.0, True, True)))
            return

        self.rx_queue.put(("tx", (sent, total,
                                  sent / max(time.time() - t0, 1e-6), True)))
        self.rx_queue.put(("sys",
            "--- 发送完毕 {:,} B，等 App 校验并自复位（盯日志里的 [app-ota]）---".format(sent)))

    def _tx_worker(self, path, chunk, delay_ms):
        """后台线程：读文件 → 分块写串口。所有 UI 更新一律走队列。"""
        total = os.path.getsize(path)
        sent = 0
        crc = 0                  # 边发边算，省一次读盘。和 MCU 那边用同一套 CRC32
        t0 = time.time()
        last_ui = 0.0
        aborted = False

        try:
            if self.auto_reset.get():
                # 走 SWD 复位。DAP-Link 的 CDC 串口只透传 TX/RX，
                # DTR/RTS 不接目标板 NRST，串口侧脉冲对它是空操作。
                ok, info = openocd_reset(self.ocd_exe, self.ocd_scr,
                                         self.ocd_iface, self.ocd_target)
                self.rx_queue.put(("sys", "--- SWD 复位 {}：{} ---".format(
                    "成功" if ok else "失败", info)))
                if ok:
                    # BootLoader 复位后先打 [ota] ready 再进窗口，等它打印完
                    time.sleep(0.3)
                else:
                    self.rx_queue.put(("sys",
                        "!!! 复位失败，OTA 窗口可能没打开（可手动按板子复位后重发）"))

            with open(path, "rb") as f:
                while sent < total:
                    if self.abort_evt.is_set():
                        aborted = True
                        break
                    block = f.read(chunk)
                    if not block:
                        break
                    self.ser.write(block)
                    crc = zlib.crc32(block, crc)
                    sent += len(block)

                    now = time.time()
                    if now - last_ui > 0.08 or sent >= total:
                        speed = sent / max(now - t0, 1e-6)
                        self.rx_queue.put(("tx", (sent, total, speed, False)))
                        last_ui = now

                    if delay_ms > 0:
                        time.sleep(delay_ms / 1000.0)

        except Exception as e:
            self.rx_queue.put(("error", "发送中断: {}".format(e)))
            self.rx_queue.put(("tx", (sent, total, 0.0, True, True)))
            return

        speed = sent / max(time.time() - t0, 1e-6)
        self.rx_queue.put(("tx", (sent, total, speed, True, aborted)))

        # ---------------- 收尾 ----------------
        # 新协议（proto=2）：把 {长度, CRC32} 作为 12 字节尾部发下去。
        # BootLoader 收齐后把 Flash 读回来重算 CRC，对得上就自己跳转 ——
        # 这一步不需要我们再做复位，也就不存在"复位时机早了把 tail 截掉"的问题。
        #
        # 老协议（proto=1）：没有校验能力，只能沿用"等 sum 对齐 → 复位"的老路。
        # 关键：老协议绝对不能发尾部，那 12 字节会被当成固件写进 Flash。
        if aborted or sent < total:
            return

        if self.ota_proto >= 2:
            crc32 = crc & 0xFFFFFFFF
            # 停一下再发尾部：让 MCU 先把最后一块固件消化掉，
            # 尾部这 12 字节才更可能单独成块，不会被拆到两个读周期里
            time.sleep(0.05)
            try:
                self.ser.write(build_ota_tail(total, crc32))
            except Exception as e:
                self.rx_queue.put(("error", "尾部记录发送失败: {}".format(e)))
                return

            self.rx_queue.put(("sys",
                "--- 已发尾部记录 len={:,} crc=0x{:08X}，等待校验 … ---".format(
                    total, crc32)))

            # 15KB 的 CRC 只要几毫秒，5 秒是留给串口日志和擦参数页的余量
            deadline = time.time() + 5.0
            while time.time() < deadline:
                if self.ota_verify is not None:
                    break
                if self.abort_evt.is_set():
                    return
                time.sleep(0.05)

            if self.ota_verify and self.ota_verify[0] == "ok":
                self.rx_queue.put(("sys",
                    "--- 校验通过，BootLoader 已自行跳转，无需复位 ---"))
            elif self.ota_verify and self.ota_verify[0] == "fail":
                self.rx_queue.put(("sys",
                    "!!! 校验失败，固件没有跳转。板子停在 OTA，可以直接重发"))
            else:
                self.rx_queue.put(("sys",
                    "!!! 5 秒内没等到校验结果。板子可能还停在 OTA，看看日志"))
            # 新协议下不再补复位：校验失败时复位也没用（它会再验一次，还是失败），
            # 校验成功时 BootLoader 已经自己跳走了。
            return

        # ---------------- 老协议兼容路径 ----------------
        # BootLoader 写完固件后停在 OTA 里等人按键，不会自己跳。
        # 这里补一次复位，让新固件直接跑起来。
        #
        # 时机不能靠固定延时：最后不足一页的 tail 要等 BootLoader 的空闲超时
        # 才落盘，这个时间不受上位机控制。发完立刻复位会把 tail 截掉，
        # 导致 Flash 里是半个固件（表现为 sum 停在最后一页边界）。
        # 正确做法是盯它自己报的 sum，够了才复位。
        if self.ocd_ready and self.auto_reset.get():
            self.rx_queue.put(("sys", "--- 等待 BootLoader 落盘确认 … ---"))
            deadline = time.time() + 8.0
            while time.time() < deadline:
                if self.ota_saved >= total:
                    break
                if self.abort_evt.is_set():
                    return
                time.sleep(0.1)

            landed = self.ota_saved
            if landed < total:
                self.rx_queue.put(("sys",
                    "!!! 只确认落盘 {:,} / {:,} B，仍执行复位（请核对日志）".format(
                        landed, total)))
            else:
                self.rx_queue.put(("sys",
                    "--- 已确认落盘 {:,} B，复位跳新固件 ---".format(landed)))

            time.sleep(0.3)          # 让 tail 那页的擦写彻底结束
            ok, info = openocd_reset(self.ocd_exe, self.ocd_scr,
                                     self.ocd_iface, self.ocd_target)
            self.rx_queue.put(("sys",
                "--- 自动复位 {}：{} ---".format(
                    "成功" if ok else "失败", info)))

    def _on_tx_progress(self, sent, total, speed, done, aborted=False):
        pct = sent * 100.0 / total if total else 0
        self.var_tx.set("已发 {:,} / {:,} B  ({:.1f}%)   {:.1f} KB/s".format(
            sent, total, pct, speed / 1024.0))

        if not done:
            return

        # done 只来一次，这里的 sent 就是本次总共写出去的字节数
        self.tx_count += sent
        self._update_counts()

        self.btn_go.configure(state="normal")
        self.btn_stop.configure(state="disabled")

        if aborted:
            self._set_state("已中止", C_WARN)
            self._sys_line("--- 发送已中止（{} B）---".format(sent))
        elif sent >= total:
            # 两种架构等的东西不一样：老流程等 BootLoader 回报 sum/verify，
            # 新流程等 App 打完 [app-ota] 并自己复位回 BootLoader
            self._set_state("发送完毕，等待 App 校验自复位…"
                            if self.fwup_mode.get()
                            else "发送完毕，等待 BootLoader 回报…", C_ACCENT)
            self._sys_line("--- 发送完毕 {} B，用时 {:.2f}s ---".format(
                sent, total / max(speed, 1e-6) if speed else 0))
        else:
            self._set_state("发送异常结束", C_BAD)

    # ==========================================================================
    # 收尾
    # ==========================================================================

    def on_close(self):
        self.reader_alive = False
        self.abort_evt.set()
        save_config({
            "port": self.port_var.get(),
            "baud": self.baud_var.get(),
            "disp_mode": self.disp_mode.get(),
            "autoscroll": self.autoscroll.get(),
            "timestamp": self.timestamp.get(),
            "chunk": self.chunk_var.get(),
            "delay": self.delay_var.get(),
            "auto_reset": self.auto_reset.get(),
            "fwup_mode": self.fwup_mode.get(),
            "fw_ver": self.fw_ver_var.get(),
            "last_file": self.fw_path.get(),
            "geometry": self.root.geometry(),
        })
        try:
            if self.ser:
                self.ser.close()
        except Exception:
            pass
        self.root.destroy()


# ==============================================================================
# 入口
# ==============================================================================

def main():
    # DPI 感知必须在创建任何窗口之前设置。
    # 顺序反了的话，125% 缩放的屏幕上整个界面会被系统拉伸，字发虚。
    try:
        from ctypes import windll
        windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass

    root = tk.Tk()
    app = OtaSerialApp(root)

    # 自检模式：验证界面能建起来 + 串口枚举是否正常，不进入事件循环。
    # 打包后用它确认 pyserial 的 Windows 后端没被打包器漏掉。
    if "--check" in sys.argv:
        root.update_idletasks()
        root.update()
        msg = "UI self-check OK: {}x{}".format(root.winfo_width(),
                                               root.winfo_height())
        if sys.stdout is not None:   # --noconsole 打包后 stdout 可能为 None
            print(msg)
            ports = list_serial_ports()
            print("serial ports({}): {}".format(
                len(ports), ", ".join(p[0] for p in ports) or "-"))
            print("debugger: {} -> interface/{}.cfg [{}]".format(
                app.ocd_name, app.ocd_iface,
                "ready" if app.ocd_ready else "unavailable"))
            print("config: {}".format(CONFIG_PATH))
        root.destroy()
        return

    root.mainloop()


if __name__ == "__main__":
    main()
