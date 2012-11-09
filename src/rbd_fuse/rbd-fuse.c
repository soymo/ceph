/*
 * rbd-fuse
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
char *pool_name;
rados_t cluster;
rados_ioctx_t ioctx;

static pthread_mutex_t readdir_lock;

struct rbd_stat {
	u_char valid;
	rbd_image_info_t rbd_info;
};

struct rbd_options {
	char *ceph_config;
	char *pool_name;
};

struct rbd_image {
	char *image_name;
	struct rbd_image *next;
};
struct rbd_image *rbd_images;

struct rbd_openimage {
	char *image_name;
	rbd_image_t image;
	struct rbd_stat rbd_stat;
};
#define MAX_RBD_IMAGES		128
struct rbd_openimage opentbl[MAX_RBD_IMAGES];

struct rbd_options rbd_options = {"/etc/ceph/ceph.conf", "rbd"};

// Minimize calls to rbd_list: marks bracketing of opendir/<ops>/releasedir
int in_opendir;

/* prototypes */
int connect_to_cluster(rados_t *pcluster);
void enumerate_images(struct rbd_image **head);
int open_rbd_image(const char *image_name);
int find_openrbd(const char *path);

void simple_err(const char *msg, int err);

void
enumerate_images(struct rbd_image **head)
{
	char *ibuf;
	size_t ibuf_len;
	struct rbd_image *im, *next;
	char *ip;
	int actual_len;

	if (*head != NULL) {
		for (im = *head; im != NULL;) {
			next = im->next;
			free(im);
			im = next;
		}
		*head = NULL;
	}

	ibuf_len = 1024;
	ibuf = malloc(ibuf_len);
	actual_len = rbd_list(ioctx, ibuf, &ibuf_len);
	if (actual_len < 0) {
		simple_err("rbd_list: error %d\n", actual_len);
		return;
	}

	fprintf(stderr, "pool %s: ", pool_name);
	for (ip = ibuf; *ip != '\0' && ip < &ibuf[actual_len];
	     ip += strlen(ip) + 1)  {
		fprintf(stderr, "%s, ", ip);
		im = malloc(sizeof(*im));
		im->image_name = ip;
		im->next = *head;
		*head = im;
	}
	fprintf(stderr, "\n");
	return;
}

int
find_openrbd(const char *path)
{
	int i;

	/* find in opentbl[] entry if already open */
	for (i = 0; i < MAX_RBD_IMAGES; i++) {
		if ((opentbl[i].image_name != NULL) &&
		    (strcmp(opentbl[i].image_name, path) == 0)) {
			return i;
			break;
		}
	}
	return -1;
}

int
open_rbd_image(const char *image_name)
{
	struct rbd_image *im;
	struct rbd_openimage *rbd;
	int fd, i;
	int ret;

	if (image_name == (char *)NULL) 
		return -1;

	// relies on caller to keep rbd_images up to date
	for (im = rbd_images; im != NULL; i++, im = im->next) {
		if (strcmp(im->image_name, image_name) == 0) {
			break;
		}
	}
	if (im == NULL)
		return -1;

	/* find in opentbl[] entry if already open */
	if ((fd = find_openrbd(image_name)) != -1) {
		rbd = &opentbl[fd];
	} else {
		// allocate an opentbl[] and open the image
		for (i = 0; i < MAX_RBD_IMAGES; i++) {
			if (opentbl[i].image == NULL) {
				fd = i;
				rbd = &opentbl[fd];
				rbd->image_name = strdup(image_name);
				break;
			}
		}
		if (i == MAX_RBD_IMAGES)
			return -1;
		ret = rbd_open(ioctx, rbd->image_name, &(rbd->image), NULL);
		if (ret < 0) {
			simple_err("open_rbd_image: can't open: ", ret);
			return ret;
		}
	}
	rbd_stat(rbd->image, &(rbd->rbd_stat.rbd_info),
		 sizeof(rbd_image_info_t));
	rbd->rbd_stat.valid = 1;
	return fd;
}

static void
iter_images(void *cookie,
	    void (*iter)(void *cookie, const char *image))
{
	struct rbd_image *im;

	pthread_mutex_lock(&readdir_lock);

	for (im = rbd_images; im != NULL; im = im->next)
		iter(cookie, im->image_name);
	pthread_mutex_unlock(&readdir_lock);
}

static int read_property(int fd, char *name, unsigned int *_value)
{
	unsigned int value;
	struct rbd_openimage *rbd;

	rbd = &opentbl[fd];

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

static void count_images_cb(void *cookie, const char *image)
{
	(*((unsigned int *)cookie))++;
}

static int count_images(void)
{
	unsigned int count = 0;

	pthread_mutex_lock(&readdir_lock);
	enumerate_images(&rbd_images);
	pthread_mutex_unlock(&readdir_lock);

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

	if (!in_opendir) {
		pthread_mutex_lock(&readdir_lock);
		enumerate_images(&rbd_images);
		pthread_mutex_unlock(&readdir_lock);
	}
	fd = open_rbd_image(path + 1);
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

	pthread_mutex_lock(&readdir_lock);
	enumerate_images(&rbd_images);
	pthread_mutex_unlock(&readdir_lock);
	fd = open_rbd_image(path + 1);
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
	struct rbd_openimage *rbd;

	if (!gotrados)
		return -ENXIO;

	rbd = &opentbl[fi->fh];
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
	struct rbd_openimage *rbd;

	if (!gotrados)
		return -ENXIO;

	rbd = &opentbl[fi->fh];
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

static void blockfs_statfs_image_cb(void *num, const char *image)
{
	unsigned int num_parts, part_size;
	int	fd;

	((uint64_t *)num)[0]++;

	fd = open_rbd_image(image);
	if (fd >= 0) {
		if (read_property(fd, "num_objs", &num_parts) >= 0 && 
		    read_property(fd, "obj_size", &part_size) >= 0) {
			((uint64_t *)num)[1] +=
				 (uint64_t)num_parts * (uint64_t)part_size;
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
	pthread_mutex_lock(&readdir_lock);
	enumerate_images(&rbd_images);
	pthread_mutex_unlock(&readdir_lock);
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
	rbd_flush(opentbl[fi->fh].image);
	return 0;
}

static int blockfs_opendir(const char *path, struct fuse_file_info *fi)
{
	// only one directory, so global "in_opendir" flag should be fine
	pthread_mutex_lock(&readdir_lock);
	in_opendir++;
	enumerate_images(&rbd_images);
	pthread_mutex_unlock(&readdir_lock);
	return 0;
}

struct blockfs_readdir_info {
	void *buf;
	fuse_fill_dir_t filler;
};

static void blockfs_readdir_cb(void *_info, const char *name)
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
	if (!in_opendir)
		fprintf(stderr, "in readdir, but not inside opendir?\n");

	if (strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);
	iter_images(&info, blockfs_readdir_cb);

	return 0;
}
static int blockfs_releasedir(const char *path, struct fuse_file_info *fi)
{
	// see opendir comments
	pthread_mutex_lock(&readdir_lock);
	in_opendir--;
	pthread_mutex_unlock(&readdir_lock);
	return 0;
}

void *
blockfs_init(struct fuse_conn_info *conn)
{
	int ret;

	// init cannot fail, so if we fail here, gotrados remains at 0,
	// causing other operations to fail immediately with ENXIO

	ret = connect_to_cluster(&cluster);
	if (ret < 0)
		exit(90);

	pool_name = rbd_options.pool_name;
	ret = rados_ioctx_create(cluster, pool_name, &ioctx);
	if (ret < 0)
		exit(91);

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

	r = rbd_create2(ioctx, path+1, size, features, &order);
	return r;
}

int
blockfs_utime(const char *path, struct utimbuf *utime)
{
	// called on create; not relevant
	return 0;
}

int
blockfs_unlink(const char *path)
{
	int fd = find_openrbd(path);
	if (fd != -1) {
		struct rbd_openimage *rbd = &opentbl[fd];
		rbd_close(rbd->image);
		rbd->image = 0;
		free(rbd->image_name);
		rbd->rbd_stat.valid = 0;
	}
	return rbd_remove(ioctx, path+1);
}

static struct fuse_operations blockfs_oper = {
	.create		= blockfs_create,
	.fsync		= blockfs_fsync,
	.getattr	= blockfs_getattr,
	.init		= blockfs_init,
	.open		= blockfs_open,
	.opendir	= blockfs_opendir,
	.read		= blockfs_read,
	.readdir	= blockfs_readdir,
	.releasedir	= blockfs_releasedir,
	.statfs		= blockfs_statfs,
	.truncate	= blockfs_truncate,
	.unlink		= blockfs_unlink,
	.utime		= blockfs_utime,
	.write		= blockfs_write,
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
	{"-c %s", offsetof(struct rbd_options, ceph_config), KEY_CEPH_CONFIG},
	{"--configfile=%s", offsetof(struct rbd_options, ceph_config),
	 KEY_CEPH_CONFIG_LONG},
	{"-p %s", offsetof(struct rbd_options, pool_name), KEY_RADOS_POOLNAME},
	{"--poolname=%s", offsetof(struct rbd_options, pool_name),
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
		if (rbd_options.ceph_config != NULL) {
			free(rbd_options.ceph_config);
			rbd_options.ceph_config = NULL;
		}
		rbd_options.ceph_config = strdup(arg+2);
		return 0;
	}

	if (key == KEY_RADOS_POOLNAME) {
		if (rbd_options.pool_name != NULL) {
			free(rbd_options.pool_name);
			rbd_options.pool_name = NULL;
		}
		rbd_options.pool_name = strdup(arg+2);
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
connect_to_cluster(rados_t *pcluster)
{
	int r;

	r = rados_create(pcluster, NULL);
	if (r < 0) {
		simple_err("Could not create cluster handle", r);
		return r;
	}
	rados_conf_parse_env(*pcluster, NULL);
	r = rados_conf_read_file(*pcluster, rbd_options.ceph_config);
	if (r < 0) {
		simple_err("Error reading Ceph config file", r);
		goto failed_shutdown;
	}
	r = rados_connect(*pcluster);
	if (r < 0) {
		simple_err("Error connecting to cluster", r);
		goto failed_shutdown;
	}

	return 0;

failed_shutdown:
	rados_shutdown(*pcluster);
	return r;
}


int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	if (fuse_opt_parse(&args, &rbd_options, blockfs_opts, blockfs_opt_proc)
	    == -1) {
		exit(1);
	}

	pthread_mutex_init(&readdir_lock, NULL);

	return fuse_main(args.argc, args.argv, &blockfs_oper, NULL);
}
