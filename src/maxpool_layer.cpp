#include "maxpool_layer.h"

#include <float.h>
#include <stdio.h>

#include "convolutional_layer.h"
#include "dark_cuda.h"
#include "gemm.h"
#include "utils.h"

void CreateMaxpoolCudnnTensors(layer* l)
{
#ifdef CUDNN
  CHECK_CUDNN(cudnnCreatePoolingDescriptor(&l->poolingDesc));
  CHECK_CUDNN(cudnnCreateTensorDescriptor(&l->srcTensorDesc));
  CHECK_CUDNN(cudnnCreateTensorDescriptor(&l->dstTensorDesc));
#endif  // CUDNN
}

void CudnnMaxpoolSetup(layer* l)
{
#ifdef CUDNN
  CHECK_CUDNN(cudnnSetPooling2dDescriptor(l->poolingDesc, CUDNN_POOLING_MAX,
      CUDNN_NOT_PROPAGATE_NAN, l->size, l->size, l->pad / 2, l->pad / 2,
      l->stride_x, l->stride_y));

  CHECK_CUDNN(cudnnSetTensor4dDescriptor(l->srcTensorDesc, CUDNN_TENSOR_NCHW,
      CUDNN_DATA_FLOAT, l->batch, l->c, l->h, l->w));
  CHECK_CUDNN(cudnnSetTensor4dDescriptor(l->dstTensorDesc, CUDNN_TENSOR_NCHW,
      CUDNN_DATA_FLOAT, l->batch, l->out_c, l->out_h, l->out_w));
#endif  // CUDNN
}

void FillMaxpoolLayer(layer* l, int batch, int h, int w, int c, int size,
    int stride_x, int stride_y, int padding, int maxpool_depth,
    int out_channels, int antialiasing, int train)
{
  l->type = MAXPOOL;
  l->train = train;

  int const blur_stride_x = stride_x;
  int const blur_stride_y = stride_y;
  l->antialiasing = antialiasing;
  if (antialiasing)
    stride_x = stride_y = l->stride = l->stride_x = l->stride_y = 1;

  l->batch = batch;
  l->h = h;
  l->w = w;
  l->c = c;
  l->pad = padding;
  l->maxpool_depth = maxpool_depth;
  l->out_channels = out_channels;
  if (maxpool_depth)
  {
    l->out_c = out_channels;
    l->out_w = l->w;
    l->out_h = l->h;
  }
  else
  {
    l->out_w = (w + padding - size) / stride_x + 1;
    l->out_h = (h + padding - size) / stride_y + 1;
    l->out_c = c;
  }
  l->outputs = l->out_h * l->out_w * l->out_c;
  l->inputs = h * w * c;
  l->size = size;
  l->stride = stride_x;
  l->stride_x = stride_x;
  l->stride_y = stride_y;
  int output_size = l->out_h * l->out_w * l->out_c * batch;

  if (train)
  {
    l->indexes = (int*)xcalloc(output_size, sizeof(int));
    l->delta = (float*)xcalloc(output_size, sizeof(float));
  }
  l->output = (float*)xcalloc(output_size, sizeof(float));

  l->forward = ForwardMaxpoolLayer;
  l->backward = BackwardMaxpoolLayer;

#ifdef GPU
  l->forward_gpu = ForwardMaxpoolLayerGpu;
  l->backward_gpu = BackwardMaxpoolLayerGpu;

  if (train)
  {
    l->indexes_gpu = cuda_make_int_array(output_size);
    l->delta_gpu = cuda_make_array(l->delta, output_size);
  }
  l->output_gpu = cuda_make_array(l->output, output_size);
  CreateMaxpoolCudnnTensors(l);
  CudnnMaxpoolSetup(l);

#endif  // GPU
  l->bflops = (l->size * l->size * l->c * l->out_h * l->out_w) / 1000000000.;

  if (maxpool_depth)
    fprintf(stderr,
        "max-depth         %2dx%2d/%2d   %4d x%4d x%4d -> %4d x%4d x%4d "
        "%5.3f BF\n",
        size, size, stride_x, w, h, c, l->out_w, l->out_h, l->out_c, l->bflops);
  else if (stride_x == stride_y)
    fprintf(stderr,
        "max               %2dx%2d/%2d   %4d x%4d x%4d -> %4d x%4d x%4d "
        "%5.3f BF\n",
        size, size, stride_x, w, h, c, l->out_w, l->out_h, l->out_c, l->bflops);
  else
    fprintf(stderr,
        "max              %2dx%2d/%2dx%2d %4d x%4d x%4d -> %4d x%4d x%4d "
        "%5.3f BF\n",
        size, size, stride_x, stride_y, w, h, c, l->out_w, l->out_h, l->out_c,
        l->bflops);

  if (l->antialiasing)
  {
    printf("AA:  ");

    int blur_size = 3;
    int blur_pad = blur_size / 2;
    if (l->antialiasing == 2)
    {
      blur_size = 2;
      blur_pad = 0;
    }

    l->input_layer = (layer*)calloc(1, sizeof(layer));
    FillConvLayer(l->input_layer, batch, 1, l->out_h, l->out_w, l->out_c,
        l->out_c, l->out_c, blur_size, blur_stride_x, blur_stride_y, 1,
        blur_pad, LINEAR, 0, 0, 0, 0, 0, 1, 0, NULL, train);

    int const blur_nweights = l->out_c * blur_size * blur_size;
    if (blur_size == 2)
    {
      for (int i = 0; i < blur_nweights; i += (blur_size * blur_size))
      {
        l->input_layer->weights[i + 0] = 1 / 4.f;
        l->input_layer->weights[i + 1] = 1 / 4.f;
        l->input_layer->weights[i + 2] = 1 / 4.f;
        l->input_layer->weights[i + 3] = 1 / 4.f;
      }
    }
    else
    {
      for (int i = 0; i < blur_nweights; i += (blur_size * blur_size))
      {
        l->input_layer->weights[i + 0] = 1 / 16.f;
        l->input_layer->weights[i + 1] = 2 / 16.f;
        l->input_layer->weights[i + 2] = 1 / 16.f;

        l->input_layer->weights[i + 3] = 2 / 16.f;
        l->input_layer->weights[i + 4] = 4 / 16.f;
        l->input_layer->weights[i + 5] = 2 / 16.f;

        l->input_layer->weights[i + 6] = 1 / 16.f;
        l->input_layer->weights[i + 7] = 2 / 16.f;
        l->input_layer->weights[i + 8] = 1 / 16.f;
      }
    }

    for (int i = 0; i < l->out_c; ++i)
    {
      l->input_layer->biases[i] = 0;
    }
#ifdef GPU
    if (cuda_get_device() >= 0)
    {
      if (l->antialiasing)
        l->input_antialiasing_gpu =
            cuda_make_array(NULL, l->batch * l->outputs);
      PushConvolutionalLayer(l->input_layer);
    }
#endif  // GPU
  }
}

void ResizeMaxpoolLayer(layer* l, int w, int h)
{
  l->h = h;
  l->w = w;
  l->inputs = h * w * l->c;

  l->out_w = (w + l->pad - l->size) / l->stride_x + 1;
  l->out_h = (h + l->pad - l->size) / l->stride_y + 1;
  l->outputs = l->out_w * l->out_h * l->out_c;
  int output_size = l->outputs * l->batch;

  if (l->train)
  {
    l->indexes = (int*)xrealloc(l->indexes, output_size * sizeof(int));
    l->delta = (float*)xrealloc(l->delta, output_size * sizeof(float));
  }
  l->output = (float*)xrealloc(l->output, output_size * sizeof(float));

#ifdef GPU
  CHECK_CUDA(cudaFree(l->output_gpu));
  l->output_gpu = cuda_make_array(l->output, output_size);

  if (l->train)
  {
    CHECK_CUDA(cudaFree((float*)l->indexes_gpu));
    l->indexes_gpu = cuda_make_int_array(output_size);

    CHECK_CUDA(cudaFree(l->delta_gpu));
    l->delta_gpu = cuda_make_array(l->delta, output_size);
  }

  CudnnMaxpoolSetup(l);
#endif
}

void ForwardMaxpoolLayer(layer* l, NetworkState state)
{
  if (l->maxpool_depth)
  {
    int b, i, j, k, g;
    for (b = 0; b < l->batch; ++b)
    {
#pragma omp parallel for
      for (i = 0; i < l->h; ++i)
      {
        for (j = 0; j < l->w; ++j)
        {
          for (g = 0; g < l->out_c; ++g)
          {
            int out_index = j + l->w * (i + l->h * (g + l->out_c * b));
            float max = -FLT_MAX;
            int max_i = -1;

            for (k = g; k < l->c; k += l->out_c)
            {
              int in_index = j + l->w * (i + l->h * (k + l->c * b));
              float val = state.input[in_index];

              max_i = (val > max) ? in_index : max_i;
              max = (val > max) ? val : max;
            }
            l->output[out_index] = max;
            if (l->indexes)
              l->indexes[out_index] = max_i;
          }
        }
      }
    }
    return;
  }

  if (!state.train && l->stride_x == l->stride_y)
  {
    forward_maxpool_layer_avx(state.input, l->output, l->indexes, l->size, l->w,
        l->h, l->out_w, l->out_h, l->c, l->pad, l->stride, l->batch);
  }
  else
  {
    int b, i, j, k, m, n;
    int w_offset = -l->pad / 2;
    int h_offset = -l->pad / 2;

    int h = l->out_h;
    int w = l->out_w;
    int c = l->c;

    for (b = 0; b < l->batch; ++b)
    {
      for (k = 0; k < c; ++k)
      {
        for (i = 0; i < h; ++i)
        {
          for (j = 0; j < w; ++j)
          {
            int out_index = j + w * (i + h * (k + c * b));
            float max = -FLT_MAX;
            int max_i = -1;
            for (n = 0; n < l->size; ++n)
            {
              for (m = 0; m < l->size; ++m)
              {
                int cur_h = h_offset + i * l->stride_y + n;
                int cur_w = w_offset + j * l->stride_x + m;
                int index = cur_w + l->w * (cur_h + l->h * (k + b * l->c));
                int valid =
                    (cur_h >= 0 && cur_h < l->h && cur_w >= 0 && cur_w < l->w);
                float val = (valid != 0) ? state.input[index] : -FLT_MAX;
                max_i = (val > max) ? index : max_i;
                max = (val > max) ? val : max;
              }
            }
            l->output[out_index] = max;
            if (l->indexes)
              l->indexes[out_index] = max_i;
          }
        }
      }
    }
  }

  if (l->antialiasing)
  {
    NetworkState s = {0};
    s.train = state.train;
    s.workspace = state.workspace;
    s.net = state.net;
    s.input = l->output;
    ForwardConvolutionalLayer(l->input_layer, s);
    memcpy(l->output, l->input_layer->output,
        l->input_layer->outputs * l->input_layer->batch * sizeof(float));
  }
}

void BackwardMaxpoolLayer(layer* l, NetworkState state)
{
  int i;
  int h = l->out_h;
  int w = l->out_w;
  int c = l->out_c;
#pragma omp parallel for
  for (i = 0; i < h * w * c * l->batch; ++i)
  {
    int index = l->indexes[i];
    state.delta[index] += l->delta[i];
  }
}

void ForwardLocalAvgpoolLayer(layer* l, NetworkState state)
{
  int b, i, j, k, m, n;
  int w_offset = -l->pad / 2;
  int h_offset = -l->pad / 2;

  int h = l->out_h;
  int w = l->out_w;
  int c = l->c;

  for (b = 0; b < l->batch; ++b)
  {
    for (k = 0; k < c; ++k)
    {
      for (i = 0; i < h; ++i)
      {
        for (j = 0; j < w; ++j)
        {
          int out_index = j + w * (i + h * (k + c * b));
          float avg = 0;
          int counter = 0;
          for (n = 0; n < l->size; ++n)
          {
            for (m = 0; m < l->size; ++m)
            {
              int cur_h = h_offset + i * l->stride_y + n;
              int cur_w = w_offset + j * l->stride_x + m;
              int index = cur_w + l->w * (cur_h + l->h * (k + b * l->c));
              int valid =
                  (cur_h >= 0 && cur_h < l->h && cur_w >= 0 && cur_w < l->w);
              if (valid)
              {
                counter++;
                avg += state.input[index];
              }
            }
          }
          l->output[out_index] = avg / counter;
        }
      }
    }
  }
}

void BackwardLocalAvgpoolLayer(layer* l, NetworkState state)
{
  int b, i, j, k, m, n;
  int w_offset = -l->pad / 2;
  int h_offset = -l->pad / 2;

  int h = l->out_h;
  int w = l->out_w;
  int c = l->c;

  for (b = 0; b < l->batch; ++b)
  {
    for (k = 0; k < c; ++k)
    {
      for (i = 0; i < h; ++i)
      {
        for (j = 0; j < w; ++j)
        {
          int out_index = j + w * (i + h * (k + c * b));
          for (n = 0; n < l->size; ++n)
          {
            for (m = 0; m < l->size; ++m)
            {
              int cur_h = h_offset + i * l->stride_y + n;
              int cur_w = w_offset + j * l->stride_x + m;
              int index = cur_w + l->w * (cur_h + l->h * (k + b * l->c));
              int valid =
                  (cur_h >= 0 && cur_h < l->h && cur_w >= 0 && cur_w < l->w);

              if (valid)
                state.delta[index] += l->delta[out_index] / (l->size * l->size);
            }
          }
        }
      }
    }
  }
}