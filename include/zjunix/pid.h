#ifndef  _ZJUNIX_PID_H
#define  _ZJUNIX_PID_H

#define  PID_NUM  256  //����������
#define  PID_BYTES ((PID_NUM + 7) >> 3) //λͼռ�ݵ��ֽ���
#define  IDLE_PID  0            //idle process
#define  INIT_PID  1            //init process (kernel shell)

typedef unsigned int pid_t;

unsigned char pidmap[PID_BYTES];    //pid λͼ
pid_t next_pid;      //pid cache

void init_pid();
pid_t pid_alloc(void); //����pid, �ɹ����ػ�õ�pid,���򷵻�-1
int pid_free(pid_t num);   //�ͷ�pid, �ɹ�����0�����򷵻�1
int pid_check(pid_t pid); //�ⲿ�ò���

#endif

