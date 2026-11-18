/* Startup code and system call stubs for PowerPC guest test programs.
 *
 * Mac OS X starts a program with r1 pointing at argc, followed by argv. A
 * system call puts its number in r0 and runs `sc`; on success the kernel
 * skips the instruction after `sc`, on failure it runs it with the error
 * number in r3. */

        .text
        .globl _start
_start:
        lwz 3, 0(1)             /* argc */
        addi 4, 1, 4            /* argv */
        clrrwi 1, 1, 4          /* align the stack to 16 bytes */
        li 0, 0
        stwu 1, -64(1)
        stw 0, 0(1)             /* end of the frame chain */
        bl rt_start
        trap

/* s32 sys_write(s32 fd, const void* buf, u32 n) */
        .globl sys_write
sys_write:
        li 0, 4
        sc
        b 1f                    /* reached only when the call failed */
        blr
1:      neg 3, 3
        blr

/* s32 sys_call(u32 number, u32 a, u32 b, u32 c): any BSD system call. */
        .globl sys_call
sys_call:
        mr 0, 3
        mr 3, 4
        mr 4, 5
        mr 5, 6
        sc
        b 1f                    /* reached only when the call failed */
        blr
1:      neg 3, 3
        blr

/* void sys_exit(s32 code) */
        .globl sys_exit
sys_exit:
        li 0, 1
        sc
        trap
