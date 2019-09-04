#include <point_cloud_proc/point_cloud_proc.h>

PointCloudProc::PointCloudProc(ros::NodeHandle n, bool debug, std::string config) :
        nh_(n), debug_(debug), cloud_transformed_(new CloudT), cloud_filtered_(new CloudT),
        cloud_hull_(new CloudT), cloud_tabletop_(new CloudT) {

    std::string config_path;
    if(config.empty()){
      std::string pkg_path = ros::package::getPath("point_cloud_proc");
      config_path = pkg_path + "/config/default.yaml";
      std::cout << "PCP: config file : " + config_path << std::endl;
    }else{
      config_path = config;
    }


    YAML::Node parameters = YAML::LoadFile(config_path);

    // General parameters
    point_cloud_topic_ = parameters["point_cloud_topic"].as<std::string>();
    fixed_frame_ = parameters["fixed_frame"].as<std::string>();

    // Segmentation parameters
    eps_angle_ = parameters["segmentation"]["sac_eps_angle"].as<float>();
    single_dist_thresh_ = parameters["segmentation"]["sac_dist_thresh_single"].as<float>();
    multi_dist_thresh_ = parameters["segmentation"]["sac_dist_thresh_multi"].as<float>();
    min_plane_size_ = parameters["segmentation"]["sac_min_plane_size"].as<int>();
    max_iter_ = parameters["segmentation"]["sac_max_iter"].as<int>();
    k_search_ = parameters["segmentation"]["ne_k_search"].as<int>();
    cluster_tol_ = parameters["segmentation"]["ec_cluster_tol"].as<float>();
    min_cluster_size_ = parameters["segmentation"]["ec_min_cluster_size"].as<int>();
    max_cluster_size_ = parameters["segmentation"]["ec_max_cluster_size"].as<int>();

    // Filter parameters
    leaf_size_ = parameters["filters"]["leaf_size"].as<float>();
    pass_limits_ = parameters["filters"]["pass_limits"].as<std::vector<float>>();
    prism_limits_ = parameters["filters"]["prism_limits"].as<std::vector<float>>();
    min_neighbors_ = parameters["filters"]["outlier_min_neighbors"].as<int>();
    radius_search_ = parameters["filters"]["outlier_radius_search"].as<float>();

    point_cloud_sub_ = nh_.subscribe(point_cloud_topic_, 10, &PointCloudProc::pointCloudCb, this);

    if (debug_) {
        plane_cloud_pub_ = nh_.advertise<sensor_msgs::PointCloud2>("plane_cloud", 10);
        debug_cloud_pub_ = nh_.advertise<sensor_msgs::PointCloud2>("debug_cloud", 10);
        tabletop_pub_ = nh_.advertise<sensor_msgs::PointCloud2>("tabletop_cloud", 10);
        object_poses_pub_ = nh_.advertise<geometry_msgs::PoseArray>("object_poses", 10);
    }
}


void PointCloudProc::pointCloudCb(const sensor_msgs::PointCloud2ConstPtr &msg) {
    boost::mutex::scoped_lock lock(pc_mutex_);
    cloud_raw_ros_ = *msg;
    pc_received_ = true;
}


bool PointCloudProc::transformPointCloud() {
    boost::mutex::scoped_lock lock(pc_mutex_);

    cloud_transformed_->clear();

    while (ros::ok()){
        if(pc_received_)
            break;
        else
            ros::Duration(0.1).sleep();
    }

    tf::TransformListener listener;
    std::string target_frame = cloud_raw_ros_.header.frame_id;

    listener.waitForTransform(fixed_frame_, target_frame, ros::Time(0), ros::Duration(2.0));
    tf::StampedTransform transform;
    tf::Transform cloud_transform;

    try {
        listener.lookupTransform(fixed_frame_, target_frame, ros::Time(0), transform);
        cloud_transform.setOrigin(transform.getOrigin());
        cloud_transform.setRotation(transform.getRotation());

        sensor_msgs::PointCloud2 cloud_transformed;
        pcl_ros::transformPointCloud(fixed_frame_, cloud_transform, cloud_raw_ros_, cloud_transformed);

        pcl::fromROSMsg(cloud_transformed, *cloud_transformed_);

        std::cout << "PCP: point cloud is transformed!" << std::endl;
        return true;

    }
    catch (tf::TransformException ex) {
        ROS_ERROR("%s", ex.what());
        return false;
    }


}

bool PointCloudProc::filterPointCloud() {

    // Remove part of the scene to leave table and objects alone

    pass_.setInputCloud(cloud_transformed_);
    pass_.setFilterFieldName("x");
    pass_.setFilterLimits(pass_limits_[0], pass_limits_[1]);
    pass_.filter(*cloud_filtered_);
    pass_.setInputCloud(cloud_filtered_);
    pass_.setFilterFieldName("y");
    pass_.setFilterLimits(pass_limits_[2], pass_limits_[3]);
    pass_.filter(*cloud_filtered_);
    pass_.setInputCloud(cloud_filtered_);
    pass_.setFilterFieldName("z");
    pass_.setFilterLimits(pass_limits_[4], pass_limits_[5]);
    pass_.filter(*cloud_filtered_);


    std::cout << "PCP: point cloud is filtered!" << std::endl;
    if (cloud_filtered_->points.size() == 0) {
        std::cout << "PCP: point cloud is empty after filtering!" << std::endl;
        return false;
    }

    // Downsample point cloud
  vg_.setInputCloud (cloud_filtered_);
  vg_.setLeafSize (leaf_size_, leaf_size_, leaf_size_);
  vg_.filter (*cloud_filtered_);

    return true;
}

bool PointCloudProc::removeOutliers(CloudT::Ptr in, CloudT::Ptr out) {

    outrem_.setInputCloud(in);
    outrem_.setRadiusSearch(radius_search_);
    outrem_.setMinNeighborsInRadius(min_neighbors_);
    outrem_.filter(*out);

}

bool PointCloudProc::segmentSinglePlane(point_cloud_proc::Plane &plane, char axis) {
//    boost::mutex::scoped_lock lock(pc_mutex_);
    std::cout << "PCP: segmenting single plane..." << std::endl;

    if (!transformPointCloud()) {
        std::cout << "PCP: couldn't transform point cloud!" << std::endl;
        return false;
    }

    if (!filterPointCloud()) {
        std::cout << "PCP: couldn't filter point cloud!" << std::endl;
        return false;
    }


    CloudT::Ptr cloud_plane(new CloudT);
    pcl::ModelCoefficients::Ptr coefficients(new pcl::ModelCoefficients);
    pcl::PointIndices::Ptr inliers(new pcl::PointIndices);

    Eigen::Vector3f axis_vector = Eigen::Vector3f(0.0, 0.0, 0.0);

    if (axis == 'x') {
        axis_vector[0] = 1.0;
    } else if (axis == 'y') {
        axis_vector[1] = 1.0;
    } else if (axis == 'z') {
        axis_vector[2] = 1.0;
    }

    seg_.setOptimizeCoefficients(true);
    seg_.setMaxIterations(max_iter_);
//    seg_.setModelType(pcl::SACMODEL_PERPENDICULAR_PLANE);
    seg_.setModelType(pcl::SACMODEL_PLANE);
    seg_.setMethodType(pcl::SAC_RANSAC);
//    seg_.setAxis(axis_vector);
//    seg_.setEpsAngle(eps_angle_ * (M_PI / 180.0f));
    seg_.setDistanceThreshold(single_dist_thresh_);
    seg_.setInputCloud(cloud_filtered_);
    seg_.segment(*inliers, *coefficients);


    if (inliers->indices.size() == 0) {
        std::cout << "PCP: plane is empty!" << std::endl;
        return false;
    }

    extract_.setInputCloud(cloud_filtered_);
    extract_.setNegative(false);
    extract_.setIndices(inliers);
    extract_.filter(*cloud_plane);

    if (debug_) {
        std::cout << "PCP: # of points in plane: " << cloud_plane->points.size() << std::endl;
        plane_cloud_pub_.publish(cloud_plane);
    }

    cloud_hull_->clear();
    chull_.setInputCloud(cloud_plane);
    chull_.setDimension(2);
    chull_.reconstruct(*cloud_hull_);

    // Get cloud
    pcl::toROSMsg(*cloud_plane, plane.cloud);

    // Construct plane object msg
    pcl_conversions::fromPCL(cloud_plane->header, plane.header);

    // Get plane center
    Eigen::Vector4f center;
    pcl::compute3DCentroid(*cloud_plane, center);
    plane.center.x = center[0];
    plane.center.y = center[1];
    plane.center.z = center[2];

    // Get plane min and max values
    Eigen::Vector4f min_vals, max_vals;
    pcl::getMinMax3D(*cloud_plane, min_vals, max_vals);

    plane.min.x = min_vals[0];
    plane.min.y = min_vals[1];
    plane.min.z = min_vals[2];


    plane.max.x = max_vals[0];
    plane.max.y = max_vals[1];
    plane.max.z = max_vals[2];

    // Get plane polygon
    for (int i = 0; i < cloud_hull_->points.size(); i++) {
        geometry_msgs::Point32 p;
        p.x = cloud_hull_->points[i].x;
        p.y = cloud_hull_->points[i].y;
        p.z = cloud_hull_->points[i].z;

        plane.polygon.push_back(p);
    }

    // Get plane coefficients
    plane.coef[0] = coefficients->values[0];
    plane.coef[1] = coefficients->values[1];
    plane.coef[2] = coefficients->values[2];
    plane.coef[3] = coefficients->values[3];

    plane.size.data = cloud_plane->points.size();

//    extract_.setNegative(true);
//    extract_.filter(*cloud_filtered_);

    return true;
}

bool PointCloudProc::segmentMultiplePlane(std::vector<point_cloud_proc::Plane> &planes) {

//    boost::mutex::scoped_lock lock(pc_mutex_);

    if (!transformPointCloud()) {
        std::cout << "PCP: couldn't transform point cloud!" << std::endl;
        return false;
    }

    if (!filterPointCloud()) {
        std::cout << "PCP: couldn't filter point cloud!" << std::endl;
        return false;
    }

    CloudT plane_clouds;
    plane_clouds.header.frame_id = cloud_transformed_->header.frame_id;
    point_cloud_proc::Plane plane_object_msg;

    int no_planes = 1;
    CloudT::Ptr cloud_plane_raw(new CloudT);
    CloudT::Ptr cloud_plane(new CloudT);
    CloudT::Ptr cloud_hull(new CloudT);

//    Eigen::Vector3f axis = Eigen::Vector3f(0.0,0.0,1.0); //z axis
//    seg_.setModelType (pcl::SACMODEL_PERPENDICULAR_PLANE);
//    seg_.setAxis(axis);

    seg_.setOptimizeCoefficients(true);
    seg_.setModelType(pcl::SACMODEL_PLANE);
    seg_.setMaxIterations(max_iter_);
    seg_.setMethodType(pcl::SAC_RANSAC);
    seg_.setEpsAngle(eps_angle_ * (M_PI / 180.0f));
    seg_.setDistanceThreshold(multi_dist_thresh_);

    while (true) {

        pcl::ModelCoefficients::Ptr coefficients(new pcl::ModelCoefficients);
        pcl::PointIndices::Ptr inliers(new pcl::PointIndices);
        seg_.setInputCloud(cloud_filtered_);
        seg_.segment(*inliers, *coefficients);

        if (inliers->indices.size() == 0 and no_planes == 0) {
            std::cout << "PCP: no plane found!!!" << std::endl;
            return false;
        } else if (inliers->indices.size() < min_plane_size_) {
            break;
        }


//            std::cout << "PCP: plane coefficients : " << coefficients->values[0]  << " "
//                                                      << coefficients->values[1]  << " "
//                                                      << coefficients->values[2]  << " "
//                                                      << coefficients->values[3]  << std::endl;


        extract_.setInputCloud(cloud_filtered_);
        extract_.setNegative(false);
        extract_.setIndices(inliers);
        extract_.filter(*cloud_plane);

//        plane_proj_.setInputCloud(cloud_plane_raw);
//        plane_proj_.setModelType(pcl::SACMODEL_PLANE);
//        plane_proj_.setModelCoefficients (coefficients);
//        plane_proj_.filter (*cloud_plane);


//        removeOutliers(cloud_plane_raw, cloud_plane);

        plane_clouds += *cloud_plane;

        chull_.setInputCloud(cloud_plane);
        chull_.setDimension(2);
        chull_.reconstruct(*cloud_hull);

        Eigen::Vector4f center;
        pcl::compute3DCentroid(*cloud_hull, center);

        Eigen::Vector4f min_vals, max_vals;
        pcl::getMinMax3D(*cloud_plane, min_vals, max_vals);

        // Get cloud
        pcl::toROSMsg(*cloud_plane, plane_object_msg.cloud);

        // Construct plane object msg
        pcl_conversions::fromPCL(cloud_plane->header, plane_object_msg.header);

        // Get plane center
        plane_object_msg.center.x = center[0];
        plane_object_msg.center.y = center[1];
        plane_object_msg.center.z = center[2];

        // Get plane min and max values
        plane_object_msg.min.x = min_vals[0];
        plane_object_msg.min.y = min_vals[1];
        plane_object_msg.min.z = min_vals[2];


        plane_object_msg.max.x = max_vals[0];
        plane_object_msg.max.y = max_vals[1];
        plane_object_msg.max.z = max_vals[2];

        // Get plane polygon
        for (int i = 0; i < cloud_hull->points.size(); i++) {
            geometry_msgs::Point32 p;
            p.x = cloud_hull->points[i].x;
            p.y = cloud_hull->points[i].y;
            p.z = cloud_hull->points[i].z;
            plane_object_msg.polygon.push_back(p);
        }

        // Get plane coefficients
        plane_object_msg.coef[0] = coefficients->values[0];
        plane_object_msg.coef[1] = coefficients->values[1];
        plane_object_msg.coef[2] = coefficients->values[2];
        plane_object_msg.coef[3] = coefficients->values[3];

        std::string axis;

        if (std::abs(coefficients->values[0]) < 1.1 &&
            std::abs(coefficients->values[0]) > 0.9 &&
            std::abs(coefficients->values[1]) < 0.1 &&
            std::abs(coefficients->values[2]) < 0.1) {
            plane_object_msg.orientation = point_cloud_proc::Plane::XAXIS;
            axis = "X";
        } else if (std::abs(coefficients->values[0]) < 0.1 &&
                   std::abs(coefficients->values[1]) > 0.9 &&
                   std::abs(coefficients->values[1]) < 1.1 &&
                   std::abs(coefficients->values[2]) < 0.1) {
            plane_object_msg.orientation = point_cloud_proc::Plane::YAXIS;
            axis = "Y";
        } else if (std::abs(coefficients->values[0]) < 0.1 &&
                   std::abs(coefficients->values[1]) < 0.1 &&
                   std::abs(coefficients->values[2]) < 1.1 &&
                   std::abs(coefficients->values[2]) > 0.9) {
            plane_object_msg.orientation = point_cloud_proc::Plane::ZAXIS;
            axis = "Z";
        } else {
            plane_object_msg.orientation = point_cloud_proc::Plane::NOAXIS;
            axis = "NO";
        }

        std::cout << "PCP: " << no_planes << ". plane segmented! # of points: "
                  << inliers->indices.size() << " axis: " << axis << std::endl;
        no_planes++;

        plane_object_msg.size.data = cloud_plane->points.size();

        planes.push_back(plane_object_msg);
        extract_.setNegative(true);
        extract_.filter(*cloud_filtered_);


        ros::Duration(0.2).sleep();
    }

    if (debug_) {
        plane_cloud_pub_.publish(plane_clouds);
    }


    return true;
}

bool PointCloudProc::extractTabletop() {

    pcl::PointIndices::Ptr tabletop_indices(new pcl::PointIndices);
    prism_.setInputCloud(cloud_filtered_);
    prism_.setInputPlanarHull(cloud_hull_);
    prism_.setHeightLimits(prism_limits_[0], prism_limits_[1]);
    prism_.segment(*tabletop_indices);

    tabletop_indicies_ = tabletop_indices;

    extract_.setInputCloud(cloud_filtered_);
    extract_.setIndices(tabletop_indices);
    extract_.filter(*cloud_tabletop_);

    if (cloud_tabletop_->points.size() == 0) {
        return false;
    } else {
        if (debug_) {
            tabletop_pub_.publish(cloud_tabletop_);
        }
        return true;
    }
}

bool PointCloudProc::clusterObjects(std::vector<point_cloud_proc::Object> &objects,
                                    bool compute_normals, bool project) {

    geometry_msgs::PoseArray object_poses_rviz;
    std::cout << "PCP: clustering tabletop objects... " << std::endl;

    point_cloud_proc::Plane plane;
    if (!segmentSinglePlane(plane)) {
        std::cout << "PCP: failed to segment single plane" << std::endl;
        return false;
    }

    if (!extractTabletop()) {
        std::cout << "PCP: failed to extract tabletop" << std::endl;
        return false;
    }
    pcl::ModelCoefficients::Ptr coefficients(new pcl::ModelCoefficients);
    coefficients->values.push_back(plane.coef[0]);
    coefficients->values.push_back(plane.coef[1]);
    coefficients->values.push_back(plane.coef[2]);
    coefficients->values.push_back(plane.coef[3]);


    pcl::search::KdTree<pcl::PointXYZRGB>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZRGB>);

    tree->setInputCloud(cloud_tabletop_);
    std::vector<pcl::PointIndices> cloud_clusters;

    ec_.setClusterTolerance(cluster_tol_);
    ec_.setMinClusterSize(min_cluster_size_);
    ec_.setMaxClusterSize(max_cluster_size_);
    ec_.setSearchMethod(tree);
    ec_.setInputCloud(cloud_tabletop_);
    ec_.extract(cloud_clusters);

    pcl::PCA<PointT> pca_ = new pcl::PCA<PointT>;
    pcl::NormalEstimationOMP<PointT, PointNT> ne(4);


    if (cloud_clusters.size() == 0)
        return false;
    else
        std::cout << "PCP: number of clusters: " << cloud_clusters.size() << std::endl;

    int k = 0;
    for (auto cluster_indicies : cloud_clusters) {

        CloudT::Ptr cluster(new CloudT);
        CloudT::Ptr cluster_projected(new CloudT);
        CloudNT::Ptr cluster_normals(new CloudNT);

        pcl::PointIndices::Ptr object_indicies_ptr(new pcl::PointIndices);
        object_indicies_ptr->indices = cluster_indicies.indices;

        extract_.setInputCloud(cloud_tabletop_);
        extract_.setIndices(object_indicies_ptr);
        extract_.setNegative(false);
        extract_.filter(*cluster);

        // Compute PCA to find centeroid and orientation
//        Eigen::Matrix3f eigen_vectors;
//        Eigen::Vector3f eigen_values;
//        Eigen::Vector4f mean_values;
//        if (project) {
//            plane_proj_.setModelType(pcl::SACMODEL_PLANE);
//            plane_proj_.setModelCoefficients(coefficients);
//            plane_proj_.setInputCloud(cluster);
//            plane_proj_.filter(*cluster_projected);
//
//            pca_.setInputCloud(cluster_projected);
//            eigen_vectors = pca_.getEigenVectors();
//
//            eigen_values = pca_.getEigenValues();
//            pca_.setInputCloud(cluster);
//            mean_values = pca_.getMean();
//
//            std::cout << "PCP: eigen vectors: " << std::endl << eigen_vectors << std::endl;
//
//        } else {
//            pca_.setInputCloud(cluster);
//            eigen_vectors = pca_.getEigenVectors();
//            eigen_values = pca_.getEigenValues();
//            mean_values = pca_.getMean();
//        }
//
//        Eigen::Quaternionf quat(eigen_vectors);
//        quat.normalize();


        if (compute_normals) {
            // Compute point normals
            pcl::search::KdTree<PointT>::Ptr normals_tree(new pcl::search::KdTree<PointT>());
            ne.setInputCloud(cluster);
            ne.setSearchMethod(normals_tree);
            ne.setKSearch(k_search_);
            ne.compute(*cluster_normals);
        }

        // Find position
        Eigen::Vector4f center;
        pcl::compute3DCentroid(*cluster, center);

        // Find orientetions
        // Get max segment
        PointT pmin, pmax;
        pcl::getMaxSegment(*cluster, pmin, pmax);
//        double y_axis_norm = std::sqrt(std::pow(pmin.x-pmax.x, 2) + std::pow(pmin.y-pmax.y, 2));
        Eigen::Vector3d y_axis (pmin.x-pmax.x, pmin.y-pmax.y, 0.0);
        y_axis.normalize();
        Eigen::Vector3d z_axis (0.0, 0.0, 1.0);
        Eigen::Vector3d x_axis = y_axis.cross(x_axis);

        Eigen::Matrix3d rot;
        rot << x_axis(0), y_axis(0), z_axis(0),
               x_axis(1), y_axis(1), z_axis(1),
               x_axis(2), y_axis(2), z_axis(1);

        Eigen::Quaterniond q(rot);

        point_cloud_proc::Object object;
        // Get object point cloud
        pcl_conversions::fromPCL(cluster->header, object.header);

        // Get cloud
        pcl::toROSMsg(*cluster, object.cloud);

        if (compute_normals) {
            // Get point normals
            for (int i = 0; i < cluster_normals->points.size(); i++) {
                geometry_msgs::Vector3 normal;
                normal.x = cluster_normals->points[i].normal_x;
                normal.y = cluster_normals->points[i].normal_y;
                normal.z = cluster_normals->points[i].normal_z;
                object.normals.push_back(normal);
            }
        }


        object.pmin.x = pmin.x;
        object.pmin.y = pmin.y;
        object.pmin.z = pmin.z;

        object.pmax.x = pmax.x;
        object.pmax.y = pmax.y;
        object.pmax.z = pmax.z;

        // Get object center
        object.center.x = center[0];
        object.center.y = center[1];
        object.center.z = center[2];

        // geometry_msgs::Pose cluster_pose;
        object.pose.position.x = center[0];
        object.pose.position.y = center[1];
        object.pose.position.z = center[2];


        object.pose.orientation.x = q.x();
        object.pose.orientation.y = q.y();
        object.pose.orientation.z = q.z();
        object.pose.orientation.w = q.w();

        // Get min max points coords
        Eigen::Vector4f min_vals, max_vals;
        pcl::getMinMax3D(*cluster, min_vals, max_vals);

        object.min.x = min_vals[0];
        object.min.y = min_vals[1];
        object.min.z = min_vals[2];
        object.max.x = max_vals[0];
        object.max.y = max_vals[1];
        object.max.z = max_vals[2];

        object_poses_rviz.poses.push_back(object.pose);
        k++;

        std::cout << "PCP: # of points in object " << k << " : " << cluster->points.size() << std::endl;

        objects.push_back(object);
    }

    if (debug_) {
        object_poses_rviz.header.frame_id = cloud_tabletop_->header.frame_id;
        object_poses_pub_.publish(object_poses_rviz);
    }
    return true;
}

bool PointCloudProc::projectPointCloudToPlane(sensor_msgs::PointCloud2 &cloud_in,
                                              sensor_msgs::PointCloud2 &cloud_out,
                                              pcl::ModelCoefficientsPtr plane_coeffs) {

    CloudT::Ptr cloud_in_pcl(new CloudT);
    CloudT::Ptr cloud_out_pcl(new CloudT);
    pcl::fromROSMsg(cloud_in, *cloud_in_pcl);

    plane_proj_.setModelType(pcl::SACMODEL_PLANE);
    plane_proj_.setModelCoefficients(plane_coeffs);
    plane_proj_.setInputCloud(cloud_in_pcl);
    plane_proj_.filter(*cloud_out_pcl);

    pcl::toROSMsg(*cloud_out_pcl, cloud_out);

    return true;
}

bool PointCloudProc::get3DPoint(int col, int row, geometry_msgs::PointStamped &point) {

    if (!transformPointCloud()) {
        std::cout << "PCP: couldn't transform point cloud!" << std::endl;
        return false;
    }

    pcl_conversions::fromPCL(cloud_transformed_->header, point.header);

    if (pcl::isFinite(cloud_transformed_->at(col, row))) {
        point.point.x = cloud_transformed_->at(col, row).x;
        point.point.y = cloud_transformed_->at(col, row).y;
        point.point.z = cloud_transformed_->at(col, row).z;
        return true;
    } else {
        std::cout << "PCP: The 3D point is not valid!" << std::endl;
        return false;
    }

}

bool PointCloudProc::getObjectFromBBox(int *bbox, point_cloud_proc::Object &object) {

    if (!transformPointCloud()) {
        std::cout << "PCP: couldn't transform point cloud!" << std::endl;
        return false;
    }

    pcl_conversions::fromPCL(cloud_transformed_->header, object.header);

    CloudT::Ptr object_cloud(new CloudT);
    CloudT::Ptr object_cloud_filtered(new CloudT);
    object_cloud->header = cloud_transformed_->header;

    for (int i = bbox[0]; i < bbox[2]; i++) {
        for (int j = bbox[1]; j < bbox[3]; j++) {
            if (pcl::isFinite(cloud_transformed_->at(i, j))) {
                object_cloud->push_back(cloud_transformed_->at(i, j));
            }
        }

    }

    removeOutliers(object_cloud, object_cloud_filtered);
    if (object_cloud_filtered->empty()) {
        std::cout << "PCP: object cloud is empty after removing outliers!" << std::endl;
        return false;
    }

    Eigen::Vector4f min_vals, max_vals;

    pcl::getMinMax3D(*object_cloud_filtered, min_vals, max_vals);

    object.min.x = min_vals[0];
    object.min.y = min_vals[1];
    object.min.z = min_vals[2];
    object.max.x = max_vals[0];
    object.max.y = max_vals[1];
    object.max.z = max_vals[2];

    Eigen::Vector4f center;
    pcl::compute3DCentroid(*object_cloud_filtered, center);
    object.center.x = center[0];
    object.center.y = center[1];
    object.center.z = center[2];

    debug_cloud_pub_.publish(object_cloud_filtered);
    return true;

}

bool PointCloudProc::getObjectFromContour(const std::vector<int> &contour_x, const std::vector<int> &contour_y,
                                          point_cloud_proc::Object &object) {

    if (!transformPointCloud()) {
        std::cout << "PCP: couldn't transform point cloud!" << std::endl;
        return false;
    }

    pcl_conversions::fromPCL(cloud_transformed_->header, object.header);

    CloudT::Ptr object_cloud(new CloudT);
    CloudT::Ptr object_cloud_filtered(new CloudT);
    object_cloud->header = cloud_transformed_->header;

    std::cout << "PCP: getting object cluster from contours..." << std::endl;


    for (int i = 0; i < contour_x.size(); i++){
        if (pcl::isFinite(cloud_transformed_->at(contour_y[i], contour_x[i]))){
            object_cloud->push_back(cloud_transformed_->at(contour_y[i], contour_x[i]));
        }

    }

//    removeOutliers(object_cloud, object_cloud_filtered);
//    if (object_cloud_filtered->empty()) {
//        std::cout << "PCP: object cloud is empty after removing outliers!" << std::endl;
//        return false;
//    }

    Eigen::Vector4f min_vals, max_vals;
    pcl::getMinMax3D(*object_cloud, min_vals, max_vals);

    object.min.x = min_vals[0];
    object.min.y = min_vals[1];
    object.min.z = min_vals[2];
    object.max.x = max_vals[0];
    object.max.y = max_vals[1];
    object.max.z = max_vals[2];

    Eigen::Vector4f center;
    pcl::compute3DCentroid(*object_cloud, center);
    object.center.x = center[0];
    object.center.y = center[1];
    object.center.z = center[2];

    sensor_msgs::PointCloud2 cloud_ros;
    pcl::toROSMsg(*object_cloud, cloud_ros);

    debug_cloud_pub_.publish(cloud_ros);
    return true;
}

bool PointCloudProc::generatePoissonMesh(sensor_msgs::PointCloud2 &ros_cloud, pcl::PolygonMesh &mesh) {
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_smoothed(new pcl::PointCloud<pcl::PointXYZ>());
    pcl::fromROSMsg(ros_cloud, *cloud_smoothed);

/*    pcl::MovingLeastSquares<pcl::PointXYZ, pcl::PointXYZ> mls;
    mls.setInputCloud(cloud);
    mls.setSearchRadius(0.01);
    mls.setPolynomialFit(true);
    mls.setPolynomialOrder(2);
    mls.setUpsamplingMethod(pcl::MovingLeastSquares<pcl::PointXYZ, pcl::PointXYZ>::SAMPLE_LOCAL_PLANE);
    mls.setUpsamplingRadius(0.005);
    mls.setUpsamplingStepSize(0.003);

    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_smoothed(new pcl::PointCloud<pcl::PointXYZ> ());
    mls.process(*cloud_smoothed);*/

    pcl::NormalEstimationOMP<pcl::PointXYZ, pcl::Normal> ne;
    ne.setNumberOfThreads(8);
    ne.setInputCloud(cloud_smoothed);
    ne.setRadiusSearch(0.01);
    Eigen::Vector4f centroid;
    compute3DCentroid(*cloud_smoothed, centroid);
    ne.setViewPoint(centroid[0], centroid[1], centroid[2]);

    pcl::PointCloud<pcl::Normal>::Ptr cloud_normals(new pcl::PointCloud<pcl::Normal> ());
    ne.compute(*cloud_normals);

    for (size_t i = 0; i < cloud_normals->size (); ++i)
    {
      cloud_normals->points[i].normal_x *= -1;
      cloud_normals->points[i].normal_y *= -1;
      cloud_normals->points[i].normal_z *= -1;
    }

    std::cout << "PCP: Cloud normals calculated ";

    pcl::PointCloud<pcl::PointNormal>::Ptr cloud_smoothed_normals(new pcl::PointCloud<pcl::PointNormal> ());
    concatenateFields(*cloud_smoothed, *cloud_normals, *cloud_smoothed_normals);

    pcl::Poisson<pcl::PointNormal> poisson;
    poisson.setDepth(7);
    poisson.setInputCloud(cloud_smoothed_normals);
    //PolygonMesh mesh;
    poisson.reconstruct(mesh);

    std::cout << "PCP: poission mesh constructed";
}

bool PointCloudProc::generateMeshFromPointCloud(sensor_msgs::PointCloud2 &cloud, pcl_msgs::PolygonMesh &mesh, pcl::PolygonMesh &pcl_mesh) {

    pcl::NormalEstimationOMP<pcl::PointXYZ, PointNT> ne(6);
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_in(new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_in_filtered(new pcl::PointCloud<pcl::PointXYZ>);

    pcl::search::KdTree<pcl::PointXYZ>::Ptr tree1(new pcl::search::KdTree<pcl::PointXYZ>());
    pcl::search::KdTree<pcl::PointNormal>::Ptr tree2 (new pcl::search::KdTree<pcl::PointNormal>);


    pcl::PointCloud<pcl::Normal>::Ptr normals(new pcl::PointCloud<pcl::Normal>);
    pcl::PointCloud<pcl::PointNormal>::Ptr cloud_normals(new pcl::PointCloud<pcl::PointNormal>);

    pcl::fromROSMsg(cloud, *cloud_in);

    pcl::io::savePCDFileASCII ("test_pcd.pcd", *cloud_in);

    std::vector<int> indicies;
    pcl::removeNaNFromPointCloud(*cloud_in, *cloud_in, indicies);

//    pcl::VoxelGrid<pcl::PointXYZ> vg;
//    vg.setInputCloud(cloud_in);
//    vg.setLeafSize (0.03f, 0.03f, 0.03f);
//    vg.filter(*cloud_in_filtered);


    tree1->setInputCloud(cloud_in);
    ne.setInputCloud(cloud_in);
    ne.setSearchMethod(tree1);
    ne.setKSearch(40);
//    ne.setRadiusSearch(0.01);
    ne.compute(*normals);

    pcl::concatenateFields(*cloud_in, *normals, *cloud_normals);

//    for(size_t i = 0; i < cloud_normals->size(); ++i){
//        cloud_normals->points[i].normal_x *= -1;
//        cloud_normals->points[i].normal_y *= -1;
//        cloud_normals->points[i].normal_z *= -1;
//    }


    //pcl::PolygonMesh pcl_mesh;
    pcl::Poisson<pcl::PointNormal> ps;
    tree2->setInputCloud(cloud_normals);
    ps.setDepth (8);
    ps.setSolverDivide (8);
    ps.setIsoDivide (8);
    ps.setPointWeight (4.0f);
    ps.setInputCloud(cloud_normals);
    ps.setSearchMethod(tree2);
    ps.reconstruct(pcl_mesh);

    pcl_conversions::fromPCL(pcl_mesh, mesh);

    std::cout << "PCP: # of triangles : " << pcl_mesh.polygons.size() << std::endl;

    return true;

}

bool PointCloudProc::trianglePointCloud(sensor_msgs::PointCloud2 &cloud, pcl_msgs::PolygonMesh &mesh, pcl::PolygonMesh &triangles) {

    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_xyz(new pcl::PointCloud<pcl::PointXYZ>);
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_xyz_(new pcl::PointCloud<pcl::PointXYZ>);
//  pcl::copyPointCloud(*cloud, *cloud_xyz);
    pcl::fromROSMsg(cloud, *cloud_xyz);
/*    pcl::VoxelGrid<pcl::PointXYZ> vg;
    vg.setInputCloud(cloud_xyz_);
    //vg.setLeafSize (0.01f, 0.01f, 0.01f);
    vg.setLeafSize(0.0005f, 0.0005f, 0.0f);
    vg.filter(*cloud_xyz);*/

    // Compute point normals
    pcl::NormalEstimation<pcl::PointXYZ, pcl::Normal> ne;
    pcl::PointCloud<pcl::Normal>::Ptr normals(new pcl::PointCloud<pcl::Normal>);
    pcl::PointCloud<pcl::PointNormal>::Ptr cloud_normals(new pcl::PointCloud<pcl::PointNormal>);
    pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ>());

    tree->setInputCloud(cloud_xyz);
    ne.setInputCloud(cloud_xyz);
    ne.setSearchMethod(tree);
    ne.setKSearch(20);
    ne.compute(*normals);

    pcl::concatenateFields(*cloud_xyz, *normals, *cloud_normals);

    pcl::search::KdTree<pcl::PointNormal>::Ptr tree2(new pcl::search::KdTree<pcl::PointNormal>);
    tree2->setInputCloud(cloud_normals);

//  pcl::PolygonMesh triangles;
    //pcl::PolygonMesh::Ptr triangles(new pcl::PolygonMesh());
    gp3_.setSearchRadius(0.75);
    gp3_.setMu(3.5);
    gp3_.setMaximumNearestNeighbors(200);
    gp3_.setMaximumSurfaceAngle(M_PI / 4); // 45 degrees
    gp3_.setMinimumAngle(M_PI / 18); // 10 degrees
    gp3_.setMaximumAngle(2 * M_PI / 3); // 120 degrees
    gp3_.setNormalConsistency(false);

    gp3_.setInputCloud(cloud_normals);
    gp3_.setSearchMethod(tree2);
    gp3_.reconstruct(triangles);


    pcl_conversions::fromPCL(triangles, mesh);


//  pcl::PointCloud<pcl::PointXYZ> triangle_cloud;
//  pcl::fromPCLPointCloud2(triangles.cloud, triangle_cloud);
//  int i = 0;
//  mesh.vertices.resize(t)
//  for(auto point : triangle_cloud.points){
//    geometry_msgs::Point p;
//    p.x = point.x;
//    p.y = point.y;
//    p.z = point.z;
//
//    mesh.vertices.push_back(p);
//    triangles.polygons[i]
//  }


    return true;
}

bool PointCloudProc::trianglePointCloud_greedy(sensor_msgs::PointCloud2 &ros_cloud, pcl_msgs::PolygonMesh &mesh, pcl::PolygonMesh &triangles) {

    // Load input file into a PointCloud<T> with an appropriate type
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud (new pcl::PointCloud<pcl::PointXYZ>);
    pcl::fromROSMsg(ros_cloud, *cloud);

    std::vector<int> indicies;
    pcl::removeNaNFromPointCloud(*cloud, *cloud, indicies);
    //* the data should be available in cloud

    // Normal estimation*
    pcl::NormalEstimation<pcl::PointXYZ, pcl::Normal> n;
    pcl::PointCloud<pcl::Normal>::Ptr normals (new pcl::PointCloud<pcl::Normal>);
    pcl::search::KdTree<pcl::PointXYZ>::Ptr tree (new pcl::search::KdTree<pcl::PointXYZ>);
    tree->setInputCloud (cloud);
    n.setInputCloud (cloud);
    n.setSearchMethod (tree);
    n.setKSearch (20);
    n.compute (*normals);
    //* normals should not contain the point normals + surface curvatures

    // Concatenate the XYZ and normal fields*
    pcl::PointCloud<pcl::PointNormal>::Ptr cloud_with_normals (new pcl::PointCloud<pcl::PointNormal>);
    pcl::concatenateFields (*cloud, *normals, *cloud_with_normals);
    //* cloud_with_normals = cloud + normals

    // Create search tree*
    pcl::search::KdTree<pcl::PointNormal>::Ptr tree2 (new pcl::search::KdTree<pcl::PointNormal>);
    tree2->setInputCloud (cloud_with_normals);

    // Initialize objects
    pcl::GreedyProjectionTriangulation<pcl::PointNormal> gp3;
    //pcl::PolygonMesh triangles;

    // Set the maximum distance between connected points (maximum edge length)
    gp3.setSearchRadius (0.025); //0.025

    // Set typical values for the parameters
    gp3.setMu (2.5); //2.5
    gp3.setMaximumNearestNeighbors (500); //100
    gp3.setMaximumSurfaceAngle(M_PI/4); // 45 degrees
    gp3.setMinimumAngle(M_PI/18); // 10 degrees
    gp3.setMaximumAngle(2*M_PI/3); // 120 degrees
    gp3.setNormalConsistency(false);

    // Get result
    gp3.setInputCloud (cloud_with_normals);
    gp3.setSearchMethod (tree2);
    gp3.reconstruct (triangles);

    // Additional vertex information
    std::vector<int> parts = gp3.getPartIDs();
    std::vector<int> states = gp3.getPointStates();


    return true;
}

void PointCloudProc::getRemainingCloud(sensor_msgs::PointCloud2 &cloud) {
//  sensor_msgs::PointCloud2::Ptr cloud;
    pcl::toROSMsg(*cloud_filtered_, cloud);

//  return cloud;
}

void PointCloudProc::getFilteredCloud(sensor_msgs::PointCloud2 &cloud) {
    if (!transformPointCloud()) {
        std::cout << "PCP: couldn't transform point cloud!" << std::endl;
    }

    if (!filterPointCloud()) {
        std::cout << "PCP: couldn't filter point cloud!" << std::endl;
    }

    pcl::toROSMsg(*cloud_filtered_, cloud);
}

sensor_msgs::PointCloud2::Ptr PointCloudProc::getTabletopCloud() {
    sensor_msgs::PointCloud2::Ptr cloud;
    pcl::toROSMsg(*cloud_tabletop_, *cloud);

    return cloud;
}

PointCloudProc::CloudT::Ptr PointCloudProc::getFilteredCloud() {
//  sensor_msgs::PointCloud2::Ptr filtered_cloud;
//  pcl::toROSMsg(*cloud_filtered_, *filtered_cloud);

    return cloud_filtered_;
}

pcl::PointIndices::Ptr PointCloudProc::getTabletopIndicies() {
    return tabletop_indicies_;
}
