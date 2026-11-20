/**
 * OTA 双槽（A/B）在 W25Q128 上的布局与头部读写
 *
 * 【这份代码 App 和 Boot 各有一份副本，必须逐字一致】
 * 为什么要复制：两个工程各有独立的 srcDirs，物理上引用不到对方目录。
 * 维护约定：改一边必须同步另一边，否则 App 写的头 Boot 读不懂，
 * 而症状只是"槽一直是 BAD"—— 不会报错，只会让人怀疑 Flash 坏了。
 *
 * ================== 为什么是双槽 ==================
 *
 * 单分区升级的致命问题：新固件直接覆盖 App 区，传到一半断电，
 * 旧固件已经被擦掉、新固件还不完整 —— 板子变砖，只能拿 ST-Link 救。
 *
 * 双槽把"下载"和"执行"彻底分开：
 *   槽 A / 槽 B 在 外部 Flash 里，只负责"存放候选固件"
 *   内部 Flash 的 App 区才是"正在执行的固件"
 * 下载全程不碰内部 Flash，所以下载失败只是浪费一次擦写，
 * 现有系统毫发无损 —— 这是架构级的安全，不是"缩小窗口"的优化。
 *
 * 回滚同理：槽 B 长期保留上一版完整固件，新固件装上后如果连续启动失败，
 * Boot 从槽 B 重新搬一份回去即可。
 *
 * ================== 为什么头部独占一个扇区 ==================
 *
 * 另一个选择是把 32 字节头部放在固件数据前面（偏移 0），省 4KB。
 * 但那样"头部已写、数据还没收完"这个中间态就只能靠写入顺序来保证有效，
 * 一旦顺序被打乱（重发、上位机中途退出、Boot 与 App 各自写），
 * 就会出现"头说 100KB、实际只有 60KB"这种自相矛盾的状态。
 *
 * 单独占一个扇区后，头部和固件在物理上互不重叠：
 * 擦/写固件时永远碰不到头部，写头部时也永远碰不到固件。
 * 代价是每槽 4KB —— 16MB 的片子完全不必在意。
 *
 * ================== 头部什么时候写 ==================
 *
 * 必须最后写。顺序是：擦数据区 → 收完固件 → 回读算 CRC → CRC 对上才写头部。
 * 这样"头部存在"就等于"固件已经完整且校验通过"，Boot 看到一个有效头
 * 就可以放心搬运，不需要再猜。
 */

#ifndef __OTA_SLOT_H
#define __OTA_SLOT_H

#include <stdint.h>

/* ==================== 布局 ====================
 *
 * W25Q128 前 1MB 划给双槽，后面 15MB 留给日志/历史版本/出厂备份：
 *
 *   0x000000 ┌──────────────┐
 *            │ 槽 A 头 4KB  │  ← 只放 32 字节，独占扇区是为了隔离
 *   0x001000 ├──────────────┤
 *            │ 槽 A 数据    │  508KB
 *   0x080000 ├──────────────┤
 *            │ 槽 B 头 4KB  │
 *   0x081000 ├──────────────┤
 *            │ 槽 B 数据    │  508KB
 *   0x100000 └──────────────┘
 *
 * 注意 508KB > 内部 App 区的 476KB，所以槽容量足够装下任何合法固件；
 * 真正卡上限的是内部 Flash（OTA_APP_MAX），搬运阶段再查。
 */

#define OTA_SLOT_A              0U
#define OTA_SLOT_B              1U

#define OTA_SLOT_SIZE           (512UL * 1024UL)        /* 每槽 512KB */
#define OTA_SLOT_A_BASE         0x000000UL
#define OTA_SLOT_B_BASE         (OTA_SLOT_A_BASE + OTA_SLOT_SIZE)   /* 0x080000 */

#define OTA_SLOT_HDR_BYTES      4096UL                  /* = W25Q 一个扇区，头部独占 */
#define OTA_SLOT_DATA_MAX       (OTA_SLOT_SIZE - OTA_SLOT_HDR_BYTES) /* 508KB */

/* 借用最后一个扇区做读写自测，避开双槽。App 的 W25q_SelfTest 用它 */
#define OTA_SCRATCH_BASE        0x00FFF000UL

/* ==================== 试用确认标记（阶段 4） ====================
 *
 * 位置：0x100000，紧跟双槽之后，独占一个扇区。
 *
 * 为什么需要它：
 *   "槽 A 里有没有待装固件"能自证（头部有 magic/长度/CRC），
 *   但"装上之后到底跑不跑得起来"自证不了 —— 固件躺在 Flash 里是死的，
 *   它没法告诉 Boot "我下次会不会 HardFault"。所以必须由 App 亲自写一笔，
 *   再由 Boot 上电核对。
 *
 * 为什么放 W25Q 而不放内部参数页：
 *   参数页保持"只有 Boot 能写"这条不变量。App 拿不到擦写参数页的能力，
 *   就算 App 里有 bug 也改不坏启动判断。代价只是确认时多擦一个 4KB 扇区。
 */
#define OTA_CONFIRM_BASE        0x00100000UL
#define OTA_CONFIRM_MAGIC       0x464E4F43UL    /* 'C','O','N','F' 的小端形式 */

typedef struct
{
	uint32_t magic;         /* OTA_CONFIRM_MAGIC */
	uint32_t fw_len;        /* App 当时跑着的固件长度 */
	uint32_t fw_crc;        /* App 当时跑着的固件 CRC32 */
	uint32_t crc;           /* 上面 12 字节的 CRC32，防写一半 */
} OtaConfirm_t;             /* sizeof = 16 */

/* ==================== 头部结构 ====================
 *
 * sizeof = 32 字节，正好对齐，没有填充字节（全部 4 字节对齐）。
 * 任何改动都必须同时提高 OTA_SLOT_HDR_VER，否则老头部会被误判成"版本不符 → BAD"。
 */
#define OTA_SLOT_HDR_MAGIC      0x50555746UL    /* 'F','W','U','P' 的小端形式 */
#define OTA_SLOT_HDR_VER        1U              /* 头部结构版本，改布局就 +1 */

/* flags 位定义 */
#define OTA_SLOT_FLAG_SELFTEST  0x00000001UL    /* 自测用的假头，Boot 必须忽略 */
#define OTA_SLOT_FLAG_COPIED    0x00000002UL    /* 已搬运进内部 Flash（阶段 3 用） */
#define OTA_SLOT_FLAG_BACKUP    0x00000004UL    /* 槽 B 专用：这是"上一版"的备份，不是待安装的新固件 */

typedef struct
{
	uint32_t magic;         /* OTA_SLOT_HDR_MAGIC */
	uint16_t fw_ver;        /* 固件版本号，上位机填，用来比对/拦降级 */
	uint16_t hdr_ver;       /* OTA_SLOT_HDR_VER */
	uint32_t fw_len;        /* 固件字节数，1 ~ OTA_SLOT_DATA_MAX */
	uint32_t fw_crc;        /* 固件数据的 CRC32 */
	uint32_t build_time;    /* 编译时间戳（Unix 秒），0 表示未知 */
	uint32_t flags;         /* OTA_SLOT_FLAG_xxx */
	uint32_t hdr_crc;       /* 上面 24 字节的 CRC32，防止头部自己被写坏 */
	uint32_t reserved;      /* 对齐到 32 字节，必须写 0 */
} OtaSlotHeader_t;

/* ==================== 返回值 ==================== */
#define OTA_SLOT_EMPTY          0       /* 头区全 0xFF，从没写过 */
#define OTA_SLOT_OK             1       /* 头部有效，长度也在合法范围 */
#define OTA_SLOT_BAD            2       /* 有内容但 magic/版本/CRC/长度 不对 */
#define OTA_SLOT_ERR            (-1)    /* 底层 Flash 读失败 */

/* ==================== 接口 ==================== */

/* 槽基地址（头部起始）。slot 非法时返回 0xFFFFFFFF */
uint32_t OtaSlot_Base(uint8_t slot);

/* 槽数据起始地址（= 基地址 + 4KB）。slot 非法时返回 0xFFFFFFFF */
uint32_t OtaSlot_DataAddr(uint8_t slot);

/* 数据区容量，固定 508KB */
uint32_t OtaSlot_Capacity(void);

/* 填一个完整可用的头部（含 hdr_crc）。flag 传 0 表示正常固件 */
void OtaSlot_BuildHeader(OtaSlotHeader_t *hdr, uint16_t fw_ver, uint32_t fw_len,
                         uint32_t fw_crc, uint32_t build_time, uint32_t flags);

/* 读并校验头部。
 * 返回 OTA_SLOT_OK / OTA_SLOT_EMPTY / OTA_SLOT_BAD / OTA_SLOT_ERR。
 * 非 OK 时 hdr 里的内容不可信，不要用。 */
int8_t OtaSlot_ReadHeader(uint8_t slot, OtaSlotHeader_t *hdr);

/* 先擦头部扇区，再把 32 字节写进去。写完不会自动回读校验，
 * 需要确认的话调用方自己再 ReadHeader 一次（阶段 4 的回滚判断会这么做）。 */
int8_t OtaSlot_WriteHeader(uint8_t slot, const OtaSlotHeader_t *hdr);

/* 擦掉头部，让这个槽退回"从没写过"的状态 */
int8_t OtaSlot_EraseHeader(uint8_t slot);

/* 把槽里前 len 字节读出来重算 CRC32，和 hdr->fw_crc 比对。
 * 返回 1 = 一致，0 = 不一致，负数 = Flash 读失败。
 * 注意这是"读回来算"，不是"边收边算"——只有前者能证明落盘内容正确。 */
int8_t OtaSlot_VerifyData(uint8_t slot, const OtaSlotHeader_t *hdr);

/* 读确认标记。返回 OTA_SLOT_OK / OTA_SLOT_EMPTY / OTA_SLOT_BAD / OTA_SLOT_ERR */
int8_t OtaSlot_ReadConfirm(OtaConfirm_t *c);

/* 写确认标记（先擦该扇区再写 16 字节）。App 在启动成功后调一次。
 * 已经确认过就别再调 —— 每次都要擦一个扇区，白耗寿命。 */
int8_t OtaSlot_WriteConfirm(uint32_t fw_len, uint32_t fw_crc);

#endif /* __OTA_SLOT_H */
