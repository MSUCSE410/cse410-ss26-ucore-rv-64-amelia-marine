#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
	return 0;
}

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_spawn(uint64 va)
{
	// TODO: your job is to complete the sys call
	return -1;
}

uint64 sys_set_priority(long long prio)
{
	// TODO: your job is to complete the sys call
	return -1;
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}

int sys_fstat(int fd, uint64 stat_addr)
{
    // Reject out-of-range file descriptors
    if (fd < 0 || fd >= FD_BUFFER_SIZE)
        return -1;

    // Get the current process
    struct proc *p = curr_proc();

    // Look up the file in the process's file table
    struct file *f = p->files[fd];

    // fd must refer to an inode-backed file (not stdio)
    if (f == NULL || f->type != FD_INODE)
        return -1;

    // Make sure the inode is loaded from disk into memory
    ivalid(f->ip);

    // Define the Stat struct layout expected by userspace
    typedef struct {
        uint64 dev;     // disk drive number (always 0 in our impl)
        uint64 ino;     // inode number uniquely identifying the file
        uint32 mode;    // file type (directory or regular file)
        uint32 nlink;   // number of hard links pointing to this inode
        uint64 pad[7];  // padding for compatibility, unused
    } Stat;

    // Fill in the stat struct with info from the inode
    Stat st;
    st.dev = 0;                                                      // we only have one disk
    st.ino = f->ip->inum;                                            // inode number from in-memory inode
    st.mode = (f->ip->type == T_DIR) ? 0x040000 : 0x100000;         // directory vs regular file
    st.nlink = f->ip->nlink;                                         // hard link count
    memset(st.pad, 0, sizeof(st.pad));                               // zero out padding

    // Copy the filled stat struct from kernel space to user space
    copyout(p->pagetable, stat_addr, (char *)&st, sizeof(st));

    return 0;
}

int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath, uint64 flags)
{
    struct proc *p = curr_proc();
    char old[200], new[200];

    // Copy both file path strings from user space into kernel buffers
    copyinstr(p->pagetable, old, oldpath, 200);
    copyinstr(p->pagetable, new, newpath, 200);

    // Linking a file to itself (same name) is an error
    if (strncmp(old, new, 200) == 0)
        return -1;

    // Look up the inode for the existing file
    struct inode *ip = namei(old);

    // If the source file doesn't exist, fail
    if (ip == NULL)
        return -1;

    // Ensure the inode data is loaded from disk
    ivalid(ip);

    // Get the root directory inode (our FS is flat, one directory)
    struct inode *dp = root_dir();
    ivalid(dp);

    // Add a new directory entry mapping newpath -> same inode number
    // This is what creates the hard link
    if (dirlink(dp, new, ip->inum) < 0) {
        // dirlink failed (e.g. name already exists), clean up and return error
        iput(dp);
        iput(ip);
        return -1;
    }

    // Increment the link count since a new directory entry now points to this inode
    ip->nlink++;

    // Write the updated inode (with new nlink) back to disk
    iupdate(ip);

    // Release our reference to the root directory inode
    iput(dp);

    // Release our reference to the file inode
    iput(ip);

    return 0;
}

int sys_unlinkat(int dirfd, uint64 name, uint64 flags)
{
    struct proc *p = curr_proc();
    char path[200];

    // Copy the file path from user space into a kernel buffer
    copyinstr(p->pagetable, path, name, 200);

    // Look up the inode for the file to be unlinked
    struct inode *ip = namei(path);

    // If the file doesn't exist, return error
    if (ip == NULL)
        return -1;

    // Ensure the inode is loaded from disk into memory
    ivalid(ip);

    // Get the root directory inode (flat FS, all files are in root)
    struct inode *dp = root_dir();
    ivalid(dp);

    // Remove the directory entry that maps this name to the inode
    // This does not delete the file if other hard links still exist
    if (dirunlink(dp, path) < 0) {
        // Failed to find or remove the directory entry
        iput(dp);
        iput(ip);
        return -1;
    }

    // Release our reference to the root directory inode
    iput(dp);

    // Decrement the hard link count since one directory entry was removed
    ip->nlink--;

    // Write the updated link count back to disk
    iupdate(ip);

    // If no more hard links remain, free all data blocks (delete the file)
    if (ip->nlink == 0)
        itrunc(ip);

    // Release our reference to the inode
    // iput will mark it invalid and free it if nlink == 0
    iput(ip);

    return 0;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_fstat:
	    ret = sys_fstat(args[0],args[1]);
		break;
	case SYS_linkat:
	    ret = sys_linkat(args[0],args[1],args[2],args[3],args[4]);
		break;
	case SYS_unlinkat:
	    ret = sys_unlinkat(args[0],args[1],args[2]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
