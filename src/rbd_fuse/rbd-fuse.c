/*
 * rbd-fuse
 *
 * compile with
 * cc -D_FILE_OFFSET_BITS=64 rbd-fuse.c -o rbd-fuse -lfuse -lrbd
 */
#define FUSE_USE_VERSION 26

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <pthread.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <getopt.h>

#include "include/rbd/librbd.h"

static int gotrados = 0;
struct radosPool *pool;
rados_t cluster;

static pthread_mutex_t readdir_lock;

struct radosStat {
	u_char valid;
	char objname[RBD_MAX_BLOCK_NAME_SIZE];
	uint64_t size;
	time_t mtime;
};

struct rbdStat {
	u_char valid;
	rbd_image_info_t rbd_info;
};

struct rbdOptions {
	char *ceph_config;
	char *pool_name;
};

struct radosPool {
	char *pool_name;
	rados_t	cluster;
	rados_ioctx_t ioctx;
	struct radosStat rados_stat;
	struct radosPool *next;
};
#define MAX_RADOS_POOLS		32
struct radosPool radosPools[MAX_RADOS_POOLS];

struct rbdImage {
	struct radosPool *pool;
	char *image_name;
	struct rbdImage *next;
};
struct rbdImage *rbdImages;

struct rbdOpenImage {
	struct rbdImage *rbdimage;
	rbd_image_t image;
	struct rbdStat rbd_stat;
};
#define MAX_RBD_IMAGES		128
struct rbdOpenImage rbdFds[MAX_RBD_IMAGES];

struct rbdOptions rbdOptions = {"/etc/ceph/ceph.conf", "rbd"};


/* prototypes */
void radosPools_init(void);
void get_rbdImages(struct rbdImage **head);
void simple_err(const char *msg, int err);
int connect_to_cluster();

int radosPool_start(rados_t cluster, char *pool_name);
int lookup_radosPool(char *pool_name);
int allocate_radosPool(rados_t cluster, char *pool_name);

int open_rbdImage(const char *image_name);
int allocate_rbdImage(struct radosPool *pool);
void deallocate_rbdImage(int imageId);

static void
iter_images(void *cookie,
	    void (*iter)(void *cookie, const char *image, int dirfd))
{
	struct rbdImage *im;

	pthread_mutex_lock(&readdir_lock);

	for (im = rbdImages; im != NULL; im = im->next)
		iter(cookie, im->image_name, -1);
	pthread_mutex_unlock(&readdir_lock);
}

static int read_property(int fd, char *name, unsigned int *_value)
{
	unsigned int value;
	struct rbdOpenImage *rbd;

	rbd = &rbdFds[fd];

	if (rbd->image == NULL)
		return -1;

	if (strncmp(name, "obj_siz", 7) == 0) {
		value = rbd->rbd_stat.rbd_info.obj_size;
	} else if (strncmp(name, "num_obj", 7) == 0) {
		value = rbd->rbd_stat.rbd_info.num_objs;
	} else {
		return -1;
	}

	*_value = value;
	return 0;
}

static void count_images_cb(void *cookie, const char *image, int dirfd)
{
	(*((unsigned int *)cookie))++;
}

static int count_images(void)
{
	unsigned int count = 0;

	get_rbdImages(&rbdImages);
	iter_images(&count, count_images_cb);
	return count;
}

static int blockfs_getattr(const char *path, struct stat *stbuf)
{
	int fd;
	time_t now;
	unsigned int num_parts;
	unsigned int part_size;

	if (!gotrados)
		return -ENXIO;

	if (path[0] == 0)
		return -ENOENT;

	memset(stbuf, 0, sizeof(struct stat));

	if (strcmp(path, "/") == 0) {

		now = time(NULL);
		stbuf->st_mode = S_IFDIR + 0755;
		stbuf->st_nlink = 2+count_images();
		stbuf->st_uid = getuid();
		stbuf->st_gid = getgid();
		stbuf->st_size = 1024;
		stbuf->st_blksize = 1024;
		stbuf->st_blocks = 1;
		stbuf->st_atime = now;
		stbuf->st_mtime = now;
		stbuf->st_ctime = now;

		return 0;
	}

	fd = open_rbdImage(path + 1);
	if (fd < 0)
		return -ENOENT;

	if (read_property(fd, "num_objs", &num_parts) < 0) {
		return -EINVAL;
	}

	if (read_property(fd, "obj_size", &part_size) < 0) {
		return -EINVAL;
	}

	now = time(NULL);
	stbuf->st_mode = S_IFREG | 0666;
	stbuf->st_nlink = 1;
	stbuf->st_uid = getuid();
	stbuf->st_gid = getgid();
	stbuf->st_size = (uint64_t)num_parts * part_size;
	stbuf->st_blksize = part_size;
	stbuf->st_blocks = ((uint64_t)num_parts * part_size + 511) / 512;
	stbuf->st_atime = now;
	stbuf->st_mtime = now;
	stbuf->st_ctime = now;

	return 0;
}

static int blockfs_truncate(const char *path, off_t length)
{
	return -EINVAL;
}

static int blockfs_open(const char *path, struct fuse_file_info *fi)
{
	int fd;
	unsigned int num_parts;
	unsigned int part_size;

	if (!gotrados)
		return -ENXIO;

	if (path[0] == 0)
		return -ENOENT;

	fd = open_rbdImage(path + 1);
	if (fd < 0)
		return -ENOENT;

	if (read_property(fd, "num_objs", &num_parts) < 0) {
		return -EINVAL;
	}

	if (read_property(fd, "obj_size", &part_size) < 0) {
		return -EINVAL;
	}

	fi->fh = fd;

	return 0;
}

static int blockfs_read(const char *path, char *buf, size_t size,
			off_t offset, struct fuse_file_info *fi)
{
	size_t numread;
	struct rbdOpenImage *rbd;

	if (!gotrados)
		return -ENXIO;

	rbd = &rbdFds[fi->fh];
	numread = 0;
	while (size > 0) {
		ssize_t ret;

		ret = rbd_read(rbd->image, offset, size, buf);

		if (ret <= 0)
			break;
		buf += ret;
		size -= ret;
		offset += ret;
		numread += ret;
	}

	return numread;
}

static int blockfs_write(const char *path, const char *buf, size_t size,
			 off_t offset, struct fuse_file_info *fi)
{
	size_t numwritten;
	struct rbdOpenImage *rbd;

	if (!gotrados)
		return -ENXIO;

	rbd = &rbdFds[fi->fh];
	numwritten = 0;
	while (size > 0) {
		ssize_t ret;

		ret = rbd_write(rbd->image, offset, size, buf);

		if (ret < 0)
			break;
		buf += ret;
		size -= ret;
		offset += ret;
		numwritten += ret;
	}

	return numwritten;
}

static void blockfs_statfs_image_cb(void *num, const char *image, int dirfd)
{
	unsigned int num_parts, part_size;
	int	fd;

	((uint64_t *)num)[0]++;

	fd = open_rbdImage(image);
	if (fd >= 0) {
		if (read_property(fd, "num_objs", &num_parts) >= 0) {
			if (read_property(fd, "obj_size", &part_size) >= 0) {
				((uint64_t *)num)[1] += (uint64_t)num_parts * (uint64_t)part_size;
			}
		}
	}
}

static int blockfs_statfs(const char *path, struct statvfs *buf)
{
	uint64_t num[2];

	if (!gotrados)
		return -ENXIO;

	num[0] = 1;
	num[1] = 0;
	get_rbdImages(&rbdImages);
	iter_images(num, blockfs_statfs_image_cb);

#define	RBDFS_BSIZE	4096
	buf->f_bsize = RBDFS_BSIZE;
	buf->f_frsize = RBDFS_BSIZE;
	buf->f_blocks = num[1] / RBDFS_BSIZE;
	buf->f_bfree = 0;
	buf->f_bavail = 0;
	buf->f_files = num[0];
	buf->f_ffree = 0;
	buf->f_favail = 0;
	buf->f_fsid = 0;
	buf->f_flag = 0;
	buf->f_namemax = PATH_MAX;

	return 0;
}

static int blockfs_fsync(const char *path, int datasync,
			 struct fuse_file_info *fi)
{
	if (!gotrados)
		return -ENXIO;
	rbd_flush(rbdFds[fi->fh].image);
	return 0;
}

struct blockfs_readdir_info {
	void *buf;
	fuse_fill_dir_t filler;
};

static void blockfs_readdir_cb(void *_info, const char *name, int dirfd)
{
	struct blockfs_readdir_info *info = _info;

	info->filler(info->buf, name, NULL, 0);
}

static int blockfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			   off_t offset, struct fuse_file_info *fi)
{
	struct blockfs_readdir_info info = { buf, filler };

	if (!gotrados)
		return -ENXIO;

	if (strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	get_rbdImages(&rbdImages);
	iter_images(&info, blockfs_readdir_cb);

	return 0;
}

void *
blockfs_init(struct fuse_conn_info *conn)
{
	int ret;
	int poolfd;

	// init cannot fail, so if we fail here, gotrados remains at 0,
	// causing other operations to fail immediately with ENXIO

	ret = connect_to_cluster();
	if (ret < 0)
		exit(90);
	poolfd = radosPool_start(cluster, rbdOptions.pool_name);
	if (poolfd < 0)
		exit(91);

	pool = &(radosPools[poolfd]);
	conn->want |= FUSE_CAP_BIG_WRITES;
	gotrados = 1;

	// init's return value shows up in fuse_context.private_data,
	// also to void (*destroy)(void *); useful?
	return NULL;
}

#define DEFAULT_IMAGE_SIZE 	1024ULL * 1024 * 1024
#define DEFAULT_IMAGE_ORDER 	22
#define DEFAULT_IMAGE_FEATURES 	1

// return -errno on error.  fi->fh is not set until open time

int
blockfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	int r;
	uint64_t size = DEFAULT_IMAGE_SIZE;
	int order = DEFAULT_IMAGE_ORDER;
	uint64_t features = DEFAULT_IMAGE_FEATURES;

	r = rbd_create2(pool->ioctx, path+1, size, features, &order);
	fprintf(stderr, "rbd_create %s size 0x%lx returns %d\n",
 		path, size, r);
	return r;
}

int
blockfs_utime(const char *path, struct utimbuf *utime)
{
	fprintf(stderr, "blockfs_utime(%s): ignoring\n", path);
	return 0;
}

static struct fuse_operations blockfs_oper = {
	.getattr	= blockfs_getattr,
	.truncate	= blockfs_truncate,
	.open		= blockfs_open,
	.read		= blockfs_read,
	.write		= blockfs_write,
	.statfs		= blockfs_statfs,
	.fsync		= blockfs_fsync,
	.readdir	= blockfs_readdir,
	.init		= blockfs_init,
	.create		= blockfs_create,
	.utime		= blockfs_utime
};

enum {
	KEY_HELP,
	KEY_VERSION,
	KEY_CEPH_CONFIG,
	KEY_CEPH_CONFIG_LONG,
	KEY_RADOS_POOLNAME,
	KEY_RADOS_POOLNAME_LONG
};

static struct fuse_opt blockfs_opts[] = {
	FUSE_OPT_KEY("-h", KEY_HELP),
	FUSE_OPT_KEY("--help", KEY_HELP),
	FUSE_OPT_KEY("-V", KEY_VERSION),
	FUSE_OPT_KEY("--version", KEY_VERSION),
	{"-c %s", offsetof(struct rbdOptions, ceph_config), KEY_CEPH_CONFIG},
	{"--configfile=%s", offsetof(struct rbdOptions, ceph_config),
	 KEY_CEPH_CONFIG_LONG},
	{"-p %s", offsetof(struct rbdOptions, pool_name), KEY_RADOS_POOLNAME},
	{"--poolname=%s", offsetof(struct rbdOptions, pool_name),
	 KEY_RADOS_POOLNAME_LONG},
};

static void usage(const char *progname)
{
	fprintf(stderr,
"Usage: %s mountpoint [options]\n"
"\n"
"General options:\n"
"    -h   --help            print help\n"
"    -V   --version         print version\n"
"    -c   --configfile      ceph configuration file [/etc/ceph/ceph.conf]\n"
"    -p   --poolname        rados pool name [rbd]\n"
"\n", progname);
}

static int blockfs_opt_proc(void *data, const char *arg, int key,
			    struct fuse_args *outargs)
{
	if (key == KEY_HELP) {
		usage(outargs->argv[0]);
		fuse_opt_add_arg(outargs, "-ho");
		fuse_main(outargs->argc, outargs->argv, &blockfs_oper, NULL);
		exit(1);
	}

	if (key == KEY_VERSION) {
		fuse_opt_add_arg(outargs, "--version");
		fuse_main(outargs->argc, outargs->argv, &blockfs_oper, NULL);
		exit(0);
	}

	if (key == KEY_CEPH_CONFIG) {
		if (rbdOptions.ceph_config != NULL) {
			free(rbdOptions.ceph_config);
			rbdOptions.ceph_config = NULL;
		}
		rbdOptions.ceph_config = strdup(arg+2);
		return 0;
	}

	if (key == KEY_RADOS_POOLNAME) {
		if (rbdOptions.pool_name != NULL) {
			free(rbdOptions.pool_name);
			rbdOptions.pool_name = NULL;
		}
		rbdOptions.pool_name = strdup(arg+2);
		return 0;
	}

	return 1;
}

void
simple_err(const char *msg, int err)
{
    fprintf(stderr, "%s: %s\n", msg, strerror(-err));
    return;
}

int
connect_to_cluster()
{
	int r;

	r = rados_create(&(cluster), NULL);
	if (r < 0) {
		simple_err("Could not create cluster handle", r);
		return r;
	}
	rados_conf_parse_env(cluster, NULL);
	r = rados_conf_read_file(cluster, rbdOptions.ceph_config);
	if (r < 0) {
		simple_err("Error reading Ceph config file", r);
		goto failed_shutdown;
	}
	r = rados_connect(cluster);
	if (r < 0) {
		simple_err("Error connecting to cluster", r);
		goto failed_shutdown;
	}

	return 0;

failed_shutdown:
	rados_shutdown(cluster);
	return r;
}

int
radosPool_start(rados_t cluster, char *pool_name)
{
	int r = 0;
	int poolfd;
	struct radosPool *pool;
	struct radosStat *sbuf;

	poolfd = lookup_radosPool(pool_name);
	pool = NULL;

	if (poolfd >= 0) {
		simple_err("Pool already started", poolfd);
		poolfd = -EEXIST;
	} else {
		poolfd = allocate_radosPool(cluster, pool_name);
		if (poolfd >= 0) {
			pool = &(radosPools[poolfd]);
		} else {
			poolfd = -ENOSPC;
		}
	}

	if (pool != (struct radosPool *)NULL) {
		r = rados_ioctx_create(pool->cluster, pool_name,
				       &(pool->ioctx));
		if (r < 0) {
			simple_err("Error creating ioctx", r);
			poolfd = r;
		} else {
			sbuf = &(pool->rados_stat);
			rados_stat(pool->ioctx, &(sbuf->objname[0]),
				   &(sbuf->size), &(sbuf->mtime));
			pool->rados_stat.valid = 1;
		}

	}
	return poolfd;
}


void
radosPools_init(void)
{
	int i;

	for (i = 0; i < MAX_RADOS_POOLS; i++) {
		radosPools[i].pool_name = NULL;
		radosPools[i].rados_stat.valid = 0;
		if ((i+1) < MAX_RADOS_POOLS) {
			radosPools[i].next = &(radosPools[i+1]);
		} else {
			radosPools[i].next = (struct radosPool *)NULL;
		}
	}
	return;
}

void
get_rbdImages(struct rbdImage **head)
{
	char *ibuf;
	size_t ibuf_len;
	struct rbdImage *im, *next;
	char *ip;
	int r;

	if (*head != NULL) {
		for (im = *head; im != NULL;) {
			next = im->next;
			free(im);
			im = next;
		}
		*head = NULL;
	}

	ibuf_len = 0;
	rbd_list(pool->ioctx, NULL, &ibuf_len);
	ibuf = malloc(ibuf_len);
	r = rbd_list(pool->ioctx, ibuf, &ibuf_len);
	if (r < 0) {
		simple_err("rbd_list: error %d\n", r);
		return;
	}

	for (ip = ibuf; *ip != '\0' && ip < &ibuf[ibuf_len]; ip += strlen(ip) + 1)  {
		fprintf(stderr, "adding image %s/%s\n", pool->pool_name, ip);
		im = malloc(sizeof(*im));
		im->pool = pool;
		im->image_name = ip;
		im->next = *head;
		*head = im;
	}
	return;
}

int
lookup_radosPool(char *pool_name)
{
	int i, poolfd;

	poolfd = -1;

	for (i = 0; i < MAX_RADOS_POOLS; i++) {
		if (radosPools[i].pool_name == NULL) {
			continue;
		}
		if (strcmp(pool_name, radosPools[i].pool_name) == 0) {
			poolfd = i;
			break;
		}
	}

	return poolfd;
}

int
allocate_radosPool(rados_t cluster, char *pool_name)
{
	int i, poolfd;

	poolfd = -1;

	if (lookup_radosPool(pool_name) < 0) {
		for (i = 0; i < MAX_RADOS_POOLS; i++) {
			if (radosPools[i].pool_name == NULL) {
				radosPools[i].pool_name = strdup(pool_name);
				radosPools[i].cluster = cluster;
				poolfd = i;
				break;
			}
		}
	}

	return poolfd;
}

int
open_rbdImage(const char *image_name)
{
	struct rbdImage *im;
	struct rbdOpenImage *rbd;
	int fd, i;
	int ret;

	if (image_name == (char *)NULL) 
		return -1;

	/* find free rbdFds[] entry, assign fd */
	for (i = 0; i < MAX_RBD_IMAGES; i++) {
		if (rbdFds[i].rbdimage == NULL) {
			fd = i;
			rbd = &rbdFds[fd];
			break;
		}
	}
	if (i == MAX_RBD_IMAGES)
		return -1;

	get_rbdImages(&rbdImages);
	for (im = rbdImages; im != NULL; i++, im = im->next) {
		if (strcmp(im->image_name, image_name) == 0) {
			rbd->rbdimage = im;
			break;
		}
	}
	if (im == NULL)
		return -1;

	ret = rbd_open(pool->ioctx, rbd->rbdimage->image_name, &(rbd->image),
		       NULL);
	if (ret < 0) {
		simple_err("open_rbdImage: can't open: ", ret);
		return -1;
	} else {
		rbd_stat(rbd->image, &(rbd->rbd_stat.rbd_info),
			 sizeof(rbd_image_info_t));
		rbd->rbd_stat.valid = 1;
	}
	return fd;
}

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	radosPools_init();

	if (fuse_opt_parse(&args, &rbdOptions, blockfs_opts, blockfs_opt_proc)
	    == -1) {
		exit(1);
	}

	pthread_mutex_init(&readdir_lock, NULL);

	return fuse_main(args.argc, args.argv, &blockfs_oper, NULL);
}
