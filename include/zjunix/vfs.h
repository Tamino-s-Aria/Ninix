#ifndef _ZJUNIX_VFS_H
#define _ZJUNIX_VFS_H

#include <zjunix/type.h>
#include <zjunix/err.h>
#include <zjunix/errno.h>
#include <zjunix/list.h>

#define         MAX_PARTITION					4						// 最大分区数
#define			BYTES_PER_SECT					512						// 一个扇区的字节数
#define			FILENAME_MX_LEN					255						// 文件名最大允许长度

//vfs_open函数的参数，表示文件的打开方式
#define O_RDONLY	                            0x0000                  // 为读而打开
#define O_WRONLY	                            0x0001                  // 为写而打开
#define O_RDWR		                            0x0002                  // 为读和写打开
#define O_APPEND	                            0x0003                  // 总是在文件末尾写
#define O_MASK									0x0003					// 打开方式掩码
#define O_DIR									0x0004					// 打开的是目录
#define O_CREATE								0x0008					// 可以新建

//nameidata->flags，帮助函数确定具体用哪种情况进行处理
#define LOOKUP_DIRECTORY						0x0001					//要查找的最后一个分量是文件/目录
#define LOOKUP_CREATE							0x0002					//以创建/只读模式寻找

//file->mode，打开文件之后可查的文件读写属性
#define FMODE_READ		                        0x1                     // 文件为读而打开
#define FMODE_WRITE		                        0x2                     // 文件为写而打开
#define FMODE_LSEEK		                        0x4                     // 文件可以寻址

//nameidata->last_type，帮助函数确定将要创建的文件类型
enum last_type { LAST_NORM,
				 LAST_DIR,
				 LAST_ROOT,
				 LAST_DOT,
				 LAST_DOTDOT};

// 定义详见zjunix/vfscache.h
struct vfs_page;

// 记录文件系统类型信息
struct file_system_type {
    u8                                  *name;                  // 名称
};

// 超级块
struct super_block {
	u32									s_base;					// 该文件系统首扇区的绝对扇区号
    u8                                  s_dirt;                 // 修改标志
    u32                                 s_blksize;              // 以字节为单位的块大小
    struct file_system_type             *s_type;                // 文件系统类型
    struct dentry                       *s_root;                // 文件系统根目录的目录项对象
    struct super_operations             *s_op;                  // 超级块的操作函数
    void                                *s_fs_info;             // 指向特定文件系统的超级块信息的指针
};

// 挂载信息
struct vfsmount {
	struct list_head                    mnt_hash;               // 实际只有双向链表的功能
	struct vfsmount                     *mnt_parent;	        // 指向父文件系统， 这个文件安装在其上
	struct dentry                       *mnt_mountpoint;        // 指向这个文件系统安装点在其父目录中的dentry
	struct dentry                       *mnt_root;              // 指向这个文件系统根目录的dentry
	struct super_block                  *mnt_sb;                // 指向这个文件系统的超级块对象
};

// 已缓存的页
struct address_space {
    u32                                 a_pagesize;             // 页大小(字节)
    u32                                 *a_page;                // 文件页到逻辑页的映射表
    struct inode                        *a_host;                // 相关联的inode
    struct list_head                    a_cache;                // 已缓冲的页链表
    struct address_space_operations     *a_op;                  // 操作函数
};

// 文件节点
struct inode {
    u32                                 i_ino;                  // 索引节点号
    u32                                 i_count;                // 当前的引用计数
    u32                                 i_blocks;               // 块数
    u32                                 i_blksize;              // 块的字节数
    u32                                 i_size;                 // 对应文件的字节数
    struct list_head                    i_hash;                 // 用于散列链表的指针
    struct list_head                    i_LRU;                  // 用于LRU链表的指针
    struct list_head                    i_dentry;               // 引用索引节点的目录项链表的头
    struct inode_operations             *i_op;                  // 操作函数
    struct file_operations              *i_fop;                 // 对应的文件操作函数
    struct super_block                  *i_sb;                  // 指向超级块对象的指针
    struct address_space                *i_data;                // 文件的地址空间对象
};

// 字符串包装
struct qstr {
    u8									*name;                  // 字符串
    u32                                 len;                    // 长度
};

// 目录项
struct dentry {
    u32                                 d_count;                // 当前的引用计数
    u32                                 d_pinned;               // （额外）是否被锁定（一般为根目录）
    u32                                 d_mounted;              // 对目录而言，记录安装该目录项的文件系统数的计数器
    struct inode                        *d_inode;               // 与文件名相关的索引节点
    struct dentry                       *d_parent;              // 父目录的目录项对象
    struct qstr                         d_name;                 // 文件名
	struct list_head                    d_hash;                 // 指向散列表表项的指针
    //struct list_head                    d_LRU;                  // 用于未使用目录项链表的指针
    struct list_head                    d_child;                // 对目录而言，用于同一父目中目录项链表的指针
    struct list_head                    d_subdirs;              // 对目录而言，子目录项链表的头
    struct list_head                    d_alias;                // 用于与同一索引节点相关的目录项链表的指针
    struct dentry_operations            *d_op;                  // 目录项方法
    struct super_block                  *d_sb;                  // 文件的超级块对象
};

// 打开的文件
struct file {
    u32                                 f_pos;                  // 文件当前的读写位置
	struct dentry		                *f_dentry;              // 与文件相关的目录项对象
	struct vfsmount                     *f_vfsmnt;              // 含有该文件的已安装文件系统
	struct file_operations	            *f_op;                  // 指向文件操作表的指针
	u32 		                        f_flags;                // 当打开文件时所指定的标志
	u32			                        f_mode;                 // 进程的访问方式
	struct address_space	            *f_mapping;             // 指向文件地址空间对象的指针
};

// 打开用
struct open_intent {
    u32	                                flags;
	u32	                                create_mode;
};

// 寻找目录用结构一
struct nameidata {  
    struct dentry                       *dentry;                // 对应目录项
    struct vfsmount                     *mnt;                   // 对应文件系统挂载项
    struct qstr                         last;                   // 最后一个分量的名字，仅用于创建模式
    u32                                 flags;                  // 状态标记
    u32                                 last_type;              // 最后一个分量的表达形式，仅用于创建模式
    union {                                                     // 打开用
		struct open_intent open;
	} intent;
};

// 寻找目录用结构二
struct path {
  	struct vfsmount                     *mnt;                   // 对应目录项
  	struct dentry                       *dentry;                // 对应文件系统挂载项
};

// 通用目录项信息
struct dirent {
    u32                                 ino;                    // inode 号
    u32                                 type;                   // 文件类型
    u8									*name;                  // 文件名
};

// 目录项获取结构体
struct getdent {
    u32                                 count;                  // 目录项数
    struct dirent                       *dirent;                // 目录项数组
};

// 超级块的操作函数
struct super_operations {
    // 删除目录项对应的内存中的VFS索引节点和磁盘上文件数据及元数据
    u32 (*delete_inode) (struct dentry *, u32);
    // 用通过传递参数指定的索引节点对象的内容更新一个文件系统的索引节点
    u32 (*write_inode) (struct inode *, struct dentry *);
};

// 打开的文件的操作函数
struct file_operations {
    // 从文件的*offset处开始读出count个字节，然后增加*offset的值（一般与文件指针对应）
    u32 (*read) (struct file *, u8 *, u32);
    // 从文件的*offset处开始写入count个字节，然后增加*offset的值（一般与文件指针对应）
    u32 (*write) (struct file *, u8 *, u32);
    // 通过创建一个新的文件对象而打开一个文件，并把它链接到相应的索引节点对象
    u32 (*flush) (struct file *);
    // 读取一个目录的目录项并放入第二个参数中
    u32 (*readdir) (struct file *, struct getdent *);
};

// 文件节点的操作函数
struct inode_operations {
    // 在某一目录下，为与目录项相关的文件创建一个新的磁盘索引节点
    u32 (*create) (struct dentry *, u32, struct nameidata *);
	// 根据父目录的索引节点，寻找并构建dentry对应文件的索引节点，并将dentry和新的索引节点关联起来
    struct dentry * (*lookup) (struct inode *,struct dentry *, u32 type);
};

// 已缓存的页的操作函数
struct address_space_operations {
    // 把一页写回外存
    u32 (*writepage)(struct vfs_page *);
    // 从外存读入一页
    u32 (*readpage)(struct vfs_page *);
	// 为给定的索引节点再申请给定数量的新页
	u32 (*createpage)(struct inode *, u32);
    // 根据由相对文件页号得到相对物理页号
    u32 (*bmap)(struct inode *, u32);
};

// 目录项的操作函数
struct dentry_operations {
    // 比较两个文件名。name1应该属于dir所指的目录。缺省的VFS函数是常用的。不过，每个文件系统可用自己的实现。如MS-DOS文件系统不区分大小写
    u32 (*compare)(const struct qstr *, const struct qstr *);
	// 检查该目录项所对应的目录是否是空的，用于删除目录
	u32 (*isempty)(struct dentry*);
};

// 函数
//vfs.c
void init_vfs(void);

//read_write.c
u32 vfs_read(struct file *file, char *buf, u32 count);
u32 vfs_write(struct file *file, char *buf, u32 count);
u32 generic_file_read(struct file *, u8 *, u32);
u32 generic_file_write(struct file *, u8 *, u32);
u32 generic_file_flush(struct file * );
struct vfs_page* general_find_page(u32 pageNo, struct inode* inode);
u32 generic_check_filename(const struct qstr*, const struct qstr*);

//mount.c
u32 mount_ext2(void);
u32 follow_mount(struct vfsmount **, struct dentry **);

//open.c
struct file* vfs_open(const u8 *, u32, u32);
u32 open_namei(const u8*, u32, u32, struct nameidata*);
u32 path_lookup(const u8 *, u32, struct nameidata *);
u32 do_path_lookup(const u8 *, struct nameidata *);
struct file * dentry_open(struct dentry *, struct vfsmount *, u32);
void follow_dotdot(struct vfsmount **, struct dentry **);
u32 do_lookup(struct nameidata *, struct qstr *, struct path *, u32);
struct dentry* d_alloc(struct qstr*, struct dentry*);
struct dentry * do_lookup_create(struct qstr *, struct dentry *, u32);
u32 vfs_close(struct file *);
void generic_delete_alias(struct inode *inode);

//usr.c
u32 vfs_cat(const u8 *);
u32 vfs_cd(const u8 *);
u32 vfs_ls(const u8 *);
u32 vfs_new(const u8*);
u32 vfs_mkdir(const u8*);
u32 vfs_rm(const u8 *);
u32 vfs_rmdir(const u8*);
u32 vfs_vi(const u8 *);

#endif