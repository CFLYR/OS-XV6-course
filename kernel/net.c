#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

// xv6's ethernet and IP addresses
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// qemu host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

// 每个端口的数据包队列最大长度
#define MAX_UDP_QUEUE 16
// 系统支持的最大绑定端口数量
#define MAX_UDP_PORTS 32

// 表示一个被绑定的UDP端口及其相关信息
struct socket {
  int in_use;           // 标记此结构是否被使用
  short port;           // 绑定的端口号 (主机字节序)
  
  // 数据包队列
  char *q[MAX_UDP_QUEUE]; // 指向数据包缓冲区的指针数组
  int q_len;            // 当前队列中的数据包数量
  int q_read;           // 队列的读指针 (下一个要被recv读取的包)
  int q_write;          // 队列的写指针 (下一个新包要存放的位置)
};

// 全局结构，用于管理所有绑定的UDP端口
struct {
  struct spinlock lock; // 全局锁
  struct socket sockets[MAX_UDP_PORTS];
} udp_sockets;

void
netinit(void)
{
  initlock(&udp_sockets.lock, "udp_sockets_global");
}

//
// bind(int port)
// prepare to receive UDP packets address to the port,
// i.e. allocate any queues &c needed.
//
uint64
sys_bind(void)
{
  short port;
  argint(0, (int*)&port);

  acquire(&udp_sockets.lock);

  // 检查端口是否已被绑定
  for (int i = 0; i < MAX_UDP_PORTS; i++) {
    if (udp_sockets.sockets[i].in_use && udp_sockets.sockets[i].port == port) {
      release(&udp_sockets.lock);
      return -1;
    }
  }

  // 查找一个未使用的socket结构
  struct socket *sock = 0;
  for (int i = 0; i < MAX_UDP_PORTS; i++) {
    if (udp_sockets.sockets[i].in_use == 0) {
      sock = &udp_sockets.sockets[i];
      break;
    }
  }

  if (sock == 0) {
    release(&udp_sockets.lock);
    return -1;
  }

  // 初始化找到的socket结构
  sock->in_use = 1;
  sock->port = port;
  sock->q_len = 0;
  sock->q_read = 0;
  sock->q_write = 0;

  release(&udp_sockets.lock);

  return 0;
}

//
// unbind(int port)
// release any resources previously created by bind(port);
// from now on UDP packets addressed to port should be dropped.
//
uint64
sys_unbind(void)
{
  short port;
  argint(0, (int*)&port);

  acquire(&udp_sockets.lock);

  // 查找匹配的socket
  struct socket *sock = 0;
  for (int i = 0; i < MAX_UDP_PORTS; i++) {
    if (udp_sockets.sockets[i].in_use && udp_sockets.sockets[i].port == port) {
      sock = &udp_sockets.sockets[i];
      break;
    }
  }

  // 如果没有找到绑定的socket，直接返回成功
  if (sock == 0) {
    release(&udp_sockets.lock);
    return 0;
  }

  // 清理队列中所有待处理的数据包
  for (int i = 0; i < sock->q_len; i++) {
    int idx = (sock->q_read + i) % MAX_UDP_QUEUE;
    kfree(sock->q[idx]);
    sock->q[idx] = 0; // 清空指针
  }
  
  // 重置socket状态
  sock->in_use = 0;
  sock->port = 0;
  sock->q_len = 0;
  sock->q_read = 0;
  sock->q_write = 0;

  release(&udp_sockets.lock);
  
  return 0;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
//
uint64
sys_recv(void)
{
  short dport;
  uint64 src_p, sport_p, buf_p;
  int maxlen;
  struct proc *pr = myproc();

  argint(0, (int*)&dport);
  argaddr(1, &src_p);
  argaddr(2, &sport_p);
  argaddr(3, &buf_p);
  argint(4, &maxlen);

  acquire(&udp_sockets.lock);

  // 查找绑定的socket
  struct socket *sock = 0;
  for (int i = 0; i < MAX_UDP_PORTS; i++) {
    if (udp_sockets.sockets[i].in_use && udp_sockets.sockets[i].port == dport) {
      sock = &udp_sockets.sockets[i];
      break;
    }
  }

  if (sock == 0) {
    release(&udp_sockets.lock);
    return -1;
  }
  
  // 等待数据包到达
  while (sock->q_len == 0) {
    sleep(sock, &udp_sockets.lock);
  }

  // 从队列中取出一个数据包
  char *pkt_buf = sock->q[sock->q_read];
  sock->q_read = (sock->q_read + 1) % MAX_UDP_QUEUE;
  sock->q_len--;

  release(&udp_sockets.lock);
  
  // 解析数据包
  struct ip *iph = (struct ip *)(pkt_buf + sizeof(struct eth));
  struct udp *udph = (struct udp *)((char *)iph + sizeof(struct ip));

  // 提取源IP和源端口
  uint32 src_ip = ntohl(iph->ip_src);
  short src_port = ntohs(udph->sport);

  // 计算并拷贝payload
  int payload_len = ntohs(udph->ulen) - sizeof(struct udp);
  int copy_len = payload_len < maxlen ? payload_len : maxlen;
  char *payload = (char *)udph + sizeof(struct udp);

  if (copyout(pr->pagetable, src_p, (char *)&src_ip, sizeof(src_ip)) < 0 ||
      copyout(pr->pagetable, sport_p, (char *)&src_port, sizeof(src_port)) < 0 ||
      copyout(pr->pagetable, buf_p, payload, copy_len) < 0) {
    kfree(pkt_buf);
    return -1;
  }

  kfree(pkt_buf);

  return copy_len;
}

static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  while (nleft > 1)  {
    sum += *w++;
    nleft -= 2;
  }

  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);

  answer = ~sum;
  return answer;
}

uint64
sys_send(void)
{
  struct proc *p = myproc();
  int sport;
  int dst;
  int dport;
  uint64 bufaddr;
  int len;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  int total = len + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();
  if(buf == 0){
    printf("sys_send: kalloc failed\n");
    return -1;
  }
  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);
  ip->ip_vhl = 0x45;
  ip->ip_tos = 0;
  ip->ip_len = htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst);
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));

  struct udp *udp = (struct udp *)(ip + 1);
  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen = htons(len + sizeof(struct udp));

  char *payload = (char *)(udp + 1);
  if(copyin(p->pagetable, payload, bufaddr, len) < 0){
    kfree(buf);
    printf("send: copyin failed\n");
    return -1;
  }

  e1000_transmit(buf, total);

  return 0;
}

void
ip_rx(char *buf, int len)
{
  // don't delete this printf; make grade depends on it.
  static int seen_ip = 0;
  if(seen_ip == 0)
    printf("ip_rx: received an IP packet\n");
  seen_ip = 1;

  struct ip *iph = (struct ip *)(buf + sizeof(struct eth));
  
  if (iph->ip_p != IPPROTO_UDP) {
    kfree(buf);
    return;
  }
  
  struct udp *udph = (struct udp *)((char *)iph + sizeof(struct ip));

  short dport = ntohs(udph->dport);
  
  acquire(&udp_sockets.lock);

  struct socket *sock = 0;
  for (int i = 0; i < MAX_UDP_PORTS; i++) {
    if (udp_sockets.sockets[i].in_use && udp_sockets.sockets[i].port == dport) {
      sock = &udp_sockets.sockets[i];
      break;
    }
  }

  if (sock == 0 || sock->q_len >= MAX_UDP_QUEUE) {
    release(&udp_sockets.lock);
    kfree(buf);
    return;
  }

  sock->q[sock->q_write] = buf;
  sock->q_write = (sock->q_write + 1) % MAX_UDP_QUEUE;
  sock->q_len++;
  
  wakeup(sock);
  
  release(&udp_sockets.lock);
}

void
arp_rx(char *inbuf)
{
  static int seen_arp = 0;

  if(seen_arp){
    kfree(inbuf);
    return;
  }
  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth *ineth = (struct eth *) inbuf;
  struct arp *inarp = (struct arp *) (ineth + 1);

  char *buf = kalloc();
  if(buf == 0)
    panic("send_arp_reply");
  
  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, ineth->shost, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_ARP);

  struct arp *arp = (struct arp *)(eth + 1);
  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha, local_mac, ETHADDR_LEN);
  arp->sip = htonl(local_ip);
  memmove(arp->tha, ineth->shost, ETHADDR_LEN);
  arp->tip = inarp->sip;

  e1000_transmit(buf, sizeof(*eth) + sizeof(*arp));

  kfree(inbuf);
}

void
net_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *) buf;

  if(len >= sizeof(struct eth) + sizeof(struct arp) &&
     ntohs(eth->type) == ETHTYPE_ARP){
    arp_rx(buf);
  } else if(len >= sizeof(struct eth) + sizeof(struct ip) &&
     ntohs(eth->type) == ETHTYPE_IP){
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}