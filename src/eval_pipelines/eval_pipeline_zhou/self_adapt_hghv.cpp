#include "self_adapt_hghv.h"

#include <pcl/io/pcd_io.h>
#include <pcl/features/shot_omp.h>
#include <pcl/features/shot_lrf_omp.h>
#include <pcl/features/normal_3d_omp.h>
#include <pcl/features/integral_image_normal.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/correspondence.h>
#include <pcl/recognition/cg/hough_3d.h>
#include <pcl/recognition/cg/geometric_consistency.h>
#include <pcl/registration/icp.h>
#include <pcl/recognition/hv/hv_go.h>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/serialization/vector.hpp>
#include "../../implicit_shape_model/utils/utils.h"
#include "../../implicit_shape_model/utils/distance.h"


/**
 * Evaluation pipeline for the approach described in
 *
 * W. Zhou, C. Ma, A. Kuijper:
 *     Hough-space-based hypothesis generation and hypothesis verification for 3D
 *     object recognition and 6D pose estimation.
 *     2018, Computers & Graphics
 *
 */


SelfAdaptHGHV::SelfAdaptHGHV(std::string dataset, float bin, float th) :
    m_features(new pcl::PointCloud<ISMFeature>()),
    m_scene_keypoints(new pcl::PointCloud<PointT>()),
    m_scene_lrf(new pcl::PointCloud<pcl::ReferenceFrame>()),
    m_flann_index(flann::KDTreeIndexParams(4))
{
    std::cout << "-------- loading parameters for " << dataset << " dataset --------" << std::endl;

    if(dataset == "aim" || dataset == "mcgill" || dataset == "mcg" || dataset == "psb" ||
            dataset == "sh12" || dataset == "mn10" || dataset == "mn40")
    {
        /// classification
        // use this for datasets: aim, mcg, psb, shrec-12, mn10, mn40
        m_corr_threshold = -0.1;
        m_normal_radius = 0.05;
        m_reference_frame_radius = 0.3;
        m_feature_radius = 0.4;
        m_keypoint_sampling_radius = 0.2;
        m_k_search = 1; // TODO remove
        m_normal_method = 1;
        m_feature_type = "SHOT";
    }
    else if(dataset == "wash" || dataset == "bigbird" || dataset == "ycb")
    {
        /// classification
        m_corr_threshold = -0.5; // TODO VS check param
        m_normal_radius = 0.005;
        m_reference_frame_radius = 0.05;
        m_feature_radius = 0.05;
        m_keypoint_sampling_radius = 0.02;
        m_k_search = 1;
        m_normal_method = 0;
        m_feature_type = "CSHOT";
    }
    else if(dataset == "dataset1" || dataset == "dataset5")
    {
        /// detection
        m_corr_threshold = th;
        m_normal_radius = 0.005;
        m_reference_frame_radius = 0.05;
        m_feature_radius = 0.05;
        m_keypoint_sampling_radius = 0.02;
        m_k_search = 1;
        m_normal_method = 0;

        m_icp_max_iter = 100;
        m_icp_corr_distance = 0.05;

        if(dataset == "dataset1")
            m_feature_type = "SHOT";
        if(dataset == "dataset5")
            m_feature_type = "CSHOT";
    }
    else
    {
        std::cerr << "ERROR: dataset with name " << dataset << " not supported!" << std::endl;
    }
}


void SelfAdaptHGHV::train(const std::vector<std::string> &filenames,
                     const std::vector<unsigned> &class_labels,
                     const std::vector<unsigned> &instance_labels,
                     const std::string &output_file)
{
   if(filenames.size() != class_labels.size())
   {
       std::cerr << "ERROR: number of clouds does not match number of labels!" << std::endl;
       return;
   }

   // contains the whole list of features for each class id
   std::map<unsigned, pcl::PointCloud<ISMFeature>::Ptr> all_features;
   for(unsigned i = 0; i < class_labels.size(); i++)
   {
       unsigned int tr_class = class_labels.at(i);
       pcl::PointCloud<ISMFeature>::Ptr cloud(new pcl::PointCloud<ISMFeature>());
       all_features.insert({tr_class, cloud});
   }

   // contains the whole list of center vectors for each class id
   std::map<unsigned, std::vector<Eigen::Vector3f>> all_vectors;
   for(unsigned i = 0; i < class_labels.size(); i++)
   {
       unsigned int tr_class = class_labels.at(i);
       all_vectors.insert({tr_class, {}});
   }

   int num_features = 0;
   // process each input file
   for(int i = 0; i < filenames.size(); i++)
   {
       std::cout << "Processing file " << (i+1) << " of " << filenames.size() << std::endl;
       std::string file = filenames.at(i);
       unsigned int tr_class = class_labels.at(i);
       unsigned int tr_instance = instance_labels.at(i);

       // load cloud
       pcl::PointCloud<PointT>::Ptr cloud(new pcl::PointCloud<PointT>());
       if(pcl::io::loadPCDFile(file, *cloud) == -1)
       {
           std::cerr << "ERROR: loading file " << file << std::endl;
       }

       pcl::PointCloud<ISMFeature>::Ptr features_cleaned = processPointCloud(cloud);
       for(ISMFeature& ismf : features_cleaned->points)
       {
           // assign labels
           ismf.classId = tr_class;
           ismf.instanceId = tr_instance;
           // get activation position, relative to object center
           Eigen::Vector3f keyPos(ismf.x, ismf.y, ismf.z);
           Eigen::Vector4f centroid4f;
           pcl::compute3DCentroid(*cloud, centroid4f);
           Eigen::Vector3f centroid(centroid4f[0], centroid4f[1], centroid4f[2]);
           Eigen::Vector3f vote = centroid - keyPos;
           vote = Utils::rotateInto(vote, ismf.referenceFrame);
           all_vectors.at(tr_class).push_back(vote);
       }

       // add computed features to map
       (*all_features.at(tr_class)) += (*features_cleaned);
       num_features += features_cleaned->size();
   }

   std::cout << "Extracted " << num_features << " features." << std::endl;

   std::string save_file(output_file);
   if(saveModelToFile(save_file, all_features, all_vectors))
   {
       std::cout << "Hough3D training finished!" << std::endl;
   }
   else
   {
       std::cerr << "ERROR saving Hough3D model!" << std::endl;
   }
}


std::vector<std::pair<unsigned, float>> SelfAdaptHGHV::classify(const std::string &filename)
{
    pcl::PointCloud<PointT>::Ptr cloud(new pcl::PointCloud<PointT>());
    if(pcl::io::loadPCDFile<PointT>(filename, *cloud) == -1)
    {
        std::cerr << "ERROR: loading file " << filename << std::endl;
        return std::vector<std::pair<unsigned, float>>();
    }

    // extract features
    pcl::PointCloud<ISMFeature>::Ptr features = processPointCloud(cloud);

    // get results
    std::vector<std::pair<unsigned, float>> results;
    results = classifyObject(features);

    // here higher values are better
    std::sort(results.begin(), results.end(), [](const std::pair<unsigned, float> &a, const std::pair<unsigned, float> &b)
    {
        return a.second > b.second;
    });

    return results;
}


std::vector<std::pair<unsigned, float>> SelfAdaptHGHV::classifyObject(const pcl::PointCloud<ISMFeature>::Ptr& scene_features)
{
    // get model-scene correspondences
    // query index is scene, match index is codebook ("object")
    pcl::CorrespondencesPtr model_scene_corrs = findNnCorrespondences(scene_features);
    std::cout << "Found " << model_scene_corrs->size() << " correspondences" << std::endl;

    bool use_first_ransac = false;
    // NOTE: first RANSAC in the method of zhou et al. is used to eliminate false correspondences
    // they are checking object instances separately, however, here I implement a generic pipeline
    // this means: correspondences belong to multiple classes and are therefore ambiguous regarding the transformation
    // TODO rethink feature distance based ransac

    // TODO VS
    // run first RANSAC here to eliminate false correspondences - how exactly?
    // NOTE:
    // * find transformation while eliminating fp with ransac
    // * only keep inliers
    // --> this requires to have separate codebooks for matching, otherwise transformations won't make sense
    // alternative with a common codebook: matching threshold

    // TODO VS: own idea for filtering:
    // match knn with k = 3
    // if all are same class -> accept
    // if k1 and k2 are same class
    //       -> accept if valid distance ratio between k1 and k3
    // otherwise: distance ratio between k1 and k2 -> accept or discard
    // if k2 and k3 are same, but different from k1, accept k2 if distance ratio k1-k2 invalid?
    // if all are different
    //      -> accept if valid distance ratio between k1 and k2

    // object keypoints are simply the matched keypoints from the codebook
    pcl::PointCloud<PointT>::Ptr object_keypoints(new pcl::PointCloud<PointT>());
    pcl::PointCloud<ISMFeature>::Ptr object_features(new pcl::PointCloud<ISMFeature>());
    pcl::PointCloud<pcl::ReferenceFrame>::Ptr object_lrf(new pcl::PointCloud<pcl::ReferenceFrame>());
    // however in order not to pass the whole codebook, we need to ajust the index mapping
    for(unsigned i = 0; i < model_scene_corrs->size(); i++)
    {
        // create new list of keypoints and reassign the object (i.e. match) index
        pcl::Correspondence &corr = model_scene_corrs->at(i);
        int &index = corr.index_match;
        const ISMFeature &feat = m_features->at(index);
        object_features->push_back(feat);

        PointT keypoint = PointT(feat.x, feat.y, feat.z);
        index = object_keypoints->size(); // remap index to new position in the created point cloud
        object_keypoints->push_back(keypoint);

        pcl::ReferenceFrame lrf = feat.referenceFrame;
        object_lrf->push_back(lrf);
    }

    std::vector<double> maxima;
    std::vector<std::vector<int>> voteIndices;
    pcl::CorrespondencesPtr model_scene_corrs_filtered(new pcl::Correspondences());
    performSelfAdaptedHoughVoting(maxima, voteIndices, model_scene_corrs_filtered,
                                  model_scene_corrs, object_keypoints, object_features, object_lrf);

    // check all maxima since highest valued maximum might still be composed of different class votes
    // therefore we need to count votes per class per maximum
    std::vector<std::pair<unsigned, float>> results;
    results = std::move(getResultsFromMaxima(maxima, voteIndices, model_scene_corrs_filtered, object_features));
    return results;
}


void SelfAdaptHGHV::performSelfAdaptedHoughVoting(std::vector<double> &maxima,
                                   std::vector<std::vector<int>> &voteIndices,
                                   pcl::CorrespondencesPtr &model_scene_corrs_filtered,
                                   const pcl::CorrespondencesPtr &model_scene_corrs,
                                   const pcl::PointCloud<PointT>::Ptr object_keypoints,
                                   const pcl::PointCloud<ISMFeature>::Ptr object_features,
                                   const pcl::PointCloud<pcl::ReferenceFrame>::Ptr object_lrf)
{
    std::cout << "Total number of correspondences: " << model_scene_corrs->size() << std::endl;

    std::vector<double> filtered_maxima;
    std::vector<std::vector<int>> filtered_vote_indices;

    // self-adapt measure for number of correspondences after filtering
    float t_corr = 0.4;
    float ratio;
    do
    {
        // increase number of matches by increasing the threshold
        t_corr += 0.1;

        // apply feature distance threshold
        model_scene_corrs_filtered->clear();
        for(const pcl::Correspondence &corr : *model_scene_corrs)
        {
            if(corr.distance < t_corr)
                model_scene_corrs_filtered->push_back(corr);
        }

        std::cout << "Selecting " << model_scene_corrs_filtered->size() << " correspondences with threshold " << t_corr << std::endl;

        // prepare votes and hough space
        std::vector<std::pair<float,float>> votes; // first: rmse_E, second: rmse_T
        std::pair<float,float> rmse_E_min_max = {std::numeric_limits<float>::max(), std::numeric_limits<float>::min()};
        std::pair<float,float> rmse_T_min_max = {std::numeric_limits<float>::max(), std::numeric_limits<float>::min()};
        prepare_voting(votes, rmse_E_min_max, rmse_T_min_max,
                       model_scene_corrs_filtered, object_keypoints, object_features, object_lrf);

        // self-adapt measure for number of bins
        int h_n = 5; // number of bins per dimension
        while(h_n >= 3)
        {
            std::cout << "Constructing Houghspace with " << h_n << " bins per dimension" << std::endl;

            // construct hough space - degrade the 3rd dimension to represent 2d hough
            // size of dims
            float b_l = (rmse_E_min_max.second - rmse_E_min_max.first) / h_n;
            float b_w = (rmse_T_min_max.second - rmse_T_min_max.first) / h_n;

            m_min_coord = Eigen::Vector3d(0, 0, 0);
            m_max_coord = Eigen::Vector3d(rmse_E_min_max.second, rmse_T_min_max.second, 1);
            m_bin_size = Eigen::Vector3d(b_l,b_w,1);
            m_hough_space = std::make_shared<pcl::recognition::HoughSpace3D>(m_min_coord, m_bin_size, m_max_coord);

            // vote all votes into empty hough space
            m_hough_space->reset();
            maxima.clear();
            voteIndices.clear();
            for(int vid = 0; vid < votes.size(); vid++)
            {
                Eigen::Vector3d vote(votes[vid].first, votes[vid].second, 0);
                // voting with interpolation
                // votes are in the same order as correspondences, vid is therefore the index in the correspondence list
                m_hough_space->voteInt(vote, 1.0, vid);
            }
            // find maxima
            float m_relThreshold = 0.01f; // TODO VS tune threshold: must be member and set differently for classification / detection
            m_hough_space->findMaxima(-m_relThreshold, maxima, voteIndices);

            // only keep maxima with at least 3 votes
            filtered_maxima.clear();
            filtered_vote_indices.clear();
            for(int idx = 0; idx < maxima.size(); idx++)
            {
                if(voteIndices[idx].size() >= 3)
                {
                    filtered_maxima.push_back(maxima[idx]);
                    filtered_vote_indices.push_back(voteIndices[idx]);
                }
            }

            // if maxima are found, continue with next step
            if(filtered_maxima.size() > 0)
            {
                break;
            }
            h_n--; // reduce number of bins and search again
        }

        // if maxima are found, continue with next step
        if(filtered_maxima.size() > 0)
        {
            break;
        }

        float n = float(model_scene_corrs->size());
        float m = float(model_scene_corrs_filtered->size());
        ratio = m/n;
    }while(ratio < 0.5); // check if we have already selected at least 50% of all matches

    std::cout << "Found " << filtered_maxima.size() << " maxima" << std::endl;
    maxima = filtered_maxima;
    voteIndices = filtered_vote_indices;
}


void SelfAdaptHGHV::prepare_voting(
            std::vector<std::pair<float,float>> &votes,
            std::pair<float,float> &rmse_E_min_max,
            std::pair<float,float> &rmse_T_min_max,
            const pcl::CorrespondencesPtr &model_scene_corrs_filtered,
            const pcl::PointCloud<PointT>::Ptr object_keypoints,
            const pcl::PointCloud<ISMFeature>::Ptr object_features,
            const pcl::PointCloud<pcl::ReferenceFrame>::Ptr object_lrf) const
{
    std::vector<Eigen::Vector3f> rotations;
    std::vector<Eigen::Vector3f> translations;
    std::vector<float> matching_weights;
    float max_weight = 0;
    for(const pcl::Correspondence &corr : *model_scene_corrs_filtered)
    {
        // get rotation matrix
        pcl::ReferenceFrame lrf_scene = m_scene_lrf->at(corr.index_query);
        pcl::ReferenceFrame lrf_object = object_lrf->at(corr.index_match);
        Eigen::Matrix3f lrf_s = lrf_scene.getMatrix3fMap();
        Eigen::Matrix3f lrf_o = lrf_object.getMatrix3fMap();
        Eigen::Matrix3f R = lrf_s.transpose() * lrf_o;
        // get translation vector
        Eigen::Vector3f keypoint_scene = m_scene_keypoints->at(corr.index_query).getVector3fMap();
        Eigen::Vector3f keypoint_object = object_keypoints->at(corr.index_match).getVector3fMap();
        Eigen::Vector3f T = keypoint_scene - R * keypoint_object;
        // convert rotation to euler angles
        float phi, theta, psi;
        if(R(3,3) == 0)
        {
            phi = 0;
        }
        else
        {
            phi = std::atan(R(3,2) / R(3,3));
        }
        if(R(3,3) == 0 && R(3,2) == 0)
        {
            theta = 0;
        }
        else
        {
            theta = std::atan(-R(3,1) / std::sqrt(R(3,2)*R(3,2) + R(3,3)*R(3,3)));
        }
        if(R(1,1) == 0)
        {
            psi = 0;
        }
        else
        {
            psi = atan(R(2,1)/R(1,1));
        }
        // store translations and rotations
        rotations.push_back(Eigen::Vector3f(phi, theta, psi));
        translations.push_back(T);

        // compute descriptor distance as weight
        ISMFeature feat_scene = m_features->at(corr.index_query);
        ISMFeature feat_object = object_features->at(corr.index_match);
        DistanceEuclidean dist;
        float weight_raw = dist(feat_object.descriptor, feat_scene.descriptor);
        max_weight = weight_raw > max_weight ? weight_raw : max_weight;
        matching_weights.push_back(weight_raw);
    }

    // compute the center values and all weights from raw values that were saved
    Eigen::Vector3f E_c(0,0,0);
    Eigen::Vector3f T_c(0,0,0);
    float sum_weights = 0;
    for(unsigned i = 0; i < rotations.size(); i++)
    {
        float &raw_weight = matching_weights.at(i);
        raw_weight = 1 - (raw_weight / max_weight);
        sum_weights += raw_weight;
        E_c += raw_weight * rotations.at(i);
        T_c += raw_weight * translations.at(i);
    }
    // not in the paper, but shouldn't it be normalized by sum of weights?
    // NOTE: missing normalization does not affect results negatively
    //E_c /= sum_weights;
    //T_c /= sum_weights;

    // compute RMSEs as votes
    for(unsigned i = 0; i < rotations.size(); i++)
    {
        Eigen::Vector3f rot = rotations.at(i);
        float ex = rot.x()-E_c.x();
        float ey = rot.y()-E_c.y();
        float ez = rot.z()-E_c.z();
        float rmse_e_i = std::sqrt((ex*ex + ey*ey + ez*ez)/3);
        if(rmse_e_i < rmse_E_min_max.first)
            rmse_E_min_max.first = rmse_e_i;
        if(rmse_e_i > rmse_E_min_max.second)
            rmse_E_min_max.second = rmse_e_i;

        Eigen::Vector3f tr = translations.at(i);
        float tx = tr.x() - T_c.x();
        float ty = tr.y() - T_c.y();
        float tz = tr.z() - T_c.z();
        float rmse_t_i = std::sqrt((tx*tx + ty*ty + tz*tz)/3);
        if(rmse_t_i < rmse_T_min_max.first)
            rmse_T_min_max.first = rmse_t_i;
        if(rmse_t_i > rmse_T_min_max.second)
            rmse_T_min_max.second = rmse_t_i;

        votes.push_back({rmse_e_i, rmse_t_i});
    }
}


std::vector<pcl::Correspondences> SelfAdaptHGHV::getCorrespondeceClustersFromMaxima(
        const std::vector<double> &maxima,
        const std::vector<std::vector<int>> &voteIndices,
        const pcl::CorrespondencesPtr &model_scene_corrs_filtered) const
{
    std::vector<pcl::Correspondences> clustered_corrs;
    for (size_t j = 0; j < maxima.size (); ++j) // loop over all maxima
    {
        pcl::Correspondences max_corrs;
        for (size_t i = 0; i < voteIndices[j].size(); ++i)
        {
            max_corrs.push_back(model_scene_corrs_filtered->at(voteIndices[j][i]));
        }
        clustered_corrs.push_back(max_corrs);
    }
    return clustered_corrs;
}


std::vector<std::pair<unsigned, float>> SelfAdaptHGHV::getResultsFromMaxima(
        const std::vector<double> &maxima,
        const std::vector<std::vector<int>> &voteIndices,
        const pcl::CorrespondencesPtr &model_scene_corrs_filtered,
        const pcl::PointCloud<ISMFeature>::Ptr object_features) const
{
    std::vector<std::pair<unsigned, float>> results;
    for (size_t j = 0; j < maxima.size (); ++j) // loop over all maxima
    {
        std::map<unsigned, int> class_occurences;
        pcl::Correspondences max_corrs;
        for (size_t i = 0; i < voteIndices[j].size(); ++i)
        {
            max_corrs.push_back(model_scene_corrs_filtered->at(voteIndices[j][i]));
        }

        // count class occurences in filtered corrs
        for(unsigned fcorr_idx = 0; fcorr_idx < max_corrs.size(); fcorr_idx++) // loop over all correspondences per max
        {
            // match_idx refers to the modified list, not the original codebook!!!
            unsigned match_idx = max_corrs.at(fcorr_idx).index_match;
            const ISMFeature &cur_feat = object_features->at(match_idx);
            unsigned class_id = cur_feat.classId;
            // add occurence of this class_id
            if(class_occurences.find(class_id) != class_occurences.end())
            {
                class_occurences.at(class_id) += 1;
            }
            else
            {
                class_occurences.insert({class_id, 1});
            }
        }

        // determine most frequent label
        unsigned cur_class = 0;
        int cur_best_num = 0;
        for(auto occ_elem : class_occurences)
        {
            if(occ_elem.second > cur_best_num)
            {
                cur_best_num = occ_elem.second;
                cur_class = occ_elem.first;
            }
        }
        results.push_back({cur_class, cur_best_num});
    }
    return results;
}


std::vector<VotingMaximum> SelfAdaptHGHV::detect(const std::string &filename)
{
    pcl::PointCloud<PointT>::Ptr cloud(new pcl::PointCloud<PointT>());
    if(pcl::io::loadPCDFile<PointT>(filename, *cloud) == -1)
    {
      std::cerr << "ERROR: loading file " << filename << std::endl;
      return std::vector<VotingMaximum>();
    }

    // extract features
    pcl::PointCloud<ISMFeature>::Ptr features = processPointCloud(cloud);

    std::vector<VotingMaximum> result_maxima;

    // get results
    std::vector<std::pair<unsigned, float>> results;
    std::vector<Eigen::Vector3f> positions;
    std::tie(results, positions) = findObjects(features, cloud);

    // here higher values are better
    std::sort(results.begin(), results.end(), [](const std::pair<unsigned, float> &a, const std::pair<unsigned, float> &b)
    {
        return a.second > b.second;
    });

    // convert result to maxima
    for(unsigned i = 0; i < results.size(); i++)
    {
        auto res = results.at(i);
        VotingMaximum max;
        max.classId = res.first;
        max.globalHypothesis.classId = res.first;
        max.weight = res.second;
        max.position = positions.at(i);
        result_maxima.emplace_back(std::move(max));
    }

    return result_maxima;
}


std::tuple<std::vector<std::pair<unsigned, float>>,std::vector<Eigen::Vector3f>>
SelfAdaptHGHV::findObjects(const pcl::PointCloud<ISMFeature>::Ptr& scene_features,
                           const pcl::PointCloud<PointT>::Ptr scene_cloud)
{
    // get model-scene correspondences
    // query index is scene, match index is codebook ("object")
    pcl::CorrespondencesPtr object_scene_corrs = findNnCorrespondences(scene_features);
    std::cout << "Found " << object_scene_corrs->size() << " correspondences" << std::endl;

    // TODO VS
    // run first RANSAC here to eliminate false correspondences - how exactly?
    // NOTE:
    // * find transformation while eliminating fp with ransac
    // * only keep inliers
    // --> this requires to have separate codebooks for matching, otherwise transformations won't make sense
    // alternative with a common codebook: matching threshold

    // TODO VS: own idea for filtering:
    // match knn with k = 3
    // if all are same class -> accept
    // if k1 and k2 are same class
    //       -> accept if valid distance ratio between k1 and k3
    // otherwise: distance ratio between k1 and k2 -> accept or discard
    // if k2 and k3 are same, but different from k1, accept k2 if distance ratio k1-k2 invalid?
    // if all are different
    //      -> accept if valid distance ratio between k1 and k2

    // object keypoints are simply the matched keypoints from the codebook
    pcl::PointCloud<PointT>::Ptr object_keypoints(new pcl::PointCloud<PointT>());
    pcl::PointCloud<ISMFeature>::Ptr object_features(new pcl::PointCloud<ISMFeature>());
    pcl::PointCloud<pcl::ReferenceFrame>::Ptr object_lrf(new pcl::PointCloud<pcl::ReferenceFrame>());
    // however in order not to pass the whole codebook, we need to adjust the index mapping
    for(unsigned i = 0; i < object_scene_corrs->size(); i++)
    {
        // create new list of keypoints and reassign the object (i.e. match) index
        pcl::Correspondence &corr = object_scene_corrs->at(i);
        int &index = corr.index_match;
        const ISMFeature &feat = m_features->at(index);
        object_features->push_back(feat);

        PointT keypoint = PointT(feat.x, feat.y, feat.z);
        index = object_keypoints->size(); // remap index to new position in the created point cloud
        object_keypoints->push_back(keypoint);

        pcl::ReferenceFrame lrf = feat.referenceFrame;
        object_lrf->push_back(lrf);
    }

    std::vector<double> maxima;
    std::vector<std::vector<int>> voteIndices;
    pcl::CorrespondencesPtr model_scene_corrs_filtered(new pcl::Correspondences());
    performSelfAdaptedHoughVoting(maxima, voteIndices, model_scene_corrs_filtered,
                                  object_scene_corrs, object_keypoints, object_features, object_lrf);

    // get clusters from each maximum, note that correspondences might still be of different classes
    std::vector<pcl::Correspondences> clustered_corrs;
    clustered_corrs = std::move(getCorrespondeceClustersFromMaxima(maxima, voteIndices, model_scene_corrs_filtered));

    // generate 6DOF hypotheses from clusters
    std::vector<std::pair<unsigned, float>> results;
    std::vector<Eigen::Vector3f> positions;
    std::vector<Eigen::Matrix4f> transformations;
    std::vector<pcl::Correspondences> filtered_clustered_corrs;
    pcl::registration::CorrespondenceRejectorSampleConsensus<PointT> corr_rejector;
    corr_rejector.setMaximumIterations(10000);
    corr_rejector.setInlierThreshold(m_bin_size(0));
    corr_rejector.setInputSource(m_scene_keypoints);
    corr_rejector.setInputTarget(object_keypoints); // idea: use keypoints of features matched in the codebook
    //corr_rejector.setRefineModel(true); // TODO VS verify: slightly worse results

    // second RANSAC: filter correspondence groups by position
    for(const pcl::Correspondences &temp_corrs : clustered_corrs)
    {
        pcl::Correspondences filtered_corrs;
        corr_rejector.getRemainingCorrespondences(temp_corrs, filtered_corrs);
        Eigen::Matrix4f best_transform = corr_rejector.getBestTransformation();

        // TODO VS check if best_transform is computed with "absolute orientation"
        // check code of corr_rejector --> confirmed it is using absolute orientation!!!

        // TODO VS check for background knowledge:
        // https://www.ais.uni-bonn.de/papers/RAM_2015_Holz_PCL_Registration_Tutorial.pdf
        // https://www.programmersought.com/article/93804217152/

        // TODO VS check if this if clause works and makes sense
        if(!best_transform.isIdentity(0.01))
        {
            // keep transformation and correspondences if RANSAC was run successfully
            transformations.push_back(best_transform);
            filtered_clustered_corrs.push_back(filtered_corrs);

            // TODO VS: the following is probably not needed at this point
            unsigned res_class;
            int res_num_votes;
            Eigen::Vector3f res_position;
            findClassAndPositionFromCluster(filtered_corrs, m_features, scene_features,
                                            res_class, res_num_votes, res_position);
            results.push_back({res_class, res_num_votes});
            positions.push_back(res_position);
        }
    }

//    // ------ this is where the aldoma contibution starts -------
//    std::vector<std::pair<unsigned, float>> results;
//    std::vector<Eigen::Vector3f> positions;

//    // Stop if no instances
//    if (rototranslations.size () <= 0)
//    {
//        std::cout << "No instances found!" << std::endl;
//        return std::make_tuple(results, positions);
//    }
//    else
//    {
//        std::cout << "Resulting clusters: " << rototranslations.size() << std::endl;
//    }

//    if(use_global_hv) // aldoma global hypotheses verification
//    {
//        // Generates clouds for each instances found
//        std::vector<pcl::PointCloud<PointT>::ConstPtr> instances;
//        for(size_t i = 0; i < rototranslations.size (); ++i)
//        {
//            pcl::PointCloud<PointT>::Ptr rotated_model(new pcl::PointCloud<PointT>());
//            // NOTE: object_keypoints might contain multiple objects, so use only keypoints of a single cluster
//            //pcl::transformPointCloud(*object_keypoints, *rotated_model, rototranslations[i]);
//            pcl::PointCloud<PointT>::Ptr cluster_keypoints(new pcl::PointCloud<PointT>());
//            pcl::Correspondences this_cluster = clustered_corrs[i];
//            for(const pcl::Correspondence &corr : this_cluster)
//            {
//                const ISMFeature &feat = object_features->at(corr.index_match);
//                PointT keypoint = PointT(feat.x, feat.y, feat.z);
//                cluster_keypoints->push_back(keypoint);
//            }
//            pcl::transformPointCloud(*cluster_keypoints, *rotated_model, rototranslations[i]);
//            instances.push_back(rotated_model);
//        }

//        // ICP
//        std::cout << "--- ICP ---------" << std::endl;
//        std::vector<pcl::PointCloud<PointT>::ConstPtr> registered_instances;
//        for (size_t i = 0; i < rototranslations.size (); ++i)
//        {
//            pcl::IterativeClosestPoint<PointT, PointT> icp;
//            icp.setMaximumIterations(m_icp_max_iter);
//            icp.setMaxCorrespondenceDistance(m_icp_corr_distance);
//            icp.setInputTarget(m_scene_keypoints); // TODO VS scene keypoints or scene cloud?? --> keypoints slightly better in first test
//            icp.setInputSource(instances[i]);
//            pcl::PointCloud<PointT>::Ptr registered (new pcl::PointCloud<PointT>);
//            icp.align(*registered);
//            registered_instances.push_back(registered);
////            std::cout << "Instance " << i << " ";
////            if (icp.hasConverged ())
////            {
////                std::cout << "Aligned!" << std::endl;
////            }
////            else
////            {
////                std::cout << "Not Aligned!" << std::endl;
////            }
//        }

//        // Hypothesis Verification
//        std::cout << "--- Hypotheses Verification ---" << std::endl;
//        // TODO VS: tune thresholds
//        std::vector<bool> hypotheses_mask;  // Mask Vector to identify positive hypotheses
//        pcl::GlobalHypothesesVerification<PointT, PointT> GoHv;
//        GoHv.setSceneCloud(scene_cloud);  // Scene Cloud
//        GoHv.addModels(registered_instances, true);  //Models to verify
//        GoHv.setInlierThreshold(0.01);
//        GoHv.setOcclusionThreshold(0.02);
//        GoHv.setRegularizer(3.0);
//        GoHv.setClutterRegularizer(5.0);
//        GoHv.setRadiusClutter(0.25);
//        GoHv.setDetectClutter(true);
//        GoHv.setRadiusNormals(m_normal_radius);
//        GoHv.verify();
//        GoHv.getMask(hypotheses_mask);  // i-element TRUE if hvModels[i] verifies hypotheses

//        for (int i = 0; i < hypotheses_mask.size (); i++)
//        {
//            if(hypotheses_mask[i])
//            {
//                std::cout << "Instance " << i << " is GOOD!" << std::endl;
//                pcl::Correspondences filtered_corrs = clustered_corrs[i];
//                unsigned res_class;
//                int res_num_votes;
//                Eigen::Vector3f res_position;
//                findClassAndPositionFromCluster(filtered_corrs, object_features, scene_features,
//                                                res_class, res_num_votes, res_position);

//                // find aligned position
//                pcl::PointCloud<PointT>::ConstPtr reg_inst = registered_instances[i];
//                Eigen::Vector4f centroid4f;
//                pcl::compute3DCentroid(*reg_inst, centroid4f);

//                // store results
//                results.push_back({res_class, res_num_votes});
//                // TODO VS: which position? transformed centroid or position from cluster (the latter was better in first test)
//                //positions.emplace_back(Eigen::Vector3f(centroid4f.x(), centroid4f.y(), centroid4f.z()));
//                positions.push_back(res_position);
//            }
//            else
//            {
//                //std::cout << "Instance " << i << " is bad, discarding!" << std::endl;
//            }
//        }
//    }
//    // NOTE: else branch is just for verification and should not be used normally
//    // NOTE: if above use_hough == true, this else branch should correspond to the
//    //       tombari pipeline (i.e. executable "eval_pipeline_tombari_detection")
//    else
//    {
//        for (size_t j = 0; j < clustered_corrs.size (); ++j) // loop over all maxima
//        {
//            pcl::Correspondences filtered_corrs = clustered_corrs[j];
//            unsigned res_class;
//            int res_num_votes;
//            Eigen::Vector3f res_position;
//            findClassAndPositionFromCluster(filtered_corrs, object_features, scene_features,
//                                            res_class, res_num_votes, res_position);
//            results.push_back({res_class, res_num_votes});
//            positions.push_back(res_position);
//        }
//    }

    return std::make_tuple(results, positions);
}


void SelfAdaptHGHV::findClassAndPositionFromCluster(
        const pcl::Correspondences &filtered_corrs,
        const pcl::PointCloud<ISMFeature>::Ptr object_features,
        const pcl::PointCloud<ISMFeature>::Ptr scene_features,
        unsigned &resulting_class,
        int &resulting_num_votes,
        Eigen::Vector3f &resulting_position) const
{
    // determine position based on filtered correspondences for each remaining class
    // (ideally, only one class remains after filtering)
    std::vector<Eigen::Vector3f> all_centers(m_number_of_classes);
    std::vector<int> num_votes(m_number_of_classes);
    // init
    for(unsigned temp_idx = 0; temp_idx < all_centers.size(); temp_idx++)
    {
        all_centers[temp_idx] = Eigen::Vector3f(0.0f,0.0f,0.0f);
        num_votes[temp_idx] = 0;
    }
    // compute
    for(unsigned fcorr_idx = 0; fcorr_idx < filtered_corrs.size(); fcorr_idx++)
    {
        // match_idx refers to the modified list, not the original codebook!!!
        unsigned match_idx = filtered_corrs.at(fcorr_idx).index_match;
        // const ISMFeature &cur_feat = m_features->at(match_idx);
        const ISMFeature &cur_feat = object_features->at(match_idx);
        unsigned class_id = cur_feat.classId;

        unsigned scene_idx = filtered_corrs.at(fcorr_idx).index_query;
        const ISMFeature &scene_feat = scene_features->at(scene_idx);
        pcl::ReferenceFrame ref = scene_feat.referenceFrame;
        Eigen::Vector3f keyPos(scene_feat.x, scene_feat.y, scene_feat.z);
        Eigen::Vector3f vote = m_center_vectors.at(match_idx);
        Eigen::Vector3f pos = keyPos + Utils::rotateBack(vote, ref);
        all_centers[class_id] += pos;
        num_votes[class_id] += 1;
    }
    for(unsigned temp_idx = 0; temp_idx < all_centers.size(); temp_idx++)
    {
        all_centers[temp_idx] /= num_votes[temp_idx];
    }
    // find class with max votes
    unsigned cur_class = 0;
    int cur_best_num = 0;
    for(unsigned class_idx = 0; class_idx < num_votes.size(); class_idx++)
    {
        if(num_votes[class_idx] > cur_best_num)
        {
            cur_best_num = num_votes[class_idx];
            cur_class = class_idx;
        }
    }

    // fill in results
    resulting_class = cur_class;
    resulting_num_votes = cur_best_num;
    resulting_position = all_centers[cur_class];
}


bool SelfAdaptHGHV::loadModel(std::string &filename)
{
    if(!loadModelFromFile(filename)) return false;

    flann::Matrix<float> dataset = createFlannDataset();

    m_flann_index = flann::Index<flann::L2<float>>(dataset, flann::KDTreeIndexParams(4));
    m_flann_index.buildIndex();

    return true;
}


pcl::PointCloud<ISMFeature>::Ptr SelfAdaptHGHV::processPointCloud(pcl::PointCloud<PointT>::Ptr cloud)
{
    // create search tree
    pcl::search::Search<PointT>::Ptr searchTree;
    searchTree = pcl::search::KdTree<PointT>::Ptr(new pcl::search::KdTree<PointT>());

    // compute normals
    pcl::PointCloud<pcl::Normal>::Ptr normals;
    computeNormals(cloud, normals, searchTree);

    // filter normals
    pcl::PointCloud<pcl::Normal>::Ptr normals_without_nan;
    pcl::PointCloud<PointT>::Ptr cloud_without_nan;
    filterNormals(normals, normals_without_nan, cloud, cloud_without_nan);

    // compute keypoints
    pcl::PointCloud<PointT>::Ptr keypoints;
    computeKeypoints(keypoints, cloud_without_nan);
    m_scene_keypoints = keypoints;

    // compute reference frames
    pcl::PointCloud<pcl::ReferenceFrame>::Ptr reference_frames;
    computeReferenceFrames(reference_frames, keypoints, cloud_without_nan, searchTree);
    m_scene_lrf = reference_frames;

    // sort out invalid reference frames and associated keypoints
    pcl::PointCloud<pcl::ReferenceFrame>::Ptr cleanReferenceFrames(new pcl::PointCloud<pcl::ReferenceFrame>());
    pcl::PointCloud<PointT>::Ptr cleanKeypoints(new pcl::PointCloud<PointT>());
    unsigned missedFrames = 0;
    for (int i = 0; i < (int)reference_frames->size(); i++) {
        const pcl::ReferenceFrame& frame = reference_frames->at(i);
        if (std::isfinite (frame.x_axis[0]) &&
                std::isfinite (frame.y_axis[0]) &&
                std::isfinite (frame.z_axis[0])) {
            cleanReferenceFrames->push_back(frame);
            cleanKeypoints->push_back(keypoints->at(i));
        }
        else
            missedFrames++;
    }

    // compute descriptors
    pcl::PointCloud<ISMFeature>::Ptr features;
    computeDescriptors(cloud_without_nan, normals_without_nan, cleanKeypoints, searchTree, cleanReferenceFrames, features);


    // store keypoint positions and reference frames
    for (int i = 0; i < (int)features->size(); i++)
    {
        ISMFeature& feature = features->at(i);
        const PointT& keypoint = cleanKeypoints->at(i);
        feature.x = keypoint.x;
        feature.y = keypoint.y;
        feature.z = keypoint.z;
        feature.referenceFrame = cleanReferenceFrames->at(i);
    }

    // remove NAN features
    pcl::PointCloud<ISMFeature>::Ptr features_cleaned;
    removeNanDescriptors(features, features_cleaned);

    return features_cleaned;
}


void SelfAdaptHGHV::computeNormals(pcl::PointCloud<PointT>::Ptr cloud,
                           pcl::PointCloud<pcl::Normal>::Ptr& normals,
                           pcl::search::Search<PointT>::Ptr searchTree) const
{
    normals = pcl::PointCloud<pcl::Normal>::Ptr(new pcl::PointCloud<pcl::Normal>());

    if(m_normal_method == 0)
    {
         // prepare PCL normal estimation object
        pcl::NormalEstimationOMP<PointT, pcl::Normal> normalEst;
        normalEst.setInputCloud(cloud);
        normalEst.setNumberOfThreads(0);
        normalEst.setSearchMethod(searchTree);
        normalEst.setRadiusSearch(m_normal_radius);
        normalEst.setViewPoint(0,0,0);
        normalEst.compute(*normals);
    }
    else
    {
        // prepare PCL normal estimation object
        pcl::NormalEstimationOMP<PointT, pcl::Normal> normalEst;
        normalEst.setInputCloud(cloud);
        normalEst.setSearchMethod(searchTree);
        normalEst.setRadiusSearch(m_normal_radius);

        // move model to origin, then point normals away from origin
        pcl::PointCloud<PointT>::Ptr model_no_centroid(new pcl::PointCloud<PointT>());
        pcl::copyPointCloud(*cloud, *model_no_centroid);

        // compute the object centroid
        Eigen::Vector4f centroid4f;
        pcl::compute3DCentroid(*model_no_centroid, centroid4f);
        Eigen::Vector3f centroid(centroid4f[0], centroid4f[1], centroid4f[2]);
        // remove centroid for normal computation
        for(PointT& point : model_no_centroid->points)
        {
            point.x -= centroid.x();
            point.y -= centroid.y();
            point.z -= centroid.z();
        }
        normalEst.setInputCloud(model_no_centroid);
        normalEst.setViewPoint(0,0,0);
        normalEst.compute(*normals);
        // invert normals
        for(pcl::Normal& norm : normals->points)
        {
            norm.normal_x *= -1;
            norm.normal_y *= -1;
            norm.normal_z *= -1;
        }
    }
}

void SelfAdaptHGHV::filterNormals(pcl::PointCloud<pcl::Normal>::Ptr &normals,
                          pcl::PointCloud<pcl::Normal>::Ptr &normals_without_nan,
                          pcl::PointCloud<PointT>::Ptr &cloud,
                          pcl::PointCloud<PointT>::Ptr &cloud_without_nan) const
{
    std::vector<int> mapping;
    normals_without_nan = pcl::PointCloud<pcl::Normal>::Ptr(new pcl::PointCloud<pcl::Normal>());
    pcl::removeNaNNormalsFromPointCloud(*normals, *normals_without_nan, mapping);

    // create new point cloud without NaN normals
    cloud_without_nan = pcl::PointCloud<PointT>::Ptr(new pcl::PointCloud<PointT>());
    for (int i = 0; i < (int)mapping.size(); i++)
    {
        cloud_without_nan->push_back(cloud->at(mapping[i]));
    }
}


void SelfAdaptHGHV::computeKeypoints(pcl::PointCloud<PointT>::Ptr &keypoints, pcl::PointCloud<PointT>::Ptr &cloud) const
{
    pcl::VoxelGrid<PointT> voxelGrid;
    voxelGrid.setInputCloud(cloud);
    voxelGrid.setLeafSize(m_keypoint_sampling_radius, m_keypoint_sampling_radius, m_keypoint_sampling_radius);
    keypoints = pcl::PointCloud<PointT>::Ptr(new pcl::PointCloud<PointT>());
    voxelGrid.filter(*keypoints);
}


void SelfAdaptHGHV::computeReferenceFrames(pcl::PointCloud<pcl::ReferenceFrame>::Ptr &reference_frames,
                                   pcl::PointCloud<PointT>::Ptr &keypoints,
                                   pcl::PointCloud<PointT>::Ptr &cloud,
                                   pcl::search::Search<PointT>::Ptr &searchTree) const
{
    reference_frames = pcl::PointCloud<pcl::ReferenceFrame>::Ptr(new pcl::PointCloud<pcl::ReferenceFrame>());
    pcl::SHOTLocalReferenceFrameEstimationOMP<PointT, pcl::ReferenceFrame> refEst;
    refEst.setRadiusSearch(m_reference_frame_radius);
    refEst.setInputCloud(keypoints);
    refEst.setSearchSurface(cloud);
    refEst.setSearchMethod(searchTree);
    refEst.compute(*reference_frames);

    pcl::PointCloud<pcl::ReferenceFrame>::Ptr cleanReferenceFrames(new pcl::PointCloud<pcl::ReferenceFrame>());
    pcl::PointCloud<PointT>::Ptr cleanKeypoints(new pcl::PointCloud<PointT>());
    for(int i = 0; i < (int)reference_frames->size(); i++)
    {
        const pcl::ReferenceFrame& frame = reference_frames->at(i);
        if(std::isfinite(frame.x_axis[0]) && std::isfinite(frame.y_axis[0]) && std::isfinite(frame.z_axis[0]))
        {
            cleanReferenceFrames->push_back(frame);
            cleanKeypoints->push_back(keypoints->at(i));
        }
    }

    keypoints = cleanKeypoints;
    reference_frames = cleanReferenceFrames;
}


void SelfAdaptHGHV::computeDescriptors(pcl::PointCloud<PointT>::Ptr &cloud,
                               pcl::PointCloud<pcl::Normal>::Ptr &normals,
                               pcl::PointCloud<PointT>::Ptr &keypoints,
                               pcl::search::Search<PointT>::Ptr &searchTree,
                               pcl::PointCloud<pcl::ReferenceFrame>::Ptr &reference_frames,
                               pcl::PointCloud<ISMFeature>::Ptr &features) const
{
    if(m_feature_type == "SHOT")
    {
        pcl::SHOTEstimationOMP<PointT, pcl::Normal, pcl::SHOT352> shotEst;
        shotEst.setSearchSurface(cloud);
        shotEst.setInputNormals(normals);
        shotEst.setInputCloud(keypoints);
        shotEst.setInputReferenceFrames(reference_frames);
        shotEst.setSearchMethod(searchTree);
        shotEst.setRadiusSearch(m_feature_radius);
        pcl::PointCloud<pcl::SHOT352>::Ptr shot_features(new pcl::PointCloud<pcl::SHOT352>());
        shotEst.compute(*shot_features);

        // create descriptor point cloud
        features = pcl::PointCloud<ISMFeature>::Ptr(new pcl::PointCloud<ISMFeature>());
        features->resize(shot_features->size());

        for (int i = 0; i < (int)shot_features->size(); i++)
        {
            ISMFeature& feature = features->at(i);
            const pcl::SHOT352& shot = shot_features->at(i);

            // store the descriptor
            feature.descriptor.resize(352);
            for (int j = 0; j < feature.descriptor.size(); j++)
                feature.descriptor[j] = shot.descriptor[j];
        }
    }
    else if(m_feature_type == "CSHOT")
    {
        pcl::SHOTColorEstimationOMP<PointT, pcl::Normal, pcl::SHOT1344> shotEst;

        // temporary workaround to fix race conditions in OMP version of CSHOT in PCL
        if (shotEst.sRGB_LUT[0] < 0)
        {
          for (int i = 0; i < 256; i++)
          {
            float f = static_cast<float> (i) / 255.0f;
            if (f > 0.04045)
              shotEst.sRGB_LUT[i] = powf ((f + 0.055f) / 1.055f, 2.4f);
            else
              shotEst.sRGB_LUT[i] = f / 12.92f;
          }

          for (int i = 0; i < 4000; i++)
          {
            float f = static_cast<float> (i) / 4000.0f;
            if (f > 0.008856)
              shotEst.sXYZ_LUT[i] = static_cast<float> (powf (f, 0.3333f));
            else
              shotEst.sXYZ_LUT[i] = static_cast<float>((7.787 * f) + (16.0 / 116.0));
          }
        }

        shotEst.setSearchSurface(cloud);
        shotEst.setInputNormals(normals);
        shotEst.setInputCloud(keypoints);
        shotEst.setInputReferenceFrames(reference_frames);
        shotEst.setSearchMethod(searchTree);
        shotEst.setRadiusSearch(m_feature_radius);
        pcl::PointCloud<pcl::SHOT1344>::Ptr shot_features(new pcl::PointCloud<pcl::SHOT1344>());
        shotEst.compute(*shot_features);

        // create descriptor point cloud
        features = pcl::PointCloud<ISMFeature>::Ptr(new pcl::PointCloud<ISMFeature>());
        features->resize(shot_features->size());

        for (int i = 0; i < (int)shot_features->size(); i++)
        {
            ISMFeature& feature = features->at(i);
            const pcl::SHOT1344& shot = shot_features->at(i);

            // store the descriptor
            feature.descriptor.resize(1344);
            for (int j = 0; j < feature.descriptor.size(); j++)
                feature.descriptor[j] = shot.descriptor[j];
        }
    }
}


void SelfAdaptHGHV::removeNanDescriptors(pcl::PointCloud<ISMFeature>::Ptr &features,
                                 pcl::PointCloud<ISMFeature>::Ptr &features_cleaned) const
{
    features_cleaned = pcl::PointCloud<ISMFeature>::Ptr(new pcl::PointCloud<ISMFeature>());
    features_cleaned->header = features->header;
    features_cleaned->height = 1;
    features_cleaned->is_dense = false;
    bool nan_found = false;
    for(int a = 0; a < features->size(); a++)
    {
        ISMFeature fff = features->at(a);
        for(int b = 0; b < fff.descriptor.size(); b++)
        {
            if(std::isnan(fff.descriptor.at(b)))
            {
                nan_found = true;
                break;
            }
        }
        if(!nan_found)
        {
            features_cleaned->push_back(fff);
        }
        nan_found = false;
    }
    features_cleaned->width = features_cleaned->size();
}


flann::Matrix<float> SelfAdaptHGHV::createFlannDataset() const
{
    // create a dataset with all features for matching / activation
    int descriptor_size = m_features->at(0).descriptor.size();
    flann::Matrix<float> dataset(new float[m_features->size() * descriptor_size],
            m_features->size(), descriptor_size);

    // build dataset
    for(int i = 0; i < m_features->size(); i++)
    {
        ISMFeature ism_feat = m_features->at(i);
        std::vector<float> descriptor = ism_feat.descriptor;
        for(int j = 0; j < (int)descriptor.size(); j++)
        {
            dataset[i][j] = descriptor.at(j);
        }
    }

    return dataset;
}


pcl::CorrespondencesPtr SelfAdaptHGHV::findNnCorrespondences(const pcl::PointCloud<ISMFeature>::Ptr& scene_features) const
{
    pcl::CorrespondencesPtr model_scene_corrs(new pcl::Correspondences());

    // loop over all features extracted from the scene
    #pragma omp parallel for
    for(int fe = 0; fe < scene_features->size(); fe++)
    {
        // insert the query point
        ISMFeature feature = scene_features->at(fe);
        flann::Matrix<float> query(new float[feature.descriptor.size()], 1, feature.descriptor.size());
        for(int i = 0; i < feature.descriptor.size(); i++)
        {
            query[0][i] = feature.descriptor.at(i);
        }

        // prepare results
        std::vector<std::vector<int>> indices;
        std::vector<std::vector<float>> distances;
        m_flann_index.knnSearch(query, indices, distances, 1, flann::SearchParams(128));

        delete[] query.ptr();

        // PCL implementation has a threshold of 0.25, however, without a threshold we get better results
        float threshold = std::numeric_limits<float>::max();
        if(distances[0][0] < threshold)
        {
            // query index is scene, match is codebook ("object")
            pcl::Correspondence corr(fe, indices[0][0], distances[0][0]);
            #pragma omp critical
            {
                model_scene_corrs->push_back(corr);
            }
        }
    }

    return model_scene_corrs;
}

bool SelfAdaptHGHV::saveModelToFile(std::string &filename,
                              std::map<unsigned, pcl::PointCloud<ISMFeature>::Ptr> &all_features,
                              std::map<unsigned, std::vector<Eigen::Vector3f>> &all_vectors) const
{
    // create boost data object
    std::ofstream ofs(filename);
    boost::archive::binary_oarchive oa(ofs);

    // store label maps
    unsigned size = m_instance_to_class_map.size();
    oa << size;
    for(auto const &it : m_instance_to_class_map)
    {
        unsigned label_inst = it.first;
        unsigned label_class = it.second;
        oa << label_inst;
        oa << label_class;
    }

    size = m_class_labels.size();
    oa << size;
    for(auto it : m_class_labels)
    {
        std::string label = it.second;
        oa << label;
    }
    size = m_instance_labels.size();
    oa << size;
    for(auto it : m_instance_labels)
    {
        std::string label = it.second;
        oa << label;
    }

    // write extracted features
    int num_features = 0;
    for(auto elem : all_features)
        num_features += elem.second->size();

    int descriptor_dim = all_features[0]->at(0).descriptor.size();

    size = all_features.size();
    oa << size;
    oa << num_features;
    oa << descriptor_dim;

    //write classes
    for(auto elem : all_features)
    {
        for(unsigned int feat = 0; feat < elem.second->size(); feat++)
        {
            unsigned class_id = elem.first;
            oa << class_id;
        }
    }

    //write features
    for(auto elem : all_features)
    {
        for(unsigned int feat = 0; feat < elem.second->size(); feat++)
        {
            // write descriptor
            for(unsigned int i_dim = 0; i_dim < descriptor_dim; i_dim++)
            {
                float temp = elem.second->at(feat).descriptor.at(i_dim);
                oa << temp;
            }
            // write keypoint position
            float pos = elem.second->at(feat).x;
            oa << pos;
            pos = elem.second->at(feat).y;
            oa << pos;
            pos = elem.second->at(feat).z;
            oa << pos;
            // write reference frame
            for(unsigned lrf = 0; lrf < 9; lrf++)
            {
                pos = elem.second->at(feat).referenceFrame.rf[lrf];
                oa << pos;
            }
        }
    }

    // write all vectors
    size = all_vectors.size();
    oa << size;
    for(auto elem : all_vectors)
    {
        unsigned class_id = elem.first;
        oa << class_id;
        size = elem.second.size();
        oa << size;

        for(unsigned int vec = 0; vec < elem.second.size(); vec++)
        {
            float pos;
            pos = elem.second.at(vec).x();
            oa << pos;
            pos = elem.second.at(vec).y();
            oa << pos;
            pos = elem.second.at(vec).z();
            oa << pos;
        }
    }

    ofs.close();
    return true;
}

bool SelfAdaptHGHV::loadModelFromFile(std::string& filename)
{
    std::ifstream ifs(filename);
    if(ifs)
    {
        boost::archive::binary_iarchive ia(ifs);

        // load original labels
        unsigned size;
        ia >> size;
        m_instance_to_class_map.clear();
        for(unsigned i = 0; i < size; i++)
        {
            unsigned label_inst, label_class;
            ia >> label_inst;
            ia >> label_class;
            m_instance_to_class_map.insert({label_inst, label_class});
        }
        ia >> size;
        m_class_labels.clear();
        for(unsigned i = 0; i < size; i++)
        {
            std::string label;
            ia >> label;
            m_class_labels.insert({i, label});
        }
        ia >> size;
        m_instance_labels.clear();
        for(unsigned i = 0; i < size; i++)
        {
            std::string label;
            ia >> label;
            m_instance_labels.insert({i, label});
        }

        int number_of_features;
        int descriptor_dim;

        ia >> size;
        m_number_of_classes = size;
        ia >> number_of_features;
        ia >> descriptor_dim;

        // read classes
        m_class_lookup.clear();
        m_class_lookup.resize (number_of_features, 0);
        for (unsigned int feat_i = 0; feat_i < number_of_features; feat_i++)
        {
            ia >> m_class_lookup[feat_i];
        }

        // read features
        m_features->clear();
        for (unsigned int feat_i = 0; feat_i < number_of_features; feat_i++)
        {
            ISMFeature feature;
            feature.descriptor.resize(descriptor_dim);

            // read descriptor
            for (unsigned int dim_i = 0; dim_i < descriptor_dim; dim_i++)
            {
                ia >> feature.descriptor[dim_i];
            }

            // read keypoint position
            float pos;
            ia >> pos;
            feature.x = pos;
            ia >> pos;
            feature.y = pos;
            ia >> pos;
            feature.z = pos;
            // read reference frame
            for(unsigned lrf = 0; lrf < 9; lrf++)
            {
                ia >> pos;
                feature.referenceFrame.rf[lrf] = pos;
            }
            feature.classId = m_class_lookup.at(feat_i);
            m_features->push_back(feature);
        }

        // read all vectors
        m_center_vectors.clear();
        ia >> size;
        for(unsigned vec = 0; vec < size; vec++)
        {
            unsigned class_id;
            ia >> class_id;
            unsigned vec_size;
            ia >> vec_size;
            for(unsigned elem = 0; elem < vec_size; elem++)
            {
                float x,y,z;
                ia >> x;
                ia >> y;
                ia >> z;
                m_center_vectors.push_back(Eigen::Vector3f(x,y,z));
            }
        }

        ifs.close();
    }
    else
    {
        std::cerr << "Error opening file: " << filename << std::endl;
        return false;
    }

    return true;
}