#ifndef  _ZJUNIX_VM_H
#define  _ZJUNIX_VM_H

#include <zjunix/page.h>

#define  USER_CODE_ENTRY   0x00100000
#define  USER_DATA_ENTRY   0x01000000
#define  USER_DATA_END
#define  USER_BRK_ENTRY     0x10000000
#define  USER_STACK_ENTRY   0x80000000
#define  USER_DEFAULT_ATTR     0x0f

// extern struct page;

//define every virtual memeory area
struct vma_struct {
	struct mm_struct *vm_mm;
	unsigned long vm_start, vm_end;
	struct vma_struct *vm_next;
};

//define the whole virtual memory area
struct mm_struct {
	struct vma_struct *mmap;        // VMA list
	struct vma_struct *mmap_cache;  // Latest used VMA
	int map_count;                  // number of VMAs

	pgd_t *pgd;                     // Page table entry

	unsigned int start_code, end_code;
	unsigned int start_data, end_data;
	unsigned int start_brk, brk;
	unsigned int start_stack;
};


struct mm_struct* mm_create();
void mm_delete(struct mm_struct* mm);
void pgd_delete(pgd_t* pgd);
void delete_mmap(struct mm_struct* mm);

extern void set_tlb_asid(unsigned int asid);
unsigned long do_map(unsigned long addr, unsigned long len, unsigned long flags);
int do_unmap(unsigned long addr, unsigned long len);
int is_in_vma(unsigned long addr);

/*************VMA*************/
struct vma_struct* find_vma(struct mm_struct* mm, unsigned long addr);
unsigned long get_unmapped_area(unsigned long addr, unsigned long len, unsigned long flags);
struct vma_struct* find_vma_intersection(struct mm_struct* mm, unsigned long start_addr, unsigned long end_addr);
struct vma_struct* find_vma_and_prev(struct mm_struct* mm, unsigned long addr, struct vma_struct** prev);
struct vma_struct* find_vma_prepare(struct mm_struct* mm, unsigned long addr);
void insert_vma_struct(struct mm_struct* mm, struct vma_struct* area);


#endif
