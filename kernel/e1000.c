#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}
// 参考自: https://blog.miigon.net/posts/s081-lab11-network/
int
e1000_transmit(struct mbuf *m)
{
  //
  // Your code here.
  //
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //

  acquire(&e1000_lock);
  uint32 p = regs[E1000_TDT];
  if ((tx_ring[p].status & E1000_TXD_STAT_DD) == 0){
    release(&e1000_lock);
    return -1;
  }
  // 释放从该描述符传输的最后一个 mbuf
  if (tx_mbufs[p]){
    mbuffree(tx_mbufs[p]);
    tx_mbufs[p] = 0;
  }
  tx_ring[p].addr = (uint64)m->head;
  tx_ring[p].length = m->len;
  // 设置参数，EOP 表示该 buffer 含有一个完整的 packet
  // *** RS 告诉网卡在发送完成后,设置 status 中的 E1000_TXD_STAT_DD 位,表示发送完成 ***
  tx_ring[p].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  // 保存指针,下一次到这个位置时释放物理内存
  tx_mbufs[p] = m;
  regs[E1000_TDT] = (p + 1) % RX_RING_SIZE;
  release(&e1000_lock);
  return 0;
}

static void
e1000_recv(void)
{
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //
  // recv()是在bottom half的interrupt handler中,只有一个进程在跑这个handler,因此不存在共享的数据结构
  // from https://fanxiao.tech/posts/MIT-6S081-notes/
  while (1){
    // 首先通过提取 E1000_RDT 控制寄存器并加一对 RX_RING_SIZE 取模,
    // 向E1000询问下一个等待接收数据包(如果有)所在的环索引
    uint32 p = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
    // 检查描述符 status 部分中的 E1000_RXD_STAT_DD 位来检查新数据包是否可用
    if(!(rx_ring[p].status & E1000_RXD_STAT_DD)){
      return;
    }
    // 将 mbuf 的 m->len 更新为描述符中报告的长度
    rx_mbufs[p]->len = rx_ring[p].length;
    // 将 mbuf 传送到网络栈
    net_rx(rx_mbufs[p]);
    // 替换刚刚给 net_rx() 的 mbuf
    struct mbuf* b = mbufalloc(0);
    // 数据指针(m->head)编程到描述符
    rx_ring[p].addr = (uint64)b->head;
    // 描述符的状态位清除为零
    rx_ring[p].status = 0;
    rx_mbufs[p] = b;
    // 将 E1000_RDT 寄存器更新为最后处理的环描述符的索引
    regs[E1000_RDT] = p;
  }
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
