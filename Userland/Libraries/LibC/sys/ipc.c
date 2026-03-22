#include <errno.h>
#include <stdint.h>
#include <sys/ipc.h>
#include <sys/stat.h>

key_t ftok(const char* path, int proj_id)
{
    if (!path)
    {
        errno = EINVAL;
        return (key_t) -1;
    }

    struct stat st;
    if (stat(path, &st) < 0)
        return (key_t) -1;

    uint32_t ino_low = (uint32_t) st.st_ino & 0xFFFFU;
    uint32_t dev_low = (uint32_t) st.st_dev & 0xFFU;
    uint32_t proj_low = (uint32_t) proj_id & 0xFFU;

    return (key_t) (ino_low | (dev_low << 16) | (proj_low << 24));
}
