/*
 *
 *  VirtFS Client - Only virtio transport is supportted
 *
 */

#include <osv/p9client.hh>
#include <osv/mutex.h>
#include <api/stdarg.h>
#include <osv/uio.h>
#include <osv/debug.h>
#include <unordered_map>
#include <drivers/virtio-9p.hh>


p9_idpool::~p9_idpool()
{
	_pool.clear();
	_last_id = -1;
}

int p9_idpool::p9_idpool_get()
{
	int last = _last_id;
	int id = last;

	WITH_LOCK(_lock)
	{
		do
		{
			id++;
			if (id > P9_ID_MAX)
			{
				id = 0;
			}
			if (!_pool.count(id))
			{
				_pool.insert(id);
				_last_id = id;
				return id;
			}
		} while (id != last);

		return -1;
	}
}

void p9_idpool::p9_idpool_put(int id)
{
	WITH_LOCK(_lock)
	{
		_pool.erase(id);
	}
}

bool p9_idpool::p9_idpool_check(int id)
{
	return _pool.count(id);
}





seq::seq(unsigned long size)
:_buf((char *) malloc(size)), _size(size), _count(0)
{
}

seq::~seq()
{
	free(_buf);
	_buf = nullptr;
	_size = 0;
	_count = 0;
}

void seq::seq_printf(const char *f, ...)
{
	va_list args;

	va_start(args, f);
	seq_vprintf(f, args);
	va_end(args);
}


void seq::seq_putc(char c)
{
	if (_count >= _size)
		return;

	_buf[_count++] = c;
}

void seq::seq_puts(const char *s)
{
	int len = strlen(s);

	if (_count + len >= _size)
	{
		_count = _size;
		return;
	}

	memcpy(_buf + _count, s, len);
	_count += len;
}

int seq::seq_write(const void *data, size_t len)
{
	if (_count + len < _size)
	{
		memcpy(_buf + _count, data, len);
		_count += len;
		return 0;
	}

	_count = _size;
	return -1;
}

void seq::seq_vprintf(const char *f, va_list args)
{
	int len;

	if (_count < _size)
	{
		len = vsnprintf(_buf + _count, _size - _count, f, args);
		if (_count + len < _size)
		{
			_count += len;
			return;
		}
	}
	_count = _size;
}



static size_t p9pdu_read(struct p9_fcall *pdu, void *data, size_t size);
static size_t p9pdu_write(struct p9_fcall *pdu, const void *data, size_t size);

static size_t p9pdu_write_u(struct p9_fcall *pdu, struct uio *from, size_t size);

static int p9pdu_vreadf(struct p9_fcall *pdu, int proto_version, const char *fmt, 
	va_list ap);
static int p9pdu_vwritef(struct p9_fcall *pdu, int proto_version, const char *fmt, 
	va_list ap);

static int p9pdu_readf(struct p9_fcall *pdu, int proto_version, const char *fmt, ...);
static int p9pdu_writef(struct p9_fcall *pdu, int proto_version, const char *fmt, ...);

static int p9pdu_prepare(struct p9_fcall *pdu, int16_t tag, int8_t type);
static int p9pdu_finalize(struct p9_client *clnt, struct p9_fcall *pdu);
static void p9pdu_reset(struct p9_fcall *pdu);

/**
 * p9du_read - read data from pdu 
 */
static size_t p9pdu_read(struct p9_fcall *pdu, void *data, size_t size)
{
	size_t len = std::min<size_t>(pdu->size - pdu->offset, size);
	memcpy(data, &pdu->sdata[pdu->offset], len);
	pdu->offset += len;
	return size - len;
}

/**
 * p9du_write - write data to pdu
 */
static size_t p9pdu_write(struct p9_fcall *pdu, const void *data, size_t size)
{
	size_t len = std::min<size_t>(pdu->capacity - pdu->size, size);
	memcpy(&pdu->sdata[pdu->size], data, len);
	pdu->size += len;
	return size - len;
}

static size_t p9pdu_write_u(struct p9_fcall *pdu, struct uio *from, size_t size)
{
	size_t len = std::min<size_t>(pdu->capacity - pdu->size, size);
	len = uiomove(&pdu->sdata[pdu->size], len, from);
	pdu->size += len;
	return size - len;
}

/*
	b - int8_t
	w - int16_t
	d - int32_t
	q - int64_t
	s - string
	u - numeric uid
	g - numeric gid
	S - stat
	Q - qid
	D - data blob (int32_t size followed by void *, results are not freed)
	T - array of strings (int16_t count, followed by strings)
	R - array of qids (int16_t count, followed by qids)
	A - stat for 9p2000.L (p9_stat_dotl)
	? - if optional = 1, continue parsing
*/
static int p9pdu_vreadf(struct p9_fcall *pdu, int proto_version, const char *fmt,
	va_list ap)
{
	const char *ptr;
	int errcode = 0;

	for (ptr = fmt; *ptr; ptr++) {
		switch (*ptr) {
		case 'b':{
				int8_t *val = va_arg(ap, int8_t *);
				if (p9pdu_read(pdu, val, sizeof(*val))) {
					errcode = -EFAULT;
					break;
				}
			}
			break;
		case 'w':{
				int16_t *val = va_arg(ap, int16_t *);
				__le16 le_val;
				if (p9pdu_read(pdu, &le_val, sizeof(le_val))) {
					errcode = -EFAULT;
					break;
				}
				*val = le16_to_cpu(le_val);
			}
			break;
		case 'd':{
				int32_t *val = va_arg(ap, int32_t *);
				__le32 le_val;
				if (p9pdu_read(pdu, &le_val, sizeof(le_val))) {
					errcode = -EFAULT;
					break;
				}
				*val = le32_to_cpu(le_val);
			}
			break;
		case 'q':{
				int64_t *val = va_arg(ap, int64_t *);
				__le64 le_val;
				if (p9pdu_read(pdu, &le_val, sizeof(le_val))) {
					errcode = -EFAULT;
					break;
				}
				*val = le64_to_cpu(le_val);
			}
			break;
		case 's':{
				char **sptr = va_arg(ap, char **);
				uint16_t len;

				errcode = p9pdu_readf(pdu, proto_version,
								"w", &len);
				if (errcode)
					break;

				*sptr = (char *) malloc(len + 1);
				if (*sptr == NULL) {
					errcode = -EFAULT;
					break;
				}
				if (p9pdu_read(pdu, *sptr, len)) {
					errcode = -EFAULT;
					free(*sptr);
					*sptr = NULL;
				} else
					(*sptr)[len] = 0;
			}
			break;
		case 'u': {
				kuid_t *uid = va_arg(ap, kuid_t *);
				__le32 le_val;
				if (p9pdu_read(pdu, &le_val, sizeof(le_val))) {
					errcode = -EFAULT;
					break;
				}
				// *uid = make_kuid(&init_user_ns,
				// 		 le32_to_cpu(le_val));
				// uid is meaningless in OSv
				uid->val = le32_to_cpu(le_val);
			} break;
		case 'g': {
				kgid_t *gid = va_arg(ap, kgid_t *);
				__le32 le_val;
				if (p9pdu_read(pdu, &le_val, sizeof(le_val))) {
					errcode = -EFAULT;
					break;
				}
				// *gid = make_kgid(&init_user_ns,
				// 		 le32_to_cpu(le_val));
				// gid is meaningless is OSv
				gid->val = le32_to_cpu(le_val);
			} break;
		case 'Q':{
				struct p9_qid *qid =
				    va_arg(ap, struct p9_qid *);

				errcode = p9pdu_readf(pdu, proto_version, "bdq",
						      &qid->type, &qid->version,
						      &qid->path);
			}
			break;
		case 'S':{
				struct p9_wstat *stbuf =
				    va_arg(ap, struct p9_wstat *);

				memset(stbuf, 0, sizeof(struct p9_wstat));
				stbuf->n_uid = stbuf->n_muid = (kuid_t) {(unsigned) ~0};
				stbuf->n_gid = (kgid_t) {(unsigned) ~0};

				errcode =
				    p9pdu_readf(pdu, proto_version,
						"wwdQdddqssss?sugu",
						&stbuf->size, &stbuf->type,
						&stbuf->dev, &stbuf->qid,
						&stbuf->mode, &stbuf->atime,
						&stbuf->mtime, &stbuf->length,
						&stbuf->name, &stbuf->uid,
						&stbuf->gid, &stbuf->muid,
						&stbuf->extension,
						&stbuf->n_uid, &stbuf->n_gid,
						&stbuf->n_muid);
				if (errcode)
					p9stat_free(stbuf);
			}
			break;
		case 'D':{
				uint32_t *count = va_arg(ap, uint32_t *);
				void **data = va_arg(ap, void **);

				errcode =
				    p9pdu_readf(pdu, proto_version, "d", count);
				if (!errcode) {
					*count = std::min<uint32_t>(*count,
						  pdu->size - pdu->offset);
					*data = &pdu->sdata[pdu->offset];
				}
			}
			break;
		case 'T':{
				uint16_t *nwname = va_arg(ap, uint16_t *);
				char ***wnames = va_arg(ap, char ***);

				errcode = p9pdu_readf(pdu, proto_version,
								"w", nwname);
				if (!errcode) {
					*wnames = (char **) malloc(sizeof(char *) * *nwname);
					if (!*wnames)
						errcode = -ENOMEM;
				}

				if (!errcode) {
					int i;

					for (i = 0; i < *nwname; i++) {
						errcode =
						    p9pdu_readf(pdu,
								proto_version,
								"s",
								&(*wnames)[i]);
						if (errcode)
							break;
					}
				}

				if (errcode) {
					if (*wnames) {
						int i;

						for (i = 0; i < *nwname; i++)
							free((*wnames)[i]);
					}
					free(*wnames);
					*wnames = NULL;
				}
			}
			break;
		case 'R':{
				uint16_t *nwqid = va_arg(ap, uint16_t *);
				struct p9_qid **wqids =
				    va_arg(ap, struct p9_qid **);

				*wqids = NULL;

				errcode =
				    p9pdu_readf(pdu, proto_version, "w", nwqid);
				if (!errcode) {
					*wqids = (struct p9_qid *) 
					    malloc(*nwqid * sizeof(struct p9_qid));
					if (*wqids == NULL)
						errcode = -ENOMEM;
				}

				if (!errcode) {
					int i;

					for (i = 0; i < *nwqid; i++) {
						errcode =
						    p9pdu_readf(pdu,
								proto_version,
								"Q",
								&(*wqids)[i]);
						if (errcode)
							break;
					}
				}

				if (errcode) {
					free(*wqids);
					*wqids = NULL;
				}
			}
			break;
		case 'A': {
				struct p9_stat_dotl *stbuf =
				    va_arg(ap, struct p9_stat_dotl *);

				memset(stbuf, 0, sizeof(struct p9_stat_dotl));
				errcode =
				    p9pdu_readf(pdu, proto_version,
					"qQdugqqqqqqqqqqqqqqq",
					&stbuf->st_result_mask,
					&stbuf->qid,
					&stbuf->st_mode,
					&stbuf->st_uid, &stbuf->st_gid,
					&stbuf->st_nlink,
					&stbuf->st_rdev, &stbuf->st_size,
					&stbuf->st_blksize, &stbuf->st_blocks,
					&stbuf->st_atime_sec,
					&stbuf->st_atime_nsec,
					&stbuf->st_mtime_sec,
					&stbuf->st_mtime_nsec,
					&stbuf->st_ctime_sec,
					&stbuf->st_ctime_nsec,
					&stbuf->st_btime_sec,
					&stbuf->st_btime_nsec,
					&stbuf->st_gen,
					&stbuf->st_data_version);
			}
			break;
		case '?':
			if ((proto_version != p9_proto_2000u) &&
				(proto_version != p9_proto_2000L))
				return 0;
			break;
		default:
			BUG();
			break;
		}

		if (errcode)
			break;
	}

	return errcode;
}

static int p9pdu_vwritef(struct p9_fcall *pdu, int proto_version, const char *fmt,
	va_list ap)
{
	const char *ptr;
	int errcode = 0;

	for (ptr = fmt; *ptr; ptr++) {
		switch (*ptr) {
		case 'b':{
				int8_t val = va_arg(ap, int);
				if (p9pdu_write(pdu, &val, sizeof(val)))
					errcode = -EFAULT;
			}
			break;
		case 'w':{
				__le16 val = cpu_to_le16(va_arg(ap, int));
				if (p9pdu_write(pdu, &val, sizeof(val)))
					errcode = -EFAULT;
			}
			break;
		case 'd':{
				__le32 val = cpu_to_le32(va_arg(ap, int32_t));
				if (p9pdu_write(pdu, &val, sizeof(val)))
					errcode = -EFAULT;
			}
			break;
		case 'q':{
				__le64 val = cpu_to_le64(va_arg(ap, int64_t));
				if (p9pdu_write(pdu, &val, sizeof(val)))
					errcode = -EFAULT;
			}
			break;
		case 's':{
				const char *sptr = va_arg(ap, const char *);
				uint16_t len = 0;
				if (sptr)
					len = std::min<size_t>(strlen(sptr),
								USHRT_MAX);

				errcode = p9pdu_writef(pdu, proto_version,
								"w", len);
				if (!errcode && p9pdu_write(pdu, sptr, len))
					errcode = -EFAULT;
			}
			break;
		case 'u': {
				kuid_t uid = va_arg(ap, kuid_t);
				__le32 val = cpu_to_le32(uid.val);
				if (p9pdu_write(pdu, &val, sizeof(val)))
					errcode = -EFAULT;
			} break;
		case 'g': {
				kgid_t gid = va_arg(ap, kgid_t);
				__le32 val = cpu_to_le32(gid.val);
				if (p9pdu_write(pdu, &val, sizeof(val)))
					errcode = -EFAULT;
			} break;
		case 'Q':{
				const struct p9_qid *qid =
				    va_arg(ap, const struct p9_qid *);
				errcode =
				    p9pdu_writef(pdu, proto_version, "bdq",
						 qid->type, qid->version,
						 qid->path);
			} break;
		case 'S':{
				const struct p9_wstat *stbuf =
				    va_arg(ap, const struct p9_wstat *);
				errcode =
				    p9pdu_writef(pdu, proto_version,
						 "wwdQdddqssss?sugu",
						 stbuf->size, stbuf->type,
						 stbuf->dev, &stbuf->qid,
						 stbuf->mode, stbuf->atime,
						 stbuf->mtime, stbuf->length,
						 stbuf->name, stbuf->uid,
						 stbuf->gid, stbuf->muid,
						 stbuf->extension, stbuf->n_uid,
						 stbuf->n_gid, stbuf->n_muid);
			} break;
		case 'U':{
				uint32_t count = va_arg(ap, uint32_t);
				struct uio *from =
						va_arg(ap, struct uio *);
				errcode = p9pdu_writef(pdu, proto_version, "d",
									count);
				if (!errcode && p9pdu_write_u(pdu, from, count))
					errcode = -EFAULT;
			}
			break;
		case 'T':{
				uint16_t nwname = va_arg(ap, int);
				const char **wnames = va_arg(ap, const char **);

				errcode = p9pdu_writef(pdu, proto_version, "w",
									nwname);
				if (!errcode) {
					int i;

					for (i = 0; i < nwname; i++) {
						errcode =
						    p9pdu_writef(pdu,
								proto_version,
								 "s",
								 wnames[i]);
						if (errcode)
							break;
					}
				}
			}
			break;
		case 'R':{
				uint16_t nwqid = va_arg(ap, int);
				struct p9_qid *wqids =
				    va_arg(ap, struct p9_qid *);

				errcode = p9pdu_writef(pdu, proto_version, "w",
									nwqid);
				if (!errcode) {
					int i;

					for (i = 0; i < nwqid; i++) {
						errcode =
						    p9pdu_writef(pdu,
								proto_version,
								 "Q",
								 &wqids[i]);
						if (errcode)
							break;
					}
				}
			}
			break;
		case 'I':{
				struct p9_iattr_dotl *p9attr = va_arg(ap,
							struct p9_iattr_dotl *);

				errcode = p9pdu_writef(pdu, proto_version,
							"ddugqqqqq",
							p9attr->valid,
							p9attr->mode,
							p9attr->uid,
							p9attr->gid,
							p9attr->size,
							p9attr->atime_sec,
							p9attr->atime_nsec,
							p9attr->mtime_sec,
							p9attr->mtime_nsec);
			}
			break;
		case '?':
			if ((proto_version != p9_proto_2000u) &&
				(proto_version != p9_proto_2000L))
				return 0;
			break;
		default:
			BUG();
			break;
		}

		if (errcode)
			break;
	}

	return errcode;
}

static int p9pdu_readf(struct p9_fcall *pdu, int proto_version, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = p9pdu_vreadf(pdu, proto_version, fmt, ap);
	va_end(ap);

	return ret;
}

static int p9pdu_writef(struct p9_fcall *pdu, int proto_version, const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = p9pdu_vwritef(pdu, proto_version, fmt, ap);
	va_end(ap);

	return ret;
}

static int p9pdu_prepare(struct p9_fcall *pdu, int16_t tag, int8_t type)
{
	pdu->id = type;
	return p9pdu_writef(pdu, 0, "dbw", 0, type, tag);
}

static int p9pdu_finalize(struct p9_client *clnt, struct p9_fcall *pdu)
{
	int size = pdu->size;
	int err;

	pdu->size = 0;
	err = p9pdu_writef(pdu, 0, "d", size);
	pdu->size = size;

	debugf(">>> size=%d type: %d tag: %d\n",
		 pdu->size, pdu->id, pdu->tag);

	return err;
}

static void p9pdu_reset(struct p9_fcall *pdu)
{
	pdu->offset = 0;
	pdu->size = 0;
}

/* client utils */

static int p9_get_protocol_version(char *s)
{
	int version = -EINVAL;

	if (!strcmp(s, "9p2000")) {
		version = p9_proto_legacy;
		debugf("Protocol version: Legacy\n");
	} else if (!strcmp(s, "9p2000.u")) {
		version = p9_proto_2000u;
		debugf("Protocol version: 9P2000.u\n");
	} else if (!strcmp(s, "9p2000.L")) {
		version = p9_proto_2000L;
		debugf("Protocol version: 9P2000.L\n");
	} else
		debugf("Unknown protocol version %s\n", s);

	return version;
}

static struct p9_fcall *p9_fcall_alloc(int alloc_msize)
{
	struct p9_fcall *fc;
	fc = (struct p9_fcall *) malloc(sizeof(struct p9_fcall) + alloc_msize);
	if (!fc)
		return nullptr;
	fc->capacity = alloc_msize;
	fc->sdata = (uint8_t *) ((char *) fc + sizeof(struct p9_fcall));
	return fc;
}

/**
 * p9_parse_header - parse header arguments out of a packet
 * @pdu: packet to parse
 * @size: size of packet
 * @type: type of request
 * @tag: tag of packet
 * @rewind: set if we need to rewind offset afterwards
 */
static int p9_parse_header(struct p9_fcall *pdu, int32_t *size, int8_t *type, int16_t *tag,
								int rewind)
{
	int8_t r_type;
	int16_t r_tag;
	int32_t r_size;
	int offset = pdu->offset;
	int err;

	pdu->offset = 0;
	if (pdu->size == 0)
		pdu->size = 7;

	err = p9pdu_readf(pdu, 0, "dbw", &r_size, &r_type, &r_tag);
	if (err)
		goto rewind_and_exit;

	pdu->size = r_size;
	pdu->id = r_type;
	pdu->tag = r_tag;

	debugf("<<< size=%d type: %d tag: %d\n",
		 pdu->size, pdu->id, pdu->tag);

	if (type)
		*type = r_type;
	if (tag)
		*tag = r_tag;
	if (size)
		*size = r_size;


rewind_and_exit:
	if (rewind)
		pdu->offset = offset;
	return err;
}

static std::unordered_map<const char *, int> errmap = {
	{"Operation not permitted", EPERM},
	{"wstat prohibited", EPERM},
	{"No such file or directory", ENOENT},
	{"directory entry not found", ENOENT},
	{"file not found", ENOENT},
	{"Interrupted system call", EINTR},
	{"Input/output error", EIO},
	{"No such device or address", ENXIO},
	{"Argument list too long", E2BIG},
	{"Bad file descriptor", EBADF},
	{"Resource temporarily unavailable", EAGAIN},
	{"Cannot allocate memory", ENOMEM},
	{"Permission denied", EACCES},
	{"Bad address", EFAULT},
	{"Block device required", ENOTBLK},
	{"Device or resource busy", EBUSY},
	{"File exists", EEXIST},
	{"Invalid cross-device link", EXDEV},
	{"No such device", ENODEV},
	{"Not a directory", ENOTDIR},
	{"Is a directory", EISDIR},
	{"Invalid argument", EINVAL},
	{"Too many open files in system", ENFILE},
	{"Too many open files", EMFILE},
	{"Text file busy", ETXTBSY},
	{"File too large", EFBIG},
	{"No space left on device", ENOSPC},
	{"Illegal seek", ESPIPE},
	{"Read-only file system", EROFS},
	{"Too many links", EMLINK},
	{"Broken pipe", EPIPE},
	{"Numerical argument out of domain", EDOM},
	{"Numerical result out of range", ERANGE},
	{"Resource deadlock avoided", EDEADLK},
	{"File name too long", ENAMETOOLONG},
	{"No locks available", ENOLCK},
	{"Function not implemented", ENOSYS},
	{"Directory not empty", ENOTEMPTY},
	{"Too many levels of symbolic links", ELOOP},
	{"No message of desired type", ENOMSG},
	{"Identifier removed", EIDRM},
	{"No data available", ENODATA},
	{"Machine is not on the network", ENONET},
	{"Package not installed", ENOPKG},
	{"Object is remote", EREMOTE},
	{"Link has been severed", ENOLINK},
	{"Communication error on send", ECOMM},
	{"Protocol error", EPROTO},
	{"Bad message", EBADMSG},
	{"File descriptor in bad state", EBADFD},
	{"Streams pipe error", ESTRPIPE},
	{"Too many users", EUSERS},
	{"Socket operation on non-socket", ENOTSOCK},
	{"Message too long", EMSGSIZE},
	{"Protocol not available", ENOPROTOOPT},
	{"Protocol not supported", EPROTONOSUPPORT},
	{"Socket type not supported", ESOCKTNOSUPPORT},
	{"Operation not supported", EOPNOTSUPP},
	{"Protocol family not supported", EPFNOSUPPORT},
	{"Network is down", ENETDOWN},
	{"Network is unreachable", ENETUNREACH},
	{"Network dropped connection on reset", ENETRESET},
	{"Software caused connection abort", ECONNABORTED},
	{"Connection reset by peer", ECONNRESET},
	{"No buffer space available", ENOBUFS},
	{"Transport endpoint is already connected", EISCONN},
	{"Transport endpoint is not connected", ENOTCONN},
	{"Cannot send after transport endpoint shutdown", ESHUTDOWN},
	{"Connection timed out", ETIMEDOUT},
	{"Connection refused", ECONNREFUSED},
	{"Host is down", EHOSTDOWN},
	{"No route to host", EHOSTUNREACH},
	{"Operation already in progress", EALREADY},
	{"Operation now in progress", EINPROGRESS},
	{"Is a named type file", EISNAM},
	{"Remote I/O error", EREMOTEIO},
	{"Disk quota exceeded", EDQUOT},
/* errors from fossil, vacfs, and u9fs */
	{"fid unknown or out of range", EBADF},
	{"permission denied", EACCES},
	{"file does not exist", ENOENT},
	{"authentication failed", ECONNREFUSED},
	{"bad offset in directory read", ESPIPE},
	{"bad use of fid", EBADF},
	{"wstat can't convert between files and directories", EPERM},
	{"directory is not empty", ENOTEMPTY},
	{"file exists", EEXIST},
	{"file already exists", EEXIST},
	{"file or directory already exists", EEXIST},
	{"fid already in use", EBADF},
	{"file in use", ETXTBSY},
	{"i/o error", EIO},
	{"file already open for I/O", ETXTBSY},
	{"illegal mode", EINVAL},
	{"illegal name", ENAMETOOLONG},
	{"not a directory", ENOTDIR},
	{"not a member of proposed group", EPERM},
	{"not owner", EACCES},
	{"only owner can change group in wstat", EACCES},
	{"read only file system", EROFS},
	{"no access to special file", EPERM},
	{"i/o count too large", EIO},
	{"unknown group", EINVAL},
	{"unknown user", EINVAL},
	{"bogus wstat buffer", EPROTO},
	{"exclusive use file already open", EAGAIN},
	{"corrupted directory entry", EIO},
	{"corrupted file entry", EIO},
	{"corrupted block label", EIO},
	{"corrupted meta data", EIO},
	{"illegal offset", EINVAL},
	{"illegal path element", ENOENT},
	{"root of file system is corrupted", EIO},
	{"corrupted super block", EIO},
	{"protocol botch", EPROTO},
	{"file system is full", ENOSPC},
	{"file is in use", EAGAIN},
	{"directory entry is not allocated", ENOENT},
	{"file is read only", EROFS},
	{"file has been removed", EIDRM},
	{"only support truncation to zero length", EPERM},
	{"cannot remove root", EPERM},
	{"file too big", EFBIG},
	{"venti i/o error", EIO},
	/* these are not errors */
	{"u9fs rhostsauth: no authentication required", 0},
	{"u9fs authnone: no authentication required", 0},
	{NULL, -1}
};

/**
 * errstr2errno - convert error string to error number
 * @errstr: error string
 * @len: length of error string
 *
 */
static int p9_errstr2errno(char *errstr, int len)
{
	int errno;

	errno = 0;
	auto it = errmap.find(errstr);
	if (it != errmap.end())
		errno = it->second;

	return -errno;
}

static int p9_statsize(struct p9_wstat *wst, int proto_version)
{
	int ret;

	/* NOTE: size shouldn't include its own length */
	/* size[2] type[2] dev[4] qid[13] */
	/* mode[4] atime[4] mtime[4] length[8]*/
	/* name[s] uid[s] gid[s] muid[s] */
	ret = 2+4+13+4+4+4+8+2+2+2+2;

	if (wst->name)
		ret += strlen(wst->name);
	if (wst->uid)
		ret += strlen(wst->uid);
	if (wst->gid)
		ret += strlen(wst->gid);
	if (wst->muid)
		ret += strlen(wst->muid);

	if ((proto_version == p9_proto_2000u) ||
		(proto_version == p9_proto_2000L)) {
		ret += 2+4+4+4;	/* extension[s] n_uid[4] n_gid[4] n_muid[4] */
		if (wst->extension)
			ret += strlen(wst->extension);
	}

	return ret;
}




inline static int
p9_virtio_create(struct p9_client *client, const char *devname, char *args)
{
	return virtio::vt9p::bind_client(client, devname, args);
}

inline static void p9_virtio_close(struct p9_client *client)
{
	virtio::vt9p::unbind_client(client);
}

inline static int
p9_virtio_request(struct p9_client *client, struct p9_req_t *req)
{
	virtio::vt9p *vt = (virtio::vt9p *) client->p9_trans();

	return vt->make_request(req);
}

static int p9_virtio_cancel(struct p9_client *client, struct p9_req_t *req)
{
	return 1;
}


#define VIRTQUEUE_NUM   128

struct p9_trans_module p9_virtio_trans = []{
	struct p9_trans_module mod;
	mod.name = (char *) "virtio";
	mod.create = p9_virtio_create;
	mod.close = p9_virtio_close;
	mod.request = p9_virtio_request;
	mod.cancel = p9_virtio_cancel;
	mod.maxsize = PAGE_SIZE * (VIRTQUEUE_NUM - 3);
	mod.def = 1;
	return mod;
}();






p9_client::p9_client(const char *dev_name, char *options)
{

	memcpy(_name, dev_name, strlen(dev_name) + 1);

	// Reserver tag 0
	if (_tagpool.p9_idpool_get() < 0)
	{
		throw std::runtime_error("invalid tagpool");
	}
	_max_tag = 0;

	// Parse options
	if (p9_parse_options(options) < 0)
	{
		throw std::runtime_error("invalid options");
	}

	// Set default tranport
	_trans_mod = &p9_virtio_trans;

	if(_trans_mod->create(this, dev_name, options))
	{
		throw std::runtime_error("failed to create tranport channel");
	}

	_msize = _trans_mod->maxsize;

	// Get version
	if (p9_client_version())
	{
		throw std::runtime_error("version error");
	}

}

p9_client::~p9_client()
{
	// Close tranport channel
	if (_trans_mod)
	{
		_trans_mod->close(this);
	}

	int row, col;

	// Free all fid in _fidlist
	for (auto fid : _fidlist)
	{
		debugf("Found fid %d not clunked\n", fid->fid);
		p9_fid_destroy(fid);
	}


	// Check to insure all requests are idle
	for (row = 0; row < (_max_tag/P9_ROW_MAXTAG); row++) {
		for (col = 0; col < P9_ROW_MAXTAG; col++) {
			if (_reqs[row][col].status != REQ_STATUS_IDLE) {
				debugf("Attempting to cleanup non-free tag %d,%d\n",
					 row, col);
				/* TODO: delay execution of cleanup */
				return;
			}
		}
	}
	 // Free reserved tag 0
	_tagpool.p9_idpool_put(0);

	// Free requests associated with tags
	for (row = 0; row < (_max_tag/P9_ROW_MAXTAG); row++) {
		for (col = 0; col < P9_ROW_MAXTAG; col++) {
			// free(c->reqs[row][col].wq);
			free(_reqs[row][col].tc);
			free(_reqs[row][col].rc);
		}
		free(_reqs[row]);
	}
	_max_tag = 0;
}

/* PUBLIC FUNCTIONS */

int p9_client::p9_proto()
{
	return _proto_version;
}

int p9_client::p9_msize()
{
	return _msize;
}

void *p9_client::p9_trans()
{
	return _trans;
}

int p9_client::p9_is_proto_dotu()
{
	return _proto_version == p9_proto_2000u;
}

int p9_client::p9_is_proto_dotl()
{
	return _proto_version == p9_proto_2000L;
}

int p9_client::p9_client_show_options(seq *s)
{
	if (_msize != 8192)
	{
		s->seq_printf(",msize=%u", _msize);
	}
	s->seq_printf(",trans=%s", _trans_mod->name);

	switch (_proto_version)
	{
		case p9_proto_legacy:
			s->seq_puts(",noextend");
			break;
		case p9_proto_2000u:
			s->seq_puts(",version=9p2000.u");
			break;
		case p9_proto_2000L:
			// Default
			break;
	}

	if (_trans_mod->show_options)
	{
		return _trans_mod->show_options(s, this);
	}
	return 0;
}

void p9_client::p9_client_connect(void *trans)
{
	_trans = trans;
	_status = Connected;
}

struct p9_fid *p9_client::p9_client_attach(struct p9_fid *afid, 
	const char *uname, kuid_t n_uname, const char *aname)
{
	int err = 0;
	struct p9_req_t *req;
	struct p9_fid *fid;
	struct p9_qid qid;


	debugf(">>> TATTACH afid %d uname %s aname %s\n",
		 afid ? afid->fid : -1, uname, aname);
	;
	if (!(fid = p9_fid_create())) {
		return nullptr;
	}
	fid->uid = n_uname;

	req = p9_client_rpc(P9_TATTACH, "ddss?u", fid->fid,
			afid ? afid->fid : P9_NOFID, uname, aname, n_uname);
	if (!req) {
		p9_fid_destroy(fid);
		return nullptr;
	}

	err = p9pdu_readf(req->rc, _proto_version, "Q", &qid);
	if (err) {
		p9_free_req(req);
		p9_fid_destroy(fid);
		return nullptr;
	}

	debugf("<<< RATTACH qid %x.%llx.%x\n",
		 qid.type, (unsigned long long)qid.path, qid.version);

	memmove(&fid->qid, &qid, sizeof(struct p9_qid));

	p9_free_req(req);

	return fid;
}

void p9_client::p9_client_begin_disconnect()
{
	debugf("clnt %p\n", this);
	_status = BeginDisconnect;
}

void p9_client::p9_client_disconnect()
{
	debugf("clnt %p\n", this);
	_status = Disconnected;
}


/* STATIC FUNCTIONS */

void p9_client::p9_client_cb(struct p9_req_t *req, int status)
{
	debugf(" tag %d\n", req->tc->tag);

	/*
	 * This barrier is needed to make sure any change made to req before
	 * the status change is visible to another thread
	 */
	smp_wmb();
	req->status = status;

	debugf("wakeup: %d\n", req->tc->tag);
}

int p9_client::p9_client_clunk(struct p9_fid *fid)
{
	int err;
	p9_client *clnt;
	struct p9_req_t *req;
	int retries = 0;

	if (!fid) {
		debugf( "%s: Trying to clunk with NULL fid\n", __func__);
		return 0;
	}

again:
	debugf(">>> TCLUNK fid %d (try %d)\n", fid->fid, retries);
	err = 0;
	clnt = fid->clnt;

	req = clnt->p9_client_rpc(P9_TCLUNK, "d", fid->fid);
	if (!req) {
		err = -1;
		goto error;
	}

	debugf("<<< RCLUNK fid %d\n", fid->fid);

	clnt->p9_free_req(req);
error:
	/*
	 * Fid is not valid even after a failed clunk
	 * If interrupted, retry once then give up and
	 * leak fid until umount.
	 */
	if (retries++ == 0) {
		goto again;
	} else
		clnt->p9_fid_destroy(fid);
	return err;
}

int p9_client::p9_client_statfs(struct p9_fid *fid, struct p9_rstatfs *sb)
{
	int err;
	struct p9_req_t *req;
	struct p9_client *clnt;

	err = 0;
	clnt = fid->clnt;

	debugf( ">>> TSTATFS fid %d\n", fid->fid);

	req = clnt->p9_client_rpc(P9_TSTATFS, "d", fid->fid);
	if (!req) {
		return -1;
	}

	err = p9pdu_readf(req->rc, clnt->_proto_version, "ddqqqqqqd", &sb->type,
		&sb->bsize, &sb->blocks, &sb->bfree, &sb->bavail,
		&sb->files, &sb->ffree, &sb->fsid, &sb->namelen);
	if (err) {
		clnt->p9_free_req(req);
		return err;
	}

	debugf("<<< RSTATFS fid %d type 0x%lx bsize %ld "
		"blocks %llu bfree %llu bavail %llu files %llu ffree %llu "
		"fsid %llu namelen %ld\n",
		fid->fid, (long unsigned int)sb->type, (long int)sb->bsize,
		sb->blocks, sb->bfree, sb->bavail, sb->files,  sb->ffree,
		sb->fsid, (long int)sb->namelen);

	clnt->p9_free_req(req);

	return err;
}

struct p9_fid *p9_client::p9_client_walk(struct p9_fid *oldfid, uint16_t nwname, 
	const unsigned char * const *wnames, int clone)
{
	int err;
	struct p9_client *clnt;
	struct p9_fid *fid;
	struct p9_qid *wqids;
	struct p9_req_t *req;
	uint16_t nwqids, count;

	err = 0;
	wqids = nullptr;
	clnt = oldfid->clnt;
	if (clone) {
		fid = clnt->p9_fid_create();
		if (!fid) {
			err = -1;
			fid = nullptr;
			goto error;
		}

		fid->uid = oldfid->uid;
	} else
		fid = oldfid;


	debugf(">>> TWALK fids %d,%d nwname %ud wname[0] %s\n",
		 oldfid->fid, fid->fid, nwname, wnames ? wnames[0] : NULL);

	req = clnt->p9_client_rpc(P9_TWALK, "ddT", oldfid->fid, fid->fid,
								nwname, wnames);
	if (!req) {
		err = -1;
		goto error;
	}

	err = p9pdu_readf(req->rc, clnt->_proto_version, "R", &nwqids, &wqids);
	if (err) {
		clnt->p9_free_req(req);
		goto clunk_fid;
	}
	clnt->p9_free_req(req);

	debugf("<<< RWALK nwqid %d:\n", nwqids);

	if (nwqids != nwname) {
		err = -ENOENT;
		goto clunk_fid;
	}

	for (count = 0; count < nwqids; count++)
		debugf("<<<     [%d] %x.%llx.%x\n",
			count, wqids[count].type,
			(unsigned long long)wqids[count].path,
			wqids[count].version);

	if (nwname)
		memmove(&fid->qid, &wqids[nwqids - 1], sizeof(struct p9_qid));
	else
		fid->qid = oldfid->qid;

	free(wqids);
	return fid;

clunk_fid:
	free(wqids);
	p9_client_clunk(fid);
	fid = nullptr;

error:
	if (fid && (fid != oldfid))
		clnt->p9_fid_destroy(fid);

	return nullptr;
}

int p9_client::p9_client_open(struct p9_fid *fid, int mode)
{
	int err;
	struct p9_client *clnt;
	struct p9_req_t *req;
	struct p9_qid qid;
	int iounit;

	clnt = fid->clnt;
	debugf(">>> %s fid %d mode %d\n",
		clnt->p9_is_proto_dotl() ? "TLOPEN" : "TOPEN", fid->fid, mode);
	err = 0;

	if (fid->mode != -1)
		return -EINVAL;

	if (clnt->p9_is_proto_dotl())
		req = clnt->p9_client_rpc(P9_TLOPEN, "dd", fid->fid, mode);
	else
		req = clnt->p9_client_rpc(P9_TOPEN, "db", fid->fid, mode);
	if (!req) {
		return -1;
	}

	err = p9pdu_readf(req->rc, clnt->_proto_version, "Qd", &qid, &iounit);
	if (err) {
		goto free_and_error;
	}

	debugf("<<< %s qid %x.%llx.%x iounit %x\n",
		clnt->p9_is_proto_dotl() ? "RLOPEN" : "ROPEN",  qid.type,
		(unsigned long long)qid.path, qid.version, iounit);

	fid->mode = mode;
	fid->iounit = iounit;

free_and_error:
	clnt->p9_free_req(req);
	return err;
}

int p9_client::p9_client_read(struct p9_fid *fid, struct uio *to, size_t len, int *err)
{
	struct p9_client *clnt = fid->clnt;
	struct p9_req_t *req;
	int total = 0;
	u64 offset = to->uio_offset;
	*err = 0;

	debugf(">>> TREAD fid %d offset %llu %d\n",
		   fid->fid, (unsigned long long) offset, (int)to->uio_resid);

	while (len) {
		int count = len;
		int rsize;
		char *dataptr;
			
		rsize = fid->iounit;
		if (!rsize || (unsigned) rsize > clnt->_msize - P9_IOHDRSZ)
			rsize = clnt->_msize - P9_IOHDRSZ;

		if (count < rsize)
			rsize = count;

		req = clnt->p9_client_rpc(P9_TREAD, "dqd", fid->fid, offset,
					    rsize);
		if (!req) {
			*err = -1;
			break;
		}

		*err = p9pdu_readf(req->rc, clnt->_proto_version,
				   "D", &count, &dataptr);
		if (*err) {
			clnt->p9_free_req(req);
			break;
		}
		if (rsize < count) {
			debugf("bogus RREAD count (%d > %d)\n", count, rsize);
			count = rsize;
		}

		debugf("<<< RREAD count %d\n", count);
		if (!count) {
			clnt->p9_free_req(req);
			break;
		}

		int n = uiomove(dataptr, count, to);
		len -= n;
		total += n;
		offset += n;
		if (n != count) {
			*err = -EFAULT;
			clnt->p9_free_req(req);
			break;
		}
		clnt->p9_free_req(req);
	}
	return total;
}

int p9_client::p9_client_write(struct p9_fid *fid, struct uio *from, size_t len, int *err)
{
	struct p9_client *clnt = fid->clnt;
	struct p9_req_t *req;
	int total = 0;
	u64 offset = from->uio_offset;
	*err = 0;

	debugf(">>> TWRITE fid %d offset %llu count %zd\n",
				fid->fid, (unsigned long long) offset,
				from->uio_resid);

	while (len) {
		int count = len;
		int rsize = fid->iounit;
		if (!rsize || (unsigned) rsize > clnt->_msize - P9_IOHDRSZ)
			rsize = clnt->_msize - P9_IOHDRSZ;

		if (count < rsize)
			rsize = count;

		req = clnt->p9_client_rpc(P9_TWRITE, "dqU", fid->fid,
						    offset, rsize, from);
		if (!req) {
			*err = -1;
			break;
		}

		*err = p9pdu_readf(req->rc, clnt->_proto_version, "d", &count);
		if (*err) {
			clnt->p9_free_req(req);
			break;
		}
		if (rsize < count) {
			debugf("bogus RWRITE count (%d > %d)\n", count, rsize);
			count = rsize;
		}

		debugf("<<< RWRITE count %d\n", count);

		clnt->p9_free_req(req);
		len -= count;
		total += count;
		offset += count;
	}
	return total;
}

int p9_client::p9_client_fsync(struct p9_fid *fid, int datasync)
{
	int err;
	struct p9_client *clnt;
	struct p9_req_t *req;

	debugf(">>> TFSYNC fid %d datasync:%d\n",
			fid->fid, datasync);
	err = 0;
	clnt = fid->clnt;

	req = clnt->p9_client_rpc(P9_TFSYNC, "dd", fid->fid, datasync);
	if (!req) {
		err = -1;
		return err;
	}

	debugf("<<< RFSYNC fid %d\n", fid->fid);

	clnt->p9_free_req(req);

	return err;
}

int p9_client::p9_client_readdir_dotl(struct p9_fid *fid, char *data, u32 count, u64 offset)
{
	int err, rsize;
	struct p9_client *clnt;
	struct p9_req_t *req;
	char *dataptr;

	debugf(">>> TREADDIR fid %d offset %llu count %d\n",
				fid->fid, (unsigned long long) offset, count);

	err = 0;
	clnt = fid->clnt;

	rsize = fid->iounit;
	if (!rsize || (unsigned) rsize > clnt->_msize - P9_READDIRHDRSZ)
		rsize = clnt->_msize - P9_READDIRHDRSZ;

	if (count < (unsigned) rsize)
		rsize = count;

	req = clnt->p9_client_rpc(P9_TREADDIR, "dqd", fid->fid,
				    offset, rsize);
	if (!req) {
		return -1;
	}

	err = p9pdu_readf(req->rc, clnt->_proto_version, "D", &count, &dataptr);
	if (err) {
		goto free_and_error;
	}
	if ((unsigned) rsize < count) {
		debugf("bogus RREADDIR count (%d > %d)\n", count, rsize);
		count = rsize;
	}

	debugf("<<< RREADDIR count %d\n", count);

	memmove(data, dataptr, count);

	clnt->p9_free_req(req);
	return count;

free_and_error:
	clnt->p9_free_req(req);
	return err;
}

int p9_client::p9_client_readlink_dotl(struct p9_fid *fid, char **target)
{
	int err;
	struct p9_client *clnt;
	struct p9_req_t *req;

	err = 0;
	clnt = fid->clnt;
	debugf( ">>> TREADLINK fid %d\n", fid->fid);

	req = clnt->p9_client_rpc(P9_TREADLINK, "d", fid->fid);
	if (!req)
		return -1;

	err = p9pdu_readf(req->rc, clnt->_proto_version, "s", target);
	if (err) {
		goto error;
	}
	debugf("<<< RREADLINK target %s\n", *target);

error:
	clnt->p9_free_req(req);
	return err;
}
	
int p9_client::p9_client_fcreate(struct p9_fid *fid, const char *name, u32 perm, int mode, 
	char *extension)
{
	int err;
	struct p9_client *clnt;
	struct p9_req_t *req;
	struct p9_qid qid;
	int iounit;

	debugf(">>> TCREATE fid %d name %s perm %d mode %d\n",
						fid->fid, name, perm, mode);
	err = 0;
	clnt = fid->clnt;

	if (fid->mode != -1)
		return -EINVAL;

	req = clnt->p9_client_rpc(P9_TCREATE, "dsdb?s", fid->fid, name, perm,
				mode, extension);
	if (!req) {
		return -1;
	}

	err = p9pdu_readf(req->rc, clnt->_proto_version, "Qd", &qid, &iounit);
	if (err) {
		goto free_and_error;
	}

	debugf("<<< RCREATE qid %x.%llx.%x iounit %x\n",
				qid.type,
				(unsigned long long)qid.path,
				qid.version, iounit);

	fid->mode = mode;
	fid->iounit = iounit;

free_and_error:
	clnt->p9_free_req(req);
	return err;
}

int p9_client::p9_client_fcreate_dotl(struct p9_fid *ofid, const char *name, u32 flags, u32 mode, 
	kgid_t gid, struct p9_qid *qid)
{
	int err = 0;
	struct p9_client *clnt;
	struct p9_req_t *req;
	int iounit;

	debugf(">>> TLCREATE fid %d name %s flags %d mode %d gid %d\n",
			ofid->fid, name, flags, mode,
		 	gid.val);
	clnt = ofid->clnt;

	if (ofid->mode != -1)
		return -EINVAL;

	req = clnt->p9_client_rpc(P9_TLCREATE, "dsddg", ofid->fid, name, flags,
			mode, gid);
	if (!req) {
		return -1;
	}

	err = p9pdu_readf(req->rc, clnt->_proto_version, "Qd", qid, &iounit);
	if (err) {
		goto free_and_error;
	}

	debugf("<<< RLCREATE qid %x.%llx.%x iounit %x\n",
			qid->type,
			(unsigned long long)qid->path,
			qid->version, iounit);

	ofid->mode = mode;
	ofid->iounit = iounit;

free_and_error:
	clnt->p9_free_req(req);
	return err;	
}

int p9_client::p9_client_mknod_dotl(struct p9_fid *fid, const char *name, int mode, 
	dev_t rdev, kgid_t gid, struct p9_qid *qid)
{
	int err;
	struct p9_client *clnt;
	struct p9_req_t *req;

	err = 0;
	clnt = fid->clnt;
	debugf(">>> TMKNOD fid %d name %s mode %d major %d "
		"minor %d\n", fid->fid, name, mode, MAJOR(rdev), MINOR(rdev));
	req = clnt->p9_client_rpc(P9_TMKNOD, "dsdddg", fid->fid, name, mode,
		MAJOR(rdev), MINOR(rdev), gid);
	if (!req)
		return -1;

	err = p9pdu_readf(req->rc, clnt->_proto_version, "Q", qid);
	if (err) {
		goto error;
	}
	debugf("<<< RMKNOD qid %x.%llx.%x\n", qid->type,
				(unsigned long long)qid->path, qid->version);

error:
	clnt->p9_free_req(req);
	return err;
}

int p9_client::p9_client_link_dotl(struct p9_fid *dfid, struct p9_fid *oldfid, const char *newname)
{
	struct p9_client *clnt;
	struct p9_req_t *req;

	debugf(">>> TLINK dfid %d oldfid %d newname %s\n",
			dfid->fid, oldfid->fid, newname);
	clnt = dfid->clnt;
	req = clnt->p9_client_rpc(P9_TLINK, "dds", dfid->fid, oldfid->fid,
			newname);
	if (!req)
		return -1;

	debugf("<<< RLINK\n");
	clnt->p9_free_req(req);
	return 0;
}

int p9_client::p9_client_symlink_dotl(struct p9_fid *dfid, const char *name, const char *symtgt, 
	kgid_t gid, struct p9_qid *qid)
{
	int err = 0;
	struct p9_client *clnt;
	struct p9_req_t *req;

	debugf(">>> TSYMLINK dfid %d name %s  symtgt %s\n",
			dfid->fid, name, symtgt);
	clnt = dfid->clnt;

	req = clnt->p9_client_rpc(P9_TSYMLINK, "dssg", dfid->fid, name, symtgt,
			gid);
	if (!req) {
		return -1;
	}

	err = p9pdu_readf(req->rc, clnt->_proto_version, "Q", qid);
	if (err) {
		goto free_and_error;
	}

	debugf("<<< RSYMLINK qid %x.%llx.%x\n",
			qid->type, (unsigned long long)qid->path, qid->version);

free_and_error:
	clnt->p9_free_req(req);
	return err;
}

int p9_client::p9_client_mkdir_dotl(struct p9_fid *fid, const char *name, int mode, 
	kgid_t gid, struct p9_qid *qid)
{
	int err;
	struct p9_client *clnt;
	struct p9_req_t *req;

	err = 0;
	clnt = fid->clnt;
	debugf(">>> TMKDIR fid %d name %s mode %d gid %d\n",
		 fid->fid, name, mode, gid.val);
	req = clnt->p9_client_rpc(P9_TMKDIR, "dsdg", fid->fid, name, mode,
		gid);
	if (!req)
		return -1;

	err = p9pdu_readf(req->rc, clnt->_proto_version, "Q", qid);
	if (err) {
		goto error;
	}
	debugf("<<< RMKDIR qid %x.%llx.%x\n", qid->type,
				(unsigned long long)qid->path, qid->version);

error:
	clnt->p9_free_req(req);
	return err;
}

/**
 * [Debug] clunk or destroy?
 */
int p9_client::p9_client_remove(struct p9_fid *fid)
{
	int err;
	struct p9_client *clnt;
	struct p9_req_t *req;

	debugf(">>> TREMOVE fid %d\n", fid->fid);
	err = 0;
	clnt = fid->clnt;

	req = clnt->p9_client_rpc(P9_TREMOVE, "d", fid->fid);
	if (!req) {
		err = -1;
		goto error;
	}

	debugf("<<< RREMOVE fid %d\n", fid->fid);

	clnt->p9_free_req(req);
error:
	if (err == -ERESTART)
		p9_client_clunk(fid);
	else
		clnt->p9_fid_destroy(fid);
	return err;
}

int p9_client::p9_client_unlinkat_dotl(struct p9_fid *dfid, const char *name, int flags)
{
	int err = 0;
	struct p9_req_t *req;
	struct p9_client *clnt;

	debugf(">>> TUNLINKAT fid %d %s %d\n",
		   dfid->fid, name, flags);

	clnt = dfid->clnt;
	req = clnt->p9_client_rpc(P9_TUNLINKAT, "dsd", dfid->fid, name, flags);
	if (req) {
		return -1;
	}
	debugf("<<< RUNLINKAT fid %d %s\n", dfid->fid, name);

	clnt->p9_free_req(req);
	return err;	
}

int p9_client::p9_client_rename(struct p9_fid *fid, struct p9_fid *newdirfid, 
	const char *name)
{
	int err;
	struct p9_req_t *req;
	struct p9_client *clnt;

	err = 0;
	clnt = fid->clnt;

	debugf(">>> TRENAME fid %d newdirfid %d name %s\n",
			fid->fid, newdirfid->fid, name);

	req = clnt->p9_client_rpc(P9_TRENAME, "dds", fid->fid,
			newdirfid->fid, name);
	if (!req) {
		return -1;
	}

	debugf("<<< RRENAME fid %d\n", fid->fid);

	clnt->p9_free_req(req);
	return err;
}

int p9_client:: p9_client_renameat_dotl(struct p9_fid *olddirfid, const char *old_name, 
	struct p9_fid *newdirfid, const char *new_name)
{
	int err;
	struct p9_req_t *req;
	struct p9_client *clnt;

	err = 0;
	clnt = olddirfid->clnt;

	debugf(">>> TRENAMEAT olddirfid %d old name %s"
		   " newdirfid %d new name %s\n", olddirfid->fid, old_name,
		   newdirfid->fid, new_name);

	req = clnt->p9_client_rpc(P9_TRENAMEAT, "dsds", olddirfid->fid,
			    old_name, newdirfid->fid, new_name);
	if (!req) {
		return -1;
	}

	debugf("<<< RRENAMEAT newdirfid %d new name %s\n",
		   newdirfid->fid, new_name);

	clnt->p9_free_req(req);
	return err;
}


struct p9_wstat *p9_client::p9_client_getattr(struct p9_fid *fid)
{
	int err;
	struct p9_client *clnt;
	struct p9_wstat *ret = (struct p9_wstat *) malloc(sizeof(struct p9_wstat));
	struct p9_req_t *req;
	u16 ignored;

	debugf(">>> TSTAT fid %d\n", fid->fid);

	if (!ret)
		return nullptr;

	err = 0;
	clnt = fid->clnt;

	req = clnt->p9_client_rpc(P9_TSTAT, "d", fid->fid);
	if (!req) {
		goto error;
	}

	err = p9pdu_readf(req->rc, clnt->_proto_version, "wS", &ignored, ret);
	if (err) {
		clnt->p9_free_req(req);
		goto error;
	}

	debugf("<<< RSTAT sz=%x type=%x dev=%x qid=%x.%llx.%x\n"
		"<<<    mode=%8.8x atime=%8.8x mtime=%8.8x length=%llx\n"
		"<<<    name=%s uid=%s gid=%s muid=%s extension=(%s)\n"
		"<<<    uid=%d gid=%d n_muid=%d\n",
		ret->size, ret->type, ret->dev, ret->qid.type,
		(unsigned long long)ret->qid.path, ret->qid.version, ret->mode,
		ret->atime, ret->mtime, (unsigned long long)ret->length,
		ret->name, ret->uid, ret->gid, ret->muid, ret->extension,
		(ret->n_uid).val,
		(ret->n_gid).val,
		(ret->n_muid).val);

	clnt->p9_free_req(req);
	return ret;

error:
	free(ret);
	return nullptr;
}

int p9_client::p9_client_setattr(struct p9_fid *fid, struct p9_wstat *wst)
{
	int err;
	struct p9_req_t *req;
	struct p9_client *clnt;

	err = 0;
	clnt = fid->clnt;
	wst->size = p9_statsize(wst, clnt->_proto_version);
	debugf(">>> TWSTAT fid %d\n", fid->fid);
	debugf("     sz=%x type=%x dev=%x qid=%x.%llx.%x\n"
		"     mode=%8.8x atime=%8.8x mtime=%8.8x length=%llx\n"
		"     name=%s uid=%s gid=%s muid=%s extension=(%s)\n"
		"     uid=%d gid=%d n_muid=%d\n",
		wst->size, wst->type, wst->dev, wst->qid.type,
		(unsigned long long)wst->qid.path, wst->qid.version, wst->mode,
		wst->atime, wst->mtime, (unsigned long long)wst->length,
		wst->name, wst->uid, wst->gid, wst->muid, wst->extension,
		(wst->n_uid).val,
		(wst->n_gid).val,
		(wst->n_muid).val);

	req = clnt->p9_client_rpc(P9_TWSTAT, "dwS", fid->fid, wst->size+2, wst);
	if (!req) {
		return -1;
	}

	debugf("<<< RWSTAT fid %d\n", fid->fid);

	clnt->p9_free_req(req);
	return err;
}

struct p9_stat_dotl *p9_client::p9_client_getattr_dotl(struct p9_fid *fid, u64 request_mask)
{
	int err;
	struct p9_client *clnt;
	struct p9_stat_dotl *ret = (struct p9_stat_dotl *) malloc(sizeof(struct p9_stat_dotl));
	struct p9_req_t *req;

	debugf(">>> TGETATTR fid %d, request_mask %lld\n",
							fid->fid, request_mask);

	if (!ret)
		return nullptr;

	err = 0;
	clnt = fid->clnt;

	req = clnt->p9_client_rpc(P9_TGETATTR, "dq", fid->fid, request_mask);
	if (!req) {
		goto error;
	}

	err = p9pdu_readf(req->rc, clnt->_proto_version, "A", ret);
	if (err) {
		clnt->p9_free_req(req);
		goto error;
	}

	debugf("<<< RGETATTR st_result_mask=%lld\n"
		"<<< qid=%x.%llx.%x\n"
		"<<< st_mode=%8.8x st_nlink=%llu\n"
		"<<< st_uid=%d st_gid=%d\n"
		"<<< st_rdev=%llx st_size=%llx st_blksize=%llu st_blocks=%llu\n"
		"<<< st_atime_sec=%lld st_atime_nsec=%lld\n"
		"<<< st_mtime_sec=%lld st_mtime_nsec=%lld\n"
		"<<< st_ctime_sec=%lld st_ctime_nsec=%lld\n"
		"<<< st_btime_sec=%lld st_btime_nsec=%lld\n"
		"<<< st_gen=%lld st_data_version=%lld",
		ret->st_result_mask, ret->qid.type, ret->qid.path,
		ret->qid.version, ret->st_mode, ret->st_nlink,
		(ret->st_uid).val,
		(ret->st_gid).val,
		ret->st_rdev, ret->st_size, ret->st_blksize,
		ret->st_blocks, ret->st_atime_sec, ret->st_atime_nsec,
		ret->st_mtime_sec, ret->st_mtime_nsec, ret->st_ctime_sec,
		ret->st_ctime_nsec, ret->st_btime_sec, ret->st_btime_nsec,
		ret->st_gen, ret->st_data_version);

	clnt->p9_free_req(req);
	return ret;

error:
	free(ret);
	return nullptr;
}

int p9_client::p9_client_setattr_dotl(struct p9_fid *fid, struct p9_iattr_dotl *p9attr)
{
	int err;
	struct p9_req_t *req;
	struct p9_client *clnt;

	err = 0;
	clnt = fid->clnt;
	debugf(">>> TSETATTR fid %d\n", fid->fid);
	debugf("    valid=%x mode=%x uid=%d gid=%d size=%lld\n"
		"    atime_sec=%lld atime_nsec=%lld\n"
		"    mtime_sec=%lld mtime_nsec=%lld\n",
		p9attr->valid, p9attr->mode,
		(p9attr->uid).val,
		(p9attr->gid).val,
		p9attr->size, p9attr->atime_sec, p9attr->atime_nsec,
		p9attr->mtime_sec, p9attr->mtime_nsec);

	req = clnt->p9_client_rpc(P9_TSETATTR, "dI", fid->fid, p9attr);

	if (!req) {
		return -1;
	}
	debugf("<<< RSETATTR fid %d\n", fid->fid);
	clnt->p9_free_req(req);
	return err;
}

int p9_client::p9_client_lock_dotl(struct p9_fid *fid, struct p9_flock *flock, u8 *status)
{
	int err;
	struct p9_client *clnt;
	struct p9_req_t *req;

	err = 0;
	clnt = fid->clnt;
	debugf(">>> TLOCK fid %d type %i flags %d "
			"start %lld length %lld proc_id %d client_id %s\n",
			fid->fid, flock->type, flock->flags, flock->start,
			flock->length, flock->proc_id, flock->client_id);

	req = clnt->p9_client_rpc(P9_TLOCK, "dbdqqds", fid->fid, flock->type,
				flock->flags, flock->start, flock->length,
					flock->proc_id, flock->client_id);

	if (!req)
		return -1;

	err = p9pdu_readf(req->rc, clnt->_proto_version, "b", status);
	if (err) {
		goto error;
	}
	debugf("<<< RLOCK status %i\n", *status);
error:
	clnt->p9_free_req(req);
	return err;
}

int p9_client::p9_client_getlock_dotl(struct p9_fid *fid, struct p9_getlock *glock)
{
	int err;
	struct p9_client *clnt;
	struct p9_req_t *req;

	err = 0;
	clnt = fid->clnt;
	debugf(">>> TGETLOCK fid %d, type %i start %lld "
		"length %lld proc_id %d client_id %s\n", fid->fid, glock->type,
		glock->start, glock->length, glock->proc_id, glock->client_id);

	req = clnt->p9_client_rpc(P9_TGETLOCK, "dbqqds", fid->fid,  glock->type,
		glock->start, glock->length, glock->proc_id, glock->client_id);

	if (!req)
		return -1;

	err = p9pdu_readf(req->rc, clnt->_proto_version, "bqqds", &glock->type,
			&glock->start, &glock->length, &glock->proc_id,
			&glock->client_id);
	if (err) {
		goto error;
	}
	debugf("<<< RGETLOCK type %i start %lld length %lld "
		"proc_id %d client_id %s\n", glock->type, glock->start,
		glock->length, glock->proc_id, glock->client_id);
error:
	clnt->p9_free_req(req);
	return err;
}

struct p9_fid *p9_client::p9_client_xattrwalk_dotl(struct p9_fid *file_fid, 
	const char *attr_name, u64 *attr_size)
{
	int err;
	struct p9_req_t *req;
	struct p9_client *clnt;
	struct p9_fid *attr_fid;

	err = 0;
	clnt = file_fid->clnt;
	attr_fid = clnt->p9_fid_create();
	if (!attr_fid) {
		goto error;
	}
	debugf(">>> TXATTRWALK file_fid %d, attr_fid %d name %s\n",
		file_fid->fid, attr_fid->fid, attr_name);

	req = clnt->p9_client_rpc(P9_TXATTRWALK, "dds",
			file_fid->fid, attr_fid->fid, attr_name);
	if (!req) {
		goto error;
	}
	err = p9pdu_readf(req->rc, clnt->_proto_version, "q", attr_size);
	if (err) {
		clnt->p9_free_req(req);
		goto clunk_fid;
	}
	clnt->p9_free_req(req);
	debugf("<<<  RXATTRWALK fid %d size %llu\n",
		attr_fid->fid, *attr_size);
	return attr_fid;
clunk_fid:
	p9_client_clunk(attr_fid);
	attr_fid = NULL;
error:
	if (attr_fid && (attr_fid != file_fid))
		clnt->p9_fid_destroy(attr_fid);

	return nullptr;
}

int p9_client::p9_client_xattrcreate_dotl(struct p9_fid *fid, 
	const char *name, u64 attr_size, int flags)
{
	int err;
	struct p9_req_t *req;
	struct p9_client *clnt;

	debugf(">>> TXATTRCREATE fid %d name  %s size %lld flag %d\n",
		fid->fid, name, (long long)attr_size, flags);
	err = 0;
	clnt = fid->clnt;
	req = clnt->p9_client_rpc(P9_TXATTRCREATE, "dsqd",
			fid->fid, name, attr_size, flags);
	if (!req) {
		return -1;
	}
	debugf("<<< RXATTRCREATE fid %d\n", fid->fid);
	clnt->p9_free_req(req);
	return err;
}


/* PRIVATE FUNCTIONS */

enum {
	Opt_msize,
	Opt_trans,
	Opt_legacy,
	Opt_version,
	Opt_err,
};

static const match_table_t tokens = {
	{Opt_msize, "msize=%u"},
	{Opt_legacy, "noextend"},
	{Opt_trans, "trans=%s"},
	{Opt_version, "version=%s"},
	{Opt_err, NULL},
};

int p9_client::p9_parse_options(const char *opts)
{
	char *options, *tmp_options;
	char *p;
	substring_t args[MAX_OPT_ARGS];
	int option;
	char *s;
	int ret = 0;

	_proto_version = p9_proto_2000L;
	_msize = 8192;

	if (!opts)
		return 0;

	tmp_options = strdup(opts);
	if (!tmp_options) {
		debugf("failed to allocate copy of option string\n");
		return -ENOMEM;
	}
	options = tmp_options;

	while ((p = strsep(&options, ",")) != NULL) {
		int token, r;
		if (!*p)
			continue;
		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_msize:
			r = match_int(&args[0], &option);
			if (r < 0) {
				debugf("integer field, but no integer?\n");
				ret = r;
				continue;
			}
			_msize = option;
			break;
		case Opt_trans:
			s = match_strdup(&args[0]);
			if (!s) {
				ret = -ENOMEM;
				debugf("problem allocating copy of trans arg\n");
				goto free_and_return;
			 }
			_trans_mod = &p9_virtio_trans;
			if (_trans_mod == NULL) {
				debugf("Could not find request transport: %s\n", s);
				ret = -EINVAL;
				free(s);
				goto free_and_return;
			}
			free(s);
			break;
		case Opt_legacy:
			_proto_version = p9_proto_legacy;
			break;
		case Opt_version:
			s = match_strdup(&args[0]);
			if (!s) {
				ret = -ENOMEM;
				debugf("problem allocating copy of version arg\n");
				goto free_and_return;
			}
			ret = p9_get_protocol_version(s);
			if (ret == -EINVAL) {
				free(s);
				goto free_and_return;
			}
			free(s);
			_proto_version = ret;
			break;
		default:
			continue;
		}
	}

free_and_return:
	free(tmp_options);
	return ret;
}

/**
 * p9_alloc_req - lookup/allocate a request by tag
 * @tag: numeric id for transaction
 *
 * this is a simple array lookup, but will grow the
 * request_slots as necessary to accommodate transaction
 * ids which did not previously have a slot.
 *
 * this code relies on the client spinlock to manage locks, its
 * possible we should switch to something else, but I'd rather
 * stick with something low-overhead for the common case.
 *
 */
struct p9_req_t *p9_client::p9_alloc_req(u16 tag, unsigned int max_size)
{
	// unsigned long flags;
	int row, col;
	struct p9_req_t *req;
	int alloc_msize = std::min<unsigned>(_msize, max_size);

	/* This looks up the original request by tag so we know which
	 * buffer to read the data into */
	tag++;

	if (tag >= _max_tag) {
		_lock.lock();
		/* check again since original check was outside of lock */
		while (tag >= _max_tag) {
			row = (tag / P9_ROW_MAXTAG);
			_reqs[row] = (struct p9_req_t *) calloc(P9_ROW_MAXTAG,
					sizeof(struct p9_req_t));

			if (!_reqs[row]) {
				debugf("Couldn't grow tag array\n");
				_lock.unlock();
				return nullptr;
			}
			for (col = 0; col < P9_ROW_MAXTAG; col++) {
				_reqs[row][col].status = REQ_STATUS_IDLE;
				_reqs[row][col].tc = NULL;
			}
			_max_tag += P9_ROW_MAXTAG;
		}
		_lock.unlock();
	}
	row = tag / P9_ROW_MAXTAG;
	col = tag % P9_ROW_MAXTAG;

	req = &_reqs[row][col];


	if (!req->tc)
		req->tc = p9_fcall_alloc(alloc_msize);
	if (!req->rc)
		req->rc = p9_fcall_alloc(alloc_msize);
	if (!req->tc || !req->rc)
	{
		debugf("Couldn't grow tag array\n");
		free(req->tc);
		free(req->rc);
		req->tc = req->rc = NULL;
		return nullptr;
	}

	p9pdu_reset(req->tc);
	p9pdu_reset(req->rc);

	req->tc->tag = tag-1;
	req->status = REQ_STATUS_ALLOC;

	return req;
}

/**
 * p9_tag_lookup - lookup a request by tag
 * @tag: numeric id for transaction
 *
 */
struct p9_req_t *p9_client::p9_lookup_req(u16 tag)
{
	int row, col;

	/* This looks up the original request by tag so we know which
	 * buffer to read the data into */
	tag++;

	if(tag >= _max_tag) 
		return NULL;

	row = tag / P9_ROW_MAXTAG;
	col = tag % P9_ROW_MAXTAG;

	return &_reqs[row][col];
}

/**
 * p9_free_req - free a request and clean-up as necessary
 * r: request to release
 *
 */
void p9_client::p9_free_req(struct p9_req_t *r)
{
	int tag = r->tc->tag;
	debugf("req %p tag: %d\n", r, tag);

	r->status = REQ_STATUS_IDLE;
	if (tag != P9_NOTAG && _tagpool.p9_idpool_check(tag))
		_tagpool.p9_idpool_put(tag);
}

/**
 * p9_check_errors - check 9p packet for error return and process it
 * @c: current client instance
 * @req: request to parse and check for error conditions
 *
 * returns error code if one is discovered, otherwise returns 0
 *
 * this will have to be more complicated if we have multiple
 * error packet types
 */

int p9_client::p9_check_errors(struct p9_req_t *req)
{
	int8_t type;
	int err;
	int ecode;

	err = p9_parse_header(req->rc, NULL, &type, NULL, 0);
	/*
	 * dump the response from server
	 * This should be after check errors which poplulate pdu_fcall.
	 */
	if (err) {
		debugf("couldn't parse header %d\n", err);
		return err;
	}
	if (type != P9_RERROR && type != P9_RLERROR)
		return 0;

	if (p9_is_proto_dotl()) {
		char *ename;
		err = p9pdu_readf(req->rc, _proto_version, "s?d",
				  &ename, &ecode);
		if (err)
			goto out_err;

		if (p9_is_proto_dotu() && ecode < 512)
			err = -ecode;

		if (!err) {
			err = p9_errstr2errno(ename, strlen(ename));

			debugf("<<< RERROR (%d) %s\n",
				 -ecode, ename);
		}
		free(ename);
	} else {
		err = p9pdu_readf(req->rc, _proto_version, "d", &ecode);
		err = -ecode;

		debugf("<<< RLERROR (%d)\n", -ecode);
	}

	return err;

out_err:
	debugf("couldn't parse error%d\n", err);

	return err;
}

struct p9_req_t *p9_client::p9_client_prepare_req(int8_t type, int req_size, 
	const char *fmt, va_list ap)
{
	int tag, err;
	struct p9_req_t *req;

	debugf("op %d\n", type);

	/* we allow for any status other than disconnected */
	if (_status == Disconnected)
		return nullptr;

	/* if status is begin_disconnected we allow only clunk request */
	if ((_status == BeginDisconnect) && (type != P9_TCLUNK))
		return nullptr;

	tag = P9_NOTAG;
	if (type != P9_TVERSION) {
		if ((tag = _tagpool.p9_idpool_get()) < 0)
			return nullptr;
	}

	;
	if (!(req = p9_alloc_req(tag, req_size)))
		return req;

	/* marshall the data */
	p9pdu_prepare(req->tc, tag, type);
	err = p9pdu_vwritef(req->tc, _proto_version, fmt, ap);
	if (err)
	{
		p9_free_req(req);
		return nullptr;
	}
	p9pdu_finalize(this, req->tc);

	return req;
}

struct p9_req_t *p9_client::p9_client_rpc(int8_t type, 
	const char *fmt, ...)
{
	va_list ap;
	int err;
	struct p9_req_t *req;

	va_start(ap, fmt);
	req = p9_client_prepare_req(type, _msize, fmt, ap);
	va_end(ap);
	if (!req)
		return req;

	/* Issue a request and wait for response
	 */
	err = _trans_mod->request(this, req);
	if (err < 0) {
		if (err != -ERESTART && err != -EFAULT)
			_status = Disconnected;
		goto reterr;
	}

	/*
	 * Make sure our req is coherent with regard to updates in other
	 * threads - echoes to wmb() in the callback
	 */
	smp_rmb();

	if (req->status == REQ_STATUS_ERROR) {
		debugf("req_status error %d\n", req->t_err);
		err = req->t_err;
	}
	if ((err == -ERESTART) && (_status == Connected)) {
		debugf("flushing\n");

		if (_trans_mod->cancel(this, req))
			p9_client_flush(req);

		/* if we received the response anyway, don't signal error */
		if (req->status == REQ_STATUS_RCVD)
			err = 0;
	}
	
	if (err < 0)
		goto reterr;

	err = p9_check_errors(req);
	if (!err)
		return req;
reterr:
	p9_free_req(req);
	return nullptr;
}

struct p9_fid *p9_client::p9_fid_create()
{
	int ret;
	struct p9_fid *fid;

	fid = (struct p9_fid *) malloc(sizeof(struct p9_fid));
	if (!fid)
		return nullptr;

	ret = _fidpool.p9_idpool_get();
	if (ret < 0) {
		free(fid);
		return nullptr;
	}
	fid->fid = ret;

	memset(&fid->qid, 0, sizeof(struct p9_qid));
	fid->mode = -1;
	fid->uid = (kuid_t) {(unsigned) ~0};
	fid->clnt = this;
	fid->rdir = nullptr;
	WITH_LOCK(_lock) {
		_fidlist.push_back(fid);
	}

	return fid;
}

void p9_client::p9_fid_destroy(struct p9_fid *fid)
{

	debugf("fid %d\n", fid->fid);
	_fidpool.p9_idpool_put(fid->fid);
	WITH_LOCK(_lock) {
		// Use remove or erase?
		_fidlist.remove(fid);
	}
	free(fid->rdir);
	free(fid);
}

/**
 * p9_client_flush - flush (cancel) a request
 * @oldreq: request to cancel
 *
 * This sents a flush for a particular request and links
 * the flush request to the original request.  The current
 * code only supports a single flush request although the protocol
 * allows for multiple flush requests to be sent for a single request.
 *
 */
int p9_client::p9_client_flush(struct p9_req_t *oldreq)
{
	struct p9_req_t *req;
	int16_t oldtag;
	int err;

	err = p9_parse_header(oldreq->tc, NULL, NULL, &oldtag, 1);
	if (err)
		return err;

	debugf(">>> TFLUSH tag %d\n", oldtag);

	req = p9_client_rpc(P9_TFLUSH, "w", oldtag);
	if (!req)
		return -1;

	/*
	 * if we haven't received a response for oldreq,
	 * remove it from the list
	 */
	if (oldreq->status == REQ_STATUS_SENT)
		if (_trans_mod->cancelled)
			_trans_mod->cancelled(this, oldreq);

	p9_free_req(req);
	return 0;
}

int p9_client::p9_client_version()
{
	int err = 0;
	struct p9_req_t *req;
	char *version;
	int msize;

	debugf(">>> TVERSION msize %d protocol %d\n",
		 _msize, _proto_version);

	switch (_proto_version) {
	case p9_proto_2000L:
		req = p9_client_rpc(P9_TVERSION, "ds",
					_msize, "9P2000.L");
		break;
	case p9_proto_2000u:
		req = p9_client_rpc(P9_TVERSION, "ds",
					_msize, "9P2000.u");
		break;
	case p9_proto_legacy:
		req = p9_client_rpc(P9_TVERSION, "ds",
					_msize, "9P2000");
		break;
	default:
		return -EINVAL;
	}

	if (!req)
		return -1;

	err = p9pdu_readf(req->rc, _proto_version, "ds", &msize, &version);
	if (err) {
		debugf("version error %d\n", err);
		goto error;
	}

	debugf("<<< RVERSION msize %d %s\n", msize, version);
	if (!strncmp(version, "9P2000.L", 8))
		_proto_version = p9_proto_2000L;
	else if (!strncmp(version, "9P2000.u", 8))
		_proto_version = p9_proto_2000u;
	else if (!strncmp(version, "9P2000", 6))
		_proto_version = p9_proto_legacy;
	else {
		err = -EREMOTEIO;
		goto error;
	}

	if ((unsigned) msize < _msize)
		_msize = msize;

error:
	free(version);
	p9_free_req(req);

	return err;
}


void p9stat_init(struct p9_wstat *stbuf)
{
    stbuf->name  = nullptr;
    stbuf->uid   = nullptr;
    stbuf->gid   = nullptr;
    stbuf->muid  = nullptr;
    stbuf->extension = nullptr;
}

int p9stat_read(struct p9_client *clnt, char *buf, int len, struct p9_wstat *st)
{
	struct p9_fcall fake_pdu;
	int ret;

	fake_pdu.size = len;
	fake_pdu.capacity = len;
	fake_pdu.sdata = (uint8_t *) buf;
	fake_pdu.offset = 0;

	ret = p9pdu_readf(&fake_pdu, clnt->p9_proto(), "S", st);
	if (ret) {
		debugf("<<< p9stat_read failed: %d\n", ret);
	}

	return ret;
}

void p9stat_free(struct p9_wstat *stbuf)
{
	free((void *)stbuf->name);
	free((void *)stbuf->uid);
	free((void *)stbuf->gid);
	free((void *)stbuf->muid);
	free(stbuf->extension);
}

int p9dirent_read(struct p9_client *clnt, char *buf, int len,
		  struct p9_dirent *dirent)
{
	struct p9_fcall fake_pdu;
	int ret;
	char *nameptr;

	fake_pdu.size = len;
	fake_pdu.capacity = len;
	fake_pdu.sdata = (uint8_t *) buf;
	fake_pdu.offset = 0;

	ret = p9pdu_readf(&fake_pdu, clnt->p9_proto(), "Qqbs", &dirent->qid,
			  &dirent->d_off, &dirent->d_type, &nameptr);
	if (ret) {
		debugf("<<< p9dirent_read failed: %d\n", ret);
		goto out;
	}

	strcpy(dirent->d_name, nameptr);
	free(nameptr);

out:
	return fake_pdu.offset;
}


/* match utils */

/**
 * match_one: - Determines if a string matches a simple pattern
 * @s: the string to examine for presence of the pattern
 * @p: the string containing the pattern
 * @args: array of %MAX_OPT_ARGS &substring_t elements. Used to return match
 * locations.
 *
 * Description: Determines if the pattern @p is present in string @s. Can only
 * match extremely simple token=arg style patterns. If the pattern is found,
 * the location(s) of the arguments will be returned in the @args array.
 */
static int match_one(char *s, const char *p, substring_t args[])
{
	char *meta;
	int argc = 0;

	if (!p)
		return 1;

	while(1) {
		int len = -1;
		meta = strchr(p, '%');
		if (!meta)
			return strcmp(p, s) == 0;

		if (strncmp(p, s, meta-p))
			return 0;

		s += meta - p;
		p = meta + 1;

		if (isdigit(*p))
			len = strtoul(p, (char **) &p, 10);
		else if (*p == '%') {
			if (*s++ != '%')
				return 0;
			p++;
			continue;
		}

		if (argc >= MAX_OPT_ARGS)
			return 0;

		args[argc].from = s;
		switch (*p++) {
		case 's': {
			size_t str_len = strlen(s);

			if (str_len == 0)
				return 0;
			if (len == -1 || (size_t) len > str_len)
				len = str_len;
			args[argc].to = s + len;
			break;
		}
		case 'd':
			strtol(s, &args[argc].to, 0);
			goto num;
		case 'u':
			strtoul(s, &args[argc].to, 0);
			goto num;
		case 'o':
			strtoul(s, &args[argc].to, 8);
			goto num;
		case 'x':
			strtoul(s, &args[argc].to, 16);
		num:
			if (args[argc].to == args[argc].from)
				return 0;
			break;
		default:
			return 0;
		}
		s = args[argc].to;
		argc++;
	}
}

/**
 * match_token: - Find a token (and optional args) in a string
 * @s: the string to examine for token/argument pairs
 * @table: match_table_t describing the set of allowed option tokens and the
 * arguments that may be associated with them. Must be terminated with a
 * &struct match_token whose pattern is set to the NULL pointer.
 * @args: array of %MAX_OPT_ARGS &substring_t elements. Used to return match
 * locations.
 *
 * Description: Detects which if any of a set of token strings has been passed
 * to it. Tokens can include up to MAX_OPT_ARGS instances of basic c-style
 * format identifiers which will be taken into account when matching the
 * tokens, and whose locations will be returned in the @args array.
 */
int match_token(char *s, const match_table_t table, substring_t args[])
{
	const struct match_token *p;

	for (p = table; !match_one(s, p->pattern, args) ; p++)
		;

	return p->token;
}

/**
 * match_number: scan a number in the given base from a substring_t
 * @s: substring to be scanned
 * @result: resulting integer on success
 * @base: base to use when converting string
 *
 * Description: Given a &substring_t and a base, attempts to parse the substring
 * as a number in that base. On success, sets @result to the integer represented
 * by the string and returns 0. Returns -ENOMEM, -EINVAL, or -ERANGE on failure.
 */
int match_number(substring_t *s, int *result, int base)
{
	char *endp;
	char *buf;
	int ret;
	long val;
	size_t len = s->to - s->from;

	buf = (char *) malloc(len + 1);
	if (!buf)
		return -ENOMEM;
	memcpy(buf, s->from, len);
	buf[len] = '\0';

	ret = 0;
	val = strtol(buf, &endp, base);
	if (endp == buf)
		ret = -EINVAL;
	else if (val < (long)INT_MIN || val > (long)INT_MAX)
		ret = -ERANGE;
	else
		*result = (int) val;
	free(buf);
	return ret;
}

/**
 * match_int: - scan a decimal representation of an integer from a substring_t
 * @s: substring_t to be scanned
 * @result: resulting integer on success
 *
 * Description: Attempts to parse the &substring_t @s as a decimal integer. On
 * success, sets @result to the integer represented by the string and returns 0.
 * Returns -ENOMEM, -EINVAL, or -ERANGE on failure.
 */
int match_int(substring_t *s, int *result)
{
	return match_number(s, result, 0);
}

/**
 * match_strdup: - allocate a new string with the contents of a substring_t
 * @s: &substring_t to copy
 *
 * Description: Allocates and returns a string filled with the contents of
 * the &substring_t @s. The caller is responsible for freeing the returned
 * string with kfree().
 */
char *match_strdup(const substring_t *s)
{
	size_t sz = s->to - s->from + 1;
	char *p = (char *) malloc(sz);
	if (p)
		strlcpy(p, (char *) s, sz);
	return p;
}
