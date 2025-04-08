/**
 * Command-line interface goes here
 */
#define _GNU_SOURCE
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <libxal.h>
#include <libxnvme.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xal_odf.h>

#define BUF_NBYTES 4096

struct xal_nodeinspector_stats {
	uint64_t ndirs;
	uint64_t nfiles;
};

struct xal_cli_args {
	int verbose;
	char *filename;
};

int
parse_args(int argc, char *argv[], struct xal_cli_args *args)
{
	args->verbose = 0;
	args->filename = NULL;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s [-v | --verbose] <filename>\n", argv[0]);
		return -EINVAL;
	}

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
			args->verbose = 1;
		} else if (args->filename == NULL) {
			args->filename = argv[i];
		} else {
			fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
			return -EINVAL;
		}
	}

	if (args->filename == NULL) {
		fprintf(stderr, "Error: Filename is required\n");
		return -EINVAL;
	}

	return 0;
}

void
node_inspector(struct xal_inode *inode, void *cb_args, int level)
{
	struct xal_nodeinspector_stats *stats = cb_args;

	switch (inode->ftype) {
	case XAL_ODF_DIR3_FT_DIR:
		stats->ndirs += 1;
		printf("%*s%.*s [dir]\n", level * 2, "", inode->namelen, inode->name);
		break;

	case XAL_ODF_DIR3_FT_REG_FILE:
		stats->nfiles += 1;
		printf("%*s%.*s [file]\n", level * 2, "", inode->namelen, inode->name);
		break;
	default:
		printf("# UNKNOWN\n");
		return;
	}
}

int
main(int argc, char *argv[])
{
	struct xal_cli_args args = {0};
	struct xal_nodeinspector_stats cb_args = {0};
	struct xal *xal;
	int err;

	err = parse_args(argc, argv, &args);
	if (err) {
		return err;
	}

	err = xal_open(argv[argc - 1], &xal);
	if (err < 0) {
		printf("xal_open(...); err(%d)\n", err);
		return -err;
	}

	if (args.verbose) {
		xal_pp(xal);
	}

	err = xal_dinodes_retrieve(xal);
	if (err) {
		printf("xal_dinodes_retrieve(...); err(%d)\n", err);
		return err;
	}

	err = xal_index(xal);
	if (err) {
		printf("xal_get_index(...); err(%d)\n", err);
		goto exit;
	}

	err = xal_walk(xal->root, args.verbose ? node_inspector : NULL,
		       args.verbose ? &cb_args : NULL);
	if (err) {
		printf("xal_walk(...); err(%d)\n", err);
		goto exit;
	}

	if (args.verbose) {
		printf("ndirs(%" PRIu64 "); nfiles(%" PRIu64 ")\n", cb_args.ndirs, cb_args.nfiles);
	}

exit:
	xal_close(xal);

	return -err;
}