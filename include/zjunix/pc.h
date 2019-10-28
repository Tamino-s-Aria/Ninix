#ifndef _ZJUNIX_PC_H
#define _ZJUNIX_PC_H

#include <zjunix/list.h>
#include <zjunix/pid.h>
#include <zjunix/vm.h>
#include <zjunix/fat32.h>
#include <zjunix/vfs.h>

#define  KERNEL_STACK_SIZE  4096
#define  TASK_NAME_LEN   32

//����״̬
#define  TASK_UNINIT    0
#define  TASK_READY    1
#define  TASK_RUNNING  2
#define  TASK_WAITING  3
#define  TASK_TERMINAL 4

//���Ȳ���
//ʵʱ������FIFO �� RR
//��ͨ������normal
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

//0-49 ��ʵʱ����
//50-63����ͨ����
#define MAX_PRIO 64
#define MAX_RT_PRIO    49
#define INIT_PRIO  55
#define MAX_SLEEP_AVG  5555
#define MAX_BONUS 10
#define BITMAP_SIZE ((MAX_PRIO+7)/8)
#define MAX_STARVE_COUNT  3


//�Ĵ���
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

//�ۺ������̵�Դ���Linux��task_strct��ΪPCB
struct task_struct {
	unsigned int pid;              //����pid��
	unsigned char name[TASK_NAME_LEN];  //������
	unsigned int parent;           //������pid��
	int ASID;               //���̵�ַ�ռ�id��
	int state;              //����״̬  

	struct regs_context context;    //���̼Ĵ�����Ϣ
	struct mm_struct *mm;           //���̵�ַ�ռ�ṹָ��
	struct file *task_files;               //���̴��ļ�ָ��

	unsigned long static_prio;    //��ͨ���̵ľ�̬���ȼ�
	unsigned long prio;         //��ͨ���̵Ķ�̬���ȼ�
	unsigned long rt_priority;  //ʵʱ�������ȼ�
	unsigned long sleep_avg; //ƽ��˯��ʱ�䣬����������̬���ȼ�
	unsigned long policy; //��������
	unsigned int starve_count;

	struct list_head position_in_processes; //������process�е�λ�� 
	struct list_head position_in_runlist; //�������ڵ��Ǹ����ȼ������ж����е�λ��
	struct prio_array *array;  //ָ��ǰ�������ڵ��������ڻ����ǻ�Ծ
	unsigned int timestamp; //���һ�ε��л�ʱ�䣬��һ�ν��̵��л�ʱ��
	unsigned int time_slice; //����ʣ���ʱ��Ƭ
							 //	unsigned long last_ran;  //��timestamp���񣬿��ò��ð�
};
typedef struct task_struct task_t;

union task_union {
	struct task_struct task;        //���̿��ƿ�
	unsigned char kernel_stack[KERNEL_STACK_SIZE];  //���̵��ں�ջ
};

//���ȼ�����
struct prio_array {
	unsigned int nr_active; //������
	int highest_prio; //��¼��ǰ�����е�������ȼ�����͵����ȼ���ֵ��
	unsigned char bitmap[BITMAP_SIZE];  //���ȼ�λͼ ��Ӧ����ǿ�ʱ��1
	struct list_head queue[MAX_PRIO]; //���ȼ����е�ͷ�ڵ�
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

