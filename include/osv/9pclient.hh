/*
 *  include/osv/p9client.hh
 *
 *  VirtFS Client - Only virtio transport is supportted
 *
 */

#ifndef __OSV_P9CLIENT_H
#define __OSV_P9CLIENT_H

#include <osv/irqlock.hh>
#include <api/limits.h>
#include <unordered_set>
#include <list>
#include <sys/stat.h>
#include <endian.h>
#include <byteswap.h>

/**
 * enum p9_msg_t - 9P message types
 * @P9_TLERROR: not used
 * @P9_RLERROR: response for any failed request for 9P2000.L
 * @P9_TSTATFS: file system status request
 * @P9_RSTATFS: file system status response
 * @P9_TSYMLINK: make symlink request
 * @P9_RSYMLINK: make symlink response
 * @P9_TMKNOD: create a special file object request
 * @P9_RMKNOD: create a special file object response
 * @P9_TLCREATE: prepare a handle for I/O on an new file for 9P2000.L
 * @P9_RLCREATE: response with file access information for 9P2000.L
 * @P9_TRENAME: rename request
 * @P9_RRENAME: rename response
 * @P9_TMKDIR: create a directory request
 * @P9_RMKDIR: create a directory response
 * @P9_TVERSION: version handshake request
 * @P9_RVERSION: version handshake response
 * @P9_TAUTH: request to establish authentication channel
 * @P9_RAUTH: response with authentication information
 * @P9_TATTACH: establish user access to file service
 * @P9_RATTACH: response with top level handle to file hierarchy
 * @P9_TERROR: not used
 * @P9_RERROR: response for any failed request
 * @P9_TFLUSH: request to abort a previous request
 * @P9_RFLUSH: response when previous request has been cancelled
 * @P9_TWALK: descend a directory hierarchy
 * @P9_RWALK: response with new handle for position within hierarchy
 * @P9_TOPEN: prepare a handle for I/O on an existing file
 * @P9_ROPEN: response with file access information
 * @P9_TCREATE: prepare a handle for I/O on a new file
 * @P9_RCREATE: response with file access information
 * @P9_TREAD: request to transfer data from a file or directory
 * @P9_RREAD: response with data requested
 * @P9_TWRITE: reuqest to transfer data to a file
 * @P9_RWRITE: response with out much data was transferred to file
 * @P9_TCLUNK: forget about a handle to an entity within the file system
 * @P9_RCLUNK: response when server has forgotten about the handle
 * @P9_TREMOVE: request to remove an entity from the hierarchy
 * @P9_RREMOVE: response when server has removed the entity
 * @P9_TSTAT: request file entity attributes
 * @P9_RSTAT: response with file entity attributes
 * @P9_TWSTAT: request to update file entity attributes
 * @P9_RWSTAT: response when file entity attributes are updated
 *
 * There are 14 basic operations in 9P2000, paired as
 * requests and responses.  The one special case is ERROR
 * as there is no @P9_TERROR request for clients to transmit to
 * the server, but the server may respond to any other request
 * with an @P9_RERROR.
 *
 * See Also: http://plan9.bell-labs.com/sys/man/5/INDEX.html
 */
enum p9_msg_t {
	P9_TLERROR = 6,
	P9_RLERROR,
	P9_TSTATFS = 8,
	P9_RSTATFS,
	P9_TLOPEN = 12,
	P9_RLOPEN,
	P9_TLCREATE = 14,
	P9_RLCREATE,
	P9_TSYMLINK = 16,
	P9_RSYMLINK,
	P9_TMKNOD = 18,
	P9_RMKNOD,
	P9_TRENAME = 20,
	P9_RRENAME,
	P9_TREADLINK = 22,
	P9_RREADLINK,
	P9_TGETATTR = 24,
	P9_RGETATTR,
	P9_TSETATTR = 26,
	P9_RSETATTR,
	P9_TXATTRWALK = 30,
	P9_RXATTRWALK,
	P9_TXATTRCREATE = 32,
	P9_RXATTRCREATE,
	P9_TREADDIR = 40,
	P9_RREADDIR,
	P9_TFSYNC = 50,
	P9_RFSYNC,
	P9_TLOCK = 52,
	P9_RLOCK,
	P9_TGETLOCK = 54,
	P9_RGETLOCK,
	P9_TLINK = 70,
	P9_RLINK,
	P9_TMKDIR = 72,
	P9_RMKDIR,
	P9_TRENAMEAT = 74,
	P9_RRENAMEAT,
	P9_TUNLINKAT = 76,
	P9_RUNLINKAT,
	P9_TVERSION = 100,
	P9_RVERSION,
	P9_TAUTH = 102,
	P9_RAUTH,
	P9_TATTACH = 104,
	P9_RATTACH,
	P9_TERROR = 106,
	P9_RERROR,
	P9_TFLUSH = 108,
	P9_RFLUSH,
	P9_TWALK = 110,
	P9_RWALK,
	P9_TOPEN = 112,
	P9_ROPEN,
	P9_TCREATE = 114,
	P9_RCREATE,
	P9_TREAD = 116,
	P9_RREAD,
	P9_TWRITE = 118,
	P9_RWRITE,
	P9_TCLUNK = 120,
	P9_RCLUNK,
	P9_TREMOVE = 122,
	P9_RREMOVE,
	P9_TSTAT = 124,
	P9_RSTAT,
	P9_TWSTAT = 126,
	P9_RWSTAT,
};

/**
 * enum p9_open_mode_t - 9P open modes
 * @P9_OREAD: open file for reading only
 * @P9_OWRITE: open file for writing only
 * @P9_ORDWR: open file for reading or writing
 * @P9_OEXEC: open file for execution
 * @P9_OTRUNC: truncate file to zero-length before opening it
 * @P9_OREXEC: close the file when an exec(2) system call is made
 * @P9_ORCLOSE: remove the file when the file is closed
 * @P9_OAPPEND: open the file and seek to the end
 * @P9_OEXCL: only create a file, do not open it
 *
 * 9P open modes differ slightly from Posix standard modes.
 * In particular, there are extra modes which specify different
 * semantic behaviors than may be available on standard Posix
 * systems.  For example, @P9_OREXEC and @P9_ORCLOSE are modes that
 * most likely will not be issued from the Linux VFS client, but may
 * be supported by servers.
 *
 * See Also: http://plan9.bell-labs.com/magic/man2html/2/open
 */
enum p9_open_mode_t {
	P9_OREAD = 0x00,
	P9_OWRITE = 0x01,
	P9_ORDWR = 0x02,
	P9_OEXEC = 0x03,
	P9_OTRUNC = 0x10,
	P9_OREXEC = 0x20,
	P9_ORCLOSE = 0x40,
	P9_OAPPEND = 0x80,
	P9_OEXCL = 0x1000,
};

/**
 * enum p9_perm_t - 9P permissions
 * @P9_DMDIR: mode bit for directories
 * @P9_DMAPPEND: mode bit for is append-only
 * @P9_DMEXCL: mode bit for excluse use (only one open handle allowed)
 * @P9_DMMOUNT: mode bit for mount points
 * @P9_DMAUTH: mode bit for authentication file
 * @P9_DMTMP: mode bit for non-backed-up files
 * @P9_DMSYMLINK: mode bit for symbolic links (9P2000.u)
 * @P9_DMLINK: mode bit for hard-link (9P2000.u)
 * @P9_DMDEVICE: mode bit for device files (9P2000.u)
 * @P9_DMNAMEDPIPE: mode bit for named pipe (9P2000.u)
 * @P9_DMSOCKET: mode bit for socket (9P2000.u)
 * @P9_DMSETUID: mode bit for setuid (9P2000.u)
 * @P9_DMSETGID: mode bit for setgid (9P2000.u)
 * @P9_DMSETVTX: mode bit for sticky bit (9P2000.u)
 *
 * 9P permissions differ slightly from Posix standard modes.
 *
 * See Also: http://plan9.bell-labs.com/magic/man2html/2/stat
 */
enum p9_perm_t {
	P9_DMDIR = 0x80000000,
	P9_DMAPPEND = 0x40000000,
	P9_DMEXCL = 0x20000000,
	P9_DMMOUNT = 0x10000000,
	P9_DMAUTH = 0x08000000,
	P9_DMTMP = 0x04000000,
/* 9P2000.u extensions */
	P9_DMSYMLINK = 0x02000000,
	P9_DMLINK = 0x01000000,
	P9_DMDEVICE = 0x00800000,
	P9_DMNAMEDPIPE = 0x00200000,
	P9_DMSOCKET = 0x00100000,
	P9_DMSETUID = 0x00080000,
	P9_DMSETGID = 0x00040000,
	P9_DMSETVTX = 0x00010000,
};

/* 9p2000.L open flags */
#define P9_DOTL_RDONLY        00000000
#define P9_DOTL_WRONLY        00000001
#define P9_DOTL_RDWR          00000002
#define P9_DOTL_NOACCESS      00000003
#define P9_DOTL_CREATE        00000100
#define P9_DOTL_EXCL          00000200
#define P9_DOTL_NOCTTY        00000400
#define P9_DOTL_TRUNC         00001000
#define P9_DOTL_APPEND        00002000
#define P9_DOTL_NONBLOCK      00004000
#define P9_DOTL_DSYNC         00010000
#define P9_DOTL_FASYNC        00020000
#define P9_DOTL_DIRECT        00040000
#define P9_DOTL_LARGEFILE     00100000
#define P9_DOTL_DIRECTORY     00200000
#define P9_DOTL_NOFOLLOW      00400000
#define P9_DOTL_NOATIME       01000000
#define P9_DOTL_CLOEXEC       02000000
#define P9_DOTL_SYNC          04000000

/* 9p2000.L at flags */
#define P9_DOTL_AT_REMOVEDIR		0x200

/* 9p2000.L lock type */
#define P9_LOCK_TYPE_RDLCK 0
#define P9_LOCK_TYPE_WRLCK 1
#define P9_LOCK_TYPE_UNLCK 2

/**
 * enum p9_qid_t - QID types
 * @P9_QTDIR: directory
 * @P9_QTAPPEND: append-only
 * @P9_QTEXCL: excluse use (only one open handle allowed)
 * @P9_QTMOUNT: mount points
 * @P9_QTAUTH: authentication file
 * @P9_QTTMP: non-backed-up files
 * @P9_QTSYMLINK: symbolic links (9P2000.u)
 * @P9_QTLINK: hard-link (9P2000.u)
 * @P9_QTFILE: normal files
 *
 * QID types are a subset of permissions - they are primarily
 * used to differentiate semantics for a file system entity via
 * a jump-table.  Their value is also the most significant 16 bits
 * of the permission_t
 *
 * See Also: http://plan9.bell-labs.com/magic/man2html/2/stat
 */
enum p9_qid_t {
	P9_QTDIR = 0x80,
	P9_QTAPPEND = 0x40,
	P9_QTEXCL = 0x20,
	P9_QTMOUNT = 0x10,
	P9_QTAUTH = 0x08,
	P9_QTTMP = 0x04,
	P9_QTSYMLINK = 0x02,
	P9_QTLINK = 0x01,
	P9_QTFILE = 0x00,
};

/* 9P Magic Numbers */
#define P9_NOTAG	(uint16_t)(~0)
#define P9_NOFID	(uint32_t)(~0)
#define P9_MAXWELEM	16

/* ample room for Twrite/Rread header */
#define P9_IOHDRSZ	24

/* Room for readdir header */
#define P9_READDIRHDRSZ	24

/* size of header for zero copy read/write */
#define P9_ZC_HDR_SZ 4096


/**
 * struct p9_qid - file system entity information
 * @type: 8-bit type &p9_qid_t
 * @version: 16-bit monotonically incrementing version number
 * @path: 64-bit per-server-unique ID for a file system element
 *
 * qids are identifiers used by 9P servers to track file system
 * entities.  The type is used to differentiate semantics for operations
 * on the entity (ie. read means something different on a directory than
 * on a file).  The path provides a server unique index for an entity
 * (roughly analogous to an inode number), while the version is updated
 * every time a file is modified and can be used to maintain cache
 * coherency between clients and serves.
 * Servers will often differentiate purely synthetic entities by setting
 * their version to 0, signaling that they should never be cached and
 * should be accessed synchronously.
 *
 * See Also://plan9.bell-labs.com/magic/man2html/2/stat
 */
struct p9_qid {
	uint8_t type;
	uint32_t version;
	uint64_t path;
};

typedef struct {
	uid_t val;
} kuid_t;


typedef struct {
	gid_t val;
} kgid_t;

/**
 * struct p9_wstat - file system metadata information
 * @size: length prefix for this stat structure instance
 * @type: the type of the server (equivalent to a major number)
 * @dev: the sub-type of the server (equivalent to a minor number)
 * @qid: unique id from the server of type &p9_qid
 * @mode: Plan 9 format permissions of type &p9_perm_t
 * @atime: Last access/read time
 * @mtime: Last modify/write time
 * @length: file length
 * @name: last element of path (aka filename)
 * @uid: owner name
 * @gid: group owner
 * @muid: last modifier
 * @extension: area used to encode extended UNIX support
 * @n_uid: numeric user id of owner (part of 9p2000.u extension)
 * @n_gid: numeric group id (part of 9p2000.u extension)
 * @n_muid: numeric user id of laster modifier (part of 9p2000.u extension)
 *
 * See Also: http://plan9.bell-labs.com/magic/man2html/2/stat
 */
struct p9_wstat {
	uint16_t size;
	uint16_t type;
	uint32_t dev;
	struct p9_qid qid;
	uint32_t mode;
	uint32_t atime;
	uint32_t mtime;
	uint64_t length;
	const char *name;
	const char *uid;
	const char *gid;
	const char *muid;
	char *extension;	/* 9p2000.u extensions */
	kuid_t n_uid;		/* 9p2000.u extensions */
	kgid_t n_gid;		/* 9p2000.u extensions */
	kuid_t n_muid;		/* 9p2000.u extensions */
};

struct p9_stat_dotl {
	uint64_t st_result_mask;
	struct p9_qid qid;
	uint32_t st_mode;
	kuid_t st_uid;
	kgid_t st_gid;
	uint64_t st_nlink;
	uint64_t st_rdev;
	uint64_t st_size;
	uint64_t st_blksize;
	uint64_t st_blocks;
	uint64_t st_atime_sec;
	uint64_t st_atime_nsec;
	uint64_t st_mtime_sec;
	uint64_t st_mtime_nsec;
	uint64_t st_ctime_sec;
	uint64_t st_ctime_nsec;
	uint64_t st_btime_sec;
	uint64_t st_btime_nsec;
	uint64_t st_gen;
	uint64_t st_data_version;
};

#define P9_STATS_MODE		0x00000001ULL
#define P9_STATS_NLINK		0x00000002ULL
#define P9_STATS_UID		0x00000004ULL
#define P9_STATS_GID		0x00000008ULL
#define P9_STATS_RDEV		0x00000010ULL
#define P9_STATS_ATIME		0x00000020ULL
#define P9_STATS_MTIME		0x00000040ULL
#define P9_STATS_CTIME		0x00000080ULL
#define P9_STATS_INO		0x00000100ULL
#define P9_STATS_SIZE		0x00000200ULL
#define P9_STATS_BLOCKS		0x00000400ULL

#define P9_STATS_BTIME		0x00000800ULL
#define P9_STATS_GEN		0x00001000ULL
#define P9_STATS_DATA_VERSION	0x00002000ULL

#define P9_STATS_BASIC		0x000007ffULL /* Mask for fields up to BLOCKS */
#define P9_STATS_ALL		0x00003fffULL /* Mask for All fields above */

/**
 * struct p9_iattr_dotl - P9 inode attribute for setattr
 * @valid: bitfield specifying which fields are valid
 *         same as in struct iattr
 * @mode: File permission bits
 * @uid: user id of owner
 * @gid: group id
 * @size: File size
 * @atime_sec: Last access time, seconds
 * @atime_nsec: Last access time, nanoseconds
 * @mtime_sec: Last modification time, seconds
 * @mtime_nsec: Last modification time, nanoseconds
 */
struct p9_iattr_dotl {
	uint32_t valid;
	uint32_t mode;
	kuid_t uid;
	kgid_t gid;
	uint64_t size;
	uint64_t atime_sec;
	uint64_t atime_nsec;
	uint64_t mtime_sec;
	uint64_t mtime_nsec;
};

#define P9_LOCK_SUCCESS 0
#define P9_LOCK_BLOCKED 1
#define P9_LOCK_ERROR 2
#define P9_LOCK_GRACE 3

#define P9_LOCK_FLAGS_BLOCK 1
#define P9_LOCK_FLAGS_RECLAIM 2

/**
 * struct p9_flock: POSIX lock structure
 * @type - type of lock
 * @flags - lock flags
 * @start - starting offset of the lock
 * @length - number of bytes
 * @proc_id - process id which wants to take lock
 * @client_id - client id
 */
struct p9_flock {
	uint8_t type;
	uint32_t flags;
	uint64_t start;
	uint64_t length;
	uint32_t proc_id;
	char *client_id;
};

/**
 * struct p9_getlock: getlock structure
 * @type - type of lock
 * @start - starting offset of the lock
 * @length - number of bytes
 * @proc_id - process id which wants to take lock
 * @client_id - client id
 */
struct p9_getlock {
	uint8_t type;
	uint64_t start;
	uint64_t length;
	uint32_t proc_id;
	char *client_id;
};

struct p9_rstatfs {
	uint32_t type;
	uint32_t bsize;
	uint64_t blocks;
	uint64_t bfree;
	uint64_t bavail;
	uint64_t files;
	uint64_t ffree;
	uint64_t fsid;
	uint32_t namelen;
};

/**
 * struct p9_fcall - primary packet structure
 * @size: prefixed length of the structure
 * @id: protocol operating identifier of type &p9_msg_t
 * @tag: transaction id of the request
 * @offset: used by marshalling routines to track current position in buffer
 * @capacity: used by marshalling routines to track total malloc'd capacity
 * @sdata: payload
 *
 * &p9_fcall represents the structure for all 9P RPC
 * transactions.  Requests are packaged into fcalls, and reponses
 * must be extracted from them.
 *
 * See Also: http://plan9.bell-labs.com/magic/man2html/2/fcall
 */
struct p9_fcall {
	uint32_t size;
	uint8_t id;
	uint16_t tag;

	size_t offset;
	size_t capacity;

	uint8_t *sdata;
};

#define U32_MAX_DIGITS 10

#define MINORBITS   20
#define MINORMASK   ((1U << MINORBITS) - 1)

#define MAJOR(dev)  ((unsigned int) ((dev) >> MINORBITS))
#define MINOR(dev)  ((unsigned int) ((dev) & MINORMASK))
#define MKDEV(ma,mi)    (((ma) << MINORBITS) | (mi))

/* Number of requests per row */
#define P9_ROW_MAXTAG 255

#define __NEW_UTS_LEN 64

/**
 * enum p9_proto_versions - 9P protocol versions
 * @p9_proto_legacy: 9P Legacy mode, pre-9P2000.u
 * @p9_proto_2000u: 9P2000.u extension
 * @p9_proto_2000L: 9P2000.L extension
 */
enum p9_proto_versions{
	p9_proto_legacy,
	p9_proto_2000u,
	p9_proto_2000L,
};

/**
 * enum p9_trans_status - different states of underlying transports
 * @Connected: transport is connected and healthy
 * @Disconnected: transport has been disconnected
 * @Hung: transport is connected by wedged
 *
 * This enumeration details the various states a transport
 * instatiation can be in.
 */
enum p9_trans_status {
	Connected,
	BeginDisconnect,
	Disconnected,
	Hung,
};

/**
 * enum p9_req_status_t - status of a request
 * @REQ_STATUS_IDLE: request slot unused
 * @REQ_STATUS_ALLOC: request has been allocated but not sent
 * @REQ_STATUS_UNSENT: request waiting to be sent
 * @REQ_STATUS_SENT: request sent to server
 * @REQ_STATUS_RCVD: response received from server
 * @REQ_STATUS_FLSHD: request has been flushed
 * @REQ_STATUS_ERROR: request encountered an error on the client side
 *
 * The @REQ_STATUS_IDLE state is used to mark a request slot as unused
 * but use is actually tracked by the idpool structure which handles tag
 * id allocation.
 *
 */
enum p9_req_status_t {
	REQ_STATUS_IDLE,
	REQ_STATUS_ALLOC,
	REQ_STATUS_UNSENT,
	REQ_STATUS_SENT,
	REQ_STATUS_RCVD,
	REQ_STATUS_FLSHD,
	REQ_STATUS_ERROR,
};

/**
 * struct p9_req_t - request slots
 * @status: status of this request slot
 * @t_err: transport error
 * @flush_tag: tag of request being flushed (for flush requests)
 * @wq: wait_queue for the client to block on for this request
 * @tc: the request fcall structure
 * @rc: the response fcall structure
 * @aux: transport specific data (provided for trans_fd migration)
 * @req_list: link for higher level objects to chain requests
 *
 * Transport use an array to track outstanding requests
 * instead of a list.  While this may incurr overhead during initial
 * allocation or expansion, it makes request lookup much easier as the
 * tag id is a index into an array.  (We use tag+1 so that we can accommodate
 * the -1 tag for the T_VERSION request).
 * This also has the nice effect of only having to allocate wait_queues
 * once, instead of constantly allocating and freeing them.  Its possible
 * other resources could benefit from this scheme as well.
 *
 */
struct p9_req_t {
	int status;
	int t_err;
	// struct kref refcount;
	// wait_queue_head_t *wq;
	struct p9_fcall *tc;
	struct p9_fcall *rc;
	void *aux;

	// struct list_head req_list;
};

class p9_client;

/**
 * struct p9_fid - file system entity handle
 * @clnt: back pointer to instantiating &p9_client
 * @fid: numeric identifier for this handle
 * @mode: current mode of this fid (enum?)
 * @qid: the &p9_qid server identifier this handle points to
 * @iounit: the server reported maximum transaction size for this file
 * @uid: the numeric uid of the local user who owns this handle
 * @rdir: readdir accounting structure (allocated on demand)
 * @flist: per-client-instance fid tracking
 * @dlist: per-dentry fid tracking
 *
 * TODO: This needs lots of explanation.
 */
struct p9_fid {
	p9_client *clnt;
	u32 fid;
	int mode;
	struct p9_qid qid;
	u32 iounit;
	kuid_t uid;

	void *rdir;

	// struct list_head flist;
	// struct hlist_node dlist;	/* list of all fids attached to a dentry */
};

/**
 * struct p9_dirent - directory entry structure
 * @qid: The p9 server qid for this dirent
 * @d_off: offset to the next dirent
 * @d_type: type of file
 * @d_name: file name
 */
struct p9_dirent {
	struct p9_qid qid;
	u64 d_off;
	unsigned char d_type;
	char d_name[256];
};


/**
 * class p9_idpool - per-connection accounting for tag idpool
 * @_lock: spin lock to protect @_pool
 * @_pool: pool for id accounting
 * @_last_id : the last allocated id
 */
class p9_idpool
{
public:
	const static int P9_ID_MAX = INT_MAX - 1; 
public:
	p9_idpool(): _last_id(-1) {}
	~p9_idpool();

	int p9_idpool_get();
	void p9_idpool_put(int id);
	bool p9_idpool_check(int id);

private:
	irq_save_lock_type _lock;
	std::unordered_set<int> _pool;
	int _last_id;
};

class seq
{

public:
	seq(unsigned long size);
	~seq();

	void seq_printf(const char *f, ...);
	void seq_putc(char c);
	void seq_puts(const char *s);
	int seq_write(const void *data, size_t len);

private:
	void seq_vprintf(const char *f, va_list args);

private:
	char *_buf;
	size_t _size;
	size_t _count;

};

#if __BYTE_ORDER == __BIG_ENDIAN
#define cpu_to_le16 bswap_16
#define cpu_to_le32 bswap_32
#define cpu_to_le64 bswap_64
#define le16_to_cpu bswap_16
#define le32_to_cpu bswap_32
#define le64_to_cpu bswap_64
#define cpu_to_be16
#define cpu_to_be32
#define cpu_to_be64
#define be16_to_cpu
#define be32_to_cpu
#define be64_to_cpu
#else
#define cpu_to_le16
#define cpu_to_le32
#define cpu_to_le64
#define le16_to_cpu
#define le32_to_cpu
#define le64_to_cpu
#define cpu_to_be16 bswap_16
#define cpu_to_be32 bswap_32
#define cpu_to_be64 bswap_64
#define be16_to_cpu bswap_16
#define be32_to_cpu bswap_32
#define be64_to_cpu bswap_64
#endif

#ifdef __CHECKER__
#define __bitwise__ __attribute__((bitwise))
#else
#define __bitwise__
#endif
#define __bitwise __bitwise__

typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;

typedef __u16 __bitwise __le16;
typedef __u16 __bitwise __be16;
typedef __u32 __bitwise __le32;
typedef __u32 __bitwise __be32;
typedef __u64 __bitwise __le64;
typedef __u64 __bitwise __be64;

#define BUG() do { \
	printf("BUG: failure at %s:%d/%s()!\n", __FILE__, __LINE__, __func__); \
} while (0)

#define ARRAY_SIZE(array) (sizeof(array) / sizeof(array[0]))

struct match_token {
	int token;
	const char *pattern;
};

typedef struct match_token match_table_t[];

/* Maximum number of arguments that match_token will find in a pattern */
enum {MAX_OPT_ARGS = 3};

/* Describe the location within a string of a substring */
typedef struct {
	char *from;
	char *to;
} substring_t;


struct p9_trans_module {
	char *name;		/* name of transport */
	int maxsize;		/* max message size of transport */
	int def;		/* this transport should be default */
	// struct module *owner;
	int (*create)(struct p9_client *, const char *, char *);
	void (*close) (struct p9_client *);
	int (*request) (struct p9_client *, struct p9_req_t *req);
	int (*cancel) (struct p9_client *, struct p9_req_t *req);
	int (*cancelled)(struct p9_client *, struct p9_req_t *req);
	int (*zc_request)(struct p9_client *, struct p9_req_t *,
			  struct iov_iter *, struct iov_iter *, int , int, int);
	int (*show_options)(struct seq *, struct p9_client *);
};

extern struct p9_trans_module p9_virtio_trans;

/**
 * class p9_client - per client instance state
 * @lock: protect @fidlist
 * @msize: maximum data size negotiated by protocol
 * @dotu: extension flags negotiated by protocol
 * @proto_version: 9P protocol version to use
 * @trans_mod: module API instantiated with this client
 * @trans: tranport instance state and API
 * @fidpool: fid handle accounting for session
 * @fidlist: List of active fid handles
 * @tagpool - transaction id accounting for session
 * @reqs - 2D array of requests
 * @max_tag - current maximum tag id allocated
 * @name - node name used as client id
 *
 * The client structure is used to keep track of various per-client
 * state that has been instantiated.
 * In order to minimize per-transaction overhead we use a
 * simple array to lookup requests instead of a hash table
 * or linked list.  In order to support larger number of
 * transactions, we make this a 2D array, allocating new rows
 * when we need to grow the total number of the transactions.
 *
 * Each row is 256 requests and we'll support up to 256 rows for
 * a total of 64k concurrent requests per session.
 *
 * Bugs: duplicated data and potentially unnecessary elements.
 */
class p9_client {

public:
	p9_client(const char *dev_name, char *options);
	~p9_client();

	/* GETTER FUNCTIONS */
	int p9_proto();
	int p9_msize();
	void *p9_trans();

	/* PROTOCOL VERSION FUNCTIONS */
	int p9_is_proto_dotu();
	int p9_is_proto_dotl();

	int p9_client_show_options(seq *s);

	/* CLIENT CONNECTION FUNCTIONS */
	void p9_client_connect(void *trans);
	struct p9_fid *p9_client_attach(struct p9_fid *afid, 
		const char *uname, kuid_t n_uname, const char *aname);
	void p9_client_begin_disconnect();
	void p9_client_disconnect();


	/* STATIC FUNCTIONS >> */

	static void p9_client_cb(struct p9_req_t *req, int status);

	static int p9_client_clunk(struct p9_fid *fid);

	static int p9_client_statfs(struct p9_fid *fid, struct p9_rstatfs *sb);

	static struct p9_fid *p9_client_walk(struct p9_fid *oldfid, uint16_t nwname, 
		const unsigned char * const *wnames, int clone);

	static int p9_client_open(struct p9_fid *fid, int mode);

	static int p9_client_read(struct p9_fid *fid, struct uio *to, size_t len, int *err);
	static int p9_client_write(struct p9_fid *fid, struct uio *from, size_t len, int *err);
	static int p9_client_fsync(struct p9_fid *fid, int datasync);


	static int p9_client_readdir_dotl(struct p9_fid *fid, char *data, u32 count, u64 offset);

	static int p9_client_readlink_dotl(struct p9_fid *fid, char **target);
	
	static int p9_client_fcreate(struct p9_fid *fid, const char *name, u32 perm, int mode, 
		char *extension);
	static int p9_client_fcreate_dotl(struct p9_fid *ofid, const char *name, u32 flags, u32 mode, 
		kgid_t gid, struct p9_qid *qid);

	static int p9_client_mknod_dotl(struct p9_fid *fid, const char *name, int mode, 
		dev_t rdev, kgid_t gid, struct p9_qid *qid);

	static int p9_client_link_dotl(struct p9_fid *dfid, struct p9_fid *oldfid, const char *newname);
	static int p9_client_symlink_dotl(struct p9_fid *dfid, const char *name, const char *symtgt, 
		kgid_t gid, struct p9_qid *qid);

	static int p9_client_mkdir_dotl(struct p9_fid *fid, const char *name, int mode, 
		kgid_t gid, struct p9_qid *qid);

	static int p9_client_remove(struct p9_fid *fid);
	static int p9_client_unlinkat_dotl(struct p9_fid *dfid, const char *name, int flags);

	static int p9_client_rename(struct p9_fid *fid, struct p9_fid *newdirfid, 
		const char *name);
	static int p9_client_renameat_dotl(struct p9_fid *olddirfid, const char *old_name, 
		struct p9_fid *newdirfid, const char *new_name);


	static struct p9_wstat *p9_client_getattr(struct p9_fid *fid);
	static int p9_client_setattr(struct p9_fid *fid, struct p9_wstat *wst);
	static struct p9_stat_dotl *p9_client_getattr_dotl(struct p9_fid *fid, u64 request_mask);
	static int p9_client_setattr_dotl(struct p9_fid *fid, struct p9_iattr_dotl *p9attr);

	static int p9_client_lock_dotl(struct p9_fid *fid, struct p9_flock *flock, u8 *status);
	static int p9_client_getlock_dotl(struct p9_fid *fid, struct p9_getlock *glock);

	static struct p9_fid *p9_client_xattrwalk_dotl(struct p9_fid *file_fid, 
		const char *attr_name, u64 *attr_size);
	static int p9_client_xattrcreate_dotl(struct p9_fid *fid, const char *name, 
		u64 attr_size, int flag);

	/* << STATIC FUNCTIONS */

private:
	/* OPTIONS FUNCTION */
	int p9_parse_options(const char *opts);

	/* REQ FUNCTIONS */
	struct p9_req_t *p9_alloc_req(u16 tag, unsigned int max_size);
	struct p9_req_t *p9_lookup_req(u16 tag);
	void p9_free_req(struct p9_req_t *r);

	int p9_check_errors(struct p9_req_t *req);


	struct p9_req_t *p9_client_prepare_req(int8_t type, int req_size, 
		const char *fmt, va_list ap);

	/* RPC FUNCTION*/
	struct p9_req_t *p9_client_rpc(int8_t type, 
		const char *fmt, ...);

	/* FID FUNCTIONS */
	struct p9_fid *p9_fid_create();
	void p9_fid_destroy(struct p9_fid *fid);

	/* MESSAGE FUNCTIONS */
	int p9_client_flush(struct p9_req_t *oldreq);
	int p9_client_version();

private:

	irq_save_lock_type _lock; /* protect client structure */
	unsigned int _msize;
	unsigned char _proto_version;
	struct p9_trans_module *_trans_mod;
	enum p9_trans_status _status;
	void *_trans;

	union {
		struct {
			int rfd;
			int wfd;
		} fd;
		struct {
			u16 port;
			bool privport;

		} tcp;
	} _trans_opts;

	p9_idpool _fidpool;
	std::list<struct p9_fid *> _fidlist;

	p9_idpool _tagpool;
	struct p9_req_t *_reqs[P9_ROW_MAXTAG];
	int _max_tag;

	char _name[__NEW_UTS_LEN + 1];

};


/* Utils */
void p9stat_init(struct p9_wstat *stbuf);
int p9stat_read(struct p9_client *, char *, int, struct p9_wstat *);
void p9stat_free(struct p9_wstat *);

int p9dirent_read(struct p9_client *clnt, char *buf, int len, 
	struct p9_dirent *dirent);

int match_token(char *, const match_table_t table, substring_t args[]);
int match_int(substring_t *, int *result);
char *match_strdup(const substring_t *);

#endif