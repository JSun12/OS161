/*
 * Driver code for airballoon problem
 */
#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>
#include <synch.h>
#include <airballoon.h>

static int ropes_left = NROPES;
static int threads_left = NTHREADS;
static int threads_sleep = NSLEEP;
static struct rope *ropes;

/*
 * Locks:
 *
 * The ropes_left_lk must be held by a thread to decrement ropes_left
 * once a rope is cut.
 *
 * The threads_lk is held by a thread upon sleeping or waking up
 * from the condition variable's waiting channel found in threads_cv.
 */
struct lock *ropes_left_lk;
struct lock *threads_lk;
struct cv *threads_cv;

/*
 * Prince Dandelion thread.
 *
 * Will attempt to cut a rope in the ropes array every time it is scheduled.
 * Upon successfully cutting a rope, it will decrement ropes_left and
 * deschedule iself.
 *
 * When there are no ropes left to cut, it will sleep on the threads_cv
 * condition variable. It will wake up once all the other actors are also
 * sleeping on the same channel and return it's finishing statement.
*/
static
void
dandelion(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Dandelion thread starting\n");
	/* Yield to allow all actors to load before modifiying ropes. */
	thread_yield();

	while (ropes_left > 0){
		int next = random() % NROPES;

		if (ropes[next].rp_cut == false){
			lock_acquire(ropes[next].rp_lock);

			if (ropes[next].rp_cut == true){
				/* Exit if the rope's condition changed after obtaining the lock. */
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

/*
* Princess Marigold thread.
*
* Will attempt to cut a rope in the ropes array every time it is scheduled.
* Upon successfully cutting a rope, it will decrement ropes_left and
* deschedule iself.
*
* When there are no ropes left to cut, it will sleep on the threads_cv
* condition variable. It will wake up once all the other actors are also
* sleeping on the same channel and return it's finishing statement.
*/
static
void
marigold(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Marigold thread starting\n");
	/* Yield to allow all actors to load before modifiying ropes. */
	thread_yield();

	while (ropes_left > 0){
		int next = random() % NROPES;

		if (ropes[next].rp_cut == false){
			lock_acquire(ropes[next].rp_lock);

			if (ropes[next].rp_cut == true){
				/* Exit if the rope's condition changed after obtaining the lock. */
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

/*
* Lord FlowerKiller thread.
*
* Will attempt to move a rope to a different stake every time it is scheduled.
* Upon successfully moving a rope, it will deschedule iself.
*
* When there are no ropes left to move, it will sleep on the threads_cv
* condition variable. It will wake up once all the other actors are also
* sleeping on the same channel and return it's finishing statement.
*/
static
void
flowerkiller(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;

	kprintf("Lord FlowerKiller thread starting\n");
	/* Yield to allow all actors to load before modifiying ropes. */
	thread_yield();

	while (ropes_left > 0){
		int next = random() % NROPES;

		if (ropes[next].rp_cut == false){
			lock_acquire(ropes[next].rp_lock);

			if (ropes[next].rp_cut == true){
				/* Exit if the rope's condition changed after obtaining the lock. */
				lock_release(ropes[next].rp_lock);
				continue;
			}

			int old_stake = ropes[next].rp_stake;
			int new_stake = random() % NROPES;
			/* Flowerkiller shouldn't switch a rope to the same stake. */
 			while (new_stake == old_stake){
				new_stake = random() % NROPES;
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

/*
* Balloon thread.
*
* Will sleep on the threads_cv condition variable until all ropes are cut.
* It will wake up once all the other actors are also sleeping on the same
* channel and return it's finishing statements.
*/
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

/*
* Main thread.
*
* Responsible for the creation of all other threads and variables. Will
* monitor the number of sleeping threads and ensure that all actors awake
* at the correct times.
*
* Will deallocate memory and reset variables before returning control to
* the operating system.
*/
int
airballoon(int nargs, char **args)
{
	(void)nargs;
	(void)args;

	int err = 0;
	ropes_left_lk = lock_create("");
	threads_lk = lock_create("");
	threads_cv = cv_create("");
	ropes = kmalloc(sizeof(struct rope) * NROPES);

	for (int i = 0; i < NROPES; i++){
		ropes[i].rp_lock = lock_create("");
		ropes[i].rp_stake = i;
		ropes[i].rp_hook = i;
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
		/* Yield until all other threads are sleeping. */
		thread_yield();
	}

	/*
	 * Wake up all sleeping threads to allow for return statments
	 * to print.
	 */
	lock_acquire(threads_lk);
	cv_broadcast(threads_cv, threads_lk);
	lock_release(threads_lk);

	while(threads_left != 1){
		/* Wait until all threads exit. */
		thread_yield();
	}

	/* Reset all variables to allow for multiple airballoon calls. */
	ropes_left = NROPES;
	threads_left = NTHREADS;
	threads_sleep = NSLEEP;

	lock_destroy(ropes_left_lk);
	lock_destroy(threads_lk);
	cv_destroy(threads_cv);
	for (int i = 0; i < NROPES; i++){
		lock_destroy(ropes[i].rp_lock);
	}
	kfree(ropes);

	kprintf("Main thread done\n");
	return 0;
}
