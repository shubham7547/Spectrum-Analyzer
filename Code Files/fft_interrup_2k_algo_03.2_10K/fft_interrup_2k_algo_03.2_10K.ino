#include <U8g2lib.h>
#include <SPI.h>
#include "fix_fft.h"
#include <avr/pgmspace.h>
#include <avr/interrupt.h>

#define SAMPLE_RATE   10000UL     // 10 kHz sampling
#define BUFFER_LEN    128
#define GROUP_SIZE     2
#define OLED_CS    10
#define OLED_DC     8
#define OLED_RESET  9

U8G2_SSD1306_128X64_NONAME_1_4W_HW_SPI u8g2(U8G2_R0, OLED_CS, OLED_DC, OLED_RESET);

char data[BUFFER_LEN];
char im[BUFFER_LEN];
int16_t magAvg[BUFFER_LEN / 2] = {0};
int16_t magFiltered[BUFFER_LEN / 2] = {0};

const uint8_t hannWindow[BUFFER_LEN] PROGMEM = {
  0,0,0,0,0,0,0,1,1,1,1,2,2,3,4,5,
  6,7,8,10,12,14,16,19,22,25,28,32,37,41,46,51,
  57,63,69,76,83,90,98,105,113,122,130,138,147,155,164,172,
  180,189,196,204,211,218,224,230,235,240,244,248,251,253,254,255,
  255,254,253,251,248,244,240,235,230,224,218,211,204,196,189,180,
  172,164,155,147,138,130,122,113,105,98,90,83,76,69,63,57,
  51,46,41,37,32,28,25,22,19,16,14,12,10,8,7,6,
  5,4,3,2,2,1,1,1,1,0,0,0,0,0,0,0
};

int fastLog10(int val) {
  int log10x100 = 0;
  while (val >= 10) { val /= 10; log10x100 += 100; }
  if (val >= 8) log10x100 += 90;
  else if (val >= 6) log10x100 += 78;
  else if (val >= 4) log10x100 += 60;
  else if (val >= 2) log10x100 += 30;
  return log10x100;
}

int median3(int a, int b, int c) {
  if ((a <= b && b <= c) || (c <= b && b <= a)) return b;
  if ((b <= a && a <= c) || (c <= a && a <= b)) return a;
  return c;
}

void setup() {
  u8g2.begin();
  u8g2.setBusClock(4000000);
  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(8, 32, "Sampler Ready");
  } while (u8g2.nextPage());

  // ADC setup — prescaler = 64, AVcc ref
  ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1);
  ADMUX  = (1 << REFS0) | 1;   // Channel A1
  DIDR0  = (1 << ADC1D);       // Disable digital input buffer on A1
}

void loop() {
  unsigned long t0 = micros();

  for (uint8_t i = 0; i < BUFFER_LEN; i++) {
    // --- ADC read using registers ---
    ADCSRA |= (1 << ADSC);
    while (ADCSRA & (1 << ADSC));  // Wait conversion
    uint16_t adcVal = ADC;

    int8_t sample = (adcVal >> 2) - 128;
    uint8_t w = pgm_read_byte(&hannWindow[i]);
    data[i] = ((int16_t)sample * w) / 255;
    im[i] = 0;

    // Maintain exact 10 kHz sampling
    while (micros() - t0 < (1000000UL / SAMPLE_RATE));
    t0 += (1000000UL / SAMPLE_RATE);
  }

  // Perform FFT
  fix_fft(data, im, 7, 0);

  const int REF_MAG = 16384;
  for (int i = 1; i < BUFFER_LEN / 2; i++) {
    int re = data[i];
    int imv = im[i];
    long mag2 = (long)re * re + (long)imv * imv;

    int dbVal;
    if (mag2 < 20) dbVal = -60;
    else {
      int dbX100 = 10 * fastLog10((int)mag2) - 10 * fastLog10(REF_MAG);
      dbVal = dbX100 / 100;
      if (dbVal < -60) dbVal = -60;
      if (dbVal > 0) dbVal = 0;
    }
    magAvg[i] = ((magAvg[i] * 3) + dbVal) >> 2;
  }

  // Apply 3-point median filter
  magFiltered[0] = magAvg[0];
  for (int k = 1; k < BUFFER_LEN / 2 - 1; k++) {
    magFiltered[k] = median3(magAvg[k - 1], magAvg[k], magAvg[k + 1]);
  }
  magFiltered[BUFFER_LEN / 2 - 1] = magAvg[BUFFER_LEN / 2 - 1];

  // Find peak
  int peakIndex = 1;
  int peakValue = -128;
  for (int k = 1; k < BUFFER_LEN / 2; k++) {
    if (magFiltered[k] > peakValue) {
      peakValue = magFiltered[k];
      peakIndex = k;
    }
  }

  // Display spectrum on OLED
  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_5x7_tr);

    // Y-axis labels
    for (int dB = -60; dB <= 0; dB += 20) {
      int yPos = map(dB, -60, 0, 50, 10);
      u8g2.drawLine(0, yPos, 2, yPos);
      char buf[6];
      sprintf(buf, "%d", dB);
      u8g2.drawStr(4, yPos + 3, buf);
    }

    // Bars
    for (int i = 1; i < BUFFER_LEN / 2; i += GROUP_SIZE) {
      long sumMag = 0;
      for (int j = 0; j < GROUP_SIZE; j++) {
        int idx = i + j;
        if (idx >= BUFFER_LEN / 2) break;
        sumMag += magFiltered[idx];
      }
      int avgDB = sumMag / GROUP_SIZE;
      int barHeight = map(avgDB, -60, 0, 0, 40);
      int x = map(i, 1, BUFFER_LEN / 2 - 1, 16, 127);
      int yTop = 50 - barHeight;
      u8g2.drawLine(x, 50, x, yTop);
    }

    // Peak line
    int xPeak = map(peakIndex, 1, BUFFER_LEN / 2 - 1, 16, 127);
    u8g2.drawLine(xPeak, 50, xPeak, 10);

    // X-axis labels (0–5 kHz)
    const char* freqLabels[] = {"0", "1k", "2k", "3k", "4k", "5k"};
    for (int j = 0; j < 6; j++) {
      int bin = (BUFFER_LEN / 2 - 1) * j / 5;
      int x = map(bin, 1, BUFFER_LEN / 2 - 1, 16, 127);
      u8g2.drawStr(x - 6, 62, freqLabels[j]);
    }

  } while (u8g2.nextPage());
}
