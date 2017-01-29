/*
 * Driver code for airballoon problem
 */
#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>
#include <synch.h>

#define NROPES 16
#define NTHREADS 5
#define NSLEEP 0

static int ropes_left = NROPES;
static int threads_left = NTHREADS;
static int threads_sleep = NSLEEP;

// Data structures for rope mappings

struct rope {
	struct lock *rp_lock;
	int rp_hook;
	volatile int rp_stake;
	volatile bool rp_cut;
};
struct rope ropes[NROPES];

// Synchronization primitives

struct lock *ropes_left_lk;
struct lock *threads_lk;
struct cv *threads_cv;

/*
 * Describe your design and any invariants or locking protocols
 * that must be maintained. Explain the exit conditions. How
 * do all threads know when they are done?
 */

static
void
dandelion(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Dandelion thread starting\n");
	thread_yield(); // Yield to allow all actors to load before modifiying ropes

	while (ropes_left > 0){
		int next = random() % NROPES;
		if (ropes[next].rp_cut == false){
			lock_acquire(ropes[next].rp_lock);

			if (ropes[next].rp_cut == true){
				// Exit if the rope's condition changed after obtaining the lock.
				lock_release(ropes[next].rp_lock);
				continue;
			}

			ropes[next].rp_cut = true;
			kprintf("Dandelion severed rope %d\n", ropes[next].rp_hook);

			lock_acquire(ropes_left_lk);
			ropes_left--;
			lock_release(ropes_left_lk);

			lock_release(ropes[next].rp_lock);
			thread_yield();
		}
	}

	lock_acquire(threads_lk);

	threads_sleep++;
	cv_wait(threads_cv, threads_lk);

	kprintf("Dandelion thread done\n");
	threads_left--;

	lock_release(threads_lk);

	thread_exit();
}

static
void
marigold(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Marigold thread starting\n");
	thread_yield(); // Yield to allow all actors to load before modifiying ropes

	while (ropes_left > 0){
		int next = random() % NROPES;
		if (ropes[next].rp_cut == false){
			lock_acquire(ropes[next].rp_lock);

			if (ropes[next].rp_cut == true){
				// Exit if the rope's condition changed after obtaining the lock.
				lock_release(ropes[next].rp_lock);
				continue;
			}

			ropes[next].rp_cut = true;
			kprintf("Marigold severed rope %d from stake %d\n", ropes[next].rp_hook, ropes[next].rp_stake);

			lock_acquire(ropes_left_lk);
			ropes_left--;
			lock_release(ropes_left_lk);

			lock_release(ropes[next].rp_lock);
			thread_yield();
		}
	}
	lock_acquire(threads_lk);

	threads_sleep++;
	cv_wait(threads_cv, threads_lk);

	kprintf("Marigold thread done\n");
	threads_left--;

	lock_release(threads_lk);

	thread_exit();
}

static
void
flowerkiller(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Lord FlowerKiller thread starting\n");
	thread_yield(); // Yield to allow all actors to load before modifiying ropes

	while (ropes_left > 0){
		int next = random() % NROPES;
		if (ropes[next].rp_cut == false){
			lock_acquire(ropes[next].rp_lock);

			if (ropes[next].rp_cut == true){
				// Exit if the rope's condition changed after obtaining the lock.
				lock_release(ropes[next].rp_lock);
				continue;
			}

			int old_stake = ropes[next].rp_stake;
			int new_stake = random() % NROPES + 1;
 			while (new_stake == old_stake){
				new_stake = random() % NROPES + 1;
			}
			ropes[next].rp_stake = new_stake;

			kprintf("Lord FlowerKiller switched rope %d from stake %d to stake %d\n", ropes[next].rp_hook, old_stake, new_stake);
			lock_release(ropes[next].rp_lock);
			thread_yield();
		}
	}
	lock_acquire(threads_lk);

	threads_sleep++;
	cv_wait(threads_cv, threads_lk);

	kprintf("Lord FlowerKiller thread done\n");
	threads_left--;

	lock_release(threads_lk);

	thread_exit();
}

static
void
balloon(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Balloon thread starting\n");

	lock_acquire(threads_lk);

	threads_sleep++;
	cv_wait(threads_cv, threads_lk);

	kprintf("Balloon freed and Prince Dandelion escapes!\n");
	kprintf("Balloon thread done\n");
	threads_left--;

	lock_release(threads_lk);

	thread_exit();
}


// Change this function as necessary
int
airballoon(int nargs, char **args)
{
	//TODO: Implement non-random item selection, but rather by array index
	//TODO: Clean up locks kfree()
	//TODO: Delete the global char aray -> malloc it's ass
	//TODO: The balloon cv seems to not be working properly. Look into this...
	//TODO: Lock the threads with spinlocks rather than sleepers

	(void)nargs;
	(void)args;

	int err = 0;

	ropes_left_lk = lock_create("");
	threads_lk = lock_create("");
	threads_cv = cv_create("");

	for (int i = 0; i < NROPES; i++){
		ropes[i].rp_lock = lock_create("");
		ropes[i].rp_stake = i + 1;
		ropes[i].rp_hook = i + 1;
		ropes[i].rp_cut = false;
	}

	err = thread_fork("Marigold Thread",
			  NULL, marigold, NULL, 0);
	if(err)
		goto panic;

	err = thread_fork("Dandelion Thread",
			  NULL, dandelion, NULL, 0);
	if(err)
		goto panic;

	err = thread_fork("Lord FlowerKiller Thread",
			  NULL, flowerkiller, NULL, 0);
	if(err)
		goto panic;

	err = thread_fork("Air Balloon",
			  NULL, balloon, NULL, 0);
	if(err)
		goto panic;

	goto done;
panic:
	panic("airballoon: thread_fork failed: %s)\n",
	      strerror(err));

done:
	while(threads_sleep != 4){
		thread_yield(); // Yield until all actors are waiting
	}

	lock_acquire(threads_lk);
	cv_broadcast(threads_cv, threads_lk);
	lock_release(threads_lk);

	while(threads_left != 1){
		thread_yield(); // Yield until all actors finish
	}

	// Reset all variables to allow for multiple airballoon calls
	ropes_left = NROPES;
	threads_left = NTHREADS;
	threads_sleep = NSLEEP;

	// Clean up before exit
	lock_destroy(ropes_left_lk);
	lock_destroy(threads_lk);
	cv_destroy(threads_cv);
	for (int i = 0; i < NROPES; i++){
		lock_destroy(ropes[i].rp_lock);
	}


	kprintf("Main thread done\n");
	return 0;
}
