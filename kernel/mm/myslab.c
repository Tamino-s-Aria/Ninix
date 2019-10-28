#include <arch.h>
#include <driver/vga.h>
#include <zjunix/slab.h>
#include <zjunix/utils.h>
//#define SLAB_DEBUG
#define KMEM_ADDR(PAGE, BASE) ((((PAGE) - (BASE)) << PAGE_SHIFT) | 0x80000000)

/* one list of PAGE_SHIFT(now it's 12) possbile memory size
96, 192, 8, 16, 32, 64, 128, 256, 512, 1024, (2 undefined)
in current stage, set (2 undefined) to be (4, 2048)  */
//struct kmem_cache kmalloc_caches[PAGE_SHIFT];

struct kmem_cache kmalloc_caches[PAGE_SHIFT];
static unsigned int size_kmem_cache[PAGE_SHIFT] = { 96, 192, 8, 16, 32, 64, 128, 256, 512, 1024, 1536, 2048 };

// init the struct kmem_cache_node
void init_kmem_cache_node(struct kmem_cache_node *knode) {
	INIT_LIST_HEAD(&(knode->partial));
	INIT_LIST_HEAD(&(knode->full));
}

// init the struct kmem_cache_cpu
void init_kmem_cache_cpu(struct kmem_cache_cpu *kcpu) {
	kcpu->page = 0;
	//kcpu->freeobj = 0;
}

//init the struct kmem_cache using given size
void init_each_slab(struct kmem_cache *cache, unsigned int size) {
	cache->objsize = size + (SIZE_INT - 1);
	cache->objsize &= ~(SIZE_INT - 1);
	cache->size = cache->objsize + sizeof(void *);  // add one char as mark(available)
	cache->offset = cache->size;
	init_kmem_cache_node(&(cache->node));
	init_kmem_cache_cpu(&(cache->cpu));
}

//initialize the whole slab system
void init_slab()
{
	unsigned int i;
	for (i = 0; i < PAGE_SHIFT; i++)
	{
		init_each_slab(&(kmalloc_caches[i]), size_kmem_cache[i]);
	}
	//slab_info();
}

//formalize the slab page
void format_slabpage(struct kmem_cache *cache, struct page *page) {
	unsigned char *moffset = (unsigned char *)KMEM_ADDR(page, pages);  // physical address
	struct slab_head *s_head = (struct slab_head *)moffset;
	void *ptr = moffset + sizeof(struct slab_head);

	//the page is used by slab system
	set_flag(page, BUDDY_SLAB);

	cache->cpu.page = page;
	page->virtual = (void *)cache;
	page->slabp = 0;   //points to the base-addr of free space   

	s_head->end_ptr = ptr;
	s_head->nr_objs = 0;
	s_head->is_full = 0;

}

//free given slab, and if the page becomes empty, free the page
void slab_free(struct kmem_cache *cache, void *object)
{

	struct page *spage = pages + ((unsigned int)object >> PAGE_SHIFT); //find the page to be freed
	struct slab_head *s_head = (struct slab_head *)KMEM_ADDR(spage, pages);
	unsigned int iffull;
	unsigned int is_full = s_head->is_full;   //record the full situation of s_head
	object = (void*)((unsigned int)object | KERNEL_ENTRY);
#ifdef SLAB_DEBUG
	kernel_printf("page addr:%x object:%x slabp:%x end_ptr:%x full: %d, nr_obj: %x\n",
		spage, object, spage->slabp, s_head->end_ptr, s_head->is_full, s_head->nr_objs);
#endif
	if (!(s_head->nr_objs)) {   //the head points to an empty space
		kernel_printf("ERROR : slab_free error!\n");
		while (1);
	}
	/////////////////////////////////////////////////////////

	iffull = (!spage->slabp) && is_full;  // if the page is full and not used by slab

	*(unsigned int*)object = spage->slabp;
	spage->slabp = (unsigned int)object;
	s_head->nr_objs -= 1;
	//kernel_printf("nrobj: %x", s_head->nr_objs);
	if (is_full)         //if the page was full before, after the free operation, it will not be full
		s_head->is_full = 0;      //set the page to be not full

	//free given page
	if (!(s_head->nr_objs)) {
		//kernel_printf("free\n");
		list_del_init(&(spage->list));
		__free_pages(spage, 0);
		return;
	}

	//there is nothing more to free
	if (list_empty(&(spage->list)))
		return;

	if (iffull) {
		list_del_init(&(spage->list));   //delete from spage
		list_add_tail(&(spage->list), &(cache->node.partial));   //add partial page to the list
	}

}

//allocate a slab 
void *slab_alloc(struct kmem_cache *cache)
{
	void *object = 0;
	struct page *npage;
	struct slab_head *s_head;

	if (!cache->cpu.page)  //no allocated page unit
	{
#ifdef SLAB_DEBUG
		kernel_printf("Page full\n");
#endif  
		if (list_empty(&(cache->node.partial)))  //no partial pages left
		{
			//call buddy system to allocate one more page
			npage = __alloc_pages(0);  //order = 0
			if (!npage)  //allocation failed, no more pages to be used
			{
				kernel_printf("ERROR: memory use up, slab request failed\n");
				while (1)
					;
			}
			//if allocation succeeded, use standard format to shape the new page
			format_slabpage(cache, npage);
			s_head = (struct slab_head *)KMEM_ADDR(npage, pages);

			//page no full
			if (!s_head->is_full) //this newly allocated page has remaining space
			{
				object = s_head->end_ptr;
				s_head->end_ptr = object + cache->size;
				s_head->nr_objs += 1;

				if (s_head->end_ptr + cache->size - (void*)s_head >= 1 << PAGE_SHIFT)//if it is full
				{
					s_head->is_full = 1;
					list_add_tail(&(npage->list), &(cache->node.full));
				}
#ifdef SLAB_DEBUG
		kernel_printf("Page not full\nnr_objs:%d\tobject:%x\tend_ptr:%x\n",
					s_head->nr_objs, object, s_head->end_ptr);
#endif  
				return object;
			}

		}
		//there exists partial pages, get one
		cache->cpu.page = container_of(cache->node.partial.next, struct page, list);
		npage = cache->cpu.page;
		list_del(cache->node.partial.next);
		s_head = (struct slab_head *)KMEM_ADDR(npage, pages);

		//go to freelist
		if (npage->slabp != 0) //allocate from freelist
		{
			object = (void*)npage->slabp;
			npage->slabp = *(unsigned int*)npage->slabp;
			s_head->nr_objs += 1;
			return object;
		}
	}
	else {
		npage = cache->cpu.page;
		s_head = (struct slab_head *)KMEM_ADDR(npage, pages);
		if (npage->slabp != 0)
		{
			object = (void*)npage->slabp;
			npage->slabp = *(unsigned int*)npage->slabp;
			s_head->nr_objs += 1;
			return object;
		}
		
		if (!s_head->is_full) //this newly allocated page has remaining space
		{
			object = s_head->end_ptr;
			s_head->end_ptr = object + cache->size;
			s_head->nr_objs += 1;

			if (s_head->end_ptr + cache->size - (void*)s_head >= 1 << PAGE_SHIFT)//if it is full
			{
				s_head->is_full = 1;
				list_add_tail(&(npage->list), &(cache->node.full));
			}
			return object;
		}
		
	}

}

/*find the best-fit size slab system for
@param size : start page frame number of buddy system
return value: the index of the gotten slab
*/
unsigned int get_slab(unsigned int size)
{
	unsigned int i;
	unsigned int num = (1 << (PAGE_SHIFT - 1));  // half page
	unsigned int index = PAGE_SHIFT;             // record the best fit num & index

												 //find the suitable cache for the slab
	for (i = 0; i < PAGE_SHIFT; i++) {
		if ((kmalloc_caches[i].objsize >= size) && (kmalloc_caches[i].objsize <= num)) {
			num = kmalloc_caches[i].objsize;
			index = i;
		}
	}
	return index;
}

/*allocate block of given size
@param size : block size needed to be allocated
return value
*/
void *kmalloc(unsigned int size) {
	struct kmem_cache *cache;
	unsigned int index;
	void *res;
	if (size == 0)
		return 0;

	// if the size larger than the max size of slab system, then call buddy to
	// solve this
	if (size > kmalloc_caches[PAGE_SHIFT - 1].objsize) {
		size = UPPER_ALLIGN(size, 1 << PAGE_SHIFT);
		return (void *)(KERNEL_ENTRY | (unsigned int)alloc_pages(size >> PAGE_SHIFT));
	}

	//get the index of the suitable slab
	index = get_slab(size);
	if (index >= PAGE_SHIFT) {
		kernel_printf("ERROR: No available slab\n");
		while (1)
			;
	}
	return (void *)(KERNEL_ENTRY | (unsigned int)slab_alloc(&(kmalloc_caches[index])));
}

/*free given object
@param obj : object to be freed
return value
*/
void kfree(void *obj) {
	struct page *page;

	obj = (void *)((unsigned int)obj & (~KERNEL_ENTRY));
	page = pages + ((unsigned int)obj >> PAGE_SHIFT);
	if (!(page->flag == BUDDY_SLAB))
		return free_pages((void *)((unsigned int)obj & ~((1 << PAGE_SHIFT) - 1)), page->bplevel);

	return slab_free(page->virtual, obj);
}

//display information about the whole slab system
void slab_info()
{
	kernel_printf("Setup Slab :\n");
	kernel_printf("\tcurrent slab cache size list:\n\t");
	for (int i = 0; i < PAGE_SHIFT; i++) {
		//if(kmalloc_caches[i].cpu.page->slabp)
		kernel_printf("%x %x   \n", kmalloc_caches[i].objsize, (unsigned int)(&(kmalloc_caches[i])));
		//kernel_printf("empty or not: %d \n",list_empty(&(kmalloc_caches[i].node.partial))));
		//kernel_printf("%d \n", kmalloc_caches[i].cpu.page->slabp); 
	}
	kernel_printf("\n");
}

//display information about every size of the slab
void slab_m_info() {
	kernel_printf("More infos about slab:\n");
	struct page *npage;
	struct slab_head *s_head;
	for (int i = 0; i < PAGE_SHIFT; i++) {
		npage = kmalloc_caches[i].cpu.page;
		s_head = (struct slab_head *)KMEM_ADDR(npage, pages);
		if (!npage)
			kernel_printf("page size[%d]: nr_onjs 0, end_ptr: %x, full or not: 0 \n", size_kmem_cache[i], s_head->end_ptr, s_head->is_full);
		else
			kernel_printf("page size[%d]: nr_objs %x, end_ptr: %x, full or not: %d \n", size_kmem_cache[i], s_head->nr_objs, s_head->end_ptr, s_head->is_full);
	}
	//kernel_printf("slabHead size: %x\n", sizeof(struct slab_head));
	kernel_printf("\n");
}

void slab_c_info() {
	struct page *npage;
	struct slab_head *s_head;
	npage = kmalloc_caches[6].cpu.page;
	s_head = (struct slab_head *)KMEM_ADDR(npage, pages);
	kernel_printf("for page size[128]: ");
	kernel_printf("nr_objs %d, end_ptr: %x, full or not: %d \n", s_head->nr_objs, s_head->end_ptr, s_head->is_full);
}

//display information for size 128
void slab_d_info() {
	struct page *npage;
	struct slab_head *s_head;
	npage = kmalloc_caches[6].cpu.page;
	s_head = (struct slab_head *)KMEM_ADDR(npage, pages);
	if (!npage)
		kernel_printf("page size[128] has no page noww");
	else {
		kernel_printf("for page size[128]: ");
		kernel_printf("nr_objs %d, slabp: %x, full or not: %d \n", s_head->nr_objs, npage->slabp, s_head->is_full);
	}
}
