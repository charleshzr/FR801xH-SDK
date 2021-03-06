/**
 * Copyright (c) 2019, Freqchip
 *
 * All rights reserved.
 *
 *
 */

/*
 * INCLUDES
 */
#include <string.h>
#include "driver_plf.h"


#include "sbc.h"
#include "adpcm.h"
#ifdef ADPCM_IMA_FANGTANG
#include "adpcm_ima_fangtang.h"
#endif
#include "co_list.h"
#include "co_printf.h"
#include "os_task.h"
#include "os_mem.h"
#include "audio_encoder.h"



enum encoder_state_t
{
    ENCODER_STATE_IDLE,
    ENCODER_STATE_BUSY,
    ENCODER_STATE_MAX,
};


#define ENCODER_EVENT_NEXT_FRAME (0)


struct pcm_data_t
{
    struct co_list_hdr node;
    uint8_t  data[];
};

struct encoder_env_t
{
    sbc_t *sbc;

    struct co_list pcm_data_list;

    struct pcm_data_t *tmp_pcm;
    uint16_t tmp_pcm_write_pos;

    uint8_t *out_buffer;
    uint16_t send_read_pos;
    uint16_t encode_write_pos;
    uint16_t reserved_space;

    uint16_t block_size;
    uint16_t frame_size;
};

static enum encode_type encoder_type = ENCODE_TYPE_ADPCM;        // 0 = ADPCM; 1= SBC;
#ifdef ADPCM_IMA_FANGTANG
adpcm_state adpcm_ima_fangtang_state;
#endif
struct CodecState adpcm_state_;
struct encoder_env_t encoder_env;
static enum encoder_state_t encode_task_status = ENCODER_STATE_IDLE;
void audio_encode_start(encode_param_t param)
{
    uint8_t *sbc_str_buffer;
    //co_printf("encode_start\r\n");

    if(encode_task_status != ENCODER_STATE_IDLE)
        return;

    memset((void *)&encoder_env, 0, sizeof(struct encoder_env_t));

    encoder_env.sbc = (sbc_t *)os_malloc(sizeof(sbc_t));
    sbc_str_buffer = os_malloc(0x4F0);  //sizeof(struct sbc_priv) + SBC_ALIGN_MASK = 0x4F0

    sbc_init(encoder_env.sbc, (void *)sbc_str_buffer);

    encoder_env.sbc->frequency = param.freq;        //SBC_FREQ_48000, SBC_FREQ_16000
    encoder_env.sbc->blocks = SBC_BLK_16;
    encoder_env.sbc->subbands = SBC_SB_8;
    encoder_env.sbc->allocation = SBC_AM_LOUDNESS;
    encoder_env.sbc->bitpool = param.bitpool;      //1bitpool = 6~7 kbps, higher, high quality
    /*
        bitpool frame_len
        14      36
        21      50
        25      58
        26      60
        27      62
        29      66
        32      72
        34      76
        35      78
        36      80
    */
    encoder_env.block_size = sbc_get_codesize(encoder_env.sbc);     //0x100 = 256

    if(encoder_type == ENCODE_TYPE_ADPCM)
    {
        encoder_env.frame_size = encoder_env.block_size>>2;     //adpcm, fixed compressing rate == 4; frame_len = 64
        memset(&adpcm_state_,0x0,sizeof(adpcm_state_));
#ifdef ADPCM_IMA_FANGTANG
        memset(&adpcm_ima_fangtang_state,0x0,sizeof(adpcm_ima_fangtang_state));
#endif
    }
    else
        encoder_env.frame_size = sbc_get_frame_length(encoder_env.sbc);     //0x3A = 58

    co_printf("blk_sz:%d,frm_sz:%d\r\n",encoder_env.block_size,encoder_env.frame_size);

    co_list_init(&encoder_env.pcm_data_list);
    encoder_env.out_buffer = os_malloc(ENCODER_MAX_BUFFERING_BLOCK_COUNT*encoder_env.frame_size);       //0x244 = 580, <1block enc to 1frame>
    encoder_env.reserved_space = ENCODER_MAX_BUFFERING_BLOCK_COUNT*encoder_env.frame_size;      //0x244 = 580

    //co_printf("out_buf:%x,sbc:%x,priv:%x,str_buf:%x\r\n",encoder_env.out_buffer,encoder_env.sbc,encoder_env.sbc->priv_alloc_base,sbc_str_buffer);

    encoder_env.tmp_pcm = NULL;
    encode_task_status = ENCODER_STATE_BUSY;
}

void audio_encode_stop(void)
{
    //co_printf("encode_stop\r\n");

    if(encode_task_status == ENCODER_STATE_IDLE)
        return;

    GLOBAL_INT_DISABLE();
    memset(&adpcm_state_,0x0,sizeof(adpcm_state_));
#ifdef ADPCM_IMA_FANGTANG
    memset(&adpcm_ima_fangtang_state,0x0,sizeof(adpcm_ima_fangtang_state));
#endif
    if(encoder_env.tmp_pcm)
    {
        os_msg_free(encoder_env.tmp_pcm);
    }
    encode_task_status = ENCODER_STATE_IDLE;
    GLOBAL_INT_RESTORE();

    os_free(encoder_env.out_buffer);
    os_free(encoder_env.sbc->priv_alloc_base);
    os_free(encoder_env.sbc);
}
void audio_encode_store_pcm_data(uint16_t *data, uint8_t len)
{
    uint16_t store_len;

    if(encode_task_status == ENCODER_STATE_BUSY)
    {
        len *= 2;
        while(len)
        {
            if(encoder_env.tmp_pcm == NULL)
            {
                encoder_env.tmp_pcm = (struct pcm_data_t *)os_msg_malloc(ENCODER_EVENT_NEXT_FRAME,task_id_audio_encode,TASK_ID_NONE
                                      ,sizeof(struct pcm_data_t)+encoder_env.block_size );
                encoder_env.tmp_pcm_write_pos = 0;
            }

            store_len = encoder_env.block_size-encoder_env.tmp_pcm_write_pos;
            if(store_len >= len)
            {
                store_len = len;
            }

            memcpy(&encoder_env.tmp_pcm->data[encoder_env.tmp_pcm_write_pos], data, store_len);
            len -= store_len;
            encoder_env.tmp_pcm_write_pos += store_len;
            if(encoder_env.tmp_pcm_write_pos >= encoder_env.block_size)
            {
                os_msg_send(encoder_env.tmp_pcm);
                encoder_env.tmp_pcm = NULL;
            }
        }
    }
}
void  __attribute__((weak)) encoder_frame_out_func(uint8_t *frame_data,uint16_t frame_size)
{
    ;
}
static int audio_encoder_next_frame_handler(struct pcm_data_t *pcm_data)
{
//    int encoded_len, org_len;
    //printf("N:%d,%d\r\n",encoder_env.reserved_space,encoder_env.frame_size);

    if( (encode_task_status == ENCODER_STATE_BUSY) && (encoder_env.reserved_space >= encoder_env.frame_size) )
    {
        if(encoder_type == ENCODE_TYPE_ADPCM)
        {

#ifdef ADPCM_IMA_FANGTANG
            adpcm_coder((short *)(pcm_data->data)
                        , &encoder_env.out_buffer[encoder_env.encode_write_pos]
                        , encoder_env.block_size>>1
                        , &adpcm_ima_fangtang_state);
#else
            encode( &adpcm_state_
                    ,(short *)(pcm_data->data)
                    ,encoder_env.block_size>>1
                    ,&encoder_env.out_buffer[encoder_env.encode_write_pos]
                  );
#endif
        }
        else
        {
            int encoded_len, org_len;
            org_len = sbc_encode(encoder_env.sbc,
                                 pcm_data->data,
                                 encoder_env.block_size,
                                 &encoder_env.out_buffer[encoder_env.encode_write_pos],
                                 encoder_env.frame_size,
                                 &encoded_len);
        }
        encoder_env.reserved_space -= encoder_env.frame_size;
        encoder_frame_out_func(&encoder_env.out_buffer[encoder_env.encode_write_pos],encoder_env.frame_size);
        encoder_env.reserved_space += encoder_env.frame_size;
        encoder_env.encode_write_pos += encoder_env.frame_size;
        if(encoder_env.encode_write_pos >= ENCODER_MAX_BUFFERING_BLOCK_COUNT*encoder_env.frame_size)
        {
            encoder_env.encode_write_pos = 0;
        }

    }
    return 0;
}
uint16_t task_id_audio_encode = TASK_ID_NONE;
int audio_encode_task(os_event_t *param)
{
    switch(param->event_id)
    {
        case ENCODER_EVENT_NEXT_FRAME:
            audio_encoder_next_frame_handler( (struct pcm_data_t *)(param->param) );
            break;
    }
    return EVT_CONSUMED;
}
void audio_encoder_init(enum encode_type type)
{
    encoder_type = type;
    task_id_audio_encode = os_task_create(audio_encode_task);
    encode_task_status = ENCODER_STATE_IDLE;
}



