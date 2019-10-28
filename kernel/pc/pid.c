#include <zjunix/pid.h>
#include <driver/vga.h>

//pid位图初始化
//已分配则该pid位置1
void init_pid()
{
    int i;
    for (i=1; i<PID_BYTES; i++)
    {
        pidmap[i] = 0;
    }
    pidmap[0] = 0x01; //idle
    next_pid = INIT_PID;

   // kernel_printf("Debug: init_pid succeed!\n");
}

//检查pid是否可以被分配
//可分配返回1
//不可分配返回0
int pid_check(pid_t pid)
{
    int index, off;

    if (pid >= PID_NUM || pid < IDLE_PID) 
        return 0;
    
    index = pid >> 3;
    off = pid & 0x07;
    if (pidmap[index] & (1 << off)) //已分配
        return 0;
    else // 未分配
        return 1;
}

//分配pid
//分配成功返回pid
//否则返回-1
pid_t pid_alloc(void)
{
    pid_t ret;
    if (pid_check(next_pid))
    {
        pidmap[next_pid>>3] |= 1<<(next_pid&0x07); 
        ret = next_pid;
        if (next_pid < PID_NUM-1)
            next_pid = next_pid + 1;
        else
            next_pid = 1;
        //kernel_printf("Debug: pid_alloc succeed! PID = %d\n", ret);
        return ret;
    }
    else
    {
        int i;
        for (i = 1; i < PID_NUM-1; i++)
        {
            if(pid_check(i))
            {
                pidmap[i>>3] |= 1<<(i&0x07); 
                ret = i;
                next_pid = i + 1;
                //kernel_printf("Debug: pid_alloc succeed! PID = %d\n", ret);
                return ret;
            }
        }    
        if ((i==PID_NUM-1) && pid_check(i))
        {
            pidmap[i>>3] |= 1<<(i&0x07); 
            ret = i;
            next_pid = 1;
            //kernel_printf("Debug: pid_alloc succeed! PID = %d\n", ret);
            return ret;            
        }
        //kernel_printf("Debug: pid_alloc failed!\n");
        return -1;  
    }    
}

//释放pid
//成功则返回0
//否则返回1
int pid_free(pid_t pid) 
{
    int res;
    int index, off;

    if (pid_check(pid)) //未分配该pid
    {
        kernel_printf("pid = %d\n", pid);
        //kernel_printf("Debug: pid_free failed.\n");
        return 1;
    }
    else 
    {
        pidmap[pid>>3] &= (~(1<<pid&0x07));
        //kernel_printf("Debug: pid_free succeed!\n");
        return 0;
    }
}
