// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
#include "motion_engine.h"
#include <math.h>
#include <string.h>
#define CALIBRATION_SAMPLES 60U
#define RAD_PER_DEG         0.01745329252f
#define NOTE_LENGTH_MS      35U
#define HIT_COOLDOWN_MS     120U
#define PEAK_WINDOW_MS      20U
#define STALE_MS            150U
static const uint8_t drum_notes[] = {36, 38, 42, 46, 45, 49};
static const uint8_t scales[][7]  = {{0, 2, 4, 7, 9},
                                     {0, 3, 5, 7, 10},
                                     {0, 2, 4, 5, 7, 9, 11},
                                     {0, 2, 3, 5, 7, 8, 10}};
static float clampf_(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static float dot_(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float norm_(const float v[3])
{
    return sqrtf(dot_(v, v));
}

static void normalize_(float v[3])
{
    float n = norm_(v);
    if(n > 0.0001f)
        for(int i = 0; i < 3; ++i)
            v[i] /= n;
}

static void note_off_(motion_engine_t* e)
{
    if(e->status.playing)
    {
        e->send(e->user, 0x80U | e->active_channel, e->active_note, 0);
        e->status.playing = false;
    }
}

static void note_on_(motion_engine_t* e,
                     uint8_t channel,
                     uint8_t note,
                     uint8_t velocity)
{
    note_off_(e);
    e->active_channel  = channel;
    e->active_note     = note;
    e->status.note     = note;
    e->status.velocity = velocity;
    e->status.playing  = true;
    e->send(e->user, 0x90U | channel, note, velocity);
}

poom_midi_config_t motion_default_config(void)
{
    return (poom_midi_config_t) {.mode        = POOM_MIDI_DRUM,
                                 .drum        = 1,
                                 .sensitivity = 1,
                                 .response    = 1,
                                 .scale       = POOM_MIDI_PENTA_MAJOR,
                                 .tonic       = 0,
                                 .octave      = 4,
                                 .intensity   = 1};
}

void motion_engine_pause(motion_engine_t* e)
{
    note_off_(e);
    e->gate           = false;
    e->status.enabled = false;
    e->hit_armed      = false;
    e->peak_pending   = false;
    e->quiet_ms       = 0;
}

void motion_engine_configure(motion_engine_t* e,
                             const poom_midi_config_t* config)
{
    motion_engine_pause(e);
    e->config = *config;
    if((unsigned)e->config.mode > POOM_MIDI_MELODY)
        e->config.mode = POOM_MIDI_DRUM;
    if(e->config.drum >= sizeof(drum_notes))
        e->config.drum = 1;
    if(e->config.sensitivity > 2)
        e->config.sensitivity = 1;
    if(e->config.response > 2)
        e->config.response = 1;
    if((unsigned)e->config.scale > POOM_MIDI_MINOR)
        e->config.scale = POOM_MIDI_PENTA_MAJOR;
    if(e->config.tonic > 11)
        e->config.tonic = 0;
    if(e->config.octave > 8)
        e->config.octave = 8;
    if(e->config.intensity > 2)
        e->config.intensity = 1;
    e->degree     = e->config.scale < POOM_MIDI_MAJOR ? 2 : 3;
    unsigned note = 12U * (e->config.octave + 1U) + e->config.tonic +
                    scales[e->config.scale][e->degree];
    e->status.note = e->config.mode == POOM_MIDI_DRUM
                         ? drum_notes[e->config.drum]
                         : (uint8_t)(note > 127U ? 127U : note);
}

static void reset_calibration_samples_(motion_engine_t* e)
{
    e->status.calibration_percent = 0;
    e->calibration_samples        = 0;
    memset(e->sum_acc, 0, sizeof(e->sum_acc));
    memset(e->sum_gyro, 0, sizeof(e->sum_gyro));
}

void motion_engine_calibrate(motion_engine_t* e)
{
    motion_engine_pause(e);
    e->status.calibrated = false;
    reset_calibration_samples_(e);
    e->tilt = 0;
}

static void wait_for_sensor_(motion_engine_t* e)
{
    motion_engine_pause(e);
    e->status.sensor_wait = true;
    if(!e->status.calibrated)
        reset_calibration_samples_(e);
}

void motion_engine_init(motion_engine_t* e,
                        const poom_midi_config_t* config,
                        motion_send_fn send,
                        void* user)
{
    memset(e, 0, sizeof(*e));
    e->send = send;
    e->user = user;
    motion_engine_configure(e, config);
    motion_engine_calibrate(e);
}

void motion_engine_gate(motion_engine_t* e, bool pressed)
{
    if(!pressed)
    {
        if(e->config.mode == POOM_MIDI_MELODY)
            note_off_(e);
        e->gate = false;
    }
    else if(e->config.mode == POOM_MIDI_MELODY && e->status.connected &&
            e->status.calibrated && !e->status.error && !e->status.sensor_wait)
        e->gate = true;
}

void motion_engine_toggle_drums(motion_engine_t* e)
{
    if(e->status.enabled)
        motion_engine_pause(e);
    else if(e->config.mode == POOM_MIDI_DRUM && e->status.connected &&
            e->status.calibrated && !e->status.error && !e->status.sensor_wait)
    {
        e->status.enabled = true;
        e->quiet_ms       = 0;
        e->hit_armed      = false;
    }
}

static void calibrate_sample_(motion_engine_t* e, const motion_sample_t* s)
{
    const float acc_norm = norm_(s->acc);
    float delta[3];
    for(int i = 0; i < 3; ++i)
        delta[i] = s->acc[i] - e->previous_acc[i];
    const bool quiet = acc_norm > 0.90f && acc_norm < 1.10f &&
                       norm_(s->gyro) < 12.0f &&
                       (e->calibration_samples == 0 || norm_(delta) < 0.035f);
    memcpy(e->previous_acc, s->acc, sizeof(e->previous_acc));
    if(!quiet)
    {
        reset_calibration_samples_(e);
        return;
    }
    for(int i = 0; i < 3; ++i)
    {
        e->sum_acc[i] += s->acc[i];
        e->sum_gyro[i] += s->gyro[i];
    }
    e->calibration_samples++;
    e->status.calibration_percent =
        (uint8_t)(e->calibration_samples * 100U / CALIBRATION_SAMPLES);
    if(e->calibration_samples < CALIBRATION_SAMPLES)
        return;
    for(int i = 0; i < 3; ++i)
    {
        e->gravity[i] = e->sum_acc[i] / CALIBRATION_SAMPLES;
        e->bias[i]    = e->sum_gyro[i] / CALIBRATION_SAMPLES;
    }
    normalize_(e->gravity);
    memcpy(e->center, e->gravity, sizeof(e->center));
    /* Project board Y into the gravity-normal plane; use X for near-vertical Y.
     */
    const unsigned axis = fabsf(e->center[1]) < 0.9f ? 1U : 0U;
    for(int i = 0; i < 3; ++i)
        e->lateral[i] = ((unsigned)i == axis ? 1.0f : 0.0f) -
                        e->center[axis] * e->center[i];
    normalize_(e->lateral);
    e->status.calibrated = true;
}

static void orientation_(motion_engine_t* e, const motion_sample_t* s, float dt)
{
    float w[3];
    for(int i = 0; i < 3; ++i)
        w[i] = (s->gyro[i] - e->bias[i]) * RAD_PER_DEG;
    const float* g = e->gravity;
    /* Gravity in device coordinates rotates opposite to the device. */
    float predicted[3]     = {g[0] + (g[1] * w[2] - g[2] * w[1]) * dt,
                              g[1] + (g[2] * w[0] - g[0] * w[2]) * dt,
                              g[2] + (g[0] * w[1] - g[1] * w[0]) * dt};
    const float n          = norm_(s->acc);
    const float correction = n > 0.85f && n < 1.15f ? dt / (0.45f + dt) : 0.0f;
    for(int i = 0; i < 3; ++i)
        e->gravity[i] =
            (1.0f - correction) * predicted[i] + correction * s->acc[i];
    normalize_(e->gravity);
    const float tilt =
        atan2f(dot_(e->gravity, e->lateral), dot_(e->gravity, e->center)) /
        RAD_PER_DEG;
    e->tilt += dt / (0.045f + dt) * (tilt - e->tilt);
}

static void melody_(motion_engine_t* e)
{
    const uint8_t count = e->config.scale < POOM_MIDI_MAJOR ? 5 : 7;
    const float spacing = 60.0f / (float)(count - 1);
    if(e->degree >= count)
        e->degree = count / 2;
    /* Two-degree hysteresis on either side of every note boundary. */
    while(e->degree + 1 < count &&
          e->tilt > -30.0f + (e->degree + 0.5f) * spacing + 2.0f)
        e->degree++;
    while(e->degree > 0 &&
          e->tilt < -30.0f + (e->degree - 0.5f) * spacing - 2.0f)
        e->degree--;
    unsigned note = 12U * (e->config.octave + 1U) + e->config.tonic +
                    scales[e->config.scale][e->degree];
    if(note > 127U)
        note = 127U;
    const uint8_t velocities[] = {48, 85, 115};
    if(e->gate && (!e->status.playing ||
                   (e->config.moving_note && e->active_note != note)))
        note_on_(e, 0, (uint8_t)note, velocities[e->config.intensity]);
    else if(!e->status.playing)
        e->status.note = (uint8_t)note;
}

static void drum_(motion_engine_t* e, const motion_sample_t* s, uint32_t now)
{
    if(!e->status.enabled)
        return;
    const float thresholds[] = {0.60f, 0.38f, 0.22f};
    const float threshold    = thresholds[e->config.sensitivity];
    /* Signed downward linear acceleration rejects the upward stroke. */
    const float downward = 1.0f - dot_(s->acc, e->gravity);
    float linear[3];
    for(int i = 0; i < 3; ++i)
        linear[i] = s->acc[i] - e->gravity[i];
    if(!e->hit_armed && !e->peak_pending)
    {
        if(norm_(linear) < 0.14f && norm_(s->gyro) < 70.0f)
        {
            if(e->quiet_ms == 0)
                e->quiet_ms = now;
            if(now - e->quiet_ms >= 40U && now - e->hit_ms >= HIT_COOLDOWN_MS)
                e->hit_armed = true;
        }
        else
            e->quiet_ms = 0;
    }
    if(e->hit_armed && downward >= threshold)
    {
        e->hit_armed    = false;
        e->peak_pending = true;
        e->peak         = downward;
        e->peak_ms      = now;
        e->quiet_ms     = 0;
    }
    if(!e->peak_pending)
        return;
    if(downward > e->peak)
        e->peak = downward;
    if(now - e->peak_ms < PEAK_WINDOW_MS)
        return;
    const float response[] = {1.5f, 1.0f, 0.65f};
    const float strength =
        clampf_((e->peak - threshold) / (1.8f - threshold), 0.0f, 1.0f);
    const uint8_t velocity =
        (uint8_t)(30.0f + 97.0f * powf(strength, response[e->config.response]));
    note_on_(e, 9, drum_notes[e->config.drum], velocity);
    e->status.hit_count++;
    e->hit_ms       = now;
    e->note_on_ms   = now;
    e->peak_pending = false;
}

void motion_engine_step(motion_engine_t* e,
                        const motion_sample_t* s,
                        uint32_t now,
                        bool connected)
{
    if(connected != e->status.connected)
    {
        motion_engine_pause(e);
        e->status.connected = connected;
    }
    if(e->status.playing && e->active_channel == 9 &&
       now - e->note_on_ms >= NOTE_LENGTH_MS)
        note_off_(e);
    if(e->have_sample && now - e->sample_ms > STALE_MS)
    {
        wait_for_sensor_(e);
    }
    if(s == NULL)
        return;
    for(int i = 0; i < 3; ++i)
    {
        if(!isfinite(s->acc[i]) || !isfinite(s->gyro[i]))
        {
            wait_for_sensor_(e);
            return;
        }
    }
    const bool recovering = e->status.sensor_wait;
    const float dt        = recovering ? 0.0f
                            : e->have_sample
                                ? clampf_((now - e->sample_ms) * 0.001f, 0.001f, 0.05f)
                                : 0.01f;
    e->sample_ms          = now;
    e->have_sample        = true;
    e->status.error       = false;
    e->status.sensor_wait = false;
    if(!e->status.calibrated)
    {
        calibrate_sample_(e, s);
        return;
    }
    if(recovering)
    {
        /* Motion during a read gap cannot be integrated. Re-anchor gravity
         * from a usable accelerometer sample, retaining center and gyro bias.
         */
        const float acc_norm = norm_(s->acc);
        if(acc_norm > 0.85f && acc_norm < 1.15f)
        {
            memcpy(e->gravity, s->acc, sizeof(e->gravity));
            normalize_(e->gravity);
        }
    }
    orientation_(e, s, dt);
    if(e->config.mode == POOM_MIDI_MELODY)
        melody_(e);
    else
    {
        e->status.note = drum_notes[e->config.drum];
        if(connected)
            drum_(e, s, now);
    }
}
