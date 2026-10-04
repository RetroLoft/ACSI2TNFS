| Start of a small TOS program without the C library: own stack in the BSS,
| then _tos_main(basepage). The program gives back the rest of its memory.
	.text
	.globl	__start
__start:
	movel	%sp@(4),%a0
	lea	_stack_area+4096,%sp
	movel	%a0,%sp@-
	jsr	_tos_main
	clrw	%sp@-
	trap	#1			| Pterm0, in case tos_main returns
