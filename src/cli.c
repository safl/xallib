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
	char *backend;
	char *dev_uri;
	bool filemap;
	char *file_name;
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
		fprintf(stderr, "Usage: %s [-b | --verbose] <dev_uri>\n", argv[0]);
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
		} else if (strcmp(argv[i], "--backend") == 0) {
			if (i+1 >= argc) {
				fprintf(stderr, "Error: Backend argument must define a valid backend (choices: xfs, fiemap)\n");
				return -EINVAL;
			}
			args->backend = argv[++i];
		} else if (strcmp(argv[i], "--filename") == 0) {
			if (i + 2 >= argc) {
				fprintf(stderr, "Error: Filename argument must define a valid "
						"choice: --filename device filename\n");
				return -EINVAL;
			}
			args->dev_uri = argv[i + 1];   // Set dev_uri
			args->file_name = argv[i + 2]; // Set file_name to the actual filename
			args->bmap = 1;
			printf("Device: %s, Filename: %s\n", args->dev_uri, args->file_name);
			i += 2; // Skip both arguments in the next iteration
		} else if (args->dev_uri == NULL) {
			args->dev_uri = argv[i];
		} else {
			fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
			return -EINVAL;
		}
	}

	if (args->dev_uri == NULL) {
		fprintf(stderr, "Error: Device uri is required\n");
		return -EINVAL;
	}

	return 0;
}

/**
 * Produces output on stdout similar to the output produced by running "find /mount/point"
 */
int
node_inspector_find(struct xal *XAL_UNUSED(xal), struct xal_inode *inode, void *cb_args,
		    int XAL_UNUSED(level))
{
	struct xal_nodeinspector_args *args = cb_args;

	if (xal_inode_is_dir(inode)) {
		args->ndirs += 1;
	} else if (xal_inode_is_file(inode)) {
		args->nfiles += 1;
	} else {
		printf("# UNKNOWN(%.*s)", inode->namelen, inode->name);
		return 0;
	}

	printf("%s", args->cli_args->dev_uri);
	if ((inode->parent) &&
	    (args->cli_args->dev_uri[strlen(args->cli_args->dev_uri) - 1] == '/')) {
		printf("/");
	}
	xal_inode_path_pp(inode);
	printf("\n");
	return 0;
}

int
node_inspector_bmap(struct xal *xal, struct xal_inode *inode, void *cb_args, int XAL_UNUSED(level))
{
	struct xal_nodeinspector_args *args = cb_args;
	uint32_t blocksize = xal_get_sb_blocksize(xal);
	if (xal_inode_is_dir(inode)) {
		args->ndirs += 1;
		return 0;
	} else if (xal_inode_is_file(inode)) {
		args->nfiles += 1;
	} else {
		printf("# UNKNOWN(%.*s)", inode->namelen, inode->name);
		return 0;
	}

	printf("'%s", args->cli_args->dev_uri);
	if ((inode->parent) &&
	    (args->cli_args->dev_uri[strlen(args->cli_args->dev_uri) - 1] == '/')) {
		printf("/");
	}

	xal_inode_path_pp(inode);
	printf("':");

	if (!inode->content.extents.count) {
		printf(" ~\n");
		return 0;
	}

	printf("\n");

	for (uint32_t i = 0; i < inode->content.extents.count; ++i) {
		struct xal_extent *extent = &inode->content.extents.extent[i];
		size_t fofz_begin, fofz_end, bofz_begin, bofz_end;

		fofz_begin = (extent->start_offset * blocksize) / 512;
		fofz_end = fofz_begin + (extent->nblocks * blocksize) / 512 - 1;
		bofz_begin = xal_fsbno_offset(xal, extent->start_block) / 512;
		bofz_end = bofz_begin + (extent->nblocks * blocksize) / 512 - 1;

		printf("- [%" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 "]\n", fofz_begin,
		       fofz_end, bofz_begin, bofz_end);
	}

	set_file_extent_info(xal, inode->name, inode->content.extents);

	//This code will calculate full file path and store in Hash, but currently not supported by khash.h
	/*
	char path[56] = {0};
	char final_path[56];

	if (build_inode_path(inode, path) == 0) {

		memset(final_path, 0, sizeof(final_path));

		// Add the device URI
		size_t dev_uri_len = strlen(args->cli_args->dev_uri);
		if (dev_uri_len >= sizeof(final_path)) {
			printf("Device URI is too long for the buffer\n");
			return 0; // or handle error appropriately
		}
		strncat(final_path, args->cli_args->dev_uri, sizeof(final_path) - 1);

		// Add a slash if needed
		if (dev_uri_len > 0 && final_path[dev_uri_len - 1] != '/') {
			strncat(final_path, "/", sizeof(final_path) - strlen(final_path) - 1);
		}

		// Add the path
		size_t remaining_space = sizeof(final_path) - strlen(final_path) - 1;
		if (remaining_space > 0) {
			strncat(final_path, path, remaining_space);
		} else {
			printf("Not enough space to add path to final_path\n");
			return 0; // or handle error appropriately
		}

		set_file_extent_info(xal, final_path, inode->content.extents);
	} else {
		printf("Failed to generate path\n");
	}
	*/
	return 0;
}

void 
print_file_extents(struct xal *xal, struct xal_extents *extents)
{
	uint64_t start_offset, start_block, nblocks;
	uint8_t flag;
	size_t fofz_begin, fofz_end, bofz_begin, bofz_end;
	uint32_t blocksize = xal_get_sb_blocksize(xal);

	if (extents != NULL) {
		start_offset = extents->extent->start_offset;
		start_block = extents->extent->start_block;
		nblocks = extents->extent->nblocks;
		flag = extents->extent->flag;

		fofz_begin = (extents->extent->start_offset * blocksize) / 512;
		fofz_end = fofz_begin + (extents->extent->nblocks * blocksize) / 512 - 1;
		bofz_begin = xal_fsbno_offset(xal, extents->extent->start_block) / 512;
		bofz_end = bofz_begin + (extents->extent->nblocks * blocksize) / 512 - 1;

		printf("fofz_begin = %lu, fofz_end = %lu, bofz_begin = %lu, bofz_end = %lu\n",
		       fofz_begin, fofz_end, bofz_begin, bofz_end);
		printf("start_offset = %lu, start_block = %lu, nblocks = %lu, flag = %hhu\n",
		       start_offset, start_block, nblocks, flag);
	} else {
		printf("Key not found.\n");
	}

}


int
main(int argc, char *argv[])
{
	struct xal_cli_args args = {0};
	struct xnvme_opts xnvme_opts = {0};
	struct xal_opts opts = {0};
	struct xal_nodeinspector_args cb_args;
	struct xnvme_dev *dev;
	struct xal *xal;
	int err;

	err = parse_args(argc, argv, &args);
	if (err) {
		return err;
	}

	xnvme_opts_set_defaults(&xnvme_opts);

	dev = xnvme_dev_open(args.dev_uri, &xnvme_opts);
	if (!dev) {
		printf("xnvme_dev_open(...); err(%d)\n", errno);
		return -errno;
	}

	if (args.backend) {
		if (strcmp(args.backend, "xfs") == 0) {
			opts.be = XAL_BACKEND_XFS;
		} else if (strcmp(args.backend, "fiemap") == 0) {
			opts.be = XAL_BACKEND_FIEMAP;
		} else {
			printf("Invalid backend: %s; Valid choices: xfs, fiemap\n", args.backend);
			return -EINVAL;
		}
	}

	err = xal_open(dev, &xal, &opts);
	if (err < 0) {
		printf("xal_open(...); err(%d)\n", err);
		return -err;
	}

	// Create new hash map
	create_file_extent_hash_map(xal);

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
		printf("xal_index(...); err(%d)\n", err);
		goto exit;
	}

	if (args.bmap) {
		struct xal_inode *root = xal_get_root(xal);

		memset(&cb_args, 0, sizeof(cb_args));
		cb_args.cli_args = &args;

		err = xal_walk(xal, root, node_inspector_bmap, &cb_args);
		if (err) {
			printf("xal_walk(.. node_visistor_bmap ..); err(%d)\n", err);
			goto exit;
		}
	}

	if (args.find) {
		struct xal_inode *root = xal_get_root(xal);

		memset(&cb_args, 0, sizeof(cb_args));
		cb_args.cli_args = &args;

		err = xal_walk(xal, root, node_inspector_find, &cb_args);
		if (err) {
			printf("xal_walk(.. node_visistor_find ..); err(%d)\n", err);
			goto exit;
		}
	}

	if (args.file_name != NULL) {
		struct xal_extents *file_extents;
		get_file_extent_info(xal, args.file_name, &file_extents);
		print_file_extents(xal, file_extents);
	}

	if (args.stats) {
		printf("ndirs(%" PRIu64 "); nfiles(%" PRIu64 ")\n", cb_args.ndirs, cb_args.nfiles);
	}

exit:
	xal_close(xal);
	xnvme_dev_close(dev);

	return -err;
}

