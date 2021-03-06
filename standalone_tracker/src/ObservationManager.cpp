/*
 * Software License Agreement (BSD License)
 * 
 * Copyright (c)  2012, Wongun Choi
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met: 
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer. 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution. 
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * The views and conclusions contained in the software and documentation are those
 * of the authors and should not be interpreted as representing official policies, 
 * either expressed or implied, of the FreeBSD Project.
 */

#include <stdio.h>
#include <iostream>
#include <observation/ObservationManager.h>
#include <common/util.h>
#ifdef HAVE_TBB
#include <tbb/tbb.h>
#include <tbb/tbb_stddef.h>
#endif

namespace people {
#ifdef HAVE_TBB
	struct PreprocessInvoker
	{
			PreprocessInvoker(std::vector<ObservationNode*> nodes) 
			{
				nodes_ = nodes;
			}
			
			void operator()( const tbb::blocked_range<int>& range ) const
			{
					int i, i1 = range.begin(), i2 = range.end();
					for( i = i1; i < i2; i++ )
						nodes_[i]->preprocess();
			}

			std::vector<ObservationNode*> nodes_;
	};
#endif

	/////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Observation Manager for Simplified Camera Model
	////////////////////////////////////////////////////////////////////////////////////////////////////////
	ObservationManager::ObservationManager()
	{
		min_height_ = 1.3;
		max_height_ = 2.3;
		total_weight_ = 1.0;

		obs_lkhood_out_of_height_ = -15.0; // heavily penalize too tall/small human
		feat_tracker_ = NULL;

		obj_type_ = g_objtype; // ObjPerson;

		mean_horizon_ = 0;
		std_horizon_ = 0;

		has_depth_ = false;
	}

	ObservationManager::~ObservationManager()
	{
		releaseNodes();
	}

	void ObservationManager::setObjType(ObjectType type) 
	{
		obj_type_ = type;
		for(size_t i = 0; i < nodes_.size(); i++)
			nodes_[i]->setObjType(type);
	}

	void ObservationManager::releaseNodes()
	{
		for(size_t i = 0; i < nodes_.size(); i++)
			delete nodes_[i];
		nodes_.clear();
	}

	void ObservationManager::setData(const void *data, const std::string &type)
	{
		// need the depth image to get 3d point from rect.
		if(type == "image_mono") img_mono_ = *(cv::Mat*)data;
		else if(type == "image_color") img_color_ = *(cv::Mat*)data;
		else if(type == "time_sec") time_sec_ = *(double*)data;
		else if(type == "feat_tracker") feat_tracker_ = (FeatTracker*)data;
		else if(type == "vp_estimate_file") assert(vp_est_.readPreprocessedFile(*(std::string*)data));

		std::vector<ObservationNode*>::iterator it;
		for(it = nodes_.begin(); it < nodes_.end(); it++)
			(*it)->setData(data, type);
	}

	void ObservationManager::setParameters(const std::string &name, const std::string &value)
	{
		if(name == "min_height") 				min_height_ = boost::lexical_cast<double>(value);
		else if(name == "max_height") 	max_height_ = boost::lexical_cast<double>(value);
		else if(name == "total_weight") total_weight_ = boost::lexical_cast<double>(value);
		else if(name == "feat_sigma_u")  gfeat_sigma_u_ = boost::lexical_cast<double>(value);
		else if(name == "feat_sigma_v")  gfeat_sigma_v_ = boost::lexical_cast<double>(value);
		else if(name == "mean_horizon")  mean_horizon_ = boost::lexical_cast<double>(value);
		else if(name == "std_horizon")  std_horizon_ = boost::lexical_cast<double>(value);

		std::vector<ObservationNode*>::iterator it;
		for(it = nodes_.begin(); it < nodes_.end(); it++)
			(*it)->setParameter(name, value);
	}

	ObservationNode* ObservationManager::getObservationNode(std::string &type)
	{
		std::vector<ObservationNode*>::iterator it;
		for(it = nodes_.begin(); it < nodes_.end(); it++)
			if(type == (*it)->getType()) return *it;
		return NULL;
	}

	void ObservationManager::insertObservationNode(ObservationNode* node)
	{
		nodes_.push_back(node);
	}

	void ObservationManager::preprocess()
	{
#ifdef HAVE_TBB
		tbb::parallel_for(tbb::blocked_range<int>(0, nodes_.size()), PreprocessInvoker(nodes_));
#else
		std::vector<ObservationNode*>::iterator it;
		for(it = nodes_.begin(); it < nodes_.end(); it++) {
			(*it)->preprocess();
		}
#endif
		assert(feat_tracker_ != NULL);

		feat_tracker_->setDetectorType("SURF");
		cv::Mat img_cropped(img_mono_, cv::Rect(0, floor(img_mono_.rows / 2), img_mono_.cols, floor(img_mono_.rows / 2)));
		feat_tracker_->setNewImage(img_cropped, time_sec_);
		feat_tracker_->processTracking();
	}

	// set ordered feats
	// and return the list of features to be deleted 
	std::vector<int> ObservationManager::preprocessFeats(const std::vector<int>& prev_feats_idx, const int max_feats, const std::vector<cv::Rect> &targets)
	{
		std::vector<int> deleted_feats;
		std::vector<int> current_feat_idx;
		std::vector<float> responses;
		std::vector<cv::Point2f> feat_pts;

		feat_tracker_->get_features(time_sec_, feat_pts, responses, current_feat_idx);
		for(size_t i = 0; i < feat_pts.size(); i++) {
			feat_pts[i].y += floor(img_mono_.rows / 2);
		}

		std::vector<cv::Rect> dets = getDetections();
		std::vector<cv::Rect> bbs;

		for(size_t i = 0; i < targets.size(); i++) {
			bbs.push_back(targets[i]);
			bbs.push_back(targets[i]);
		}
		for(size_t i = 0; i < dets.size(); i++) {
			bbs.push_back(dets[i]);
			bbs.push_back(dets[i]);
		}
		cv::groupRectangles(bbs, 1, 0.2);

		gfeats_.clear(); // clear all features;
		gfeats_idx_.clear(); // clear all features;
		// find matches
		int n = 0;
		for(size_t i = 0; i < prev_feats_idx.size(); i++) {
			std::vector<int>::iterator it;
			it = std::find(current_feat_idx.begin(), current_feat_idx.end(), prev_feats_idx[i]);
			// if found.. 
			if(*it == prev_feats_idx[i]) {
				// add the features.. 
				int idx = (int)(it - current_feat_idx.begin());
				float x = feat_pts[idx].x;
				float y = feat_pts[idx].y;

				if(!in_any_rect(bbs, cv::Point2f(x, y))) {
					cv::Point2f feat(x, y);

					gfeats_.push_back(feat);
					gfeats_idx_.push_back(*it);

					n++;
				}
				else {
					deleted_feats.push_back((int)i);
				}

				// remove already selected features.. 
				current_feat_idx.erase(current_feat_idx.begin() + idx);
				feat_pts.erase(feat_pts.begin() + idx);
				responses.erase(responses.begin() + idx);
			}
			else {
				deleted_feats.push_back((int)i);
			}
			if(n >= max_feats) break;
		}
		
		// add new features!!! 
		while(n < max_feats && current_feat_idx.size() > 0) {
			std::vector<float>::iterator it;
			it = std::max_element(responses.begin(), responses.end());
			int idx = (int)(it - responses.begin());
			// std::cout << "selecting " << idx << ":" << responses[idx] << "=" << *it << std::endl;
			float x = feat_pts[idx].x;
			float y = feat_pts[idx].y;
			if(!in_any_rect(bbs, cv::Point2f(x, y))) {
				cv::Point2f feat(x, y);
				gfeats_.push_back(feat);
				gfeats_idx_.push_back(current_feat_idx[idx]);
				n++;
			}
			// remove already selected features.. 
			current_feat_idx.erase(current_feat_idx.begin() + idx);
			feat_pts.erase(feat_pts.begin() + idx);
			responses.erase(responses.begin() + idx);
		}
		assert(current_feat_idx.size() == feat_pts.size());
		assert(responses.size() == feat_pts.size());

		return deleted_feats;
	}
	
	class CV_EXPORTS SimilarProposals
	{
	public:    
		SimilarProposals(double delta) : delta_sq_(delta * delta) {}
		inline bool operator()(const ObjectStatePtr p1, const ObjectStatePtr p2) const
		{
			return state_dist(p1, p2) < delta_sq_;
		}    
		double delta_sq_; 
	};   

	std::vector<cv::Rect> ObservationManager::getDetections()
	{
		std::vector<cv::Rect> ret;
		std::vector<cv::Rect> temp;

		std::vector<ObservationNode*>::iterator it;
		for(it = nodes_.begin(); it < nodes_.end(); it++) {
			temp = (*it)->getDetections();
			ret.insert(ret.end(), temp.begin(), temp.end());
		}
		// ret = std::vector<cv::Rect>();
		return ret;
	}

	void ObservationManager::quaryData(const std::string &name, void *data)
	{
		std::vector<ObservationNode*>::iterator it;
		for(it = nodes_.begin(); it < nodes_.end(); it++) {
			(*it)->quaryData(name, data);
		}
	}

	FeatureStatePtr ObservationManager::getInitialFeatureState(int idx, CameraStatePtr cam_state)
	{
		std::vector<int>::iterator it = std::find(gfeats_idx_.begin(), gfeats_idx_.end(), idx);
		assert(*it == idx);
		int i = (int)(it - gfeats_idx_.begin());
		cv::Point2f pt = gfeats_[i];
		FeatureStatePtr feat = cam_state->iproject(pt);
#if 0
		if(isnan(feat->x_) || isnan(feat->y_) || isnan(feat->z_) || 
		  isinf(feat->x_) || isinf(feat->y_) || isinf(feat->z_)) {
			cam_state->print();
			std::cout << "obs point u: " << ptd.x << " v: " << ptd.y << " d: " << ptd.z << std::endl;
			std::cout << "state point x: " << feat->x_ << " y: " << feat->y_ << " z: " << feat->z_ << std::endl;
			my_assert(0);
		}
#endif
		return feat;
	}

	double 	ObservationManager::getCameraConfidence(CameraStatePtr cam_state)
	{
		double ret = 0;
		if(cam_state->getStateType() == "simplified_camera") {
			// ret = vp_est_.getHorizonConfidence(cam_state->getHorizon());
			ret = vp_est_.getHorizonConfidence(cam_state->getElement(7));
			std::vector<int> votes;
			std::vector<double> std;
			getHorizonVotes(votes, std, cam_state->getElement(3));
			for(size_t i = 0; i < votes.size(); i++) {
				double diff = votes[i] - cam_state->getElement(7);
				ret -= min(pow(diff / std[i], 2), 9.0);
			}
			if(mean_horizon_ != 0) {
				ret -= pow((cam_state->getElement(7) - mean_horizon_) / std_horizon_, 2);
			}
		}
		else {
			// not implemented!
			assert(0);
		}
		return ret;
	}

	CameraStatePtr	ObservationManager::initializeCamera(CameraStatePtr cam_state)
	{
		if(cam_state->getStateType() == "simplified_camera") {
			CameraStatePtr temp = cam_state->clone();
			int imin = cam_state->getElement(7) - 200, imax = cam_state->getElement(7) + 200;
			double max_conf=-DBL_MAX;

			for(int i = imin; i < imax; i++) {
				temp->setElement(7, (double)i);
				double tval = getCameraConfidence(temp);
				if(max_conf < tval) {
					cam_state->setElement(7, (double)i);
					max_conf = tval;
				}
			}
		}
		else {
			// not implemented!
			assert(0);
		}

		return cam_state;
	}

	double	ObservationManager::getObjectConfidence(ObjectStatePtr obj_state, CameraStatePtr cam_state, std::string type)
	{
		double ret = 0;
		cv::Rect rt;

		// assuming y is height direction!!!
		if((obj_state->getElement(1) < min_height_) || (obj_state->getElement(1) > max_height_))
			return obs_lkhood_out_of_height_;

		// get image projection.
		rt = cam_state->project(obj_state);

		if(type == std::string("all")) {
			// iterate over all observation nodes_
			std::vector<ObservationNode*>::iterator it;
			for(it = nodes_.begin(); it < nodes_.end(); it++)
#if 1
				ret += (*it)->getConfidence(rt);
#else
				ret += soft_max((*it)->getConfidence(rt), 4.0f);
#endif
		}
		else {
			std::vector<ObservationNode*>::iterator it;
			for(it = nodes_.begin(); it < nodes_.end(); it++)
			{
				if((*it)->getType() == type)
#if 1
					ret += (*it)->getConfidence(rt);
#else
					ret += soft_max((*it)->getConfidence(rt), 4.0f);
#endif
			}
		}
		ret *= total_weight_;

		return ret;
	}

	double	ObservationManager::getFeatureConfidence(FeatureStatePtr feat_state, int feat_idx, CameraStatePtr cam_state, std::string type)
	{
		double ret;
		if(has_depth_)
		{
			assert(0);
		}
		else {
			cv::Point3f proj = cam_state->project(feat_state);
			cv::Point2f obs = gfeats_[feat_idx];
			
			ret = log_gaussian_prob(obs.x, proj.x, gfeat_sigma_u_);
			ret += log_gaussian_prob(obs.y, proj.y, gfeat_sigma_v_);
			// returning log(P(feat, valid | obs) / P(feat, invalid | obx))
			ret -= log_gaussian_prob(1.4 * gfeat_sigma_u_, 0.0, gfeat_sigma_u_)
					+ log_gaussian_prob(1.4 * gfeat_sigma_v_, 0.0, gfeat_sigma_v_);
		}
		// remove features with nan 
		if(isnan(ret)) return -100.0f;
		my_assert(!isnan(ret));

		return ret;
	}

	void ObservationManager::getHorizonVotes(std::vector<int> &votes, std::vector<double> &stds, double camh)
	{
		std::vector<cv::Rect> dets = getDetections();
		votes.clear();
		stds.clear();

		double mh, stdh;
		if(obj_type_ == ObjPerson) {
			mh = MEAN_PERSON_HEIGHT;
			stdh = STD_PERSON_HEIGHT;
		}
		else if(obj_type_ == ObjCar) {
			mh = MEAN_CAR_HEIGHT;
			stdh = STD_CAR_HEIGHT;
		}
		else {
			assert(0);
		}
		for(size_t i = 0; i < dets.size(); i++) {
			votes.push_back( dets[i].y + round((double)dets[i].height * (mh - camh) / mh) );
			stds.push_back((double)dets[i].height / mh * stdh);
		}
	}
#if 0
	cv::Mat	ObservationManager::getPeopleConfidenceMap(double y, CamStatePtr cam_state, std::string type)
	{
		cv::Mat ret(200, 200, CV_32FC1);
		double pt[3];

		PeopleStatePtr state = boost::make_shared<PeopleState>(PeopleState());
		for(int i = 0; i < 200; i++) {
			for(int j = 0; j < 200; j++) {
				double dx = (i - 100) * 0.1;
				double dz = j * 0.1;
				
				pt[0] = dx; pt[1] = 0; pt[2] = dz;
				// pt = pt0 + cam_state->getRot() * pt;
				pt[1] = y; // fix the height
				
				state->setX(pt[0]);
				state->setY(pt[1]);
				state->setZ(pt[2]);

				ret.at<float>(i, j) = (float)getPeopleConfidence(state, cam_state, type);
			}
		}

		return ret;
	}
#endif
}; // Namespace
