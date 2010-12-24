/*  SSNES - A Super Ninteno Entertainment System (SNES) Emulator frontend for libsnes.
 *  Copyright (C) 2010 - Hans-Kristian Arntzen
 *
 *  Some code herein may be based on code found in BSNES.
 * 
 *  SSNES is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  SSNES is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with SSNES.
 *  If not, see <http://www.gnu.org/licenses/>.
 */


#include <stdbool.h>
#include <GL/glfw.h>
#include <samplerate.h>
#include <libsnes.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include "config.h"
#include "driver.h"
#include "file.h"
#include "hqflt/pastlib.h"
#include "hqflt/grayscale.h"
#include "hqflt/bleed.h"
#include "hqflt/ntsc.h"
#include "general.h"


// To avoid continous switching if we hold the button down, we require that the button must go from pressed, unpressed back to pressed to be able to toggle between then.

#define AUDIO_CHUNK_SIZE_BLOCKING 64
#define AUDIO_CHUNK_SIZE_NONBLOCKING 2048 // So we don't get complete line-noise when fast-forwarding audio.
static size_t audio_chunk_size = AUDIO_CHUNK_SIZE_BLOCKING;
void set_fast_forward_button(bool new_button_state)
{
   static bool old_button_state = false;
   static bool syncing_state = false;
   if (new_button_state && !old_button_state)
   {
      syncing_state = !syncing_state;
      if (video_active)
         driver.video->set_nonblock_state(driver.video_data, syncing_state);
      if (audio_active)
         driver.audio->set_nonblock_state(driver.audio_data, (audio_sync) ? syncing_state : true);
      if (syncing_state)
         audio_chunk_size = AUDIO_CHUNK_SIZE_NONBLOCKING;
      else
         audio_chunk_size = AUDIO_CHUNK_SIZE_BLOCKING;
   }
   old_button_state = new_button_state;
}

#if VIDEO_FILTER != FILTER_NONE
static inline void process_frame (uint16_t * restrict out, const uint16_t * restrict in, unsigned width, unsigned height)
{
   int pitch = 1024;
   if ( height == 448 || height == 478 )
      pitch = 512;

   for ( int y = 0; y < height; y++ )
   {
      const uint16_t *src = in + y * pitch;
      uint16_t *dst = out + y * width;

      memcpy(dst, src, width * sizeof(uint16_t));
   }
}
#endif

// libsnes: 0.065
// Format received is 16-bit 0RRRRRGGGGGBBBBB
static void video_frame(const uint16_t *data, unsigned width, unsigned height)
{
   if ( !video_active )
      return;

#if VIDEO_FILTER == FILTER_HQ2X
   uint16_t outputHQ2x[width * height * 2 * 2];
#elif VIDEO_FILTER == FILTER_HQ4X
   uint16_t outputHQ4x[width * height * 4 * 4];
#elif VIDEO_FILTER == FILTER_NTSC
   uint16_t output_ntsc[SNES_NTSC_OUT_WIDTH(width) * height];
#endif

#if VIDEO_FILTER != FILTER_NONE
   uint16_t output[width * height];
   process_frame(output, data, width, height);
#endif

#if VIDEO_FILTER == FILTER_HQ2X
   ProcessHQ2x(output, outputHQ2x);
   if ( !driver.video->frame(driver.video_data, outputHQ2x, width << 1, height << 1, width << 2) )
      video_active = false;
#elif VIDEO_FILTER == FILTER_HQ4X
   ProcessHQ4x(output, outputHQ4x);
   if ( !driver.video->frame(driver.video_data, outputHQ4x, width << 2, height << 2, width << 3) )
      video_active = false;
#elif VIDEO_FILTER == FILTER_GRAYSCALE
   grayscale_filter(output, width, height);
   if ( !driver.video->frame(driver.video_data, output, width, height, width << 1) )
      video_active = false;
#elif VIDEO_FILTER == FILTER_BLEED
   bleed_filter(output, width, height);
   if ( !driver.video->frame(driver.video_data, output, width, height, width << 1) )
      video_active = false;
#elif VIDEO_FILTER == FILTER_NTSC
   ntsc_filter(output_ntsc, output, width, height);
   if ( !driver.video->frame(driver.video_data, output_ntsc, SNES_NTSC_OUT_WIDTH(width), height, SNES_NTSC_OUT_WIDTH(width) << 1) )
      video_active = false;
#else
   if ( !driver.video->frame(driver.video_data, data, width, height, (height == 448 || height == 478) ? 1024 : 2048) )
      video_active = false;
#endif

}

SRC_STATE* source = NULL;
static void audio_sample(uint16_t left, uint16_t right)
{
   if ( !audio_active )
      return;

   static float data[AUDIO_CHUNK_SIZE_NONBLOCKING];
   static int data_ptr = 0;

   data[data_ptr++] = (float)(*(int16_t*)&left)/0x7FFF; 
   data[data_ptr++] = (float)(*(int16_t*)&right)/0x7FFF;

   if ( data_ptr >= audio_chunk_size )
   {
      float outsamples[audio_chunk_size * 16];
      int16_t temp_outsamples[audio_chunk_size * 16];

      SRC_DATA src_data;

      src_data.data_in = data;
      src_data.data_out = outsamples;
      src_data.input_frames = audio_chunk_size / 2;
      src_data.output_frames = audio_chunk_size * 8;
      src_data.end_of_input = 0;
      src_data.src_ratio = (double)out_rate / (double)in_rate;

      src_process(source, &src_data);

      src_float_to_short_array(outsamples, temp_outsamples, src_data.output_frames_gen * 2);

      if ( driver.audio->write(driver.audio_data, temp_outsamples, src_data.output_frames_gen * 4) < 0 )
      {
         fprintf(stderr, "SSNES [ERROR]: Audio backend failed to write. Will continue without sound.\n");
         audio_active = false;
      }

      data_ptr = 0;
   }
}

static void input_poll(void)
{
   driver.input->poll(driver.input_data);
}

static int16_t input_state(bool port, unsigned device, unsigned index, unsigned id)
{
   const struct snes_keybind *binds[] = { snes_keybinds_1, snes_keybinds_2 };
   return driver.input->input_state(driver.input_data, binds, port, device, index, id);
}

static void fill_pathname(char *out_path, char *in_path, const char *replace)
{
   char tmp_path[strlen(in_path) + 1];
   strcpy(tmp_path, in_path);
   char *tok = NULL;
   tok = strrchr(tmp_path, '.');
   if (tok != NULL)
      *tok = '\0';
   strcpy(out_path, tmp_path);
   strcat(out_path, replace);
}

static void print_help(void)
{
   puts("=================================================");
   puts("ssnes: Simple Super Nintendo Emulator (libsnes)");
   puts("=================================================");
   puts("Usage: ssnes [rom file] [-h/--help | -s/--save]");
   puts("\t-h/--help: Show this help message");
   puts("\t-s/--save: Path for save file (*.srm). Required when rom is input from stdin");
#ifdef HAVE_CG
   puts("\t-f/--shader: Path to Cg shader. Will be compiled at runtime.\n");
#endif
   puts("\t-v/--verbose: Verbose logging");
}

static FILE* rom_file = NULL;
static char savefile_name_srm[256] = {0};
bool verbose = false;
#ifdef HAVE_CG
char cg_shader_path[256] = DEFAULT_CG_SHADER;
#endif

static void parse_input(int argc, char *argv[])
{
   if (argc < 2)
   {
      print_help();
      exit(1);
   }

   struct option opts[] = {
      { "help", 0, NULL, 'h' },
      { "save", 1, NULL, 's' },
      { "verbose", 0, NULL, 'v' },
#ifdef HAVE_CG
      { "shader", 1, NULL, 'f' },
#endif
      { NULL, 0, NULL, 0 }
   };

   int option_index = 0;
#ifdef HAVE_CG
   char optstring[] = "hs:vf:";
#else
   char optstring[] = "hs:v";
#endif
   for(;;)
   {
      int c = getopt_long(argc, argv, optstring, opts, &option_index);

      if (c == -1)
         break;

      switch (c)
      {
         case 'h':
            print_help();
            exit(0);

         case 's':
            strncpy(savefile_name_srm, optarg, sizeof(savefile_name_srm));
            savefile_name_srm[sizeof(savefile_name_srm)-1] = '\0';
            break;

#ifdef HAVE_CG
         case 'f':
            strncpy(cg_shader_path, optarg, sizeof(cg_shader_path) - 1);
            break;
#endif

         case 'v':
            verbose = true;
            break;

         case '?':
            print_help();
            exit(1);

         default:
            SSNES_ERR("Error parsing arguments.\n");
            exit(1);
      }
   }

   if (optind < argc)
   {
      char tmp[strlen(argv[optind]) + 1];
      strcpy(tmp, argv[optind]);
      char *dst = strrchr(tmp, '.');
      if (dst)
      {
         *dst = '\0';
         snes_set_cartridge_basename(tmp);
      }
      else
         snes_set_cartridge_basename(tmp);

      SSNES_LOG("Opening file: \"%s\"\n", argv[optind]);
      rom_file = fopen(argv[optind], "rb");
      if (rom_file == NULL)
      {
         SSNES_ERR("Could not open file: \"%s\"\n", optarg);
         exit(1);
      }
      if (strlen(savefile_name_srm) == 0)
         fill_pathname(savefile_name_srm, argv[optind], ".srm");
   }
   else if (strlen(savefile_name_srm) == 0)
   {
      SSNES_ERR("Need savefile argument when reading rom from stdin.\n");
      print_help();
      exit(1);
   }
}

int main(int argc, char *argv[])
{
   snes_init();
   parse_input(argc, argv);

   void *rom_buf;
   ssize_t rom_len = 0;
   if ((rom_len = read_file(rom_file, &rom_buf)) == -1)
   {
      SSNES_ERR("Could not read ROM file.\n");
      exit(1);
   }
   SSNES_LOG("ROM size: %zi bytes\n", rom_len);

   if (rom_file != NULL)
      fclose(rom_file);

   char statefile_name[strlen(savefile_name_srm)+strlen(".state")+1];
   char savefile_name_rtc[strlen(savefile_name_srm)+strlen(".rtc")+1];

   fill_pathname(statefile_name, argv[1], ".state");
   fill_pathname(savefile_name_rtc, argv[1], ".rtc");

   init_drivers();

   snes_set_video_refresh(video_frame);
   snes_set_audio_sample(audio_sample);
   snes_set_input_poll(input_poll);
   snes_set_input_state(input_state);

   if (!snes_load_cartridge_normal(NULL, rom_buf, rom_len))
   {
      SSNES_ERR("ROM file is not valid!\n");
      goto error;
   }

   free(rom_buf);

   unsigned serial_size = snes_serialize_size();
   uint8_t *serial_data = malloc(serial_size);
   if (serial_data == NULL)
   {
      SSNES_ERR("Failed to allocate memory for states!\n");
      goto error;
   }

   load_save_file(savefile_name_srm, SNES_MEMORY_CARTRIDGE_RAM);
   load_save_file(savefile_name_rtc, SNES_MEMORY_CARTRIDGE_RTC);

   ///// TODO: Modular friendly!!!
   for(;;)
   {
      bool quitting = glfwGetKey(GLFW_KEY_ESC) || !glfwGetWindowParam(GLFW_OPENED);
      
      if ( quitting )
         break;

      if ( glfwGetKey( SAVE_STATE_KEY ))
      {
         write_file(statefile_name, serial_data, serial_size);
      }

      else if ( glfwGetKey( LOAD_STATE_KEY ) )
         load_state(statefile_name, serial_data, serial_size);

      else if ( glfwGetKey( TOGGLE_FULLSCREEN ) )
      {
         fullscreen = !fullscreen;
         uninit_drivers();
         init_drivers();
      }

      snes_run();
   }

   save_file(savefile_name_srm, SNES_MEMORY_CARTRIDGE_RAM);
   save_file(savefile_name_rtc, SNES_MEMORY_CARTRIDGE_RTC);

   snes_unload_cartridge();
   snes_term();
   uninit_drivers();
   free(serial_data);

   return 0;

error:
   snes_unload_cartridge();
   snes_term();
   uninit_drivers();

   return 1;
}

