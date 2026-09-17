#include <stdio.h>
#include <signal.h>

int main(int argc, char **argv)
{
    int byte;

    (void)argv;
    byte = getchar();
    if (argc == 1 && byte != 'A') {
        raise(SIGKILL);
    }
    return 0;
}
