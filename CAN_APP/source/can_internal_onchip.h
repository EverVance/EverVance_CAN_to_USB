#ifndef CAN_INTERNAL_ONCHIP_H
#define CAN_INTERNAL_ONCHIP_H

#include <stdbool.h>

#include "can_types.h"

bool CAN_InternalOnChipInit(void);
bool CAN_InternalOnChipApplyConfig(can_channel_t channel, const can_channel_config_t *config);
void CAN_InternalOnChipTask(void);
/* 鎸夐€氶亾鎵ц鐗囦笂 CAN 鍛ㄦ湡浠诲姟锛堝綋鍓嶅疄鐜颁负杞欢鐜洖锛夈€?*/
void CAN_InternalOnChipTaskChannel(can_channel_t channel);
/* 鍙戦€佹帴鍙ｏ細鍚庣画浼氭浛鎹负鐪熷疄 FlexCAN 閭鍙戦€併€?*/
bool CAN_InternalOnChipSend(can_channel_t channel, const can_frame_t *frame);
/* 鎺ユ敹鎺ュ彛锛氬悗缁細鏇挎崲涓虹湡瀹?FlexCAN FIFO/閭鎺ユ敹銆?*/
bool CAN_InternalOnChipReceive(can_channel_t channel, can_frame_t *frame);

#endif
