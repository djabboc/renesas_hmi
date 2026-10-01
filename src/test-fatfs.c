/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-fatfs.c
 * @brief 引入工程已有 FatFs 引擎，不启动额外 DFS/SD 后台线程。
 * 磁盘 I/O 由 test-sd.c 提供，ptest 线程串行独占。
 * Studio 与 SCons 均只编译这个适配单元，不能再单独编译同一份 ff.c。
 */
#include "../rt-thread/components/dfs/dfs_v1/filesystems/elmfat/ff.c"
