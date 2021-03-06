#include "route_layer.h"

#include <stdio.h>

#include "blas.h"
#include "dark_cuda.h"
#include "utils.h"

void FillRouteLayer(layer* l, int batch, int n, int* input_layers,
    int* input_sizes, int groups, int group_id)
{
  fprintf(stderr, "route ");

  l->type = ROUTE;
  l->batch = batch;
  l->n = n;
  l->input_layers = input_layers;
  l->input_sizes = input_sizes;
  l->groups = groups;
  l->group_id = group_id;

  int outputs = 0;
  for (int i = 0; i < n; ++i)
  {
    fprintf(stderr, " %d", input_layers[i]);
    outputs += input_sizes[i];
  }
  outputs = outputs / groups;
  l->outputs = outputs;
  l->inputs = outputs;
  l->delta = (float*)xcalloc(outputs * batch, sizeof(float));
  l->output = (float*)xcalloc(outputs * batch, sizeof(float));

  l->forward = ForwardRouteLayer;
  l->backward = BackwardRouteLayer;
#ifdef GPU
  l->forward_gpu = ForwardRouteLayerGpu;
  l->backward_gpu = BackwardRouteLayerGpu;

  l->delta_gpu = cuda_make_array(l->delta, outputs * batch);
  l->output_gpu = cuda_make_array(l->output, outputs * batch);
#endif
}

void ResizeRouteLayer(layer* l, Network* net)
{
  int i;
  layer first = net->layers[l->input_layers[0]];
  l->out_w = first.out_w;
  l->out_h = first.out_h;
  l->out_c = first.out_c;
  l->outputs = first.outputs;
  l->input_sizes[0] = first.outputs;
  for (i = 1; i < l->n; ++i)
  {
    int index = l->input_layers[i];
    layer next = net->layers[index];
    l->outputs += next.outputs;
    l->input_sizes[i] = next.outputs;
    if (next.out_w == first.out_w && next.out_h == first.out_h)
    {
      l->out_c += next.out_c;
    }
    else
    {
      printf("Error: Different size of input layers: %d x %d, %d x %d\n",
          next.out_w, next.out_h, first.out_w, first.out_h);
      l->out_h = l->out_w = l->out_c = 0;
      exit(EXIT_FAILURE);
    }
  }
  l->out_c = l->out_c / l->groups;
  l->outputs = l->outputs / l->groups;
  l->inputs = l->outputs;
  l->delta = (float*)xrealloc(l->delta, l->outputs * l->batch * sizeof(float));
  l->output =
      (float*)xrealloc(l->output, l->outputs * l->batch * sizeof(float));

#ifdef GPU
  cuda_free(l->output_gpu);
  cuda_free(l->delta_gpu);
  l->output_gpu = cuda_make_array(l->output, l->outputs * l->batch);
  l->delta_gpu = cuda_make_array(l->delta, l->outputs * l->batch);
#endif
}

void ForwardRouteLayer(layer* l, NetworkState state)
{
  int offset = 0;
  for (int i = 0; i < l->n; ++i)
  {
    int index = l->input_layers[i];
    float* input = state.net->layers[index].output;
    int input_size = l->input_sizes[i];
    int part_input_size = input_size / l->groups;
    for (int j = 0; j < l->batch; ++j)
    {
      copy_cpu(part_input_size,
          input + j * input_size + part_input_size * l->group_id, 1,
          l->output + offset + j * l->outputs, 1);
    }
    offset += part_input_size;
  }
}

void BackwardRouteLayer(layer* l, NetworkState state)
{
  int offset = 0;
  for (int i = 0; i < l->n; ++i)
  {
    int index = l->input_layers[i];
    float* delta = state.net->layers[index].delta;
    int input_size = l->input_sizes[i];
    int part_input_size = input_size / l->groups;
    for (int j = 0; j < l->batch; ++j)
    {
      axpy_cpu(part_input_size, 1, l->delta + offset + j * l->outputs, 1,
          delta + j * input_size + part_input_size * l->group_id, 1);
    }
    offset += part_input_size;
  }
}

#ifdef GPU
void ForwardRouteLayerGpu(layer* l, NetworkState state)
{
  int offset = 0;
  for (int i = 0; i < l->n; ++i)
  {
    int index = l->input_layers[i];
    float* input = state.net->layers[index].output_gpu;
    int input_size = l->input_sizes[i];
    int part_input_size = input_size / l->groups;
    for (int j = 0; j < l->batch; ++j)
    {
      simple_copy_ongpu(part_input_size,
          input + j * input_size + part_input_size * l->group_id,
          l->output_gpu + offset + j * l->outputs);
    }
    offset += part_input_size;
  }
}

void BackwardRouteLayerGpu(layer* l, NetworkState state)
{
  int offset = 0;
  for (int i = 0; i < l->n; ++i)
  {
    int index = l->input_layers[i];
    float* delta = state.net->layers[index].delta_gpu;
    int input_size = l->input_sizes[i];
    int part_input_size = input_size / l->groups;
    for (int j = 0; j < l->batch; ++j)
    {
      axpy_ongpu(part_input_size, 1, l->delta_gpu + offset + j * l->outputs, 1,
          delta + j * input_size + part_input_size * l->group_id, 1);
    }
    offset += part_input_size;
  }
}
#endif
