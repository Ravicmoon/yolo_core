#include "crop_layer.h"

#include <stdio.h>

#include "dark_cuda.h"
#include "utils.h"

void BackwardCropLayer(layer* l, NetworkState state) {}
void BackwardCropLayerGpu(layer* l, NetworkState state) {}

void FillCropLayer(layer* l, int batch, int h, int w, int c, int crop_height,
    int crop_width, int flip, float angle, float saturation, float exposure)
{
  fprintf(stderr, "Crop layer: %d x %d -> %d x %d x %d image\n", h, w,
      crop_height, crop_width, c);

  l->type = CROP;
  l->batch = batch;
  l->h = h;
  l->w = w;
  l->c = c;
  l->scale = (float)crop_height / h;
  l->flip = flip;
  l->angle = angle;
  l->saturation = saturation;
  l->exposure = exposure;
  l->out_w = crop_width;
  l->out_h = crop_height;
  l->out_c = c;
  l->inputs = l->w * l->h * l->c;
  l->outputs = l->out_w * l->out_h * l->out_c;
  l->output = (float*)xcalloc(l->outputs * batch, sizeof(float));
  l->forward = ForwardCropLayer;
  l->backward = BackwardCropLayer;

#ifdef GPU
  l->forward_gpu = ForwardCropLayerGpu;
  l->backward_gpu = BackwardCropLayerGpu;
  l->output_gpu = cuda_make_array(l->output, l->outputs * batch);
  l->rand_gpu = cuda_make_array(0, l->batch * 8);
#endif
}

void ResizeCropLayer(layer* l, int w, int h)
{
  l->w = w;
  l->h = h;

  l->out_w = l->scale * w;
  l->out_h = l->scale * h;

  l->inputs = l->w * l->h * l->c;
  l->outputs = l->out_h * l->out_w * l->out_c;

  l->output =
      (float*)xrealloc(l->output, l->batch * l->outputs * sizeof(float));
#ifdef GPU
  cuda_free(l->output_gpu);
  l->output_gpu = cuda_make_array(l->output, l->outputs * l->batch);
#endif
}

void ForwardCropLayer(layer* l, NetworkState state)
{
  int count = 0;
  int flip = (l->flip && rand() % 2);
  int dh = rand() % (l->h - l->out_h + 1);
  int dw = rand() % (l->w - l->out_w + 1);
  float scale = 2;
  float trans = -1;
  if (l->noadjust)
  {
    scale = 1;
    trans = 0;
  }
  if (!state.train)
  {
    flip = 0;
    dh = (l->h - l->out_h) / 2;
    dw = (l->w - l->out_w) / 2;
  }
  for (int b = 0; b < l->batch; ++b)
  {
    for (int c = 0; c < l->c; ++c)
    {
      for (int i = 0; i < l->out_h; ++i)
      {
        for (int j = 0; j < l->out_w; ++j)
        {
          int col = j + dw;
          if (flip)
            col = l->w - dw - j - 1;

          int row = i + dh;
          int idx = col + l->w * (row + l->h * (c + l->c * b));

          l->output[count++] = state.input[idx] * scale + trans;
        }
      }
    }
  }
}
