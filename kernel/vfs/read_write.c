#include <zjunix/vfs.h>
#include <zjunix/vfscache.h>

#include <zjunix/slab.h>
#include <zjunix/utils.h>
#include <driver/vga.h>
#include <zjunix/errno.h>

extern struct cache	*pcache;

//�����ļ�ϵͳ���ӿ�
u32 vfs_read(struct file *file, char *buf, u32 count) {
	if (file->f_mode & FMODE_READ) {
		return file->f_op->read(file, buf, count);
	}
	//������ļ�û�ж�Ȩ�ޣ�ֱ�Ӿܾ���������0(ʵ�ʶ���0���ֽ�)
	return 0;
}

//�����ļ�ϵͳд�ӿ�
u32 vfs_write(struct file *file, char *buf, u32 count) {
	if (file->f_mode & FMODE_WRITE) {
		return file->f_op->write(file, buf, count);
	}
	//������ļ�û��дȨ�ޣ�ֱ�Ӿܾ�д������0(ʵ��д��0���ֽ�)
	return 0;
}

//ͨ�ö��ļ�����
//����ʵ�ʶ��˶��ٸ��ֽ�
u32 generic_file_read(struct file *file, u8 *buf, u32 count) {
	//����buf�Ѿ��������㹻�Ŀռ�
	struct vfs_page			*page;
	struct inode			*inode;
	struct address_space	*mapping;
	u32 page_size;
	u32 begin_page;						//��ʼ��ȡ��ҳ
	u32 end_page;						//��ֹ��ȡ��ҳ(�������)
	u32 begin_off;						//(ҳ��)��ʼ��ȡ��ƫ��
	u32 end_off;						//(ҳ��)��ֹ��ȡ��ƫ��(�������)
	u32 vpn;							//��ǰҳ���ļ����߼�ҳ��
	u32 ppn;							//��ǰҳ���������ҳ��
	u32 len;							//�ܹ���ȡ�������ֽڳ���

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
		//�ȶԸ�ҳ����ʼ��ȡλ�ú���ֹ��ȡλ�ý���ȷ��
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
		//�ҵ���ҳ��page�ṹ
		ppn = mapping->a_op->bmap(inode, vpn);
		
		if (ppn == -1) {
			goto file_read_end;
		}

		page = general_find_page(ppn, inode);

		if (page == 0 || page->p_data == 0) {
			goto file_read_end;
		}

		//����buf����
		kernel_memcpy(buf + len, page->p_data + begin_off, end_off - begin_off + 1);
		len += (end_off - begin_off + 1);
	}
file_read_end:
	//���������len == count������Ҫ���ǳ�������
	file->f_pos += len;
	return len;
}

//ͨ��д�ļ�����
//����ʵ��д�˶��ٸ��ֽ�
u32 generic_file_write(struct file *file, u8 *buf, u32 count) {
	//����buf�������Ѿ����
	struct vfs_page			*page;
	struct inode			*inode;
	struct address_space	*mapping;
	u32 page_size;
	u32 begin_page;						//��ʼд��ҳ
	u32 end_page;						//��ֹд��ҳ(�������)
	u32 begin_off;						//(ҳ��)��ʼд��ƫ��
	u32 end_off;						//(ҳ��)��ֹд��ƫ��(�������)
	u32 vpn;							//��ǰҳ���ļ����߼�ҳ��
	u32 ppn;							//��ǰҳ���������ҳ��
	u32 len;							//�ܹ�д�����ֽڳ���


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
	//��������end_page���ܻᳬ����ǰ���ļ����߼�ҳ���
	//��Ҫʱ��Ҫ���������ڵ�Ĵ�С�����һ�Ҫ�޸��丸Ŀ¼�еĶ�Ӧ����
	//��������ݽ�ԭ�������������ظ��£���ʣ��Ŀռ䵱�ɿ��༴��
	if (file->f_pos + count > inode->i_size) {
		struct super_block		*sb;
		struct list_head		*head;
		struct dentry			*dentry;
		struct dentry			*p_dentry;
		u32 err;

		//׼��������
		sb = inode->i_sb;
		if (sb == 0 || sb->s_op == 0)
			return 0;

		//׼����Ŀ¼��
		head = &inode->i_dentry;
		if (head->next == head)
			//������������ڵ�û��Ŀ¼���Ӧ
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
		//�ȶԸ�ҳ����ʼдλ�ú���ֹдλ�ý���ȷ��
		begin_off = (vpn == begin_page) ? file->f_pos % page_size : 0;
		end_off = (vpn == end_page) ? (file->f_pos + count - 1) % page_size : page_size - 1;

		//�ҵ���ҳ��page�ṹ������߼�ҳ�ų�����bmap�ӿ��ڲ���ʵ�ִ����µ�ҳ
		ppn = mapping->a_op->bmap(inode, vpn);
		if (ppn == -1) {
			goto file_write_end;
		}

		kernel_printf("mapping addr: %d %d mapping[0]: %d\n", (u32)mapping, (u32)inode->i_data, mapping->a_page[0]);
		/*����û�������µ�ҳ���ı�mapping����ĵ�ַ������mapping���ظ���*/
		page = general_find_page(ppn, inode);

		if (page == 0 || page->p_data == 0) {
			goto file_write_end;
		}

		

		//����ҳ����
		kernel_memcpy(page->p_data + begin_off, buf + len, end_off - begin_off + 1);
		len += (end_off - begin_off + 1);
		page->p_state = PAGE_DIRTY;
		kernel_printf("page addr: %d\n", (u32)page);
		for (u32 i = 0; i < 5; i++)
			kernel_printf("%c", page->p_data[i]);
		kernel_printf("\n");
	}
file_write_end:
	//���������len == count������Ҫ���ǳ�������
	file->f_pos += len;
	return len;
}

//ͨ�ó�ϴ���������ط������
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
		//writepage�ӿڻ��Զ��ж��Ƿ�����ҳ
		if (mapping->a_op->writepage(page))
			return -EIO;
	}
	return 0;
}

//������ҳ����������ַ�͸�ҳ��Ӧ�������ڵ㣬�ҵ�ĳһ����ҳ���أ�
//�ȴ�pcache�����ң��Ҳ������½�һ������ҳ�������Ӧ����(�����������)������pcache�󷵻�
struct vfs_page* general_find_page(u32 pageNo, struct inode* inode) {
	struct vfs_page*		page;
	struct address_space*	mapping;
	struct condition		cond;
	//kernel_printf("Welcome to general_find_page().\n");
	mapping = inode->i_data;

	//�ȴ�pcache������
	cond.cond1 = (void*)(&pageNo);
	cond.cond2 = (void*)(inode);
	page = (struct vfs_page*)(pcache->c_op->look_up(pcache, &cond));

	if (page) {
		return page;
	}

	//����Ҳ������½�һ������ҳ��������������
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

	//��ʽ������
	if (mapping->a_op->readpage(page)) {
		kfree(page);
		kfree(page->p_data);
		return 0;
	}

	list_add(&(page->p_list), &(mapping->a_cache));
	pcache->c_op->add(pcache, (void*)page);
	return page;
}
