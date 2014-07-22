#include "trajectory_library.h"

#define STUB ROS_INFO("LINE %d", __LINE__)

TrajectoryLibrary::TrajectoryLibrary(ros::NodeHandle nh)
{
    /* Load up robot model */
    ROS_INFO("Loading ur5 robot model.");
    _rmodel_loader.reset(new robot_model_loader::RobotModelLoader("robot_description"));
    _rmodel = _rmodel_loader->getModel();

    ROS_INFO("Loaded model %s.", _rmodel->getName().c_str());

    ROS_INFO("Grabbing JointModelGroup.");
    _jmg = _rmodel->getJointModelGroup(UR5_GROUP_NAME);

    /* Init planning scene */
    ROS_INFO("Initializing PlanningScene from RobotModel");
    _plan_scene = planning_scene::PlanningScenePtr(new planning_scene::PlanningScene(_rmodel));

    /* Load the planner */
    ROS_INFO("Loading the planner plugin.");
    boost::scoped_ptr<pluginlib::ClassLoader<planning_interface::PlannerManager> > planner_plugin_loader;
    try
    {
        planner_plugin_loader.reset(new pluginlib::ClassLoader<planning_interface::PlannerManager>("moveit_core", "planning_interface::PlannerManager"));
    }
    catch(pluginlib::PluginlibException& ex)
    {
        ROS_FATAL_STREAM("Exception while creating planning plugin loader " << ex.what());
    }
    try
    {
        _planner.reset(planner_plugin_loader->createUnmanagedInstance("ompl_interface/OMPLPlanner"));
        _planner->initialize(_rmodel, nh.getNamespace());
    }
    catch(pluginlib::PluginlibException& ex)
    {
        const std::vector<std::string> &classes = planner_plugin_loader->getDeclaredClasses();
        std::stringstream ss;
        for (std::size_t i = 0 ; i < classes.size() ; ++i)
        ss << classes[i] << " ";
        ROS_ERROR_STREAM("Exception while loading planner: " << ex.what() << std::endl
                       << "Available plugins: " << ss.str());
    }

    // Initialize time parameterizer
    _time_parametizer.reset(new trajectory_processing::IterativeParabolicTimeParameterization());

    // Create publisher for rviz
    _trajectory_publisher = nh.advertise<moveit_msgs::DisplayTrajectory>("/move_group/display_planned_path", 1, true);
    _plan_scene_publisher = nh.advertise<moveit_msgs::PlanningScene>("/move_group/monitored_planning_scene", 1, true);

    return;
}

void TrajectoryLibrary::initWorld()
{
    collision_detection::WorldPtr world = _plan_scene->getWorldNonConst();
    collision_detection::AllowedCollisionMatrix acm = _plan_scene->getAllowedCollisionMatrixNonConst();

    // Define workspace limits with planes
    shapes::ShapeConstPtr ground_plane(new shapes::Plane(0,0,1,0));
    geometry_msgs::Pose pose;
    pose.orientation.w = 1;
    pose.orientation.x = 0;
    pose.orientation.y = 0;
    pose.orientation.z = 0;
    pose.position.x = 0;
    pose.position.y = 0;
    pose.position.z = 0;
    Eigen::Affine3d eigen_pose;
    tf::poseMsgToEigen(pose, eigen_pose);
    world->addToObject("workspace_bounds", ground_plane, eigen_pose);
    acm.setEntry("workspace_bounds", "world", true);
    acm.setEntry("workspace_bounds", "base_link", true);

//    // Add obstructo-sphere in middle of workspace
//    shapes::ShapeConstPtr obstructo(new shapes::Sphere(0.2));
//    pose.position.x = 0;
//    pose.position.y = 0;
//    pose.position.z = 0.7;
//    tf::poseMsgToEigen(pose, eigen_pose);
//    world->addToObject("obstructo_sphere", obstructo, eigen_pose);
//    acm.setEntry("obstructo_sphere", "world", true);
//    acm.setEntry("obstructo_sphere", "base_link", true);
//    acm.setEntry("obstructo_sphere", "shoulder_link", true);

    // Publish updated planning scene
    acm.print(std::cout);
    moveit_msgs::PlanningScene scene_msg;
    _plan_scene->getPlanningSceneMsg(scene_msg);
    _plan_scene_publisher.publish(scene_msg);

    return;
}

std::size_t TrajectoryLibrary::gridLinspace(std::vector<joint_values_t>& jvals, rect_grid& grid)
{
    double di, dj, dk;
    di = dj = dk = 0.0;
    int num_poses;

    // Calculate grid spacings if xres != 1
    if (grid.xres != 1) {
        di = (grid.xlim_high - grid.xlim_low)/(grid.xres-1);
    }
    if (grid.yres != 1) {
        dj = (grid.ylim_high - grid.ylim_low)/(grid.yres-1);
    }
    if (grid.zres != 1) {
        dk = (grid.zlim_high - grid.zlim_low)/(grid.zres-1);
    }

    // Reserve ahead of time the number of poses for speed
    num_poses = grid.xres * grid.yres * grid.zres;
    jvals.reserve(num_poses);
    ROS_INFO("Attempting to generate %d targets.", num_poses);

    geometry_msgs::Pose geo_pose;
    robot_state::RobotStatePtr state(new robot_state::RobotState(_rmodel));

    // Linspace
    int n = -1;
    for (int i = 0; i < grid.xres; i++)
    {
        for (int j = 0; j < grid.yres; j++)
        {
            for (int k = 0; k < grid.zres; k++)
            {
                n++;
                geo_pose.position.x = grid.xlim_low + i*di;
                geo_pose.position.y = grid.ylim_low + j*dj;
                geo_pose.position.z = grid.zlim_low + k*dk;
                geo_pose.orientation = grid.orientation;
                bool ik_success;
                // Do IK
                ik_success = state->setFromIK(_jmg, geo_pose, 10, 0.1, boost::bind(&TrajectoryLibrary::ikValidityCallback, this, _1, _2, _3));
                if (!ik_success)
                {
                    ROS_WARN("Could not solve IK for pose %d: Skipping.", n);
                    printPose(geo_pose);
                    continue;
                }
                // If IK succeeded
                joint_values_t j;
                state->copyJointGroupPositions(_jmg, j);
                // Add to vector
                ROS_INFO("Successfully generated joint values for pose %d.", n);
                jvals.push_back(j);
            }
        }
    }
    return jvals.size();
}

bool TrajectoryLibrary::ikValidityCallback(robot_state::RobotState* p_state, const robot_model::JointModelGroup* p_jmg, const double* jvals)
{
    // Check for self-collisions
    p_state->setJointGroupPositions(p_jmg, jvals);
    p_state->update(true);
    return _plan_scene->isStateValid(*p_state, p_jmg->getName());
}

void TrajectoryLibrary::generateJvals(rect_grid& pick_grid, rect_grid& place_grid)
{
    _pick_grid = pick_grid;
    ROS_INFO("Generating pick joint values.");
    _num_pick_targets = gridLinspace(_pick_jvals, _pick_grid);

    _place_grid = place_grid;
    ROS_INFO("Generating place joint values.");
    _num_place_targets = gridLinspace(_place_jvals, _place_grid);

    ROS_INFO("Generated %d of %d possible pick targets.", (int) _num_pick_targets, pick_grid.xres * pick_grid.yres * pick_grid.zres);
    ROS_INFO("Genereted %d of %d possible place targets.", (int) _num_place_targets, place_grid.xres * place_grid.yres * place_grid.zres);
    return;
}

void TrajectoryLibrary::printPose(const geometry_msgs::Pose& pose)
{
    printf("     Position (%f, %f, %f).\n", pose.position.x, pose.position.y, pose.position.z);
    printf("     Orientation (%f, %f, %f, %f).\n", pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z);
    return;
}

void TrajectoryLibrary::printJointValues(const joint_values_t& jvals)
{
    printf("Joint values: ");
    for (int i=0; i<jvals.size(); i++)
    {
        printf(" %f", jvals[i]);
    }
    printf("\n");
    return;
}

bool TrajectoryLibrary::planTrajectory(ur5_motion_plan& plan, std::vector<moveit_msgs::Constraints> constraints)
{
    planning_interface::MotionPlanRequest req;
    planning_interface::MotionPlanResponse res;
    req.group_name = UR5_GROUP_NAME;

    // Add constraints
    req.goal_constraints = constraints;
    req.planner_id = "manipulator[RRTConnectkConfigDefault]";
    //req.planner_id = "manipulator[RRTstarkConfigDefault]";
    req.allowed_planning_time = 5.0;

    // Define workspace
    req.workspace_parameters.max_corner.x = 1.0;
    req.workspace_parameters.max_corner.y = 1.0;
    req.workspace_parameters.max_corner.z = 0.7;
    req.workspace_parameters.min_corner.x = -1.0;
    req.workspace_parameters.min_corner.y = -1.0;
    req.workspace_parameters.min_corner.z = 0.25;

    // Now prepare the planning context
    int tries = 0;
    while (tries < 3)
    {
        planning_interface::PlanningContextPtr context = _planner->getPlanningContext(_plan_scene, req, res.error_code_);
        context->solve(res);
        if (res.error_code_.val == res.error_code_.SUCCESS)
        {
            robot_trajectory::RobotTrajectoryPtr traj(res.trajectory_);

            // Do optimization
            robot_trajectory::RobotTrajectoryPtr traj_opt(new robot_trajectory::RobotTrajectory(_rmodel, UR5_GROUP_NAME));
            optimizeTrajectory(traj_opt, traj);

            // Do time parameterization again
            _time_parametizer->computeTimeStamps(*traj_opt);
            // Now slow it down for safety
            timeWarpTrajectory(traj_opt, 3);

            // Pack motion plan struct
            moveit::core::robotStateToRobotStateMsg(_plan_scene->getCurrentState(), plan.start_state);
            moveit::core::robotStateToRobotStateMsg(traj_opt->getLastWayPoint(), plan.end_state);
            plan.num_wpts = traj_opt->getWayPointCount();
            plan.duration = traj_opt->getWaypointDurationFromStart(plan.num_wpts-1);
            traj_opt->getRobotTrajectoryMsg(plan.trajectory);

            if (!_plan_scene->isPathValid(plan.start_state, plan.trajectory, UR5_GROUP_NAME, false))
            {
                ROS_ERROR("Path invalid.");
                tries++;
                continue;
            }

            ROS_INFO("Duration = %f.", plan.duration);
            return true;
        }
        // else planner failed
        tries++;
    }
    return false;
}

void TrajectoryLibrary::timeWarpTrajectory(robot_trajectory::RobotTrajectoryPtr traj, double slow_factor)
{
    std::size_t wpt_count = traj->getWayPointCount();
    for (std::size_t i = 1; i < wpt_count; i++)
    {
        double t = traj->getWayPointDurationFromPrevious(i);
        t = slow_factor*t;
        traj->setWayPointDurationFromPrevious(i, t);
    }
    return;
}

void TrajectoryLibrary::optimizeTrajectory(robot_trajectory::RobotTrajectoryPtr traj_opt, const robot_trajectory::RobotTrajectoryPtr traj)
{
    // Make a copy
    *traj_opt = *traj;

    std::size_t wpt_count = traj->getWayPointCount();
    ROS_INFO("Optimize starts with %d waypoints.", (int) wpt_count);

    // Iterate forwards from the first waypoint
    for (std::size_t i=0; i < wpt_count; i++)
    {
        // Do time parameterization
        _time_parametizer->computeTimeStamps(*traj_opt);

        // Check for shortcut to other waypoint
        // Starting from the end and going backwards
        for (std::size_t j=(wpt_count-1); j > i; j--)
        {
            // Define shortcut as straight line in joint space between waypoints i and j
            robot_state::RobotState wpt_i = traj_opt->getWayPoint(i);
            robot_state::RobotState wpt_j = traj_opt->getWayPoint(j);

            // Generate intermediate states and check each for collisions
            double duration = traj_opt->getWaypointDurationFromStart(j) - traj_opt->getWaypointDurationFromStart(i);
            std::size_t step_count = duration / DT_LOCAL_COLLISION_CHECK;
            double step_scaled;
            robot_state::RobotState step_state(_rmodel);
            bool shortcut_invalid = false;

            for (std::size_t s=1; s < step_count; s++)
            {
                step_scaled = s / ((float) step_count);
                // Calculate intermediate state at step s
                wpt_i.interpolate(wpt_j, step_scaled, step_state);
                step_state.update(true);
                // Check state for collision
                if (!_plan_scene->isStateValid(step_state, UR5_GROUP_NAME, true) )
                {
                    // ROS_INFO("Collision found at %f of the way between waypoints %d and %d.", step_scaled, (int) i, (int) j);
                    shortcut_invalid = true;
                    break;
                }
            }

            // If the shortcut is valid
            if (!shortcut_invalid)
            {
                //ROS_INFO("Shortcut found between nodes %d and %d.", (int) i, (int) j);
                // Create new trajectory object with only neccessary endpoints
                robot_trajectory::RobotTrajectory temp_traj(_rmodel, UR5_GROUP_NAME);
                // Copy in waypoints before the shortcut
                for (std::size_t n=0; n <= i; n++)
                {
                    robot_state::RobotStatePtr state = traj_opt->getWayPointPtr(n);
                    temp_traj.insertWayPoint(n, state, 0.0);
                }
                // Copy in waypoints after the shortcut
                for (std::size_t n=j; n < wpt_count; n++)
                {
                    robot_state::RobotStatePtr state = traj_opt->getWayPointPtr(n);
                    temp_traj.insertWayPoint(n - (j-i-1), state, 0.0);
                }
                ROS_ASSERT(temp_traj.getWayPointCount() == (wpt_count - (j-i-1)));

                // Copy temporary trajectory
                *traj_opt = temp_traj;
                wpt_count = traj_opt->getWayPointCount();
                break; // break out of j loop
            }

            // Otherwise shortcut is invalid, so move on
        }
    }

    ROS_INFO("Successfully trimmed %d nodes.", (int) (traj->getWayPointCount() - traj_opt->getWayPointCount()) );
    return;
}

moveit_msgs::Constraints TrajectoryLibrary::genPoseConstraint(geometry_msgs::Pose pose_goal)
{
    geometry_msgs::PoseStamped pose_pkt;
    pose_pkt.header.frame_id = "world";
    pose_pkt.pose = pose_goal;
    // Position and orientation tolerances
    std::vector<double> tolerance_pose(3, 0.01);
    std::vector<double> tolerance_angle(3, 0.01);

    // Create constraint from pose using IK
    return kinematic_constraints::constructGoalConstraints("ee_link", pose_pkt, tolerance_pose, tolerance_angle);
}

moveit_msgs::Constraints TrajectoryLibrary::genJointValueConstraint(joint_values_t jvals)
{
    // Initialize state variable with joint values
    robot_state::RobotState state(_rmodel);
    state.setJointGroupPositions(UR5_GROUP_NAME, jvals);
    // Verify joint values are within bounds
    if (!_jmg->satisfiesPositionBounds( jvals.data() ) )
    {
        ROS_ERROR("Joint value constraint does not satisfy position bounds.");
    }
    // Create constraint
    return kinematic_constraints::constructGoalConstraints(state, _jmg, 0.01);
}

int TrajectoryLibrary::build()
{
    /* Check that target grids have been generated */
    if (_num_pick_targets == 0 || _num_place_targets == 0)
    {
        ROS_ERROR("No pick or place targets defined. Cannot build library.");
        return 0;
    }

    _num_trajects = 0;

    /* Iterate through place targets */
    for (int n = 0; n < _num_place_targets; n++)
    {
        //sleep(5);
        // Solve IK for place position n
        ROS_INFO("Jumping to place pose %d", n);
        robot_state::RobotState place_state(_rmodel);
        place_state.setJointGroupPositions(UR5_GROUP_NAME, _place_jvals[n]);
        // Jump current state to place pose
        _plan_scene->setCurrentState(place_state);
        // Publish planning scene
        moveit_msgs::PlanningScene scene_msg;
        _plan_scene->getPlanningSceneDiffMsg(scene_msg);
        _plan_scene_publisher.publish(scene_msg);

        /* Iterate through pick targets */
        for (int m = 0; m < _num_pick_targets; m++) {

            ////////////////// Pick target

            // Assume we are at a place pose
            // Set a pick target
            std::vector<moveit_msgs::Constraints> v_constraints;
            v_constraints.push_back(genJointValueConstraint(_pick_jvals[m]));
            // Generate trajectory
            ur5_motion_plan pick_traj;
            bool success = planTrajectory(pick_traj, v_constraints);
            if (!success)
            {
                ROS_ERROR("Planner failed to generate plan for pick target %d. Skipping.", m);
                continue;
            }

            // Now change state to our pick target pose
            _plan_scene->setCurrentState(pick_traj.end_state);
            _plan_scene->getPlanningSceneDiffMsg(scene_msg);
            _plan_scene_publisher.publish(scene_msg);
            ROS_INFO("Successfully planned pick trajectory.");

            // TODO: Attach object (ie apple) for return trajectory



            //////////////////// Place target

            // Assume we are at a pick pose
            // Set a place target (in joint space)
            v_constraints.clear();
            v_constraints.push_back(genJointValueConstraint(_place_jvals[n]));;
            // Generate trajectory
            ur5_motion_plan place_traj;
            success = planTrajectory(place_traj, v_constraints);
            if (!success)
            {
                ROS_ERROR("Planner failed to generate plan for place target %d. Skipping.", m);
                continue;
            }
            // Add trajectory to display message and publish to Rviz
            moveit_msgs::DisplayTrajectory display_trajectory;
            display_trajectory.trajectory_start = pick_traj.start_state;
            display_trajectory.trajectory.push_back(pick_traj.trajectory);
            display_trajectory.trajectory.push_back(place_traj.trajectory);
            _trajectory_publisher.publish(display_trajectory);

            // Now record indices of pick and place locations
            pick_traj.pick_loc_index = m;
            pick_traj.place_loc_index = n;
            place_traj.pick_loc_index = m;
            place_traj.place_loc_index = n;

            // Store trajectories in library
            _pick_trajects.push_back(pick_traj);
            _place_trajects.push_back(place_traj);

            // Now change state back to our place target pose
            _plan_scene->setCurrentState(place_state);
            _plan_scene->getPlanningSceneDiffMsg(scene_msg);
            _plan_scene_publisher.publish(scene_msg);

            // TODO: Detach object (apple) for next pick trajectory

            ROS_INFO("Successfully planned place trajectory.");
            _num_trajects++;
            ROS_INFO("Trajectory set %d saved.", (int) _num_trajects);

            /* Sleep a little to allow time for rviz to display path */
            //sleep(1);
        }
    }

    ROS_INFO("Generated %d trajectories out of a theoretical %d.", (int) _num_trajects, (int) (_num_pick_targets*_num_place_targets) );

//    //SAVE DATA TO .bin FILE
//    ROS_INFO("--------------SAVING!!!!-------------------");
//    bool pickcheck = filewrite(_pick_trajects, "pickplan.bin",0);
//    bool placecheck = filewrite(_place_trajects, "placeplan.bin",0);
//    ROS_INFO("%d-----------DONE!!!!!!-----------------%d",pickcheck,placecheck);

    return _num_trajects;
}

void TrajectoryLibrary::demo()
{
    srand(0);

    int n; // Take n to be our place state index
    int m; // Take m to be our pick state index

    bool success;
    ur5_motion_plan plan;
    robot_trajectory::RobotTrajectory traj(_rmodel, UR5_GROUP_NAME);
    robot_state::RobotState end_state(_rmodel);
    robot_state::RobotState start_state(_rmodel);
    moveit_msgs::PlanningScene scene_msg;
    std::size_t num_wpts;
    double duration;
    double j_dist;

    //LOAD DATA FROM .bin FILE
    ROS_INFO("--------------LOADING!!!!-------------------");
    bool pickcheck = fileread(_pick_trajects, "pickplan.bin",0);
    bool placecheck = fileread(_place_trajects, "placeplan.bin",0);
    ROS_INFO("%d-----------DONE!!!!!!-----------------%d",pickcheck,placecheck);

    n = rand() % _num_place_targets; // Pick random place target
    end_state.setJointGroupPositions(_jmg, _place_jvals[n]);

    while (1)
    {
        // Pick path
        // Set current start state
        _plan_scene->setCurrentState(end_state);
        _plan_scene->getPlanningSceneDiffMsg(scene_msg);
        _plan_scene_publisher.publish(scene_msg);

        int tries = 0;
        do {
            // Now select random pick target
            m = rand() % _num_pick_targets;
            // Fetch plan
            success = getPickPlan(plan, n, m);
            tries++;
        } while (!success);\

        ROS_INFO("Found pick trajectory from place %d to pick %d after %d tries.", n, m, tries);

        // Build RobotTrajectory and RobotState objects from message
        robot_state::robotStateMsgToRobotState(plan.start_state, start_state);
        traj.setRobotTrajectoryMsg(start_state, plan.trajectory);
        // Set current start state
        _plan_scene->setCurrentState(start_state);
        _plan_scene->getPlanningSceneDiffMsg(scene_msg);
        _plan_scene_publisher.publish(scene_msg);
        // Get trajectory data
        num_wpts = traj.getWayPointCount();
        duration = plan.duration;
        j_dist = start_state.distance(end_state);
        ROS_INFO("Start state is %f from previous end state.", j_dist);
        ROS_INFO("Trajectory has %d nodes and takes %f seconds.", (int) num_wpts, duration);

        moveit_msgs::DisplayTrajectory display_trajectory;
        display_trajectory.trajectory_start = plan.start_state;
        display_trajectory.trajectory.push_back(plan.trajectory);

        // Update end_state
        robot_state::robotStateMsgToRobotState(plan.end_state, end_state);

        /* Place path */
        tries = 0;
        do {
            // Now select random place target
            n = rand() % _num_place_targets;
            // Fetch plan
            success = getPlacePlan(plan, m, n);
            tries++;
        } while (!success);

        ROS_INFO("Found place trajectory from pick %d to place %d after %d tries.", m, n, tries);

        // Build RobotTrajectory and RobotState objects from message
        robot_state::robotStateMsgToRobotState(plan.start_state, start_state);
        traj.setRobotTrajectoryMsg(start_state, plan.trajectory);
        // Get trajectory data
        num_wpts = traj.getWayPointCount();
        duration += plan.duration;
        j_dist = start_state.distance(end_state);
        ROS_INFO("Start state is %f from previous end state.", j_dist);
        ROS_INFO("Trajectory has %d nodes and takes %f seconds.", (int) num_wpts, plan.duration);

        display_trajectory.trajectory.push_back(plan.trajectory);
        _trajectory_publisher.publish(display_trajectory);

        // Update end_state
        robot_state::robotStateMsgToRobotState(plan.end_state, end_state);
        // Set current start state
        _plan_scene->setCurrentState(end_state);
        _plan_scene->getPlanningSceneDiffMsg(scene_msg);
        _plan_scene_publisher.publish(scene_msg);

        _trajectory_publisher.publish(display_trajectory);

        ros::WallDuration sleep_time(duration);
        sleep_time.sleep();
    }

    return;
}

bool TrajectoryLibrary::getPickPlan(ur5_motion_plan &plan, int place_start, int pick_end)
{
    // For now just do linear search
    for (int i=0; i < _num_trajects; i++)
    {
        if (_pick_trajects[i].place_loc_index == place_start)
        {
            if (_pick_trajects[i].pick_loc_index == pick_end)
            {
                plan = _pick_trajects[i];
                return true;
            }
            // TODO: If we assume certain order of trajectories in vector.
            // If pick location doesn't match up, maybe we can skip ahead a bit?
        }
    }

    // If no match
    return false;
}

bool TrajectoryLibrary::getPlacePlan(ur5_motion_plan &plan, int pick_start, int place_end)
{
    // For now just do linear search
    for (int i=0; i < _num_trajects; i++)
    {
        if (_place_trajects[i].place_loc_index == place_end)
        {
            if (_place_trajects[i].pick_loc_index == pick_start)
            {
                plan = _place_trajects[i];
                return true;
            }
            // TODO: If we assume certain order of trajectories in vector.
            // If pick location doesn't match up, maybe we can skip ahead a bit?
        }
    }

    // If no match
    return false;
}

bool TrajectoryLibrary::filewrite(std::vector<ur5_motion_plan> &Library, const char* filename, bool debug)
{
    bool check;
    int itersize = Library.size();
    int nodesize = 0;

    std::ofstream file;
    file.open (filename, std::ofstream::out | std::ofstream::binary);
    if(file.is_open())
    {
        file.write((char *)(&itersize),sizeof(itersize));
        if (debug == 1) ROS_INFO("%d",itersize);
        for (size_t n = 0; n < itersize; n++)
        {
            //RobotTrajectory -> JointTrajectory -> JointTrajectoryPoints
            nodesize = Library[n].trajectory.joint_trajectory.points.size();
            ROS_INFO("%d",nodesize);
            file.write((char *)(&nodesize),sizeof(nodesize));
            for (size_t idx = 0; idx < nodesize; idx++)
            {
                for (size_t i=0; i < 6; i++) file.write((char *)(&Library[n].trajectory.joint_trajectory.points[idx].positions[i]),sizeof(double));
                file.write((char *)(&Library[n].trajectory.joint_trajectory.points[idx].time_from_start),sizeof(ros::Duration));
            }

            //RobotTrajectory -> JointTrajectory -> Header
            file.write((char *)(&Library[n].trajectory.joint_trajectory.header.seq),sizeof(uint32_t));
            file.write((char *)(&Library[n].trajectory.joint_trajectory.header.stamp),sizeof(ros::Time));
            file << Library[n].trajectory.joint_trajectory.header.frame_id << '\n';

            //RobotTrajectory -> JointTrajectory -> joint_names
            for (size_t i=0; i < 6; i++) file << Library[n].trajectory.joint_trajectory.joint_names[i] << '\n';


            //start_state
            //RobotState -> JointState -> Header
            file.write((char *)(&Library[n].start_state.joint_state.header.seq),sizeof(uint32_t));
            file.write((char *)(&Library[n].start_state.joint_state.header.stamp),sizeof(ros::Time));
            file << Library[n].start_state.joint_state.header.frame_id << '\n';
            //RobotState -> JointState -> stirng & position
            for (size_t j = 0; j < 6; j++)
            {
                file << Library[n].start_state.joint_state.name[j] << '\n';
                file.write((char *)(&Library[n].start_state.joint_state.position[j]),sizeof(double));
            }

            //end_state
            //RobotState -> JointState -> Header
            file.write((char *)(&Library[n].end_state.joint_state.header.seq),sizeof(uint32_t));
            file.write((char *)(&Library[n].end_state.joint_state.header.stamp),sizeof(ros::Time));
            file << Library[n].end_state.joint_state.header.frame_id << '\n';
            //RobotState -> JointState -> stirng & position
            for (size_t j = 0; j < 6; j++)
            {
                file << Library[n].end_state.joint_state.name[j] << '\n';
                file.write((char *)(&Library[n].end_state.joint_state.position[j]),sizeof(double));
            }

            //index
            file.write((char *)(&Library[n].pick_loc_index),sizeof(unsigned int));
            file.write((char *)(&Library[n].place_loc_index),sizeof(unsigned int));


        }

//        //string test;
//        Library[0].trajectory.joint_trajectory.header.frame_id = "This is a Test. Also Im Hungry!!";
//        std::cout << Library[0].trajectory.joint_trajectory.header.frame_id << '\n';
//        file << Library[0].trajectory.joint_trajectory.header.frame_id  << '\n';

        check = 1;
        file.close();
    }
    else check = 0;

    return check;
}

bool TrajectoryLibrary::fileread(std::vector<ur5_motion_plan> &Library, const char* filename, bool debug)
{
    bool check;
    int nodesize;
    int itersize;

    //int nodeset = 10;
    double temp_double;
    std::string temp_string;
    std::string blank_string;
//    uint32_t temp_uint32;
//    ros::Time temp_time;
    ros::Duration temp_duration;

    ur5_motion_plan ur5;
    ur5_motion_plan empty;
    std::vector<int> temp;
    trajectory_msgs::JointTrajectoryPoint temp_points;
    trajectory_msgs::JointTrajectoryPoint blank;

    std::ifstream info;
    info.open(filename,std::ifstream::in | std::ofstream::binary);

    if(info.is_open())
    {
        //Trajectory
        info.read((char*)(&itersize),sizeof(itersize));
        if (debug == 1) ROS_INFO("%d",itersize);
        for (size_t n = 0; n < itersize; n++)
        {
            info.read((char*)(&nodesize),sizeof(nodesize));
            temp.push_back(nodesize);
            if (debug == 1) ROS_INFO("%d",nodesize);
            for (size_t idx = 0; idx < nodesize; idx++)
            {
                for (size_t i=0; i<6; i++)
                {
                    info.read((char *)(&temp_double),sizeof(temp_double));
                    temp_points.positions.push_back(temp_double);
                }
                info.read((char *)(&temp_duration),sizeof(temp_duration));
                //ros::Duration d(z);
                temp_points.time_from_start = temp_duration;
                ur5.trajectory.joint_trajectory.points.push_back(temp_points);
                temp_points = blank;

            }

            //RobotTrajectory -> JointTrajectory -> Header
            info.read((char *)(&ur5.trajectory.joint_trajectory.header.seq),sizeof(uint32_t));
            info.read((char *)(&ur5.trajectory.joint_trajectory.header.stamp),sizeof(ros::Time));
            getline (info,ur5.trajectory.joint_trajectory.header.frame_id);

            //RobotTrajectory -> JointTrajectory -> joint_names
            for (size_t i=0; i < 6; i++)
            {
                getline(info,temp_string);
                ur5.trajectory.joint_trajectory.joint_names.push_back(temp_string);
                temp_string = blank_string;
            }

            //start_state
            //RobotState -> JointState -> Header
            info.read((char *)(&ur5.start_state.joint_state.header.seq),sizeof(uint32_t));
            info.read((char *)(&ur5.start_state.joint_state.header.stamp),sizeof(ros::Time));
            getline (info,ur5.start_state.joint_state.header.frame_id);

            //RobotState -> JointState -> stirng & position
            for (size_t j = 0; j < 6; j++)
            {
                getline(info,temp_string);
                ur5.start_state.joint_state.name.push_back(temp_string);
                temp_string = blank_string;

                info.read((char *)(&temp_double),sizeof(temp_double));
                ur5.start_state.joint_state.position.push_back(temp_double);
            }

            //end_state
            //RobotState -> JointState -> Header
            info.read((char *)(&ur5.end_state.joint_state.header.seq),sizeof(uint32_t));
            info.read((char *)(&ur5.end_state.joint_state.header.stamp),sizeof(ros::Time));
            getline (info,ur5.end_state.joint_state.header.frame_id);

            //RobotState -> JointState -> stirng & position
            for (size_t j = 0; j < 6; j++)
            {
                getline(info,temp_string);
                ur5.end_state.joint_state.name.push_back(temp_string);
                temp_string = blank_string;

                info.read((char *)(&temp_double),sizeof(temp_double));
                ur5.end_state.joint_state.position.push_back(temp_double);

            }

            //Index
            info.read((char *)(&ur5.pick_loc_index),sizeof(unsigned int));
            info.read((char *)(&ur5.place_loc_index),sizeof(unsigned int));

            Library.push_back(ur5);
            ur5 = empty;
        }

//        //string test;
//        std::string James;
//        getline (info,James);
//        std::cout << James << '\n';

        check = 1;
        info.close();
    }
    else check = 0;

    return check;

}