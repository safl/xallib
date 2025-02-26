/**
 * Command-line interface goes here
 */
#define _GNU_SOURCE
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <libxal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <xal.h>

#define BUF_NBYTES 4096

void node_inspector(struct xal_inode *inode, void *cb_args)
{
	switch (inode->ftype) {
	case XAL_XFS_DIR3_FT_DIR:
		printf("# '%.*s'\n", inode->namelen, inode->name);
		break;

	case XAL_XFS_DIR3_FT_REG_FILE:
		printf("'%.*s'\n", inode->namelen, inode->name);
		break;
	}
}

int
main(int argc, char *argv[])
{
	struct xal_inode *index;
	struct xal *xal;
	int err;

	if (argc != 2) {
		printf("Invalid input; argc(%d)\n", argc);
		return EINVAL;
	}

	err = xal_open(argv[argc - 1], &xal);
	if (err < 0) {
		return -err;
	}

	err = xal_get_index(xal, &index);
	if (err) {
		goto exit;
	}

	err = xal_walk(index, node_inspector, NULL);
	if (err) {
		goto exit;
	}

exit:
	xal_close(xal);

	return -err;
}