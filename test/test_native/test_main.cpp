// ---------------------------------------------------------------------------
// Host-side tests for the parts of the firmware that fail silently.
//
// These four modules are deliberately Arduino-free so they can run here:
//
//   timeMath.h       the millis() rollover fix - otherwise you find out in
//                    49.7 days, once, in a car
//   stringUtil.h     the bounded copy that replaced strcpy on network input
//   audioSpectrum.*  FFT, band mapping and the noise gates - a wrong band edge
//                    just looks like "the bars are off"
//   solarTime.*      sunrise/sunset for the night dim, checked against
//                    published times for Istanbul
//
// Run with:  pio test -e native      (needs a host g++ and platform-native)
//            pio test -e cyd2usb     (runs the same suite on the board)
// ---------------------------------------------------------------------------

#include <unity.h>

#include <math.h>
#include <string.h>

#include "audioSpectrum.h"
#include "solarTime.h"
#include "stringUtil.h"
#include "timeMath.h"

// ---------------------------------------------------------------------------
// timeMath: the 49.7-day rollover
// ---------------------------------------------------------------------------

static void test_deadline_not_reached_before_it_arrives()
{
  TEST_ASSERT_FALSE(hasReached(1000UL, 2000UL));
  TEST_ASSERT_TRUE(hasReached(2000UL, 2000UL));
  TEST_ASSERT_TRUE(hasReached(2001UL, 2000UL));
}

static void test_deadline_survives_the_counter_wrap()
{
  // 100 ms before the wrap, scheduling 200 ms out lands at 100 past zero.
  const unsigned long now = 0xFFFFFF9CUL; // 2^32 - 100
  const unsigned long deadline = now + 200UL;

  TEST_ASSERT_TRUE(deadline < now); // the deadline really did wrap

  TEST_ASSERT_FALSE(hasReached(now, deadline));
  TEST_ASSERT_FALSE(hasReached(0xFFFFFFFFUL, deadline));
  TEST_ASSERT_FALSE(hasReached(50UL, deadline));
  TEST_ASSERT_TRUE(hasReached(100UL, deadline));
  TEST_ASSERT_TRUE(hasReached(500UL, deadline));

  // The naive comparison v0.3.15 used gets this wrong in both directions.
  TEST_ASSERT_TRUE(now > deadline);
}

static void test_elapsed_survives_the_counter_wrap()
{
  const unsigned long since = 0xFFFFFF06UL; // 250 before the wrap

  TEST_ASSERT_FALSE(hasElapsed(0xFFFFFFF0UL, since, 500UL));
  TEST_ASSERT_FALSE(hasElapsed(200UL, since, 500UL));
  TEST_ASSERT_TRUE(hasElapsed(250UL, since, 500UL));
}

// ---------------------------------------------------------------------------
// stringUtil: bounded copies
// ---------------------------------------------------------------------------

static void test_copy_field_truncates_and_terminates()
{
  char destination[8];
  memset(destination, 'X', sizeof(destination));

  copyField(destination, "0123456789abcdef");

  TEST_ASSERT_EQUAL_STRING("0123456", destination);
  TEST_ASSERT_EQUAL_CHAR('\0', destination[7]);
}

static void test_copy_field_exact_fit_still_terminates()
{
  // The case plain strncpy gets wrong: source exactly fills the buffer.
  char destination[4];
  copyField(destination, "abc");
  TEST_ASSERT_EQUAL_STRING("abc", destination);
  TEST_ASSERT_EQUAL_CHAR('\0', destination[3]);
}

static void test_copy_field_handles_null_source()
{
  char destination[8] = "keepme";
  copyField(destination, nullptr);
  TEST_ASSERT_EQUAL_STRING("", destination);
}

// ---------------------------------------------------------------------------
// audioSpectrum
// ---------------------------------------------------------------------------

static AudioSpectrum spectrum;
static int32_t samples[AudioSpectrum::FFT_SIZE];

// Builds one window of a sine at `frequency` Hz, in the raw I2S format the
// INMP441 produces: signed 24-bit audio in the upper bits of a 32-bit word.
static void fillTone(double frequency, double amplitude)
{
  constexpr double SAMPLE_RATE = 16000.0;
  for (size_t i = 0; i < AudioSpectrum::FFT_SIZE; ++i)
  {
    const double value = amplitude * sin(2.0 * M_PI * frequency * i / SAMPLE_RATE);
    const int32_t sample24 = static_cast<int32_t>(value * 8388608.0);
    samples[i] = sample24 << 8;
  }
}

static size_t strongestBand(const AudioSpectrum::Output &out)
{
  size_t best = 0;
  for (size_t i = 1; i < AudioSpectrum::BAR_COUNT; ++i)
    if (out.levels[i] > out.levels[best])
      best = i;
  return best;
}

// Runs enough windows for the attack/release filter to settle.
static void settle(double frequency, double amplitude, AudioSpectrum::Output &out)
{
  spectrum.reset();
  for (int i = 0; i < 12; ++i)
  {
    fillTone(frequency, amplitude);
    spectrum.process(samples, out);
  }
}

static void test_fft_concentrates_a_pure_tone_in_one_bin()
{
  // Drive the transform directly with a cosine at bin 20, no window applied.
  // A correct radix-2 FFT puts all the energy at bins 20 and N-20 with
  // magnitude N/2; this is what catches a broken twiddle table.
  constexpr size_t N = AudioSpectrum::FFT_SIZE;
  constexpr size_t BIN = 20;

  float *re = spectrum.realPart();
  float *im = spectrum.imagPart();
  for (size_t i = 0; i < N; ++i)
  {
    re[i] = static_cast<float>(cos(2.0 * M_PI * BIN * i / N));
    im[i] = 0.0f;
  }

  spectrum.forwardTransform();

  double peak = 0.0;
  size_t peakBin = 0;
  double nextHighest = 0.0;
  for (size_t bin = 1; bin < N / 2; ++bin)
  {
    const double magnitude = sqrt(re[bin] * re[bin] + im[bin] * im[bin]);
    if (magnitude > peak)
    {
      nextHighest = peak;
      peak = magnitude;
      peakBin = bin;
    }
    else if (magnitude > nextHighest)
    {
      nextHighest = magnitude;
    }
  }

  TEST_ASSERT_EQUAL_UINT(BIN, peakBin);
  TEST_ASSERT_DOUBLE_WITHIN(1.0, N / 2.0, peak);
  // Everything else must be numerical noise, not a smeared second peak.
  TEST_ASSERT_TRUE(nextHighest < 0.01);
}

static void test_one_khz_tone_lights_the_expected_band()
{
  // 16 kHz / 256 = 62.5 Hz per bin, so 1 kHz is bin 16, which the band edge
  // table maps to band 8.
  AudioSpectrum::Output out = {};
  settle(1000.0, 0.2, out);

  TEST_ASSERT_EQUAL_UINT(8, strongestBand(out));
  TEST_ASSERT_TRUE(out.levels[8] > 50);
}

static void test_two_khz_tone_lights_the_expected_band()
{
  AudioSpectrum::Output out = {};
  settle(2000.0, 0.2, out);

  TEST_ASSERT_EQUAL_UINT(10, strongestBand(out));
  TEST_ASSERT_TRUE(out.levels[10] > 50);
}

static void test_silence_produces_a_completely_dark_frame()
{
  // Regression test. The mid and treble lifts add a constant offset, and the
  // silence gate used to run before them, so bands 8-15 sat at a permanent
  // floor and the visualizer never went fully dark in a quiet cabin.
  AudioSpectrum::Output out = {};
  settle(0.0, 0.0, out);

  for (size_t i = 0; i < AudioSpectrum::BAR_COUNT; ++i)
    TEST_ASSERT_EQUAL_UINT8(0, out.levels[i]);

  TEST_ASSERT_EQUAL_UINT8(0, out.overall);
}

static void test_silence_leaves_the_oscilloscope_flat()
{
  AudioSpectrum::Output out = {};
  settle(0.0, 0.0, out);

  for (size_t i = 0; i < AudioSpectrum::WAVEFORM_COUNT; ++i)
    TEST_ASSERT_EQUAL_UINT8(AudioSpectrum::WAVE_CENTRE, out.waveform[i]);
}

static void test_low_frequency_rumble_is_heavily_attenuated()
{
  // The cabin rejection contract: at identical amplitude a 125 Hz rumble must
  // read far quieter than a mid-band tone, or engine and electrical noise makes
  // the display twitch constantly at idle.
  AudioSpectrum::Output rumble = {};
  settle(125.0, 0.02, rumble);
  const uint8_t rumblePeak = rumble.levels[strongestBand(rumble)];

  AudioSpectrum::Output tone = {};
  settle(2000.0, 0.02, tone);
  const uint8_t tonePeak = tone.levels[strongestBand(tone)];

  TEST_ASSERT_TRUE(rumblePeak * 3 < tonePeak);
  TEST_ASSERT_TRUE(strongestBand(rumble) < 5);
}

static void test_loud_audio_moves_the_oscilloscope_off_centre()
{
  AudioSpectrum::Output out = {};
  settle(400.0, 0.2, out);

  bool moved = false;
  for (size_t i = 0; i < AudioSpectrum::WAVEFORM_COUNT; ++i)
  {
    if (out.waveform[i] != AudioSpectrum::WAVE_CENTRE)
    {
      moved = true;
      break;
    }
  }
  TEST_ASSERT_TRUE(moved);
}

static void test_levels_never_leave_the_display_range()
{
  AudioSpectrum::Output out = {};
  settle(1000.0, 0.45, out); // deliberately near clipping

  for (size_t i = 0; i < AudioSpectrum::BAR_COUNT; ++i)
    TEST_ASSERT_TRUE(out.levels[i] <= 100);
  TEST_ASSERT_TRUE(out.overall <= 100);

  for (size_t i = 0; i < AudioSpectrum::WAVEFORM_COUNT; ++i)
  {
    TEST_ASSERT_TRUE(out.waveform[i] >= 6);
    TEST_ASSERT_TRUE(out.waveform[i] <= 94);
  }
}

// ---------------------------------------------------------------------------
// solarTime
// ---------------------------------------------------------------------------

static constexpr double ISTANBUL_LAT = 41.0082;
static constexpr double ISTANBUL_LON = 28.9784;

static struct tm utcDate(int yearDayOneBased, int hour, int minute)
{
  struct tm value = {};
  value.tm_yday = yearDayOneBased - 1;
  value.tm_hour = hour;
  value.tm_min = minute;
  return value;
}

static void test_istanbul_midsummer_matches_published_times()
{
  // 21 June, published local (UTC+3) times are about 05:27 and 20:47.
  const struct tm date = utcDate(172, 0, 0);

  const double sunrise = solarEventUtcHour(date, ISTANBUL_LAT, ISTANBUL_LON, true) + 3.0;
  const double sunset = solarEventUtcHour(date, ISTANBUL_LAT, ISTANBUL_LON, false) + 3.0;

  TEST_ASSERT_DOUBLE_WITHIN(0.34, 5.45, sunrise); // +/- 20 minutes
  TEST_ASSERT_DOUBLE_WITHIN(0.34, 20.78, sunset);
}

static void test_istanbul_midwinter_matches_published_times()
{
  // 21 December, published local times are about 08:28 and 17:44.
  const struct tm date = utcDate(355, 0, 0);

  const double sunrise = solarEventUtcHour(date, ISTANBUL_LAT, ISTANBUL_LON, true) + 3.0;
  const double sunset = solarEventUtcHour(date, ISTANBUL_LAT, ISTANBUL_LON, false) + 3.0;

  TEST_ASSERT_DOUBLE_WITHIN(0.34, 8.47, sunrise);
  TEST_ASSERT_DOUBLE_WITHIN(0.34, 17.73, sunset);
}

static void test_winter_days_are_shorter_than_summer_days()
{
  const struct tm summer = utcDate(172, 0, 0);
  const struct tm winter = utcDate(355, 0, 0);

  const double summerLength =
      solarEventUtcHour(summer, ISTANBUL_LAT, ISTANBUL_LON, false) -
      solarEventUtcHour(summer, ISTANBUL_LAT, ISTANBUL_LON, true);
  const double winterLength =
      solarEventUtcHour(winter, ISTANBUL_LAT, ISTANBUL_LON, false) -
      solarEventUtcHour(winter, ISTANBUL_LAT, ISTANBUL_LON, true);

  TEST_ASSERT_TRUE(summerLength > winterLength + 4.0);
}

static void test_night_flag_tracks_local_midnight_and_noon()
{
  // Istanbul in midwinter: 12:00 UTC is 15:00 local, comfortably daylight;
  // 22:00 UTC is 01:00 local, comfortably night.
  TEST_ASSERT_FALSE(isNight(utcDate(355, 12, 0), ISTANBUL_LAT, ISTANBUL_LON));
  TEST_ASSERT_TRUE(isNight(utcDate(355, 22, 0), ISTANBUL_LAT, ISTANBUL_LON));
}

static void test_polar_latitude_falls_back_instead_of_returning_nan()
{
  // Above the Arctic circle in midsummer the sun never sets; the approximation
  // must return the documented neutral answer rather than NaN.
  const struct tm date = utcDate(172, 0, 0);
  const double sunrise = solarEventUtcHour(date, 78.0, 15.0, true);
  const double sunset = solarEventUtcHour(date, 78.0, 15.0, false);

  TEST_ASSERT_EQUAL_DOUBLE(6.0, sunrise);
  TEST_ASSERT_EQUAL_DOUBLE(18.0, sunset);
}

// ---------------------------------------------------------------------------

void setUp() {}
void tearDown() {}

static int runAllTests()
{
  UNITY_BEGIN();

  RUN_TEST(test_deadline_not_reached_before_it_arrives);
  RUN_TEST(test_deadline_survives_the_counter_wrap);
  RUN_TEST(test_elapsed_survives_the_counter_wrap);

  RUN_TEST(test_copy_field_truncates_and_terminates);
  RUN_TEST(test_copy_field_exact_fit_still_terminates);
  RUN_TEST(test_copy_field_handles_null_source);

  RUN_TEST(test_fft_concentrates_a_pure_tone_in_one_bin);
  RUN_TEST(test_one_khz_tone_lights_the_expected_band);
  RUN_TEST(test_two_khz_tone_lights_the_expected_band);
  RUN_TEST(test_silence_produces_a_completely_dark_frame);
  RUN_TEST(test_silence_leaves_the_oscilloscope_flat);
  RUN_TEST(test_low_frequency_rumble_is_heavily_attenuated);
  RUN_TEST(test_loud_audio_moves_the_oscilloscope_off_centre);
  RUN_TEST(test_levels_never_leave_the_display_range);

  RUN_TEST(test_istanbul_midsummer_matches_published_times);
  RUN_TEST(test_istanbul_midwinter_matches_published_times);
  RUN_TEST(test_winter_days_are_shorter_than_summer_days);
  RUN_TEST(test_night_flag_tracks_local_midnight_and_noon);
  RUN_TEST(test_polar_latitude_falls_back_instead_of_returning_nan);

  return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>
void setup()
{
  delay(2000); // let the host attach to the serial port
  runAllTests();
}
void loop() {}
#else
int main()
{
  return runAllTests();
}
#endif
