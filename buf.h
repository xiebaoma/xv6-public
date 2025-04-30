struct buf
{
  int flags;             // 标志位，表示当前缓冲区的状态（如是否有效/脏）
  uint dev;              // 所属设备号（通常是硬盘）
  uint blockno;          // 缓存的磁盘块号
  struct sleeplock lock; // 用于锁定该缓冲区，防止并发访问
  uint refcnt;           // 引用计数，表示有多少人正在使用这个缓冲区

  struct buf *prev; // LRU 缓存链表的前向指针
  struct buf *next; // LRU 缓存链表的后向指针

  struct buf *qnext; // 用于磁盘队列（不是 LRU），异步写时使用

  uchar data[BSIZE]; // 实际缓存的数据内容，一个磁盘块（512 字节）
};

#define B_VALID 0x2 // 表示缓冲区数据是有效的（已经从磁盘读取过）
#define B_DIRTY 0x4 // 表示缓冲区内容已被修改，需要写回磁盘
