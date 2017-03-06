#ifndef _PSYSCALL_H_
#define _PSYSCALL_H_


/* Process system calls */
int sys_fork(struct trapframe *, int32_t *);
int sys_getpid(int32_t *);
int sys_waitpid(pid_t, int32_t *, int32_t);
void sys__exit(int32_t);
int sys_execv(const char *, char **);

/* Creating and entering a new process */
void enter_usermode(void *, unsigned long);

#endif /* _PSYSCALL_H_ */
