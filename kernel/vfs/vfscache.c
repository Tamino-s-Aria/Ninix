#include <zjunix/vfscache.h>
#include <zjunix/slab.h>
#include <zjunix/errno.h>

struct cache		*dcache;	//dentry缓存
struct cache		*pcache;	//page缓存

//对通用缓存的操作函数进行实例化
struct cache_operations dentry_cache_operations = {
	.look_up = dcache_look_up,
	.add = dcache_add,
	.del = dcache_del,
	.is_full = 0,		//dcache没有最大容量的概念，也不存在满不满
};


struct cache_operations page_cache_operations = {
	.look_up = pcache_look_up,
	.add = pcache_add,
	.del = pcache_del,
	.is_full = cache_is_full,
};

//分别对各种全局cache进行初始化，返回非零错误
u32 init_cache(void){
	dcache = (struct cache*)kmalloc(sizeof(struct cache));
	if (dcache == 0)
		goto bad_out;
	pcache = (struct cache*)kmalloc(sizeof(struct cache));
	if (pcache == 0)
		goto bad_out;

	if (cache_init(dcache, CACHE_HASHSIZE, CACHE_CAPACITY, DCACHE) != 0) {
		goto bad_out;
	}
	if (cache_init(pcache, CACHE_HASHSIZE, CACHE_CAPACITY, PCACHE) != 0) {
		goto bad_out;
	}

	return 0;

bad_out:
	if(dcache)
		kfree(dcache);
	if(pcache)
		kfree(pcache);
	return -ENOMEM;
}

u32 read_block(u8* buf, u32 base, u32 cnt) {
	return sd_read_block(buf, base, cnt);
}

//调试用
u8* read_block_ret(u8* buf, u32 base, u32 cnt) {
	u32 err;
	kernel_printf("Welcome to read_block_ret()\n");
	err = sd_read_block(buf, base, cnt);
	kernel_printf("in read_block_ret():\n");
	for (u32 x = 0x40; x < 0x60; x++) {
		kernel_printf("%c", buf[x]);
	}
	kernel_printf("\nerr: %d buf: %d\n", err, (u32)buf);
	return err ? 0 : buf;
}

u32 write_block(u8* buf, u32 base, u32 cnt) {
	return sd_write_block(buf, base, cnt);
}

u32 cache_init(struct cache *cache, u32 hashsize , u32 capacity, u32 type) {
	u32 i;
	cache->c_capacity = capacity;
	cache->c_tablesize = hashsize;
	cache->c_size = 0;
	cache->c_op = (type == DCACHE) ? &dentry_cache_operations : &page_cache_operations;
	cache->c_hashtable = (struct list_head*)kmalloc(sizeof(struct list_head) * CACHE_HASHSIZE);
	INIT_LIST_HEAD(&(cache->c_LRU));
	if (cache->c_hashtable == 0)
		return -1;
	for (i = 0; i < CACHE_HASHSIZE; i++) {
		INIT_LIST_HEAD(&(cache->c_hashtable[i]));
	}

	//正常情况下从这里返回
	return 0;
}

u32 cache_is_full(struct cache *cache){
	return (cache->c_size == cache->c_capacity);
}

void* dcache_look_up(struct cache *dcache, struct condition *cond){
	struct dentry		*parent;
	struct dentry		*dentry;
	struct qstr			*name;
	struct list_head	*head;
	struct list_head	*pos;
	u32 hash_value;
	u32 found;

	name = (struct qstr*)(cond->cond1);
	parent = (struct dentry*)(cond->cond2);
	hash_value = __stringHash(name->len, name->name, dcache->c_tablesize);
	head = &dcache->c_hashtable[hash_value];

	found = 0;
	list_for_each(pos, head) {
		dentry = container_of(pos, struct dentry, d_hash);
		//匹配条件1：文件名字匹配
		if (dentry->d_op->compare(&dentry->d_name, name) != 0)
			continue;
		//匹配条件2：父目录项匹配
		if (dentry->d_parent != parent)
			continue;

		found = 1;
		break;
	}
	return ((found) ? (void*)dentry : (void*)0);
}

u32 dcache_add(struct cache *dcache, void *d){
	struct dentry		*dentry;
	struct dentry		*deldentry;
	struct qstr			*name;
	struct list_head	*head;
	struct list_head	*pos;
	u32 hash;
	u32 err;

	dentry = (struct dentry*)d;
	hash = __stringHash(dentry->d_name.len, dentry->d_name.name, dcache->c_tablesize);
	head = &dcache->c_hashtable[hash];
	//再次强调，dcache没有最大容量一说，没有替换一说，只有当某dentry没有被其他数据结构指向的时候
	//会被删除并释放掉

	//为了保证功能独立性，该函数不对dentry的其他链表做修改
	list_add(&(dentry->d_hash), head);
	//list_add_tail(&(dentry->d_LRU), &(dcache->c_LRU));
	dcache->c_size++;
	return 0;
}

u32 dcache_del(struct cache* dcache, void *d) {
	struct dentry		*deldentry;

	deldentry = (struct dentry*)d;
	list_del(&(deldentry->d_hash));
	//list_del(&(deldentry->d_LRU));
	list_del(&(deldentry->d_child));
	list_del(&(deldentry->d_alias));
	list_del(&(deldentry->d_subdirs));

	//kernel_printf("kfree a dentry name:%s; ino:%d\n", deldentry->d_name, deldentry->d_inode->i_ino);
	kfree(deldentry);
	/*kernel_printf("kfree dentry done\n");*/

	dcache->c_size--;
	return 0;
}

void* pcache_look_up(struct cache *pcache, struct condition *cond){
	struct inode		*inode;
	struct vfs_page		*page;
	struct list_head	*head;
	struct list_head	*pos;
	u32 hash_value;
	u32 ppn;
	u32 found;

	ppn = *((u32*)cond->cond1);
	inode = (struct inode*)cond->cond2;

	//找到哈希值对应的那一个bucket
	hash_value = __intHash(ppn, pcache->c_tablesize);
	
	//kernel_printf("cond.inode: %d ", (u32)inode);
	head = &pcache->c_hashtable[hash_value];

	//对该bucket进行遍历查找
	found = 0;
	list_for_each(pos, head) {
		page = container_of(pos, struct vfs_page, p_hash);
		//kernel_printf("page.inode: %d\n", (u32)page->p_mapping->a_host);
		//匹配条件1：相对物理页号一致
		if (page->p_location != ppn)
			continue;
		//匹配条件2：该页有效
		if (page->p_state == PAGE_INVALID)
			continue;
		//匹配条件3：该页所隶属的索引节点与参数匹配(因为考虑到多个文件系统之间的区分)
		if (page->p_mapping->a_host != inode)
			continue;

		//若以上条件均满足，则该页为想要的那一页
		found = 1;

		//更新LRU链表
		list_move_tail(&(page->p_LRU), &(pcache->c_LRU));
		break;
	}
	return ((found) ? (void*)page : (void*)0);
}

u32 pcache_add(struct cache *pcache, void *p){
	struct vfs_page			*page;
	struct vfs_page			*delpage;
	struct address_space	*mapping;
	struct list_head		*head;
	struct list_head		*pos;
	u32 err;
	u32 hash;
	
	page = (struct vfs_page*)p;
	hash = __intHash(page->p_location, pcache->c_tablesize);
	//kernel_printf("inode addr: %d\n", (u32)page->p_mapping->a_host);
	head = &pcache->c_hashtable[hash];

	if (pcache->c_op->is_full(pcache)) {
		//如果缓存容量已满，则用LRU策略实行替换
		pos = pcache->c_LRU.next;
		delpage = container_of(pos, struct vfs_page, p_LRU);
		err = pcache_del(pcache, (void*)delpage);
		if (err)
			return err;
	}

	list_add(&(page->p_hash), head);
	list_add_tail(&(page->p_LRU), &(pcache->c_LRU));
	pcache->c_size++;
	return 0;
}

u32 pcache_del(struct cache* pcache, void *p) {
	struct vfs_page			*delpage;
	struct address_space	*mapping;
	u32 err;

	delpage = (struct vfs_page*)p;
	if (delpage->p_state != PAGE_INVALID) {
		//如果该页还有效，则执行写回外存的操作
		mapping = delpage->p_mapping;
		err = mapping->a_op->writepage(delpage);
		if (err)
			return -EIO;
	}

	//把选中的这一页剔出缓存并释放
	list_del(&(delpage->p_hash));
	list_del(&(delpage->p_LRU));
	if (delpage->p_data)
		kfree(delpage->p_data);
	kfree(delpage);

	pcache->c_size--;
	return 0;
}

void dget(struct dentry *dentry){
	dentry->d_count++;
	return;
}

void dput(struct dentry *dentry){
	dentry->d_count--;
	if (dentry->d_count == 0 && dentry->d_pinned == 0) {
		//若该目录项已经没有被任何对象引用，又没有被锁定，则把它移出缓存
		dcache->c_op->del(dcache, (void*)dentry);
	}
	return;
}

u32 __intHash(u32 i, u32 hashsize){
	return (i % hashsize);
}

u32 __stringHash(u32 len, u8 *s, u32 hashsize){
	u32 sum;
	u32 n;
	u32 i;
	
	sum = 0;
	n = (len < 24) ? len : 24;

	for (i = 0; i < n; i++) {
		sum = sum << 1;
		sum += (u32)(s[i]);
	}

	return sum % hashsize;
}
