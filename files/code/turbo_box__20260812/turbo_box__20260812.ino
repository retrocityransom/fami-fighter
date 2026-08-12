/*
  FAMI-FIGHTER Turbo Box
  by RetroCityRansom 
	https://github.com/retrocityransom
	www.youtube.com/@RetroCityRansom
  
  Features:
  - Separate turbofire toggle for buttons A and B via hotkey combination
  - Selectable autofire speed via hotkey combination
  - Optional Neopixel LED support to visualize, if Turbo is active and at which speed
  - Easy to extend - change LED colors, use an OLED instead of the LED, add / change turbofire frequencies as required

  Hotkey combos:
  - SELECT + UP: Increase turbofire speed
  - SELECT + DOWN: Decrease turbofire speed
  - SELECT + RIGHT: (De)Activate turbofire for button A
  - SELECT + LEFT: (De)Activate turbofire for button B
  
  Notes:
  If you want to use the Turbo Box with a Four Score, a 10ko pullup for console latch is a must.
  Otherwise it will only work with a second original NES controller plugged in.
*/
#include <avr/interrupt.h>
#include <Adafruit_NeoPixel.h>

const uint8_t PAD_CLOCK_PIN = 3;
const uint8_t PAD_LATCH_PIN = 4;
const uint8_t PAD_DATA_PIN  = 5;
const uint8_t CON_LATCH_PIN = 9;
const uint8_t CON_DATA_PIN  = 11;
const uint8_t CON_CLOCK_PIN = 12;
const uint8_t NEOPIXEL_PIN      = A3; // pin for Neopixel LED
const uint8_t NUM_NEOPIXEL_LEDS = 1;  // for later extension

#define SET_CON_DATA_HIGH()  DDRB &= ~(1 << DDB3) 
#define SET_CON_DATA_LOW()   { DDRB |= (1 << DDB3); PORTB &= ~(1 << PORTB3); }
#define BTN_A      0
#define BTN_B      1
#define BTN_SELECT 2
#define BTN_START  3
#define BTN_UP     4
#define BTN_DOWN   5
#define BTN_LEFT   6
#define BTN_RIGHT  7
#define SEND_BUTTON_BIT(bitMask) \
  while (PINB & (1 << PINB4)); \
  if (out & (bitMask)) { DDRB &= ~(1 << DDB3); } else { DDRB |= (1 << DDB3); } \
  while (!(PINB & (1 << PINB4)));

bool buttons_state[8];
// RGB + turbo stuff
struct RgbColor {uint8_t r, g, b;};
struct TurboLevel {
  uint8_t hz;         // turbo frequency
  RgbColor colorA;    // color for activated turbo A
  RgbColor colorB;    // color for activated turbo B
  RgbColor colorAB;   // color for activated turbo A + B
};
// Frequencies + respective RGB colors
const TurboLevel TURBO_LEVELS[] = {
  // Hz,  { Turbo A },        { Turbo B },          { Turbo A+B }
  {  4,   {255, 200, 0},     {70,  160, 245},      {255, 130, 205} }, // 4 Hz
  {  8,   {255, 140, 0},     {30,  110, 240},      {240, 80,  220} }, // 8 Hz
  { 10,   {255, 80,  0},     {0,   70,  230},      {220, 30,  235} }, // 10 Hz
  { 15,   {255, 30,  0},     {0,   30,  210},      {200, 0,   245} }, // 15 Hz
  { 20,   {255, 0,   0},     {0,   0,   180},      {160, 0,   255} }, // 20 Hz => red, deep blue, deep purple (hahaha)
  {  2,   {255, 255, 0},     {135, 206, 250},      {255, 182, 193} }  // 2 Hz  => yellow,  light blue, light pink
};

const uint8_t NUM_FREQ = sizeof(TURBO_LEVELS) / sizeof(TURBO_LEVELS[0]);
uint8_t p1_freqIdx = 0;
bool p1_turboA = false;
bool p1_turboB = false;
bool p1_lastUp    = false;
bool p1_lastDown  = false;
bool p1_lastLeft  = false;
bool p1_lastRight = false;
uint8_t frameCounter = 0;

static bool lastA = false;
static bool lastB = false;

volatile uint8_t p1OutBuffer = 0xFF;

Adafruit_NeoPixel strip(NUM_NEOPIXEL_LEDS, NEOPIXEL_PIN, NEO_RGB + NEO_KHZ800);

bool blockButtonInActiveFrame(uint8_t hz, uint8_t counter) {
  switch (hz) {
    case 20: return (counter != 0);
    case 15: return (counter >= 2);
    case 10: return (counter >= 3);
    case 8:  return (counter >= 3);
    case 4:  return (counter >= 8);
    case 2:  return (counter >= 15);
  }
  return false;
}

void readController() {
  PORTD |= (1 << PORTD4);
  delayMicroseconds(4);
  PORTD &= ~(1 << PORTD4);
  delayMicroseconds(2);

  for (uint8_t i = 0; i < 8; i++) {
    buttons_state[i] = ((PIND & (1 << PIND5)) != 0);

    PORTD |= (1 << PORTD3);
    delayMicroseconds(2);
    PORTD &= ~(1 << PORTD3);
    delayMicroseconds(2);
  }

  PORTD &= ~((1 << PORTD4) | (1 << PORTD3));
}

void updateLed() {
  uint32_t color;

  // no turbo -> LED green
  if (!p1_turboA && !p1_turboB) {
    color = strip.Color(0, 255, 0); 
  } 
  // only turbo A active
  else if (p1_turboA && !p1_turboB) {
    RgbColor c = TURBO_LEVELS[p1_freqIdx].colorA;
    color = strip.Color(c.r, c.g, c.b);
  } 
  // only turbo B active
  else if (!p1_turboA && p1_turboB) {
    RgbColor c = TURBO_LEVELS[p1_freqIdx].colorB;
    color = strip.Color(c.r, c.g, c.b);
  } 
  // turbo for both active
  else {
    RgbColor c = TURBO_LEVELS[p1_freqIdx].colorAB;
    color = strip.Color(c.r, c.g, c.b);
  }

  sei();
  strip.setPixelColor(0, color);
  strip.show();
  cli();
}

inline void exchangeWithConsole(uint8_t out) {
  if (out & (1 << BTN_A)) { SET_CON_DATA_HIGH(); } else { SET_CON_DATA_LOW(); }

  while (PINB & (1 << PINB1));

  SEND_BUTTON_BIT(1 << BTN_B);
  SEND_BUTTON_BIT(1 << BTN_SELECT);
  SEND_BUTTON_BIT(1 << BTN_START);
  SEND_BUTTON_BIT(1 << BTN_UP);
  SEND_BUTTON_BIT(1 << BTN_DOWN);
  SEND_BUTTON_BIT(1 << BTN_LEFT);
  SEND_BUTTON_BIT(1 << BTN_RIGHT);

  while (PINB & (1 << PINB4)); 
  SET_CON_DATA_HIGH();
}

void setup() {

  delay(100);

  pinMode(CON_DATA_PIN, INPUT);
  SET_CON_DATA_HIGH();

  // pinMode(CON_LATCH_PIN, INPUT_PULLUP); // NOT RECOMMENDED -> if no external 10k ohm pullup: use this instead
  pinMode(CON_LATCH_PIN, INPUT); // HIGHLY RECOMMENDED -> if external 10k ohm pullup: use this instead

  pinMode(CON_CLOCK_PIN, INPUT_PULLUP); // if no external 10k ohm pullup: use this
  // pinMode(CON_CLOCK_PIN, INPUT); // if external 10k ohm pullup: use this instead

  pinMode(PAD_LATCH_PIN, OUTPUT);
  pinMode(PAD_CLOCK_PIN, OUTPUT);

  pinMode(PAD_DATA_PIN, INPUT_PULLUP); // if no external 10k ohm pullup: use this
  // pinMode(PAD_DATA_PIN, INPUT); // if external 10k ohm pullup: use this instead

  strip.begin();
  strip.setBrightness(40);

  cli();
  updateLed();
}

void loop() {
  // wait for latch
  while (!(PINB & (1 << PINB1)));
  exchangeWithConsole(p1OutBuffer);
  // additional latch read for some special cases (Journey to Silius, Castlevania III, ...)
  for (uint16_t i = 0; i < 400; i++) {
    if (PINB & (1 << PINB1)) {
      exchangeWithConsole(p1OutBuffer);
      break; 
    }
  }
  readController();

  bool p1A     = !buttons_state[BTN_A];
  bool p1B     = !buttons_state[BTN_B];
  bool p1Sel   = !buttons_state[BTN_SELECT];
  bool p1Up    = !buttons_state[BTN_UP];
  bool p1Down  = !buttons_state[BTN_DOWN];
  bool p1Left  = !buttons_state[BTN_LEFT];
  bool p1Right = !buttons_state[BTN_RIGHT];

  if (p1Sel) {
    // hotkey SEL+UP - more turbo speed
    if (p1Up && !p1_lastUp) {
      if (p1_freqIdx < NUM_FREQ - 1) {
        p1_freqIdx++;
      } else {
        p1_freqIdx = 0; 
      }
      updateLed();
    }
    // hotkey SEL+DOWN -  decrease turbo speed
    if (p1Down && !p1_lastDown) {
      if (p1_freqIdx > 0) {
        p1_freqIdx--;
      } else {
        p1_freqIdx = NUM_FREQ - 1; 
      }
      updateLed();
    }
    // hotkey SEL-RIGHT - toggle turbo A
    if (p1Right && !p1_lastRight) {
      p1_turboA = !p1_turboA;
      updateLed();
    }
    // hotkey SEL-LEFT - toggle turbo B
    if (p1Left && !p1_lastLeft) {
      p1_turboB = !p1_turboB;
      updateLed();
    }
  }
  
  p1_lastUp    = p1Up; 
  p1_lastDown  = p1Down;
  p1_lastRight = p1Right; 
  p1_lastLeft  = p1Left;

  uint8_t period = 1;
  switch (TURBO_LEVELS[p1_freqIdx].hz) {
    case 20: period = 3;  break;
    case 15: period = 4;  break;
    case 10: period = 6;  break;
    case 8:  period = 7;  break;
    case 4:  period = 15; break;
    case 2:  period = 30; break;
  }

  frameCounter++;
  if (frameCounter >= period) {
    frameCounter = 0;
  }

  if ((p1A && !lastA && p1_turboA) || (p1B && !lastB && p1_turboB)) {
    frameCounter = 0;
  }

  lastA = p1A;
  lastB = p1B;

  uint8_t nextOut = 0xFF;
  for (uint8_t i = 0; i < 8; i++) {
    if (!buttons_state[i]) {
      nextOut &= ~(1 << i);
    }
  }

  if (p1Sel) {
    nextOut |= (1 << BTN_UP);
    nextOut |= (1 << BTN_DOWN);
    nextOut |= (1 << BTN_LEFT);
    nextOut |= (1 << BTN_RIGHT);
  }

  bool shouldBlock = blockButtonInActiveFrame(TURBO_LEVELS[p1_freqIdx].hz, frameCounter);

  if (p1_turboA && p1A) {
    if (shouldBlock) {
      nextOut |= (1 << BTN_A); 
    } else {
      nextOut &= ~(1 << BTN_A);
    }
  }
  if (p1_turboB && p1B) {
    if (shouldBlock) {
      nextOut |= (1 << BTN_B); 
    } else {
      nextOut &= ~(1 << BTN_B);
    }
  }
  p1OutBuffer = nextOut;

}