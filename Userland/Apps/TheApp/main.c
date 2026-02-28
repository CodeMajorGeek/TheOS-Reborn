#include <stdio.h>
#include <syscall.h>

int main(void)
{
    printf("Hello World from TheApp (ring3)\n");

    const char* const shell_argv[] = { "TheShell", NULL };
    const char* const shell_envp[] = { NULL };
    int shell_rc = sys_execve("/bin/TheShell", shell_argv, shell_envp);
    printf("[TheApp] execve('/bin/TheShell') failed rc=%d\n", shell_rc);
    return 1;
}
