#define _GNU_SOURCE
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef BLOCK_SIZE
#define BLOCK_SIZE 512ULL
#endif

#define FIEMAP_ALLOC_SIZE (128 * 1024 * 1024ULL) // 128MB

static struct fiemap *fiemap = NULL;

static int
process_file(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
	int printed_header = 0;
	uint64_t start = 0;
	struct fiemap_extent *last;
	struct fiemap_extent *fe;
	uint32_t extent_capacity;
	uint32_t i;
	int fd;

	(void)sb;
	(void)ftwbuf;

	if (typeflag != FTW_F) {
		return 0;
	}

	fd = open(fpath, O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 0;
	}

	extent_capacity = (FIEMAP_ALLOC_SIZE - offsetof(struct fiemap, fm_extents)) /
			  sizeof(struct fiemap_extent);

	while (1) {
		fiemap->fm_start = start;
		fiemap->fm_length = ~0ULL;
		fiemap->fm_flags = 0;
		fiemap->fm_extent_count = extent_capacity;

		if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
			perror("ioctl(FS_IOC_FIEMAP)");
			break;
		}

		if (fiemap->fm_mapped_extents == 0) {
			break;
		}

		if (!printed_header) {
			printf("\"%s\":\n", fpath);
			printed_header = 1;
		}

		for (i = 0; i < fiemap->fm_mapped_extents; ++i) {
			uint64_t logical_start = 0;
			uint64_t physical_start = 0;
			uint64_t length_blocks = 0;
			uint64_t logical_end = 0;
			uint64_t physical_end = 0;

			fe = &fiemap->fm_extents[i];

			logical_start = fe->fe_logical / BLOCK_SIZE;
			physical_start = fe->fe_physical / BLOCK_SIZE;
			length_blocks = fe->fe_length / BLOCK_SIZE;

			logical_end = logical_start + length_blocks - 1;
			physical_end = physical_start + length_blocks - 1;

			printf("- [%llu, %llu, %llu, %llu]\n", (unsigned long long)logical_start,
			       (unsigned long long)logical_end, (unsigned long long)physical_start,
			       (unsigned long long)physical_end);
		}

		last = &fiemap->fm_extents[fiemap->fm_mapped_extents - 1];
		start = last->fe_logical + last->fe_length;

		if (last->fe_flags & FIEMAP_EXTENT_LAST) {
			break;
		}
	}

	close(fd);
	return 0;
}

int
main(int argc, char *argv[])
{
	const char *mountpoint = NULL;
	long nopenfd = 0;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <mountpoint>\n", argv[0]);
		return EXIT_FAILURE;
	}

	mountpoint = argv[1];

	fiemap = malloc(FIEMAP_ALLOC_SIZE);
	if (!fiemap) {
		perror("malloc");
		return EXIT_FAILURE;
	}

	nopenfd = sysconf(_SC_OPEN_MAX);
	if (nopenfd <= 0) {
		nopenfd = 64;
	}

	if (nftw(mountpoint, process_file, (int)nopenfd, FTW_PHYS) == -1) {
		perror("nftw");
		free(fiemap);
		return EXIT_FAILURE;
	}

	free(fiemap);
	return EXIT_SUCCESS;
}
