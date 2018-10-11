#include "ros/ros.h"
#include <trajectory_generation/TrajectoryGeneration.h>
#include <trajectory_generation/Velocity.h>
#include <trajectory_generation/VelocityArray.h>
#include <tf/transform_listener.h>
#include <infant_planning/Velocity.h>
#include <geometry_msgs/PoseStamped.h>
#include <sensor_msgs/Joy.h>
#include <std_msgs/Float32.h>
#include <std_msgs/Bool.h>
#include <std_msgs/Int32.h>
#include <std_msgs/Int16.h>

#include <knm_tiny_msgs/Velocity.h>

#define HALF_PI M_PI*0.5
#define STOP 0
#define TURN 2
#define STOP_TIME 2*10 //wait sec * loop rate

using namespace std;

const string header_frame("/localmap");
const string robot_frame("/velodyne");
const float Vmax=0.9;
const float ANGULARmax=1.0; 
const float ANGULARmin=0.1;
const int MaxStop=40; //4[s]
const float Max_ndist=3.0;
float THRESH_ANG=0.25*M_PI;//HALF_PI

ros::Publisher vel_pub;
int time_count=0;
bool v_a_flag=false, cheat_flag=false, joy_flag=false, t_wp_flag=false;
infant_planning::Velocity cheat_vel;
boost::mutex v_array_mutex_;
trajectory_generation::VelocityArray g_v_array; 
geometry_msgs::PoseStamped t_wp;
int mode=0;
float n_dist=0;

float calcRelativeAngle(geometry_msgs::PoseStamped& wp, geometry_msgs::PoseStamped& robo)
{
	float dx=wp.pose.position.x-robo.pose.position.x;
	float dy=wp.pose.position.x-robo.pose.position.y;
	float wp_ang;
	if (fabs(dx)<=0.01 && dy>=0) wp_ang = HALF_PI; //vertical
	else if (fabs(dx)<=0.01 && dy<0) wp_ang = -HALF_PI;
	else{
		wp_ang=atan(dy/dx);
		if (dx<0 && dy>0) wp_ang += M_PI;
		else if (dx<0 && dy<0) wp_ang += -M_PI;
	} 

	//cout<<"wp-robo:"<<wp_ang*180.0/M_PI<<endl;
	//cout<<"relative:"<<(wp_ang-robo.pose.orientation.z)*180.0/M_PI<<endl;
	return wp_ang-robo.pose.orientation.z;
}

void setStopCommand(infant_planning::Velocity& cmd_vel)
{
	cmd_vel.op_linear = 0;
	cmd_vel.op_angular = 0;
}

void setTurnCommand(infant_planning::Velocity& cmd_vel,
					geometry_msgs::PoseStamped& wp,
					geometry_msgs::PoseStamped& robo)
{
	float turn_angular = 0.3;
//	float turn_angular = 0.2;
	float turn_dir = 1;
	if (calcRelativeAngle(wp, robo)<0) turn_dir=-1;
	cmd_vel.op_linear = 0;
	cmd_vel.op_angular = turn_dir * turn_angular;
	cout<<"now turn"<<endl;
}

void commandDecision(	trajectory_generation::VelocityArray& v_a,
						infant_planning::Velocity& cmd_vel)
{
	// float hoge = 0.6*10.0/0.4;
	float hoge = 15;
	int ind = hoge+time_count;
	if((int)v_a.vel.size() > ind){
		cmd_vel.header.stamp = ros::Time::now();
		cmd_vel.op_linear = v_a.vel[ind].op_linear;
		cmd_vel.op_angular = -v_a.vel[ind].op_angular;
	}else{
		cmd_vel.op_linear = 0;
		cmd_vel.op_angular = 0;
	}
	//cout<<"lin1 = "<<cmd_vel.op_linear<<"\tang1 = "<<cmd_vel.op_angular<<endl;
}

inline bool checkTF(tf::StampedTransform& transform, geometry_msgs::PoseStamped& robo, tf::TransformListener& listener)
{
	try{
		//listener.waitForTransform(header_frame, robot_frame, ros::Time(0), ros::Duration(1.0));
	    listener.lookupTransform(header_frame, robot_frame, ros::Time(0), transform);
        robo.pose.position.x=transform.getOrigin().x();
        robo.pose.position.y=transform.getOrigin().y();
        robo.pose.position.z=0;
        robo.pose.orientation.z = tf::getYaw(transform.getRotation());
        // float angle = tf::getYaw(transform.getRotation());
        // robo.pose.orientation = tf::createQuaternionMsgFromRollPitchYaw(0,0,angle);
		return true;
	}
	catch (tf::TransformException ex){
		cout<<"waiting for global and robot frame relation"<<endl;
		return false;
	}
}

void JoyCallback(const sensor_msgs::JoyConstPtr& msg){
	joy_flag = true;
}

void CheatCallback(const knm_tiny_msgs::VelocityPtr& msg){
	cheat_vel.op_linear = msg->op_linear;
	cheat_vel.op_angular = msg->op_angular;
	cheat_flag = true;
}

void NormalDistCallback(const std_msgs::Float32ConstPtr& msg)
{
	n_dist=msg->data;
}

void TargetWpCallback(const geometry_msgs::PoseStampedConstPtr& msg)
{
	t_wp=*msg;
	t_wp_flag=true;
}

void vArrayCallback(const trajectory_generation::VelocityArrayConstPtr& msg)
{
	boost::mutex::scoped_lock(v_array_mutex_);
	g_v_array=*msg;
	v_a_flag=true;
}

void ModeCallback(const std_msgs::Int16ConstPtr& msg)
{
  mode = msg->data;

}

void MotionPlanner()
{
	ros::NodeHandle n;
	ros::Subscriber cntl_sub       = n.subscribe("/joy", 10, JoyCallback);
	ros::Subscriber cheat_sub       = n.subscribe("/tinypower/cheat_velocity", 10, CheatCallback);
	ros::Subscriber v_array_sub    = n.subscribe("/plan/velocity_array", 10, vArrayCallback);
	ros::Subscriber twp_sub        = n.subscribe("/target/pose", 10, TargetWpCallback);
	ros::Subscriber norm_dist_sub  = n.subscribe("/waypoint/normal_dist", 10, NormalDistCallback);
	
	ros::Subscriber mode_sub = n.subscribe("/mode",1,ModeCallback);

	vel_pub = n.advertise<infant_planning::Velocity>("tinypower/command_velocity", 10);
	
	bool turn_flag = false;
	int stop_count=0;
	// float relative_ang=0;
	infant_planning::Velocity cmd_vel;
    tf::StampedTransform transform;
    tf::TransformListener listener;
	geometry_msgs::PoseStamped robo;

	ros::Rate loop_rate(10);
	while(ros::ok()){
		//get robot position
		bool tf_flag = checkTF(transform, robo, listener);
		if (t_wp_flag && tf_flag && !cheat_flag){ //normal state
			trajectory_generation::VelocityArray v_array; 
			if (mode != 1){
				if (v_a_flag){
					boost::mutex::scoped_lock(v_array_mutex_);
					v_array = g_v_array;
				}
				if (v_array.vel.size()){// path is generated
					cout<<"run"<<endl;
				}
				else { // path is not generated
					if (stop_count>MaxStop){
						turn_flag=true;
						cout<<"stop_count_turn"<<endl;
					}
					else{
						cout<<"no path stop"<<endl;
					}
					stop_count++;
				}
			}
		
			// set velocity and angular //
			if (mode == 3){
				cout<<"stop"<<endl;
				setStopCommand(cmd_vel);
			}
			else if (mode == 1 || turn_flag){
				cout<<"turn"<<endl;
				setTurnCommand(cmd_vel, t_wp, robo);
				stop_count=0;
				turn_flag = false;
			}
			else if (mode == 0){ 
				commandDecision(v_array, cmd_vel);
				stop_count=0;
				time_count=0;
			}
			else {
				cout<<"unknown"<<endl;
				setStopCommand(cmd_vel);
			}
			
			// safety //
			if (cmd_vel.op_linear>Vmax) cmd_vel.op_linear=Vmax;
			if (cmd_vel.op_angular>ANGULARmax) cmd_vel.op_angular=ANGULARmax;
			else if (cmd_vel.op_angular<-ANGULARmax) cmd_vel.op_angular=-ANGULARmax;
			
			// publish //
			vel_pub.publish(cmd_vel);
			
			cout<<"mode: "<<mode<<endl;
			cout<<"lin = "<<cmd_vel.op_linear<<"\tang = "<<cmd_vel.op_angular<<endl<<endl;
			
			//v_a_flag=false;
			//t_wp_flag=false;
			tf_flag=false;
		}
		else if (cheat_flag){
			vel_pub.publish(cheat_vel);
			cheat_flag=false;
		}
		else { //waiting for callback
			cout<<"-----------"<<endl;
			cout<<"v_arr:"<<v_a_flag<<endl;
			cout<<"t_wp:"<<t_wp_flag<<endl;
			cout<<"tf_flag:"<<tf_flag<<endl;
			cout<<"cheat_flag:"<<cheat_flag<<endl;
			cout<<"-----------"<<endl;
			cmd_vel.op_linear=0;
			cmd_vel.op_angular=0;
			vel_pub.publish(cmd_vel);
		}
		time_count++;
		
		ros::spinOnce();
		loop_rate.sleep();
	}
}

int main(int argc, char **argv)
{
	ros::init(argc, argv, "motion_decision");
	MotionPlanner();
	
	ROS_INFO("Killing now!!!!!");
	return 0;
}
