/*
 * Copyright (C) 2020 Open Source Robotics Foundation
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

#ifndef SRC__RMF_TRAFFIC__INTERNAL_ROUTE_HPP
#define SRC__RMF_TRAFFIC__INTERNAL_ROUTE_HPP

#include <rmf_traffic/Route.hpp>

namespace rmf_traffic {

//==============================================================================
class Route::Implementation
{
public:

  std::string map;
  Trajectory trajectory;
  std::set<uint64_t> checkpoints;
  DependsOnParticipant dependencies;

  static const Route::Implementation& get(const Route& route)
  {
    return *route._pimpl;
  }
};

using RouteData = Route::Implementation;

} // namespace rmf_traffic

#endif // SRC__RMF_TRAFFIC__INTERNAL_ROUTE_HPP
