#include <stdio.h>
#include <syscall.h>

int main(void)
{
    printf("Hello World from TheApp (ring3)\n");

    const char* const argv[] = { "TheShell", NULL };
    const char* const envp[] = { NULL };
    int exec_rc = sys_execve("/bin/TheShell", argv, envp);
    if (exec_rc != 0)
    {
        printf("[TheApp] execve('/bin/TheShell') failed rc=%d\n", exec_rc);
        return 1;
    }

    return 0;
}
