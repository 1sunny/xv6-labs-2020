//
// low-level driver routines for 16550a UART.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// the UART control registers are memory-mapped
// at address UART0. this macro returns the
// address of one of the registers.
#define Reg(reg) ((volatile unsigned char *)(UART0 + reg))

// the UART control registers.
// some have different meanings for
// read vs write.
// see http://byterunner.com/16550.html
#define RHR 0                 // receive holding register (for input bytes)
#define THR 0                 // transmit holding register (for output bytes)
#define IER 1                 // interrupt enable register
#define IER_RX_ENABLE (1<<0)
#define IER_TX_ENABLE (1<<1)
#define FCR 2                 // FIFO control register
#define FCR_FIFO_ENABLE (1<<0)
#define FCR_FIFO_CLEAR (3<<1) // clear the content of the two FIFOs
#define ISR 2                 // interrupt status register
#define LCR 3                 // line control register
#define LCR_EIGHT_BITS (3<<0)
#define LCR_BAUD_LATCH (1<<7) // special mode to set baud rate
#define LSR 5                 // line status register
#define LSR_RX_READY (1<<0)   // input is waiting to be read from RHR
#define LSR_TX_IDLE (1<<5)    // THR can accept another character to send

#define ReadReg(reg) (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))

// the transmit output buffer.
struct spinlock uart_tx_lock;
#define UART_TX_BUF_SIZE 32
char uart_tx_buf[UART_TX_BUF_SIZE];
int uart_tx_w; // write next to uart_tx_buf[uart_tx_w++]
int uart_tx_r; // read next from uart_tx_buf[uar_tx_r++]

extern volatile int panicked; // from printf.c

void uartstart();

void
uartinit(void)
{
  // 这里的流程是先关闭中断,之后设置波特率,
  // 设置字符长度为8bit,重置FIFO,最后再重新打开中断
  // disable interrupts.
  WriteReg(IER, 0x00);

  // special mode to set baud rate.
  WriteReg(LCR, LCR_BAUD_LATCH);

  // LSB for baud rate of 38.4K.
  WriteReg(0, 0x03);

  // MSB for baud rate of 38.4K.
  WriteReg(1, 0x00);

  // leave set-baud mode,
  // and set word length to 8 bits, no parity.
  WriteReg(LCR, LCR_EIGHT_BITS);

  // reset and enable FIFOs.
  WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

  // enable transmit and receive interrupts.
  WriteReg(IER, IER_TX_ENABLE | IER_RX_ENABLE);

  initlock(&uart_tx_lock, "uart");
}

// add a character to the output buffer and tell the
// UART to start sending if it isn't already.
// blocks if the output buffer is full.
// because it may block, it can't be called
// from interrupts; it's only suitable for use
// by write().
void
uartputc(int c)
{
  //锁确保了我们可以在下一个字符写入到缓存之前,处理完缓存中的字符,这样缓存中的数据就不会被覆盖
  acquire(&uart_tx_lock);

  if(panicked){
    for(;;)
      ;
  }
// 在UART的内部会有一个buffer用来发送数据,buffer的大小是32个字符
// 同时还有一个为consumer提供的读指针和为producer提供的写指针,
// 来构建一个环形的buffer(注，或者可以认为是环形队列)
  while(1){
    // 第一件事情是判断环形buffer是否已经满了
    // 如果读写指针相同,那么buffer是空的,如果写指针加1等于读指针,那么buffer满了
    if(((uart_tx_w + 1) % UART_TX_BUF_SIZE) == uart_tx_r){
      // buffer is full.
      // wait for uartstart() to open up space in the buffer.
      // 暂时搁置Shell并运行其他的进程
      sleep(&uart_tx_r, &uart_tx_lock);
    } else {
      uart_tx_buf[uart_tx_w] = c;
      uart_tx_w = (uart_tx_w + 1) % UART_TX_BUF_SIZE;
      uartstart();
      release(&uart_tx_lock);
      return;
    }
  }
}

// alternate version of uartputc() that doesn't 
// use interrupts, for use by kernel printf() and
// to echo characters. it spins waiting for the uart's
// output register to be empty.
void
uartputc_sync(int c)
{
  push_off();

  if(panicked){
    for(;;)
      ;
  }

  // wait for Transmit Holding Empty to be set in LSR.
  while((ReadReg(LSR) & LSR_TX_IDLE) == 0)
    ;
  WriteReg(THR, c);

  pop_off();
}

// if the UART is idle, and a character is waiting
// in the transmit buffer, send it.
// caller must hold uart_tx_lock.
// called from both the top- and bottom-half.
void
uartstart()
{
  // 通知设备执行操作,循环将buffer中的字符全部送出
  // 每当有一个中断,并且读指针落后于写指针,
  // uartintr函数就会从读指针中读取一个字符再通过UART设备发送,并且将读指针加1
  // 当读指针追上写指针,也就是两个指针相等的时候,buffer为空,这时就不用做任何操作
  while(1){
    if(uart_tx_w == uart_tx_r){
      // transmit buffer is empty.
      return;
    }
    // 先是检查当前设备是否空闲,
    if((ReadReg(LSR) & LSR_TX_IDLE) == 0){
      // the UART transmit holding register is full,
      // so we cannot give it another byte.
      // it will interrupt when it's ready for a new byte.
      return;
    }
    // 如果空闲的话,我们会从buffer中读出数据
    int c = uart_tx_buf[uart_tx_r];
    uart_tx_r = (uart_tx_r + 1) % UART_TX_BUF_SIZE;
    
    // maybe uartputc() is waiting for space in the buffer.
    wakeup(&uart_tx_r);

    // 锁确保了一个时间只有一个CPU上的进程可以写入UART的寄存器,THR
    // 所以这里锁确保了硬件寄存器只有一个写入者

    // 然后将数据写入到THR(Transmission Holding Register)发送寄存器
    // 这里相当于告诉设备,我这里有一个字节需要你来发送
    // 一旦数据送到了设备,系统调用会返回,用户应用程序Shell就可以继续执行
    // 这里从内核返回到用户空间的机制与lec06的trap机制是一样的
    WriteReg(THR, c);
    //  当UART硬件完成传输，会产生一个中断

    // 与此同时,UART设备会将数据送出.在某个时间点,我们会收到中断,
    // 因为我们之前设置了要处理UART设备中断
    // 接下来我们看一下,当发生中断时,实际会发生什么
    // ->
    // 在我们向Console输出字符时,如果发生了中断,RISC-V会做什么操作?
    // 我们之前已经在SSTATUS寄存器中打开了中断,所以处理器会被中断.
    // 假设键盘生成了一个中断并且发向了PLIC,PLIC会将中断路由给一个特定的CPU核,
    // 并且如果这个CPU核设置了SIE寄存器的E(enable) bit(注,针对外部中断的bit位),
    // 1. 首先,会清除SIE寄存器相应的bit,这样可以阻止CPU核被其他中断打扰,该CPU核可以专心处理当前中断.
    //    处理完成之后,可以再次恢复SIE寄存器相应的bit.
    // 2. 会设置SEPC寄存器为当前的程序计数器.
    //    我们假设Shell正在用户空间运行,突然来了一个中断,
    //    那么当前Shell的程序计数器会被保存.
    // 3. 要保存当前的mode.在我们的例子里面,因为当前运行的是Shell程序,所以会记录user mode
    //    再将mode设置为Supervisor mode
    // 4. 将程序计数器的值设置成STVEC的值(STVEC保存trap处理程序的地址)
    //    在XV6中,STVEC保存的要么是uservec或者kernelvec函数的地址,
    //    具体取决于发生中断时程序运行是在用户空间还是内核空间
    //    在我们的例子中,Shell运行在用户空间,所以STVEC保存的是uservec函数的地址
    //    而从之前的课程我们可以知道uservec函数会调用usertrap函数
    //    所以最终,我们在usertrap函数中
  }
}

// read one input character from the UART.
// return -1 if none is waiting.
int
uartgetc(void)
{
  if(ReadReg(LSR) & 0x01){
    // input data is ready.
    return ReadReg(RHR);
  } else {
    return -1;
  }
}

// handle a uart interrupt, raised because input has
// arrived, or the uart is ready for more output, or
// both. called from trap.c.
void
uartintr(void)
{
  // read and process incoming characters.
  while(1){
    int c = uartgetc();
    if(c == -1)
      break;
    consoleintr(c);
  }

  // send buffered characters.
  // 当UART硬件完成传输,会产生一个中断
  // uartstart的调用者会获得锁以确保不会有多个进程同时向THR寄存器写数据
  // 但是UART中断本身也可能与调用printf的进程并行执行
  // 如果一个进程调用了printf,它运行在CPU0上
  // CPU1处理了UART中断,那么CPU1也会调用uartstart
  // 因为我们想要确保对于THR寄存器只有一个写入者,
  // 同时也确保传输缓存的特性不变(指在uartstart中对于uart_tx_r指针的更新)
  // 我们需要在中断处理函数中也获取锁
  acquire(&uart_tx_lock);
  uartstart();
  release(&uart_tx_lock);
  // 驱动的bottom部分(中断处理程序uartstart)和驱动的up部分(uartputc函数)
  // 可以完全的并行运行,所以中断处理程序也需要获取锁
}
