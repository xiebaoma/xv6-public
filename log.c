#include "types.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// Simple logging that allows concurrent FS system calls.
//
// A log transaction contains the updates of multiple FS system
// calls. The logging system only commits when there are
// no FS system calls active. Thus there is never
// any reasoning required about whether a commit might
// write an uncommitted system call's updates to disk.
//
// A system call should call begin_op()/end_op() to mark
// its start and end. Usually begin_op() just increments
// the count of in-progress FS system calls and returns.
// But if it thinks the log is close to running out, it
// sleeps until the last outstanding end_op() commits.
//
// The log is a physical re-do log containing disk blocks.
// The on-disk log format:
//   header block, containing block #s for block A, B, C, ...
//   block A
//   block B
//   block C
//   ...
// Log appends are synchronous.

// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block# before commit.
struct logheader
{
  int n;              // number of blocks in this transaction
  int block[LOGSIZE]; // 一个数组，记录了本次事务中哪些磁盘块被修改（即块号列表）
};

struct log
{
  struct spinlock lock;
  int start;       // block # of start of log on disk
  int size;        // size of log in blocks
  int outstanding; // how many FS sys calls are executing.
  int committing;  // in commit(), please wait.
  int dev;
  struct logheader lh;
};
struct log log;

static void recover_from_log(void);
static void commit();

void initlog(int dev)
{
  if (sizeof(struct logheader) >= BSIZE)
    panic("initlog: too big logheader");

  struct superblock sb;
  initlock(&log.lock, "log");
  readsb(dev, &sb); // 从磁盘上读取超级块（superblock）到内存中，获取日志在磁盘上的位置和大小等元信息。
                    // readsb() 其实是从磁盘的 block 1（超级块）中读取 struct superblock 的数据。
  log.start = sb.logstart;
  log.size = sb.nlog;
  log.dev = dev;
  recover_from_log(); // 如果系统上次崩溃时还有未完成的 commit 操作（日志区中的数据还没完全写入真实数据区），就会在这里检测到，并将这些块重新写回到目标位置
}

// Copy committed blocks from log to their home location
static void
install_trans(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++)
  {
    struct buf *lbuf = bread(log.dev, log.start + tail + 1); // read log block
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]);   // read dst
    memmove(dbuf->data, lbuf->data, BSIZE);                  // copy block to dst
    bwrite(dbuf);                                            // write dst to disk
    brelse(lbuf);
    brelse(dbuf);
  }
}

// Read the log header from disk into the in-memory log header
/*
块号（block number）	     内容
----------------------	----------------------------------
log.start	                log header 块（结构：struct logheader）
log.start + 1	            数据块 0（事务中第一个被修改的块副本）
log.start + 2	            数据块 1（事务中第二个被修改的块副本）
...	...
log.start + N-1	          数据块 N-2

*/
static void
read_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *lh = (struct logheader *)(buf->data);
  int i;
  log.lh.n = lh->n;
  for (i = 0; i < log.lh.n; i++)
  {
    log.lh.block[i] = lh->block[i];
  }
  brelse(buf);
}

// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
static void
write_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *hb = (struct logheader *)(buf->data);
  int i;
  hb->n = log.lh.n;
  for (i = 0; i < log.lh.n; i++)
  {
    hb->block[i] = log.lh.block[i];
  }
  bwrite(buf);
  brelse(buf);
}

/*
recover_from_log()
│
├── read_head()       // 从 log.start 读取日志头，得到事务涉及哪些块
├── install_trans()   // 将事务内容从日志区拷贝到实际目标块
├── log.lh.n = 0      // 清空日志头的块计数
└── write_head()      // 把清空后的日志头写回磁盘，标志恢复完成
*/
static void
recover_from_log(void)
{
  read_head();
  install_trans(); // if committed, copy from log to disk
  log.lh.n = 0;
  write_head(); // clear the log
}

// called at the start of each FS system call.
void begin_op(void)
{
  acquire(&log.lock);
  while (1)
  {
    if (log.committing)
    {
      sleep(&log, &log.lock);
    }
    else if (log.lh.n + (log.outstanding + 1) * MAXOPBLOCKS > LOGSIZE)
    {
      // this op might exhaust log space; wait for commit.
      sleep(&log, &log.lock);
    }
    else
    {
      log.outstanding += 1;
      release(&log.lock);
      break;
    }
  }
}

// called at the end of each FS system call.
// commits if this was the last outstanding operation.
void end_op(void)
{
  int do_commit = 0;

  acquire(&log.lock);
  log.outstanding -= 1;
  if (log.committing)
    panic("log.committing");
  if (log.outstanding == 0)
  {
    do_commit = 1;
    log.committing = 1;
  }
  else
  {
    // begin_op() may be waiting for log space,
    // and decrementing log.outstanding has decreased
    // the amount of reserved space.
    wakeup(&log);
  }
  release(&log.lock);

  if (do_commit)
  {
    // call commit w/o holding locks, since not allowed
    // to sleep with locks.
    commit();
    acquire(&log.lock);
    log.committing = 0;
    wakeup(&log);
    release(&log.lock);
  }
}

// Copy modified blocks from cache to log.
static void
write_log(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++)
  {
    struct buf *to = bread(log.dev, log.start + tail + 1); // log block
    struct buf *from = bread(log.dev, log.lh.block[tail]); // cache block
    memmove(to->data, from->data, BSIZE);
    bwrite(to); // write the log
    brelse(from);
    brelse(to);
  }
}

static void
commit()
{
  if (log.lh.n > 0)
  {
    write_log();     // Write modified blocks from cache to log
    write_head();    // Write header to disk -- the real commit
    install_trans(); // Now install writes to home locations
    log.lh.n = 0;
    write_head(); // Erase the transaction from the log
  }
}

// Caller has modified b->data and is done with the buffer.
// Record the block number and pin in the cache with B_DIRTY.
// commit()/write_log() will do the disk write.
//
// log_write() replaces bwrite(); a typical use is:
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
// log_write() 并不立即将块写入磁盘，而是把这个块的编号记录到日志系统中，表示它将在本次事务 commit() 时被写入磁盘
/*
log_write(buf):
    assert 处于事务中
    如果该块未被记录到当前事务
        添加块号到 log.lh.block[]
        log.lh.n++
    标记该块为 dirty（防止缓存回收）
*/
void log_write(struct buf *b)
{
  int i;

  if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1)
    panic("too big a transaction");
  if (log.outstanding < 1)
    panic("log_write outside of trans");

  acquire(&log.lock);
  for (i = 0; i < log.lh.n; i++)
  {
    if (log.lh.block[i] == b->blockno) // log absorbtion
      break;
  }
  log.lh.block[i] = b->blockno;
  if (i == log.lh.n)
    log.lh.n++;
  b->flags |= B_DIRTY; // prevent eviction
  release(&log.lock);
}
