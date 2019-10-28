#include <zjunix/vfs.h>
#include <zjunix/vfscache.h>

#include <zjunix/type.h>
#include <zjunix/utils.h>
#include <zjunix/slab.h>
#include <zjunix/errno.h>
#include <zjunix/err.h>
#include <driver/vga.h>

// �ⲿ����
extern struct cache     *dcache;
extern struct dentry    *root_dentry;
extern struct dentry    *pwd_dentry;
extern struct vfsmount  *root_mount;
extern struct vfsmount  *pwd_mount;

//�ļ��򿪺���
struct file* vfs_open(const u8 *filename, u32 flags, u32 mode) {
	struct nameidata	nd;
	u32 err;

	//��һ��������·���ͱ�־����Ŀ��Ŀ¼��͹��������Ϣ�Ž�nd�д���
	err = open_namei(filename, flags, mode, &nd);
	if (err)
		return (struct file*)ERR_PTR(err);

	//�ڶ��������õ�һ���ռ��õ�nd���ݽṹ�������һ���ļ�ָ�벢����
	return dentry_open(nd.dentry, nd.mnt, flags);
}

//�ú��������ҵ����ߴ�����Ŀ��·����Ŀ¼�����Ϣ��Ϊʵ�����ļ�ָ����׼��
u32 open_namei(const u8 *pathname, u32 flag, u32 mode, struct nameidata *nd) {
	struct dentry	*dentry;
	u32 new_flag;
	u32 err;

	new_flag = (flag & O_DIR) ? LOOKUP_DIRECTORY : 0;
	//��vfs_open�еı�־ת����nd->flags�еı�־
	if ((flag & O_CREATE) == 0) {
		//�����һ������ȡ��nd���棬ֱ�ӷ��ؼ���
		err = path_lookup(pathname, new_flag, nd);
		if(err)
			goto fail;
		goto done;
	}
	
	//�������ֻ���򿪣��������������û�п�����ʱ����һ��
	new_flag |= LOOKUP_CREATE;

	//�ѵ����ڶ�������ȡ��nd����
	err = path_lookup(pathname, new_flag, nd);
	if (err)
		goto fail;

	//������һ�����������ͣ����˵������ʵ�
	err = -ELASTTYPE;
	if ((nd->last_type != LAST_NORM) && (nd->last_type != LAST_DIR))
		goto fail;

	//ͨ����Ŀ¼����ļ������ȴ�������в���Ŀ��·����Ŀ¼��(ע�����ʱӦ���ֶԴ�Ŀ¼����ͨ�ļ�)
	dentry = do_lookup_create(&nd->last, nd->dentry, nd->last_type);

	//���ﲢû�е���follow_mount�����������ļ�ϵͳ�����ͣ�ԭ�����£�
	//1.����ڴ���ģʽ�´򿪵����ļ�������������ļ�����Ŀ¼���ļ�ϵͳ���ͱ��뱣��һ�£�
	//2.����ڴ���ģʽ�´򿪵���Ŀ¼����Ĭ�����½���Ŀ¼�����½���Ŀ¼���ܸ����ļ�ϵͳ��
	//3.�������ֻ��Ŀ¼�������������ļ�ϵͳ�л��Ŀ��ܣ�����ʱ�벻Ҫʹ�ô���ģʽ����Ҫ��Ѱ��ģʽ��

	if (dentry == 0) {
		//�����ڴ�����ԭ��
		err = -ENOMEM;
		goto fail;
	}

	if (dentry->d_inode) {
		if (nd->last_type == LAST_DIR) {
			//�Ѿ����ڵ�Ŀ¼�����ô���ģʽ�����ʺʹ�
			kernel_printf("Existant directory cannot be accessed in create mode!\n");
			err = -ELASTTYPE;
			goto fail;
		}
		else {
			//�ļ��Ѿ�����������������ˣ����Գɹ�����
			//������nd
			nd->dentry = dentry;
		}
	}
	else {
		//����涼û�У�����ȷʵ��Ҫ������
		err = nd->dentry->d_inode->i_op->create(dentry, mode, nd);
		if (err) {
			err = -ENOINODE;
			goto fail;
		}

		//������nd
		nd->dentry = dentry;
	}

done:
	return 0;
fail:
	return err;	
}

//�ú�����Ҫ��nd�ṹ���г�ʼ����Ȼ�����do_path_lookup��������Ŀ��·����Ŀ¼�����Ϣ
//���ط������
u32 path_lookup(const u8 * name, u32 flags, struct nameidata *nd) {
	u32 err;
	nd->last_type = LAST_ROOT;
	nd->flags = flags;

	if (*name == 0)
		return -ENOPATH;

	if (name[0] == '/') {
		//�Ӹ�Ŀ¼��ʼ
		nd->dentry = root_dentry;
		nd->mnt = root_mount;
	}
	else {
		//�ӵ�ǰĿ¼��ʼ
		nd->dentry = pwd_dentry;
		nd->mnt = pwd_mount;
	}

	//��ʽ��·������
	err = do_path_lookup(name, nd);
	return err;
}

//˳��·��һ·������������ģʽ������һ����Ϣװ��nd���أ�
//����ģʽ��ѵ����ڶ�����Ϣװ��nd����
//���ط������
u32 do_path_lookup(const u8 *name, struct nameidata *nd) {
	struct path		this_path;
	struct qstr		this_name;
	struct inode	*this_inode;
	u32 err;
	u8  *p;
	//kernel_printf("Welcome to do_path_lookup()\n");

	//path�ṹ��ʼ��
	this_path.dentry = nd->dentry;
	this_path.mnt = nd->mnt;

	p = (u8*) name;
	//�����ʼ��б��
	while (*p == '/') {
		p++;
	}
	if (*p == 0) {
		//·����ֻ��'/'������ǲ���Ŀ¼������������
		//������Ϊ��Ч·��
		if (nd->flags & LOOKUP_DIRECTORY)
			goto done;
		else {
			err = -ENOPATH;
			goto fail;
		}
	}

	//��ʽ��ʼѭ��
	while (1) {
		//kernel_printf("loop head.\n");
		//�����÷���������
		this_name.name = p;
		while ((*p) && (*p != '/'))
			p++;
		this_name.len = p - this_name.name;
		//kernel_printf("this_name: %d %s\n", this_name.len, this_name.name);
		//����Ѿ��������һ�����������˳���ѭ��ר�Ŵ���
		if (*p == 0)
			goto last_component;

		//*p == '/'
		while (*p == '/')
			p++;
		if (*p == 0)
			goto last_component_with_slash;

		//�м�����������
		//�ȴ���·����Ϊ'.'��'..'�����
		if (this_name.name[0] == '.') {
			switch (this_name.len) {
			case 1: continue;	//ֱ�ӽ�����һ��ѭ��
			case 2: {
				if (this_name.name[1] == '.') {
					follow_dotdot(&nd->mnt, &nd->dentry);	//���ݵ���Ŀ¼��
					continue;	//ֱ�ӽ�����һ��ѭ��
				}
				else
					break;		//��������ѭ��
			}
			default:
				break;			//��������ѭ��
			}
		}

		//����·�����͸�Ŀ¼��Ϣȥ�Ҹ�·����Ӧ��Ŀ¼��Ϣ
		err = do_lookup(nd, &this_name, &this_path, LAST_DIR);
		if (err) {
			err = -ENOINODE;
			goto fail;
		}
		//kernel_printf("nd   position: %s\n", nd->dentry->d_name.name);
		//kernel_printf("path position: %s\n", this_path.dentry->d_name.name);

		//����Ƿ���Ҫ��Խ�ļ�ϵͳ
		err = follow_mount(&this_path.mnt, &this_path.dentry);
		if (err) {
			goto fail;
		}

		//kernel_printf("follow_mount() done.\n");

		//����Ŀ¼�����������Ƿ���ʵ����
		//���������Ǵ����м�����ģ����Ա���Ҫ���ڶ�Ӧ�������ڵ�Ͳ��ҷ���
		this_inode = this_path.dentry->d_inode;
		if (this_inode == 0 || this_inode->i_op->lookup == 0) {
			err = -ENOINODE;
			goto fail;
		}

		//kernel_printf("inode check done.\n");

		//����nd�ṹ��������һ��ѭ��
		nd->dentry = this_path.dentry;
		nd->mnt = this_path.mnt;
		//kernel_printf("nd   position: %s\n", nd->dentry->d_name.name);
		continue;
		//��ѭ�����˽���

last_component_with_slash:
		//˵�����һ��������Ŀ¼����һ��������ļ������ԭ��Ҫ��һ����ͨ�ļ����򱨴�
		if ((nd->flags & LOOKUP_DIRECTORY) == 0) {
			err = -ELASTTYPE;
			goto fail;
		}

last_component:
		//��ֻ��ģʽ�ʹ���ģʽ�����������
		if (nd->flags & LOOKUP_CREATE)
			goto lookup_parent;

		//ֻ��ģʽ�����������÷���
		//�������һ����������'.'��'..'�����������Ŀ¼���ļ�Ӧ��������Ӧ�Բ���
		if (nd->flags & LOOKUP_DIRECTORY) {
			//����ǲ���Ŀ¼�����������ֱ�﷽ʽ
			if (this_name.name[0] == '.') {
				switch (this_name.len) {
				case 1: goto done;		//ֱ�����
				case 2: {
					if (this_name.name[1] == '.') {
						follow_dotdot(&nd->mnt, &nd->dentry);	//���ݵ���Ŀ¼��
						goto done;		//ֱ�����
					}
					else
						break;
				}
				default:
					break;
				}
			}
		}
		else {
			//����ǲ����ļ������������ֱ�﷽ʽ
			err = 0;
			switch (this_name.len) {
			case 1: err = -ENOPATH;  break;		//����
			case 2: {
				if (this_name.name[1] == '.') {
					err = -ENOPATH;	 break;		//����
				}
				else
					break;
			}
			default:
				break;
			}
			if (err)
				goto fail;
		}
		//kernel_printf("final do_lookup() begin!\n");
		//����·�����͸�Ŀ¼��Ϣȥ�Ҹ�·����Ӧ��Ŀ¼��Ϣ
		u32 type;
		type = (nd->flags & LOOKUP_DIRECTORY) ? LAST_DIR : LAST_NORM;
		err = do_lookup(nd, &this_name, &this_path, type);
		if (err) {
			err = -ENOINODE;
			goto fail;
		}

		/*kernel_printf("nd   position: %s\n", nd->dentry->d_name.name);
		kernel_printf("path position: %s\n", this_path.dentry->d_name.name);*/

		//����Ƿ���Ҫ��Խ�ļ�ϵͳ
		err = follow_mount(&this_path.mnt, &this_path.dentry);
		if (err) {
			goto fail;
		}

		//kernel_printf("follow_mount() done.\n");

		//����Ŀ¼�����������Ƿ���ʵ����
		//������Ҫ��Ŀ¼���ļ��������������
		this_inode = this_path.dentry->d_inode;
		if (this_inode == 0) {
			err = -ENOINODE;
			goto fail;
		}
		if ((nd->flags & LOOKUP_DIRECTORY) && (this_inode->i_op->lookup == 0)) {
			//��Ŀ¼����û����Ӧ�Ĳ��ҽӿں���Ҳ����
			err = -ENOINODE;
			goto fail;
		}

		//kernel_printf("inode check done.\n");

		//����nd�ṹ������
		nd->dentry = this_path.dentry;
		nd->mnt = this_path.mnt;
		//kernel_printf("final nd position: %s\n", nd->dentry->d_name.name);
		goto done;

lookup_parent:
		//����ģʽ
		//��ʱnd��dentry��mnt�Ѿ���Ӧ�����ڶ��������ˣ�����ֻ���nd��last�ֶθ�ֵ
		nd->last.name = (u8*)kmalloc(sizeof(u8)*(this_name.len + 1));
		if (nd->last.name == 0) {
			err = -ENOMEM;
			goto fail;
		}
		kernel_memcpy(nd->last.name, this_name.name, this_name.len);
		nd->last.name[this_name.len] = 0;
		nd->last.len = this_name.len;

		//��������nd��last_type��ֵ
		if (nd->flags & LOOKUP_DIRECTORY)
			nd->last_type = LAST_DIR;
		else {
			nd->last_type = LAST_NORM;
			if (this_name.name[0] == '.') {
				switch (this_name.len) {
				case 1: nd->last_type = LAST_DOT;		break;
				case 2: {
					if (this_name.name[1] == '.') {
						nd->last_type = LAST_DOTDOT;	break;
					}
					else
						break;
				}
				default:
					break;
				}
			}
		}
		goto done;
	}
fail:
	return err;

done:
	return 0;
}

//����Ŀ¼��͹�����Ϣ���Լ�vfs_open�Ĳ���flags�����첢��ʼ����һ���ļ�ָ��
struct file * dentry_open(struct dentry *dentry, struct vfsmount *mnt, u32 flags) {
	struct file	*f;
	u32 rwflag = flags & O_MASK;

	f = (struct file*)kmalloc(sizeof(struct file));
	if (f == 0)
		return (struct file*)ERR_PTR(-ENOMEM);

	f->f_flags = (flags & O_DIR) ? LAST_DIR : LAST_NORM;
	f->f_mode = 0;
	if (rwflag != O_RDONLY)
		f->f_mode |= FMODE_WRITE;
	if (rwflag == O_RDONLY || rwflag == O_RDWR)
		f->f_mode |= FMODE_READ;
	f->f_dentry = dentry;
	f->f_vfsmnt = mnt;
	f->f_mapping = dentry->d_inode->i_data;
	f->f_op = dentry->d_inode->i_fop;
	f->f_pos = 0;
	return f;
}

//��ͼ���ص���Ŀ¼��
//��Ҫ���ǿ�Խ�ļ�ϵͳ�����
void follow_dotdot(struct vfsmount **p_mnt, struct dentry **p_dentry) {
	struct dentry	*old_d;
	struct vfsmount	*old_mnt;
	
	while (1) {
		old_d = *p_dentry;
		old_mnt = *p_mnt;
		//���һ����ǰ����λ����ȫ�ֵĸ�Ŀ¼��ֱ���˳�
		if ((old_d == root_dentry) && (*p_mnt == root_mount)) {
			break;
		}

		//���������ǰ����λ�ò��ǵ�ǰ�ļ�ϵͳ�ĸ���ֱ��ȡ��Ŀ¼���
		if (old_d != (old_mnt->mnt_root)) {
			*p_dentry = old_d->d_parent;
			break;
		}
		
		//���������ǰ����λ�����ǵ�ǰ�ļ�ϵͳ�ĸ���������ȫ�ֵĸ�
		//��ʱ��Ҫ�ȿ�Խ�����ļ�ϵͳ���ٽ���һ��ѭ��
		*p_mnt = old_mnt->mnt_parent;
		*p_dentry = old_mnt->mnt_mountpoint;
	}
}

//���ݸ�Ŀ¼��͸��ļ������ҵ����ļ���Ŀ¼��͹�����Ϣ������path�д���
u32 do_lookup(struct nameidata *nd, struct qstr *name, struct path *path, u32 type) {
	struct dentry		*p_dentry;
	struct dentry		*this;
	struct dentry		*new;
	struct inode		*p_inode;
	struct condition	cond;
	//kernel_printf("Welcome to do_lookup().\n");
	//�����Ŵ�dcache����
	p_dentry = nd->dentry;
	cond.cond1 = (void*)name;
	cond.cond2 = (void*)p_dentry;
	this = (struct dentry*)dcache->c_op->look_up(dcache, &cond);
	if (this)
		goto find_done;
	//kernel_printf("cannot find the dentry.\n");
	//������û�У����ŵ��õײ�ӿڴ������Ѱ��
	//����ռ䲢��ʼ��һ��Ŀ¼�����
	new = d_alloc(name, p_dentry);
	if (new == 0)
		goto find_fail;

	p_inode = p_dentry->d_inode;
	this = p_inode->i_op->lookup(p_inode, new, type);	//����Ҫ��Ŀ¼���͵�
	
	if (this) {
		//�����ҳɹ�
		//kernel_printf("i_op->lookup() done.\n");
		goto find_done;
	}
	else {
		//������ʧ�ܣ�������ʧ��
		//kernel_printf("i_op->lookup() fail.\n");
		goto find_fail;
	}

find_fail:
	return -1;

find_done:
	path->dentry = this;
	path->mnt = nd->mnt;
	return 0;
}

struct dentry* d_alloc(struct qstr* name, struct dentry* p_dentry) {
	//ע�⣺�������p_dentry����NULL��
	struct list_head	*head;
	struct list_head	*p;
	struct vfsmount		*mnt;
	struct dentry		*dentry;
	u8	*buf;

	dentry = (struct dentry*)kmalloc(sizeof(struct dentry));
	if (dentry == 0)
		return dentry;

	//�ļ��������ݿռ䲻�ܹ������뿽��һ��
	buf = (u8*)kmalloc(sizeof(u8)*(name->len + 1));
	if (buf == 0)
		return 0;
	kernel_memcpy(buf, name->name, name->len);
	buf[name->len] = 0;

	dentry->d_name.name = buf;
	dentry->d_name.len = name->len;
	dentry->d_count = 1;
	dentry->d_mounted = 0;					//�ݶ�
	dentry->d_inode = 0;					//�ݶ�
	dentry->d_pinned = 0;
	dentry->d_sb = p_dentry->d_sb;
	dentry->d_op = p_dentry->d_op;
	dentry->d_parent = p_dentry;
	INIT_LIST_HEAD(&(dentry->d_alias));		//�ݶ�
	INIT_LIST_HEAD(&(dentry->d_child));		//�ݶ�
	INIT_LIST_HEAD(&(dentry->d_hash));		//�ݶ�
	//INIT_LIST_HEAD(&(dentry->d_LRU));		//�ݶ�
	INIT_LIST_HEAD(&(dentry->d_subdirs));

	//����dentry->mount�ĸ�ֵ
	head = &root_mount->mnt_hash;
	p = head;
	while (1) {
		mnt = container_of(p, struct vfsmount, mnt_hash);
		if (mnt->mnt_mountpoint == dentry) {
			//�������ļ�ϵͳ���ص���ǰĿ¼���ϣ����޸�d_mountֵ
			dentry->d_mounted = 1;
			break;
		}
		p = p->next;
		if (p == head)
			break;
	}

	//����dentry->child�ĸ�ֵ
	list_add(&(dentry->d_child), &(p_dentry->d_subdirs));

	//�Ž������У�ͬʱ��hash��ֵ
	dcache->c_op->add(dcache, (void*)dentry);

	//��inode��alias�ĸ�ֵ��Ҫ�ȵ��˿ں�����ʵ��
	return dentry;
}

//���ݸ�Ŀ¼�����ֲ��Ҷ�Ӧ��Ŀ¼�����ģʽר�ã�
//�ú����Ǵ����ļ���һ�����ڣ�Ҳ���ǰѴ������ļ���dentry׼����
struct dentry * do_lookup_create(struct qstr *name, struct dentry *p_dentry, u32 type) {
	struct dentry		*dentry;
	struct dentry		*new;
	struct inode		*p_inode;
	struct condition	cond;

	p_inode = p_dentry->d_inode;

	//��ȥ��������һ�鿴��û��
	cond.cond1 = (void*)name;
	cond.cond2 = (void*)p_dentry;
	dentry = dcache->c_op->look_up(dcache, &cond);
	if (dentry)
		return dentry;

	//���û�У���ͼ�����������
	new = d_alloc(name, p_dentry);	//�˺����Ѿ���new���뻺����ȥ
	if (new == 0) {
		//�ڴ�����ʧ��
		return 0;
	}
	dentry = p_inode->i_op->lookup(p_inode, new, type);		//�����һ������ͱ���һ��
	if (dentry == 0) {
		//�����Ҳû�У���Ҫ�������ѻ�ȱ��inode��Ŀ¼����Ʒ��new����
		return new;
	}
	else {
		//������Ѿ����ڣ������ٴ����������ء���Ʒ��dentry
		//���ڽӿں�����ʵ�ַ�ʽ��dentry��new����ָ��ͬһ����������
		//��ʱ��dentry->d_countӦ��Ϊ1�����ּ���ʹ�䱣���ڻ�����
		return dentry;
	}
}

u32 vfs_close(struct file *filp) {
	u32 err;
	//�Ѹ��ļ�����ҳ�����е�ҳд�ش���
	err = filp->f_op->flush(filp);
	if (err)
		return err;
	kfree(filp);
	return 0;
}

void generic_delete_alias(struct inode *inode) {
	struct dentry		*dentry;
	struct list_head	*head;
	struct list_head	*p;
	struct list_head	*p_next;

	head = &inode->i_dentry;
	list_for_each_safe(p, p_next, head) {
		dentry = container_of(p, struct dentry, d_alias);
		dcache->c_op->del(dcache, (void*)dentry);	//�߳����棬�ͷſռ�
	}
}

//�Ƚ�dentry�����ֺ�direntry���ļ����Ƿ�ƥ�䣬����0����ƥ��
//������strcmp�ĺ���
u32 generic_check_filename(const struct qstr* dentry_s, const struct qstr* filename_s) {
	u32 i;
	u32 ret;
	u8  c1;
	u8  c2;
	if (dentry_s->len != filename_s->len)
		return 1;
	//kernel_printf("Welcome to generic_check_filename().\n");
	//kernel_printf("%d %s; %d %s\n", dentry_s->len, dentry_s->name, filename_s->len, filename_s->name);
	//dentry_s->len == filename_s->len
	i = 0;
	while (1) {
		if (i >= dentry_s->len)
			break;
		c1 = dentry_s->name[i];
		c2 = filename_s->name[i];
		c1 = (c1 >= 'a' && c1 <= 'z') ? (c1 - 'a' + 'A') : c1;
		c2 = (c2 >= 'a' && c2 <= 'z') ? (c2 - 'a' + 'A') : c2;
		//kernel_printf("%c %c\n", c1, c2);
		if (c1 != c2)
			return 1;
		i++;
	}
	return 0;
}
