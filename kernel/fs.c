// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

/*
--------------------------------------------------
|boot|super|log|...|inodes|...|bitmap|data|...|
  0    1    2        32          45    46
--------------------------------------------------
创建一个文件涉及到了多个操作:
1. 是分配inode,因为首先写的是block 33
2. inode被初始化,然后又写了一次block 33
3. 写block 46,是将文件x的inode编号写入到x所在目录的inode的data block中
4. 更新root inode,因为文件x创建在根目录,所以需要更新根目录的inode的size字段,
   以包含这里新创建的文件x,最后再次更新了文件x的inode

成功的创建了文件x,之后会调用write系统调用,write系统调用也执行了多个写磁盘的操作
1. 首先会从bitmap block,也就是block 45中,分配data block,
   通过从bitmap中分配一个bit,来表明一个data block已被分配
2. 上一步分配的data block是block 595,这里将字符“h”写入到block 595
   将字符“i”写入到block 595
3. 最后更新文件夹x的inode来更新size字段
*/

/*
[log write]当需要更新文件系统时,我们并不是更新文件系统本身
           假设我们在内存中缓存了bitmap block,也就是block 45
           当需要更新bitmap时,我们并不是直接写block 45,
           而是将数据写入到log中,并记录这个更新应该写入到block 45.
           对于所有的写 block都会有相同的操作,
           例如更新inode,也会记录一条写block 33的log

所以基本上,任何一次写操作都是先写入到log,我们并不是直接写入到block所在的位置,
而总是先将写操作写入到log中
[commit op]之后在某个时间,当文件系统的操作结束了,
           比如说我们前一节看到的4-5个写block操作都结束,
           并且都存在于log中,我们会commit文件系统的操作.
           这意味着我们需要在log的某个位置记录属于同一个文件系统的操作的个数,
           例如5
[install log]当我们在log中存储了所有写block的内容时,如果我们要真正执行这些操作,
             只需要将block从log分区移到文件系统分区.
             我们知道第一个操作该写入到block 45,
             我们会直接将数据从log写到block45,第二个操作该写入到block 33,
             我们会将它写入到block 33,依次类推
[clean log]一旦完成了,就可以清除log.
           清除log实际上就是将属于同一个文件系统的操作的个数设置为0
 */


#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb;

// Read the super block.
static void
readsb(int dev, struct superblock *sb) {
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Init fs
void
fsinit(int dev) {
  readsb(dev, &sb);
  if (sb.magic != FSMAGIC)
    panic("invalid file system");
  initlog(dev, &sb);
}

// Zero a block.
static void
bzero(int dev, int bno) {
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
static uint
balloc(uint dev) {
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  // 遍历所有的 bitmap block
  for (b = 0; b < sb.size; b += BPB) {
    // 找到管理当前块(b)的bitmap块号
    bp = bread(dev, BBLOCK(b, sb));
    // 遍历当前 bitmap block 的所有位来找出一个空闲块
    for (bi = 0; bi < BPB && b + bi < sb.size; bi++) {
      m = 1 << (bi % 8);
      // bp->data[bi/8] is a byte
      if ((bp->data[bi / 8] & m) == 0) {  // Is block free?
        bp->data[bi / 8] |= m;  // Mark block in use.
        log_write(bp);
        brelse(bp);
        // assume b = 0 may be understanding
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  panic("balloc: out of blocks");
}

// Free a disk block.
static void
bfree(int dev, uint b) {
  struct buf *bp;
  int bi, m;
  // 找到对应bitmap块
  bp = bread(dev, BBLOCK(b, sb));
  //
  bi = b % BPB;
  m = 1 << (bi % 8);
  // bp->data[bi/8]是块b所在的直接, m是块b在这个字节中的位置
  if ((bp->data[bi / 8] & m) == 0)
    panic("freeing free block");
  // 该位标记为0
  bp->data[bi / 8] &= ~m;
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at
// sb.startinode. Each inode has a number, indicating its
// position on the disk.
//
// *** The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes.***
// The cached inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   *** the reference and link counts have fallen to zero. ***
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a cache entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The icache.lock spin-lock protects the allocation of icache
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold icache.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
    // icache.lock 保护以下两个不变量:
    // 1. inode最多在缓存中出现一次;
    // 2. 缓存inode的 ref 字段记录指向缓存inode的内存指针数量
    struct spinlock lock;
    // node缓存只缓存内核代码或数据结构持有C指针的inode
    // 它的主要工作实际上是同步多个进程的访问;缓存是次要的
    struct inode inode[NINODE];
} icache;

void
iinit() {
  int i = 0;

  initlock(&icache.lock, "icache");
  for (i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }
}

static struct inode *iget(uint dev, uint inum);

// Allocate an inode on device dev.
// *** Mark it as allocated by  giving it type type. ***
// Returns an unlocked but allocated and referenced inode.
// 返回一个未锁定但已分配(type)并引用的inode
struct inode *
ialloc(uint dev, short type) {
  int inum;
  struct buf *bp;
  struct dinode *dip;
  // ialloc 的正确操作取决于这样一个事实:
  // 一次只有一个进程可以保存对 bp 的引用:
  // ialloc 可以确保其他进程不会同时看到inode可用并尝试声明它
  for (inum = 1; inum < sb.ninodes; inum++) {
    // IBLOCK(inum, sb)得到包含编号为inum的inode的块编号
    bp = bread(dev, IBLOCK(inum, sb));
    dip = (struct dinode *) bp->data + inum % IPB;
    if (dip->type == 0) {  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      // log_write就是我们之前看到在console中有关写block的输出 ???
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      // 从inode缓存返回一个条目
      return iget(dev, inum);
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk, since i-node cache is write-through.
// Caller must hold ip->lock.
void
iupdate(struct inode *ip) {
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode *) bp->data + ip->inum % IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  // inode缓存是直写的,意味着修改已缓存inode的代码
  // 必须立即使用 iupdate 将其写入磁盘 ???????
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode *
iget(uint dev, uint inum) {
  struct inode *ip, *empty;
  // icache.lock 保护以下两个不变量:
  // 1. inode最多在缓存中出现一次;
  // 2. 缓存inode的 ref 字段记录指向缓存inode的内存指针数量
  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for (ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++) {
    if (ip->ref > 0 && ip->dev == dev && ip->inum == inum) {
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if (empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if (empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode *
idup(struct inode *ip) {
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
// 在读取或写入inode的元数据或内容之前,代码必须使用 ilock 锁定inode
// 可能会从磁盘(更可能是buffer cache)中读 inode
void
ilock(struct inode *ip) {
  struct buf *bp;
  struct dinode *dip;

  if (ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if (ip->valid == 0) {
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode *) bp->data + ip->inum % IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if (ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip) {
  if (ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
//
// 通过减少引用计数释放指向inode的C指针
// 如果这是最后一次引用,那么 inode cache 会被回收
// 如果是最后一次引用且 links 也等于0, 那么磁盘上的inode也会被free
// 必须在transaction中调用 iput()(因为要释放inode)
void
iput(struct inode *ip) {
  acquire(&icache.lock);
  // 没有指向inode的C指针引用,并且inode没有指向它的链接(发生于无目录)
  // 如果是最后一次引用且 links 也等于0, 那么磁盘上的inode也会被free
  if (ip->ref == 1 && ip->valid && ip->nlink == 0) {
    // inode has no links and no other references: truncate and free.

    // ip->ref == 1 means no other process can have ip locked,
    // so this acquiresleep() won't block (or deadlock).
    acquiresleep(&ip->lock);

    release(&icache.lock);
    // 将文件截断为零字节,释放数据块(12+1)
    itrunc(ip);
    // 设置为未分配
    ip->type = 0;
    // 写入磁盘
    iupdate(ip);
    ip->valid = 0;

    releasesleep(&ip->lock);

    acquire(&icache.lock);
  }

  ip->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip) {
  iunlock(ip);
  iput(ip);
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
static uint
bmap(struct inode *ip, uint bn) {
  uint addr, *a;
  struct buf *bp;

  if (bn < NDIRECT) {
    if ((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev);
    return addr;
  }
  bn -= NDIRECT;

  if (bn < NINDIRECT) {
    // Load indirect block, allocating if necessary.
    if ((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint *) bp->data;
    if ((addr = a[bn]) == 0) {
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }
  // --- my code for lab9 start ---
  bn -= NINDIRECT;

  if (bn < NINDIRECT2) {
    int p = bn / NINDIRECT;
    int off = bn % NINDIRECT;
    if (ip->addrs[NDIRECT + 1] == 0) {
      ip->addrs[NDIRECT + 1] = balloc(ip->dev);
    }
    bp = bread(ip->dev, ip->addrs[NDIRECT + 1]);
    // bp->data 是 uchar类型,这里必须转换一下
    a = (uint*)bp->data;
    if (a[p] == 0) {
      a[p] = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);

    bp = bread(ip->dev, a[p]);
    a = (uint*)bp->data;
    if ((addr = a[off]) == 0) {
      a[off] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }
  // --- my code for lab9 end ---

  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Caller must hold ip->lock.
// 1. size 置0
// 2. 释放所有数据块(12+256+1)
void
itrunc(struct inode *ip) {
  int i, j;
  struct buf *bp;
  uint *a, *b;

  for (i = 0; i < NDIRECT; i++) {
    if (ip->addrs[i]) {
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if (ip->addrs[NDIRECT]) {
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint *) bp->data;
    for (j = 0; j < NINDIRECT; j++) {
      if (a[j])
        bfree(ip->dev, a[j]);
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }
  // --- my code for lab9 start ---
  if (ip->addrs[NDIRECT + 1]) {
    bp = bread(ip->dev, ip->addrs[NDIRECT + 1]);
    a = (uint *) bp->data;
    struct buf *bp2;
    for (j = 0; j < NINDIRECT; j++) {
      if (a[j]) {
        bp2 = bread(ip->dev, a[j]);
        b = (uint *) bp2->data;
        for (int k = 0; k < NINDIRECT; k++) {
          if (b[k]) {
            bfree(ip->dev, b[k]);
          }
        }
        brelse(bp2);
        bfree(ip->dev, a[j]);
      }
    }
    bfree(ip->dev, ip->addrs[NDIRECT + 1]);
    brelse(bp);
    ip->addrs[NDIRECT + 1] = 0;
  }
  // --- my code for lab9 end ---
  ip->size = 0;
  iupdate(ip);
}

// Copy stat information from inode.
// Caller must hold ip->lock.
// 将inode元数据复制到 stat 结构体中,该结构通过 stat 系统调用向用户程序公开
void
stati(struct inode *ip, struct stat *st) {
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

// Read data from inode.
// Caller must hold ip->lock.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
// 1. 需要持有睡眠锁
// 2. user_dst=1, 用户虚拟地址, 否则未内核地址
int
readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n) {
  uint tot, m;
  struct buf *bp;
  // 确保偏移量和计数不超过文件的末尾
  if (off > ip->size || off + n < off)
    return 0;
  // 从文件末尾开始或穿过文件末尾的读取返回的字节数少于请求的字节数
  if (off + n > ip->size)
    n = ip->size - off;

  for (tot = 0; tot < n; tot += m, off += m, dst += m) {
    bp = bread(ip->dev, bmap(ip, off / BSIZE));
    m = min(n - tot, BSIZE - off % BSIZE);
    if (either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
      brelse(bp);
      tot = -1;
      break;
    }
    brelse(bp);
  }
  return tot;
}

// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address;
// otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n,
// there was an error of some kind.
//
// 向 inode 中写入数据
// 调用者需要持有 ip->lock
// 返回写入成功的字节数
int
writei(struct inode *ip, int user_src, uint64 src, uint off, uint n) {
  uint tot, m;
  struct buf *bp;

  if (off > ip->size || off + n < off)
    return -1;
  if (off + n > MAXFILE * BSIZE)
    return -1;

  for (tot = 0; tot < n; tot += m, off += m, src += m) {
    bp = bread(ip->dev, bmap(ip, off / BSIZE));
    m = min(n - tot, BSIZE - off % BSIZE);
    if (either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1) {
      brelse(bp);
      break;
    }
    log_write(bp);
    brelse(bp);
  }

  if (off > ip->size)
    ip->size = off;

  // write the i-node back to disk even if the size didn't change
  // because the loop above might have called bmap() and added a new
  // block to ip->addrs[].
  iupdate(ip);

  return tot;
}

// Directories

int
namecmp(const char *s, const char *t) {
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode *
dirlookup(struct inode *dp, char *name, uint *poff) {
  uint off, inum;
  struct dirent de;

  if (dp->type != T_DIR)
    panic("dirlookup not DIR");
  // 在目录中搜索具有给定名称的条目
  for (off = 0; off < dp->size; off += sizeof(de)) {
    if (readi(dp, 0, (uint64) &de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if (de.inum == 0)
      continue;
    if (namecmp(name, de.name) == 0) {
      // entry matches path element
      // 将*poff设置为目录中条目的字节偏移量,满足调用方希望对其进行编辑的情形
      if (poff)
        *poff = off;
      inum = de.inum;
      /* 返回通过 iget 获得的**未锁定**的inode
       调用者已锁定 dp,因此,如果对当前目录(.)进行查找
       则在返回之前尝试锁定indoe将导致重新锁定 dp 并产生死锁
       (还有更复杂的死锁场景,涉及多个进程和父目录..)
       调用者可以解锁 dp,然后锁定 ip,确保它一次只持有一个锁
       * */
      // 返回一个指向相应inode的指针
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
int
dirlink(struct inode *dp, char *name, uint inum) {
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if ((ip = dirlookup(dp, name, 0)) != 0) {
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for (off = 0; off < dp->size; off += sizeof(de)) {
    if (readi(dp, 0, (uint64) &de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if (de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  // writei 中会调用 bmap 自动分配空间
  if (writei(dp, 0, (uint64) &de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");

  return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char *
skipelem(char *path, char *name) {
  char *s;
  int len;

  while (*path == '/')
    path++;
  if (*path == 0)
    return 0;
  s = path;
  while (*path != '/' && *path != 0)
    path++;
  len = path - s;
  if (len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while (*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode *
namex(char *path, int nameiparent, char *name) {
  struct inode *ip, *next;

  if (*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd); // 从当前目录开始

  while ((path = skipelem(path, name)) != 0) {
    /*
     锁定 ip 是必要的,不是因为 ip->type 可以被更改,
     而是因为在 ilock 运行之前, ip->type 不能保证已从磁盘加载

     Namex 分别锁定路径中的每个目录,以便在不同目录中进行并行查找
     */
    ilock(ip);
    if (ip->type != T_DIR) {
      iunlockput(ip);
      return 0;
    }
    if (nameiparent && *path == '\0') {
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    /*
     challenge: 当一个内核线程正在查找路径名时,另一个内核线程可能
     正在通过取消目录链接来更改目录树.一个潜在的风险是,
     查找可能正在搜索已被另一个内核线程删除且其块已被重新用于另一个目录或文件的目录

     explain: 在 namex 中执行 dirlookup 时,lookup线程持有目录上的锁,
     dirlookup 返回使用 iget 获得的inode. Iget 增加索引节点的引用计数.
     只有在从 dirlookup 接收inode之后, namex 才会释放目录上的锁.
     现在,另一个线程可以从目录中取消inode的链接,但是xv6还不会删除inode,
     因为inode的引用计数仍然大于零
     */
    if ((next = dirlookup(ip, name, 0)) == 0) {
      iunlockput(ip);
      return 0;
    }
    /*
     查找“ . ”时, next 指向与 ip 相同的inode.
     在释放 ip 上的锁之前锁定 next 将导致死锁.
     为了避免这种死锁， namex 在获得下一个目录的锁之前解锁该目录.
     这里我们再次看到为什么 iget 和 ilock 之间的分离很重要
     */
    iunlockput(ip);
    ip = next;
  }
  if (nameiparent) {
    iput(ip);
    return 0;
  }
  return ip;
}

// 计算 path 并返回相应的inode
struct inode *
namei(char *path) {
  char name[DIRSIZ];
  return namex(path, 0, name);
}

// 在最后一个元素之前停止,返回父目录的inode并将最后一个元素复制到 name 中
struct inode *
nameiparent(char *path, char *name) {
  return namex(path, 1, name);
}
