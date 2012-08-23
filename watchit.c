/*
 * watchit.c - monitor and record successful open() calls
 *
 */

/*
 * The child command we run results in one or more processes that use the
 * open() wrappers to connect to us on our listening socket. We need to accept
 * and read from all these connections until the child we started exits. The
 * lines we read we stick in a hash, and when we are done reading we write the
 * hash lines to a file.
 */

#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#define _WITH_DPRINTF
#endif

#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/param.h>
#include <glib.h>
#include <getopt.h>
#include <libgen.h>
#include <fnmatch.h>

// #define DEBUG
#include "watchit.h"

// XXX use tmpnam()
#define SOCK_PATH	"/tmp/wi-sock"
#define SO_NAME		"libwatchit.so"
#define LISTEN_BACKLOG	10

#ifdef __linux__
extern char *program_invocation_name;
#else
const char *program_invocation_name;
#endif

typedef struct {
    FILE* fh;
    const char *cwd_prefix;
    const char *fn_glob;
} user_data;

static int
read_line(int fd, char *buf, int max_length, char terminator)
{
    int ch, n;
    char *p = buf;

    for (int i = 0; i < max_length; ++i) {
	n = read(fd, &ch, 1);
	if (n == 1) {
	    *p = ch;
	    if (ch == terminator)
		break;
	    ++p;
	} else if (n == 0) { /* EOF */
	    if (i == 0)
		return 0;
	    else
		break;
	} else /* Error */
	    return -1;
    }
    *p = '\0';
    return n;
}

static void
handler(int sig) {
    /* Do nothing */
}

static int
handle_children(pid_t child, int sock, GHashTable *ht)
{
    fd_set active_fd_set, read_fd_set;
    char buf[PATH_MAX];
    struct sockaddr_un clientname;
    struct sigaction sa;
    int rc = 0, done = 0;

    DPRINTF("reading from pid %d\n", child);

    FD_ZERO(&active_fd_set);
    FD_SET(sock, &active_fd_set);

    sigset_t emptyset, blockset;;
    sigemptyset(&blockset);
    sigaddset(&blockset, SIGCHLD);
    sigprocmask(SIG_BLOCK, &blockset, NULL);

    sa.sa_handler = handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);

    sigemptyset(&emptyset);

    for (;;) {
	int ready;
	read_fd_set = active_fd_set;
	ready = pselect(FD_SETSIZE, &read_fd_set, NULL, NULL, NULL, &emptyset);
	if (errno == EINTR) {
	    int status;
	    if (waitpid(child, &status, WNOHANG) < 0)
		handle_error("waitpid");
	    done = 1;
	}

	/* Service all the sockets with input pending */
	for (int i = 0; i < FD_SETSIZE; ++i)
	    if (FD_ISSET (i, &read_fd_set)) {
		if (i == sock) {
		    /* Connection request on original socket */
		    int newsock;
		    size_t size = sizeof(clientname);
		    newsock = accept(sock, (struct sockaddr *)&clientname, &size);
		    if (newsock < 0) {
			if (errno == EAGAIN)
			    continue;
		    	handle_error("accept");
		    }
		    FD_SET(newsock, &active_fd_set);
		} else {
		    int rc;
		    /* Data arriving on an already-connected socket */
		    rc = read_line(i, buf, sizeof(buf), '\n');
		    switch (rc) {
			case 0:
			    close(i);
			    FD_CLR(i, &active_fd_set);
			    break;
			case -1: /* Error */
			    break;
			default:
			    DPRINTF("rc=%d, buf=[%s]\n", rc,buf);
			    g_hash_table_insert(ht, g_strdup(buf), "1");
			    break;
		    }
		}
	    }

	if (done)
	    break;
    }
    return rc;
}

static void
free_key_value(gpointer key, gpointer value, gpointer udata)
{
    g_free(key);
}

static void
iterator(gpointer key, gpointer value, gpointer udata)
{
    user_data *ud = (user_data *)udata;
    char *name = (char *)key;

    /* Don't print if the glob doesn't match */
    if (ud->fn_glob != NULL && fnmatch(ud->fn_glob, name, 0))
	return;
    /* Prefix $PWD if required */
    if (ud->cwd_prefix != NULL && name[0] != '/') {
	fprintf(ud->fh, "%s/", ud->cwd_prefix);
    }
    fprintf(ud->fh, "%s\n", name);
}

/* Write results to output_path */
static int
write_results(GHashTable *ht, char *output_path, user_data *udata)
    {
    FILE *fh = fopen(output_path, "w");
    if (fh == NULL)
	return -1;
    else {
	udata->fh = fh;
	g_hash_table_foreach(ht, (GHFunc)iterator, udata);
	fclose(fh);
    }
    return 0;
}

static int
create_socket(char *sock_path)
{
    struct sockaddr_un my_addr;
    int sock, flags;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0)
	handle_error("socket");
    memset(&my_addr, 0, sizeof(struct sockaddr_un));

    my_addr.sun_family = AF_UNIX;
    strncpy(my_addr.sun_path, sock_path,
	    sizeof(my_addr.sun_path) - 1);

    /*
     * Mark socket non-blocking as accept() might block despite select()
     * returning with data to read. This is because the client might have died
     * after the select() returned.
     */
    flags = fcntl(sock, F_GETFL);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    if (bind(sock, (struct sockaddr *) &my_addr,
		sizeof(struct sockaddr_un)) < 0)
	handle_error("bind");

    if (listen(sock, LISTEN_BACKLOG) < 0)
	handle_error("listen");

    return (sock);
}

static void
usage(char *msg)
{
    if (msg)
	fprintf(stderr, "%s: %s\n", program_invocation_name, msg);
    fprintf(stderr, "Usage: %s [options] cmd args ...\n"
	    "\n"
	    "Options:\n"
	    "    --cwd                  Prefix relative path with current working directory\n"
	    "    --match <PATTERN>,-m   Match filenames using glob pattern PATTERN\n"
	    "    --output <PATH>,-o     Write output to PATH\n"
	    "                           Default: /dev/stdout\n"
	    "    --preload <PATH>       Specify path of preload library\n"
	    "                           Default: %s in program directory\n"
	    "    --socket <PATH>        Path stem of the listen socket\n"
	    "                           A dot and the pid of the process will be appended\n"
	    "                           to make the socket pathname unique\n"
	    "                           Default: %s\n"
	    "    --help,-h              This message\n"
	    , program_invocation_name, SO_NAME, SOCK_PATH);
    exit(64);
}

int
main(int argc, char *argv[])
{
    pid_t child;
    char *sock_path = SOCK_PATH;
    char *output_path = "/dev/stdout";
    char *preload_path;
    char *progdir;
    int sock;
    int c;
    user_data udata = { NULL, NULL, NULL };

#ifndef __linux_
    program_invocation_name = getprogname();
#endif
    progdir = strdup(program_invocation_name);
    if (progdir == NULL)
	handle_error("strdup(program_invocation_name)");

    /* Default library path */
    if (asprintf(&preload_path, "%s/%s", dirname(progdir), SO_NAME) < 0)
	handle_error("asprintf(preload_path)");

    enum {
	OPT_CWD, OPT_MATCH, OPT_OUTPUT, OPT_PRELOAD, OPT_SOCKET, OPT_HELP
    };
    static struct option long_options[] = {
	{"cwd",		no_argument,       NULL,  0 },
	{"match",	required_argument, NULL, 'm' },
	{"output",	required_argument, NULL, 'o' },
	{"preload",	required_argument, NULL,  OPT_PRELOAD },
	{"socket",	required_argument, NULL,  OPT_SOCKET },
	{"help",	no_argument,       NULL, 'h' },
	{NULL,		0,                 NULL,  0 }
    };

    for (;;) {
	int option_index = 0;
	c = getopt_long(argc, argv, "+m:o:h",
		long_options, &option_index);

	if (c == -1)
	    break;

	switch (c) {
	    case 0:
		switch (option_index) {
		    case OPT_CWD:
			udata.cwd_prefix = getcwd(NULL, 0); // XXX NULL
			break;
		    case OPT_PRELOAD:
			preload_path = optarg;
			break;
		    case OPT_SOCKET:
			sock_path = optarg;
			break;
		    default:
			usage("invalid option");
			break;
		}
	    case 'm':
		udata.fn_glob = optarg;
		break;
	    case 'o':
		output_path = optarg;
		break;
	    case 'h':
	    default:
		usage(NULL);
		break;
	}
    }

    argv += optind;
    argc -= optind;

    if (argc < 1)
	usage("not enough arguments");

    if (access(preload_path, R_OK) < 0) {
	char buf[PATH_MAX * 2];
	/* Print error message if we can't access the library */
	snprintf(buf, sizeof(buf), "%s: unable to read preload library %s",
	    program_invocation_name, preload_path);
	handle_error(buf);
    }

    /* Make the socket path unique among all processes currently running */
    if (asprintf(&sock_path, "%s.%d", sock_path, getpid()) < 0)
	handle_error("asprintf(sock_path)");

    /* Just in case */
    (void)unlink(sock_path);

    sock = create_socket(sock_path);
    GHashTable *ht;

    switch (child = fork()) {
	case 0: /* child */
	    setenv("LD_PRELOAD", preload_path, 1);
	    setenv("SOCK_PATH", sock_path, 1);
	    if (execvp(*argv, argv) < 0)
		handle_error("execvp");
	    break;
	case -1:
	    handle_error("fork");
	    break;
	default: /* parent */
	    ht = g_hash_table_new(g_str_hash, g_str_equal);
	    handle_children(child, sock, ht);
	    if (write_results(ht, output_path, &udata) < 0)
		handle_error("write_results");
	    /* Free and destroy the hash table */
	    g_hash_table_foreach(ht, free_key_value, NULL);
	    g_hash_table_destroy(ht);
	    break;
    }

    /* Clean up */
    (void)unlink(sock_path);

    // XXX exit with child's exit code
    exit(0);
}
