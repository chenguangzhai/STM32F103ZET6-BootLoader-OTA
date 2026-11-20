/**
 * OTA 黑匣子：把启动/升级/回滚的关键决策记到外部 Flash 里
 *
 * 解决什么问题：
 *   串口日志是"现场直播"，拔了线、断了电就什么都不剩。于是遇到
 *   "设备自己变回旧版了""昨天还好好的今天一直重启"这类反馈时，
 *   只能靠复现去猜。而这类问题往往几十次里出一次，现场根本复现不了。
 *
 *   黑匣子把最近 128 条关键事件落到 W25Q，每次上电自动倒出最近几条，
 *   于是"它经历过什么"变成可查的事实，而不是猜测。
 *
 * 存哪：0x101000，紧跟试用确认标记之后，独占一个扇区（4KB）。
 *   这个位置在双槽（0~0x0FFFFF）和确认标记（0x100000）之外，
 *   不和任何现有数据重叠。将来要扩到多个扇区，只需要改 OTA_LOG_SECTORS。
 *
 * 为什么不用内部 Flash：参数页一页 2KB 且擦写寿命只有 1 万次，
 *   日志是高频写入，放进去会把参数页的寿命吃光 —— 而参数页是启动判断
 *   依赖的东西，它坏了代价大得多。W25Q 有 10 万次擦写寿命，且容量富裕。
 *
 * 写满了怎么办：
 *   128 条写满后擦掉整个扇区从头开始（序号继续递增，不归零）。
 *   也就是"永远保留最近 128 条"。对排查故障来说足够了 —— 一个设备
 *   在 128 次升级/启动之内还没定位到问题，问题大概率不在记录里。
 */
#ifndef __OTA_LOG_H
#define __OTA_LOG_H

#include <stdint.h>

/* ==================== 布局 ==================== */

/* 紧跟确认标记（0x100000）之后的那个扇区 */
#define OTA_LOG_BASE            0x00101000UL
#define OTA_LOG_SECTOR_BYTES    4096UL
#define OTA_LOG_ENTRY_BYTES     32UL
#define OTA_LOG_MAX             (OTA_LOG_SECTOR_BYTES / OTA_LOG_ENTRY_BYTES)  /* 128 条 */

/* 记录识别码，写成 'LOGB' 的小端形式 */
#define OTA_LOG_MAGIC           0x42474F4CUL

/* ==================== 事件类型 ====================
 *
 * 只加不减：已经烧进设备的记录可能带老编号，删掉会张冠李戴。
 * 新增一律往后排。
 */
#define OTA_LOG_EVT_RST_POR     1U      /* 上电 / 欠压复位 */
#define OTA_LOG_EVT_RST_PIN     2U      /* 复位引脚（人按的） */
#define OTA_LOG_EVT_RST_SFT     3U      /* 软件复位（App 收完固件主动回） */
#define OTA_LOG_EVT_RST_IWDG    4U      /* 看门狗复位 */
#define OTA_LOG_EVT_RST_OTHER   5U      /* 其它复位（低功耗唤醒等） */
#define OTA_LOG_EVT_APPLY       6U      /* 把槽里的固件搬进内部 Flash */
#define OTA_LOG_EVT_ALREADY     7U      /* 内部 Flash 已是这份，只补标记 */
#define OTA_LOG_EVT_TRIAL       8U      /* 试用计数 +1（arg = 第几次） */
#define OTA_LOG_EVT_CONFIRM     9U      /* 试用转正（App 已确认） */
#define OTA_LOG_EVT_TRIAL_FAIL  10U     /* 判定失败（arg = 第几次） */
#define OTA_LOG_EVT_ROLLBACK    11U     /* 回滚到槽 B */
#define OTA_LOG_EVT_DOWNGRADE   12U     /* 拒绝降级（arg = 当前版本） */
#define OTA_LOG_EVT_RECOVER     13U     /* 参数页丢失，按槽重建 */
#define OTA_LOG_EVT_OTA_MODE    14U     /* 上电窗口内收到数据，进入 OTA */
#define OTA_LOG_EVT_STUCK       15U     /* 上次跳转后没进 App 的 main */
#define OTA_LOG_EVT_NOAPP       16U     /* 没有可用 App，停在串口等重传 */

/* 一条记录，32 字节整（扇区正好装 128 条，不留碎片） */
typedef struct
{
	uint32_t magic;         /* OTA_LOG_MAGIC */
	uint32_t seq;           /* 单调递增序号，跨擦除也不归零 */
	uint32_t evt;           /* OTA_LOG_EVT_xxx */
	uint32_t ver;           /* 相关固件版本号，无则 0 */
	uint32_t fw_len;        /* 相关固件长度，无则 0 */
	uint32_t arg;           /* 附加信息，含义随事件而定 */
	uint32_t reserved;      /* 预留，写 0 */
	uint32_t crc;           /* 上面 28 字节的 CRC32，防止写一半被当成有效记录 */
} OtaLogEntry_t;

/* ==================== 接口 ==================== */

/* 追加一条记录。返回 1 = 写成功（且回读校验通过），0 = 失败。
 * 失败不影响调用方主流程 —— 日志丢一条远不如升级失败严重，
 * 所以这里从不返回值拦截升级动作。 */
uint8_t OtaLog_Append(uint32_t evt, uint32_t ver, uint32_t len, uint32_t arg);

/* 打印最近 n 条（n 从新到旧，最多 OTA_LOG_MAX）。
 * 返回实际打印的条数。没有记录时返回 0 且不打印任何东西。 */
uint8_t OtaLog_DumpRecent(uint8_t n);

/* 当前有效记录条数（0 ~ 128） */
uint8_t OtaLog_Count(void);

#endif /* __OTA_LOG_H */
