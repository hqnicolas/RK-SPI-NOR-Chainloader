/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Board-isolated boot policy for the YY3568 chainloader U-Boot snapshot.
 */

#ifndef __EVB_RK3568_H
#define __EVB_RK3568_H

#define ROCKCHIP_DEVICE_SETTINGS \
			"stdout=serial,vidconsole\0" \
			"stderr=serial,vidconsole\0"

#define BOOT_TARGETS "mmc1 nvme mmc0 scsi usb pxe dhcp"

#include <configs/rk3568_common.h>

#endif
