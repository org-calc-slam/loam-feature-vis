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

#include "LaserOdometry.h"
#include "common.h"
#include "math_utils.h"

#include <pcl/filters/filter.h>
#include <Eigen/Eigenvalues>
#include <Eigen/QR>


namespace loam {

using std::sin;
using std::cos;
using std::asin;
using std::atan2;
using std::sqrt;
using std::fabs;
using std::pow;


LaserOdometry::LaserOdometry(const LaserOdometryParams& params)
      : _params(params),
        _systemInited(false),
        _frameCount(0),
        _timeCornerPointsSharp(0),
        _timeCornerPointsLessSharp(0),
        _timeSurfPointsFlat(0),
        _timeSurfPointsLessFlat(0),
        _timeLaserCloudFullRes(0),
        _timeImuTrans(0),
        _newCornerPointsSharp(false),
        _newCornerPointsLessSharp(false),
        _newSurfPointsFlat(false),
        _newSurfPointsLessFlat(false),
        _newLaserCloudFullRes(false),
        _newImuTrans(false),
        _cornerPointsSharp(new pcl::PointCloud<pcl::PointXYZI>()),
        _cornerPointsLessSharp(new pcl::PointCloud<pcl::PointXYZI>()),
        _surfPointsFlat(new pcl::PointCloud<pcl::PointXYZI>()),
        _surfPointsLessFlat(new pcl::PointCloud<pcl::PointXYZI>()),
        _laserCloudFullRes(new pcl::PointCloud<pcl::PointXYZI>()),
        _lastCornerCloud(new pcl::PointCloud<pcl::PointXYZI>()),
        _lastSurfaceCloud(new pcl::PointCloud<pcl::PointXYZI>()),
        _laserCloudOri(new pcl::PointCloud<pcl::PointXYZI>()),
        _coeffSel(new pcl::PointCloud<pcl::PointXYZI>()),
        _lastCornerKDTree(new nanoflann::KdTreeFLANN<pcl::PointXYZI>()),
        _lastSurfaceKDTree(new nanoflann::KdTreeFLANN<pcl::PointXYZI>())
{ }


void LaserOdometry::transformToStart(const pcl::PointXYZI& pi, pcl::PointXYZI& po)
{
  // first translate, then rotate based on registered scan time
  float s = 10 * (pi.intensity - int(pi.intensity));

  po.x = pi.x - s * _transform.pos.x();
  po.y = pi.y - s * _transform.pos.y();
  po.z = pi.z - s * _transform.pos.z();
  po.intensity = pi.intensity;

  Angle rx = -s * _transform.rot_x.rad();
  Angle ry = -s * _transform.rot_y.rad();
  Angle rz = -s * _transform.rot_z.rad();
  rotateZXY(po, rz, rx, ry);
}



size_t LaserOdometry::transformToEnd(pcl::PointCloud<pcl::PointXYZI>::Ptr& cloud)
{
  size_t cloudSize = cloud->points.size();

  for (size_t i = 0; i < cloudSize; i++) {
    pcl::PointXYZI& point = cloud->points[i];

    float s = 10 * (point.intensity - int(point.intensity));

    // rotate to start
    point.x -= s * _transform.pos.x();
    point.y -= s * _transform.pos.y();
    point.z -= s * _transform.pos.z();
    point.intensity = int(point.intensity);

    Angle rx = -s * _transform.rot_x.rad();
    Angle ry = -s * _transform.rot_y.rad();
    Angle rz = -s * _transform.rot_z.rad();
    rotateZXY(point, rz, rx, ry);

    // rotate to end
    rotateYXZ(point, _transform.rot_y, _transform.rot_x, _transform.rot_z);

    if (fabs(rx.deg()) > 5.0 || fabs(ry.deg()) > 5.0 || fabs(rz.deg()) > 5.0) {
      // ROS_INFO("[laserOdometry] LARGE transformToEnd: %f, %f, %f || s: %f", rz.deg(), rx.deg(), ry.deg(), s);
    }

    // incorporating IMU
    point.x += _transform.pos.x() - _imuShiftFromStart.x();
    point.y += _transform.pos.y() - _imuShiftFromStart.y();
    point.z += _transform.pos.z() - _imuShiftFromStart.z();

    rotateZXY(point, _imuRollStart, _imuPitchStart, _imuYawStart);
    rotateYXZ(point, -_imuYawEnd, -_imuPitchEnd, -_imuRollEnd);
  }

  return cloudSize;
}



void LaserOdometry::pluginIMURotation(const Angle& bcx, const Angle& bcy, const Angle& bcz,
                                      const Angle& blx, const Angle& bly, const Angle& blz,
                                      const Angle& alx, const Angle& aly, const Angle& alz,
                                      Angle &acx, Angle &acy, Angle &acz)
{
  float sbcx = bcx.sin();
  float cbcx = bcx.cos();
  float sbcy = bcy.sin();
  float cbcy = bcy.cos();
  float sbcz = bcz.sin();
  float cbcz = bcz.cos();

  float sblx = blx.sin();
  float cblx = blx.cos();
  float sbly = bly.sin();
  float cbly = bly.cos();
  float sblz = blz.sin();
  float cblz = blz.cos();

  float salx = alx.sin();
  float calx = alx.cos();
  float saly = aly.sin();
  float caly = aly.cos();
  float salz = alz.sin();
  float calz = alz.cos();

  float srx = -sbcx*(salx*sblx + calx*caly*cblx*cbly + calx*cblx*saly*sbly)
            - cbcx*cbcz*(calx*saly*(cbly*sblz - cblz*sblx*sbly)
                         - calx*caly*(sbly*sblz + cbly*cblz*sblx) + cblx*cblz*salx)
            - cbcx*sbcz*(calx*caly*(cblz*sbly - cbly*sblx*sblz)
                         - calx*saly*(cbly*cblz + sblx*sbly*sblz) + cblx*salx*sblz);
  acx = -asin(srx);

  float srycrx = (cbcy*sbcz - cbcz*sbcx*sbcy)*(calx*saly*(cbly*sblz - cblz*sblx*sbly)
                                               - calx*caly*(sbly*sblz + cbly*cblz*sblx) + cblx*cblz*salx)
                 - (cbcy*cbcz + sbcx*sbcy*sbcz)*(calx*caly*(cblz*sbly - cbly*sblx*sblz)
                                                 - calx*saly*(cbly*cblz + sblx*sbly*sblz) + cblx*salx*sblz)
                 + cbcx*sbcy*(salx*sblx + calx*caly*cblx*cbly + calx*cblx*saly*sbly);
  float crycrx = (cbcz*sbcy - cbcy*sbcx*sbcz)*(calx*caly*(cblz*sbly - cbly*sblx*sblz)
                                               - calx*saly*(cbly*cblz + sblx*sbly*sblz) + cblx*salx*sblz)
                 - (sbcy*sbcz + cbcy*cbcz*sbcx)*(calx*saly*(cbly*sblz - cblz*sblx*sbly)
                                                 - calx*caly*(sbly*sblz + cbly*cblz*sblx) + cblx*cblz*salx)
                 + cbcx*cbcy*(salx*sblx + calx*caly*cblx*cbly + calx*cblx*saly*sbly);
  acy = atan2(srycrx / acx.cos(), crycrx / acx.cos());

  float srzcrx = sbcx*(cblx*cbly*(calz*saly - caly*salx*salz) - cblx*sbly*(caly*calz + salx*saly*salz) + calx*salz*sblx)
                 - cbcx*cbcz*((caly*calz + salx*saly*salz)*(cbly*sblz - cblz*sblx*sbly)
                              + (calz*saly - caly*salx*salz)*(sbly*sblz + cbly*cblz*sblx)
                              - calx*cblx*cblz*salz)
                 + cbcx*sbcz*((caly*calz + salx*saly*salz)*(cbly*cblz + sblx*sbly*sblz)
                              + (calz*saly - caly*salx*salz)*(cblz*sbly - cbly*sblx*sblz)
                              + calx*cblx*salz*sblz);
  float crzcrx = sbcx*(cblx*sbly*(caly*salz - calz*salx*saly) - cblx*cbly*(saly*salz + caly*calz*salx) + calx*calz*sblx)
                 + cbcx*cbcz*((saly*salz + caly*calz*salx)*(sbly*sblz + cbly*cblz*sblx)
                              + (caly*salz - calz*salx*saly)*(cbly*sblz - cblz*sblx*sbly)
                              + calx*calz*cblx*cblz)
                 - cbcx*sbcz*((saly*salz + caly*calz*salx)*(cblz*sbly - cbly*sblx*sblz)
                              + (caly*salz - calz*salx*saly)*(cbly*cblz + sblx*sbly*sblz)
                              - calx*calz*cblx*sblz);
  acz = atan2(srzcrx / acx.cos(), crzcrx / acx.cos()); 
}



void LaserOdometry::accumulateRotation(Angle cx, Angle cy, Angle cz,
                                       Angle lx, Angle ly, Angle lz,
                                       Angle &ox, Angle &oy, Angle &oz)
{
  /*
  float srx = lx.cos()*cx.cos()*ly.sin()*cz.sin()
            - cx.cos()*cz.cos()*lx.sin()
            - lx.cos()*ly.cos()*cx.sin();
  ox = -asin(srx);

  float srycrx = lx.sin()*(cy.cos()*cz.sin() - cz.cos()*cx.sin()*cy.sin())
               + lx.cos()*ly.sin()*(cy.cos()*cz.cos() + cx.sin()*cy.sin()*cz.sin())
               + lx.cos()*ly.cos()*cx.cos()*cy.sin();
  float crycrx = lx.cos()*ly.cos()*cx.cos()*cy.cos()
               - lx.cos()*ly.sin()*(cz.cos()*cy.sin() - cy.cos()*cx.sin()*cz.sin())
               - lx.sin()*(cy.sin()*cz.sin() + cy.cos()*cz.cos()*cx.sin());
  oy = atan2(srycrx / ox.cos(), crycrx / ox.cos());

  float srzcrx = cx.sin()*(lz.cos()*ly.sin() - ly.cos()*lx.sin()*lz.sin())
               + cx.cos()*cz.sin()*(ly.cos()*lz.cos() + lx.sin()*ly.sin()*lz.sin())
               + lx.cos()*cx.cos()*cz.cos()*lz.sin();
  float crzcrx = lx.cos()*lz.cos()*cx.cos()*cz.cos()
               - cx.cos()*cz.sin()*(ly.cos()*lz.sin() - lz.cos()*lx.sin()*ly.sin())
               - cx.sin()*(ly.sin()*lz.sin() + ly.cos()*lz.cos()*lx.sin());
  oz = atan2(srzcrx / ox.cos(), crzcrx / ox.cos()); */

  Eigen::Affine3f current = pcl::getTransformation(0, 0, 0, cy.rad(), cx.rad(), cz.rad());
  Eigen::Affine3f last = pcl::getTransformation(0, 0, 0, ly.rad(), lx.rad(), lz.rad());

  float oy_f, ox_f, oz_f;
  pcl::getEulerAngles(last*current, oy_f, ox_f, oz_f);
  ox = ox_f;
  oy = oy_f;
  oz = oz_f;
}


void LaserOdometry::spin()
{
  // ros::Rate rate(100);
  // bool status = ros::ok();

  // // loop until shutdown
  // while (status) {
  //   ros::spinOnce();

  //   // try processing new data
  //   process();

  //   status = ros::ok();
  //   rate.sleep();
  // }
}



void LaserOdometry::reset()
{
  _newCornerPointsSharp = false;
  _newCornerPointsLessSharp = false;
  _newSurfPointsFlat = false;
  _newSurfPointsLessFlat = false;
  _newLaserCloudFullRes = false;
  _newImuTrans = false;
}



bool LaserOdometry::hasNewData()
{
  return _newCornerPointsSharp && _newCornerPointsLessSharp && _newSurfPointsFlat &&
         _newSurfPointsLessFlat && _newLaserCloudFullRes && _newImuTrans &&
         fabs(_timeCornerPointsSharp - _timeSurfPointsLessFlat) < 0.005 &&
         fabs(_timeCornerPointsLessSharp - _timeSurfPointsLessFlat) < 0.005 &&
         fabs(_timeSurfPointsFlat - _timeSurfPointsLessFlat) < 0.005 &&
         fabs(_timeLaserCloudFullRes - _timeSurfPointsLessFlat) < 0.005 &&
         fabs(_timeImuTrans - _timeSurfPointsLessFlat) < 0.005;
}



bool LaserOdometry::process()
{
  if (!hasNewData()) {
    // waiting for new data to arrive...
    return false;
  }

//  ecl::StopWatch stopWatch;

  // reset flags, etc.
  reset();

  if (!_systemInited) {
    _cornerPointsLessSharp.swap(_lastCornerCloud);
    _surfPointsLessFlat.swap(_lastSurfaceCloud);

    _lastCornerKDTree->setInputCloud(_lastCornerCloud);
    _lastSurfaceKDTree->setInputCloud(_lastSurfaceCloud);

    _transformSum.rot_x += _imuPitchStart;
    _transformSum.rot_z += _imuRollStart;

    _systemInited = true;
    return false;
  }

  pcl::PointXYZI coeff;
  bool isDegenerate = false;
  Eigen::Matrix<float,6,6> matP;

  _frameCount++;
  _transform.pos -= _imuVeloFromStart * _params.scanPeriod;

  bool isConverged = false;

  size_t lastCornerCloudSize = _lastCornerCloud->points.size();
  size_t lastSurfaceCloudSize = _lastSurfaceCloud->points.size();

  if (lastCornerCloudSize > 10 && lastSurfaceCloudSize > 100) {
    std::vector<int> pointSearchInd(1);
    std::vector<float> pointSearchSqDis(1);
    std::vector<int> indices;

    pcl::removeNaNFromPointCloud(*_cornerPointsSharp, *_cornerPointsSharp, indices);
    size_t cornerPointsSharpNum = _cornerPointsSharp->points.size();
    size_t surfPointsFlatNum = _surfPointsFlat->points.size();

    _pointSearchCornerInd1.resize(cornerPointsSharpNum);
    _pointSearchCornerInd2.resize(cornerPointsSharpNum);
    _pointSearchSurfInd1.resize(surfPointsFlatNum);
    _pointSearchSurfInd2.resize(surfPointsFlatNum);
    _pointSearchSurfInd3.resize(surfPointsFlatNum);

    for (size_t iterCount = 0; iterCount < _params.maxIterations; iterCount++) {
      pcl::PointXYZI pointSel, pointProj, tripod1, tripod2, tripod3;
      _laserCloudOri->clear();
      _coeffSel->clear();

      for (size_t i = 0; i < cornerPointsSharpNum; i++) {
//        transformToStart(_cornerPointsSharp->points[i], pointSel); //

        if (iterCount % 5 == 0) {
          pcl::removeNaNFromPointCloud(*_lastCornerCloud, *_lastCornerCloud, indices);
          _lastCornerKDTree->nearestKSearch(pointSel, 1, pointSearchInd, pointSearchSqDis);

          int closestPointInd = -1, minPointInd2 = -1;
          if (pointSearchSqDis[0] < 25) {
            closestPointInd = pointSearchInd[0];
            int closestPointScan = int(_lastCornerCloud->points[closestPointInd].intensity);

            float pointSqDis, minPointSqDis2 = 25;
            for (size_t j = closestPointInd + 1; j < cornerPointsSharpNum; j++) {
              if (size_t(_lastCornerCloud->points[j].intensity) > closestPointScan + 2.5) {
                break;
              }

              pointSqDis = calcSquaredDiff(_lastCornerCloud->points[j], pointSel);

              if (int(_lastCornerCloud->points[j].intensity) > closestPointScan) {
                if (pointSqDis < minPointSqDis2) {
                  minPointSqDis2 = pointSqDis;
                  minPointInd2 = j;
                }
              }
            }
            for (int j = closestPointInd - 1; j >= 0; j--) {
              if (int(_lastCornerCloud->points[j].intensity) < closestPointScan - 2.5) {
                break;
              }

              pointSqDis = calcSquaredDiff(_lastCornerCloud->points[j], pointSel);

              if (int(_lastCornerCloud->points[j].intensity) < closestPointScan) {
                if (pointSqDis < minPointSqDis2) {
                  minPointSqDis2 = pointSqDis;
                  minPointInd2 = j;
                }
              }
            }
          }

          _pointSearchCornerInd1[i] = closestPointInd;
          _pointSearchCornerInd2[i] = minPointInd2;
        }

        if (_pointSearchCornerInd2[i] >= 0) {
          tripod1 = _lastCornerCloud->points[_pointSearchCornerInd1[i]];
          tripod2 = _lastCornerCloud->points[_pointSearchCornerInd2[i]];

          float x0 = pointSel.x;
          float y0 = pointSel.y;
          float z0 = pointSel.z;
          float x1 = tripod1.x;
          float y1 = tripod1.y;
          float z1 = tripod1.z;
          float x2 = tripod2.x;
          float y2 = tripod2.y;
          float z2 = tripod2.z;

          float a012 = sqrt(((x0 - x1)*(y0 - y2) - (x0 - x2)*(y0 - y1))
                            * ((x0 - x1)*(y0 - y2) - (x0 - x2)*(y0 - y1))
                            + ((x0 - x1)*(z0 - z2) - (x0 - x2)*(z0 - z1))
                              * ((x0 - x1)*(z0 - z2) - (x0 - x2)*(z0 - z1))
                            + ((y0 - y1)*(z0 - z2) - (y0 - y2)*(z0 - z1))
                              * ((y0 - y1)*(z0 - z2) - (y0 - y2)*(z0 - z1)));

          float l12 = sqrt((x1 - x2)*(x1 - x2) + (y1 - y2)*(y1 - y2) + (z1 - z2)*(z1 - z2));

          float la = ((y1 - y2)*((x0 - x1)*(y0 - y2) - (x0 - x2)*(y0 - y1))
                      + (z1 - z2)*((x0 - x1)*(z0 - z2) - (x0 - x2)*(z0 - z1))) / a012 / l12;

          float lb = -((x1 - x2)*((x0 - x1)*(y0 - y2) - (x0 - x2)*(y0 - y1))
                       - (z1 - z2)*((y0 - y1)*(z0 - z2) - (y0 - y2)*(z0 - z1))) / a012 / l12;

          float lc = -((x1 - x2)*((x0 - x1)*(z0 - z2) - (x0 - x2)*(z0 - z1))
                       + (y1 - y2)*((y0 - y1)*(z0 - z2) - (y0 - y2)*(z0 - z1))) / a012 / l12;

          float ld2 = a012 / l12;

          // TODO: Why writing to a variable that's never read?
          pointProj = pointSel;
          pointProj.x -= la * ld2;
          pointProj.y -= lb * ld2;
          pointProj.z -= lc * ld2;

          float s = 1;
          if (iterCount >= 5) {
            s = 1 - 1.8f * fabs(ld2);
          }

          coeff.x = s * la;
          coeff.y = s * lb;
          coeff.z = s * lc;
          coeff.intensity = s * ld2;

          if (s > 0.1 && ld2 != 0) {
            _laserCloudOri->push_back(_cornerPointsSharp->points[i]);
            _coeffSel->push_back(coeff);
          }
        }
      }

      for (size_t i = 0; i < surfPointsFlatNum; i++) {
//        transformToStart(_surfPointsFlat->points[i], pointSel);

        if (iterCount % 5 == 0) {
          _lastSurfaceKDTree->nearestKSearch(pointSel, 1, pointSearchInd, pointSearchSqDis);
          int closestPointInd = -1, minPointInd2 = -1, minPointInd3 = -1;
          if (pointSearchSqDis[0] < 25) {
            closestPointInd = pointSearchInd[0];
            int closestPointScan = int(_lastSurfaceCloud->points[closestPointInd].intensity);

            float pointSqDis, minPointSqDis2 = 25, minPointSqDis3 = 25;
            for (size_t j = closestPointInd + 1; j < surfPointsFlatNum; j++) {
              if (int(_lastSurfaceCloud->points[j].intensity) > closestPointScan + 2.5) {
                break;
              }

              pointSqDis = calcSquaredDiff(_lastSurfaceCloud->points[j], pointSel);

              if (int(_lastSurfaceCloud->points[j].intensity) <= closestPointScan) {
                if (pointSqDis < minPointSqDis2) {
                  minPointSqDis2 = pointSqDis;
                  minPointInd2 = j;
                }
              } else {
                if (pointSqDis < minPointSqDis3) {
                  minPointSqDis3 = pointSqDis;
                  minPointInd3 = j;
                }
              }
            }
            for (int j = closestPointInd - 1; j >= 0; j--) {
              if (int(_lastSurfaceCloud->points[j].intensity) < closestPointScan - 2.5) {
                break;
              }

              pointSqDis = calcSquaredDiff(_lastSurfaceCloud->points[j], pointSel);

              if (int(_lastSurfaceCloud->points[j].intensity) >= closestPointScan) {
                if (pointSqDis < minPointSqDis2) {
                  minPointSqDis2 = pointSqDis;
                  minPointInd2 = j;
                }
              } else {
                if (pointSqDis < minPointSqDis3) {
                  minPointSqDis3 = pointSqDis;
                  minPointInd3 = j;
                }
              }
            }
          }

          _pointSearchSurfInd1[i] = closestPointInd;
          _pointSearchSurfInd2[i] = minPointInd2;
          _pointSearchSurfInd3[i] = minPointInd3;
        }

        if (_pointSearchSurfInd2[i] >= 0 && _pointSearchSurfInd3[i] >= 0) {
          tripod1 = _lastSurfaceCloud->points[_pointSearchSurfInd1[i]];
          tripod2 = _lastSurfaceCloud->points[_pointSearchSurfInd2[i]];
          tripod3 = _lastSurfaceCloud->points[_pointSearchSurfInd3[i]];

          float pa = (tripod2.y - tripod1.y) * (tripod3.z - tripod1.z)
                     - (tripod3.y - tripod1.y) * (tripod2.z - tripod1.z);
          float pb = (tripod2.z - tripod1.z) * (tripod3.x - tripod1.x)
                     - (tripod3.z - tripod1.z) * (tripod2.x - tripod1.x);
          float pc = (tripod2.x - tripod1.x) * (tripod3.y - tripod1.y)
                     - (tripod3.x - tripod1.x) * (tripod2.y - tripod1.y);
          float pd = -(pa * tripod1.x + pb * tripod1.y + pc * tripod1.z);

          float ps = sqrt(pa * pa + pb * pb + pc * pc);
          pa /= ps;
          pb /= ps;
          pc /= ps;
          pd /= ps;

          float pd2 = pa * pointSel.x + pb * pointSel.y + pc * pointSel.z + pd;

          // TODO: Why writing to a variable that's never read? Maybe it should be used afterwards?
          pointProj = pointSel;
          pointProj.x -= pa * pd2;
          pointProj.y -= pb * pd2;
          pointProj.z -= pc * pd2;

          float s = 1;
          if (iterCount >= 5) {
            s = 1 - 1.8f * fabs(pd2) / sqrt(calcPointDistance(pointSel));
          }

          coeff.x = s * pa;
          coeff.y = s * pb;
          coeff.z = s * pc;
          coeff.intensity = s * pd2;

          if (s > 0.1 && pd2 != 0) {
            _laserCloudOri->push_back(_surfPointsFlat->points[i]);
            _coeffSel->push_back(coeff);
          }
        }
      }

      size_t pointSelNum = _laserCloudOri->points.size();
      if (pointSelNum < 10) {
        continue;
      }

      Eigen::Matrix<float,Eigen::Dynamic,6> matA(pointSelNum, 6);
      Eigen::Matrix<float,6,Eigen::Dynamic> matAt(6,pointSelNum);
      Eigen::Matrix<float,6,6> matAtA;
      Eigen::VectorXf matB(pointSelNum);
      Eigen::Matrix<float,6,1> matAtB;
      Eigen::Matrix<float,6,1> matX;

      for (size_t i = 0; i < pointSelNum; i++) {
        const pcl::PointXYZI& pointOri = _laserCloudOri->points[i];
        coeff = _coeffSel->points[i];

        float s = 1;

        float srx = sin(s * _transform.rot_x.rad());
        float crx = cos(s * _transform.rot_x.rad());
        float sry = sin(s * _transform.rot_y.rad());
        float cry = cos(s * _transform.rot_y.rad());
        float srz = sin(s * _transform.rot_z.rad());
        float crz = cos(s * _transform.rot_z.rad());
        float tx = s * _transform.pos.x();
        float ty = s * _transform.pos.y();
        float tz = s * _transform.pos.z();

      	// updated derivatives with respect to rotation and translation
      	float arx, ary, arz, atx, aty, atz;
      	int selectMethodType = 1; // 1: original; else: disturbance model

      	if (selectMethodType == 1) {
           arx = s * (- pointOri.x * (crx * sry * srz)
                      + pointOri.y * (crx * crz * sry)
                      + pointOri.z * (srx * sry)
                      + tx * (crx * sry * srz)
                      - ty * (crx * crz * sry)
                      - tz * (srx * sry)) * coeff.x
               + s * (+ pointOri.x * (srx * srz)
                      - pointOri.y * (crz * srx)
                      + pointOri.z * crx
                      - tx * (srx * srz)
                      + ty * (crz * srx)
                      - tz * (crx)) * coeff.y
               + s * (+ pointOri.x * (crx * cry * srz)
                      - pointOri.y * (crx * cry * crz)
                      - pointOri.z * (cry * srx)
                      - tx * (crx * cry * srz)
                      + ty * (crx * cry * crz)
                      + tz * (cry * srx)) * coeff.z;

           ary = s * (- pointOri.x * (crz * sry + cry * srx * srz)
                      - pointOri.y * (sry * srz - cry * crz * srx)
                      - pointOri.z * (crx * cry)
                      + tx * (crz * sry + cry * srx * srz)
                      + ty * (sry * srz - cry * crz * srx)
                      + tz * (crx * cry)) * coeff.x
               + s * (+ pointOri.x * (cry * crz - srx * sry * srz)
                      + pointOri.y * (cry * srz + crz * srx * sry)
                      - pointOri.z * (crx * sry)
                      - tx * (cry * crz - srx * sry * srz)
                      - ty * (cry * srz + crz * srx * sry)
                      + tz * (crx * sry)) * coeff.z;

           arz = s * (- pointOri.x * (cry * srz + crz * srx * sry)
                      + pointOri.y * (cry * crz - srx * sry * srz)
                      + tx * (cry * srz + crz * srx * sry)
                      - ty * (cry * crz - srx * sry * srz)) * coeff.x
               + s * (- pointOri.x * (crx * crz)
                      - pointOri.y * (crx * srz)
                      + tx * crx * crz
                      + ty * crx * srz) * coeff.y
               + s * (+ pointOri.x * (cry * crz * srx - sry * srz)
                      + pointOri.y * (crz * sry + cry * srx * srz)
                      + tx * (sry * srz - cry * crz * srx)
                      - ty * (crz * sry + cry * srx * srz)) * coeff.z;

           atx = - s * (cry * crz - srx * sry * srz) * coeff.x
                 + s * (crx * srz) * coeff.y
                 - s * (crz * sry + cry * srx * srz) * coeff.z;
           aty = - s * (cry * srz + crz * srx * sry) * coeff.x
                 - s * (crx * crz) * coeff.y
                 - s * (sry * srz - cry * crz * srx) * coeff.z;
           atz = + s * (crx * sry) * coeff.x
                 - s * (srx) * coeff.y
                 - s * (crx * cry) * coeff.z;
      	} else {
           s = 1.0;
           float x_trf_bck = + pointOri.x * (crz * cry + srx * sry * srz)
                             + pointOri.y * (cry * srz - crz * sry * srx)
                             + pointOri.z * (crx * sry)
                             + tx * (-crz * cry - srz * sry * srz)
                             + ty * (-cry * srz + crz * sry * srx)
                             + tz * (-crx * sry);
           float y_trf_bck = + pointOri.x * (-crx * srz)
                             + pointOri.y * (crz * crx)
                             + pointOri.z * (srx)
                             + tx * (crx * srz)
                             + ty * (-crz * crx)
                             + tz * (-srx);
           float z_trf_bck = + pointOri.x * (-crz * sry + cry * srz * srx)
                             + pointOri.y * (-srz * sry - crz * cry * srx)
                             + pointOri.z * (cry * crx)
                             + tx * (crz * sry - cry * srz * srx)
                             + ty * (srz * sry + crz * cry * srx)
                             + tz * (-cry * crx);

           arx = -s * (0.0 *        coeff.x - z_trf_bck * coeff.y + y_trf_bck * coeff.z);
           ary = -s * (z_trf_bck *  coeff.x + 0.0 *       coeff.y - x_trf_bck * coeff.z);
           arz = -s * (-y_trf_bck * coeff.x + x_trf_bck * coeff.y + 0.0       * coeff.z);

           atx = -s * coeff.x;
           aty = -s * coeff.y;
           atz = -s * coeff.z;
      	}

        float d2 = coeff.intensity;

        matA(i, 0) = arx;
        matA(i, 1) = ary;
        matA(i, 2) = arz;
        matA(i, 3) = atx;
        matA(i, 4) = aty;
        matA(i, 5) = atz;
        matB(i, 0) = -0.05 * d2;
      }
      matAt = matA.transpose();
      matAtA = matAt * matA;
      matAtB = matAt * matB;

      matX = matAtA.colPivHouseholderQr().solve(matAtB);

      if (iterCount == 0) {
        Eigen::Matrix<float,1,6> matE;
        Eigen::Matrix<float,6,6> matV;
        Eigen::Matrix<float,6,6> matV2;

        Eigen::SelfAdjointEigenSolver< Eigen::Matrix<float,6, 6> > esolver(matAtA);
        matE = esolver.eigenvalues().real();
        matV = esolver.eigenvectors().real();

        matV2 = matV;

        isDegenerate = false;
        float eignThre[6] = {10, 10, 10, 10, 10, 10};
        for (int i = 5; i >= 0; i--) {
          if (matE(0, i) < eignThre[i]) {
            for (int j = 0; j < 6; j++) {
              matV2(i, j) = 0;
            }
            isDegenerate = true;
          } else {
            break;
          }
        }
        matP = matV.inverse() * matV2;
      }

      if (isDegenerate) {
        Eigen::Matrix<float,6,1> matX2;
        matX2 = matX;
        matX = matP * matX2;
      }

      _transform.rot_x = _transform.rot_x.rad() + matX(0, 0);
      _transform.rot_y = _transform.rot_y.rad() + matX(1, 0);
      _transform.rot_z = _transform.rot_z.rad() + matX(2, 0);
      _transform.pos.x() += matX(3, 0);
      _transform.pos.y() += matX(4, 0);
      _transform.pos.z() += matX(5, 0);

      if( !pcl_isfinite(_transform.rot_x.rad()) ) _transform.rot_x = Angle();
      if( !pcl_isfinite(_transform.rot_y.rad()) ) _transform.rot_y = Angle();
      if( !pcl_isfinite(_transform.rot_z.rad()) ) _transform.rot_z = Angle();

      if( !pcl_isfinite(_transform.pos.x()) ) _transform.pos.x() = 0.0;
      if( !pcl_isfinite(_transform.pos.y()) ) _transform.pos.y() = 0.0;
      if( !pcl_isfinite(_transform.pos.z()) ) _transform.pos.z() = 0.0;

      float deltaR = sqrt(pow(rad2deg(matX(0, 0)), 2) +
                          pow(rad2deg(matX(1, 0)), 2) +
                          pow(rad2deg(matX(2, 0)), 2));
      float deltaT = sqrt(pow(matX(3, 0) * 100, 2) +
                          pow(matX(4, 0) * 100, 2) +
                          pow(matX(5, 0) * 100, 2));

      if (deltaR < _params.deltaRAbort && deltaT < _params.deltaTAbort) {
        // ROS_INFO("[laserOdometry] Optimization Done: %lu, %i, %f, %f", pointSelNum, int(iterCount), deltaR, deltaT);
        std::printf("[laserOdometry] Optimization Done: %lu, %i, %f, %f", pointSelNum, int(iterCount), deltaR, deltaT);
        isConverged = true;
        break;
      }
    }
    if (!isConverged) {
       std::printf("[LaserOdometry] Optimization Incomplete\n");
    }
  }

  if (_transform.rot_x.deg() > 1.0 || _transform.rot_y.deg() > 1.0 || _transform.rot_z.deg() > 1.0 ) {
    // ROS_INFO("[laserOdometry] LARGE _transform.rot %f, %f, %f", _transform.rot_x.deg(), _transform.rot_y.deg(), _transform.rot_z.deg());
    std::printf("[laserOdometry] LARGE _transform.rot %f, %f, %f", _transform.rot_x.deg(), _transform.rot_y.deg(), _transform.rot_z.deg());
  }

  Angle rx, ry, rz;
  float corr = 1.0;
  accumulateRotation(_transformSum.rot_x,
                     _transformSum.rot_y,
                     _transformSum.rot_z,
                     -_transform.rot_x,
                     -_transform.rot_y.rad() * corr,
                     -_transform.rot_z,
                     rx, ry, rz);

  Vector3 v( _transform.pos.x()        - _imuShiftFromStart.x(),
             _transform.pos.y()        - _imuShiftFromStart.y(),
             _transform.pos.z() * corr - _imuShiftFromStart.z() );
  rotateZXY(v, rz, rx, ry);
  Vector3 trans = _transformSum.pos - v;

  pluginIMURotation(rx, ry, rz,
                    _imuPitchStart, _imuYawStart, _imuRollStart,
                    _imuPitchEnd, _imuYawEnd, _imuRollEnd,
                    rx, ry, rz);

  _transformSum.rot_x = rx;
  _transformSum.rot_y = ry;
  _transformSum.rot_z = rz;
  _transformSum.pos = trans;

  transformToEnd(_cornerPointsLessSharp);
  transformToEnd(_surfPointsLessFlat);

  _cornerPointsLessSharp.swap(_lastCornerCloud);
  _surfPointsLessFlat.swap(_lastSurfaceCloud);

  lastCornerCloudSize = _lastCornerCloud->points.size();
  lastSurfaceCloudSize = _lastSurfaceCloud->points.size();

  if (lastCornerCloudSize > 10 && lastSurfaceCloudSize > 100) {
//    _lastCornerKDTree.setInputCloud(_lastCornerCloud);
//    _lastSurfaceKDTree.setInputCloud(_lastSurfaceCloud);
  }

  return true;
}

bool LaserOdometry::generateRegisteredCloud(pcl::PointCloud<pcl::PointXYZI>::Ptr& registered_cloud) {
  // transform full resolution input cloud to end
  if (_params.ioRatio < 2 || _frameCount % _params.ioRatio == 1) {
    transformToEnd(_laserCloudFullRes);  // transform full resolution cloud to sweep end before sending it

//    pcl::copyPointCloud(*_laserCloudFullRes, *registered_cloud); // TODO: fix it

    return true;
  }
  
  return false;
}

} // end namespace loam
