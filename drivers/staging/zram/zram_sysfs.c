/*
 * Compressed RAM block device
 *
 * Copyright (C) 2008, 2009, 2010  Nitin Gupta
 *
 * This code is released using a dual license strategy: BSD/GPL
 * You can choose the licence that better fits your requirements.
 *
 * Released under the terms of 3-clause BSD License
 * Released under the terms of GNU General Public License Version 2.0
 *
 * Project home: http://compcache.googlecode.com/
 */

#include <linux/device.h>
#include <linux/genhd.h>

#include "zram_drv.h"

#ifdef CONFIG_SYSFS

/*
 * Individual percpu values can go negative but the sum across all CPUs
 * must always be positive (we store various counts). So, return sum as
 * unsigned value.
 */
static u64 zram_get_stat(struct zram *zram, enum zram_stats_index idx)
{
	int cpu;
	s64 val = 0;

	for_each_possible_cpu(cpu) {
		s64 temp;
		unsigned int start;
		struct zram_stats_cpu *stats;

		stats = per_cpu_ptr(zram->stats, cpu);
		do {
			start = u64_stats_fetch_begin(&stats->syncp);
			temp = stats->count[idx];
		} while (u64_stats_fetch_retry(&stats->syncp, start));
		val += temp;
	}

	WARN_ON(val < 0);
	return val;
}

static struct zram *dev_to_zram(struct device *dev)
{
	int i;
	struct zram *zram = NULL;

	for (i = 0; i < num_devices; i++) {
		zram = &zram_devices[i];
		if (disk_to_dev(zram->disk) == dev)
			break;
	}

	return zram;
}

static ssize_t disksize_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return sprintf(buf, "%llu\n", zram->disksize);
}

static ssize_t disksize_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int ret;
	struct zram *zram = dev_to_zram(dev);

	if (zram->init_done) {
		pr_info("Cannot change disksize for initialized device\n");
		return -EBUSY;
	}

	ret = strict_strtoull(buf, 10, &zram->disksize);
	if (ret)
		return ret;

	zram->disksize &= PAGE_MASK;
	set_capacity(zram->disk, zram->disksize >> SECTOR_SHIFT);

	return len;
}

static ssize_t initstate_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return sprintf(buf, "%u\n", zram->init_done);
}

static ssize_t reset_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int ret;
	unsigned long do_reset;
	struct zram *zram;
	struct block_device *bdev;

	zram = dev_to_zram(dev);
	bdev = bdget_disk(zram->disk, 0);

	/* Do not reset an active device! */
	if (bdev->bd_holders)
		return -EBUSY;

	ret = strict_strtoul(buf, 10, &do_reset);
	if (ret)
		return ret;

	if (!do_reset)
		return -EINVAL;

	/* Make sure all pending I/O is finished */
	if (bdev)
		fsync_bdev(bdev);

	if (zram->init_done)
		zram_reset_device(zram);

	return len;
}

static ssize_t num_reads_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return sprintf(buf, "%llu\n",
		zram_get_stat(zram, ZRAM_STAT_NUM_READS));
}

static ssize_t num_writes_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return sprintf(buf, "%llu\n",
		zram_get_stat(zram, ZRAM_STAT_NUM_WRITES));
}

static ssize_t invalid_io_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return sprintf(buf, "%llu\n",
		zram_get_stat(zram, ZRAM_STAT_INVALID_IO));
}

static ssize_t notify_free_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return sprintf(buf, "%llu\n",
		zram_get_stat(zram, ZRAM_STAT_NOTIFY_FREE));
}

static ssize_t discard_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return sprintf(buf, "%llu\n",
		zram_get_stat(zram, ZRAM_STAT_DISCARD));
}

static ssize_t zero_pages_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return sprintf(buf, "%llu\n",
		zram_get_stat(zram, ZRAM_STAT_PAGES_ZERO));
}

static ssize_t orig_data_size_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return sprintf(buf, "%llu\n",
		zram_get_stat(zram, ZRAM_STAT_PAGES_STORED) << PAGE_SHIFT);
}

static ssize_t compr_data_size_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return sprintf(buf, "%llu\n",
		zram_get_stat(zram, ZRAM_STAT_COMPR_SIZE));
}

static ssize_t mem_used_total_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u64 val = 0;
	struct zram *zram = dev_to_zram(dev);

	if (zram->init_done) {
		val = xv_get_total_size_bytes(zram->mem_pool) +
			(zram_get_stat(zram, ZRAM_STAT_PAGES_EXPAND)
				<< PAGE_SHIFT);
	}

	return sprintf(buf, "%llu\n", val);
}

static DEVICE_ATTR(disksize, S_IRUGO | S_IWUGO,
		disksize_show, disksize_store);
static DEVICE_ATTR(initstate, S_IRUGO, initstate_show, NULL);
static DEVICE_ATTR(reset, S_IWUGO, NULL, reset_store);
static DEVICE_ATTR(num_reads, S_IRUGO, num_reads_show, NULL);
static DEVICE_ATTR(num_writes, S_IRUGO, num_writes_show, NULL);
static DEVICE_ATTR(invalid_io, S_IRUGO, invalid_io_show, NULL);
static DEVICE_ATTR(notify_free, S_IRUGO, notify_free_show, NULL);
static DEVICE_ATTR(discard, S_IRUGO, discard_show, NULL);
static DEVICE_ATTR(zero_pages, S_IRUGO, zero_pages_show, NULL);
static DEVICE_ATTR(orig_data_size, S_IRUGO, orig_data_size_show, NULL);
static DEVICE_ATTR(compr_data_size, S_IRUGO, compr_data_size_show, NULL);
static DEVICE_ATTR(mem_used_total, S_IRUGO, mem_used_total_show, NULL);

static struct attribute *zram_disk_attrs[] = {
	&dev_attr_disksize.attr,
	&dev_attr_initstate.attr,
	&dev_attr_reset.attr,
	&dev_attr_num_reads.attr,
	&dev_attr_num_writes.attr,
	&dev_attr_invalid_io.attr,
	&dev_attr_notify_free.attr,
	&dev_attr_discard.attr,
	&dev_attr_zero_pages.attr,
	&dev_attr_orig_data_size.attr,
	&dev_attr_compr_data_size.attr,
	&dev_attr_mem_used_total.attr,
	NULL,
};

struct attribute_group zram_disk_attr_group = {
	.attrs = zram_disk_attrs,
};

#endif	/* CONFIG_SYSFS */
