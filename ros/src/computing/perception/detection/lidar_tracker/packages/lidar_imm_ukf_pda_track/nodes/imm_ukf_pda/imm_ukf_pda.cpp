#include <chrono>

#include <pcl_conversions/pcl_conversions.h>
#include "imm_ukf_pda.h"

enum TrackingState : int
{
  Die = 0,     // No longer tracking
  Init = 1,    // Start tracking
  Stable = 4,  // Stable tracking
  Lost = 10,   // About to lose target
};

ImmUkfPda::ImmUkfPda()
{
  ros::NodeHandle private_nh_("~");
  private_nh_.param<std::string>("pointcloud_frame", pointcloud_frame_, "velodyne");
  private_nh_.param<std::string>("tracking_frame", tracking_frame_, "world");
  private_nh_.param<int>("life_time_thres", life_time_thres_, 8);
  private_nh_.param<double>("gating_thres", gating_thres_, 9.22);
  private_nh_.param<double>("gate_probability", gate_probability_, 0.99);
  private_nh_.param<double>("detection_probability", detection_probability_, 0.9);
  private_nh_.param<double>("distance_thres", distance_thres_, 99);
  private_nh_.param<double>("static_velocity_thres", static_velocity_thres_, 0.5);

  init_ = false;
  use_vectormap_ = false;
  target_id_ = 0;
}


void ImmUkfPda::run()
{
  csv_file_.open ("/home/kosuke/example.csv", std::ios::app);
  // csv_file_.open ("/home/kosuke/example.csv");
  csv_file_ << "Num tracking targets"<<";"<<" Time(ms)"<<std::endl;

  pub_jskbbox_array_ = node_handle_.advertise<jsk_recognition_msgs::BoundingBoxArray>("/bounding_boxes_tracked", 1);
  pub_object_array_  = node_handle_.advertise<autoware_msgs::DetectedObjectArray>("/detected_objects", 1);
  pub_points_        = node_handle_.advertise<visualization_msgs::Marker>("/points/debug", 1);
  pub_texts_array_   = node_handle_.advertise<visualization_msgs::MarkerArray>("/texts/debug", 1);

  if(use_vectormap_)
  {
    vmap_.subscribe(node_handle_, vector_map::Category::POINT | vector_map::Category::NODE |
                                  vector_map::Category::LANE  | vector_map::Category::DTLANE, 10);
    setPredictionObject();

    pub_adas_direction_array_  = node_handle_.advertise<visualization_msgs::MarkerArray>("/adas_direction", 1);
    pub_adas_prediction_array_ = node_handle_.advertise<visualization_msgs::MarkerArray>("/adas_prediction", 1);
  }

  sub_detected_array_ = node_handle_.subscribe("/detected_objects_range", 1, &ImmUkfPda::callback, this);
}

double calcTime()
{
  struct::timespec getTime;
  clock_gettime(CLOCK_MONOTONIC, &getTime);
  return (getTime.tv_sec + getTime.tv_nsec*1e-9) *1000;
}

void ImmUkfPda::callback(const autoware_msgs::DetectedObjectArray& input)
{
  double start = calcTime();

  autoware_msgs::DetectedObjectArray transformed_input;
  jsk_recognition_msgs::BoundingBoxArray jskbboxes_output;
  autoware_msgs::DetectedObjectArray detected_objects_output;

  // only transform pose(clusteArray.clusters.bouding_box.pose)
  transformPoseToGlobal(input, transformed_input);
  tracker(transformed_input, jskbboxes_output, detected_objects_output);
  // relayJskbbox(input, jskbboxes_output);
  transformPoseToLocal(jskbboxes_output, detected_objects_output);

  pub_jskbbox_array_.publish(jskbboxes_output);
  pub_object_array_.publish(detected_objects_output);

  double end = calcTime();
  double elapsed = end - start;
   csv_file_ << targets_.size() << ";" << elapsed << std::endl;


  if(use_vectormap_)
  {
    visualization_msgs::MarkerArray directionMarkers;
    visualization_msgs::MarkerArray predictionMarkers;
    prediction_.adasMapAssitDirectionAndPrediction(input, tf_listener_, targets_, directionMarkers, predictionMarkers);
    pub_adas_direction_array_.publish(directionMarkers);
    pub_adas_prediction_array_.publish(predictionMarkers);
  }
}

void ImmUkfPda::setPredictionObject()
{
  lanes_ = vmap_.findByFilter([](const vector_map_msgs::Lane &lane){return true;});
  ModelBasePrediction mbp;
  // todo: change to init()
  mbp.setVMap(vmap_);
  mbp.setLanes(lanes_);
  prediction_ = mbp;
}

void ImmUkfPda::relayJskbbox(const autoware_msgs::DetectedObjectArray& input,
                                   jsk_recognition_msgs::BoundingBoxArray& jskbboxes_output)
{
  jskbboxes_output.header = input.header;
  for (size_t i = 0; i < input.objects.size(); i++)
  {
    jsk_recognition_msgs::BoundingBox bb;
    bb.header = input.header;
    bb.pose = input.objects[i].pose;
    bb.dimensions = input.objects[i].dimensions;
    jskbboxes_output.boxes.push_back(bb);
  }
}

void ImmUkfPda::transformPoseToGlobal(const autoware_msgs::DetectedObjectArray& input,
                                            autoware_msgs::DetectedObjectArray& transformed_input)
{
  transformed_input.header = input.header;
  try{
    tf_listener_.waitForTransform(pointcloud_frame_, tracking_frame_, ros::Time(0), ros::Duration(1.0));
    // todo: make transform obejct for later use
  }
  catch (tf::TransformException ex){
    std::cout << "cannot transform" << std::endl;
    ROS_ERROR("%s",ex.what());
    ros::Duration(1.0).sleep();
  }
  for (size_t i = 0; i < input.objects.size(); i++)
  {
    geometry_msgs::PoseStamped pose_in, pose_out;

    pose_in.header = input.header;
    pose_in.pose = input.objects[i].pose;

    tf_listener_.transformPose(tracking_frame_, ros::Time(0), pose_in, input.header.frame_id, pose_out);

    autoware_msgs::DetectedObject dd;
    dd.header = input.header;
    dd = input.objects[i];
    dd.pose = pose_out.pose;

    transformed_input.objects.push_back(dd);
  }
}

void ImmUkfPda::transformPoseToLocal(jsk_recognition_msgs::BoundingBoxArray& jskbboxes_output,
                                     autoware_msgs::DetectedObjectArray& detected_objects_output)
{
  for (size_t i = 0; i < jskbboxes_output.boxes.size(); i++)
  {
    geometry_msgs::PoseStamped pose_in, pose_out;

    pose_in.header = jskbboxes_output.header;
    pose_in.header.frame_id = tracking_frame_;
    pose_in.pose = jskbboxes_output.boxes[i].pose;

    tf_listener_.transformPose(pointcloud_frame_, ros::Time(0), pose_in, tracking_frame_, pose_out);
    pose_out.header.frame_id = jskbboxes_output.header.frame_id = pointcloud_frame_;
    jskbboxes_output.boxes[i].pose = pose_out.pose;
    detected_objects_output.objects[i].pose = pose_out.pose;
  }
}

void ImmUkfPda::findMaxZandS(const UKF& target, Eigen::VectorXd& max_det_z, Eigen::MatrixXd& max_det_s)
{
  double cv_det = target.s_cv_.determinant();
  double ctrv_det = target.s_ctrv_.determinant();
  double rm_det = target.s_rm_.determinant();

  if (cv_det > ctrv_det)
  {
    if (cv_det > rm_det)
    {
      max_det_z = target.z_pred_cv_;
      max_det_s = target.s_cv_;
    }
    else
    {
      max_det_z = target.z_pred_rm_;
      max_det_s = target.s_rm_;
    }
  }
  else
  {
    if (ctrv_det > rm_det)
    {
      max_det_z = target.z_pred_ctrv_;
      max_det_s = target.s_ctrv_;
    }
    else
    {
      max_det_z = target.z_pred_rm_;
      max_det_s = target.s_rm_;
    }
  }
}

void ImmUkfPda::measurementValidation(const autoware_msgs::DetectedObjectArray &input, UKF& target, const bool second_init,
                                      const Eigen::VectorXd &max_det_z, const Eigen::MatrixXd &max_det_s,
                                      std::vector<autoware_msgs::DetectedObject>& object_vec,
                                      std::vector<bool>& matching_vec)
{
  int count = 0;
  bool second_init_done = false;
  double smallest_nis = std::numeric_limits<double>::max();
  autoware_msgs::DetectedObject smallest_meas_object;
  for (size_t i = 0; i < input.objects.size(); i++)
  {
    double x = input.objects[i].pose.position.x;
    double y = input.objects[i].pose.position.y;

    Eigen::VectorXd meas = Eigen::VectorXd(2);
    meas << x, y;

    Eigen::VectorXd diff = meas - max_det_z;
    double nis = diff.transpose() * max_det_s.inverse() * diff;

    if (nis < gating_thres_)
    {  // x^2 99% range
      count++;
      if (matching_vec[i] == false)
        target.lifetime_++;
      // pick one meas with smallest nis
      if (second_init)
      {
        if (nis < smallest_nis)
        {
          smallest_nis = nis;
          smallest_meas_object = input.objects[i];
          matching_vec[i] = true;
          second_init_done = true;
        }
      }
      else
      {
        object_vec.push_back(input.objects[i]);
        matching_vec[i] = true;
      }
    }
  }
  if (second_init_done)
  {
    object_vec.push_back(smallest_meas_object);
  }
}

void ImmUkfPda::filterPDA(UKF& target,
                          const std::vector<autoware_msgs::DetectedObject>& object_vec,
                          std::vector<double>& lambda_vec)
{
  // calculating association probability
  double num_meas = object_vec.size();
  double b = 2 * num_meas * (1 - detection_probability_ * gate_probability_) / (gating_thres_ * detection_probability_);
  double e_cv_sum = 0;
  double e_ctrv_sum = 0;
  double e_rm_sum = 0;

  std::vector<double> e_cv_vec;
  std::vector<double> e_ctrv_vec;
  std::vector<double> e_rm_vec;

  std::vector<Eigen::VectorXd> diff_cv_vec;
  std::vector<Eigen::VectorXd> diff_ctrv_vec;
  std::vector<Eigen::VectorXd> diff_rm_vec;

  for (size_t i = 0; i < num_meas; i++)
  {
    Eigen::VectorXd meas_vec = Eigen::VectorXd(2);
    meas_vec(0) = object_vec[i].pose.position.x;
    meas_vec(1) = object_vec[i].pose.position.y;

    Eigen::VectorXd diff_cv   = meas_vec - target.z_pred_cv_;
    Eigen::VectorXd diff_ctrv = meas_vec - target.z_pred_ctrv_;
    Eigen::VectorXd diff_rm   = meas_vec - target.z_pred_rm_;

    diff_cv_vec.push_back(diff_cv);
    diff_ctrv_vec.push_back(diff_ctrv);
    diff_rm_vec.push_back(diff_rm);

    double e_cv = exp(-0.5 * diff_cv.transpose() * target.s_cv_.inverse() * diff_cv);
    double e_ctrv = exp(-0.5 * diff_ctrv.transpose() * target.s_ctrv_.inverse() * diff_ctrv);
    double e_rm = exp(-0.5 * diff_rm.transpose() * target.s_rm_.inverse() * diff_rm);

    e_cv_vec.push_back(e_cv);
    e_ctrv_vec.push_back(e_ctrv);
    e_rm_vec.push_back(e_rm);

    e_cv_sum += e_cv;
    e_ctrv_sum += e_ctrv;
    e_rm_sum += e_rm;
  }
  double beta_cv_zero = b / (b + e_cv_sum);
  double beta_ctrv_zero = b / (b + e_ctrv_sum);
  double beta_rm_zero = b / (b + e_rm_sum);

  std::vector<double> beta_cv;
  std::vector<double> beta_ctrv;
  std::vector<double> beta_rm;

  for (size_t i = 0; i < num_meas; i++)
  {
    double temp_cv = e_cv_vec[i] / (b + e_cv_sum);
    double temp_ctrv = e_ctrv_vec[i] / (b + e_ctrv_sum);
    double temp_rm = e_rm_vec[i] / (b + e_rm_sum);

    beta_cv.push_back(temp_cv);
    beta_ctrv.push_back(temp_ctrv);
    beta_rm.push_back(temp_rm);
  }
  Eigen::VectorXd sigma_x_cv;
  Eigen::VectorXd sigma_x_ctrv;
  Eigen::VectorXd sigma_x_rm;
  sigma_x_cv.setZero(2);
  sigma_x_ctrv.setZero(2);
  sigma_x_rm.setZero(2);

  for (size_t i = 0; i < num_meas; i++)
  {
    sigma_x_cv += beta_cv[i] * diff_cv_vec[i];
    sigma_x_ctrv += beta_ctrv[i] * diff_ctrv_vec[i];
    sigma_x_rm += beta_rm[i] * diff_rm_vec[i];
  }

  Eigen::MatrixXd sigma_p_cv;
  Eigen::MatrixXd sigma_p_ctrv;
  Eigen::MatrixXd sigma_p_rm;
  sigma_p_cv.setZero(2, 2);
  sigma_p_ctrv.setZero(2, 2);
  sigma_p_rm.setZero(2, 2);
  for (size_t i = 0; i < num_meas; i++)
  {
    sigma_p_cv += (beta_cv[i] * diff_cv_vec[i] * diff_cv_vec[i].transpose() - sigma_x_cv * sigma_x_cv.transpose());
    sigma_p_ctrv +=
        (beta_ctrv[i] * diff_ctrv_vec[i] * diff_ctrv_vec[i].transpose() - sigma_x_ctrv * sigma_x_ctrv.transpose());
    sigma_p_rm += (beta_rm[i] * diff_rm_vec[i] * diff_rm_vec[i].transpose() - sigma_x_rm * sigma_x_rm.transpose());
  }

  // update x and P
  target.x_cv_ = target.x_cv_ + target.k_cv_ * sigma_x_cv;
  target.x_ctrv_ = target.x_ctrv_ + target.k_ctrv_ * sigma_x_ctrv;
  target.x_rm_ = target.x_rm_ + target.k_rm_ * sigma_x_rm;

  while (target.x_cv_(3) > M_PI)
    target.x_cv_(3) -= 2. * M_PI;
  while (target.x_cv_(3) < -M_PI)
    target.x_cv_(3) += 2. * M_PI;
  while (target.x_ctrv_(3) > M_PI)
    target.x_ctrv_(3) -= 2. * M_PI;
  while (target.x_ctrv_(3) < -M_PI)
    target.x_ctrv_(3) += 2. * M_PI;
  while (target.x_rm_(3) > M_PI)
    target.x_rm_(3) -= 2. * M_PI;
  while (target.x_rm_(3) < -M_PI)
    target.x_rm_(3) += 2. * M_PI;

  if (num_meas != 0)
  {
    target.p_cv_ = beta_cv_zero * target.p_cv_ +
                   (1 - beta_cv_zero) * (target.p_cv_ - target.k_cv_ * target.s_cv_ * target.k_cv_.transpose()) +
                   target.k_cv_ * sigma_p_cv * target.k_cv_.transpose();
    target.p_ctrv_ =
        beta_ctrv_zero * target.p_ctrv_ +
        (1 - beta_ctrv_zero) * (target.p_ctrv_ - target.k_ctrv_ * target.s_ctrv_ * target.k_ctrv_.transpose()) +
        target.k_ctrv_ * sigma_p_ctrv * target.k_ctrv_.transpose();
    target.p_rm_ = beta_rm_zero * target.p_rm_ +
                   (1 - beta_rm_zero) * (target.p_rm_ - target.k_rm_ * target.s_rm_ * target.k_rm_.transpose()) +
                   target.k_rm_ * sigma_p_rm * target.k_rm_.transpose();
  }
  else
  {
    target.p_cv_ = target.p_cv_ - target.k_cv_ * target.s_cv_ * target.k_cv_.transpose();
    target.p_ctrv_ = target.p_ctrv_ - target.k_ctrv_ * target.s_ctrv_ * target.k_ctrv_.transpose();
    target.p_rm_ = target.p_rm_ - target.k_rm_ * target.s_rm_ * target.k_rm_.transpose();
  }

  Eigen::VectorXd max_det_z;
  Eigen::MatrixXd max_det_s;

  findMaxZandS(target, max_det_z, max_det_s);
  double Vk = M_PI * sqrt(gating_thres_ * max_det_s.determinant());

  double lambda_cv, lambda_ctrv, lambda_rm;
  if (num_meas != 0)
  {
    lambda_cv = (1 - gate_probability_ * detection_probability_) / pow(Vk, num_meas) +
                detection_probability_ * pow(Vk, 1 - num_meas) * e_cv_sum /
                    (num_meas * sqrt(2 * M_PI * target.s_cv_.determinant()));
    lambda_ctrv = (1 - gate_probability_ * detection_probability_) / pow(Vk, num_meas) +
                  detection_probability_ * pow(Vk, 1 - num_meas) * e_ctrv_sum /
                      (num_meas * sqrt(2 * M_PI * target.s_ctrv_.determinant()));
    lambda_rm = (1 - gate_probability_ * detection_probability_) / pow(Vk, num_meas) +
                detection_probability_ * pow(Vk, 1 - num_meas) * e_rm_sum /
                    (num_meas * sqrt(2 * M_PI * target.s_rm_.determinant()));
  }
  else
  {
    lambda_cv = (1 - gate_probability_ * detection_probability_) / pow(Vk, num_meas);
    lambda_ctrv = (1 - gate_probability_ * detection_probability_) / pow(Vk, num_meas);
    lambda_rm = (1 - gate_probability_ * detection_probability_) / pow(Vk, num_meas);
  }
  lambda_vec.push_back(lambda_cv);
  lambda_vec.push_back(lambda_ctrv);
  lambda_vec.push_back(lambda_rm);
}

void ImmUkfPda::getNearestEuclidCluster(const UKF& target, const std::vector<autoware_msgs::DetectedObject>& object_vec,
                                        autoware_msgs::DetectedObject& object, double& min_dist)
{
  int min_ind = 0;
  double px = target.x_merge_(0);
  double py = target.x_merge_(1);

  for (size_t i = 0; i < object_vec.size(); i++)
  {
    double meas_x = object_vec[i].pose.position.x;
    double meas_y = object_vec[i].pose.position.y;

    double dist = sqrt((px - meas_x) * (px - meas_x) + (py - meas_y) * (py - meas_y));
    if (dist < min_dist)
    {
      min_dist = dist;
      min_ind = i;
    }
  }

  object = object_vec[min_ind];
}

void ImmUkfPda::associateBB(const std::vector<autoware_msgs::DetectedObject>& object_vec, UKF& target)
{
  // skip if no validated measurement
  if (object_vec.size() == 0)
  {
    return;
  }
  if (target.tracking_num_ == TrackingState::Stable && target.lifetime_ >= life_time_thres_)
  {
    autoware_msgs::DetectedObject nearest_object;
    double min_dist = std::numeric_limits<double>::max();
    getNearestEuclidCluster(target, object_vec, nearest_object, min_dist);
    if (min_dist < distance_thres_)
    {
      target.is_vis_bb_ = true;
      target.jsk_bb_.pose       = nearest_object.pose;
      target.jsk_bb_.dimensions = nearest_object.dimensions;
    }
  }
}

double ImmUkfPda::getJskBBoxYaw(const jsk_recognition_msgs::BoundingBox& jsk_bb)
{
  tf::Quaternion q(jsk_bb.pose.orientation.x, jsk_bb.pose.orientation.y, jsk_bb.pose.orientation.z,
                   jsk_bb.pose.orientation.w);
  double roll, pitch, yaw;
  tf::Matrix3x3(q).getRPY(roll, pitch, yaw);
  return yaw;
}

double ImmUkfPda::getJskBBoxArea(const jsk_recognition_msgs::BoundingBox& jsk_bb)
{
  double area = jsk_bb.dimensions.x * jsk_bb.dimensions.y;
  return area;
}

void ImmUkfPda::updateBB(UKF& target)
{
  // skip to prevent memory leak by accessing empty target.bbox_
  if (!target.is_vis_bb_)
  {
    return;
  }
  double yaw = getJskBBoxYaw(target.jsk_bb_);

  // skip the rest of process if it is first bbox associaiton
  if (target.is_best_jsk_bb_empty_ == false)
  {
    target.best_jsk_bb_ = target.jsk_bb_;
    target.best_yaw_ = yaw;
    target.is_best_jsk_bb_empty_ = true;
    return;
  }

  // restricting yaw movement
  double diff_yaw = yaw - target.best_yaw_;

  // diffYaw is within the threshold, apply the diffYaw chamge
  if(abs(diff_yaw) < bb_yaw_change_thres_)
  {
      target.best_jsk_bb_.pose.orientation = target.jsk_bb_.pose.orientation;
      target.best_yaw_ = yaw;
  }
  else
  {
      target.jsk_bb_.pose.orientation = target.best_jsk_bb_.pose.orientation;
  }

  // // bbox area
  double area = getJskBBoxArea(target.jsk_bb_);
  double best_area = getJskBBoxArea(target.best_jsk_bb_);


  // start updating bbox params
  double delta_area = area - best_area;

  // when the delta area is under 0, keep best area and relocate(slide) it for current cp
  if (delta_area < 0)
  {
    // updateVisBoxArea(target, dtCP);
    target.jsk_bb_.dimensions = target.best_jsk_bb_.dimensions;
    // for  mergeSegmentation, area comparison
    target.bb_area_ = best_area;
  }
  else if (delta_area > 0)
  {
    // target.bestBBox_ = target.BBox_;
    target.best_jsk_bb_.dimensions = target.jsk_bb_.dimensions;
    // for mergeSegmentation, area comparison
    target.bb_area_ = area;
  }
}

void ImmUkfPda::updateLabel(const UKF& target, autoware_msgs::DetectedObject& dd)
{
  int tracking_num = target.tracking_num_;
  // cout << "trackingnum "<< trackingNum << endl;
  if (target.is_static_)
  {
    dd.label = "Static";
  }
  else if (tracking_num > TrackingState::Die && tracking_num < TrackingState::Stable)
  {
    dd.label = "Initialized";
  }
  else if (tracking_num == TrackingState::Stable)
  {
    dd.label = "Stable";
  }
  else if (tracking_num > TrackingState::Stable && tracking_num <= TrackingState::Lost)
  {
    dd.label = "Lost";
  }
  else
  {
    dd.label = "None";
  }
}

void ImmUkfPda::updateJskLabel(const UKF& target, jsk_recognition_msgs::BoundingBox& bb)
{
  int tracking_num = target.tracking_num_;
  if (target.is_static_)
  {
    bb.label = 15;  // white color
  }
  else if (tracking_num == TrackingState::Stable)
  {
    bb.label = 2;  // orange color
  }
}

void ImmUkfPda::initTracker(const autoware_msgs::DetectedObjectArray& input, double timestamp)
{
  for (size_t i = 0; i < input.objects.size(); i++)
  {
    double px = input.objects[i].pose.position.x;
    double py = input.objects[i].pose.position.y;
    Eigen::VectorXd init_meas = Eigen::VectorXd(2);
    init_meas << px, py;

    UKF ukf;
    ukf.initialize(init_meas, timestamp, target_id_);
    targets_.push_back(ukf);
    target_id_ ++;
  }
  timestamp_ = timestamp;
  init_ = true;
  return;
}

void ImmUkfPda::secondInit(UKF& target, const std::vector<autoware_msgs::DetectedObject>& object_vec, double dt)
{
  if (object_vec.size() == 0)
  {
    target.tracking_num_ = TrackingState::Die;
    return;
  }
  // record init measurement for env classification
  target.init_meas_ << target.x_merge_(0), target.x_merge_(1);

  // state update
  double target_x = object_vec[0].pose.position.x;
  double target_y = object_vec[0].pose.position.y;
  double target_diff_x = target_x - target.x_merge_(0);
  double target_diff_y = target_y - target.x_merge_(1);
  double target_yaw = atan2(target_diff_y, target_diff_x);
  double dist = sqrt(target_diff_x * target_diff_x + target_diff_y * target_diff_y);
  double target_v = dist / dt;

  while (target_yaw > M_PI)
    target_yaw -= 2. * M_PI;
  while (target_yaw < -M_PI)
    target_yaw += 2. * M_PI;

  target.x_merge_(0) = target.x_cv_(0) = target.x_ctrv_(0) = target.x_rm_(0) = target_x;
  target.x_merge_(1) = target.x_cv_(1) = target.x_ctrv_(1) = target.x_rm_(1) = target_y;
  target.x_merge_(2) = target.x_cv_(2) = target.x_ctrv_(2) = target.x_rm_(2) = target_v;
  target.x_merge_(3) = target.x_cv_(3) = target.x_ctrv_(3) = target.x_rm_(3) = target_yaw;

  target.tracking_num_++;
  return;
}

void ImmUkfPda::updateTrackingNum(const std::vector<autoware_msgs::DetectedObject>& object_vec, UKF& target)
{
  if (object_vec.size() > 0)
  {
    if (target.tracking_num_ < TrackingState::Stable)
    {
      target.tracking_num_++;
    }
    else if (target.tracking_num_ == TrackingState::Stable)
    {
      target.tracking_num_ = TrackingState::Stable;
    }
    else if (target.tracking_num_ >= TrackingState::Stable && target.tracking_num_ < TrackingState::Lost)
    {
      target.tracking_num_ = TrackingState::Stable;
    }
    else if (target.tracking_num_ == TrackingState::Lost)
    {
      target.tracking_num_ = TrackingState::Die;
    }
  }
  else
  {
    if (target.tracking_num_ < TrackingState::Stable)
    {
      target.tracking_num_ = TrackingState::Die;
    }
    else if (target.tracking_num_ >= TrackingState::Stable && target.tracking_num_ < TrackingState::Lost)
    {
      target.tracking_num_++;
    }
    else if (target.tracking_num_ == TrackingState::Lost)
    {
      target.tracking_num_ = TrackingState::Die;
    }
  }

  return;
}

void ImmUkfPda::probabilisticDataAssociation(const autoware_msgs::DetectedObjectArray& input, const double dt,
                                             const double det_explode_param, std::vector<bool>& matching_vec,
                                             std::vector<double>& lambda_vec, UKF& target, bool& is_skip_target)
{
  Eigen::VectorXd max_det_z;
  Eigen::MatrixXd max_det_s;
  std::vector<autoware_msgs::DetectedObject> object_vec;
  is_skip_target = false;
  // find maxDetS associated with predZ
  findMaxZandS(target, max_det_z, max_det_s);

  double det_s = max_det_s.determinant();

  // prevent ukf not to explode
  if (std::isnan(det_s) || det_s > det_explode_param)
  {
    target.tracking_num_ = TrackingState::Die;
    is_skip_target = true;
    return;
  }

  bool is_second_init;
  if (target.tracking_num_ == TrackingState::Init)
  {
    is_second_init = true;
  }
  else
  {
    is_second_init = false;
  }

  // measurement gating, get measVec, bboxVec, matchingVec through reference
  measurementValidation(input, target, is_second_init, max_det_z, max_det_s, object_vec, matching_vec);

  // bounding box association if target is stable :plus, right angle correction if its needed
  // input: track number, bbox measurements, &target
  associateBB(object_vec, target);

  // second detection for a target: update v and yaw
  if (is_second_init)
  {
    secondInit(target, object_vec, dt);
    is_skip_target = true;
    return;
  }

  // update tracking number
  updateTrackingNum(object_vec, target);

  if (target.tracking_num_ == TrackingState::Die)
  {
    is_skip_target = true;
    return;
  }
  filterPDA(target, object_vec, lambda_vec);
}

void ImmUkfPda::makeNewTargets(const double timestamp, const autoware_msgs::DetectedObjectArray& input, const std::vector<bool>& matching_vec)
{
  for (size_t i = 0; i < input.objects.size(); i++)
  {
    if (matching_vec[i] == false)
    {
      double px = input.objects[i].pose.position.x;
      double py = input.objects[i].pose.position.y;
      Eigen::VectorXd init_meas = Eigen::VectorXd(2);
      init_meas << px, py;

      UKF ukf;
      ukf.initialize(init_meas, timestamp, target_id_);
      targets_.push_back(ukf);
      target_id_ ++;
    }
  }
}

void ImmUkfPda::staticClassification()
{
  for (size_t i = 0; i < targets_.size(); i++)
  {
    targets_[i].vel_history_.push_back(targets_[i].x_merge_(2));
    if (targets_[i].tracking_num_ == TrackingState::Stable &&
        targets_[i].lifetime_ > life_time_thres_)
    {
      double sum_vel = 0;
      double avg_vel = 0;
      for (int ind = 1; ind < life_time_thres_; ind++)
      {
        sum_vel += targets_[i].vel_history_.end()[-ind];
      }
      avg_vel = double(sum_vel/life_time_thres_);

      if ((avg_vel< static_velocity_thres_) &&
         (targets_[i].mode_prob_rm_ > targets_[i].mode_prob_cv_ ||
          targets_[i].mode_prob_rm_ > targets_[i].mode_prob_ctrv_))
      {
        targets_[i].is_static_ = true;
      }
    }
  }
}

void ImmUkfPda::makeOutput(const autoware_msgs::DetectedObjectArray& input,
                           jsk_recognition_msgs::BoundingBoxArray& jskbboxes_output,
                           autoware_msgs::DetectedObjectArray& detected_objects_output)
{
  tf::StampedTransform transform;
  tf_listener_.lookupTransform(tracking_frame_, pointcloud_frame_, ros::Time(0), transform);

  // get yaw angle from tracking_frame_ to pointcloud_frame_ for direction(arrow) visualization
  tf::Matrix3x3 m(transform.getRotation());
  double roll, pitch, yaw;
  m.getRPY(roll, pitch, yaw);

  // output.header = input.header;
  jskbboxes_output.header = input.header;
  detected_objects_output.header = input.header;
  for (size_t i = 0; i < targets_.size(); i++)
  {
    // if (targets_[i].is_vis_bb_ && isVisible(targets_[i]))
    if (targets_[i].is_vis_bb_ )
    {
      double tx = targets_[i].x_merge_(0);
      double ty = targets_[i].x_merge_(1);
      double mx = targets_[i].init_meas_(0);
      double my = targets_[i].init_meas_(1);

      // for static classification
      targets_[i].dist_from_init_ = sqrt((tx - mx) * (tx - mx) + (ty - my) * (ty - my));

      double tv = targets_[i].x_merge_(2);
      double tyaw = targets_[i].x_merge_(3) - yaw;

      // std::cout << "ukf_id "<< targets_[i].ukf_id_ << std::endl;
      // std::cout << "world yaw "<< targets_[i].x_merge_(3) << std::endl;

      while (tyaw > M_PI)
        tyaw -= 2. * M_PI;
      while (tyaw < -M_PI)
        tyaw += 2. * M_PI;

      jsk_recognition_msgs::BoundingBox bb;
      bb.header = input.header;
      bb = targets_[i].jsk_bb_;
      updateJskLabel(targets_[i], bb);
      jskbboxes_output.boxes.push_back(bb);

      autoware_msgs::DetectedObject dd;
      dd.header = input.header;
      dd.id = targets_[i].ukf_id_;
      dd.velocity.linear.x = tv;
      dd.pose = targets_[i].jsk_bb_.pose;
      dd.dimensions = targets_[i].jsk_bb_.dimensions;
      // Store tyaw in velocity.linear.y since nowhere to store estimated_yaw
      dd.velocity.linear.y = tyaw;
      updateLabel(targets_[i], dd);
      detected_objects_output.objects.push_back(dd);
    }
  }
}

void ImmUkfPda::removeUnnecessaryTarget()
{
  std::vector<UKF> temp_targets;
  for(size_t i = 0; i < targets_.size(); i++)
  {
    if(targets_[i].tracking_num_ != TrackingState::Die)
    {
      temp_targets.push_back(targets_[i]);
    }
  }
  std::vector<UKF>().swap(targets_);
  targets_ = temp_targets;
}

void ImmUkfPda::pubPoints(const autoware_msgs::DetectedObjectArray& input)
{
  visualization_msgs::MarkerArray texts_markers;
  visualization_msgs::Marker target_points, meas_points;
  target_points.header.frame_id = meas_points.header.frame_id =  "/world";
  target_points.header.stamp = meas_points.header.stamp = input.header.stamp;
  target_points.ns = meas_points.ns = "target_points";
  target_points.action = meas_points.action = visualization_msgs::Marker::ADD;
  target_points.pose.orientation.w =  meas_points.pose.orientation.w =1.0;

  target_points.id = 0;
  meas_points.id = 1;

  target_points.type = meas_points.type = visualization_msgs::Marker::POINTS;

  // POINTS markers use x and y scale for width/height respectively
  target_points.scale.x = 0.4;
  target_points.scale.y = 0.4;
  meas_points.scale.x = 0.3;
  meas_points.scale.y = 0.3;

  // Points are green
  target_points.color.r = 1.0f;
  target_points.color.a =1.0;
  meas_points.color.g = 1.0f;
  meas_points.color.a = 1.0;

  for(size_t i = 0; i < targets_.size(); i++)
  {
    // std::cout << "target id "<< targets_[i].ukf_id_ << std::endl;
    // std::cout << "tracking num " << targets_[i].tracking_num_ << std::endl;
    // std::cout << "pos " << targets_[i].x_merge_(0) << " " << targets_[i].x_merge_(1) << std::endl;

    geometry_msgs::Point p;
    p.x = targets_[i].x_merge_(0);
    p.y = targets_[i].x_merge_(1);
    p.z = 1.0;

    target_points.points.push_back(p);

    visualization_msgs::Marker id;
    id.header.frame_id =  "/world";
    id.header.stamp = input.header.stamp;
    id.ns ="target_points";
    id.action = visualization_msgs::Marker::ADD;
    id.type = visualization_msgs::Marker::TEXT_VIEW_FACING;
    id.id = i;
    id.lifetime = ros::Duration(0.1);

    id.color.g = 1.0f;
    id.color.a = 1.0;

    // Set the pose of the marker.  This is a full 6DOF pose relative to the frame/time specified in the header
    id.pose.position.x = targets_[i].x_merge_(0);
    id.pose.position.y = targets_[i].x_merge_(1);
    id.pose.position.z = 1.5;

    id.scale.z = 1.0;

    double tv = targets_[i].x_merge_(2);
    // not to visualize '-0.0'
    if(abs(tv) < 0.1)
    {
      tv = 0.0;
    }
    std::string s_velocity = std::to_string(tv*3.6);
    std::string modified_sv = s_velocity.substr(0, s_velocity.find(".")+3);
    std::string text = "<" + std::to_string(targets_[i].ukf_id_) + ">"
                      +" " + std::to_string(targets_[i].tracking_num_) + " "
                      + modified_sv + " km/h";
    // id.text = std::to_string(input.objects[i].id);
    id.text = text;
    texts_markers.markers.push_back(id);
  }


  for (size_t i = 0; i < input.objects.size(); i++)
  {
    geometry_msgs::Point p;
    p.x = input.objects[i].pose.position.x;
    p.y = input.objects[i].pose.position.y;
    p.z = 1.0;

    meas_points.points.push_back(p);
  }
  pub_points_.publish(target_points);
  pub_points_.publish(meas_points);
  pub_texts_array_.publish(texts_markers);
}

void ImmUkfPda::tracker(const autoware_msgs::DetectedObjectArray& input,
                        jsk_recognition_msgs::BoundingBoxArray& jskbboxes_output,
                        autoware_msgs::DetectedObjectArray& detected_objects_output)
{
  double timestamp = input.header.stamp.toSec();

  const double det_explode_param = 10;
  const double cov_explode_param = 1000;

  if (!init_)
  {
    initTracker(input, timestamp);
    return;
  }

  double dt = (timestamp - timestamp_);
  timestamp_ = timestamp;
  // // used for making new target with no data association
  std::vector<bool> matching_vec(input.objects.size(), false);  // make 0 vector

  // start UKF process
  for (size_t i = 0; i < targets_.size(); i++)
  {
    // reset is_vis_bb_ to false
    targets_[i].is_vis_bb_ = false;
    targets_[i].is_static_ = false;

    // todo: modify here. This skips irregular measurement and nan
    if (targets_[i].tracking_num_ == TrackingState::Die)
      continue;
    // prevent ukf not to explode
    if (targets_[i].p_merge_.determinant() > det_explode_param || targets_[i].p_merge_(4, 4) > cov_explode_param)
    {
      targets_[i].tracking_num_ = TrackingState::Die;
      continue;
    }
    // immukf prediction step
    targets_[i].predictionIMMUKF(dt);

    bool is_skip_target;
    std::vector<double> lambda_vec;
    probabilisticDataAssociation(input, dt, det_explode_param, matching_vec, lambda_vec, targets_[i], is_skip_target);
    if (is_skip_target)
      continue;

    // immukf update step
    targets_[i].updateIMMUKF(lambda_vec);
  }
  // end UKF process

  //debug, green is for measurement points, red is for estimated points
  pubPoints(input);

  // making new ukf target for no data association clusters
  makeNewTargets(timestamp, input, matching_vec);

  // static dynamic classification
  staticClassification();

  // making output(CludClusterArray) for visualization
  makeOutput(input, jskbboxes_output, detected_objects_output);

  removeUnnecessaryTarget();

  if(jskbboxes_output.boxes.size() != detected_objects_output.objects.size())
  {
    ROS_ERROR("Something wrong");
  }
  if(matching_vec.size() != input.objects.size())
  {
    ROS_ERROR("Something wrong");
  }
}
