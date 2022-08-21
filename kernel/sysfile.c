//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf) {
  int fd;
  struct file *f;

  if (argint(n, &fd) < 0)
    return -1;
  if (fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0)
    return -1;
  if (pfd)
    *pfd = fd;
  if (pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
// 最小未使用的
static int
fdalloc(struct file *f) {
  int fd;
  struct proc *p = myproc();

  for (fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd] == 0) {
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void) {
  struct file *f;
  int fd;

  if (argfd(0, 0, &f) < 0)
    return -1;
  if ((fd = fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void) {
  struct file *f;
  int n;
  uint64 p;

  if (argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void) {
  struct file *f;
  int n;
  uint64 p;

  if (argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void) {
  int fd;
  struct file *f;

  if (argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void) {
  struct file *f;
  uint64 st; // user pointer to struct stat

  if (argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
// 向 new path 中写入一个和 old path 相同的目录项
uint64
sys_link(void) {
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if ((ip = namei(old)) == 0) {
    end_op();
    return -1;
  }

  ilock(ip);
  // 不能是一个目录
  if (ip->type == T_DIR) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);
  // new的父目录须存在且与现有inode位于同一设备:inode号在一个磁盘上有唯一的含义
  if ((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if (dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0) {
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

  bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp) {
  int off;
  struct dirent de;

  for (off = 2 * sizeof(de); off < dp->size; off += sizeof(de)) {
    if (readi(dp, 0, (uint64) &de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if (de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void) {
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if (argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  // dp -> 目标的父级目录
  if ((dp = nameiparent(path, name)) == 0) {
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if ((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if (ip->nlink < 1)
    panic("unlink: nlink < 1");
  // unlink对象是目录且目录不为空,失败
  if (ip->type == T_DIR && !isdirempty(ip)) {
    iunlockput(ip);
    goto bad;
  }
  // 将目录项位置清空
  memset(&de, 0, sizeof(de));
  if (writei(dp, 0, (uint64) &de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  // 如果unlink目标是一个目录, 其父目录nlink也要减小?
  if (ip->type == T_DIR) {
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

  bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode *
create(char *path, short type, short major, short minor) {
  struct inode *ip, *dp;
  char name[DIRSIZ];
  // 解析路径名并找到最后一个目录,之后会查看文件是否存在,如果存在的话会返回错误
  // 获取父目录的inode
  if ((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);
  // 检查名称是否已经存在
  if ((ip = dirlookup(dp, name, 0)) != 0) {
    iunlockput(dp);
    ilock(ip);
    // 代表 open(type == T_FILE)使用的 ??
    // 并且存在的名称本身是一个常规文件那么 open 会将其视为成功
    if (type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }
  // 如果名称不存在,分配inode
  if ((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  // 如果新inode是目录, create 将使用 . 和 .. 条目对它进行初始化
  if (type == T_DIR) {  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if (dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }
  // 数据已正确初始化,create 可以将其链接到父目录
  if (dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);
  // Create返回一个锁定的inode,但namei不锁定,因此sys_open必须锁定inode本身
  return ip;
}

// --- my code for lab9 start ---
/*
from: https://www.cnblogs.com/weijunji/p/xv6-study-14.html
实现符号链接,符号链接就是在文件中保存指向文件的路径名,
在打开文件的时候根据保存的路径名再去查找实际文件.
与符号链接相反的就是硬链接,硬链接是将文件的inode号指向目标文件的inode,并将引用计数加一
 */
uint64
sys_symlink(void) {
  char path[MAXPATH], target[MAXPATH];
  struct inode *ip;

  if (argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0) {
    return -1;
  }
  begin_op();
  // struct inode* create(char *path, short type, short major, short minor)
  // create 返回锁定的ip
  if ((ip = create(path, T_SYMLINK, 0, 0)) == 0) {
    end_op();
    return -1;
  }
  // writei 需要持有锁, create 返回的是持有锁的 ip
  // writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
  if (writei(ip, 0, (uint64) target, 0, MAXPATH) != MAXPATH) {
    panic("sys_symlink: writei");
  }
  iunlockput(ip); // iput必须在transaction中调用
  end_op();
  return 0;
}
// --- my code for lab9 end ---
uint64
sys_open(void) {
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if ((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;
  // 开始一个事务,这些写block操作要么全写入,要么全不写入
  begin_op();

  if (omode & O_CREATE) {
    ip = create(path, T_FILE, 0, 0);
    if (ip == 0) {
      // 在begin_op和end_op之间,磁盘上或者内存中的数据结构会更新
      // 但是在end_op之前,并不会有实际的改变(也就是不会写入到实际的block中).
      // 在end_op时,我们将数据写入到log中,再写入commit record或者log header
      // end_op中会实现commit操作
      end_op();
      return -1;
    }
  } else {
    if ((ip = namei(path)) == 0) {
      end_op();
      return -1;
    }
    ilock(ip);
    // --- my code for lab9 start ---
    if (ip->type == T_SYMLINK && (omode & O_NOFOLLOW) == 0) {
      int step = 0;
      char name[MAXPATH];
      while (ip->type == T_SYMLINK) {
        if (step > 10) {
          iunlockput(ip);
          end_op();
          return -1;
        }
        // int readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n) 需要持有锁
        if (readi(ip, 0, (uint64) name, 0, MAXPATH) != MAXPATH) {
          end_op();
          return -1;
        }
        iunlockput(ip); // namei 中需要持有锁
        if ((ip = namei(name)) == 0) {
          end_op();
          return -1;
        }
        ilock(ip); // namei 返回无锁 ip
        step++;
      }
    }
    // --- my code for lab9 end ---
    // 打开目录只能以读方式
    if (ip->type == T_DIR && omode != O_RDONLY) {
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if (ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0) {
    if (f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if (ip->type == T_DEVICE) {
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if ((omode & O_TRUNC) && ip->type == T_FILE) {
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void) {
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void) {
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  if ((argstr(0, path, MAXPATH)) < 0 ||
      argint(1, &major) < 0 ||
      argint(2, &minor) < 0 ||
      (ip = create(path, T_DEVICE, major, minor)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void) {
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();

  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);
  if (ip->type != T_DIR) {
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void) {
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if (argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for (i = 0;; i++) {
    if (i >= NELEM(argv)) {
      goto bad;
    }
    if (fetchaddr(uargv + sizeof(uint64) * i, (uint64 *) &uarg) < 0) {
      goto bad;
    }
    if (uarg == 0) {
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if (argv[i] == 0)
      goto bad;
    if (fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

  bad:
  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void) {
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if (argaddr(0, &fdarray) < 0)
    return -1;
  if (pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0) {
    if (fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if (copyout(p->pagetable, fdarray, (char *) &fd0, sizeof(fd0)) < 0 ||
      copyout(p->pagetable, fdarray + sizeof(fd0), (char *) &fd1, sizeof(fd1)) < 0) {
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}
