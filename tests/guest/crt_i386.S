/* Startup code and system call stubs for 32-bit Intel guest test programs.
 *
 * Mac OS X starts a program with esp pointing at argc, followed by argv. A
 * system call puts its number in eax and runs `int $0x80`, with arguments on
 * the stack above a return address, exactly where a called function finds
 * them. The carry flag reports failure, with the error number in eax. */

        .text
        .globl start
start:
        movl %esp, %eax
        andl $-16, %esp
        subl $8, %esp           /* keeps the stack 16-byte aligned at the call */
        leal 4(%eax), %edx
        pushl %edx              /* argv */
        pushl (%eax)            /* argc */
        call _rt_start
        hlt

/* s32 sys_write(s32 fd, const void* buf, u32 n) */
        .globl _sys_write
_sys_write:
        movl $4, %eax
        int $0x80
        jnc 1f
        negl %eax
1:      ret

/* s32 sys_call(u32 number, u32 a, u32 b, u32 c): any BSD system call. The
 * arguments are copied to where the kernel looks, above a return-address
 * slot. */
        .globl _sys_call
_sys_call:
        pushl 16(%esp)          /* c */
        pushl 16(%esp)          /* b */
        pushl 16(%esp)          /* a */
        pushl $0
        movl 20(%esp), %eax     /* number */
        int $0x80
        jnc 1f
        negl %eax
1:      addl $16, %esp
        ret

/* void sys_exit(s32 code) */
        .globl _sys_exit
_sys_exit:
        movl $1, %eax
        int $0x80
        hlt
