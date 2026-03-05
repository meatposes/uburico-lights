// SPDX-License-Identifier: GPL-2.0-only
/*
 * orico_cf1000_leds.c — ORICO CF1000 NAS bay LED kernel driver
 *
 * Exposes all 24 LEDs (10 bays × blue+red, plus network and system
 * indicators) via the Linux LED class:
 *
 *   /sys/class/leds/cf1000:bay1:blue/brightness
 *   /sys/class/leds/cf1000:bay1:red/brightness
 *   ...
 *   /sys/class/leds/cf1000:bay10:blue/brightness
 *   /sys/class/leds/cf1000:bay10:red/brightness
 *   /sys/class/leds/cf1000:network:blue/brightness
 *   /sys/class/leds/cf1000:network:red/brightness
 *   /sys/class/leds/cf1000:system:blue/brightness
 *   /sys/class/leds/cf1000:system:red/brightness
 *
 * Hardware: Intel Alder Lake-N PCH GPIO PADCFG0 registers, two MMIO
 * regions are ioremap'd at init time:
 *
 *   Region A  0xFD6A0000  bays 6-10, network LED
 *   Region D  0xFD6D0000  bays 1-5,  system LED
 *
 * NOTE ON PINCTRL BYPASS
 * The pinctrl-alderlake driver already owns these GPIO pads.  We do
 * NOT use the gpiod/pinctrl subsystem — instead we write PADCFG0
 * registers directly via ioremap, exactly as the OEM firmware does.
 * This is intentional; do not replace ioremap with gpiod_get().
 *
 * Register data sourced from /oem/board-config on the original
 * siyouyunOS image.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/leds.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/err.h>

MODULE_AUTHOR("uburico-lights contributors");
MODULE_DESCRIPTION("ORICO CF1000 NAS bay LED driver — PCH GPIO PADCFG0 MMIO");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0.0");

/* -------------------------------------------------------------------------
 * Physical memory layout
 * ------------------------------------------------------------------------- */

#define REGION_A_BASE   0xFD6A0000UL   /* bays 6-10, network */
#define REGION_D_BASE   0xFD6D0000UL   /* bays 1-5,  system  */
#define REGION_SIZE     0x1000UL       /* 4 KiB — all used offsets fit */

/*
 * PADCFG0 values.  "Normal" pads: TX=1 → LED on.
 * "Inverted" pads (GPP_H4, GPP_H5, GPP_H7, GPP_F13): TX=1 also turns
 * the LED on but the PADCFG0[GPIOTXDIS] bit differs, giving 0x202/0x203
 * rather than 0x200/0x201.
 */
#define VAL_ON_NORMAL   0x04000200U
#define VAL_OFF_NORMAL  0x04000201U
#define VAL_ON_INV      0x04000202U
#define VAL_OFF_INV     0x04000203U

/* -------------------------------------------------------------------------
 * Compile-time LED table
 * ------------------------------------------------------------------------- */

enum cf1000_region { RGN_A, RGN_D };

struct led_def {
	const char        *name;
	enum cf1000_region region;
	u32                offset;  /* byte offset within region page */
	u32                on_val;
	u32                off_val;
};

/*
 * Order within each colour pair: blue first, then red.
 * Bay numbering matches physical chassis labels (1-10).
 * HCTL↔bay assignment is the daemon's responsibility, not the driver's.
 */
static const struct led_def LED_TABLE[] = {
	/* ---- Bay 1: GPP_H5 (blue, INV), GPP_H4 (red, INV) ---- */
	{ "cf1000:bay1:blue",    RGN_D, 0x07D0, VAL_ON_INV,    VAL_OFF_INV    },
	{ "cf1000:bay1:red",     RGN_D, 0x07C0, VAL_ON_INV,    VAL_OFF_INV    },
	/* ---- Bay 2: GPP_H7 (blue, INV), GPP_D3 (red, normal) ---- */
	{ "cf1000:bay2:blue",    RGN_D, 0x07F0, VAL_ON_INV,    VAL_OFF_INV    },
	{ "cf1000:bay2:red",     RGN_D, 0x0930, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	/* ---- Bay 3: GPP_D1 (blue), GPP_D0 (red) ---- */
	{ "cf1000:bay3:blue",    RGN_D, 0x0910, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	{ "cf1000:bay3:red",     RGN_D, 0x0900, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	/* ---- Bay 4: GPP_D19 (blue), GPP_S0 (red) ---- */
	{ "cf1000:bay4:blue",    RGN_D, 0x0A30, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	{ "cf1000:bay4:red",     RGN_D, 0x0700, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	/* ---- Bay 5: GPP_S1 (blue), GPP_S2 (red) ---- */
	{ "cf1000:bay5:blue",    RGN_D, 0x0710, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	{ "cf1000:bay5:red",     RGN_D, 0x0720, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	/* ---- Bay 6: GPP_E11 (blue), GPP_E12 (red) ---- */
	{ "cf1000:bay6:blue",    RGN_A, 0x0B20, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	{ "cf1000:bay6:red",     RGN_A, 0x0B30, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	/* ---- Bay 7: GPP_E13 (blue), GPP_E17 (red) ---- */
	{ "cf1000:bay7:blue",    RGN_A, 0x0B40, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	{ "cf1000:bay7:red",     RGN_A, 0x0B80, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	/* ---- Bay 8: GPP_F12 (blue, normal), GPP_F13 (red, INV) ---- */
	{ "cf1000:bay8:blue",    RGN_A, 0x0940, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	{ "cf1000:bay8:red",     RGN_A, 0x0950, VAL_ON_INV,    VAL_OFF_INV    },
	/* ---- Bay 9: GPP_F14 (blue), GPP_F15 (red) ---- */
	{ "cf1000:bay9:blue",    RGN_A, 0x0960, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	{ "cf1000:bay9:red",     RGN_A, 0x0970, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	/* ---- Bay 10: GPP_F16 (blue), GPP_F17 (red) ---- */
	{ "cf1000:bay10:blue",   RGN_A, 0x0980, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	{ "cf1000:bay10:red",    RGN_A, 0x0990, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	/* ---- Network indicator: GP_D22 (blue), GP_D23 (red) ---- */
	{ "cf1000:network:blue", RGN_A, 0x0A80, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	{ "cf1000:network:red",  RGN_A, 0x0A90, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	/* ---- System indicator ---- */
	{ "cf1000:system:blue",  RGN_D, 0x0730, VAL_ON_NORMAL, VAL_OFF_NORMAL },
	{ "cf1000:system:red",   RGN_D, 0x0740, VAL_ON_NORMAL, VAL_OFF_NORMAL },
};

#define NUM_LEDS ARRAY_SIZE(LED_TABLE)

/* -------------------------------------------------------------------------
 * Per-LED runtime state
 * ------------------------------------------------------------------------- */

struct cf1000_led {
	struct led_classdev  cdev;
	void __iomem        *reg;     /* pre-computed: region_base + offset */
	u32                  on_val;
	u32                  off_val;
};

static struct cf1000_led  *cf1000_leds;
static void __iomem       *region_a;
static void __iomem       *region_d;
static unsigned int        leds_registered;

/* -------------------------------------------------------------------------
 * LED class callback
 * ------------------------------------------------------------------------- */

/*
 * brightness_set runs in a non-blocking context.  writel() is safe here;
 * no sleeping is required or permitted.
 */
static void cf1000_brightness_set(struct led_classdev *cdev,
				  enum led_brightness brightness)
{
	struct cf1000_led *led = container_of(cdev, struct cf1000_led, cdev);

	writel(brightness ? led->on_val : led->off_val, led->reg);
}

/* -------------------------------------------------------------------------
 * Module init / exit
 * ------------------------------------------------------------------------- */

static int __init cf1000_leds_init(void)
{
	unsigned int i;
	int ret;

	pr_info("cf1000_leds: loading — %zu LEDs, region A 0x%08lx, region D 0x%08lx\n",
		NUM_LEDS, REGION_A_BASE, REGION_D_BASE);

	/*
	 * We do NOT call request_mem_region(): pinctrl-alderlake already
	 * owns these pages and the call would fail.  We are intentionally
	 * bypassing the pinctrl subsystem and writing PADCFG0 directly.
	 *
	 * ioremap_np() (non-posted writes) is the correct mapping type for
	 * PCH config registers on x86 — it ensures each write completes
	 * before the next instruction and takes a code path that avoids
	 * the iomem resource-tree exclusivity checks tightened in 6.x.
	 * Falls back to plain ioremap on platforms without NP support.
	 */
	region_a = ioremap_np(REGION_A_BASE, REGION_SIZE);
	if (!region_a) {
		pr_warn("cf1000_leds: ioremap_np unavailable for region A, trying ioremap\n");
		region_a = ioremap(REGION_A_BASE, REGION_SIZE);
	}
	if (!region_a) {
		pr_err("cf1000_leds: ioremap failed for region A (0x%08lx)\n",
		       REGION_A_BASE);
		return -ENOMEM;
	}

	region_d = ioremap_np(REGION_D_BASE, REGION_SIZE);
	if (!region_d) {
		pr_warn("cf1000_leds: ioremap_np unavailable for region D, trying ioremap\n");
		region_d = ioremap(REGION_D_BASE, REGION_SIZE);
	}
	if (!region_d) {
		pr_err("cf1000_leds: ioremap failed for region D (0x%08lx)\n",
		       REGION_D_BASE);
		ret = -ENOMEM;
		goto err_iounmap_a;
	}

	cf1000_leds = kcalloc(NUM_LEDS, sizeof(*cf1000_leds), GFP_KERNEL);
	if (!cf1000_leds) {
		ret = -ENOMEM;
		goto err_iounmap_d;
	}

	for (i = 0; i < NUM_LEDS; i++) {
		const struct led_def *def = &LED_TABLE[i];
		struct cf1000_led    *led = &cf1000_leds[i];

		led->on_val  = def->on_val;
		led->off_val = def->off_val;
		led->reg     = (def->region == RGN_A ? region_a : region_d)
			       + def->offset;

		led->cdev.name           = def->name;
		led->cdev.max_brightness = LED_FULL;  /* 255 */
		led->cdev.brightness     = LED_OFF;
		led->cdev.brightness_set = cf1000_brightness_set;
		/* flags: leave at 0; no blink, no hw trigger needed */

		ret = led_classdev_register(NULL, &led->cdev);
		if (ret) {
			pr_err("cf1000_leds: failed to register '%s': %d\n",
			       def->name, ret);
			goto err_unregister;
		}
		leds_registered++;

		/* Drive to a known-off state immediately */
		writel(led->off_val, led->reg);
		pr_debug("cf1000_leds: registered %s (reg +0x%03x on=0x%08x off=0x%08x)\n",
			 def->name, def->offset, def->on_val, def->off_val);
	}

	pr_info("cf1000_leds: %u LED devices registered under /sys/class/leds/\n",
		leds_registered);
	return 0;

err_unregister:
	while (leds_registered > 0) {
		leds_registered--;
		led_classdev_unregister(&cf1000_leds[leds_registered].cdev);
	}
	kfree(cf1000_leds);
	cf1000_leds = NULL;
err_iounmap_d:
	iounmap(region_d);
	region_d = NULL;
err_iounmap_a:
	iounmap(region_a);
	region_a = NULL;
	return ret;
}

static void __exit cf1000_leds_exit(void)
{
	unsigned int i;

	/* Turn all LEDs off before releasing the MMIO mapping */
	for (i = 0; i < leds_registered; i++)
		writel(cf1000_leds[i].off_val, cf1000_leds[i].reg);

	for (i = 0; i < leds_registered; i++)
		led_classdev_unregister(&cf1000_leds[i].cdev);

	kfree(cf1000_leds);
	cf1000_leds = NULL;

	iounmap(region_d);
	iounmap(region_a);

	pr_info("cf1000_leds: unloaded, all LEDs off\n");
}

module_init(cf1000_leds_init);
module_exit(cf1000_leds_exit);
