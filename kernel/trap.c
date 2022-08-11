#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void) {
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void) {
  w_stvec((uint64) kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void) {
  int which_dev = 0;

  if ((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64) kernelvec);

  struct proc *p = myproc();

  // save user program counter.
  // 接下来我们要保存用户程序计数器,它仍然保存在SEPC寄存器中,但是可能发生这种情况:
  // 当程序还在内核中执行时,我们可能切换到另一个进程,并进入到那个程序的用户空间,
  // 然后那个进程可能再调用一个系统调用进而导致SEPC寄存器的内容被覆盖
  // 所以,我们需要保存当前进程的SEPC寄存器到一个与该进程关联的内存中,这样这个数据才不会被覆盖
  p->trapframe->epc = r_sepc();

  // 接下来我们需要找出我们现在会在usertrap函数的原因
  // 根据触发trap的原因,RISC-V的SCAUSE寄存器会有不同的数字.
  // 数字8表明,我们现在在trap代码中是因为系统调用
  if (r_scause() == 8) {
    // system call

    if (p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    // 在RISC-V中,存储在SEPC寄存器中的程序计数器,是用户程序中触发trap的指令的地址
    // 但是当我们恢复用户程序时,我们希望在下一条指令恢复,也就是ecall之后的一条指令
    // 所以对于系统调用,我们对于保存的用户程序计数器加4,这样我们会在ecall的下一条指令恢复,而不是重新执行ecall指令
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    // XV6会在处理系统调用的时候使能中断,这样中断可以更快的服务,有些系统调用需要许多时间处理
    // 中断总是会被RISC-V的trap硬件关闭,所以在这个时间点,我们需要显式的打开中断
    intr_on();

    syscall();
  } else if ((which_dev = devintr()) != 0) {
    // ok
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }
  // 我们再次检查当前用户进程是否被杀掉了,因为我们不想恢复一个被杀掉的进程
  if (p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  // if(which_dev == 2)
  //   yield();
  // --- my code for lab4 start ---
  if (which_dev == 2) {
    if (p->alarm_period) {
      p->ticks_past++;
      if (p->ticks_past == p->alarm_period) {
        p->ticks_past = 0; // !
        if (p->in_handler == 0) {
          p->alarm_trapframe = p->alarm_trapframe ? p->alarm_trapframe : (struct trapframe *) kalloc();
          // *p->alarm_trapframe = *p->trapframe;
          memmove(p->alarm_trapframe, p->trapframe, 512);
          p->trapframe->epc = p->handler;
          p->in_handler = 1;
        } else {
          yield();
        }
      }else{
        yield();
      }
    } else {
      yield();
    }
  }
  // --- my code for lab4 end ---

  // usertrap函数的最后调用了usertrapret函数,来设置好我之前说过的,在返回到用户空间之前内核要做的工作
  usertrapret();
}

//
// return to user space
//
void
usertrapret(void) {
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  // 我们之前在系统调用的过程中是打开了中断的,
  // 这里关闭中断是因为我们将要更新STVEC寄存器来指向用户空间的trap处理代码,
  // 而之前在内核中的时候,我们指向的是内核空间的trap处理代码（6.6）
  // 我们关闭中断因为当我们将STVEC更新到指向用户空间的trap处理代码时,我们仍然在内核中执行代码
  // 如果这时发生了一个中断,那么程序执行会走向用户空间的trap处理代码
  // 即便我们现在仍然在内核中,出于各种各样具体细节的原因,这会导致内核出错,所以我们这里关闭中断
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  // 在下一行我们设置了STVEC寄存器指向trampoline代码,在那里最终会执行sret指令返回到用户空间
  // 位于trampoline代码最后的sret指令会重新打开中断
  // 即使我们刚刚关闭了中断,当我们在执行用户代码时中断是打开的
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  // 现在我们在usertrapret函数中,我们正在设置trapframe中的数据.
  // 这样下一次从用户空间转换到内核空间时可以用到这些数据
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64) usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()
  /*
   * 学生提问:为什么trampoline代码中不保存SEPC寄存器?
   * Robert教授:可以存储. trampoline代码没有像其他寄存器一样保存这个寄存器,
   * 但是我们非常欢迎大家修改XV6来保存它.如果你还记得的话,这个寄存器实际上是在C代码usertrap中保存的,
   * 而不是在汇编代码trampoline中保存的. 我想不出理由这里哪种方式更好.
   * 用户寄存器(User Registers)必须在汇编代码中保存,因为任何需要经过编译器的语言
   * 例如C语言,都不能修改任何用户寄存器. 所以对于用户寄存器,必须要在进入C代码之前在汇编代码中保存好
   * 但是对于SEPC寄存器(注,控制寄存器),我们可以早点保存或者晚点保存
   * */

  // set up the registers that trampoline.S's sret will use
  // to get to user space.

  // set S Previous Privilege mode to User.
  // 我们要设置SSTATUS寄存器,这是一个控制寄存器.
  // 这个寄存器的SPP bit位控制了sret指令的行为,该bit为0表示下次执行sret的时候,
  // 我们想要返回user mode而不是supervisor mode.
  // 这个寄存器的SPIE bit位控制了,在执行完sret之后,是否打开中断.
  // 因为我们在返回到用户空间之后,我们的确希望打开中断,所以这里将SPIE bit位设置为1
  // 修改完这些bit位之后,我们会把新的值写回到SSTATUS寄存器
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  // 这条指令会将程序计数器设置成SEPC寄存器的值,
  // 所以现在我们将SEPC寄存器的值设置成之前保存的用户程序计数器的值
  // 在不久之前,我们在usertrap函数中将用户程序计数器保存在trapframe中的epc字段
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  // 根据user page table地址生成相应的SATP值,这样我们在返回到用户空间的时候才能完成page table的切换
  // 实际上,我们会在汇编代码trampoline中完成page table的切换,并且也只能在trampoline中完成切换,
  // 因为只有trampoline中代码是同时在用户和内核空间中映射.切换后,pc才是在页表中才是正确的
  // 但是我们现在还没有在trampoline代码中,我们现在还在一个普通的C函数中,
  // 所以这里我们将page table指针准备好,并将这个指针作为第二个参数传递给汇编代码,这个参数会出现在a1寄存器
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  // 计算出我们将要跳转到汇编代码的地址.
  // 我们期望跳转的地址是tampoline中的userret函数,这个函数包含了所有能将我们带回到用户空间的指令
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64, uint64)) fn)(TRAPFRAME, satp);

  // back to trampoline.S
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void
kerneltrap() {
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();

  if ((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if (intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if ((which_dev = devintr()) == 0) {
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  // 如果由于计时器中断而调用了 kerneltrap,并且一个进程的内核线程正在运行(而
  // 不是调度程序线程),kerneltrap 会调用 yield,给其他线程一个运行的机会
  // 某个时刻,其中一个线程会让步,让我们的线程和它的 kerneltrap 再次恢复
  if (which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr() {
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr() {
  uint64 scause = r_scause();

  if ((scause & 0x8000000000000000L) &&
      (scause & 0xff) == 9) {
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if (irq == UART0_IRQ) {
      uartintr();
    } else if (irq == VIRTIO0_IRQ) {
      virtio_disk_intr();
    } else if (irq) {
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if (irq)
      plic_complete(irq);

    return 1;
  } else if (scause == 0x8000000000000001L) {
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if (cpuid() == 0) {
      clockintr();
    }

    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

