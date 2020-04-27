/*
 * Copyright (c) 2019, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.

 *
 */

/*
 * File:   OmafExtractorSelector.cpp
 * Author: media
 *
 * Created on May 28, 2019, 1:19 PM
 */

#include "OmafExtractorSelector.h"
#include "OmafMediaStream.h"
#include "OmafReaderManager.h"
#include <cfloat>
#include <math.h>
#include <chrono>
#include <cstdint>
#ifndef _ANDROID_NDK_OPTION_
#include "../trace/MtHQ_tp.h"
#endif

VCD_OMAF_BEGIN

OmafExtractorSelector::OmafExtractorSelector( int size )
{
    pthread_mutex_init(&mMutex, NULL);
    mSize = size;
    m360ViewPortHandle = nullptr;
    mParamViewport = nullptr;
    mCurrentExtractor = nullptr;
    mPose = nullptr;
    mUsePrediction = false;
    mPredictPluginName = "";
    mLibPath = "";
}

OmafExtractorSelector::~OmafExtractorSelector()
{
    pthread_mutex_destroy( &mMutex );

    if(m360ViewPortHandle)
    {
        I360SCVP_unInit(m360ViewPortHandle);
        m360ViewPortHandle = nullptr;
    }

    SAFE_DELETE(mParamViewport);

    if(mPoseHistory.size())
    {
        for(auto &p:mPoseHistory)
        {
            SAFE_DELETE(p.pose);
        }
    }
    if (mPredictPluginMap.size())
    {
        for (auto &p:mPredictPluginMap)
        {
            SAFE_DELETE(p.second);
        }
    }
    mUsePrediction = false;
}

int OmafExtractorSelector::SelectExtractors(OmafMediaStream* pStream)
{
    OmafExtractor* pSelectedExtrator = NULL;
    if(mUsePrediction)
    {
        ListExtractor predict_extractors = GetExtractorByPosePrediction( pStream );
        if (predict_extractors.empty())
        {
            if (mPoseHistory.size() < POSE_SIZE) // at the beginning
            {
                pSelectedExtrator = GetExtractorByPose( pStream );
            }
        }
        else
            pSelectedExtrator = predict_extractors.front();
    }
    else
    {
        pSelectedExtrator = GetExtractorByPose( pStream );
    }

    if(NULL == pSelectedExtrator && !mCurrentExtractor)
        return ERROR_NULL_PTR;

    bool isExtractorChanged = false;
    //not first time and changed and change to different extractor
    if (mCurrentExtractor && pSelectedExtrator && mCurrentExtractor != pSelectedExtrator)
    {
        isExtractorChanged = true;
    }

    mCurrentExtractor = pSelectedExtrator ? pSelectedExtrator : mCurrentExtractor;

    ListExtractor extractors;

    extractors.push_front(mCurrentExtractor);

    if( isExtractorChanged || extractors.size() > 1)
    {
        list<int> trackIDs;
        for(auto &it: extractors)
        {
            trackIDs.push_back(it->GetTrackNumber());
        }
        READERMANAGER::GetInstance()->RemoveTrackFromPacketQueue(trackIDs);
    }

    int ret = pStream->UpdateEnabledExtractors(extractors);

    return ret;
}

int OmafExtractorSelector::UpdateViewport(HeadPose* pose)
{
    if (!pose)
        return ERROR_NULL_PTR;

    pthread_mutex_lock(&mMutex);

    PoseInfo pi;
    pi.pose = new HeadPose;
    memcpy(pi.pose, pose, sizeof(HeadPose));
    std::chrono::high_resolution_clock clock;
    pi.time = std::chrono::duration_cast<std::chrono::milliseconds>(clock.now().time_since_epoch()).count();
    mPoseHistory.push_front(pi);
    if( mPoseHistory.size() > (uint32_t)(this->mSize) )
    {
        auto pit = mPoseHistory.back();
        SAFE_DELETE(pit.pose);
        mPoseHistory.pop_back();
    }

    pthread_mutex_unlock(&mMutex);
    return ERROR_NONE;
}

int OmafExtractorSelector::SetInitialViewport( std::vector<Viewport*>& pView, HeadSetInfo* headSetInfo, OmafMediaStream* pStream)
{
    if(!headSetInfo || !headSetInfo->viewPort_hFOV || !headSetInfo->viewPort_vFOV
        || !headSetInfo->viewPort_Width || !headSetInfo->viewPort_Height)
    {
        return ERROR_INVALID;
    }

    mParamViewport = new param_360SCVP;
    mParamViewport->usedType = E_VIEWPORT_ONLY;
    mParamViewport->paramViewPort.viewportWidth = headSetInfo->viewPort_Width;
    mParamViewport->paramViewPort.viewportHeight = headSetInfo->viewPort_Height;
    mParamViewport->paramViewPort.viewPortPitch = headSetInfo->pose->pitch;
    mParamViewport->paramViewPort.viewPortYaw = headSetInfo->pose->yaw;
    mParamViewport->paramViewPort.viewPortFOVH = headSetInfo->viewPort_hFOV;
    mParamViewport->paramViewPort.viewPortFOVV = headSetInfo->viewPort_vFOV;
    mParamViewport->paramViewPort.geoTypeInput = (EGeometryType)headSetInfo->input_geoType;
    mParamViewport->paramViewPort.geoTypeOutput = (EGeometryType)headSetInfo->output_geoType;
    mParamViewport->paramViewPort.tileNumRow = pStream->GetRowSize();
    mParamViewport->paramViewPort.tileNumCol = pStream->GetColSize();
    mParamViewport->paramViewPort.usageType = E_VIEWPORT_ONLY;
    mParamViewport->paramViewPort.faceWidth = pStream->GetStreamHighResWidth();
    mParamViewport->paramViewPort.faceHeight = pStream->GetStreamHighResHeight();
    m360ViewPortHandle = I360SCVP_Init(mParamViewport);
    if(!m360ViewPortHandle)
        return ERROR_NULL_PTR;

    //set current Pose;
    mPose = new HeadPose;
    memcpy(mPose, headSetInfo->pose, sizeof(HeadPose));

    return UpdateViewport(mPose);
}

bool OmafExtractorSelector::IsDifferentPose(HeadPose* pose1, HeadPose* pose2)
{
    // return false if two pose is same
    if(abs(pose1->yaw - pose2->yaw)<1e-3 && abs(pose1->pitch - pose2->pitch)<1e-3)
    {
        LOG(INFO)<<"pose has not changed!"<<std::endl;
        return false;
    }
    return true;
}

OmafExtractor* OmafExtractorSelector::GetExtractorByPose( OmafMediaStream* pStream )
{
    pthread_mutex_lock(&mMutex);
    if(mPoseHistory.size() == 0)
    {
        pthread_mutex_unlock(&mMutex);
        return NULL;
    }

    HeadPose* previousPose = mPose;
    int64_t historySize = 0;

    mPose = mPoseHistory.front().pose;
    mPoseHistory.pop_front();

    if(!mPose)
    {
        pthread_mutex_unlock(&mMutex);
        return nullptr;
    }

    historySize = mPoseHistory.size();

    pthread_mutex_unlock(&mMutex);

    // won't get viewport if pose hasn't changed
    if( previousPose && mPose && !IsDifferentPose( previousPose, mPose ) && historySize > 1)
    {
        LOG(INFO)<<"pose hasn't changed!"<<endl;
#ifndef _ANDROID_NDK_OPTION_
        //trace
        tracepoint(mthq_tp_provider, T2_detect_pose_change, 0);
#endif
        return NULL;
    }

    // to select extractor;
    OmafExtractor *selectedExtractor = SelectExtractor(pStream, mPose);
    if(selectedExtractor && previousPose)
    {
        LOG(INFO)<<"pose has changed from ("<<previousPose->yaw<<","<<previousPose->pitch<<") to ("<<mPose->yaw<<","<<mPose->pitch<<") ! extractor id is: "<<selectedExtractor->GetID()<<endl;
#ifndef _ANDROID_NDK_OPTION_
        //trace
        tracepoint(mthq_tp_provider, T2_detect_pose_change, 1);
#endif
    }

    if(previousPose != mPose)
        SAFE_DELETE(previousPose);

    return selectedExtractor;
}

OmafExtractor* OmafExtractorSelector::SelectExtractor(OmafMediaStream* pStream, HeadPose* pose)
{
    // to select extractor;
    int ret = I360SCVP_setViewPort(m360ViewPortHandle, pose->yaw, pose->pitch);
    if(ret != 0)
        return NULL;
    ret = I360SCVP_process(mParamViewport, m360ViewPortHandle);
    if(ret != 0)
        return NULL;

    // get Content Coverage from 360SCVP library
    CCDef* outCC = new CCDef;
    ret = I360SCVP_getContentCoverage(m360ViewPortHandle, outCC);
    if(ret != 0)
        return NULL;

    // get the extractor with largest intersection
    OmafExtractor *selectedExtractor = GetNearestExtractor(pStream, outCC);

    if(outCC)
    {
        delete outCC;
        outCC = nullptr;
    }

    return selectedExtractor;
}

OmafExtractor* OmafExtractorSelector::GetNearestExtractor(OmafMediaStream* pStream, CCDef* outCC)
{
    // calculate which extractor should be chosen
    OmafExtractor *selectedExtractor = nullptr;
    float leastDistance = FLT_MAX;
    std::map<int, OmafExtractor*> extractors = pStream->GetExtractors();
    for(auto &ie: extractors)
    {
        ContentCoverage* cc = ie.second->GetContentCoverage();
        if(!cc)
            continue;

        int32_t ca = cc->coverage_infos[0].centre_azimuth;
        int32_t ce = cc->coverage_infos[0].centre_elevation;

        // for now, every extractor has the same azimuth_range and elevation_range
        // , so we just calculate least Euclidean distance between centres to find the
        // extractor with largest intersection
        float distance = sqrt( pow((ca - outCC->centreAzimuth), 2) + pow((ce - outCC->centreElevation), 2) );
        if(distance < leastDistance)
        {
            leastDistance = distance;
            selectedExtractor =  ie.second;
        }
    }

    return selectedExtractor;
}

ListExtractor OmafExtractorSelector::GetExtractorByPosePrediction( OmafMediaStream* pStream )
{
    ListExtractor extractors;
    pthread_mutex_lock(&mMutex);
    if(mPoseHistory.size() <= 1)
    {
        pthread_mutex_unlock(&mMutex);
        return extractors;
    }
    pthread_mutex_unlock(&mMutex);
    if (mPredictPluginMap.size() == 0)
    {
        LOG(INFO)<<"predict plugin map is empty!"<<endl;
        return extractors;
    }
    ViewportPredictPlugin *plugin = mPredictPluginMap.at(mPredictPluginName);
    uint32_t pose_interval = 40;
    uint32_t pre_pose_count = 25;
    uint32_t predict_interval = 1000;
    plugin->Intialize(pose_interval, pre_pose_count, predict_interval);
    std::list<ViewportAngle> pose_history;
    pthread_mutex_lock(&mMutex);
    for (auto it=mPoseHistory.begin(); it!=mPoseHistory.end(); it++)
    {
        ViewportAngle angle;
        angle.yaw = it->pose->yaw;
        angle.pitch = it->pose->pitch;
        pose_history.push_front(angle);
    }
    pthread_mutex_unlock(&mMutex);
    ViewportAngle* predict_angle = plugin->Predict(pose_history);
    if (predict_angle == NULL)
    {
        LOG(ERROR)<<"predictPose_func return an invalid value!"<<endl;
        return extractors;
    }
    HeadPose* previousPose = mPose;
    pthread_mutex_lock(&mMutex);
    mPose = mPoseHistory.front().pose;
    mPoseHistory.pop_front();
    if(!mPose)
    {
        pthread_mutex_unlock(&mMutex);
        return extractors;
    }
    pthread_mutex_unlock(&mMutex);
    // won't get viewport if pose hasn't changed
    if( previousPose && mPose && !IsDifferentPose( previousPose, mPose ) && pose_history.size() > 1)
    {
        LOG(INFO)<<"pose hasn't changed!"<<endl;
#ifndef _ANDROID_NDK_OPTION_
        //trace
        tracepoint(mthq_tp_provider, T2_detect_pose_change, 0);
#endif
        SAFE_DELETE(previousPose);
        SAFE_DELETE(predict_angle);
        return extractors;
    }
    // to select extractor;
    HeadPose *predictPose = new HeadPose;
    predictPose->yaw = predict_angle->yaw;
    predictPose->pitch = predict_angle->pitch;
    OmafExtractor *selectedExtractor = SelectExtractor(pStream, predictPose);
    if(selectedExtractor && previousPose)
    {
        extractors.push_back(selectedExtractor);
        LOG(INFO)<<"pose has changed from ("<<previousPose->yaw<<","<<previousPose->pitch<<") to ("<<mPose->yaw<<","<<mPose->pitch<<") ! extractor id is: "<<selectedExtractor->GetID()<<endl;
#ifndef _ANDROID_NDK_OPTION_
        //trace
        tracepoint(mthq_tp_provider, T2_detect_pose_change, 1);
#endif
    }
    SAFE_DELETE(previousPose);
    SAFE_DELETE(predictPose);
    SAFE_DELETE(predict_angle);
    return extractors;
}

int OmafExtractorSelector::InitializePredictPlugins()
{
    if (mLibPath.empty() || mPredictPluginName.empty())
    {
        LOG(ERROR)<<"Viewport predict plugin path OR name is invalid!"<<endl;
        return ERROR_INVALID;
    }
    ViewportPredictPlugin *plugin = new ViewportPredictPlugin();
    std::string pluginPath = mLibPath + mPredictPluginName;
    int ret = plugin->LoadPlugin(pluginPath.c_str());
    if (ret != ERROR_NONE)
    {
        LOG(ERROR)<<"Load plugin failed!"<<endl;
        return ret;
    }
    mPredictPluginMap.insert(std::pair<std::string, ViewportPredictPlugin*>(mPredictPluginName, plugin));
    return ERROR_NONE;
}

VCD_OMAF_END
