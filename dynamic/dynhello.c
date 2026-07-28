/* No static libc: every external C call below is resolved from libgreet.so
 * by Kestrel's ELF64 dynamic linker before the first instruction runs. */

extern int shared_answer(void);
extern const char *shared_banner(void);

static long raw_syscall(long n, long a, long b, long c)
{
    register long rax __asm__("rax") = n;
    register long rdi __asm__("rdi") = a;
    register long rsi __asm__("rsi") = b;
    register long rdx __asm__("rdx") = c;
    __asm__ volatile("int $0x80"
                     : "+a"(rax)
                     : "D"(rdi), "S"(rsi), "d"(rdx)
                     : "rcx", "r11", "memory");
    return rax;
}

__attribute__((noreturn)) void _start(void)
{
    const char *msg = shared_banner();
    long len = 0;
    while (msg[len])
        len++;
    raw_syscall(1, 1, (long)msg, len);
    raw_syscall(0, shared_answer() == 42 ? 0 : 1, 0, 0);
    for (;;)
        __asm__ volatile("hlt");
}
