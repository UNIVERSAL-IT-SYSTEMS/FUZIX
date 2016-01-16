#include <kernel.h>
#include <printf.h>
#include <kdata.h>
#include "raspberrypi.h"
#include "externs.h"

extern uint8_t __bssstart;
extern uint8_t __bssend;
extern uint8_t __kerneltop;
extern uint8_t __vectors;
extern uint8_t __vectorsstart;
extern uint8_t __vectorsend;

#define MEGABYTE (1024*1024)
extern volatile uint32_t tlbtable[4096];

#define CACHED (1<<3)
#define BUFFERED (1<<2)

#define CR_M (1<<0) /* MMU on */
#define CR_A (1<<1) /* Strict alignment checking */
#define CR_C (1<<2) /* L1 data cache on */
#define CR_Z (1<<11) /* Branch flow prediction on */
#define CR_I (1<<12) /* L1 instruction cache on */

uaddr_t ramtop;

void set_tlb_entry(uint32_t virtual, uint32_t physical, uint32_t flags)
{
	int page = virtual / MEGABYTE;
	tlbtable[page] = physical | flags
		| (3<<10) /* AP = 3 (global access) */
		| (2<<0)  /* 1MB page */
		;
}

void jtag_init(void)
{
	gpio_set_pin_func(4, GPIO_FUNC_ALT5, GPIO_PULL_OFF); /* TDI */
	gpio_set_pin_func(22, GPIO_FUNC_ALT4, GPIO_PULL_OFF); /* TRST */
	gpio_set_pin_func(24, GPIO_FUNC_ALT4, GPIO_PULL_OFF); /* TDO */
	gpio_set_pin_func(25, GPIO_FUNC_ALT4, GPIO_PULL_OFF); /* TCK */
	gpio_set_pin_func(27, GPIO_FUNC_ALT4, GPIO_PULL_OFF); /* TMS */
}

static void change_control_register(uint32_t set, uint32_t reset)
{
	uint32_t value = mrc(15, 0, 1, 0, 0);
	value |= set;
	value &= ~reset;
	mcr(15, 0, 1, 0, 0, value);
}

static void enable_mmu(void)
{
	mcr(15, 0, 3, 0,  0, 0x3); /* domain */
	mcr(15, 0, 2, 0,  0, tlbtable); /* tlb base register 0 */
	mcr(15, 0, 2, 0,  1, tlbtable); /* tlb base register 1 */

	change_control_register(CR_A, CR_M|CR_C|CR_I);

	invalidate_data_cache();
	invalidate_insn_cache();
	invalidate_tlb();

	change_control_register(CR_M|CR_C|CR_I, 0);
}

static inline void tlb_flush(void* address)
{
	mcr(15, 0, 8, 7, 1, address);
}

void pagemap_init(void)
{
	/* We use megabyte pages --- gross overkill for Fuzix, but it's easy.
	 * The kernel occupies page 0 and is mapped at 0. User processes occupy
	 * all the others, and each one is mapped at 0x80000000. */

	int i;
	for (i=2; i<PTABSIZE; i++)
		pagemap_add(i);

	/* Page 1 must be mapped *last*, so that it gets allocated first. */

	pagemap_add(1);
}

void map_init(void)
{
}

void program_vectors(uint16_t* pageptr)
{
}

void platform_discard(void)
{
}

void platform_init(uint8_t* atags)
{
	/* Copy the exception vectors to their final home. */

	memcpy(&__vectors, &__vectorsstart, &__vectorsend - &__vectorsstart);

	/* Wipe BSS. */

	memset(&__bssstart, 0, &__bssend - &__bssstart);

	/* Create a 1:1 TLB table and turn it on, so that our peripherals end up
	 * being in a known good location. */

	/* Kernel page 1:1 mapping */
	set_tlb_entry(0x00000000, 0x00000000, CACHED|BUFFERED);

	/* The 16 peripheral pages (we map them to the Pi2 virtual address, for
	 * simplicity) */
	for (int page=0; page<0x10; page++)
	{
		uint32_t offset = page * MEGABYTE;
		set_tlb_entry(0x3f000000|offset, 0x20000000|offset, 0);
	}

	/* ...and our user address space. */
	set_tlb_entry(0x80000000, 0x00100000, CACHED|BUFFERED);
	enable_mmu();

	/* Initialise system peripherals. */

	kmemaddblk(&__bssend, &__kerneltop - &__bssend);
	led_init();
	//jtag_init();
	tty_rawinit();

	/* Detect how much memory we have. */

	ramsize = mbox_get_arm_memory() / 1024;
	procmem = ramsize - 1024; /* reserve 1MB for the kernel */

	/* We're actually in a process (which will eventually exec init). We need
	 * to make sure that the udata pointer is preset to the right block. We've
	 * ensured that init will go in page 1, by means of the nasty logic in
	 * pagemap_init().
	 */

	udata_ptr = get_udata_for_page(1);
	memset(&udata, 0, sizeof(udata));
	udata.u_page = 1;

	/* And go! */

	fuzix_main();
}

/* Uget/Uput 32bit */

uint32_t ugetl(void *uaddr)
{
	if (!valaddr(uaddr, 4)) {
		return -1;
	}
	return *(uint32_t *)uaddr;

}

int uputl(uint32_t val, void *uaddr)
{
	if (!valaddr(uaddr, 4))
		return -1;
	*(uint32_t *)uaddr = val;
	return 0;
}

void trap_monitor(void)
{
	led_halt_and_blink(3);
}

void unexpected_handler_c(const char* reason, uint32_t address)
{
	kprintf("panic: unexpected %s at pc 0x%x\n", reason, address);
	for (;;);
}

void dabt_handler_c(void)
{
	uint32_t insn = (uint32_t)__builtin_return_address(0) - 8;
	uint32_t reason = mrc(15, 0, 5, 0, 0) & 0xf;
	uint32_t fault_addr = mrc(15, 0, 6, 0, 0);
	kprintf("abort for address %x at %x because %x\n", fault_addr, insn, reason);
	for (;;);
}

struct registers
{
	uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, sp, lr;
	uint32_t pc, spsr;
};

void dump_user_registers_c(struct registers* r)
{
	kprintf("User mode registers:\n"
	        "spsr: %x %c%c%c%c%c %c%c mode: %x\n"
	        " r0: %x  r4: %x  r8: %x r12: %x\n"
			" r1: %x  r5: %x  r9: %x  sp: %x\n"
			" r2: %x  r6: %x r10: %x  lr: %x\n"
			" r3: %x  r7: %x r11: %x  pc: %x\n",
			r->spsr,
			(r->spsr & (1<<31)) ? 'N' : 'n',
			(r->spsr & (1<<30)) ? 'Z' : 'z',
			(r->spsr & (1<<29)) ? 'C' : 'c',
			(r->spsr & (1<<28)) ? 'V' : 'v',
			(r->spsr & (1<<27)) ? 'Q' : 'q',
			(r->spsr & (1<<7)) ? 'i' : 'I',
			(r->spsr & (1<<6)) ? 'f' : 'F',
			r->spsr & 0x1f,
			r->r0, r->r4, r->r8, r->r12,
			r->r1, r->r5, r->r9, r->sp,
			r->r2, r->r6, r->r10, r->lr,
			r->r3, r->r7, r->r11, r->pc);
}

void print_r0(uint32_t r0)
{
	kprintf("<%x>\n", r0);
}

