/**
 *
 * Motion server v2.0
 *
 **/

#include <string>
#include <map>
#include <iostream>
#include <sstream>
#include <math.h>

#include <boost/thread.hpp>

#include <ros/ros.h>
#include <moveit/planning_scene_interface/planning_scene_interface.h>
#include <moveit/move_group_interface/move_group.h>
#include <tf/transform_datatypes.h>

#include "moveit_msgs/CollisionObject.h"
#include "rosie_msgs/RobotCommand.h"
#include "rosie_msgs/RobotAction.h"
#include "rosie_msgs/Observations.h"
#include "rosie_msgs/ObjectData.h"
#include "control_msgs/GripperCommandAction.h"
#include "control_msgs/GripperCommandGoal.h"

class MotionServer
{
public:
    enum ActionState {WAIT,
                      HOME,
                      GRAB,
                      POINT,
                      DROP,
                      FAILURE,
                      SCENE};

    static std::string asToString(ActionState a)
    {
        switch (a)
        {
            case WAIT: return "WAIT";
            case HOME: return "HOME";
            case GRAB: return "GRAB";
            case POINT: return "POINT";
            case DROP: return "DROP";
            case FAILURE: return "FAILURE";
            case SCENE: return "SCENE";
            default: return "WTF";
        }
    }

    MotionServer(bool humanCheck=true) : state(WAIT),
                                         lastCommandTime(0),
                                         grabbedObject(-1),
                                         checkPlans(humanCheck),
                                         group("arm")
    {
        ROS_INFO("RosieMotionServer starting up!");

        group.setMaxVelocityScalingFactor(0.3);

        obsSubscriber = n.subscribe("rosie_observations", 10,
                                    &MotionServer::obsCallback, this);
        commSubscriber = n.subscribe("rosie_arm_commands", 10,
                                     &MotionServer::commandCallback, this);
        statusPublisher = n.advertise<rosie_msgs::RobotAction>("rosie_arm_status", 10);

        pubTimer = n.createTimer(ros::Duration(0.1),
                                 &MotionServer::publishStatus, this);
    };

    void obsCallback(const rosie_msgs::Observations::ConstPtr& msg)
    {
        boost::lock_guard<boost::mutex> guard(objMutex);

        objectPoses.clear();
        objectSizes.clear();

        currentTable = msg->table;

        for (std::vector<rosie_msgs::ObjectData>::const_iterator i = msg->observations.begin();
             i != msg->observations.end(); i++) {
            std::vector<float> pos = std::vector<float>();
            pos.push_back(i->bbox_xyzrpy.translation.x);
            pos.push_back(i->bbox_xyzrpy.translation.y);
            pos.push_back(i->bbox_xyzrpy.translation.z);
            tf::Quaternion quat;
            tf::quaternionMsgToTF(i->bbox_xyzrpy.rotation, quat);
            double roll, pitch, yaw;
            tf::Matrix3x3(quat).getRPY(roll, pitch, yaw);
            pos.push_back(float(roll));
            pos.push_back(float(pitch));
            pos.push_back(float(yaw));
            objectPoses.insert(std::pair<int, std::vector<float> >(i->obj_id, pos));

            std::vector<float> dim = std::vector<float>();
            pos.push_back(i->bbox_dim.x);
            pos.push_back(i->bbox_dim.y);
            pos.push_back(i->bbox_dim.z);
            objectSizes.insert(std::pair<int, std::vector<float> >(i->obj_id, dim));
        }
    }

    void commandCallback(const rosie_msgs::RobotCommand::ConstPtr& msg)
    {
        if (asToString(state) == msg->action || msg->utime == lastCommandTime)
            return;

        lastCommandTime = msg->utime;
        if (msg->action.find("GRAB")!=std::string::npos)
        {
            state = GRAB;
            std::string num = msg->action.substr(msg->action.find("=")+1);
            ROS_INFO("Handling pickup command for object %s", num.c_str());

            int idNum;
            std::stringstream ss(num);
            if (!(ss >> idNum)) {
                ROS_INFO("Invalid object ID number %s", num.c_str());
                return;
            }

            handleGrabCommand(idNum);
        }
        else if (msg->action.find("DROP")!=std::string::npos){
            ROS_INFO("Handling putdown command");
            state = DROP;
            std::vector<float> t = std::vector<float>();
            t.push_back(msg->dest.translation.x);
            t.push_back(msg->dest.translation.y);
            t.push_back(msg->dest.translation.z);
            handleDropCommand(t);
        }
        else if (msg->action.find("POINT")!=std::string::npos){
            state = POINT;
            std::string num = msg->action.substr(msg->action.find("=")+1);
            ROS_INFO("Handling point command for object %s", num.c_str());

            int idNum;
            std::stringstream ss(num);
            if (!(ss >> idNum)) {
                ROS_INFO("Invalid object ID number %s", num.c_str());
                return;
            }

            handlePointCommand(idNum);
        }
        else if (msg->action.find("HOME")!=std::string::npos){
            ROS_INFO("Handling home command");
            state = HOME;
            homeArm();
        }
        else if (msg->action.find("SCENE")!=std::string::npos){
            ROS_INFO("Handling build scene command");
            state = SCENE;
            setUpScene();
            state = WAIT;
        }
        else {
            ROS_INFO("Unknown command type received");
            state = FAILURE;
        }
    }

    void handleGrabCommand(int id)
    {
        ROS_INFO("GRAB command recieved ok");
    }

    void handleDropCommand(std::vector<float> target)
    {
        ROS_INFO("DROP command recieved ok");
    }

    void handlePointCommand(int id)
    {
        boost::lock_guard<boost::mutex> guard(objMutex);
        float x = objectPoses[id][0] - 0.22;
        float y = objectPoses[id][1];
        float z = objectPoses[id][2] + 0.22;
        moveToXYZTarget(x, y, z);
        ros::Duration(1.0).sleep();
        homeArm();
    }

    void publishStatus(const ros::TimerEvent& e)
    {
        rosie_msgs::RobotAction msg = rosie_msgs::RobotAction();
        msg.utime = ros::Time::now().toNSec();
        msg.action = asToString(state).c_str();
        msg.obj_id = grabbedObject;
        statusPublisher.publish(msg);
     }

    void setUpScene()
    {
        moveit_msgs::CollisionObject collision_object;
        collision_object.header.frame_id = group.getPlanningFrame();

        /* The id of the object is used to identify it. */
        collision_object.id = "box1";

        /* Define a box to add to the world. */
        shape_msgs::SolidPrimitive primitive;
        primitive.type = primitive.BOX;
        primitive.dimensions.resize(3);
        primitive.dimensions[0] = 0.4;
        primitive.dimensions[1] = 0.1;
        primitive.dimensions[2] = 0.4;

       /* A pose for the box (specified relative to frame_id) */
       geometry_msgs::Pose box_pose;
       box_pose.orientation.w = 1.0;
       box_pose.position.x =  0.6;
       box_pose.position.y = -0.4;
       box_pose.position.z =  1.2;

       collision_object.primitives.push_back(primitive);
       collision_object.primitive_poses.push_back(box_pose);
       collision_object.operation = collision_object.ADD;

       std::vector<moveit_msgs::CollisionObject> collision_objects;
       collision_objects.push_back(collision_object);

       ROS_INFO("Adding an object into the world");
       scene.addCollisionObjects(collision_objects);

/* Sleep so we have time to see the object in RViz */
       sleep(2.0);
 //        #self.scene.remove_world_object() <- broken
//         co = CollisionObject()
//         co.operation = CollisionObject.REMOVE
//         co.header.frame_id = self.group.get_planning_frame()
//         self.collision_pub.publish(co)
//         rospy.sleep(0.1)

//         self.obj_lock.acquire()
//         try:
//             for onum, o in self.perceived_objects.iteritems():
//                 p = geometry_msgs.msg.Pose()
//                 p.position.x = o[0].translation.x + (o[1].x/2.0)
//                 p.position.y = o[0].translation.y - (o[1].y/2.0)
//                 p.position.z = o[0].translation.z + (o[1].z/2.0)
//                 p.orientation.x = o[0].rotation.x
//                 p.orientation.y = o[0].rotation.y
//                 p.orientation.z = o[0].rotation.z
//                 p.orientation.w = o[0].rotation.w
//                 ps = geometry_msgs.msg.PoseStamped()
//                 ps.pose = p
//                 ps.header.frame_id = self.group.get_planning_frame()

//                 self.scene.add_box(str(onum), ps,
//                                    (o[1].x+0.02,
//                                     o[1].y+0.02,
//                                     o[1].z+0.02))
//                 rospy.sleep(0.1)

//         finally:
//             self.obj_lock.release()

//         planep = geometry_msgs.msg.Pose()
//         planep.position.x = 0.8
//         planep.position.y = 0.0
//         planep.position.z = (self.current_table[3] + self.current_table[0]*0.8 +
//                              self.current_table[1]*0.0) / -self.current_table[2]
//         planep.position.z += 0.01

//         planep.orientation.w = 1.0
//         pps = geometry_msgs.msg.PoseStamped()
//         pps.pose = planep
//         pps.header.frame_id = self.group.get_planning_frame()
//         # add_plane is broken, I think, so I'm adding the table as a flat box
//         self.scene.add_box("table", pps, (1, 1, 0.02))
    }

    void homeArm()
    {
        group.setStartStateToCurrentState();
        std::vector<double> joints = std::vector<double>();
        joints.push_back(1.32);
        joints.push_back(0.7);
        joints.push_back(0.0);
        joints.push_back(-2.0);
        joints.push_back(0.0);
        joints.push_back(-0.57);
        joints.push_back(0.0);
        group.setJointValueTarget(joints);

        moveit::planning_interface::MoveGroup::Plan homePlan;
        bool success = group.plan(homePlan);

        // XXX Safety!!
        // if self.check_motion:
        //     goahead = raw_input("Is this plan okay? ")
        //     if goahead == "y" or goahead == "yes":
        //         rospy.loginfo("Motion plan approved. Execution starting.")
        //     else:
        //         rospy.loginfo("Motion execution cancelled.")
        //         return

        bool moveSuccess = group.execute(homePlan);
        state = WAIT;
    }

    void moveToXYZTarget(float x, float y, float z)
    {
        //self.set_up_scene()

        //target_rpy = [0, -math.pi/4.0, 0]
        //target_quat = rpy2quat(target_rpy)

        geometry_msgs::Quaternion q =
            tf::createQuaternionMsgFromRollPitchYaw(0, M_PI/4.0, 0);
        geometry_msgs::Pose target = geometry_msgs::Pose();
        target.orientation = q;
        target.position.x = x;
        target.position.y = y;
        target.position.z = z;

        group.setPoseTarget(target);
        moveit::planning_interface::MoveGroup::Plan xyzPlan;
        bool success = group.plan(xyzPlan);

        // XXX Safety!!
        // if self.check_motion:
        //     goahead = raw_input("Is this plan okay? ")
        //     if goahead == "y" or goahead == "yes":
        //         rospy.loginfo("Motion plan approved. Execution starting.")
        //     else:
        //         rospy.loginfo("Motion execution cancelled.")
        //         return

        bool moveSuccess = group.execute(xyzPlan);
        state = WAIT;
    }

private:
    ros::NodeHandle n;
    ros::Subscriber obsSubscriber;
    ros::Subscriber commSubscriber;
    ros::Publisher statusPublisher;
    ros::Timer pubTimer;

    ActionState state;
    long lastCommandTime;
    int grabbedObject;
    bool checkPlans;

    moveit::planning_interface::MoveGroup group;
    moveit::planning_interface::PlanningSceneInterface scene;

    std::map<int, std::vector<float> > objectPoses;
    std::map<int, std::vector<float> > objectSizes;
    std::vector<float> currentTable;
    boost::mutex objMutex;
};

int main(int argc, char** argv)
{
    ros::init(argc, argv, "rosie_motion_server");
    MotionServer ms;

    ros::AsyncSpinner spinner(4);
    spinner.start();
    ros::waitForShutdown();

    return 0;
}
