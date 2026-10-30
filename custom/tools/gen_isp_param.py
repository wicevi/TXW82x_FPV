#!/usr/bin/env python3
"""gen_isp_param.py - Path A generator: transplant the imaging team's
isp.bin (v2.7.0.7 export, single-gamma layout) into the compiled-in
isp_param_gc20c3.c as byte-exact const unions/arrays.

File layout (board-verified 2026-10-30):
  [6B hdr] [info_src 4] [head 28] [basic 60] [param_info 2864]
  [rgb gamma 256] [lsc 2448] = 5666B
param_info offsets (cross-compiler measured):
  enable 0 / awb 4 / ae 456 / ccm 696 / blc 720 / md 728 / csc 740
  gamma_bv 752 / dpc 804 / ce 816 / sharp 940 / nr 996 (flags4 + rawnr
  176 @+4 + yuvnr 72 @+180 + csupp 60 @+252) / gic 1308 / lhs 1316
  wdr 1424 / ygamma 1556 / rgb_gamma 2856 / lsc_tbl 2860
"""
import struct
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\admin\Downloads\isp.bin'
DST = 'sdk/lib/video/isp/param/isp_param_gc20c3.c'

d = open(SRC, 'rb').read()
P, G, L = 98, 2962, 3218
OFF = dict(enable=0, awb=4, ae=456, ccm=696, blc=720, md=728, csc=740,
           gamma=752, dpc=804, ce=816, sharp=940, nr=996, gic=1308,
           lhs=1316, wdr=1424, ygamma=1556)


def sl(name, n):
    return d[P + OFF[name]: P + OFF[name] + n]


def hexlines(b, per=16):
    return [', '.join('0x%02x' % x for x in b[i:i + per])
            for i in range(0, len(b), per)]


def arr_u32(b):
    ws = struct.unpack('<%dI' % (len(b) // 4), b)
    out = []
    for i in range(0, len(ws), 6):
        out.append(', '.join('0x%08x' % w for w in ws[i:i + 6]) + ',')
    return '\n    '.join(out)


def union(cname, ctype, blob, size):
    s = ('static const union {\n    %s s;\n    uint8_t b[%d];\n} '
         '%s_u = {\n    .b = {\n' % (ctype, size, cname))
    for ln in hexlines(blob):
        s += '        %s,\n' % ln
    s += '    },\n};\n#define %s %s_u.s\n' % (cname, cname)
    return s


def raw_array(cname, etype, blob, cnt):
    s = 'static const %s %s[%d] = {\n' % (etype, cname, cnt)
    for ln in hexlines(blob):
        s += '    %s,\n' % ln
    return s + '};\n'


def u32_array(cname, blob, cnt):
    s = 'static const unsigned int %s[%d] = {\n    ' % (cname, cnt)
    return s + arr_u32(blob) + '\n};\n'


out = []
out.append('''/*
 * isp_param_gc20c3.c - GC20C3 ISP calibration table.
 * PORTED FROM THE IMAGING TEAM'S isp.bin EXPORT (v2.7.0.7 tool,
 * 2026-10-30) via Path A: values transplanted byte-exact into the
 * compiled-in table - the flash-param delivery path is unusable on
 * v2.7.1.7 (loader/driver checks reject any third-party file, board
 * findings 2026-10-29/30). Every sub-struct below is a const union
 * carrying the exported bytes verbatim; layout guarded by
 * _Static_assert against the measured v2.7.1.7 struct sizes.
 * Regenerate: python custom/tools/gen_isp_param.py <isp.bin>
 */
// isp_param.c
#include "sys_config.h"
#include "typesdef.h"
#include "osal/string.h"
#include "hal/isp_param.h"
#include "hal/isp.h"
#include "lib/video/dvp/cmos_sensor/csi.h"

#define GC20C3_MASTER_SENSOR 1
#define GC20C3_SLAVE0_SENSOR 0
#define GC20C3_SLAVE1_SENSOR 0

#if GC20C3_MASTER_SENSOR
''')

out.append('/* ---------- Enable ---------- */\n')
out.append(union('gc20c3_master_enable', '_ISP_FUNC', sl('enable', 4), 4))
out.append('_Static_assert(sizeof(_ISP_FUNC) == 4, "layout drift");\n\n')
out.append('/* ---------- AWB ---------- */\n')
out.append(union('gc20c3_master_awb', 'TYPE_HGISP_CFG_AWB', sl('awb', 452), 452))
out.append('_Static_assert(sizeof(TYPE_HGISP_CFG_AWB) == 452, "layout drift");\n\n')
out.append('/* ---------- AE ---------- */\n')
out.append(union('gc20c3_master_ae', 'struct hgisp_cfg_ae', sl('ae', 240), 240))
out.append('_Static_assert(sizeof(struct hgisp_cfg_ae) == 240, "layout drift");\n\n')
out.append('/* ---------- BLC ---------- */\n')
out.append(union('gc20c3_master_blc', '_Sensor_BLC', sl('blc', 8), 8))
out.append('_Static_assert(sizeof(_Sensor_BLC) == 8, "layout drift");\n\n')
out.append('/* ---------- CCM ---------- */\n')
out.append(union('gc20c3_master_ccm', '_Sensor_CCM', sl('ccm', 24), 24))
out.append('_Static_assert(sizeof(_Sensor_CCM) == 24, "layout drift");\n\n')
out.append('/* ---------- CSC ---------- */\n')
out.append(union('gc20c3_master_csc', '_Sensor_CSC', sl('csc', 12), 12))
out.append('_Static_assert(sizeof(_Sensor_CSC) == 12, "layout drift");\n\n')
out.append('/* ---------- Gamma BV ---------- */\n')
out.append(union('gc20c3_master_gamma', '_Sensor_GAMMA_BV', sl('gamma', 52), 52))
out.append('_Static_assert(sizeof(_Sensor_GAMMA_BV) == 52, "layout drift");\n\n')
out.append('/* ---------- DPC ---------- */\n')
out.append(union('gc20c3_master_dpc', '_Sensor_DPC', sl('dpc', 12), 12))
out.append('_Static_assert(sizeof(_Sensor_DPC) == 12, "layout drift");\n\n')
out.append('/* ---------- CE (color enhance) ---------- */\n')
out.append(union('gc20c3_master_ce', '_Sensor_BV2COLENH', sl('ce', 124), 124))
out.append('_Static_assert(sizeof(_Sensor_BV2COLENH) == 124, "layout drift");\n\n')
out.append('/* ---------- Sharpness ---------- */\n')
out.append(union('gc20c3_master_sharp', '_Sensor_SHARP', sl('sharp', 56), 56))
out.append('_Static_assert(sizeof(_Sensor_SHARP) == 56, "layout drift");\n\n')

nr = sl('nr', 312)
out.append('/* ---------- NR maps ---------- */\n')
out.append(raw_array('gc20c3_master_bv2nr', '_Sensor_BV2NR', nr[4:180], 11))
out.append('_Static_assert(sizeof(_Sensor_BV2NR) == 16, "layout drift");\n\n')
out.append(raw_array('gc20c3_master_yuvnr', '_Sensor_YUVNR', nr[180:252], 6))
out.append('_Static_assert(sizeof(_Sensor_YUVNR) == 12, "layout drift");\n\n')
out.append(raw_array('gc20c3_master_csupp', '_Sensor_CSUPP', nr[252:312], 3))
out.append('_Static_assert(sizeof(_Sensor_CSUPP) == 20, "layout drift");\n\n')

out.append('/* ---------- GIC ---------- */\n')
out.append(union('gc20c3_master_gic', '_Sensor_GIC', sl('gic', 8), 8))
out.append('_Static_assert(sizeof(_Sensor_GIC) == 8, "layout drift");\n\n')
out.append('/* ---------- LHS ---------- */\n')
out.append(union('gc20c3_master_lhs', '_Sensor_LHS_MAP', sl('lhs', 108), 108))
out.append('_Static_assert(sizeof(_Sensor_LHS_MAP) == 108, "layout drift");\n\n')
out.append('/* ---------- WDR ---------- */\n')
out.append(union('gc20c3_master_wdr', 'TYPE_HGISP_CFG_WDR', sl('wdr', 124), 124))
out.append('_Static_assert(sizeof(TYPE_HGISP_CFG_WDR) == 124, "layout drift");\n\n')
out.append('/* ---------- Y Gamma ---------- */\n')
out.append(union('gc20c3_master_ygamma', '_Sensor_YGAMMA_MAP', sl('ygamma', 1300), 1300))
out.append('_Static_assert(sizeof(_Sensor_YGAMMA_MAP) == 1300, "layout drift");\n\n')

out.append('/* ---------- RGB gamma (256B single table, v2.7.0.7 export) ---------- */\n')
out.append(u32_array('gc20c3_2_2_rgbgamma', d[G:G + 256], 64))
out.append('\nstatic const _Sensor_RGBGAMMA gc20c3_rgb_gamma_tbl = {\n'
           '    .p_rgb_gamma_tbl = (unsigned int *)gc20c3_2_2_rgbgamma,\n};\n\n')

out.append('/* ---------- LSC (2448B = 612 u32) ---------- */\n')
out.append(u32_array('gc20c3_master_lsc_tbl', d[L:L + 2448], 612))
out.append('\nstatic const _Sensor_LSC gc20c3_master_lsc = {\n'
           '    .p_lsc_tbl = (unsigned int *)gc20c3_master_lsc_tbl,\n};\n\n')

out.append('#endif\n\n')
out.append('''/* export for gc20C3_mipi.c (unchanged wiring) */
const _Sensor_ISP_Init gc20c3_isp_param_init[ISP_SUPPORT_SENSOR_MAX_NUM] =
{
#if GC20C3_MASTER_SENSOR
    {
        .p_func        = (_ISP_FUNC          *)&gc20c3_master_enable,
        .p_blc         = (_Sensor_BLC        *)&gc20c3_master_blc,
        .p_ccm         = (_Sensor_CCM        *)&gc20c3_master_ccm,
        .p_dpc         = (_Sensor_DPC        *)&gc20c3_master_dpc,
        .p_colenh      = (_Sensor_BV2COLENH  *)&gc20c3_master_ce,
        .p_csc         = (_Sensor_CSC        *)&gc20c3_master_csc,
        .p_gamma_param = (_Sensor_GAMMA_BV   *)&gc20c3_master_gamma,
        .p_gic         = (_Sensor_GIC        *)&gc20c3_master_gic,
        .p_csupp       = (_Sensor_CSUPP      *)gc20c3_master_csupp,
        .p_sharp       = (_Sensor_SHARP      *)&gc20c3_master_sharp,
        .p_yuvnr       = (_Sensor_YUVNR      *)gc20c3_master_yuvnr,
        .p_bv2nr       = (_Sensor_BV2NR      *)gc20c3_master_bv2nr,
        .p_lsc         = (_Sensor_LSC        *)&gc20c3_master_lsc,
        .p_lhs         = (_Sensor_LHS_MAP    *)&gc20c3_master_lhs,
        .p_ygamma      = (_Sensor_YGAMMA_MAP *)&gc20c3_master_ygamma,
        .p_rgb_gamma   = (_Sensor_RGBGAMMA   *)&gc20c3_rgb_gamma_tbl,
        .p_awb         = (TYPE_HGISP_CFG_AWB *)&gc20c3_master_awb,
        .p_ae          = (TYPE_HGISP_CFG_AE  *)&gc20c3_master_ae,
        .p_wdr         = (TYPE_HGISP_CFG_WDR *)&gc20c3_master_wdr,
    },
#endif
    { 0 },
    { 0 },
    { 0 },
};
''')

open(DST, 'w', encoding='utf-8', newline='\n').write(''.join(out))
print('generated %s (%d bytes) from %s (%d B)' %
      (DST, sum(len(x) for x in out), SRC, len(d)))
