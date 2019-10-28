#include <zjunix/vfs.h>
#include <zjunix/vfscache.h>
#include <zjunix/type.h>
#include <driver/vga.h>
#include <zjunix/errno.h>

extern struct dentry	*root_dentry;		//ȫ�ָ�Ŀ¼��
extern struct vfsmount	*root_mount;		//ȫ�ֵĹ�����Ϣ

//�ѹ��ز���д��
u32 mount_ext2(void) {
	struct dentry		*dentry;
	struct list_head	*head;
	struct list_head	*pos;
	struct vfsmount		*mnt;
	struct qstr			name;
	u32 found;
	
	//�ҵ�ext2�ļ�ϵͳ�Ĺ��ؽṹ
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

	//����FAT32�ļ�ϵͳ�Ĺ��ص�Ŀ¼��
	//���øú���ʱ��Ŀ¼������Ѿ��ڻ����У�����һ��Ҫ����һ������
	name.name = "ext2";
	name.len = 4;
	dentry = d_alloc(&name, root_dentry);	//���ص�㶨ΪFAT32ϵͳ��/ext2��
	if (dentry == 0)
		return -ENODENTRY;

	//����
	dentry->d_mounted = 1;
	dentry->d_inode = mnt->mnt_root->d_inode;
	list_add(&(dentry->d_alias), &(mnt->mnt_root->d_inode->i_dentry));
	mnt->mnt_mountpoint = dentry;
	mnt->mnt_parent = root_mount;
	return 0;
}

//���Ŀ¼����ָ��ĳ���ļ�ϵͳ��װ���һ��Ŀ¼����Ҫͨ����ǰ�ļ�ϵͳ��Ŀ¼�������ļ�ϵͳ����,�ҵ����ص��������dentry�ṹ
//ע��˺���ֻ�ǰ�ͬһ�㼶��Ŀ¼�һ������ݡ������㼶����û�з����仯
//���ļ�ϵͳ�Ĺ��ص�����ļ�ϵͳ�ľֲ���Ŀ¼��ȫ���Ͽ�����һ��Ŀ¼��
//ע�⺯���ڲ����ܻ�Դ���ָ�뱾������޸ģ�����Ҫ�ö���ָ��
//�˺������ط������
u32 follow_mount(struct vfsmount **p_mnt, struct dentry **p_dentry) {
	struct vfsmount		*p_newmnt;
	struct list_head	*head;
	struct list_head	*p;
	u32 err;

	//��ͨ�����ļ�ϵͳdentry��d_mounted�ֶ��ж��Ƿ������ļ�ϵͳ
	if ((*p_dentry)->d_mounted == 0)
		return 0;

	//ͨ������ȫ�ֵ�vfsmount�����õ���dentry�ϵĹ������ݽṹ
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
		//û���ҵ���Ӧ��vfsmount�ṹ������
		return err;

	//�ҵ��˶�Ӧ�Ĺ������ݽṹp_newmnt
	//��ʱ��Ҫ����p_mnt��p_dentry��ָ������ݣ�ʵ�ֲ�ͬ�ļ�ϵͳ�µ��ض���
	*p_mnt = p_newmnt;
	*p_dentry = p_newmnt->mnt_root;
	return 0;
}