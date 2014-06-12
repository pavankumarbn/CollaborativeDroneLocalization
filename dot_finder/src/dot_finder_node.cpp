/*
 * dot_finder_node.cpp
 *
 * Created on: Jul 29, 2013
 *      Author: Karl Schwabe
 *  Edited on:  May 14 2014
 *      Author: Philippe Bbin
 */

/** \file mutual_camera_localizator_node.cpp
 * \brief File containing the main function of the package
 *
 * This file is responsible for the flow of the program.
 *
 */
using namespace std;
#include "dot_finder/dot_finder_node.h"

namespace dot_finder
{

/**
 * Constructor of the Dot finder Node class
 *
 */
DotFinder::DotFinder()
{
  // Generate the name of the publishing and subscribing topics
  string topic_image_raw, topic_camera_info, topic_dots, topic_image_with_detections;
  ros::param::get("~topic", m_topic);
  topic_image_raw = m_topic + "/image_raw";
 // topic_camera_info = "cameraA/camera_info";
  topic_camera_info = m_topic + "/camera_info";
  topic_dots = m_topic + "/dots";
  topic_image_with_detections = m_topic + "/image_with_detections";

  // Set up a dynamic reconfigure server.
  // This should be done before reading parameter server values.
  dynamic_reconfigure::Server<dot_finder::DotFinderConfig>::CallbackType m_cb;
  m_cb = boost::bind(&DotFinder::dynamicParametersCallback, this, _1, _2);
  m_dr_server.setCallback(m_cb);

  // Initialize subscribers
  ROS_INFO("Subscribing to %s and %s ", topic_image_raw.c_str(), topic_camera_info.c_str());
  m_image_sub = m_nodeHandler.subscribe(topic_image_raw, 1, &DotFinder::imageCallback, this);
  m_camera_info_sub = m_nodeHandler.subscribe(topic_camera_info, 1, &DotFinder::cameraInfoCallback, this);

  // Initialize detected leds publisher
  m_dot_hypothesis_pub = m_nodeHandler.advertise<dot_finder::DuoDot>(topic_dots, 1);


  // Initialize image publisher for visualization
  image_transport::ImageTransport image_transport(m_nodeHandler);
  m_image_pub = image_transport.advertise(topic_image_with_detections, 1);
}

DotFinder::~DotFinder(){}

/**
 * The callback function that retrieves the camera calibration information
 *
 * \param msg the ROS message containing the camera calibration information
 *
 */
void DotFinder::cameraInfoCallback(const sensor_msgs::CameraInfo::ConstPtr& msg){
  if (!m_have_camera_info)
  {
    m_cam_info = *msg;

    // Calibrated camera
    Eigen::Matrix<double, 3, 4> camera_matrix;
    camera_matrix(0, 0) = m_cam_info.P[0];
    camera_matrix(0, 2) = m_cam_info.P[2];
    camera_matrix(1, 1) = m_cam_info.P[5];
    camera_matrix(1, 2) = m_cam_info.P[6];
    camera_matrix(2, 2) = 1.0;
    
    m_camera_matrix_K = cv::Mat(3, 3, CV_64F);
    m_camera_matrix_P = cv::Mat(3, 4, CV_64F);

    m_camera_matrix_K.at<double>(0, 0) = m_cam_info.K[0];
    m_camera_matrix_K.at<double>(0, 1) = m_cam_info.K[1];
    m_camera_matrix_K.at<double>(0, 2) = m_cam_info.K[2];
    m_camera_matrix_K.at<double>(1, 0) = m_cam_info.K[3];
    m_camera_matrix_K.at<double>(1, 1) = m_cam_info.K[4];
    m_camera_matrix_K.at<double>(1, 2) = m_cam_info.K[5];
    m_camera_matrix_K.at<double>(2, 0) = m_cam_info.K[6];
    m_camera_matrix_K.at<double>(2, 1) = m_cam_info.K[7];
    m_camera_matrix_K.at<double>(2, 2) = m_cam_info.K[8];
    m_camera_distortion_coeffs = m_cam_info.D;
    m_camera_matrix_P.at<double>(0, 0) = m_cam_info.P[0];
    m_camera_matrix_P.at<double>(0, 1) = m_cam_info.P[1];
    m_camera_matrix_P.at<double>(0, 2) = m_cam_info.P[2];
    m_camera_matrix_P.at<double>(0, 3) = m_cam_info.P[3];
    m_camera_matrix_P.at<double>(1, 0) = m_cam_info.P[4];
    m_camera_matrix_P.at<double>(1, 1) = m_cam_info.P[5];
    m_camera_matrix_P.at<double>(1, 2) = m_cam_info.P[6];
    m_camera_matrix_P.at<double>(1, 3) = m_cam_info.P[7];
    m_camera_matrix_P.at<double>(2, 0) = m_cam_info.P[8];
    m_camera_matrix_P.at<double>(2, 1) = m_cam_info.P[9];
    m_camera_matrix_P.at<double>(2, 2) = m_cam_info.P[10];
    m_camera_matrix_P.at<double>(2, 3) = m_cam_info.P[11];

    m_camera_projection_matrix = camera_matrix;
    m_have_camera_info = true;
    ROS_INFO("Camera calibration information obtained.");
  }

}

/**
 * The callback function that is executed every time an image is received. It runs the main logic of the program.
 *
 * \param image_msg the ROS message containing the image to be processed
 */
void DotFinder::imageCallback(const sensor_msgs::Image::ConstPtr& image_msg)
{

  // Check whether already received the camera calibration data
  if (!m_have_camera_info)
  {
    ROS_WARN("No camera info yet...");
    return;
  }

  // Import the image from ROS message to OpenCV mat
  cv_bridge::CvImagePtr cv_ptr;
  try
  {
    cv_ptr = cv_bridge::toCvCopy(image_msg, sensor_msgs::image_encodings::BGR16);// MONO
  }
  catch (cv_bridge::Exception& e)
  {
    ROS_ERROR("cv_bridge exception: %s", e.what());
    return;
  }

  publishDetectedLed(cv_ptr->image);

  publishVisualizationImage(cv_ptr->image, image_msg);
}

void DotFinder::publishDetectedLed(cv::Mat image){
  m_region_of_interest = cv::Rect(0, 0, image.cols, image.rows);

    // Do detection of LEDs in image
  std::vector< std::vector<cv::Point2f> > dots_hypothesis_undistorted;

  // Convert image to gray
  cv::Mat bwImage;
  cv::cvtColor(image , bwImage, CV_BGR2GRAY, 1);
  LEDDetector::LedFilteringArDrone(bwImage, m_min_blob_area, m_max_blob_area, m_max_circular_distortion,
              m_radius_ratio_tolerance, m_ratio_int_tolerance, m_hor_line_angle,
              m_ratio_ellipse_min, m_ratio_ellipse_max,
              m_pos_ratio, m_pos_ratio_tolerance,
              m_acos_tolerance,
              m_dots_hypothesis_distorted, dots_hypothesis_undistorted,
              m_camera_matrix_K, m_camera_distortion_coeffs, m_camera_matrix_P, 0);
  
  if(dots_hypothesis_undistorted.size() == 0){
    ROS_WARN("No LED detected");
    return;
  }

  // Publication of the position of the possible position in the image
  dot_finder::DuoDot duoDot_msg_to_publish;
  geometry_msgs::Pose2D position_in_image;
  for (int i = 0; i < dots_hypothesis_undistorted.size(); i++){
    //Left dot
    position_in_image.x = dots_hypothesis_undistorted[i][0].x;
    position_in_image.y = dots_hypothesis_undistorted[i][0].y;
    duoDot_msg_to_publish.leftDot.push_back(position_in_image);

    //Right dot
    position_in_image.x = dots_hypothesis_undistorted[i].back().x;
    position_in_image.y = dots_hypothesis_undistorted[i].back().y;
    duoDot_msg_to_publish.rightDot.push_back(position_in_image);
  }

  duoDot_msg_to_publish.topicName = m_topic;
  // We also publish the focal length
  duoDot_msg_to_publish.fx = m_camera_matrix_K.at<double>(0, 0);
  duoDot_msg_to_publish.fy = m_camera_matrix_K.at<double>(1, 1);

  duoDot_msg_to_publish.px = m_camera_matrix_K.at<double>(0, 2);
  duoDot_msg_to_publish.py = m_camera_matrix_K.at<double>(1, 2);

  m_dot_hypothesis_pub.publish(duoDot_msg_to_publish);
  
}

void DotFinder::publishVisualizationImage(cv::Mat &image, const sensor_msgs::Image::ConstPtr& image_msg){
   cv::Mat imgHSV;
   //vector<cv::Mat> split_image(3);
   //cv::split(image, split_image);// Split color channel
   //cv::threshold(split_image[0], split_image[0], m_threshold, 255, cv::THRESH_TOZERO);

   //cv::cvtColor(split_image[0] , colorImage, CV_GRAY2BGR); // Convert to color image for visualisation
   cv::cvtColor(image , imgHSV, CV_BGR2HSV); // Convert to color image for visualisation


   cv::Mat imgThresholded;

   if(m_lowH < 0){// We split the lower and higher part of the interval than we fusion them
       cv::Mat lowImgThresholded;
       cv::Mat highImgThresholded;
       cv::inRange(imgHSV, cv::Scalar(180 + m_lowH, m_lowS, m_lowV), cv::Scalar(179, m_highS, m_highV), lowImgThresholded); //Lower part threshold

       cv::inRange(imgHSV, cv::Scalar(0, m_lowS, m_lowV), cv::Scalar(m_highH, m_highS, m_highV), highImgThresholded); //Higher part threshold

       bitwise_or(lowImgThresholded,highImgThresholded, imgThresholded); //Fusion of the two range
   }
   else{
       cv::inRange(imgHSV, cv::Scalar(m_lowH, m_lowS, m_lowV), cv::Scalar(m_highH, m_highS, m_highV), imgThresholded); //Threshold the image
   }


   //image.copyTo(image, imgThresholded);
   //cv::bitwise_and(imgThresholded, imgThresholded, mask= imgThresholded)
   //morphological opening (remove small objects from the foreground)
   //cv::erode(imgThresholded, imgThresholded, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5)) );
   //cv::dilate( imgThresholded, imgThresholded, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5)) );

   //morphological closing (fill small holes in the foreground)
   //cv::dilate( imgThresholded, imgThresholded, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5)) );
  //cv::erode(imgThresholded, imgThresholded, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5)) );


   Visualization::createVisualizationImage(imgThresholded, m_dots_hypothesis_distorted, m_region_of_interest);

  cv::cvtColor(imgThresholded , imgThresholded, CV_GRAY2BGR); // Convert to color image for visualisation
  // Publish image for visualization
  cv_bridge::CvImage visualized_image_msg;
  visualized_image_msg.header = image_msg->header;
  visualized_image_msg.encoding = sensor_msgs::image_encodings::BGR8; //BGR8
  visualized_image_msg.image = imgThresholded;

  m_image_pub.publish(visualized_image_msg.toImageMsg());
}

/**
 * The dynamic reconfigure callback function. This function updates the variable within the program whenever they are changed using dynamic reconfigure.
 */
void DotFinder::dynamicParametersCallback(dot_finder::DotFinderConfig &config, uint32_t level){
  m_highH = config.highH;
  m_highS = config.highS;
  m_highV = config.highV;
  m_lowH = config.lowH;
  m_lowS = config.lowS;
  m_lowV = config.lowV;

  m_threshold = config.threshold;

  m_gaussian_sigma = config.gaussian_sigma;
  m_min_blob_area = config.min_blob_area;
  m_max_blob_area = config.max_blob_area;
  m_max_circular_distortion = config.max_circular_distortion;
  m_ratio_ellipse_min = config.ratio_ellipse_min;
  m_ratio_ellipse_max = config.ratio_ellipse_max;
  m_pos_ratio = config.pos_ratio;
  m_pos_ratio_tolerance = config.pos_ratio_tolerance;
  m_acos_tolerance = config.line_angle_tolerance;
  m_hor_line_angle = config.hor_line_angle;

  m_radius_ratio_tolerance = config.radius_ratio_tolerance;
  m_ratio_int_tolerance = config.ratio_int_tolerance;

  ROS_INFO("Parameters changed");
}

} // namespace dot_finder

int main(int argc, char* argv[])
{
  ros::init(argc, argv, "dot_finder");

  dot_finder::DotFinder dot_finder_node;
  ros::spin();
  return 0;
}