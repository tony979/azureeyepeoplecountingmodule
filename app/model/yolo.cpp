// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Standard library includes
#include <string>
#include <thread>
#include <vector>

// Third party includes
#include <opencv2/gapi/mx.hpp>
#include <opencv2/gapi/core.hpp>
#include <opencv2/gapi/infer.hpp>
#include <opencv2/gapi/streaming/desync.hpp>
#include <opencv2/highgui.hpp>

// Local includes
#include "objectdetector.hpp"
#include "yolo.hpp"
#include "../kernels/yolo_kernels.hpp"
#include "../util/helper.hpp"
#include "../util/labels.hpp"

namespace model {

/** A YOLO network takes a single input and outputs a single output (which we will parse into boxes, labels, and confidences) */
G_API_NET(YOLONetwork, <cv::GMat(cv::GMat)>, "yolo-network");

YoloModel::YoloModel(const std::string &labelfpath, const std::vector<std::string> &modelfpaths, const std::string &mvcmd, const std::string &videofile, const cv::gapi::mx::Camera::Mode &resolution)
    : ObjectDetector{ labelfpath, modelfpaths, mvcmd, videofile, resolution }
{
}

void YoloModel::run(cv::GStreamingCompiled *pipeline)
{
    while (true)
    {
        // Wait for the VPU to come up.
        this->wait_for_device();

        // Read in the labels for classification
        label::load_label_file(this->class_labels, this->labelfpath);

        // Log some metadata.
        this->log_parameters();

        // Build the camera pipeline with G-API
        *pipeline = this->compile_cv_graph();
        util::log_info("starting the pipeline...");
        pipeline->start();

        // Pull data through the pipeline
        bool ran_out_naturally = this->pull_data(*pipeline);
        if (!ran_out_naturally)
        {
            break;
        }
    }
}

cv::GStreamingCompiled YoloModel::compile_cv_graph() const
{
    // The input node of the G-API pipeline. This will be filled in, one frame at time.
    cv::GMat in;

    // We have a custom preprocessing node for the Myriad X-attached camera.
    cv::GMat preproc = cv::gapi::mx::preproc(in, this->resolution);

    // This path is the H.264 path. It gets our frames one at a time from
    // the camera and encodes them into H.264.
    cv::GArray<uint8_t> h264;
    cv::GOpaque<int64_t> h264_seqno;
    cv::GOpaque<int64_t> h264_ts;
    std::tie(h264, h264_seqno, h264_ts) = cv::gapi::streaming::encH264ts(preproc);

    // We branch off from the preproc node into H.264 (above), raw BGR output (here),
    // and neural network inferences (below).
    cv::GMat img = cv::gapi::copy(cv::gapi::streaming::desync(preproc));
    cv::GOpaque<int64_t> img_ts = cv::gapi::streaming::timestamp(img);

    // This node branches off from the preproc node for neural network inferencing.
    cv::GMat bgr = cv::gapi::streaming::desync(preproc);
    cv::GOpaque<int64_t> nn_ts = cv::gapi::streaming::timestamp(preproc);

    // Here's where we actually run our neural network. It runs on the VPU.
    cv::GMat nn = cv::gapi::infer<YOLONetwork>(bgr);

    // Get some useful metadata.
    cv::GOpaque<int64_t> nn_seqno = cv::gapi::streaming::seqNo(nn);
    cv::GOpaque<cv::Size> sz = cv::gapi::streaming::size(bgr);

    // Here's where we post-process our network's outputs into bounding boxes, IDs, and confidences.
    cv::GArray<cv::Rect> rcs;
    cv::GArray<int> ids;
    cv::GArray<float> cfs;
    std::tie(rcs, ids, cfs) = cv::gapi::streaming::parseYoloWithConf(nn, sz);

    // Specify the boundaries of the G-API graph (the inputs and outputs).
    auto graph = cv::GComputation(cv::GIn(in),
                                  cv::GOut(h264, h264_seqno, h264_ts,               // H.264 branch
                                           img, img_ts,                             // Raw frame branch
                                           nn_seqno, nn_ts, rcs, ids, cfs, sz));    // Neural network inference branch

    // Pass the actual neural network blob file into the graph. We assume we have a modelfiles of at least length 1.
    CV_Assert(this->modelfiles.size() >= 1);
    auto networks = cv::gapi::networks(cv::gapi::mx::Params<YOLONetwork>{this->modelfiles.at(0)});

    // Here we wrap up all the kernels (the implementations of the G-API ops) that we need for our graph.
    auto kernels = cv::gapi::combine(cv::gapi::mx::kernels(), cv::gapi::kernels<cv::gapi::streaming::GOCVParseYoloWithConf>());

    // Compile the graph in streamnig mode; set all the parameters; feed the firmware file into the VPU.
    auto pipeline = graph.compileStreaming(cv::gapi::mx::Camera::params(), cv::compile_args(networks, kernels, cv::gapi::mx::mvcmdFile{ this->mvcmd }));

    // Specify the Percept DK's camera as the input to the pipeline.
    pipeline.setSource(cv::gapi::wip::make_src<cv::gapi::mx::Camera>());

    return pipeline;
}

void YoloModel::log_parameters() const
{
    // Log all the stuff
    std::string msg = "blobs: ";
    for (const auto &blob : this->modelfiles)
    {
        msg += blob + ", ";
    }
    msg += ", firmware: " + this->mvcmd + ", parser: YOLO, label: " + this->labelfpath + ", classes: " + std::to_string((int)this->class_labels.size());
    util::log_info(msg);
}

} // namespace model
