/* Host-side stand-in for the FatFs diskio glue types (ff_gen_drv.h / diskio.h). */
#ifndef STUB_FF_GEN_DRV_H
#define STUB_FF_GEN_DRV_H

#include <stdint.h>

typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef unsigned int UINT;
typedef BYTE DSTATUS;
typedef enum { RES_OK = 0, RES_ERROR, RES_WRPRT, RES_NOTRDY, RES_PARERR } DRESULT;

#define STA_NOINIT 0x01
#define CTRL_SYNC 0
#define GET_SECTOR_COUNT 1
#define GET_SECTOR_SIZE 2
#define GET_BLOCK_SIZE 3
#define _USE_WRITE 1
#define _USE_IOCTL 1

typedef struct {
    DSTATUS (*disk_initialize)(BYTE);
    DSTATUS (*disk_status)(BYTE);
    DRESULT (*disk_read)(BYTE, BYTE *, DWORD, UINT);
    DRESULT (*disk_write)(BYTE, const BYTE *, DWORD, UINT);
    DRESULT (*disk_ioctl)(BYTE, BYTE, void *);
} Diskio_drvTypeDef;

#endif
