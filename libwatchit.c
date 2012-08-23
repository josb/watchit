/*
 * libwatchit.c - LD_PRELOAD library that traces successful open() calls
 */

#define __USE_GNU
#include <stdarg.h>
#include <fcntl.h>
#include <dlfcn.h>

// #define DEBUG
#include "watchit.h"

static int sock;

int open(const char *, int, ...) __attribute__ ((weak, alias("wrap_open")));
int __open(const char *, int, ...) __attribute__ ((weak, alias("wrap_open")));
int open64(const char *, int, ...) __attribute__ ((weak, alias("wrap_open64")));
int __open64(const char *, int, ...) __attribute__ ((weak, alias("wrap_open64")));

static int (*orig_open)(const char *, int, ...) = NULL;
static int (*orig_open64)(const char *, int, ...) = NULL;

static int
write_sock(int sock, const char *name)
{
    /*
     * Maybe use:
     * open:<name>
     * or
     * <pid>:open:<name>
     */
    DPRINTF("writing %s\n", name);
    if (write(sock, name, strlen(name)) < 0)
	return -1;
    if (write(sock, "\n", 1) < 0)
	return -1;

    return 0;
}

int
wrap_open(const char *name, int flags, ...)
{
    va_list args;
    mode_t mode;
    int rc;

    va_start(args, flags);
    mode = va_arg(args, mode_t);
    va_end(args);

    DPRINTF("calling libc open(%s, 0x%x, 0x%x)\n", name, flags, mode);

    rc = orig_open(name, flags, mode);
    if (rc >= 0)
	(void)write_sock(sock, name); // XXX

    return rc;
}

int
wrap_open64(const char *name, int flags, ...)
{
    va_list args;
    mode_t mode;
    int rc;

    va_start(args, flags);
    mode = va_arg(args, mode_t);
    va_end(args);

    DPRINTF("calling libc open64(%s, 0x%x, 0x%x)\n", name, flags, mode);

    rc = orig_open64(name, flags, mode);
    if (rc >= 0)
	(void)write_sock(sock, name); // XXX

    return rc;
}

void _init(void)
{
    char *p;

    p = getenv("SOCK_PATH");
    if (p == NULL)
	handle_error("SOCK_PATH not set");
    DPRINTF("sock_path=%s\n", p);

    struct sockaddr_un my_addr;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0)
	handle_error("socket");
    memset(&my_addr, 0, sizeof(struct sockaddr_un));

    my_addr.sun_family = AF_UNIX;
    strncpy(my_addr.sun_path, p,
	    sizeof(my_addr.sun_path) - 1);

    if (connect(sock, (const struct sockaddr *)&my_addr,  sizeof(my_addr.sun_path) - 1) < 0)
	handle_error("connect");

    orig_open = dlsym( ((void *) -1l), "open");
    if (orig_open < 0)
	handle_error("error: missing symbol open!");
    orig_open64 = dlsym( ((void *) -1l), "open64");
    if (orig_open64 < 0)
	handle_error("error: missing symbol open64!");
}
