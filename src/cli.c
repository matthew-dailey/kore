/*
 * Copyright (c) 2014 Joris Vink <joris@coders.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/queue.h>
#include <sys/wait.h>
#include <sys/mman.h>

#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <libgen.h>
#include <inttypes.h>
#include <fcntl.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <utime.h>

#include "kore.h"

#if defined(OpenBSD) || defined(__FreeBSD_version) || \
    defined(NetBSD) || defined(__DragonFly_version)
#define PRI_TIME_T		"d"
#endif

#if defined(linux)
#if defined(__x86_64__)
#define PRI_TIME_T		PRIu64
#else
#define PRI_TIME_T		"ld"
#endif
#endif

#if defined(__MACH__)
#define PRI_TIME_T		"ld"
#endif

#define LD_FLAGS_MAX		10
#define CFLAGS_MAX		10

struct cmd {
	const char		*name;
	const char		*descr;
	void			(*cb)(int, char **);
};

struct filegen {
	void			(*cb)(void);
};

struct cfile {
	struct stat		st;
	int			build;
	int			cpp;
	char			*name;
	char			*fpath;
	char			*opath;
	TAILQ_ENTRY(cfile)	list;
};

TAILQ_HEAD(cfile_list, cfile);

static void		cli_fatal(const char *, ...) __attribute__((noreturn));

static void		cli_file_close(int);
static void		cli_run_kore(void *);
static void		cli_generate_certs(void);
static void		cli_link_library(void *);
static void		cli_compile_cfile(void *);
static void		cli_mkdir(const char *, int);
static int		cli_dir_exists(const char *);
static int		cli_file_exists(const char *);
static void		cli_cleanup_files(const char *);
static void		cli_file_writef(int, const char *, ...);
static void		cli_file_open(const char *, int, int *);
static void		cli_file_remove(char *, struct dirent *);
static void		cli_build_asset(char *, struct dirent *);
static void		cli_file_write(int, const void *, size_t);
static int		cli_vasprintf(char **, const char *, ...);
static void		cli_spawn_proc(void (*cb)(void *), void *);
static void		cli_write_asset(const char *, const char *);
static void		cli_register_cfile(char *, struct dirent *);
static void		cli_file_create(const char *, const char *, size_t);
static int		cli_file_requires_build(struct stat *, const char *);
static void		cli_find_files(const char *,
			    void (*cb)(char *, struct dirent *));
static void		cli_add_cfile(char *, char *, char *,
			    struct stat *, int, int);

static void		cli_run(int, char **);
static void		cli_help(int, char **);
static void		cli_build(int, char **);
static void		cli_clean(int, char **);
static void		cli_create(int, char **);

static void		file_create_src(void);
static void		file_create_config(void);
static void		file_create_gitignore(void);

static struct cmd cmds[] = {
	{ "help",	"this help text",			cli_help },
	{ "run",	"run an application (-fnr implied)",	cli_run },
	{ "build",	"build an application",			cli_build },
	{ "clean",	"cleanup the build files",		cli_clean },
	{ "create",	"create a new application skeleton",	cli_create },
	{ NULL,		NULL,					NULL }
};

static struct filegen gen_files[] = {
	{ file_create_src },
	{ file_create_config },
	{ file_create_gitignore },
	{ NULL }
};

static const char *gen_dirs[] = {
	"src",
#if !defined(KORE_NO_TLS)
	"cert",
#endif
	"conf",
	"assets",
	NULL
};

static const char *src_data =
	"#include <kore/kore.h>\n"
	"#include <kore/http.h>\n"
	"\n"
	"int\t\tpage(struct http_request *);\n"
	"\n"
	"int\n"
	"page(struct http_request *req)\n"
	"{\n"
	"\thttp_response(req, 200, NULL, 0);\n"
	"\treturn (KORE_RESULT_OK);\n"
	"}\n";

static const char *config_data =
	"# Placeholder configuration\n"
	"\n"
	"bind\t\t127.0.0.1 8888\n"
	"load\t\t./%s.so\n"
#if !defined(KORE_NO_TLS)
	"tls_dhparam\tdh2048.pem\n"
#endif
	"\n"
	"domain 127.0.0.1 {\n"
#if !defined(KORE_NO_TLS)
	"\tcertfile\tcert/server.crt\n"
	"\tcertkey\t\tcert/server.key\n"
#endif
	"\tstatic\t/\tpage\n"
	"}\n";

#if !defined(KORE_NO_TLS)
static const char *dh2048_data =
	"-----BEGIN DH PARAMETERS-----\n"
	"MIIBCAKCAQEAn4f4Qn5SudFjEYPWTbUaOTLUH85YWmmPFW1+b5bRa9ygr+1wfamv\n"
	"VKVT7jO8c4msSNikUf6eEfoH0H4VTCaj+Habwu+Sj+I416r3mliMD4SjNsUJrBrY\n"
	"Y0QV3ZUgZz4A8ARk/WwQcRl8+ZXJz34IaLwAcpyNhoV46iHVxW0ty8ND0U4DIku/\n"
	"PNayKimu4BXWXk4RfwNVP59t8DQKqjshZ4fDnbotskmSZ+e+FHrd+Kvrq/WButvV\n"
	"Bzy9fYgnUlJ82g/bziCI83R2xAdtH014fR63MpElkqdNeChb94pPbEdFlNUvYIBN\n"
	"xx2vTUQMqRbB4UdG2zuzzr5j98HDdblQ+wIBAg==\n"
	"-----END DH PARAMETERS-----";
#endif

static const char *gitignore_data = "*.o\n.objs\n%s.so\nassets.h\ncert\n";

static int			s_fd = -1;
static char			*appl = NULL;
static char			*rootdir = NULL;
static char			*compiler = "gcc";
static struct cfile_list	source_files;
static int			cfiles_count;
static struct cmd		*command = NULL;

void
kore_cli_usage(int local)
{
	int		i;

	if (local)
		fprintf(stderr, "Usage: kore [command]\n");

	fprintf(stderr, "\nAvailable commands:\n");
	for (i = 0; cmds[i].name != NULL; i++)
		printf("\t%s\t%s\n", cmds[i].name, cmds[i].descr);

	fprintf(stderr, "\nThe commands mostly exist for your convenience\n");
	fprintf(stderr, "when hacking on your Kore applications.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Production servers should be started using ");
	fprintf(stderr, "the options.\n");

	fprintf(stderr, "\nFind more information on https://kore.io\n");
	exit(1);
}

int
kore_cli_main(int argc, char **argv)
{
	int		i;

	if (argc < 1)
		kore_cli_usage(1);

	(void)umask(S_IWGRP|S_IWOTH);

	for (i = 0; cmds[i].name != NULL; i++) {
		if (!strcmp(argv[0], cmds[i].name)) {
			argc--;
			argv++;
			command = &cmds[i];
			cmds[i].cb(argc, argv);
			break;
		}
	}

	if (cmds[i].name == NULL) {
		fprintf(stderr, "No such command: %s\n", argv[0]);
		kore_cli_usage(1);
	}

	return (0);
}

static void
cli_help(int argc, char **argv)
{
	kore_cli_usage(1);
}

static void
cli_create(int argc, char **argv)
{
	int		i;
	char		*fpath;

	if (argc != 1)
		cli_fatal("missing application name");

	appl = argv[0];
	cli_mkdir(appl, 0755);
	rootdir = appl;

	for (i = 0; gen_dirs[i] != NULL; i++) {
		(void)cli_vasprintf(&fpath, "%s/%s", appl, gen_dirs[i]);
		cli_mkdir(fpath, 0755);
		free(fpath);
	}

	for (i = 0; gen_files[i].cb != NULL; i++)
		gen_files[i].cb();

	cli_generate_certs();

	printf("%s created successfully!\n", appl);

#if !defined(KORE_NO_TLS)
	printf("note: do NOT use the created DH parameters/certificates in production\n");
#endif
}

static void
cli_build(int argc, char **argv)
{
	struct cfile	*cf;
	struct timeval	times[2];
	int		requires_relink;
	char		pwd[PATH_MAX], *src_path, *assets_header;
	char		*assets_path, *p, *obj_path, *cpath, *config;

	if (argc == 0) {
		if (getcwd(pwd, sizeof(pwd)) == NULL)
			cli_fatal("could not get cwd: %s", errno_s);

		rootdir = ".";
		appl = basename(pwd);
	} else {
		appl = argv[0];
		rootdir = appl;
	}

	if ((p = getenv("CC")) != NULL)
		compiler = p;

	cfiles_count = 0;
	TAILQ_INIT(&source_files);

	(void)cli_vasprintf(&src_path, "%s/src", rootdir);
	(void)cli_vasprintf(&assets_path, "%s/assets", rootdir);
	(void)cli_vasprintf(&config, "%s/conf/%s.conf", rootdir, appl);
	(void)cli_vasprintf(&assets_header, "%s/src/assets.h", rootdir);
	if (!cli_dir_exists(src_path) || !cli_file_exists(config))
		cli_fatal("%s doesn't appear to be a kore app", appl);

	free(config);

	(void)cli_vasprintf(&obj_path, "%s/.objs", rootdir);
	if (!cli_dir_exists(obj_path))
		cli_mkdir(obj_path, 0755);
	free(obj_path);

	(void)unlink(assets_header);

	/* Generate the assets. */
	if (cli_dir_exists(assets_path)) {
		cli_file_open(assets_header,
		    O_CREAT | O_TRUNC | O_WRONLY, &s_fd);

		cli_file_writef(s_fd, "#ifndef __H_KORE_ASSETS_H\n");
		cli_file_writef(s_fd, "#define __H_KORE_ASSETS_H\n");
		cli_find_files(assets_path, cli_build_asset);
		cli_file_writef(s_fd, "\n#endif\n");
		cli_file_close(s_fd);
	}

	free(assets_path);

	/* Build all source files. */
	cli_find_files(src_path, cli_register_cfile);
	
	free(src_path);
	requires_relink = 0;

	TAILQ_FOREACH(cf, &source_files, list) {
		if (cf->build == 0)
			continue;

		printf("compiling %s\n", cf->name);
		cli_spawn_proc(cli_compile_cfile, cf);

		times[0].tv_usec = 0;
		times[0].tv_sec = cf->st.st_mtime;
		times[1] = times[0];

		if (utimes(cf->opath, times) == -1)
			printf("utime(%s): %s\n", cf->opath, errno_s);

		requires_relink++;
	}

	(void)unlink(assets_header);
	free(assets_header);

	(void)cli_vasprintf(&cpath, "%s/cert", rootdir);
	if (!cli_dir_exists(cpath)) {
		cli_mkdir(cpath, 0700);
		cli_generate_certs();
	}
	free(cpath);

	if (requires_relink) {
		cli_spawn_proc(cli_link_library, NULL);
		printf("%s built successfully!\n", appl);
	} else {
		printf("nothing to be done\n");
	}
}

static void
cli_clean(int argc, char **argv)
{
	char		pwd[PATH_MAX], *sofile;

	if (cli_dir_exists(".objs"))
		cli_cleanup_files(".objs");

	if (getcwd(pwd, sizeof(pwd)) == NULL)
		cli_fatal("could not get cwd: %s", errno_s);

	appl = basename(pwd);
	(void)cli_vasprintf(&sofile, "%s.so", appl);
	if (unlink(sofile) == -1 && errno != ENOENT)
		printf("couldn't unlink %s: %s", sofile, errno_s);

	free(sofile);
}

static void
cli_run(int argc, char **argv)
{
	cli_build(argc, argv);

	if (chdir(rootdir) == -1)
		cli_fatal("couldn't change directory to %s", rootdir);

	/*
	 * We are exec()'ing kore again, while we could technically set
	 * the right cli options manually and just continue running.
	 */
	cli_run_kore(NULL);
}

static void
file_create_src(void)
{
	char		*name;

	(void)cli_vasprintf(&name, "src/%s.c", appl);
	cli_file_create(name, src_data, strlen(src_data));
	free(name);
}

static void
file_create_config(void)
{
	int		l;
	char		*name, *data;

	(void)cli_vasprintf(&name, "conf/%s.conf", appl);
	l = cli_vasprintf(&data, config_data, appl);
	cli_file_create(name, data, l);

	free(name);
	free(data);
}

static void
file_create_gitignore(void)
{
	int		l;
	char		*data;

	l = cli_vasprintf(&data, gitignore_data, appl);
	cli_file_create(".gitignore", data, l);
	free(data);
}

static void
cli_mkdir(const char *fpath, int mode)
{
	if (mkdir(fpath, mode) == -1)
		cli_fatal("cli_mkdir(%s): %s", fpath, errno_s);
}

static int
cli_file_exists(const char *fpath)
{
	struct stat		st;

	if (stat(fpath, &st) == -1)
		return (0);

	if (!S_ISREG(st.st_mode))
		return (0);

	return (1);
}

static int
cli_file_requires_build(struct stat *fst, const char *opath)
{
	struct stat	ost;

	if (stat(opath, &ost) == -1) {
		if (errno == ENOENT)
			return (1);
		cli_fatal("stat(%s): %s", opath, errno_s);
	}

	return (fst->st_mtime != ost.st_mtime);
}

static int
cli_dir_exists(const char *fpath)
{
	struct stat		st;

	if (stat(fpath, &st) == -1)
		return (0);

	if (!S_ISDIR(st.st_mode))
		return (0);

	return (1);
}

static void
cli_file_open(const char *fpath, int flags, int *fd)
{
	if ((*fd = open(fpath, flags, 0644)) == -1)
		cli_fatal("cli_file_open(%s): %s", fpath, errno_s);
}

static void
cli_file_close(int fd)
{
	if (close(fd) == -1)
		printf("warning: close() %s\n", errno_s);
}

static void
cli_file_writef(int fd, const char *fmt, ...)
{
	int		l;
	char		*buf;
	va_list		args;

	va_start(args, fmt);
	l = vasprintf(&buf, fmt, args);
	va_end(args);

	if (l == -1)
		cli_fatal("cli_file_writef");

	cli_file_write(fd, buf, l);
	free(buf);
}

static void
cli_file_write(int fd, const void *buf, size_t len)
{
	ssize_t		r;
	const u_int8_t	*d;
	size_t		written;

	d = buf;
	written = 0;
	while (written != len) {
		r = write(fd, d + written, len - written);
		if (r == -1) {
			if (errno == EINTR)
				continue;
			cli_fatal("cli_file_write: %s", errno_s);
		}

		written += r;
	}
}

static void
cli_file_create(const char *name, const char *data, size_t len)
{
	int		fd;
	char		*fpath;

	(void)cli_vasprintf(&fpath, "%s/%s", rootdir, name);

	cli_file_open(fpath, O_CREAT | O_TRUNC | O_WRONLY, &fd);
	cli_file_write(fd, data, len);
	cli_file_close(fd);

	printf("created %s\n", fpath);
	free(fpath);
}

static void
cli_write_asset(const char *n, const char *e)
{
	cli_file_writef(s_fd, "extern u_int8_t asset_%s_%s[];\n", n, e);
	cli_file_writef(s_fd, "extern u_int32_t asset_len_%s_%s;\n", n, e);
	cli_file_writef(s_fd, "extern time_t asset_mtime_%s_%s;\n", n, e);
}

static void
cli_build_asset(char *fpath, struct dirent *dp)
{
	struct stat		st;
	u_int8_t		*d;
	off_t			off;
	void			*base;
	int			in, out;
	char			*cpath, *ext, *opath, *p, *name;

	name = kore_strdup(dp->d_name);

	/* Grab the extension as we're using it in the symbol name. */
	if ((ext = strrchr(name, '.')) == NULL)
		cli_fatal("couldn't find ext in %s", name);

	/* Replace dots, spaces, etc etc with underscores. */
	for (p = name; *p != '\0'; p++) {
		if (*p == '.' || isspace(*p) || *p == '-')
			*p = '_';
	}

	/* Grab inode information. */
	if (stat(fpath, &st) == -1)
		cli_fatal("stat: %s %s", fpath, errno_s);

	/* If this file was empty, skip it. */
	if (st.st_size == 0) {
		printf("skipping empty asset %s\n", name);
		return;
	}

	(void)cli_vasprintf(&opath, "%s/.objs/%s.o", rootdir, name);
	(void)cli_vasprintf(&cpath, "%s/.objs/%s.c", rootdir, name);

	/* Check if the file needs to be built. */
	if (!cli_file_requires_build(&st, opath)) {
		*(ext)++ = '\0';
		cli_write_asset(name, ext);
		*ext = '_';
		
		cli_add_cfile(name, cpath, opath, &st, 0, 0);
		kore_mem_free(name);
		return;
	}

	/* Open the file we're converting. */
	cli_file_open(fpath, O_RDONLY, &in);

	/* mmap our in file. */
	if ((base = mmap(NULL, st.st_size,
	    PROT_READ, MAP_PRIVATE, in, 0)) == MAP_FAILED)
		cli_fatal("mmap: %s %s", fpath, errno_s);

	/* Create the c file where we will write too. */
	cli_file_open(cpath, O_CREAT | O_TRUNC | O_WRONLY, &out);

	/* No longer need name so cut off the extension. */
	printf("building asset %s\n", dp->d_name);
	*(ext)++ = '\0';

	/* Start generating the file. */
	cli_file_writef(out, "/* Auto generated */\n");
	cli_file_writef(out, "#include <sys/param.h>\n\n");

	/* Write the file data as a byte array. */
	cli_file_writef(out, "u_int8_t asset_%s_%s[] = {\n", name, ext);
	d = base;
	for (off = 0; off < st.st_size; off++)
		cli_file_writef(out, "0x%02x,", *d++);

	/*
	 * Always NUL-terminate the asset, even if this NUL is not included in
	 * the actual length. This way assets can be cast to char * without
	 * any additional thinking for the developer.
	 */
	cli_file_writef(out, "0x00");

	/* Add the meta data. */
	cli_file_writef(out, "};\n\n");
	cli_file_writef(out, "u_int32_t asset_len_%s_%s = %" PRIu32 ";\n",
	    name, ext, (u_int32_t)st.st_size);
	cli_file_writef(out, "time_t asset_mtime_%s_%s = %" PRI_TIME_T ";\n",
	    name, ext, st.st_mtime);

	/* Write the file symbols into assets.h so they can be used. */
	cli_write_asset(name, ext);

	/* Cleanup static file source. */
	if (munmap(base, st.st_size) == -1)
		cli_fatal("munmap: %s %s", fpath, errno_s);

	/* Cleanup fds */
	cli_file_close(in);
	cli_file_close(out);

	/* Restore the original name */
	*--ext = '.';

	/* Register the .c file now (cpath is free'd later). */
	cli_add_cfile(name, cpath, opath, &st, 1, 0);
	kore_mem_free(name);
}

static void
cli_add_cfile(char *name, char *fpath, char *opath, struct stat *st,
    int build, int cpp)
{
	struct cfile		*cf;

	cfiles_count++;
	cf = kore_malloc(sizeof(*cf));

	cf->st = *st;
	cf->build = build;
	cf->cpp = cpp;
	cf->fpath = fpath;
	cf->opath = opath;
	cf->name = kore_strdup(name);

	TAILQ_INSERT_TAIL(&source_files, cf, list);
}

static void
cli_register_cfile(char *fpath, struct dirent *dp)
{
	struct stat		st;
	char			*ext, *opath;
	int			cpp;

	if ((ext = strrchr(fpath, '.')) == NULL ||
	    (strcmp(ext, ".c") && strcmp(ext, ".cpp")))
		return;

	if (!strcmp(ext, ".cpp"))
		cpp = 1;
	else
		cpp = 0;

	if (stat(fpath, &st) == -1)
		cli_fatal("stat(%s): %s", fpath, errno_s);

	(void)cli_vasprintf(&opath, "%s/.objs/%s.o", rootdir, dp->d_name);
	if (!cli_file_requires_build(&st, opath)) {
		cli_add_cfile(dp->d_name, fpath, opath, &st, 0, cpp);
		return;
	}

	cli_add_cfile(dp->d_name, fpath, opath, &st, 1, cpp);
}

static void
cli_file_remove(char *fpath, struct dirent *dp)
{
	if (unlink(fpath) == -1)
		fprintf(stderr, "couldn't unlink %s: %s", fpath, errno_s);
}

static void
cli_find_files(const char *path, void (*cb)(char *, struct dirent *))
{
	DIR			*d;
	struct stat		st;
	struct dirent		*dp;
	char			*fpath;

	if ((d = opendir(path)) == NULL)
		cli_fatal("cli_find_files: opendir(%s): %s", path, errno_s);

	while ((dp = readdir(d)) != NULL) {
		if (!strcmp(dp->d_name, ".") ||
		    !strcmp(dp->d_name, ".."))
			continue;

		(void)cli_vasprintf(&fpath, "%s/%s", path, dp->d_name);
		if (stat(fpath, &st) == -1) {
			fprintf(stderr, "stat(%s): %s\n", fpath, errno_s);
			free(fpath);
			continue;
		}

		if (S_ISDIR(st.st_mode)) {
			cli_find_files(fpath, cb);
			free(fpath);
		} else if (S_ISREG(st.st_mode)) {
			cb(fpath, dp);
		} else {
			fprintf(stderr, "ignoring %s\n", fpath);
			free(fpath);
		}
	}

	closedir(d);
}

static void
cli_generate_certs(void)
{
#if !defined(KORE_NO_TLS)
	BIGNUM			*e;
	FILE			*fp;
	time_t			now;
	X509_NAME		*name;
	EVP_PKEY		*pkey;
	X509			*x509;
	RSA			*kpair;
	char			*fpath, issuer[64];

	/* Write out DH parameters. */
	cli_file_create("dh2048.pem", dh2048_data, strlen(dh2048_data));

	/* Create new certificate. */
	if ((x509 = X509_new()) == NULL)
		cli_fatal("X509_new(): %s", ssl_errno_s);

	/* Generate version 3. */
	if (!X509_set_version(x509, 2))
		cli_fatal("X509_set_version(): %s", ssl_errno_s);

	/* Generate RSA keys. */
	if ((pkey = EVP_PKEY_new()) == NULL)
		cli_fatal("EVP_PKEY_new(): %s", ssl_errno_s);
	if ((kpair = RSA_new()) == NULL)
		cli_fatal("RSA_new(): %s", ssl_errno_s);
	if ((e = BN_new()) == NULL)
		cli_fatal("BN_new(): %s", ssl_errno_s);

	if (!BN_set_word(e, 65537))
		cli_fatal("BN_set_word(): %s", ssl_errno_s);
	if (!RSA_generate_key_ex(kpair, 2048, e, NULL))
		cli_fatal("RSA_generate_key_ex(): %s", ssl_errno_s);

	BN_free(e);

	if (!EVP_PKEY_assign_RSA(pkey, kpair))
		cli_fatal("EVP_PKEY_assign_RSA(): %s", ssl_errno_s);

	/* Set serial number to current timestamp. */
	time(&now);
	if (!ASN1_INTEGER_set(X509_get_serialNumber(x509), now))
		cli_fatal("ASN1_INTEGER_set(): %s", ssl_errno_s);

	/* Not before and not after dates. */
	if (!X509_gmtime_adj(X509_get_notBefore(x509), 0))
		cli_fatal("X509_gmtime_adj(): %s", ssl_errno_s);
	if (!X509_gmtime_adj(X509_get_notAfter(x509),
	    (long)60 * 60 * 24 * 3000))
		cli_fatal("X509_gmtime_adj(): %s", ssl_errno_s);

	/* Attach the pkey to the certificate. */
	if (!X509_set_pubkey(x509, pkey))
		cli_fatal("X509_set_pubkey(): %s", ssl_errno_s);

	/* Set certificate information. */
	if ((name = X509_get_subject_name(x509)) == NULL)
		cli_fatal("X509_get_subject_name(): %s", ssl_errno_s);

	(void)snprintf(issuer, sizeof(issuer), "kore autogen: %s", appl);
	if (!X509_NAME_add_entry_by_txt(name, "C",
	    MBSTRING_ASC, (const unsigned char *)"SE", -1, -1, 0))
		cli_fatal("X509_NAME_add_entry_by_txt(): C %s", ssl_errno_s);
	if (!X509_NAME_add_entry_by_txt(name, "O",
	    MBSTRING_ASC, (const unsigned char *)issuer, -1, -1, 0))
		cli_fatal("X509_NAME_add_entry_by_txt(): O %s", ssl_errno_s);
	if (!X509_NAME_add_entry_by_txt(name, "CN",
	    MBSTRING_ASC, (const unsigned char *)"localhost", -1, -1, 0))
		cli_fatal("X509_NAME_add_entry_by_txt(): CN %s", ssl_errno_s);

	if (!X509_set_issuer_name(x509, name))
		cli_fatal("X509_set_issuer_name(): %s", ssl_errno_s);

	if (!X509_sign(x509, pkey, EVP_sha256()))
		cli_fatal("X509_sign(): %s", ssl_errno_s);

	(void)cli_vasprintf(&fpath, "%s/cert/server.key", rootdir);
	if ((fp = fopen(fpath, "w")) == NULL)
		cli_fatal("fopen(%s): %s", fpath, errno_s);
	free(fpath);

	if (!PEM_write_PrivateKey(fp, pkey, NULL, NULL, 0, NULL, NULL))
		cli_fatal("PEM_write_PrivateKey(): %s", ssl_errno_s);
	fclose(fp);

	(void)cli_vasprintf(&fpath, "%s/cert/server.crt", rootdir);
	if ((fp = fopen(fpath, "w")) == NULL)
		cli_fatal("fopen(%s): %s", fpath, errno_s);
	free(fpath);

	if (!PEM_write_X509(fp, x509))
		cli_fatal("PEM_write_X509(%s)", errno_s);
	fclose(fp);

	EVP_PKEY_free(pkey);
	X509_free(x509);
#endif
}

static void
cli_compile_cfile(void *arg)
{
	int		idx, f, i;
	struct cfile	*cf = arg;
	char		*flags[CFLAGS_MAX], *p;
	char		*args[32 + CFLAGS_MAX], *ipath[2], *cppstandard;
#if defined(KORE_USE_PGSQL)
	char		*ppath;
#endif

	(void)cli_vasprintf(&ipath[0], "-I%s/src", rootdir);
	(void)cli_vasprintf(&ipath[1], "-I%s/src/includes", rootdir);

	idx = 0;
	args[idx++] = compiler;
	args[idx++] = ipath[0];
	args[idx++] = ipath[1];
#if defined(PREFIX)
	(void)cli_vasprintf(&args[idx++], "-I%s/include", PREFIX);
#else
	args[idx++] = "-I/usr/local/include";
#endif

#if defined(__MACH__)
	/* Add default openssl include path from homebrew / ports under OSX. */
	args[idx++] = "-I/opt/local/include";
	args[idx++] = "-I/usr/local/opt/openssl/include";
#endif

	/* Add any user specified flags. */
	if ((p = getenv("CFLAGS")) != NULL)
		f = kore_split_string(p, " ", flags, CFLAGS_MAX);
	else
		f = 0;

	for (i = 0; i < f; i++)
		args[idx++] = flags[i];

#if defined(KORE_USE_PGSQL)
	(void)cli_vasprintf(&ppath, "-I%s", PGSQL_INCLUDE_PATH);
	args[idx++] = ppath;
#endif

	args[idx++] = "-Wall";
	args[idx++] = "-Wmissing-declarations";
	args[idx++] = "-Wshadow";
	args[idx++] = "-Wpointer-arith";
	args[idx++] = "-Wcast-qual";
	args[idx++] = "-Wsign-compare";
	args[idx++] = "-fPIC";
	args[idx++] = "-g";

	if (cf->cpp) {
		args[idx++] = "-Woverloaded-virtual";
		args[idx++] = "-Wold-style-cast";
		args[idx++] = "-Wnon-virtual-dtor";

		if ((p = getenv("CXXSTD")) != NULL) {
			(void)cli_vasprintf(&cppstandard, "-std=%s", p);
			args[idx++] = cppstandard;
		}
	} else {
		args[idx++] = "-Wstrict-prototypes";
		args[idx++] = "-Wmissing-prototypes";
	}

	args[idx++] = "-c";
	args[idx++] = cf->fpath;
	args[idx++] = "-o";
	args[idx++] = cf->opath;
	args[idx] = NULL;

	execvp(compiler, args);
}

static void
cli_link_library(void *arg)
{
	struct cfile		*cf;
	int			idx, f, i, has_cpp;
	char			*args[cfiles_count + 11 + LD_FLAGS_MAX];
	char			*p, *libname, *flags[LD_FLAGS_MAX], *cpplib;

	if ((p = getenv("LDFLAGS")) != NULL)
		f = kore_split_string(p, " ", flags, LD_FLAGS_MAX);
	else
		f = 0;

	(void)cli_vasprintf(&libname, "%s/%s.so", rootdir, appl);

	idx = 0;
	args[idx++] = compiler;

#if defined(__MACH__)
	args[idx++] = "-dynamiclib";
	args[idx++] = "-undefined";
	args[idx++] = "suppress";
	args[idx++] = "-flat_namespace";
#else
	args[idx++] = "-shared";
#endif

	has_cpp = 0;
	TAILQ_FOREACH(cf, &source_files, list) {
		if (cf->cpp)
			has_cpp = 1;
		args[idx++] = cf->opath;
	}

	if (has_cpp) {
		if ((p = getenv("CXXLIB")) != NULL) {
			(void)cli_vasprintf(&cpplib, "-l%s", p);
			args[idx++] = cpplib;
		} else {
			args[idx++] = "-lstdc++";
		}
	}

	for (i = 0; i < f; i++)
		args[idx++] = flags[i];

	args[idx++] = "-o";
	args[idx++] = libname;
	args[idx] = NULL;

	execvp(compiler, args);
}

static void
cli_run_kore(void *arg)
{
	char		*args[4], *cpath;

	(void)cli_vasprintf(&cpath, "conf/%s.conf", appl);

	args[0] = "kore";
	args[1] = "-fnrc";
	args[2] = cpath;
	args[3] = NULL;

	execvp("kore", args);
}

static void
cli_spawn_proc(void (*cb)(void *), void *arg)
{
	pid_t		pid;
	int		status;

	pid = fork();
	switch (pid) {
	case -1:
		cli_fatal("cli_compile_cfile: fork() %s", errno_s);
		/* NOTREACHED */
	case 0:
		cb(arg);
		cli_fatal("cli_spawn_proc: %s", errno_s);
		/* NOTREACHED */
	default:
		break;
	}

	if (waitpid(pid, &status, 0) == -1)
		cli_fatal("couldn't wait for child %d", pid);

	if (WEXITSTATUS(status) || WTERMSIG(status) || WCOREDUMP(status))
		cli_fatal("subprocess trouble, check output");
}

static int
cli_vasprintf(char **out, const char *fmt, ...)
{
	int		l;
	va_list		args;

	va_start(args, fmt);
	l = vasprintf(out, fmt, args);
	va_end(args);

	if (l == -1)
		cli_fatal("cli_vasprintf");

	return (l);
}

static void
cli_cleanup_files(const char *spath)
{
	cli_find_files(spath, cli_file_remove);

	if (rmdir(spath) == -1 && errno != ENOENT)
		printf("couldn't rmdir %s\n", spath);
}

static void
cli_fatal(const char *fmt, ...)
{
	va_list		args;
	char		buf[2048];

	va_start(args, fmt);
	(void)vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (command != NULL)
		printf("kore %s: %s\n", command->name, buf);
	else
		printf("kore: %s\n", buf);

	exit(1);
}
