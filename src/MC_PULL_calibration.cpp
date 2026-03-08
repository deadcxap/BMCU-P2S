#include "MC_PULL_calibration.h"
#include "Motion_control.h"
#include "ADC_DMA.h"
#include "Flash_saves.h"
#include "hal/time_hw.h"
#include "Debug_log.h"
#include "app_api.h"
#include <math.h>

extern void RGB_update();

static inline float adc_pull_raw_ch(int ch, const float *v8)
{
    switch (ch)
    {
    case 0: return (float)v8[6];
    case 1: return (float)v8[4];
    case 2: return (float)v8[2];
    default:return (float)v8[0];
    }
}

static inline float adc_pull_v_cal(int ch)
{
    const float *d = ADC_DMA_get_value();
    return adc_pull_raw_ch(ch, d) + MC_PULL_V_OFFSET[ch];
}

static const float    CAL_PRESS_DELTA_V = 0.1f;
static const float    CAL_CENTER_EPS_V  = 0.02f;
static const uint32_t CAL_STABLE_MS     = 200;
static const uint32_t CAL_TIMEOUT_MS    = 30000;

static void blink_all(uint8_t r, uint8_t g, uint8_t b, int times = 4, int on_ms = 60, int off_ms = 60)
{
    for (int k = 0; k < times; k++)
    {
        for (int ch = 0; ch < 4; ch++) MC_PULL_ONLINE_RGB_set(ch, r, g, b);
        RGB_update(); delay(on_ms);
        for (int ch = 0; ch < 4; ch++) MC_PULL_ONLINE_RGB_set(ch, 0, 0, 0);
        RGB_update(); delay(off_ms);
    }
}

static void blink_one(int ch, uint8_t r, uint8_t g, uint8_t b, int times = 3, int on_ms = 70, int off_ms = 70)
{
    for (int k = 0; k < times; k++)
    {
        MC_PULL_ONLINE_RGB_set(ch, r, g, b);
        RGB_update(); delay(on_ms);
        MC_PULL_ONLINE_RGB_set(ch, 0, 0, 0);
        RGB_update(); delay(off_ms);
    }
}

static bool capture_extreme_wait_release(int ch, float center_v, bool want_min, float &out_best)
{
    const uint32_t tpm = time_hw_ticks_per_ms();
    const uint32_t t0  = time_ticks32();
    const uint32_t dt  = (uint32_t)CAL_TIMEOUT_MS * tpm;

    bool pressed = false;
    float best = want_min ? 10.0f : 0.0f;

    uint32_t stable_t0 = 0;

    for (;;)
    {
        const uint32_t now_t = time_ticks32();
        if ((uint32_t)(now_t - t0) >= dt) break;

        const float v = adc_pull_v_cal(ch);

        // miganie
        const uint32_t elapsed_ms = (uint32_t)((now_t - t0) / tpm);
        if (((elapsed_ms / 200u) & 1u) == 0u)
        {
            if (want_min) MC_PULL_ONLINE_RGB_set(ch, 0x00, 0x00, 0x10);
            else          MC_PULL_ONLINE_RGB_set(ch, 0x10, 0x00, 0x00);
        }
        else
        {
            MC_PULL_ONLINE_RGB_set(ch, 0, 0, 0);
        }
        RGB_update();

        if (!pressed)
        {
            if (want_min) { if (v < (center_v - CAL_PRESS_DELTA_V)) { pressed = true; best = v; } }
            else          { if (v > (center_v + CAL_PRESS_DELTA_V)) { pressed = true; best = v; } }
        }
        else
        {
            if (want_min) { if (v < best) best = v; }
            else          { if (v > best) best = v; }

            if (fabsf(v - center_v) <= CAL_CENTER_EPS_V)
            {
                if (stable_t0 == 0) stable_t0 = now_t;
                if ((uint32_t)(now_t - stable_t0) >= (uint32_t)CAL_STABLE_MS * tpm)
                {
                    out_best = best;
                    MC_PULL_ONLINE_RGB_set(ch, 0, 0, 0);
                    RGB_update();
                    return true;
                }
            }
            else
            {
                stable_t0 = 0;
            }
        }

        delay(15);
    }

    out_best = center_v;
    MC_PULL_ONLINE_RGB_set(ch, 0, 0, 0);
    RGB_update();
    return false;
}

static void capture_minmax_one_ch_event(int ch, float center_v, float &out_min, float &out_max)
{
    float vmin = center_v;
    float vmax = center_v;

    bool ok_min = capture_extreme_wait_release(ch, center_v, true, vmin);
    if (ok_min) blink_one(ch, 0x10, 0x10, 0x00, 2, 60, 60);

    bool ok_max = capture_extreme_wait_release(ch, center_v, false, vmax);
    if (ok_max) blink_one(ch, 0x10, 0x10, 0x00, 2, 60, 60);

    if (vmin > (center_v - 0.050f)) vmin = (center_v - 0.050f);
    if (vmax < (center_v + 0.050f)) vmax = (center_v + 0.050f);
    if (vmax <= vmin + 0.10f) { vmin = center_v - 0.10f; vmax = center_v + 0.10f; }

    out_min = vmin;
    out_max = vmax;
}

void MC_PULL_calibration_clear()
{
    Flash_MC_PULL_cal_clear();
}

void MC_PULL_calibration_boot()
{
    for (int i = 0; i < 6; i++) { ADC_DMA_poll(); delay(20); }

    MC_PULL_detect_channels_inserted();

    float offs[4], vmin[4], vmax[4];
    if (Flash_MC_PULL_cal_read(offs, vmin, vmax))
    {
        for (int ch = 0; ch < 4; ch++)
        {
            MC_PULL_V_OFFSET[ch] = offs[ch];
            MC_PULL_V_MIN[ch]    = vmin[ch];
            MC_PULL_V_MAX[ch]    = vmax[ch];
        }
        return;
    }

    double sum_raw[4] = {0, 0, 0, 0};
    const int N = 90;
    const uint32_t tpm = time_hw_ticks_per_ms();
    const uint32_t t0  = time_ticks32();

    for (int k = 0; k < N; k++)
    {
        const float *v = ADC_DMA_get_value();

        for (int ch = 0; ch < 4; ch++)
        {
            if (!filament_channel_inserted[ch]) continue;
            sum_raw[ch] += adc_pull_raw_ch(ch, v);
        }

        const uint32_t now_t = time_ticks32();
        const uint32_t elapsed_ms = (uint32_t)((now_t - t0) / tpm);
        bool on = (((elapsed_ms / 200u) & 1u) == 0u);
        for (int ch = 0; ch < 4; ch++)
            MC_PULL_ONLINE_RGB_set(ch, on ? 0x10 : 0, on ? 0x10 : 0, 0x00);
        RGB_update();
        delay(15);
    }

    float center_raw[4] = {1.65f, 1.65f, 1.65f, 1.65f};
    for (int ch = 0; ch < 4; ch++)
    {
        if (!filament_channel_inserted[ch]) continue;
        center_raw[ch] = (float)(sum_raw[ch] / (double)N);
    }

    for (int ch = 0; ch < 4; ch++)
    {
        if (!filament_channel_inserted[ch]) {
            MC_PULL_V_OFFSET[ch] = 0.0f;
            continue;
        }
        MC_PULL_V_OFFSET[ch] = 1.65f - center_raw[ch];
    }

    float center_v_ref[4] = {1.65f, 1.65f, 1.65f, 1.65f};
    for (int ch = 0; ch < 4; ch++)
    {
        if (!filament_channel_inserted[ch]) continue;
        center_v_ref[ch] = center_raw[ch] + MC_PULL_V_OFFSET[ch];
    }

    blink_all(0x10, 0x10, 0x00, 3, 60, 60);

    for (int ch = 0; ch < 4; ch++)
    {
        if (!filament_channel_inserted[ch]) {
            MC_PULL_V_MIN[ch] = 1.55f;
            MC_PULL_V_MAX[ch] = 1.75f;
            continue;
        }

        float mn, mx;
        capture_minmax_one_ch_event(ch, center_v_ref[ch], mn, mx);
        MC_PULL_V_MIN[ch] = mn;
        MC_PULL_V_MAX[ch] = mx;

        MC_PULL_ONLINE_RGB_set(ch, 0, 0, 0);
        RGB_update();
        delay(80);
    }

    bool ok = Flash_MC_PULL_cal_write_all(MC_PULL_V_OFFSET, MC_PULL_V_MIN, MC_PULL_V_MAX);
    if (!ok) blink_all(0x10, 0x00, 0x00, 6, 80, 80);
    else     blink_all(0x10, 0x10, 0x00, 6, 60, 60);

    delay(200);
}
