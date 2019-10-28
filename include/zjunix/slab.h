#ifndef _ZJUNIX_SLAB_H
#define _ZJUNIX_SLAB_H

#include <zjunix/list.h>
#include <zjunix/buddy.h>

#define SIZE_INT 4
#define SLAB_AVAILABLE 0x0
#define SLAB_USED 0xff

/*slab_head makes the allocation accessible from end_ptr to the end of the page
* @end_ptr : points to the head of the rest of the page
* @nr_objs : keeps the numbers of memory segments that has been allocated
* @is_full: to record whether the page is full
for every page, there exists a slab_head */
struct slab_head {
	void *end_ptr;
	unsigned int nr_objs;
	int is_full;
};

/*
* slab pages is chained in this struct
* @partial keeps the list of un-totally-allocated pages
* @full keeps the list of totally-allocated pages
*/
struct kmem_cache_node {
	struct list_head partial;
	struct list_head full;
};

/*current being allocated page unit */
struct kmem_cache_cpu {
	void **freeobj;
	struct page *page;
};

//for each slab there is a kmem_cache
struct kmem_cache {
	unsigned int size;   //total size of a slab
	unsigned int objsize;  //size of object
	unsigned int offset;					   //unsigned int offset;   
	struct kmem_cache_node node;  //partial lists and full lists
	struct kmem_cache_cpu cpu;  //allocated pages
};

// extern struct kmem_cache kmalloc_caches[PAGE_SHIFT];
extern void init_slab();
extern void *kmalloc(unsigned int size);
extern void *phy_kmalloc(unsigned int size);
extern void kfree(void *obj);
extern void slab_info();
extern void slab_m_info();
extern void slab_c_info();
extern void slab_d_info();
//extern void slab_e_info();

#endif

