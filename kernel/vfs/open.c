#include <zjunix/vfs.h>
#include <zjunix/vfscache.h>

#include <zjunix/type.h>
#include <zjunix/utils.h>
#include <zjunix/slab.h>
#include <zjunix/errno.h>
#include <zjunix/err.h>
#include <driver/vga.h>

// 外部变量
extern struct cache     *dcache;
extern struct dentry    *root_dentry;
extern struct dentry    *pwd_dentry;
extern struct vfsmount  *root_mount;
extern struct vfsmount  *pwd_mount;

//文件打开函数
struct file* vfs_open(const u8 *filename, u32 flags, u32 mode) {
	struct nameidata	nd;
	u32 err;

	//第一步，分析路径和标志，把目标目录项和挂载项等信息放进nd中传回
	err = open_namei(filename, flags, mode, &nd);
	if (err)
		return (struct file*)ERR_PTR(err);

	//第二步，利用第一步收集好的nd数据结构，构造出一个文件指针并返回
	return dentry_open(nd.dentry, nd.mnt, flags);
}

//该函数负责找到或者创建出目标路径的目录项等信息，为实例化文件指针做准备
u32 open_namei(const u8 *pathname, u32 flag, u32 mode, struct nameidata *nd) {
	struct dentry	*dentry;
	u32 new_flag;
	u32 err;

	new_flag = (flag & O_DIR) ? LOOKUP_DIRECTORY : 0;
	//把vfs_open中的标志转换成nd->flags中的标志
	if ((flag & O_CREATE) == 0) {
		//把最后一个分量取到nd里面，直接返回即可
		err = path_lookup(pathname, new_flag, nd);
		if(err)
			goto fail;
		goto done;
	}
	
	//如果不是只读打开，都看做如果现在没有可以临时创建一个
	new_flag |= LOOKUP_CREATE;

	//把倒数第二个分量取到nd里面
	err = path_lookup(pathname, new_flag, nd);
	if (err)
		goto fail;

	//检查最后一个分量的类型，过滤掉不合适的
	err = -ELASTTYPE;
	if ((nd->last_type != LAST_NORM) && (nd->last_type != LAST_DIR))
		goto fail;

	//通过父目录项和文件名，先从内外存中查找目标路径的目录项(注意查找时应区分对待目录和普通文件)
	dentry = do_lookup_create(&nd->last, nd->dentry, nd->last_type);

	//这里并没有调用follow_mount函数来调整文件系统的类型，原因如下：
	//1.如果在创建模式下打开的是文件，则无论如何文件与其目录的文件系统类型必须保持一致；
	//2.如果在创建模式下打开的是目录，则默认是新建该目录，而新建的目录不能更换文件系统；
	//3.如果存在只读目录的情况，会存在文件系统切换的可能，但这时请不要使用创建模式，而要用寻找模式。

	if (dentry == 0) {
		//这是内存分配的原因
		err = -ENOMEM;
		goto fail;
	}

	if (dentry->d_inode) {
		if (nd->last_type == LAST_DIR) {
			//已经存在的目录不能用创建模式来访问和打开
			kernel_printf("Existant directory cannot be accessed in create mode!\n");
			err = -ELASTTYPE;
			goto fail;
		}
		else {
			//文件已经存在于物理介质上了，可以成功返回
			//最后更新nd
			nd->dentry = dentry;
		}
	}
	else {
		//内外存都没有，所以确实需要创建了
		err = nd->dentry->d_inode->i_op->create(dentry, mode, nd);
		if (err) {
			err = -ENOINODE;
			goto fail;
		}

		//最后更新nd
		nd->dentry = dentry;
	}

done:
	return 0;
fail:
	return err;	
}

//该函数主要对nd结构进行初始化，然后调用do_path_lookup函数查找目标路径的目录项等信息
//返回非零错误
u32 path_lookup(const u8 * name, u32 flags, struct nameidata *nd) {
	u32 err;
	nd->last_type = LAST_ROOT;
	nd->flags = flags;

	if (*name == 0)
		return -ENOPATH;

	if (name[0] == '/') {
		//从根目录开始
		nd->dentry = root_dentry;
		nd->mnt = root_mount;
	}
	else {
		//从当前目录开始
		nd->dentry = pwd_dentry;
		nd->mnt = pwd_mount;
	}

	//正式沿路径查找
	err = do_path_lookup(name, nd);
	return err;
}

//顺着路径一路找下来。查找模式会把最后一项信息装进nd返回；
//创建模式会把倒数第二项信息装进nd返回
//返回非零错误
u32 do_path_lookup(const u8 *name, struct nameidata *nd) {
	struct path		this_path;
	struct qstr		this_name;
	struct inode	*this_inode;
	u32 err;
	u8  *p;
	//kernel_printf("Welcome to do_path_lookup()\n");

	//path结构初始化
	this_path.dentry = nd->dentry;
	this_path.mnt = nd->mnt;

	p = (u8*) name;
	//跳过最开始的斜杠
	while (*p == '/') {
		p++;
	}
	if (*p == 0) {
		//路径名只有'/'，如果是查找目录可以正常返回
		//否则将视为无效路径
		if (nd->flags & LOOKUP_DIRECTORY)
			goto done;
		else {
			err = -ENOPATH;
			goto fail;
		}
	}

	//正式开始循环
	while (1) {
		//kernel_printf("loop head.\n");
		//解析该分量的名字
		this_name.name = p;
		while ((*p) && (*p != '/'))
			p++;
		this_name.len = p - this_name.name;
		//kernel_printf("this_name: %d %s\n", this_name.len, this_name.name);
		//如果已经到达最后一个分量，则退出主循环专门处理
		if (*p == 0)
			goto last_component;

		//*p == '/'
		while (*p == '/')
			p++;
		if (*p == 0)
			goto last_component_with_slash;

		//中间分量处理程序
		//先处理路径名为'.'和'..'的情况
		if (this_name.name[0] == '.') {
			switch (this_name.len) {
			case 1: continue;	//直接进行下一轮循环
			case 2: {
				if (this_name.name[1] == '.') {
					follow_dotdot(&nd->mnt, &nd->dentry);	//回溯到父目录项
					continue;	//直接进行下一轮循环
				}
				else
					break;		//继续本轮循环
			}
			default:
				break;			//继续本轮循环
			}
		}

		//拿着路径名和父目录信息去找该路径对应的目录信息
		err = do_lookup(nd, &this_name, &this_path, LAST_DIR);
		if (err) {
			err = -ENOINODE;
			goto fail;
		}
		//kernel_printf("nd   position: %s\n", nd->dentry->d_name.name);
		//kernel_printf("path position: %s\n", this_path.dentry->d_name.name);

		//检查是否需要跨越文件系统
		err = follow_mount(&this_path.mnt, &this_path.dentry);
		if (err) {
			goto fail;
		}

		//kernel_printf("follow_mount() done.\n");

		//检查该目录项在物理上是否真实存在
		//由于这里是处理中间分量的，所以必须要存在对应的索引节点和查找方法
		this_inode = this_path.dentry->d_inode;
		if (this_inode == 0 || this_inode->i_op->lookup == 0) {
			err = -ENOINODE;
			goto fail;
		}

		//kernel_printf("inode check done.\n");

		//更新nd结构，进行下一轮循环
		nd->dentry = this_path.dentry;
		nd->mnt = this_path.mnt;
		//kernel_printf("nd   position: %s\n", nd->dentry->d_name.name);
		continue;
		//主循环到此结束

last_component_with_slash:
		//说明最后一个分量是目录而非一般意义的文件，如果原本要找一个普通文件，则报错
		if ((nd->flags & LOOKUP_DIRECTORY) == 0) {
			err = -ELASTTYPE;
			goto fail;
		}

last_component:
		//分只读模式和创建模式两种情况处理
		if (nd->flags & LOOKUP_CREATE)
			goto lookup_parent;

		//只读模式，继续解析该分量
		//对于最后一个分量名是'.'或'..'的情况，查找目录和文件应该有两种应对策略
		if (nd->flags & LOOKUP_DIRECTORY) {
			//如果是查找目录，则允许这种表达方式
			if (this_name.name[0] == '.') {
				switch (this_name.len) {
				case 1: goto done;		//直接完成
				case 2: {
					if (this_name.name[1] == '.') {
						follow_dotdot(&nd->mnt, &nd->dentry);	//回溯到父目录项
						goto done;		//直接完成
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
			//如果是查找文件，则不允许这种表达方式
			err = 0;
			switch (this_name.len) {
			case 1: err = -ENOPATH;  break;		//报错
			case 2: {
				if (this_name.name[1] == '.') {
					err = -ENOPATH;	 break;		//报错
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
		//拿着路径名和父目录信息去找该路径对应的目录信息
		u32 type;
		type = (nd->flags & LOOKUP_DIRECTORY) ? LAST_DIR : LAST_NORM;
		err = do_lookup(nd, &this_name, &this_path, type);
		if (err) {
			err = -ENOINODE;
			goto fail;
		}

		/*kernel_printf("nd   position: %s\n", nd->dentry->d_name.name);
		kernel_printf("path position: %s\n", this_path.dentry->d_name.name);*/

		//检查是否需要跨越文件系统
		err = follow_mount(&this_path.mnt, &this_path.dentry);
		if (err) {
			goto fail;
		}

		//kernel_printf("follow_mount() done.\n");

		//检查该目录项在物理上是否真实存在
		//这里需要分目录和文件两种情况来检验
		this_inode = this_path.dentry->d_inode;
		if (this_inode == 0) {
			err = -ENOINODE;
			goto fail;
		}
		if ((nd->flags & LOOKUP_DIRECTORY) && (this_inode->i_op->lookup == 0)) {
			//是目录但是没有响应的查找接口函数也不行
			err = -ENOINODE;
			goto fail;
		}

		//kernel_printf("inode check done.\n");

		//更新nd结构，返回
		nd->dentry = this_path.dentry;
		nd->mnt = this_path.mnt;
		//kernel_printf("final nd position: %s\n", nd->dentry->d_name.name);
		goto done;

lookup_parent:
		//创建模式
		//此时nd的dentry和mnt已经对应倒数第二个分量了，这里只需对nd的last字段赋值
		nd->last.name = (u8*)kmalloc(sizeof(u8)*(this_name.len + 1));
		if (nd->last.name == 0) {
			err = -ENOMEM;
			goto fail;
		}
		kernel_memcpy(nd->last.name, this_name.name, this_name.len);
		nd->last.name[this_name.len] = 0;
		nd->last.len = this_name.len;

		//分类讨论nd的last_type赋值
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

//根据目录项和挂载信息，以及vfs_open的参数flags，构造并初始化出一个文件指针
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

//试图返回到父目录项
//需要考虑跨越文件系统的情况
void follow_dotdot(struct vfsmount **p_mnt, struct dentry **p_dentry) {
	struct dentry	*old_d;
	struct vfsmount	*old_mnt;
	
	while (1) {
		old_d = *p_dentry;
		old_mnt = *p_mnt;
		//情况一：当前所在位置是全局的根目录，直接退出
		if ((old_d == root_dentry) && (*p_mnt == root_mount)) {
			break;
		}

		//情况二：当前所在位置不是当前文件系统的根，直接取父目录项即可
		if (old_d != (old_mnt->mnt_root)) {
			*p_dentry = old_d->d_parent;
			break;
		}
		
		//情况三：当前所在位置正是当前文件系统的根，但不是全局的根
		//这时需要先跨越到父文件系统，再进行一次循环
		*p_mnt = old_mnt->mnt_parent;
		*p_dentry = old_mnt->mnt_mountpoint;
	}
}

//根据父目录项和该文件名，找到该文件的目录项和挂载信息，放入path中传回
u32 do_lookup(struct nameidata *nd, struct qstr *name, struct path *path, u32 type) {
	struct dentry		*p_dentry;
	struct dentry		*this;
	struct dentry		*new;
	struct inode		*p_inode;
	struct condition	cond;
	//kernel_printf("Welcome to do_lookup().\n");
	//先试着从dcache中找
	p_dentry = nd->dentry;
	cond.cond1 = (void*)name;
	cond.cond2 = (void*)p_dentry;
	this = (struct dentry*)dcache->c_op->look_up(dcache, &cond);
	if (this)
		goto find_done;
	//kernel_printf("cannot find the dentry.\n");
	//缓存中没有，试着调用底层接口从外存中寻找
	//分配空间并初始化一个目录项出来
	new = d_alloc(name, p_dentry);
	if (new == 0)
		goto find_fail;

	p_inode = p_dentry->d_inode;
	this = p_inode->i_op->lookup(p_inode, new, type);	//必须要是目录类型的
	
	if (this) {
		//外存查找成功
		//kernel_printf("i_op->lookup() done.\n");
		goto find_done;
	}
	else {
		//外存查找失败，即彻底失败
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
	//注意：这里假设p_dentry不是NULL！
	struct list_head	*head;
	struct list_head	*p;
	struct vfsmount		*mnt;
	struct dentry		*dentry;
	u8	*buf;

	dentry = (struct dentry*)kmalloc(sizeof(struct dentry));
	if (dentry == 0)
		return dentry;

	//文件名的数据空间不能共享，必须拷贝一份
	buf = (u8*)kmalloc(sizeof(u8)*(name->len + 1));
	if (buf == 0)
		return 0;
	kernel_memcpy(buf, name->name, name->len);
	buf[name->len] = 0;

	dentry->d_name.name = buf;
	dentry->d_name.len = name->len;
	dentry->d_count = 1;
	dentry->d_mounted = 0;					//暂定
	dentry->d_inode = 0;					//暂定
	dentry->d_pinned = 0;
	dentry->d_sb = p_dentry->d_sb;
	dentry->d_op = p_dentry->d_op;
	dentry->d_parent = p_dentry;
	INIT_LIST_HEAD(&(dentry->d_alias));		//暂定
	INIT_LIST_HEAD(&(dentry->d_child));		//暂定
	INIT_LIST_HEAD(&(dentry->d_hash));		//暂定
	//INIT_LIST_HEAD(&(dentry->d_LRU));		//暂定
	INIT_LIST_HEAD(&(dentry->d_subdirs));

	//考虑dentry->mount的赋值
	head = &root_mount->mnt_hash;
	p = head;
	while (1) {
		mnt = container_of(p, struct vfsmount, mnt_hash);
		if (mnt->mnt_mountpoint == dentry) {
			//发现有文件系统挂载到当前目录项上，则修改d_mount值
			dentry->d_mounted = 1;
			break;
		}
		p = p->next;
		if (p == head)
			break;
	}

	//考虑dentry->child的赋值
	list_add(&(dentry->d_child), &(p_dentry->d_subdirs));

	//放进缓存中，同时对hash赋值
	dcache->c_op->add(dcache, (void*)dentry);

	//对inode和alias的赋值需要等到端口函数中实现
	return dentry;
}

//根据父目录和名字查找对应的目录项（创建模式专用）
//该函数是创建文件的一个环节，也就是把带创建文件的dentry准备好
struct dentry * do_lookup_create(struct qstr *name, struct dentry *p_dentry, u32 type) {
	struct dentry		*dentry;
	struct dentry		*new;
	struct inode		*p_inode;
	struct condition	cond;

	p_inode = p_dentry->d_inode;

	//先去缓存里找一遍看有没有
	cond.cond1 = (void*)name;
	cond.cond2 = (void*)p_dentry;
	dentry = dcache->c_op->look_up(dcache, &cond);
	if (dentry)
		return dentry;

	//如果没有，试图从外存中载入
	new = d_alloc(name, p_dentry);	//此函数已经把new放入缓存中去
	if (new == 0) {
		//内存申请失败
		return 0;
	}
	dentry = p_inode->i_op->lookup(p_inode, new, type);		//和最后一项的类型保持一致
	if (dentry == 0) {
		//外存中也没有，需要创建，把还缺少inode的目录项“半成品”new返回
		return new;
	}
	else {
		//外存中已经存在，不必再创建它，返回“成品”dentry
		//由于接口函数的实现方式，dentry和new现在指向同一块数据区域
		//此时的dentry->d_count应该为1，保持计数使其保留在缓存中
		return dentry;
	}
}

u32 vfs_close(struct file *filp) {
	u32 err;
	//把该文件仍在页缓存中的页写回磁盘
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
		dcache->c_op->del(dcache, (void*)dentry);	//踢出缓存，释放空间
	}
}

//比较dentry的名字和direntry的文件名是否匹配，返回0代表匹配
//类似于strcmp的函数
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
