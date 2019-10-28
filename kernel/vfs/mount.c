#include <zjunix/vfs.h>
#include <zjunix/vfscache.h>
#include <zjunix/type.h>
#include <driver/vga.h>
#include <zjunix/errno.h>

extern struct dentry	*root_dentry;		//全局根目录项
extern struct vfsmount	*root_mount;		//全局的挂载信息

//把挂载操作写死
u32 mount_ext2(void) {
	struct dentry		*dentry;
	struct list_head	*head;
	struct list_head	*pos;
	struct vfsmount		*mnt;
	struct qstr			name;
	u32 found;
	
	//找到ext2文件系统的挂载结构
	found = 0;
	head = &root_mount->mnt_hash;
	list_for_each(pos, head) {
		mnt = container_of(pos, struct vfsmount, mnt_hash);
		if (kernel_strcmp(mnt->mnt_sb->s_type->name, "EXT2") == 0) {
			found = 1;
			break;
		}
	}
	if (!found)
		return -ENOVFSMNT;

	//构建FAT32文件系统的挂载点目录项
	//调用该函数时该目录项不可能已经在缓存中，所以一定要创建一个出来
	name.name = "ext2";
	name.len = 4;
	dentry = d_alloc(&name, root_dentry);	//挂载点恒定为FAT32系统的/ext2处
	if (dentry == 0)
		return -ENODENTRY;

	//挂载
	dentry->d_mounted = 1;
	dentry->d_inode = mnt->mnt_root->d_inode;
	list_add(&(dentry->d_alias), &(mnt->mnt_root->d_inode->i_dentry));
	mnt->mnt_mountpoint = dentry;
	mnt->mnt_parent = root_mount;
	return 0;
}

//如果目录项是指向某个文件系统安装点的一个目录，需要通过当前文件系统的目录项对象和文件系统对象,找到挂载点的真正的dentry结构
//注意此函数只是把同一层级的目录项换一个“身份”，而层级本身没有发生变化
//父文件系统的挂载点和子文件系统的局部根目录在全局上看都是一个目录项
//注意函数内部可能会对传入指针本身进行修改，所以要用二级指针
//此函数返回非零错误
u32 follow_mount(struct vfsmount **p_mnt, struct dentry **p_dentry) {
	struct vfsmount		*p_newmnt;
	struct list_head	*head;
	struct list_head	*p;
	u32 err;

	//先通过父文件系统dentry的d_mounted字段判断是否有子文件系统
	if ((*p_dentry)->d_mounted == 0)
		return 0;

	//通过查找全局的vfsmount链表，得到该dentry上的挂载数据结构
	err = -ENOVFSMNT;
	head = &root_mount->mnt_hash;
	p = head;
	while (1) {
		p_newmnt = container_of(p, struct vfsmount, mnt_hash);
		if (p_newmnt->mnt_parent == *p_mnt && p_newmnt->mnt_mountpoint == *p_dentry) {
			err = 0;
			break;
		}
		p = p->next;
		if (p == head)
			break;
	}

	if (err)
		//没有找到对应的vfsmount结构，出错
		return err;

	//找到了对应的挂载数据结构p_newmnt
	//此时需要更换p_mnt和p_dentry所指向的内容，实现不同文件系统下的重定向
	*p_mnt = p_newmnt;
	*p_dentry = p_newmnt->mnt_root;
	return 0;
}