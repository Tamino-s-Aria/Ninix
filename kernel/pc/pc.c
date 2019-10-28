#include <zjunix/pc.h>
#include <zjunix/pid.h>
#include <arch.h>
#include <intr.h>
#include <zjunix/syscall.h>
#include <zjunix/utils.h>
#include <zjunix/log.h>
#include <zjunix/slab.h>
#include <driver/vga.h>
#include <driver/ps2.h>
#include <zjunix/vfs.h>
#include "../page.h"

//以下链表均不包含idle
//存储所有进程的链表
struct list_head processes;
//统计所有进程数 
int nr_process = 0;
//进程等待队列
struct list_head waiting_list;
//统计等待进程数
int nr_waiting = 0;
//结束进程链表
struct list_head exited_list;
//结束进程数
int nr_exited = 0;
//处于运行队列中的进程
int nr_running = 0;

//拆解了linux中的runqueue 
//因为只有单核 所以直接作为全局使用
struct task_struct *current_task = 0;  //指向当前正在运行的进程
struct task_struct *idle;  //指向idle进程
struct task_struct *init;  //指向init进程
struct prio_array *active;   //活动队列指针
struct prio_array *expired;  //过期队列指针
struct prio_array arrays[2];  //活动队列和过期队列
unsigned int timestamp_last_tick = 0;  //上一次的时间戳
unsigned int array_exchange_count = 0; //交换次数统计

									   // struct mm_struct *prev_mm;  //存放被替换进程的进程描述符指针

									   //初始化进程管理
									   //创建idle进程
									   //在init_kernel()中调用
void init_pc()
{
	struct task_struct *idle;

	//初始化
	init_pc_list();
	init_pid();

	//静态创建idle进程
	idle = (struct task_struct*)(kernel_sp - KERNEL_STACK_SIZE);
	//初始化idle进程
	init_idle(idle);

	//运行idle
	current_task = idle;
	idle->state = TASK_RUNNING;

	//注册kill系统调用
	//register_syscall(10, pc_kill_syscall);
	//注册scheduler_tick，时钟中断触发
	register_interrupt_handler(7, scheduler_tick);
	asm volatile(
		"li $v0, 1000000\n\t"
		"mtc0 $v0, $11\n\t"
		"mtc0 $zero, $9");
}

//链表初始化
void init_pc_list()
{
	INIT_LIST_HEAD(&processes);
	INIT_LIST_HEAD(&waiting_list);
	INIT_LIST_HEAD(&exited_list);
	init_prio_array(&arrays[0]);
	init_prio_array(&arrays[1]);
	active = &arrays[0];
	expired = &arrays[1];
}

//初始化优先级队列
void init_prio_array(struct prio_array *arraylist)
{
	arraylist->nr_active = 0;
	arraylist->highest_prio = MAX_PRIO;
	int i = 0;
	for (i = 0; i < BITMAP_SIZE; i++)
	{
		arraylist->bitmap[i] = 0;
	}
	for (i = 0; i < MAX_PRIO; i++)
	{
		INIT_LIST_HEAD(&(arraylist->queue[i]));

	}
}

//初始化idle
void init_idle(struct task_struct *idle)
{
	idle->pid = IDLE_PID;
	kernel_strcpy(idle->name, "idle");
	idle->parent = IDLE_PID;
	idle->ASID = IDLE_PID;
	idle->state = TASK_UNINIT;    //

								  //idle->context = NULL;
	idle->mm = 0;
	idle->task_files = 0;

	idle->static_prio = MAX_PRIO; //最低优先级
	idle->prio = MAX_PRIO;
	idle->sleep_avg = 0;
	idle->policy = 0;
	INIT_LIST_HEAD(&(idle->position_in_processes));
	INIT_LIST_HEAD(&(idle->position_in_runlist));
	idle->array = 0;
	idle->timestamp = 0;
	idle->time_slice = 0;

	idle->state = TASK_READY;

}

//获取当前时间
//因为不支持unsigned long long 类型
//而单取 ticks_low 粒度太小 单取ticks_high粒度太大
//所以各取了一部分进行拼接合并 实现了一个时钟
unsigned int sched_clock()
{
	unsigned int timestamp = 0;
	unsigned int ticks_high, ticks_low;
	asm volatile(
		"mfc0 %0, $9, 6\n\t"
		"mfc0 %1, $9, 7\n\t"
		: "=r"(ticks_low), "=r"(ticks_high));

	timestamp |= ticks_high << 12;
	timestamp |= ticks_low >> 20;
	//kernel_printf("clock = %d\n", timestamp);
	return timestamp;
}

//外部创建进程的接口函数 
//接收set_prio作为新进程的静态优先级
//如果set_prio为-1 作为默认情况 新进程将继承父进程的动态优先级
//fork如果成功返回子进程pid 否则分会0
unsigned int fork(char *task_name, int set_prio)
{
	//对传入的参数进行检查
	if (!(set_prio > 0 && set_prio < MAX_PRIO) && set_prio != -1)
	{
		kernel_printf("DO FORK ERROR: The priority you set is out of range.!\n");
		return 0;
	}

	//根据fork时指定的优先级决定内核线程的入口函数 方便不同情况下的测试
	//如果是实时进程中静态优先级较高的一类 默认采用FIFO调度 
	//将入口函数设为runprog2 测试exit
	if (0 < set_prio  && set_prio <= MAX_RT_PRIO / 2)
	{
		kernel_printf("rt task\n");
		return do_fork(task_name, (void*)runprog2, 0, (void *)0, set_prio);
	}
	//如果是实时进程中静态优先级较低的一类 默认采用RR调度
	//将入口函数设为runprog1 死循环 方便调度调试   慢速打印
	else if (set_prio > MAX_RT_PRIO / 2 && set_prio <= MAX_RT_PRIO)
	{
		kernel_printf("rt task\n");
		return do_fork(task_name, (void*)runprog1, 0, (void *)0, set_prio);
	}
	//如果是普通进程 采用普通调度 
	//将入口函数设置为runprog3 快速打印 最后退出 方便调度测试
	else if (set_prio > MAX_RT_PRIO)
	{
		kernel_printf("normal task\n");
		return do_fork(task_name, (void*)runprog3, 0, (void *)0, set_prio);
	}
	//默认情况下入口为2 进程主动退出
	else if (set_prio == -1)
	{
		return do_fork(task_name, (void*)runprog4, 0, (void *)0, set_prio);
	}
	else
	{
		return 0;
	}
}

//创建新进程
//set_prio 用来创建的时候设置静态优先级 若set_prio ==  -1 则继承父进程的静态优先级
//创建成功返回子进程的pid 否则返回0
unsigned int do_fork(char *task_name,
	void(*entry)(unsigned int argc, void *args),
	unsigned int argc, void *args, int set_prio)
{
	//对传入的参数进行检查
	if (!(set_prio >= 0 && set_prio < MAX_PRIO) && set_prio != -1)
	{
		kernel_printf("DO FORK ERROR: The priority you set is out of range.!\n");
		return 0;
	}

	//为新进程分配数据结构 
	//分配pid
	unsigned int new_pid = pid_alloc();

	//没有足够的pid可供分配
	if (new_pid < 0)
	{
		kernel_printf("DO FORK ERROR: pid allocated failed!\n");
		return 0;
	}

	//分配task_union
	union task_union *new_union;
	new_union = (union task_union*) kmalloc(sizeof(union task_union));
	if (new_union == 0)
	{
		kernel_printf("DO FORK ERROR: task union allocated failed!\n");
		pid_free(new_pid);
		return 0;
	}

	//为了避免进程创建中可能出现的中断 直接采取关中断的方式
	disable_interrupts();

	//初始化task_struct	的信息
	new_union->task.pid = new_pid;
	kernel_strcpy(new_union->task.name, task_name);
	new_union->task.ASID = new_union->task.pid;
	new_union->task.parent = current_task->pid;
	new_union->task.state = TASK_UNINIT;

	//将进程加入进程链表
	INIT_LIST_HEAD(&(new_union->task.position_in_processes));
	add_task_in_processes(&(new_union->task));

	//拷贝父进程资源 
	new_union->task.task_files = 0;
	new_union->task.mm = 0;

	//regs
	unsigned int init_gp;
	kernel_memset(&(new_union->task.context), 0, sizeof(struct regs_context));
	new_union->task.context.epc = (unsigned int)entry;      //设置新进程入口地址
	new_union->task.context.sp = (unsigned int)new_union + KERNEL_STACK_SIZE; //设置新进程内核栈
	asm volatile("la %0, _gp\n\t" : "=r"(init_gp));
	new_union->task.context.gp = init_gp;
	new_union->task.context.a0 = argc;
	new_union->task.context.a1 = (unsigned int)args;

	//为子进程设置调度信息
	sched_fork(&new_union->task, set_prio);

	//唤醒新进程 将进程装入运行队列
	wake_up_new_task(&new_union->task);

	//打开中断
	enable_interrupts();

	return new_union->task.pid;
}


//为进程设置调度信息
//把子进程插入到父进程的运行队列，插入时让子进程恰好在父进程前面
//因此迫使子进程优于父进程先运行
void sched_fork(struct task_struct *p, int set_prio)
{
	p->state = TASK_RUNNING;

	//用户未主动设置优先级
	//默认继承父进程的静态优先级
	if (set_prio == -1)
	{
		p->static_prio = current_task->static_prio; //继承父进程的静态优先级
		if (p->static_prio <= MAX_RT_PRIO)
		{
			//实时进程 设置实时优先级
			p->rt_priority = p->static_prio;
			p->prio = p->rt_priority;
			p->sleep_avg = current_task->sleep_avg;
			//根据实时进程的优先级决定调度算法
			//如果是较高优先级的实时进程 0- MAX_RT_PRIO/2 FIFO
			//如果是较低优先级的实时进程 MAX_RT_PRIO/2- MAX_RT_PRIO RR			
			if (p->rt_priority > MAX_RT_PRIO / 2 && p->rt_priority <= MAX_RT_PRIO)
			{
				p->policy = SCHED_RR;
			}
			else
			{
				p->policy = SCHED_FIFO;
			}

		}
		else
		{
			//普通进程 设置动态优先级
			p->prio = p->static_prio;
			p->rt_priority = p->prio;
			p->sleep_avg = current_task->sleep_avg;
			p->policy = SCHED_NORMAL;
		}
	}
	else
	{
		p->static_prio = set_prio;
		if (p->static_prio <= MAX_RT_PRIO)
		{
			//实时进程
			p->rt_priority = p->static_prio;
			p->prio = p->rt_priority;
			p->sleep_avg = current_task->sleep_avg;
			if (p->rt_priority > MAX_RT_PRIO / 2 && p->rt_priority <= MAX_RT_PRIO)
			{
				p->policy = SCHED_RR;
			}
			else
			{
				p->policy = SCHED_FIFO;
			}
		}
		else
		{
			//普通进程
			p->prio = p->static_prio;
			p->rt_priority = p->prio;
			p->sleep_avg = current_task->sleep_avg;
			p->policy = SCHED_NORMAL;
		}
	}

	//初始化
	//此时还未将进程插入运行队列
	INIT_LIST_HEAD(&p->position_in_runlist);
	p->array = 0;

	if (p->pid == 1) //init进程获得基本时间片
	{
		p->time_slice = basic_timeslice(p);
	}
	else //父进程和子进程平分父进程的时间片
	{
		//为了避免父进程通过fork不断获取时间片
		//所以在创建进程时 规定父进程和子进程平分时间片
		p->time_slice = (current_task->time_slice + 1) / 2;
		current_task->time_slice = current_task->time_slice / 2;
	}

	if (current_task->time_slice == 0)
	{
		current_task->time_slice = 1;
	}

	//进程创建的时间戳
	p->timestamp = sched_clock();
}

//唤醒新创建的进程
//将子进程加入运行队列
void wake_up_new_task(struct task_struct * p)
{
	//更新子进程的睡眠时间
	p->sleep_avg = CHILD_PENALTY / 100 * p->sleep_avg;
	if (p->sleep_avg < 0)
	{
		p->sleep_avg = 0;
	}
	else if (p->sleep_avg > MAX_SLEEP_AVG)
	{
		p->sleep_avg = MAX_SLEEP_AVG;
	}

	//计算普通进程的动态优先级
	if (!is_rt_task(p))
	{
		p->prio = dynamic_prio(p);
	}

	//让子进程先于父进程运行
	//即将子进程插到父进程的前面	
	// if(run_child_first)
	// {
	// 	p->prio = current_task->prio;
	// 	list_add_tail(&p->position_in_runlist, &current_task->position_in_runlist);
	// 	p->array = current_task->array;
	// 	p->array->nr_active++;
	// 	nr_running++;
	// }
	// else
	// {
	activate_task(p); //队尾
					  // }

					  //重新调度 抢占
					  // if (is_rt_task(p))
					  // {
					  // 	if (p->rt_priority < current_task->prio)
					  // 	{
					  // 		;
					  // 		//kernel_printf("Debug: wake_up_new_task: need reschedule.\n");
					  // 		// schedule(pt_context);
					  // 	}
					  // }
					  // else
					  // {
					  // 	if (p->prio < current_task->prio)
					  // 	{
					  // 		;
					  // 	//kernel_printf("Debug: wake_up_new_task: need reschedule.\n");
					  // 	// schedule(pt_context);
					  // 	}
					  // }


					  //更新父进程的睡眠时间
	current_task->sleep_avg = PARENT_PENALTY / 100 * current_task->sleep_avg;
	if (current_task->sleep_avg < 0)
	{
		current_task->sleep_avg = 0;
	}
	else if (current_task->sleep_avg > MAX_SLEEP_AVG)
	{
		current_task->sleep_avg = MAX_SLEEP_AVG;
	}

	//kernel_printf("Debug: wake_up_new_task succeed!\n");
}


//更新进程的时间片
//由时钟中断触发
void scheduler_tick(unsigned int status, unsigned int cause, context* pt_context)
{
	//kernel_printf("Debug: scheduler_tick starts!\n");
	struct task_struct *p = current_task;
	//kernel_printf("schedule_tick starts: CURRENT TASK: %d\n", current_task->pid);

	//更新tick时的timestamp
	timestamp_last_tick = sched_clock();


	//如果当前进程为idle 进行调度
	if (p->pid == IDLE_PID)
	{
		//无需更新idle的时间片
		schedule(pt_context);
		asm volatile("mtc0 $zero, $9\n\t");
		//kernel_printf("schedule_tick ends: rescheduled: CURRENT TASK: %d\n", current_task->pid);
		return;
	}
	//如果进程已经过期 进行调度
	if (p->array == expired)
	{
		schedule(pt_context);
		asm volatile("mtc0 $zero, $9\n\t");
		//kernel_printf("schedule_tick ends: rescheduled: CURRENT TASK: %d\n", current_task->pid);
		return;
	}

	//存在优先级更高的进程  抢占
	if (is_rt_task(p))
	{
		if (active->highest_prio < p->rt_priority)
		{
			schedule(pt_context);
			asm volatile("mtc0 $zero, $9\n\t");
			//kernel_printf("schedule_tick ends: rescheduled: CURRENT TASK: %d\n", current_task->pid);
			return;
		}
	}
	else
	{
		if (active->highest_prio < p->prio)
		{
			schedule(pt_context);
			asm volatile("mtc0 $zero, $9\n\t");
			//kernel_printf("schedule_tick ends: rescheduled: CURRENT TASK: %d\n", current_task->pid);
			return;
		}
	}


	//实时进程
	//检查是否超过最大时常
	//如果没有则重新加入
	if (is_rt_task(p))
	{
		//只针对RR调度
		if (p->policy == SCHED_RR)
		{
			if (--p->time_slice == 0)
			{
				//时间片耗尽 重新分配时间片
				p->time_slice = basic_timeslice(p);
				//检查时常
				//如果太常移到过期队列中
				if (timestamp_last_tick - p->time_slice > STARVATION_LIMIT)
				{
					kernel_printf("starve: move to expired\n");
					dequeue_task(p, active);
					p->time_slice = basic_timeslice(p);   //重新分配时间片
					enqueue_task(p, expired);
					schedule(pt_context);
					asm volatile("mtc0 $zero, $9\n\t");
					//kernel_printf("schedule_tick ends: rescheduled: CURRENT TASK: %d\n", current_task->pid);
					return;
				}
				else
				{
					kernel_printf("requeue\n");
					p->time_slice = basic_timeslice(p);   //重新分配时间片
					requeue_task(p, active);  //移到队尾
					schedule(pt_context);
					asm volatile("mtc0 $zero, $9\n\t");
					//kernel_printf("schedule_tick ends: rescheduled: CURRENT TASK: %d\n", current_task->pid);
					return;
				}
			}
		}
	}
	//普通进程
	else
	{
		//时间片耗尽 过期
		//移动到expired队列
		if (--p->time_slice == 0)
		{
			dequeue_task(p, active);
			p->prio = dynamic_prio(p);  //更新动态优先级
			p->time_slice = basic_timeslice(p);   //重新分配时间片
			enqueue_task(p, expired);

			schedule(pt_context);
			asm volatile("mtc0 $zero, $9\n\t");
			//kernel_printf("schedule_tick ends: rescheduled: CURRENT TASK: %d\n", current_task->pid);
			return;
		}
		//未过期，移动到队尾
		else
		{
			// requeue_task(p, active);
			// schedule();
		}
	}
	asm volatile("mtc0 $zero, $9\n\t");
	return;
	//kernel_printf("schedule_tick ends: CURRENT TASK: %d\n", current_task->pid);

}


//更新进程的动态优先级  只针对普通进程
//实时进程的优先级等于其静态优先级
//根据sleep_avg动态调整
//动态优先级=max(MAX_RT_PRIO , min((静态优先级 – bonus + 5) , MAX_PRIO)))
static int dynamic_prio(struct task_struct *p)
{
	int bonus, prio;

	if (is_rt_task(p))
		return p->rt_priority;

	bonus = get_bonus(p);
	prio = p->static_prio - bonus + 2;
	//kernel_printf("Debug: dynamic_prio = %d\t", p->prio);
	//kernel_printf("bonus = %d\t static_prio = %d\n", bonus, p->static_prio);

	//将普通进程的优先级限制在规定范围内
	if (prio >= MAX_PRIO)
		prio = MAX_PRIO - 1;

	if (prio <= MAX_RT_PRIO)
		prio = MAX_RT_PRIO + 1;

	//kernel_printf("new_dynamic_prio = %d!\n", prio);
	return prio;
}

//根据平均睡眠时间计算bonus
int get_bonus(struct task_struct *p)
{
	int bonus;
	if (p->sleep_avg >= 0 && p->sleep_avg <= MAX_SLEEP_AVG / 5)
		bonus = -2;
	else if (p->sleep_avg > MAX_SLEEP_AVG / 5 && p->sleep_avg <= MAX_SLEEP_AVG / 5 * 2)
		bonus = -1;
	else if (p->sleep_avg > MAX_SLEEP_AVG / 5 * 2 && p->sleep_avg <= MAX_SLEEP_AVG / 5 * 3)
		bonus = 0;
	else if (p->sleep_avg > MAX_SLEEP_AVG / 5 * 3 && p->sleep_avg <= MAX_SLEEP_AVG / 5 * 4)
		bonus = 1;
	else if (p->sleep_avg > MAX_SLEEP_AVG / 5 * 4 && p->sleep_avg <= MAX_SLEEP_AVG)
		bonus = 2;

	return bonus;
}

//更新进程的时间片
//由静态优先级决定的基本时间片
//实时进程 基本时间片=max((最高优先级-静态优先级)*20, MIN_TIMESLICE)
//普通进程 基本时间片=max((最高优先级-静态优先级)*5, MIN_TIMESLICE)
static unsigned int basic_timeslice(struct task_struct *p)
{
	unsigned int ret_timeslice;
	if (is_rt_task(p))
	{
		ret_timeslice = (MAX_PRIO - p->rt_priority) * 20;
		if (ret_timeslice < MIN_RT_TIMESLICE)
			ret_timeslice = MIN_RT_TIMESLICE;
		else if (ret_timeslice > MAX_RT_TIMESLICE)
			ret_timeslice = MAX_RT_TIMESLICE;

		if (p->starve_count >= MAX_STARVE_COUNT)
			pc_kill(p->pid);
	}
	else
	{
		ret_timeslice = (MAX_PRIO - p->prio) * 5;
		if (ret_timeslice < MIN_TIMESLICE)
			ret_timeslice = MIN_TIMESLICE;
		else if (ret_timeslice > MAX_TIMESLICE)
			ret_timeslice = MAX_TIMESLICE;
	}
	return ret_timeslice;
}

struct task_struct * find_next_task(void)
{
	struct task_struct *prev;
	struct task_struct *next;
	struct prio_array *temp_array;
	struct list_head *pos;
	unsigned int now;
	unsigned int run_time;

	prev = current_task;

	//运行队列中的进程数为0
	//运行idle进程
	if (nr_running == 0)
	{
		next = idle;
	}

	//检查active是否为空
	//如果空 交换active和expired
	if (active->nr_active == 0)
	{
		//kernel_printf("IN SCHEDULE: the ACTIVE is empty. need to exchange.\n ");
		temp_array = active;
		active = expired;
		expired = temp_array;
		array_exchange_count++;
	}

	//寻找下一个进程 O(1)
	//kernel_printf("DEBUG: HIGHEST_PRIO = %d\n", active->highest_prio);
	if (active->highest_prio < MAX_PRIO)
	{
		pos = active->queue + active->highest_prio;
		next = list_entry(pos->next, struct task_struct, position_in_runlist);
	}
	else if (active->highest_prio == MAX_PRIO)
	{
		next = idle;
	}

	//更新next的睡眠时间
	recalc_task_prio(next, now);

	//激活地址空间
	activate_mm(next);

	// 下一个进程开始运行的时间
	next->timestamp = now;

	//print_sched_info();
	return next;
}


//进程调度函数
void schedule(context* pt_context)
{
	//kernel_printf("Debug: schedule starts!\n");
	//print_sched_info();
	struct task_struct *prev;
	struct task_struct *next;
	struct prio_array *temp_array;
	struct list_head *pos;
	unsigned int now;
	unsigned int run_time;

	prev = current_task;

	if (prev == idle)
	{

	}

	//计算该进程的运行时间
	//用来计算bonus
	now = sched_clock();
	if (now - prev->timestamp < MAX_SLEEP_AVG)
	{
		run_time = now - prev->timestamp;
	}
	else
	{
		run_time = MAX_SLEEP_AVG;
	}

	//运行队列中的进程数为0
	//运行idle进程
	if (nr_running == 0)
	{
		next = idle;
	}


	//检查active是否为空
	//如果空 交换active和expired
	if (active->nr_active == 0)
	{
		//kernel_printf("IN SCHEDULE: the ACTIVE is empty. need to exchange.\n ");
		temp_array = active;
		active = expired;
		expired = temp_array;
	}

	//寻找下一个进程 O(1)
	// if (active->highest_prio == MAX_PRIO)
	// {
	// 	print_prio_array(active);
	// 	print_prio_array(expired);
	// 	kernel_printf("DEBUG: HIGHEST_PRIO is MAX_PRIO");
	// }

	if (active->highest_prio < MAX_PRIO && active->highest_prio > 0)
	{
		pos = active->queue + active->highest_prio;
		next = list_entry(pos->next, struct task_struct, position_in_runlist);
		//kernel_printf("Debug: schedule: next process is %d\n", next->pid);
	}
	else
	{
		next = idle;
	}
	//kernel_printf("Debug: schedule: next process is %d\n", next->pid);

	//更新next的睡眠时间
	recalc_task_prio(next, now);

	//平均睡眠时间中减去其上次的运行时间
	if (prev->sleep_avg < run_time)
		prev->sleep_avg = 0;
	else
		prev->sleep_avg -= run_time;
	//记录开始睡眠的时间 更新时间戳
	prev->timestamp = now;


	//需要进行进程切换
	if (current_task != next)
	{
		//激活地址空间
		activate_mm(next);

		// 保存上下文	
		copy_context(pt_context, &(current_task->context));

		// 下一个进程开始运行的时间
		next->timestamp = now;
		// 进程切换
		current_task = next;

		// 载入上下文
		copy_context(&(current_task->context), pt_context);
	}


	//kernel_printf("Debug: schedule ends!\n");
	//print_sched_info();

}

//上下文切换 src->dest
static void copy_context(context* src, context* dest)
{
	dest->epc = src->epc;
	dest->at = src->at;
	dest->v0 = src->v0;
	dest->v1 = src->v1;
	dest->a0 = src->a0;
	dest->a1 = src->a1;
	dest->a2 = src->a2;
	dest->a3 = src->a3;
	dest->t0 = src->t0;
	dest->t1 = src->t1;
	dest->t2 = src->t2;
	dest->t3 = src->t3;
	dest->t4 = src->t4;
	dest->t5 = src->t5;
	dest->t6 = src->t6;
	dest->t7 = src->t7;
	dest->s0 = src->s0;
	dest->s1 = src->s1;
	dest->s2 = src->s2;
	dest->s3 = src->s3;
	dest->s4 = src->s4;
	dest->s5 = src->s5;
	dest->s6 = src->s6;
	dest->s7 = src->s7;
	dest->t8 = src->t8;
	dest->t9 = src->t9;
	dest->hi = src->hi;
	dest->lo = src->lo;
	dest->gp = src->gp;
	dest->sp = src->sp;
	dest->fp = src->fp;
	dest->ra = src->ra;
}

//更新动态优先级和平均睡眠时间
static void recalc_task_prio(struct task_struct *p, unsigned int now)
{
	unsigned int sleep_time = now - p->timestamp;
	if (sleep_time > MAX_SLEEP_AVG)
		sleep_time = MAX_SLEEP_AVG;

	//kernel_printf("pre sleepavg = %d\n", p->sleep_avg);
	if (sleep_time > 0)
	{
		p->sleep_avg = p->sleep_avg + sleep_time;
		if (p->sleep_avg > MAX_SLEEP_AVG)
			p->sleep_avg = MAX_SLEEP_AVG;
	}

	if (!is_rt_task(p))
		p->prio = dynamic_prio(p);

}


//进程主动退出
//在进程代码中调用
void pc_exit()
{
	struct task_struct* temp;
	struct task_struct* next;

	//kernel_printf("exit starts\n");

	if (current_task->pid == IDLE_PID)
	{
		kernel_printf("!!!IDLE PROCESS IS EXITTING!\n");
	}

	if (current_task->pid == INIT_PID)
	{
		kernel_printf("WARNING: init process(kernel shell) is exiting\n");
	}

	temp = current_task;
	//清理task_struct中的一些信息
	// if (current_task->task_files != 0) {
	//     task_files_delete(current_task);
	// }    
	// if (current_task->mm != 0) {
	//     mm_delete(current_task->mm);
	// }

	asm volatile (      //进入异常模式 => 中断关闭
		"mfc0  $t0, $12\n\t"
		"ori   $t0, $t0, 0x02\n\t"
		"mtc0  $t0, $12\n\t"
		"nop\n\t"
		"nop\n\t"
		);
	current_task->state = TASK_TERMINAL;

	//wakeup_parent();

	delete_task(temp);

	//调用调度算法选择下一个要运行的进程
	next = find_next_task();

	// if (next->mm != 0) {
	//     //激活地址空间
	//     activate_mm(next);
	// }

	pid_free(temp->pid);
	current_task = next;

	//print_sched_info();

	//调用汇编代码,加载新的进程的上下文信息
	kernel_printf("task exits.");
	switch_ex(&(current_task->context));
}


// kill进程 
// 返回0表示执行成功，否则表示执行失败
int pc_kill(unsigned int pid) {
	int res;
	struct task_struct* p;

	if (pid >= PID_NUM)
	{
		return 1;
	}
	//idle进程不能被杀死
	if (pid == IDLE_PID) {
		kernel_printf("You can't kill IDLE process!\n");
		return 1;
	}

	//init进程不能被杀死 
	if (pid == INIT_PID) {
		kernel_printf("You can't kill INIT process!\n");
		return 1;
	}

	//进程不能杀死自身
	if (pid == current_task->pid) {
//		kernel_printf("You can't kill the current task!\n");
		return 1;
	}


	if (pid_check(pid) != 0)
	{
		return 1;
	}

	disable_interrupts();

	// 通过pid获取要被杀死的的进程的task_struct结构
	p = find_in_processes(pid);
	if (p == 0)
		return 0;
	p->state = TASK_TERMINAL;
	delete_task(p);

	// if (p->task_files != 0) {
	//     task_files_delete(p);
	// }    

	// if (p->mm != 0) {
	//     mm_delete(p->mm);
	// }

	//释放pid
	pid_free(pid);
	enable_interrupts();
	return 0;
}


//判断是不是实时进程
//如果是返回1
int is_rt_task(struct task_struct* p)
{
	if (p->static_prio > MAX_RT_PRIO)
	{
		return 0;
	}
	else
	{
		return 1;
	}
}

//根据进程号在进程链表中查找进程
//如果找到，则返回进程的task_struct结构
//否则返回0
struct task_struct* find_in_processes(unsigned int pid) {
	struct task_struct *next;
	struct list_head *pos;

	if (pid == IDLE_PID)
		return idle;
	list_for_each(pos, &processes)
	{
		next = container_of(pos, struct task_struct, position_in_processes);
		if (next->pid == pid)
			return next;
	}
	kernel_printf("No such process.\n");
	return 0;
}


//将进程加到对应优先级的活跃队列中
void activate_task(struct task_struct *p)
{
	enqueue_task(p, active);
	nr_running++;
	//kernel_printf("Debug: activate_task succeed!\n");
}

//移除进程
void delete_task(struct task_struct *p)
{

	//if (p->state == TASK_READY)
	dequeue_task(p, p->array);
	nr_running--;
	remove_task_from_processes(p);
}


//将进程插入优先级队列尾部
static void enqueue_task(struct task_struct *p, struct prio_array *array)
{
	//debug
	//print_task(p);
	if (is_rt_task(p))
	{
		list_add_tail(&(p->position_in_runlist), &(array->queue[p->rt_priority]));
	}
	else
	{
		list_add_tail(&(p->position_in_runlist), &(array->queue[p->prio]));
	}

	// if (list_empty(&(array->queue[p->prio])))
	// {
	if (is_rt_task(p))
	{
		set_bit_map(p->rt_priority, array);
		update_highest_pro(array, p->rt_priority, 1);
	}
	else
	{
		set_bit_map(p->prio, array);
		update_highest_pro(array, p->prio, 1);
	}
	// }
	array->nr_active++;
	p->array = array;
	//debug
	//kernel_printf("Debug: enqueue_task succeed!\n");
}

//将进程换到当前优先级队列的尾部
static void requeue_task(struct task_struct *p, struct prio_array *array)
{
	if (is_rt_task(p))
	{
		list_move_tail(&p->position_in_runlist, &(array->queue[p->rt_priority]));
	}
	else
	{
		list_move_tail(&p->position_in_runlist, &(array->queue[p->prio]));
	}
	//kernel_printf("Debug: requeue_task succeed!\n");
}


//将进程从当前优先级队列中删除
static void dequeue_task(struct task_struct *p, struct prio_array *array)
{
	array->nr_active--;
	list_del_init(&p->position_in_runlist);
	//list_del(&p->position_in_runlist);
	p->array = 0;

	if (list_empty(&(array->queue[p->prio])))
	{
		//kernel_printf("list is empty \n");
		if (is_rt_task(p))
		{
			clear_bit_map(p->rt_priority, array);
			update_highest_pro(array, p->rt_priority, 0);
		}
		else
		{
			clear_bit_map(p->prio, array);
			update_highest_pro(array, p->prio, 0);
		}
	}
	//p->array = 0;
}

//将进程插入新的优先级队列的头部
static void enqueue_task_head(struct task_struct *p, struct prio_array *array)
{
	if (is_rt_task(p))
	{
		list_add(&p->position_in_runlist, &(array->queue[p->rt_priority]));
	}
	else
	{
		list_add(&p->position_in_runlist, &(array->queue[p->prio]));
	}

	//if (list_empty(&(array->queue[p->prio])))
	//{
	if (is_rt_task(p))
	{
		set_bit_map(p->rt_priority, array);
		update_highest_pro(array, p->rt_priority, 1);
	}
	else
	{
		set_bit_map(p->prio, array);
		update_highest_pro(array, p->prio, 1);
	}
	// }
	array->nr_active++;
	p->array = array;
	//kernel_printf("Debug: enqueue_task_head succeed!\n");
}

//将位图中对应优先级置位
void set_bit_map(int priority, struct prio_array* p_array)
{
	int index;
	int off;
	index = priority / 8;
	off = priority & 0x07;
	// kernel_printf("index = %d off = %d, priority = %d, ", index, off, priority);
	// kernel_printf("prev = %d, ", (unsigned int)p_array->bitmap[index]);	
	p_array->bitmap[index] |= 1 << off;
	// kernel_printf("aft = %d\n", (unsigned int)p_array->bitmap[index]);
	// kernel_printf("Debug: set_bit_map succeed!\n");
}


//记录队列中的最高优先级 来保证调度时候能马上找到最高优先级所在的队列
//当队列中有进程加入或者退出时调用
//priority为进程的优先级 is_enqueue标记进程是加入还是退出
void update_highest_pro(struct prio_array * arraylist, int priority, int is_enqueue)
{
	int index;
	int off;

	if (priority >= MAX_PRIO || priority < 0)
	{
		kernel_printf("ERROR IN UPDATE HIGHEST PRIOI: %d\n", priority);
		return;
	}

	//不受影响 直接返回
	if (arraylist->highest_prio < priority)
		return;

	//加入进程
	if (is_enqueue)
	{
		//更新最高优先级
		if (arraylist->highest_prio > priority)
		{
			arraylist->highest_prio = priority;
		}
		return;
	}
	//移除进程
	else
	{
		//因为当前的最高优先级被移除  所以需要在列表中重新寻找最高优先级
		if (arraylist->highest_prio == priority)
		{
			for (index = priority / 8; index < BITMAP_SIZE; index++)
			{
				if (arraylist->bitmap[index] != 0)
				{
					break;
				}
			}
			if (index == BITMAP_SIZE)
			{
				//kernel_printf("max prio\n");
				arraylist->highest_prio = MAX_PRIO;
			}
			else
			{
				off = 0;
				while (off < 8)
				{
					if (((int)(arraylist->bitmap[index]) & (1 << off)) != 0)
					{
						arraylist->highest_prio = index * 8 + off;
						break;
					}
					off++;
				}
			}
		}
		else
		{
			//kernel_printf("ERROR IN UPDATE HIGHEST PRIOI: %d\n", priority);
			return;
		}
	}
	//kernel_printf("Debug: update_highest_pro succeed! %d\n", arraylist->highest_prio);
}


//将位图中对应优先级的位清零
void clear_bit_map(int priority, struct prio_array* p_array)
{
	int index;
	int off;
	index = priority / 8;
	off = priority & 0x07;
	p_array->bitmap[index] &= ~(1 << off);
	//kernel_printf("Debug: clear_bit_map succeed!\n");
}

//将进程放入进程链表
static void add_task_in_processes(struct task_struct *p)
{
	list_add_tail(&(p->position_in_processes), &processes);
	nr_process++;
	//	kernel_printf("DEBUG: add_task_in_processes succeed.");
}

//将进程从进程链表中移除
static void remove_task_from_processes(struct task_struct *p)
{
	list_del_init(&(p->position_in_processes));
	nr_process--;
	//kernel_printf("DEBUG: remove_task_from_processes succeed.\n");
}


//激活task所指向进程的地址空间
void activate_mm(struct task_struct* task)
{
	//set_tlb_asid(task->ASID);
}

void print_prio_array(struct prio_array *arraylist)
{
	kernel_printf("nr_active = %d\t highest_prio = %d\n", arraylist->nr_active, arraylist->highest_prio);
	search_prio_bitmap(arraylist);
}

void print_task(struct task_struct* p)
{
	if (is_rt_task(p))
	{
		kernel_printf("rt_task\t name:%s =======\n", p->name);
		kernel_printf("pid:%d \t  parent:%d\t state: %d\n", p->pid, p->parent, p->state);
		kernel_printf("static_prio:%d\t rt_priority:%d\n", p->static_prio, p->rt_priority);
		kernel_printf("sleep_avg:%d\t  policy:%d\t timestamp:%d\t time_slice:%d\n", p->sleep_avg, p->policy, p->timestamp, p->time_slice);
	}
	else
	{
		kernel_printf("======normal_task\t name:%s ======\n", p->name);
		kernel_printf("pid:%d \t  parent:%d\t state: %d\n", p->pid, p->parent, p->state);
		kernel_printf("static_prio:%d\t  prio:%d\n", p->static_prio, p->prio);
		kernel_printf("sleep_avg:%d\t  policy:%d\t timestamp:%d\t time_slice:%d\n", p->sleep_avg, p->policy, p->timestamp, p->time_slice);
	}
}

void print_sched_info()
{
	kernel_printf("=====schedule information===== \n");
	kernel_printf("nr_processes = %d\t nr_running = %d\t nr_waiting = %d\t nr_exited = %d\n", nr_process, nr_running, nr_waiting, nr_exited);
	kernel_printf("arraylist_exchange_count == %d\n", array_exchange_count);
	kernel_printf("=====active_info=====\n");
	print_prio_array(active);
	kernel_printf("=====expired_info======\n");
	print_prio_array(expired);
}

void search_prio_bitmap(struct prio_array *arraylist)
{
	int index;
	int off;
	int priority;
	struct task_struct *p;
	struct list_head *pos;
	int count = 0;

	for (index = 0; index < BITMAP_SIZE; index++)
	{
		if (arraylist->bitmap[index] != 0)
		{
			for (off = 0; off < 8; off++)
			{
				// kernel_printf("off = %d\t", off);
				// kernel_printf("bit = %d\t off = %d\t res = %d\n", arraylist->bitmap[index], 1 << off, ((arraylist->bitmap[index]) & (1 << off)));
				if (((int)(arraylist->bitmap[index]) & (1 << off)) != 0)
				{
					priority = index * 8 + off;
					list_for_each(pos, &(arraylist->queue[priority]))
					{

						p = container_of(pos, struct task_struct, position_in_runlist);
						if (p->pid >= PID_NUM)
						{
							return;
						}
						count++;
						if (count > arraylist->nr_active)
							break;
						print_task(p);

					}
				}
			}
		}
	}
}

int print_task_pid(unsigned int task_pid)
{
	struct task_struct* temp = find_in_processes(task_pid);
	if (temp->pid == IDLE_PID)
		print_task(idle);

	if (temp == 0)
		return -1;

	print_task(temp);

	return 0;
}


//内核线程入口函数
//用于测试调度 和 Kill
//死循环
//慢速打印
int runprog1(unsigned int argc, void *args) {
	kernel_printf("running program1\t");
	kernel_printf("current_task: %d\n", current_task->pid);

	int i;
	while (1)
	{
		for (i = 0; i < 1000000000; i++)
		{
			;
		}
		kernel_printf("running program1\t current_task: %d\n", current_task->pid);
		if (current_task->pid == 0)
			return 0;

		print_sched_info();

	}
}

unsigned int starve(void)
{
	unsigned int tester_pid;

	tester_pid = do_fork("starve tester", (void*)starve_test, 0, 0, 49); //RR调度

	if (tester_pid == 0)
	{
		kernel_printf("fork error.\n");
		return 0;
	}

	int i;
	for (i = 0; i < 20; i++)
	{
		kernel_printf("I am starving!\n");
	}

	kernel_printf("tester_pid == %d\n", tester_pid);
	for (i = 0; i < 10000000; i++)
	{
		;
	}
	pc_kill(tester_pid);

	print_sched_info();
}

int starve_test(unsigned int argc, void *args)
{
	kernel_printf("starve tester\t");
	kernel_printf("current_task: %d\n", current_task->pid);
	kernel_printf("real time task using RR schedule.\n");

	int i;
	while (1)
	{
		for (i = 0; i < 10000000; i++)
		{
			;
		}
		kernel_printf("starve tester\t current_task: %d\n", current_task->pid);
		if (current_task->pid == 0)
			return 0;
	}
}

//用于测试exit
int runprog2(unsigned int argc, void *args)
{

	kernel_printf("running program2\t");
	kernel_printf("current_task: %d\n", current_task->pid);
	int i;
	int j;
	for (j = 0; j < 5; j++)
	{
		for (i = 0; i < 100000; i++)
		{
			;
		}
		kernel_printf("running program2\t current_task: %d\n", current_task->pid);
	}

	//进程将要结束的提示信息
	kernel_printf("current_task: %d  End.......\n", current_task->pid);

	print_sched_info();

	//进程退出
	pc_exit(0);
	return 0;
}

//测试 快速打印
int runprog3(unsigned int argc, void *args) {
	kernel_printf("running program3\t");
	kernel_printf("current_task: %d\n", current_task->pid);
	int i;
	int j;
	for (j = 0; j < 5; j++)
	{
		for (i = 0; i < 1000; i++)
		{
			;
		}
		kernel_printf("running program3\t current_task: %d\t i == %d\t j == %d\n", current_task->pid, i, j);
		if (current_task->pid == 0)
			return 0;
	}

	print_sched_info();

	pc_exit(0);
	return 0;
}

//死循环 
//测试kill
int runprog4(unsigned int argc, void *args) {
	kernel_printf("running program4\t");
	kernel_printf("current_task: %d\n", current_task->pid);

	while (1)
		;

	return 0;
}
