#include "yolo_layer.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "activations.h"
#include "blas.h"
#include "box.h"
#include "dark_cuda.h"
#include "utils.h"

void FillYoloLayer(layer* l, int batch, int w, int h, int n, int total,
    int* mask, int classes, int max_boxes)
{
  l->type = YOLO;
  l->n = n;
  l->total = total;
  l->batch = batch;
  l->h = h;
  l->w = w;
  l->c = n * (classes + 4 + 1);
  l->out_w = l->w;
  l->out_h = l->h;
  l->out_c = l->c;
  l->classes = classes;
  l->cost = (float*)xcalloc(1, sizeof(float));
  l->biases = (float*)xcalloc(total * 2, sizeof(float));
  if (mask)
    l->mask = mask;
  else
  {
    l->mask = (int*)xcalloc(n, sizeof(int));
    for (int i = 0; i < n; ++i)
    {
      l->mask[i] = i;
    }
  }
  l->bias_updates = (float*)xcalloc(n * 2, sizeof(float));
  l->outputs = h * w * n * (classes + 4 + 1);
  l->inputs = l->outputs;
  l->max_boxes = max_boxes;
  l->truths = l->max_boxes * (4 + 1);  // 90*(4 + 1);
  l->delta = (float*)xcalloc(batch * l->outputs, sizeof(float));
  l->output = (float*)xcalloc(batch * l->outputs, sizeof(float));
  for (int i = 0; i < total * 2; ++i)
  {
    l->biases[i] = .5;
  }

  l->forward = ForwardYoloLayer;
  l->backward = BackwardYoloLayer;
#ifdef GPU
  l->forward_gpu = ForwardYoloLayerGpu;
  l->backward_gpu = BackwardYoloLayerGpu;
  l->output_gpu = cuda_make_array(l->output, batch * l->outputs);
  l->delta_gpu = cuda_make_array(l->delta, batch * l->outputs);

  free(l->output);
  if (cudaSuccess == cudaHostAlloc(&l->output,
                         batch * l->outputs * sizeof(float),
                         cudaHostRegisterMapped))
    l->output_pinned = 1;
  else
  {
    cudaGetLastError();  // reset CUDA-error
    l->output = (float*)xcalloc(batch * l->outputs, sizeof(float));
  }

  free(l->delta);
  if (cudaSuccess == cudaHostAlloc(&l->delta,
                         batch * l->outputs * sizeof(float),
                         cudaHostRegisterMapped))
    l->delta_pinned = 1;
  else
  {
    cudaGetLastError();  // reset CUDA-error
    l->delta = (float*)xcalloc(batch * l->outputs, sizeof(float));
  }
#endif

  fprintf(stderr, "yolo\n");
  srand(time(0));
}

void ResizeYoloLayer(layer* l, int w, int h)
{
  l->w = w;
  l->h = h;

  l->outputs = h * w * l->n * (l->classes + 4 + 1);
  l->inputs = l->outputs;

  if (!l->output_pinned)
    l->output =
        (float*)xrealloc(l->output, l->batch * l->outputs * sizeof(float));
  if (!l->delta_pinned)
    l->delta =
        (float*)xrealloc(l->delta, l->batch * l->outputs * sizeof(float));

#ifdef GPU
  if (l->output_pinned)
  {
    CHECK_CUDA(cudaFreeHost(l->output));
    if (cudaSuccess != cudaHostAlloc(&l->output,
                           l->batch * l->outputs * sizeof(float),
                           cudaHostRegisterMapped))
    {
      cudaGetLastError();  // reset CUDA-error
      l->output = (float*)xcalloc(l->batch * l->outputs, sizeof(float));
      l->output_pinned = 0;
    }
  }

  if (l->delta_pinned)
  {
    CHECK_CUDA(cudaFreeHost(l->delta));
    if (cudaSuccess != cudaHostAlloc(&l->delta,
                           l->batch * l->outputs * sizeof(float),
                           cudaHostRegisterMapped))
    {
      cudaGetLastError();  // reset CUDA-error
      l->delta = (float*)xcalloc(l->batch * l->outputs, sizeof(float));
      l->delta_pinned = 0;
    }
  }

  cuda_free(l->delta_gpu);
  cuda_free(l->output_gpu);

  l->delta_gpu = cuda_make_array(l->delta, l->batch * l->outputs);
  l->output_gpu = cuda_make_array(l->output, l->batch * l->outputs);
#endif
}

Box GetYoloBox(float const* x, float const* biases, int n, int index, int i,
    int j, int lw, int lh, int w, int h, int stride)
{
  Box b;
  b.x = (i + x[index + 0 * stride]) / lw;
  b.y = (j + x[index + 1 * stride]) / lh;
  b.w = exp(x[index + 2 * stride]) * biases[2 * n] / w;
  b.h = exp(x[index + 3 * stride]) * biases[2 * n + 1] / h;
  return b;
}

static inline float fix_nan_inf(float val)
{
  if (isnan(val) || isinf(val))
    val = 0;
  return val;
}

static inline float clip_value(float val, const float max_val)
{
  if (val > max_val)
  {
    // printf("\n val = %f > max_val = %f \n", val, max_val);
    val = max_val;
  }
  else if (val < -max_val)
  {
    // printf("\n val = %f < -max_val = %f \n", val, -max_val);
    val = -max_val;
  }
  return val;
}

Ious delta_yolo_box(Box truth, float* x, float* biases, int n, int index, int i,
    int j, int lw, int lh, int w, int h, float* delta, float scale, int stride,
    float iou_normalizer, IOU_LOSS iou_loss, int accumulate, float max_delta)
{
  Ious all_ious = {0};
  // i - step in layer width
  // j - step in layer height
  //  Returns a box in absolute coordinates
  Box pred = GetYoloBox(x, biases, n, index, i, j, lw, lh, w, h, stride);
  all_ious.iou = Box::Iou(pred, truth);
  all_ious.giou = Box::Giou(pred, truth);
  all_ious.diou = Box::Diou(pred, truth);
  all_ious.ciou = Box::Ciou(pred, truth);
  // avoid nan in dx_box_iou
  if (pred.w == 0)
  {
    pred.w = 1.0;
  }
  if (pred.h == 0)
  {
    pred.h = 1.0;
  }
  if (iou_loss == MSE)  // old loss
  {
    float tx = (truth.x * lw - i);
    float ty = (truth.y * lh - j);
    float tw = log(truth.w * w / biases[2 * n]);
    float th = log(truth.h * h / biases[2 * n + 1]);

    // printf(" tx = %f, ty = %f, tw = %f, th = %f \n", tx, ty, tw, th);
    // printf(" x = %f, y = %f, w = %f, h = %f \n", x[index + 0 * stride],
    // x[index + 1 * stride], x[index + 2 * stride], x[index + 3 * stride]);

    // accumulate delta
    delta[index + 0 * stride] +=
        scale * (tx - x[index + 0 * stride]) * iou_normalizer;
    delta[index + 1 * stride] +=
        scale * (ty - x[index + 1 * stride]) * iou_normalizer;
    delta[index + 2 * stride] +=
        scale * (tw - x[index + 2 * stride]) * iou_normalizer;
    delta[index + 3 * stride] +=
        scale * (th - x[index + 3 * stride]) * iou_normalizer;
  }
  else
  {
    // https://github.com/generalized-iou/g-darknet
    // https://arxiv.org/abs/1902.09630v2
    // https://giou.stanford.edu/
    all_ious.dx_iou = Box::DxIou(pred, truth, iou_loss);

    // jacobian^t (transpose)
    // float dx = (all_ious.dx_iou.dl + all_ious.dx_iou.dr);
    // float dy = (all_ious.dx_iou.dt + all_ious.dx_iou.db);
    // float dw = ((-0.5 * all_ious.dx_iou.dl) + (0.5 * all_ious.dx_iou.dr));
    // float dh = ((-0.5 * all_ious.dx_iou.dt) + (0.5 * all_ious.dx_iou.db));

    // jacobian^t (transpose)
    float dx = all_ious.dx_iou.dt;
    float dy = all_ious.dx_iou.db;
    float dw = all_ious.dx_iou.dl;
    float dh = all_ious.dx_iou.dr;

    // predict exponential, apply gradient of e^delta_t ONLY for w,h
    dw *= exp(x[index + 2 * stride]);
    dh *= exp(x[index + 3 * stride]);

    // normalize iou weight
    dx *= iou_normalizer;
    dy *= iou_normalizer;
    dw *= iou_normalizer;
    dh *= iou_normalizer;

    dx = fix_nan_inf(dx);
    dy = fix_nan_inf(dy);
    dw = fix_nan_inf(dw);
    dh = fix_nan_inf(dh);

    if (max_delta != FLT_MAX)
    {
      dx = clip_value(dx, max_delta);
      dy = clip_value(dy, max_delta);
      dw = clip_value(dw, max_delta);
      dh = clip_value(dh, max_delta);
    }

    if (!accumulate)
    {
      delta[index + 0 * stride] = 0;
      delta[index + 1 * stride] = 0;
      delta[index + 2 * stride] = 0;
      delta[index + 3 * stride] = 0;
    }

    // accumulate delta
    delta[index + 0 * stride] += dx;
    delta[index + 1 * stride] += dy;
    delta[index + 2 * stride] += dw;
    delta[index + 3 * stride] += dh;
  }

  return all_ious;
}

void averages_yolo_deltas(
    int class_index, int box_index, int stride, int classes, float* delta)
{
  int classes_in_one_box = 0;
  int c;
  for (c = 0; c < classes; ++c)
  {
    if (delta[class_index + stride * c] > 0)
      classes_in_one_box++;
  }

  if (classes_in_one_box > 0)
  {
    delta[box_index + 0 * stride] /= classes_in_one_box;
    delta[box_index + 1 * stride] /= classes_in_one_box;
    delta[box_index + 2 * stride] /= classes_in_one_box;
    delta[box_index + 3 * stride] /= classes_in_one_box;
  }
}

void delta_yolo_class(float* output, float* delta, int index, int class_id,
    int classes, int stride, float* avg_cat, int focal_loss,
    float label_smooth_eps, float* classes_multipliers)
{
  int n;
  if (delta[index + stride * class_id])
  {
    float y_true = 1;
    if (label_smooth_eps)
      y_true = y_true * (1 - label_smooth_eps) + 0.5 * label_smooth_eps;
    float result_delta = y_true - output[index + stride * class_id];
    if (!isnan(result_delta) && !isinf(result_delta))
      delta[index + stride * class_id] = result_delta;
    // delta[index + stride*class_id] = 1 - output[index + stride*class_id];

    if (classes_multipliers)
      delta[index + stride * class_id] *= classes_multipliers[class_id];
    if (avg_cat)
      *avg_cat += output[index + stride * class_id];
    return;
  }
  // Focal loss
  if (focal_loss)
  {
    // Focal Loss
    float alpha = 0.5;  // 0.25 or 0.5
    // float gamma = 2;    // hardcoded in many places of the grad-formula

    int ti = index + stride * class_id;
    float pt = output[ti] + 0.000000000000001F;
    // http://fooplot.com/#W3sidHlwZSI6MCwiZXEiOiItKDEteCkqKDIqeCpsb2coeCkreC0xKSIsImNvbG9yIjoiIzAwMDAwMCJ9LHsidHlwZSI6MTAwMH1d
    float grad =
        -(1 - pt) *
        (2 * pt * logf(pt) + pt -
            1);  // http://blog.csdn.net/linmingan/article/details/77885832
    // float grad = (1 - pt) * (2 * pt*logf(pt) + pt - 1);    //
    // https://github.com/unsky/focal-loss

    for (n = 0; n < classes; ++n)
    {
      delta[index + stride * n] =
          (((n == class_id) ? 1 : 0) - output[index + stride * n]);

      delta[index + stride * n] *= alpha * grad;

      if (n == class_id && avg_cat)
        *avg_cat += output[index + stride * n];
    }
  }
  else
  {
    // default
    for (n = 0; n < classes; ++n)
    {
      float y_true = ((n == class_id) ? 1 : 0);
      if (label_smooth_eps)
        y_true = y_true * (1 - label_smooth_eps) + 0.5 * label_smooth_eps;
      float result_delta = y_true - output[index + stride * n];
      if (!isnan(result_delta) && !isinf(result_delta))
        delta[index + stride * n] = result_delta;

      if (classes_multipliers && n == class_id)
        delta[index + stride * class_id] *= classes_multipliers[class_id];
      if (n == class_id && avg_cat)
        *avg_cat += output[index + stride * n];
    }
  }
}

int compare_yolo_class(float* output, int classes, int class_index, int stride,
    float objectness, int class_id, float conf_thresh)
{
  int j;
  for (j = 0; j < classes; ++j)
  {
    // float prob = objectness * output[class_index + stride*j];
    float prob = output[class_index + stride * j];
    if (prob > conf_thresh)
    {
      return 1;
    }
  }
  return 0;
}

static int EntryIndex(layer const* l, int batch, int location, int entry)
{
  int n = location / (l->w * l->h);
  int loc = location % (l->w * l->h);
  return batch * l->outputs + n * l->w * l->h * (4 + l->classes + 1) +
         entry * l->w * l->h + loc;
}

void ForwardYoloLayer(layer* l, NetworkState state)
{
  int i, j, b, t, n;
  memcpy(l->output, state.input, l->outputs * l->batch * sizeof(float));

#ifndef GPU
  for (b = 0; b < l->batch; ++b)
  {
    for (n = 0; n < l->n; ++n)
    {
      int index = EntryIndex(l, b, n * l->w * l->h, 0);
      activate_array(l->output + index, 2 * l->w * l->h, LOGISTIC);  // x,y,
      scal_add_cpu(2 * l->w * l->h, l->scale_x_y, -0.5 * (l->scale_x_y - 1),
          l->output + index, 1);  // scale x,y
      index = EntryIndex(l, b, n * l->w * l->h, 4);
      activate_array(
          l->output + index, (1 + l->classes) * l->w * l->h, LOGISTIC);
    }
  }
#endif

  // delta is zeroed
  memset(l->delta, 0, l->outputs * l->batch * sizeof(float));
  if (!state.train)
    return;
  // float avg_iou = 0;
  float tot_iou = 0;
  float tot_giou = 0;
  float tot_diou = 0;
  float tot_ciou = 0;
  float tot_iou_loss = 0;
  float tot_giou_loss = 0;
  float tot_diou_loss = 0;
  float tot_ciou_loss = 0;
  float recall = 0;
  float recall75 = 0;
  float avg_cat = 0;
  float avg_obj = 0;
  float avg_anyobj = 0;
  int count = 0;
  int class_count = 0;
  *(l->cost) = 0;
  for (b = 0; b < l->batch; ++b)
  {
    for (j = 0; j < l->h; ++j)
    {
      for (i = 0; i < l->w; ++i)
      {
        for (n = 0; n < l->n; ++n)
        {
          int box_index = EntryIndex(l, b, n * l->w * l->h + j * l->w + i, 0);
          Box pred = GetYoloBox(l->output, l->biases, l->mask[n], box_index, i,
              j, l->w, l->h, state.net->w, state.net->h, l->w * l->h);
          float best_match_iou = 0;
          float best_iou = 0;
          int best_t = 0;
          for (t = 0; t < l->max_boxes; ++t)
          {
            Box truth(state.truth + t * (4 + 1) + b * l->truths);
            int class_id = state.truth[t * (4 + 1) + b * l->truths + 4];
            if (class_id >= l->classes || class_id < 0)
            {
              printf(
                  "\n Warning: in txt-labels class_id=%d >= classes=%d in "
                  "cfg-file. In txt-labels class_id should be [from 0 to %d] "
                  "\n",
                  class_id, l->classes, l->classes - 1);
              printf(
                  "\n truth.x = %f, truth.y = %f, truth.w = %f, truth.h = %f, "
                  "class_id = %d \n",
                  truth.x, truth.y, truth.w, truth.h, class_id);
              continue;  // if label contains class_id more than number of
                         // classes in the cfg-file and class_id check garbage
                         // value
            }
            if (!truth.x)
              break;  // continue;

            int class_index =
                EntryIndex(l, b, n * l->w * l->h + j * l->w + i, 4 + 1);
            int obj_index = EntryIndex(l, b, n * l->w * l->h + j * l->w + i, 4);
            float objectness = l->output[obj_index];
            if (isnan(objectness) || isinf(objectness))
              l->output[obj_index] = 0;
            int class_id_match = compare_yolo_class(l->output, l->classes,
                class_index, l->w * l->h, objectness, class_id, 0.25f);

            float iou = Box::Iou(pred, truth);
            if (iou > best_match_iou && class_id_match == 1)
            {
              best_match_iou = iou;
            }
            if (iou > best_iou)
            {
              best_iou = iou;
              best_t = t;
            }
          }
          int obj_index = EntryIndex(l, b, n * l->w * l->h + j * l->w + i, 4);
          avg_anyobj += l->output[obj_index];
          l->delta[obj_index] = l->cls_normalizer * (0 - l->output[obj_index]);
          if (best_match_iou > l->ignore_thresh)
          {
            l->delta[obj_index] = 0;
          }
          if (best_iou > l->truth_thresh)
          {
            l->delta[obj_index] =
                l->cls_normalizer * (1 - l->output[obj_index]);

            int class_id = state.truth[best_t * (4 + 1) + b * l->truths + 4];
            if (l->map)
              class_id = l->map[class_id];
            int class_index =
                EntryIndex(l, b, n * l->w * l->h + j * l->w + i, 4 + 1);
            delta_yolo_class(l->output, l->delta, class_index, class_id,
                l->classes, l->w * l->h, 0, l->focal_loss, l->label_smooth_eps,
                l->classes_multipliers);
            Box truth(state.truth + best_t * (4 + 1) + b * l->truths);
            const float class_multiplier =
                (l->classes_multipliers) ? l->classes_multipliers[class_id] :
                                           1.0f;
            delta_yolo_box(truth, l->output, l->biases, l->mask[n], box_index,
                i, j, l->w, l->h, state.net->w, state.net->h, l->delta,
                (2 - truth.w * truth.h), l->w * l->h,
                l->iou_normalizer * class_multiplier, l->iou_loss, 1,
                l->max_delta);
          }
        }
      }
    }
    for (t = 0; t < l->max_boxes; ++t)
    {
      Box truth(state.truth + t * (4 + 1) + b * l->truths);
      if (truth.x < 0 || truth.y < 0 || truth.x > 1 || truth.y > 1 ||
          truth.w < 0 || truth.h < 0)
      {
        char buff[256];
        printf(
            " Wrong label: truth.x = %f, truth.y = %f, truth.w = %f, truth.h = "
            "%f \n",
            truth.x, truth.y, truth.w, truth.h);
        sprintf(buff,
            "echo \"Wrong label: truth.x = %f, truth.y = %f, truth.w = %f, "
            "truth.h = %f\" >> bad_label->list",
            truth.x, truth.y, truth.w, truth.h);
        system(buff);
      }
      int class_id = state.truth[t * (4 + 1) + b * l->truths + 4];
      if (class_id >= l->classes || class_id < 0)
        continue;  // if label contains class_id more than number of classes in
                   // the cfg-file and class_id check garbage value

      if (!truth.x)
        break;  // continue;
      float best_iou = 0;
      int best_n = 0;
      i = (truth.x * l->w);
      j = (truth.y * l->h);
      Box truth_shift = truth;
      truth_shift.x = truth_shift.y = 0;
      for (n = 0; n < l->total; ++n)
      {
        Box pred;
        pred.w = l->biases[2 * n] / state.net->w;
        pred.h = l->biases[2 * n + 1] / state.net->h;
        float iou = Box::Iou(pred, truth_shift);
        if (iou > best_iou)
        {
          best_iou = iou;
          best_n = n;
        }
      }

      int mask_n = int_index(l->mask, best_n, l->n);
      if (mask_n >= 0)
      {
        int class_id = state.truth[t * (4 + 1) + b * l->truths + 4];
        if (l->map)
          class_id = l->map[class_id];

        int box_index =
            EntryIndex(l, b, mask_n * l->w * l->h + j * l->w + i, 0);
        const float class_multiplier =
            (l->classes_multipliers) ? l->classes_multipliers[class_id] : 1.0f;
        Ious all_ious = delta_yolo_box(truth, l->output, l->biases, best_n,
            box_index, i, j, l->w, l->h, state.net->w, state.net->h, l->delta,
            (2 - truth.w * truth.h), l->w * l->h,
            l->iou_normalizer * class_multiplier, l->iou_loss, 1, l->max_delta);

        // range is 0 <= 1
        tot_iou += all_ious.iou;
        tot_iou_loss += 1 - all_ious.iou;
        // range is -1 <= giou <= 1
        tot_giou += all_ious.giou;
        tot_giou_loss += 1 - all_ious.giou;

        tot_diou += all_ious.diou;
        tot_diou_loss += 1 - all_ious.diou;

        tot_ciou += all_ious.ciou;
        tot_ciou_loss += 1 - all_ious.ciou;

        int obj_index =
            EntryIndex(l, b, mask_n * l->w * l->h + j * l->w + i, 4);
        avg_obj += l->output[obj_index];
        l->delta[obj_index] =
            class_multiplier * l->cls_normalizer * (1 - l->output[obj_index]);

        int class_index =
            EntryIndex(l, b, mask_n * l->w * l->h + j * l->w + i, 4 + 1);
        delta_yolo_class(l->output, l->delta, class_index, class_id, l->classes,
            l->w * l->h, &avg_cat, l->focal_loss, l->label_smooth_eps,
            l->classes_multipliers);

        ++count;
        ++class_count;
        if (all_ious.iou > .5)
          recall += 1;
        if (all_ious.iou > .75)
          recall75 += 1;
      }

      // iou_thresh
      for (n = 0; n < l->total; ++n)
      {
        int mask_n = int_index(l->mask, n, l->n);
        if (mask_n >= 0 && n != best_n && l->iou_thresh < 1.0f)
        {
          Box pred;
          pred.w = l->biases[2 * n] / state.net->w;
          pred.h = l->biases[2 * n + 1] / state.net->h;
          float iou = Box::Iou(pred, truth_shift, l->iou_thresh_kind);

          if (iou > l->iou_thresh)
          {
            int class_id = state.truth[t * (4 + 1) + b * l->truths + 4];
            if (l->map)
              class_id = l->map[class_id];

            int box_index =
                EntryIndex(l, b, mask_n * l->w * l->h + j * l->w + i, 0);
            const float class_multiplier =
                (l->classes_multipliers) ? l->classes_multipliers[class_id] :
                                           1.0f;
            Ious all_ious = delta_yolo_box(truth, l->output, l->biases, n,
                box_index, i, j, l->w, l->h, state.net->w, state.net->h,
                l->delta, (2 - truth.w * truth.h), l->w * l->h,
                l->iou_normalizer * class_multiplier, l->iou_loss, 1,
                l->max_delta);

            // range is 0 <= 1
            tot_iou += all_ious.iou;
            tot_iou_loss += 1 - all_ious.iou;
            // range is -1 <= giou <= 1
            tot_giou += all_ious.giou;
            tot_giou_loss += 1 - all_ious.giou;

            tot_diou += all_ious.diou;
            tot_diou_loss += 1 - all_ious.diou;

            tot_ciou += all_ious.ciou;
            tot_ciou_loss += 1 - all_ious.ciou;

            int obj_index =
                EntryIndex(l, b, mask_n * l->w * l->h + j * l->w + i, 4);
            avg_obj += l->output[obj_index];
            l->delta[obj_index] = class_multiplier * l->cls_normalizer *
                                  (1 - l->output[obj_index]);

            int class_index =
                EntryIndex(l, b, mask_n * l->w * l->h + j * l->w + i, 4 + 1);
            delta_yolo_class(l->output, l->delta, class_index, class_id,
                l->classes, l->w * l->h, &avg_cat, l->focal_loss,
                l->label_smooth_eps, l->classes_multipliers);

            ++count;
            ++class_count;
            if (all_ious.iou > .5)
              recall += 1;
            if (all_ious.iou > .75)
              recall75 += 1;
          }
        }
      }
    }

    // averages the deltas obtained by the function: delta_yolo_box()_accumulate
    for (j = 0; j < l->h; ++j)
    {
      for (i = 0; i < l->w; ++i)
      {
        for (n = 0; n < l->n; ++n)
        {
          int box_index = EntryIndex(l, b, n * l->w * l->h + j * l->w + i, 0);
          int class_index =
              EntryIndex(l, b, n * l->w * l->h + j * l->w + i, 4 + 1);
          const int stride = l->w * l->h;

          averages_yolo_deltas(
              class_index, box_index, stride, l->classes, l->delta);
        }
      }
    }
  }

  if (count == 0)
    count = 1;
  if (class_count == 0)
    class_count = 1;

  //*(l->cost) = pow(mag_array(l->delta, l->outputs * l->batch), 2);
  // printf("Region %d Avg IOU: %f, Class: %f, Obj: %f, No Obj: %f, .5R: %f,
  // .75R: %f,  count: %d\n", state.index, avg_iou / count, avg_cat /
  // class_count, avg_obj / count, avg_anyobj / (l->w*l->h*l->n*l->batch),
  // recall / count, recall75 / count, count);

  int stride = l->w * l->h;
  float* no_iou_loss_delta =
      (float*)calloc(l->batch * l->outputs, sizeof(float));
  memcpy(no_iou_loss_delta, l->delta, l->batch * l->outputs * sizeof(float));
  for (b = 0; b < l->batch; ++b)
  {
    for (j = 0; j < l->h; ++j)
    {
      for (i = 0; i < l->w; ++i)
      {
        for (n = 0; n < l->n; ++n)
        {
          int index = EntryIndex(l, b, n * l->w * l->h + j * l->w + i, 0);
          no_iou_loss_delta[index + 0 * stride] = 0;
          no_iou_loss_delta[index + 1 * stride] = 0;
          no_iou_loss_delta[index + 2 * stride] = 0;
          no_iou_loss_delta[index + 3 * stride] = 0;
        }
      }
    }
  }
  float classification_loss =
      l->cls_normalizer *
      pow(mag_array(no_iou_loss_delta, l->outputs * l->batch), 2);
  free(no_iou_loss_delta);
  float loss = pow(mag_array(l->delta, l->outputs * l->batch), 2);
  float iou_loss = loss - classification_loss;

  float avg_iou_loss = 0;
  // gIOU loss + MSE (objectness) loss
  if (l->iou_loss == MSE)
  {
    *(l->cost) = pow(mag_array(l->delta, l->outputs * l->batch), 2);
  }
  else
  {
    // Always compute classification loss both for iou + cls loss and for
    // logging with mse loss
    // TODO: remove IOU loss fields before computing MSE on class
    //   probably split into two arrays
    if (l->iou_loss == GIOU)
    {
      avg_iou_loss =
          count > 0 ? l->iou_normalizer * (tot_giou_loss / count) : 0;
    }
    else
    {
      avg_iou_loss = count > 0 ? l->iou_normalizer * (tot_iou_loss / count) : 0;
    }
    *(l->cost) = avg_iou_loss + classification_loss;
  }

  loss /= l->batch;
  classification_loss /= l->batch;
  iou_loss /= l->batch;

  // fprintf(stderr,
  //     "%s loss, normalizer: (iou: %.2f, cls: %.2f) region %d avg (IOU: %.2f,
  //     " "GIOU: %.2f), class: %.2f, obj: %.2f, no-obj: %.2f, .5R: %.2f, .75R:
  //     "
  //     "%.2f, count: %d, class_loss = %.2f, iou_loss = %.2f, total_loss = "
  //     "%.2f\n",
  //     (l->iou_loss == MSE ? "mse" : (l->iou_loss == GIOU ? "giou" : "iou")),
  //     l->iou_normalizer, l->cls_normalizer, state.index, tot_iou / count,
  //     tot_giou / count, avg_cat / class_count, avg_obj / count,
  //     avg_anyobj / (l->w * l->h * l->n * l->batch), recall / count,
  //     recall75 / count, count, classification_loss, iou_loss, loss);
}

void BackwardYoloLayer(layer* l, NetworkState state)
{
  axpy_cpu(l->batch * l->inputs, 1, l->delta, 1, state.delta, 1);
}

int YoloNumDetections(layer const* l, float thresh)
{
  int count = 0;
  for (int n = 0; n < l->n; ++n)
  {
    for (int i = 0; i < l->w * l->h; ++i)
    {
      int obj_index = EntryIndex(l, 0, n * l->w * l->h + i, 4);
      if (l->output[obj_index] > thresh)
        ++count;
    }
  }
  return count;
}

int GetYoloDetections(
    layer const* l, int net_w, int net_h, float thresh, Detection* dets)
{
  float const* pred = l->output;

  int count = 0;
  for (int n = 0; n < l->n; ++n)
  {
    for (int i = 0; i < l->w * l->h; ++i)
    {
      int loc = n * l->w * l->h + i;
      int obj_idx = EntryIndex(l, 0, loc, 4);

      float objectness = pred[obj_idx];
      if (objectness <= thresh)
        continue;

      int box_idx = EntryIndex(l, 0, loc, 0);
      int col = i % l->w;
      int row = i / l->w;

      dets[count].bbox = GetYoloBox(pred, l->biases, l->mask[n], box_idx, col,
          row, l->w, l->h, net_w, net_h, l->w * l->h);
      dets[count].objectness = objectness;
      dets[count].classes = l->classes;

      for (int j = 0; j < l->classes; ++j)
      {
        int class_idx = EntryIndex(l, 0, loc, 4 + 1 + j);

        float prob = objectness * pred[class_idx];
        dets[count].prob[j] = (prob > thresh) ? prob : 0;
      }
      ++count;
    }
  }

  return count;
}

#ifdef GPU

void ForwardYoloLayerGpu(layer* l, NetworkState state)
{
  simple_copy_ongpu(l->batch * l->inputs, state.input, l->output_gpu);
  for (int b = 0; b < l->batch; ++b)
  {
    for (int n = 0; n < l->n; ++n)
    {
      int index = EntryIndex(l, b, n * l->w * l->h, 0);
      activate_array_ongpu(l->output_gpu + index, 2 * l->w * l->h, LOGISTIC);
      if (l->scale_x_y != 1)
        scal_add_ongpu(2 * l->w * l->h, l->scale_x_y, -0.5 * (l->scale_x_y - 1),
            l->output_gpu + index, 1);

      index = EntryIndex(l, b, n * l->w * l->h, 4);
      activate_array_ongpu(
          l->output_gpu + index, (1 + l->classes) * l->w * l->h, LOGISTIC);
    }
  }
  if (!state.train || l->onlyforward)
  {
    cuda_pull_array_async(l->output_gpu, l->output, l->batch * l->outputs);
    CHECK_CUDA(cudaPeekAtLastError());
    return;
  }

  float* in_cpu = (float*)xcalloc(l->batch * l->inputs, sizeof(float));
  cuda_pull_array(l->output_gpu, l->output, l->batch * l->outputs);
  memcpy(in_cpu, l->output, l->batch * l->outputs * sizeof(float));
  float* truth_cpu = 0;
  if (state.truth)
  {
    int num_truth = l->batch * l->truths;
    truth_cpu = (float*)xcalloc(num_truth, sizeof(float));
    cuda_pull_array(state.truth, truth_cpu, num_truth);
  }
  NetworkState cpu_state = state;
  cpu_state.net = state.net;
  cpu_state.index = state.index;
  cpu_state.train = state.train;
  cpu_state.truth = truth_cpu;
  cpu_state.input = in_cpu;
  ForwardYoloLayer(l, cpu_state);
  cuda_push_array(l->delta_gpu, l->delta, l->batch * l->outputs);
  free(in_cpu);
  if (cpu_state.truth)
    free(cpu_state.truth);
}

void BackwardYoloLayerGpu(layer* l, NetworkState state)
{
  axpy_ongpu(l->batch * l->inputs, state.net->loss_scale, l->delta_gpu, 1,
      state.delta, 1);
}
#endif
