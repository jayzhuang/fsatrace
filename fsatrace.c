#include <sys/types.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>
#ifdef _MSC_VER
#include <io.h>
#define ssize_t size_t
#define basename(x) strrchr(x, '\\')+1
#else
#include <unistd.h>
#include <libgen.h>
#endif

#include "fsatrace.h"
#include "shm.h"
#include "proc.h"

static void
errv(const char *fmt, const char *pref, va_list ap)
{
	char		fullpath  [PATH_MAX];
	procPath(fullpath);
	fprintf(stderr, "%s(%d): %s", basename(fullpath), getpid(), pref);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	fflush(stderr);
}

static void
error(const char *fmt,...)
{
	va_list		ap;
	va_start(ap, fmt);
	errv(fmt, "error: ", ap);
	va_end(ap);
}

static void
fatal(const char *fmt,...)
{
	va_list		ap;
	va_start(ap, fmt);
	errv(fmt, "fatal error: ", ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

static void
slurp(char *p, size_t sz, const char *path)
{
	int		fd;
	ssize_t		r = 0;
	fd = open(path, O_RDONLY);
	if (fd >= 0)
		r = read(fd, p, sz);
	if (r)
		p[r] = 0;
	if (fd >= 0)
		close(fd);
}

static void
dump(const char *path, char *p, size_t sz)
{
	int		fd;
	ssize_t		r = 0;
	if (strcmp(path, "-"))
		fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0777);
	else
		fd = 1;
	if (fd >= 0)
		r = write(fd, p, sz);
	else
		error("Unable to open output file '%s'", path);
	if (r != sz)
		error("Short write (%d/%d)", r, sz);
	if (fd != 1)
		close(fd);
}

static void
uniq(char *d, size_t * tot, const char *s, const char *last, size_t lastsz)
{
	const char     *end = strchr(s, '\n');
	size_t		sz = end - s;
	if (!end) {
		strcpy(d, s);
		*tot += strlen(s);
	} else if (sz != lastsz || strncmp(s, last, lastsz)) {
		memcpy(d, s, sz + 1);
		*tot += sz + 1;
		uniq(d + sz + 1, tot, end + 1, s, sz);
	} else
		uniq(d, tot, end + 1, last, lastsz);
}

static void
dumpargs(char *dst, size_t sz, int n, char *const *l)
{
	int		i;
	size_t		sofar = 0;
	for (i = 0; i < n; i++)
		sofar += snprintf(dst + sofar, sz - sofar, "\nargv[%d]=%s", i, l[i]);
}

static unsigned
lines(char *s, char **args)
{
	unsigned	nargs = 0;
	int		c;
	char           *start = s;
	while ((c = *s++))
		if (c == '\n') {
			args[nargs++] = start;
			s[-1] = 0;
			start = s;
		}
	return nargs;
}

int
main(int argc, char *const argv[])
{
	int		err;
	int		rc = EXIT_FAILURE;
	const char     *out;
	struct shm	shm;
	size_t		sz = 0;
	char		envout    [PATH_MAX];
	static char	buf [LOGSZ];
	char           *const *args = argv + 3;
	unsigned	nargs = argc - 3;
	if (argc < 4 || (strcmp(argv[2], "--") && strcmp(argv[2], "---")))
		fatal(" usage: %s <output> -- <cmdline>", argv[0]);
	out = argv[1];
	if ((err = shmInit(&shm, out, LOGSZ, 1)))
		fatal("allocating shared memory (%d)", err);
	snprintf(envout, sizeof(envout), ENVOUT "=%s", out);
	putenv(envout);

	if (argv[3][0] == '@') {
		const int	MAXARGS = 0x8000;
		size_t		bsz = sizeof(buf) - MAXARGS * sizeof(char *);
		slurp(buf, bsz, argv[3] + 1);
		args = (char *const *)(buf + bsz);
		nargs = lines(buf, (char **)(buf + bsz));
	}
	switch (procRun(nargs, args, &rc)) {
	case ERR_PROC_FORK:
		dumpargs(buf, sizeof(buf), nargs, args);
		error("forking process:%s", buf);
		break;
	case ERR_PROC_EXEC:
		dumpargs(buf, sizeof(buf), nargs, args);
		error("executing command:%s", buf);
		break;
	case ERR_PROC_WAIT:
		dumpargs(buf, sizeof(buf), nargs, args);
		error("waiting for command completion:%s", buf);
		break;
	default:
		if (rc) {
			dumpargs(buf, sizeof(buf), nargs, args);
			error("command failed with code %d:%s", rc, buf);
		} else if (strcmp(argv[2], "---")) {
			uniq(buf, &sz, shm.buf + 4, "", 0);
			dump(out, buf, sz);
		} else
			dump(out, shm.buf + 4, *(uint32_t *) shm.buf);

	}
	if ((err = shmTerm(&shm, 1)))
		error("freeing shared memory (%d)", err);
	return rc;
}
