// main/xhs_config.h
// 空模板。配网记录在 NVS，不把 Wi-Fi 密码、后端地址或设备令牌写进固件。
// 四项都留空时设备进入热点配网，而不是停在失败页。
#pragma once

#define XHS_WIFI_SSID ""
#define XHS_WIFI_PASSWORD ""
#define XHS_BACKEND_BASE ""
#define XHS_DEVICE_TOKEN ""
