#ifndef _ZJUNIX_BUDDY_H
#define _ZJUNIX_BUDDY_H

#include <zjunix/list.h>
#include <zjunix/lock.h>

#define PAGE_SHIFT 12
#define BUDDY_RESERVED (1 << 31)
#define BUDDY_ALLOCATED (1 << 30)
#define BUDDY_SLAB (1 << 29)
#define BUDDY_FREE 0

//the page struct contains infos about every page in the buddy system
struct page {
	unsigned int flag;       // the declaration of the usage of this page
	unsigned int reference;  //
	struct list_head list;   // double-way list
	void *virtual;           // default 0x(-1)
	unsigned int bplevel;    // the order level of the page  
	unsigned int slabp;      // base-addr of free space if used by slab system, or 0 if not used by slab
};


/*
* order means the size of the set of pages, e.g. order = 1 -> 2^1
* pages(consequent) are free In current system, we allow the max order to be
* 4(2^4 consequent free pages)
*/
#define BUDDY_MAX_ORDER 4

struct freelist {
	unsigned int nr_free;   //number of free blocks
	struct list_head free_head;
};

//infos of the whole buddy system
struct buddy_sys {
	unsigned int buddy_start_pfn; //start page frame node of buddy
	unsigned int buddy_end_pfn;   //end page frame node of buddy
	struct page *start_page; //pages in buddy
	struct lock_t lock;		 //lock that buddy has	
	struct freelist freelist[BUDDY_MAX_ORDER + 1]; //lists indicate corresponding free areas
};

//create some easy-to-execute functions
#define is_same_group(page, bage) (((*(page)).bplevel == (*(bage)).bplevel))
//#define is_same_level(page, lval) ((*(page)).bplevel == lval)
//#define set_level(page, lval) ((*page)).bplevel = lval)
#define is_same_level(page, lval) ((*(page)).bplevel == (lval))
#define set_level(page, lval) ((*(page)).bplevel = (lval))
#define set_flag(page, val) ((*(page)).flag = val)
#define clean_flag(page, val) ((*(page)).flag = 0)
//#define has_flag(page, val) ((*(page)).flag & val)
#define set_ref(page, val) ((*(page)).reference = (val))
#define inc_ref(page, val) ((*(page)).reference += (val))
#define dec_ref(page, val) ((*(page)).reference -= (val))

extern struct page *pages;
extern struct buddy_sys buddy;

extern void init_pages(unsigned int start_pfn, unsigned int end_pfn);
extern void __free_pages(struct page *page, unsigned int order);
extern struct page *__alloc_pages(unsigned int level);
extern void free_pages(void *addr, unsigned int order);
extern void *alloc_pages(unsigned int order);

extern void init_buddy();
extern void buddy_info();
extern void buddy_n_info();

#endif
#pragma once
