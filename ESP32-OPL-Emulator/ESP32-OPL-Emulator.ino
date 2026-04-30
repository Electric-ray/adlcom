// ============================================
// ESP32-S3 OPL Emulator (adlcom 호환)
// 시리얼 수신 디버깅 강화 버전
// m5stack-chipstream 참조 반영
// ============================================

#include <Arduino.h>
#include <driver/i2s_std.h>
#include <driver/i2s_common.h>
#include "opl_config.h"

#include "ymfm/ymfm_opl.h"
#include "ymfm/ymfm_opl.cpp"

// ============================================
// YMFM 인터페이스
// ============================================
class ESP32OPLInterface : public ymfm::ymfm_interface {
public:
    virtual void ymfm_sync_mode_write(uint8_t data) override {}
    virtual void ymfm_sync_check_interrupts() override {}
    virtual void ymfm_set_timer(uint32_t tnum, int32_t duration) override {}
    virtual void ymfm_set_busy_end(uint32_t clocks) override {}
    virtual bool ymfm_is_busy() override { return false; }
    virtual uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override { return 0; }
    virtual void ymfm_external_write(ymfm::access_class type, uint32_t address, uint8_t data) override {}
};

// ============================================
// 전역 변수
// ============================================
static ESP32OPLInterface g_opl_interface;

#if OPL_TYPE == OPL_TYPE_OPL2
    static ymfm::ym3812* g_opl = nullptr;
    typedef ymfm::ym3812::output_data opl_output_t;
#else
    static ymfm::ymf262* g_opl = nullptr;
    typedef ymfm::ymf262::output_data opl_output_t;
#endif

struct OPLCommand {
    uint8_t bank;
    uint8_t reg;
    uint8_t val;
};

static volatile OPLCommand g_cmd_buffer[CMD_RING_BUFFER_SIZE];
static volatile uint16_t g_cmd_write_idx = 0;
static volatile uint16_t g_cmd_read_idx = 0;

// ============================================
// 프로토콜 모드 (런타임 전환 가능)
// ============================================
enum ProtocolMode { PROTO_UNKNOWN, PROTO_NUKED, PROTO_SIMPLE };
#if PROTOCOL_TYPE == PROTOCOL_NUKED
    static volatile ProtocolMode g_proto_mode = PROTO_NUKED;
#elif PROTOCOL_TYPE == PROTOCOL_SIMPLE
    static volatile ProtocolMode g_proto_mode = PROTO_SIMPLE;
#else
    static volatile ProtocolMode g_proto_mode = PROTO_UNKNOWN; // AUTO
#endif
static uint32_t g_proto_switches = 0; // 프로토콜 감지/전환 횟수

static int16_t g_audio_buffer[AUDIO_BUFFER_SAMPLES * 2];

static TaskHandle_t g_serial_task_handle = nullptr;
static TaskHandle_t g_audio_task_handle = nullptr;

// 통계 (volatile 제거)
static uint32_t g_commands_received = 0;
static uint32_t g_buffer_overflows = 0;
static uint32_t g_protocol_errors = 0;
static uint32_t g_i2s_write_errors = 0;
static uint32_t g_bytes_received = 0;
static uint32_t g_last_byte_time = 0;

static i2s_chan_handle_t tx_chan = nullptr;
static bool g_i2s_initialized = false;

// ============================================
// I2S 초기화 (새 API 사용)
// ============================================
void init_i2s() {
    Serial.println("\n[I2S] 초기화 중...");
    
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = OPL_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_BCK_PIN,
            .ws = (gpio_num_t)I2S_LRCK_PIN,
            .dout = (gpio_num_t)I2S_DATA_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    
    Serial.printf("[I2S] Sample Rate: %d Hz\n", OPL_SAMPLE_RATE);
    
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_chan, nullptr);
    if (err == ESP_OK) {
        err = i2s_channel_init_std_mode(tx_chan, &std_cfg);
        if (err == ESP_OK) {
            err = i2s_channel_enable(tx_chan);
            if (err == ESP_OK) {
                g_i2s_initialized = true;
                Serial.printf("[I2S] ✓ 초기화 완료\n");
                Serial.printf("[I2S]   BCK=%d, LRCK=%d, DATA=%d\n", 
                             I2S_BCK_PIN, I2S_LRCK_PIN, I2S_DATA_PIN);
            } else {
                Serial.printf("[I2S] ✗ 채널 활성화 실패 (err=%d)\n", err);
            }
        } else {
            Serial.printf("[I2S] ✗ 모드 설정 실패 (err=%d)\n", err);
        }
    } else {
        Serial.printf("[I2S] ✗ 채널 생성 실패 (err=%d)\n", err);
    }
    
    if (!g_i2s_initialized) {
        Serial.println("[I2S] ⚠ 오디오 출력 불가");
    }
}

// ============================================
// OPL 명령 큐
// ============================================
inline void add_opl_command(uint8_t bank, uint8_t reg, uint8_t val) {
    uint16_t next_idx = (g_cmd_write_idx + 1) % CMD_RING_BUFFER_SIZE;
    if (next_idx != g_cmd_read_idx) {
        g_cmd_buffer[g_cmd_write_idx].bank = bank;
        g_cmd_buffer[g_cmd_write_idx].reg = reg;
        g_cmd_buffer[g_cmd_write_idx].val = val;
        g_cmd_write_idx = next_idx;
        g_commands_received++;
    } else {
        g_buffer_overflows++;
        #if DEBUG_SERIAL
        Serial.println("[ERROR] 링버퍼 오버플로!");
        #endif
    }
}

inline void process_pending_commands() {
    while (g_cmd_read_idx != g_cmd_write_idx) {
        OPLCommand cmd;
        cmd.bank = g_cmd_buffer[g_cmd_read_idx].bank;
        cmd.reg = g_cmd_buffer[g_cmd_read_idx].reg;
        cmd.val = g_cmd_buffer[g_cmd_read_idx].val;
        
        #if DEBUG_OPL_EXECUTE
        Serial.printf("  [OPL] Bank%d Reg=0x%02X Val=0x%02X\n", 
                     cmd.bank, cmd.reg, cmd.val);
        #endif
        
        #if OPL_TYPE == OPL_TYPE_OPL2
            g_opl->write_address(cmd.reg);
            g_opl->write_data(cmd.val);
        #else
            if (cmd.bank == 0) {
                g_opl->write_address(cmd.reg);
                g_opl->write_data(cmd.val);
            } else {
                g_opl->write_address_hi(cmd.reg);
                g_opl->write_data(cmd.val);
            }
        #endif
        
        g_cmd_read_idx = (g_cmd_read_idx + 1) % CMD_RING_BUFFER_SIZE;
    }
}

void opl_hard_reset() {
    g_opl->reset();
    g_cmd_read_idx = 0;
    g_cmd_write_idx = 0;
    Serial.println("\n[RESET] OPL 칩 리셋!\n");
}

// ============================================
// 시리얼 수신 태스크 (NUKED / SIMPLE / AUTO 지원)
// ============================================
void serial_receive_task(void* parameter) {
    uint8_t state = 0;
    uint8_t cmd_byte = 0;
    uint8_t reg_byte = 0;

    Serial.println("\n\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550");
    Serial.println("  \uc2dc\ub9ac\uc5bc \uc218\uc2e0 \ud0dc\uc2a4\ud06c (NUKED \uace0\uc815)");
    Serial.println("\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550");
    Serial.printf("RX \ud540: GPIO%d\n", OPL_SERIAL_RX_PIN);
    Serial.printf("Baud: %d\n", OPL_SERIAL_BAUD);
    Serial.println("\ud504\ub85c\ud1a0\ucf5c: NUKED \uace0\uc815 (3\ubc14\uc774\ud2b8, OPL3)");
    Serial.println("\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\u2550\n");

    while (true) {
        while (Serial1.available()) {
            uint8_t byte = Serial1.read();
            g_bytes_received++;
            g_last_byte_time = millis();

            #if DEBUG_SERIAL_RAW
            Serial.printf("[RX] 0x%02X\n", byte);
            #endif

            // NUKED \ud504\ub85c\ud1a0\ucf5c \ud30c\uc11c (3\ubc14\uc774\ud2b8: CMD + REG + VAL)
            switch (state) {
                case 0: {
                    if (byte == 0x02) {
                        // \ub9ac\uc14b \uba85\ub839
                        #if DEBUG_PROTOCOL_PARSE
                        Serial.println("\u2192 RESET!");
                        #endif
                        opl_hard_reset();
                    } else if (byte & 0x80) {
                        // CMD \ubc14\uc774\ud2b8 (MSB=1)
                        cmd_byte = byte;
                        state = 1;
                    } else {
                        // \uc608\uc0c1\uce58 \ubabb\ud55c \ubc14\uc774\ud2b8 \u2014 \ud30c\uc11c \ub9ac\uc14b
                        g_protocol_errors++;
                        state = 0;
                        #if DEBUG_PROTOCOL_PARSE
                        Serial.printf("\u2192 ERROR! CMD MSB=0 (0x%02X), \ub9ac\uc14b\n", byte);
                        #endif
                    }
                    break;
                }
                case 1: {
                    reg_byte = byte;
                    state = 2;
                    break;
                }
                case 2: {
                    uint8_t bank    = (cmd_byte >> 2) & 0x03;
                    uint8_t addr_hi = cmd_byte & 0x03;
                    uint8_t addr    = (addr_hi << 6) | (reg_byte >> 1);
                    uint8_t val_hi  = (reg_byte & 0x01) << 7;
                    uint8_t val     = val_hi | byte;

                    if (bank <= 1) {
                        add_opl_command(bank, addr, val);
                        #if DEBUG_PROTOCOL_PARSE
                        Serial.printf("\u2192 NUKED Bank%d Reg=0x%02X Val=0x%02X \u2713\n", bank, addr, val);
                        #endif
                    } else {
                        g_protocol_errors++;
                    }
                    state = 0;
                    break;
                }
            }
        }
        vTaskDelay(1);
    }
}

// ============================================
// 오디오 생성 태스크
// ============================================
void audio_generate_task(void* parameter) {
    opl_output_t output;
    size_t bytes_written;
    
    Serial.println("[AUDIO] 태스크 시작\n");
    
    while (true) {
        for (int i = 0; i < AUDIO_BUFFER_SAMPLES; i++) {
            /* 샘플 생성 직전에 쌓인 명령 전부 처리.
             * 버퍼 앞에서 한 번만 처리하면 빠른 음악에서
             * 10ms 지연이 생겨 음 누락/타이밍 어긋남 발생. */
            process_pending_commands();
            g_opl->generate(&output);
            
            #if OPL_TYPE == OPL_TYPE_OPL2
                int16_t sample = (int16_t)((int32_t)output.data[0] / 2);
                g_audio_buffer[i * 2]     = sample;
                g_audio_buffer[i * 2 + 1] = sample;
            #else
                /* ymf262(OPL3): 4채널 합산 후 50% gain 적용
                 * 합산 시 int32로 오버플로 방지, 최종 50% = >> 1 */
                int32_t left  = (int32_t)output.data[0] + (int32_t)output.data[2];
                int32_t right = (int32_t)output.data[1] + (int32_t)output.data[3];
                g_audio_buffer[i * 2]     = (int16_t)constrain(left  >> 1, -32768, 32767);
                g_audio_buffer[i * 2 + 1] = (int16_t)constrain(right >> 1, -32768, 32767);
            #endif
        }
        
        if (g_i2s_initialized) {
            esp_err_t err = i2s_channel_write(tx_chan, g_audio_buffer, 
                      AUDIO_BUFFER_SAMPLES * 2 * sizeof(int16_t),
                      &bytes_written, portMAX_DELAY);
            if (err != ESP_OK) {
                g_i2s_write_errors++;
            }
        } else {
            vTaskDelay(10);
        }
    }
}

// ============================================
// Setup
// ============================================
void setup() {
    Serial.begin(115200);
    delay(1500);
    
    Serial.println("\n\n\n");
    Serial.println("════════════════════════════════════════");
    Serial.println("  ESP32-S3 OPL Emulator");
    Serial.println("  시리얼 수신 디버그 버전");
    Serial.println("  (m5stack-chipstream 참조)");
    Serial.println("════════════════════════════════════════");
    Serial.printf("빌드: %s %s\n", __DATE__, __TIME__);
    Serial.printf("OPL: OPL3 (YMF262, OPL2 호환)\n");
    Serial.printf("프로토콜: AUTO 감지 (NUKED/SIMPLE)\n\n");
    
    Serial.println("디버그 모드:");
    Serial.println("  ✓ Raw 바이트 로그");
    Serial.println("  ✓ 프로토콜 파싱 로그");
    Serial.println("  ✓ OPL 명령 실행 로그");
    Serial.println("  ✓ 자동 통계 (5초)\n");
    Serial.println("════════════════════════════════════════\n");
    
    Serial.println("[UART] Serial1 초기화...");
    Serial1.begin(OPL_SERIAL_BAUD, SERIAL_8N1, OPL_SERIAL_RX_PIN, OPL_SERIAL_TX_PIN);
    Serial.printf("[UART] RX=GPIO%d, Baud=%d\n", OPL_SERIAL_RX_PIN, OPL_SERIAL_BAUD);
    Serial.println("[UART] ✓ 준비 완료\n");
    
    Serial.println("[OPL] YMFM 초기화...");
    #if OPL_TYPE == OPL_TYPE_OPL2
        g_opl = new ymfm::ym3812(g_opl_interface);
    #else
        g_opl = new ymfm::ymf262(g_opl_interface);
    #endif
    g_opl->reset();
    Serial.println("[OPL] ✓ 준비 완료\n");
    
    init_i2s();
    
    Serial.println("\n[TASK] 태스크 생성...");
    xTaskCreatePinnedToCore(serial_receive_task, "SerialRX", 8192, NULL, 2, &g_serial_task_handle, 0);
    Serial.println("[TASK] ✓ SerialRX (Core 0)");
    
    xTaskCreatePinnedToCore(audio_generate_task, "AudioGen", 8192, NULL, 1, &g_audio_task_handle, 1);
    Serial.println("[TASK] ✓ AudioGen (Core 1)\n");
    
    Serial.println("════════════════════════════════════════");
    Serial.println("  준비 완료!");
    Serial.println("════════════════════════════════════════");
    Serial.println("\nDOS에서 실행:");
    Serial.println("  ADLCOM COM1 OPL3");
    Serial.println("  IMPLAY SONG.IMF");
    Serial.println("\nUSB 명령:");
    Serial.println("  stat   - 통계 보기");
    Serial.println("  clear  - 통계 초기화");
    Serial.println("════════════════════════════════════════\n");
}

// ============================================
// Loop
// ============================================
void loop() {
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        cmd.toLowerCase();
        
        if (cmd == "stat" || cmd == "status") {
            uint16_t pending = (g_cmd_write_idx >= g_cmd_read_idx) 
                ? (g_cmd_write_idx - g_cmd_read_idx)
                : (CMD_RING_BUFFER_SIZE - g_cmd_read_idx + g_cmd_write_idx);
            
            uint32_t idle_sec = (millis() - g_last_byte_time) / 1000;
            
            const char* proto_name = 
                (g_proto_mode == PROTO_NUKED)  ? "NUKED (3바이트, OPL3)" :
                (g_proto_mode == PROTO_SIMPLE) ? "SIMPLE (2바이트, OPL2)" :
                                                  "UNKNOWN (대기 중)";

            Serial.println("\n════════════════════════════════════════");
            Serial.println("  통계");
            Serial.println("════════════════════════════════════════");
            Serial.printf("프로토콜:        %s\n", proto_name);
            Serial.printf("프로토콜 전환:   %lu\n", g_proto_switches);
            Serial.printf("수신 바이트:     %lu\n", g_bytes_received);
            Serial.printf("OPL 명령:        %lu\n", g_commands_received);
            Serial.printf("대기 명령:       %d\n", pending);
            Serial.printf("버퍼 오버플로:   %lu\n", g_buffer_overflows);
            Serial.printf("프로토콜 에러:   %lu\n", g_protocol_errors);
            Serial.printf("I2S 에러:        %lu\n", g_i2s_write_errors);
            Serial.printf("유휴 시간:       %lu 초\n", idle_sec);
            Serial.printf("빈 힙:           %d bytes\n", ESP.getFreeHeap());
            Serial.println("════════════════════════════════════════\n");
        }
        else if (cmd == "clear") {
            g_bytes_received = 0;
            g_commands_received = 0;
            g_buffer_overflows = 0;
            g_protocol_errors = 0;
            g_i2s_write_errors = 0;
            g_proto_switches = 0;
            Serial.println("[CMD] 통계 초기화\n");
        }
        else if (cmd == "proto nuked" || cmd == "proto opl3") {
            g_proto_mode = PROTO_NUKED;
            Serial.println("[CMD] 프로토콜 → NUKED (OPL3)\n");
        }
        else if (cmd == "proto simple" || cmd == "proto opl2") {
            g_proto_mode = PROTO_SIMPLE;
            Serial.println("[CMD] 프로토콜 → SIMPLE (OPL2)\n");
        }
        else if (cmd == "proto auto") {
            g_proto_mode = PROTO_UNKNOWN;
            Serial.println("[CMD] 프로토콜 → AUTO 감지\n");
        }
        else if (cmd == "reset") {
            opl_hard_reset();
        }
        else if (cmd.length() > 0) {
            Serial.printf("명령: '%s'\n", cmd.c_str());
            Serial.println("  stat       - 통계");
            Serial.println("  clear      - 통계 초기화");
            Serial.println("  proto opl2 - SIMPLE 프로토콜 강제");
            Serial.println("  proto opl3 - NUKED 프로토콜 강제");
            Serial.println("  proto auto - 자동 감지");
            Serial.println("  reset      - OPL 리셋\n");
        }
    }
    
    #if DEBUG_STATS_AUTO
    static uint32_t last_stats = 0;
    if (millis() - last_stats > DEBUG_STATS_INTERVAL) {
        if (g_bytes_received > 0 || g_commands_received > 0) {
            uint16_t pending = (g_cmd_write_idx >= g_cmd_read_idx) 
                ? (g_cmd_write_idx - g_cmd_read_idx)
                : (CMD_RING_BUFFER_SIZE - g_cmd_read_idx + g_cmd_write_idx);
            
            Serial.println("\n[STAT] ════════════════════════════════");
            Serial.printf("       Bytes=%lu Cmds=%lu Pending=%d\n", 
                         g_bytes_received, g_commands_received, pending);
            if (g_protocol_errors > 0) {
                Serial.printf("       ⚠ 프로토콜 에러: %lu\n", g_protocol_errors);
            }
            if (g_buffer_overflows > 0) {
                Serial.printf("       ⚠ 버퍼 오버플로: %lu\n", g_buffer_overflows);
            }
            Serial.println("       ════════════════════════════════\n");
        }
        last_stats = millis();
    }
    #endif
    
    vTaskDelay(100 / portTICK_PERIOD_MS);
}
