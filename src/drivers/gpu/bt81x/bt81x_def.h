#pragma once

#define EVE_CHIP_ID_ADDRESS             (0x000C0000UL) // = 786432UL

#define EVE_CMD_ACTIVE 0x00
#define EVE_CMD_CLKEXT 0x44
#define EVE_CMD_CLKSEL 0x61



// My defines for the BT8xx PINDRIVE registers and settings
// Byte 2, bits 1:0 specifies the drive level
#define EVE_PINDRIVE_HR_5mA             (0x00)
#define EVE_PINDRIVE_HR_10mA            (0x01)
#define EVE_PINDRIVE_HR_15mA            (0x02)
#define EVE_PINDRIVE_HR_20mA            (0x03)
#define EVE_PINDRIVE_LR_1p2mA           (0x00)
#define EVE_PINDRIVE_LR_2p4mA           (0x01)
#define EVE_PINDRIVE_LR_3p6mA           (0x02)
#define EVE_PINDRIVE_LR_4p8mA           (0x03)
// Byte 2, bits 7:2 specifies the pin(s)
#define EVE_PINDRIVE_GPIO_0_HR          (0x00) // 0x00 << 2 =  0 * 4 =   0
#define EVE_PINDRIVE_GPIO_1_HR          (0x04) // 0x01 << 2 =  1 * 4 =   4
#define EVE_PINDRIVE_GPIO_2_HR          (0x08) // 0x02 << 2 =  2 * 4 =   8
#define EVE_PINDRIVE_GPIO_3_HR          (0x0C) // 0x03 << 2 =  3 * 4 =  12
#define EVE_PINDRIVE_DISP_LR            (0x20) // 0x08 << 2 =  8 * 4 =  32
#define EVE_PINDRIVE_DE_LR              (0x24) // 0x09 << 2 =  9 * 4 =  36
#define EVE_PINDRIVE_VSYNC_HSYNC_LR     (0x28) // 0x0A << 2 = 10 * 4 =  40
#define EVE_PINDRIVE_PCLK_LR            (0x2C) // 0x0B << 2 = 11 * 4 =  44
#define EVE_PINDRIVE_BACKLIGHT_LR       (0x30) // 0x0C << 2 = 12 * 4 =  48
#define EVE_PINDRIVE_RGB_LR             (0x34) // 0x0D << 2 = 13 * 4 =  52
#define EVE_PINDRIVE_AUDIO_L_HR         (0x38) // 0x0E << 2 = 14 * 4 =  56
#define EVE_PINDRIVE_INT_N_HR           (0x3C) // 0x0F << 2 = 15 * 4 =  60
#define EVE_PINDRIVE_CTP_RST_N_HR       (0x40) // 0x10 << 2 = 16 * 4 =  64
#define EVE_PINDRIVE_CTP_SCL_HR         (0x44) // 0x11 << 2 = 17 * 4 =  68
#define EVE_PINDRIVE_CTP_SDA_HR         (0x48) // 0x12 << 2 = 18 * 4 =  72
#define EVE_PINDRIVE_SPI_DATA_HR        (0x4C) // 0x13 << 2 = 19 * 4 =  76
#define EVE_PINDRIVE_SPIM_SCLK_HR       (0x50) // 0x14 << 2 = 20 * 4 =  80
#define EVE_PINDRIVE_SPIM_SS_N_HR       (0x54) // 0x15 << 2 = 21 * 4 =  84
#define EVE_PINDRIVE_SPIM_MISO_HR       (0x58) // 0x16 << 2 = 22 * 4 =  88
#define EVE_PINDRIVE_SPIM_MOSI_HR       (0x5C) // 0x17 << 2 = 23 * 4 =  92
#define EVE_PINDRIVE_SPIM_IO2_HR        (0x60) // 0x18 << 2 = 24 * 4 =  96
#define EVE_PINDRIVE_SPIM_IO3_HR        (0x64) // 0x19 << 2 = 25 * 4 = 100


#define EVE_LOW_FREQ_BOUND              (58800000L)    //98% of 60Mhz

#define EVE_RAM_G_SIZE                  (1024*1024L)
#define EVE_RAM_BIST                    (0x00380000UL) // = 3670016UL
#define EVE_RAM_CMD                     (0x00308000UL) // = 3178496UL
#define EVE_RAM_DL                      (0x00300000UL) // = 3145728UL
#define EVE_RAM_G                       (0x00000000UL) // = 0UL
#define EVE_RAM_REG                     (0x00302000UL) // = 3153920UL
#define EVE_RAM_ROMSUB                  (0x0030A000UL) // = 3186688UL
#define EVE_RAM_TOP                     (0x00304000UL) // = 3162112UL
#define EVE_CMDBUF_SIZE                 (0x00001000UL) // = 4096UL
#define EVE_RAM_CMD_SIZE                (4*1024L)
#define EVE_RAM_ERR_REPORT              (0x00309800UL) // = 3184640UL
#define ROMFONT_TABLEADDRESS            (0x002FFFFCUL) // = 3145724UL
#define EVE_REG_ADAPTIVE_FRAMERATE      (0x0030257CUL) // = 3155324UL
#define EVE_REG_AH_HCYCLE_MAX           (0x00302610UL) // = 3155472UL
#define EVE_REG_ANALOG                  (0x0030216CUL) // = 3154284UL
#define EVE_REG_ANA_COMP                (0x00302184UL) // = 3154308UL
#define EVE_REG_ANIM_ACTIVE             (0x0030902CUL) // = 3182636UL
#define EVE_REG_BIST_EN                 (0x00302174UL) // = 3154292UL
#define EVE_REG_BUSYBITS                (0x003020E8UL) // = 3154152UL
#define EVE_REG_CLOCK                   (0x00302008UL) // = 3153928UL
#define EVE_REG_CMDB_SPACE              (0x00302574UL) // = 3155316UL
#define EVE_REG_CMDB_WRITE              (0x00302578UL) // = 3155320UL
#define EVE_REG_CMD_DL                  (0x00302100UL) // = 3154176UL
#define EVE_REG_CMD_READ                (0x003020F8UL) // = 3154168UL
#define EVE_REG_CMD_WRITE               (0x003020FCUL) // = 3154172UL
#define EVE_REG_CPURESET                (0x00302020UL) // = 3153952UL
#define EVE_REG_CRC                     (0x00302178UL) // = 3154296UL
#define EVE_REG_CSPREAD                 (0x00302068UL) // = 3154024UL
#define EVE_REG_CTOUCH_EXTENDED         (0x00302108UL) // = 3154184UL
#define EVE_REG_CTOUCH_TOUCH0_XY        (0x00302124UL) // = 3154212UL
#define EVE_REG_CTOUCH_TOUCH4_X         (0x0030216CUL) // = 3154284UL
#define EVE_REG_CTOUCH_TOUCH4_Y         (0x00302120UL) // = 3154208UL
#define EVE_REG_CTOUCH_TOUCH1_XY        (0x0030211CUL) // = 3154204UL
#define EVE_REG_CTOUCH_TOUCH2_XY        (0x0030218CUL) // = 3154316UL
#define EVE_REG_CTOUCH_TOUCH3_XY        (0x00302190UL) // = 3154320UL
#define EVE_REG_TOUCH_CONFIG            (0x00302168UL) // = 3154280UL
#define EVE_REG_DATESTAMP               (0x00302564UL) // = 3155300UL
#define EVE_REG_DF_TUNED                (0x00309030UL) // = 3182640UL
#define EVE_REG_DITHER                  (0x00302060UL) // = 3154016UL
#define EVE_REG_DLSWAP                  (0x00302054UL) // = 3154004UL
#define EVE_REG_EHOST_TOUCH_ACK         (0x00302170UL) // = 3154288UL
#define EVE_REG_EHOST_TOUCH_ID          (0x00302114UL) // = 3154196UL
#define EVE_REG_EHOST_TOUCH_X           (0x0030210CUL) // = 3154188UL
#define EVE_REG_EHOST_TOUCH_Y           (0x00302118UL) // = 3154200UL
#define EVE_REG_EJPG_ACC                (0x00302358UL) // = 3154776UL
#define EVE_REG_EJPG_BUSY               (0x00302198UL) // = 3154328UL
#define EVE_REG_EJPG_DAT                (0x0030219CUL) // = 3154332UL
#define EVE_REG_EJPG_DCC                (0x00302340UL) // = 3154752UL
#define EVE_REG_EJPG_DEBUG              (0x0030255CUL) // = 3155292UL
#define EVE_REG_EJPG_DST                (0x003021A4UL) // = 3154340UL
#define EVE_REG_EJPG_FORMAT             (0x003021B0UL) // = 3154352UL
#define EVE_REG_EJPG_H                  (0x003021ACUL) // = 3154348UL
#define EVE_REG_EJPG_HT                 (0x00302240UL) // = 3154496UL
#define EVE_REG_EJPG_OPTIONS            (0x003021A0UL) // = 3154336UL
#define EVE_REG_EJPG_Q                  (0x003021C0UL) // = 3154368UL
#define EVE_REG_EJPG_READY              (0x00302194UL) // = 3154324UL
#define EVE_REG_EJPG_RI                 (0x003021B4UL) // = 3154356UL
#define EVE_REG_EJPG_SCALE              (0x00302558UL) // = 3155288UL
#define EVE_REG_EJPG_TDA                (0x003021BCUL) // = 3154364UL
#define EVE_REG_EJPG_TQ                 (0x003021B8UL) // = 3154360UL
#define EVE_REG_EJPG_W                  (0x003021A8UL) // = 3154344UL
#define EVE_REG_ESPIM_ADD               (0x0030259CUL) // = 3155356UL
#define EVE_REG_ESPIM_COUNT             (0x003025A0UL) // = 3155360UL
#define EVE_REG_ESPIM_DUMMY             (0x003025E4UL) // = 3155428UL
#define EVE_REG_ESPIM_READSTART         (0x00302588UL) // = 3155336UL
#define EVE_REG_ESPIM_SEQ               (0x0030258CUL) // = 3155340UL
#define EVE_REG_ESPIM_TRIG              (0x003025E8UL) // = 3155432UL
#define EVE_REG_ESPIM_WINDOW            (0x003025A4UL) // = 3155364UL
#define EVE_REG_FLASH_SIZE              (0x00309024UL) // = 3182628UL
#define EVE_REG_FLASH_STATUS            (0x003025F0UL) // = 3155440UL
#define EVE_REG_FRAMES                  (0x00302004UL) // = 3153924UL
#define EVE_REG_FREQUENCY               (0x0030200CUL) // = 3153932UL
#define EVE_REG_FULLBUSYBITS            (0x003025F4UL) // = 3155444UL
#define EVE_REG_GPIO                    (0x00302094UL) // = 3154068UL
#define EVE_REG_GPIOX                   (0x0030209CUL) // = 3154076UL
#define EVE_REG_GPIOX_DIR               (0x00302098UL) // = 3154072UL
#define EVE_REG_GPIO_DIR                (0x00302090UL) // = 3154064UL
#define EVE_REG_HCYCLE                  (0x0030202CUL) // = 3153964UL
#define EVE_REG_HOFFSET                 (0x00302030UL) // = 3153968UL
#define EVE_REG_HSIZE                   (0x00302034UL) // = 3153972UL
#define EVE_REG_HSYNC0                  (0x00302038UL) // = 3153976UL
#define EVE_REG_HSYNC1                  (0x0030203CUL) // = 3153980UL
#define EVE_REG_ID                      (0x00302000UL) // = 3153920UL
#define EVE_REG_INT_EN                  (0x003020ACUL) // = 3154092UL
#define EVE_REG_INT_FLAGS               (0x003020A8UL) // = 3154088UL
#define EVE_REG_INT_MASK                (0x003020B0UL) // = 3154096UL
#define EVE_REG_MACRO_0                 (0x003020D8UL) // = 3154136UL
#define EVE_REG_MACRO_1                 (0x003020DCUL) // = 3154140UL
#define EVE_REG_MEDIAFIFO_BASE          (0x0030901CUL) // = 3182620UL
#define EVE_REG_MEDIAFIFO_READ          (0x00309014UL) // = 3182612UL
#define EVE_REG_MEDIAFIFO_SIZE          (0x00309020UL) // = 3182624UL
#define EVE_REG_MEDIAFIFO_WRITE         (0x00309018UL) // = 3182616UL
#define EVE_REG_OUTBITS                 (0x0030205CUL) // = 3154012UL
#define EVE_REG_PCLK                    (0x00302070UL) // = 3154032UL
#define EVE_REG_PCLK_POL                (0x0030206CUL) // = 3154028UL
#define EVE_REG_PLAY                    (0x0030208CUL) // = 3154060UL
#define EVE_REG_PLAYBACK_FORMAT         (0x003020C4UL) // = 3154116UL
#define EVE_REG_PLAYBACK_FREQ           (0x003020C0UL) // = 3154112UL
#define EVE_REG_PLAYBACK_LENGTH         (0x003020B8UL) // = 3154104UL
#define EVE_REG_PLAYBACK_LOOP           (0x003020C8UL) // = 3154120UL
#define EVE_REG_PLAYBACK_PAUSE          (0x003025ECUL) // = 3155436UL
#define EVE_REG_PLAYBACK_PLAY           (0x003020CCUL) // = 3154124UL
#define EVE_REG_PLAYBACK_READPTR        (0x003020BCUL) // = 3154108UL
#define EVE_REG_PLAYBACK_START          (0x003020B4UL) // = 3154100UL
#define EVE_REG_PLAY_CONTROL            (0x0030914EUL) // = 3182926UL
#define EVE_REG_COPRO_PATCH_PTR         (0x00309162UL) // = 3182946UL
#define EVE_REG_PWM_DUTY                (0x003020D4UL) // = 3154132UL
#define EVE_REG_PWM_HZ                  (0x003020D0UL) // = 3154128UL
#define EVE_REG_RAM_FOLD                (0x003020F4UL) // = 3154164UL
#define EVE_REG_RASTERY                 (0x00302560UL) // = 3155296UL
#define EVE_REG_RENDERMODE              (0x00302010UL) // = 3153936UL
#define EVE_REG_ROMSUB_SEL              (0x003020F0UL) // = 3154160UL
#define EVE_REG_ROTATE                  (0x00302058UL) // = 3154008UL
#define EVE_REG_SNAPFORMAT              (0x0030201CUL) // = 3153948UL
#define EVE_REG_SNAPSHOT                (0x00302018UL) // = 3153944UL
#define EVE_REG_SNAPY                   (0x00302014UL) // = 3153940UL
#define EVE_REG_SOUND                   (0x00302088UL) // = 3154056UL
#define EVE_REG_SPIM                    (0x00302584UL) // = 3155332UL
#define EVE_REG_SPIM_DIR                (0x00302580UL) // = 3155328UL
#define EVE_REG_SPI_EARLY_TX            (0x0030217CUL) // = 3154300UL
#define EVE_REG_SPI_WIDTH               (0x00302188UL) // = 3154312UL
#define EVE_REG_SWIZZLE                 (0x00302064UL) // = 3154020UL
#define EVE_REG_TAG                     (0x0030207CUL) // = 3154044UL
#define EVE_REG_TAG_X                   (0x00302074UL) // = 3154036UL
#define EVE_REG_TAG_Y                   (0x00302078UL) // = 3154040UL
#define EVE_REG_TAP_CRC                 (0x00302024UL) // = 3153956UL
#define EVE_REG_TAP_MASK                (0x00302028UL) // = 3153960UL
#define EVE_REG_TOUCH_ADC_MODE          (0x00302108UL) // = 3154184UL
#define EVE_REG_TOUCH_CHARGE            (0x0030210CUL) // = 3154188UL
#define EVE_REG_TOUCH_DIRECT_XY         (0x0030218CUL) // = 3154316UL
#define EVE_REG_TOUCH_DIRECT_Z1Z2       (0x00302190UL) // = 3154320UL
#define EVE_REG_TOUCH_FAULT             (0x00302170UL) // = 3154288UL
#define EVE_REG_TOUCH_MODE              (0x00302104UL) // = 3154180UL
#define EVE_REG_TOUCH_OVERSAMPLE        (0x00302114UL) // = 3154196UL
#define EVE_REG_TOUCH_RAW_XY            (0x0030211CUL) // = 3154204UL
#define EVE_REG_TOUCH_RZ                (0x00302120UL) // = 3154208UL
#define EVE_REG_TOUCH_RZTHRESH          (0x00302118UL) // = 3154200UL
#define EVE_REG_TOUCH_SCREEN_XY         (0x00302124UL) // = 3154212UL
#define EVE_REG_TOUCH_SETTLE            (0x00302110UL) // = 3154192UL
#define EVE_REG_TOUCH_TAG               (0x0030212CUL) // = 3154220UL
#define EVE_REG_TOUCH_TAG1              (0x00302134UL) // = 3154228UL
#define EVE_REG_TOUCH_TAG1_XY           (0x00302130UL) // = 3154224UL
#define EVE_REG_TOUCH_TAG2              (0x0030213CUL) // = 3154236UL
#define EVE_REG_TOUCH_TAG2_XY           (0x00302138UL) // = 3154232UL
#define EVE_REG_TOUCH_TAG3              (0x00302144UL) // = 3154244UL
#define EVE_REG_TOUCH_TAG3_XY           (0x00302140UL) // = 3154240UL
#define EVE_REG_TOUCH_TAG4              (0x0030214CUL) // = 3154252UL
#define EVE_REG_TOUCH_TAG4_XY           (0x00302148UL) // = 3154248UL
#define EVE_REG_TOUCH_TAG_XY            (0x00302128UL) // = 3154216UL
#define EVE_REG_TOUCH_TRANSFORM_A       (0x00302150UL) // = 3154256UL
#define EVE_REG_TOUCH_TRANSFORM_B       (0x00302154UL) // = 3154260UL
#define EVE_REG_TOUCH_TRANSFORM_C       (0x00302158UL) // = 3154264UL
#define EVE_REG_TOUCH_TRANSFORM_D       (0x0030215CUL) // = 3154268UL
#define EVE_REG_TOUCH_TRANSFORM_E       (0x00302160UL) // = 3154272UL
#define EVE_REG_TOUCH_TRANSFORM_F       (0x00302164UL) // = 3154276UL
#define EVE_REG_TRACKER                 (0x00309000UL) // = 3182592UL
#define EVE_REG_TRACKER_1               (0x00309004UL) // = 3182596UL
#define EVE_REG_TRACKER_2               (0x00309008UL) // = 3182600UL
#define EVE_REG_TRACKER_3               (0x0030900CUL) // = 3182604UL
#define EVE_REG_TRACKER_4               (0x00309010UL) // = 3182608UL
#define EVE_REG_TRIM                    (0x00302180UL) // = 3154304UL
#define EVE_REG_VCYCLE                  (0x00302040UL) // = 3153984UL
#define EVE_REG_VOFFSET                 (0x00302044UL) // = 3153988UL
#define EVE_REG_VOL_PB                  (0x00302080UL) // = 3154048UL
#define EVE_REG_VOL_SOUND               (0x00302084UL) // = 3154052UL
#define EVE_REG_VSIZE                   (0x00302048UL) // = 3153992UL
#define EVE_REG_VSYNC0                  (0x0030204CUL) // = 3153996UL
#define EVE_REG_VSYNC1                  (0x00302050UL) // = 3154000UL

#define EVE_ENC_CMD_ANIMDRAW            (0xFFFFFF56UL) // = 4294967126UL
#define EVE_ENC_CMD_ANIMFRAME           (0xFFFFFF5AUL) // = 4294967130UL
#define EVE_ENC_CMD_ANIMSTART           (0xFFFFFF53UL) // = 4294967123UL
#define EVE_ENC_CMD_ANIMSTOP            (0xFFFFFF54UL) // = 4294967124UL
#define EVE_ENC_CMD_ANIMXY              (0xFFFFFF55UL) // = 4294967125UL
#define EVE_ENC_CMD_APPEND              (0xFFFFFF1EUL) // = 4294967070UL
#define EVE_ENC_CMD_APPENDF             (0xFFFFFF59UL) // = 4294967129UL
#define EVE_ENC_CMD_BGCOLOR             (0xFFFFFF09UL) // = 4294967049UL
#define EVE_ENC_CMD_BITMAP_TRANSFORM    (0xFFFFFF21UL) // = 4294967073UL
#define EVE_ENC_CMD_BUTTON              (0xFFFFFF0DUL) // = 4294967053UL
#define EVE_ENC_CMD_CALIBRATE           (0xFFFFFF15UL) // = 4294967061UL
#define EVE_ENC_CMD_CLEARCACHE          (0xFFFFFF4FUL) // = 4294967119UL
#define EVE_ENC_CMD_CLOCK               (0xFFFFFF14UL) // = 4294967060UL
#define EVE_ENC_CMD_COLDSTART           (0xFFFFFF32UL) // = 4294967090UL
#define EVE_ENC_CMD_CRC                 (0xFFFFFF03UL) // = 4294967043UL
#define EVE_ENC_CMD_DEPRECATED_CSKETCH  (0xFFFFFF35UL) // = 4294967093UL
#define EVE_ENC_CMD_DIAL                (0xFFFFFF2DUL) // = 4294967085UL
#define EVE_ENC_CMD_DLSTART             (0xFFFFFF00UL) // = 4294967040UL
#define EVE_ENC_CMD_EXECUTE             (0xFFFFFF07UL) // = 4294967047UL
#define EVE_ENC_CMD_FGCOLOR             (0xFFFFFF0AUL) // = 4294967050UL
#define EVE_ENC_CMD_FILLWIDTH           (0xFFFFFF58UL) // = 4294967128UL
#define EVE_ENC_CMD_FLASHATTACH         (0xFFFFFF49UL) // = 4294967113UL
#define EVE_ENC_CMD_FLASHDETACH         (0xFFFFFF48UL) // = 4294967112UL
#define EVE_ENC_CMD_FLASHERASE          (0xFFFFFF44UL) // = 4294967108UL
#define EVE_ENC_CMD_FLASHFAST           (0xFFFFFF4AUL) // = 4294967114UL
#define EVE_ENC_CMD_FLASHREAD           (0xFFFFFF46UL) // = 4294967110UL
#define EVE_ENC_CMD_FLASHSOURCE         (0xFFFFFF4EUL) // = 4294967118UL
#define EVE_ENC_CMD_FLASHSPIDESEL       (0xFFFFFF4BUL) // = 4294967115UL
#define EVE_ENC_CMD_FLASHSPIRX          (0xFFFFFF4DUL) // = 4294967117UL
#define EVE_ENC_CMD_FLASHSPITX          (0xFFFFFF4CUL) // = 4294967116UL
#define EVE_ENC_CMD_FLASHUPDATE         (0xFFFFFF47UL) // = 4294967111UL
#define EVE_ENC_CMD_FLASHWRITE          (0xFFFFFF45UL) // = 4294967109UL
#define EVE_ENC_CMD_GAUGE               (0xFFFFFF13UL) // = 4294967059UL
#define EVE_ENC_CMD_GETMATRIX           (0xFFFFFF33UL) // = 4294967091UL
#define EVE_ENC_CMD_GETPOINT            (0xFFFFFF08UL) // = 4294967048UL
#define EVE_ENC_CMD_GETPROPS            (0xFFFFFF25UL) // = 4294967077UL
#define EVE_ENC_CMD_GETPTR              (0xFFFFFF23UL) // = 4294967075UL
#define EVE_ENC_CMD_GRADCOLOR           (0xFFFFFF34UL) // = 4294967092UL
#define EVE_ENC_CMD_GRADIENT            (0xFFFFFF0BUL) // = 4294967051UL
#define EVE_ENC_CMD_GRADIENTA           (0xFFFFFF57UL) // = 4294967127UL
#define EVE_ENC_CMD_HAMMERAUX           (0xFFFFFF04UL) // = 4294967044UL
#define EVE_ENC_CMD_HMAC                (0xFFFFFF5DUL) // = 4294967133UL
#define EVE_ENC_CMD_IDCT_DELETED        (0xFFFFFF06UL) // = 4294967046UL
#define EVE_ENC_CMD_INFLATE             (0xFFFFFF22UL) // = 4294967074UL
#define EVE_ENC_CMD_INFLATE2            (0xFFFFFF50UL) // = 4294967120UL
#define EVE_ENC_CMD_INTERRUPT           (0xFFFFFF02UL) // = 4294967042UL
#define EVE_ENC_CMD_INT_RAMSHARED       (0xFFFFFF3DUL) // = 4294967101UL
#define EVE_ENC_CMD_INT_SWLOADIMAGE     (0xFFFFFF3EUL) // = 4294967102UL
#define EVE_ENC_CMD_KEYS                (0xFFFFFF0EUL) // = 4294967054UL
#define EVE_ENC_CMD_LAST_               (0xFFFFFF5EUL) // = 4294967134UL
#define EVE_ENC_CMD_LOADIDENTITY        (0xFFFFFF26UL) // = 4294967078UL
#define EVE_ENC_CMD_LOADIMAGE           (0xFFFFFF24UL) // = 4294967076UL
#define EVE_ENC_CMD_LOGO                (0xFFFFFF31UL) // = 4294967089UL
#define EVE_ENC_CMD_MARCH               (0xFFFFFF05UL) // = 4294967045UL
#define EVE_ENC_CMD_MEDIAFIFO           (0xFFFFFF39UL) // = 4294967097UL
#define EVE_ENC_CMD_MEMCPY              (0xFFFFFF1DUL) // = 4294967069UL
#define EVE_ENC_CMD_MEMCRC              (0xFFFFFF18UL) // = 4294967064UL
#define EVE_ENC_CMD_MEMSET              (0xFFFFFF1BUL) // = 4294967067UL
#define EVE_ENC_CMD_MEMWRITE            (0xFFFFFF1AUL) // = 4294967066UL
#define EVE_ENC_CMD_MEMZERO             (0xFFFFFF1CUL) // = 4294967068UL
#define EVE_ENC_CMD_NOP                 (0xFFFFFF5BUL) // = 4294967131UL
#define EVE_ENC_CMD_NUMBER              (0xFFFFFF2EUL) // = 4294967086UL
#define EVE_ENC_CMD_PLAYVIDEO           (0xFFFFFF3AUL) // = 4294967098UL
#define EVE_ENC_CMD_PROGRESS            (0xFFFFFF0FUL) // = 4294967055UL
#define EVE_ENC_CMD_REGREAD             (0xFFFFFF19UL) // = 4294967065UL
#define EVE_ENC_CMD_RESETFONTS          (0xFFFFFF52UL) // = 4294967122UL
#define EVE_ENC_CMD_ROMFONT             (0xFFFFFF3FUL) // = 4294967103UL
#define EVE_ENC_CMD_ROTATE              (0xFFFFFF29UL) // = 4294967081UL
#define EVE_ENC_CMD_ROTATEAROUND        (0xFFFFFF51UL) // = 4294967121UL
#define EVE_ENC_CMD_SCALE               (0xFFFFFF28UL) // = 4294967080UL
#define EVE_ENC_CMD_SCREENSAVER         (0xFFFFFF2FUL) // = 4294967087UL
#define EVE_ENC_CMD_SCROLLBAR           (0xFFFFFF11UL) // = 4294967057UL
#define EVE_ENC_CMD_SETBASE             (0xFFFFFF38UL) // = 4294967096UL
#define EVE_ENC_CMD_SETBITMAP           (0xFFFFFF43UL) // = 4294967107UL
#define EVE_ENC_CMD_SETFONT             (0xFFFFFF2BUL) // = 4294967083UL
#define EVE_ENC_CMD_SETFONT2            (0xFFFFFF3BUL) // = 4294967099UL
#define EVE_ENC_CMD_SETMATRIX           (0xFFFFFF2AUL) // = 4294967082UL
#define EVE_ENC_CMD_SETROTATE           (0xFFFFFF36UL) // = 4294967094UL
#define EVE_ENC_CMD_SETSCRATCH          (0xFFFFFF3CUL) // = 4294967100UL
#define EVE_ENC_CMD_SHA1                (0xFFFFFF5CUL) // = 4294967132UL
#define EVE_ENC_CMD_SKETCH              (0xFFFFFF30UL) // = 4294967088UL
#define EVE_ENC_CMD_SLIDER              (0xFFFFFF10UL) // = 4294967056UL
#define EVE_ENC_CMD_SNAPSHOT            (0xFFFFFF1FUL) // = 4294967071UL
#define EVE_ENC_CMD_SNAPSHOT2           (0xFFFFFF37UL) // = 4294967095UL
#define EVE_ENC_CMD_SPINNER             (0xFFFFFF16UL) // = 4294967062UL
#define EVE_ENC_CMD_STOP                (0xFFFFFF17UL) // = 4294967063UL
#define EVE_ENC_CMD_SWAP                (0xFFFFFF01UL) // = 4294967041UL
#define EVE_ENC_CMD_SYNC                (0xFFFFFF42UL) // = 4294967106UL
#define EVE_ENC_CMD_TEXT                (0xFFFFFF0CUL) // = 4294967052UL
#define EVE_ENC_CMD_TOGGLE              (0xFFFFFF12UL) // = 4294967058UL
#define EVE_ENC_CMD_TOUCH_TRANSFORM     (0xFFFFFF20UL) // = 4294967072UL
#define EVE_ENC_CMD_TRACK               (0xFFFFFF2CUL) // = 4294967084UL
#define EVE_ENC_CMD_TRANSLATE           (0xFFFFFF27UL) // = 4294967079UL
#define EVE_ENC_CMD_VIDEOFRAME          (0xFFFFFF41UL) // = 4294967105UL
#define EVE_ENC_CMD_VIDEOSTART          (0xFFFFFF40UL) // = 4294967104UL
#define EVE_ENC_CMD_VIDEOSTARTF         (0xFFFFFF5FUL) // = 4294967135UL

#define EVE_ENC_VERTEX2F(x,y)                                (( 1UL<<30)|(((x)&0x07FFFUL)<<15)|(((y)&0x07FFFUL)<<0))
#define EVE_ENC_VERTEX2II(x,y,handle,cell)                   (( 2UL<<30)|(((x)&0x01FFUL)<<21)|(((y)&0x01FFUL)<<12)|(((handle)&0x01FUL)<<7)|(((cell)&0x07FUL)<<0))
//This throws "warning: comparison of unsigned expression < 0 is always false"
//#define EVE_ENC_BITMAP_SOURCE(addr)                          (( 1UL<<24)|((addr) < 0 ?  (((addr)&0x007FFFFFUL)) : ((addr)&0x00FFFFFFUL)))
//My edit:
#define EVE_ENC_BITMAP_SOURCE(addr)                          (( 1UL<<24)|((addr)&0x00FFFFFFUL))
#define EVE_ENC_BITMAP_SOURCE2(flash_or_ram, addr)           (( 1UL<<24)|((flash_or_ram) << 23)|(((addr)&0x007FFFFFUL)<<0))
#define EVE_ENC_CLEAR_COLOR_RGB(red,green,blue)              (( 2UL<<24)|(((red)&0x0FFUL)<<16)|(((green)&0x0FFUL)<<8)|(((blue)&0x0FFUL)<<0))
#define EVE_ENC_TAG(s)                                       (( 3UL<<24)|(((s)&0x0FFUL)<<0))
#define EVE_ENC_COLOR_RGB(red,green,blue)                    (( 4UL<<24)|(((red)&0x0FFUL)<<16)|(((green)&0x0FFUL)<<8)|(((blue)&0x0FFUL)<<0))
#define EVE_ENC_BITMAP_HANDLE(handle)                        (( 5UL<<24)|(((handle)&0x01FUL)<<0))
#define EVE_ENC_CELL(cell)                                   (( 6UL<<24)|(((cell)&0x07FUL)<<0))
#define EVE_ENC_BITMAP_LAYOUT(format,linestride,height)      (( 7UL<<24)|(((format)&0x01FUL)<<19)|(((linestride)&0x03FFUL)<<9)|(((height)&0x01FFUL)<<0))
#define EVE_ENC_BITMAP_SIZE(filter,wrapx,wrapy,width,height) (( 8UL<<24)|(((filter)&0x01UL)<<20)|(((wrapx)&0x01UL)<<19)|(((wrapy)&0x01UL)<<18)|(((width)&0x01FFUL)<<9)|(((height)&0x01FFUL)<<0))
#define EVE_ENC_ALPHA_FUNC(func,ref)                         (( 9UL<<24)|(((func)&0x07UL)<<8)|(((ref)&0x0FFUL)<<0))
#define EVE_ENC_STENCIL_FUNC(func,ref,mask)                  ((10UL<<24)|(((func)&0x07UL)<<16)|(((ref)&0x0FFUL)<<8)|(((mask)&0x0FFUL)<<0))
#define EVE_ENC_BLEND_FUNC(src,dst)                          ((11UL<<24)|(((src)&0x07UL)<<3)|(((dst)&0x07UL)<<0))
#define EVE_ENC_STENCIL_OP(sfail,spass)                      ((12UL<<24)|(((sfail)&0x07UL)<<3)|(((spass)&0x07UL)<<0))
#define EVE_ENC_POINT_SIZE(size)                             ((13UL<<24)|(((size)&0x01FFFUL)<<0))
#define EVE_ENC_LINE_WIDTH(width)                            ((14UL<<24)|(((width)&0x0FFFUL)<<0))
#define EVE_ENC_CLEAR_COLOR_A(alpha)                         ((15UL<<24)|(((alpha)&0x0FFUL)<<0))
#define EVE_ENC_COLOR_A(alpha)                               ((16UL<<24)|(((alpha)&0x0FFUL)<<0))
#define EVE_ENC_CLEAR_STENCIL(s)                             ((17UL<<24)|(((s)&0x0FFUL)<<0))
#define EVE_ENC_CLEAR_TAG(s)                                 ((18UL<<24)|(((s)&0x0FFUL)<<0))
#define EVE_ENC_STENCIL_MASK(mask)                           ((19UL<<24)|(((mask)&0x0FFUL)<<0))
#define EVE_ENC_TAG_MASK(mask)                               ((20UL<<24)|(((mask)&0x01UL)<<0))
#define EVE_ENC_BITMAP_TRANSFORM_A(p,v)                      ((21UL<<24)|(((p)&0x01UL)<<17)|(((v)&0x01FFFFUL)<<0))
#define EVE_ENC_BITMAP_TRANSFORM_B(p,v)                      ((22UL<<24)|(((p)&0x01UL)<<17)|(((v)&0x01FFFFUL)<<0))
#define EVE_ENC_BITMAP_TRANSFORM_C(c)                        ((23UL<<24)|(((c)&0x0FFFFFFUL)<<0))
#define EVE_ENC_BITMAP_TRANSFORM_D(p,v)                      ((24UL<<24)|(((p)&0x01UL)<<17)|(((v)&0x01FFFFUL)<<0))
#define EVE_ENC_BITMAP_TRANSFORM_E(p,v)                      ((25UL<<24)|(((p)&0x01UL)<<17)|(((v)&0x01FFFFUL)<<0))
#define EVE_ENC_BITMAP_TRANSFORM_F(f)                        ((26UL<<24)|(((f)&0x0FFFFFFUL)<<0))
#define EVE_ENC_SCISSOR_XY(x,y)                              ((27UL<<24)|(((x)&0x07FFUL)<<11)|(((y)&0x07FFUL)<<0))
#define EVE_ENC_SCISSOR_SIZE(width,height)                   ((28UL<<24)|(((width)&0x0FFFUL)<<12)|(((height)&0x0FFFUL)<<0))
#define EVE_ENC_CALL(dest)                                   ((29UL<<24)|(((dest)&0x0FFFFUL)<<0))
#define EVE_ENC_JUMP(dest)                                   ((30UL<<24)|(((dest)&0x0FFFFUL)<<0))
#define EVE_ENC_BEGIN(prim)                                  ((31UL<<24)|(((prim)&15UL)<<0))
#define EVE_ENC_COLOR_MASK(r,g,b,a)                          ((32UL<<24)|(((r)&0x01UL)<<3)|(((g)&0x01UL)<<2)|(((b)&0x01UL)<<1)|(((a)&0x01UL)<<0))
#define EVE_ENC_END()                                        ((33UL<<24))
#define EVE_ENC_SAVE_CONTEXT()                               ((34UL<<24))
#define EVE_ENC_RESTORE_CONTEXT()                            ((35UL<<24))
#define EVE_ENC_RETURN()                                     ((36UL<<24))
#define EVE_ENC_MACRO(m)                                     ((37UL<<24)|(((m)&0x01UL)<<0))
#define EVE_ENC_CLEAR(c,s,t)                                 ((38UL<<24)|(((c)&0x01UL)<<2)|(((s)&0x01UL)<<1)|(((t)&0x01UL)<<0))
#define EVE_ENC_VERTEX_FORMAT(frac)                          ((39UL<<24)|(((frac)&0x07UL)<<0))
#define EVE_ENC_BITMAP_LAYOUT_H(linestride,height)           ((40UL<<24)|(((linestride)&0x03UL)<<2)|(((height)&0x03UL)<<0))
#define EVE_ENC_BITMAP_SIZE_H(width,height)                  ((41UL<<24)|(((width)&0x03UL)<<2)|(((height)&0x03UL)<<0))
#define EVE_ENC_PALETTE_SOURCE(addr)                         ((42UL<<24)|(((addr)&0x03FFFFFUL)<<0))
#define EVE_ENC_VERTEX_TRANSLATE_X(x)                        ((43UL<<24)|(((x)&0x01FFFFUL)<<0))
#define EVE_ENC_VERTEX_TRANSLATE_Y(y)                        ((44UL<<24)|(((y)&0x01FFFFUL)<<0))
#define EVE_ENC_NOP()                                        ((45UL<<24))
#define EVE_ENC_BITMAP_EXT_FORMAT(format)                    ((46UL<<24)|(((format)&0x0FFFFUL)<<0))
#define EVE_ENC_BITMAP_SWIZZLE(r,g,b,a)                      ((47UL<<24)|(((r)&0x07UL)<<9)|(((g)&0x07UL)<<6)|(((b)&0x07UL)<<3)|(((a)&0x07UL)<<0))
#define EVE_ENC_DISPLAY()                                    ((0UL<<24))

#define EVE_FORMAT_COMPRESSED_RGBA_ASTC_4x4_KHR              (0x000093B0UL) // = 37808UL
#define EVE_FORMAT_COMPRESSED_RGBA_ASTC_5x4_KHR              (0x000093B1UL) // = 37809UL
#define EVE_FORMAT_COMPRESSED_RGBA_ASTC_5x5_KHR              (0x000093B2UL) // = 37810UL
#define EVE_FORMAT_COMPRESSED_RGBA_ASTC_6x5_KHR              (0x000093B3UL) // = 37811UL
#define EVE_FORMAT_COMPRESSED_RGBA_ASTC_6x6_KHR              (0x000093B4UL) // = 37812UL
#define EVE_FORMAT_COMPRESSED_RGBA_ASTC_8x5_KHR              (0x000093B5UL) // = 37813UL
#define EVE_FORMAT_COMPRESSED_RGBA_ASTC_8x6_KHR              (0x000093B6UL) // = 37814UL
#define EVE_FORMAT_COMPRESSED_RGBA_ASTC_8x8_KHR              (0x000093B7UL) // = 37815UL
#define EVE_FORMAT_COMPRESSED_RGBA_ASTC_10x10_KHR            (0x000093BBUL) // = 37819UL
#define EVE_FORMAT_COMPRESSED_RGBA_ASTC_10x5_KHR             (0x000093B8UL) // = 37816UL
#define EVE_FORMAT_COMPRESSED_RGBA_ASTC_10x6_KHR             (0x000093B9UL) // = 37817UL
#define EVE_FORMAT_COMPRESSED_RGBA_ASTC_10x8_KHR             (0x000093BAUL) // = 37818UL
#define EVE_FORMAT_COMPRESSED_RGBA_ASTC_12x10_KHR            (0x000093BCUL) // = 37820UL
#define EVE_FORMAT_COMPRESSED_RGBA_ASTC_12x12_KHR            (0x000093BDUL) // = 37821UL

#define EVE_RAM_FLASH           0x800000UL
#define EVE_RAM_FLASH_POSTBLOB  0x801000UL

#define EVE_CTOUCH_MODE_COMPATIBILITY   (0x00000001UL) // = 1UL
#define EVE_CTOUCH_MODE_EXTENDED        (0x00000000UL) // = 0UL

#define EVE_STENCIL_DECR                (0x00000004UL) // = 4UL
#define EVE_DLSWAP_DONE                 (0x00000000UL) // = 0UL
#define EVE_DLSWAP_FRAME                (0x00000002UL) // = 2UL
#define EVE_DLSWAP_LINE                 (0x00000001UL) // = 1UL
#define EVE_BLEND_DST_ALPHA             (0x00000003UL) // = 3UL
#define EVE_BEGIN_EDGE_STRIP_A          (0x00000007UL) // = 7UL
#define EVE_BEGIN_EDGE_STRIP_B          (0x00000008UL) // = 8UL
#define EVE_BEGIN_EDGE_STRIP_L          (0x00000006UL) // = 6UL
#define EVE_BEGIN_EDGE_STRIP_R          (0x00000005UL) // = 5UL
#define EVE_TEST_EQUAL                  (0x00000005UL) // = 5UL
// Return values from EVE_REG_FLASH_STATUS
#define EVE_FLASH_STATUS_INIT           (0x00000000UL) // = 0UL
#define EVE_FLASH_STATUS_DETACHED       (0x00000001UL) // = 1UL
#define EVE_FLASH_STATUS_BASIC          (0x00000002UL) // = 2UL
#define EVE_FLASH_STATUS_FULL           (0x00000003UL) // = 3UL
#define EVE_TEST_GEQUAL                 (0x00000004UL) // = 4UL
#define EVE_GLFORMAT                    (0x0000001FUL) // = 31UL
#define EVE_TEST_GREATER                (0x00000003UL) // = 3UL
#define EVE_GREEN                       (0x00000003UL) // = 3UL
#define EVE_STENCIL_INCR                (0x00000003UL) // = 3UL
#define EVE_INT_CMDEMPTY                (0x00000020UL) // = 32UL
#define EVE_INT_CMDFLAG                 (0x00000040UL) // = 64UL
#define EVE_INT_CONVCOMPLETE            (0x00000080UL) // = 128UL
#define EVE_INT_G8                      (0x00000012UL) // = 18UL
#define EVE_INT_L8C                     (0x0000000CUL) // = 12UL
#define EVE_INT_PLAYBACK                (0x00000010UL) // = 16UL
#define EVE_INT_SOUND                   (0x00000008UL) // = 8UL
#define EVE_INT_SWAP                    (0x00000001UL) // = 1UL
#define EVE_INT_TAG                     (0x00000004UL) // = 4UL
#define EVE_INT_TOUCH                   (0x00000002UL) // = 2UL
#define EVE_INT_VGA                     (0x0000000DUL) // = 13UL
#define EVE_STENCIL_INVERT              (0x00000005UL) // = 5UL

#define EVE_STENCIL_KEEP                (0x00000001UL) // = 1UL
#define EVE_FORMAT_L1                   (0x00000001UL) // = 1UL
#define EVE_FORMAT_L2                   (0x00000011UL) // = 17UL
#define EVE_FORMAT_L4                   (0x00000002UL) // = 2UL
#define EVE_FORMAT_L8                   (0x00000003UL) // = 3UL
#define EVE_TEST_LEQUAL                 (0x00000002UL) // = 2UL
#define EVE_TEST_LESS                   (0x00000001UL) // = 1UL
#define EVE_LINEAR_SAMPLES              (0x00000000UL) // = 0UL
#define EVE_BEGIN_LINES                 (0x00000003UL) // = 3UL
#define EVE_BEGIN_LINE_STRIP            (0x00000004UL) // = 4UL
#define EVE_FILTER_NEAREST              (0x00000000UL) // = 0UL
#define EVE_TEST_NEVER                  (0x00000000UL) // = 0UL
#define EVE_TEST_NOTEQUAL               (0x00000006UL) // = 6UL
#define EVE_BLEND_ONE                   (0x00000001UL) // = 1UL
#define EVE_BLEND_ONE_MINUS_DST_ALPHA   (0x00000005UL) // = 5UL
#define EVE_BLEND_ONE_MINUS_SRC_ALPHA   (0x00000004UL) // = 4UL
#define EVE_OPT_CENTER                  (0x00000600UL) // = 1536UL
#define EVE_OPT_CENTERX                 (0x00000200UL) // = 512UL
#define EVE_OPT_CENTERY                 (0x00000400UL) // = 1024UL
#define EVE_OPT_FILL                    (0x00002000UL) // = 8192UL
#define EVE_OPT_FLASH                   (0x00000040UL) // = 64UL
#define EVE_OPT_FLAT                    (0x00000100UL) // = 256UL
#define EVE_OPT_FORMAT                  (0x00001000UL) // = 4096UL
#define EVE_OPT_FULLSCREEN              (0x00000008UL) // = 8UL
#define EVE_OPT_MEDIAFIFO               (0x00000010UL) // = 16UL
#define EVE_OPT_MONO                    (0x00000001UL) // = 1UL
#define EVE_OPT_NOBACK                  (0x00001000UL) // = 4096UL
#define EVE_OPT_NODL                    (0x00000002UL) // = 2UL
#define EVE_OPT_NOHANDS                 (0x0000C000UL) // = 49152UL
#define EVE_OPT_NOHM                    (0x00004000UL) // = 16384UL
#define EVE_OPT_NOPOINTER               (0x00004000UL) // = 16384UL
#define EVE_OPT_NOSECS                  (0x00008000UL) // = 32768UL
#define EVE_OPT_NOTEAR                  (0x00000004UL) // = 4UL
#define EVE_OPT_NOTICKS                 (0x00002000UL) // = 8192UL
#define EVE_OPT_OVERLAY                 (0x00000080UL) // = 128UL
#define EVE_OPT_RIGHTX                  (0x00000800UL) // = 2048UL
#define EVE_OPT_SIGNED                  (0x00000100UL) // = 256UL
#define EVE_OPT_SOUND                   (0x00000020UL) // = 32UL
#define EVE_FORMAT_PALETTED             (0x00000008UL) // = 8UL
#define EVE_FORMAT_PALETTED4444         (0x0000000FUL) // = 15UL
#define EVE_FORMAT_PALETTED565          (0x0000000EUL) // = 14UL
#define EVE_FORMAT_PALETTED8            (0x00000010UL) // = 16UL
#define EVE_BEGIN_POINTS                (0x00000002UL) // = 2UL
#define EVE_BEGIN_RECTS                 (0x00000009UL) // = 9UL
#define EVE_RED                         (0x00000002UL) // = 2UL
//#define PPC                           (0x00000010UL) // = 16UL
//#define BANKS                         (0x00000010UL) // = 16UL
//#define BANKW                         (0x00000010UL) // = 16UL
#define EVE_ADC_DIFFERENTIAL            (0x00000001UL) // = 1UL
#define EVE_ADC_SINGLE_ENDED            (0x00000000UL) // = 0UL
#define EVE_ADPCM_SAMPLES               (0x00000002UL) // = 2UL
#define EVE_ALPHA                       (0x00000005UL) // = 5UL
#define EVE_TEST_ALWAYS                 (0x00000007UL) // = 7UL
#define EVE_ANIM_HOLD                   (0x00000002UL) // = 2UL
#define EVE_ANIM_LOOP                   (0x00000001UL) // = 1UL
#define EVE_ANIM_ONCE                   (0x00000000UL) // = 0UL
#define EVE_FORMAT_ARGB1555             (0x00000000UL) // = 0UL
#define EVE_FORMAT_ARGB2                (0x00000005UL) // = 5UL
#define EVE_FORMAT_ARGB4                (0x00000006UL) // = 6UL
#define EVE_FORMAT_BARGRAPH             (0x0000000BUL) // = 11UL
#define EVE_FILTER_BILINEAR             (0x00000001UL) // = 1UL
#define EVE_BEGIN_BITMAPS               (0x00000001UL) // = 1UL
#define EVE_BLUE                        (0x00000004UL) // = 4UL
#define EVE_WRAP_BORDER                 (0x00000000UL) // = 0UL
#define EVE_WRAP_REPEAT                 (0x00000001UL) // = 1UL
#define EVE_STENCIL_REPLACE             (0x00000002UL) // = 2UL
#define EVE_FORMAT_RGB332               (0x00000004UL) // = 4UL
#define EVE_FORMAT_RGB565               (0x00000007UL) // = 7UL
#define EVE_BLEND_SRC_ALPHA             (0x00000002UL) // = 2UL
#define EVE_SS_A0                       (0x00000013UL) // = 19UL
#define EVE_SS_A1                       (0x00000014UL) // = 20UL
#define EVE_SS_A2                       (0x00000015UL) // = 21UL
#define EVE_SS_A3                       (0x00000016UL) // = 22UL
#define EVE_SS_A4                       (0x00000017UL) // = 23UL
#define EVE_SS_A5                       (0x00000018UL) // = 24UL
#define EVE_SS_A6                       (0x00000019UL) // = 25UL
#define EVE_SS_A7                       (0x0000001AUL) // = 26UL
#define EVE_SS_PAUSE                    (0x00000012UL) // = 18UL
#define EVE_SS_Q0                       (0x00000000UL) // = 0UL
#define EVE_SS_Q1                       (0x00000001UL) // = 1UL
#define EVE_SS_Q2                       (0x00000002UL) // = 2UL
#define EVE_SS_Q3                       (0x00000003UL) // = 3UL
#define EVE_SS_Q4                       (0x00000004UL) // = 4UL
#define EVE_SS_Q5                       (0x00000005UL) // = 5UL
#define EVE_SS_Q6                       (0x00000006UL) // = 6UL
#define EVE_SS_Q7                       (0x00000007UL) // = 7UL
#define EVE_SS_Q8                       (0x00000008UL) // = 8UL
#define EVE_SS_Q9                       (0x00000009UL) // = 9UL
#define EVE_SS_QA                       (0x0000000AUL) // = 10UL
#define EVE_SS_QB                       (0x0000000BUL) // = 11UL
#define EVE_SS_QC                       (0x0000000CUL) // = 12UL
#define EVE_SS_QD                       (0x0000000DUL) // = 13UL
#define EVE_SS_QE                       (0x0000000EUL) // = 14UL
#define EVE_SS_QF                       (0x0000000FUL) // = 15UL
#define EVE_SS_QI                       (0x0000001FUL) // = 31UL
#define EVE_SS_S0                       (0x00000010UL) // = 16UL
#define EVE_SS_S1                       (0x00000011UL) // = 17UL
#define EVE_TEXT8EVE_SS                 (0x00000009UL) // = 9UL
#define EVE_TEXTVGA                     (0x0000000AUL) // = 10UL
#define EVE_TOUCHMODE_CONTINUOUS        (0x00000003UL) // = 3UL
#define EVE_TOUCHMODE_FRAME             (0x00000002UL) // = 2UL
#define EVE_TOUCHMODE_OFF               (0x00000000UL) // = 0UL
#define EVE_TOUCHMODE_ONESHOT           (0x00000001UL) // = 1UL
#define EVE_ULAW_SAMPLES                (0x00000001UL) // = 1UL
#define EVE_VOL_ZERO                    (0x00000000UL) // = 0UL

#undef EVE_ENC_BITMAP_TRANSFORM_A //New BT815 file do not compatible with the legacy code
#undef EVE_ENC_BITMAP_TRANSFORM_B //New BT815 file do not compatible with the legacy code
#undef EVE_ENC_BITMAP_TRANSFORM_D //New BT815 file do not compatible with the legacy code
#undef EVE_ENC_BITMAP_TRANSFORM_E //New BT815 file do not compatible with the legacy code

#define EVE_ENC_BITMAP_TRANSFORM_A_EXT(p,v) ((21UL<<24)|(((p)&0x01UL)<<17)|(((v)&0x01FFFFUL)<<0))
#define EVE_ENC_BITMAP_TRANSFORM_B_EXT(p,v) ((22UL<<24)|(((p)&0x01UL)<<17)|(((v)&0x01FFFFUL)<<0))
#define EVE_ENC_BITMAP_TRANSFORM_C(c)       ((23UL<<24)|(((c)&0x0FFFFFFUL)<<0))
#define EVE_ENC_BITMAP_TRANSFORM_D_EXT(p,v) ((24UL<<24)|(((p)&0x01UL)<<17)|(((v)&0x01FFFFUL)<<0))
#define EVE_ENC_BITMAP_TRANSFORM_E_EXT(p,v) ((25UL<<24)|(((p)&0x01UL)<<17)|(((v)&0x01FFFFUL)<<0))
#define EVE_ENC_BITMAP_TRANSFORM_F(f)       ((26UL<<24)|(((f)&0x0FFFFFFUL)<<0))

// Added to ensure previous macros are fine
#define EVE_ENC_BITMAP_TRANSFORM_A(a) EVE_ENC_BITMAP_TRANSFORM_A_EXT(0,a)
#define EVE_ENC_BITMAP_TRANSFORM_B(b) EVE_ENC_BITMAP_TRANSFORM_B_EXT(0,b)
#define EVE_ENC_BITMAP_TRANSFORM_D(d) EVE_ENC_BITMAP_TRANSFORM_D_EXT(0,d)
#define EVE_ENC_BITMAP_TRANSFORM_E(e) EVE_ENC_BITMAP_TRANSFORM_E_EXT(0,e)

