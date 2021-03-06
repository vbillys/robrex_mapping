/**
 *  @file surfel_mapper_node.cpp
 *  @author Artur Wilkowski <ArturWilkowski@piap.pl>
 * 
 *  @section LICENSE
 *
 *  Copyright (C) 2015, Industrial Research Institute for Automation and Measurements
 *  Security and Defence Systems Division <http://www.piap.pl>
 */

#include "ros/ros.h"
#include "nav_msgs/Path.h"
#include "sensor_msgs/PointCloud2.h"
#include <sensor_msgs/CameraInfo.h>
#include "visualization_msgs/Marker.h"

#include "visualization_msgs/MarkerArray.h"
#include "pcl_conversions/pcl_conversions.h"
#include "pcl/common/point_tests.h"
#include "pcl/common/io.h"
#include "pcl/filters/filter.h"
#include <Eigen/Geometry>
#include <limits.h>
#include <tf/transform_listener.h>
#include <tf_conversions/tf_eigen.h>  
#include "surfel_mapper.hpp"
#include "surfel_mapper/ResetMap.h"
#include "surfel_mapper/PublishMap.h"
#include "surfel_mapper/SaveMap.h"
#include <algorithm>
#include <math.h>


//Node parameters
double dmax ; /**< @brief distance threshold for surfel update*/ 
double min_kinect_dist ; /**< @brief reliable minimum sensor reading distance*/
double max_kinect_dist ; /**< @brief reliable maximum sensor reading distance*/
double octree_resolution ; /**< @brief resolution of underlying octree*/
double preview_resolution ; /**< @brief resolution of output preview map*/
int preview_color_samples_in_voxel ; /**< @brief number of samples in voxel used for constructing preview point (affects preview efficiency)*/
int confidence_threshold ; /**< @brief confidence threshold used for establishing reliable surfels*/
double min_scan_znormal ; /**< @brief acceptable minimum z-component of scan normal*/
bool use_frustum ; /**< @brief use frustum or no*/
int scene_size ; /**< @brief preallocated size of scene*/
bool logging ; /**< @brief logging turned on or off*/
bool use_update ; /**< @brief use surfel update or no*/

/**
 * @brief Structure describing sensor pose
 */
struct SensorPose {
	public:
		Eigen::Quaternionf orientation ; /**< @brief sensor orientation */
		Eigen::Vector4f origin ; /**< @brief sensor origin */
} ;

//typedef std::list<pcl::PointCloud<pcl::PointXYZRGB>::Ptr> PointCloudMsgListT ;
typedef std::list<sensor_msgs::PointCloud2::ConstPtr> PointCloudMsgListT ; /**< @brief message list of points clouds */

nav_msgs::Path::ConstPtr current_path ; /**< @brief pointer to the current path message */
PointCloudMsgListT cloudMsgQueue ; /**< @brief queue of point cloud messages */ 

//Eigen::Matrix4d cameraRgbToCameraLinkTrans ;
boost::shared_ptr<SurfelMapper> mapper ; /**< @brief mapper pointer */

ros::Publisher surfel_map_pub ; /**< @brief surfel mapper publisher */ 

//ccny_rgbd uses timestamps for keyframes compatible with rgb camera, but odometry path is time stamped anew (so it can be actually some microseconds later than keyframe
//simple workaround is to round time stamps to miliseconds.TODO: possibly some patch to ccny_rgbd could be proposed?

/**
 * @brief rounds the time stamp to miliseconds
 * 
 * @param time_stamp time stamp to round
 * @return rounded time stamp
 */
ros::Time roundTimeStamp(const ros::Time &time_stamp)
{
	ros::Time rounded_time_stamp = time_stamp ;
	uint32_t time_stamp_nsec_mod = time_stamp.nsec % 1000000l ;   
	rounded_time_stamp.nsec -= time_stamp_nsec_mod ; //Results in full milliseconds contained in the timestamp 
		if (time_stamp_nsec_mod > 500000l) { //If the remainder was grater than 0.5 millisecond
			rounded_time_stamp.nsec += 1000000l ; //Round up to the full milisecond
			if (rounded_time_stamp.nsec == 1000000000l) { //And if after addition we hit a full second (should not overflow on int32)
				rounded_time_stamp.sec++ ;
				rounded_time_stamp.nsec = 0l ;
			}
		}
	return rounded_time_stamp ;
}

/**
 * @brief Retrieves sensor position associated with the given timestamp
 *
 * @param time_stamp time stamp to search for
 * @param sensor_pose output sensor pose
 * @return true if the time stamp was found, false otherwise
 */
bool getSensorPosition(const ros::Time &time_stamp, SensorPose &sensor_pose)
{
	ros::Time time_stamp_rounded = roundTimeStamp(time_stamp) ;	
	if (!current_path) {
		ROS_WARN("No odometry path message available!") ;
		return false ;
	} else if (current_path->poses.empty()) {
		ROS_WARN("Empty list of poses in odometry path message") ;
		return false ;
	} else if (roundTimeStamp(current_path->poses.front().header.stamp) > time_stamp_rounded || roundTimeStamp(current_path->poses.back().header.stamp) < time_stamp_rounded) {
		ROS_WARN("Odometry path message does not contain pose corresponding with the keyframe. Keyframe timestamp (rounded) [%d.%d]. Odometry timestamps (rounded) [%d.%d]-[%d.%d]", 
				time_stamp_rounded.sec, time_stamp_rounded.nsec, roundTimeStamp(current_path->poses.front().header.stamp).sec, roundTimeStamp(current_path->poses.front().header.stamp).nsec, 
				roundTimeStamp(current_path->poses.back().header.stamp).sec, roundTimeStamp(current_path->poses.back().header.stamp).nsec) ;
		return false ;
	} else {
		//Search by bi-section
		size_t i, j, k ;
		i = 0 ; j = current_path->poses.size() - 1 ;
		while (i + 1 < j) {
			k = (i + j) / 2 ;
			if (roundTimeStamp(current_path->poses[k].header.stamp) <= time_stamp_rounded)
				i = k ;
			else
				j = k ;
		}

		//Find closest match (nearest neighbor)	
		ros::Duration duri = roundTimeStamp(time_stamp) - roundTimeStamp(current_path->poses[i].header.stamp) ;
		ros::Duration durj = roundTimeStamp(current_path->poses[j].header.stamp) - roundTimeStamp(time_stamp) ;
		if (duri < durj)
			k = i ;
		else
			k = j ;

		geometry_msgs::PoseStamped pose_stamped = current_path->poses[k] ;
		//ROS_INFO("Stamp found for k = %ld (out of %ld), number of steps [%ld]", k, current_path->poses.size(), steps) ;
		//ROS_INFO("search time stamp [%d,%d], found time stamp [%d,%d]", time_stamp.sec, time_stamp.nsec, current_path->poses[i].header.stamp.sec, current_path->poses[i].header.stamp.nsec) ;
		//ROS_INFO("search time stamp [%d,%d], found time stamp [%d,%d]", time_stamp.sec, time_stamp.nsec, current_path->poses[j].header.stamp.sec, current_path->poses[j].header.stamp.nsec) ;
		ROS_INFO("search time stamp (rounded) [%d,%d], found time stamp (rounded) [%d,%d]", time_stamp_rounded.sec, time_stamp_rounded.nsec, roundTimeStamp(pose_stamped.header.stamp).sec, roundTimeStamp(pose_stamped.header.stamp).nsec) ;

		sensor_pose.origin = Eigen::Vector4f((float) pose_stamped.pose.position.x, (float) pose_stamped.pose.position.y, (float) pose_stamped.pose.position.z, 1.0f) ;
		sensor_pose.orientation = Eigen::Quaternionf((float) pose_stamped.pose.orientation.w, (float) pose_stamped.pose.orientation.x, 
							     (float) pose_stamped.pose.orientation.y, (float) pose_stamped.pose.orientation.z) ;
		std::cout << "Orientation: " << pose_stamped.pose.orientation.w << " " << pose_stamped.pose.orientation.x << " " << pose_stamped.pose.orientation.y << " " << pose_stamped.pose.orientation.z << std::endl ;
		std::cout << "Pose: " << pose_stamped.pose.position.x << " " << pose_stamped.pose.position.y << " " << pose_stamped.pose.position.z << " " << std::endl ;
		return true ;
	}
}

/**
 * @brief Process a queue of buffered cloud messages 
 */
void processCloudMsgQueue()
{
	//Try to associate clouds from the queue with appropriate transforms and process them
	if (mapper) {
		while(!cloudMsgQueue.empty()) {
			SensorPose sensor_pose ;
			const sensor_msgs::PointCloud2::ConstPtr& msg = cloudMsgQueue.front() ;
			bool res = getSensorPosition(msg->header.stamp, sensor_pose) ;
			if (res) {
				//Convert message to PointCloud
				pcl::PCLPointCloud2 pcl_pc2;
				pcl_conversions::toPCL(*msg, pcl_pc2);
				pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZRGB>());
				pcl::fromPCLPointCloud2(pcl_pc2, *cloud);

				//Fix sensor pose		
				cloud->sensor_origin_ = sensor_pose.origin ;
				cloud->sensor_orientation_ = sensor_pose.orientation ;

				//Add cloud to the map
				ROS_INFO("-------------->Adding point cloud [%d, %d]", msg->header.stamp.sec, msg->header.stamp.nsec) ;
				ROS_INFO("Sensor position data: [%f, %f, %f, %f] ", cloud->sensor_origin_.x(), cloud->sensor_origin_.y(), cloud->sensor_origin_.z(), cloud->sensor_origin_.w()) ;
				ROS_INFO("Sensor orientation data: [%f, %f, %f, %f] ", cloud->sensor_orientation_.x(), cloud->sensor_orientation_.y(), cloud->sensor_orientation_.z(), cloud->sensor_orientation_.w()) ;

				mapper->addPointCloudToScene(cloud) ;
				//addPointCloudToScene1(cloud) ;

				//Remove message from queue
				cloudMsgQueue.pop_front() ;	
			} else break ;
		}
	} else 
		ROS_INFO("processCloudMsgQueue: mapper not initialized") ;
}

/**
 * @brief Callback for the incoming path message
 *
 * @param msg incoming path message
 */
void pathCallback(const nav_msgs::Path::ConstPtr& msg)
{
	ROS_DEBUG("pathCallback: [%s]", msg->header.frame_id.c_str());
	current_path = msg ;
}

/**
 * @brief Callback for the incoming keyframe (cloud) message 
 *
 * @param msg incoming point cloud message 
 */
void keyframeCallback(const sensor_msgs::PointCloud2::ConstPtr& msg)
{
	ROS_INFO("keyframeCallback: [%s]", msg->header.frame_id.c_str());
	//Add point cloud to our local queue (the queue is needed since we must sometimes wait for a transform from a path)
	cloudMsgQueue.push_back(msg) ;	
	processCloudMsgQueue() ;
}

/**
 * @brief Callback for the incoming camera info message 
 *
 * @param msg incoming camera info message 
 */
void cameraInfoCallback(const sensor_msgs::CameraInfo::ConstPtr &msg)
{
	if (!mapper) {

		ROS_INFO("cameraInfoCallback: camera params message arrived [%s]", msg->header.frame_id.c_str());

		CameraParams camera_params ;
		camera_params.alpha = msg->K[0] ;
		camera_params.beta = msg->K[4] ;
		camera_params.cx = msg->K[2] ;
		camera_params.cy = msg->K[5] ;
		
		
		mapper.reset(new SurfelMapper(dmax, min_kinect_dist, max_kinect_dist, octree_resolution,
						preview_resolution, preview_color_samples_in_voxel,
						confidence_threshold, min_scan_znormal, 
						use_frustum, scene_size, logging, use_update, camera_params)) ;

		processCloudMsgQueue() ; //In case we only waited for camera_info message
	}
}


/**
 * @brief Sends downsampled cloud message 
 *
 * @param downsampled_map_pub publisher of the downsampled clouds 
 */
void sendDownsampledMapMessage(ros::Publisher &downsampled_map_pub) 
{
	pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloudSceneDownsampled = mapper->getCloudSceneDownsampled() ;

	pcl::PCLPointCloud2 pcl_pc2;
	pcl::toPCLPointCloud2(*cloudSceneDownsampled, pcl_pc2) ;
	sensor_msgs::PointCloud2 cloud_msg ;
	pcl_conversions::fromPCL(pcl_pc2, cloud_msg) ;
	cloud_msg.header.frame_id = "/odom" ;
	downsampled_map_pub.publish(cloud_msg) ;
}

/**
 * @brief Upper limit for the number of markers in a single displayed map fragment 
 */
#define MAX_MARKERS 100000 

/**
 * @brief Sends surfel map message 
 *
 * The cloud is sent in the form of the MarkeArray. For efficiency only a part of the map 
 * is sent so the bounding box must be set appropriately 
 *
 * @param map_pub surfel map publisher 
 * @param min_bb coordinates of the first corner of the bounding box
 * @param max_bb coordinates of the second corner of the bounding box
 */
void sendMapMessage(ros::Publisher &map_pub, Eigen::Vector3f &min_bb, Eigen::Vector3f &max_bb) 
{
	pcl::PointCloud<PointCustomSurfel>::Ptr cloudScene = mapper->getCloudScene() ;
	std::vector<int> point_indices ;
	mapper->getBoundingBoxIndices(min_bb, max_bb, point_indices) ;
	
	//When iterating through point cloud - we encounter also NaN surfels: 
	//TODO: might be better to iterate the original tree or remove NaNs from the cloud before sending

	visualization_msgs::Marker marker;
	visualization_msgs::MarkerArray marray ;
	
	marker.header.frame_id = "/odom";
	marker.header.stamp = ros::Time();
	marker.ns = "surfelmap";
	marker.type = visualization_msgs::Marker::CYLINDER ;
	marker.action = visualization_msgs::Marker::ADD ;

	marker.color.a = 1.0f;
	marker.pose.orientation.w = 1.0f;

	Eigen::Vector3f zaxis(0.0f, 0.0f, 1.0f) ;
	Eigen::Quaternionf orientation ; 
	size_t nmarkers = std::min<unsigned int>(point_indices.size(), MAX_MARKERS) ;
	for (size_t i = 0; i < nmarkers ; i++) {
		PointCustomSurfel &point = cloudScene->at(point_indices[i]) ;
		if (pcl::isFinite(point) && i % 2 == 0) { 
			Eigen::Vector3f normal(point.normal_x, point.normal_y, point.normal_z) ;
			orientation.setFromTwoVectors(zaxis, normal) ;

			//if (std::isnan(orientation.x())) {
				//std::cout << "Orientation " << orientation.x() << " " << orientation.y() << " " << orientation.z() << " " << orientation.w() << std::endl ;
				//std::cout << "Normal " << normal << std::endl ;
				//std::cout << "zaxis " << zaxis << std::endl ;
			//}
			//Set marker data
			marker.id = i;
			marker.pose.position.x = point.x ;
			marker.pose.position.y = point.y ;
			marker.pose.position.z = point.z ;
			marker.pose.orientation.x = orientation.x() ;
			marker.pose.orientation.y = orientation.y() ;
			marker.pose.orientation.z = orientation.z() ;
			marker.pose.orientation.w = orientation.w() ;

			marker.scale.x = point.radius * 2.0 ;
			marker.scale.y = point.radius * 2.0 ;
			marker.scale.z = 0.0001 ;
			marker.color.r = point.r / 255.0f ;
			marker.color.g = point.g / 255.0f ;
			marker.color.b = point.b / 255.0f ;
			marray.markers.push_back(marker) ;
		}
	}
	if (nmarkers < point_indices.size())
		ROS_INFO("Number of points [%d] too large for marker publishing", (int) point_indices.size()) ;
	ROS_INFO("Publishing: %d points ", (int) nmarkers) ;
	
	surfel_map_pub.publish(marray) ;
}

/**
 * @brief Saves map under the filename specified 
 *
 * Only XYZRGB components of surfels are saved.
 *
 * @param fileName point cloud file name 
 */
void saveMap(const std::string &fileName) 
{
	//Copy map to standard RGBXYZ point cloud. Leave only points that are actually valid (octree indices are present) 
	pcl::PointCloud<PointCustomSurfel>::Ptr cloud = mapper->getCloudScene() ;
	pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloudXYZRGB(new pcl::PointCloud<pcl::PointXYZRGB>) ;
	pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloudXYZRGBfilt(new pcl::PointCloud<pcl::PointXYZRGB>) ;
	std::vector<int> indv ;
	mapper->getAllIndices(indv) ;
	//Divide point downcasting and filtering in two steps (one step approach causes crash frequently...)
	//Investigate if this was due to unordered indices

	pcl::copyPointCloud(*cloud, *cloudXYZRGB) ;

	//We cannot perform in-place copy - indices may be unordered
	pcl::copyPointCloud(*cloudXYZRGB, indv, *cloudXYZRGBfilt) ;

	pcl::io::savePCDFileBinary(fileName, *cloudXYZRGBfilt) ;

	/*	
	std::cout << "Cloud length: " << cloudXYZRGB->size() << std::endl ;
	//pcl::removeNaNFromPointCloud(*cloudXYZRGB, *cloudXYZRGB, indv) ;
	int count = 0 ;
	for (int i = 0; i < cloudXYZRGB->points.size() ; i++)
		if (!pcl_isfinite(cloudXYZRGB->points[i].x))
			count++ ;	

	std::vector<int> ind1 ;
	ind1.resize(cloudXYZRGB->size()) ;	
	for (int i = 0; i < ind1.size() ; i++)
		ind1[i] = 0 ;
	
	for (int i = 0; i < indv.size() ; i++)
		ind1[indv[i]] = 1 ;

	int count1 = 0 ;
	int count2 = 0 ;
	for (int i = 0; i < ind1.size() ; i++)
		if (ind1[i] == 0) {
			std::cout << "*" << i << "+" << cloudXYZRGB->points[i].x << std::endl ;
			count1++ ;
			if (pcl_isfinite(cloudXYZRGB->points[i].x))
				count2++ ;
		}

	std::cout << "Number of non nans: " << indv.size() << std::endl ;
	std::cout << "Number of nans: " << count << std::endl ;
	std::cout << "Cloud length: " << cloudXYZRGB->size() << std::endl ;
	std::cout << "Count1: " << count1 << std::endl ;
	std::cout << "Count2: " << count2 << std::endl ;

	if (mapper->getPointCount() != indv.size())
		std::cerr << "saveMap: mapper->getPointCount does not match cloud index vector size" ;

	std::cout << "Saving map. Point count: " << mapper->getPointCount() << std::endl ;
	std::cout.flush() ;
	*/	
}

/**
 * @brief Callback for the ResetMap service. 
 *
 * Removes all surfels from the map.
 *
 * @param request service request object
 * @param response service response object
 *
 * @return true if service call is correctly handled
 */
bool resetMapCallback(
  surfel_mapper::ResetMap::Request& request,
  surfel_mapper::ResetMap::Response& response)
{
	ROS_INFO("ResetMap request arrived") ;	
	if (mapper) {
		mapper->resetMap() ;
		ROS_INFO("The map has been reset") ;	
	} else
		ROS_INFO("resetMapCallback: Mapper not initialized.") ;

	return true ;
}

/**
 * @brief Callback for the PublishMap service. 
 *
 * Publishes a fragment of the map.
 *
 * @param request service request object
 * @param response service response object
 *
 * @return true if service call is correctly handled
 */
bool publishMapCallback(
  surfel_mapper::PublishMap::Request& request,
  surfel_mapper::PublishMap::Response& response)
{
	Eigen::Vector3f minbb(request.x1, request.y1, request. z1) ;
	Eigen::Vector3f maxbb(request.x2, request.y2, request. z2) ;

	ROS_INFO("PublishMap request arrived for bb. [%f,%f,%f]-[%f,%f,%f]", minbb[0], minbb[1], minbb[2], maxbb[0], maxbb[1], maxbb[2]) ;	
	if (mapper) {
		sendMapMessage(surfel_map_pub, minbb, maxbb) ;	
		ROS_INFO("The map has been sent") ;	
	} else
		ROS_INFO("resetMapCallback: Mapper not initialized.") ;
	return true ;
}

/**
 * @brief Callback for the SaveMap service. 
 *
 * Save the current map as a RGBXYZ point cloud 
 *
 * @param request service request object
 * @param response service response object
 *
 * @return true if service call is correctly handled
 */
bool saveMapCallback(
  surfel_mapper::SaveMap::Request& request,
  surfel_mapper::SaveMap::Response& response)
{
	ROS_INFO("SaveMap request arrived.") ;	
	if (mapper) {
		saveMap("cloud.pcd") ;	
		ROS_INFO("The map has been saved") ;	
	} else
		ROS_INFO("saveMapCallback: Mapper not initialized.") ;
	return true ;
}

/**
 * @brief Main program function 
 *
 * @param argc program argument count 
 * @param argv program argument values 
 *
 * @return 0 on correct exit, 1 on failure
 */
int main(int argc, char **argv)
{
	ros::init(argc, argv, "surfel_mapper");
	ros::NodeHandle n ;
	ros::NodeHandle np("~") ;

	//Parse parameters
	if (!np.getParam("dmax", dmax)) dmax = 0.005f ;
	if (!np.getParam("min_kinect_dist", min_kinect_dist)) min_kinect_dist = 0.8 ;
	if (!np.getParam("max_kinect_dist", max_kinect_dist))  max_kinect_dist = 4.0 ;
	if (!np.getParam("octree_resolution", octree_resolution)) octree_resolution = 0.2 ;
	if (!np.getParam("preview_resolution", preview_resolution)) preview_resolution= 0.2 ;
	if (!np.getParam("preview_color_samples_in_voxel", preview_color_samples_in_voxel)) preview_color_samples_in_voxel = 3 ;
	if (!np.getParam("confidence_threshold", confidence_threshold)) confidence_threshold = 5 ;
	if (!np.getParam("min_scan_znormal", min_scan_znormal)) min_scan_znormal = 0.2f ;
	if (!np.getParam("use_frustum", use_frustum)) use_frustum = true ;
	if (!np.getParam("scene_size", scene_size)) scene_size = 3e7 ;
	if (!np.getParam("logging", logging)) logging = true ;
	if (!np.getParam("use_update", use_update)) use_update = true ;

	ros::Subscriber sub_path = n.subscribe("mapper_path", 3, pathCallback);
	ros::Subscriber sub_keyframe = n.subscribe("keyframes", 200, keyframeCallback);
	ros::Subscriber sub_camerainfo = n.subscribe("camera/rgb/camera_info", 3, cameraInfoCallback);

	ros::Publisher downsampled_map_pub = n.advertise<sensor_msgs::PointCloud2>("surfelmap_preview", 5);
	surfel_map_pub = n.advertise<visualization_msgs::MarkerArray>( "surfelmap", 1);

	ros::ServiceServer resetmap_service = n.advertiseService("reset_map", resetMapCallback);
	ros::ServiceServer publishmap_service = n.advertiseService("publish_map", publishMapCallback);
	ros::ServiceServer savemap_service = n.advertiseService("save_map", saveMapCallback);

	ros::Rate r(2) ;

	//testOctreeIterator() ;

	tf::TransformListener listener ;

	/*while(ros::ok()) {
		ROS_INFO("Waiting for camera_link->camera_rgb_optical_frame transform") ;
		tf::StampedTransform transform;
		try {
			listener.waitForTransform("camera_link", "camera_rgb_optical_frame", ros::Time(0), ros::Duration(0.5) );
			listener.lookupTransform("camera_link", "camera_rgb_optical_frame", ros::Time(0), transform);
			ROS_INFO("Transform found!") ;
			Eigen::Affine3d affTrans ;
			tf::transformTFToEigen(transform, affTrans) ;
			//cameraRgbToCameraLinkTrans = affTrans.matrix().inverse() ;
			cameraRgbToCameraLinkTrans = affTrans.matrix().inverse() ;
			std::cout << cameraRgbToCameraLinkTrans << std::endl ;
			break ;
		}
		catch (tf::TransformException &ex) {
			ROS_ERROR("%s",ex.what());
			r.sleep() ;
		}
	}*/


	ros::Time time_stamp, time_stamp_rounded ;
	time_stamp.sec = 100l ;
	time_stamp.nsec = 999501341l ;
	

	time_stamp_rounded = roundTimeStamp(time_stamp) ;

	ROS_INFO("Test time_stamp rounding: original: sec = %d, nsec = %d", time_stamp.sec, time_stamp.nsec) ;
	ROS_INFO("Test time_stamp rounding: rounded: sec = %d, nsec = %d", time_stamp_rounded.sec, time_stamp_rounded.nsec) ;

	while(ros::ok()) {
		ros::spinOnce();
		processCloudMsgQueue() ;
		if (mapper) {
			ros::Time start = ros::Time::now() ;
			sendDownsampledMapMessage(downsampled_map_pub) ;
			ros::Time stop = ros::Time::now() ;
			ROS_DEBUG("Sending Map Message time (s): [%.6lf]", (stop - start).toSec()) ;
		} else 
			ROS_INFO("Downsampled map not sent. Mapper is not initialized.") ;
		r.sleep() ;

		//ros::Time time_stamp ;
		//time_stamp.sec = 1408952265 ;
		//time_stamp.nsec = 954192400 ;
	
		//SensorPose sensor_pose ;

		//ros::Time begin = ros::Time::now();
		//bool ret = getSensorPosition(time_stamp, sensor_pose) ;
		//ros::Time end = ros::Time::now();
		//ros::Duration duration = end - begin ;
		//ROS_INFO("getSensorPosition: %.6lf (s) ", duration.toSec()) ;
		
		//ROS_INFO("Sensor position correctly acquired: [%d] ", ret) ;
		//ROS_INFO("Sensor position data: [%f, %f, %f, %f] ", sensor_pose.origin.x(), sensor_pose.origin.y(), sensor_pose.origin.z(), sensor_pose.origin.w()) ;
		//ROS_INFO("Sensor orientation data: [%f, %f, %f, %f] ", sensor_pose.orientation.x(), sensor_pose.orientation.y(), sensor_pose.orientation.z(), sensor_pose.orientation.w()) ;
	}

	return 0;
}
