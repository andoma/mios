#pragma once

#define DMI_BIOS_VENDOR           0x0000
#define DMI_BIOS_VERSION          0x0001
#define DMI_BIOS_RELEASE_DATE     0x0002

#define DMI_PRODUCT_MANUFACTURER  0x1000
#define DMI_PRODUCT_PRODUCT_NAME  0x1001
#define DMI_PRODUCT_VERSION       0x1002
#define DMI_PRODUCT_SERIAL        0x1003
#define DMI_PRODUCT_SKU           0x1004
#define DMI_PRODUCT_FAMILY        0x1005

#define DMI_BB_MANUFACTURER       0x2000
#define DMI_BB_PRODUCT            0x2001
#define DMI_BB_VERSION            0x2002
#define DMI_BB_SERIAL             0x2003
#define DMI_BB_ASSET_TAG          0x2004

#define DMI_CHASSI_MANUFACTURER   0x3000
#define DMI_CHASSI_VERSION        0x3001
#define DMI_CHASSI_SERIAL         0x3002
#define DMI_CHASSI_ASSET_TAG      0x3003


const char *dmi_get_str(int id);
