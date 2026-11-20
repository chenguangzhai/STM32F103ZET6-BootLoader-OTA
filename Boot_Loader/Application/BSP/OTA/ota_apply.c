/**
 * 槽 → 内部 Flash 的搬运实现。设计说明见 ota_apply.h。
 *
 * 一次搬运的阻塞时间（自己心里要有数）：
 *   每 2KB：W25Q 读 ~1ms + 内部页擦 ~20-40ms + 写 ~1ms
 *   17KB 固件  ≈ 9 页  ≈ 0.3s
 *   476KB 固件 ≈ 238 页 ≈ 8-10s
 * 这段时间串口日志会明显卡顿，是正常的：Flash 擦写期间 CPU 停止取指。
 * 也正因为如此，搬运阶段只在整段前后各打一行，中间按 32KB 打点。
 */
#include "ota_apply.h"
#include "ota_slot.h"
#include "ota_param.h"
#include "ota_log.h"
#include "flash_app.h"
#include "w25q.h"
#include "uart_app.h"

#include <string.h>		/* memcpy：把内部 Flash 的内容搬进 W25Q 缓冲 */

/* 每次搬一页（2KB）。选 2KB 而不是更大，是为了和内部 Flash 的页大小对齐：
   擦一页、写一页，不用处理"这一段跨了两页"的情况 */
#define APPLY_CHUNK         FLASH_PAGE_BYTES

/* 每搬这么多字节打一行进度。476KB 全搬也不会刷屏 */
#define APPLY_LOG_STEP      (32UL * 1024UL)

static uint8_t s_buf[APPLY_CHUNK];      /* 2KB 落地缓冲，.bss 里 */

/* ==================== 回滚用的旧固件备份 ====================
 *
 * 为什么必须在"覆盖内部 Flash 之前"备份：
 *   一旦开始搬新固件，内部 Flash 的旧内容就被擦掉了，再也拿不回来。
 *   备份是回滚的唯一前提——槽 B 里必须始终留着一份完整可用的上一版。
 *
 * 备份失败不阻止本次升级（只打一行日志）。权衡如下：
 *   有备份 → 新固件跑不起来能自动退回
 *   没备份 → 只能靠串口重传，最坏是拿 ST-Link 救
 *   但如果因为"备份失败"就拒绝升级，用户会发现"明明固件是好的却不让装"，
 *   现场体验更差。所以按"能备份就备份，备份不了照升"处理。
 */
static int8_t BackupOldFirmware(void)
{
	OtaParam_t      p;
	OtaSlotHeader_t hb;
	OtaSlotHeader_t nb;
	uint32_t        dst;
	uint32_t        sectors;
	uint32_t        off;
	uint32_t        n;
	uint32_t        i;

	if(Ota_Param_Load(&p) != OTA_PARM_OK)
	{
		Uart_Printf(UART_LOG, "[trial] 参数页不可信，跳过备份\r\n");
		return -1;
	}

	/* 参数页说的和 Flash 里的内容对不上，说明旧固件本身就是坏的，
	   备份它只会把坏东西存进槽 B。宁可不备份 */
	if(Ota_VerifyFlash(APP_ADDR, p.length, p.crc32) != 0)
	{
		Uart_Printf(UART_LOG, "[trial] 旧固件 CRC 对不上，跳过备份\r\n");
		return -1;
	}

	if((OtaSlot_ReadHeader(OTA_SLOT_B, &hb) == OTA_SLOT_OK)
	   && (hb.fw_len == p.length)
	   && (hb.fw_crc == p.crc32)
	   && ((hb.flags & OTA_SLOT_FLAG_BACKUP) != 0UL))
	{
		return 0;		/* 槽 B 里已经是这一版，不重复擦写 */
	}

	/* 先擦头。这样即便备份中途断电，槽 B 也只是"空槽"，
	   不会留下一个看起来有效、实际数据残缺的头 —— 那会导致回滚时
	   把半截固件搬回内部 Flash，比不回滚更糟 */
	(void)OtaSlot_EraseHeader(OTA_SLOT_B);

	dst = OtaSlot_DataAddr(OTA_SLOT_B);
	if(dst == 0xFFFFFFFFUL)
	{
		return -1;
	}

	sectors = (p.length + OTA_SLOT_HDR_BYTES - 1UL) / OTA_SLOT_HDR_BYTES;
	for(i = 0UL; i < sectors; i++)
	{
		if(W25q_EraseSector(dst + (i * OTA_SLOT_HDR_BYTES)) != W25Q_OK)
		{
			Uart_Printf(UART_LOG, "[trial] 擦槽 B 失败，本次不备份\r\n");
			return -1;
		}
	}

	for(off = 0UL; off < p.length; off += APPLY_CHUNK)
	{
		n = p.length - off;
		if(n > APPLY_CHUNK)
		{
			n = APPLY_CHUNK;
		}

		/* 内部 Flash 是内存映射的，按指针读就行，不需要 HAL */
		(void)memcpy(s_buf, (const void *)(APP_ADDR + off), n);

		if(W25q_Write(dst + off, s_buf, n) != W25Q_OK)
		{
			Uart_Printf(UART_LOG, "[trial] 写槽 B 失败，本次不备份\r\n");
			return -1;
		}
	}

	OtaSlot_BuildHeader(&nb, (uint16_t)p.version, p.length, p.crc32, 0UL,
	                    OTA_SLOT_FLAG_BACKUP);

	if(OtaSlot_WriteHeader(OTA_SLOT_B, &nb) != OTA_SLOT_OK)
	{
		Uart_Printf(UART_LOG, "[trial] 写槽 B 头部失败\r\n");
		return -1;
	}

	/* 回读比对：写进去 != 写对了。和搬运那一步是同一个道理 */
	if(OtaSlot_VerifyData(OTA_SLOT_B, &nb) != 1)
	{
		(void)OtaSlot_EraseHeader(OTA_SLOT_B);
		Uart_Printf(UART_LOG, "[trial] 槽 B 回读 CRC 不符，已作废\r\n");
		return -1;
	}

	Uart_Printf(UART_LOG, "[trial] 旧固件已备份到槽 B：长度=%lu CRC=0x%08lX\r\n",
	            (unsigned long)p.length, (unsigned long)p.crc32);
	return 0;
}

/**
 * 从槽 B 把上一版搬回内部 Flash（回滚）。
 *
 * 只在"试用期用完还没被 App 确认"时调用。搬完会：
 *   1. 回读校验
 *   2. 参数页写成"已确认"（否则下次上电又当成试用中，来回滚）
 *   3. 擦掉槽 A 的头部 —— 那份跑不起来的固件没有留着的价值，
 *      留着反而会让下次上电再搬它一遍
 */
int8_t OtaApply_RestoreFromB(void)
{
	OtaSlotHeader_t hb;
	uint32_t        src;
	uint32_t        off;
	uint32_t        n;

	if(OtaSlot_ReadHeader(OTA_SLOT_B, &hb) != OTA_SLOT_OK)
	{
		Uart_Printf(UART_LOG, "[trial] 槽 B 没有可用的备份\r\n");
		return OTA_APPLY_REJECT;
	}

	if((hb.flags & OTA_SLOT_FLAG_SELFTEST) != 0UL)
	{
		return OTA_APPLY_REJECT;
	}

	if((hb.fw_len == 0UL) || (hb.fw_len > OTA_APP_MAX))
	{
		Uart_Printf(UART_LOG, "[trial] 槽 B 长度越界，拒绝回滚\r\n");
		return OTA_APPLY_REJECT;
	}

	src = OtaSlot_DataAddr(OTA_SLOT_B);
	if(src == 0xFFFFFFFFUL)
	{
		return OTA_APPLY_REJECT;
	}

	Uart_Printf(UART_LOG, "[trial] 回滚：槽 B -> 0x%08lX，长度=%lu CRC=0x%08lX\r\n",
	            (unsigned long)APP_ADDR,
	            (unsigned long)hb.fw_len, (unsigned long)hb.fw_crc);

	for(off = 0UL; off < hb.fw_len; off += APPLY_CHUNK)
	{
		n = hb.fw_len - off;
		if(n > APPLY_CHUNK)
		{
			n = APPLY_CHUNK;
		}

		if(W25q_Read(src + off, s_buf, n) != W25Q_OK)
		{
			Uart_Printf(UART_LOG, "[trial] 回滚失败：读槽 B\r\n");
			return OTA_APPLY_ERR;
		}

		if(Flash_ErasePage(APP_ADDR + off) != FLASH_OK)
		{
			Uart_Printf(UART_LOG, "[trial] 回滚失败：擦内部 Flash\r\n");
			return OTA_APPLY_ERR;
		}

		if(Flash_Write(APP_ADDR + off, s_buf, n) != FLASH_OK)
		{
			Uart_Printf(UART_LOG, "[trial] 回滚失败：写内部 Flash\r\n");
			return OTA_APPLY_ERR;
		}
	}

	if(Ota_VerifyFlash(APP_ADDR, hb.fw_len, hb.fw_crc) != 0)
	{
		Uart_Printf(UART_LOG, "[trial] 回滚失败：回读 CRC 不符\r\n");
		return OTA_APPLY_ERR;
	}

	if(Ota_Param_SaveEx(hb.fw_len, hb.fw_crc, (uint32_t)hb.fw_ver,
	                    OTA_PARM_STATE_CONFIRMED, 0U) != OTA_PARM_OK)
	{
		Uart_Printf(UART_LOG, "[trial] 回滚后写参数页失败\r\n");
		return OTA_APPLY_ERR;
	}

	/* 作废槽 A：那份固件已经证明跑不起来，留着只会在下次上电被重搬 */
	(void)OtaSlot_EraseHeader(OTA_SLOT_A);

	Uart_Printf(UART_LOG, "[trial] 回滚完成，已退回上一版\r\n");
	(void)OtaLog_Append(OTA_LOG_EVT_ROLLBACK, (uint32_t)hb.fw_ver, hb.fw_len, 0U);
	return OTA_APPLY_OK;
}

/**
 * 搬运失败时统一走这里：**把槽头擦掉**。
 *
 * 为什么不保留失败现场：如果头还在、COPIED 还是 0，下次上电会再搬一遍，
 * 再失败、再搬……启动时间里永远多了几秒到十几秒，而且日志看起来
 * 像"每次都卡在同一个地方"，很难意识到是上一次失败的残留。
 * 擦掉头等于明确写下"这槽的东西不能用"，下次上电就是干净的空槽。
 */
static int8_t FailAndErase(uint8_t slot, const char *why)
{
	Uart_Printf(UART_LOG, "[apply] 失败：%s，作废该槽头部\r\n", why);
	(void)OtaSlot_EraseHeader(slot);
	return OTA_APPLY_ERR;
}

/* ==================== 参数页丢失后的自愈 ==================== */
int8_t OtaApply_RecoverParam(void)
{
	OtaParam_t      p;
	OtaSlotHeader_t h;
	uint8_t         slot;

	/* 只有"整页空白"才重建。有内容却校验不过 = 写到一半断电，
	   那种情况猜不得，留给 CheckAppBoot 报损坏并停在 OTA */
	if(Ota_Param_Load(&p) != OTA_PARM_EMPTY)
	{
		return OTA_APPLY_NOOP;
	}

	for(slot = OTA_SLOT_A; slot <= OTA_SLOT_B; slot++)
	{
		if(OtaSlot_ReadHeader(slot, &h) != OTA_SLOT_OK)
		{
			continue;
		}
		if(((h.flags & OTA_SLOT_FLAG_SELFTEST) != 0UL)
		   || (h.fw_len == 0UL) || (h.fw_len > OTA_APP_MAX))
		{
			continue;
		}

		/* 关键一步：槽头记的 CRC 必须和内部 Flash 实测 CRC 相等，
		   才说明"板子上现在跑的就是这一份"。只有头对、Flash 不对的话，
		   重建出来就是一本人为造的错误账本，下次上电必然校验失败 */
		if(Ota_VerifyFlash(APP_ADDR, h.fw_len, h.fw_crc) != 0)
		{
			continue;
		}

		if(Ota_Param_SaveEx(h.fw_len, h.fw_crc, h.fw_ver,
							OTA_PARM_STATE_TRIAL, 0U) != OTA_PARM_OK)
		{
			return OTA_APPLY_ERR;
		}

		Uart_Printf(UART_LOG,
					"[param] 参数页丢失，已按槽 %c 重建为试用态：长度=%lu CRC=0x%08lX\r\n",
					(slot == OTA_SLOT_A) ? 'A' : 'B',
					(unsigned long)h.fw_len, (unsigned long)h.fw_crc);
		(void)OtaLog_Append(OTA_LOG_EVT_RECOVER, (uint32_t)h.fw_ver, h.fw_len,
		                    (uint32_t)slot);
		return OTA_APPLY_OK;
	}

	/* 两个槽都对不上内部 Flash：不重建。宁可退回兼容模式，
	   也不能拿一份对不上的账本跳过校验 */
	Uart_Printf(UART_LOG,
				"[param] 参数页丢失，且无槽可与内部 Flash 对上，保持兼容模式\r\n");
	return OTA_APPLY_ERR;
}

int8_t OtaApply_FromSlot(uint8_t slot)
{
	OtaSlotHeader_t h;
	OtaSlotHeader_t nh;			/* 改过 flags 的新头，hdr_crc 必须重算 */
	uint32_t data_addr;
	uint32_t off;
	int8_t   st;

	st = OtaSlot_ReadHeader(slot, &h);

	if(st == OTA_SLOT_EMPTY)
	{
		return OTA_APPLY_NOOP;
	}

	if(st != OTA_SLOT_OK)
	{
		/* 头不可信就当没这回事。这里故意不擦：BAD 槽是排查线索，
		   擦掉就再也不知道它当初为什么坏了 */
		return OTA_APPLY_NOOP;
	}

	/* 自测假头必须拒绝。阶段 1 的自检会用 16 字节的假数据写一个合法头，
	   一旦被搬进内部 Flash 再跳过去，第一句取指就是 HardFault。
	   这个检查放在最前面，因为它比长度检查更"致命" */
	if((h.flags & OTA_SLOT_FLAG_SELFTEST) != 0UL)
	{
		Uart_Printf(UART_LOG, "[apply] 槽 %c：自测假头，忽略\r\n",
					(slot == OTA_SLOT_A) ? 'A' : 'B');
		return OTA_APPLY_REJECT;
	}

	/* 搬过的就不再搬。COPIED 只在"参数页也写成功"之后才置位 */
	if((h.flags & OTA_SLOT_FLAG_COPIED) != 0UL)
	{
		return OTA_APPLY_NOOP;
	}

	/* 上界必须查：OTA_APP_MAX 到参数页为止，越过它就会把参数页擦掉，
	   而参数页一没，下次上电连"原来那份固件多长"都不知道了 */
	if((h.fw_len == 0UL) || (h.fw_len > OTA_APP_MAX))
	{
		Uart_Printf(UART_LOG, "[apply] 槽 %c：长度=%lu 超出 App 区上限 %lu\r\n",
					(slot == OTA_SLOT_A) ? 'A' : 'B',
					(unsigned long)h.fw_len, (unsigned long)OTA_APP_MAX);
		return OTA_APPLY_REJECT;
	}

	data_addr = OtaSlot_DataAddr(slot);
	if(data_addr == 0xFFFFFFFFUL)
	{
		return OTA_APPLY_REJECT;
	}

	/* 内部 Flash 里已经是这份固件时，不重复擦写。
	   两个作用：一是省掉几百毫秒的擦写；二是"重发同一版"不会无谓消耗
	   内部 Flash 的擦写寿命（F103 只有 1 万次） */
	if(Ota_VerifyFlash(APP_ADDR, h.fw_len, h.fw_crc) == 0)
	{
		/* 打标记必须用 OtaSlot_BuildHeader 重建，不能只改 h.flags：
		   hdr_crc 覆盖的 24 字节里就包含 flags，直接改字段会让 CRC 失效，
		   OtaSlot_WriteHeader 的 RAM 自检会把它判成"半成品头"而拒绝写入。
		   这个坑踩过一次 —— 表现是日志里一句"槽头标记写入失败"，
		   数据其实搬对了，但每次上电都要重新回读校验一遍。 */
		OtaSlot_BuildHeader(&nh, h.fw_ver, h.fw_len, h.fw_crc, h.build_time,
							h.flags | OTA_SLOT_FLAG_COPIED);
		(void)OtaSlot_WriteHeader(slot, &nh);

		/* 参数页在这条路径上必须单独兜一次。
		 *
		 * 为什么：本条分支跳过了下面"搬运 + 写参数页"的整套流程，而参数页
		 * 完全可能是空的。最典型的场景就是"先用 SWD 把 App 烧进 0x08008000，
		 * 再从串口发同一版" —— 校验一比对，内容一样，于是只补标记就返回，
		 * 参数页永远建不起来。之后每次上电 Boot 都报"无参数页"，
		 * 只能靠兼容模式盲跳，试用/回滚机制等于不存在。
		 *
		 * 只在"参数页确实没描述这份固件"时才写，并且写 TRIAL 而不是
		 * CONFIRMED：内容一致不代表这份固件被验证过（可能是第一次装）。
		 * 反过来，如果参数页已经准确匹配，就一个字都不动 ——
		 * 否则重发同一版会把已确认状态打回试用，白白多跑一轮试用期。 */
		{
			OtaParam_t p;
			int8_t     st = Ota_Param_Load(&p);

			if((st != OTA_PARM_OK)
			   || (p.length != h.fw_len) || (p.crc32 != h.fw_crc))
			{
				if(Ota_Param_SaveEx(h.fw_len, h.fw_crc, (uint32_t)h.fw_ver,
				                    OTA_PARM_STATE_TRIAL, 0U) == OTA_PARM_OK)
				{
					Uart_Printf(UART_LOG, "[apply] 参数页缺失或不符，已补写为试用态\r\n");
				}
				else
				{
					Uart_Printf(UART_LOG, "[apply] 警告：参数页补写失败\r\n");
				}
			}
			else if(p.version != (uint32_t)h.fw_ver)
			{
				/* 内容没变、只有版本标注变了（典型场景：同一份固件重发时
				   把版本号从 1 改成 2）。必须同步，否则参数页会成为降级
				   比较的错基准 —— 板子实际是 2，账本记 1，下次发 1 就给放了。
				   这里要**保留**原来的 state/cnt：内容没换，没理由把已经
				   确认过的固件打回试用，白多一轮 5 秒等待和一次擦写 */
				if(Ota_Param_SaveEx(p.length, p.crc32, (uint32_t)h.fw_ver,
				                    Ota_Param_State(p.reserved),
				                    Ota_Param_TrialCnt(p.reserved)) == OTA_PARM_OK)
				{
					Uart_Printf(UART_LOG, "[apply] 参数页版本更新为 %u\r\n",
								(unsigned)h.fw_ver);
				}
			}
		}

		Uart_Printf(UART_LOG, "[apply] 内部 Flash 已是 长度=%lu CRC=0x%08lX，只补标记\r\n",
					(unsigned long)h.fw_len, (unsigned long)h.fw_crc);
		(void)OtaLog_Append(OTA_LOG_EVT_ALREADY, (uint32_t)h.fw_ver, h.fw_len, 0U);
		return OTA_APPLY_ALREADY;
	}

#if (OTA_ALLOW_DOWNGRADE == 0)
	/* 降级拦截。
	 *
	 * 放在这里的两个理由：
	 *   1. 必须在上面那条"内部 Flash 已是这份"的快路径之后 —— 重发当前版本
	 *      （产线复检、刷同一版回滚现场）是正常操作，它不擦不写，没有风险，
	 *      不该被版本号挡住。
	 *   2. 必须在 BackupOldFirmware 之前 —— 被拒绝的固件不应该顺手把槽 B
	 *      里的回滚源覆盖掉（备份是"覆盖旧固件"的伴生动作，既然不覆盖，
	 *      就不该有副作用）。
	 *
	 * 只比较参数页里记的版本，因为那是"板子上真正在跑的那一版"。槽 B 头里的
	 * 版本是老版本，拿它比较会把正常的升级误判成降级。
	 *
	 * 参数页为空（老固件/刚量产）或版本字段异常时不拦：没有基准就无从比较，
	 * 这时宁可放行，也不要把板子卡在"永远刷不进去"的状态。
	 */
	{
		OtaParam_t p;

		if((Ota_Param_Load(&p) == OTA_PARM_OK)
		   && (p.version <= 0xFFFFUL)
		   && (p.version != 0UL)
		   && ((uint32_t)h.fw_ver < p.version))
		{
			Uart_Printf(UART_LOG,
						"[apply] 槽 %c：版本 %u 低于当前 %u，拒绝降级\r\n",
						(slot == OTA_SLOT_A) ? 'A' : 'B',
						(unsigned)h.fw_ver, (unsigned)p.version);
			Uart_Printf(UART_LOG,
						"[apply] 如需强制降级，请改用槽 B 回滚或重编 Boot（OTA_ALLOW_DOWNGRADE=1）\r\n");
			/* 记一笔"被谁挡下来的"。现场最常见的问题就是"我明明发了新固件，
			   设备纹丝不动"，而日志一断电就没了，事后完全无从判断是降级被拒
			   还是压根没收到 */
			(void)OtaLog_Append(OTA_LOG_EVT_DOWNGRADE, (uint32_t)h.fw_ver,
			                    h.fw_len, p.version);
			return OTA_APPLY_REJECT;
		}
	}
#endif

	/* 覆盖内部 Flash 之前，先把"现在这份"存进槽 B。
	   这是回滚的唯一前提：一旦开始搬新固件，旧内容就再也拿不回来了。
	   失败也不中止升级，理由见 BackupOldFirmware 的注释 */
	(void)BackupOldFirmware();

	Uart_Printf(UART_LOG, "[apply] 槽 %c -> 0x%08lX，长度=%lu CRC=0x%08lX\r\n",
				(slot == OTA_SLOT_A) ? 'A' : 'B',
				(unsigned long)APP_ADDR,
				(unsigned long)h.fw_len, (unsigned long)h.fw_crc);

	for(off = 0UL; off < h.fw_len; off += APPLY_CHUNK)
	{
		uint32_t n = h.fw_len - off;
		uint32_t dst = APP_ADDR + off;

		if(n > APPLY_CHUNK)
		{
			n = APPLY_CHUNK;
		}

		if(W25q_Read(data_addr + off, s_buf, n) != W25Q_OK)
		{
			return FailAndErase(slot, "读 W25Q");
		}

		/* 擦和写都按页对齐。off 是 2KB 的整数倍、APP_ADDR 也是页首，
		   所以 dst 一定是页首，不会出现"擦错页"那种灾难 */
		if(Flash_ErasePage(dst) != FLASH_OK)
		{
			return FailAndErase(slot, "擦内部 Flash");
		}

		if(Flash_Write(dst, s_buf, n) != FLASH_OK)
		{
			return FailAndErase(slot, "写内部 Flash");
		}

		if(((off + n) % APPLY_LOG_STEP) < n)
		{
			Uart_Printf(UART_LOG, "[apply]   %lu/%lu\r\n",
						(unsigned long)(off + n), (unsigned long)h.fw_len);
		}
	}

	/* 读回来重算 CRC。这一步是唯一能证明"内部 Flash 里确实是对的"的手段：
	   边搬边算只能证明"我从 W25Q 读到了什么"，证明不了"写进 Flash 的是什么" */
	if(Ota_VerifyFlash(APP_ADDR, h.fw_len, h.fw_crc) != 0)
	{
		return FailAndErase(slot, "回读 CRC 校验");
	}

	/* 顺序不能反：先写参数页，再给槽头打 COPIED。
	   反过来的话，如果参数页写失败，槽头已经标了"搬过"→ 下次上电跳过搬运，
	   而参数页里还是旧的长度/CRC → CheckAppBoot 失败 → 明明固件是好的，
	   板子却停在 OTA 等重传。按现在的顺序，最坏只是重搬一次（幂等）。

	   这里必须用 SaveEx 并把状态置成"试用中"：刚装上的固件还没被 App
	   确认过，万一它根本跑不起来，要靠这个状态触发回滚。
	   用 Ota_Param_Save 会写成"已确认"，试用期直接失效。 */
	if(Ota_Param_SaveEx(h.fw_len, h.fw_crc, (uint32_t)h.fw_ver,
	                    OTA_PARM_STATE_TRIAL, 0U) != OTA_PARM_OK)
	{
		return FailAndErase(slot, "写参数页");
	}

	/* 注意：不能只改 h.flags 就写回去。hdr_crc 覆盖的 24 字节里包含 flags，
	   直接改字段会让 CRC 过期，OtaSlot_WriteHeader 的 RAM 自检会拒绝写入。
	   必须用 OtaSlot_BuildHeader 重建整个头部（它会顺带重算 hdr_crc）。 */
	OtaSlot_BuildHeader(&nh, h.fw_ver, h.fw_len, h.fw_crc, h.build_time,
						h.flags | OTA_SLOT_FLAG_COPIED);

	/* 这里失败其实无伤大雅：参数页已经更新，下次上电会走"只补标记"那条路 */
	if(OtaSlot_WriteHeader(slot, &nh) != OTA_SLOT_OK)
	{
		Uart_Printf(UART_LOG, "[apply] 警告：槽头标记写入失败，下次上电会重搬\r\n");
	}

	Uart_Printf(UART_LOG, "[apply] 完成 %luB，参数页已更新\r\n",
				(unsigned long)h.fw_len);

	(void)OtaLog_Append(OTA_LOG_EVT_APPLY, (uint32_t)h.fw_ver, h.fw_len, 0U);
	return OTA_APPLY_OK;
}
