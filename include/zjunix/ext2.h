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

#define			DIR_NORMFILE				0X1		//Ŀ¼���б�ʾ��ͨ�ļ�����ֵ
#define			DIR_DIRECTORY				0x2		//Ŀ¼���б�ʾĿ¼�ļ�����ֵ
#define			INODE_NORMFILE				0x8000	//�����ڵ��б�ʾ��ͨ�ļ�����ֵ
#define			INODE_DIRECTORY				0x4000	//�����ڵ��б�ʾĿ¼�ļ�����ֵ

#define			test						kernel_printf

//λͼ�ṹ�������˿�λͼ�������ڵ�λͼ
struct ext2_bitmap {
	u32					bg;							//����
	u32					dirty;						//�����
	u8					*blockbit;					//��λͼ
	u8					*inodebit;					//�����ڵ�λͼ
};

// ��������������ṹ
struct ext2_dptentry_resolution {
	u32					block_bitmap_base;			//�����������λͼ�Ŀ��
	u32					inode_bitmap_base;			//��������������ڵ�Ŀ��
	u32					inode_table_base;			//��������������ڵ��Ŀ��
	u16					unalloc_blocks;				//���������δ����Ŀ���
	u16					unalloc_inodes;				//���������δ����������ڵ���
	u16					dirs;						//�������Ŀ¼�ļ�������
	u8					rsv[14];					//δʹ��
};

// ����������ṹ
struct ext2_dptentry {
	union {
		u8				buf[DESCRIPTOR_ENTRY_LEN];
		struct ext2_dptentry_resolution
						resolution;
	}data;
};

// Ŀ¼������ļ�����Ľṹ
struct ext2_direntry_meta {
	u32					ino;						//�����ڵ���
	u16					total_len;					//�˱����ܳ���
	u8					name_len;					//�ļ�������
	u8					type;						//�ļ�����
};

// ������Ŀ¼����ṹ
struct ext2_direntry {
	struct ext2_direntry_meta	meta;
	u8							filename[FILENAME_MX_LEN];
};

// �����ڵ�����ṹ
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

// �����ڵ�ṹ (ע�⣺����ֻ����һ�������ڵ��¼��ǰ128���ֽڣ�)
struct ext2_inode {
	union {
		u8				buf[INODE_SIZE_VALID];
		struct ext2_inode_resolution
						resolution;
	}data;
};

//�����ڵ�������(��һ��Ϊ��λ��д)
struct ext2_itable_block {
	u32					group;				//����
	u32					block;				//����ڵĿ�ƫ��
	u32					dirty;				//�����
	u8					*buf;				//����
};

// EXT2�ļ�ϵͳ��Ԫ��Ϣ����Ϊͨ�ó����������
struct ext2_ext_sb {
	u32					total_inodes;				//�����ļ�ϵͳ�������ڵ�����
	u32					total_blocks;				//�����ļ�ϵͳ�Ŀ���(�˴��Ŀ�����ΪFAT�еĴ�)
	u32					total_groups;				//�����ļ�ϵͳ�������(����ֱ�ӴӴ��̵�֪)
	u32					total_unalloc_inodes;		//�����ļ�ϵͳ����δ����������ڵ�����
	u32					total_unalloc_blocks;		//�����ļ�ϵͳ����δ����Ŀ�����
	u32					blocks_per_group;			//ÿ�������ж��ٸ���
	u32					sects_per_block;			//ÿ������ж��ٸ�����(��Ҫ����õ�)
	u32					inodes_per_group;			//ÿ�������ж��ٸ������ڵ�
	u16					inode_size;					//ÿ�������ڵ�ĳ���(���ֽڼ���)
	u32					inode_table_blks;			//ÿ�����������ڵ����ռ����
	u32					descriptor_block;			//������������ڵĿ��(1����2����Ҫ����õ�)
	struct ext2_bitmap	bitmap;						//����λͼ
	struct ext2_itable_block
		itable;						//�����ڵ������
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