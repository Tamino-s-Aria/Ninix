#include <zjunix/vi.h>
#include <zjunix/utils.h>
#include <driver/ps2.h>
#include <driver/vga.h>

//全局变量：vi控制块
struct victrl vc;
extern int cursor_freq;

u32 vi(const u8* filename) {
	u8  key;
	u32 err;
	u32 pre_cursor_freq;

	//初始化vi控制块信息
	init_vi(filename);

	//调整光标闪烁频率为零(不闪烁)
	pre_cursor_freq = cursor_freq;
	cursor_freq = 0;
	kernel_set_cursor();

	//加载文件内容至缓冲区内
	err = load_file(vc.filename);
	if (err) {
		/*kernel_printf("Load this file failed. Error code: %d\n", -err);*/
		return err;
	}

	//首次把缓冲区中的内容展示到屏幕上
	screen_flush();

	while (vc.again) {
		//接收用户键盘输入
		key = kernel_getchar();

		//根据不同的模式采取不同的措施
		switch (vc.mode) {
		case 0: do_command_mode(key);	break;
		case 1: do_insert_mode(key);		break;
		case 2: do_last_line_mode(key);	break;
		default: break;
		}
	}

	//恢复光标设置并清屏
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
	//打开文件，这里默认不能新建文件，如果要新建要先用touch命令
	filp = vfs_open(file_path, O_RDONLY, 0);
	if (IS_ERR_OR_NULL(filp)) {
		return PTR_ERR(filp);
	}

	//检查文件大小是否合适
	filesize = filp->f_dentry->d_inode->i_size;
	if (filesize >= BUFFER_SIZE) {
		err = vfs_close(filp);
		err = -ESIZE;
		return err;
	}
	vc.old_size = filesize;
	vc.size = filesize;

	//读取文件信息
	readsize = filp->f_op->read(filp, vc.buffer, vc.size);
	if (readsize < vc.size) {
		err = -EREAD;
		return err;
	}

	//确保缓冲区总是以换行符结尾
	vc.buffer[vc.size] = KEY_ENTER_N;
	vc.size++;

	//关闭文件
	err = vfs_close(filp);
	return err;
}

void screen_flush(void) {
	u32 loc;		//正在处理的位置在缓冲区内的索引
	u32 color;
	u32 row, col;
	u8 c;

	loc = vc.page_begin_loc;	//从文本的此处开始显示
	row = col = 0;				//屏幕上的行列坐标清零

	//将文本(的一部分)显示在屏幕上
	while (row < ROW_LEN && loc < vc.size) {
		//根据光标位置，确定当前颜色
		color = (loc == vc.cursor_loc) ? COLOR_WHITE_BLACK : COLOR_BLACK_WHITE;

		//取到当前字符，分情况处理
		c = vc.buffer[loc];
		switch (c)
		{
		case KEY_ENTER_N: {
			put_char_on_screen(KEY_SPC, row, col, color);		//在回车处打印空格
			color = COLOR_BLACK_WHITE;
			for (++col; col < COL_LEN; col++)
				put_char_on_screen(KEY_SPC, row, col, color);	//该行之后的内容全部清空
			col = 0;
			row++;
			break;
		}
		case KEY_ENTER_R:
			break;
		case KEY_TAB: {
			u32 dst_col = (col & 0xfffffffc) + 4;				//制表符终止处
			put_char_on_screen(KEY_SPC, row, col, color);		//在制表符处打印空格
			color = COLOR_BLACK_WHITE;
			for (++col; col < dst_col; col++)
				put_char_on_screen(KEY_SPC, row, col, color);
			break;
		}
		default:
			//常规字符
			put_char_on_screen(c, row, col, color);
			col++;
			break;
		}

		//若一行填满，自动换行
		if (col >= COL_LEN) {
			col = 0;
			row++;
		}

		//迭代
		loc++;
	}
	vc.page_end_loc = loc;		//标记显示末尾处

	//若有空行，在每个空行的开头打印一个‘~’
	if (row < ROW_LEN) {
		//如果条件成立，说明已经展示到缓冲区的末尾
		//根据load_file()函数的定义，缓冲区的最后一个字符一定是KEY_ENTER_N
		//所以当前row和col一定代表不是文件部分的首行开头
		for (; row < ROW_LEN; row++) {
			col = 0;
			put_char_on_screen('~', row, col, COLOR_BLACK_BLUE);
			for (++col; col < COL_LEN; col++)
				put_char_on_screen(KEY_SPC, row, col, COLOR_BLACK_WHITE);
		}
	}

	//打印最后一行即命令行和模式部分
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
			screen_flush();		//更新vc.page_end
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

	//调整页面窗口的位置
	if (vc.cursor_loc < vc.page_begin_loc)
		page_location_last_line();
	if (vc.cursor_loc >= vc.page_end_loc)
		page_location_next_line();

	//更新屏幕
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
			screen_flush();	//更新vc.page_end
		}
		break;

	default:
		insert_key(key, vc.cursor_loc);
		vc.cursor_loc++;
		screen_flush();		//更新vc.page_end
		break;
	}

	//调整页面窗口的位置
	if (vc.cursor_loc < vc.page_begin_loc)
		page_location_last_line();
	if (vc.cursor_loc >= vc.page_end_loc)
		page_location_next_line();

	//更新屏幕
	screen_flush();
}

void do_last_line_mode(u8 key) {
	//规范成小写
	if (key >= 'A' && key <= 'Z')
		key = key - 'A' + 'a';

	switch (key) {
	case KEY_ESC:
		vc.mode = 0;
		kernel_memset(vc.inst, 0, sizeof(u8) * COL_LEN);
		vc.inst_len = 0;
		break;

	case KEY_ENTER_N:
		//确认命令
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

	//更新屏幕
	screen_flush();
}

void insert_key(u8 key, u32 site) {
	//检查缓冲区容量
	if (vc.size == BUFFER_SIZE) {
		return;
	}
	for (int i = vc.size - 1; i >= (int)site; i--)
		vc.buffer[i + 1] = vc.buffer[i];
	vc.size++;
	vc.buffer[site] = key;
}

void delete_key(u32 site) {
	//检查缓冲区容量
	if (vc.size == 0) {
		return;
	}

	for (u32 i = site; i < vc.size - 1; i++)
		vc.buffer[i] = vc.buffer[i + 1];
	vc.buffer[vc.size - 1] = KEY_SPC;
	vc.size--;
}

void page_location_last_line() {
	//page_begin只可能指向某行的行首

	int loc;
	if (vc.page_begin_loc == 0) {
		//该行为首行，不用变动
		return;
	}
	else {
		//存在上一行
		loc = vc.page_begin_loc - 2;
		//此时loc可能指向上上行的行末处，也可能指向上一行的非行末处

		//试图找到上上行的行末回车处
		while (loc >= 0 && vc.buffer[loc] != KEY_ENTER_N)
			loc--;
		if (loc < 0) {
			//说明上一行就是首行
			vc.page_begin_loc = 0;
		}
		else {
			//loc正指向上上行的行末，加一即可得到上一行的行首
			vc.page_begin_loc = loc + 1;
		}
	}
}

void page_location_next_line() {
	//page_begin只可能指向某行的行首

	u32 loc = vc.page_begin_loc;

	//vc.buffer[vc.size-1] == KEY_ENTER_N
	while (loc < vc.size - 1 && vc.buffer[loc] != KEY_ENTER_N) {
		loc++;
	}

	if (loc >= vc.size - 1) {
		//如果没有下一行(包括文末)，直接返回
		return;
	}
	else {
		//loc已经到达该段落的末尾，应该把光标移动到下一行的行首处
		vc.page_begin_loc = loc + 1;
	}
}

void cursor_last_line() {
	int loc = vc.cursor_loc - 1;	//此时loc可能指向上一行的行末，也可能指向本行除行末外的地址
	
	//先找到上一行的行末回车处
	while (loc >= 0 && vc.buffer[loc] != KEY_ENTER_N)
		loc--;

	if (loc < 0) {
		//说明本行就是首行，只需要把光标移到行首即可
		vc.cursor_loc = 0;
	}
	else if (loc == vc.cursor_loc - 1) {
		//说明原光标处为某行行首，loc正指向上一行行末处
		//那么光标应该移到上一行的行首

		loc--;		//此时loc可能指向上上行的行末，也可能指向上一行除行末外的地址

		//试图找到上上行的行末回车处
		while (loc >= 0 && vc.buffer[loc] != KEY_ENTER_N)
			loc--;
		if (loc < 0) {
			//说明上一行就是首行
			vc.cursor_loc = 0;
		}
		else {
			//loc正指向上上行的行末
			vc.cursor_loc = loc + 1;
		}
	}
	else {
		//说明原光标处为某行行中，loc正指向上一行行末处
		//那么光标应该移到本行行首
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
		//如果没有下一行(包括文末)，直接返回
		return;
	}
	else {
		//loc已经到达该段落的末尾，应该把光标移动到下一行的行首处
		vc.cursor_loc = loc + 1;
	}
}

u32 vfs_save(u8* filename) {
	struct file*	filp;
	u32 writesize;
	u32 err;

	//kernel_printf("Welcome to vfs_save()\n");

	//打开文件
	filp = vfs_open(filename, O_WRONLY, 0);
	if (IS_ERR_OR_NULL(filp)) {
		return PTR_ERR(filp);
	}

	//把缓冲区的内容写入文件
	if (vc.size - 1 > vc.old_size)
		writesize = vc.size - 1;
	else
		writesize = vc.old_size;

	if (filp->f_op->write(filp, vc.buffer, writesize) != writesize) {
		err = -EWRITE;
		err = vfs_close(filp);
		return err;
	}

	//关闭文件
	//kernel_printf("filp->pos:%d\n", filp->f_pos);
	//while (1);
	err = vfs_close(filp);
	return err;
}
