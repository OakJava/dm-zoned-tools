/*
 * This file is part of dm-zoned tools.
 *
 * Copyright (C) 2016, Western Digital.  All rights reserved.
 *
 * This software is distributed under the terms of the BSD 2-clause license,
 * "as is," without technical support, and WITHOUT ANY WARRANTY, without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. You should have received a copy of the BSD 2-clause license along
 * with dm-zoned tools.
 * If not, see <http://opensource.org/licenses/BSD-2-Clause>.
 *
 * Authors: Damien Le Moal (damien.lemoal@wdc.com)
 */

#include "dmz.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <sys/types.h>
#include <asm/byteorder.h>

/*
 * Fill and write a super block.
 */
int dmz_write_super(struct dmz_dev *dev,
		    __u64 gen, __u64 offset)
{
	__u64 sb_block = dev->sb_block + offset, bdev_sb_block;
	struct dm_zoned_super *sb;
	struct dmz_block_dev *bdev = &dev->bdev[0];
	__u32 crc;
	__u8 *buf;
	int ret;

	buf = malloc(DMZ_BLOCK_SIZE);
	if (!buf) {
		fprintf(stderr, "Not enough memory\n");
		return -1;
	}
	memset(buf, 0, DMZ_BLOCK_SIZE);

	bdev_sb_block = sb_block;
	if (dev->bdev[1].name) {
		if (sb_block >= dev->bdev[1].block_offset) {
			bdev_sb_block -= dev->bdev[1].block_offset;
			bdev = &dev->bdev[1];
		}
	}

	printf("  Writing super block to %s block %llu\n",
	       bdev->name, bdev_sb_block);

	sb = (struct dm_zoned_super *) buf;

	sb->magic = __cpu_to_le32(DMZ_MAGIC);
	sb->version = __cpu_to_le32(dev->sb_version);

	sb->gen = __cpu_to_le64(gen);

	sb->sb_block = __cpu_to_le64(sb_block);
	sb->nr_meta_blocks = __cpu_to_le32(dev->nr_meta_blocks);
	sb->nr_reserved_seq = __cpu_to_le32(dev->nr_reserved_seq);
	sb->nr_chunks = __cpu_to_le32(dev->nr_chunks);

	sb->nr_map_blocks = __cpu_to_le32(dev->nr_map_blocks);
	sb->nr_bitmap_blocks = __cpu_to_le32(dev->nr_bitmap_blocks);

	if (dev->sb_version > 1) {
		memcpy(sb->dmz_uuid, dev->uuid, 16);
		memcpy(sb->dmz_label, dev->label, 32);
		memcpy(sb->dev_uuid, bdev->uuid, 16);
	}
	crc = dmz_crc32(gen, sb, DMZ_BLOCK_SIZE);
	sb->crc = __cpu_to_le32(crc);

	ret = dmz_write_block(dev, sb_block, buf);
	if (ret < 0)
		fprintf(stderr,
			"%s: Write super block at block %llu failed\n",
			bdev->name, bdev_sb_block);

	free(buf);

	return ret;
}

/*
 * Write mapping table blocks.
 */
static int dmz_write_mapping(struct dmz_dev *dev,
			     __u64 offset)
{
	__u64 map_block;
	struct dm_zoned_map *dmap;
	unsigned int i;
	__u8 *buf;
	int ret = -1;

	printf("  Writing mapping table\n");

	/* Setup "all unmapped" mapping block */
	buf = malloc(DMZ_BLOCK_SIZE);
	if (!buf) {
		fprintf(stderr, "Not enough memory\n");
		return -1;
	}
	dmap = (struct dm_zoned_map *)buf;
	for (i = 0; i < DMZ_MAP_ENTRIES; i++) {
		dmap->dzone_id = __cpu_to_le32(DMZ_MAP_UNMAPPED);
		dmap->bzone_id = __cpu_to_le32(DMZ_MAP_UNMAPPED);
		dmap++;
	}

	/* Write mapping table */
	map_block = offset + dev->map_block;
	for (i = 0; i < dev->nr_map_blocks; i++) {
		ret = dmz_write_block(dev, map_block + i, buf);
		if (ret < 0) {
			fprintf(stderr,
				"%s: Write mapping block %llu failed\n",
				dev->label,
				map_block + i);
			break;
		}
	}

	free(buf);

	return ret;
}

/*
 * Write zone bitmap blocks.
 */
static int dmz_write_bitmap(struct dmz_dev *dev,
			    __u64 offset)
{
	__u64 bitmap_block;
	unsigned int i;
	__u8 *buf;
	int ret = -1;

	printf("  Writing bitmap blocks\n");

	buf = malloc(DMZ_BLOCK_SIZE);
	if (!buf) {
		fprintf(stderr, "Not enough memory\n");
		return -1;
	}
	memset(buf, 0, DMZ_BLOCK_SIZE);

	/* Clear bitmap blocks */
	bitmap_block = offset + dev->bitmap_block;
	for (i = 0; i < dev->nr_bitmap_blocks; i++) {
		ret = dmz_write_block(dev, bitmap_block + i, buf);
		if (ret < 0) {
			fprintf(stderr,
				"%s: Write bitmap block %llu failed\n",
				dev->label,
				bitmap_block + i);
			break;
		}
	}

	free(buf);

	return ret;
}

/*
 * Write formatted metadata blocks.
 */
static int dmz_write_meta(struct dmz_dev *dev,
			  __u64 offset)
{

	/* Write mapping table */
	if (dmz_write_mapping(dev, offset) < 0)
		return -1;

	/* Write bitmap blocks */
	if (dmz_write_bitmap(dev, offset) < 0)
		return -1;

	/* Write super block */
	if (dmz_write_super(dev, 1, offset) < 0)
		return -1;

	return 0;
}

/*
 * Format a device.
 */
int dmz_format(struct dmz_dev *dev)
{
	if (dev->sb_version > DMZ_META_VER) {
		dev->sb_version = DMZ_META_VER;
		fprintf(stderr, "Falling back to metadata version %d\n",
			dev->sb_version);
	} else if (!dev->sb_version) {
		dev->sb_version = DMZ_META_VER;
		fprintf(stderr, "Defaulting to metadata version %d\n",
			dev->sb_version);
	}
	if (dev->sb_version > 1 && !strlen(dev->label))
		sprintf(dev->label, "dmz-%s", dev->bdev[0].name);

	/* calculate location of metadata blocks */
	if (dmz_locate_metadata(dev) < 0)
		return -1;

	if (dev->sb_version > 1) {
		if (uuid_is_null(dev->uuid))
			uuid_generate_random(dev->uuid);
		if (uuid_is_null(dev->bdev[0].uuid))
			uuid_generate_random(dev->bdev[0].uuid);
		if (dev->bdev[1].name && uuid_is_null(dev->bdev[1].uuid))
			uuid_generate_random(dev->bdev[1].uuid);
	}
	if (dev->flags & DMZ_VERBOSE) {
		unsigned int nr_seq_data_zones;

		printf("Format metadata %d:\n", dev->sb_version);
		if (dev->sb_version > 1) {
			struct dmz_block_dev *bdev = &dev->bdev[0];
			char dev_uuid[UUID_STR_LEN];
			char bdev_uuid[UUID_STR_LEN];

			uuid_unparse(dev->uuid, dev_uuid);
			uuid_unparse(bdev->uuid, bdev_uuid);
			printf("  DM-Zoned UUID %s\n", dev_uuid);
			printf("  DM-Zoned Label %s\n", dev->label);
			printf("  Device %s UUID %s\n",
			       bdev->name, bdev_uuid);
			if (dev->bdev[1].name) {
				printf("  Device %s block offset %llu\n",
				       bdev->name, bdev->block_offset);
				bdev = &dev->bdev[1];
				uuid_unparse(bdev->uuid, bdev_uuid);
				printf("  Device %s UUID %s\n",
				       bdev->name, bdev_uuid);
				printf("  Device %s block offset %llu\n",
				       bdev->name, bdev->block_offset);
			}
		}
		printf("  %u useable zones\n",
		       dev->nr_useable_zones);
		printf("  Primary meta-data set: %u metadata blocks from block %llu (zone %u)\n",
		       dev->nr_meta_blocks,
		       dev->sb_block,
		       dmz_zone_id(dev, dev->sb_zone));
		printf("    Super block at block %llu and %llu\n",
		       dev->sb_block,
		       dev->sb_block + (dev->nr_meta_zones * dev->zone_nr_blocks));
		printf("    %u chunk mapping table blocks\n",
		       dev->nr_map_blocks);
		printf("    %u bitmap blocks\n",
		       dev->nr_bitmap_blocks);
		printf("    Using %u zones per meta-data set (%u total)\n",
		       dev->nr_meta_zones,
		       dev->total_nr_meta_zones);

		dev->nr_rnd_zones -= dev->total_nr_meta_zones;
		nr_seq_data_zones = dev->nr_useable_zones
			- (dev->total_nr_meta_zones + dev->nr_rnd_zones +
			dev->nr_reserved_seq);
		printf("  %u data chunks capacity\n",
		       dev->nr_chunks);
		printf("    %u random zone%s\n",
		       dev->nr_rnd_zones,
		       dev->nr_rnd_zones > 1 ? "s" : "");
		printf("    %u sequential zone%s\n",
		       nr_seq_data_zones,
		       nr_seq_data_zones > 1 ? "s" : "");
		printf("  %u sequential zone%s reserved for reclaim\n",
		       dev->nr_reserved_seq,
		       dev->nr_reserved_seq > 1 ? "s" : "");

	}

	/* Ready to write: first reset all zones */
	printf("Resetting sequential zones\n");
	if (dmz_reset_zones(dev) < 0)
		return -1;

	/* Write primary metadata set */
	printf("Writing primary metadata set\n");
	if (dmz_write_meta(dev, 0) < 0)
		return -1;

	/* Write secondary metadata set */
	printf("Writing secondary metadata set\n");
	if (dmz_write_meta(dev,
			   dev->zone_nr_blocks * dev->nr_meta_zones) < 0)
		return -1;

	if (dev->sb_version > 1 && dev->bdev[1].name) {
		printf("Writing tertiary metadata\n");
		if (dmz_write_super(dev, 0, dev->bdev[1].block_offset) < 0)
			return -1;
	}

	/* Sync */
	if (dmz_sync_dev(&dev->bdev[0]) < 0)
		return -1;
	if (dev->bdev[1].name) {
		if (dmz_sync_dev(&dev->bdev[1]) < 0)
			return -1;
	}

	printf("Done.\n");

	return 0;
}

