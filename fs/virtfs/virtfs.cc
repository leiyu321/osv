
#include "virtfs.hh"
#include <osv/device.h>
#include <osv/bio.h>
#include <osv/spinlock.h>

#if defined(V9FS_DIAGNOSTICS_ENABLED)
extern std::atomic<long> v9fs_block_read_count;
extern std::atomic<long> v9fs_block_read_ms;
#endif

static spinlock_t v9fs_sessionlist_lock;


enum {
	/* Options that take integer arguments */
	Opt_debug, Opt_dfltuid, Opt_dfltgid, Opt_afid,
	/* String options */
	Opt_uname, Opt_remotename, Opt_cache, Opt_cachetag,
	/* Options that take no arguments */
	Opt_nodevmap,
	/* Cache options */
	Opt_cache_loose, Opt_fscache, Opt_mmap,
	/* Access options */
	Opt_access, Opt_posixacl,
	/* Error token */
	Opt_err
};

static const match_table_t tokens = {
	{Opt_debug, "debug=%x"},
	{Opt_dfltuid, "dfltuid=%u"},
	{Opt_dfltgid, "dfltgid=%u"},
	{Opt_afid, "afid=%u"},
	{Opt_uname, "uname=%s"},
	{Opt_remotename, "aname=%s"},
	{Opt_nodevmap, "nodevmap"},
	{Opt_cache, "cache=%s"},
	{Opt_cache_loose, "loose"},
	{Opt_fscache, "fscache"},
	{Opt_mmap, "mmap"},
	{Opt_cachetag, "cachetag=%s"},
	{Opt_access, "access=%s"},
	{Opt_posixacl, "posixacl"},
	{Opt_err, NULL}
};


static const char *const v9fs_cache_modes[nr__p9_cache_modes] = {
	[CACHE_NONE]	= "none",
	[CACHE_MMAP]	= "mmap",
	[CACHE_LOOSE]	= "loose",
	[CACHE_FSCACHE]	= "fscache",
};


static int get_cache_mode(char *s)
{
	int version = -EINVAL;

	if (!strcmp(s, "loose")) {
		version = CACHE_LOOSE;
		debugf("Cache mode: loose\n");
	} else if (!strcmp(s, "fscache")) {
		version = CACHE_FSCACHE;
		debugf("Cache mode: fscache\n");
	} else if (!strcmp(s, "mmap")) {
		version = CACHE_MMAP;
		debugf("Cache mode: mmap\n");
	} else if (!strcmp(s, "none")) {
		version = CACHE_NONE;
		debugf("Cache mode: none\n");
	} else
		debugf("Unknown Cache mode %s\n", s);
	return version;
}

/**
 * v9fs_parse_options - parse mount options into session structure
 * @v9ses: existing v9fs session information
 *
 * Return 0 upon success, -ERRNO upon failure.
 */
static int v9fs_parse_options(struct v9fs_session_info *v9ses, char *opts)
{
	char *options, *tmp_options;
	substring_t args[MAX_OPT_ARGS];
	char *p;
	int option = 0;
	char *s, *e;
	int ret = 0;

	/* setup defaults */
	v9ses->afid = ~0;
	v9ses->debug = 0;
	v9ses->cache = CACHE_NONE;
#ifdef CONFIG_9P_FSCACHE
	v9ses->cachetag = NULL;
#endif

	if (!opts)
		return 0;

	tmp_options = strdup(opts);
	if (!tmp_options) {
		ret = -ENOMEM;
		goto fail_option_alloc;
	}
	options = tmp_options;

	while ((p = strsep(&options, ",")) != NULL) {
		int token, r;
		if (!*p)
			continue;
		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_debug:
			r = match_int(&args[0], &option);
			if (r < 0) {
				debugf("integer field, but no integer?\n");
				ret = r;
				continue;
			}
			v9ses->debug = option;
#ifdef CONFIG_NET_9P_DEBUG
			debugf_level = option;
#endif
			break;

		case Opt_dfltuid:
			r = match_int(&args[0], &option);
			if (r < 0) {
				debugf("integer field, but no integer?\n");
				ret = r;
				continue;
			}
			v9ses->dfltuid = (kuid_t) {(unsigned) option};
			if ((v9ses->dfltuid).val == (unsigned) ~0) {
				debugf("uid field, but not a uid?\n");
				ret = -EINVAL;
				continue;
			}
			break;
		case Opt_dfltgid:
			r = match_int(&args[0], &option);
			if (r < 0) {
				debugf("integer field, but no integer?\n");
				ret = r;
				continue;
			}
			v9ses->dfltgid = (kgid_t) {(unsigned) option};
			if ((v9ses->dfltgid).val == (unsigned) ~0) {
				debugf("gid field, but not a gid?\n");
				ret = -EINVAL;
				continue;
			}
			break;
		case Opt_afid:
			r = match_int(&args[0], &option);
			if (r < 0) {
				debugf("integer field, but no integer?\n");
				ret = r;
				continue;
			}
			v9ses->afid = option;
			break;
		case Opt_uname:
			free(v9ses->uname);
			v9ses->uname = match_strdup(&args[0]);
			if (!v9ses->uname) {
				ret = -ENOMEM;
				goto free_and_return;
			}
			break;
		case Opt_remotename:
			free(v9ses->aname);
			v9ses->aname = match_strdup(&args[0]);
			if (!v9ses->aname) {
				ret = -ENOMEM;
				goto free_and_return;
			}
			break;
		case Opt_nodevmap:
			v9ses->nodev = 1;
			break;
		case Opt_cache_loose:
			v9ses->cache = CACHE_LOOSE;
			break;
		case Opt_fscache:
			v9ses->cache = CACHE_FSCACHE;
			break;
		case Opt_mmap:
			v9ses->cache = CACHE_MMAP;
			break;
		case Opt_cachetag:
#ifdef CONFIG_9P_FSCACHE
			free(v9ses->cachetag);
			v9ses->cachetag = match_strdup(&args[0]);
#endif
			break;
		case Opt_cache:
			s = match_strdup(&args[0]);
			if (!s) {
				ret = -ENOMEM;
				debugf("problem allocating copy of cache arg\n");
				goto free_and_return;
			}
			ret = get_cache_mode(s);
			if (ret == -EINVAL) {
				free(s);
				goto free_and_return;
			}

			v9ses->cache = ret;
			free(s);
			break;

		case Opt_access:
			s = match_strdup(&args[0]);
			if (!s) {
				ret = -ENOMEM;
				debugf("problem allocating copy of access arg\n");
				goto free_and_return;
			}

			v9ses->flags &= ~V9FS_ACCESS_MASK;
			if (strcmp(s, "user") == 0)
				v9ses->flags |= V9FS_ACCESS_USER;
			else if (strcmp(s, "any") == 0)
				v9ses->flags |= V9FS_ACCESS_ANY;
			else if (strcmp(s, "client") == 0) {
				v9ses->flags |= V9FS_ACCESS_CLIENT;
			} else {
				uid_t uid;
				v9ses->flags |= V9FS_ACCESS_SINGLE;
				uid = strtoul(s, &e, 10);
				if (*e != '\0') {
					ret = -EINVAL;
					debugf("Unknown access argument %s\n",
						s);
					free(s);
					goto free_and_return;
				}
				v9ses->uid = (kuid_t) {uid};
				if ((v9ses->uid).val == (unsigned) ~0) {
					ret = -EINVAL;
					debugf("Uknown uid %s\n", s);
					free(s);
					goto free_and_return;
				}
			}

			free(s);
			break;

		case Opt_posixacl:
#ifdef CONFIG_9P_FS_POSIX_ACL
			v9ses->flags |= V9FS_POSIX_ACL;
#else
			debugf("Not defined CONFIG_9P_FS_POSIX_ACL. Ignoring posixacl option\n");
#endif
			break;

		default:
			continue;
		}
	}

free_and_return:
	free(tmp_options);
fail_option_alloc:
	return ret;
}

/**
 * v9fs_session_init - initialize session
 * @v9ses: session information structure
 * @dev_name: device being mounted
 * @data: options
 *
 */
struct p9_fid *v9fs_session_init(struct v9fs_session_info *v9ses,
		  const char *dev_name, char *data)
{
	struct p9_fid *fid;
	int rc = -ENOMEM;

	v9ses->uname = strdup(V9FS_DEFUSER);
	if (!v9ses->uname)
		goto err_names;

	v9ses->aname = strdup(V9FS_DEFANAME);
	if (!v9ses->aname)
		goto err_names;
	// init_rwsem(&v9ses->rename_sem);

	v9ses->uid = (kuid_t) {(unsigned) ~0};
	v9ses->dfltuid = (kuid_t) {(unsigned) ~0};
	v9ses->dfltgid = (kgid_t) {(unsigned) ~0};


	try
	{
		v9ses->clnt = new p9_client(dev_name, data);
	}
	catch (std::runtime_error& err)
	{
		// rc = ;
		debugf("problem initializing 9p client: %s\n", err.what());
		goto err_names;
	}

	v9ses->flags = V9FS_ACCESS_USER;

	if (v9ses->clnt->p9_is_proto_dotl()) {
		v9ses->flags = V9FS_ACCESS_CLIENT;
		v9ses->flags |= V9FS_PROTO_2000L;
	} else if (v9ses->clnt->p9_is_proto_dotu()) {
		v9ses->flags |= V9FS_PROTO_2000U;
	}


	rc = v9fs_parse_options(v9ses, data);
	if (rc < 0)
		goto err_clnt;

	v9ses->maxdata = v9ses->clnt->p9_msize() - P9_IOHDRSZ;

	if (!(v9ses->clnt->p9_is_proto_dotl()) &&
	    ((v9ses->flags & V9FS_ACCESS_MASK) == V9FS_ACCESS_CLIENT)) {
		/*
		 * We support ACCESS_CLIENT only for dotl.
		 * Fall back to ACCESS_USER
		 */
		v9ses->flags &= ~V9FS_ACCESS_MASK;
		v9ses->flags |= V9FS_ACCESS_USER;
	}
	/*FIXME !! */
	/* for legacy mode, fall back to V9FS_ACCESS_ANY */
	if (!(v9ses->clnt->p9_is_proto_dotu() || v9ses->clnt->p9_is_proto_dotl()) &&
		((v9ses->flags&V9FS_ACCESS_MASK) == V9FS_ACCESS_USER)) {

		v9ses->flags &= ~V9FS_ACCESS_MASK;
		v9ses->flags |= V9FS_ACCESS_ANY;
		v9ses->uid = (kuid_t) {(unsigned) ~0};
	}
	if (!(v9ses->clnt->p9_is_proto_dotl()) ||
		!((v9ses->flags & V9FS_ACCESS_MASK) == V9FS_ACCESS_CLIENT)) {
		/*
		 * We support ACL checks on clinet only if the protocol is
		 * 9P2000.L and access is V9FS_ACCESS_CLIENT.
		 */
		v9ses->flags &= ~V9FS_ACL_MASK;
	}

	fid = v9ses->clnt->p9_client_attach(NULL, v9ses->uname, (kuid_t) {(unsigned) ~0},
							v9ses->aname);
	if (!fid) {
		// rc = PTR_ERR(fid);
		debugf("cannot attach\n");
		goto err_clnt;
	}

	if ((v9ses->flags & V9FS_ACCESS_MASK) == V9FS_ACCESS_SINGLE)
		fid->uid = v9ses->uid;
	else
		fid->uid = (kuid_t) {(unsigned) ~0};

#ifdef CONFIG_9P_FSCACHE
	/* register the session for caching */
	/* 暂时不知道用途
	 */
	// v9fs_cache_session_get_cookie(v9ses);
#endif
	// WITH_LOCK(v9fs_sessionlist_lock) {
	// 	list_add(&v9ses->slist, &v9fs_sessionlist);
	// }
	return fid;

err_clnt:
	// p9_client_destroy(v9ses->clnt);
	delete v9ses->clnt;
	v9ses->clnt = nullptr;
err_names:
	free(v9ses->uname);
	free(v9ses->aname);
	return nullptr;
}

/**
 * v9fs_session_close - shutdown a session
 * @v9ses: session information structure
 *
 */
void v9fs_session_close(struct v9fs_session_info *v9ses)
{
	if (v9ses->clnt) {
		delete v9ses->clnt;
		v9ses->clnt = NULL;
	}

#ifdef CONFIG_9P_FSCACHE
	if (v9ses->fscache) {
		// v9fs_cache_session_put_cookie(v9ses);
		free(v9ses->cachetag);
	}
#endif
	free(v9ses->uname);
	free(v9ses->aname);

	// spin_lock(&v9fs_sessionlist_lock);
	// list_del(&v9ses->slist);
	// spin_unlock(&v9fs_sessionlist_lock);
}

/**
 * v9fs_session_cancel - terminate a session
 * @v9ses: session to terminate
 *
 * mark transport as disconnected and cancel all pending requests.
 */
void v9fs_session_cancel(struct v9fs_session_info *v9ses) {
	debug("cancel session %p\n", v9ses);
	v9ses->clnt->p9_client_disconnect();
}

/**
 * v9fs_session_begin_cancel - Begin terminate of a session
 * @v9ses: session to terminate
 *
 * After this call we don't allow any request other than clunk.
 */
void v9fs_session_begin_cancel(struct v9fs_session_info *v9ses)
{
	debug("begin cancel session %p\n", v9ses);
	v9ses->clnt->p9_client_begin_disconnect();
}


/* Convert qid into ino
 */
ino_t v9fs_qid2ino(struct p9_qid *qid)
{
	u64 path = qid->path + 2;
	ino_t i = 0;

	if (sizeof(ino_t) == sizeof(path))
		memcpy(&i, &path, sizeof(ino_t));
	else
		i = (ino_t) (path ^ (path >> 32));

	return i;
}

/* Fill vnode with p9_wstat
 */
void v9fs_set_vnode(struct vnode* vp, struct p9_wstat *st)
{
	vp->v_type = IFTOVT(st->type & S_IFMT);
	vp->v_mode = st->mode & ~S_IFMT;
	vp->v_size = st->size;
}

/* Fill vnode with p9_wstat_dotl
 */
void v9fs_set_vnode_dotl(struct vnode* vp, struct p9_stat_dotl *st)
{
	uint64_t type = st->st_mode & S_IFMT;
	if (S_ISCHR(type) || S_ISBLK(type) || S_ISFIFO(type) || S_ISSOCK(type)) {
       	// FIXME: Not sure it's the right error code.
       	debugf("V9FS: Invalid file type: %d\n", IFTOVT(type));
   	}
    vp->v_type = IFTOVT(type);
	if ((st->st_result_mask & P9_STATS_BASIC) == P9_STATS_BASIC)
	{
		vp->v_mode = st->st_mode & S_IALLUGO;
	}
	else
	{
		vp->v_mode = st->st_mode & ~S_IFMT;
	}
	vp->v_size = st->st_size;
}

/* Convert Linux specific open flags to plan 9 mode bits
 */
int v9fs_flags2omode(int flags, int extended)
{
	int ret = 0;
	switch (flags&3) {
	default:
	case O_RDONLY:
		ret = P9_OREAD;
		break;
	case O_WRONLY:
		ret = P9_OWRITE;
		break;
	case O_RDWR:
		ret = P9_ORDWR;
		break;
	}

	if (extended) {
		if (flags & O_EXCL)
			ret |= P9_OEXCL;

		if (flags & O_APPEND)
			ret |= P9_OAPPEND;
	}

	return ret;
}

struct dotl_openflag_map {
	int open_flag;
	int dotl_flag;
};

static int v9fs_mapped_dotl_flags(int flags)
{
	unsigned i;
	int rflags = 0;
	struct dotl_openflag_map dotl_oflag_map[] = {
		{ O_CREAT,	P9_DOTL_CREATE },
		{ O_EXCL,	P9_DOTL_EXCL },
		{ O_NOCTTY,	P9_DOTL_NOCTTY },
		{ O_APPEND,	P9_DOTL_APPEND },
		{ O_NONBLOCK,	P9_DOTL_NONBLOCK },
		{ O_DSYNC,	P9_DOTL_DSYNC },
		{ FASYNC,	P9_DOTL_FASYNC },
		{ O_DIRECT,	P9_DOTL_DIRECT },
		{ O_LARGEFILE,	P9_DOTL_LARGEFILE },
		{ O_DIRECTORY,	P9_DOTL_DIRECTORY },
		{ O_NOFOLLOW,	P9_DOTL_NOFOLLOW },
		{ O_NOATIME,	P9_DOTL_NOATIME },
		{ O_CLOEXEC,	P9_DOTL_CLOEXEC },
		{ O_SYNC,	P9_DOTL_SYNC},
	};
	for (i = 0; i < ARRAY_SIZE(dotl_oflag_map); i++) {
		if (flags & dotl_oflag_map[i].open_flag)
			rflags |= dotl_oflag_map[i].dotl_flag;
	}
	return rflags;
}

/* Convert Linux specific open flags to plan 9 mode bits
 */
int v9fs_flags2omode_dotl(int flags)
{
	int rflags = 0;

	/*
	 * We have same bits for P9_DOTL_READONLY, P9_DOTL_WRONLY
	 * and P9_DOTL_NOACCESS
	 */
	rflags |= flags & O_ACCMODE;
	rflags |= v9fs_mapped_dotl_flags(flags);

	return rflags;
}

void v9fs_blank_wstat(struct p9_wstat *wstat)
{
	wstat->type = ~0;
	wstat->dev = ~0;
	wstat->qid.type = ~0;
	wstat->qid.version = ~0;
	wstat->qid.path = ~0;
	wstat->mode = ~0;
	wstat->atime = ~0;
	wstat->mtime = ~0;
	wstat->length = ~0;
	wstat->name = NULL;
	wstat->uid = NULL;
	wstat->gid = NULL;
	wstat->muid = NULL;
	wstat->n_uid = (kuid_t) {(unsigned) ~0};
	wstat->n_gid = (kgid_t) {(unsigned) ~0};
	wstat->n_muid = (kuid_t) {(unsigned) ~0};
	wstat->extension = NULL;
}