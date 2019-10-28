#include <zjunix/fat32.h>
#include <zjunix/utils.h>
#include <zjunix/vfscache.h>
#include <zjunix/slab.h>
#include <driver/sd.h>

//extern vfs.c
extern struct dentry* 		root_dentry;		//全局的根目录项
extern struct dentry* 		pwd_dentry;			//当前所在目录项
extern struct vfsmount* 	root_mount;			//全局的根挂载信息
extern struct vfsmount* 	pwd_mount;			//当前所在挂载信息

//extern vfscache.c
extern struct cache			*dcache;			//dentry缓存
extern struct cache			*pcache;			//page缓存

const u32 lgfile_mapping[13] = { 0x1, 0x3, 0x5, 0x7, 0x9, 0xe, 0x10, 0x12, 0x14, 0x16, 0x18, 0x1c, 0x1e };

//实例化抽象数据结构，让函数指针指向FAT32系统对应的具体函数
struct address_space_operations fat32_address_space_operations = {
    .writepage = fat32_writepage,
    .readpage = fat32_readpage,
	.createpage = fat32_create_page,
    .bmap = fat32_bmap,
};

struct dentry_operations fat32_dentry_operations = {
    .compare = generic_check_filename,
	.isempty = fat32_isempty,
};

struct inode_operations fat32_inode_operations = {
    .create = fat32_createinode,
    .lookup = fat32_lookup,
};

struct file_operations fat32_file_operations = {
    .read = generic_file_read,
    .write = generic_file_write,
    .flush = generic_file_flush,
    .readdir = fat32_readdir,
};

struct super_operations fat32_super_operations = {
    .delete_inode = fat32_deleteinode,
    .write_inode = fat32_writeinode,
};

//从指定(绝对)扇区开始读入信息，以初始化FAT32系统
u32 fat32_init(u32 base_sect) {
	struct super_block      *fat32_sb;          //通用超级块
	struct fat32_ext_sb     *fat32_esb;         //超级块针对该文件系统的扩展信息
	struct fat32_fsinfo		*fat32_fi;			//该文件系统的FSINFO信息
	struct fat32_fattable	*fat32_ft;			//该文件系统的FAT表缓存
	struct file_system_type *fat32_fs_type;     //该文件系统类型
	struct dentry           *fat32_root_dentry; //该文件系统的根目录项
	struct inode            *fat32_root_inode;  //该文件系统根的索引节点对象
	struct address_space	*mapping;			//该文件系统根索引节点的地址空间
	struct vfsmount			*fat32_root_mount;	//该文件系统根的挂载信息
	u8						*DBRdata;			//DBR数据

	//从磁盘读入该文件系统的第1扇区
	DBRdata = (u8*)kmalloc(sizeof(u8) * BYTES_PER_SECT);
	if (DBRdata == 0)
		return -ENOMEM;
	if (read_block(DBRdata, base_sect, 1))
		return -EIO;

	//实例化扩展超级块
	fat32_esb = (struct fat32_ext_sb*)kmalloc(sizeof(struct fat32_ext_sb));
	if (fat32_esb == 0)
		return -ENOMEM;

	//进一步初始化扩展超级块
	kernel_memcpy(&fat32_esb->sects_per_clst, DBRdata + 13, sizeof(fat32_esb->sects_per_clst));
	kernel_memcpy(&fat32_esb->reserved_sects, DBRdata + 14, sizeof(fat32_esb->reserved_sects));
	kernel_memcpy(&fat32_esb->total_fats, DBRdata + 16, sizeof(fat32_esb->total_fats));
	kernel_memcpy(&fat32_esb->total_sects, DBRdata + 32, sizeof(fat32_esb->total_sects));
	kernel_memcpy(&fat32_esb->sects_per_fat, DBRdata + 36, sizeof(fat32_esb->sects_per_fat));
	kernel_memcpy(&fat32_esb->rootdir_base_clst, DBRdata + 0x2c, sizeof(fat32_esb->rootdir_base_clst));
	kernel_memcpy(&fat32_esb->fsinfo_base_sect, DBRdata + 0x30, sizeof(fat32_esb->fsinfo_base_sect));

	fat32_esb->fat_base_sect = (u32*)kmalloc(sizeof(u32)*fat32_esb->total_fats);
	if (fat32_esb->fat_base_sect == 0)
		return -ENOMEM;
	fat32_esb->fat_base_sect[0] = fat32_esb->reserved_sects + base_sect;
	u32 i = 1;
	while (i < fat32_esb->total_fats) {
		fat32_esb->fat_base_sect[i] = fat32_esb->fat_base_sect[i - 1] + fat32_esb->sects_per_fat;
		i++;
	}
	fat32_esb->data_clst_sect = fat32_esb->fat_base_sect[fat32_esb->total_fats - 1] + fat32_esb->sects_per_fat;
	kfree(DBRdata);

	//实例化fat32_fsinfo结构体
	fat32_fi = (struct fat32_fsinfo*)kmalloc(sizeof(struct fat32_fsinfo));
	if (fat32_fi == 0)
		return -ENOMEM;

	//再读取FSINFO所在的扇区,同时解析数据
	if (read_block(fat32_fi->data.buf, base_sect + (u32)fat32_esb->fsinfo_base_sect, 1))
		return -EIO;
	fat32_esb->fs_info = fat32_fi;

	//实例化FAT表缓存结构体
	fat32_ft = (struct fat32_fattable*)kmalloc(sizeof(struct fat32_fattable));
	fat32_ft->fat_sect_off = 0;
	fat32_ft->dirty = 0;
	if (read_block(fat32_ft->data.buf, (u32)fat32_esb->fat_base_sect[0], 1))
		return -EIO;
	fat32_esb->fat_table = fat32_ft;
	//自此，扩展超级块的实例化工作已经全部完成

	//创建并初始化file_system_type结构体
	fat32_fs_type = (struct file_system_type*)kmalloc(sizeof(struct file_system_type));
	if (fat32_fs_type == 0)
		return -ENOMEM;
	fat32_fs_type->name = "FAT32";

	//创建并部分初始化超级块对象
	fat32_sb = (struct super_block*)kmalloc(sizeof(struct super_block));
	if (fat32_sb == 0)
		return -ENOMEM;

	fat32_sb->s_base = base_sect;
	fat32_sb->s_blksize = BYTES_PER_SECT * fat32_esb->sects_per_clst;	//这里的块概念上等同于簇
	fat32_sb->s_dirt = 0;
	fat32_sb->s_root = 0;	//待会儿再设置
	fat32_sb->s_type = fat32_fs_type;
	fat32_sb->s_fs_info = (void*)fat32_esb;
	fat32_sb->s_op = &fat32_super_operations;

	//创建根对应的索引节点
	fat32_root_inode = (struct inode*)kmalloc(sizeof(struct inode));
	if (fat32_root_inode == 0)
		return -ENOMEM;

	//在FAT32中，i_ino与文件首簇的簇号对应起来
	fat32_root_inode->i_ino = fat32_esb->rootdir_base_clst;
	fat32_root_inode->i_blksize = fat32_sb->s_blksize;
	fat32_root_inode->i_count = 1;
	//为了方便，在FAT32系统中，目录文件的索引节点的i_size字段统一设为0
	fat32_root_inode->i_size = 0;
	fat32_root_inode->i_sb = fat32_sb;
	fat32_root_inode->i_op = &fat32_inode_operations;
	fat32_root_inode->i_fop = &fat32_file_operations;
	INIT_LIST_HEAD(&(fat32_root_inode->i_hash));
	INIT_LIST_HEAD(&(fat32_root_inode->i_LRU));
	INIT_LIST_HEAD(&(fat32_root_inode->i_dentry));		//待会儿再赋值
														//还需进一步调用函数对i_blocks、i_data赋值
	mapping = fat32_fill_mapping(fat32_root_inode);
	if (mapping == 0)
		return -ENOMAPPING;
	fat32_root_inode->i_data = mapping;

	//创建根对应的目录项
	fat32_root_dentry = (struct dentry*)kmalloc(sizeof(struct dentry));
	if (fat32_root_dentry == 0)
		return -ENOMEM;

	fat32_root_dentry->d_count = 1;
	fat32_root_dentry->d_inode = 0;	//待会儿再赋值
	fat32_root_dentry->d_mounted = 0;
	fat32_root_dentry->d_pinned = 1;
	fat32_root_dentry->d_sb = fat32_sb;
	fat32_root_dentry->d_parent = fat32_root_dentry;
	fat32_root_dentry->d_name.name = "/";
	fat32_root_dentry->d_name.len = 1;
	fat32_root_dentry->d_op = &fat32_dentry_operations;
	INIT_LIST_HEAD(&(fat32_root_dentry->d_alias));
	INIT_LIST_HEAD(&(fat32_root_dentry->d_child));
	INIT_LIST_HEAD(&(fat32_root_dentry->d_hash));
	//INIT_LIST_HEAD(&(fat32_root_dentry->d_LRU));
	INIT_LIST_HEAD(&(fat32_root_dentry->d_subdirs));

	//dentry加入dcache当中
	dcache->c_op->add(dcache, (void*)fat32_root_dentry);

	//根的inode和dentry互相关联
	fat32_root_dentry->d_inode = fat32_root_inode;
	list_add(&(fat32_root_dentry->d_alias), &(fat32_root_inode->i_dentry));

	//完善超级块的初始化
	fat32_sb->s_root = fat32_root_dentry;
	//自此，超级块、索引节点、目录项都实例化完毕

	//实例化和初始化vfsmount结构体
	fat32_root_mount = (struct vfsmount*)kmalloc(sizeof(struct vfsmount));
	if (fat32_root_mount == 0)
		return -ENOMEM;
	fat32_root_mount->mnt_parent = fat32_root_mount;
	fat32_root_mount->mnt_root = fat32_root_dentry;
	fat32_root_mount->mnt_mountpoint = fat32_root_dentry;
	fat32_root_mount->mnt_sb = fat32_sb;
	INIT_LIST_HEAD(&(fat32_root_mount->mnt_hash));

	//FAT32文件系统和全局根和全局挂载联系起来
	root_dentry = fat32_root_dentry;
	root_mount = fat32_root_mount;
	return 0;
}

//读取一个目录文件的目录项并放入第二个参数中，返回非零错误
u32 fat32_readdir(struct file *file, struct getdent *getdent) {
	struct fat32_ext_sb		*esb;
	struct inode			*inode;
	struct address_space	*mapping;
	struct fat32_direntry	dr;
	struct qstr				dr_filename;
	struct dirent			*gen_temp;
	u32 err;
	u32 capacity;			//首次分配的容量(可以保证空间足够)
	u32 size;				//实际的容量
	u32 base;				//长文件名有用
	u32 i;
	u32 j;
	u32 k;
	u32 n;

	//首先检查文件的类型，不是目录文件不得执行下去
	if (file->f_flags != LAST_DIR)
		return -ELASTTYPE;

	esb = (struct fat32_ext_sb*)file->f_dentry->d_sb->s_fs_info;
	if (esb == 0)
		return -ENOALLOCATE;
	inode = file->f_dentry->d_inode;
	if (inode == 0)
		return -ENOALLOCATE;
	if (inode->i_data == 0 || inode->i_data->a_page == 0) {
		mapping = fat32_fill_mapping(inode);
		if (mapping == 0)
			return -ENOMEM;
		inode->i_data = mapping;
	}
	////已确保inode的地址空间是有效的

	//kernel_printf("Welcome to fat32_readdir()\n");
	//kernel_printf("inode_ino: %d inode->page: %d\n", inode->i_ino, inode->i_blocks);

	i = 0;
	size = 0;
	err = 0;
	capacity = inode->i_blocks * esb->sects_per_clst * BYTES_PER_SECT / DIRENTRY_LEN;	//目录表项不会超过这个数
	gen_temp = (struct dirent*)kmalloc(sizeof(struct dirent)*capacity);
	if (gen_temp == 0)
		return -ENOMEM;

	dr_filename.name = (u8*)kmalloc(sizeof(u8) * FILENAME_MX_LEN);
	if (dr_filename.name == 0)
		return -ENOMEM;

	while (true) {
		err = read_dr(inode, i, &dr);
		if (err)
			//已经超出了有效目录区，遍历完毕
			break;

		if (dr.data.resolution.st_filename[0] == 0) {
			//已经超出了有效目录区，遍历完毕
			break;
		}
		else if (dr.data.resolution.st_filename[0] == 0xe5 ||
				 dr.data.resolution.attr == VOLNAME) {
			//无效目录表项，跳过
			i++;
			continue;
		}

		//给dr_filename赋值
		kernel_memset(dr_filename.name, 0, sizeof(u8) * FILENAME_MX_LEN);
		if (dr.data.resolution.attr == LONGNAME) {
			//如果遇到长文件名类型的目录表项，则一直循环到8-3类型的目录表项
			//并在循环过程中重构出长文件名
			n = dr.data.buf[0] & 0x3f;		//一共有几个长文件类型目录表项
			while (n > 0) {
				base = (n - 1) * 13;
				for (j = 0; j < 13; j++) {
					dr_filename.name[base + j] = dr.data.buf[lgfile_mapping[j]];
					if (dr_filename.name[base + j] == 0)
						dr_filename.len = base + j;
				}
				n--;
				i++;
				err = read_dr(inode, i, &dr);
				if (err)
					goto fat32_readdir_end;
			}
		}
		else {
			j = 0;
			k = 0;
			while (j < 8 && dr.data.resolution.st_filename[j] != 0x20) {
				dr_filename.name[k++] = dr.data.resolution.st_filename[j++];
			}
			dr_filename.name[k++] = '.';
			j = 8;
			while (j < 11 && dr.data.resolution.st_filename[j] != 0x20) {
				dr_filename.name[k++] = dr.data.resolution.st_filename[j++];
			}
			if (j == 8) {
				//取消‘.’
				dr_filename.name[--k] = 0;
			}
			dr_filename.len = k;
		}
		
		//运行至此，当前i对应的目录表项一定是8-3类型
		//对第size个dirent元素赋值，并更新size
		(gen_temp[size].ino) = (u32)(dr.data.resolution.first_clst_hi << 16) + (u32)(dr.data.resolution.first_clst_lo);
		(gen_temp[size].type) = (dr.data.resolution.attr & SUBDIR) ? LAST_DIR : LAST_NORM;
		(gen_temp[size].name) = (u8*)kmalloc(sizeof(u8) * (dr_filename.len + 1));
		if (gen_temp[size].name == 0) {
			err = -ENOMEM;
			goto fat32_readdir_end;
		}
		kernel_memcpy(gen_temp[size].name, dr_filename.name, dr_filename.len);
		gen_temp[size].name[dr_filename.len] = 0;
		size++;			//文件数量加一
		i++;
	}
	//正常循环结束程序跳至此处，此时size就是该目录下的所有文件数
	getdent->count = size;
	getdent->dirent = (struct dirent*)kmalloc(sizeof(struct dirent) * size);
	if (getdent->dirent == 0) {
		err = -ENOMEM;
		goto fat32_readdir_end;
	}
	kernel_memcpy(getdent->dirent, gen_temp, sizeof(struct dirent) * size);

	//正常结束和异常结束都要执行这段代码：
fat32_readdir_end:
	kfree(dr_filename.name);
	kfree(gen_temp);
	return err;
}

//给定一个修改过的索引节点和其父目录项，更新索引节点的目录表项
u32 fat32_writeinode(struct inode *inode, struct dentry *parent) {
	struct inode			*pinode;	//父目录的索引文件
	struct dentry			*dentry;	//该索引文件对应的目录项
	struct list_head		*head;
	struct fat32_direntry	dr;
	u32 found;
	u32 begin;
	u32 len;
	u32 err;

	head = &inode->i_dentry;
	dentry = container_of(head->next, struct dentry, d_alias);
	if (dentry == 0 || head->next == head)
		return -ENODENTRY;

	pinode = parent->d_inode;

	//寻找该索引节点对应的目录表项
	found = fat32_lookup_direntry(pinode, dentry, &dr, &begin, &len, LAST_NORM);
	if (!found)
		return -ENOINODE;

	//此处暂时只考虑普通文件大小变化的情况
	//忽略时间因素
	dr.data.resolution.size = inode->i_size;

	//写回父目录的数据页当中
	err = write_dr(pinode, begin + len - 1, &dr);
	if (err) {
		return -EIO;
	}
	return 0;
}

//删除目录项对应的内存中的VFS索引节点和磁盘上文件数据及元数据
//该函数暂不负责对dentry的更新，且不能删除目录类文件，返回非零错误
u32 fat32_deleteinode(struct dentry *dentry, u32 type) {
	struct inode			*p_inode;
	struct inode			*inode;
	struct address_space	*mapping;
	struct vfs_page			*page;
	struct list_head		*pos;
	struct list_head		*next_pos;
	struct super_block		*sb;
	struct fat32_direntry	dr;
	u32 found;
	u32 begin;
	u32 len;
	u32 i;
	u32 clst;
	u32 err;

	p_inode = dentry->d_parent->d_inode;
	inode = dentry->d_inode;
	mapping = inode->i_data;
	sb = dentry->d_sb;

	//删除目录表项
	found = fat32_lookup_direntry(p_inode, dentry, &dr, &begin, &len, type);
	if (!found) {
		//kernel_printf("Cannot find this file!\n");
		return -ENOINODE;
	}

	if (fat32_delete_direntry(p_inode, begin, len))
		return -ENOPAGE;

	//清空磁盘内的相应簇
	list_for_each_safe(pos, next_pos, &mapping->a_cache) {
		page = container_of(pos, struct vfs_page, p_list);
		//kernel_printf("page: %d\n", page->p_location);
		page->p_state = PAGE_INVALID;
		err = pcache->c_op->del(pcache, page);
		if (err)
			return err;
	}

	//删除FAT表项
	for (i = 0; i < inode->i_blocks; i++) {
		clst = mapping->a_page[i];
		retake_fat(sb, clst);
	}

	//释放地址空间和索引节点结构体
	generic_delete_alias(inode);
	kfree(mapping->a_page);
	kfree(mapping);
	kfree(inode);
	return 0;
}

//根据父目录的索引节点，寻找并构建dentry对应文件的索引节点，并将dentry和新的索引节点关联起来
struct dentry * fat32_lookup(struct inode *inode, struct dentry *dentry, u32 type) {
	//假设dentry已经基本初始化完毕，只需要和对应的new_inode关联
	struct fat32_direntry				dr;
	struct address_space				*mapping;
	struct inode						*new_inode;
	u32 begin;
	u32 len;
	u32 found;
	//kernel_printf("Welcome to fat32_lookup()\n");
	//kernel_printf("parent_id: %d; name: %s\n", inode->i_ino, dentry->d_name);
	found = fat32_lookup_direntry(inode, dentry, &dr, &begin, &len, type);
	if (!found) {
		return (struct dentry*)0;
	}

	//找到了表项，则构建新的索引节点
	new_inode = (struct inode*)kmalloc(sizeof(struct inode));
	if (new_inode == 0)
		return 0;
	new_inode->i_blksize = dentry->d_sb->s_blksize;
	new_inode->i_count = 1;
	new_inode->i_size = (dr.data.resolution.attr == SUBDIR) ? 0 : (dr.data.resolution.size);
	new_inode->i_ino = (u32)(dr.data.resolution.first_clst_hi << 16) + (dr.data.resolution.first_clst_lo & 0xff);
	new_inode->i_sb = dentry->d_sb;
	new_inode->i_op = &fat32_inode_operations;
	new_inode->i_fop = &fat32_file_operations;
	INIT_LIST_HEAD(&(new_inode->i_dentry));
	INIT_LIST_HEAD(&(new_inode->i_hash));
	INIT_LIST_HEAD(&(new_inode->i_LRU));

	mapping = fat32_fill_mapping(new_inode);
	if (mapping == 0)
		return 0;
	new_inode->i_data = mapping;

	//最后将dentry和new_inode关联起来
	dentry->d_inode = new_inode;
	list_add(&(dentry->d_alias), &(new_inode->i_dentry));

	//kernel_printf("inode_ino: %d\n", new_inode->i_ino);
	return dentry;
}

//对一个已经存在的索引节点实例化并初始化地址空间
//同时还会对索引节点的总块数做赋值
struct address_space* fat32_fill_mapping(struct inode* inode) {
	//假设inode的首簇和超级块已知
	struct super_block		*sb;
	struct address_space	*mapping;
	u32 capacity;
	u32 size;
	u32 clst_no;
	u32 *table;
	u32 *temp;

	//kernel_printf("Welcome to fat32_fill_mapping()\n");

	sb = inode->i_sb;
	clst_no = inode->i_ino;		//inode_no和该文件首簇对应
	capacity = 10;				//首次最大容量
	size = 0;					//实际容量
	table = (u32*)kmalloc(sizeof(u32)*capacity);
	if (table == 0)
		return 0;

	//kernel_printf("clst: %d\n", clst_no);

	while (true) {
		if (size == capacity) {
			//现有容量已经不够用，只能拓宽容量
			capacity *= 2;		//容量翻番
			temp = (u32*)kmalloc(sizeof(u32)*capacity);
			if (temp == 0)
				return 0;

			kernel_memcpy(temp, table, sizeof(u32)*capacity / 2);
			kfree(table);
			table = temp;		//table指向新的表空间
		}
		table[size++] = clst_no;
		clst_no = read_fat(sb, clst_no);
		if (clst_no == 0x0fffffff) {
			break;
		}
	}

	//跳出循环时，size代表该文件一共有多少个簇
	inode->i_blocks = size;

	//重新整理页映射表
	temp = (u32*)kmalloc(sizeof(u32)*size);
	if (temp == 0)
		return 0;

	kernel_memcpy(temp, table, sizeof(u32)*size);
	kfree(table);

	//构建地址空间
	mapping = (struct address_space*)kmalloc(sizeof(struct address_space));
	if (mapping == 0)
		return 0;

	mapping->a_host = inode;
	mapping->a_page = temp;
	mapping->a_pagesize = inode->i_blksize;
	mapping->a_op = &fat32_address_space_operations;
	INIT_LIST_HEAD(&(mapping->a_cache));
	return mapping;
}

u32 fat32_createinode(struct dentry *dentry, u32 mode, struct nameidata *nd) {
	//使用此函数时，假设inode还未分配空间，dentry除对应inode外已经完全赋值
	struct super_block		*sb;
	struct address_space	*mapping;
	struct inode			*inode;
	struct vfs_page			*page;
	u32 inode_no;
	u32 type;
	u32 err;

	sb = dentry->d_sb;
	if (sb == 0)
		return 1;

	//先初始化inode对应的地址空间
	mapping = (struct address_space*)kmalloc(sizeof(struct address_space));
	if (mapping == 0)
		return 1;

	inode = (struct inode*)kmalloc(sizeof(struct inode));
	if (inode == 0)
		return 1;

	mapping->a_pagesize = sb->s_blksize;
	mapping->a_host = inode;
	INIT_LIST_HEAD(&(mapping->a_cache));
	mapping->a_op = &fat32_address_space_operations;
	mapping->a_page = (u32*)kmalloc(sizeof(u32) * 1);
	if (mapping->a_page == 0)
		return 1;

	//kernel_printf("in fat32_createinode(), nd->last_type: %d\n", type);
	//需要在父目录对应的文件里面申请、登记新文件的目录项
	//这需要对父目录的文件(以及各FAT表)进行修改
	//这里把索引节点编号直接定位新文件的首簇号
	inode_no = fat32_apply_direntry(nd, dentry, nd->last_type);
	if (inode_no == -1)
		return 1;

	//对新的文件申请一个空的簇，更新mapping，初始化FAT值
	mapping->a_page[0] = inode_no;
	write_fat(sb, inode_no, 0x0fffffff);

	//填充inode
	inode->i_ino = inode_no;
	inode->i_size = 0;
	inode->i_count = (nd->last_type == LAST_DIR) ? 2 : 1;
	inode->i_blocks = 1;
	inode->i_data = mapping;
	inode->i_blksize = sb->s_blksize;
	inode->i_sb = sb;
	inode->i_op = &fat32_inode_operations;
	inode->i_fop = &fat32_file_operations;
	INIT_LIST_HEAD(&(inode->i_hash));
	INIT_LIST_HEAD(&(inode->i_LRU));
	INIT_LIST_HEAD(&(inode->i_dentry));
	
	list_add(&(dentry->d_alias), &(inode->i_dentry));
	dentry->d_inode = inode;

	//创造新的空页
	page = (struct vfs_page*)kmalloc(sizeof(struct vfs_page));
	if (page == 0)
		return 1;
	page->p_data = (u8*)kmalloc(sizeof(u8) * inode->i_blksize);
	if (page->p_data == 0)
		return 1;
	kernel_memset(page->p_data, 0, sizeof(u8) * inode->i_blksize);

	page->p_mapping = mapping;
	page->p_location = inode->i_ino;
	INIT_LIST_HEAD(&(page->p_list));
	INIT_LIST_HEAD(&(page->p_hash));
	INIT_LIST_HEAD(&(page->p_LRU));

	if (nd->last_type == LAST_DIR) {
		//如果要新建一个目录，要先对该目录文件进行初始化
		struct fat32_direntry	dr;
		u32 pinode_no;
		u32 i;

		//kernel_printf("DIR needs to do more!\n");
		//添加'.'目录项
		dr.data.resolution.st_filename[0] = '.';
		for (i = 1; i < FILENAME_ST_LEN; i++)
			dr.data.resolution.st_filename[i] = 0x20;
		dr.data.resolution.first_clst_hi = (u16)((inode_no >> 16) & 0xff);
		dr.data.resolution.first_clst_lo = (u16)(inode_no & 0xff);
		dr.data.resolution.attr = SUBDIR;
		dr.data.resolution.size = 0;
		dr.data.resolution.c_ms = 0;
		dr.data.resolution.c_time = 0x95c8;
		dr.data.resolution.c_date = 0x4d95;
		dr.data.resolution.a_date = 0x4d95;
		dr.data.resolution.d_time = 0x95c8;
		dr.data.resolution.d_date = 0x4d95;
		dr.data.resolution.rsv = 0x18;
		kernel_memcpy(page->p_data, dr.data.buf, DIRENTRY_LEN);

		//添加'..'目录项
		pinode_no = nd->dentry->d_inode->i_ino;
		dr.data.resolution.st_filename[0] = '.';
		dr.data.resolution.st_filename[1] = '.';
		for (i = 2; i < FILENAME_ST_LEN; i++)
			dr.data.resolution.st_filename[i] = 0x20;
		dr.data.resolution.first_clst_hi = (u16)((pinode_no >> 16) & 0xff);
		dr.data.resolution.first_clst_lo = (u16)(pinode_no & 0xff);
		dr.data.resolution.attr = SUBDIR;
		dr.data.resolution.size = 0;
		dr.data.resolution.c_ms = 0;
		dr.data.resolution.c_time = 0x95c8;
		dr.data.resolution.c_date = 0x4d95;
		dr.data.resolution.a_date = 0x4d95;
		dr.data.resolution.d_time = 0x95c8;
		dr.data.resolution.d_date = 0x4d95;
		dr.data.resolution.rsv = 0x18;
		kernel_memcpy(page->p_data + DIRENTRY_LEN, dr.data.buf, DIRENTRY_LEN);
	}

	page->p_state = PAGE_DIRTY;
	err = fat32_writepage(page);
	if (err)
		return 1;
	list_add(&(page->p_list), &(mapping->a_cache));
	pcache->c_op->add(pcache, (void*)page);
	return 0;
}

u32 fat32_isempty(struct dentry* dentry) {
	struct inode			*inode;
	struct fat32_direntry	dr;
	u32 err  = 0;
	u32 i    = 2;
	u32 flag = 1;

	inode = dentry->d_inode;

	//从第三项(i == 2)开始遍历
	while (1) {
		err = read_dr(inode, i, &dr);
		if (err)
			break;

		if (dr.data.buf[0] == 0xe5) {
			//如果该目录项已经被删，不能说明问题，继续看下一项
			i++;
			continue;
		}
		else if (dr.data.buf[0] == 0) {
			//如果首字节是0，说明该目录没有多余的子文件了，是空的
			break;
		}
		else {
			//其他的情况就是，该目录还有其他的子文件，不是空的
			flag = 0;
			break;
		}
	}
	return err ? 0 : flag;
}

u32 fat32_bmap(struct inode *inode, u32 vpn) {
	u32 cnt;
	u32 i;
	//if (inode->i_data == 0) {
	//	//如果还没有给地址空间赋值，则进行现场赋值
	//	mapping = fat32_fill_mapping(inode);
	//	if (mapping == 0)
	//		return -1;
	//	inode->i_data = mapping;
	//}

	if (vpn >= inode->i_blocks) {
		//如果逻辑页号超过了现在索引节点的最大编号，则当场申请新的页
		cnt = vpn - inode->i_blocks + 1;
		if (fat32_create_page(inode, cnt) != cnt)
			return -1;
	}
	return inode->i_data->a_page[vpn];
}

//返回非零错误
u32 fat32_readpage(struct vfs_page * page) {
	//该函数不负责页的链表结构和缓存结构的更新
	u32 base;						//从(绝对)扇区号开始
	u32 count;						//要读多少个扇区
	u32 err;						//返回值
	struct fat32_ext_sb*  esb;

	//kernel_printf("Welcome to fat32_readpage()\n");
	//kernel_printf("page->p_mapping addr: %d page->p_loc: %d\n", (u32)page->p_mapping, page->p_location);
	page->p_state = PAGE_INTACT;
	esb = (struct fat32_ext_sb*)page->p_mapping->a_host->i_sb->s_fs_info;
	if (esb == 0)
		return 1;

	base = esb->data_clst_sect + (page->p_location - 2) * esb->sects_per_clst;
	count = esb->sects_per_clst;

	err = read_block(page->p_data, base, count);
	if (err) {
		return err;
	}

	return 0;
}

//返回非零错误
u32 fat32_writepage(struct vfs_page* page) {
	u32 base;						//从(绝对)扇区号开始
	u32 count;						//要写多少个扇区
	u32 ret;						//返回值
	struct fat32_ext_sb*  esb;

	if (page->p_data == 0) {
		return 1;	//没有数据，错误
	}

	if (page->p_state == PAGE_INTACT)
		return 0;	//不需写回
	if (page->p_state == PAGE_INVALID)
		return 1;	//空页，错误

	//脏页，准备写函数的参数
	page->p_state = PAGE_INTACT;											//去除脏标记
	esb = (struct fat32_ext_sb*)page->p_mapping->a_host->i_sb->s_fs_info;	//找到该文件系统的元信息
	if (esb == 0)
		return 1;

	base = esb->data_clst_sect + (page->p_location - 2) * esb->sects_per_clst;
	count = esb->sects_per_clst;

	//执行写操作
	ret = write_block(page->p_data, base, count);
	return ret;
}

//在给定的父目录nd下，为指定的dentry申请并注册一个目录项
//这需要修改父目录的数据和inode信息
//返回申请到的inode_no号，-1表示申请失败
u32 fat32_apply_direntry(struct nameidata* nd, struct dentry* dentry, u32 type) {
	struct fat32_direntry	dr;			//缓存当前目录项用
	struct inode*			inode;		//父目录的索引节点
	struct address_space*   mapping;	//父目录的地址空间
	struct vfs_page*		page;		//数据页
	u32	err;
	u32	i;				//遍历的目录项索引
	u32 i_inpage;		//目录项在该页内的偏移
	u32 vpn;			//相对逻辑页号
	u32 ppn;			//相对物理页号
	u32 blks;			//父目录文件的总簇数
	u32	begin;			//开始注册的目录项索引号
	u32 len;			//可被替换的目录项数
	u32 entries_needed;	//该文件所需要的目录项数(主要考虑到长文件名会占用多个目录项)
	u32 inode_no;
	//kernel_printf("in fat32_apply_direntry(), nd->last_type: %d\n", type);
	entries_needed = get_direnty_no(dentry->d_name);

	//假设父目录的地址空间的页号映射表已经赋值完毕，直接用即可，不需再访问FAT表
	inode = nd->dentry->d_inode;
	if (inode == 0)
		return -1;
	blks = inode->i_blocks;

	mapping = inode->i_data;
	//对父目录文件的数据区做遍历，找到可以注册的地方
	i = 0;
	len = 0;
	while (1) {
		err = read_dr(inode, i, &dr);
		if (err)
			return -1;

		if (dr.data.resolution.st_filename[0] == 0) {
			//说明目录文件已经到结尾，从这里开始注册新目录项
			begin = i;
			break;
		}
		else if (dr.data.resolution.st_filename[0] == 0xe5) {
			//说明该目录项已被删除，可以替代，但还需要考虑长度合适否
			if ((++len) == entries_needed) {
				//若长度够了，则可以替换进去
				begin = i - len + 1;
				break;
			}
		}
		else {
			//该目录项被占用，不得替换出去
			len = 0;
		}
		//查看下一个目录项
		i++;
	}
	//结束循环时，begin就是开始注册的目录项索引号

	//申请新文件的索引节点号，用新文件的首簇号来表示
	//注意这里申请到的首簇号务必和目录表项中的对应数据相匹配!!
	inode_no = apply_fat(dentry->d_sb);
	if (inode_no == -1)
		return -1;

	//下面要开始正式注册该文件
	for (i = begin; i < begin + entries_needed; i++) {
		i_inpage = i % DIRENTRY_LEN;
		vpn = i / DIRENTRY_LEN;
		if (vpn == blks) {
			//需要给父目录申请一个新的页，以存放新的目录表项
			if (fat32_create_page(inode, 1) != 1)
				return -1;

			//更新blks和mapping
			blks = inode->i_blocks;
			mapping = inode->i_data;
		}
		//把这一页找过来
		ppn = mapping->a_page[vpn];
		page = general_find_page(ppn, inode);

		//填充这一项目录表项，注意要从最后一个表项开始倒序填充
		fill_direntry(dentry, type, &dr, entries_needed - 1 - (i - begin), entries_needed, inode_no);
		kernel_memcpy(page->p_data + i_inpage * DIRENTRY_LEN, dr.data.buf, DIRENTRY_LEN);
		page->p_state = PAGE_DIRTY;
		err = mapping->a_op->writepage(page);
		if (err)
			return -1;
	}
	//在FAT32文件系统中，目录文件的索引节点不需要用到i_size字段，所以可以不用更新inode->i_no
	return inode_no;
}

//给定父目录的索引节点，查找是否包含dentry所对应的文件，返回是否找到、对应的8-3目录表项
//对于长文件，还需要从何处开始长文件表项，总共的表项长度
u32 fat32_lookup_direntry(struct inode* inode, struct dentry* dentry, struct fat32_direntry* dr, u32* begin, u32* len, u32 type) {
	struct qstr			dr_filename;
	u32 i;
	u32 j;
	u32 k;
	u32 n;				//一个长文件名目录表项的数量
	u32 base;
	u32 found;
	u32 err;

	//kernel_printf("Welcome to fat32_lookup_direntry()\n");
	i = 0;
	found = 0;
	dr_filename.name = (u8*)kmalloc(sizeof(u8) * FILENAME_MX_LEN);
	if (dr_filename.name == 0) {
		return 0;
	}

	while (true) {
		err = read_dr(inode, i, dr);
		//kernel_printf("i: %d  ", i);
		if (err) {
			goto fat32_lookup_direntry_end;	//已经不是有效簇了
		}

		if (dr->data.resolution.st_filename[0] == 0) {
			//已经超出了有效目录区
			break;
		}
		else if (dr->data.resolution.st_filename[0] == 0xe5) {
			//无效目录表项，跳过
			i++;
			continue;
		}
		//给dr_filename赋值
		kernel_memset(dr_filename.name, 0, sizeof(u8) * FILENAME_MX_LEN);
		if (dr->data.resolution.attr == LONGNAME) {
			//如果遇到长文件名类型的目录表项，则一直循环到8-3类型的目录表项
			//并在循环过程中重构出长文件名
			*begin = i;
			n = dr->data.buf[0] & 0x3f;		//一共有几个长文件类型目录表项
			*len = n + 1;
			while (n > 0) {
				base = (n - 1) * 13;
				for (j = 0; j < 13; j++) {
					dr_filename.name[base + j] = dr->data.buf[lgfile_mapping[j]];
					if (dr_filename.name[base + j] == 0)
						dr_filename.len = base + j;
				}
				n--;
				i++;
				read_dr(inode, i, dr);
			}
		}
		else {
			*begin = i;
			*len = 1;
			j = 0;
			k = 0;
			while (j < 8 && dr->data.resolution.st_filename[j] != 0x20) {
				dr_filename.name[k++] = dr->data.resolution.st_filename[j++];
			}
			dr_filename.name[k++] = '.';
			j = 8;
			while (j < 11 && dr->data.resolution.st_filename[j] != 0x20) {
				dr_filename.name[k++] = dr->data.resolution.st_filename[j++];
			}
			if (j == 8) {
				//取消‘.’
				dr_filename.name[--k] = 0;
			}
			dr_filename.len = k;
		}
		//运行至此，当前i对应的目录表项一定是8-3类型
		//kernel_printf("dr_filename: %s\n", dr_filename.name);
		if (generic_check_filename(&dentry->d_name, &dr_filename) == 0) {
			if ((dr->data.resolution.attr == 0x20 && type == LAST_NORM) ||
				(dr->data.resolution.attr == 0x10 && type == LAST_DIR)) {
				//kernel_printf("find it!\n");
				found = 1;
				break;
			}
		}
		//kernel_printf("no find it!\n");
		i++;
	}
fat32_lookup_direntry_end:
	kfree(dr_filename.name);
	return found;
}

//给定父目录的索引节点，从给定位置开始的给定长度的表项全部删除
u32 fat32_delete_direntry(struct inode* inode, u32 begin, u32 len) {
	struct address_space	*mapping;	//父目录的地址空间
	struct vfs_page			*page;
	u32 i_inpage;						//目录项在该页内的偏移
	u32 vpn;							//相对逻辑页号
	u32 ppn;							//相对物理页号
	u32 i;
	u32 err;

	mapping = inode->i_data;
	for (i = begin; i < begin + len; i++) {
		i_inpage = i % DIRENTRY_LEN;
		vpn = i / DIRENTRY_LEN;
		ppn = mapping->a_page[vpn];
		page = general_find_page(ppn, inode);
		if (page == 0)
			return -ENOPAGE;

		page->p_data[i_inpage * DIRENTRY_LEN] = 0xe5;	//相当于直接修改页内数据
		page->p_state = PAGE_DIRTY;
		err = fat32_writepage(page);
		if (err)
			return err;
	}
	return 0;
}

//在索引节点对应的目录文件中，找到第i个目录项的内容并返回
//返回非零错误
u32 read_dr(struct inode* inode, u32 i, struct fat32_direntry* dr) {
	struct address_space	*mapping;	//父目录的地址空间
	struct vfs_page			*page;		//数据页
	u32 i_inpage;						//目录项在该页内的偏移
	u32 vpn;							//相对逻辑页号
	u32 ppn;							//相对物理页号
	
	mapping = inode->i_data;
	i_inpage = i % DIRENTRY_LEN;
	vpn = i / DIRENTRY_LEN;
	if (vpn >= inode->i_blocks) {
		return 1;
	}

	ppn = mapping->a_page[vpn];
	//kernel_printf("ppn: %d inode_ino: %d\n", ppn, inode->i_ino);
	page = general_find_page(ppn, inode);

	kernel_memcpy(dr, page->p_data + i_inpage * DIRENTRY_LEN, DIRENTRY_LEN);
	return 0;
}

//给定一个目录的索引节点和待写入的目录表项和写入的位置信息，完成写入操作
u32 write_dr(struct inode* inode, u32 i, struct fat32_direntry* dr) {
	struct address_space	*mapping;
	struct vfs_page			*page;
	u32 vpn;
	u32 ppn;
	u32 i_inpage;

	mapping = inode->i_data;
	i_inpage = i % DIRENTRY_LEN;
	vpn = i / DIRENTRY_LEN;
	if (vpn >= inode->i_blocks) {
		return 1;
	}

	ppn = mapping->a_page[vpn];
	//kernel_printf("ppn: %d inode_ino: %d\n", ppn, inode->i_ino);
	page = general_find_page(ppn, inode);

	kernel_memcpy(page->p_data + i_inpage * DIRENTRY_LEN, dr, DIRENTRY_LEN);
	page->p_state = PAGE_DIRTY;
	return mapping->a_op->writepage(page);
}

void write_fat(struct super_block* sb, u32 clst_no, u32 fat_value) {
	struct fat32_ext_sb		*fat_esb;
	struct fat32_fattable	*ft;				// FAT表缓存
	u32  j;
	u32  sect_off;
	u32  entr_off;

	fat_esb = (struct fat32_ext_sb*)sb->s_fs_info;
	ft = ((struct fat32_ext_sb*)sb->s_fs_info)->fat_table;

	sect_off = (clst_no * 4) / BYTES_PER_SECT;	//该簇所在FAT表中的扇区偏移
	entr_off = clst_no % (BYTES_PER_SECT / 4);	//该簇在该扇区内的表项数偏移
	if (ft->fat_sect_off != sect_off) {
		if (ft->dirty) {
			//如果FAT表原扇区的数据被修改过，需要依次写回磁盘内所有的FAT扇区
			for (j = 0; j < fat_esb->total_fats; j++) {
				write_block(ft->data.buf, fat_esb->fat_base_sect[j] + ft->fat_sect_off, 1);
			}
		}
		ft->fat_sect_off = sect_off;
		read_block(ft->data.buf, fat_esb->fat_base_sect[0] + sect_off, 1);
	}

	//自此当前扇区内的FAT表数据已经加载进内存
	ft->data.fat[entr_off] = fat_value;

	//马上写回磁盘，以求同步
	for (j = 0; j < fat_esb->total_fats; j++) {
		write_block(ft->data.buf, fat_esb->fat_base_sect[j] + ft->fat_sect_off, 1);
	}
}

//查询某簇的FAT值
u32 read_fat(struct super_block* sb, u32 clst_no) {
	struct fat32_ext_sb		*fat_esb;
	struct fat32_fsinfo		*fi;				// 指向fat32_fsinfo结构体
	struct fat32_fattable	*ft;				// FAT表缓存
	u32  j;
	u32  sect_off;
	u32  entr_off;

	fat_esb = (struct fat32_ext_sb*)sb->s_fs_info;
	if (fat_esb == 0)
		return -1;
	fi = ((struct fat32_ext_sb*)sb->s_fs_info)->fs_info;
	ft = ((struct fat32_ext_sb*)sb->s_fs_info)->fat_table;
	if (fi == 0 || ft == 0)
		return -1;

	sect_off = (clst_no * 4) / BYTES_PER_SECT;			//该簇所在FAT表中的扇区偏移
	entr_off = ((clst_no * 4) % BYTES_PER_SECT) / 4;	//该簇在该扇区内的表项数偏移	

	if (ft->fat_sect_off != sect_off) {
		if (ft->dirty) {
			//如果FAT表原扇区的数据被修改过，需要依次写回磁盘内所有的FAT扇区
			for (j = 0; j < fat_esb->total_fats; j++) {
				write_block(ft->data.buf, fat_esb->fat_base_sect[j] + ft->fat_sect_off, 1);
			}
		}
		ft->fat_sect_off = sect_off;
		read_block(ft->data.buf, fat_esb->fat_base_sect[0] + sect_off, 1);
	}

	//自此当前扇区内的FAT表数据已经加载进内存
	return ft->data.fat[entr_off];
}

//申请一个新的簇用来存放数据
//这需要修改各FAT表的数据和FSINFO扇区的数据
//返回申请到的簇号，-1表示申请失败
u32 apply_fat(struct super_block* sb) {
	struct fat32_ext_sb		*fat_esb;
	struct fat32_fsinfo		*fi;				// 指向fat32_fsinfo结构体
	struct fat32_fattable	*ft;				// FAT表缓存
	u32	 clst_no;
	u32  i;
	u32  j;
	u32  err;
	u32  sect_off;
	u32  entr_off;
	u32  total_dat_clst;						//数据区总共拥有的簇的数量

	fat_esb = (struct fat32_ext_sb*)sb->s_fs_info;
	if (fat_esb == 0)
		return -1;
	fi = ((struct fat32_ext_sb*)sb->s_fs_info)->fs_info;
	ft = ((struct fat32_ext_sb*)sb->s_fs_info)->fat_table;
	if (fi == 0 || ft == 0)
		return -1;
	total_dat_clst = (fat_esb->total_sects - (fat_esb->data_clst_sect - sb->s_base)) / fat_esb->sects_per_clst;

	if (fi->data.resolution.free_clsts == 0) {
		//已经没有空闲簇
		return -1;
	}
	//一定还有空闲簇
	clst_no = fi->data.resolution.next_free_clst;	//准备返回的新簇簇号
	i = clst_no + 1;								//从下一个簇开始找新的空闲簇
	while (true) {
		if (i >= total_dat_clst) 
			i = 0;									//循环遍历
		if (read_fat(sb, i) == 0)
			break;
		//继续迭代
		i++;
	}
	//更新并写回FSINFO结构
	fi->data.resolution.free_clsts--;
	fi->data.resolution.next_free_clst = i;
	err = write_block(fi->data.buf, sb->s_base + (u32)fat_esb->fsinfo_base_sect, 1);
	return err ? -1 : clst_no;
}

void retake_fat(struct super_block* sb, u32 clst_no) {
	struct fat32_ext_sb		*fat_esb;
	struct fat32_fsinfo		*fi;				// 指向fat32_fsinfo结构体

	fat_esb = (struct fat32_ext_sb*)sb->s_fs_info;
	fi = fat_esb->fs_info;

	//先修改FSINFO结构体并立即写回
	fi->data.resolution.free_clsts++;
	if (clst_no < fi->data.resolution.next_free_clst)
		fi->data.resolution.next_free_clst = clst_no;
	write_block(fi->data.buf, sb->s_base + (u32)fat_esb->fsinfo_base_sect, 1);

	//再修改FAT表的对应表项
	write_fat(sb, clst_no, 0);
}

//给定一个子目录的索引节点，给该子目录扩充给定数量的新数据页
//需要修改FAT表的数据、FSINFO扇区的数据
//返回成功扩充的页数量，正常情况下返回值务必等于cnt
u32 fat32_create_page(struct inode* inode, u32 cnt) {
	struct address_space*	mapping;
	u32 i;
	u32 *buf;
	u32 new_clst;
	u32 cur_cnt;

	mapping = inode->i_data;

	//把原来的地址空间内的页映射表拷到buf里面
	buf = (u32*)kmalloc(sizeof(u32)*inode->i_blocks);
	if (buf == 0)
		return 0;
	kernel_memcpy(buf, mapping->a_page, inode->i_blocks * sizeof(u32));
	kfree(mapping->a_page);

	//拓宽地址空间，并把buf数据拷进来
	mapping->a_page = (u32*)kmalloc(sizeof(u32)*(inode->i_blocks + cnt));
	if (mapping->a_page == 0)
		return 0;
	kernel_memcpy(mapping->a_page, buf, inode->i_blocks * sizeof(u32));
	kfree(buf);

	//最后申请新的页，并更新FAT表和地址空间
	cur_cnt = 0;
	for (i = 0; i < cnt; i++) {
		new_clst = apply_fat(inode->i_sb);
		if (new_clst == -1) {
			break;
		}
		mapping->a_page[inode->i_blocks++] = new_clst;		//在此处会顺便更新inode->i_blocks
		write_fat(inode->i_sb, mapping->a_page[inode->i_blocks - 2], new_clst);
		write_fat(inode->i_sb, new_clst, 0x0fffffff);
		cur_cnt++;
	}
	return cur_cnt;
}

//根据dentry填充第index个目录表项信息，总共有total_no个表项表示同一个文件
void fill_direntry(struct dentry* dentry, u32 type, struct fat32_direntry* dir, u32 index, u32 total_no, u32 inode_no) {
	//这里假设index都是合法的，不再对index做额外的检验
	//长文件名不考虑扩展名很长的情况

	int ext_base;
	u32 ext_len;
	u32 file_len;

	//找到扩展名的开始处和扩展名的长度
	for (ext_base = dentry->d_name.len - 1; ext_base >= 0 && dentry->d_name.name[ext_base] != '.'; ext_base--);
	if (ext_base < 0) {
		//全名没有‘.’，认为没有扩展名
		ext_len = 0;
		file_len = dentry->d_name.len;
		ext_base = dentry->d_name.len;
	}
	else {
		//ext_base指向‘.’后面的那个字符
		ext_len = dentry->d_name.len - ext_base - 1;
		file_len = ext_base;
		ext_base++;
	}

	if (index == 0) {
		//按照8-3格式填充目录表项		
		u32 i;
		u8  c;

		//分情况对文件名进行赋值
		if (total_no == 1) {
			//按照短文件名来处理
			for (i = 0; i < 8; i++) {
				if (i < file_len) {
					//在文件名范围内
					c = dentry->d_name.name[i];
					if (c >= 'a' && c <= 'z')
						c += ('A' - 'a');	//小写换大写
					dir->data.resolution.st_filename[i] = c;
				}
				else {
					//不在文件名范围内，用0x20来填充
					dir->data.resolution.st_filename[i] = 0x20;
				}

			}
			for (i = 8; i < 11; i++) {
				if (i < ext_len + 8) {
					//在扩展名范围内
					c = dentry->d_name.name[ext_base + i - 8];
					if (c >= 'a' && c <= 'z')
						c += ('A' - 'a');	//小写换大写
					dir->data.resolution.st_filename[i] = c;
				}
				else {
					//不在扩展名范围内，用0x20填充
					dir->data.resolution.st_filename[i] = 0x20;
				}
			}
		}
		else {
			//按照长文件名来处理
			for (i = 0; i < 6; i++) {
				c = dentry->d_name.name[i];
				if (c >= 'a' && c <= 'z')
					c += ('A' - 'a');	//小写换大写
				dir->data.resolution.st_filename[i] = c;
			}
			dir->data.resolution.st_filename[6] = '~';
			dir->data.resolution.st_filename[7] = '1';	//此处简化处理，等有了fcache再优化

			for (i = 8; i < 11; i++) {
				if (i < ext_len + 8) {
					//在扩展名范围内
					c = dentry->d_name.name[ext_base + i - 8];
					if (c >= 'a' && c <= 'z')
						c += ('A' - 'a');	//小写换大写
					dir->data.resolution.st_filename[i] = c;
				}
				else {
					//不在扩展名范围内，用0x20填充
					dir->data.resolution.st_filename[i] = 0x20;
				}
			}
		}

		//对属性赋值
		if (type == LAST_NORM)
			dir->data.resolution.attr = 0x20;
		else if (type == LAST_DIR)
			dir->data.resolution.attr = 0x10;
		else
			kernel_printf("nd->last_type: %d\n", type);

		//对各种时间日期赋值
		//略

		dir->data.resolution.first_clst_hi = (u16)((inode_no >> 16) & 0xff);
		dir->data.resolution.first_clst_lo = (u16)(inode_no & 0xff);

		//对文件长度赋值
		dir->data.resolution.size = 0;

		//对时间和大小写的赋值(伪赋值)
		dir->data.resolution.c_time = 0x95c8;
		dir->data.resolution.c_date = 0x4d95;
		dir->data.resolution.a_date = 0x4d95;
		dir->data.resolution.d_time = 0x95c8;
		dir->data.resolution.d_date = 0x4d95;
		dir->data.resolution.rsv = 0x18;
	}
	else {
		//按照长文件名填充目录表项
		u32 base;	//从此处开始填充目录表项
		u32 end = dentry->d_name.len;	//buf[end] == 0
		u8 buf[FILENAME_MX_LEN];
		kernel_memset(buf, 0, sizeof(u8) * FILENAME_MX_LEN);
		kernel_memcpy(buf, dentry->d_name.name, file_len);
		buf[file_len++] = '.';
		kernel_memcpy(buf + file_len, dentry->d_name.name + ext_base, ext_len);
		base = (index - 1) * 13;
		dir->data.buf[0x00] = (index == total_no - 1) ? ((u8)index | 0x40) : (u8)index;
		dir->data.buf[0x01] = buf[base];
		dir->data.buf[0x02] = 0;
		dir->data.buf[0x03] = (base + 1 > end) ? 0xff : buf[base + 1];
		dir->data.buf[0x04] = (base + 1 > end) ? 0xff : 0;
		dir->data.buf[0x05] = (base + 2 > end) ? 0xff : buf[base + 2];
		dir->data.buf[0x06] = (base + 2 > end) ? 0xff : 0;
		dir->data.buf[0x07] = (base + 3 > end) ? 0xff : buf[base + 3];
		dir->data.buf[0x08] = (base + 3 > end) ? 0xff : 0;
		dir->data.buf[0x09] = (base + 4 > end) ? 0xff : buf[base + 4];
		dir->data.buf[0x0a] = (base + 4 > end) ? 0xff : 0;
		dir->data.buf[0x0b] = 0x0f;
		dir->data.buf[0x0c] = 0;
		dir->data.buf[0x0d] = 0x2a;
		dir->data.buf[0x0e] = (base + 5 > end) ? 0xff : buf[base + 5];
		dir->data.buf[0x0f] = (base + 5 > end) ? 0xff : 0;
		dir->data.buf[0x10] = (base + 6 > end) ? 0xff : buf[base + 6];
		dir->data.buf[0x11] = (base + 6 > end) ? 0xff : 0;
		dir->data.buf[0x12] = (base + 7 > end) ? 0xff : buf[base + 7];
		dir->data.buf[0x13] = (base + 7 > end) ? 0xff : 0;
		dir->data.buf[0x14] = (base + 8 > end) ? 0xff : buf[base + 8];
		dir->data.buf[0x15] = (base + 8 > end) ? 0xff : 0;
		dir->data.buf[0x16] = (base + 9 > end) ? 0xff : buf[base + 9];
		dir->data.buf[0x17] = (base + 9 > end) ? 0xff : 0;
		dir->data.buf[0x18] = (base + 10 > end) ? 0xff : buf[base + 10];
		dir->data.buf[0x19] = (base + 10 > end) ? 0xff : 0;
		dir->data.buf[0x1a] = 0;
		dir->data.buf[0x1b] = 0;
		dir->data.buf[0x1c] = (base + 11 > end) ? 0xff : buf[base + 11];
		dir->data.buf[0x1d] = (base + 11 > end) ? 0xff : 0;
		dir->data.buf[0x1e] = (base + 12 > end) ? 0xff : buf[base + 12];
		dir->data.buf[0x1f] = (base + 12 > end) ? 0xff : 0;
	}
}

//根据文件名，算出需要几个目录项才能装得下该文件信息
//主要是因为长文件的问题
u32 get_direnty_no(struct qstr filename) {
	u32 i;
	u32 len = filename.len;
	if (len <= 11) {
		return 1;
	}
	else {
		return 2 + len / 13;
	}
}
