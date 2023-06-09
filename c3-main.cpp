
#include <carla/client/Client.h>
#include <carla/client/ActorBlueprint.h>
#include <carla/client/BlueprintLibrary.h>
#include <carla/client/Map.h>
#include <carla/geom/Location.h>
#include <carla/geom/Transform.h>
#include <carla/client/Sensor.h>
#include <carla/sensor/data/LidarMeasurement.h>
#include <thread>

#include <carla/client/Vehicle.h>

//pcl code
//#include "render/render.h"

namespace cc = carla::client;
namespace cg = carla::geom;
namespace csd = carla::sensor::data;

using namespace std::chrono_literals;
using namespace std::string_literals;

using namespace std;

#include <string>
#include <pcl/io/pcd_io.h>
#include <pcl/visualization/pcl_visualizer.h>
#include <pcl/filters/voxel_grid.h>
#include "helper.h"
#include <sstream>
#include <chrono> 
#include <ctime> 
#include <pcl/registration/icp.h>
#include <pcl/registration/ndt.h>
#include <pcl/console/time.h>   // TicToc

//Eigen库: 一个C++模板库，用于线性代数计算。它提供了一些常用的矩阵和向量操作，如矩阵乘法、矩阵求逆、特征值分解等。
using namespace Eigen;

PointCloudT pclCloud;
cc::Vehicle::Control control;
std::chrono::time_point<std::chrono::system_clock> currentTime;
vector<ControlState> cs;

bool refresh_view = false;
void keyboardEventOccurred(const pcl::visualization::KeyboardEvent &event, void* viewer)
{
  	//boost::shared_ptr<pcl::visualization::PCLVisualizer> viewer = *static_cast<boost::shared_ptr<pcl::visualization::PCLVisualizer> *>(viewer_void);
	if (event.getKeySym() == "Right" && event.keyDown()){
		cs.push_back(ControlState(0, -0.02, 0));
  	}
	else if (event.getKeySym() == "Left" && event.keyDown()){
		cs.push_back(ControlState(0, 0.02, 0)); 
  	}
  	if (event.getKeySym() == "Up" && event.keyDown()){
		cs.push_back(ControlState(0.1, 0, 0));
  	}
	else if (event.getKeySym() == "Down" && event.keyDown()){
		cs.push_back(ControlState(-0.1, 0, 0)); 
  	}
	if(event.getKeySym() == "a" && event.keyDown()){
		refresh_view = true;
	}
}

void Accuate(ControlState response, cc::Vehicle::Control& state){
	if(response.t > 0){
		if(!state.reverse){
			state.throttle = min(state.throttle+response.t, 1.0f);
		}
		else{
			state.reverse = false;
			state.throttle = min(response.t, 1.0f);
		}
	}
	else if(response.t < 0){
		response.t = -response.t;
		if(state.reverse){
			state.throttle = min(state.throttle+response.t, 1.0f);
		}
		else{
			state.reverse = true;
			state.throttle = min(response.t, 1.0f);
		}
	}
	state.steer = min( max(state.steer+response.s, -1.0f), 1.0f);
	state.brake = response.b;
}

void drawCar(Pose pose, int num, Color color, double alpha, pcl::visualization::PCLVisualizer::Ptr& viewer){
	BoxQ box;
	box.bboxTransform = Eigen::Vector3f(pose.position.x, pose.position.y, 0);
    box.bboxQuaternion = getQuaternion(pose.rotation.yaw);
    box.cube_length = 4;
    box.cube_width = 2;
    box.cube_height = 2;
	renderBox(viewer, box, num, color, alpha);
}

/*
Eigen库的Matrix4d: 用于存储3D空间中的变换矩阵
这个函数的作用是使用Normal Distributions Transform (NDT)算法来估计两个点云之间的刚体变换，以便将它们对齐。

NDT算法是一种点云配准算法，它可以在不需要先对点云进行配准初始化的情况下，直接对两个点云进行匹配。
这个算法的基本思想是将点云中的每个点看作一个高斯分布，然后通过计算两个点云之间的高斯分布之间的相似度来估计它们之间的刚体变换。
这个算法的优点是可以处理大规模的点云数据，并且可以在不需要先验信息的情况下进行配准。
reference: Solution: NDT Alignment
*/  	
Matrix4d NDT(
  pcl::NormalDistributionsTransform<pcl::PointXYZ, pcl::PointXYZ> ndt, //NDT对象
  PointCloudT::Ptr source, //一个PointCloudT类型的指针source
  Pose startingPose, //一个Pose类型的startingPose，Pose类型包含了位置和旋转信息
  int iterations) {   	
  
  	//根据startingPose计算出一个初始的变换矩阵init_guess，然后将其传递给NDT对象，  
  	Matrix4f init_guess = transform3D(
      startingPose.rotation.yaw, 
      startingPose.rotation.pitch, 
      startingPose.rotation.roll, 
      startingPose.position.x, 
      startingPose.position.y, 
      startingPose.position.z).cast<float>();

    // Setting max number of registration iterations.
    ndt.setMaximumIterations (iterations);
	
  	// 使用setInputSource()函数设置source作为输入点云
  	ndt.setInputSource (source);

  	pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_ndt (new pcl::PointCloud<pcl::PointXYZ>);
  	
  	// 使用align()函数进行配准，
    ndt.align (*cloud_ndt, init_guess);

  	// 最后返回估计得到的变换矩阵transformation_matrix。
    Matrix4d transformation_matrix = ndt.getFinalTransformation ().cast<double>();

    return transformation_matrix;
}


int main(){
	auto client = cc::Client("localhost", 2000);
	client.SetTimeout(2s);
	auto world = client.GetWorld();

	auto blueprint_library = world.GetBlueprintLibrary();
	auto vehicles = blueprint_library->Filter("vehicle");

	auto map = world.GetMap();
	auto transform = map->GetRecommendedSpawnPoints()[1];
	auto ego_actor = world.SpawnActor((*vehicles)[12], transform);

	// Create lidar
	auto lidar_bp = *(blueprint_library->Find("sensor.lidar.ray_cast"));
	// CANDO: Can modify lidar values to get different scan resolutions
	lidar_bp.SetAttribute("upper_fov", "15");
    lidar_bp.SetAttribute("lower_fov", "-25");
    lidar_bp.SetAttribute("channels", "32");
    lidar_bp.SetAttribute("range", "30");
	lidar_bp.SetAttribute("rotation_frequency", "60");
	lidar_bp.SetAttribute("points_per_second", "500000");

	auto user_offset = cg::Location(0, 0, 0);
	auto lidar_transform = cg::Transform(cg::Location(-0.5, 0, 1.8) + user_offset);
	auto lidar_actor = world.SpawnActor(lidar_bp, lidar_transform, ego_actor.get());
	auto lidar = boost::static_pointer_cast<cc::Sensor>(lidar_actor);
	bool new_scan = true;
	std::chrono::time_point<std::chrono::system_clock> lastScanTime, startTime;

	pcl::visualization::PCLVisualizer::Ptr viewer (new pcl::visualization::PCLVisualizer ("3D Viewer"));
  	viewer->setBackgroundColor (0, 0, 0);
	viewer->registerKeyboardCallback(keyboardEventOccurred, (void*)&viewer);

	auto vehicle = boost::static_pointer_cast<cc::Vehicle>(ego_actor);
	Pose pose(Point(0,0,0), Rotate(0,0,0));

	// Load map
	PointCloudT::Ptr mapCloud(new PointCloudT);
  	pcl::io::loadPCDFile("map.pcd", *mapCloud);
  	cout << "Loaded " << mapCloud->points.size() << " data points from map.pcd" << endl;
	renderPointCloud(viewer, mapCloud, "map", Color(0,0,1)); 

	typename pcl::PointCloud<PointT>::Ptr cloudFiltered (new pcl::PointCloud<PointT>);
	typename pcl::PointCloud<PointT>::Ptr scanCloud (new pcl::PointCloud<PointT>);

	lidar->Listen([&new_scan, &lastScanTime, &scanCloud](auto data){
		if(new_scan){
			auto scan = boost::static_pointer_cast<csd::LidarMeasurement>(data);
			for (auto detection : *scan){
				if((detection.point.x*detection.point.x + detection.point.y*detection.point.y + detection.point.z*detection.point.z) > 8.0){ // Don't include points touching ego
					pclCloud.points.push_back(PointT(detection.point.x, detection.point.y, detection.point.z));
				}
			}
			if(pclCloud.points.size() > 5000){ // CANDO: Can modify this value to get different scan resolutions
				lastScanTime = std::chrono::system_clock::now();
				*scanCloud = pclCloud;
				new_scan = false;
			}
		}
	});
	
	Pose poseRef(Point(vehicle->GetTransform().location.x, vehicle->GetTransform().location.y, vehicle->GetTransform().location.z), Rotate(vehicle->GetTransform().rotation.yaw * pi/180, vehicle->GetTransform().rotation.pitch * pi/180, vehicle->GetTransform().rotation.roll * pi/180));
	double maxError = 0;

	while (!viewer->wasStopped())
  	{
		while(new_scan){
			std::this_thread::sleep_for(0.1s);
			world.Tick(1s);
		}
		if(refresh_view){
			viewer->setCameraPosition(pose.position.x, pose.position.y, 60, pose.position.x+1, pose.position.y+1, 0, 0, 0, 1);
			refresh_view = false;
		}
		
		viewer->removeShape("box0");
		viewer->removeShape("boxFill0");
		Pose truePose = Pose(Point(vehicle->GetTransform().location.x, vehicle->GetTransform().location.y, vehicle->GetTransform().location.z), Rotate(vehicle->GetTransform().rotation.yaw * pi/180, vehicle->GetTransform().rotation.pitch * pi/180, vehicle->GetTransform().rotation.roll * pi/180)) - poseRef;
		drawCar(truePose, 0,  Color(1,0,0), 0.7, viewer);
		double theta = truePose.rotation.yaw;
		double stheta = control.steer * pi/4 + theta;
		viewer->removeShape("steer");
		renderRay(viewer, Point(truePose.position.x+2*cos(theta), truePose.position.y+2*sin(theta),truePose.position.z),  Point(truePose.position.x+4*cos(stheta), truePose.position.y+4*sin(stheta),truePose.position.z), "steer", Color(0,1,0));

		ControlState accuate(0, 0, 1);
		if(cs.size() > 0){
			accuate = cs.back();
			cs.clear();

			Accuate(accuate, control);
			vehicle->ApplyControl(control);
		}

  		viewer->spinOnce ();
		
		if(!new_scan){			
			new_scan = true;
			// ------ Step1 ------
			// TODO: (Filter scan using voxel filter) 
          	// reference: Solution: ICP Alignment
			pcl::VoxelGrid<PointT> vg;
			vg.setInputCloud(scanCloud);
			// 设置体素滤波器的分辨率
			double filterResolution = 0.5;// in the tutorial, is 0.5
			vg.setLeafSize(filterResolution, filterResolution, filterResolution);
			// 定义 typename pcl::PointCloud<PointT>::Ptr 类型的指针变量 cloudFiltered，用于保存滤波后的点云数据。
			typename pcl::PointCloud<PointT>::Ptr cloudFiltered (new pcl::PointCloud<PointT>);
			vg.filter(*cloudFiltered);

			// ------ Step2 ------
			// TODO: Find pose transform by using ICP or NDT matching
			// 创建了一个名为ndt的正态分布变换对象。
			pcl::NormalDistributionsTransform<pcl::PointXYZ, pcl::PointXYZ> ndt;
			
			// 设置变换的最小误差为0.001。误差小于0.001时，算法将停止迭代，返回最终的匹配结果。
			ndt.setTransformationEpsilon (.0001);// offical ndt.setTransformationEpsilon (.0001);
          	ndt.setStepSize (1);//offical 1
			ndt.setResolution (1);//分辨率, offical ndt.setResolution (1);
			ndt.setInputTarget (mapCloud);
			
			/*
			参数: 将ndt对象、过滤后的点云数据、当前的位姿pose和最大迭代次数(90)作为参数传递给它。
			作用: 使用NDT算法进行点云匹配，并返回一个变换矩阵transform。
			NDT算法是一种基于高斯分布的点云匹配算法，它可以在不同的场景中实现高精度的匹配效果。
			在匹配过程中，NDT算法会根据两个点云之间的差异来调整变换矩阵，从而实现点云的精确匹配。
			最大迭代次数的设置可以影响匹配的精度和速度，一般来说，迭代次数越多，匹配的精度就越高，但是计算时间也会相应增加。
			*/
			Eigen::Matrix4d ndtTransform = NDT(ndt, cloudFiltered, pose, 90);			
	
			// 使用getPose函数将变换矩阵转换为一个位姿对象pose，以便后续使用。
			pose = getPose(ndtTransform);

			// ------ Step3 ------
			// TODO: Transform scan so it aligns with ego's actual pose and render that scan
			// 创建点云对象，用于存储变换后的点云数据。
			PointCloudT::Ptr transformed_scan(new PointCloudT);
			// 使用pcl库中的transformPointCloud函数将过滤后的点云数据*cloudFiltered进行变换，并将结果存储到transformed_scan中。
			pcl:transformPointCloud(*cloudFiltered, *transformed_scan, ndtTransform);

			// 使用pcl库中的removePointCloud函数将原始的点云数据从可视化窗口中删除，
			// 然后使用renderPointCloud函数将变换后的点云数据渲染出来，以便我们观察匹配的效果。
			viewer->removePointCloud("scan");
			
			// ------ Step4 ------
			// TODO: Change `scanCloud` below to your transformed scan
			renderPointCloud(viewer, transformed_scan, "scan", Color(1,0,0));
			// ------ End ------
          
			viewer->removeAllShapes();
			drawCar(pose, 1,  Color(0,1,0), 0.35, viewer);
          
          	double poseError = sqrt( (truePose.position.x - pose.position.x) * (truePose.position.x - pose.position.x) + (truePose.position.y - pose.position.y) * (truePose.position.y - pose.position.y) );
			if(poseError > maxError)
				maxError = poseError;
			double distDriven = sqrt( (truePose.position.x) * (truePose.position.x) + (truePose.position.y) * (truePose.position.y) );
			viewer->removeShape("maxE");
			viewer->addText("Max Error: "+to_string(maxError)+" m", 200, 100, 32, 1.0, 1.0, 1.0, "maxE",0);
			viewer->removeShape("derror");
			viewer->addText("Pose error: "+to_string(poseError)+" m", 200, 150, 32, 1.0, 1.0, 1.0, "derror",0);
			viewer->removeShape("dist");
			viewer->addText("Distance: "+to_string(distDriven)+" m", 200, 200, 32, 1.0, 1.0, 1.0, "dist",0);

			if(maxError > 1.2 || distDriven >= 170.0 ){
				viewer->removeShape("eval");
			if(maxError > 1.2){
				viewer->addText("Try Again", 200, 50, 32, 1.0, 0.0, 0.0, "eval",0);
			}
			else{
				viewer->addText("Passed!", 200, 50, 32, 0.0, 1.0, 0.0, "eval",0);
			}
		}

			pclCloud.points.clear();
		}
  	}
	return 0;
}