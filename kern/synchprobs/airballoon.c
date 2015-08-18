/*
 * Copyright (c) 2001, 2002, 2009
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
 * Driver code for whale mating problem
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
