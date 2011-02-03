// stb_audio_mixer.h v0.01 - Sean Barrett - http://nothings.org/stb/
// public domain - no copyright

/*
   You can use this two ways:
       1. core mixing library
       2. mixing library running on top of direct sound

   It's possible to use "both", e.g. to playback audio then switch
   to mixing to a temporary buffer. However you can't do both at once
   (the core mixer uses global variables, so there's only one instance
   of it).



*/

#ifndef STB_INCLUDE_STB_AUDIO_MIXER_H
#define STB_INCLUDE_STB_AUDIO_MIXER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int stb_mixint;

// initialize mixlow; max_premix_samples is number of samples to mix in advance,
// which determines memory usage (12 bytes * max_premix_samples); note that
extern void stb_mixlow_init(stb_mixint max_premix_samples);
extern void stb_mixlow_deinit(void);

// clear all sample data and set current time in samples
extern void stb_mixlow_reset(stb_mixint curtime);

// advance the current time (discarding sample info)
extern void stb_mixlow_set_curtime(stb_mixint curtime);
extern stb_mixint stb_mixlow_get_curtime(void);

extern void stb_mixlow_global_volume(float vol); // use to turn up/down overall mix

// compute a stereo 16-bit mix buffer--start_time must be >= curtime!
// returns number of samples written to output
extern int stb_mixlow_mix(short *output, stb_mixint start_time, stb_mixint duration);
// note that the tricky part of mixlow is that you can ask for, say,
// .1s of data every 0.01s, and mixlow avoids remixing it if it can.
// in particular, you can add new samples to play without forcing remixing;
// only ending old samples incurs a remix cost (and this might change later)
// note that 'premix_samples' in the init function gives a max for duration

/*****     SAMPLE PLAYBACK INTERFACE    *****/

typedef enum
{
   FADE_none=0,
   FADE_linear=1,
   FADE_equalpower,
   FADE_release,      // faux-logarithmix curve
   FADE_pulserelease, // a little volume _increase_ at the start, good for strings
} Fademode;

// add a sample to be played back, with the following properties:
//       samples   : pointer to (interleaved) sample data (not copied)
//       sample_len: length of 'samples', in (stereo) samples
//       safe      : 0 if sample is valid until 'end_blocks'; 1 if it's always valid
//       channels  : 1 or 2
//       start     : starting offset in samples (can be fractional; fraction ignored if rate=1)
//       start_time: time to play back block in global sample ticks
//       duration  : number of samples to play
//       step      : 1/rate of playback relative to global sample rate
//       vol       : attenuation applied at start_time
//       end_vol   : attenuation applied at start_time+duration
//           actual attenuation is linearly interpolated from the above
//       pan       : -1 to 1; if mono, attenuates one side; if stereo, turns both to same side
//                   (i.e. lerps between stereo image and a panned, mono-ified image)
//       handle    : handle used to distinguish a collection of active blocks
extern void stb_mixlow_add_playback(short *samples, int sample_len, int safe,
                                int channels, float first,
                                stb_mixint start_time, stb_mixint duration, float step,
                                int fadein_mode, stb_mixint fadein_start, stb_mixint fadein_len,
                                float vol, float pan,
                                void *handle);
extern void stb_mixlow_add_playback_float(float *samples, int sample_len, int safe,
                                int channels, float first,
                                stb_mixint start_time, stb_mixint duration, float step,
                                int fadein_mode, stb_mixint fadein_start, stb_mixint fadein_len,
                                float vol, float pan,
                                void *handle);

// silences and deletes a collection of sample playbacks
//       if 'end_start_time' is earlier than the current global tick clock, it is
//           interpreted as the current global tick clock.
//       samples that do not end before 'end_start_time + end_duration' will not
//         stop playing until that time, and they will be attenuated to 0 during
//         'end_duration'.
//       Note that once you call 'end_blocks', you can free sample memory; mixlow
//         will make a copy of anything not marked 'safe'.
extern void stb_mixlow_end_set(void *handle, Fademode mode, stb_mixint end_start_time, stb_mixint end_duration);

// check if any samples of this sort are present
extern int stb_mixlow_present(void *handle);

extern int stb_mixlow_num_active(void);

extern int        stb_mixhigh_init(stb_mixint max_premix_samples, float time_offset, stb_mixint buffer_size);
extern stb_mixint stb_mixhigh_step(int stb__premix_samples);
extern stb_mixint stb_mixhigh_time(void);
extern void       stb_mixhigh_deinit(void);

#ifdef __cplusplus
}
#endif

#if defined(STB_DEFINE) || defined(STB_DEFINE_DSOUND)

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include <math.h>
#include <assert.h>
#include <stdarg.h>


// assume ~4 blocks per sample--if you do wavetable, it would be much worse
#define MAX_BLOCKS   1000   // 64KB
typedef struct
{
   short *samples;
   float *fsamples;
   int sample_len;
   unsigned char must_free;
   unsigned char safe;
   unsigned char channels;
   unsigned char fadein_mode;
   unsigned char fadeout_mode;
   float first;
   stb_mixint start_time, duration;
   stb_mixint fadein_start, fadein_len;
   stb_mixint fadeout_start, fadeout_len;
   float step;
   float vol;
   float lpan, rpan;
   void *handle;
} stb__block; // 64 bytes

static stb__block stb__bl[MAX_BLOCKS];
static int stb__num_blocks;

// a less leaf-y design would avoid having to copy this into directsound buffers
static short *stb__premix_int;
static float *stb__premix_float;
static stb_mixint stb__premix_size;
static stb_mixint stb__premix_offset, stb__premix_time, stb__premix_len;

static float stb__global_volume=1;
void stb_mixlow_global_volume(float v)
{
   stb__global_volume = v;
   stb__premix_len = 0;
}

static stb_mixint stb__wrap(stb_mixint t)
{
   return (t >= stb__premix_size ? t- stb__premix_size : t);
}

void *stb__pi1,*stb__pf1;

static unsigned int stb__cpuid_features(void)
{
   unsigned int res;
   __asm {
      mov  eax,1
      cpuid
      mov  res,edx
   }
   return res;
}

static int stb__has_sse2, stb__has_sse;
void stb_mixlow_init(stb_mixint stb__premix_samples)
{
   stb_mixint features;
   stb__premix_size   = stb__premix_samples;
   stb__premix_int    = (short *) malloc(sizeof(*stb__premix_int  ) * 2 * stb__premix_samples+15);
   stb__premix_float  = (float *) malloc(sizeof(*stb__premix_float) * 2 * stb__premix_samples+15);

   stb__pi1 = stb__premix_int;
   stb__pf1 = stb__premix_float;

   stb__premix_int   = (short *) (((int) stb__premix_int + 15) & ~15);
   stb__premix_float = (float *) (((int) stb__premix_float + 15) & ~15);

   stb__premix_offset = 0;
   stb__premix_time   = 0;
   stb__premix_len    = 0;
   stb__num_blocks    = 0;
   memset(stb__premix_int, 0, 4*stb__premix_size);

   #ifdef _MSC_VER
   features = stb__cpuid_features();
   if (features & (1 << 26))
      stb__has_sse2 = 1;
   if (features & (1 << 25))
      stb__has_sse = 1;
   #endif
}

void stb_mixlow_deinit(void)
{
   stb_mixlow_reset(0);
   free(stb__pi1);
   free(stb__pf1);
}

static stb_mixint stb__mix_time;

static void end_block(int i)
{
   if (stb__bl[i].must_free) {
      if (stb__bl[i].samples)
         free(stb__bl[i].samples);
      if (stb__bl[i].fsamples)
         free(stb__bl[i].fsamples);
   }
}

// clear all sample data and set current time
void stb_mixlow_reset(stb_mixint curtime)
{
   int i;
   for (i=0; i < stb__num_blocks; ++i)
      end_block(i);
   stb__num_blocks = 0;
   stb__mix_time = curtime;
   stb__premix_time = stb__premix_len = stb__premix_offset = 0;
}

int stb_mixlow_num_active(void)
{
   return stb__num_blocks;
}

stb_mixint stb_mixlow_get_curtime(void)
{
   return stb__mix_time;
}

// advance the current time (discarding sample info)
void stb_mixlow_set_curtime(stb_mixint curtime)
{
   int i;
   if (curtime < stb__mix_time) return;
   stb__mix_time = curtime;
   for (i=0; i < stb__num_blocks; ) {
      if (stb__bl[i].start_time + stb__bl[i].duration <= curtime) {
         end_block(i);
         stb__bl[i] = stb__bl[--stb__num_blocks];
      } else {
         ++i;
      }
   }
   if (stb__premix_time < curtime) {
      if (stb__premix_time + stb__premix_len < curtime) {
         stb__premix_len = stb__premix_offset = 0;
         stb__premix_time = curtime;
      } else {
         stb__premix_offset = stb__wrap(stb__premix_offset + (curtime - stb__premix_time));
         stb__premix_len    = stb__premix_len - (curtime - stb__premix_time);
         stb__premix_time   = curtime;
      }
   }
}

static void stb__premix_to(stb_mixint when);
int stb_mixlow_mix(short *output, stb_mixint start_time, stb_mixint duration)
{
   stb_mixint offset;
   if (start_time + duration <= stb__mix_time) return 0;

   // make sure our premix buffer covers this much
   stb__premix_to(start_time + duration);

   if (start_time < stb__premix_time) return 0;

   // if we didn't mix as much as requested, compute the actual amount
   if (start_time + duration > stb__premix_time + stb__premix_len)
      duration = stb__premix_time + stb__premix_len - start_time;

   // now duration samples at start_time are well-defined

   // compute the offset
   offset = stb__premix_offset + (start_time - stb__premix_time);
   if (offset > stb__premix_size) offset -= stb__premix_size;

   if (duration > stb__premix_size - offset) {
      int first = (stb__premix_size - offset);
      assert((char *) (stb__premix_int+offset*2) + first*4 == (char *) (stb__premix_int + stb__premix_size*2));
      memcpy(output, stb__premix_int + offset*2, first * 4);
      memcpy(output + first * 2, stb__premix_int, (duration - first) * 4);
   } else {
      assert((char *) (stb__premix_int+offset*2) + duration*4 <= (char *) (stb__premix_int + stb__premix_size*2));
      memcpy(output, stb__premix_int + offset*2, duration*4);
   }
   return duration;
}

// make a copy of the sample data
static void stb__copy_samples(stb__block *b)
{
   float f=0;
   void *old = b->samples;
   int num_samples, num_bytes;

   // if the sample is 'safe', then we don't need to copy it
   if (b->safe) return;

   if (old == NULL) old = b->fsamples;
   // @TODO: adjust 'first' and b->samples

   // compute length of buffer in samples
   if (b->step == 1) {
      num_samples = (int) b->first + b->duration;
   } else {
      num_samples = (int) ceil(b->first + b->duration * b->step+1);
   }
   assert(num_samples <= b->sample_len);
   if (num_samples > b->sample_len)
      num_samples = b->sample_len;
   num_bytes = 2 * b->channels * num_samples;
   if (b->fsamples) num_bytes *= 2;

   if (b->must_free == 0) {
      void *data = malloc(num_bytes);
      if (data)
         if (b->samples)
            memcpy(data, b->samples, num_bytes);
         else
            memcpy(data, b->fsamples, num_bytes);
      if (b->must_free) free(old);
      if (b->samples)
         b->samples = (short *) data;
      else
         b->fsamples = (float *) data;
   }
   b->must_free = 1;
}

static void stb__add_to_premix(stb__block *b);
void stb_mixlow_add_playback_raw(short *samples, float *fsamples, int sample_len, int safe,
                         int channels, float first,
                         stb_mixint start_time, stb_mixint duration, float step,
                         int fadein_mode, stb_mixint fadein_start, stb_mixint fadein_len,
                         float vol, float pan,
                         void *handle)
{
   stb__block *b = &stb__bl[stb__num_blocks];
   if (duration == 0) return;
   if (stb__num_blocks == MAX_BLOCKS) { printf("No free pseudo-oscillators!"); return; }
   ++stb__num_blocks;

   // check for null pointer, possibly advanced somewhat
   if (samples == NULL) {
      if (fsamples <= (float *) 65536) return;
   } else {
      if (samples <= (short *) 65536) return;
   }
   b->samples = samples;
   b->fsamples = fsamples;
   b->sample_len = sample_len;
   b->must_free = 0;
   b->safe = safe;
   b->channels = channels;
   b->first = first;
   b->start_time = start_time;
   b->duration = duration;
   b->step = step;
   b->fadein_mode = fadein_mode;
   b->fadein_start = fadein_start;
   b->fadein_len = fadein_len;
   b->fadeout_mode = FADE_none;
   b->fadeout_start = 0xffffffff;
   b->fadeout_len = 0;
   b->vol = vol;
   if (pan == 0) {
      b->lpan = b->rpan = 1;
   } else if (pan < 0) {
      if (pan < -1) pan = -1;
      b->lpan = 1;
      b->rpan = 1 + pan;
   } else {
      if (pan > 1) pan = 1;
      b->rpan = 1;
      b->lpan = 1 - pan;
   }
   b->handle = handle;

   if (handle == NULL)
      stb__copy_samples(b); // if unsafe and not explicitly ended, immediately copy

   if (start_time < stb__premix_time + stb__premix_len) {
      stb__add_to_premix(b);
   }
}

void stb_mixlow_add_playback(short *samples, int sample_len, int safe,
                         int channels, float first,
                         stb_mixint start_time, stb_mixint duration, float step,
                         int fadein_mode, stb_mixint fadein_start, stb_mixint fadein_len,
                         float vol, float pan,
                         void *handle)
{
   assert(samples != NULL);
   if (samples)
      stb_mixlow_add_playback_raw(samples,NULL,sample_len,safe,channels,first,start_time,duration,step,fadein_mode,fadein_start,fadein_len,vol,pan,handle);
}

void stb_mixlow_add_playback_float(float *fsamples, int sample_len, int safe,
                         int channels, float first,
                         stb_mixint start_time, stb_mixint duration, float step,
                         int fadein_mode, stb_mixint fadein_start, stb_mixint fadein_len,
                         float vol, float pan,
                         void *handle)
{
   assert(fsamples != NULL);
   if (fsamples)
      stb_mixlow_add_playback_raw(NULL,fsamples,sample_len,safe,channels,first,start_time,duration,step,fadein_mode,fadein_start,fadein_len,vol,pan,handle);
}

int stb_mixlow_present(void *handle)
{
   int i;
   for (i=0; i < stb__num_blocks; ++i)
      if (stb__bl[i].handle == handle)
         return 1;
   return 0;
}

void stb_mixlow_end_set(void *handle, Fademode mode, stb_mixint end_start_time, stb_mixint end_duration)
{
   int i;
   stb_mixint end_final;
   assert(handle != NULL);
   if (handle == NULL) return;
   
   if (end_start_time == 0) end_start_time = stb__mix_time;
   end_final = end_start_time + end_duration;

   if (end_start_time < stb__premix_time + stb__premix_len) {
      // @TODO: unmix from the existing buffers instead?
      if (end_start_time < stb__premix_time)
         stb__premix_len = 0;
      else
         stb__premix_len = end_start_time - stb__premix_time;
   }

   // first, delete all blocks that have no effect
   for (i=0; i < stb__num_blocks; )
      if (stb__bl[i].handle == handle && stb__bl[i].start_time >= end_final)
         stb__bl[i] = stb__bl[--stb__num_blocks];
      else
         ++i;

   // then go through and reset them all
   for (i=0; i < stb__num_blocks; ++i) {
      stb__block *b = &stb__bl[i];
      if (b->handle == handle) {
         stb__copy_samples(b);
         b->handle = NULL;
         b->fadeout_mode  = mode;
         b->fadeout_start = end_start_time;
         b->fadeout_len   = end_duration;
         if (b->fadeout_start + b->fadeout_len < b->start_time + b->duration)
            b->duration = (b->fadeout_start + b->fadeout_len) - b->start_time;
      }
   }
}

#ifdef _MSC_VER
__forceinline
#elif defined(__cplusplus)
inline
#endif
static int stb__integerize_one(float *z)
{
   int k;
   #ifdef _MSC_VER
   float p = *z;
   _asm {
      fld p
      fistp k
   }
   #else
   k = (int) *z;
   #endif

   if ((stb_mixint) (k + 32768) > 65535) {
      if (k < -32768) k = -32768;
      else k = 32767;
   }
   return k;
}

static void stb__mix_integerize(stb_mixint off, stb_mixint len)
{
   // convert float to int and saturate
   float *output = stb__premix_float + off*2;
   short *outi   = stb__premix_int   + off*2;
   stb_mixint i;

   if (len == 0) return;
   len *= 2;

   while (((int) output | (int) outi) & 0xf) {
      *outi++ = stb__integerize_one(output++);
      if (!--len) break;
   }

   i=0;
   if (len) {
      assert((((int) output) & 0xf) == 0);
      assert((((int) outi) & 0xf) == 0);
   }
   #ifdef _MSC_VER
   if (stb__has_sse2) {
      for (; i+15 < len; i += 16) {
         float *src = &output[i];
         short *dest = &outi[i];
         __asm {
            mov     eax,src
            mov     ebx,dest
            prefetcht0  [eax+512]
            movaps  xmm0,[eax]
            movaps  xmm1,[eax+16]
            movaps  xmm2,[eax+32]
            movaps  xmm3,[eax+48]
            cvttps2dq xmm0,xmm0
            cvttps2dq xmm1,xmm1
            cvttps2dq xmm2,xmm2
            cvttps2dq xmm3,xmm3
            packssdw xmm0,xmm1
            packssdw xmm2,xmm3
            movaps  [ebx],xmm0
            movaps  [ebx+16],xmm2
         }
      }
      __asm emms;
   } else if (stb__has_sse) {
      for (; i+15 < len; i += 16) {
         float *src = &output[i];
         short *dest = &outi[i];
         __asm {
            mov     eax,src
            mov     ebx,dest
            prefetcht0  [eax+512]
            movaps  xmm0,[eax]
            movaps  xmm1,[eax+16]
            movaps  xmm2,[eax+32]
            movaps  xmm3,[eax+48]
            cvttps2pi mm0,xmm0
            shufps    xmm0,xmm0,4eh
            cvttps2pi mm1,xmm1
            shufps    xmm1,xmm1,4eh
            cvttps2pi mm2,xmm2
            shufps    xmm2,xmm2,4eh
            cvttps2pi mm3,xmm3
            shufps    xmm3,xmm3,4eh
            cvttps2pi mm4,xmm0
            cvttps2pi mm5,xmm1
            cvttps2pi mm6,xmm2
            cvttps2pi mm7,xmm3
            packssdw  mm0,mm4
            packssdw  mm1,mm5
            packssdw  mm2,mm6
            packssdw  mm3,mm7
            movq      [ebx   ],mm0
            movq      [ebx+ 8],mm1
            movq      [ebx+16],mm2
            movq      [ebx+24],mm3
         }
      }
      __asm emms;
   }
   #endif
   for (; i < len; ++i)
      outi[i] = stb__integerize_one(&output[i]);
}

static void stb__mix_block(float *output, stb_mixint start_time, stb_mixint len, stb__block *b);
static void mix(stb_mixint t, stb_mixint off, int len)
{
   int i;
   float *output = stb__premix_float + off*2;

   for (i=0; i < len*2; ++i)
      output[i] = 0;

   for (i=0; i < stb__num_blocks; ++i) {
      stb__block *b = &stb__bl[i];
      if (b->start_time < t+len && b->start_time + b->duration >= t)
         stb__mix_block(output, t, len, b);
   }

   stb__mix_integerize(off, len);
}

static void stb__premix_to(stb_mixint when)
{
   if (when < stb__mix_time) return;
   if (when > stb__mix_time + stb__premix_size)
      when = stb__mix_time + stb__premix_size;

   // if the start pos is wrong, clear it so we can reinit
   assert(stb__premix_time == stb__mix_time);

   // determine region we need to mix
   if (stb__premix_time + stb__premix_len < when) {
      stb_mixint t = stb__premix_time + stb__premix_len;
      stb_mixint newlen = when - (stb__premix_time + stb__premix_len);
      stb_mixint offset = stb__wrap(stb__premix_offset + stb__premix_len);
      if (offset + newlen > stb__premix_size) {
         int left = stb__premix_size - offset;
         mix(t, offset, left);
         mix(t+left, 0, newlen-left);
      } else {
         mix(t, offset, newlen);
      }

      stb__premix_len += newlen;
   }
}

void stb__mix_block_short(float *output, stb_mixint start_time, stb_mixint len, stb__block *b, float start, float end, float first)
{
   stb_mixint i;
   float latt,ratt,lstep,rstep;
   if (start == end) {
      if (start == 0) return;
      latt = b->lpan * start;
      ratt = b->rpan * start;
      lstep = rstep = 0;
   } else {
      float vstep = (end - start) / len;
      latt = b->lpan * start;
      ratt = b->rpan * start;
      lstep = b->lpan * vstep;
      rstep = b->rpan * vstep;
   }
   if (b->step == 1) {
      short *data = b->samples + (int) first * b->channels;
      if (b->channels == 2) {
         for (i=0; i < len; ++i) {
            output[0] += data[0] * latt; latt += lstep;
            output[1] += data[1] * ratt; ratt += rstep;
            output += 2;
            data += 2;
         }
         assert(data - 2 < b->samples + b->sample_len*2);
      } else {
         for (i=0; i < len; ++i) {
            assert(latt >= -0.01 && latt <= 1.01);
            assert(ratt >= -0.01 && ratt <= 1.01);
            output[0] += data[0] * latt; latt += lstep;
            output[1] += data[0] * ratt; ratt += rstep;
            output += 2;
            data += 1;
         }
         assert(data - 1 < b->samples + b->sample_len);
      }
   } else {
      float ff = (float) floor(first), step = b->step;
      int ifirst = (int) ff, istep;
      short *data = b->samples + ((int) ff) * b->channels;
      first -= ff;
      istep = (int) floor(step);
      step -= istep;
      if (b->channels == 2) {
         istep *= 2;
         for (i=0; i < len; ++i) {
            output[0] += (data[0] + first*(data[2]-data[0])) * latt; latt += lstep;
            output[1] += (data[1] + first*(data[3]-data[1])) * ratt; ratt += rstep;
            output += 2;
            first += step;
            if (first >= 1) {
               first -= 1;
               data += 2 + istep;
            } else {
               data += istep;
            }
         }
         first -= step;
         data -= istep;
         if (first < 0) {
            data -= 2;
            assert(data+1 < b->samples + b->sample_len * 2);
         } else
            assert(data+1 < b->samples + b->sample_len * 2);
      } else {
         for (i=0; i < len; ++i) {
            float z = data[0] + (data[1]-data[0]) * first;
            output[0] += z * latt; latt += lstep;
            output[1] += z * ratt; ratt += rstep;
            output += 2;
            first += step;
            if (first >= 1) {
               first -= 1;
               data += 1 + istep;
            } else {
               data += istep;
            }
         }
         first -= step;
         data -= istep;
         if (first < 0) data -= 1;
         assert(data+1 < b->samples + b->sample_len);
      }
   }
}

void stb__mix_block_float(float *output, stb_mixint start_time, stb_mixint len, stb__block *b, float start, float end, float first)
{
   stb_mixint i;
   float latt,ratt,lstep,rstep;
   start *= 32767;
   end *= 32767;
   if (start == end) {
      if (start == 0) return;
      latt = b->lpan * start;
      ratt = b->rpan * start;
      lstep = rstep = 0;
   } else {
      float vstep = (end - start) / len;
      latt = b->lpan * start;
      ratt = b->rpan * start;
      lstep = b->lpan * vstep;
      rstep = b->rpan * vstep;
   }
   if (b->step == 1) {
      float *data = b->fsamples + (int) first * b->channels;
      if (b->channels == 2) {
         for (i=0; i < len; ++i) {
            output[0] += data[0] * latt; latt += lstep;
            output[1] += data[1] * ratt; ratt += rstep;
            output += 2;
            data += 2;
         }
         assert(data - 2 < b->fsamples + b->sample_len*2);
      } else {
         for (i=0; i < len; ++i) {
            assert(latt >= -32800 && latt <= 32800);
            assert(ratt >= -32800 && ratt <= 32800);
            output[0] += data[0] * latt; latt += lstep;
            output[1] += data[0] * ratt; ratt += rstep;
            output += 2;
            data += 1;
         }
         assert(data - 1 < b->fsamples + b->sample_len);
      }
   } else {
      float ff = (float) floor(first), step = b->step;
      int ifirst = (int) ff, istep;
      float *data = b->fsamples + ((int) ff) * b->channels;
      first -= ff;
      istep = (int) floor(step);
      step -= istep;
      if (b->channels == 2) {
         istep *= 2;
         for (i=0; i < len; ++i) {
            output[0] += (data[0] + first*(data[2]-data[0])) * latt; latt += lstep;
            output[1] += (data[1] + first*(data[3]-data[1])) * ratt; ratt += rstep;
            output += 2;
            first += step;
            if (first >= 1) {
               first -= 1;
               data += 2 + istep;
            } else {
               data += istep;
            }
         }
         first -= step;
         data -= istep;
         if (first < 0) {
            data -= 2;
            assert(data+1 < b->fsamples + b->sample_len * 2);
         } else
            assert(data+1 < b->fsamples + b->sample_len * 2);
      } else {
         for (i=0; i < len; ++i) {
            float z = data[0] + (data[1]-data[0]) * first;
            output[0] += z * latt; latt += lstep;
            output[1] += z * ratt; ratt += rstep;
            output += 2;
            first += step;
            if (first >= 1) {
               first -= 1;
               data += 1 + istep;
            } else {
               data += istep;
            }
         }
         first -= step;
         data -= istep;
         if (first < 0) data -= 1;
         assert(data+1 < b->fsamples + b->sample_len);
      }
   }
}

void stb__mix_block_base(float *output, stb_mixint start_time, stb_mixint len, stb__block *b, float start, float end, float first)
{
   if (b->samples)
      stb__mix_block_short(output, start_time, len, b, start, end, first);
   else
      stb__mix_block_float(output, start_time, len, b, start, end, first);
}

static float stb__fade(int mode, float t)
{
   float d,r,p=1;
   assert(t >= 0 && t <= 1);
   switch (mode) {
      case FADE_linear:     return t;
      case FADE_equalpower: return 1.57f*t + t*t*(-0.43f*t - 0.14f);
               // a true equal-power curve would be, say, sin(t/(pi/2)) from 0..1
               // we use a cubic that has the same endpoints and first derivative
      case FADE_pulserelease: 
                            // p is a pre-pulse
                            d = (float) fabs((1-t)*20-1);
                            if (d < 1) p = 1 + (1-(3*d*d - 2*d*d*d))*0.2f;
                            else p = 1;
                            r = t*t*t; r=r*r; r *= 0.5f; // pseudo log
                            d = (t < 0.95f) ? 1-(0.95f-t)*16 : 1;
                            return p*(r < d ? d : r);
      case FADE_release:    r = t*t*t; r=r*r; r *= 0.5f; // pseudo log
                            d = 1-(1-t)*15;
                            return r < d ? d : r;
      case FADE_none:
      default:              assert(0); return 1;
   }

   //  equal power approximation: same endpoints, derivatives as sin(x/1.57)
   //     f(t)  = at^3 + bt^2 + ct + d
   //     f'(t) = 3at^2 + 2bt + c
   //     f(0) = 0, f(1) = 1, f'(0) = 1.57, f'(1) = 0
   //     d = 0, c = 1.57, b = -0.14, a = 0.43
}

static float stb__compute_fade(stb__block *b, stb_mixint t)
{
   if (t < b->fadein_start + b->fadein_len) {
      if (t < b->fadein_start) return 0;
      return stb__fade(b->fadein_mode, ((t-b->fadein_start) / (float) b->fadein_len));
   } else if (t > b->fadeout_start) {
      if (t > b->fadeout_start + b->fadeout_len) return 0;
      return stb__fade(b->fadeout_mode, 1-((t-b->fadeout_start) / (float) b->fadeout_len));
   }
   return 1;
}

static void stb__mix_block(float *output, stb_mixint start_time, stb_mixint len, stb__block *b)
{
   stb_mixint tstart, tend;
   float att, vstart, vend;
   float first;

   assert(b->start_time < start_time + len);
   assert(b->start_time + b->duration >= start_time);
   if (b->start_time > start_time) {
      stb_mixint skip = b->start_time - start_time;
      len -= skip;
      output += skip * 2;
      start_time = b->start_time;
   }

   first = b->first + (start_time - b->start_time) * b->step;
   if (b->start_time + b->duration < start_time + len)
      len = (b->start_time + b->duration) - start_time;

   att = b->vol * stb__global_volume;

   #define ENVELOPE_SAMPLE_TIME      441   // 100 times per second
   tstart = start_time;
   vstart = stb__compute_fade(b, tstart) * att;
   tend   = tstart + ENVELOPE_SAMPLE_TIME;
   for (; tend <= start_time+len;) {
      vend = stb__compute_fade(b, tend) * att;
      stb__mix_block_base(output, tstart, tend-tstart, b, vstart, vend, first);
      output += ENVELOPE_SAMPLE_TIME * 2;
      first += ENVELOPE_SAMPLE_TIME * b->step;
      vstart = vend;
      tstart = tend;
      tend += ENVELOPE_SAMPLE_TIME;
   }
   tend = start_time+len;
   if (tstart != tend) {
      vend = stb__compute_fade(b, tend) * att;
      stb__mix_block_base(output, tstart, tend-tstart, b, vstart, vend, first);
   }
}

void stb__add_to_premix(stb__block *b)
{
   if (stb__premix_offset + stb__premix_len > stb__premix_size) {
      int left = stb__premix_size - stb__premix_offset;
      if (b->start_time < stb__premix_time + left && b->start_time + b->duration > stb__premix_time) {
         stb__mix_block(stb__premix_float + stb__premix_offset*2, stb__premix_time, left, b);
         stb__mix_integerize(stb__premix_offset, left);
      }
      if (b->start_time < stb__premix_time + stb__premix_len && b->start_time + b->duration > stb__premix_time+left) {
         stb__mix_block(stb__premix_float, stb__premix_time+left, stb__premix_len - left, b);
         stb__mix_integerize(0, stb__premix_len - left);
      }
   } else {
      if (b->start_time < stb__premix_time + stb__premix_len && b->start_time + b->duration > stb__premix_time) {
         stb__mix_block(stb__premix_float + stb__premix_offset*2, stb__premix_time, stb__premix_len, b);
         stb__mix_integerize(stb__premix_offset, stb__premix_len);
      }
   }
}


#ifdef STB_DEFINE_DSOUND

#ifndef __cplusplus
#error "Must compile dsound wrapper as C++"
#endif

static int  stb_dsound_init(void *hwnd, int samples_per_second, int buffer_bytes);
static void stb_dsound_deinit(void);
static void stb_dsound_getcursors(int *play, int *write);
static void stb_dsound_writesound(int write_loc_bytes, int size_in_bytes, void *data);

#include <windows.h>
#include <mmsystem.h>
#include <assert.h>
#include <stdio.h>
#include <dsound.h>

#pragma comment(lib, "dsound.lib")

static LPDIRECTSOUND stb__directsound;
static LPDIRECTSOUNDBUFFER stb__buffer;
static int stb__buffer_length;

int stb_dsound_init(void *hwnd, int samples_per_second, int buffer_bytes)
{
   WAVEFORMATEX wfx;
   HRESULT hr;

   hr = DirectSoundCreate(NULL, &stb__directsound, NULL);
   if (hr != DS_OK) {
      return 0;
   }

   memset(&wfx, 0, sizeof(wfx));
   wfx.wFormatTag = WAVE_FORMAT_PCM; 
   wfx.nChannels = 2;
   wfx.nSamplesPerSec = samples_per_second;
   wfx.wBitsPerSample = 16; 
   wfx.nBlockAlign = wfx.wBitsPerSample / 8 * wfx.nChannels;
   wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

   if (hwnd == NULL) hwnd = GetDesktopWindow();
   hr = stb__directsound->SetCooperativeLevel((HWND) hwnd, DSSCL_PRIORITY);
   if (hr != DS_OK) {
      hr = stb__directsound->SetCooperativeLevel((HWND) hwnd, DSSCL_NORMAL);
   } else {
      DSBUFFERDESC primary_buff_desc;
      memset(&primary_buff_desc, 0, sizeof(primary_buff_desc));
      primary_buff_desc.dwSize = sizeof(primary_buff_desc);
      primary_buff_desc.dwFlags = DSBCAPS_PRIMARYBUFFER;
      primary_buff_desc.lpwfxFormat = 0;
      IDirectSoundBuffer* Ppsb = 0;
		hr = stb__directsound->CreateSoundBuffer(&primary_buff_desc, &Ppsb, NULL);
		if (hr == DS_OK) {
   		hr = Ppsb->SetFormat(&wfx);
      }
   }

   stb__buffer_length = buffer_bytes;
	DSBUFFERDESC secondary_buff_desc;
	memset(&secondary_buff_desc, 0, sizeof(secondary_buff_desc));
	secondary_buff_desc.dwSize = sizeof(DSBUFFERDESC);
	secondary_buff_desc.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
	secondary_buff_desc.dwBufferBytes = buffer_bytes;
	secondary_buff_desc.lpwfxFormat = &wfx;					//same wave format as primary stb__buffer

	hr = stb__directsound->CreateSoundBuffer(&secondary_buff_desc, &stb__buffer, NULL);
	if (hr != DS_OK) {
		return 0;
   }

   void *pclear;
   DWORD len;
   hr = stb__buffer->Lock(0,0,&pclear, &len, NULL,0, DSBLOCK_ENTIREBUFFER);
   if (hr != DS_OK) {
      return 0;
   }
   memset(pclear, 0, len);
   stb__buffer->Unlock(pclear, len, NULL, 0);
   hr = stb__buffer->SetCurrentPosition(0);
   if (hr != DS_OK)
      return 0;
   hr = stb__buffer->Play(0,0,DSBPLAY_LOOPING);
   if (hr != DS_OK)
      return 0;

   return 1;
}

void stb_dsound_deinit(void)
{
   stb__buffer->Release(); stb__buffer = NULL;
   stb__directsound->Release(); stb__directsound = NULL;
}

void stb_dsound_getcursors(int *play, int *write)
{
   DWORD play2, write2;
   HRESULT hr = stb__buffer->GetCurrentPosition(&play2, &write2);
   if (hr != DS_OK)
      *play = *write = 0;
   else
      *play = play2, *write = write2;
}

void stb_dsound_writesound(int write_loc_bytes, int size_in_bytes, void *data)
{
   assert(write_loc_bytes >= 0);
   assert(write_loc_bytes <= stb__buffer_length);
   if (size_in_bytes > 0) {
      LPVOID write0, write1;
      DWORD  len0  , len1  ;
      HRESULT hr = stb__buffer->Lock(write_loc_bytes, size_in_bytes, &write0, &len0, &write1, &len1, 0);
      if (hr == DSERR_BUFFERLOST) {
         stb__buffer->Restore();
         hr = stb__buffer->Lock(write_loc_bytes, size_in_bytes, &write0, &len0, &write1, &len1, 0);
      }
      if (hr != DS_OK) {
         return;
      }
      memcpy(write0, data, len0);
      if (write1)
         memcpy(write1, (char *) data + len0, len1);
      hr = stb__buffer->Unlock(write0, len0, write1, len1);
   }
}

static int stb__last_write_cursor, stb__write_cursor_offset;
static stb_mixint stb__mixhigh_time;
static int stb__dsound_buffer_size, stb__dsound_buffer_bytes;
static short *stb__mixbuf;

static int stb__wrap(int t)
{
   if (t < 0) return t + stb__dsound_buffer_size;
   return (t > stb__dsound_buffer_size) ? t-stb__dsound_buffer_size : t;
}

static int stb__dist(int early, int late)
{
   if (early > late)
      late += stb__dsound_buffer_size;
   return late - early;
}

static int stb__prev_write;
int stb_mixhigh_init(stb_mixint max_premix_samples, float time_offset, stb_mixint buffer_size)
{
   int dummy;
   const int samples_per_sec = 44100;
   stb_mixlow_init(max_premix_samples);
   if (!stb_dsound_init(NULL, samples_per_sec, buffer_size))
      return 0;
   stb__dsound_buffer_bytes = buffer_size;
   stb__dsound_buffer_size = buffer_size >> 2;
   stb__write_cursor_offset = (int) (time_offset * samples_per_sec);
   stb__mixbuf = (short *) malloc(sizeof(*stb__mixbuf) * 2 * max_premix_samples);
   stb_dsound_getcursors(&dummy, &stb__prev_write);
   stb_mixhigh_step(1);
   return 1;
}

void stb_mixhigh_deinit(void)
{
   free(stb__mixbuf);
   stb__mixbuf = NULL;
   stb_mixlow_deinit();
   stb_dsound_deinit();
   stb__mixhigh_time = 0;
}

stb_mixint stb_mixhigh_step_raw(int stb__premix_samples)
{
   int play, write, advance, d, available, len;

   // find out how much time has passed in sound time
   stb_dsound_getcursors(&play, &write);
   play >>= 2; write >>= 2;
   assert(write < stb__dsound_buffer_size);
   advance = stb__dist(stb__prev_write, write);

   // update our timer and the low-level timer
   stb__mixhigh_time += advance;

   stb_mixlow_set_curtime(stb__mixhigh_time);

   // record for posterity
   stb__prev_write = write;

   // check how much room is in the buffer for writing
   available = stb__dist(write, play) - stb__write_cursor_offset;
   if (available < stb__premix_samples)
      stb__premix_samples = available;

   // mix that much
   len = stb_mixlow_mix(stb__mixbuf, stb__mixhigh_time, stb__premix_samples);

   // cursors may have advanced while we mixed, so get them again
   stb_dsound_getcursors(&play, &write);
   play >>= 2; write >>= 2;
   assert(write < stb__dsound_buffer_size);
   d = stb__dist(stb__prev_write, write);

   // of the 'len' we mixed, skip first d and output len-d
   if (d < len) {
      int where = stb__wrap(write + stb__write_cursor_offset + d*4);
      len -= d;
      stb_dsound_writesound(where*4, len * 4, stb__mixbuf+d*2);
   }

   return stb__mixhigh_time;
}

stb_mixint stb_mixhigh_step(int stb__premix_samples)
{
   // to avoid latency, start by premixing a short buffer
   if (stb__premix_samples > 1200)
      stb_mixhigh_step_raw(800);
   return stb_mixhigh_step_raw(stb__premix_samples);
}

stb_mixint stb_mixhigh_time(void)
{
   return stb__mixhigh_time;
}
#endif // STB_DEFINE_DSOUND
#endif // STB_DEFINE
#endif // STB_INCLUDE_STB_AUDIO_MIXER_H
