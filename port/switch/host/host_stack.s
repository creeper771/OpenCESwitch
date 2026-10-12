/*
HOST_STACK.S

void host_call_on_stack(uint64_t argument, void (*function)(uint64_t), void *stack_top)

Calls function(argument) on another stack. Guest (ILP32) code keeps stack
addresses in 32-bit registers, so it must run on a stack below 4 GB, while
libnx gives each thread its stack in the stack region, above 4 GB. The
caller's stack pointer is kept on the new stack and restored on return.

The frame pointer is cleared for the call, so that the chain of frame
records ends at the guest's outermost frame: the game's stack walker
(source/cseries/stack_walk_windows.c) reads frame pointers as 32 bits, and
would take the host's, above 4 GB, for an address below it.
*/

	.text
	.global host_call_on_stack
	.type host_call_on_stack, %function
	.balign 16
host_call_on_stack:
	stp x29, x30, [sp, #-16]!
	mov x29, sp
	mov x3, sp
	and x2, x2, #~15
	mov sp, x2
	sub sp, sp, #16
	str x3, [sp]
	mov x29, xzr
	blr x1
	ldr x3, [sp]
	mov sp, x3
	ldp x29, x30, [sp], #16
	ret
	.size host_call_on_stack, . - host_call_on_stack
