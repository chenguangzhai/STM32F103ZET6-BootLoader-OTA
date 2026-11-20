# CHANGELOG

本文件记录 BootLoader / Application 两侧的代码改动。新记录追加在最上方。

---

## 2026-11-20 17:05 — OTA 黑匣子（W25Q 事件日志）

### 为什么改

串口日志是"现场直播"：拔了线、断了电就什么都不剩。于是遇到"设备自己变回旧版了"
"昨天还好好的今天一直重启"这类反馈时只能靠复现去猜，而这类问题往往几十次才出一次。
黑匣子把最近 128 条关键事件落到 W25Q，每次上电自动倒出最近 5 条，
"它经历过什么"从猜测变成可查的事实。

### 新增

- `Boot_Loader/Application/BSP/OTA/ota_log.h/.c`
  - 地址 `0x101000`，紧跟试用确认标记（`0x100000`）之后，独占一个扇区。
  - 记录 32 字节：`magic / seq / evt / ver / fw_len / arg / reserved / crc`，
    一扇区 128 条。每条自带 CRC —— 掉电写半截的记录必须能被识别出来，
    否则后面的扫描位置全错一位。
  - 扇区内顺序追加，写满擦整扇区重来，**序号不归零**（序号是唯一能跨擦除
    排序的依据，重置了就分不清新一轮和上一轮）。
  - 16 种事件：五种复位来源、搬运、已是最新、试用计数、转正、试用失败、
    回滚、拒绝降级、参数重建、进入 OTA、跳转未响应、无可用 App。

### 埋点

Boot 侧：启动事件 + `ota_apply.c` 里的搬运/回滚/拒绝降级/参数重建，
`main.c` 里的试用计数/转正/失败/进入 OTA/无可用 App。

### 踩到的坑（很隐蔽）

`0x101000` 上原本就有别的东西（`08 4E 02 00`，旧工程或别的工具留下的），
不是全 0xFF。Flash 只能 1→0，所以第一次写入直接失败，而 `Append` 的返回值
被 `(void)` 掉了 —— 表现是"黑匣子一条记录都没有，日志里也看不出任何异常"。

修法：`Log_Scan` 遇到无效记录时再判一次"是不是空白"，不是空白就擦掉整扇区重来，
序号接着上一条往上走（从序号跳跃能看出中间清过场）。

### 另一个冲突（脚本侧）

黑匣子回放的历史行会污染上位机的关键字判定：历史里那句
`[log] #11 拒绝降级 ...` 让脚本把一次正常升级误报成"被拒绝"。
`ota_headless.py` 的 `saw()` / `wait_for()` 改为跳过 `[log]` 开头的行 ——
历史是历史，不是这一次发生的事。

### 真机验证

```
[log] 最近 5 条（共 24 条）：
[log] #20 试用失败 v=14 18940B a=1
[log] #21 试用计数 v=14 18940B a=2
[log] #22 看门狗复位 v=0 0B a=10
[log] #23 试用失败 v=14 18940B a=2
[log] #24 回滚 v=13 18960B a=0
```

一次完整的"坏固件 → 三次狗咬 → 自动回滚"在断电之后仍然查得到。
另外拒绝降级（`#11 拒绝降级 v=5 ... a=13`）、搬运（`#6 搬运固件 v=13`）、
转正（`#8 试用转正 v=13`）都已验证。

### 体积

Boot 23108 → **24128 B**（32KB 的 73.6%，crc `0x6AFF5FD1`）。

---

## 2026-11-20 16:52 — 参数页双备份（两页轮转 + 老格式兼容）

### 为什么改

参数页原来只有一页（`0x0807F800`）。写它的过程是「擦 2KB 页 → 写 24 字节」，
那段擦除要 20~40ms，而升级恰恰是最容易断电的时刻。这一下掉电就变成
「有内容但 self_crc 不对」，而自愈**故意不碰**这种状态（不猜），于是 Boot
停在 OTA 等串口 —— 现场没有串口就是一块砖。

### Boot 侧

- `ota_param.h`：拆成 `OTA_PARAM_ADDR_A = 0x0807F000` / `OTA_PARAM_ADDR_B = 0x0807F800`；
  `OtaParam_t` 在 `self_crc` 前插入 `seq`（24 → 28 字节）；`OTA_APP_MAX` 相应少一页。
  `OTA_PARAM_ADDR` 保留为 A 页别名。
- `ota_param.c`：新增 `Page_Load()` / `LoadBest()`。
  - 每次只写「当前没在用的那一页」，序号 +1，写完回读校验（seq/crc/len 三项）。
  - 回读失败**不补写另一页** —— 那页是此刻唯一有效记录。
  - 老格式（24 字节、`self_crc` 在偏移 20）单独识别，命中按 `seq=0` 接入轮转，
    板上已有记录无需人工迁移。老格式命中时必须显式清 `p->seq`，那个位置是垃圾。
  - `Ota_Param_Erase()` 改为擦两页（只擦一页等于没擦）。

### App 侧

- `app_ota.c`：新增 `ReadParamPage()` / `ReadParamBest()`，`AppOta_Confirm()` 改为双页取最新。
  这一步漏了会很隐蔽：Boot 把参数写到 A 页后，App 仍读 B 页那份旧记录，
  写出的确认标记与参数页对不上，试用永远转不正，现象是"升完级又自己回滚了"。
- `ota_param.h`：地址宏与结构体同步（两边布局必须一字不差，已加注释警示）。

### 真机验证（全部通过）

| 场景 | 结果 |
|---|---|
| 板上有老格式记录（B 页）、A 页空 | 按 legacy 识别，正常启动，无人工干预 |
| 升级一次 | A 页写新格式 `seq=1 / ver=10`，B 页保持老记录，两页共存 |
| 升级第二次 | 轮转到 B 页 `seq=2`；转正时再写 A 页 `seq=3` |
| SWD 擦掉 active 页（模拟掉电） | 无缝回落到另一页，直接跳 App，未停 OTA |
| 两页都擦 | `[param] 参数页丢失，已按槽 A 重建为试用态` + `[trial] 试用转正` |

### 体积

Boot 22768 → **23108 B**（32KB 的 70.5%，crc `0xE637B383`）；
App 18732 → **18960 B**（crc `0xB5A60E6A`，build 16:54:10）。

### 顺带确认

参数页 `version` 一直显示 1 的原因：槽 A 的槽头 `fw_ver` 就是 1（历史某次用默认版本发的）。
用 `--ver 10` 发一版后参数页正确变成 10，说明 `version` 链路本来就是通的，不是 bug。

---

## 2026-11-20 16:45 — 修掉"连按复位键被当成试用失败"（复位原因 + 后备寄存器握手）

### 问题

升级后 5 秒确认门限内连按三次复位键，Boot 会认定新固件三次都没撑住，**自动回滚到旧版**。
根因是 `TrialGuard()` 只看"状态=试用 + 没有确认标记"就 `cnt+1`，**不问这次是怎么复位的**：
它把"人按复位键"和"固件死机复位"当成了同一件事。

### 两条判定依据（缺一不可）

1. **复位原因快照**（`ResetCause_Snapshot()`）
   `RCC->CSR` 的复位标志只能读一次（读一次就得清，否则下次读到的还是旧值），
   而 `TrialGuard` 是几百毫秒后才需要这个信息。所以开机第一条语句就抄进 RAM 的
   `s_rst_cause`，`PrintResetCause()` 改成只打印、不再读也不清。

   只有 `IWDG/WWDG`（看门狗咬的）才算"固件自己没撑住"。
   `PIN`（人按）、`SFT`（App 收完固件主动回 Boot，属正常流程）、`POR`（插拔电源）
   一律不计，并且**不写参数页**（省一次 2KB 页擦写）。

2. **"跳出去没回来"标记**（BKP_DR1 握手）
   看门狗只能抓"进了主循环之后死掉"。固件死在时钟配置里、或 HardFault 卡在默认
   处理函数里时**狗还没开**，板子就那么挂着，Boot 只看到"复位键"，永远不回滚。
   做法：`Bkp_MarkJump()` 在 `Boot_Loader_JumpToApp()` 之前写一笔，
   App 进 `main` 的第一条语句 `Bkp_ClearJumpMark()` 擦掉。下次上电这笔还在，
   就说明上次控制权交出去根本没活过来，计一次失败。

   选 BKP_DR1 而不是 `.noinit` 变量：Boot 和 App 是两个独立工程，各编各的链接脚本，
   普通变量在两边地址未必一致，还可能和 App 的栈撞上；BKP 是芯片自带的独立小存储，
   和 RAM/Flash 布局无关，两边只需约定同一个魔术字（和双槽布局同一个思路）。

### 踩坑：BKP_DRx 只有 16 位

第一版魔术字写成 32 位的 `0x4A4D5031`，回读 `0x40006C04` 只有 `0x00005031` ——
F1 的后备数据寄存器只有低 16 位有效，高位被静默丢掉，`==` 永远不成立，
整条握手失效而**日志上看不出任何异常**。改成 16 位 `0xA55A` 后 dump 回读正确。
（注：`BKP_TypeDef` 首个字段是 `RESERVED0`，所以 `DR1` 的地址是 `0x40006C04` 而不是 `0x40006C00`。）

### 验证（真机，逐条对照）

| 场景 | 日志 | 结果 |
|---|---|---|
| 试用期内连按 4 次复位 | `本次是复位键复位，不计试用失败（0/3）` ×4 | 计数不动，参数页 `state=1 cnt=0`，未回滚 |
| 不喂狗的固件 | `判定失败：看门狗复位` → `2/3` → `3/3` → `回滚：槽 B` | 狗咬的照样计数并回滚 |
| 回滚后 | `[slot] A：空` | 坏固件槽头被作废，不会反复重装 |

体积：Boot 22352 → **22768 B**（69.5%，crc `0xF4A99BC6`），App 18668 → **18732 B**。

---


## 2026-11-20 16:35 — 固件版本号可填 + Boot 侧降级拦截

### 为什么必须做这条

版本号的数据通路其实一直是通的（FWUP 头 `fw_ver` → 槽头 → 参数页 `version`），
缺的是两头：上位机永远发 `ver=1`，Boot 搬之前不做比较。结果是**任何版本都能覆盖任何版本**。

CRC 顶替不了这个角色：同一份源码重编一次，`__DATE__/__TIME__` 变了 CRC 就变，
Boot 无法从中分辨"只是重编"和"这是另一个固件"。版本号是唯一能表达新旧关系的字段。

### Boot 侧

`ota_apply.h` 新增编译期开关：

```c
#ifndef OTA_ALLOW_DOWNGRADE
#define OTA_ALLOW_DOWNGRADE     0     /* 0 = 拒绝降级 */
#endif
```

`ota_apply.c` 的 `OtaApply_FromSlot()` 加拦截，位置在两处关键点之间：

- **在"内部 Flash 已是这份固件"快路径之后**：重发当前版本（产线复检、现场刷回同一版）
  不擦不写、没有风险，不该被版本号挡住
- **在 `BackupOldFirmware()` 之前**：被拒绝的固件不该顺手把槽 B 的回滚源覆盖掉

比较对象是**参数页里的版本**（板子上真正在跑的那一版），不是槽 B 头里的版本 ——
后者是老版本，拿它比会把正常升级误判成降级。参数页为空、版本字段异常（≥0x10000）时不拦：
没有基准就无从比较，宁可放行，也不要把板子卡成"永远刷不进去"。

### 顺带修掉一个同步漏洞

`OtaApply_FromSlot()` 的快路径（内容一致、只补标记）原来只在"长度或 CRC 不符"时写参数页，
版本号从 1 改成 2 时不写 —— 参数页成了降级的错误基准（板子实际 2、账本记 1，下次发 1 就给放）。
现在补成：内容相同但版本不同时，**保留原 state/cnt** 只更新 version。
保留的理由是内容没换，没理由把已确认的固件打回试用白跑一轮。

### 上位机

- `ota_headless.py`：新增 `--ver`（1..65535，默认 1），非法值直接退出；
  收尾判定识别 `拒绝降级` 并打印明确解释
- `serial_assist.py`：新架构勾选框下方新增「固件版本（升序，降级会被拒绝）」Spinbox；
  版本号在主线程读出后传进发送线程（Tk 变量不是线程安全对象），非法输入回落 1 并提示；
  存进配置 `fw_ver`。后续控件行号整体下移一位
- exe 已重新打包（`dist_new/STM32_OTA_SerialAssist.exe`）

### 验证

版本 2 真搬运：

```
[slot] A：正常 版本=2 长度=18668 CRC=0x72705E04
[apply] 槽 A -> 0x08008000，长度=18668 CRC=0x72705E04
[apply] 完成 18668B，参数页已更新
[trial] 等 App 确认 1/3
```

内容不同、版本 1（把 bin 末字节翻转，CRC 0x5F72B189）：

```
[slot] A：正常 版本=1 长度=18668 CRC=0x5F72B189
[apply] 槽 A：版本 1 低于当前 2，拒绝降级
[apply] 如需强制降级，请改用槽 B 回滚或重编 Boot（OTA_ALLOW_DOWNGRADE=1）
[boot] 参数：App=18668B CRC=0x72705E04
[boot] CRC 校验通过
```

板子没有搬运、继续跑当前好固件。之后重发 ver=2 把槽 A 恢复成当前版本。

**副产物（意外但重要）**：测试中一次发送撞进 Boot 的 3 秒 OTA 等待窗口，固件被当成
老协议流写进内部 Flash，把 App 区写坏（`App 无效：向量表不对`）。Boot 自动用槽 B 自救：

```
[boot] 内部 Flash 校验不过，用槽 B 自救
[trial] 回滚：槽 B -> 0x08008000，长度=18668 CRC=0xDF6055AD
```

板子当场恢复，无需接线。同时暴露一个操作注意点：**SWD 复位后 3 秒内不要发新架构固件**，
否则会被 Boot 的老协议接走。

体积：Boot 22036 → 22352 B，32KB 占 68.2%。

---

## 2026-11-20 16:20 — 参数页丢失自愈（Boot）

### 现象

升级成功后 4 分钟按复位，Boot 报 `[boot] 无参数页，App 未经验证`，退回兼容模式。
SWD 直接 dump `0x0807F800` 确认 256 字节全 `0xFF`，参数页是被**擦掉**的，不是判定错误。

排查结论：工程内没有任何擦参数页的路径 —— `Ota_Param_Erase()` 只有定义、无调用；
App 侧完全没有内部 Flash 写操作；`Application/.eide/eide.yml` 的下载配置 `baseAddr=0x08008000`
只覆盖 App 区，写 18668B 只涉及页 16~25，碰不到页 255 的参数页。因此判定为外部动作
（IDE 的"擦除芯片"、其它烧写工具或手工 openocd 擦除）导致。

### 设计判断

参数页只是"记账本"，真相在别处且天然双份：W25Q 槽头里的 len/crc（A、B 各一份），
以及内部 Flash 里那段字节本身。账本丢了不该让 CRC 校验、试用计数、回滚一起静默失效 ——
表面上"板子还能跑"，实际已经退化成盲跳。

### 改动

`Boot_Loader/Application/BSP/OTA/ota_apply.c` 新增 `OtaApply_RecoverParam()`：

1. 只在 `Ota_Param_Load()` 返回 `OTA_PARM_EMPTY`（整页全 FF）时动手。
   有内容却校验不过 = 写到一半断电，那种情况不能猜，留给 `CheckAppBoot` 报损坏停在 OTA。
2. 依次读槽 A / 槽 B 头，跳过自测假头与长度越界，要求 `Ota_VerifyFlash(APP_ADDR, len, crc) == 0`
   —— 槽头记的 CRC 必须和内部 Flash 实测 CRC 相等，才说明"板子上跑的就是这一份"。
3. 命中后 `Ota_Param_SaveEx(..., TRIAL, 0)` 重建，**一律试用态、计数 0**，绝不直接写已确认：
   此刻只能证明内容一致，证明不了跑得起来。代价只是 App 正常启动后 5 秒自确认，
   真是坏固件则三次后照样回滚。
4. 两个槽都对不上 → 不重建，保持兼容模式（宁可退化，也不拿错账本跳过校验）。

`Boot_Loader/Core/Src/main.c` 在 `OtaApply_FromSlot()` 之后调用它。放这个位置的理由：
刚搬完的固件已经写好参数页，自愈不会误动；只有"没搬东西且账本空白"时才触发。

### 验证

真机复现：擦掉参数页 → 复位，实测日志

```
[slot] A：正常 版本=1 长度=18668 CRC=0x72705E04
[param] 参数页丢失，已按槽 A 重建为试用态：长度=18668 CRC=0x72705E04
[trial] App 已确认 长度=18668，试用转正
[boot] CRC 校验通过
[boot] 跳转到 App 0x08008000
```

注意这里直接转正、没走"等 App 确认 1/3"：W25Q 里的确认标记（及其 len/crc）与账本是两个独立的真相源，
参数页被擦不影响标记，所以重建后标记立刻成立。这正是设计意图 —— 证据越多，恢复越快。

烧录后参数页由全 `0xFF` 恢复为：
`magic=0x4F544150 length=18668 crc=0x72705E04 ver=1 state=0 cnt=0`

体积：Boot 21724 → 22036 B，32KB 占 67.2%。

---

## 2026-11-20 16:05 — 串口日志全中文化 + App 独立看门狗 + 实证确认

### 一、日志中文化

App 侧 35 处、Boot 侧 45 处打印全部改为中文。保留 `[boot] / [slot] / [apply] / [trial] / [app-ota]` 前缀不动
（`serial_assist.py` 的按前缀着色依赖它）。格式符与字段顺序不变，只改文案。

上位机同步修正（`Tools/SerialAssist/ota_headless.py`），否则自动判定会全部失效：

| 原关键字 | 新关键字 |
|---|---|
| `header written` | `头部已写入` |
| `[slot] A:` | `[slot] A：` |
| `jump to app` | `跳转到 App` |
| `[app] build` | `编译时间` |
| `[ota] ready` | `[ota] 就绪` |
| `verify OK` | `校验通过` |
| `reset to bootloader` | `复位回 BootLoader` |

代价：中文 UTF-8 每字 3 字节。App 18180→18668 B（+488），Boot 21564→21724 B（+160）。

### 二、App 独立看门狗（新增 `Application/BSP/WDG/`）

寄存器直写，不引入 HAL 驱动——本工程 `Drivers/` 是裁剪过的，没有 `stm32f1xx_hal_iwdg.c`。
配置 `/64` 分频 + `RLR=1250` ≈ 2.0 s。

**踩坑（本次唯一一处死机）**：先写 `PR/RLR` 再写 `0xCCCC` 启动 → 卡死在等 `PVU/RVU` 的 `while` 里。
原因：手动开 LSI 并等 `LSIRDY` 只证明 LSI 振荡了，并没有把时钟路由给 IWDG；此时写 `PR/RLR`
会让 `SR` 的 PVU/RVU 置位却永远完不成。现象：日志停在 W25Q 自检最后一行，之后一片空白，
而且永不复位（狗根本没启动）。靠 SWD 读 PC = `0x08008f08` 定位。
正确顺序：**`0xCCCC` → `0x5555` → `PR/RLR` → 等 PVU/RVU → `0xAAAA`**，与 HAL 一致。

### 三、试用确认从"乐观"改为"实证"

`AppOta_Confirm()` 从初始化末尾挪进主循环：跑满 5 s 且期间一直在喂狗才确认。
失败每 1 s 重试，最多 5 次，之后放弃但继续运行（保持 TRIAL，不自殺）。

配套补丁：`app_ota.c::FinishRx()` 在 CRC 通过后补一次确认。否则"升级后 5 s 内又发一版"
会带着 TRIAL 复位，已经被证明健康的这一版被记一次失败，连刷三次就莫名回滚。
依据：能收完整包 + CRC 逐位相同，证据强于空跑 5 s。

### 四、修复 `OtaApply_FromSlot` 的"只补标记"分支

该分支在内部 Flash 已是目标固件时直接返回，**从不写参数页**。当参数页为空
（典型场景：SWD 烧过 App 再从串口发同一版）时，参数页永远建不起来，Boot 每次上电
都报"无参数页"、只能兼容模式盲跳，试用/回滚机制等于不存在。
现改为：参数页缺失或与目标固件不符时补写为 TRIAL；已匹配则一字不动
（避免重发同版把 CONFIRMED 打回 TRIAL）。

### 上板验证

```
[apply] 参数页缺失或不符，已补写为试用态
[trial] 等 App 确认 1/3
[app] 看门狗 2000ms，确认门限 5000ms
[app-ota] 监听中，槽 A 位于 0x00001000
[app-ota] 自确认 长度=18668 CRC=0xDF6055AD      ← 出现在监听之后 5 s
```

看门狗 + 回滚链（发一个"主循环 HAL_Delay(10000) 不喂狗"的固件）：

```
[app] 看门狗 2000ms ...  → [rst] IWDG / 独立看门狗复位
[trial] 等 App 确认 2/3  → 再死 → [rst] IWDG
[trial] 连续 3 次没等到确认，开始回滚
[trial] 回滚：槽 B -> 0x08008000，长度=18668 CRC=0xDF6055AD
[trial] 回滚完成，已退回上一版   → [app] 编译时间 15:55:42（好固件）
```

---

## 2026-11-20 15:47 — 阶段 5 验收：App 在线升级全链路首次闭环

代码无改动，这是「下载区与执行区分离」架构的一次完整端到端验收。

### 链路

上位机（串口助手勾「新架构 FWUP」）→ App 常驻监听收 18180 B 写槽 A →
App 写完头后 `NVIC_SystemReset()` → Boot 上电读槽 A 校验 → 搬进
`0x08008000` → 写参数页并置 `TRIAL` 1/3 → 跳转 →
App 跑完初始化 `AppOta_Confirm()` → Boot 下次上电转 `CONFIRMED` →
App 回到监听态。

### 关键日志

```
[app-ota] recv ver=1 len=18180B (5 sectors)
[app-ota] done 18180B crc ok, header written
[slot] A: ok ver=1 len=18180 crc=0xCD46F111
[apply] slot A -> 0x08008000, len=18180 crc=0xCD46F111
[trial] 等 App 确认 1/3
[app] build Nov 20 2026 15:44:43
[app-ota] self-confirm len=18180 crc=0xCD46F111
```

### 说明：本次日志没有备份打印，不是漏了

`BackupOldFirmware()` 第 66 行有一个幂等短路：槽 B 头部已经是
「参数页里那一版 + BACKUP 标志」时直接 return，不擦不写也不打印。
本次参数页是 `18180/0xB3081AAF`、槽 B 也是同一份，所以静默跳过——
省掉 5 个扇区擦除，符合设计，不是 bug。

### 遗留隐患（下一步）

`AppOta_Confirm()` 目前在初始化末尾**立即**调用，属于"乐观确认"：
固件只要能跑完初始化就被转正，之后主循环里死机也不会回滚。
要让它变成"实证确认"，必须和看门狗一起改——见下一条建议。

---

## 2026-11-20 15:52 — 阶段 4 补：搬运途中断电自动自救

### 问题

阶段 4 的备份机制只解决了"新固件跑不起来"，没解决"搬运到一半断电"：
参数页还是旧的（它要等校验通过才更新），CheckAppBoot 必然失败，
Boot 停在串口等重传 —— 槽 B 里明明有一份完好的上一版，却没人去看。

### 改动

- `Boot_Loader/Core/Src/main.c` — `CheckAppBoot()` 返回 0（真的坏了）时，
  若槽 B 头部有效，直接调 `OtaApply_RestoreFromB()` 搬回来，再判一次。
  只在 `boot_st == 0` 时触发，老固件走的 `2`（能用只是没参数）不受影响。

### 上板验证

用 SWD 擦掉内部 Flash 第 24 页（0x0800C000，App 中间一页）模拟"搬到一半断电"：

```
[boot] crc mismatch, stay in ota
[boot] 内部 Flash 校验不过，用槽 B 自救
[trial] 回滚：槽 B -> 0x08008000, len=18180 crc=0xB3081AAF
--- 复位后 ---
[boot] crc ok
[boot] jump to app 0x08008000
[app] build Nov 20 2026 15:39:04
```

不需要串口、不需要人，设备自己回到上一版。

### 踩到的坑（给以后省时间）

`openocd flash erase_sector 0 8 8` 里的索引是 **2KB 页号**，不是"第 8 个扇区"。
0x08004000 落在 Boot 自己身上（Boot 24KB），擦掉后 MCU 直接 HardFault、
串口一行日志都没有，看起来像"新代码把 Boot 写崩了"，实际是自己擦的。
App 区首页 = 页 16（0x08008000），页 24 = 0x0800C000。

---

## 2026-11-20 15:45 — 阶段 4：槽 B 备份 + 试用计数 + 自动回滚

### 问题

阶段 3 之后还剩两个洞：

1. **搬运途中断电**：旧固件已被擦、新固件只写了一半，参数页还没更新 →
   Boot 校验不过 → 停在串口等重传。不跑飞，但必须有一台连着串口的电脑才能救。
2. **新固件本身跑不起来**：CRC 完全正确、搬运完全成功，但 App 一启动就
   HardFault。Boot 每次上电都照样跳过去，现场表现为"一上电就重启"，
   而且没有回滚路径。

### 改动

- `ota_slot.h` — 新增 `OTA_CONFIRM_BASE`(0x100000) + `OtaConfirm_t`(16B) +
  `OtaSlot_ReadConfirm/WriteConfirm`；新增 `OTA_SLOT_FLAG_BACKUP`。
- `ota_slot.c` — 实现确认标记读/写（擦扇区 → 写 16B，自带 CRC 防写一半）。
- `ota_param.h/.c` — `reserved` 拆成「状态(bit0-3) + 试用计数(bit8-15)」；
  新增 `Ota_Param_State/TrialCnt/SaveEx/SetState`，`Ota_Param_Save` 改为
  委托 `SaveEx(..., CONFIRMED, 0)`，老路径行为不变。老参数 `reserved=0xFFFFFFFF`
  读出 state=0xF，按"已确认"处理，升级后不会误回滚。
- `ota_apply.c` — 新增 `BackupOldFirmware()`（覆盖内部 Flash 前把现有固件
  存进槽 B，带回读校验）与 `OtaApply_RestoreFromB()`（回滚）；搬运成功后
  参数写 `state=TRIAL,cnt=0`。
- `Boot_Loader/Core/Src/main.c` — 新增 `TrialGuard()`，位置在
  `OtaApply_FromSlot` 之后、`CheckAppBoot` 之前；返回 1 时跳过跳转。
- `Application/.../OTA/app_ota.c/.h` — 新增 `AppOta_Confirm()`：按内存映射
  只读参数页，向 W25Q 确认扇区写一次标记，不碰内部 Flash。
- `Application/Core/Src/main.c` — 初始化末尾调用 `AppOta_Confirm()`。
- App 侧新增 `ota_param.h` / `flash_app.h` 两份**头文件**副本（不复制 .c，
  App 因此没有擦写参数页的能力，编译期也调不到）。

### 上板验证（真机）

```
[trial] 旧固件已备份到槽 B: len=17756 crc=0xFA8C7D74
[apply] slot A -> 0x08008000, len=18180 crc=0xDA69F834
[trial] 等 App 确认 1/3
[app-ota] self-confirm len=18180 crc=0xDA69F834
--- 下次上电 ---
[trial] App 已确认 len=18180，试用转正
```

回滚路径用"跑得起来但永不确认"的固件验证（编译期把调用注掉）：
```
[trial] 等 App 确认 2/3
[trial] 连续 3 次没等到确认，开始回滚
[trial] 回滚：槽 B -> 0x08008000, len=18180 crc=0xB3081AAF
[trial] 回滚完成，已退回上一版
[app] build Nov 20 2026 15:39:04      ← 退回上一版
```

### 踩到的坑

`reserved` 基底写成 `0xFFFFFF00`，把计数位（bit8-15）也预置成 1 →
读出来 cnt=255，加一次回绕成 0，永远到不了上限。日志表现是
`[trial] 等 App 确认 0/3`。改成 `0xFFFF0000` 后正常。

### 体积

Boot 23988+92 = 24,080 B / 32KB（73.5%），bss 8112。
App 18,180 B（带确认）/ 17,756 B（测试版不含确认）。

---

## 2026-11-20 15:30 — 串口助手：「新架构」勾选框挪到发送区 + 重打 exe

### 现象

用户找不到「新架构」勾选框。原因是 `dist/STM32_OTA_SerialAssist.exe`
是 13:37 的构建，而勾选框是 15:05 才加进 `serial_assist.py` 的 ——
源码里有、exe 里没有，窗口自然看不到。**源码改了就必须重新打包**，
否则跑的永远是旧逻辑。

### 改了什么（`Tools/SerialAssist/serial_assist.py`）

- 勾选框从卡片最底部（SWD 复位按钮下方的说明行之后）挪到
  「发送前调试器复位（抢 OTA 窗口）」紧下面。两者是互斥的两条发送路径，
  放一起才不容易漏。
- 新增 `_sync_send_mode()`：勾上「新架构」时把「发送前复位」置灰，
  取消勾选后恢复。只是让界面说真话 —— 新架构的发送函数根本不读
  `auto_reset`，留着可勾会让人以为复位仍起作用。
- 行号顺移：发送按钮 5→6、进度文字 6→7、SWD 复位 7→8、调试器提示 8→9。

### 验证

用脚本构造 `OtaSerialApp` 量 `winfo_ismapped` 与 `winfo_rooty`：
两个勾选框都 mapped，y=419 / 442，状态联动 normal↔disabled 正确。
重打包后 exe 启动能正常出窗口（pid 有 MainWindowHandle）。

---

## 2026-11-20 15:30 — 阶段 3：Boot 把槽 A 搬进内部 Flash

### 做了什么

补上整条升级链的最后一环。此前 App 能把固件写进 W25Q 槽 A 并复位，
但 Boot 只会把它打印出来就跳回旧 App —— 槽里那份固件永远是"躺着不执行"。

现在 Boot 上电后会：读槽头 → 判断该不该装 → 逐页搬进内部 App 区 →
回读 CRC → 更新参数页 → 给槽头打 `COPIED` → 走原有跳转流程。

### 改动

- `Boot_Loader/Application/BSP/OTA/ota_apply.h/.c` — **新增**
  · `OtaApply_FromSlot()`：搬运主流程，返回 OK / ALREADY / NOOP / REJECT / ERR
  · 边界检查：`FLAG_SELFTEST` 假头直接拒（否则会把 16 字节垃圾搬进去跑飞）、
    `fw_len > OTA_APP_MAX` 直接拒（否则会擦到参数页，连"原来多长"都丢失）
  · 每 2KB 一页：读 W25Q → 擦内部页 → 写内部页，2KB 缓冲在 `.bss`
  · 搬完用 `Ota_VerifyFlash()` 把内部 Flash **读回来**重算 CRC，
    这是唯一能证明"写进去的东西是对的"的手段（边搬边算证明不了）
  · 失败一律 `OtaSlot_EraseHeader()`：否则每次上电都重搬一遍、再失败一遍，
    启动时间被白白吃掉，日志看起来像"每次都卡在同一个地方"
- `Boot_Loader/Core/Src/main.c` — 在"上电窗口"之前调用 `OtaApply_FromSlot(A)`。
  必须在 `CheckAppBoot()` 之前：搬运完参数页才是最新的，判断才不会慢一拍
- `Boot_Loader/Application/App/Boot_Loader/Boot_Loader.c::Boot_Loader_JumpToApp`
  — 跳转前补 `HAL_SPI_DeInit(&hspi2)`。阶段 3 起 Boot 自己会用 SPI2，
  不 DeInit 就跳，App 重新 `MX_SPI2_Init()` 相当于在活着的 SPI 上改配置

### 顺序上的两个刻意选择

1. **先写参数页，再打 COPIED**。反过来的话，若参数页写失败而槽头已标"搬过"，
   下次上电跳过搬运、参数页却还是旧值 → 固件明明是好的，板子却停在 OTA
2. **改 `flags` 必须用 `OtaSlot_BuildHeader()` 重建头部**。`hdr_crc` 覆盖的
   24 字节里包含 `flags`，直接改字段会让 CRC 过期，`OtaSlot_WriteHeader()`
   的 RAM 自检会拒绝写入。这个坑在真机上踩到了，症状是一句
   `[apply] 警告：槽头标记写入失败` —— 数据搬对了，但每次上电都要重新校验一遍

### 真机验证（三条路径都走通）

```
[app-ota] done 17756B crc ok, header written      ← App 写槽并复位
[slot] A: ok ver=1 len=17756 crc=0xFA8C7D74       ← Boot 读出槽头
[apply] slot A -> 0x08008000, len=17756 crc=0xFA8C7D74
[apply] done 17756B, 参数页已更新
[boot] param: app=17756B crc=0xFA8C7D74           ← 参数页被搬运流程改写
[boot] crc ok
[boot] jump to app 0x08008000
[app] build Nov 20 2026 15:22:55                  ← 跑起来的是新固件
```

- 幂等：再复位一次，`[apply]` 整行消失（`COPIED` 生效，不再重搬）
- 已安装判定：槽内 CRC 与内部 Flash 相同时走"只补标记"，不重复擦写
- Boot 体积 21876 B / 32KB（66.8%），RAM 7856 B；App 17756 B

### 新增工具能力

- `Tools/SerialAssist/ota_headless.py` 增加 `flash-boot`（SWD 烧 Boot）
  与 `watch`（复位并看满上电日志，静默阈值 5s 以跨过 3s 上电窗口）
- `Application/Core/Src/main.c` 启动时打印 `[app] build __DATE__ __TIME__`：
  OTA 系统里"现在跑的到底是哪一版"必须有一行能直接看出来

### 已知残余风险（阶段 4 要解决）

搬运期间旧 App 已被擦掉，此刻断电 → 内部 Flash 是残缺固件。因为参数页
要等校验通过才更新，Boot 下次上电不会跳转，会停在串口等重传——**不会跑飞，
但需要一台连着串口的电脑才能救**。真正的解法是 A/B 双分区，
或阶段 4 的槽 B 回滚。

---

## 2026-11-20 15:12 — 阶段 2 上板验证通过 + 无界面调试发送端

### 验证结果（真机，COM25 @ 115200）

先用老协议把修复版 App 灌进去（旧 App 还不会收新协议），再用新协议发一次：

```
[app-ota] listening, slot A at 0x00001000
[app-ota] recv ver=1 len=17708B (5 sectors)
[app-ota] done 17708B crc ok, header written
[app-ota] reset to bootloader ...
[boot] w25q id=0xEF4018 OK
[slot] A: ok ver=1 len=17708 crc=0x3705B3E2
[boot] crc ok
```

`crc=0x3705B3E2` 与上位机算出的固件 CRC 逐位一致，**阶段 2 闭环成立**：
App 把 17KB 完整写进 W25Q 槽 A、留下了自洽的头部、并自己复位回 Boot。
顺带证明惰性擦除有效 —— 5 个扇区边收边擦，全程没有出现 `crc mismatch`。

### 新增

- `Tools/SerialAssist/ota_headless.py` — **无界面 OTA 发送端**，两个子命令：
  · `boot-ota <bin>` 老协议（SWD 复位 → 裸字节 → OTAE 尾），给 BootLoader
  · `app-ota <bin>` 新协议（FWUP 头 → 等 300ms → 裸字节），给运行中的 App
  可在命令行/CI 里跑，不用开 GUI

### 改动

- `serial_assist.py` — 把协议打包抽成模块级 `build_fwup_header()` /
  `build_ota_tail()`，GUI 与 `ota_headless.py` 共用同一份字节定义。
  在这之前，头部 pack 是内联在 `_tx_worker_fwup` 里的，谁想写个脚本调试
  都会再抄一份 —— 一旦两边漂移，症状是"发送说成功、板子报 crc mismatch"，
  不报任何错，极难排查。已验证重构前后字节完全一致（含 32 字节头与 12 字节尾）

### 注意

- `Application/build/Debug/Application.bin` 已被替换成**修复版**（17708 B，
  crc=0x3705B3E2）。原来那份是 14:57 的带 bug 构建，留着会让人再次发出有问题的固件
- 槽 B 仍是 `bad`（残留的 `1B FF F0 6B` 测试图案），阶段 3 写槽 B 时才清

---

## 2026-11-20 15:05 — 修 App 接收状态机三个上板才暴露的缺陷

### 现象

上板实测，App 打印 `listening` 后发固件，一个字节都没收就连续两次 abort：

```
[app-ota] listening, slot A at 0x00001000
--- 开始发送 Application.bin （17708 B，块 256，延时 1.0ms）---
[app-ota] abort: timeout
[app-ota] abort: hdr ver
```

### 根因（三处，前两处是连锁的）

1. **`AppOta_Poll` 的超时判定用了过期时间戳**。刷新 `last_act` 被写在
   `if(s_rx.state != ST_IDLE)` 里面，而状态是从 IDLE 内部（`FeedByte` 扫到
   `FWUP`）切过去的——那一次 Poll 顶部 state 还是 IDLE，时间戳不刷新。
   紧接着下一轮若恰好没数据，`Timer_Timeout` 拿的是「上次有数据的时刻」，
   可能是十几秒前，于是刚进 HEADER 就判超时。**固件一个字节都没收就 aborted。**

2. **`abort: hdr ver` 是第一处的连锁反应**。放弃接收后状态回到 IDLE，
   但剩余 17KB 数据仍在流里被扫描；而固件镜像本身包含 `"FWUP"` 这 4 个
   ASCII 字节（就是 `memcmp(s_rx.win, "FWUP", 4U)` 这个常量）。扫到它就能
   拼出一个 magic 正确的假头部，于是报出看起来很吓人的 `hdr ver`。
   真实故障只有一个，日志却给了两个不相关的方向。

3. **一次性擦除会撑爆串口缓冲**（修好前两处后必然撞上）。`StartData` 原来
   在开收前把整段固件区的扇区全擦掉：17KB = 5 扇区 × 45ms = 225ms，
   115200 下这段时间进来约 2600 字节，而 DMA 环形缓冲只有 1024 字节
   → 溢出丢包 → 最后 `crc mismatch`。476KB 固件更是要连续阻塞十几秒。

### 改动

- `app_ota.c::AppOta_Poll` — 只要本轮取到字节就无条件刷新 `last_act`，
  不再区分状态；超时只在 `state != ST_IDLE` 且本轮无数据时判定
- `app_ota.c::FlushPage` — 改为**惰性擦除**：写之前才擦「这一页要落进去的
  那个扇区」，新增 `erased_end` 记录擦到哪。单次阻塞从 225ms 降到 45ms
- `app_ota.c::FeedByte` — 头部校验顺序改为 `magic → hdr_crc → hdr_ver →
  长度 → flags`。先算 CRC 才能把误判准确归类为 `hdr crc`，而不是
  误导性的 `hdr ver`；CRC 只有 24 字节计算量，比误判后重走流程便宜
- `Tools/SerialAssist/serial_assist.py::_tx_worker_fwup` — 发完 32 字节头
  后等 300ms 再发数据，让 App 先把头部扇区擦完，缓冲从空开始

### 验证

App 全量编译链接零警告，text 15372 / data 92 / bss 3620。

---

## 2026-11-20 14:50 — 阶段 2：App 一直监听，收到就写槽 A

### 问题

Boot 收固件的模式是"上电抢 3 秒窗口"，现场没有电脑就永远升不了级，
且上电那一刻就已经擦掉了旧固件。改成由 App 常驻接收后，下载全程不碰内部
Flash，旧固件始终完好——这是架构级的安全，不是"把窗口缩小"的优化。

### 改动

- `Application/Application/BSP/OTA/app_ota.h/.c` — **新增**。三态状态机
  （IDLE → HEADER → DATA）：
  · IDLE 用 4 字节滑动窗口找 `'F','W','U','P'`（标识可能被拆在两个读周期里）
  · HEADER 收满 32 字节才解析，校验 magic / hdr_ver / hdr_crc / 长度范围 /
    拒绝 `FLAG_SELFTEST`
  · DATA 以 256 字节页攒够再写 W25Q，CRC 在「写成功之后」才累计
  · 收满后 `~crc` 与头部 `fw_crc` 比对，一致才 `OtaSlot_WriteHeader()` 并
    `NVIC_SystemReset()`，让 Boot 去搬运
  · 3 秒静默自动放弃（线拔了/上位机崩了不会一直往 Flash 里灌业务字节）
- `Application/Core/Src/main.c::AppOta_Poll` 调用点 — 主循环由 `HAL_Delay(500)`
  阻塞式闪灯改为 `Board_Millis()` 计时翻转，否则 500ms 内串口数据全堆在
  1024 字节环形缓冲里，超过就丢包。
- `Application/Core/Src/main.c` — 关闭 `OtaSlot_SelfTest()` 调用（阶段 1 已通过），
  函数本体加 `__attribute__((unused))` 保留，作为改布局后的回归测试。
- `Tools/SerialAssist/serial_assist.py` — 新增 `_tx_worker_fwup()` 与
  「新架构：直接发给 App（FWUP 头，免复位）」勾选框（`fwup_mode`，默认关）。
  与老路径的四点区别：不复位、先发 32 字节头、不发 OTAE 尾部、发完不复位。
- `Boot_Loader/Application/BSP/OTA/ota_slot.h/.c` — **修注释错误**：
  `hdr_crc` 覆盖范围是 **24** 字节（到 `hdr_crc` 之前的偏移），不是 28。
  代码本来用 `offsetof` 算，行为正确，只是注释会误导人。两份副本已同步。

### 注意事项

- 上位机头部字节序与 `OtaSlotHeader_t` 必须逐字节一致，已用
  `struct.unpack("<IHHIIIIII")` 回读验证：magic=0x50555746、32 字节、CRC 复算 PASS。
- App 侧擦除顺序是**先擦头、再擦数据**。反了会出现"数据已擦、旧头还在"的窗口，
  此刻断电会让 Boot 搬一车 0xFF 进内部 Flash。
- 体积：App 14756 → 15380 B。零警告。

---

## 2026-11-20 14:48 — 槽 BAD 时打印头部原始字节

### 问题

上板第一次跑阶段 1，两个槽都报 `bad`。判 BAD 只说明「这 32 字节不构成合法头部」，
但没说它是空白以外的什么：可能是 MISO 恒低读回全 0、可能是这个槽以前被别的程序
用过、也可能是两份 `ota_slot.h` 布局不一致。日志信息量不足以区分，只能靠猜。

其中槽 A 的 BAD 已确认原因：刷的还是旧 App（打印 `erase sector 0`），
老自测在地址 0 写了 `A0..AF`。槽 B（0x080000）没有解释，需要看原始内容。

### 改动

- `Boot_Loader/Core/Src/main.c::main` — `OTA_SLOT_BAD` 分支增加 32 字节原始头部
  dump，16 字节一行。为什么不用一次打完：`Uart_Printf` 内部缓冲 100 字节，
  一行 32 个 `%02X` 加空格会超长被 vsnprintf 截断，正好截掉最需要看的尾部。

### 判读方法

| dump 内容 | 含义 |
|---|---|
| 全 `00` | MISO 一直低，SPI 没读到数据 |
| 有数据但不是头部 | 该槽被别的程序用过，正式使用前要擦 |
| 像头部但 CRC 不符 | 布局不一致，或写头部时断了电 |

---

## 2026-11-20 13:40 — 阶段 1：双槽布局 + 头部读写（ota_slot）

### 问题

App 下载固件必须先落到 W25Q 里"暂存"，Boot 复位后再搬进内部 Flash。
这要求两边对「固件放在哪、怎么判断这份东西可信」有完全一致的理解。
没有这一层时，地址和校验规则会散落在 App 的接收代码和 Boot 的搬运代码里，
改一处忘一处，症状是"CRC 对不上"，但根本说不清是哪一头算错了。

另外发现一个**会静默毁掉升级流程**的问题：App 的 `W25q_SelfTest()` 每次上电
都 `W25q_EraseSector(0)` —— 地址 0 正是槽 A 的头部扇区。也就是说每开机一次，
刚下载好的升级就被悄悄取消，再往后写还会把头部搞成 BAD。

### 改动

- `Boot_Loader/Application/BSP/OTA/ota_slot.h` — **新增**。定义 W25Q 上双槽布局
  （A: 0x000000 / B: 0x080000，每槽 512KB = 头 4KB + 数据 508KB）、
  32 字节头部结构 `OtaSlotHeader_t`、状态码与 flags 位。
  头部**独占一个扇区**，理由写在文件头注释里：让"写头"和"写固件"物理隔离，
  避免"头说 100KB、实际只有 60KB"这种自相矛盾的中间态。
- `Boot_Loader/Application/BSP/OTA/ota_slot.c` — **新增**。实现
  `OtaSlot_ReadHeader/WriteHeader/EraseHeader/BuildHeader/VerifyData`。
  校验四项：magic、结构版本、头 CRC（覆盖前 28 字节）、长度落在 (0, 508KB]。
  `VerifyData` 分块读 W25Q 重算 CRC，证明"落盘的就是收到的"。
- `Boot_Loader/Core/Src/main.c::main` — USER CODE 2 增加槽状态打印
  （`[slot] A: ok/empty/bad`），只读不写，作为阶段 1 的验收信号；
  `W25q_Init()` 实参改用 `W25Q128_CS_GPIO_Port/Pin` 宏。
- `Application/Application/BSP/CRC/crc32.c/.h` — **新增**（Boot 侧副本复制过来，
  App 原先没有 CRC 模块）。
- `Application/Application/BSP/OTA/ota_slot.c/.h` — **新增**（与 Boot 侧逐字一致）。
- `Application/Core/Src/main.c::W25q_SelfTest` — 擦写目标由 `0` 改为
  `OTA_SCRATCH_BASE`（0x00FFF000，W25Q 最后一个扇区），不再碰槽 A 头部；
  `W25q_Init()` 同样改用引脚宏。
- `Application/Core/Src/main.c::OtaSlot_SelfTest` — **新增**。往槽 A 写
  16 字节假固件 + 头部（`fw_ver=3`、`flags=OTA_SLOT_FLAG_SELFTEST`），
  写完立刻读回并重算 CRC，验证 App 侧闭环。

### 注意事项

- 两份 `ota_slot.c/h`、`crc32.c/h` 是**物理副本**，改一边必须同步另一边。
- 槽 A 里现有 `FLAG_SELFTEST` 的假头。阶段 3 的搬运逻辑**必须跳过带该标志的槽**，
  否则会把 16 字节假数据当固件搬进内部 Flash。
- 体积：Boot 16652 → 17152 B / 32KB（+500）；App 链接后 14756 B。均零警告。

---

## 2026-11-20 13:33 — Boot 片选改用 CubeMX 生成的引脚宏

### 问题

`.ioc` 里新增了 `PB12 = GPIO_Output`（Label `W25Q128_CS`，上拉、高速、初值高），
引脚配置从此由 `gpio.c` 统一管理，`main.h` 也生成了 `W25Q128_CS_GPIO_Port/Pin`。
但 `main.c` 自检处仍写死 `GPIOB, GPIO_PIN_12`，一旦以后在 CubeMX 里把片选换到别的脚，
`.ioc`/`gpio.c`/`main.h` 都跟着变，唯独这一行不变 —— 表现为 SPI 读到的 ID 恒为
`0xFFFFFF`，且编译期毫无提示。

### 改动

- `Boot_Loader/Core/Src/main.c::main` — `W25q_Init()` 的实参由
  `GPIOB, GPIO_PIN_12` 换成 `W25Q128_CS_GPIO_Port, W25Q128_CS_Pin`。
  副作用：无。宏展开后与原来完全等价，Flash 16616 → 16652 B（+36，属对齐差异）。
  注意：`main.h` 已由生成区包含，不要为此再手动 `#include "main.h"`。

- 未改 `w25q.c`：其中 `W25q_Init()` 仍会自行 `HAL_GPIO_Init(PP/PULLUP/HIGH)`，
  与 `gpio.c` 的配置**逐字相同**。保留是为了驱动自包含；但这意味着
  **以后在 CubeMX 里改 PB12 的 Pull/Speed 不会真正生效**（会被驱动覆盖回上拉+高速）。

---

## 2026-11-20 13:36 — 修正：清空接收只清界面，不碰串口缓冲

### 问题

上一条把需求理解成了"清空缓冲区"，实现里调了 `reset_input_buffer()`
并排空了待渲染队列 —— 那等于把板子刚发来、还没上屏的字节扔掉。
实际需求是**清接收区的显示**：串口照收、解析照跑，只把这一屏字擦掉。
（撤掉的东西不影响任何已落盘的数据，只是显示层行为。）

### 改动

- `Tools/SerialAssist/serial_assist.py::OtaSerialApp.clear_buffer` — **删除**。
  它做的事（purge 驱动层、排空队列、重置解析中间态、RX 归零）全部不再需要。
- `::clear_rx` — 恢复为唯一的清空入口，docstring 里写清边界：只清 Text
  内容 + HEX 尾巴 + 偏移，`in_waiting` / `rx_queue` / `line_buf` /
  增量解码器 / RX·TX 计数一律不动。副作用单独注明：HEX 偏移会归零。
- `::_reader_loop` — 撤掉 `rx_epoch` 代号闸，reader 不再丢弃任何数据
  （它原本的作用是丢掉"清空瞬间正在 read() 的那一块"，属于清缓冲语义）。
- `::__init__` — 移除 `rx_epoch` 字段，保留一行注释记录它为何被撤，
  免得以后有人翻到旧提交又加回来。
- `::_build_statusbar` — 按钮文案回到「清空接收」，command 指回 `clear_rx`。
- `::__init__` — `Ctrl+L` 改绑 `clear_rx`（快捷键保留，语义跟着变）。
- `Tools/SerialAssist/README.md` — 该小节重写为「清空接收」，用表格列出
  "保留的东西"，并写清"清屏不等于停收"。

### 验证

真机：SWD 复位让 BootLoader 吐 380 B，清屏瞬间队列仍压着 6 块、RX 计数不变；
随后 4 s 内 12 行 / 380 B 全部自动补上屏，一行没少。
（第一次只等 1.2 s，漏看 `jump to app` —— BootLoader 的接收窗口是 3 s。）

---

## 2026-11-20 13:32 — 串口助手加「清空缓冲」

> **已被上面 13:36 那条撤回。** 需求是清接收区显示，不是清串口缓冲；
> 保留此条只为记录当时的判断过程。

### 问题

状态栏原来只有「清空接收」，实际只做了 `Text.delete()`。串口是流式的，
点下去那一刻数据还散在三处：主线程队列里压着的待渲染块、驱动层
`in_waiting` 里还没被 reader 线程读走的字节、reader 正在 `read()` 里
马上要返回的那一块。只清屏幕，前两批会在几十毫秒后重新铺满，
现象是"点了像没反应"，在 OTA 抓日志时尤其难受。

### 改动

- `Tools/SerialAssist/serial_assist.py::OtaSerialApp.__init__` —
  新增 `rx_epoch` 分代号（初始 0），作为"清空/读取"之间的同步闸。
- `::_reader_loop` — `read()` 之前记下 `rx_epoch`，返回后比对；
  不一致说明这期间被清空过，该块直接丢弃。代价是清空点附近少量
  新字节一并丢，属于"清空"的预期语义。
- `::clear_rx` — **语义收窄为只清显示区**，保留给 `on_disp_mode_change`
  使用（切换显示模式时缓冲区本来就要按新格式重来，不该把计数也清了）。
- `::clear_buffer` — **新增**，全清：`rx_epoch += 1` → 记 `in_waiting`
  → `reset_input_buffer()` → 排空队列 → 清 `line_buf`/`hex_buf`/`hex_off`
  → 重建两个增量解码器 → `clear_rx()` → `rx_count` 归零。
  顺序不能反：先清队列的话 reader 手上的字节随后又塞回来，等于白清。
  解码器必须重建，否则清空后第一批数据会拼上残留的半个多字节字符解出乱码。
- `::_build_statusbar` — 按钮文案「清空接收」→「清空缓冲」，
  command 改指 `clear_buffer`。名字得对得上实际行为。
- `::__init__` — 绑 `Ctrl+L`（`"break"` 收尾，避免主题吞键）。
- `Tools/SerialAssist/README.md` — 新增「清空缓冲」小节，说明清了哪四处、
  为什么只 delete 文本不够、提示行里的字节数怎么算。

### 验证

界面级脚本（`Tk` + 假串口对象）逐项断言：驱动层 purge、队列排空、
RX 归零、半行/HEX 尾丢弃、解码器干净、分代号推进、提示行字节数
= `in_waiting + 队列`，8 项全过。另用会在 `read()` 中推进 `rx_epoch`
的假设备验证竞态：清空前的残渣被丢弃，清空后的新数据正常保留。

---

## 2026-11-20 13:26 — 修正 CubeMX 生成 SPI 后的三处冲突

### 问题

阶段 0 手写了一份 `Application/BSP/SPI/spi.c`。之后用 CubeMX 生成 Boot 工程
底层，它把 SPI2 生成到了 `Core/Src/spi.c`（并新建了 `Boot_Loader.ioc`）。
于是工程里同时存在两份 `spi.c`，各自定义了 `hspi2` 和 `MX_SPI2_Init()`，
而 `srcDirs` 同时包含 `Core` 和 `Application` —— **链接期必然多重定义**。

同时发现另一处不一致：CubeMX 默认给的是 `SPI_BAUDRATEPRESCALER_2`
（PCLK1 36MHz / 2 = 18MHz），而 Application 侧是 `PRESCALER_4`（9MHz）。
两边读写同一颗 W25Q 却跑在不同时钟下，一旦出问题极难定位
（两边各自自测都通，只有搬到一起才对不上）。

### 改动

- `Boot_Loader/Application/BSP/SPI/` — **整个移除**。改由 CubeMX 生成的
  `Core/Src/spi.c` + `Core/Inc/spi.h` 提供，往后只维护这一份。
- `Boot_Loader/Core/Src/spi.c::MX_SPI2_Init` —
  `SPI_BAUDRATEPRESCALER_2` → `SPI_BAUDRATEPRESCALER_4`（18MHz → 9MHz）。
  理由：①与 App 侧时序一致 ②搬运瓶颈在内部 Flash 擦写（约 20ms/页），
  SPI 快一倍几乎不省时间 ③18MHz 对飞线敏感，误码表现为"偶发 CRC 不对"。
- `Boot_Loader/Boot_Loader.ioc` — 补 `SPI2.BaudRatePrescaler=SPI_BAUDRATEPRESCALER_4`，
  `CalculateBaudRate` 18.0 → 9.0，并把 `BaudRatePrescaler` 加进 `IPParameters`。
  **必须同步改这里**，否则下次 CubeMX 生成会把 spi.c 的分频改回 18MHz。
- `Boot_Loader/Core/Src/main.c` — 删除 USER CODE 区重复的 `#include "spi.h"`
  （CubeMX 已把它加进生成区）。

### 配置核对结论（`Boot_Loader.ioc`）

| 项 | 值 | 判定 |
|---|---|---|
| Mode | Full_Duplex_Master | 正确 |
| 引脚 | PB13=SCK / PB14=MISO / PB15=MOSI | 与板上接线一致 |
| NSS | 软件（PB12 未在 CubeMX 中出现） | 正确，片选交给 w25q.c |
| CPOL / CPHA | Low / 1Edge（Mode 0） | W25Q 要求 |
| DataSize | 8bit | 正确 |
| DMA | USART1_RX → DMA1_Channel5，CIRCULAR | 原样保留，NVIC 也在 |
| 波特率 | 18MHz → **9MHz** | 已改 |

### 验证

- 全量编译 + 链接：零警告，Flash **16708 B / 32KB (51%)**，RAM 5900 B
- `dma.c` / `usart.c` 经 CubeMX 重生成后内容未变，UART DMA 绑定完好
- 上板：`[boot] w25q id=0xEF4018 OK` → `crc ok` → `jump to app` → `Hello, world!`
  App 侧 `[w25q] read back: A0 A1 ... AF` 十六字节全对

### 注意（CubeMX 使用约定）

工程现在有 `Boot_Loader.ioc`，从此**每次生成都会覆盖生成区**：

- USER CODE 区的东西（W25Q 自检、PrintResetCause 调用）安全
- 生成区的手工改动会被冲掉 —— 改生成区代码时必须同步改 `.ioc`
- `USART1.IPParameters` 目前只有 `VirtualMode`，没记波特率/字长/校验。
  CubeMX 会用默认 115200 8N1，恰好与现状一致，属于"碰巧对"。
  **建议在 CubeMX 界面里把 USART1 参数显式设一遍**，让它记进 .ioc。

---

## 2026-11-20 13:20 — 阶段 0：BootLoader 接入 SPI2 + W25Q128

### 需求（方案调整）

OTA 架构由「Boot 直接串口收固件写内部 Flash」改为「双槽 + 外部暂存」：

```
App 一直监听串口 → 把固件写进 W25Q128 槽 A → 标记待装 → 自己复位
                                                          ↓
Boot 上电 → 查槽 A 有没有完整新固件 → 有则搬进内部 Flash → 标记试用
         → 试用期内跑不起来（启动计数超限）→ 从槽 B 回滚上一版
```

关键收益：下载阶段内部 Flash 一个字节都不动，旧固件全程可用；
传一半断网/断电只是"白传一次"，重启照常跑旧系统，而不是变砖。

代价：**BootLoader 必须自己驱动 SPI2 去读 W25Q** —— 它此前只碰过 USART1
和内部 Flash，连 SPI 的 HAL 驱动都不在工程里。本阶段只做这一件事：
让 Boot 能读到 W25Q 的 JEDEC ID。

### 改动

- `Boot_Loader/Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_spi.c`
  `.../Inc/stm32f1xx_hal_spi.h` — **新增（从 Application 复制）**。
  Boot 工程的 HAL 是裁剪过的，只有 gpio/uart/flash/rcc 等 12 个 .c，
  没有 spi。缺了会直接编译报错 `stm32f1xx_hal_spi.h: No such file`。
- `Boot_Loader/Core/Inc/stm32f1xx_hal_conf.h::HAL_SPI_MODULE_ENABLED` —
  取消注释。注意：这个开关不开时 HAL_SPI_Init() 会被编译成空实现，
  **不报错不告警**，现象只是"读 W25Q 全是 0xFF"，很难定位。
- `Boot_Loader/Application/BSP/SPI/spi.c/.h` — **新增**。SPI2 配置与
  `Application/Core/Src/spi.c` 逐字节一致（Mode 0 / 8bit / 9MHz / 软件 NSS）。
  两边不一致的后果是 App 写进去的 Boot 读出来是乱码，且不报错。
- `Boot_Loader/Application/BSP/W25Q/w25q.c/.h` — **新增（从 Application 复制）**。
  两个工程各自只把本目录下 Core/Drivers/Application 加进 srcDirs，
  物理上无法互相引用，只能各存一份。**维护约定：改动必须两边同步。**
- `Boot_Loader/Core/Src/gpio.c::MX_GPIO_Init` — 补 `__HAL_RCC_GPIOB_CLK_ENABLE()`。
  HAL_SPI_MspInit 里虽然也开，但那只在 MX_SPI2_Init 之后才生效；
  在那之前对 PB 的操作会静默失败，先开一次去掉这个时序依赖。
- `Boot_Loader/Core/Src/main.c::main` — 新增 `MX_SPI2_Init()`；在
  `PrintResetCause()` 之后加 W25Q 自检块，打印 `[boot] w25q id=...`。

### 验证

编译（arm-none-eabi-gcc 14.3.1，-Os，-Wall）四个相关文件全部零警告。
完整链接：

| 项目 | 改动前 | 改动后 | 占比 |
|---|---|---|---|
| Flash | 14372 B | **16708 B** | 51.0% / 32KB |
| RAM | 5796 B | **5892 B** | 9.0% / 64KB |

增量 2336 B（SPI HAL ≈2.2KB + W25Q 驱动 ≈750B + 自检代码）。

上板验收信号：

```
[boot] w25q id=0xEF4018 OK
```

### 遗留（后续阶段处理）

- **跳转 App 前要收尾 SPI**：`Boot_Loader_JumpToApp()` 目前只
  `HAL_UART_AbortReceive`，等阶段 3 真正用 SPI 搬运时，必须补
  `HAL_SPI_Abort(&hspi2)`，否则 App 接手可能拿到半帧状态。
- 本阶段**不改变任何现有 OTA 行为**（proto=2 流程原样保留），
  改动只增不减，出问题可直接回退这部分。

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
