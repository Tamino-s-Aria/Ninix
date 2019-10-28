#include <zjunix/ext2.h>
#include <zjunix/vfscache.h>
#include <zjunix/slab.h>
#include <zjunix/utils.h>
#include <driver/sd.h>

//extern vfs.c
extern struct dentry* 		root_dentry;		//全局的根目录项
extern struct dentry* 		pwd_dentry;			//当前所在目录项
extern struct vfsmount* 	root_mount;			//全局的根挂载信息
extern struct vfsmount* 	pwd_mount;			//当前所在挂载信息

//extern vfscache.c
extern struct cache			*dcache;			//dentry缓存
extern struct cache			*pcache;			//page缓存

//全局变量
struct ext2_dptentry		*dpttable;				//组块描述符数组
u8							sb_buf[BYTES_PER_SECT];	//超级块内容

//实例化抽象数据结构，让函数指针指向EXT2系统对应的具体函数
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

//从指定(绝对)扇区开始读入信息，以初始化EXT2系统
u32 ext2_init(u32 base_sect) {
	struct super_block		*ext2_sb;				//通用超级块
	struct ext2_ext_sb		*ext2_esb;				//超级块针对该文件系统的扩展信息
	struct file_system_type *ext2_fs_type;			//该文件系统类型
	struct dentry           *ext2_root_dentry;		//该文件系统的根目录项
	struct inode            *ext2_root_inode;		//该文件系统根的索引节点对象
	struct ext2_inode		xinode;					//该文件系统根在磁盘上的索引节点对象
	struct address_space	*mapping;				//该文件系统根索引节点的地址空间
	struct vfsmount			*ext2_root_mount;		//该文件系统根的挂载信息
	u32						blocksize_factor;
	u32						blocksize;
	u32						bitmapsize;
	u32						err;

	//读取超级块内容，超级块在文件系统基地址的1024字节(2个扇区)之后开始
	if (read_block(sb_buf, base_sect + 2, 1))
		return -EIO;

	//实例化扩展超级块
	ext2_esb = (struct ext2_ext_sb*)kmalloc(sizeof(struct ext2_ext_sb));
	if (ext2_esb == 0)
		return -ENOMEM;

	//进一步初始化扩展超级块
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

	//完善位图/索引节点表的数据分配 (此处假设每个组块两个位图的大小都是一块)	
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
	//自此，扩展超级块的实例化工作已经全部完成

	//创建并初始化file_system_type结构体
	ext2_fs_type = (struct file_system_type*)kmalloc(sizeof(struct file_system_type));
	if (ext2_fs_type == 0)
		return -ENOMEM;
	ext2_fs_type->name = "EXT2";

	//创建并部分初始化超级块对象
	ext2_sb = (struct super_block*)kmalloc(sizeof(struct super_block));
	if (ext2_sb == 0)
		return -ENOMEM;

	ext2_sb->s_base = base_sect;
	ext2_sb->s_blksize = blocksize;
	ext2_sb->s_dirt = 0;
	ext2_sb->s_root = 0;	//待会儿再设置
	ext2_sb->s_type = ext2_fs_type;
	ext2_sb->s_fs_info = (void*)ext2_esb;
	ext2_sb->s_op = &ext2_super_operations;

	//加载描述符数组的信息
	err = load_dpttable(ext2_sb);
	if (err)
		return -EDPTTABLE;

	//创建根对应的索引节点
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
	INIT_LIST_HEAD(&(ext2_root_inode->i_dentry));	//等会儿再赋值

	mapping = ext2_fill_mapping(ext2_root_inode, xinode.data.resolution.pointers);
	if (mapping == 0)
		return -ENOMAPPING;
	ext2_root_inode->i_data = mapping;

	//创建根对应的目录项
	ext2_root_dentry = (struct dentry*)kmalloc(sizeof(struct dentry));
	if (ext2_root_dentry == 0)
		return -ENOMEM;

	ext2_root_dentry->d_count = 1;
	ext2_root_dentry->d_inode = 0;	//待会儿再赋值
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

	//dentry加入dcache当中
	dcache->c_op->add(dcache, (void*)ext2_root_dentry);

	//根的inode和dentry互相关联
	ext2_root_dentry->d_inode = ext2_root_inode;
	list_add(&(ext2_root_dentry->d_alias), &(ext2_root_inode->i_dentry));

	//完善超级块的初始化
	ext2_sb->s_root = ext2_root_dentry;
	//自此，超级块、索引节点、目录项都实例化完毕

	//实例化和初始化vfsmount结构体
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

//返回非零错误
u32 ext2_readpage(struct vfs_page * page) {
	struct super_block *sb;
	struct ext2_ext_sb *esb;
	u32 base;						//从(绝对)扇区号开始
	u32 count;						//要读多少个扇区
	u32 err;						//返回值

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

//返回非零错误
u32 ext2_writepage(struct vfs_page* page) {
	struct super_block *sb;
	struct ext2_ext_sb *esb;
	u32 base;						//从(绝对)扇区号开始
	u32 count;						//要读多少个扇区
	u32 err;						//返回值

	if (page->p_data == 0) {
		return 1;	//没有数据，错误
	}

	if (page->p_state == PAGE_INTACT)
		return 0;	//不需写回
	if (page->p_state == PAGE_INVALID)
		return 1;	//空页，错误

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

//实现一个文件内逻辑块号到物理块号的转换
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
		//如果逻辑页号超过了现在索引节点的最大编号，则当场申请新的页
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

	//读取一级索引
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

	//读取二级索引
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

	//读取三级索引
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

//给定一个子目录的索引节点，给该子目录扩充给定数量的新数据页
//该函数需要修改块位图、索引节点表、超级块等控制信息
//返回成功扩充的页数量，正常情况下返回值务必等于cnt
u32 ext2_create_page(struct inode* inode, u32 cnt) {
	union {
		u8  *buf;
		u32 *data;
	} index_page;
	struct super_block		*sb = inode->i_sb;
	struct ext2_ext_sb		*esb = (struct ext2_ext_sb*)sb->s_fs_info;
	struct address_space	*mapping = inode->i_data;
	struct ext2_inode		xinode;
	u32 start = inode->i_blocks;	//索引节点当前块数，也是第一个未分配的块号
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

	//准备好EXT2索引节点信息
	err = read_xinode(sb, inode->i_ino, &xinode);
	if (err)
		goto fail;

	for (i = 0; i < cnt; i++) {
		//找到待申请块的索引地址
		err = get_location(sb, start + i, &dp, &fp, &sp, &tp);
		if (err)
			goto fail;

		//申请块号
		err = apply_block(sb, xinode.data.resolution.bg_id, &block_no);
		if (err)
			goto fail;

		//注册该块
		if (fp == -1) {
			//修改xinode的指针(更新磁盘)和mapping信息(更新内存)
			xinode.data.resolution.pointers[dp] = block_no;
			mapping->a_page[dp] = block_no;
			inode->i_blocks++;
			continue;
		}

		//访问一级索引
		next_pointer = xinode.data.resolution.pointers[dp];
		if (next_pointer == 0) {
			//还需要现场申请一级索引块
			err = apply_block(sb, xinode.data.resolution.bg_id, &xinode.data.resolution.pointers[dp]);
			if (err)
				goto fail;

			//更新mappping和next_pointer
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

			//修改后立刻写回磁盘
			err = write_block(index_page.buf, base, count);
			if (err)
				goto fail;
			continue;
		}

		//访问二级索引
		next_pointer = index_page.data[fp];
		if (next_pointer == 0) {
			//还需要现场申请二级索引块
			err = apply_block(sb, xinode.data.resolution.bg_id, &index_page.data[fp]);
			if (err)
				goto fail;

			//更新mappping和next_pointer
			next_pointer = index_page.data[fp];

			//写回磁盘
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

			//修改后立刻写回磁盘
			err = write_block(index_page.buf, base, count);
			if (err)
				goto fail;
			continue;
		}

		//访问三级索引
		next_pointer = index_page.data[sp];
		if (next_pointer == 0) {
			//还需要现场申请三级索引块
			err = apply_block(sb, xinode.data.resolution.bg_id, &index_page.data[sp]);
			if (err)
				goto fail;

			//更新mappping和next_pointer
			next_pointer = index_page.data[sp];

			//写回磁盘
			err = write_block(index_page.buf, base, count);
		}

		base = sb->s_base + next_pointer * esb->sects_per_block;
		count = esb->sects_per_block;
		err = read_block(index_page.buf, base, count);
		if (err)
			goto fail;

		index_page.data[tp] = block_no;
		inode->i_blocks++;

		//修改后立刻写回磁盘
		err = write_block(index_page.buf, base, count);
		if (err)
			goto fail;
	}

	//最终更新索引节点表中的内容，主要更新指针内容和所占扇区数这两个域
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
	u32 j;			//运行中的动态块内偏移
	u32 vpn;
	u32 ppn;
	u32 capacity;
	u32 size;
	u32 err;

	//首先检查文件的类型，不是目录文件不得执行下去
	if (file->f_flags != LAST_DIR)
		return -ELASTTYPE;

	//根据目录文件的块数，先计算出可能出现的最多的目录项数
	inode = file->f_dentry->d_inode;
	capacity = (sb->s_blksize / (DIRENTRY_META_LEN + 4)) * inode->i_blocks;
	size = 0;
	err = 0;

	//先按照最大容量分配空间
	gen_temp = (struct dirent*)kmalloc(sizeof(struct dirent) * capacity);
	if (gen_temp == 0)
		return -ENOMEM;

	//对逻辑页依次进行遍历
	for (vpn = 0; vpn < inode->i_blocks; vpn++) {
		//找到对应的物理页号，并找来此页
		ppn = ext2_bmap(inode, vpn);
		page = general_find_page(ppn, inode);
		if (page == 0) {
			err = -ENOPAGE;
			goto ext2_readdir_end;
		}

		//在每一页中遍历
		j = 0;
		while (j < sb->s_blksize) {
			//拿到一个目录项除文件名外的元信息
			kernel_memcpy(&dr.meta, page->p_data + j, sizeof(dr.meta));
			if (dr.meta.ino) {
				//该目录项有效，添加进gen_temp结构体中
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
	//正常循环结束程序跳至此处，此时size就是该目录下的所有文件数
	getdent->count = size;
	getdent->dirent = (struct dirent*)kmalloc(sizeof(struct dirent) * size);
	if (getdent->dirent == 0) {
		err = -ENOMEM;
		goto ext2_readdir_end;
	}
	kernel_memcpy(getdent->dirent, gen_temp, sizeof(struct dirent) * size);

	//正常结束和异常结束都要执行这段代码：
ext2_readdir_end:
	kfree(gen_temp);
	return err;
}

//根据父目录的索引节点，寻找并构建dentry对应文件的索引节点，并将dentry和新的索引节点关联起来
struct dentry* ext2_lookup(struct inode *inode, struct dentry *dentry, u32 type) {
	struct ext2_direntry		dr;
	struct address_space		*mapping;
	struct inode				*new_inode;
	struct ext2_inode			xinode;
	u32 found;
	u32 block;
	u32 offset;
	u32 err;

	//去父目录的数据区查找是否存在dentry对应的文件
	found = ext2_lookup_direntry(inode, dentry, &dr, &block, &offset, type);
	if (!found) {
		return (struct dentry*)0;
	}

	//为了获取更多的信息，这里再去索引节点表中获取信息
	err = read_xinode(dentry->d_sb, dr.meta.ino, &xinode);

	//找到了表项，则构建新的索引节点
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

	//最后将dentry和new_inode关联起来
	dentry->d_inode = new_inode;
	list_add(&(dentry->d_alias), &(new_inode->i_dentry));
	return dentry;
}

u32 ext2_createinode(struct dentry *dentry, u32 mode, struct nameidata *nd) {
	//使用此函数时，假设inode还未分配空间，dentry除对应inode外已经完全赋值
	struct super_block		*sb = dentry->d_sb;
	struct address_space	*mapping;
	struct inode			*inode;
	struct inode			*pinode;	//父目录索引节点
	struct vfs_page			*page;
	u32 inode_no;
	u32 type;
	u32 err;

	//先初始化inode对应的地址空间
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

	//申请一个索引节点，更新超级块、位图、索引节点表
	err = apply_inode(sb, &inode->i_ino, nd->last_type);
	if (err)
		return err;

	//在其父目录处注册登记
	err = ext2_apply_direntry(nd, dentry, inode->i_ino, nd->last_type);
	if (err)
		return err;

	//填充inode
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

	//创造新的空页
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
		//如果要新建一个目录，要先对该目录文件进行初始化
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

	//把页和其他数据结构关联起来
	list_add(&(page->p_list), &(mapping->a_cache));
	pcache->c_op->add(pcache, (void*)page);

	//更新父索引节点的count值
	pinode = nd->dentry->d_inode;
	pinode->i_count++;
	err = ext2_writeinode(pinode, nd->dentry->d_parent);
	return err;
}

//给定一个修改过的索引节点，更新索引节点表项
u32 ext2_writeinode(struct inode *inode, struct dentry *parent) {
	struct super_block	*sb = inode->i_sb;
	struct ext2_inode	xinode;
	u32 err;

	/*
	该函数在我的设计中，主要用在inode的i_size和i_count域主动发生改变时，需要在文件系统的相应控制数据得到及时的同步。
	在FAT32系统中，i_size存在父目录中，所以需要对父目录的数据进行修改。
	在EXT2系统中，i_size存在全局的索引节点表中，与父目录无关	以这里的第二个参数并无意义，这样声明只是为了和FAT32文件系统统一接口而已。
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

	//在父目录的数据里面注销
	err = ext2_delete_direntry(pinode, block, offset);
	if (err)
		return err;

	//更新父节点的count值并写回
	pinode->i_count--;
	err = ext2_writeinode(pinode, dentry->d_parent->d_parent);
	if (err)
		return err;

	//删除文件数据
	list_for_each_safe(pos, next_pos, &mapping->a_cache) {
		page = container_of(pos, struct vfs_page, p_list);
		page->p_state = PAGE_INVALID;
		err = pcache->c_op->del(pcache, page);
		if (err)
			return err;
	}

	//注销文件拥有的每一个数据块
	for (vpn = 0; vpn < inode->i_blocks; vpn++) {
		ppn = ext2_bmap(inode, vpn);
		err = retake_block(dentry->d_sb, ppn);
		if (err)
			return err;
	}

	//如果这个文件比较大，还需要考虑回收并清空其间接索引所在数据块
	if (mapping->a_page[12]) {
		//一级间接索引
		err = retake_block(sb, mapping->a_page[12]);
		if (err)
			return err;

		//清空该块
		err = clear_block(sb, mapping->a_page[12]);
		if (err)
			return err;
	}
	if (mapping->a_page[13]) {
		union {
			u8  *buf;
			u32 *data;
		} index_page;

		//二级间接索引
		err = retake_block(sb, mapping->a_page[13]);
		if (err)
			return err;
		
		//读取索引页
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

		//三级间接索引
		err = retake_block(sb, mapping->a_page[14]);
		if (err)
			return err;

		//读取索引页
		base = sb->s_base + esb->sects_per_block * mapping->a_page[14];
		count = esb->sects_per_block;
		err = read_block(index_page1.buf, base, count);
		if (err)
			return err;

		for (u32 i = 0; i < POINTERS_PER_INODE && index_page1.data[i]; i++) {
			err = retake_block(sb, index_page1.data[i]);
			if (err)
				return err;

			//读取索引页
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
	//在位图、描述符等全局结构中注销
	err = retake_inode(dentry->d_sb, inode->i_ino, type);
	if (err)
		return err;

	//释放地址空间和索引节点结构体
	generic_delete_alias(inode);
	kfree(mapping->a_page);
	kfree(mapping);
	kfree(inode);
	return 0;
}

u32 ext2_isempty(struct dentry* dentry) {
	//对于ext2文件系统来说，inode的i_count天生代表子目录的个数
	return (dentry->d_inode->i_count == 2);
}

//根据父目录的inode结构，查找dentry目录项
//返回得到是否存在、其所在的逻辑块号和块内字节偏移
u32 ext2_lookup_direntry(struct inode* inode, struct dentry* dentry, struct ext2_direntry *dr, u32* block, u32* offset, u32 type) {
	struct super_block		*sb;
	struct ext2_ext_sb		*esb;
	struct vfs_page			*page;
	struct qstr				filename;
	u32 j;			//运行中的动态块内偏移
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

	//对逻辑页依次进行遍历
	for (vpn = 0; vpn < inode->i_blocks; vpn++) {
		//找到对应的物理页号，并找来此页
		ppn = ext2_bmap(inode, vpn);
		page = general_find_page(ppn, inode);
		if (page == 0)
			break;

		//在每一页中遍历
		j = 0;
		while (j < sb->s_blksize) {
			//拿到一个目录项除文件名外的元信息
			kernel_memcpy(&dr->meta, page->p_data + j, sizeof(dr->meta));
			kernel_memset(filename.name, 0, sizeof(u8) * FILENAME_MX_LEN);
			filename.len = dr->meta.name_len;
			kernel_memcpy(filename.name, page->p_data + j + DIRENTRY_META_LEN, filename.len);
			
			//条件一：ino号不为零(代表该目录项没有被删)
			if (dr->meta.ino != 0) {
				//条件二：文件名吻合
				if (generic_check_filename(&dentry->d_name, &filename) == 0) {
					//条件三：文件类型匹配
					if ((dr->meta.type == DIR_NORMFILE && type == LAST_NORM) ||
						(dr->meta.type == DIR_DIRECTORY && type == LAST_DIR)) {
						found = 1;
						break;
					}
				}
			}

			//这一项不是，还得继续遍历找
			j += dr->meta.total_len;
		}

		if (found)
			break;
		//这一页中还没有，需要找下一页
	}

	kfree(filename.name);

	//对匹配项的所在逻辑块号和块内偏移赋值并传回
	if (found) {
		*block = vpn;
		*offset = j;
	}
	//注意返回时dr并没有带文件名字，但这对于上层函数来说已经不需要了
	return found;
}

//在给定的父目录nd下，为指定的dentry注册一个目录项
u32 ext2_apply_direntry(struct nameidata *nd, struct dentry* dentry, u32 ino, u32 type) {
	struct super_block		*sb = nd->dentry->d_sb;
	struct ext2_ext_sb		*esb = (struct ext2_ext_sb*)sb->s_fs_info;
	struct ext2_direntry	dr;
	struct ext2_direntry	prev_dr;
	struct inode			*inode;		//父目录的索引节点
	struct address_space	*mapping;	//父目录的地址空间
	struct vfs_page			*page;		//父目录的数据页
	u32 len_taken;		//已被占用的字节数
	u32 len_needed1;	//需要保证的连续字节数
	u32 len_needed2;	//遍历中目录项需要保证的连续字节数
	u32 len_tobe;		//新表项中的total_len值
	u32 err;
	u32 j;
	u32 vpn;
	u32 ppn;
	u32 block;			//可以放进去的位置所在逻辑块号
	u32 offset;			//可以放进去的位置所在块内偏移
	u32 found;

	//先算好该目录项至少需要多少字节(4的倍数)
	len_needed1 = ((((dentry->d_name.len - 1) >> 2) + 1) << 2) + DIRENTRY_META_LEN;
	inode = nd->dentry->d_inode;

	found = 0;
	//对inode的每个页依次遍历
	for (vpn = 0; vpn < inode->i_blocks; vpn++) {
		ppn = ext2_bmap(inode, vpn);
		page = general_find_page(ppn, inode);
		if (page == 0)
			return -ENOMEM;

		j = 0;
		while (j < sb->s_blksize) {
			//拿到一个目录项除文件名外的元信息
			kernel_memcpy(&prev_dr.meta, page->p_data + j, sizeof(prev_dr.meta));

			//考虑一种特殊情况
			if (prev_dr.meta.ino == 0 && j == 0) {
				//在删除目录项时，若一页中唯一的一项要删除时，由于不能造成空页的情况
				//所以表项数据仍旧保留，但ino要置零，整个表项长一个页的大小，即撑满整个页。
				//如果遍历时遇到这样的表项记录，可以直接把它覆盖掉
				found = 1;
				block = vpn;
				offset = 0;
				len_tobe = sb->s_blksize;

				//由于要覆盖原记录，而原记录撑满了整个页，所以先清空该页
				kernel_memset(page->p_data, 0, sizeof(u8) * sb->s_blksize);
				break;
			}

			//计算该目录项需要的最少长度和实际长度(都是4的倍数)
			len_taken = prev_dr.meta.total_len;
			len_needed2 = ((((prev_dr.meta.name_len - 1) >> 2) + 1) << 2) + DIRENTRY_META_LEN;

			if (len_needed1 + len_needed2 <= len_taken) {
				//找到可以填进去的位置了
				found = 1;
				block = vpn;
				offset = j + len_needed2;
				len_tobe = len_taken - len_needed2;
				prev_dr.meta.total_len = len_needed2;	//修改上一个目录项的总长度
				kernel_memcpy(page->p_data + j, &prev_dr.meta, sizeof(prev_dr.meta));
				break;
			}

			//没有找到，需要再看下一个表项
			j += len_taken;
		}

		if (found)
			break;
	}

	if (!found) {
		//所有现存页里面都找不到合适的位置，只有再添加新的一页
		if (ext2_create_page(inode, 1) != 1)
			return -ENOPAGE;

		block = inode->i_blocks - 1;	//(扩容后的)最后一页
		offset = 0;						//开头
		len_tobe = sb->s_blksize;
		ppn = ext2_bmap(inode, block);
		page = general_find_page(ppn, inode);
		if (page == 0)
			return -ENOPAGE;
	}
	//这样所有的情况都找到了应该写入的位置

	//下面对要写入的东西做赋值
	dr.meta.ino = ino;
	dr.meta.total_len = len_tobe;
	dr.meta.name_len = dentry->d_name.len;
	dr.meta.type = (type == LAST_DIR) ? DIR_DIRECTORY : DIR_NORMFILE;
	kernel_memset(dr.filename, 0, sizeof(u8) * FILENAME_MX_LEN);
	kernel_memcpy(dr.filename, dentry->d_name.name, dentry->d_name.len);

	//正式写入并写回
	kernel_memcpy(page->p_data + offset, &dr, len_needed1);
	page->p_state = PAGE_DIRTY;
	err = ext2_writepage(page);
	return err;
}

//给定父目录的索引节点，从给定块号和给定位置开始的表项删除
u32 ext2_delete_direntry(struct inode* inode, u32 block, u32 offset) {
	struct vfs_page			*page;
	struct ext2_direntry	dr;
	u32 len_todelete;		//待删除的表项长度
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

	//查找待删表项的前一项，因为在长度部分需要修改，不过需要排除待删表项是第一项的情况
	if (offset != 0) {
		prev_offset = 0;
		while (1) {
			kernel_memcpy(&dr.meta, page->p_data + prev_offset, sizeof(u8) * DIRENTRY_META_LEN);
			if (dr.meta.total_len + prev_offset == offset)
				break;
			prev_offset += dr.meta.total_len;
		}
		
		//修改长度、清空待删表项
		dr.meta.total_len += len_todelete;
		kernel_memcpy(page->p_data + prev_offset, &dr.meta, sizeof(u8) * DIRENTRY_META_LEN);
		kernel_memset(page->p_data + offset, 0, sizeof(u8) * len_todelete);
	}
	else {
		//offset == 0，待删表项在第一个
		if (len_todelete == inode->i_sb->s_blksize) {
			//此时该页中只有这一个表项，为了不空页，这里只把其ino域设置成0
			dr.meta.ino = 0;
			kernel_memcpy(page->p_data, &dr.meta, sizeof(u8) * DIRENTRY_META_LEN);
		}
		else {
			//此时该页中还有其他表项，找到待删表项其后的下一个表项
			next_offset = dr.meta.total_len;
			kernel_memcpy(&dr.meta, page->p_data + next_offset, sizeof(u8) *DIRENTRY_META_LEN);
			len_needed = ((((dr.meta.name_len - 1) >> 2) + 1) << 2) + DIRENTRY_META_LEN;
			kernel_memcpy(&dr.filename, page->p_data + next_offset + DIRENTRY_META_LEN, sizeof(u8) * (len_needed - DIRENTRY_META_LEN));
			
			//搬到前面来覆盖待删记录，更新下一个表项的总长度
			kernel_memset(page->p_data, 0, sizeof(u8) * (next_offset + len_needed));
			dr.meta.total_len += next_offset;
			kernel_memcpy(page->p_data, &dr, sizeof(u8) * len_needed);
		}
	}

	//删除完毕，写回该页数据
	page->p_state = PAGE_DIRTY;
	err = ext2_writepage(page);
	return err;
}

//拿到某文件的逻辑块号，算出它所对应的索引位置
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

	//块号太大，EXT2文件系统已经不支持
	return 1;
}

//根据某索引节点的15个数据块索引指针，求出该索引节点拥有的数据块确切数目
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
	u32 tmp;	//计算块数的中间变量
	u32 err = 0;
	u32 dp;		//直接索引，从0到14
	u32 fp;		//一级索引，从0到块大小/4 - 1
	u32 sp;		//二级索引，从0到块大小/4 - 1
	u32 tp;		//三级索引，从0到块大小/4 - 1

				//申请索引页缓存的空间
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
		next_pointer = 0;	//不再遍历下去了
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

	//访问一级间接索引，dp == 13/14/15
	if (next_pointer) {
		//读入该索引页的数据
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
			next_pointer = 0;	//不再遍历下去
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

	//访问二级索引，dp == 14/15
	if (next_pointer) {
		//读入该索引页的数据
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
			next_pointer = 0;	//不再遍历下去
		}
		else if (dp == 15) {
			tmp += (sp - 1) * pointers_per_block;
			next_pointer = index_page.data[sp - 1];
		}
	}

	//访问三级索引，dp == 15
	if (next_pointer) {
		//读入该索引页的数据
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

	//最终完成赋值
	*blocks = tmp;
fail:
	kfree(index_page.buf);
	return err;
}

//构造指定索引节点的地址空间，另需最高级数据块索引指针
//其中对a_page域也简单处理成定长数据，且仅保存最高级索引
//但是inode的i_blocks域为了给上层函数提供服务，必须是准确值
struct address_space* ext2_fill_mapping(struct inode* inode, u32 *pointers) {
	struct address_space	*mapping;
	u32 err;

	//先计算出该索引节点拥有数据块的确切数目
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

//加载好的位图作为第一个参数，对象在对应组块内的偏移作为第二个参数
u32 check_iorb(u8 *bitmap, u32 index) {
	return (bitmap[index >> 3] >> (index & 7)) & 1;
}

//申请一个新的索引节点，以指针的方式得到索引节点编号，返回非零错误
u32 apply_inode(struct super_block *sb, u32 *ino, u32 type) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	struct ext2_inode	xinode;
	u32 group;
	u32 byte;
	u32 bit;
	u32 err;
	u8  data;
	if (esb->total_unalloc_inodes == 0)
		return 1;	//已经满了
	
	//找到还未满的组块
	for (group = 0; group < esb->total_groups; group++) {
		if (dpttable[group].data.resolution.unalloc_inodes > 0)
			break;
	}

	//加载这个组块的位图信息
	err = load_bitmap(sb, group);
	if (err)
		return err;

	//找到还未满的那个字节
	for (byte = 0; byte < sb->s_blksize; byte++) {
		if (esb->bitmap.inodebit[byte] != 0xff) {
			data = esb->bitmap.inodebit[byte];
			break;
		}
	}

	//最终定位那一位‘0’
	for (bit = 0; bit < 8; bit++) {
		if (((data >> bit) & 1) == 0)
			break;
	}

	//最终确定索引节点编号
	*ino = group * esb->inodes_per_group + byte * 8 + bit + 1;

	//修改超级块数据、描述符数据和位图数据
	esb->total_unalloc_inodes--;
	dpttable[group].data.resolution.unalloc_inodes--;
	esb->bitmap.inodebit[byte] |= (1 << bit);
	esb->bitmap.dirty = 1;
	if (type == LAST_DIR)
		dpttable[group].data.resolution.dirs++;

	//更新索引节点表
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

	//马上写回磁盘，以求同步
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

//在指定组块内申请一个新的块，以指针的方式得到相对物理块号，返回非零错误
u32 apply_block(struct super_block *sb, u32 group, u32 *block) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 byte;
	u32 bit;
	u32 err;
	u8  data;

	if (esb->total_unalloc_blocks == 0 || dpttable->data.resolution.unalloc_blocks == 0)
		return 1;	//彻底满了，申请不了

	//定位找到那一个‘0’
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
	
	//修改元数据以及写回
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

//回收指定编号的索引节点，返回非零错误
u32 retake_inode(struct super_block* sb, u32 ino, u32 type) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	struct ext2_inode	xinode;
	u32 group;		//该索引节点所在组块号
	u32 index;		//所在组块内部的第几个
	u32 valid;
	u32 err;
	
	//定位
	group = (ino - 1) / esb->inodes_per_group;
	index = (ino - 1) % esb->inodes_per_group;

	err = load_bitmap(sb, group);
	if (err)
		return err;

	valid = check_iorb(esb->bitmap.inodebit, index);
	if (!valid)
		return 1;	//本来就无效，不能再回收

	//清除索引节点表项
	kernel_memset(&xinode, 0, sizeof(xinode));
	err = write_xinode(sb, ino, &xinode);
	if (err)
		return err;

	//执行回收，保存修改
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

//回收指定相对物理块号的块，返回非零错误
u32 retake_block(struct super_block *sb, u32 block) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 group;		//在第几个组块
	u32 index;		//所在组块内部的第几个
	u32 valid;
	u32 err;
	
	//定位
	group = block / esb->blocks_per_group;
	index = block % esb->blocks_per_group;
	err = load_bitmap(sb, group);
	if (err)
		return err;

	valid = check_iorb(esb->bitmap.blockbit, index);
	if (!valid)
		return 1;	//本来就无效，不能再回收

	//执行回收，保存修改
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

//清空一块数据，该函数主要针对不是文件数据块的那些块
//最典型的比如文件的间接索引的块
u32 clear_block(struct super_block *sb, u32 block) {
	struct ext2_ext_sb	*esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 base;
	u32 count;
	u32 err;
	u8  *buf;

	//为缓冲区申请空间
	buf = (u8*)kmalloc(sizeof(u8) * sb->s_blksize);
	if (buf == 0)
		return -ENOMEM;
	kernel_memset(buf, 0, sb->s_blksize);
	
	//写回磁盘
	base = sb->s_base + block * esb->sects_per_block;
	count = esb->sects_per_block;
	err = write_block(buf, base, count);
	return err;
}

//保存修改过后的超级块信息，主要针对两个表示未分配量的域
u32 save_sb(struct super_block *sb) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 group;
	u32 base;
	u32 err;
	u32 pow3, pow5, pow7;	//3、5、7的幂

	//从结构体到缓冲区
	kernel_memcpy(sb_buf + 12, &esb->total_unalloc_blocks, sizeof(u32));
	kernel_memcpy(sb_buf + 16, &esb->total_unalloc_inodes, sizeof(u32));

	//从缓冲区到磁盘，注意有多个组块同时备份超级块
	if (write_block(sb_buf, sb->s_base + 2, 1))
		return 1;

	//遍历其他组块，找到含超级块备份的组块号
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

		//只有含备份的组块才能运行至此，执行写回
		base = sb->s_base + group * esb->blocks_per_group * esb->sects_per_block;
		if (write_block(sb_buf, base, 1))
			return 1;
	}
	return 0;
}

//加载全局的描述符数组信息，虽然在文件系统工作过程中可能会有修改
//但是读取描述符数组信息只在初始化时发生一次，可谓一劳永逸
u32 load_dpttable(struct super_block *sb) {
	struct ext2_ext_sb	*esb;
	u32 blocks;		//描述符数据所跨的总块数
	u32 basesect;	//读取的起始扇区
	u32 i;			//遍历用块号
	u32 entries;	//描述符记录数
	u32 err;
	u8  *buf;
	u8  *p;

	esb = (struct ext2_ext_sb*)sb->s_fs_info;
	blocks = (esb->total_groups - 1) * DESCRIPTOR_ENTRY_LEN / sb->s_blksize + 1;
	
	//给缓冲区申请空间
	buf = (u8*)kmalloc(sizeof(u8) * sb->s_blksize);
	if (buf == 0)
		return -ENOMEM;

	//给描述符数组申请空间
	dpttable = (struct ext2_dptentry*)kmalloc(sb->s_blksize * blocks);
	if (dpttable == 0)
		return -ENOMEM;
	p = (u8*)dpttable;

	for (i = 0; i < blocks; i++) {
		basesect = sb->s_base + (esb->descriptor_block + i) * esb->sects_per_block;
		//读取该块内的数据
		err = read_block(buf, basesect, esb->sects_per_block);
		if (err)
			return err;

		if (i < blocks - 1)
			entries = sb->s_blksize / DESCRIPTOR_ENTRY_LEN;
		else
			entries = esb->total_groups % (sb->s_blksize / DESCRIPTOR_ENTRY_LEN);

		//把数据送到描述符数组中
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

//保存修改过后的描述符数组信息，第二个参数为被修改的组块号
u32 save_dpttable(struct super_block *sb, u32 group) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 block;		//该组块的描述符信息所在的块号偏移
	u32 basesect;	//要写的起始扇区号
	u32 len;
	u32 err;
	u8  *buf;	//缓冲区
	u8  *p;

	//申请并初始化该块缓冲区
	buf = (u8*)kmalloc(sizeof(u8) * sb->s_blksize);
	if (buf == 0)
		return 1;
	kernel_memset(buf, 0, sizeof(u8) * sb->s_blksize);

	//计算起始地址和长度
	block = group * DESCRIPTOR_ENTRY_LEN / sb->s_blksize;
	p = (u8*)&dpttable[block * sb->s_blksize / DESCRIPTOR_ENTRY_LEN];
	if ((esb->total_groups - 1) * DESCRIPTOR_ENTRY_LEN / sb->s_blksize == block)
		len = ((esb->total_groups - 1) % (sb->s_blksize / DESCRIPTOR_ENTRY_LEN)) * DESCRIPTOR_ENTRY_LEN;
	else
		len = sb->s_blksize;
	
	//从结构体到缓冲区
	kernel_memcpy(buf, p, len);

	//从缓冲区到磁盘
	basesect = sb->s_base + (esb->descriptor_block + block) * esb->sects_per_block;
	err = write_block(buf, basesect, esb->sects_per_block);
	kfree(buf);
	return err;
}

//加载指定组块的位图数据，包括块位图和索引节点位图
u32 load_bitmap(struct super_block *sb, u32 group) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 bbitmap_block;
	u32 ibitmap_block;
	u32 basesect;		//读取的起始扇区号
	u32 err;

	if (esb->bitmap.bg == group)
		return 0;

	//先保存原来的位图信息
	if (esb->bitmap.bg != group && esb->bitmap.dirty == 1) {
		err = save_bitmap(sb);
	}

	//计算块位图和索引节点位图的块号
	bbitmap_block = dpttable[group].data.resolution.block_bitmap_base;
	ibitmap_block = dpttable[group].data.resolution.inode_bitmap_base;

	//读取数据
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

//写回当前超级块内的位图数据，包括块位图和索引节点位图
u32 save_bitmap(struct super_block *sb) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 bbitmap_block;
	u32 ibitmap_block;
	u32 basesect;		//写回的起始扇区号
	u32 err;
	u32 group = esb->bitmap.bg;

	if (!esb->bitmap.dirty)
		return 0;	//如果不是脏的就不写回

	//计算块位图和索引节点位图的块号
	bbitmap_block = dpttable[group].data.resolution.block_bitmap_base;
	ibitmap_block = dpttable[group].data.resolution.inode_bitmap_base;

	//读取数据
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

//加载指定组块中的指定索引节点表块
u32 load_itable(struct super_block *sb, u32 group, u32 block) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 itable_base;	//该组块索引节点表的首块号
	u32 basesect;		//读取的首扇区号
	u32 err;

	if (esb->itable.group == group && esb->itable.block == block)
		return 0;

	//有必要时，需写回原来的索引节点表块
	if ((esb->itable.group != group || esb->itable.block != block) && esb->itable.dirty) {
		err = save_itable(sb);
		if (err)
			return err;
	}

	//计算地址并读相应的块进来
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

//写回当前超级块内的指定索引节点表块
u32 save_itable(struct super_block *sb) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 itable_base;	//该组块索引节点表的首块号
	u32 basesect;		//写回的首扇区号
	u32 group = esb->itable.group;
	u32 block = esb->itable.block;
	u32 err;

	//计算地址并读相应的块进来
	itable_base = dpttable[group].data.resolution.inode_table_base;
	basesect = sb->s_base + (itable_base + block) * esb->sects_per_block;
	err = write_block(esb->itable.buf, basesect, esb->sects_per_block);
	if (err)
		return err;

	esb->itable.dirty = 0;
	return 0;
}

//根据索引节点编号获得对应的索引节点结构
//返回非零错误
u32 read_xinode(struct super_block *sb, u32 ino, struct ext2_inode *xinode) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 group;		//组块号
	u32 index;		//组块内第几个
	u32 block;		//所在块的相对块号
	u32 offset;		//位于所在块的第几个
	u32 valid;		//对应的索引节点是否有效
	u32 err;
	
	//算好各种重要的中间量
	group = (ino - 1) / esb->inodes_per_group;
	index = (ino - 1) % esb->inodes_per_group;
	block = index * esb->inode_size / sb->s_blksize;
	offset = index % (sb->s_blksize / esb->inode_size);

	//加载位图并检查该索引节点是否有效
	err = load_bitmap(sb, group);
	if (err)
		return err;
	valid = check_iorb(esb->bitmap.inodebit, index);
	if (!valid)
		return -ENOINODE;

	//读取索引节点表
	err = load_itable(sb, group, block);
	if (err)
		return err;

	kernel_memcpy(xinode, esb->itable.buf + offset * esb->inode_size, INODE_SIZE_VALID);
	return 0;
}

u32 write_xinode(struct super_block *sb, u32 ino, struct ext2_inode *xinode) {
	struct ext2_ext_sb *esb = (struct ext2_ext_sb*)sb->s_fs_info;
	u32 group;		//组块号
	u32 index;		//组块内第几个
	u32 block;		//所在块的相对块号
	u32 offset;		//位于所在块的第几个
	u32 valid;		//对应的索引节点是否有效
	u32 err;

	//算好各种重要的中间量
	group = (ino - 1) / esb->inodes_per_group;
	index = (ino - 1) % esb->inodes_per_group;
	block = index * esb->inode_size / sb->s_blksize;
	offset = index % (sb->s_blksize / esb->inode_size);

	//加载位图并检查该索引节点是否有效
	err = load_bitmap(sb, group);
	if (err)
		return err;
	valid = check_iorb(esb->bitmap.inodebit, index);
	if (!valid)
		return -ENOINODE;

	//写回索引节点表
	err = load_itable(sb, group, block);
	if (err)
		return err;
	kernel_memcpy(esb->itable.buf + offset * esb->inode_size, xinode, INODE_SIZE_VALID);
	err = save_itable(sb);
	return err;
}
