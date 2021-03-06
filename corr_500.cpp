/*

g++ -O3 -msse3 -mfpmath=sse -fopenmp -lOpenCL -lm -o corr_500 corr_500.cpp

*/
#include <omp.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <math.h>
#include <malloc.h>
#include <string.h>

#include <iostream>
#include <fstream>
#include <sys/stat.h>

#include <CL/cl.h>

#include "sse.h"

using namespace std;

double dtime() {
  struct timeval t;
  gettimeofday(&t, NULL);
  return (double)t.tv_sec + (((double)t.tv_usec) / 1000000.0);
}

int align(int idx, int n) {
  return (n - idx%n) % n;
}


void correlate_scalar
(
 float *correlation, int corr_size,
 const float *base, const float *mask,
 int sample_size)
{
  for (int offset_y=0; offset_y < corr_size; offset_y++) {
    for (int offset_x=0; offset_x < corr_size; offset_x++) {
      int correlation_index = offset_y*corr_size + offset_x;
      float sum = 0.0f;
      for (int rows=0; rows < sample_size-offset_y; rows++) {
        for (int columns=0; columns < sample_size-offset_x; columns++) {
          int mi = 4*(rows * sample_size + columns);
          int bi = 4*((offset_y + rows) * sample_size + columns + offset_x);
          sum +=
            base[bi] * mask[mi] +
            base[bi+1] * mask[mi+1] +
            base[bi+2] * mask[mi+2] +
            base[bi+3] * mask[mi+3];
          ;
        }
      }
      correlation[correlation_index] = sum;
    }
  }
}


void correlate
(
 float *correlation, int corr_size,
 const float *basef, const float *maskf,
 int sample_size)
{
  const float4* base = (float4*) basef;
  const float4* mask = (float4*) maskf;
  #pragma omp parallel for
  for (int offset_y=0; offset_y < corr_size; offset_y++) {
    for (int offset_x=0; offset_x < corr_size; offset_x++) {
      float4 sum = float4(0.0);
      for (int rows=0; rows < sample_size-offset_y; rows++) {
        int mask_index = rows * sample_size;
        int base_index = (offset_y+rows) * sample_size + offset_x;
        for (int columns=0; columns < sample_size-offset_x; columns++) {
          sum += base[base_index+columns] * mask[mask_index+columns];
        }
      }
      correlation[offset_y*corr_size + offset_x] = sum.sum();
    }
  }
}

void correlate_optimized
(
 float *correlation, int corr_size,
 const float *obase, const float *omask,
 int sample_size)
{
  int stride = sample_size + 4;
  float4 *base = (float4*)memalign(16, stride*sample_size*sizeof(float4));
  float4 *mask = (float4*)memalign(16, stride*sample_size*sizeof(float4));
  for (int y=0; y<sample_size; y++) {
    memcpy(&base[y*stride], &obase[y*sample_size*4], sample_size*sizeof(float4));
    memset(&base[y*stride+sample_size], 0, (stride-sample_size)*sizeof(float4));
    memcpy(&mask[y*stride], &omask[y*sample_size*4], sample_size*sizeof(float4));
    memset(&mask[y*stride+sample_size], 0, (stride-sample_size)*sizeof(float4));
  }
  #pragma omp parallel for
  for (int offset_y=0; offset_y < corr_size; offset_y++) {
    for (int rows=0; rows < sample_size-offset_y; rows++) {
      for (int offset_x=0; offset_x < corr_size; offset_x+=4) {
        float4 sum[4];
        int mask_index = rows * stride;
        int base_index = (offset_y+rows) * stride + offset_x;
        for (int columns=0; columns < sample_size-offset_x; columns++) {
          for (int i=0; i<4; i++)
            sum[i] += base[base_index+columns+i] * mask[mask_index+columns];
        }
        for (int i=0; i<4; i++)
          correlation[offset_y*corr_size + offset_x + i] += sum[i].sum();
      }
    }
  }
  free(base);
  free(mask);
}

void correlate_optimized_double
(
 double *correlation, int corr_size,
 const double *obase, const double *omask,
 int sample_size)
{
  int stride = sample_size + 2;
  double4 *base = (double4*)memalign(16, stride*sample_size*sizeof(double4));
  double4 *mask = (double4*)memalign(16, stride*sample_size*sizeof(double4));
  for (int y=0; y<sample_size; y++) {
    memcpy(&base[y*stride], &obase[y*sample_size*4], sample_size*sizeof(double4));
    memset(&base[y*stride+sample_size], 0, (stride-sample_size)*sizeof(double4));
    memcpy(&mask[y*stride], &omask[y*sample_size*4], sample_size*sizeof(double4));
    memset(&mask[y*stride+sample_size], 0, (stride-sample_size)*sizeof(double4));
  }
  #pragma omp parallel for
  for (int offset_y=0; offset_y < corr_size; offset_y++) {
    for (int rows=0; rows < sample_size-offset_y; rows++) {
      for (int offset_x=0; offset_x < corr_size; offset_x+=2) {
        double4 sum[2];
        int mask_index = rows * stride;
        int base_index = (offset_y+rows) * stride + offset_x;
        for (int columns=0; columns < sample_size-offset_x; columns++) {
          for (int i=0; i<2; i++)
            sum[i] += base[base_index+columns+i] * mask[mask_index+columns];
        }
        for (int i=0; i<2; i++)
          correlation[offset_y*corr_size + offset_x + i] += sum[i].sum();
      }
    }
  }
  free(base);
  free(mask);
}


unsigned char *readFile (const char *filename, size_t *read_bytes)
{
  ifstream file;
  file.open(filename, ios::binary|ios::in|ios::ate);
  size_t sz = file.tellg();
  char *data = (char*)memalign(16, sz+1);
  data[sz] = 0;
  file.seekg(0, ios::beg);
  file.read(data, sz);
  file.close();
  *read_bytes = sz;
  return (unsigned char*)data;
}



const unsigned char *programBinary = NULL;
size_t programBinaryLength = 0;
const char *programSource = NULL;

void save_program_binary(cl_program program, const char *filename)
{
  size_t binsize;
  clGetProgramInfo(program, CL_PROGRAM_BINARY_SIZES, sizeof(&binsize), (void*)&binsize, NULL);
  const unsigned char *bin = (unsigned char*)malloc(binsize);
  const unsigned char **bins = &bin;
  clGetProgramInfo(program, CL_PROGRAM_BINARIES, sizeof(bins), bins, NULL);
  ofstream binfile;
  binfile.open(filename, ios::binary|ios::out|ios::trunc);
  binfile.write((char*)bin, binsize);
  binfile.close();
}

void print_error(int err) {
  const char* name;
  switch(err) {
    case CL_INVALID_CONTEXT: name = "CL_INVALID_CONTEXT"; break;
    case CL_INVALID_VALUE: name = "CL_INVALID_VALUE"; break;
    case CL_INVALID_IMAGE_FORMAT_DESCRIPTOR: name = "CL_INVALID_IMAGE_FORMAT_DESCRIPTOR"; break;
    case CL_INVALID_IMAGE_SIZE: name = "CL_INVALID_IMAGE_SIZE"; break;
    case CL_INVALID_HOST_PTR: name = "CL_INVALID_HOST_PTR"; break;
    case CL_IMAGE_FORMAT_NOT_SUPPORTED: name = "CL_IMAGE_FORMAT_NOT_SUPPORTED"; break;
    case CL_MEM_OBJECT_ALLOCATION_FAILURE: name = "CL_MEM_OBJECT_ALLOCATION_FAILURE"; break;
    case CL_INVALID_OPERATION: name = "CL_INVALID_OPERATION"; break;
    case CL_OUT_OF_RESOURCES: name = "CL_OUT_OF_RESOURCES"; break;
    case CL_OUT_OF_HOST_MEMORY: name = "CL_OUT_OF_HOST_MEMORY"; break;
    case CL_INVALID_PROGRAM_EXECUTABLE: name="CL_INVALID_PROGRAM_EXECUTABLE"; break;
    case CL_INVALID_COMMAND_QUEUE: name = "CL_INVALID_COMMAND_QUEUE"; break;
    case CL_INVALID_KERNEL: name = "CL_INVALID_KERNEL"; break;
    case CL_INVALID_KERNEL_ARGS: name = "CL_INVALID_KERNEL_ARGS"; break;
    case CL_INVALID_WORK_DIMENSION: name = "CL_INVALID_WORK_DIMENSION"; break;
    case CL_INVALID_GLOBAL_WORK_SIZE: name = "CL_INVALID_GLOBAL_WORK_SIZE"; break;
    case CL_INVALID_GLOBAL_OFFSET: name = "CL_INVALID_GLOBAL_OFFSET"; break;
    case CL_INVALID_WORK_GROUP_SIZE: name = "CL_INVALID_WORK_GROUP_SIZE"; break;
    case CL_INVALID_WORK_ITEM_SIZE: name = "CL_INVALID_WORK_ITEM_SIZE"; break;
    case CL_INVALID_EVENT_WAIT_LIST: name = "CL_INVALID_EVENT_WAIT_LIST"; break;
    default: name = "unknown";
  }
  printf("\nError: %s\n", name);
  exit(1);
}

struct build_t { double buildTime; double kernelTime; double argTime; double releaseTime; double readTime; };

struct build_t correlate_openCL
(
 float *correlation, int corr_size,
 const float *obase, const float *omask,
 int sample_size, bool useCPU)
{
  double t0 = dtime();

  cl_platform_id platform;
  clGetPlatformIDs( 1, &platform, NULL );

  cl_device_id device;
  clGetDeviceIDs( platform, useCPU ? CL_DEVICE_TYPE_CPU : CL_DEVICE_TYPE_GPU, 1, &device, NULL );

  cl_int support;
  clGetDeviceInfo( device, CL_DEVICE_IMAGE_SUPPORT, sizeof(cl_int), &support, NULL );
  support &= !useCPU;

  cl_context context = clCreateContext( NULL, 1, &device, NULL, NULL, NULL );
  cl_command_queue queue = clCreateCommandQueue( context, device, 0, NULL );

  const char *program_source_filename = useCPU ? "correlate2.cl" : (support ? "correlate_image.cl" : "correlate_500.cl");

  int err = 0;

  cl_program program;
  size_t len;
  const char* programSource = (char*)readFile(program_source_filename, &len);
  program = clCreateProgramWithSource( context, 1, &programSource, NULL, NULL );
  err = clBuildProgram( program, 1, &device, NULL, NULL, NULL );
  free((void*) programSource);
  if (err != CL_SUCCESS) {
    char log[2048];
    clGetProgramBuildInfo( program, device, CL_PROGRAM_BUILD_LOG, sizeof(log), log, &len);
    printf("Kernel build log: %s\n", log);
    print_error(err);
  }

  cl_kernel kernel = clCreateKernel( program, "correlate", NULL );

  double buildTime = dtime()-t0;

  int stride = (support ? sample_size : sample_size + 16) + align(sample_size, 8); // pad by 32, align rows on 128 bytes
  int corr_stride = corr_size + align(corr_size, 8); // pad to divisible by 8
  float *base = (float*)memalign(16, stride*stride*16);
  float *mask = (float*)memalign(16, stride*stride*16);
  for (int y=0; y<sample_size; y++) {
    memcpy(&base[y*stride*4], &obase[y*sample_size*4], sample_size*16);
    memset(&base[y*stride*4+sample_size*4], 0, (stride-sample_size)*16);
    memcpy(&mask[y*stride*4], &omask[y*sample_size*4], sample_size*16);
  }
  for (int y=sample_size; y<stride; y++) {
    memset(&base[y*stride*4], 0, stride*16);
    memset(&mask[y*stride*4], 0, stride*16);
  }

  float *tmp = (float*)memalign(16, corr_stride*corr_stride*sizeof(cl_float));
  memset(tmp, 0, corr_stride*corr_stride*sizeof(cl_float));

  t0 = dtime();


  cl_image_format fmt;
  fmt.image_channel_order = CL_RGBA;
  fmt.image_channel_data_type = CL_FLOAT;

  cl_mem base_buf = support
  ? clCreateImage2D(context, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, &fmt, stride, sample_size, 0, (void*)base, &err)
  : clCreateBuffer(
    context,
    CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,
    stride*stride*sizeof(cl_float4),
    (void*)base, &err );
  if (err != CL_SUCCESS) {
    printf("\nbase_buf error: %d\n", err);
    print_error(err);
  }

  err = CL_SUCCESS;
  cl_mem mask_buf = support
  ? clCreateImage2D(context, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, &fmt, stride, sample_size, 0, (void*)mask, &err)
  : clCreateBuffer(
    context,
    CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR,
    stride*stride*sizeof(cl_float4),
    (void*)mask, &err );
  if (err != CL_SUCCESS) {
    printf("\nmask_buf error: %d\n", err);
    print_error(err);
  }

  err = CL_SUCCESS;
  cl_mem corr_buf = clCreateBuffer(
    context,
    CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR,
    (corr_stride*corr_stride)*sizeof(float),
    (void*)tmp, &err );
  if (err != CL_SUCCESS)
    printf("\ncorr_buf error: %d\n", err);
  err = CL_SUCCESS;

  err = clSetKernelArg(kernel, 0, sizeof(corr_buf), (void*) &corr_buf);
  if (err != CL_SUCCESS) printf("\narg 0 error: %d\n", err);
  err = CL_SUCCESS;
  clSetKernelArg(kernel, 1, sizeof(corr_stride), (void*) &corr_stride);
  if (err != CL_SUCCESS) printf("\narg 1 error: %d\n", err);
  err = CL_SUCCESS;
  clSetKernelArg(kernel, 2, sizeof(base_buf), (void*) &base_buf);
  if (err != CL_SUCCESS) printf("\narg 2 error: %d\n", err);
  err = CL_SUCCESS;
  clSetKernelArg(kernel, 3, sizeof(mask_buf), (void*) &mask_buf);
  if (err != CL_SUCCESS) printf("\narg 3 error: %d\n", err);
  err = CL_SUCCESS;
  clSetKernelArg(kernel, 4, sizeof(sample_size), (void*) &sample_size);
  if (err != CL_SUCCESS) printf("\narg 4 error: %d\n", err);
  err = CL_SUCCESS;
  clSetKernelArg(kernel, 5, sizeof(stride), (void*) &stride);
  if (err != CL_SUCCESS) printf("\narg 5 error: %d\n", err);
  err = CL_SUCCESS;

  double argTime = dtime() - t0;

  t0 = dtime();
  if (useCPU) {
    size_t cpu_sz[1] = { corr_size };
    err = clEnqueueNDRangeKernel( queue, kernel, 1, NULL, cpu_sz, NULL, 0, NULL, NULL);
  } else {
    size_t gpu_sz[1] = { corr_stride*corr_stride/8 };
    err = clEnqueueNDRangeKernel( queue, kernel, 1, NULL, gpu_sz, NULL, 0, NULL, NULL);
  }
  if (err != CL_SUCCESS) {
    printf("\nError running kernel\n");
    print_error(err);
  }
  clFinish(queue);
  double kernelTime = dtime() - t0;

  t0 = dtime();

  clEnqueueReadBuffer(queue, corr_buf, CL_TRUE, 0, corr_stride*corr_size*sizeof(cl_float), (void*)tmp, NULL, NULL, NULL);

  for (int y=0; y<corr_size; y++) {
    for (int x=0; x<corr_size; x++) {
      correlation[y*corr_size+x] = tmp[y*corr_stride+x];
    }
  }

  double readTime = dtime () - t0;

  t0 = dtime();

  clReleaseMemObject( base_buf );
  clReleaseMemObject( mask_buf );
  clReleaseMemObject( corr_buf );

  free(base);
  free(mask);
  free(tmp);
  double releaseTime = dtime () - t0;

  t0 = dtime();
  clReleaseCommandQueue( queue );
  clReleaseKernel( kernel );
  clReleaseProgram( program );
  clReleaseContext( context );
  buildTime += dtime() - t0;

  build_t t;
  t.buildTime = buildTime;
  t.kernelTime = kernelTime;
  t.readTime = readTime;
  t.argTime = argTime;
  t.releaseTime = releaseTime;
  return t;
}


float* makeImage(int ssz, bool initialize)
{
  float *img = (float*)memalign(16, ssz*ssz*4*sizeof(float));
  if (initialize)
    for (int i=0; i<ssz*ssz*4; i++)
      img[i] = 0.00625*(i/(ssz*4)) + 0.00625*(i%(ssz*4));
  return img;
}

double* makeImaged(int ssz, bool initialize)
{
  double *img = (double*)memalign(16, ssz*ssz*4*sizeof(double));
  if (initialize)
    for (int i=0; i<ssz*ssz*4; i++)
      img[i] = 0.00625*(i/(ssz*4)) + 0.00625*(i%(ssz*4));
  return img;
}


int main () {
  double t0, t1;
  int ssz = 500;
  int csz = ssz/2;

  float *base = makeImage(ssz, true);
  float *mask = makeImage(ssz, true);
  double *based = makeImaged(ssz, true);
  double *maskd = makeImaged(ssz, true);
  // reverse mask
  float tmp;
  double tmpd;
  int len = ssz*ssz*4;
  for (int i=0; i<len/2; i++) {
    tmp = mask[len-1-i];
    mask[len-1-i] = mask[i];
    mask[i] = tmp;
    tmpd = maskd[len-1-i];
    maskd[len-1-i] = maskd[i];
    maskd[i] = tmpd;
  }

  float *corr = (float*)memalign(16, csz*csz*sizeof(float));
  float *corr1 = (float*)memalign(16, csz*csz*sizeof(float));
  float *corr2 = (float*)memalign(16, csz*csz*sizeof(float));
  float *corr3 = (float*)memalign(16, csz*csz*sizeof(float));
  double *corr4 = (double*)memalign(16, csz*csz*sizeof(double));
  memset((void*)corr, 0, csz*csz*sizeof(float));
  memset((void*)corr1, 0, csz*csz*sizeof(float));
  memset((void*)corr2, 0, csz*csz*sizeof(float));
  memset((void*)corr3, 0, csz*csz*sizeof(float));
  memset((void*)corr4, 0, csz*csz*sizeof(double));


  fprintf(stderr, "Achieved bandwidth in GBps, divide by four for GFLOPS (sse_dbl by eight)\n");
  printf("in_sz\tout_sz\tbw_used\tcl_gpu\tkbw_gpu\tgbld_t\targt\treadt\trelt\tcl_cpu\tkbw_cpu\tcbld_t\tsse_opt\tsse_dbl\tsse\n");


  for (int isz=ssz*ssz; isz<=ssz*ssz; isz+=20000) {
    int sz = sqrt(isz);
    double gb = 1e-9 * (2*sz*0.75*sz*0.75*4*4 * sz*0.5 * sz*0.5 + sz*0.5*sz*0.5);
    printf("%d\t%d\t%.2f", 2*(sz*sz)*16, (sz/2)*(sz/2)*4, gb);
    fflush(stdout);

    // Correlate_openCL runs the kernel 10 times to make
    // it take the same amount of time as the CPU impls.
    // This is kinda hacky though, a better benchmark would
    // be to run all versions over ten different images?
    int repeats = 1;
    double elapsed = 0.0;
    build_t bt;
    for (int j=0; j<repeats; j++) {
      t0 = dtime();
      bt = correlate_openCL(corr3, sz/2, base, mask, sz, false);
      elapsed += dtime()-bt.buildTime-t0;
    }
    printf("\t%.2f\t%.2f\t%.2f\t%.4f\t%.4f\t%.4f", repeats*gb/elapsed, gb/bt.kernelTime, bt.buildTime, bt.argTime, bt.readTime, bt.releaseTime);
    fflush(stdout);

    t0 = dtime();
    bt = correlate_openCL(corr2, sz/2, base, mask, sz, true);
    t1 = dtime()-bt.buildTime;
    printf("\t%.2f\t%.2f\t%.2f", gb/(t1-t0), gb/bt.kernelTime, bt.buildTime);
    fflush(stdout);
/*
    t0 = dtime();
    correlate_optimized(corr1, sz/2, base, mask, sz);
    t1 = dtime();
    printf("\t%.2f", gb/(t1-t0));
    fflush(stdout);

    t0 = dtime();
    correlate_optimized_double(corr4, sz/2, based, maskd, sz);
    t1 = dtime();
    // twice as much stuff to truck with doubles
    printf("\t%.2f", (2*gb)/(t1-t0));
    fflush(stdout);

*/

    t0 = dtime();
    correlate(corr, sz/2, base, mask, sz);
    t1 = dtime();
    printf("\t%.2f", gb/(t1-t0));
    fflush(stdout);

    printf("\n");

    for (int i=0; i<(sz/2)*(sz/2); i++) {
      // less than one tenth-thousandth error
      if (
        // corr1 and corr2 have same algo
        //fabs(corr1[i]-corr2[i]) > fabs(corr1[i]*0.0001) ||
        // corr and corr3 have same algo
        fabs(corr2[i]-corr3[i]) > fabs(corr2[i]*0.001) ||
        // ground optimized CPU impl to normal impl
        // the order of calculations is different,
        // which causes the discrepancy?
        fabs(corr[i]-corr2[i]) > fabs(corr[i]*0.001) //||
        //fabs(corr4[i]-corr1[i]) > fabs(corr1[i]*0.001)
      ) {
        fprintf(stderr, "%d: discrepancy sse %f sse_opt %f sse_dbl %f cl_cpu %f cl_gpu %f\n", i, corr[i], corr1[i], corr4[i], corr2[i], corr3[i]);
        break;
      }
    }
  }

  return 0;
}
