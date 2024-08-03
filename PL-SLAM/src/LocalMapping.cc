/**
* This file is part of ORB-SLAM2.
*
* Copyright (C) 2014-2016 Raúl Mur-Artal <raulmur at unizar dot es> (University of Zaragoza)
* For more information see <https://github.com/raulmur/ORB_SLAM2>
*
* ORB-SLAM2 is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM2 is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with ORB-SLAM2. If not, see <http://www.gnu.org/licenses/>.
*/

#include "LocalMapping.h"
#include "LoopClosing.h"
#include "ORBmatcher.h"
#include "Optimizer.h"

#include<mutex>

#define PI 3.1415926

namespace ORB_SLAM2
{

LocalMapping::LocalMapping(Map *pMap, const float bMonocular):
    mbMonocular(bMonocular), mbResetRequested(false), mbFinishRequested(false), mbFinished(true), mpMap(pMap),
    mbAbortBA(false), mbStopped(false), mbStopRequested(false), mbNotStop(false), mbAcceptKeyFrames(true)
{
}

void LocalMapping::SetLoopCloser(LoopClosing* pLoopCloser)
{
    mpLoopCloser = pLoopCloser;
}

void LocalMapping::SetTracker(Tracking *pTracker)
{
    mpTracker=pTracker;
}

void LocalMapping::Run()
{

    mbFinished = false;

    while(1)
    {
        // Tracking will see that Local Mapping is busy
        // 告诉Tracking线程，LocalMapping正处于繁忙状态，LocalMapping线程处理的关键帧都是Tracking线程发过来的，
        // 在LocalMapping线程还没有处理完关键帧之前，Tracking线程最好不要发太快
        SetAcceptKeyFrames(false);

        // Check if there are keyframes in the queue
        // 检查是否有在排队的关键帧
        if(CheckNewKeyFrames())
        {
            // 计算关键帧特征点的BoW映射，将关键帧插入地图
            ProcessNewKeyFrame();

            // 剔除ProcessNewKeyFrame函数中引入的不合格的MapPoints 和 MapLines
            thread threadCullPoint(&LocalMapping::MapPointCulling, this);
            thread threadCullLine(&LocalMapping::MapLineCulling, this);
            threadCullPoint.join();
            threadCullLine.join();

            // 相机运动过程中与相邻关键帧通过三角化恢复出一些MapPoints
            thread threadCreateP(&LocalMapping::CreateNewMapPoints, this);
            //thread threadCreateL(&LocalMapping::CreateNewMapLines, this);
            thread threadCreateL(&LocalMapping::CreateNewMapLinesConstraint, this);
            threadCreateP.join();
            threadCreateL.join();

            // 已经处理完关键帧队列中最后一个关键帧
            if(!CheckNewKeyFrames())
            {
                // Find more matches in neighbor keyframes and fuse point duplications
                // 检查并融合当前关键帧与相邻帧（两级相邻）重复的MapPoints,一级重复的MapLines
                SearchInNeighbors();
            }

            mbAbortBA = false;

            // 已经处理完队列中的最后一个关键帧，并且闭环检测没有请求停止LocalMapping
            if(!CheckNewKeyFrames() && !stopRequested())
            {
                // VI-D Local BA
                if(mpMap->KeyFramesInMap()>2)
                {
                    Optimizer::LocalBundleAdjustmentWithLine(mpCurrentKeyFrame, &mbAbortBA, mpMap);     //包含线特征的局部BA
                }

                // 检测并剔除当前帧相邻的关键帧中冗余的关键帧
                // 剔除的标准：该关键帧的90%的MapPoints可以被其他关键帧观测到
                // trick：
                // Tracking中先把关键帧交给LocalMapping线程，并且在Tracking中InsertKeyFrame函数的条件比较松，交给LocalMapping线程的关键帧会比较密
                // 在这里再删除冗余的关键帧
                KeyFrameCulling();
            }

            // 将当前帧插入到闭环检测队列中
            mpLoopCloser->InsertKeyFrame(mpCurrentKeyFrame);
        }
        else if(Stop())
        {
            // Safe area to stop
            while(isStopped() && !CheckFinish())
            {
                usleep(3000);
            }
            if(CheckFinish())
                break;
        }

        ResetIfRequested();

        // Tracking will see that Local Mapping is busy
        SetAcceptKeyFrames(true);

        if(CheckFinish())
            break;

        usleep(3000);
    }

    SetFinish();
}

void LocalMapping::InsertKeyFrame(KeyFrame *pKF)
{
    unique_lock<mutex> lock(mMutexNewKFs);
    mlNewKeyFrames.push_back(pKF);
    mbAbortBA=true;
}


bool LocalMapping::CheckNewKeyFrames()
{
    unique_lock<mutex> lock(mMutexNewKFs);
    return(!mlNewKeyFrames.empty());
}

/**
 * @brief 处理队列中的关键帧
 *
 * --计算BoW，加速三角化新的MapPoint
 * --关联当前关键帧至MapPoints，并更新MapPoints的平均观测方向和观测距离范围
 * --插入关键帧，更新Covisibility图和Essential图
 */
void LocalMapping::ProcessNewKeyFrame()
{
    // step1：从缓冲队列中取出一帧关键帧
    // Tracking线程向LocalMapping中插入的关键帧在该队列中
    {
        unique_lock<mutex> lock(mMutexNewKFs);
        // 从列表中获取一个等待被插入的关键帧
        mpCurrentKeyFrame = mlNewKeyFrames.front();
        mlNewKeyFrames.pop_front();
    }

    // Compute Bags of Words structures
    // step2: 计算该关键帧特征点的BoW映射关系
    mpCurrentKeyFrame->ComputeBoW();

    // Associate MapPoints to the new keyframe and update normal and descriptor
    // step3:跟踪局部地图过程中新匹配上的MapPoints和当前关键帧绑定
    // 在TrackLocalMap函数中将局部地图中的MapPoints与当前帧进行了匹配
    // 但没有对这些匹配上的MapPoints与当前帧进行关联
    const vector<MapPoint*> vpMapPointMatches = mpCurrentKeyFrame->GetMapPointMatches();

    for(size_t i=0; i<vpMapPointMatches.size(); i++)
    {
        MapPoint* pMP = vpMapPointMatches[i];
        if(pMP)
        {
            if(!pMP->isBad())
            {
                // 为当前帧在Tracking过程中跟踪到的MapPoints更新属性
                if(!pMP->IsInKeyFrame(mpCurrentKeyFrame))
                {
                    // 添加观测
                    pMP->AddObservation(mpCurrentKeyFrame, i);
                    // 获得该点的平均观测方向和观测距离范围
                    pMP->UpdateNormalAndDepth();
                    // 加入关键帧后，更新3D点的最佳描述子
                    pMP->ComputeDistinctiveDescriptors();
                }
                else // this can only happen for new stereo points inserted by the Tracking
                {
                    // 将双目或RGBD跟踪过程中新插入的MapPoints放入mlpRecentAddedMapPoints，等待检查
                    // CreateNewMapPoints函数中通过三角化也会生成MapPoints
                    // 这些MapPoints都会经过MapPointCulling函数的检验
                    mlpRecentAddedMapPoints.push_back(pMP);
                }
            }
        }
    }

    /// 跟踪局部地图过程中新匹配上的MapLines,和当前关键帧进行绑定
    const vector<MapLine*> vpMapLineMatches = mpCurrentKeyFrame->GetMapLineMatches();

    for(size_t i=0; i<vpMapLineMatches.size(); i++)
    {
        MapLine* pML = vpMapLineMatches[i];
        if(pML)
        {
            if(!pML->isBad())
            {
                if(!pML->IsInKeyFrame(mpCurrentKeyFrame))
                {
                    pML->AddObservation(mpCurrentKeyFrame, i);  //添加观测
                    pML->UpdateAverageDir();    //更新观测方向
                    pML->ComputeDistinctiveDescriptors();
                } else
                {
                    mlpRecentAddedMapLines.push_back(pML);
                }
            }
        }
    }

    // Update links in the Covisibility Graph
    // step6；更新关键帧之间的连接关系，Covisibility Graph和Essential Graph
    mpCurrentKeyFrame->UpdateConnections(); //此处与MapPoint有关，暂时不修改

    // Insert Keyframe in Map
    // step5：将该关键帧插入到地图中
    mpMap->AddKeyFrame(mpCurrentKeyFrame);
}

/**
 * @brief 剔除ProcessNewKeyFrame和CreateNewMapPoints函数中引入的质量不好的MapPoints
 */
void LocalMapping::MapPointCulling()
{
    // Check Recent Added MapPoints
    list<MapPoint*>::iterator lit = mlpRecentAddedMapPoints.begin();
    const unsigned long int nCurrentKFid = mpCurrentKeyFrame->mnId;

    int nThObs;
    if(mbMonocular)
        nThObs = 2;
    else
        nThObs = 3;
    const int cnThObs = nThObs;

    // 遍历等待检查的MapPoints
    while(lit!=mlpRecentAddedMapPoints.end())
    {
        MapPoint* pMP = *lit;
        if(pMP->isBad())
        {
            // step1：已经是坏点的MapPoint，直接从检查链表中剔除
            lit = mlpRecentAddedMapPoints.erase(lit);
        }
        else if(pMP->GetFoundRatio()<0.25f )
        {
            // step2:将不满足VI-B条件的MapPoint剔除
            // VI-B 条件1：
            // 跟踪到该MapPoint的Frame数量相比预计可观测到该MapPoint的Frame数的比例需要大于25%
            // IncreaseFound / IncreaseVisible < 25%，注意不一定是关键帧
            pMP->SetBadFlag();
            lit = mlpRecentAddedMapPoints.erase(lit);
        }
        else if(((int)nCurrentKFid-(int)pMP->mnFirstKFid)>=2 && pMP->Observations()<=cnThObs)
        {
            // step3: 将不满足VI-B条件的MApPoint剔除
            // VI-B 条件2：从该点建立开始，到现在已经过了不小于2帧，但是观测到该点的关键帧数却不超过cnThObs，那么该点检验不合格
            pMP->SetBadFlag();
            lit = mlpRecentAddedMapPoints.erase(lit);
        }
        else if(((int)nCurrentKFid-(int)pMP->mnFirstKFid)>=3)
            // step4：从建立该点开始，已经过了3帧，放弃对该MapPoint的检测
            lit = mlpRecentAddedMapPoints.erase(lit);
        else
            lit++;
    }
}

void LocalMapping::MapLineCulling()
{
    // Check Recent Added MapLines
    list<MapLine*>::iterator lit = mlpRecentAddedMapLines.begin();
    const unsigned long int nCurrentKFid = mpCurrentKeyFrame->mnId;

    int nThObs;
    if(mbMonocular)
        nThObs = 2;
    else
        nThObs = 3;
    const int cnThObs = nThObs;

    // 遍历等待检查的MapLines
    while(lit!=mlpRecentAddedMapLines.end())
    {
        MapLine* pML = *lit;
        if(pML->isBad())
        {
            // step1: 将已经是坏的MapLine从检查链中删除
            lit = mlpRecentAddedMapLines.erase(lit);
        }
        else if(pML->GetFoundRatio()<0.25f)
        {
            pML->SetBadFlag();
            lit = mlpRecentAddedMapLines.erase(lit);
        }
        else if(((int)nCurrentKFid-(int)pML->mnFirstKFid)>=2 && pML->Observations()<=cnThObs)
        {
            pML->SetBadFlag();
            lit = mlpRecentAddedMapLines.erase(lit);
        }
        else if(((int)nCurrentKFid-(int)pML->mnFirstKFid)>=3)
            lit = mlpRecentAddedMapLines.erase(lit);
        else
            lit++;
    }
}

/**
 * @brief 相机运动过程中和共视程度比较高的关键帧通过三角化恢复出一些MapPoints
 */
void LocalMapping::CreateNewMapPoints()
{
    // Retrieve neighbor keyframes in covisibility graph
    int nn = 10;
    if(mbMonocular)
        nn=20;

    // -step1: 在当前关键帧的共视关键帧中找到共视程度最高的nn帧相邻的vpNeighKFs
    const vector<KeyFrame*> vpNeighKFs = mpCurrentKeyFrame->GetBestCovisibilityKeyFrames(nn);

    ORBmatcher matcher(0.6,false);

    cv::Mat Rcw1 = mpCurrentKeyFrame->GetRotation();
    cv::Mat Rwc1 = Rcw1.t();
    cv::Mat tcw1 = mpCurrentKeyFrame->GetTranslation();
    cv::Mat Tcw1(3,4,CV_32F);
    Rcw1.copyTo(Tcw1.colRange(0,3));
    tcw1.copyTo(Tcw1.col(3));

    // 得到当前关键帧在世界坐标系中的坐标
    cv::Mat Ow1 = mpCurrentKeyFrame->GetCameraCenter();

    const float &fx1 = mpCurrentKeyFrame->fx;
    const float &fy1 = mpCurrentKeyFrame->fy;
    const float &cx1 = mpCurrentKeyFrame->cx;
    const float &cy1 = mpCurrentKeyFrame->cy;
    const float &invfx1 = mpCurrentKeyFrame->invfx;
    const float &invfy1 = mpCurrentKeyFrame->invfy;

    const float ratioFactor = 1.5f*mpCurrentKeyFrame->mfScaleFactor;

    int nnew=0;

    // Search matches with epipolar restriction and triangulate
    // -step2：遍历相邻关键帧vpNeighKFs,根据对极约束寻找匹配对，并且三角化
    for(size_t i=0; i<vpNeighKFs.size(); i++)
    {
        if(i>0 && CheckNewKeyFrames())
            return;

        KeyFrame* pKF2 = vpNeighKFs[i];

        // Check first that baseline is not too short
        cv::Mat Ow2 = pKF2->GetCameraCenter();  // 邻接关键帧在世界坐标系中的坐标
        cv::Mat vBaseline = Ow2-Ow1;    //基线向量，两个关键帧间的相机位移
        const float baseline = cv::norm(vBaseline); //基线长度，两个关键帧的位移长度

        // step3：判断两个关键帧之间的位移是不是足够长，如果太短就没必要计算
        if(!mbMonocular)
        {
            if(baseline<pKF2->mb)   //如果是双目相机，关键帧间距太小时，不生成3D点
            continue;
        }
        else
        {
            const float medianDepthKF2 = pKF2->ComputeSceneMedianDepth(2);  //邻接关键帧的场景深度中值
            const float ratioBaselineDepth = baseline/medianDepthKF2;   //两个关键帧的位移长度与场景深度的比值

            if(ratioBaselineDepth<0.01) //如果特别远，则不考虑当前邻接的关键帧，不生成3D点
                continue;
        }

        // Compute Fundamental Matrix
        // -step4：根据两个关键帧的位姿计算它们之间的基本矩阵
        cv::Mat F12 = ComputeF12(mpCurrentKeyFrame,pKF2);

        // Search matches that fullfil epipolar constraint
        // -step5:根据极线约束限制匹配时的搜索范围，进行特征点匹配
        vector<pair<size_t,size_t> > vMatchedIndices;
        matcher.SearchForTriangulation(mpCurrentKeyFrame,pKF2,F12,vMatchedIndices,false);

        cv::Mat Rcw2 = pKF2->GetRotation();
        cv::Mat Rwc2 = Rcw2.t();
        cv::Mat tcw2 = pKF2->GetTranslation();
        cv::Mat Tcw2(3,4,CV_32F);
        Rcw2.copyTo(Tcw2.colRange(0,3));
        tcw2.copyTo(Tcw2.col(3));

        const float &fx2 = pKF2->fx;
        const float &fy2 = pKF2->fy;
        const float &cx2 = pKF2->cx;
        const float &cy2 = pKF2->cy;
        const float &invfx2 = pKF2->invfx;
        const float &invfy2 = pKF2->invfy;

        // Triangulate each match
        // step6：对每对匹配通过三角化生成3D点，和Triangulate函数差不多
        const int nmatches = vMatchedIndices.size();
        for(int ikp=0; ikp<nmatches; ikp++)
        {
            // step6.1：取出匹配的特征点
            const int &idx1 = vMatchedIndices[ikp].first;   //匹配对在当前关键帧中的索引
            const int &idx2 = vMatchedIndices[ikp].second;  //匹配对在邻接关键帧中的索引

            const cv::KeyPoint &kp1 = mpCurrentKeyFrame->mvKeysUn[idx1];
            const float kp1_ur=mpCurrentKeyFrame->mvuRight[idx1];
            bool bStereo1 = kp1_ur>=0;

            const cv::KeyPoint &kp2 = pKF2->mvKeysUn[idx2];
            const float kp2_ur = pKF2->mvuRight[idx2];
            bool bStereo2 = kp2_ur>=0;

            // Check parallax between rays
            // 得到特征点在归一化平面上的坐标
            cv::Mat xn1 = (cv::Mat_<float>(3,1) << (kp1.pt.x-cx1)*invfx1, (kp1.pt.y-cy1)*invfy1, 1.0);
            cv::Mat xn2 = (cv::Mat_<float>(3,1) << (kp2.pt.x-cx2)*invfx2, (kp2.pt.y-cy2)*invfy2, 1.0);

            // 将特征点坐标，从相机坐标系转换到世界坐标系下
            cv::Mat ray1 = Rwc1*xn1;
            cv::Mat ray2 = Rwc2*xn2;
            // 计算在世界坐标系下，两个坐标向量间的余弦值
            const float cosParallaxRays = ray1.dot(ray2)/(cv::norm(ray1)*cv::norm(ray2));

            float cosParallaxStereo = cosParallaxRays+1;
            float cosParallaxStereo1 = cosParallaxStereo;
            float cosParallaxStereo2 = cosParallaxStereo;

            if(bStereo1)
                cosParallaxStereo1 = cos(2*atan2(mpCurrentKeyFrame->mb/2,mpCurrentKeyFrame->mvDepth[idx1]));
            else if(bStereo2)
                cosParallaxStereo2 = cos(2*atan2(pKF2->mb/2,pKF2->mvDepth[idx2]));

            cosParallaxStereo = min(cosParallaxStereo1,cosParallaxStereo2);

            // 三角化恢复3D点坐标
            cv::Mat x3D;
            if(cosParallaxRays<cosParallaxStereo && cosParallaxRays>0 && (bStereo1 || bStereo2 || cosParallaxRays<0.9998))
            {
                // Linear Triangulation Method
                cv::Mat A(4,4,CV_32F);
                A.row(0) = xn1.at<float>(0)*Tcw1.row(2)-Tcw1.row(0);
                A.row(1) = xn1.at<float>(1)*Tcw1.row(2)-Tcw1.row(1);
                A.row(2) = xn2.at<float>(0)*Tcw2.row(2)-Tcw2.row(0);
                A.row(3) = xn2.at<float>(1)*Tcw2.row(2)-Tcw2.row(1);

                cv::Mat w,u,vt;
                cv::SVD::compute(A,w,u,vt,cv::SVD::MODIFY_A| cv::SVD::FULL_UV);

                x3D = vt.row(3).t();

                if(x3D.at<float>(3)==0)
                    continue;

                // Euclidean coordinates
                x3D = x3D.rowRange(0,3)/x3D.at<float>(3);

            }
            else if(bStereo1 && cosParallaxStereo1<cosParallaxStereo2)
            {
                x3D = mpCurrentKeyFrame->UnprojectStereo(idx1);                
            }
            else if(bStereo2 && cosParallaxStereo2<cosParallaxStereo1)
            {
                x3D = pKF2->UnprojectStereo(idx2);
            }
            else
                continue; //No stereo and very low parallax

            cv::Mat x3Dt = x3D.t();

            //Check triangulation in front of cameras
            // 检测生成的3D点是否在相机前方，两个关键帧都需要检验，可以参考《视觉SLAM十四讲》三角化那一节
            float z1 = Rcw1.row(2).dot(x3Dt)+tcw1.at<float>(2);
            if(z1<=0)
                continue;

            float z2 = Rcw2.row(2).dot(x3Dt)+tcw2.at<float>(2);
            if(z2<=0)
                continue;

            //Check reprojection error in first keyframe
            // 在第一个关键帧中检测重投影误差
            const float &sigmaSquare1 = mpCurrentKeyFrame->mvLevelSigma2[kp1.octave];
            //将三角化的点再转换到相机坐标系下，归一化平面上
            const float x1 = Rcw1.row(0).dot(x3Dt)+tcw1.at<float>(0);
            const float y1 = Rcw1.row(1).dot(x3Dt)+tcw1.at<float>(1);
            const float invz1 = 1.0/z1;

            if(!bStereo1)
            {
                // 将上面计算得到的相机坐标系下的坐标投影到像素平面上
                float u1 = fx1*x1*invz1+cx1;
                float v1 = fy1*y1*invz1+cy1;
                // 计算重投影误差
                float errX1 = u1 - kp1.pt.x;
                float errY1 = v1 - kp1.pt.y;
                if((errX1*errX1+errY1*errY1)>5.991*sigmaSquare1)    //5.991是基于卡方检验计算出的阈值，假设测量有一个像素的偏差
                    continue;
            }
            else
            {
                float u1 = fx1*x1*invz1+cx1;
                float u1_r = u1 - mpCurrentKeyFrame->mbf*invz1;
                float v1 = fy1*y1*invz1+cy1;
                float errX1 = u1 - kp1.pt.x;
                float errY1 = v1 - kp1.pt.y;
                float errX1_r = u1_r - kp1_ur;
                if((errX1*errX1+errY1*errY1+errX1_r*errX1_r)>7.8*sigmaSquare1)
                    continue;
            }

            //Check reprojection error in second keyframe
            // 计算3D点在另一个关键帧下的重投影误差，和上面步骤一样
            const float sigmaSquare2 = pKF2->mvLevelSigma2[kp2.octave];
            const float x2 = Rcw2.row(0).dot(x3Dt)+tcw2.at<float>(0);
            const float y2 = Rcw2.row(1).dot(x3Dt)+tcw2.at<float>(1);
            const float invz2 = 1.0/z2;
            if(!bStereo2)
            {
                float u2 = fx2*x2*invz2+cx2;
                float v2 = fy2*y2*invz2+cy2;
                float errX2 = u2 - kp2.pt.x;
                float errY2 = v2 - kp2.pt.y;
                if((errX2*errX2+errY2*errY2)>5.991*sigmaSquare2)
                    continue;
            }
            else
            {
                float u2 = fx2*x2*invz2+cx2;
                float u2_r = u2 - mpCurrentKeyFrame->mbf*invz2;
                float v2 = fy2*y2*invz2+cy2;
                float errX2 = u2 - kp2.pt.x;
                float errY2 = v2 - kp2.pt.y;
                float errX2_r = u2_r - kp2_ur;
                if((errX2*errX2+errY2*errY2+errX2_r*errX2_r)>7.8*sigmaSquare2)  ///7.8和上面的5.991联系？
                    continue;
            }

            //Check scale consistency
            // 检测尺度连续性
            cv::Mat normal1 = x3D-Ow1;
            float dist1 = cv::norm(normal1);

            cv::Mat normal2 = x3D-Ow2;
            float dist2 = cv::norm(normal2);

            if(dist1==0 || dist2==0)
                continue;

            const float ratioDist = dist2/dist1;
            const float ratioOctave = mpCurrentKeyFrame->mvScaleFactors[kp1.octave]/pKF2->mvScaleFactors[kp2.octave];

            /*if(fabs(ratioDist-ratioOctave)>ratioFactor)
                continue;*/
            if(ratioDist*ratioFactor<ratioOctave || ratioDist>ratioOctave*ratioFactor)
                continue;

            // Triangulation is succesfull
            MapPoint* pMP = new MapPoint(x3D,mpCurrentKeyFrame,mpMap);  //构造MapPoint

            pMP->AddObservation(mpCurrentKeyFrame,idx1);            
            pMP->AddObservation(pKF2,idx2);

            mpCurrentKeyFrame->AddMapPoint(pMP,idx1);
            pKF2->AddMapPoint(pMP,idx2);

            pMP->ComputeDistinctiveDescriptors();

            pMP->UpdateNormalAndDepth();

            mpMap->AddMapPoint(pMP);
            mlpRecentAddedMapPoints.push_back(pMP);

            nnew++;
        }
    }
}

void LocalMapping::CreateNewMapLines()
{
    // Retrieve neighbor keyframes in covisibility graph
    int nn=5;
    if(mbMonocular)
        nn=10;
    //step1：在当前关键帧的共视关键帧中找到共视成都最高的nn帧相邻帧vpNeighKFs
    const vector<KeyFrame*> vpNeighKFs = mpCurrentKeyFrame->GetBestCovisibilityKeyFrames(nn);
    
    if(vpNeighKFs.size() < 3)
        return;

    LSDmatcher lmatcher(0.6);    //建立线特征匹配

    // 获取当前帧的转换矩阵
    cv::Mat Rcw1 = mpCurrentKeyFrame->GetRotation();
    cv::Mat Rwc1 = Rcw1.t();
    cv::Mat tcw1 = mpCurrentKeyFrame->GetTranslation();
    cv::Mat Tcw1(3, 4, CV_32F);
    Rcw1.copyTo(Tcw1.colRange(0,3));
    tcw1.copyTo(Tcw1.col(3));

    //得到当前关键帧在世界坐标系中的坐标
    cv::Mat Ow1 = mpCurrentKeyFrame->GetCameraCenter();

    //获取当前帧的相机内参
    const Mat &K1 = mpCurrentKeyFrame->mK;
    const float &fx1 = mpCurrentKeyFrame->fx;
    const float &fy1 = mpCurrentKeyFrame->fy;
    const float &cx1 = mpCurrentKeyFrame->cx;
    const float &cy1 = mpCurrentKeyFrame->cy;
    const float &invfx1 = mpCurrentKeyFrame->invfx;
    const float &invfy1 = mpCurrentKeyFrame->invfy;

    const float ratioFactor = 1.5f*mpCurrentKeyFrame->mfScaleFactorLine;

    int nnew = 0;

    // Search matches with epipolar restriction and triangulate
    // step2: 遍历相邻关键帧vpNeighKFs
    for(size_t i=0; i<vpNeighKFs.size(); i++)
    {
        if(i>0 && CheckNewKeyFrames())
            return;

        KeyFrame* pKF2 = vpNeighKFs[i];

        // Check first that baseline is not too short
        // 邻接的关键帧在世界坐标系中的坐标
        cv::Mat Ow2 = pKF2->GetCameraCenter();
        // 基线向量，两个关键帧间的相机位移
        cv::Mat vBaseline = Ow2 - Ow1;
        // 基线长度
        const float baseline = cv::norm(vBaseline);

        // step3：判断相机运动的基线是不是足够长
        if(!mbMonocular)
        {
            // 如果是立体相机，关键帧间距太小时不生成3D点
            if(baseline<pKF2->mb)
                continue;
        }
        else
        {
            // 邻接关键帧的场景深度中值
            const float medianDepthKF2 = pKF2->ComputeSceneMedianDepth(2);
            // baseline 与景深的比例
            const float ratioBaselineDepth = baseline/medianDepthKF2;
            // 如果特别远（比例特别小），那么不考虑当前邻接的关键帧，不生成3D点
            if(ratioBaselineDepth<0.01)
                continue;
        }

        // Compute Fundamental Matrix
        // step4：根据两个关键帧的位姿计算它们之间的基本矩阵
        cv::Mat F12 = ComputeF12(mpCurrentKeyFrame, pKF2);

        // Search matches that fulfill epipolar constraint
        // step5：通过极线约束限制匹配时的搜索单位，进行特征点匹配
        cv::Mat pic = DrawLines(mpCurrentKeyFrame, pKF2);
        lmatcher.pic = pic;
        vector<pair<size_t, size_t>> vMatchedIndices;
        lmatcher.SearchForTriangulation(mpCurrentKeyFrame, pKF2, vMatchedIndices);

        ////////////////////////////////////////////////////////////////////////////////////////////
        // 排除错误值
        vector<double> offsets;
        for(int ipair = 0; ipair<vMatchedIndices.size();ipair++)
        {
            KeyLine kl1 = mpCurrentKeyFrame->mvKeyLines[vMatchedIndices[ipair].first];
            KeyLine kl2 = pKF2->mvKeyLines[vMatchedIndices[ipair].second];
            double midX1 = (kl1.startPointX + kl1.endPointX)/2; double midY1 = (kl1.startPointY + kl1.endPointY)/2;
            double midX2 = (kl2.startPointX + kl2.endPointX)/2; double midY2 = (kl2.startPointY + kl2.endPointY)/2;
            double offset = sqrt((midX1 - midX2) * (midX1 - midX2) + (midY1 - midY2)  * (midY1 - midY2)) ;
            offsets.push_back(offset);
        }

        double sum = std::accumulate(std::begin(offsets), std::end(offsets), 0.0);
        double mean =  sum / offsets.size(); //均值
    
        double accum  = 0.0;
        std::for_each (std::begin(offsets), std::end(offsets), [&](const double d) {
            accum  += (d-mean)*(d-mean);
        });
    
        double stdev = sqrt(accum/(offsets.size()-1));
        ////////////////////////////////////////////////////////////////////////////////////////////

        cv::Mat Rcw2 = pKF2->GetRotation();
        cv::Mat Rwc2 = Rcw2.t();
        cv::Mat tcw2 = pKF2->GetTranslation();
        cv::Mat Tcw2(3, 4, CV_32F);
        Rcw2.copyTo(Tcw2.colRange(0,3));
        tcw2.copyTo(Tcw2.col(3));

        const Mat &K2 = pKF2->mK;
        const float &fx2 = pKF2->fx;
        const float &fy2 = pKF2->fy;
        const float &cx2 = pKF2->cx;
        const float &cy2 = pKF2->cy;
        const float &invfx2 = pKF2->invfx;
        const float &invfy2 = pKF2->invfy;

        // Triangulate each matched line Segment
        const int nmatches = vMatchedIndices.size();
        for(int ikl=0; ikl<nmatches; ikl++)
        {
            if(offsets[ikl] - mean  > stdev * 3){
                //cerr<<vMatchedIndices[ikl].second<<endl;
                continue;
            }
                
            // step6.1：取出匹配的特征线
            const int &idx1 = vMatchedIndices[ikl].first;
            const int &idx2 = vMatchedIndices[ikl].second;

            const KeyLine &keyline1 = mpCurrentKeyFrame->mvKeyLines[idx1];
            const KeyLine &keyline2 = pKF2->mvKeyLines[idx2];
            const Vector3d keyline1_function = mpCurrentKeyFrame->mvKeyLineFunctions[idx1];
            const Vector3d keyline2_function = pKF2->mvKeyLineFunctions[idx2];
            const Mat klF1 = (Mat_<float>(3,1) << keyline1_function(0),
                                                keyline1_function(1),
                                                keyline1_function(2));
            const Mat klF2 = (Mat_<float>(3,1) << keyline2_function(0),
                                                keyline2_function(1),
                                                keyline2_function(2));

            // 保障不在同一对极平面上
            cv::Mat lineVector2 = (Mat_<float>(2,1) << -keyline2_function(1), keyline2_function(0));
            cv::Mat _ray1Start = (Mat_<float>(3,1) <<keyline1.startPointX, keyline1.startPointY, 1); cv::Mat _ray1End = (Mat_<float>(3,1) << keyline1.endPointX, keyline1.endPointY, 1);
            cv::Mat R21 = Rcw2 * Rwc1;
            cv::Mat t21 = Rcw2 * ( Rwc2 * tcw2 - Rwc1 * tcw1 );
            cv::Mat t21x = SkewSymmetricMatrix(t21);
            cv::Mat F21 = (K2.t()).inv() * t21x * R21 * K1.inv();
            cv::Mat Th1 = F21*_ray1Start;
            cv::Mat Th1_ = (Mat_<float>(2,1) << -Th1.at<float>(1, 0), Th1.at<float>(0, 0));
            float Result1 =  Th1_.dot(lineVector2) / (norm(Th1_) * norm(lineVector2));
            cv::Mat Th2 = F21*_ray1End;
            cv::Mat Th2_ = (Mat_<float>(2,1) << -Th2.at<float>(1, 0), Th2.at<float>(0, 0));
            float Result2 =  Th2_.dot(lineVector2) / (norm(Th2_) * norm(lineVector2));

            if(abs(Result1)>0.98 || abs(Result2)>0.98)
                continue;

            // step6.2：线段在第一帧图像中的坐标
            cv::Mat StartC1, EndC1;
            StartC1 = (cv::Mat_<float>(3,1) << (keyline1.startPointX-cx1)*invfx1, (keyline1.startPointY-cy1)*invfy1, 1.0);
            EndC1 = (cv::Mat_<float>(3,1) << (keyline1.endPointX-cx1)*invfx1, (keyline1.endPointY-cy1)*invfy1, 1.0);

            // step6.3：两帧图像的投影矩阵
            Mat M1 = K1 * Tcw1;
            Mat M2 = K2 * Tcw2;

            // step6.4：三角化恢复线段的3D端点
            cv::Mat s3D, e3D;
            // 起始点
            cv::Mat A(4,4,CV_32F);
            A.row(0) = klF1.t()*M1;
            A.row(1) = klF2.t()*M2;
            A.row(2) = StartC1.at<float>(0)*Tcw1.row(2)-Tcw1.row(0);
            A.row(3) = StartC1.at<float>(1)*Tcw1.row(2)-Tcw1.row(1);

            cv::Mat w1, u1, vt1;
            cv::SVD::compute(A, w1, u1, vt1, cv::SVD::MODIFY_A| cv::SVD::FULL_UV);

            s3D = vt1.row(3).t();

            if(s3D.at<float>(3)==0)
                continue;

            // Euclidean coordinates
            s3D = s3D.rowRange(0,3)/s3D.at<float>(3);

            // 终止点
            cv::Mat B(4,4,CV_32F);
            B.row(0) = klF1.t()*M1;
            B.row(1) = klF2.t()*M2;
            B.row(2) = EndC1.at<float>(0)*Tcw1.row(2)-Tcw1.row(0);
            B.row(3) = EndC1.at<float>(1)*Tcw1.row(2)-Tcw1.row(1);

            cv::Mat w2, u2, vt2;
            cv::SVD::compute(B, w2, u2, vt2, cv::SVD::MODIFY_A| cv::SVD::FULL_UV);

            e3D = vt2.row(3).t();

            if(e3D.at<float>(3)==0)
                continue;

            // Euclidean coordinates
            e3D = e3D.rowRange(0,3)/e3D.at<float>(3);

            cv::Mat s3Dt = s3D.t();
            cv::Mat e3Dt = e3D.t();

            // 判断起始点是否离两个相机中心太近
            // 邻接关键帧的场景深度中值
            const float medianDepthKF2 = pKF2->ComputeSceneMedianDepth(2);
            cv::Mat v1 = s3D - Ow1;
            float distance1 = cv::norm(v1);
            const float ratio1 = distance1/medianDepthKF2;
            if(ratio1 < 0.3)
                continue;

            cv::Mat v2 = s3D - Ow2;
            float distance2 = cv::norm(v2);
            const float ratio2 = distance2/medianDepthKF2;
            if(ratio2 < 0.3)
                continue;

            // 判断线段是否太长
            cv::Mat v3 = e3D - s3D;
            float distance3 = cv::norm(v3);
            const float ratio3 = distance3/medianDepthKF2;
            if(ratio3 > 1)
                continue;

            // step6.5：检测生成的3D点是否在相机前方
            float SZC1 = Rcw1.row(2).dot(s3Dt)+tcw1.at<float>(2);   //起始点在C1下的Z坐标值
            if(SZC1<=0)
                continue;

            float SZC2 = Rcw2.row(2).dot(s3Dt)+tcw2.at<float>(2);   //起始点在C2下的Z坐标值
            if(SZC2<=0)
                continue;

            float EZC1 = Rcw1.row(2).dot(e3Dt)+tcw1.at<float>(2);   //终止点在C1下的Z坐标值
            if(EZC1<=0)
                continue;

            float EZC2 = Rcw2.row(2).dot(e3Dt)+tcw2.at<float>(2);   //终止点在C2下的Z坐标值
            if(EZC2<=0)
                continue;

            // step6.9: 三角化成功，构造MapLine
            Vector6d line3D;
            line3D << s3D.at<float>(0), s3D.at<float>(1), s3D.at<float>(2), e3D.at<float>(0), e3D.at<float>(1), e3D.at<float>(2);
            MapLine* pML = new MapLine(line3D, mpCurrentKeyFrame, mpMap);

            // step6.10：为该MapLine添加属性
            pML->AddObservation(mpCurrentKeyFrame, idx1);
            pML->AddObservation(pKF2, idx2);

            mpCurrentKeyFrame->AddMapLine(pML, idx1);
            pKF2->AddMapLine(pML, idx2);

            pML->ComputeDistinctiveDescriptors();
            pML->UpdateAverageDir();
            mpMap->AddMapLine(pML);

            // step6.11：将新产生的线特征放入检测队列，这些MapLines都会经过MapLineCulling函数的检验
            mlpRecentAddedMapLines.push_back(pML);

            nnew++;
        }

    }
}

void LocalMapping::CreateNewMapLinesConstraint()
{
    // Retrieve neighbor keyframes in covisibility graph
    int nn=5;
    if(mbMonocular)
        nn=10;
    //step1：在当前关键帧的共视关键帧中找到共视成都最高的nn帧相邻帧vpNeighKFs
    const vector<KeyFrame*> vpNeighKFs = mpCurrentKeyFrame->GetBestCovisibilityKeyFrames(nn);
    
    if(vpNeighKFs.size() < 2)
        return;

    LSDmatcher lmatcher(0.8);    //建立线特征匹配

    // 获取当前帧的转换矩阵
    cv::Mat Rcw1 = mpCurrentKeyFrame->GetRotation();
    cv::Mat Rwc1 = Rcw1.t();
    cv::Mat tcw1 = mpCurrentKeyFrame->GetTranslation();
    cv::Mat Tcw1(3, 4, CV_32F);
    Rcw1.copyTo(Tcw1.colRange(0,3));
    tcw1.copyTo(Tcw1.col(3));

    //得到当前关键帧在世界坐标系中的坐标
    cv::Mat Ow1 = mpCurrentKeyFrame->GetCameraCenter();

    //获取当前帧的相机内参
    const Mat &K1 = mpCurrentKeyFrame->mK;
    const float &fx1 = mpCurrentKeyFrame->fx;
    const float &fy1 = mpCurrentKeyFrame->fy;
    const float &cx1 = mpCurrentKeyFrame->cx;
    const float &cy1 = mpCurrentKeyFrame->cy;
    const float &invfx1 = mpCurrentKeyFrame->invfx;
    const float &invfy1 = mpCurrentKeyFrame->invfy;

    const float ratioFactor = 1.5f*mpCurrentKeyFrame->mfScaleFactorLine;

    int nnew = 0;

    // Search matches with epipolar restriction and triangulate
    // step2: 遍历相邻关键帧vpNeighKFs
    
    vector<vector<int>> TotalvMatchedIndices;
    vector<int> nTotalMatched;
    nTotalMatched.reserve(vpNeighKFs.size());
    TotalvMatchedIndices.reserve(vpNeighKFs.size());

    for(size_t i=0; i<vpNeighKFs.size(); i++)
    {
        if(i>1 && CheckNewKeyFrames())
            return;

        KeyFrame* pKF2 = vpNeighKFs[i];

        // Check first that baseline is not too short
        // 邻接的关键帧在世界坐标系中的坐标
        cv::Mat Ow2 = pKF2->GetCameraCenter();
        // 基线向量，两个关键帧间的相机位移
        cv::Mat vBaseline = Ow2 - Ow1;
        // 基线长度
        const float baseline = cv::norm(vBaseline);

        // step3：判断相机运动的基线是不是足够长
        if(!mbMonocular)
        {
            // 如果是立体相机，关键帧间距太小时不生成3D点
            if(baseline<pKF2->mb)
                continue;
        }
        else
        {
            // 邻接关键帧的场景深度中值
            const float medianDepthKF2 = pKF2->ComputeSceneMedianDepth(2);
            // baseline 与景深的比例
            const float ratioBaselineDepth = baseline/medianDepthKF2;
            // 如果特别远（比例特别小），那么不考虑当前邻接的关键帧，不生成3D点
            if(ratioBaselineDepth<0.01)
                continue;
        }

        // Search matches that fulfill epipolar constraint
        // step5：通过极线约束限制匹配时的搜索单位，进行特征点匹配
        cv::Mat pic = DrawLines(mpCurrentKeyFrame, pKF2);
        lmatcher.pic = pic;

        vector<int> vMatchedIndices;
        //int nlmatch = lmatcher.SearchForTriangulationNew(mpCurrentKeyFrame, pKF2, vMatchedIndices, true);
        int nlmatch = lmatcher.SearchForTriangulation(mpCurrentKeyFrame, pKF2, vMatchedIndices, true);
        TotalvMatchedIndices.push_back(vMatchedIndices);
        nTotalMatched.push_back(nlmatch);
    }

    if(TotalvMatchedIndices.size() < 2)
        return;

    for(size_t i = 0; i<TotalvMatchedIndices.size() -1; i++)
    {
        vector<int> vMatchedIndices1 = TotalvMatchedIndices[i];

        if(nTotalMatched[i] == 0)
            continue;

        KeyFrame* pKF2 = vpNeighKFs[i];

        cv::Mat Rcw2 = pKF2->GetRotation();
        cv::Mat Rwc2 = Rcw2.t();
        cv::Mat tcw2 = pKF2->GetTranslation();
        cv::Mat Tcw2(3, 4, CV_32F);
        Rcw2.copyTo(Tcw2.colRange(0,3));
        tcw2.copyTo(Tcw2.col(3));

        cv::Mat Ow2 = pKF2->GetCameraCenter();

        const Mat &K2 = pKF2->mK;
        const float &fx2 = pKF2->fx;
        const float &fy2 = pKF2->fy;
        const float &cx2 = pKF2->cx;
        const float &cy2 = pKF2->cy;
        const float &invfx2 = pKF2->invfx;
        const float &invfy2 = pKF2->invfy;

        for(size_t j = i+1; j<TotalvMatchedIndices.size(); j++)
        {
            vector<int> vMatchedIndices2 = TotalvMatchedIndices[j];

            if(nTotalMatched[j] == 0)
                continue;

            KeyFrame* pKF3 = vpNeighKFs[j];

            cv::Mat Rcw3 = pKF3->GetRotation();
            cv::Mat Rwc3 = Rcw3.t();
            cv::Mat tcw3 = pKF3->GetTranslation();
            cv::Mat Tcw3(3, 4, CV_32F);
            Rcw3.copyTo(Tcw3.colRange(0,3));
            tcw3.copyTo(Tcw3.col(3));

            cv::Mat Ow3 = pKF3->GetCameraCenter();

            const Mat &K3 = pKF3->mK;
            const float &fx3 = pKF3->fx;
            const float &fy3 = pKF3->fy;
            const float &cx3 = pKF3->cx;
            const float &cy3 = pKF3->cy;
            const float &invfx3 = pKF3->invfx;
            const float &invfy3 = pKF3->invfy;

            //FOR DEBUG
            
            cv::Mat mImRGBTemp1, mImRGBTemp2, mImRGBTemp3;
            int nRows = mpCurrentKeyFrame->ImageGray.rows; 
            int nCols = mpCurrentKeyFrame->ImageGray.cols;
            cv::cvtColor(mpCurrentKeyFrame->ImageGray, mImRGBTemp1, cv::COLOR_GRAY2BGR);
            cv::cvtColor(pKF2->ImageGray, mImRGBTemp2, cv::COLOR_GRAY2BGR);
            cv::cvtColor(pKF3->ImageGray, mImRGBTemp3, cv::COLOR_GRAY2BGR);
            cv::Mat cubemapMatch_rgb(nRows, nCols * 3, mImRGBTemp1.type());

            (mImRGBTemp1.rowRange(0, mImRGBTemp1.rows).colRange(0, mImRGBTemp1.cols)).copyTo(cubemapMatch_rgb.colRange(0, nCols));
            (mImRGBTemp2.rowRange(0, mImRGBTemp2.rows).colRange(0, mImRGBTemp2.cols)).copyTo(cubemapMatch_rgb.colRange(nCols, nCols * 2));
            (mImRGBTemp3.rowRange(0, mImRGBTemp3.rows).colRange(0, mImRGBTemp3.cols)).copyTo(cubemapMatch_rgb.colRange(nCols*2, nCols * 3));

            for(int ikl=0; ikl<mpCurrentKeyFrame->NL; ikl++)
            {   
                // step6.1：取出匹配的特征线
                const int &idx1 = vMatchedIndices1[ikl];
                const int &idx2 = vMatchedIndices2[ikl];

                if(idx1 == -1 || idx2 == -1 || idx1>=pKF2->NL || idx2 >= pKF3->NL)
                    continue;

                if(mpCurrentKeyFrame->GetMapLine(ikl) || pKF2->GetMapLine(idx1) || pKF3->GetMapLine(idx2))
                    continue;

                const KeyLine &keyline1 = mpCurrentKeyFrame->mvKeyLines[ikl];
                const KeyLine &keyline2 = pKF2->mvKeyLines[idx1];
                const KeyLine &keyline3 = pKF3->mvKeyLines[idx2];
                const Vector3d keyline1_function = mpCurrentKeyFrame->mvKeyLineFunctions[ikl];
                const Vector3d keyline2_function = pKF2->mvKeyLineFunctions[idx1];
                const Vector3d keyline3_function = pKF3->mvKeyLineFunctions[idx2];
                const Mat klF1 = (Mat_<float>(3,1) << keyline1_function(0),
                                                    keyline1_function(1),
                                                    keyline1_function(2));
                const Mat klF2 = (Mat_<float>(3,1) << keyline2_function(0),
                                                    keyline2_function(1),
                                                    keyline2_function(2));
                const Mat klF3 = (Mat_<float>(3,1) << keyline3_function(0),
                                                    keyline3_function(1),
                                                    keyline3_function(2));

                // 保障不在同一对极平面上
                ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
                cv::Mat lineVector2 = (Mat_<float>(2,1) << -keyline2_function(1), keyline2_function(0));
                cv::Mat _ray1Start = (Mat_<float>(3,1) <<keyline1.startPointX, keyline1.startPointY, 1); cv::Mat _ray1End = (Mat_<float>(3,1) << keyline1.endPointX, keyline1.endPointY, 1);
                cv::Mat R21 = Rcw2 * Rwc1;
                cv::Mat t21 = Rcw2 * ( Rwc2 * tcw2 - Rwc1 * tcw1 );
                cv::Mat t21x = SkewSymmetricMatrix(t21);
                cv::Mat F21 = (K2.t()).inv() * t21x * R21 * K1.inv();
                cv::Mat Th1 = F21*_ray1Start;
                cv::Mat Th1_ = (Mat_<float>(2,1) << -Th1.at<float>(1, 0), Th1.at<float>(0, 0));
                float Result1 =  Th1_.dot(lineVector2) / (norm(Th1_) * norm(lineVector2));
                cv::Mat Th2 = F21*_ray1End;
                cv::Mat Th2_ = (Mat_<float>(2,1) << -Th2.at<float>(1, 0), Th2.at<float>(0, 0));
                float Result2 =  Th2_.dot(lineVector2) / (norm(Th2_) * norm(lineVector2));

                if(abs(Result1)>0.996 || abs(Result2)>0.996)
                    continue;
                ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

                ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
                cv::Mat R12 = Rcw1*Rwc2;
                cv::Mat R13 = Rcw1*Rwc3;

                cv::Mat lS = (cv::Mat_<float>(3, 1) << keyline1.startPointX, keyline1.startPointY, 1.0);
                cv::Mat lE = (cv::Mat_<float>(3, 1) << keyline1.endPointX, keyline1.endPointY, 1.0);
                cv::Mat lS_ = K1.inv() * lS; cv::Mat lE_ = K1.inv() * lE;
                cv::Mat L1 = lS_.cross(lE_);

                lS = (cv::Mat_<float>(3, 1) << keyline2.startPointX, keyline2.startPointY, 1.0);
                lE = (cv::Mat_<float>(3, 1) << keyline2.endPointX, keyline2.endPointY, 1.0);
                lS_ = K2.inv() * lS; lE_ = K2.inv() * lE;
                cv::Mat L2 = lS_.cross(lE_);

                lS = (cv::Mat_<float>(3, 1) << keyline3.startPointX, keyline3.startPointY, 1.0);
                lE = (cv::Mat_<float>(3, 1) << keyline3.endPointX, keyline3.endPointY, 1.0);
                lS_ = K3.inv() * lS; lE_ = K3.inv() * lE;
                cv::Mat L3 = lS_.cross(lE_);

                cv::Mat tWorldVector = (R12*L2).cross(R13*L3);
                float norm_ = cv::norm(tWorldVector);
                if(norm_ == 0)
                    continue;
                tWorldVector /= norm_;
                norm_ = cv::norm(L1); L1 /= norm_;
                if(norm_ == 0)
                    continue;
                float CosSita = abs(L1.dot(tWorldVector));

                if(CosSita>0.0087)
                {
                    continue;
                }
                ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

                // step6.2：线段在第一帧图像中的坐标
                cv::Mat StartC1, EndC1, StartC2, EndC2, StartC3, EndC3;
                StartC1 = (cv::Mat_<float>(3,1) << (keyline1.startPointX-cx1)*invfx1, (keyline1.startPointY-cy1)*invfy1, 1.0);
                EndC1 = (cv::Mat_<float>(3,1) << (keyline1.endPointX-cx1)*invfx1, (keyline1.endPointY-cy1)*invfy1, 1.0);

                // step6.3：两帧图像的投影矩阵
                cv::Mat M1 = K1 * Tcw1;
                cv::Mat M2 = K2 * Tcw2;
                cv::Mat M3 = K3 * Tcw3;

                // step6.4：三角化恢复线段的3D端点
                cv::Mat s3D, e3D;
                // 起始点
                cv::Mat A(4,4,CV_32F);
                A.row(0) = klF3.t()*M3;
                A.row(1) = klF2.t()*M2;
                A.row(2) = keyline1.startPointX*M1.row(2)-M1.row(0);
                A.row(3) = keyline1.startPointY*M1.row(2)-M1.row(1);
                /*A.row(0) = L3.t()*Tcw3;
                A.row(1) = L2.t()*Tcw2;
                A.row(2) = StartC1.at<float>(0)*Tcw1.row(2)-Tcw1.row(0);
                A.row(3) = StartC1.at<float>(1)*Tcw1.row(2)-Tcw1.row(1);*/

                cv::Mat w1, u1, vt1;
                cv::SVD::compute(A, w1, u1, vt1, cv::SVD::MODIFY_A| cv::SVD::FULL_UV);

                s3D = vt1.row(3).t();

                if(s3D.at<float>(3)==0)
                    continue;

                // Euclidean coordinates
                s3D = s3D.rowRange(0,3)/s3D.at<float>(3);

                // 终止点
                cv::Mat B(4,4,CV_32F);
                B.row(0) = klF3.t()*M3;
                B.row(1) = klF2.t()*M2;
                B.row(2) = keyline1.endPointX*M1.row(2)-M1.row(0);
                B.row(3) = keyline1.endPointY*M1.row(2)-M1.row(1);
                /*B.row(0) = L3.t()*Tcw3;
                B.row(1) = L2.t()*Tcw2;
                B.row(2) = EndC1.at<float>(0)*Tcw1.row(2)-Tcw1.row(0);
                B.row(3) = EndC1.at<float>(1)*Tcw1.row(2)-Tcw1.row(1);*/

                cv::Mat w2, u2, vt2;
                cv::SVD::compute(B, w2, u2, vt2, cv::SVD::MODIFY_A| cv::SVD::FULL_UV);

                e3D = vt2.row(3).t();

                if(e3D.at<float>(3)==0)
                    continue;

                // Euclidean coordinates
                e3D = e3D.rowRange(0,3)/e3D.at<float>(3);

                cv::Mat s3Dt = s3D.t();
                cv::Mat e3Dt = e3D.t();

                // 判断端点与光心的夹角
                cv::Mat normal1 = s3D - Ow1;
                float dist1 = cv::norm(normal1);

                cv::Mat normal2 = s3D - Ow2;
                float dist2 = cv::norm(normal2);

                cv::Mat normal3 = s3D - Ow3;
                float dist3 = cv::norm(normal3);

                float cosParallax1 = normal1.dot(normal2)/(dist1*dist2);
                float cosParallax2 = normal1.dot(normal3)/(dist1*dist3);

                if(cosParallax1 >= 0.99998 || cosParallax2 >= 0.99998)
                    continue;

                normal1 = e3D - Ow1;
                dist1 = cv::norm(normal1);

                normal2 = e3D - Ow2;
                dist2 = cv::norm(normal2);

                normal3 = e3D - Ow3;
                dist3 = cv::norm(normal3);

                cosParallax1 = normal1.dot(normal2)/(dist1*dist2);
                cosParallax2 = normal1.dot(normal3)/(dist1*dist3);

                if(cosParallax1 >= 0.99998 || cosParallax2 >= 0.99998)
                    continue;

                // 判断起始点是否离两个相机中心太近
                // 邻接关键帧的场景深度中值
                const float medianDepthKF2 = pKF2->ComputeSceneMedianDepth(2);
                cv::Mat v1 = s3D - Ow1;
                float distance1 = cv::norm(v1);
                const float ratio1 = distance1/medianDepthKF2;
                if(ratio1 < 0.3)
                    continue;

                cv::Mat v2 = s3D - Ow2;
                float distance2 = cv::norm(v2);
                const float ratio2 = distance2/medianDepthKF2;
                if(ratio2 < 0.3)
                    continue;

                // 判断线段是否太长
                cv::Mat v3 = e3D - s3D;
                float distance3 = cv::norm(v3);
                const float ratio3 = distance3/medianDepthKF2;
                if(ratio3 > 1)
                    continue;

                // step6.5：检测生成的3D点是否在相机前方
                float SZC1 = Rcw1.row(2).dot(s3Dt)+tcw1.at<float>(2);   //起始点在C1下的Z坐标值
                if(SZC1<=0)
                    continue;

                float EZC1 = Rcw1.row(2).dot(e3Dt)+tcw1.at<float>(2);   //终止点在C1下的Z坐标值
                if(EZC1<=0)
                    continue;

                float SZC2 = Rcw2.row(2).dot(s3Dt)+tcw2.at<float>(2);   //起始点在C2下的Z坐标值
                if(SZC2<=0)
                    continue;

                float EZC2 = Rcw2.row(2).dot(e3Dt)+tcw2.at<float>(2);   //终止点在C2下的Z坐标值
                if(EZC2<=0)
                    continue;

                float SZC3 = Rcw3.row(2).dot(s3Dt)+tcw3.at<float>(2);   //起始点在C3下的Z坐标值
                if(SZC3<=0)
                    continue;

                float EZC3 = Rcw3.row(2).dot(e3Dt)+tcw3.at<float>(2);   //终止点在C3下的Z坐标值
                if(EZC3<=0)
                    continue;

                // 检测重投影误差
                // 在第一个关键帧中检测重投影误差
                const float &sigmaSquare1 = mpCurrentKeyFrame->mvLevelSigma2Line[keyline1.octave];
                // 初始端点
                const float x1s = Rcw1.row(0).dot(s3Dt)+tcw1.at<float>(0);
                const float y1s = Rcw1.row(1).dot(s3Dt)+tcw1.at<float>(1);
                const float invz1s = 1.0/SZC1;

                float u1s = fx1*x1s*invz1s+cx1;
                float v1s = fy1*y1s*invz1s+cy1;
                // 计算重投影误差
                double err1s = keyline1_function(0)*u1s + keyline1_function(1)*v1s + keyline1_function(2);
                if((err1s*err1s)>3.84*sigmaSquare1)
                    continue;

                // 结束端点
                const float x1e = Rcw1.row(0).dot(e3Dt)+tcw1.at<float>(0);
                const float y1e = Rcw1.row(1).dot(e3Dt)+tcw1.at<float>(1);
                const float invz1e = 1.0/EZC1;

                float u1e = fx1*x1e*invz1e+cx1;
                float v1e = fy1*y1e*invz1e+cy1;
                // 计算重投影误差
                double err1e = keyline1_function(0)*u1e + keyline1_function(1)*v1e + keyline1_function(2);
                if((err1e*err1e)>3.84*sigmaSquare1)
                    continue;

                // 在第二个关键帧中检测重投影误差
                const float &sigmaSquare2 = pKF2->mvLevelSigma2Line[keyline2.octave];
                // 初始端点
                const float x2s = Rcw2.row(0).dot(s3Dt)+tcw2.at<float>(0);
                const float y2s = Rcw2.row(1).dot(s3Dt)+tcw2.at<float>(1);
                const float invz2s = 1.0/SZC2;

                float u2s = fx2*x2s*invz2s+cx2;
                float v2s = fy2*y2s*invz2s+cy2;
                // 计算重投影误差
                double err2s = keyline2_function(0)*u2s + keyline2_function(1)*v2s + keyline2_function(2);
                if((err2s*err2s)>3.84*sigmaSquare2)
                    continue;
                
                // 结束端点
                const float x2e = Rcw2.row(0).dot(e3Dt)+tcw2.at<float>(0);
                const float y2e = Rcw2.row(1).dot(e3Dt)+tcw2.at<float>(1);
                const float invz2e = 1.0/EZC2;

                float u2e = fx2*x2e*invz2e+cx2;
                float v2e = fy2*y2e*invz2e+cy2;
                // 计算重投影误差
                double err2e = keyline2_function(0)*u2e + keyline2_function(1)*v2e + keyline2_function(2);
                if((err2e*err2e)>3.84*sigmaSquare2)
                    continue;

                // 在第三个关键帧中检测重投影误差
                const float &sigmaSquare3 = pKF3->mvLevelSigma2Line[keyline3.octave];
                // 初始端点
                const float x3s = Rcw3.row(0).dot(s3Dt)+tcw3.at<float>(0);
                const float y3s = Rcw3.row(1).dot(s3Dt)+tcw3.at<float>(1);
                const float invz3s = 1.0/SZC3;

                float u3s = fx3*x3s*invz3s+cx3;
                float v3s = fy3*y3s*invz3s+cy3;
                // 计算重投影误差
                double err3s = keyline3_function(0)*u3s + keyline3_function(1)*v3s + keyline3_function(2);
                if((err3s*err3s)>3.84*sigmaSquare3)
                    continue;
                
                // 结束端点
                const float x3e = Rcw3.row(0).dot(e3Dt)+tcw3.at<float>(0);
                const float y3e = Rcw3.row(1).dot(e3Dt)+tcw3.at<float>(1);
                const float invz3e = 1.0/EZC3;

                float u3e = fx3*x3e*invz3e+cx3;
                float v3e = fy3*y3e*invz3e+cy3;
                // 计算重投影误差
                double err3e = keyline3_function(0)*u3e + keyline3_function(1)*v3e + keyline3_function(2);
                if((err3e*err3e)>3.84*sigmaSquare3)
                    continue;

                // 检测线重叠误差
                // 在第一个关键帧中检测线重叠误差
                if(fabs(keyline1.angle) < 3.0*PI/4.0 && fabs(keyline1.angle) > 1.0*PI/4.0)
                {
                    if(min(v1e, v1s) > max(keyline1.startPointY, keyline1.endPointY) || min(keyline1.startPointY, keyline1.endPointY) > max(v1e, v1s))
                        continue;

                    float Ymax = min(max(v1e, v1s), max(keyline1.startPointY, keyline1.endPointY));
                    float Ymin= max(min(v1e, v1s), min(keyline1.startPointY, keyline1.endPointY));

                    float ratio1 = (Ymax - Ymin)/(max(v1e, v1s)-min(v1e, v1s));
                    float ratio2 = (Ymax - Ymin)/(max(keyline1.startPointY, keyline1.endPointY)-min(keyline1.startPointY, keyline1.endPointY));

                    if(ratio1 < 0.85 || ratio2 < 0.85)
                        continue;
                }else{
                     if(min(u1e, u1s) > max(keyline1.startPointX, keyline1.endPointX) || min(keyline1.startPointX, keyline1.endPointX) > max(u1e, u1s))
                        continue;

                    float Xmax = min(max(u1e, u1s), max(keyline1.startPointX, keyline1.endPointX));
                    float Xmin= max(min(u1e, u1s), min(keyline1.startPointX, keyline1.endPointX));

                    float ratio1 = (Xmax - Xmin)/(max(u1e, u1s)-min(u1e, u1s));
                    float ratio2 = (Xmax - Xmin)/(max(keyline1.startPointX, keyline1.endPointX)-min(keyline1.startPointX, keyline1.endPointX));

                    if(ratio1 < 0.85 || ratio2 < 0.85)
                        continue;
                }

                //在第二个关键帧中检测线重叠误差
                if(fabs(keyline2.angle) < 3.0*PI/4.0 && fabs(keyline2.angle) > 1.0*PI/4.0)
                {
                    if(min(v2e, v2s) > max(keyline2.startPointY, keyline2.endPointY) || min(keyline2.startPointY, keyline2.endPointY) > max(v2e, v2s))
                        continue;

                    float Ymax = min(max(v2e, v2s), max(keyline2.startPointY, keyline2.endPointY));
                    float Ymin= max(min(v2e, v2s), min(keyline2.startPointY, keyline2.endPointY));

                    float ratio1 = (Ymax - Ymin)/(max(v2e, v2s)-min(v2e, v2s));
                    float ratio2 = (Ymax - Ymin)/(max(keyline2.startPointY, keyline2.endPointY)-min(keyline2.startPointY, keyline2.endPointY));

                    if(ratio1 < 0.85 || ratio2 < 0.85)
                        continue;
                }else{
                     if(min(u2e, u2s) > max(keyline2.startPointX, keyline2.endPointX) || min(keyline2.startPointX, keyline2.endPointX) > max(u2e, u2s))
                        continue;

                    float Xmax = min(max(u2e, u2s), max(keyline2.startPointX, keyline2.endPointX));
                    float Xmin= max(min(u2e, u2s), min(keyline2.startPointX, keyline2.endPointX));

                    float ratio1 = (Xmax - Xmin)/(max(u2e, u2s)-min(u2e, u2s));
                    float ratio2 = (Xmax - Xmin)/(max(keyline2.startPointX, keyline2.endPointX)-min(keyline2.startPointX, keyline2.endPointX));

                    if(ratio1 < 0.85 || ratio2 < 0.85)
                        continue;
                }

                //在第三个关键帧中检测线重叠误差
                if(fabs(keyline3.angle) < 3.0*PI/4.0 && fabs(keyline3.angle) > 1.0*PI/4.0)
                {
                    if(min(v3e, v3s) > max(keyline3.startPointY, keyline3.endPointY) || min(keyline3.startPointY, keyline3.endPointY) > max(v3e, v3s))
                        continue;

                    float Ymax = min(max(v3e, v3s), max(keyline3.startPointY, keyline3.endPointY));
                    float Ymin= max(min(v3e, v3s), min(keyline3.startPointY, keyline3.endPointY));

                    float ratio1 = (Ymax - Ymin)/(max(v3e, v3s)-min(v3e, v3s));
                    float ratio2 = (Ymax - Ymin)/(max(keyline3.startPointY, keyline3.endPointY)-min(keyline3.startPointY, keyline3.endPointY));

                    if(ratio1 < 0.85 || ratio2 < 0.85)
                        continue;
                }else{
                     if(min(u3e, u3s) > max(keyline3.startPointX, keyline3.endPointX) || min(keyline3.startPointX, keyline3.endPointX) > max(u3e, u3s))
                        continue;

                    float Xmax = min(max(u3e, u3s), max(keyline3.startPointX, keyline3.endPointX));
                    float Xmin= max(min(u3e, u3s), min(keyline3.startPointX, keyline3.endPointX));

                    float ratio1 = (Xmax - Xmin)/(max(u3e, u3s)-min(u3e, u3s));
                    float ratio2 = (Xmax - Xmin)/(max(keyline3.startPointX, keyline3.endPointX)-min(keyline3.startPointX, keyline3.endPointX));

                    if(ratio1 < 0.85 || ratio2 < 0.85)
                        continue;
                }

                // step6.9: 三角化成功，构造MapLine
                Vector6d line3D;
                line3D << s3D.at<float>(0), s3D.at<float>(1), s3D.at<float>(2), e3D.at<float>(0), e3D.at<float>(1), e3D.at<float>(2);
                MapLine* pML = new MapLine(line3D, mpCurrentKeyFrame, mpMap);

                // step6.10：为该MapLine添加属性
                pML->AddObservation(mpCurrentKeyFrame, ikl);
                pML->AddObservation(pKF2, idx1);
                pML->AddObservation(pKF3, idx2);

                mpCurrentKeyFrame->AddMapLine(pML, ikl);
                pKF2->AddMapLine(pML, idx1);
                pKF3->AddMapLine(pML, idx2);

                pML->ComputeDistinctiveDescriptors();
                pML->UpdateAverageDir();
                mpMap->AddMapLine(pML);

                // step6.11：将新产生的线特征放入检测队列，这些MapLines都会经过MapLineCulling函数的检验
                mlpRecentAddedMapLines.push_back(pML);

                nnew++;

                //FOR DEBUG
                
                cv::Mat pic_Temp1, pic_Temp2;
                
                cubemapMatch_rgb.copyTo(pic_Temp2);
                cubemapMatch_rgb.copyTo(pic_Temp1);

                cv::Point2f Point_1, Point_2, Point_3;
                cv::Point2f lp11, lp12, lp21, lp22, lp31, lp32;
                cv::Point2f lp11r, lp12r, lp21r, lp22r, lp31r, lp32r;
                KeyLine line_1 = keyline1;
                KeyLine line_2 = keyline2;
                KeyLine line_3 = keyline3;
                Point_1.x = (line_1.startPointX + line_1.endPointX)/2.0;
                Point_1.y = (line_1.startPointY + line_1.endPointY)/2.0;
                Point_2.x = (line_2.startPointX + line_2.endPointX)/2.0 + pKF2->ImageGray.cols;
                Point_2.y = (line_2.startPointY + line_2.endPointY)/2.0;
                Point_3.x = (line_3.startPointX + line_3.endPointX)/2.0 + pKF3->ImageGray.cols*2;
                Point_3.y = (line_3.startPointY + line_3.endPointY)/2.0;
                
                lp11r.x = line_1.startPointX; lp11r.y = line_1.startPointY; 
                lp12r.x = line_1.endPointX; lp12r.y = line_1.endPointY; 
                lp21r.x = line_2.startPointX+ pKF2->ImageGray.cols; lp21r.y = line_2.startPointY; 
                lp22r.x = line_2.endPointX+ pKF2->ImageGray.cols; lp22r.y = line_2.endPointY; 
                lp31r.x = line_3.startPointX+ pKF3->ImageGray.cols*2; lp31r.y = line_3.startPointY; 
                lp32r.x = line_3.endPointX+ pKF3->ImageGray.cols*2; lp32r.y = line_3.endPointY; 
                cv::line(pic_Temp2, lp11r, lp12r, Scalar(255,255,0), 2, cv::LINE_AA);
                cv::line(pic_Temp2, lp21r, lp22r, Scalar(255,255,0), 2, cv::LINE_AA);
                cv::line(pic_Temp2, lp31r, lp32r, Scalar(255,255,0), 2, cv::LINE_AA);
                
                lp11.x = u1s; lp11.y = v1s; 
                lp12.x = u1e; lp12.y = v1e; 
                lp21.x = u2s+ pKF2->ImageGray.cols; lp21.y = v2s; 
                lp22.x = u2e+ pKF2->ImageGray.cols; lp22.y = v2e; 
                lp31.x = u3s+ pKF3->ImageGray.cols*2; lp31.y = v3s; 
                lp32.x = u3e+ pKF3->ImageGray.cols*2; lp32.y = v3e; 
                cv::line(pic_Temp1, lp11, lp12, Scalar(255,0,255), 1, cv::LINE_AA);
                cv::line(pic_Temp1, lp21, lp22, Scalar(255,0,255), 1, cv::LINE_AA);
                cv::line(pic_Temp1, lp31, lp32, Scalar(255,0,255), 1, cv::LINE_AA);

                cv::line(pic_Temp1, Point_1, Point_2, Scalar(0,255,255), 1, cv::LINE_AA);
                cv::line(pic_Temp1, Point_2, Point_3, Scalar(0,255,255), 1, cv::LINE_AA);

                //cv::line(pic_Temp2, Point_1, Point_2, Scalar(0,255,255), 1, cv::LINE_AA);
                //cv::line(pic_Temp2, Point_2, Point_3, Scalar(0,255,255), 1, cv::LINE_AA);

                cv::imwrite("./tt1.jpg", pic_Temp1);
                cv::imwrite("./tt2.jpg", pic_Temp2);
                //cerr<<CosSita<<endl;

            }

        }
    }

    cout<<endl;
    cout<<"Make "<< nnew << "Lines"<<endl;
    cout<<endl;

}

/**
 * 检查并融合当前关键帧与相邻帧（两级相邻）重复的MapPoints
 */
void LocalMapping::SearchInNeighbors()
{
    // Retrieve neighbor keyframes
    // step1:获得当前关键帧在covisibility图中权重排名前nn的邻接关键帧，找到当前帧一级相邻与二级相邻关键帧
    int nn = 10;
    if(mbMonocular)
        nn=20;
    const vector<KeyFrame*> vpNeighKFs = mpCurrentKeyFrame->GetBestCovisibilityKeyFrames(nn);
    vector<KeyFrame*> vpTargetKFs;
    for(vector<KeyFrame*>::const_iterator vit=vpNeighKFs.begin(), vend=vpNeighKFs.end(); vit!=vend; vit++)
    {
        KeyFrame* pKFi = *vit;
        if(pKFi->isBad() || pKFi->mnFuseTargetForKF == mpCurrentKeyFrame->mnId)
            continue;
        vpTargetKFs.push_back(pKFi);    // 加入一级相邻帧
        pKFi->mnFuseTargetForKF = mpCurrentKeyFrame->mnId;

        // Extend to some second neighbors
        const vector<KeyFrame*> vpSecondNeighKFs = pKFi->GetBestCovisibilityKeyFrames(5);
        for(vector<KeyFrame*>::const_iterator vit2=vpSecondNeighKFs.begin(), vend2=vpSecondNeighKFs.end(); vit2!=vend2; vit2++)
        {
            KeyFrame* pKFi2 = *vit2;
            if(pKFi2->isBad() || pKFi2->mnFuseTargetForKF==mpCurrentKeyFrame->mnId || pKFi2->mnId==mpCurrentKeyFrame->mnId)
                continue;
            vpTargetKFs.push_back(pKFi2);
        }
    }

    //=====================MapPoint====================
    // Search matches by projection from current KF in target KFs
    ORBmatcher matcher;
    // step2：将当前帧的MapPoints分别与一级二级相邻帧（的MapPoints）进行融合
    vector<MapPoint*> vpMapPointMatches = mpCurrentKeyFrame->GetMapPointMatches();
    for(vector<KeyFrame*>::iterator vit=vpTargetKFs.begin(), vend=vpTargetKFs.end(); vit!=vend; vit++)
    {
        KeyFrame* pKFi = *vit;

        // 投影当前帧的MapPoints到相邻关键帧pKFi中，并判断是否有重复的MapPoints
        // 1.如果MapPoint能匹配关键帧的特征点，并且该点有对应的MapPoint，那么将两个MapPoint合并
        // 2.如果MapPoint能匹配关键帧的特征点，但是该点没有对应的MapPoint，那么为该点添加MapPoint
        matcher.Fuse(pKFi,vpMapPointMatches);
    }

    // Search matches by projection from target KFs in current KF
    // 用于存储一级邻接和二级邻接关键帧所有的MapPoints的集合
    vector<MapPoint*> vpFuseCandidates;
    vpFuseCandidates.reserve(vpTargetKFs.size()*vpMapPointMatches.size());

    // step3：将一级二级相邻帧的MapPoints分别与当前帧（的MapPoints)进行融合
    // 遍历每一个一级邻接和二级邻接关键帧
    for(vector<KeyFrame*>::iterator vitKF=vpTargetKFs.begin(), vendKF=vpTargetKFs.end(); vitKF!=vendKF; vitKF++)
    {
        KeyFrame* pKFi = *vitKF;

        vector<MapPoint*> vpMapPointsKFi = pKFi->GetMapPointMatches();

        for(vector<MapPoint*>::iterator vitMP=vpMapPointsKFi.begin(), vendMP=vpMapPointsKFi.end(); vitMP!=vendMP; vitMP++)
        {
            MapPoint* pMP = *vitMP;
            if(!pMP)
                continue;
            if(pMP->isBad() || pMP->mnFuseCandidateForKF == mpCurrentKeyFrame->mnId)
                continue;
            pMP->mnFuseCandidateForKF = mpCurrentKeyFrame->mnId;
            vpFuseCandidates.push_back(pMP);
        }
    }

    matcher.Fuse(mpCurrentKeyFrame,vpFuseCandidates);

    // Update points
    // step4：更新当前帧MapPoints的描述子，深度，观测主方向等属性
    vpMapPointMatches = mpCurrentKeyFrame->GetMapPointMatches();
    for(size_t i=0, iend=vpMapPointMatches.size(); i<iend; i++)
    {
        MapPoint* pMP=vpMapPointMatches[i];
        if(pMP)
        {
            if(!pMP->isBad())
            {
                // 在所有找到pMP的关键帧中，获得最佳的描述子
                pMP->ComputeDistinctiveDescriptors();
                // 更新平均观测方向和观测距离
                pMP->UpdateNormalAndDepth();
            }
        }
    }

    //=====================MapLine====================
    // Search matches by projection from current KF in target KFs
    LSDmatcher lineMatcher(0.6);
    vector<MapLine*> vpMapLineMatches = mpCurrentKeyFrame->GetMapLineMatches();     //也就是当前帧的mvpMapLines
    for(vector<KeyFrame*>::iterator vit=vpTargetKFs.begin(), vend=vpTargetKFs.end(); vit!=vend; vit++)
    {
        KeyFrame* pKFi = *vit;
        lineMatcher.Fuse(pKFi, vpMapLineMatches);
    }

    vector<MapLine*> vpLineFuseCandidates;
    vpLineFuseCandidates.reserve(vpTargetKFs.size()*vpMapLineMatches.size());

    for(vector<KeyFrame*>::iterator vitKF=vpTargetKFs.begin(), vendKF=vpTargetKFs.end(); vitKF!=vendKF; vitKF++)
    {
        KeyFrame* pKFi = *vitKF;

        vector<MapLine*> vpMapLinesKFi = pKFi->GetMapLineMatches();

        // 遍历当前一级邻接和二级邻接关键帧中所有的MapLines
        for(vector<MapLine*>::iterator vitML=vpMapLinesKFi.begin(), vendML=vpMapLinesKFi.end(); vitML!=vendML; vitML++)
        {
            MapLine* pML = *vitML;
            if(!pML)
                continue;

            if(pML->isBad() || pML->mnFuseCandidateForKF == mpCurrentKeyFrame->mnId)
                continue;

            pML->mnFuseCandidateForKF = mpCurrentKeyFrame->mnId;
            vpLineFuseCandidates.push_back(pML);
        }
    }

    lineMatcher.Fuse(mpCurrentKeyFrame, vpLineFuseCandidates);

    // Update Lines
    vpMapLineMatches = mpCurrentKeyFrame->GetMapLineMatches();
    for(size_t i=0, iend=vpMapLineMatches.size(); i<iend; i++)
    {
        MapLine* pML=vpMapLineMatches[i];
        if(pML)
        {
            if(!pML->isBad())
            {
                pML->ComputeDistinctiveDescriptors();
                pML->UpdateAverageDir();
            }
        }
    }

    // Update connections in covisibility graph
    // step5：更新当前帧的MapPoints后更新与其他帧的连接关系，更新Covisibility图
    mpCurrentKeyFrame->UpdateConnections();
}

/**
 * 检查并融合当前关键帧与相邻帧重复的MapLines
 */
void LocalMapping::SearchLineInNeighbors()
{
    // step1:获得当前关键帧在Covisibility图中权重排名前nn的邻接关键帧，再寻找与一级关键帧相连的二级关键帧
    int nn=10;
    if(mbMonocular)
        nn=20;
    const vector<KeyFrame*> vpNeighKFs = mpCurrentKeyFrame->GetBestCovisibilityKeyFrames(nn);

    vector<KeyFrame*> vpTargetKFs;
    for(vector<KeyFrame*>::const_iterator vit=vpNeighKFs.begin(), vend=vpNeighKFs.end(); vit!=vend; vit++)
    {
        KeyFrame* pKFi = *vit;
        if(pKFi->isBad() || pKFi->mnFuseTargetForKF==mpCurrentKeyFrame->mnId)
            continue;
        vpTargetKFs.push_back(pKFi);
        pKFi->mnFuseTargetForKF = mpCurrentKeyFrame->mnId;

        // Extend to some second neighbors
        const vector<KeyFrame*> vpSecondNeighKFs = pKFi->GetBestCovisibilityKeyFrames(5);
        for(vector<KeyFrame*>::const_iterator vit2=vpSecondNeighKFs.begin(), vend2=vpSecondNeighKFs.end(); vit2!=vit2; vit2++)
        {
            KeyFrame* pKFi2 = *vit2;
            if(pKFi2->isBad() || pKFi2->mnFuseTargetForKF==mpCurrentKeyFrame->mnId  || pKFi2->mnId==mpCurrentKeyFrame->mnId)
                continue;
            vpTargetKFs.push_back(pKFi2);
        }
    }

    cout << "vpTargetKFs.size() = " << vpTargetKFs.size() << endl;

    LSDmatcher matcher;

    // step2:将当前帧的MapLines分别与一级和二级相邻帧的MapLines进行融合
    vector<MapLine*> vpMapLineMatches = mpCurrentKeyFrame->GetMapLineMatches();     //也就是当前帧的mvpMapLines

    cout << "vpMapLineMatches.size() = " << vpMapLineMatches.size() << endl;
    for(vector<KeyFrame*>::iterator vit=vpTargetKFs.begin(), vend=vpTargetKFs.end(); vit!=vend; vit++)
    {
        KeyFrame* pKFi = *vit;
        matcher.Fuse(pKFi, vpMapLineMatches);
    }

    vector<MapLine*> vpFuseCandidates;
    vpFuseCandidates.reserve(vpTargetKFs.size()*vpMapLineMatches.size());

    for(vector<KeyFrame*>::iterator vitKF=vpTargetKFs.begin(), vendKF=vpTargetKFs.end(); vitKF!=vendKF; vitKF++)
    {
        KeyFrame* pKFi = *vitKF;

        vector<MapLine*> vpMapLinesKFi = pKFi->GetMapLineMatches();

        // 遍历当前一级邻接和二级邻接关键帧中所有的MapLines
        for(vector<MapLine*>::iterator vitML=vpMapLinesKFi.begin(), vendML=vpMapLinesKFi.end(); vitML!=vendML; vitML++)
        {
            MapLine* pML = *vitML;
            if(!pML)
                continue;

            if(pML->isBad() || pML->mnFuseCandidateForKF == mpCurrentKeyFrame->mnId)
                continue;

            pML->mnFuseCandidateForKF = mpCurrentKeyFrame->mnId;
            vpFuseCandidates.push_back(pML);
        }
    }

    cout << "vpFuseCandidates.size() = " << vpFuseCandidates.size() << endl;
    matcher.Fuse(mpCurrentKeyFrame, vpFuseCandidates);

    // Update Lines
    vpMapLineMatches = mpCurrentKeyFrame->GetMapLineMatches();
    for(size_t i=0, iend=vpMapLineMatches.size(); i<iend; i++)
    {
        MapLine* pML=vpMapLineMatches[i];
        if(pML)
        {
            if(!pML->isBad())
            {
                pML->ComputeDistinctiveDescriptors();
                pML->UpdateAverageDir();
            }
        }
    }

    mpCurrentKeyFrame->UpdateConnections();
}

cv::Mat LocalMapping::ComputeF12(KeyFrame *&pKF1, KeyFrame *&pKF2)
{
    cv::Mat R1w = pKF1->GetRotation();
    cv::Mat t1w = pKF1->GetTranslation();
    cv::Mat R2w = pKF2->GetRotation();
    cv::Mat t2w = pKF2->GetTranslation();

    cv::Mat R12 = R1w*R2w.t();
    cv::Mat t12 = -R1w*R2w.t()*t2w+t1w;

    cv::Mat t12x = SkewSymmetricMatrix(t12);

    const cv::Mat &K1 = pKF1->mK;
    const cv::Mat &K2 = pKF2->mK;


    return K1.t().inv()*t12x*R12*K2.inv();
}

void LocalMapping::RequestStop()
{
    unique_lock<mutex> lock(mMutexStop);
    mbStopRequested = true;
    unique_lock<mutex> lock2(mMutexNewKFs);
    mbAbortBA = true;
}

bool LocalMapping::Stop()
{
    unique_lock<mutex> lock(mMutexStop);
    if(mbStopRequested && !mbNotStop)
    {
        mbStopped = true;
        cout << "Local Mapping STOP" << endl;
        return true;
    }

    return false;
}

bool LocalMapping::isStopped()
{
    unique_lock<mutex> lock(mMutexStop);
    return mbStopped;
}

bool LocalMapping::stopRequested()
{
    unique_lock<mutex> lock(mMutexStop);
    return mbStopRequested;
}

void LocalMapping::Release()
{
    unique_lock<mutex> lock(mMutexStop);
    unique_lock<mutex> lock2(mMutexFinish);
    if(mbFinished)
        return;
    mbStopped = false;
    mbStopRequested = false;
    for(list<KeyFrame*>::iterator lit = mlNewKeyFrames.begin(), lend=mlNewKeyFrames.end(); lit!=lend; lit++)
        delete *lit;
    mlNewKeyFrames.clear();

    cout << "Local Mapping RELEASE" << endl;
}

bool LocalMapping::AcceptKeyFrames()
{
    unique_lock<mutex> lock(mMutexAccept);
    return mbAcceptKeyFrames;
}

void LocalMapping::SetAcceptKeyFrames(bool flag)
{
    unique_lock<mutex> lock(mMutexAccept);
    mbAcceptKeyFrames=flag;
}

bool LocalMapping::SetNotStop(bool flag)
{
    unique_lock<mutex> lock(mMutexStop);

    if(flag && mbStopped)
        return false;

    mbNotStop = flag;

    return true;
}

void LocalMapping::InterruptBA()
{
    mbAbortBA = true;
}

void LocalMapping::KeyFrameCulling()
{
    // Check redundant keyframes (only local keyframes)
    // A keyframe is considered redundant if the 90% of the MapPoints it sees, are seen
    // in at least other 3 keyframes (in the same or finer scale)
    // We only consider close stereo points
    vector<KeyFrame*> vpLocalKeyFrames = mpCurrentKeyFrame->GetVectorCovisibleKeyFrames();

    for(vector<KeyFrame*>::iterator vit=vpLocalKeyFrames.begin(), vend=vpLocalKeyFrames.end(); vit!=vend; vit++)
    {
        KeyFrame* pKF = *vit;
        if(pKF->mnId==0)
            continue;
        const vector<MapPoint*> vpMapPoints = pKF->GetMapPointMatches();

        int nObs = 3;
        const int thObs=nObs;
        int nRedundantObservations=0;
        int nMPs=0;
        for(size_t i=0, iend=vpMapPoints.size(); i<iend; i++)
        {
            MapPoint* pMP = vpMapPoints[i];
            if(pMP)
            {
                if(!pMP->isBad())
                {
                    if(!mbMonocular)
                    {
                        if(pKF->mvDepth[i]>pKF->mThDepth || pKF->mvDepth[i]<0)
                            continue;
                    }

                    nMPs++;
                    if(pMP->Observations()>thObs)
                    {
                        const int &scaleLevel = pKF->mvKeysUn[i].octave;
                        const map<KeyFrame*, size_t> observations = pMP->GetObservations();
                        int nObs=0;
                        for(map<KeyFrame*, size_t>::const_iterator mit=observations.begin(), mend=observations.end(); mit!=mend; mit++)
                        {
                            KeyFrame* pKFi = mit->first;
                            if(pKFi==pKF)
                                continue;
                            const int &scaleLeveli = pKFi->mvKeysUn[mit->second].octave;

                            if(scaleLeveli<=scaleLevel+1)
                            {
                                nObs++;
                                if(nObs>=thObs)
                                    break;
                            }
                        }
                        if(nObs>=thObs)
                        {
                            nRedundantObservations++;
                        }
                    }
                }
            }
        }  

        if(nRedundantObservations>0.9*nMPs)
            pKF->SetBadFlag();
    }
}

cv::Mat LocalMapping::SkewSymmetricMatrix(const cv::Mat &v)
{
    return (cv::Mat_<float>(3,3) <<             0, -v.at<float>(2), v.at<float>(1),
            v.at<float>(2),               0,-v.at<float>(0),
            -v.at<float>(1),  v.at<float>(0),              0);
}

void LocalMapping::RequestReset()
{
    {
        unique_lock<mutex> lock(mMutexReset);
        mbResetRequested = true;
    }

    while(1)
    {
        {
            unique_lock<mutex> lock2(mMutexReset);
            if(!mbResetRequested)
                break;
        }
        usleep(3000);
    }
}

void LocalMapping::ResetIfRequested()
{
    unique_lock<mutex> lock(mMutexReset);
    if(mbResetRequested)
    {
        mlNewKeyFrames.clear();
        mlpRecentAddedMapPoints.clear();    // 点特征
        mlpRecentAddedMapLines.clear();     // 线特征
        mbResetRequested=false;
    }
}

void LocalMapping::RequestFinish()
{
    unique_lock<mutex> lock(mMutexFinish);
    mbFinishRequested = true;
}

bool LocalMapping::CheckFinish()
{
    unique_lock<mutex> lock(mMutexFinish);
    return mbFinishRequested;
}

void LocalMapping::SetFinish()
{
    unique_lock<mutex> lock(mMutexFinish);
    mbFinished = true;    
    unique_lock<mutex> lock2(mMutexStop);
    mbStopped = true;
}

bool LocalMapping::isFinished()
{
    unique_lock<mutex> lock(mMutexFinish);
    return mbFinished;
}

cv::Mat LocalMapping::DrawLines(KeyFrame* KF1, KeyFrame* KF2)
{
        cv::Mat mImRGBPrevTemp, mImRGBTemp;
        int nRows = KF2->ImageGray.rows; 
        int nCols = KF2->ImageGray.cols;
        cv::cvtColor(KF1->ImageGray, mImRGBPrevTemp, cv::COLOR_GRAY2BGR);
        cv::cvtColor(KF2->ImageGray, mImRGBTemp, cv::COLOR_GRAY2BGR);
        cv::Mat cubemapMatch_rgb(nRows, nCols * 2, mImRGBTemp.type());

        drawKeylines(mImRGBPrevTemp, KF1->mvKeyLines, mImRGBPrevTemp, Scalar(200, 0, 0));
        drawKeylines(mImRGBTemp, KF2->mvKeyLines, mImRGBTemp, Scalar(0, 200, 0));

        for(int i = 0; i < KF1->mvKeyLines.size(); i++){
            cv::Point origin;
            KeyLine line_temp = KF1->mvKeyLines[i];
            origin.x = (line_temp.startPointX + line_temp.endPointX)/2.0;
            origin.y = (line_temp.startPointY + line_temp.endPointY)/2.0;
            cv::putText(mImRGBPrevTemp, std::to_string(i), origin, cv::FONT_HERSHEY_COMPLEX, 0.4, cv::Scalar(200, 0, 0), 1, 8, 0);
        }
        (mImRGBPrevTemp.rowRange(0, mImRGBPrevTemp.rows).colRange(0, mImRGBPrevTemp.cols)).copyTo(cubemapMatch_rgb.colRange(0, nCols));
            
        for(int i = 0; i < KF2->mvKeyLines.size(); i++){
            cv::Point origin;
            KeyLine line_temp = KF2->mvKeyLines[i];
            origin.x = (line_temp.startPointX + line_temp.endPointX)/2.0;
            origin.y = (line_temp.startPointY + line_temp.endPointY)/2.0;
            cv::putText(mImRGBTemp, std::to_string(i), origin, cv::FONT_HERSHEY_COMPLEX, 0.4, cv::Scalar(0, 200, 0), 1, 8, 0);
        }
        (mImRGBTemp.rowRange(0, mImRGBTemp.rows).colRange(0, mImRGBTemp.cols)).copyTo(cubemapMatch_rgb.colRange(nCols, nCols * 2));

        return cubemapMatch_rgb;
}

} //namespace ORB_SLAM
