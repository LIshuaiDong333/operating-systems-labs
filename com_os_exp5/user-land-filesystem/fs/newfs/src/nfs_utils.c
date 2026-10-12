#include "nfs.h"

extern struct nfs_super      nfs_super;
extern struct custom_options nfs_options;

/******************************************************************************
* SECTION: Helper Functions (Path parsing)
*******************************************************************************/

/**
 * @brief 从完整路径中提取文件名
 * ex: /a/b/c -> c
 */
char* nfs_get_fname(const char* path) {
    char ch = '/';
    // strrchr: 反向搜索最后一个 '/'
    char *q = strrchr(path, ch);
    return (q == NULL) ? (char*)path : (q + 1);
}

/**
 * @brief 计算路径层级深度
 * ex: /av/c/d/f -> lvl = 4
 */
int nfs_calc_lvl(const char * path) {
    const char* str = path;
    int lvl = 0;
    
    if (strcmp(path, "/") == 0) {
        return 0;
    }
    
    while (*str != '\0') {
        if (*str == '/') {
            lvl++;
        }
        str++;
    }
    return lvl;
}

/******************************************************************************
* SECTION: Driver IO Wrappers (Read/Write with Block Alignment)
*******************************************************************************/

/**
 * @brief 驱动读封装
 * 处理不对齐的 offset，读取包含该范围的完整块，然后截取所需数据。
 */
int nfs_driver_read(int offset, uint8_t *out_content, int size) {
    // 对齐到逻辑块边界
    int      offset_aligned = NFS_ROUND_DOWN(offset, NFS_BLK_SZ());
    int      bias           = offset - offset_aligned;
    int      size_aligned   = NFS_ROUND_UP((size + bias), NFS_BLK_SZ());
    
    uint8_t* temp_content   = (uint8_t*)malloc(size_aligned);
    uint8_t* cur            = temp_content;

    // 定位到对齐后的起始位置
    ddriver_seek(NFS_DRIVER(), offset_aligned, SEEK_SET);

    // 循环读取直到读完所有对齐后的块
    int remain = size_aligned;
    while (remain > 0) {
        ddriver_read(NFS_DRIVER(), cur, NFS_IO_SZ());
        cur    += NFS_IO_SZ();
        remain -= NFS_IO_SZ();
    }

    // 拷贝真正需要的数据段
    memcpy(out_content, temp_content + bias, size);
    free(temp_content);
    return NFS_ERROR_NONE;
}

/**
 * @brief 驱动写封装
 * 读-改-写 (Read-Modify-Write) 模式，确保不覆盖块内其他数据。
 */
int nfs_driver_write(int offset, uint8_t *in_content, int size) {
    int      offset_aligned = NFS_ROUND_DOWN(offset, NFS_BLK_SZ());
    int      bias           = offset - offset_aligned;
    int      size_aligned   = NFS_ROUND_UP((size + bias), NFS_BLK_SZ());
    
    uint8_t* temp_content   = (uint8_t*)malloc(size_aligned);
    uint8_t* cur            = temp_content;

    // 1. 读出原有内容
    nfs_driver_read(offset_aligned, temp_content, size_aligned);
    
    // 2. 在内存中修改内容
    memcpy(temp_content + bias, in_content, size);

    // 3. 写回磁盘
    ddriver_seek(NFS_DRIVER(), offset_aligned, SEEK_SET);
    int remain = size_aligned;
    while (remain > 0) {
        ddriver_write(NFS_DRIVER(), cur, NFS_IO_SZ());
        cur    += NFS_IO_SZ();
        remain -= NFS_IO_SZ();
    }

    free(temp_content);
    return NFS_ERROR_NONE;
}

/******************************************************************************
* SECTION: Allocation & Inode Management
*******************************************************************************/

/**
 * @brief 在 Inode 中插入一个新的 Dentry (头插法)
 * 并在必要时分配新的 Data Block 给该目录 Inode
 */
int nfs_alloc_dentry(struct nfs_inode* inode, struct nfs_dentry* dentry) {
    if (inode->dentrys == NULL) {
        inode->dentrys = dentry;
    } else {
        dentry->brother = inode->dentrys;
        inode->dentrys = dentry;
    }
    
    inode->dir_cnt++;
    inode->size += sizeof(struct nfs_dentry);

    // 检查目录文件是否需要新的数据块来存储这个新目录项
    // 如果当前目录项数量模 "每块目录项数" 余 1，说明刚跨过块边界，需要分配新块
    if (inode->dir_cnt % NFS_DENTRY_PER_BLK() == 1) {
        inode->block_pointer[inode->block_allocted] = nfs_alloc_data();
        inode->block_allocted++;
    }
    return inode->dir_cnt;
}

/**
 * @brief 分配数据块位图
 * @return 成功返回 block index，失败返回错误码
 */
int nfs_alloc_data() {
    int byte_cursor = 0;
    int bit_cursor  = 0;
    int dno_cursor  = 0; 
    int is_find     = 0;

    // 遍历位图的所有字节
    for (byte_cursor = 0; byte_cursor < NFS_BLKS_SZ(nfs_super.map_data_blks); byte_cursor++) {
        // 遍历字节中的每一位
        for (bit_cursor = 0; bit_cursor < UINT8_BITS; bit_cursor++) {
            if((nfs_super.map_data[byte_cursor] & (0x1 << bit_cursor)) == 0) {
                // 找到空闲位，置位
                nfs_super.map_data[byte_cursor] |= (0x1 << bit_cursor);
                is_find = 1;
                break;
            }
            dno_cursor++;
        }
        if (is_find) break;
    }

    if (!is_find || dno_cursor >= nfs_super.max_dno) {
        return -NFS_ERROR_NOSPACE;
    }

    return dno_cursor;
}

/**
 * @brief 分配 Inode
 * 1. 查找空闲 Inode 位图
 * 2. 初始化 Inode 结构
 * 3. 若为普通文件，预分配数据块内存缓存
 */
struct nfs_inode* nfs_alloc_inode(struct nfs_dentry * dentry) {
    struct nfs_inode* inode;
    int byte_cursor = 0; 
    int bit_cursor  = 0; 
    int ino_cursor  = 0;
    boolean is_find = FALSE;

    inode = (struct nfs_inode*)malloc(sizeof(struct nfs_inode));

    for (byte_cursor = 0; byte_cursor < NFS_BLKS_SZ(nfs_super.map_inode_blks); byte_cursor++) {
        for (bit_cursor = 0; bit_cursor < UINT8_BITS; bit_cursor++) {
            if((nfs_super.map_inode[byte_cursor] & (0x1 << bit_cursor)) == 0) {    
                nfs_super.map_inode[byte_cursor] |= (0x1 << bit_cursor);
                is_find = TRUE;           
                break;
            }
            ino_cursor++;
        }
        if (is_find) break;
    }

    if (!is_find || ino_cursor >= nfs_super.max_ino) {
        free(inode);
        return (struct nfs_inode*)(long)-NFS_ERROR_NOSPACE;
    }

    inode->ino  = ino_cursor; 
    inode->size = 0;
    inode->dir_cnt = 0;
    inode->block_allocted = 0;
    inode->dentrys = NULL;
    
    dentry->inode = inode;
    dentry->ino   = inode->ino;
    inode->dentry = dentry;
    inode->ftype  = dentry->ftype; // 确保 Inode 知道自己的类型
    
    if (NFS_IS_REG(inode)) {
        for (int i = 0; i < NFS_DATA_PER_FILE; i++) {
            inode->data[i] = (uint8_t *)malloc(NFS_BLK_SZ());
        }
    }

    return inode;
}

/**
 * @brief 将内存 Inode 同步回磁盘 (Sync)
 * 递归写入：Inode本身 -> 目录项数据 -> 子目录项指向的Inode...
 */
int nfs_sync_inode(struct nfs_inode * inode) {
    struct nfs_inode_d  inode_d;
    struct nfs_dentry* dentry_cursor;
    struct nfs_dentry_d dentry_d;
    int offset;
    int ino = inode->ino;

    /* 1. 序列化 Inode 到磁盘结构 */
    inode_d.ino            = ino;
    inode_d.size           = inode->size;
    inode_d.ftype          = inode->dentry->ftype;
    inode_d.dir_cnt        = inode->dir_cnt;
    inode_d.block_allocted = inode->block_allocted;
    for (int i = 0; i < NFS_DATA_PER_FILE; i++) {
        inode_d.block_pointer[i] = inode->block_pointer[i];
    }
    
    if (nfs_driver_write(NFS_INO_OFS(ino), (uint8_t *)&inode_d, sizeof(struct nfs_inode_d)) != NFS_ERROR_NONE) {
        NFS_DBG("[%s] io error writing inode\n", __func__);
        return -NFS_ERROR_IO;
    }

    /* 2. 写回数据块内容 */
    if (NFS_IS_DIR(inode)) { 
        /* 如果是目录，数据块存储的是 Dentry 列表 */
        dentry_cursor = inode->dentrys;
        int data_blk_idx = 0;
        
        // 遍历所有已分配的数据块
        while ((dentry_cursor != NULL) && (data_blk_idx < inode->block_allocted)) {
            offset = NFS_DATA_OFS(inode->block_pointer[data_blk_idx]);
            int blk_end = NFS_DATA_OFS(inode->block_pointer[data_blk_idx] + 1); // 下一块的起始位置

            // 在当前块内填充 Dentry
            while ((dentry_cursor != NULL) && (offset + sizeof(struct nfs_dentry_d) < blk_end)) {
                
                // 序列化 Dentry
                memset(&dentry_d, 0, sizeof(struct nfs_dentry_d));
                memcpy(dentry_d.fname, dentry_cursor->fname, NFS_MAX_FILE_NAME);
                dentry_d.ftype = dentry_cursor->ftype;
                dentry_d.ino   = dentry_cursor->ino;

                if (nfs_driver_write(offset, (uint8_t *)&dentry_d, sizeof(struct nfs_dentry_d)) != NFS_ERROR_NONE) {
                    return -NFS_ERROR_IO;
                }

                // 递归：如果该目录项指向一个有效的 Inode，也同步它
                if (dentry_cursor->inode != NULL) {
                    nfs_sync_inode(dentry_cursor->inode);
                }

                dentry_cursor = dentry_cursor->brother;
                offset += sizeof(struct nfs_dentry_d);
            }
            data_blk_idx++;
        }
    } 
    else if (NFS_IS_REG(inode)) { 
        /* 如果是文件，直接写回数据内存块 */
        for (int i = 0; i < inode->block_allocted; i++) {
            if (nfs_driver_write(NFS_DATA_OFS(inode->block_pointer[i]), inode->data[i], NFS_BLK_SZ()) != NFS_ERROR_NONE) {
                return -NFS_ERROR_IO;
            }
        }
    }

    return NFS_ERROR_NONE;
}

/**
 * @brief 从磁盘读取 Inode 到内存
 */
struct nfs_inode* nfs_read_inode(struct nfs_dentry * dentry, int ino) {
    struct nfs_inode* inode = (struct nfs_inode*)malloc(sizeof(struct nfs_inode));
    struct nfs_inode_d  inode_d;
    struct nfs_dentry* sub_dentry;
    struct nfs_dentry_d dentry_d;
    
    /* 1. 读取 Inode 结构 */
    if (nfs_driver_read(NFS_INO_OFS(ino), (uint8_t *)&inode_d, sizeof(struct nfs_inode_d)) != NFS_ERROR_NONE) {
        free(inode);
        return NULL;                    
    }

    /* 2. 反序列化 */
    inode->dir_cnt        = 0; // 后面读取目录项时会重新统计，或者这里使用 inode_d.dir_cnt
    inode->ino            = inode_d.ino;
    inode->size           = inode_d.size;
    inode->dentry         = dentry;
    inode->dentrys        = NULL;
    inode->block_allocted = inode_d.block_allocted;
    for (int i = 0; i < NFS_DATA_PER_FILE; i++) {
        inode->block_pointer[i] = inode_d.block_pointer[i];
    }

    /* 3. 读取数据内容 */
    if (NFS_IS_DIR(inode)) {
        int dir_cnt_remain = inode_d.dir_cnt;
        int data_blk_idx   = 0;
        int offset;

        while((dir_cnt_remain > 0) && (data_blk_idx < NFS_DATA_PER_FILE)){
            offset = NFS_DATA_OFS(inode->block_pointer[data_blk_idx]);
            int blk_end = NFS_DATA_OFS(inode->block_pointer[data_blk_idx] + 1);

            while((dir_cnt_remain > 0) && (offset + sizeof(struct nfs_dentry_d) < blk_end)){
                if (nfs_driver_read(offset, (uint8_t *)&dentry_d, sizeof(struct nfs_dentry_d)) != NFS_ERROR_NONE){
                    return NULL;  
                }
                
                // 重建内存 Dentry 链表
                sub_dentry = new_dentry(dentry_d.fname, dentry_d.ftype);
                sub_dentry->parent = inode->dentry;
                sub_dentry->ino    = dentry_d.ino; 
                nfs_alloc_dentry(inode, sub_dentry);

                offset += sizeof(struct nfs_dentry_d);
                dir_cnt_remain--;
            }
            data_blk_idx++;
        }
    } 
    else if (NFS_IS_REG(inode)) {
        for (int i = 0; i < NFS_DATA_PER_FILE; i++) {
            inode->data[i] = (uint8_t *)malloc(NFS_BLK_SZ());
            // 注意：这里是否应该只读已分配的块？原代码全部读了
            if (nfs_driver_read(NFS_DATA_OFS(inode->block_pointer[i]), (uint8_t *)inode->data[i], NFS_BLK_SZ()) != NFS_ERROR_NONE) {
                return NULL;                    
            }
        }
    }

    return inode;
}

/**
 * @brief 在内存 Dentry 链表中获取第 N 个目录项
 */
struct nfs_dentry* nfs_get_dentry(struct nfs_inode * inode, int dir_idx) {
    struct nfs_dentry* dentry_cursor = inode->dentrys;
    int cnt = 0;
    while (dentry_cursor) {
        if (dir_idx == cnt) {
            return dentry_cursor;
        }
        cnt++;
        dentry_cursor = dentry_cursor->brother;
    }
    return NULL;
}

/**
 * @brief 路径查找 (Lookup)
 * 解析路径，逐层查找 Dentry。支持缓存机制（若 Inode 为空则读盘）。
 * @return 找到的 Dentry，若未完全匹配则返回最后有效路径的 Dentry
 */
struct nfs_dentry* nfs_lookup(const char * path, boolean* is_find, boolean* is_root) {
    struct nfs_dentry* dentry_cursor = nfs_super.root_dentry;
    struct nfs_dentry* dentry_ret = NULL;
    struct nfs_inode* inode;
    
    int   total_lvl = nfs_calc_lvl(path);
    int   lvl       = 0;
    boolean is_hit;
    char* fname     = NULL;
    char* path_cpy  = (char*)malloc(strlen(path) + 1); // Fix sizeof pointer bug
    
    *is_root = FALSE;
    strcpy(path_cpy, path);

    /* Case: 根目录 */
    if (total_lvl == 0) {                           
        *is_find = TRUE;
        *is_root = TRUE;
        dentry_ret = nfs_super.root_dentry;
        free(path_cpy);
        return dentry_ret;
    }

    fname = strtok(path_cpy, "/");
    while (fname) {   
        lvl++;
        
        /* 缓存未命中：从磁盘读取 Inode */
        if (dentry_cursor->inode == NULL) {           
            nfs_read_inode(dentry_cursor, dentry_cursor->ino);
        }
        inode = dentry_cursor->inode;

        /* 路径中间遇到普通文件，无法继续查找 */
        if (NFS_IS_REG(inode) && lvl < total_lvl) {
            NFS_DBG("[%s] not a dir mid-path\n", __func__);
            dentry_ret = inode->dentry;
            *is_find = FALSE;
            break;
        }

        /* 在当前目录中查找匹配的文件名 */
        if (NFS_IS_DIR(inode)) {
            dentry_cursor = inode->dentrys;
            is_hit        = FALSE;
            
            while (dentry_cursor) {
                if (memcmp(dentry_cursor->fname, fname, strlen(fname) + 1) == 0) { // +1 to compare terminator
                    is_hit = TRUE;
                    break;
                }
                dentry_cursor = dentry_cursor->brother;
            }
            
            /* 未找到 */
            if (!is_hit) {
                *is_find = FALSE;
                dentry_ret = inode->dentry; // 返回父目录
                break;
            }

            /* 找到且是最后一层 */
            if (is_hit && lvl == total_lvl) {
                *is_find = TRUE;
                dentry_ret = dentry_cursor;
                break;
            }
        }
        fname = strtok(NULL, "/");
    }

    /* 确保返回的 Dentry 加载了 Inode */
    if (dentry_ret != NULL && dentry_ret->inode == NULL && dentry_ret->ino != -1) {
        dentry_ret->inode = nfs_read_inode(dentry_ret, dentry_ret->ino);
    }
    
    free(path_cpy);
    return dentry_ret;
}

/******************************************************************************
* SECTION: Mount & Unmount
*******************************************************************************/

/**
 * @brief 挂载文件系统
 * 读取超级块，初始化位图，建立根节点
 */
int nfs_mount(struct custom_options options){
    int                 ret = NFS_ERROR_NONE;
    int                 driver_fd;
    struct nfs_super_d  nfs_super_d; 
    struct nfs_dentry* root_dentry;
    struct nfs_inode* root_inode;
    boolean             is_init = FALSE;

    nfs_super.is_mounted = FALSE;
    driver_fd = ddriver_open(options.device);
    if (driver_fd < 0) return driver_fd;

    /* 1. 初始化基本设备参数 */
    nfs_super.fd = driver_fd;
    ddriver_ioctl(NFS_DRIVER(), IOC_REQ_DEVICE_SIZE,  &nfs_super.sz_disk);
    ddriver_ioctl(NFS_DRIVER(), IOC_REQ_DEVICE_IO_SZ, &nfs_super.sz_io);
    nfs_super.sz_blks = 2 * nfs_super.sz_io; // 1024B

    /* 2. 构建内存根目录 Dentry */
    root_dentry = new_dentry("/", NFS_DIR);

    /* 3. 读取/初始化 Superblock */
    if (nfs_driver_read(NFS_SUPER_OFS, (uint8_t *)(&nfs_super_d), sizeof(struct nfs_super_d)) != NFS_ERROR_NONE) {
        return -NFS_ERROR_IO;
    }   
    
    /* 检查 Magic Number，若不匹配则进行格式化 (MKFS逻辑) */
    if (nfs_super_d.magic != NFS_MAGIC_NUM) {    
        // 设置 Superblock 参数
        nfs_super.max_ino           = NFS_INODE_BLKS;
        nfs_super.max_dno           = NFS_DATA_BLKS;
        
        nfs_super_d.map_inode_blks  = NFS_INODE_MAP_BLKS; 
        nfs_super_d.map_data_blks   = NFS_DATA_MAP_BLKS;
        nfs_super_d.map_inode_offset= NFS_SUPER_OFS + NFS_BLKS_SZ(NFS_SUPER_BLKS);
        nfs_super_d.map_data_offset = nfs_super_d.map_inode_offset + NFS_BLKS_SZ(NFS_INODE_MAP_BLKS);
        nfs_super_d.inode_offset    = nfs_super_d.map_data_offset + NFS_BLKS_SZ(NFS_DATA_MAP_BLKS);
        nfs_super_d.data_offset     = nfs_super_d.inode_offset + NFS_BLKS_SZ(NFS_INODE_BLKS);
        nfs_super_d.sz_usage        = 0;
        nfs_super_d.magic           = NFS_MAGIC_NUM;
        
        is_init = TRUE;
    }

    /* 4. 加载到内存结构 */
    nfs_super.sz_usage          = nfs_super_d.sz_usage;
    nfs_super.map_inode_blks    = nfs_super_d.map_inode_blks;
    nfs_super.map_inode_offset  = nfs_super_d.map_inode_offset;
    nfs_super.inode_offset      = nfs_super_d.inode_offset;
    nfs_super.map_data_blks     = nfs_super_d.map_data_blks;
    nfs_super.map_data_offset   = nfs_super_d.map_data_offset;
    nfs_super.data_offset       = nfs_super_d.data_offset;

    nfs_super.map_inode         = (uint8_t *)malloc(NFS_BLKS_SZ(nfs_super_d.map_inode_blks));
    nfs_super.map_data          = (uint8_t *)malloc(NFS_BLKS_SZ(nfs_super_d.map_data_blks));

    // 读取位图
    if (nfs_driver_read(nfs_super_d.map_inode_offset, (uint8_t *)(nfs_super.map_inode), NFS_BLKS_SZ(nfs_super_d.map_inode_blks)) != NFS_ERROR_NONE) {
        return -NFS_ERROR_IO;
    }
    if (nfs_driver_read(nfs_super_d.map_data_offset, (uint8_t *)(nfs_super.map_data), NFS_BLKS_SZ(nfs_super_d.map_data_blks)) != NFS_ERROR_NONE) {
        return -NFS_ERROR_IO;
    }

    /* 5. 首次初始化处理：创建根 Inode */
    if (is_init) {                                    
        root_inode = nfs_alloc_inode(root_dentry);
        nfs_sync_inode(root_inode);
    }
    
    /* 6. 读取根 Inode */
    root_inode            = nfs_read_inode(root_dentry, NFS_ROOT_INO);
    root_dentry->inode    = root_inode;
    nfs_super.root_dentry = root_dentry;
    nfs_super.is_mounted  = TRUE;

    return ret;
}

/**
 * @brief 卸载文件系统
 * 将内存中的位图、Superblock、Inode 树刷回磁盘
 */
int nfs_umount() {
    struct nfs_super_d  nfs_super_d;

    if (!nfs_super.is_mounted) {
        return NFS_ERROR_NONE;
    }

    /* 1. 递归同步 Inode 树 */
    nfs_sync_inode(nfs_super.root_dentry->inode);

    /* 2. 准备 Superblock 磁盘结构 */
    nfs_super_d.magic               = NFS_MAGIC_NUM;
    nfs_super_d.sz_usage            = nfs_super.sz_usage;
    
    nfs_super_d.map_inode_blks      = nfs_super.map_inode_blks;
    nfs_super_d.map_inode_offset    = nfs_super.map_inode_offset;
    nfs_super_d.inode_offset        = nfs_super.inode_offset;

    nfs_super_d.map_data_blks       = nfs_super.map_data_blks;
    nfs_super_d.map_data_offset     = nfs_super.map_data_offset;
    nfs_super_d.data_offset         = nfs_super.data_offset;

    /* 3. 写入 Superblock */
    if (nfs_driver_write(NFS_SUPER_OFS, (uint8_t *)&nfs_super_d, sizeof(struct nfs_super_d)) != NFS_ERROR_NONE) {
        return -NFS_ERROR_IO;
    }

    /* 4. 写入位图 */
    if (nfs_driver_write(nfs_super_d.map_inode_offset, (uint8_t *)(nfs_super.map_inode), NFS_BLKS_SZ(nfs_super_d.map_inode_blks)) != NFS_ERROR_NONE) {
        return -NFS_ERROR_IO;
    }
    if (nfs_driver_write(nfs_super_d.map_data_offset, (uint8_t *)(nfs_super.map_data), NFS_BLKS_SZ(nfs_super_d.map_data_blks)) != NFS_ERROR_NONE) {
        return -NFS_ERROR_IO;
    }

    /* 5. 清理内存 */
    free(nfs_super.map_inode);
    free(nfs_super.map_data);
    ddriver_close(NFS_DRIVER());

    return NFS_ERROR_NONE;
}