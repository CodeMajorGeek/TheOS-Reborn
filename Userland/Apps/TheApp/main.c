#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv, char** envp)
{
    (void) argc;
    (void) argv;
    (void) envp;

    char* const shell_argv[] = { "TheShell", NULL };
    int shell_rc = execv("/bin/TheShell", shell_argv);
    printf("[TheApp] execv('/bin/TheShell') failed rc=%d\n", shell_rc);
    return 1;
}
