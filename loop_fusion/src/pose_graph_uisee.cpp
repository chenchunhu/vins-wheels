/*******************************************************
 * Copyright (C) 2019, Aerial Robotics Group, Hong Kong University of Science and Technology
 * 
 * This file is part of VINS.
 * 
 * Licensed under the GNU General Public License v3.0;
 * you may not use this file except in compliance with the License.
 *
 * Author: Qin Tong (qintonguav@gmail.com)
 *******************************************************/

#include "pose_graph_uisee.h"

PoseGraph::PoseGraph()
{
    posegraph_visualization = new CameraPoseVisualization(1.0, 0.0, 1.0, 1.0);
    posegraph_visualization->setScale(0.1);
    posegraph_visualization->setLineWidth(0.01);
    earliest_loop_index = -1;
    t_drift = Eigen::Vector3d(0, 0, 0);
    yaw_drift = 0;
    r_drift = Eigen::Matrix3d::Identity();
    w_t_vio = Eigen::Vector3d(0, 0, 0);
    w_r_vio = Eigen::Matrix3d::Identity();
    global_index = 0;
    sequence_cnt = 0;
    sequence_loop.push_back(0);
    base_sequence = 1;
    use_imu = 0;
}

PoseGraph::~PoseGraph()
{
    t_optimization.detach();
}

void PoseGraph::registerPub(ros::NodeHandle &n)
{
    pub_pg_path = n.advertise<nav_msgs::Path>("pose_graph_path", 1000);
    pub_base_path = n.advertise<nav_msgs::Path>("base_path", 1000);
    pub_pose_graph = n.advertise<visualization_msgs::MarkerArray>("pose_graph", 1000);
    for (int i = 1; i < 10; i++)
        pub_path[i] = n.advertise<nav_msgs::Path>("path_" + to_string(i), 1000);
}

void PoseGraph::setIMUFlag(bool _use_imu)
{
    use_imu = _use_imu;
    if(use_imu)
    {
        printf("VIO input, perfrom 4 DoF (x, y, z, yaw) pose graph optimization\n");
        t_optimization = std::thread(&PoseGraph::optimize4DoF, this);
    }
    else
    {
        printf("VO input, perfrom 6 DoF pose graph optimization\n");
//        t_optimization = std::thread(&PoseGraph::optimize6DoF_uisee, this);
    }

}

void PoseGraph::loadVocabulary(std::string voc_path)
{
    voc = new BriefVocabulary(voc_path);
    db.setVocabulary(*voc, false, 0);
}

void PoseGraph::addKeyFrame_uisee(KeyFrame* cur_kf, bool flag_detect_loop)//直接就回环  不做检测，为了做里程计回环而写
{
    //shift to base frame
    Vector3d vio_P_cur;
    Matrix3d vio_R_cur;
    if (sequence_cnt != cur_kf->sequence)//如果sequence_cnt != cur_kf->sequence，则新建一个新的图像序列;
    {
        sequence_cnt++;
        sequence_loop.push_back(0);
        w_t_vio = Eigen::Vector3d(0, 0, 0);// w_t_vio,w_r_vio描述的就是当前序列的第一帧，与世界坐标系之间的转换关系。
        w_r_vio = Eigen::Matrix3d::Identity();
        m_drift.lock();
        t_drift = Eigen::Vector3d(0, 0, 0);
        r_drift = Eigen::Matrix3d::Identity();
        m_drift.unlock();
    }
    //获取当前帧的位姿vio_P_cur、vio_R_cur并更新
    cur_kf->getVioPose(vio_P_cur, vio_R_cur);
    vio_P_cur = w_r_vio * vio_P_cur + w_t_vio;
    vio_R_cur = w_r_vio *  vio_R_cur;
    cur_kf->updateVioPose(vio_P_cur, vio_R_cur);
    cur_kf->index = global_index;
    global_index++;
	int loop_index = -1;
    if (flag_detect_loop)
    {
        TicToc tmp_t;
        //回环检测，返回回环候选帧的索引
//        loop_index = detectLoop(cur_kf, cur_kf->index);
        loop_index = cur_kf->loop_index;
    }
    else
    {
        addKeyFrameIntoVoc_uisee(cur_kf);
    }
    //得到匹配上关键帧后，经过计算相对位姿，并把当前帧号记录到全局优化内
    //如果存在回环候选帧，将当前帧与回环帧进行描述子匹配并计算位姿，并执行优化
	if (loop_index != -1)
	{
        printf(" %d detect loop with %d \n", cur_kf->index, loop_index);
        KeyFrame* old_kf = getKeyFrame(loop_index);//返回对应关键帧的地址,//获取回环候选帧
        // findConnection 是为了计算相对位姿，最主要的就是利用了PnPRANSAC(matched_2d_old_norm, matched_3d, status, PnP_T_old, PnP_R_old)函数，
        //并且它负责把匹配好的点发送到estimator节点中去
        if (1)//(cur_kf->findConnection(old_kf))////当前帧与回环候选帧进行描述子匹配  来确定是否是一个真正的闭环
        {
            cur_kf->has_loop = true;
            cur_kf->loop_index = old_kf->index;
//            loop_info << relative_t.x(), relative_t.y(), relative_t.z(),
//                    relative_q.w(), relative_q.x(), relative_q.y(), relative_q.z(),
//                    relative_yaw;
            if (earliest_loop_index > loop_index || earliest_loop_index == -1)//earliest_loop_index为最早的回环候选帧
                earliest_loop_index = loop_index;

            Vector3d w_P_old, w_P_cur, vio_P_cur;
            Matrix3d w_R_old, w_R_cur, vio_R_cur;
            old_kf->getVioPose(w_P_old, w_R_old);
            cur_kf->getVioPose(vio_P_cur, vio_R_cur);

            ////获取当前帧与回环帧的相对位姿relative_q、relative_t
            Vector3d relative_t;
            Quaterniond relative_q;
            relative_t = Eigen::Vector3d::Zero();//cur_kf->getLoopRelativeT();
            relative_q = Eigen::Quaterniond(1,0,0,0);//(cur_kf->getLoopRelativeQ()).toRotationMatrix();
            Eigen::Matrix4d H_odom_cam;
            H_odom_cam << -0.999852  , -0.016876,  0.00345549 , -0.0297031,
            -0.00318591  , -0.015975  , -0.999867   ,-0.508023,
            0.016929  ,  -0.99973  , 0.0159189    ,       0,
            0    ,       0    ,       0        ,   1;
            double scale = 12.270911292620692;//
//            double scale =  10.2579;
            Eigen::Matrix3d R_cam_odom=H_odom_cam.matrix().block<3,3>(0, 0);
            Eigen::Quaterniond q_cam_odom(R_cam_odom);
            Eigen::Quaterniond old_q,cur_q;
            old_q=Eigen::Quaterniond (0.95435740000000002,-0.0054029000000000004,0.2986181,0.0001283);
//            old_q=old_q*q_cam_odom.inverse();
            Eigen::Matrix3d R_old=Eigen::Matrix3d(old_q);

            cur_q=Eigen::Quaterniond (0.79456740000000003,-0.016074000000000001,0.60682400000000003,-0.0129926);
//            cur_q=cur_q*q_cam_odom.inverse();
            Eigen::Matrix3d R_cur=Eigen::Matrix3d(cur_q);

            Eigen::Vector3d t_old,t_cur;
            t_old<<-0.062686000000000006,-0.0042929999999999999,-0.23250609999999999;
            t_cur<<-0.085717399999999999,-0.0066736,-0.3196445;
            t_old=scale*t_old;
            t_cur=scale*t_cur;

            Eigen::Matrix4d T_old,T_cur;
            T_old=Eigen::Matrix4d::Identity();
            T_cur=Eigen::Matrix4d::Identity();
            T_old.matrix().block<3,3>(0,0)=R_old;
            T_cur.matrix().block<3,3>(0,0)=R_cur;
            T_old.matrix().block<3,1>(0,3)=t_old;
            T_cur.matrix().block<3,1>(0,3)=t_cur;

            Eigen::Matrix4d T_old_cur;
            T_old_cur=H_odom_cam*T_old.inverse()*T_cur*H_odom_cam.inverse();

            Eigen::Matrix3d relative_R=T_old_cur.matrix().block<3,3>(0,0);
            relative_q=Eigen::Quaterniond (relative_R);
//            relative_q=old_q.inverse() * cur_q;
            relative_t=T_old_cur.matrix().block<3,1>(0,3);
            cur_kf->loop_info << relative_t.x() ,relative_t.y(), relative_t.z(),
            relative_q.w(), relative_q.x(), relative_q.y(), relative_q.z(),
            0;


            //重新计算当前帧位姿w_P_cur、w_R_cur
            w_P_cur = w_R_old * relative_t + w_P_old;
            w_R_cur = w_R_old * relative_q;

            //回环得到的位姿和VIO位姿之间的偏移量shift_r、shift_t
            double shift_yaw;
            Matrix3d shift_r;
            Vector3d shift_t;
            // 根据old frame 和相对位姿能计算出当前帧位姿，也就能得出和已知当前帧位姿的差别
            //分别计算出shift_r, shift_t，用来更新其他帧位姿

            if(use_imu)
            {
                shift_yaw = Utility::R2ypr(w_R_cur).x() - Utility::R2ypr(vio_R_cur).x();
                shift_r = Utility::ypr2R(Vector3d(shift_yaw, 0, 0));
            }
            else
                shift_r = w_R_cur * vio_R_cur.transpose();//旋转矩阵的转置等于逆
            shift_t = w_P_cur - w_R_cur * vio_R_cur.transpose() * vio_P_cur; 
            // shift vio pose of whole sequence to the world frame将整个序列的vio姿势转移到世界坐标系
            if (old_kf->sequence != cur_kf->sequence && sequence_loop[cur_kf->sequence] == 0)//正常回环检测不执行这个
            {  
                w_r_vio = shift_r;
                w_t_vio = shift_t;
                vio_P_cur = w_r_vio * vio_P_cur + w_t_vio;//这里是不是多了？
                vio_R_cur = w_r_vio *  vio_R_cur;
                cur_kf->updateVioPose(vio_P_cur, vio_R_cur);
                list<KeyFrame*>::iterator it = keyframelist.begin();
                for (; it != keyframelist.end(); it++)   
                {
                    if((*it)->sequence == cur_kf->sequence)
                    {
                        Vector3d vio_P_cur;
                        Matrix3d vio_R_cur;
                        (*it)->getVioPose(vio_P_cur, vio_R_cur);
                        vio_P_cur = w_r_vio * vio_P_cur + w_t_vio;
                        vio_R_cur = w_r_vio *  vio_R_cur;
                        (*it)->updateVioPose(vio_P_cur, vio_R_cur);
                    }
                }
                sequence_loop[cur_kf->sequence] = 1;
            }
            //将当前帧放入优化队列中
            m_optimize_buf.lock();
            optimize_buf.push(cur_kf->index);//push后，optimize6DoF 6自由度位姿变换线程开始执行优化的程序  std::thread(&PoseGraph::optimize6DoF, this);
            m_optimize_buf.unlock();
        }
	}


	m_keyframelist.lock();
    Vector3d P;
    Matrix3d R;
//    P.x()=odom_[1];P.y()=odom_[2];P.z()=odom_[3];
    cur_kf->getVioPose(P, R); //获取VIO当前帧的位姿P、R，根据偏移量得到实际位姿
    P = r_drift * P + t_drift;//在optimize6DoF线程中进行了赋值
    R = r_drift * R;
//    std::cout<<"r_drift="<<r_drift<<"  t_drift="<<t_drift<<std::endl;
//    cur_kf->updatePose(P, R);//更新当前帧的位姿P、R到T_w_i R_w_i

    //发布path[sequence_cnt]

    std::cout<<"cur_kf->time_stamp="<<std::setprecision(17)<<cur_kf->time_stamp<<std::endl;
    Quaterniond Q{R};
    geometry_msgs::PoseStamped pose_stamped;
    pose_stamped.header.stamp =ros::Time(cur_kf->time_stamp);// ros::Time::now();//
    pose_stamped.header.frame_id = "world";
    pose_stamped.pose.position.x = P.x() + VISUALIZATION_SHIFT_X;
    pose_stamped.pose.position.y = P.y() + VISUALIZATION_SHIFT_Y;
    pose_stamped.pose.position.z = P.z();
    pose_stamped.pose.orientation.x = Q.x();
    pose_stamped.pose.orientation.y = Q.y();
    pose_stamped.pose.orientation.z = Q.z();
    pose_stamped.pose.orientation.w = Q.w();
    path[sequence_cnt].poses.push_back(pose_stamped);
    path[sequence_cnt].header = pose_stamped.header;

    //保存闭环轨迹到VINS_RESULT_PATH
    if (SAVE_LOOP_PATH)
    {
        ofstream loop_path_file(VINS_RESULT_PATH, ios::app);
        loop_path_file.setf(ios::fixed, ios::floatfield);
        loop_path_file.precision(0);
        loop_path_file << cur_kf->time_stamp << ",";
        loop_path_file.precision(5);
        loop_path_file  << P.x() << ","
              << P.y() << ","
              << P.z() << ","
              << Q.w() << ","
              << Q.x() << ","
              << Q.y() << ","
              << Q.z() << ","
              << endl;
        loop_path_file.close();
    }
    //draw local connection
    if (SHOW_S_EDGE)
    {
        list<KeyFrame*>::reverse_iterator rit = keyframelist.rbegin();
        for (int i = 0; i < 4; i++)
        {
            if (rit == keyframelist.rend())
                break;
            Vector3d conncected_P;
            Matrix3d connected_R;
            if((*rit)->sequence == cur_kf->sequence)
            {
                (*rit)->getPose(conncected_P, connected_R);
                posegraph_visualization->add_edge(P, conncected_P);
            }
            rit++;
        }
    }

    //当前帧与其回环帧连线
    if (SHOW_L_EDGE)
    {
        if (cur_kf->has_loop)
        {
            //printf("has loop \n");
            KeyFrame* connected_KF = getKeyFrame(cur_kf->loop_index);
            Vector3d connected_P,P0;
            Matrix3d connected_R,R0;
            connected_KF->getPose(connected_P, connected_R);
            //cur_kf->getVioPose(P0, R0);
            cur_kf->getPose(P0, R0);
            if(cur_kf->sequence > 0)
            {
                //printf("add loop into visual \n");
                posegraph_visualization->add_loopedge(P0, connected_P + Vector3d(VISUALIZATION_SHIFT_X, VISUALIZATION_SHIFT_Y, 0));
            }
            
        }
    }

    //posegraph_visualization->add_pose(P + Vector3d(VISUALIZATION_SHIFT_X, VISUALIZATION_SHIFT_Y, 0), Q);
    //发送path主题数据，用以显示
	keyframelist.push_back(cur_kf);
//    std::cout<<"pub_path"<<std::endl;
    publish_uisee();
	m_keyframelist.unlock();
//    std::cout<<"pub_path end"<<std::endl;

}
void PoseGraph::addKeyFrame_uisee(KeyFrame* cur_kf, bool flag_detect_loop,bool flag_input_img)//视觉检测回环
{
    //shift to base frame
    Vector3d vio_P_cur;
    Matrix3d vio_R_cur;
    if (sequence_cnt != cur_kf->sequence)//如果sequence_cnt != cur_kf->sequence，则新建一个新的图像序列;
    {
        sequence_cnt++;
        sequence_loop.push_back(0);
        w_t_vio = Eigen::Vector3d(0, 0, 0);// w_t_vio,w_r_vio描述的就是当前序列的第一帧，与世界坐标系之间的转换关系。
        w_r_vio = Eigen::Matrix3d::Identity();
        m_drift.lock();
        t_drift = Eigen::Vector3d(0, 0, 0);
        r_drift = Eigen::Matrix3d::Identity();
        m_drift.unlock();
    }
    //获取当前帧的位姿vio_P_cur、vio_R_cur并更新
    cur_kf->getVioPose(vio_P_cur, vio_R_cur);
    vio_P_cur = w_r_vio * vio_P_cur + w_t_vio;
    vio_R_cur = w_r_vio *  vio_R_cur;
    cur_kf->updateVioPose(vio_P_cur, vio_R_cur);
    cur_kf->index = global_index;
    global_index++;
    int loop_index = -1;
    if (flag_detect_loop)
    {
        TicToc tmp_t;
        //回环检测，返回回环候选帧的索引
        std::cout<<"cur_kf->index="<<cur_kf->index<<std::endl;
        loop_index = detectLoop(cur_kf, cur_kf->index);
    }
    else
    {
        addKeyFrameIntoVoc_uisee(cur_kf);
    }
    //得到匹配上关键帧后，经过计算相对位姿，并把当前帧号记录到全局优化内
    //如果存在回环候选帧，将当前帧与回环帧进行描述子匹配并计算位姿，并执行优化
    if (loop_index != -1)
    {
        printf(" %d detect loop with %d \n", cur_kf->index, loop_index);
        KeyFrame* old_kf = getKeyFrame(loop_index);//返回对应关键帧的地址,//获取回环候选帧
        // findConnection 是为了计算相对位姿，最主要的就是利用了PnPRANSAC(matched_2d_old_norm, matched_3d, status, PnP_T_old, PnP_R_old)函数，
        //并且它负责把匹配好的点发送到estimator节点中去
        if (1)//(cur_kf->findConnection(old_kf))////当前帧与回环候选帧进行描述子匹配  来确定是否是一个真正的闭环
        {
            cur_kf->has_loop = true;
            cur_kf->loop_index = old_kf->index;
//            loop_info << relative_t.x(), relative_t.y(), relative_t.z(),
//                    relative_q.w(), relative_q.x(), relative_q.y(), relative_q.z(),
//                    relative_yaw;
            if (earliest_loop_index > loop_index || earliest_loop_index == -1)//earliest_loop_index为最早的回环候选帧
                earliest_loop_index = loop_index;

            Vector3d w_P_old, w_P_cur, vio_P_cur;
            Matrix3d w_R_old, w_R_cur, vio_R_cur;
            old_kf->getVioPose(w_P_old, w_R_old);
            cur_kf->getVioPose(vio_P_cur, vio_R_cur);

            ////获取当前帧与回环帧的相对位姿relative_q、relative_t
            Vector3d relative_t;
            Quaterniond relative_q;
            relative_t = Eigen::Vector3d::Zero();//cur_kf->getLoopRelativeT();
            relative_q = Eigen::Quaterniond(1,0,0,0);//(cur_kf->getLoopRelativeQ()).toRotationMatrix();
            Eigen::Matrix4d H_odom_cam;
            H_odom_cam << -0.999852  , -0.016876,  0.00345549 , -0.0297031,
                    -0.00318591  , -0.015975  , -0.999867   ,-0.508023,
                    0.016929  ,  -0.99973  , 0.0159189    ,       0,
                    0    ,       0    ,       0        ,   1;
            double scale = 12.270911292620692;//
//            double scale =  10.2579;
            Eigen::Matrix3d R_cam_odom=H_odom_cam.matrix().block<3,3>(0, 0);
            Eigen::Quaterniond q_cam_odom(R_cam_odom);
            Eigen::Quaterniond old_q,cur_q;
            old_q=Eigen::Quaterniond (0.95435740000000002,-0.0054029000000000004,0.2986181,0.0001283);
//            old_q=old_q*q_cam_odom.inverse();
            Eigen::Matrix3d R_old=Eigen::Matrix3d(old_q);

            cur_q=Eigen::Quaterniond (0.79456740000000003,-0.016074000000000001,0.60682400000000003,-0.0129926);
//            cur_q=cur_q*q_cam_odom.inverse();
            Eigen::Matrix3d R_cur=Eigen::Matrix3d(cur_q);

            Eigen::Vector3d t_old,t_cur;
            t_old<<-0.062686000000000006,-0.0042929999999999999,-0.23250609999999999;
            t_cur<<-0.085717399999999999,-0.0066736,-0.3196445;
            t_old=scale*t_old;
            t_cur=scale*t_cur;

            Eigen::Matrix4d T_old,T_cur;
            T_old=Eigen::Matrix4d::Identity();
            T_cur=Eigen::Matrix4d::Identity();
            T_old.matrix().block<3,3>(0,0)=R_old;
            T_cur.matrix().block<3,3>(0,0)=R_cur;
            T_old.matrix().block<3,1>(0,3)=t_old;
            T_cur.matrix().block<3,1>(0,3)=t_cur;

            Eigen::Matrix4d T_old_cur;
            T_old_cur=H_odom_cam*T_old.inverse()*T_cur*H_odom_cam.inverse();

            Eigen::Matrix3d relative_R=T_old_cur.matrix().block<3,3>(0,0);
            relative_q=Eigen::Quaterniond (relative_R);
//            relative_q=old_q.inverse() * cur_q;
            relative_t=T_old_cur.matrix().block<3,1>(0,3);
            cur_kf->loop_info << relative_t.x() ,relative_t.y(), relative_t.z(),
                    relative_q.w(), relative_q.x(), relative_q.y(), relative_q.z(),
                    0;


            //重新计算当前帧位姿w_P_cur、w_R_cur
            w_P_cur = w_R_old * relative_t + w_P_old;
            w_R_cur = w_R_old * relative_q;

            //回环得到的位姿和VIO位姿之间的偏移量shift_r、shift_t
            double shift_yaw;
            Matrix3d shift_r;
            Vector3d shift_t;
            // 根据old frame 和相对位姿能计算出当前帧位姿，也就能得出和已知当前帧位姿的差别
            //分别计算出shift_r, shift_t，用来更新其他帧位姿

            if(use_imu)
            {
                shift_yaw = Utility::R2ypr(w_R_cur).x() - Utility::R2ypr(vio_R_cur).x();
                shift_r = Utility::ypr2R(Vector3d(shift_yaw, 0, 0));
            }
            else
                shift_r = w_R_cur * vio_R_cur.transpose();//旋转矩阵的转置等于逆
            shift_t = w_P_cur - w_R_cur * vio_R_cur.transpose() * vio_P_cur;
            // shift vio pose of whole sequence to the world frame将整个序列的vio姿势转移到世界坐标系
            if (old_kf->sequence != cur_kf->sequence && sequence_loop[cur_kf->sequence] == 0)//正常回环检测不执行这个
            {
                w_r_vio = shift_r;
                w_t_vio = shift_t;
                vio_P_cur = w_r_vio * vio_P_cur + w_t_vio;//这里是不是多了？
                vio_R_cur = w_r_vio *  vio_R_cur;
                cur_kf->updateVioPose(vio_P_cur, vio_R_cur);
                list<KeyFrame*>::iterator it = keyframelist.begin();
                for (; it != keyframelist.end(); it++)
                {
                    if((*it)->sequence == cur_kf->sequence)
                    {
                        Vector3d vio_P_cur;
                        Matrix3d vio_R_cur;
                        (*it)->getVioPose(vio_P_cur, vio_R_cur);
                        vio_P_cur = w_r_vio * vio_P_cur + w_t_vio;
                        vio_R_cur = w_r_vio *  vio_R_cur;
                        (*it)->updateVioPose(vio_P_cur, vio_R_cur);
                    }
                }
                sequence_loop[cur_kf->sequence] = 1;
            }
            //将当前帧放入优化队列中
            m_optimize_buf.lock();
            optimize_buf.push(cur_kf->index);//push后，optimize6DoF 6自由度位姿变换线程开始执行优化的程序  std::thread(&PoseGraph::optimize6DoF, this);
            m_optimize_buf.unlock();
        }
    }


    m_keyframelist.lock();
    Vector3d P;
    Matrix3d R;
//    P.x()=odom_[1];P.y()=odom_[2];P.z()=odom_[3];
    cur_kf->getVioPose(P, R); //获取VIO当前帧的位姿P、R，根据偏移量得到实际位姿
    P = r_drift * P + t_drift;//在optimize6DoF线程中进行了赋值
    R = r_drift * R;
//    std::cout<<"r_drift="<<r_drift<<"  t_drift="<<t_drift<<std::endl;
//    cur_kf->updatePose(P, R);//更新当前帧的位姿P、R到T_w_i R_w_i

    //发布path[sequence_cnt]

    std::cout<<"cur_kf->time_stamp="<<std::setprecision(17)<<cur_kf->time_stamp<<std::endl;
    Quaterniond Q{R};
    geometry_msgs::PoseStamped pose_stamped;
    pose_stamped.header.stamp =ros::Time(cur_kf->time_stamp);// ros::Time::now();//
    pose_stamped.header.frame_id = "world";
    pose_stamped.pose.position.x = P.x() + VISUALIZATION_SHIFT_X;
    pose_stamped.pose.position.y = P.y() + VISUALIZATION_SHIFT_Y;
    pose_stamped.pose.position.z = P.z();
    pose_stamped.pose.orientation.x = Q.x();
    pose_stamped.pose.orientation.y = Q.y();
    pose_stamped.pose.orientation.z = Q.z();
    pose_stamped.pose.orientation.w = Q.w();
    path[sequence_cnt].poses.push_back(pose_stamped);
    path[sequence_cnt].header = pose_stamped.header;

    //保存闭环轨迹到VINS_RESULT_PATH
    if (SAVE_LOOP_PATH)
    {
        ofstream loop_path_file(VINS_RESULT_PATH, ios::app);
        loop_path_file.setf(ios::fixed, ios::floatfield);
        loop_path_file.precision(0);
        loop_path_file << cur_kf->time_stamp << ",";
        loop_path_file.precision(5);
        loop_path_file  << P.x() << ","
                        << P.y() << ","
                        << P.z() << ","
                        << Q.w() << ","
                        << Q.x() << ","
                        << Q.y() << ","
                        << Q.z() << ","
                        << endl;
        loop_path_file.close();
    }
    //draw local connection
    if (SHOW_S_EDGE)
    {
        list<KeyFrame*>::reverse_iterator rit = keyframelist.rbegin();
        for (int i = 0; i < 4; i++)
        {
            if (rit == keyframelist.rend())
                break;
            Vector3d conncected_P;
            Matrix3d connected_R;
            if((*rit)->sequence == cur_kf->sequence)
            {
                (*rit)->getPose(conncected_P, connected_R);
                posegraph_visualization->add_edge(P, conncected_P);
            }
            rit++;
        }
    }

    //当前帧与其回环帧连线
    if (SHOW_L_EDGE)
    {
        if (cur_kf->has_loop)
        {
            //printf("has loop \n");
            KeyFrame* connected_KF = getKeyFrame(cur_kf->loop_index);
            Vector3d connected_P,P0;
            Matrix3d connected_R,R0;
            connected_KF->getPose(connected_P, connected_R);
            //cur_kf->getVioPose(P0, R0);
            cur_kf->getPose(P0, R0);
            if(cur_kf->sequence > 0)
            {
                //printf("add loop into visual \n");
                posegraph_visualization->add_loopedge(P0, connected_P + Vector3d(VISUALIZATION_SHIFT_X, VISUALIZATION_SHIFT_Y, 0));
            }

        }
    }

    //posegraph_visualization->add_pose(P + Vector3d(VISUALIZATION_SHIFT_X, VISUALIZATION_SHIFT_Y, 0), Q);
    //发送path主题数据，用以显示
    keyframelist.push_back(cur_kf);
//    std::cout<<"pub_path"<<std::endl;
    publish_uisee();
    m_keyframelist.unlock();
//    std::cout<<"pub_path end"<<std::endl;

}



void PoseGraph::loadKeyFrame(KeyFrame* cur_kf, bool flag_detect_loop)
{
    cur_kf->index = global_index;
    global_index++;
    int loop_index = -1;
    if (flag_detect_loop)
       loop_index = detectLoop(cur_kf, cur_kf->index);
    else
    {
        addKeyFrameIntoVoc_uisee(cur_kf);
    }
    if (loop_index != -1)
    {
        printf(" %d detect loop with %d \n", cur_kf->index, loop_index);
        KeyFrame* old_kf = getKeyFrame(loop_index);
        if (cur_kf->findConnection(old_kf))
        {
            if (earliest_loop_index > loop_index || earliest_loop_index == -1)
                earliest_loop_index = loop_index;
            m_optimize_buf.lock();
            optimize_buf.push(cur_kf->index);
            m_optimize_buf.unlock();
        }
    }
    m_keyframelist.lock();
    Vector3d P;
    Matrix3d R;
    cur_kf->getPose(P, R);
    Quaterniond Q{R};
    geometry_msgs::PoseStamped pose_stamped;
    pose_stamped.header.stamp = ros::Time(cur_kf->time_stamp);
    pose_stamped.header.frame_id = "world";
    pose_stamped.pose.position.x = P.x() + VISUALIZATION_SHIFT_X;
    pose_stamped.pose.position.y = P.y() + VISUALIZATION_SHIFT_Y;
    pose_stamped.pose.position.z = P.z();
    pose_stamped.pose.orientation.x = Q.x();
    pose_stamped.pose.orientation.y = Q.y();
    pose_stamped.pose.orientation.z = Q.z();
    pose_stamped.pose.orientation.w = Q.w();
    base_path.poses.push_back(pose_stamped);
    base_path.header = pose_stamped.header;

    //draw local connection
    if (SHOW_S_EDGE)
    {
        list<KeyFrame*>::reverse_iterator rit = keyframelist.rbegin();
        for (int i = 0; i < 1; i++)
        {
            if (rit == keyframelist.rend())
                break;
            Vector3d conncected_P;
            Matrix3d connected_R;
            if((*rit)->sequence == cur_kf->sequence)
            {
                (*rit)->getPose(conncected_P, connected_R);
                posegraph_visualization->add_edge(P, conncected_P);
            }
            rit++;
        }
    }
    /*
    if (cur_kf->has_loop)
    {
        KeyFrame* connected_KF = getKeyFrame(cur_kf->loop_index);
        Vector3d connected_P;
        Matrix3d connected_R;
        connected_KF->getPose(connected_P,  connected_R);
        posegraph_visualization->add_loopedge(P, connected_P, SHIFT);
    }
    */

    keyframelist.push_back(cur_kf);
    //publish();
    m_keyframelist.unlock();
}

KeyFrame* PoseGraph::getKeyFrame(int index)
{
//    unique_lock<mutex> lock(m_keyframelist);
    list<KeyFrame*>::iterator it = keyframelist.begin();
    for (; it != keyframelist.end(); it++)   
    {
        cout<<"(*it)->index"<<(*it)->index<<endl;
        if((*it)->index == index)
            break;
    }
    if (it != keyframelist.end())
    {
        cout<<"find (*it)->index"<<(*it)->index<<endl;
        return *it;
    }
    else
        return NULL;
}

int PoseGraph::detectLoop(KeyFrame* keyframe, int frame_index)//输入关键帧和关键帧的索引
{
    // put image into image_pool; for visualization将图像放入图像池；以便可视化
    cv::Mat compressed_image;
    if (DEBUG_IMAGE)//如果在调试状态DEBUG_IMAGE 就是1在config文件里写入
    {
        int feature_num = keyframe->keypoints.size();
        cv::resize(keyframe->image, compressed_image, cv::Size(376, 240));
        //putText(compressed_image, "feature_num:" + to_string(feature_num), cv::Point2f(10, 10), CV_FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0));
        image_pool[frame_index] = compressed_image;//放入图像池 写完字放入图像池？
    }
    TicToc tmp_t;
    //first query; then add this frame into database! //首先查询；然后将此坐标系添加到数据库中！
    QueryResults ret;//    查询的多个结果
    TicToc t_query;
    //第一个参数是描述子，第二个是检测结果，第三个是结果个数，第四个是结果帧号必须小于此  ret=1 result:<EntryId: 18, Score: 0.113851>
    db.query(keyframe->brief_descriptors, ret, 4, frame_index - 50);
    printf("query time: %f\n", t_query.toc());//输出查询的时间
    cout << "  Searching for Image " << frame_index << ". " << ret <<"   db.size="<<db.size()<< endl;

    TicToc t_add;
    db.add(keyframe->brief_descriptors);//属于namespace DBoW2
    //printf("add feature time: %f", t_add.toc());
    // ret[0] is the nearest neighbour's score. threshold change with neighour score
    bool find_loop = false;
    cv::Mat loop_result;
    if (DEBUG_IMAGE)
    {
        loop_result = compressed_image.clone();
        if (ret.size() > 0)
        {
            int gap = 10;
            cv::Mat notation(50, loop_result.cols, CV_8UC1, cv::Scalar(255));
            putText(notation, "neighbour score:" + to_string(ret[0].Score) + "  index:"+to_string(frame_index), cv::Point2f(10, 20), CV_FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0));
            cv::vconcat(notation,loop_result, loop_result);
        }

    }
    // visual loop result 
    if (DEBUG_IMAGE)
    {
        for (unsigned int i = 0; i < ret.size(); i++)
        {
            int gap = 10;
            int tmp_index = ret[i].Id;
            auto it = image_pool.find(tmp_index);
            cv::Mat tmp_image = (it->second).clone();
            cv::Mat notation(50, tmp_image.cols, CV_8UC1, cv::Scalar(255));
            putText(notation, "index:  " + to_string(tmp_index) + "   loop score:" + to_string(ret[i].Score), cv::Point2f(10, 30), CV_FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0));
            cv::vconcat(notation,tmp_image, tmp_image);
            cv::hconcat(loop_result, tmp_image, loop_result);
        }
    }
    // a good match with its nerghbour    //找到最小帧号的匹配帧
    if (ret.size() >= 1 &&ret[0].Score > 0.05)
        for (unsigned int i = 1; i < ret.size(); i++)
        {
            //if (ret[i].Score > ret[0].Score * 0.3)
            if (ret[i].Score > 0.015)
            {          
                find_loop = true;
                int tmp_index = ret[i].Id;
                if (DEBUG_IMAGE && 1)
                {
                    int gap = 10;
                    auto it = image_pool.find(tmp_index);
                    cv::Mat tmp_image = (it->second).clone();
                    cv::Mat notation(50, tmp_image.cols, CV_8UC1, cv::Scalar(255));
                    putText(notation, "loop score: " + to_string(ret[i].Score) + "   index:"+to_string(tmp_index), cv::Point2f(10, 30), CV_FONT_HERSHEY_SIMPLEX, 1, cv::Scalar(255), 3);
                    //putText(tmp_image, "loop score:" + to_string(ret[i].Score)+"   index:"+to_string(tmp_index), cv::Point2f(10, 50), CV_FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0));
                    cv::vconcat(notation,tmp_image, tmp_image);
                    cv::hconcat(loop_result, tmp_image, loop_result);//简单的拼接
                }
            }

        }
/**/
    if (DEBUG_IMAGE)
    {
        cv::imshow("loop_result", loop_result);
        cv::waitKey(20);
    }
/**/
    if (find_loop && frame_index > 50)
    {
        int min_index = -1;
        for (unsigned int i = 0; i < ret.size(); i++)
        {
            if (min_index == -1 || (ret[i].Id < min_index && ret[i].Score > 0.015))
                min_index = ret[i].Id;
        }
        return min_index;
    }
    else
        return -1;

}

void PoseGraph::addKeyFrameIntoVoc_uisee(KeyFrame* keyframe)
{
    // put image into image_pool; for visualization
    cv::Mat compressed_image;
    /*
    if (DEBUG_IMAGE)
    {
        int feature_num = keyframe->keypoints.size();
        cv::resize(keyframe->image, compressed_image, cv::Size(376, 240));
        putText(compressed_image, "feature_num:" + to_string(feature_num), cv::Point2f(10, 10), CV_FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(255));
        image_pool[keyframe->index] = compressed_image;
    }
     */

//    db.add(keyframe->brief_descriptors);
}

void PoseGraph::optimize4DoF()
{
    while(true)
    {
        int cur_index = -1;
        int first_looped_index = -1;
        m_optimize_buf.lock();
        while(!optimize_buf.empty())
        {
            cur_index = optimize_buf.front();
            first_looped_index = earliest_loop_index;
            optimize_buf.pop();
        }
        m_optimize_buf.unlock();
        if (cur_index != -1)
        {
            printf("optimize pose graph \n");
            TicToc tmp_t;
            m_keyframelist.lock();
            KeyFrame* cur_kf = getKeyFrame(cur_index);

            int max_length = cur_index + 1;

            // w^t_i   w^q_i
            double t_array[max_length][3];
            Quaterniond q_array[max_length];
            double euler_array[max_length][3];
            double sequence_array[max_length];

            ceres::Problem problem;
            ceres::Solver::Options options;
            options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
            //options.minimizer_progress_to_stdout = true;
            //options.max_solver_time_in_seconds = SOLVER_TIME * 3;
            options.max_num_iterations = 5;
            ceres::Solver::Summary summary;
            ceres::LossFunction *loss_function;
            loss_function = new ceres::HuberLoss(0.1);
            //loss_function = new ceres::CauchyLoss(1.0);
            ceres::LocalParameterization* angle_local_parameterization =
                AngleLocalParameterization::Create();

            list<KeyFrame*>::iterator it;

            int i = 0;
            for (it = keyframelist.begin(); it != keyframelist.end(); it++)
            {
                if ((*it)->index < first_looped_index)
                    continue;
                (*it)->local_index = i;
                Quaterniond tmp_q;
                Matrix3d tmp_r;
                Vector3d tmp_t;
                (*it)->getVioPose(tmp_t, tmp_r);
                tmp_q = tmp_r;
                t_array[i][0] = tmp_t(0);
                t_array[i][1] = tmp_t(1);
                t_array[i][2] = tmp_t(2);
                q_array[i] = tmp_q;

                Vector3d euler_angle = Utility::R2ypr(tmp_q.toRotationMatrix());
                euler_array[i][0] = euler_angle.x();
                euler_array[i][1] = euler_angle.y();
                euler_array[i][2] = euler_angle.z();

                sequence_array[i] = (*it)->sequence;

                problem.AddParameterBlock(euler_array[i], 1, angle_local_parameterization);
                problem.AddParameterBlock(t_array[i], 3);

                if ((*it)->index == first_looped_index || (*it)->sequence == 0)
                {   
                    problem.SetParameterBlockConstant(euler_array[i]);
                    problem.SetParameterBlockConstant(t_array[i]);
                }

                //add edge
                for (int j = 1; j < 5; j++)
                {
                  if (i - j >= 0 && sequence_array[i] == sequence_array[i-j])
                  {
                    Vector3d euler_conncected = Utility::R2ypr(q_array[i-j].toRotationMatrix());
                    Vector3d relative_t(t_array[i][0] - t_array[i-j][0], t_array[i][1] - t_array[i-j][1], t_array[i][2] - t_array[i-j][2]);
                    relative_t = q_array[i-j].inverse() * relative_t;
                    double relative_yaw = euler_array[i][0] - euler_array[i-j][0];
                    ceres::CostFunction* cost_function = FourDOFError::Create( relative_t.x(), relative_t.y(), relative_t.z(),
                                                   relative_yaw, euler_conncected.y(), euler_conncected.z());
                    problem.AddResidualBlock(cost_function, NULL, euler_array[i-j], 
                                            t_array[i-j], 
                                            euler_array[i], 
                                            t_array[i]);
                  }
                }

                //add loop edge
                
                if((*it)->has_loop)
                {
                    assert((*it)->loop_index >= first_looped_index);
                    int connected_index = getKeyFrame((*it)->loop_index)->local_index;
                    Vector3d euler_conncected = Utility::R2ypr(q_array[connected_index].toRotationMatrix());
                    Vector3d relative_t;
                    relative_t = (*it)->getLoopRelativeT();
                    double relative_yaw = (*it)->getLoopRelativeYaw();
                    ceres::CostFunction* cost_function = FourDOFWeightError::Create( relative_t.x(), relative_t.y(), relative_t.z(),
                                                                               relative_yaw, euler_conncected.y(), euler_conncected.z());
                    problem.AddResidualBlock(cost_function, loss_function, euler_array[connected_index], 
                                                                  t_array[connected_index], 
                                                                  euler_array[i], 
                                                                  t_array[i]);
                    
                }
                
                if ((*it)->index == cur_index)
                    break;
                i++;
            }
            m_keyframelist.unlock();

            ceres::Solve(options, &problem, &summary);
            //std::cout << summary.BriefReport() << "\n";
            
            //printf("pose optimization time: %f \n", tmp_t.toc());
            /*
            for (int j = 0 ; j < i; j++)
            {
                printf("optimize i: %d p: %f, %f, %f\n", j, t_array[j][0], t_array[j][1], t_array[j][2] );
            }
            */
            m_keyframelist.lock();
            i = 0;
            for (it = keyframelist.begin(); it != keyframelist.end(); it++)
            {
                if ((*it)->index < first_looped_index)
                    continue;
                Quaterniond tmp_q;
                tmp_q = Utility::ypr2R(Vector3d(euler_array[i][0], euler_array[i][1], euler_array[i][2]));
                Vector3d tmp_t = Vector3d(t_array[i][0], t_array[i][1], t_array[i][2]);
                Matrix3d tmp_r = tmp_q.toRotationMatrix();
                (*it)-> updatePose(tmp_t, tmp_r);

                if ((*it)->index == cur_index)
                    break;
                i++;
            }

            Vector3d cur_t, vio_t;
            Matrix3d cur_r, vio_r;
            cur_kf->getPose(cur_t, cur_r);
            cur_kf->getVioPose(vio_t, vio_r);
            m_drift.lock();
            yaw_drift = Utility::R2ypr(cur_r).x() - Utility::R2ypr(vio_r).x();
            r_drift = Utility::ypr2R(Vector3d(yaw_drift, 0, 0));
            t_drift = cur_t - r_drift * vio_t;
            m_drift.unlock();
            // cout << "t_drift " << t_drift.transpose() << endl;
            // cout << "r_drift " << Utility::R2ypr(r_drift).transpose() << endl;
            // cout << "yaw drift " << yaw_drift << endl;

            it++;
            for (; it != keyframelist.end(); it++)
            {
                Vector3d P;
                Matrix3d R;
                (*it)->getVioPose(P, R);
                P = r_drift * P + t_drift;
                R = r_drift * R;
                (*it)->updatePose(P, R);
            }
            m_keyframelist.unlock();
            updatePath();
        }

        std::chrono::milliseconds dura(2000);
        std::this_thread::sleep_for(dura);
    }
    return;
}

void PoseGraph::optimize6DoF_uisee()
{
        int cur_index = -1;
        int first_looped_index = -1;
//        m_optimize_buf.lock();
        cur_index=51;
        first_looped_index=1;
        while(!optimize_buf.empty())
        {
            cur_index = optimize_buf.front();
    //            cur_index=360;
            first_looped_index = earliest_loop_index;
    //            first_looped_index=1;
            optimize_buf.pop();
        }
//        printf("optimize pose graph \n");
//        TicToc tmp_t;
        KeyFrame* cur_kf = getKeyFrame(cur_index);
        std:cout<<"cur_kf"<<cur_kf->index<<std::endl;
        int max_length = cur_index + 1;
//        // w^t_i   w^q_i
        double t_array[max_length][3];
        double q_array[max_length][4];
        double sequence_array[max_length];
        printf("optimize ceres::Problem \n");
        ceres::Problem problem;
        ceres::Solver::Options options;
        options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
        //ptions.minimizer_progress_to_stdout = true;
        //options.max_solver_time_in_seconds = SOLVER_TIME * 3;
        options.max_num_iterations = 5;
        ceres::Solver::Summary summary;
        ceres::LossFunction *loss_function;
        loss_function = new ceres::HuberLoss(0.1);
        //loss_function = new ceres::CauchyLoss(1.0);
        ceres::LocalParameterization* local_parameterization = new ceres::QuaternionParameterization();
        list<KeyFrame*>::iterator it;
        int i = 0;

        for (it = keyframelist.begin(); it != keyframelist.end(); it++)//一次遍历所有帧获取回环起始后的所有帧位姿
        {
            if ((*it)->index < first_looped_index)
                continue;
            (*it)->local_index = i;
            Quaterniond tmp_q;
            Matrix3d tmp_r;
            Vector3d tmp_t;
            (*it)->getVioPose(tmp_t, tmp_r);
            tmp_q = tmp_r;
            t_array[i][0] = tmp_t(0);
            t_array[i][1] = tmp_t(1);
            t_array[i][2] = tmp_t(2);
            q_array[i][0] = tmp_q.w();
            q_array[i][1] = tmp_q.x();
            q_array[i][2] = tmp_q.y();
            q_array[i][3] = tmp_q.z();

            sequence_array[i] = (*it)->sequence;

            problem.AddParameterBlock(q_array[i], 4, local_parameterization);//帧位姿优化
            problem.AddParameterBlock(t_array[i], 3);

            if ((*it)->index == first_looped_index || (*it)->sequence == 0)
            {
                problem.SetParameterBlockConstant(q_array[i]);
                problem.SetParameterBlockConstant(t_array[i]);
            }

            //add edge
            for (int j = 1; j < 5; j++)
            {
                if (i - j >= 0 && sequence_array[i] == sequence_array[i-j])
                {
                    Vector3d relative_t(t_array[i][0] - t_array[i-j][0], t_array[i][1] - t_array[i-j][1], t_array[i][2] - t_array[i-j][2]);
                    Quaterniond q_i_j = Quaterniond(q_array[i-j][0], q_array[i-j][1], q_array[i-j][2], q_array[i-j][3]);
                    Quaterniond q_i = Quaterniond(q_array[i][0], q_array[i][1], q_array[i][2], q_array[i][3]);
                    relative_t = q_i_j.inverse() * relative_t;
                    Quaterniond relative_q = q_i_j.inverse() * q_i;
                    ceres::CostFunction* vo_function = RelativeRTError::Create(relative_t.x(), relative_t.y(), relative_t.z(),
                                                                               relative_q.w(), relative_q.x(), relative_q.y(), relative_q.z(),
                                                                               0.1, 0.01);
                    problem.AddResidualBlock(vo_function, NULL, q_array[i-j], t_array[i-j], q_array[i], t_array[i]);
                }
            }

            //add loop edge
            if((*it)->has_loop)
                std::cout<<"(*it)->index="<<(*it)->index<<"  (*it)->has_loop="<<(*it)->has_loop<<std::endl;
            if((*it)->has_loop)
            {
                assert((*it)->loop_index >= first_looped_index);
                int connected_index = getKeyFrame((*it)->loop_index)->local_index;
                Vector3d relative_t;
                relative_t = (*it)->getLoopRelativeT();
//                relative_t =Eigen::Vector3d(20,20,20);
                Quaterniond relative_q;
                relative_q = (*it)->getLoopRelativeQ();
                ceres::CostFunction* loop_function = RelativeRTError::Create(relative_t.x(), relative_t.y(), relative_t.z(),
                                                                             relative_q.w(), relative_q.x(), relative_q.y(), relative_q.z(),
                                                                             0.1, 0.01);
//                problem.AddResidualBlock(loop_function, loss_function, q_array[connected_index], t_array[connected_index], q_array[i], t_array[i]);
                problem.AddResidualBlock(loop_function, NULL, q_array[connected_index], t_array[connected_index], q_array[i], t_array[i]);

            }
            if ((*it)->index == cur_index)
                break;
            i++;

        }

//            m_keyframelist.unlock();
        ceres::Solve(options, &problem, &summary);
        std::cout << summary.BriefReport() << "\n";

        //printf("pose optimization time: %f \n", tmp_t.toc());
        for (int j = 0 ; j < i; j++)
        {
            printf("optimize i: %d p: %f, %f, %f\n", j, t_array[j][0], t_array[j][1], t_array[j][2] );
        }
//        m_keyframelist.lock();
        i = 0;
        for (it = keyframelist.begin(); it != keyframelist.end(); it++)
        {
            if ((*it)->index < first_looped_index)
                continue;
            Quaterniond tmp_q(q_array[i][0], q_array[i][1], q_array[i][2], q_array[i][3]);
            Vector3d tmp_t = Vector3d(t_array[i][0], t_array[i][1], t_array[i][2]);
            Matrix3d tmp_r = tmp_q.toRotationMatrix();
            (*it)-> updatePose(tmp_t, tmp_r);

            if ((*it)->index == cur_index)
                break;
            i++;
        }

        Vector3d cur_t, vio_t;
        Matrix3d cur_r, vio_r;
        cur_kf->getPose(cur_t, cur_r);//获取世界系下坐标
        cur_kf->getVioPose(vio_t, vio_r);//获取VIO坐标
        m_drift.lock();
        r_drift = cur_r * vio_r.transpose();//在process线程里面的子函数用到了
        t_drift = cur_t - r_drift * vio_t;
        m_drift.unlock();
//        cout << "t_drift " << t_drift.transpose() << endl;
//        cout << "r_drift " << Utility::R2ypr(r_drift).transpose() << endl;

        it++;
        for (; it != keyframelist.end(); it++)//将所有位姿改一遍
        {
            Vector3d P;
            Matrix3d R;
            (*it)->getVioPose(P, R);
            P = r_drift * P + t_drift;
            R = r_drift * R;
            (*it)->updatePose(P, R);
        }
//        m_keyframelist.unlock();
        updatePath();//更新ROSmsg的path


//        std::chrono::milliseconds dura(2000);
//        std::this_thread::sleep_for(dura);
        std::cout<<"optimize successed"<<std::endl;
    return;
}

void PoseGraph::optimize6DoF()
{
    while(true)
    {
        int cur_index = -1;
        int first_looped_index = -1;
        m_optimize_buf.lock();
        while(!optimize_buf.empty())
        {
            cur_index = optimize_buf.front();
//            cur_index=360;
            first_looped_index = earliest_loop_index;
//            first_looped_index=1;
            optimize_buf.pop();
        }
        m_optimize_buf.unlock();
        if (cur_index != -1)
        {
            printf("optimize pose graph \n");
            printf("optimize pose graph \n");
            printf("optimize pose graph \n");
            printf("optimize pose graph \n");
            printf("optimize pose graph \n");

            TicToc tmp_t;
            m_keyframelist.lock();
            KeyFrame* cur_kf = getKeyFrame(cur_index);

            int max_length = cur_index + 1;

            // w^t_i   w^q_i
            double t_array[max_length][3];
            double q_array[max_length][4];
            double sequence_array[max_length];

            ceres::Problem problem;
            ceres::Solver::Options options;
            options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
            //ptions.minimizer_progress_to_stdout = true;
            //options.max_solver_time_in_seconds = SOLVER_TIME * 3;
            options.max_num_iterations = 5;
            ceres::Solver::Summary summary;
            ceres::LossFunction *loss_function;
            loss_function = new ceres::HuberLoss(0.1);
            //loss_function = new ceres::CauchyLoss(1.0);
            ceres::LocalParameterization* local_parameterization = new ceres::QuaternionParameterization();

            list<KeyFrame*>::iterator it;

            int i = 0;
            for (it = keyframelist.begin(); it != keyframelist.end(); it++)//一次遍历所有帧获取回环起始后的所有帧位姿
            {
                if ((*it)->index < first_looped_index)
                    continue;
                (*it)->local_index = i;
                Quaterniond tmp_q;
                Matrix3d tmp_r;
                Vector3d tmp_t;
                (*it)->getVioPose(tmp_t, tmp_r);
                tmp_q = tmp_r;
                t_array[i][0] = tmp_t(0);
                t_array[i][1] = tmp_t(1);
                t_array[i][2] = tmp_t(2);
                q_array[i][0] = tmp_q.w();
                q_array[i][1] = tmp_q.x();
                q_array[i][2] = tmp_q.y();
                q_array[i][3] = tmp_q.z();

                sequence_array[i] = (*it)->sequence;

                problem.AddParameterBlock(q_array[i], 4, local_parameterization);//帧位姿优化
                problem.AddParameterBlock(t_array[i], 3);

                if ((*it)->index == first_looped_index || (*it)->sequence == 0)
                {   
                    problem.SetParameterBlockConstant(q_array[i]);
                    problem.SetParameterBlockConstant(t_array[i]);
                }

                //add edge
                for (int j = 1; j < 5; j++)
                {
                    if (i - j >= 0 && sequence_array[i] == sequence_array[i-j])
                    {
                        Vector3d relative_t(t_array[i][0] - t_array[i-j][0], t_array[i][1] - t_array[i-j][1], t_array[i][2] - t_array[i-j][2]);
                        Quaterniond q_i_j = Quaterniond(q_array[i-j][0], q_array[i-j][1], q_array[i-j][2], q_array[i-j][3]);
                        Quaterniond q_i = Quaterniond(q_array[i][0], q_array[i][1], q_array[i][2], q_array[i][3]);
                        relative_t = q_i_j.inverse() * relative_t;
                        Quaterniond relative_q = q_i_j.inverse() * q_i;
                        ceres::CostFunction* vo_function = RelativeRTError::Create(relative_t.x(), relative_t.y(), relative_t.z(),
                                                                                relative_q.w(), relative_q.x(), relative_q.y(), relative_q.z(),
                                                                                0.1, 0.01);
                        problem.AddResidualBlock(vo_function, NULL, q_array[i-j], t_array[i-j], q_array[i], t_array[i]);
                    }
                }

                //add loop edge
                
                if((*it)->has_loop)
                {
                    assert((*it)->loop_index >= first_looped_index);
                    int connected_index = getKeyFrame((*it)->loop_index)->local_index;
                    Vector3d relative_t;
                    relative_t = (*it)->getLoopRelativeT();
                    Quaterniond relative_q;
                    relative_q = (*it)->getLoopRelativeQ();
                    ceres::CostFunction* loop_function = RelativeRTError::Create(relative_t.x(), relative_t.y(), relative_t.z(),
                                                                                relative_q.w(), relative_q.x(), relative_q.y(), relative_q.z(),
                                                                                0.1, 0.01);
                    problem.AddResidualBlock(loop_function, loss_function, q_array[connected_index], t_array[connected_index], q_array[i], t_array[i]);                    
                }
                
                if ((*it)->index == cur_index)
                    break;
                i++;
            }
            m_keyframelist.unlock();

            ceres::Solve(options, &problem, &summary);
            //std::cout << summary.BriefReport() << "\n";
            
            //printf("pose optimization time: %f \n", tmp_t.toc());
            /*
            for (int j = 0 ; j < i; j++)
            {
                printf("optimize i: %d p: %f, %f, %f\n", j, t_array[j][0], t_array[j][1], t_array[j][2] );
            }
            */
            m_keyframelist.lock();
            i = 0;
            for (it = keyframelist.begin(); it != keyframelist.end(); it++)
            {
                if ((*it)->index < first_looped_index)
                    continue;
                Quaterniond tmp_q(q_array[i][0], q_array[i][1], q_array[i][2], q_array[i][3]);
                Vector3d tmp_t = Vector3d(t_array[i][0], t_array[i][1], t_array[i][2]);
                Matrix3d tmp_r = tmp_q.toRotationMatrix();
                (*it)-> updatePose(tmp_t, tmp_r);

                if ((*it)->index == cur_index)
                    break;
                i++;
            }

            Vector3d cur_t, vio_t;
            Matrix3d cur_r, vio_r;
            cur_kf->getPose(cur_t, cur_r);//获取世界系下坐标
            cur_kf->getVioPose(vio_t, vio_r);//获取VIO坐标
            m_drift.lock();
            r_drift = cur_r * vio_r.transpose();//在process线程里面的子函数用到了
            t_drift = cur_t - r_drift * vio_t;
            m_drift.unlock();
//            cout << "t_drift " << t_drift.transpose() << endl;
//            cout << "r_drift " << Utility::R2ypr(r_drift).transpose() << endl;

            it++;
            for (; it != keyframelist.end(); it++)//将所有位姿改一遍
            {
                Vector3d P;
                Matrix3d R;
                (*it)->getVioPose(P, R);
                P = r_drift * P + t_drift;
                R = r_drift * R;
                (*it)->updatePose(P, R);
            }
            m_keyframelist.unlock();
            updatePath();//更新ROSmsg的path
        }

        std::chrono::milliseconds dura(2000);
        std::this_thread::sleep_for(dura);
    }
    return;
}

void PoseGraph::updatePath()
{
    m_keyframelist.lock();
    list<KeyFrame*>::iterator it;
    for (int i = 1; i <= sequence_cnt; i++)
    {
        path[i].poses.clear();
    }
    base_path.poses.clear();
    posegraph_visualization->reset();

    if (SAVE_LOOP_PATH)
    {
        ofstream loop_path_file_tmp(VINS_RESULT_PATH, ios::out);
        loop_path_file_tmp.close();
    }

    for (it = keyframelist.begin(); it != keyframelist.end(); it++)
    {
        Vector3d P;
        Matrix3d R;
        (*it)->getPose(P, R);
        Quaterniond Q;
        Q = R;
//        printf("path p: %f, %f, %f\n",  P.x(),  P.z(),  P.y() );

        geometry_msgs::PoseStamped pose_stamped;
        pose_stamped.header.stamp = ros::Time((*it)->time_stamp);
        pose_stamped.header.frame_id = "world";
        pose_stamped.pose.position.x = P.x() + VISUALIZATION_SHIFT_X;
        pose_stamped.pose.position.y = P.y() + VISUALIZATION_SHIFT_Y;
        pose_stamped.pose.position.z = P.z();
        pose_stamped.pose.orientation.x = Q.x();
        pose_stamped.pose.orientation.y = Q.y();
        pose_stamped.pose.orientation.z = Q.z();
        pose_stamped.pose.orientation.w = Q.w();
        std::cout<<"(*it)->sequence"<<(*it)->sequence<<"  P="<<P.transpose()<<std::endl;
        if((*it)->sequence == 0)
        {
            base_path.poses.push_back(pose_stamped);
            base_path.header = pose_stamped.header;
        }
        else
        {
            path[(*it)->sequence].poses.push_back(pose_stamped);
            path[(*it)->sequence].header = pose_stamped.header;
        }

        if (SAVE_LOOP_PATH)
        {
            ofstream loop_path_file(VINS_RESULT_PATH, ios::app);
            loop_path_file.setf(ios::fixed, ios::floatfield);
            loop_path_file.precision(17);
            loop_path_file << (*it)->time_stamp<< " ";
            loop_path_file.precision(5);
            loop_path_file  << P.x() << " "
                  << P.y() << " "
//                  << P.z() << ","
                    << 0.0 << " "
                    << Q.x() << " "
                  << Q.y() << " "
                  << Q.z() << " "
                  << Q.w() << " "
                  << endl;
            loop_path_file.close();
        }
        //draw local connection
        if (SHOW_S_EDGE)
        {
            list<KeyFrame*>::reverse_iterator rit = keyframelist.rbegin();
            list<KeyFrame*>::reverse_iterator lrit;
            for (; rit != keyframelist.rend(); rit++)  
            {  
                if ((*rit)->index == (*it)->index)
                {
                    lrit = rit;
                    lrit++;
                    for (int i = 0; i < 4; i++)
                    {
                        if (lrit == keyframelist.rend())
                            break;
                        if((*lrit)->sequence == (*it)->sequence)
                        {
                            Vector3d conncected_P;
                            Matrix3d connected_R;
                            (*lrit)->getPose(conncected_P, connected_R);
                            posegraph_visualization->add_edge(P, conncected_P);
                        }
                        lrit++;
                    }
                    break;
                }
            } 
        }
        if (SHOW_L_EDGE)
        {
            if ((*it)->has_loop && (*it)->sequence == sequence_cnt)
            {
                
                KeyFrame* connected_KF = getKeyFrame((*it)->loop_index);
                Vector3d connected_P;
                Matrix3d connected_R;
                connected_KF->getPose(connected_P, connected_R);
                //(*it)->getVioPose(P, R);
                (*it)->getPose(P, R);
                if((*it)->sequence > 0)
                {
                    posegraph_visualization->add_loopedge(P, connected_P + Vector3d(VISUALIZATION_SHIFT_X, VISUALIZATION_SHIFT_Y, 0));
                }
            }
        }

    }
    publish_uisee();
    m_keyframelist.unlock();
}


void PoseGraph::savePoseGraph()
{
    m_keyframelist.lock();
    TicToc tmp_t;
    FILE *pFile;
    printf("pose graph path: %s\n",POSE_GRAPH_SAVE_PATH.c_str());
    printf("pose graph saving... \n");
    string file_path = POSE_GRAPH_SAVE_PATH + "pose_graph.txt";
    pFile = fopen (file_path.c_str(),"w");
    //fprintf(pFile, "index time_stamp Tx Ty Tz Qw Qx Qy Qz loop_index loop_info\n");
    list<KeyFrame*>::iterator it;
    for (it = keyframelist.begin(); it != keyframelist.end(); it++)
    {
        std::string image_path, descriptor_path, brief_path, keypoints_path;
        if (DEBUG_IMAGE)
        {
            image_path = POSE_GRAPH_SAVE_PATH + to_string((*it)->index) + "_image.png";
            imwrite(image_path.c_str(), (*it)->image);
        }
        Quaterniond VIO_tmp_Q{(*it)->vio_R_w_i};
        Quaterniond PG_tmp_Q{(*it)->R_w_i};
        Vector3d VIO_tmp_T = (*it)->vio_T_w_i;
        Vector3d PG_tmp_T = (*it)->T_w_i;

        fprintf (pFile, " %d %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %d %f %f %f %f %f %f %f %f %d\n",(*it)->index, (*it)->time_stamp, 
                                    VIO_tmp_T.x(), VIO_tmp_T.y(), VIO_tmp_T.z(), 
                                    PG_tmp_T.x(), PG_tmp_T.y(), PG_tmp_T.z(), 
                                    VIO_tmp_Q.w(), VIO_tmp_Q.x(), VIO_tmp_Q.y(), VIO_tmp_Q.z(), 
                                    PG_tmp_Q.w(), PG_tmp_Q.x(), PG_tmp_Q.y(), PG_tmp_Q.z(), 
                                    (*it)->loop_index, 
                                    (*it)->loop_info(0), (*it)->loop_info(1), (*it)->loop_info(2), (*it)->loop_info(3),
                                    (*it)->loop_info(4), (*it)->loop_info(5), (*it)->loop_info(6), (*it)->loop_info(7),
                                    (int)(*it)->keypoints.size());

        // write keypoints, brief_descriptors   vector<cv::KeyPoint> keypoints vector<BRIEF::bitset> brief_descriptors;
        assert((*it)->keypoints.size() == (*it)->brief_descriptors.size());
        brief_path = POSE_GRAPH_SAVE_PATH + to_string((*it)->index) + "_briefdes.dat";
        std::ofstream brief_file(brief_path, std::ios::binary);
        keypoints_path = POSE_GRAPH_SAVE_PATH + to_string((*it)->index) + "_keypoints.txt";
        FILE *keypoints_file;
        keypoints_file = fopen(keypoints_path.c_str(), "w");
        for (int i = 0; i < (int)(*it)->keypoints.size(); i++)
        {
            brief_file << (*it)->brief_descriptors[i] << endl;
            fprintf(keypoints_file, "%f %f %f %f\n", (*it)->keypoints[i].pt.x, (*it)->keypoints[i].pt.y, 
                                                     (*it)->keypoints_norm[i].pt.x, (*it)->keypoints_norm[i].pt.y);
        }
        brief_file.close();
        fclose(keypoints_file);
    }
    fclose(pFile);

    printf("save pose graph time: %f s\n", tmp_t.toc() / 1000);
    m_keyframelist.unlock();
}
void PoseGraph::loadPoseGraph()
{
    TicToc tmp_t;
    FILE * pFile;
    string file_path = POSE_GRAPH_SAVE_PATH + "pose_graph.txt";
    printf("lode pose graph from: %s \n", file_path.c_str());
    printf("pose graph loading...\n");
    pFile = fopen (file_path.c_str(),"r");
    if (pFile == NULL)
    {
        printf("lode previous pose graph error: wrong previous pose graph path or no previous pose graph \n the system will start with new pose graph \n");
        return;
    }
    int index;
    double time_stamp;
    double VIO_Tx, VIO_Ty, VIO_Tz;
    double PG_Tx, PG_Ty, PG_Tz;
    double VIO_Qw, VIO_Qx, VIO_Qy, VIO_Qz;
    double PG_Qw, PG_Qx, PG_Qy, PG_Qz;
    double loop_info_0, loop_info_1, loop_info_2, loop_info_3;
    double loop_info_4, loop_info_5, loop_info_6, loop_info_7;
    int loop_index;
    int keypoints_num;
    Eigen::Matrix<double, 8, 1 > loop_info;
    int cnt = 0;
    while (fscanf(pFile,"%d %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %d %lf %lf %lf %lf %lf %lf %lf %lf %d", &index, &time_stamp, 
                                    &VIO_Tx, &VIO_Ty, &VIO_Tz, 
                                    &PG_Tx, &PG_Ty, &PG_Tz, 
                                    &VIO_Qw, &VIO_Qx, &VIO_Qy, &VIO_Qz, 
                                    &PG_Qw, &PG_Qx, &PG_Qy, &PG_Qz, 
                                    &loop_index,
                                    &loop_info_0, &loop_info_1, &loop_info_2, &loop_info_3, 
                                    &loop_info_4, &loop_info_5, &loop_info_6, &loop_info_7,
                                    &keypoints_num) != EOF) 
    {
        /*
        printf("I read: %d %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %d %lf %lf %lf %lf %lf %lf %lf %lf %d\n", index, time_stamp, 
                                    VIO_Tx, VIO_Ty, VIO_Tz, 
                                    PG_Tx, PG_Ty, PG_Tz, 
                                    VIO_Qw, VIO_Qx, VIO_Qy, VIO_Qz, 
                                    PG_Qw, PG_Qx, PG_Qy, PG_Qz, 
                                    loop_index,
                                    loop_info_0, loop_info_1, loop_info_2, loop_info_3, 
                                    loop_info_4, loop_info_5, loop_info_6, loop_info_7,
                                    keypoints_num);
        */
        cv::Mat image;
        std::string image_path, descriptor_path;
        if (DEBUG_IMAGE)
        {
            image_path = POSE_GRAPH_SAVE_PATH + to_string(index) + "_image.png";
            image = cv::imread(image_path.c_str(), 0);
        }

        Vector3d VIO_T(VIO_Tx, VIO_Ty, VIO_Tz);
        Vector3d PG_T(PG_Tx, PG_Ty, PG_Tz);
        Quaterniond VIO_Q;
        VIO_Q.w() = VIO_Qw;
        VIO_Q.x() = VIO_Qx;
        VIO_Q.y() = VIO_Qy;
        VIO_Q.z() = VIO_Qz;
        Quaterniond PG_Q;
        PG_Q.w() = PG_Qw;
        PG_Q.x() = PG_Qx;
        PG_Q.y() = PG_Qy;
        PG_Q.z() = PG_Qz;
        Matrix3d VIO_R, PG_R;
        VIO_R = VIO_Q.toRotationMatrix();
        PG_R = PG_Q.toRotationMatrix();
        Eigen::Matrix<double, 8, 1 > loop_info;
        loop_info << loop_info_0, loop_info_1, loop_info_2, loop_info_3, loop_info_4, loop_info_5, loop_info_6, loop_info_7;

        if (loop_index != -1)
            if (earliest_loop_index > loop_index || earliest_loop_index == -1)
            {
                earliest_loop_index = loop_index;
            }

        // load keypoints, brief_descriptors   
        string brief_path = POSE_GRAPH_SAVE_PATH + to_string(index) + "_briefdes.dat";
        std::ifstream brief_file(brief_path, std::ios::binary);
        string keypoints_path = POSE_GRAPH_SAVE_PATH + to_string(index) + "_keypoints.txt";
        FILE *keypoints_file;
        keypoints_file = fopen(keypoints_path.c_str(), "r");
        vector<cv::KeyPoint> keypoints;
        vector<cv::KeyPoint> keypoints_norm;
        vector<BRIEF::bitset> brief_descriptors;
        for (int i = 0; i < keypoints_num; i++)
        {
            BRIEF::bitset tmp_des;
            brief_file >> tmp_des;
            brief_descriptors.push_back(tmp_des);
            cv::KeyPoint tmp_keypoint;
            cv::KeyPoint tmp_keypoint_norm;
            double p_x, p_y, p_x_norm, p_y_norm;
            if(!fscanf(keypoints_file,"%lf %lf %lf %lf", &p_x, &p_y, &p_x_norm, &p_y_norm))
                printf(" fail to load pose graph \n");
            tmp_keypoint.pt.x = p_x;
            tmp_keypoint.pt.y = p_y;
            tmp_keypoint_norm.pt.x = p_x_norm;
            tmp_keypoint_norm.pt.y = p_y_norm;
            keypoints.push_back(tmp_keypoint);
            keypoints_norm.push_back(tmp_keypoint_norm);
        }
        brief_file.close();
        fclose(keypoints_file);

        KeyFrame* keyframe = new KeyFrame(time_stamp, index, VIO_T, VIO_R, PG_T, PG_R, image, loop_index, loop_info, keypoints, keypoints_norm, brief_descriptors);
        loadKeyFrame(keyframe, 0);
        if (cnt % 20 == 0)
        {
            publish_uisee();
        }
        cnt++;
    }
    fclose (pFile);
    printf("load pose graph time: %f s\n", tmp_t.toc()/1000);
    base_sequence = 0;
}

void PoseGraph::publish_uisee()
{
    for (int i = 1; i <= sequence_cnt; i++)
    {
        //if (sequence_loop[i] == true || i == base_sequence)
        if (1)//(1 || i == base_sequence)
        {
            pub_pg_path.publish(path[i]);//"pose_graph_path"
            pub_path[i].publish(path[i]);//"path_" + to_string(i)
//            posegraph_visualization->publish_by(pub_pose_graph, path[sequence_cnt].header);//"pose_graph"
        }
    }
//    pub_base_path.publish(base_path);//"base_path"
    //posegraph_visualization->publish_by(pub_pose_graph, path[sequence_cnt].header);
}
