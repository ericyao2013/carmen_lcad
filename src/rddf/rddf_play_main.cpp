#include <stdio.h>
#include <iostream>
#include <vector>
#include <string.h>
#include <algorithm>
#include <list>

using namespace std;

#include <carmen/carmen.h>
#include <carmen/readlog.h>
#include <carmen/carmen_stdio.h>
#include <carmen/velodyne_interface.h>
#include <carmen/fused_odometry_interface.h>
#include <carmen/carmen_gps_wrapper.h>
#include <carmen/traffic_light_interface.h>
#include <carmen/traffic_light_messages.h>
#include <carmen/collision_detection.h>
#include <carmen/moving_objects_messages.h>
#include <carmen/moving_objects_interface.h>
#include <carmen/road_mapper.h>
#include <carmen/grid_mapping.h>

#include "rddf_interface.h"
#include "rddf_messages.h"
#include "rddf_index.h"

#include <kml/base/file.h>
#include <kml/engine.h>
#include <kml/dom.h>
#include <kml/engine/kml_file.h>
#include "g2o/types/slam2d/se2.h"

#include <gsl/gsl_errno.h>
#include <gsl/gsl_spline.h>
#include <gsl/gsl_multimin.h>
#include <gsl/gsl_math.h>

using namespace g2o;

typedef std::vector<kmldom::PlacemarkPtr> placemark_vector_t;

#include "rddf_util.h"

static bool use_road_map = false;
static bool robot_pose_queued = false;
static carmen_localize_ackerman_globalpos_message *current_globalpos_msg = NULL;
static carmen_simulator_ackerman_truepos_message *current_truepos_msg = NULL;
static carmen_map_p current_road_map = NULL;

static char *carmen_rddf_filename = NULL;

static int carmen_rddf_perform_loop = 0;
static int carmen_rddf_num_poses_ahead_max = 100;
static double rddf_min_distance_between_waypoints = 0.5;
static double distance_between_front_and_rear_axles;
static double distance_between_front_car_and_front_wheels;
static int road_mapper_kernel_size = 7;
static bool debug = false;

static int carmen_rddf_end_point_is_set = 0;
static carmen_point_t carmen_rddf_end_point;

static int carmen_rddf_nearest_waypoint_is_set = 0;
static carmen_point_t carmen_rddf_nearest_waypoint_to_end_point;

static carmen_ackerman_traj_point_t *carmen_rddf_poses_ahead = NULL;
static carmen_ackerman_traj_point_t *carmen_rddf_poses_back = NULL;
static int carmen_rddf_num_poses_ahead = 0;
static int carmen_rddf_num_poses_back = 0;
static int *annotations_codes;
static int *annotations;

static int carmen_rddf_pose_initialized = 0;
static int already_reached_nearest_waypoint_to_end_point = 0;

char *carmen_annotation_filename = NULL;
vector<carmen_annotation_t> annotation_read_from_file;
typedef struct
{
	carmen_annotation_t annotation;
	size_t index;
} annotation_and_index;
vector<annotation_and_index> annotations_to_publish;
carmen_rddf_annotation_message annotation_queue_message;

static int use_truepos = 0;

static int traffic_lights_camera = 3;
carmen_traffic_light_message *traffic_lights = NULL;

deque<carmen_rddf_dynamic_annotation_message> dynamic_annotation_messages;

carmen_moving_objects_point_clouds_message *moving_objects = NULL;

bool simulated_pedestrian_on = false;


static void
carmen_rddf_play_shutdown_module(int signo)
{
	if (signo == SIGINT)
	{
		carmen_ipc_disconnect();
		exit(0);
	}
}


int
carmen_rddf_play_nearest_waypoint_reached(carmen_ackerman_traj_point_t pose)
{
	if (sqrt(pow(pose.x - carmen_rddf_nearest_waypoint_to_end_point.x, 2) + pow(pose.y - carmen_rddf_nearest_waypoint_to_end_point.y, 2)) < 2.0)
		return 1;
	else
		return 0;
}


int
carmen_rddf_play_find_position_of_nearest_waypoint(carmen_ackerman_traj_point_t *poses_ahead, int num_poses)
{
	int i, position = -1;

	for (i = 0; i < num_poses; i++)
	{
		if (carmen_rddf_play_nearest_waypoint_reached(poses_ahead[i]))
		{
			position = i;
			break;
		}
	}

	return position;
}


int
carmen_rddf_play_adjust_poses_ahead_and_add_end_point_to_list(carmen_ackerman_traj_point_t *poses_ahead, int num_poses, int nearest_end_waypoint_position, int *rddf_annotations)
{
	int position_nearest_waypoint = nearest_end_waypoint_position;
	int position_end_point = nearest_end_waypoint_position + 1;

	//
	// se o waypoint mais proximo ao end point for o ultimo da lista,
	// a sua posicao eh decrementada para abrir espaco para o end point
	//

	if (nearest_end_waypoint_position == (num_poses - 1))
	{
		position_nearest_waypoint--;
		position_end_point--;
	}

	poses_ahead[position_nearest_waypoint].x = carmen_rddf_nearest_waypoint_to_end_point.x;
	poses_ahead[position_nearest_waypoint].y = carmen_rddf_nearest_waypoint_to_end_point.y;
	rddf_annotations[position_nearest_waypoint] = RDDF_ANNOTATION_TYPE_NONE;

	poses_ahead[position_end_point].x = carmen_rddf_end_point.x;
	poses_ahead[position_end_point].y = carmen_rddf_end_point.y;
	poses_ahead[position_end_point].theta = carmen_rddf_end_point.theta;
	poses_ahead[position_end_point].v = 0.0;
	rddf_annotations[position_end_point] = RDDF_ANNOTATION_TYPE_END_POINT_AREA;

	return (position_end_point + 1);
}


int
carmen_rddf_play_check_if_end_point_is_reachable(carmen_ackerman_traj_point_t *poses_ahead, int num_poses, int *rddf_annotations)
{
	if (carmen_rddf_nearest_waypoint_is_set)
	{
		// se o robo ja passou pelo waypoint mais proximo do end point, so o end point eh publicado
		if (already_reached_nearest_waypoint_to_end_point)
		{
			poses_ahead[0].x = carmen_rddf_end_point.x;
			poses_ahead[0].y = carmen_rddf_end_point.y;
			poses_ahead[0].theta = carmen_rddf_end_point.theta;
			poses_ahead[0].v = 0.0;
			rddf_annotations[0] = RDDF_ANNOTATION_TYPE_END_POINT_AREA;

			return 1;
		}

		// verifica se algum dos waypoints esta a uma distancia minima do waypoint mais proximo do end point
		int nearest_end_waypoint_position = carmen_rddf_play_find_position_of_nearest_waypoint (poses_ahead, num_poses);

		if (nearest_end_waypoint_position != -1)
		{
			// se um dos dois primeiros waypoints esta a uma distancia minima do waypoint mais proximo do end point, passamos a publicar somente o end point
			if (nearest_end_waypoint_position < 2)
				already_reached_nearest_waypoint_to_end_point = 1;

			return carmen_rddf_play_adjust_poses_ahead_and_add_end_point_to_list(poses_ahead, num_poses, nearest_end_waypoint_position, rddf_annotations);
		}
		else
			return num_poses;
	}
	else
		return num_poses;
}


static void
clear_annotations(int *rddf_annotations, int num_annotations)
{
	int i;

	for(i = 0; i < num_annotations; i++)
	{
		rddf_annotations[i] = RDDF_ANNOTATION_TYPE_NONE;
		annotations_codes[i] = RDDF_ANNOTATION_CODE_NONE;
	}
}


void
clear_annotations()
{
	for (int i = 0; i < carmen_rddf_num_poses_ahead; i++)
	{
		annotations[i] = RDDF_ANNOTATION_TYPE_NONE;
		annotations_codes[i] = RDDF_ANNOTATION_CODE_NONE;
	}
}


static int
carmen_rddf_play_find_nearest_poses_ahead(double x, double y, double yaw, double timestamp /* only for debugging */, carmen_ackerman_traj_point_t *poses_ahead, carmen_ackerman_traj_point_t *poses_back, int *num_poses_back, int num_poses_ahead_max, int *rddf_annotations)
{
	clear_annotations(rddf_annotations, num_poses_ahead_max);

	int num_poses_ahead = carmen_search_next_poses_index(x, y, yaw, timestamp, poses_ahead, poses_back, num_poses_back, num_poses_ahead_max, rddf_annotations, carmen_rddf_perform_loop);
	return carmen_rddf_play_check_if_end_point_is_reachable(poses_ahead, num_poses_ahead, rddf_annotations);
}


int
get_key_non_blocking(void)
{
	struct termios oldt, newt;
	int ch;
	int oldf;

	tcgetattr(STDIN_FILENO, &oldt);
	newt = oldt;
	newt.c_lflag &= ~(ICANON | ECHO);
	tcsetattr(STDIN_FILENO, TCSANOW, &newt);
	oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
	fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

	ch = getchar();

	tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
	fcntl(STDIN_FILENO, F_SETFL, oldf);

	return (ch);
}


bool
pedestrian_track_busy(carmen_moving_objects_point_clouds_message *moving_objects, carmen_annotation_t pedestrain_track_annotation)
{
	int ch = get_key_non_blocking();

	if (ch == 'p')
		simulated_pedestrian_on = true;
	if (ch == ' ')
		simulated_pedestrian_on = false;

	if (simulated_pedestrian_on)
		return (true);

	if (moving_objects == NULL)
		return (false);

	carmen_vector_2D_t world_point;
	double displacement = distance_between_front_and_rear_axles + distance_between_front_car_and_front_wheels;
	double theta = pedestrain_track_annotation.annotation_orientation;
	world_point.x = pedestrain_track_annotation.annotation_point.x + displacement * cos(theta);
	world_point.y = pedestrain_track_annotation.annotation_point.y + displacement * sin(theta);

	for (int i = 0; i < moving_objects->num_point_clouds; i++)
	{
		if ((strcmp(moving_objects->point_clouds[i].model_features.model_name, "pedestrian") == 0) &&
			(DIST2D(moving_objects->point_clouds[i].object_pose, world_point) < pedestrain_track_annotation.annotation_point.z))
			return (true);
	}
	return (false);
}


bool
add_annotation(double x, double y, double theta, size_t annotation_index)
{
	double dx = annotation_read_from_file[annotation_index].annotation_point.x - x;
	double dy = annotation_read_from_file[annotation_index].annotation_point.y - y;
	double dist = sqrt(pow(dx, 2) + pow(dy, 2));
	double angle_to_annotation = carmen_radians_to_degrees(fabs(carmen_normalize_theta(theta - annotation_read_from_file[annotation_index].annotation_orientation)));

	if (annotation_read_from_file[annotation_index].annotation_type == RDDF_ANNOTATION_TYPE_TRAFFIC_LIGHT)
	{
		bool orientation_ok = angle_to_annotation < 70.0 ? true : false;

		if ((dist < MAX_TRAFFIC_LIGHT_DISTANCE) && orientation_ok)
		{
			if ((traffic_lights != NULL) &&
				(traffic_lights->num_traffic_lights > 0)) // @@@ Alberto: deveria verificar a maioria...
			{
				int num_red = 0;
				for (int i = 0; i < traffic_lights->num_traffic_lights; i++)
					if (traffic_lights->traffic_lights[i].color == RDDF_ANNOTATION_CODE_TRAFFIC_LIGHT_RED)
						num_red++;

				annotation_and_index annotation_i = {annotation_read_from_file[annotation_index], annotation_index};
				if (num_red > 0)
					annotation_i.annotation.annotation_code = RDDF_ANNOTATION_CODE_TRAFFIC_LIGHT_RED;
				else
					annotation_i.annotation.annotation_code = traffic_lights->traffic_lights[0].color;
				annotations_to_publish.push_back(annotation_i);
			}
			else
			{
				annotation_and_index annotation_i = {annotation_read_from_file[annotation_index], annotation_index};
				annotations_to_publish.push_back(annotation_i);
			}
			return (true);
		}
	}
	else if (annotation_read_from_file[annotation_index].annotation_type == RDDF_ANNOTATION_TYPE_PEDESTRIAN_TRACK)
	{
		bool orientation_ok = angle_to_annotation < 70.0 ? true : false;

		if ((dist < 100.0) && orientation_ok)
		{
			annotation_and_index annotation_i = {annotation_read_from_file[annotation_index], annotation_index};
			if (pedestrian_track_busy(moving_objects, annotation_read_from_file[annotation_index]))
				annotation_i.annotation.annotation_code = RDDF_ANNOTATION_TYPE_PEDESTRIAN_TRACK_BUSY;
			else
				annotation_i.annotation.annotation_code = RDDF_ANNOTATION_CODE_NONE;
			annotations_to_publish.push_back(annotation_i);
			return (true);
		}
	}
	else if (annotation_read_from_file[annotation_index].annotation_type == RDDF_ANNOTATION_TYPE_STOP)
	{
		bool orientation_ok = angle_to_annotation < 15.0 ? true : false;

		if ((dist < 20.0) && orientation_ok)
		{
			annotation_and_index annotation_i = {annotation_read_from_file[annotation_index], annotation_index};
			annotations_to_publish.push_back(annotation_i);
			return (true);
		}
	}
	else if (dist < 20.0)
	{
		bool orientation_ok = angle_to_annotation < 70.0 ? true : false;

		if (orientation_ok)
		{
			annotation_and_index annotation_i = {annotation_read_from_file[annotation_index], annotation_index};
			annotations_to_publish.push_back(annotation_i);
		}
		return (true);
	}

	return (false);
}


int
direction_traffic_sign_found(carmen_point_t robot_pose)
{
	double distance_car_pose_car_front = distance_between_front_and_rear_axles + distance_between_front_car_and_front_wheels;

	for (size_t annotation_index = 0; annotation_index < annotation_read_from_file.size(); annotation_index++)
	{
		if (annotation_read_from_file[annotation_index].annotation_type == RDDF_ANNOTATION_TYPE_TRAFFIC_SIGN)
		{
			carmen_ackerman_traj_point_t annotation_point;
			annotation_point.x = annotation_read_from_file[annotation_index].annotation_point.x;
			annotation_point.y = annotation_read_from_file[annotation_index].annotation_point.y;
			annotation_point.theta = annotation_read_from_file[annotation_index].annotation_orientation;
			carmen_point_t new_annotation_point = carmen_collision_detection_displace_car_pose_according_to_car_orientation(&annotation_point, distance_car_pose_car_front);

			double dist = DIST2D(robot_pose, new_annotation_point);
			double search_radius = annotation_read_from_file[annotation_index].annotation_point.z;
			double angle_to_annotation = carmen_radians_to_degrees(fabs(carmen_normalize_theta(robot_pose.theta - annotation_read_from_file[annotation_index].annotation_orientation)));

			bool orientation_ok = angle_to_annotation < 70.0 ? true : false;

			if ((dist < search_radius) && orientation_ok)
				return (annotation_read_from_file[annotation_index].annotation_code);
		}
	}
	return (RDDF_ANNOTATION_CODE_NONE);
}


void
carmen_check_for_annotations(carmen_point_t robot_pose,
		carmen_ackerman_traj_point_t *carmen_rddf_poses_ahead, carmen_ackerman_traj_point_t *carmen_rddf_poses_back,
		int carmen_rddf_num_poses_ahead, int carmen_rddf_num_poses_back, double timestamp)
{
	for (size_t annotation_index = 0; annotation_index < annotation_read_from_file.size(); annotation_index++)
	{
		if (add_annotation(robot_pose.x, robot_pose.y, robot_pose.theta, annotation_index))
			continue;

		bool added = false;
		for (int j = 0; j < carmen_rddf_num_poses_ahead; j++)
		{
			if (add_annotation(carmen_rddf_poses_ahead[j].x, carmen_rddf_poses_ahead[j].y, carmen_rddf_poses_ahead[j].theta, annotation_index))
			{
				added = true;
				break;
			}
		}

		if (!added)
		{
			for (int j = 0; j < carmen_rddf_num_poses_back; j++)
				if (add_annotation(carmen_rddf_poses_back[j].x, carmen_rddf_poses_back[j].y, carmen_rddf_poses_back[j].theta, annotation_index))
					break;
		}
	}

	for (size_t j = 0; j < dynamic_annotation_messages.size(); j++)
	{
		if ((timestamp - dynamic_annotation_messages[j].timestamp) < 2.0)
		{
			carmen_annotation_t annotation;
			annotation.annotation_type = dynamic_annotation_messages[j].annotation_type;
			annotation.annotation_code = dynamic_annotation_messages[j].annotation_code;
			annotation.annotation_point = dynamic_annotation_messages[j].annotation_point;
			annotation.annotation_description = dynamic_annotation_messages[j].annotation_description;
			annotation.annotation_orientation = dynamic_annotation_messages[j].annotation_orientation;
			annotation_and_index annotation_i = {annotation, 0};
			annotations_to_publish.push_back(annotation_i);
		}
		else
			dynamic_annotation_messages.erase(dynamic_annotation_messages.begin() + j);
	}
}


void
find_nearest_waypoint_and_dist(carmen_annotation_t annotation, int *nearest_pose_out, double *nearest_pose_dist_out)
{
	int nearest_pose;
	double min_distance_to_annotation;
	double distance_to_annotation;

	nearest_pose = -1;
	min_distance_to_annotation = DBL_MAX;

	for (int i = 0; i < carmen_rddf_num_poses_ahead; i++)
	{
		distance_to_annotation = sqrt(pow(carmen_rddf_poses_ahead[i].x - annotation.annotation_point.x, 2) + pow(carmen_rddf_poses_ahead[i].y - annotation.annotation_point.y, 2));

		if (distance_to_annotation < min_distance_to_annotation)
		{
			min_distance_to_annotation = distance_to_annotation;
			nearest_pose = i;
		}
	}

	(*nearest_pose_out) = nearest_pose;
	(*nearest_pose_dist_out) = min_distance_to_annotation;
}


int
annotation_is_forward_from_robot(carmen_point_t pose, carmen_annotation_t annotation)
{
	carmen_vector_3D_t annotation_point;

	annotation_point = annotation.annotation_point;

	SE2 robot_pose_mat(pose.x, pose.y, pose.theta);
	SE2 annotation_point_mat(annotation_point.x, annotation_point.y, 0.0);
	SE2 annotation_in_car_reference = robot_pose_mat.inverse() * annotation_point_mat;

	if (annotation_in_car_reference[0] > 0.0)
		return 1;
	else
		return 0;
}


void
set_annotations(carmen_point_t robot_pose)
{
	int nearest_pose;
	double nearest_pose_dist;

	for (uint i = 0; i < annotations_to_publish.size(); i++)
	{
		find_nearest_waypoint_and_dist(annotations_to_publish[i].annotation, &nearest_pose, &nearest_pose_dist);

		if ((nearest_pose >= 0) && nearest_pose_dist < 10.0 && (annotation_is_forward_from_robot(robot_pose, annotations_to_publish[i].annotation)))
		{
			annotations[nearest_pose] = annotations_to_publish[i].annotation.annotation_type;
			annotations_codes[nearest_pose] = annotations_to_publish[i].annotation.annotation_code;
		}
	}
}


void
calculate_phi_ahead(carmen_ackerman_traj_point_t *path, int num_poses)
{
	double L = distance_between_front_and_rear_axles;

	for (int i = 0; i < (num_poses - 1); i++)
	{
		double delta_theta = carmen_normalize_theta(path[i + 1].theta - path[i].theta);
		double l = DIST2D(path[i], path[i + 1]);
		if (l < 0.01)
		{
			path[i].phi = 0.0;
			continue;
		}
		path[i].phi = L * atan(delta_theta / l);
	}

	for (int i = 1; i < (num_poses - 1); i++)
	{
		path[i].phi = (path[i].phi + path[i - 1].phi + path[i + 1].phi) / 3.0;
	}
}


void
calculate_phi_back(carmen_ackerman_traj_point_t *path, int num_poses)
{
	double L = distance_between_front_and_rear_axles;

	for (int i = (num_poses - 1); i > 0; i--)
	{
		double delta_theta = carmen_normalize_theta(path[i - 1].theta - path[i].theta);
		double l = DIST2D(path[i], path[i - 1]);
		if (l < 0.01)
		{
			path[i].phi = 0.0;
			continue;
		}
		path[i].phi = L * atan(delta_theta / l);
	}

	for (int i = (num_poses - 2); i > 0; i--)
	{
		path[i].phi = (path[i].phi + path[i - 1].phi + path[i + 1].phi) / 3.0;
	}
}


void
calculate_theta_ahead(carmen_ackerman_traj_point_t *path, int num_poses)
{
	for (int i = 0; i < (num_poses - 1); i++)
		path[i].theta = atan2(path[i + 1].y - path[i].y, path[i + 1].x - path[i].x);
	if (num_poses > 1)
		path[num_poses - 1].theta = path[num_poses - 2].theta;
}


void
calculate_theta_back(carmen_ackerman_traj_point_t *path, int num_poses)
{
	for (int i = 1; i < num_poses; i++)
//		path[i].theta = atan2(path[i - 1].y - path[i].y, path[i - 1].x - path[i].x);
		path[i].theta = carmen_normalize_theta(atan2(path[i].y - path[i - 1].y, path[i].x - path[i - 1].x) + M_PI);
}


void
calculate_theta_and_phi(carmen_ackerman_traj_point_t *poses_ahead, int num_poses_ahead,
		carmen_ackerman_traj_point_t *poses_back, int num_poses_back)
{
	calculate_theta_ahead(poses_ahead, num_poses_ahead);
	poses_back[0].theta = poses_ahead[0].theta;
	calculate_theta_back(poses_back, num_poses_back);

	calculate_phi_ahead(poses_ahead, num_poses_ahead);
	poses_back[0].phi = poses_ahead[0].phi;
	calculate_phi_back(poses_back, num_poses_back);
}


//////////////////////////////////////////////////////////////////////////////////

/* GSL - GNU Scientific Library
 * Multidimensional Minimization
 * https://www.gnu.org/software/gsl/doc/html/multimin.html
 *
 * Sebastian Thrun
 */


//Function to be minimized summation[x(i+1)-2x(i)+x(i-1)]
double
my_f(const gsl_vector *v, void *params)
{
	list<carmen_ackerman_traj_point_t> *p = (list<carmen_ackerman_traj_point_t> *) params;
	int i, j, size = (p->size() - 2);           //we have to discount the first and last point that wont be optimized
	double a = 0.0, b = 0.0, sum = 0.0;

	double x_prev = p->front().x;				//x(i-1)
	double x      = gsl_vector_get(v, 0);		//x(i)
	double x_next = gsl_vector_get(v, 1);		//x(i+1)

	double y_prev = p->front().y;
	double y      = gsl_vector_get(v, size);
	double y_next = gsl_vector_get(v, size+1);

	for (i = 2, j = (size+2); i < size; i++, j++)
	{
		a = x_next - (2*x) + x_prev;
		b = y_next - (2*y) + y_prev;
		sum += (a*a + b*b);

		x_prev = x;
		x      = x_next;
		x_next = gsl_vector_get(v, i);

		y_prev = y;
		y      = y_next;
		y_next = gsl_vector_get(v, j);
	}

	x_prev = x;
	x      = x_next;
	x_next = p->back().x;

	y_prev = y;
	y      = y_next;
	y_next = p->back().y;

	a = x_next - (2*x) + x_prev;
	b = y_next - (2*y) + y_prev;
	sum += (a*a + b*b);

	return (sum);
}


//The gradient of f, df = (df/dx, df/dy)
//derivative in each point [2x(i-2)-8x(i-1)+12x(i)-8x(i+1)+2x(i+2)]
void
my_df (const gsl_vector *v, void *params, gsl_vector *df)
{
	list<carmen_ackerman_traj_point_t> *p = (list<carmen_ackerman_traj_point_t> *) params;
	int i, j, size =(p->size() - 2);

	double x_prev2= 0;
	double x_prev = p->front().x;
	double x      = gsl_vector_get(v, 0);
	double x_next = gsl_vector_get(v, 1);
	double x_next2= gsl_vector_get(v, 2);
	double sum_x  =  (10*x) - (8*x_next) + (2*x_next2) - (4*x_prev);
	gsl_vector_set(df, 0, sum_x);

	double y_prev2= 0;
	double y_prev = p->front().y;
	double y      = gsl_vector_get(v, size);
	double y_next = gsl_vector_get(v, size+1);
	double y_next2= gsl_vector_get(v, size+2);
	double sum_y  = (10*y) - (8*y_next) + (2*y_next2) - (4*y_prev);
	gsl_vector_set(df, size, sum_y);

	for (i = 3, j = (size+3); i < size; i++, j++)
	{
		x_prev2= x_prev;
		x_prev = x;
		x      = x_next;
		x_next = x_next2;
		x_next2= gsl_vector_get(v, i);
		sum_x = (2*x_prev2) - (8*x_prev) + (12*x) - (8*x_next) + (2*x_next2);
		gsl_vector_set(df, (i-2), sum_x);

		y_prev2= y_prev;
		y_prev = y;
		y      = y_next;
		y_next = y_next2;
		y_next2= gsl_vector_get(v, j);
		sum_y = (2*y_prev2) - (8*y_prev) + (12*y) - (8*y_next) + (2*y_next2);
		gsl_vector_set(df, (j-2), sum_y);
	}

	x_prev2= x_prev;
	x_prev = x;
	x      = x_next;
	x_next = x_next2;
	x_next2= p->back().x;
	sum_x  = (2*x_prev2) - (8*x_prev) + (12*x) - (8*x_next) + (2*x_next2);
	gsl_vector_set(df, size-2, sum_x);

	y_prev2= y_prev;
	y_prev = y;
	y      = y_next;
	y_next = y_next2;
	y_next2= p->back().y;
	sum_y  = (2*y_prev2) - (8*y_prev) + (12*y) - (8*y_next) + (2*y_next2);
	gsl_vector_set(df, (2*size)-2, sum_y);

	x_prev2= x_prev;
	x_prev = x;
	x      = x_next;
	x_next = x_next2;
	sum_x  = (2*x_prev2) - (8*x_prev) + (10*x) - (4*x_next);
	gsl_vector_set(df, size-1, sum_x);

	y_prev2= y_prev;
	y_prev = y;
	y      = y_next;
	y_next = y_next2;
	sum_y  = (2*y_prev2) - (8*y_prev) + (10*y) - (4*y_next);
	gsl_vector_set(df, (2*size)-1, sum_y);
}


// Compute both f and df together
void
my_fdf (const gsl_vector *x, void *params, double *f, gsl_vector *df)
{
	*f = my_f(x, params);
	my_df(x, params, df);
}


int
smooth_rddf_using_conjugate_gradient(carmen_ackerman_traj_point_t *poses_ahead, int num_poses_ahead,
		carmen_ackerman_traj_point_t *poses_back, int num_poses_back)
{
	int iter = 0;
	int status, i = 0, j = 0, size;

	const gsl_multimin_fdfminimizer_type *T;
	gsl_multimin_fdfminimizer *s;
	gsl_vector *v;
	gsl_multimin_function_fdf my_func;

	list<carmen_ackerman_traj_point_t>::iterator it;
	list<carmen_ackerman_traj_point_t> path;

	for (i = (num_poses_back - 1); i > 0; i--) // skip poses_back[0], because it is equal to poses_ahead[0]
		path.push_back(poses_back[i]);

	for (i = 0; i < num_poses_ahead; i++)
		path.push_back(poses_ahead[i]);

	if (path.size() < 5)
		return (1);

	size = path.size();

	my_func.n = (2 * size) - 4;
	my_func.f = my_f;
	my_func.df = my_df;
	my_func.fdf = my_fdf;
	my_func.params = &path;

	v = gsl_vector_alloc ((2 * size) - 4);

	static int count = 0;
	count++;
//	FILE *plot = fopen("gnuplot_smooth_lane.m", "w");

//	fprintf(plot, "a%d = [\n", count);
	it = path.begin();
//	fprintf(plot, "%f %f\n", it->x, it->y);

	it++; // skip the first pose
	for (i = 0, j = (size - 2); i < (size - 2); i++, j++, it++)
	{
//		fprintf(plot, "%f %f\n", it->x, it->y);

		gsl_vector_set (v, i, it->x);
		gsl_vector_set (v, j, it->y);
	}

//	fprintf(plot, "%f %f]\n\n", it->x, it->y);

	T = gsl_multimin_fdfminimizer_conjugate_fr;
	s = gsl_multimin_fdfminimizer_alloc (T, (2 * size) - 4);

	gsl_multimin_fdfminimizer_set (s, &my_func, v, 0.1, 0.01);  //(function_fdf, gsl_vector, step_size, tol)

	do
	{
		iter++;
		status = gsl_multimin_fdfminimizer_iterate (s);
		if (status) // error code
			return (0);

		status = gsl_multimin_test_gradient (s->gradient, 0.2);   //(gsl_vector, epsabs) and  |g| < epsabs
		// status == 0 (GSL_SUCCESS), if a minimum has been found
	} while (status == GSL_CONTINUE && iter < 999);

//	printf("status %d, iter %d\n", status, iter);
//	fflush(stdout);
	it = path.begin();

//	fprintf(plot, "b%d = [   \n%f %f\n", count, it->x, it->y);

	it++; // skip the first pose
	for (i = 0, j = (size - 2); i < (size - 2); i++, j++, it++)
	{
		it->x = gsl_vector_get (s->x, i);
		it->y = gsl_vector_get (s->x, j);

//		fprintf(plot, "%f %f\n", it->x, it->y);
	}

//	fprintf(plot, "%f %f]\n\n", it->x, it->y);
//	fprintf(plot, "\nplot (a%d(:,1), a%d(:,2), b%d(:,1), b%d(:,2)); \nstr = input (\"a   :\");\n\n", count, count, count, count);
//	fclose(plot);

	it = path.begin();
	it++;
	for (i = (num_poses_back - 2); i > 0; i--, it++) // skip first and last poses
		poses_back[i] = *it;
	poses_back[0] = *it;

	for (i = 0; i < num_poses_ahead - 1; i++, it++) // skip last pose
		poses_ahead[i] = *it;

	calculate_theta_and_phi(poses_ahead, num_poses_ahead, poses_back, num_poses_back);

	gsl_multimin_fdfminimizer_free (s);
	gsl_vector_free (v);

	return (1);
}

//////////////////////////////////////////////////////////////////////////////////


void
plot_state(carmen_ackerman_traj_point_t *path, int num_points, carmen_ackerman_traj_point_t *path2, int num_points2, bool display)
{
	static bool first_time = true;
	static FILE *gnuplot_pipeMP;

	if (first_time)
	{
		gnuplot_pipeMP = popen("gnuplot", "w"); // -persist to keep last plot after program closes
		fprintf(gnuplot_pipeMP, "set xlabel 'x'\n");
		fprintf(gnuplot_pipeMP, "set ylabel 'y'\n");
		fprintf(gnuplot_pipeMP, "set tics out\n");
		first_time = false;
	}

	if (display)
	{
		system("cp gnuplot_data_lane.txt gnuplot_data_lane_.txt");
		system("cp gnuplot_data_lane2.txt gnuplot_data_lane2_.txt");
	}

	FILE *gnuplot_data_lane  = fopen("gnuplot_data_lane.txt", "w");
	FILE *gnuplot_data_lane2 = fopen("gnuplot_data_lane2.txt", "w");

	for (int i = 0; i < num_points; i++)
	{
		fprintf(gnuplot_data_lane, "%lf %lf %lf %lf %lf %lf\n", path[i].x, path[i].y,
				0.8 * cos(path[i].theta), 0.8 * sin(path[i].theta), path[i].theta, path[i].phi);
	}
	for (int i = 0; i < num_points2; i++)
	{
		fprintf(gnuplot_data_lane2, "%lf %lf %lf %lf %lf %lf\n", path2[i].x, path2[i].y,
				0.8 * cos(path2[i].theta), 0.8 * sin(path2[i].theta), path2[i].theta, path2[i].phi);
	}
	fclose(gnuplot_data_lane);
	fclose(gnuplot_data_lane2);

	if (display)
	{
		fprintf(gnuplot_pipeMP, "plot "
				"'./gnuplot_data_lane_.txt' using 1:2:3:4 w vec size  0.3, 10 filled title 'Lane Ahead normal'"
				", './gnuplot_data_lane2_.txt' using 1:2:3:4 w vec size  0.3, 10 filled title 'Lane Back normal'"
				", './gnuplot_data_lane.txt' using 1:2:3:4 w vec size  0.3, 10 filled title 'Lane Ahead smooth'"
				", './gnuplot_data_lane2.txt' using 1:2:3:4 w vec size  0.3, 10 filled title 'Lane Back smooth' axes x1y1\n");
		fflush(gnuplot_pipeMP);
//		fprintf(gnuplot_pipeMP, "plot "
////				"'./gnuplot_data_lane_.txt' using 1:2 w l title 'Lane Ahead normal'"
////				", './gnuplot_data_lane2_.txt' using 1:2 w l title 'Lane Back normal'"
//				"'./gnuplot_data_lane.txt' using 1:2 w l title 'Lane Ahead smooth'"
//				", './gnuplot_data_lane2.txt' using 1:2 w l title 'Lane Back smooth' axes x1y1\n");
//		fflush(gnuplot_pipeMP);
	}
}


int
pose_out_of_map_coordinates(carmen_point_t pose, carmen_map_p map)
{
	double x_min = map->config.x_origin;
	double x_max = map->config.x_origin + map->config.x_size * map->config.resolution;
	double y_min = map->config.y_origin;
	double y_max = map->config.y_origin + map->config.y_size * map->config.resolution;
	int out_of_map = (pose.x < x_min || pose.x >= x_max || pose.y < y_min || pose.y >= y_max);

	return (out_of_map);
}


double
get_lane_prob(carmen_point_t pose, carmen_map_p road_map)
{
	int x = round((pose.x - road_map->config.x_origin) / road_map->config.resolution);
	int y = round((pose.y - road_map->config.y_origin) / road_map->config.resolution);
	if (x < 0 || x >= road_map->config.x_size || y < 0 || y >= road_map->config.y_size)
		return (-1.0);

	double cell = road_map->map[x][y];
	road_prob *cell_prob = (road_prob *) &cell;
//	double prob = (double) (cell_prob->lane_center - cell_prob->off_road - cell_prob->broken_marking - cell_prob->solid_marking) / MAX_PROB;
	double prob = (double) (cell_prob->lane_center) / MAX_PROB;

	return ((prob > 0.0) ? prob : 0.0);
}


int
get_nearest_lane(carmen_point_p lane_pose, carmen_point_t pose, carmen_map_p road_map)
{
	double dx = 1.0 + (pose.x - road_map->config.x_origin) / road_map->config.resolution;
	double dy = 1.0 + (pose.y - road_map->config.y_origin) / road_map->config.resolution;
	if (dx < road_map->config.x_size / 2.0)
		dx = road_map->config.x_size - dx;
	if (dy < road_map->config.y_size / 2.0)
		dy = road_map->config.y_size - dy;
	double max_radius = sqrt(dx * dx + dy * dy);
	double max_angle = 2.0 * M_PI;
	for (double radius = 1.0; radius <= max_radius; radius += 1.0)
	{
		double delta_angle = 1.0 / radius;
		for (double angle = 0.0; angle < max_angle; angle += delta_angle)
		{
			lane_pose->x = pose.x + radius * cos(angle) * road_map->config.resolution;
			lane_pose->y = pose.y + radius * sin(angle) * road_map->config.resolution;
			if (get_lane_prob(*lane_pose, road_map) >= 0.25) // pose is probably located in a road lane
				return (1);
		}
	}

	return (0);
}


int
find_pose_in_vector(vector<carmen_point_t> poses, carmen_point_t pose, double resolution)
{
	int pose_x = round(pose.x / resolution);
	int pose_y = round(pose.y / resolution);
	for (size_t i = 0; i < poses.size(); i++)
	{
		int x = round(poses[i].x / resolution);
		int y = round(poses[i].y / resolution);
		if (x == pose_x && y == pose_y)
			return (i);
	}

	return (-1);
}


int
distance_within_limits(carmen_point_p pose1, carmen_point_p pose2, double dist_min, double dist_max)
{
	if (dist_min == 0 && dist_max == 0)	// dist_max == 0 corresponds to MAX_VALUE
		return (1);
	if (pose1 == NULL || pose2 == NULL)
		return (0);

	double distance = DIST2D(*pose1, *pose2);
	int within_limits = ((distance >= dist_min) && (distance <= dist_max || dist_max == 0));

	return (within_limits);
}


double
get_center_of_mass(carmen_point_p central_pose, carmen_map_p road_map, int kernel_size, double weight(carmen_point_t, carmen_map_p),
		carmen_point_p skip_pose = NULL, double dist_min = 0.0, double dist_max = 0.0)
{
	carmen_point_t pose;
	double sum_wx = 0.0, sum_wy = 0.0, sum_w = 0.0;
	int count = 0;

	pose.x = central_pose->x - floor(kernel_size / 2) * road_map->config.resolution;
	for (int x = 0; x < kernel_size; x++, pose.x += road_map->config.resolution)
	{
		pose.y = central_pose->y - floor(kernel_size / 2) * road_map->config.resolution;
		for (int y = 0; y < kernel_size; y++, pose.y += road_map->config.resolution)
		{
			if (distance_within_limits(&pose, skip_pose, dist_min, dist_max))
			{
				double pose_weight = weight(pose, road_map);
				if (pose_weight >= 0.0)
				{
					sum_wx += pose_weight * pose.x;
					sum_wy += pose_weight * pose.y;
					sum_w  += pose_weight;
					count++;
				}
			}
		}
	}
	if (sum_w == 0.0)
		return (0.0);

	central_pose->x = sum_wx / sum_w;
	central_pose->y = sum_wy / sum_w;
	double mean_w = sum_w / count;

	return (mean_w);
}


carmen_point_t
get_pose_with_max_lane_prob(vector<carmen_point_t> poses, carmen_map_p road_map)
{
	carmen_point_t pose;
	double max_prob = -1.0;

	for (size_t i = 0; i < poses.size(); i++)
	{
		double lane_prob = get_lane_prob(poses[i], road_map);
		if (lane_prob > max_prob)
		{
			pose = poses[i];
			max_prob = lane_prob;
		}
	}

	return (pose);
}


double
get_orthogonal_angle(double x1, double y1, double x2, double y2)
{
	double orthogonal_angle = atan2(x1 - x2, y2 - y1);

	return (orthogonal_angle);
}


double
mean_angle(double angle1, double angle2)
{
	double mean = atan2((sin(angle1) + sin(angle2)) / 2, (cos(angle1) + cos(angle2)) / 2);

	return (mean);
}


carmen_point_t
add_distance_to_pose(carmen_point_t pose, double distance)
{
	carmen_point_t next_pose = pose;
	next_pose.x += distance * cos(pose.theta);
	next_pose.y += distance * sin(pose.theta);

	return (next_pose);
}


carmen_point_t
add_orthogonal_distance_to_pose(carmen_point_t pose, double distance)
{
	carmen_point_t next_pose = pose;
	double orthogonal_theta;
	if (distance >= 0.0)
		orthogonal_theta = carmen_normalize_theta(pose.theta + (M_PI / 2.0));
	else
		orthogonal_theta = carmen_normalize_theta(pose.theta - (M_PI / 2.0));
	next_pose.x += fabs(distance) * cos(orthogonal_theta);
	next_pose.y += fabs(distance) * sin(orthogonal_theta);

	return (next_pose);
}


int
lane_prob_cmp(double left_prob, double right_prob)
{
	double max_prob_diff = 0.04;
	double prob_diff = left_prob - right_prob;
	int cmp = (prob_diff < -max_prob_diff) ? -1 : (prob_diff > max_prob_diff) ? 1 : 0;

	return (cmp);
}


//FILE *rddf_log = NULL;
//int g_num_pose;


carmen_point_t
find_nearest_pose_by_road_map(carmen_point_t previous_pose, carmen_point_t rddf_pose_candidate, carmen_map_p road_map, bool ahead)
{
	carmen_point_t rddf_pose_right, rddf_pose_left;
	carmen_point_t lane_pose = rddf_pose_right = rddf_pose_left = rddf_pose_candidate; // Keep candidate's theta

	double step = road_map->config.resolution / 4.0;
	double lane_expected_width = 1.0;
	double left_limit = lane_expected_width / 2.0;
	double right_limit = -lane_expected_width / 2.0;
	double max_angle_diff = M_PI / 4.0;  // 45 degrees
	double max_lane_prob_right = -1.0, max_lane_prob_left = -1.0;
	double lane_prob, last_lane_prob = -1.0;
	double angle_diff_right = M_PI, angle_diff_left = M_PI;
	bool check_angle_diff = (DIST2D(previous_pose, rddf_pose_candidate) != 0.0); // First rddf_pose_candidate has no distinct previous_pose
	bool downslope_found = false, valley_found = false;
	bool first_time = true;

	// Search for the two highest lane probabilities, from right to left
	for (double delta_pose = right_limit; delta_pose <= left_limit; delta_pose += step)
	{
		lane_pose = add_orthogonal_distance_to_pose(rddf_pose_candidate, delta_pose);
		lane_prob = get_lane_prob(lane_pose, road_map);

		carmen_point_t lane_pose_rounded = lane_pose;
		int cell_x = round((lane_pose.x - road_map->config.x_origin) / road_map->config.resolution);
		int cell_y = round((lane_pose.y - road_map->config.y_origin) / road_map->config.resolution);
		lane_pose_rounded.x = road_map->config.x_origin + cell_x * road_map->config.resolution;
		lane_pose_rounded.y = road_map->config.y_origin + cell_y * road_map->config.resolution;

//		fprintf(rddf_log, "%d\t%.0lf\t%lf\t%.0lf\t", g_num_pose, round(delta_pose/step), lane_prob, round(lane_prob * 255));
//		fprintf(rddf_log, "lane_pose\t%lf\t%lf\t%lf\n", lane_pose_rounded.x, lane_pose_rounded.y, lane_pose.theta);

		if (lane_prob < 0.0)
			continue; // Ignore pose out of the map boundaries

		double theta, angle_diff = 0.0;
		if (check_angle_diff)
		{
			if (ahead)
			{
				theta = atan2(lane_pose.y - previous_pose.y, lane_pose.x - previous_pose.x);
				angle_diff = carmen_normalize_theta(theta - previous_pose.theta);
			}
			else
			{
				theta = atan2(previous_pose.y - lane_pose.y, previous_pose.x - lane_pose.x);
				angle_diff = carmen_normalize_theta(previous_pose.theta - theta);
			}
		}
		if (fabs(angle_diff) > max_angle_diff)
			continue; // Ignore pose too far from the average orientation of previous poses

		if (!first_time && (DIST2D(lane_pose_rounded, rddf_pose_left) == 0.0))
			continue; // Ignore pose in the same map cell

		int slope = lane_prob_cmp(lane_prob, last_lane_prob);
		downslope_found |= (slope < 0);
		if (slope > 0 && !valley_found && downslope_found)
		{
			valley_found = true;
			max_lane_prob_left = -1.0;
		}
		last_lane_prob = lane_prob;

		if (lane_prob > max_lane_prob_left)
		{
			max_lane_prob_left = lane_prob;
			rddf_pose_left = lane_pose_rounded;
			angle_diff_left = angle_diff;
			if (!valley_found) // there is no distinction between left and right yet
			{
				max_lane_prob_right = lane_prob;
				rddf_pose_right = lane_pose_rounded;
				angle_diff_right = angle_diff;
			}
		}
		first_time = false;
	}

	int direction_code = direction_traffic_sign_found(rddf_pose_candidate);

//	fprintf(rddf_log, "%d\tprevious\t%lf\t%lf\t%lf\t", g_num_pose, previous_pose.x, previous_pose.y, previous_pose.theta);
//	fprintf(rddf_log, "candidate\t%lf\t%lf\t%lf\t", rddf_pose_candidate.x, rddf_pose_candidate.y, rddf_pose_candidate.theta);
//	fprintf(rddf_log, "pose_left\t%lf\t%lf\t%lf\t", rddf_pose_left.x, rddf_pose_left.y, rddf_pose_left.theta);
//	fprintf(rddf_log, "pose_right\t%lf\t%lf\t%lf\t", rddf_pose_right.x, rddf_pose_right.y, rddf_pose_right.theta);
//	fprintf(rddf_log, "direction\t%d\n", direction_code);

	switch (direction_code)
	{
		case RDDF_ANNOTATION_CODE_TRAFFIC_SIGN_TURN_RIGHT:
			return (rddf_pose_right);

		case RDDF_ANNOTATION_CODE_TRAFFIC_SIGN_TURN_LEFT:
			return (rddf_pose_left);

		case RDDF_ANNOTATION_CODE_TRAFFIC_SIGN_GO_STRAIGHT:
			// Pick the pose most aligned with the average orientation of previous poses
			if (fabs(angle_diff_right) < fabs(angle_diff_left))
				return (rddf_pose_right);
			else
				return (rddf_pose_left);

		default:
			// Pick the pose with the highest lane probability
			if (max_lane_prob_right > max_lane_prob_left)
				return (rddf_pose_right);
			else
				return (rddf_pose_left);
	}
}


//carmen_point_t
//find_nearest_pose_by_road_map(carmen_point_t previous_pose, carmen_point_t rddf_pose_candidate, carmen_map_p road_map)
//{
//	carmen_point_t rddf_pose_right, rddf_pose_left;
//	carmen_point_t lane_pose = rddf_pose_right = rddf_pose_left = rddf_pose_candidate; // Keep candidate's theta
//
//	double step = road_map->config.resolution / 4.0;
//	double lane_expected_width = 1.0;
//	double left_limit = lane_expected_width / 2.0;
//	double right_limit = -lane_expected_width / 2.0;
//	double max_angle_diff = M_PI / 4.0;  // 45 degrees
//	double max_lane_prob_right = -1.0, max_lane_prob_left = -1.0;
//	double angle_diff_right = M_PI, angle_diff_left = M_PI;
//	bool check_angle_diff = (DIST2D(previous_pose, rddf_pose_candidate) != 0.0); // First rddf_pose_candidate has no distinct previous_pose
//	bool first_time = true;
//
//	// Search for the two highest lane probabilities, from right to left
//	for (double delta_pose = right_limit; delta_pose <= left_limit; delta_pose += step)
//	{
//		lane_pose = add_orthogonal_distance_to_pose(rddf_pose_candidate, delta_pose);
//		double lane_prob = get_lane_prob(lane_pose, road_map);
//		if (lane_prob < 0.0)
//			continue; // Ignore pose out of the map boundaries
//
//		double angle_diff = 0.0;
//		if (check_angle_diff)
//		{
//			double theta = atan2(lane_pose.y - previous_pose.y, lane_pose.x - previous_pose.x);
//			angle_diff = carmen_normalize_theta(theta - previous_pose.theta);
//			if (fabs(angle_diff) > max_angle_diff)
//				continue; // Ignore pose too far from the average orientation of previous poses
//		}
//
//		carmen_point_t lane_pose_rounded = lane_pose;
//		lane_pose_rounded.x = round(lane_pose.x / road_map->config.resolution) * road_map->config.resolution;
//		lane_pose_rounded.y = round(lane_pose.y / road_map->config.resolution) * road_map->config.resolution;
//		if (!first_time && lane_pose_rounded.x == rddf_pose_left.x && lane_pose_rounded.y == rddf_pose_left.y)
//			continue; // Ignore pose in the same map cell
//
//		if (lane_prob >= max_lane_prob_left || lane_prob > max_lane_prob_right) // found pose with higher probability
//		{
//			if (max_lane_prob_left > max_lane_prob_right)
//			{
//				max_lane_prob_right = max_lane_prob_left;
//				rddf_pose_right = rddf_pose_left;
//				angle_diff_right = angle_diff_left;
//			}
//			max_lane_prob_left = lane_prob;
//			rddf_pose_left = lane_pose_rounded;
//			angle_diff_left = angle_diff;
//			first_time = false;
//		}
//	}
//
//	int direction_code = direction_traffic_sign_found(rddf_pose_candidate);
//	switch (direction_code)
//	{
//		case RDDF_ANNOTATION_CODE_TRAFFIC_SIGN_TURN_RIGHT:
//			return (rddf_pose_right);
//
//		case RDDF_ANNOTATION_CODE_TRAFFIC_SIGN_TURN_LEFT:
//			return (rddf_pose_left);
//
//		case RDDF_ANNOTATION_CODE_TRAFFIC_SIGN_GO_STRAIGHT:
//			// Pick the pose most aligned with the average orientation of previous poses
//			if (fabs(angle_diff_right) < fabs(angle_diff_left))
//				return (rddf_pose_right);
//			else
//				return (rddf_pose_left);
//
//		default:
//			// Pick the pose with the highest lane probability
//			if (max_lane_prob_right > max_lane_prob_left)
//				return (rddf_pose_right);
//			else
//				return (rddf_pose_left);
//	}
//}


//carmen_point_t
//find_nearest_pose_back_by_road_map(carmen_point_t previous_pose, carmen_point_t rddf_pose_candidate, carmen_map_p road_map)
//{
//	carmen_point_t rddf_pose_right, rddf_pose_left;
//	carmen_point_t lane_pose = rddf_pose_right = rddf_pose_left = rddf_pose_candidate; // Keep candidate's theta
//
//	double step = road_map->config.resolution / 4.0;
//	double lane_expected_width = 1.0;
//	double left_limit = lane_expected_width / 2.0;
//	double right_limit = -lane_expected_width / 2.0;
//	double max_angle_diff = M_PI / 4.0;  // 45 degrees
//	double max_lane_prob_right = -1.0, max_lane_prob_left = -1.0;
//	double angle_diff_right = M_PI, angle_diff_left = M_PI;
//	double lane_prob, last_lane_prob = -1.0;
//	bool downslope_found = false, valley_found = false;
//	bool first_time = true;
//
//	// Search for the two highest lane probabilities, from right to left
//	for (double delta_pose = right_limit; delta_pose <= left_limit; delta_pose += step)
//	{
//		lane_pose = add_orthogonal_distance_to_pose(rddf_pose_candidate, delta_pose);
//		lane_prob = get_lane_prob(lane_pose, road_map);
//
//		carmen_point_t lane_pose_rounded = lane_pose;
//		int cell_x = round((lane_pose.x - road_map->config.x_origin) / road_map->config.resolution);
//		int cell_y = round((lane_pose.y - road_map->config.y_origin) / road_map->config.resolution);
//		lane_pose_rounded.x = road_map->config.x_origin + cell_x * road_map->config.resolution;
//		lane_pose_rounded.y = road_map->config.y_origin + cell_y * road_map->config.resolution;
//
//		fprintf(rddf_log, "%d\t%.0lf\t%lf\t%.0lf\t", g_num_pose, round(delta_pose/step), lane_prob, round(lane_prob * 255));
//		fprintf(rddf_log, "lane_pose\t%lf\t%lf\t%lf\n", lane_pose_rounded.x, lane_pose_rounded.y, lane_pose.theta);
//
//		if (lane_prob < 0.0)
//			continue; // Ignore pose out of the map boundaries
//
//		double angle_diff = 0.0;
//		double theta = atan2(previous_pose.y - lane_pose.y, previous_pose.x - lane_pose.x);
//		angle_diff = carmen_normalize_theta(previous_pose.theta - theta);
//		if (fabs(angle_diff) > max_angle_diff)
//			continue; // Ignore pose too far from the average orientation of previous poses
//
//		if (!first_time && lane_pose_rounded.x == rddf_pose_left.x && lane_pose_rounded.y == rddf_pose_left.y)
//			continue; // Ignore pose in the same map cell
//
//		int slope = lane_prob_cmp(lane_prob, last_lane_prob);
//		downslope_found |= (slope < 0);
//		if (slope > 0 && !valley_found && downslope_found)
//		{
//			valley_found = true;
//			max_lane_prob_left = -1.0;
//		}
//		last_lane_prob = lane_prob;
//
//		if (!valley_found)
//		{
//			slope = lane_prob_cmp(lane_prob, max_lane_prob_left);
//			if (slope > 0) // peak not reached yet
//			{
//				max_lane_prob_left = max_lane_prob_right = lane_prob;
//				rddf_pose_left = rddf_pose_right = lane_pose_rounded;
//				angle_diff_left = angle_diff_right = angle_diff;
//			}
//			else if (slope == 0) // peak reached: keep distinct right and left poses (might be a junction or a fork)
//			{
//				max_lane_prob_left = lane_prob;
//				rddf_pose_left = lane_pose_rounded;
//				angle_diff_left = angle_diff;
//			}
//		}
//		else
//		{
//			if (lane_prob >= max_lane_prob_left)
//			{
//				max_lane_prob_left = lane_prob;
//				rddf_pose_left = lane_pose_rounded;
//				angle_diff_left = angle_diff;
//			}
//		}
//
////		if (lane_prob >= max_lane_prob_left || lane_prob > max_lane_prob_right) // found pose with higher probability
////		{
////			if (!valley_found && max_lane_prob_left > max_lane_prob_right)
////			{
////				max_lane_prob_right = max_lane_prob_left;
////				rddf_pose_right = rddf_pose_left;
////				angle_diff_right = angle_diff_left;
////			}
////			if (!valley_found || lane_prob >= max_lane_prob_left)
////			{
////				max_lane_prob_left = lane_prob;
////				rddf_pose_left = lane_pose_rounded;
////				angle_diff_left = angle_diff;
////			}
////		}
//
//		first_time = false;
//	}
//
//	int direction_code = direction_traffic_sign_found(rddf_pose_candidate);
//
//	fprintf(rddf_log, "%d\tprevious\t%lf\t%lf\t%lf\t", g_num_pose, previous_pose.x, previous_pose.y, previous_pose.theta);
//	fprintf(rddf_log, "candidate\t%lf\t%lf\t%lf\t", rddf_pose_candidate.x, rddf_pose_candidate.y, rddf_pose_candidate.theta);
//	fprintf(rddf_log, "pose_left\t%lf\t%lf\t%lf\t", rddf_pose_left.x, rddf_pose_left.y, rddf_pose_left.theta);
//	fprintf(rddf_log, "pose_right\t%lf\t%lf\t%lf\t", rddf_pose_right.x, rddf_pose_right.y, rddf_pose_right.theta);
//	fprintf(rddf_log, "direction\t%d\n", direction_code);
//
//	switch (direction_code)
//	{
//		case RDDF_ANNOTATION_CODE_TRAFFIC_SIGN_TURN_RIGHT:
//			return (rddf_pose_right);
//
//		case RDDF_ANNOTATION_CODE_TRAFFIC_SIGN_TURN_LEFT:
//			return (rddf_pose_left);
//
//		case RDDF_ANNOTATION_CODE_TRAFFIC_SIGN_GO_STRAIGHT:
//			// Pick the pose most aligned with the average orientation of previous poses
//			if (fabs(angle_diff_right) < fabs(angle_diff_left))
//				return (rddf_pose_right);
//			else
//				return (rddf_pose_left);
//
//		default:
//			// Pick the pose with the highest lane probability
//			if (max_lane_prob_right > max_lane_prob_left)
//				return (rddf_pose_right);
//			else
//				return (rddf_pose_left);
//	}
//}


//carmen_point_t
//find_nearest_pose_by_road_map(carmen_point_t previous_pose, carmen_point_t rddf_pose_candidate, carmen_map_p road_map)
//{
//	carmen_point_t rddf_pose;
//	carmen_point_t lane_pose = rddf_pose = rddf_pose_candidate;
//
//	double step = road_map->config.resolution / 4.0;
//	double lane_expected_width = 1.0;
//	double left_limit = lane_expected_width / 2.0;
//	double right_limit = -lane_expected_width / 2.0;
//
//	int direction_code = direction_traffic_sign_found(rddf_pose_candidate);
//	switch (direction_code)
//	{
//		case RDDF_ANNOTATION_CODE_TRAFFIC_SIGN_GO_STRAIGHT:
////			left_limit /= 10.0;
////			right_limit /= 10.0;
//			break;
//		case RDDF_ANNOTATION_CODE_TRAFFIC_SIGN_TURN_LEFT:
//			right_limit /= 10.0;
//			break;
//		case RDDF_ANNOTATION_CODE_TRAFFIC_SIGN_TURN_RIGHT:
//			left_limit /= 10.0;
//			break;
//	}
//
//	double max_lane_prob = 0.0;
//	for (double delta_pose = right_limit; delta_pose <= left_limit; delta_pose += step)
//	{
//		lane_pose = add_orthogonal_distance_to_pose(rddf_pose_candidate, delta_pose);
//		if (DIST2D(previous_pose, rddf_pose_candidate) != 0.0) // A pose zero do rddf nao tem previous_pose
//		{
//			double angle = atan2(lane_pose.y - previous_pose.y, lane_pose.x - previous_pose.x);
//			double angle_diff = carmen_radians_to_degrees(fabs(carmen_normalize_theta(previous_pose.theta - angle)));
//			if ((direction_code == RDDF_ANNOTATION_CODE_TRAFFIC_SIGN_GO_STRAIGHT) && (angle_diff > 15.0))
//				continue;
//		}
//		double lane_prob = get_lane_prob(lane_pose, road_map);
//		if (lane_prob > max_lane_prob)
//		{
//			max_lane_prob = lane_prob;
//			rddf_pose.x = round(lane_pose.x / road_map->config.resolution) * road_map->config.resolution;
//			rddf_pose.y = round(lane_pose.y / road_map->config.resolution) * road_map->config.resolution;
//		}
//	}
//
//	return (rddf_pose);
//}


//carmen_point_t
//carmen_rddf_play_find_nearest_pose_back_by_road_map(carmen_point_t previous_pose, carmen_point_t rddf_pose_candidate, carmen_map_p road_map)
//{
//	carmen_point_t rddf_pose;
//	carmen_point_t lane_pose = rddf_pose = rddf_pose_candidate;
//
//	double step = road_map->config.resolution / 4.0;
//	double lane_expected_width = 1.0;
//	double left_limit = lane_expected_width / 2.0;
//	double right_limit = -lane_expected_width / 2.0;
//
//	int direction_code = direction_traffic_sign_found(previous_pose);
//	switch (direction_code)
//	{
//		case RDDF_ANNOTATION_CODE_TRAFFIC_SIGN_GO_STRAIGHT:
////			left_limit /= 10.0;
////			right_limit /= 10.0;
//			break;
//		case RDDF_ANNOTATION_CODE_TRAFFIC_SIGN_TURN_LEFT:
//			right_limit /= 10.0;
//			break;
//		case RDDF_ANNOTATION_CODE_TRAFFIC_SIGN_TURN_RIGHT:
//			left_limit /= 10.0;
//			break;
//	}
//
//	double max_lane_prob = 0.0;
//	for (double delta_pose = right_limit; delta_pose <= left_limit; delta_pose += step)
//	{
//		lane_pose = add_orthogonal_distance_to_pose(rddf_pose_candidate, delta_pose);
//		double angle = atan2(previous_pose.y - lane_pose.y, previous_pose.x - lane_pose.x);
//		double angle_diff = carmen_radians_to_degrees(fabs(carmen_normalize_theta(previous_pose.theta - angle)));
//		if ((direction_code == RDDF_ANNOTATION_CODE_TRAFFIC_SIGN_GO_STRAIGHT) && (angle_diff > 15.0))
//			continue;
//		double lane_prob = get_lane_prob(lane_pose, road_map);
//		if (lane_prob > max_lane_prob)
//		{
//			max_lane_prob = lane_prob;
//			rddf_pose.x = round(lane_pose.x / road_map->config.resolution) * road_map->config.resolution;
//			rddf_pose.y = round(lane_pose.y / road_map->config.resolution) * road_map->config.resolution;
//		}
//	}
//
//	return (rddf_pose);
//}


double
average_theta(carmen_ackerman_traj_point_t *poses, int curr_index, int num_poses_avg)
{
	double sum_theta_x = 0.0, sum_theta_y = 0.0;
	int num_poses = ((curr_index + 1) >= num_poses_avg) ? num_poses_avg : (curr_index + 1);

	for (int i = 0, index = curr_index; i < num_poses; i++, index--)
	{
		sum_theta_x += cos(poses[index].theta);
		sum_theta_y += sin(poses[index].theta);
	}

	if (sum_theta_x == 0.0 && sum_theta_y == 0.0)
		return (0.0);

	double avg_theta = atan2(sum_theta_y, sum_theta_x);

	return (avg_theta);
}


int
fill_in_poses_ahead_by_road_map(carmen_point_t initial_pose, carmen_map_p road_map,
		carmen_ackerman_traj_point_t *poses_ahead, int num_poses_desired)
{
	int num_poses = 0;
	double velocity = 10.0, phi = 0.0;
	int num_poses_avg = 5;

//	rddf_log = fopen("/home/rcarneiro/carmen_lcad/src/rddf/log.txt", "w");
//	g_num_pose = 0;

	carmen_point_t rddf_pose = find_nearest_pose_by_road_map(initial_pose, initial_pose, road_map, true);
	poses_ahead[0] = create_ackerman_traj_point_struct(rddf_pose.x, rddf_pose.y, velocity, phi, rddf_pose.theta);
	carmen_point_t previous_pose = rddf_pose;
	carmen_point_t rddf_pose_candidate = add_distance_to_pose(previous_pose, rddf_min_distance_between_waypoints);
	num_poses = 1;
	do
	{
//		g_num_pose = num_poses;
		rddf_pose = find_nearest_pose_by_road_map(previous_pose, rddf_pose_candidate, road_map, true);
		if (pose_out_of_map_coordinates(rddf_pose, road_map))
			break;

		rddf_pose.theta = atan2(rddf_pose.y - previous_pose.y, rddf_pose.x - previous_pose.x);
		poses_ahead[num_poses] = create_ackerman_traj_point_struct(rddf_pose.x, rddf_pose.y, velocity, phi, rddf_pose.theta);

		previous_pose = rddf_pose;
		previous_pose.theta = average_theta(poses_ahead, num_poses, num_poses_avg); // new approach
		rddf_pose_candidate = add_distance_to_pose(previous_pose, rddf_min_distance_between_waypoints);
		num_poses++;
	} while (num_poses < num_poses_desired);

//	for (int i = 0; i < num_poses; i++)
//		fprintf(rddf_log, "%d\t%lf\t%lf\t%lf\n", i, poses_ahead[i].x, poses_ahead[i].y, poses_ahead[i].theta);

	calculate_theta_ahead(poses_ahead, num_poses);
	calculate_phi_ahead(poses_ahead, num_poses);

	return (num_poses);
}


int
fill_in_poses_ahead_by_road_map(carmen_point_t initial_pose, carmen_map_p road_map,
		carmen_ackerman_traj_point_t *poses_ahead, int num_poses_desired, int aligned_pose_index)
{
	int num_poses = 0;
	double velocity = 10.0, phi = 0.0;
	int index_of_pose_with_good_theta = aligned_pose_index;

	carmen_point_t rddf_pose = find_nearest_pose_by_road_map(initial_pose, initial_pose, road_map, true);
	double distance_to_first_point = DIST2D(rddf_pose, poses_ahead[index_of_pose_with_good_theta]);
	poses_ahead[0] = create_ackerman_traj_point_struct(rddf_pose.x, rddf_pose.y, velocity, phi, rddf_pose.theta);
	carmen_point_t previous_pose = rddf_pose;
	carmen_point_t rddf_pose_candidate = add_distance_to_pose(previous_pose, distance_to_first_point);
	num_poses = 1;
	do
	{
		rddf_pose = find_nearest_pose_by_road_map(previous_pose, rddf_pose_candidate, road_map, true);
		if (pose_out_of_map_coordinates(rddf_pose, road_map))
			break;

		if (index_of_pose_with_good_theta < num_poses_desired - 1)
			rddf_pose.theta = poses_ahead[++index_of_pose_with_good_theta].theta;
		else
			rddf_pose.theta = atan2(rddf_pose.y - previous_pose.y, rddf_pose.x - previous_pose.x);
		poses_ahead[num_poses] = create_ackerman_traj_point_struct(rddf_pose.x, rddf_pose.y, velocity, phi, rddf_pose.theta);

		previous_pose = rddf_pose;
		rddf_pose_candidate = add_distance_to_pose(previous_pose, rddf_min_distance_between_waypoints);
		num_poses++;
	} while (num_poses < num_poses_desired);

	calculate_theta_ahead(poses_ahead, num_poses);
	calculate_phi_ahead(poses_ahead, num_poses);

	return (num_poses);
}


int
fill_in_poses_back_by_road_map(carmen_point_t initial_pose, carmen_map_p road_map,
		carmen_ackerman_traj_point_t *poses_back, int num_poses_desired)
{
	int num_poses = 0;
	double velocity = 10.0, phi = 0.0;
	int num_poses_avg = 5;

	poses_back[0] = create_ackerman_traj_point_struct(initial_pose.x, initial_pose.y, velocity, phi, initial_pose.theta);
	carmen_point_t previous_pose = initial_pose;
	carmen_point_t rddf_pose_candidate = add_distance_to_pose(previous_pose, -rddf_min_distance_between_waypoints);
	num_poses = 1;
	do
	{
//		g_num_pose = num_poses;
		carmen_point_t rddf_pose = find_nearest_pose_by_road_map(previous_pose, rddf_pose_candidate, road_map, false);
		if (pose_out_of_map_coordinates(rddf_pose, road_map))
			break;

		rddf_pose.theta = atan2(previous_pose.y - rddf_pose.y, previous_pose.x - rddf_pose.x);
		poses_back[num_poses] = create_ackerman_traj_point_struct(rddf_pose.x, rddf_pose.y, velocity, phi, rddf_pose.theta);

		previous_pose = rddf_pose;
		previous_pose.theta = average_theta(poses_back, num_poses, num_poses_avg); // new approach
		rddf_pose_candidate = add_distance_to_pose(previous_pose, -rddf_min_distance_between_waypoints);
		num_poses++;
	} while (num_poses < num_poses_desired);

//	for (int i = 0; i < num_poses; i++)
//		fprintf(rddf_log, "%d\t%lf\t%lf\t%lf\n", i, poses_back[i].x, poses_back[i].y, poses_back[i].theta);
//	fclose(rddf_log);

	calculate_theta_back(poses_back, num_poses);
	calculate_phi_back(poses_back, num_poses);

	return (num_poses);
}


int
find_aligned_pose_index(carmen_point_t initial_pose, carmen_ackerman_traj_point_t *poses_ahead, int num_poses_ahead)
{
	int nearest_pose_index = 0;
	double min_distance = DIST2D(initial_pose, poses_ahead[0]);
	for (int i = 0; i < num_poses_ahead; i++)
	{
		double distance = DIST2D(initial_pose, poses_ahead[i]);
		if (distance < min_distance)
		{
			min_distance = distance;
			nearest_pose_index = i;
		}
	}

	return (nearest_pose_index);
}


int
carmen_rddf_play_find_nearest_poses_by_road_map(carmen_point_t initial_pose, carmen_map_p road_map,
		carmen_ackerman_traj_point_t *poses_ahead, carmen_ackerman_traj_point_t *poses_back, int *num_poses_back, int num_poses_ahead_max)
{
	static bool first_time = true;
	static int num_poses_ahead;

	if (first_time)
		num_poses_ahead = fill_in_poses_ahead_by_road_map(initial_pose, road_map, poses_ahead, num_poses_ahead_max);
	else
	{
		int aligned_pose_index = find_aligned_pose_index(initial_pose, poses_ahead, num_poses_ahead);
		num_poses_ahead = fill_in_poses_ahead_by_road_map(initial_pose, road_map, poses_ahead, num_poses_ahead_max, aligned_pose_index);
	}

	initial_pose.x = poses_ahead[0].x;
	initial_pose.y = poses_ahead[0].y;
	initial_pose.theta = poses_ahead[0].theta;
	(*num_poses_back) = fill_in_poses_back_by_road_map(initial_pose, road_map, poses_back, num_poses_ahead_max / 3);
	poses_back[0].phi = poses_ahead[0].phi;

	if (debug)
		plot_state(poses_ahead, num_poses_ahead, poses_back, *num_poses_back, false);

	smooth_rddf_using_conjugate_gradient(poses_ahead, num_poses_ahead, poses_back, *num_poses_back);

	if (debug)
		plot_state(poses_ahead, num_poses_ahead, poses_back, *num_poses_back, true);

//	first_time = false;

	return (num_poses_ahead);
}


///////////////////////////////////////////////////////////////////////////////////////////////



///////////////////////////////////////////////////////////////////////////////////////////////
//                                                                                           //
// Publishers                                                                                //
//                                                                                           //
///////////////////////////////////////////////////////////////////////////////////////////////


void
carmen_rddf_play_publish_annotation_queue()
{
	IPC_RETURN_TYPE err;

	if (annotations_to_publish.size() == 0)
	{
		annotation_and_index annotation_i;
		memset(&annotation_i, 0, sizeof(annotation_and_index));
		carmen_annotation_t &annotation = annotation_i.annotation;
		annotation.annotation_type = RDDF_ANNOTATION_TYPE_NONE;
		annotation.annotation_code = RDDF_ANNOTATION_CODE_NONE;
		annotations_to_publish.push_back(annotation_i);
	}

	if (annotation_queue_message.annotations == NULL)
	{
		annotation_queue_message.annotations = (carmen_annotation_t *) calloc(annotations_to_publish.size(), sizeof(carmen_annotation_t));

		if (!annotation_queue_message.annotations)
			exit(printf("Allocation error in carmen_rddf_play_publish_annotation_queue()::1\n"));
	}
	else if (annotation_queue_message.num_annotations != (int) annotations_to_publish.size())
	{
		annotation_queue_message.annotations = (carmen_annotation_t *) realloc(annotation_queue_message.annotations, annotations_to_publish.size() * sizeof(carmen_annotation_t));

		if (!annotation_queue_message.annotations)
			exit(printf("Allocation error in carmen_rddf_play_publish_annotation_queue()::2\n"));
	}

	annotation_queue_message.num_annotations = annotations_to_publish.size();

	for (size_t i = 0; i < annotations_to_publish.size(); i++)
		memcpy(&(annotation_queue_message.annotations[i]), &(annotations_to_publish[i].annotation), sizeof(carmen_annotation_t));

	annotation_queue_message.host = carmen_get_host();
	annotation_queue_message.timestamp = carmen_get_time();

	err = IPC_publishData(CARMEN_RDDF_ANNOTATION_MESSAGE_NAME, &annotation_queue_message);
	carmen_test_ipc_exit(err, "Could not publish", CARMEN_RDDF_ANNOTATION_MESSAGE_FMT);
}


static void
carmen_rddf_play_publish_rddf_and_annotations(carmen_point_t robot_pose)
{
	// so publica rddfs quando a pose do robo ja estiver setada
	if ((carmen_rddf_num_poses_ahead > 0) && (carmen_rddf_num_poses_back > 0))
	{
		clear_annotations();
		set_annotations(robot_pose);

		carmen_rddf_publish_road_profile_message(
			carmen_rddf_poses_ahead,
			carmen_rddf_poses_back,
			carmen_rddf_num_poses_ahead,
			carmen_rddf_num_poses_back,
			annotations,
			annotations_codes);

		carmen_rddf_play_publish_annotation_queue();
	}
}


void
carmen_rddf_publish_road_profile_around_end_point(carmen_ackerman_traj_point_t *poses_around_end_point, int num_poses_acquired)
{
	carmen_rddf_publish_road_profile_around_end_point_message(poses_around_end_point, num_poses_acquired);
}
///////////////////////////////////////////////////////////////////////////////////////////////


void
carmen_rddf_play_find_and_publish_poses_around_end_point(double x, double y, double yaw, int num_poses_desired, double timestamp)
{
	int num_poses_acquired = 0;
	carmen_ackerman_traj_point_t *poses_around_end_point;

	poses_around_end_point = (carmen_ackerman_traj_point_t *) calloc (num_poses_desired, sizeof(carmen_ackerman_traj_point_t));
	carmen_test_alloc(poses_around_end_point);

	num_poses_acquired = carmen_find_poses_around(x, y, yaw, timestamp, poses_around_end_point, num_poses_desired);
	carmen_rddf_publish_road_profile_around_end_point(poses_around_end_point, num_poses_acquired);

	free(poses_around_end_point);
}

///////////////////////////////////////////////////////////////////////////////////////////////



///////////////////////////////////////////////////////////////////////////////////////////////
//                                                                                           //
// Handlers                                                                                  //
//                                                                                           //
///////////////////////////////////////////////////////////////////////////////////////////////


static void
localize_globalpos_handler(carmen_localize_ackerman_globalpos_message *msg)
{
	carmen_point_t robot_pose = msg->globalpos;
	carmen_rddf_pose_initialized = 1;

	if (use_road_map)
	{
		current_globalpos_msg = msg;
		robot_pose_queued = (current_road_map == NULL || pose_out_of_map_coordinates(robot_pose, current_road_map));
		if (robot_pose_queued)
			return;
		carmen_rddf_num_poses_ahead = carmen_rddf_play_find_nearest_poses_by_road_map(
				robot_pose,
				current_road_map,
				carmen_rddf_poses_ahead,
				carmen_rddf_poses_back,
				&carmen_rddf_num_poses_back,
				carmen_rddf_num_poses_ahead_max);
	}
	else
	{
		carmen_rddf_num_poses_ahead = carmen_rddf_play_find_nearest_poses_ahead(
				robot_pose.x,
				robot_pose.y,
				robot_pose.theta,
				msg->timestamp,
				carmen_rddf_poses_ahead,
				carmen_rddf_poses_back,
				&carmen_rddf_num_poses_back,
				carmen_rddf_num_poses_ahead_max,
				annotations);
	}

	annotations_to_publish.clear();
	carmen_check_for_annotations(robot_pose, carmen_rddf_poses_ahead, carmen_rddf_poses_back,
			carmen_rddf_num_poses_ahead, carmen_rddf_num_poses_back, msg->timestamp);

	carmen_rddf_play_publish_rddf_and_annotations(robot_pose);
}


static void
simulator_ackerman_truepos_message_handler(carmen_simulator_ackerman_truepos_message *msg)
{
	carmen_point_t robot_pose = msg->truepose;
	carmen_rddf_pose_initialized = 1;

	if (use_road_map)
	{
		current_truepos_msg = msg;
		robot_pose_queued = (current_road_map == NULL || pose_out_of_map_coordinates(robot_pose, current_road_map));
		if (robot_pose_queued)
			return;
		carmen_rddf_num_poses_ahead = carmen_rddf_play_find_nearest_poses_by_road_map(
				robot_pose,
				current_road_map,
				carmen_rddf_poses_ahead,
				carmen_rddf_poses_back,
				&carmen_rddf_num_poses_back,
				carmen_rddf_num_poses_ahead_max);
	}
	else
	{
		carmen_rddf_num_poses_ahead = carmen_rddf_play_find_nearest_poses_ahead(
				robot_pose.x,
				robot_pose.y,
				robot_pose.theta,
				msg->timestamp,
				carmen_rddf_poses_ahead,
				carmen_rddf_poses_back,
				&carmen_rddf_num_poses_back,
				carmen_rddf_num_poses_ahead_max,
				annotations);
	}

	annotations_to_publish.clear();
	carmen_check_for_annotations(robot_pose, carmen_rddf_poses_ahead, carmen_rddf_poses_back,
			carmen_rddf_num_poses_ahead, carmen_rddf_num_poses_back, msg->timestamp);

	carmen_rddf_play_publish_rddf_and_annotations(robot_pose);
}


static void
road_map_handler(carmen_map_server_road_map_message *msg)
{
	static bool first_time = true;

	if (first_time)
	{
		current_road_map = (carmen_map_p) calloc (1, sizeof(carmen_map_t));
		carmen_grid_mapping_initialize_map(current_road_map, msg->config.x_size, msg->config.resolution, 'r');
		first_time = false;
	}

	if (msg->config.x_origin != current_road_map->config.x_origin || msg->config.y_origin != current_road_map->config.y_origin) // new map
	{
		memcpy(current_road_map->complete_map, msg->complete_map, sizeof(double) * msg->size);
		current_road_map->config = msg->config;

		if (robot_pose_queued)
		{
			if (use_truepos)
				simulator_ackerman_truepos_message_handler(current_truepos_msg);
			else
				localize_globalpos_handler(current_globalpos_msg);
		}
	}
}


//static void
//carmen_rddf_play_nearest_waypoint_message_handler(carmen_rddf_nearest_waypoint_message *rddf_nearest_waypoint_message)
//{
//	carmen_rddf_nearest_waypoint_to_end_point = rddf_nearest_waypoint_message->point;
//	carmen_rddf_nearest_waypoint_is_set = 1;
//
//	carmen_rddf_publish_nearest_waypoint_confirmation_message(rddf_nearest_waypoint_message->point);
//	already_reached_nearest_waypoint_to_end_point = 0;
//}


void
carmen_rddf_play_end_point_message_handler(carmen_rddf_end_point_message *rddf_end_point_message)
{
	if (rddf_end_point_message->number_of_poses > 1)
	{
		carmen_rddf_play_find_and_publish_poses_around_end_point(
				rddf_end_point_message->point.x,
				rddf_end_point_message->point.y,
				rddf_end_point_message->point.theta,
				rddf_end_point_message->number_of_poses,
				rddf_end_point_message->timestamp
		);

		carmen_rddf_end_point = rddf_end_point_message->point;
		carmen_rddf_end_point_is_set = 1;
	}
	else
	{
		carmen_rddf_play_find_and_publish_poses_around_end_point(
				rddf_end_point_message->point.x,
				rddf_end_point_message->point.y,
				rddf_end_point_message->point.theta,
				rddf_end_point_message->number_of_poses,
				rddf_end_point_message->timestamp
		);

		carmen_rddf_nearest_waypoint_to_end_point = rddf_end_point_message->point;
		carmen_rddf_nearest_waypoint_is_set = 1;

		already_reached_nearest_waypoint_to_end_point = 0;
	}
}


static void
carmen_traffic_light_message_handler(carmen_traffic_light_message *message)
{
	traffic_lights = message;
}


static void
carmen_rddf_dynamic_annotation_message_handler(carmen_rddf_dynamic_annotation_message *message)
{
	// Avoid reinserting the same annotation over and over.
	for (size_t i = 0; i < dynamic_annotation_messages.size(); i++)
	{
		carmen_rddf_dynamic_annotation_message &stored = dynamic_annotation_messages[i];
		if (stored.annotation_type == message->annotation_type &&
			stored.annotation_code == message->annotation_code &&
			DIST2D(stored.annotation_point, message->annotation_point) < 0.5)
		{
			stored = *message;
			return;
		}
	}

	dynamic_annotation_messages.push_front(*message);
	if (dynamic_annotation_messages.size() > 30)
		dynamic_annotation_messages.pop_back();
}


void
carmen_moving_objects_point_clouds_message_handler(carmen_moving_objects_point_clouds_message *moving_objects_point_clouds_message)
{
	moving_objects = moving_objects_point_clouds_message;
}
///////////////////////////////////////////////////////////////////////////////////////////////



//////////////////////////////////////////////////////////////////////////////////////////////////
//                                                                                              //
// Initializations                                                                              //
//                                                                                              //
//////////////////////////////////////////////////////////////////////////////////////////////////


void
carmen_rddf_play_subscribe_messages()
{
	if (!use_truepos)
		carmen_localize_ackerman_subscribe_globalpos_message(NULL, (carmen_handler_t) localize_globalpos_handler, CARMEN_SUBSCRIBE_LATEST);
	else
		carmen_simulator_ackerman_subscribe_truepos_message(NULL, (carmen_handler_t) simulator_ackerman_truepos_message_handler, CARMEN_SUBSCRIBE_LATEST);

	if (use_road_map)
		carmen_map_server_subscribe_road_map(NULL, (carmen_handler_t) road_map_handler, CARMEN_SUBSCRIBE_LATEST);

//	carmen_rddf_subscribe_nearest_waypoint_message(NULL,
//			(carmen_handler_t) carmen_rddf_play_nearest_waypoint_message_handler,
//			CARMEN_SUBSCRIBE_LATEST);

	carmen_rddf_subscribe_end_point_message(NULL,
			(carmen_handler_t) carmen_rddf_play_end_point_message_handler,
			CARMEN_SUBSCRIBE_LATEST);

    carmen_traffic_light_subscribe(traffic_lights_camera, NULL, (carmen_handler_t) carmen_traffic_light_message_handler, CARMEN_SUBSCRIBE_LATEST);

    carmen_rddf_subscribe_dynamic_annotation_message(NULL, (carmen_handler_t) carmen_rddf_dynamic_annotation_message_handler, CARMEN_SUBSCRIBE_LATEST);

	carmen_moving_objects_point_clouds_subscribe_message(NULL, (carmen_handler_t) carmen_moving_objects_point_clouds_message_handler, CARMEN_SUBSCRIBE_LATEST);

}


static void
carmen_rddf_play_load_index(char *rddf_filename)
{
	int annotation = 0;
	carmen_fused_odometry_message message;
	placemark_vector_t placemark_vector;

	if (!carmen_rddf_index_exists(rddf_filename))
	{
		if (strcmp(rddf_filename + (strlen(rddf_filename) - 3), "kml") == 0)
		{
			carmen_rddf_play_open_kml(rddf_filename, &placemark_vector);

			for (unsigned int i = 0; i < placemark_vector.size(); i++)
			{
				if (carmen_rddf_play_copy_kml(placemark_vector[i], &message, &annotation))
					carmen_rddf_index_add(&message, 0, 0, annotation);
			}

			carmen_rddf_index_save(rddf_filename);
		}
		else
		{
			FILE *fptr = fopen(rddf_filename, "r");

			while (!feof(fptr))
			{
				memset(&message, 0, sizeof(message));

				fscanf(fptr, "%lf %lf %lf %lf %lf %lf\n",
					&(message.pose.position.x), &(message.pose.position.y),
					&(message.pose.orientation.yaw), &(message.velocity.x), &(message.phi),
					&(message.timestamp));

				carmen_rddf_index_add(&message, 0, 0, 0);
			}

			carmen_rddf_index_save(rddf_filename);
			fclose(fptr);
		}
	}

	carmen_rddf_load_index(rddf_filename);
}


void
carmen_rddf_play_load_annotation_file()
{
	if (carmen_annotation_filename == NULL)
		return;

	FILE *f = fopen(carmen_annotation_filename, "r");
	char line[1024];
	while(fgets(line, 1023, f) != NULL)
	{
		if (line[0] == '#') // comment line
			continue;

		carmen_annotation_t annotation;
		char annotation_description[1024];
		if (sscanf(line, "%s %d %d %lf %lf %lf %lf\n",
				annotation_description,
				&annotation.annotation_type,
				&annotation.annotation_code,
				&annotation.annotation_orientation,
				&annotation.annotation_point.x,
				&annotation.annotation_point.y,
				&annotation.annotation_point.z) == 7)
		{
			annotation.annotation_description = (char *) calloc (1024, sizeof(char));
			strcpy(annotation.annotation_description, annotation_description);
//			printf("%s\t%d\t%d\t%lf\t%lf\t%lf\t%lf\n", annotation.annotation_description,
//					annotation.annotation_type,
//					annotation.annotation_code,
//					annotation.annotation_orientation,
//					annotation.annotation_point.x,
//					annotation.annotation_point.y,
//					annotation.annotation_point.z);

			carmen_ackerman_traj_point_t annotation_point;
			annotation_point.x = annotation.annotation_point.x;
			annotation_point.y = annotation.annotation_point.y;
			annotation_point.theta = annotation.annotation_orientation;
			double distance_car_pose_car_front = distance_between_front_and_rear_axles + distance_between_front_car_and_front_wheels;
			carmen_point_t new_annotation_point = carmen_collision_detection_displace_car_pose_according_to_car_orientation(&annotation_point, -distance_car_pose_car_front);

			annotation.annotation_point.x = new_annotation_point.x;
			annotation.annotation_point.y = new_annotation_point.y;
			annotation_read_from_file.push_back(annotation);
		}
	}

	fclose(f);
}


static void
carmen_rddf_play_get_parameters(int argc, char** argv)
{
	carmen_param_t param_list[] =
	{
		{(char *) "robot", (char *) "distance_between_front_and_rear_axles", CARMEN_PARAM_DOUBLE, &distance_between_front_and_rear_axles, 1, NULL},
		{(char *) "robot", (char *) "distance_between_front_car_and_front_wheels", CARMEN_PARAM_DOUBLE, &distance_between_front_car_and_front_wheels, 1, NULL},
		{(char *) "rddf", (char *) "num_poses_ahead", CARMEN_PARAM_INT, &carmen_rddf_num_poses_ahead_max, 0, NULL},
		{(char *) "rddf", (char *) "min_distance_between_waypoints", CARMEN_PARAM_DOUBLE, &rddf_min_distance_between_waypoints, 0, NULL},
		{(char *) "rddf", (char *) "loop", CARMEN_PARAM_ONOFF, &carmen_rddf_perform_loop, 0, NULL},
		{(char *) "behavior_selector", (char *) "use_truepos", CARMEN_PARAM_ONOFF, &use_truepos, 0, NULL},
		{(char *) "road_mapper", (char *) "kernel_size", CARMEN_PARAM_INT, &road_mapper_kernel_size, 0, NULL},
	};

	int num_items = sizeof(param_list) / sizeof(param_list[0]);
	carmen_param_install_params(argc, argv, param_list, num_items);
}


void
carmen_rddf_play_initialize(void)
{
	carmen_rddf_poses_ahead = (carmen_ackerman_traj_point_t *) calloc (carmen_rddf_num_poses_ahead_max, sizeof(carmen_ackerman_traj_point_t));
	carmen_rddf_poses_back = (carmen_ackerman_traj_point_t *) calloc (carmen_rddf_num_poses_ahead_max, sizeof(carmen_ackerman_traj_point_t));
	annotations = (int *) calloc (carmen_rddf_num_poses_ahead_max, sizeof(int));
	annotations_codes = (int *) calloc (carmen_rddf_num_poses_ahead_max, sizeof(int));
	memset(&annotation_queue_message, 0, sizeof(annotation_queue_message));

	carmen_test_alloc(carmen_rddf_poses_ahead);
	carmen_test_alloc(annotations);

	// publish rddf at a rate of 10Hz
	// carmen_ipc_addPeriodicTimer(1.0 / 10.0, (TIMER_HANDLER_TYPE) carmen_rddf_play_publish_rddf, NULL);
}


int
main(int argc, char **argv)
{
	setlocale(LC_ALL, "C");
	char *usage[] = {(char *) "<rddf_filename> [<annotation_filename> [<traffic_lights_camera>]]",
			         (char *) "-use_road_map   [<annotation_filename> [<traffic_lights_camera>]]"};

	if (argc >= 2 && strcmp(argv[1], "-h") == 0)
	{
		printf("\nUsage 1: %s %s\nUsage 2: %s %s\n\nCarmen parameters:\n", argv[0], usage[0], argv[0], usage[1]);
		carmen_rddf_play_get_parameters(argc, argv); // display help and exit
	}
	if (argc < 2 || argc > 4)
		exit(printf("Error: Usage 1: %s %s\n       Usage 2: %s %s\n", argv[0], usage[0], argv[0], usage[1]));

	if (strcmp(argv[1], "-use_road_map") == 0)
	{
		use_road_map = true;
		printf("Road map option set.\n");
	}
	else
	{
		carmen_rddf_filename = argv[1];
		printf("RDDF filename: %s.\n", carmen_rddf_filename);
	}

	if (argc >= 3)
		carmen_annotation_filename = argv[2];
	if (carmen_annotation_filename)
		printf("Annotation filename: %s.\n", carmen_annotation_filename);

	if (argc >= 4)
		traffic_lights_camera = atoi(argv[3]);
	printf("Traffic lights camera: %d.\n", traffic_lights_camera);

	carmen_ipc_initialize(argc, argv);
	carmen_param_check_version(argv[0]);
	carmen_rddf_play_get_parameters(argc, argv);
	carmen_rddf_play_initialize();
	carmen_rddf_define_messages();
	carmen_rddf_play_subscribe_messages();
	if (!use_road_map)
		carmen_rddf_play_load_index(carmen_rddf_filename);
	carmen_rddf_play_load_annotation_file();
	signal (SIGINT, carmen_rddf_play_shutdown_module);
	carmen_ipc_dispatch();

	return (0);
}
