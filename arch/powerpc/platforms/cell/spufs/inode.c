
/*
 * SPU file system
 *
 * (C) Copyright IBM Deutschland Entwicklung GmbH 2005
 *
 * Author: Arnd Bergmann <arndb@de.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fsnotify.h>
#include <linux/backing-dev.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/parser.h>

#include <asm/prom.h>
#include <asm/spu.h>
#include <asm/spu_priv1.h>
#include <linux/uaccess.h>

#include "spufs.h"

struct spufs_sb_info {
	int debug;
};

static struct kmem_cache *spufs_inode_cache;
char *isolated_loader;
static int isolated_loader_size;

static struct spufs_sb_info *spufs_get_sb_info(struct super_block *sb)
{
	return sb->s_fs_info;
}

static struct inode *
spufs_alloc_inode(struct super_block *sb)
{
	struct spufs_inode_info *ei;

	ei = kmem_cache_alloc(spufs_inode_cache, GFP_KERNEL);
	if (!ei)
		return NULL;

	ei->i_gang = NULL;
	ei->i_ctx = NULL;
	ei->i_openers = 0;

	return &ei->vfs_inode;
}

static void spufs_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	kmem_cache_free(spufs_inode_cache, SPUFS_I(inode));
}

static void spufs_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, spufs_i_callback);
}

static void
spufs_init_once(void *p)
{
	struct spufs_inode_info *ei = p;

	inode_init_once(&ei->vfs_inode);
}

static struct inode *
spufs_new_inode(struct super_block *sb, umode_t mode)
{
	struct inode *inode;

	inode = new_inode(sb);
	if (!inode)
		goto out;

	inode->i_ino = get_next_ino();
	inode->i_mode = mode;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
out:
	return inode;
}

static int
spufs_setattr(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);

	if ((attr->ia_valid & ATTR_SIZE) &&
	    (attr->ia_size != inode->i_size))
		return -EINVAL;
	setattr_copy(inode, attr);
	mark_inode_dirty(inode);
	return 0;
}


static int
spufs_new_file(struct super_block *sb, struct dentry *dentry,
		const struct file_operations *fops, umode_t mode,
		size_t size, struct spu_context *ctx)
{
	static const struct inode_operations spufs_file_iops = {
		.setattr = spufs_setattr,
	};
	struct inode *inode;
	int ret;

	ret = -ENOSPC;
	inode = spufs_new_inode(sb, S_IFREG | mode);
	if (!inode)
		goto out;

	ret = 0;
	inode->i_op = &spufs_file_iops;
	inode->i_fop = fops;
	inode->i_size = size;
	inode->i_private = SPUFS_I(inode)->i_ctx = get_spu_context(ctx);
	d_add(dentry, inode);
out:
	return ret;
}

static void
spufs_evict_inode(struct inode *inode)
{
	struct spufs_inode_info *ei = SPUFS_I(inode);
	clear_inode(inode);
	if (ei->i_ctx)
		put_spu_context(ei->i_ctx);
	if (ei->i_gang)
		put_spu_gang(ei->i_gang);
}

static void spufs_prune_dir(struct dentry *dir)
{
	struct dentry *dentry, *tmp;

	inode_lock(d_inode(dir));
	list_for_each_entry_safe(dentry, tmp, &dir->d_subdirs, d_child) {
		spin_lock(&dentry->d_lock);
		if (simple_positive(dentry)) {
			dget_dlock(dentry);
			__d_drop(dentry);
			spin_unlock(&dentry->d_lock);
			simple_unlink(d_inode(dir), dentry);
			/* XXX: what was dcache_lock protecting here? Other
			 * filesystems (IB, configfs) release dcache_lock
			 * before unlink */
			dput(dentry);
		} else {
			spin_unlock(&dentry->d_lock);
		}
	}
	shrink_dcache_parent(dir);
	inode_unlock(d_inode(dir));
}

/* Caller must hold parent->i_mutex */
static int spufs_rmdir(struct inode *parent, struct dentry *dir)
{
	/* remove all entries */
	int res;
	spufs_prune_dir(dir);
	d_drop(dir);
	res = simple_rmdir(parent, dir);
	/* We have to give up the mm_struct */
	spu_forget(SPUFS_I(d_inode(dir))->i_ctx);
	return res;
}

static int spufs_fill_dir(struct dentry *dir,
		const struct spufs_tree_descr *files, umode_t mode,
		struct spu_context *ctx)
{
	while (files->name && files->name[0]) {
		int ret;
		struct dentry *dentry = d_alloc_name(dir, files->name);
		if (!dentry)
			return -ENOMEM;
		ret = spufs_new_file(dir->d_sb, dentry, files->ops,
					files->mode & mode, files->size, ctx);
		if (ret)
			return ret;
		files++;
	}
	return 0;
}

static int spufs_dir_close(struct inode *inode, struct file *file)
{
	struct spu_context *ctx;
	struct inode *parent;
	struct dentry *dir;
	int ret;

	dir = file->f_path.dentry;
	parent = d_inode(dir->d_parent);
	ctx = SPUFS_I(d_inode(dir))->i_ctx;

	inode_lock_nested(parent, I_MUTEX_PARENT);
	ret = spufs_rmdir(parent, dir);
	inode_unlock(parent);
	WARN_ON(ret);

	return dcache_dir_close(inode, file);
}

const struct file_operations spufs_context_fops = {
	.open		= dcache_dir_open,
	.release	= spufs_dir_close,
	.llseek		= dcache_dir_lseek,
	.read		= generic_read_dir,
	.iterate_shared	= dcache_readdir,
	.fsync		= noop_fsync,
};
EXPORT_SYMBOL_GPL(spufs_context_fops);

static int
spufs_mkdir(struct inode *dir, struct dentry *dentry, unsigned int flags,
		umode_t mode)
{
	int ret;
	struct inode *inode;
	struct spu_context *ctx;

	inode = spufs_new_inode(dir->i_sb, mode | S_IFDIR);
	if (!inode)
		return -ENOSPC;

	if (dir->i_mode & S_ISGID) {
		inode->i_gid = dir->i_gid;
		inode->i_mode &= S_ISGID;
	}
	ctx = alloc_spu_context(SPUFS_I(dir)->i_gang); /* XXX gang */
	SPUFS_I(inode)->i_ctx = ctx;
	if (!ctx) {
		iput(inode);
		return -ENOSPC;
	}

	ctx->flags = flags;
	inode->i_op = &simple_dir_inode_operations;
	inode->i_fop = &simple_dir_operations;

	inode_lock(inode);

	dget(dentry);
	inc_nlink(dir);
	inc_nlink(inode);

	d_instantiate(dentry, inode);

	if (flags & SPU_CREATE_NOSCHED)
		ret = spufs_fill_dir(dentry, spufs_dir_nosched_contents,
					 mode, ctx);
	else
		ret = spufs_fill_dir(dentry, spufs_dir_contents, mode, ctx);

	if (!ret && spufs_get_sb_info(dir->i_sb)->debug)
		ret = spufs_fill_dir(dentry, spufs_dir_debug_contents,
				mode, ctx);

	if (ret)
		spufs_rmdir(dir, dentry);

	inode_unlock(inode);

	return ret;
}

static int spufs_context_open(struct path *path)
{
	int ret;
	struct file *filp;

	ret = get_unused_fd_flags(0);
	if (ret < 0)
		return ret;

	filp = dentry_open(path, O_RDONLY, current_cred());
	if (IS_ERR(filp)) {
		put_unused_fd(ret);
		return PTR_ERR(filp);
	}

	filp->f_op = &spufs_context_fops;
	fd_install(ret, filp);
	return ret;
}

static struct spu_context *
spufs_assert_affinity(unsigned int flags, struct spu_gang *gang,
						struct file *filp)
{
	struct spu_context *tmp, *neighbor, *err;
	int count, node;
	int aff_supp;

	aff_supp = !list_empty(&(list_entry(cbe_spu_info[0].spus.next,
					struct spu, cbe_list))->aff_list);

	if (!aff_supp)
		return ERR_PTR(-EINVAL);

	if (flags & SPU_CREATE_GANG)
		return ERR_PTR(-EINVAL);

	if (flags & SPU_CREATE_AFFINITY_MEM &&
	    gang->aff_ref_ctx &&
	    gang->aff_ref_ctx->flags & SPU_CREATE_AFFINITY_MEM)
		return ERR_PTR(-EEXIST);

	if (gang->aff_flags & AFF_MERGED)
		return ERR_PTR(-EBUSY);

	neighbor = NULL;
	if (flags & SPU_CREATE_AFFINITY_SPU) {
		if (!filp || filp->f_op != &spufs_context_fops)
			return ERR_PTR(-EINVAL);

		neighbor = get_spu_context(
				SPUFS_I(file_inode(filp))->i_ctx);

		if (!list_empty(&neighbor->aff_list) && !(neighbor->aff_head) &&
		    !list_is_last(&neighbor->aff_list, &gang->aff_list_head) &&
		    !list_entry(neighbor->aff_list.next, struct spu_context,
		    aff_list)->aff_head) {
			err = ERR_PTR(-EEXIST);
			goto out_put_neighbor;
		}

		if (gang != neighbor->gang) {
			err = ERR_PTR(-EINVAL);
			goto out_put_neighbor;
		}

		count = 1;
		list_for_each_entry(tmp, &gang->aff_list_head, aff_list)
			count++;
		if (list_empty(&neighbor->aff_list))
			count++;

		for (node = 0; node < MAX_NUMNODES; node++) {
			if ((cbe_spu_info[node].n_spus - atomic_read(
				&cbe_spu_info[node].reserved_spus)) >= count)
				break;
		}

		if (node == MAX_NUMNODES) {
			err = ERR_PTR(-EEXIST);
			goto out_put_neighbor;
		}
	}

	return neighbor;

out_put_neighbor:
	put_spu_context(neighbor);
	return err;
}

static void
spufs_set_affinity(unsigned int flags, struct spu_context *ctx,
					struct spu_context *neighbor)
{
	if (flags & SPU_CREATE_AFFINITY_MEM)
		ctx->gang->aff_ref_ctx = ctx;

	if (flags & SPU_CREATE_AFFINITY_SPU) {
		if (list_empty(&neighbor->aff_list)) {
			list_add_tail(&neighbor->aff_list,
				&ctx->gang->aff_list_head);
			neighbor->aff_head = 1;
		}

		if (list_is_last(&neighbor->aff_list, &ctx->gang->aff_list_head)
		    || list_entry(neighbor->aff_list.next, struct spu_context,
							aff_list)->aff_head) {
			list_add(&ctx->aff_list, &neighbor->aff_list);
		} else  {
			list_add_tail(&ctx->aff_list, &neighbor->aff_list);
			if (neighbor->aff_head) {
				neighbor->aff_head = 0;
				ctx->aff_head = 1;
			}
		}

		if (!ctx->gang->aff_ref_ctx)
			ctx->gang->aff_ref_ctx = ctx;
	}
}

static int
spufs_create_context(struct inode *inode, struct dentry *dentry,
			struct vfsmount *mnt, int flags, umode_t mode,
			struct file *aff_filp)
{
	int ret;
	int affinity;
	struct spu_gang *gang;
	struct spu_context *neighbor;
	struct path path = {.mnt = mnt, .dentry = dentry};

	if ((flags & SPU_CREATE_NOSCHED) &&
	    !capable(CAP_SYS_NICE))
		return -EPERM;

	if ((flags & (SPU_CREATE_NOSCHED | SPU_CREATE_ISOLATE))
	    == SPU_CREATE_ISOLATE)
		return -EINVAL;

	if ((flags & SPU_CREATE_ISOLATE) && !isolated_loader)
		return -ENODEV;

	gang = NULL;
	neighbor = NULL;
	affinity = flags & (SPU_CREATE_AFFINITY_MEM | SPU_CREATE_AFFINITY_SPU);
	if (affinity) {
		gang = SPUFS_I(inode)->i_gang;
		if (!gang)
			return -EINVAL;
		mutex_lock(&gang->aff_mutex);
		neighbor = spufs_assert_affinity(flags, gang, aff_filp);
		if (IS_ERR(neighbor)) {
			ret = PTR_ERR(neighbor);
			goto out_aff_unlock;
		}
	}

	ret = spufs_mkdir(inode, dentry, flags, mode & 0777);
	if (ret)
		goto out_aff_unlock;

	if (affinity) {
		spufs_set_affinity(flags, SPUFS_I(d_inode(dentry))->i_ctx,
								neighbor);
		if (neighbor)
			put_spu_context(neighbor);
	}

	ret = spufs_context_open(&path);
	if (ret < 0)
		WARN_ON(spufs_rmdir(inode, dentry));

out_aff_unlock:
	if (affinity)
		mutex_unlock(&gang->aff_mutex);
	return ret;
}

static int
spufs_mkgang(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int ret;
	struct inode *inode;
	struct spu_gang *gang;

	ret = -ENOSPC;
	inode = spufs_new_inode(dir->i_sb, mode | S_IFDIR);
	if (!inode)
		goto out;

	ret = 0;
	if (dir->i_mode & S_ISGID) {
		inode->i_gid = dir->i_gid;
		inode->i_mode &= S_ISGID;
	}
	gang = alloc_spu_gang();
	SPUFS_I(inode)->i_ctx = NULL;
	SPUFS_I(inode)->i_gang = gang;
	if (!gang) {
		ret = -ENOMEM;
		goto out_iput;
	}

	inode->i_op = &simple_dir_inode_operations;
	inode->i_fop = &simple_dir_operations;

	d_instantiate(dentry, inode);
	inc_nlink(dir);
	inc_nlink(d_inode(dentry));
	return ret;

out_iput:
	iput(inode);
out:
	return ret;
}

static int spufs_gang_open(struct path *path)
{
	int ret;
	struct file *filp;

	ret = get_unused_fd_flags(0);
	if (ret < 0)
		return ret;

	/*
	 * get references for dget and mntget, will be released
	 * in error path of *_open().
	 */
	filp = dentry_open(path, O_RDONLY, current_cred());
	if (IS_ERR(filp)) {
		put_unused_fd(ret);
		return PTR_ERR(filp);
	}

	filp->f_op = &simple_dir_operations;
	fd_install(ret, filp);
	return ret;
}

static int spufs_create_gang(struct inode *inode,
			struct dentry *dentry,
			struct vfsmount *mnt, umode_t mode)
{
	struct path path = {.mnt = mnt, .dentry = dentry};
	int ret;

	ret = spufs_mkgang(inode, dentry, mode & 0777);
	if (!ret) {
		ret = spufs_gang_open(&path);
		if (ret < 0) {
			int err = simple_rmdir(inode, dentry);
			WARN_ON(err);
		}
	}
	return ret;
}


static struct file_system_type spufs_type;

long spufs_create(struct path *path, struct dentry *dentry,
		unsigned int flags, umode_t mode, struct file *filp)
{
	struct inode *dir = d_inode(path->dentry);
	int ret;

	/* check if we are on spufs */
	if (path->dentry->d_sb->s_type != &spufs_type)
		return -EINVAL;

	/* don't accept undefined flags */
	if (flags & (~SPU_CREATE_FLAG_ALL))
		return -EINVAL;

	/* only threads can be underneath a gang */
	if (path->dentry != path->dentry->d_sb->s_root)
		if ((flags & SPU_CREATE_GANG) || !SPUFS_I(dir)->i_gang)
			return -EINVAL;

	mode &= ~current_umask();

	if (flags & SPU_CREATE_GANG)
		ret = spufs_create_gang(dir, dentry, path->mnt, mode);
	else
		ret = spufs_create_context(dir, dentry, path->mnt, flags, mode,
					    filp);
	if (ret >= 0)
		fsnotify_mkdir(dir, dentry);

	return ret;
}

/* File system initialization */
enum {
	Opt_uid, Opt_gid, Opt_mode, Opt_debug, Opt_err,
};

static const match_table_t spufs_tokens = {
	{ Opt_uid,   "uid=%d" },
	{ Opt_gid,   "gid=%d" },
	{ Opt_mode,  "mode=%o" },
	{ Opt_debug, "debug" },
	{ Opt_err,    NULL  },
};

static int spufs_show_options(struct seq_file *m, struct dentry *root)
{
	struct spufs_sb_info *sbi = spufs_get_sb_info(root->d_sb);
	struct inode *inode = root->d_inode;

	if (!uid_eq(inode->i_uid, GLOBAL_ROOT_UID))
		seq_printf(m, ",uid=%u",
			   from_kuid_munged(&init_user_ns, inode->i_uid));
	if (!gid_eq(inode->i_gid, GLOBAL_ROOT_GID))
		seq_printf(m, ",gid=%u",
			   from_kgid_munged(&init_user_ns, inode->i_gid));
	if ((inode->i_mode & S_IALLUGO) != 0775)
		seq_printf(m, ",mode=%o", inode->i_mode);
	if (sbi->debug)
		seq_puts(m, ",debug");
	return 0;
}

static int
spufs_parse_options(struct super_block *sb, char *options, struct inode *root)
{
	char *p;
	substring_t args[MAX_OPT_ARGS];

	while ((p = strsep(&options, ",")) != NULL) {
		int token, option;

		if (!*p)
			continue;

		token = match_token(p, spufs_tokens, args);
		switch (token) {
		case Opt_uid:
			if (match_int(&args[0], &option))
				return 0;
			root->i_uid = make_kuid(current_user_ns(), option);
			if (!uid_valid(root->i_uid))
				return 0;
			break;
		case Opt_gid:
			if (match_int(&args[0], &option))
				return 0;
			root->i_gid = make_kgid(current_user_ns(), option);
			if (!gid_valid(root->i_gid))
				return 0;
			break;
		case Opt_mode:
			if (match_octal(&args[0], &option))
				return 0;
			root->i_mode = option | S_IFDIR;
			break;
		case Opt_debug:
			spufs_get_sb_info(sb)->debug = 1;
			break;
		default:
			return 0;
		}
	}
	return 1;
}

static void spufs_exit_isolated_loader(void)
{
	free_pages((unsigned long) isolated_loader,
			get_order(isolated_loader_size));
}

static void
spufs_init_isolated_loader(void)
{
	struct device_node *dn;
	const char *loader;
	int size;

	dn = of_find_node_by_path("/spu-isolation");
	if (!dn)
		return;

	loader = of_get_property(dn, "loader", &size);
	of_node_put(dn);
	if (!loader)
		return;

	/* the loader must be align on a 16 byte boundary */
	isolated_loader = (char *)__get_free_pages(GFP_KERNEL, get_order(size));
	if (!isolated_loader)
		return;

	isolated_loader_size = size;
	memcpy(isolated_loader, loader, size);
	printk(KERN_INFO "spufs: SPU isolation mode enabled\n");
}

static int
spufs_create_root(struct super_block *sb, void *data)
{
	struct inode *inode;
	int ret;

	ret = -ENODEV;
	if (!spu_management_ops)
		goto out;

	ret = -ENOMEM;
	inode = spufs_new_inode(sb, S_IFDIR | 0775);
	if (!inode)
		goto out;

	inode->i_op = &simple_dir_inode_operations;
	inode->i_fop = &simple_dir_operations;
	SPUFS_I(inode)->i_ctx = NULL;
	inc_nlink(inode);

	ret = -EINVAL;
	if (!spufs_parse_options(sb, data, inode))
		goto out_iput;

	ret = -ENOMEM;
	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		goto out;

	return 0;
out_iput:
	iput(inode);
out:
	return ret;
}

static int
spufs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct spufs_sb_info *info;
	static const struct super_operations s_ops = {
		.alloc_inode = spufs_alloc_inode,
		.destroy_inode = spufs_destroy_inode,
		.statfs = simple_statfs,
		.evict_inode = spufs_evict_inode,
		.show_options = spufs_show_options,
	};

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_magic = SPUFS_MAGIC;
	sb->s_op = &s_ops;
	sb->s_fs_info = info;

	return spufs_create_root(sb, data);
}

static struct dentry *
spufs_mount(struct file_system_type *fstype, int flags,
		const char *name, void *data)
{
	return mount_single(fstype, flags, data, spufs_fill_super);
}

static struct file_system_type spufs_type = {
	.owner = THIS_MODULE,
	.name = "spufs",
	.mount = spufs_mount,
	.kill_sb = kill_litter_super,
};
MODULE_ALIAS_FS("spufs");

static int __init spufs_init(void)
{
	int ret;

	ret = -ENODEV;
	if (!spu_management_ops)
		goto out;

	ret = -ENOMEM;
	spufs_inode_cache = kmem_cache_create("spufs_inode_cache",
			sizeof(struct spufs_inode_info), 0,
			SLAB_HWCACHE_ALIGN|SLAB_ACCOUNT, spufs_init_once);

	if (!spufs_inode_cache)
		goto out;
	ret = spu_sched_init();
	if (ret)
		goto out_cache;
	ret = register_spu_syscalls(&spufs_calls);
	if (ret)
		goto out_sched;
	ret = register_filesystem(&spufs_type);
	if (ret)
		goto out_syscalls;

	spufs_init_isolated_loader();

	return 0;

out_syscalls:
	unregister_spu_syscalls(&spufs_calls);
out_sched:
	spu_sched_exit();
out_cache:
	kmem_cache_destroy(spufs_inode_cache);
out:
	return ret;
}
module_init(spufs_init);

static void __exit spufs_exit(void)
{
	spu_sched_exit();
	spufs_exit_isolated_loader();
	unregister_spu_syscalls(&spufs_calls);
	unregister_filesystem(&spufs_type);
	kmem_cache_destroy(spufs_inode_cache);
}
module_exit(spufs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Arnd Bergmann <arndb@de.ibm.com>");

