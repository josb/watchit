#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>

#ifdef DEBUG
#define DPRINTF(format, args...) fprintf(stderr, "debug: " format, ##args)
#else
#define DPRINTF(format, args...)
#endif

#define handle_error(msg) \
        do { perror(msg); exit(EXIT_FAILURE); } while (0)
