#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

#define PIPESIZE 512

struct pipe {
  struct spinlock lock; // 自旋锁，用于进程互斥
  char data[PIPESIZE]; // 管道内存储的数据，最大长度为512
  uint nread;     // 读入的数据量（bytes）
  uint nwrite;    // 写入的数据量（bytes）
  int readopen;   // 表示读端文件描述符仍在打开状态
  int writeopen;  // 表示写端文件描述符仍在打开状态
};

int
pipealloc(struct file **f0, struct file **f1)
{
  struct pipe *pi;

  pi = 0;
  *f0 = *f1 = 0;
  if((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
    goto bad;
  if((pi = (struct pipe*)kalloc()) == 0)
    goto bad;
  pi->readopen = 1;
  pi->writeopen = 1;
  pi->nwrite = 0;
  pi->nread = 0;
  initlock(&pi->lock, "pipe");
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = pi;
  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = pi;
  return 0;

 bad:
  if(pi)
    kfree((char*)pi);
  if(*f0)
    fileclose(*f0);
  if(*f1)
    fileclose(*f1);
  return -1;
}

void pipeclose(struct pipe *pi, int writable)
{
  acquire(&pi->lock);
  if(writable){
    pi->writeopen = 0;
    wakeup(&pi->nread);
  } else {
    pi->readopen = 0;
    wakeup(&pi->nwrite);
  }
  if(pi->readopen == 0 && pi->writeopen == 0){
    release(&pi->lock);
    kfree((char*)pi);
  } else
    release(&pi->lock);
}

int pipewrite(struct pipe *pi, uint64 addr, int n) // 管道的write函数
{
  int i = 0; // 用于循环计数，同时记录实际写入的字节数
  struct proc *pr = myproc(); // 获取当前进程的指针

  acquire(&pi->lock); // 获取锁
  while(i < n){ // i < n时都可以循环写入
    if(pi->readopen == 0 || pr->killed){ // 如果读端还开着（需要关闭才能写入）或该进程撤销，则不写入
      release(&pi->lock); // 释放锁
      return -1; // 返回-1
    }
    if(pi->nwrite == pi->nread + PIPESIZE){ //DOC: pipewrite-full
      wakeup(&pi->nread); // 唤醒读端
      sleep(&pi->nwrite, &pi->lock); // 令写端休眠
    } else {
      char ch;
      if(copyin(pr->pagetable, &ch, addr + i, 1) == -1) // 复制到pagetable
        break;
      pi->data[pi->nwrite++ % PIPESIZE] = ch; // 逐字写入
      i++;
    }
  }
  wakeup(&pi->nread); // 写入完毕，唤醒读端
  release(&pi->lock); // 释放锁

  return i; // 返回实际写入的字节数
}

int piperead(struct pipe *pi, uint64 addr, int n) // 管道的read函数
{
  int i; // 用于循环计数，同时记录实际读取的字节数
  struct proc *pr = myproc(); // 获取当前进程的指针
  char ch; // 用于临时存储管道中的单位数据

  acquire(&pi->lock); // 获取自旋锁
  while(pi->nread == pi->nwrite && pi->writeopen){  //DOC: pipe-empty
    if(pr->killed){ // 如果当前进程被撤销，则不再读取
      release(&pi->lock); // 释放锁
      return -1; // 返回-1
    }
    sleep(&pi->nread, &pi->lock); //DOC: piperead-sleep // 否则休眠
  }
  for(i = 0; i < n; i++){  //DOC: piperead-copy
    if(pi->nread == pi->nwrite)
      break; // 如果已读完 退出循环
    ch = pi->data[pi->nread++ % PIPESIZE];
    if(copyout(pr->pagetable, addr + i, &ch, 1) == -1) // 读取pipe里的内容并复制到pagetable
      break;
  }
  wakeup(&pi->nwrite);  //DOC: piperead-wakeup // 唤醒写端
  release(&pi->lock); // 释放锁
  return i; // 返回实际读取的字节数
}
