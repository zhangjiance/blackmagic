#include "general.h"
#include "target.h"
#include "target_internal.h"
#include "adi.h"
#include "cortexm.h"
#include "timing.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#ifdef CONFIG_RISCV
#include "riscv_debug.h"
#endif
#include "jep106.h"


#define HPM_XPI_FLASH_BASE          (0x80000000)
#define HPM_XPI_FLASH_SIZE          (0x2000000)
#define HPM_XPI_PAGE_SIZE           (0x1000)

#define HPM_XPI_BASE_DEFAULT        (0xF3000000UL)
#define HPM_XPI_HDR_DEFAULT         (0xfcf90001U)
#define HPM_XPI_OPT0_DEFAULT        (0x00000007U)
#define HPM_XPI_OPT1_DEFAULT        (0)

#define HPM_ALGO_LOAD_BASE_ADDR     (0x00000000)
#define HPM_ALGO_STACK_BASE_ADDR    (HPM_ALGO_LOAD_BASE_ADDR+10240)
#define HPM_ALGO_BUFF_BASE_ADDR     (HPM_ALGO_STACK_BASE_ADDR+256)

#define HPM_FLASH_INIT              (0)
#define HPM_FLASH_ERASE             (0x6)
#define HPM_FLASH_PROGRAM           (0xc)
#define HPM_FLASH_READ              (0x12)
#define HPM_FLASH_GET_INFO          (0x18)
#define HPM_FLASH_ERASE_CHIP        (0x1e)

#define HPM_ALGO_ERASE_TIMEOUT      (100000)
#define HPM_ALGO_WRITE_TIMEOUT      (10000)
#define HPM_ALGO_CMD_TIMEOUT        (500)

#define ROM_API_TABLE_ROOT          (0x2001FF00U)
#define HPM6700_A0_SILICON          (0x2001f398UL)
#define HPM6700_A1_SILICON          (0x2001fa40UL)
#define HPM6300_A0_SILICON          (0x2001e6fcUL)
#define HPM6300_A1_SILICON          (0x2001d8e4UL)
#define HPM6200_A0_SILICON          (0x2001c448UL)
#define HPM6200_A1_SILICON          (0x20014e0cUL)
#define HPM6800_A0_SILICON          (0x2001db20UL)
#define HPM5300_A0_SILICON          (0x01000200UL)
#define HPM6200_A2_SILICON          (0x10000500UL)
#define HPM6E00_A0_SILICON          (0x10001200UL)
#define HPM6P00_A0_SILICON          (0x10000600UL)

typedef struct {
    uint32_t magic;
    const char *series;
} soc_map_t;

typedef struct {
    uint32_t total_sz_in_bytes;
    uint32_t sector_sz_in_bytes;
} hpm_flash_info_t;

typedef struct{
    uint32_t header;
    uint32_t xpi_base;
    uint32_t flash_base;
	uint32_t flash_size;
	uint32_t sector_size;
    uint32_t opt0;
    uint32_t opt1;
}hpm_xpi_cfg_t;

static uint32_t hpm_read_u32_via_sysbus(target_s *target,uint32_t *buffer, uint32_t addr)
{
    target->mem_read(target, buffer, addr, sizeof(uint32_t));
    return 0;
}

static const uint32_t hpm_soc_set[] = { HPM6700_A0_SILICON, HPM6700_A1_SILICON, HPM6300_A0_SILICON, HPM6300_A1_SILICON,
    HPM6200_A0_SILICON, HPM6200_A1_SILICON, HPM6800_A0_SILICON };

static soc_map_t hpm_soc_maps[] = {
    { HPM6700_A0_SILICON, "hpm6700" }, { HPM6700_A1_SILICON, "hpm6700" }, { HPM6300_A0_SILICON, "hpm6300" }, { HPM6300_A1_SILICON, "hpm6300" },
    { HPM6200_A0_SILICON, "hpm6200" }, { HPM6200_A1_SILICON, "hpm6200" }, { HPM6200_A2_SILICON, "hpm6200" }, { HPM5300_A0_SILICON, "hpm5300" },
    { HPM6800_A0_SILICON, "hpm6800" }, { HPM6E00_A0_SILICON, "hpm6e00" }, { HPM6P00_A0_SILICON, "hpm6p00" },
};

static const char * hpm_get_series_name(uint32_t magic)
{
    const char *series_name = "Unknown";
    for (uint32_t i = 0; i < sizeof(hpm_soc_maps)/sizeof(hpm_soc_maps[0]); i++) {
        if (magic == hpm_soc_maps[i].magic) {
            series_name = hpm_soc_maps[i].series;
            break;
        }
    }
    return series_name;
}

static uint32_t hpm_get_soc_magic(target_s *target)
{
    const uint32_t hpm_soc_id_tag = 0x022010bfUL;
    const uint32_t hpm_rom_tag = (uint32_t)ROM_API_TABLE_ROOT + 0x30;
    uint32_t value;
    uint32_t soc_magic = 0xffffffff;
    do {
        /* Probe soc magic if the offset 0x30 of ROM API Tree Root is non-zero */
        hpm_read_u32_via_sysbus(target,&value, hpm_rom_tag);
        if (value != 0) {
            soc_magic = value;
            break;
        }
        /* Probe soc magic for the old-style BootROM */
        for (uint32_t i = 0; i < sizeof(hpm_soc_set)/sizeof(hpm_soc_set[0]); i++) {
            hpm_read_u32_via_sysbus(target,&value, hpm_soc_set[i]);
            if (value == hpm_soc_id_tag) {
                soc_magic = hpm_soc_set[i];
                break;
            }
        }
    } while (false);
    return soc_magic;
}

static hpm_xpi_cfg_t xpi_cfgs;

const static uint8_t hpm_flash_algo[]=
{
    0xef,0x00,0x00,0x05,0x02,0x90,0xef,0x00,0x60,0x0f,0x02,0x90,0xef,0x00,0x60,0x1f,
    0x02,0x90,0xef,0x00,0x00,0x22,0x02,0x90,0xef,0x00,0xc0,0x24,0x02,0x90,0xef,0x00,
    0x60,0x26,0x02,0x90,0xef,0x00,0x80,0x28,0x02,0x90,0x9c,0x41,0x05,0x47,0xbd,0x8b,
    0x63,0x7f,0xf7,0x00,0x9c,0x45,0x05,0x67,0x13,0x07,0x07,0xf0,0xf9,0x8f,0x13,0x07,
    0x00,0x10,0x63,0x96,0xe7,0x00,0x23,0x20,0x05,0x06,0x23,0x22,0x05,0x06,0x82,0x80,
    0x39,0x71,0x22,0xdc,0x17,0x04,0x00,0x00,0x03,0x24,0x44,0x37,0x83,0x47,0x04,0x00,
    0x4a,0xd8,0x06,0xde,0x26,0xda,0x4e,0xd6,0x17,0x09,0x00,0x00,0x03,0x29,0x49,0x36,
    0x23,0x20,0xe9,0x00,0xa5,0xef,0x97,0x04,0x00,0x00,0x83,0xa4,0xa4,0x34,0x3a,0x85,
    0x02,0xcc,0x02,0xce,0xa6,0x87,0x13,0x88,0x04,0x10,0x23,0xa0,0x07,0x00,0x91,0x07,
    0xe3,0x1d,0xf8,0xfe,0x93,0xd7,0x76,0x00,0x89,0x8b,0x17,0x07,0x00,0x00,0x03,0x27,
    0xa7,0x32,0x1c,0xc3,0xb7,0x07,0x02,0x20,0x83,0xa7,0x47,0xf1,0x93,0x09,0xc1,0x00,
    0x2e,0xc6,0xfc,0x47,0x32,0xc8,0x36,0xca,0x4e,0x86,0x97,0x05,0x00,0x00,0x83,0xa5,
    0x65,0x30,0x82,0x97,0x01,0xc9,0xf2,0x50,0x62,0x54,0xd2,0x54,0x42,0x59,0xb2,0x59,
    0x21,0x61,0x82,0x80,0x03,0x25,0x09,0x00,0xce,0x85,0x81,0x3f,0x83,0x47,0x04,0x00,
    0x23,0x8a,0x04,0x02,0x81,0xe7,0x85,0x47,0x23,0x00,0xf4,0x00,0xf2,0x50,0x62,0x54,
    0xd2,0x54,0x42,0x59,0xb2,0x59,0x01,0x45,0x21,0x61,0x82,0x80,0x01,0x11,0x97,0x07,
    0x00,0x00,0x83,0xa7,0x27,0x2c,0x22,0xcc,0x03,0xd4,0x87,0x02,0x26,0xca,0x4a,0xc8,
    0x06,0xce,0x4e,0xc6,0x52,0xc4,0x56,0xc2,0x5a,0xc0,0x2a,0x04,0xb2,0x84,0x2e,0x89,
    0x63,0x7e,0x86,0x00,0x01,0x45,0xc9,0xec,0xf2,0x40,0x62,0x44,0xd2,0x44,0x42,0x49,
    0xb2,0x49,0x22,0x4a,0x92,0x4a,0x02,0x4b,0x05,0x61,0x82,0x80,0xb3,0xf9,0x85,0x02,
    0x33,0x0a,0x34,0x41,0x63,0x0d,0x44,0x03,0x37,0x07,0x02,0x20,0x03,0x27,0x47,0xf1,
    0x17,0x0b,0x00,0x00,0x03,0x2b,0x4b,0x27,0x97,0x0a,0x00,0x00,0x83,0xaa,0x4a,0x27,
    0x03,0x28,0x87,0x01,0x83,0x25,0x0b,0x00,0x03,0xa5,0x0a,0x00,0x52,0x87,0xca,0x86,
    0x3e,0x86,0x02,0x98,0x55,0xf9,0xa6,0x99,0xb3,0x84,0x89,0x40,0x52,0x99,0xe3,0x73,
    0x94,0xfa,0x17,0x0b,0x00,0x00,0x03,0x2b,0x2b,0x24,0x97,0x0a,0x00,0x00,0x83,0xaa,
    0x2a,0x24,0x37,0x0a,0x02,0x20,0x97,0x09,0x00,0x00,0x83,0xa9,0xa9,0x22,0x21,0xa0,
    0x22,0x99,0x63,0x77,0x94,0x02,0x83,0x27,0x4a,0xf1,0x83,0x25,0x0b,0x00,0x03,0xa5,
    0x0a,0x00,0xdc,0x53,0xca,0x86,0x4e,0x86,0x82,0x97,0x81,0x8c,0x75,0xd1,0xad,0xb7,
    0x17,0x0b,0x00,0x00,0x03,0x2b,0x4b,0x20,0x97,0x0a,0x00,0x00,0x83,0xaa,0x4a,0x20,
    0xb7,0x07,0x02,0x20,0x83,0xa7,0x47,0xf1,0x62,0x44,0x83,0x25,0x0b,0x00,0x03,0xa5,
    0x0a,0x00,0xf2,0x40,0xb2,0x49,0x22,0x4a,0x92,0x4a,0x02,0x4b,0x9c,0x4f,0x26,0x87,
    0xca,0x86,0xd2,0x44,0x42,0x49,0x17,0x06,0x00,0x00,0x03,0x26,0xa6,0x1c,0x05,0x61,
    0x82,0x87,0xb7,0x07,0x02,0x20,0x83,0xa7,0x47,0xf1,0x2e,0x87,0x03,0xa8,0x87,0x02,
    0x97,0x07,0x00,0x00,0x83,0xa7,0x47,0x1b,0x8c,0x43,0x97,0x07,0x00,0x00,0x83,0xa7,
    0x27,0x1b,0x88,0x43,0xb6,0x87,0xb2,0x86,0x17,0x06,0x00,0x00,0x03,0x26,0x86,0x19,
    0x02,0x88,0xb7,0x07,0x02,0x20,0x83,0xa7,0x47,0xf1,0xae,0x88,0x32,0x87,0x03,0xa8,
    0xc7,0x02,0x97,0x07,0x00,0x00,0x83,0xa7,0x27,0x18,0x8c,0x43,0x97,0x07,0x00,0x00,
    0x83,0xa7,0x07,0x18,0x88,0x43,0x17,0x06,0x00,0x00,0x03,0x26,0xa6,0x16,0xb6,0x87,
    0xc6,0x86,0x02,0x88,0x91,0xcd,0x97,0x07,0x00,0x00,0x83,0xa7,0xa7,0x15,0x98,0x53,
    0x83,0xd7,0x67,0x02,0x01,0x45,0x2a,0x07,0xaa,0x07,0x98,0xc1,0xdc,0xc1,0x82,0x80,
    0x09,0x45,0x82,0x80,0xb7,0x07,0x02,0x20,0x83,0xa7,0x47,0xf1,0x17,0x07,0x00,0x00,
    0x03,0x27,0x87,0x13,0x0c,0x43,0x17,0x07,0x00,0x00,0x03,0x27,0x67,0x13,0xdc,0x4f,
    0x08,0x43,0x17,0x06,0x00,0x00,0x03,0x26,0xe6,0x11,0x82,0x87,0x82,0x80,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xbc,0x02,0x00,0x00,0xb4,0x02,0x00,0x00,0xb8,0x02,0x00,0x00,0xb0,0x02,0x00,0x00,
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,
};

static bool hpm_xpi_cmd_load(target_s *target, int argc, const char **argv)
{
    /* load flash algo */
    target->mem_write(target,HPM_ALGO_LOAD_BASE_ADDR,hpm_flash_algo,sizeof(hpm_flash_algo));
    return true;
}

static bool hpm_xpi_cmd_init(target_s *target, int argc, const char **argv)
{
    uint32_t tmp,timeout=0;
    tmp = HPM_ALGO_STACK_BASE_ADDR;
    target->reg_write(target,2,&tmp,4);/* set sp  */
    target->reg_write(target,10,&xpi_cfgs.flash_base,4);/* set flash addr  */   
    target->reg_write(target,11,&xpi_cfgs.header,4);/* set flash header  */
    target->reg_write(target,12,&xpi_cfgs.opt0,4);/* set flash op0  */
    target->reg_write(target,13,&xpi_cfgs.opt1,4);/* set flash op1  */
    target->reg_write(target,14,&xpi_cfgs.xpi_base,4);/* set flash io_base  */
    tmp = HPM_ALGO_LOAD_BASE_ADDR+HPM_FLASH_INIT;
    target->reg_write(target,32,&tmp,4);/* set pc  */
    tmp = HPM_ALGO_LOAD_BASE_ADDR+HPM_FLASH_INIT+4;
    target->reg_write(target,1,&tmp,4);/* set ra  */
    target->halt_resume(target, 0);/* run flash algo */
    do {
        if (TARGET_HALT_REQUEST==target->halt_poll(target, NULL))
            break;
        platform_delay(1);
    }while (timeout++<HPM_ALGO_CMD_TIMEOUT);
    target->reg_read(target, 10, &tmp, 4);
    if((tmp)||(timeout>=HPM_ALGO_CMD_TIMEOUT))
        return false;
    return true;
}

static bool hpm_xpi_cmd_info(target_s *target, int argc, const char **argv)
{
    uint32_t tmp,timeout=0;
    hpm_flash_info_t flash_info;
    tc_printf(target,"  cfg_xpi_header: 0x%x \n",xpi_cfgs.header);
	tc_printf(target,"  cfg_flash_base: 0x%x \n",xpi_cfgs.flash_base);
	tc_printf(target,"  cfg_flash_size: 0x%x \n",xpi_cfgs.flash_size);
	tc_printf(target,"    cfg_xpi_base: 0x%x \n",xpi_cfgs.xpi_base);
	tc_printf(target,"        cfg_opt0: 0x%x \n",xpi_cfgs.opt0);
	tc_printf(target,"        cfg_opt1: 0x%x \n",xpi_cfgs.opt1);
    hpm_xpi_cmd_load(target,0,NULL);
    hpm_xpi_cmd_init(target,0,NULL);
    tmp = HPM_ALGO_STACK_BASE_ADDR;
    target->reg_write(target,2,&tmp,4);/* set sp  */
    target->reg_write(target,10,&xpi_cfgs.flash_base,4);/* set flash addr  */   
    tmp = HPM_ALGO_BUFF_BASE_ADDR;
    target->reg_write(target,11,&tmp,4);/* set flash header  */
    tmp = HPM_ALGO_LOAD_BASE_ADDR+HPM_FLASH_GET_INFO;
    target->reg_write(target,32,&tmp,4);/* set pc  */
    tmp = HPM_ALGO_LOAD_BASE_ADDR+HPM_FLASH_GET_INFO+4;
    target->reg_write(target,1,&tmp,4);/* set ra  */
    target->halt_resume(target, 0);/* run flash algo */
    do {
        if (TARGET_HALT_REQUEST==target->halt_poll(target, NULL))
            break;
        platform_delay(1);
    }while (timeout++<HPM_ALGO_CMD_TIMEOUT);
    target->reg_read(target, 10, &tmp, 4);
    if((tmp)||(timeout>=HPM_ALGO_CMD_TIMEOUT))
        return true;
    target->mem_read(target,&flash_info,HPM_ALGO_BUFF_BASE_ADDR,sizeof(flash_info));/* get flash info */
    tc_printf(target," real total size: 0x%x \n",flash_info.total_sz_in_bytes);
	tc_printf(target,"real sector size: 0x%x \n",flash_info.sector_sz_in_bytes);
    return true;
}

static bool hpm_xpi_cmd_erase(target_s *const target, int argc, const char **argv)
{
    uint32_t tmp,timeout=0;
    tmp = HPM_ALGO_STACK_BASE_ADDR;
    target->reg_write(target,2,&tmp,4);/* set sp  */
    target->reg_write(target,10,&xpi_cfgs.flash_base,4);/* set flash addr  */   
    tmp = 0;
    target->reg_write(target,11,&tmp,4);/* set addr  */
    tmp = target->flash->length;
    target->reg_write(target,12,&tmp,4);/* set size  */
    tmp = HPM_ALGO_LOAD_BASE_ADDR+HPM_FLASH_ERASE;
    target->reg_write(target,32,&tmp,4);/* set pc  */
    tmp = HPM_ALGO_LOAD_BASE_ADDR+HPM_FLASH_ERASE+4;
    target->reg_write(target,1,&tmp,4);/* set ra  */
    target->halt_resume(target, 0);/* run flash algo */
    do {
        if (TARGET_HALT_REQUEST==target->halt_poll(target, NULL))
            break;
        platform_delay(1);
    }while (timeout++<HPM_ALGO_ERASE_TIMEOUT);
    target->reg_read(target, 10, &tmp, 4);
    if((tmp)||(timeout>=HPM_ALGO_ERASE_TIMEOUT))
        return false;
    return true;
}

static bool hpm_xpi_cmd_series(target_s *const target, int argc, const char **argv)
{
	const char *series_name = "Unknown";
	uint32_t soc_magic = hpm_get_soc_magic(target);
    series_name = hpm_get_series_name(soc_magic);
	tc_printf(target,"hpmicro chip info: %s \n",series_name);
	return true;
}

static bool hpm_xpi_cmd_config(target_s *const target, int argc, const char **argv)
{
	if ((argc<4)||(argc>6))
	{
		tc_printf(target,"xpi_cfg args invalid \n");
		return false;
	}
	xpi_cfgs.flash_base = strtoul(argv[1], NULL, 0);
	xpi_cfgs.flash_size = strtoul(argv[2], NULL, 0);
	xpi_cfgs.xpi_base = strtoul(argv[3], NULL, 0);
	switch(argc)
	{
		case 4:
			xpi_cfgs.header = HPM_XPI_HDR_DEFAULT;
			xpi_cfgs.opt0 = HPM_XPI_OPT0_DEFAULT;
			xpi_cfgs.opt1 = HPM_XPI_OPT1_DEFAULT;
			break;
		case 5:
			xpi_cfgs.header = HPM_XPI_HDR_DEFAULT + 1;
			xpi_cfgs.opt0 = strtoul(argv[4], NULL, 0);
			xpi_cfgs.opt1 = HPM_XPI_OPT1_DEFAULT;
			break;
		case 6:
            xpi_cfgs.header = HPM_XPI_HDR_DEFAULT + 1;
			xpi_cfgs.opt0 = strtoul(argv[4], NULL, 0);
			xpi_cfgs.opt1 = strtoul(argv[5], NULL, 0);
			break;
		default:
			return false;
	}
	target->flash->start = xpi_cfgs.flash_base;
	target->flash->length = xpi_cfgs.flash_size;
	return true;
}

const command_s hpm_xpi_cmd_list[] = {
	{"chip_info", hpm_xpi_cmd_series, "hpm_xpi_cmd_series"},
	{"xpi_cfg", hpm_xpi_cmd_config, "<flash_base> <flash_size> <xpi_base> [opt0] [opt1]"},
    {"xpi_info", hpm_xpi_cmd_info, "hpm_xpi_cmd_info"},
	{NULL, NULL, NULL},
};

static bool hpm_xpi_flash_mass_erase(target_flash_s *flash, platform_timeout_s *print_progess)
{
    target_s *target = flash->t;
    hpm_xpi_cmd_load(target,0,NULL);
	hpm_xpi_cmd_init(target,0,NULL);
    hpm_xpi_cmd_info(target,0,NULL);
	hpm_xpi_cmd_erase(target,0,NULL);
    return true;
}

static bool hpm_xpi_flash_prepare(target_flash_s *flash)
{
    target_s *target = flash->t;
    uint32_t tmp,timeout=0;

	target->mem_write(target,HPM_ALGO_LOAD_BASE_ADDR,hpm_flash_algo,sizeof(hpm_flash_algo));
    /* write args to flash init register */
	target->reg_write(target,10U,&xpi_cfgs.flash_base,4);/* a0 */
	target->reg_write(target,11U,&xpi_cfgs.header,4);/* a1  */
	target->reg_write(target,12U,&xpi_cfgs.opt0,4);/* a2  */
	target->reg_write(target,13U,&xpi_cfgs.opt1,4);/* a3  */
	target->reg_write(target,14U,&xpi_cfgs.xpi_base,4);/* a4  */
	tmp = HPM_ALGO_LOAD_BASE_ADDR+HPM_FLASH_INIT;/* modify pc */
	target->reg_write(target,32U,&tmp,4);
	tmp = HPM_ALGO_LOAD_BASE_ADDR+HPM_FLASH_INIT+4;/* modify ra */
	target->reg_write(target,1U,&tmp,4);
	tmp = HPM_ALGO_STACK_BASE_ADDR;
	target->reg_write(target,2U,&tmp,4);/* set sp */
	target->halt_resume(target,false);/* run algorithm */
	do{
		if(TARGET_HALT_REQUEST==target->halt_poll(target,NULL))
			break;
		platform_delay(1);
	}while(timeout++<HPM_ALGO_CMD_TIMEOUT);
	target->reg_read(target,10,&tmp,4);
	if((tmp)||(timeout>=HPM_ALGO_CMD_TIMEOUT))
		return false;
    return true;
}

static bool hpm_xpi_flash_erase(target_flash_s *flash, target_addr_t addr, size_t len)
{
    uint32_t tmp,timeout=0;
    target_s *target = flash->t;
    target->reg_write(target,10,&xpi_cfgs.flash_base,4);/* set flash base  */   
    tmp = addr-xpi_cfgs.flash_base;
    target->reg_write(target,11,&tmp,4);/* set addr offset  */
    target->reg_write(target,12,&len,4);/* set size  */
    tmp = HPM_ALGO_STACK_BASE_ADDR;
    target->reg_write(target,2,&tmp,4);/* set sp  */
    tmp = HPM_ALGO_LOAD_BASE_ADDR+HPM_FLASH_ERASE;
    target->reg_write(target,32,&tmp,4);/* set pc  */
    tmp = HPM_ALGO_LOAD_BASE_ADDR+HPM_FLASH_GET_INFO+4;
    target->reg_write(target,1,&tmp,4);/* set ra  */
    target->halt_resume(target, 0);/* run flash algo */
    do {
        if (TARGET_HALT_REQUEST==target->halt_poll(target, NULL))
            break;
        platform_delay(1);
    }while (timeout++<HPM_ALGO_ERASE_TIMEOUT);
    target->reg_read(target, 10, &tmp, 4);/* get return value */
    if((tmp)||(timeout>=HPM_ALGO_ERASE_TIMEOUT))
        return false;
    return true;
}

static bool hpm_xpi_flash_write(target_flash_s *flash, target_addr_t dest, const void *src, size_t len)
{
    uint32_t tmp,timeout=0;
    target_s *target = flash->t;
    target->mem_write(target,HPM_ALGO_BUFF_BASE_ADDR,src,len);/* load data to sram */
    target->reg_write(target,10,&xpi_cfgs.flash_base,4);/* set flash base  */   
    tmp = dest-xpi_cfgs.flash_base;
    target->reg_write(target,11,&tmp,4);/* set addr offset  */
    tmp = HPM_ALGO_BUFF_BASE_ADDR;
    target->reg_write(target,12,&tmp,4);/* set size  */
    target->reg_write(target,13,&len,4);/* set size  */
    tmp = HPM_ALGO_STACK_BASE_ADDR;
    target->reg_write(target,2,&tmp,4);/* set sp  */
    tmp = HPM_ALGO_LOAD_BASE_ADDR+HPM_FLASH_PROGRAM;
    target->reg_write(target,32,&tmp,4);/* set pc  */
    tmp = HPM_ALGO_LOAD_BASE_ADDR+HPM_FLASH_PROGRAM+4;
    target->reg_write(target,1,&tmp,4);/* set ra  */
    target->halt_resume(target, 0);/* run flash algo */
    do {
        if (TARGET_HALT_REQUEST==target->halt_poll(target, NULL))
            break;
        platform_delay(1);
    }while(timeout++<HPM_ALGO_WRITE_TIMEOUT);
    target->reg_read(target, 10, &tmp, 4);/* get return value */
    if((tmp)||(timeout>=HPM_ALGO_WRITE_TIMEOUT))
        return false;
    return true;
}

static void hpm_xpi_add_flash(target_s *target, uint32_t addr, size_t length, size_t erasesize)
{
    target_flash_s *flash = calloc(1, sizeof(*flash));
    flash->start = addr;
    flash->length = length;
    flash->blocksize = erasesize;
    flash->writesize = erasesize;
    flash->mass_erase = hpm_xpi_flash_mass_erase;
    flash->prepare = hpm_xpi_flash_prepare;
    flash->erase = hpm_xpi_flash_erase;
    flash->write = hpm_xpi_flash_write;
    flash->erased = 0xff;
    target_add_flash(target, flash);
}

bool hpm_xpi_probe(target_s *const target)
{
    uint32_t soc_magic = hpm_get_soc_magic(target);
    target->driver = hpm_get_series_name(soc_magic);
    if(!strcmp("Unknown", target->driver))
	    target->driver = "HPMicro";

	xpi_cfgs.header = HPM_XPI_HDR_DEFAULT;
	xpi_cfgs.xpi_base = HPM_XPI_BASE_DEFAULT;
	xpi_cfgs.flash_base = HPM_XPI_FLASH_BASE;
	xpi_cfgs.flash_size = HPM_XPI_FLASH_SIZE;
	xpi_cfgs.opt0 = HPM_XPI_OPT0_DEFAULT;
	xpi_cfgs.opt1 = HPM_XPI_OPT1_DEFAULT;
	xpi_cfgs.sector_size = HPM_XPI_PAGE_SIZE;

    switch (soc_magic) 
    {
        case HPM6700_A0_SILICON:
        case HPM6700_A1_SILICON:
        {
            xpi_cfgs.xpi_base = 0xF3040000;
            target_add_ram32(target, 0x00000000, 256*1024);
            target_add_ram32(target, 0x00080000, 256*1024);
            target_add_ram32(target, 0x01080000, 512*1024);
            target_add_ram32(target, 0x01100000, 256*1024);
            target_add_ram32(target, 0x0117C000, 16*1024);
            target_add_ram32(target, 0xF0300000, 32*1024);
            target_add_ram32(target, 0xF40F0000, 8*1024);
            break;
        }
        case HPM6300_A0_SILICON:
        case HPM6300_A1_SILICON:
        {
            xpi_cfgs.xpi_base = 0xF3040000;
            xpi_cfgs.flash_size = 0x1000000;
            target_add_ram32(target, 0x00000000, 128*1024);
            target_add_ram32(target, 0x00080000, 128*1024);
            target_add_ram32(target, 0x01080000, 256*1024);
            target_add_ram32(target, 0x010C0000, 256*1024);
            target_add_ram32(target, 0xF0300000, 32*1024);
            break;
        }
        case HPM6200_A0_SILICON:
        case HPM6200_A1_SILICON:
        case HPM6200_A2_SILICON:
        {
            xpi_cfgs.xpi_base = 0xF3040000;
            xpi_cfgs.flash_size = 0x1000000;
            target_add_ram32(target, 0x00000000, 128*1024);
            target_add_ram32(target, 0x00080000, 128*1024);
            target_add_ram32(target, 0x01080000, 128*1024);
            target_add_ram32(target, 0x010A0000, 128*1024);
            target_add_ram32(target, 0xF0300000, 32*1024);
            break;
        }
        case HPM6800_A0_SILICON:
        {
            target_add_ram32(target, 0x00000000, 256*1024);
            target_add_ram32(target, 0x00080000, 256*1024);
            target_add_ram32(target, 0x01200000, 256*1024);
            target_add_ram32(target, 0x01240000, 256*1024);
            target_add_ram32(target, 0xF0400000, 32*1024);
            target_add_ram32(target, 0xF4130000, 16*1024);
            break;
        }
        case HPM5300_A0_SILICON:
        {
            xpi_cfgs.header = HPM_XPI_HDR_DEFAULT + 1;
            xpi_cfgs.opt0 = 6;
            xpi_cfgs.opt1 = 0x1000;
            target_add_ram32(target, 0x00000000, 128*1024);
            target_add_ram32(target, 0x00080000, 128*1024);
            target_add_ram32(target, 0xf0400000, 32*1024);
            break;
        }
        case HPM6E00_A0_SILICON:
        {
            xpi_cfgs.opt0 = 7;
            xpi_cfgs.opt1 = 0;
            target_add_ram32(target, 0x00000000, 256*1024);
            target_add_ram32(target, 0x00200000, 256*1024);
            target_add_ram32(target, 0x01200000, 512*1024);
            target_add_ram32(target, 0x01280000, 256*1024);
            target_add_ram32(target, 0x012FC000, 16*1024);
            target_add_ram32(target, 0xF0200000, 32*1024);
            break;
        }
        case HPM6P00_A0_SILICON:
        {
            xpi_cfgs.header = HPM_XPI_HDR_DEFAULT+1;
            xpi_cfgs.opt0 = 5;
            xpi_cfgs.opt1 = 0x1000;
            target_add_ram32(target, 0x00000000, 128*1024);
            target_add_ram32(target, 0x00200000, 128*1024);
            target_add_ram32(target, 0x01200000, 128*1024);
            target_add_ram32(target, 0x01220000, 128*1024);
            target_add_ram32(target, 0xF0200000, 32*1024);
            break;
        }
        default:
            break;
    }
	hpm_xpi_add_flash(target, xpi_cfgs.flash_base, xpi_cfgs.flash_size, xpi_cfgs.sector_size);
	target_add_commands(target, hpm_xpi_cmd_list, target->driver);
    return true;
}

