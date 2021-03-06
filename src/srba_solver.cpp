#include <relative_slam/srba_solver.h>
#include <ros/ros.h>
#include <mrpt/gui.h>  // For rendering results as a 3D scene
#include <string>
#include <tf/transform_datatypes.h>

using namespace srba;
using mrpt::poses::CPose2D;

const double STD_NOISE_XY = 0.001;
const double STD_NOISE_YAW = 0.005;

SRBASolver::SRBASolver()
{
  rba_.setVerbosityLevel( 2 );   // 0: None; 1:Important only; 2:Verbose
  
// =========== Topology parameters ===========
  rba_.parameters.srba.max_tree_depth       = 3;
  rba_.parameters.srba.max_optimize_depth   = 3; 
  rba_.parameters.ecp.min_obs_to_loop_closure = 1; // This is a VERY IMPORTANT PARAM, if it is set to 1 everything goes to shit

  rba_.parameters.srba.use_robust_kernel = false;
  rba_.parameters.srba.optimize_new_edges_alone = true;
  rba_.parameters.srba.dumpToConsole();

  first_keyframe_ = true;
  curr_kf_id_ = 0;

  marker_count_ = 0;

  relative_map_frame_ = "relative_map";
  global_map_frame_ = "global_map";
  loop_closed_ = false;

  // Information matrix for relative pose observations:
  {
    Eigen::Matrix3d ObsL;
  ObsL.setZero();
  ObsL(0,0) = 1/(STD_NOISE_XY*STD_NOISE_XY); // x
  ObsL(1,1) = 1/(STD_NOISE_XY*STD_NOISE_XY); // y
  ObsL(2,2) = 1/(STD_NOISE_YAW*STD_NOISE_YAW); // phi
  // Set:
  rba_.parameters.obs_noise.lambda = ObsL;
  }
}

SRBASolver::~SRBASolver()
{
}

IdPoseVector& SRBASolver::GetCorrections() 
{
  ROS_INFO("Computing corrected poses up to %d", curr_kf_id_-1);
  corrections_.clear();

  if(!rba_.get_rba_state().keyframes.empty())
  {
    // Use a spanning tree to estimate the global pose of every node
    //  starting (root) at the given keyframe:
   
    //corrections_.resize(rba_.get_rba_state().keyframes.size());
    srba_t::frameid2pose_map_t  spantree;
    if(curr_kf_id_ == 0)
      return corrections_;
    TKeyFrameID root_keyframe(0);
    rba_.create_complete_spanning_tree(root_keyframe,spantree);

    for (srba_t::frameid2pose_map_t::const_iterator itP = spantree.begin();itP!=spantree.end();++itP)
    {
      std::pair<int, karto::Pose2> id_pose;
      id_pose.first = itP->first;
      const CPose2D p = itP->second.pose;
      karto::Pose2 pos(p.x(), p.y(), p.phi());
      id_pose.second = pos;
      corrections_.push_back(id_pose);
    }
  }
 
    return corrections_;
}

void SRBASolver::Compute()
{
  ROS_INFO("Computing corrected poses");
  corrections_.clear();

  if(!rba_.get_rba_state().keyframes.empty())
  {
    // Use a spanning tree to estimate the global pose of every node
    //  starting (root) at the given keyframe:
   
    //corrections_.resize(rba_.get_rba_state().keyframes.size());
    srba_t::frameid2pose_map_t  spantree;
    if(curr_kf_id_ == 0)
      return;
    TKeyFrameID root_keyframe(0);
    rba_.create_complete_spanning_tree(root_keyframe,spantree);

    for (srba_t::frameid2pose_map_t::const_iterator itP = spantree.begin();itP!=spantree.end();++itP)
    {
      std::pair<int, karto::Pose2> id_pose;
      id_pose.first = itP->first;
      const CPose2D p = itP->second.pose;
      karto::Pose2 pos(p.x(), p.y(), p.phi());
      id_pose.second = pos;
      corrections_.push_back(id_pose);
    }
  }
   
  // Get the global graph and return updated poses?
  //typedef std::vector<sba::Node2d, Eigen::aligned_allocator<sba::Node2d> > NodeVector;
  //ROS_INFO("Calling SRBA compute");

  // Do nothing here?
}


int SRBASolver::AddNode(const karto::Pose2 &pose)
{
  ROS_INFO("Adding node: %d", curr_kf_id_);
  srba_t::new_kf_observations_t  list_obs;
  srba_t::new_kf_observation_t obs_field;
  obs_field.is_fixed = false;
  obs_field.obs.feat_id = curr_kf_id_;// Feature ID == keyframe ID
  obs_field.obs.obs_data.x = 0;//pose.GetX();   // Landmark values are actually ignored.
  obs_field.obs.obs_data.y = 0;//pose.GetY();
  obs_field.obs.obs_data.yaw = 0;//pose.GetHeading();
  list_obs.push_back( obs_field );
  
  // Add the last keyframe
  srba_t::TNewKeyFrameInfo new_kf_info;
  rba_.define_new_keyframe(
    list_obs,      // Input observations for the new KF
    new_kf_info,   // Output info
    true // Also run local optimization?
  );

  ROS_INFO("Added node: %d with self observation %d", (int)new_kf_info.kf_id, (int)list_obs[0].obs.feat_id);
  curr_kf_id_ = new_kf_info.kf_id+1;
  return new_kf_info.kf_id;
}

void SRBASolver::AddConstraint(int sourceId, int targetId, const karto::Pose2 &rDiff, const karto::Matrix3& rCovariance)
{
  // Need to call create_kf2kf_edge here
  srba_t::new_kf_observations_t  list_obs;
  srba_t::new_kf_observation_t obs_field;
  obs_field.is_fixed = false;   // "Landmarks" (relative poses) have unknown relative positions (i.e. treat them as unknowns to be estimated)
  obs_field.is_unknown_with_init_val = false; // Ignored, since all observed "fake landmarks" already have an initialized value.

  bool reverse_edge = false;
  if(sourceId < targetId)
  {
    ROS_ERROR("REVERSE EDGE");
    reverse_edge = true;
  }
  else
    ROS_ERROR("$$$$$$$$$$$$$$$$NOT REVERSE EDGE");

  karto::Matrix3 precisionMatrix = rCovariance.Inverse();
  Eigen::Matrix<double,3,3> m;
  m(0,0) = precisionMatrix(0,0);
  m(0,1) = m(1,0) = precisionMatrix(0,1);
  m(0,2) = m(2,0) = precisionMatrix(0,2);
  m(1,1) = precisionMatrix(1,1);
  m(1,2) = m(2,1) = precisionMatrix(1,2);
  m(2,2) = precisionMatrix(2,2);

//  if(reverse_edge)
//  {
    CPose2D pose(rDiff.GetX(), rDiff.GetY(), rDiff.GetHeading());
    pose.inverse();
    obs_field.obs.feat_id      = sourceId;  // Is this right??
    obs_field.obs.obs_data.x   = pose.x();
    obs_field.obs.obs_data.y   = pose.y();
    obs_field.obs.obs_data.yaw = pose.phi();
//  }
//  else
/*  {
    obs_field.obs.feat_id      = targetId;  // Is this right??
    obs_field.obs.obs_data.x   = rDiff.GetX();
    obs_field.obs.obs_data.y   = rDiff.GetY();
    obs_field.obs.obs_data.yaw = rDiff.GetHeading();
  }*/

  list_obs.push_back( obs_field );

  std::vector<srba::TNewEdgeInfo> new_edge_ids;

  //if(reverse_edge)
 // {
    rba_.add_observation(targetId, obs_field.obs, NULL, NULL ); 
//    rba_.determine_kf2kf_edges_to_create(targetId,
 //     list_obs,
  //    new_edge_ids);

    ROS_INFO("Created new edge from source: %d to target %d (%f, %f, %f)", sourceId, targetId, -rDiff.GetX(), -rDiff.GetY(), -rDiff.GetHeading());
  //}
 /* else
  { 
    rba_.determine_kf2kf_edges_to_create(sourceId,
      list_obs,
      new_edge_ids);

     rba_.add_observation(sourceId, obs_field.obs, NULL, NULL ); 
  }*/
}

void SRBASolver::getActiveIds(std::vector<int> &ids)
{
  if(!rba_.get_rba_state().keyframes.empty())
  {
    srba_t::frameid2pose_map_t  spantree;
    if(curr_kf_id_ == 0)
      return;
    
    TKeyFrameID root_keyframe(curr_kf_id_-1);
    rba_.create_complete_spanning_tree(root_keyframe,spantree, 30);

    int id = 0;
    for (srba_t::frameid2pose_map_t::const_iterator itP = spantree.begin();itP!=spantree.end();++itP)
    {
      ids.push_back(itP->first);
    }
  }
}

void SRBASolver::publishGlobalGraph()
{
  if(! (rba_.get_rba_state().keyframes.size() < 5))
  {
    mrpt::graphs::CNetworkOfPoses3D poseGraph;
  rba_.get_global_graphslam_problem(poseGraph);

  // Run optimization:
  mrpt::graphslam::TResultInfoSpaLevMarq out_info;
  mrpt::utils::TParametersDouble extra_params;
  if(loop_closed_)
  {
  mrpt::graphslam::optimize_graph_spa_levmarq(
    poseGraph, 
    out_info,
    NULL, /* in_nodes_to_optimize, NULL=all */
    extra_params
    );
  loop_closed_ = false;
  }
  mrpt::gui::CDisplayWindow3D win2("Global optimized map",640,480);
  {
    mrpt::opengl::COpenGLScenePtr &scene = win2.get3DSceneAndLock();
    
    mrpt::utils::TParametersDouble render_params;   // See docs for mrpt::opengl::graph_tools::graph_visualize()
    render_params["show_ID_labels"] = 1;    
    
    // Get opengl representation of the graph:
    mrpt::opengl::CSetOfObjectsPtr gl_global_map = mrpt::opengl::graph_tools::graph_visualize( poseGraph,render_params );
    scene->insert(gl_global_map);
    
    win2.unlockAccess3DScene();
    win2.repaint();
    win2.waitForKey();
  }
  }
}

void SRBASolver::publishGraphVisualization(visualization_msgs::MarkerArray &marray)
{ 
  ROS_INFO("Visualizing");
  // Vertices are round, red spheres
  visualization_msgs::Marker m;
  m.header.frame_id = relative_map_frame_;
  m.header.stamp = ros::Time::now();
  m.id = 0;
  m.ns = "karto";
  m.type = visualization_msgs::Marker::ARROW;
  m.pose.position.x = 0.0;
  m.pose.position.y = 0.0;
  m.pose.position.z = 0.0;
  m.scale.x = 0.15;
  m.scale.y = 0.15;
  m.scale.z = 0.15;
  m.color.r = 1.0;
  m.color.g = 0;
  m.color.b = 0.0;
  m.color.a = 1.0;
  m.lifetime = ros::Duration(0);

  // Odometry edges are opaque blue line strips 
  visualization_msgs::Marker edge;
  edge.header.frame_id = relative_map_frame_;
  edge.header.stamp = ros::Time::now();
  edge.action = visualization_msgs::Marker::ADD;
  edge.ns = "karto";
  edge.id = 0;
  edge.type = visualization_msgs::Marker::LINE_STRIP;
  edge.scale.x = 0.1;
  edge.scale.y = 0.1;
  edge.scale.z = 0.1;
  edge.color.a = 1.0;
  edge.color.r = 0.0;
  edge.color.g = 0.0;
  edge.color.b = 1.0;

  // Loop edges are purple, opacity depends on backend state
  visualization_msgs::Marker loop_edge;
  loop_edge.header.frame_id = relative_map_frame_;
  loop_edge.header.stamp = ros::Time::now();
  loop_edge.action = visualization_msgs::Marker::ADD;
  loop_edge.ns = "spanning_tree";
  loop_edge.id = 0;
  loop_edge.type = visualization_msgs::Marker::LINE_STRIP;
  loop_edge.scale.x = 0.1;
  loop_edge.scale.y = 0.1;
  loop_edge.scale.z = 0.1;
  loop_edge.color.a = 1.0;
  loop_edge.color.r = 1.0;
  loop_edge.color.g = 0.0;
  loop_edge.color.b = 1.0;
 
  visualization_msgs::Marker node_text;
  node_text.header.frame_id = relative_map_frame_;
  node_text.header.stamp = ros::Time::now();
  node_text.action = visualization_msgs::Marker::ADD;
  node_text.ns = "karto";
  node_text.id = 0;
  node_text.type = visualization_msgs::Marker::TEXT_VIEW_FACING;
  node_text.scale.z = 0.3;
  node_text.color.a = 1.0;
  node_text.color.r = 1.0;
  node_text.color.g = 1.0;
  node_text.color.b = 1.0;
 
  if(!rba_.get_rba_state().keyframes.empty())
  {
    // Use a spanning tree to estimate the global pose of every node
    //  starting (root) at the given keyframe:
    
    srba_t::frameid2pose_map_t  spantree;
    if(curr_kf_id_ == 0)
      return;
    TKeyFrameID root_keyframe(curr_kf_id_ -1 );
    rba_.create_complete_spanning_tree(root_keyframe,spantree);

    int id = 0;
    for (srba_t::frameid2pose_map_t::const_iterator itP = spantree.begin();itP!=spantree.end();++itP)
    {
      if (root_keyframe==itP->first) continue;

      const CPose2D p = itP->second.pose;
      
      // Add the vertex to the marker array 
      m.id = id;
      m.pose.position.x = p.x();
      m.pose.position.y = p.y();
      geometry_msgs::Quaternion q = tf::createQuaternionMsgFromYaw(p.phi());

      m.pose.orientation = q;
      marray.markers.push_back(visualization_msgs::Marker(m));
      id++;

      node_text.id = id;
      node_text.text= boost::lexical_cast<std::string>(itP->first);
      node_text.pose.position.x = p.x()+0.15; 
      node_text.pose.position.y = p.y()+0.15; 
      marray.markers.push_back(visualization_msgs::Marker(node_text));
      id++;

    }
    
    for (srba_t::rba_problem_state_t::k2k_edges_deque_t::const_iterator itEdge = rba_.get_rba_state().k2k_edges.begin();
        itEdge!=rba_.get_rba_state().k2k_edges.end();++itEdge)
    {
      CPose2D p1, p2;
      if(itEdge->from != root_keyframe)
      {
        srba_t::frameid2pose_map_t::const_iterator itN1 = spantree.find(itEdge->from);
        if(itN1==spantree.end())
          continue;
        p1 = itN1->second.pose;
      }
      if(itEdge->to != root_keyframe)
      {
        srba_t::frameid2pose_map_t::const_iterator itN2 = spantree.find(itEdge->to);
        if(itN2==spantree.end())
          continue;
        p2 = itN2->second.pose;
      }
      geometry_msgs::Point pt1, pt2;
      pt1.x = p1.x();
      pt1.y = p1.y();
      pt2.x = p2.x();
      pt2.y = p2.y();

      loop_edge.points.clear();
      loop_edge.points.push_back(pt1);
      loop_edge.points.push_back(pt2);
      loop_edge.id = id;
      marray.markers.push_back(visualization_msgs::Marker(loop_edge));
      id++;
    }

    // Render landmark as pose constraint
    // For each KF: check all its "observations"
    for (srba_t::frameid2pose_map_t::const_iterator it=spantree.begin();it!=spantree.end();++it)
    {
      const TKeyFrameID kf_id = it->first;
      const srba_t::pose_flag_t & pf = it->second;

      const typename srba_t::keyframe_info &kfi = rba_.get_rba_state().keyframes[kf_id];

      for (size_t i=0;i<kfi.adjacent_k2f_edges.size();i++)
      {
        const srba_t::k2f_edge_t * k2f = kfi.adjacent_k2f_edges[i];
        const TKeyFrameID other_kf_id = k2f->feat_rel_pos->id_frame_base;
        if (kf_id==other_kf_id)
          continue; // It's not an constraint with ANOTHER keyframe

        // Is the other KF in the spanning tree?
        srba_t::frameid2pose_map_t::const_iterator other_it=spantree.find(other_kf_id);
        if (other_it==spantree.end()) continue;

        const srba_t::pose_flag_t & other_pf = other_it->second;

        // Add edge between the two KFs to represent the pose constraint:
        mrpt::poses::CPose2D p1 = mrpt::poses::CPose2D(pf.pose);  // Convert to 3D
        mrpt::poses::CPose2D p2 = mrpt::poses::CPose2D(other_pf.pose);

        geometry_msgs::Point pt1, pt2;
        pt1.x = p1.x();
        pt1.y = p1.y();
        pt2.x = p2.x();
        pt2.y = p2.y();

        edge.points.clear();
        edge.points.push_back(pt1);
        edge.points.push_back(pt2);
        edge.id = id;
        marray.markers.push_back(visualization_msgs::Marker(edge));
        id++;
      }

  } // end for each KF

  // Delete any excess markers whose ids haven't been reclaimed
  m.action = visualization_msgs::Marker::DELETE;
  for (; id < marker_count_; id++) 
  {
    m.id = id;
    marray.markers.push_back(visualization_msgs::Marker(m));
  }
  marker_count_ = marray.markers.size();
}
  else
    ROS_INFO("Graph is empty");
}

void SRBASolver::Clear()
{
  corrections_.clear();
}

std::vector<int> SRBASolver::GetNearLinkedObjects(int kf_id, int max_topo_distance)
{
  MY_FEAT_VISITOR feat;
  MY_KF_VISITOR vis(rba_.get_rba_state(), kf_id);
  MY_K2K_EDGE_VISITOR k2k;
  MY_K2F_EDGE_VISITOR k2f;
  rba_.bfs_visitor(kf_id, max_topo_distance, false, vis, feat, k2k, k2f);
  return vis.near_linked_ids_;
}
