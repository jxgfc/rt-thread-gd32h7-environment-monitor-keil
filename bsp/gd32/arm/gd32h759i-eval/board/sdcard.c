/*!
    \file    sdcard.c
    \brief   SD card driver 
    
    \version 2025-02-19, V2.1.0, demo for GD32H7xx
*/

/*
    Copyright (c) 2024, GigaDevice Semiconductor Inc.

    Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice, this
       list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright notice,
       this list of conditions and the following disclaimer in the documentation
       and/or other materials provided with the distribution.
    3. Neither the name of the copyright holder nor the names of its contributors
       may be used to endorse or promote products derived from this software without
       specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
OF SUCH DAMAGE.
*/

#include "sdcard.h"
#include "gd32h7xx_sdio.h"
#include <stddef.h>
#include <rtthread.h>

/* card status of R1 definitions */
#define SD_R1_OUT_OF_RANGE                  BIT(31)                   /* command's argument was out of the allowed range */
#define SD_R1_ADDRESS_ERROR                 BIT(30)                   /* misaligned address which did not match the block length */
#define SD_R1_BLOCK_LEN_ERROR               BIT(29)                   /* transferred block length is not allowed */
#define SD_R1_ERASE_SEQ_ERROR               BIT(28)                   /* an error in the sequence of erase commands occurred */
#define SD_R1_ERASE_PARAM                   BIT(27)                   /* an invalid selection of write-blocks for erase occurred */
#define SD_R1_WP_VIOLATION                  BIT(26)                   /* the host attempts to write to a protected block or to the temporary or permanent write protected card */
#define SD_R1_CARD_IS_LOCKED                BIT(25)                   /* the card is locked by the host */
#define SD_R1_LOCK_UNLOCK_FAILED            BIT(24)                   /* a sequence or password error has been detected in lock/unlock card command */
#define SD_R1_COM_CRC_ERROR                 BIT(23)                   /* CRC check of the previous command failed */
#define SD_R1_ILLEGAL_COMMAND               BIT(22)                   /* command not legal for the card state */
#define SD_R1_CARD_ECC_FAILED               BIT(21)                   /* card internal ECC was applied but failed to correct the data */
#define SD_R1_CC_ERROR                      BIT(20)                   /* internal card controller error */
#define SD_R1_GENERAL_UNKNOWN_ERROR         BIT(19)                   /* a general or an unknown error occurred during the operation */
#define SD_R1_CSD_OVERWRITE                 BIT(16)                   /* read only section of the CSD does not match or attempt to reverse the copy or permanent WP bits */
#define SD_R1_WP_ERASE_SKIP                 BIT(15)                   /* partial address space was erased */
#define SD_R1_CARD_ECC_DISABLED             BIT(14)                   /* command has been executed without using the internal ECC */
#define SD_R1_ERASE_RESET                   BIT(13)                   /* an erase sequence was cleared before executing */
#define SD_R1_READY_FOR_DATA                BIT(8)                    /* correspond to buffer empty signaling on the bus */
#define SD_R1_APP_CMD                       BIT(5)                    /* card will expect ACMD */
#define SD_R1_AKE_SEQ_ERROR                 BIT(3)                    /* error in the sequence of the authentication process */
#define SD_R1_ERROR_BITS                    ((uint32_t)0xFDF9E008U)   /* all the R1 error bits */

/* card status of R6 definitions */
#define SD_R6_COM_CRC_ERROR                 BIT(15)                   /* CRC check of the previous command failed */
#define SD_R6_ILLEGAL_COMMAND               BIT(14)                   /* command not legal for the card state */
#define SD_R6_GENERAL_UNKNOWN_ERROR         BIT(13)                   /* a general or an unknown error occurred during the operation */

/* card state */
#define SD_CARDSTATE_IDLE                   ((uint8_t)0x00)           /* card is in idle state */
#define SD_CARDSTATE_READY                  ((uint8_t)0x01)           /* card is in ready state */
#define SD_CARDSTATE_IDENTIFICAT            ((uint8_t)0x02)           /* card is in identificat state */
#define SD_CARDSTATE_STANDBY                ((uint8_t)0x03)           /* card is in standby state */
#define SD_CARDSTATE_TRANSFER               ((uint8_t)0x04)           /* card is in transfer state */
#define SD_CARDSTATE_DATA                   ((uint8_t)0x05)           /* card is in data sending state */
#define SD_CARDSTATE_RECEIVING              ((uint8_t)0x06)           /* card is in receiving state */
#define SD_CARDSTATE_PROGRAMMING            ((uint8_t)0x07)           /* card is in programming state */
#define SD_CARDSTATE_DISCONNECT             ((uint8_t)0x08)           /* card is in disconnect state */
#define SD_CARDSTATE_LOCKED                 ((uint32_t)0x02000000U)   /* card is in locked state */

#define SD_CHECK_PATTERN                    ((uint32_t)0x000001AAU)    /* check pattern for CMD8 */
#define SD_VOLTAGE_WINDOW                   ((uint32_t)0x80100000U)    /* host 3.3V request in ACMD41 */
#define SD_VOLTAGE_18V                      ((uint32_t)0x01000000U)    /* host 1.8V request in ACMD41 */

/* parameters for ACMD41(voltage validation) */
#define SD_HIGH_CAPACITY                    ((uint32_t)0x40000000U)    /* high capacity SD memory card */
#define SD_STD_CAPACITY                     ((uint32_t)0x00000000U)    /* standard capacity SD memory card */

/* SD bus width, check SCR register */
#define SD_BUS_WIDTH_4BIT                   ((uint32_t)0x00040000U)    /* 4-bit width bus mode */
#define SD_BUS_WIDTH_1BIT                   ((uint32_t)0x00010000U)    /* 1-bit width bus mode */

/* masks for SCR register */
#define SD_MASK_0_7BITS                     ((uint32_t)0x000000FFU)    /* mask [7:0] bits */
#define SD_MASK_8_15BITS                    ((uint32_t)0x0000FF00U)    /* mask [15:8] bits */
#define SD_MASK_16_23BITS                   ((uint32_t)0x00FF0000U)    /* mask [23:16] bits */
#define SD_MASK_24_31BITS                   ((uint32_t)0xFF000000U)    /* mask [31:24] bits */

#define SDIO_FIFO_ADDR                      SDIO_FIFO(SDIO)            /* address of SDIO_FIFO */
#define SD_FIFOHALF_WORDS                   ((uint32_t)0x00000008U)    /* words of FIFO half full/empty */
#define SD_FIFOHALF_BYTES                   ((uint32_t)0x00000020U)    /* bytes of FIFO half full/empty */

#define SD_DATATIMEOUT                      ((uint32_t)400000U)       /* one second at 400 kHz */
#define SD_MAX_VOLT_VALIDATION              ((uint32_t)1000U)
#define SD_MAX_DATA_LENGTH                  ((uint32_t)0x01FFFFFFU)    /* the maximum length of data */
#define SD_ALLZERO                          ((uint32_t)0x00000000U)    /* all zero */
#define SD_RCA_SHIFT                        ((uint8_t)0x10U)           /* RCA shift bits */

/* user can according to need to change the macro values */
#define SD_CLK_DIV_INIT                     ((uint32_t)0x01F4)        /* SD clock division in initilization phase */
#define SD_CLK_DIV_TRANS_DSPEED             ((uint32_t)250U)          /* PLL1R 200 MHz / (2 * 250) = 400 kHz */
#define SD_CLK_DIV_TRANS_HSPEED             ((uint32_t)0x0004)        /* SD clock division in high speed transmission phase */
#define SD_CLK_DIV_TRANS_SDR25SPEED         ((uint32_t)0x0004)        /* SD clock division in SDR25 high speed transmission phase */
#define SD_CLK_DIV_TRANS_SDR50SPEED         ((uint32_t)0x0002)        /* SD clock division in SDR50 high speed transmission phase */
#define SD_CLK_DIV_TRANS_SDR104SPEED        ((uint32_t)0x0001)        /* SD clock division in SDR104 high speed transmission phase */
#define SD_CLK_DIV_TRANS_DDR50SPEED         ((uint32_t)0x0004)        /* SD clock division in DDR50 high speed transmission phase */

#define SDIO_MASK_INTC_FLAGS                ((uint32_t)0x1FE00FFF)    /* mask flags of SDIO_INTC */
#define SDIO_MASK_CMD_FLAGS                 ((uint32_t)0x002000C5)    /* mask flags of CMD FLAGS */
#define SDIO_MASK_DATA_FLAGS                ((uint32_t)0x18000F3A)    /* mask flags of DATA FLAGS */
#define SD_CMD_WAIT_LIMIT                   ((uint32_t)100000U)
#define SD_CMD_TIMEOUT_MS                   ((uint32_t)50U)
#define SD_DATA_WAIT_LIMIT                  ((uint32_t)32000000U)
#define SD_DATA_TIMEOUT_MS                  1500
#define SD_BUSY_TIMEOUT_MS                  2000

uint32_t sd_scr[2] = {0, 0};                                          /* content of SCR register */

static sdio_card_type_enum cardtype = SDIO_STD_CAPACITY_SD_CARD_V1_1; /* SD card type */
static uint32_t sdcardtype = SD_STD_CAPACITY;                         /* SD card capacity level, for ACMD41 parameter */
uint32_t cardcapacity = SD_SDSC;                                      /* SD card capacity type */

static uint32_t sd_csd[4] = {0, 0, 0, 0};                             /* content of CSD register */
static uint32_t sd_cid[4] = {0, 0, 0, 0};                             /* content of CID register */
static uint16_t sd_rca = 0U;                                          /* RCA of SD card */
static uint32_t transmode = SD_POLLING_MODE;
static uint32_t totalnumber_bytes = 0U, stopcondition = 0U;

static __IO sd_error_enum transerror = SD_OK;
static __IO uint32_t transend = 0U, number_bytes = 0U;

/* The original demo only bounded command waits by a CPU-loop counter.  That
 * counter is not a time bound across clock configurations and can monopolize
 * the envlog thread when SDIO is disconnected.  Keep the loop guard as a
 * secondary protection, but also stop after a real RT-Thread tick deadline. */
static int sd_cmd_timeout_expired(rt_tick_t started)
{
    return (rt_tick_t)(rt_tick_get() - started) >=
           rt_tick_from_millisecond(SD_CMD_TIMEOUT_MS);
}

/* check if the command sent error occurs */
static sd_error_enum cmdsent_error_check(void);
/* check if error occurs for R1 response */
static sd_error_enum r1_error_check(uint8_t cmdindex);
/* check if error type for R1 response */
static sd_error_enum r1_error_type_check(uint32_t resp);
/* check if error occurs for R2 response */
static sd_error_enum r2_error_check(void);
/* check if error occurs for R3 response */
static sd_error_enum r3_error_check(void);
/* check if error occurs for R6 response */
static sd_error_enum r6_error_check(uint8_t cmdindex, uint16_t *prca);
/* check if error occurs for R7 response */
static sd_error_enum r7_error_check(void);

/* get the state which the card is in */
static sd_error_enum sd_card_state_get(uint8_t *pcardstate);
/* configure the bus width mode */
static sd_error_enum sd_bus_width_config(uint32_t buswidth);
/* get the SCR of corresponding card */
static sd_error_enum sd_scr_get(uint16_t rca, uint32_t *pscr);
/* get the data block size */
static uint32_t sd_datablocksize_get(uint16_t bytesnumber);

/* configure the GPIO of SDIO interface */
static void gpio_config(void);
/* configure the RCU of SDIO */
static sd_error_enum rcu_config(void);
/* This BSP uses bounded polling: the LCD and sensors may preempt SD I/O. */
static void sd_data_cleanup(void)
{
    sdio_trans_start_disable(SDIO);
    sdio_dsm_disable(SDIO);
    sdio_idma_disable(SDIO);
    sdio_fifo_reset_enable(SDIO);
    sdio_fifo_reset_disable(SDIO);
    sdio_flag_clear(SDIO, SDIO_MASK_DATA_FLAGS);
}

static sd_error_enum sd_wait_transfer_ready(void)
{
    rt_tick_t started = rt_tick_get();
    uint32_t response = 0U;
    uint32_t attempts = SD_BUSY_TIMEOUT_MS;
    sd_error_enum status;

    do {
        status = sd_cardstatus_get(&response);
        if(SD_OK != status)
            return status;
        if(response & SD_CARDSTATE_LOCKED)
            return SD_LOCK_UNLOCK_FAILED;
        if((response & SD_R1_READY_FOR_DATA) &&
           (((response >> 9U) & 0x0FU) == SD_CARDSTATE_TRANSFER))
            return SD_OK;
        if((rt_tick_t)(rt_tick_get() - started) >=
           rt_tick_from_millisecond(SD_BUSY_TIMEOUT_MS))
            break;
        rt_thread_mdelay(1);
    } while(--attempts != 0U);
    return SD_DATA_TIMEOUT;
}

/* RFE alone is not a receive-ready indication while the DSM is idle.
 * DTEND may leave a final partial FIFO; drain only the requested words. */
static sd_error_enum sd_poll_data(uint32_t *buffer, uint32_t bytes, uint32_t direction)
{
    rt_tick_t started = rt_tick_get();
    uint32_t remaining = SD_DATA_WAIT_LIMIT;
    uint32_t words = bytes / sizeof(uint32_t);
    uint32_t done = 0U;
    uint32_t flags = 0U;
    uint32_t count;
    sd_error_enum status = SD_DATA_TIMEOUT;

    while(remaining-- != 0U) {
        flags = SDIO_STAT(SDIO);
        if(flags & (SDIO_FLAG_DTCRCERR | SDIO_FLAG_DTTMOUT |
                    SDIO_FLAG_RXORE | SDIO_FLAG_TXURE)) {
            if(flags & SDIO_FLAG_DTCRCERR)
                status = SD_DATA_CRC_ERROR;
            else if(flags & SDIO_FLAG_DTTMOUT)
                status = SD_DATA_TIMEOUT;
            else if(flags & SDIO_FLAG_RXORE)
                status = SD_RX_OVERRUN_ERROR;
            else
                status = SD_TX_UNDERRUN_ERROR;
            break;
        }

        if(direction == SDIO_TRANSDIRECTION_TOSDIO) {
            if((done < words) && !(flags & SDIO_FLAG_RFE) &&
               (flags & (SDIO_FLAG_DATSTA | SDIO_FLAG_DTEND))) {
                buffer[done++] = sdio_data_read(SDIO);
                continue;
            }
        } else if((done < words) && !(flags & SDIO_FLAG_DTEND) &&
                  (flags & SDIO_FLAG_TFH)) {
            count = words - done;
            if(count > SD_FIFOHALF_WORDS)
                count = SD_FIFOHALF_WORDS;
            while(count-- != 0U)
                sdio_data_write(SDIO, buffer[done++]);
        }

        if(flags & SDIO_FLAG_DTEND) {
            if(done == words)
                return SD_OK;
            break;
        }
        if((rt_tick_t)(rt_tick_get() - started) >=
           rt_tick_from_millisecond(SD_DATA_TIMEOUT_MS))
            break;
    }
    /* Capture the failure before sd_data_cleanup clears the hardware flags. */
    rt_kprintf("sdio: data error=%d, STAT=0x%08lx, words=%lu/%lu, left=%lu\n",
               status, (unsigned long)flags, (unsigned long)done,
               (unsigned long)words,
               (unsigned long)sdio_data_counter_get(SDIO));
    return status;
}

/* Addresses are sector numbers for both SDHC and SDSC.
 * The DFS adapter supplies an aligned 512-byte bounce buffer. */
static sd_error_enum sd_poll_block(uint32_t *buffer, uint32_t sector,
                                  uint16_t blocksize, uint32_t direction)
{
    sd_error_enum status;
    uint8_t command;

    if(buffer == NULL || ((uint32_t)buffer & 3U) != 0U ||
       blocksize != 512U || transmode != SD_POLLING_MODE)
        return SD_PARAMETER_INVALID;

    sd_data_cleanup();
    status = sd_wait_transfer_ready();
    if(status != SD_OK)
        return status;
    if(cardtype != SDIO_HIGH_CAPACITY_SD_CARD) {
        if(sector > (0xFFFFFFFFU / 512U))
            return SD_OUT_OF_RANGE;
        sector *= 512U;
        sdio_command_response_config(SDIO, SD_CMD_SET_BLOCKLEN, 512U,
                                     SDIO_RESPONSETYPE_SHORT);
        sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
        sdio_csm_enable(SDIO);
        status = r1_error_check(SD_CMD_SET_BLOCKLEN);
        if(status != SD_OK)
            return status;
    }

    sdio_data_config(SDIO, SD_DATATIMEOUT, 512U, SDIO_DATABLOCKSIZE_512BYTES);
    sdio_data_transfer_config(SDIO, SDIO_TRANSMODE_BLOCKCOUNT, direction);
    /* H7 TREN starts the DSM with the data command (as in the official
     * CMD17/CMD24 example); DATAEN is reserved for explicit DSM starts. */
    sdio_trans_start_enable(SDIO);
    command = (direction == SDIO_TRANSDIRECTION_TOSDIO) ?
              SD_CMD_READ_SINGLE_BLOCK : SD_CMD_WRITE_BLOCK;
    sdio_command_response_config(SDIO, command, sector, SDIO_RESPONSETYPE_SHORT);
    sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
    sdio_csm_enable(SDIO);
    status = r1_error_check(command);
    if(status == SD_OK)
        status = sd_poll_data(buffer, 512U, direction);
    sd_data_cleanup();
    if(status == SD_OK)
        status = sd_wait_transfer_ready();
    return status;
}

/*!
    \brief      initialize the SD card and make it in standby state
    \param[in]  none
    \param[out] none
    \retval     sd_error_enum
*/
sd_error_enum sd_init(void)
{
    sd_error_enum status = SD_OK;
    /* A reinserted card can have a different capacity and RCA. */
    cardtype = SDIO_STD_CAPACITY_SD_CARD_V1_1;
    sdcardtype = SD_STD_CAPACITY;
    cardcapacity = SD_SDSC;
    sd_rca = 0U;
    transmode = SD_POLLING_MODE;
    rt_memset(sd_csd, 0, sizeof(sd_csd));
    rt_memset(sd_cid, 0, sizeof(sd_cid));
    rt_memset(sd_scr, 0, sizeof(sd_scr));
    /* configure the RCU and GPIO, deinitialize the SDIO */
    status = rcu_config();
    if (status != SD_OK)
        return status;
    gpio_config();
    sdio_deinit(SDIO);

    /* configure the clock and work voltage */
    status = sd_power_on();
    if(SD_OK != status) {
        return status;
    }

    /* initialize the card and get CID and CSD of the card */
    status = sd_card_init();
    if(SD_OK != status) {
        return status;
    }

    /* configure the SDIO peripheral */
    sdio_clock_config(SDIO, SDIO_SDIOCLKEDGE_RISING, SDIO_CLOCKPWRSAVE_DISABLE, SD_CLK_DIV_INIT);
    sdio_bus_mode_set(SDIO, SDIO_BUSMODE_1BIT);
    sdio_hardware_clock_enable(SDIO);

    return status;
}

/*!
    \brief      initialize the card and get CID and CSD of the card
    \param[in]  none
    \param[out] none
    \retval     sd_error_enum
*/
sd_error_enum sd_card_init(void)
{
    sd_error_enum status = SD_OK;
    uint16_t temp_rca = 0x01U;

    if(SDIO_POWER_OFF == sdio_power_state_get(SDIO)) {
        status = SD_OPERATION_IMPROPER;
        return status;
    }

    /* the card is not I/O only card */
    if(SDIO_SECURE_DIGITAL_IO_CARD != cardtype) {
        /* send CMD2(SD_CMD_ALL_SEND_CID) to get the CID numbers */
        sdio_command_response_config(SDIO, SD_CMD_ALL_SEND_CID, (uint32_t)0x0, SDIO_RESPONSETYPE_LONG);
        sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
        sdio_csm_enable(SDIO);
        /* check if some error occurs */
        status = r2_error_check();
        if(SD_OK != status) {
            return status;
        }

        /* store the CID numbers */
        sd_cid[0] = sdio_response_get(SDIO, SDIO_RESPONSE0);
        sd_cid[1] = sdio_response_get(SDIO, SDIO_RESPONSE1);
        sd_cid[2] = sdio_response_get(SDIO, SDIO_RESPONSE2);
        sd_cid[3] = sdio_response_get(SDIO, SDIO_RESPONSE3);
    }

    /* the card is SD memory card or the I/O card has the memory portion */
    if((SDIO_STD_CAPACITY_SD_CARD_V1_1 == cardtype) || (SDIO_STD_CAPACITY_SD_CARD_V2_0 == cardtype) || (SDIO_STD_CAPACITY_SD_CARD_V3_0 == cardtype) ||
            (SDIO_HIGH_CAPACITY_SD_CARD == cardtype) || (SDIO_SECURE_DIGITAL_IO_COMBO_CARD == cardtype)) {
        /* send CMD3(SEND_RELATIVE_ADDR) to ask the card to publish a new relative address (RCA) */
        sdio_command_response_config(SDIO, SD_CMD_SEND_RELATIVE_ADDR, (uint32_t)0x0, SDIO_RESPONSETYPE_SHORT);
        sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
        sdio_csm_enable(SDIO);
        /* check if some error occurs */
        status = r6_error_check(SD_CMD_SEND_RELATIVE_ADDR, &temp_rca);
        if(SD_OK != status) {
            return status;
        }
    }

    if(SDIO_SECURE_DIGITAL_IO_CARD != cardtype) {
        /* the card is not I/O only card */
        sd_rca = temp_rca;

        /* send CMD9(SEND_CSD) to get the addressed card's card-specific data (CSD) */
        sdio_command_response_config(SDIO, (uint32_t)SD_CMD_SEND_CSD, ((uint32_t)temp_rca) << SD_RCA_SHIFT, SDIO_RESPONSETYPE_LONG);
        sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
        sdio_csm_enable(SDIO);
        /* check if some error occurs */
        status = r2_error_check();
        if(SD_OK != status) {
            return status;
        }

        /* store the card-specific data (CSD) */
        sd_csd[0] = sdio_response_get(SDIO, SDIO_RESPONSE0);
        sd_csd[1] = sdio_response_get(SDIO, SDIO_RESPONSE1);
        sd_csd[2] = sdio_response_get(SDIO, SDIO_RESPONSE2);
        sd_csd[3] = sdio_response_get(SDIO, SDIO_RESPONSE3);
    }
    return status;
}

/*!
    \brief      configure the clock and the work voltage, and get the card type
    \param[in]  none
    \param[out] none
    \retval     sd_error_enum
*/
sd_error_enum sd_power_on(void)
{
    sd_error_enum status = SD_OK;
    uint32_t response = 0U, count = 0U;
    uint8_t busyflag = 0U;
    rt_tick_t started;

    /* configure the SDIO peripheral */
    sdio_clock_config(SDIO, SDIO_SDIOCLKEDGE_RISING, SDIO_CLOCKPWRSAVE_DISABLE, SD_CLK_DIV_INIT);
    sdio_bus_mode_set(SDIO, SDIO_BUSMODE_1BIT);
    sdio_hardware_clock_disable(SDIO);
    sdio_power_state_set(SDIO, SDIO_POWER_ON);

    /* Allow supply settling and at least 74 initial SD clocks. */
    rt_thread_mdelay(2);

    /* send CMD0(GO_IDLE_STATE) to reset the card */
    sdio_command_response_config(SDIO, SD_CMD_GO_IDLE_STATE, (uint32_t)0x0, SDIO_RESPONSETYPE_NO);
    sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
    /* enable the CSM */
    sdio_csm_enable(SDIO);

    /* check if command sent error occurs */
    status = cmdsent_error_check();
    if(SD_OK != status) {
        return status;
    }

    /* send CMD8(SEND_IF_COND) to get SD memory card interface condition */
    sdio_command_response_config(SDIO, SD_CMD_SEND_IF_COND, SD_CHECK_PATTERN, SDIO_RESPONSETYPE_SHORT);
    sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
    sdio_csm_enable(SDIO);

    status = r7_error_check();
    if(SD_OK == status) {
        /* SD Card 2.0 */
        cardtype = SDIO_STD_CAPACITY_SD_CARD_V2_0;
        sdcardtype = SD_HIGH_CAPACITY;
    } else if(status != SD_CMD_RESP_TIMEOUT)
        return status;

    /* send CMD55(APP_CMD) to indicate next command is application specific command */
    sdio_command_response_config(SDIO, SD_CMD_APP_CMD, (uint32_t)0x0, SDIO_RESPONSETYPE_SHORT);
    sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
    sdio_csm_enable(SDIO);

    status = r1_error_check(SD_CMD_APP_CMD);
    if(SD_OK == status) {
        /* SD memory card */
        started = rt_tick_get();
        while((!busyflag) && (count < SD_MAX_VOLT_VALIDATION)) {
            /* send CMD55(APP_CMD) to indicate next command is application specific command */
            sdio_command_response_config(SDIO, SD_CMD_APP_CMD, (uint32_t)0x0, SDIO_RESPONSETYPE_SHORT);
            sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
            sdio_csm_enable(SDIO);
            /* check if some error occurs */
            status = r1_error_check(SD_CMD_APP_CMD);
            if(SD_OK != status) {
                return status;
            }

            /* send ACMD41(SD_SEND_OP_COND) to get host capacity support information (HCS) and OCR content */
            /* This board port uses 3.3 V signalling; do not request S18R. */
            sdio_command_response_config(SDIO, SD_APPCMD_SD_SEND_OP_COND, (SD_VOLTAGE_WINDOW | sdcardtype), SDIO_RESPONSETYPE_SHORT);
            sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
            sdio_csm_enable(SDIO);
            /* check if some error occurs */
            status = r3_error_check();
            if(SD_OK != status) {
                return status;
            }
            /* get the response and check card power up status bit(busy) */
            response = sdio_response_get(SDIO, SDIO_RESPONSE0);
            busyflag = (uint8_t)((response >> 31U) & (uint32_t)0x01U);
            ++count;
            if(!busyflag) {
                if((rt_tick_t)(rt_tick_get() - started) >= rt_tick_from_millisecond(SD_BUSY_TIMEOUT_MS))
                    return SD_VOLTRANGE_INVALID;
                rt_thread_mdelay(1);
            }
        }
        if(!busyflag) {
            status = SD_VOLTRANGE_INVALID;
            return status;
        }
        if(response & SD_HIGH_CAPACITY) {
            /* SDHC card */
            cardcapacity = SD_SDHC_SDXC;
            cardtype = SDIO_HIGH_CAPACITY_SD_CARD;
        }
    }
    return status;
}

/*!
    \brief      close the power of SDIO
    \param[in]  none
    \param[out] none
    \retval     sd_error_enum
*/
sd_error_enum sd_power_off(void)
{
    sd_error_enum status = SD_OK;
    sdio_power_state_set(SDIO, SDIO_POWER_OFF);
    return status;
}

/*!
    \brief      configure the bus mode
    \param[in]  busmode: the bus mode
      \arg        SDIO_BUSMODE_1BIT: 1-bit SDIO card bus mode
      \arg        SDIO_BUSMODE_4BIT: 4-bit SDIO card bus mode
      \arg        SDIO_BUSMODE_8BIT: 8-bit SDIO card bus mode (MMC only)
    \param[in]  speed: the bus speed mode
      \arg        SD_SPEED_DEFAULT: default bus speed
      \arg        SD_SPEED_HIGH: high bus speed
      \arg        SD_SPEED_SDR25: SDR25 bus speed
      \arg        SD_SPEED_SDR50: SDR50 bus speed
      \arg        SD_SPEED_SDR104: SDR104 bus speed
      \arg        SD_SPEED_DDR50: DDR50 bus speed
    \param[out] none
    \retval     sd_error_enum
*/
sd_error_enum sd_bus_mode_config(uint32_t busmode, uint32_t speed)
{
    sd_error_enum status;
    /* PC11 is LCD B4. This port only supports 1-bit, 3.3 V signalling. */
    if(busmode != SDIO_BUSMODE_1BIT || speed != SD_SPEED_DEFAULT)
        return SD_FUNCTION_UNSUPPORTED;
    status = sd_bus_width_config(SD_BUS_WIDTH_1BIT);
    if(status != SD_OK)
        return status;
    sdio_bus_mode_set(SDIO, SDIO_BUSMODE_1BIT);
    sdio_clock_config(SDIO, SDIO_SDIOCLKEDGE_RISING,
                      SDIO_CLOCKPWRSAVE_DISABLE, SD_CLK_DIV_TRANS_DSPEED);
    sdio_hardware_clock_enable(SDIO);
    return SD_OK;
}
/*!
    \brief      switch 1.8V power level of SD card
    \param[in]  none
    \param[out] none
    \retval     sd_error_enum
*/
sd_error_enum sd_card_voltage_switch(void)
{
    /* Keep the LCD-compatible SD wiring at 3.3 V. */
    return SD_FUNCTION_UNSUPPORTED;
}
/*!
    \brief      configure the mode of transmission
    \param[in]  txmode: transfer mode
      \arg        SD_DMA_MODE: DMA mode
      \arg        SD_POLLING_MODE: polling mode
    \param[out] none
    \retval     sd_error_enum
*/
sd_error_enum sd_transfer_mode_config(uint32_t txmode)
{
    /* DMA interrupt completion/cache handling is not used by this BSP. */
    if(txmode != SD_POLLING_MODE)
        return SD_FUNCTION_UNSUPPORTED;
    transmode = txmode;
    return SD_OK;
}
/*!
    \brief      read a block data into a buffer from the specified address of a card
    \param[out] preadbuffer: a pointer that store a block read data
    \param[in]  readaddr: the read data address
    \param[in]  blocksize: the data block size
    \retval     sd_error_enum
*/
sd_error_enum sd_block_read(uint32_t *preadbuffer, uint32_t readaddr, uint16_t blocksize)
{
    return sd_poll_block(preadbuffer, readaddr, blocksize, SDIO_TRANSDIRECTION_TOSDIO);
}
/*!
    \brief      read multiple blocks data into a buffer from the specified address of a card
    \param[out] preadbuffer: a pointer that store multiple blocks read data
    \param[in]  readaddr: the read data address
    \param[in]  blocksize: the data block size
    \param[in]  blocksnumber: number of blocks that will be read
    \retval     sd_error_enum
*/
sd_error_enum sd_multiblocks_read(uint32_t *preadbuffer, uint32_t readaddr, uint16_t blocksize, uint32_t blocksnumber)
{
    uint32_t block;
    sd_error_enum status;
    if(preadbuffer == NULL || blocksize != 512U || blocksnumber == 0U ||
       blocksnumber > SD_MAX_DATA_LENGTH / 512U ||
       readaddr > 0xFFFFFFFFU - (blocksnumber - 1U))
        return SD_PARAMETER_INVALID;
    /* Each block has a bounded transaction and an independent CRC. */
    for(block = 0U; block < blocksnumber; ++block) {
        status = sd_block_read(preadbuffer + block * 128U, readaddr + block, blocksize);
        if(status != SD_OK)
            return status;
    }
    return SD_OK;
}
/*!
    \brief      write a block data to the specified address of a card
    \param[in]  pwritebuffer: a pointer that store a block data to be transferred
    \param[in]  writeaddr: the read data address
    \param[in]  blocksize: the data block size
    \param[out] none
    \retval     sd_error_enum
*/
sd_error_enum sd_block_write(uint32_t *pwritebuffer, uint32_t writeaddr, uint16_t blocksize)
{
    return sd_poll_block(pwritebuffer, writeaddr, blocksize, SDIO_TRANSDIRECTION_TOCARD);
}
/*!
    \brief      write multiple blocks data to the specified address of a card
    \param[in]  pwritebuffer: a pointer that store multiple blocks data to be transferred
    \param[in]  writeaddr: the read data address
    \param[in]  blocksize: the data block size
    \param[in]  blocksnumber: number of blocks that will be written
    \param[out] none
    \retval     sd_error_enum
*/
sd_error_enum sd_multiblocks_write(uint32_t *pwritebuffer, uint32_t writeaddr, uint16_t blocksize, uint32_t blocksnumber)
{
    uint32_t block;
    sd_error_enum status;
    if(pwritebuffer == NULL || blocksize != 512U || blocksnumber == 0U ||
       blocksnumber > SD_MAX_DATA_LENGTH / 512U ||
       writeaddr > 0xFFFFFFFFU - (blocksnumber - 1U))
        return SD_PARAMETER_INVALID;
    /* Each block has a bounded transaction and an independent CRC. */
    for(block = 0U; block < blocksnumber; ++block) {
        status = sd_block_write(pwritebuffer + block * 128U, writeaddr + block, blocksize);
        if(status != SD_OK)
            return status;
    }
    return SD_OK;
}
/*!
    \brief      erase a continuous area of a card
    \param[in]  startaddr: the start address
    \param[in]  endaddr: the end address
    \param[out] none
    \retval     sd_error_enum
*/
sd_error_enum sd_erase(uint32_t startaddr, uint32_t endaddr)
{
    /* initialize the variables */
    sd_error_enum status = SD_OK;
    uint32_t count = 0U, clkdiv = 0U;
    __IO uint32_t delay = 0U;
    uint8_t cardstate = 0U, tempbyte = 0U;
    uint16_t tempccc = 0U;

    /* get the card command classes from CSD */
    tempbyte = (uint8_t)((sd_csd[1] & SD_MASK_24_31BITS) >> 24U);
    tempccc = (uint16_t)((uint16_t)tempbyte << 4U);
    tempbyte = (uint8_t)((sd_csd[1] & SD_MASK_16_23BITS) >> 16U);
    tempccc |= ((uint16_t)tempbyte & 0xF0U) >> 4U;
    if(0U == (tempccc & SD_CCC_ERASE)) {
        /* don't support the erase command */
        status = SD_FUNCTION_UNSUPPORTED;
        return status;
    }
    clkdiv = (SDIO_CLKCTL(SDIO) & SDIO_CLKCTL_DIV);
    clkdiv *= 2U;
    delay = (clkdiv != 0U) ? (168000U / clkdiv) : 0U;

    /* check whether the card is locked */
    if(sdio_response_get(SDIO, SDIO_RESPONSE0) & SD_CARDSTATE_LOCKED) {
        status = SD_LOCK_UNLOCK_FAILED;
        return(status);
    }

    /* blocksize is fixed in 512B for SDHC card */
    if(SDIO_HIGH_CAPACITY_SD_CARD != cardtype) {
        startaddr *= 512U;
        endaddr *= 512U;
    }

    if((SDIO_STD_CAPACITY_SD_CARD_V1_1 == cardtype) || (SDIO_STD_CAPACITY_SD_CARD_V2_0 == cardtype) ||
            (SDIO_HIGH_CAPACITY_SD_CARD == cardtype)) {
        /* send CMD32(ERASE_WR_BLK_START) to set the address of the first write block to be erased */
        sdio_command_response_config(SDIO, SD_CMD_ERASE_WR_BLK_START, startaddr, SDIO_RESPONSETYPE_SHORT);
        sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
        sdio_csm_enable(SDIO);
        /* check if some error occurs */
        status = r1_error_check(SD_CMD_ERASE_WR_BLK_START);
        if(SD_OK != status) {
            return status;
        }

        /* send CMD33(ERASE_WR_BLK_END) to set the address of the last write block of the continuous range to be erased */
        sdio_command_response_config(SDIO, SD_CMD_ERASE_WR_BLK_END, endaddr, SDIO_RESPONSETYPE_SHORT);
        sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
        sdio_csm_enable(SDIO);
        /* check if some error occurs */
        status = r1_error_check(SD_CMD_ERASE_WR_BLK_END);
        if(SD_OK != status) {
            return status;
        }
    }

    /* send CMD38(ERASE) to set the address of the first write block to be erased */
    sdio_command_response_config(SDIO, SD_CMD_ERASE, (uint32_t)0x0, SDIO_RESPONSETYPE_SHORT);
    sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
    sdio_csm_enable(SDIO);
    /* check if some error occurs */
    status = r1_error_check(SD_CMD_ERASE);
    if(SD_OK != status) {
        return status;
    }
    /* loop until the counter is reach to the calculated time */
    for(count = 0U; count < delay; count++) {
    }
    /* get the card state and wait the card is out of programming and receiving state */
    status = sd_card_state_get(&cardstate);
    count = SD_BUSY_TIMEOUT_MS;
    while((SD_OK == status) && ((SD_CARDSTATE_PROGRAMMING == cardstate) || (SD_CARDSTATE_RECEIVING == cardstate))) {
        if(count-- == 0U)
            return SD_DATA_TIMEOUT;
        rt_thread_mdelay(1);
        status = sd_card_state_get(&cardstate);
    }
    return status;
}



/*!
    \brief      process all the interrupts which the corresponding flags are set
    \param[in]  none
    \param[out] none
    \retval     sd_error_enum
*/
sd_error_enum sd_interrupts_process(void)
{
    transerror = SD_OK;
    if(RESET != sdio_interrupt_flag_get(SDIO, SDIO_INT_FLAG_DTEND)) {
        /* clear DTEND flag */
        sdio_interrupt_flag_clear(SDIO, SDIO_INT_FLAG_DTEND);
        /* disable idma for idma transfer */
        sdio_idma_disable(SDIO);

        /* disable all the interrupts */
        sdio_interrupt_disable(SDIO, SDIO_INT_DTCRCERR | SDIO_INT_DTTMOUT | SDIO_INT_DTEND |
                               SDIO_INT_TFH | SDIO_INT_RFH | SDIO_INT_TXURE | SDIO_INT_RXORE);
        sdio_trans_start_disable(SDIO);
        /* send CMD12 to stop data transfer in multipule blocks operation */
        if(1 == stopcondition) {
            transerror = sd_transfer_stop();
        } else {
            transerror = SD_OK;
        }
        transend = 1;
        number_bytes = 0;
        /* clear data flags */
        sdio_flag_clear(SDIO, SDIO_MASK_DATA_FLAGS);
        return transerror;
    }

    if(RESET != sdio_interrupt_flag_get(SDIO, SDIO_INT_FLAG_DTCRCERR | SDIO_INT_FLAG_DTTMOUT | SDIO_INT_FLAG_TXURE | SDIO_INT_FLAG_RXORE)) {
        /* set different errors */
        if(RESET != sdio_interrupt_flag_get(SDIO, SDIO_INT_FLAG_DTCRCERR)) {
            transerror = SD_DATA_CRC_ERROR;
        }
        if(RESET != sdio_interrupt_flag_get(SDIO, SDIO_INT_FLAG_DTTMOUT)) {
            transerror = SD_DATA_TIMEOUT;
        }
        if(RESET != sdio_interrupt_flag_get(SDIO, SDIO_INT_FLAG_TXURE)) {
            transerror = SD_TX_UNDERRUN_ERROR;
        }
        if(RESET != sdio_interrupt_flag_get(SDIO, SDIO_INT_FLAG_RXORE)) {
            transerror = SD_RX_OVERRUN_ERROR;
        }
        /* clear data flags */
        sdio_flag_clear(SDIO, SDIO_MASK_DATA_FLAGS);
        /* disable all the interrupts */
        sdio_interrupt_disable(SDIO, SDIO_INT_DTCRCERR | SDIO_INT_DTTMOUT | SDIO_INT_DTEND |
                               SDIO_INT_TXURE | SDIO_INT_RXORE);

        sdio_trans_start_disable(SDIO);
        sdio_fifo_reset_enable(SDIO);
        sdio_fifo_reset_disable(SDIO);
        /* send CMD12 to stop data transfer in multipule blocks operation */
        transerror = sd_transfer_stop();
        sdio_flag_clear(SDIO, SDIO_FLAG_DTABORT);
        number_bytes = 0;

        if(transmode == SD_DMA_MODE) {
            sdio_idma_disable(SDIO);
        }
        return transerror;
    }

    if(RESET != sdio_interrupt_flag_get(SDIO, SDIO_INT_FLAG_IDMAERR)) {
        sdio_interrupt_flag_clear(SDIO, SDIO_INT_FLAG_IDMAERR);
        transerror = SD_DMA_ERROR;
        /* disable all the interrupts */
        sdio_interrupt_disable(SDIO, SDIO_INT_DTCRCERR | SDIO_INT_DTTMOUT | SDIO_INT_DTEND |
                               SDIO_INT_TFH | SDIO_INT_RFH | SDIO_INT_TXURE | SDIO_INT_RXORE);
        sdio_trans_start_disable(SDIO);
        sdio_fifo_reset_enable(SDIO);
        sdio_fifo_reset_disable(SDIO);
        /* send CMD12 to stop data transfer in multipule blocks operation */
        transerror = sd_transfer_stop();
        sdio_flag_clear(SDIO, SDIO_FLAG_DTABORT);
        number_bytes = 0;

        sdio_idma_disable(SDIO);
        return transerror;
    }
    return transerror;
}

/*!
    \brief      select or deselect a card
    \param[in]  cardrca: the RCA of a card
    \param[out] none
    \retval     sd_error_enum
*/
sd_error_enum sd_card_select_deselect(uint16_t cardrca)
{
    sd_error_enum status = SD_OK;
    /* send CMD7(SELECT/DESELECT_CARD) to select or deselect the card */
    sdio_command_response_config(SDIO, SD_CMD_SELECT_DESELECT_CARD, (uint32_t)cardrca << SD_RCA_SHIFT, SDIO_RESPONSETYPE_SHORT);
    sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
    sdio_csm_enable(SDIO);

    status = r1_error_check(SD_CMD_SELECT_DESELECT_CARD);
    return status;
}

/*!
    \brief      get the card status whose response format R1 contains a 32-bit field
    \param[in]  none
    \param[out] pcardstatus: a pointer that store card status
    \retval     sd_error_enum
*/
sd_error_enum sd_cardstatus_get(uint32_t *pcardstatus)
{
    sd_error_enum status = SD_OK;
    if(NULL == pcardstatus) {
        status = SD_PARAMETER_INVALID;
        return status;
    }

    /* send CMD13(SEND_STATUS), addressed card sends its status register */
    sdio_command_response_config(SDIO, SD_CMD_SEND_STATUS, (uint32_t)sd_rca << SD_RCA_SHIFT, SDIO_RESPONSETYPE_SHORT);
    sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
    sdio_csm_enable(SDIO);
    /* check if some error occurs */
    status = r1_error_check(SD_CMD_SEND_STATUS);
    if(SD_OK != status) {
        return status;
    }

    *pcardstatus = sdio_response_get(SDIO, SDIO_RESPONSE0);
    return status;
}

/*!
    \brief      get SD card capacity
    \param[in]  none
    \param[out] none
    \retval     capacity of the card(KB)
*/
uint32_t sd_card_capacity_get(void)
{
    uint8_t tempbyte = 0U, devicesize_mult = 0U, readblklen = 0U;
    uint32_t capacity = 0U, devicesize = 0U;
    if((SDIO_STD_CAPACITY_SD_CARD_V1_1 == cardtype) || (SDIO_STD_CAPACITY_SD_CARD_V2_0 == cardtype)) {
        /* calculate the c_size(device size) */
        tempbyte = (uint8_t)((sd_csd[1] & SD_MASK_8_15BITS) >> 8U);
        devicesize |= ((uint32_t)tempbyte & 0x03U) << 10U;
        tempbyte = (uint8_t)(sd_csd[1] & SD_MASK_0_7BITS);
        devicesize |= (uint32_t)((uint32_t)tempbyte << 2U);
        tempbyte = (uint8_t)((sd_csd[2] & SD_MASK_24_31BITS) >> 24U);
        devicesize |= ((uint32_t)tempbyte & 0xC0U) >> 6U;

        /* calculate the c_size_mult(device size multiplier) */
        tempbyte = (uint8_t)((sd_csd[2] & SD_MASK_16_23BITS) >> 16U);
        devicesize_mult = (tempbyte & 0x03U) << 1U;
        tempbyte = (uint8_t)((sd_csd[2] & SD_MASK_8_15BITS) >> 8U);
        devicesize_mult |= (tempbyte & 0x80U) >> 7U;

        /* calculate the read_bl_len */
        tempbyte = (uint8_t)((sd_csd[1] & SD_MASK_16_23BITS) >> 16U);
        readblklen = tempbyte & 0x0FU;

        /* capacity = BLOCKNR*BLOCK_LEN, BLOCKNR = (C_SIZE+1)*MULT, MULT = 2^(C_SIZE_MULT+2), BLOCK_LEN = 2^READ_BL_LEN */
        capacity = (devicesize + 1U) * (1U << (devicesize_mult + 2U));
        capacity *= (1U << readblklen);

        /* change the unit of capacity to KByte */
        capacity /= 1024U;
    } else if(SDIO_HIGH_CAPACITY_SD_CARD == cardtype) {
        /* calculate the c_size */
        tempbyte = (uint8_t)(sd_csd[1] & SD_MASK_0_7BITS);
        devicesize = ((uint32_t)tempbyte & 0x3FU) << 16U;
        tempbyte = (uint8_t)((sd_csd[2] & SD_MASK_24_31BITS) >> 24U);
        devicesize |= (uint32_t)((uint32_t)tempbyte << 8U);
        tempbyte = (uint8_t)((sd_csd[2] & SD_MASK_16_23BITS) >> 16U);
        devicesize |= (uint32_t)tempbyte;

        /* capacity = (c_size+1)*512KByte */
        capacity = (devicesize + 1U) * 512U;
    } else {
        /* if else end */
    }
    return capacity;
}

/*!
    \brief      get the detailed information of the SD card based on received CID and CSD
    \param[in]  none
    \param[out] pcardinfo: a pointer that store the detailed card information
    \retval     sd_error_enum
*/
sd_error_enum sd_card_information_get(sd_card_info_struct *pcardinfo)
{
    sd_error_enum status = SD_OK;
    uint8_t tempbyte = 0U;

    if(NULL == pcardinfo) {
        status = SD_PARAMETER_INVALID;
        return status;
    }

    /* store the card type and RCA */
    pcardinfo->card_type = cardtype;
    pcardinfo->card_rca = sd_rca;

    /* CID byte 0 */
    tempbyte = (uint8_t)((sd_cid[0] & SD_MASK_24_31BITS) >> 24U);
    pcardinfo->card_cid.mid = tempbyte;

    /* CID byte 1 */
    tempbyte = (uint8_t)((sd_cid[0] & SD_MASK_16_23BITS) >> 16U);
    pcardinfo->card_cid.oid = (uint16_t)((uint16_t)tempbyte << 8U);

    /* CID byte 2 */
    tempbyte = (uint8_t)((sd_cid[0] & SD_MASK_8_15BITS) >> 8U);
    pcardinfo->card_cid.oid |= (uint16_t)tempbyte;

    /* CID byte 3 */
    tempbyte = (uint8_t)(sd_cid[0] & SD_MASK_0_7BITS);
    pcardinfo->card_cid.pnm0 = (uint32_t)((uint32_t)tempbyte << 24U);

    /* CID byte 4 */
    tempbyte = (uint8_t)((sd_cid[1] & SD_MASK_24_31BITS) >> 24U);
    pcardinfo->card_cid.pnm0 |= (uint32_t)((uint32_t)tempbyte << 16U);

    /* CID byte 5 */
    tempbyte = (uint8_t)((sd_cid[1] & SD_MASK_16_23BITS) >> 16U);
    pcardinfo->card_cid.pnm0 |= (uint32_t)((uint32_t)tempbyte << 8U);

    /* CID byte 6 */
    tempbyte = (uint8_t)((sd_cid[1] & SD_MASK_8_15BITS) >> 8U);
    pcardinfo->card_cid.pnm0 |= (uint32_t)(tempbyte);

    /* CID byte 7 */
    tempbyte = (uint8_t)(sd_cid[1] & SD_MASK_0_7BITS);
    pcardinfo->card_cid.pnm1 = tempbyte;

    /* CID byte 8 */
    tempbyte = (uint8_t)((sd_cid[2] & SD_MASK_24_31BITS) >> 24U);
    pcardinfo->card_cid.prv = tempbyte;

    /* CID byte 9 */
    tempbyte = (uint8_t)((sd_cid[2] & SD_MASK_16_23BITS) >> 16U);
    pcardinfo->card_cid.psn = (uint32_t)((uint32_t)tempbyte << 24U);

    /* CID byte 10 */
    tempbyte = (uint8_t)((sd_cid[2] & SD_MASK_8_15BITS) >> 8U);
    pcardinfo->card_cid.psn |= (uint32_t)((uint32_t)tempbyte << 16U);

    /* CID byte 11 */
    tempbyte = (uint8_t)(sd_cid[2] & SD_MASK_0_7BITS);
    pcardinfo->card_cid.psn |= (uint32_t)tempbyte;

    /* CID byte 12 */
    tempbyte = (uint8_t)((sd_cid[3] & SD_MASK_24_31BITS) >> 24U);
    pcardinfo->card_cid.psn |= (uint32_t)tempbyte;

    /* CID byte 13 */
    tempbyte = (uint8_t)((sd_cid[3] & SD_MASK_16_23BITS) >> 16U);
    pcardinfo->card_cid.mdt = ((uint16_t)tempbyte & 0x0FU) << 8U;

    /* CID byte 14 */
    tempbyte = (uint8_t)((sd_cid[3] & SD_MASK_8_15BITS) >> 8U);
    pcardinfo->card_cid.mdt |= (uint16_t)tempbyte;

    /* CID byte 15 */
    tempbyte = (uint8_t)(sd_cid[3] & SD_MASK_0_7BITS);
    pcardinfo->card_cid.cid_crc = (tempbyte & 0xFEU) >> 1U;

    /* CSD byte 0 */
    tempbyte = (uint8_t)((sd_csd[0] & SD_MASK_24_31BITS) >> 24U);
    pcardinfo->card_csd.csd_struct = (tempbyte & 0xC0U) >> 6U;

    /* CSD byte 1 */
    tempbyte = (uint8_t)((sd_csd[0] & SD_MASK_16_23BITS) >> 16U);
    pcardinfo->card_csd.taac = tempbyte;

    /* CSD byte 2 */
    tempbyte = (uint8_t)((sd_csd[0] & SD_MASK_8_15BITS) >> 8U);
    pcardinfo->card_csd.nsac = tempbyte;

    /* CSD byte 3 */
    tempbyte = (uint8_t)(sd_csd[0] & SD_MASK_0_7BITS);
    pcardinfo->card_csd.tran_speed = tempbyte;

    /* CSD byte 4 */
    tempbyte = (uint8_t)((sd_csd[1] & SD_MASK_24_31BITS) >> 24U);
    pcardinfo->card_csd.ccc = (uint16_t)((uint16_t)tempbyte << 4U);

    /* CSD byte 5 */
    tempbyte = (uint8_t)((sd_csd[1] & SD_MASK_16_23BITS) >> 16U);
    pcardinfo->card_csd.ccc |= ((uint16_t)tempbyte & 0xF0U) >> 4U;
    pcardinfo->card_csd.read_bl_len = tempbyte & 0x0FU;

    /* CSD byte 6 */
    tempbyte = (uint8_t)((sd_csd[1] & SD_MASK_8_15BITS) >> 8U);
    pcardinfo->card_csd.read_bl_partial = (tempbyte & 0x80U) >> 7U;
    pcardinfo->card_csd.write_blk_misalign = (tempbyte & 0x40U) >> 6U;
    pcardinfo->card_csd.read_blk_misalign = (tempbyte & 0x20U) >> 5U;
    pcardinfo->card_csd.dsp_imp = (tempbyte & 0x10U) >> 4U;

    if((SDIO_STD_CAPACITY_SD_CARD_V1_1 == cardtype) || (SDIO_STD_CAPACITY_SD_CARD_V2_0 == cardtype)) {
        /* card is SDSC card, CSD version 1.0 */
        pcardinfo->card_csd.c_size = ((uint32_t)tempbyte & 0x03U) << 10U;

        /* CSD byte 7 */
        tempbyte = (uint8_t)(sd_csd[1] & SD_MASK_0_7BITS);
        pcardinfo->card_csd.c_size |= (uint32_t)((uint32_t)tempbyte << 2U);

        /* CSD byte 8 */
        tempbyte = (uint8_t)((sd_csd[2] & SD_MASK_24_31BITS) >> 24U);
        pcardinfo->card_csd.c_size |= ((uint32_t)tempbyte & 0xC0U) >> 6U;
        pcardinfo->card_csd.vdd_r_curr_min = (tempbyte & 0x38U) >> 3U;
        pcardinfo->card_csd.vdd_r_curr_max = tempbyte & 0x07U;

        /* CSD byte 9 */
        tempbyte = (uint8_t)((sd_csd[2] & SD_MASK_16_23BITS) >> 16U);
        pcardinfo->card_csd.vdd_w_curr_min = (tempbyte & 0xE0U) >> 5U;
        pcardinfo->card_csd.vdd_w_curr_max = (tempbyte & 0x1CU) >> 2U;
        pcardinfo->card_csd.c_size_mult = (tempbyte & 0x03U) << 1U;

        /* CSD byte 10 */
        tempbyte = (uint8_t)((sd_csd[2] & SD_MASK_8_15BITS) >> 8U);
        pcardinfo->card_csd.c_size_mult |= (tempbyte & 0x80U) >> 7U;

        /* calculate the card block size and capacity */
        pcardinfo->card_blocksize = 1U << (pcardinfo->card_csd.read_bl_len);
        pcardinfo->card_capacity = pcardinfo->card_csd.c_size + 1U;
        pcardinfo->card_capacity *= (1U << (pcardinfo->card_csd.c_size_mult + 2U));
        pcardinfo->card_capacity *= pcardinfo->card_blocksize;
    } else if(SDIO_HIGH_CAPACITY_SD_CARD == cardtype) {
        /* card is SDHC card, CSD version 2.0 */
        /* CSD byte 7 */
        tempbyte = (uint8_t)(sd_csd[1] & SD_MASK_0_7BITS);
        pcardinfo->card_csd.c_size = ((uint32_t)tempbyte & 0x3FU) << 16U;

        /* CSD byte 8 */
        tempbyte = (uint8_t)((sd_csd[2] & SD_MASK_24_31BITS) >> 24U);
        pcardinfo->card_csd.c_size |= (uint32_t)((uint32_t)tempbyte << 8U);

        /* CSD byte 9 */
        tempbyte = (uint8_t)((sd_csd[2] & SD_MASK_16_23BITS) >> 16U);
        pcardinfo->card_csd.c_size |= (uint32_t)tempbyte;

        /* calculate the card block size and capacity */
        pcardinfo->card_blocksize = 512U;
        pcardinfo->card_capacity = (pcardinfo->card_csd.c_size + 1U) * 512U * 1024U;
    } else {
        /* if else end */
    }

    pcardinfo->card_csd.erase_blk_en = (tempbyte & 0x40U) >> 6U;
    pcardinfo->card_csd.sector_size = (tempbyte & 0x3FU) << 1U;

    /* CSD byte 11 */
    tempbyte = (uint8_t)(sd_csd[2] & SD_MASK_0_7BITS);
    pcardinfo->card_csd.sector_size |= (tempbyte & 0x80U) >> 7U;
    pcardinfo->card_csd.wp_grp_size = (tempbyte & 0x7FU);

    /* CSD byte 12 */
    tempbyte = (uint8_t)((sd_csd[3] & SD_MASK_24_31BITS) >> 24U);
    pcardinfo->card_csd.wp_grp_enable = (tempbyte & 0x80U) >> 7U;
    pcardinfo->card_csd.r2w_factor = (tempbyte & 0x1CU) >> 2U;
    pcardinfo->card_csd.write_bl_len = (tempbyte & 0x03U) << 2U;

    /* CSD byte 13 */
    tempbyte = (uint8_t)((sd_csd[3] & SD_MASK_16_23BITS) >> 16U);
    pcardinfo->card_csd.write_bl_len |= (tempbyte & 0xC0U) >> 6U;
    pcardinfo->card_csd.write_bl_partial = (tempbyte & 0x20U) >> 5U;

    /* CSD byte 14 */
    tempbyte = (uint8_t)((sd_csd[3] & SD_MASK_8_15BITS) >> 8U);
    pcardinfo->card_csd.file_format_grp = (tempbyte & 0x80U) >> 7U;
    pcardinfo->card_csd.copy_flag = (tempbyte & 0x40U) >> 6U;
    pcardinfo->card_csd.perm_write_protect = (tempbyte & 0x20U) >> 5U;
    pcardinfo->card_csd.tmp_write_protect = (tempbyte & 0x10U) >> 4U;
    pcardinfo->card_csd.file_format = (tempbyte & 0x0CU) >> 2U;

    /* CSD byte 15 */
    tempbyte = (uint8_t)(sd_csd[3] & SD_MASK_0_7BITS);
    pcardinfo->card_csd.csd_crc = (tempbyte & 0xFEU) >> 1U;

    return status;
}

/*!
    \brief      check if the command sent error occurs
    \param[in]  none
    \param[out] none
    \retval     sd_error_enum
*/
static sd_error_enum cmdsent_error_check(void)
{
    sd_error_enum status = SD_OK;
    uint32_t timeout = 100000U;
    rt_tick_t started = rt_tick_get();
    /* check command sent flag */
    while((RESET == sdio_flag_get(SDIO, SDIO_FLAG_CMDSEND)) &&
          (timeout > 0U) && !sd_cmd_timeout_expired(started)) {
        --timeout;
    }
    /* command response is timeout */
    if(0U == timeout || sd_cmd_timeout_expired(started)) {
        status = SD_CMD_RESP_TIMEOUT;
        return status;
    }
    /* if the command is sent, clear the CMD_FLAGS flags */
    sdio_flag_clear(SDIO, SDIO_MASK_CMD_FLAGS);
    return status;
}

/*!
    \brief      check if error type for R1 response
    \param[in]  resp: content of response
    \param[out] none
    \retval     sd_error_enum
*/
static sd_error_enum r1_error_type_check(uint32_t resp)
{
    sd_error_enum status = SD_ERROR;
    /* check which error occurs */
    if(resp & SD_R1_OUT_OF_RANGE) {
        status = SD_OUT_OF_RANGE;
    } else if(resp & SD_R1_ADDRESS_ERROR) {
        status = SD_ADDRESS_ERROR;
    } else if(resp & SD_R1_BLOCK_LEN_ERROR) {
        status = SD_BLOCK_LEN_ERROR;
    } else if(resp & SD_R1_ERASE_SEQ_ERROR) {
        status = SD_ERASE_SEQ_ERROR;
    } else if(resp & SD_R1_ERASE_PARAM) {
        status = SD_ERASE_PARAM;
    } else if(resp & SD_R1_WP_VIOLATION) {
        status = SD_WP_VIOLATION;
    } else if(resp & SD_R1_LOCK_UNLOCK_FAILED) {
        status = SD_LOCK_UNLOCK_FAILED;
    } else if(resp & SD_R1_COM_CRC_ERROR) {
        status = SD_COM_CRC_ERROR;
    } else if(resp & SD_R1_ILLEGAL_COMMAND) {
        status = SD_ILLEGAL_COMMAND;
    } else if(resp & SD_R1_CARD_ECC_FAILED) {
        status = SD_CARD_ECC_FAILED;
    } else if(resp & SD_R1_CC_ERROR) {
        status = SD_CC_ERROR;
    } else if(resp & SD_R1_GENERAL_UNKNOWN_ERROR) {
        status = SD_GENERAL_UNKNOWN_ERROR;
    } else if(resp & SD_R1_CSD_OVERWRITE) {
        status = SD_CSD_OVERWRITE;
    } else if(resp & SD_R1_WP_ERASE_SKIP) {
        status = SD_WP_ERASE_SKIP;
    } else if(resp & SD_R1_CARD_ECC_DISABLED) {
        status = SD_CARD_ECC_DISABLED;
    } else if(resp & SD_R1_ERASE_RESET) {
        status = SD_ERASE_RESET;
    } else if(resp & SD_R1_AKE_SEQ_ERROR) {
        status = SD_AKE_SEQ_ERROR;
    } else {
        /*no todo,  if else end */
    }
    return status;
}

/*!
    \brief      check if error occurs for R1 response
    \param[in]  cmdindex: the index of command
    \param[out] none
    \retval     sd_error_enum
*/
static sd_error_enum r1_error_check(uint8_t cmdindex)
{
    sd_error_enum status = SD_OK;
    uint32_t reg_status = 0U, resp_r1 = 0U, timeout = SD_CMD_WAIT_LIMIT;
    rt_tick_t started = rt_tick_get();

    /* store the content of SDIO_STAT */
    reg_status = SDIO_STAT(SDIO);
    while(!(reg_status & (SDIO_FLAG_CCRCERR | SDIO_FLAG_CMDTMOUT | SDIO_FLAG_CMDRECV)) &&
          timeout != 0U && !sd_cmd_timeout_expired(started)) {
        reg_status = SDIO_STAT(SDIO);
        --timeout;
    }
    if (timeout == 0U || sd_cmd_timeout_expired(started))
        return SD_CMD_RESP_TIMEOUT;
    /* check whether an error or timeout occurs or command response received */
    if(reg_status & SDIO_FLAG_CCRCERR) {
        status = SD_CMD_CRC_ERROR;
        sdio_flag_clear(SDIO, SDIO_FLAG_CCRCERR);
        return status;
    } else if(reg_status & SDIO_FLAG_CMDTMOUT) {
        status = SD_CMD_RESP_TIMEOUT;
        sdio_flag_clear(SDIO, SDIO_FLAG_CMDTMOUT);
        return status;
    } else {
        /* if else end */
    }

    /* check whether the last response command index is the desired one */
    if(sdio_command_index_get(SDIO) != cmdindex) {
        status = SD_ILLEGAL_COMMAND;
        return status;
    }
    /* clear all the CMD_FLAGS flags */
    sdio_flag_clear(SDIO, SDIO_MASK_CMD_FLAGS);
    /* get the SDIO response register 0 for checking */
    resp_r1 = sdio_response_get(SDIO, SDIO_RESPONSE0);
    if(SD_ALLZERO == (resp_r1 & SD_R1_ERROR_BITS)) {
        /* no error occurs, return SD_OK */
        status = SD_OK;
        return status;
    }

    /* if some error occurs, return the error type */
    status = r1_error_type_check(resp_r1);
    return status;
}

/*!
    \brief      check if error occurs for R2 response
    \param[in]  none
    \param[out] none
    \retval     sd_error_enum
*/
static sd_error_enum r2_error_check(void)
{
    sd_error_enum status = SD_OK;
    uint32_t reg_status = 0U, timeout = SD_CMD_WAIT_LIMIT;
    rt_tick_t started = rt_tick_get();

    /* store the content of SDIO_STAT */
    reg_status = SDIO_STAT(SDIO);
    while(!(reg_status & (SDIO_FLAG_CCRCERR | SDIO_FLAG_CMDTMOUT | SDIO_FLAG_CMDRECV)) &&
          timeout != 0U && !sd_cmd_timeout_expired(started)) {
        reg_status = SDIO_STAT(SDIO);
        --timeout;
    }
    if (timeout == 0U || sd_cmd_timeout_expired(started))
        return SD_CMD_RESP_TIMEOUT;
    /* check whether an error or timeout occurs or command response received */
    if(reg_status & SDIO_FLAG_CCRCERR) {
        status = SD_CMD_CRC_ERROR;
        sdio_flag_clear(SDIO, SDIO_FLAG_CCRCERR);
        return status;
    } else if(reg_status & SDIO_FLAG_CMDTMOUT) {
        status = SD_CMD_RESP_TIMEOUT;
        sdio_flag_clear(SDIO, SDIO_FLAG_CMDTMOUT);
        return status;
    } else {
        /* if else end */
    }
    /* clear all the CMD_FLAGS flags */
    sdio_flag_clear(SDIO, SDIO_MASK_CMD_FLAGS);

    return status;
}


/*!
    \brief      check if error occurs for R3 response
    \param[in]  none
    \param[out] none
    \retval     sd_error_enum
*/
static sd_error_enum r3_error_check(void)
{
    sd_error_enum status = SD_OK;
    uint32_t reg_status = 0U, timeout = SD_CMD_WAIT_LIMIT;
    rt_tick_t started = rt_tick_get();

    /* store the content of SDIO_STAT */
    reg_status = SDIO_STAT(SDIO);
    while(!(reg_status & (SDIO_FLAG_CCRCERR | SDIO_FLAG_CMDTMOUT | SDIO_FLAG_CMDRECV)) &&
          timeout != 0U && !sd_cmd_timeout_expired(started)) {
        reg_status = SDIO_STAT(SDIO);
        --timeout;
    }
    if (timeout == 0U || sd_cmd_timeout_expired(started))
        return SD_CMD_RESP_TIMEOUT;
    if(reg_status & SDIO_FLAG_CMDTMOUT) {
        status = SD_CMD_RESP_TIMEOUT;
        sdio_flag_clear(SDIO, SDIO_FLAG_CMDTMOUT);
        return status;
    }
    /* clear all the CMD_FLAGS flags */
    sdio_flag_clear(SDIO, SDIO_MASK_CMD_FLAGS);
    return status;
}

/*!
    \brief      check if error occurs for R6 response
    \param[in]  cmdindex: the index of command
    \param[out] prca: a pointer that store the RCA of card
    \retval     sd_error_enum
*/
static sd_error_enum r6_error_check(uint8_t cmdindex, uint16_t *prca)
{
    sd_error_enum status = SD_OK;
    uint32_t reg_status = 0U, response = 0U, timeout = SD_CMD_WAIT_LIMIT;
    rt_tick_t started = rt_tick_get();

    /* store the content of SDIO_STAT */
    reg_status = SDIO_STAT(SDIO);
    while(!(reg_status & (SDIO_FLAG_CCRCERR | SDIO_FLAG_CMDTMOUT | SDIO_FLAG_CMDRECV)) &&
          timeout != 0U && !sd_cmd_timeout_expired(started)) {
        reg_status = SDIO_STAT(SDIO);
        --timeout;
    }
    if (timeout == 0U || sd_cmd_timeout_expired(started))
        return SD_CMD_RESP_TIMEOUT;
    /* check whether an error or timeout occurs or command response received */
    if(reg_status & SDIO_FLAG_CCRCERR) {
        status = SD_CMD_CRC_ERROR;
        sdio_flag_clear(SDIO, SDIO_FLAG_CCRCERR);
        return status;
    } else if(reg_status & SDIO_FLAG_CMDTMOUT) {
        status = SD_CMD_RESP_TIMEOUT;
        sdio_flag_clear(SDIO, SDIO_FLAG_CMDTMOUT);
        return status;
    } else {
        /* if else end */
    }

    /* check whether the last response command index is the desired one */
    if(sdio_command_index_get(SDIO) != cmdindex) {
        status = SD_ILLEGAL_COMMAND;
        return status;
    }
    /* clear all the CMD_FLAGS flags */
    sdio_flag_clear(SDIO, SDIO_MASK_CMD_FLAGS);
    /* get the SDIO response register 0 for checking */
    response = sdio_response_get(SDIO, SDIO_RESPONSE0);

    if(SD_ALLZERO == (response & (SD_R6_COM_CRC_ERROR | SD_R6_ILLEGAL_COMMAND | SD_R6_GENERAL_UNKNOWN_ERROR))) {
        *prca = (uint16_t)(response >> 16U);
        return status;
    }
    /* if some error occurs, return the error type */
    if(response & SD_R6_COM_CRC_ERROR) {
        status = SD_COM_CRC_ERROR;
    } else if(response & SD_R6_ILLEGAL_COMMAND) {
        status = SD_ILLEGAL_COMMAND;
    } else if(response & SD_R6_GENERAL_UNKNOWN_ERROR) {
        status = SD_GENERAL_UNKNOWN_ERROR;
    } else {
        /* if else end */
    }
    return status;
}


/*!
    \brief      check if error occurs for R7 response
    \param[in]  none
    \param[out] none
    \retval     sd_error_enum
*/
static sd_error_enum r7_error_check(void)
{
    sd_error_enum status = SD_ERROR;
    uint32_t reg_status = 0U, timeout = 100000U;
    rt_tick_t started = rt_tick_get();

    /* store the content of SDIO_STAT */
    reg_status = SDIO_STAT(SDIO);
    while(((reg_status & (SDIO_FLAG_CCRCERR | SDIO_FLAG_CMDTMOUT | SDIO_FLAG_CMDRECV)) == 0U) &&
          (timeout > 0U) && !sd_cmd_timeout_expired(started)) {
        reg_status = SDIO_STAT(SDIO);
        --timeout;
    }

    /* End even a software-timed-out command before the subsequent CMD55.
     * Clear every command flag, including CRC errors from non-v2 cards. */
    sdio_csm_disable(SDIO);
    sdio_flag_clear(SDIO, SDIO_MASK_CMD_FLAGS);
    if((reg_status & SDIO_FLAG_CMDTMOUT) || (0U == timeout) ||
       sd_cmd_timeout_expired(started)) {
        status = SD_CMD_RESP_TIMEOUT;
        return status;
    }
    if(reg_status & SDIO_FLAG_CCRCERR)
        return SD_CMD_CRC_ERROR;
    if(reg_status & SDIO_FLAG_CMDRECV) {
        /* Only an echoed voltage and test pattern identifies an SD v2 card. */
        status = ((sdio_response_get(SDIO, SDIO_RESPONSE0) & 0xFFFU) ==
                  SD_CHECK_PATTERN) ? SD_OK : SD_VOLTRANGE_INVALID;
        return status;
    }
    return status;
}

/*!
    \brief      get the state which the card is in
    \param[in]  none
    \param[out] pcardstate: a pointer that store the card state
      \arg        SD_CARDSTATE_IDLE: card is in idle state
      \arg        SD_CARDSTATE_READY: card is in ready state
      \arg        SD_CARDSTATE_IDENTIFICAT: card is in identificat state
      \arg        SD_CARDSTATE_STANDBY: card is in standby state
      \arg        SD_CARDSTATE_TRANSFER: card is in transfer state
      \arg        SD_CARDSTATE_DATA: card is in data state
      \arg        SD_CARDSTATE_RECEIVING: card is in receiving state
      \arg        SD_CARDSTATE_PROGRAMMING: card is in programming state
      \arg        SD_CARDSTATE_DISCONNECT: card is in disconnect state
      \arg        SD_CARDSTATE_LOCKED: card is in locked state
    \retval     sd_error_enum
*/
static sd_error_enum sd_card_state_get(uint8_t *pcardstate)
{
    uint32_t response;
    sd_error_enum status;
    if(pcardstate == NULL)
        return SD_PARAMETER_INVALID;
    status = sd_cardstatus_get(&response);
    if(status == SD_OK)
        *pcardstate = (uint8_t)((response >> 9U) & 0x0FU);
    return status;
}
/*!
    \brief      stop an ongoing data transfer
    \param[in]  none
    \param[out] none
    \retval     sd_error_enum
*/
sd_error_enum sd_transfer_stop(void)
{
    sd_error_enum status = SD_OK;
    /* send CMD12(STOP_TRANSMISSION) to stop transmission */
    sdio_command_response_config(SDIO, SD_CMD_STOP_TRANSMISSION, (uint32_t)0x0U, SDIO_RESPONSETYPE_SHORT);
    sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
    sdio_trans_stop_enable(SDIO);
    sdio_csm_enable(SDIO);
    /* check if some error occurs */
    status = r1_error_check(SD_CMD_STOP_TRANSMISSION);
    sdio_trans_stop_disable(SDIO);
    return status;
}

/*!
    \brief      lock or unlock a card
    \param[in]  lockstate: the lock state
      \arg        SD_LOCK: lock the SD card
      \arg        SD_UNLOCK: unlock the SD card
    \param[out] none
    \retval     sd_error_enum
*/
sd_error_enum sd_lock_unlock(uint8_t lockstate)
{
    sd_error_enum status = SD_OK;
    uint8_t cardstate = 0U, tempbyte = 0U;
    uint32_t pwd1 = 0U, pwd2 = 0U, response = 0U, timeout = 0U;
    uint16_t tempccc = 0U;

    /* get the card command classes from CSD */
    tempbyte = (uint8_t)((sd_csd[1] & SD_MASK_24_31BITS) >> 24U);
    tempccc = ((uint16_t)tempbyte << 4U);
    tempbyte = (uint8_t)((sd_csd[1] & SD_MASK_16_23BITS) >> 16U);
    tempccc |= (((uint16_t)tempbyte & 0xF0U) >> 4U);

    if(0U == (tempccc & SD_CCC_LOCK_CARD)) {
        /* don't support the lock command */
        status = SD_FUNCTION_UNSUPPORTED;
        return status;
    }
    /* password pattern */
    pwd1 = (0x01020600U | lockstate);
    pwd2 = 0x03040506U;

    /* clear all DSM configuration */
    sdio_data_config(SDIO, 0U, 0U, SDIO_DATABLOCKSIZE_1BYTE);
    sdio_data_transfer_config(SDIO, SDIO_TRANSMODE_BLOCKCOUNT, SDIO_TRANSDIRECTION_TOCARD);
    sdio_dsm_disable(SDIO);
    sdio_idma_disable(SDIO);

    /* send CMD16(SET_BLOCKLEN) to set the block length */
    sdio_command_response_config(SDIO, SD_CMD_SET_BLOCKLEN, (uint32_t)8U, SDIO_RESPONSETYPE_SHORT);
    sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
    sdio_csm_enable(SDIO);
    /* check if some error occurs */
    status = r1_error_check(SD_CMD_SET_BLOCKLEN);
    if(SD_OK != status) {
        return status;
    }

    /* send CMD13(SEND_STATUS), addressed card sends its status register */
    sdio_command_response_config(SDIO, SD_CMD_SEND_STATUS, (uint32_t)sd_rca << SD_RCA_SHIFT, SDIO_RESPONSETYPE_SHORT);
    sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
    sdio_csm_enable(SDIO);
    /* check if some error occurs */
    status = r1_error_check(SD_CMD_SEND_STATUS);
    if(SD_OK != status) {
        return status;
    }

    response = sdio_response_get(SDIO, SDIO_RESPONSE0);
    timeout = SD_BUSY_TIMEOUT_MS;
    while((0U == (response & SD_R1_READY_FOR_DATA)) && (timeout > 0U)) {
        /* continue to send CMD13 to polling the state of card until buffer empty or timeout */
        --timeout;
        rt_thread_mdelay(1);
        /* send CMD13(SEND_STATUS), addressed card sends its status registers */
        sdio_command_response_config(SDIO, SD_CMD_SEND_STATUS, (uint32_t)sd_rca << SD_RCA_SHIFT, SDIO_RESPONSETYPE_SHORT);
        sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
        sdio_csm_enable(SDIO);
        /* check if some error occurs */
        status = r1_error_check(SD_CMD_SEND_STATUS);
        if(SD_OK != status) {
            return status;
        }
        response = sdio_response_get(SDIO, SDIO_RESPONSE0);
    }
    if(0U == timeout) {
        status = SD_ERROR;
        return status;
    }

    /* send CMD42(LOCK_UNLOCK) to set/reset the password or lock/unlock the card */
    sdio_command_response_config(SDIO, SD_CMD_LOCK_UNLOCK, (uint32_t)0x0, SDIO_RESPONSETYPE_SHORT);
    sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
    sdio_csm_enable(SDIO);
    /* check if some error occurs */
    status = r1_error_check(SD_CMD_LOCK_UNLOCK);
    if(SD_OK != status) {
        return status;
    }

    response = sdio_response_get(SDIO, SDIO_RESPONSE0);

    /* configure the SDIO data transmisson */
    sdio_data_config(SDIO, SD_DATATIMEOUT, (uint32_t)8, SDIO_DATABLOCKSIZE_8BYTES);
    sdio_data_transfer_config(SDIO, SDIO_TRANSMODE_BLOCKCOUNT, SDIO_TRANSDIRECTION_TOCARD);
    sdio_dsm_enable(SDIO);

    /* write password pattern */
    sdio_data_write(SDIO, pwd1);
    sdio_data_write(SDIO, pwd2);

    /* whether some error occurs and return it */
    if(RESET != sdio_flag_get(SDIO, SDIO_FLAG_DTCRCERR)) {
        status = SD_DATA_CRC_ERROR;
        sdio_flag_clear(SDIO, SDIO_FLAG_DTCRCERR);
        return status;
    } else if(RESET != sdio_flag_get(SDIO, SDIO_FLAG_DTTMOUT)) {
        status = SD_DATA_TIMEOUT;
        sdio_flag_clear(SDIO, SDIO_FLAG_DTTMOUT);
        return status;
    } else if(RESET != sdio_flag_get(SDIO, SDIO_FLAG_TXURE)) {
        status = SD_TX_UNDERRUN_ERROR;
        sdio_flag_clear(SDIO, SDIO_FLAG_TXURE);
        return status;
    } else {
        /* if else end */
    }

    /* clear the SDIO_INTC flags */
    sdio_flag_clear(SDIO, SDIO_MASK_INTC_FLAGS);
    /* get the card state and wait the card is out of programming and receiving state */
    status = sd_card_state_get(&cardstate);
    timeout = SD_BUSY_TIMEOUT_MS;
    while((SD_OK == status) && ((SD_CARDSTATE_PROGRAMMING == cardstate) || (SD_CARDSTATE_RECEIVING == cardstate))) {
        if(timeout-- == 0U)
            return SD_DATA_TIMEOUT;
        rt_thread_mdelay(1);
        status = sd_card_state_get(&cardstate);
    }
    return status;
}

/*!
    \brief      configure the bus width mode
    \param[in]  buswidth: the bus width
      \arg        SD_BUS_WIDTH_1BIT: 1-bit bus width
      \arg        SD_BUS_WIDTH_4BIT: 4-bit bus width
    \param[out] none
    \retval     sd_error_enum
*/
static sd_error_enum sd_bus_width_config(uint32_t buswidth)
{
    sd_error_enum status = SD_OK;
    /* check whether the card is locked */
    if(sdio_response_get(SDIO, SDIO_RESPONSE0) & SD_CARDSTATE_LOCKED) {
        status = SD_LOCK_UNLOCK_FAILED;
        return status;
    }
    /* get the SCR register */
    status = sd_scr_get(sd_rca, sd_scr);
    if(SD_OK != status) {
        return status;
    }

    if(SD_BUS_WIDTH_1BIT == buswidth) {
        if(SD_ALLZERO != (sd_scr[1] & buswidth)) {
            /* send CMD55(APP_CMD) to indicate next command is application specific command */
            sdio_command_response_config(SDIO, SD_CMD_APP_CMD, (uint32_t)sd_rca << SD_RCA_SHIFT, SDIO_RESPONSETYPE_SHORT);
            sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
            sdio_csm_enable(SDIO);
            /* check if some error occurs */
            status = r1_error_check(SD_CMD_APP_CMD);
            if(SD_OK != status) {
                return status;
            }

            /* send ACMD6(SET_BUS_WIDTH) to define the data bus width */
            sdio_command_response_config(SDIO, SD_APPCMD_SET_BUS_WIDTH, (uint32_t)0x0, SDIO_RESPONSETYPE_SHORT);
            sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
            sdio_csm_enable(SDIO);
            /* check if some error occurs */
            status = r1_error_check(SD_APPCMD_SET_BUS_WIDTH);
            if(SD_OK != status) {
                return status;
            }
        } else {
            status = SD_OPERATION_IMPROPER;
        }
        return status;
    } else if(SD_BUS_WIDTH_4BIT == buswidth) {
        if(SD_ALLZERO != (sd_scr[1] & buswidth)) {
            /* send CMD55(APP_CMD) to indicate next command is application specific command */
            sdio_command_response_config(SDIO, SD_CMD_APP_CMD, (uint32_t)sd_rca << SD_RCA_SHIFT, SDIO_RESPONSETYPE_SHORT);
            sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
            sdio_csm_enable(SDIO);
            /* check if some error occurs */
            status = r1_error_check(SD_CMD_APP_CMD);
            if(SD_OK != status) {
                return status;
            }

            /* send ACMD6(SET_BUS_WIDTH) to define the data bus width */
            sdio_command_response_config(SDIO, SD_APPCMD_SET_BUS_WIDTH, (uint32_t)0x2U, SDIO_RESPONSETYPE_SHORT);
            sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
            sdio_csm_enable(SDIO);
            /* check if some error occurs */
            status = r1_error_check(SD_APPCMD_SET_BUS_WIDTH);
            if(SD_OK != status) {
                return status;
            }
        } else {
            status = SD_OPERATION_IMPROPER;
        }
        return status;
    } else {
        status = SD_PARAMETER_INVALID;
        return status;
    }
}

/*!
    \brief      get the SCR of corresponding card
    \param[in]  rca: RCA of a card
    \param[out] pscr: a pointer that store the SCR content
    \retval     sd_error_enum
*/
static sd_error_enum sd_scr_get(uint16_t rca, uint32_t *pscr)
{
    uint32_t temp_scr[2] = {0, 0};
    sd_error_enum status = SD_OK;
    if(pscr == NULL)
        return SD_PARAMETER_INVALID;
    /* ACMD51 always returns eight bytes, independently of CMD16. In
     * particular, an SDHC card need not accept CMD16 with a length of 8. */
    sd_data_cleanup();

    /* send CMD55(APP_CMD) to indicate next command is application specific command */
    sdio_command_response_config(SDIO, SD_CMD_APP_CMD, (uint32_t)rca << SD_RCA_SHIFT, SDIO_RESPONSETYPE_SHORT);
    sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
    sdio_csm_enable(SDIO);
    /* check if some error occurs */
    status = r1_error_check(SD_CMD_APP_CMD);
    if(SD_OK != status) {
        return status;
    }

    /* configure SDIO data */
    sdio_data_config(SDIO, SD_DATATIMEOUT, (uint32_t)8, SDIO_DATABLOCKSIZE_8BYTES);
    sdio_data_transfer_config(SDIO, SDIO_TRANSMODE_BLOCKCOUNT, SDIO_TRANSDIRECTION_TOSDIO);
    sdio_dsm_enable(SDIO);

    /* send ACMD51(SEND_SCR) to read the SD configuration register */
    sdio_command_response_config(SDIO, SD_APPCMD_SEND_SCR, (uint32_t)0x0, SDIO_RESPONSETYPE_SHORT);
    sdio_wait_type_set(SDIO, SDIO_WAITTYPE_NO);
    sdio_csm_enable(SDIO);
    /* check if some error occurs */
    status = r1_error_check(SD_APPCMD_SEND_SCR);
    if(SD_OK == status)
        status = sd_poll_data(temp_scr, sizeof(temp_scr), SDIO_TRANSDIRECTION_TOSDIO);
    sd_data_cleanup();
    if(SD_OK != status)
        return status;
    /* readjust the temp SCR value */
    *(pscr) = ((temp_scr[1] & SD_MASK_0_7BITS) << 24U) | ((temp_scr[1] & SD_MASK_8_15BITS) << 8U) |
              ((temp_scr[1] & SD_MASK_16_23BITS) >> 8U) | ((temp_scr[1] & SD_MASK_24_31BITS) >> 24U);
    *(pscr + 1U) = ((temp_scr[0] & SD_MASK_0_7BITS) << 24U) | ((temp_scr[0] & SD_MASK_8_15BITS) << 8U) |
                   ((temp_scr[0] & SD_MASK_16_23BITS) >> 8U) | ((temp_scr[0] & SD_MASK_24_31BITS) >> 24U);
    return status;
}

/*!
    \brief      get the data block size
    \param[in]  bytesnumber: the number of bytes
    \param[out] none
    \retval     data block size
      \arg        SDIO_DATABLOCKSIZE_1BYTE: block size = 1 byte
      \arg        SDIO_DATABLOCKSIZE_2BYTES: block size = 2 bytes
      \arg        SDIO_DATABLOCKSIZE_4BYTES: block size = 4 bytes
      \arg        SDIO_DATABLOCKSIZE_8BYTES: block size = 8 bytes
      \arg        SDIO_DATABLOCKSIZE_16BYTES: block size = 16 bytes
      \arg        SDIO_DATABLOCKSIZE_32BYTES: block size = 32 bytes
      \arg        SDIO_DATABLOCKSIZE_64BYTES: block size = 64 bytes
      \arg        SDIO_DATABLOCKSIZE_128BYTES: block size = 128 bytes
      \arg        SDIO_DATABLOCKSIZE_256BYTES: block size = 256 bytes
      \arg        SDIO_DATABLOCKSIZE_512BYTES: block size = 512 bytes
      \arg        SDIO_DATABLOCKSIZE_1024BYTES: block size = 1024 bytes
      \arg        SDIO_DATABLOCKSIZE_2048BYTES: block size = 2048 bytes
      \arg        SDIO_DATABLOCKSIZE_4096BYTES: block size = 4096 bytes
      \arg        SDIO_DATABLOCKSIZE_8192BYTES: block size = 8192 bytes
      \arg        SDIO_DATABLOCKSIZE_16384BYTES: block size = 16384 bytes
*/
static uint32_t sd_datablocksize_get(uint16_t bytesnumber)
{
    uint8_t exp_val = 0U;
    /* calculate the exponent of 2 */
    while(bytesnumber > 1U) {
        bytesnumber >>= 1U;
        ++exp_val;
    }
    return DATACTL_BLKSZ(exp_val);
}

/*!
    \brief      configure the GPIO of SDIO interface
    \param[in]  none
    \param[out] none
    \retval     none
*/
static void gpio_config(void)
{
    /*
     * The LCD uses PC11 as LCD_B4 on the 29_TLI_Touch_Draw wiring.  Keep
     * the card in 1-bit mode and never switch PC9/PC10/PC11 to SDIO AF12.
     * SDIO0 therefore uses only DAT0(PB13), CLK(PC12), and CMD(PD2).
     */
    gpio_af_set(GPIOB, GPIO_AF_12, GPIO_PIN_13);
    gpio_af_set(GPIOC, GPIO_AF_12, GPIO_PIN_12);
    gpio_af_set(GPIOD, GPIO_AF_12, GPIO_PIN_2);

    gpio_mode_set(GPIOB, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_13);
    gpio_output_options_set(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_100_220MHZ, GPIO_PIN_13);

    gpio_mode_set(GPIOC, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_12);
    gpio_output_options_set(GPIOC, GPIO_OTYPE_PP, GPIO_OSPEED_100_220MHZ, GPIO_PIN_12);

    gpio_mode_set(GPIOD, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_2);
    gpio_output_options_set(GPIOD, GPIO_OTYPE_PP, GPIO_OSPEED_100_220MHZ, GPIO_PIN_2);
}

/*!
    \brief      configure the RCU of SDIO
    \param[in]  none
    \param[out] none
    \retval     none
*/
static sd_error_enum rcu_config(void)
{
    /* PLL1 is dedicated to SDIO in this BSP; LCD uses PLL2. A card retry
     * must not reprogram PLL1 while the oscillator is running. */
    if((RCU_CTL & RCU_CTL_PLL1EN) == 0U) {
        rcu_pll_input_output_clock_range_config(IDX_PLL1, RCU_PLL1RNG_4M_8M, RCU_PLL1VCO_192M_836M);
        /* HXTAL_VALUE / 5 * 40 / 1 = 200 MHz. */
        if(ERROR == rcu_pll1_config(5, 40, 1, 1, 1))
            return SD_ERROR;
    }
    /* enable PLL1R clock output */
    rcu_pll_clock_output_enable(RCU_PLL1R);
    /* enable PLL1 clock */
    rcu_osci_on(RCU_PLL1_CK);

    if(ERROR == rcu_osci_stab_wait(RCU_PLL1_CK)) {
        return SD_ERROR;
    }

    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_GPIOC);
    rcu_periph_clock_enable(RCU_GPIOD);
    rcu_sdio_clock_config(IDX_SDIO0, RCU_SDIO0SRC_PLL1R);
    rcu_periph_clock_enable(RCU_SDIO0);
    return SD_OK;
}

/*!
    \brief      configure the IDMA
    \param[in]  srcbuf: a pointer point to a buffer which will be transferred
    \param[in]  bufsize: the size of buffer(not used in flow controller is peripheral)
    \param[out] none
    \retval     none
*/
/* DMA is deliberately unavailable in this polling-only board port. */
