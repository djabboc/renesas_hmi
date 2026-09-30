/* Use the existing FatFs engine without starting RT-Thread DFS/SD workers.
 * Its disk I/O is owned by test-sd.c and the single peripheral test worker. */
#include "../rt-thread/components/dfs/dfs_v1/filesystems/elmfat/ff.c"
