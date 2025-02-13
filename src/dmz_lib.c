// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * This file is part of dm-zoned tools.
 * Copyright (C) 2016, Western Digital.  All rights reserved.
 * Copyright (c) 2020 Western Digital Corporation or its affiliates.
 *
 * Authors: Damien Le Moal (damien.lemoal@wdc.com)
 */
#include "dmz.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>

/*
 * For super block checksum (CRC32)
 */
#define CRCPOLY_LE 0xedb88320

__u32 dmz_crc32(__u32 crc, const void *buf, size_t length)
{
        unsigned char *p = (unsigned char *)buf;
	int i;

	while (length--) {
		crc ^= *p++;
		for (i = 0; i < 8; i++)
			crc = (crc >> 1) ^ ((crc & 1) ? CRCPOLY_LE : 0);
	}

	return crc;
}

/*
 * Check that the device supports reset all zones operation.
 * For now, simply exclude DM devices as that operation is never
 * supported on these devices.
 */
static bool dmz_bdev_has_reset_all(struct dmz_block_dev *bdev)
{
	char path[PATH_MAX];
	struct stat st;
	int len;

	if (bdev->type == DMZ_TYPE_REGULAR)
		return false;

	/* Check if this is a DM device */
	len = snprintf(path, sizeof(path),
		       "/sys/block/%s/dm/name",
		       bdev->name);
	if (len >= PATH_MAX)
		return false;

	return stat(path, &st) != 0;
}

/*
 * Reset all zones of a device.
 */
static int dmz_reset_all_zones(struct dmz_dev *dev,
			       struct blk_zone *zone)
{
	struct dmz_block_dev *bdev;
	struct blk_zone_range range;
	__u64 zone_sector;
	unsigned int i;
	int ret;

	bdev = dmz_sector_to_bdev(dev, dmz_zone_sector(zone), &zone_sector);
	if (zone_sector != 0 || !dmz_bdev_has_reset_all(bdev))
		return 0;

	range.sector = 0;
	range.nr_sectors = bdev->capacity;

	ret = ioctl(bdev->fd, BLKRESETZONE, &range);
	if (ret != 0)
		return ret;

	for (i = 0; i < bdev->nr_zones; i++, zone++) {
		if (dmz_zone_seq_req(zone) || dmz_zone_seq_pref(zone))
			zone->wp = zone->start;
	}

	return bdev->nr_zones;
}

/*
 * Reset a zone.
 */
int dmz_reset_zone(struct dmz_dev *dev, struct blk_zone *zone)
{
	struct dmz_block_dev *bdev;
	struct blk_zone_range range;
	__u64 zone_sector;

	if (dmz_zone_unknown(zone) || dmz_zone_conv(zone))
		return 0;

	bdev = dmz_sector_to_bdev(dev, dmz_zone_sector(zone), &zone_sector);

	/* Non empty sequential zone: reset */
	range.sector = zone_sector;
	range.nr_sectors = dmz_zone_length(zone);
	if (ioctl(bdev->fd, BLKRESETZONE, &range) < 0) {
		fprintf(stderr,
			"%s: Reset zone %u failed %d (%s)\n",
			bdev->name,
			dmz_zone_id(dev, zone),
			errno, strerror(errno));
		return -1;
	}

	zone->wp = zone->start;

	return 0;
}

/*
 * Reset all zones of a device.
 */
int dmz_reset_zones(struct dmz_dev *dev)
{
	struct blk_zone *zone;
	unsigned int i = 0;
	int ret;

	while (i < dev->nr_zones) {
		zone = &dev->zones[i];

		/*
		 * Try reset all zones of the current bdev. If the device does
		 * not support this operation, continue resetting the device
		 * zones one at a time.
		 */
		ret = dmz_reset_all_zones(dev, zone);
		if (ret > 0) {
			i += ret;
			continue;
		}

		if (dmz_reset_zone(dev, zone) < 0)
			return -1;

		i++;
	}

	return 0;
}

/*
 * Determine location and amount of metadata blocks.
 */
int dmz_locate_metadata(struct dmz_dev *dev)
{
	struct blk_zone *zone;
	unsigned int i = 0;
	unsigned int nr_meta_blocks, nr_map_blocks;
	unsigned int nr_chunks, nr_meta_zones;
	unsigned int nr_bitmap_zones;

	if (dev->flags & DMZ_VERBOSE)
		printf("Locating metadata...\n");

	dev->nr_usable_zones = 0;
	dev->max_nr_meta_zones = 0;
	dev->last_meta_zone = 0;
	dev->nr_cache_zones = 0;

	/* Count usable zones */
	for (i = 0; i < dev->nr_zones; i++) {

		zone = &dev->zones[i];

		if (dmz_zone_is_cache(dev, zone)) {
			if (dev->sb_zone == NULL) {
				dev->sb_zone = zone;
				dev->last_meta_zone = i;
				dev->max_nr_meta_zones = 1;
			} else if (dev->last_meta_zone == (i - 1)) {
				dev->last_meta_zone = i;
				dev->max_nr_meta_zones++;
			}
			dev->nr_cache_zones++;
		} else {
			if (dmz_zone_cond(zone) == BLK_ZONE_COND_READONLY) {
				printf("  Ignoring read-only zone %u\n",
				       dmz_zone_id(dev, zone));
				continue;
			}

			if (dmz_zone_cond(zone) == BLK_ZONE_COND_OFFLINE) {
				printf("  Ignoring offline zone %u\n",
				       dmz_zone_id(dev, zone));
				continue;
			}
		}
		dev->nr_usable_zones++;
	}

	/*
	 * Cache zones are mandatory: at least 3
	 * (two for metadata and one for bufferring random writes).
	 */
	if (dev->nr_cache_zones < 3) {
		fprintf(stderr,
			"%s: Not enough random zones found\n",
			dev->label);
		return -1;
	}

	/*
	 * It does not make sense to have more reserved
	 * sequential zones than random zones.
	 */
	if (dev->nr_reserved_seq > dev->nr_cache_zones)
		dev->nr_reserved_seq = dev->nr_cache_zones - 1;
	if (dev->nr_reserved_seq >= dev->nr_usable_zones) {
		fprintf(stderr,
			"%s: Not enough usable zones found\n",
			dev->label);
		return -1;
	}

	dev->sb_block = dmz_sect2blk(dmz_zone_sector(dev->sb_zone));

	/*
	 * To facilitate addressing of the bitmap blocks, create
	 * one bitmap per zone, including meta zones and unusable
	 * read-only and offline zones.
	 */
	dev->zone_nr_bitmap_blocks =
		dev->zone_nr_blocks >> (DMZ_BLOCK_SHIFT + 3);
	if (!dev->zone_nr_bitmap_blocks)
		dev->zone_nr_bitmap_blocks = 1;
	dev->nr_bitmap_blocks = dev->nr_zones * dev->zone_nr_bitmap_blocks;
	nr_bitmap_zones = (dev->nr_bitmap_blocks + dev->zone_nr_blocks - 1)
		/ dev->zone_nr_blocks;

	if ((nr_bitmap_zones + dev->nr_reserved_seq) > dev->nr_usable_zones) {
		fprintf(stderr,
			"%s: Not enough zones\n",
			dev->label);
		return -1;
	}

	/*
	 * Not counting the mapping table, the maximum number of chunks
	 * is the number of usable zones minus the bitmap zones and the
	 * number of reserved zones.
	 */
	nr_chunks = dev->nr_usable_zones -
		(nr_bitmap_zones + dev->nr_reserved_seq);

	/* Assuming the maximum number of chunks, get the mapping table size */
	nr_map_blocks = DIV_ROUND_UP(nr_chunks, DMZ_MAP_ENTRIES);

	/*
	 * And then a first estimate of the number of metadata zones
	 * (we need 2 sets of metadata).
	 */
	nr_meta_blocks = 1 + nr_map_blocks + dev->nr_bitmap_blocks;
	nr_meta_zones = ((nr_meta_blocks + dev->zone_nr_blocks - 1)
			 / dev->zone_nr_blocks);
	dev->total_nr_meta_zones = nr_meta_zones << 1;

	if (dev->total_nr_meta_zones > dev->nr_cache_zones) {
		fprintf(stderr,
			"%s: Insufficient number of cache zones "
			"(need %u, have %u)\n",
			dev->label,
			dev->total_nr_meta_zones,
			dev->nr_cache_zones);
		return -1;
	}

	/*
	 * Now, fix the number of chunks and the mapping table size to
	 * make sure that everything fits on the drive.
	 */
	dev->nr_chunks = dev->nr_usable_zones -
		(dev->total_nr_meta_zones + dev->nr_reserved_seq);
	dev->nr_map_blocks = DIV_ROUND_UP(dev->nr_chunks, DMZ_MAP_ENTRIES);
	dev->map_block = dev->sb_block + 1;
	dev->bitmap_block = dev->map_block + dev->nr_map_blocks;

	dev->nr_meta_blocks = 1 + dev->nr_map_blocks + dev->nr_bitmap_blocks;
	dev->nr_meta_zones = ((dev->nr_meta_blocks + dev->zone_nr_blocks - 1)
			 / dev->zone_nr_blocks);
	dev->total_nr_meta_zones = dev->nr_meta_zones << 1;

	return 0;
}

