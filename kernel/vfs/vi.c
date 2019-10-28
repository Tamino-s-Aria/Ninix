#include <zjunix/vi.h>
#include <zjunix/utils.h>
#include <driver/ps2.h>
#include <driver/vga.h>

//ȫ�ֱ�����vi���ƿ�
struct victrl vc;
extern int cursor_freq;

u32 vi(const u8* filename) {
	u8  key;
	u32 err;
	u32 pre_cursor_freq;

	//��ʼ��vi���ƿ���Ϣ
	init_vi(filename);

	//���������˸Ƶ��Ϊ��(����˸)
	pre_cursor_freq = cursor_freq;
	cursor_freq = 0;
	kernel_set_cursor();

	//�����ļ���������������
	err = load_file(vc.filename);
	if (err) {
		/*kernel_printf("Load this file failed. Error code: %d\n", -err);*/
		return err;
	}

	//�״ΰѻ������е�����չʾ����Ļ��
	screen_flush();

	while (vc.again) {
		//�����û���������
		key = kernel_getchar();

		//���ݲ�ͬ��ģʽ��ȡ��ͬ�Ĵ�ʩ
		switch (vc.mode) {
		case 0: do_command_mode(key);	break;
		case 1: do_insert_mode(key);		break;
		case 2: do_last_line_mode(key);	break;
		default: break;
		}
	}

	//�ָ�������ò�����
	cursor_freq = pre_cursor_freq;
	kernel_set_cursor();
	kernel_clear_screen(31);
	return vc.err;
}

void init_vi(const u8* filename) {
	kernel_memset(vc.filename, 0, FILENAME_MX_LEN);
	kernel_strcpy(vc.filename, filename);
	kernel_memset(vc.buffer, 0, BUFFER_SIZE);
	kernel_memset(vc.inst, 0, COL_LEN);
	vc.inst_len = 0;
	vc.cursor_loc = 0;
	vc.page_begin_loc = 0;
	vc.page_end_loc = 0;
	vc.mode = 0;
	vc.old_size = 0;
	vc.size = 0;
	vc.again = 1;
	vc.err = 0;
}

u32 load_file(u8* file_path) {
	struct file*	filp;
	u32 readsize;
	u32 filesize;
	u32 err;
	//���ļ�������Ĭ�ϲ����½��ļ������Ҫ�½�Ҫ����touch����
	filp = vfs_open(file_path, O_RDONLY, 0);
	if (IS_ERR_OR_NULL(filp)) {
		return PTR_ERR(filp);
	}

	//����ļ���С�Ƿ����
	filesize = filp->f_dentry->d_inode->i_size;
	if (filesize >= BUFFER_SIZE) {
		err = vfs_close(filp);
		err = -ESIZE;
		return err;
	}
	vc.old_size = filesize;
	vc.size = filesize;

	//��ȡ�ļ���Ϣ
	readsize = filp->f_op->read(filp, vc.buffer, vc.size);
	if (readsize < vc.size) {
		err = -EREAD;
		return err;
	}

	//ȷ�������������Ի��з���β
	vc.buffer[vc.size] = KEY_ENTER_N;
	vc.size++;

	//�ر��ļ�
	err = vfs_close(filp);
	return err;
}

void screen_flush(void) {
	u32 loc;		//���ڴ����λ���ڻ������ڵ�����
	u32 color;
	u32 row, col;
	u8 c;

	loc = vc.page_begin_loc;	//���ı��Ĵ˴���ʼ��ʾ
	row = col = 0;				//��Ļ�ϵ�������������

	//���ı�(��һ����)��ʾ����Ļ��
	while (row < ROW_LEN && loc < vc.size) {
		//���ݹ��λ�ã�ȷ����ǰ��ɫ
		color = (loc == vc.cursor_loc) ? COLOR_WHITE_BLACK : COLOR_BLACK_WHITE;

		//ȡ����ǰ�ַ������������
		c = vc.buffer[loc];
		switch (c)
		{
		case KEY_ENTER_N: {
			put_char_on_screen(KEY_SPC, row, col, color);		//�ڻس�����ӡ�ո�
			color = COLOR_BLACK_WHITE;
			for (++col; col < COL_LEN; col++)
				put_char_on_screen(KEY_SPC, row, col, color);	//����֮�������ȫ�����
			col = 0;
			row++;
			break;
		}
		case KEY_ENTER_R:
			break;
		case KEY_TAB: {
			u32 dst_col = (col & 0xfffffffc) + 4;				//�Ʊ����ֹ��
			put_char_on_screen(KEY_SPC, row, col, color);		//���Ʊ������ӡ�ո�
			color = COLOR_BLACK_WHITE;
			for (++col; col < dst_col; col++)
				put_char_on_screen(KEY_SPC, row, col, color);
			break;
		}
		default:
			//�����ַ�
			put_char_on_screen(c, row, col, color);
			col++;
			break;
		}

		//��һ���������Զ�����
		if (col >= COL_LEN) {
			col = 0;
			row++;
		}

		//����
		loc++;
	}
	vc.page_end_loc = loc;		//�����ʾĩβ��

	//���п��У���ÿ�����еĿ�ͷ��ӡһ����~��
	if (row < ROW_LEN) {
		//�������������˵���Ѿ�չʾ����������ĩβ
		//����load_file()�����Ķ��壬�����������һ���ַ�һ����KEY_ENTER_N
		//���Ե�ǰrow��colһ���������ļ����ֵ����п�ͷ
		for (; row < ROW_LEN; row++) {
			col = 0;
			put_char_on_screen('~', row, col, COLOR_BLACK_BLUE);
			for (++col; col < COL_LEN; col++)
				put_char_on_screen(KEY_SPC, row, col, COLOR_BLACK_WHITE);
		}
	}

	//��ӡ���һ�м������к�ģʽ����
	for (col = 0; col < COL_LEN; col++) {
		if (col < vc.inst_len)
			put_char_on_screen(vc.inst[col], ROW_LEN, col, COLOR_STATEBAR);
		else if ((col / 4) == (COL_LEN / 8))
			//col == 40, 41, 42, 43
			put_char_on_screen(MODE_TEXT(col), ROW_LEN, col, COLOR_STATEBAR);
		else if (col == 45)
			put_char_on_screen((vc.mode + '0'), ROW_LEN, col, COLOR_STATEBAR);
		else
			put_char_on_screen(KEY_SPC, ROW_LEN, col, COLOR_STATEBAR);
	}
}

void put_char_on_screen(u8 ch, u32 row, u32 column, u32 color) {
	kernel_putchar_at(ch, color & 0xFFFF, (color >> 16) & 0xFFFF, row, column);
}

void do_command_mode(u8 key) {	
	switch (key) {
	case 'h': 
		if (vc.cursor_loc > 0 && vc.buffer[vc.cursor_loc - 1] != KEY_ENTER_N)
			vc.cursor_loc--;
		break;

	case 'j': 
		cursor_next_line();
		break;

	case 'k': 
		cursor_last_line();
		break;

	case 'l': 
		if (vc.cursor_loc < vc.size && vc.buffer[vc.cursor_loc] != KEY_ENTER_N)
			vc.cursor_loc++;
		break;

	case 'x': 
		if (vc.cursor_loc < vc.size - 1) {
			delete_key(vc.cursor_loc);
			screen_flush();		//����vc.page_end
		}
		break;

	case 'i': 
		vc.mode = 1;
		break;

	case ':': 
		kernel_memset(vc.inst, KEY_SPC, sizeof(u8) * COL_LEN);
		vc.mode = 2;
		vc.inst[0] = ':';
		vc.inst_len = 1;
		break;

	default:
		break;
	}

	//����ҳ�洰�ڵ�λ��
	if (vc.cursor_loc < vc.page_begin_loc)
		page_location_last_line();
	if (vc.cursor_loc >= vc.page_end_loc)
		page_location_next_line();

	//������Ļ
	screen_flush();
}

void do_insert_mode(u8 key) {
	switch (key) {
	case KEY_ESC:
		vc.mode = 0;
		break;

	case KEY_DEL:
		if (vc.cursor_loc > 0) {
			delete_key(vc.cursor_loc - 1);
			vc.cursor_loc--;
			screen_flush();	//����vc.page_end
		}
		break;

	default:
		insert_key(key, vc.cursor_loc);
		vc.cursor_loc++;
		screen_flush();		//����vc.page_end
		break;
	}

	//����ҳ�洰�ڵ�λ��
	if (vc.cursor_loc < vc.page_begin_loc)
		page_location_last_line();
	if (vc.cursor_loc >= vc.page_end_loc)
		page_location_next_line();

	//������Ļ
	screen_flush();
}

void do_last_line_mode(u8 key) {
	//�淶��Сд
	if (key >= 'A' && key <= 'Z')
		key = key - 'A' + 'a';

	switch (key) {
	case KEY_ESC:
		vc.mode = 0;
		kernel_memset(vc.inst, 0, sizeof(u8) * COL_LEN);
		vc.inst_len = 0;
		break;

	case KEY_ENTER_N:
		//ȷ������
		if (vc.inst_len == 3 && vc.inst[1] == 'q' && vc.inst[2] == '!')
			vc.again = 0;
		else if (vc.inst_len == 3 && vc.inst[1] == 'w' && vc.inst[2] == 'q') {
			vc.err = vfs_save(vc.filename);
			vc.again = 0;
		}
		break;

	case KEY_DEL:
		if (vc.inst_len > 1) {
			vc.inst[--vc.inst_len] = KEY_SPC;
		}
		break;

	default:
		if (vc.inst_len < COL_LEN / 4) {
			vc.inst[vc.inst_len++] = key;
		}
		break;
	}

	//������Ļ
	screen_flush();
}

void insert_key(u8 key, u32 site) {
	//��黺��������
	if (vc.size == BUFFER_SIZE) {
		return;
	}
	for (int i = vc.size - 1; i >= (int)site; i--)
		vc.buffer[i + 1] = vc.buffer[i];
	vc.size++;
	vc.buffer[site] = key;
}

void delete_key(u32 site) {
	//��黺��������
	if (vc.size == 0) {
		return;
	}

	for (u32 i = site; i < vc.size - 1; i++)
		vc.buffer[i] = vc.buffer[i + 1];
	vc.buffer[vc.size - 1] = KEY_SPC;
	vc.size--;
}

void page_location_last_line() {
	//page_beginֻ����ָ��ĳ�е�����

	int loc;
	if (vc.page_begin_loc == 0) {
		//����Ϊ���У����ñ䶯
		return;
	}
	else {
		//������һ��
		loc = vc.page_begin_loc - 2;
		//��ʱloc����ָ�������е���ĩ����Ҳ����ָ����һ�еķ���ĩ��

		//��ͼ�ҵ������е���ĩ�س���
		while (loc >= 0 && vc.buffer[loc] != KEY_ENTER_N)
			loc--;
		if (loc < 0) {
			//˵����һ�о�������
			vc.page_begin_loc = 0;
		}
		else {
			//loc��ָ�������е���ĩ����һ���ɵõ���һ�е�����
			vc.page_begin_loc = loc + 1;
		}
	}
}

void page_location_next_line() {
	//page_beginֻ����ָ��ĳ�е�����

	u32 loc = vc.page_begin_loc;

	//vc.buffer[vc.size-1] == KEY_ENTER_N
	while (loc < vc.size - 1 && vc.buffer[loc] != KEY_ENTER_N) {
		loc++;
	}

	if (loc >= vc.size - 1) {
		//���û����һ��(������ĩ)��ֱ�ӷ���
		return;
	}
	else {
		//loc�Ѿ�����ö����ĩβ��Ӧ�ðѹ���ƶ�����һ�е����״�
		vc.page_begin_loc = loc + 1;
	}
}

void cursor_last_line() {
	int loc = vc.cursor_loc - 1;	//��ʱloc����ָ����һ�е���ĩ��Ҳ����ָ���г���ĩ��ĵ�ַ
	
	//���ҵ���һ�е���ĩ�س���
	while (loc >= 0 && vc.buffer[loc] != KEY_ENTER_N)
		loc--;

	if (loc < 0) {
		//˵�����о������У�ֻ��Ҫ�ѹ���Ƶ����׼���
		vc.cursor_loc = 0;
	}
	else if (loc == vc.cursor_loc - 1) {
		//˵��ԭ��괦Ϊĳ�����ף�loc��ָ����һ����ĩ��
		//��ô���Ӧ���Ƶ���һ�е�����

		loc--;		//��ʱloc����ָ�������е���ĩ��Ҳ����ָ����һ�г���ĩ��ĵ�ַ

		//��ͼ�ҵ������е���ĩ�س���
		while (loc >= 0 && vc.buffer[loc] != KEY_ENTER_N)
			loc--;
		if (loc < 0) {
			//˵����һ�о�������
			vc.cursor_loc = 0;
		}
		else {
			//loc��ָ�������е���ĩ
			vc.cursor_loc = loc + 1;
		}
	}
	else {
		//˵��ԭ��괦Ϊĳ�����У�loc��ָ����һ����ĩ��
		//��ô���Ӧ���Ƶ���������
		vc.cursor_loc = loc + 1;
	}
}

void cursor_next_line() {
	u32 loc = vc.cursor_loc;

	//vc.buffer[vc.size-1] == KEY_ENTER_N
	while (loc < vc.size - 1 && vc.buffer[loc] != KEY_ENTER_N) {
		loc++;
	}

	if (loc >= vc.size - 1) {
		//���û����һ��(������ĩ)��ֱ�ӷ���
		return;
	}
	else {
		//loc�Ѿ�����ö����ĩβ��Ӧ�ðѹ���ƶ�����һ�е����״�
		vc.cursor_loc = loc + 1;
	}
}

u32 vfs_save(u8* filename) {
	struct file*	filp;
	u32 writesize;
	u32 err;

	//kernel_printf("Welcome to vfs_save()\n");

	//���ļ�
	filp = vfs_open(filename, O_WRONLY, 0);
	if (IS_ERR_OR_NULL(filp)) {
		return PTR_ERR(filp);
	}

	//�ѻ�����������д���ļ�
	if (vc.size - 1 > vc.old_size)
		writesize = vc.size - 1;
	else
		writesize = vc.old_size;

	if (filp->f_op->write(filp, vc.buffer, writesize) != writesize) {
		err = -EWRITE;
		err = vfs_close(filp);
		return err;
	}

	//�ر��ļ�
	//kernel_printf("filp->pos:%d\n", filp->f_pos);
	//while (1);
	err = vfs_close(filp);
	return err;
}
