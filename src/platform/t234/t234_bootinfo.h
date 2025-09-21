#pragma once

#include <stdint.h>

#include "reg.h"

#define PMC_RESET_REASON_REGISTER        0x0c360070

#define SCRATCH_BLINFO_LOCATION_REGISTER 0x0c390154
#define SCRATCH_BOOTLOADER_REGISTER      0x0c3903a4
#define SCRATCH_ROOTFS_REGISTER          0x0c3903a8
#define SCRATCH_BOOT_CHAIN_REGISTER      0x0c3903cc

typedef union {
  uint32_t version;
  uint8_t version_str[128];
} tegrabl_version_t;

typedef struct  {
  uint8_t cvm[256];
  uint8_t cvb[256];
  uint32_t cvm_size;
  uint32_t cvb_size;
} tegrabl_eeprom_data_t;

typedef struct {
  void *base;
  uint64_t size;
  uint64_t flags;
} tegrabl_carveout_info_t;

#define CARVEOUT_NONE                       0
#define CARVEOUT_NVDEC                      1
#define CARVEOUT_WPR1                       2
#define CARVEOUT_WPR2                       3
#define CARVEOUT_TSEC                       4
#define CARVEOUT_XUSB                       5
#define CARVEOUT_BPMP                       6
#define CARVEOUT_APE                        7
#define CARVEOUT_SPE                        8
#define CARVEOUT_SCE                        9
#define CARVEOUT_APR                        10
#define CARVEOUT_BPMP_DCE                   11
#define CARVEOUT_UNUSED3                    12
#define CARVEOUT_BPMP_RCE                   13
#define CARVEOUT_BPMP_MCE                   14
#define CARVEOUT_ETR                        15
#define CARVEOUT_BPMP_SPE                   16
#define CARVEOUT_RCE                        17
#define CARVEOUT_BPMP_CPUTZ                 18
#define CARVEOUT_PVA_FW                     19
#define CARVEOUT_DCE                        20
#define CARVEOUT_BPMP_PSC                   21
#define CARVEOUT_PSC                        22
#define CARVEOUT_NV_SC7                     23
#define CARVEOUT_CAMERA_TASKLIST            24
#define CARVEOUT_BPMP_SCE                   25
#define CARVEOUT_CV_GOS                     26
#define CARVEOUT_PSC_TSEC                   27
#define CARVEOUT_CCPLEX_INTERWORLD_SHMEM    28
#define CARVEOUT_FSI                        29
#define CARVEOUT_MCE                        30
#define CARVEOUT_CCPLEX_IST                 31
#define CARVEOUT_TSEC_HOST1X                32
#define CARVEOUT_PSC_TZ                     33
#define CARVEOUT_SCE_CPU_NS                 34
#define CARVEOUT_OEM_SC7                    35
#define CARVEOUT_SYNCPT_IGPU_RO             36
#define CARVEOUT_SYNCPT_IGPU_NA             37
#define CARVEOUT_VM_ENCRYPT                 38
#define CARVEOUT_BLANKET_NSDRAM             CARVEOUT_VM_ENCRYPT
#define CARVEOUT_CCPLEX_SMMU_PTW            39
#define CARVEOUT_DISP_EARLY_BOOT_FB         CARVEOUT_CCPLEX_SMMU_PTW
#define CARVEOUT_BPMP_CPU_NS                40
#define CARVEOUT_FSI_CPU_NS                 41
#define CARVEOUT_TSEC_DCE                   42
#define CARVEOUT_TZDRAM                     43
#define CARVEOUT_VPR                        44
#define CARVEOUT_MTS                        45
#define CARVEOUT_RCM_BLOB                   46
#define CARVEOUT_UEFI                       47
#define CARVEOUT_UEFI_MM_IPC                48
#define CARVEOUT_DRAM_ECC_TEST              49
#define CARVEOUT_PROFILING                  50
#define CARVEOUT_OS                         51
#define CARVEOUT_FSI_KEY_BLOB               52
#define CARVEOUT_TEMP_MB2RF                 53
#define CARVEOUT_TEMP_MB2_LOAD              54
#define CARVEOUT_TEMP_MB2_PARAMS            55
#define CARVEOUT_TEMP_MB2_IO_BUFFERS        56
#define CARVEOUT_TEMP_MB2RF_DATA            57
#define CARVEOUT_TEMP_MB2                   58
#define CARVEOUT_TEMP_MB2_SYSRAM_DATA       59
#define CARVEOUT_TSEC_CCPLEX                60
#define CARVEOUT_TEMP_MB2_APLT_LOAD         61
#define CARVEOUT_TEMP_MB2_APLT_PARAMS       62
#define CARVEOUT_TEMP_MB2_APLT_IO_BUFFERS   63
#define CARVEOUT_TEMP_MB2_APLT_SYSRAM_DATA  64
#define CARVEOUT_GR                         65
#define CARVEOUT_TEMP_QB_DATA               66
#define CARVEOUT_TEMP_QB_IO_BUFFER          67
#define CARVEOUT_ATF_FSI                    68
#define CARVEOUT_OPTEE_DTB                  69
#define CARVEOUT_UNUSED2                    70
#define CARVEOUT_UNUSED4                    71
#define CARVEOUT_RAM_OOPS                   72
#define CARVEOUT_OEM_COUNT                  73


typedef struct cpubl_params_v2 {
  uint8_t sha512_digest[64];

  uint32_t version;

  uint32_t uart_instance;

  uint32_t secure_os;

  uint32_t boot_type;

  uint32_t reserved1;

  uint32_t reserved2;

  uint64_t feature_flags;

  uint64_t sdram_base;

  uint64_t sdram_size;

  tegrabl_version_t mb1bct;

  tegrabl_version_t mb1;

  tegrabl_version_t mb2;

  tegrabl_eeprom_data_t eeprom __attribute__((aligned(8)));

  uint32_t boot_chain_selection_mode;

  uint32_t non_gpio_select_boot_chain;

  uint8_t brbct_custom_data[2048] __attribute__((aligned(8)));

  uint64_t dram_page_retirement_info_address;

  uint64_t reserved3; // Start address of hvinfo page

  uint64_t reserved4; // Start address of PVIT page

  uint32_t reserved5;
  uint32_t reserved6;

  uint8_t min_ratchet_level[304]  __attribute__((aligned(8)));

  tegrabl_carveout_info_t carveout_info[CARVEOUT_OEM_COUNT] __attribute__((aligned(8)));

} cpubl_params_v2_t;



struct rcmblob_header {
  uint8_t magic[4];
  uint8_t zero[4];
  uint8_t hash[64];
  uint32_t random_value;
  uint32_t num_items;

  struct {
    uint32_t type;
    uint32_t location;
    uint32_t zero;
    uint32_t length;
  } items[64];
};


static inline void *
get_carveout_base(int id)
{
  const cpubl_params_v2_t *cbp =
    (const void *)reg_rd64(SCRATCH_BLINFO_LOCATION_REGISTER);

  return (void *)cbp->carveout_info[id].base;
}
