#include <arch.h>
#include <driver/ps2.h>
#include <driver/vga.h>
#include <exc.h>
#include <intr.h>
#include "../include/zjunix/page.h"
#include <zjunix/bootmm.h>
#include <zjunix/buddy.h>
#include <zjunix/vfs.h>
#include <zjunix/vfscache.h>
#include <zjunix/log.h>
#include <zjunix/pc.h>
#include <zjunix/slab.h>
#include <zjunix/syscall.h>
#include <zjunix/time.h>
#include "../usr/ps.h"
#include <init_place_holder.h>

void machine_info() {
    int row;
    int col;
    kernel_printf("\n%s\n", "Group_Ninix");
    row = cursor_row;
    col = cursor_col;
    cursor_row = 29;
    kernel_printf("%s", "Created by Group 9, Zhejiang University.");
    cursor_row = row;
    cursor_col = col;
    kernel_set_cursor();
}

#pragma GCC push_options
#pragma GCC optimize("O0")
void create_startup_process() {
	int res;

	res = do_fork("kernel_shell", (void*)ps, 0, 0, 55);
	//kernel_printf("debug: res = %d\n", res);
	if (res != 0)
		kernel_printf("create startup process failed!\n");
	else
		kernel_printf("kernel shell created!\n");
}
#pragma GCC pop_options

void init_kernel() {
    kernel_clear_screen(31);
    // Exception
    init_exception();
    // Page table
    init_pgtable();
    // Drivers
    init_vga();
    init_ps2();
    // Memory management
    log(LOG_START, "Memory Modules.");
    init_bootmm();
    log(LOG_OK, "Bootmem.");
	show_bootmm();
    init_buddy();
    log(LOG_OK, "Buddy.");
    init_slab();
    log(LOG_OK, "Slab.");
    log(LOG_END, "Memory Modules.");
    // File system
    log(LOG_START, "Virtual File System.");
	init_vfs();
    log(LOG_END, "Virtual File System.");
    // System call
    log(LOG_START, "System Calls.");
    init_syscall();
    log(LOG_END, "System Calls.");
    // Process control
    log(LOG_START, "Process Control Module.");
    init_pc();
    create_startup_process();
    log(LOG_END, "Process Control Module.");
    // Interrupts
    log(LOG_START, "Enable Interrupts.");
    init_interrupts();
    log(LOG_END, "Enable Interrupts.");
    // Init finished
    machine_info();
    *GPIO_SEG = 0x11223344;
    // Enter shell
    while (1)
        ;
}
