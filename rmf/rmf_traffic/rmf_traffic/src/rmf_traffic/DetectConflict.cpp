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

#include "geometry/ShapeInternal.hpp"
#include "DetectConflictInternal.hpp"
#include "ProfileInternal.hpp"
#include "Spline.hpp"
#include "StaticMotion.hpp"

#include "DetectConflictInternal.hpp"

#ifdef RMF_TRAFFIC__USING_FCL_0_6
#include <fcl/narrowphase/continuous_collision.h>
#include <fcl/math/motion/spline_motion.h>
#include <fcl/narrowphase/collision.h>
#else
#include <fcl/continuous_collision.h>
#include <fcl/ccd/motion.h>
#include <fcl/collision.h>
#endif

#include <unordered_map>

namespace rmf_traffic {

//==============================================================================
class invalid_trajectory_error::Implementation
{
public:

  std::string what;

  static invalid_trajectory_error make_segment_num_error(
    std::size_t num_segments,
    std::size_t line,
    std::string function)
  {
    invalid_trajectory_error error;
    error._pimpl->what = std::string()
      + "[rmf_traffic::invalid_trajectory_error] Attempted to check a "
      + "conflict with a Trajectory that has [" + std::to_string(num_segments)
      + "] segments. This is not supported. Trajectories must have at least "
      + "2 segments to check them for conflicts. "
      + function + ":" + std::to_string(line);
    return error;
  }

  static invalid_trajectory_error make_missing_shape_error(
    const Time time)
  {
    invalid_trajectory_error error;
    error._pimpl->what = std::string()
      + "[rmf_traffic::invalid_trajectory_error] Attempting to check a "
      + "conflict with a Trajectory that has no shape specified for the "
      + "profile of its waypoint at time ["
      + std::to_string(time.time_since_epoch().count())
      + "ns]. This is not supported.";

    return error;
  }
};

//==============================================================================
const char* invalid_trajectory_error::what() const noexcept
{
  return _pimpl->what.c_str();
}

//==============================================================================
invalid_trajectory_error::invalid_trajectory_error()
: _pimpl(rmf_utils::make_impl<Implementation>())
{
  // This constructor is a no-op, but we'll keep a definition for it in case we
  // need it in the future. Allowing the default constructor to be inferred
  // could cause issues if we want to change the implementation of this
  // exception in the future, like if we want to add more information to the
  // error message output.
}

namespace {

//==============================================================================
class Crawler
{
public:

  Crawler(
    std::size_t index_offset,
    Trajectory::const_iterator current,
    Trajectory::const_iterator end,
    const DependsOnCheckpoint* dependencies_on_me)
  : _index_offset(index_offset),
    _current(std::move(current)),
    _end(std::move(end)),
    _deps(dependencies_on_me)
  {
    if (_deps && _current != _end)
    {
      _current_dep = _deps->lower_bound(index());
    }
  }

  bool finished() const
  {
    return _current == _end;
  }

  Trajectory::const_iterator current() const
  {
    return _current;
  }

  Trajectory::const_iterator end() const
  {
    return _end;
  }

  std::size_t index() const
  {
    return _current->index() + _index_offset;
  }

  bool ignore(std::size_t other)
  {
    if (!_current_dep.has_value())
    {
      return false;
    }

    if (*_current_dep == _deps->end())
    {
      return false;
    }

    const auto ignore_other_after = (*_current_dep)->second;
    if (ignore_other_after < other)
      return true;

    return false;
  }

  void next()
  {
    if (_current_dep.has_value() && *_current_dep != _deps->end())
    {
      if ((*_current_dep)->first == index())
        ++(*_current_dep);
    }

    ++_current;
  }

  const DependsOnCheckpoint* deps() const
  {
    return _deps;
  }

private:
  std::size_t _index_offset;
  Trajectory::const_iterator _current;
  Trajectory::const_iterator _end;
  const DependsOnCheckpoint* _deps;
  std::optional<DependsOnCheckpoint::const_iterator> _current_dep;
};

//==============================================================================
bool have_time_overlap(
  const Trajectory& trajectory_a,
  const Trajectory& trajectory_b)
{
  const auto* t_a0 = trajectory_a.start_time();
  const auto* t_bf = trajectory_b.finish_time();

  // Neither of these can be null, because both trajectories should have at
  // least two elements.
  assert(t_a0 != nullptr);
  assert(t_bf != nullptr);

  if (*t_bf < *t_a0)
  {
    // If Trajectory `b` finishes before Trajectory `a` starts, then there
    // cannot be any conflict.
    return false;
  }

  const auto* t_b0 = trajectory_b.start_time();
  const auto* t_af = trajectory_a.finish_time();

  // Neither of these can be null, because both trajectories should have at
  // least two elements.
  assert(t_b0 != nullptr);
  assert(t_af != nullptr);

  if (*t_af < *t_b0)
  {
    // If Trajectory `a` finished before Trajectory `b` starts, then there
    // cannot be any conflict.
    return false;
  }

  return true;
}

//==============================================================================
std::tuple<Trajectory::const_iterator, Trajectory::const_iterator>
get_initial_iterators(
  const Trajectory& trajectory_a,
  const Trajectory& trajectory_b)
{
  const Time& t_a0 = *trajectory_a.start_time();
  const Time& t_b0 = *trajectory_b.start_time();

  Trajectory::const_iterator a_it;
  Trajectory::const_iterator b_it;

  if (t_a0 < t_b0)
  {
    // Trajectory `a` starts first, so we begin evaluating at the time
    // that `b` begins
    a_it = trajectory_a.find(t_b0);
    b_it = ++trajectory_b.begin();
  }
  else if (t_b0 < t_a0)
  {
    // Trajectory `b` starts first, so we begin evaluating at the time
    // that `a` begins
    a_it = ++trajectory_a.begin();
    b_it = trajectory_b.find(t_a0);
  }
  else
  {
    // The Trajectories begin at the exact same time, so both will begin
    // from their start
    a_it = ++trajectory_a.begin();
    b_it = ++trajectory_b.begin();
  }

  return {a_it, b_it};
}

//==============================================================================
struct BoundingBox
{
  Eigen::Vector2d min;
  Eigen::Vector2d max;
};

//==============================================================================
struct BoundingProfile
{
  BoundingBox footprint;
  BoundingBox vicinity;
};

//==============================================================================
double evaluate_spline(
  const Eigen::Vector4d& coeffs,
  const double t)
{
  // Assume time is parameterized [0,1]
  return coeffs[3] * t * t * t
    + coeffs[2] * t * t
    + coeffs[1] * t
    + coeffs[0];
}

//==============================================================================
std::array<double, 2> get_local_extrema(
  const Eigen::Vector4d& coeffs)
{
  std::vector<double> extrema_candidates;
  // Store boundary values as potential extrema
  extrema_candidates.emplace_back(evaluate_spline(coeffs, 0));
  extrema_candidates.emplace_back(evaluate_spline(coeffs, 1));

  // When derivate of spline motion is not quadratic
  if (std::abs(coeffs[3]) < 1e-12)
  {
    if (std::abs(coeffs[2]) > 1e-12)
    {
      double t = -coeffs[1] / (2 * coeffs[2]);
      extrema_candidates.emplace_back(evaluate_spline(coeffs, t));
    }
  }
  else
  {
    // Calculate the discriminant otherwise
    const double D = (4 * pow(coeffs[2], 2) - 12 * coeffs[3] * coeffs[1]);

    if (std::abs(D) < 1e-4)
    {
      const double t = (-2 * coeffs[2]) / (6 * coeffs[3]);
      const double extrema = evaluate_spline(coeffs, t);
      extrema_candidates.emplace_back(extrema);
    }
    else if (D < 0)
    {
      // If D is negative, then the local extrema would be imaginary. This will
      // happen for splines that have no local extrema. When that happens, the
      // endpoints of the spline are the only extrema.
    }
    else
    {
      const double t1 = ((-2 * coeffs[2]) + std::sqrt(D)) / (6 * coeffs[3]);
      const double t2 = ((-2 * coeffs[2]) - std::sqrt(D)) / (6 * coeffs[3]);

      extrema_candidates.emplace_back(evaluate_spline(coeffs, t1));
      extrema_candidates.emplace_back(evaluate_spline(coeffs, t2));
    }
  }

  std::array<double, 2> extrema;
  assert(!extrema_candidates.empty());
  extrema[0] = *std::min_element(
    extrema_candidates.begin(),
    extrema_candidates.end());
  extrema[1] = *std::max_element(
    extrema_candidates.begin(),
    extrema_candidates.end());

  return extrema;
}

//==============================================================================
BoundingBox get_bounding_box(const rmf_traffic::Spline& spline)
{
  auto params = spline.get_params();
  std::array<double, 2> extrema_x = get_local_extrema(params.coeffs[0]);
  std::array<double, 2> extrema_y = get_local_extrema(params.coeffs[1]);

  return BoundingBox{
    Eigen::Vector2d{extrema_x[0], extrema_y[0]},
    Eigen::Vector2d{extrema_x[1], extrema_y[1]}
  };
}

//==============================================================================
/// Create a bounding box which will never overlap with any other BoundingBox
BoundingBox void_box()
{
  constexpr double inf = std::numeric_limits<double>::infinity();
  return BoundingBox{
    Eigen::Vector2d{inf, inf},
    Eigen::Vector2d{-inf, -inf}
  };
}

//==============================================================================
BoundingBox adjust_bounding_box(
  const BoundingBox& input,
  const double value)
{
  BoundingBox box = input;
  box.min -= Eigen::Vector2d{value, value};
  box.max += Eigen::Vector2d{value, value};

  return box;
}

//==============================================================================
BoundingProfile get_bounding_profile(
  const rmf_traffic::Spline& spline,
  const Profile::Implementation& profile)
{
  BoundingBox base_box = get_bounding_box(spline);

  const auto& footprint = profile.footprint;
  const auto f_box = footprint ?
    adjust_bounding_box(base_box, footprint->get_characteristic_length()) :
    void_box();

  const auto& vicinity = profile.vicinity;
  const auto v_box = vicinity ?
    adjust_bounding_box(base_box, vicinity->get_characteristic_length()) :
    void_box();

  return BoundingProfile{f_box, v_box};
}

//==============================================================================
bool overlap(
  const BoundingBox& box_a,
  const BoundingBox& box_b)
{
  for (int i = 0; i < 2; ++i)
  {
    if (box_a.max[i] < box_b.min[i])
      return false;

    if (box_b.max[i] < box_a.min[i])
      return false;
  }

  return true;
}

//==============================================================================
#ifdef RMF_TRAFFIC__USING_FCL_0_6
using FclContinuousCollisionRequest = fcl::ContinuousCollisionRequestd;
using FclContinuousCollisionResult = fcl::ContinuousCollisionResultd;
using FclContinuousCollisionObject = fcl::ContinuousCollisionObjectd;
using FclCollisionGeometry = fcl::CollisionGeometryd;
using FclVec3 = fcl::Vector3d;
#else
using FclContinuousCollisionRequest = fcl::ContinuousCollisionRequest;
using FclContinuousCollisionResult = fcl::ContinuousCollisionResult;
using FclContinuousCollisionObject = fcl::ContinuousCollisionObject;
using FclCollisionGeometry = fcl::CollisionGeometry;
using FclVec3 = fcl::Vec3f;
#endif

//==============================================================================
std::shared_ptr<FclSplineMotion> make_uninitialized_fcl_spline_motion()
{
  // This function is only necessary because SplineMotion does not provide a
  // default constructor, and we want to be able to instantiate one before
  // we have any paramters to provide to it.
#ifdef RMF_TRAFFIC__USING_FCL_0_6
  fcl::Matrix3d R;
#else
  fcl::Matrix3f R;
#endif
  FclVec3 T;

  // The constructor that we are using is a no-op (apparently it was declared,
  // but its definition is just `// TODO`, so we don't need to worry about
  // unintended consequences. If we update the version of FCL, this may change,
  // so I'm going to leave a FIXME tag here to keep us aware of that.
  return std::make_shared<FclSplineMotion>(R, T, R, T);
}

//==============================================================================
FclContinuousCollisionRequest make_fcl_request()
{
  FclContinuousCollisionRequest request;

  request.ccd_solver_type = fcl::CCDC_CONSERVATIVE_ADVANCEMENT;
  request.gjk_solver_type = fcl::GST_LIBCCD;
  request.num_max_iterations = 15;

  return request;
}

//==============================================================================
std::optional<double> check_collision(
  const geometry::FinalConvexShape& shape_a,
  const std::shared_ptr<FclSplineMotion>& motion_a,
  const geometry::FinalConvexShape& shape_b,
  const std::shared_ptr<FclSplineMotion>& motion_b,
  const FclContinuousCollisionRequest& request)
{
  const auto obj_a = FclContinuousCollisionObject(
    geometry::FinalConvexShape::Implementation::get_collision(shape_a),
    motion_a);

  const auto obj_b = FclContinuousCollisionObject(
    geometry::FinalConvexShape::Implementation::get_collision(shape_b),
    motion_b);

  FclContinuousCollisionResult result;
  fcl::collide(&obj_a, &obj_b, request, result);

  if (result.is_collide)
    return result.time_of_contact;

  return std::nullopt;
}

//==============================================================================
Profile::Implementation convert_profile(const Profile& profile)
{
  Profile::Implementation output = Profile::Implementation::get(profile);
  if (!output.vicinity)
    output.vicinity = output.footprint;

  return output;
}

//==============================================================================
Time compute_time(
  const double scaled_time,
  const Time start_time,
  const Time finish_time)
{
  const Duration delta_t{
    Duration::rep(scaled_time * (finish_time - start_time).count())
  };

  return start_time + delta_t;
}

} // anonymous namespace

//==============================================================================
std::optional<rmf_traffic::DetectConflict::Conflict> DetectConflict::between(
  const Profile& profile_a,
  const Trajectory& trajectory_a,
  const DependsOnCheckpoint* dependencies_of_a_on_b,
  const Profile& profile_b,
  const Trajectory& trajectory_b,
  const DependsOnCheckpoint* dependencies_of_b_on_a,
  Interpolate interpolation)
{
  return Implementation::between(
    profile_a, trajectory_a, dependencies_of_a_on_b,
    profile_b, trajectory_b, dependencies_of_b_on_a,
    interpolation);
}

namespace {

//==============================================================================
bool check_overlap(
  const Profile::Implementation& profile_a,
  const Spline& spline_a,
  const Profile::Implementation& profile_b,
  const Spline& spline_b,
  const Time time)
{
  using ConvexPair = std::array<geometry::ConstFinalConvexShapePtr, 2>;
  // TODO(MXG): If footprint and vicinity are equal, we can probably reduce this
  // to just one check.
  std::array<ConvexPair, 2> pairs = {
    ConvexPair{profile_a.footprint, profile_b.vicinity},
    ConvexPair{profile_a.vicinity, profile_b.footprint}
  };

#ifdef RMF_TRAFFIC__USING_FCL_0_6
  fcl::CollisionRequestd request;
  fcl::CollisionResultd result;
  for (const auto& pair : pairs)
  {
    auto pos_a = spline_a.compute_position(time);
    auto pos_b = spline_b.compute_position(time);

    auto rot_a =
      fcl::AngleAxisd(pos_a[2], Eigen::Vector3d::UnitZ()).toRotationMatrix();
    auto rot_b =
      fcl::AngleAxisd(pos_b[2], Eigen::Vector3d::UnitZ()).toRotationMatrix();

    fcl::CollisionObjectd obj_a(
      geometry::FinalConvexShape::Implementation::get_collision(*pair[0]),
      rot_a,
      fcl::Vector3d(pos_a[0], pos_a[1], 0.0)
    );

    fcl::CollisionObjectd obj_b(
      geometry::FinalConvexShape::Implementation::get_collision(*pair[1]),
      rot_b,
      fcl::Vector3d(pos_b[0], pos_b[1], 0.0)
    );

    if (fcl::collide(&obj_a, &obj_b, request, result) > 0)
      return true;
  }
#else
  fcl::CollisionRequest request;
  fcl::CollisionResult result;

  auto convert = [](Eigen::Vector3d p) -> fcl::Transform3f
    {
      fcl::Matrix3f R;
      R.setEulerZYX(0.0, 0.0, p[2]);
      return fcl::Transform3f(R, fcl::Vec3f(p[0], p[1], 0.0));
    };

  for (const auto& pair : pairs)
  {
    fcl::CollisionObject obj_a(
      geometry::FinalConvexShape::Implementation::get_collision(*pair[0]),
      convert(spline_a.compute_position(time)));

    fcl::CollisionObject obj_b(
      geometry::FinalConvexShape::Implementation::get_collision(*pair[1]),
      convert(spline_b.compute_position(time)));

    if (fcl::collide(&obj_a, &obj_b, request, result) > 0)
      return true;
  }
#endif

  return false;
}

//==============================================================================
bool close_start(
  const Profile::Implementation& profile_a,
  const Trajectory::const_iterator& a_it,
  const Profile::Implementation& profile_b,
  const Trajectory::const_iterator& b_it)
{
  // If two trajectories start very close to each other, then we do not consider
  // it a conflict for them to be in each other's vicinities. This gives robots
  // an opportunity to back away from each other without it being considered a
  // schedule conflict.
  Spline spline_a(a_it);
  Spline spline_b(b_it);
  const auto start_time =
    std::max(spline_a.start_time(), spline_b.start_time());

  return check_overlap(profile_a, spline_a, profile_b, spline_b, start_time);
}

//==============================================================================
std::optional<DetectConflict::Conflict> detect_invasion(
  const Profile::Implementation& profile_a,
  Crawler crawl_a,
  const Profile::Implementation& profile_b,
  Crawler crawl_b,
  std::vector<DetectConflict::Conflict>* output_conflicts)
{
  using Conflict = DetectConflict::Conflict;
  std::optional<Spline> spline_a;
  std::optional<Spline> spline_b;

  std::shared_ptr<FclSplineMotion> motion_a =
    make_uninitialized_fcl_spline_motion();
  std::shared_ptr<FclSplineMotion> motion_b =
    make_uninitialized_fcl_spline_motion();

  const FclContinuousCollisionRequest request = make_fcl_request();

  // This flag lets us know that we need to test both a's footprint in b's
  // vicinity and b's footprint in a's vicinity.
  const bool test_complement =
    (profile_a.vicinity != profile_a.footprint)
    || (profile_b.vicinity != profile_b.footprint);

  if (output_conflicts)
    output_conflicts->clear();

  while (!crawl_a.finished() && !crawl_b.finished())
  {
    if (!spline_a)
      spline_a = Spline(crawl_a.current());

    if (!spline_b)
      spline_b = Spline(crawl_b.current());

    const bool ignore = crawl_a.ignore(crawl_b.index())
        || crawl_b.ignore(crawl_a.index());

    if (!ignore)
    {
      const Time start_time =
        std::max(spline_a->start_time(), spline_b->start_time());

      const Time finish_time =
        std::min(spline_a->finish_time(), spline_b->finish_time());

      *motion_a = spline_a->to_fcl(start_time, finish_time);
      *motion_b = spline_b->to_fcl(start_time, finish_time);

      const auto bound_a = get_bounding_profile(*spline_a, profile_a);
      const auto bound_b = get_bounding_profile(*spline_b, profile_b);

      if (overlap(bound_a.footprint, bound_b.vicinity))
      {
        if (const auto collision = check_collision(
            *profile_a.footprint, motion_a,
            *profile_b.vicinity, motion_b, request))
        {
          const auto time = compute_time(*collision, start_time, finish_time);
          auto conflict = Conflict{crawl_a.current(), crawl_b.current(), time};
          if (!output_conflicts)
            return conflict;

          output_conflicts->emplace_back(std::move(conflict));
        }
      }

      if (test_complement && overlap(bound_a.vicinity, bound_b.footprint))
      {
        if (const auto collision = check_collision(
            *profile_a.vicinity, motion_a,
            *profile_b.footprint, motion_b, request))
        {
          const auto time = compute_time(*collision, start_time, finish_time);
          auto conflict = Conflict{crawl_a.current(), crawl_b.current(), time};
          if (!output_conflicts)
            return conflict;

          output_conflicts->emplace_back(conflict);
        }
      }
    }

    if (spline_a->finish_time() < spline_b->finish_time())
    {
      spline_a = std::nullopt;
      crawl_a.next();
    }
    else if (spline_b->finish_time() < spline_a->finish_time())
    {
      spline_b = std::nullopt;
      crawl_b.next();
    }
    else
    {
      spline_a = std::nullopt;
      crawl_a.next();

      spline_b = std::nullopt;
      crawl_b.next();
    }
  }

  if (!output_conflicts)
    return std::nullopt;

  if (output_conflicts->empty())
    return std::nullopt;

  return output_conflicts->front();
}

//==============================================================================
Trajectory slice_trajectory(
  const Time start_time,
  const Spline& spline,
  Trajectory::const_iterator it,
  const Trajectory::const_iterator& end)
{
  Trajectory output;
  output.insert(
    start_time,
    spline.compute_position(start_time),
    spline.compute_velocity(start_time));

  for (; it != end; ++it)
    output.insert(*it);

  return output;
}

//==============================================================================
std::optional<DetectConflict::Conflict> detect_approach(
  const Profile::Implementation& profile_a,
  Crawler crawl_a,
  const Profile::Implementation& profile_b,
  Crawler crawl_b,
  std::vector<DetectConflict::Conflict>* output_conflicts)
{
  using Conflict = DetectConflict::Conflict;
  std::optional<Spline> spline_a;
  std::optional<Spline> spline_b;

  while (!crawl_a.finished() && !crawl_b.finished())
  {
    if (!spline_a)
      spline_a = Spline(crawl_a.current());

    if (!spline_b)
      spline_b = Spline(crawl_b.current());

    const DistanceDifferential D(*spline_a, *spline_b);

    const bool ignore = crawl_a.ignore(crawl_b.index())
        || crawl_b.ignore(crawl_a.index());

    if (D.initially_approaching() && !ignore)
    {
      const auto time = D.start_time();
      auto conflict = Conflict{crawl_a.current(), crawl_b.current(), time};
      if (!output_conflicts)
      {
        return conflict;
      }

      output_conflicts->emplace_back(std::move(conflict));
    }

    const auto approach_times = D.approach_times();
    for (const auto t : approach_times)
    {
      if (!check_overlap(profile_a, *spline_a, profile_b, *spline_b, t))
      {
        // If neither vehicle is in the vicinity of the other, then we should
        // revert to the normal invasion detection approach to identifying
        // conflicts.

        // TODO(MXG): Consider an approach that does not require making copies
        // of the trajectories.
        const Trajectory sliced_trajectory_a =
          slice_trajectory(t, *spline_a, crawl_a.current(), crawl_a.end());

        const Trajectory sliced_trajectory_b =
          slice_trajectory(t, *spline_b, crawl_b.current(), crawl_b.end());

        Crawler sliced_crawl_a{
          crawl_a.index() - 1,
          ++sliced_trajectory_a.begin(),
          sliced_trajectory_a.end(),
          crawl_a.deps()
        };

        Crawler sliced_crawl_b{
          crawl_b.index() - 1,
          ++sliced_trajectory_b.begin(),
          sliced_trajectory_b.end(),
          crawl_b.deps()
        };

        return detect_invasion(
          profile_a, sliced_crawl_a,
          profile_b, sliced_crawl_b,
          output_conflicts);
      }

      if (!ignore)
      {
        // If one of the vehicles is still inside the vicinity of another during
        // this approach time, then we consider this to be a conflict.
        auto conflict = Conflict{crawl_a.current(), crawl_b.current(), t};
        if (!output_conflicts)
        {
          return conflict;
        }

        output_conflicts->emplace_back(std::move(conflict));
      }
    }

    const bool still_close = check_overlap(
      profile_a, *spline_a, profile_b, *spline_b, D.finish_time());

    if (spline_a->finish_time() < spline_b->finish_time())
    {
      spline_a = std::nullopt;
      crawl_a.next();
    }
    else if (spline_b->finish_time() < spline_a->finish_time())
    {
      spline_b = std::nullopt;
      crawl_b.next();
    }
    else
    {
      spline_a = std::nullopt;
      crawl_a.next();

      spline_b = std::nullopt;
      crawl_b.next();
    }

    if (!still_close)
    {
      return detect_invasion(
        profile_a, crawl_a,
        profile_b, crawl_b,
        output_conflicts);
    }
  }

  if (!output_conflicts)
    return std::nullopt;

  if (output_conflicts->empty())
    return std::nullopt;

  return output_conflicts->front();
}

} // anonymous namespace

//==============================================================================
std::optional<DetectConflict::Conflict> DetectConflict::Implementation::between(
  const Profile& input_profile_a,
  const Trajectory& trajectory_a,
  const DependsOnCheckpoint* deps_a_on_b,
  const Profile& input_profile_b,
  const Trajectory& trajectory_b,
  const DependsOnCheckpoint* deps_b_on_a,
  Interpolate /*interpolation*/,
  std::vector<Conflict>* output_conflicts)
{
  if (trajectory_a.size() < 2)
  {
    throw invalid_trajectory_error::Implementation
          ::make_segment_num_error(
            trajectory_a.size(), __LINE__, __FUNCTION__);
  }

  if (trajectory_b.size() < 2)
  {
    throw invalid_trajectory_error::Implementation
          ::make_segment_num_error(
            trajectory_b.size(), __LINE__, __FUNCTION__);
  }

  const Profile::Implementation profile_a = convert_profile(input_profile_a);
  const Profile::Implementation profile_b = convert_profile(input_profile_b);

  // Return early if there is no geometry in the profiles
  // TODO(MXG): Should this produce an exception? Is this an okay scenario?
  if (!profile_a.footprint && !profile_b.footprint)
    return std::nullopt;

  // Return early if either profile is missing both a vicinity and a footprint.
  // NOTE(MXG): Since convert_profile will promote the vicinity to have the same
  // value as the footprint when the vicinity has a nullptr value, checking if
  // a vicinity doesn't exist is the same as checking that both the vicinity and
  // footprint doesn't exist.
  if (!profile_a.vicinity || !profile_b.vicinity)
    return std::nullopt;

  // Return early if there is no time overlap between the trajectories
  if (!have_time_overlap(trajectory_a, trajectory_b))
    return std::nullopt;

  Trajectory::const_iterator a_it;
  Trajectory::const_iterator b_it;
  std::tie(a_it, b_it) = get_initial_iterators(trajectory_a, trajectory_b);

  // NOTE: The deps are intentionally swapped here because passing them to the
  // opposite crawler makes them more efficient to crawl through.
  Crawler crawl_a(0, std::move(a_it), trajectory_a.end(), deps_b_on_a);
  Crawler crawl_b(0, std::move(b_it), trajectory_b.end(), deps_a_on_b);

  if (close_start(profile_a, crawl_a.current(), profile_b, crawl_b.current()))
  {
    // If the vehicles are already starting in close proximity, then we consider
    // it a conflict if they get any closer while within that proximity.
    return detect_approach(
      profile_a, crawl_a,
      profile_b, crawl_b,
      output_conflicts);
  }

  // If the vehicles are starting an acceptable distance from each other, then
  // check if either one invades the vicinity of the other.
  return detect_invasion(
    profile_a, crawl_a,
    profile_b, crawl_b,
    output_conflicts);
}

namespace internal {
//==============================================================================
bool detect_conflicts(
  const Profile& profile,
  const Trajectory& trajectory,
  const Spacetime& region,
  DetectConflict::Implementation::Conflicts* output_conflicts)
{
#ifndef NDEBUG
  // This should never actually happen because this function only gets used
  // internally, and so there should be several layers of quality checks on the
  // trajectories to prevent this. But we'll put it in here just in case.
  if (trajectory.size() < 2)
  {
    std::cerr << "[rmf_traffic::internal::detect_conflicts] An invalid "
              << "trajectory was passed to detect_conflicts. This is a bug "
              << "that should never happen. Please alert the RMF developers."
              << std::endl;
    throw invalid_trajectory_error::Implementation
          ::make_segment_num_error(trajectory.size(), __LINE__, __FUNCTION__);
  }
#endif // NDEBUG

  const auto vicinity = profile.vicinity();
  if (!vicinity)
    return false;

  const Time trajectory_start_time = *trajectory.start_time();
  const Time trajectory_finish_time = *trajectory.finish_time();

  const Time start_time = region.lower_time_bound ?
    std::max(*region.lower_time_bound, trajectory_start_time) :
    trajectory_start_time;

  const Time finish_time = region.upper_time_bound ?
    std::min(*region.upper_time_bound, trajectory_finish_time) :
    trajectory_finish_time;

  if (finish_time < start_time)
  {
    // If the trajectory or region finishes before the other has started, that
    // means there is no overlap in time between the region and the trajectory,
    // so it is impossible for them to conflict.
    return false;
  }

  const Trajectory::const_iterator begin_it =
    trajectory_start_time < start_time ?
    trajectory.find(start_time) : ++trajectory.begin();

  const Trajectory::const_iterator end_it =
    finish_time < trajectory_finish_time ?
    ++trajectory.find(finish_time) : trajectory.end();

  std::shared_ptr<FclSplineMotion> motion_trajectory =
    make_uninitialized_fcl_spline_motion();
  std::shared_ptr<internal::StaticMotion> motion_region =
    std::make_shared<internal::StaticMotion>(region.pose);

  const FclContinuousCollisionRequest request = make_fcl_request();

#ifdef RMF_TRAFFIC__USING_FCL_0_6
  const std::shared_ptr<fcl::CollisionGeometryd> vicinity_geom =
    geometry::FinalConvexShape::Implementation::get_collision(*vicinity);
#else
  const std::shared_ptr<fcl::CollisionGeometry> vicinity_geom =
    geometry::FinalConvexShape::Implementation::get_collision(*vicinity);
#endif

  if (output_conflicts)
    output_conflicts->clear();

  for (auto it = begin_it; it != end_it; ++it)
  {
    Spline spline_trajectory{it};

    const Time spline_start_time =
      std::max(spline_trajectory.start_time(), start_time);
    const Time spline_finish_time =
      std::min(spline_trajectory.finish_time(), finish_time);

    *motion_trajectory = spline_trajectory.to_fcl(
      spline_start_time, spline_finish_time);
#ifdef RMF_TRAFFIC__USING_FCL_0_6
    const auto obj_trajectory = fcl::ContinuousCollisionObjectd(
      vicinity_geom, motion_trajectory);
#else
    const auto obj_trajectory = fcl::ContinuousCollisionObject(
      vicinity_geom, motion_trajectory);
#endif

    assert(region.shape);
    const auto& region_shapes = geometry::FinalShape::Implementation
      ::get_collisions(*region.shape);
    for (const auto& region_shape : region_shapes)
    {
#ifdef RMF_TRAFFIC__USING_FCL_0_6
      const auto obj_region = fcl::ContinuousCollisionObjectd(
        region_shape, motion_region);
#else
      const auto obj_region = fcl::ContinuousCollisionObject(
        region_shape, motion_region);
#endif

      // TODO(MXG): We should do a broadphase test here before using
      // fcl::collide

      FclContinuousCollisionResult result;
      fcl::collide(&obj_trajectory, &obj_region, request, result);
      if (result.is_collide)
      {
        if (!output_conflicts)
          return true;

        output_conflicts->emplace_back(
          DetectConflict::Conflict{
            it, it,
            compute_time(
              result.time_of_contact,
              spline_start_time,
              spline_finish_time)
          });
      }
    }
  }

  if (!output_conflicts)
    return false;

  return !output_conflicts->empty();
}
} // namespace internal

} // namespace rmf_traffic
