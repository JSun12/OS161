#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>
#include <spl.h>
#include <mips/tlb.h>
#include <wchan.h>

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 */

extern struct spinlock global;

extern struct cm *cm;
extern struct spinlock cm_spinlock;

extern struct wchan *io_wc; 
extern bool io_flag;

int
l1_create(struct l1_pt **l1_pt)
{
	struct l1_pt *l1_pt_new;
    l1_pt_new = kmalloc(sizeof(struct l1_pt));
    if (l1_pt_new == NULL) {
        return ENOMEM;
    }

	KASSERT(((vaddr_t) l1_pt_new) % PAGE_SIZE == 0);

    for (v_page_l1_t v_l1 = 0; v_l1 < NUM_L1PT_ENTRIES; v_l1++) {
        l1_pt_new->l1_entries[v_l1] = 0;
	}

	*l1_pt = l1_pt_new;
    return 0;
}

void
l2_init(struct l2_pt *l2_pt)
{
	for (v_page_l2_t v_l2 = 0; v_l2 < NUM_L2PT_ENTRIES; v_l2++) {
		l2_pt->l2_entries[v_l2] = 0;
	}
}

void
tlb_invalidate() {
	int spl = splhigh();
	uint32_t entryhi;
	uint32_t entrylo;
	uint32_t index;

	for (index = 0; index < NUM_TLB; index++) {
		tlb_read(&entryhi, &entrylo, index);
		// entrylo = entrylo & (~TLB_VALID_BIT);
		entrylo = 0;
		tlb_write(entryhi, entrylo, index);
	}

	splx(spl);
}

struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	as->as_lock = lock_create("lock");
	if (as->as_lock == NULL) {
		kfree(as);
		return NULL;
	}

	as->l2_pt = kmalloc(sizeof(struct l2_pt));
	if (as->l2_pt == NULL) {
		lock_destroy(as->as_lock);
		kfree(as);
		return NULL;
	}

	l2_init(as->l2_pt);
	as->heap_base = 0;
	as->stack_top = USERSTACK - STACK_SIZE; // TODO get proper stack
	as->brk = 0;

	return as;
}

int
as_copy(struct addrspace *old, struct addrspace **ret, pid_t pid)
{
	struct addrspace *newas;
	int result;

	/* The process for the new address space must already be in the pid table */
	struct proc *proc = get_pid(pid);
	KASSERT(proc != NULL);
	KASSERT(proc->pid == pid);
	KASSERT(proc->p_addrspace == *ret);

	newas = as_create();
	if (newas==NULL) {
		return ENOMEM;
	}

    spinlock_acquire(&global);

    while (io_flag) {
        wchan_sleep(io_wc, &global);
    }

	KASSERT(io_flag == false);

	struct l2_pt *l2_pt_new = newas->l2_pt;
	struct l2_pt *l2_pt_old = old->l2_pt;

	for (v_page_l2_t v_l2 = 0; v_l2 < NUM_L2PT_ENTRIES; v_l2++) {
		l2_pt_old->l2_entries[v_l2] = l2_pt_old->l2_entries[v_l2] & (~ENTRY_WRITABLE);

		if (l2_pt_old->l2_entries[v_l2] & ENTRY_VALID) {
			p_page_t p_page1 = l2_pt_old->l2_entries[v_l2] & PAGE_MASK;

			if (in_swap(p_page1)) {
				spinlock_acquire(&cm_spinlock);
				result = swap_in_l1(&p_page1);
				if (result) {
					spinlock_release(&cm_spinlock);
					spinlock_release(&global);
					return result;
				}
				spinlock_release(&cm_spinlock);            
			}

			spinlock_acquire(&cm_spinlock);

			cm_incref(p_page1); // TODO: maybe change this name p_page1
			add_pid8(p_page1, pid);
				
			spinlock_release(&cm_spinlock);

			struct l1_pt *l1_pt_old = (struct l1_pt *) PADDR_TO_KVADDR(PAGE_TO_ADDR(l2_pt_old->l2_entries[v_l2] & PAGE_MASK));

			for (v_page_l1_t v_l1 = 0; v_l1 < NUM_L1PT_ENTRIES; v_l1++) {
				l1_pt_old->l1_entries[v_l1] = l1_pt_old->l1_entries[v_l1] & (~ENTRY_WRITABLE);

				if (l1_pt_old->l1_entries[v_l1] & ENTRY_VALID) {
					p_page_t p_page = l1_pt_old->l1_entries[v_l1] & PAGE_MASK;

					spinlock_acquire(&cm_spinlock);

					cm_incref(p_page);
					add_pid8(p_page, pid);

					spinlock_release(&cm_spinlock);
				}
			}
		}

		l2_pt_new->l2_entries[v_l2] = l2_pt_old->l2_entries[v_l2];
	}

	newas->heap_base = old->heap_base;
	newas->stack_top = old->stack_top;
    newas->brk = old->brk;
	*ret = newas;

	// TODO: change the dirty bits of the correct process, not just invalidate all tlb entries

	// TODO: reduce code repetition (similar to as_activate)

	tlb_invalidate();

	spinlock_release(&global);
	return 0;
}

void
as_destroy(struct addrspace *as, pid_t pid)
{
	KASSERT(as != NULL); 
	/* The process for the new address space must already be in the pid table */
	struct proc *proc = get_pid(pid);
	KASSERT(proc != NULL);
	KASSERT(proc->pid == pid);

    spinlock_acquire(&global);
    while (io_flag) {
        wchan_sleep(io_wc, &global);
    }

    KASSERT(io_flag == false);

	struct l2_pt *l2_pt = as->l2_pt;
	int result;

	for (v_page_l2_t v_l2 = 0; v_l2 < NUM_L2PT_ENTRIES; v_l2++) {
		l2_entry_t l2_entry = l2_pt->l2_entries[v_l2];

		if (l2_entry & ENTRY_VALID) {
			p_page_t p_page1 = l2_entry & PAGE_MASK;

			if (in_swap(p_page1)) {
				spinlock_acquire(&cm_spinlock);
				result = swap_in_l1(&p_page1);
				if (result) {
					spinlock_release(&cm_spinlock);
					spinlock_release(&global);
					return;
				}
				spinlock_release(&cm_spinlock);            
			}

			struct l1_pt *l1_pt = (struct l1_pt *) PADDR_TO_KVADDR(PAGE_TO_ADDR(p_page1));

			for (v_page_l1_t v_l1 = 0; v_l1 < NUM_L1PT_ENTRIES; v_l1++) {
				l1_entry_t l1_entry = l1_pt->l1_entries[v_l1];

				if (l1_entry & ENTRY_VALID) {
					p_page_t p_page = l1_entry & PAGE_MASK;

					spinlock_acquire(&cm_spinlock);

					if (cm_getref(p_page) > 1) {
						cm_decref(p_page);
						rem_pid8(p_page, pid);
					} else {
						if (in_ram(p_page)) {
							free_ppage(p_page);
						} else {
							free_ppage_swap(p_page);
						}
					}

					spinlock_release(&cm_spinlock);
				}
			}

			spinlock_acquire(&cm_spinlock);

			if (cm_getref(p_page1) > 1) {
				cm_decref(p_page1);
				rem_pid8(p_page1, pid);
			} else {
				kfree(l1_pt);
			}

			spinlock_release(&cm_spinlock);
		}
	}

	lock_destroy(as->as_lock);
	kfree(as->l2_pt);
	kfree(as);

	// TODO: is this necessary
	tlb_invalidate();

	spinlock_release(&global);
}

void
as_activate(void)
{
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		return;
	}

    spinlock_acquire(&global);

	tlb_invalidate();

	spinlock_release(&global);
}

void
as_deactivate(void)
{
	/*
	 * Write this. For many designs it won't need to actually do
	 * anything. See proc.c for an explanation of why it (might)
	 * be needed.
	 */
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	/*
	This function is implemented assuming that as_define_region is called
	by load_elf to allocate space for user program code and static variables.
	Thus, the top of this region should be available for heap. This implementation
	finds this top.

	TODO: However, there might be more we should do to implement
	as_define_region properly.
	*/
	vaddr_t region_end = vaddr + sz;
	if (region_end > as->heap_base) {
		vaddr_t page_aligned_end = VPAGE_ADDR_MASK & region_end;
		page_aligned_end += PAGE_SIZE;

		as->heap_base = page_aligned_end;
		as->brk = as->heap_base;
	}

	(void)as;
	(void)vaddr;
	(void)sz;
	(void)readable;
	(void)writeable;
	(void)executable;
	return 0;
}

int
as_prepare_load(struct addrspace *as)
{
	/*
	 * Write this.
	 */

	(void)as;
	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	/*
	 * Write this.
	 */

	(void)as;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	/*
	 * Write this.
	 */

	(void)as;

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;

	return 0;
}
