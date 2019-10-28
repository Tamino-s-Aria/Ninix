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

//COLOR_����_����
#define COLOR_BLACK_WHITE 0x00000FFF
#define COLOR_WHITE_BLACK 0x0FFF0000
#define COLOR_BLACK_BLUE  0x0000000F
#define COLOR_GREEN_WHITE 0x00F00FFF
#define COLOR_STATEBAR	  0x05520135

//vi���ƿ�
struct victrl {
	u8	filename[FILENAME_MX_LEN];	//�����ļ�·�����ļ���
	u8	buffer[BUFFER_SIZE];		//�ı����ݻ���������ʾ�ļ���С�����BUFFER_SIZE
	u8	inst[COL_LEN];				//ָ���������ָ��ģʽ�»��õ�
	u32 inst_len;					//ָ���ַ����ĳ���
	u32 old_size;					//�ļ�ԭ���Ĵ�С
	u32	size;						//��Ч������ʵ�ʴ�С�����û��༭��̬�仯
	u32 cursor_loc;					//����ڻ�����(��������Ļ)�е�����λ��
	u32 page_begin_loc;				//��Ļ���׵�ַ�ڻ������е�����λ��
	u32 page_end_loc;				//��Ļ��ĩ��ַ�ڻ������е�����λ��
	u32 mode;						//��ǰģʽ����������
	u32 again;						//���Խ�����һ��ѭ���ı�־
	u32 err;						//���������
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