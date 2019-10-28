#ifndef _ZJUNIX_VFS_VFSCACHE_H
#define _ZJUNIX_VFS_VFSCACHE_H

#include <zjunix/vfs.h>
#include <zjunix/log.h>
#include <zjunix/list.h>
#include <driver/sd.h>
#include <driver/vga.h>

#define CACHE_HASHSIZE	64
#define CACHE_CAPACITY	256

enum page_status{
	 PAGE_INVALID,	//该页无效，此时p_data指针不指向任何有效数据区
	 PAGE_INTACT,	//该页干净
	 PAGE_DIRTY		//该页有修改
};

enum cache_type {
	DCACHE,
	PCACHE
};

// 文件缓存页
struct vfs_page {
    u8                          *p_data;                    // 数据
    u32                         p_state;                    // 状态
    u32                         p_location;                 // 对应文件系统定义的块地址
    struct list_head            p_hash;                     // 哈希表链表
    struct list_head            p_list;                     // 同一文件已缓冲页的链表
	struct list_head			p_LRU;						// 用于pcache中的替换策略
    struct address_space        *p_mapping;                 // 所属的address_space结构
};

// 缓存
struct cache {
    u32                         c_size;                     // 现有项数
    u32                         c_capacity;                 // 最大项数
    u32                         c_tablesize;                // 哈希值数
    struct list_head            *c_hashtable;               // 指向哈希表的指针
	struct list_head			c_LRU;						// 替换策略所用LRU链表头
    struct cache_operations     *c_op;                      // 指向缓冲区的操作函数指针
};

// 储存查找条件，用于cache的查找
struct condition {
    void    *cond1;
    void    *cond2;
    void    *cond3;
};

// 缓存的操作函数
struct cache_operations {
    // 根据某一条件，用哈希表在缓冲区中查找对应元素是否存在
    void* (*look_up)(struct cache*, struct condition*);
    // 往缓冲区加入一个元素。如果发生缓冲区满，执行使用LRU算法的替换操作
    u32 (*add)(struct cache*, void*);
	// 从缓存区中溢出一个元素并释放之
	u32 (*del)(struct cache*, void*);
    // 判断缓冲区是否满
    u32 (*is_full)(struct cache*);
};

// 下面是函数声明
// vfscache.c
u32 init_cache();
u32 cache_init(struct cache *, u32, u32, u32);
u32 cache_is_full(struct cache *);

void* dcache_look_up(struct cache *, struct condition *);
u32 dcache_add(struct cache *, void *);
u32 dcache_del(struct cache* , void *);

void* pcache_look_up(struct cache *, struct condition *);
u32 pcache_add(struct cache *, void *);
u32 pcache_del(struct cache* , void *);

void dget(struct dentry *);
void dput(struct dentry *);

u32 read_block(u8*, u32, u32);
u32 write_block(u8*, u32, u32);
u8* read_block_ret(u8*, u32, u32);

u32 __intHash(u32, u32);
u32 __stringHash(u32, u8*, u32);

#endif