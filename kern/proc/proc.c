/*
 * Copyright (c) 2013
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <filetable.h>
#include <limits.h>
#include <kern/errno.h>
#include <machine/trapframe.h>
#include <cpu.h>


/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;

/*
 * The PID table accessible by all processes and global statuses for table
 */
static struct pidtable *pidtable;
int *ready;
int *running;
int *zombie;

/*
 * Create a proc structure.
 */
static
struct proc *
proc_create(const char *name)
{
	struct proc *proc;

	proc = kmalloc(sizeof(*proc));
	if (proc == NULL) {
		return NULL;
	}
	proc->p_name = kstrdup(name);
	if (proc->p_name == NULL) {
		kfree(proc);
		return NULL;
	}
	proc->proc_ft = ft_create();
	if (proc->proc_ft == NULL) {
		kfree(proc->p_name);
		kfree(proc);
		return NULL;
	}

	threadarray_init(&proc->p_threads);
	spinlock_init(&proc->p_lock);

	/* VM fields */
	proc->p_addrspace = NULL;

	/* VFS fields */
	proc->p_cwd = NULL;

	/* PID fields */
	proc->pid = 1;  /* The kernel thread is defined to be 1 */

	return proc;
}

/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */
void
proc_destroy(struct proc *proc)
{
	/*
	 * You probably want to destroy and null out much of the
	 * process (particularly the address space) at exit time if
	 * your wait/exit design calls for the process structure to
	 * hang around beyond process exit. Some wait/exit designs
	 * do, some don't.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd) {
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}

	/* VM fields */
	if (proc->p_addrspace) {
		/*
		 * If p is the current process, remove it safely from
		 * p_addrspace before destroying it. This makes sure
		 * we don't try to activate the address space while
		 * it's being destroyed.
		 *
		 * Also explicitly deactivate, because setting the
		 * address space to NULL won't necessarily do that.
		 *
		 * (When the address space is NULL, it means the
		 * process is kernel-only; in that case it is normally
		 * ok if the MMU and MMU- related data structures
		 * still refer to the address space of the last
		 * process that had one. Then you save work if that
		 * process is the next one to run, which isn't
		 * uncommon. However, here we're going to destroy the
		 * address space, so we need to make sure that nothing
		 * in the VM system still refers to it.)
		 *
		 * The call to as_deactivate() must come after we
		 * clear the address space, or a timer interrupt might
		 * reactivate the old address space again behind our
		 * back.
		 *
		 * If p is not the current process, still remove it
		 * from p_addrspace before destroying it as a
		 * precaution. Note that if p is not the current
		 * process, in order to be here p must either have
		 * never run (e.g. cleaning up after fork failed) or
		 * have finished running and exited. It is quite
		 * incorrect to destroy the proc structure of some
		 * random other process while it's still running...
		 */
		struct addrspace *as;

		if (proc == curproc) {
			as = proc_setas(NULL);
			as_deactivate();
		}
		else {
			as = proc->p_addrspace;
			proc->p_addrspace = NULL;
		}
		as_destroy(as);
	}

	ft_destroy(proc->proc_ft);

	threadarray_cleanup(&proc->p_threads);
	spinlock_cleanup(&proc->p_lock);

	kfree(proc->p_name);
	kfree(proc);
}

/*
 * Create the process structure for the kernel.
 */
void
proc_bootstrap(void)
{
	kproc = proc_create("[kernel]");
	if (kproc == NULL) {
		panic("proc_create for kproc failed\n");
	}
}

/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)
{
	struct proc *newproc;
	int ret;

	newproc = proc_create(name);
	if (newproc == NULL) {
		return NULL;
	}

	ret = ft_init_std(newproc->proc_ft);
	if (ret) {
		kfree(newproc);
		return NULL;
	}

	//newproc->pid_lock = lock_create("new_lock");
	//if (newproc->pid_lock == NULL) {
	//	kfree(newproc);
	//	return NULL;
	//}
	/*
	struct proc *pid_array[PID_MAX];
	for (int i = 0; i < PID_MAX; i++) {
		pid_array[i] = NULL;
	}

	newproc->pid_array = pid_array;
	newproc->pid_array[0] = newproc;
	*/
	newproc->pid = (int) NULL;

	/* VM fields */

	newproc->p_addrspace = NULL;

	/* VFS fields */

	/*
	 * Lock the current process to copy its current directory.
	 * (We don't need to lock the new process, though, as we have
	 * the only reference to it.)
	 */
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		newproc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	/* Initialize console */


	return newproc;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int
proc_addthread(struct proc *proc, struct thread *t)
{
	int result;
	int spl;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_lock);
	result = threadarray_add(&proc->p_threads, t, NULL);
	spinlock_release(&proc->p_lock);
	if (result) {
		return result;
	}
	spl = splhigh();
	t->t_proc = proc;
	splx(spl);
	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void
proc_remthread(struct thread *t)
{
	struct proc *proc;
	unsigned i, num;
	int spl;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	/* ugh: find the thread in the array */
	num = threadarray_num(&proc->p_threads);
	for (i=0; i<num; i++) {
		if (threadarray_get(&proc->p_threads, i) == t) {
			threadarray_remove(&proc->p_threads, i);
			spinlock_release(&proc->p_lock);
			spl = splhigh();
			t->t_proc = NULL;
			splx(spl);
			return;
		}
	}
	/* Did not find it. */
	spinlock_release(&proc->p_lock);
	panic("Thread (%p) has escaped from its process (%p)\n", t, proc);
}

/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace *
proc_getas(void)
{
	struct addrspace *as;
	struct proc *proc = curproc;

	if (proc == NULL) {
		return NULL;
	}

	spinlock_acquire(&proc->p_lock);
	as = proc->p_addrspace;
	spinlock_release(&proc->p_lock);
	return as;
}

/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace *
proc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}


int
sys_fork(struct trapframe *tf, int32_t *retval0)
{
	struct proc *new_proc;
	struct trapframe *new_tf;
	int result;

	new_proc = proc_create("new_proc");
	if (new_proc == NULL) {
		return ENOMEM;
	}

	new_tf = kmalloc(sizeof(struct trapframe));
	if (new_tf == NULL) {
		kfree(new_proc);
		return ENOMEM;
	}

	result = as_copy(curproc->p_addrspace, &new_proc->p_addrspace);
	if (result) {
		return result;
	}

	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		new_proc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	//new_proc->pid_lock = curproc->pid_lock;
	//new_proc->pid_array = curproc->pid_array;

	new_proc->pid = pidtable_add(new_proc);
	if(new_proc->pid == -1){
		return ENPROC;
	}
	*retval0 = new_proc->pid;
	//TODO: clean up if this fails

	ft_copy(curproc->proc_ft, new_proc->proc_ft);

	memcpy((void *) new_tf, (const void *) tf, sizeof(struct trapframe));
	new_tf->tf_v0 = 0;
	new_tf->tf_epc += 4;
	tf->tf_v0 = 0;
	tf->tf_v1 = 0;
	tf->tf_a3 = 0;      /* signal no error */

	result = thread_fork("new_thread", new_proc, enter_usermode, new_tf, 1);
	if (result) {
		return result;
	}

	return 0;

	// kprintf("here");
}

void
pidtable_bootstrap()
{
	int result;

	/* Set up the running status variables */
	running = kmalloc(sizeof(int));
	ready = kmalloc(sizeof(int));
	zombie = kmalloc(sizeof(int));

	*running = RUNNING;
	*ready = READY;
	*zombie = ZOMBIE;

	/* Set up the pidtables */
	pidtable = kmalloc(sizeof(struct pidtable));
	if (pidtable == NULL) {
		panic("Unable to initialize PID table.\n");
	}

	pidtable->pid_lock = lock_create("pidtable lock");
	if (pidtable->pid_lock == NULL) {
		panic("Unable to intialize PID table's lock.\n");
	}

	pidtable->pid_cv = cv_create("pidtable cv");
	if (pidtable->pid_lock == NULL) {
		panic("Unable to intialize PID table's cv.\n");
	}

	pidtable->pid_procs = array_create();
	if (pidtable->pid_procs == NULL) {
		panic("Unable to initialize PID process array.\n");
	}

	/* Preallocate PID_MAX + 1 spots. Index with pid value, which is 1-based. Thus we need PID_MAX + 1.*/
	result = array_preallocate(pidtable->pid_procs, PID_MAX + 1);
	if (result) {
		panic("Unable to preallocate PID process array.\n");
	}
	array_setsize(pidtable->pid_procs, PID_MAX + 1);

	pidtable->pid_stats = array_create();
	if (pidtable->pid_stats == NULL) {
		panic("Unable to initialize PID stats array.\n");
	}
	array_setsize(pidtable->pid_stats, PID_MAX + 1);

	result = array_preallocate(pidtable->pid_stats, PID_MAX + 1);
	if (result) {
		panic("Unable to preallocate PID stats array.\n");
	}

	/* Set the starting constants */
	array_set(pidtable->pid_stats, kproc->pid, running);  // The kernel's PID is 1, thus 1 will be taken
	pidtable->pid_numready = PID_MAX - 1;
	pidtable->pid_next = PID_MIN;
	pidtable->pid_tablesize = PID_MAX;

	/* Populate the initial PID stats array with ready status */
	for (int i = PID_MIN; i < PID_MAX; i++){
		array_set(pidtable->pid_stats, i, ready);
	}
}

int
pidtable_add(struct proc *proc)
{
	int output;
	int next;

	next = pidtable->pid_next;

	lock_acquire(pidtable->pid_lock);
	/* Case: Ready PID that already is in the table */
	if(pidtable->pid_numready > 0)
	{
		next = pidtable->pid_next;
		/* Check to see if we can fit the new PID into the existing table */
	 	if(pidtable->pid_tablesize >= next){
			array_set(pidtable->pid_procs, next, proc);
			array_set(pidtable->pid_stats, next, running);
			output = next;
		}
		else{
			array_add(pidtable->pid_procs, proc, (unsigned int *) &output);
			array_add(pidtable->pid_stats, running, (unsigned int *) &output);
		}

		/* If the table has at least 1 more ready, try to find it starting from next -> end */
		pidtable->pid_numready = pidtable->pid_numready - 1;

		if(pidtable->pid_numready > 0){
			for (int i = next + 1; i < pidtable->pid_tablesize; i++){
				if (array_get(pidtable->pid_stats, i) == ready){
					pidtable->pid_next = i;
					break;
				}
			}
			/* If it could not be found, signal that it's to be put at size + 1 */
			if (pidtable->pid_next == next){
				pidtable->pid_next = pidtable->pid_tablesize + 1;
			}
		}
		/* If we cannot add any new pids */
		else{
			pidtable->pid_next = NONEXT;
		}
	}
	else
	{
		panic("Return that we cannot add more stuff. TODO: Return normal error.\n");
	}

	lock_release(pidtable->pid_lock);

	return next;
}
int
pidtable_find(struct proc *proc)
{
	int pos;

	(void) proc;
	pos = -1; /* Error condition */

	lock_acquire(pidtable->pid_lock);

	lock_release(pidtable->pid_lock);

	return pos;
}

int
pidtable_remove(struct proc *proc)
{
	int pos;

	pos = -1; /* Error condition: Proc not in PID table*/

	lock_acquire(pidtable->pid_lock);
	(void) proc;
	lock_release(pidtable->pid_lock);

	return pos;
}


int
sys_getpid(int32_t *retval0)
{
	*retval0 = pidtable_find(curproc);
	return 0;
}
/*
int
assign_pid(struct proc *new_proc, int32_t *retval0)
{
	lock_acquire(new_proc->pid_lock);

	for (int i = 0; i < PID_MAX; i++){
		if (new_proc->pid_array[i] != NULL) {
			continue;
		}
		new_proc->pid_array[i] = new_proc;
		new_proc->pid = i+1;
		*retval0 = (int32_t) i;

		lock_release(new_proc->pid_lock);
		return 0;
	}

	lock_release(new_proc->pid_lock);
	return -1;
}
*/
void
enter_usermode(void *data1, unsigned long data2)
{
	(void) data2;
	void *tf = (void *) curthread->t_stack + 16;

	//TODO: FREE DATA1 I think?
	memcpy(tf, (const void *) data1, sizeof(struct trapframe));
	as_activate();
	mips_usermode(tf);
}
