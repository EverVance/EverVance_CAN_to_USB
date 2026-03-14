#ifndef __CAN_TYPEDEF_H__
#define __CAN_TYPEDEF_H__


typedef  struct  _CANFD_INIT_CONFIG
{
    unsigned char Mode; //0-正常模式，1-自发自收模式
    unsigned char ISOCRCEnable;//0-禁止ISO CRC,1-使能ISO CRC
    unsigned char RetrySend;//0-禁止重发，1-无限制重发
    unsigned char ResEnable;//0-不接入内部120欧终端电阻，1-接入内部120欧终端电阻
    //波特率参数可以用TCANLINPro软件里面的波特率计算工具计算
    //仲裁段波特率参数,波特率=40M/NBT_BRP*(1+NBT_SEG1+NBT_SEG2)
    unsigned char NBT_BRP;
    unsigned char NBT_SEG1;
    unsigned char NBT_SEG2;
    unsigned char NBT_SJW;
    //数据段波特率参数,波特率=40M/DBT_BRP*(1+DBT_SEG1+DBT_SEG2)
    unsigned char DBT_BRP;
    unsigned char DBT_SEG1;
    unsigned char DBT_SEG2;
    unsigned char DBT_SJW;
    unsigned char __Res0[8];
}CANFD_INIT_CONFIG;


#endif
