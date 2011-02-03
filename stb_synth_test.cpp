#include <stdlib.h>
#include <memory.h>
#define STB_DEFINE
#define STB_DEFINE_DSOUND
#include "stb_audio_mixer.h"
#include "stb_synth.h"

#pragma warning(disable:4305)

#define BUFFER 88200

int start_time;
void synthesize(int notes, int repeats, float octave, float duration, float pan, float vol, stb_synth_adsr *env, stb_synth_waveform *wave1, stb_synth_waveform *wave2)
{
   static float test[BUFFER];
   int i,j;
   #if 1
   static int pitches[8] = { 0,2,4,5,7,9,11,12 };
   #else
   static int pitches[8] = { 0,4,7,9,12,16,19,23 };
   octave -= 5;
   #endif
   float bps = 3.5;
   int beat_len = (int) (44100 / bps);
   int spacing = beat_len * 16 / (repeats * notes);
   // play 'notes' different random notes
   for (i=0; i < notes; ++i) {
      float pitch = octave + pitches[(rand() >> 3) & 7];
      int len = stb_synth(test, BUFFER, 44100, duration, pitch,1,env,wave1,wave2);
      // repeat each one 'repeats' times (i.e. multiple measures)
      for (j=0; j < repeats; ++j)
         stb_mixlow_add_playback_float(test, len, 0, 1, 0, start_time + (i + j * notes) * spacing, len, 1, 0,0,0,vol,pan,NULL);
   }
}

int main(int argc, char **argv)
{
   stb_synth_adsr adsr = { 0.005,0.05,0.6,0.25 };
   stb_synth_adsr attack = { 0.05,0,1,0.25 };
   stb_synth_adsr bell = { 0.001, 0.2,0,0 };
   stb_synth_waveform triangle = { 0,0.5,0,0 };
   stb_synth_waveform square   = { 0,0,1,0 };
   stb_synth_waveform sq_tri   = { 0,0.25,0.5,0 };
   stb_synth_waveform saw      = { 0,0,0,1 };
   stb_synth_waveform saw2     = { 0,1,0.5,1 };
   stb_synth_waveform square_pw= { 0.85,0,1,0 };
   stb_synth_waveform weird1   = { 0.1f, 0.3f, 0.15f, 1 };
   stb_synth_waveform weird2   = { 0.1f, 0.7f, 0.55f, 1 };

   stb_mixhigh_init(20000,0.005, 88200*4);
   stb_mixlow_global_volume(0.5);

   // 1/8th second into the future so we have time to synthesize
   start_time = stb_mixhigh_time() + 44100/8;

   synthesize(8, 4, 84, 0.4f ,  0.9, 0.25 , &bell, &sq_tri, &triangle);
   synthesize(8, 2, 60, 0.20f, -0.4, 0.95 , &attack, &saw, &saw2);
   synthesize(4, 2, 36, 0.5f ,  0.1, 0.25 , &adsr, &square, NULL);
   synthesize(8, 4, 91, 0.4f , -0.9, 0.10 , &bell, &sq_tri, NULL);
   synthesize(8, 4, 91, 0.4f , -0.9, 0.10 , &bell, &sq_tri, NULL);
   synthesize(8, 4, 91, 0.4f , -0.9, 0.10 , &bell, &sq_tri, NULL);
   synthesize(8, 4, 91, 0.4f , -0.9, 0.10 , &bell, &sq_tri, NULL);
   synthesize(8, 4, 91, 0.4f , -0.9, 0.10 , &bell, &sq_tri, NULL);
   synthesize(8, 4, 91, 0.4f , -0.9, 0.10 , &bell, &sq_tri, NULL);

   while (stb_mixlow_num_active()) {
      stb_mixhigh_step(5000);
      Sleep(1);
   }
   stb_mixhigh_deinit();

   return 0;
}
