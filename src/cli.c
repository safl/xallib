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

int
main(int argc, char *argv[])
{
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

	xal_pp(xal);

	{
		uint64_t ino_offset = xal_get_inode_offset(xal, xal->rootino);
		struct xal_dir *dir;
		char buf[BUF_NBYTES] = {0};
		ssize_t nbytes;

		nbytes = pread(xal->handle.fd, buf, xal->sectsize, ino_offset);
		if (nbytes != xal->sectsize) {
			return EIO;
		}

		xal_dinode_pp(buf);

		printf("ino_offset: %zu\n", ino_offset);

		xal_dir_from_shortform(xal, buf, &dir);
	}

	return 0;
}