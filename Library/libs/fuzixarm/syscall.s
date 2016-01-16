
.text
.globl _syscall
_syscall:
		/* On entry, r4 has been pushed onto the stack --- we need to remember
		 * to restore it before exit. */

		/* Perform the system call. */

		swi 0
		pop {r4}

        /* On exit from the kernel, the result is in r0 and r1 is an errno. */

		cmp r1, #0
		bxeq lr

		/* Error path. */

		ldr r2, =errno
		str r1, [r2, #0]
		mov r0, #-1
		bx lr

