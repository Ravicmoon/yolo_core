// Gaussian YOLOv3 implementation
#ifndef GAUSSIAN_YOLO_LAYER_H
#define GAUSSIAN_YOLO_LAYER_H

#include "network.h"
#include "yolo_core.h"

layer make_gaussian_yolo_layer(int batch, int w, int h, int n, int total,
    int* mask, int classes, int max_boxes);
void forward_gaussian_yolo_layer(const layer l, NetworkState state);
void backward_gaussian_yolo_layer(const layer l, NetworkState state);
void resize_gaussian_yolo_layer(layer* l, int w, int h);
int GaussianYoloNumDetections(layer l, float thresh);
int get_gaussian_yolo_detections(layer l, int w, int h, int netw, int neth,
    float thresh, int* map, int relative, Detection* dets, int letter);
void correct_gaussian_yolo_boxes(Detection* dets, int n, int w, int h, int netw,
    int neth, int relative, int letter);

#ifdef GPU
void forward_gaussian_yolo_layer_gpu(const layer l, NetworkState state);
void backward_gaussian_yolo_layer_gpu(layer l, NetworkState state);
#endif

#endif
