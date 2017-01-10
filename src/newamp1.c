/*---------------------------------------------------------------------------*\

  FILE........: newamp1.c
  AUTHOR......: David Rowe
  DATE CREATED: Jan 2017

  Quantisation functions for the sinusoidal coder, using "newamp1"
  algorithm that resamples variable rate L [Am} to a fixed rate K then
  VQs.

\*---------------------------------------------------------------------------*/

/*
  Copyright David Rowe 2017

  All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License version 2.1, as
  published by the Free Software Foundation.  This program is
  distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.

*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "defines.h"
#include "phase.h"
#include "quantise.h"
#include "mbest.h"
#include "newamp1.h"

/*---------------------------------------------------------------------------*\

  FUNCTION....: interp_para()
  AUTHOR......: David Rowe
  DATE CREATED: Jan 2017

  General 2nd order parabolic interpolator.  Used splines orginally,
  but this is much simpler and we don't need much accuracy.  Given two
  vectors of points xp and yp, find interpolated values y at points x.

\*---------------------------------------------------------------------------*/

void interp_para(float y[], float xp[], float yp[], int np, float x[], int n)
{
    assert(np >= 3);

    int k,i;
    float xi, x1, y1, x2, y2, x3, y3, a, b;

    k = 0;
    for (i=0; i<n; i++) {
        xi = x[i];

        /* k is index into xp of where we start 3 points used to form parabola */

        while ((xp[k+1] < xi) && (k < (np-3)))
            k++;
    
        x1 = xp[k]; y1 = yp[k]; x2 = xp[k+1]; y2 = yp[k+1]; x3 = xp[k+2]; y3 = yp[k+2];

        //printf("k: %d np: %d i: %d xi: %f x1: %f y1: %f\n", k, np, i, xi, x1, y1);

        a = ((y3-y2)/(x3-x2)-(y2-y1)/(x2-x1))/(x3-x1);
        b = ((y3-y2)/(x3-x2)*(x2-x1)+(y2-y1)/(x2-x1)*(x3-x2))/(x3-x1);
  
        y[i] = a*(xi-x2)*(xi-x2) + b*(xi-x2) + y2;
    }
}


/*---------------------------------------------------------------------------*\

  FUNCTION....: ftomel()
  AUTHOR......: David Rowe
  DATE CREATED: Jan 2017

  Non linear sampling of frequency axis, reducing the "rate" is a
  first step before VQ

\*---------------------------------------------------------------------------*/

float ftomel(float fHz) {
    float mel = floorf(2595.0*log10f(1.0 + fHz/700.0)+0.5);
    return mel;
}

void mel_sample_freqs_kHz(float rate_K_sample_freqs_kHz[], int K)
{
    float mel_start = ftomel(200.0); float mel_end = ftomel(3700.0); 
    float step = (mel_end-mel_start)/(K-1);
    float mel;
    int k;

    mel = mel_start;
    for (k=0; k<K; k++) {
        rate_K_sample_freqs_kHz[k] = 0.7*(pow(10.0, (mel/2595.0)) - 1.0);
        mel += step;
    }
}


/*---------------------------------------------------------------------------*\

  FUNCTION....: resample_const_rate_f()
  AUTHOR......: David Rowe
  DATE CREATED: Jan 2017

  Resample Am from time-varying rate L=floor(pi/Wo) to fixed rate K.

\*---------------------------------------------------------------------------*/

void resample_const_rate_f(MODEL *model, float rate_K_vec[], float rate_K_sample_freqs_kHz[], int K)
{
    int m;
    float AmdB[MAX_AMP+1], rate_L_sample_freqs_kHz[MAX_AMP+1], AmdB_peak;

    /* convert rate L=pi/Wo amplitude samples to fixed rate K */

    AmdB_peak = -100.0;
    for(m=1; m<=model->L; m++) {
        AmdB[m] = 20.0*log10(model->A[m]+1E-16);
        if (AmdB[m] > AmdB_peak) {
            AmdB_peak = AmdB[m];
        }
        rate_L_sample_freqs_kHz[m] = m*model->Wo*4.0/M_PI;
        //printf("m: %d AmdB: %f AmdB_peak: %f  sf: %f\n", m, AmdB[m], AmdB_peak, rate_L_sample_freqs_kHz[m]);
    }
    
    /* clip between peak and peak -50dB, to reduce dynamic range */

    for(m=1; m<=model->L; m++) {
        if (AmdB[m] < (AmdB_peak-50.0)) {
            AmdB[m] = AmdB_peak-50.0;
        }
    }

    interp_para(rate_K_vec, &rate_L_sample_freqs_kHz[1], &AmdB[1], model->L, rate_K_sample_freqs_kHz, K);    
}


/*---------------------------------------------------------------------------*\

  FUNCTION....: rate_K_mbest_encode
  AUTHOR......: David Rowe
  DATE CREATED: Jan 2017

  Two stage rate K newamp1 VQ quantiser using mbest search.

\*---------------------------------------------------------------------------*/

float rate_K_mbest_encode(int *indexes, float *x, float *xq, int ndim, int mbest_entries)
{
  int i, j, n1, n2;
  const float *codebook1 = newamp1vq_cb[0].cb;
  const float *codebook2 = newamp1vq_cb[1].cb;
  struct MBEST *mbest_stage1, *mbest_stage2;
  float target[ndim];
  float w[ndim];
  int   index[MBEST_STAGES];
  float mse, tmp;

  for(i=0; i<ndim; i++)
      w[i] = 1.0;

  mbest_stage1 = mbest_create(mbest_entries);
  mbest_stage2 = mbest_create(mbest_entries);
  for(i=0; i<MBEST_STAGES; i++)
      index[i] = 0;

  /* Stage 1 */

  mbest_search(codebook1, x, w, ndim, newamp1vq_cb[0].m, mbest_stage1, index);
  MBEST_PRINT("Stage 1:", mbest_stage1);

  /* Stage 2 */

  for (j=0; j<mbest_entries; j++) {
      index[1] = n1 = mbest_stage1->list[j].index[0];
      for(i=0; i<ndim; i++)
	  target[i] = x[i] - codebook1[ndim*n1+i];
      mbest_search(codebook2, target, w, ndim, newamp1vq_cb[1].m, mbest_stage2, index);
  }
  MBEST_PRINT("Stage 2:", mbest_stage2);

  n1 = mbest_stage2->list[0].index[1];
  n2 = mbest_stage2->list[0].index[0];
  mse = 0.0;
  for (i=0;i<ndim;i++) {
      tmp = codebook1[ndim*n1+i] + codebook2[ndim*n2+i];
      mse += (x[i]-tmp)*(x[i]-tmp);
      xq[i] = tmp;
  }

  mbest_destroy(mbest_stage1);
  mbest_destroy(mbest_stage2);

  indexes[0] = n1; indexes[1] = n2;;

  return mse;
}


/*---------------------------------------------------------------------------*\

  FUNCTION....: post_filter
  AUTHOR......: David Rowe
  DATE CREATED: Jan 2017

  Post Filter, has a big impact on speech quality after VQ.  When used
  on a mean removed rate K vector, it raises formants, and supresses
  anti-formants.  As it manipulates amplitudes, we normalise energy to
  prevent clipping or large level variations.  pf_gain of 1.2 to 1.5
  (dB) seems to work OK.  Good area for further investigations and
  improvements in speech quality.

\*---------------------------------------------------------------------------*/

void post_filter_newamp1(float vec[], float sample_freq_kHz[], int K, float pf_gain)
{
    int k;

    /*
      vec is rate K vector describing spectrum of current frame lets
      pre-emp before applying PF. 20dB/dec over 300Hz.  Postfilter
      affects energy of frame so we measure energy before and after
      and normalise.  Plenty of room for experiment here as well.
    */
    
    float pre[K];
    float e_before = 0.0;
    float e_after = 0.0;
    for(k=0; k<K; k++) {
        pre[k] = 20.0*log10f(sample_freq_kHz[k]/0.3);
        vec[k] += pre[k];
        e_before += powf(10.0, 2.0*vec[k]/20.0);
        vec[k] *= pf_gain;
        e_after += powf(10.0, 2.0*vec[k]/20.0);        
    }

    float gain = e_after/e_before;
    float gaindB = 10*log10f(gain);
  
    for(k=0; k<K; k++) {
        vec[k] -= gaindB;
        vec[k] -= pre[k];
    }
}


/*---------------------------------------------------------------------------*\

  FUNCTION....: interp_Wo_v
  AUTHOR......: David Rowe
  DATE CREATED: Jan 2017

  Decoder side interpolation of Wo and voicing, to go from 25 Hz
  sample rate used over channle to 100Hz internal sample rate of Codec 2.

\*---------------------------------------------------------------------------*/

void interp_Wo_v(float Wo_[], int voicing_[], float Wo1, float Wo2, int voicing1, int voicing2)
{
    int i;
    int M = 4;

    for(i=0; i<M; i++)
        voicing_[i] = 0;

    if (!voicing1 && !voicing2) {
        for(i=0; i<M; i++)
            Wo_[i] = 2.0*M_PI/100.0;
    }

    if (voicing1 && !voicing2) {
       Wo_[0] = Wo_[1] = Wo1;
       Wo_[2] = Wo_[3] = 2.0*M_PI/100.0;
       voicing_[0] = voicing_[1] = 1;
    }

    if (!voicing1 && voicing2) {
       Wo_[0] = Wo_[1] = 2.0*M_PI/100.0;
       Wo_[2] = Wo_[3] = Wo2;
       voicing_[2] = voicing_[3] = 1;
    }

    if (voicing1 && voicing2) {
        float c;
        for(i=0,c=1.0; i<M; i++,c-=1.0/M) {
            Wo_[i] = Wo1*c + Wo2*(1.0-c);
            voicing_[i] = 1;
        }
    }
}


/*---------------------------------------------------------------------------*\

  FUNCTION....: resample_rate_L
  AUTHOR......: David Rowe
  DATE CREATED: Jan 2017

  Decoder side conversion of rate K vector back to rate L.

\*---------------------------------------------------------------------------*/

void resample_rate_L(MODEL *model, float rate_K_vec[], float rate_K_sample_freqs_kHz[], int K)
{
   float rate_K_vec_term[K+2], rate_K_sample_freqs_kHz_term[K+2];
   float AmdB[MAX_AMP+1], rate_L_sample_freqs_kHz[MAX_AMP+1];
   int m;

   /* terminate either end of the rate K vecs with 0dB points */

   rate_K_vec_term[0] = rate_K_vec_term[K+1] = 0.0;
   rate_K_sample_freqs_kHz_term[0] = 0.0;
   rate_K_sample_freqs_kHz_term[K+1] = 4.0;

   for(int k=0; k<K; k++) {
       rate_K_vec_term[k+1] = rate_K_vec[k];
       rate_K_sample_freqs_kHz_term[k+1] = rate_K_sample_freqs_kHz[k];
  
       //printf("k: %d f: %f rate_K: %f\n", k, rate_K_sample_freqs_kHz[k], rate_K_vec[k]);
   }

   for(m=1; m<=model->L; m++) {
       rate_L_sample_freqs_kHz[m] = m*model->Wo*4.0/M_PI;
   }

   interp_para(&AmdB[1], rate_K_sample_freqs_kHz_term, rate_K_vec_term, K+2, &rate_L_sample_freqs_kHz[1], model->L);    
   for(m=1; m<=model->L; m++) {
       model->A[m] = pow(10.0,  AmdB[m]/20.0);
       // printf("m: %d f: %f AdB: %f A: %f\n", m, rate_L_sample_freqs_kHz[m], AmdB[m], model->A[m]);
   }
}


/*---------------------------------------------------------------------------*\

  FUNCTION....: determine_phase
  AUTHOR......: David Rowe
  DATE CREATED: Jan 2017

  Given a magnitude spectrum determine a phase spectrum, used for
  phase synthesis with newamp1.

\*---------------------------------------------------------------------------*/

void determine_phase(MODEL *model, int Nfft, codec2_fft_cfg fwd_cfg, codec2_fft_cfg inv_cfg)
{
    int i,m,b;
    int Ns = Nfft/2+1;
    float Gdbfk[Ns], sample_freqs_kHz[Ns], phase[Ns];
    float AmdB[MAX_AMP+1], rate_L_sample_freqs_kHz[MAX_AMP+1];

    printf("  AmdB.: ");
    for(m=1; m<=model->L; m++) {
        AmdB[m] = 20.0*log10(model->A[m]);
        rate_L_sample_freqs_kHz[m] = (float)m*model->Wo*4.0/M_PI;
        if (m <=5) {
            printf("%5.2f ", AmdB[m]);
        }
    }
    printf("\n");
    
    for(i=0; i<Ns; i++) {
        sample_freqs_kHz[i] = (FS/1000.0)*(float)i/Nfft;
    }

    interp_para(Gdbfk, &rate_L_sample_freqs_kHz[1], &AmdB[1], model->L, sample_freqs_kHz, Ns);

    printf("  Gdbfk: ");
    for(i=0; i<5; i++) {
        printf("%5.2f ", Gdbfk[i]);
    }
    printf("\n");

    mag_to_phase(phase, Gdbfk, Nfft, fwd_cfg, inv_cfg);

    printf("  b....: ");
    for(m=1; m<=model->L; m++) {
        b = floorf(0.5+m*model->Wo*Nfft/(2.0*M_PI));
        model->phi[m] = phase[b];
        if (m <= 5) {
            printf("%5d ", b);
        }
    }
    printf("\n");
    printf("  phi..: ");
    for(m=1; m<=5; m++) {
        printf("% 5.2f ", model->phi[m]);
    }
    printf("\n");
}
