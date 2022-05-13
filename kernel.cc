#include "kernel.hh"
#include "k-apic.hh"
#include "k-vmiter.hh"
#include <atomic>

// kernel.cc
//
//    This is the kernel.


// INITIAL PHYSICAL MEMORY LAYOUT
//
//  +-------------- Base Memory --------------+
//  v                                         v
// +-----+--------------------+----------------+--------------------+---------/
// |     | Kernel      Kernel |       :    I/O | App 1        App 1 | App 2
// |     | Code + Data  Stack |  ...  : Memory | Code + Data  Stack | Code ...
// +-----+--------------------+----------------+--------------------+---------/
// 0  0x40000              0x80000 0xA0000 0x100000             0x140000
//                                             ^
//                                             | \___ PROC_SIZE ___/
//                                      PROC_START_ADDR

#define PROC_SIZE 0x40000       // initial state only

proc ptable[NPROC];             // array of process descriptors
                                // Note that `ptable[0]` is never used.
proc* current;                  // pointer to currently executing proc

#define HZ 100                  // timer interrupt frequency (interrupts/sec)
static std::atomic<unsigned long> ticks; // # timer interrupts so far


// Memory state - see `kernel.hh`
physpageinfo physpages[NPAGES];


[[noreturn]] void schedule();
[[noreturn]] void run(proc* p);
void exception(regstate* regs);
uintptr_t syscall(regstate* regs);
void memshow();


// kernel_start(command)
//    Initialize the hardware and processes and start running. The `command`
//    string is an optional string passed from the boot loader.

static void process_setup(pid_t pid, const char* program_name);

void kernel_start(const char* command) {
    // initialize hardware
    init_hardware();
    log_printf("Starting WeensyOS\n");

    ticks = 1;
    init_timer(HZ);

    // clear screen
    console_clear();

    // (re-)initialize kernel page table
    for (vmiter it(kernel_pagetable);
         it.va() < PROC_START_ADDR;
         it += PAGESIZE) {
        if (it.va() == 0){
            it.map(it.va(), 0);
        }
        else if (it.va() != CONSOLE_ADDR) {
            it.map(it.va(), PTE_P | PTE_W);
        } else {
            it.map(it.va(), PTE_P| PTE_W| PTE_U);
        }
    }

    // set up process descriptors
    for (pid_t i = 0; i < NPROC; i++) {
        ptable[i].pid = i;
        ptable[i].state = P_FREE;
    }
    if (command && !program_image(command).empty()) {
        process_setup(1, command);
    } else {
        process_setup(1, "allocator");
        process_setup(2, "allocator2");
        process_setup(3, "allocator3");
        process_setup(4, "allocator4");
    }

    // Switch to the first process using run()
    run(&ptable[1]);
}


// kalloc(sz)
//    Kernel physical memory allocator. Allocates at least `sz` contiguous bytes
//    and returns a pointer to the allocated memory, or `nullptr` on failure.
//    The returned pointer’s address is a valid physical address, but since the
//    WeensyOS kernel uses an identity mapping for virtual memory, it is also
//    a valid virtual address that the kernel can access or modify.
//
//    The allocator selects from physical pages that can be allocated for
//    process use (so not reserved pages or kernel data), and from physical
//    pages that are currently unused (so `physpages[I].refcount == 0`).
//
//    On WeensyOS, `kalloc` is a page-based allocator: if `sz > PAGESIZE`
//    the allocation fails; if `sz < PAGESIZE` it allocates a whole page
//    anyway.
//
//    The handout code returns the next allocatable free page it can find.
//    It checks all pages. (You could maybe make this faster!)
//
//    The returned memory is initially filled with 0xCC, which corresponds to
//    the x86 instruction `int3`. This may help you debug.

void* kalloc(size_t sz) {
    if (sz > PAGESIZE) {
        return nullptr;
    }
    for (uintptr_t pa = 0; pa != MEMSIZE_PHYSICAL; pa += PAGESIZE) {
        if (allocatable_physical_address(pa)
            && physpages[pa / PAGESIZE].refcount == 0) {
            ++physpages[pa / PAGESIZE].refcount;
            memset((void*) pa, 0xCC, PAGESIZE);
            return (void*) pa;
        }
    }
    return nullptr;
}


// kfree(kptr)
//    Free `kptr`, which must have been previously returned by `kalloc`.
//    If `kptr == nullptr` does nothing.

void kfree(void* kptr) {
    if (!kptr){
        return;
    }
    --physpages[(uintptr_t)kptr/ PAGESIZE].refcount;
}

// process_setup(pid, program_name)
//    Load application program `program_name` as process number `pid`.
//    This loads the application's code and data into memory, sets its
//    %rip and %rsp, gives it a stack page, and marks it as runnable.

void process_setup(pid_t pid, const char* program_name) {
    init_process(&ptable[pid], 0);

    // initialize process page table
    ptable[pid].pagetable = kalloc_pagetable();

    auto pit = vmiter(ptable[pid].pagetable);

    for (vmiter kit(kernel_pagetable); kit.va() < PROC_START_ADDR; kit+=PAGESIZE){
        pit.map(kit.pa(), kit.perm());
        pit+=PAGESIZE;
    }
    // obtain reference to the program image
    program_image pgm(program_name);

    // allocate and map global memory required by loadable segments
    for (auto seg = pgm.begin(); seg != pgm.end(); ++seg) {
        for (uintptr_t a = round_down(seg.va(), PAGESIZE);
             a < seg.va() + seg.size();
             a += PAGESIZE) {
            // `a` is the process virtual address for the next code or data page
            // (The handout code requires that the corresponding physical
            // address is currently free.)
            void* pa = kalloc(PAGESIZE);
            if (!pa){
                panic("Out of memory!");
            }
            pit.find(a);
            if (seg.writable()){
                pit.map((uintptr_t) pa, PTE_W| PTE_P | PTE_U);
            }
            else{
                pit.map((uintptr_t) pa, PTE_P | PTE_U);
            }
        }
    }

    // initialize data in loadable segments
    for (auto seg = pgm.begin(); seg != pgm.end(); ++seg) {
        pit.find(seg.va());
        memset((void*) pit.pa(), 0, seg.size());
        memcpy((void*) pit.pa(), seg.data(), seg.data_size());
    }

    // mark entry point
    ptable[pid].regs.reg_rip = pgm.entry();

    // allocate and map stack segment
    // Compute process virtual address for stack page
    uintptr_t stack_addr = MEMSIZE_VIRTUAL - PAGESIZE;
    void* pa = kalloc(PAGESIZE);
    if (!pa){
        panic("Out of memory!");
    }
    pit.find(stack_addr);
    pit.map((uintptr_t) pa, PTE_W| PTE_P| PTE_U);
    ptable[pid].regs.reg_rsp = stack_addr + PAGESIZE;
    // mark process as runnable
    ptable[pid].state = P_RUNNABLE;
}



// exception(regs)
//    Exception handler (for interrupts, traps, and faults).
//
//    The register values from exception time are stored in `regs`.
//    The processor responds to an exception by saving application state on
//    the kernel's stack, then jumping to kernel assembly code (in
//    k-exception.S). That code saves more registers on the kernel's stack,
//    then calls exception().
//
//    Note that hardware interrupts are disabled when the kernel is running.

void exception(regstate* regs) {
    // Copy the saved registers into the `current` process descriptor.
    current->regs = *regs;
    regs = &current->regs;

    // It can be useful to log events using `log_printf`.
    // Events logged this way are stored in the host's `log.txt` file.
    //log_printf("proc %d: exception %d at rip %p\n", current->pid, regs->reg_intno, regs->reg_rip);

    // Show the current cursor location and memory state
    // (unless this is a kernel fault).
    console_show_cursor(cursorpos);
    if (regs->reg_intno != INT_PF || (regs->reg_errcode & PTE_U)) {
        memshow();
    }

    // If Control-C was typed, exit the virtual machine.
    check_keyboard();

    // Actually handle the exception.
    switch (regs->reg_intno) {

    case INT_IRQ + IRQ_TIMER:
        ++ticks;
        lapicstate::get().ack();
        schedule();
        break;                  /* will not be reached */

    case INT_PF: {
        // Analyze faulting address and access type.
        uintptr_t addr = rdcr2();
        const char* operation = regs->reg_errcode & PTE_W
                ? "write" : "read";
        const char* problem = regs->reg_errcode & PTE_P
                ? "protection problem" : "missing page";

        if (!(regs->reg_errcode & PTE_U)) {
            panic("Kernel page fault on %p (%s %s)!\n",
                  addr, operation, problem);
        }
        console_printf(CPOS(24, 0), 0x0C00,
                       "Process %d page fault on %p (%s %s, rip=%p)!\n",
                       current->pid, addr, operation, problem, regs->reg_rip);
        current->state = P_FAULTED;
        break;
    }
    default:
        panic("Unexpected exception %d!\n", regs->reg_intno);

    }
    // Return to the current process (or run something else).
    if (current->state == P_RUNNABLE) {
        run(current);
    } else {
        schedule();
    }
}


// syscall(regs)
//    System call handler.
//
//    The register values from system call time are stored in `regs`.
//    The return value, if any, is returned to the user process in `%rax`.
//
//    Note that hardware interrupts are disabled when the kernel is running.

int syscall_page_alloc(uintptr_t addr);
int syscall_fork();
void syscall_exit(proc* process);
uintptr_t syscall(regstate* regs) {
    // Copy the saved registers into the `current` process descriptor.
    current->regs = *regs;
    regs = &current->regs;

    // It can be useful to log events using `log_printf`.
    // Events logged this way are stored in the host's `log.txt` file.
    /* log_printf("proc %d: syscall %d at rip %p\n",
                  current->pid, regs->reg_rax, regs->reg_rip); */

    // Show the current cursor location and memory state.
    console_show_cursor(cursorpos);
    memshow();

    // If Control-C was typed, exit the virtual machine.
    check_keyboard();


    // Actually handle the exception.
    switch (regs->reg_rax) {

        case SYSCALL_PANIC:
            user_panic(current);    // does not return

        case SYSCALL_GETPID:
            return current->pid;

        case SYSCALL_YIELD:
            current->regs.reg_rax = 0;
            schedule();             // does not return

        case SYSCALL_PAGE_ALLOC:
            return syscall_page_alloc(current->regs.reg_rdi);

        case SYSCALL_FORK:
            return syscall_fork();

        case SYSCALL_EXIT:
            syscall_exit(current);
            schedule();         //does not return

    default:
        panic("Unexpected system call %ld!\n", regs->reg_rax);

    }

    panic("Should not get here!\n");
}

//syscall_exit(process)
//  Handles the SYSCALL_EXIT system call. Frees all entries of process's pagetables,
//  along with the tables themselves and sets the process free.
void syscall_exit(proc* process){
    x86_64_pagetable* pt = process->pagetable;
    for (vmiter it(pt); it.va() < MEMSIZE_VIRTUAL; it+= PAGESIZE){
        if (it.user() && it.va() != CONSOLE_ADDR){
            kfree((void*)it.pa());
        }
    }
    for (ptiter it(pt); !it.done(); it.next()){
        kfree((void*) it.pa());
    }
    kfree(pt);

    process->state = P_FREE;
}

//syscall_fork()
//  Handles the SYSCALL_FORK system call. Creates a child process, allocates
//  a new page table with copies of any user writable pages from the parent,
//  copies parent's register state (except for rax which is set to 0), sets process to RUNNABLE state
//  and returns child pid on success, -1 on failure (out of memory or no more processes can be created)
int syscall_fork(){
    size_t pid = 0;
    for (size_t i =1; i< NPROC; ++i){
        if (ptable[i].state == P_FREE){
            pid = i;
            break;
        }
    }
    if (pid == 0){
        return -1;
    }
    ptable[pid].pagetable = kalloc_pagetable();
    if (!ptable[pid].pagetable){
        return -1;
    }
    auto cit = vmiter(ptable[pid].pagetable);
    for (auto pit = vmiter(current->pagetable); pit.va() < MEMSIZE_VIRTUAL; pit+=PAGESIZE){
        if (pit.va() <PROC_START_ADDR){
            int r = cit.try_map(pit.pa(), pit.perm());
            if (r){
                syscall_exit(&ptable[pid]);
                return -1;
            }
        }
        else{
            if (pit.writable() && pit.user()){
                void* pa = kalloc(PAGESIZE);
                if (!pa){
                    syscall_exit(&ptable[pid]);
                    return -1;
                }
                memcpy(pa, (void*) pit.pa(), PAGESIZE);
                int r = cit.try_map((uintptr_t) pa, pit.perm());
                if (r){
                    syscall_exit(&ptable[pid]);
                    return -1;
                }
            }
            else if (!pit.writable() && pit.user()){
                int r = cit.try_map(pit.pa(), pit.perm());
                if (r){
                    syscall_exit(&ptable[pid]);
                    return -1;
                }
                if (pit.pa() != (uint64_t) -1){
                    ++physpages[pit.pa()/PAGESIZE].refcount;
                }
            }
            else{
                int r = cit.try_map(pit.pa(), pit.perm());
                if (r){
                    syscall_exit(&ptable[pid]);
                    return -1;
                }
            }
        }
        cit+=PAGESIZE;
    }
    ptable[pid].regs = current->regs;
    ptable[pid].regs.reg_rax = 0;
    ptable[pid].pid = pid;
    ptable[pid].state = P_RUNNABLE;

    return pid;
}
// syscall_page_alloc(addr)
//    Handles the SYSCALL_PAGE_ALLOC system call. This function
//    should implement the specification for `sys_page_alloc`
//    in `u-lib.hh` (but in the handout code, it does not).
// sys_page_alloc(addr)
//    Allocate a page of memory at address `addr`. The newly-allocated
//    memory is initialized to 0. Any memory previously located at `addr`
//    should be freed. Returns 0 on success and -1 on failure (out of
//    memory or invalid argument).
//
//    `Addr` should be page-aligned (i.e., a multiple of PAGESIZE == 4096),
//    >= PROC_START_ADDR, and < MEMSIZE_VIRTUAL.
int syscall_page_alloc(uintptr_t addr) {
    if (addr < PROC_START_ADDR || addr >= MEMSIZE_VIRTUAL || addr % 4096 != 0){
        return -1;
    }
    auto pit = vmiter(current->pagetable);
    void* pa = kalloc(PAGESIZE);
    pit.find(addr);
    if (!pa){
        return -1;
    }
    int r = pit.try_map((uintptr_t)pa, PTE_P| PTE_W| PTE_U);
    if (r){
        return -1;
    }
    memset((void*) pit.pa(), 0, PAGESIZE);
    return 0;
}


// schedule
//    Pick the next process to run and then run it.
//    If there are no runnable processes, spins forever.

void schedule() {
    pid_t pid = current->pid;
    for (unsigned spins = 1; true; ++spins) {
        pid = (pid + 1) % NPROC;
        if (ptable[pid].state == P_RUNNABLE) {
            run(&ptable[pid]);
        }

        // If Control-C was typed, exit the virtual machine.
        check_keyboard();

        // If spinning forever, show the memviewer.
        if (spins % (1 << 12) == 0) {
            memshow();
            log_printf("%u\n", spins);
        }
    }
}


// run(p)
//    Run process `p`. This involves setting `current = p` and calling
//    `exception_return` to restore its page table and registers.

void run(proc* p) {
    assert(p->state == P_RUNNABLE);
    current = p;

    // Check the process's current pagetable.
    check_pagetable(p->pagetable);

    // This function is defined in k-exception.S. It restores the process's
    // registers then jumps back to user mode.
    exception_return(p);

    // should never get here
    while (true) {
    }
}


// memshow()
//    Draw a picture of memory (physical and virtual) on the CGA console.
//    Switches to a new process's virtual memory map every 0.25 sec.
//    Uses `console_memviewer()`, a function defined in `k-memviewer.cc`.

void memshow() {
    static unsigned last_ticks = 0;
    static int showing = 0;

    // switch to a new process every 0.25 sec
    if (last_ticks == 0 || ticks - last_ticks >= HZ / 2) {
        last_ticks = ticks;
        showing = (showing + 1) % NPROC;
    }

    proc* p = nullptr;
    for (int search = 0; !p && search < NPROC; ++search) {
        if (ptable[showing].state != P_FREE
            && ptable[showing].pagetable) {
            p = &ptable[showing];
        } else {
            showing = (showing + 1) % NPROC;
        }
    }

    console_memviewer(p);
    if (!p) {
        console_printf(CPOS(10, 29), 0x0F00, "VIRTUAL ADDRESS SPACE\n"
            "                          [All processes have exited]\n"
            "\n\n\n\n\n\n\n\n\n\n\n");
    }
}
