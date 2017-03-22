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
#define VP_MASK              0x000fffff    /* Mask to extract the virtual page of the page frame */

#define PP_USED              0x80000000    /* Bit indicating if physical page unused */
#define KMALLOC_END          0x40000000    /* Bit indicating the last page of a kmalloc; used for kfree */
#define COPY_TO_WRITE        0x20000000    /* Bit indicating if page should be copied before writing */
#define CM_PID(pid)          ((pid) << 20)

#define NUM_L2PT_ENTRIES     PAGE_SIZE/4
#define NUM_L1PT_ENTRIES     PAGE_SIZE/4

#define L2_PAGE_NUM_MASK     0xffc00000    /* Mask to get the L2 virtual page number */
#define L1_PAGE_NUM_MASK     0x003ff000    /* Mask to get the L1 virtual page number */
#define L2_PNUM(vaddr)       (((vaddr) & L2_PAGE_NUM_MASK) >> 22)     
#define L1_PNUM(vaddr)       (((vaddr) & L1_PAGE_NUM_MASK) >> 12)

#define PAGE_MASK            0x000fffff    /* Mask to extract physical and virtual page from L1 and L2 PTEs */

/* Page table entry status bits */
#define ENTRY_VALID          0x80000000    
#define ENTRY_DIRTY          0x40000000
#define ENTRY_REFERENCE      0x20000000
#define ENTRY_READABLE       0x10000000
#define ENTRY_WRITABLE       0x08000000
#define ENTRY_EXECUTABLE     0x04000000

/*
The coremap supports 16MB of physical RAM, since cm_entry_t is a 4 bytes, 
and thus there are 2^12 cm_entries in 4 pages. However, we still use 20 bits 
to index the physical pages. The physical pages are indexed from 0 until 2^12 - 1,
each identically corresponding to the indices of the cm_entries.

A coremap entry contains the corresponding vaddr page number and the process ID 
if the address is a user address. It contains a free bit indicating if the physical 
page is free. For kmalloc, there is a kmalloc_end bit that is only set for the last 
page of a kmalloc. This is used during kfree.

The coremap supports copy on write. When a fork occurs, the l1 and l2 page of the original
process is turned into read only, and the child has an identical l2 page table. When either
the parent or child tries to write data, the corresponding tables must be changed to read write. 
The first process to try writing to a virtual page will copy the contents of the virtual page
to a different physical page, and switch the corresponding read only bit in the page table entry 
tor read write. However, to infrom the other process that the particular virtual page has been 
copied to a physical page, the coremap keeps the COPY_TO_WRITE bit, which indicates to a process 
that when it is set, it is necessary to copy the contents to another page. Thus, during a fork, 
this bit in the coremap is set in all physical pages corresponding to a virtual page. Note
that l2 page tables are also copied on write, and a "write" is where one of it's pages
are written to. 
*/
struct coremap {
    cm_entry_t cm_entries[NUM_PPAGES];
};


/*
The L2 page table is a page table for page tables for a single address space. 
Every entry is a 32 bit integer. The highest bit is the valid bit, indicating if the
corresponding L1 page table is in memory. If the entry is valid, the virtual page number
of the l1 page table is located on the lowers 20 bits (l1 will occupy a unique page, since it
is exactly 1 page in size).

Followed by the valid bit is the modify/dirty bit. 

Then is the reference bit. 

The following 3 bits are the protection bits, namely the readable, writable, exectuable bits.
*/
struct l2_pt {
    l2_entry_t l2_entries[NUM_L2PT_ENTRIES];
};


/*
The L1 page table is a page table mapping virtual pages to physical pages. Every 
entry is a 32 bit integer. The highest bit is the valid bit, indicating if the
virtual page is mapped to a physical page in memory. If the entry is valid, 
the physical page number is located on the lowers 20 bits.

Followed by the valid bit is the modify/dirty bit. 

Then is the reference bit. 

The following 3 bits are the protection bits, namely the readable, writable, exectuable bits.
*/
struct l1_pt {
    l1_entry_t l1_entries[NUM_L1PT_ENTRIES];
};


/* Global coremap functions */
void copy_to_write_set(p_page_t);

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
