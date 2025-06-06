/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/types.h>

struct flag_info {
	u64		mask;
	u64		val;
	const char	*set;
	const char	*clear;
	bool		is_val;
	int		shift;
};

struct pgtable_level {
	const struct flag_info *flag;
	size_t num;
	u64 mask;
};

extern struct pgtable_level pg_level[5];
