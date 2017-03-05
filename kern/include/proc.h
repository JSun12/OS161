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

#ifndef _PROC_H_
#define _PROC_H_

/*
 * Definition of a process.
 *
 * Note: curproc is defined by <current.h>.
 */

#include <spinlock.h>
#include <thread.h> /* required for struct threadarray */
#include <filetable.h>
#include <machine/trapframe.h>
#include <limits.h>

/*
* Table index status for pidtable
*/
#define READY 0     /* Index available for process */
#define RUNNING 1   /* Process running */
#define ZOMBIE 2    /* Process waiting to be reaped */
#define ORPHAN 3    /* Process running and parent exited */

/* Identifier for pid_next */
#define NONEXT 0

/*
 * Process structure.
 */
struct proc {
	char *p_name;			/* Name of this process */
	struct spinlock p_lock;		/* Lock for this structure */
	struct threadarray p_threads;	/* Threads in this process */

	/* PID */
	pid_t pid;  /* Process id */
	struct array *children;

	/* VM */
	struct addrspace *p_addrspace;	/* virtual address space */

	/* VFS */
	struct vnode *p_cwd;		/* current working directory */
	struct ft *proc_ft;
};

struct pidtable {
	struct lock *pid_lock;
	struct cv *pid_cv;  /* To allow for processes to sleep on waitpid */
	struct proc *pid_procs[PID_MAX+1]; /* Array to hold processes */
	int pid_status[PID_MAX+1]; /* Array to hold process statuses */
	int pid_waitcode[PID_MAX+1]; /* Array to hold the wait codes*/
	int pid_available;  /* Number of available pid spaces */
	int pid_next; /* Lowest free PID */
};

/* Initializes the pid table*/
void pidtable_bootstrap(void);
int pidtable_add(struct proc *, int32_t *);
void pidtable_exit(struct proc *, int32_t);
void pidtable_update_children(struct proc *proc);

/* This is the process structure for the kernel and for kernel-only threads. */
extern struct proc *kproc;

/* Call once during system startup to allocate data structures. */
void proc_bootstrap(void);

/* Create a fresh process for use by runprogram(). */
struct proc *proc_create_runprogram(const char *name);

/* Destroy a process. */
void proc_destroy(struct proc *proc);

/* Attach a thread to a process. Must not already have a process. */
int proc_addthread(struct proc *proc, struct thread *t);

/* Detach a thread from its process. */
void proc_remthread(struct thread *t);

/* Fetch the address space of the current process. */
struct addrspace *proc_getas(void);

/* Change the address space of the current process, and return the old one. */
struct addrspace *proc_setas(struct addrspace *);


/* Process syscalls */
int sys_fork(struct trapframe *, int32_t *);
int proc_create_fork(struct proc **);
int setup_forked_trapframe(struct trapframe *, struct trapframe **);
void enter_usermode(void *, unsigned long);

int sys_getpid(int32_t *);
int sys_waitpid(pid_t, int32_t *, int32_t);
void sys__exit(int32_t);
//int assign_pid(struct proc *, int32_t *);

int sys_execv(const char *, char **);



#endif /* _PROC_H_ */
