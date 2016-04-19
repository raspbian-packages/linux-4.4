/*
 * BCM2835 SD host driver.
 *
 * Author:      Phil Elwell <phil@raspberrypi.org>
 *              Copyright 2015
 *
 * Based on
 *  mmc-bcm2835.c by Gellert Weisz
 * which is, in turn, based on
 *  sdhci-bcm2708.c by Broadcom
 *  sdhci-bcm2835.c by Stephen Warren and Oleksandr Tymoshenko
 *  sdhci.c and sdhci-pci.c by Pierre Ossman
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define SAFE_READ_THRESHOLD     4
#define SAFE_WRITE_THRESHOLD    4
#define ALLOW_DMA               1
#define ALLOW_CMD23             0
#define ALLOW_FAST              1
#define USE_BLOCK_IRQ           1

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sd.h>
#include <linux/scatterlist.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/blkdev.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/of_dma.h>
#include <linux/time.h>

#define DRIVER_NAME "sdhost-bcm2835"

#define SDCMD  0x00 /* Command to SD card              - 16 R/W */
#define SDARG  0x04 /* Argument to SD card             - 32 R/W */
#define SDTOUT 0x08 /* Start value for timeout counter - 32 R/W */
#define SDCDIV 0x0c /* Start value for clock divider   - 11 R/W */
#define SDRSP0 0x10 /* SD card response (31:0)         - 32 R   */
#define SDRSP1 0x14 /* SD card response (63:32)        - 32 R   */
#define SDRSP2 0x18 /* SD card response (95:64)        - 32 R   */
#define SDRSP3 0x1c /* SD card response (127:96)       - 32 R   */
#define SDHSTS 0x20 /* SD host status                  - 11 R   */
#define SDVDD  0x30 /* SD card power control           -  1 R/W */
#define SDEDM  0x34 /* Emergency Debug Mode            - 13 R/W */
#define SDHCFG 0x38 /* Host configuration              -  2 R/W */
#define SDHBCT 0x3c /* Host byte count (debug)         - 32 R/W */
#define SDDATA 0x40 /* Data to/from SD card            - 32 R/W */
#define SDHBLC 0x50 /* Host block count (SDIO/SDHC)    -  9 R/W */

#define SDCMD_NEW_FLAG                  0x8000
#define SDCMD_FAIL_FLAG                 0x4000
#define SDCMD_BUSYWAIT                  0x800
#define SDCMD_NO_RESPONSE               0x400
#define SDCMD_LONG_RESPONSE             0x200
#define SDCMD_WRITE_CMD                 0x80
#define SDCMD_READ_CMD                  0x40
#define SDCMD_CMD_MASK                  0x3f

#define SDCDIV_MAX_CDIV                 0x7ff

#define SDHSTS_BUSY_IRPT                0x400
#define SDHSTS_BLOCK_IRPT               0x200
#define SDHSTS_SDIO_IRPT                0x100
#define SDHSTS_REW_TIME_OUT             0x80
#define SDHSTS_CMD_TIME_OUT             0x40
#define SDHSTS_CRC16_ERROR              0x20
#define SDHSTS_CRC7_ERROR               0x10
#define SDHSTS_FIFO_ERROR               0x08
/* Reserved */
/* Reserved */
#define SDHSTS_DATA_FLAG                0x01

#define SDHSTS_TRANSFER_ERROR_MASK      (SDHSTS_CRC7_ERROR|SDHSTS_CRC16_ERROR|SDHSTS_REW_TIME_OUT|SDHSTS_FIFO_ERROR)
#define SDHSTS_ERROR_MASK               (SDHSTS_CMD_TIME_OUT|SDHSTS_TRANSFER_ERROR_MASK)

#define SDHCFG_BUSY_IRPT_EN     (1<<10)
#define SDHCFG_BLOCK_IRPT_EN    (1<<8)
#define SDHCFG_SDIO_IRPT_EN     (1<<5)
#define SDHCFG_DATA_IRPT_EN     (1<<4)
#define SDHCFG_SLOW_CARD        (1<<3)
#define SDHCFG_WIDE_EXT_BUS     (1<<2)
#define SDHCFG_WIDE_INT_BUS     (1<<1)
#define SDHCFG_REL_CMD_LINE     (1<<0)

#define SDEDM_FORCE_DATA_MODE   (1<<19)
#define SDEDM_CLOCK_PULSE       (1<<20)
#define SDEDM_BYPASS            (1<<21)

#define SDEDM_WRITE_THRESHOLD_SHIFT 9
#define SDEDM_READ_THRESHOLD_SHIFT 14
#define SDEDM_THRESHOLD_MASK     0x1f

#define MHZ 1000000


struct bcm2835_host {
	spinlock_t		lock;

	void __iomem		*ioaddr;
	u32			bus_addr;

	struct mmc_host		*mmc;

	u32			pio_timeout;	/* In jiffies */

	int			clock;		/* Current clock speed */

	bool			slow_card;	/* Force 11-bit divisor */

	unsigned int		max_clk;	/* Max possible freq */

	struct tasklet_struct	finish_tasklet;	/* Tasklet structures */

	struct timer_list	timer;		/* Timer for timeouts */

	struct timer_list	pio_timer;	/* PIO error detection timer */

	struct sg_mapping_iter	sg_miter;	/* SG state for PIO */
	unsigned int		blocks;		/* remaining PIO blocks */

	int			irq;		/* Device IRQ */


	/* cached registers */
	u32			hcfg;
	u32			cdiv;

	struct mmc_request		*mrq;			/* Current request */
	struct mmc_command		*cmd;			/* Current command */
	struct mmc_data			*data;			/* Current data request */
	unsigned int			data_complete:1;	/* Data finished before cmd */

	unsigned int			flush_fifo:1;		/* Drain the fifo when finishing */

	unsigned int			use_busy:1;		/* Wait for busy interrupt */

	unsigned int			debug:1;		/* Enable debug output */

	u32				thread_isr;

	/*DMA part*/
	struct dma_chan			*dma_chan_rx;		/* DMA channel for reads */
	struct dma_chan			*dma_chan_tx;		/* DMA channel for writes */

	bool				allow_dma;
	bool				have_dma;
	bool				use_dma;
	/*end of DMA part*/

	int				max_delay;	/* maximum length of time spent waiting */
	struct timeval			stop_time;	/* when the last stop was issued */
	u32				delay_after_stop; /* minimum time between stop and subsequent data transfer */
	u32				overclock_50;	/* frequency to use when 50MHz is requested (in MHz) */
	u32				overclock;	/* Current frequency if overclocked, else zero */
	u32				pio_limit;	/* Maximum block count for PIO (0 = always DMA) */

	u32				sectors;	/* Cached card size in sectors */
	u32				single_read_sectors[8];
};


static inline void bcm2835_sdhost_write(struct bcm2835_host *host, u32 val, int reg)
{
	writel(val, host->ioaddr + reg);
}

static inline u32 bcm2835_sdhost_read(struct bcm2835_host *host, int reg)
{
	return readl(host->ioaddr + reg);
}

static inline u32 bcm2835_sdhost_read_relaxed(struct bcm2835_host *host, int reg)
{
	return readl_relaxed(host->ioaddr + reg);
}

static void bcm2835_sdhost_dumpcmd(struct bcm2835_host *host,
				   struct mmc_command *cmd,
				   const char *label)
{
	if (cmd)
		pr_info("%s:%c%s op %d arg 0x%x flags 0x%x - resp %08x %08x %08x %08x, err %d\n",
			mmc_hostname(host->mmc),
			(cmd == host->cmd) ? '>' : ' ',
			label, cmd->opcode, cmd->arg, cmd->flags,
			cmd->resp[0], cmd->resp[1], cmd->resp[2], cmd->resp[3],
			cmd->error);
}

static void bcm2835_sdhost_dumpregs(struct bcm2835_host *host)
{
	bcm2835_sdhost_dumpcmd(host, host->mrq->sbc, "sbc");
	bcm2835_sdhost_dumpcmd(host, host->mrq->cmd, "cmd");
	if (host->mrq->data)
		pr_err("%s: data blocks %x blksz %x - err %d\n",
		       mmc_hostname(host->mmc),
		       host->mrq->data->blocks,
		       host->mrq->data->blksz,
		       host->mrq->data->error);
	bcm2835_sdhost_dumpcmd(host, host->mrq->stop, "stop");

	pr_info("%s: =========== REGISTER DUMP ===========\n",
		mmc_hostname(host->mmc));

	pr_info("%s: SDCMD  0x%08x\n",
		mmc_hostname(host->mmc),
		bcm2835_sdhost_read(host, SDCMD));
	pr_info("%s: SDARG  0x%08x\n",
		mmc_hostname(host->mmc),
		bcm2835_sdhost_read(host, SDARG));
	pr_info("%s: SDTOUT 0x%08x\n",
		mmc_hostname(host->mmc),
		bcm2835_sdhost_read(host, SDTOUT));
	pr_info("%s: SDCDIV 0x%08x\n",
		mmc_hostname(host->mmc),
		bcm2835_sdhost_read(host, SDCDIV));
	pr_info("%s: SDRSP0 0x%08x\n",
		mmc_hostname(host->mmc),
		bcm2835_sdhost_read(host, SDRSP0));
	pr_info("%s: SDRSP1 0x%08x\n",
		mmc_hostname(host->mmc),
		bcm2835_sdhost_read(host, SDRSP1));
	pr_info("%s: SDRSP2 0x%08x\n",
		mmc_hostname(host->mmc),
		bcm2835_sdhost_read(host, SDRSP2));
	pr_info("%s: SDRSP3 0x%08x\n",
		mmc_hostname(host->mmc),
		bcm2835_sdhost_read(host, SDRSP3));
	pr_info("%s: SDHSTS 0x%08x\n",
		mmc_hostname(host->mmc),
		bcm2835_sdhost_read(host, SDHSTS));
	pr_info("%s: SDVDD  0x%08x\n",
		mmc_hostname(host->mmc),
		bcm2835_sdhost_read(host, SDVDD));
	pr_info("%s: SDEDM  0x%08x\n",
		mmc_hostname(host->mmc),
		bcm2835_sdhost_read(host, SDEDM));
	pr_info("%s: SDHCFG 0x%08x\n",
		mmc_hostname(host->mmc),
		bcm2835_sdhost_read(host, SDHCFG));
	pr_info("%s: SDHBCT 0x%08x\n",
		mmc_hostname(host->mmc),
		bcm2835_sdhost_read(host, SDHBCT));
	pr_info("%s: SDHBLC 0x%08x\n",
		mmc_hostname(host->mmc),
		bcm2835_sdhost_read(host, SDHBLC));

	pr_info("%s: ===========================================\n",
		mmc_hostname(host->mmc));
}


static void bcm2835_sdhost_set_power(struct bcm2835_host *host, bool on)
{
	bcm2835_sdhost_write(host, on ? 1 : 0, SDVDD);
}


static void bcm2835_sdhost_reset_internal(struct bcm2835_host *host)
{
	u32 temp;

	if (host->debug)
		pr_info("%s: reset\n", mmc_hostname(host->mmc));

	bcm2835_sdhost_set_power(host, false);

	bcm2835_sdhost_write(host, 0, SDCMD);
	bcm2835_sdhost_write(host, 0, SDARG);
	bcm2835_sdhost_write(host, 0xf00000, SDTOUT);
	bcm2835_sdhost_write(host, 0, SDCDIV);
	bcm2835_sdhost_write(host, 0x7f8, SDHSTS); /* Write 1s to clear */
	bcm2835_sdhost_write(host, 0, SDHCFG);
	bcm2835_sdhost_write(host, 0, SDHBCT);
	bcm2835_sdhost_write(host, 0, SDHBLC);

	/* Limit fifo usage due to silicon bug */
	temp = bcm2835_sdhost_read(host, SDEDM);
	temp &= ~((SDEDM_THRESHOLD_MASK<<SDEDM_READ_THRESHOLD_SHIFT) |
		  (SDEDM_THRESHOLD_MASK<<SDEDM_WRITE_THRESHOLD_SHIFT));
	temp |= (SAFE_READ_THRESHOLD << SDEDM_READ_THRESHOLD_SHIFT) |
		(SAFE_WRITE_THRESHOLD << SDEDM_WRITE_THRESHOLD_SHIFT);
	bcm2835_sdhost_write(host, temp, SDEDM);
	mdelay(10);
	bcm2835_sdhost_set_power(host, true);
	mdelay(10);
	host->clock = 0;
	host->sectors = 0;
	host->single_read_sectors[0] = ~0;
	bcm2835_sdhost_write(host, host->hcfg, SDHCFG);
	bcm2835_sdhost_write(host, host->cdiv, SDCDIV);
	mmiowb();
}


static void bcm2835_sdhost_reset(struct mmc_host *mmc)
{
	struct bcm2835_host *host = mmc_priv(mmc);
	unsigned long flags;
	spin_lock_irqsave(&host->lock, flags);

	bcm2835_sdhost_reset_internal(host);

	spin_unlock_irqrestore(&host->lock, flags);
}

static void bcm2835_sdhost_set_ios(struct mmc_host *mmc, struct mmc_ios *ios);

static void bcm2835_sdhost_init(struct bcm2835_host *host, int soft)
{
	pr_debug("bcm2835_sdhost_init(%d)\n", soft);

	/* Set interrupt enables */
	host->hcfg = SDHCFG_BUSY_IRPT_EN;

	bcm2835_sdhost_reset_internal(host);

	if (soft) {
		/* force clock reconfiguration */
		host->clock = 0;
		bcm2835_sdhost_set_ios(host->mmc, &host->mmc->ios);
	}
}

static bool bcm2835_sdhost_is_write_complete(struct bcm2835_host *host)
{
	bool write_complete = ((bcm2835_sdhost_read(host, SDEDM) & 0xf) == 1);

	if (!write_complete) {
		/* Request an IRQ for the last block */
		host->hcfg |= SDHCFG_BLOCK_IRPT_EN;
		bcm2835_sdhost_write(host, host->hcfg, SDHCFG);
		if ((bcm2835_sdhost_read(host, SDEDM) & 0xf) == 1) {
			/* The write has now completed. Disable the interrupt
			   and clear the status flag */
			host->hcfg &= ~SDHCFG_BLOCK_IRPT_EN;
			bcm2835_sdhost_write(host, host->hcfg, SDHCFG);
			bcm2835_sdhost_write(host, SDHSTS_BLOCK_IRPT, SDHSTS);
			write_complete = true;
		}
	}

	return write_complete;
}

static void bcm2835_sdhost_wait_write_complete(struct bcm2835_host *host)
{
	int timediff;
#ifdef DEBUG
	static struct timeval start_time;
	static int max_stall_time = 0;
	static int total_stall_time = 0;
	struct timeval before, after;

	do_gettimeofday(&before);
	if (max_stall_time == 0)
		start_time = before;
#endif

	timediff = 0;

	while (1) {
		u32 edm = bcm2835_sdhost_read(host, SDEDM);
		if ((edm & 0xf) == 1)
			break;
		timediff++;
		if (timediff > 5000000) {
#ifdef DEBUG
			do_gettimeofday(&after);
			timediff = (after.tv_sec - before.tv_sec)*1000000 +
				(after.tv_usec - before.tv_usec);

			pr_err(" wait_write_complete - still waiting after %dus\n",
			       timediff);
#else
			pr_err(" wait_write_complete - still waiting after %d retries\n",
			       timediff);
#endif
			bcm2835_sdhost_dumpregs(host);
			host->data->error = -ETIMEDOUT;
			return;
		}
	}

#ifdef DEBUG
	do_gettimeofday(&after);
	timediff = (after.tv_sec - before.tv_sec)*1000000 + (after.tv_usec - before.tv_usec);

	total_stall_time += timediff;
	if (timediff > max_stall_time)
		max_stall_time = timediff;

	if ((after.tv_sec - start_time.tv_sec) > 10) {
		pr_debug(" wait_write_complete - max wait %dus, total %dus\n",
			 max_stall_time, total_stall_time);
		start_time = after;
		max_stall_time = 0;
		total_stall_time = 0;
	}
#endif
}

static void bcm2835_sdhost_finish_data(struct bcm2835_host *host);

static void bcm2835_sdhost_dma_complete(void *param)
{
	struct bcm2835_host *host = param;
	struct dma_chan *dma_chan;
	unsigned long flags;
	u32 dir_data;

	spin_lock_irqsave(&host->lock, flags);

	if (host->data) {
		bool write_complete;
		if (USE_BLOCK_IRQ)
			write_complete = bcm2835_sdhost_is_write_complete(host);
		else {
			bcm2835_sdhost_wait_write_complete(host);
			write_complete = true;
		}
		pr_debug("dma_complete() - write_complete=%d\n",
			 write_complete);

		if (write_complete || (host->data->flags & MMC_DATA_READ))
		{
			if (write_complete) {
				dma_chan = host->dma_chan_tx;
				dir_data = DMA_TO_DEVICE;
			} else {
				dma_chan = host->dma_chan_rx;
				dir_data = DMA_FROM_DEVICE;
			}

			dma_unmap_sg(dma_chan->device->dev,
				     host->data->sg, host->data->sg_len,
				     dir_data);

			bcm2835_sdhost_finish_data(host);
		}
	}

	spin_unlock_irqrestore(&host->lock, flags);
}

static bool data_transfer_wait(struct bcm2835_host *host)
{
	unsigned long timeout = 1000000;
	while (timeout)
	{
		u32 sdhsts = bcm2835_sdhost_read(host, SDHSTS);
		if (sdhsts & SDHSTS_DATA_FLAG) {
			bcm2835_sdhost_write(host, SDHSTS_DATA_FLAG, SDHSTS);
			break;
		}
		timeout--;
	}
	if (timeout == 0) {
	    pr_err("%s: Data %s timeout\n",
		   mmc_hostname(host->mmc),
		   (host->data->flags & MMC_DATA_READ) ? "read" : "write");
	    bcm2835_sdhost_dumpregs(host);
	    host->data->error = -ETIMEDOUT;
	    return false;
	}
	return true;
}

static void bcm2835_sdhost_read_block_pio(struct bcm2835_host *host)
{
	unsigned long flags;
	size_t blksize, len;
	u32 *buf;

	blksize = host->data->blksz;

	local_irq_save(flags);

	while (blksize) {
		if (!sg_miter_next(&host->sg_miter))
			BUG();

		len = min(host->sg_miter.length, blksize);
		BUG_ON(len % 4);

		blksize -= len;
		host->sg_miter.consumed = len;

		buf = (u32 *)host->sg_miter.addr;

		while (len) {
			if (!data_transfer_wait(host))
				break;

			*(buf++) = bcm2835_sdhost_read(host, SDDATA);
			len -= 4;
		}

		if (host->data->error)
			break;
	}

	sg_miter_stop(&host->sg_miter);

	local_irq_restore(flags);
}

static void bcm2835_sdhost_write_block_pio(struct bcm2835_host *host)
{
	unsigned long flags;
	size_t blksize, len;
	u32 *buf;

	blksize = host->data->blksz;

	local_irq_save(flags);

	while (blksize) {
		if (!sg_miter_next(&host->sg_miter))
			BUG();

		len = min(host->sg_miter.length, blksize);
		BUG_ON(len % 4);

		blksize -= len;
		host->sg_miter.consumed = len;

		buf = host->sg_miter.addr;

		while (len) {
			if (!data_transfer_wait(host))
				break;

			bcm2835_sdhost_write(host, *(buf++), SDDATA);
			len -= 4;
		}

		if (host->data->error)
			break;
	}

	sg_miter_stop(&host->sg_miter);

	local_irq_restore(flags);
}


static void bcm2835_sdhost_transfer_pio(struct bcm2835_host *host)
{
	u32 sdhsts;
	bool is_read;
	BUG_ON(!host->data);

	is_read = (host->data->flags & MMC_DATA_READ) != 0;
	if (is_read)
		bcm2835_sdhost_read_block_pio(host);
	else
		bcm2835_sdhost_write_block_pio(host);

	sdhsts = bcm2835_sdhost_read(host, SDHSTS);
	if (sdhsts & (SDHSTS_CRC16_ERROR |
		      SDHSTS_CRC7_ERROR |
		      SDHSTS_FIFO_ERROR)) {
		pr_err("%s: %s transfer error - HSTS %x\n",
		       mmc_hostname(host->mmc),
		       is_read ? "read" : "write",
		       sdhsts);
		host->data->error = -EILSEQ;
	} else if ((sdhsts & (SDHSTS_CMD_TIME_OUT |
			      SDHSTS_REW_TIME_OUT))) {
		pr_err("%s: %s timeout error - HSTS %x\n",
		       mmc_hostname(host->mmc),
		       is_read ? "read" : "write",
		       sdhsts);
		host->data->error = -ETIMEDOUT;
	} else if (!is_read && !host->data->error) {
		/* Start a timer in case a transfer error occurs because
		   there is no error interrupt */
		mod_timer(&host->pio_timer, jiffies + host->pio_timeout);
	}
}


static void bcm2835_sdhost_transfer_dma(struct bcm2835_host *host)
{
	u32 len, dir_data, dir_slave;
	struct dma_async_tx_descriptor *desc = NULL;
	struct dma_chan *dma_chan;

	pr_debug("bcm2835_sdhost_transfer_dma()\n");

	WARN_ON(!host->data);

	if (!host->data)
		return;

	if (host->data->flags & MMC_DATA_READ) {
		dma_chan = host->dma_chan_rx;
		dir_data = DMA_FROM_DEVICE;
		dir_slave = DMA_DEV_TO_MEM;
	} else {
		dma_chan = host->dma_chan_tx;
		dir_data = DMA_TO_DEVICE;
		dir_slave = DMA_MEM_TO_DEV;
	}

	BUG_ON(!dma_chan->device);
	BUG_ON(!dma_chan->device->dev);
	BUG_ON(!host->data->sg);

	len = dma_map_sg(dma_chan->device->dev, host->data->sg,
			 host->data->sg_len, dir_data);
	if (len > 0) {
		desc = dmaengine_prep_slave_sg(dma_chan, host->data->sg,
					       len, dir_slave,
					       DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	} else {
		dev_err(mmc_dev(host->mmc), "dma_map_sg returned zero length\n");
	}
	if (desc) {
		desc->callback = bcm2835_sdhost_dma_complete;
		desc->callback_param = host;
		dmaengine_submit(desc);
		dma_async_issue_pending(dma_chan);
	}

}


static void bcm2835_sdhost_set_transfer_irqs(struct bcm2835_host *host)
{
	u32 all_irqs = SDHCFG_DATA_IRPT_EN | SDHCFG_BLOCK_IRPT_EN |
		SDHCFG_BUSY_IRPT_EN;
	if (host->use_dma)
		host->hcfg = (host->hcfg & ~all_irqs) |
			SDHCFG_BUSY_IRPT_EN;
	else
		host->hcfg = (host->hcfg & ~all_irqs) |
			SDHCFG_DATA_IRPT_EN |
			SDHCFG_BUSY_IRPT_EN;

	bcm2835_sdhost_write(host, host->hcfg, SDHCFG);
}


static void bcm2835_sdhost_prepare_data(struct bcm2835_host *host, struct mmc_command *cmd)
{
	struct mmc_data *data = cmd->data;

	WARN_ON(host->data);

	if (!data)
		return;

	/* Sanity checks */
	BUG_ON(data->blksz * data->blocks > 524288);
	BUG_ON(data->blksz > host->mmc->max_blk_size);
	BUG_ON(data->blocks > 65535);

	host->data = data;
	host->data_complete = 0;
	host->flush_fifo = 0;
	host->data->bytes_xfered = 0;

	if (!host->sectors && host->mmc->card)
	{
		struct mmc_card *card = host->mmc->card;
		if (!mmc_card_sd(card) && mmc_card_blockaddr(card)) {
			/*
			 * The EXT_CSD sector count is in number of 512 byte
			 * sectors.
			 */
			host->sectors = card->ext_csd.sectors;
			pr_err("%s: using ext_csd!\n", mmc_hostname(host->mmc));
		} else {
			/*
			 * The CSD capacity field is in units of read_blkbits.
			 * set_capacity takes units of 512 bytes.
			 */
			host->sectors = card->csd.capacity <<
				(card->csd.read_blkbits - 9);
		}
		host->single_read_sectors[0] = host->sectors - 65;
		host->single_read_sectors[1] = host->sectors - 64;
		host->single_read_sectors[2] = host->sectors - 33;
		host->single_read_sectors[3] = host->sectors - 32;
		host->single_read_sectors[4] = host->sectors - 1;
		host->single_read_sectors[5] = ~0; /* Safety net */
	}

	host->use_dma = host->have_dma && (data->blocks > host->pio_limit);
	if (!host->use_dma) {
		int flags;

		flags = SG_MITER_ATOMIC;
		if (data->flags & MMC_DATA_READ)
			flags |= SG_MITER_TO_SG;
		else
			flags |= SG_MITER_FROM_SG;
		sg_miter_start(&host->sg_miter, data->sg, data->sg_len, flags);
		host->blocks = data->blocks;
	}

	bcm2835_sdhost_set_transfer_irqs(host);

	bcm2835_sdhost_write(host, data->blksz, SDHBCT);
	bcm2835_sdhost_write(host, host->use_dma ? data->blocks : 0, SDHBLC);

	BUG_ON(!host->data);
}


void bcm2835_sdhost_send_command(struct bcm2835_host *host, struct mmc_command *cmd)
{
	u32 sdcmd, sdhsts;
	unsigned long timeout;
	int delay;

	WARN_ON(host->cmd);

	if (cmd->data)
		pr_debug("%s: send_command %d 0x%x "
			 "(flags 0x%x) - %s %d*%d\n",
			 mmc_hostname(host->mmc),
			 cmd->opcode, cmd->arg, cmd->flags,
			 (cmd->data->flags & MMC_DATA_READ) ?
			 "read" : "write", cmd->data->blocks,
			 cmd->data->blksz);
	else
		pr_debug("%s: send_command %d 0x%x (flags 0x%x)\n",
			 mmc_hostname(host->mmc),
			 cmd->opcode, cmd->arg, cmd->flags);

	/* Wait max 100 ms */
	timeout = 10000;

	while (bcm2835_sdhost_read(host, SDCMD) & SDCMD_NEW_FLAG) {
		if (timeout == 0) {
			pr_err("%s: previous command never completed.\n",
				mmc_hostname(host->mmc));
			bcm2835_sdhost_dumpregs(host);
			cmd->error = -EIO;
			tasklet_schedule(&host->finish_tasklet);
			return;
		}
		timeout--;
		udelay(10);
	}

	delay = (10000 - timeout)/100;
	if (delay > host->max_delay) {
		host->max_delay = delay;
		pr_warning("%s: controller hung for %d ms\n",
			   mmc_hostname(host->mmc),
			   host->max_delay);
	}

	timeout = jiffies;
	if (!cmd->data && cmd->busy_timeout > 9000)
		timeout += DIV_ROUND_UP(cmd->busy_timeout, 1000) * HZ + HZ;
	else
		timeout += 10 * HZ;
	mod_timer(&host->timer, timeout);

	host->cmd = cmd;

	/* Clear any error flags */
	sdhsts = bcm2835_sdhost_read(host, SDHSTS);
	if (sdhsts & SDHSTS_ERROR_MASK)
		bcm2835_sdhost_write(host, sdhsts, SDHSTS);

	bcm2835_sdhost_prepare_data(host, cmd);

	bcm2835_sdhost_write(host, cmd->arg, SDARG);

	if ((cmd->flags & MMC_RSP_136) && (cmd->flags & MMC_RSP_BUSY)) {
		pr_err("%s: unsupported response type!\n",
			mmc_hostname(host->mmc));
		cmd->error = -EINVAL;
		tasklet_schedule(&host->finish_tasklet);
		return;
	}

	sdcmd = cmd->opcode & SDCMD_CMD_MASK;

	if (!(cmd->flags & MMC_RSP_PRESENT))
		sdcmd |= SDCMD_NO_RESPONSE;
	else {
		if (cmd->flags & MMC_RSP_136)
			sdcmd |= SDCMD_LONG_RESPONSE;
		if (cmd->flags & MMC_RSP_BUSY) {
			sdcmd |= SDCMD_BUSYWAIT;
			host->use_busy = 1;
		}
	}

	if (cmd->data) {
		if (host->delay_after_stop) {
			struct timeval now;
			int time_since_stop;
			do_gettimeofday(&now);
			time_since_stop = (now.tv_sec - host->stop_time.tv_sec);
			if (time_since_stop < 2) {
				/* Possibly less than one second */
				time_since_stop = time_since_stop * 1000000 +
					(now.tv_usec - host->stop_time.tv_usec);
				if (time_since_stop < host->delay_after_stop)
					udelay(host->delay_after_stop -
					       time_since_stop);
			}
		}

		if (cmd->data->flags & MMC_DATA_WRITE)
			sdcmd |= SDCMD_WRITE_CMD;
		if (cmd->data->flags & MMC_DATA_READ)
			sdcmd |= SDCMD_READ_CMD;
	}

	bcm2835_sdhost_write(host, sdcmd | SDCMD_NEW_FLAG, SDCMD);
}


static void bcm2835_sdhost_finish_command(struct bcm2835_host *host);
static void bcm2835_sdhost_transfer_complete(struct bcm2835_host *host);

static void bcm2835_sdhost_finish_data(struct bcm2835_host *host)
{
	struct mmc_data *data;

	data = host->data;
	BUG_ON(!data);

	pr_debug("finish_data(error %d, stop %d, sbc %d)\n",
	       data->error, data->stop ? 1 : 0,
	       host->mrq->sbc ? 1 : 0);

	host->hcfg &= ~(SDHCFG_DATA_IRPT_EN | SDHCFG_BLOCK_IRPT_EN);
	bcm2835_sdhost_write(host, host->hcfg, SDHCFG);

	if (data->error) {
		data->bytes_xfered = 0;
	} else
		data->bytes_xfered = data->blksz * data->blocks;

	host->data_complete = 1;

	if (host->cmd) {
		/*
		 * Data managed to finish before the
		 * command completed. Make sure we do
		 * things in the proper order.
		 */
		pr_debug("Finished early - HSTS %x\n",
			 bcm2835_sdhost_read(host, SDHSTS));
	}
	else
		bcm2835_sdhost_transfer_complete(host);
}


static void bcm2835_sdhost_transfer_complete(struct bcm2835_host *host)
{
	struct mmc_data *data;

	BUG_ON(host->cmd);
	BUG_ON(!host->data);
	BUG_ON(!host->data_complete);

	data = host->data;
	host->data = NULL;

	pr_debug("transfer_complete(error %d, stop %d)\n",
	       data->error, data->stop ? 1 : 0);

	/*
	 * Need to send CMD12 if -
	 * a) open-ended multiblock transfer (no CMD23)
	 * b) error in multiblock transfer
	 */
	if (data->stop &&
	    (data->error ||
	     !host->mrq->sbc)) {
		host->flush_fifo = 1;
		bcm2835_sdhost_send_command(host, data->stop);
		if (host->delay_after_stop)
			do_gettimeofday(&host->stop_time);
		if (!host->use_busy)
			bcm2835_sdhost_finish_command(host);
	} else {
		tasklet_schedule(&host->finish_tasklet);
	}
}

static void bcm2835_sdhost_finish_command(struct bcm2835_host *host)
{
	u32 sdcmd;
	unsigned long timeout;
#ifdef DEBUG
	struct timeval before, after;
	int timediff = 0;
#endif

	pr_debug("finish_command(%x)\n", bcm2835_sdhost_read(host, SDCMD));

	BUG_ON(!host->cmd || !host->mrq);

#ifdef DEBUG
	do_gettimeofday(&before);
#endif
	/* Wait max 100 ms */
	timeout = 10000;
	for (sdcmd = bcm2835_sdhost_read(host, SDCMD);
	     (sdcmd & SDCMD_NEW_FLAG) && timeout;
	     timeout--) {
		if (host->flush_fifo) {
			while (bcm2835_sdhost_read(host, SDHSTS) &
			       SDHSTS_DATA_FLAG)
				(void)bcm2835_sdhost_read(host, SDDATA);
		}
		udelay(10);
		sdcmd = bcm2835_sdhost_read(host, SDCMD);
	}
#ifdef DEBUG
	do_gettimeofday(&after);
	timediff = (after.tv_sec - before.tv_sec)*1000000 +
		(after.tv_usec - before.tv_usec);

	pr_debug(" finish_command - waited %dus\n", timediff);
#endif

	if (timeout == 0) {
		pr_err("%s: command never completed.\n",
		       mmc_hostname(host->mmc));
		bcm2835_sdhost_dumpregs(host);
		host->cmd->error = -EIO;
		tasklet_schedule(&host->finish_tasklet);
		return;
	}

	if (host->flush_fifo) {
		for (timeout = 100;
		     (bcm2835_sdhost_read(host, SDHSTS) & SDHSTS_DATA_FLAG) && timeout;
		     timeout--) {
			(void)bcm2835_sdhost_read(host, SDDATA);
		}
		host->flush_fifo = 0;
		if (timeout == 0) {
			pr_err("%s: FIFO never drained.\n",
			       mmc_hostname(host->mmc));
			bcm2835_sdhost_dumpregs(host);
			host->cmd->error = -EIO;
			tasklet_schedule(&host->finish_tasklet);
			return;
		}
	}

	/* Check for errors */
	if (sdcmd & SDCMD_FAIL_FLAG)
	{
		u32 sdhsts = bcm2835_sdhost_read(host, SDHSTS);

		if (host->debug)
			pr_info("%s: error detected - CMD %x, HSTS %03x, EDM %x\n",
				mmc_hostname(host->mmc), sdcmd, sdhsts,
				bcm2835_sdhost_read(host, SDEDM));

		if ((sdhsts & SDHSTS_CRC7_ERROR) &&
		    (host->cmd->opcode == 1)) {
			if (host->debug)
				pr_info("%s: ignoring CRC7 error for CMD1\n",
					mmc_hostname(host->mmc));
		} else {
			if (sdhsts & SDHSTS_CMD_TIME_OUT) {
				if (host->debug)
					pr_err("%s: command %d timeout\n",
					       mmc_hostname(host->mmc),
					       host->cmd->opcode);
				host->cmd->error = -ETIMEDOUT;
			} else {
				pr_err("%s: unexpected command %d error\n",
				       mmc_hostname(host->mmc),
				       host->cmd->opcode);
				bcm2835_sdhost_dumpregs(host);
				host->cmd->error = -EIO;
			}
			tasklet_schedule(&host->finish_tasklet);
			return;
		}
	}

	if (host->cmd->flags & MMC_RSP_PRESENT) {
		if (host->cmd->flags & MMC_RSP_136) {
			int i;
			for (i = 0; i < 4; i++)
				host->cmd->resp[3 - i] = bcm2835_sdhost_read(host, SDRSP0 + i*4);
			pr_debug("%s: finish_command %08x %08x %08x %08x\n",
				 mmc_hostname(host->mmc),
				 host->cmd->resp[0], host->cmd->resp[1], host->cmd->resp[2], host->cmd->resp[3]);
		} else {
			host->cmd->resp[0] = bcm2835_sdhost_read(host, SDRSP0);
			pr_debug("%s: finish_command %08x\n",
				 mmc_hostname(host->mmc),
				 host->cmd->resp[0]);
		}
	}

	host->cmd->error = 0;

	if (host->cmd == host->mrq->sbc) {
		/* Finished CMD23, now send actual command. */
		host->cmd = NULL;
		bcm2835_sdhost_send_command(host, host->mrq->cmd);

		if (host->cmd->data && host->use_dma)
			/* DMA transfer starts now, PIO starts after irq */
			bcm2835_sdhost_transfer_dma(host);

		if (!host->use_busy)
			bcm2835_sdhost_finish_command(host);
	} else if (host->cmd == host->mrq->stop)
		/* Finished CMD12 */
		tasklet_schedule(&host->finish_tasklet);
	else {
		/* Processed actual command. */
		host->cmd = NULL;
		if (!host->data)
			tasklet_schedule(&host->finish_tasklet);
		else if (host->data_complete)
			bcm2835_sdhost_transfer_complete(host);
	}
}

static void bcm2835_sdhost_timeout(unsigned long data)
{
	struct bcm2835_host *host;
	unsigned long flags;

	host = (struct bcm2835_host *)data;

	spin_lock_irqsave(&host->lock, flags);

	if (host->mrq) {
		pr_err("%s: timeout waiting for hardware interrupt.\n",
			mmc_hostname(host->mmc));
		bcm2835_sdhost_dumpregs(host);

		if (host->data) {
			host->data->error = -ETIMEDOUT;
			bcm2835_sdhost_finish_data(host);
		} else {
			if (host->cmd)
				host->cmd->error = -ETIMEDOUT;
			else
				host->mrq->cmd->error = -ETIMEDOUT;

			pr_debug("timeout_timer tasklet_schedule\n");
			tasklet_schedule(&host->finish_tasklet);
		}
	}

	mmiowb();
	spin_unlock_irqrestore(&host->lock, flags);
}

static void bcm2835_sdhost_pio_timeout(unsigned long data)
{
	struct bcm2835_host *host;
	unsigned long flags;

	host = (struct bcm2835_host *)data;

	spin_lock_irqsave(&host->lock, flags);

	if (host->data) {
		u32 sdhsts = bcm2835_sdhost_read(host, SDHSTS);

		if (sdhsts & SDHSTS_REW_TIME_OUT) {
			pr_err("%s: transfer timeout\n",
			       mmc_hostname(host->mmc));
			if (host->debug)
				bcm2835_sdhost_dumpregs(host);
		} else {
			pr_err("%s: unexpected transfer timeout\n",
			       mmc_hostname(host->mmc));
			bcm2835_sdhost_dumpregs(host);
		}

		bcm2835_sdhost_write(host, SDHSTS_TRANSFER_ERROR_MASK,
				     SDHSTS);

		host->data->error = -ETIMEDOUT;

		bcm2835_sdhost_finish_data(host);
	}

	mmiowb();
	spin_unlock_irqrestore(&host->lock, flags);
}

static void bcm2835_sdhost_enable_sdio_irq_nolock(struct bcm2835_host *host, int enable)
{
	if (enable)
		host->hcfg |= SDHCFG_SDIO_IRPT_EN;
	else
		host->hcfg &= ~SDHCFG_SDIO_IRPT_EN;
	bcm2835_sdhost_write(host, host->hcfg, SDHCFG);
	mmiowb();
}

static void bcm2835_sdhost_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct bcm2835_host *host = mmc_priv(mmc);
	unsigned long flags;

	pr_debug("%s: enable_sdio_irq(%d)\n", mmc_hostname(mmc), enable);
	spin_lock_irqsave(&host->lock, flags);
	bcm2835_sdhost_enable_sdio_irq_nolock(host, enable);
	spin_unlock_irqrestore(&host->lock, flags);
}

static u32 bcm2835_sdhost_busy_irq(struct bcm2835_host *host, u32 intmask)
{
	const u32 handled = (SDHSTS_REW_TIME_OUT | SDHSTS_CMD_TIME_OUT |
			     SDHSTS_CRC16_ERROR | SDHSTS_CRC7_ERROR |
			     SDHSTS_FIFO_ERROR);

	if (!host->cmd) {
		pr_err("%s: got command busy interrupt 0x%08x even "
			"though no command operation was in progress.\n",
			mmc_hostname(host->mmc), (unsigned)intmask);
		bcm2835_sdhost_dumpregs(host);
		return 0;
	}

	if (!host->use_busy) {
		pr_err("%s: got command busy interrupt 0x%08x even "
			"though not expecting one.\n",
			mmc_hostname(host->mmc), (unsigned)intmask);
		bcm2835_sdhost_dumpregs(host);
		return 0;
	}
	host->use_busy = 0;

	if (intmask & SDHSTS_ERROR_MASK)
	{
		pr_err("sdhost_busy_irq: intmask %x, data %p\n", intmask, host->mrq->data);
		if (intmask & SDHSTS_CRC7_ERROR)
			host->cmd->error = -EILSEQ;
		else if (intmask & (SDHSTS_CRC16_ERROR |
				    SDHSTS_FIFO_ERROR)) {
			if (host->mrq->data)
				host->mrq->data->error = -EILSEQ;
			else
				host->cmd->error = -EILSEQ;
		} else if (intmask & SDHSTS_REW_TIME_OUT) {
			if (host->mrq->data)
				host->mrq->data->error = -ETIMEDOUT;
			else
				host->cmd->error = -ETIMEDOUT;
		} else if (intmask & SDHSTS_CMD_TIME_OUT)
			host->cmd->error = -ETIMEDOUT;

		bcm2835_sdhost_dumpregs(host);
		tasklet_schedule(&host->finish_tasklet);
	}
	else
		bcm2835_sdhost_finish_command(host);

	return handled;
}

static u32 bcm2835_sdhost_data_irq(struct bcm2835_host *host, u32 intmask)
{
	const u32 handled = (SDHSTS_REW_TIME_OUT |
			     SDHSTS_CRC16_ERROR |
			     SDHSTS_FIFO_ERROR);

	/* There are no dedicated data/space available interrupt
	   status bits, so it is necessary to use the single shared
	   data/space available FIFO status bits. It is therefore not
	   an error to get here when there is no data transfer in
	   progress. */
	if (!host->data)
		return 0;

	if (intmask & (SDHSTS_CRC16_ERROR |
		       SDHSTS_FIFO_ERROR |
		       SDHSTS_REW_TIME_OUT)) {
		if (intmask & (SDHSTS_CRC16_ERROR |
			       SDHSTS_FIFO_ERROR))
			host->data->error = -EILSEQ;
		else
			host->data->error = -ETIMEDOUT;

		bcm2835_sdhost_dumpregs(host);
		tasklet_schedule(&host->finish_tasklet);
		return handled;
	}

	/* Use the block interrupt for writes after the first block */
	if (host->data->flags & MMC_DATA_WRITE) {
		host->hcfg &= ~(SDHCFG_DATA_IRPT_EN);
		host->hcfg |= SDHCFG_BLOCK_IRPT_EN;
		bcm2835_sdhost_write(host, host->hcfg, SDHCFG);
		if (host->data->error)
			bcm2835_sdhost_finish_data(host);
		else
			bcm2835_sdhost_transfer_pio(host);
	} else {
		if (!host->data->error) {
			bcm2835_sdhost_transfer_pio(host);
			host->blocks--;
		}
		if ((host->blocks == 0) || host->data->error)
			bcm2835_sdhost_finish_data(host);
	}

	return handled;
}

static u32 bcm2835_sdhost_block_irq(struct bcm2835_host *host, u32 intmask)
{
	struct dma_chan *dma_chan;
	u32 dir_data;
	const u32 handled = (SDHSTS_REW_TIME_OUT |
			     SDHSTS_CRC16_ERROR |
			     SDHSTS_FIFO_ERROR);

	if (!host->data) {
		pr_err("%s: got block interrupt 0x%08x even "
			"though no data operation was in progress.\n",
			mmc_hostname(host->mmc), (unsigned)intmask);
		bcm2835_sdhost_dumpregs(host);
		return handled;
	}

	if (intmask & (SDHSTS_CRC16_ERROR |
		       SDHSTS_FIFO_ERROR |
		       SDHSTS_REW_TIME_OUT)) {
		if (intmask & (SDHSTS_CRC16_ERROR |
			       SDHSTS_FIFO_ERROR))
			host->data->error = -EILSEQ;
		else
			host->data->error = -ETIMEDOUT;

		if (host->debug)
			bcm2835_sdhost_dumpregs(host);
		tasklet_schedule(&host->finish_tasklet);
		return handled;
	}

	if (!host->use_dma) {
		BUG_ON(!host->blocks);
		host->blocks--;
		if ((host->blocks == 0) || host->data->error) {
			/* Cancel the timer */
			del_timer(&host->pio_timer);

			bcm2835_sdhost_finish_data(host);
		} else {
			/* Reset the timer */
			mod_timer(&host->pio_timer,
				  jiffies + host->pio_timeout);

			bcm2835_sdhost_transfer_pio(host);

			/* Reset the timer */
			mod_timer(&host->pio_timer,
				  jiffies + host->pio_timeout);
		}
	} else if (host->data->flags & MMC_DATA_WRITE) {
		dma_chan = host->dma_chan_tx;
		dir_data = DMA_TO_DEVICE;
		dma_unmap_sg(dma_chan->device->dev,
			     host->data->sg, host->data->sg_len,
			     dir_data);

		bcm2835_sdhost_finish_data(host);
	}

	return handled;
}


static irqreturn_t bcm2835_sdhost_irq(int irq, void *dev_id)
{
	irqreturn_t result = IRQ_NONE;
	struct bcm2835_host *host = dev_id;
	u32 unexpected = 0, early = 0;
	int loops = 0;

	spin_lock(&host->lock);

	for (loops = 0; loops < 1; loops++) {
		u32 intmask, handled;

		intmask = bcm2835_sdhost_read(host, SDHSTS);
		handled = intmask & (SDHSTS_BUSY_IRPT |
				     SDHSTS_BLOCK_IRPT |
				     SDHSTS_SDIO_IRPT |
				     SDHSTS_DATA_FLAG);
		if ((handled == SDHSTS_DATA_FLAG) &&
		    (loops == 0) && !host->data) {
			pr_err("%s: sdhost_irq data interrupt 0x%08x even "
			       "though no data operation was in progress.\n",
			       mmc_hostname(host->mmc),
			       (unsigned)intmask);

			bcm2835_sdhost_dumpregs(host);
		}

		if (!handled)
			break;

		if (loops)
			early |= handled;

		result = IRQ_HANDLED;

		/* Clear all interrupts and notifications */
		bcm2835_sdhost_write(host, intmask, SDHSTS);

		if (intmask & SDHSTS_BUSY_IRPT)
			handled |= bcm2835_sdhost_busy_irq(host, intmask);

		/* There is no true data interrupt status bit, so it is
		   necessary to qualify the data flag with the interrupt
		   enable bit */
		if ((intmask & SDHSTS_DATA_FLAG) &&
		    (host->hcfg & SDHCFG_DATA_IRPT_EN))
			handled |= bcm2835_sdhost_data_irq(host, intmask);

		if (intmask & SDHSTS_BLOCK_IRPT)
			handled |= bcm2835_sdhost_block_irq(host, intmask);

		if (intmask & SDHSTS_SDIO_IRPT) {
			bcm2835_sdhost_enable_sdio_irq_nolock(host, false);
			host->thread_isr |= SDHSTS_SDIO_IRPT;
			result = IRQ_WAKE_THREAD;
		}

		unexpected |= (intmask & ~handled);
	}

	mmiowb();

	spin_unlock(&host->lock);

	if (early)
		pr_debug("%s: early %x (loops %d)\n",
			 mmc_hostname(host->mmc), early, loops);

	if (unexpected) {
		pr_err("%s: unexpected interrupt 0x%08x.\n",
			   mmc_hostname(host->mmc), unexpected);
		bcm2835_sdhost_dumpregs(host);
	}

	return result;
}

static irqreturn_t bcm2835_sdhost_thread_irq(int irq, void *dev_id)
{
	struct bcm2835_host *host = dev_id;
	unsigned long flags;
	u32 isr;

	spin_lock_irqsave(&host->lock, flags);
	isr = host->thread_isr;
	host->thread_isr = 0;
	spin_unlock_irqrestore(&host->lock, flags);

	if (isr & SDHSTS_SDIO_IRPT) {
		sdio_run_irqs(host->mmc);

/* Is this necessary? Why re-enable an interrupt which is enabled?
		spin_lock_irqsave(&host->lock, flags);
		if (host->flags & SDHSTS_SDIO_IRPT_ENABLED)
			bcm2835_sdhost_enable_sdio_irq_nolock(host, true);
		spin_unlock_irqrestore(&host->lock, flags);
*/
	}

	return isr ? IRQ_HANDLED : IRQ_NONE;
}



void bcm2835_sdhost_set_clock(struct bcm2835_host *host, unsigned int clock)
{
	int div = 0; /* Initialized for compiler warning */
	unsigned int input_clock = clock;

	if (host->debug)
		pr_info("%s: set_clock(%d)\n", mmc_hostname(host->mmc), clock);

	if ((host->overclock_50 > 50) &&
	    (clock == 50*MHZ)) {
		clock = host->overclock_50 * MHZ + (MHZ - 1);
	}

	/* The SDCDIV register has 11 bits, and holds (div - 2).
	   But in data mode the max is 50MHz wihout a minimum, and only the
	   bottom 3 bits are used. Since the switch over is automatic (unless
	   we have marked the card as slow...), chosen values have to make
	   sense in both modes.
	   Ident mode must be 100-400KHz, so can range check the requested
	   clock. CMD15 must be used to return to data mode, so this can be
	   monitored.

	   clock 250MHz -> 0->125MHz, 1->83.3MHz, 2->62.5MHz, 3->50.0MHz
                           4->41.7MHz, 5->35.7MHz, 6->31.3MHz, 7->27.8MHz

			 623->400KHz/27.8MHz
			 reset value (507)->491159/50MHz

	   BUT, the 3-bit clock divisor in data mode is too small if the
	   core clock is higher than 250MHz, so instead use the SLOW_CARD
	   configuration bit to force the use of the ident clock divisor
	   at all times.
	*/

	host->mmc->actual_clock = 0;

	if (clock < 100000) {
	    /* Can't stop the clock, but make it as slow as possible
	     * to show willing
	     */
	    host->cdiv = SDCDIV_MAX_CDIV;
	    bcm2835_sdhost_write(host, host->cdiv, SDCDIV);
	    return;
	}

	div = host->max_clk / clock;
	if (div < 2)
		div = 2;
	if ((host->max_clk / div) > clock)
		div++;
	div -= 2;

	if (div > SDCDIV_MAX_CDIV)
	    div = SDCDIV_MAX_CDIV;

	clock = host->max_clk / (div + 2);
	host->mmc->actual_clock = clock;

	if (clock > input_clock) {
		/* Save the closest value, to make it easier
		   to reduce in the event of error */
		host->overclock_50 = (clock/MHZ);

		if (clock != host->overclock) {
			pr_warn("%s: overclocking to %dHz\n",
				mmc_hostname(host->mmc), clock);
			host->overclock = clock;
		}
	}
	else if (host->overclock)
	{
		host->overclock = 0;
		if (clock == 50 * MHZ)
			pr_warn("%s: cancelling overclock\n",
				mmc_hostname(host->mmc));
	}

	host->cdiv = div;
	bcm2835_sdhost_write(host, host->cdiv, SDCDIV);

	/* Set the timeout to 250ms */
	bcm2835_sdhost_write(host, host->mmc->actual_clock/4, SDTOUT);

	if (host->debug)
		pr_info("%s: clock=%d -> max_clk=%d, cdiv=%x (actual clock %d)\n",
			mmc_hostname(host->mmc), input_clock,
			host->max_clk, host->cdiv, host->mmc->actual_clock);
}

static void bcm2835_sdhost_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct bcm2835_host *host;
	unsigned long flags;

	host = mmc_priv(mmc);

	if (host->debug) {
		struct mmc_command *cmd = mrq->cmd;
		BUG_ON(!cmd);
		if (cmd->data)
			pr_info("%s: cmd %d 0x%x (flags 0x%x) - %s %d*%d\n",
				mmc_hostname(mmc),
				cmd->opcode, cmd->arg, cmd->flags,
				(cmd->data->flags & MMC_DATA_READ) ?
				"read" : "write", cmd->data->blocks,
				cmd->data->blksz);
		else
			pr_info("%s: cmd %d 0x%x (flags 0x%x)\n",
				mmc_hostname(mmc),
				cmd->opcode, cmd->arg, cmd->flags);
	}

	/* Reset the error statuses in case this is a retry */
	if (mrq->cmd)
		mrq->cmd->error = 0;
	if (mrq->data)
		mrq->data->error = 0;
	if (mrq->stop)
		mrq->stop->error = 0;

	if (mrq->data && !is_power_of_2(mrq->data->blksz)) {
		pr_err("%s: unsupported block size (%d bytes)\n",
		       mmc_hostname(mmc), mrq->data->blksz);
		mrq->cmd->error = -EINVAL;
		mmc_request_done(mmc, mrq);
		return;
	}

	spin_lock_irqsave(&host->lock, flags);

	WARN_ON(host->mrq != NULL);

	host->mrq = mrq;

	if (mrq->sbc)
		bcm2835_sdhost_send_command(host, mrq->sbc);
	else
		bcm2835_sdhost_send_command(host, mrq->cmd);

	mmiowb();
	spin_unlock_irqrestore(&host->lock, flags);

	if (!mrq->sbc && mrq->cmd->data && host->use_dma)
		/* DMA transfer starts now, PIO starts after irq */
		bcm2835_sdhost_transfer_dma(host);

	if (!host->use_busy)
		bcm2835_sdhost_finish_command(host);
}


static void bcm2835_sdhost_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{

	struct bcm2835_host *host = mmc_priv(mmc);
	unsigned long flags;

	if (host->debug)
		pr_info("%s: ios clock %d, pwr %d, bus_width %d, "
			"timing %d, vdd %d, drv_type %d\n",
			mmc_hostname(mmc),
			ios->clock, ios->power_mode, ios->bus_width,
			ios->timing, ios->signal_voltage, ios->drv_type);

	spin_lock_irqsave(&host->lock, flags);

	if (!ios->clock || ios->clock != host->clock) {
		bcm2835_sdhost_set_clock(host, ios->clock);
		host->clock = ios->clock;
	}

	/* set bus width */
	host->hcfg &= ~SDHCFG_WIDE_EXT_BUS;
	if (ios->bus_width == MMC_BUS_WIDTH_4)
		host->hcfg |= SDHCFG_WIDE_EXT_BUS;

	host->hcfg |= SDHCFG_WIDE_INT_BUS;

	/* Disable clever clock switching, to cope with fast core clocks */
	host->hcfg |= SDHCFG_SLOW_CARD;

	bcm2835_sdhost_write(host, host->hcfg, SDHCFG);

	mmiowb();

	spin_unlock_irqrestore(&host->lock, flags);
}

static int bcm2835_sdhost_multi_io_quirk(struct mmc_card *card,
					 unsigned int direction,
					 u32 blk_pos, int blk_size)
{
	/* There is a bug in the host controller hardware that makes
	   reading the final sector of the card as part of a multiple read
	   problematic. Detect that case and shorten the read accordingly.
	*/
	struct bcm2835_host *host;

	host = mmc_priv(card->host);

	if (direction == MMC_DATA_READ)
	{
		int i;
		int sector;
		for (i = 0; blk_pos > (sector = host->single_read_sectors[i]); i++)
			continue;

		if ((blk_pos + blk_size) > sector)
			blk_size = (blk_pos == sector) ? 1 : (sector - blk_pos);
	}
	return blk_size;
}


static struct mmc_host_ops bcm2835_sdhost_ops = {
	.request = bcm2835_sdhost_request,
	.set_ios = bcm2835_sdhost_set_ios,
	.enable_sdio_irq = bcm2835_sdhost_enable_sdio_irq,
	.hw_reset = bcm2835_sdhost_reset,
	.multi_io_quirk = bcm2835_sdhost_multi_io_quirk,
};


static void bcm2835_sdhost_tasklet_finish(unsigned long param)
{
	struct bcm2835_host *host;
	unsigned long flags;
	struct mmc_request *mrq;

	host = (struct bcm2835_host *)param;

	spin_lock_irqsave(&host->lock, flags);

	/*
	 * If this tasklet gets rescheduled while running, it will
	 * be run again afterwards but without any active request.
	 */
	if (!host->mrq) {
		spin_unlock_irqrestore(&host->lock, flags);
		return;
	}

	del_timer(&host->timer);

	mrq = host->mrq;

	/* Drop the overclock after any data corruption, or after any
	   error overclocked */
	if (host->overclock) {
		if ((mrq->cmd && mrq->cmd->error) ||
		    (mrq->data && mrq->data->error) ||
		    (mrq->stop && mrq->stop->error)) {
			host->overclock_50--;
			pr_warn("%s: reducing overclock due to errors\n",
				mmc_hostname(host->mmc));
			bcm2835_sdhost_set_clock(host,50*MHZ);
			mrq->cmd->error = -EILSEQ;
			mrq->cmd->retries = 1;
		}
	}

	host->mrq = NULL;
	host->cmd = NULL;
	host->data = NULL;

	mmiowb();

	spin_unlock_irqrestore(&host->lock, flags);
	mmc_request_done(host->mmc, mrq);
}



int bcm2835_sdhost_add_host(struct bcm2835_host *host)
{
	struct mmc_host *mmc;
	struct dma_slave_config cfg;
	char pio_limit_string[20];
	int ret;

	mmc = host->mmc;

	bcm2835_sdhost_reset_internal(host);

	mmc->f_max = host->max_clk;
	mmc->f_min = host->max_clk / SDCDIV_MAX_CDIV;

	mmc->max_busy_timeout =  (~(unsigned int)0)/(mmc->f_max/1000);

	pr_debug("f_max %d, f_min %d, max_busy_timeout %d\n",
		 mmc->f_max, mmc->f_min, mmc->max_busy_timeout);

	/* host controller capabilities */
	mmc->caps |= /* MMC_CAP_SDIO_IRQ |*/ MMC_CAP_4_BIT_DATA |
		MMC_CAP_SD_HIGHSPEED | MMC_CAP_MMC_HIGHSPEED |
		MMC_CAP_NEEDS_POLL | MMC_CAP_HW_RESET | MMC_CAP_ERASE |
		(ALLOW_CMD23 * MMC_CAP_CMD23);

	spin_lock_init(&host->lock);

	if (host->allow_dma) {
		if (IS_ERR_OR_NULL(host->dma_chan_tx) ||
		    IS_ERR_OR_NULL(host->dma_chan_rx)) {
			pr_err("%s: unable to initialise DMA channels. "
			       "Falling back to PIO\n",
			       mmc_hostname(mmc));
			host->have_dma = false;
		} else {
			host->have_dma = true;

			cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
			cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
			cfg.slave_id = 13;		/* DREQ channel */

			cfg.direction = DMA_MEM_TO_DEV;
			cfg.src_addr = 0;
			cfg.dst_addr = host->bus_addr + SDDATA;
			ret = dmaengine_slave_config(host->dma_chan_tx, &cfg);

			cfg.direction = DMA_DEV_TO_MEM;
			cfg.src_addr = host->bus_addr + SDDATA;
			cfg.dst_addr = 0;
			ret = dmaengine_slave_config(host->dma_chan_rx, &cfg);
		}
	} else {
		host->have_dma = false;
	}

	mmc->max_segs = 128;
	mmc->max_req_size = 524288;
	mmc->max_seg_size = mmc->max_req_size;
	mmc->max_blk_size = 512;
	mmc->max_blk_count =  65535;

	/* report supported voltage ranges */
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;

	tasklet_init(&host->finish_tasklet,
		bcm2835_sdhost_tasklet_finish, (unsigned long)host);

	setup_timer(&host->timer, bcm2835_sdhost_timeout,
		    (unsigned long)host);

	setup_timer(&host->pio_timer, bcm2835_sdhost_pio_timeout,
		    (unsigned long)host);

	bcm2835_sdhost_init(host, 0);
	ret = request_threaded_irq(host->irq, bcm2835_sdhost_irq,
				   bcm2835_sdhost_thread_irq,
				   IRQF_SHARED,	mmc_hostname(mmc), host);
	if (ret) {
		pr_err("%s: failed to request IRQ %d: %d\n",
		       mmc_hostname(mmc), host->irq, ret);
		goto untasklet;
	}

	mmiowb();
	mmc_add_host(mmc);

	pio_limit_string[0] = '\0';
	if (host->have_dma && (host->pio_limit > 0))
		sprintf(pio_limit_string, " (>%d)", host->pio_limit);
	pr_info("%s: %s loaded - DMA %s%s\n",
		mmc_hostname(mmc), DRIVER_NAME,
		host->have_dma ? "enabled" : "disabled",
		pio_limit_string);

	return 0;

untasklet:
	tasklet_kill(&host->finish_tasklet);

	return ret;
}

static int bcm2835_sdhost_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct clk *clk;
	struct resource *iomem;
	struct bcm2835_host *host;
	struct mmc_host *mmc;
	const __be32 *addr;
	int ret;

	pr_debug("bcm2835_sdhost_probe\n");
	mmc = mmc_alloc_host(sizeof(*host), dev);
	if (!mmc)
		return -ENOMEM;

	mmc->ops = &bcm2835_sdhost_ops;
	host = mmc_priv(mmc);
	host->mmc = mmc;
	host->pio_timeout = msecs_to_jiffies(500);
	host->max_delay = 1; /* Warn if over 1ms */
	spin_lock_init(&host->lock);

	iomem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	host->ioaddr = devm_ioremap_resource(dev, iomem);
	if (IS_ERR(host->ioaddr)) {
		ret = PTR_ERR(host->ioaddr);
		goto err;
	}

	addr = of_get_address(node, 0, NULL, NULL);
	if (!addr) {
		dev_err(dev, "could not get DMA-register address\n");
		return -ENODEV;
	}
	host->bus_addr = be32_to_cpup(addr);
	pr_debug(" - ioaddr %lx, iomem->start %lx, bus_addr %lx\n",
		 (unsigned long)host->ioaddr,
		 (unsigned long)iomem->start,
		 (unsigned long)host->bus_addr);

	host->allow_dma = ALLOW_DMA;

	if (node) {
		/* Read any custom properties */
		of_property_read_u32(node,
				     "brcm,delay-after-stop",
				     &host->delay_after_stop);
		of_property_read_u32(node,
				     "brcm,overclock-50",
				     &host->overclock_50);
		of_property_read_u32(node,
				     "brcm,pio-limit",
				     &host->pio_limit);
		host->allow_dma = ALLOW_DMA &&
			!of_property_read_bool(node, "brcm,force-pio");
		host->debug = of_property_read_bool(node, "brcm,debug");
	}

	if (host->allow_dma) {
		if (node) {
			host->dma_chan_tx =
				dma_request_slave_channel(dev, "tx");
			host->dma_chan_rx =
				dma_request_slave_channel(dev, "rx");
		} else {
			dma_cap_mask_t mask;

			dma_cap_zero(mask);
			/* we don't care about the channel, any would work */
			dma_cap_set(DMA_SLAVE, mask);
			host->dma_chan_tx =
				dma_request_channel(mask, NULL, NULL);
			host->dma_chan_rx =
				dma_request_channel(mask, NULL, NULL);
		}
	}

	clk = devm_clk_get(dev, NULL);
	if (IS_ERR(clk)) {
		dev_err(dev, "could not get clk\n");
		ret = PTR_ERR(clk);
		goto err;
	}

	host->max_clk = clk_get_rate(clk);

	host->irq = platform_get_irq(pdev, 0);
	if (host->irq <= 0) {
		dev_err(dev, "get IRQ failed\n");
		ret = -EINVAL;
		goto err;
	}

	pr_debug(" - max_clk %lx, irq %d\n",
		 (unsigned long)host->max_clk,
		 (int)host->irq);

	if (node)
		mmc_of_parse(mmc);
	else
		mmc->caps |= MMC_CAP_4_BIT_DATA;

	ret = bcm2835_sdhost_add_host(host);
	if (ret)
		goto err;

	platform_set_drvdata(pdev, host);

	pr_debug("bcm2835_sdhost_probe -> OK\n");

	return 0;

err:
	pr_debug("bcm2835_sdhost_probe -> err %d\n", ret);
	mmc_free_host(mmc);

	return ret;
}

static int bcm2835_sdhost_remove(struct platform_device *pdev)
{
	struct bcm2835_host *host = platform_get_drvdata(pdev);

	pr_debug("bcm2835_sdhost_remove\n");

	mmc_remove_host(host->mmc);

	bcm2835_sdhost_set_power(host, false);

	free_irq(host->irq, host);

	del_timer_sync(&host->timer);

	tasklet_kill(&host->finish_tasklet);

	mmc_free_host(host->mmc);
	platform_set_drvdata(pdev, NULL);

	pr_debug("bcm2835_sdhost_remove - OK\n");
	return 0;
}


static const struct of_device_id bcm2835_sdhost_match[] = {
	{ .compatible = "brcm,bcm2835-sdhost" },
	{ }
};
MODULE_DEVICE_TABLE(of, bcm2835_sdhost_match);



static struct platform_driver bcm2835_sdhost_driver = {
	.probe      = bcm2835_sdhost_probe,
	.remove     = bcm2835_sdhost_remove,
	.driver     = {
		.name		= DRIVER_NAME,
		.owner		= THIS_MODULE,
		.of_match_table	= bcm2835_sdhost_match,
	},
};
module_platform_driver(bcm2835_sdhost_driver);

MODULE_ALIAS("platform:sdhost-bcm2835");
MODULE_DESCRIPTION("BCM2835 SDHost driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Phil Elwell");
