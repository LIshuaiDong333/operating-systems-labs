#ifndef _TYPES_H_
#define _TYPES_H_

#include <stdint.h>
#include <errno.h>

/******************************************************************************
* SECTION: Basic Types & Constants
*******************************************************************************/
typedef int boolean;
typedef uint16_t flag16;

#define TRUE                    1
#define FALSE                   0
#define UINT32_BITS             32
#define UINT8_BITS              8

/* 文件类型枚举 */
typedef enum nfs_file_type {
    NFS_REG_FILE,       // 普通文件
    NFS_DIR,            // 目录文件
} NFS_FILE_TYPE;

/******************************************************************************
* SECTION: Filesystem Parameters (Magic, Limits, Errors)
*******************************************************************************/
#define NFS_MAGIC_NUM           0x52415453  // Magic Number "STAR" (Little Endian)
#define NFS_SUPER_OFS           0           // 超级块偏移
#define NFS_ROOT_INO            0           // 根目录 Inode 编号

/* 错误码映射 */
#define NFS_ERROR_NONE          0
#define NFS_ERROR_ACCESS        EACCES      // 权限不足
#define NFS_ERROR_SEEK          ESPIPE      // 偏移错误
#define NFS_ERROR_ISDIR         EISDIR      // 是目录
#define NFS_ERROR_NOSPACE       ENOSPC      // 空间不足
#define NFS_ERROR_EXISTS        EEXIST      // 文件已存在
#define NFS_ERROR_NOTFOUND      ENOENT      // 文件未找到
#define NFS_ERROR_UNSUPPORTED   ENXIO       // 不支持的操作
#define NFS_ERROR_IO            EIO         // 输入输出错误
#define NFS_ERROR_INVAL         EINVAL      // 无效参数

/* 限制参数 */
#define NFS_MAX_FILE_NAME       128         // 最大文件名长度
#define NFS_INODE_PER_FILE      1           // 每个文件占用 Inode 数
#define NFS_DATA_PER_FILE       7           // 每个文件直接索引块数量
#define NFS_DEFAULT_PERM        0777        // 默认权限

/* 磁盘布局参数 */
#define NFS_INODE_PER_BLK       8           // 每个逻辑块包含的Inode 数量
#define NFS_SUPER_BLKS          1
#define NFS_INODE_MAP_BLKS      1
#define NFS_DATA_MAP_BLKS       1
#define NFS_INODE_BLKS          64          
#define NFS_DATA_BLKS           4029        

/******************************************************************************
* SECTION: Macro Helpers
*******************************************************************************/
/* 获取全局 Superblock 中的参数 */
#define NFS_IO_SZ()                     (nfs_super.sz_io)      // 物理IO大小 (512B)
#define NFS_BLK_SZ()                    (nfs_super.sz_blks)    // 逻辑块大小 (1KB)
#define NFS_DISK_SZ()                   (nfs_super.sz_disk)    // 磁盘总大小
#define NFS_DRIVER()                    (nfs_super.fd)         // 设备描述符

/* 大小转换与计算 */
#define NFS_BLKS_SZ(blks)               ((blks) * NFS_BLK_SZ())
#define NFS_DENTRY_PER_BLK()            (NFS_BLK_SZ() / sizeof(struct nfs_dentry))

/* 对齐宏 (向下/向上取整) */
#define NFS_ROUND_DOWN(value, round)    ((value) % (round) == 0 ? (value) : ((value) / (round)) * (round))
#define NFS_ROUND_UP(value, round)      ((value) % (round) == 0 ? (value) : ((value) / (round) + 1) * (round))

/* 字符串操作 */
#define NFS_ASSIGN_FNAME(pnfs_dentry, _fname) memcpy(pnfs_dentry->fname, _fname, strlen(_fname))

/* 磁盘偏移计算 */
// Inode区域基地址 + 前面所有Inode的大小
#define NFS_INO_OFS(ino)                (nfs_super.inode_offset + NFS_BLKS_SZ(ino)) 
// Data区域基地址 + 前面所有Data Block的大小
#define NFS_DATA_OFS(dno)               (nfs_super.data_offset + NFS_BLKS_SZ(dno))

/* 类型检查 */
#define NFS_IS_DIR(pinode)              (pinode->dentry->ftype == NFS_DIR)
#define NFS_IS_REG(pinode)              (pinode->dentry->ftype == NFS_REG_FILE)


/******************************************************************************
* SECTION: On-Disk Structures (持久化存储结构)
* 后缀 _d 表示 Disk，直接写入磁盘的格式
*******************************************************************************/

struct nfs_super_d {
    uint32_t    magic;              // 幻数
    int         sz_usage;           // 已使用空间大小
    
    int         max_ino;            // Inode最大数量
    int         map_inode_blks;     // Inode位图占用的块数
    int         map_inode_offset;   // Inode位图在磁盘的字节偏移
    
    int         max_dno;            // Data Block最大数量
    int         map_data_blks;      // Data位图占用的块数
    int         map_data_offset;    // Data位图在磁盘的字节偏移
    
    int         inode_offset;       // Inode区域起始偏移
    int         data_offset;        // Data区域起始偏移
};

struct nfs_inode_d {
    uint32_t        ino;            // Inode 编号
    int             size;           // 文件大小 (字节或块数，取决于实现，此处似乎混用)
    int             link;           // 硬链接数
    int             block_pointer[NFS_DATA_PER_FILE]; // 直接数据块指针
    int             dir_cnt;        // 若为目录，包含的目录项数量
    NFS_FILE_TYPE   ftype;          // 文件类型
    int             block_allocted; // 已分配的数据块数量
};

struct nfs_dentry_d {
    char            fname[NFS_MAX_FILE_NAME]; // 文件名
    int             ino;                      // 指向的 Inode 编号
    NFS_FILE_TYPE   ftype;                    // 文件类型
};


/******************************************************************************
* SECTION: In-Memory Structures (内存数据结构)
* 运行时使用，包含指针和运行时状态
*******************************************************************************/

struct custom_options {
    const char* device;             // 设备路径
};

struct nfs_super {
    uint32_t    magic;
    int         fd;                 // 设备文件描述符
    
    /* 尺寸信息 */
    int         sz_io;              // 物理扇区大小
    int         sz_blks;            // 逻辑块大小
    int         sz_disk;            // 磁盘总大小
    int         sz_usage;           // 当前用量

    /* Inode Map */
    int         max_ino;
    uint8_t* map_inode;          // 内存中的 Inode 位图缓存
    int         map_inode_blks;
    int         map_inode_offset;

    /* Data Map */
    int         max_dno;
    uint8_t* map_data;           // 内存中的 Data 位图缓存
    int         map_data_blks;
    int         map_data_offset;

    /* Offsets */
    int         inode_offset;
    int         data_offset;

    /* Runtime Info */
    boolean            is_mounted;
    struct nfs_dentry* root_dentry; // 根目录项缓存
};

struct nfs_inode {
    uint32_t           ino;
    int                size;
    int                link;
    int                block_pointer[NFS_DATA_PER_FILE];
    int                dir_cnt;
    
    /* 内存特有指针 */
    struct nfs_dentry  *dentry;     // 指向该 Inode 的父 Dentry (反向指针)
    struct nfs_dentry  *dentrys;    // 若为目录，指向其子 Dentry 链表头
    
    NFS_FILE_TYPE      ftype;
    uint8_t* data[NFS_DATA_PER_FILE]; // 数据块内容的内存缓存
    int                block_allocted;
};

struct nfs_dentry {
    char               fname[NFS_MAX_FILE_NAME];
    struct nfs_dentry* parent;      // 父目录 Dentry
    struct nfs_dentry* brother;     // 兄弟 Dentry (链表结构)
    int                ino;         // Inode 编号
    struct nfs_inode* inode;       // 关联的内存 Inode 指针 (Cache)
    NFS_FILE_TYPE      ftype;
};

/**
 * @brief 工厂函数：创建新的内存 Dentry
 */
static inline struct nfs_dentry* new_dentry(char * fname, NFS_FILE_TYPE ftype) {
    struct nfs_dentry * dentry = (struct nfs_dentry *)malloc(sizeof(struct nfs_dentry));
    memset(dentry, 0, sizeof(struct nfs_dentry));
    NFS_ASSIGN_FNAME(dentry, fname);
    dentry->ftype   = ftype;
    dentry->ino     = -1;
    dentry->inode   = NULL;
    dentry->parent  = NULL;
    dentry->brother = NULL;
    return dentry;
}

#endif /* _TYPES_H_ */