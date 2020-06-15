#include <stdlib.h>

#include "box.h"
#include "cost_layer.h"
#include "image.h"
#include "network.h"
#include "option_list.h"
#include "parser.h"
#include "region_layer.h"
#include "utils.h"
#include "yolo_core.h"

void TrainDetector(char const* data_file, char const* model_file,
    char const* weights_file, int* gpus, int num_gpus, int clear, int show_imgs,
    int calc_map, int benchmark_layers)
{
  list* options = ReadDataCfg(data_file);
  char* train_images = FindOptionStr(options, "train", "data/train.txt");
  char* valid_images = FindOptionStr(options, "valid", train_images);
  char* backup_dir = FindOptionStr(options, "backup", "/backup/");

  if (!Exists(train_images))
  {
    printf("%s does not exists", train_images);
    return;
  }

  if (!Exists(backup_dir))
    MakeDir(backup_dir, 0);

  Network net_map = {0};
  if (calc_map)
  {
    if (!Exists(valid_images))
    {
      printf("%s does not exists", valid_images);
      return;
    }

    cuda_set_device(gpus[0]);
    printf(" Prepare additional network for mAP calculation...\n");
    ParseNetworkCfgCustom(&net_map, model_file, 1, 1);
    net_map.benchmark_layers = benchmark_layers;

    for (int k = 0; k < net_map.n - 1; ++k)
    {
      free_layer(&net_map.layers[k], true);
    }

    char* name_list = FindOptionStr(options, "names", "data/names.list");
    int names_size = 0;
    char** names = GetLabels(name_list, &names_size);

    int const num_classes = net_map.layers[net_map.n - 1].classes;
    if (num_classes != names_size)
    {
      printf(
          "\n Error: in the file %s number of names %d that isn't equal to "
          "classes=%d in the file %s \n",
          name_list, names_size, num_classes, model_file);
      if (num_classes > names_size)
        getchar();
    }
    free_ptrs((void**)names, names_size);
  }

  srand(time(0));

  char* base = BaseCfg(model_file);
  printf("%s\n", base);

  Network* nets = (Network*)xcalloc(num_gpus, sizeof(Network));
  for (int k = 0; k < num_gpus; ++k)
  {
#ifdef GPU
    cuda_set_device(gpus[k]);
#endif
    ParseNetworkCfg(&nets[k], model_file);
    nets[k].benchmark_layers = benchmark_layers;
    if (weights_file)
    {
      LoadWeights(&nets[k], weights_file);
    }
    if (clear)
    {
      *nets[k].seen = 0;
      *nets[k].cur_iteration = 0;
    }
    nets[k].learning_rate *= num_gpus;
  }

  Network* net = &nets[0];

  int const actual_batch_size = net->batch * net->subdiv;
  if (actual_batch_size == 1)
  {
    printf("Error: batch size = 1");
    return;
  }
  else if (actual_batch_size < 8)
  {
    printf("Warning: batch size < 8");
  }

  int imgs = net->batch * net->subdiv * num_gpus;
  printf("Learning rate: %g, Momentum: %g, Decay: %g\n", net->learning_rate,
      net->momentum, net->decay);

  data train, buffer;

  layer* l = &net->layers[net->n - 1];

  int classes = l->classes;
  float jitter = l->jitter;

  list* plist = get_paths(train_images);
  int num_train_imgs = plist->size;
  char** paths = (char**)ListToArray(plist);

  int const init_w = net->w;
  int const init_h = net->h;
  int const init_b = net->batch;
  int iter_save = GetCurrentIteration(net);
  int iter_save_last = GetCurrentIteration(net);
  int iter_map = GetCurrentIteration(net);
  float map = -FLT_MAX;
  float best_map = map;

  load_args args = {0};
  args.w = net->w;
  args.h = net->h;
  args.c = net->c;
  args.paths = paths;
  args.n = imgs;
  args.m = plist->size;
  args.classes = classes;
  args.flip = net->flip;
  args.jitter = jitter;
  args.num_boxes = l->max_boxes;
  net->num_boxes = args.num_boxes;
  net->train_images_num = num_train_imgs;
  args.d = &buffer;
  args.type = DETECTION_DATA;
  args.threads = 64;  // 16 or 64

  args.angle = net->angle;
  args.gaussian_noise = net->gaussian_noise;
  args.blur = net->blur;
  args.mixup = net->mixup;
  args.exposure = net->exposure;
  args.saturation = net->saturation;
  args.hue = net->hue;
  args.letter_box = net->letter_box;
  args.show_imgs = show_imgs;

  args.threads = 6 * num_gpus;  // 3 for - Amazon EC2 Tesla V100: p3.2xlarge (8
                                // logical cores) - p3.16xlarge
  // args.threads = 12 * ngpus;    // Ryzen 7 2700X (16 logical cores)
  char windows_name[100];
  sprintf(windows_name, "chart_%s.png", base);
  mat_cv* img =
      draw_train_chart(windows_name, 20, net->max_batches, 100, 1024, nullptr);

  if (net->track)
  {
    args.track = net->track;
    args.augment_speed = net->augment_speed;
    if (net->seq_subdiv)
      args.threads = net->seq_subdiv * num_gpus;
    else
      args.threads = net->subdiv * num_gpus;
    args.mini_batch = net->batch / net->time_steps;
    printf("batch = %d, subdiv = %d, time_steps = %d, mini_batch = %d \n",
        net->batch, net->subdiv, net->time_steps, args.mini_batch);
  }

  pthread_t load_thread = load_data(args);

  int count = 0;
  double time_remaining = 0.0;
  double avg_time = -DBL_MAX;
  double alpha_time = 0.01;
  float avg_loss = -FLT_MAX;

  while (GetCurrentIteration(net) < net->max_batches)
  {
    if (l->random && count++ % 10 == 0)
    {
      float rand_coef = 1.4f;
      if (abs(l->random - 1.0f) > FLT_EPSILON)
        rand_coef = l->random;
      printf("Resizing, random_coef = %.2f \n", rand_coef);

      float rand_scale = RandScale(rand_coef);
      int dim_w =
          roundl(rand_scale * init_w / net->resize_step + 1) * net->resize_step;
      int dim_h =
          roundl(rand_scale * init_h / net->resize_step + 1) * net->resize_step;

      if (rand_scale < 1 && (dim_w > init_w || dim_h > init_h))
      {
        dim_w = init_w;
        dim_h = init_h;
      }

      int max_dim_w =
          roundl(rand_coef * init_w / net->resize_step + 1) * net->resize_step;
      int max_dim_h =
          roundl(rand_coef * init_h / net->resize_step + 1) * net->resize_step;

      // at the beginning (check if enough memory) and at the end (calc rolling
      // mean/variance)
      if (avg_loss < 0 || GetCurrentIteration(net) > net->max_batches - 100)
      {
        dim_w = max_dim_w;
        dim_h = max_dim_h;
      }

      if (dim_w < net->resize_step)
        dim_w = net->resize_step;
      if (dim_h < net->resize_step)
        dim_h = net->resize_step;

      int dim_b = (init_b * max_dim_w * max_dim_h) / (dim_w * dim_h);
      int new_dim_b = (int)(dim_b * 0.8);
      if (new_dim_b > init_b)
        dim_b = new_dim_b;

      args.w = dim_w;
      args.h = dim_h;

      printf("%d x %d\n", dim_w, dim_h);

      pthread_join(load_thread, nullptr);
      train = buffer;
      free_data(train);
      load_thread = load_data(args);

      for (int k = 0; k < num_gpus; ++k)
      {
        ResizeNetwork(nets + k, dim_w, dim_h);
      }
    }

    double load_start = GetTimePoint();
    pthread_join(load_thread, nullptr);

    train = buffer;
    if (net->track)
    {
      net->seq_subdiv = GetCurrentSeqSubdivisions(net);
      args.threads = net->seq_subdiv * num_gpus;
      printf("seq_subdiv = %d, sequence = %d \n", net->seq_subdiv,
          GetSequenceValue(net));
    }
    load_thread = load_data(args);

    double load_end = GetTimePoint();
    printf("Load time: %lf s\n", (load_end - load_start) / 1000.0);

    float loss = 0;
#ifdef GPU
    if (num_gpus == 1)
      loss = TrainNetwork(net, train);
    else
      loss = TrainNetworks(nets, num_gpus, train, 4);
#else
    loss = TrainNetwork(net, train);
#endif

    if (avg_loss < 0)
      avg_loss = loss;
    avg_loss = avg_loss * 0.9f + loss * 0.1f;

    int const iter = GetCurrentIteration(net);

    // calculate mAP for each 4 Epochs
    int calc_map_for_each =
        __max(100, 4 * num_train_imgs / (net->batch * net->subdiv));
    int next_map_calc = __max(net->burn_in, iter_map + calc_map_for_each);

    if (calc_map)
    {
      printf("Next mAP calculation: %d\n", next_map_calc);
      if (map > 0)
        printf(
            "mAP@0.5 = %2.2f%%, best = %2.2f%%\n", map * 100, best_map * 100);
    }

    if (net->cudnn_half)
    {
      if (iter < net->burn_in * 3)
        printf("Tensor Cores are disabled until the first %d iterations\n",
            3 * net->burn_in);
      else
        printf("Tensor Cores are used\n");
    }

    int draw_precision = 0;
    if (calc_map && (iter >= next_map_calc || iter == net->max_batches))
    {
      if (l->random)
      {
        printf("Resizing to initial size: %d x %d ", init_w, init_h);
        args.w = init_w;
        args.h = init_h;

        pthread_join(load_thread, nullptr);
        free_data(train);
        train = buffer;
        load_thread = load_data(args);
        for (int k = 0; k < num_gpus; ++k)
        {
          ResizeNetwork(nets + k, init_w, init_h);
        }
      }

      CopyNetWeights(net, &net_map);

      iter_map = iter;
      map = ValidateDetector(data_file, model_file, weights_file, 0.25, 0.5, 0,
          net->letter_box, &net_map);
      printf("mAP@0.5 = %f\n", map);

      if (map > best_map)
      {
        best_map = map;
        printf("New best mAP!\n");
        char buff[256];
        sprintf(buff, "%s/%s_best.weights", backup_dir, base);
        SaveWeights(net, buff);
      }
      draw_precision = 1;
    }

    double alg_end = GetTimePoint();
    double time_remaining = ((net->max_batches - iter) / num_gpus) *
                            (alg_end - load_start) / 1e6 / 3600;

    if (avg_time < 0)
      avg_time = time_remaining;
    else
      avg_time = alpha_time * time_remaining + (1 - alpha_time) * avg_time;

    printf(
        "[%04d] loss: %f, avg_loss: %f, rate: %f, images: %d, %lf hours left\n",
        iter, loss, avg_loss, GetCurrentRate(net), iter * imgs, avg_time);

    draw_train_loss(windows_name, img, 1024, avg_loss, 20, iter,
        net->max_batches, map, draw_precision, "mAP%", avg_time);

    if (iter >= (iter_save + 1000) || iter % 1000 == 0)
    {
      iter_save = iter;
#ifdef GPU
      if (num_gpus != 1)
        SyncNetworks(nets, num_gpus, 0);
#endif
      char buff[256];
      sprintf(buff, "%s/%s_%d.weights", backup_dir, base, iter);
      SaveWeights(net, buff);
    }

    if (iter >= (iter_save_last + 100) || (iter % 100 == 0 && iter > 1))
    {
      iter_save_last = iter;
#ifdef GPU
      if (num_gpus != 1)
        SyncNetworks(nets, num_gpus, 0);
#endif
      char buff[256];
      sprintf(buff, "%s/%s_last.weights", backup_dir, base);
      SaveWeights(net, buff);
    }

    free_data(train);
  }

#ifdef GPU
  if (num_gpus != 1)
    SyncNetworks(nets, num_gpus, 0);
#endif

  char buff[256];
  sprintf(buff, "%s/%s_final.weights", backup_dir, base);
  SaveWeights(net, buff);

  release_mat(&img);
  destroy_all_windows_cv();

  // free memory
  pthread_join(load_thread, nullptr);
  free_data(buffer);

  free_load_threads(&args);

  free(base);
  free(paths);
  FreeListContents(plist);
  FreeList(plist);

  FreeListContentsKvp(options);
  FreeList(options);

  for (int k = 0; k < num_gpus; ++k)
  {
    FreeNetwork(&nets[k]);
  }
  free(nets);

  if (calc_map)
  {
    net_map.n = 0;
    FreeNetwork(&net_map);
  }
}

typedef struct
{
  Box b;
  float p;
  int class_id;
  int image_index;
  int truth_flag;
  int unique_truth_index;
} box_prob;

int detections_comparator(const void* pa, const void* pb)
{
  box_prob a = *(const box_prob*)pa;
  box_prob b = *(const box_prob*)pb;
  float diff = a.p - b.p;
  if (diff < 0)
    return 1;
  else if (diff > 0)
    return -1;
  return 0;
}

float ValidateDetector(char const* data_file, char const* model_file,
    char const* weights_file, float const thresh_calc_avg_iou,
    float const iou_thresh, int const map_points, int letter_box,
    Network* existing_net)
{
  int j;
  list* options = ReadDataCfg(data_file);
  char* valid_images = FindOptionStr(options, "valid", "data/train.txt");
  char* difficult_valid_images = FindOptionStr(options, "difficult", NULL);
  char* name_list = FindOptionStr(options, "names", "data/names.list");
  int names_size = 0;
  char** names = GetLabels(name_list, &names_size);

  Network net;
  // int initial_batch;
  if (existing_net)
  {
    char* train_images = FindOptionStr(options, "train", "data/train.txt");
    valid_images = FindOptionStr(options, "valid", train_images);
    net = *existing_net;
  }
  else
  {
    ParseNetworkCfgCustom(&net, model_file, 1, 1);  // set batch=1
    if (weights_file)
      LoadWeights(&net, weights_file);

    FuseConvBatchNorm(&net);
    calculate_binary_weights(net);
  }
  if (net.layers[net.n - 1].classes != names_size)
  {
    printf(
        "\n Error: in the file %s number of names %d that isn't equal to "
        "classes=%d in the file %s \n",
        name_list, names_size, net.layers[net.n - 1].classes, model_file);
    getchar();
  }
  srand(time(0));
  printf("\n calculation mAP (mean average precision)...\n");

  list* plist = get_paths(valid_images);
  char** paths = (char**)ListToArray(plist);

  char** paths_dif = NULL;
  if (difficult_valid_images)
  {
    list* plist_dif = get_paths(difficult_valid_images);
    paths_dif = (char**)ListToArray(plist_dif);
  }

  layer l = net.layers[net.n - 1];
  int classes = l.classes;

  int m = plist->size;
  int i = 0;
  int t;

  const float thresh = .005;
  const float nms = .45;
  // const float iou_thresh = 0.5;

  int nthreads = 4;
  if (m < 4)
    nthreads = m;
  Image* val = (Image*)xcalloc(nthreads, sizeof(Image));
  Image* val_resized = (Image*)xcalloc(nthreads, sizeof(Image));
  Image* buf = (Image*)xcalloc(nthreads, sizeof(Image));
  Image* buf_resized = (Image*)xcalloc(nthreads, sizeof(Image));
  pthread_t* thr = (pthread_t*)xcalloc(nthreads, sizeof(pthread_t));

  load_args args = {0};
  args.w = net.w;
  args.h = net.h;
  args.c = net.c;
  if (letter_box)
    args.type = LETTERBOX_DATA;
  else
    args.type = IMAGE_DATA;

  // const float thresh_calc_avg_iou = 0.24;
  float avg_iou = 0;
  int tp_for_thresh = 0;
  int fp_for_thresh = 0;

  box_prob* detections = (box_prob*)xcalloc(1, sizeof(box_prob));
  int detections_count = 0;
  int unique_truth_count = 0;

  int* truth_classes_count = (int*)xcalloc(classes, sizeof(int));

  // For multi-class precision and recall computation
  float* avg_iou_per_class = (float*)xcalloc(classes, sizeof(float));
  int* tp_for_thresh_per_class = (int*)xcalloc(classes, sizeof(int));
  int* fp_for_thresh_per_class = (int*)xcalloc(classes, sizeof(int));

  for (t = 0; t < nthreads; ++t)
  {
    args.path = paths[i + t];
    args.im = &buf[t];
    args.resized = &buf_resized[t];
    thr[t] = load_data_in_thread(args);
  }
  time_t start = time(0);
  for (i = nthreads; i < m + nthreads; i += nthreads)
  {
    fprintf(stderr, "\r%d", i);
    for (t = 0; t < nthreads && (i + t - nthreads) < m; ++t)
    {
      pthread_join(thr[t], 0);
      val[t] = buf[t];
      val_resized[t] = buf_resized[t];
    }
    for (t = 0; t < nthreads && (i + t) < m; ++t)
    {
      args.path = paths[i + t];
      args.im = &buf[t];
      args.resized = &buf_resized[t];
      thr[t] = load_data_in_thread(args);
    }
    for (t = 0; t < nthreads && i + t - nthreads < m; ++t)
    {
      const int image_index = i + t - nthreads;
      char* path = paths[image_index];
      char* id = BaseCfg(path);
      float* X = val_resized[t].data;
      NetworkPredict(&net, X);

      int nboxes = 0;
      float hier_thresh = 0;
      Detection* dets;
      if (args.type == LETTERBOX_DATA)
      {
        dets = GetNetworkBoxes(&net, val[t].w, val[t].h, thresh, hier_thresh, 0,
            1, &nboxes, letter_box);
      }
      else
      {
        dets = GetNetworkBoxes(
            &net, 1, 1, thresh, hier_thresh, 0, 0, &nboxes, letter_box);
      }
      // detection *dets = get_network_boxes(&net, val[t].w, val[t].h, thresh,
      // hier_thresh, 0, 1, &nboxes, letter_box); // for letter_box=1
      if (nms)
      {
        if (l.nms_kind == DEFAULT_NMS)
          NmsSort(dets, nboxes, l.classes, nms);
        else
          DiouNmsSort(dets, nboxes, l.classes, nms, l.nms_kind, l.beta_nms);
      }

      char label_file[4096];
      ReplaceImage2Label(path, label_file);
      int num_labels = 0;
      box_label* truth = read_boxes(label_file, &num_labels);
      int j;
      for (j = 0; j < num_labels; ++j)
      {
        truth_classes_count[truth[j].id]++;
      }

      // difficult
      box_label* truth_dif = NULL;
      int num_labels_dif = 0;
      if (paths_dif)
      {
        char* path_dif = paths_dif[image_index];

        char labelpath_dif[4096];
        ReplaceImage2Label(path_dif, labelpath_dif);

        truth_dif = read_boxes(labelpath_dif, &num_labels_dif);
      }

      const int checkpoint_detections_count = detections_count;

      int i;
      for (i = 0; i < nboxes; ++i)
      {
        int class_id;
        for (class_id = 0; class_id < classes; ++class_id)
        {
          float prob = dets[i].prob[class_id];
          if (prob > 0)
          {
            detections_count++;
            detections = (box_prob*)xrealloc(
                detections, detections_count * sizeof(box_prob));
            detections[detections_count - 1].b = dets[i].bbox;
            detections[detections_count - 1].p = prob;
            detections[detections_count - 1].image_index = image_index;
            detections[detections_count - 1].class_id = class_id;
            detections[detections_count - 1].truth_flag = 0;
            detections[detections_count - 1].unique_truth_index = -1;

            int truth_index = -1;
            float max_iou = 0;
            for (j = 0; j < num_labels; ++j)
            {
              Box t(truth[j].x, truth[j].y, truth[j].w, truth[j].h);
              // printf(" IoU = %f, prob = %f, class_id = %d, truth[j].id = %d
              // \n",
              //    box_iou(dets[i].bbox, t), prob, class_id, truth[j].id);
              float current_iou = Box::Iou(dets[i].bbox, t);
              if (current_iou > iou_thresh && class_id == truth[j].id)
              {
                if (current_iou > max_iou)
                {
                  max_iou = current_iou;
                  truth_index = unique_truth_count + j;
                }
              }
            }

            // best IoU
            if (truth_index > -1)
            {
              detections[detections_count - 1].truth_flag = 1;
              detections[detections_count - 1].unique_truth_index = truth_index;
            }
            else
            {
              // if object is difficult then remove detection
              for (j = 0; j < num_labels_dif; ++j)
              {
                Box t(truth_dif[j].x, truth_dif[j].y, truth_dif[j].w,
                    truth_dif[j].h);
                float current_iou = Box::Iou(dets[i].bbox, t);
                if (current_iou > iou_thresh && class_id == truth_dif[j].id)
                {
                  --detections_count;
                  break;
                }
              }
            }

            // calc avg IoU, true-positives, false-positives for required
            // Threshold
            if (prob > thresh_calc_avg_iou)
            {
              int z, found = 0;
              for (z = checkpoint_detections_count; z < detections_count - 1;
                   ++z)
              {
                if (detections[z].unique_truth_index == truth_index)
                {
                  found = 1;
                  break;
                }
              }

              if (truth_index > -1 && found == 0)
              {
                avg_iou += max_iou;
                ++tp_for_thresh;
                avg_iou_per_class[class_id] += max_iou;
                tp_for_thresh_per_class[class_id]++;
              }
              else
              {
                fp_for_thresh++;
                fp_for_thresh_per_class[class_id]++;
              }
            }
          }
        }
      }

      unique_truth_count += num_labels;

      FreeDetections(dets, nboxes);
      free(id);
      free_image(val[t]);
      free_image(val_resized[t]);
    }
  }

  // for (t = 0; t < nthreads; ++t) {
  //    pthread_join(thr[t], 0);
  //}

  if ((tp_for_thresh + fp_for_thresh) > 0)
    avg_iou = avg_iou / (tp_for_thresh + fp_for_thresh);

  int class_id;
  for (class_id = 0; class_id < classes; class_id++)
  {
    if ((tp_for_thresh_per_class[class_id] +
            fp_for_thresh_per_class[class_id]) > 0)
      avg_iou_per_class[class_id] =
          avg_iou_per_class[class_id] / (tp_for_thresh_per_class[class_id] +
                                            fp_for_thresh_per_class[class_id]);
  }

  // SORT(detections)
  qsort(detections, detections_count, sizeof(box_prob), detections_comparator);

  typedef struct
  {
    double precision;
    double recall;
    int tp, fp, fn;
  } pr_t;

  // for PR-curve
  pr_t** pr = (pr_t**)xcalloc(classes, sizeof(pr_t*));
  for (i = 0; i < classes; ++i)
  {
    pr[i] = (pr_t*)xcalloc(detections_count, sizeof(pr_t));
  }
  printf("\n detections_count = %d, unique_truth_count = %d  \n",
      detections_count, unique_truth_count);

  int* detection_per_class_count = (int*)xcalloc(classes, sizeof(int));
  for (j = 0; j < detections_count; ++j)
  {
    detection_per_class_count[detections[j].class_id]++;
  }

  int* truth_flags = (int*)xcalloc(unique_truth_count, sizeof(int));

  int rank;
  for (rank = 0; rank < detections_count; ++rank)
  {
    if (rank % 100 == 0)
      printf(" rank = %d of ranks = %d \r", rank, detections_count);

    if (rank > 0)
    {
      int class_id;
      for (class_id = 0; class_id < classes; ++class_id)
      {
        pr[class_id][rank].tp = pr[class_id][rank - 1].tp;
        pr[class_id][rank].fp = pr[class_id][rank - 1].fp;
      }
    }

    box_prob d = detections[rank];
    // if (detected && isn't detected before)
    if (d.truth_flag == 1)
    {
      if (truth_flags[d.unique_truth_index] == 0)
      {
        truth_flags[d.unique_truth_index] = 1;
        pr[d.class_id][rank].tp++;  // true-positive
      }
      else
        pr[d.class_id][rank].fp++;
    }
    else
    {
      pr[d.class_id][rank].fp++;  // false-positive
    }

    for (i = 0; i < classes; ++i)
    {
      const int tp = pr[i][rank].tp;
      const int fp = pr[i][rank].fp;
      const int fn = truth_classes_count[i] -
                     tp;  // false-negative = objects - true-positive
      pr[i][rank].fn = fn;

      if ((tp + fp) > 0)
        pr[i][rank].precision = (double)tp / (double)(tp + fp);
      else
        pr[i][rank].precision = 0;

      if ((tp + fn) > 0)
        pr[i][rank].recall = (double)tp / (double)(tp + fn);
      else
        pr[i][rank].recall = 0;

      if (rank == (detections_count - 1) &&
          detection_per_class_count[i] != (tp + fp))
      {  // check for last rank
        printf(
            " class_id: %d - detections = %d, tp+fp = %d, tp = %d, fp = %d \n",
            i, detection_per_class_count[i], tp + fp, tp, fp);
      }
    }
  }

  free(truth_flags);

  double mean_average_precision = 0;

  for (i = 0; i < classes; ++i)
  {
    double avg_precision = 0;

    // MS COCO - uses 101-Recall-points on PR-chart.
    // PascalVOC2007 - uses 11-Recall-points on PR-chart.
    // PascalVOC2010-2012 - uses Area-Under-Curve on PR-chart.
    // ImageNet - uses Area-Under-Curve on PR-chart.

    // correct mAP calculation: ImageNet, PascalVOC 2010-2012
    if (map_points == 0)
    {
      double last_recall = pr[i][detections_count - 1].recall;
      double last_precision = pr[i][detections_count - 1].precision;
      for (rank = detections_count - 2; rank >= 0; --rank)
      {
        double delta_recall = last_recall - pr[i][rank].recall;
        last_recall = pr[i][rank].recall;

        if (pr[i][rank].precision > last_precision)
        {
          last_precision = pr[i][rank].precision;
        }

        avg_precision += delta_recall * last_precision;
      }
    }
    // MSCOCO - 101 Recall-points, PascalVOC - 11 Recall-points
    else
    {
      int point;
      for (point = 0; point < map_points; ++point)
      {
        double cur_recall = point * 1.0 / (map_points - 1);
        double cur_precision = 0;
        for (rank = 0; rank < detections_count; ++rank)
        {
          if (pr[i][rank].recall >= cur_recall)
          {  // > or >=
            if (pr[i][rank].precision > cur_precision)
            {
              cur_precision = pr[i][rank].precision;
            }
          }
        }
        // printf("class_id = %d, point = %d, cur_recall = %.4f, cur_precision =
        // %.4f \n", i, point, cur_recall, cur_precision);

        avg_precision += cur_precision;
      }
      avg_precision = avg_precision / map_points;
    }

    printf("class_id = %d, name = %s, ap = %2.2f%%   \t (TP = %d, FP = %d) \n",
        i, names[i], avg_precision * 100, tp_for_thresh_per_class[i],
        fp_for_thresh_per_class[i]);

    mean_average_precision += avg_precision;
  }

  const float cur_precision =
      (float)tp_for_thresh / ((float)tp_for_thresh + (float)fp_for_thresh);
  const float cur_recall =
      (float)tp_for_thresh /
      ((float)tp_for_thresh + (float)(unique_truth_count - tp_for_thresh));
  const float f1_score =
      2.F * cur_precision * cur_recall / (cur_precision + cur_recall);
  printf(
      "\n for conf_thresh = %1.2f, precision = %1.2f, recall = %1.2f, F1-score "
      "= %1.2f \n",
      thresh_calc_avg_iou, cur_precision, cur_recall, f1_score);

  printf(
      " for conf_thresh = %0.2f, TP = %d, FP = %d, FN = %d, average IoU = "
      "%2.2f %% \n",
      thresh_calc_avg_iou, tp_for_thresh, fp_for_thresh,
      unique_truth_count - tp_for_thresh, avg_iou * 100);

  mean_average_precision = mean_average_precision / classes;
  printf("\n IoU threshold = %2.0f %%, ", iou_thresh * 100);
  if (map_points)
    printf("used %d Recall-points \n", map_points);
  else
    printf("used Area-Under-Curve for each unique Recall \n");

  printf(" mean average precision (mAP@%0.2f) = %f, or %2.2f %% \n", iou_thresh,
      mean_average_precision, mean_average_precision * 100);

  for (i = 0; i < classes; ++i)
  {
    free(pr[i]);
  }
  free(pr);
  free(detections);
  free(truth_classes_count);
  free(detection_per_class_count);

  free(avg_iou_per_class);
  free(tp_for_thresh_per_class);
  free(fp_for_thresh_per_class);

  fprintf(stderr, "Total Detection Time: %d Seconds\n", (int)(time(0) - start));
  printf("\nSet -points flag:\n");
  printf(" `-points 101` for MS COCO \n");
  printf(
      " `-points 11` for PascalVOC 2007 (uncomment `difficult` in voc.data) "
      "\n");
  printf(
      " `-points 0` (AUC) for ImageNet, PascalVOC 2010-2012, your custom "
      "dataset\n");

  // free memory
  free_ptrs((void**)names, net.layers[net.n - 1].classes);
  FreeListContentsKvp(options);
  FreeList(options);

  if (!existing_net)
    FreeNetwork(&net);

  if (val)
    free(val);
  if (val_resized)
    free(val_resized);
  if (thr)
    free(thr);
  if (buf)
    free(buf);
  if (buf_resized)
    free(buf_resized);

  return mean_average_precision;
}