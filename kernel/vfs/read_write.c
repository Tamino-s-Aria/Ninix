#include <zjunix/vfs.h>
#include <zjunix/vfscache.h>

#include <zjunix/slab.h>
#include <zjunix/utils.h>
#include <driver/vga.h>
#include <zjunix/errno.h>

extern struct cache	*pcache;

//虚拟文件系统读接口
u32 vfs_read(struct file *file, char *buf, u32 count) {
	if (file->f_mode & FMODE_READ) {
		return file->f_op->read(file, buf, count);
	}
	//如果该文件没有读权限，直接拒绝读，返回0(实际读了0个字节)
	return 0;
}

//虚拟文件系统写接口
u32 vfs_write(struct file *file, char *buf, u32 count) {
	if (file->f_mode & FMODE_WRITE) {
		return file->f_op->write(file, buf, count);
	}
	//如果该文件没有写权限，直接拒绝写，返回0(实际写了0个字节)
	return 0;
}

//通用读文件方法
//返回实际读了多少个字节
u32 generic_file_read(struct file *file, u8 *buf, u32 count) {
	//假设buf已经申请了足够的空间
	struct vfs_page			*page;
	struct inode			*inode;
	struct address_space	*mapping;
	u32 page_size;
	u32 begin_page;						//开始读取的页
	u32 end_page;						//终止读取的页(自身包含)
	u32 begin_off;						//(页内)开始读取的偏移
	u32 end_off;						//(页内)终止读取的偏移(自身包含)
	u32 vpn;							//当前页的文件内逻辑页号
	u32 ppn;							//当前页的相对物理页号
	u32 len;							//总共读取出来的字节长度

	inode = file->f_dentry->d_inode;
	if (inode == 0)
		return 0;
	mapping = file->f_mapping;
	if (mapping == 0)
		return 0;
	
	len = 0;
	page_size = inode->i_blksize;
	begin_page = file->f_pos / page_size;
	if (file->f_pos + count > inode->i_size) {
		end_page = (inode->i_size - 1) / page_size;
	}
	else {
		end_page = (file->f_pos + count - 1) / page_size;
	}

	for (vpn = begin_page; vpn <= end_page; vpn++) {
		//先对该页的起始读取位置和终止读取位置进行确定
		begin_off = (vpn == begin_page) ? file->f_pos % page_size : 0;
		if (vpn == end_page) {
			if (file->f_pos + count > inode->i_size) {
				end_off = (inode->i_size - 1) % page_size;
			}
			else {
				end_off = (file->f_pos + count - 1) % page_size;
			}
		}
		else {
			end_off = page_size - 1;
		}
		//找到该页的page结构
		ppn = mapping->a_op->bmap(inode, vpn);
		
		if (ppn == -1) {
			goto file_read_end;
		}

		page = general_find_page(ppn, inode);

		if (page == 0 || page->p_data == 0) {
			goto file_read_end;
		}

		//存入buf当中
		kernel_memcpy(buf + len, page->p_data + begin_off, end_off - begin_off + 1);
		len += (end_off - begin_off + 1);
	}
file_read_end:
	//正常情况下len == count，但需要考虑出错的情况
	file->f_pos += len;
	return len;
}

//通用写文件方法
//返回实际写了多少个字节
u32 generic_file_write(struct file *file, u8 *buf, u32 count) {
	//假设buf的数据已经存好
	struct vfs_page			*page;
	struct inode			*inode;
	struct address_space	*mapping;
	u32 page_size;
	u32 begin_page;						//开始写的页
	u32 end_page;						//终止写的页(自身包含)
	u32 begin_off;						//(页内)开始写的偏移
	u32 end_off;						//(页内)终止写的偏移(自身包含)
	u32 vpn;							//当前页的文件内逻辑页号
	u32 ppn;							//当前页的相对物理页号
	u32 len;							//总共写过的字节长度


	inode = file->f_dentry->d_inode;
	if (inode == 0)
		return 0;
	mapping = file->f_mapping;
	if (mapping == 0)
		return 0;

	len = 0;
	page_size = inode->i_blksize;
	begin_page = file->f_pos / page_size;
	end_page = (file->f_pos + count - 1) / page_size;	
	//算下来的end_page可能会超过当前该文件的逻辑页编号
	//必要时还要更新索引节点的大小，而且还要修改其父目录中的对应表项
	//如果新内容较原来有收缩，不必更新，把剩余的空间当成空余即可
	if (file->f_pos + count > inode->i_size) {
		struct super_block		*sb;
		struct list_head		*head;
		struct dentry			*dentry;
		struct dentry			*p_dentry;
		u32 err;

		//准备超级块
		sb = inode->i_sb;
		if (sb == 0 || sb->s_op == 0)
			return 0;

		//准备父目录项
		head = &inode->i_dentry;
		if (head->next == head)
			//不允许该索引节点没有目录项对应
			return 0;

		dentry = container_of(head->next, struct dentry, d_alias);
		p_dentry = dentry->d_parent;
		if (p_dentry == 0)
			return 0;

		inode->i_size = file->f_pos + count;
		err = sb->s_op->write_inode(inode, p_dentry);
		if (err)
			return 0;
	}
	
	for (vpn = begin_page; vpn <= end_page; vpn++) {
		//先对该页的起始写位置和终止写位置进行确定
		begin_off = (vpn == begin_page) ? file->f_pos % page_size : 0;
		end_off = (vpn == end_page) ? (file->f_pos + count - 1) % page_size : page_size - 1;

		//找到该页的page结构，如果逻辑页号超出，bmap接口内部会实现创建新的页
		ppn = mapping->a_op->bmap(inode, vpn);
		if (ppn == -1) {
			goto file_write_end;
		}

		kernel_printf("mapping addr: %d %d mapping[0]: %d\n", (u32)mapping, (u32)inode->i_data, mapping->a_page[0]);
		/*由于没有申请新的页并改变mapping本身的地址，所以mapping不必更新*/
		page = general_find_page(ppn, inode);

		if (page == 0 || page->p_data == 0) {
			goto file_write_end;
		}

		

		//存入页当中
		kernel_memcpy(page->p_data + begin_off, buf + len, end_off - begin_off + 1);
		len += (end_off - begin_off + 1);
		page->p_state = PAGE_DIRTY;
		kernel_printf("page addr: %d\n", (u32)page);
		for (u32 i = 0; i < 5; i++)
			kernel_printf("%c", page->p_data[i]);
		kernel_printf("\n");
	}
file_write_end:
	//正常情况下len == count，但需要考虑出错的情况
	file->f_pos += len;
	return len;
}

//通用冲洗方法，返回非零错误
u32 generic_file_flush(struct file * file) {
	struct vfs_page			*page;
	struct address_space	*mapping;
	struct list_head		*page_head;
	struct list_head		*pos;
	mapping = file->f_mapping;
	page_head = &file->f_mapping->a_cache;
	//kernel_printf("Welcome to generic_file_flush()\n");
	//kernel_printf("mapping addr:%d, page_head:%d", (u32)mapping, (u32)page_head);
	list_for_each(pos, page_head) {
		page = container_of(pos, struct vfs_page, p_list);
		//kernel_printf("pos addr: %d\n", (u32)pos);
		//writepage接口会自动判断是否是脏页
		if (mapping->a_op->writepage(page))
			return -EIO;
	}
	return 0;
}

//给定该页的相对物理地址和该页对应的索引节点，找到某一数据页（簇）
//先从pcache里面找，找不到就新建一个缓存页，读入对应数据(假设外存上有)、插入pcache后返回
struct vfs_page* general_find_page(u32 pageNo, struct inode* inode) {
	struct vfs_page*		page;
	struct address_space*	mapping;
	struct condition		cond;
	//kernel_printf("Welcome to general_find_page().\n");
	mapping = inode->i_data;

	//先从pcache里面找
	cond.cond1 = (void*)(&pageNo);
	cond.cond2 = (void*)(inode);
	page = (struct vfs_page*)(pcache->c_op->look_up(pcache, &cond));

	if (page) {
		return page;
	}

	//如果找不到，新建一个缓存页，从外存读入数据
	page = (struct vfs_page*)kmalloc(sizeof(struct vfs_page));
	if (page == 0)
		return 0;
	page->p_data = (u8*)kmalloc(sizeof(u8) * inode->i_blksize);
	if (page->p_data == 0)
		return 0;

	page->p_state = PAGE_INVALID;
	page->p_mapping = mapping;
	page->p_location = pageNo;
	INIT_LIST_HEAD(&(page->p_list));
	INIT_LIST_HEAD(&(page->p_hash));
	INIT_LIST_HEAD(&(page->p_LRU));

	//正式读数据
	if (mapping->a_op->readpage(page)) {
		kfree(page);
		kfree(page->p_data);
		return 0;
	}

	list_add(&(page->p_list), &(mapping->a_cache));
	pcache->c_op->add(pcache, (void*)page);
	return page;
}
