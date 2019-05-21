/* 
 * v9fs
 * 
 * Adjust native 9P FS in Linux for OSv
 */

#ifndef __INCLUDE_V9FS_H__
#define __INCLUDE_V9FS_H__

#include <osv/vnode.h>
#include <osv/mount.h>
#include <osv/dentry.h>
#include <osv/prex.h>
#include <osv/buf.h>
#include <osv/9pclient.hh>

#define V9FS_MAGIC              0x01021997

#define V9FS_INODE_SIZE ((uint64_t)sizeof(struct v9fs_inode))

#define V9FS_SUPERBLOCK_SIZE sizeof(struct v9fs_super_block)
#define V9FS_SUPERBLOCK_BLOCK 0

//#define V9FS_DEBUG_ENABLED 1

#if defined(V9FS_DEBUG_ENABLED)
#define print(...) kprintf(__VA_ARGS__)
#else
#define print(...)
#endif

#define V9FS_DIAGNOSTICS_ENABLED 1

#if defined(V9FS_DIAGNOSTICS_ENABLED)
#define V9FS_STOPWATCH_START auto begin = std::chrono::high_resolution_clock::now();
#define V9FS_STOPWATCH_END(total) auto end = std::chrono::high_resolution_clock::now(); \
std::chrono::duration<double> sec = end - begin; \
total += ((long)(sec.count() * 1000000));
//TODO: Review - avoid conversions
#else
#define V9FS_STOPWATCH_START
#define V9FS_STOPWATCH_END(...)
#endif

extern struct vfsops v9fs_vfsops;
extern struct vnops v9fs_vnops;

/**
 * enum p9_session_flags - option flags for each 9P session
 * @V9FS_PROTO_2000U: whether or not to use 9P2000.u extensions
 * @V9FS_PROTO_2000L: whether or not to use 9P2000.l extensions
 * @V9FS_ACCESS_SINGLE: only the mounting user can access the hierarchy
 * @V9FS_ACCESS_USER: a new attach will be issued for every user (default)
 * @V9FS_ACCESS_CLIENT: Just like user, but access check is performed on client.
 * @V9FS_ACCESS_ANY: use a single attach for all users
 * @V9FS_ACCESS_MASK: bit mask of different ACCESS options
 * @V9FS_POSIX_ACL: POSIX ACLs are enforced
 *
 * Session flags reflect options selected by users at mount time
 */
#define	V9FS_ACCESS_ANY (V9FS_ACCESS_SINGLE | \
			 V9FS_ACCESS_USER |   \
			 V9FS_ACCESS_CLIENT)
#define V9FS_ACCESS_MASK V9FS_ACCESS_ANY
#define V9FS_ACL_MASK V9FS_POSIX_ACL

enum p9_session_flags {
	V9FS_PROTO_2000U	= 0x01,
	V9FS_PROTO_2000L	= 0x02,
	V9FS_ACCESS_SINGLE	= 0x04,
	V9FS_ACCESS_USER	= 0x08,
	V9FS_ACCESS_CLIENT	= 0x10,
	V9FS_POSIX_ACL		= 0x20
};

/* possible values of ->cache */
/**
 * enum p9_cache_modes - user specified cache preferences
 * @CACHE_NONE: do not cache data, dentries, or directory contents (default)
 * @CACHE_LOOSE: cache data, dentries, and directory contents w/no consistency
 *
 * eventually support loose, tight, time, session, default always none
 */

enum p9_cache_modes {
	CACHE_NONE,
	CACHE_MMAP,
	CACHE_LOOSE,
	CACHE_FSCACHE,
	nr__p9_cache_modes
};

/**
 * struct v9fs_session_info - per-instance session information
 * @flags: session options of type &p9_session_flags
 * @nodev: set to 1 to disable device mapping
 * @debug: debug level
 * @afid: authentication handle
 * @cache: cache mode of type &p9_cache_modes
 * @cachetag: the tag of the cache associated with this session
 * @fscache: session cookie associated with FS-Cache
 * @uname: string user name to mount hierarchy as
 * @aname: mount specifier for remote hierarchy
 * @maxdata: maximum data to be sent/recvd per protocol message
 * @dfltuid: default numeric userid to mount hierarchy as
 * @dfltgid: default numeric groupid to mount hierarchy as
 * @uid: if %V9FS_ACCESS_SINGLE, the numeric uid which mounted the hierarchy
 * @clnt: reference to 9P network client instantiated for this session
 * @slist: reference to list of registered 9p sessions
 *
 * This structure holds state for each session instance established during
 * a sys_mount() .
 *
 * Bugs: there seems to be a lot of state which could be condensed and/or
 * removed.
 */


struct v9fs_session_info {
	/* options */
	unsigned char flags;
	unsigned char nodev;
	unsigned short debug;
	unsigned int afid;
	unsigned int cache;
#ifdef CONFIG_9P_FSCACHE
	char *cachetag;
	struct fscache_cookie *fscache;
#endif

	char *uname;		/* user name to mount as */
	char *aname;		/* name of remote hierarchy being mounted */
	unsigned int maxdata;	/* max data for client interface */
	kuid_t dfltuid;		/* default uid/muid for legacy support */
	kgid_t dfltgid;		/* default gid for legacy support */
	kuid_t uid;		/* if ACCESS_SINGLE, the uid that has access */
	struct p9_client *clnt;	/* 9p client */
	// struct list_head slist; /* list of sessions registered with v9fs */
	// struct rw_semaphore rename_sem;
	// struct rwlock_t rename_sem;
};

struct v9fs_super_block {
	unsigned char s_blocksize_bits;
	unsigned long s_blocksize;
	loff_t s_maxbytes;
	unsigned long s_flags;
	unsigned long s_iflags;
	unsigned long s_magic;
	struct dentry *s_root;
	struct v9fs_session_info *s_v9ses;
};

typedef unsigned short		umode_t;
// typedef __u32 __kernel_dev_t;
// typedef __kernel_dev_t		dev_t;

#ifdef CONFIG_LBDAF
typedef u64 sector_t;
// typedef u64 blkcnt_t;
#else
typedef unsigned long sector_t;
// typedef unsigned long blkcnt_t;
#endif

// typedef long		__kernel_long_t;
// typedef __kernel_long_t	__kernel_time_t;
// #ifndef _STRUCT_TIMESPEC
// #define _STRUCT_TIMESPEC
// struct timespec {
// 	__kernel_time_t	tv_sec;			/* seconds */
// 	long		tv_nsec;		/* nanoseconds */
// };
// #endif

struct v9fs_dirent {
	struct p9_dirent dirent;
	struct v9fs_dirent *next;
};

struct v9fs_inode {
#ifdef CONFIG_9P_FSCACHE
	struct mutex fscache_lock;
	struct fscache_cookie *fscache;
#endif
	struct p9_fid *fid;        /* fid for inode operations */
	struct p9_fid *handle_fid; /* fid for read/write operations */
	struct v9fs_dirent *entries;
	struct v9fs_dirent *current;
	// mutex_t v_mutex;
};

extern int v9fs_show_options(struct seq *m, struct dentry *root);

struct p9_fid *v9fs_session_init(struct v9fs_session_info *, const char *,
									char *);
extern void v9fs_session_close(struct v9fs_session_info *v9ses);
extern void v9fs_session_cancel(struct v9fs_session_info *v9ses);
extern void v9fs_session_begin_cancel(struct v9fs_session_info *v9ses);

/* other default globals */
#define V9FS_PORT	564
#define V9FS_DEFUSER	"nobody"
#define V9FS_DEFANAME	""
#define V9FS_DEFUID	KUIDT_INIT(-2)
#define V9FS_DEFGID	KGIDT_INIT(-2)



namespace v9fs {
	int cache_read(struct v9fs_inode *inode, struct device *device, struct rofs_super_block *sb, struct uio *uio);
}

/* Common Operations
 */
ino_t v9fs_qid2ino(struct p9_qid *qid);
void v9fs_set_vnode(struct vnode* vp, struct p9_wstat *st);
void v9fs_set_vnode_dotl(struct vnode* vp, struct p9_stat_dotl *st);

int v9fs_flags2omode(int flags, int extended);
int v9fs_flags2omode_dotl(int flags);

void v9fs_blank_wstat(struct p9_wstat *wstat);


#endif