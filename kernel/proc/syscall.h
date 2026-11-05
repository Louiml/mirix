/* Syscall interface: syscall/sysret setup + the C dispatcher.
 * Numbers: 0=write(fd,buf,len) 1=exit(code) 2=gettid(). */
#ifndef MIRIX_PROC_SYSCALL_H
#define MIRIX_PROC_SYSCALL_H

#include <stdint.h>

#define SYS_write  0
#define SYS_exit   1
#define SYS_gettid 2

void syscall_init(void);

/* Called from syscall.asm. Args arrive in SysV order. Returns rax value. */
uint64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6,
                          uint64_t user_rip, uint64_t user_rflags, uint64_t user_rsp);

/* asm helpers */
void enter_usermode(uint64_t entry, uint64_t user_sp) __attribute__((noreturn));
extern uint64_t ring0_stack_top;   /* data symbol: top of the ring-0 IRQ stack */

#endif
