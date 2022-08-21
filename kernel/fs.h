// On-disk file system format.
// Both the kernel and user programs use this header file.


#define ROOTINO  1   // root i-number
#define BSIZE 1024  // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system.
// The super block describes the disk layout:
struct superblock {
  uint magic;        // Must be FSMAGIC
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
  uint nlog;         // Number of log blocks
  uint logstart;     // Block number of first log block
  uint inodestart;   // Block number of first inode block
  uint bmapstart;    // Block number of first free map block
};

#define FSMAGIC 0x10203040
// --- my code for lab9 start ---
//#define NDIRECT 12
#define NDIRECT 11
// --- my code for lab9 end ---
#define NINDIRECT (BSIZE / sizeof(uint))
// --- my code for lab9 start ---
#define NINDIRECT2 (NINDIRECT * NINDIRECT)
// --- my code for lab9 end ---
#define MAXFILE (NDIRECT + NINDIRECT + NINDIRECT2)

// On-disk inode structure
// 64 bytes
struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEVICE only)
  short minor;          // Minor device number (T_DEVICE only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  // --- my code for lab9 start ---
  // uint addrs[NDIRECT+1];   // Data block addresses
  uint addrs[NDIRECT+1+1];   // Data block addresses
  // --- my code for lab9 end ---
};

// Inodes per block.
#define IPB           (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB           (BSIZE*8)

// Block of free map containing bit for block b
// 给定一个块号和 super block 结构体变量,
// 就能返回在这个 super block 的描述下,
// 目标块号 b 是受块号为几的 bitmap block 管理的
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};

