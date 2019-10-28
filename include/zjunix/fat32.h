#ifndef _ZJUNIX_FAT32_H_
#define _ZJUNIX_FAT32_H_

#include <zjunix/vfs.h>

#define 			DIRENTRY_LEN			32		// 一个目录项的长度
#define 			FILENAME_ST_LEN			11		// 8-3格式文件名（连带扩展名）长度上限

// 属性常量
#define				RDONLY					0x01 	// 只读
#define				HIDDEN					0x02 	// 隐藏
#define 			SYSFILE					0x04	// 系统文件
#define 			VOLNAME					0x08 	// 卷号
#define				SUBDIR					0x10 	// 子目录
#define 			ARCHIVE					0x20 	// 真·文件
#define 			LONGNAME				0x0F	// 长文件名补充项

#define				true					1


// FAT32文件系统的元信息，作为通用超级块的扩充
struct fat32_ext_sb{
	u8 						sects_per_clst;			// 每个簇的扇区数
	u16 					reserved_sects;			// 预留扇区数，用于定位FAT表的首扇区号
	u8						total_fats; 			// FAT表的数目，一般为2
	u32 					total_sects;			// 扇区总数
	u32 					sects_per_fat;			// 每个FAT表所占扇区数
	u32 					rootdir_base_clst;		// 根目录的首簇号，一般为2
	u16 					fsinfo_base_sect;		// FSINFO扇区的编号
	u32						*fat_base_sect;			// FAT表的首扇区号(数组)
	u32						data_clst_sect;			// 数据区(第2簇始)的总首扇区号
	struct fat32_fsinfo		*fs_info;				// 指向fat32_fsinfo结构体
	struct fat32_fattable	*fat_table;				// FAT表缓存
};

struct fat32_fattable {
	u32						fat_sect_off;			// 当前数据时FAT表中的第几个扇区
	u8						dirty;					// 是否被修改过
	union {
		u8					buf[BYTES_PER_SECT];	// FAT表该扇区的数据缓冲区
		u32					fat[BYTES_PER_SECT / 4];// 解析后的FAT表项数据
	} data;
};

struct fat32_fsinfo_resolution {
	// 和FSINFO数据全部对齐
	u8				reverse1[0x1e8];		// 前0x1e8个无关字节
	u32				free_clsts;				// 空闲簇的总数
	u32				next_free_clst;			// 下一个空闲簇的编号
	u8				reverse2[0x10];			// 后0x10个无关字节
};

// 解析FSINFO扇区的数据
struct fat32_fsinfo{
	union {
		u8								buf[BYTES_PER_SECT];		// FSINFO扇区的缓冲区
		struct fat32_fsinfo_resolution	resolution;					// 解析出来的数据
	} data;
};

struct fat32_direntry_resolution {
	// 和目录项数据全部对齐
	u8						st_filename[FILENAME_ST_LEN];		// 短文件名加上扩展名
	u8						attr;								// 属性
	u8						rsv;								// 保留字节
	u8						c_ms;								// 创建文件精确时间
	u16						c_time;								// 创建文件时分秒
	u16						c_date;								// 创建文件年月日
	u16						a_date;								// 最后一次访问年月日
	u16						first_clst_hi;						// 数据首簇簇号高16位
	u16						d_time;								// 最后一次修改时分秒
	u16						d_date;								// 最后一次修改年月日
	u16						first_clst_lo;						// 数据首簇簇号低16位
	u32						size;								// 文件大小
};

// 解析目录表项
struct fat32_direntry {
	union {
		u8									buf[DIRENTRY_LEN];		// 目录项缓冲区
		struct fat32_direntry_resolution	resolution;				// 解析出来的数据
	} data;
};

// fat32.c
u32 fat32_init(u32);

u32 fat32_bmap(struct inode *, u32);
u32 fat32_readpage(struct vfs_page * );
u32 fat32_writepage(struct vfs_page* );

u32 fat32_createinode(struct dentry *, u32, struct nameidata *);
struct dentry * fat32_lookup(struct inode *, struct dentry *, u32);
u32 fat32_writeinode(struct inode *, struct dentry *);
u32 fat32_deleteinode(struct dentry *, u32);

u32 fat32_readdir(struct file *, struct getdent *);
u32 fat32_isempty(struct dentry* dentry);

struct address_space* fat32_fill_mapping(struct inode*);
u32 fat32_create_page(struct inode*, u32);

u32 get_direnty_no(struct qstr);
u32 read_dr(struct inode*, u32, struct fat32_direntry*);
u32 write_dr(struct inode*, u32, struct fat32_direntry*);
u32 fat32_apply_direntry(struct nameidata* , struct dentry*, u32);
u32 fat32_lookup_direntry(struct inode* , struct dentry*, struct fat32_direntry* , u32* , u32* , u32);
u32 fat32_delete_direntry(struct inode* , u32, u32);
void fill_direntry(struct dentry* , u32, struct fat32_direntry* , u32, u32, u32);

void write_fat(struct super_block* , u32, u32);
u32 read_fat(struct super_block*, u32);
u32 apply_fat(struct super_block*);
void retake_fat(struct super_block*, u32);
#endif