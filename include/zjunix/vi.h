#ifndef _ZJUNIX_VI_H
#define _ZJUNIX_VI_H

#include <zjunix/vfs.h>

#define BUFFER_SIZE 4096
#define ROW_LEN 29
#define COL_LEN 80

#define KEY_DEL 0x8
#define KEY_TAB 0x9
#define KEY_SPC 0x20
#define KEY_ESC	0x1b
#define KEY_ENTER_N 0xa
#define KEY_ENTER_R 0xd

#define MODE_TEXT(x)	"MODE"[(x) & 3]

//COLOR_背景_文字
#define COLOR_BLACK_WHITE 0x00000FFF
#define COLOR_WHITE_BLACK 0x0FFF0000
#define COLOR_BLACK_BLUE  0x0000000F
#define COLOR_GREEN_WHITE 0x00F00FFF
#define COLOR_STATEBAR	  0x05520135

//vi控制块
struct victrl {
	u8	filename[FILENAME_MX_LEN];	//保存文件路径及文件名
	u8	buffer[BUFFER_SIZE];		//文本内容缓冲区，暗示文件大小最大是BUFFER_SIZE
	u8	inst[COL_LEN];				//指令缓冲区，在指令模式下会用到
	u32 inst_len;					//指令字符串的长度
	u32 old_size;					//文件原来的大小
	u32	size;						//有效缓冲区实际大小，随用户编辑动态变化
	u32 cursor_loc;					//光标在缓冲区(不是在屏幕)中的索引位置
	u32 page_begin_loc;				//屏幕上首地址在缓冲区中的索引位置
	u32 page_end_loc;				//屏幕上末地址在缓冲区中的索引位置
	u32 mode;						//当前模式，共有三种
	u32 again;						//可以进行下一次循环的标志
	u32 err;						//保存错误码
};

//vi.c
u32 vi(const u8* );
void init_vi(const u8* );
u32 load_file(u8* );
void screen_flush(void);
void put_char_on_screen(u8, u32, u32, u32);
void do_command_mode(u8);
void do_insert_mode(u8);
void do_last_line_mode(u8);
void insert_key(u8, u32);
void delete_key(u32);
void page_location_last_line(void);
void page_location_next_line(void);
void cursor_last_line(void);
void cursor_next_line(void);
u32 vfs_save(u8*);

#endif