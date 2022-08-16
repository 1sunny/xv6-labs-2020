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
static void wakeup1(struct proc *chan);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// initialize the proc table at boot time.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");

      // Allocate a page for the process's kernel stack.
      // Map it high in memory, followed by an invalid
      // guard page.
      char *pa = kalloc();
      if(pa == 0)
        panic("kalloc");
      uint64 va = KSTACK((int) (p - proc));
      kvmmap(va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
      p->kstack = va;
  }
  kvminithart();
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
// cpuid 和 mycpu 的返回值很脆弱:如果定时器中断并导致线程让步(yield),
// 然后移动到另一个CPU;以前返回的值将不再正确.为了避免这个问题,xv6要求调用
// 者禁用中断,并且只有在使用完返回的 struct cpu 后才重新启用
// 函数 myproc (kernel/proc.c:68)返回当前CPU上运行进程 struct proc 的指
// 针.myproc 禁用中断,调用 mycpu ,从 struct cpu 中取出当前进程指针(c->proc)
// 然后启用中断.即使启用中断, myproc 的返回值也可以安全使用:如果
// 计时器中断将调用进程移动到另一个CPU,其 struct proc 指针不会改变
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

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
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
  // 如果我们需要释放进程内核栈,那么也应该在这里释放
  // 但是因为内核栈的guard page,我们没有必要再释放一次内核栈 ????????
  // 不管怎样,当进程还在exit函数中运行时,任何这些资源在exit函数中释放都会很难受,
  // 所以这些资源都是由父进程释放的
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
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
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
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  np->parent = p;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  np->state = RUNNABLE;

  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold p->lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    // this code uses pp->parent without holding pp->lock.
    // acquiring the lock first could cause a deadlock
    // if pp or a child of pp were also in exit()
    // and about to try to lock p.
    if(pp->parent == p){
      // pp->parent can't change between the check and the acquire()
      // because only the parent changes it, and we're the parent.
      acquire(&pp->lock);
      pp->parent = initproc;
      // we should wake up init here, but that would require
      // initproc->lock, which would be a deadlock, since we hold
      // the lock on one of init's children (pp). this is why
      // exit() always wakes init (before acquiring any locks).
      release(&pp->lock);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
// 在XV6中,一个进程如果退出的话,我们需要释放用户内存,释放page table,
// 释放trapframe对象,将进程在进程表单中标为REUSABLE,这些都是典型的清理步骤
// 当进程退出或者被杀掉时,有许多东西都需要被释放
// 这里会产生的两大问题:
// 1.首先我们不能直接单方面的摧毁另一个线程,因为:
//   另一个线程可能正在另一个CPU核上运行,并使用着自己的栈:
//   也可能另一个线程正在内核中持有了锁;也可能另一个线程正在更新一个复杂的内核数据,
//   如果我们直接就把线程杀掉了,我们可能在线程完成更新复杂的内核数据过程中就把线程杀掉了
// 2.即使一个线程调用了exit系统调用,并且是自己决定要退出.
//   它仍然持有了运行代码所需要的一些资源,例如它的栈,以及它在进程表单中的位置
//   当它还在执行代码,它就不能释放正在使用的资源
//   所以我们需要一种方法让线程能释放最后几个对于运行代码来说关键的资源
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  // we might re-parent a child to init. we can't be precise about
  // waking up init, since we can't acquire its lock once we've
  // acquired any other proc lock. so wake up init whether that's
  // necessary or not. init may miss this wakeup, but that seems
  // harmless.
  acquire(&initproc->lock);
  wakeup1(initproc);
  release(&initproc->lock);

  // grab a copy of p->parent, to ensure that we unlock the same
  // parent we locked. in case our parent gives us away to init while
  // we're waiting for the parent lock. we may then race with an
  // exiting parent, but the result will be a harmless spurious wakeup
  // to a dead or wrong process; proc structs are never re-allocated
  // as anything else.
  acquire(&p->lock);
  struct proc *original_parent = p->parent;
  release(&p->lock);
  
  // we need the parent's lock in order to wake it up from wait().
  // the parent-then-child rule says we have to lock it first.
  acquire(&original_parent->lock);

  acquire(&p->lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup1(original_parent);

  p->xstate = status;
  p->state = ZOMBIE;
  // 学生提问:为什么我们在唤醒父进程之后才将进程的状态设置为ZOMBIE?
  //         难道我们不应该在之前就设置吗?
  //Robert教授:
  // 正在退出的进程会先获取自己进程的锁,同时,
  // 因为父进程的wait系统调用中也需要获取子进程的锁,
  // 所以父进程并不能查看正在执行exit函数的进程的状态
  // 这意味着,正在退出的进程获取自己的锁到它调用sched进入到调度器线程之间
  // (注,因为调度器线程会释放进程的锁),父进程并不能看到这之间代码引起的中间状态.
  // 所以这之间的代码顺序并不重要.大部分时候,如果没有持有锁,
  // exit中任何代码顺序都不能工作.因为有了锁,代码的顺序就不再重要,
  // 因为父进程也看不到进程状态
  release(&original_parent->lock);
  // 必须在设置状态后释放父进程的说,否则可能造成...

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  // hold p->lock for the whole time to avoid lost
  // wakeups from a child's exit().
  acquire(&p->lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      // this code uses np->parent without holding np->lock.
      // acquiring the lock first would cause a deadlock,
      // since np might be an ancestor, and we already hold p->lock.
      if(np->parent == p){
        // np->parent can't change between the check and the acquire()
        // because only the parent changes it, and we're the parent.
        acquire(&np->lock);
        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&p->lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&p->lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&p->lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &p->lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  // 我们已经停止了spin进程的运行,所以我们需要抹去对于spin进程的记录
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    // 在实际运行进程之前,会执行intr_on函数来使得CPU能接收中断
    // 设置SSTATUS寄存器，打开中断标志位
    intr_on();
    
    int nproc = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      // 当我们开始要运行一个进程时,p->lock也有类似的保护功能
      // 当我们要运行一个进程时,我们需要将进程的状态设置为RUNNING,
      // 我们需要将进程的context移到RISC-V的寄存器中.
      // 但是,如果在这个过程中,发生了中断,
      // 从中断的角度来说进程将会处于一个奇怪的状态
      // 比如说进程的状态是RUNNING,但是又还没有将所有的寄存器从context对象
      // 拷贝到RISC-V寄存器中
      // 如果这时候有了一个定时器中断将会是个灾难
      // 因为我们可能在寄存器完全恢复之前,从这个进程中切换走
      // 而从这个进程切换走的过程中,将会保存不完整的RISC-V寄存器到进程的context对象中.
      // 所以我们希望启动一个进程的过程也具有原子性.
      // 在这种情况下,切换到一个进程的过程中,也需要获取进程的锁
      // 以确保其他的CPU核不能看到这个进程.同时在切换到进程的过程中,
      // 还需要关中断,避免定时器中断看到还在切换过程中的进程
      acquire(&p->lock);
      if(p->state != UNUSED) {
        nproc++;
      }
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;

        // trap.c(yield)->proc.c(sched)->swtch.S->scheduler.switch
        // 因为调度器线程中context上一次保存的地址是这里
        // 恢复的是目标进程的内核线程
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      //
      release(&p->lock);
    }
    if(nproc <= 2) {   // only init and sh exist
      intr_on();
      asm volatile("wfi");
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
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  // swtch函数会将当前的内核线程的寄存器保存到p->context中
  // swtch函数的另一个参数c->context,c表示当前CPU的结构体
  // CPU结构体中的context保存了当前CPU核的调度器线程的寄存器
  swtch(&p->context, &mycpu()->context);
  // Q: p->context->ra 里保存的什么
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  // 它首先获取了进程的锁.在锁释放之前,进程的状态会变得不一致,
  // 例如,yield将要将进程的状态改为RUNABLE,表明进程并没有在运行,
  // 但是实际上这个进程还在运行,代码正在当前进程的内核线程中运行
  // 所以这里加锁的目的之一就是:即使我们将进程的状态改为了RUNABLE,
  // 其他的CPU核的调度器线程也不可能看到进程的状态为RUNABLE并尝试运行它
  // 否则的话,进程就会在两个CPU核上运行了,而一个进程只有一个栈,
  // 这意味着两个CPU核在同一个栈上运行代码(因为XV6中一个用户进程只有一个用户线程)
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
// 第一个用户进程swtch的返回地址
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
// 调用sleep时需要持有condition lock,这样sleep函数才能知道相应的锁
// sleep函数只有在获取到进程的锁p->lock之后,才能释放condition lock
// wakeup需要同时持有两个锁才能查看进程
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.
  // 函数中第一件事情就是释放这个锁.
  // 当然在释放锁之后,我们会担心在这个时间点相应的wakeup会被调用并尝试唤醒当前进程
  // 而当前进程还没有进入到SLEEPING状态
  // 所以我们不能让wakeup在release锁之后执行
  // 为了让它不在release锁之后执行,在release锁之前,sleep会获取即将进入SLEEPING状态的进程的锁
  if(lk != &p->lock){  //DOC: sleeplock0
    acquire(&p->lock);  //DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &p->lock){
    release(&p->lock);
    acquire(lk);
  }
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == SLEEPING && p->chan == chan) {
      p->state = RUNNABLE;
    }
    release(&p->lock);
  }
}

// Wake up p if it is sleeping in wait(); used by exit().
// Caller must hold p->lock.
static void
wakeup1(struct proc *p)
{
  if(!holding(&p->lock))
    panic("wakeup1");
  if(p->chan == p && p->state == SLEEPING) {
    p->state = RUNNABLE;
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
// kill系统调用并不是真正的立即停止进程的运行,它更像是这样:如果进程在用户空间,
// 那么下一次它执行系统调用它就会退出,又或者目标进程正在执行用户代码,
// 当时下一次定时器中断或者其他中断触发了,进程才会退出
// 所以从一个进程调用kill,到另一个进程真正退出,中间可能有很明显的延时

// 学生提问:为什么一个进程允许kill另一个进程?这样一个进程不是能杀掉所有其他进程吗?
// Robert教授:在XV6中允许这么做是因为,XV6这是个教学用的操作系统,
// 任何与权限相关的内容在XV6中都不存在.在Linux或者真正的操作系统中,
// 每个进程都有一个user id或多或少的对应了执行进程的用户,
// 一些系统调用使用进程的user id来检查进程允许做的操作.
// 所以在Linux中会有额外的检查,调用kill的进程必须与被kill的进程有相同的user id
// 否则的话,kill操作不被允许
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      // 所以对于SLEEPING状态的进程,如果它被kill了,它会被直接唤醒,
      // 包装了sleep的循环会检查进程的killed标志位,最后再调用exit
      // 同时还有一些情况,如果进程在SLEEPING状态中被kill了并不能直接退出
      // 例如,一个进程正在更新一个文件系统并创建一个文件的过程中,
      // 进程不适宜在这个时间点退出,因为我们想要完成文件系统的操作,之后进程才能退出
      // 我会向你展示一个磁盘驱动中的sleep循环,这个循环中就没有检查进程的killed标志位
      p->killed = 1;
      // 进程运行到内核代码中能安全停止运行的位置时,会检查自己的killed标志位
      // 如果设置为1,目标进程会自愿的执行exit系统调用
      // 你可以在trap.c中看到所有可以安全停止运行的位置
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
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
