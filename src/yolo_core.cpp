#include "yolo_core.h"

#include <stdio.h>
#include <stdlib.h>

#include "visualize.h"

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#include <algorithm>

#ifdef GPU
#include <cuda.h>
#endif

#include <gflags/gflags.h>

#include <opencv2/opencv.hpp>

DEFINE_bool(clear, false, "Clear weights in model");
DEFINE_bool(show_imgs, false, "");
DEFINE_bool(save_output, false, "Save output to image or video");
DEFINE_bool(calc_map, true, "Calculate mAP during training");

DEFINE_int32(benchmark_layers, 0, "Indexes of layers to be benchmarked");
DEFINE_int32(num_gpus, 1, "Number of GPUs");
DEFINE_int32(cuda_dbg_sync, 0, "");

DEFINE_double(thresh, 0.25, "Threshold for object's confidence");
DEFINE_double(nms_thresh, 0.45, "Threshold for non-maxima suppression");

DEFINE_string(mode, "video", "Either train/valid/image/video");
DEFINE_string(data_file, "yolo.data", "Data file path");
DEFINE_string(model_file, "yolo.cfg", "Model file path");
DEFINE_string(weights_file, "yolo.weights", "Weights file path");
DEFINE_string(input_file, "test.avi", "Input file path for image/video modes");

#ifdef GPU
#define CUDA_ASSERT(x) CudaAssert((x), __FILE__, __LINE__)

void CudaAssert(cudaError_t code, char const* file, int line, bool abort = true)
{
  if (code != cudaSuccess)
  {
    fprintf(stderr, "CUDA assert: %s %s %d\n", cudaGetErrorString(code), file,
        line);
    if (abort)
      exit(code);
  }
}

void ShowCudaCudnnInfo()
{
  int cuda_version = 0;
  int cuda_driver_version = 0;
  int device_count = 0;

  CUDA_ASSERT(cudaRuntimeGetVersion(&cuda_version));
  CUDA_ASSERT(cudaDriverGetVersion(&cuda_driver_version));

  fprintf(stderr, "CUDA-version: %d (%d)", cuda_version, cuda_driver_version);
  if (cuda_version > cuda_driver_version)
    fprintf(stderr, "\nWarning: CUDA-version is higher than driver-version!\n");

#ifdef CUDNN
  fprintf(
      stderr, ", cuDNN: %d.%d.%d", CUDNN_MAJOR, CUDNN_MINOR, CUDNN_PATCHLEVEL);
#endif  // CUDNN

#ifdef CUDNN_HALF
  fprintf(stderr, ", CUDNN_HALF=1");
#endif  // CUDNN_HALF

  CUDA_ASSERT(cudaGetDeviceCount(&device_count));
  fprintf(stderr, ", GPU count: %d\n", device_count);
}
#endif

cv::Mat ProcImage(Metadata const& md, Network* net, cv::Mat const& input)
{
  cv::Mat resize, display;
  Image image = {0, 0, 0, nullptr};

  cv::resize(input, resize, cv::Size(net->w, net->h));
  cv::resize(input, display, input.size() / 2);
  cv::cvtColor(resize, resize, cv::COLOR_RGB2BGR);
  Mat2Image(resize, &image);
  NetworkPredict(net, image.data);

  int num_boxes = 0;
  Detection* dets =
      GetNetworkBoxes(net, net->w, net->h, FLAGS_thresh, 1, &num_boxes);

  layer* l = &net->layers[net->n - 1];
  if (l->nms_kind == DEFAULT_NMS)
    NmsSort(dets, num_boxes, l->classes, FLAGS_nms_thresh);
  else
    DiouNmsSort(dets, num_boxes, l->classes, FLAGS_nms_thresh, l->nms_kind,
        l->beta_nms);

  DrawYoloDetections(display, dets, num_boxes, FLAGS_thresh, md);

  FreeDetections(dets, num_boxes);

  return display;
}

int main(int argc, char** argv)
{
#ifdef _DEBUG
  _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

  gflags::ParseCommandLineFlags(&argc, &argv, true);

#ifndef GPU
  printf("GPU isn't used\n");
  init_cpu();
#else   // GPU
  ShowCudaCudnnInfo();
#endif  // GPU

  Metadata md(FLAGS_data_file);

  if (FLAGS_mode == "train")
  {
    TrainDetector(md, FLAGS_model_file.c_str(), FLAGS_weights_file.c_str(),
        FLAGS_num_gpus, FLAGS_clear, FLAGS_show_imgs, FLAGS_calc_map,
        FLAGS_benchmark_layers);
  }
  else
  {
    Network* net = (Network*)calloc(1, sizeof(Network));
    LoadNetwork(net, FLAGS_model_file.c_str(), FLAGS_weights_file.c_str());

    // calculate mAP@0.5
    if (FLAGS_mode == "valid")
      ValidateDetector(md, net, 0.5);

    // processing a single image
    if (FLAGS_mode == "image")
    {
      cv::Mat input = cv::imread(FLAGS_input_file);

      using namespace std::chrono;
      auto start = system_clock::now();
      ///
      cv::Mat display = ProcImage(md, net, input);
      ///
      auto end = system_clock::now();

      DrawProcTime(display, duration_cast<milliseconds>(end - start).count());

      cv::imshow(FLAGS_mode, display);
      cv::waitKey(0);
    }

    // processing video stream
    if (FLAGS_mode == "video")
    {
      net->benchmark_layers = FLAGS_benchmark_layers;

      cv::VideoCapture video_capture(FLAGS_input_file);
      cv::VideoWriter writer;
      cv::Size display_size(1920 / 2, 1080 / 2);
      if (FLAGS_save_output)
      {
        int idx = FLAGS_input_file.find_last_of('.');
        std::string output_file = FLAGS_input_file.substr(0, idx) + "_out.avi";

        double fps = video_capture.get(cv::CAP_PROP_FPS);

        writer.open(output_file, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
            fps, display_size);
      }

      int64_t curr_frame = 0;
      int64_t max_frame = video_capture.get(cv::CAP_PROP_FRAME_COUNT);

      cv::Mat input;
      while (video_capture.isOpened() && video_capture.read(input))
      {
        if (curr_frame++ % 3 != 0)
          continue;

        using namespace std::chrono;
        auto start = system_clock::now();
        ///
        cv::Mat display = ProcImage(md, net, input);
        ///
        auto end = system_clock::now();

        if (FLAGS_save_output)
          writer << display;

        DrawProcTime(display, duration_cast<milliseconds>(end - start).count());
        DrawFrameInfo(display, curr_frame, max_frame);

        cv::imshow(FLAGS_mode, display);
        if (cv::waitKey(1) == 27)
          break;
      }
    }

    FreeNetwork(net);
    free(net);
  }

  return 0;
}
