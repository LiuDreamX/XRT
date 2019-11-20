/**
 *  Copyright (C) 2017 Xilinx, Inc. All rights reserved.
 *  Author: Sonal Santan
 *  Code copied verbatim from SDAccel xcldma kernel mode driver
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <linux/firmware.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/uuid.h>
#include <linux/pid.h>
#include <linux/key.h>
#include <linux/efi.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0)
#include <linux/verification.h>
#endif
#include "xclbin.h"
#include "../xocl_drv.h"
#include "../xocl_drm.h"
#include "mgmt-ioctl.h"

#if defined(XOCL_UUID)
static xuid_t uuid_null = NULL_UUID_LE;
#endif

static DEFINE_MUTEX(icap_keyring_lock);
static struct key *icap_keys;
static int icap_key_users;

#define	ICAP_ERR(icap, fmt, arg...)	\
	xocl_err(&(icap)->icap_pdev->dev, fmt "\n", ##arg)
#define	ICAP_WARN(icap, fmt, arg...)	\
	xocl_warn(&(icap)->icap_pdev->dev, fmt "\n", ##arg)
#define	ICAP_INFO(icap, fmt, arg...)	\
	xocl_info(&(icap)->icap_pdev->dev, fmt "\n", ##arg)
#define	ICAP_DBG(icap, fmt, arg...)	\
	xocl_dbg(&(icap)->icap_pdev->dev, fmt "\n", ##arg)

#define	ICAP_PRIVILEGED(icap)	((icap)->icap_regs != NULL)
#define DMA_HWICAP_BITFILE_BUFFER_SIZE 1024

#define	ICAP_MAX_NUM_CLOCKS		4
#define OCL_CLKWIZ_STATUS_OFFSET	0x4
#define OCL_CLKWIZ_CONFIG_OFFSET(n)	(0x200 + 4 * (n))
#define OCL_CLK_FREQ_COUNTER_OFFSET	0x8
#define ICAP_DEFAULT_EXPIRE_SECS	1

#define INVALID_MEM_IDX			0xFFFF
/*
 * Bitstream header information.
 */
typedef struct {
	unsigned int HeaderLength;     /* Length of header in 32 bit words */
	unsigned int BitstreamLength;  /* Length of bitstream to read in bytes*/
	unsigned char *DesignName;     /* Design name read from bitstream header */
	unsigned char *PartName;       /* Part name read from bitstream header */
	unsigned char *Date;           /* Date read from bitstream header */
	unsigned char *Time;           /* Bitstream creation time read from header */
	unsigned int MagicLength;      /* Length of the magic numbers in header */
} XHwIcap_Bit_Header;
#define XHI_BIT_HEADER_FAILURE	-1
/* Used for parsing bitstream header */
#define XHI_EVEN_MAGIC_BYTE	0x0f
#define XHI_ODD_MAGIC_BYTE	0xf0
/* Extra mode for IDLE */
#define XHI_OP_IDLE		-1
/* The imaginary module length register */
#define XHI_MLR			15

#define	GATE_FREEZE_USER	0x0c

static u32 gate_free_user[] = {0xe, 0xc, 0xe, 0xf};

static struct attribute_group icap_attr_group;

enum icap_sec_level {
	ICAP_SEC_NONE = 0,
	ICAP_SEC_DEDICATE,
	ICAP_SEC_SYSTEM,
	ICAP_SEC_MAX = ICAP_SEC_SYSTEM,
};

/*
 * AXI-HWICAP IP register layout
 */
struct icap_reg {
	u32			ir_rsvd1[7];
	u32			ir_gier;
	u32			ir_isr;
	u32			ir_rsvd2;
	u32			ir_ier;
	u32			ir_rsvd3[53];
	u32			ir_wf;
	u32			ir_rf;
	u32			ir_sz;
	u32			ir_cr;
	u32			ir_sr;
	u32			ir_wfv;
	u32			ir_rfo;
	u32			ir_asr;
} __attribute__((packed));

struct icap_generic_state {
	u32			igs_state;
} __attribute__((packed));

struct icap_axi_gate {
	u32			iag_wr;
	u32			iag_rvsd;
	u32			iag_rd;
} __attribute__((packed));

struct icap_bitstream_user {
	struct list_head	ibu_list;
	pid_t			ibu_pid;
};

struct icap {
	struct platform_device	*icap_pdev;
	struct mutex		icap_lock;
	struct icap_reg		*icap_regs;
	struct icap_generic_state *icap_state;
	unsigned int		idcode;
	bool			icap_axi_gate_frozen;
	struct icap_axi_gate	*icap_axi_gate;

	xuid_t			icap_bitstream_uuid;
	int			icap_bitstream_ref;

	char			*icap_clear_bitstream;
	unsigned long		icap_clear_bitstream_length;

	char			*icap_clock_bases[ICAP_MAX_NUM_CLOCKS];
	unsigned short		icap_ocl_frequency[ICAP_MAX_NUM_CLOCKS];

	struct clock_freq_topology *icap_clock_freq_topology;
	unsigned long		icap_clock_freq_topology_length;
	char			*icap_clock_freq_counter;
	struct mem_topology	*mem_topo;
	struct ip_layout	*ip_layout;
	struct debug_ip_layout	*debug_layout;
	struct connectivity	*connectivity;

	void			*rp_bit;
	unsigned long		rp_bit_len;
	void			*rp_fdt;
	unsigned long		rp_fdt_len;
	void			*rp_mgmt_bin;
	unsigned long		rp_mgmt_bin_len;
	void			*rp_sche_bin;
	unsigned long		rp_sche_bin_len;
	void			*rp_sc_bin;
	unsigned long		*rp_sc_bin_len;

	char			*icap_clock_freq_counter_hbm;

	uint64_t		cache_expire_secs;
	struct xcl_pr_region	cache;
	ktime_t			cache_expires;

	enum icap_sec_level	sec_level;

	bool			sysfs_created;
};

static inline u32 reg_rd(void __iomem *reg)
{
	if (!reg)
		return -1;

	return XOCL_READ_REG32(reg);
}

static inline void reg_wr(void __iomem *reg, u32 val)
{
	if (!reg)
		return;

	iowrite32(val, reg);
}

/*
 * Precomputed table with config0 and config2 register values together with
 * target frequency. The steps are approximately 5 MHz apart. Table is
 * generated by wiz.pl.
 */
const static struct xclmgmt_ocl_clockwiz {
	/* target frequency */
	unsigned short ocl;
	/* config0 register */
	unsigned long config0;
	/* config2 register */
	unsigned short config2;
} frequency_table[] = {
	{/* 600*/   60, 0x0601, 0x000a},
	{/* 600*/   66, 0x0601, 0x0009},
	{/* 600*/   75, 0x0601, 0x0008},
	{/* 800*/   80, 0x0801, 0x000a},
	{/* 600*/   85, 0x0601, 0x0007},
	{/* 900*/   90, 0x0901, 0x000a},
	{/*1000*/  100, 0x0a01, 0x000a},
	{/*1100*/  110, 0x0b01, 0x000a},
	{/* 700*/  116, 0x0701, 0x0006},
	{/*1100*/  122, 0x0b01, 0x0009},
	{/* 900*/  128, 0x0901, 0x0007},
	{/*1200*/  133, 0x0c01, 0x0009},
	{/*1400*/  140, 0x0e01, 0x000a},
	{/*1200*/  150, 0x0c01, 0x0008},
	{/*1400*/  155, 0x0e01, 0x0009},
	{/* 800*/  160, 0x0801, 0x0005},
	{/*1000*/  166, 0x0a01, 0x0006},
	{/*1200*/  171, 0x0c01, 0x0007},
	{/* 900*/  180, 0x0901, 0x0005},
	{/*1300*/  185, 0x0d01, 0x0007},
	{/*1400*/  200, 0x0e01, 0x0007},
	{/*1300*/  216, 0x0d01, 0x0006},
	{/* 900*/  225, 0x0901, 0x0004},
	{/*1400*/  233, 0x0e01, 0x0006},
	{/*1200*/  240, 0x0c01, 0x0005},
	{/*1000*/  250, 0x0a01, 0x0004},
	{/*1300*/  260, 0x0d01, 0x0005},
	{/* 800*/  266, 0x0801, 0x0003},
	{/*1100*/  275, 0x0b01, 0x0004},
	{/*1400*/  280, 0x0e01, 0x0005},
	{/*1200*/  300, 0x0c01, 0x0004},
	{/*1300*/  325, 0x0d01, 0x0004},
	{/*1000*/  333, 0x0a01, 0x0003},
	{/*1400*/  350, 0x0e01, 0x0004},
	{/*1100*/  366, 0x0b01, 0x0003},
	{/*1200*/  400, 0x0c01, 0x0003},
	{/*1300*/  433, 0x0d01, 0x0003},
	{/* 900*/  450, 0x0901, 0x0002},
	{/*1400*/  466, 0x0e01, 0x0003},
	{/*1000*/  500, 0x0a01, 0x0002}
};

static int icap_parse_bitstream_axlf_section(struct platform_device *pdev,
	const struct axlf *xclbin, enum axlf_section_kind kind);
static void icap_set_data(struct icap *icap, struct xcl_pr_region *hwicap);
static uint64_t icap_get_data_nolock(struct platform_device *pdev, enum data_kind kind);
static uint64_t icap_get_data(struct platform_device *pdev, enum data_kind kind);
static const struct axlf_section_header *get_axlf_section_hdr(
	struct icap *icap, const struct axlf *top, enum axlf_section_kind kind);

static void icap_free_bins(struct icap *icap)
{
	if (icap->rp_bit) {
		vfree(icap->rp_bit);
		icap->rp_bit = NULL;
		icap->rp_bit_len = 0;
	}
	if (icap->rp_fdt) {
		vfree(icap->rp_fdt);
		icap->rp_fdt = NULL;
		icap->rp_fdt_len = 0;
	}
	if (icap->rp_mgmt_bin) {
		vfree(icap->rp_mgmt_bin);
		icap->rp_mgmt_bin = NULL;
		icap->rp_mgmt_bin_len = 0;
	}
	if (icap->rp_sche_bin) {
		vfree(icap->rp_sche_bin);
		icap->rp_sche_bin = NULL;
		icap->rp_sche_bin_len = 0;
	}
}

static void icap_read_from_peer(struct platform_device *pdev)
{
	struct xcl_mailbox_subdev_peer subdev_peer = {0};
	struct icap *icap = platform_get_drvdata(pdev);
	struct xcl_pr_region xcl_hwicap = {0};
	size_t resp_len = sizeof(struct xcl_pr_region);
	size_t data_len = sizeof(struct xcl_mailbox_subdev_peer);
	struct xcl_mailbox_req *mb_req = NULL;
	size_t reqlen = sizeof(struct xcl_mailbox_req) + data_len;
	xdev_handle_t xdev = xocl_get_xdev(pdev);

	ICAP_INFO(icap, "reading from peer");
	BUG_ON(ICAP_PRIVILEGED(icap));

	mb_req = vmalloc(reqlen);
	if (!mb_req)
		return;

	mb_req->req = XCL_MAILBOX_REQ_PEER_DATA;
	subdev_peer.size = resp_len;
	subdev_peer.kind = XCL_ICAP;
	subdev_peer.entries = 1;

	memcpy(mb_req->data, &subdev_peer, data_len);

	(void) xocl_peer_request(xdev,
		mb_req, reqlen, &xcl_hwicap, &resp_len, NULL, NULL, 0);

	icap_set_data(icap, &xcl_hwicap);

	vfree(mb_req);
}

static void icap_set_data(struct icap *icap, struct xcl_pr_region *hwicap)
{
	memcpy(&icap->cache, hwicap, sizeof(struct xcl_pr_region));
	icap->cache_expires = ktime_add(ktime_get_boottime(), ktime_set(icap->cache_expire_secs, 0));
}

static unsigned find_matching_freq_config(unsigned freq)
{
	unsigned start = 0;
	unsigned end = ARRAY_SIZE(frequency_table) - 1;
	unsigned idx = ARRAY_SIZE(frequency_table) - 1;

	if (freq < frequency_table[0].ocl)
		return 0;

	if (freq > frequency_table[ARRAY_SIZE(frequency_table) - 1].ocl)
		return ARRAY_SIZE(frequency_table) - 1;

	while (start < end) {
		if (freq == frequency_table[idx].ocl)
			break;
		if (freq < frequency_table[idx].ocl)
			end = idx;
		else
			start = idx + 1;
		idx = start + (end - start) / 2;
	}
	if (freq < frequency_table[idx].ocl)
		idx--;

	return idx;
}

static unsigned find_matching_freq(unsigned freq)
{
	int idx = find_matching_freq_config(freq);

	return frequency_table[idx].ocl;
}


static unsigned short icap_get_ocl_frequency(const struct icap *icap, int idx)
{
#define XCL_INPUT_FREQ 100
	const u64 input = XCL_INPUT_FREQ;
	u32 val;
	u32 mul0, div0;
	u32 mul_frac0 = 0;
	u32 div1;
	u32 div_frac1 = 0;
	u64 freq = 0;
	char *base = NULL;

	if (ICAP_PRIVILEGED(icap)) {
		base = icap->icap_clock_bases[idx];
		if (!base)
			return 0;
		val = reg_rd(base + OCL_CLKWIZ_STATUS_OFFSET);
		if ((val & 1) == 0)
			return 0;

		val = reg_rd(base + OCL_CLKWIZ_CONFIG_OFFSET(0));

		div0 = val & 0xff;
		mul0 = (val & 0xff00) >> 8;
		if (val & BIT(26)) {
			mul_frac0 = val >> 16;
			mul_frac0 &= 0x3ff;
		}

		/*
		 * Multiply both numerator (mul0) and the denominator (div0) with 1000
		 * to account for fractional portion of multiplier
		 */
		mul0 *= 1000;
		mul0 += mul_frac0;
		div0 *= 1000;

		val = reg_rd(base + OCL_CLKWIZ_CONFIG_OFFSET(2));

		div1 = val & 0xff;
		if (val & BIT(18)) {
			div_frac1 = val >> 8;
			div_frac1 &= 0x3ff;
		}

		/*
		 * Multiply both numerator (mul0) and the denominator (div1) with 1000 to
		 * account for fractional portion of divider
		 */

		div1 *= 1000;
		div1 += div_frac1;
		div0 *= div1;
		mul0 *= 1000;
		if (div0 == 0) {
			ICAP_ERR(icap, "clockwiz 0 divider");
			return 0;
		}
		freq = (input * mul0) / div0;
	} else {
		switch (idx) {
		case 0:
			freq = icap_get_data_nolock(icap->icap_pdev, CLOCK_FREQ_0);
			break;
		case 1:
			freq = icap_get_data_nolock(icap->icap_pdev, CLOCK_FREQ_1);
			break;
		case 2:
			freq = icap_get_data_nolock(icap->icap_pdev, CLOCK_FREQ_2);
			break;
		default:
			break;
		}
	}
	return freq;
}

static unsigned int icap_get_clock_frequency_counter_khz(const struct icap *icap, int idx)
{
	u32 freq = 0, status;
	int times = 10;
	/*
	 * reset and wait until done
	 */
	if (ICAP_PRIVILEGED(icap)) {
		if (uuid_is_null(&icap->icap_bitstream_uuid))
			return freq;

		if (!icap->icap_clock_freq_counter)
			return freq;

		if (idx < 2) {
			reg_wr(icap->icap_clock_freq_counter, 0x1);
			while (times != 0) {
				status = reg_rd(icap->icap_clock_freq_counter);
				if (status == 0x2)
					break;
				mdelay(1);
				times--;
			};
			freq = reg_rd(icap->icap_clock_freq_counter + OCL_CLK_FREQ_COUNTER_OFFSET + idx*sizeof(u32));
		} else if (idx == 2) {
			if (!icap->icap_clock_freq_counter_hbm)
				return 0;

			reg_wr(icap->icap_clock_freq_counter_hbm, 0x1);
			while (times != 0) {
				status = reg_rd(icap->icap_clock_freq_counter_hbm);
				if (status == 0x2)
					break;
				mdelay(1);
				times--;
			};
			freq = reg_rd(icap->icap_clock_freq_counter_hbm + OCL_CLK_FREQ_COUNTER_OFFSET);
		}

	} else {
		switch (idx) {
		case 0:
			freq = icap_get_data_nolock(icap->icap_pdev, FREQ_COUNTER_0);
			break;
		case 1:
			freq = icap_get_data_nolock(icap->icap_pdev, FREQ_COUNTER_1);
			break;
		case 2:
			freq = icap_get_data_nolock(icap->icap_pdev, FREQ_COUNTER_2);
			break;
		default:
			break;
		}
	}
	return freq;
}
/*
 * Based on Clocking Wizard v5.1, section Dynamic Reconfiguration
 * through AXI4-Lite
 * Note: this is being protected by write_lock which is atomic context,
 *       we should only use n[m]delay instead of n[m]sleep.
 *       based on Linux doc of timers, mdelay may not be exactly accurate
 *       on non-PC devices.
 */
static int icap_ocl_freqscaling(struct icap *icap, bool force)
{
	unsigned curr_freq;
	u32 config;
	int i;
	int j = 0;
	u32 val = 0;
	unsigned idx = 0;
	long err = 0;

	for (i = 0; i < ICAP_MAX_NUM_CLOCKS; ++i) {
		/* A value of zero means skip scaling for this clock index */
		if (!icap->icap_ocl_frequency[i])
			continue;

		idx = find_matching_freq_config(icap->icap_ocl_frequency[i]);
		curr_freq = icap_get_ocl_frequency(icap, i);
		ICAP_INFO(icap, "Clock %d, Current %d Mhz, New %d Mhz ",
				i, curr_freq, icap->icap_ocl_frequency[i]);

		/*
		 * If current frequency is in the same step as the
		 * requested frequency then nothing to do.
		 */
		if (!force && (find_matching_freq_config(curr_freq) == idx))
			continue;

		val = reg_rd(icap->icap_clock_bases[i] +
			OCL_CLKWIZ_STATUS_OFFSET);
		if (val != 1) {
			ICAP_ERR(icap, "clockwiz %d is busy", i);
			err = -EBUSY;
			break;
		}

		config = frequency_table[idx].config0;
		reg_wr(icap->icap_clock_bases[i] + OCL_CLKWIZ_CONFIG_OFFSET(0),
			config);
		config = frequency_table[idx].config2;
		reg_wr(icap->icap_clock_bases[i] + OCL_CLKWIZ_CONFIG_OFFSET(2),
			config);
		mdelay(10);
		reg_wr(icap->icap_clock_bases[i] + OCL_CLKWIZ_CONFIG_OFFSET(23),
			0x00000007);
		mdelay(1);
		reg_wr(icap->icap_clock_bases[i] + OCL_CLKWIZ_CONFIG_OFFSET(23),
			0x00000002);

		ICAP_INFO(icap, "clockwiz waiting for locked signal");
		mdelay(100);
		for (j = 0; j < 100; j++) {
			val = reg_rd(icap->icap_clock_bases[i] +
				OCL_CLKWIZ_STATUS_OFFSET);
			if (val != 1) {
				mdelay(100);
				continue;
			}
		}
		if (val != 1) {
			ICAP_ERR(icap, "clockwiz MMCM/PLL did not lock after %d"
				"ms, restoring the original configuration",
				100 * 100);
			/* restore the original clock configuration */
			reg_wr(icap->icap_clock_bases[i] +
				OCL_CLKWIZ_CONFIG_OFFSET(23), 0x00000004);
			mdelay(10);
			reg_wr(icap->icap_clock_bases[i] +
				OCL_CLKWIZ_CONFIG_OFFSET(23), 0x00000000);
			err = -ETIMEDOUT;
			break;
		}
		val = reg_rd(icap->icap_clock_bases[i] +
			OCL_CLKWIZ_CONFIG_OFFSET(0));
		ICAP_INFO(icap, "clockwiz CONFIG(0) 0x%x", val);
		val = reg_rd(icap->icap_clock_bases[i] +
			OCL_CLKWIZ_CONFIG_OFFSET(2));
		ICAP_INFO(icap, "clockwiz CONFIG(2) 0x%x", val);
	}

	return err;
}

static bool icap_bitstream_in_use(struct icap *icap)
{
	BUG_ON(icap->icap_bitstream_ref < 0);
	return icap->icap_bitstream_ref != 0;
}

static int icap_freeze_axi_gate(struct icap *icap)
{
	xdev_handle_t xdev = xocl_get_xdev(icap->icap_pdev);

	ICAP_INFO(icap, "freezing CL AXI gate");
	BUG_ON(icap->icap_axi_gate_frozen);

	if (XOCL_DSA_IS_SMARTN(xdev)) {
		xocl_xmc_dr_freeze(xdev);
	} else {

		write_lock(&XDEV(xdev)->rwlock);
		(void) reg_rd(&icap->icap_axi_gate->iag_rd);
		reg_wr(&icap->icap_axi_gate->iag_wr, GATE_FREEZE_USER);
		(void) reg_rd(&icap->icap_axi_gate->iag_rd);

		if (!xocl_is_unified(xdev)) {
			reg_wr(&icap->icap_regs->ir_cr, 0xc);
			ndelay(20);
		} else {
			/* New ICAP reset sequence applicable only to unified dsa. */
			reg_wr(&icap->icap_regs->ir_cr, 0x8);
			ndelay(2000);
			reg_wr(&icap->icap_regs->ir_cr, 0x0);
			ndelay(2000);
			reg_wr(&icap->icap_regs->ir_cr, 0x4);
			ndelay(2000);
			reg_wr(&icap->icap_regs->ir_cr, 0x0);
			ndelay(2000);
		}
	}
	icap->icap_axi_gate_frozen = true;

	return 0;
}

static int icap_free_axi_gate(struct icap *icap)
{
	xdev_handle_t xdev = xocl_get_xdev(icap->icap_pdev);
	int i;

	ICAP_INFO(icap, "freeing CL AXI gate");
	/*
	 * First pulse the OCL RESET. This is important for PR with multiple
	 * clocks as it resets the edge triggered clock converter FIFO
	 */

	if (!icap->icap_axi_gate_frozen)
		return 0;

	if (XOCL_DSA_IS_SMARTN(xdev)) {
		xocl_xmc_dr_free(xdev);
	} else {
		for (i = 0; i < ARRAY_SIZE(gate_free_user); i++) {
			(void) reg_rd(&icap->icap_axi_gate->iag_rd);
			reg_wr(&icap->icap_axi_gate->iag_wr, gate_free_user[i]);
			ndelay(500);
		}

		(void) reg_rd(&icap->icap_axi_gate->iag_rd);

		write_unlock(&XDEV(xdev)->rwlock);
	}
	icap->icap_axi_gate_frozen = false;
	return 0;
}

static void platform_reset_axi_gate(struct platform_device *pdev)
{
	struct icap *icap = platform_get_drvdata(pdev);

	/* Can only be done from mgmt pf. */
	if (!ICAP_PRIVILEGED(icap))
		return;

	mutex_lock(&icap->icap_lock);
	if (!icap_bitstream_in_use(icap)) {
		(void) icap_freeze_axi_gate(platform_get_drvdata(pdev));
		msleep(500);
		(void) icap_free_axi_gate(platform_get_drvdata(pdev));
		msleep(500);
	}
	mutex_unlock(&icap->icap_lock);
}

static int set_freqs(struct icap *icap, unsigned short *freqs, int num_freqs)
{
	int i;
	int err = 0;
	u32 val;

	for (i = 0; i < min(ICAP_MAX_NUM_CLOCKS, num_freqs); ++i) {
		if (freqs[i] == 0)
			continue;

		if (!icap->icap_clock_bases[i])
			continue;

		val = reg_rd(icap->icap_clock_bases[i] +
			OCL_CLKWIZ_STATUS_OFFSET);
		if ((val & 0x1) == 0) {
			ICAP_ERR(icap, "clockwiz %d is busy", i);
			err = -EBUSY;
			goto done;
		}
	}

	memcpy(icap->icap_ocl_frequency, freqs,
		sizeof(*freqs) * min(ICAP_MAX_NUM_CLOCKS, num_freqs));

	icap_freeze_axi_gate(icap);
	err = icap_ocl_freqscaling(icap, false);
	icap_free_axi_gate(icap);

done:
	return err;

}

static int set_and_verify_freqs(struct icap *icap, unsigned short *freqs, int num_freqs)
{
	int i;
	int err;
	u32 clock_freq_counter, request_in_khz, tolerance, lookup_freq;

	err = set_freqs(icap, freqs, num_freqs);
	if (err)
		return err;

	for (i = 0; i < min(ICAP_MAX_NUM_CLOCKS, num_freqs); ++i) {
		if (!freqs[i])
			continue;

		lookup_freq = find_matching_freq(freqs[i]);
		clock_freq_counter = icap_get_clock_frequency_counter_khz(icap, i);
		request_in_khz = lookup_freq*1000;
		tolerance = lookup_freq*50;
		if (tolerance < abs(clock_freq_counter-request_in_khz)) {
			ICAP_ERR(icap, "Frequency is higher than tolerance value, request %u"
					"khz, actual %u khz", request_in_khz, clock_freq_counter);
			err = -EDOM;
			break;
		}
	}

	return err;
}

static int icap_ocl_set_freqscaling(struct platform_device *pdev,
	unsigned int region, unsigned short *freqs, int num_freqs)
{
	struct icap *icap = platform_get_drvdata(pdev);
	int err = 0;

	/* Can only be done from mgmt pf. */
	if (!ICAP_PRIVILEGED(icap))
		return -EPERM;

	/* For now, only PR region 0 is supported. */
	if (region != 0)
		return -EINVAL;

	mutex_lock(&icap->icap_lock);

	err = set_freqs(icap, freqs, num_freqs);

	mutex_unlock(&icap->icap_lock);

	return err;
}

static void icap_get_ocl_frequency_max_min(struct icap *icap,
	int idx, unsigned short *freq_max, unsigned short *freq_min)
{
	struct clock_freq_topology *topology = 0;
	int num_clocks = 0;

	BUG_ON(!mutex_is_locked(&icap->icap_lock));

	if (!uuid_is_null(&icap->icap_bitstream_uuid)) {
		topology = icap->icap_clock_freq_topology;
		if (!topology)
			return;

		num_clocks = topology->m_count;

		if (idx >= num_clocks)
			return;

		if (freq_max)
			*freq_max = topology->m_clock_freq[idx].m_freq_Mhz;

		if (freq_min)
			*freq_min = frequency_table[0].ocl;
	}

}

static int icap_ocl_update_clock_freq_topology(struct platform_device *pdev, struct xclmgmt_ioc_freqscaling *freq_obj)
{
	struct icap *icap = platform_get_drvdata(pdev);
	struct clock_freq_topology *topology = 0;
	int num_clocks = 0;
	int i = 0;
	int err = 0;
	unsigned short freq_max, freq_min;

	mutex_lock(&icap->icap_lock);
	if (!uuid_is_null(&icap->icap_bitstream_uuid)) {
		topology = icap->icap_clock_freq_topology;
		if (!topology) {
			err = -EDOM;
			goto done;
		}

		num_clocks = topology->m_count;
		ICAP_INFO(icap, "Num clocks is %d", num_clocks);
		for (i = 0; i < ARRAY_SIZE(freq_obj->ocl_target_freq); i++) {
			if (!freq_obj->ocl_target_freq[i])
				continue;
			freq_max = freq_min = 0;
			icap_get_ocl_frequency_max_min(icap, i, &freq_max, &freq_min);
			ICAP_INFO(icap, "requested frequency is : %d, "
				"xclbin freq is: %d, "
				"xclbin minimum freq allowed is: %d",
				freq_obj->ocl_target_freq[i],
				freq_max, freq_min);
			if (freq_obj->ocl_target_freq[i] > freq_max ||
				freq_obj->ocl_target_freq[i] < freq_min) {
				ICAP_ERR(icap, "Unable to set frequency! "
					"Frequency max: %d, Frequency min: %d, "
					"Requested frequency: %d",
					freq_max, freq_min,
					freq_obj->ocl_target_freq[i]);
				err = -EDOM;
				goto done;
			}
		}
	} else {
		ICAP_ERR(icap, "ERROR: There isn't a hardware accelerator loaded in the dynamic region."
			" Validation of accelerator frequencies cannot be determine");
		err = -EDOM;
		goto done;
	}

	err = set_and_verify_freqs(icap, freq_obj->ocl_target_freq, ARRAY_SIZE(freq_obj->ocl_target_freq));

done:
	mutex_unlock(&icap->icap_lock);
	return err;
}

static int icap_ocl_get_freqscaling(struct platform_device *pdev,
	unsigned int region, unsigned short *freqs, int num_freqs)
{
	int i;
	struct icap *icap = platform_get_drvdata(pdev);

	/* For now, only PR region 0 is supported. */
	if (region != 0)
		return -EINVAL;

	mutex_lock(&icap->icap_lock);
	for (i = 0; i < min(ICAP_MAX_NUM_CLOCKS, num_freqs); i++)
		freqs[i] = icap_get_ocl_frequency(icap, i);
	mutex_unlock(&icap->icap_lock);

	return 0;
}

static inline bool mig_calibration_done(struct icap *icap)
{
	return icap->icap_state ? (reg_rd(&icap->icap_state->igs_state) & BIT(0)) != 0 : 0;
}

/* Check for MIG calibration. */
static int calibrate_mig(struct icap *icap)
{
	int i;

	for (i = 0; i < 20 && !mig_calibration_done(icap); ++i)
		msleep(500);

	if (!mig_calibration_done(icap)) {
		ICAP_ERR(icap,
			"MIG calibration timeout after bitstream download");
		return -ETIMEDOUT;
	}

	return 0;
}

static inline void free_clock_freq_topology(struct icap *icap)
{
	vfree(icap->icap_clock_freq_topology);
	icap->icap_clock_freq_topology = NULL;
	icap->icap_clock_freq_topology_length = 0;
}

static void icap_write_clock_freq(struct clock_freq *dst, struct clock_freq *src)
{
	dst->m_freq_Mhz = src->m_freq_Mhz;
	dst->m_type = src->m_type;
	memcpy(&dst->m_name, &src->m_name, sizeof(src->m_name));
}


static int icap_setup_clock_freq_topology(struct icap *icap, const struct axlf *xclbin)
{
	int i;
	struct clock_freq_topology *topology;
	struct clock_freq *clk_freq = NULL;
	const struct axlf_section_header *hdr =
		get_axlf_section_hdr(icap, xclbin, CLOCK_FREQ_TOPOLOGY);

	/* Can't find CLOCK_FREQ_TOPOLOGY, just return*/
	if (!hdr)
		return 0;

	free_clock_freq_topology(icap);

	icap->icap_clock_freq_topology = vzalloc(hdr->m_sectionSize);
	if (!icap->icap_clock_freq_topology)
		return -ENOMEM;

	topology = (struct clock_freq_topology *)(((char *)xclbin) + hdr->m_sectionOffset);

	/*
	 *  icap->icap_clock_freq_topology->m_clock_freq
	 *  must follow the order
	 *
	 *	0: DATA_CLK
	 *	1: KERNEL_CLK
	 *	2: SYSTEM_CLK
	 *
	 */
	icap->icap_clock_freq_topology->m_count = topology->m_count;
	for (i = 0; i < topology->m_count; ++i) {
		if (topology->m_clock_freq[i].m_type == CT_SYSTEM)
			clk_freq = &icap->icap_clock_freq_topology->m_clock_freq[SYSTEM_CLK];
		else if (topology->m_clock_freq[i].m_type == CT_DATA)
			clk_freq = &icap->icap_clock_freq_topology->m_clock_freq[DATA_CLK];
		else if (topology->m_clock_freq[i].m_type == CT_KERNEL)
			clk_freq = &icap->icap_clock_freq_topology->m_clock_freq[KERNEL_CLK];
		else
			break;

		icap_write_clock_freq(clk_freq, &topology->m_clock_freq[i]);
	}

	return 0;
}

static inline void free_clear_bitstream(struct icap *icap)
{
	vfree(icap->icap_clear_bitstream);
	icap->icap_clear_bitstream = NULL;
	icap->icap_clear_bitstream_length = 0;
}

static int icap_setup_clear_bitstream(struct icap *icap,
	const char *buffer, unsigned long length)
{
	free_clear_bitstream(icap);

	if (length == 0)
		return 0;

	icap->icap_clear_bitstream = vmalloc(length);
	if (!icap->icap_clear_bitstream)
		return -ENOMEM;

	memcpy(icap->icap_clear_bitstream, buffer, length);
	icap->icap_clear_bitstream_length = length;

	return 0;
}

static int wait_for_done(struct icap *icap)
{
	u32 w;
	int i = 0;

	for (i = 0; i < 10; i++) {
		udelay(5);
		w = reg_rd(&icap->icap_regs->ir_sr);
		ICAP_INFO(icap, "XHWICAP_SR: %x", w);
		if (w & 0x5)
			return 0;
	}

	ICAP_ERR(icap, "bitstream download timeout");
	return -ETIMEDOUT;
}

static int icap_write(struct icap *icap, const u32 *word_buf, int size)
{
	int i;
	u32 value = 0;

	for (i = 0; i < size; i++) {
		value = be32_to_cpu(word_buf[i]);
		reg_wr(&icap->icap_regs->ir_wf, value);
	}

	reg_wr(&icap->icap_regs->ir_cr, 0x1);

	for (i = 0; i < 20; i++) {
		value = reg_rd(&icap->icap_regs->ir_cr);
		if ((value & 0x1) == 0)
			return 0;
		ndelay(50);
	}

	ICAP_ERR(icap, "writing %d dwords timeout", size);
	return -EIO;
}

static uint64_t icap_get_section_size(struct icap *icap, enum axlf_section_kind kind)
{
	uint64_t size = 0;

	switch (kind) {
	case IP_LAYOUT:
		size = sizeof_sect(icap->ip_layout, m_ip_data);
		break;
	case MEM_TOPOLOGY:
		size = sizeof_sect(icap->mem_topo, m_mem_data);
		break;
	case DEBUG_IP_LAYOUT:
		size = sizeof_sect(icap->debug_layout, m_debug_ip_data);
		break;
	case CONNECTIVITY:
		size = sizeof_sect(icap->connectivity, m_connection);
		break;
	case CLOCK_FREQ_TOPOLOGY:
		size = sizeof_sect(icap->icap_clock_freq_topology, m_clock_freq);
		break;
	default:
		break;
	}

	return size;
}

static int bitstream_parse_header(struct icap *icap, const unsigned char *data,
	unsigned int size, XHwIcap_Bit_Header *header)
{
	unsigned int i;
	unsigned int len;
	unsigned int tmp;
	unsigned int index;

	/* Start Index at start of bitstream */
	index = 0;

	/* Initialize HeaderLength.  If header returned early inidicates
	 * failure.
	 */
	header->HeaderLength = XHI_BIT_HEADER_FAILURE;

	/* Get "Magic" length */
	header->MagicLength = data[index++];
	header->MagicLength = (header->MagicLength << 8) | data[index++];

	/* Read in "magic" */
	for (i = 0; i < header->MagicLength - 1; i++) {
		tmp = data[index++];
		if (i%2 == 0 && tmp != XHI_EVEN_MAGIC_BYTE)
			return -1;   /* INVALID_FILE_HEADER_ERROR */

		if (i%2 == 1 && tmp != XHI_ODD_MAGIC_BYTE)
			return -1;   /* INVALID_FILE_HEADER_ERROR */

	}

	/* Read null end of magic data. */
	tmp = data[index++];

	/* Read 0x01 (short) */
	tmp = data[index++];
	tmp = (tmp << 8) | data[index++];

	/* Check the "0x01" half word */
	if (tmp != 0x01)
		return -1;	 /* INVALID_FILE_HEADER_ERROR */

	/* Read 'a' */
	tmp = data[index++];
	if (tmp != 'a')
		return -1;	  /* INVALID_FILE_HEADER_ERROR	*/

	/* Get Design Name length */
	len = data[index++];
	len = (len << 8) | data[index++];

	/* allocate space for design name and final null character. */
	header->DesignName = kmalloc(len, GFP_KERNEL);

	/* Read in Design Name */
	for (i = 0; i < len; i++)
		header->DesignName[i] = data[index++];


	if (header->DesignName[len-1] != '\0')
		return -1;

	/* Read 'b' */
	tmp = data[index++];
	if (tmp != 'b')
		return -1;	/* INVALID_FILE_HEADER_ERROR */

	/* Get Part Name length */
	len = data[index++];
	len = (len << 8) | data[index++];

	/* allocate space for part name and final null character. */
	header->PartName = kmalloc(len, GFP_KERNEL);

	/* Read in part name */
	for (i = 0; i < len; i++)
		header->PartName[i] = data[index++];

	if (header->PartName[len-1] != '\0')
		return -1;

	/* Read 'c' */
	tmp = data[index++];
	if (tmp != 'c')
		return -1;	/* INVALID_FILE_HEADER_ERROR */

	/* Get date length */
	len = data[index++];
	len = (len << 8) | data[index++];

	/* allocate space for date and final null character. */
	header->Date = kmalloc(len, GFP_KERNEL);

	/* Read in date name */
	for (i = 0; i < len; i++)
		header->Date[i] = data[index++];

	if (header->Date[len - 1] != '\0')
		return -1;

	/* Read 'd' */
	tmp = data[index++];
	if (tmp != 'd')
		return -1;	/* INVALID_FILE_HEADER_ERROR  */

	/* Get time length */
	len = data[index++];
	len = (len << 8) | data[index++];

	/* allocate space for time and final null character. */
	header->Time = kmalloc(len, GFP_KERNEL);

	/* Read in time name */
	for (i = 0; i < len; i++)
		header->Time[i] = data[index++];

	if (header->Time[len - 1] != '\0')
		return -1;

	/* Read 'e' */
	tmp = data[index++];
	if (tmp != 'e')
		return -1;	/* INVALID_FILE_HEADER_ERROR */

	/* Get byte length of bitstream */
	header->BitstreamLength = data[index++];
	header->BitstreamLength = (header->BitstreamLength << 8) | data[index++];
	header->BitstreamLength = (header->BitstreamLength << 8) | data[index++];
	header->BitstreamLength = (header->BitstreamLength << 8) | data[index++];
	header->HeaderLength = index;

	ICAP_INFO(icap, "Design \"%s\"", header->DesignName);
	ICAP_INFO(icap, "Part \"%s\"", header->PartName);
	ICAP_INFO(icap, "Timestamp \"%s %s\"", header->Time, header->Date);
	ICAP_INFO(icap, "Raw data size 0x%x", header->BitstreamLength);
	return 0;
}

static int bitstream_helper(struct icap *icap, const u32 *word_buffer,
	unsigned word_count)
{
	unsigned remain_word;
	unsigned word_written = 0;
	int wr_fifo_vacancy = 0;
	int err = 0;

	for (remain_word = word_count; remain_word > 0;
		remain_word -= word_written, word_buffer += word_written) {
		wr_fifo_vacancy = reg_rd(&icap->icap_regs->ir_wfv);
		if (wr_fifo_vacancy <= 0) {
			ICAP_ERR(icap, "no vacancy: %d", wr_fifo_vacancy);
			err = -EIO;
			break;
		}
		word_written = (wr_fifo_vacancy < remain_word) ?
			wr_fifo_vacancy : remain_word;
		if (icap_write(icap, word_buffer, word_written) != 0) {
			ICAP_ERR(icap, "write failed remain %d, written %d",
					remain_word, word_written);
			err = -EIO;
			break;
		}
	}

	return err;
}

static long icap_download(struct icap *icap, const char *buffer,
	unsigned long length)
{
	long err = 0;
	XHwIcap_Bit_Header bit_header = { 0 };
	unsigned numCharsRead = DMA_HWICAP_BITFILE_BUFFER_SIZE;
	unsigned byte_read;

	BUG_ON(!buffer);
	BUG_ON(!length);

	if (bitstream_parse_header(icap, buffer,
		DMA_HWICAP_BITFILE_BUFFER_SIZE, &bit_header)) {
		err = -EINVAL;
		goto free_buffers;
	}

	if ((bit_header.HeaderLength + bit_header.BitstreamLength) > length) {
		err = -EINVAL;
		goto free_buffers;
	}

	buffer += bit_header.HeaderLength;

	for (byte_read = 0; byte_read < bit_header.BitstreamLength;
		byte_read += numCharsRead) {
		numCharsRead = bit_header.BitstreamLength - byte_read;
		if (numCharsRead > DMA_HWICAP_BITFILE_BUFFER_SIZE)
			numCharsRead = DMA_HWICAP_BITFILE_BUFFER_SIZE;

		err = bitstream_helper(icap, (u32 *)buffer,
			numCharsRead / sizeof(u32));
		if (err)
			goto free_buffers;
		buffer += numCharsRead;
	}

	err = wait_for_done(icap);

free_buffers:
	kfree(bit_header.DesignName);
	kfree(bit_header.PartName);
	kfree(bit_header.Date);
	kfree(bit_header.Time);
	return err;
}

static const struct axlf_section_header *get_axlf_section_hdr(
	struct icap *icap, const struct axlf *top, enum axlf_section_kind kind)
{
	int i;
	const struct axlf_section_header *hdr = NULL;

	for (i = 0; i < top->m_header.m_numSections; i++) {
		if (top->m_sections[i].m_sectionKind == kind) {
			hdr = &top->m_sections[i];
			break;
		}
	}

	if (hdr) {
		if ((hdr->m_sectionOffset + hdr->m_sectionSize) >
			top->m_header.m_length) {
			ICAP_ERR(icap, "found section %d is invalid", kind);
			hdr = NULL;
		} else {
			ICAP_INFO(icap, "section %d offset: %llu, size: %llu",
				kind, hdr->m_sectionOffset, hdr->m_sectionSize);
		}
	} else {
		ICAP_WARN(icap, "could not find section header %d", kind);
	}

	return hdr;
}

static int alloc_and_get_axlf_section(struct icap *icap,
	const struct axlf *top, enum axlf_section_kind kind,
	void **addr, uint64_t *size)
{
	void *section = NULL;
	const struct axlf_section_header *hdr =
		get_axlf_section_hdr(icap, top, kind);

	if (hdr == NULL)
		return -EINVAL;

	section = vmalloc(hdr->m_sectionSize);
	if (section == NULL)
		return -ENOMEM;

	memcpy(section, ((const char *)top) + hdr->m_sectionOffset,
		hdr->m_sectionSize);

	*addr = section;
	*size = hdr->m_sectionSize;
	return 0;
}

static int icap_download_boot_firmware(struct platform_device *pdev)
{
	struct icap *icap = platform_get_drvdata(pdev);
	struct pci_dev *pcidev = XOCL_PL_TO_PCI_DEV(pdev);
	struct pci_dev *pcidev_user = NULL;
	xdev_handle_t xdev = xocl_get_xdev(pdev);
	int funcid = PCI_FUNC(pcidev->devfn);
	int slotid = PCI_SLOT(pcidev->devfn);
	unsigned short deviceid = pcidev->device;
	struct axlf *bin_obj_axlf;
	const struct firmware *fw, *sche_fw;
	char fw_name[256];
	XHwIcap_Bit_Header bit_header = { 0 };
	long err = 0;
	uint64_t length = 0;
	uint64_t primaryFirmwareOffset = 0;
	uint64_t primaryFirmwareLength = 0;
	uint64_t secondaryFirmwareOffset = 0;
	uint64_t secondaryFirmwareLength = 0;
	uint64_t mbBinaryOffset = 0;
	uint64_t mbBinaryLength = 0;
	const struct axlf_section_header *primaryHeader = 0;
	const struct axlf_section_header *secondaryHeader = 0;
	const struct axlf_section_header *mbHeader = 0;
	bool load_sched = false, load_mgmt = false;

	/* Can only be done from mgmt pf. */
	if (!ICAP_PRIVILEGED(icap))
		return -EPERM;

	/* Read xsabin first, if failed, try dsabin from file system. */
	if (funcid != 0) {
		pcidev_user = pci_get_slot(pcidev->bus,
			PCI_DEVFN(slotid, funcid - 1));
		if (!pcidev_user) {
			pcidev_user = pci_get_device(pcidev->vendor,
				pcidev->device + 1, NULL);
		}
		if (pcidev_user)
			deviceid = pcidev_user->device;
	}

	err = xocl_rom_find_firmware(xdev, fw_name, sizeof(fw_name),
			deviceid, &fw);
	if (err) {
		/* Give up on finding xsabin and dsabin. */
		ICAP_ERR(icap, "unable to find firmware, giving up");
		return err;
	}

	if (memcmp(fw->data, ICAP_XCLBIN_V2, sizeof(ICAP_XCLBIN_V2)) != 0) {
		ICAP_ERR(icap, "invalid firmware %s", fw_name);
		err = -EINVAL;
		goto done;
	}

	ICAP_INFO(icap, "boot_firmware in axlf format");
	bin_obj_axlf = (struct axlf *)fw->data;
	length = bin_obj_axlf->m_header.m_length;

	if (length > fw->size) {
		err = -EINVAL;
		goto done;
	}

	/* Match the xclbin with the hardware. */
	if (!xocl_verify_timestamp(xdev,
		bin_obj_axlf->m_header.m_featureRomTimeStamp)) {
		ICAP_ERR(icap, "timestamp of ROM did not match xclbin");
		err = -EINVAL;
		goto done;
	}
	ICAP_INFO(icap, "VBNV and timestamps matched");

	if (xocl_xrt_version_check(xdev, bin_obj_axlf, true)) {
		ICAP_ERR(icap, "Major version does not match xrt");
		err = -EINVAL;
		goto done;
	}
	ICAP_INFO(icap, "runtime version matched");

	/* Grab lock and touch hardware. */
	mutex_lock(&icap->icap_lock);

	if (xocl_mb_sched_on(xdev)) {
		/* Try locating the microblaze binary. */
		if (XDEV(xdev)->priv.sched_bin) {
			err = request_firmware(&sche_fw,
				XDEV(xdev)->priv.sched_bin, &pcidev->dev);
			if (!err)  {
				xocl_mb_load_sche_image(xdev, sche_fw->data,
					sche_fw->size);
				ICAP_INFO(icap, "stashed shared mb sche bin, len %ld", sche_fw->size);
				load_sched = true;
				release_firmware(sche_fw);
			}
		}
		if (!load_sched) {
			mbHeader = get_axlf_section_hdr(icap, bin_obj_axlf,
					SCHED_FIRMWARE);
			if (mbHeader) {
				mbBinaryOffset = mbHeader->m_sectionOffset;
				mbBinaryLength = mbHeader->m_sectionSize;
				xocl_mb_load_sche_image(xdev,
					fw->data + mbBinaryOffset,
					mbBinaryLength);
				ICAP_INFO(icap,
					"stashed mb sche binary, len %lld",
					mbBinaryLength);
				load_sched = true;
				err = 0;
			}
		}
	}

	if (xocl_mb_mgmt_on(xdev)) {
		/* Try locating the board mgmt binary. */
		mbHeader = get_axlf_section_hdr(icap, bin_obj_axlf, FIRMWARE);
		if (mbHeader) {
			mbBinaryOffset = mbHeader->m_sectionOffset;
			mbBinaryLength = mbHeader->m_sectionSize;
			xocl_mb_load_mgmt_image(xdev, fw->data + mbBinaryOffset,
				mbBinaryLength);
			ICAP_INFO(icap, "stashed mb mgmt binary, len %lld",
					mbBinaryLength);
			load_mgmt = true;
		}
	}

	if (load_mgmt || load_sched)
		xocl_mb_reset(xdev);

	primaryHeader = get_axlf_section_hdr(icap, bin_obj_axlf, BITSTREAM);
	secondaryHeader = get_axlf_section_hdr(icap, bin_obj_axlf,
		CLEARING_BITSTREAM);
	if (primaryHeader) {
		primaryFirmwareOffset = primaryHeader->m_sectionOffset;
		primaryFirmwareLength = primaryHeader->m_sectionSize;
	}
	if (secondaryHeader) {
		secondaryFirmwareOffset = secondaryHeader->m_sectionOffset;
		secondaryFirmwareLength = secondaryHeader->m_sectionSize;
	}

	if ((primaryFirmwareOffset + primaryFirmwareLength) > length) {
		err = -EINVAL;
		goto done;
	}

	if ((secondaryFirmwareOffset + secondaryFirmwareLength) > length) {
		err = -EINVAL;
		goto done;
	}

	if (primaryFirmwareLength) {
		ICAP_INFO(icap,
			"found second stage bitstream of size 0x%llx in %s",
			primaryFirmwareLength, fw_name);
		err = icap_download(icap, fw->data + primaryFirmwareOffset,
			primaryFirmwareLength);
		/*
		 * If we loaded a new second stage, we do not need the
		 * previously stashed clearing bitstream if any.
		 */
		free_clear_bitstream(icap);
		if (err) {
			ICAP_ERR(icap,
				"failed to download second stage bitstream");
			goto done;
		}
		ICAP_INFO(icap, "downloaded second stage bitstream");
	}

	/*
	 * If both primary and secondary bitstreams have been provided then
	 * ignore the previously stashed bitstream if any. If only secondary
	 * bitstream was provided, but we found a previously stashed bitstream
	 * we should use the latter since it is more appropriate for the
	 * current state of the device
	 */
	if (secondaryFirmwareLength && (primaryFirmwareLength ||
		!icap->icap_clear_bitstream)) {
		free_clear_bitstream(icap);
		icap->icap_clear_bitstream = vmalloc(secondaryFirmwareLength);
		if (!icap->icap_clear_bitstream) {
			err = -ENOMEM;
			goto done;
		}
		icap->icap_clear_bitstream_length = secondaryFirmwareLength;
		memcpy(icap->icap_clear_bitstream,
			fw->data + secondaryFirmwareOffset,
			icap->icap_clear_bitstream_length);
		ICAP_INFO(icap, "found clearing bitstream of size 0x%lx in %s",
			icap->icap_clear_bitstream_length, fw_name);
	} else if (icap->icap_clear_bitstream) {
		ICAP_INFO(icap,
			"using existing clearing bitstream of size 0x%lx",
		       icap->icap_clear_bitstream_length);
	}

	if (icap->icap_clear_bitstream &&
		bitstream_parse_header(icap, icap->icap_clear_bitstream,
		DMA_HWICAP_BITFILE_BUFFER_SIZE, &bit_header)) {
		err = -EINVAL;
		free_clear_bitstream(icap);
	}

done:
	mutex_unlock(&icap->icap_lock);
	release_firmware(fw);
	kfree(bit_header.DesignName);
	kfree(bit_header.PartName);
	kfree(bit_header.Date);
	kfree(bit_header.Time);
	ICAP_INFO(icap, "%s err: %ld", __func__, err);
	return err;
}


static long icap_download_clear_bitstream(struct icap *icap)
{
	long err = 0;
	const char *buffer = icap->icap_clear_bitstream;
	unsigned long length = icap->icap_clear_bitstream_length;

	ICAP_INFO(icap, "downloading clear bitstream of length 0x%lx", length);

	if (!buffer)
		return 0;

	err = icap_download(icap, buffer, length);

	free_clear_bitstream(icap);
	return err;
}

static int icap_post_download_rp(struct platform_device *pdev)
{
	struct icap *icap = platform_get_drvdata(pdev);
	xdev_handle_t xdev = xocl_get_xdev(pdev);
	bool load_mbs = false;

	if (xocl_mb_mgmt_on(xdev) && icap->rp_mgmt_bin) {
		xocl_mb_load_mgmt_image(xdev, icap->rp_mgmt_bin,
			icap->rp_mgmt_bin_len);
		ICAP_INFO(icap, "stashed mb mgmt binary, len %ld",
			icap->rp_mgmt_bin_len);
		vfree(icap->rp_mgmt_bin);
		icap->rp_mgmt_bin = NULL;
		icap->rp_mgmt_bin_len = 0;
		load_mbs = true;
	}

	if (xocl_mb_sched_on(xdev) && icap->rp_sche_bin) {
		xocl_mb_load_sche_image(xdev, icap->rp_sche_bin,
			icap->rp_sche_bin_len);
		ICAP_INFO(icap, "stashed mb sche binary, len %ld",
			icap->rp_sche_bin_len);
		vfree(icap->rp_sche_bin);
		icap->rp_sche_bin = NULL;
		icap->rp_sche_bin_len =0;
		load_mbs = true;
	}

	if (load_mbs)
		xocl_mb_reset(xdev);

	return 0;
}

static int icap_download_rp(struct platform_device *pdev, int level, int flag)
{
	struct icap *icap = platform_get_drvdata(pdev);
	xdev_handle_t xdev = xocl_get_xdev(pdev);
	struct xcl_mailbox_req mbreq = { 0 };
	int ret = 0;

	mbreq.req = XCL_MAILBOX_REQ_CHG_SHELL;
	mutex_lock(&icap->icap_lock);
	if (flag == RP_DOWNLOAD_CLEAR) {
		xocl_xdev_info(xdev, "Clear firmware bins");
		icap_free_bins(icap);
		goto end;
	}
	if (!icap->rp_bit || !icap->rp_fdt) {
		xocl_xdev_err(xdev, "Invalid reprogram request %p.%p",
			icap->rp_bit, icap->rp_fdt);
		ret = -EINVAL;
		goto failed;
	}

	if (!XDEV(xdev)->fdt_blob) {
		xocl_xdev_err(xdev, "Empty fdt blob");
		ret = -EINVAL;
		goto failed;
	}

	ret = xocl_fdt_check_uuids(xdev, icap->rp_fdt, XDEV(xdev)->fdt_blob);
	if (ret) {
		xocl_xdev_err(xdev, "Incompatible uuids");
		goto failed;
	}

	if (flag == RP_DOWNLOAD_DRY)
		goto end;
	else if (flag == RP_DOWNLOAD_NORMAL) {
		(void) xocl_peer_notify(xocl_get_xdev(icap->icap_pdev), &mbreq,
				sizeof(struct xcl_mailbox_req));
		ICAP_INFO(icap, "Notified userpf to program rp");
		goto end;
	}

	ret = xocl_fdt_blob_input(xdev, icap->rp_fdt, icap->rp_fdt_len);
	if (ret) {
		xocl_xdev_err(xdev, "failed to parse fdt %d", ret);
		goto failed;
	}

	ret = xocl_axigate_freeze(xdev, XOCL_SUBDEV_LEVEL_BLD);
	if (ret) {
		xocl_xdev_err(xdev, "freeze blp gate failed %d", ret);
		goto failed;
	}


	//wait_event_interruptible(mytestwait, false);

	reg_wr(&icap->icap_regs->ir_cr, 0x8);
	ndelay(2000);
	reg_wr(&icap->icap_regs->ir_cr, 0x0);
	ndelay(2000);
	reg_wr(&icap->icap_regs->ir_cr, 0x4);
	ndelay(2000);
	reg_wr(&icap->icap_regs->ir_cr, 0x0);
	ndelay(2000);

	ret = icap_download(icap, icap->rp_bit, icap->rp_bit_len);
	if (ret)
		goto failed;

	ret = xocl_axigate_free(xdev, XOCL_SUBDEV_LEVEL_BLD);
	if (ret) {
		xocl_xdev_err(xdev, "freeze blp gate failed %d", ret);
		goto failed;
	}

failed:
	if (icap->rp_bit) {
		vfree(icap->rp_bit);
		icap->rp_bit = NULL;
		icap->rp_bit_len = 0;
	}
	if (icap->rp_fdt) {
		vfree(icap->rp_fdt);
		icap->rp_fdt = NULL;
		icap->rp_fdt_len = 0;
	}

end:
	mutex_unlock(&icap->icap_lock);
	return ret;
}

static long axlf_set_freqscaling(struct icap *icap)
{
	struct clock_freq_topology *freqs = NULL;
	int clock_type_count = 0;
	int i = 0;
	struct clock_freq *freq = NULL;
	int data_clk_count = 0;
	int kernel_clk_count = 0;
	int system_clk_count = 0;
	unsigned short target_freqs[4] = {0};

	BUG_ON(!mutex_is_locked(&icap->icap_lock));

	if (!icap->icap_clock_freq_topology)
		return 0;

	freqs = icap->icap_clock_freq_topology;
	if (freqs->m_count > 4) {
		ICAP_ERR(icap, "More than 4 clocks found in clock topology");
		return -EDOM;
	}

	/* Error checks - we support 1 data clk (reqd), 1 kernel clock(reqd) and
	 * at most 2 system clocks (optional/reqd for aws).
	 * Data clk needs to be the first entry, followed by kernel clock
	 * and then system clocks
	 */

	for (i = 0; i < freqs->m_count; i++) {
		freq = &(freqs->m_clock_freq[i]);
		if (freq->m_type == CT_DATA)
			data_clk_count++;
		if (freq->m_type == CT_KERNEL)
			kernel_clk_count++;
		if (freq->m_type == CT_SYSTEM)
			system_clk_count++;
	}

	if (data_clk_count != 1) {
		ICAP_ERR(icap, "Data clock not found in clock topology");
		return -EDOM;
	}
	if (kernel_clk_count != 1) {
		ICAP_ERR(icap, "Kernel clock not found in clock topology");
		return -EDOM;
	}
	if (system_clk_count > 2) {
		ICAP_ERR(icap,
			"More than 2 system clocks found in clock topology");
		return -EDOM;
	}

	for (i = 0; i < freqs->m_count; i++) {
		freq = &(freqs->m_clock_freq[i]);
		if (freq->m_type == CT_DATA)
			target_freqs[0] = freq->m_freq_Mhz;
	}

	for (i = 0; i < freqs->m_count; i++) {
		freq = &(freqs->m_clock_freq[i]);
		if (freq->m_type == CT_KERNEL)
			target_freqs[1] = freq->m_freq_Mhz;
	}

	clock_type_count = 2;
	for (i = 0; i < freqs->m_count; i++) {
		freq = &(freqs->m_clock_freq[i]);
		if (freq->m_type == CT_SYSTEM)
			target_freqs[clock_type_count++] = freq->m_freq_Mhz;
	}


	ICAP_INFO(icap, "set %lu freq, data: %d, kernel: %d, sys: %d, sys1: %d",
		ARRAY_SIZE(target_freqs), target_freqs[0], target_freqs[1],
		target_freqs[2], target_freqs[3]);
	return set_freqs(icap, target_freqs, ARRAY_SIZE(target_freqs));
}


static int icap_download_hw(struct icap *icap, const char *bit_buf,
	unsigned long length)
{
	long err = 0;
	XHwIcap_Bit_Header bit_header = { 0 };
	unsigned numCharsRead = DMA_HWICAP_BITFILE_BUFFER_SIZE;
	unsigned byte_read;

	ICAP_INFO(icap, "downloading bitstream, length: %lu", length);

	icap_freeze_axi_gate(icap);

	err = icap_download_clear_bitstream(icap);
	if (err)
		goto free_buffers;

	if (bitstream_parse_header(icap, bit_buf,
		DMA_HWICAP_BITFILE_BUFFER_SIZE, &bit_header)) {
		err = -EINVAL;
		goto free_buffers;
	}
	if ((bit_header.HeaderLength + bit_header.BitstreamLength) > length) {
		err = -EINVAL;
		goto free_buffers;
	}

	bit_buf += bit_header.HeaderLength;
	for (byte_read = 0; byte_read < bit_header.BitstreamLength;
		byte_read += numCharsRead) {
		numCharsRead = bit_header.BitstreamLength - byte_read;
		if (numCharsRead > DMA_HWICAP_BITFILE_BUFFER_SIZE)
			numCharsRead = DMA_HWICAP_BITFILE_BUFFER_SIZE;

		err = bitstream_helper(icap, (u32 *)bit_buf,
			numCharsRead / sizeof(u32));
		if (err)
			goto free_buffers;

		bit_buf += numCharsRead;
	}

	err = wait_for_done(icap);
	if (err)
		goto free_buffers;

	/*
	 * Perform frequency scaling since PR download can silenty overwrite
	 * MMCM settings in static region changing the clock frequencies
	 * although ClockWiz CONFIG registers will misleading report the older
	 * configuration from before bitstream download as if nothing has
	 * changed.
	 */
	if (!err)
		err = icap_ocl_freqscaling(icap, true);

free_buffers:
	icap_free_axi_gate(icap);
	kfree(bit_header.DesignName);
	kfree(bit_header.PartName);
	kfree(bit_header.Date);
	kfree(bit_header.Time);
	return err;
}

static void icap_clean_axlf_section(struct icap *icap,
	enum axlf_section_kind kind)
{
	void **target = NULL;

	switch (kind) {
	case IP_LAYOUT:
		target = (void **)&icap->ip_layout;
		break;
	case MEM_TOPOLOGY:
		target = (void **)&icap->mem_topo;
		break;
	case DEBUG_IP_LAYOUT:
		target = (void **)&icap->debug_layout;
		break;
	case CONNECTIVITY:
		target = (void **)&icap->connectivity;
		break;
	default:
		break;
	}
	if (target) {
		vfree(*target);
		*target = NULL;
	}
}

static void icap_clean_bitstream_axlf(struct platform_device *pdev)
{
	struct icap *icap = platform_get_drvdata(pdev);

	uuid_copy(&icap->icap_bitstream_uuid, &uuid_null);
	icap_clean_axlf_section(icap, IP_LAYOUT);
	icap_clean_axlf_section(icap, MEM_TOPOLOGY);
	icap_clean_axlf_section(icap, DEBUG_IP_LAYOUT);
	icap_clean_axlf_section(icap, CONNECTIVITY);
}

static uint32_t convert_mem_type(const char *name)
{
	/* Use MEM_DDR3 as a invalid memory type. */
	enum MEM_TYPE mem_type = MEM_DDR3;

	if (!strncasecmp(name, "DDR", 3))
		mem_type = MEM_DRAM;
	else if (!strncasecmp(name, "HBM", 3))
		mem_type = MEM_HBM;
	else if (!strncasecmp(name, "bank", 4))
		mem_type = MEM_DDR4;

	return mem_type;
}

static uint16_t icap_get_memidx(struct mem_topology *mem_topo, enum MEM_TYPE mem_type,
	int idx)
{
	uint16_t memidx = INVALID_MEM_IDX, i, mem_idx = 0;
	enum MEM_TYPE m_type;

	if (!mem_topo)
		goto done;

	for (i = 0; i < mem_topo->m_count; ++i) {
		/* Don't trust m_type in xclbin, convert name to m_type instead.
		 * m_tag[i] = "HBM[0]" -> m_type = MEM_HBM
		 * m_tag[i] = "DDR[1]" -> m_type = MEM_DRAM
		 */
		m_type = convert_mem_type(mem_topo->m_mem_data[i].m_tag);
		if (m_type == mem_type) {
			if (idx == mem_idx)
				return i;
			mem_idx++;
		}
	}

done:
	return memidx;
}

static int icap_create_subdev(struct platform_device *pdev, struct axlf *xclbin)
{
	struct icap *icap = platform_get_drvdata(pdev);
	int err = 0, i = 0;
	xdev_handle_t xdev = xocl_get_xdev(pdev);
	uint64_t section_size = 0;
	struct ip_layout *ip_layout = NULL;
	struct mem_topology *mem_topo = NULL;

	if (alloc_and_get_axlf_section(icap, xclbin,
		IP_LAYOUT,
		(void **)&ip_layout, &section_size) != 0) {
		err = -EFAULT;
		goto done;
	}

	if (alloc_and_get_axlf_section(icap, xclbin,
		MEM_TOPOLOGY,
		(void **)&mem_topo, &section_size) != 0) {
		err = -EFAULT;
		goto done;
	}

	for (i = 0; i < ip_layout->m_count; ++i) {
		struct ip_data *ip = &ip_layout->m_ip_data[i];
		struct xocl_mig_label mig_label = { {0} };
		uint32_t memidx = 0;

		if (ip->m_type == IP_KERNEL)
			continue;

		if (ip->m_type == IP_DDR4_CONTROLLER || ip->m_type == IP_MEM_DDR4) {
			struct xocl_subdev_info subdev_info = XOCL_DEVINFO_MIG;
			uint32_t target_m_type;
			/*
			 * Get global memory index by feeding desired memory type and index
			 */
			if (ip->m_type == IP_MEM_DDR4)
				target_m_type = MEM_DRAM;
			else if (ip->m_type == IP_DDR4_CONTROLLER)
				target_m_type = MEM_DDR4;
			else
				continue;

			memidx = icap_get_memidx(mem_topo, target_m_type, ip->properties);

			if (memidx == INVALID_MEM_IDX) {
				ICAP_ERR(icap, "INVALID_MEM_IDX: %u",
					ip->properties);
				continue;
			}

			if (!mem_topo || memidx >= mem_topo->m_count ||
				mem_topo->m_mem_data[memidx].m_type !=
				target_m_type) {
				ICAP_ERR(icap, "bad ECC controller index: %u",
					ip->properties);
				continue;
			}
			if (!mem_topo->m_mem_data[memidx].m_used) {
				ICAP_INFO(icap,
					"ignore ECC controller for: %s",
					mem_topo->m_mem_data[memidx].m_tag);
				continue;
			}

			memcpy(&mig_label.tag, mem_topo->m_mem_data[memidx].m_tag, 16);
			mig_label.mem_idx = i;

			subdev_info.res[0].start += ip->m_base_address;
			subdev_info.res[0].end += ip->m_base_address;
			subdev_info.priv_data = &mig_label;
			subdev_info.data_len =
				sizeof(struct xocl_mig_label);

			if (!ICAP_PRIVILEGED(icap))
				subdev_info.num_res = 0;

			err = xocl_subdev_create(xdev, &subdev_info);
			if (err) {
				ICAP_ERR(icap, "can't create MIG subdev");
				goto done;
			}
		} else if (ip->m_type == IP_MEM_HBM) {
			struct xocl_subdev_info subdev_info = XOCL_DEVINFO_MIG_HBM;
			uint16_t memidx = icap_get_memidx(mem_topo, MEM_HBM, ip->indices.m_index);

			if (memidx == INVALID_MEM_IDX)
				continue;

			if (!mem_topo || memidx >= mem_topo->m_count) {
				ICAP_ERR(icap, "bad ECC controller index: %u",
					ip->properties);
				continue;
			}

			if (!mem_topo->m_mem_data[memidx].m_used) {
				ICAP_INFO(icap,
					"ignore ECC controller for: %s",
					mem_topo->m_mem_data[memidx].m_tag);
				continue;
			}

			memcpy(&mig_label.tag, mem_topo->m_mem_data[memidx].m_tag, 16);
			mig_label.mem_idx = i;

			subdev_info.res[0].start += ip->m_base_address;
			subdev_info.res[0].end += ip->m_base_address;
			subdev_info.priv_data = &mig_label;
			subdev_info.data_len =
				sizeof(struct xocl_mig_label);

			if (!ICAP_PRIVILEGED(icap))
				subdev_info.num_res = 0;

			err = xocl_subdev_create(xdev, &subdev_info);
			if (err) {
				ICAP_ERR(icap, "can't create MIG_HBM subdev");
				goto done;
			}
		} else if (ip->m_type == IP_DNASC) {
			struct xocl_subdev_info subdev_info = XOCL_DEVINFO_DNA;

			subdev_info.res[0].start += ip->m_base_address;
			subdev_info.res[0].end += ip->m_base_address;

			if (!ICAP_PRIVILEGED(icap))
				subdev_info.num_res = 0;

			err = xocl_subdev_create(xdev, &subdev_info);
			if (err) {
				ICAP_ERR(icap, "can't create DNA subdev");
				goto done;
			}
		}
	}
done:
	vfree(ip_layout);
	vfree(mem_topo);
	return err;
}

static int icap_verify_bitstream_axlf(struct platform_device *pdev,
	struct axlf *xclbin)
{
	struct icap *icap = platform_get_drvdata(pdev);
	xdev_handle_t xdev = xocl_get_xdev(pdev);
	int err = 0;
	uint64_t section_size = 0;
	u32 capability;

	/* Destroy all dynamically add sub-devices*/
	xocl_subdev_destroy_by_id(xdev, XOCL_SUBDEV_DNA);
	xocl_subdev_destroy_by_id(xdev, XOCL_SUBDEV_MIG);
	/*
	 * Add sub device dynamically.
	 * restrict any dynamically added sub-device and 1 base address,
	 * Has pre-defined length
	 *  Ex:    "ip_data": {
	 *         "m_type": "IP_DNASC",
	 *         "properties": "0x0",
	 *         "m_base_address": "0x1100000", <--  base address
	 *         "m_name": "slr0\/dna_self_check_0"
	 */

	err = icap_create_subdev(pdev, xclbin);
	if (err)
		goto done;


	/* Skip dna validation in userpf*/
	if (!ICAP_PRIVILEGED(icap))
		goto done;

	/* capability BIT8 as DRM IP enable, BIT0 as AXI mode
	 * We only check if anyone of them is set.
	 */
	capability = ((xocl_dna_capability(xdev) & 0x101) != 0);

	if (capability) {
		uint32_t *cert = NULL;

		if (0x1 & xocl_dna_status(xdev))
			goto done;
		/*
		 * Any error occurs here should return -EACCES for app to
		 * know that DNA has failed.
		 */
		err = -EACCES;

		ICAP_INFO(icap, "DNA version: %s", (capability & 0x1) ? "AXI" : "BRAM");

		if (alloc_and_get_axlf_section(icap, xclbin,
			DNA_CERTIFICATE,
			(void **)&cert, &section_size) != 0) {

			/* We keep dna sub device if IP_DNASC presents */
			ICAP_ERR(icap, "Can't get certificate section");
			goto dna_cert_fail;
		}

		ICAP_INFO(icap, "DNA Certificate Size 0x%llx", section_size);
		if (section_size % 64 || section_size < 576)
			ICAP_ERR(icap, "Invalid certificate size");
		else
			xocl_dna_write_cert(xdev, cert, section_size);

		vfree(cert);


		/* Check DNA validation result. */
		if (0x1 & xocl_dna_status(xdev))
			err = 0; /* xclbin is valid */
		else {
			ICAP_ERR(icap, "DNA inside xclbin is invalid");
			goto dna_cert_fail;
		}
	}

done:
	if (err) {
		xocl_subdev_destroy_by_id(xdev, XOCL_SUBDEV_DNA);
		xocl_subdev_destroy_by_id(xdev, XOCL_SUBDEV_MIG);
	}
dna_cert_fail:
	return err;
}

static int __icap_peer_xclbin_download(struct icap *icap, struct axlf *xclbin)
{
	xdev_handle_t xdev = xocl_get_xdev(icap->icap_pdev);
	uint64_t ch_state = 0;
	uint32_t data_len = 0;
	struct xcl_mailbox_req *mb_req = NULL;
	int msgerr = -ETIMEDOUT;
	size_t resplen = sizeof(msgerr);
	xuid_t *peer_uuid = NULL;
	struct xcl_mailbox_bitstream_kaddr mb_addr = {0};

	BUG_ON(!mutex_is_locked(&icap->icap_lock));

	/* Optimization for transferring entire xclbin thru mailbox. */
	peer_uuid = (xuid_t *)icap_get_data_nolock(icap->icap_pdev, PEER_UUID);
	if (uuid_equal(peer_uuid, &xclbin->m_header.uuid)) {
		ICAP_INFO(icap, "xclbin already on peer, skip downloading");
		return 0;
	}

	xocl_mailbox_get(xdev, CHAN_STATE, &ch_state);
	if ((ch_state & XCL_MB_PEER_SAME_DOMAIN) != 0) {
		data_len = sizeof(struct xcl_mailbox_req) +
			sizeof(struct xcl_mailbox_bitstream_kaddr);
		mb_req = vmalloc(data_len);
		if (!mb_req) {
			ICAP_ERR(icap, "can't create mb_req\n");
			return -ENOMEM;
		}
		mb_req->req = XCL_MAILBOX_REQ_LOAD_XCLBIN_KADDR;
		mb_addr.addr = (uint64_t)xclbin;
		memcpy(mb_req->data, &mb_addr,
			sizeof(struct xcl_mailbox_bitstream_kaddr));
	} else {
		data_len = sizeof(struct xcl_mailbox_req) +
			xclbin->m_header.m_length;
		mb_req = vmalloc(data_len);
		if (!mb_req) {
			ICAP_ERR(icap, "can't create mb_req\n");
			return -ENOMEM;
		}
		mb_req->req = XCL_MAILBOX_REQ_LOAD_XCLBIN;
		memcpy(mb_req->data, xclbin, xclbin->m_header.m_length);
	}

	/* Set timeout to be 1s per 2MB for downloading xclbin. */
	(void) xocl_peer_request(xdev, mb_req, data_len,
		&msgerr, &resplen, NULL, NULL,
		xclbin->m_header.m_length / (2048 * 1024));
	vfree(mb_req);

	if (msgerr != 0) {
		ICAP_ERR(icap, "peer xclbin download err: %d", msgerr);
		return msgerr;
	}

	/* Clean up and expire cache after download xclbin */
	memset(&icap->cache, 0, sizeof(struct xcl_pr_region));
	icap->cache_expires = ktime_sub(ktime_get_boottime(), ktime_set(1, 0));
	return 0;
}

static int icap_verify_signature(struct icap *icap,
	const void *data, size_t data_len, const void *sig, size_t sig_len)
{
	int ret = 0;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0)
#define	SYS_KEYS	((void *)1UL)
	ret = verify_pkcs7_signature(data, data_len, sig, sig_len,
		(icap->sec_level == ICAP_SEC_SYSTEM) ? SYS_KEYS : icap_keys,
		VERIFYING_UNSPECIFIED_SIGNATURE, NULL, NULL);
	if (ret) {
		ICAP_ERR(icap, "signature verification failed: %d", ret);
		if (icap->sec_level == ICAP_SEC_NONE) {
			/* Ignore error to allow bitstream downloading. */
			ret = 0;
		} else {
			ret = -EKEYREJECTED;
		}
	} else {
		ICAP_INFO(icap, "signature verification is done successfully");
	}
#else
	ret = -EOPNOTSUPP;
	ICAP_ERR(icap,
		"signature verification isn't supported with kernel < 4.7.0");
#endif
	return ret;
}

static int __icap_xclbin_download(struct icap *icap, struct axlf *xclbin)
{
	xdev_handle_t xdev = xocl_get_xdev(icap->icap_pdev);
	const struct axlf_section_header *primaryHeader = NULL;
	const struct axlf_section_header *clearHeader = NULL;
	uint64_t primaryFirmwareOffset = 0;
	uint64_t primaryFirmwareLength = 0;
	uint64_t clearFirmwareOffset = 0;
	uint64_t clearFirmwareLength = 0;
	char *buffer = NULL;
	long err = 0;

	BUG_ON(!mutex_is_locked(&icap->icap_lock));

	if (xclbin->m_signature_length != -1) {
		int siglen = xclbin->m_signature_length;
		u64 origlen = xclbin->m_header.m_length - siglen;

		ICAP_INFO(icap, "signed xclbin detected");
		ICAP_INFO(icap, "original size: %llu, signature size: %d",
			origlen, siglen);

		/* restore original xclbin for verification */
		xclbin->m_signature_length = -1;
		xclbin->m_header.m_length = origlen;

		err = icap_verify_signature(icap, xclbin, origlen,
			((char *)xclbin) + origlen, siglen);
		if (err)
			return err;
	} else if (icap->sec_level > ICAP_SEC_NONE) {
		ICAP_ERR(icap, "xclbin is not signed, rejected");
		return -EKEYREJECTED;
	}

	if (!XOCL_DSA_IS_SMARTN(xdev)) {
		err = icap_setup_clock_freq_topology(icap, xclbin);
		if (err)
			return err;
		err = axlf_set_freqscaling(icap);
		if (err)
			return err;
	}
	/* Download bitstream */
	primaryHeader = get_axlf_section_hdr(icap, xclbin, BITSTREAM);
	if (primaryHeader == NULL)
		return -EINVAL;
	primaryFirmwareOffset = primaryHeader->m_sectionOffset;
	primaryFirmwareLength = primaryHeader->m_sectionSize;
	buffer = (char *)xclbin;
	buffer += primaryFirmwareOffset;
	err = icap_download_hw(icap, buffer, primaryFirmwareLength);
	if (err)
		return err;

	/* Save clearing bitstream */
	clearHeader = get_axlf_section_hdr(icap, xclbin, CLEARING_BITSTREAM);
	if (clearHeader != NULL) {
		clearFirmwareOffset = clearHeader->m_sectionOffset;
		clearFirmwareLength = clearHeader->m_sectionSize;
	}
	buffer = (char *)xclbin;
	buffer += clearFirmwareOffset;
	err = icap_setup_clear_bitstream(icap, buffer, clearFirmwareLength);
	if (err)
		return err;

	/* Wait for mig recalibration */
	if ((xocl_is_unified(xdev) || XOCL_DSA_XPR_ON(xdev)))
		err = calibrate_mig(icap);

	return err;
}

static int icap_download_bitstream_axlf(struct platform_device *pdev,
	const void *u_xclbin)
{
	struct icap *icap = platform_get_drvdata(pdev);
	struct axlf *xclbin = (struct axlf *)u_xclbin;
	int err = 0;
	xdev_handle_t xdev = xocl_get_xdev(pdev);
	const struct axlf_section_header *dtbHeader = NULL;

	/* Sanity check xclbin. */
	if (memcmp(xclbin->m_magic, ICAP_XCLBIN_V2, sizeof(ICAP_XCLBIN_V2))) {
		ICAP_ERR(icap, "invalid xclbin magic string");
		return -EINVAL;
	}

	dtbHeader = get_axlf_section_hdr(icap, xclbin, PARTITION_METADATA);
	if (dtbHeader) {
		ICAP_INFO(icap, "check interface uuid");
		if (!XDEV(xdev)->fdt_blob) {
			ICAP_ERR(icap, "did not find platform dtb");
			return -EINVAL;
		}
		err = xocl_fdt_check_uuids(xdev,
				(const void *)XDEV(xdev)->fdt_blob,
				(const void *)((char *)xclbin +
				dtbHeader->m_sectionOffset));
		if (err) {
			ICAP_ERR(icap, "interface uuids do not match");
			return -EINVAL;
		}
	}

	if (xocl_xrt_version_check(xdev, xclbin, true)) {
		ICAP_ERR(icap, "xclbin isn't supported by current XRT");
		return -EINVAL;
	}
	if (!xocl_verify_timestamp(xdev,
		xclbin->m_header.m_featureRomTimeStamp)) {
		ICAP_ERR(icap, "TimeStamp of ROM did not match Xclbin");
		return -EOPNOTSUPP;
	}

	mutex_lock(&icap->icap_lock);

	ICAP_INFO(icap, "incoming xclbin: %pUb\non device xclbin: %pUb",
		&xclbin->m_header.uuid, &icap->icap_bitstream_uuid);

	if (icap_bitstream_in_use(icap)) {
		ICAP_ERR(icap, "bitstream is in-use, can't change");
		err = -EBUSY;
		goto done;
	}

	if (ICAP_PRIVILEGED(icap)) {
		err = __icap_xclbin_download(icap, xclbin);
		if (err)
			goto done;

		icap_parse_bitstream_axlf_section(pdev, xclbin, MEM_TOPOLOGY);
		icap_parse_bitstream_axlf_section(pdev, xclbin, IP_LAYOUT);
		err = icap_verify_bitstream_axlf(pdev, xclbin);
		if (err)
			goto done;
	} else {
		err = __icap_peer_xclbin_download(icap, xclbin);
		/*
		 * xclbin download changes PR region, make sure next
		 * ERT configure cmd will go through
		 */
		(void) xocl_exec_reconfig(xdev);

		icap_parse_bitstream_axlf_section(pdev, xclbin, IP_LAYOUT);
		icap_parse_bitstream_axlf_section(pdev, xclbin, MEM_TOPOLOGY);
		icap_parse_bitstream_axlf_section(pdev, xclbin, CONNECTIVITY);
		icap_parse_bitstream_axlf_section(pdev, xclbin,
			DEBUG_IP_LAYOUT);
		icap_setup_clock_freq_topology(icap, xclbin);
		/* not really doing verification, but just create subdevs */
		(void) icap_verify_bitstream_axlf(pdev, xclbin);
	}

done:
	if (err) {
		icap_clean_bitstream_axlf(pdev);
	} else {
		/* Remember "this" bitstream, so avoid redownload next time. */
		uuid_copy(&icap->icap_bitstream_uuid, &xclbin->m_header.uuid);
	}
	mutex_unlock(&icap->icap_lock);
	ICAP_INFO(icap, "%s err: %d", __func__, err);
	return err;
}

/*
 * On x86_64, reset hwicap by loading special bitstream sequence which
 * forces the FPGA to reload from PROM.
 */
static int icap_reset_bitstream(struct platform_device *pdev)
{
/*
 * Booting FPGA from PROM
 * http://www.xilinx.com/support/documentation/user_guides/ug470_7Series_Config.pdf
 * Table 7.1
 */
#define DUMMY_WORD         0xFFFFFFFF
#define SYNC_WORD          0xAA995566
#define TYPE1_NOOP         0x20000000
#define TYPE1_WRITE_WBSTAR 0x30020001
#define WBSTAR_ADD10       0x00000000
#define WBSTAR_ADD11       0x01000000
#define TYPE1_WRITE_CMD    0x30008001
#define IPROG_CMD          0x0000000F
#define SWAP_ENDIAN_32(x)						\
	(unsigned)((((x) & 0xFF000000) >> 24) | (((x) & 0x00FF0000) >> 8) | \
		   (((x) & 0x0000FF00) << 8)  | (((x) & 0x000000FF) << 24))
	/*
	 * The bitstream is expected in big endian format
	 */
	const unsigned fpga_boot_seq[] = {				\
		SWAP_ENDIAN_32(DUMMY_WORD),				\
		SWAP_ENDIAN_32(SYNC_WORD),				\
		SWAP_ENDIAN_32(TYPE1_NOOP),				\
		SWAP_ENDIAN_32(TYPE1_WRITE_CMD),			\
		SWAP_ENDIAN_32(IPROG_CMD),				\
		SWAP_ENDIAN_32(TYPE1_NOOP),				\
		SWAP_ENDIAN_32(TYPE1_NOOP)				\
	};
	struct icap *icap = platform_get_drvdata(pdev);
	int i;

	/* Can only be done from mgmt pf. */
	if (!ICAP_PRIVILEGED(icap))
		return -EPERM;

	mutex_lock(&icap->icap_lock);

	if (icap_bitstream_in_use(icap)) {
		mutex_unlock(&icap->icap_lock);
		ICAP_ERR(icap, "bitstream is locked, can't reset");
		return -EBUSY;
	}

	for (i = 0; i < ARRAY_SIZE(fpga_boot_seq); i++) {
		unsigned value = be32_to_cpu(fpga_boot_seq[i]);

		reg_wr(&icap->icap_regs->ir_wfv, value);
	}
	reg_wr(&icap->icap_regs->ir_cr, 0x1);

	msleep(4000);

	mutex_unlock(&icap->icap_lock);

	ICAP_INFO(icap, "reset bitstream is done");
	return 0;
}

static int icap_lock_bitstream(struct platform_device *pdev, const xuid_t *id)
{
	struct icap *icap = platform_get_drvdata(pdev);
	int ref = 0;

	BUG_ON(uuid_is_null(id));

	mutex_lock(&icap->icap_lock);

	if (!uuid_equal(id, &icap->icap_bitstream_uuid)) {
		ICAP_ERR(icap, "lock bitstream %pUb failed, on device: %pUb",
			id, &icap->icap_bitstream_uuid);
		mutex_unlock(&icap->icap_lock);
		return -EBUSY;
	}

	ref = icap->icap_bitstream_ref;
	icap->icap_bitstream_ref++;
	ICAP_INFO(icap, "bitstream %pUb locked, ref=%d", id,
		icap->icap_bitstream_ref);

	if (ref == 0) {
		/* reset on first reference */
		xocl_exec_reset(xocl_get_xdev(pdev), id);
	}

	mutex_unlock(&icap->icap_lock);
	return 0;
}

static int icap_unlock_bitstream(struct platform_device *pdev, const xuid_t *id)
{
	struct icap *icap = platform_get_drvdata(pdev);
	int err = 0;

	if (id == NULL)
		id = &uuid_null;

	mutex_lock(&icap->icap_lock);

	if (uuid_is_null(id)) /* force unlock all */
		icap->icap_bitstream_ref = 0;
	else if (uuid_equal(id, &icap->icap_bitstream_uuid))
		icap->icap_bitstream_ref--;
	else
		err = -EINVAL;
	if (err == 0) {
		ICAP_INFO(icap, "bitstream %pUb unlocked, ref=%d",
			&icap->icap_bitstream_uuid, icap->icap_bitstream_ref);
	} else {
		ICAP_ERR(icap, "unlock bitstream %pUb failed, on device: %pUb",
			id, &icap->icap_bitstream_uuid);
		mutex_unlock(&icap->icap_lock);
		return err;
	}

	if (icap->icap_bitstream_ref == 0 && !ICAP_PRIVILEGED(icap))
		(void) xocl_exec_stop(xocl_get_xdev(pdev));

	mutex_unlock(&icap->icap_lock);

	return 0;
}

static int icap_parse_bitstream_axlf_section(struct platform_device *pdev,
	const struct axlf *xclbin, enum axlf_section_kind kind)
{
	struct icap *icap = platform_get_drvdata(pdev);
	long err = 0;
	uint64_t section_size = 0, sect_sz = 0;
	void **target = NULL;

	if (memcmp(xclbin->m_magic, ICAP_XCLBIN_V2, sizeof(ICAP_XCLBIN_V2)))
		return -EINVAL;

	switch (kind) {
	case IP_LAYOUT:
		target = (void **)&icap->ip_layout;
		break;
	case MEM_TOPOLOGY:
		target = (void **)&icap->mem_topo;
		break;
	case DEBUG_IP_LAYOUT:
		target = (void **)&icap->debug_layout;
		break;
	case CONNECTIVITY:
		target = (void **)&icap->connectivity;
		break;
	case CLOCK_FREQ_TOPOLOGY:
		target = (void **)&icap->icap_clock_freq_topology;
		break;
	default:
		return -EINVAL;
	}
	if (target) {
		vfree(*target);
		*target = NULL;
	}

	err = alloc_and_get_axlf_section(icap, xclbin, kind,
		target, &section_size);
	if (err != 0)
		goto done;
	sect_sz = icap_get_section_size(icap, kind);
	if (sect_sz > section_size) {
		err = -EINVAL;
		goto done;
	}

done:
	if (err) {
		vfree(*target);
		*target = NULL;
	}
	ICAP_INFO(icap, "%s kind %d, err: %ld", __func__, kind, err);
	return err;
}

static uint64_t icap_get_data_nolock(struct platform_device *pdev,
	enum data_kind kind)
{
	struct icap *icap = platform_get_drvdata(pdev);
	ktime_t now = ktime_get_boottime();
	uint64_t target = 0;

	if (!ICAP_PRIVILEGED(icap)) {

		if (ktime_compare(now, icap->cache_expires) > 0)
			icap_read_from_peer(pdev);

		switch (kind) {
		case IPLAYOUT_AXLF:
			target = (uint64_t)icap->ip_layout;
			break;
		case MEMTOPO_AXLF:
			target = (uint64_t)icap->mem_topo;
			break;
		case DEBUG_IPLAYOUT_AXLF:
			target = (uint64_t)icap->debug_layout;
			break;
		case CONNECTIVITY_AXLF:
			target = (uint64_t)icap->connectivity;
			break;
		case XCLBIN_UUID:
			target = (uint64_t)&icap->icap_bitstream_uuid;
			break;
		case CLOCK_FREQ_0:
			target = icap->cache.freq_0;
			break;
		case CLOCK_FREQ_1:
			target = icap->cache.freq_1;
			break;
		case CLOCK_FREQ_2:
			target = icap->cache.freq_2;
			break;
		case FREQ_COUNTER_0:
			target = icap->cache.freq_cntr_0;
			break;
		case FREQ_COUNTER_1:
			target = icap->cache.freq_cntr_1;
			break;
		case FREQ_COUNTER_2:
			target = icap->cache.freq_cntr_2;
			break;
		case IDCODE:
			target = icap->cache.idcode;
			break;
		case PEER_UUID:
			target = (uint64_t)&icap->cache.uuid;
			break;
		case MIG_CALIB:
			target = (uint64_t)icap->cache.mig_calib;
			break;
		default:
			break;
		}


	} else {
		switch (kind) {
		case IPLAYOUT_AXLF:
			target = (uint64_t)icap->ip_layout;
			break;
		case MEMTOPO_AXLF:
			target = (uint64_t)icap->mem_topo;
			break;
		case DEBUG_IPLAYOUT_AXLF:
			target = (uint64_t)icap->debug_layout;
			break;
		case CONNECTIVITY_AXLF:
			target = (uint64_t)icap->connectivity;
			break;
		case IDCODE:
			target = icap->idcode;
			break;
		case XCLBIN_UUID:
			target = (uint64_t)&icap->icap_bitstream_uuid;
			break;
		case CLOCK_FREQ_0:
			target = icap_get_ocl_frequency(icap, 0);
			break;
		case CLOCK_FREQ_1:
			target = icap_get_ocl_frequency(icap, 1);
			break;
		case CLOCK_FREQ_2:
			target = icap_get_ocl_frequency(icap, 2);
			break;
		case FREQ_COUNTER_0:
			target = icap_get_clock_frequency_counter_khz(icap, 0);
			break;
		case FREQ_COUNTER_1:
			target = icap_get_clock_frequency_counter_khz(icap, 1);
			break;
		case FREQ_COUNTER_2:
			target = icap_get_clock_frequency_counter_khz(icap, 2);
			break;
		case MIG_CALIB:
			target = mig_calibration_done(icap);
			break;
		default:
			break;
		}
	}
	return target;
}
static uint64_t icap_get_data(struct platform_device *pdev,
	enum data_kind kind)
{
	struct icap *icap = platform_get_drvdata(pdev);
	uint64_t target = 0;

	mutex_lock(&icap->icap_lock);
	target = icap_get_data_nolock(pdev, kind);
	mutex_unlock(&icap->icap_lock);
	return target;
}

static void icap_refresh_addrs(struct platform_device *pdev)
{
	struct icap *icap = platform_get_drvdata(pdev);
	xdev_handle_t xdev = xocl_get_xdev(pdev);

	icap->icap_state = xocl_iores_get_base(xdev, IORES_MEMCALIB);
	ICAP_INFO(icap, "memcalib @ %lx", (unsigned long)icap->icap_state);
	icap->icap_axi_gate = xocl_iores_get_base(xdev, IORES_GATEPRPRP);
	ICAP_INFO(icap, "axi_gate @ %lx", (unsigned long)icap->icap_axi_gate);
	icap->icap_clock_bases[0] =
		xocl_iores_get_base(xdev, IORES_CLKWIZKERNEL1);
	ICAP_INFO(icap, "clk0 @ %lx", (unsigned long)icap->icap_clock_bases[0]);
	icap->icap_clock_bases[1] =
		xocl_iores_get_base(xdev, IORES_CLKWIZKERNEL2);
	ICAP_INFO(icap, "clk1 @ %lx", (unsigned long)icap->icap_clock_bases[1]);
	icap->icap_clock_bases[2] =
		xocl_iores_get_base(xdev, IORES_CLKWIZKERNEL3);
	ICAP_INFO(icap, "clk2 @ %lx", (unsigned long)icap->icap_clock_bases[2]);
	icap->icap_clock_freq_counter =
		xocl_iores_get_base(xdev, IORES_CLKFREQ1);
	ICAP_INFO(icap, "freq0 @ %lx",
			(unsigned long)icap->icap_clock_freq_counter);
	icap->icap_clock_freq_counter_hbm =
		xocl_iores_get_base(xdev, IORES_CLKFREQ2);
	ICAP_INFO(icap, "freq1 @ %lx",
			(unsigned long)icap->icap_clock_freq_counter_hbm);
}

static int icap_offline(struct platform_device *pdev)
{
	struct icap *icap = platform_get_drvdata(pdev);

	xocl_drvinst_kill_proc(platform_get_drvdata(pdev));

	sysfs_remove_group(&pdev->dev.kobj, &icap_attr_group);
	free_clear_bitstream(icap);
	free_clock_freq_topology(icap);

	icap_clean_bitstream_axlf(pdev);

	return 0;
}

static int icap_online(struct platform_device *pdev)
{
	struct icap *icap = platform_get_drvdata(pdev);
	int ret;

	icap_refresh_addrs(pdev);
	ret = sysfs_create_group(&pdev->dev.kobj, &icap_attr_group);
	if (ret)
		ICAP_ERR(icap, "create icap attrs failed: %d", ret);

	return ret;
}

/* Kernel APIs exported from this sub-device driver. */
static struct xocl_icap_funcs icap_ops = {
	.offline_cb = icap_offline,
	.online_cb = icap_online,
	.reset_axi_gate = platform_reset_axi_gate,
	.reset_bitstream = icap_reset_bitstream,
	.download_boot_firmware = icap_download_boot_firmware,
	.download_bitstream_axlf = icap_download_bitstream_axlf,
	.download_rp = icap_download_rp,
	.post_download_rp = icap_post_download_rp,
	.ocl_set_freq = icap_ocl_set_freqscaling,
	.ocl_get_freq = icap_ocl_get_freqscaling,
	.ocl_update_clock_freq_topology = icap_ocl_update_clock_freq_topology,
	.ocl_lock_bitstream = icap_lock_bitstream,
	.ocl_unlock_bitstream = icap_unlock_bitstream,
	.get_data = icap_get_data,
};

static ssize_t clock_freqs_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct icap *icap = platform_get_drvdata(to_platform_device(dev));
	ssize_t cnt = 0;
	int i;
	u32 freq_counter, freq, request_in_khz, tolerance;

	mutex_lock(&icap->icap_lock);

	for (i = 0; i < ICAP_MAX_NUM_CLOCKS; i++) {
		freq = icap_get_ocl_frequency(icap, i);
		if (!uuid_is_null(&icap->icap_bitstream_uuid)) {
			freq_counter = icap_get_clock_frequency_counter_khz(icap, i);

			request_in_khz = freq*1000;
			tolerance = freq*50;

			if (abs(freq_counter-request_in_khz) > tolerance)
				ICAP_INFO(icap, "Frequency mismatch, Should be %u khz, Now is %ukhz", request_in_khz, freq_counter);
			cnt += sprintf(buf + cnt, "%d\n", DIV_ROUND_CLOSEST(freq_counter, 1000));
		} else
			cnt += sprintf(buf + cnt, "%d\n", freq);
	}

	mutex_unlock(&icap->icap_lock);

	return cnt;
}
static DEVICE_ATTR_RO(clock_freqs);

static ssize_t clock_freqs_max_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct icap *icap = platform_get_drvdata(to_platform_device(dev));
	ssize_t cnt = 0;
	int i;
	unsigned short freq;

	mutex_lock(&icap->icap_lock);

	for (i = 0; i < ICAP_MAX_NUM_CLOCKS; i++) {
		freq = 0;
		icap_get_ocl_frequency_max_min(icap, i, &freq, NULL);
		cnt += sprintf(buf + cnt, "%d\n", freq);
	}

	mutex_unlock(&icap->icap_lock);

	return cnt;
}
static DEVICE_ATTR_RO(clock_freqs_max);

static ssize_t clock_freqs_min_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct icap *icap = platform_get_drvdata(to_platform_device(dev));
	ssize_t cnt = 0;
	int i;
	unsigned short freq;

	mutex_lock(&icap->icap_lock);

	for (i = 0; i < ICAP_MAX_NUM_CLOCKS; i++) {
		freq = 0;
		icap_get_ocl_frequency_max_min(icap, i, NULL, &freq);
		cnt += sprintf(buf + cnt, "%d\n", freq);
	}
	mutex_unlock(&icap->icap_lock);

	return cnt;
}
static DEVICE_ATTR_RO(clock_freqs_min);

static ssize_t idcode_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct icap *icap = platform_get_drvdata(to_platform_device(dev));
	ssize_t cnt = 0;
	uint32_t val;

	mutex_lock(&icap->icap_lock);
	if (ICAP_PRIVILEGED(icap)) {
		cnt = sprintf(buf, "0x%x\n", icap->idcode);
	} else {
		val = icap_get_data_nolock(to_platform_device(dev), IDCODE);
		cnt = sprintf(buf, "0x%x\n", val);
	}
	mutex_unlock(&icap->icap_lock);

	return cnt;
}

static DEVICE_ATTR_RO(idcode);


static ssize_t cache_expire_secs_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct icap *icap = platform_get_drvdata(to_platform_device(dev));
	u64 val = 0;

	mutex_lock(&icap->icap_lock);
	if (!ICAP_PRIVILEGED(icap))
		val = icap->cache_expire_secs;

	mutex_unlock(&icap->icap_lock);
	return sprintf(buf, "%llu\n", val);
}

static ssize_t cache_expire_secs_store(struct device *dev,
	struct device_attribute *da, const char *buf, size_t count)
{
	struct icap *icap = platform_get_drvdata(to_platform_device(dev));
	u64 val;

	mutex_lock(&icap->icap_lock);
	if (kstrtou64(buf, 10, &val) == -EINVAL || val > 10) {
		xocl_err(&to_platform_device(dev)->dev,
			"usage: echo [0 ~ 10] > cache_expire_secs");
		return -EINVAL;
	}

	if (!ICAP_PRIVILEGED(icap))
		icap->cache_expire_secs = val;

	mutex_unlock(&icap->icap_lock);
	return count;
}
static DEVICE_ATTR_RW(cache_expire_secs);

#ifdef	KEY_DEBUG
/* Test code for now, will remove later. */
void icap_key_test(struct icap *icap)
{
	struct pci_dev *pcidev = XOCL_PL_TO_PCI_DEV(icap->icap_pdev);
	const struct firmware *sig = NULL;
	const struct firmware *text = NULL;
	int err = 0;

	err = request_firmware(&sig, "xilinx/signature", &pcidev->dev);
	if (err) {
		ICAP_ERR(icap, "can't load signature: %d", err);
		goto done;
	}
	err = request_firmware(&text, "xilinx/text", &pcidev->dev);
	if (err) {
		ICAP_ERR(icap, "can't load text: %d", err);
		goto done;
	}

	err = icap_verify_signature(icap, text->data, text->size,
		sig->data, sig->size);
	if (err) {
		ICAP_ERR(icap, "Failed to verify data file");
		goto done;
	}

	ICAP_INFO(icap, "Successfully verified data file!!!");

done:
	if (sig)
		release_firmware(sig);
	if (text)
		release_firmware(text);
}
#endif

static ssize_t sec_level_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct icap *icap = platform_get_drvdata(to_platform_device(dev));
	u64 val = 0;

	mutex_lock(&icap->icap_lock);
	if (!ICAP_PRIVILEGED(icap))
		val = ICAP_SEC_NONE;
	else
		val = icap->sec_level;
	mutex_unlock(&icap->icap_lock);
	return sprintf(buf, "%llu\n", val);
}

static ssize_t sec_level_store(struct device *dev,
	struct device_attribute *da, const char *buf, size_t count)
{
	struct icap *icap = platform_get_drvdata(to_platform_device(dev));
	u64 val;
	int ret = count;

	if (kstrtou64(buf, 10, &val) == -EINVAL || val > ICAP_SEC_MAX) {
		xocl_err(&to_platform_device(dev)->dev,
			"max sec level is %d", ICAP_SEC_MAX);
		return -EINVAL;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 7, 0)
	if (val == 0)
		return ret;
	/* Can't enable xclbin signature verification. */
	ICAP_ERR(icap,
		"verifying signed xclbin is not supported with < 4.7.0 kernel");
	return -EOPNOTSUPP;
#else
	mutex_lock(&icap->icap_lock);

	if (ICAP_PRIVILEGED(icap)) {
#if defined(EFI_SECURE_BOOT) 
		if (!efi_enabled(EFI_SECURE_BOOT)) {
			icap->sec_level = val;
		} else {
			ICAP_ERR(icap,
				"security level is fixed in secure boot");
			ret = -EROFS;
		}
#else
		icap->sec_level = val;
#endif

#ifdef	KEY_DEBUG
		icap_key_test(icap);
#endif
	}

	mutex_unlock(&icap->icap_lock);

	return ret;
#endif
}
static DEVICE_ATTR_RW(sec_level);

static struct attribute *icap_attrs[] = {
	&dev_attr_clock_freqs.attr,
	&dev_attr_idcode.attr,
	&dev_attr_cache_expire_secs.attr,
	&dev_attr_sec_level.attr,
	&dev_attr_clock_freqs_max.attr,
	&dev_attr_clock_freqs_min.attr,
	NULL,
};

/*- Debug IP_layout-- */
static ssize_t icap_read_debug_ip_layout(struct file *filp, struct kobject *kobj,
	struct bin_attribute *attr, char *buffer, loff_t offset, size_t count)
{
	struct icap *icap;
	u32 nread = 0;
	size_t size = 0;

	icap = (struct icap *)dev_get_drvdata(container_of(kobj, struct device, kobj));

	if (!icap || !icap->debug_layout)
		return 0;

	mutex_lock(&icap->icap_lock);

	size = sizeof_sect(icap->debug_layout, m_debug_ip_data);
	if (offset >= size)
		goto unlock;

	if (count < size - offset)
		nread = count;
	else
		nread = size - offset;

	memcpy(buffer, ((char *)icap->debug_layout) + offset, nread);

unlock:
	mutex_unlock(&icap->icap_lock);
	return nread;
}
static struct bin_attribute debug_ip_layout_attr = {
	.attr = {
		.name = "debug_ip_layout",
		.mode = 0444
	},
	.read = icap_read_debug_ip_layout,
	.write = NULL,
	.size = 0
};

/* IP layout */
static ssize_t icap_read_ip_layout(struct file *filp, struct kobject *kobj,
	struct bin_attribute *attr, char *buffer, loff_t offset, size_t count)
{
	struct icap *icap;
	u32 nread = 0;
	size_t size = 0;

	icap = (struct icap *)dev_get_drvdata(container_of(kobj, struct device, kobj));

	if (!icap || !icap->ip_layout)
		return 0;

	mutex_lock(&icap->icap_lock);

	size = sizeof_sect(icap->ip_layout, m_ip_data);
	if (offset >= size)
		goto unlock;

	if (count < size - offset)
		nread = count;
	else
		nread = size - offset;

	memcpy(buffer, ((char *)icap->ip_layout) + offset, nread);

unlock:
	mutex_unlock(&icap->icap_lock);
	return nread;
}

static struct bin_attribute ip_layout_attr = {
	.attr = {
		.name = "ip_layout",
		.mode = 0444
	},
	.read = icap_read_ip_layout,
	.write = NULL,
	.size = 0
};

/* -Connectivity-- */
static ssize_t icap_read_connectivity(struct file *filp, struct kobject *kobj,
	struct bin_attribute *attr, char *buffer, loff_t offset, size_t count)
{
	struct icap *icap;
	u32 nread = 0;
	size_t size = 0;

	icap = (struct icap *)dev_get_drvdata(container_of(kobj, struct device, kobj));

	if (!icap || !icap->connectivity)
		return 0;

	mutex_lock(&icap->icap_lock);

	size = sizeof_sect(icap->connectivity, m_connection);
	if (offset >= size)
		goto unlock;

	if (count < size - offset)
		nread = count;
	else
		nread = size - offset;

	memcpy(buffer, ((char *)icap->connectivity) + offset, nread);

unlock:
	mutex_unlock(&icap->icap_lock);
	return nread;
}

static struct bin_attribute connectivity_attr = {
	.attr = {
		.name = "connectivity",
		.mode = 0444
	},
	.read = icap_read_connectivity,
	.write = NULL,
	.size = 0
};


/* -Mem_topology-- */
static ssize_t icap_read_mem_topology(struct file *filp, struct kobject *kobj,
	struct bin_attribute *attr, char *buffer, loff_t offset, size_t count)
{
	struct icap *icap;
	u32 nread = 0;
	size_t size = 0;

	icap = (struct icap *)dev_get_drvdata(container_of(kobj, struct device, kobj));

	if (!icap || !icap->mem_topo)
		return 0;

	mutex_lock(&icap->icap_lock);

	size = sizeof_sect(icap->mem_topo, m_mem_data);
	if (offset >= size)
		goto unlock;

	if (count < size - offset)
		nread = count;
	else
		nread = size - offset;

	memcpy(buffer, ((char *)icap->mem_topo) + offset, nread);
unlock:
	mutex_unlock(&icap->icap_lock);
	return nread;
}


static struct bin_attribute mem_topology_attr = {
	.attr = {
		.name = "mem_topology",
		.mode = 0444
	},
	.read = icap_read_mem_topology,
	.write = NULL,
	.size = 0
};

/* -Mem_topology-- */
static ssize_t icap_read_clock_freqs(struct file *filp, struct kobject *kobj,
	struct bin_attribute *attr, char *buffer, loff_t offset, size_t count)
{
	struct icap *icap;
	u32 nread = 0;
	size_t size = 0;

	icap = (struct icap *)dev_get_drvdata(container_of(kobj, struct device, kobj));

	if (!icap || !icap->icap_clock_freq_topology)
		return 0;

	mutex_lock(&icap->icap_lock);

	size = sizeof_sect(icap->icap_clock_freq_topology, m_clock_freq);
	if (offset >= size)
		goto unlock;

	if (count < size - offset)
		nread = count;
	else
		nread = size - offset;

	memcpy(buffer, ((char *)icap->icap_clock_freq_topology) + offset, nread);
unlock:
	mutex_unlock(&icap->icap_lock);
	return nread;
}


static struct bin_attribute clock_freq_topology_attr = {
	.attr = {
		.name = "clock_freq_topology",
		.mode = 0444
	},
	.read = icap_read_clock_freqs,
	.write = NULL,
	.size = 0
};

static ssize_t rp_bit_output(struct file *filp, struct kobject *kobj,
		struct bin_attribute *attr, char *buf, loff_t off, size_t count)
{
	struct icap *icap;
	ssize_t ret = 0;

	icap = (struct icap *)dev_get_drvdata(container_of(kobj,
				struct device, kobj));
	if (!icap || !icap->rp_bit)
		return 0;

	if (off >= icap->rp_bit_len)
		goto bail;

	if (off + count > icap->rp_bit_len)
		count = icap->rp_bit_len - off;

	memcpy(buf, icap->rp_bit + off, count);

	ret = count;

bail:
	return ret;
}

static struct bin_attribute rp_bit_attr = {
	.attr = {
		.name = "rp_bit",
		.mode = 0400
	},
	.read = rp_bit_output,
	.size = 0
};

static struct bin_attribute *icap_bin_attrs[] = {
	&debug_ip_layout_attr,
	&ip_layout_attr,
	&connectivity_attr,
	&mem_topology_attr,
	&rp_bit_attr,
	&clock_freq_topology_attr,
	NULL,
};

static struct attribute_group icap_attr_group = {
	.attrs = icap_attrs,
	.bin_attrs = icap_bin_attrs,
};

static int icap_load_keyring(void)
{
	int ret = 0;

	mutex_lock(&icap_keyring_lock);
	BUG_ON(icap_key_users < 0);

	if (icap_key_users) {
		/* Not first user, just bump up the ref cnt. */
		key_get(icap_keys);
		icap_key_users++;
	} else {
		/* First user, alloc our keyring. */
		BUG_ON(icap_keys != NULL);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0)
		icap_keys = keyring_alloc(".xilinx_fpga_xclbin_keys",
			KUIDT_INIT(0), KGIDT_INIT(0), current_cred(),
			((KEY_POS_ALL & ~KEY_POS_SETATTR) |
			KEY_USR_VIEW | KEY_USR_WRITE | KEY_USR_SEARCH),
			KEY_ALLOC_NOT_IN_QUOTA, NULL, NULL);
		if (IS_ERR(icap_keys)) {
			ret = PTR_ERR(icap_keys);
			icap_keys = NULL;
		} else {
			icap_key_users = 1;
		}
#endif
	}

	mutex_unlock(&icap_keyring_lock);
	return ret;
}

static void icap_release_keyring(void)
{
	mutex_lock(&icap_keyring_lock);
	BUG_ON(icap_key_users < 0);

	if (icap_key_users) {
		icap_key_users--;
		key_put(icap_keys);
		/*
		 * After key_put(), our key ring will be automatically freed
		 * some time later. Make sure icap_keys is cleared.
		 */
		if (icap_key_users == 0)
			icap_keys = NULL;
	}

	mutex_unlock(&icap_keyring_lock);
}

static int icap_remove(struct platform_device *pdev)
{
	struct icap *icap = platform_get_drvdata(pdev);

	BUG_ON(icap == NULL);

	icap_free_bins(icap);

	icap_release_keyring();

	iounmap(icap->icap_regs);
	free_clear_bitstream(icap);
	free_clock_freq_topology(icap);

	if (icap->sysfs_created)
		sysfs_remove_group(&pdev->dev.kobj, &icap_attr_group);

	ICAP_INFO(icap, "cleaned up successfully");
	platform_set_drvdata(pdev, NULL);
	vfree(icap->mem_topo);
	vfree(icap->ip_layout);
	vfree(icap->debug_layout);
	vfree(icap->connectivity);
	xocl_drvinst_free(icap);
	return 0;
}

/*
 * Run the following sequence of canned commands to obtain IDCODE of the FPGA
 */
static void icap_probe_chip(struct icap *icap)
{
	u32 w;

	if (!ICAP_PRIVILEGED(icap))
		return;

	w = reg_rd(&icap->icap_regs->ir_sr);
	w = reg_rd(&icap->icap_regs->ir_sr);
	reg_wr(&icap->icap_regs->ir_gier, 0x0);
	w = reg_rd(&icap->icap_regs->ir_wfv);
	reg_wr(&icap->icap_regs->ir_wf, 0xffffffff);
	reg_wr(&icap->icap_regs->ir_wf, 0xaa995566);
	reg_wr(&icap->icap_regs->ir_wf, 0x20000000);
	reg_wr(&icap->icap_regs->ir_wf, 0x20000000);
	reg_wr(&icap->icap_regs->ir_wf, 0x28018001);
	reg_wr(&icap->icap_regs->ir_wf, 0x20000000);
	reg_wr(&icap->icap_regs->ir_wf, 0x20000000);
	w = reg_rd(&icap->icap_regs->ir_cr);
	reg_wr(&icap->icap_regs->ir_cr, 0x1);
	w = reg_rd(&icap->icap_regs->ir_cr);
	w = reg_rd(&icap->icap_regs->ir_cr);
	w = reg_rd(&icap->icap_regs->ir_sr);
	w = reg_rd(&icap->icap_regs->ir_cr);
	w = reg_rd(&icap->icap_regs->ir_sr);
	reg_wr(&icap->icap_regs->ir_sz, 0x1);
	w = reg_rd(&icap->icap_regs->ir_cr);
	reg_wr(&icap->icap_regs->ir_cr, 0x2);
	w = reg_rd(&icap->icap_regs->ir_rfo);
	icap->idcode = reg_rd(&icap->icap_regs->ir_rf);
	w = reg_rd(&icap->icap_regs->ir_cr);
}

static int icap_probe(struct platform_device *pdev)
{
	struct icap *icap = NULL;
	struct resource *res;
	int ret;
	void **regs;

	icap = xocl_drvinst_alloc(&pdev->dev, sizeof(*icap));
	if (!icap)
		return -ENOMEM;
	platform_set_drvdata(pdev, icap);

	icap->icap_pdev = pdev;
	mutex_init(&icap->icap_lock);

	regs = (void **)&icap->icap_regs;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res != NULL) {
		*regs = ioremap_nocache(res->start,
			res->end - res->start + 1);
		if (*regs == NULL) {
			ICAP_ERR(icap, "failed to map in register");
			ret = -EIO;
			goto failed;
		} else {
			ICAP_INFO(icap,
				"mapped in register @ 0x%p", *regs);
		}

		icap_refresh_addrs(pdev);
	}

	ret = sysfs_create_group(&pdev->dev.kobj, &icap_attr_group);
	if (ret) {
		ICAP_ERR(icap, "create icap attrs failed: %d", ret);
		goto failed;
	} else {
		icap->sysfs_created = true;
	}

	if (ICAP_PRIVILEGED(icap)) {
		ret = icap_load_keyring();
		if (ret) {
			ICAP_ERR(icap, "create icap keyring failed: %d", ret);
			goto failed;
		}
#ifdef	EFI_SECURE_BOOT
		if (efi_enabled(EFI_SECURE_BOOT)) {
			ICAP_INFO(icap, "secure boot mode detected");
			icap->sec_level = ICAP_SEC_SYSTEM;
		} else {
			icap->sec_level = ICAP_SEC_NONE;
		}
#else
		ICAP_INFO(icap, "no support for detection of secure boot mode");
		icap->sec_level = ICAP_SEC_NONE;
#endif
	}

	icap->cache_expire_secs = ICAP_DEFAULT_EXPIRE_SECS;

	icap_probe_chip(icap);
	ICAP_INFO(icap, "successfully initialized FPGA IDCODE 0x%x",
			icap->idcode);
	return 0;

failed:
	(void) icap_remove(pdev);
	return ret;
}

#if PF == MGMTPF
static int icap_open(struct inode *inode, struct file *file)
{
	struct icap *icap = NULL;

	icap = xocl_drvinst_open_single(inode->i_cdev);
	if (!icap)
		return -ENXIO;

	file->private_data = icap;
	return 0;
}

static int icap_close(struct inode *inode, struct file *file)
{
	struct icap *icap = file->private_data;

	xocl_drvinst_close(icap);
	return 0;
}

static ssize_t icap_write_rp(struct file *filp, const char __user *data,
		size_t data_len, loff_t *off)
{
	struct icap *icap = filp->private_data;
	struct axlf axlf_header = { {0} };
	struct axlf *axlf = NULL;
	const struct axlf_section_header *section;
	void *header;
	XHwIcap_Bit_Header bit_header = { 0 };
	ssize_t ret, len;

	mutex_lock(&icap->icap_lock);
	if (icap->rp_fdt) {
		ICAP_ERR(icap, "Previous Dowload is not completed");
		mutex_unlock(&icap->icap_lock);
		return -EBUSY;
	}
	if (*off == 0) {
		ICAP_INFO(icap, "Download rp dsabin");
		if (data_len < sizeof(struct axlf)) {
			ICAP_ERR(icap, "axlf file is too small %ld", data_len);
			ret = -ENOMEM;
			goto failed;
		}

		ret = copy_from_user(&axlf_header, data, sizeof(struct axlf));
		if (ret) {
			ICAP_ERR(icap, "copy header buffer failed %ld", ret);
			goto failed;
		}

		if (memcmp(axlf_header.m_magic, ICAP_XCLBIN_V2,
			sizeof(ICAP_XCLBIN_V2))) {
			ICAP_ERR(icap, "Incorrect magic string");
			ret = -EINVAL;
			goto failed;
		}

		if (!axlf_header.m_header.m_length ||
			axlf_header.m_header.m_length >= GB(1)) {
			ICAP_ERR(icap, "Invalid xclbin size");
			ret = -EINVAL;
			goto failed;			
		}

		icap->rp_bit_len = axlf_header.m_header.m_length;

		icap->rp_bit = vmalloc(icap->rp_bit_len);
		if (!icap->rp_bit) {
			ret = -ENOMEM;
			goto failed;
		}

		ret = copy_from_user(icap->rp_bit, data, data_len);
		if (ret) {
			ICAP_ERR(icap, "copy bit file failed %ld", ret);
			goto failed;
		}
		len = data_len;
	} else {
		len = (ssize_t)(min((loff_t)(icap->rp_bit_len),
				(*off + (loff_t)data_len)) - *off);
		if (len < 0) {
			ICAP_ERR(icap, "Invalid len %ld", len);
			ret = -EINVAL;
			goto failed;
		}
		ret = copy_from_user(icap->rp_bit + *off, data, len);
		if (ret) {
			ICAP_ERR(icap, "copy failed off %lld, len %ld",
					*off, len);
			goto failed;
		}
	}

	*off += len;
	if (*off < icap->rp_bit_len) {
		mutex_unlock(&icap->icap_lock);
		return len;
	}

	ICAP_INFO(icap, "parse incoming axlf");

	axlf = vmalloc(icap->rp_bit_len);
	if (!axlf) {
		ICAP_ERR(icap, "it stream buffer allocation failed");
		ret = -ENOMEM;
		goto failed;
	}

	memcpy(axlf, icap->rp_bit, icap->rp_bit_len);
	vfree(icap->rp_bit);
	icap->rp_bit = NULL;
	icap->rp_bit_len = 0;

	section = get_axlf_section_hdr(icap, axlf, PARTITION_METADATA);
	if (!section) {
		ICAP_ERR(icap, "did not find PARTITION_METADATA section");
		ret = -EINVAL;
		goto failed;
	}

	header = (char *)axlf + section->m_sectionOffset;
	if (fdt_check_header(header) || fdt_totalsize(header) >
			section->m_sectionSize) {
		ICAP_ERR(icap, "Invalid PARTITION_METADATA");
		ret = -EINVAL;
		goto failed;
	}

	icap->rp_fdt = vmalloc(fdt_totalsize(header));
	if (!icap->rp_fdt) {
		ICAP_ERR(icap, "Not enough memory for PARTITION_METADATA");
		ret = -ENOMEM;
		goto failed;
	}
	icap->rp_fdt_len = fdt_totalsize(header);
	memcpy(icap->rp_fdt, header, fdt_totalsize(header));

	section = get_axlf_section_hdr(icap, axlf, BITSTREAM);
	if (!section) {
		ICAP_ERR(icap, "did not find BITSTREAM section");
		ret = -EINVAL;
		goto failed;
	}

	if (section->m_sectionSize < DMA_HWICAP_BITFILE_BUFFER_SIZE) {
		ICAP_ERR(icap, "bitstream is too small");
		ret = -EINVAL;
		goto failed;
	}

	header = (char *)axlf + section->m_sectionOffset;
	if (bitstream_parse_header(icap, header,
			DMA_HWICAP_BITFILE_BUFFER_SIZE, &bit_header)) {
		ICAP_ERR(icap, "parse header failed");
		ret = -EINVAL;
		goto failed;
	}

	icap->rp_bit_len = bit_header.HeaderLength + bit_header.BitstreamLength;
	if (icap->rp_bit_len > section->m_sectionSize) {
		ICAP_ERR(icap, "bitstream is too big");
		ret = -EINVAL;
		goto failed;
	}

	icap->rp_bit = vmalloc(icap->rp_bit_len);
	if (!icap->rp_bit) {
		ICAP_ERR(icap, "Not enough memory for BITSTREAM");
		ret = -ENOMEM;
		goto failed;
	}

	memcpy(icap->rp_bit, header, icap->rp_bit_len);

	/* Try locating the board mgmt binary. */
	section = get_axlf_section_hdr(icap, axlf, FIRMWARE);
	if (section) {
		header = (char *)axlf + section->m_sectionOffset;
		icap->rp_mgmt_bin = vmalloc(section->m_sectionSize);
		if (!icap->rp_mgmt_bin) {
			ICAP_ERR(icap, "Not enough memory for cmc bin");
			ret = -ENOMEM;
			goto failed;
		}
		memcpy(icap->rp_mgmt_bin, header, section->m_sectionSize);
		icap->rp_mgmt_bin_len = section->m_sectionSize;
	}

	section = get_axlf_section_hdr(icap, axlf, SCHED_FIRMWARE);
	if (section) {
		header = (char *)axlf + section->m_sectionOffset;
		icap->rp_sche_bin = vmalloc(section->m_sectionSize);
		if (!icap->rp_sche_bin) {
			ICAP_ERR(icap, "Not enough memory for cmc bin");
			ret = -ENOMEM;
			goto failed;
		}
		memcpy(icap->rp_sche_bin, header, section->m_sectionSize);
		icap->rp_sche_bin_len = section->m_sectionSize;
	}

	vfree(axlf);

	ICAP_INFO(icap, "write axlf to device successfully. len %ld", len);

	mutex_unlock(&icap->icap_lock);

	return len;

failed:
	icap_free_bins(icap);

	vfree(axlf);
	mutex_unlock(&icap->icap_lock);

	return ret;
}

static const struct file_operations icap_fops = {
	.open = icap_open,
	.release = icap_close,
	.write = icap_write_rp,
};

struct xocl_drv_private icap_drv_priv = {
	.ops = &icap_ops,
	.fops = &icap_fops,
	.dev = -1,
	.cdev_name = NULL,
};
#else
struct xocl_drv_private icap_drv_priv = {
	.ops = &icap_ops,
};
#endif

struct platform_device_id icap_id_table[] = {
	{ XOCL_DEVNAME(XOCL_ICAP), (kernel_ulong_t)&icap_drv_priv },
	{ },
};

static struct platform_driver icap_driver = {
	.probe		= icap_probe,
	.remove		= icap_remove,
	.driver		= {
		.name	= XOCL_DEVNAME(XOCL_ICAP),
	},
	.id_table = icap_id_table,
};

int __init xocl_init_icap(void)
{
	int err = 0;

	if (icap_drv_priv.fops) {
		err = alloc_chrdev_region(&icap_drv_priv.dev, 0,
				XOCL_MAX_DEVICES, icap_driver.driver.name);
		if (err < 0)
			goto err_reg_cdev;
	}

	err = platform_driver_register(&icap_driver);
	if (err)
		goto err_reg_driver;

	return 0;

err_reg_driver:
	if (icap_drv_priv.fops && icap_drv_priv.dev != -1)
		unregister_chrdev_region(icap_drv_priv.dev, XOCL_MAX_DEVICES);
err_reg_cdev:
	return err;
}

void xocl_fini_icap(void)
{
	if (icap_drv_priv.fops && icap_drv_priv.dev != -1)
		unregister_chrdev_region(icap_drv_priv.dev, XOCL_MAX_DEVICES);
	platform_driver_unregister(&icap_driver);
}
