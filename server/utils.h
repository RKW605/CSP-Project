#ifndef UTILS_H
#define UTILS_H

extern pthread_mutex_t print_mutex;

void error_exit(const char *msg);
void myPrint(const char *format, ...);
int is_running_in_wsl(void);
int is_running_in_windows(void);
void clear_screen(void);

#endif
