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

#include <kern/fcntl.h>
#include <lib.h>
#include <vm.h>
#include <vfs.h>
#include <syscall.h>
#include <test.h>
#include <kern/wait.h>
#include <copyinout.h>

/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;

/*
 * The PID table accessible by all processes and global statuses for table
 */
static struct pidtable *pidtable;

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
		as_destroy(as);
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
	int ret;

	ret = proc_create_fork("new_proc", &new_proc);
	if(ret) {
		return ret;
	}

	ret = pidtable_add(new_proc, &new_proc->pid);
	if (ret){
		proc_destroy(new_proc);
		return ret;
	}

	struct trapframe *new_tf;
	setup_forked_trapframe(tf, &new_tf);

	*retval0 = new_proc->pid;
	ret = thread_fork("new_thread", new_proc, enter_usermode, new_tf, 1);
	if (ret) {
		proc_destroy(new_proc);
		kfree(new_tf);
		return ret;
	}

	return 0;
}

int
proc_create_fork(const char *name, struct proc **new_proc)
{
	int ret;

	*new_proc = proc_create(name);
	if (*new_proc == NULL) {
		return ENOMEM;
	}

	ret = as_copy(curproc->p_addrspace, &(*new_proc)->p_addrspace);
	if (ret) {
		proc_destroy(*new_proc);
		return ret;
	}

	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL) {
		VOP_INCREF(curproc->p_cwd);
		(*new_proc)->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	struct ft *ft = curproc->proc_ft;
	lock_acquire(ft->ft_lock);
	ft_copy(ft, (*new_proc)->proc_ft);
	lock_release(ft->ft_lock);

	return 0;
}

int
setup_forked_trapframe(struct trapframe *old_tf, struct trapframe **new_tf)
{
	*new_tf = kmalloc(sizeof(struct trapframe));
	if (*new_tf == NULL) {
		return ENOMEM;
	}

	memcpy((void *) *new_tf, (const void *) old_tf, sizeof(struct trapframe));
	(*new_tf)->tf_v0 = 0;
	(*new_tf)->tf_v1 = 0;
	(*new_tf)->tf_a3 = 0;      /* signal no error */
	(*new_tf)->tf_epc += 4;

	return 0;
}

void
enter_usermode(void *data1, unsigned long data2)
{
	(void) data2;
	void *tf = (void *) curthread->t_stack + 16;

	memcpy(tf, (const void *) data1, sizeof(struct trapframe));
	kfree((struct trapframe *) data1);

	as_activate();
	mips_usermode(tf);
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
	pidtable->pid_procs[kproc->pid] = kproc;
	pidtable->pid_status[kproc->pid] = RUNNING;
	pidtable->pid_waitcode[kproc->pid] = (int) NULL;
	pidtable->pid_available = PID_MAX - 1;
	pidtable->pid_next = PID_MIN;

	/* Populate the initial PID stats array with ready status */
	for (int i = PID_MIN; i < PID_MAX; i++){
		pidtable->pid_procs[i] = NULL;
		pidtable->pid_status[i] = READY;
		pidtable->pid_waitcode[i] = (int) NULL;
	}
}

/*
 * Will add a process to the filetable and return the number in the retval input. Errors will be
 * passed through the integer output following the format of the other system calls.
 */
int
pidtable_add(struct proc *proc, int32_t *retval)
{
	int output;
	int next;

	lock_acquire(pidtable->pid_lock);

	// Add the given process to the parent
	array_add(curproc->children, proc, NULL);

	if(pidtable->pid_available > 0){
		next = pidtable->pid_next;
		*retval = next;
		output = 0;

		pidtable->pid_procs[next] = proc;
		pidtable->pid_status[next] = RUNNING;
		pidtable->pid_waitcode[next] = (int) NULL;
		pidtable->pid_available--;

		if(pidtable->pid_available > 0){
			for (int i = next; i < PID_MAX; i++){
				/* Find the next available PID */
				if (pidtable->pid_status[i] == READY){
					pidtable->pid_next = i;
					break;
				}
			}
		}
		else{
			/*
			 * Update pid_next to reflect that the table it full. The value we set it to is beyond
			 * PID_MAX as when we remove, we update pid_next to be the lowest possible pid value we
			 * have, which is always under PID_MAX + 1.
			 */
			pidtable->pid_next = PID_MAX + 1;
		}
	}
	else{
		/* The PID table is full*/
		retval = NULL;
		output = ENPROC;
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
	//TODO: Orphan children of a parent
	lock_acquire(pidtable->pid_lock);

	/* Begin by orphaning all children */
	pidtable_update_children(proc);

	/* Case: Signal the parent that the child ended with waitcode given. */
	if(pidtable->pid_status[proc->pid] == RUNNING){
		pidtable->pid_status[proc->pid] = ZOMBIE;
		pidtable->pid_waitcode[proc->pid] = waitcode;
	}
	/* Case: Parent already exited. Reset the current pidtable spot for later use. */
	else if(pidtable->pid_status[proc->pid] == ORPHAN){
		pidtable->pid_available++;
		pidtable->pid_procs[proc->pid] = NULL;
		pidtable->pid_status[proc->pid] = READY;
		pidtable->pid_waitcode[proc->pid] = (int) NULL;
		proc_destroy(curproc);
	}
	else{
		panic("Tried to remove a bad process.\n");
	}

	/* Broadcast to any waiting processes. There is no guarentee that the processes on the cv are waiting for us */
	cv_broadcast(pidtable->pid_cv,pidtable->pid_lock);

	lock_release(pidtable->pid_lock);

	// We might want to synchronize this more
	thread_exit();
}

/*
 * Will update the status of children to either ORPHAN or ZOMBIE.
 */
void
pidtable_update_children(struct proc *proc)
{
	/* We assume that a function holding a lock calls this function */
	KASSERT(lock_do_i_hold(pidtable->pid_lock));

	int num_child = array_num(proc->children);
	/* Loop downwards as removing children will cause array shrinking and disrupt indexing */
	for(int i = num_child-1; i >= 0; i--){

		struct proc *child = array_get(proc->children, i);
		int child_pid = child->pid;
		/* Signal to the child we don't need it anymore */
		if(pidtable->pid_status[child_pid] == RUNNING){
			pidtable->pid_status[child_pid] = ORPHAN;
		}
		else if (pidtable->pid_status[child_pid] == ZOMBIE){
			/* Update the next pid indicator */
			if(child_pid < pidtable->pid_next){
				pidtable->pid_next = child_pid;
			}
			pidtable->pid_available++;
			pidtable->pid_procs[child->pid] = NULL;
			pidtable->pid_status[child->pid] = READY;
			pidtable->pid_waitcode[child->pid] = (int) NULL;
			proc_destroy(child);
		}
		else{
			panic("Tried to modify a child that did not exist.\n");
		}
	}
}

/*
 * Gets the PID of the current process.
 */
int
sys_getpid(int32_t *retval0)
{
	lock_acquire(pidtable->pid_lock);

	*retval0 = curproc->pid;

	lock_release(pidtable->pid_lock);
	return 0;
}


/*
 * Function called by a parent process to wait until a child process exits.
 */
int
sys_waitpid(pid_t pid, int32_t *retval0, int32_t options)
{
	int status; // The status of the process which is being waited upon
	int waitcode; // The reason for process exit as defined in wait.h

	/* Check that we are calling a valid options argument. Currently this is only 0. */
	if (options != 0){
		return EINVAL;
	}

	/* Check that this is a valid process. This includes checking bounds and ensuring
	 * that the pid_status is not empty (ie. READY to run)
	 */
	if (pid < PID_MIN || pid > PID_MAX || pidtable->pid_status[pid] == READY){
		return ESRCH;
	}

	/* Check that the pid being called is a child of the current process */
	int ischild = 0;
	struct proc *child = pidtable->pid_procs[pid];
	int parentnum = array_num(curproc->children);
	for (int i = 0; i < parentnum; i++){
		if (child == array_get(curproc->children, i)){
			ischild = 1;
			break;
		}
	}
	if(ischild == 0){
		return ECHILD;
	}

	lock_acquire(pidtable->pid_lock);

	status = pidtable->pid_status[pid];
	while(status != ZOMBIE){
		cv_wait(pidtable->pid_cv, pidtable->pid_lock);
		status = pidtable->pid_status[pid];
	}
	waitcode = pidtable->pid_waitcode[pid];

	lock_release(pidtable->pid_lock);

	/* A NULL retval0 indicates that nothing is to be returned. */
	if(retval0 != NULL){
		int ret = copyout(&waitcode, (userptr_t) retval0, sizeof(int32_t));
		if (ret){
			return ret;
		}
	}

	return 0;
}

/*
 * Exits the current process and stores the waitcode as defined in <kern/wait.h>.
 * The supplied waitcode to this funtion is assumed to be already encoded properly.
 */
void
sys__exit(int32_t waitcode)
{
	/* Use definitions from <kern/wait.h> to encode the waitcode status  */
	/*int waitcode;
	int switchcode;

	switchcode = _WWHAT(exit_status);
	switch (switchcode) {
		case __WEXITED:
		waitcode = _MKWAIT_EXIT(exit_status);
		break;
		case __WSIGNALED:
		waitcode = _MKWAIT_SIG(exit_status);
		break;
		case __WCORED:
		waitcode = _MKWAIT_CORE(exit_status);
		break;
		case __WSTOPPED:
		waitcode = _MKWAIT_STOP(exit_status);
		break;
		default:
		waitcode = exit_status;
	}
	(void) waitcode;*/
	pidtable_exit(curproc, waitcode);
	panic("Exit syscall should never get to this point.");
}





/*
Make crashed programs go back to kernel menu.
*/



int
sys_execv(const char *prog, char **args)
{
	int ret;

	if (prog == NULL || args == NULL) {
		return EFAULT;
	}

	char *progname;
	ret = string_in(prog, &progname, PATH_MAX);
	if (ret) {
		return ret;
	}

	int argc;
	ret = get_argc(args, &argc);
	if (ret) {
		return ret;
	}

	char **args_in = kmalloc(argc*sizeof(char *));
	int *size = kmalloc(argc*sizeof(int));
	ret = copy_in_args(argc, args, args_in, size);
	if (ret) {
		return ret;
	}

	struct vnode *v;
	vaddr_t entrypoint, stackptr;

	ret = vfs_open(progname, O_RDONLY, 0, &v);
	if (ret) {
		return ret;
	}

	struct addrspace *as;
	as = proc_setas(NULL);
	as_deactivate();
	as_destroy(as);

	KASSERT(proc_getas() == NULL);

	as = as_create();
	if (as == NULL) {
		return ENOMEM;
	}

	proc_setas(as);
	as_activate();

	ret = load_elf(v, &entrypoint);
	if (ret) {
		vfs_close(v);
		return ret;
	}

	vfs_close(v);

	ret = as_define_stack(as, &stackptr);
	if (ret) {
		return ret;
	}

	userptr_t args_out_addr;
	copy_out_args(argc, args_in, size, &stackptr, &args_out_addr);

	enter_new_process(argc, args_out_addr, NULL, stackptr, entrypoint);

	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}

/*
Checks to make sure the user string is less than max_len. If it is, then
actual_length holds true length (not including null terminator).
*/
int
strlen_check(const char *string, int max_len, size_t *actual_length)
{
	int i = 0;
	while (string[i] != 0 && i < max_len) i++;

	if (string[i] != 0) {
		return E2BIG;
	}

	*actual_length = i;
	return 0;
}

int
get_argc(char **args, int *argc)
{
	int i = 0;
	while(args[i] != NULL && i < ARG_MAX) i++;

	if (args[i] != NULL) {
		return E2BIG;
	}

	*argc = i;
	return 0;
}

int
string_in(const char *user_src, char **kern_dest, size_t copy_size)
{
	int ret;

	copy_size++;
	*kern_dest = kmalloc(copy_size*sizeof(char));
    size_t *path_len = kmalloc(sizeof(int));
	ret = copyinstr((const_userptr_t) user_src, *kern_dest, copy_size, path_len);
	if (ret) {
		return ret;
	}

	kfree(path_len);
	return 0;
}

int
string_out(const char *kernel_src, userptr_t user_dest, size_t copy_size)
{
	int ret;

	size_t *path_len = kmalloc(sizeof(int));
	ret = copyoutstr(kernel_src, user_dest, copy_size, path_len);
	if (ret) {
		return ret;
	}

	kfree(path_len);
	return 0;
}

/*
Copies the user strings into ther kernel, populating size[] with their respective lengths.
Returns an error if bytes surpasses ARG_MAX.
*/
int
copy_in_args(int argc, char **args, char **args_in, int *size)
{
	int arg_size_left = ARG_MAX;
	size_t cur_size;
	int ret;

	for (int i = 0; i < argc; i++) {
		ret = strlen_check((const char *) args[i], arg_size_left - 1, &cur_size);
		if (ret) {
			return ret;
		}

		arg_size_left -= (cur_size + 1);
		size[i] = (int) cur_size + 1;
		string_in((const char *) args[i], &args_in[i], cur_size);
	}

	return 0;
}

/*
Copies the kernel strings out to the user stack, and cleans up the kernel strings.
*/
void
copy_out_args(int argc, char **args, int *size, vaddr_t *stackptr, userptr_t *args_out_addr)
{
	userptr_t arg_addr = (userptr_t) (*stackptr - argc*sizeof(userptr_t *) - sizeof(NULL));
	userptr_t *args_out = (userptr_t *) (*stackptr - argc*sizeof(userptr_t *) - sizeof(NULL));
	for (int i = 0; i < argc; i++) {
		arg_addr -= size[i];
		*args_out = arg_addr;
		string_out((const char *) args[i], arg_addr, (size_t) size[i]);
		args_out++;
		kfree(args[i]);
	}

	*args_out = NULL;
	*args_out_addr = (userptr_t) (*stackptr - argc*sizeof(int) - sizeof(NULL));
	arg_addr -= (int) arg_addr % sizeof(void *);
	*stackptr = (vaddr_t) arg_addr;
}
