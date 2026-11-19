/* Host-side stand-in for FatFs: one in-memory file with fault injection. */
#ifndef STUB_FATFS_H
#define STUB_FATFS_H

#include <stddef.h>

typedef unsigned int UINT;
typedef unsigned char BYTE;
typedef enum { FR_OK = 0, FR_DISK_ERR, FR_INT_ERR, FR_NOT_READY, FR_NO_FILE, FR_NO_PATH, FR_INVALID_NAME, FR_DENIED } FRESULT;
typedef struct { int mounted; } FATFS;
typedef struct { int open; } FIL;

#define FA_WRITE 0x02
#define FA_OPEN_ALWAYS 0x10
#define FA_OPEN_APPEND 0x30

FRESULT f_mount(FATFS *fs, const char *path, BYTE opt);
FRESULT f_open(FIL *fp, const char *path, BYTE mode);
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_sync(FIL *fp);
FRESULT f_close(FIL *fp);
int f_puts(const char *str, FIL *fp);
size_t stub_f_size(void);
#define f_size(fp) stub_f_size()

/* Test hooks (hal_stub.c) */
typedef struct {
    char data[8192];
    size_t size;
    FRESULT mount_result;
    FRESULT open_result;
    FRESULT write_result;
    int short_write;
    int mount_calls;
    int open_files; /* must be 0 between records */
} StubDisk;
extern StubDisk stub_disk;

#endif
