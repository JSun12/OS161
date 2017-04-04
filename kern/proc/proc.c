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

/* Global PID table */
struct pidtable *pidtable;

/*
 * Create a proc structure.
 */
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

	proc->children = array_create();
	if (proc->children == NULL) {
		kfree(proc->proc_ft);
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

	/* PID Fields */
	int children_size = array_num(proc->children);
	for (int i = 0; i < children_size; i++){
		array_remove(proc->children, 0);
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
		as_destroy(as, proc->pid);
	}

	ft_destroy(proc->proc_ft);

	int threadarray_size = threadarray_num(&proc->p_threads);
	for (int i = 0; i < threadarray_size; i++){
		threadarray_remove(&proc->p_threads, 0);
	}
	threadarray_cleanup(&proc->p_threads);
	spinlock_cleanup(&proc->p_lock);

	array_destroy(proc->children);

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

	ret = pidtable_add(newproc, &newproc->pid);
	if(ret){
		ft_destroy(newproc->proc_ft);
		kfree(newproc);
		return NULL;
	}

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

	return newproc;
}


/*
 * Sets up the memory structures for a newly forked process.
 */
int
proc_create_fork(const char *name, struct proc **new_proc)
{
	int ret;
	struct proc *proc;

	proc = proc_create(name);
	if (proc == NULL) {
		return ENOMEM;
	}

	ret = pidtable_add(proc, &proc->pid);
	if (ret){
		proc_destroy(proc);
		return ret;
	}

	ret = as_copy(curproc->p_addrspace, &proc->p_addrspace, proc->pid);
	if (ret) {
		pidtable_freepid(proc->pid);
		proc_destroy(proc);
		return ret;
	}

	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		proc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	struct ft *ft = curproc->proc_ft;
	lock_acquire(ft->ft_lock);
	ft_copy(ft, proc->proc_ft);
	lock_release(ft->ft_lock);

	*new_proc = proc;
	return 0;
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

/* Clears the pidtable for a given index */
static
void
clear_pid(pid_t pid)
{
	KASSERT(pid >= PID_MIN && pid <= PID_MAX);

	pidtable->pid_available++;
	pidtable->pid_procs[pid] = NULL;
	pidtable->pid_status[pid] = READY;
	pidtable->pid_waitcode[pid] = (int) NULL;
}

struct proc *
get_pid(pid_t pid)
{
	KASSERT(pid >= PID_MIN && pid <= PID_MAX);

	struct proc *proc;

	lock_acquire(pidtable->pid_lock);
	proc = pidtable->pid_procs[pid]; 
	lock_release(pidtable->pid_lock);

	return proc;	
}

/* Removes a given PID from the PID table. Used for failed forks. */
void
pidtable_freepid(pid_t pid)
{
	KASSERT(pid >= PID_MIN && pid <= PID_MAX);

	lock_acquire(pidtable->pid_lock);
	clear_pid(pid);
	lock_release(pidtable->pid_lock);
}

/* Adds a given process to the pidtable at the given index */
static
void
add_pid(pid_t pid, struct proc *proc)
{
	KASSERT(proc != NULL);

	pidtable->pid_procs[pid] = proc;
	pidtable->pid_status[pid] = RUNNING;
	pidtable->pid_waitcode[pid] = (int) NULL;
	pidtable->pid_available--;
}

/* Will update the status of children to either ORPHAN or ZOMBIE. */
static
void
pidtable_update_children(struct proc *proc)
{
	KASSERT(lock_do_i_hold(pidtable->pid_lock));
	KASSERT(proc != NULL);

	int num_child = array_num(proc->children);

	for(int i = num_child-1; i >= 0; i--){

		struct proc *child = array_get(proc->children, i);
		int child_pid = child->pid;

		if(pidtable->pid_status[child_pid] == RUNNING){
			pidtable->pid_status[child_pid] = ORPHAN;
		}
		else if (pidtable->pid_status[child_pid] == ZOMBIE){
			/* Update the next pid indicator */
			if(child_pid < pidtable->pid_next){
				pidtable->pid_next = child_pid;
			}
			clear_pid(child_pid);
			proc_destroy(child);
		}
		else{
			panic("Tried to modify a child that did not exist.\n");
		}
	}
}

/*
 * Initializes the PID table upon starting the kernel.
 */
void
pidtable_bootstrap()
{
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

	/* Set the kernel thread parameters */
	pidtable->pid_available = 1; /* One space for the kernel process */
	pidtable->pid_next = PID_MIN;
	add_pid(kproc->pid, kproc);

	/* Create space for more pids within the table */
	for (int i = PID_MIN; i < PID_MAX; i++){
		clear_pid(i);
	}
}

/*
 * Will add a process to the filetable and return the number in the retval input. Errors will be
 * passed through the integer output following the format of the other system calls.
 */
int
pidtable_add(struct proc *proc, int32_t *retval)
{
	int next;
	int output = 0;

	KASSERT(proc != NULL);

	lock_acquire(pidtable->pid_lock);

	if (pidtable->pid_available < 1){
		lock_release(pidtable->pid_lock);
		return ENPROC;
	}

	array_add(curproc->children, proc, NULL);

	next = pidtable->pid_next;
	*retval = next;

	add_pid(next, proc);

	/* Find the next available PID */
	if(pidtable->pid_available > 0){
		for (int i = next; i < PID_MAX; i++){
			if (pidtable->pid_status[i] == READY){
				pidtable->pid_next = i;
				break;
			}
		}
	}
	/* Put an out-of-bounds value to signify a full table */
	else{
		pidtable->pid_next = PID_MAX + 1;
	}

	lock_release(pidtable->pid_lock);

	return output;
}

/*
 * Function called when a process exits.
 */
void
pidtable_exit(struct proc *proc, int32_t waitcode)
{
	KASSERT(proc != NULL);

	lock_acquire(pidtable->pid_lock);

	pidtable_update_children(proc);

	/* Case: Signal the parent that the child ended with waitcode given. */
	if(pidtable->pid_status[proc->pid] == RUNNING){
		pidtable->pid_status[proc->pid] = ZOMBIE;
		pidtable->pid_waitcode[proc->pid] = waitcode;
	}
	/* Case: Parent already exited. Reset the current pidtable spot for later use. */
	else if(pidtable->pid_status[proc->pid] == ORPHAN){
		pid_t pid = proc->pid; 
		proc_destroy(curproc);
		clear_pid(pid);
	}
	else{
		panic("Tried to remove a bad process.\n");
	}

	/* Broadcast to any waiting processes. There is no guarentee that the processes on the cv are waiting for us */
	cv_broadcast(pidtable->pid_cv,pidtable->pid_lock);

	lock_release(pidtable->pid_lock);

	thread_exit();
}
