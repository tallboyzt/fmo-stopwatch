// fwmo_audio.h — 音频输出管理
// 单通道阻塞playRaw + PSRAM ring buffer + 独立FreeRTOS播放任务
// playRaw阻塞形成32ms自然节拍, 三重缓冲保护
#pragma once
#include "fwmo_config.h"
#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include "fwmo_audio_full.h"   // 全双工音频（TX 播放）

class FMO_Audio {
public:
    enum OutputMode { MODE_LOCAL=0, MODE_BT=1, MODE_AUTO=2 };

    // 绑定全双工音频（播放 TX 用，由主程序注入）
    void setAudioFull(FMO_AudioFull* a) { _audioFull = a; }

    bool begin() {
        // 全双工方案：I2S0 由 FMO_AudioFull 统一管理（TX 播放 + RX 采集）
        // 这里只准备 ring buffer
        _ready = true;

        if (!_ring) {
            _ring = (int16_t*)heap_caps_malloc(
                AUDIO_RING_SAMPLES * sizeof(int16_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!_ring)
                _ring = (int16_t*)heap_caps_malloc(
                    AUDIO_RING_SAMPLES * sizeof(int16_t), MALLOC_CAP_8BIT);
            if (_ring) {
                memset(_ring, 0, AUDIO_RING_SAMPLES * sizeof(int16_t));
                Serial.printf("[Audio] Ring: %dKB\n", (int)(AUDIO_RING_SAMPLES*2/1024));
            } else { _ready=false; return false; }
        }
        _r=_w=0; _mux=portMUX_INITIALIZER_UNLOCKED;
        return true;
    }

    void start() {
        if(!_ready) return;
        // 若旧播放任务仍在运行（stop 未等它退出），先等待其退出（限时100ms）
        // ⚠ 不调 eTaskGetState（任务自删后句柄失效会 assert）
        if (_task) {
            uint32_t t0 = millis();
            while (_task != nullptr && millis() - t0 < 100)
                vTaskDelay(1);
            _task = nullptr;
        }
        // 全双工方案：I2S0 由 AudioFull 统一管理，无需重建 Speaker
        _running=true;
        xTaskCreatePinnedToCore(playTask,"audio",3584,this,8,&_task,0);
    }
    void stop() {
        _running=false;
        // 不阻塞等待任务删除（任务检测到 _running=false 后自行退出）
        if(_task){
            _needsRebuild = false;   // 全双工无需重建
        }
    }
    void setMuted(bool m){ _muted=m; applyVolume(); }
    // ── 严格互斥：发送语音时停 playTask 并等待其彻底退出（不碰 Speaker，保持 MCLK） ──
    void stopTask() {
        _paused = true;       // ① 先置暂停：playTask 立即离开 playRaw 分支（最多 32ms 内）
        _running = false;     // ② 再停循环
        // ③ 等任务退出：playTask 退出时置 _task=nullptr（不调 eTaskGetState！
        //    任务自删后句柄失效，eTaskGetState(失效句柄) 会 assert 崩溃）
        if (_task) {
            uint32_t t0 = millis();
            while (_task != nullptr && millis() - t0 < 200)
                vTaskDelay(pdMS_TO_TICKS(5));
            Serial.printf("[Audio] playTask 退出耗时 %lums\n", (unsigned long)(millis() - t0));
            _task = nullptr;
        }
    }
    void startTask() {
        if (!_ready || _task) return;
        _paused = false;
        _running = true;
        xTaskCreatePinnedToCore(playTask, "audio", 3584, this, 8, &_task, 0);
    }
    void pause()  { stopTask(); }    // 发送前：停播放任务（Speaker 硬件保持 → MCLK 给 Mic）
    void resume() {
        // 方案二：Speaker 硬件从未停止（仅 PA 静音），无需 begin
        // 只需重启播放任务
        startTask();
    }
    bool isPaused() const { return _paused; }
    bool isMuted() const { return _muted; }
    bool isReady() const { return _ready; }
    void setVolume(uint8_t v) {
        if(v>100)v=100;
        _volume=v;
        applyVolume();   // 立即应用到扬声器（0-100 → 0-255）
    }
    // 应用音量：静音=0，否则 0-100 映射到播放缩放系数（playTask 使用 _volume/_muted）
    void applyVolume() {
        // 音量在 playTask 播放时实时读取 _volume/_muted 缩放，这里仅保留状态
        // （_muted=true → 静音；_volume 0-100 → 播放前乘以系数）
    }
    void setOutputMode(OutputMode m) { _outputMode=m; }
    void setBtConnected(bool c) { _btConnected=c; }
    bool btConnected() const { return _btConnected; }
    bool localActive() const {
        if(_outputMode==MODE_LOCAL) return true;
        if(_outputMode==MODE_BT) return false;
        return !_btConnected;
    }

    // 线程安全写入 (主loop调用)
    void pushPCM(const int16_t* data, int n) {
        if(!_ready||!_ring||!data||n<=0) return;
        _bytesIn += n*2;
        portENTER_CRITICAL(&_mux);
        for(int i=0;i<n;i++){
            _ring[_w]=data[i];
            _w=(_w+1)%AUDIO_RING_SAMPLES;
            if(_w==_r) _r=(_r+1)%AUDIO_RING_SAMPLES;
        }
        portEXIT_CRITICAL(&_mux);
    }

    void update() {}  // 保留兼容, 播放由独立任务负责
    void flush() { portENTER_CRITICAL(&_mux); _r=_w; portEXIT_CRITICAL(&_mux); }
    int bufferedSamples() const { return ringCount(); }

private:
    bool _ready=false,_muted=true,_running=false;
    bool _paused=false;         // 暂停播放（发送语音时置位）
    bool _needsRebuild=false;   // stop() 暂停后需要重建扬声器
    uint8_t _volume=60;
    OutputMode _outputMode=MODE_AUTO;
    bool _btConnected=false;
    int16_t* _ring=nullptr;
    volatile int _r=0,_w=0;
    uint32_t _bytesIn=0;
    TaskHandle_t _task=nullptr;
    FMO_AudioFull* _audioFull = nullptr;   // 全双工音频（TX 播放）
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

    int ringCount() const { int d=_w-_r; return d<0?d+AUDIO_RING_SAMPLES:d; }

    static void playTask(void* arg) {
        FMO_Audio* a=(FMO_Audio*)arg;
        static const int BLOCK=128;      // 8kHz 块（16ms，写入更快，避免 I2S DMA 满导致超时丢帧）
        int16_t pbuf[3][BLOCK];  // 三重缓冲
        int bi=0;

        while(a->_running){
            int idx=bi; bi=(bi+1)%3;
            int got=0;

            portENTER_CRITICAL(&a->_mux);
            while(got<BLOCK && a->_r!=a->_w){
                pbuf[idx][got++]=a->_ring[a->_r];
                a->_r=(a->_r+1)%AUDIO_RING_SAMPLES;
            }
            portEXIT_CRITICAL(&a->_mux);

            if(got<BLOCK){
                if(got>4) for(int i=0;i<4;i++){
                    int g=((4-i)*8192);
                    pbuf[idx][got-1-i]=(int16_t)(((int)pbuf[idx][got-1-i]*g)>>15);
                }
                memset(pbuf[idx]+got,0,(BLOCK-got)*sizeof(int16_t));
            }

            if(a->localActive() && !a->_paused && a->_audioFull)
            {
                // 8kHz 直出（无升采样，音质最保真）
                // I2S 为 STEREO 16bit：每帧 = L+R 两个 int16，L/R 写相同样本。
                // 音量：_volume(0-100) → 8bit 定点系数，100% 时 1.5x 数字放大（避免 2x+DAC+6dB 过载爆音，
                //       配合 ES8311 DAC +6dB 达到足够响度）；静音 _muted → 输出 0
                // 系数 = _volume/100 * 384（100% → 384 = 1.5x），带削波钳制
                uint32_t vol = a->_muted ? 0 : ((uint32_t)a->_volume * 384) / 100;
                static int16_t tx8[BLOCK*2];
                if (vol == 0) {
                    memset(tx8, 0, BLOCK*2*sizeof(int16_t));
                } else {
                    for (int i = 0; i < BLOCK; i++) {
                        int32_t s = (int32_t)pbuf[idx][i] * (int32_t)vol >> 8;
                        if (s > 32767) s = 32767;
                        else if (s < -32768) s = -32768;
                        tx8[i*2]   = (int16_t)s;
                        tx8[i*2+1] = (int16_t)s;
                    }
                }
                if (!a->_audioFull->writeTx(tx8, BLOCK*2)) {
                    // 诊断：打印丢帧（每 50 次一次，避免刷屏）
                    static int dropCnt = 0;
                    if ((++dropCnt % 50) == 1)
                        Serial.printf("[Audio] TX 丢帧! got=%d/%d\n", got, BLOCK);
                }
            }
            else
                vTaskDelay(pdMS_TO_TICKS(32));
        }
        a->_task=nullptr;
        vTaskDelete(NULL);
    }
};
