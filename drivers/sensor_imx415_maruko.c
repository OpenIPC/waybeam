/* Copyright (c) 2018-2019 Sigmastar Technology Corp.
 All rights reserved.

 Unless otherwise stipulated in writing, any and all information contained
herein regardless in any format shall remain the sole proprietary of
Sigmastar Technology Corp. and be kept in strict confidence
(Sigmastar Confidential Information) by the recipient.
Any unauthorized act including without limitation unauthorized disclosure,
copying, use, reproduction, sale, distribution, modification, disassembling,
reverse engineering and compiling of the contents of Sigmastar Confidential
Information is unlawful and strictly prohibited. Sigmastar hereby reserves the
rights to any and all damages, losses, costs and expenses resulting therefrom.
*/

#ifdef __cplusplus
extern "C" {
#endif

#include <drv_sensor.h>
#include <drv_sensor_common.h>
#include <drv_sensor_init_table.h> //TODO: move this header to drv_sensor_common.h
#include <sensor_i2c_api.h>
#ifdef __cplusplus
}
#endif

SENSOR_DRV_ENTRY_IMPL_BEGIN_EX(IMX415_HDR);

#ifndef ARRAY_SIZE
#define ARRAY_SIZE CAM_OS_ARRAY_SIZE
#endif

//#define SENSOR_PAD_GROUP_SET CUS_SENSOR_PAD_GROUP_A
//#define SENSOR_CHANNEL_NUM (0)

//============================================
#define ENABLE 1
#define DISABLE 0
#undef SENSOR_DBG
#define SENSOR_DBG 0

#define DEBUG_INFO 0

#if SENSOR_DBG == 1
//#define SENSOR_DMSG(args...) LOGD(args)
//#define SENSOR_DMSG(args...) LOGE(args)
//#define SENSOR_DMSG(args...) printf(args)
#elif SENSOR_DBG == 0
//#define SENSOR_DMSG(args...)
#endif
///////////////////////////////////////////////////////////////
//          @@@                                              //
//         @ @@      ==  S t a r t * H e r e  ==             //
//           @@      ==  S t a r t * H e r e  ==             //
//           @@      ==  S t a r t * H e r e  ==             //
//          @@@@                                             //
//                                                           //
//      Start Step 1 --  show preview on LCM                 //
//                                                           //
//  Fill these #define value and table with correct settings //
//      camera can work and show preview on LCM              //
//                                                           //
///////////////////////////////////////////////////////////////

////////////////////////////////////
// Sensor-If Info                 //
////////////////////////////////////
// MIPI config begin.
#define SENSOR_MIPI_LANE_NUM (4)

#define SENSOR_ISP_TYPE ISP_EXT // ISP_EXT, ISP_SOC (Non-used)
//#define SENSOR_DATAFMT             CUS_DATAFMT_BAYER    //CUS_DATAFMT_YUV, CUS_DATAFMT_BAYER
#define SENSOR_IFBUS_TYPE CUS_SENIF_BUS_MIPI // CFG //CUS_SENIF_BUS_PARL, CUS_SENIF_BUS_MIPI
#define SENSOR_MIPI_HSYNC_MODE PACKET_HEADER_EDGE1
#define SENSOR_DATAPREC CUS_DATAPRECISION_12
#define SENSOR_DATAMODE CUS_SEN_10TO12_9098 // CFG
#define SENSOR_BAYERID CUS_BAYER_GB // 0h: CUS_BAYER_RG, 1h: CUS_BAYER_GR, 2h: CUS_BAYER_BG, 3h: CUS_BAYER_GB
#define SENSOR_RGBIRID CUS_RGBIR_NONE
#define SENSOR_ORIT CUS_ORIT_M0F0 // CUS_ORIT_M0F0, CUS_ORIT_M1F0, CUS_ORIT_M0F1, CUS_ORIT_M1F1,
//#define SENSOR_YCORDER              CUS_SEN_YCODR_YC     //CUS_SEN_YCODR_YC, CUS_SEN_YCODR_CY
//#define long_packet_type_enable     0x00 //UD1~UD8 (user define)

////////////////////////////////////
// MCLK Info                      //
////////////////////////////////////
#define Preview_MCLK_SPEED CUS_CMU_CLK_27MHZ // CUS_CMU_CLK_24MHZ //CUS_CMU_CLK_37P125MHZ//CUS_CMU_CLK_27MHZ

////////////////////////////////////
// I2C Info                       //
////////////////////////////////////
#define SENSOR_I2C_ADDR 0x34 // I2C slave address
#define SENSOR_I2C_SPEED 300000 // 200000 //300000 //240000                  //I2C speed, 60000~320000
//#define SENSOR_I2C_CHANNEL           1                 //I2C Channel
//#define SENSOR_I2C_PAD_MODE          2                 //Pad/Mode Number
#define SENSOR_I2C_LEGACY I2C_NORMAL_MODE // usally set CUS_I2C_NORMAL_MODE,  if use old OVT I2C protocol=> set CUS_I2C_LEGACY_MODE
#define SENSOR_I2C_FMT I2C_FMT_A16D8 // CUS_I2C_FMT_A8D8, CUS_I2C_FMT_A8D16, CUS_I2C_FMT_A16D8, CUS_I2C_FMT_A16D16

////////////////////////////////////
// Sensor Signal                  //
////////////////////////////////////
#define SENSOR_PWDN_POL CUS_CLK_POL_NEG // if PWDN pin High can makes sensor in power down, set CUS_CLK_POL_POS
#define SENSOR_RST_POL CUS_CLK_POL_NEG // if RESET pin High can makes sensor in reset state, set CUS_CLK_POL_NEG
                                       // VSYNC/HSYNC POL can be found in data sheet timing diagram,
                                       // Notice: the initial setting may contain VSYNC/HSYNC POL inverse settings so that condition is different.

////////////////////////////////////
// Sensor ID                      //
////////////////////////////////////
// define SENSOR_ID

#undef SENSOR_NAME
#define SENSOR_NAME IMX415

#define CHIP_ID 0x0415

////////////////////////////////////
// Image Info                     //
////////////////////////////////////
static struct { // LINEAR
    // Modify it based on number of support resolution
    enum { LINEAR_RES_1 = 0,
        LINEAR_RES_2,
        LINEAR_RES_3,
        LINEAR_RES_4,
        LINEAR_RES_5,
        LINEAR_RES_END } mode;
    // Sensor Output Image info
    struct _senout {
        s32 width, height, min_fps, max_fps;
    } senout;
    // VIF Get Image Info
    struct _sensif {
        s32 crop_start_X, crop_start_y, preview_w, preview_h;
    } senif;
    // Show resolution string
    struct _senstr {
        const char* strResDesc;
    } senstr;
    // MIPI link rate in Mbps/lane (keys the CSI-MAC clock in pCus_poweron;
    // the _1485 name suffix separately keys the FRAMEBASE bind in
    // maruko_pipeline.c)
    u32 link_mbps;
} imx415_mipi_linear[] = {
    /* Maruko (SSC378QE) — one best mode per FPS tier, all non-binned and
     * ~16:9 (sensor native aspect), sorted by FPS.  The 1485 modes are sized
     * to the measured ISP m2m ceiling T ~= 1.7ms + 3.65ns/px and deliver
     * their full nominal rate; mode 0 stays on the 891 link for the
     * REALTIME (lowest-latency) VIF->ISP bind.  The retired vendor binned /
     * superwide tables are parked under #if 0 below — see
     * documentation/MARUKO_IMX415_1485_MODES.md for why they were dropped
     * (binned tables burst 393 MPix/s > the 384 MPix/s ISP drain). */
    { LINEAR_RES_1, { 3760, 2116, 3, 30 }, { 0, 0, 3760, 2116 }, { "3760x2116@30fps" }, 891 },        /* best IQ, REALTIME */
    { LINEAR_RES_2, { 2952, 1656, 3, 50 }, { 0, 0, 2952, 1656 }, { "2952x1656@50fps_1485" }, 1485 },  /* 1:1 5MP readout */
    { LINEAR_RES_3, { 2688, 1512, 3, 60 }, { 0, 0, 2688, 1512 }, { "2688x1512@60fps_1485" }, 1485 },  /* ISP-ceiling max 16:9 @60 */
    { LINEAR_RES_4, { 2112, 1184, 3, 90 }, { 0, 0, 2112, 1184 }, { "2112x1184@90fps_1485" }, 1485 },  /* ISP-ceiling max @90 */
    { LINEAR_RES_5, { 1920, 1080, 3, 100 }, { 0, 0, 1920, 1080 }, { "1920x1080@100fps_1485" }, 1485 },/* low-latency 100 */
};

u32 vts_30fps = 2250;
u32 Preview_line_period = 17778;

////////////////////////////////////
// AE Info                        //
////////////////////////////////////
#define SENSOR_MAX_GAIN (3981 * 1024) // max sensor again, a-gain * conversion-gain*d-gain
#define SENSOR_MIN_GAIN (1 * 1024)
#define SENSOR_GAIN_DELAY_FRAME_COUNT (2)
#define SENSOR_SHUTTER_DELAY_FRAME_COUNT (2)

////////////////////////////////////
// Mirror-Flip Info               //
////////////////////////////////////
#define MIRROR_FLIP 0x3030
#define SENSOR_NOR 0x0
#define SENSOR_MIRROR_EN (0x1 << 0)
#define SENSOR_FLIP_EN (0x1 << 1)
#define SENSOR_MIRROR_FLIP_EN 0x3

#if defined(SENSOR_MODULE_VERSION)
#define TO_STR_NATIVE(e) #e
#define TO_STR_PROXY(m, e) m(e)
#define MACRO_TO_STRING(e) TO_STR_PROXY(TO_STR_NATIVE, e)
static char* sensor_module_version = MACRO_TO_STRING(SENSOR_MODULE_VERSION);
module_param(sensor_module_version, charp, S_IRUGO);
#endif

typedef struct {
    struct {
        u16 pre_div0;
        u16 div124;
        u16 div_cnt7b;
        u16 sdiv0;
        u16 mipi_div0;
        u16 r_divp;
        u16 sdiv1;
        u16 r_seld5;
        u16 r_sclk_dac;
        u16 sys_sel;
        u16 pdac_sel;
        u16 adac_sel;
        u16 pre_div_sp;
        u16 r_div_sp;
        u16 div_cnt5b;
        u16 sdiv_sp;
        u16 div12_sp;
        u16 mipi_lane_sel;
        u16 div_dac;
    } clk_tree;
    struct {
        bool bVideoMode;
        u16 res_idx;
        CUS_CAMSENSOR_ORIT orit;
    } res;
    struct {
        float sclk;
        u32 hts;
        u32 vts;
        u32 ho;
        u32 xinc;
        u32 line_freq;
        u32 us_per_line;
        u32 final_us;
        u32 final_gain;
        u32 back_pv_us;
        u32 fps;
        u32 preview_fps;
        u32 expo_lines;
        u32 expo_lef_us;
    } expo;

    I2C_ARRAY tVts_reg[3];
    I2C_ARRAY tExpo_reg[3];
    I2C_ARRAY tGain_reg[2];
    bool dirty;
    bool orien_dirty;
} imx415_params;

static int pCus_SetAEUSecs(ms_cus_sensor* handle, u32 us);
///////////////////////////////////////////////////////////////
//          @@@                                              //
//         @  @@                                             //
//           @@                                              //
//          @@                                               //
//         @@@@@                                             //
//                                                           //
//      Start Step 2 --  set Sensor initial and              //
//                       adjust parameter                    //
//  Fill these register table with resolution settings       //
//      camera can work and show preview on LCM              //
//                                                           //
///////////////////////////////////////////////////////////////

/* ── RETIRED VENDOR MODES (kept for posterity, not selectable) ──────────
 * The binned vendor tables run HMAX=365 (4.882us line): a 1920px line
 * bursts at 393 MPix/s, above the 384 MPix/s ISP REALTIME drain — chronic
 * FIFO pressure (the 120fps mode only ever delivered 115-118 fps).  The
 * surfaced lineup replaced them with non-binned ~16:9 1485 modes; see
 * documentation/MARUKO_IMX415_1485_MODES.md and git history for the full
 * story (including the HMAX=550 ispsafe 1080p60 rework). ── */
#if 0
// 1920x1080@90fps binned
const static I2C_ARRAY Sensor_2m_90fps_init_table_4lane_linear[] = {
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME[9:0]
    { 0x300A, 0x42 }, // CPWAIT_TIME[9:0]
    { 0x3020, 0x01 }, // HADD (horizontal binning)
    { 0x3021, 0x01 }, // VADD (vertical binning)
    { 0x3022, 0x01 }, // ADDMODE (binning 2/2)
    { 0x3024, 0xCA }, // VMAX
    { 0x3025, 0x08 }, //
    { 0x3028, 0x6D }, // HMAX
    { 0x3029, 0x01 }, //
    { 0x3031, 0x00 }, // ADBIT (10bit)
    { 0x3032, 0x01 }, // VMAX MSB — must be 0x01, default 0x00 produces dark image
    { 0x3033, 0x05 }, // SYS_MODE (891Mbps)
    { 0x3050, 0x08 }, // SHR0[19:0]
    { 0x30C1, 0x00 }, // XVS_DRV[1:0]
    { 0x30D9, 0x02 }, // DIG_CLP_VSTART (binning 2/2)
    { 0x30DA, 0x01 }, // DIG_CLP_VNUM (binning 2/2)
    { 0x3116, 0x23 }, // INCKSEL2
    { 0x3118, 0xC6 }, // INCKSEL3
    { 0x311A, 0xE7 }, // INCKSEL4
    { 0x311E, 0x23 }, // INCKSEL5
    { 0x32D4, 0x21 }, // -
    { 0x32EC, 0xA1 }, // -
    { 0x3452, 0x7F }, // -
    { 0x3453, 0x03 }, // -
    { 0x358A, 0x04 }, // -
    { 0x35A1, 0x02 }, // -
    { 0x36BC, 0x0C }, // -
    { 0x36CC, 0x53 }, // -
    { 0x36CD, 0x00 }, // -
    { 0x36CE, 0x3C }, // -
    { 0x36D0, 0x8C }, // -
    { 0x36D1, 0x00 }, // -
    { 0x36D2, 0x71 }, // -
    { 0x36D4, 0x3C }, // -
    { 0x36D6, 0x53 }, // -
    { 0x36D7, 0x00 }, // -
    { 0x36D8, 0x71 }, // -
    { 0x36DA, 0x8C }, // -
    { 0x36DB, 0x00 }, // -
    { 0x3701, 0x00 }, // ADBIT1[7:0]
    { 0x3724, 0x02 }, // -
    { 0x3726, 0x02 }, // -
    { 0x3732, 0x02 }, // -
    { 0x3734, 0x03 }, // -
    { 0x3736, 0x03 }, // -
    { 0x3742, 0x03 }, // -
    { 0x3862, 0xE0 }, // -
    { 0x38CC, 0x30 }, // -
    { 0x38CD, 0x2F }, // -
    { 0x395C, 0x0C }, // -
    { 0x3A42, 0xD1 }, // -
    { 0x3A4C, 0x77 }, // -
    { 0x3AE0, 0x02 }, // -
    { 0x3AEC, 0x0C }, // -
    { 0x3B00, 0x2E }, // -
    { 0x3B06, 0x29 }, // -
    { 0x3B98, 0x25 }, // -
    { 0x3B99, 0x21 }, // -
    { 0x3B9B, 0x13 }, // -
    { 0x3B9C, 0x13 }, // -
    { 0x3B9D, 0x13 }, // -
    { 0x3B9E, 0x13 }, // -
    { 0x3BA1, 0x00 }, // -
    { 0x3BA2, 0x06 }, // -
    { 0x3BA3, 0x0B }, // -
    { 0x3BA4, 0x10 }, // -
    { 0x3BA5, 0x14 }, // -
    { 0x3BA6, 0x18 }, // -
    { 0x3BA7, 0x1A }, // -
    { 0x3BA8, 0x1A }, // -
    { 0x3BA9, 0x1A }, // -
    { 0x3BAC, 0xED }, // -
    { 0x3BAD, 0x01 }, // -
    { 0x3BAE, 0xF6 }, // -
    { 0x3BAF, 0x02 }, // -
    { 0x3BB0, 0xA2 }, // -
    { 0x3BB1, 0x03 }, // -
    { 0x3BB2, 0xE0 }, // -
    { 0x3BB3, 0x03 }, // -
    { 0x3BB4, 0xE0 }, // -
    { 0x3BB5, 0x03 }, // -
    { 0x3BB6, 0xE0 }, // -
    { 0x3BB7, 0x03 }, // -
    { 0x3BB8, 0xE0 }, // -
    { 0x3BBA, 0xE0 }, // -
    { 0x3BBC, 0xDA }, // -
    { 0x3BBE, 0x88 }, // -
    { 0x3BC0, 0x44 }, // -
    { 0x3BC2, 0x7B }, // -
    { 0x3BC4, 0xA2 }, // -
    { 0x3BC8, 0xBD }, // -
    { 0x3BCA, 0xBD }, // -
    { 0x4004, 0xC0 }, // TXCLKESC_FREQ[15:0]
    { 0x4005, 0x06 }, //
    { 0x400C, 0x00 }, // INCKSEL6
    { 0x4018, 0x7F }, // TCLKPOST
    { 0x401A, 0x37 }, // TCLKPREPARE
    { 0x401C, 0x37 }, // TCLKTRAIL
    { 0x401E, 0xF7 }, // TCLKZERO
    { 0x401F, 0x00 }, //
    { 0x4020, 0x3F }, // THSPREPARE
    { 0x4022, 0x6F }, // THSZERO
    { 0x4024, 0x3F }, // THSTRAIL
    { 0x4026, 0x5F }, // THSEXIT
    { 0x4028, 0x2F }, // TLPX
    { 0x4074, 0x01 }, // INCKSEL7
    { 0xFFFF, 0x24 },
    { 0x3002, 0x00 }, // Master mode start
    { 0xFFFF, 0x10 },
    { 0x3000, 0x00 }, // Operating
};

// 1472x816@120fps
const static I2C_ARRAY Sensor_1m_120fps_init_table_4lane_linear[] = {
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME[9:0]
    { 0x300A, 0x42 }, // CPWAIT_TIME[9:0]
    { 0x301C, 0x04 }, // WINMODE (cropping mode)
    { 0x3020, 0x01 }, // HADD (horizontal binning)
    { 0x3021, 0x01 }, // VADD (vertical binning)
    { 0x3022, 0x01 }, // ADDMODE (binning 2/2)
    { 0x3024, 0xA4 }, // VMAX
    { 0x3025, 0x06 }, //
    { 0x3028, 0x6D }, // HMAX
    { 0x3029, 0x01 }, //
    { 0x3031, 0x00 }, // ADBIT (10bit)
    { 0x3032, 0x01 }, // VMAX MSB — must be 0x01, default 0x00 produces dark image
    { 0x3033, 0x05 }, // SYS_MODE (891Mbps)
    { 0x3040, 0xD4 }, // PIX_HST
    { 0x3041, 0x01 }, //
    { 0x3042, 0x88 }, // PIX_HWIDTH
    { 0x3043, 0x0B }, //
    { 0x3044, 0x24 }, // PIX_VST
    { 0x3045, 0x02 }, //
    { 0x3046, 0xD8 }, // PIX_VWIDTH
    { 0x3047, 0x0C }, //
    { 0x3050, 0x08 }, // SHR0[19:0]
    { 0x30C1, 0x00 }, // XVS_DRV[1:0]
    { 0x30D9, 0x02 }, // DIG_CLP_VSTART (binning 2/2)
    { 0x30DA, 0x01 }, // DIG_CLP_VNUM (binning 2/2)
    { 0x3116, 0x23 }, // INCKSEL2[7:0]
    { 0x3118, 0xC6 }, // INCKSEL3[10:0]
    { 0x311A, 0xE7 }, // INCKSEL4[10:0]
    { 0x311E, 0x23 }, // INCKSEL5[7:0]
    { 0x32D4, 0x21 }, // -
    { 0x32EC, 0xA1 }, // -
    { 0x3452, 0x7F }, // -
    { 0x3453, 0x03 }, // -
    { 0x358A, 0x04 }, // -
    { 0x35A1, 0x02 }, // -
    { 0x36BC, 0x0C }, // -
    { 0x36CC, 0x53 }, // -
    { 0x36CD, 0x00 }, // -
    { 0x36CE, 0x3C }, // -
    { 0x36D0, 0x8C }, // -
    { 0x36D1, 0x00 }, // -
    { 0x36D2, 0x71 }, // -
    { 0x36D4, 0x3C }, // -
    { 0x36D6, 0x53 }, // -
    { 0x36D7, 0x00 }, // -
    { 0x36D8, 0x71 }, // -
    { 0x36DA, 0x8C }, // -
    { 0x36DB, 0x00 }, // -
    { 0x3701, 0x00 }, // ADBIT1[7:0]
    { 0x3724, 0x02 }, // -
    { 0x3726, 0x02 }, // -
    { 0x3732, 0x02 }, // -
    { 0x3734, 0x03 }, // -
    { 0x3736, 0x03 }, // -
    { 0x3742, 0x03 }, // -
    { 0x3862, 0xE0 }, // -
    { 0x38CC, 0x30 }, // -
    { 0x38CD, 0x2F }, // -
    { 0x395C, 0x0C }, // -
    { 0x3A42, 0xD1 }, // -
    { 0x3A4C, 0x77 }, // -
    { 0x3AE0, 0x02 }, // -
    { 0x3AEC, 0x0C }, // -
    { 0x3B00, 0x2E }, // -
    { 0x3B06, 0x29 }, // -
    { 0x3B98, 0x25 }, // -
    { 0x3B99, 0x21 }, // -
    { 0x3B9B, 0x13 }, // -
    { 0x3B9C, 0x13 }, // -
    { 0x3B9D, 0x13 }, // -
    { 0x3B9E, 0x13 }, // -
    { 0x3BA1, 0x00 }, // -
    { 0x3BA2, 0x06 }, // -
    { 0x3BA3, 0x0B }, // -
    { 0x3BA4, 0x10 }, // -
    { 0x3BA5, 0x14 }, // -
    { 0x3BA6, 0x18 }, // -
    { 0x3BA7, 0x1A }, // -
    { 0x3BA8, 0x1A }, // -
    { 0x3BA9, 0x1A }, // -
    { 0x3BAC, 0xED }, // -
    { 0x3BAD, 0x01 }, // -
    { 0x3BAE, 0xF6 }, // -
    { 0x3BAF, 0x02 }, // -
    { 0x3BB0, 0xA2 }, // -
    { 0x3BB1, 0x03 }, // -
    { 0x3BB2, 0xE0 }, // -
    { 0x3BB3, 0x03 }, // -
    { 0x3BB4, 0xE0 }, // -
    { 0x3BB5, 0x03 }, // -
    { 0x3BB6, 0xE0 }, // -
    { 0x3BB7, 0x03 }, // -
    { 0x3BB8, 0xE0 }, // -
    { 0x3BBA, 0xE0 }, // -
    { 0x3BBC, 0xDA }, // -
    { 0x3BBE, 0x88 }, // -
    { 0x3BC0, 0x44 }, // -
    { 0x3BC2, 0x7B }, // -
    { 0x3BC4, 0xA2 }, // -
    { 0x3BC8, 0xBD }, // -
    { 0x3BCA, 0xBD }, // -
    { 0x4004, 0xC0 }, // TXCLKESC_FREQ[15:0]
    { 0x4005, 0x06 }, //
    { 0x400C, 0x00 }, // INCKSEL6
    { 0x4018, 0x7F }, // TCLKPOST[15:0]
    { 0x401A, 0x37 }, // TCLKPREPARE[15:0]
    { 0x401C, 0x37 }, // TCLKTRAIL[15:0]
    { 0x401E, 0xF7 }, // TCLKZERO[15:0]
    { 0x401F, 0x00 }, //
    { 0x4020, 0x3F }, // THSPREPARE[15:0]
    { 0x4022, 0x6F }, // THSZERO[15:0]
    { 0x4024, 0x3F }, // THSTRAIL[15:0]
    { 0x4026, 0x5F }, // THSEXIT[15:0]
    { 0x4028, 0x2F }, // TLPX[15:0]
    { 0x4074, 0x01 }, // INCKSEL7 [2:0]
    { 0xFFFF, 0x24 },
    { 0x3002, 0x00 }, // Master mode start
    { 0xFFFF, 0x10 },
    { 0x3000, 0x00 }, // Operating
};
#endif /* retired vendor binned tables */

const static I2C_ARRAY Sensor_id_table[] = {
    { 0x3F12, 0x14 }, // {address of ID, ID },
    { 0x3F13, 0x75 }, // {address of ID, ID },
};

static I2C_ARRAY PatternTbl[] = {
    { 0x0000, 0x00 }, // colorbar pattern , bit 0 to enable
};

const static I2C_ARRAY expo_reg[] = {
    // SHS0 (For Linear)
    { 0x3052, 0x00 },
    { 0x3051, 0x00 },
    { 0x3050, 0x08 },
};

const static I2C_ARRAY vts_reg[] = {
    // VMAX
    { 0x3026, 0x00 }, // bit0-3-->MSB
    { 0x3025, 0x08 },
    { 0x3024, 0xCA },
};

const static I2C_ARRAY gain_reg[] = {
    { 0x3090, 0x2A }, // low bit
    { 0x3091, 0x00 }, // hcg mode,bit 4
};

// static int g_sensor_ae_min_gain = 1024;
static CUS_GAIN_GAP_ARRAY gain_gap_compensate[16] = { // compensate  gain gap
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 },
    { 0, 0 }
};

/////////////////////////////////////////////////////////////////
//       @@@@@@@                                               //
//           @@                                                //
//          @@                                                 //
//          @@@                                                //
//       @   @@                                                //
//        @@@@                                                 //
//                                                             //
//      Step 3 --  complete camera features                    //
//                                                             //
//  camera set EV, MWB, orientation, contrast, sharpness       //
//   , saturation, and Denoise can work correctly.             //
//                                                             //
/////////////////////////////////////////////////////////////////

/////////////////// I2C function definition ///////////////////
#define SensorReg_Read(_reg, _data) (handle->i2c_bus->i2c_rx(handle->i2c_bus, &(handle->i2c_cfg), _reg, _data))
#define SensorReg_Write(_reg, _data) (handle->i2c_bus->i2c_tx(handle->i2c_bus, &(handle->i2c_cfg), _reg, _data))
#define SensorRegArrayW(_reg, _len) (handle->i2c_bus->i2c_array_tx(handle->i2c_bus, &(handle->i2c_cfg), (_reg), (_len)))
#define SensorRegArrayR(_reg, _len) (handle->i2c_bus->i2c_array_rx(handle->i2c_bus, &(handle->i2c_cfg), (_reg), (_len)))

/////////////////// sensor hardware dependent ///////////////////
static int cus_camsensor_release_handle(ms_cus_sensor* handle)
{
    return SUCCESS;
}

static int pCus_poweron(ms_cus_sensor* handle, u32 idx)
{
    ISensorIfAPI* sensor_if = handle->sensor_if_api;

    // Sensor power on sequence
    sensor_if->PowerOff(idx, handle->pwdn_POLARITY); // Powerdn Pull Low
    sensor_if->Reset(idx, handle->reset_POLARITY); // Rst Pull Low
    sensor_if->SetIOPad(idx, handle->sif_bus, handle->interface_attr.attr_mipi.mipi_lane_num);
    /* CSI-MAC clock (1485Mbps modes): 216M is borderline at 1485 line
     * rates — taller/wider crops (e.g. 2952x1848) yield ZERO frames at
     * 216M. Enum 12 "432M" is NOT a real csi-mac parent (mux tops out at
     * 288M per infinity6c-clks.dtsi) and behaved oddly (~250MPix/s flat cap).
     * 288M (enum 5) is the highest real parent — use it for all 1485 modes;
     * keep proven 216M for the 891 modes. */
    if (imx415_mipi_linear[handle->video_res_supported.ulcur_res].link_mbps >= 1485)
        sensor_if->SetCSI_Clk(idx, CUS_CSI_CLK_288M);
    else
        sensor_if->SetCSI_Clk(idx, CUS_CSI_CLK_216M);
    sensor_if->SetCSI_Lane(idx, handle->interface_attr.attr_mipi.mipi_lane_num, ENABLE);
    sensor_if->SetCSI_LongPacketType(idx, 0, 0x1C00, 0);

    if (handle->interface_attr.attr_mipi.mipi_hdr_mode == CUS_HDR_MODE_SONY_DOL) {
        sensor_if->SetCSI_hdr_mode(idx, handle->interface_attr.attr_mipi.mipi_hdr_mode, 1);
    }

    sensor_if->PowerOff(idx, !handle->pwdn_POLARITY);
    // Sensor board PWDN Enable, 1.8V & 2.9V need 30ms then Pull High
    SENSOR_MSLEEP(31);
    sensor_if->Reset(idx, !handle->reset_POLARITY);
    SENSOR_UDELAY(1);
    sensor_if->MCLK(idx, 1, handle->mclk);
    SENSOR_UDELAY(20);
    SENSOR_DMSG("Sensor Power On finished\n");
    return SUCCESS;
}

static int pCus_poweroff(ms_cus_sensor* handle, u32 idx)
{
    // power/reset low
    ISensorIfAPI* sensor_if = handle->sensor_if_api;

    SENSOR_DMSG("[%s] reset low\n", __FUNCTION__);
    sensor_if->Reset(idx, handle->reset_POLARITY); // Rst Pull Low
    SENSOR_MSLEEP(1);
    sensor_if->PowerOff(idx, handle->pwdn_POLARITY); // Powerdn Pull Low
    SENSOR_MSLEEP(1);
    sensor_if->MCLK(idx, 0, handle->mclk);

    sensor_if->SetCSI_Clk(idx, CUS_CSI_CLK_DISABLE);
    if (handle->interface_attr.attr_mipi.mipi_hdr_mode == CUS_HDR_MODE_SONY_DOL) {
        sensor_if->SetCSI_hdr_mode(idx, handle->interface_attr.attr_mipi.mipi_hdr_mode, 0);
    }

    handle->orient = SENSOR_ORIT;

    return SUCCESS;
}

/* Hardware reset the sensor before each mode init.  Toggles the
 * RESET pin via poweroff→poweron to clear all I2C register state.
 * Without this, switching between binned and non-binned modes leaves
 * stale state (e.g. HADD/VADD, DIG_CLP) that causes dark image or
 * incorrect output — even across soft reboots since the sensor chip
 * stays powered. */
static void pCus_HardwareReset(ms_cus_sensor* handle)
{
    ISensorIfAPI* sensor_if = handle->sensor_if_api;
    u32 idx = 0;

    /* Assert reset (active low for IMX415) */
    sensor_if->Reset(idx, handle->reset_POLARITY);
    SENSOR_MSLEEP(5);

    /* Deassert reset */
    sensor_if->Reset(idx, !handle->reset_POLARITY);
    SENSOR_MSLEEP(20);

    SENSOR_DMSG("[%s] sensor hardware reset complete\n", __FUNCTION__);
}

/////////////////// Check Sensor Product ID /////////////////////////
static int pCus_CheckSensorProductID(ms_cus_sensor* handle)
{
    u16 sen_id_msb, sen_id_lsb;

    /* Read Product ID */
    SensorReg_Read(0x3f12, (void*)&sen_id_lsb);
    SensorReg_Read(0x3f13, (void*)&sen_id_msb); // CHIP_ID_r3F13

    return SUCCESS;
}

// Get and check sensor ID
// if i2c error or sensor id does not match then return FAIL
static int pCus_GetSensorID(ms_cus_sensor* handle, u32* id)
{
    int i, n;
    int table_length = ARRAY_SIZE(Sensor_id_table);
    I2C_ARRAY id_from_sensor[ARRAY_SIZE(Sensor_id_table)];

    for (n = 0; n < table_length; ++n) {
        id_from_sensor[n].reg = Sensor_id_table[n].reg;
        id_from_sensor[n].data = 0;
    }

    *id = 0;
    if (table_length > 8)
        table_length = 8;

    SENSOR_DMSG("\n\n[%s]", __FUNCTION__);

    for (n = 0; n < 4; ++n) // retry , until I2C success
    {
        if (SensorRegArrayR((I2C_ARRAY*)id_from_sensor, table_length) == SUCCESS) // read sensor ID from I2C
            break;
        else
            SENSOR_MSLEEP(1);
    }
    if (n >= 4)
        return FAIL;

    // convert sensor id to u32 format
    for (i = 0; i < table_length; ++i) {
        if (id_from_sensor[i].data != Sensor_id_table[i].data) {
            SENSOR_DMSG("[%s] Please Check IMX415 Sensor Insert!!\n", __FUNCTION__);
            return FAIL;
        }
        *id = id_from_sensor[i].data;
    }

    SENSOR_DMSG("[%s]IMX415 sensor ,Read sensor id, get 0x%x Success\n", __FUNCTION__, (int)*id);
    return SUCCESS;
}

static int imx415_SetPatternMode(ms_cus_sensor* handle, u32 mode)
{
    int i;
    switch (mode) {
    case 1:
        PatternTbl[0].data = 0x21; // enable
        break;
    case 0:
        PatternTbl[0].data &= 0xFE; // disable
        break;
    default:
        PatternTbl[0].data &= 0xFE; // disable
        break;
    }
    for (i = 0; i < ARRAY_SIZE(PatternTbl); i++) {
        if (SensorReg_Write(PatternTbl[i].reg, PatternTbl[i].data) != SUCCESS) {
            SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
            return FAIL;
        }
    }
    return SUCCESS;
}

#if 0 /* retired vendor binned mode (see banner above Sensor_2m_90fps table) */
static int pCus_init_2m_90fps_mipi4lane_linear(ms_cus_sensor* handle)
{
    int i, cnt = 0;

    if (pCus_CheckSensorProductID(handle) == FAIL) {
        return FAIL;
    }

    for (i = 0; i < ARRAY_SIZE(Sensor_2m_90fps_init_table_4lane_linear); i++) {
        if (Sensor_2m_90fps_init_table_4lane_linear[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_2m_90fps_init_table_4lane_linear[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_2m_90fps_init_table_4lane_linear[i].reg, Sensor_2m_90fps_init_table_4lane_linear[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
                // SENSOR_UDELAY(1);
            }
        }
    }

    return SUCCESS;
}
#endif /* retired pCus_init_2m_90fps */

/* Non-binned base register table — HMAX=1100, WINMODE=crop, no PIX regs.
 * PIX registers are computed dynamically from the mode table resolution. */
const static I2C_ARRAY Sensor_nobinned_base_init_table[] = {
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME
    { 0x300A, 0x42 }, // CPWAIT_TIME
    { 0x301C, 0x04 }, // WINMODE (crop)
    { 0x3020, 0x00 }, // HADD = no binning
    { 0x3021, 0x00 }, // VADD = no binning
    { 0x3022, 0x00 }, // ADDMODE = no binning
    { 0x3024, 0xCA }, // VMAX default (overridden by SetFPS)
    { 0x3025, 0x08 },
    { 0x3028, 0x4C }, // HMAX = 1100 = 0x044C
    { 0x3029, 0x04 },
    { 0x3031, 0x00 }, // ADBIT = 10-bit ADC
    { 0x3032, 0x01 }, // VMAX MSB — must be 0x01, default 0x00 produces dark image
    { 0x3033, 0x05 }, // SYS_MODE (891Mbps)
    { 0x3050, 0x08 }, // SHR0
    { 0x30C1, 0x00 },
    { 0x30D9, 0x06 }, // DIG_CLP_VSTART (all-pixel; explicit — stale binned value
    { 0x30DA, 0x02 }, // DIG_CLP_VNUM (all-pixel)   survives teardown/warm reboot)
    { 0x3116, 0x23 }, // INCKSEL2
    { 0x3118, 0xC6 }, // INCKSEL3 (I6C clock)
    { 0x311A, 0xE7 }, // INCKSEL4
    { 0x311E, 0x23 }, // INCKSEL5
    { 0x32D4, 0x21 }, { 0x32EC, 0xA1 },
    { 0x3452, 0x7F }, { 0x3453, 0x03 },
    { 0x358A, 0x04 }, { 0x35A1, 0x02 }, { 0x36BC, 0x0C },
    { 0x36CC, 0x53 }, { 0x36CD, 0x00 }, { 0x36CE, 0x3C },
    { 0x36D0, 0x8C }, { 0x36D1, 0x00 }, { 0x36D2, 0x71 },
    { 0x36D4, 0x3C }, { 0x36D6, 0x53 }, { 0x36D7, 0x00 },
    { 0x36D8, 0x71 }, { 0x36DA, 0x8C }, { 0x36DB, 0x00 },
    { 0x3701, 0x00 },
    { 0x3724, 0x02 }, { 0x3726, 0x02 }, { 0x3732, 0x02 },
    { 0x3734, 0x03 }, { 0x3736, 0x03 }, { 0x3742, 0x03 },
    { 0x3862, 0xE0 }, { 0x38CC, 0x30 }, { 0x38CD, 0x2F },
    { 0x395C, 0x0C }, { 0x3A42, 0xD1 }, { 0x3A4C, 0x77 },
    { 0x3AE0, 0x02 }, { 0x3AEC, 0x0C },
    { 0x3B00, 0x2E }, { 0x3B06, 0x29 },
    { 0x3B98, 0x25 }, { 0x3B99, 0x21 },
    { 0x3B9B, 0x13 }, { 0x3B9C, 0x13 }, { 0x3B9D, 0x13 }, { 0x3B9E, 0x13 },
    { 0x3BA1, 0x00 }, { 0x3BA2, 0x06 }, { 0x3BA3, 0x0B }, { 0x3BA4, 0x10 },
    { 0x3BA5, 0x14 }, { 0x3BA6, 0x18 }, { 0x3BA7, 0x1A }, { 0x3BA8, 0x1A },
    { 0x3BA9, 0x1A },
    { 0x3BAC, 0xED }, { 0x3BAD, 0x01 }, { 0x3BAE, 0xF6 }, { 0x3BAF, 0x02 },
    { 0x3BB0, 0xA2 }, { 0x3BB1, 0x03 }, { 0x3BB2, 0xE0 }, { 0x3BB3, 0x03 },
    { 0x3BB4, 0xE0 }, { 0x3BB5, 0x03 }, { 0x3BB6, 0xE0 }, { 0x3BB7, 0x03 },
    { 0x3BB8, 0xE0 }, { 0x3BBA, 0xE0 }, { 0x3BBC, 0xDA }, { 0x3BBE, 0x88 },
    { 0x3BC0, 0x44 }, { 0x3BC2, 0x7B }, { 0x3BC4, 0xA2 },
    { 0x3BC8, 0xBD }, { 0x3BCA, 0xBD },
    { 0x4004, 0xC0 }, { 0x4005, 0x06 }, { 0x400C, 0x00 },
    { 0x4018, 0x7F }, { 0x401A, 0x37 }, { 0x401C, 0x37 },
    { 0x401E, 0xF7 }, { 0x401F, 0x00 }, { 0x4020, 0x3F },
    { 0x4022, 0x6F }, { 0x4024, 0x3F }, { 0x4026, 0x5F },
    { 0x4028, 0x2F }, { 0x4074, 0x01 },
    { 0xFFFF, 0x24 }, { 0x3002, 0x00 }, { 0xFFFF, 0x10 }, { 0x3000, 0x00 },
};

/* Both 30fps and 59fps non-binned modes use the same base init table
 * (identical HMAX=1100, I6C clocks). Only VTS/fps differ in SetVideoRes. */

/* Dynamic non-binned init — computes PIX crop from mode table resolution.
 * Non-binned: PIX_HWIDTH=W, PIX_VWIDTH=H*2, centered and 8-aligned. */
static int pCus_init_nobinned_dynamic(ms_cus_sensor* handle)
{
    int i, cnt = 0;
    u32 res_idx = handle->video_res_supported.ulcur_res;
    u32 w = imx415_mipi_linear[res_idx].senout.width;
    u32 h = imx415_mipi_linear[res_idx].senout.height;
    u16 pix_hwidth = (w <= 3864) ? (u16)w : 3864;
    u16 pix_vwidth = (h * 2 <= 4384) ? (u16)(h * 2) : 4384;
    u16 pix_hst = ((3864 - pix_hwidth) / 2) & ~7; /* 8-aligned center */
    u16 pix_vst = (pix_vwidth < 4384) ? (((4384 - pix_vwidth) / 2) & ~7) : 0;

    pCus_HardwareReset(handle);
    if (pCus_CheckSensorProductID(handle) == FAIL)
        return FAIL;

    /* Write base non-binned registers */
    for (i = 0; i < ARRAY_SIZE(Sensor_nobinned_base_init_table); i++) {
        if (Sensor_nobinned_base_init_table[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_nobinned_base_init_table[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_nobinned_base_init_table[i].reg,
                    Sensor_nobinned_base_init_table[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
            }
        }
    }

    /* Write computed PIX crop registers */
    SensorReg_Write(0x3040, pix_hst & 0xFF);
    SensorReg_Write(0x3041, (pix_hst >> 8) & 0xFF);
    SensorReg_Write(0x3042, pix_hwidth & 0xFF);
    SensorReg_Write(0x3043, (pix_hwidth >> 8) & 0xFF);
    SensorReg_Write(0x3044, pix_vst & 0xFF);
    SensorReg_Write(0x3045, (pix_vst >> 8) & 0xFF);
    SensorReg_Write(0x3046, pix_vwidth & 0xFF);
    SensorReg_Write(0x3047, (pix_vwidth >> 8) & 0xFF);

    return SUCCESS;
}

#if 0 /* retired vendor binned mode (see banner above Sensor_2m_90fps table) */
static int pCus_init_1m_120fps_mipi4lane_linear(ms_cus_sensor* handle)
{
    int i, cnt = 0;
    pCus_HardwareReset(handle);
    if (pCus_CheckSensorProductID(handle) == FAIL) {
        return FAIL;
    }

    for (i = 0; i < ARRAY_SIZE(Sensor_1m_120fps_init_table_4lane_linear); i++) {
        if (Sensor_1m_120fps_init_table_4lane_linear[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_1m_120fps_init_table_4lane_linear[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_1m_120fps_init_table_4lane_linear[i].reg, Sensor_1m_120fps_init_table_4lane_linear[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
                // SENSOR_UDELAY(1);
            }
        }
    }

    return SUCCESS;
}
#endif /* retired pCus_init_1m_120fps */

/* Mode 1: 2952x1656@50fps NON-BINNED, 1485Mbps MIPI — port of the stock SSC378QE
 * (Infinity6C) IMX415 driver's Sensor_5m_60fps_init_table_4lane_linear (which proved
 * 1485Mbps locks on I6C: the vendor ships this exact table, so the legacy "1485
 * stalls on I6C" finding was an incomplete-attempt artifact — the old hybrid grafted
 * 1485 D-PHY timing onto the 891 analog front-end and omitted this mode's analog
 * block, 0x32D4..0x3BCA).  Only VMAX differs from the vendor table: 1720 -> 2289
 * paces the sensor to 50.0 fps, the ISP m2m ceiling for 4.89 MPix.  Everything else
 * byte-for-byte — do not hand-tune.  HMAX=0x028C=652, SYS_MODE=0x08, INCKSEL3=0xA5.
 * PIX: HST=0x01BC, HWIDTH=0x0B88(2952), VST=0x01F8, VWIDTH=0x0CF0(1656*2 non-binned). */
const static I2C_ARRAY Sensor_5m_50fps_1485_init_table_4lane_linear[] = {
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME[9:0]
    { 0x300A, 0x42 }, // CPWAIT_TIME[9:0]
    { 0x301C, 0x04 }, // WINMODE (cropping mode)
    { 0x3024, 0xF1 }, // VMAX = 0x08F1 = 2289 (50.0 fps @ 8.736us line)
    { 0x3025, 0x08 }, //
    { 0x3028, 0x8C }, // HMAX = 0x028C = 652
    { 0x3029, 0x02 }, //
    { 0x3031, 0x00 }, // ADBIT (10bit)
    { 0x3032, 0x01 }, // must be 0x01 (vendor table ships 0x00 = dark image; same bug as the 2026-05-14 891-table fix, live-poke verified)
    { 0x3033, 0x08 }, // SYS_MODE (1485Mbps)
    { 0x3020, 0x00 }, // HADD (all-pixel)
    { 0x3021, 0x00 }, // VADD (all-pixel)
    { 0x3022, 0x00 }, // ADDMODE (all-pixel) — must be explicit: a prior binned-mode
                      // run leaves these =1, and the sensor keeps that state across
                      // pipeline teardown AND warm reboot (chip stays powered; the
                      // RESET-pin toggle does not clear it). Stale binning breaks
                      // every warm 1485 bring-up until a power-cycle.
    { 0x3040, 0xBC }, // PIX_HST = 0x01BC = 444
    { 0x3041, 0x01 }, //
    { 0x3042, 0x88 }, // PIX_HWIDTH = 0x0B88 = 2952
    { 0x3043, 0x0B }, //
    { 0x3044, 0xF8 }, // PIX_VST = 0x01F8 = 504
    { 0x3045, 0x01 }, //
    { 0x3046, 0xF0 }, // PIX_VWIDTH = 0x0CF0 = 3312 (1656*2)
    { 0x3047, 0x0C }, //
    { 0x3050, 0x08 }, // SHR0[19:0]
    { 0x30C1, 0x00 }, // XVS_DRV[1:0]
    { 0x30D9, 0x06 }, // DIG_CLP_VSTART (all-pixel; binned tables set 0x02 — explicit
    { 0x30DA, 0x02 }, // DIG_CLP_VNUM (all-pixel; binned 0x01)   for same reason as HADD)
    { 0x3116, 0x23 }, // INCKSEL2
    { 0x3118, 0xA5 }, // INCKSEL3 (1485Mbps)
    { 0x311A, 0xE7 }, // INCKSEL4
    { 0x311E, 0x23 }, // INCKSEL5
    { 0x32D4, 0x21 }, // analog front-end (1485-specific block begins)
    { 0x32EC, 0xA1 }, //
    { 0x3452, 0x7F }, //
    { 0x3453, 0x03 }, //
    { 0x358A, 0x04 }, //
    { 0x35A1, 0x02 }, //
    { 0x36BC, 0x0C }, //
    { 0x36CC, 0x53 }, //
    { 0x36CD, 0x00 }, //
    { 0x36CE, 0x3C }, //
    { 0x36D0, 0x8C }, //
    { 0x36D1, 0x00 }, //
    { 0x36D2, 0x71 }, //
    { 0x36D4, 0x3C }, //
    { 0x36D6, 0x53 }, //
    { 0x36D7, 0x00 }, //
    { 0x36D8, 0x71 }, //
    { 0x36DA, 0x8C }, //
    { 0x36DB, 0x00 }, //
    { 0x3701, 0x00 }, // ADBIT1[7:0]
    { 0x3724, 0x02 }, //
    { 0x3726, 0x02 }, //
    { 0x3732, 0x02 }, //
    { 0x3734, 0x03 }, //
    { 0x3736, 0x03 }, //
    { 0x3742, 0x03 }, //
    { 0x3862, 0xE0 }, //
    { 0x38CC, 0x30 }, //
    { 0x38CD, 0x2F }, //
    { 0x395C, 0x0C }, //
    { 0x3A42, 0xD1 }, //
    { 0x3A4C, 0x77 }, //
    { 0x3AE0, 0x02 }, //
    { 0x3AEC, 0x0C }, //
    { 0x3B00, 0x2E }, //
    { 0x3B06, 0x29 }, //
    { 0x3B98, 0x25 }, //
    { 0x3B99, 0x21 }, //
    { 0x3B9B, 0x13 }, //
    { 0x3B9C, 0x13 }, //
    { 0x3B9D, 0x13 }, //
    { 0x3B9E, 0x13 }, //
    { 0x3BA1, 0x00 }, //
    { 0x3BA2, 0x06 }, //
    { 0x3BA3, 0x0B }, //
    { 0x3BA4, 0x10 }, //
    { 0x3BA5, 0x14 }, //
    { 0x3BA6, 0x18 }, //
    { 0x3BA7, 0x1A }, //
    { 0x3BA8, 0x1A }, //
    { 0x3BA9, 0x1A }, //
    { 0x3BAC, 0xED }, //
    { 0x3BAD, 0x01 }, //
    { 0x3BAE, 0xF6 }, //
    { 0x3BAF, 0x02 }, //
    { 0x3BB0, 0xA2 }, //
    { 0x3BB1, 0x03 }, //
    { 0x3BB2, 0xE0 }, //
    { 0x3BB3, 0x03 }, //
    { 0x3BB4, 0xE0 }, //
    { 0x3BB5, 0x03 }, //
    { 0x3BB6, 0xE0 }, //
    { 0x3BB7, 0x03 }, //
    { 0x3BB8, 0xE0 }, //
    { 0x3BBA, 0xE0 }, //
    { 0x3BBC, 0xDA }, //
    { 0x3BBE, 0x88 }, //
    { 0x3BC0, 0x44 }, //
    { 0x3BC2, 0x7B }, //
    { 0x3BC4, 0xA2 }, //
    { 0x3BC8, 0xBD }, //
    { 0x3BCA, 0xBD }, // analog front-end (1485-specific block ends)
    { 0x4004, 0xC0 }, // TXCLKESC_FREQ[15:0]
    { 0x4005, 0x06 }, //
    { 0x400C, 0x01 }, // INCKSEL6 (1485)
    { 0x4018, 0xA7 }, // TCLKPOST (1485)
    { 0x401A, 0x57 }, // TCLKPREPARE (1485)
    { 0x401C, 0x5F }, // TCLKTRAIL (1485)
    { 0x401E, 0x97 }, // TCLKZERO (1485)
    { 0x401F, 0x01 }, //
    { 0x4020, 0x5F }, // THSPREPARE (1485)
    { 0x4022, 0xAF }, // THSZERO (1485)
    { 0x4024, 0x5F }, // THSTRAIL (1485)
    { 0x4026, 0x9F }, // THSEXIT (1485)
    { 0x4028, 0x4F }, // TLPX (1485)
    { 0x4074, 0x00 }, // INCKSEL7 (1485)
    { 0xFFFF, 0x24 }, // sleep 36ms
    { 0x3002, 0x00 }, // Master mode start
    { 0xFFFF, 0x10 }, // sleep 16ms
    { 0x3000, 0x00 }, // Operating
};

static int pCus_init_5m_50fps_1485_mipi4lane_linear(ms_cus_sensor* handle)
{
    int i, cnt = 0;
    pCus_HardwareReset(handle);
    if (pCus_CheckSensorProductID(handle) == FAIL) {
        return FAIL;
    }

    for (i = 0; i < ARRAY_SIZE(Sensor_5m_50fps_1485_init_table_4lane_linear); i++) {
        if (Sensor_5m_50fps_1485_init_table_4lane_linear[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_5m_50fps_1485_init_table_4lane_linear[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_5m_50fps_1485_init_table_4lane_linear[i].reg,
                    Sensor_5m_50fps_1485_init_table_4lane_linear[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
            }
        }
    }

    return SUCCESS;
}

/* Mode 2: 2688x1512@60 — ISP-ceiling-sized max non-binned 16:9 @60 (4.06 MPix).
 * Replaces the earlier 2952x1368 (19.4:9) sizing with the sensor-native 16:9
 * aspect at the same pixel budget.  Generated from the 5m 1485 skeleton:
 * VMAX=1908 (60.0fps @ 8.736us line), crop HST=(3864-2688)/2=588,
 * HWIDTH=2688, VST=2192-1512=680, VWIDTH=2*1512=3024. */
const static I2C_ARRAY Sensor_2688x1512_60_init_table_4lane_linear[] = {
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME[9:0]
    { 0x300A, 0x42 }, // CPWAIT_TIME[9:0]
    { 0x301C, 0x04 }, // WINMODE (cropping mode)
    { 0x3024, 0x74 },
    { 0x3025, 0x07 },
    { 0x3028, 0x8C }, // HMAX = 0x028C = 652
    { 0x3029, 0x02 }, //
    { 0x3031, 0x00 }, // ADBIT (10bit)
    { 0x3032, 0x01 }, // must be 0x01 (vendor table ships 0x00 = dark image; same bug as the 2026-05-14 891-table fix, live-poke verified)
    { 0x3033, 0x08 }, // SYS_MODE (1485Mbps)
    { 0x3020, 0x00 }, // HADD (all-pixel)
    { 0x3021, 0x00 }, // VADD (all-pixel)
    { 0x3022, 0x00 }, // ADDMODE (all-pixel) — must be explicit: a prior binned-mode
                      // run leaves these =1, and the sensor keeps that state across
                      // pipeline teardown AND warm reboot (chip stays powered; the
                      // RESET-pin toggle does not clear it). Stale binning breaks
                      // every warm 1485 bring-up until a power-cycle.
    { 0x3040, 0x4C }, // PIX_HST = 588 = 0x024C (centered 2688 in 3864)
    { 0x3041, 0x02 },
    { 0x3042, 0x80 }, // PIX_HWIDTH = 2688 = 0x0A80
    { 0x3043, 0x0A },
    { 0x3044, 0xA8 }, // PIX_VST = 680 = 0x02A8 (2192 - 1512)
    { 0x3045, 0x02 },
    { 0x3046, 0xD0 }, // PIX_VWIDTH = 3024 = 0x0BD0 (1512*2 non-binned)
    { 0x3047, 0x0B },
    { 0x3050, 0x08 }, // SHR0[19:0]
    { 0x30C1, 0x00 }, // XVS_DRV[1:0]
    { 0x30D9, 0x06 }, // DIG_CLP_VSTART (all-pixel; binned tables set 0x02 — explicit
    { 0x30DA, 0x02 }, // DIG_CLP_VNUM (all-pixel; binned 0x01)   for same reason as HADD)
    { 0x3116, 0x23 }, // INCKSEL2
    { 0x3118, 0xA5 }, // INCKSEL3 (1485Mbps)
    { 0x311A, 0xE7 }, // INCKSEL4
    { 0x311E, 0x23 }, // INCKSEL5
    { 0x32D4, 0x21 }, // analog front-end (1485-specific block begins)
    { 0x32EC, 0xA1 }, //
    { 0x3452, 0x7F }, //
    { 0x3453, 0x03 }, //
    { 0x358A, 0x04 }, //
    { 0x35A1, 0x02 }, //
    { 0x36BC, 0x0C }, //
    { 0x36CC, 0x53 }, //
    { 0x36CD, 0x00 }, //
    { 0x36CE, 0x3C }, //
    { 0x36D0, 0x8C }, //
    { 0x36D1, 0x00 }, //
    { 0x36D2, 0x71 }, //
    { 0x36D4, 0x3C }, //
    { 0x36D6, 0x53 }, //
    { 0x36D7, 0x00 }, //
    { 0x36D8, 0x71 }, //
    { 0x36DA, 0x8C }, //
    { 0x36DB, 0x00 }, //
    { 0x3701, 0x00 }, // ADBIT1[7:0]
    { 0x3724, 0x02 }, //
    { 0x3726, 0x02 }, //
    { 0x3732, 0x02 }, //
    { 0x3734, 0x03 }, //
    { 0x3736, 0x03 }, //
    { 0x3742, 0x03 }, //
    { 0x3862, 0xE0 }, //
    { 0x38CC, 0x30 }, //
    { 0x38CD, 0x2F }, //
    { 0x395C, 0x0C }, //
    { 0x3A42, 0xD1 }, //
    { 0x3A4C, 0x77 }, //
    { 0x3AE0, 0x02 }, //
    { 0x3AEC, 0x0C }, //
    { 0x3B00, 0x2E }, //
    { 0x3B06, 0x29 }, //
    { 0x3B98, 0x25 }, //
    { 0x3B99, 0x21 }, //
    { 0x3B9B, 0x13 }, //
    { 0x3B9C, 0x13 }, //
    { 0x3B9D, 0x13 }, //
    { 0x3B9E, 0x13 }, //
    { 0x3BA1, 0x00 }, //
    { 0x3BA2, 0x06 }, //
    { 0x3BA3, 0x0B }, //
    { 0x3BA4, 0x10 }, //
    { 0x3BA5, 0x14 }, //
    { 0x3BA6, 0x18 }, //
    { 0x3BA7, 0x1A }, //
    { 0x3BA8, 0x1A }, //
    { 0x3BA9, 0x1A }, //
    { 0x3BAC, 0xED }, //
    { 0x3BAD, 0x01 }, //
    { 0x3BAE, 0xF6 }, //
    { 0x3BAF, 0x02 }, //
    { 0x3BB0, 0xA2 }, //
    { 0x3BB1, 0x03 }, //
    { 0x3BB2, 0xE0 }, //
    { 0x3BB3, 0x03 }, //
    { 0x3BB4, 0xE0 }, //
    { 0x3BB5, 0x03 }, //
    { 0x3BB6, 0xE0 }, //
    { 0x3BB7, 0x03 }, //
    { 0x3BB8, 0xE0 }, //
    { 0x3BBA, 0xE0 }, //
    { 0x3BBC, 0xDA }, //
    { 0x3BBE, 0x88 }, //
    { 0x3BC0, 0x44 }, //
    { 0x3BC2, 0x7B }, //
    { 0x3BC4, 0xA2 }, //
    { 0x3BC8, 0xBD }, //
    { 0x3BCA, 0xBD }, // analog front-end (1485-specific block ends)
    { 0x4004, 0xC0 }, // TXCLKESC_FREQ[15:0]
    { 0x4005, 0x06 }, //
    { 0x400C, 0x01 }, // INCKSEL6 (1485)
    { 0x4018, 0xA7 }, // TCLKPOST (1485)
    { 0x401A, 0x57 }, // TCLKPREPARE (1485)
    { 0x401C, 0x5F }, // TCLKTRAIL (1485)
    { 0x401E, 0x97 }, // TCLKZERO (1485)
    { 0x401F, 0x01 }, //
    { 0x4020, 0x5F }, // THSPREPARE (1485)
    { 0x4022, 0xAF }, // THSZERO (1485)
    { 0x4024, 0x5F }, // THSTRAIL (1485)
    { 0x4026, 0x9F }, // THSEXIT (1485)
    { 0x4028, 0x4F }, // TLPX (1485)
    { 0x4074, 0x00 }, // INCKSEL7 (1485)
    { 0xFFFF, 0x24 }, // sleep 36ms
    { 0x3002, 0x00 }, // Master mode start
    { 0xFFFF, 0x10 }, // sleep 16ms
    { 0x3000, 0x00 }, // Operating
};

static int pCus_init_2688x1512_60_mipi4lane_linear(ms_cus_sensor* handle)
{
    int i, cnt = 0;
    pCus_HardwareReset(handle);
    if (pCus_CheckSensorProductID(handle) == FAIL) {
        return FAIL;
    }

    for (i = 0; i < ARRAY_SIZE(Sensor_2688x1512_60_init_table_4lane_linear); i++) {
        if (Sensor_2688x1512_60_init_table_4lane_linear[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_2688x1512_60_init_table_4lane_linear[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_2688x1512_60_init_table_4lane_linear[i].reg,
                    Sensor_2688x1512_60_init_table_4lane_linear[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
            }
        }
    }

    return SUCCESS;
}

/* Mode 3: 2112x1184@90 — ISP-ceiling-sized max non-binned @90 (2.50 MPix).
 * Generated from the 5m 1485 skeleton: VMAX=1272 (90.0fps @ 8.736us line),
 * crop HST=(3864-2112)/2=876, HWIDTH=2112, VST=2192-1184=1008, VWIDTH=2*1184=2368. */
const static I2C_ARRAY Sensor_2112x1184_90_init_table_4lane_linear[] = {
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME[9:0]
    { 0x300A, 0x42 }, // CPWAIT_TIME[9:0]
    { 0x301C, 0x04 }, // WINMODE (cropping mode)
    { 0x3024, 0xF8 },
    { 0x3025, 0x04 },
    { 0x3028, 0x8C }, // HMAX = 0x028C = 652
    { 0x3029, 0x02 }, //
    { 0x3031, 0x00 }, // ADBIT (10bit)
    { 0x3032, 0x01 }, // must be 0x01 (vendor table ships 0x00 = dark image; same bug as the 2026-05-14 891-table fix, live-poke verified)
    { 0x3033, 0x08 }, // SYS_MODE (1485Mbps)
    { 0x3020, 0x00 }, // HADD (all-pixel)
    { 0x3021, 0x00 }, // VADD (all-pixel)
    { 0x3022, 0x00 }, // ADDMODE (all-pixel) — must be explicit: a prior binned-mode
                      // run leaves these =1, and the sensor keeps that state across
                      // pipeline teardown AND warm reboot (chip stays powered; the
                      // RESET-pin toggle does not clear it). Stale binning breaks
                      // every warm 1485 bring-up until a power-cycle.
    { 0x3040, 0x6C },
    { 0x3041, 0x03 },
    { 0x3042, 0x40 },
    { 0x3043, 0x08 },
    { 0x3044, 0xF0 },
    { 0x3045, 0x03 },
    { 0x3046, 0x40 },
    { 0x3047, 0x09 },
    { 0x3050, 0x08 }, // SHR0[19:0]
    { 0x30C1, 0x00 }, // XVS_DRV[1:0]
    { 0x30D9, 0x06 }, // DIG_CLP_VSTART (all-pixel; binned tables set 0x02 — explicit
    { 0x30DA, 0x02 }, // DIG_CLP_VNUM (all-pixel; binned 0x01)   for same reason as HADD)
    { 0x3116, 0x23 }, // INCKSEL2
    { 0x3118, 0xA5 }, // INCKSEL3 (1485Mbps)
    { 0x311A, 0xE7 }, // INCKSEL4
    { 0x311E, 0x23 }, // INCKSEL5
    { 0x32D4, 0x21 }, // analog front-end (1485-specific block begins)
    { 0x32EC, 0xA1 }, //
    { 0x3452, 0x7F }, //
    { 0x3453, 0x03 }, //
    { 0x358A, 0x04 }, //
    { 0x35A1, 0x02 }, //
    { 0x36BC, 0x0C }, //
    { 0x36CC, 0x53 }, //
    { 0x36CD, 0x00 }, //
    { 0x36CE, 0x3C }, //
    { 0x36D0, 0x8C }, //
    { 0x36D1, 0x00 }, //
    { 0x36D2, 0x71 }, //
    { 0x36D4, 0x3C }, //
    { 0x36D6, 0x53 }, //
    { 0x36D7, 0x00 }, //
    { 0x36D8, 0x71 }, //
    { 0x36DA, 0x8C }, //
    { 0x36DB, 0x00 }, //
    { 0x3701, 0x00 }, // ADBIT1[7:0]
    { 0x3724, 0x02 }, //
    { 0x3726, 0x02 }, //
    { 0x3732, 0x02 }, //
    { 0x3734, 0x03 }, //
    { 0x3736, 0x03 }, //
    { 0x3742, 0x03 }, //
    { 0x3862, 0xE0 }, //
    { 0x38CC, 0x30 }, //
    { 0x38CD, 0x2F }, //
    { 0x395C, 0x0C }, //
    { 0x3A42, 0xD1 }, //
    { 0x3A4C, 0x77 }, //
    { 0x3AE0, 0x02 }, //
    { 0x3AEC, 0x0C }, //
    { 0x3B00, 0x2E }, //
    { 0x3B06, 0x29 }, //
    { 0x3B98, 0x25 }, //
    { 0x3B99, 0x21 }, //
    { 0x3B9B, 0x13 }, //
    { 0x3B9C, 0x13 }, //
    { 0x3B9D, 0x13 }, //
    { 0x3B9E, 0x13 }, //
    { 0x3BA1, 0x00 }, //
    { 0x3BA2, 0x06 }, //
    { 0x3BA3, 0x0B }, //
    { 0x3BA4, 0x10 }, //
    { 0x3BA5, 0x14 }, //
    { 0x3BA6, 0x18 }, //
    { 0x3BA7, 0x1A }, //
    { 0x3BA8, 0x1A }, //
    { 0x3BA9, 0x1A }, //
    { 0x3BAC, 0xED }, //
    { 0x3BAD, 0x01 }, //
    { 0x3BAE, 0xF6 }, //
    { 0x3BAF, 0x02 }, //
    { 0x3BB0, 0xA2 }, //
    { 0x3BB1, 0x03 }, //
    { 0x3BB2, 0xE0 }, //
    { 0x3BB3, 0x03 }, //
    { 0x3BB4, 0xE0 }, //
    { 0x3BB5, 0x03 }, //
    { 0x3BB6, 0xE0 }, //
    { 0x3BB7, 0x03 }, //
    { 0x3BB8, 0xE0 }, //
    { 0x3BBA, 0xE0 }, //
    { 0x3BBC, 0xDA }, //
    { 0x3BBE, 0x88 }, //
    { 0x3BC0, 0x44 }, //
    { 0x3BC2, 0x7B }, //
    { 0x3BC4, 0xA2 }, //
    { 0x3BC8, 0xBD }, //
    { 0x3BCA, 0xBD }, // analog front-end (1485-specific block ends)
    { 0x4004, 0xC0 }, // TXCLKESC_FREQ[15:0]
    { 0x4005, 0x06 }, //
    { 0x400C, 0x01 }, // INCKSEL6 (1485)
    { 0x4018, 0xA7 }, // TCLKPOST (1485)
    { 0x401A, 0x57 }, // TCLKPREPARE (1485)
    { 0x401C, 0x5F }, // TCLKTRAIL (1485)
    { 0x401E, 0x97 }, // TCLKZERO (1485)
    { 0x401F, 0x01 }, //
    { 0x4020, 0x5F }, // THSPREPARE (1485)
    { 0x4022, 0xAF }, // THSZERO (1485)
    { 0x4024, 0x5F }, // THSTRAIL (1485)
    { 0x4026, 0x9F }, // THSEXIT (1485)
    { 0x4028, 0x4F }, // TLPX (1485)
    { 0x4074, 0x00 }, // INCKSEL7 (1485)
    { 0xFFFF, 0x24 }, // sleep 36ms
    { 0x3002, 0x00 }, // Master mode start
    { 0xFFFF, 0x10 }, // sleep 16ms
    { 0x3000, 0x00 }, // Operating
};

static int pCus_init_2112x1184_90_mipi4lane_linear(ms_cus_sensor* handle)
{
    int i, cnt = 0;
    pCus_HardwareReset(handle);
    if (pCus_CheckSensorProductID(handle) == FAIL) {
        return FAIL;
    }

    for (i = 0; i < ARRAY_SIZE(Sensor_2112x1184_90_init_table_4lane_linear); i++) {
        if (Sensor_2112x1184_90_init_table_4lane_linear[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_2112x1184_90_init_table_4lane_linear[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_2112x1184_90_init_table_4lane_linear[i].reg,
                    Sensor_2112x1184_90_init_table_4lane_linear[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
            }
        }
    }

    return SUCCESS;
}

/* Mode 4: 1920x1080@100 — low-latency non-binned 1080p (2.07 MPix).
 * Sized ~8% under the ISP m2m ceiling (capacity ~108 fps) so the FRAMEBASE
 * queue stays empty at 100 fps — minimum latency, no ceiling shortfall.
 * Generated from the 5m 1485 skeleton: VMAX=1144 (100.06fps @ 8.736us line;
 * 64 blanking lines >= the ~42-line floor), crop HST=968 (centered,
 * 8-aligned), HWIDTH=1920, VST=2192-1080=1112, VWIDTH=2*1080=2160. */
const static I2C_ARRAY Sensor_1920x1080_100_init_table_4lane_linear[] = {
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME[9:0]
    { 0x300A, 0x42 }, // CPWAIT_TIME[9:0]
    { 0x301C, 0x04 }, // WINMODE (cropping mode)
    { 0x3024, 0x78 }, // VMAX = 0x0478 = 1144
    { 0x3025, 0x04 },
    { 0x3028, 0x8C }, // HMAX = 0x028C = 652
    { 0x3029, 0x02 }, //
    { 0x3031, 0x00 }, // ADBIT (10bit)
    { 0x3032, 0x01 }, // must be 0x01 (vendor table ships 0x00 = dark image; same bug as the 2026-05-14 891-table fix, live-poke verified)
    { 0x3033, 0x08 }, // SYS_MODE (1485Mbps)
    { 0x3020, 0x00 }, // HADD (all-pixel)
    { 0x3021, 0x00 }, // VADD (all-pixel)
    { 0x3022, 0x00 }, // ADDMODE (all-pixel) — must be explicit: a prior binned-mode
                      // run leaves these =1, and the sensor keeps that state across
                      // pipeline teardown AND warm reboot (chip stays powered; the
                      // RESET-pin toggle does not clear it). Stale binning breaks
                      // every warm 1485 bring-up until a power-cycle.
    { 0x3040, 0xC8 }, // PIX_HST = 968 = 0x03C8 (centered 1920 in 3864, 8-aligned)
    { 0x3041, 0x03 },
    { 0x3042, 0x80 }, // PIX_HWIDTH = 1920 = 0x0780
    { 0x3043, 0x07 },
    { 0x3044, 0x58 }, // PIX_VST = 1112 = 0x0458 (2192 - 1080)
    { 0x3045, 0x04 },
    { 0x3046, 0x70 }, // PIX_VWIDTH = 2160 = 0x0870 (1080*2 non-binned)
    { 0x3047, 0x08 },
    { 0x3050, 0x08 }, // SHR0[19:0]
    { 0x30C1, 0x00 }, // XVS_DRV[1:0]
    { 0x30D9, 0x06 }, // DIG_CLP_VSTART (all-pixel; binned tables set 0x02 — explicit
    { 0x30DA, 0x02 }, // DIG_CLP_VNUM (all-pixel; binned 0x01)   for same reason as HADD)
    { 0x3116, 0x23 }, // INCKSEL2
    { 0x3118, 0xA5 }, // INCKSEL3 (1485Mbps)
    { 0x311A, 0xE7 }, // INCKSEL4
    { 0x311E, 0x23 }, // INCKSEL5
    { 0x32D4, 0x21 }, // analog front-end (1485-specific block begins)
    { 0x32EC, 0xA1 }, //
    { 0x3452, 0x7F }, //
    { 0x3453, 0x03 }, //
    { 0x358A, 0x04 }, //
    { 0x35A1, 0x02 }, //
    { 0x36BC, 0x0C }, //
    { 0x36CC, 0x53 }, //
    { 0x36CD, 0x00 }, //
    { 0x36CE, 0x3C }, //
    { 0x36D0, 0x8C }, //
    { 0x36D1, 0x00 }, //
    { 0x36D2, 0x71 }, //
    { 0x36D4, 0x3C }, //
    { 0x36D6, 0x53 }, //
    { 0x36D7, 0x00 }, //
    { 0x36D8, 0x71 }, //
    { 0x36DA, 0x8C }, //
    { 0x36DB, 0x00 }, //
    { 0x3701, 0x00 }, // ADBIT1[7:0]
    { 0x3724, 0x02 }, //
    { 0x3726, 0x02 }, //
    { 0x3732, 0x02 }, //
    { 0x3734, 0x03 }, //
    { 0x3736, 0x03 }, //
    { 0x3742, 0x03 }, //
    { 0x3862, 0xE0 }, //
    { 0x38CC, 0x30 }, //
    { 0x38CD, 0x2F }, //
    { 0x395C, 0x0C }, //
    { 0x3A42, 0xD1 }, //
    { 0x3A4C, 0x77 }, //
    { 0x3AE0, 0x02 }, //
    { 0x3AEC, 0x0C }, //
    { 0x3B00, 0x2E }, //
    { 0x3B06, 0x29 }, //
    { 0x3B98, 0x25 }, //
    { 0x3B99, 0x21 }, //
    { 0x3B9B, 0x13 }, //
    { 0x3B9C, 0x13 }, //
    { 0x3B9D, 0x13 }, //
    { 0x3B9E, 0x13 }, //
    { 0x3BA1, 0x00 }, //
    { 0x3BA2, 0x06 }, //
    { 0x3BA3, 0x0B }, //
    { 0x3BA4, 0x10 }, //
    { 0x3BA5, 0x14 }, //
    { 0x3BA6, 0x18 }, //
    { 0x3BA7, 0x1A }, //
    { 0x3BA8, 0x1A }, //
    { 0x3BA9, 0x1A }, //
    { 0x3BAC, 0xED }, //
    { 0x3BAD, 0x01 }, //
    { 0x3BAE, 0xF6 }, //
    { 0x3BAF, 0x02 }, //
    { 0x3BB0, 0xA2 }, //
    { 0x3BB1, 0x03 }, //
    { 0x3BB2, 0xE0 }, //
    { 0x3BB3, 0x03 }, //
    { 0x3BB4, 0xE0 }, //
    { 0x3BB5, 0x03 }, //
    { 0x3BB6, 0xE0 }, //
    { 0x3BB7, 0x03 }, //
    { 0x3BB8, 0xE0 }, //
    { 0x3BBA, 0xE0 }, //
    { 0x3BBC, 0xDA }, //
    { 0x3BBE, 0x88 }, //
    { 0x3BC0, 0x44 }, //
    { 0x3BC2, 0x7B }, //
    { 0x3BC4, 0xA2 }, //
    { 0x3BC8, 0xBD }, //
    { 0x3BCA, 0xBD }, // analog front-end (1485-specific block ends)
    { 0x4004, 0xC0 }, // TXCLKESC_FREQ[15:0]
    { 0x4005, 0x06 }, //
    { 0x400C, 0x01 }, // INCKSEL6 (1485)
    { 0x4018, 0xA7 }, // TCLKPOST (1485)
    { 0x401A, 0x57 }, // TCLKPREPARE (1485)
    { 0x401C, 0x5F }, // TCLKTRAIL (1485)
    { 0x401E, 0x97 }, // TCLKZERO (1485)
    { 0x401F, 0x01 }, //
    { 0x4020, 0x5F }, // THSPREPARE (1485)
    { 0x4022, 0xAF }, // THSZERO (1485)
    { 0x4024, 0x5F }, // THSTRAIL (1485)
    { 0x4026, 0x9F }, // THSEXIT (1485)
    { 0x4028, 0x4F }, // TLPX (1485)
    { 0x4074, 0x00 }, // INCKSEL7 (1485)
    { 0xFFFF, 0x24 }, // sleep 36ms
    { 0x3002, 0x00 }, // Master mode start
    { 0xFFFF, 0x10 }, // sleep 16ms
    { 0x3000, 0x00 }, // Operating
};

static int pCus_init_1920x1080_100_mipi4lane_linear(ms_cus_sensor* handle)
{
    int i, cnt = 0;
    pCus_HardwareReset(handle);
    if (pCus_CheckSensorProductID(handle) == FAIL) {
        return FAIL;
    }

    for (i = 0; i < ARRAY_SIZE(Sensor_1920x1080_100_init_table_4lane_linear); i++) {
        if (Sensor_1920x1080_100_init_table_4lane_linear[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_1920x1080_100_init_table_4lane_linear[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_1920x1080_100_init_table_4lane_linear[i].reg,
                    Sensor_1920x1080_100_init_table_4lane_linear[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
            }
        }
    }

    return SUCCESS;
}






#if 0 /* retired vendor binned 1080p modes (plain + HMAX=550 ispsafe rework).
       * Kept for posterity — the ispsafe variant device-verified 60.0fps but
       * is superseded by the non-binned 2688x1512@60_1485 mode.  The binned
       * VMAX-counts-PHYSICAL-lines trap it documents lives on in
       * documentation/MARUKO_IMX415_1485_MODES.md. */
/* Maruko 1920x1080@30fps binned — full 1080p via crop+binning.
 * PIX_HWIDTH=3840(1920×2), PIX_VWIDTH=4320(1080×4).
 * PIX_HST=12, PIX_VST=0. */
const static I2C_ARRAY Sensor_1080p_binned_init_table_4lane_linear[] = {
    { 0x3000, 0x01 }, // Standby
    { 0x3002, 0x01 }, // Master mode stop
    { 0x3008, 0x5D }, // BCWAIT_TIME
    { 0x300A, 0x42 }, // CPWAIT_TIME
    { 0x301C, 0x04 }, // WINMODE (crop)
    { 0x3020, 0x01 }, // HADD (binning)
    { 0x3021, 0x01 }, // VADD (binning)
    { 0x3022, 0x01 }, // ADDMODE (2/2)
    { 0x3024, 0xA4 }, // VMAX (reuse 120fps, SetFPS adjusts)
    { 0x3025, 0x06 },
    { 0x3028, 0x6D }, // HMAX (same as 120fps)
    { 0x3029, 0x01 },
    { 0x3031, 0x00 }, // ADBIT
    { 0x3032, 0x01 }, // VMAX MSB — must be 0x01, default 0x00 produces dark image
    { 0x3033, 0x05 }, // SYS_MODE (891Mbps)
    { 0x3040, 0x0C }, // PIX_HST = 12 = 0x000C (centered 3840 in 3864)
    { 0x3041, 0x00 },
    { 0x3042, 0x00 }, // PIX_HWIDTH = 3840 = 0x0F00
    { 0x3043, 0x0F },
    { 0x3044, 0x00 }, // PIX_VST = 0
    { 0x3045, 0x00 },
    { 0x3046, 0xE0 }, // PIX_VWIDTH = 4320 = 0x10E0 (1080×4)
    { 0x3047, 0x10 },
    { 0x3050, 0x08 }, // SHR0
    { 0x30C1, 0x00 },
    { 0x30D9, 0x02 }, // DIG_CLP (binning)
    { 0x30DA, 0x01 },
    { 0x3116, 0x23 }, // INCKSEL2
    { 0x3118, 0xC6 }, // INCKSEL3 (I6C pixel clock)
    { 0x311A, 0xE7 }, // INCKSEL4
    { 0x311E, 0x23 }, // INCKSEL5
    { 0x32D4, 0x21 }, { 0x32EC, 0xA1 },
    { 0x3452, 0x7F }, { 0x3453, 0x03 },
    { 0x358A, 0x04 }, { 0x35A1, 0x02 }, { 0x36BC, 0x0C },
    { 0x36CC, 0x53 }, { 0x36CD, 0x00 }, { 0x36CE, 0x3C },
    { 0x36D0, 0x8C }, { 0x36D1, 0x00 }, { 0x36D2, 0x71 },
    { 0x36D4, 0x3C }, { 0x36D6, 0x53 }, { 0x36D7, 0x00 },
    { 0x36D8, 0x71 }, { 0x36DA, 0x8C }, { 0x36DB, 0x00 },
    { 0x3701, 0x00 },
    { 0x3724, 0x02 }, { 0x3726, 0x02 }, { 0x3732, 0x02 },
    { 0x3734, 0x03 }, { 0x3736, 0x03 }, { 0x3742, 0x03 },
    { 0x3862, 0xE0 }, { 0x38CC, 0x30 }, { 0x38CD, 0x2F },
    { 0x395C, 0x0C }, { 0x3A42, 0xD1 }, { 0x3A4C, 0x77 },
    { 0x3AE0, 0x02 }, { 0x3AEC, 0x0C },
    { 0x3B00, 0x2E }, { 0x3B06, 0x29 },
    { 0x3B98, 0x25 }, { 0x3B99, 0x21 },
    { 0x3B9B, 0x13 }, { 0x3B9C, 0x13 }, { 0x3B9D, 0x13 }, { 0x3B9E, 0x13 },
    { 0x3BA1, 0x00 }, { 0x3BA2, 0x06 }, { 0x3BA3, 0x0B }, { 0x3BA4, 0x10 },
    { 0x3BA5, 0x14 }, { 0x3BA6, 0x18 }, { 0x3BA7, 0x1A }, { 0x3BA8, 0x1A },
    { 0x3BA9, 0x1A },
    { 0x3BAC, 0xED }, { 0x3BAD, 0x01 }, { 0x3BAE, 0xF6 }, { 0x3BAF, 0x02 },
    { 0x3BB0, 0xA2 }, { 0x3BB1, 0x03 }, { 0x3BB2, 0xE0 }, { 0x3BB3, 0x03 },
    { 0x3BB4, 0xE0 }, { 0x3BB5, 0x03 }, { 0x3BB6, 0xE0 }, { 0x3BB7, 0x03 },
    { 0x3BB8, 0xE0 }, { 0x3BBA, 0xE0 }, { 0x3BBC, 0xDA }, { 0x3BBE, 0x88 },
    { 0x3BC0, 0x44 }, { 0x3BC2, 0x7B }, { 0x3BC4, 0xA2 },
    { 0x3BC8, 0xBD }, { 0x3BCA, 0xBD },
    { 0x4004, 0xC0 }, { 0x4005, 0x06 }, { 0x400C, 0x00 },
    { 0x4018, 0x7F }, { 0x401A, 0x37 }, { 0x401C, 0x37 },
    { 0x401E, 0xF7 }, { 0x401F, 0x00 }, { 0x4020, 0x3F },
    { 0x4022, 0x6F }, { 0x4024, 0x3F }, { 0x4026, 0x5F },
    { 0x4028, 0x2F }, { 0x4074, 0x01 },
    { 0xFFFF, 0x24 }, { 0x3002, 0x00 }, { 0xFFFF, 0x10 }, { 0x3000, 0x00 },
};

static int pCus_init_1080p_binned_mipi4lane_linear(ms_cus_sensor* handle)
{
    int i, cnt = 0;
    pCus_HardwareReset(handle);
    if (pCus_CheckSensorProductID(handle) == FAIL)
        return FAIL;
    for (i = 0; i < ARRAY_SIZE(Sensor_1080p_binned_init_table_4lane_linear); i++) {
        if (Sensor_1080p_binned_init_table_4lane_linear[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_1080p_binned_init_table_4lane_linear[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_1080p_binned_init_table_4lane_linear[i].reg,
                    Sensor_1080p_binned_init_table_4lane_linear[i].data) != SUCCESS) {
                cnt++;
                if (cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
            }
        }
    }
    return SUCCESS;
}

/*
 * ISP-safe 1920x1080@60 (binned). Same registers as the binned table but with
 * HMAX widened 365 -> 1100 (line period 4882 -> 14815ns) so the I6C ISP input
 * FIFO can drain each line. The binned table copied HMAX from the 120fps mode,
 * delivering lines ~3x faster than the ISP sustains (~254 MPix/s per line) ->
 * "ISP P0 FIFO FULL". HMAX comes ONLY from the init table (SetFPS/AE write VMAX
 * via tVts_reg every frame, never HMAX), so HMAX MUST be programmed BEFORE the
 * stream-start tail — a post-stream write does not latch. At HMAX=1100 the AE's
 * VMAX=1125 @60fps gives 1080 active lines in 16.67ms, matching working modes 0/1.
 */
#define IMX415_1080P_BINNED_TAIL 4 /* trailing {sleep, XMSTA=0, sleep, STBY=0} */
static int pCus_init_1080p60_ispsafe_mipi4lane_linear(ms_cus_sensor* handle)
{
    int i, cnt;
    const int n = ARRAY_SIZE(Sensor_1080p_binned_init_table_4lane_linear);
    pCus_HardwareReset(handle);
    if (pCus_CheckSensorProductID(handle) == FAIL)
        return FAIL;
    /* All config registers except the stream-start tail. */
    for (i = 0; i < n - IMX415_1080P_BINNED_TAIL; i++) {
        if (Sensor_1080p_binned_init_table_4lane_linear[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_1080p_binned_init_table_4lane_linear[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_1080p_binned_init_table_4lane_linear[i].reg,
                    Sensor_1080p_binned_init_table_4lane_linear[i].data) != SUCCESS) {
                if (++cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
            }
        }
    }
    /* ISP-safe HMAX = 0x0226 (550, 7.407us line) BEFORE stream-start
     * (overrides table 0x016D=365).  365 puts the line burst at 393 MPix/s,
     * above the 384 MPix/s ISP drain -> FIFO FULL; 550 gives 259 MPix/s
     * with margin, and VMAX=2250 (>= 2192 physical lines in 2x2 binning;
     * binned VMAX counts PHYSICAL lines, 2x the output lines) then lands
     * exactly 60.0 fps.  HMAX=1100 was tried first but caps the mode at
     * 30 fps: 60 fps would need VMAX=1125 < the physical readout length,
     * which the sensor answers with zero frames. */
    SensorReg_Write(0x3028, 0x26);
    SensorReg_Write(0x3029, 0x02);
    /* Stream-start tail (XMSTA + standby cancel). */
    for (i = n - IMX415_1080P_BINNED_TAIL; i < n; i++) {
        if (Sensor_1080p_binned_init_table_4lane_linear[i].reg == 0xffff) {
            SENSOR_MSLEEP(Sensor_1080p_binned_init_table_4lane_linear[i].data);
        } else {
            cnt = 0;
            while (SensorReg_Write(Sensor_1080p_binned_init_table_4lane_linear[i].reg,
                    Sensor_1080p_binned_init_table_4lane_linear[i].data) != SUCCESS) {
                if (++cnt >= 10) {
                    SENSOR_EMSG("[%s:%d]Sensor init fail!!\n", __FUNCTION__, __LINE__);
                    return FAIL;
                }
            }
        }
    }
    return SUCCESS;
}
#endif /* retired vendor binned 1080p modes */

static int pCus_GetVideoResNum(ms_cus_sensor* handle, u32* ulres_num)
{
    *ulres_num = handle->video_res_supported.num_res;

    return SUCCESS;
}

static int pCus_GetVideoRes(ms_cus_sensor* handle, u32 res_idx, cus_camsensor_res** res)
{
    u32 num_res = handle->video_res_supported.num_res;

    if (res_idx >= num_res) {
        SENSOR_EMSG("[%s] Please check the number of resolutions supported by the sensor!\n", __FUNCTION__);
        return FAIL;
    }

    *res = &handle->video_res_supported.res[res_idx];

    return SUCCESS;
}

static int pCus_GetCurVideoRes(ms_cus_sensor* handle, u32* cur_idx, cus_camsensor_res** res)
{
    u32 num_res = handle->video_res_supported.num_res;

    *cur_idx = handle->video_res_supported.ulcur_res;

    if (*cur_idx >= num_res) {
        SENSOR_EMSG("[%s] Please check the number of resolutions supported by the sensor!\n", __FUNCTION__);
        return FAIL;
    }

    *res = &handle->video_res_supported.res[*cur_idx];

    return SUCCESS;
}

static int pCus_SetVideoRes(ms_cus_sensor* handle, u32 res_idx)
{
    imx415_params* params = (imx415_params*)handle->private_data;
    u32 num_res = handle->video_res_supported.num_res;

    if (res_idx >= num_res) {
        SENSOR_EMSG("[%s] Please check the number of resolutions supported by the sensor!\n", __FUNCTION__);
        return FAIL;
    }

    handle->video_res_supported.ulcur_res = res_idx;

    handle->data_prec = CUS_DATAPRECISION_12;

    switch (res_idx) {
    case 0: // 3760x2116@30fps — non-binned, 97% FOV, best quality, 891/REALTIME
        handle->video_res_supported.ulcur_res = 0;
        handle->pCus_sensor_init = pCus_init_nobinned_dynamic;
        vts_30fps = 2250; // VTS at 30fps with HMAX=1100
        params->expo.vts = vts_30fps;
        params->expo.fps = 30;
        Preview_line_period = 14815; // HMAX=1100 at INCKSEL3=0xC6
        break;

    case 1: // 2952x1656@50fps — NON-BINNED 1485Mbps, 1:1 full 5MP readout (no SCL scale).
        // 50 fps is the honest rate: the ISP m2m ceiling caps 4.89 MPix at ~51 fps,
        // so the sensor is paced to 50.0 (VMAX=2289) for even frame cadence instead
        // of dropping ~10 of 60 sensor frames at the VIF->ISP handoff.
        handle->video_res_supported.ulcur_res = 1;
        handle->pCus_sensor_init = pCus_init_5m_50fps_1485_mipi4lane_linear;
        vts_30fps = 2289; // VMAX @50fps, 8.736us line (HMAX=652 @1485)
        params->expo.vts = vts_30fps;
        params->expo.fps = 50;
        Preview_line_period = 8736;
        break;

    case 2: // 2688x1512@60 — ISP-ceiling-sized max non-binned 16:9 @60 (4.06 MPix)
        handle->video_res_supported.ulcur_res = 2;
        handle->pCus_sensor_init = pCus_init_2688x1512_60_mipi4lane_linear;
        vts_30fps = 1908; // VMAX @60fps, 8.736us line
        params->expo.vts = vts_30fps;
        params->expo.fps = 60;
        Preview_line_period = 8736;
        break;

    case 3: // 2112x1184@90 — ISP-ceiling-sized max non-binned @90 (2.50 MPix)
        handle->video_res_supported.ulcur_res = 3;
        handle->pCus_sensor_init = pCus_init_2112x1184_90_mipi4lane_linear;
        vts_30fps = 1272; // VMAX @90fps, 8.736us line
        params->expo.vts = vts_30fps;
        params->expo.fps = 90;
        Preview_line_period = 8736;
        break;

    case 4: // 1920x1080@100 — low-latency non-binned 1080p, ~8% under the ISP ceiling
        handle->video_res_supported.ulcur_res = 4;
        handle->pCus_sensor_init = pCus_init_1920x1080_100_mipi4lane_linear;
        vts_30fps = 1144; // VMAX @100fps, 8.736us line
        params->expo.vts = vts_30fps;
        params->expo.fps = 100;
        Preview_line_period = 8736;
        break;

    default:
        break;
    }

    return SUCCESS;
}

static int pCus_GetOrien(ms_cus_sensor* handle, CUS_CAMSENSOR_ORIT* orit)
{
    s16 sen_data;

    // Read SENSOR MIRROR-FLIP STATUS
    SensorReg_Read(MIRROR_FLIP, (void*)&sen_data);

    switch (sen_data & SENSOR_MIRROR_FLIP_EN) {
    case SENSOR_NOR:
        *orit = CUS_ORIT_M0F0;
        break;
    case SENSOR_FLIP_EN:
        *orit = CUS_ORIT_M0F1;
        break;
    case SENSOR_MIRROR_EN:
        *orit = CUS_ORIT_M1F0;
        break;
    case SENSOR_MIRROR_FLIP_EN:
        *orit = CUS_ORIT_M1F1;
        break;
    }
    return SUCCESS;
}

static int pCus_SetOrien(ms_cus_sensor* handle, CUS_CAMSENSOR_ORIT orit)
{
    imx415_params* params = (imx415_params*)handle->private_data;

    handle->orient = orit;
    params->orien_dirty = true;

    return SUCCESS;
}

static int DoOrien(ms_cus_sensor* handle, CUS_CAMSENSOR_ORIT orit)
{
    s16 sen_data;
    // Read SENSOR MIRROR-FLIP STATUS
    SensorReg_Read(MIRROR_FLIP, (void*)&sen_data);
    sen_data &= ~(SENSOR_MIRROR_FLIP_EN);

    switch (orit) {
    case CUS_ORIT_M0F0:
        // sen_data |= SENSOR_NOR;
        handle->orient = CUS_ORIT_M0F0;
        break;
    case CUS_ORIT_M1F0:
        sen_data |= SENSOR_MIRROR_EN;
        handle->orient = CUS_ORIT_M1F0;
        break;
    case CUS_ORIT_M0F1:
        sen_data |= SENSOR_FLIP_EN;
        handle->orient = CUS_ORIT_M0F1;
        break;
    case CUS_ORIT_M1F1:
        sen_data |= SENSOR_MIRROR_FLIP_EN;
        handle->orient = CUS_ORIT_M1F1;
        break;
    default:
        handle->orient = CUS_ORIT_M0F0;
        break;
    }
    // Write SENSOR MIRROR-FLIP STATUS
    SensorReg_Write(MIRROR_FLIP, sen_data);

    return SUCCESS;
}

static int pCus_GetFPS(ms_cus_sensor* handle)
{
    imx415_params* params = (imx415_params*)handle->private_data;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 tVts = (params->tVts_reg[0].data << 16) | (params->tVts_reg[1].data << 8) | (params->tVts_reg[2].data << 0);

    if (params->expo.fps >= 1000)
        params->expo.preview_fps = (u32)(((u64)vts_30fps * max_fps * 1000) / tVts);
    else
        params->expo.preview_fps = (u32)(((u64)vts_30fps * max_fps) / tVts);

    return params->expo.preview_fps;
}

static int pCus_SetFPS(ms_cus_sensor* handle, u32 fps)
{
    // u32 vts = 0, cur_vts_30fps = 0;
    imx415_params* params = (imx415_params*)handle->private_data;
    u32 max_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].max_fps;
    u32 min_fps = handle->video_res_supported.res[handle->video_res_supported.ulcur_res].min_fps;
    // pr_info("[%s]  leslie_fps,maxfps,minfps : %d,%d,%d\n\n", __FUNCTION__,fps,max_fps,min_fps);
    // cur_vts_30fps = vts_30fps;
    // pr_info("[%s]  leslie_vts_30fps : %u\n\n", __FUNCTION__,vts_30fps);
    if (fps >= min_fps && fps <= max_fps) {
        if (CUS_CMU_CLK_36MHZ == handle->mclk) {
            fps = fps > 29 ? 29 : fps; // limit fps at 29 fps due to MCLK=36MHz
            params->expo.vts = (u32)(((u64)vts_30fps * 29091 + fps * 500) / (fps * 1000));
        } else
            params->expo.vts = (u32)(((u64)vts_30fps * max_fps * 1000 + fps * 500) / (fps * 1000));
    } else if (fps >= (min_fps * 1000) && fps <= (max_fps * 1000)) {
        if (CUS_CMU_CLK_36MHZ == handle->mclk) {
            fps = fps > 29091 ? 29091 : fps; // limit fps at 29.091 fps due to MCLK=36MHz
            params->expo.vts = (u32)(((u64)vts_30fps * 29091 + (fps >> 1)) / fps);
        } else
            params->expo.vts = (u32)(((u64)vts_30fps * max_fps * 1000 + (fps >> 1)) / fps);
    } else {
        SENSOR_DMSG("[%s] FPS %d out of range.\n", __FUNCTION__, fps);
        return FAIL;
    }
    // pr_info("[%s]  leslie_vts : %u\n\n", __FUNCTION__,params->expo.vts);
    if (params->expo.expo_lines > params->expo.vts - 4) {
        // vts = params->expo.expo_lines + 4;
#if 0 // Update FPS Status
        if(fps>=3 && fps <= 30)
            fps = (vts_30fps*30000)/(params->expo.vts * 1000 - 500);
        else if(fps>=3000 && fps <= 30000)
            fps = (vts_30fps*30000)/(params->expo.vts - (500 / 1000));
#endif
    } else {
        // vts = params->expo.vts;
    }

    params->expo.fps = fps;
    params->dirty = true;

    pCus_SetAEUSecs(handle, params->expo.expo_lef_us);

    return SUCCESS;
}

///////////////////////////////////////////////////////////////////////
// auto exposure
///////////////////////////////////////////////////////////////////////
// unit: micro seconds
// AE status notification
static int pCus_AEStatusNotify(ms_cus_sensor* handle, CUS_CAMSENSOR_AE_STATUS_NOTIFY status)
{
    imx415_params* params = (imx415_params*)handle->private_data;
    // ISensorIfAPI2 *sensor_if1 = handle->sensor_if_api2;

    switch (status) {
    case CUS_FRAME_INACTIVE:
        break;
    case CUS_FRAME_ACTIVE:
        if (params->dirty || params->orien_dirty) {
            SensorReg_Write(0x3001, 1); // Global hold on
            if (params->dirty) {
                SensorRegArrayW((I2C_ARRAY*)params->tExpo_reg, ARRAY_SIZE(expo_reg));
                SensorRegArrayW((I2C_ARRAY*)params->tGain_reg, ARRAY_SIZE(gain_reg));
                SensorRegArrayW((I2C_ARRAY*)params->tVts_reg, ARRAY_SIZE(vts_reg));
                params->dirty = false;
            }

            if (params->orien_dirty) {
                DoOrien(handle, handle->orient);
                params->orien_dirty = false;
            }
            SensorReg_Write(0x3001, 0); // Global hold off
        }
        break;
    default:
        break;
    }
    return SUCCESS;
}

static int pCus_GetAEUSecs(ms_cus_sensor* handle, u32* us)
{
    u32 lines = 0;
    imx415_params* params = (imx415_params*)handle->private_data;

    lines |= (u32)(params->tExpo_reg[0].data & 0x03) << 16;
    lines |= (u32)(params->tExpo_reg[1].data & 0xff) << 8;
    lines |= (u32)(params->tExpo_reg[2].data & 0xff) << 0;

    *us = (lines * Preview_line_period) / 1000;

    SENSOR_DMSG("[%s] sensor expo lines/us %u,%u us\n", __FUNCTION__, lines, *us);

    return SUCCESS;
}

static int pCus_SetAEUSecs(ms_cus_sensor* handle, u32 us)
{
    u32 expo_lines = 0, vts = 0, SHR0 = 0;
    imx415_params* params = (imx415_params*)handle->private_data;

    params->expo.expo_lef_us = us;
    expo_lines = (1000 * us) / Preview_line_period;

    if (expo_lines > params->expo.vts) {
        vts = expo_lines + 8;
    } else
        vts = params->expo.vts;
    SHR0 = vts - expo_lines;

    if (SHR0 <= 12) // 8+4
        SHR0 = 8;
    else
        SHR0 -= 4;

    params->expo.expo_lines = expo_lines;
    // params->expo.vts = vts;

    SENSOR_DMSG("[%s] us %u, SHR0 %u, vts %u\n", __FUNCTION__,
        us,
        SHR0,
        vts);
    // pr_info("[%s]  leslie_shutter,expo_lines,params_expo_lines : %d,%d,%d\n\n", __FUNCTION__,us,expo_lines,params->expo.expo_lines);
    // pr_info("[%s]  leslie_shutter_vts : %u,%u\n\n", __FUNCTION__,params->expo.vts,vts);
    params->tExpo_reg[0].data = (SHR0 >> 16) & 0x0003;
    params->tExpo_reg[1].data = (SHR0 >> 8) & 0x00ff;
    params->tExpo_reg[2].data = (SHR0 >> 0) & 0x00ff;

    params->tVts_reg[0].data = (vts >> 16) & 0x0003;
    params->tVts_reg[1].data = (vts >> 8) & 0x00ff;
    params->tVts_reg[2].data = (vts >> 0) & 0x00ff;

    params->dirty = true;
    return SUCCESS;
}

// Gain: 1x = 1024
static int pCus_GetAEGain(ms_cus_sensor* handle, u32* gain)
{
    imx415_params* params = (imx415_params*)handle->private_data;
#if 0
    u16 temp_gain;

    temp_gain=gain_reg[0].data;
    *gain=(u32)(10^((temp_gain*3)/200))*1024;
    if (gain_reg[1].data & 0x10)
       *gain = (*gain) * 2;
#endif
    *gain = params->expo.final_gain;
    SENSOR_DMSG("[%s] get gain %u\n", __FUNCTION__, *gain);

    return SUCCESS;
}

static int pCus_SetAEGain(ms_cus_sensor* handle, u32 gain)
{
    imx415_params* params = (imx415_params*)handle->private_data;
    u32 i;
    CUS_GAIN_GAP_ARRAY* Sensor_Gain_Linearity;
    u64 gain_double;

    params->expo.final_gain = gain;
    if (gain < SENSOR_MIN_GAIN)
        gain = SENSOR_MIN_GAIN;
    else if (gain >= SENSOR_MAX_GAIN)
        gain = SENSOR_MAX_GAIN;

    Sensor_Gain_Linearity = gain_gap_compensate;

    for (i = 0; i < sizeof(gain_gap_compensate) / sizeof(CUS_GAIN_GAP_ARRAY); i++) {
        if (Sensor_Gain_Linearity[i].gain == 0)
            break;
        if ((gain > Sensor_Gain_Linearity[i].gain) && (gain < (Sensor_Gain_Linearity[i].gain + Sensor_Gain_Linearity[i].offset))) {
            gain = Sensor_Gain_Linearity[i].gain;
            break;
        }
    }
    gain_double = 20 * (intlog10(gain) - intlog10(1024));
    gain_double = ((gain_double * 10) >> 24) / 3;

    params->tGain_reg[0].data = gain_double & 0xff;
    params->tGain_reg[1].data = (gain_double >> 8) & 0xff;

#if DEBUG_INFO
    SENSOR_DMSG("[%s]gain %u gain_double %llu\n", __FUNCTION__, gain, gain_double);
#endif

    SENSOR_DMSG("[%s] set gain/reg=%u/0x%x 0x%x\n", __FUNCTION__, gain, params->tGain_reg[0].data, params->tGain_reg[1].data);
    params->dirty = true;
    return SUCCESS;
}

static int pCus_GetAEMinMaxUSecs(ms_cus_sensor* handle, u32* min, u32* max)
{
    u32 cur = handle->video_res_supported.ulcur_res;
    *min = 1;
    *max = 1000000 / imx415_mipi_linear[cur].senout.min_fps;
    return SUCCESS;
}

static int pCus_GetAEMinMaxGain(ms_cus_sensor* handle, u32* min, u32* max)
{
    *min = SENSOR_MIN_GAIN; // handle->sat_mingain;
    *max = SENSOR_MAX_GAIN; // 10^(72db/20)*1024;
    return SUCCESS;
}

static int IMX415_GetShutterInfo(struct __ms_cus_sensor* handle, CUS_SHUTTER_INFO* info)
{
    u32 cur = handle->video_res_supported.ulcur_res;
    info->max = 1000000000 / imx415_mipi_linear[cur].senout.min_fps;
    info->min = (Preview_line_period * 1);
    info->step = Preview_line_period;
    return SUCCESS;
}

int cus_camsensor_init_handle_linear(ms_cus_sensor* drv_handle)
{
    ms_cus_sensor* handle = drv_handle;
    imx415_params* params;
    int res;

    if (!handle) {
        SENSOR_DMSG("[%s] not enough memory!\n", __FUNCTION__);
        return FAIL;
    }
    SENSOR_DMSG("[%s]", __FUNCTION__);
    ////////////////////////////////////
    // private data allocation & init //
    ////////////////////////////////////
    if (handle->private_data == NULL) {
        SENSOR_EMSG("[%s] Private data is empty!\n", __FUNCTION__);
        return FAIL;
    }

    params = (imx415_params*)handle->private_data;
    memcpy(params->tVts_reg, vts_reg, sizeof(vts_reg));
    memcpy(params->tGain_reg, gain_reg, sizeof(gain_reg));
    memcpy(params->tExpo_reg, expo_reg, sizeof(expo_reg));

    ////////////////////////////////////
    //    sensor model ID             //
    ////////////////////////////////////
    snprintf(handle->model_id, sizeof(handle->model_id), "IMX415_MIPI");

    ////////////////////////////////////
    //    i2c config                  //
    ////////////////////////////////////
    handle->i2c_cfg.mode = SENSOR_I2C_LEGACY; //(CUS_ISP_I2C_MODE) FALSE;
    handle->i2c_cfg.fmt = SENSOR_I2C_FMT; // CUS_I2C_FMT_A16D8;
    handle->i2c_cfg.address = SENSOR_I2C_ADDR; // 0x34;
    handle->i2c_cfg.speed = SENSOR_I2C_SPEED; // 300000;

    ////////////////////////////////////
    //    mclk                        //
    ////////////////////////////////////
    // handle->mclk                  = UseParaMclk(SENSOR_DRV_PARAM_MCLK());
    handle->mclk = Preview_MCLK_SPEED;

    ////////////////////////////////////
    //    sensor interface info       //
    ////////////////////////////////////
    handle->isp_type = SENSOR_ISP_TYPE;
    // handle->data_fmt              = SENSOR_DATAFMT;
    handle->sif_bus = SENSOR_IFBUS_TYPE;
    handle->data_prec = SENSOR_DATAPREC;
    handle->data_mode = SENSOR_DATAMODE;
    handle->bayer_id = SENSOR_BAYERID;
    handle->RGBIR_id = SENSOR_RGBIRID;
    handle->orient = SENSOR_ORIT;
    // handle->YC_ODER               = SENSOR_YCORDER;   //CUS_SEN_YCODR_CY;
    handle->interface_attr.attr_mipi.mipi_lane_num = SENSOR_MIPI_LANE_NUM;
    handle->interface_attr.attr_mipi.mipi_data_format = CUS_SEN_INPUT_FORMAT_RGB; // RGB pattern.
    handle->interface_attr.attr_mipi.mipi_yuv_order = 0; // don't care in RGB pattern.
    handle->interface_attr.attr_mipi.mipi_hsync_mode = SENSOR_MIPI_HSYNC_MODE;
    handle->interface_attr.attr_mipi.mipi_hdr_mode = CUS_HDR_MODE_NONE;
    handle->interface_attr.attr_mipi.mipi_hdr_virtual_channel_num = 0; // Short frame

    ////////////////////////////////////
    //    resolution capability       //
    ////////////////////////////////////
    handle->video_res_supported.ulcur_res = 0; // default resolution index is 0.
    // handle->video_res_supported.num_res = LINEAR_RES_END;
    for (res = 0; res < LINEAR_RES_END; res++) {
        handle->video_res_supported.num_res = res + 1;
        handle->video_res_supported.res[res].width = imx415_mipi_linear[res].senif.preview_w;
        handle->video_res_supported.res[res].height = imx415_mipi_linear[res].senif.preview_h;
        handle->video_res_supported.res[res].max_fps = imx415_mipi_linear[res].senout.max_fps;
        handle->video_res_supported.res[res].min_fps = imx415_mipi_linear[res].senout.min_fps;
        handle->video_res_supported.res[res].crop_start_x = imx415_mipi_linear[res].senif.crop_start_X;
        handle->video_res_supported.res[res].crop_start_y = imx415_mipi_linear[res].senif.crop_start_y;
        handle->video_res_supported.res[res].nOutputWidth = imx415_mipi_linear[res].senout.width;
        handle->video_res_supported.res[res].nOutputHeight = imx415_mipi_linear[res].senout.height;
        snprintf(handle->video_res_supported.res[res].strResDesc, sizeof(handle->video_res_supported.res[res].strResDesc), "%s", imx415_mipi_linear[res].senstr.strResDesc);
    }

    ////////////////////////////////////
    //    Sensor polarity             //
    ////////////////////////////////////
    handle->pwdn_POLARITY = SENSOR_PWDN_POL; // CUS_CLK_POL_NEG;
    handle->reset_POLARITY = SENSOR_RST_POL; // CUS_CLK_POL_NEG;
    // handle->VSYNC_POLARITY             = SENSOR_VSYNC_POL; //CUS_CLK_POL_POS;
    // handle->HSYNC_POLARITY             = SENSOR_HSYNC_POL; //CUS_CLK_POL_POS;
    // handle->PCLK_POLARITY              = SENSOR_PCLK_POL;  //CUS_CLK_POL_POS);    // use '!' to clear board latch error

    ////////////////////////////////////////
    // Sensor Status Control and Get Info //
    ////////////////////////////////////////
    handle->pCus_sensor_release = cus_camsensor_release_handle;
    handle->pCus_sensor_init = pCus_init_nobinned_dynamic;
    // handle->pCus_sensor_powerupseq     = pCus_powerupseq;
    handle->pCus_sensor_poweron = pCus_poweron;
    handle->pCus_sensor_poweroff = pCus_poweroff;
    handle->pCus_sensor_GetSensorID = pCus_GetSensorID;
    handle->pCus_sensor_GetVideoResNum = pCus_GetVideoResNum;
    handle->pCus_sensor_GetVideoRes = pCus_GetVideoRes;
    handle->pCus_sensor_GetCurVideoRes = pCus_GetCurVideoRes;
    handle->pCus_sensor_SetVideoRes = pCus_SetVideoRes;

    handle->pCus_sensor_GetOrien = pCus_GetOrien;
    handle->pCus_sensor_SetOrien = pCus_SetOrien;
    handle->pCus_sensor_GetFPS = pCus_GetFPS;
    handle->pCus_sensor_SetFPS = pCus_SetFPS;
    // handle->pCus_sensor_GetSensorCap    = pCus_GetSensorCap;
    handle->pCus_sensor_SetPatternMode = imx415_SetPatternMode; // NONE

    ////////////////////////////////////
    //    AE parameters               //
    ////////////////////////////////////
    handle->ae_gain_delay = SENSOR_GAIN_DELAY_FRAME_COUNT;
    handle->ae_shutter_delay = SENSOR_SHUTTER_DELAY_FRAME_COUNT;
    handle->ae_gain_ctrl_num = 1;
    handle->ae_shutter_ctrl_num = 1;
    handle->sat_mingain = SENSOR_MIN_GAIN; // g_sensor_ae_min_gain;
    // handle->dgain_remainder = 0;

    ////////////////////////////////////
    //  AE Control and Get Info       //
    ////////////////////////////////////
    // unit: micro seconds
    // handle->pCus_sensor_GetAETrigger_mode      = pCus_GetAETrigger_mode;
    // handle->pCus_sensor_SetAETrigger_mode      = pCus_SetAETrigger_mode;
    handle->pCus_sensor_AEStatusNotify = pCus_AEStatusNotify;
    handle->pCus_sensor_GetAEUSecs = pCus_GetAEUSecs;
    handle->pCus_sensor_SetAEUSecs = pCus_SetAEUSecs;
    handle->pCus_sensor_GetAEGain = pCus_GetAEGain;
    handle->pCus_sensor_SetAEGain = pCus_SetAEGain;

    handle->pCus_sensor_GetAEMinMaxGain = pCus_GetAEMinMaxGain;
    handle->pCus_sensor_GetAEMinMaxUSecs = pCus_GetAEMinMaxUSecs;
    // handle->pCus_sensor_GetDGainRemainder = pCus_GetDGainRemainder;

    // sensor calibration
    // handle->pCus_sensor_SetAEGain_cal   = pCus_SetAEGain_cal;
    // handle->pCus_sensor_setCaliData_gain_linearity=pCus_setCaliData_gain_linearity;
    handle->pCus_sensor_GetShutterInfo = IMX415_GetShutterInfo;

    params->expo.vts = vts_30fps;

    return SUCCESS;
}

// lef functions
#if 0
//static int pCus_GetDGainRemainder(ms_cus_sensor *handle, u32 *dgain_remainder)
//{
//    *dgain_remainder = handle->dgain_remainder;
//    return SUCCESS;
//}

#endif

SENSOR_DRV_ENTRY_IMPL_END_EX(IMX415_HDR,
    cus_camsensor_init_handle_linear,
    NULL,
    NULL,
    imx415_params);
