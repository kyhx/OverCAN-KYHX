#ifndef _INCLUDE_SIMPLE_GUI_CONFIG_H_
#define _INCLUDE_SIMPLE_GUI_CONFIG_H_

//=======================================================================//
//= Used for SimpleGUI virtual SDK.                                     =//
//=======================================================================//
#ifdef _SIMPLE_GUI_ENCODE_TEXT_
 #define _SIMPLE_GUI_ENCODE_TEXT_SRC_       ("UTF-8")
 #define _SIMPLE_GUI_ENCODE_TEXT_DEST_      ("GB2312")
#endif // _SIMPLE_GUI_ENCODE_TEXT_
// [移植] 虚拟 SDK 开关。
//   模拟器(PC/SDL2)   : 定义此宏（默认）
//   真实硬件(STM32等) : 不要定义此宏
//
// 默认开启以保持与上游仓库一致，可直接编译 SDL2 模拟器。
//
// 移植到硬件平台时**不要手工注释本行**，而是在工程的编译选项中加一个宏：
//     -DSGUI_USE_HARDWARE_PLATFORM
// 这样同一份源码既能编模拟器也能编固件，无需来回修改文件。
//
// 硬件平台还需要提供该平台的 RTC.h（GUI/src/SGUI_Interface.c 会 include）。
#if !defined(SGUI_USE_HARDWARE_PLATFORM)
#define _SIMPLE_GUI_IN_VIRTUAL_SDK_
#endif

//=======================================================================//
//= Used for SimpleGUI interface.                                       =//
//=======================================================================//
//#define _SIMPLE_GUI_ENABLE_DYNAMIC_MEMORY_

#endif // _INCLUDE_SIMPLE_GUI_CONFIG_H_
