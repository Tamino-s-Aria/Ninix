#include <zjunix/vfs.h>
#include <zjunix/vfscache.h>
#include <zjunix/vi.h>
#include <zjunix/type.h>
#include <zjunix/errno.h>
#include <zjunix/err.h>
#include <zjunix/slab.h>

//�ⲿ����
extern struct cache                     * dcache;
extern struct cache                     * pcache;
extern struct dentry					* root_dentry;
extern struct dentry                    * pwd_dentry;
extern struct vfsmount                  * pwd_mount;

//���ļ��ṩ���ļ�ϵͳ��API���û�ʹ��

//cat������ú���
u32 vfs_cat(const u8 *path) {
	struct file*	f;
	u32 err;
	u32 size;
	u32 cnt;
	u8	*buf;

	//���ļ�
	f = vfs_open(path, O_RDONLY, 0);	//���һ��������ʵû���õ�
	if (IS_ERR_VALUE(f)) {
		kernel_printf("Cannot find or open this file!\n");
		err = PTR_ERR(f);
		return err;
	}

	size = f->f_dentry->d_inode->i_size;
	buf = (u8*)kmalloc(sizeof(u8)*(size + 1));
	if (buf == 0)
		return -ENOMEM;

	//��ȡ���ݲ���֤�Ƿ����ȷ
	cnt = f->f_op->read(f, buf, size);
	if (cnt != size)
		return -EREAD;

	//��ӡ
	buf[size] = 0;
	kernel_printf("%s\n", buf);

	//�ر��ļ����ͷ���Դ
	err = vfs_close(f);
	if (err) {
		kernel_printf("Fail to close this file!\n");
	}
		
	kfree(buf);
	return err;
}

//cd������ú���
u32 vfs_cd(const u8 *path) {
	struct nameidata	nd;
	u32 err;

	err = path_lookup(path, LOOKUP_DIRECTORY, &nd);
	if (err) {
		kernel_printf("Cannot find this directory!\n");
		return err;
	}

	//��������Ч��nd
	pwd_dentry = nd.dentry;
	pwd_mount = nd.mnt;
	return 0;
}

//ls������ú���
u32 vfs_ls(const u8 *path) {
	u32 err;
	u32 i;
	u8  type;
	struct file			*f;
	struct getdent		dir;
	
	err = 0;
	if (*path) 
		//�����ر�ָ�������ϸ���ָ����·����Ŀ¼�ļ�
		f = vfs_open(path, O_RDONLY | O_DIR, 0);
	else 
		//�����Ĭ�ϣ���򿪵�ǰ·���ϵ�Ŀ¼�ļ�
		f = vfs_open(".", O_RDONLY | O_DIR, 0);
	
	//���
	if (IS_ERR_VALUE(f)) {
		kernel_printf("Cannot find this directory!\n");
		err = PTR_ERR(f);
		goto out;
	}
	//kernel_printf("fopen done!\n");
	//��ͨ��Ŀ¼���ȡ�ṹ��ȥȡ��Ŀ¼�ļ���
	err = f->f_op->readdir(f, &dir);
	if (err) {
		kernel_printf("Fail to read this directory!\n");
		goto out;
	}
		
	//��ӡ��Ϣ
	kernel_printf("Type    Name\n");
	for (i = 0; i < dir.count; i++) {
		type = (dir.dirent[i].type == LAST_DIR) ? 'D' : 'A';
		kernel_printf(" %c      %s\n", type, dir.dirent[i].name);
	}

	//�ر��ļ�
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

//touch������ú���
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

//mkdir������ú���
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

//rm������ú���
u32 vfs_rm(const u8 *path) {
	struct super_block	*sb;
	struct nameidata	nd;
	u32 err;

	//��ͨ��·���ҵ���Ӧ��Ŀ¼����Ϣ
	err = path_lookup(path, 0, &nd);	//�˴�flagΪ0��ʾ��ֻ��ģʽ�ҵ���Ӧ����ͨ�ļ�
	if (err) {
		kernel_printf("Cannot find this file!\n");
		return err;
	}	

	//���õײ�ӿ�ɾ���ļ���Ԫ��Ϣ��������Ϣ
	sb = nd.dentry->d_sb;
	err = sb->s_op->delete_inode(nd.dentry, LAST_NORM);
	if (err) {
		kernel_printf("Fail to delete this file!\n");
	}
	return err;
}

//rmdir������ú���
u32 vfs_rmdir(const u8 *path) {
	struct super_block	*sb;
	struct nameidata	nd;
	u32 isempty;
	u32 err;

	//��ͨ��·���ҵ���Ӧ��Ŀ¼����Ϣ
	err = path_lookup(path, LOOKUP_DIRECTORY, &nd);	//��ֻ��ģʽ�ҵ���Ӧ��Ŀ¼�ļ�
	if (err) {
		kernel_printf("Cannot find this directory!\n");
		return err;
	}

	//�鿴��Ŀ¼�Ƿ��ǿյģ�������ǲ���ɾ
	isempty = nd.dentry->d_op->isempty(nd.dentry);
	if (!isempty) {
		kernel_printf("Cannot delete a directory that is not empty!\n");
		return 0;
	}

	//�����Ŀ¼���ڱ�ʹ�ã�Ҳ����ɾ
	if (nd.dentry->d_pinned || pwd_dentry == nd.dentry || root_dentry == nd.dentry) {
		kernel_printf("Cannot delete a directory that is in use now!\n");
		return 0;
	}

	//��������Ϳ���ɾ��
	sb = nd.dentry->d_sb;
	err = sb->s_op->delete_inode(nd.dentry, LAST_DIR);
	if (err) {
		kernel_printf("Fail to delete this directory!\n");
		kernel_printf("Excode: %d", -err);
	}
	return err;
}

//vi������ú���
u32 vfs_vi(const u8 *path) {
	u32 err;
	err = vi(path);
	if (err) {
		kernel_printf("Some problems occur! Error code: %d\n", -err);
	}
	return err;
}
