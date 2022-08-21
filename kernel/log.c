#include "types.h"
#include "riscv.h"
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
//
// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block# before commit.
struct logheader {
  int n;
  int block[LOGSIZE];
};

struct log {
  struct spinlock lock;
  int start;
  int size;
  int outstanding; // how many FS sys calls are executing.
  int committing;  // in commit(), please wait.
  int dev;
  struct logheader lh;
};
struct log log;

static void recover_from_log(void);
static void commit();

void
initlog(int dev, struct superblock *sb)
{
  if (sizeof(struct logheader) >= BSIZE)
    panic("initlog: too big logheader");

  initlock(&log.lock, "log");
  log.start = sb->logstart;
  log.size = sb->nlog;
  log.dev = dev;
  recover_from_log();
}

// Copy committed blocks from log to their home location
static void
install_trans(int recovering)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    struct buf *lbuf = bread(log.dev, log.start+tail+1); // read log block
    struct buf *dbuf = bread(log.dev, log.lh.block[tail]); // read dst
    memmove(dbuf->data, lbuf->data, BSIZE);  // copy block to dst
    // 在这里代码的某个位置会出现问题,但是这应该也没问题,
    // 因为在恢复的时候,我们会从最开始重新执行过
    bwrite(dbuf);  // write dst to disk
    if(recovering == 0)
      bunpin(dbuf);
    brelse(lbuf);
    brelse(dbuf);
  }
}

// Read the log header from disk into the in-memory log header
static void
read_head(void)
{
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *lh = (struct logheader *) (buf->data);
  int i;
  log.lh.n = lh->n;
  for (i = 0; i < log.lh.n; i++) {
    log.lh.block[i] = lh->block[i];
  }
  brelse(buf);
}

// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
// write_head称为commit point
static void
write_head(void)
{
  // 先读取log的header block
  struct buf *buf = bread(log.dev, log.start);
  struct logheader *hb = (struct logheader *) (buf->data);
  int i;
  hb->n = log.lh.n;
  for (i = 0; i < log.lh.n; i++) {
    hb->block[i] = log.lh.block[i];
  }
  // 将header block写回到磁盘(log区)

  // 那crash发生在bwrite之后会发生什么呢?
  // 这时header会写入到磁盘中,当重启恢复相应的文件系统操作会被恢复.
  // 在恢复过程中,恢复程序可以读到log header并发现比如说有5个log还没有install,
  // 恢复程序可以将这5个log拷贝到实际的位置

  // 这里的bwrite就是实际的commit point.
  // 在commit point之前,transaction并没有发生,
  // 在commit point之后,只要恢复程序正确运行,transaction必然可以完成
  bwrite(buf);
  brelse(buf);
}

static void
recover_from_log(void)
{
  read_head();
  // 如果我们在install_trans函数中又crash了,也不会有问题,
  // 再重启时,XV6再调用initlog,再调用recover_from_log来重新install log
  install_trans(1); // if committed, copy from log to disk
  log.lh.n = 0;
  write_head(); // clear the log
}

// called at the start of each FS system call.
void
begin_op(void)
{
  acquire(&log.lock);
  while(1){
    if(log.committing){
      sleep(&log, &log.lock);
      // log.outstanding 预定了日志空间的系统调用数
      // log.lh.n ?
    } else if(log.lh.n + (log.outstanding+1)*MAXOPBLOCKS > LOGSIZE){
      // this op might exhaust log space; wait for commit.
      sleep(&log, &log.lock);
    } else {
      log.outstanding += 1;
      release(&log.lock);
      break;
    }
  }
}

// called at the end of each FS system call.
// commits if this was the last outstanding operation.
void
end_op(void)
{
  int do_commit = 0;
  acquire(&log.lock);
  // 一个transaction正在结束
  log.outstanding -= 1;
  if(log.committing)
    panic("log.committing");
  // 当前操作是整个并发操作的最后一个的话
  if(log.outstanding == 0){
    do_commit = 1;
    log.committing = 1;
  } else {
    // begin_op() may be waiting for log space,
    // and decrementing log.outstanding has decreased
    // the amount of reserved space.
    // 不是整个并发操作的最后一个的话,我们需要唤醒在begin_op中sleep的操作
    // 让它们检查是不是能运行
    wakeup(&log);
  }
  release(&log.lock);

  if(do_commit){
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

  for (tail = 0; tail < log.lh.n; tail++) {
    // 首先读出log block
    struct buf *to = bread(log.dev, log.start+tail+1); // log block
    struct buf *from = bread(log.dev, log.lh.block[tail]); // cache block
    // 将cache中的block拷贝到log block
    memmove(to->data, from->data, BSIZE);
    // 最后再将log block写回到磁盘中
    bwrite(to);  // write the log
    brelse(from);
    brelse(to);
    // 在这个位置,我们还没有commit,现在我们只是将block存放在了log中
    // 如果我们在这个位置也就是在write_head之前crash了,
    // 那么最终的表现就像是transaction从来没有发生过
  }
}

static void
commit()
{
  if (log.lh.n > 0) {
    // 将所有存在于内存中的log header中的block编号对应的block,
    // 从block cache写入到磁盘上的log区域中(也就是将变化先从内存拷贝到log中)
    write_log();     // Write modified blocks from cache to log
    // 将内存中的log header写入到磁盘中
    write_head();    // Write header to disk -- the real commit
    // 实际应用transaction
    install_trans(0); // Now install writes to home locations
    log.lh.n = 0;
    write_head();    // Erase the transaction from the log
  }
}

// Caller has modified b->data and is done with the buffer.
// 调用者已经在缓冲区中写入数据
// Record the block number and pin in the cache by increasing refcnt.
// 记录块号并增加块在cache中的引用
// commit()/write_log() will do the disk write.
//
// log_write() replaces bwrite(); a typical use is:
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
// 任何一个文件系统调用的begin_op和end_op之间的写操作总是会走到log_write
void
log_write(struct buf *b)
{
  int i;

  if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1)
    panic("too big a transaction");
  if (log.outstanding < 1)
    panic("log_write outside of trans");

  acquire(&log.lock);
  for (i = 0; i < log.lh.n; i++) {
    if (log.lh.block[i] == b->blockno)   // log absorbtion
      break;
  }
  log.lh.block[i] = b->blockno;
  if (i == log.lh.n) {  // Add new block to log?
    bpin(b);
    log.lh.n++;
  }
  release(&log.lock);
}

