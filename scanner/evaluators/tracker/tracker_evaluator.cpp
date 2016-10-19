/* Copyright 2016 Carnegie Mellon University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "scanner/evaluators/tracker/tracker_evaluator.h"
#include "scanner/evaluators/serialize.h"

#include "scanner/util/common.h"
#include "scanner/util/util.h"

#include "struck/Tracker.h"
#include "struck/Config.h"

#ifdef HAVE_CUDA
#include "scanner/util/cuda.h"
#endif

#include <cmath>

namespace scanner {

TrackerEvaluator::TrackerEvaluator(const EvaluatorConfig& config,
                                   DeviceType device_type,
                                   i32 device_id,
                                   i32 warmup_count)
    : config_(config),
      device_type_(device_type),
      device_id_(device_id),
      warmup_count_(warmup_count)
{
  if (device_type_ == DeviceType::GPU) {
    LOG(FATAL) << "GPU tracker support not implemented yet";
  }
}

void TrackerEvaluator::configure(const VideoMetadata& metadata) {
  LOG(INFO) << "Tracker configure";
  metadata_ = metadata;
}

void TrackerEvaluator::reset() {
  LOG(INFO) << "Tracker reset";
  tracks_.clear();
}

void TrackerEvaluator::evaluate(
    const std::vector<std::vector<u8 *>> &input_buffers,
    const std::vector<std::vector<size_t>> &input_sizes,
    std::vector<std::vector<u8 *>> &output_buffers,
    std::vector<std::vector<size_t>> &output_sizes) {
  assert(input_buffers.size() >= 2);

  i32 input_count = input_buffers[0].size();
  LOG(INFO) << "Tracker evaluate on " << input_count << " inputs";

  for (i32 b = 0; b < input_count; ++b) {
    u8 *bbox_buffer = input_buffers[1][b];
    size_t num_bboxes = *((size_t *)bbox_buffer);
    bbox_buffer += sizeof(size_t);
    i32 bbox_size = *((i32 *)bbox_buffer);
    bbox_buffer += sizeof(i32);

    // Find all the boxes which overlap the existing tracked boxes and update
    // the tracked boxes confidence values to those as well as the time since
    // last being seen

    // For boxes which don't overlap existing ones, create a new track for them
    std::vector<BoundingBox> detected_bboxes;
    std::vector<BoundingBox> new_detected_bboxes;
    for (size_t i = 0; i < num_bboxes; ++i) {
      BoundingBox box;
      box.ParseFromArray(bbox_buffer, bbox_size);
      bbox_buffer += bbox_size;

      i32 overlap_idx = -1;
      for (size_t j = 0; j < tracks_.size(); ++j) {
        auto& tracked_bbox = tracks_[j].box;
        if (iou(box, tracked_bbox) > IOU_THRESHOLD) {
          overlap_idx = j;
          break;
        }
      }
      if (overlap_idx != -1) {
        // Overlap with existing box
        tracks_[overlap_idx].box = box;
        tracks_[overlap_idx].frames_since_last_detection = 0;
      } else {
        // New box
        new_detected_bboxes.push_back(box);
      }
      detected_bboxes.push_back(box);
    }

    // Check if any tracks have been many frames without being redetected and
    // remove them
    for (i32 i = 0; i < (i32)tracks_.size(); ++i) {
      if (frames_since_last_detection_[i] > UNDETECTED_WINDOW) {
        tracked_bboxes_.erase(tracked_bboxes_.begin() + i);
        frames_since_last_detection_.erase(
            frames_since_last_detection_.begin() + i);
        trackers_.erase(trackers_.begin() + i);
        tracker_configs_.erase(tracker_configs_.begin() + i);
        tracker_ids_.erase(tracker_ids_.begin() + i);
        i--;
      }
    }

    // Perform tracking for all existing tracks that we have
    std::vector<BoundingBox> generated_bboxes;
    {
      u8 *buffer = input_buffers[0][b];
      cv::Mat frame(metadata_.height(), metadata_.width(), CV_8UC3, buffer);
      for (i32 i = 0; i < (i32)tracks_.size(); ++i) {
        auto& track = tracks_[i];
        auto& tracker = track.tracker;
        tracker->Track(frame);
        const struck::FloatRect &tracked_bbox = tracker->GetBB();
        f64 score = tracker->GetScore();

        if (score < TRACK_SCORE_THRESHOLD) {
          tracks_.erase(tracks_.begin() + i);
          i--;
        } else {
          BoundingBox box;
          box.set_x1(tracked_bbox.XMin());
          box.set_y1(tracked_bbox.YMin());
          box.set_x2(tracked_bbox.XMax());
          box.set_y2(tracked_bbox.YMax());
          box.set_score(tracked_bboxes_[i].score());
          box.set_track_id(tracker_ids_[i]);
          box.set_track_score(score);
          generated_bboxes.push_back(box);

          track.frames_since_last_detection_++;
        }
      }
    }

    // Add new detected bounding boxes to the fold
    for (BoundingBox& box : new_detected_bboxes) {
      tracks_.resize(tracks_.size() + 1);
      Track& track = tracks_.back();
      //i32 tracker_id = next_tracker_id_++;
      i32 tracker_id = unif(gen);
      track.id = tracker_id;
      track.config.reset(new struck::Config{});
      struck::Config &config = *track.config.get();;
      config.frameWidth = metadata_.width();
      config.frameHeight = metadata_.height();
      struck::Config::FeatureKernelPair fkp;
      fkp.feature = struck::Config::kFeatureTypeHaar;
      fkp.kernel = struck::Config::kKernelTypeLinear;
      config.features.push_back(fkp);
      track.tracker = new struck::Tracker(config);

      u8 *buffer = input_buffers[0][b];
      cv::Mat frame(metadata_.height(), metadata_.width(), CV_8UC3, buffer);
      struck::FloatRect r(box.x1(), box.y1(), box.x2() - box.x1(),
                          box.y2() - box.y1());
      track.tracker->Initialise(frame, r);

      box.set_track_id(track.id);
      track.frames_since_last_detection = 0;

      generated_bboxes.push_back(box);
    }

    {
      size_t size;
      u8 *buffer;

      serialize_bbox_vector(detected_bboxes, buffer, size);
      output_buffers[1].push_back(buffer);
      output_sizes[1].push_back(size);

      serialize_bbox_vector(generated_bboxes, buffer, size);
      output_buffers[2].push_back(buffer);
      output_sizes[2].push_back(size);
    }
  }

  u8 *buffer = nullptr;
  for (i32 b = 0; b < input_count; ++b) {
    size_t size = input_sizes[0][b];
    if (device_type_ == DeviceType::GPU) {
#ifdef HAVE_CUDA
      cudaMalloc((void **)&buffer, size);
      cudaMemcpy(buffer, input_buffers[0][b], size, cudaMemcpyDefault);
#else
      LOG(FATAL) << "Not built with CUDA support.";
#endif
    } else {
      buffer = new u8[size];
      memcpy(buffer, input_buffers[0][b], size);
    }
    output_buffers[0].push_back(buffer);
    output_sizes[0].push_back(size);
  }
}

float TrackerEvaluator::iou(const BoundingBox& bl, const BoundingBox& br) {
  float x1 = std::max(bl.x1(), br.x1());
  float y1 = std::max(bl.y1(), br.y1());
  float x2 = std::min(bl.x2(), br.x2());
  float y2 = std::min(bl.y2(), br.y2());

  float bl_width = bl.x2() - bl.x1();
  float bl_height = bl.y2() - bl.y1();
  float br_width = br.x2() - br.x1();
  float br_height= br.y2() - br.y1();
  if (x1 >= x2 || y1 >= y2) { return 0.0; }
  float intersection = (y2 - y1) * (x2 - x1);
  float _union = (bl_width * bl_height) + (br_width * br_height) - intersection;
  float iou = intersection / _union;
  return std::isnan(iou) ? 0.0 : iou;
}


TrackerEvaluatorFactory::TrackerEvaluatorFactory(DeviceType device_type,
                                                 i32 warmup_count)
    : device_type_(device_type), warmup_count_(warmup_count) {
  if (device_type_ == DeviceType::GPU) {
    LOG(FATAL) << "GPU tracker support not implemented yet";
  }
}

EvaluatorCapabilities TrackerEvaluatorFactory::get_capabilities() {
  EvaluatorCapabilities caps;
  caps.device_type = device_type_;
  caps.max_devices = 1;
  caps.warmup_size = warmup_count_;
  return caps;
}

std::vector<std::string> TrackerEvaluatorFactory::get_output_names() {
  return {"image", "before_bboxes", "after_bboxes"};
}

Evaluator *
TrackerEvaluatorFactory::new_evaluator(const EvaluatorConfig &config) {
  return new TrackerEvaluator(config, device_type_, 0, warmup_count_);
}
}
