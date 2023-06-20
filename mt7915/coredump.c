// SPDX-License-Identifier: ISC
/* Copyright (C) 2022 MediaTek Inc. */

#include <linux/devcoredump.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/utsname.h>
#include "coredump.h"

static bool coredump_memdump = true;
module_param(coredump_memdump, bool, 0644);
MODULE_PARM_DESC(coredump_memdump, "Optional ability to dump firmware memory");

static const struct mt7915_mem_region mt7915_mem_regions[] = {
	{
		.start = 0xe003b400,
		.len = 0x00003bff,
		.name = "CRAM",
	},
};

static const struct mt7915_mem_region mt7916_mem_regions[] = {
	{
		.start = 0x00800000,
		.len = 0x0005ffff,
		.name = "ROM",
	},
	{
		.start = 0x00900000,
		.len = 0x00013fff,
		.name = "ULM1",
	},
	{
		.start = 0x02200000,
		.len = 0x0004ffff,
		.name = "ULM2",
	},
	{
		.start = 0x02300000,
		.len = 0x0004ffff,
		.name = "ULM3",
	},
	{
		.start = 0x00400000,
		.len = 0x00027fff,
		.name = "SRAM",
	},
	{
		.start = 0xe0000000,
		.len = 0x00157fff,
		.name = "CRAM",
	},
};

static const struct mt7915_mem_region mt7986_mem_regions[] = {
	{
		.start = 0x00800000,
		.len = 0x0005ffff,
		.name = "ROM",
	},
	{
		.start = 0x00900000,
		.len = 0x0000ffff,
		.name = "ULM1",
	},
	{
		.start = 0x02200000,
		.len = 0x0004ffff,
		.name = "ULM2",
	},
	{
		.start = 0x02300000,
		.len = 0x0004ffff,
		.name = "ULM3",
	},
	{
		.start = 0x00400000,
		.len = 0x00017fff,
		.name = "SRAM",
	},
	{
		.start = 0xe0000000,
		.len = 0x00113fff,
		.name = "CRAM",
	},
};

const struct mt7915_mem_region*
mt7915_coredump_get_mem_layout(struct mt7915_dev *dev, u8 type, u32 *num)
{
	if (type == MT76_RAM_TYPE_WA)
		return NULL;

	switch (mt76_chip(&dev->mt76)) {
	case 0x7915:
		*num = ARRAY_SIZE(mt7915_mem_regions);
		return &mt7915_mem_regions[0];
	case 0x7986:
		*num = ARRAY_SIZE(mt7986_mem_regions);
		return &mt7986_mem_regions[0];
	case 0x7916:
		*num = ARRAY_SIZE(mt7916_mem_regions);
		return &mt7916_mem_regions[0];
	default:
		return NULL;
	}
}

static int mt7915_coredump_get_mem_size(struct mt7915_dev *dev, u8 type)
{
	const struct mt7915_mem_region *mem_region;
	size_t size = 0;
	u32 num;
	int i;

	mem_region = mt7915_coredump_get_mem_layout(dev, type, &num);
	if (!mem_region)
		return 0;

	for (i = 0; i < num; i++) {
		size += mem_region->len;
		mem_region++;
	}

	/* reserve space for the headers */
	size += num * sizeof(struct mt7915_mem_hdr);
	/* make sure it is aligned 4 bytes for debug message print out */
	size = ALIGN(size, 4);

	return size;
}

struct mt7915_crash_data *mt7915_coredump_new(struct mt7915_dev *dev, u8 type)
{
	struct mt7915_crash_data *crash_data = dev->coredump.crash_data[type];

	lockdep_assert_held(&dev->dump_mutex);

	guid_gen(&crash_data->guid);
	ktime_get_real_ts64(&crash_data->timestamp);

	return crash_data;
}

static void
mt7915_coredump_fw_state(struct mt7915_dev *dev, u8 type, struct mt7915_coredump *dump,
			 bool *exception)
{
	u32 state, count, category;

	if (type == MT76_RAM_TYPE_WA)
		return;

	category = (u32)mt76_get_field(dev, MT_FW_EXCEPT_TYPE, GENMASK(7, 0));
	state = (u32)mt76_get_field(dev, MT_FW_ASSERT_STAT, GENMASK(7, 0));
	count = is_mt7915(&dev->mt76) ?
		(u32)mt76_get_field(dev, MT_FW_EXCEPT_COUNT, GENMASK(15, 8)) :
		(u32)mt76_get_field(dev, MT_FW_EXCEPT_COUNT, GENMASK(7, 0));

	/* normal mode: driver can manually trigger assert for detail info */
	if (!count)
		strscpy(dump->fw_state, "normal", sizeof(dump->fw_state));
	else if (state > 1 && (count == 1) && category == 5)
		strscpy(dump->fw_state, "assert", sizeof(dump->fw_state));
	else if ((state > 1 && count == 1) || count > 1)
		strscpy(dump->fw_state, "exception", sizeof(dump->fw_state));

	*exception = !!count;
}

static void
mt7915_coredump_fw_trace(struct mt7915_dev *dev, u8 type, struct mt7915_coredump *dump,
			 bool exception)
{
	u32 n, irq, sch, base = MT_FW_EINT_INFO;

	if (type == MT76_RAM_TYPE_WA)
		return;

	/* trap or run? */
	dump->last_msg_id = mt76_rr(dev, MT_FW_LAST_MSG_ID);

	n = is_mt7915(&dev->mt76) ?
	    (u32)mt76_get_field(dev, base, GENMASK(7, 0)) :
	    (u32)mt76_get_field(dev, base, GENMASK(15, 8));
	dump->eint_info_idx = n;

	irq = mt76_rr(dev, base + 0x8);
	n = is_mt7915(&dev->mt76) ?
	    FIELD_GET(GENMASK(7, 0), irq) : FIELD_GET(GENMASK(23, 16), irq);
	dump->irq_info_idx = n;

	sch = mt76_rr(dev, MT_FW_SCHED_INFO);
	n = is_mt7915(&dev->mt76) ?
	    FIELD_GET(GENMASK(7, 0), sch) : FIELD_GET(GENMASK(15, 8), sch);
	dump->sched_info_idx = n;

	if (exception) {
		u32 i, y;

		/* sched trace */
		n = is_mt7915(&dev->mt76) ?
		    FIELD_GET(GENMASK(15, 8), sch) : FIELD_GET(GENMASK(7, 0), sch);
		n = n > 60 ? 60 : n;

		strscpy(dump->trace_sched, "(sched_info) id, time",
			sizeof(dump->trace_sched));

		for (y = dump->sched_info_idx, i = 0; i < n; i++, y++) {
			mt7915_memcpy_fromio(dev, dump->sched, base + 0xc + y * 12,
					     sizeof(dump->sched));
			y = y >= n ? 0 : y;
		}

		/* irq trace */
		n = is_mt7915(&dev->mt76) ?
		    FIELD_GET(GENMASK(15, 8), irq) : FIELD_GET(GENMASK(7, 0), irq);
		n = n > 60 ? 60 : n;

		strscpy(dump->trace_irq, "(irq_info) id, time",
			sizeof(dump->trace_irq));

		for (y = dump->irq_info_idx, i = 0; i < n; i++, y++) {
			mt7915_memcpy_fromio(dev, dump->irq, base + 0x4 + y * 16,
					     sizeof(dump->irq));
			y = y >= n ? 0 : y;
		}
	}
}

static void
mt7915_coredump_fw_stack(struct mt7915_dev *dev, u8 type, struct mt7915_coredump *dump,
			 bool exception)
{
	u32 reg, i;

	if (type == MT76_RAM_TYPE_WA)
		return;

	/* read current PC */
	mt76_rmw_field(dev, MT_CONN_DBG_CTL_LOG_SEL,
		       MT_CONN_DBG_CTL_PC_LOG_SEL, 0x22);
	for (i = 0; i < 10; i++) {
		dump->pc_cur[i] = mt76_rr(dev, MT_CONN_DBG_CTL_PC_LOG);
		usleep_range(100, 500);
	}

	/* stop call stack record */
	if (!exception) {
		mt76_clear(dev, MT_MCU_WM_EXCP_PC_CTRL, BIT(0));
		mt76_clear(dev, MT_MCU_WM_EXCP_LR_CTRL, BIT(0));
	}

	/* read PC log */
	dump->pc_dbg_ctrl = mt76_rr(dev, MT_MCU_WM_EXCP_PC_CTRL);
	dump->pc_cur_idx = FIELD_GET(MT_MCU_WM_EXCP_PC_CTRL_IDX_STATUS,
				     dump->pc_dbg_ctrl);
	for (i = 0; i < 32; i++) {
		reg = MT_MCU_WM_EXCP_PC_LOG + i * 4;
		dump->pc_stack[i] = mt76_rr(dev, reg);
	}

	/* read LR log */
	dump->lr_dbg_ctrl = mt76_rr(dev, MT_MCU_WM_EXCP_LR_CTRL);
	dump->lr_cur_idx = FIELD_GET(MT_MCU_WM_EXCP_LR_CTRL_IDX_STATUS,
				     dump->lr_dbg_ctrl);
	for (i = 0; i < 32; i++) {
		reg = MT_MCU_WM_EXCP_LR_LOG + i * 4;
		dump->lr_stack[i] = mt76_rr(dev, reg);
	}

	/* start call stack record */
	if (!exception) {
		mt76_set(dev, MT_MCU_WM_EXCP_PC_CTRL, BIT(0));
		mt76_set(dev, MT_MCU_WM_EXCP_LR_CTRL, BIT(0));
	}
}

static void
mt7915_coredump_fw_task(struct mt7915_dev *dev, u8 type, struct mt7915_coredump *dump)
{
	u32 offs = is_mt7915(&dev->mt76) ? 0xe0 : 0x170;

	if (type == MT76_RAM_TYPE_WA)
		return;

	strscpy(dump->task_qid, "(task queue id) read, write",
		sizeof(dump->task_qid));

	dump->taskq[0].read = mt76_rr(dev, MT_FW_TASK_QID1);
	dump->taskq[0].write = mt76_rr(dev, MT_FW_TASK_QID1 - 4);
	dump->taskq[1].read = mt76_rr(dev, MT_FW_TASK_QID2);
	dump->taskq[1].write = mt76_rr(dev, MT_FW_TASK_QID2 - 4);

	strscpy(dump->task_info, "(task stack) start, end, size",
		sizeof(dump->task_info));

	dump->taski[0].start = mt76_rr(dev, MT_FW_TASK_START);
	dump->taski[0].end = mt76_rr(dev, MT_FW_TASK_END);
	dump->taski[0].size = mt76_rr(dev, MT_FW_TASK_SIZE);
	dump->taski[1].start = mt76_rr(dev, MT_FW_TASK_START + offs);
	dump->taski[1].end = mt76_rr(dev, MT_FW_TASK_END + offs);
	dump->taski[1].size = mt76_rr(dev, MT_FW_TASK_SIZE + offs);
}

static void
mt7915_coredump_fw_context(struct mt7915_dev *dev, u8 type, struct mt7915_coredump *dump)
{
	u32 count, idx, id;

	if (type == MT76_RAM_TYPE_WA)
		return;

	count = mt76_rr(dev, MT_FW_CIRQ_COUNT);

	/* current context */
	if (!count) {
		strscpy(dump->fw_context, "(context) interrupt",
			sizeof(dump->fw_context));

		idx = is_mt7915(&dev->mt76) ?
		      (u32)mt76_get_field(dev, MT_FW_CIRQ_IDX, GENMASK(31, 16)) :
		      (u32)mt76_get_field(dev, MT_FW_CIRQ_IDX, GENMASK(15, 0));
		dump->context.idx = idx;
		dump->context.handler = mt76_rr(dev, MT_FW_CIRQ_LISR);
	} else {
		idx = mt76_rr(dev, MT_FW_TASK_IDX);
		id = mt76_rr(dev, MT_FW_TASK_ID);

		if (!id && idx == 3) {
			strscpy(dump->fw_context, "(context) idle",
				sizeof(dump->fw_context));
		} else if (id && idx != 3) {
			strscpy(dump->fw_context, "(context) task",
				sizeof(dump->fw_context));

			dump->context.idx = idx;
			dump->context.handler = id;
		}
	}
}

static struct mt7915_coredump *mt7915_coredump_build(struct mt7915_dev *dev, u8 type)
{
	struct mt76_dev *mdev = &dev->mt76;
	struct mt7915_crash_data *crash_data = dev->coredump.crash_data[type];
	struct mt7915_coredump *dump;
	struct mt7915_coredump_mem *dump_mem;
	size_t len, sofar = 0, hdr_len = sizeof(*dump);
	unsigned char *buf;
	bool exception;

	len = hdr_len;

	if (coredump_memdump && crash_data->memdump_buf_len)
		len += sizeof(*dump_mem) + crash_data->memdump_buf_len;

	sofar += hdr_len;

	/* this is going to get big when we start dumping memory and such,
	 * so go ahead and use vmalloc.
	 */
	buf = vzalloc(len);
	if (!buf)
		return NULL;

	mutex_lock(&dev->dump_mutex);

	dump = (struct mt7915_coredump *)(buf);
	dump->len = len;
	dump->hdr_len = hdr_len;

	/* plain text */
	strscpy(dump->magic, "mt76-crash-dump", sizeof(dump->magic));
	strscpy(dump->kernel, init_utsname()->release, sizeof(dump->kernel));
	strscpy(dump->fw_ver, mdev->hw->wiphy->fw_version,
		sizeof(dump->fw_ver));
	strscpy(dump->fw_type, ((type == MT76_RAM_TYPE_WA) ? "WA" : "WM"),
		sizeof(dump->fw_type));
	strscpy(dump->fw_patch_date, mdev->patch_hdr->build_date,
		sizeof(dump->fw_patch_date));
	strscpy(dump->fw_ram_date[MT76_RAM_TYPE_WM],
		mdev->wm_hdr->build_date,
		sizeof(mdev->wm_hdr->build_date));
	strscpy(dump->fw_ram_date[MT76_RAM_TYPE_WA],
		mdev->wa_hdr->build_date,
		sizeof(mdev->wa_hdr->build_date));

	guid_copy(&dump->guid, &crash_data->guid);
	dump->tv_sec = crash_data->timestamp.tv_sec;
	dump->tv_nsec = crash_data->timestamp.tv_nsec;
	dump->device_id = mt76_chip(&dev->mt76);

	mt7915_coredump_fw_state(dev, type, dump, &exception);
	mt7915_coredump_fw_trace(dev, type, dump, exception);
	mt7915_coredump_fw_task(dev, type, dump);
	mt7915_coredump_fw_context(dev, type, dump);
	mt7915_coredump_fw_stack(dev, type, dump, exception);

	/* gather memory content */
	dump_mem = (struct mt7915_coredump_mem *)(buf + sofar);
	dump_mem->len = crash_data->memdump_buf_len;
	if (coredump_memdump && crash_data->memdump_buf_len)
		memcpy(dump_mem->data, crash_data->memdump_buf,
		       crash_data->memdump_buf_len);

	mutex_unlock(&dev->dump_mutex);

	return dump;
}

int mt7915_coredump_submit(struct mt7915_dev *dev, u8 type)
{
	struct mt7915_coredump *dump;

	dump = mt7915_coredump_build(dev, type);
	if (!dump) {
		dev_warn(dev->mt76.dev, "no crash dump data found\n");
		return -ENODATA;
	}

	dev_coredumpv(dev->mt76.dev, dump, dump->len, GFP_KERNEL);
	dev_info(dev->mt76.dev, "%s coredump completed\n",
		 wiphy_name(dev->mt76.hw->wiphy));

	return 0;
}

int mt7915_coredump_register(struct mt7915_dev *dev)
{
	struct mt7915_crash_data *crash_data;
	int i;

	for (i = 0; i < __MT76_RAM_TYPE_MAX; i++) {
		crash_data = vzalloc(sizeof(*dev->coredump.crash_data[i]));
		if (!crash_data)
			return -ENOMEM;

		dev->coredump.crash_data[i] = crash_data;

		if (coredump_memdump) {
			crash_data->memdump_buf_len = mt7915_coredump_get_mem_size(dev, i);
			if (!crash_data->memdump_buf_len)
				/* no memory content */
				return 0;

			crash_data->memdump_buf = vzalloc(crash_data->memdump_buf_len);
			if (!crash_data->memdump_buf) {
				vfree(crash_data);
				return -ENOMEM;
			}
		}
	}

	return 0;
}

void mt7915_coredump_unregister(struct mt7915_dev *dev)
{
	int i;

	for (i = 0; i < __MT76_RAM_TYPE_MAX; i++) {
		if (dev->coredump.crash_data[i]->memdump_buf) {
			vfree(dev->coredump.crash_data[i]->memdump_buf);
			dev->coredump.crash_data[i]->memdump_buf = NULL;
			dev->coredump.crash_data[i]->memdump_buf_len = 0;
		}

		vfree(dev->coredump.crash_data[i]);
		dev->coredump.crash_data[i] = NULL;
	}
}

