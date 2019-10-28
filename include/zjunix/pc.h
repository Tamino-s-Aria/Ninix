#ifndef _ZJUNIX_PC_H
#define _ZJUNIX_PC_H

#include <zjunix/list.h>
#include <zjunix/pid.h>
#include <zjunix/vm.h>
#include <zjunix/fat32.h>
#include <zjunix/vfs.h>

#define  KERNEL_STACK_SIZE  4096
#define  TASK_NAME_LEN   32

//进程状态
#define  TASK_UNINIT    0
#define  TASK_READY    1
#define  TASK_RUNNING  2
#define  TASK_WAITING  3
#define  TASK_TERMINAL 4

//调度策略
//实时进程用FIFO 和 RR
//普通进程用normal
#define SCHED_NORMAL		0
#define SCHED_FIFO		1
#define SCHED_RR		2

#define MIN_TIMESLICE		1
#define DEF_TIMESLICE		20
#define MAX_TIMESLICE       150
#define MIN_RT_TIMESLICE    500
#define MAX_RT_TIMESLICE    1200

#define CHILD_PENALTY		 60
#define PARENT_PENALTY		100

#define PRIO_BONUS_RATIO	 25

//#define INTERACTIVE_DELTA	  2
#define STARVATION_LIMIT	(MAX_SLEEP_AVG)

//0-49 给实时进程
//50-63给普通进程
#define MAX_PRIO 64
#define MAX_RT_PRIO    49
#define INIT_PRIO  55
#define MAX_SLEEP_AVG  5555
#define MAX_BONUS 10
#define BITMAP_SIZE ((MAX_PRIO+7)/8)
#define MAX_STARVE_COUNT  3


//寄存器
struct regs_context {
	unsigned int epc;
	unsigned int at;
	unsigned int v0, v1;
	unsigned int a0, a1, a2, a3;
	unsigned int t0, t1, t2, t3, t4, t5, t6, t7;
	unsigned int s0, s1, s2, s3, s4, s5, s6, s7;
	unsigned int t8, t9;
	unsigned int hi, lo;
	unsigned int gp, sp, fp, ra;
};
typedef struct regs_context context;

//综合了助教的源码和Linux的task_strct作为PCB
struct task_struct {
	unsigned int pid;              //进程pid号
	unsigned char name[TASK_NAME_LEN];  //进程名
	unsigned int parent;           //父进程pid号
	int ASID;               //进程地址空间id号
	int state;              //进程状态  

	struct regs_context context;    //进程寄存器信息
	struct mm_struct *mm;           //进程地址空间结构指针
	struct file *task_files;               //进程打开文件指针

	unsigned long static_prio;    //普通进程的静态优先级
	unsigned long prio;         //普通进程的动态优先级
	unsigned long rt_priority;  //实时进程优先级
	unsigned long sleep_avg; //平均睡眠时间，用来调整动态优先级
	unsigned long policy; //调度类型
	unsigned int starve_count;

	struct list_head position_in_processes; //进程在process中的位置 
	struct list_head position_in_runlist; //进程所在的那个优先级的运行队列中的位置
	struct prio_array *array;  //指向当前进程所在的链表，过期或者是活跃
	unsigned int timestamp; //最近一次的切换时间，上一次进程的切换时间
	unsigned int time_slice; //进程剩余的时间片
							 //	unsigned long last_ran;  //和timestamp很像，看用不用吧
};
typedef struct task_struct task_t;

union task_union {
	struct task_struct task;        //进程控制块
	unsigned char kernel_stack[KERNEL_STACK_SIZE];  //进程的内核栈
};

//优先级队列
struct prio_array {
	unsigned int nr_active; //进程数
	int highest_prio; //记录当前队列中的最高优先级（最低的优先级数值）
	unsigned char bitmap[BITMAP_SIZE];  //优先级位图 对应链表非空时置1
	struct list_head queue[MAX_PRIO]; //优先级队列的头节点
};
typedef struct prio_array prio_array_t;
extern struct task_struct *current_task;

void init_pc();
void init_pc_list();
void init_prio_array(struct prio_array *arraylist);
void init_idle(task_t *idle);
unsigned int sched_clock();
unsigned int do_fork(char *task_name,
	void(*entry)(unsigned int argc, void *args),
	unsigned int argc, void *args, int set_prio);
void sched_fork(task_t *p, int set_prio);
void wake_up_new_task(task_t * p);
int is_rt_task(struct task_struct* p);
void activate_task(task_t *p);
static void enqueue_task(task_t *p, prio_array_t *array);
static void requeue_task(task_t *p, prio_array_t *array);
static void dequeue_task(task_t *p, prio_array_t *array);
static void enqueue_task_head(task_t *p, prio_array_t *array);
void set_bit_map(int priority, prio_array_t* p_array);
void update_highest_pro(prio_array_t * arraylist, int priority, int is_enqueue);
void clear_bit_map(int priority, prio_array_t* p_array);
void activate_mm(struct task_struct* task);
void scheduler_tick(unsigned int status, unsigned int cause, context* pt_context);
static int dynamic_prio(task_t *p);
int get_bonus(task_t *p);
static unsigned int basic_timeslice(task_t *p);
void schedule(context* pt_context);
static void copy_context(context* src, context* dest);
static void recalc_task_prio(task_t *p, unsigned int now);
void pc_exit();
int pc_kill(unsigned int pid);
int runprog1(unsigned int argc, void *args);
int runprog2(unsigned int argc, void *args);
extern void switch_ex(struct regs_context* regs);
static void delete_task(struct task_struct *p);
static void add_task_in_processes(struct task_struct *p);
struct task_struct* find_in_processes(unsigned int pid);
static void remove_task_from_processes(struct task_struct *p);
void print_prio_array(struct prio_array *arraylist);
void print_task(struct task_struct* p);
unsigned int fork(char *task_name, int set_prio);
void print_sched_info(void);
void search_prio_bitmap(struct prio_array *arraylist);
int print_task_pid(unsigned int task_pid);
int runprog3(unsigned int argc, void *args);
unsigned int starve(void);
int starve_test(unsigned int argc, void *args);
int runprog4(unsigned int argc, void *args);
#endif

