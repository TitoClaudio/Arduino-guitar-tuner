#include <LiquidCrystal.h>
#include <math.h>

LiquidCrystal lcd(7, 8, 9, 10, 11, 12);

byte barChar[8] = {
  B11111,
  B11111,
  B11111,
  B11111,
  B11111,
  B11111,
  B11111,
  B11111
};


typedef struct {
  const char* name;
  int octave;
  float cents;
  float freqNote;
} NoteResult;

static float clampf(float v, float lo, float hi);
static NoteResult freqToNote(float f);
static void captureFrame();
static int computeAmplitude();
static bool autocorrFrequency(float &freqOut);

// ---------- DSP parameters ----------
static const uint16_t N = 192;     // samples per frame
static const uint16_t FS = 4000;   // approx sample rate (Hz)
static const uint16_t MIN_F = 70;  // ~E2=82Hz
static const uint16_t MAX_F = 400;
static const uint16_t MIN_LAG = FS / MAX_F;
static const uint16_t MAX_LAG = FS / MIN_F;

static const int AMP_MIN = 3;                 // min amplitude (ADC counts)
static const float CORR_THRESHOLD = 0.35;     // normalized correlation threshold

static int16_t x[N];
static const char* NOTE_NAMES[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

// ---------- Helpers ----------
static float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static NoteResult freqToNote(float f) {
  float midi = 69.0f + 12.0f * (logf(f / 440.0f) / logf(2.0f));
  int midiRound = (int)lroundf(midi);

  int noteIndex = (midiRound % 12 + 12) % 12;
  int octave = (midiRound / 12) - 1;

  float freqNote = 440.0f * powf(2.0f, (midiRound - 69) / 12.0f);
  float cents = 1200.0f * (logf(f / freqNote) / logf(2.0f));

  NoteResult r;
  r.name = NOTE_NAMES[noteIndex];
  r.octave = octave;
  r.cents = cents;
  r.freqNote = freqNote;
  return r;
}

// ---------- Sampling ----------
static void captureFrame() {
  int vb = analogRead(A1);
  if (vb < 200 || vb > 800) vb = 512;

  const uint32_t dt = 1000000UL / FS;
  uint32_t t = micros();

  for (uint16_t i = 0; i < N; i++) {
    int v = analogRead(A0);
    x[i] = (int16_t)(v - vb);

    t += dt;
    while ((int32_t)(micros() - t) < 0) { /* wait */ }
  }
}

static int computeAmplitude() {
  int16_t vmin = 32767, vmax = -32768;
  for (uint16_t i = 0; i < N; i++) {
    if (x[i] < vmin) vmin = x[i];
    if (x[i] > vmax) vmax = x[i];
  }
  return (int)((vmax - vmin) / 2);
}

// ---------- Autocorrelation ----------
static bool autocorrFrequency(float &freqOut) {
  int amp = computeAmplitude();
  if (amp < AMP_MIN) return false;

  float r0 = 0.0f;
  for (uint16_t i = 0; i < N; i++) {
    float v = (float)x[i];
    r0 += v * v;
  }
  if (r0 < 1e-6f) return false;

  float bestR = -1.0f;
  uint16_t bestLag = 0;

  uint16_t maxLag = MAX_LAG;
  if (maxLag > N - 2) maxLag = N - 2;

  for (uint16_t lag = MIN_LAG; lag <= maxLag; lag++) {
    float r = 0.0f;
    for (uint16_t i = 0; i < N - lag; i++) {
      r += (float)x[i] * (float)x[i + lag];
    }
    float rn = r / r0;
    if (rn > bestR) {
      bestR = rn;
      bestLag = lag;
    }
  }

  if (bestLag == 0) return false;
  if (bestR < CORR_THRESHOLD) return false;

  auto corrAt = [&](uint16_t lag) -> float {
    float r = 0.0f;
    for (uint16_t i = 0; i < N - lag; i++) r += (float)x[i] * (float)x[i + lag];
    return r / r0;
  };

  float r1 = (bestLag > MIN_LAG) ? corrAt(bestLag - 1) : bestR;
  float r2 = bestR;
  float r3 = (bestLag + 1 <= maxLag) ? corrAt(bestLag + 1) : bestR;

  float denom = (r1 - 2.0f * r2 + r3);
  float delta = 0.0f;
  if (fabsf(denom) > 1e-6f) {
    delta = 0.5f * (r1 - r3) / denom;
    delta = clampf(delta, -0.5f, 0.5f);
  }

  float lagInterp = (float)bestLag + delta;
  float f = (float)FS / lagInterp;

  if (f < (float)MIN_F || f > (float)MAX_F) return false;

  freqOut = f;
  return true;
}

// ---------- Bar meter ----------
static void drawTunerBar(float cents) {
  const int width = 16;
  const int center = 7;
  const float range = 50.0f;   // +/- cents for full travel

  cents = clampf(cents, -range, range);

  int pos = (int)lroundf((cents + range) * (width - 1) / (2.0f * range));

  // Draw base bar
  lcd.setCursor(0, 1);
  for (int i = 0; i < width; i++) {
    if (i == center) lcd.print('|');
    else lcd.print('-');
  }

  // Draw moving vertical block
  lcd.setCursor(pos, 1);
  lcd.write(byte(0));
}


void setup() {
  lcd.begin(16, 2);
  lcd.clear();
  lcd.createChar(0, barChar);
  lcd.print("Autocorr Tuner");
  lcd.setCursor(0, 1);
  lcd.print("Pluck a string");
  delay(700);
  lcd.clear();
}

void loop() {
  captureFrame();

  float f;
  if (!autocorrFrequency(f)) {
    lcd.setCursor(0, 0);
    lcd.print("Freq: ---       ");
    lcd.setCursor(0, 1);
    lcd.print("No/weak signal  ");
    delay(20);
    return;
  }

  NoteResult n = freqToNote(f);

  // Line 1: Note + octave + freq
  lcd.setCursor(0, 0);
  lcd.print(n.name);
  lcd.print(n.octave);
  lcd.print(" ");
  lcd.print(f, 1);
  lcd.print("Hz     "); // pad

  // Line 2: Bar
  drawTunerBar(n.cents);

  delay(20);
}
