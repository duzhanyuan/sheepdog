/*
 * Copyright (C) 2015 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "dog.h"

static struct sd_option upgrade_options[] = {
	{'o', "orig-version", true, "version of converting file"},
	{ 0, NULL, false, NULL },
};

enum orig_version {
	ORIG_VERSION_0_7 = 1,
	ORIG_VERSION_0_8,
};

static struct upgrade_cmd_data {
	enum orig_version orig;
} upgrade_cmd_data = { ~0, };

/* FIXME: this is a ugly copy from group.c, unify is todo */
static int get_zones_nr_from(struct rb_root *nroot)
{
	int nr_zones = 0, j;
	uint32_t zones[SD_MAX_COPIES];
	struct sd_node *n;

	rb_for_each_entry(n, nroot, rb) {
		/*
		 * Only count zones that actually store data, pure gateways
		 * don't contribute to the redundancy level.
		 */
		if (!n->nr_vnodes)
			continue;

		for (j = 0; j < nr_zones; j++) {
			if (n->zone == zones[j])
				break;
		}

		if (j == nr_zones) {
			zones[nr_zones] = n->zone;
			if (++nr_zones == ARRAY_SIZE(zones))
				break;
		}
	}

	return nr_zones;
}

static struct vnode_info *alloc_vnode_info_from_epoch_file(const char *epoch_file)
{
	int fd, buf_len, ret, nr_nodes;
	struct stat epoch_stat;
	struct vnode_info *vinfo = NULL;
	struct sd_node *nodes;

	fd = open(epoch_file, O_RDONLY);
	if (fd < 0) {
		sd_err("failed to read epoch file %s: %m", epoch_file);
		return NULL;
	}

	memset(&epoch_stat, 0, sizeof(epoch_stat));
	ret = fstat(fd, &epoch_stat);
	if (ret < 0) {
		sd_err("failed to stat epoch log file: %m");
		goto close_fd;
	}

	buf_len = epoch_stat.st_size - sizeof(time_t);
	if (buf_len < 0) {
		sd_err("invalid epoch log file: %m");
		goto close_fd;
	}

	sd_assert(buf_len % sizeof(struct sd_node) == 0);
	nr_nodes = buf_len / sizeof(struct sd_node);

	nodes = xzalloc(buf_len);

	ret = xread(fd, nodes, buf_len);
	if (ret != buf_len) {
		sd_err("failed to read from epoch file: %m");
		goto free_nodes;
	}

	vinfo = xzalloc(sizeof(*vinfo));

	INIT_RB_ROOT(&vinfo->vroot);
	INIT_RB_ROOT(&vinfo->nroot);

	for (int i = 0; i < nr_nodes; i++) {
		rb_insert(&vinfo->nroot, &nodes[i], rb, node_cmp);
		vinfo->nr_nodes++;
	}

	nodes_to_vnodes(&vinfo->nroot, &vinfo->vroot);
	vinfo->nr_zones = get_zones_nr_from(&vinfo->nroot);

	return vinfo;

free_nodes:
	free(nodes);

close_fd:
	close(fd);

	return vinfo;
}

/*
 * caution: currently upgrade_object_location() doesn't assume disk vnodes mode
 */
static int upgrade_object_location(int argc, char **argv)
{
	const char *epoch_file = argv[optind++], *oid_string = NULL;
	uint64_t oid;
	struct vnode_info *vinfo;

	if (optind < argc)
		oid_string = argv[optind++];
	else {
		sd_info("please specify object id in hex format");
		return EXIT_USAGE;
	}

	oid = strtoull(oid_string, NULL, 16);

	vinfo = alloc_vnode_info_from_epoch_file(epoch_file);
	if (!vinfo) {
		sd_err("failed to construct vnode info from epoch file %s",
		       epoch_file);
		return EXIT_SYSFAIL;
	}


	/* TODO: erasure coded objects */
	sd_info("%s", node_to_str(oid_to_node(oid, &vinfo->vroot, 0)));

	return EXIT_SUCCESS;
}

static int upgrade_config_convert(int argc, char **argv)
{
	const char *orig_file = argv[optind++], *dst_file = NULL;
	struct stat config_stat;
	int fd, ret, new_fd;
	struct sheepdog_config config;

	BUILD_BUG_ON(sizeof(config) != SD_CONFIG_SIZE);

	if (optind < argc)
		dst_file = argv[optind++];
	else {
		sd_info("please specify destination file path");
		return EXIT_USAGE;
	}

	fd = open(orig_file, O_RDONLY);
	if (fd < 0) {
		sd_err("failed to open config file: %m");
		return EXIT_SYSFAIL;
	}

	memset(&config_stat, 0, sizeof(config_stat));
	ret = fstat(fd, &config_stat);
	if (ret < 0) {
		sd_err("failed to stat config file: %m");
		return EXIT_SYSFAIL;
	}

	if (config_stat.st_size != SD_CONFIG_SIZE) {
		sd_err("original config file has invalid size: %lu",
		       config_stat.st_size);
		return EXIT_USAGE;
	}

	ret = xread(fd, &config, sizeof(config));
	if (ret != sizeof(config)) {
		sd_err("failed to read config file: %m");
		return EXIT_SYSFAIL;
	}

	if (!(config.version == 0x0002 || config.version == 0x0004)) {
		/* 0x0002: v0.7.x, 0x0004: v0.8.x */
		sd_err("unknown version config file: %x", config.version);
		return EXIT_USAGE;
	}

	config.block_size_shift = SD_DEFAULT_BLOCK_SIZE_SHIFT;
	config.version = 0x0006;

	new_fd = open(dst_file, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
	if (new_fd < 0) {
		sd_err("failed to create a new config file: %m");
		return EXIT_SYSFAIL;
	}

	ret = xwrite(new_fd, &config, sizeof(config));
	if (ret != sizeof(config)) {
		sd_err("failed to write to a new config file: %m");
		return EXIT_SYSFAIL;
	}

	return EXIT_SUCCESS;
}

static int upgrade_epoch_convert(int argc, char **argv)
{
	const char *orig_file = argv[optind++], *dst_file = NULL;
	struct stat epoch_stat;
	time_t timestamp;
	int fd, new_fd, buf_len, ret, nr_nodes;
	struct sd_node_0_7 *nodes_0_7 = NULL;
	struct sd_node_0_8 *nodes_0_8 = NULL;
	int node_size = -1;
	struct sd_node *new_nodes;

	if (optind < argc)
		dst_file = argv[optind++];
	else {
		sd_info("please specify destination file path");
		return EXIT_USAGE;
	}

	if (upgrade_cmd_data.orig == ORIG_VERSION_0_7)
		node_size = sizeof(struct sd_node_0_7);
	else if (upgrade_cmd_data.orig == ORIG_VERSION_0_8)
		node_size = sizeof(struct sd_node_0_8);
	else {
		sd_info("please specify original version of epoch file");
		return EXIT_USAGE;
	}

	fd = open(orig_file, O_RDONLY);
	if (fd < 0) {
		sd_err("failed to open epoch log file: %m");
		return EXIT_SYSFAIL;
	}

	memset(&epoch_stat, 0, sizeof(epoch_stat));
	ret = fstat(fd, &epoch_stat);
	if (ret < 0) {
		sd_err("failed to stat epoch log file: %m");
		return EXIT_SYSFAIL;
	}

	buf_len = epoch_stat.st_size - sizeof(timestamp);
	if (buf_len < 0) {
		sd_err("invalid epoch log file: %m");
		return EXIT_SYSFAIL;
	}

	if (upgrade_cmd_data.orig == ORIG_VERSION_0_7) {
		nodes_0_7 = xzalloc(buf_len);
		ret = xread(fd, nodes_0_7, buf_len);
	} else {
		sd_assert(upgrade_cmd_data.orig == ORIG_VERSION_0_8);
		nodes_0_8 = xzalloc(buf_len);
		ret = xread(fd, nodes_0_8, buf_len);
	}

	if (ret < 0) {
		sd_err("failed to read epoch log file: %m");
		return EXIT_SYSFAIL;
	}

	if (ret % node_size != 0) {
		sd_err("invalid epoch log file size");
		return EXIT_SYSFAIL;
	}

	nr_nodes = ret / node_size;
	new_nodes = xcalloc(nr_nodes, sizeof(struct sd_node));

	ret = xread(fd, &timestamp, sizeof(timestamp));
	if (ret != sizeof(timestamp)) {
		sd_err("invalid epoch log file, failed to read timestamp: %m");
		return EXIT_SYSFAIL;
	}

	for (int i = 0; i < nr_nodes; i++) {
		if (upgrade_cmd_data.orig == ORIG_VERSION_0_7) {
			memcpy(&new_nodes[i].nid, &nodes_0_7[i].nid,
			       sizeof(struct node_id));
			new_nodes[i].nr_vnodes = nodes_0_7[i].nr_vnodes;
			new_nodes[i].zone = nodes_0_7[i].zone;
			new_nodes[i].space = nodes_0_7[i].space;
		} else {
			sd_assert(upgrade_cmd_data.orig == ORIG_VERSION_0_8);

			memcpy(&new_nodes[i].nid, &nodes_0_8[i].nid,
			       sizeof(struct node_id));
			new_nodes[i].nr_vnodes = nodes_0_8[i].nr_vnodes;
			new_nodes[i].zone = nodes_0_8[i].zone;
			new_nodes[i].space = nodes_0_8[i].space;
		}
	}

	new_fd = open(dst_file, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
	if (new_fd < 0) {
		sd_err("failed to create a new epoch log file: %m");
		return EXIT_SYSFAIL;
	}

	ret = xwrite(new_fd, new_nodes, sizeof(struct sd_node) * nr_nodes);
	if (ret != sizeof(struct sd_node) * nr_nodes) {
		sd_err("failed to write node list to a new epoch log file: %m");
		return EXIT_SYSFAIL;
	}

	ret = xwrite(new_fd, &timestamp, sizeof(timestamp));
	if (ret != sizeof(timestamp)) {
		sd_err("failed to write timestamp to a new epoch log file: %m");
		return EXIT_SYSFAIL;
	}

	sd_info("number of vnodes of each nodes:");
	for (int i = 0; i < nr_nodes; i++)
		sd_info("\t%s == %"PRIu16, node_to_str(&new_nodes[i]),
			new_nodes[i].nr_vnodes);

	sd_info("please supply the above numbers to sheeps with -V option");

	return EXIT_SUCCESS;
}

static int upgrade_inode_convert(int argc, char **argv)
{
	const char *orig_file = argv[optind++], *dst_file = NULL;
	int orig_fd, dst_fd, ret;
	struct sd_inode_0_7 *orig_0_7;
	struct sd_inode_0_8 *orig_0_8;
	struct stat orig_stat;
	struct sd_inode *dst;

	if (optind < argc)
		dst_file = argv[optind++];
	else {
		sd_info("please specify destination file path");
		return EXIT_USAGE;
	}

	orig_fd = open(orig_file, O_RDONLY);
	if (orig_fd < 0) {
		sd_err("failed to open original inode file: %m");
		return EXIT_SYSFAIL;
	}

	memset(&orig_stat, 0, sizeof(orig_stat));
	ret = fstat(orig_fd, &orig_stat);
	if (ret < 0) {
		sd_err("failed to stat original inode file: %m");
		return EXIT_SYSFAIL;
	}

	dst = xzalloc(sizeof(*dst));

	if (upgrade_cmd_data.orig == ORIG_VERSION_0_7) {
		orig_0_7 = xzalloc(sizeof(*orig_0_7));
		ret = xread(orig_fd, orig_0_7, orig_stat.st_size);
		if (ret != orig_stat.st_size) {
			sd_err("failed to read original inode file: %m");
			return EXIT_SYSFAIL;
		}

		if (orig_0_7->snap_ctime) {
			sd_err("snapshot cannot be converted");
			return EXIT_USAGE;
		}

		memcpy(dst->name, orig_0_7->name, SD_MAX_VDI_LEN);
		memcpy(dst->tag, orig_0_7->tag, SD_MAX_VDI_TAG_LEN);
		dst->create_time = orig_0_7->create_time;
		dst->vm_clock_nsec = orig_0_7->vm_clock_nsec;
		dst->vdi_size = orig_0_7->vdi_size;
		dst->vm_state_size = orig_0_7->vm_state_size;
		dst->copy_policy = orig_0_7->copy_policy;
		dst->nr_copies = orig_0_7->nr_copies;
		dst->block_size_shift = orig_0_7->block_size_shift;
		dst->vdi_id = orig_0_7->vdi_id;

		memcpy(dst->data_vdi_id, orig_0_7->data_vdi_id,
		       sizeof(uint32_t) * SD_INODE_DATA_INDEX);
	} else if (upgrade_cmd_data.orig == ORIG_VERSION_0_8) {
		orig_0_8 = xzalloc(sizeof(*orig_0_8));
		ret = xread(orig_fd, orig_0_8, orig_stat.st_size);

		if (ret != orig_stat.st_size) {
			sd_err("failed to read original inode file: %m");
			return EXIT_SYSFAIL;
		}

		if (orig_0_8->snap_ctime) {
			sd_err("snapshot cannot be converted");
			return EXIT_USAGE;
		}

		memcpy(dst->name, orig_0_8->name, SD_MAX_VDI_LEN);
		memcpy(dst->tag, orig_0_8->tag, SD_MAX_VDI_TAG_LEN);
		dst->create_time = orig_0_8->create_time;
		dst->vm_clock_nsec = orig_0_8->vm_clock_nsec;
		dst->vdi_size = orig_0_8->vdi_size;
		dst->vm_state_size = orig_0_8->vm_state_size;
		dst->copy_policy = orig_0_8->copy_policy;
		dst->nr_copies = orig_0_8->nr_copies;
		dst->block_size_shift = orig_0_8->block_size_shift;
		dst->vdi_id = orig_0_8->vdi_id;

		memcpy(dst->data_vdi_id, orig_0_8->data_vdi_id,
		       sizeof(uint32_t) * SD_INODE_DATA_INDEX);
	} else {
		sd_info("please specify original version of inode file");
		return EXIT_FAILURE;
	}

	dst_fd = open(dst_file, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
	if (dst_fd < 0) {
		sd_err("failed to create converted inode file: %m");
		return EXIT_SYSFAIL;
	}

	ret = xwrite(dst_fd, dst, sizeof(*dst));
	if (ret != sizeof(*dst)) {
		sd_err("failed to write converted inode file: %m");
		return EXIT_SYSFAIL;
	}

	return EXIT_SUCCESS;
}

static struct subcommand upgrade_cmd[] = {
	{"inode-convert", "<path of original inode file>"
	 " <path of new inode file>",
	 "hTo", "upgrade inode object file",
	 NULL, CMD_NEED_ARG, upgrade_inode_convert, upgrade_options},
	{"epoch-convert", "<path of original epoch log file>"
	 " <path of new epoch log file>",
	 "hTo", "upgrade epoch log file",
	 NULL, CMD_NEED_ARG, upgrade_epoch_convert, upgrade_options},
	{"config-convert", "<path of original config file>"
	 " <path of new config file>",
	 "hT", "upgrade config file",
	 NULL, CMD_NEED_ARG, upgrade_config_convert, upgrade_options},
	{"object-location", "<path of latest epoch file> <oid>",
	 "hT", "print object location",
	 NULL, CMD_NEED_ARG, upgrade_object_location, upgrade_options},
	{NULL,},
};

static int upgrade_parser(int ch, const char *opt)
{
	switch (ch) {
	case 'o':
		if (!strcmp(opt, "v0.7"))
			upgrade_cmd_data.orig = ORIG_VERSION_0_7;
		else if (!strcmp(opt, "v0.8"))
			upgrade_cmd_data.orig = ORIG_VERSION_0_8;
		else {
			sd_info("unknown original version: %s", opt);
			sd_info("valid versions are v0.7 or v0.8");
			exit(EXIT_FAILURE);
		}

		break;
	}

	return 0;
}

struct command upgrade_command = {
	"upgrade",
	upgrade_cmd,
	upgrade_parser
};

