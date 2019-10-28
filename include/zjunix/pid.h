#ifndef  _ZJUNIX_PID_H
#define  _ZJUNIX_PID_H

#define  PID_NUM  256  //最大进程数量
#define  PID_BYTES ((PID_NUM + 7) >> 3) //位图占据的字节数
#define  IDLE_PID  0            //idle process
#define  INIT_PID  1            //init process (kernel shell)

typedef unsigned int pid_t;

unsigned char pidmap[PID_BYTES];    //pid 位图
pid_t next_pid;      //pid cache

void init_pid();
pid_t pid_alloc(void); //分配pid, 成功返回获得的pid,否则返回-1
int pid_free(pid_t num);   //释放pid, 成功返回0，否则返回1
int pid_check(pid_t pid); //外部用不到

#endif

