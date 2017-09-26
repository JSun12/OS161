# OS161

This is an implementation of an operating system for the Operating Systems course, [CPEN 331](
https://sites.google.com/site/cpen331/), at the University of British Columbia.

## Overview
This is a project that involved implementing various portions of an operating system. The main goal was to gain a strengthened understanding of concurrency, synchronization, virtual memory, system calls, and file systems.

The [course](https://courses.students.ubc.ca/cs/main?pname=subjarea&tname=subjareas&req=3&dept=CPEN&course=331) that this project was associated with describes the key aspects of the course as: 
Operating systems, their design and their implementation. Process concurrency, synchronization, communication and scheduling. Device drivers, memory management, virtual memory, file systems, networking and security.

## Implemented Features
### Concurrency
#### Mutex Lock
A mutex lock is an struct that may be acquired only by one thread at a time. It operates using a waiting channel and a spinlock. When a thread wants to use the mutex lock, the implementation will acquire a spinlock (and spin on it if it not available), check the status of the lock flag, and sleep on the waiting channel / release the spinlock if the flag is not free.

The implementation of mutex locks can be found in /kern/thread/synch.c

#### Condition Variable
A condition variable is a struct that threads may be placed on to sleep until some function calls upon it to wake a single thread, or all threads. Condition variables allow for synchronization as the act of going to sleep/waking up is an atomic action. They are often used for pausing until a condition is satisfied that allows for concurrent operations to occur safely.

The implementation of condition variables can be found in /kern/thread/synch.c

### Filesystem System Calls
#### open()
The implementation of open() can be found in /kern/syscall/fsyscall.c
#### read()
The implementation of read() can be found in /kern/syscall/fsyscall.c
#### write()
The implementation of write() can be found in /kern/syscall/fsyscall.c
#### lseek()
The implementation of lseek() can be found in /kern/syscall/fsyscall.c
#### close()
The implementation of close() can be found in /kern/syscall/fsyscall.c
#### dup2()
The implementation of dup2() can be found in /kern/syscall/fsyscall.c
#### chdir()
The implementation of chdir() can be found in /kern/syscall/fsyscall.c
#### __getcwd()
The implementation of __getcwd() can be found in /kern/syscall/fsyscall.c

### Process System Calls
#### System Call: getpid()
The implementation of getpid() can be found in /kern/syscall/psyscall.c
#### System Call: fork()
The implementation of fork() can be found in /kern/syscall/psyscall.c
#### System Call: execv()
The implementation of execv() can be found in /kern/syscall/psyscall.c
#### System Call: _exit()
The implementation of _exit() can be found in /kern/syscall/psyscall.c
#### System Call: kill_curthread()
The implementation of kill_curthread() can be found in [TODO: fill in]
#### System Call: sbrk()
The implementation of sbrk() can be found in [TODO: fill in]

### Virtual Memory
OS161 has implemented virtual memory. TODO: Talk about TLB, page tables, MMU, swap.

## Configuration
### Installation
Detailed installation instructions for the base OS161 may be found [here](https://sites.google.com/site/os161ubc/os161-installation) and [here](http://os161.eecs.harvard.edu/resources/building.html). If you are simply working with this repository, the following libraries will be required:
* [bmake](http://crufty.net/help/sjg/bmake.html)
* [libmpc](http://www.multiprecision.org/index.php?prog=mpc)
* [gmp](https://gmplib.org/)
* [wget](https://www.gnu.org/software/wget/)

On Linux these may be installed by the command:
```
sudo apt-get -y install bmake ncurses-dev libmpc-dev
```
On macOS these may be installed by the Homebrew commands:
```
brew install bmake
brew install libmpc
brew install gmp
brew install wget
```
### Setup
We assume os161 is installed at ~/os161. We must use the following commands to setup and compile the kernel
1. Configure the build. 
```
cd ~/os161/src
./configure --ostree=$HOME/os161/root
```
2. Build userland.
```
bmake
bmake install
```
3. Configure a kernel.
```
cd kern/conf
./config DUMBVM
```
4. Compile and install the kernel
```
cd ~/os161/src/kern/compile/DUMBVM
bmake depend
bmake
bmake install
```
