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
extern struct lock *global_lock;
extern struct cv *global_cv;

extern struct cm *cm;
extern struct spinlock cm_spinlock;
extern size_t cm_counter;

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

	as->l2_pt = kmalloc(sizeof(struct l2_pt));
	if (as->l2_pt == NULL) {
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
	/* The process for the new address space must already be in the pid table */
	struct proc *proc = get_pid(pid);
	KASSERT(proc != NULL);
	KASSERT(proc->pid == pid);
	KASSERT(proc->p_addrspace == *ret);

	struct addrspace *newas;
	int result;

	newas = as_create();
	if (newas==NULL) {
		return ENOMEM;
	}

    lock_acquire(global_lock);

    while (!enough_free()) {
        cv_wait(global_cv, global_lock);
    }

	tlb_invalidate();	

	struct l2_pt *l2_pt_new = newas->l2_pt;
	struct l2_pt *l2_pt_old = old->l2_pt;
	struct l1_pt *l1_pt_old;

	for (v_page_l2_t v_l2 = 0; v_l2 < NUM_L2PT_ENTRIES; v_l2++) {
		l2_pt_old->l2_entries[v_l2] = l2_pt_old->l2_entries[v_l2] & (~ENTRY_WRITABLE);

		if (l2_pt_old->l2_entries[v_l2] & ENTRY_VALID) {
			result = get_l1_pt(l2_pt_old, v_l2, &l1_pt_old, false);
			if (result) {
				KASSERT(0); // debugging
				lock_release(global_lock);
				return result;
			}

			p_page_t p_page_l1 = ADDR_TO_PAGE(KVADDR_TO_PADDR((vaddr_t) l1_pt_old));

			spinlock_acquire(&cm_spinlock);

			cm_incref(p_page_l1);
			add_pid8(p_page_l1, pid);
				
			spinlock_release(&cm_spinlock);

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


	lock_release(global_lock);
	return 0;
}

void
as_destroy(struct addrspace *as, pid_t pid)
{
	KASSERT(as != NULL); 
	KASSERT(as->l2_pt != NULL);

	/* The process for the old address space must still be in the pid table */
	struct proc *proc = get_pid(pid);
	KASSERT(proc != NULL);
	KASSERT(proc->pid == pid);

    lock_acquire(global_lock);
	tlb_invalidate();

	struct l2_pt *l2_pt = as->l2_pt;
	struct l1_pt *l1_pt;
	int result;

	for (v_page_l2_t v_l2 = 0; v_l2 < NUM_L2PT_ENTRIES; v_l2++) {
		if (l2_pt->l2_entries[v_l2] & ENTRY_VALID) {
			result = get_l1_pt(l2_pt, v_l2, &l1_pt, false);
			if (result) {
				KASSERT(0); // debugging
				lock_release(global_lock);
				return;
			}

			for (v_page_l1_t v_l1 = 0; v_l1 < NUM_L1PT_ENTRIES; v_l1++) {
				l1_entry_t l1_entry = l1_pt->l1_entries[v_l1];

				if (l1_entry & ENTRY_VALID) {
					p_page_t p_page = l1_entry & PAGE_MASK;
					release_ppage(p_page, pid);
				}
			}

			release_ppage(ADDR_TO_PAGE(KVADDR_TO_PADDR((vaddr_t) l1_pt)), pid);
		}
	}

	kfree(as->l2_pt);
	kfree(as);

	lock_release(global_lock);
}

void
as_activate(void)
{
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		return;
	}

	tlb_invalidate();
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
