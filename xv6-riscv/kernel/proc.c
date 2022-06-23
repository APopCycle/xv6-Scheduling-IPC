#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void userinit(void) {
  struct proc *p; // 指向第一个用户进程

  p = allocproc(); // 分配一个进程
  initproc = p; // 引用init进程的全局变量

  uvminit(p->pagetable, initcode, sizeof(initcode)); // 分配一个页表并将init进程的指示和数据传进去
  p->sz = PGSIZE;

  // 准备内核向用户的第一个返回值，构造trapframe
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name)); // 设置进程名称为“initcode”
  p->cwd = namei("/"); // 设置工作目录为“/”

  p->state = RUNNABLE; // 设置进程状态为可执行runnable

  release(&p->lock); // 释放锁
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int fork(void) {
  int i, pid; // i用于循环计数，pid用于记录进程pid
  struct proc *np; // 指向子进程的指针
  struct proc *p = myproc(); // 指向当前进程的指针

  if((np = allocproc()) == 0){ // 为进程进行内存分配
    return -1; // 如果进程的内存分配失败，则创建进程失败，令fork()返回-1
  }

  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){ // 将父进程的内存内容复制给子进程
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  *(np->trapframe) = *(p->trapframe);   // copy saved user registers.

  np->trapframe->a0 = 0; // 令子进程返回0

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));
  pid = np->pid; // 将pid赋值为子进程的pid
  release(&np->lock);

  acquire(&wait_lock); // 获取wait_lock，为了访问parent
  np->parent = p; // 连接父子进程关系
  release(&wait_lock);

  acquire(&np->lock); // 获取lock，为了修改进程状态
  np->state = RUNNABLE; // 将进程状态改为runnable可执行
  release(&np->lock);

  return pid; // 返回子进程pid
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void exit(int status) {
  struct proc *p = myproc(); // 指向当前（调用了exit的）进程

  if(p == initproc) // 不允许init进程调用exit
    panic("init exiting");

  for(int fd = 0; fd < NOFILE; fd++){ // 关闭所有打开的文件
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op(); // 文件系统调用前执行
  iput(p->cwd);
  end_op(); // 文件系统调用结束
  p->cwd = 0; // 工作目录改为空值

  acquire(&wait_lock); // 获取wait锁以访问parent

  reparent(p); // 如果该进程由子进程，将其转交给init进程

  wakeup(p->parent); // 如果父进程在wait状态中休眠，唤醒父进程
  
  acquire(&p->lock); // 获取锁以修改进程状态

  p->xstate = status;
  p->state = ZOMBIE; // 状态改为zombie

  release(&wait_lock); // 释放锁

  sched(); // 切换调度至其他进程
  panic("zombie exit"); // 正常不会执行至此，否则panic
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(uint64 addr)
{
  struct proc *np; // 指向子进程
  int havekids, pid; // 分别记录该进程是否有孩子（dummy变量）和子进程的pid
  struct proc *p = myproc(); // 指向当前进程

  acquire(&wait_lock); // 获取wait lock以访问遍历进程的父进程

  for(;;){ // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){ // 遍历所有进程以寻找子进程
      if(np->parent == p){ // 发现子进程
        acquire(&np->lock); // 获取锁
        havekids = 1; // 发现子进程，记1
        if(np->state == ZOMBIE){ // 子进程处于zombie状态，需要对子进程进行处理
          pid = np->pid; // 记录子进程pid
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np); // 释放子进程指针
          release(&np->lock);
          release(&wait_lock);
          return pid; // 返回pid
        }
        release(&np->lock); // 如果子进程不在zombie状态，将锁释放
      }
    }

    if(!havekids || p->killed){ // 该进程没有孩子
      release(&wait_lock);
      return -1; // 返回-1
    }
    
    sleep(p, &wait_lock);  //DOC: wait-sleep // 有孩子，但要等待孩子退出，该进程休眠
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void scheduler(void) { // 调度器
  struct proc *p; // 指向当前进程
  struct cpu *c = mycpu(); // 指向cpu
  
  c->proc = 0; // 清理cpu的进程指针
  for(;;){ // 无限循环
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on(); // 开启中断，避免死锁

    for(p = proc; p < &proc[NPROC]; p++) { // 遍历每一个进程
      acquire(&p->lock); // 获取p->lock，从而获得修改进程状态的权限
      if(p->state == RUNNABLE) { // 只处理就绪进程
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING; // 将运行状态改为running
        c->proc = p; // 设置CPU的当前进程指针指向该进程
        swtch(&c->context, &p->context); // 调用swtch，完成上下文切换

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0; // 开始下一次调度，重新选择进程，清理cpu的进程指针
      }
      release(&p->lock); // 释放p->lock
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void) {
  int intena;
  struct proc *p = myproc(); // 指向当前进程

  if(!holding(&p->lock)) // 需要先获取p->lock
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING) // 进程不能处于运行状态，而是应当处于RUNNABLE、SLEEPING、ZOMBIE的状态之一
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context); // 调用swtch完成场景切换
  mycpu()->intena = intena; // intena为切入进程的堆栈变量
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  struct proc *p = myproc(); // 获取当前进程
  acquire(&p->lock); // 获取p->lock
  p->state = RUNNABLE; // 将状态修改为runnable
  sched(); // 调用sched切换到其他进程
  release(&p->lock); // 释放p->lock
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk) { // 休眠阻塞函数，需传入阻塞队列和自旋锁
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1 // 如果lk不是ptable.lock，对ptable.lock加锁
  release(lk); // 获取ptable.lock后，可以暂时释放lk

  p->chan = chan; // 加入休眠阻塞队列
  p->state = SLEEPING; // 修改进程状态为sleeping

  sched(); // 调用sched切换到其他进程

  // Tidy up.
  p->chan = 0; // 被唤醒后，从这一行开始执行

  // Reacquire original lock.
  release(&p->lock); // 释放ptable.lock
  acquire(lk); // 重新获取lk
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) { // 遍历所有进程
    if(p != myproc()){
      acquire(&p->lock); // 如果是其他进程，需要先获取p->lock
      if(p->state == SLEEPING && p->chan == chan) { // 如果该进程在休眠且在阻塞队列中，唤醒它
        p->state = RUNNABLE; // 唤醒它，将状态改回runnable
      }
      release(&p->lock); // 释放p->lock
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kill(int pid) {
  struct proc *p; // 指向要被撤销的进程

  for(p = proc; p < &proc[NPROC]; p++){ // 遍历所有进程
    acquire(&p->lock); // 获取锁
    if(p->pid == pid){ // 找到了该进程
      p->killed = 1; // 将进程的killed标识符改为1
      if(p->state == SLEEPING){ // 如果该进程在休眠，则唤醒它
        p->state = RUNNABLE; // 将进程改为runnable状态
      }
      release(&p->lock); // 释放锁
      return 0;
    }
    release(&p->lock); // 这一轮没找到，释放锁
  }
  return -1; // 没找到拥有该pid的进程，返回-1
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
