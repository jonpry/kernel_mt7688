/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  publishhed by the Free Software Foundation.
 *
 *  Copyright (C) 2015 John Crispin <blogic@openwrt.org>
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/mtd/mtd.h>
#include <linux/gpio.h>

#define LINKIT_LATCH_GPIO	11

struct linkit_hw_data {
	char board[16];
	char rev[16];
};

static void sanify_string(char *s)
{
	int i;

	for (i = 0; i < 15; i++)
		if (s[i] <= 0x20)
			s[i] = '\0';
	s[15] = '\0';
}

static int linkit_probe(struct platform_device *pdev)
{
	struct linkit_hw_data hw;
	struct mtd_info *mtd;
	size_t retlen;
	int ret;

	mtd = get_mtd_device_nm("factory");
	if (IS_ERR(mtd))
		return PTR_ERR(mtd);

	ret = mtd_read(mtd, 0x400, sizeof(hw), &retlen, (u_char *) &hw);
	put_mtd_device(mtd);

	sanify_string(hw.board);
	sanify_string(hw.rev);

	dev_info(&pdev->dev, "Version  : %s\n", hw.board);
	dev_info(&pdev->dev, "Revision : %s\n", hw.rev);

	if (!strcmp(hw.board, "LINKITS7688")) {
		dev_info(&pdev->dev, "setting up bootstrap latch\n");

		if (devm_gpio_request(&pdev->dev, LINKIT_LATCH_GPIO, "bootstrap")) {
			dev_err(&pdev->dev, "failed to setup bootstrap gpio\n");
			return -1;
		}
		gpio_direction_output(LINKIT_LATCH_GPIO, 0);
	}

	return 0;
}

static const struct of_device_id linkit_match[] = {
	{ .compatible = "mediatek,linkit" },
	{},
};
MODULE_DEVICE_TABLE(of, linkit_match);

static struct platform_driver linkit_driver = {
	.probe = linkit_probe,
	.driver = {
		.name = "mtk-linkit",
		.owner = THIS_MODULE,
		.of_match_table = linkit_match,
	},
};

int __init linkit_init(void)
{
	return platform_driver_register(&linkit_driver);
}
late_initcall_sync(linkit_init);
