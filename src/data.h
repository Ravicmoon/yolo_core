#pragma once
#include <pthread.h>

#include <vector>

#include "list.h"
#include "matrix.h"
#include "yolo_core.h"

// typedef struct{
//    int w, h;
//    matrix X;
//    matrix y;
//    int shallow;
//    int *num_boxes;
//    box **boxes;
//} data;

// typedef enum {
//    CLASSIFICATION_DATA, DETECTION_DATA, CAPTCHA_DATA, REGION_DATA,
//    IMAGE_DATA, LETTERBOX_DATA, COMPARE_DATA, WRITING_DATA, SWAG_DATA,
//    TAG_DATA, OLD_CLASSIFICATION_DATA, STUDY_DATA, DET_DATA, SUPER_DATA
//} data_type;
/*
typedef struct load_args{
    int threads;
    char **paths;
    char *path;
    int n;
    int m;
    char **labels;
    int h;
    int w;
        int c; // color depth
        int out_w;
    int out_h;
    int nh;
    int nw;
    int num_boxes;
    int min, max, size;
    int classes;
    int background;
    int scale;
        int small_object;
    float jitter;
    int flip;
    float angle;
    float aspect;
    float saturation;
    float exposure;
    float hue;
    data *d;
    image *im;
    image *resized;
    data_type type;
    tree *hierarchy;
} load_args;

typedef struct{
    int id;
    float x,y,w,h;
    float left, right, top, bottom;
} box_label;

void free_data(data d);

pthread_t load_data(load_args args);

pthread_t load_data_in_thread(load_args args);
*/
data load_data_detection(int n, char** paths, int m, int w, int h, int c,
    int boxes, int classes, int use_flip, int gaussian_noise, int use_blur,
    int use_mixup, float jitter, float hue, float saturation, float exposure,
    int mini_batch, int show_imgs);

std::vector<BoxLabel> ReadBoxAnnot(std::string filename);

list* get_paths(char const* filename);
std::vector<std::string> GetList(std::string filename);
data GetPartialData(data d, int part, int total);
void get_next_batch(data d, int n, int offset, float* X, float* y);
data concat_data(data d1, data d2);
data concat_datas(data* d, int n);
