# STM32 OTA 串口助手

专为这个 OTA BootLoader 工程写的串口调试工具，替代 SSCOM。

## 为什么不用 SSCOM

| 痛点 | SSCOM | 本工具 |
|---|---|---|
| 中文日志显示成 `寮曡剼` | 固定 GBK，无法切换 | 固定 UTF-8，可切 GBK / HEX |
| 发固件要手动分块、计时 | 无内建文件发送 | 选 bin 直接发，块大小 / 延时可调 |
| 进度全靠数日志行 | — | 解析 `[ota] page ... sum: N` 自动算进度 |
| 丢没丢数据不知道 | — | 文件大小 vs `sum` 自动对账 |
| 升级完要按板子复位键 | — | **SWD 复位**（经 DAP-Link），全自动 |
| 每次打开都拉一次 DTR | 会（可能误复位） | 打开时 DTR/RTS 保持低，不打扰板子 |

## 复位方式：必须走 SWD，串口 DTR 不行

调试器的 CDC 串口只透传 TX/RX，**DTR/RTS 根本没有接到 MCU 的 NRST**，
所以串口侧的 DTR 脉冲是空操作 —— SSCOM 里勾 DTR 也照样没反应。

真正有效的是通过 SWD 让调试器复位芯片。本工具调用 EIDE 自带的 OpenOCD：

```
openocd -f interface/cmsis-dap.cfg -f target/stm32f1x.cfg \
        -c "init; reset run; shutdown"
```

一次干净的单脉冲，实测复位后日志只出现一组 `[rst]`，不会像按键抖动那样打好几遍。

### 换调试器不用改配置

启动时会读当前插着的 USB 设备，自动换成对应的 interface 脚本：

| 调试器 | USB VID:PID | interface 脚本 |
|---|---|---|
| DAP-Link / DAPLink | 1FC9:5601、1FC9:0132、0D28:0204 | `cmsis-dap.cfg` |
| ST-Link V1 / V2 / V2-1 / V3 | 0483:3744 / 3748 / 374B / 374D / 374E / 374F | `stlink.cfg` |
| J-Link | 1366:0101、1366:0105 | `jlink.cfg` |

面板底部绿字会显示实际认到的设备名，比如 `SWD 复位就绪 · ST-Link V2`。

判断依据是 **CFGMGR32 的 PRESENT 过滤**，只认此刻真的连着的设备。
不能用注册表 `Enum\USB` 判断 —— 拔掉的设备键还留在里面，换调试器时
会认成旧的那个，然后复位必然失败（这是我自己踩过的）。

认错了、或者用的是表里没有的设备，在 `assist_config.json` 里手写一行覆盖：

```json
"ocd_iface": "stlink"
```

### ST-Link 的坑：报 open failed

OpenOCD 通过 libusb 访问 ST-Link，而 STM32CubeProgrammer / ST-Link Utility
装的官方驱动会独占设备，两者不能共存。表现就是复位失败，
OpenOCD 报 `Error: open failed`（我实测指定 stlink 时就是这个报错）。

三种解法，按省事排序：

1. 换 **ST-Link V2-1 或 V3**（Nucleo 板载那种）—— 走 HID，通常免驱直接用
2. 用 Zadig 把 ST-Link 的接口驱动从 ST 官方换成 **WinUSB**
3. 继续用 DAP-Link —— 兼容性最好，本工程全程用它验证

**找不到 OpenOCD、或认到的调试器在 OpenOCD 里没有对应脚本时**，
按钮自动置灰，面板底部用红字说明原因，此时手动按板子复位。

## 运行

**方式一：exe（免装 Python，推荐拷给别人用）**

双击 `dist\STM32_OTA_SerialAssist.exe`。

单文件打包，第一次启动要先把 tkinter 解包到临时目录，比源码版多等 1~2 秒，
之后每次启动都是这个速度（没有缓存加速）。体积约 10 MB，因为 Python 运行时
和 Tcl/Tk 都打进去了。

配置写在 **exe 同目录** 的 `assist_config.json`。换机器时把 exe 和这个 json
一起拷走，串口、波特率、块大小、窗口位置都能带过去。exe 改名不影响运行。

**方式二：源码运行（改代码时用）**

双击 **`run_serial_assist.bat`**。

需要 `C:\Python311`（自带 tkinter + pyserial）。若装在别处，改 bat 里的路径，
或直接：

```
python serial_assist.py
```

首次运行会在同目录生成 `assist_config.json`，记住串口、波特率、块大小、
窗口位置，下次打开自动恢复。

## 打包成 exe

改完代码后重新打包（约 15 秒）：

```
cd Tools\SerialAssist
C:\Python311\python.exe -m PyInstaller --noconfirm --onefile --noconsole ^
  --name STM32_OTA_SerialAssist --icon serial_assist.ico ^
  --hidden-import serial.tools.list_ports_windows ^
  --exclude-module PIL --exclude-module numpy ^
  --exclude-module matplotlib --exclude-module pandas ^
  --distpath dist --workpath build serial_assist.py
```

产物在 `dist\STM32_OTA_SerialAssist.exe`。

几个参数不是随便写的：

- `--noconsole`：GUI 程序，不带它每次启动会弹一个黑框
- `--hidden-import serial.tools.list_ports_windows`：pyserial 的 Windows
  端口枚举后端是运行时按平台动态导入的，静态分析扫不到。漏了它程序照样能启动，
  但端口列表永远是空的 —— 这是打包串口工具最典型的坑
- `--exclude-module PIL/numpy/...`：这些只是我生成图标时用过，运行时不需要，
  排掉能省几 MB

打包后务必自检一遍，确认端口枚举没被漏掉：

```
dist\STM32_OTA_SerialAssist.exe --check
```

正常输出类似：

```
UI self-check OK: 1280x900
serial ports(7): COM25, COM13, COM14, COM3, ...
config: ...\dist\assist_config.json
```

注意最后一行必须是 exe 所在目录，**不能**是 `C:\Users\...\AppData\Local\Temp\_MEIxxxx`。
如果指向了 Temp，说明 `app_dir()` 被改坏了，配置每次关窗都会丢。


## 界面

```
┌ 端口 / 波特率 / 打开串口 ──────────────── 时间戳 自动滚动 显示[UTF-8] ┐
├──────────────────────────────┬──────────────────────┤
│                              │  OTA 状态            │
│   接收区（按来源着色）        │   状态 / App基址      │
│   [boot] 紫  [ota] 青        │   页大小 / 已落盘     │
│   [rst] 橙   ok 绿  FAIL 红  │   进度条 / 对账结论   │
│                              ├──────────────────────┤
│                              │  固件发送            │
│                              │   选文件 / 块 / 延时  │
│                              │   开始 / 中止         │
│                              │   SWD 复位 / 唤醒     │
├──────────────────────────────┴──────────────────────┤
│  发送框                                    HEX  \r\n │
│                                            [发送]    │
├─────────────────────────────────────────────────────┤
│ 已连接 COM3 115200          RX 12.3 KB  TX 14.5 KB   │
└─────────────────────────────────────────────────────┘
```

## 清空接收（Ctrl+L）

状态栏右下角「清空接收」只清接收区这一屏字，**串口一个字节都不动**。

| 保留的东西 | 为什么不能清 |
|---|---|
| 驱动层 `in_waiting` | 板子刚发来、驱动已收到但还没读的字节 |
| `rx_queue` | 已读到、还没渲染的块 |
| `line_buf` | 跨块的半行，OTA 面板靠它把一行拼完整 |
| 增量解码器 | 可能正存着半个多字节字符 |
| `rx_count` / `tx_count` | 累计收发统计 |

所以清完之后只要板子还在发，屏幕会接着往下滚 —— **清屏不等于停收，
更不等于丢板子的数据**。实测：清屏瞬间队列里压着 6 块，4 秒后 380 B
全部补上屏，一行没少。

唯一动到的"数据"是 HEX 模式下的偏移：清屏后从 `00000000` 重新起算。
不归零的话第一行会接着上一屏的地址，看起来像中间丢了一段，反而更迷惑。

## 一次完整 OTA 升级（全自动，不用碰板子）

1. 点 **刷新**，选中端口（真实 USB 口会排在最前，蓝牙虚拟口排在最后），
   波特率 115200，点 **打开串口**
2. 点 **选择 .bin 文件**，选 `Application/build/Debug/Application.bin`
   （默认会直接打开这个目录）
3. 块大小 **256**、块延时 **1ms**——这是实测能稳定收的组合
4. 保持 **发送前调试器复位** 勾选
5. 点 **开始发送**

接下来全自动：

```
SWD 复位            → [ota] ready, proto=2，进入 3 秒窗口
发 14,868 B         → [ota] page ... 逐页落盘
发 12 字节尾部      → [ota] tail: len=14868 crc=0x7D7DF238
MCU 读回 Flash 校验  → [ota] verify OK 14868B，参数页写入 {长度, CRC}
MCU 自己跳转        → [boot] jump to app
```

实测全程 **1.46 秒**（原来要 9.1 秒），最后那次复位彻底省掉了。

右侧状态会依次走：`等待固件` → `接收中` → `收到尾部记录，校验中` →
`CRC 校验通过，跳转中…` → `已跳转 App`，全程不用按键。

⚠️ 如果显示 **✗ CRC 校验失败**，说明 Flash 里的内容和发出去的对不上。
板子会停在 OTA 等重传，直接再点一次发送即可，**不用复位**。

### 尾部记录：12 个字节

固件发完之后补 12 字节，告诉 MCU「发完了、共多少、CRC 多少」：

| 偏移 | 内容 |
|---|---|
| 0..3 | `OTAE` |
| 4..7 | 固件长度，小端 |
| 8..11 | CRC32，小端 |

MCU 判定时不只看 magic，还要求**声明的长度必须等于已收字节数**，
两个条件同时满足才认。固件里恰好出现 `OTAE` 的概率约 2⁻³²，
再叠上长度约束，实际误判可以忽略。

CRC 是 MCU **把 Flash 读回来重算**的，不是边收边算。这点很关键：
边收边算只能证明「我收到了什么」，证明不了「Flash 里存的是什么」。
擦除不干净、写入被打断、电压跌落，都会让两者不一样。

### 为什么现在不用再复位了

`proto=1`（加校验之前的 BootLoader）收完固件后不会自己跳，工具得补一次复位。
`proto=2` 之后 MCU 自己完成「校验 → 写参数 → 跳转」，工具只管发数据。

工具靠 `[ota] ready` 末尾有没有 `proto=2` 来区分：
新工具配老 BootLoader 会**自动退回**老流程（等 sum 对齐再复位），
不会因为多发 12 字节把固件写坏。

### 上电时的判断顺序

复位后 BootLoader 按这个顺序决定跳不跳：

```
1. 向量表像不像样         SP 在 RAM？复位向量在 Flash？   不像 → 停在 OTA
2. 参数页有没有内容       全 0xFF = 从没写过              空 → legacy 模式，跳但不保证
3. 参数页自身有没有写坏    magic + self_crc               坏 → 停在 OTA
4. 固件到底对不对         读回 Flash 重算 CRC 比对        不对 → 停在 OTA
```

第 2 步的 legacy 模式是给「加校验之前刷进去的老固件」留的退路 ——
否则刷完新 BootLoader 会发现自己的 App 起不来了，像被刷砖了一样。
日志里会写 `[boot] no param, unverified app`，提示这份固件没被验证过。

### 为什么不能"发完就立刻复位"

最后不足一页的 tail（本例 532 B）要等 BootLoader 的**空闲超时**才落盘，
这个时间不受上位机控制。发完立刻复位会把 tail 截断，Flash 里只留下半截固件
（表现为 `sum` 停在最后一页边界，例如 14336）。
所以工具是盯 `sum` 而不是靠固定延时。

## 日志怎么读

工具会解析这几类行，直接反映到右侧面板：

| 日志 | 含义 | 面板反应 |
|---|---|---|
| `[ota] ready, app=0x08008000, page=2048B` | BootLoader 就绪 | 显示基址和页大小；**升级刚完成时的那次不会清零** |
| `[boot] enter OTA` | 收到数据，进入升级 | 状态变"接收中" |
| `[ota] page 0x... ok, sum: N` | 一页落盘 | 进度按 N 往前走 |
| `[ota] tail 0x... xxxB ok, sum: N` | 尾块落盘 | 进度 100%，出现对账结论 |
| `[boot] jump to app 0x...` | 跳转 App | 状态变"已跳转 App" |
| `[rst] PIN / SFT / POR / IWDG` | 复位来源 | 看门狗复位会标红 |

SWD 复位会同时报 `PIN` 和 `SFT`：OpenOCD 发的是 SYSRESETREQ（软件复位请求），
芯片内部会顺带把 NRST 拉一次，所以两条都置位。这是正常的，
只有单独一条 `PIN` 且反复出现好几次，才是按键抖动。

## 常见问题

**打开串口报"被占用"**
SSCOM / XCOM / VSCode 串口监视器还开着。关掉它们。

**SWD 复位没反应**
1. 板子没上电 / USB 只接了串口没接 SWD
2. DAP-Link 被别的程序占着 —— VSCode 的 Cortex-Debug、Keil、STM32CubeProgrammer
   同时只能用一家，先断开调试会话
3. 面板底部如果是红字"未找到 OpenOCD"，说明 EIDE 工具链路径变了，
   重装 EIDE 的 OpenOCD 或改脚本里的 `find_openocd()` 候选路径

**发完不自动跳 App**
先确认右侧出现过 `✓ 字节数一致`。如果一直是 `✗ 少 N B`，
说明 tail 没落盘就去复位了 —— 本工具已经改成等 `sum` 确认，正常不会发生。

**中文还是乱码**
本工具固定按 UTF-8 解，正常不会乱。若源码里有 GBK 字符串，
把右上角"显示"切成 GBK 看。
