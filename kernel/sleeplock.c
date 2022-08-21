// Sleeping locks

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"

void
initsleeplock(struct sleeplock *lk, char *name)
{
  initlock(&lk->lk, "sleep lock");
  lk->name = name;
  lk->locked = 0;
  lk->pid = 0;
}

// 对于spinlock有很多限制,其中之一是加锁时中断必须要关闭
// 所以如果使用spinlock的话,当我们对block cache做操作的时候需要持有锁,
// 那么我们就永远也不能从磁盘收到数据,或许另一个CPU核可以收到中断并读到磁盘数据
// 但是如果我们只有一个CPU核的话,我们就永远也读不到数据了
// 出于同样的原因,也不能在持有spinlock的时候进入sleep状态
// (见13.1: sleep会使得线程切换,而在xv6线程切换过程中,只能持有proc->lock).
// 所以这里我们使用sleep lock.
// sleep lock的优势就是,我们可以在持有锁的时候不关闭中断
// 我们可以在磁盘操作的过程中持有锁,我们也可以长时间持有锁
// 当我们在等待sleep lock的时候,我们并没有让CPU一直空转,我们通过sleep将CPU出让出去了
void
acquiresleep(struct sleeplock *lk)
{
  acquire(&lk->lk);
  while (lk->locked) {
    sleep(lk, &lk->lk);
  }
  lk->locked = 1;
  lk->pid = myproc()->pid;
  release(&lk->lk);
}

void
releasesleep(struct sleeplock *lk)
{
  acquire(&lk->lk);
  lk->locked = 0;
  lk->pid = 0;
  wakeup(lk);
  release(&lk->lk);
}

int
holdingsleep(struct sleeplock *lk)
{
  int r;
  
  acquire(&lk->lk);
  r = lk->locked && (lk->pid == myproc()->pid);
  release(&lk->lk);
  return r;
}



