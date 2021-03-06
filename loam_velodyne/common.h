// Copyright 2013, Ji Zhang, Carnegie Mellon University
// Further contributions copyright (c) 2016, Southwest Research Institute
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from this
//    software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// This is an implementation of the algorithm described in the following paper:
//   J. Zhang and S. Singh. LOAM: Lidar Odometry and Mapping in Real-time.
//     Robotics: Science and Systems Conference (RSS). Berkeley, CA, July 2014.

#ifndef LOAM_COMMON_H
#define LOAM_COMMON_H

#include <fstream>

//#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_types.h>
//#include <ecl/time/stopwatch.hpp>
#include <pcl/common/eigen.h>
#include <pcl/common/transforms.h>


namespace loam {

typedef int Time;
typedef size_t Key;

const Eigen::Quaterniond rot_kitti(0, 0, 0, 1.0);


inline void savePoseToFile(const Eigen::Matrix3d& rot, const Eigen::Vector3d& trans, const std::string& filename)
{
  std::ofstream myfile;
  myfile.open (filename, std::ios_base::app);
  myfile << rot(0,0) << " " << rot(0,1) << " " << rot(0,2) << " " << trans(0) << " "
         << rot(1,0) << " " << rot(1,1) << " " << rot(1,2) << " " << trans(1) << " "
         << rot(2,0) << " " << rot(2,1) << " " << rot(2,2) << " " << trans(2) << "\n";
  myfile.close();
}

} // end namespace loam

#endif // LOAM_COMMON_H
