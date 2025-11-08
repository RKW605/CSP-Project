#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "utils.h"

pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

// Print error and exit
void error_exit(const char *msg)
{
    perror(msg);
    exit(1);
}

// Thread-safe print function
void myPrint(const char *format, ...)
{
    va_list args;

    pthread_mutex_lock(&print_mutex);

    va_start(args, format);
    vprintf(format, args);   // Use vprintf to handle variable args
    va_end(args);

    fflush(stdout);

    pthread_mutex_unlock(&print_mutex);
}

// Detect if running in WSL
int is_running_in_wsl(void)
{
    // Check for the presence of the WSL environment variable
    return getenv("WSL_DISTRO_NAME") != NULL;
}

// Detect if running in Windows
int is_running_in_windows(void)
{
    #ifdef WIN_32
    return 1;
    #endif
    return 0;
}

// Clears the screen
void clear_screen(void)
{
    system("clear");
}