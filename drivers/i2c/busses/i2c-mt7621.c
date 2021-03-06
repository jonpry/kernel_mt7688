/*
 * drivers/i2c/busses/i2c-mt7621.c
 *
 * Copyright (C) 2013 Steven Liu <steven_liu@mediatek.com>
 * Copyright (C) 2016 Michael Lee <igvtee@gmail.com>
 *
 * Improve driver for i2cdetect from i2c-tools to detect i2c devices on the bus.
 * (C) 2014 Sittisak <sittisaks@hotmail.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/reset.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/clk.h>

#define REG_SM0CFG0		0x08
#define REG_SM0DOUT		0x10
#define REG_SM0DIN		0x14
#define REG_SM0ST		0x18
#define REG_SM0AUTO		0x1C
#define REG_SM0CFG1		0x20
#define REG_SM0CFG2		0x28
#define REG_SM0CTL0		0x40
#define REG_SM0CTL1		0x44
#define REG_SM0D0		0x50
#define REG_SM0D1		0x54
#define REG_PINTEN		0x5C
#define REG_PINTST		0x60
#define REG_PINTCL		0x64

/* REG_SM0CFG0 */
#define I2C_DEVADDR_MASK	0x7f

/* REG_SM0ST */
#define I2C_DATARDY		BIT(2)
#define I2C_SDOEMPTY		BIT(1)
#define I2C_BUSY		BIT(0)

/* REG_SM0AUTO */
#define READ_CMD		BIT(0)

/* REG_SM0CFG1 */
#define BYTECNT_MAX		64
#define SET_BYTECNT(x)		(x - 1)

/* REG_SM0CFG2 */
#define AUTOMODE_EN		BIT(0)

/* REG_SM0CTL0 */
#define ODRAIN_HIGH_SM0		BIT(31)
#define VSYNC_SHIFT		28
#define VSYNC_MASK		0x3
#define VSYNC_PULSE		(0x1 << VSYNC_SHIFT)
#define VSYNC_RISING		(0x2 << VSYNC_SHIFT)
#define CLK_DIV_SHIFT		16
#define CLK_DIV_MASK		0xfff
#define DEG_CNT_SHIFT		8
#define DEG_CNT_MASK		0xff
#define WAIT_HIGH		BIT(6)
#define DEG_EN			BIT(5)
#define CS_STATUA		BIT(4)
#define SCL_STATUS		BIT(3)
#define SDA_STATUS		BIT(2)
#define SM0_EN			BIT(1)
#define SCL_STRECH		BIT(0)

/* REG_SM0CTL1 */
#define ACK_SHIFT		16
#define ACK_MASK		0xff
#define PGLEN_SHIFT		8
#define PGLEN_MASK		0x7
#define SM0_MODE_SHIFT		4
#define SM0_MODE_MASK		0x7
#define SM0_MODE_START		0x1
#define SM0_MODE_WRITE		0x2
#define SM0_MODE_STOP		0x3
#define SM0_MODE_READ_NACK	0x4
#define SM0_MODE_READ_ACK	0x5
#define SM0_TRI_BUSY		BIT(0)

/* timeout waiting for I2C devices to respond (clock streching) */
#define TIMEOUT_MS              1000
#define DELAY_INTERVAL_US       100

struct mtk_i2c {
	void __iomem *base;
	struct clk *clk;
	struct device *dev;
	struct i2c_adapter adap;
	u32 cur_clk;
	u32 clk_div;
	u32 flags;
};

static void mtk_i2c_w32(struct mtk_i2c *i2c, u32 val, unsigned reg)
{
	iowrite32(val, i2c->base + reg);
}

static u32 mtk_i2c_r32(struct mtk_i2c *i2c, unsigned reg)
{
	return ioread32(i2c->base + reg);
}

static int poll_down_timeout(void __iomem *addr, u32 mask)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(TIMEOUT_MS);

	do {
		if (!(readl_relaxed(addr) & mask))
			return 0;

		usleep_range(DELAY_INTERVAL_US, DELAY_INTERVAL_US + 50);
	} while (time_before(jiffies, timeout));

	return (readl_relaxed(addr) & mask) ? -EAGAIN : 0;
}

static int mtk_i2c_wait_idle(struct mtk_i2c *i2c)
{
	int ret;

	ret = poll_down_timeout(i2c->base + REG_SM0ST, I2C_BUSY);
	if (ret < 0)
		dev_dbg(i2c->dev, "idle err(%d)\n", ret);

	return ret;
}

static int poll_up_timeout(void __iomem *addr, u32 mask)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(TIMEOUT_MS);
	u32 status;

	do {
		status = readl_relaxed(addr);
		if (status & mask)
			return 0;
		usleep_range(DELAY_INTERVAL_US, DELAY_INTERVAL_US + 50);
	} while (time_before(jiffies, timeout));

	return -ETIMEDOUT;
}

static int mtk_i2c_wait_done(struct mtk_i2c *i2c)
{
	int ret;

	ret = poll_down_timeout(i2c->base + REG_SM0CTL1, SM0_TRI_BUSY);
	if (ret < 0)
		dev_dbg(i2c->dev, "rx err(%d)\n", ret);

	return ret;
}

static void mtk_i2c_reset(struct mtk_i2c *i2c)
{
	u32 reg;
	device_reset(i2c->adap.dev.parent);
	barrier();

	/* ctrl0 */
	reg = ODRAIN_HIGH_SM0 | VSYNC_PULSE | (i2c->clk_div << CLK_DIV_SHIFT) |
		WAIT_HIGH | SM0_EN;
	mtk_i2c_w32(i2c, reg, REG_SM0CTL0);

	/* auto mode */
	mtk_i2c_w32(i2c, 0, REG_SM0CFG2);
}

static void mtk_i2c_dump_reg(struct mtk_i2c *i2c)
{
	dev_dbg(i2c->dev, "cfg0 %08x, dout %08x, din %08x, " \
			"status %08x, auto %08x, cfg1 %08x, " \
			"cfg2 %08x, ctl0 %08x, ctl1 %08x\n",
			mtk_i2c_r32(i2c, REG_SM0CFG0),
			mtk_i2c_r32(i2c, REG_SM0DOUT),
			mtk_i2c_r32(i2c, REG_SM0DIN),
			mtk_i2c_r32(i2c, REG_SM0ST),
			mtk_i2c_r32(i2c, REG_SM0AUTO),
			mtk_i2c_r32(i2c, REG_SM0CFG1),
			mtk_i2c_r32(i2c, REG_SM0CFG2),
			mtk_i2c_r32(i2c, REG_SM0CTL0),
			mtk_i2c_r32(i2c, REG_SM0CTL1));
}

static void mtk_i2c_read(struct mtk_i2c *i2c, int len, uint8_t* buf){
        int pkt_num=0;
        int i;
        uint64_t d;
   	while(len>0){
                int plen=len; 
                if(plen > 8)
                  plen = 8;
	     	mtk_i2c_w32(i2c, (0xFF<<16) | ((plen-1) << 8) | (SM0_MODE_READ_ACK << 4) | 1, REG_SM0CTL1);
		mtk_i2c_wait_done(i2c);
                d = mtk_i2c_r32(i2c,REG_SM0D0) | ((uint64_t)mtk_i2c_r32(i2c,REG_SM0D1)<<32UL);
                for(i=0; i < plen; i++){
                   buf[i+pkt_num*8] = d >> (8*i);
                }
           	len -= plen;
                pkt_num++;
	}
}

static int mtk_i2c_master_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
		int num)
{
	struct mtk_i2c *i2c;
	struct i2c_msg *pmsg;
	int i, j, ret;
        int pkt_num, plen,len;

	i2c = i2c_get_adapdata(adap);

	for (i = 0; i < num; i++) {
		pmsg = &msgs[i];

		dev_dbg(i2c->dev, "addr: 0x%x, len: %d, flags: 0x%x\n",
				pmsg->addr, pmsg->len, pmsg->flags);

		/* wait hardware idle */
		if ((ret = mtk_i2c_wait_idle(i2c)))
			goto err_timeout;

		if (pmsg->flags & I2C_M_TEN) {
			printk("10 bits addr not supported\n");
			return -EINVAL;
		} else {
			/* 7 bits address */
			mtk_i2c_w32(i2c, pmsg->addr & I2C_DEVADDR_MASK,
					REG_SM0CFG0);
		}

		/* buffer length */
		if (pmsg->len == 0) {
			printk("length is 0\n");
			return -EINVAL;
		}
//		mtk_i2c_w32(i2c, SET_BYTECNT(pmsg->len),
//					REG_SM0CFG1);

                //Issue start
                mtk_i2c_w32(i2c, (0xFF<<16) | (0 << 8) | (SM0_MODE_START << 4) | 1, REG_SM0CTL1);
		mtk_i2c_wait_done(i2c);
      
                //Address on the b
                mtk_i2c_w32(i2c, ((pmsg->addr << 1) | ((pmsg->flags & I2C_M_RD)?1:0))& 0xFF, REG_SM0D0);
                mtk_i2c_w32(i2c, (0xFF<<16) | (0 << 8) | (SM0_MODE_WRITE << 4) | 1, REG_SM0CTL1);
         	mtk_i2c_wait_done(i2c);
                
		j = 0;
                if (pmsg->flags & I2C_M_RECV_LEN) {
                       mtk_i2c_read(i2c, 1, pmsg->buf);
                       if(pmsg->buf[0] < I2C_SMBUS_BLOCK_MAX && pmsg->buf[0] > 0)
                          mtk_i2c_read(i2c, pmsg->buf[0], &pmsg->buf[1]);
                       else{
                          printk("RECV_LEN bogus %d\n", pmsg->buf[0]);  
	                  mtk_i2c_w32(i2c, (0xFF<<16) | (0 << 8) | (SM0_MODE_STOP << 4) | 1, REG_SM0CTL1);
			  mtk_i2c_wait_done(i2c);                          
                          return -EINVAL;
                       }
                }else if (pmsg->flags & I2C_M_RD) {
                       mtk_i2c_read(i2c, pmsg->len, pmsg->buf);
		} else {
			plen = pmsg->len;
			pkt_num = 0;
               		while(plen>0){
				len = plen;
				if(len>8)
					len = 8;
	                        uint64_t verb=0;
       	                 	for(j=0; j < len; j++){
					verb |= pmsg->buf[j+pkt_num*8] << (j*8);
       	                 	}
       	                 	mtk_i2c_w32(i2c, verb, REG_SM0D0);
       	                 	mtk_i2c_w32(i2c, verb>>32, REG_SM0D1);

                     		mtk_i2c_w32(i2c, (0xFF<<16) | (((pmsg->len-1)&0x7) << 8) | (SM0_MODE_WRITE << 4) | 1, REG_SM0CTL1);
              			mtk_i2c_wait_done(i2c);
				plen -= len;
				pkt_num++;
			}
		}

                mtk_i2c_w32(i2c, (0xFF<<16) | (0 << 8) | (SM0_MODE_STOP << 4) | 1, REG_SM0CTL1);
		mtk_i2c_wait_done(i2c);
	}
	/* the return value is number of executed messages */
	ret = i;

	return ret;

err_timeout:
        printk("i2c timeout\n");
	mtk_i2c_dump_reg(i2c);
	mtk_i2c_reset(i2c);
	return ret;
}

static u32 mtk_i2c_func(struct i2c_adapter *a)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm mtk_i2c_algo = {
	.master_xfer	= mtk_i2c_master_xfer,
	.functionality	= mtk_i2c_func,
};

static const struct of_device_id i2c_mtk_dt_ids[] = {
	{ .compatible = "mediatek,mt7621-i2c" },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, i2c_mtk_dt_ids);

static struct i2c_adapter_quirks mtk_i2c_quirks = {
        .max_write_len = BYTECNT_MAX,
        .max_read_len = BYTECNT_MAX,
};

static void mtk_i2c_init(struct mtk_i2c *i2c)
{
	i2c->clk_div = clk_get_rate(i2c->clk) / i2c->cur_clk;
	if (i2c->clk_div > CLK_DIV_MASK)
		i2c->clk_div = CLK_DIV_MASK;

	mtk_i2c_reset(i2c); 
}

static int mtk_i2c_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct mtk_i2c *i2c;
	struct i2c_adapter *adap;
	const struct of_device_id *match;
	int ret;

	match = of_match_device(i2c_mtk_dt_ids, &pdev->dev);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "no memory resource found\n");
		return -ENODEV;
	}

	i2c = devm_kzalloc(&pdev->dev, sizeof(struct mtk_i2c), GFP_KERNEL);
	if (!i2c) {
		dev_err(&pdev->dev, "failed to allocate i2c_adapter\n");
		return -ENOMEM;
	}

	i2c->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(i2c->base))
		return PTR_ERR(i2c->base);

	i2c->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(i2c->clk)) {
		dev_err(&pdev->dev, "no clock defined\n");
		return -ENODEV;
	}
	clk_prepare_enable(i2c->clk);
	i2c->dev = &pdev->dev;

	if (of_property_read_u32(pdev->dev.of_node,
				"clock-frequency", &i2c->cur_clk))
		i2c->cur_clk = 400000;

	adap = &i2c->adap;
	adap->owner = THIS_MODULE;
	adap->class = I2C_CLASS_HWMON | I2C_CLASS_SPD;
	adap->algo = &mtk_i2c_algo;
	adap->retries = 3;
	adap->dev.parent = &pdev->dev;
	i2c_set_adapdata(adap, i2c);
	adap->dev.of_node = pdev->dev.of_node;
	strlcpy(adap->name, dev_name(&pdev->dev), sizeof(adap->name));
	adap->quirks = &mtk_i2c_quirks;

	platform_set_drvdata(pdev, i2c);

	mtk_i2c_init(i2c);

	ret = i2c_add_adapter(adap);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to add adapter\n");
		clk_disable_unprepare(i2c->clk);
		return ret;
	}

	dev_info(&pdev->dev, "clock %uKHz, re-start not support\n",
			i2c->cur_clk/1000);

	return ret;
}

static int mtk_i2c_remove(struct platform_device *pdev)
{
	struct mtk_i2c *i2c = platform_get_drvdata(pdev);

	i2c_del_adapter(&i2c->adap);
	clk_disable_unprepare(i2c->clk);

	return 0;
}

static struct platform_driver mtk_i2c_driver = {
	.probe		= mtk_i2c_probe,
	.remove		= mtk_i2c_remove,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "i2c-mt7621",
		.of_match_table = i2c_mtk_dt_ids,
	},
};

static int __init i2c_mtk_init (void)
{
	return platform_driver_register(&mtk_i2c_driver);
}
subsys_initcall(i2c_mtk_init);

static void __exit i2c_mtk_exit (void)
{
	platform_driver_unregister(&mtk_i2c_driver);
}
module_exit(i2c_mtk_exit);

MODULE_AUTHOR("Steven Liu <steven_liu@mediatek.com>");
MODULE_DESCRIPTION("MT7621 I2c host driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:MT7621-I2C");
