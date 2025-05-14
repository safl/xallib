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

struct xal_cli_args {
	bool bmap;
	bool find;
	bool meta;
	bool stats;
	char *mountpoint;
};

struct xal_nodeinspector_args {
	uint64_t ndirs;
	uint64_t nfiles;
	struct xal_cli_args *cli_args;
};

int
parse_args(int argc, char *argv[], struct xal_cli_args *args)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s [-b | --verbose] <mountpoint>\n", argv[0]);
		return -EINVAL;
	}

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--bmap") == 0) {
			args->bmap = 1;
		} else if (strcmp(argv[i], "--find") == 0) {
			args->find = 1;
		} else if (strcmp(argv[i], "--meta") == 0) {
			args->meta = 1;
		} else if (strcmp(argv[i], "--stats") == 0) {
			args->stats = 1;
		} else if (args->mountpoint == NULL) {
			args->mountpoint = argv[i];
		} else {
			fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
			return -EINVAL;
		}
	}

	if (args->mountpoint == NULL) {
		fprintf(stderr, "Error: Mountpoint is required\n");
		return -EINVAL;
	}

	return 0;
}

/**
 * Produces output on stdout similar to the output produced by running "find /mount/point"
 */
void
node_inspector_find(struct xal *XAL_UNUSED(xal), struct xal_inode *inode, void *cb_args,
		    int XAL_UNUSED(level))
{
	struct xal_nodeinspector_args *args = cb_args;

	switch (inode->ftype) {
	case XAL_ODF_DIR3_FT_DIR:
		args->ndirs += 1;
		break;

	case XAL_ODF_DIR3_FT_REG_FILE:
		args->nfiles += 1;
		break;
	default:
		printf("# UNKNOWN(%.*s)", inode->namelen, inode->name);
		return;
	}

	printf("%s", args->cli_args->mountpoint);
	if ((inode->parent) &&
	    (args->cli_args->mountpoint[strlen(args->cli_args->mountpoint) - 1] == '/')) {
		printf("/");
	}
	xal_inode_path_pp(inode);
	printf("\n");
}

void
node_inspector_bmap(struct xal *xal, struct xal_inode *inode, void *cb_args, int XAL_UNUSED(level))
{
	struct xal_nodeinspector_args *args = cb_args;

	switch (inode->ftype) {
	case XAL_ODF_DIR3_FT_DIR:
		args->ndirs += 1;
		break;

	case XAL_ODF_DIR3_FT_REG_FILE:
		args->nfiles += 1;
		break;
	default:
		printf("# UNKNOWN(%.*s)", inode->namelen, inode->name);
		return;
	}

	printf("'%s", args->cli_args->mountpoint);
	if ((inode->parent) &&
	    (args->cli_args->mountpoint[strlen(args->cli_args->mountpoint) - 1] == '/')) {
		printf("/");
	}
	xal_inode_path_pp(inode);
	printf("':");
	if (!inode->content.extents.count) {
		printf(" ~\n");
		return;
	}
	printf("\n");

	for (uint32_t i = 0; i < inode->content.extents.count; ++i) {
		struct xal_extent *extent = &inode->content.extents.extent[i];
		size_t fofz_begin, fofz_end, bofz_begin, bofz_end;

		fofz_begin = (extent->start_offset * xal->sb.blocksize) / 512;
		fofz_end = fofz_begin + (extent->nblocks * xal->sb.blocksize) / 512 - 1;
		bofz_begin = xal_fsbno_offset(xal, extent->start_block) / 512;
		bofz_end = bofz_begin + (extent->nblocks * xal->sb.blocksize) / 512 - 1;

		printf("- [%" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 "]\n", fofz_begin,
		       fofz_end, bofz_begin, bofz_end);
	}
}

int
main(int argc, char *argv[])
{
	struct xal_cli_args args = {0};
	struct xnvme_opts opts = {0};
	struct xal_nodeinspector_args cb_args;
	struct xnvme_dev *dev;
	struct xal *xal;
	int err;

	err = parse_args(argc, argv, &args);
	if (err) {
		return err;
	}

	xnvme_opts_set_defaults(&opts);

	dev = xnvme_dev_open(argv[argc - 1], &opts);
	if (!dev) {
		printf("xnvme_dev_open(...); err(%d)\n", errno);
		return -errno;
	}

	err = xal_open(dev, &xal);
	if (err < 0) {
		printf("xal_open(...); err(%d)\n", err);
		return -err;
	}

	if (args.meta) {
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

	if (args.bmap) {
		memset(&cb_args, 0, sizeof(cb_args));
		cb_args.cli_args = &args;

		err = xal_walk(xal, xal->root, node_inspector_bmap, &cb_args);
		if (err) {
			printf("xal_walk(.. node_visistor_bmap ..); err(%d)\n", err);
			goto exit;
		}
	}

	if (args.find) {
		memset(&cb_args, 0, sizeof(cb_args));
		cb_args.cli_args = &args;

		err = xal_walk(xal, xal->root, node_inspector_find, &cb_args);
		if (err) {
			printf("xal_walk(.. node_visistor_find ..); err(%d)\n", err);
			goto exit;
		}
	}

	if (args.stats) {
		printf("ndirs(%" PRIu64 "); nfiles(%" PRIu64 ")\n", cb_args.ndirs, cb_args.nfiles);
	}

exit:
	xal_close(xal);
	xnvme_dev_close(dev);

	return -err;
}
