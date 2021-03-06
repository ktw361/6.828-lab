// implement fork from user space

#include <inc/string.h>
#include <inc/lib.h>

// PTE_COW marks copy-on-write page table entries.
// It is one of the bits explicitly allocated to user processes (PTE_AVAIL).
#define PTE_COW		0x800

//
// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
//
static void
pgfault(struct UTrapframe *utf)
{
	void *addr = (void *) utf->utf_fault_va;
	uint32_t err = utf->utf_err;
	int r;

	// Check that the faulting access was (1) a write, and (2) to a
	// copy-on-write page.  If not, panic.
	// Hint:
	//   Use the read-only page table mappings at uvpt
	//   (see <inc/memlayout.h>).

	// LAB 4: Your code here.
	if (!(uvpd[PDX(addr)] & PTE_P && uvpt[PGNUM(addr)] & PTE_P))
		panic("pgfault: page directory or pgae table not availible");
	if (!(err & FEC_WR))
		panic("pgfault: faulting access was not a write");
	if (!(uvpt[PGNUM(addr)] & PTE_COW))
		panic("pgfault: faulting access was not to a copy-on-write page");

	// Allocate a new page, map it at a temporary location (PFTEMP),
	// copy the data from the old page to the new page, then move the new
	// page to the old page's address.
	// Hint:
	//   You should make three system calls.

	// LAB 4: Your code here.

	addr = ROUNDDOWN(addr, PGSIZE); // Page-aligned is IMPORTANT!!!
	if ((r = sys_page_alloc(0, PFTEMP, 
				PTE_W|PTE_U|PTE_P)) < 0)
		panic("pgfault: %e at sys_page_alloc", r);
	memmove(PFTEMP, addr, PGSIZE);
	if ((r = sys_page_map(0, PFTEMP, 0, addr, PTE_W|PTE_U|PTE_P)) < 0)
		panic("pgfault: %e at sys_page_map", r);
	if ((r = sys_page_unmap(0, PFTEMP)) < 0)
		panic("pgfault: %e at sys_page_unmap", r);
}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  (Exercise: Why do we need to mark ours
// copy-on-write again if it was already copy-on-write at the beginning of
// this function?)
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
//
static int
duppage(envid_t envid, unsigned pn)
{
	int r;

	// LAB 4: Your code here.	
	void *addr = (void *)(pn * PGSIZE);
    
    // Share library state
	if (uvpt[pn] & PTE_SHARE) {
        if ((r = sys_page_map(0, addr, envid, addr, 
                        uvpt[pn]&PTE_SYSCALL)) < 0)
            panic("sys_page_map: %e", r);
        return 0;
    }

	// Read-only
	if (!(uvpt[pn] & PTE_W) && !(uvpt[pn] & PTE_COW)) {
		if ((r = sys_page_map(0, addr, envid, addr, PTE_U|PTE_P)) < 0)
			panic("duppage: %e when sys_page_map", r);
		return 0;
	}

	if (uvpt[pn] & PTE_W && uvpt[pn] & PTE_COW)
			panic("duppage: %x both copy-on-write and writable");

	if ((r = sys_page_map(0, addr, envid, addr, 
					PTE_COW|PTE_U|PTE_P)) < 0)
		panic("duppage: %e when sys_page_map", r);
	if ((r = sys_page_map(0, addr, 0, addr, 
					PTE_COW|PTE_U|PTE_P)) < 0)
		panic("duppage: %e when sys_page_map", r);
	return 0;
}

//
// User-level fork with copy-on-write.
// Set up our page fault handler appropriately.
// Create a child.
// Copy our address space and page fault handler setup to the child.
// Then mark the child as runnable and return.
//
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
// It is also OK to panic on error.
//
// Hint:
//   Use uvpd, uvpt, and duppage.
//   Remember to fix "thisenv" in the child process.
//   Neither user exception stack should ever be marked copy-on-write,
//   so you must allocate a new page for the child's user exception stack.
//
envid_t
fork(void)
{
	// LAB 4: Your code here.
	int r;
	envid_t envid;
	// Setup page fault handler
	set_pgfault_handler(pgfault);
	// Create a child
	envid = sys_exofork();
	if (envid < 0)
		panic("fork: %e at sys_exofork", envid);
	if (envid == 0) {
		thisenv = &envs[ENVX(sys_getenvid())];
		return 0;
	}
	// Copy address space 
	uintptr_t addr;
	for (addr = 0; addr < USTACKTOP; addr += PGSIZE) {
		if (!(uvpd[PDX(addr)] & PTE_P))
			continue;
		if (uvpt[PGNUM(addr)] & PTE_P && uvpt[PGNUM(addr)] & PTE_U)
			duppage(envid, PGNUM(addr));
	}
	// Set pgfault handler for child
	if ((r = sys_page_alloc(envid, (void *)(UXSTACKTOP - PGSIZE), 
				PTE_W | PTE_U | PTE_P)) < 0)
		panic("set_pgfault_handler: %e", r);
	extern void _pgfault_upcall(void);
	sys_env_set_pgfault_upcall(envid, _pgfault_upcall);
	// Set child status
	if ((r = sys_env_set_status(envid, ENV_RUNNABLE)) < 0)
		panic("fork: %e at sys_env_set_status", envid);
	return envid;
}

// 
// A shared version of duppage, for use in sfork
// keep COW still COW after mapping
//
static int
sharepage(envid_t envid, unsigned pn)
{
	int r;

	void *addr = (void *)(pn * PGSIZE);
	// Read-only
	if (!(uvpt[pn] & PTE_W)) {
		if ((r = sys_page_map(0, addr, envid, addr, PTE_U|PTE_P)) < 0)
			panic("sys_page_map: %e", r);
		return 0;
	}

	if (uvpt[pn] & PTE_W && uvpt[pn] & PTE_COW)
			panic("sharepage: %x both copy-on-write and writable");

    if (uvpt[pn] & PTE_COW) {
        if ((r = sys_page_map(0, addr, envid, addr, 
                        PTE_COW|PTE_U|PTE_P)) < 0)
            panic("sys_page_map: %e", r);
        if ((r = sys_page_map(0, addr, 0, addr, 
                        PTE_COW|PTE_U|PTE_P)) < 0)
            panic("sys_page_map: %e", r);
    } else {
        if ((r = sys_page_map(0, addr, envid, addr, 
                        PTE_W|PTE_U|PTE_P)) < 0)
            panic("sys_page_map: %e", r);
    } 
	return 0;
}

// Challenge!
int
sfork(void)
{
	int r;
	envid_t envid;
	// Setup page fault handler
	set_pgfault_handler(pgfault);
	// Create a child
	envid = sys_exofork();
	if (envid < 0)
		panic("fork: %e at sys_exofork", envid);
	if (envid == 0) {
		thisenv = &envs[ENVX(sys_getenvid())];
		return 0;
	}
	// Copy most address space 
	uintptr_t addr;
	for (addr = 0; addr < USTACKTOP - PGSIZE; addr += PGSIZE) {
		if (!(uvpd[PDX(addr)] & PTE_P))
			continue;
		if (uvpt[PGNUM(addr)] & PTE_P && uvpt[PGNUM(addr)] & PTE_U)
			sharepage(envid, PGNUM(addr));
	}
    // Copy-on-write stack
    assert(addr == USTACKTOP - PGSIZE);
    if (uvpt[PGNUM(addr)] & PTE_P && uvpt[PGNUM(addr)] & PTE_U)
        duppage(envid, PGNUM(addr));
    else
        panic("sfork: Stack area not available");
    

	// Set pgfault handler for child
	if ((r = sys_page_alloc(envid, (void *)(UXSTACKTOP - PGSIZE), 
				PTE_W | PTE_U | PTE_P)) < 0)
		panic("set_pgfault_handler: %e", r);
	extern void _pgfault_upcall(void);
	sys_env_set_pgfault_upcall(envid, _pgfault_upcall);
	// Set child status
	if ((r = sys_env_set_status(envid, ENV_RUNNABLE)) < 0)
		panic("fork: %e at sys_env_set_status", envid);
	return envid;
}
