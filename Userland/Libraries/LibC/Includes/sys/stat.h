#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <sys/types.h>

#define S_IFMT    0170000U
#define S_IFSOCK  0140000U
#define S_IFLNK   0120000U
#define S_IFREG   0100000U
#define S_IFBLK   0060000U
#define S_IFDIR   0040000U
#define S_IFCHR   0020000U
#define S_IFIFO   0010000U

#define S_ISUID   04000U
#define S_ISGID   02000U
#define S_ISVTX   01000U

#define S_IRUSR  00400U
#define S_IWUSR  00200U
#define S_IXUSR  00100U
#define S_IRGRP  00040U
#define S_IWGRP  00020U
#define S_IXGRP  00010U
#define S_IROTH  00004U
#define S_IWOTH  00002U
#define S_IXOTH  00001U

#define S_IRWXU  (S_IRUSR | S_IWUSR | S_IXUSR)
#define S_IRWXG  (S_IRGRP | S_IWGRP | S_IXGRP)
#define S_IRWXO  (S_IROTH | S_IWOTH | S_IXOTH)

#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#define S_ISLNK(mode) (((mode) & S_IFMT) == S_IFLNK)
#define S_ISCHR(mode) (((mode) & S_IFMT) == S_IFCHR)
#define S_ISBLK(mode) (((mode) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(mode) (((mode) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(mode) (((mode) & S_IFMT) == S_IFSOCK)

struct stat
{
    dev_t st_dev;
    ino_t st_ino;
    mode_t st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    dev_t st_rdev;
    off_t st_size;
    blksize_t st_blksize;
    blkcnt_t st_blocks;
    time_t st_atime;
    time_t st_mtime;
    time_t st_ctime;
};

int stat(const char* path, struct stat* out_stat);
int mkdir(const char* path, mode_t mode);

#endif
