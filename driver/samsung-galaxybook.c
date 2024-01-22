// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Samsung Galaxybook Extras driver
 *
 * Copyright (c) 2023 Joshua Grisham <josh@joshuagrisham.com>
 *
 * Implementation inspired by existing x86 platform drivers.
 * Thank you to the authors!
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/leds.h>
#include <linux/platform_device.h>

#define SAMSUNG_GALAXYBOOK_CLASS "samsung-galaxybook"
#define SAMSUNG_GALAXYBOOK_NAME "Samsung Galaxybook Extras"

#define ACPI_METHOD_SETTINGS "CSFI"
#define ACPI_METHOD_PERFORMANCE_MODE "CSXI"

static const struct acpi_device_id galaxybook_device_ids[] = {
	{ "SAM0429", 0 },
	{ "", 0 },
};
MODULE_DEVICE_TABLE(acpi, galaxybook_device_ids);

static const struct dmi_system_id galaxybook_dmi_ids[] = {
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR,
					"SAMSUNG ELECTRONICS CO., LTD."),
			/* TODO: restrict to only specific product names if it is not working for all?  */
			/* DMI_MATCH(DMI_PRODUCT_NAME,	"950XED"), */
			DMI_MATCH(DMI_CHASSIS_TYPE, "10"), /* Notebook */
		},
	},
	{}
};

struct samsung_galaxybook {
	struct acpi_device *device;
	struct platform_device *platform;
	struct led_classdev kbd_backlight;
};

/*
 * Keyboard Backlight
 */

static enum led_brightness galaxybook_kbd_backlight_get(struct led_classdev *kbd_backlight)
{
	return kbd_backlight->brightness;
}

static int galaxybook_kbd_backlight_set(struct led_classdev *led,
		enum led_brightness brightness)
{
	struct samsung_galaxybook *galaxybook = container_of(led,
			struct samsung_galaxybook, kbd_backlight);

	acpi_status status;
	union acpi_object args;
	struct acpi_object_list arg;
	//struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };

	u8 set_payload[21] = { 0 };

	set_payload[0] = 0x43;
	set_payload[1] = 0x58;
	set_payload[2] = 0x78;

	set_payload[5] = 0x82;
	set_payload[6] = brightness;

	args.type 			= ACPI_TYPE_BUFFER;
	args.buffer.length 	= sizeof(set_payload);
	args.buffer.pointer	= set_payload;

	arg.count = 1;
	arg.pointer = &args;

	status = acpi_evaluate_object(galaxybook->device->handle,
			ACPI_METHOD_SETTINGS, &arg, NULL); //&buffer);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to set kbd_backlight brightness with ACPI method %s, got %s\n",
				ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		return -ENXIO;
	}

	// "set" should reply with:
	// {0x43, 0x58, 0x78, 0x00, 0xaa, 0x00, 0x80 * brightness, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
	// kfree(buffer.pointer);

	pr_info("setting kbd_backlight brightness to %d\n", brightness);
	galaxybook->kbd_backlight.brightness = brightness;

	return 0;
}

static int galaxybook_kbd_backlight_init(struct samsung_galaxybook *galaxybook)
{
	acpi_status status;
	union acpi_object args;
	struct acpi_object_list arg;
	//struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };

	u8 init_payload[21] = { 0 };

	init_payload[0] = 0x43;
	init_payload[1] = 0x58;
	init_payload[2] = 0x78;

	init_payload[5] = 0xbb;
	init_payload[6] = 0xaa;

	args.type 			= ACPI_TYPE_BUFFER;
	args.buffer.length 	= sizeof(init_payload);
	args.buffer.pointer	= init_payload;

	arg.count = 1;
	arg.pointer = &args;

	status = acpi_evaluate_object(galaxybook->device->handle,
			ACPI_METHOD_SETTINGS, &arg, NULL); //&buffer);

	if (ACPI_FAILURE(status) && status != AE_NOT_FOUND) {
		pr_err("failed to execute kbd_backlight init with ACPI method %s, got %s\n",
				ACPI_METHOD_SETTINGS,
				acpi_format_exception(status));
		return -ENXIO;
	}

	// "init" should reply with:
	// {0x43, 0x58, 0x78, 0x00, 0xaa, 0xdd, 0xcc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
	// kfree(buffer.pointer);

	pr_info("kbd_backlight initialized via ACPI method %s, registering led class...\n",
			ACPI_METHOD_SETTINGS);

	galaxybook->kbd_backlight = (struct led_classdev) {
		/* .default_trigger = "rfkill0", TODO ?? */
		.brightness_get = galaxybook_kbd_backlight_get,
		.brightness_set_blocking = galaxybook_kbd_backlight_set,
		.name = SAMSUNG_GALAXYBOOK_CLASS "::kbd_backlight",
		.max_brightness = 3,
	};

	return led_classdev_register(&galaxybook->platform->dev, &galaxybook->kbd_backlight);
}

static void galaxybook_kbd_backlight_exit(struct samsung_galaxybook *galaxybook)
{
	led_classdev_unregister(&galaxybook->kbd_backlight);
}

/*
 * Platform
 */

static struct platform_driver galaxybook_platform_driver = {
	.driver = {
		.name = SAMSUNG_GALAXYBOOK_CLASS,
		.acpi_match_table = galaxybook_device_ids,
	},
};

static int galaxybook_platform_init(struct samsung_galaxybook *galaxybook)
{
	int err;

	galaxybook->platform = platform_device_alloc(SAMSUNG_GALAXYBOOK_CLASS, PLATFORM_DEVID_NONE);
	if (!galaxybook->platform)
		return -ENOMEM;

	platform_set_drvdata(galaxybook->platform, galaxybook);

	err = platform_device_add(galaxybook->platform);
	if (err)
		goto err_device_put;

	return 0;

err_device_put:
	platform_device_put(galaxybook->platform);
	return err;
}

static void galaxybook_platform_exit(struct samsung_galaxybook *galaxybook)
{
	platform_device_unregister(galaxybook->platform);
}

/*
 * ACPI
 */

static void galaxybook_acpi_notify(struct acpi_device *device, u32 event)
{
	pr_info("ACPI notification received: %x\n", event);
}

static int galaxybook_acpi_init(struct samsung_galaxybook *galaxybook)
{
	return 0;
}

static void galaxybook_acpi_exit(struct samsung_galaxybook *galaxybook)
{
	return;
}

static int galaxybook_acpi_add(struct acpi_device *device)
{
	struct samsung_galaxybook *galaxybook;
	int err;

	dmi_check_system(galaxybook_dmi_ids);

	galaxybook = kzalloc(sizeof(struct samsung_galaxybook), GFP_KERNEL);
	if (!galaxybook)
		return -ENOMEM;

	strcpy(acpi_device_name(device), "Galaxybook Extras Controller");
	strcpy(acpi_device_class(device), SAMSUNG_GALAXYBOOK_CLASS);
	device->driver_data = galaxybook;
	galaxybook->device = device;

	err = galaxybook_acpi_init(galaxybook); /* TODO needed? */
	if (err)
		goto err_free;

	err = galaxybook_platform_init(galaxybook);
	if (err)
		goto err_acpi_exit;

	err = galaxybook_kbd_backlight_init(galaxybook);
	if (err)
		goto err_platform_exit;

	return 0;

err_platform_exit:
	galaxybook_platform_exit(galaxybook);
err_acpi_exit:
	galaxybook_acpi_exit(galaxybook);
err_free:
	kfree(galaxybook);
	return err;
}

static void galaxybook_acpi_remove(struct acpi_device *device)
{
	struct samsung_galaxybook *galaxybook = acpi_driver_data(device);

	galaxybook_kbd_backlight_exit(galaxybook);
	galaxybook_platform_exit(galaxybook);
	galaxybook_acpi_exit(galaxybook);
	/* TODO: later will do things like in fujitsu-laptop, remove sysfs etc ? */

	kfree(galaxybook);
}

static struct acpi_driver galaxybook_acpi_driver = {
	.name = SAMSUNG_GALAXYBOOK_NAME,
	.class = SAMSUNG_GALAXYBOOK_CLASS,
	.ids = galaxybook_device_ids,
//	.flags	= ACPI_DRIVER_ALL_NOTIFY_EVENTS,
	.ops = {
		.add = galaxybook_acpi_add,
		.remove = galaxybook_acpi_remove,
		.notify = galaxybook_acpi_notify,
		},
	.owner = THIS_MODULE,
};

static int __init samsung_galaxybook_init(void)
{
	int ret;

	ret = platform_driver_register(&galaxybook_platform_driver);
	if (ret < 0)
		goto err_unregister_acpi;

	ret = acpi_bus_register_driver(&galaxybook_acpi_driver);
	if (ret < 0)
		goto err_unregister_platform;

	pr_info("driver successfully loaded\n");

	return 0;

err_unregister_platform:
	platform_driver_unregister(&galaxybook_platform_driver);
err_unregister_acpi:
	platform_driver_unregister(&galaxybook_platform_driver);

	return ret;
}

static void __exit samsung_galaxybook_exit(void)
{
	acpi_bus_unregister_driver(&galaxybook_acpi_driver);
	platform_driver_unregister(&galaxybook_platform_driver);
}

module_init(samsung_galaxybook_init);
module_exit(samsung_galaxybook_exit);

MODULE_AUTHOR("Joshua Grisham");
MODULE_DESCRIPTION("Samsung Galaxybook Extras");
MODULE_LICENSE("GPL");
