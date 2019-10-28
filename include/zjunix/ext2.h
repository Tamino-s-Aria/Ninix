#ifndef _ZJUNIX_EXT2_H_
#define _ZJUNIX_EXT2_H_

#include <zjunix/vfs.h>

#define			INODE_SIZE_VALID			128
#define			DESCRIPTOR_ENTRY_LEN		32
#define			DIRENTRY_META_LEN			8
#define			POINTERS_PER_INODE			15
#define			SINGLE_INDIRECT				12
#define			DOUBLE_INDIRECT				13
#define			TRIPLE_INDIRECT				14

#define			DIR_NORMFILE				0X1		//目录项中表示普通文件的数值
#define			DIR_DIRECTORY				0x2		//目录项中表示目录文件的数值
#define			INODE_NORMFILE				0x8000	//索引节点中表示普通文件的数值
#define			INODE_DIRECTORY				0x4000	//索引节点中表示目录文件的数值

#define			test						kernel_printf

//位图结构，整合了块位图和索引节点位图
struct ext2_bitmap {
	u32					bg;							//组块号
	u32					dirty;						//脏块标记
	u8					*blockbit;					//块位图
	u8					*inodebit;					//索引节点位图
};

// 描述符表项解析结构
struct ext2_dptentry_resolution {
	u32					block_bitmap_base;			//该组块所属块位图的块号
	u32					inode_bitmap_base;			//该组块所属索引节点的块号
	u32					inode_table_base;			//该组块所属索引节点表的块号
	u16					unalloc_blocks;				//该组块内尚未分配的块数
	u16					unalloc_inodes;				//该组块内尚未分配的索引节点数
	u16					dirs;						//该组块内目录文件的数量
	u8					rsv[14];					//未使用
};

// 描述符表项结构
struct ext2_dptentry {
	union {
		u8				buf[DESCRIPTOR_ENTRY_LEN];
		struct ext2_dptentry_resolution
						resolution;
	}data;
};

// 目录表项除文件名外的结构
struct ext2_direntry_meta {
	u32					ino;						//索引节点编号
	u16					total_len;					//此表项总长度
	u8					name_len;					//文件名长度
	u8					type;						//文件类型
};

// 完整的目录表项结构
struct ext2_direntry {
	struct ext2_direntry_meta	meta;
	u8							filename[FILENAME_MX_LEN];
};

// 索引节点解析结构
struct ext2_inode_resolution {
	u16					type_permission;
	u16					user_id;
	u32					size;
	u32					a_time;
	u32					c_time;
	u32					m_time;
	u32					d_time;
	u16					bg_id;
	u16					count;
	u32					sectors;
	u32					others1[2];
	u32					pointers[POINTERS_PER_INODE];
	u32					others2[7];
};

// 索引节点结构 (注意：这里只包含一个索引节点记录的前128个字节！)
struct ext2_inode {
	union {
		u8				buf[INODE_SIZE_VALID];
		struct ext2_inode_resolution
						resolution;
	}data;
};

//索引节点表的容器(以一块为单位读写)
struct ext2_itable_block {
	u32					group;				//组块号
	u32					block;				//组块内的块偏移
	u32					dirty;				//脏块标记
	u8					*buf;				//数据
};

// EXT2文件系统的元信息，作为通用超级块的扩充
struct ext2_ext_sb {
	u32					total_inodes;				//整个文件系统的索引节点总数
	u32					total_blocks;				//整个文件系统的块数(此处的块可理解为FAT中的簇)
	u32					total_groups;				//整个文件系统的组块数(并非直接从磁盘得知)
	u32					total_unalloc_inodes;		//整个文件系统中尚未分配的索引节点总数
	u32					total_unalloc_blocks;		//整个文件系统中尚未分配的块总数
	u32					blocks_per_group;			//每个组块各有多少个块
	u32					sects_per_block;			//每个块各有多少个扇区(需要计算得到)
	u32					inodes_per_group;			//每个组块各有多少个索引节点
	u16					inode_size;					//每个索引节点的长度(按字节计算)
	u32					inode_table_blks;			//每个块组索引节点表所占块数
	u32					descriptor_block;			//组块描述符所在的块号(1还是2，需要计算得到)
	struct ext2_bitmap	bitmap;						//两个位图
	struct ext2_itable_block
		itable;						//索引节点表数据
};

//ext2.c
u32 ext2_init(u32);

u32 ext2_readpage(struct vfs_page *);
u32 ext2_writepage(struct vfs_page*);
u32 ext2_bmap(struct inode *, u32);
u32 ext2_create_page(struct inode*, u32);

u32 ext2_readdir(struct file *, struct getdent *);
struct dentry* ext2_lookup(struct inode *, struct dentry *, u32);
u32 ext2_createinode(struct dentry *, u32, struct nameidata *);
u32 ext2_writeinode(struct inode *, struct dentry *);
u32 ext2_deleteinode(struct dentry *, u32);
u32 ext2_isempty(struct dentry*);

u32 ext2_lookup_direntry(struct inode*, struct dentry*, struct ext2_direntry *, u32*, u32*, u32);
u32 ext2_apply_direntry(struct nameidata *, struct dentry*, u32, u32);
u32 ext2_delete_direntry(struct inode*, u32, u32);

u32 get_location(struct super_block *, u32, u32 *, u32 *, u32 *, u32 *);
u32 count_blocks(struct super_block *, u32 *, u32 *);
struct address_space* ext2_fill_mapping(struct inode*, u32 *);

u32 check_iorb(u8 *, u32);
u32 apply_inode(struct super_block *, u32 *, u32);
u32 apply_block(struct super_block *, u32, u32 *);
u32 retake_inode(struct super_block*, u32, u32);
u32 retake_block(struct super_block *, u32);
u32 clear_block(struct super_block *, u32);

u32 save_sb(struct super_block *);
u32 load_dpttable(struct super_block *);
u32 save_dpttable(struct super_block *, u32);
u32 load_bitmap(struct super_block *, u32);
u32 save_bitmap(struct super_block *);
u32 load_itable(struct super_block *, u32, u32);
u32 save_itable(struct super_block *);
u32 read_xinode(struct super_block *, u32, struct ext2_inode *);
u32 write_xinode(struct super_block *, u32, struct ext2_inode *);

#endif