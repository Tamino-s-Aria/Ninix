#include <driver/vga.h>
#include <zjunix/bootmm.h>
#include <zjunix/buddy.h>
#include <zjunix/list.h>
#include <zjunix/lock.h>
#include <zjunix/utils.h>

unsigned int kernel_start_pfn, kernel_end_pfn;

struct page *pages;
struct buddy_sys buddy;

/*initialize page struct for all memory
@param start_pfn : start page frame number of buddy systm
@param end_pfn : end page frame number of buddy systm
*/
void init_pages(unsigned int start_pfn, unsigned int end_pfn)
{
	unsigned int i;
	for (i = start_pfn; i < end_pfn; i++)   //initialize all the values of a page
	{
		set_flag((pages + i), BUDDY_RESERVED);
		(pages + i)->reference = 1;
		(pages + i)->bplevel = 0;
		(pages + i)->slabp = 0;
		(pages + i)->virtual = (void *)(-1);
		INIT_LIST_HEAD(&(pages[i].list));
	}
}

/*free pages with given buddy pair level
@param order : the buddy pair level of the page  */
void __free_pages(struct page *page, unsigned int order)
{
	struct page *friend1_page;
	struct page *friend2_page;
	//page_id: id of the current page
	//group_id: id of the buddy group that current page is in
	unsigned int page_id, friend1_id, friend2_id; //can find two friends
	unsigned int combined_id, temp;

	lockup(&buddy.lock);

	page_id = page - buddy.start_page;


	while (order < BUDDY_MAX_ORDER)
	{
		friend1_id = page_id - pow(2, order);
		friend2_id = page_id + pow(2, order);
		friend1_page = page + (friend1_id - page_id);
		friend2_page = page + (friend2_id - page_id);
	#ifdef BUDDY_DEBUG
	kernel_printf("page: %x, friend1: %x, friend2: %x", page_id, friend1_id, friend2_id);
	#endif
	if (!is_same_level(friend1_page, page_id) && !is_same_level(friend2_page, page_id))
		break;
	if (is_same_level(friend1_page, page_id))
	{
		if (friend1_page->flag != BUDDY_FREE) //this page is not free
			break;
		//delete from freelist
		list_del_init(&friend1_page->list);
		buddy.freelist[order].nr_free--;
		//reset the order of the combined block
		set_level(page, -1);
		page = friend1_page;
		page_id = friend1_id;
		order++;
	}
	if (is_same_level(friend2_page, page_id))
	{
		if (friend2_page->flag != BUDDY_FREE) //this page is not free
			break;
		//delete from freelist
		list_del_init(&friend2_page->list);
		buddy.freelist[order].nr_free--;
		//reset the order of the combined block
		set_level(friend2_page, -1);
		//page = friend1_page;
		//page_id = friend1_id;
		order++;
	}
	set_level(page, order);
	set_flag(page, BUDDY_FREE);
	list_add(&(page->list), &(buddy.freelist[order].free_head));
	buddy.freelist[order].nr_free++;
	unlock(&buddy.lock);
	}

}

/*allocate pages from given buddy pair level
@param level : the buddy pair level of the page
*/
struct page *__alloc_pages(unsigned int level)
{
	unsigned int clevel, size;
	struct page *page, *buddy_page;
	struct freelist *flist;

	lockup(&buddy.lock);

	//search for free pages
	for (clevel = level; clevel <= BUDDY_MAX_ORDER; clevel++)
	{
		flist = buddy.freelist + clevel;
		if (!list_empty(&flist->free_head))  //find the smallest buddy pair level
		{
			page = container_of(flist->free_head.next, struct page, list);
			list_del_init(&(page->list));
			set_level(page, level);
			set_flag(page, BUDDY_ALLOCATED);    //allocate the page
			flist->nr_free--;
			size = pow(2, clevel);

			//there exists free part
			while (clevel > level)
			{
				flist--;
				clevel--;
				size = size / 2;
				buddy_page = page + size;
				//add the remaining part to free list
				list_add(&(buddy_page->list), &(flist->free_head));
				flist->nr_free++;
				set_flag(buddy_page, BUDDY_FREE);
				set_level(buddy_page, clevel);
			}
			unlock(&buddy.lock);
			return page;
		}

	}

	//if free pages not found
	unlock(&buddy.lock);
	return 0;
}

/*free pages with given address
@param order : the buddy pair level of the page  */
void free_pages(void *addr, unsigned int order)
{
	__free_pages(pages + ((unsigned int)addr >> PAGE_SHIFT), order);
}

/*allocate pages with given buddy pair level
@param order : the buddy pair level of the page  */
void *alloc_pages(unsigned int order)
{
	unsigned int level = 0;
	//find the maximum qualified buddy pair level
	if (!order)
		return 0;
	while (1 << level < order)
		level++;

	//allocate pages using __alloc_pages() function
	struct page *page = __alloc_pages(level);

	if (!page)
		return 0;
	return (void *)((page - pages) << PAGE_SHIFT);
}


/* display infos of buddy, used for debugging*/
void buddy_info()
{
	unsigned int index;
	kernel_printf("Buddy-system :\n");
	kernel_printf("\tstart page-frame number : %x\n", buddy.buddy_start_pfn);
	kernel_printf("\tend page-frame number : %x\n", buddy.buddy_end_pfn);
	for (index = 0; index <= BUDDY_MAX_ORDER; ++index) {
		kernel_printf("\t(%x)# : %x frees\n", index, buddy.freelist[index].nr_free);
	}
}

/* initialize the whole buddy system*/
void init_buddy()
{
	//kernel_printf("enter init buddy\n");
	unsigned int bpsize = sizeof(struct page);
	unsigned char *bp_base;
	unsigned int i;

	bp_base = bootmm_alloc_pages(bpsize * bmm.max_pfn, _MM_KERNEL, 1 << PAGE_SHIFT);
	if (!bp_base) {  //no enough memory to initialize buddy
		kernel_printf("\nERROR : bootmm_alloc_pages failed!\nNo enough memory!\n");
		while (1);
	}
	// initialize free lists for all buddy orders
	for (i = 0; i < BUDDY_MAX_ORDER; i++) {
		buddy.freelist[i].nr_free = 0;
		INIT_LIST_HEAD(&(buddy.freelist[i].free_head));
	}

	//kernel_printf("buddy base address is %d\n", (unsigned int)bp_base);

	// Get virtual address for pages array
	pages = (struct page *)((unsigned int)bp_base | 0x80000000);

	init_pages(0, bmm.max_pfn);  //initialize the pages to be used  //bmm.max_pfn = 32768
	kernel_start_pfn = 0;
	kernel_end_pfn = 0;
	//kernel_printf("bmm.cntinfos = %d\n", bmm.cnt_infos);  //cnt_infos = 1
	for (i = 0; i < bmm.cnt_infos; i++) {
		unsigned int end_pfn = bmm.info[i].end >> PAGE_SHIFT;
		if (end_pfn > kernel_end_pfn)     ////
			kernel_end_pfn = end_pfn;
	}

	// Buddy system occupies the space after kernel part
	buddy.buddy_start_pfn = (kernel_end_pfn + (1 << BUDDY_MAX_ORDER) - 1) &
		~((1 << BUDDY_MAX_ORDER) - 1);
	// remain 2 pages for I/O
	buddy.buddy_end_pfn = bmm.max_pfn & ~((1 << BUDDY_MAX_ORDER) - 1);

	//buddy_info();

	buddy.start_page = pages + buddy.buddy_start_pfn;
	init_lock(&(buddy.lock));

	//free the pages in buddy system first
	for (i = buddy.buddy_start_pfn; i < buddy.buddy_end_pfn; ++i) {
		__free_pages(pages + i, 0);
	}
	//buddy_info();
}

/* display some useful information about buddy*/
void buddy_n_info() {
	unsigned int index;
	kernel_printf("\tstart pfn : %x\n", buddy.buddy_start_pfn);
	kernel_printf("\tend page-frame number : %x\n", buddy.buddy_end_pfn);
	for (index = 0; index <= BUDDY_MAX_ORDER; ++index) {
		kernel_printf("%x,  ", buddy.freelist[index].nr_free);
	}
	kernel_printf("\n");
	kernel_printf("   ");
}



