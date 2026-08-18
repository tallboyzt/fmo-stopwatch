# M5 项目音频输出不流畅问题分析

对比参考代码 `audio_ws.c` / `audio_output.c` 与 M5 项目 `fwmo_audio.h` / `fwmo_client.h` / `fwmo_main.ino` 后，发现以下关键问题。

---

## 问题 1（致命）：没有独立的音频播放任务

### 参考代码做法
`audio_ws.c` 创建了独立的 FreeRTOS 任务 `net_audio_play_task`（第 2218 行），该任务：
- 以固定速率从 ring buffer 读取 `PLAY_CHUNK_SAMPLES` 个采样
- 调用 `audio_output_write_pcm()` → `i2s_write()` **阻塞写入** DMA
- `i2s_write()` 本身就是天然节拍器——DMA 消费数据的速度就是播放速度
- **完全不受主循环 UI 渲染、WiFi 轮询等干扰**

### M5 项目做法
`fwmo_audio.h` 的 `update()` 由主 `loop()` 调用（`fwmo_main.ino` 第 151 行）。主 loop 中还要执行：
- WiFi 轮询
- WebSocket 数据处理（handleEvents / handleStation / handleAudio / handleRequests）
- **UI 渲染（每 200ms 一次，466×466 AMOLED 绘制耗时可达数十毫秒）**
- 触摸处理
- 按键处理
- 电池/蓝牙/RSSI 监测

UI 渲染期间 `audio.update()` 根本不会被调用，导致音频输出出现明显间隙。

### 修复方向
创建独立的 FreeRTOS 音频播放任务，在 Core 0 上运行，使用阻塞式 I2S 写入：
```cpp
void audioPlayTask(void* arg) {
    while (true) {
        if (ringCount() >= BLOCK) {
            // 从 ring buffer 读取 BLOCK 个采样
            // 用 M5.Speaker 的底层 I2S 阻塞写入
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}
```

---

## 问题 2（致命）：M5.Speaker.playRaw() 非阻塞 + 4 通道轮转方案

### 问题描述
`fwmo_audio.h` 第 108-145 行使用 4 个 DMA 通道轮转：
```cpp
M5.Speaker.playRaw(playBuf[idx], BLOCK, 8000, false, 1, idx);
```
`playRaw()` 是**非阻塞**的——它只是把数据排入队列。代码在 `update()` 中检查 `M5.Speaker.isPlaying(i)` 来判断通道是否空闲，但 `update()` 的调用频率取决于主 loop 速度。

当主 loop 被 UI 渲染或其他操作阻塞时：
1. 4 个通道可能全部播完，但新数据还没到来 → **静音间隙**
2. 或者通道还没播完就尝试塞入新数据 → 行为不确定

### 参考代码做法
参考代码用 `i2s_write()` 阻塞写入，DMA 缓冲区 `dma_buf_count=4, dma_buf_len=PLAY_CHUNK_SAMPLES * UPSAMPLE * 2`。`i2s_write()` 会在 DMA 缓冲满时阻塞，自然产生正确节拍，不会出现间隙。

### 修复方向
改用 M5.Speaker 的底层 I2S 接口进行阻塞写入，或直接操作 ES8311 的 I2S driver。

---

## 问题 3（严重）：每块边界淡入淡出导致周期性幅度调制

### 问题描述
`fwmo_audio.h` 第 136-141 行：
```cpp
const int FADE = 8;
for (int k = 0; k < FADE; k++) {
    int g = (k * 32768) / FADE;
    playBuf[idx][k]            = (int16_t)(((int)playBuf[idx][k]            * g) >> 15);
    playBuf[idx][BLOCK - 1 - k] = (int16_t)(((int)playBuf[idx][BLOCK - 1 - k] * g) >> 15);
}
```

每个 256 样本块（32ms @8kHz）的首尾各 8 个样本做淡入淡出。这意味着：
- 每 32ms 音量从 0 升到满再降到 0
- **产生约 31Hz 的周期性幅度调制（颤音/颤抖效果）**
- 人耳对 20-100Hz 的幅度调制非常敏感

参考代码**完全没有**这种逐块淡入淡出——它依赖 I2S DMA 的连续输出，块之间是无缝衔接的。

### 修复方向
删除淡入淡出代码。改用阻塞式 I2S 写入后，数据是连续流入 DMA 的，不需要任何块边界处理。

---

## 问题 4（严重）：启动阈值定义了但从未使用

### 问题描述
`fwmo_config.h` 第 44 行定义了：
```c
#define AUDIO_START_SAMPLES 800  // 缓冲达到后开始播放 (100ms)
```

但 `fwmo_audio.h` 的 `update()` 中**完全没有引用这个常量**。只要有 256 个样本就开始播放，导致：
- 缓冲不够深，容易立即欠载
- 播放一开始就不稳定

### 参考代码做法
`audio_ws.c` 第 2262-2277 行：
```c
if (!started) {
    if (avail < START_BUFFER_SAMPLES) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
    }
    started = true;
    audio_amp_enable(true);  // 缓冲足够后才开功放
}
```
等缓冲积累到 `START_BUFFER_SAMPLES` 后才开始播放并打开功放。

### 修复方向
在播放逻辑中加入启动阈值判断，缓冲不足时等待，不急于播放。

---

## 问题 5（中等）：无自适应缓冲管理

### 参考代码做法
`audio_ws.c` 有完整的自适应缓冲系统（第 837-887 行）：
- 追踪欠载次数（`s_underflow_in_window`）
- 欠载时自动增大目标缓冲（`TARGET_BUFFER_INC_STEP`）
- 稳定 5 秒后逐步减小缓冲（`TARGET_BUFFER_DEC_STEP`）
- 延迟过高时主动丢老数据（第 1735-1773 行）

### M5 项目做法
`fwmo_audio.h` 没有任何自适应机制。ring buffer 满了就静默覆盖旧数据（第 87 行），不追踪丢弃量，不调整目标缓冲。

### 修复方向
增加欠载计数和目标缓冲动态调整逻辑。

---

## 问题 6（中等）：ring buffer 无锁保护 + 静默覆盖

### 问题描述
`fwmo_audio.h` 的 `pushPCM()` 第 84-88 行：
```cpp
for (int i = 0; i < n; i++) {
    _ring[_w] = data[i];
    _w = (_w + 1) % AUDIO_RING_SAMPLES;
    if (_w == _r) _r = (_r + 1) % AUDIO_RING_SAMPLES;  // 静默覆盖
}
```

当前 `pushPCM()` 和 `update()` 都在主 loop 中调用，暂时是单线程。但如果将来改为独立播放任务（应该改），这里就**必须加锁**。

参考代码用 `portENTER_CRITICAL` / `portEXIT_CRITICAL` 保护所有 ring buffer 操作。

另外，静默覆盖旧数据时不追踪丢弃量，无法做延迟控制。

---

## 问题 7（中等）：配置了升采样但从未实现

### 问题描述
`fwmo_config.h`：
```c
#define FMO_AUDIO_IN_RATE   8000   // 输入 8kHz
#define FMO_AUDIO_OUT_RATE  16000  // 输出 16kHz
#define FMO_UPSAMPLE_FACTOR 2      // 升采样因子
```

但 `fwmo_audio.h` 中：
```cpp
cfg.sample_rate = 8000;  // M5.Speaker 配置为 8kHz
M5.Speaker.playRaw(playBuf[idx], BLOCK, 8000, false, 1, idx);  // 8kHz 直出
```

**升采样完全没有实现**。8kHz 直出在 ES8311 上音质较差。参考代码有完整的重复采样/线性插值升采样（第 1031-1092 行）。

### 修复方向
要么配置 M5.Speaker 为 16000Hz 并在写入前做 2x 升采样，要么保持 8kHz 但确认 ES8311 在 8kHz 下的表现可接受。

---

## 问题 8（中等）：wsRead() 阻塞主循环最多 50ms

### 问题描述
`fwmo_client.h` 第 277 行：
```cpp
uint32_t t=millis(); while(c.available()<(int)len&&millis()-t<50) { delay(1); yield(); }
```

`wsRead()` 在等待 WebSocket 帧数据完整时最多阻塞 50ms。这段时间内主 loop 完全停滞，`audio.update()` 不会被调用。

### 参考代码做法
参考代码使用 `esp_websocket_client` 库，WebSocket 数据接收在独立的库内部线程中回调，不阻塞任何任务。

---

## 问题 9（轻微）：handleAudio() 每轮限制 16 帧

### 问题描述
`fwmo_client.h` 第 376 行：
```cpp
while (_audio_c.available() && totalFrames < 16) {
```

如果网络一次涌入大量数据，最多只读 16 帧就返回。剩余数据在 TCP 缓冲中积压，增加延迟。下一轮 loop 才继续读取，但中间又隔了 UI 渲染等操作。

---

## 问题 10（轻微）：vTaskDelay(5ms) 增加音频更新延迟

### 问题描述
`fwmo_main.ino` 第 284 行：
```cpp
vTaskDelay(pdMS_TO_TICKS(5));
```

主 loop 末尾固定延迟 5ms。加上 loop 内各操作耗时，`audio.update()` 的实际调用间隔可能远大于 5ms 且不稳定。256 样本 @8kHz = 32ms，如果 update 间隔不稳定，就会出现有时来不及填充新块的情况。

---

## 总结：核心问题优先级

| 优先级 | 问题 | 影响 |
|--------|------|------|
| P0 | 无独立音频播放任务 | UI 渲染时音频中断 |
| P0 | playRaw 非阻塞 + 4 通道轮转 | 通道播完无新数据 → 静音间隙 |
| P0 | 块边界淡入淡出 | 31Hz 幅度调制 → 颤抖/爆音 |
| P1 | 启动阈值未使用 | 播放初始不稳定 |
| P1 | 无自适应缓冲管理 | 网络抖动时无法自动调整 |
| P2 | ring buffer 无锁 | 改为多线程后必须修复 |
| P2 | 升采样未实现 | 8kHz 直出音质差 |
| P2 | wsRead 阻塞 50ms | 加剧 update 间隔不稳定 |
| P3 | 16 帧限制 | 大数据涌入时延迟增加 |
| P3 | vTaskDelay(5ms) | update 间隔不稳定 |
