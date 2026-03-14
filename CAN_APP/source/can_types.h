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
    kCanBusEvent_None = 0,
    kCanBusEvent_RxFrame,
    kCanBusEvent_TxComplete,
    kCanBusEvent_Error
} can_bus_event_type_t;

typedef struct
{
    can_bus_event_type_t type;
    can_frame_t frame;
    uint8_t errorCode;
    uint8_t isTx;
    uint16_t reserved0;
    uint32_t rawStatus;
} can_bus_event_t;

typedef struct
{
    uint8_t busOff;
    uint8_t errorPassive;
    uint8_t rxPending;
    uint8_t txPending;
    uint8_t reserved0;
    uint8_t reserved1;
    uint16_t reserved2;
    uint32_t lastErrorCode;
} can_driver_runtime_state_t;

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
