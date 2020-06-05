#ifndef REGION_LAYER_H
#define REGION_LAYER_H

#include "network.h"

typedef layer region_layer;

#ifdef __cplusplus
extern "C" {
#endif
region_layer make_region_layer(
    int batch, int w, int h, int n, int classes, int coords, int max_boxes);
void forward_region_layer(const region_layer l, NetworkState state);
void backward_region_layer(const region_layer l, NetworkState state);
void get_region_boxes(layer l, int w, int h, float thresh, float** probs,
    Box* boxes, int only_objectness, int* map);
void resize_region_layer(layer* l, int w, int h);
void get_region_detections(layer l, int w, int h, int netw, int neth,
    float thresh, int* map, float tree_thresh, int relative, Detection* dets);
void correct_region_boxes(
    Detection* dets, int n, int w, int h, int netw, int neth, int relative);
void zero_objectness(layer l);

#ifdef GPU
void forward_region_layer_gpu(const region_layer l, NetworkState state);
void backward_region_layer_gpu(region_layer l, NetworkState state);
#endif

#ifdef __cplusplus
}
#endif
#endif
