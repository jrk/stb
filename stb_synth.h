// stb_synth.h v0.03 - Sean Barrett - http://nothings.org/stb/stb_synth.h
// placed in the public domain -- not copyrighted

/*
Usage:

   Call 'stb_synth' to synthesize a waveform into a buffer
   Call 'stb_synth_add' to synthesize a waveform and add it into a buffer

   Parameters:
      output_buffer -- mono floating-point output buffer to synthesize to
      buffer_limit  -- the maximum writeable length of the buffer
                       (the "release" phase may write out more data than requested,
                        but clamps to this length)
      samples_per_second -- sample rate to synthesize at (e.g. 44100)
      note_duration_until_release -- note duration in seconds, including attack&decay phase
      pitch -- pitch expressed as a MIDI note (60 = middle C, 72 = next higher C, etc.)
               (can be microtonal)
      volume -- scale factor applied to note, normally 0..1 for output 0..1
      adsr -- NULL or stb_synth_adsr with volume waveform (see below)
      waveform1 -- audio waveform descriptor (see below), cannot be null
      waveform2 -- if not NULL, audio waveform descriptor at 'note_duration' time; morphs between them

   stb_synth_adsr -- This is a classic audio ADSR volume envelope; fields are:
      (A) attack_time   -- time in seconds for wave to rise to full volume
      (D) decay_time    -- time in seconds for wave to fall to sustain phase
      (S) sustain_level -- volume for sustain phase, 0..1 (1 is attack peak)
      (R) release_time  -- time for sound to decay to 0 after note ends (fake exponential)

   stb_synth_waveform -- Description of wave shape (see illustration below); fields are:
      (Z) zero_wait   -- time to wait at 0-level before doing waveform (PWM)
      (P) peak_time   -- time at which maximum peak (at 1) occurs, from 0..1 (1 = halfway point)
      (H) half_height -- height of waveform at halfway point (before it flips)
      (M) reflect     -- if false, 2nd half is first half inverted; if true, 2nd half is first half reflected & inverted


   M=0

              0-----P---1
       0  - - Z - - - - 1
     1              
     :             /\_
     :            /   \_
     H           /      \  
     :          /       |
     0 ________/        |________         _
                                 \        |
                                  \       |
                                   \     _/
                                    \  _/
                                     \/

   M=1

              0-----P---1
       0  - - Z - - - - 1
     1
     :             /\_
     :            /   \_
     H           /      \  
     :          /       |
     0 ________/        |         ________ 
                        |        /
                        |       / 
                        \_     / 
                          \_  /  
                            \/  

   Canonical shapes:

      Triangle wave:
          Z=0, P=0.5, H=0, M=* (either value of M works)

      Square wave:
          Z=0, P=0, H=1, M=* (either value of M works)

      Saw wave:
          Z=0, P=0, H=0, M=1 (normal)
          Z=0, P=1, H=*, M=1 (180-phase-shifted)
             (try morphing between these two; the H=* is a free variable to change the sound;
              if H=0 the half-way point is a pure triangle wave)

      PWM square wave (w = width; 0=full width, 1 = narrow)
          Z=w, P=0, H=1, M=0 (M=1 is different kind of PWM, I think M=0 is canonical)
      PWM saw wave (w = width; 0 = full width, 1 = narrow)
          Z=w, P=0, H=0, M=1 (M=0 is different kind of PWM, I think M=1 is canonical)
      PWM triangle wave
          Z=w, P=0.5, H=0, M=0 (M=1 is different kind of PWM, I think M=0 is canonical)
*/

#ifndef STB_INCLUDE_STB_SYNTH_H
#define STB_INCLUDE_STB_SYNTH_H

typedef struct
{
   float zero_wait;   // 0 to 1 PWM effect
   float peak_time;   // 0 to 1
   float half_height; // 0 to 1
   int   reflect;     // boolean symmetry of 2nd half of wave--mirror or identity
} stb_synth_waveform;

typedef struct
{
   float attack_time  ; // linear rise time in seconds
   float decay_time   ; // linear decay time in seconds;
   float sustain_level; // 0 to 1, level of sustained note
   float release_time ; // faux-exponential decay time
} stb_synth_adsr;

#ifdef __cplusplus
extern "C" {
#endif
int stb_synth(float *output_buffer, int buffer_limit,
                          int samples_per_second,
                          float note_duration_until_release,
                          float pitch, // in seconds
                          float volume, // 0..1
                          stb_synth_adsr *adsr,
                          stb_synth_waveform *waveform1,
                          stb_synth_waveform *waveform2); // if non-NULL, morph between two waveforms

int stb_synth_add(float *output_buffer, int buffer_limit,
                          int samples_per_second,
                          float note_duration_until_release,
                          float pitch, // in seconds
                          float volume, // 0..1
                          stb_synth_adsr *adsr,
                          stb_synth_waveform *waveform1,
                          stb_synth_waveform *waveform2); // if non-NULL, morph between two waveforms
#ifdef __cplusplus
}
#endif

#ifdef STB_DEFINE
#include <math.h>

typedef struct
{
   float start_height;
   float start_zero;
   float peak_time;   // 0 to 1
   float end_height; // 0 to 1
   float end_zero;
} stb_synth__right;

static float stb__pitch_to_freq(float pitch)
{
   return (float) (440.0 * pow(2.0, (pitch - 69.0) / 12.0));
}

#define stb__synth_lerp(t,a,b)        ((a) + ((b)-(a))*(t))
#define stb__synth_unlerp(t,a,b)      (((t) - (a)) / ((b) - (a)))
#define stb__synth_remap(t,a,b,c,d)   stb__synth_lerp(stb__synth_unlerp(t,a,b),c,d)
// remap without divide:
#define stb__synth_reciprocal(a,b)    ((b) != (a) ? (1.0f / ((b) - (a))) : 1)
#define stb__synth_remap_r(t,a,r,c,d) ((c) + ((d)-(c))*((t)-(a))*(r))

static void stb__make_right(stb_synth__right *wave, stb_synth_waveform *src)
{
   float p = stb__synth_lerp(src->peak_time, src->zero_wait, 1);
   if (src->reflect) {
      wave->start_height = -src->half_height;
      wave->start_zero = 0;
      wave->peak_time = 1 - p;
      wave->end_height = 0;
      wave->end_zero = 1 - src->zero_wait;
   } else {
      wave->start_height = 0;
      wave->start_zero = src->zero_wait;
      wave->peak_time = p;
      wave->end_height = -src->half_height;
      wave->end_zero = 1;
   }
   wave->start_zero += 1;
   wave->peak_time += 1;
   wave->end_zero += 1;
}

static stb_synth_adsr stb__default_adsr = { 0.001f,0,1,0.002f };

int stb__synth_raw(float *output_buffer, int buffer_limit, int zero,
                          int samples_per_second,
                          float note_duration_until_release,
                          float pitch, // in seconds
                          float volume, // 0..1
                          stb_synth_adsr *adsr,
                          stb_synth_waveform *waveform1,
                          stb_synth_waveform *waveform2)
{
   stb_synth_adsr env = *(adsr ? adsr : &stb__default_adsr);
   int i,j,len = (int) ((note_duration_until_release + env.release_time) * samples_per_second);
   float p, release_level = -1;
   float r0,r1,r2,r3,r4,r5,r6;
   float scale=0;
   float sec=0, dsec = 1.0f / samples_per_second;
   float t=0,dt = 1.0f / ((note_duration_until_release + env.release_time/4) * samples_per_second);
   float freq = stb__pitch_to_freq(pitch);
   float wavelength = samples_per_second / freq; // samples per waveform
   float wavesteps = 2 / wavelength;             // fraction of a waveform to step each sample
   stb_synth_waveform left_half_a, left_half_b, left_half;
   stb_synth__right right_half_a, right_half_b, right_half;

   if (len > buffer_limit) len = buffer_limit;
   env.decay_time += env.attack_time;

   left_half_a = *waveform1;
   left_half_a.peak_time = stb__synth_lerp(waveform1->peak_time, waveform1->zero_wait,1);
   stb__make_right(&right_half_a, waveform1);
   if (waveform2) {
      left_half_b = *waveform2;
      stb__make_right(&right_half_b, waveform2);
   } else {
      left_half_b = left_half_a;
      right_half_b = right_half_a;
   }

   p = 0;
   left_half = left_half_a;
   right_half = right_half_a;
   #ifndef STB_SYNTH_BLOCK_SIZE
   #define STB_SYNTH_BLOCK_SIZE 256
   #endif
   r0 = stb__synth_reciprocal(right_half.start_zero, right_half.peak_time);
   r1 = stb__synth_reciprocal(right_half.peak_time, right_half.end_zero);
   r2 = stb__synth_reciprocal(left_half.zero_wait, left_half.peak_time);
   r3 = stb__synth_reciprocal(left_half.peak_time, 1);
   r4 = stb__synth_reciprocal(0,env.attack_time);
   r5 = stb__synth_reciprocal(env.attack_time,env.decay_time);
   r6 = stb__synth_reciprocal(0,env.release_time);
   dt *= wavelength;
   for(j=0; j < len; j += STB_SYNTH_BLOCK_SIZE) {
      float data[STB_SYNTH_BLOCK_SIZE];
      int end = j + STB_SYNTH_BLOCK_SIZE;
      if (end > len) end = len;
      for (i=j; i < end; ++i) {
         float pcm;
         if (p >= 1) {
            if (p < right_half.start_zero || p > right_half.end_zero) {
               pcm = 0;
            } else if (p < right_half.peak_time) {
               pcm = stb__synth_remap_r(p, right_half.start_zero, r0, right_half.start_height, -1);
            } else {
               pcm = stb__synth_remap_r(p, right_half.peak_time, r1, -1, right_half.end_height);
            }
         } else {
            if (p < left_half.zero_wait)
               pcm = 0;
            else if (p < left_half.peak_time) {
               pcm = stb__synth_remap_r(p, left_half.zero_wait, r2, 0,1);
            } else {
               pcm = stb__synth_remap_r(p, left_half.peak_time, r3, 1, left_half.half_height);
            }
         }
         data[i-j] = pcm;
         p += wavesteps;
         if (p >= 2) {
            p -= 2;
            t += dt;
            if (t > 1) t=1;
            #define stb_s_lerp(t,a,b)  ((a) + ((b)-(a))*(t))
            #define stb_zl(field)      stb_s_lerp(t, left_half_a.field, left_half_b.field)
            left_half.zero_wait   = stb_zl(zero_wait);
            left_half.peak_time   = stb_zl(peak_time);
            left_half.half_height = stb_zl(half_height);
            #define stb_zr(field)      stb_s_lerp(t, right_half_a.field, right_half_b.field)
            right_half.start_height = stb_zr(start_height);
            right_half.start_zero   = stb_zr(start_zero);
            right_half.peak_time    = stb_zr(peak_time);
            right_half.end_height   = stb_zr(end_height);
            right_half.end_zero     = stb_zr(end_zero);
            #undef stb_zr
            #undef stb_zl
            #undef stb_s_lerp
            r0 = stb__synth_reciprocal(right_half.start_zero, right_half.peak_time);
            r1 = stb__synth_reciprocal(right_half.peak_time, right_half.end_zero);
            r2 = stb__synth_reciprocal(left_half.zero_wait, left_half.peak_time);
            r3 = stb__synth_reciprocal(left_half.peak_time, 1);
         }
      }
      for (i=j; i < end; ++i) {
         if (sec < env.attack_time)
            scale = stb__synth_remap_r(sec, 0,r4, 0,1);
         else if (sec < env.decay_time)
            scale = stb__synth_remap_r(sec, env.attack_time, r5, 1, env.sustain_level);
         else if (sec > note_duration_until_release) {
            float x;
            if (release_level == -1) release_level = scale;
            x = sec - note_duration_until_release;
            x = 1 - x * r6;
            scale = x*x*x * release_level;
         } else
            scale = env.sustain_level;
         data[i-j] *= scale;
         sec += dsec;
      }

      if (zero)
         for (i=j; i < end; ++i)
            output_buffer[i] = data[i-j] * volume;
      else
         for (i=j; i < end; ++i)
            output_buffer[i] += data[i-j] * volume;
   }
   return len;
}

int stb_synth(float *output_buffer, int buffer_limit,
                          int samples_per_second,
                          float note_duration_until_release,
                          float pitch, // in seconds
                          float volume, // 0..1
                          stb_synth_adsr *adsr,
                          stb_synth_waveform *waveform1,
                          stb_synth_waveform *waveform2)
{
   return stb__synth_raw(output_buffer, buffer_limit, 1, samples_per_second,
                  note_duration_until_release, pitch, volume, adsr, waveform1, waveform2);
}

int stb_synth_add(float *output_buffer, int buffer_limit,
                          int samples_per_second,
                          float note_duration_until_release,
                          float pitch, // in seconds
                          float volume, // 0..1
                          stb_synth_adsr *adsr,
                          stb_synth_waveform *waveform1,
                          stb_synth_waveform *waveform2)
{
   return stb__synth_raw(output_buffer, buffer_limit, 0, samples_per_second,
                  note_duration_until_release, pitch, volume, adsr, waveform1, waveform2);
}
#endif

#endif // STB_INCLUDE_STB_SYNTH_H