/* 
 * v9fs_vfsops.cc
 *     v9fs_vfsops [V9FS Entry]
 *         v9fs_mount   - Mount 9p file system
 *         v9fs_unmount - Unmount 9p fille system
 *         v9fs_sync    - Sync 9p file system
 *         v9fs_statfs  - View stat of 9p file system
 *         v9fs_vnops   - [See v9fs_vnops.cc]
 *
 */

#include "virtfs.hh"
#include <sys/types.h>
#include <osv/device.h>
#include <osv/debug.h>
#include <iomanip>
#include <iostream>
#include <api/sys/mount.h>

#define NAME_MAX         255    /* # chars in a file name */

static int v9fs_mount(struct mount *mp, const char *dev, int flags, const void *data);
static int v9fs_unmount(struct mount *mp, int flags);
static int v9fs_sync(struct mount *mp);
static int v9fs_statfs(struct mount *mp, struct statfs *statp);

#define v9fs_vget    ((vfsop_vget_t)vfs_nullop)

#if defined(V9FS_DIAGNOSTICS_ENABLED)
std::atomic<long> v9fs_block_read_ms(0);
std::atomic<long> v9fs_block_read_count(0);
std::atomic<long> v9fs_block_allocated(0);
std::atomic<long> v9fs_cache_reads(0);
std::atomic<long> v9fs_cache_misses(0);
#endif

/*
 * File system operations
 */
struct vfsops v9fs_vfsops = {
    v9fs_mount,		/* mount */
    v9fs_unmount,	/* unmount */
    v9fs_sync,		/* sync */
    v9fs_vget,      /* vget */
    v9fs_statfs,	/* statfs */
    &v9fs_vnops	    /* vnops */
};

#define MAX_LFS_FILESIZE    ((loff_t)LLONG_MAX)

static inline int fls(int x)
{
    int r = 32;

    if (!x)
        return 0;
    if (!(x & 0xffff0000u)) {
        x <<= 16;
        r -= 16;
    }
    if (!(x & 0xff000000u)) {
        x <<= 8;
        r -= 8;
    }
    if (!(x & 0xf0000000u)) {
        x <<= 4;
        r -= 4;
    }
    if (!(x & 0xc0000000u)) {
        x <<= 2;
        r -= 2;
    }
    if (!(x & 0x80000000u)) {
        x <<= 1;
        r -= 1;
    }
    return r;
}

/*
 * Mount a file system
 */
static int v9fs_mount(struct mount *mp, const char *dev, int flags, const void *data)
{
    struct v9fs_super_block *sb = nullptr;
    struct v9fs_session_info *v9ses = nullptr;
    struct v9fs_inode *root;
    struct p9_fid *fid;
    struct vnode *vp;
    int retval;

    v9ses = (struct v9fs_session_info *) malloc(sizeof(struct v9fs_session_info));
    memset(v9ses, 0, sizeof(struct v9fs_session_info));

    fid = v9fs_session_init(v9ses, dev, (char *) data);
    if (!fid) {
        retval = -1;
        goto free_session;
    }

    sb = (struct v9fs_super_block *) malloc(sizeof(struct v9fs_super_block));
    if (!sb) {
        retval = -1;
        goto clunk_fid;
    }

    sb->s_v9ses = v9ses;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_blocksize_bits = fls(v9ses->maxdata - 1);
    sb->s_blocksize = 1 << sb->s_blocksize_bits;
    sb->s_magic = V9FS_MAGIC;

    sb->s_flags |= MS_ACTIVE | MS_DIRSYNC | MS_NOATIME;
    if (!v9ses->cache)
        sb->s_flags |= MS_SYNCHRONOUS;

#ifdef CONFIG_9P_FS_POSIX_ACL
    if ((v9ses->flags & V9FS_ACL_MASK) == V9FS_POSIX_ACL)
        sb->s_flags |= MS_POSIXACL;
#endif


    vp = mp->m_root->d_vnode;
    if (fid->clnt->p9_is_proto_dotl())
    {

        struct p9_stat_dotl *st = nullptr;
        st = p9_client::p9_client_getattr_dotl(fid, P9_STATS_BASIC);
        if (!st)
        {
            retval = -1;
            goto release_sb;
        }
        vp->v_ino = v9fs_qid2ino(&st->qid);
        v9fs_set_vnode_dotl(vp, st);
    }
    else
    {

        struct p9_wstat *st = nullptr;
        st = p9_client::p9_client_getattr(fid);
        if (!st)
        {
            retval = -1;
            goto release_sb;
        }
        vp->v_ino = v9fs_qid2ino(&st->qid);
        v9fs_set_vnode(vp, st);
    }
    vp->v_mount = mp;
    root = (struct v9fs_inode *) malloc(sizeof(v9fs_inode));
    memset(root, 0, sizeof(v9fs_inode));
    root->fid = fid;
    vp->v_data = root;

    sb->s_root = mp->m_root;
    mp->m_data = sb;

    debugf(" simple set mount, return 0\n");

    return 0;

clunk_fid:
    p9_client::p9_client_clunk(fid);
    v9fs_session_close(v9ses);
free_session:
    free(v9ses);
    return retval;

release_sb:
    /*
     * we will do the session_close and root dentry release
     * in the below call. But we need to clunk fid, because we haven't
     * attached the fid to dentry so it won't get clunked
     * automatically.
     */
    p9_client::p9_client_clunk(fid);
    free(sb);
    return retval;
}

/*
 * Unmount a file system
 */
static int v9fs_unmount(struct mount *mp, int flags)
{
    struct v9fs_super_block *sb = (struct v9fs_super_block *) mp->m_data;
    struct v9fs_session_info *v9ses = (struct v9fs_session_info *) sb->s_v9ses;

    v9fs_session_begin_cancel(v9ses);
    v9fs_session_cancel(v9ses);
    v9fs_session_close(v9ses);
    sb->s_v9ses = nullptr;

    delete v9ses;
    delete sb;

#if defined(V9FS_DIAGNOSTICS_ENABLED)
    debugf("V9FS: spent %.2f ms reading from disk\n", ((double) v9fs_block_read_ms.load()) / 1000);
    debugf("V9FS: read %d 512-byte blocks from disk\n", v9fs_block_read_count.load());
    debugf("V9FS: allocated %d 512-byte blocks of cache memory\n", v9fs_block_allocated.load());
    long total_cache_reads = v9fs_cache_reads.load();
    double hit_ratio = total_cache_reads > 0 ? (v9fs_cache_reads.load() - v9fs_cache_misses.load()) / ((double)total_cache_reads) : 0;
    debugf("V9FS: hit ratio is %.2f%%\n", hit_ratio * 100);
#endif
	return 0;
}

/* Flush unwritten data
 */
static int v9fs_sync(struct mount *mp)
{
    debugf("V9FS: Unsupported file operation: sync\n");
    return 0;
}

/* Inquire file system status
 */
static int v9fs_statfs(struct mount *mp, struct statfs *statp)
{
    struct v9fs_super_block *sb;
    struct p9_fid *fid;
    struct p9_rstatfs rs;
    int res;

    fid = ((struct v9fs_inode *) (mp->m_root->d_vnode->v_data))->fid;
    if (!fid) {
        return -1;
    }

    sb = (struct v9fs_super_block *) mp->m_data;

    if (fid->clnt->p9_is_proto_dotl()) {
        res = p9_client::p9_client_statfs(fid, &rs);
        if (res == 0) {
            statp->f_type = rs.type;
            statp->f_bsize = rs.bsize;
            statp->f_blocks = rs.blocks;
            statp->f_bfree = rs.bfree;
            statp->f_bavail = rs.bavail;
            statp->f_files = rs.files;
            statp->f_ffree = rs.ffree;
            statp->f_fsid.__val[0] = rs.fsid & 0xFFFFFFFFUL;
            statp->f_fsid.__val[1] = (rs.fsid >> 32) & 0xFFFFFFFFUL;
            statp->f_namelen = rs.namelen;
        }
        if (res != -ENOSYS)
            return res;
    }
    

    statp->f_type = sb->s_magic;
    statp->f_bsize = PAGE_SIZE;
    statp->f_namelen = NAME_MAX;
    res = 0;

    return res;
}