/*
 * SPECTER CLI — persistent preferences in STM32 flash.
 *
 * Storage: the last 1 KB flash page of the STM32F103C6/C8 (128 KB device),
 * 0x0801F000, which the ~30-50 KB firmware image never reaches.
 *
 * Layout: 18 × 32-bit words (72 B), written as whole words (STM32 F1
 * requirement). Validity = magic + CRC16. `erase` wipes the page; a page
 * without a valid record simply yields build-time defaults on next boot.
 *
 * Note: page erase takes up to ~1 s. The CPU stalls (cannot fetch from
 * flash while the flash is busy), so interrupts are disabled for the
 * whole write; the 8 s IWDG budget is not a problem.
 */
#include "config.h"

#include <Arduino.h>
#include <string.h>
#include "stm32f1xx_hal.h"   // shipped with the arduino-ststm32 framework

#define PREFS_ADDR    0x0801F000UL
#define PREFS_WORDS   18

#define PREFS_MAGIC   0x53505452UL   // "Sptr"
#define PREFS_VER     1UL

// AdminConfig record (separate, in the same page) — words [20..35]:
//  [20] magic   [21] version
//  [22..27] admin_password (24 B)
//  [28..33] guest_password (24 B)
//  [34] admin_timeout (low 16 b) + pad
//  [35] CRC16-CCITT over words [20..34]
#define ADMIN_MAGIC   0x41444D4EUL   // "AdmN"
#define ADMIN_VER     1UL
#define ADMIN_OFF     20             // word offset of the admin record
#define ADMIN_CRCN    15             // words covered by the admin CRC ([20..34])

// 18 words:
//  [0] magic   [1] version
//  [2..16] NodeConfig payload (60 B, zero-padded to 64)
//  [17] CRC16-CCITT over words [0..16] (low word)

static uint16_t crc16_ccitt(const uint8_t* data, uint32_t len) {
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// ─── Runtime config instance (defined here, shared via config.h) ────────────
NodeConfig cfg;
AdminConfig adm;

static void admin_defaults(void) {
    memset(&adm, 0, sizeof(adm));   // empty passwords = mesh admin disabled
    // Compile-time default admin password (build flag -D ADMIN_PASSWORD="...").
    // Seeded like NODE_LAT_I/NODE_LON_I; a CLI "set admin.password" (or clearing it)
    // overrides this in flash, including disabling admin entirely.
    strncpy(adm.admin_password, ADMIN_PASSWORD, sizeof(adm.admin_password) - 1);
    adm.admin_password[sizeof(adm.admin_password) - 1] = '\0';
    adm.admin_timeout = DEF_ADMIN_TIMEOUT;
}

static void config_defaults(void) {
    memset(&cfg, 0, sizeof(cfg));
    // Build-flag overrides (platformio.ini: NODE_NAME_STR, NODE_LAT_I, NODE_LON_I)
#ifdef NODE_NAME_STR
    snprintf(cfg.name, sizeof(cfg.name), "%s", NODE_NAME_STR);
#else
    snprintf(cfg.name, sizeof(cfg.name), "%s", DEF_NAME);
#endif
#ifdef NODE_LAT_I
    cfg.lat_micro = (int32_t)(NODE_LAT_I) * 1000;   // deg*1e4 → deg*1e6
#endif
#ifdef NODE_LON_I
    cfg.lon_micro = (int32_t)(NODE_LON_I) * 1000;
#endif
    cfg.freq_khz        = DEF_FREQ_KHZ;
    cfg.bw_tenth        = DEF_BW_TENTH;
    cfg.sf              = DEF_SF;
    cfg.cr              = DEF_CR;
    cfg.tx_dbm          = DEF_TX_DBM;
    cfg.flood_advert_h  = DEF_FLOOD_ADV_H;
    cfg.zerohop_adv_min = DEF_ZERO_ADV_M;
    cfg.repeat_en       = 1;
    cfg.loop_detect     = DEF_LOOP_DETECT;
    cfg.txdelay_x10     = DEF_TXDELAY_X10;
    cfg.direct_txdelay_x10 = DEF_DIRTX_X10;
    cfg.dutycycle       = DEF_DUTY_CYCLE;
    cfg.flood_max       = DEF_FLOOD_MAX;
}

static bool flash_write_words(uint32_t addr, const uint32_t* data, int n) {
    uint32_t err;
    __disable_irq();
    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGERR);
    FLASH_EraseInitTypeDef e = {};
    e.TypeErase     = FLASH_TYPEERASE_PAGES;
    e.PageAddress   = addr;
    e.NbPages       = 1;
    bool ok = (HAL_FLASHEx_Erase(&e, &err) == HAL_OK);
    if (ok) {
        for (int i = 0; i < n; i++) {
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + 4UL * (uint32_t)i,
                                  data[i]) != HAL_OK) { ok = false; break; }
        }
    }
    HAL_FLASH_Lock();
    __enable_irq();
    return ok;
}

void prefs_save(void) {
    uint32_t w[ADMIN_OFF + ADMIN_CRCN + 1] = {0};   // words [0..35]
    // NodeConfig record ([0..17])
    w[0] = PREFS_MAGIC;
    w[1] = PREFS_VER;
    memcpy(w + 2, &cfg, sizeof(NodeConfig));   // 60 B → words 2..16
    w[17] = crc16_ccitt((const uint8_t*)w, 17 * 4);
    // AdminConfig record ([20..35])
    w[ADMIN_OFF]     = ADMIN_MAGIC;
    w[ADMIN_OFF + 1] = ADMIN_VER;
    memcpy(w + ADMIN_OFF + 2, adm.admin_password, 24);
    memcpy(w + ADMIN_OFF + 8, adm.guest_password, 24);
    w[ADMIN_OFF + 14] = adm.admin_timeout;      // low 16 b; high = pad
    w[ADMIN_OFF + 15] = crc16_ccitt((const uint8_t*)(w + ADMIN_OFF), ADMIN_CRCN * 4);
    flash_write_words(PREFS_ADDR, w, ADMIN_OFF + ADMIN_CRCN + 1);
}

void prefs_erase(void) {
    uint32_t err;
    __disable_irq();
    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGERR);
    FLASH_EraseInitTypeDef e = {};
    e.TypeErase     = FLASH_TYPEERASE_PAGES;
    e.PageAddress   = PREFS_ADDR;
    e.NbPages       = 1;
    HAL_FLASHEx_Erase(&e, &err);
    HAL_FLASH_Lock();
    __enable_irq();
}

void prefs_load(void) {
    config_defaults();
    admin_defaults();
    const uint32_t* src = (const uint32_t*)PREFS_ADDR;
    uint32_t w[ADMIN_OFF + ADMIN_CRCN + 1];
    for (int i = 0; i < (int)sizeof(w)/4; i++) w[i] = src[i];   // erased = 0xFFFFFFFF

    // NodeConfig record ([0..17]) — independent of the admin record
    if (w[0] == PREFS_MAGIC && w[1] == PREFS_VER &&
        w[17] == crc16_ccitt((const uint8_t*)w, 17 * 4)) {
        NodeConfig stored;
        memcpy(&stored, w + 2, sizeof(NodeConfig));
        stored.name[sizeof(stored.name) - 1] = '\0';
        cfg = stored;
    }

    // AdminConfig record ([20..35]) — absent/old → keep admin_defaults()
    if (w[ADMIN_OFF] == ADMIN_MAGIC && w[ADMIN_OFF + 1] == ADMIN_VER &&
        w[ADMIN_OFF + 15] == crc16_ccitt((const uint8_t*)(w + ADMIN_OFF), ADMIN_CRCN * 4)) {
        memcpy(adm.admin_password, w + ADMIN_OFF + 2, 24); adm.admin_password[23] = '\0';
        memcpy(adm.guest_password, w + ADMIN_OFF + 8, 24); adm.guest_password[23] = '\0';
        adm.admin_timeout = (uint16_t)(w[ADMIN_OFF + 14] & 0xFFFFUL);
    }
}
