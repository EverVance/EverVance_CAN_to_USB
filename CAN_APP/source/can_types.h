#ifndef CAN_TYPES_H
#define CAN_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    kCanChannel_CanFd1Ext = 0, /* CH0: LPSPI1 + MCP2517FD */
    kCanChannel_Can2,          /* CH1: FLEXCAN3 */
    kCanChannel_Can3,          /* CH2: FLEXCAN1 */
    kCanChannel_Can4,          /* CH3: FLEXCAN2 */
    kCanChannel_Count
} can_channel_t;

typedef struct
{
    uint32_t id;
    uint8_t dlc;
    uint8_t flags;
    uint8_t data[64];
} can_frame_t;

typedef enum
{
    kCanFrameFormat_Classic = 0,
    kCanFrameFormat_Fd = 1
} can_frame_format_t;

typedef struct
{
    bool enabled;
    bool terminationEnabled;
    can_frame_format_t frameFormat;
    uint32_t nominalBitrate;
    uint16_t nominalSamplePointPermille;
    uint32_t dataBitrate;
    uint16_t dataSamplePointPermille;
} can_channel_config_t;

#endif
