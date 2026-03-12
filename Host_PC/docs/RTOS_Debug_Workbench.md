# RTOS 调试工作台说明

## MCUXpresso IDE
1. 进入 Debug 会话后暂停程序。
2. 打开 `Window -> Show View -> Other...`。
3. 在 `FreeRTOS` 分类下打开 `Tasks / Queues / Semaphores`。
4. 若看不到任务，请确认：
   - 已进入 `vTaskStartScheduler()` 之后
   - `FreeRTOSConfig.h` 启用了任务统计相关宏（当前基础可用）

## VSCode (Cortex-Debug)
- 可查看线程列表（RTOS aware 依赖 gdb server 插件能力）。
- 若使用 J-Link，可在启动配置中添加 `rtos: FreeRTOS`（具体字段名取决于扩展版本）。
- 复杂对象（队列/信号量）可视化一般不如 MCUXpresso 完整。

## 现状建议
- 日常开发：VSCode 编码 + CMake 构建
- RTOS 任务诊断：优先 MCUXpresso 的 FreeRTOS 视图
