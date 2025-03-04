/*
 * Copyright (C) 2016-2021 Canonical, Ltd.
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
#include "core-sort.h"
#include "core-target-clones.h"

#define MIN_MERGESORT_SIZE	(1 * KB)
#define MAX_MERGESORT_SIZE	(4 * MB)
#define DEFAULT_MERGESORT_SIZE	(256 * KB)

static const stress_help_t help[] = {
	{ NULL,	"mergesort N",		"start N workers merge sorting 32 bit random integers" },
	{ NULL,	"mergesort-method M",	"select sort method [ method-libc | method-nonlibc" },
	{ NULL,	"mergesort-ops N",	"stop after N merge sort bogo operations" },
	{ NULL,	"mergesort-size N",	"number of 32 bit integers to sort" },
	{ NULL,	NULL,			NULL }
};

static volatile bool do_jmp = true;
static sigjmp_buf jmp_env;

typedef int (*mergesort_func_t)(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));

typedef struct {
	const char *name;
	const mergesort_func_t mergesort_func;
} stress_mergesort_method_t;

#define IDX(base, idx, size)	((base) + ((idx) * (size)))

static inline ALWAYS_INLINE void mergesort_copy(uint8_t *RESTRICT p1, uint8_t *RESTRICT p2, register size_t size)
{
	if (size == 4) {
		register uint32_t *u32p1 = (uint32_t *)p1;
		register uint32_t *u32p2 = (uint32_t *)p2;
		register uint32_t *u32end = (uint32_t *)(p1 + size);

		while (LIKELY(u32p1 < u32end)) {
			*(u32p1++) = *(u32p2++);
		}
	} else {
		register uint8_t *u8p1 = p1;
		register uint8_t *u8p2 = p2;
		register uint8_t *u8end = p1 + size;

		while (LIKELY(u8p1 < u8end)) {
			*(u8p1++) = *(u8p2++);
		}
	}
}

/*
 *  mergesort_partition4
 *  	partitioning with 4 byte data
 */
static inline void mergesort_partition4(
	register uint8_t * RESTRICT base,
	register uint8_t * RESTRICT lhs,
	const size_t left,
	const size_t right,
	int (*compar)(const void *, const void *))
{
	size_t mid, lhs_size, rhs_size, lhs_len, rhs_len;
	register ssize_t n;
	register uint8_t *rhs, *lhs_end, *rhs_end;

	mid = left + ((right - left) >> 1);
	if (left < mid)
		mergesort_partition4(base, lhs, left, mid, compar);
	if (mid + 1 < right)
		mergesort_partition4(base, lhs, mid + 1, right, compar);

	lhs_len = mid - left + 1;
	rhs_len = right - mid;

	lhs_size = lhs_len * 4;
	rhs_size = rhs_len * 4;

	rhs = lhs + lhs_size;

	mergesort_copy(lhs, IDX(base, left, 4), lhs_size);
	mergesort_copy(rhs, IDX(base, (mid + 1), 4), rhs_size);

	base = IDX(base, left, 4);
	lhs_end = rhs;
	rhs_end = rhs + rhs_size;

	for (;;) {
		if (compar(lhs, rhs) < 0) {
			*(uint32_t *)base = *(uint32_t *)lhs;
			lhs += 4;
			if (lhs > lhs_end)
				break;
			base += 4;
		} else {
			*(uint32_t *)base = *(uint32_t *)rhs;
			rhs += 4;
			if (rhs > rhs_end)
				break;
			base += 4;
		}
	}

	n = lhs_end - lhs;
	if (n > 0) {
		mergesort_copy(base, lhs, n);
		base += n;
	}
	n = rhs_end - rhs;
	if (n > 0) {
		mergesort_copy(base, rhs, n);
	}
}

/*
 *  mergesort_partition
 *  	partitioning with size sized byte data
 */
static inline void mergesort_partition(
	register uint8_t * RESTRICT base,
	register uint8_t * RESTRICT lhs,
	const size_t left,
	const size_t right,
	const size_t size,
	int (*compar)(const void *, const void *))
{
	size_t mid, lhs_size, rhs_size, lhs_len, rhs_len;
	register ssize_t n;
	register uint8_t *rhs, *lhs_end, *rhs_end;

	mid = left + ((right - left) >> 1);
	if (left < mid)
		mergesort_partition(base, lhs, left, mid, size, compar);
	if (mid + 1 < right)
		mergesort_partition(base, lhs, mid + 1, right, size, compar);

	lhs_len = mid - left + 1;
	rhs_len = right - mid;

	lhs_size = lhs_len * size;
	rhs_size = rhs_len * size;

	rhs = lhs + lhs_size;

	mergesort_copy(lhs, IDX(base, left, size), lhs_size);
	mergesort_copy(rhs, IDX(base, (mid + 1), size), rhs_size);

	base = IDX(base, left, size);
	lhs_end = rhs;
	rhs_end = rhs + rhs_size;

	for (;;) {
		if (compar(lhs, rhs) < 0) {
			mergesort_copy(base, lhs, size);
			lhs += size;
			if (lhs > lhs_end)
				break;
			base += size;
		} else {
			mergesort_copy(base, rhs, size);
			rhs += size;
			if (rhs > rhs_end)
				break;
			base += size;
		}
	}

	n = lhs_end - lhs;
	if (n > 0) {
		mergesort_copy(base, lhs, n);
		base += n;
	}
	n = rhs_end - rhs;
	if (n > 0)
		mergesort_copy(base, rhs, n);
}

static int mergesort_nonlibc(
	void *base,
	size_t nmemb,
	size_t size,
	int (*compar)(const void *, const void *))
{
	uint8_t *lhs;
	size_t mmap_size = nmemb * size;

	lhs = (uint8_t *)stress_mmap_populate(NULL, mmap_size,
			PROT_READ | PROT_WRITE,
			MAP_ANONYMOUS | MAP_PRIVATE,
			-1, 0);
	if (lhs == MAP_FAILED)
		return -1;

	switch (size) {
	case 4:
		mergesort_partition4((uint8_t *)base, lhs, 0, nmemb - 1, compar);
		break;
	default:
		mergesort_partition((uint8_t *)base, lhs, 0, nmemb - 1, size, compar);
		break;
	}
	(void)munmap((void *)lhs, mmap_size);
	return 0;
}

static const stress_mergesort_method_t stress_mergesort_methods[] = {
#if defined(HAVE_LIB_BSD)
	{ "mergesort-libc",	mergesort },
#endif
	{ "mergesort-nonlibc",	mergesort_nonlibc },
};

static int stress_set_mergesort_method(const char *opt)
{
	size_t i;

	for (i = 0; i < SIZEOF_ARRAY(stress_mergesort_methods); i++) {
		if (strcmp(opt, stress_mergesort_methods[i].name) == 0) {
			stress_set_setting("mergesort-method", TYPE_ID_SIZE_T, &i);
			return 0;
		}
	}

	(void)fprintf(stderr, "mergesort-method must be one of:");
	for (i = 0; i < SIZEOF_ARRAY(stress_mergesort_methods); i++) {
		(void)fprintf(stderr, " %s", stress_mergesort_methods[i].name);
	}
	(void)fprintf(stderr, "\n");
	return -1;
}

/*
 *  stress_set_mergesort_size()
 *	set mergesort size
 */
static int stress_set_mergesort_size(const char *opt)
{
	uint64_t mergesort_size;

	mergesort_size = stress_get_uint64(opt);
	stress_check_range("mergesort-size", mergesort_size,
		MIN_MERGESORT_SIZE, MAX_MERGESORT_SIZE);
	return stress_set_setting("mergesort-size", TYPE_ID_UINT64, &mergesort_size);
}

static const stress_opt_set_func_t opt_set_funcs[] = {
	{ OPT_mergesort_size,	stress_set_mergesort_size },
	{ OPT_mergesort_method,	stress_set_mergesort_method },
	{ 0,			NULL }
};

#if !defined(__OpenBSD__) &&	\
    !defined(__NetBSD__)
/*
 *  stress_mergesort_handler()
 *	SIGALRM generic handler
 */
static void MLOCKED_TEXT stress_mergesort_handler(int signum)
{
	(void)signum;

	if (do_jmp) {
		do_jmp = false;
		siglongjmp(jmp_env, 1);		/* Ugly, bounce back */
	}
}
#endif

/*
 *  stress_mergesort()
 *	stress mergesort
 */
static int stress_mergesort(stress_args_t *args)
{
	uint64_t mergesort_size = DEFAULT_MERGESORT_SIZE;
	int32_t *data, *ptr;
	size_t n, i, mergesort_method = 0;
	struct sigaction old_action;
	int ret;
	double rate;
	NOCLOBBER double duration = 0.0, count = 0.0, sorted = 0.0;
	mergesort_func_t mergesort_func;

	(void)stress_get_setting("mergesort-method", &mergesort_method);

	mergesort_func = stress_mergesort_methods[mergesort_method].mergesort_func;
	if (args->instance == 0)
		pr_inf("%s: using method '%s'\n",
			args->name, stress_mergesort_methods[mergesort_method].name);

	if (!stress_get_setting("mergesort-size", &mergesort_size)) {
		if (g_opt_flags & OPT_FLAGS_MAXIMIZE)
			mergesort_size = MAX_MERGESORT_SIZE;
		if (g_opt_flags & OPT_FLAGS_MINIMIZE)
			mergesort_size = MIN_MERGESORT_SIZE;
	}
	n = (size_t)mergesort_size;

	if ((data = calloc(n, sizeof(*data))) == NULL) {
		pr_inf_skip("%s: malloc failed, allocating %zd integers, skipping stressor\n",
			args->name, n);
		return EXIT_NO_RESOURCE;
	}

	ret = sigsetjmp(jmp_env, 1);
	if (ret) {
		/*
		 * We return here if SIGALRM jmp'd back
		 */
		(void)stress_sigrestore(args->name, SIGALRM, &old_action);
		goto tidy;
	}

#if !defined(__OpenBSD__) &&	\
    !defined(__NetBSD__)
	if (stress_sighandler(args->name, SIGALRM, stress_mergesort_handler, &old_action) < 0) {
		free(data);
		return EXIT_FAILURE;
	}
#endif

	stress_sort_data_int32_init(data, n);
	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	do {
		double t;

		stress_sort_data_int32_shuffle(data, n);

		stress_sort_compare_reset();
		t = stress_time_now();
		/* Sort "random" data */
		if (mergesort_func(data, n, sizeof(*data), stress_sort_cmp_fwd_int32) < 0) {
			pr_fail("%s: mergesort of random data failed: %d (%s)\n",
				args->name, errno, strerror(errno));
		} else {
			duration += stress_time_now() - t;
			count += (double)stress_sort_compare_get();
			sorted += (double)n;

			if (g_opt_flags & OPT_FLAGS_VERIFY) {
				for (ptr = data, i = 0; i < n - 1; i++, ptr++) {
					if (*ptr > *(ptr + 1)) {
						pr_fail("%s: sort error "
							"detected, incorrect ordering "
							"found\n", args->name);
						break;
					}
				}
			}
		}
		if (!stress_continue_flag())
			break;

		/* Reverse sort */
		stress_sort_compare_reset();
		t = stress_time_now();
		if (mergesort_func(data, n, sizeof(*data), stress_sort_cmp_rev_int32) < 0) {
			pr_fail("%s: reversed mergesort of random data failed: %d (%s)\n",
				args->name, errno, strerror(errno));
		} else {
			duration += stress_time_now() - t;
			count += (double)stress_sort_compare_get();
			sorted += (double)n;

			if (g_opt_flags & OPT_FLAGS_VERIFY) {
				for (ptr = data, i = 0; i < n - 1; i++, ptr++) {
					if (*ptr < *(ptr + 1)) {
						pr_fail("%s: reverse sort "
							"error detected, incorrect "
							"ordering found\n", args->name);
						break;
					}
				}
			}
		}
		if (!stress_continue_flag())
			break;

		/* And re-order */
		stress_sort_data_int32_mangle(data, n);
		stress_sort_compare_reset();

		/* Reverse sort this again */
		stress_sort_compare_reset();
		t = stress_time_now();
		if (mergesort_func(data, n, sizeof(*data), stress_sort_cmp_rev_int32) < 0) {
			pr_fail("%s: reversed mergesort of random data failed: %d (%s)\n",
				args->name, errno, strerror(errno));
		} else {
			duration += stress_time_now() - t;
			count += (double)stress_sort_compare_get();
			sorted += (double)n;

			if (g_opt_flags & OPT_FLAGS_VERIFY) {

				for (ptr = data, i = 0; i < n - 1; i++, ptr++) {
					if (*ptr < *(ptr + 1)) {
						pr_fail("%s: reverse sort "
							"error detected, incorrect "
							"ordering found\n", args->name);
						break;
					}
				}
			}
		}
		if (!stress_continue_flag())
			break;

		stress_bogo_inc(args);
	} while (stress_continue(args));

	do_jmp = false;
	(void)stress_sigrestore(args->name, SIGALRM, &old_action);
tidy:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
	rate = (duration > 0.0) ? count / duration : 0.0;
	stress_metrics_set(args, 0, "mergesort comparisons per sec",
		rate, STRESS_HARMONIC_MEAN);
	stress_metrics_set(args, 1, "mergesort comparisons per item",
		count / sorted, STRESS_HARMONIC_MEAN);

	free(data);

	return EXIT_SUCCESS;
}

stressor_info_t stress_mergesort_info = {
	.stressor = stress_mergesort,
	.stress_class = CLASS_CPU_CACHE | CLASS_CPU | CLASS_MEMORY,
	.opt_set_funcs = opt_set_funcs,
	.verify = VERIFY_OPTIONAL,
	.help = help
};
