#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;

  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");

  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  p->lev=0;
  p->priority=0;
  p->monopolize=0;
  p->stime=0;
  p->rtime=0;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();

  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void scheduler(void) {
  struct cpu *c = mycpu();
  c->proc = 0;

  for (;;) {
    sti(); // 인터럽트 활성화

    acquire(&ptable.lock);

    // MoQ에 실행 가능한 프로세스가 있는지 확인
    int mn_flag = 0;
    for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
      if (p->monopolize == 1 && p->state == RUNNABLE) {
        // MoQ 큐에 있는 첫 번째 프로세스를 실행
        c->proc = p;
        switchuvm(p);
        p->state = RUNNING;
        swtch(&(c->scheduler), p->context);
        switchkvm();
        c->proc = 0;

        mn_flag = 1;
        break;
      }
    }

    if (!mn_flag) {
      // MoQ에 프로세스가 없으면 MLFQ 스케줄링 수행
      int L0count = 0;
      for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
        if (p->lev == 0 && p->state == RUNNABLE) {
          L0count++;
          break;
        }
      }

      if (L0count > 0) {
        // L0 큐에서 실행
        for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
          if (p->state != RUNNABLE)
            continue;
          if (p->lev == 0) {
            // L0 큐의 time quantum을 확인하고 time quantum이 끝나면 레벨을 L1로 변경
            if ((ticks - p->stime) >= 3) {
              p->lev = 1; // L1로 레벨 변경
              p->stime = ticks; // 새로운 큐에서의 시작 시간 설정
              p->rtime = 0; // 실행 시간 초기화
            }
            c->proc = p;
            switchuvm(p);
            p->state = RUNNING;
            p->stime = ticks;
            p->rtime = 0;
            swtch(&(c->scheduler), p->context);
            switchkvm();
            c->proc = 0;
          }
        }
      } else {
        // L0 큐가 비어있을 때 L1 큐에서 우선순위가 높은 프로세스 실행
        struct proc *priority_prc = 0;
        for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
          if (p->state != RUNNABLE)
            continue;
          if (p->lev == 1) {
            if (priority_prc != 0) {
              if (priority_prc->priority > p->priority)
                priority_prc = p;
              else if (priority_prc->priority == p->priority) {
                if (priority_prc > p)
                  priority_prc = p;
              }
            } else
              priority_prc = p;
          }
        }

        if (priority_prc != 0) {
          struct proc *p = priority_prc;
          c->proc = p;
          switchuvm(p);
          p->state = RUNNING;
          p->stime = ticks;
          p->rtime = 0;
          swtch(&(c->scheduler), p->context);
          switchkvm();
          c->proc = 0;

          // L1 time quantum이 끝나면 L2로 이동
          if (p->lev == 1 && (ticks - p->stime) >= 4) {
            p->lev = 2;
            p->stime = ticks; // 새로운 큐에서의 시작 시간 설정
            p->rtime = 0; // 실행 시간 초기화
          }
        }
      }
    }

    // L2 큐에서 L3 큐로 이동
    for (struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
      if (p->state != RUNNABLE)
        continue;
      if (p->lev == 2) {
        struct proc *priority_prc = 0;
        for (struct proc *q = ptable.proc; q < &ptable.proc[NPROC]; q++) {
          if (q->state != RUNNABLE)
            continue;
          if (q->lev == 3) {
            if (priority_prc != 0) {
              if (priority_prc->priority > q->priority)
                priority_prc = q;
              else if (priority_prc->priority == q->priority) {
                if (priority_prc > q)
                  priority_prc = q;
              }
            } else
              priority_prc = q;
          }
        }

        if (priority_prc != 0) {
          struct proc *q = priority_prc;
          c->proc = q;
          switchuvm(q);
          q->state = RUNNING;
          q->stime = ticks;
          q->rtime = 0;
          swtch(&(c->scheduler), q->context);
          switchkvm();
          c->proc = 0;

          // L2 time quantum이 끝나면 L3로 이동
          if (q->lev == 2 && (ticks - q->stime) >= 6) {
            q->lev = 3;
            q->stime = ticks; // 새로운 큐에서의 시작 시간 설정
            q->rtime = 0; // 실행 시간 초기화
          }
        }
      }
    }

    release(&ptable.lock);
  }
}

      // Context Switching 부분
      // c->proc = p;
      // switchuvm(p);
      // p->state = RUNNING;

      // swtch(&(c->scheduler), p->context);
      // switchkvm();

      // // Process is done running for now.
      // // It should have changed its p->state before coming back.
      // c->proc = 0;

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

// Project02
int getlev(void) {
  if (myproc() -> monopolize == 1) {
    return 0;
  }
  return myproc() -> lev;
}

// void prioboosting(void) {
//   acquire(&ptable.lock);
//   struct proc* p;
//
//   for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
//     if(p -> lev == 1){
//       p -> lev = 0;
//       p -> priority = 0;
//     }
//   }
//   release(&ptable.lock);
// }

int setpriority(int pid, int priority) {
  struct proc *p;

  // 만약 pid가 0이면 현재 프로세스의 pid를 사용합니다.
  if (!pid) {
    pid = myproc() -> pid;
  }

  // 프로세스 테이블에서 해당 pid를 가진 프로세스를 찾습니다.
  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p -> pid == pid) {
      break;
    }
  }
  release(&ptable.lock);

  // 프로세스를 찾지 못한 경우 -1을 반환합니다.
  if (p == 0) {
    return -1;
  }

  // 이전 우선순위를 저장합니다.
  int old_priority = p -> priority;

  // 우선순위를 설정합니다.
  p -> priority = priority;

  // 우선순위가 이전보다 낮은 경우, yield() 함수를 호출하여 스케줄링을 재조정합니다.
  if (p -> priority < old_priority) {
    yield();
  }

  // 이전 우선순위를 반환합니다.
  return old_priority;
}

int setmonopoly(int pid, int password) {
  struct proc *p;

  // 프로세스 테이블에서 해당 pid를 가진 프로세스를 찾습니다.
  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p -> pid == pid) { // 해당 pid를 가진 프로세스를 찾았을 경우
      if (password == 2020068331) { // 암호가 일치하는 경우
        p -> monopolize = 1; // MoQ로 이동하여 독점 설정
        release(&ptable.lock);
        return 1; // MoQ의 크기를 반환합니다.
      } else { // 암호가 일치하지 않는 경우
        release(&ptable.lock);
        return -2; // 암호가 일치하지 않음을 나타내는 -2를 반환합니다.
      }
    }
  }

  // 해당 pid를 가진 프로세스를 찾지 못한 경우
  release(&ptable.lock);
  return -1; // pid가 존재하지 않음을 나타내는 -1을 반환합니다.
}

void monopolize(void) {
  acquire(&ptable.lock);

  if (myproc() -> monopolize == 1) {
    myproc() -> monopolize = 0;
    myproc() -> lev = 0;
    myproc() -> priority = 0;
  } else {
    myproc() -> monopolize = 1;
  }

  release(&ptable.lock);
}

void unmonopolize(void) {
  acquire(&ptable.lock);

  // 독점 중인지 확인하고, 독점 플래그를 해제합니다.
  if (myproc() -> monopolize == 1) {
    myproc() -> monopolize = 0;

    // 독점 중지 후에는 MLFQ part로 돌아가야 합니다.
    myproc() -> lev = 0;
    myproc() -> priority = 0;
  }

  release(&ptable.lock);
}
