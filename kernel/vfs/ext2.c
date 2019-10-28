#include <zjunix/ext2.h>
#include <zjunix/vfscache.h>
#include <zjunix/slab.h>
#include <zjunix/utils.h>
#include <driver/sd.h>

//extern vfs.c
extern struct dentry* 		root_dentry;		//ȫ�ֵĸ�Ŀ¼��
extern struct dentry* 		pwd_dentry;			//��ǰ����Ŀ¼��
extern struct vfsmount* 	root_mount;			//ȫ�ֵĸ�������Ϣ
extern struct vfsmount* 	pwd_mount;			//��ǰ���ڹ�����Ϣ

//extern vfscache.c
extern struct cache			*dcache;			//dentry����
extern struct cache			*pcache;			//page����

//ȫ�ֱ���
struct ext2_dptentry		*dpttable;				//�������������
u8							sb_buf[BYTES_PER_SECT];	//����������

//ʵ�����������ݽṹ���ú���ָ��ָ��EXT2ϵͳ��Ӧ�ľ��庯��
struct address_space_operations ext2_address_space_operations = {
	.writepage = ext2_writepage,
	.readpage = ext2_readpage,
	.createpage = ext2_create_page,
	.bmap = ext2_bmap,
};

struct dentry_operations ext2_dentry_operations = {
	.compare = generic_check_filename,
	.isempty = ext2_isempty,
};

struct inode_operations ext2_inode_operations = {
	.create = ext2_createinode,
	.lookup = ext2_lookup,
};

struct file_operations ext2_file_operations = {
	.read = generic_file_read,
	.write = generic_file_write,
	.flush = generic_file_flush,
	.readdir = ext2_readdir,
};

struct super_operations ext2_super_operations = {
	.delete_inode = ext2_deleteinode,
	.write_inode = ext2_writeinode,
};

//��ָ��(����)������ʼ������Ϣ���Գ�ʼ��EXT2ϵͳ
u32 ext2_init(u32 base_sect) {
	struct super_block		*ext2_sb;				//ͨ�ó�����
	struct ext2_ext_sb		*ext2_esb;				//��������Ը��ļ�ϵͳ����չ��Ϣ
	struct file_system_type *ext2_fs_type;			//���ļ�ϵͳ����
	struct dentry           *ext2_root_dentry;		//���ļ�ϵͳ�ĸ�Ŀ¼��
	struct inode            *ext2_root_inode;		//���ļ�ϵͳ���������ڵ����
	struct ext2_inode		xinode;					//���ļ�ϵͳ���ڴ����ϵ������ڵ����
	struct address_space	*mapping;				//���ļ�ϵͳ�������ڵ�ĵ�ַ�ռ�
	struct vfsmount			*ext2_root_mount;		//���ļ�ϵͳ���Ĺ�����Ϣ
	u32						blocksize_factor;
	u32						blocksize;
	u32						bitmapsize;
	u32						err;

	//��ȡ���������ݣ����������ļ�ϵͳ����ַ��1024�ֽ�(2������)֮��ʼ
	if (read_block(sb_buf, base_sect + 2, 1))
		return -EIO;

	//ʵ������չ������
	ext2_esb = (struct ext2_ext_sb*)kmalloc(sizeof(struct ext2_ext_sb));
	if (ext2_esb == 0)
		return -ENOMEM;

	//��һ����ʼ����չ������
	kernel_memcpy(&ext2_esb->total_inodes, sb_buf, sizeof(u32));
	kernel_memcpy(&ext2_esb->total_blocks, sb_buf + 4, sizeof(u32));
	kernel_memcpy(&ext2_esb->total_unalloc_blocks, sb_buf + 12, sizeof(u32));
	kernel_memcpy(&ext2_esb->total_unalloc_inodes, sb_buf + 16, sizeof(u32));
	kernel_memcpy(&blocksize_factor, sb_buf + 24, sizeof(u32));
	kernel_memcpy(&ext2_esb->blocks_per_group, sb_buf + 32, sizeof(u32));
	kernel_memcpy(&ext2_esb->inodes_per_group, sb_buf + 40, sizeof(u32));
	kernel_memcpy(&ext2_esb->inode_size, sb_buf + 88, sizeof(u16));

	blocksize = 1 << (blocksize_factor + 10);
	ext2_esb->sects_per_block = blocksize / BYTES_PER_SECT;
	ext2_esb->total_groups = (ext2_esb->total_blocks - 1) / ext2_esb->blocks_per_group + 1;
	ext2_esb->inode_table_blks = ext2_esb->inodes_per_group * ext2_esb->inode_size / blocksize;
	ext2_esb->descriptor_block = blocksize_factor ? 1 : 2;

	//����λͼ/�����ڵ������ݷ��� (�˴�����ÿ���������λͼ�Ĵ�С����һ��)	
	ext2_esb->bitmap.bg = -1;
	ext2_esb->bitmap.dirty = 0;
	ext2_esb->bitmap.blockbit = (u8*)kmalloc(sizeof(u8) * blocksize);
	ext2_esb->bitmap.inodebit = (u8*)kmalloc(sizeof(u8) * blocksize);
	if (ext2_esb->bitmap.blockbit == 0 || ext2_esb->bitmap.inodebit == 0)
		return -ENOMEM;

	ext2_esb->itable.group = -1;
	ext2_esb->itable.block = -1;
	ext2_esb->itable.dirty = 0;
	ext2_esb->itable.buf = (u8*)kmalloc(sizeof(u8) * blocksize);
	if (ext2_esb->itable.buf == 0)
		return -ENOMEM;
	//�Դˣ���չ�������ʵ���������Ѿ�ȫ�����

	//��������ʼ��file_system_type�ṹ��
	ext2_fs_type = (struct file_system_type*)kmalloc(sizeof(struct file_system_type));
	if (ext2_fs_type == 0)
		return -ENOMEM;
	ext2_fs_type->name = "EXT2";

	//���������ֳ�ʼ�����������
	ext2_sb = (struct super_block*)kmalloc(sizeof(struct super_block));
	if (ext2_sb == 0)
		return -ENOMEM;

	ext2_sb->s_base = base_sect;
	ext2_sb->s_blksize = blocksize;
	ext2_sb->s_dirt = 0;
	ext2_sb->s_root = 0;	//�����������
	ext2_sb->s_type = ext2_fs_type;
	ext2_sb->s_fs_info = (void*)ext2_esb;
	ext2_sb->s_op = &ext2_super_operations;

	//�����������������Ϣ
	err = load_dpttable(ext2_sb);
	if (err)
		return -EDPTTABLE;

	//��������Ӧ�������ڵ�
	err = read_xinode(ext2_sb, 2, &xinode);
	if (err)
		return -ENOINODE;

	ext2_root_inode = (struct inode*)kmalloc(sizeof(struct inode));
	if (ext2_root_inode == 0)
		return -ENOMEM;
	ext2_root_inode->i_ino = 2;
	ext2_root_inode->i_blksize = blocksize;
	ext2_root_inode->i_count = (u32)xinode.data.resolution.count;
	ext2_root_inode->i_size = blocksize;
	ext2_root_inode->i_sb = ext2_sb;
	ext2_root_inode->i_op = &ext2_inode_operations;
	ext2_root_inode->i_fop = &ext2_file_operations;
	INIT_LIST_HEAD(&(ext2_root_inode->i_hash));
	INIT_LIST_HEAD(&(ext2_root_inode->i_LRU));
	INIT_LIST_HEAD(&(ext2_root_inode->i_dentry));	//�Ȼ���ٸ�ֵ

	mapping = ext2_fill_mapping(ext2_root_inode, xinode.data.resolution.pointers);
	if (mapping == 0)
		return -ENOMAPPING;
	ext2_root_inode->i_data = mapping;

	//��������Ӧ��Ŀ¼��
	ext2_root_dentry = (struct dentry*)kmalloc(sizeof(struct dentry));
	if (ext2_root_dentry == 0)
		return -ENOMEM;

	ext2_root_dentry->d_count = 1;
	ext2_root_dentry->d_inode = 0;	//������ٸ�ֵ
	ext2_root_dentry->d_mounted = 0;
	ext2_root_dentry->d_pinned = 1;
	ext2_root_dentry->d_sb = ext2_sb;
	ext2_root_dentry->d_parent = ext2_root_dentry;
	ext2_root_dentry->d_name.len = 5;
	ext2_root_dentry->d_name.name = "ext2/";
	ext2_root_dentry->d_op = &ext2_dentry_operations;
	INIT_LIST_HEAD(&(ext2_root_dentry->d_alias));
	INIT_LIST_HEAD(&(ext2_root_dentry->d_child));
	INIT_LIST_HEAD(&(ext2_root_dentry->d_hash));
	INIT_LIST_HEAD(&(ext2_root_dentry->d_subdirs));

	//dentry����dcache����
	dcache->c_op->add(dcache, (void*)ext2_root_dentry);

	//����inode��dentry�������
	ext2_root_dentry->d_inode = ext2_root_inode;
	list_add(&(ext2_root_dentry->d_alias), &(ext2_root_inode->i_dentry));

	//���Ƴ�����ĳ�ʼ��
	ext2_sb->s_root = ext2_root_dentry;
	//�Դˣ������顢�����ڵ㡢Ŀ¼�ʵ�������

	//ʵ�����ͳ�ʼ��vfsmount�ṹ��
	ext2_root_mount = (struct vfsmount*)kmalloc(sizeof(struct vfsmount));
	if (ext2_root_mount == 0)
		return -ENOMEM;
	ext2_root_mount->mnt_parent = ext2_root_mount;
	ext2_root_mount->mnt_root = ext2_root_dentry;
	ext2_root_mount->mnt_mountpoint = ext2_root_dentry;
	ext2_root_mount->mnt_sb = ext2_sb;
	INIT_LIST_HEAD(&(ext2_root_mount->mnt_hash));
	list_add(&(ext2_root_mount->mnt_hash), &(root_mount->mnt_hash));
	return 0;
}

//���ط������
u32 ext2_readpage(struct vfs_page * page) {
	struct super_block *sb;
	struct ext2_ext_sb *esb;
	u32 base;						//��(����)�����ſ�ʼ
	u32 count;						//Ҫ�����ٸ�����
	u32 err;						//����ֵ

	page->p_state = PAGE_INTACT;
	sb = page->p_mapping->a_host->i_sb;
	esb = (struct ext2_ext_sb*)sb->s_fs_info;
	if (sb == 0 || esb == 0)
		return 1;

	base = sb->s_base + page->p_location * esb->sects_per_block;
	count = esb->sects_per_block;
	err = read_block(page->p_data, base, count);
	return err;
}

//���ط������
u32 ext2_writepage(struct vfs_page* page) {
	struct super_block *sb;
	struct ext2_ext_sb *esb;
	u32 base;						//��(����)�����ſ�ʼ
	u32 count;						//Ҫ�����ٸ�����
	u32 err;						//����ֵ

	if (page->p_data == 0) {
		return 1;	//û�����ݣ�����
	}

	if (page->p_state == PAGE_INTACT)
		return 0;	//����д��
	if (page->p_state == PAGE_INVALID)
		return 1;	//��ҳ������

	page->p_state = PAGE_INTACT;
	sb = page->p_mapping->a_host->i_sb;
	esb = (struct ext2_ext_sb*)sb->s_fs_info;
	if (sb == 0 || esb == 0)
		return 1;

	base = sb->s_base + page->p_location * esb->sects_per_block;
	count = esb->sects_per_block;
	err = write_block(page->p_data, base, count);
	return err;
}

//ʵ��һ���ļ����߼���ŵ������ŵ�ת��
u32 ext2_bmap(struct inode *inode, u32 vpn) {
	union {
		u8  *buf;
		u32 *data;
	} index_page;
	struct super_block		*sb = inode->i_sb;
	struct ext2_ext_sb		*esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 cnt;
	u32 i;
	u32 dp, fp, sp, tp;
	u32 next_pointer;
	u32 base;
	u32 count;
	u32 err;
	u32 ret;

	if (vpn >= inode->i_blocks) {
		//����߼�ҳ�ų��������������ڵ������ţ��򵱳������µ�ҳ
		cnt = vpn - inode->i_blocks + 1;
		if (ext2_create_page(inode, cnt) != cnt)
			return -1;
	}

	err = get_location(sb, vpn, &dp, &fp, &sp, &tp);
	if (err)
		return -1;

	index_page.buf = (u8*)kmalloc(sizeof(u8) * sb->s_blksize);
	if (index_page.buf == 0)
		return -1;

	if (fp == -1) {
		ret = inode->i_data->a_page[dp];
		goto done;
	}

	//��ȡһ������
	next_pointer = inode->i_data->a_page[dp];
	base = sb->s_base + next_pointer * esb->sects_per_block;
	count = esb->sects_per_block;
	err = read_block(index_page.buf, base, count);
	if (err) {
		ret = -1;
		goto done;
	}

	if (sp == -1) {
		ret = index_page.data[fp];
		goto done;
	}

	//��ȡ��������
	next_pointer = index_page.data[fp];
	base = sb->s_base + next_pointer * esb->sects_per_block;
	count = esb->sects_per_block;
	err = read_block(index_page.buf, base, count);
	if (err) {
		ret = -1;
		goto done;
	}

	if (tp == -1) {
		ret = index_page.data[sp];
		goto done;
	}

	//��ȡ��������
	next_pointer = index_page.data[sp];
	base = sb->s_base + next_pointer * esb->sects_per_block;
	count = esb->sects_per_block;
	err = read_block(index_page.buf, base, count);
	if (err) {
		ret = -1;
		goto done;
	}

	ret = index_page.data[tp];
done:
	kfree(index_page.buf);
	return ret;
}

//����һ����Ŀ¼�������ڵ㣬������Ŀ¼�������������������ҳ
//�ú�����Ҫ�޸Ŀ�λͼ�������ڵ��������ȿ�����Ϣ
//���سɹ������ҳ��������������·���ֵ��ص���cnt
u32 ext2_create_page(struct inode* inode, u32 cnt) {
	union {
		u8  *buf;
		u32 *data;
	} index_page;
	struct super_block		*sb = inode->i_sb;
	struct ext2_ext_sb		*esb = (struct ext2_ext_sb*)sb->s_fs_info;
	struct address_space	*mapping = inode->i_data;
	struct ext2_inode		xinode;
	u32 start = inode->i_blocks;	//�����ڵ㵱ǰ������Ҳ�ǵ�һ��δ����Ŀ��
	u32 i;
	u32 base;
	u32 count;
	u32 err;
	u32 dp, fp, sp, tp;
	u32 next_pointer;
	u32 block_no;
	
	u32 group = (inode->i_ino - 1) / esb->inodes_per_group;
	u32 index = (inode->i_ino - 1) % esb->inodes_per_group;
	u32 block = index * esb->inode_size / sb->s_blksize;

	index_page.buf = (u8*)kmalloc(sizeof(u8) * sb->s_blksize);
	if (index_page.buf == 0)
		return 0;

	//׼����EXT2�����ڵ���Ϣ
	err = read_xinode(sb, inode->i_ino, &xinode);
	if (err)
		goto fail;

	for (i = 0; i < cnt; i++) {
		//�ҵ���������������ַ
		err = get_location(sb, start + i, &dp, &fp, &sp, &tp);
		if (err)
			goto fail;

		//������
		err = apply_block(sb, xinode.data.resolution.bg_id, &block_no);
		if (err)
			goto fail;

		//ע��ÿ�
		if (fp == -1) {
			//�޸�xinode��ָ��(���´���)��mapping��Ϣ(�����ڴ�)
			xinode.data.resolution.pointers[dp] = block_no;
			mapping->a_page[dp] = block_no;
			inode->i_blocks++;
			continue;
		}

		//����һ������
		next_pointer = xinode.data.resolution.pointers[dp];
		if (next_pointer == 0) {
			//����Ҫ�ֳ�����һ��������
			err = apply_block(sb, xinode.data.resolution.bg_id, &xinode.data.resolution.pointers[dp]);
			if (err)
				goto fail;

			//����mappping��next_pointer
			mapping->a_page[dp] = xinode.data.resolution.pointers[dp];
			next_pointer = xinode.data.resolution.pointers[dp];
		}

		base = sb->s_base + next_pointer * esb->sects_per_block;
		count = esb->sects_per_block;
		err = read_block(index_page.buf, base, count);
		if (err)
			goto fail;

		if (sp == -1) {
			index_page.data[fp] = block_no;
			inode->i_blocks++;

			//�޸ĺ�����д�ش���
			err = write_block(index_page.buf, base, count);
			if (err)
				goto fail;
			continue;
		}

		//���ʶ�������
		next_pointer = index_page.data[fp];
		if (next_pointer == 0) {
			//����Ҫ�ֳ��������������
			err = apply_block(sb, xinode.data.resolution.bg_id, &index_page.data[fp]);
			if (err)
				goto fail;

			//����mappping��next_pointer
			next_pointer = index_page.data[fp];

			//д�ش���
			err = write_block(index_page.buf, base, count);
		}

		base = sb->s_base + next_pointer * esb->sects_per_block;
		count = esb->sects_per_block;
		err = read_block(index_page.buf, base, count);
		if (err)
			goto fail;

		if (tp == -1) {
			index_page.data[sp] = block_no;
			inode->i_blocks++;

			//�޸ĺ�����д�ش���
			err = write_block(index_page.buf, base, count);
			if (err)
				goto fail;
			continue;
		}

		//������������
		next_pointer = index_page.data[sp];
		if (next_pointer == 0) {
			//����Ҫ�ֳ���������������
			err = apply_block(sb, xinode.data.resolution.bg_id, &index_page.data[sp]);
			if (err)
				goto fail;

			//����mappping��next_pointer
			next_pointer = index_page.data[sp];

			//д�ش���
			err = write_block(index_page.buf, base, count);
		}

		base = sb->s_base + next_pointer * esb->sects_per_block;
		count = esb->sects_per_block;
		err = read_block(index_page.buf, base, count);
		if (err)
			goto fail;

		index_page.data[tp] = block_no;
		inode->i_blocks++;

		//�޸ĺ�����д�ش���
		err = write_block(index_page.buf, base, count);
		if (err)
			goto fail;
	}

	//���ո��������ڵ���е����ݣ���Ҫ����ָ�����ݺ���ռ��������������
	xinode.data.resolution.sectors = inode->i_blocks * esb->sects_per_block;

	err = write_xinode(sb, inode->i_ino, &xinode);
	if (err)
		goto fail;
	kfree(index_page.buf);
	return cnt;

fail:
	kfree(index_page.buf);
	return 0;
}

u32 ext2_readdir(struct file *file, struct getdent *getdent) {
	struct super_block		*sb = file->f_dentry->d_sb;
	struct ext2_ext_sb		*esb = (struct ext2_ext_sb*)sb->s_fs_info;
	struct inode			*inode;
	struct vfs_page			*page;
	struct ext2_direntry	dr;
	struct dirent			*gen_temp;
	u32 j;			//�����еĶ�̬����ƫ��
	u32 vpn;
	u32 ppn;
	u32 capacity;
	u32 size;
	u32 err;

	//���ȼ���ļ������ͣ�����Ŀ¼�ļ�����ִ����ȥ
	if (file->f_flags != LAST_DIR)
		return -ELASTTYPE;

	//����Ŀ¼�ļ��Ŀ������ȼ�������ܳ��ֵ�����Ŀ¼����
	inode = file->f_dentry->d_inode;
	capacity = (sb->s_blksize / (DIRENTRY_META_LEN + 4)) * inode->i_blocks;
	size = 0;
	err = 0;

	//�Ȱ��������������ռ�
	gen_temp = (struct dirent*)kmalloc(sizeof(struct dirent) * capacity);
	if (gen_temp == 0)
		return -ENOMEM;

	//���߼�ҳ���ν��б���
	for (vpn = 0; vpn < inode->i_blocks; vpn++) {
		//�ҵ���Ӧ������ҳ�ţ���������ҳ
		ppn = ext2_bmap(inode, vpn);
		page = general_find_page(ppn, inode);
		if (page == 0) {
			err = -ENOPAGE;
			goto ext2_readdir_end;
		}

		//��ÿһҳ�б���
		j = 0;
		while (j < sb->s_blksize) {
			//�õ�һ��Ŀ¼����ļ������Ԫ��Ϣ
			kernel_memcpy(&dr.meta, page->p_data + j, sizeof(dr.meta));
			if (dr.meta.ino) {
				//��Ŀ¼����Ч����ӽ�gen_temp�ṹ����
				gen_temp[size].ino = dr.meta.ino;
				gen_temp[size].type = (dr.meta.type == DIR_DIRECTORY) ? LAST_DIR : LAST_NORM;
				gen_temp[size].name = (u8*)kmalloc(sizeof(u8) * (dr.meta.name_len + 1));
				if (gen_temp[size].name == 0) {
					err = -ENOMEM;
					goto ext2_readdir_end;
				}
				kernel_memcpy(gen_temp[size].name, page->p_data + j + DIRENTRY_META_LEN, dr.meta.name_len);
				gen_temp[size].name[dr.meta.name_len] = 0;
				size++;
			}
			j += dr.meta.total_len;
		}
	}
	//����ѭ���������������˴�����ʱsize���Ǹ�Ŀ¼�µ������ļ���
	getdent->count = size;
	getdent->dirent = (struct dirent*)kmalloc(sizeof(struct dirent) * size);
	if (getdent->dirent == 0) {
		err = -ENOMEM;
		goto ext2_readdir_end;
	}
	kernel_memcpy(getdent->dirent, gen_temp, sizeof(struct dirent) * size);

	//�����������쳣������Ҫִ����δ��룺
ext2_readdir_end:
	kfree(gen_temp);
	return err;
}

//���ݸ�Ŀ¼�������ڵ㣬Ѱ�Ҳ�����dentry��Ӧ�ļ��������ڵ㣬����dentry���µ������ڵ��������
struct dentry* ext2_lookup(struct inode *inode, struct dentry *dentry, u32 type) {
	struct ext2_direntry		dr;
	struct address_space		*mapping;
	struct inode				*new_inode;
	struct ext2_inode			xinode;
	u32 found;
	u32 block;
	u32 offset;
	u32 err;

	//ȥ��Ŀ¼�������������Ƿ����dentry��Ӧ���ļ�
	found = ext2_lookup_direntry(inode, dentry, &dr, &block, &offset, type);
	if (!found) {
		return (struct dentry*)0;
	}

	//Ϊ�˻�ȡ�������Ϣ��������ȥ�����ڵ���л�ȡ��Ϣ
	err = read_xinode(dentry->d_sb, dr.meta.ino, &xinode);

	//�ҵ��˱���򹹽��µ������ڵ�
	new_inode = (struct inode*)kmalloc(sizeof(struct inode));
	if (new_inode == 0)
		return 0;
	new_inode->i_blksize = dentry->d_sb->s_blksize;
	new_inode->i_count = xinode.data.resolution.count;
	new_inode->i_size = xinode.data.resolution.size;
	new_inode->i_ino = dr.meta.ino;
	new_inode->i_sb = dentry->d_sb;
	new_inode->i_op = &ext2_inode_operations;
	new_inode->i_fop = &ext2_file_operations;
	INIT_LIST_HEAD(&(new_inode->i_dentry));
	INIT_LIST_HEAD(&(new_inode->i_hash));
	INIT_LIST_HEAD(&(new_inode->i_LRU));

	mapping = ext2_fill_mapping(new_inode, xinode.data.resolution.pointers);
	if (mapping == 0)
		return 0;
	new_inode->i_data = mapping;

	//���dentry��new_inode��������
	dentry->d_inode = new_inode;
	list_add(&(dentry->d_alias), &(new_inode->i_dentry));
	return dentry;
}

u32 ext2_createinode(struct dentry *dentry, u32 mode, struct nameidata *nd) {
	//ʹ�ô˺���ʱ������inode��δ����ռ䣬dentry����Ӧinode���Ѿ���ȫ��ֵ
	struct super_block		*sb = dentry->d_sb;
	struct address_space	*mapping;
	struct inode			*inode;
	struct inode			*pinode;	//��Ŀ¼�����ڵ�
	struct vfs_page			*page;
	u32 inode_no;
	u32 type;
	u32 err;

	//�ȳ�ʼ��inode��Ӧ�ĵ�ַ�ռ�
	mapping = (struct address_space*)kmalloc(sizeof(struct address_space));
	if (mapping == 0)
		return -ENOMEM;

	inode = (struct inode*)kmalloc(sizeof(struct inode));
	if (inode == 0)
		return -ENOMEM;

	mapping->a_pagesize = sb->s_blksize;
	mapping->a_host = inode;
	INIT_LIST_HEAD(&(mapping->a_cache));
	mapping->a_op = &ext2_address_space_operations;
	mapping->a_page = (u32*)kmalloc(sizeof(u32) * POINTERS_PER_INODE);
	if (mapping->a_page == 0)
		return -ENOMEM;
	kernel_memset(mapping->a_page, 0, sizeof(u32) * POINTERS_PER_INODE);

	//����һ�������ڵ㣬���³����顢λͼ�������ڵ��
	err = apply_inode(sb, &inode->i_ino, nd->last_type);
	if (err)
		return err;

	//���丸Ŀ¼��ע��Ǽ�
	err = ext2_apply_direntry(nd, dentry, inode->i_ino, nd->last_type);
	if (err)
		return err;

	//���inode
	inode->i_size = (nd->last_type == LAST_DIR) ? sb->s_blksize : 0;
	inode->i_count = (nd->last_type == LAST_DIR) ? 2 : 1;
	inode->i_blocks = 0;
	inode->i_data = mapping;
	inode->i_blksize = sb->s_blksize;
	inode->i_sb = sb;
	inode->i_op = &ext2_inode_operations;
	inode->i_fop = &ext2_file_operations;
	INIT_LIST_HEAD(&(inode->i_hash));
	INIT_LIST_HEAD(&(inode->i_LRU));
	INIT_LIST_HEAD(&(inode->i_dentry));

	list_add(&(dentry->d_alias), &(inode->i_dentry));
	dentry->d_inode = inode;

	//�����µĿ�ҳ
	if (ext2_create_page(inode, 1) != 1)
		return -ENOPAGE;

	page = (struct vfs_page*)kmalloc(sizeof(struct vfs_page));
	if (page == 0)
		return -ENOMEM;
	page->p_data = (u8*)kmalloc(sizeof(u8) * inode->i_blksize);
	if (page->p_data == 0)
		return -ENOMEM;
	kernel_memset(page->p_data, 0, sizeof(u8) * inode->i_blksize);

	page->p_mapping = mapping;
	page->p_location = mapping->a_page[0];
	INIT_LIST_HEAD(&(page->p_list));
	INIT_LIST_HEAD(&(page->p_hash));
	INIT_LIST_HEAD(&(page->p_LRU));

	if (nd->last_type == LAST_DIR) {
		//���Ҫ�½�һ��Ŀ¼��Ҫ�ȶԸ�Ŀ¼�ļ����г�ʼ��
		struct ext2_direntry	dr;
		dr.meta.ino = inode->i_ino;
		dr.meta.name_len = 1;
		dr.meta.total_len = 0xc;
		dr.meta.type = DIR_DIRECTORY;
		kernel_memset(dr.filename, 0, sizeof(u8) * FILENAME_MX_LEN);
		dr.filename[0] = '.';
		kernel_memcpy(page->p_data, &dr, 0xc);

		dr.meta.ino = nd->dentry->d_inode->i_ino;
		dr.meta.name_len = 2;
		dr.meta.total_len = sb->s_blksize - 0xc;
		dr.meta.type = DIR_DIRECTORY;
		dr.filename[1] = '.';
		kernel_memcpy(page->p_data + 0xc, &dr, 0xc);
	}
	page->p_state = PAGE_DIRTY;
	err = ext2_writepage(page);
	if (err)
		return err;

	//��ҳ���������ݽṹ��������
	list_add(&(page->p_list), &(mapping->a_cache));
	pcache->c_op->add(pcache, (void*)page);

	//���¸������ڵ��countֵ
	pinode = nd->dentry->d_inode;
	pinode->i_count++;
	err = ext2_writeinode(pinode, nd->dentry->d_parent);
	return err;
}

//����һ���޸Ĺ��������ڵ㣬���������ڵ����
u32 ext2_writeinode(struct inode *inode, struct dentry *parent) {
	struct super_block	*sb = inode->i_sb;
	struct ext2_inode	xinode;
	u32 err;

	/*
	�ú������ҵ�����У���Ҫ����inode��i_size��i_count�����������ı�ʱ����Ҫ���ļ�ϵͳ����Ӧ�������ݵõ���ʱ��ͬ����
	��FAT32ϵͳ�У�i_size���ڸ�Ŀ¼�У�������Ҫ�Ը�Ŀ¼�����ݽ����޸ġ�
	��EXT2ϵͳ�У�i_size����ȫ�ֵ������ڵ���У��븸Ŀ¼�޹�	������ĵڶ��������������壬��������ֻ��Ϊ�˺�FAT32�ļ�ϵͳͳһ�ӿڶ��ѡ�
	*/

	err = read_xinode(sb, inode->i_ino, &xinode);
	if (err)
		return err;

	xinode.data.resolution.size = inode->i_size;
	xinode.data.resolution.count = inode->i_count;
	err = write_xinode(sb, inode->i_ino, &xinode);
	return err;
}

u32 ext2_deleteinode(struct dentry *dentry, u32 type) {
	struct super_block		*sb = dentry->d_sb;
	struct ext2_ext_sb		*esb = (struct ext2_ext_sb*)sb->s_fs_info;
	struct inode			*inode;
	struct inode			*pinode;
	struct address_space	*mapping;
	struct vfs_page			*page;
	struct ext2_direntry	dr;
	struct list_head		*pos;
	struct list_head		*next_pos;
	u32 block;
	u32 offset;
	u32 vpn;
	u32 ppn;
	u32 found;
	u32 base;
	u32 count;
	u32 err;

	pinode = dentry->d_parent->d_inode;
	inode = dentry->d_inode;
	mapping = inode->i_data;

	found = ext2_lookup_direntry(pinode, dentry, &dr, &block, &offset, type);
	if (!found) {
		//kernel_printf("Cannot find this file!\n");
		return -ENOINODE;
	}

	//�ڸ�Ŀ¼����������ע��
	err = ext2_delete_direntry(pinode, block, offset);
	if (err)
		return err;

	//���¸��ڵ��countֵ��д��
	pinode->i_count--;
	err = ext2_writeinode(pinode, dentry->d_parent->d_parent);
	if (err)
		return err;

	//ɾ���ļ�����
	list_for_each_safe(pos, next_pos, &mapping->a_cache) {
		page = container_of(pos, struct vfs_page, p_list);
		page->p_state = PAGE_INVALID;
		err = pcache->c_op->del(pcache, page);
		if (err)
			return err;
	}

	//ע���ļ�ӵ�е�ÿһ�����ݿ�
	for (vpn = 0; vpn < inode->i_blocks; vpn++) {
		ppn = ext2_bmap(inode, vpn);
		err = retake_block(dentry->d_sb, ppn);
		if (err)
			return err;
	}

	//�������ļ��Ƚϴ󣬻���Ҫ���ǻ��ղ�������������������ݿ�
	if (mapping->a_page[12]) {
		//һ���������
		err = retake_block(sb, mapping->a_page[12]);
		if (err)
			return err;

		//��ոÿ�
		err = clear_block(sb, mapping->a_page[12]);
		if (err)
			return err;
	}
	if (mapping->a_page[13]) {
		union {
			u8  *buf;
			u32 *data;
		} index_page;

		//�����������
		err = retake_block(sb, mapping->a_page[13]);
		if (err)
			return err;
		
		//��ȡ����ҳ
		base = sb->s_base + esb->sects_per_block * mapping->a_page[13];
		count = esb->sects_per_block;
		err = read_block(index_page.buf, base, count);
		if (err)
			return err;

		for (u32 i = 0; i < POINTERS_PER_INODE && index_page.data[i]; i++) {
			err = retake_block(sb, index_page.data[i]);
			if (err)
				return err;

			err = clear_block(sb, index_page.data[i]);
			if (err)
				return err;
		}

		err = clear_block(sb, mapping->a_page[13]);
		if (err)
			return err;
	}
	if (mapping->a_page[14]) {
		union {
			u8  *buf;
			u32 *data;
		} index_page1;
		union {
			u8  *buf;
			u32 *data;
		} index_page2;

		//�����������
		err = retake_block(sb, mapping->a_page[14]);
		if (err)
			return err;

		//��ȡ����ҳ
		base = sb->s_base + esb->sects_per_block * mapping->a_page[14];
		count = esb->sects_per_block;
		err = read_block(index_page1.buf, base, count);
		if (err)
			return err;

		for (u32 i = 0; i < POINTERS_PER_INODE && index_page1.data[i]; i++) {
			err = retake_block(sb, index_page1.data[i]);
			if (err)
				return err;

			//��ȡ����ҳ
			base = sb->s_base + esb->sects_per_block * index_page1.data[i];
			count = esb->sects_per_block;
			err = read_block(index_page2.buf, base, count);
			if (err)
				return err;

			for (u32 j = 0; j < POINTERS_PER_INODE && index_page2.data[j]; j++) {
				err = retake_block(sb, index_page2.data[j]);
				if (err)
					return err;

				err = clear_block(sb, index_page2.data[j]);
				if (err)
					return err;
			}

			err = clear_block(sb, index_page1.data[i]);
			if (err)
				return err;
		}

		err = clear_block(sb, mapping->a_page[14]);
		if (err)
			return err;
	}
	//��λͼ����������ȫ�ֽṹ��ע��
	err = retake_inode(dentry->d_sb, inode->i_ino, type);
	if (err)
		return err;

	//�ͷŵ�ַ�ռ�������ڵ�ṹ��
	generic_delete_alias(inode);
	kfree(mapping->a_page);
	kfree(mapping);
	kfree(inode);
	return 0;
}

u32 ext2_isempty(struct dentry* dentry) {
	//����ext2�ļ�ϵͳ��˵��inode��i_count����������Ŀ¼�ĸ���
	return (dentry->d_inode->i_count == 2);
}

//���ݸ�Ŀ¼��inode�ṹ������dentryĿ¼��
//���صõ��Ƿ���ڡ������ڵ��߼���źͿ����ֽ�ƫ��
u32 ext2_lookup_direntry(struct inode* inode, struct dentry* dentry, struct ext2_direntry *dr, u32* block, u32* offset, u32 type) {
	struct super_block		*sb;
	struct ext2_ext_sb		*esb;
	struct vfs_page			*page;
	struct qstr				filename;
	u32 j;			//�����еĶ�̬����ƫ��
	u32 vpn;
	u32 ppn;
	u32 err;
	u32 found;

	sb = inode->i_sb;
	esb = (struct ext2_ext_sb*)sb->s_fs_info;
	found = 0;
	filename.name = (u8*)kmalloc(sizeof(u8) * FILENAME_MX_LEN);
	if (filename.name == 0)
		return 0;

	//���߼�ҳ���ν��б���
	for (vpn = 0; vpn < inode->i_blocks; vpn++) {
		//�ҵ���Ӧ������ҳ�ţ���������ҳ
		ppn = ext2_bmap(inode, vpn);
		page = general_find_page(ppn, inode);
		if (page == 0)
			break;

		//��ÿһҳ�б���
		j = 0;
		while (j < sb->s_blksize) {
			//�õ�һ��Ŀ¼����ļ������Ԫ��Ϣ
			kernel_memcpy(&dr->meta, page->p_data + j, sizeof(dr->meta));
			kernel_memset(filename.name, 0, sizeof(u8) * FILENAME_MX_LEN);
			filename.len = dr->meta.name_len;
			kernel_memcpy(filename.name, page->p_data + j + DIRENTRY_META_LEN, filename.len);
			
			//����һ��ino�Ų�Ϊ��(�����Ŀ¼��û�б�ɾ)
			if (dr->meta.ino != 0) {
				//���������ļ����Ǻ�
				if (generic_check_filename(&dentry->d_name, &filename) == 0) {
					//���������ļ�����ƥ��
					if ((dr->meta.type == DIR_NORMFILE && type == LAST_NORM) ||
						(dr->meta.type == DIR_DIRECTORY && type == LAST_DIR)) {
						found = 1;
						break;
					}
				}
			}

			//��һ��ǣ����ü���������
			j += dr->meta.total_len;
		}

		if (found)
			break;
		//��һҳ�л�û�У���Ҫ����һҳ
	}

	kfree(filename.name);

	//��ƥ����������߼���źͿ���ƫ�Ƹ�ֵ������
	if (found) {
		*block = vpn;
		*offset = j;
	}
	//ע�ⷵ��ʱdr��û�д��ļ����֣���������ϲ㺯����˵�Ѿ�����Ҫ��
	return found;
}

//�ڸ����ĸ�Ŀ¼nd�£�Ϊָ����dentryע��һ��Ŀ¼��
u32 ext2_apply_direntry(struct nameidata *nd, struct dentry* dentry, u32 ino, u32 type) {
	struct super_block		*sb = nd->dentry->d_sb;
	struct ext2_ext_sb		*esb = (struct ext2_ext_sb*)sb->s_fs_info;
	struct ext2_direntry	dr;
	struct ext2_direntry	prev_dr;
	struct inode			*inode;		//��Ŀ¼�������ڵ�
	struct address_space	*mapping;	//��Ŀ¼�ĵ�ַ�ռ�
	struct vfs_page			*page;		//��Ŀ¼������ҳ
	u32 len_taken;		//�ѱ�ռ�õ��ֽ���
	u32 len_needed1;	//��Ҫ��֤�������ֽ���
	u32 len_needed2;	//������Ŀ¼����Ҫ��֤�������ֽ���
	u32 len_tobe;		//�±����е�total_lenֵ
	u32 err;
	u32 j;
	u32 vpn;
	u32 ppn;
	u32 block;			//���ԷŽ�ȥ��λ�������߼����
	u32 offset;			//���ԷŽ�ȥ��λ�����ڿ���ƫ��
	u32 found;

	//����ø�Ŀ¼��������Ҫ�����ֽ�(4�ı���)
	len_needed1 = ((((dentry->d_name.len - 1) >> 2) + 1) << 2) + DIRENTRY_META_LEN;
	inode = nd->dentry->d_inode;

	found = 0;
	//��inode��ÿ��ҳ���α���
	for (vpn = 0; vpn < inode->i_blocks; vpn++) {
		ppn = ext2_bmap(inode, vpn);
		page = general_find_page(ppn, inode);
		if (page == 0)
			return -ENOMEM;

		j = 0;
		while (j < sb->s_blksize) {
			//�õ�һ��Ŀ¼����ļ������Ԫ��Ϣ
			kernel_memcpy(&prev_dr.meta, page->p_data + j, sizeof(prev_dr.meta));

			//����һ���������
			if (prev_dr.meta.ino == 0 && j == 0) {
				//��ɾ��Ŀ¼��ʱ����һҳ��Ψһ��һ��Ҫɾ��ʱ�����ڲ�����ɿ�ҳ�����
				//���Ա��������Ծɱ�������inoҪ���㣬�������һ��ҳ�Ĵ�С������������ҳ��
				//�������ʱ���������ı����¼������ֱ�Ӱ������ǵ�
				found = 1;
				block = vpn;
				offset = 0;
				len_tobe = sb->s_blksize;

				//����Ҫ����ԭ��¼����ԭ��¼����������ҳ����������ո�ҳ
				kernel_memset(page->p_data, 0, sizeof(u8) * sb->s_blksize);
				break;
			}

			//�����Ŀ¼����Ҫ�����ٳ��Ⱥ�ʵ�ʳ���(����4�ı���)
			len_taken = prev_dr.meta.total_len;
			len_needed2 = ((((prev_dr.meta.name_len - 1) >> 2) + 1) << 2) + DIRENTRY_META_LEN;

			if (len_needed1 + len_needed2 <= len_taken) {
				//�ҵ��������ȥ��λ����
				found = 1;
				block = vpn;
				offset = j + len_needed2;
				len_tobe = len_taken - len_needed2;
				prev_dr.meta.total_len = len_needed2;	//�޸���һ��Ŀ¼����ܳ���
				kernel_memcpy(page->p_data + j, &prev_dr.meta, sizeof(prev_dr.meta));
				break;
			}

			//û���ҵ�����Ҫ�ٿ���һ������
			j += len_taken;
		}

		if (found)
			break;
	}

	if (!found) {
		//�����ִ�ҳ���涼�Ҳ������ʵ�λ�ã�ֻ��������µ�һҳ
		if (ext2_create_page(inode, 1) != 1)
			return -ENOPAGE;

		block = inode->i_blocks - 1;	//(���ݺ��)���һҳ
		offset = 0;						//��ͷ
		len_tobe = sb->s_blksize;
		ppn = ext2_bmap(inode, block);
		page = general_find_page(ppn, inode);
		if (page == 0)
			return -ENOPAGE;
	}
	//�������е�������ҵ���Ӧ��д���λ��

	//�����Ҫд��Ķ�������ֵ
	dr.meta.ino = ino;
	dr.meta.total_len = len_tobe;
	dr.meta.name_len = dentry->d_name.len;
	dr.meta.type = (type == LAST_DIR) ? DIR_DIRECTORY : DIR_NORMFILE;
	kernel_memset(dr.filename, 0, sizeof(u8) * FILENAME_MX_LEN);
	kernel_memcpy(dr.filename, dentry->d_name.name, dentry->d_name.len);

	//��ʽд�벢д��
	kernel_memcpy(page->p_data + offset, &dr, len_needed1);
	page->p_state = PAGE_DIRTY;
	err = ext2_writepage(page);
	return err;
}

//������Ŀ¼�������ڵ㣬�Ӹ�����ź͸���λ�ÿ�ʼ�ı���ɾ��
u32 ext2_delete_direntry(struct inode* inode, u32 block, u32 offset) {
	struct vfs_page			*page;
	struct ext2_direntry	dr;
	u32 len_todelete;		//��ɾ���ı����
	u32 prev_offset;
	u32 next_offset;
	u32 len_needed;
	u32 ppn;
	u32 err;

	ppn = ext2_bmap(inode, block);
	page = general_find_page(ppn, inode);
	if (page == 0)
		return -ENOPAGE;

	kernel_memcpy(&dr.meta, page->p_data + offset, sizeof(u8) * DIRENTRY_META_LEN);
	len_todelete = dr.meta.total_len;

	//���Ҵ�ɾ�����ǰһ���Ϊ�ڳ��Ȳ�����Ҫ�޸ģ�������Ҫ�ų���ɾ�����ǵ�һ������
	if (offset != 0) {
		prev_offset = 0;
		while (1) {
			kernel_memcpy(&dr.meta, page->p_data + prev_offset, sizeof(u8) * DIRENTRY_META_LEN);
			if (dr.meta.total_len + prev_offset == offset)
				break;
			prev_offset += dr.meta.total_len;
		}
		
		//�޸ĳ��ȡ���մ�ɾ����
		dr.meta.total_len += len_todelete;
		kernel_memcpy(page->p_data + prev_offset, &dr.meta, sizeof(u8) * DIRENTRY_META_LEN);
		kernel_memset(page->p_data + offset, 0, sizeof(u8) * len_todelete);
	}
	else {
		//offset == 0����ɾ�����ڵ�һ��
		if (len_todelete == inode->i_sb->s_blksize) {
			//��ʱ��ҳ��ֻ����һ�����Ϊ�˲���ҳ������ֻ����ino�����ó�0
			dr.meta.ino = 0;
			kernel_memcpy(page->p_data, &dr.meta, sizeof(u8) * DIRENTRY_META_LEN);
		}
		else {
			//��ʱ��ҳ�л�����������ҵ���ɾ����������һ������
			next_offset = dr.meta.total_len;
			kernel_memcpy(&dr.meta, page->p_data + next_offset, sizeof(u8) *DIRENTRY_META_LEN);
			len_needed = ((((dr.meta.name_len - 1) >> 2) + 1) << 2) + DIRENTRY_META_LEN;
			kernel_memcpy(&dr.filename, page->p_data + next_offset + DIRENTRY_META_LEN, sizeof(u8) * (len_needed - DIRENTRY_META_LEN));
			
			//�ᵽǰ�������Ǵ�ɾ��¼��������һ��������ܳ���
			kernel_memset(page->p_data, 0, sizeof(u8) * (next_offset + len_needed));
			dr.meta.total_len += next_offset;
			kernel_memcpy(page->p_data, &dr, sizeof(u8) * len_needed);
		}
	}

	//ɾ����ϣ�д�ظ�ҳ����
	page->p_state = PAGE_DIRTY;
	err = ext2_writepage(page);
	return err;
}

//�õ�ĳ�ļ����߼���ţ����������Ӧ������λ��
u32 get_location(struct super_block *sb, u32 vpn, u32 *dp, u32 *fp, u32 *sp, u32 *tp) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 pointers_per_block = sb->s_blksize / sizeof(u32);
	u32 tmp;

	*dp = -1;
	*fp = -1;
	*sp = -1;
	*tp = -1;

	if (vpn < 12) {
		*dp = vpn;
		return 0;
	}

	if (vpn < 12 + pointers_per_block) {
		*dp = 12;
		*fp = vpn - 12;
		return 0;
	}

	if (vpn < 12 + pointers_per_block + pointers_per_block * pointers_per_block) {
		tmp = vpn - 12 - pointers_per_block;
		*dp = 13;
		*fp = tmp / pointers_per_block;
		*sp = tmp % pointers_per_block;
		return 0;
	}

	if (vpn < 12 + pointers_per_block + pointers_per_block * pointers_per_block +
		pointers_per_block * pointers_per_block * pointers_per_block) {
		tmp = vpn - 12 - pointers_per_block - pointers_per_block * pointers_per_block;
		*dp = 14;
		*fp = tmp / (pointers_per_block * pointers_per_block);
		*sp = (tmp % (pointers_per_block * pointers_per_block)) / pointers_per_block;
		*tp = tmp % pointers_per_block;
		return 0;
	}

	//���̫��EXT2�ļ�ϵͳ�Ѿ���֧��
	return 1;
}

//����ĳ�����ڵ��15�����ݿ�����ָ�룬����������ڵ�ӵ�е����ݿ�ȷ����Ŀ
u32 count_blocks(struct super_block *sb, u32 *pointers, u32 *blocks) {
	union {
		u8  *buf;
		u32 *data;
	} index_page;
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 pointers_per_block;
	u32 next_pointer;
	u32 base;
	u32 count;
	u32 tmp;	//����������м����
	u32 err = 0;
	u32 dp;		//ֱ����������0��14
	u32 fp;		//һ����������0�����С/4 - 1
	u32 sp;		//������������0�����С/4 - 1
	u32 tp;		//������������0�����С/4 - 1

				//��������ҳ����Ŀռ�
	pointers_per_block = sb->s_blksize / sizeof(u32);
	index_page.buf = (u8*)kmalloc(sizeof(u8) * sb->s_blksize);
	if (index_page.buf == 0)
		return -ENOMEM;

	for (dp = 0; dp < POINTERS_PER_INODE; dp++) {
		if (pointers[dp] == 0)
			break;
	}

	if (dp <= 12) {
		tmp = dp;
		next_pointer = 0;	//���ٱ�����ȥ��
	}
	else if (dp == 13) {
		tmp = 12;
		next_pointer = pointers[dp - 1];
	}
	else if (dp == 14) {
		tmp = 12 + pointers_per_block;
		next_pointer = pointers[dp - 1];
	}
	else if (dp == 15) {
		tmp = 12 + pointers_per_block + pointers_per_block * pointers_per_block;
		next_pointer = pointers[dp - 1];
	}

	//����һ�����������dp == 13/14/15
	if (next_pointer) {
		//���������ҳ������
		base = sb->s_base + next_pointer * esb->sects_per_block;
		count = esb->sects_per_block;
		err = read_block(index_page.buf, base, count);
		if (err)
			goto fail;

		for (fp = 0; fp < pointers_per_block; fp++) {
			if (index_page.data[fp] == 0)
				break;
		}

		if (dp == 13) {
			tmp += fp;
			next_pointer = 0;	//���ٱ�����ȥ
		}
		else if (dp == 14) {
			tmp += (fp - 1) * pointers_per_block;
			next_pointer = index_page.data[fp - 1];
		}
		else if (dp == 15) {
			tmp += (fp - 1) * pointers_per_block * pointers_per_block;
			next_pointer = index_page.data[fp - 1];
		}
	}

	//���ʶ���������dp == 14/15
	if (next_pointer) {
		//���������ҳ������
		base = sb->s_base + next_pointer * esb->sects_per_block;
		count = esb->sects_per_block;
		err = read_block(index_page.buf, base, count);
		if (err)
			goto fail;

		for (sp = 0; sp < pointers_per_block; sp++) {
			if (index_page.data[sp] == 0)
				break;
		}

		if (dp == 14) {
			tmp += sp;
			next_pointer = 0;	//���ٱ�����ȥ
		}
		else if (dp == 15) {
			tmp += (sp - 1) * pointers_per_block;
			next_pointer = index_page.data[sp - 1];
		}
	}

	//��������������dp == 15
	if (next_pointer) {
		//���������ҳ������
		base = sb->s_base + next_pointer * esb->sects_per_block;
		count = esb->sects_per_block;
		err = read_block(index_page.buf, base, count);
		if (err)
			goto fail;

		for (tp = 0; tp < pointers_per_block; tp++) {
			if (index_page.data[tp] == 0)
				break;
		}

		tmp += tp;
	}

	//������ɸ�ֵ
	*blocks = tmp;
fail:
	kfree(index_page.buf);
	return err;
}

//����ָ�������ڵ�ĵ�ַ�ռ䣬������߼����ݿ�����ָ��
//���ж�a_page��Ҳ�򵥴���ɶ������ݣ��ҽ�������߼�����
//����inode��i_blocks��Ϊ�˸��ϲ㺯���ṩ���񣬱�����׼ȷֵ
struct address_space* ext2_fill_mapping(struct inode* inode, u32 *pointers) {
	struct address_space	*mapping;
	u32 err;

	//�ȼ�����������ڵ�ӵ�����ݿ��ȷ����Ŀ
	err = count_blocks(inode->i_sb, pointers, &inode->i_blocks);
	if (err)
		return 0;

	mapping = (struct address_space*)kmalloc(sizeof(struct address_space));
	if (mapping == 0)
		return 0;

	mapping->a_page = (u32*)kmalloc(sizeof(u32) * POINTERS_PER_INODE);
	if (mapping->a_page == 0)
		return 0;

	kernel_memcpy(mapping->a_page, pointers, sizeof(u32) * POINTERS_PER_INODE);
	mapping->a_pagesize = inode->i_sb->s_blksize;
	mapping->a_host = inode;
	mapping->a_op = &ext2_address_space_operations;
	INIT_LIST_HEAD(&(mapping->a_cache));
	return mapping;
}

//���غõ�λͼ��Ϊ��һ�������������ڶ�Ӧ����ڵ�ƫ����Ϊ�ڶ�������
u32 check_iorb(u8 *bitmap, u32 index) {
	return (bitmap[index >> 3] >> (index & 7)) & 1;
}

//����һ���µ������ڵ㣬��ָ��ķ�ʽ�õ������ڵ��ţ����ط������
u32 apply_inode(struct super_block *sb, u32 *ino, u32 type) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	struct ext2_inode	xinode;
	u32 group;
	u32 byte;
	u32 bit;
	u32 err;
	u8  data;
	if (esb->total_unalloc_inodes == 0)
		return 1;	//�Ѿ�����
	
	//�ҵ���δ�������
	for (group = 0; group < esb->total_groups; group++) {
		if (dpttable[group].data.resolution.unalloc_inodes > 0)
			break;
	}

	//�����������λͼ��Ϣ
	err = load_bitmap(sb, group);
	if (err)
		return err;

	//�ҵ���δ�����Ǹ��ֽ�
	for (byte = 0; byte < sb->s_blksize; byte++) {
		if (esb->bitmap.inodebit[byte] != 0xff) {
			data = esb->bitmap.inodebit[byte];
			break;
		}
	}

	//���ն�λ��һλ��0��
	for (bit = 0; bit < 8; bit++) {
		if (((data >> bit) & 1) == 0)
			break;
	}

	//����ȷ�������ڵ���
	*ino = group * esb->inodes_per_group + byte * 8 + bit + 1;

	//�޸ĳ��������ݡ����������ݺ�λͼ����
	esb->total_unalloc_inodes--;
	dpttable[group].data.resolution.unalloc_inodes--;
	esb->bitmap.inodebit[byte] |= (1 << bit);
	esb->bitmap.dirty = 1;
	if (type == LAST_DIR)
		dpttable[group].data.resolution.dirs++;

	//���������ڵ��
	kernel_memset(xinode.data.buf, 0, sizeof(u8) * INODE_SIZE_VALID);
	if (type == LAST_DIR) {
		xinode.data.resolution.type_permission = INODE_DIRECTORY | 0x1ed;
	}
	else {
		xinode.data.resolution.type_permission = INODE_NORMFILE | 0x1a4;
	}
	xinode.data.resolution.bg_id = group;
	xinode.data.resolution.a_time = 0x5c248d99;
	xinode.data.resolution.c_time = 0x5c248d99;
	xinode.data.resolution.m_time = 0x5c248d99;
	if (type == LAST_DIR)
		xinode.data.resolution.size = sb->s_blksize;
	else
		xinode.data.resolution.size = 0;
	xinode.data.resolution.count = (type == LAST_DIR) ? 2 : 1;
	//xinode.data.resolution.size = (type == LAST_DIR) ? sb->s_blksize : 0;
	err = write_xinode(sb, *ino, &xinode);
	if (err)
		return err;

	//����д�ش��̣�����ͬ��
	err = save_bitmap(sb);
	if (err)
		return err;
	err = save_dpttable(sb, group);
	if (err)
		return err;
	err = save_sb(sb);
	if (err)
		return err;
	return 0;
}

//��ָ�����������һ���µĿ飬��ָ��ķ�ʽ�õ���������ţ����ط������
u32 apply_block(struct super_block *sb, u32 group, u32 *block) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 byte;
	u32 bit;
	u32 err;
	u8  data;

	if (esb->total_unalloc_blocks == 0 || dpttable->data.resolution.unalloc_blocks == 0)
		return 1;	//�������ˣ����벻��

	//��λ�ҵ���һ����0��
	err = load_bitmap(sb, group);
	if (err)
		return err;

	for (byte = 0; byte < sb->s_blksize; byte++) {
		if (esb->bitmap.blockbit[byte] != 0xff) {
			data = esb->bitmap.blockbit[byte];
			break;
		}
	}

	for (bit = 0; bit < 8; bit++) {
		if (((data >> bit) & 1) == 0)
			break;
	}
	
	//�޸�Ԫ�����Լ�д��
	*block = group * esb->blocks_per_group + byte * 8 + bit;
	esb->bitmap.dirty = 1;
	esb->bitmap.blockbit[byte] |= (1 << bit);
	dpttable[group].data.resolution.unalloc_blocks--;
	esb->total_unalloc_blocks--;

	err = save_bitmap(sb);
	if (err)
		return err;
	err = save_dpttable(sb, group);
	if (err)
		return err;
	err = save_sb(sb);
	if (err)
		return err;
	return 0;
}

//����ָ����ŵ������ڵ㣬���ط������
u32 retake_inode(struct super_block* sb, u32 ino, u32 type) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	struct ext2_inode	xinode;
	u32 group;		//�������ڵ���������
	u32 index;		//��������ڲ��ĵڼ���
	u32 valid;
	u32 err;
	
	//��λ
	group = (ino - 1) / esb->inodes_per_group;
	index = (ino - 1) % esb->inodes_per_group;

	err = load_bitmap(sb, group);
	if (err)
		return err;

	valid = check_iorb(esb->bitmap.inodebit, index);
	if (!valid)
		return 1;	//��������Ч�������ٻ���

	//��������ڵ����
	kernel_memset(&xinode, 0, sizeof(xinode));
	err = write_xinode(sb, ino, &xinode);
	if (err)
		return err;

	//ִ�л��գ������޸�
	esb->bitmap.dirty = 1;
	esb->bitmap.inodebit[index >> 3] &= ~(1 << (index & 7));
	dpttable[group].data.resolution.unalloc_inodes++;
	esb->total_unalloc_inodes++;
	if (type == LAST_DIR)
		dpttable[group].data.resolution.dirs--;

	err = save_bitmap(sb);
	if (err)
		return err;
	err = save_dpttable(sb, group);
	if (err)
		return err;
	err = save_sb(sb);
	if (err)
		return err;
	return 0;
}

//����ָ����������ŵĿ飬���ط������
u32 retake_block(struct super_block *sb, u32 block) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 group;		//�ڵڼ������
	u32 index;		//��������ڲ��ĵڼ���
	u32 valid;
	u32 err;
	
	//��λ
	group = block / esb->blocks_per_group;
	index = block % esb->blocks_per_group;
	err = load_bitmap(sb, group);
	if (err)
		return err;

	valid = check_iorb(esb->bitmap.blockbit, index);
	if (!valid)
		return 1;	//��������Ч�������ٻ���

	//ִ�л��գ������޸�
	esb->bitmap.dirty = 1;
	esb->bitmap.blockbit[index >> 3] &= ~(1 << (index & 7));
	dpttable[group].data.resolution.unalloc_blocks++;
	esb->total_unalloc_blocks++;

	err = save_bitmap(sb);
	if (err)
		return err;
	err = save_dpttable(sb, group);
	if (err)
		return err;
	err = save_sb(sb);
	if (err)
		return err;	
	return 0;
}

//���һ�����ݣ��ú�����Ҫ��Բ����ļ����ݿ����Щ��
//����͵ı����ļ��ļ�������Ŀ�
u32 clear_block(struct super_block *sb, u32 block) {
	struct ext2_ext_sb	*esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 base;
	u32 count;
	u32 err;
	u8  *buf;

	//Ϊ����������ռ�
	buf = (u8*)kmalloc(sizeof(u8) * sb->s_blksize);
	if (buf == 0)
		return -ENOMEM;
	kernel_memset(buf, 0, sb->s_blksize);
	
	//д�ش���
	base = sb->s_base + block * esb->sects_per_block;
	count = esb->sects_per_block;
	err = write_block(buf, base, count);
	return err;
}

//�����޸Ĺ���ĳ�������Ϣ����Ҫ���������ʾδ����������
u32 save_sb(struct super_block *sb) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 group;
	u32 base;
	u32 err;
	u32 pow3, pow5, pow7;	//3��5��7����

	//�ӽṹ�嵽������
	kernel_memcpy(sb_buf + 12, &esb->total_unalloc_blocks, sizeof(u32));
	kernel_memcpy(sb_buf + 16, &esb->total_unalloc_inodes, sizeof(u32));

	//�ӻ����������̣�ע���ж�����ͬʱ���ݳ�����
	if (write_block(sb_buf, sb->s_base + 2, 1))
		return 1;

	//����������飬�ҵ��������鱸�ݵ�����
	pow3 = 1;
	pow5 = 5;
	pow7 = 7;
	for (group = 1; group < esb->total_groups; group++) {
		if (group == pow3) {
			pow3 *= 3;
		}
		else if (group == pow5) {
			pow5 *= 5;
		}
		else if (group == pow7) {
			pow7 *= 7;
		}
		else
			continue;

		//ֻ�к����ݵ��������������ˣ�ִ��д��
		base = sb->s_base + group * esb->blocks_per_group * esb->sects_per_block;
		if (write_block(sb_buf, base, 1))
			return 1;
	}
	return 0;
}

//����ȫ�ֵ�������������Ϣ����Ȼ���ļ�ϵͳ���������п��ܻ����޸�
//���Ƕ�ȡ������������Ϣֻ�ڳ�ʼ��ʱ����һ�Σ���νһ������
u32 load_dpttable(struct super_block *sb) {
	struct ext2_ext_sb	*esb;
	u32 blocks;		//����������������ܿ���
	u32 basesect;	//��ȡ����ʼ����
	u32 i;			//�����ÿ��
	u32 entries;	//��������¼��
	u32 err;
	u8  *buf;
	u8  *p;

	esb = (struct ext2_ext_sb*)sb->s_fs_info;
	blocks = (esb->total_groups - 1) * DESCRIPTOR_ENTRY_LEN / sb->s_blksize + 1;
	
	//������������ռ�
	buf = (u8*)kmalloc(sizeof(u8) * sb->s_blksize);
	if (buf == 0)
		return -ENOMEM;

	//����������������ռ�
	dpttable = (struct ext2_dptentry*)kmalloc(sb->s_blksize * blocks);
	if (dpttable == 0)
		return -ENOMEM;
	p = (u8*)dpttable;

	for (i = 0; i < blocks; i++) {
		basesect = sb->s_base + (esb->descriptor_block + i) * esb->sects_per_block;
		//��ȡ�ÿ��ڵ�����
		err = read_block(buf, basesect, esb->sects_per_block);
		if (err)
			return err;

		if (i < blocks - 1)
			entries = sb->s_blksize / DESCRIPTOR_ENTRY_LEN;
		else
			entries = esb->total_groups % (sb->s_blksize / DESCRIPTOR_ENTRY_LEN);

		//�������͵�������������
		kernel_memcpy(p, buf, entries * DESCRIPTOR_ENTRY_LEN);
		p += entries * DESCRIPTOR_ENTRY_LEN;
	}
	
	//for (i = 0; i < 3; i++) {
	//	kernel_printf("%d %d %d %d %d %d\n", 
	//		dpttable[i].data.resolution.block_bitmap_base,
	//		dpttable[i].data.resolution.inode_bitmap_base,
	//		dpttable[i].data.resolution.inode_table_base,
	//		dpttable[i].data.resolution.unalloc_blocks,
	//		dpttable[i].data.resolution.unalloc_inodes,
	//		dpttable[i].data.resolution.dirs);
	//}

	kfree(buf);
	return 0;
}

//�����޸Ĺ����������������Ϣ���ڶ�������Ϊ���޸ĵ�����
u32 save_dpttable(struct super_block *sb, u32 group) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 block;		//��������������Ϣ���ڵĿ��ƫ��
	u32 basesect;	//Ҫд����ʼ������
	u32 len;
	u32 err;
	u8  *buf;	//������
	u8  *p;

	//���벢��ʼ���ÿ黺����
	buf = (u8*)kmalloc(sizeof(u8) * sb->s_blksize);
	if (buf == 0)
		return 1;
	kernel_memset(buf, 0, sizeof(u8) * sb->s_blksize);

	//������ʼ��ַ�ͳ���
	block = group * DESCRIPTOR_ENTRY_LEN / sb->s_blksize;
	p = (u8*)&dpttable[block * sb->s_blksize / DESCRIPTOR_ENTRY_LEN];
	if ((esb->total_groups - 1) * DESCRIPTOR_ENTRY_LEN / sb->s_blksize == block)
		len = ((esb->total_groups - 1) % (sb->s_blksize / DESCRIPTOR_ENTRY_LEN)) * DESCRIPTOR_ENTRY_LEN;
	else
		len = sb->s_blksize;
	
	//�ӽṹ�嵽������
	kernel_memcpy(buf, p, len);

	//�ӻ�����������
	basesect = sb->s_base + (esb->descriptor_block + block) * esb->sects_per_block;
	err = write_block(buf, basesect, esb->sects_per_block);
	kfree(buf);
	return err;
}

//����ָ������λͼ���ݣ�������λͼ�������ڵ�λͼ
u32 load_bitmap(struct super_block *sb, u32 group) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 bbitmap_block;
	u32 ibitmap_block;
	u32 basesect;		//��ȡ����ʼ������
	u32 err;

	if (esb->bitmap.bg == group)
		return 0;

	//�ȱ���ԭ����λͼ��Ϣ
	if (esb->bitmap.bg != group && esb->bitmap.dirty == 1) {
		err = save_bitmap(sb);
	}

	//�����λͼ�������ڵ�λͼ�Ŀ��
	bbitmap_block = dpttable[group].data.resolution.block_bitmap_base;
	ibitmap_block = dpttable[group].data.resolution.inode_bitmap_base;

	//��ȡ����
	basesect = sb->s_base + bbitmap_block * esb->sects_per_block;
	err = read_block(esb->bitmap.blockbit, basesect, esb->sects_per_block);
	if (err)
		return err;

	basesect = sb->s_base + ibitmap_block * esb->sects_per_block;
	err = read_block(esb->bitmap.inodebit, basesect, esb->sects_per_block);
	if (err)
		return err;

	esb->bitmap.bg = group;
	esb->bitmap.dirty = 0;
	return 0;
}

//д�ص�ǰ�������ڵ�λͼ���ݣ�������λͼ�������ڵ�λͼ
u32 save_bitmap(struct super_block *sb) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 bbitmap_block;
	u32 ibitmap_block;
	u32 basesect;		//д�ص���ʼ������
	u32 err;
	u32 group = esb->bitmap.bg;

	if (!esb->bitmap.dirty)
		return 0;	//���������ľͲ�д��

	//�����λͼ�������ڵ�λͼ�Ŀ��
	bbitmap_block = dpttable[group].data.resolution.block_bitmap_base;
	ibitmap_block = dpttable[group].data.resolution.inode_bitmap_base;

	//��ȡ����
	basesect = sb->s_base + bbitmap_block * esb->sects_per_block;
	err = write_block(esb->bitmap.blockbit, basesect, esb->sects_per_block);
	if (err)
		return err;

	basesect = sb->s_base + ibitmap_block * esb->sects_per_block;
	err = write_block(esb->bitmap.inodebit, basesect, esb->sects_per_block);
	if (err)
		return err;

	esb->bitmap.dirty = 0;
	return 0;
}

//����ָ������е�ָ�������ڵ���
u32 load_itable(struct super_block *sb, u32 group, u32 block) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 itable_base;	//����������ڵ����׿��
	u32 basesect;		//��ȡ����������
	u32 err;

	if (esb->itable.group == group && esb->itable.block == block)
		return 0;

	//�б�Ҫʱ����д��ԭ���������ڵ���
	if ((esb->itable.group != group || esb->itable.block != block) && esb->itable.dirty) {
		err = save_itable(sb);
		if (err)
			return err;
	}

	//�����ַ������Ӧ�Ŀ����
	itable_base = dpttable[group].data.resolution.inode_table_base;
	basesect = sb->s_base + (itable_base + block) * esb->sects_per_block;
	err = read_block(esb->itable.buf, basesect, esb->sects_per_block);
	if (err)
		return err;

	esb->itable.group = group;
	esb->itable.block = block;
	esb->itable.dirty = 0;
	return 0;
}

//д�ص�ǰ�������ڵ�ָ�������ڵ���
u32 save_itable(struct super_block *sb) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 itable_base;	//����������ڵ����׿��
	u32 basesect;		//д�ص���������
	u32 group = esb->itable.group;
	u32 block = esb->itable.block;
	u32 err;

	//�����ַ������Ӧ�Ŀ����
	itable_base = dpttable[group].data.resolution.inode_table_base;
	basesect = sb->s_base + (itable_base + block) * esb->sects_per_block;
	err = write_block(esb->itable.buf, basesect, esb->sects_per_block);
	if (err)
		return err;

	esb->itable.dirty = 0;
	return 0;
}

//���������ڵ��Ż�ö�Ӧ�������ڵ�ṹ
//���ط������
u32 read_xinode(struct super_block *sb, u32 ino, struct ext2_inode *xinode) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 group;		//����
	u32 index;		//����ڵڼ���
	u32 block;		//���ڿ����Կ��
	u32 offset;		//λ�����ڿ�ĵڼ���
	u32 valid;		//��Ӧ�������ڵ��Ƿ���Ч
	u32 err;
	
	//��ø�����Ҫ���м���
	group = (ino - 1) / esb->inodes_per_group;
	index = (ino - 1) % esb->inodes_per_group;
	block = index * esb->inode_size / sb->s_blksize;
	offset = index % (sb->s_blksize / esb->inode_size);

	//����λͼ�����������ڵ��Ƿ���Ч
	err = load_bitmap(sb, group);
	if (err)
		return err;
	valid = check_iorb(esb->bitmap.inodebit, index);
	if (!valid)
		return -ENOINODE;

	//��ȡ�����ڵ��
	err = load_itable(sb, group, block);
	if (err)
		return err;

	kernel_memcpy(xinode, esb->itable.buf + offset * esb->inode_size, INODE_SIZE_VALID);
	return 0;
}

u32 write_xinode(struct super_block *sb, u32 ino, struct ext2_inode *xinode) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 group;		//����
	u32 index;		//����ڵڼ���
	u32 block;		//���ڿ����Կ��
	u32 offset;		//λ�����ڿ�ĵڼ���
	u32 valid;		//��Ӧ�������ڵ��Ƿ���Ч
	u32 err;

	//��ø�����Ҫ���м���
	group = (ino - 1) / esb->inodes_per_group;
	index = (ino - 1) % esb->inodes_per_group;
	block = index * esb->inode_size / sb->s_blksize;
	offset = index % (sb->s_blksize / esb->inode_size);

	//����λͼ�����������ڵ��Ƿ���Ч
	err = load_bitmap(sb, group);
	if (err)
		return err;
	valid = check_iorb(esb->bitmap.inodebit, index);
	if (!valid)
		return -ENOINODE;

	//д�������ڵ��
	err = load_itable(sb, group, block);
	if (err)
		return err;
	kernel_memcpy(esb->itable.buf + offset * esb->inode_size, xinode, INODE_SIZE_VALID);
	err = save_itable(sb);
	return err;
}
