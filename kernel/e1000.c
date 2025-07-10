#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static char *tx_bufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static char *rx_bufs[RX_RING_SIZE];

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
    tx_bufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_bufs[i] = kalloc();
    if (!rx_bufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_bufs[i];
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

int
e1000_transmit(char *buf, int len)
{
  // 获取锁，保护对发送环形队列的并发访问。
  acquire(&e1000_lock);

  // 1. 读取 TDT (Transmit Descriptor Tail) 寄存器，获取下一次要填充的描述符的索引。
  int next_tx_idx = regs[E1000_TDT];

  // 2. 检查发送环形队列是否已满。
  //    如果下一个要使用的描述符的 DD (Descriptor Done) 位没有被设置，
  //    说明网卡还没有处理完上次放在这里的发送请求，队列已满。
  if ((tx_ring[next_tx_idx].status & E1000_TXD_STAT_DD) == 0) {
    // 队列已满，释放锁并返回错误。
    // 调用者会负责处理这个错误（比如释放它传过来的 buf）。
    release(&e1000_lock);
    return -1;
  }

  // 3. 如果之前的描述符已经被处理 (DD 位已设置)，释放上一次与该描述符关联的缓冲区。
  //    这是为了防止内存泄漏。tx_bufs 数组保存了每个描述符对应的缓冲区指针。
  //    注意：我们释放的是上一次发送完成的 buffer，而不是现在要发送的这个。
  if (tx_bufs[next_tx_idx]) {
    kfree(tx_bufs[next_tx_idx]);
  }

  // 4. 将新的缓冲区指针保存起来，以便在发送完成后释放。
  //    buf 是由 net.c 中 kalloc() 分配的，现在驱动程序接管了它的所有权。
  tx_bufs[next_tx_idx] = buf;

  // 5. 填充描述符。
  //    - addr: 设置为数据包缓冲区(buf)的物理地址。
  //    - length: 设置数据包的长度(len)。
  //    - cmd: 设置命令标志 (EOP 和 RS)。
  tx_ring[next_tx_idx].addr = (uint64)buf;
  tx_ring[next_tx_idx].length = len;
  tx_ring[next_tx_idx].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;

  // 6. 更新 TDT 寄存器。
  //    将 TDT 指向下一个可用的描述符，通知网卡有新的数据包等待发送。
  regs[E1000_TDT] = (next_tx_idx + 1) % TX_RING_SIZE;

  // 释放锁。
  release(&e1000_lock);

  return 0; // 返回 0 表示成功
}

static void
e1000_recv(void)
{
  // 这个函数在中断处理程序 e1000_intr() 中被调用，该函数已经获取了 e1000_lock。

  while(1) {
    // 1. 计算下一个要检查的接收描述符的索引。
    //    RDT (Receive Descriptor Tail) 是驱动已处理的最后一个描述符的索引。
    //    所以下一个可能包含新数据包的是 (RDT + 1) % RX_RING_SIZE。
    int next_rx_idx = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
    struct rx_desc *desc = &rx_ring[next_rx_idx];

    // 2. 检查描述符的 status 字段中的 DD (Descriptor Done) 位。
    //    如果 DD 位没有被设置，说明没有新的数据包了。
    if ((desc->status & E1000_RXD_STAT_DD) == 0) {
      break;
    }

    // 3. 有新数据包。获取缓冲区指针和数据包长度。
    //    rx_bufs 数组保存了与每个接收描述符关联的缓冲区。
    char *buf = rx_bufs[next_rx_idx];
    int len = desc->length;

    // 4. 将接收到的数据包（缓冲区指针和长度）传递给上层网络协议栈进行处理。
    net_rx(buf, len);

    // 5. 为这个描述符重新分配一个新的缓冲区，以备接收下一个数据包。
    //    因为旧的缓冲区 buf 已经交给网络栈了，我们必须提供一个新的空缓冲区给网卡。
    char *new_buf = kalloc();
    if (!new_buf) {
      // 这是一个严重问题，但在本实验中，我们假设它总能成功。
      panic("e1000_recv: kalloc failed");
    }
    // 更新我们的记录，让 rx_bufs 指向新的缓冲区。
    rx_bufs[next_rx_idx] = new_buf;
    
    // 6. 更新硬件描述符，使其指向新分配的缓冲区，并清除状态。
    desc->addr = (uint64)new_buf;
    desc->status = 0;

    // 7. 更新 RDT 寄存器。
    //    将 RDT 指向我们刚刚处理完的这个描述符，告诉网卡这个位置的缓冲区可以被重用了。
    regs[E1000_RDT] = next_rx_idx;
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
