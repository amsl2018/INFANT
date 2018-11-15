#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>
#include <nav_msgs/OccupancyGrid.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/filters/passthrough.h>
// #include <pcl/filters/crop_box.h>
#include <pcl/features/normal_3d.h>
#include <tf/tf.h>

class OccupancyGridLidar{
	private:
		ros::NodeHandle nh;
		/*subscribe*/
		ros::Subscriber sub_rmground;
		ros::Subscriber sub_ground;
		/*publish*/
		ros::Publisher pub;
		/*cloud*/
		pcl::PointCloud<pcl::PointXYZI>::Ptr rmground {new pcl::PointCloud<pcl::PointXYZI>};
		pcl::PointCloud<pcl::PointXYZINormal>::Ptr ground {new pcl::PointCloud<pcl::PointXYZINormal>};
		/*grid*/
		nav_msgs::OccupancyGrid grid;
		nav_msgs::OccupancyGrid grid_all_minusone;
		/*publish infomations*/
		std::string pub_frameid;
		ros::Time pub_stamp;
		/*const values*/
		const double w = 20.0;	//x[m]
		const double h = 20.0;	//y[m]
		const double resolution = 0.2;	//[m]
		const double threshold_curvature = 3.0e-4;
		const double range_road_intensity[2] = {5, 15};
	public:
		OccupancyGridLidar();
		void GridInitialization(void);
		void CallbackRmGround(const sensor_msgs::PointCloud2ConstPtr& msg);
		void CallbackGround(const sensor_msgs::PointCloud2ConstPtr& msg);
		void ExtractPCInRange(pcl::PointCloud<pcl::PointXYZI>::Ptr pc);
		void NormalEstimation(void);
		void InputGrid(void);
		int MeterpointToIndex(double x, double y);
		void Publication(void);
};

OccupancyGridLidar::OccupancyGridLidar()
{
	sub_rmground = nh.subscribe("/rm_ground2/renamed_frame/transformed", 1, &OccupancyGridLidar::CallbackRmGround, this);
	sub_ground = nh.subscribe("/ground2/renamed_frame/transformed", 1, &OccupancyGridLidar::CallbackGround, this);
	pub = nh.advertise<nav_msgs::OccupancyGrid>("/occupancygrid/lidar", 1);
	GridInitialization();
}

void OccupancyGridLidar::GridInitialization(void)
{
	grid.info.resolution = resolution;
	grid.info.width = w/resolution + 1;
	grid.info.height = h/resolution + 1;
	grid.info.origin.position.x = -w/2.0;
	grid.info.origin.position.y = -h/2.0;
	grid.info.origin.position.z = 0.0;
	grid.info.origin.orientation.x = 0.0;
	grid.info.origin.orientation.y = 0.0;
	grid.info.origin.orientation.z = 0.0;
	grid.info.origin.orientation.w = 1.0;
	for(int i=0;i<grid.info.width*grid.info.height;i++)	grid.data.push_back(-1);
	// frame_id is same as the one of subscribed pc
	
	grid_all_minusone = grid;
}

void OccupancyGridLidar::CallbackRmGround(const sensor_msgs::PointCloud2ConstPtr &msg)
{
	pcl::fromROSMsg(*msg, *rmground);

	ExtractPCInRange(rmground);

	pub_frameid = msg->header.frame_id;
	pub_stamp = msg->header.stamp;

	InputGrid();
	Publication();
}

void OccupancyGridLidar::CallbackGround(const sensor_msgs::PointCloud2ConstPtr &msg)
{
	pcl::PointCloud<pcl::PointXYZI>::Ptr tmp_pc {new pcl::PointCloud<pcl::PointXYZI>};
	pcl::fromROSMsg(*msg, *tmp_pc);
	
	ExtractPCInRange(tmp_pc);
	
	pcl::copyPointCloud(*tmp_pc, *ground);

	NormalEstimation();
}

void OccupancyGridLidar::ExtractPCInRange(pcl::PointCloud<pcl::PointXYZI>::Ptr pc)
{
	pcl::PassThrough<pcl::PointXYZI> pass;
	pass.setInputCloud(pc);
	pass.setFilterFieldName("x");
	pass.setFilterLimits(-w/2.0, w/2.0);
	pass.filter(*pc);
	pass.setInputCloud(pc);
	pass.setFilterFieldName("y");
	pass.setFilterLimits(-h/2.0, h/2.0);
	pass.filter(*pc);
}

void OccupancyGridLidar::NormalEstimation(void)
{
	pcl::NormalEstimation<pcl::PointXYZINormal, pcl::PointXYZINormal> ne;
	ne.setInputCloud (ground);
	pcl::search::KdTree<pcl::PointXYZINormal>::Ptr tree (new pcl::search::KdTree<pcl::PointXYZINormal> ());
	ne.setSearchMethod(tree);
	ne.setRadiusSearch(0.1);
	ne.compute(*ground);
}

void OccupancyGridLidar::InputGrid(void)
{
	grid = grid_all_minusone;

	for(size_t i=0;i<ground->points.size();i++){
		// std::cout << "ground->points[i].curvature = " << ground->points[i].curvature << std::endl;
		// std::cout << "ground->points[i].intensity = " << ground->points[i].intensity << std::endl;
		if(ground->points[i].curvature>threshold_curvature)	grid.data[MeterpointToIndex(ground->points[i].x, ground->points[i].y)] = 50;
		else if(ground->points[i].intensity<range_road_intensity[0] || ground->points[i].intensity>range_road_intensity[1])	grid.data[MeterpointToIndex(ground->points[i].x, ground->points[i].y)] = 50;
		else	grid.data[MeterpointToIndex(ground->points[i].x, ground->points[i].y)] = 0;
	}
	for(size_t i=0;i<rmground->points.size();i++){
		grid.data[MeterpointToIndex(rmground->points[i].x, rmground->points[i].y)] = 100;
	}
}

int OccupancyGridLidar::MeterpointToIndex(double x, double y)
{
	int x_ = x/grid.info.resolution + grid.info.width/2.0;
	int y_ = y/grid.info.resolution + grid.info.height/2.0;
	int index = y_*grid.info.width + x_;
	return index;
}

void OccupancyGridLidar::Publication(void)
{
	grid.header.frame_id = pub_frameid;
	grid.header.stamp = pub_stamp;
	pub.publish(grid);
}

int main(int argc, char** argv)
{
    ros::init(argc, argv, "occupancygrid_lidar");
	std::cout << "= occupancygrid_lidar =" << std::endl;
	
	OccupancyGridLidar occupancygrid_lidar;

	ros::spin();
}
