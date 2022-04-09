/*
 * PicoDrive
 * (c) Copyright Dave, 2004
 * (C) notaz, 2006-2009
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */

#include <string.h>
#include "ym2612.h"
#include "sn76496.h"
#include "../pico_int.h"
#include "mix.h"
#include "emu2413/emu2413.h"

#define USE_RESAMPLER	1

#if USE_BLIPPER
#include "blipper.h"
#else
#include "resampler.h"
#endif

void (*PsndMix_32_to_16l)(s16 *dest, s32 *src, int count) = mix_32_to_16l_stereo;

// master int buffer to mix to
// +1 for a fill triggered by an instruction overhanging into the next scanline
static s32 PsndBuffer[2*(96000+100)/50+2];

// cdda output buffer
s16 cdda_out_buffer[2*1152];

// sn76496
extern int *sn76496_regs;

// ym2413
#define YM2413_CLK 3579545
OPLL old_opll;
static OPLL *opll = NULL;
unsigned YM2413_reg;

#if USE_BLIPPER
static blipper_t *fmlblip, *fmrblip;
#else
static resampler_t *fmresampler;
#endif

PICO_INTERNAL void PsndInit(void)
{
  opll = OPLL_new(YM2413_CLK, PicoIn.sndRate);
  OPLL_setChipType(opll,0);
  OPLL_reset(opll);
}

PICO_INTERNAL void PsndExit(void)
{
  OPLL_delete(opll);
  opll = NULL;

#if USE_BLIPPER
  blipper_free(fmlblip); fmlblip = NULL;
  blipper_free(fmrblip); fmrblip = NULL;
#else
  resampler_free(fmresampler); fmresampler = NULL;
#endif
}

PICO_INTERNAL void PsndReset(void)
{
  // PsndRerate calls YM2612Init, which also resets
  PsndRerate(0);
  timers_reset();
}

int (*PsndFMUpdate)(s32 *buffer, int length, int stereo, int is_buf_empty);

#if USE_RESAMPLER
// FM polyphase FIR resampling

#if USE_BLIPPER
#define FMFIR_TAPS	11

// resample FM from its native 53267Hz/52781Hz with the blipper library
static u32 ymmulinv;

int YM2612UpdateFIR(s32 *buffer, int length, int stereo, int is_buf_empty)
{
  int mul = Pico.snd.fm_fir_mul, div = Pico.snd.fm_fir_div;
  s32 *p = buffer, *q = buffer;
  int ymlen;
  int ret = 0;

  if (length <= 0) return ret;

  // FM samples needed: (length*div + div-blipper_read_phase(fmlblip)) / mul
  ymlen = ((u64)(length*div + div-blipper_read_phase(fmlblip)) * ymmulinv) >> 32;
  if (ymlen > 0)
    ret = YM2612UpdateOne(p, ymlen, stereo, is_buf_empty);

  if (stereo) {
    blipper_push_long_samples(fmlblip, p  , ymlen, 2, mul);
    blipper_push_long_samples(fmrblip, p+1, ymlen, 2, mul);
    blipper_read_long(fmlblip, q  , blipper_read_avail(fmlblip), 2);
    blipper_read_long(fmrblip, q+1, blipper_read_avail(fmrblip), 2);
  } else {
    blipper_push_long_samples(fmlblip, p  , ymlen, 1, mul);
    blipper_read_long(fmlblip, q  , blipper_read_avail(fmlblip), 1);
  }

  return ret;
}

static void YM2612_setup_FIR(int inrate, int outrate, int stereo)
{
  int mindiff = 999;
  int diff, mul, div;
  int minmult=22, maxmult = 55; // max interpolation factor

  // compute filter ratio with largest multiplier for smallest error
  for (mul = minmult; mul <= maxmult; mul++) {
    div = (inrate*mul + outrate/2) / outrate;
    diff = outrate*div/mul - inrate;
    if (abs(diff) < abs(mindiff)) {
      mindiff = diff;
      Pico.snd.fm_fir_mul = mul;
      Pico.snd.fm_fir_div = div;
      if (abs(mindiff) <= inrate/1000) break;
    }
  }
  ymmulinv = (1ULL << 32) / mul; /* 1/mul in Q32 */
  printf("FM polyphase FIR ratio=%d/%d error=%.3f%%\n",
        Pico.snd.fm_fir_mul, Pico.snd.fm_fir_div, 100.0*mindiff/inrate);

  // create blipper (modified for polyphase resampling). Not really perfect for
  // FM, but has SINC generator, a good window, and computes the filter in Q16.
  blipper_free(fmlblip);
  blipper_free(fmrblip);
  fmlblip = blipper_new(FMFIR_TAPS, 0.85, 8.5, Pico.snd.fm_fir_div, 1000, NULL);
  if (!stereo) return;
  fmrblip = blipper_new(FMFIR_TAPS, 0.85, 8.5, Pico.snd.fm_fir_div, 1000, NULL);
}
#else
#define FMFIR_TAPS	9

// resample FM from its native 53267Hz/52781Hz with polyphase FIR filter
static int ymchans;
static void YM2612Update(s32 *buffer, int length, int stereo)
{
  ymchans = YM2612UpdateOne(buffer, length, stereo, 1);
}

int YM2612UpdateFIR(s32 *buffer, int length, int stereo, int is_buf_empty)
{
  resampler_update(fmresampler, buffer, length, YM2612Update);
  return ymchans;
}

static void YM2612_setup_FIR(int inrate, int outrate, int stereo)
{
  int mindiff = 999;
  int diff, mul, div;
  int minmult = 22, maxmult = 55; // min,max interpolation factor

  // compute filter ratio with largest multiplier for smallest error
  for (mul = minmult; mul <= maxmult; mul++) {
    div = (inrate*mul + outrate/2) / outrate;
    diff = outrate*div/mul - inrate;
    if (abs(diff) < abs(mindiff)) {
      mindiff = diff;
      Pico.snd.fm_fir_mul = mul;
      Pico.snd.fm_fir_div = div;
      if (abs(mindiff) <= inrate/2000) break; // below error limit
    }
  }
  printf("FM polyphase FIR ratio=%d/%d error=%.3f%%\n",
        Pico.snd.fm_fir_mul, Pico.snd.fm_fir_div, 100.0*mindiff/inrate);

  resampler_free(fmresampler);
  fmresampler = resampler_new(FMFIR_TAPS, Pico.snd.fm_fir_mul, Pico.snd.fm_fir_div,
        0.85, 2, 2*inrate/50, stereo);
}
#endif

#else
static u32 hu, hi;
static s32 hh[2*8];
static s32 hm;

int YM2612UpdateAVG(s32 *buffer, int length, int stereo, int is_buf_empty)
{
  int spf = (stereo?2:1);
  int hs = 1 << hm;
  int inlen;
  s32 *p, *q;
  s32 l, r;
  s32 i, n, ch;

  // generate samples in buffer (2*length out of samples at native rate)
  inlen = ((u64)hu + (length<<hm)*(u64)hi) >> 16;
  p = buffer + ((length<<hm)-inlen)*spf, q = buffer;
  ch = YM2612UpdateOne(p+2*spf, inlen, stereo, 1);

#if 0
  // upsampling by linear interpolation. Lowpass, 1st sidelobe @ about -26 dB 
  memcpy(p, hh, 2*spf*sizeof(s32));
  q = buffer + spf;
  if (stereo) {
    for (i = 0; i < length<<hm; i++) {
      n = hu;
      *q++ = (p[0]*((1<<16)-n) + p[2]*n) >> 16;
      *q++ = (p[1]*((1<<16)-n) + p[3]*n) >> 16;
      hu += hi;
      p += (hu >> 16) * 2;
      hu = (u16) hu;
    }
  } else { 
    for (i = 0; i < length<<hm; i++) {
      n = hu;
      *q++ = (p[0]*((1<<16)-n) + p[1]*n) >> 16;
      hu += hi;
      p += (hu >> 16);
      hu = (u16) hu;
    }
  }
  memcpy(hh, p, 2*spf*sizeof(s32));

  p = buffer + spf, q = buffer;
  if (stereo) {
    while (--length >= 0) {
      l = r = 0;
      for (i = 0; i < hs; i += 2) {
        l += *p++, r += *p++;
        l += *p++, r += *p++;
      }
      *q++ = l >> hm, *q++ = r >> hm;
    }
  } else {
    while (--length >= 0) {
      l = 0;
      for (i = 0; i < hs; i += 2) {
        l += *p++;
        l += *p++;
      }
      *q++ = l >> hm;
    }
  }
#else
  // hs-point averaging filter, cutoff 0.5/hs, gain hs. Since only every hs'th
  // output sample is needed, this collapses to a stepwise sum folding hs input
  // samples into 1 output sample. Absolutely one of the worst filters to have,
  // but better than nothing and very fast. Together with the interpolated
  // upsampling in YM2612 it does an acceptable job at attenuating aliasing,
  // albeit with a mediocre passband perfomance (shhhh!).
  memcpy(p, hh, 2*spf*sizeof(s32));
  if (stereo) {
    while (--length >= 0) {
      l = r = 0;
      for (i = hs; i > 0; i -= 1) {
        // linear interpolation upsampler. Lowpass, 1st sidelobe @ about -26 dB
        n = hu;
        l += (p[0]*((1<<16)-n) + p[2]*n) >> 16;
        r += (p[1]*((1<<16)-n) + p[3]*n) >> 16;
        hu += hi;
        n = hu >> 16;
        p += n * 2;
        hu -= n << 16;
      }
      *q++ = l >> hm, *q++ = r >> hm;
    }
  } else {
    while (--length >= 0) {
      l = 0;
      for (i = hs; i > 0; i -= 1) {
        n = hu;
        l += (p[0]*((1<<16)-n) + p[1]*n) >> 16;
        hu += hi;
        n = hu >> 16;
        p += n;
        hu -= n << 16;
      }
      *q++ = l >> hm;
    }
  }
  memcpy(hh, p, 2*spf*sizeof(s32));
#endif

  return ch;
}

static void YM2612_setup_AVG(int ymrate, int inrate, int outrate)
{
  hu = 0; hi = ((u64)ymrate << 16) / inrate;
  for (hm = 0; inrate > outrate; hm++, inrate /= 2) ;
}
#endif

// to be called after changing sound rate or chips
void PsndRerate(int preserve_state)
{
  void *state = NULL;
  int target_fps = Pico.m.pal ? 50 : 60;
  int target_lines = Pico.m.pal ? 313 : 262;
  int ym2612_clock = Pico.m.pal ? OSC_PAL/7 : OSC_NTSC/7;

  if (preserve_state) {
    state = malloc(0x204);
    if (state == NULL) return;
    ym2612_pack_state();
    memcpy(state, YM2612GetRegs(), 0x204);
  }
  if (PicoIn.opt & POPT_EN_FM_FILTER) {
    int ym2612_rate = (ym2612_clock+(6*24)/2) / (6*24);
#if USE_RESAMPLER
    // polyphase FIR resampler, resampling directly from native to output rate
    YM2612Init(ym2612_clock, ym2612_rate,
        ((PicoIn.opt&POPT_DIS_FM_SSGEG) ? 0 : ST_SSG) |
        ((PicoIn.opt&POPT_EN_FM_DAC)    ? ST_DAC : 0));
    YM2612_setup_FIR(ym2612_rate, PicoIn.sndRate, PicoIn.opt & POPT_EN_STEREO);
    PsndFMUpdate = YM2612UpdateFIR;
#else
    // Do linear interpolation upsampling to a multiple of the output rate in
    // YM2612, then downsample by averaging here
    int overrate = PicoIn.sndRate;
    while (overrate < ym2612_rate) overrate *= 2;
    YM2612Init(ym2612_clock, overrate != PicoIn.sndRate ? ym2612_rate : PicoIn.sndRate,
        ((PicoIn.opt&POPT_DIS_FM_SSGEG) ? 0 : ST_SSG) |
        ((PicoIn.opt&POPT_EN_FM_DAC)    ? ST_DAC : 0));
    YM2612_setup_AVG(ym2612_rate, overrate, PicoIn.sndRate);
    PsndFMUpdate = overrate != PicoIn.sndRate ? YM2612UpdateAVG:YM2612UpdateOne;
#endif
  } else {
    YM2612Init(ym2612_clock, PicoIn.sndRate,
        ((PicoIn.opt&POPT_DIS_FM_SSGEG) ? 0 : ST_SSG) |
        ((PicoIn.opt&POPT_EN_FM_DAC)    ? ST_DAC : 0));
    PsndFMUpdate = YM2612UpdateOne;
  }
  if (preserve_state) {
    // feed it back it's own registers, just like after loading state
    memcpy(YM2612GetRegs(), state, 0x204);
    ym2612_unpack_state();
  }

  if (preserve_state) memcpy(state, sn76496_regs, 28*4); // remember old state
  SN76496_init(Pico.m.pal ? OSC_PAL/15 : OSC_NTSC/15, PicoIn.sndRate);
  if (preserve_state) memcpy(sn76496_regs, state, 28*4); // restore old state

  if(opll != NULL){
    if (preserve_state) memcpy(&old_opll, opll, sizeof(OPLL)); // remember old state
    OPLL_setRate(opll, PicoIn.sndRate);
    OPLL_reset(opll);
  }

  if (state)
    free(state);

  // calculate Pico.snd.len
  Pico.snd.len = PicoIn.sndRate / target_fps;
  Pico.snd.len_e_add = ((PicoIn.sndRate - Pico.snd.len * target_fps) << 16) / target_fps;
  Pico.snd.len_e_cnt = 0; // Q16

  // samples per line (Q16)
  Pico.snd.smpl_mult = 65536LL * PicoIn.sndRate / (target_fps*target_lines);
  // samples per z80 clock (Q20)
  Pico.snd.clkl_mult = 16 * Pico.snd.smpl_mult * 15/7 / 488;
  // samples per 44.1 KHz sample
  Pico.snd.cdda_mult = 65536LL * 44100 / PicoIn.sndRate;
  Pico.snd.cdda_div  = 65536LL * PicoIn.sndRate / 44100;

  // clear all buffers
  memset32(PsndBuffer, 0, sizeof(PsndBuffer)/4);
  memset(cdda_out_buffer, 0, sizeof(cdda_out_buffer));
  if (PicoIn.sndOut)
    PsndClear();

  // set mixer
  PsndMix_32_to_16l = (PicoIn.opt & POPT_EN_STEREO) ? mix_32_to_16l_stereo : mix_32_to_16_mono;
  mix_reset(PicoIn.opt & POPT_EN_SNDFILTER ? PicoIn.sndFilterAlpha : 0);

  if (PicoIn.AHW & PAHW_PICO)
    PicoReratePico();
}


PICO_INTERNAL void PsndStartFrame(void)
{
  // compensate for float part of Pico.snd.len
  Pico.snd.len_use = Pico.snd.len;
  Pico.snd.len_e_cnt += Pico.snd.len_e_add;
  if (Pico.snd.len_e_cnt >= 0x10000) {
    Pico.snd.len_e_cnt -= 0x10000;
    Pico.snd.len_use++;
  }
}

PICO_INTERNAL void PsndDoDAC(int cyc_to)
{
  int pos, len;
  int dout = ym2612.dacout;

  // nothing to do if sound is off
  if (!PicoIn.sndOut) return;

  // number of samples to fill in buffer (Q20)
  len = (cyc_to * Pico.snd.clkl_mult) - Pico.snd.dac_pos;

  // update position and calculate buffer offset and length
  pos = (Pico.snd.dac_pos+0x80000) >> 20;
  Pico.snd.dac_pos += len;
  len = ((Pico.snd.dac_pos+0x80000) >> 20) - pos;

  // avoid loss of the 1st sample of a new block (Q rounding issues)
  if (pos+len == 0)
    len = 1, Pico.snd.dac_pos += 0x80000;
  if (len <= 0)
    return;

  if (!PicoIn.sndOut)
    return;

  // fill buffer, applying a rather weak order 1 bessel IIR on the way
  // y[n] = (x[n] + x[n-1])*(1/2) (3dB cutoff at 11025 Hz, no gain)
  // 1 sample delay for correct IIR filtering over audio frame boundaries
  if (PicoIn.opt & POPT_EN_STEREO) {
    s16 *d = PicoIn.sndOut + pos*2;
    // left channel only, mixed ro right channel in mixing phase
    *d++ += Pico.snd.dac_val2; d++;
    while (--len) *d++ += Pico.snd.dac_val, d++;
  } else {
    s16 *d = PicoIn.sndOut + pos;
    *d++ += Pico.snd.dac_val2;
    while (--len) *d++ += Pico.snd.dac_val;
  }
  Pico.snd.dac_val2 = (Pico.snd.dac_val + dout) >> 1;
  Pico.snd.dac_val = dout;
}

PICO_INTERNAL void PsndDoPSG(int cyc_to)
{
  int pos, len;
  int stereo = 0;

  // nothing to do if sound is off
  if (!PicoIn.sndOut) return;

  // number of samples to fill in buffer (Q20)
  len = (cyc_to * Pico.snd.clkl_mult) - Pico.snd.psg_pos;

  // update position and calculate buffer offset and length
  pos = (Pico.snd.psg_pos+0x80000) >> 20;
  Pico.snd.psg_pos += len;
  len = ((Pico.snd.psg_pos+0x80000) >> 20) - pos;

  if (len <= 0)
    return;
  if (!PicoIn.sndOut || !(PicoIn.opt & POPT_EN_PSG))
    return;

  if (PicoIn.opt & POPT_EN_STEREO) {
    stereo = 1;
    pos <<= 1;
  }
  SN76496Update(PicoIn.sndOut + pos, len, stereo);
}

#if 0
PICO_INTERNAL void PsndDoYM2413(int cyc_to)
{
  int pos, len;
  int stereo = 0;
  s16 *buf;

  // nothing to do if sound is off
  if (!PicoIn.sndOut) return;

  // number of samples to fill in buffer (Q20)
  len = (cyc_to * Pico.snd.clkl_mult) - Pico.snd.ym2413_pos;

  // update position and calculate buffer offset and length
  pos = (Pico.snd.ym2413_pos+0x80000) >> 20;
  Pico.snd.ym2413_pos += len;
  len = ((Pico.snd.ym2413_pos+0x80000) >> 20) - pos;

  if (len <= 0)
    return;
  if (!PicoIn.sndOut || !(PicoIn.opt & POPT_EN_YM2413))
    return;

  if (PicoIn.opt & POPT_EN_STEREO) {
    stereo = 1;
    pos <<= 1;
  }

  buf = PicoIn.sndOut + pos;
  while (len-- > 0) {
    int16_t getdata = OPLL_calc(opll) * 3;
    *buf++ += getdata;
    buf += stereo; // only left for stereo, to be mixed to right later
  }
}
#endif

void YM2413_regWrite(unsigned data){
  OPLL_writeIO(opll,0,data);
}
void YM2413_dataWrite(unsigned data){
  OPLL_writeIO(opll,1,data);
}


PICO_INTERNAL void PsndDoFM(int cyc_to)
{
  int pos, len;
  int stereo = 0;

  // nothing to do if sound is off
  if (!PicoIn.sndOut) return;

  // Q20, number of samples since last call
  len = (cyc_to * Pico.snd.clkl_mult) - Pico.snd.fm_pos;

  // update position and calculate buffer offset and length
  pos = (Pico.snd.fm_pos+0x80000) >> 20;
  Pico.snd.fm_pos += len;
  len = ((Pico.snd.fm_pos+0x80000) >> 20) - pos;
  if (len <= 0)
    return;

  // fill buffer
  if (PicoIn.opt & POPT_EN_STEREO) {
    stereo = 1;
    pos <<= 1;
  }
  if (PicoIn.opt & POPT_EN_FM)
    PsndFMUpdate(PsndBuffer + pos, len, stereo, 1);
}

// cdda
static void cdda_raw_update(s32 *buffer, int length, int stereo)
{
  int ret, cdda_bytes;

  cdda_bytes = (length * Pico.snd.cdda_mult >> 16) * 4;

  ret = pm_read_audio(cdda_out_buffer, cdda_bytes, Pico_mcd->cdda_stream);
  if (ret < cdda_bytes) {
    memset((char *)cdda_out_buffer + ret, 0, cdda_bytes - ret);
    Pico_mcd->cdda_stream = NULL;
    return;
  }

  // now mix
  if (stereo) switch (Pico.snd.cdda_mult) {
    case 0x10000: mix_16h_to_32(buffer, cdda_out_buffer, length*2);     break;
    case 0x20000: mix_16h_to_32_s1(buffer, cdda_out_buffer, length*2);  break;
    case 0x40000: mix_16h_to_32_s2(buffer, cdda_out_buffer, length*2);  break;
    default: mix_16h_to_32_resample_stereo(buffer, cdda_out_buffer, length, Pico.snd.cdda_mult);
  } else
    mix_16h_to_32_resample_mono(buffer, cdda_out_buffer, length, Pico.snd.cdda_mult);
}

void cdda_start_play(int lba_base, int lba_offset, int lb_len)
{
  if (Pico_mcd->cdda_type == CT_MP3)
  {
    int pos1024 = 0;

    if (lba_offset)
      pos1024 = lba_offset * 1024 / lb_len;

    mp3_start_play(Pico_mcd->cdda_stream, pos1024);
    return;
  }

  pm_seek(Pico_mcd->cdda_stream, (lba_base + lba_offset) * 2352, SEEK_SET);
  if (Pico_mcd->cdda_type == CT_WAV)
  {
    // skip headers, assume it's 44kHz stereo uncompressed
    pm_seek(Pico_mcd->cdda_stream, 44, SEEK_CUR);
  }
}


PICO_INTERNAL void PsndClear(void)
{
  int len = Pico.snd.len;
  if (Pico.snd.len_e_add) len++;

  // drop pos remainder to avoid rounding errors (not entirely correct though)
  Pico.snd.dac_pos = Pico.snd.fm_pos = Pico.snd.psg_pos = Pico.snd.ym2413_pos = 0;
  if (!PicoIn.sndOut) return;

  if (PicoIn.opt & POPT_EN_STEREO)
    memset32((int *) PicoIn.sndOut, 0, len); // assume PicoIn.sndOut to be aligned
  else {
    s16 *out = PicoIn.sndOut;
    if ((uintptr_t)out & 2) { *out++ = 0; len--; }
    memset32((int *) out, 0, len/2);
    if (len & 1) out[len-1] = 0;
  }
  if (!(PicoIn.opt & POPT_EN_FM))
    memset32(PsndBuffer, 0, PicoIn.opt & POPT_EN_STEREO ? len*2 : len);
}


static int PsndRender(int offset, int length)
{
  s32 *buf32;
  int stereo = (PicoIn.opt & 8) >> 3;
  int fmlen = ((Pico.snd.fm_pos+0x80000) >> 20);
  int daclen = ((Pico.snd.dac_pos+0x80000) >> 20);
  int psglen = ((Pico.snd.psg_pos+0x80000) >> 20);

  buf32 = PsndBuffer+(offset<<stereo);

  pprof_start(sound);

  if (PicoIn.AHW & PAHW_PICO) {
    // XXX ugly hack, need to render sound for interrupts
    s16 *buf16 = PicoIn.sndOut ? PicoIn.sndOut : (s16 *)PsndBuffer;
    PicoPicoPCMUpdate(buf16+(offset<<stereo), length-offset, stereo);
    return length;
  }

  // Fill up DAC output in case of missing samples (Q rounding errors)
  if (length-daclen > 0 && PicoIn.sndOut) {
    s16 *dacbuf = PicoIn.sndOut + (daclen << stereo);
    Pico.snd.dac_pos += (length-daclen) << 20;
    *dacbuf++ += Pico.snd.dac_val2;
    if (stereo) dacbuf++;
    for (daclen++; length-daclen > 0; daclen++) {
      *dacbuf++ += Pico.snd.dac_val;
      if (stereo) dacbuf++;
    }
    Pico.snd.dac_val2 = Pico.snd.dac_val;
  }

  // Add in parts of the PSG output not yet done
  if (length-psglen > 0 && PicoIn.sndOut) {
    s16 *psgbuf = PicoIn.sndOut + (psglen << stereo);
    Pico.snd.psg_pos += (length-psglen) << 20;
    if (PicoIn.opt & POPT_EN_PSG)
      SN76496Update(psgbuf, length-psglen, stereo);
  }

  // Add in parts of the FM buffer not yet done
  if (length-fmlen > 0 && PicoIn.sndOut) {
    s32 *fmbuf = buf32 + ((fmlen-offset) << stereo);
    Pico.snd.fm_pos += (length-fmlen) << 20;
    if (PicoIn.opt & POPT_EN_FM)
      PsndFMUpdate(fmbuf, length-fmlen, stereo, 1);
  }

  // CD: PCM sound
  if (PicoIn.AHW & PAHW_MCD) {
    pcd_pcm_update(buf32, length-offset, stereo);
  }

  // CD: CDDA audio
  // CD mode, cdda enabled, not data track, CDC is reading
  if ((PicoIn.AHW & PAHW_MCD) && (PicoIn.opt & POPT_EN_MCD_CDDA)
      && Pico_mcd->cdda_stream != NULL
      && !(Pico_mcd->s68k_regs[0x36] & 1))
  {
    if (Pico_mcd->cdda_type == CT_MP3)
      mp3_update(buf32, length-offset, stereo);
    else
      cdda_raw_update(buf32, length-offset, stereo);
  }

  if ((PicoIn.AHW & PAHW_32X) && (PicoIn.opt & POPT_EN_PWM))
    p32x_pwm_update(buf32, length-offset, stereo);

  // convert + limit to normal 16bit output
  if (PicoIn.sndOut)
    PsndMix_32_to_16l(PicoIn.sndOut+(offset<<stereo), buf32, length-offset);

  pprof_end(sound);

  return length;
}

PICO_INTERNAL void PsndGetSamples(int y)
{
  static int curr_pos = 0;

  curr_pos  = PsndRender(0, Pico.snd.len_use);

  if (PicoIn.writeSound && PicoIn.sndOut)
    PicoIn.writeSound(curr_pos * ((PicoIn.opt & POPT_EN_STEREO) ? 4 : 2));
  // clear sound buffer
  PsndClear();
}

static int PsndRenderMS(int offset, int length)
{
  int stereo = (PicoIn.opt & 8) >> 3;
  int psglen = ((Pico.snd.psg_pos+0x80000) >> 20);
  int ym2413len = ((Pico.snd.ym2413_pos+0x80000) >> 20);

  if (!PicoIn.sndOut)
    return length;

  pprof_start(sound);

  // Add in parts of the PSG output not yet done
  if (length-psglen > 0) {
    s16 *psgbuf = PicoIn.sndOut + (psglen << stereo);
    Pico.snd.psg_pos += (length-psglen) << 20;
    if (PicoIn.opt & POPT_EN_PSG)
      SN76496Update(psgbuf, length-psglen, stereo);
  }

  if (length-ym2413len > 0) {
    s16 *ym2413buf = PicoIn.sndOut + (ym2413len << stereo);
    Pico.snd.ym2413_pos += (length-ym2413len) << 20;
    int len = (length-ym2413len);
    if (PicoIn.opt & POPT_EN_YM2413){
      while (len-- > 0) {
        int16_t getdata = OPLL_calc(opll) * 3;
        *ym2413buf += getdata;
        ym2413buf += 1<<stereo;
      }
    }
  }

  // upmix to "stereo" if needed
  if (PicoIn.opt & POPT_EN_STEREO) {
    int i;
    s16 *p;
    for (i = length, p = (s16 *)PicoIn.sndOut; i > 0; i--, p+=2)
      *(p + 1) = *p;
  }

  pprof_end(sound);

  return length;
}

PICO_INTERNAL void PsndGetSamplesMS(int y)
{
  static int curr_pos = 0;

  curr_pos  = PsndRenderMS(0, Pico.snd.len_use);

  if (PicoIn.writeSound != NULL && PicoIn.sndOut)
    PicoIn.writeSound(curr_pos * ((PicoIn.opt & POPT_EN_STEREO) ? 4 : 2));
  PsndClear();
}

// vim:shiftwidth=2:ts=2:expandtab
