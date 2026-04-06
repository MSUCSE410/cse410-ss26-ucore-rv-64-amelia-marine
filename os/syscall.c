#include "console.h"
#include "defs.h"
#include "trap.h" 
#include "loader.h"
#include "sync.h"
#include "syscall.h"
#include "syscall_ids.h"
#include "timer.h"

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
	case FD_PIPE:
		return pipewrite(f->pipe, va, len);
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
	case FD_PIPE:
		return piperead(f->pipe, va, len);
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

uint64 sys_pipe(uint64 fdarray)
{
	struct proc *p = curr_proc();
	uint64 fd0, fd1;
	struct file *f0, *f1;
	if (f0 < 0 || f1 < 0) {
		return -1;
	}
	f0 = filealloc();
	f1 = filealloc();
	if (pipealloc(f0, f1) < 0)
		goto err0;
	fd0 = fdalloc(f0);
	fd1 = fdalloc(f1);
	if (fd0 < 0 || fd1 < 0)
		goto err0;
	if (copyout(p->pagetable, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
	    copyout(p->pagetable, fdarray + sizeof(uint64), (char *)&fd1,
		    sizeof(fd1)) < 0) {
		goto err1;
	}
	return 0;

err1:
	p->files[fd0] = 0;
	p->files[fd1] = 0;
err0:
	fileclose(f0);
	fileclose(f1);
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

int sys_thread_create(uint64 entry, uint64 arg)
{
	struct proc *p = curr_proc();
	int tid = allocthread(p, entry, 1);
	if (tid < 0) {
		errorf("fail to create thread");
		return -1;
	}
	struct thread *t = &p->threads[tid];
	t->trapframe->a0 = arg;
	t->state = RUNNABLE;
	add_task(t);
	return tid;
}

int sys_gettid()
{
	return curr_thread()->tid;
}

int sys_waittid(int tid)
{
	if (tid < 0 || tid >= NTHREAD) {
		errorf("unexpected tid %d", tid);
		return -1;
	}
	struct thread *t = &curr_proc()->threads[tid];
	if (t->state == T_UNUSED || tid == curr_thread()->tid) {
		return -1;
	}
	if (t->state != EXITED) {
		return -2;
	}
	memset((void *)t->kstack, 7, KSTACK_SIZE);
	t->tid = -1;
	t->state = T_UNUSED;
	return t->exit_code;
}

// Banker's algorithm deadlock detection.
// Returns 1 if deadlock detected, 0 if safe.
int deadlock_detect(
    const int available[LOCK_POOL_SIZE],           // current available count per resource
    const int allocation[NTHREAD][LOCK_POOL_SIZE], // how much each thread currently holds
    const int request[NTHREAD][LOCK_POOL_SIZE]     // how much each thread is requesting
) {
    int work[LOCK_POOL_SIZE];  // simulated available resources as threads finish
    int finish[NTHREAD] = {0}; // tracks whether each thread can finish; 0 = not yet

    // Step 1: initialize work to match currently available resources
    for (int i = 0; i < LOCK_POOL_SIZE; i++) {
        work[i] = available[i]; // work starts as a copy of available
    }

    // Step 2: repeatedly find threads whose requests can be satisfied
    int changed = 1;          // flag to keep looping as long as progress is made
    while (changed) {
        changed = 0;          // assume no progress this iteration

        for (int t = 0; t < NTHREAD; t++) {
            if (finish[t]) continue; // skip threads already marked as finished

            int can_run = 1;         // assume this thread can run until proven otherwise

            for (int r = 0; r < LOCK_POOL_SIZE; r++) {
                if (request[t][r] > work[r]) { // thread needs more of resource r than available
                    can_run = 0;               // can't satisfy this thread's request
                    break;                     // no need to check remaining resources
                }
            }

            if (can_run) {
                // simulate this thread finishing and releasing all its held resources
                for (int r = 0; r < LOCK_POOL_SIZE; r++) {
                    work[r] += allocation[t][r]; // reclaim what this thread was holding
                }
                finish[t] = 1; // mark thread as finished
                changed = 1;   // made progress, so loop again to find more runnable threads
            }
        }
    }

    // Step 3: if any thread couldn't finish, we have a deadlock
    for (int t = 0; t < NTHREAD; t++) {
        if (!finish[t]) return 1; // this thread is stuck — deadlock detected
    }

    return 0; // all threads can finish — safe state
}

// Creates a new mutex and initializes its resource tracking.
// blocking: whether the mutex should block (1) or spin (0) when contended.
// Returns the mutex ID on success, -1 on failure.
int sys_mutex_create(int blocking)
{
    struct mutex *m = mutex_create(blocking); // allocate a mutex from the pool
    if (m == NULL) {
        errorf("fail to create mutex: out of resource");
        return -1; // no free mutex slots
    }
    int mutex_id = m - curr_proc()->mutex_pool; // compute index into mutex pool
    curr_proc()->available[mutex_id] = 1;       // mutex starts with 1 available unit (binary)
    debugf("create mutex %d", mutex_id);
    return mutex_id;
}

// Attempts to lock a mutex, with optional deadlock detection.
// Returns 0 on success, -0xdead if deadlock detected, -1 on invalid input.
int sys_mutex_lock(int mutex_id)
{
    if (mutex_id < 0 || mutex_id >= curr_proc()->next_mutex_id) {
        errorf("Unexpected mutex id %d", mutex_id);
        return -1; // mutex_id out of valid range
    }

    struct proc *p = curr_proc();
    int tid = curr_thread()->tid;

    // Only run deadlock detection if enabled and mutex is already held by someone
    if (p->deadlock_detect_enabled && p->available[mutex_id] == 0) {
        p->request[tid][mutex_id] = 1; // record that this thread is requesting the mutex

        if (deadlock_detect(p->available, p->allocation, p->request)) {
            p->request[tid][mutex_id] = 0;                    // undo the request — we're not proceeding
            errorf("detect deadlock on locking mutex %d!", mutex_id);
            return -0xdead; // signal deadlock to the caller
        }

        p->request[tid][mutex_id] = 0; // safe to proceed — clear the request record
    }

    mutex_lock(&curr_proc()->mutex_pool[mutex_id]); // perform the actual lock (may block)

    p->allocation[tid][mutex_id] = 1; // record that this thread now holds the mutex
    p->available[mutex_id]--;         // decrement available count (0 = mutex is held)

    return 0;
}

// Unlocks a mutex and updates resource tracking.
// Returns 0 on success, -1 on invalid input.
int sys_mutex_unlock(int mutex_id)
{
    if (mutex_id < 0 || mutex_id >= curr_proc()->next_mutex_id) {
        errorf("Unexpected mutex id %d", mutex_id);
        return -1; // mutex_id out of valid range
    }

    struct proc *p = curr_proc();
    int tid = curr_thread()->tid;

    p->allocation[tid][mutex_id] = 0; // record that this thread no longer holds the mutex
    p->available[mutex_id]++;         // return the resource unit to the available pool

    mutex_unlock(&curr_proc()->mutex_pool[mutex_id]); // perform the actual unlock

    return 0;
}

int sys_semaphore_create(int res_count)
{
	struct semaphore *s = semaphore_create(res_count);
	if (s == NULL) {
		errorf("fail to create semaphore: out of resource");
		return -1;
	}
	// LAB5: (4-2) You may want to maintain some variables for detect here
	int sem_id = s - curr_proc()->semaphore_pool;
	curr_proc()->available[sem_id] = res_count;
	debugf("create semaphore %d", sem_id);
	return sem_id;
}

// Signals (increments) a semaphore, releasing one resource unit.
// Returns 0 on success, -1 on invalid input.
int sys_semaphore_up(int semaphore_id)
{
    if (semaphore_id < 0 ||
        semaphore_id >= curr_proc()->next_semaphore_id) {
        errorf("Unexpected semaphore id %d", semaphore_id);
        return -1; // semaphore_id out of valid range
    }

    struct proc *p = curr_proc();
    int tid = curr_thread()->tid;

    p->allocation[tid][semaphore_id]--; // this thread is releasing one unit it was holding
    p->available[semaphore_id]++;       // return that unit to the available pool

    semaphore_up(&curr_proc()->semaphore_pool[semaphore_id]); // perform the actual signal

    return 0;
}

// Waits on (decrements) a semaphore, acquiring one resource unit.
// Runs deadlock detection before blocking if detection is enabled.
// Returns 0 on success, -1 if deadlock detected or invalid input.
int sys_semaphore_down(int semaphore_id)
{
    if (semaphore_id < 0 ||
        semaphore_id >= curr_proc()->next_semaphore_id) {
        errorf("Unexpected semaphore id %d", semaphore_id);
        return -1; // semaphore_id out of valid range
    }

    struct proc *p = curr_proc();
    int tid = curr_thread()->tid;

    // record that this thread is now requesting one unit of this semaphore
    p->request[tid][semaphore_id]++;

    int simulated = 0; // tracks whether we performed a trial allocation for detection

    // tentatively simulate granting the resource, but only if one is currently available.
    if (p->available[semaphore_id] > 0) {
        p->allocation[tid][semaphore_id]++; // pretend this thread got the resource
        p->available[semaphore_id]--;       // pretend one unit was consumed
        simulated = 1;                      // remember we need to undo this later
    }

    if (p->deadlock_detect_enabled &&
        deadlock_detect(p->available, p->allocation, p->request)) {

        // deadlock would occur — undo the simulation before returning
        if (simulated) {
            p->allocation[tid][semaphore_id]--; // undo trial allocation
            p->available[semaphore_id]++;       // restore available count
        }

        p->request[tid][semaphore_id]--; // cancel the request since we're not proceeding

        errorf("detect deadlock on down semaphore %d!", semaphore_id);
        return -1; // report deadlock to caller
    }

    // safe to proceed — undo the simulation so the real operation starts from true state
    if (simulated) {
        p->allocation[tid][semaphore_id]--; // undo trial allocation
        p->available[semaphore_id]++;       // restore available count
    }

    // perform the real semaphore wait — this may block if no units are available
    semaphore_down(&curr_proc()->semaphore_pool[semaphore_id]);

    // semaphore_down returned, meaning this thread successfully acquired a unit —
    // update tracking to reflect the real post-acquisition state
    p->request[tid][semaphore_id]--;    // request fulfilled, no longer waiting
    p->allocation[tid][semaphore_id]++; // this thread now holds one unit
    p->available[semaphore_id]--;       // one fewer unit available to others

    return 0;
}

int sys_condvar_create()
{
	struct condvar *c = condvar_create();
	if (c == NULL) {
		errorf("fail to create condvar: out of resource");
		return -1;
	}
	int cond_id = c - curr_proc()->condvar_pool;
	debugf("create condvar %d", cond_id);
	return cond_id;
}

int sys_condvar_signal(int cond_id)
{
	if (cond_id < 0 || cond_id >= curr_proc()->next_condvar_id) {
		errorf("Unexpected condvar id %d", cond_id);
		return -1;
	}
	cond_signal(&curr_proc()->condvar_pool[cond_id]);
	return 0;
}

int sys_condvar_wait(int cond_id, int mutex_id)
{
	if (cond_id < 0 || cond_id >= curr_proc()->next_condvar_id) {
		errorf("Unexpected condvar id %d", cond_id);
		return -1;
	}
	if (mutex_id < 0 || mutex_id >= curr_proc()->next_mutex_id) {
		errorf("Unexpected mutex id %d", mutex_id);
		return -1;
	}
	cond_wait(&curr_proc()->condvar_pool[cond_id],
		  &curr_proc()->mutex_pool[mutex_id]);
	return 0;
}

// LAB5: (2) you may need to define function enable_deadlock_detect here
int sys_enable_deadlock_detect(int enable)
{
    struct proc *p = curr_proc();
    p->deadlock_detect_enabled = enable;
    return 0;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_thread()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	if (id != SYS_write && id != SYS_read && id != SYS_sched_yield) {
		debugf("syscall %d args = [%x, %x, %x, %x, %x, %x]", id,
		       args[0], args[1], args[2], args[3], args[4], args[5]);
	}
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
	// case SYS_nanosleep:
	// 	ret = sys_nanosleep(args[0]);
	// 	break;
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
	case SYS_pipe2:
		ret = sys_pipe(args[0]);
	case SYS_thread_create:
		ret = sys_thread_create(args[0], args[1]);
		break;
	case SYS_gettid:
		ret = sys_gettid();
		break;
	case SYS_waittid:
		ret = sys_waittid(args[0]);
		break;
	case SYS_mutex_create:
		ret = sys_mutex_create(args[0]);
		break;
	case SYS_mutex_lock:
		ret = sys_mutex_lock(args[0]);
		break;
	case SYS_mutex_unlock:
		ret = sys_mutex_unlock(args[0]);
		break;
	case SYS_semaphore_create:
		ret = sys_semaphore_create(args[0]);
		break;
	case SYS_semaphore_up:
		ret = sys_semaphore_up(args[0]);
		break;
	case SYS_semaphore_down:
		ret = sys_semaphore_down(args[0]);
		break;
	case SYS_condvar_create:
		ret = sys_condvar_create();
		break;
	case SYS_condvar_signal:
		ret = sys_condvar_signal(args[0]);
		break;
	case SYS_condvar_wait:
		ret = sys_condvar_wait(args[0], args[1]);
		break;
	// LAB5: (2) you may need to add case SYS_enable_deadlock_detect here
	case SYS_enable_deadlock_detect:
		ret = sys_enable_deadlock_detect(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	curr_thread()->trapframe->a0 = ret;
	if (id != SYS_write && id != SYS_read && id != SYS_sched_yield) {
		debugf("syscall %d ret %d", id, ret);
	}
}
