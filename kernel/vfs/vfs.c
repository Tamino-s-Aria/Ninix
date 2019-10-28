#include <zjunix/vfs.h>
#include <zjunix/vfscache.h>
#include <zjunix/errno.h>
#include <driver/vga.h>

#include <zjunix/fat32.h>
#include <zjunix/ext2.h>

//全局变量
struct dentry* 			root_dentry;		//全局的根目录项
struct dentry* 			pwd_dentry;			//当前所在目录项
struct vfsmount* 		root_mount;			//全局的根挂载信息
struct vfsmount* 		pwd_mount;			//当前所在挂载信息
extern struct cache		*dcache;			//dentry缓存
extern struct cache		*pcache;			//page缓存

void init_vfs(void){
	u8  MBR[BYTES_PER_SECT];
	u32 err;
	u32 base;
	u32 i;
	u32 fs_id;
	u32 fs_base;

	//初始化各缓存
	err = init_cache();
	if (err) {
		kernel_printf("Cache allocation fail!\n");
		goto vfs_init_err;
	}

	//读取MBR扇区的数据
	err = read_block(MBR, 0, 1);
	if (err) {
		err = -EIO;
		kernel_printf("Read MBR error!\n");
		goto vfs_init_err;
	}

	//分别对四个分区初始化其对应的文件系统
	base = 0x1be;
	for (i = 0; i < MAX_PARTITION; i++) {
		fs_id = MBR[base + 4];
		kernel_memcpy(&fs_base, MBR + base + 8, sizeof(u32));
		if (fs_id == 0x0b || fs_id == 0x0c) {
			err = fat32_init(fs_base);
		}
		else if (fs_id == 0x83) {
			err = ext2_init(fs_base);
		}
		else {
			err = 0;
		}

		if (err) {
			kernel_printf("Partition %d init error!\n", i);
			goto vfs_init_err;
		}
		base += 16;
	}

	//把另外一个文件系统挂载到FAT32系统上
	err = mount_ext2();
	if (err) {
		kernel_printf("Ext2 fs mount fail!\n");
		goto vfs_init_err;
	}

	//初始化当前路径
	dget(root_dentry);
	pwd_dentry = root_dentry;
	pwd_mount = root_mount;

	//正常返回路径
	return;

vfs_init_err:
	//异常返回路径
	kernel_printf("vfs_init() aborted. Error code is %d\n", -err);
	return;
}