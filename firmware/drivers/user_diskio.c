/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    user_diskio.c
  * @brief   SD-SPI Disk I/O driver for FATFS
  *
  * Build note: STM32CubeMX generates an empty FATFS/Target/user_diskio.c for the
  * "User-defined" FatFs mode. Replace that file with this one (see
  * firmware/README.md).
  *
  * Audit fixes (docs/FIRMWARE_AUDIT.md) - verified against the simulated card in
  * firmware/tests only, NEEDS HARDWARE RE-TEST:
  *  - single-sector read/write always returned RES_ERROR (=> f_mount FR_DISK_ERR)
  *  - multi-sector write sent the 0xFC token but never the data
  *  - multi-sector read released CS after the first block
  *  - the card was polled for "ready" while de-selected, so busy was never seen
  *  - timeouts were ~10x longer than documented (5 s / 10 s per command)
  *  - HAL_SPI_Receive() clocked out the previous buffer contents on MOSI
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include <string.h>
#include <stdlib.h>
#include "ff_gen_drv.h"
#include "main.h" // Includes HAL and peripheral handles

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
#define SD_CS_PORT GPIOB
#define SD_CS_PIN  GPIO_PIN_10

#define SD_CS_LOW()   HAL_GPIO_WritePin(SD_CS_PORT, SD_CS_PIN, GPIO_PIN_RESET)
#define SD_CS_HIGH()  HAL_GPIO_WritePin(SD_CS_PORT, SD_CS_PIN, GPIO_PIN_SET)

// SD Card Commands
#define CMD0   (0x40 + 0)  // GO_IDLE_STATE
#define CMD8   (0x40 + 8)  // SEND_IF_COND
#define CMD17  (0x40 + 17) // READ_SINGLE_BLOCK
#define CMD24  (0x40 + 24) // WRITE_BLOCK
#define CMD55  (0x40 + 55) // APP_CMD
#define CMD41  (0x40 + 41) // SEND_OP_COND
// --- SD Card Command Constants (Additions) ---
#define CMD9   (0x40 + 9)  // SEND_CSD
#define CMD10  (0x40 + 10) // SEND_CID
#define CMD12  (0x40 + 12) // STOP_TRANSMISSION  <-- MISSING (Needed for CMD18 stop)
#define CMD13  (0x40 + 13) // SEND_STATUS
#define CMD18  (0x40 + 18) // READ_MULTIPLE_BLOCK <-- MISSING
#define CMD25  (0x40 + 25) // WRITE_MULTIPLE_BLOCK <-- MISSING
#define CMD58  (0x40 + 58) // READ_OCR <-- MISSING
// ---------------------------------------------
// Data Tokens
#define DATA_START_BLOCK        0xFE
#define WRITE_START_BLOCK       0xFE
#define WRITE_MULTIPLE_BLOCK    0xFC
#define STOP_TRAN_TOKEN         0xFD

// Timeouts (HAL tick = 1 ms)
#define SD_READY_TIMEOUT_MS     500U   // card busy after a write / before a command
#define SD_TOKEN_TIMEOUT_MS     200U   // wait for the data start token on a read

/* Private variables ---------------------------------------------------------*/
extern SPI_HandleTypeDef hspi1; // Your SPI handle

static volatile DSTATUS Stat = STA_NOINIT;
static BYTE CardType;  // Type of SD card (SDv1/SDv2/MMC)

/* Private function prototypes -----------------------------------------------*/
static void spi_xmit_byte(BYTE data);
static BYTE spi_rcvr_byte(void);
static BYTE spi_wait_ready(void);
static void sd_deselect(void);
static int sd_select(void);
static BYTE sd_send_cmd(BYTE cmd, DWORD arg);
static DRESULT sd_rcvr_datablock(BYTE *buff, UINT btr);
static DRESULT sd_xmit_datablock(const BYTE *buff, BYTE token);

/*-----------------------------------------------------------------------*/
/* Low-Level SPI Transfer Functions                                      */
/*-----------------------------------------------------------------------*/

/**
  * @brief Transmits a single byte over SPI1.
  */
static void spi_xmit_byte(BYTE data)
{
    HAL_SPI_Transmit(&hspi1, &data, 1, 100);
}

/**
  * @brief Receives a single byte over SPI1 (sends dummy byte).
  */
static BYTE spi_rcvr_byte(void)
{
    BYTE data;
    BYTE dummy = 0xFF;
    HAL_SPI_TransmitReceive(&hspi1, &dummy, &data, 1, 100);
    return data;
}

/**
  * @brief Waits for the card to transition from busy to ready.
  * @note  Only meaningful while the card is selected (CS low): a de-selected
  *        card releases MISO and the pull-up reads back 0xFF immediately.
  */
static BYTE spi_wait_ready(void)
{
    BYTE d;
    uint32_t start = HAL_GetTick();

    do {
        d = spi_rcvr_byte();
        if (d == 0xFF) return d;
    } while ((HAL_GetTick() - start) < SD_READY_TIMEOUT_MS);
    return d;
}

/**
  * @brief Releases the card and clocks one idle byte so it lets go of MISO.
  */
static void sd_deselect(void)
{
    SD_CS_HIGH();
    spi_rcvr_byte();
}

/**
  * @brief Selects the card and waits until it is not busy.
  * @retval 1 = ready, 0 = timeout (card is de-selected again).
  */
static int sd_select(void)
{
    SD_CS_LOW();
    spi_rcvr_byte();
    if (spi_wait_ready() == 0xFF) return 1;
    sd_deselect();
    return 0;
}

/**
  * @brief Sends a command to the SD card.
  * @param cmd: Command byte.
  * @param arg: 32-bit command argument.
  * @retval R1 response (0xFF = no response / card not ready).
  * @note  The card is left selected so that the caller can read the rest of the
  *        response or the data phase; callers finish with sd_deselect().
  */
static BYTE sd_send_cmd(BYTE cmd, DWORD arg)
{
    BYTE n, res;

    // Start a new transaction and wait for the card (except to stop a read)
    if (cmd != CMD12) {
        sd_deselect();
        if (!sd_select()) return 0xFF;
    }

    // Send command packet
    spi_xmit_byte(cmd);
    spi_xmit_byte((BYTE)(arg >> 24));
    spi_xmit_byte((BYTE)(arg >> 16));
    spi_xmit_byte((BYTE)(arg >> 8));
    spi_xmit_byte((BYTE)arg);

    // CRC (only mandatory for CMD0 and CMD8)
    n = 0x01;
    if (cmd == CMD0) n = 0x95;
    if (cmd == CMD8) n = 0x87;
    spi_xmit_byte(n);

    if (cmd == CMD12) spi_rcvr_byte(); // Discard the stuff byte after STOP_TRANSMISSION

    // Receive command response (R1 is single byte, wait up to 10 trials)
    for (n = 10; n; n--) {
        res = spi_rcvr_byte();
        if (!(res & 0x80)) break;
    }

    return res;
}

/**
  * @brief Receives a data block from the SD card.
  * @param buff: Pointer to the buffer to store data.
  * @param btr: Number of bytes to receive (should be 512).
  * @retval DRESULT: Operation result.
  * @note  CS is left asserted: a multi-block read continues with the next block.
  */
static DRESULT sd_rcvr_datablock(BYTE *buff, UINT btr)
{
    BYTE token;
    uint32_t start = HAL_GetTick();

    do {                            // Wait for data start token
        token = spi_rcvr_byte();
    } while ((token == 0xFF) && ((HAL_GetTick() - start) < SD_TOKEN_TIMEOUT_MS));

    if(token != DATA_START_BLOCK) return RES_ERROR;

    // Receive data packet (512 bytes). In 2-line master mode HAL_SPI_Receive()
    // transmits the buffer contents, and the card must see MOSI high.
    memset(buff, 0xFF, btr);
    if (HAL_SPI_Receive(&hspi1, buff, btr, HAL_MAX_DELAY) != HAL_OK) return RES_ERROR;

    spi_rcvr_byte();                // Discard CRC
    spi_rcvr_byte();

    return RES_OK;
}

/**
  * @brief Sends a data block to the SD card.
  * @param buff: Pointer to the buffer containing data (unused for the stop token).
  * @param token: WRITE_START_BLOCK, WRITE_MULTIPLE_BLOCK or STOP_TRAN_TOKEN.
  * @retval DRESULT: Operation result.
  */
static DRESULT sd_xmit_datablock(const BYTE *buff, BYTE token)
{
    BYTE resp;

    if (spi_wait_ready() != 0xFF) return RES_ERROR;

    spi_xmit_byte(token); // Send token

    if (token != STOP_TRAN_TOKEN) {
        // Send data packet (512 bytes)
        if (HAL_SPI_Transmit(&hspi1, (uint8_t*)buff, 512, HAL_MAX_DELAY) != HAL_OK) return RES_ERROR;

        spi_xmit_byte(0xFF); // Dummy CRC
        spi_xmit_byte(0xFF);

        resp = spi_rcvr_byte(); // Get data response
        if ((resp & 0x1F) != 0x05) return RES_ERROR; // Check for acceptance
    }

    return RES_OK;
}


/*-----------------------------------------------------------------------*/
/* Disk I/O Functions (FATFS Interface)                                  */
/*-----------------------------------------------------------------------*/

/**
  * @brief  Initializes a Drive
  * @param  pdrv: Physical drive number (0..)
  * @retval DSTATUS: Operation status
  */
DSTATUS USER_initialize (
	BYTE pdrv           /* Physical drive nmuber to identify the drive */
)
{
  /* USER CODE BEGIN INIT */
    BYTE n, ty, ocr[4];
    DWORD tmr;

    if (pdrv) return STA_NOINIT; // Only support drive 0

    // ************************************************************
    // 1. LOW SPEED INIT: Force SPI clock to the minimum speed (/256)
    // ************************************************************
    // Clear the Baud Rate Control bits (BR[2:0]) and set Prescaler /256.
    hspi1.Instance->CR1 &= (~SPI_CR1_BR_Msk);
    hspi1.Instance->CR1 |= SPI_BAUDRATEPRESCALER_256;


    // 2. Initial power-up sequence
    SD_CS_HIGH();
    // Send 80 dummy clock pulses (10 bytes of 0xFF)
    for (n = 10; n; n--) spi_xmit_byte(0xFF);

    // 3. CMD0: GO_IDLE_STATE
    ty = 0;
    if (sd_send_cmd(CMD0, 0) == 1) { // R1 response should be 0x01 (idle state)
        // 4. CMD8: SEND_IF_COND (for SDv2)
        if (sd_send_cmd(CMD8, 0x1AA) == 1) {
            // SDv2/SDHC/SDXC
            for (n = 0; n < 4; n++) ocr[n] = spi_rcvr_byte();
            if ((ocr[2] == 0x01) && (ocr[3] == 0xAA)) {
                // Wait for READY state (ACMD41 with HCS set)
                for (tmr = 1000; tmr; tmr--) {
                    if (sd_send_cmd(CMD55, 0) <= 1 && sd_send_cmd(CMD41, 1UL << 30) == 0) break;
                    HAL_Delay(1);
                }
                if (tmr && sd_send_cmd(CMD58, 0) == 0) { // Check CCS bit
                    for (n = 0; n < 4; n++) ocr[n] = spi_rcvr_byte();
                    ty = (ocr[0] & 0x40) ? 6 : 2; // SDHC/SDXC : SDv2
                }
            }
        } else {
            // SDv1 or MMC (simplified check)
            // TODO(verify on hardware): a real SDv1 card needs ACMD41 repeated
            // until it leaves idle, MMC needs CMD1, and both need CMD16 (512).
            // Only SDv2/SDHC cards are expected to work.
            if (sd_send_cmd(CMD55, 0) <= 1 && sd_send_cmd(CMD41, 0) == 0) ty = 1; // SDv1
            else ty = 3; // MMCv3
        }
    }

    sd_deselect();

    CardType = ty;
    Stat = STA_NOINIT; // A failed re-initialisation must not leave the old state
    if (ty) {
        Stat &= ~STA_NOINIT; // Clear NOINIT flag on success

        // ************************************************************
        // 5. HIGH SPEED SWITCH: Switch to fast SPI clock (Prescaler /2)
        // ************************************************************
        // Restore the high-speed setting from MX_SPI1_Init for data transfer.
        // PCLK2/2 = 8 MHz with the 16 MHz HSI clock of the .ioc. If the system
        // clock is ever raised to 84 MHz this becomes 42 MHz - above the 25 MHz
        // default-speed limit of SD cards - and the prescaler must be increased.
        hspi1.Instance->CR1 &= (~SPI_CR1_BR_Msk); // Clear Baud Rate bits
        hspi1.Instance->CR1 |= SPI_BAUDRATEPRESCALER_2; // Set Prescaler to /2
    }

    return Stat;
  /* USER CODE END INIT */
}
/**
  * @brief  Gets Disk Status
  * @param  pdrv: Physical drive number (0..)
  * @retval DSTATUS: Operation status
  */
DSTATUS USER_status (
	BYTE pdrv       /* Physical drive number to identify the drive */
)
{
  /* USER CODE BEGIN STATUS */
    if (pdrv) return STA_NOINIT;
    return Stat;
  /* USER CODE END STATUS */
}

/**
  * @brief  Reads Sector(s)
  * @param  pdrv: Physical drive number (0..)
  * @param  *buff: Data buffer to store read data
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to read (1..128)
  * @retval DRESULT: Operation result
  */
DRESULT USER_read (
	BYTE pdrv,      /* Physical drive nmuber to identify the drive */
	BYTE *buff,     /* Data buffer to store read data */
	DWORD sector,   /* Sector address in LBA */
	UINT count      /* Number of sectors to read */
)
{
  /* USER CODE BEGIN READ */
    if (pdrv || (Stat & STA_NOINIT)) return RES_NOTRDY;
    if (!count) return RES_PARERR;
    if (!(CardType & 4)) sector *= 512; // Convert LBA to byte address for SDv1/MMC

    // Read multiple sectors
    if (count > 1) {
        if (sd_send_cmd(CMD18, sector) == 0) {
            do {
                if (sd_rcvr_datablock(buff, 512) != RES_OK) break;
                buff += 512;
            } while (--count);
            sd_send_cmd(CMD12, 0); // Stop transmission
        }
    }
    // Read single sector
    else {
        if (sd_send_cmd(CMD17, sector) == 0 && sd_rcvr_datablock(buff, 512) == RES_OK) {
            count = 0;
        }
    }

    sd_deselect();

    return count ? RES_ERROR : RES_OK;
  /* USER CODE END READ */
}

/**
  * @brief  Writes Sector(s)
  * @param  pdrv: Physical drive number (0..)
  * @param  *buff: Data to be written
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to write (1..128)
  * @retval DRESULT: Operation result
  */
#if _USE_WRITE == 1
DRESULT USER_write (
	BYTE pdrv,          /* Physical drive nmuber to identify the drive */
	const BYTE *buff,   /* Data to be written */
	DWORD sector,       /* Sector address in LBA */
	UINT count          /* Number of sectors to write */
)
{
  /* USER CODE BEGIN WRITE */
    if (pdrv || (Stat & STA_NOINIT)) return RES_NOTRDY;
    if (!count) return RES_PARERR;
    if (!(CardType & 4)) sector *= 512; // Convert LBA to byte address

    // Write multiple sectors
    if (count > 1) {
        if (sd_send_cmd(CMD25, sector) == 0) {
            do {
                if (sd_xmit_datablock(buff, WRITE_MULTIPLE_BLOCK) != RES_OK) break;
                buff += 512;
            } while (--count);
            if (sd_xmit_datablock(0, STOP_TRAN_TOKEN) != RES_OK) count = 1; // Stop token
        }
    }
    // Write single sector
    else {
        if (sd_send_cmd(CMD24, sector) == 0 && sd_xmit_datablock(buff, WRITE_START_BLOCK) == RES_OK) {
            count = 0;
        }
    }

    // The card is still programming here; the next sd_select() waits for it.
    sd_deselect();

    return count ? RES_ERROR : RES_OK;
  /* USER CODE END WRITE */
}
#endif /* _USE_WRITE == 1 */

/**
  * @brief  I/O control operation
  * @param  pdrv: Physical drive number (0..)
  * @param  cmd: Control code
  * @param  *buff: Buffer to send/receive control data
  * @retval DRESULT: Operation result
  */
#if _USE_IOCTL == 1
DRESULT USER_ioctl (
	BYTE pdrv,      /* Physical drive nmuber (0..) */
	BYTE cmd,       /* Control code */
	void *buff      /* Buffer to send/receive control data */
)
{
  /* USER CODE BEGIN IOCTL */
    // Simplified IOCTL implementation
    if (pdrv || (Stat & STA_NOINIT)) return RES_NOTRDY;

    DRESULT res = RES_ERROR;

    switch (cmd) {
        case CTRL_SYNC: // Make sure that data has been written to the card
            if (sd_select()) res = RES_OK; // select + wait until not busy
            break;
        case GET_SECTOR_SIZE: // Returns 512
            *(WORD*)buff = 512;
            res = RES_OK;
            break;
        case GET_BLOCK_SIZE: // Returns 1 (single block operation)
            *(DWORD*)buff = 1;
            res = RES_OK;
            break;
        default:
            res = RES_PARERR;
    }

    sd_deselect();

    return res;
  /* USER CODE END IOCTL */
}
#endif /* _USE_IOCTL == 1 */

/* Private function prototypes -----------------------------------------------*/
// ... (Your standard function prototypes remain here)

Diskio_drvTypeDef  USER_Driver =
{
  USER_initialize,
  USER_status,
  USER_read,
#if  _USE_WRITE
  USER_write,
#endif  /* _USE_WRITE == 1 */
#if  _USE_IOCTL == 1
  USER_ioctl,
#endif /* _USE_IOCTL == 1 */
};
