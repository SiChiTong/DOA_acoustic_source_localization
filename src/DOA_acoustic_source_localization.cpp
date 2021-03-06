/*
 *  Copyright (c) 2015, Riccardo Levorato <riccardo.levorato@dei.unipd.it>
 *
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *     1. Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *     2. Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 *     3. Neither the name of the copyright holder(s) nor the
 *        names of its contributors may be used to endorse or promote products
 *        derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 *  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 *  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <ros/ros.h>
#include <ros/package.h>
#include <tf/transform_listener.h>
#include <tf/transform_broadcaster.h>
#include <visualization_msgs/Marker.h>
#include "tf_conversions/tf_eigen.h"

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <Eigen/StdVector>
#include <eigen_conversions/eigen_msg.h>

#include <hark_msgs/HarkSource.h>

#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <math.h>
#include <limits>

using namespace Eigen;
using namespace std;

// Association of the number of the hw audio: For now it is progressive and it depends by the order of the usb-plugin of the kinect. So for now plug in first kinect 1, after kinect 2, etc..
//TODO insert weights for gaussian
//TODO insert gaussian formula

vector<vector<hark_msgs::HarkSourceVal> > DOAs_hark;
vector<Eigen::Affine3d, Eigen::aligned_allocator<Eigen::Affine3d> > sensors_3D_pose;

double get_absolute_DOA(Eigen::Affine3d sensor_3D_pose, vector<hark_msgs::HarkSourceVal> DOA_hark)
{
	double relative_DOA = 0, max_power = -1;
	int num_sources_detected = DOA_hark.size();
	for (int i = 0; i < num_sources_detected; i++)
	{
		if (DOA_hark[i].power > max_power) // use the DOA detection with higher power
		{
			max_power = DOA_hark[i].power;
			relative_DOA = (DOA_hark[i].azimuth / 180.0) * M_PI; // hark returns angles in degrees
		}
	}
	double sensor_yaw = atan2(sensor_3D_pose.rotation().matrix()(1,0), sensor_3D_pose.rotation().matrix()(0,0));
	return relative_DOA + sensor_yaw;
}

void harkCallback(const hark_msgs::HarkSource::ConstPtr & msg, int i)
{
	DOAs_hark[i] = msg->src;
	/*ROS_INFO("DOAs_hark from sensor: %d", msg->exist_src_num);
	for (unsigned j = 0; j < DOAs_hark[i].size(); j++)
	{
		ROS_INFO("\t%d DOAs_hark from sensor: %d - azimuth: %f, power: %f", i, DOAs_hark[i][j].azimuth/180.0 * M_PI, DOAs_hark[i][j].power);
	}*/
}

Eigen::Vector3d locate_WLS_2D(vector<bool> detected_DOA)
{

	int n_sources = 0;
	for (unsigned int i = 0; i < detected_DOA.size(); i++)
	{
		if (detected_DOA[i])
		{
			n_sources++;
		}
	}

	int dim = 2;
	// TODO insert the possibility to set different weights
	MatrixXd w = MatrixXd::Ones(n_sources, 1);
	MatrixXd W = MatrixXd::Identity(n_sources, n_sources);
	MatrixXd K = MatrixXd::Zero(2, n_sources);
	for (unsigned int k = 0; k < detected_DOA.size(); k++)
	{
		if (detected_DOA[k])
		{
			double relative_DOA = get_absolute_DOA(sensors_3D_pose[k], DOAs_hark[k]);
			K.col(k) << cos(relative_DOA), sin(relative_DOA);
		}
	}
	MatrixXd I = MatrixXd::Identity(dim, dim);
	MatrixXd A = MatrixXd::Zero(dim, n_sources);
	for (int i = 0; i < n_sources; i++)
	{
		A.col(i) = (I - K.col(i) * K.col(i).adjoint()) * sensors_3D_pose[i].translation().head(2);
	}
	Eigen::Vector2d solution_2D = (w.sum() * I - K * K.adjoint()).lu().solve(A * w);
	Eigen::Vector3d solution_3D;
	solution_3D << solution_2D, 0;
	return solution_3D;
}

double evaluate_angle_from_2D_grid(double x, double y)
{
	double value = 0;
	for (unsigned int i = 0; i < sensors_3D_pose.size(); i++)
	{
		double absolute_DOA = get_absolute_DOA(sensors_3D_pose[i], DOAs_hark[i]);
		double angle_point2sensor = atan2(y - sensors_3D_pose[i].translation()(1), x - sensors_3D_pose[i].translation()(0));
		double angle = angle_point2sensor - absolute_DOA;
			
		double range = M_PI;
		while (angle > range)
		{
			angle = angle - 2*M_PI;
		}
		while (angle < -range)
		{		
			angle = angle + 2*M_PI;
		}
		value += angle * angle;
	}
	return value;
}

Eigen::Vector3d locate_min_angle_2D_slow(double start_x, double start_y, double range, double precision_grid)
{
	double x_sol = 0, y_sol = 0;
	double min_val = std::numeric_limits<double>::max();
	for (double x = start_x - range/2; x <= start_x + range/2; x += precision_grid)
	{
		for (double y = start_y - range/2; y <= start_y + range/2; y += precision_grid)
		{
			double tmp_val = evaluate_angle_from_2D_grid(x, y);
			if (tmp_val < min_val)
			{
				min_val = tmp_val;
				x_sol = x;
				y_sol = y;
			}
		}
	}
	Eigen::Vector3d solution;
	solution << x_sol, y_sol, 0;
	return solution;
}

void set_max_rec(int n_coordinate, double x_tmp_value, Eigen::Vector3d value_indexes, double *max_value, Eigen::Vector3d *max_value_indexes, int *x_max_index)
{
	if (x_tmp_value > *max_value)
	{
		*max_value = x_tmp_value;
		*max_value_indexes = value_indexes;
		*x_max_index = value_indexes[n_coordinate];
	}

	if (x_tmp_value == *max_value && value_indexes[n_coordinate] < *x_max_index)
	{
		*x_max_index = value_indexes[n_coordinate];
	}
}

Eigen::Vector3d locate_min_angle_2D_fast_rec(int num_var, int n_max_coordinates, Eigen::Vector3d previous_indexes, double start_x, double start_y, double *max_value, double range, double precision_grid)
{
	Eigen::Vector3d max_value_indexes = previous_indexes;
	int n_points = range/precision_grid;
	int levels = ceil(log2(n_points));

	if (n_max_coordinates == 0)
	{
		double i = start_x + (- pow(2, levels-1) + previous_indexes[0]) * precision_grid;
		double j = start_y + (- pow(2, levels-1) + previous_indexes[1]) * precision_grid;
		*max_value = - evaluate_angle_from_2D_grid(i, j);
	} else
	{
		*max_value = -std::numeric_limits<double>::max();

		int x_tmp_index = pow(2, levels-1);
		int x_max_index = std::numeric_limits<int>::max();

		int l = (levels-1);
		while (l >= 0)
		{
			previous_indexes[num_var - n_max_coordinates] = x_tmp_index;
			// update value of pivot index
			double x_tmp_value;
			Eigen::Vector3d value_indexes = locate_min_angle_2D_fast_rec(num_var, n_max_coordinates - 1, previous_indexes, start_x, start_y, &x_tmp_value, range, precision_grid);
			// update max value
			set_max_rec(num_var - n_max_coordinates, x_tmp_value, value_indexes, max_value, &max_value_indexes, &x_max_index);
			if (l > 0)
			{
				if (l < levels -1 && x_tmp_value < *max_value)// jump immedialtely if the tmp_value is less than the maximum value. If equal (--^--) continue the research of a different value.
				{
					if (x_tmp_index < max_value_indexes[num_var - n_max_coordinates])
					{
						x_tmp_index = x_tmp_index + pow(2, l-1);
					} else
					{
						x_tmp_index = x_tmp_index - pow(2, l-1);
					}
				} else
				{
					// update value and index of back pivot
					double x_tmp_back_value = x_tmp_value;
					double x_tmp_back_index = x_tmp_index;
					while (((x_tmp_index - x_tmp_back_index) < pow(2, l)-1) && (x_tmp_back_value == x_tmp_value))
					{
						x_tmp_back_index = x_tmp_back_index-1;
						previous_indexes[num_var - n_max_coordinates] = x_tmp_back_index;
						value_indexes = locate_min_angle_2D_fast_rec(num_var, n_max_coordinates - 1, previous_indexes, start_x, start_y, &x_tmp_back_value, range, precision_grid);
					}
					// update max value
					set_max_rec(num_var - n_max_coordinates, x_tmp_back_value, value_indexes, max_value, &max_value_indexes, &x_max_index);
					// jump
					if (x_tmp_back_value > x_tmp_value)// if back value is higher, jump back
					{
						x_tmp_index = x_tmp_index - pow(2, l-1);
						while (x_tmp_index > x_tmp_back_index)
						{
							l = l - 1;
							x_tmp_index = x_tmp_index - pow(2, l-1);
						}
					}else// if back value is less or equal (after having arrived to the previous level), jump next
					{
						if (x_tmp_index == pow(2, levels) - 1)// look forward to reach 2^(levels) element
						{
							x_tmp_index = x_tmp_index+1;
							previous_indexes[num_var - n_max_coordinates] = x_tmp_index;
							Eigen::Vector3d value_indexes = locate_min_angle_2D_fast_rec(num_var, n_max_coordinates - 1, previous_indexes, start_x, start_y, &x_tmp_value, range, precision_grid);
							// update max value
							set_max_rec(num_var - n_max_coordinates, x_tmp_value, value_indexes, max_value, &max_value_indexes, &x_max_index);
						}else
						{
							x_tmp_index = x_tmp_index + pow(2, l-1);
						}
					}
				}
			}
			l = l - 1;
		}
		max_value_indexes[num_var - n_max_coordinates] = x_max_index;
	}
	return max_value_indexes;
}

Eigen::Vector3d locate_min_angle_2D_fast(double start_x, double start_y, double range, double precision_grid)
{
	double max_value;
	Eigen::Vector3d previous_indexes;
	previous_indexes << 0, 0, 0;

	Eigen::Vector3d solution = locate_min_angle_2D_fast_rec(2, 2, previous_indexes, start_x, start_y, &max_value, range, precision_grid);

	int n_points = range/precision_grid; 
	int levels = ceil(log2(n_points));

	solution[0] = start_x + (- pow(2,levels-1) + solution[0]) * precision_grid;
	solution[1] = start_y + (- pow(2,levels-1) + solution[1]) * precision_grid;
	solution[2] = 0;

	return solution;
}

void update_sensors_pose(string tf_world_name, vector<string> sensors_3D_pose_topics){
	tf::TransformListener listener;
	for (unsigned int i = 0; i < sensors_3D_pose.size(); i++)
	{
		bool found = false;
		tf::StampedTransform tmp_transform;
		while (!found)
		{
			try
			{
				ros::Time now = ros::Time(0);
				listener.waitForTransform(tf_world_name, sensors_3D_pose_topics[i], now, ros::Duration(0.5));
				listener.lookupTransform(tf_world_name, sensors_3D_pose_topics[i], ros::Time(0), tmp_transform);
				found = true;
			} catch (tf::TransformException ex)
			{
				ROS_ERROR("%s", ex.what());
			}
		}
		Eigen::Affine3d tmp_eigen_transform;
		tf::transformTFToEigen(tmp_transform, tmp_eigen_transform);
		sensors_3D_pose[i] = tmp_eigen_transform;
	}
}

int main(int argc, char **argv)
{
	ros::init(argc, argv, "DOA_acoustic_source_localization");
	ros::NodeHandle nh;
	ros::NodeHandle input_nh("~");
	ros::Rate lr(10);

	bool is_simulation;
	input_nh.getParam("is_simulation", is_simulation);
	cout << "is_simulation: " << is_simulation << endl;

	string data_simulation_file_path;
	input_nh.getParam("data_simulation_file_path", data_simulation_file_path);
	if (is_simulation){
		cout << "data_simulation_file_path: " << data_simulation_file_path << endl;
	}

	int n_acoustic_DOA_sensors;
	input_nh.getParam("n_acoustic_DOA_sensors", n_acoustic_DOA_sensors);
	cout << "n_acoustic_DOA_sensors: " << n_acoustic_DOA_sensors << endl;

	double audio_signal_power_threshold = 0;
	input_nh.getParam("audio_signal_power_threshold", audio_signal_power_threshold);
	cout << "audio_signal_power_threshold: " << audio_signal_power_threshold << endl;

	int algorithm_type;
	input_nh.getParam("algorithm_type", algorithm_type);
	cout << "algorithm_type: " << algorithm_type << endl;

	double range = 10;
	input_nh.getParam("range", range);
	cout << "range: " << range << endl;

	double precision_grid = 0.01;
	input_nh.getParam("precision_grid", precision_grid);
	cout << "precision_grid: " << precision_grid << endl;

	string tf_world_name;
	input_nh.getParam("tf_world_name", tf_world_name);
	cout << "tf_world_name: " << tf_world_name << endl;

	string tf_solution_name;
	input_nh.getParam("tf_solution_name", tf_solution_name);
	cout << "tf_solution_name: " << tf_solution_name << endl;

	double rviz_DOA_line_lenght = 20;
	input_nh.getParam("rviz_DOA_line_lenght", rviz_DOA_line_lenght);
	cout << "rviz_DOA_line_lenght: " << rviz_DOA_line_lenght << endl;

	DOAs_hark.resize(n_acoustic_DOA_sensors);
	sensors_3D_pose.resize(n_acoustic_DOA_sensors);
	vector<string> sensors_3D_pose_topics(n_acoustic_DOA_sensors);
	vector<string> DOA_topics(n_acoustic_DOA_sensors);
	vector<ros::Subscriber> sub(n_acoustic_DOA_sensors);

	int start_numbering_labels = 1;
	for (int i = 0; i < n_acoustic_DOA_sensors; i++)
	{
		std::ostringstream oss;
		oss << i + start_numbering_labels;
		input_nh.getParam("sensor_3D_pose_topic_" + oss.str(), sensors_3D_pose_topics[i]);
		input_nh.getParam("DOA_topic_" + oss.str(), DOA_topics[i]);
		if (!is_simulation)
		{
			sub[i] = nh.subscribe<hark_msgs::HarkSource>(DOA_topics[i], 100, boost::bind(harkCallback, _1, i));
		}
	}

	ros::Publisher visualization_marker_pub = nh.advertise<visualization_msgs::Marker>( "visualization_marker", 10);

	while (ros::ok())
	{
		ros::spinOnce();

		// Points visualization
		visualization_msgs::Marker solution_points, line_strip;
		solution_points.header.frame_id = line_strip.header.frame_id = tf_world_name;
		solution_points.header.stamp = line_strip.header.stamp = ros::Time(0);
		line_strip.ns = "points_and_lines"; solution_points.ns = "solutions";
		solution_points.action = line_strip.action = visualization_msgs::Marker::ADD;
		solution_points.pose.orientation.w = line_strip.pose.orientation.w = 1.0;
		solution_points.id = 0;
		solution_points.type = visualization_msgs::Marker::POINTS;

		// Points markers use x and y scale for width/height respectively
		solution_points.scale.x = 0.05;
		solution_points.scale.y = 0.05;

		// Solution points are red
		solution_points.color.r = 1.0f;
		solution_points.color.a = 1.0;

		line_strip.id = 1;
		line_strip.type = visualization_msgs::Marker::LINE_LIST;

		// Line_strip markers use only the x component of scale, for the line width
		line_strip.scale.x = 0.01;
		line_strip.scale.y = 0.01;

		// Line strip is blue
		line_strip.color.b = 1.0;
		line_strip.color.a = 1.0;

		if (is_simulation)
		{
			audio_signal_power_threshold = -1;
			std::ifstream infile(data_simulation_file_path.c_str());
			std::string line;
			vector<double> simulation_angles;
			while (std::getline(infile, line))
			{
				std::istringstream iss(line);
				double a;
				if (!(iss >> a)){break; } // error
				simulation_angles.push_back(a);
				// cout << "angle: " << a << endl;
			}
			// cout << endl;

			DOAs_hark.clear();
			for (int i = 0; i < n_acoustic_DOA_sensors; i++)
			{
				vector<hark_msgs::HarkSourceVal> vec;
				hark_msgs::HarkSourceVal tmp_hark;
				tmp_hark.azimuth = simulation_angles[i];
				vec.push_back(tmp_hark);
				DOAs_hark.push_back(vec);	
			}
		}

		update_sensors_pose(tf_world_name, sensors_3D_pose_topics);

		// Plotting lines
		for (int k = 0; k < n_acoustic_DOA_sensors; k++)
		{
			double absolute_DOA = get_absolute_DOA(sensors_3D_pose[k], DOAs_hark[k]);
				Vector3d prt = sensors_3D_pose[k].translation();
				Vector3d p1, p2;
				p1 << prt[0] + rviz_DOA_line_lenght * cos(absolute_DOA), prt[1] + rviz_DOA_line_lenght * sin(absolute_DOA), 0;
				p2 << prt;

				geometry_msgs::Point gp1, gp2;
				tf::pointEigenToMsg(p1, gp1);
				tf::pointEigenToMsg(p2, gp2);

				line_strip.points.push_back(gp1);
				line_strip.points.push_back(gp2);
		}

		vector<bool> detected_DOA(n_acoustic_DOA_sensors);

		// Control if there is at least one acoustic detected in each DOA sensor
		int n_detected = 0;
		for (int i = 0; i < n_acoustic_DOA_sensors; i++) {
			int num_sources_detected = DOAs_hark[i].size();
			detected_DOA[i] = false;
			if (num_sources_detected > 0){
				int j = 0;
				bool detected = false;
				while (!detected && j < num_sources_detected)
				{
					if (DOAs_hark[i][j].power > audio_signal_power_threshold)
					{
						detected = true;
						n_detected++;
					}
					j++;
				}
				if (detected)
				{
					detected_DOA[i] = true;	
				}
			}
		}

		Eigen::Vector3d solution, WLS_solution;
		geometry_msgs::Point gp;

		if (n_detected >= 2)
		{
			WLS_solution = locate_WLS_2D(detected_DOA);
			if (n_detected == 2)
			{
				solution = WLS_solution;	
			}
		} 
		if (n_detected >= 3)
		{
			switch (algorithm_type)
			{
				case 1: solution = locate_min_angle_2D_fast(WLS_solution[0], WLS_solution[1], range, precision_grid);
					break;
				case 2: solution = locate_min_angle_2D_slow(WLS_solution[0], WLS_solution[1], range, precision_grid);
					break;
				case 3: solution = WLS_solution;
					break;
			}
		}

		// Publish TF_solution
		tf::Transform transform;
		transform.setOrigin( tf::Vector3(solution(0), solution(1), 0.0) );
		tf::Quaternion q;
		q.setRPY(0, 0, 0);
		transform.setRotation(q);
		static tf::TransformBroadcaster br;
		br.sendTransform(tf::StampedTransform(transform, ros::Time(0),tf_world_name, tf_solution_name));

		// Visualize solution
		tf::pointEigenToMsg(solution, gp);
		solution_points.points.push_back(gp);

		visualization_marker_pub.publish(solution_points);
		visualization_marker_pub.publish(line_strip);

		lr.sleep();
	}
}
