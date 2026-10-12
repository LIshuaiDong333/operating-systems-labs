#define _XOPEN_SOURCE 700
#include "nfs.h"

/******************************************************************************
* SECTION: Globals & Macros
*******************************************************************************/
#define OPTION(t, p)  { t, offsetof(struct custom_options, p), 1 }

static const struct fuse_opt option_spec[] = {
    OPTION("--device=%s", device),
    FUSE_OPT_END
};

struct custom_options nfs_options;    /* 全局选项 */
struct nfs_super      nfs_super;      /* 超级块实例 */

/******************************************************************************
* SECTION: FUSE Operations Implementation
*******************************************************************************/

/**
 * @brief 挂载文件系统
 */
void* nfs_init(struct fuse_conn_info * conn_info) {
    if (nfs_mount(nfs_options) != 0) {
        NFS_DBG("[%s] mount error\n", __func__);
        fuse_exit(fuse_get_context()->fuse);
        return NULL;
    }
    return NULL;
}

/**
 * @brief 卸载文件系统
 */
void nfs_destroy(void* p) {
    if (nfs_umount() != NFS_ERROR_NONE) {
        NFS_DBG("[%s] unmount error\n", __func__);
        fuse_exit(fuse_get_context()->fuse);
        return;
    }
}

/**
 * @brief 创建目录
 * 1. 解析路径找到父目录
 * 2. 检查父目录类型和存在性
 * 3. 分配新目录的 Dentry 和 Inode
 */
int nfs_mkdir(const char* path, mode_t mode) {
    (void)mode;
    boolean is_find, is_root;
    char* fname;
    struct nfs_dentry* last_dentry = nfs_lookup(path, &is_find, &is_root);
    struct nfs_dentry* dentry;
    struct nfs_inode* inode;

    /* 目录已存在 */
    if (is_find) {
        return -NFS_ERROR_EXISTS;
    }

    /* 父节点必须是目录，不能是普通文件 */
    if (NFS_IS_REG(last_dentry->inode)) {
        return -NFS_ERROR_UNSUPPORTED;
    }

    /* 创建新目录 */
    fname  = nfs_get_fname(path);
    dentry = new_dentry(fname, NFS_DIR);
    dentry->parent = last_dentry;
    
    inode  = nfs_alloc_inode(dentry);
    if ((long)inode < 0) return (long)inode; // Error check if alloc returns error code cast to ptr

    nfs_alloc_dentry(last_dentry->inode, dentry);

    return NFS_ERROR_NONE;
}

/**
 * @brief 获取文件属性 (stat)
 */
int nfs_getattr(const char* path, struct stat * nfs_stat) {
    boolean is_find, is_root;
    struct nfs_dentry* dentry = nfs_lookup(path, &is_find, &is_root);

    if (is_find == FALSE) {
        return -NFS_ERROR_NOTFOUND;
    }

    /* 填充基础属性 */
    memset(nfs_stat, 0, sizeof(struct stat));
    
    if (NFS_IS_DIR(dentry->inode)) {
        nfs_stat->st_mode = S_IFDIR | NFS_DEFAULT_PERM;
        nfs_stat->st_size = dentry->inode->dir_cnt * sizeof(struct nfs_dentry_d);
    } else if (NFS_IS_REG(dentry->inode)) {
        nfs_stat->st_mode = S_IFREG | NFS_DEFAULT_PERM;
        nfs_stat->st_size = dentry->inode->size;
    }

    nfs_stat->st_nlink   = 1;
    nfs_stat->st_uid     = getuid();
    nfs_stat->st_gid     = getgid();
    nfs_stat->st_atime   = time(NULL);
    nfs_stat->st_mtime   = time(NULL);
    nfs_stat->st_blksize = NFS_IO_SZ();
    nfs_stat->st_blocks  = NFS_DATA_PER_FILE; // 这是一个硬编码值

    /* 根目录特殊处理 */
    if (is_root) {
        nfs_stat->st_size   = nfs_super.sz_usage;
        nfs_stat->st_blocks = NFS_DISK_SZ() / NFS_IO_SZ();
        nfs_stat->st_nlink  = 2;
    }

    return NFS_ERROR_NONE;
}

/**
 * @brief 读取目录项 (readdir)
 * 根据 offset 获取目录下的第 N 个子项，并通过 filler 填入 buffer
 */
int nfs_readdir(const char * path, void * buf, fuse_fill_dir_t filler, off_t offset,
                struct fuse_file_info * fi) {
    boolean is_find, is_root;
    int     cur_dir_idx = offset;
    struct nfs_dentry* dentry = nfs_lookup(path, &is_find, &is_root);
    struct nfs_dentry* sub_dentry;
    struct nfs_inode* inode;

    if (is_find) {
        inode = dentry->inode;
        /* 根据 offset 查找链表中的第 N 个 Dentry */
        sub_dentry = nfs_get_dentry(inode, cur_dir_idx);
        if (sub_dentry) {
            filler(buf, sub_dentry->fname, NULL, ++offset);
        }
        return NFS_ERROR_NONE;
    }
    return -NFS_ERROR_NOTFOUND;
}

/**
 * @brief 创建普通文件或其他类型节点
 */
int nfs_mknod(const char* path, mode_t mode, dev_t dev) {
    boolean is_find, is_root;
    struct nfs_dentry* last_dentry = nfs_lookup(path, &is_find, &is_root);
    struct nfs_dentry* dentry;
    struct nfs_inode* inode;
    char* fname;

    if (is_find == TRUE) {
        return -NFS_ERROR_EXISTS;
    }

    fname = nfs_get_fname(path);

    /* 确定文件类型 */
    if (S_ISREG(mode)) {
        dentry = new_dentry(fname, NFS_REG_FILE);
    } else if (S_ISDIR(mode)) {
        dentry = new_dentry(fname, NFS_DIR);
    } else {
        dentry = new_dentry(fname, NFS_REG_FILE); // 默认
    }

    dentry->parent = last_dentry;
    inode = nfs_alloc_inode(dentry);
    nfs_alloc_dentry(last_dentry->inode, dentry);

    return NFS_ERROR_NONE;
}

/**
 * @brief 更新时间戳 (Stub)
 */
int nfs_utimens(const char* path, const struct timespec tv[2]) {
    (void)path; 
    return 0;
}

/* --- Optional / Stub Functions --- */

int nfs_write(const char* path, const char* buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    return size; /* Stub: Mock success */
}

int nfs_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    return size; /* Stub: Mock success */
}

int nfs_unlink(const char* path) { return 0; }
int nfs_rmdir(const char* path) { return 0; }
int nfs_rename(const char* from, const char* to) { return 0; }
int nfs_open(const char* path, struct fuse_file_info* fi) { return 0; }
int nfs_opendir(const char* path, struct fuse_file_info* fi) { return 0; }
int nfs_truncate(const char* path, off_t offset) { return 0; }
int nfs_access(const char* path, int type) { return 0; }

/******************************************************************************
* SECTION: FUSE Entry Point
*******************************************************************************/
static struct fuse_operations operations = {
    .init       = nfs_init,
    .destroy    = nfs_destroy,
    .mkdir      = nfs_mkdir,
    .getattr    = nfs_getattr,
    .readdir    = nfs_readdir,
    .mknod      = nfs_mknod,
    .write      = NULL,
    .read       = NULL,
    .utimens    = nfs_utimens,
    .truncate   = NULL,
    .unlink     = NULL,
    .rmdir      = NULL,
    .rename     = NULL,
    .open       = NULL,
    .opendir    = NULL,
    .access     = NULL
};

int main(int argc, char **argv)
{
    int ret;
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    
    // TODO: Update this path to your actual ddriver device file
    nfs_options.device = strdup("/home/oslab/user-land-filesystem/driver/ddriver"); 

    if (fuse_opt_parse(&args, &nfs_options, option_spec, NULL) == -1)
        return -1;

    ret = fuse_main(args.argc, args.argv, &operations, NULL);
    fuse_opt_free_args(&args);
    return ret;
}