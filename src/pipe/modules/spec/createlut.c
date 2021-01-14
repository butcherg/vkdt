// this uses the coefficient cube optimiser from the paper:
//
// Wenzel Jakob and Johannes Hanika. A low-dimensional function space for
// efficient spectral upsampling. Computer Graphics Forum (Proceedings of
// Eurographics), 38(2), March 2019. 
//

// run like
// make && ./createlut 512 lut.pfm XYZ && eu lut.pfm -w 1400 -h 1400

// creates spectra.lut (c0*1e5 y l s)/(x y) and abney.lut (x y)/(s l)
#include <math.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "details/lu.h"
#include "details/matrices.h"
#include "clip.h"
#include "inpaint.h"
#include "../o-pfm/half.h"
#include "../../../core/core.h"

// #define BAD_CMF
#ifdef BAD_CMF
// okay let's also hack the cie functions to our taste (or the gpu approximations we'll do)
#define CIE_SAMPLES 30
#define CIE_FINE_SAMPLES 30
#define CIE_LAMBDA_MIN 400.0
#define CIE_LAMBDA_MAX 700.0
#else
/// Discretization of quadrature scheme
#define CIE_SAMPLES 95
#define CIE_LAMBDA_MIN 360.0
#define CIE_LAMBDA_MAX 830.0
#define CIE_FINE_SAMPLES ((CIE_SAMPLES - 1) * 3 + 1)
#endif
#define RGB2SPEC_EPSILON 1e-4
#define MOM_EPS 1e-3

#include "details/cie1931.h"

/// Precomputed tables for fast spectral -> RGB conversion
double lambda_tbl[CIE_FINE_SAMPLES],
       rgb_tbl[3][CIE_FINE_SAMPLES],
       rgb_to_xyz[3][3],
       xyz_to_rgb[3][3],
       xyz_whitepoint[3];

/// Currently supported gamuts
typedef enum Gamut {
    SRGB,
    ProPhotoRGB,
    ACES2065_1,
    ACES_AP1,
    REC2020,
    ERGB,
    XYZ,
} Gamut;

double sigmoid(double x) {
    return 0.5 * x / sqrt(1.0 + x * x) + 0.5;
}

void lookup2d(float *map, int w, int h, int stride, double *xy, float *res)
{
  double x[] = {xy[0] * w, xy[1] * h};
  x[0] = fmax(0.0, fmin(x[0], w-2));
  x[1] = fmax(0.0, fmin(x[1], h-2));
#if 1 // bilin
  double u[2] = {x[0] - (int)x[0], x[1] - (int)x[1]};
  for(int i=0;i<stride;i++)
    res[i] = (1.0-u[0]) * (1.0-u[1]) * map[stride * (w* (int)x[1]    + (int)x[0]    ) + i]
           + (    u[0]) * (1.0-u[1]) * map[stride * (w* (int)x[1]    + (int)x[0] + 1) + i]
           + (    u[0]) * (    u[1]) * map[stride * (w*((int)x[1]+1) + (int)x[0] + 1) + i]
           + (1.0-u[0]) * (    u[1]) * map[stride * (w*((int)x[1]+1) + (int)x[0]    ) + i];
#else // box
  for(int i=0;i<stride;i++)
    res[i] = map[stride * (w*(int)x[1] + (int)x[0]) +i];
#endif
}

void lookup1d(float *map, int w, int stride, double x, float *res)
{
  x = x * w;
  x = fmax(0.0, fmin(x, w-2));
  double u = x - (int)x;
  for(int i=0;i<stride;i++)
    res[i] = (1.0-u) * map[stride * (int)x + i] + u * map[stride * ((int)x+1) + i];
}

double sqrd(double x) { return x * x; }

void cvt_c0yl_c012(const double *c0yl, double *coeffs)
{
  coeffs[0] = c0yl[0];
  coeffs[1] = c0yl[2] * -2.0 * c0yl[0];
  coeffs[2] = c0yl[1] + c0yl[0] * c0yl[2] * c0yl[2];
}

void cvt_c012_c0yl(const double *coeffs, double *c0yl)
{
  // account for normalising lambda:
  double c0 = CIE_LAMBDA_MIN, c1 = 1.0 / (CIE_LAMBDA_MAX - CIE_LAMBDA_MIN);
  double A = coeffs[0], B = coeffs[1], C = coeffs[2];

  double A2 = (double)(A*(sqrd(c1)));
  double B2 = (double)(B*c1 - 2.0*A*c0*(sqrd(c1)));
  double C2 = (double)(C - B*c0*c1 + A*(sqrd(c0*c1)));

  if(fabs(A2) < 1e-12)
  {
    c0yl[0] = c0yl[1] = c0yl[2] = 0.0;
    return;
  }
  // convert to c0 y dom-lambda:
  c0yl[0] = A2;                           // square slope stays
  c0yl[2] = B2 / (-2.0*A2);               // dominant wavelength
  c0yl[1] = C2 - B2*B2 / (4.0 * A2);      // y
}

void quantise_coeffs(double coeffs[3], float out[3])
{
  // account for normalising lambda:
  double c0 = CIE_LAMBDA_MIN, c1 = 1.0 / (CIE_LAMBDA_MAX - CIE_LAMBDA_MIN);
  double A = coeffs[0], B = coeffs[1], C = coeffs[2];

  const double A2 = (A*(sqrd(c1)));
  const double B2 = (B*c1 - 2*A*c0*(sqrd(c1)));
  const double C2 = (C - B*c0*c1 + A*(sqrd(c0*c1)));
  out[0] = (float)A2;
  out[1] = (float)B2;
  out[2] = (float)C2;
}

void init_coeffs(double coeffs[3])
{
  coeffs[0] = 0.0;
  coeffs[1] = 1.0;
  coeffs[2] = 0.0;
}

void clamp_coeffs(double coeffs[3])
{
  double max = fmax(fmax(fabs(coeffs[0]), fabs(coeffs[1])), fabs(coeffs[2]));
  if (max > 1000) {
    for (int j = 0; j < 3; ++j)
      coeffs[j] *= 1000 / max;
  }
#if 0
  // clamp dom lambda to visible range:
  // this will cause the fitter to diverge on the ridge.
  double c0yl[3];
  c0yl[0] = coeffs[0];
  if(fabs(coeffs[0]) < 1e-12) return;

  c0yl[2] = coeffs[1] / (-2.0*coeffs[0]);
  c0yl[1] = coeffs[2] - coeffs[1]*coeffs[1] / (4.0 * coeffs[0]);

  c0yl[2] = CLAMP(c0yl[2], 0.0, 1.0);

  coeffs[0] = c0yl[0];
  coeffs[1] = c0yl[2] * -2.0 * c0yl[0];
  coeffs[2] = c0yl[1] + c0yl[0] * c0yl[2] * c0yl[2];
#endif
}

int check_gamut(double rgb[3])
{
  double xyz[3] = {0.0};
  for(int j=0;j<3;j++)
  for(int i=0;i<3;i++)
    xyz[i] += rgb_to_xyz[i][j] * rgb[j];
  double x = xyz[0] / (xyz[0] + xyz[1] + xyz[2]);
  double y = xyz[1] / (xyz[0] + xyz[1] + xyz[2]);
  return spectrum_outside(x, y);
}

/**
 * This function precomputes tables used to convert arbitrary spectra
 * to RGB (either sRGB or ProPhoto RGB)
 *
 * A composite quadrature rule integrates the CIE curves, reflectance, and
 * illuminant spectrum over each 5nm segment in the 360..830nm range using
 * Simpson's 3/8 rule (4th-order accurate), which evaluates the integrand at
 * four positions per segment. While the CIE curves and illuminant spectrum are
 * linear over the segment, the reflectance could have arbitrary behavior,
 * hence the extra precations.
 */
void init_tables(Gamut gamut) {
    memset(rgb_tbl, 0, sizeof(rgb_tbl));
    memset(xyz_whitepoint, 0, sizeof(xyz_whitepoint));

    const double *illuminant = 0;

    switch (gamut) {
        case SRGB:
            illuminant = cie_d65;
            memcpy(xyz_to_rgb, xyz_to_srgb, sizeof(double) * 9);
            memcpy(rgb_to_xyz, srgb_to_xyz, sizeof(double) * 9);
            break;

        case ERGB:
            illuminant = cie_e;
            memcpy(xyz_to_rgb, xyz_to_ergb, sizeof(double) * 9);
            memcpy(rgb_to_xyz, ergb_to_xyz, sizeof(double) * 9);
            break;

        case XYZ:
            illuminant = cie_e;
            memcpy(xyz_to_rgb, xyz_to_xyz, sizeof(double) * 9);
            memcpy(rgb_to_xyz, xyz_to_xyz, sizeof(double) * 9);
            break;

        case ProPhotoRGB:
            illuminant = cie_d50;
            memcpy(xyz_to_rgb, xyz_to_prophoto_rgb, sizeof(double) * 9);
            memcpy(rgb_to_xyz, prophoto_rgb_to_xyz, sizeof(double) * 9);
            break;

        case ACES2065_1:
            illuminant = cie_d60;
            memcpy(xyz_to_rgb, xyz_to_aces2065_1, sizeof(double) * 9);
            memcpy(rgb_to_xyz, aces2065_1_to_xyz, sizeof(double) * 9);
            break;

        case ACES_AP1:
            illuminant = cie_d60;
            memcpy(xyz_to_rgb, xyz_to_aces_ap1, sizeof(double) * 9);
            memcpy(rgb_to_xyz, aces_ap1_to_xyz, sizeof(double) * 9);
            break;

        case REC2020:
            illuminant = cie_d65;
            memcpy(xyz_to_rgb, xyz_to_rec2020, sizeof(double) * 9);
            memcpy(rgb_to_xyz, rec2020_to_xyz, sizeof(double) * 9);
            break;
    }

    double norm = 0.0, n2[3] = {0.0};
    for (int i = 0; i < CIE_FINE_SAMPLES; ++i) {

#ifndef BAD_CMF
      double h = (CIE_LAMBDA_MAX - CIE_LAMBDA_MIN) / (CIE_FINE_SAMPLES - 1.0);

        double lambda = CIE_LAMBDA_MIN + i * h;
        double xyz[3] = { cie_interp(cie_x, lambda),
                          cie_interp(cie_y, lambda),
                          cie_interp(cie_z, lambda) },
               I = cie_interp(illuminant, lambda);
#else
      double h = (CIE_LAMBDA_MAX - CIE_LAMBDA_MIN) / (double)CIE_FINE_SAMPLES;
        double lambda = CIE_LAMBDA_MIN + (i+0.5) * h;
        // double lambda = CIE_LAMBDA_MIN + i * h;
        double xyz[3] = { cie_interp(cie_x, lambda),
                          cie_interp(cie_y, lambda),
                          cie_interp(cie_z, lambda) };
        const double I = cie_interp(illuminant, lambda);
        // const double cw[3] = {
        //   -9.16167e-06, 0.00870653, -2.35259 // d65 / 3
        //     // 0.00014479, -0.189595, 62.5251 // d65 / 1.1
        //     // 0, 0, 10000 // illum E
        //     //  -14.1899, 13622.4, -3.26377e+06
        //     // -8.2609e-05, 0.0823704, -25.0921
        //     // 0.00395809, -4.02143, 1021.5
        //     // -9.12318e-05, 0.0924729, -27.9558
        //     // 0.0691431, -74.2713, 19943.2 // says rec2020
        //     //  0.0871685, -94.3229, 25511.3 // says xyz
        // };
#endif
        norm += I;

#ifndef BAD_CMF
        double weight = 3.0 / 8.0 * h;
        if (i == 0 || i == CIE_FINE_SAMPLES - 1)
            ;
        else if ((i - 1) % 3 == 2)
            weight *= 2.f;
        else
            weight *= 3.f;
#else
        double weight = h;
#endif

#if 0 // output table for shader code
        double out[3] = {0.0};
        for (int k = 0; k < 3; ++k)
            for (int j = 0; j < 3; ++j)
                out[k] += xyz_to_rgb[k][j] * xyz[j];
        fprintf(stderr, "vec3(%g, %g, %g), // %g nm\n", out[0], out[1], out[2], lambda);
#endif
        lambda_tbl[i] = lambda;
        for (int k = 0; k < 3; ++k)
            for (int j = 0; j < 3; ++j)
                rgb_tbl[k][i] += xyz_to_rgb[k][j] * xyz[j] * I * weight;

        for (int k = 0; k < 3; ++k)
            xyz_whitepoint[k] += xyz[k] * I * weight;
        for (int k = 0; k < 3; ++k)
            n2[k] += xyz[k] * weight;
    }
}

void eval_residual(const double *coeff, const double *rgb, double *residual)
{
    double out[3] = { 0.0, 0.0, 0.0 };

    for (int i = 0; i < CIE_FINE_SAMPLES; ++i)
    {
      // the optimiser doesn't like nanometers.
      // we'll do the normalised lambda thing and later convert when we write out.
#ifdef BAD_CMF
      double lambda = (i+.5)/(double)CIE_FINE_SAMPLES;//(lambda_tbl[i] - CIE_LAMBDA_MIN) / (CIE_LAMBDA_MAX - CIE_LAMBDA_MIN); /* Scale lambda to 0..1 range */
#else
      double lambda = i/(double)CIE_FINE_SAMPLES;//(lambda_tbl[i] - CIE_LAMBDA_MIN) / (CIE_LAMBDA_MAX - CIE_LAMBDA_MIN); /* Scale lambda to 0..1 range */
#endif
      double cf[3] = {coeff[0], coeff[1], coeff[2]};

      /* Polynomial */
      double x = 0.0;
      for (int i = 0; i < 3; ++i)
        x = x * lambda + cf[i];

      /* Sigmoid */
      double s = sigmoid(x);

      /* Integrate against precomputed curves */
      for (int j = 0; j < 3; ++j)
        out[j] += rgb_tbl[j][i] * s;
    }
    memcpy(residual, rgb, sizeof(double) * 3);

    for (int j = 0; j < 3; ++j)
        residual[j] -= out[j];
}

void eval_jacobian(const double *coeffs, const double *rgb, double **jac) {
    double r0[3], r1[3], tmp[3];

    for (int i = 0; i < 3; ++i) {
        memcpy(tmp, coeffs, sizeof(double) * 3);
        tmp[i] -= RGB2SPEC_EPSILON;
        eval_residual(tmp, rgb, r0);

        memcpy(tmp, coeffs, sizeof(double) * 3);
        tmp[i] += RGB2SPEC_EPSILON;
        eval_residual(tmp, rgb, r1);

        for(int j=0;j<3;j++) assert(r1[j] == r1[j]);
        for(int j=0;j<3;j++) assert(r0[j] == r0[j]);

        for (int j = 0; j < 3; ++j)
            jac[j][i] = (r1[j] - r0[j]) * 1.0 / (2 * RGB2SPEC_EPSILON);
    }
}

double gauss_newton(const double rgb[3], double coeffs[3])
{
  int it = 40;//15;
    double r = 0;
    for (int i = 0; i < it; ++i) {
        double J0[3], J1[3], J2[3], *J[3] = { J0, J1, J2 };

        double residual[3];

        clamp_coeffs(coeffs);
        eval_residual(coeffs, rgb, residual);
        eval_jacobian(coeffs, rgb, J);

        int P[4];
        int rv = LUPDecompose(J, 3, 1e-15, P);
        if (rv != 1) {
          fprintf(stdout, "RGB %g %g %g -> %g %g %g\n", rgb[0], rgb[1], rgb[2], coeffs[0], coeffs[1], coeffs[2]);
          fprintf(stdout, "J0 %g %g %g\n", J0[0], J0[1], J0[2]);
          fprintf(stdout, "J1 %g %g %g\n", J1[0], J1[1], J1[2]);
          fprintf(stdout, "J2 %g %g %g\n", J2[0], J2[1], J2[2]);
          return 666.0;
        }

        double x[3];
        LUPSolve(J, P, residual, 3, x);

        r = 0.0;
        for (int j = 0; j < 3; ++j) {
            coeffs[j] -= x[j];
            r += residual[j] * residual[j];
        }

        if (r < 1e-6)
            break;
    }
    return sqrt(r);
}

static Gamut parse_gamut(const char *str)
{
  if(!strcasecmp(str, "sRGB"))
    return SRGB;
  if(!strcasecmp(str, "eRGB"))
    return ERGB;
  if(!strcasecmp(str, "XYZ"))
    return XYZ;
  if(!strcasecmp(str, "ProPhotoRGB"))
    return ProPhotoRGB;
  if(!strcasecmp(str, "ACES2065_1"))
    return ACES2065_1;
  if(!strcasecmp(str, "ACES_AP1"))
    return ACES_AP1;
  if(!strcasecmp(str, "REC2020"))
    return REC2020;
  return SRGB;
}

int main(int argc, char **argv)
{
  if (argc < 3)
  {
    printf("syntax: createlut <resolution> <output> [<gamut>]\n"
        "where <gamut> is one of sRGB,eRGB,XYZ,ProPhotoRGB,ACES2065_1,ACES_AP1,REC2020\n");
    exit(-1);
  }
  Gamut gamut = XYZ;
  if(argc > 3) gamut = parse_gamut(argv[3]);
  init_tables(gamut);

  const int res = atoi(argv[1]); // resolution of 2d lut

  printf("optimising ");

  typedef struct header_t
  {
    uint32_t magic;
    uint16_t version;
    uint8_t  channels;
    uint8_t  datatype;
    uint32_t wd;
    uint32_t ht;
  }
  header_t;

  // read max macadam brightness lut
  int max_w, max_h;
  float *max_b = 0;
  {
    FILE *f = fopen("macadam.lut", "rb");
    header_t header;
    if(!f) goto mac_error;
    if(fread(&header, sizeof(header_t), 1, f) != 1) goto mac_error;
    max_w = header.wd;
    max_h = header.ht;
    if(header.channels != 1) goto mac_error;
    if(header.version != 2) goto mac_error;
    max_b = calloc(sizeof(float), max_w*max_h);
    uint16_t *half = calloc(sizeof(float), max_w*max_h);
    fread(half, header.wd*header.ht, sizeof(uint16_t), f);
    fclose(f);
    for(int k=0;k<header.wd*header.ht;k++)
      max_b[k] = half_to_float(half[k]);
    free(half);
    if(0)
    {
mac_error:
      if(f) fclose(f);
      fprintf(stderr, "could not read macadam.lut!\n");
      exit(2);
    }
  }

  int lsres = res/4;
  float *lsbuf = calloc(sizeof(float), 5*lsres*lsres);

  size_t bufsize = 5*res*res;
  float *out = calloc(sizeof(float), bufsize);
#if defined(_OPENMP)
#pragma omp parallel for schedule(dynamic) shared(stdout,out,max_b,max_w,max_h)
#endif
  for (int j = 0; j < res; ++j)
  {
    // const double y = (j+0.5) / (double)res;
    const double y = (j) / (double)res;
    printf(".");
    fflush(stdout);
    for (int i = 0; i < res; ++i)
    {
      // const double x = (i+0.5) / (double)res;
      const double x = (i) / (double)res;
      double rgb[3];
      double coeffs[3];
      init_coeffs(coeffs);
      // normalise to max(rgb)=1
      rgb[0] = x;
      rgb[1] = y;
      rgb[2] = 1.0-x-y;
      if(check_gamut(rgb)) continue;

      int ii = (int)fmin(max_w - 1, fmax(0, i * (max_w / (double)res)));
      int jj = (int)fmin(max_h - 1, fmax(0, j * (max_h / (double)res)));
      double m = fmax(0.001, 0.5*max_b[ii + max_w * jj]);
      double rgbm[3] = {rgb[0] * m, rgb[1] * m, rgb[2] * m};
      double resid = gauss_newton(rgbm, coeffs);

      double c0yl[3];
      cvt_c012_c0yl(coeffs, c0yl);

#if 0
      // now compute round trip error, to be sure:
      // consider *1e5 -> half -> float -> *1e-5
      float unq[3] = {
        1e-5*half_to_float(float_to_half(c0yl[0] * 1e5)),
        half_to_float(float_to_half(c0yl[1])),
        half_to_float(float_to_half(c0yl[2]))};
      // float unq[3] = { c0yl[0], c0yl[1], c0yl[2]};
#if 0
      double rgb_out[3] = { 0.0, 0.0, 0.0 };
      for (int k = 0; k < CIE_FINE_SAMPLES; ++k)
      {
        double lambda = (k+.5)/(double)CIE_FINE_SAMPLES*(CIE_LAMBDA_MAX-CIE_LAMBDA_MIN) + CIE_LAMBDA_MIN;
        // double x = c0yl[0] * (lambda - c0yl[2])*(lambda - c0yl[2]) + c0yl[1];
        double x = unq[0] * (lambda - unq[2])*(lambda - unq[2]) + unq[1];
        double s = sigmoid(x);
        for (int c = 0; c < 3; ++c)
          rgb_out[c] += rgb_tbl[c][k] * s;
      }
#else
      float rgb_out[3] = { 0.0, 0.0, 0.0 };
      for (int k = 0; k < CIE_FINE_SAMPLES; ++k)
      {
        float lambda = (k+.5f)/(float)CIE_FINE_SAMPLES*(CIE_LAMBDA_MAX-CIE_LAMBDA_MIN) + CIE_LAMBDA_MIN;
        // double x = c0yl[0] * (lambda - c0yl[2])*(lambda - c0yl[2]) + c0yl[1];
        float x = unq[0] * (lambda - unq[2])*(lambda - unq[2]) + unq[1];
        float s = sigmoid(x);
        for (int c = 0; c < 3; ++c)
          rgb_out[c] += rgb_tbl[c][k] * s;
      }
#endif
      double resid2 = sqrt(
          pow(rgb_out[0]-rgbm[0], 2)+
          pow(rgb_out[1]-rgbm[1], 2)+
          pow(rgb_out[2]-rgbm[2], 2));
      // TODO output resid and resid2 and go all alarm!!
      if(resid2 > resid && resid2 > 0.1)
        fprintf(stderr, "\nerrors %g %g -- rgb %g %g %g -- coeffs %g %g %g -- %g %g %g\n", resid, resid2,
            rgb_out[0], rgb_out[1], rgb_out[2],
            c0yl[0], c0yl[1], c0yl[2],
            unq[0], unq[1], unq[2]);

      int idx = j*res + i;
      out[5*idx + 0] = resid;//coeffs[0];
      out[5*idx + 1] = resid2;//coeffs[1];
      out[5*idx + 2] = coeffs[2];
#else
      (void)resid;
      int idx = j*res + i;
      out[5*idx + 0] = coeffs[0];
      out[5*idx + 1] = coeffs[1];
      out[5*idx + 2] = coeffs[2];
#endif

      float xy[2] = {x, y}, white[2] = {1.0f/3.0f, 1.0f/3.0f}; // illum E //{.3127266, .32902313}; // D65
      float sat = spectrum_saturation(xy, white);

      // bin into lambda/saturation buffer
      float satc = lsres * sat;
      // normalise to extended range:
      float norm = (c0yl[2] - 400.0)/(700.0-400.0);
      // float lamc = 1.0/(1.0+exp(-1.0*(-4.0 + 8.0*norm))) * lsres / 2;
      // float lamc = 1.0/(1.0+exp(-1.0*(2.0*norm-1.0))) * lsres / 2; // this is good for the tails, but bad for the resolution of regular colours
      float lamc = 1.0/(1.0+exp(-2.0*(2.0*norm-1.0))) * lsres / 2; // center deriv=1
      // float lamc = (c0yl[2] - 300.0)/(900.0-300.0) * lsres / 2;
      // cie range
      // float lamc = (c0yl[2] - CIE_LAMBDA_MIN)/(CIE_LAMBDA_MAX-CIE_LAMBDA_MIN) * lsres / 2;
      int lami = fmaxf(0, fminf(lsres/2-1, lamc));
      int sati = satc;
      if(c0yl[0] > 0) lami += lsres/2;
      lami = fmaxf(0, fminf(lsres-1, lami));
      sati = fmaxf(0, fminf(lsres-1, sati));
      float olamc = lsbuf[5*(lami*lsres + sati)+3];
      float osatc = lsbuf[5*(lami*lsres + sati)+4];
      float odist = 
        (olamc - lami - 0.5f)*(olamc - lami - 0.5f)+
        (osatc - sati - 0.5f)*(osatc - sati - 0.5f);
      float  dist = 
        ( lamc - lami - 0.5f)*( lamc - lami - 0.5f)+
        ( satc - sati - 0.5f)*( satc - sati - 0.5f);
      if(dist < odist)
      {
        lsbuf[5*(lami*lsres + sati)+0] = x;
        lsbuf[5*(lami*lsres + sati)+1] = y;
        lsbuf[5*(lami*lsres + sati)+2] = 1.0-x-y;
        lsbuf[5*(lami*lsres + sati)+3] = lamc;
        lsbuf[5*(lami*lsres + sati)+4] = satc;
      }
      out[5*idx + 3] = (lami+0.5f) / (float)lsres;
      out[5*idx + 4] = (sati+0.5f) / (float)lsres;
    }
  }

#ifndef BAD_CMF // don't overwrite for simplified cmf
  { // scope write abney map on (lambda, saturation)
    buf_t inpaint_buf = {
      .dat = lsbuf,
      .wd  = lsres,
      .ht  = lsres,
      .cpp = 5,
    };
    inpaint(&inpaint_buf);

    // determine gamut boundaries for rec709 and rec2020:
    // walk each row and find first time it goes outside.
    // record this in special 1d tables
    float *bound_rec709  = calloc(sizeof(float), lsres);
    float *bound_rec2020 = calloc(sizeof(float), lsres);
    for(int j=0;j<lsres;j++)
    {
      int active = 3;
      for(int i=0;i<lsres;i++)
      {
        int idx = j*lsres + i;
        double xyz[] = {lsbuf[5*idx], lsbuf[5*idx+1], 1.0-lsbuf[5*idx]-lsbuf[5*idx+1]};
        double rec709 [3] = {0.0};
        double rec2020[3] = {0.0};
        for (int k = 0; k < 3; ++k)
          for (int l = 0; l < 3; ++l)
            rec709[k] += xyz_to_srgb[k][l] * xyz[l];
        for (int k = 0; k < 3; ++k)
          for (int l = 0; l < 3; ++l)
            rec2020[k] += xyz_to_rec2020[k][l] * xyz[l];
        if((active & 1) && (rec709 [0] < 0 || rec709 [1] < 0 || rec709 [2] < 0))
        {
          bound_rec709[j] = (i-.5f)/(float)lsres;
          active &= ~1;
        }
        if((active & 2) && (rec2020[0] < 0 || rec2020[1] < 0 || rec2020[2] < 0))
        {
          bound_rec2020[j] = (i-.5f)/(float)lsres;
          active &= ~2;
        }
        if(!active) break;
      }
    }

    // write 2 channel half lut:
    uint32_t size = 2*sizeof(uint16_t)*lsres*(lsres+1);
    uint16_t *b16 = malloc(size);
    // also write pfm for debugging purposes
    FILE *pfm = fopen(argv[2], "wb");
    if(pfm) fprintf(pfm, "PF\n%d %d\n-1.0\n", lsres+1, lsres);
    for(int j=0;j<lsres;j++)
    {
      for(int i=0;i<lsres;i++)
      {
        int ki = j*lsres + i, ko = j*(lsres+1) + i;
        b16[2*ko+0] = float_to_half(lsbuf[5*ki+0]);
        b16[2*ko+1] = float_to_half(lsbuf[5*ki+1]);
        float q[] = {lsbuf[5*ki], lsbuf[5*ki+1], 1.0f-lsbuf[5*ki]-lsbuf[5*ki+1]};
        if(pfm) fwrite(q, sizeof(float), 3, pfm);
      }
      b16[2*(j*(lsres+1)+lsres)+0] = float_to_half(bound_rec709 [j]);
      b16[2*(j*(lsres+1)+lsres)+1] = float_to_half(bound_rec2020[j]);
      float q[] = {bound_rec709[j], bound_rec2020[j], 0.0f};
      if(pfm) fwrite(q, sizeof(float), 3, pfm);
    }
    header_t head = (header_t) {
      .magic    = 1234,
      .version  = 2,
      .channels = 2,
      .datatype = 0,
      .wd       = lsres+1,
      .ht       = lsres,
    };
    FILE *f = fopen("abney.lut", "wb");
    if(f)
    {
      fwrite(&head, sizeof(head), 1, f);
      fwrite(b16, size, 1, f);
      fclose(f);
    }
    free(b16);
    free(bound_rec709);
    free(bound_rec2020);
    if(pfm) fclose(pfm);
  }
#endif

#ifdef BAD_CMF // write four channel half lut only for abridged cmf
  { // write spectra map: (x,y) |--> sigmoid coeffs + saturation
    uint32_t size = 4*sizeof(uint16_t)*res*res;
    uint16_t *b16 = malloc(size);
    // also write pfm for debugging purposes
    FILE *pfm = fopen(argv[2], "wb");
    if(pfm) fprintf(pfm, "PF\n%d %d\n-1.0\n", res, res);
    for(int k=0;k<res*res;k++)
    {
      double coeffs[3] = {out[5*k+0], out[5*k+1], out[5*k+2]};
      // double c0yl[3]; // this is okay in half float
      // cvt_c012_c0yl(coeffs, c0yl);
      // float q[4] = {1e5f*c0yl[0], c0yl[1], c0yl[2], out[5*k+4]};
      // q[0] is okay in half float, saturation too. the rest not so much :(
      float q[4] = {coeffs[0], coeffs[1], coeffs[2], out[5*k+4]};
      quantise_coeffs(coeffs, q);
      // q[0] *= 1e5; q[1] *= 1e-1; q[2] *= 1e1;
      // if(fabsf(q[0]) > 0) fprintf(stderr, "%g %g %g\n", q[0], q[1], q[2]); // XXX DEBUG
      if(pfm) fwrite(q, sizeof(float), 3, pfm);
      b16[4*k+0] = float_to_half(q[0]);
      b16[4*k+1] = float_to_half(q[1]);
      b16[4*k+2] = float_to_half(q[2]);
      b16[4*k+3] = float_to_half(q[3]);
    }
    header_t head = (header_t) {
      .magic    = 1234,
      .version  = 2,
      .channels = 4,
      .datatype = 1,  // 32-bit float
      .wd       = res,
      .ht       = res,
    };
    FILE *f = fopen("spectra.lut", "wb");
    if(f)
    {
      fwrite(&head, sizeof(head), 1, f);
      for(int k=0;k<res*res;k++)
      {
        double coeffs[3] = {out[5*k+0], out[5*k+1], out[5*k+2]};
        float q[] = {0, 0, 0, out[5*k+4]};
        quantise_coeffs(coeffs, q);
        fwrite(q, sizeof(float), 4, f);
      }
      // fwrite(b16, size, 1, f);
      fclose(f);
    }
    free(b16);
    if(pfm) fclose(pfm);
  }
#endif
  free(out);
  printf("\n");
}
