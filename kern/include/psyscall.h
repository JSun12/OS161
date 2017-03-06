#ifndef _PSYSCALL_H_
#define _PSYSCALL_H_


/* Process system calls */
int sys_fork(struct trapframe *, int32_t *);
int sys_getpid(int32_t *);
int sys_waitpid(pid_t, int32_t *, int32_t);
void sys__exit(int32_t);
int sys_execv(const char *, char **);

/* Creating and entering a new process */
int proc_create_fork(const char *, struct proc **);
int setup_forked_trapframe(struct trapframe *, struct trapframe **);
void enter_usermode(void *, unsigned long);


int strlen_check(const char *, int, size_t*);
int get_argc(char **, int *);
int string_in(const char *, char **, size_t);
int copy_in_args(int, char **, char **, int *);
void copy_out_args(int, char **, int *, vaddr_t *, userptr_t *);
int string_out(const char *, userptr_t, size_t);

#endif /* _PSYSCALL_H_ */
