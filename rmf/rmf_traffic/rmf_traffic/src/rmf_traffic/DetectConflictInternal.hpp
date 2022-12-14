/*
 * Copyright (C) 2019 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#ifndef SRC__RMF_UTILS__DETECTCONFLICTINTERNAL_HPP
#define SRC__RMF_UTILS__DETECTCONFLICTINTERNAL_HPP

#include <rmf_traffic/DetectConflict.hpp>

#include "geometry/ShapeInternal.hpp"

#include <rmf_traffic/Profile.hpp>
#include <rmf_traffic/Trajectory.hpp>

#include <unordered_map>

namespace rmf_traffic {

class DetectConflict::Implementation
{
public:

  using Conflicts = std::vector<Conflict>;

  static std::optional<Conflict> between(
    const Profile& profile_a,
    const Trajectory& trajectory_a,
    const DependsOnCheckpoint* deps_a,
    const Profile& profile_b,
    const Trajectory& trajectory_b,
    const DependsOnCheckpoint* deps_b,
    Interpolate interpolation,
    std::vector<Conflict>* output_conflicts = nullptr);

};

namespace internal {

//==============================================================================
struct Spacetime
{
  const Time* lower_time_bound;
  const Time* upper_time_bound;

  Eigen::Isometry2d pose;
  geometry::ConstFinalShapePtr shape;
};

//==============================================================================
bool detect_conflicts(
  const Profile& profile,
  const Trajectory& trajectory,
  const Spacetime& region,
  DetectConflict::Implementation::Conflicts* output_conflicts = nullptr);

} // namespace internal

} // namespace rmf_traffic

#endif // SRC__RMF_UTILS__DETECTCONFLICTINTERNAL_HPP
