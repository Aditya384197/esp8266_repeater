/* RF calibration / init data handling (required by the SDK). */
#include "stdint.h"
#include "c_types.h"
#include "version.h"
#include "user_interface.h"
#include "spi_flash.h"
#include "osapi.h"
#include "user_config.h"

// For versions ESP8266_NONOS_SDK v1.5.2 to v2.2.1, user_rf_cal_sector_set() need to be added.
// Docker SDK comes with a user_rf_cal_sector_set() in libmain.a, needed for official SDKs.
#if USER_RF_CAL
uint32 ICACHE_FLASH_ATTR user_rf_cal_sector_set(void)
{
    enum flash_size_map size_map = system_get_flash_size_map();
    uint32 rf_cal_sec = 0;

    switch (size_map)
    {
    case FLASH_SIZE_4M_MAP_256_256:
        rf_cal_sec = 128 - 5;
        break;
    case FLASH_SIZE_8M_MAP_512_512:
        rf_cal_sec = 256 - 5;
        break;
    case FLASH_SIZE_16M_MAP_512_512:
    case FLASH_SIZE_16M_MAP_1024_1024:
        rf_cal_sec = 512 - 5;
        break;
    case FLASH_SIZE_32M_MAP_512_512:
    case FLASH_SIZE_32M_MAP_1024_1024:
        rf_cal_sec = 1024 - 5;
        break;
    case FLASH_SIZE_64M_MAP_1024_1024:
        rf_cal_sec = 2048 - 5;
        break;
    case FLASH_SIZE_128M_MAP_1024_1024:
        rf_cal_sec = 4096 - 5;
        break;
    default:
        rf_cal_sec = 0;
        break;
    }
    return rf_cal_sec;
}
#endif

const uint8_t esp_init_data_default[] = {
    "\x05\x08\x04\x02\x05\x05\x05\x02\x05\x00\x04\x05\x05\x04\x05\x05"
    "\x04\xFE\xFD\xFF\xF0\xF0\xF0\xE0\xE0\xE0\xE1\x0A\xFF\xFF\xF8\x00"
    "\xF8\xF8\x4E\x4A\x46\x40\x3C\x38\x00\x00\x01\x01\x02\x03\x04\x05"
    "\x01\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\xE1\x0A\x00\x00\x00\x00\x00\x00\x00\x00\x01\x93\x43\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xFF\x00\x00\x00\x00"
    "\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"};

void user_rf_pre_init()
{
    uint8_t esp_init_data_current[sizeof(esp_init_data_default)];
    enum flash_size_map size_map = system_get_flash_size_map();
    uint32 rf_cal_sec = 0, addr, i;

    rf_cal_sec = user_rf_cal_sector_set();
    addr = ((rf_cal_sec)*SPI_FLASH_SEC_SIZE) + SPI_FLASH_SEC_SIZE;
    spi_flash_read(addr, (uint32_t *)esp_init_data_current, sizeof(esp_init_data_current));

    for (i = 0; i < sizeof(esp_init_data_default); i++)
    {
        if (esp_init_data_current[i] != esp_init_data_default[i])
        {
            spi_flash_erase_sector(rf_cal_sec);
            spi_flash_erase_sector(rf_cal_sec + 1);
            spi_flash_erase_sector(rf_cal_sec + 2);
            addr = ((rf_cal_sec)*SPI_FLASH_SEC_SIZE) + SPI_FLASH_SEC_SIZE;
            os_printf("Writing rfcal init data @0x%08X\n", addr);
            spi_flash_write(addr, (uint32 *)esp_init_data_default, sizeof(esp_init_data_default));

            break;
        }
        /*
        else
        {
            os_printf("RF data[%u] is ok\n", i);
        }
        */
    }
}

#if ESP_SDK_VERSION_NUMBER >= 0x030000

// user_pre_init is required from SDK v3.0.0 onwards
// It is used to register the parition map with the SDK, primarily to allow
// the app to use the SDK's OTA capability.  We don't make use of that in
// otb-iot and therefore the only info we provide is the mandatory stuff:
// - RF calibration data
// - Physical data
// - System parameter
// The location and length of these are from the 2A SDK getting started guide
void ICACHE_FLASH_ATTR user_pre_init(void)
{
    bool rc = false;
    static const partition_item_t part_table[] =
    {
        {SYSTEM_PARTITION_RF_CAL,
        0x3fb000,
        0x1000},
        {SYSTEM_PARTITION_PHY_DATA,
        0x3fc000,
        0x1000},
        {SYSTEM_PARTITION_SYSTEM_PARAMETER,
        0x3fd000,
        0x3000},
    };

    // This isn't an ideal approach but there's not much point moving on unless
    // or until this has succeeded cos otherwise the SDK will just barf and
    // refuse to call user_init()
    while (!rc)
    {
        rc = system_partition_table_regist(part_table,
            sizeof(part_table) / sizeof(part_table[0]), 4);
    }

    // check and update esp_init_data
    user_rf_pre_init();

    return;
}
#endif
