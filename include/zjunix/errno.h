#ifndef _ZJUNIX_ERRNO_H
#define _ZJUNIX_ERRNO_H

#define ENOMEM		512		//申请内存空间失败
#define EIO			513		//磁盘读写出错
#define ENOINODE	514		//读取索引节点信息出错
#define ENOALLOCATE	515		//需要用到的指针字段未分配空间
#define ENOMAPPING	516		//没有地址空间
#define ENOVFSMNT	517		//没有找到对应的挂载信息
#define ENOPATH		518		//路径格式无效
#define ENOPAGE		519		//没有找到对应的页
#define	ELASTTYPE	520		//最后一个分量的类型错误
#define EREAD		521		//读取时出错
#define EWRITE		522		//写回时出错
#define ESIZE		523		//文件大小不合适(过大)
#define ENODENTRY	524		//没有对应的目录项
#define	EDPTTABLE	525		//描述符加载失败

#endif