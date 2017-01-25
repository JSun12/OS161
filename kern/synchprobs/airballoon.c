/*
 * Driver code for airballoon problem
 */
#include <types.h>
#include <lib.h>
#include <thread.h>
#include <test.h>

#define NROPES 16
static int ropes_left = NROPES;

// Data structures for rope mappings

/* Implement this! */

// Synchronization primitives 

/* Implement this! */

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

	// Implement this function
}

static
void
marigold(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;
	
	kprintf("Marigold thread starting\n");

	// Implement this function
}

static
void
flowerkiller(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;
	
	kprintf("Lord FlowerKiller thread starting\n");

	// Implement this function
}

static
void
balloon(void *p, unsigned long arg)
{
	(void)p;
	(void)arg;
	
	kprintf("Balloon thread starting\n");

	// Implement this function
}


// Change this function as necessary
int
airballoon(int nargs, char **args)
{

	int err = 0;

	(void)nargs;
	(void)args;
	(void)ropes_left;
	
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
	return 0;
}
