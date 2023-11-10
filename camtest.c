/*
 * Copyright (c) 2012-2021 Israel Jacquez
 * See LICENSE for details.
 *
 * Israel Jacquez <mrkotfw@gmail.com>
 * Nikita Sokolov <hitomi2500@mail.ru>
 */

#include <yaul.h>

#include <stdio.h>
#include <stdlib.h>

//some test locations for read and write buffers
#define SCI_BUFFER_TX 0x26080000
#define SCI_BUFFER_RX 0x260A0000

//doing 64K SCI transaction
#define TEST_SEQUENCE_LENGTH 0x800 

static void _dmac_handler0(void *);

static volatile uint16_t _frt = 0;
static volatile uint32_t _ovf = 0;
static volatile bool _done = false;

#define ORDER_SYSTEM_CLIP_COORDS_INDEX  0
#define ORDER_LOCAL_COORDS_INDEX        1
#define ORDER_SPRITE_INDEX             2
#define ORDER_DRAW_END_INDEX            3
#define ORDER_COUNT                     4

#define SCREEN_WIDTH    320
#define SCREEN_HEIGHT   240

//there is no core func for this yet, should be moved to not-yet-existent SCI driver 
static inline void __always_inline
cpu_sci_interrupt_priority_set(uint8_t priority)
{
        MEMORY_WRITE_AND(16, CPU(IPRB), 0x7FFF);
        MEMORY_WRITE_OR(16, CPU(IPRB), (priority & 0x0F) << 12);
}

void
sci_setup()
{
	MEMORY_WRITE(8,CPU(SCR),0x00); //stop all
	MEMORY_WRITE(8,CPU(SMR),0x80); //sync mode, 8bit, no parity, no MP, 1/4 clock
	MEMORY_WRITE(8,CPU(BRR),0x00); //maximum baudrate
	MEMORY_WRITE(8,CPU(SCR),0x01); //internal clock output
	MEMORY_WRITE(8,CPU(SCR),0xF1); //interrupts on, RX/TX on, internal clock output
}

static void
_vdp1_drawing_list_init(vdp1_cmdt_list_t *cmdt_list)
{
        static const int16_vec2_t local_coord_ul =
            INT16_VEC2_INITIALIZER(0,
                                      0);

        static const vdp1_cmdt_draw_mode_t sprite_draw_mode = {
                .raw = 0x0000,
                .bits.pre_clipping_disable = true
        };

        assert(cmdt_list != NULL);

        vdp1_cmdt_t *cmdts;
        cmdts = &cmdt_list->cmdts[0];

        (void)memset(&cmdts[0], 0x00, sizeof(vdp1_cmdt_t) * ORDER_COUNT);

        cmdt_list->count = ORDER_COUNT;

        vdp1_cmdt_normal_sprite_set(&cmdts[ORDER_SPRITE_INDEX]);
        vdp1_cmdt_param_draw_mode_set(&cmdts[ORDER_SPRITE_INDEX], sprite_draw_mode);

        vdp1_cmdt_system_clip_coord_set(&cmdts[ORDER_SYSTEM_CLIP_COORDS_INDEX]);

        vdp1_cmdt_local_coord_set(&cmdts[ORDER_LOCAL_COORDS_INDEX]);
        vdp1_cmdt_param_vertex_set(&cmdts[ORDER_LOCAL_COORDS_INDEX],
            CMDT_VTX_LOCAL_COORD, &local_coord_ul);

        vdp1_cmdt_end_set(&cmdts[ORDER_DRAW_END_INDEX]);
}

static void
_vdp1_drawing_list_set(vdp1_cmdt_list_t *cmdt_list)
{
        /*static vdp1_cmdt_color_bank_t polygon_color_bank = {
                .type_0.data.dc = 16
        };*/

        vdp1_cmdt_color_bank_t dummy_bank;
        dummy_bank.raw = 0;

        assert(cmdt_list != NULL);

        vdp1_cmdt_t *cmdts;
        cmdts = &cmdt_list->cmdts[0];

        vdp1_cmdt_t *cmdt_sprite;
        cmdt_sprite = &cmdts[ORDER_SPRITE_INDEX];

        vdp1_cmdt_t *cmdt_system_clip_coords;
        cmdt_system_clip_coords = &cmdts[ORDER_SYSTEM_CLIP_COORDS_INDEX];

        cmdt_system_clip_coords->cmd_xc = SCREEN_WIDTH - 1;
        cmdt_system_clip_coords->cmd_yc = SCREEN_HEIGHT - 1;

        //polygon_color_bank.type_8.data.dc = 16;

        cmdt_sprite->cmd_xa = 0;
        cmdt_sprite->cmd_ya = 0;//SCREEN_HEIGHT - 1;

        cmdt_sprite->cmd_xb = 0;//SCREEN_WIDTH - 1;
        cmdt_sprite->cmd_yb = 0;// SCREEN_HEIGHT - 1;

        cmdt_sprite->cmd_xc = 0;//SCREEN_WIDTH - 1;
        cmdt_sprite->cmd_yc = 0;

        cmdt_sprite->cmd_xd = 0;
        cmdt_sprite->cmd_yd = 0;

        //vdp1_cmdt_param_color_mode4_set(cmdt_polygon, dummy_bank);

        cmdt_sprite->cmd_pmod &= 0xFFC7;
        cmdt_sprite->cmd_pmod |= 0x0028;
        cmdt_sprite->cmd_colr = 0;//color_bank.raw & 0xFF00;
        cmdt_sprite->cmd_srca = 0x80;
        cmdt_sprite->cmd_size = 0x28F0; //320x240
        //cmdt_sprite->cmd_size = 0x1440; //320x240
        vdp1_cmdt_param_color_bank_set(cmdt_sprite, dummy_bank);

        //vdp1_cmdt_param_color_bank_set(cmdt_polygon, polygon_color_bank);
}

int
main(void)
{
        int i=0;

        vdp1_cmdt_list_t *cmdt_list;
        cmdt_list = vdp1_cmdt_list_alloc(5);

        dbgio_dev_default_init(DBGIO_DEV_VDP2_ASYNC);
        dbgio_dev_font_load();
        dbgio_dev_font_load_wait();

        _vdp1_drawing_list_init(cmdt_list);

        _vdp1_drawing_list_set(cmdt_list);
        dbgio_flush();
        vdp1_sync_cmdt_list_put(cmdt_list, 0, NULL, NULL);
        vdp_sync();

        cpu_dmac_interrupt_priority_set(8);
        cpu_sci_interrupt_priority_set(0); //disable interrupts from SCI, because they will be served by DMAC, not CPU

        //fill poly in VDP1 vram

        uint16_t * pVRAM_16 = (uint16_t *) VDP1_VRAM(0x400);
        for (int i=0;i<320*240;i++)
                pVRAM_16[i] = 0x8000 | i%0x8000;

 
        cpu_dmac_cfg_t cfg0 __unused = {
                .channel= 0,
                .src = SCI_BUFFER_TX,
                .src_mode = CPU_DMAC_SOURCE_INCREMENT,
                .dst = CPU(TDR),
                .dst_mode = CPU_DMAC_DESTINATION_FIXED,
                .len = TEST_SEQUENCE_LENGTH, 
                .stride = CPU_DMAC_STRIDE_1_BYTE,
                .request_mode = CPU_DMAC_REQUEST_MODE_MODULE,
                .detect_mode = CPU_DMAC_DETECT_MODE_EDGE,
                .bus_mode = CPU_DMAC_BUS_MODE_CYCLE_STEAL,
                .resource_select = CPU_DMAC_RESOURCE_SELECT_TXI,
                .ihr = _dmac_handler0,
                .ihr_work = NULL
        };

        cpu_dmac_cfg_t cfg1 __unused = {
                .channel= 1,
                .src = CPU(RDR),
                .src_mode = CPU_DMAC_SOURCE_FIXED,
                .dst = SCI_BUFFER_RX,
                .dst_mode = CPU_DMAC_DESTINATION_INCREMENT,
                .len = TEST_SEQUENCE_LENGTH,
                .stride = CPU_DMAC_STRIDE_1_BYTE,
                .request_mode = CPU_DMAC_REQUEST_MODE_MODULE,
                .detect_mode = CPU_DMAC_DETECT_MODE_EDGE,
                .bus_mode = CPU_DMAC_BUS_MODE_CYCLE_STEAL,
                .resource_select = CPU_DMAC_RESOURCE_SELECT_RXI,
                .ihr = NULL,//_dmac_handler0,
                .ihr_work = NULL
        };
                
        sci_setup();

        //prepare write buffer
        for (i=0;i<TEST_SEQUENCE_LENGTH;i++)
        {
                MEMORY_WRITE(8,SCI_BUFFER_TX+i,0xA5+i);
        }

        while (1)
        {



                //clear read buffer
                memset((uint8_t*)SCI_BUFFER_RX,0x00,TEST_SEQUENCE_LENGTH);

                //apply config to DMA channels
                cpu_dmac_channel_config_set(&cfg0);
                cpu_dmac_channel_config_set(&cfg1);

                //set dmac priority to round-robin
                cpu_dmac_priority_mode_set(CPU_DMAC_PRIORITY_MODE_ROUND_ROBIN);
                //enable dmac, because priority mode set disables it
                cpu_dmac_enable();
                
                //fire DMA
                _done = false;
                MEMORY_WRITE(8,CPU(SSR),0x00); //reset all SCI status flags
                MEMORY_WRITE(8,CPU(SCR),0x00); //stop SCI
                cpu_dmac_channel_start(1); //start the read channel first, so it's with sync with write channel
                cpu_dmac_channel_start(0); //start the write channel
                MEMORY_WRITE(8,CPU(SCR),0xF1); //enable SCI back, the requests to DMAC shoukld start automatically
                        
                //wait for DMA
                while (!_done) ;

                int index = MEMORY_READ(8,SCI_BUFFER_RX);
                memcpy((void*)VDP1_VRAM(0x400+index*TEST_SEQUENCE_LENGTH),(void*)SCI_BUFFER_RX,TEST_SEQUENCE_LENGTH);

                
                dbgio_printf(".");
                
                dbgio_flush();
                vdp_sync();
                
                //datacheck
                /*int iErrors = 0;
                uint8_t written,readen;
                for (i=0;i<TEST_SEQUENCE_LENGTH;i++)
                {
                        readen = MEMORY_READ(8,SCI_BUFFER_RX+i);
                        written = 0xA5+i;

                        if (readen !=written)
                        {
                                if (iErrors < 10)
                                {
                                        dbgio_printf("ERR: pos %u wr = 0x%02lX rd = 0x%02lX\n",iErrors,written,readen);
                                        dbgio_flush();
                                        vdp_sync();
                                }
                                iErrors ++; 
                        }
                }

                dbgio_printf("SCI loopback test error count = %u\n",iErrors);

                if (iErrors)
                {
                        dbgio_printf("\nSCI loopback test failed.\nEither pins 5 and 6 on CN6 (serial port)\nare not shorted, or you're running \nthe test within an emulator.\n");
                }
                else
                {
                        dbgio_printf("\nSCI loopback test was sucessful.\n");
                }

                dbgio_flush();
                vdp_sync(); */
        }

        //stop
        while (1);

        return 0;
}

void
user_init(void)
{
        vdp2_tvmd_display_res_set(VDP2_TVMD_INTERLACE_NONE, VDP2_TVMD_HORZ_NORMAL_A,
            VDP2_TVMD_VERT_224);

        vdp2_scrn_back_screen_color_set(VDP2_VRAM_ADDR(3, 0x01FFFE),
            COLOR_RGB1555(1, 0, 3, 15));

        cpu_intc_mask_set(0);

        vdp2_sprite_priority_set(0, 7);
        vdp2_sprite_priority_set(1, 7);
        vdp2_sprite_priority_set(2, 7);
        vdp2_sprite_priority_set(3, 7);
        vdp2_sprite_priority_set(4, 7);
        vdp2_sprite_priority_set(5, 7);
        vdp2_sprite_priority_set(6, 7);
        vdp2_sprite_priority_set(7, 7);

        vdp2_tvmd_display_set();
}

static void
_dmac_handler0(void *work __unused)
{
        _done = true;
}
