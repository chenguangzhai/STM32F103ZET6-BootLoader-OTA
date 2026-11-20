# CHANGELOG

本文件记录 BootLoader / Application 两侧的代码改动。新记录追加在最上方。

---

## 2026-11-20 12:40 — OTA 加 CRC32 校验 + 收完主动跳转（proto=2）

### 解决什么问题

之前把「tail 落盘、sum 对上」当作升级成功的判据。但字节数对上不等于内容对：
串口掉一个字节、Flash 某位没擦干净、写入被打断，都可能让长度照样对上而内容是坏的。
`Boot_Loader_AppValid()` 只看 SP 是否在 RAM、复位向量是否在 Flash，
这只能挡住空片，挡不住「写了一半」的固件。

### 新增模块

- `Application/BSP/CRC/crc32.c/.h` — 标准 CRC32（查表法，1KB 表放 Flash）
  - `Crc32_Buf()` 算 RAM 里的数据
  - `Crc32_Flash()` 把 Flash 读回来算 —— 判 App 能不能跳用的是这个
- `Application/BSP/OTA/ota_param.c/.h` — App 参数区 + 校验
  - 参数页 = App 区最后一页 `0x0807F800`，OTA 可写范围收紧到 `0x0807F7FF`
  - 结构 24 B：`magic / length / crc32 / version / reserved / self_crc`
  - `self_crc` 覆盖前 20 字节，防止参数页自己写坏（擦到一半断电）

### 协议 proto=2

固件发完后上位机补 12 字节：`'OTAE' + length(LE32) + crc32(LE32)`

MCU 判定要求 **magic 匹配 且 声明长度 == 已收字节数**，两条同时满足才认。
`[ota] ready` 行末尾加 `proto=2`，上位机据此决定发不发尾部 ——
新工具配旧 BootLoader 会自动退回老流程，不会多发 12 字节把固件写坏。

### 上电判断顺序

1. 向量表合法性（原 `Boot_Loader_AppValid`）
2. 参数页有效性（magic + self_crc）
3. 固件 CRC（读回 Flash 重算比对）

参数页全 `0xFF` 时进 **legacy 模式**：照跳，但日志标 `unverified`。
不留这条退路的话，刷完新 BootLoader 会发现自己的 App 起不来，像被刷砖。

### 踩的坑：尾部 12 字节被当成固件写进 Flash

第一版只在数据块**开头**找 magic。实测 `sum` 变成 14880（14868+12）——
串口是流式的，上位机发完最后一块紧接着发尾部，MCU 一次 `Uart_Read`
把「532 B 固件 + 12 B 尾部」合并成一块，magic 不在开头，检测失败，整块喂进 Flash。

改为**整块扫描**：在 `buf[0..len-12]` 里找 magic，扫到就把前面 i 字节先 Feed，
再走校验。上位机侧同时加 50 ms 停顿，让尾部更可能单独成块。

### 实测（COM25 / 115200 / 256 B × 1 ms）

| 场景 | 结果 |
|---|---|
| 完整升级 | **1.46 s**（原来 9.1 s），`verify OK` → `jump to app` → `Hello, world!` |
| 再次上电 | `[boot] param: app=14868B crc=0x7D7DF238` → `crc ok` → 跳转 |
| 故意擦掉 App 中间一页 | `[boot] crc mismatch, stay in ota`，拒绝跳转 |
| 重刷恢复 | 通过，回到 `Hello, world!` |

第 3 行的关键：此时**向量表检查是通过的**（SP 和 Reset 向量都还在第一页），
只有 CRC 拦住了 —— 这就是为什么原来那两个检查不够。

### 资源占用

BootLoader 14372 B / 32KB（43.7%），RAM 5796 B / 64KB（8.8%）

### 串口助手

- 边发边算 `zlib.crc32`，发完补 12 字节尾部
- 新增解析：`[ota] tail:` / `verify OK` / `verify FAIL` / `[boot] crc mismatch`
- 「校验」一栏优先显示 CRC 结论（字节数对上不代表内容对）
- `proto=1` 时走原「等 sum → 复位」路径，完全兼容

### 备份

刷之前把板上原 BootLoader 存到了 `backup/bootloader_before_crc_20261120.bin`（32 KB）。

---

## 2026-11-20 11:20 — 串口助手改用 SWD 复位，OTA 全流程自动化

### 背景

这台机器的串口是 **DAP-Link 的 CDC 虚拟串口**（`VID_1FC9&PID_5601`，
工程里配的是 `interface: cmsis-dap`）。它的 DTR/RTS **不接目标板 NRST**，
所以之前加的 `DTR 复位` / `RTS 复位` 按钮在这套硬件上是空操作。

### 改动（仅 `Tools/SerialAssist/serial_assist.py`）

1. **新增 SWD 复位** — 调用 EIDE 自带 OpenOCD：
   `-f interface/cmsis-dap.cfg -f target/stm32f1x.cfg -c "init; reset run; shutdown"`
   - `find_openocd()` 按 `~/.eide/tools/openocd*/` 等候选路径自动探测
   - 找不到时按钮置灰，面板底部红字提示，降级为手动按键
2. **删除 DTR/RTS 复位按钮** — 对这套硬件无效，留着只会误导
3. **发送前自动 SWD 复位** — 勾选后先复位再发，不用手按复位抢窗口
4. **发送后自动复位跳新固件** — 关键：不能靠固定延时。
   最后不足一页的 tail 要等 BootLoader 的空闲超时才落盘，
   发完立刻复位会把 tail 截断（`sum` 停在 14336 而不是 14868）。
   现在改为**轮询 `self.ota_saved` 直到等于文件大小**（上限 8s）再复位。
5. **修复升级完成后统计被清零** — 自动复位后 BootLoader 重新打印
   `[ota] ready`，原逻辑会把"已落盘"清成 0，看着像失败。
   新增 `ota_done_ts`，完成 15 秒内的 ready 不清零，状态显示"升级完成，重启中…"
6. **复选框换用 tk.Checkbutton** — clam 主题下 ttk 的选中标记渲染成 `✕`，
   看着像"关闭"，容易误判当前状态

### 实测（真机，COM25，14868 B）

```
5.5s  落盘 14868 B   写入完成        ✓ 字节数一致 (14,868 B)
6.0s  落盘 14868 B   升级完成，重启中…
9.1s  落盘 14868 B   已跳转 App      ✓ 字节数一致 (14,868 B)
```

日志侧：

```
[ota] tail 0x0800B800 532B ok, sum: 14868
[rst] PIN / SFT              ← SWD 复位（SYSRESETREQ 会连带拉一次 NRST）
[ota] ready, app=0x08008000, page=2048B
[boot] jump to app 0x08008000
Hello, world!
[w25q] id=0xEF4018 OK ... 五项全过
```

**这是第一次证明"OTA 写进去的固件真的能启动"**——此前只验证到 `sum` 一致。

### 遗留（BootLoader 侧待做）

- 收完固件后应「校验 → 主动跳转」，现在要等上位机补一次复位
- tail 落盘依赖空闲超时，时间偏长（发送完约 3.5s 才写），可考虑显式收尾命令

---

## 2026-11-20 10:55 — 新增工程专用串口助手（Tools/SerialAssist）

### 背景

SSCOM 5.13.1 无法切换字符编码（配置文件 50 项里没有任何编码选项，只认
Windows ANSI/GBK），中文日志必然乱码；且没有文件发送、无法显示 OTA 进度。

### 新增

`Tools/SerialAssist/`（独立于固件，不影响两侧构建）：

- `serial_assist.py` — tkinter + pyserial 实现
- `run_serial_assist.bat` — 双击启动，自动找 `C:\Python311\pythonw.exe`
- `README.md` — 用法与排错

### 相比通用串口助手的关键能力

1. **固定 UTF-8 解码**（增量解码器，解决多字节字符跨数据块被切开）
2. **固件分块发送** — 块大小 64~2048、块间延时 0~20ms、实时进度与速率、可中止
3. **解析 BootLoader 日志** — 抓 `[ota] page/tail ... sum: N` 还原真实落盘进度
4. **字节数对账** — 文件大小 vs 末尾 `sum`，直接给 ✓ / ✗ 结论
5. **日志按来源着色** — `[boot]` 紫 / `[ota]` 青 / `[rst]` 橙 / ok 绿 / FAIL 红
6. **DTR / RTS 脉冲复位** — 打开发送前可自动复位抢 3 秒 OTA 窗口
7. **打开串口不拉 DTR** — 先构造 `Serial()` 并把 dtr/rts 置 False 再 open，
   避免 open 瞬间误复位板子
8. 配置记忆（`assist_config.json`）：串口、波特率、块大小、窗口位置

### 实现要点

- 串口读线程只负责把字节丢进队列，所有 UI 更新回主线程 `after()` 轮询，避免跨线程改控件
- `tkinter` 的 DPI 感知必须在 `tk.Tk()` **之前**设置，顺序反了 125% 屏上会整体发虚
- 右栏宽度不写死（不用 `grid_propagate(False)` + 固定 width），
  否则内容最小宽度一超标，控件会被画到窗口外且极难察觉

### 修复：端口自动选择被蓝牙虚拟串口干扰（10:58）

首次实机运行发现选中的是 `COM13 蓝牙链接上的标准串行`，不是板子。

本机 7 个串口中 6 个是 `BTHENUM\...` 蓝牙虚拟口，只有 COM25 是真实 USB 设备。
原逻辑按描述里的 "CH340/CP210" 关键字挑，匹配不上通用的 "USB 串行设备"，
于是回落到按 COM 号排序的第一个 —— 正好是蓝牙口。

改为按 `hwid` 判断：`BTHENUM` 开头视为蓝牙虚拟口，含 `USB VID` 视为真实设备，
真实 USB 排最前、蓝牙排最后，自动选中列表首项。

---

## 2026-11-19 22:01 — 串口日志输出改为纯 ASCII

### 问题

加入 `PrintResetCause()` 后，串口日志出现乱码：

```
[rst] PIN  / NRST 寮曡剼
[rst] POR  / 涓婄數鎴栨帀鐢?
```

原因是编码不一致：源码以 UTF-8 保存，汉字占 3 字节，这些字节被原样烧进固件；
串口助手默认按 GBK 解码（1 个汉字 2 字节），「引脚」的 6 个字节 `E5 BC 95 E8 84 9A`
被拆成 3 个 GBK 汉字 → `寮曡剼`。字节数不是 2 的整数倍时还会多出一个 `?`。

### 改动

- `Boot_Loader/Core/Src/main.c::PrintResetCause` — 6 条日志字符串由中文改为英文：
  `POR / power-on or brown-out`、`PIN / NRST pin`、`SFT / software reset`、
  `IWDG / independent watchdog`、`WWDG / window watchdog`、`LPWR / low-power wakeup`。
  注释里补充了乱码成因说明。
- 全项目扫描所有 `.c` 的字符串字面量，确认 `Uart_Printf` / `OTA_LOG` 的参数中已无中文。
  中文仅保留在注释中（注释不进二进制，不受影响）。

### 副作用

固件体积 12008 B → **12016 B**（英文比中文长 8 字节）。日志改用 ASCII 后，
换任何串口工具、任何系统都不会再乱码。

---

## 2026-11-17 12:38 — 新增复位原因诊断

### 问题

按一次复位键，串口打印出 3 次 `[ota] ready`，其中 2 次是截断的
（`app=0` / `app=0x080` / 完整），说明 BootLoader 被复位打断了不止一次。
`Uart_Printf` 是阻塞发送，截断只可能来自复位，但无法确定复位来源。

### 改动

- `Boot_Loader/Core/Src/main.c::PrintResetCause` — 新增函数，读 `RCC->CSR`
  的复位标志，打印 POR / PIN / SFT / IWDG / WWDG / LPWR，最后
  `__HAL_RCC_CLEAR_RESET_FLAGS()` 清标志（不清则下次仍读到旧值）。
- `Boot_Loader/Core/Src/main.c::main` — 在 `Uart_Init()` 之后调用一次。

### 说明

判读依据：真正上电 = POR + PIN 两条（上电瞬间 NRST 也会被拉低）；
单纯按复位键 = 只有 PIN 一条；看门狗 = IWDG。据此可区分「按键抖动产生的多个脉冲」
与「固件自身循环重启」。

体积 11700 B → 12008 B（+308 B）。

---

## 2026-11-17 12:31 — OTA 分区从 16KB 扩到 32KB

### 问题

BootLoader 加 `-Os` 后仍有 11700 B / 16 KB，加入 W25Q 驱动（约 750 B）和
HAL SPI（约 2.5~3 KB）后会溢出；后续还要放 CRC32、参数区、传输协议，
16 KB 撑不到功能做全。

### 改动

- `Boot_Loader/STM32F103XX_FLASH.ld::MEMORY` — `FLASH LENGTH` 由 `16K` 改 `32K`。
- `Boot_Loader/Application/BSP/Flash/flash_app.h::BOOT_SIZE` — 由 `0x4000U` 改 `0x8000U`。
  `APP_ADDR` 由 `BOOT_ADDR + BOOT_SIZE` 推导，自动变为 `0x08008000`，
  因此 `Ota_Init()` / `Boot_Loader_AppValid()` / `Boot_Loader_JumpToApp()` 三处调用
  全部自动跟随，无需逐个修改。
- `Application/STM32F103XX_FLASH.ld::MEMORY` — `ORIGIN 0x08004000, LENGTH 512K-16K`
  改为 `ORIGIN 0x08008000, LENGTH 512K-32K`。
- `Application/Core/Src/main.c::APP_ADDR` — `0x08004000U` 改 `0x08008000U`
  （必须与链接脚本 ORIGIN 一致，否则 `SCB->VTOR` 指错，一进中断即 HardFault）。
- `Application/.eide/eide.yml::targets.Debug.uploadConfigMap.OpenOCD.baseAddr` —
  `0x08004000` 改 `0x08008000`。此项若漏改，按 bin 烧录会写到旧地址。

### 说明

`Boot_Loader.c` 的 `APP_VEC_BASE 0x08000000` 是判断「复位向量落在 Flash 区」的掩码，
与分区边界无关，未作改动。

### 验证

- BootLoader：FLASH 11700 B / 32 KB（35.71%），RAM 5800 B / 64 KB
- Application：FLASH 14868 B / 480 KB（3.02%），RAM 3320 B / 64 KB
- `g_pfnVectors = 0x08008000`，`Reset_Handler = 0x0800AE90`
- bin 头 8 字节 `00 00 01 20 | 91 ae 00 08` → SP = `0x20010000`，复位向量 = `0x0800AE91`
- 上板：`[boot] jump to app 0x08008000` → `Hello, world!` → W25Q 五项全通过

### 注意（烧录顺序）

必须先烧 BootLoader、再烧 Application。只烧 BootLoader 后会出现
`[boot] no valid app, waiting for firmware`，这是正常的（新 App 区还是空白）。

---

## 2026-11-17 12:26 — BootLoader 优化等级由 -O0 改为 -Os

### 问题

BootLoader 体积 13376 B / 16 KB（81.6%），剩余仅 3008 B，装不下 W25Q 驱动 + HAL SPI。

### 改动

- `Boot_Loader/.eide/eide.yml::targets.Debug.toolchainConfigMap.GCC.options.c/cpp-compiler.optimization`
  — `level-debug`（-O0）改为 `level-size`（-Os）。

### 验证

13376 B → **11700 B**，省 1676 B（12.5%）。省得比预期少，因为 startup 与 HAL
的固定开销不随优化等级变化。
