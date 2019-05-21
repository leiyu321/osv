# Overview

	FreeBSD File System (9P Part)

```
	--------     ----------       --------       ------------
	|file  |  -> |dentry  |  ->   |vnode |    -> |v9fs_inode|
	--------  |  ----------  |    --------    |  ------------
	|flag  |  |  |refcnt  |  |    |ino   |    |  |fid       |
	|refcnt|  |  |path    |  | -- |mp    |    |  |handle_fid|
	|offset|  |  |vnode   | -- |  |vnops | -- |  -----------
	|dentry| --  |mp      | ----  |refcnt|  | |
	|data  |     |parent  |   |   |type  |  | |
	|type  |     |children|   |   |flag  |  | |
	| ...  |     |  ...   |   |   |mode  |  | |
	--------     ----------   |   |size  |  | |
	                          |   |data  | ----
	                          |   | ...  |  |
	                          |   --------  |
	                          |             |
	---------      ---------  |    -------  |
	|vfsops | <-   |mount  | <-    |vnops| <-
	|-------|  |   ---------       -------
	|mount  |  --  |vfsops |       |open |
	|unmount|      |flag   |       |close|
	|vget   |      |refcnt |       |read |
	|sync   |      |path   |       |write|
	|statfs |      |dev    |       | ... |
	---------      |root   |       -------
	               |covered|
	               |data   |  -->  ---------
	               |fsid   |       |v9fs_sb|
	               ---------       ---------
	                               |magic  |
	                               |bsize  |
	                               |inocnt |
	                               |v9ses  |
	                               ---------

```

## file

	Real File in OS
	
## dentry

	Directory Entry

## vnode

	FreeBSD Vnode including private data (inode, device, or others)

## mount

	Mount Point

## vfsops

	Specific FS Operations excluding Vnode Operations

| definition | description |
| :--------- | :---------- |
| int (\*vfs_mount)	(struct mount \*, const char \*, int, const void \*); | 挂载文件系统实例 |
| int (\*vfs_unmount)	(struct mount \*, int flags); | 卸载文件系统实例 |
| int (\*vfs_sync)		(struct mount \*); | 同步文件系统元数据到磁盘 |
| int (\*vfs_vget)		(struct mount \*, struct vnode \*); | <Unknown> |
| int (\*vfs_statfs)	(struct mount \*, struct statfs \*); | 获取文件系统统计信息 |

## vnops

	FreeBSD Vnode Operations (File Operations)

| definition | description | modification |
| :--------- | :---------- | :----------- |
| int (\*vnop_open_t)	(struct file \*fp); | 打开一个文件,并提供后续读写所需的句柄 | fp |
| int (\*vnop_close_t)	(struct vnode \*vp, struct file \*fp); | 关闭一个文件(不会被调用) |  |
| int (\*vnop_read_t)	(struct vnode \*vp, struct file \*fp, struct uio \*uio, int flag); | 读取文件到uio | uio |
| int (\*vnop_write_t)	(struct vnode \*vp, struct uio \*uio, int flag); | 将uio写入文件,并更新vp统计信息 | vp |
| int (\*vnop_seek_t)	(struct vnode \*vp, struct file \*fp, off_t oldoff, off_t off); | 保证后续读写可以在新位置进行, fp在外部会被修改 |  |
| int (\*vnop_ioctl_t)	(struct vnode \*vp, struct file \*fp, u_long cmd, void \*data); | 控制设备文件操作,可能会写data | \[data\] |
| int (\*vnop_fsync_t)	(struct vnode \*vp, struct file \*fp); | 将文件同步到磁盘 |  |
| int (\*vnop_readdir_t)	(struct vnode \*vp, file \*fp, struct dirent \*dir); | 读取目录列表下一项构建新的dirent,一般会修改fp->f_offset | fp, dir |
| int (\*vnop_lookup_t)	(struct vnode \*dvp, char \*name, struct vnode \*\*vpp); | 查找目录下指定文件构建新的vnode | vpp |
| int (\*vnop_create_t)	(struct vnode \*dvp, char \*name, mode_t mode); | 创建一个文件 |  |
| int (\*vnop_remove_t)	(struct vnode \*dvp, struct vnode \*vp, char \*name); | 删除一个文件 |  |
| int (\*vnop_rename_t)	(struct vnode \*dvp1, struct vnode \*vp1, char \*sname, struct vnode \*dvp2, struct vnode \*vp2, char \*dname); | 重命名一个文件 |  |
| int (\*vnop_mkdir_t)	(struct vnode \*dvp, char \*name, mode_t mode); | 创建一个目录 |  |
| int (\*vnop_rmdir_t)	(struct vnode \*dvp, struct vnode \*vp, char \*name); | 删除一个目录 |  |
| int (\*vnop_getattr_t)	(struct vnode \*vp, struct vattr \*vat); | 获取文件属性至vat | vat |
| int (\*vnop_setattr_t)	(struct vnode \*vp, struct vattr \*vat); | 设置文件属性,一般vnode内属性不受影响或由其他部分处理 | vat |
| int (\*vnop_inactive_t)	(struct vnode \*vp); | 失效文件打开的句柄,相当于关闭文件 | vp |
| int (\*vnop_truncate_t)	(struct vnode \*vp, off_t len); | 调整文件大小,并更新vp->v_size | vp |
| int (\*vnop_link_t)      (struct vnode \*ndvp, struct vnode \*vp, char \*name); | 建立硬链接,新的dentry已分配且关联同一vp |  |
| int (\*vnop_cache_t) (struct vnode \*, struct file \*, struct uio \*); | 将数据写入cache |  |
| int (\*vnop_fallocate_t) (struct vnode \*vp, int mode, loff_t off, loff_t len); | 预留存储空间 |  |
| int (\*vnop_readlink_t)  (struct vnode \*vp, struct uio \*uio); | 读取链接信息至uio | uio |
| int (\*vnop_symlink_t)   (struct vnode \*ndvp, char *name, char \*oldpath); | 建立软链接 |  |

---

	Lifecycle of Operations

```
	                           |-> READ  -> CACHE -|
	                           |-> WRITE -> SYNC  -|
	          |-> OPEN      -> |-> SEEK           -|-> INACTIVE
	          |-> CREATE       |-> IOCTL -> SYNC  -|
	          |-> REMOVE       |-> READDIR        -|
	          |-> RENAME
	          |-> MKDIR
	          |-> RMDIR
	LOOKUP -> |-> GETATTR
	          |-> SETATTR
	          |-> TRUNCATE
	          |-> LINK
	          |-> FALLOCATE
	          |-> READLINK
	          |-> SYMLINK

```

---

	Descriptions of Operations

| Operation | NFS | ZFS | 9P |
| :-------- | :-- | :-- | :- |
| LOOKUP    | 读取inode构建新的vnode,填充\{v_ino, v_type, v_mode, v_size, v_mount\} | 读取inode构建新的vnode,填充\{v_ino, v_type, v_mode, v_size, v_mount, v_data\} | 读取inode构建新的vnode,填充\{v_ino, v_type, v_mode, v_size, v_mount\} |
| OPEN      | 打开文件获得句柄以填充v_data | 更新内部文件打开计数 | 打开文件获得fid以填充v_data |
| READ      | 使用句柄读取文件至uio | 从cache或直接读取文件至uio | 使用fid读取文件至uio |
| WRITE     | 必要时先通过TRUNCATE调整文件大小,使用句柄将uio写入文件,并更新v_size | 将uio写入文件,并更新v_size | 使用fid将uio写入文件,并更新v_size |
| SEEK      | \[NULL\] | 检查位置的合法性 | \[NULL\]? |
| IOCTL     | \[NULL\] | 执行控制指令,可能会读写data | \[NULL\] |
| READDIR   | 使用句柄读取下一项,填充\{d_ino, d_type, d_name\},并更新f_offset | 处理特殊项\{., .., .zfs\}或读取常规下一项,填充\{d_ino, d_type, d_name\},并更新f_offset | 使用fid读取下一项,填充\{d_ino, d_off, d_type, d_name},并更新f_offset |
| CACHE     | \[NULL\] | 将uio写入缓存 | \[NULL\]? |
| SYNC      | 使用句柄同步文件 |  | 使用fid同步文件 |
| CREATE    | 创建文件 | 创建文件 | 创建文件 |
| REMOVE    | 删除文件 | 删除文件 | 删除文件 |
| RENAME    | 重命名文件 | 重命名文件 | 重命名文件 |
| MKDIR     | 创建目录 | 创建目录 | 创建目录 |
| RMDIR     | 删除目录 | 删除目录 | 删除目录 |
| GETATTR   | 获取属性 | 获取属性 | 获取属性 |
| SETATTR   | 设置属性 | 设置属性 | 设置属性 |
| TRUNCATE  | 调整文件大小,并更新v_size | 调整文件大小 | 调整文件大小,并更新v_size |
| LINK      | \[NULL\] | 建立硬连接 | 建立硬连接 |
| FALLOCATE | \[NULL\] | 分配额外空间 | 分配额外空间 |
| READLINK  | 读取连接信息至uio | 读取连接信息至uio | 读取连接信息至uio |
| SYMLINK   | 建立软连接 | 建立软连接 | 建立软连接 |


## Xfs_info

	Specific FS Information including SuperBlock, Inodes and etc

## Xfs_sb

	Specific FS SuperBlock

## Xfs_inode

	Specific FS Inode
