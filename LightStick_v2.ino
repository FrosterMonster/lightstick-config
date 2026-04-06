#include <FastLED.h>
#include <EEPROM.h>

// ========== 硬體腳位 ==========
#define LED_PIN_1     9
#define LED_PIN_2     7
#define LED_PIN_3     8
#define NUM_LEDS      21
#define NUM_STRIPS    3
#define BTN_A_PIN     10   // 按鈕A（Mode1: 下一個效果）
#define BTN_B_PIN     11   // 按鈕B（Mode1: 上一個效果）
#define SW_PIN        2    // 指撥開關（LOW=Mode1 按鈕模式, HIGH=Mode2 計時模式）

// ========== EEPROM 配置 ==========
#define MAGIC_BYTE    0xA5
#define MAX_STEPS     40
#define ADDR_MAGIC    0
#define ADDR_M1_CNT   1
#define ADDR_M2_CNT   2
#define ADDR_M1       3                          // 每筆 5 bytes
#define ADDR_M2       (ADDR_M1 + MAX_STEPS * 5)  // 每筆 7 bytes = addr 78

// ========== 資料結構 ==========
struct Step {
  uint8_t type;   // 0=常亮 1=波浪 2=對稱噴發 3=呼吸 4=三面色差 5=閃爍
  uint8_t hue;    // 0-255 色相
  uint8_t sat;    // 0-255 飽和度（0=白光）
  uint8_t bri;    // 0-255 亮度
  uint8_t spd;    // 0-255 速度參數
  uint32_t dur;   // 秒數（僅 Mode2）
};

// ========== 全域變數 ==========
CRGB leds[NUM_STRIPS][NUM_LEDS];
Step m1[MAX_STEPS], m2[MAX_STEPS];
uint8_t m1Cnt = 0, m2Cnt = 0;
int curStep = 0;
unsigned long stepStart = 0;
bool prevSw = HIGH;
unsigned long lastBtnA = 0, lastBtnB = 0;
const int DEBOUNCE = 300;
bool prevBtnA = HIGH;
bool prevBtnB = HIGH;
bool isRunning = false; // 標記是否解除待機並正在運行

// Serial 解析
char sBuf[60];
int sLen = 0;
enum { S_IDLE, S_M1, S_M2 } sState = S_IDLE;
int sExpect = 0, sGot = 0;

// 預覽模式
bool previewOn = false;
Step previewStep;

// 動畫計數器
uint8_t anim = 0;

// ========== Setup ==========
void setup() {
  Serial.begin(9600);
  FastLED.addLeds<WS2812B, LED_PIN_1, GRB>(leds[0], NUM_LEDS);
  FastLED.addLeds<WS2812B, LED_PIN_2, GRB>(leds[1], NUM_LEDS);
  FastLED.addLeds<WS2812B, LED_PIN_3, GRB>(leds[2], NUM_LEDS);
  pinMode(BTN_A_PIN, INPUT_PULLUP);
  pinMode(BTN_B_PIN, INPUT_PULLUP);
  pinMode(SW_PIN, INPUT_PULLUP);
  FastLED.setBrightness(255);
  loadConfig();
  allOff();
  FastLED.show();
  Serial.println("LS2:READY");
}

// ========== Main Loop ==========
void loop() {
  readSerial();
  EVERY_N_MILLISECONDS(30) { anim++; }

  // 預覽模式（網頁端觸發）
  if (previewOn) {
    renderEffect(previewStep);
    FastLED.show();
    return;
  }

  // 讀取指撥開關
  bool sw = digitalRead(SW_PIN);

  // 偵測開關切換 → 重置並進入待機狀態
  if (sw != prevSw) {
    prevSw = sw;
    curStep = 0;
    isRunning = false; // 切換開關一律回到待機
    allOff();
    FastLED.show();
    delay(200);
    return;
  }

  // 統一讀取按鈕並做防彈跳
  unsigned long now = millis();
  bool curBtnA = digitalRead(BTN_A_PIN);
  bool curBtnB = digitalRead(BTN_B_PIN);
  bool anyBtnPressed = false;

  // 按鈕A偵測 (邊緣觸發)
  if (curBtnA == LOW && prevBtnA == HIGH && now - lastBtnA > DEBOUNCE) {
    anyBtnPressed = true;
    lastBtnA = now;
  }
  prevBtnA = curBtnA;

  // 按鈕B偵測 (邊緣觸發)
  if (curBtnB == LOW && prevBtnB == HIGH && now - lastBtnB > DEBOUNCE) {
    anyBtnPressed = true;
    lastBtnB = now;
  }
  prevBtnB = curBtnB;

  // 待機狀態判斷
  if (!isRunning) {
    if (anyBtnPressed) {
      isRunning = true;      // 偵測到按鈕，解除待機
      stepStart = millis();  // 重置 Mode 2 的計時器
      curStep = 0;           // 確保從第一個效果(第0步)開始
      anyBtnPressed = false; // 消耗掉這次按鍵事件，避免喚醒後立刻跳過第一個效果
    } else {
      return; // 繼續待機，不往下執行渲染
    }
  }

  // 若已在運行狀態，根據開關決定模式
  if (sw == HIGH) {
    handleMode1(anyBtnPressed);  // Mode 1: 把按鈕觸發狀態傳入
  } else {
    handleMode2();               // Mode 2: 自動計時
  }

  FastLED.show();
}

// ========== Mode1: 按鈕切換效果 ==========
void handleMode1(bool pressed) {
  if (m1Cnt == 0) { allOff(); return; }

  // 如果有按鈕按下
  if (pressed) {
    if (curStep >= m1Cnt - 1) {
      // 如果已經是最後一個效果，關閉燈光並進入待機
      isRunning = false;
      allOff();
      return; // 提早結束，不往下渲染
    } else {
      // 否則，切換到下一個效果
      curStep++;
    }
  }

  // 正常渲染當前效果
  renderEffect(m1[curStep]);
}

// ========== Mode2: 自動計時切換 ==========
void handleMode2() {
  if (m2Cnt == 0) { allOff(); return; }

  unsigned long now = millis();
  unsigned long elapsed = now - stepStart; // <--- 移除 / 1000，直接使用毫秒計算

  // 如果持續時間大於0，且已經達到設定的毫秒數
  if (m2[curStep].dur > 0 && elapsed >= m2[curStep].dur) {
    if (curStep >= m2Cnt - 1) {
      isRunning = false;
      allOff();
      return; 
    } else {
      curStep++;
      stepStart = now;
    }
  }
  renderEffect(m2[curStep]);
}

// ========== 效果渲染引擎 ==========
void renderEffect(Step &s) {
  uint8_t h = s.hue;
  uint8_t sa = s.sat;
  uint8_t b = s.bri;
  uint8_t sp = max((uint8_t)1, s.spd);
  uint8_t phase = anim * ((sp >> 5) + 1);

  switch (s.type) {

    case 0: // 常亮
      for (int i = 0; i < NUM_STRIPS; i++)
        fill_solid(leds[i], NUM_LEDS, CHSV(h, sa, b));
      break;

    case 1: // 波浪
      for (int i = 0; i < NUM_STRIPS; i++)
        for (int j = 0; j < NUM_LEDS; j++) {
          uint8_t wh = h + sin8(phase + j * 12) / 12;
          leds[i][j] = CHSV(wh, sa, b);
        }
      break;

    case 2: // 對稱噴發
      allOff();
      {
        uint8_t bpm = max((uint8_t)(sp / 4), (uint8_t)10);
        int pos = beatsin8(bpm, 0, NUM_LEDS - 1);
        for (int i = 0; i < NUM_STRIPS; i++) {
          leds[i][pos] = CHSV(h, sa, b);
          leds[i][NUM_LEDS - 1 - pos] = CHSV(h, sa, b);
        }
      }
      break;

    case 3: // 中心呼吸
      {
        uint8_t bpm2 = max((uint8_t)(sp / 8), (uint8_t)5);
        for (int i = 0; i < NUM_STRIPS; i++)
          for (int j = 0; j < NUM_LEDS; j++) {
            int dist = abs(j - NUM_LEDS / 2);
            uint8_t v = scale8(beatsin8(bpm2, 50, 255, 0, dist * 15), b);
            leds[i][j] = CHSV(h, sa, v);
          }
      }
      break;

    case 4: // 三面色差
      {
        uint8_t spread = max((uint8_t)(sp / 8), (uint8_t)1);
        for (int i = 0; i < NUM_STRIPS; i++) {
          uint8_t sh = h + (int8_t)(i - 1) * spread;
          fill_solid(leds[i], NUM_LEDS, CHSV(sh, sa, b));
        }
      }
      break;

    case 5: // 隨機閃爍
      {
        uint8_t fade = max((uint8_t)(sp / 4), (uint8_t)10);
        for (int i = 0; i < NUM_STRIPS; i++)
          fadeToBlackBy(leds[i], NUM_LEDS, fade);
        if (random8() > (uint8_t)(255 - sp / 2))
          leds[random8(NUM_STRIPS)][random8(NUM_LEDS)] = CHSV(h + random8(20), sa, b);
      }
      break;
  }
}

void allOff() {
  for (int i = 0; i < NUM_STRIPS; i++)
    fill_solid(leds[i], NUM_LEDS, CRGB::Black);
}

// ========== Serial 通訊 ==========
void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (sLen > 0) {
        sBuf[sLen] = '\0';
        processCmd(sBuf);
        sLen = 0;
      }
    } else if (sLen < 79) {
      sBuf[sLen++] = c;
    }
  }
}

void processCmd(char *cmd) {
  // PING → PONG
  if (strcmp(cmd, "PING") == 0) {
    Serial.println("PONG");
    return;
  }

  // READ → 回傳設定
  if (strcmp(cmd, "READ") == 0) {
    sendConfig();
    return;
  }

  // SAVE → 存入 EEPROM
  if (strcmp(cmd, "SAVE") == 0) {
    saveConfig();
    sState = S_IDLE;
    Serial.println("OK:SAVED");
    return;
  }

  // STOP → 結束預覽
  if (strcmp(cmd, "STOP") == 0) {
    previewOn = false;
    isRunning = false; // 結束預覽後，回到全暗待機狀態
    allOff();
    FastLED.show();
    Serial.println("OK:STOP");
    return;
  }

  // TEST:type,hue,sat,bri,spd → 即時預覽
  if (strncmp(cmd, "TEST:", 5) == 0) {
    int t, h, s, b, sp;
    if (sscanf(cmd + 5, "%d,%d,%d,%d,%d", &t, &h, &s, &b, &sp) == 5) {
      previewStep.type = (uint8_t)t;
      previewStep.hue = (uint8_t)h;
      previewStep.sat = (uint8_t)s;
      previewStep.bri = (uint8_t)b;
      previewStep.spd = (uint8_t)sp;
      previewStep.dur = 0;
      previewOn = true;
      Serial.println("OK:TEST");
    } else {
      Serial.println("ERR:FMT");
    }
    return;
  }

  // M1:N → 開始接收 Mode1 設定
  if (strncmp(cmd, "M1:", 3) == 0) {
    sExpect = atoi(cmd + 3);
    if (sExpect > MAX_STEPS) sExpect = MAX_STEPS;
    m1Cnt = (uint8_t)sExpect;
    sGot = 0;
    sState = S_M1;
    Serial.print("OK:M1:"); Serial.println(sExpect);
    return;
  }

  // M2:N → 開始接收 Mode2 設定
  if (strncmp(cmd, "M2:", 3) == 0) {
    sExpect = atoi(cmd + 3);
    if (sExpect > MAX_STEPS) sExpect = MAX_STEPS;
    m2Cnt = (uint8_t)sExpect;
    sGot = 0;
    sState = S_M2;
    Serial.print("OK:M2:"); Serial.println(sExpect);
    return;
  }

  // E:type,hue,sat,bri,spd[,dur] → 效果資料
  if (cmd[0] == 'E' && cmd[1] == ':') {
    if (sState == S_M1 && sGot < sExpect) {
      int t, h, s, b, sp;
      if (sscanf(cmd + 2, "%d,%d,%d,%d,%d", &t, &h, &s, &b, &sp) == 5) {
        m1[sGot].type = (uint8_t)t;
        m1[sGot].hue = (uint8_t)h;
        m1[sGot].sat = (uint8_t)s;
        m1[sGot].bri = (uint8_t)b;
        m1[sGot].spd = (uint8_t)sp;
        m1[sGot].dur = 0;
        sGot++;
        Serial.print("OK:E:"); Serial.println(sGot);
      } else {
        Serial.println("ERR:FMT");
      }
    } else if (sState == S_M2 && sGot < sExpect) {
      int t, h, s, b, sp;
      long d; // 接毫秒
      if (sscanf(cmd + 2, "%d,%d,%d,%d,%d,%ld", &t, &h, &s, &b, &sp, &d) == 6) {
        m2[sGot].type = (uint8_t)t;
        m2[sGot].hue = (uint8_t)h;
        m2[sGot].sat = (uint8_t)s;
        m2[sGot].bri = (uint8_t)b;
        m2[sGot].spd = (uint8_t)sp;
        m2[sGot].dur = (uint32_t)d; // 存入毫秒
        sGot++;
        Serial.print("OK:E:"); Serial.println(sGot);
      } else {
        Serial.println("ERR:FMT");
      }
    } else {
      Serial.println("ERR:STATE");
    }
    return;
  }

  Serial.println("ERR:UNKNOWN");
}

// 回傳目前設定給網頁端
void sendConfig() {
  Serial.print("CFG:M1:"); Serial.println(m1Cnt);
  for (int i = 0; i < m1Cnt; i++) {
    Serial.print("E:");
    Serial.print(m1[i].type); Serial.print(",");
    Serial.print(m1[i].hue);  Serial.print(",");
    Serial.print(m1[i].sat);  Serial.print(",");
    Serial.print(m1[i].bri);  Serial.print(",");
    Serial.println(m1[i].spd);
  }
  Serial.print("CFG:M2:"); Serial.println(m2Cnt);
  for (int i = 0; i < m2Cnt; i++) {
    Serial.print("E:");
    Serial.print(m2[i].type); Serial.print(",");
    Serial.print(m2[i].hue);  Serial.print(",");
    Serial.print(m2[i].sat);  Serial.print(",");
    Serial.print(m2[i].bri);  Serial.print(",");
    Serial.print(m2[i].spd);  Serial.print(",");
    Serial.println(m2[i].dur);
  }
  Serial.println("CFG:END");
}

// ========== EEPROM 讀寫 ==========
void saveConfig() {
  EEPROM.update(ADDR_MAGIC, MAGIC_BYTE);
  EEPROM.update(ADDR_M1_CNT, m1Cnt);
  EEPROM.update(ADDR_M2_CNT, m2Cnt);

  int addr = ADDR_M1;
  for (int i = 0; i < m1Cnt; i++) {
    EEPROM.update(addr++, m1[i].type);
    EEPROM.update(addr++, m1[i].hue);
    EEPROM.update(addr++, m1[i].sat);
    EEPROM.update(addr++, m1[i].bri);
    EEPROM.update(addr++, m1[i].spd);
  }

  addr = ADDR_M2;
  for (int i = 0; i < m2Cnt; i++) {
    EEPROM.update(addr++, m2[i].type);
    EEPROM.update(addr++, m2[i].hue);
    EEPROM.update(addr++, m2[i].sat);
    EEPROM.update(addr++, m2[i].bri);
    EEPROM.update(addr++, m2[i].spd);
    // 將 32 位元整數(毫秒)拆成 4 個位元組儲存
    EEPROM.update(addr++, (uint8_t)(m2[i].dur >> 24));
    EEPROM.update(addr++, (uint8_t)(m2[i].dur >> 16));
    EEPROM.update(addr++, (uint8_t)(m2[i].dur >> 8));
    EEPROM.update(addr++, (uint8_t)(m2[i].dur & 0xFF));
  }
}

void loadConfig() {
  if (EEPROM.read(ADDR_MAGIC) != MAGIC_BYTE) {
    m1Cnt = 0;
    m2Cnt = 0;
    return;
  }

  m1Cnt = min((uint8_t)EEPROM.read(ADDR_M1_CNT), (uint8_t)MAX_STEPS);
  m2Cnt = min((uint8_t)EEPROM.read(ADDR_M2_CNT), (uint8_t)MAX_STEPS);

  int addr = ADDR_M1;
  for (int i = 0; i < m1Cnt; i++) {
    m1[i].type = EEPROM.read(addr++);
    m1[i].hue  = EEPROM.read(addr++);
    m1[i].sat  = EEPROM.read(addr++);
    m1[i].bri  = EEPROM.read(addr++);
    m1[i].spd  = EEPROM.read(addr++);
    m1[i].dur  = 0;
  }

  addr = ADDR_M2;
  for (int i = 0; i < m2Cnt; i++) {
    m2[i].type = EEPROM.read(addr++);
    m2[i].hue  = EEPROM.read(addr++);
    m2[i].sat  = EEPROM.read(addr++);
    m2[i].bri  = EEPROM.read(addr++);
    m2[i].spd  = EEPROM.read(addr++);
    // 將 4 個位元組合併回 32 位元整數(毫秒)
    m2[i].dur  = ((uint32_t)EEPROM.read(addr++) << 24) |
                 ((uint32_t)EEPROM.read(addr++) << 16) |
                 ((uint32_t)EEPROM.read(addr++) << 8) |
                  (uint32_t)EEPROM.read(addr++);
  }
}
