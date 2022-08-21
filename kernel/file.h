// inode或管道的封装,加上一个I/O偏移量
// 如果多个进程独立地打开同一个文件,那么不同的实例将具有不同的I/O偏移量
struct file {
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE } type;
  // 单个打开的文件（同一个 struct file ）可以多次出现在一个进程的文件表中,
  // 也可以出现在多个进程的文件表中。如果一个进程使用 open 打开文件,
  // 然后使用 dup 创建别名,或使用 fork 与子进程共享,就会发生这种情况
  // 引用计数跟踪对特定打开文件的引用数
  int ref; // reference count
  char readable;
  char writable;
  struct pipe *pipe; // FD_PIPE
  struct inode *ip;  // FD_INODE and FD_DEVICE
  uint off;          // FD_INODE
  short major;       // FD_DEVICE
};

#define major(dev)  ((dev) >> 16 & 0xFFFF)
#define minor(dev)  ((dev) & 0xFFFF)
#define	mkdev(m,n)  ((uint)((m)<<16| (n)))

// in-memory copy of an inode
struct inode {
  uint dev;           // Device number
  uint inum;          // Inode number
  int ref;            // Reference count
  struct sleeplock lock; // protects everything below here
  int valid;          // inode has been read from disk?

  // copy of disk inode
  short type;
  short major;
  short minor;
  short nlink;
  uint size;
  // --- my code for lab9 start ---
  // uint addrs[NDIRECT+1];
  uint addrs[NDIRECT+1+1];
  // --- my code for lab9 end ---
  // copy of disk inode
};

// map major device number to device functions.
struct devsw {
  int (*read)(int, uint64, int);
  int (*write)(int, uint64, int);
};

extern struct devsw devsw[];

#define CONSOLE 1
