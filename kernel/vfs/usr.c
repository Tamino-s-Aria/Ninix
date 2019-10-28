#include <zjunix/vfs.h>
#include <zjunix/vfscache.h>
#include <zjunix/vi.h>
#include <zjunix/type.h>
#include <zjunix/errno.h>
#include <zjunix/err.h>
#include <zjunix/slab.h>

//外部变量
extern struct cache                     * dcache;
extern struct cache                     * pcache;
extern struct dentry					* root_dentry;
extern struct dentry                    * pwd_dentry;
extern struct vfsmount                  * pwd_mount;

//该文件提供了文件系统的API供用户使用

//cat命令调用函数
u32 vfs_cat(const u8 *path) {
	struct file*	f;
	u32 err;
	u32 size;
	u32 cnt;
	u8	*buf;

	//打开文件
	f = vfs_open(path, O_RDONLY, 0);	//最后一个参数其实没有用到
	if (IS_ERR_VALUE(f)) {
		kernel_printf("Cannot find or open this file!\n");
		err = PTR_ERR(f);
		return err;
	}

	size = f->f_dentry->d_inode->i_size;
	buf = (u8*)kmalloc(sizeof(u8)*(size + 1));
	if (buf == 0)
		return -ENOMEM;

	//读取数据并验证是否读正确
	cnt = f->f_op->read(f, buf, size);
	if (cnt != size)
		return -EREAD;

	//打印
	buf[size] = 0;
	kernel_printf("%s\n", buf);

	//关闭文件、释放资源
	err = vfs_close(f);
	if (err) {
		kernel_printf("Fail to close this file!\n");
	}
		
	kfree(buf);
	return err;
}

//cd命令调用函数
u32 vfs_cd(const u8 *path) {
	struct nameidata	nd;
	u32 err;

	err = path_lookup(path, LOOKUP_DIRECTORY, &nd);
	if (err) {
		kernel_printf("Cannot find this directory!\n");
		return err;
	}

	//返回了有效的nd
	pwd_dentry = nd.dentry;
	pwd_mount = nd.mnt;
	return 0;
}

//ls命令调用函数
u32 vfs_ls(const u8 *path) {
	u32 err;
	u32 i;
	u8  type;
	struct file			*f;
	struct getdent		dir;
	
	err = 0;
	if (*path) 
		//如有特别指定，则严格按照指定的路径打开目录文件
		f = vfs_open(path, O_RDONLY | O_DIR, 0);
	else 
		//如果是默认，则打开当前路径上的目录文件
		f = vfs_open(".", O_RDONLY | O_DIR, 0);
	
	//检错
	if (IS_ERR_VALUE(f)) {
		kernel_printf("Cannot find this directory!\n");
		err = PTR_ERR(f);
		goto out;
	}
	//kernel_printf("fopen done!\n");
	//用通用目录项获取结构体去取该目录文件的
	err = f->f_op->readdir(f, &dir);
	if (err) {
		kernel_printf("Fail to read this directory!\n");
		goto out;
	}
		
	//打印信息
	kernel_printf("Type    Name\n");
	for (i = 0; i < dir.count; i++) {
		type = (dir.dirent[i].type == LAST_DIR) ? 'D' : 'A';
		kernel_printf(" %c      %s\n", type, dir.dirent[i].name);
	}

	//关闭文件
	err = vfs_close(f);
	if (err) {
		kernel_printf("Fail to close this file!\n");
	}
		
	if (dir.dirent) {
		for (i = 0; i < dir.count; i++) {
			if (dir.dirent[i].name)
				kfree(dir.dirent[i].name);
		}
		kfree(dir.dirent);
	}
out:	
	return err;
}

//touch命令调用函数
u32 vfs_new(const u8* path) {
	struct file		*f;
	u32 err;
	f = vfs_open(path, O_RDONLY | O_CREATE, 0);
	if (IS_ERR_OR_NULL(f)) {
		kernel_printf("Fail to find or create this file!\n");
		return PTR_ERR(f);
	}

	err = vfs_close(f);
	if (err) {
		kernel_printf("Fail to close this file!\n");
	}
	return err;
}

//mkdir命令调用函数
u32 vfs_mkdir(const u8* path) {
	struct file		*f;
	u32 err;
	f = vfs_open(path, O_RDONLY | O_CREATE | O_DIR, 0);
	if (IS_ERR_OR_NULL(f)) {
		kernel_printf("Fail to create this direntory!\n");
		return PTR_ERR(f);
	}
	err = vfs_close(f);
	if (err) {
		kernel_printf("Fail to close this file!\n");
	}
	return err;
}

//rm命令调用函数
u32 vfs_rm(const u8 *path) {
	struct super_block	*sb;
	struct nameidata	nd;
	u32 err;

	//先通过路径找到对应的目录项信息
	err = path_lookup(path, 0, &nd);	//此处flag为0表示用只读模式找到对应的普通文件
	if (err) {
		kernel_printf("Cannot find this file!\n");
		return err;
	}	

	//调用底层接口删除文件的元信息和数据信息
	sb = nd.dentry->d_sb;
	err = sb->s_op->delete_inode(nd.dentry, LAST_NORM);
	if (err) {
		kernel_printf("Fail to delete this file!\n");
	}
	return err;
}

//rmdir命令调用函数
u32 vfs_rmdir(const u8 *path) {
	struct super_block	*sb;
	struct nameidata	nd;
	u32 isempty;
	u32 err;

	//先通过路径找到对应的目录项信息
	err = path_lookup(path, LOOKUP_DIRECTORY, &nd);	//用只读模式找到对应的目录文件
	if (err) {
		kernel_printf("Cannot find this directory!\n");
		return err;
	}

	//查看该目录是否是空的，如果不是不能删
	isempty = nd.dentry->d_op->isempty(nd.dentry);
	if (!isempty) {
		kernel_printf("Cannot delete a directory that is not empty!\n");
		return 0;
	}

	//如果该目录正在被使用，也不能删
	if (nd.dentry->d_pinned || pwd_dentry == nd.dentry || root_dentry == nd.dentry) {
		kernel_printf("Cannot delete a directory that is in use now!\n");
		return 0;
	}

	//其他情况就可以删了
	sb = nd.dentry->d_sb;
	err = sb->s_op->delete_inode(nd.dentry, LAST_DIR);
	if (err) {
		kernel_printf("Fail to delete this directory!\n");
		kernel_printf("Excode: %d", -err);
	}
	return err;
}

//vi命令调用函数
u32 vfs_vi(const u8 *path) {
	u32 err;
	err = vi(path);
	if (err) {
		kernel_printf("Some problems occur! Error code: %d\n", -err);
	}
	return err;
}
