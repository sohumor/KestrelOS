/* Tiny PIC shared object used by dynhello and as a linker smoke test. */

int shared_answer(void)
{
    return 42;
}

const char *shared_banner(void)
{
    return "dynhello: DT_NEEDED library returned 42\n";
}
