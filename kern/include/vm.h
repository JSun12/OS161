/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
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

#ifndef _VM_H_
#define _VM_H_

/*
 * VM system-related definitions.
 *
 * You'll probably want to add stuff here.
 */


#include <machine/vm.h>

/* Fault-type arguments to vm_fault() */
#define VM_FAULT_READ        0    /* A read was attempted */
#define VM_FAULT_WRITE       1    /* A write was attempted */
#define VM_FAULT_READONLY    2    /* A write to a readonly page was attempted*/

#define COREMAP_PAGES        4    /* Pages used for coremap */
#define NUM_PPAGES           COREMAP_PAGES*PAGE_SIZE/4    /* Number of page frames managed by coremap */  
#define PP_USED              0x80000000    /* Bit indicating if physical page unused */
#define KMALLOC_END          0x40000000    /* Bit indicating the last page of a kmalloc; used for kfree */
#define VP_MASK              0x000fffff    /* Mask to extract the virtual page of the page frame */

#define L_ONE_PT_NUM_ENTRIES 1024

/*
We do not need freelists for the coremap or l1 page tables. For coremap, we need used bit. 
For 
*/


/*
The coremap supports 16MB of physical RAM. Since cm_entry_t is a 32bit integer, 
there are 2^12 cm_entries. The physical pages are indexed from 0 until 2^12 - 1,
each identically corresponding to the indices of the cm_entries.

A coremap entry contains the corresponding vaddr page number and the process ID.
It contains a free bit indicating if the physical page is free. For kmalloc, there
is a kmalloc_end bit that is only set for the last page of a kmalloc. This is used 
during kfree.
*/
struct coremap {
    cm_entry_t cm_entries[NUM_PPAGES];
};


// struct l1_pt {
//     struct l1_entry l1_entries[L_ONE_PT_NUM_ENTRIES*sizeof(l1_entry)];
// };  

// /*
// An L1 page table entry has 2 four byte fields. The first uses 12 bits
// to map to the physical page, and 20 bits to map to the next free virtual 
// page in the virtual page free list. The second field uses 20 bits to 
// as an offset value, for the amount of contiguous virtual pages which are free, 
// or the number of contiguous pages which are allocated from kmalloc. 
// */
// struct l1_entry {
//     uint32_t page_refs; 
//     uint32_t status; 
// };


/* Initialization function */
void vm_bootstrap(void);

/* Fault handling function called by trap code */
int vm_fault(int faulttype, vaddr_t faultaddress);

/* Allocate/free kernel heap pages (called by kmalloc/kfree) */
vaddr_t alloc_kpages(unsigned npages);
void free_kpages(vaddr_t addr);

/* TLB shootdown handling called from interprocessor_interrupt */
void vm_tlbshootdown_all(void);
void vm_tlbshootdown(const struct tlbshootdown *);


#endif /* _VM_H_ */
