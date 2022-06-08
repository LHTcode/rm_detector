
//
// Created by yamabuki on 2022/4/18.
//

#include <rm_detector/detector.h>
#define CHECK(status) \
    do\
    {\
        auto ret = (status);\
        if (ret != 0)\
        {\
            std::cerr << "Cuda failure: " << ret << std::endl;\
            abort();\
        }\
    } while (0)

#define DEVICE 0
static const int INPUT_W = 416;
static const int INPUT_H = 416;
static const int NUM_CLASSES = 1;
const char* INPUT_BLOB_NAME = "images";
const char* OUTPUT_BLOB_NAME = "output";
static Logger gLogger;

namespace rm_detector
{
Detector::Detector()
{
}

void Detector::onInit()
{
  ros::NodeHandle nh = getMTPrivateNodeHandle();
  nh.getParam("g_model_path", model_path_);
  nh.getParam("nodelet_name", nodelet_name_);
  nh.getParam("camera_pub_name", camera_pub_name_);
  nh.getParam("roi_data1_name", roi_data1_name_);
  nh.getParam("roi_data2_name", roi_data2_name_);
  nh.getParam("roi_data3_name", roi_data3_name_);
  nh.getParam("roi_data4_name", roi_data4_name_);
  nh.getParam("roi_data5_name", roi_data5_name_);
//  setDataToMatrix(discoeffs_vec_, camera_matrix_vec_);
    initalizeInfer();
    callback_ = boost::bind(&Detector::dynamicCallback, this, _1);
    server_.setCallback(callback_);

    nh_ = ros::NodeHandle(nh, nodelet_name_);
  camera_sub_ = nh_.subscribe("/hk_camera/image_raw", 1, &Detector::receiveFromCam, this);
  camera_pub_ = nh_.advertise<sensor_msgs::Image>(camera_pub_name_, 1);
  camera_pub2_ = nh_.advertise<sensor_msgs::Image>("sub_publisher", 1);

  roi_data_pub1_ = nh_.advertise<std_msgs::Float32MultiArray>(roi_data1_name_, 1);
  roi_data_pub2_ = nh_.advertise<std_msgs::Float32MultiArray>(roi_data2_name_, 1);
  roi_data_pub3_ = nh_.advertise<std_msgs::Float32MultiArray>(roi_data3_name_, 1);
  roi_data_pub4_ = nh_.advertise<std_msgs::Float32MultiArray>(roi_data4_name_, 1);
  roi_data_pub5_ = nh_.advertise<std_msgs::Float32MultiArray>(roi_data5_name_, 1);

  roi_data_pub_vec.push_back(roi_data_pub1_);
  roi_data_pub_vec.push_back(roi_data_pub2_);
  roi_data_pub_vec.push_back(roi_data_pub3_);
  roi_data_pub_vec.push_back(roi_data_pub4_);
  roi_data_pub_vec.push_back(roi_data_pub5_);

  generateGridsAndStride(INPUT_W, INPUT_H);  // the wide height strides need to be changed depending on demand
}

void Detector::receiveFromCam(const sensor_msgs::ImageConstPtr& image)
{
  cv_image_ = boost::make_shared<cv_bridge::CvImage>(*cv_bridge::toCvShare(image, image->encoding));
  mainFuc(cv_image_);
  objects_.clear();
//  roi_picture_vec_.clear();
}

void Detector::dynamicCallback(rm_detector::dynamicConfig& config)
{
  nms_thresh_ = config.g_nms_thresh;
  bbox_conf_thresh_ = config.g_bbox_conf_thresh;
  turn_on_image_ = config.g_turn_on_image;
  target_is_red_ = config.target_is_red;
  target_is_blue_ = config.target_is_blue;
  ratio_of_pixels_ = config.ratio_of_pixels;
  pixels_thresh_ = config.pixels_thresh;
  binary_threshold_ = config.binary_threshold;
  aspect_ratio_=config.aspect_ratio;
  ROS_INFO("Settings have been seted");
}

void Detector::staticResize(cv::Mat& img)
{
  // r = std::min(r, 1.0f);
  int unpad_w = scale_ * img.cols;
  int unpad_h = scale_ * img.rows;
  int resize_unpad = std::max(unpad_h, unpad_w);
  //  auto origin_img_ptr = std::make_shared<cv::Mat>(img);
  origin_img_w_ = img.cols;
  origin_img_h_ = img.rows;
  cv::copyMakeBorder(img, img, abs(img.rows - img.cols) / 2, abs(img.rows - img.cols) / 2, 0, 0, cv::BORDER_CONSTANT,
                     cv::Scalar(122, 122, 122));
  cv::resize(img, img, cv::Size(resize_unpad, resize_unpad));
  //  cv::Mat out(INPUT_H, INPUT_W, CV_8UC3, cv::Scalar(114, 114, 114));
  //  img.copyTo(out(cv::Rect(0, 0, img.cols, img.rows)));
}

    float *Detector::blobFromImage(cv::Mat &img) {
        float *blob = new float[img.total() * 3];
        int channels = 3;
        int img_h = img.rows;
        int img_w = img.cols;
        for (size_t c = 0; c < channels; c++) {
            for (size_t h = 0; h < img_h; h++) {
                for (size_t w = 0; w < img_w; w++) {
                    blob[c * img_w * img_h + h * img_w + w] =
                            (float) img.at<cv::Vec3b>(h, w)[c];
                }
            }
        }
        return blob;
    }
// if the 3 parameters is fixed the strides can be fixed too
void Detector::generateGridsAndStride(const int target_w, const int target_h)
{
  std::vector<int> strides = { 8, 16, 32 };
  std::vector<GridAndStride> grid_strides;
  for (auto stride : strides)
  {
    int num_grid_w = target_w / stride;
    int num_grid_h = target_h / stride;
    for (int g1 = 0; g1 < num_grid_h; g1++)
    {
      for (int g0 = 0; g0 < num_grid_w; g0++)
      {
        grid_strides.push_back((GridAndStride){ g0, g1, stride });
      }
    }
  }

  grid_strides_ = grid_strides;
}

void Detector::generateYoloxProposals(std::vector<GridAndStride> grid_strides, const float* feat_ptr,
                                      float prob_threshold, std::vector<Object>& proposals)
{
  const int num_anchors = grid_strides.size();

  for (int anchor_idx = 0; anchor_idx < num_anchors; anchor_idx++)
  {
    const int grid0 = grid_strides[anchor_idx].grid0;
    const int grid1 = grid_strides[anchor_idx].grid1;
    const int stride = grid_strides[anchor_idx].stride;

    const int basic_pos = anchor_idx * (NUM_CLASSES + 5);

    // yolox/models/yolo_head.py decode logic
    //  outputs[..., :2] = (outputs[..., :2] + grids) * strides
    //  outputs[..., 2:4] = torch.exp(outputs[..., 2:4]) * strides
    float x_center = (feat_ptr[basic_pos + 0] + grid0) * stride;
    float y_center = (feat_ptr[basic_pos + 1] + grid1) * stride;
    float w = exp(feat_ptr[basic_pos + 2]) * stride;
    float h = exp(feat_ptr[basic_pos + 3]) * stride;
    // above all get from model
    float x0 = x_center - w * 0.5f;
    float y0 = y_center - h * 0.5f;

    float box_objectness = feat_ptr[basic_pos + 4];
    for (int class_idx = 0; class_idx < NUM_CLASSES; class_idx++)
    {
      float box_cls_score = feat_ptr[basic_pos + 5 + class_idx];
      float box_prob = box_objectness * box_cls_score;
      if (box_prob > prob_threshold)
      {
        Object obj;
        obj.rect.x = x0;
        obj.rect.y = y0;
        obj.rect.width = w;
        obj.rect.height = h;
        //                    obj.lu=cv::Point2f (x0-(w/2.0),y0+(h/2.0));
        //                    obj.ld=cv::Point2f (x0-(w/2.0),y0-(h/2.0));
        //                    obj.ru=cv::Point2f (x0+(w/2.0),y0+(h/2.0));
        //                    obj.rd=cv::Point2f (x0+(w/2.0),y0-(h/2.0));
        obj.label = class_idx;
        obj.prob = box_prob;

        proposals.push_back(obj);
      }
    }
  }
}

inline float Detector::intersectionArea(const Object& a, const Object& b)
{
  cv::Rect_<float> inter = a.rect & b.rect;
  return inter.area();
}

void Detector::qsortDescentInplace(std::vector<Object>& faceobjects, int left, int right)
{
  int i = left;                                    // num on object the init one
  int j = right;                                   // num on object the last one
  float p = faceobjects[(left + right) / 2].prob;  // middle obj 's prob

  while (i <= j)
  {
    while (faceobjects[i].prob > p)
      i++;

    while (faceobjects[j].prob < p)
      j--;

    if (i <= j)
    {
      // swap
      std::swap(faceobjects[i], faceobjects[j]);

      i++;
      j--;
    }
  }  // doing the sort work  the biggest pro obj will be seted on the init place

#pragma omp parallel sections
  {
#pragma omp section
    {
      if (left < j)
        qsortDescentInplace(faceobjects, left, j);
    }
#pragma omp section
    {
      if (i < right)
        qsortDescentInplace(faceobjects, i, right);
    }
  }
}

inline void Detector::qsortDescentInplace(std::vector<Object>& proposals)
{
  if (proposals.empty())
    return;

  qsortDescentInplace(proposals, 0, proposals.size() - 1);
}

void Detector::nmsSortedBboxes(const std::vector<Object>& faceobjects, std::vector<int>& picked, float nms_threshold)
{
  picked.clear();

  const int n = faceobjects.size();

  std::vector<float> areas(n);
  for (int i = 0; i < n; i++)
  {
    areas[i] = faceobjects[i].rect.area();
  }

  for (int i = 0; i < n; i++)
  {
    const Object& a = faceobjects[i];
    int keep = 1;
    for (int j = 0; j < (int)picked.size(); j++)  //
    {
      const Object& b = faceobjects[picked[j]];

      // intersection over union
      float inter_area = intersectionArea(a, b);
      float union_area = areas[i] + areas[picked[j]] - inter_area;
      // float IoU = inter_area / union_area
      if (inter_area / union_area >
          nms_threshold)  // if the obj's iou is larger than nms_threshold it will be filtrated
        keep = 0;
    }

    if (keep)
      picked.push_back(i);
  }
}

void Detector::selectTargetColor(std::vector<Object>& proposals,std::vector<cv::Mat> &color_filtrated_roi_vec)
{

    for (int i = 0; i < proposals.size(); i++)
  {
    cv::Rect rect(proposals[i].rect.tl().x, proposals[i].rect.tl().y, proposals[i].rect.width, proposals[i].rect.height);
    if (rect.tl().x < 0)
      rect.x = 0;
    if (rect.br().x > 640)
      rect.x = cv_image_->image.cols - rect.width-1;
    if (rect.tl().y < 0)
      rect.y = 0;
    if (rect.br().y > 640)
      rect.y = cv_image_->image.rows - rect.height-1;
    roi_picture_ = cv_image_->image(rect);
    roi_picture_vec_.push_back(roi_picture_);
  }

  // select the target's color
  if (target_is_red_)
  {
    for (int i = 0; i < roi_picture_vec_.size(); i++)
    {
        cv::split(roi_picture_vec_[i],roi_picture_split_vec_);
        cv::Mat r_decrease_b; //r-b
        cv::subtract(roi_picture_split_vec_[2],roi_picture_split_vec_[0],r_decrease_b);

        counter_of_pixel_=0;

      for (int j = 0; j < r_decrease_b.cols; j++)
      {
        for (int k = 0; k < r_decrease_b.rows; k++)
        {
          if (r_decrease_b.at<uchar>(j, k) >pixels_thresh_)
            counter_of_pixel_++;
        }
      }

      if (counter_of_pixel_ / (r_decrease_b.rows * r_decrease_b.cols) > ratio_of_pixels_) {
          filter_objects_.push_back(proposals[i]);
          color_filtrated_roi_vec.push_back(roi_picture_vec_[i]);
      }
      roi_picture_split_vec_.clear();

    }
    proposals.assign(filter_objects_.begin(), filter_objects_.end());
  }

  else if (target_is_blue_)
  {
    for (int i = 0; i < roi_picture_vec_.size(); i++)
    {
        cv::split(roi_picture_vec_[i],roi_picture_split_vec_);
        cv::Mat b_decrease_r;
        cv::subtract(roi_picture_split_vec_[0],roi_picture_split_vec_[2],b_decrease_r); //b-r

        counter_of_pixel_=0;
        for (int j = 0; j < b_decrease_r.cols; j++)
      {
        for (int k = 0; k < b_decrease_r.rows; k++)
        {
          if (b_decrease_r.at<uchar>(j, k) >pixels_thresh_)
            counter_of_pixel_++;
        }
      }
      if (counter_of_pixel_ / (b_decrease_r.rows * b_decrease_r.cols) > ratio_of_pixels_) {
          filter_objects_.push_back(proposals[i]);
          color_filtrated_roi_vec.push_back(roi_picture_vec_[i]);
      }
          roi_picture_split_vec_.clear();
    }
    proposals.assign(filter_objects_.begin(), filter_objects_.end());
  }
    if(!filter_objects_.empty())
    filter_objects_.clear();
}

void Detector::contoursProcess(std::vector<Object>& proposals,std::vector<cv::Mat> &color_filtrated_roi_vec)
{
    for(int i=0;i<color_filtrated_roi_vec.size();i++)
    {
        cv::Mat pyrup_img;
        cv::Mat gray_img;
        cv::Mat threshold_img;
        cv::Rect rect;
        std::vector<std::vector<cv::Point>> contours;
        std::vector<cv::Vec4i> hierarchy;
        cv::pyrUp(color_filtrated_roi_vec[i],pyrup_img); //x2
        cv::cvtColor(pyrup_img,gray_img,CV_BGR2GRAY);
        cv::adaptiveThreshold(gray_img,threshold_img,binary_threshold_,CV_ADAPTIVE_THRESH_GAUSSIAN_C,CV_THRESH_BINARY,3,0);
        cv::findContours(threshold_img,contours,hierarchy,CV_RETR_EXTERNAL,CV_CHAIN_APPROX_SIMPLE);
        rect=cv::boundingRect(contours[0]);
        if(rect.width/rect.height>aspect_ratio_)   filter_objects_.push_back(proposals[i]);
    }
    if(!filter_objects_.empty()) {
        proposals.assign(filter_objects_.begin(), filter_objects_.end());
        filter_objects_.clear();
    }
    else
    {
        proposals.clear();
    }

}

void Detector::decodeOutputs(const float* prob, const int img_w, const int img_h)
{
  std::vector<Object> proposals;
  generateYoloxProposals(grid_strides_, prob, bbox_conf_thresh_, proposals);  // initial filtrate
  std::vector<cv::Mat> color_filtrated_roi_vec;
//  selectTargetColor(proposals, color_filtrated_roi_vec);
//  contoursProcess(proposals,color_filtrated_roi_vec);

  if (proposals.empty()) {
      return;
  }

  qsortDescentInplace(proposals);
  std::vector<int> picked;
  nmsSortedBboxes(proposals, picked, nms_thresh_);


  int count = picked.size();
  if (count > 5)
    count = 5;
  objects_.resize(count);

  for (int i = 0; i < count; i++)
  {
    objects_[i] = proposals[picked[i]];

    // adjust offset to original unpadded
    //    float x0 = (objects_[i].rect.x) / scale;
    //    float y0 = (objects_[i].rect.y) / scale;
    //    float x1 = (objects_[i].rect.x + objects_[i].rect.width) / scale;
    //    float y1 = (objects_[i].rect.y + objects_[i].rect.height) / scale;

    float x0 = (objects_[i].rect.x);
    float y0 = (objects_[i].rect.y);
    float x1 = (objects_[i].rect.x + objects_[i].rect.width);
    float y1 = (objects_[i].rect.y + objects_[i].rect.height);

    // clip
    x0 = std::max(std::min(x0, (float)(img_w - 1)), 0.f);
    y0 = std::max(std::min(y0, (float)(img_h - 1)), 0.f);
    x1 = std::max(std::min(x1, (float)(img_w - 1)), 0.f);
    y1 = std::max(std::min(y1, (float)(img_h - 1)), 0.f);

    objects_[i].rect.x = x0;
    objects_[i].rect.y = y0;
    objects_[i].rect.width = x1 - x0;
    objects_[i].rect.height = y1 - y0;
  }  // make the real object
     //  for (size_t i = 0; i < objects_.size(); i++)


  for (size_t i = 0; i < 5; i++)
  {
    roi_point_vec_.clear();
    roi_data_.data.clear();

    roi_data_point_l_.x = (objects_[i].rect.tl().x) / scale_;
    roi_data_point_l_.y = ((objects_[i].rect.tl().y) / scale_) - (abs(origin_img_w_ - origin_img_h_) / 2);
    roi_data_point_r_.x = (objects_[i].rect.br().x) / scale_;
    roi_data_point_r_.y = ((objects_[i].rect.br().y) / scale_) - (abs(origin_img_w_ - origin_img_h_) / 2);

    roi_point_vec_.push_back(roi_data_point_l_);
    roi_point_vec_.push_back(roi_data_point_r_);


    roi_data_.data.push_back(roi_point_vec_[0].x);
    roi_data_.data.push_back(roi_point_vec_[0].y);
    roi_data_.data.push_back(roi_point_vec_[1].x);
    roi_data_.data.push_back(roi_point_vec_[1].y);
    roi_data_pub_vec[i].publish(roi_data_);
  }
}

void Detector::drawObjects(const cv::Mat& bgr)
{
  //        static const char* class_names[] = {
  //                "armor"
  //        };

  for (size_t i = 0; i < objects_.size(); i++)
  {
    //        fprintf(stderr, "%d = %.5f at %.2f %.2f %.2f x %.2f\n", obj.label, obj.prob,
    //                obj.rect.tl().x, obj.rect.tl().y, obj.rect.width, obj.rect.height);

    //        float c_mean = cv::mean(color)[0];
    //        cv::Scalar txt_color;
    //        if (c_mean > 0.5){
    //            txt_color = cv::Scalar(0, 0, 0);
    //        }else{
    //            txt_color = cv::Scalar(255, 255, 255);
    //        }

    cv::rectangle(bgr, objects_[i].rect, cv::Scalar(255, 0, 0), 2);

    //        char text[256];
    //        sprintf(text, "%s %.1f%%", class_names[obj.label], obj.prob * 100);
    //
    //        int baseLine = 0;
    //        cv::Size label_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.4, 1, &baseLine);
    //
    //        cv::Scalar txt_bk_color = color * 0.7 * 255;
    //
    //        int x = obj.rect.x;
    //        int y = obj.rect.y + 1;
    //        //int y = obj.rect.y - label_size.height - baseLine;
    //        if (y > image.rows)
    //            y = image.rows;
    //        //if (x + label_size.width > image.cols)
    //        //x = image.cols - label_size.width;
    //
    //        cv::rectangle(image, cv::Rect(cv::Point(x, y), cv::Size(label_size.width, label_size.height + baseLine)),
    //                      txt_bk_color, -1);
    //
    //        cv::putText(image, text, cv::Point(x, y + label_size.height),
    //                    cv::FONT_HERSHEY_SIMPLEX, 0.4, txt_color, 1);
  }
  camera_pub_.publish(cv_bridge::CvImage(std_msgs::Header(), "bgr8", bgr).toImageMsg());
}

    void doInference(nvinfer1::IExecutionContext &context, float *input, float *output, const int output_size, cv::Size input_shape) {
        const nvinfer1::ICudaEngine &engine = context.getEngine();

        // Pointers to input and output device buffers to pass to engine.
        // Engine requires exactly IEngine::getNbBindings() number of buffers.
        assert(engine.getNbBindings() == 2);
        void *buffers[2];

        // In order to bind the buffers, we need to know the names of the input and output tensors.
        // Note that indices are guaranteed to be less than IEngine::getNbBindings()
        const int inputIndex = engine.getBindingIndex(INPUT_BLOB_NAME);

        assert(engine.getBindingDataType(inputIndex) == nvinfer1::DataType::kFLOAT);
        const int outputIndex = engine.getBindingIndex(OUTPUT_BLOB_NAME);
        assert(engine.getBindingDataType(outputIndex) == nvinfer1::DataType::kFLOAT);
        int mBatchSize = engine.getMaxBatchSize();
//        std::cout << "max_batch:" << mBatchSize << std::endl;
        // Create GPU buffers on device
        CHECK(cudaMalloc(&buffers[inputIndex], 3 * input_shape.height * input_shape.width * sizeof(float)));
        CHECK(cudaMalloc(&buffers[outputIndex], output_size * sizeof(float)));

        // Create stream
        cudaStream_t stream;
        CHECK(cudaStreamCreate(&stream));

        // DMA input batch data to device, infer on the batch asynchronously, and DMA output back to host
        CHECK(cudaMemcpyAsync(buffers[inputIndex], input, 3 * input_shape.height * input_shape.width * sizeof(float),
                              cudaMemcpyHostToDevice, stream));
        context.enqueue(1, buffers, stream, nullptr);
        CHECK(cudaMemcpyAsync(output, buffers[outputIndex], output_size * sizeof(float), cudaMemcpyDeviceToHost,
                              stream));
        cudaStreamSynchronize(stream);

        // Release stream and buffers
        cudaStreamDestroy(stream);
        CHECK(cudaFree(buffers[inputIndex]));
        CHECK(cudaFree(buffers[outputIndex]));
    }


void Detector::mainFuc(cv_bridge::CvImagePtr& image_ptr)
{
  scale_ = std::min(INPUT_W / (image_ptr->image.cols * 1.0), INPUT_H / (image_ptr->image.rows * 1.0));
  staticResize(cv_image_->image);
  float *blob;
  blob=blobFromImage(cv_image_->image);
  doInference(*context_, blob, prob_, output_size_, cv_image_->image.size());
  delete blob;
  decodeOutputs(prob_, cv_image_->image.cols, cv_image_->image.rows);
  if (turn_on_image_)
    drawObjects(cv_image_->image);
  context_->destroy();
  engine_->destroy();
  runtime_->destroy();
}

void Detector::initalizeInfer()
{
    cudaSetDevice(DEVICE);
    // create a model using the API directly and serialize it to a stream
    char *trtModelStream{nullptr};
    size_t size{0};

//    if (argc == 4 && std::string(argv[2]) == "-i") {
//        const std::string engine_file_path {argv[1]};
    std::string engine_file_path = model_path_;
    std::ifstream file(engine_file_path, std::ios::binary);
    if (file.good()) {
        file.seekg(0, file.end);
        size = file.tellg();
        file.seekg(0, file.beg);
        trtModelStream = new char[size];
        assert(trtModelStream);
        file.read(trtModelStream, size);
        file.close();
    }

    runtime_= nvinfer1::createInferRuntime(gLogger);
    assert(runtime_ != nullptr);
    engine_ = runtime_->deserializeCudaEngine(trtModelStream, size);
    assert(engine_ != nullptr);
    context_= engine_->createExecutionContext();
    assert(context_ != nullptr);
    delete[] trtModelStream;
    auto out_dims = engine_->getBindingDimensions(1);
    output_size_ = 1;
    for (int j = 0; j < out_dims.nbDims; j++) {
        output_size_ *= out_dims.d[j];
    }

    prob_ = new float[output_size_];
}
Detector::~Detector()
{
}

}  // namespace rm_detector
PLUGINLIB_EXPORT_CLASS(rm_detector::Detector, nodelet::Nodelet)