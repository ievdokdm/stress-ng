/*
 * Copyright (C) 2013-2021 Canonical, Ltd.
 * Copyright (C) 2022-2024 Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"
#include "core-attribute.h"
#include "core-builtin.h"
#include "core-capabilities.h"
#include "core-madvise.h"
#include "core-mmap.h"

#if defined(HAVE_LINUX_MEMPOLICY_H)
#include <linux/mempolicy.h>
#endif

#define MIN_NUMA_MMAP_BYTES	(1 * MB)
#define MAX_NUMA_MMAP_BYTES	(MAX_MEM_LIMIT)
#define DEFAULT_NUMA_MMAP_BYTES	(4 * MB)

static const stress_help_t help[] = {
	{ NULL,	"numa N",		"start N workers stressing NUMA interfaces" },
	{ NULL,	"numa-bytes N",		"size of memory region to be exercised" },
	{ NULL,	"numa-ops N",		"stop after N NUMA bogo operations" },
	{ NULL,	"numa-shuffle-addr",	"shuffle page addresses to move to numa nodes" },
	{ NULL,	"numa-shuffle-node",	"shuffle numa nodes on numa pages moves" },
	{ NULL,	NULL,			NULL }
};

static int stress_set_numa_bytes(const char *opt)
{
	size_t numa_bytes;

	numa_bytes = (size_t)stress_get_uint64_byte_memory(opt, 1);
	stress_check_range_bytes("numa-bytes", numa_bytes,
		MIN_NUMA_MMAP_BYTES, MAX_NUMA_MMAP_BYTES);
	return stress_set_setting("numa-bytes", TYPE_ID_SIZE_T, &numa_bytes);
}

static int stress_set_numa_shuffle_addr(const char *opt)
{
	return stress_set_setting_true("numa-shuffle-addr", opt);
}

static int stress_set_numa_shuffle_node(const char *opt)
{
	return stress_set_setting_true("numa-shuffle-node", opt);
}

static const stress_opt_set_func_t opt_set_funcs[] = {
	{ OPT_numa_bytes,		stress_set_numa_bytes },
	{ OPT_numa_shuffle_addr,	stress_set_numa_shuffle_addr },
	{ OPT_numa_shuffle_node,	stress_set_numa_shuffle_node },
};

#if defined(__NR_get_mempolicy) &&	\
    defined(__NR_mbind) &&		\
    defined(__NR_migrate_pages) &&	\
    defined(__NR_move_pages) &&		\
    defined(__NR_set_mempolicy)

#define BITS_PER_BYTE		(8)
#define NUMA_LONG_BITS		(sizeof(unsigned long) * BITS_PER_BYTE)

#if !defined(MPOL_DEFAULT)
#define MPOL_DEFAULT		(0)
#endif
#if !defined(MPOL_PREFERRED)
#define MPOL_PREFERRED		(1)
#endif
#if !defined(MPOL_BIND)
#define MPOL_BIND		(2)
#endif
#if !defined(MPOL_INTERLEAVE)
#define MPOL_INTERLEAVE		(3)
#endif
#if !defined(MPOL_LOCAL)
#define MPOL_LOCAL		(4)
#endif
#if !defined(MPOL_PREFERRED_MANY)
#define MPOL_PREFERRED_MANY	(5)
#endif
#if !defined(MPOL_WEIGHTED_INTERLEAVE)
#define MPOL_WEIGHTED_INTERLEAVE (6)
#endif

#if !defined(MPOL_F_NODE)
#define MPOL_F_NODE		(1 << 0)
#endif
#if !defined(MPOL_F_ADDR)
#define MPOL_F_ADDR		(1 << 1)
#endif
#if !defined(MPOL_F_MEMS_ALLOWED)
#define MPOL_F_MEMS_ALLOWED	(1 << 2)
#endif

#if !defined(MPOL_MF_STRICT)
#define MPOL_MF_STRICT		(1 << 0)
#endif
#if !defined(MPOL_MF_MOVE)
#define MPOL_MF_MOVE		(1 << 1)
#endif
#if !defined(MPOL_MF_MOVE_ALL)
#define MPOL_MF_MOVE_ALL	(1 << 2)
#endif

#if !defined(MPOL_F_NUMA_BALANCING)
#define MPOL_F_NUMA_BALANCING	(1 << 13)
#endif
#if !defined(MPOL_F_RELATIVE_NODES)
#define MPOL_F_RELATIVE_NODES	(1 << 14)
#endif
#if !defined(MPOL_F_STATIC_NODES)
#define MPOL_F_STATIC_NODES	(1 << 15)
#endif

typedef struct stress_node {
	struct stress_node	*next;
	unsigned long		node_id;
} stress_node_t;

#define STRESS_NUMA_STAT_NUMA_HIT	(0)
#define STRESS_NUMA_STAT_NUMA_MISS	(1)
#define STRESS_NUMA_STAT_MAX		(2)

typedef struct {
	uint64_t value[STRESS_NUMA_STAT_MAX];
} stress_numa_stats_t;

static void stress_numa_stats_read(stress_numa_stats_t *stats)
{
	DIR *dir;
	struct dirent *d;
	static const char *path = "/sys/devices/system/node";
	static const struct {
		const char *name;
		const size_t len;
		size_t index;
	} numa_fields[] = {
		{ "numa_hit",	8,	STRESS_NUMA_STAT_NUMA_HIT },
		{ "numa_miss",	9,	STRESS_NUMA_STAT_NUMA_MISS },
	};

	(void)memset(stats, 0, sizeof(*stats));

	dir = opendir(path);
	if (!dir)
		return;

	while ((d = readdir(dir)) != NULL) {
		char filename[PATH_MAX];
		char buffer[256];
		FILE *fp;

		if (shim_dirent_type(path, d) != SHIM_DT_DIR)
			continue;
		if (strncmp(d->d_name, "node", 4))
			continue;

		(void)snprintf(filename, sizeof(filename), "%s/%s/numastat", path, d->d_name);
		fp = fopen(filename, "r");
		if (!fp)
			continue;

		while (fgets(buffer, sizeof(buffer), fp) != NULL) {
			size_t i;

			for (i = 0; i < SIZEOF_ARRAY(numa_fields); i++) {
				if (strncmp(buffer, numa_fields[i].name, numa_fields[i].len) == 0) {
					uint64_t val = 0;

					if (sscanf(buffer + numa_fields[i].len + 1, "%" SCNu64, &val) == 1) {
						const size_t index = numa_fields[i].index;

						stats->value[index] += val;
					}
				}
			}
		}
		(void)fclose(fp);
	}
	(void)closedir(dir);
}

/*
 *  stress_numa_free_nodes()
 *	free circular list of node info
 */
static void stress_numa_free_nodes(stress_node_t *nodes)
{
	stress_node_t *n = nodes;

	while (n) {
		stress_node_t *next = n->next;

		free(n);
		n = next;

		if (n == nodes)
			break;
	}
}

/*
 *  hex_to_int()
 *	convert ASCII hex digit to integer
 */
static inline int PURE hex_to_int(const char ch)
{
	if ((ch >= '0') && (ch <= '9'))
		return ch - '0';
	if ((ch >= 'a') && (ch <= 'f'))
		return ch - 'a' + 10;
	if ((ch >= 'A') && (ch <= 'F'))
		return ch - 'F' + 10;
	return -1;
}

/*
 *  stress_numa_get_mem_nodes(void)
 *	collect number of NUMA memory nodes, add them to a
 *	circular linked list - also, return maximum number
 *	of nodes
 */
static long stress_numa_get_mem_nodes(
	stress_node_t **node_ptr,
	unsigned long *max_nodes)
{
	FILE *fp;
	long n = 0;
	unsigned long node_id = 0;
	stress_node_t *tail = NULL;
	char buffer[8192], *str = NULL, *ptr;

	*node_ptr = NULL;
	fp = fopen("/proc/self/status", "r");
	if (!fp)
		return -1;

	while (fgets(buffer, sizeof(buffer), fp)) {
		if (!strncmp(buffer, "Mems_allowed:", 13)) {
			str = buffer + 13;
			break;
		}
	}
	(void)fclose(fp);

	if (!str)
		return -1;

	ptr = buffer + strlen(buffer) - 2;

	/*
	 *  Parse hex digits into NUMA node ids, these
	 *  are listed with least significant node last
	 *  so we need to scan backwards from the end of
	 *  the string back to the start.
	 */
	while ((*ptr != ' ') && (ptr > str)) {
		int val, i;

		/* Skip commas */
		if (*ptr == ',') {
			ptr--;
			continue;
		}

		val = hex_to_int(*ptr);
		if (val < 0)
			return -1;

		/* Each hex digit represent 4 memory nodes */
		for (i = 0; i < 4; i++) {
			if (val & (1 << i)) {
				stress_node_t *node = calloc(1, sizeof(*node));
				if (!node)
					return -1;
				node->node_id = node_id;
				node->next = *node_ptr;
				*node_ptr = node;
				if (!tail)
					tail = node;
				tail->next = node;
				n++;
			}
			node_id++;
		}
		ptr--;
	}

	*max_nodes = node_id;
	return n;
}

static inline void stress_set_numa_array(void *array, uint8_t val, size_t nmemb, size_t size)
{
	const size_t n = nmemb * size;

	(void)shim_memset(array, val, n);
}

/*
 *  stress_numa()
 *	stress the Linux NUMA interfaces
 */
static int stress_numa(stress_args_t *args)
{
	long numa_nodes;
	unsigned long max_nodes;
	const size_t page_size = args->page_size;
	size_t num_pages, numa_bytes = 0;
	uint8_t *buf;
	stress_node_t *n;
	int rc = EXIT_FAILURE;
	const bool cap_sys_nice = stress_check_capability(SHIM_CAP_SYS_NICE);
	int *status, *dest_nodes;
	void **pages;
	size_t mask_elements, k;
	unsigned long *node_mask, *old_node_mask;
	bool numa_shuffle_addr, numa_shuffle_node;
	stress_numa_stats_t stats_begin, stats_end;
	double t, duration, rate;

	(void)stress_get_setting("numa-bytes", &numa_bytes);
	(void)stress_get_setting("numa-shuffle-addr", &numa_shuffle_addr);
	(void)stress_get_setting("numa-shuffle-node", &numa_shuffle_node);

	if (numa_bytes == 0) {
		numa_bytes = DEFAULT_NUMA_MMAP_BYTES;
	} else {
		if (args->num_instances > 0) {
			numa_bytes /= args->num_instances;
			numa_bytes &= ~(page_size - 1);
		}
		if (numa_bytes < MIN_NUMA_MMAP_BYTES)
			numa_bytes = MIN_NUMA_MMAP_BYTES;
	}

	num_pages = numa_bytes / page_size;
	numa_nodes = stress_numa_get_mem_nodes(&n, &max_nodes);
	if (numa_nodes < 1) {
		pr_inf_skip("%s: no NUMA nodes found, "
			"skipping test\n", args->name);
		rc = EXIT_NO_RESOURCE;
		goto numa_free;
	}

	if (!args->instance) {
		char str[32];

		stress_uint64_to_str(str, sizeof(str), (uint64_t)numa_bytes);
		pr_inf("%s: system has %lu of a maximum %lu memory NUMA nodes. Using %sB mappings for each instance.\n",
			args->name, numa_nodes, max_nodes, str);
	}

	mask_elements = (max_nodes + NUMA_LONG_BITS - 1) / NUMA_LONG_BITS;
	node_mask = (unsigned long *)calloc(mask_elements, sizeof(*node_mask));
	if (!node_mask) {
		pr_inf_skip("%s: cannot allocate node mask array of %zu elements, skipping stressor\n",
			args->name, mask_elements);
		rc = EXIT_NO_RESOURCE;
		goto numa_free;
	}
	old_node_mask = (unsigned long *)calloc(mask_elements, sizeof(*old_node_mask));
	if (!old_node_mask) {
		pr_inf_skip("%s: cannot allocate old mask array of %zu elements, skipping stressor\n",
			args->name, mask_elements);
		rc = EXIT_NO_RESOURCE;
		goto node_mask_free;
	}

	status = (int *)calloc(num_pages, sizeof(*status));
	if (!status) {
		pr_inf_skip("%s: cannot allocate status array of %zu elements, skipping stressor\n",
			args->name, num_pages);
		rc = EXIT_NO_RESOURCE;
		goto old_node_mask_free;
	}
	dest_nodes = (int *)calloc(num_pages, sizeof(*dest_nodes));
	if (!dest_nodes) {
		pr_inf("%s: cannot allocate dest_nodes array of %zu elements, skipping stressor\n",
			args->name, num_pages);
		rc = EXIT_NO_RESOURCE;
		goto status_free;
	}
	pages = (void **)calloc(num_pages, sizeof(*pages));
	if (!pages) {
		pr_inf_skip("%s: cannot allocate pages array of %zu elements, skipping stressor\n",
			args->name, num_pages);
		rc = EXIT_NO_RESOURCE;
		goto dest_nodes_free;
	}

	/*
	 *  We need a buffer to migrate around NUMA nodes
	 */
	buf = stress_mmap_populate(NULL, numa_bytes,
		PROT_READ | PROT_WRITE,
		MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
	if (buf == MAP_FAILED) {
		rc = stress_exit_status(errno);
		pr_fail("%s: mmap'd region of %zu bytes failed\n",
			args->name, numa_bytes);
		goto pages_free;
	}
	(void)stress_madvise_mergeable(buf, numa_bytes);

	stress_numa_stats_read(&stats_begin);
	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	k = 0;
	t = stress_time_now();
	do {
		int j, mode, ret;
		long lret;
		unsigned long i;
		uint8_t *ptr;
		stress_node_t *n_tmp;
		unsigned cpu, curr_node;
		struct shim_getcpu_cache cache;

		stress_set_numa_array(node_mask, 0x00, mask_elements, sizeof(*node_mask));

		/*
		 *  Fetch memory policy
		 */
		ret = shim_get_mempolicy(&mode, node_mask, max_nodes,
					 buf, MPOL_F_ADDR);
		if (UNLIKELY(ret < 0)) {
			if (errno != ENOSYS) {
				pr_fail("%s: get_mempolicy failed, errno=%d (%s)\n",
					args->name, errno, strerror(errno));
				goto err;
			}
		}

		/* Exercise invalid max_nodes */
		VOID_RET(int, shim_get_mempolicy(&mode, node_mask, 0, buf, MPOL_F_NODE));

		/* Exercise invalid flag */
		VOID_RET(int, shim_get_mempolicy(&mode, node_mask, max_nodes, buf, ~0UL));

		/* Exercise invalid NULL addr condition */
		VOID_RET(int, shim_get_mempolicy(&mode, node_mask, max_nodes, NULL, MPOL_F_ADDR));

		VOID_RET(int, shim_get_mempolicy(&mode, node_mask, max_nodes, buf, MPOL_F_NODE));

		/* Exercise MPOL_F_MEMS_ALLOWED flag syscalls */
		VOID_RET(int, shim_get_mempolicy(&mode, node_mask, max_nodes, buf, MPOL_F_MEMS_ALLOWED));

		VOID_RET(int, shim_get_mempolicy(&mode, node_mask, max_nodes, buf, MPOL_F_MEMS_ALLOWED | MPOL_F_NODE));

		if (!stress_continue_flag())
			break;

		ret = shim_set_mempolicy(MPOL_PREFERRED, NULL, max_nodes);
		if (UNLIKELY(ret < 0)) {
			if (errno != ENOSYS) {
				pr_fail("%s: set_mempolicy failed, errno=%d (%s)\n",
					args->name, errno, strerror(errno));
				goto err;
			}
		}

		(void)stress_mmap_set_light(buf, numa_bytes, page_size);
		if (!stress_continue_flag())
			break;

		/* Create a mix of _NONES options, invalid ones too */
		mode = 0;
#if defined(MPOL_F_STATIC_NODES)
		if (stress_mwc1())
			mode |= MPOL_F_STATIC_NODES;
#endif
#if defined(MPOL_F_RELATIVE_NODES)
		if (stress_mwc1())
			mode |= MPOL_F_RELATIVE_NODES;
#endif

		switch (stress_mwc8modn(12)) {
		case 0:
#if defined(MPOL_DEFAULT)
			VOID_RET(long, shim_set_mempolicy(MPOL_DEFAULT | mode, NULL, max_nodes));
			break;
#endif
		case 1:
#if defined(MPOL_BIND)
			VOID_RET(long, shim_set_mempolicy(MPOL_BIND | mode, node_mask, max_nodes));
			break;
#endif
		case 2:
#if defined(MPOL_INTERLEAVE)
			VOID_RET(long, shim_set_mempolicy(MPOL_INTERLEAVE | mode, node_mask, max_nodes));
			break;
#endif
		case 3:
#if defined(MPOL_PREFERRED)
			VOID_RET(long, shim_set_mempolicy(MPOL_PREFERRED | mode, node_mask, max_nodes));
			break;
#endif
		case 4:
#if defined(MPOL_LOCAL)
			VOID_RET(long, shim_set_mempolicy(MPOL_LOCAL | mode, node_mask, max_nodes));
			break;
#endif
		case 5:
#if defined(MPOL_PREFERRED_MANY)
			VOID_RET(long, shim_set_mempolicy(MPOL_PREFERRED_MANY | mode, node_mask, max_nodes));
			break;
#endif
		case 6:
#if defined(MPOL_WEIGHTED_INTERLEAVE)
			VOID_RET(long, shim_set_mempolicy(MPOL_WEIGHTED_INTERLEAVE | mode, node_mask, max_nodes));
			break;
#endif
		case 7:
			VOID_RET(long, shim_set_mempolicy(0, node_mask, max_nodes));
			break;
		case 8:
			VOID_RET(long, shim_set_mempolicy(mode, node_mask, max_nodes));
			break;
		case 9:
			/* Invalid mode */
			VOID_RET(long, shim_set_mempolicy(mode | MPOL_F_STATIC_NODES | MPOL_F_RELATIVE_NODES, node_mask, max_nodes));
			break;
#if defined(MPOL_F_NUMA_BALANCING) &&	\
    defined(MPOL_LOCAL)
		case 10:
			/* Invalid  MPOL_F_NUMA_BALANCING | MPOL_LOCAL */
			VOID_RET(long, shim_set_mempolicy(MPOL_F_NUMA_BALANCING | MPOL_LOCAL, node_mask, max_nodes));
			break;
#endif
		default:
			/* Intentionally invalid mode */
			VOID_RET(long, shim_set_mempolicy(~0, node_mask, max_nodes));
		}

		/*
		 *  Fetch CPU and node, we just waste some cycled
		 *  doing this for stress reasons only
		 */
		(void)shim_getcpu(&cpu, &curr_node, NULL);

		/* Initialised cache to be safe */
		stress_set_numa_array(&cache, 0x00, 1, sizeof(cache));

		/*
		 * tcache argument is unused in getcpu currently.
		 * Exercise getcpu syscall with non-null tcache
		 * pointer to ensure kernel doesn't break even
		 * when this argument is used in future.
		 */
		(void)shim_getcpu(&cpu, &curr_node, &cache);

		/*
		 *  mbind the buffer, first try MPOL_STRICT which
		 *  may fail with EIO
		 */
		stress_set_numa_array(node_mask, 0x00, mask_elements, sizeof(*node_mask));
		STRESS_SETBIT(node_mask, n->node_id);
		lret = shim_mbind((void *)buf, numa_bytes, MPOL_BIND, node_mask,
			max_nodes, MPOL_MF_STRICT);
		if (UNLIKELY(lret < 0)) {
			if ((errno != EIO) && (errno != ENOSYS)) {
				pr_fail("%s: mbind failed, errno=%d (%s)\n",
					args->name, errno, strerror(errno));
				goto err;
			}
		} else {
			(void)shim_set_mempolicy_home_node((unsigned long)buf,
				(unsigned long)numa_bytes, n->node_id, 0);
			(void)stress_mmap_set_light(buf, numa_bytes, page_size);
		}
		if (!stress_continue_flag())
			break;

		/*
		 *  Exercise set_mempolicy_home_node
		 */
		(void)shim_set_mempolicy_home_node((unsigned long)buf,
				(unsigned long)numa_bytes, max_nodes - 1, 0);
		(void)shim_set_mempolicy_home_node((unsigned long)buf,
				(unsigned long)numa_bytes, 1, 0);
		(void)shim_set_mempolicy_home_node((unsigned long)buf,
				(unsigned long)0, n->node_id, 0);
		(void)shim_set_mempolicy_home_node((unsigned long)buf,
				(unsigned long)numa_bytes, n->node_id, 0);

		/*
		 *  mbind the buffer, now try MPOL_DEFAULT
		 */
		stress_set_numa_array(node_mask, 0x00, mask_elements, sizeof(*node_mask));
		STRESS_SETBIT(node_mask, n->node_id);
		lret = shim_mbind((void *)buf, numa_bytes, MPOL_BIND, node_mask,
			max_nodes, MPOL_DEFAULT);
		if (UNLIKELY(lret < 0)) {
			if ((errno != EIO) && (errno != ENOSYS)) {
				pr_fail("%s: mbind failed, errno=%d (%s)\n",
					args->name, errno, strerror(errno));
				goto err;
			}
		} else {
			(void)shim_set_mempolicy_home_node((unsigned long)buf,
				(unsigned long)numa_bytes, n->node_id, 0);
			(void)stress_mmap_set_light(buf, numa_bytes, page_size);
		}
		if (!stress_continue_flag())
			break;

		/* Exercise invalid start address */
		VOID_RET(long, shim_mbind((void *)(buf + 7), numa_bytes, MPOL_BIND, node_mask,
			max_nodes, MPOL_MF_STRICT));

		/* Exercise wrap around */
		VOID_RET(long, shim_mbind((void *)(~(uintptr_t)0 & ~(page_size - 1)), page_size * 2,
			MPOL_BIND, node_mask, max_nodes, MPOL_MF_STRICT));

		/* Exercise invalid length */
		VOID_RET(long, shim_mbind((void *)buf, ~0UL, MPOL_BIND, node_mask,
			max_nodes, MPOL_MF_STRICT));

		/* Exercise zero length, allowed, but is a no-op */
		VOID_RET(long, shim_mbind((void *)buf, 0, MPOL_BIND, node_mask,
			max_nodes, MPOL_MF_STRICT));

		/* Exercise invalid max_nodes */
		VOID_RET(long, shim_mbind((void *)buf, numa_bytes, MPOL_BIND, node_mask,
			0, MPOL_MF_STRICT));
		VOID_RET(long, shim_mbind((void *)buf, numa_bytes, MPOL_BIND, node_mask,
			0xffffffff, MPOL_MF_STRICT));

		/* Exercise invalid flags */
		VOID_RET(long, shim_mbind((void *)buf, numa_bytes, MPOL_BIND, node_mask,
			max_nodes, ~0U));

		/* Check mbind syscall cannot succeed without capability */
		if (!cap_sys_nice) {
			lret = shim_mbind((void *)buf, numa_bytes, MPOL_BIND, node_mask,
				max_nodes, MPOL_MF_MOVE_ALL);
			if (lret >= 0) {
				pr_fail("%s: mbind without capability CAP_SYS_NICE unexpectedly succeeded, "
						"errno=%d (%s)\n", args->name, errno, strerror(errno));
			}
		}

		/* Move to next node */
		n = n->next;

		/*
		 *  Migrate all this processes pages to the current new node
		 */
		stress_set_numa_array(old_node_mask, 0xff, mask_elements, sizeof(*node_mask));
		stress_set_numa_array(node_mask, 0x00, mask_elements, sizeof(*node_mask));
		STRESS_SETBIT(node_mask, n->node_id);

		/*
	 	 *  Ignore any failures, this is not strictly important
		 */
		VOID_RET(long, shim_migrate_pages(args->pid, max_nodes,
			old_node_mask, node_mask));

		/*
		 *  Exercise illegal pid
		 */
		VOID_RET(long, shim_migrate_pages(~0, max_nodes, old_node_mask, node_mask));

		/*
		 *  Exercise illegal max_nodes
		 */
		VOID_RET(long, shim_migrate_pages(args->pid, ~0UL, old_node_mask, node_mask));
		VOID_RET(long, shim_migrate_pages(args->pid, 0, old_node_mask, node_mask));

		if (!stress_continue_flag())
			break;

		n_tmp = n;
		for (j = 0; j < 16; j++) {
			/*
			 *  Now move pages to lots of different numa nodes
			 */
			for (ptr = buf, i = 0; i < num_pages; i++, ptr += page_size, n_tmp = n_tmp->next) {
				pages[k] = ptr;
				dest_nodes[k] = (int)n_tmp->node_id;
				k++;
				if (k >= num_pages)
					k = 0;
			}
			if (numa_shuffle_addr) {
				for (i = 0; i < num_pages; i++) {
					register const size_t l = stress_mwc32modn(num_pages);
					register void *tmp;

					tmp = pages[i];
					pages[i] = pages[l];
					pages[l] = tmp;
				}
			}
			if (numa_shuffle_node) {
				for (i = 0; i < num_pages; i++) {
					register const size_t l = stress_mwc32modn(num_pages);
					register int tmp;

					tmp = dest_nodes[i];
					dest_nodes[i] = dest_nodes[l];
					dest_nodes[l] = tmp;
				}
			}

			/*
			 *  ..and bump k to ensure next round the pages get reassigned to
			 *  a different node
			 */
			k++;
			if (k >= num_pages)
				k = 0;

			stress_set_numa_array(status, 0x00, num_pages, sizeof(*status));
			lret = shim_move_pages(args->pid, num_pages, pages,
				dest_nodes, status, MPOL_MF_MOVE);
			if (UNLIKELY(lret < 0)) {
				if (errno != ENOSYS) {
					pr_fail("%s: move_pages failed, errno=%d (%s)\n",
						args->name, errno, strerror(errno));
					goto err;
				}
			}
			(void)stress_mmap_set_light(buf, numa_bytes, page_size);
			if (!stress_continue_flag())
				break;
		}

#if defined(MPOL_MF_MOVE_ALL)
		/* Exercise MPOL_MF_MOVE_ALL, this needs privilege, ignore failure */
		stress_set_numa_array(status, 0x00, num_pages, sizeof(*status));
		pages[0] = buf;
		VOID_RET(long, shim_move_pages(args->pid, num_pages, pages, dest_nodes, status, MPOL_MF_MOVE_ALL));
#endif

		/* Exercise invalid pid on move pages */
		stress_set_numa_array(status, 0x00, num_pages, sizeof(*status));
		pages[0] = buf;
		VOID_RET(long, shim_move_pages(~0, 1, pages, dest_nodes, status, MPOL_MF_MOVE));

		/* Exercise 0 nr_pages */
		stress_set_numa_array(status, 0x00, num_pages, sizeof(*status));
		pages[0] = buf;
		VOID_RET(long, shim_move_pages(args->pid, 0, pages, dest_nodes, status, MPOL_MF_MOVE));

		/* Exercise invalid move flags */
		stress_set_numa_array(status, 0x00, num_pages, sizeof(*status));
		pages[0] = buf;
		VOID_RET(long, shim_move_pages(args->pid, 1, pages, dest_nodes, status, ~0));

		/* Exercise zero flag, should succeed */
		stress_set_numa_array(status, 0x00, num_pages, sizeof(*status));
		pages[0] = buf;
		VOID_RET(long, shim_move_pages(args->pid, 1, pages, dest_nodes, status, 0));

		/* Exercise invalid address */
		stress_set_numa_array(status, 0x00, num_pages, sizeof(*status));
		pages[0] = (void *)(~(uintptr_t)0 & ~(args->page_size - 1));
		VOID_RET(long, shim_move_pages(args->pid, 1, pages, dest_nodes, status, MPOL_MF_MOVE));

		/* Exercise invalid dest_node */
		stress_set_numa_array(status, 0x00, num_pages, sizeof(*status));
		pages[0] = buf;
		dest_nodes[0] = ~0;
		VOID_RET(long, shim_move_pages(args->pid, 1, pages, dest_nodes, status, MPOL_MF_MOVE));

		/* Exercise NULL nodes */
		stress_set_numa_array(status, 0x00, num_pages, sizeof(*status));
		pages[0] = buf;
		VOID_RET(long, shim_move_pages(args->pid, 1, pages, NULL, status, MPOL_MF_MOVE));

		stress_bogo_inc(args);
	} while (stress_continue(args));

	duration = stress_time_now() - t;
	stress_numa_stats_read(&stats_end);

	rate = (duration > 0) ? ((double)stats_end.value[STRESS_NUMA_STAT_NUMA_HIT] -
				 (double)stats_begin.value[STRESS_NUMA_STAT_NUMA_HIT]) / duration : 0.0;
	stress_metrics_set(args, 0, "NUMA hits per sec", rate, STRESS_GEOMETRIC_MEAN);

	rate = (duration > 0) ? ((double)stats_end.value[STRESS_NUMA_STAT_NUMA_MISS] -
				 (double)stats_begin.value[STRESS_NUMA_STAT_NUMA_MISS]) / duration : 0.0;
	stress_metrics_set(args, 1, "NUMA misses per sec", rate, STRESS_GEOMETRIC_MEAN);

	rc = EXIT_SUCCESS;
err:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
	(void)munmap(buf, numa_bytes);

pages_free:
	free(pages);
dest_nodes_free:
	free(dest_nodes);
status_free:
	free(status);
old_node_mask_free:
	free(old_node_mask);
node_mask_free:
	free(node_mask);
numa_free:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
	stress_numa_free_nodes(n);

	return rc;
}

stressor_info_t stress_numa_info = {
	.stressor = stress_numa,
	.stress_class = CLASS_CPU | CLASS_MEMORY | CLASS_OS,
	.verify = VERIFY_ALWAYS,
	.opt_set_funcs = opt_set_funcs,
	.help = help
};
#else
stressor_info_t stress_numa_info = {
	.stressor = stress_unimplemented,
	.stress_class = CLASS_CPU | CLASS_MEMORY | CLASS_OS,
	.verify = VERIFY_ALWAYS,
	.opt_set_funcs = opt_set_funcs,
	.help = help,
	.unimplemented_reason = "built without linux/mempolicy.h, get_mempolicy(), mbind(), migrate_pages(), move_pages() or set_mempolicy()"
};
#endif
