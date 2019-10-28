#include <arch.h>
#include <driver/vga.h>
#include <zjunix/bootmm.h>
#include <zjunix/utils.h>

struct bootmm bmm;
unsigned char bootmmmap[MACHINE_MMSIZE >> PAGE_SHIFT];
unsigned int firstusercode_start;
unsigned int firstusercode_len;
char *memType[] = { "Kernel use", "Mm Bitmap", "Vga Buffer", "Kernel page directory", "Kernel page table", "Dynamic", "Reserved" };



/*set the content of struct bootmm_info
@param start : the start number of mminfo
@param end : the end number of mminfo
@param type : usage of mminfo (eg:kernel use)
*/
void set_mminfo(struct bootmm_info *info, unsigned int start, unsigned int end, unsigned int type)
{
	info->start = start;
	info->end = end;
	info->type = type;
}

/*insert one mminfo and do merging if needed
return value is enum type which stands for the insertion state
*/
enum insertion_state insert_mminfo(struct bootmm *mm, unsigned int start, unsigned int end, unsigned int type)
{
	unsigned int i, j;
	unsigned int num = mm->cnt_infos;
	if (num == MAX_INFO)   //member of bootmm has already reached the maximum value
		return FAILED;				//thus cannot insert any more
	for (i = 0; i < num; i++)
	{
		//kernel_printf("i = 0\n");
		if (mm->info[i].type != type)	//if the types do not match
			continue;					//go to the next part
		if (mm->info[i].end == start - 1)  //the new part connects to the forward part
		{
			if (i == num - 1)   //it is the last part
			{
				mm->info[i].end = end;
				return AFTER_LAST_CONNECTION;
			}
			else
			{
				if (mm->info[i + 1].start - 1 == end) //the new part can also connect to the backward part
				{
					mm->info[i].end = mm->info[i + 1].end;
					mm->cnt_infos -= 1;
					return TWO_WAY_CONNECTION;
				}
				else if (mm->info[i + 1].start <= end)//the new part is bigger than space available
					return FAILED;
				else   //the new part is only forward connecting
				{
					mm->info[i].end = end;
					return FORWARD_CONNECTION;
				}
			}
		}
		if (mm->info[i].end < start - 1 && mm->info[i + 1].start - 1 > end) //the new part does not connect to either part
		{
			mm->cnt_infos += 1;
			for (j = mm->cnt_infos - 2; j >= i + 1; j--)
			{
				set_mminfo(mm->info + j, mm->info[j].start, mm->info[j].end, mm->info[j].type);
			}
			set_mminfo(mm->info + i, start, end, type);
			return NON_CONNECTION;
		}
		if (mm->info[i].start - 1 == end) //the new part connects to the backward part
		{
			mm->info[i].start = start;
			return BACKWARD_CONNECTION;
		}
	}
	set_mminfo(mm->info + mm->cnt_infos, start, end, type);
	mm->cnt_infos++;
	return FIRST_INSERTION;  //there is no mminfo before it

}

/*split one sequential memory area into two parts
@param index : the serial number of the area to be splited
return number : 0 - split failed, 1 - split succeeded
*/
unsigned int split_mminfo(struct bootmm* mm, unsigned int index, unsigned int split_start)
{
	split_start &= PAGE_ALIGN;
	unsigned int i, j;
	unsigned int last_end = split_start - 1;
	unsigned int next_start = split_start;
	unsigned int start = mm->info[index].start;
	unsigned int end = mm->info[index].end;
	unsigned int type = mm->info[index].type;

	if (mm->cnt_infos == MAX_INFO) //the capacity has reached it upper limit
		return 0;
	if (split_start <= start || split_start >= end) //split_start is not valid
		return 0;
	if (index >= mm->cnt_infos) //index out of range
		return 0;

	mm->info[index].end = last_end;
	mm->cnt_infos += 1;
	for (i = mm->cnt_infos - 1; i > index + 1; i--) //move the parts backward
		mm->info[i] = mm->info[i - 1];

	//set the values of the new part
	mm->info[index + 1].start = next_start;
	mm->info[index + 1].end = end;
	mm->info[index + 1].type = type;
	return 1;
}

/*remove one part from bootmm
@param index : the serial number of the mminfo to be removed
*/
void remove_mminfo(struct bootmm* mm, unsigned int index)
{
	unsigned int i;
	if (index >= mm->cnt_infos)  //index out of range
		return;

	if (index == mm->cnt_infos - 1) //the part to be removed is the last part
	{
		mm->cnt_infos -= 1;
		return;
	}

	for (i = index; i < mm->cnt_infos - 1; i++)
	{
		mm->info[i] = mm->info[i + 1];
	}
	mm->cnt_infos -= 1;
}

/* initialize bootmm*/
void init_bootmm()
{
	unsigned int index;
	unsigned char *t_map;
	unsigned int end;
	end = 16 * 1024 * 1024;  //16MB for kernel
	kernel_memset(&bmm, 0, sizeof(bmm));
	bmm.phymm = get_phymm_size();      //physical address
	bmm.max_pfn = bmm.phymm >> PAGE_SHIFT;   //physical page number, 32768 pages 
	bmm.s_map = bootmmmap;    //32768 bits
	bmm.e_map = bootmmmap + sizeof(bootmmmap);
	bmm.cnt_infos = 0;
	kernel_memset(bmm.s_map, PAGE_FREE, sizeof(bootmmmap));  //set those pages to be free
	insert_mminfo(&bmm, 0, (unsigned int)(end - 1), _MM_KERNEL);
	bmm.last_alloc_end = (((unsigned int)(end) >> PAGE_SHIFT) - 1);

	for (index = 0; index<end >> PAGE_SHIFT; index++) {
		bmm.s_map[index] = PAGE_USED;
	}
}

/*set value of the page bit map
@param s_pfn : start page frame node
@param cnt : number of pages to be set
@param value : the value to be set
*/
void set_maps(unsigned int s_pfn, unsigned int cnt, unsigned char value)
{
	while (cnt)
	{
		bmm.s_map[s_pfn] = value;
		cnt--;
		s_pfn++;
	}
}

/*find sequential pages that can be allocated
@param page_cnt : number of pages to be allocated
@param s_pfn : page frame node of the start page
@param e_pfn : page frame node of the end page
return number : 0 - pages not found , others - start number of the qualified pages
*/
unsigned char* find_pages(unsigned int page_cnt, unsigned int s_pfn, unsigned int e_pfn, unsigned int align_pfn)
{
	unsigned int i, temp;
	unsigned int cnt;

	s_pfn = (s_pfn + (align_pfn - 1)) & (~align_pfn - 1);

	for (i = s_pfn; i < e_pfn; )
	{
		if (bmm.s_map[i] == PAGE_USED)
		{
			i++;
			continue;
		}
		temp = i;
		cnt = page_cnt;

		while (cnt)
		{
			if (temp >= e_pfn) //reach the end, still not find qualified pages
				return 0;

			if (bmm.s_map[temp] == PAGE_USED)  //reach a used page
				break;

			if (bmm.s_map[temp] == PAGE_FREE)  //the page is free
			{
				temp++;
				cnt--;
			}
		}

		if (cnt == 0)  //qualified pages have been found
		{
			bmm.last_alloc_end = temp - 1; //set the last_alloc_end 
			set_maps(i, page_cnt, PAGE_USED); //set the values of the pages to 'PAGE_USED'
			return (unsigned char *)(i << PAGE_SHIFT);
		}
		else
			i = temp + align_pfn;
	}
	return 0;
}

/*find qualified pages for allocating, then allocate them
@param size : number of pages to be allocated
@param type : usage of the page
@param e_pfn : page frame node of the end page
*/
unsigned char* bootmm_alloc_pages(unsigned int size, unsigned int type, unsigned int align)
{
	unsigned int nsize;
	unsigned char* res;

	size += ((1 << PAGE_SHIFT) - 1);
	size &= PAGE_ALIGN;
	nsize = size >> PAGE_SHIFT;

	//firstly search pages backward the last_alloc_end
	res = find_pages(nsize, bmm.last_alloc_end + 1, bmm.max_pfn, align >> PAGE_SHIFT);
	if (res)
	{
		insert_mminfo(&bmm, (unsigned int)res, (unsigned int)res + size - 1, type);
		return res;
	}

	//if not found, try to search forward pages 
	res = find_pages(nsize, 0, bmm.last_alloc_end, align >> PAGE_SHIFT);
	if (res)
	{
		insert_mminfo(&bmm, (unsigned int)res, (unsigned int)res + size - 1, type);
		return res;
	}
	return 0; //not found
}

/* display module, used for debugging */
void show_bootmap_info(unsigned char* msg)
{
	unsigned int i;
	kernel_printf("%s: ", msg);
	for (i = 0; i < bmm.cnt_infos; i++) {
		kernel_printf(" \t%x-%x : %s\n", bmm.info[i].start, bmm.info[i].end, memType[bmm.info[i].type]);
	}
	//kernel_printf("physical address: %d\n", bmm.phymm);
	//kernel_printf("max page number is %d\n", bmm.max_pfn);
}

void show_bootmm()
{
	kernel_printf("max page number of bootmm is %d\n", bmm.max_pfn);
	kernel_printf("number of infos of bootmm is %d\n", bmm.cnt_infos);
}
