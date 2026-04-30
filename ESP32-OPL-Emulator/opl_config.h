#ifndef OPL_CONFIG_H
#define OPL_CONFIG_H

// ============================================
// ESP32-S3 OPL Emulator Configuration
// m5stack-chipstream 참조 반영 버전
// ============================================

// ============================================
// 시리얼 통신 설정 (DOS PC ↔ ESP32)
// ============================================
#define OPL_SERIAL_BAUD     115200
#define OPL_SERIAL_RX_PIN   44      // D7 (GPIO44)
#define OPL_SERIAL_TX_PIN   43      // D6 (GPIO43)

// ============================================
// I2S 오디오 출력 설정 (PCM5102A)
// ============================================
#define I2S_BCK_PIN         5       // D4  (GPIO5)
#define I2S_LRCK_PIN        6       // D5  (GPIO6)
#define I2S_DATA_PIN        9       // D10 (GPIO9)

// ============================================
// OPL 에뮬레이션 설정
// ============================================
#define OPL_SAMPLE_RATE     44100

#define OPL_TYPE_OPL2       0
#define OPL_TYPE_OPL3       1
#define OPL_TYPE            OPL_TYPE_OPL3

// ============================================
// 프로토콜 설정
// ============================================
#define PROTOCOL_SIMPLE     0
#define PROTOCOL_NUKED      1
#define PROTOCOL_AUTO       2   // 자동 감지: 첫 바이트 패턴으로 판단
#define PROTOCOL_TYPE       PROTOCOL_NUKED  // adlcom ElectricRay: NUKED 3바이트 프로토콜 (OPL3)

// ============================================
// 버퍼 설정
// ============================================
#define AUDIO_BUFFER_SAMPLES    512
#define CMD_RING_BUFFER_SIZE    4096

// ============================================
// 디버그 설정
// ============================================
#define DEBUG_SERIAL            1       // 기본 메시지
#define DEBUG_SERIAL_RAW        0       // Raw 바이트 ★
#define DEBUG_PROTOCOL_PARSE    0       // 프로토콜 파싱 ★
#define DEBUG_OPL_EXECUTE       0       // OPL 명령 실행 ★
#define DEBUG_STATS_AUTO        0       // 자동 통계 (프로덕션: 끔)
#define DEBUG_STATS_INTERVAL    5000    // 5초마다

#endif
