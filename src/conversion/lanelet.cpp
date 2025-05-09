// Copyright 2024 TIER IV, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "autoware/diffusion_planner/conversion/lanelet.hpp"

#include "autoware/diffusion_planner/polyline.hpp"

#include <autoware_lanelet2_extension/regulatory_elements/Forward.hpp>

#include <geometry_msgs/msg/detail/point__struct.hpp>

#include <lanelet2_core/Forward.h>

#include <cmath>
#include <iostream>
#include <optional>
#include <vector>

namespace autoware::diffusion_planner
{
enum LIGHT_SIGNAL_STATE {
  GREEN = 0,
  AMBER = 1,
  RED = 2,
  UNKNOWN = 3,
};
// uint8_t get_traffic_signal(
//   const TrafficLightIdMap & traffic_light_id_map, lanelet::ConstLanelet & lanelet)
// {
//   const auto tl_reg_elems = lanelet.regulatoryElementsAs<const lanelet::TrafficLight>();
//   if (tl_reg_elems.empty()) { return}
// }

// Compute Euclidean distance between two LanePoints
inline float euclidean_distance(const LanePoint & p1, const LanePoint & p2)
{
  float dx = p2.x() - p1.x();
  float dy = p2.y() - p1.y();
  float dz = p2.z() - p1.z();
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Linearly interpolate between two LanePoints
inline LanePoint linear_interpolate(const LanePoint & p1, const LanePoint & p2, float t)
{
  float x = p1.x() + t * (p2.x() - p1.x());
  float y = p1.y() + t * (p2.y() - p1.y());
  float z = p1.z() + t * (p2.z() - p1.z());

  float dx = (1 - t) * (p1.data_ptr()[3]) + t * (p2.data_ptr()[3]);
  float dy = (1 - t) * (p1.data_ptr()[4]) + t * (p2.data_ptr()[4]);
  float dz = (1 - t) * (p1.data_ptr()[5]) + t * (p2.data_ptr()[5]);
  float label = 0.0;  // TODO(Daniel): add labels

  return {x, y, z, dx, dy, dz, label};
}

std::vector<LanePoint> interpolate_points(const std::vector<LanePoint> & input, size_t num_points)
{
  if (input.size() < 2 || num_points < 2) {
    std::cerr << "Need at least 2 input points\n";
    return input;
  }
  // Step 1: Compute cumulative distances
  std::vector<float> arc_lengths(input.size(), 0.0f);
  for (size_t i = 1; i < input.size(); ++i) {
    arc_lengths[i] = arc_lengths[i - 1] + input[i].distance(input[i - 1]);
  }
  float total_length = arc_lengths.back();

  // Step 2: Generate target arc lengths
  std::vector<float> target_lengths(num_points);
  float step = total_length / (num_points - 1);
  for (size_t i = 0; i < num_points; ++i) {
    target_lengths[i] = i * step;
  }

  // Step 3: Interpolate new points
  std::vector<LanePoint> result;
  size_t seg_idx = 0;

  for (float target : target_lengths) {
    // Move to the correct segment
    while (seg_idx + 1 < arc_lengths.size() && arc_lengths[seg_idx + 1] < target) {
      ++seg_idx;
    }

    // Interpolate between input[seg_idx] and input[seg_idx + 1]
    float seg_start = arc_lengths[seg_idx];
    float seg_end = arc_lengths[seg_idx + 1];
    float den = (seg_end - seg_start);
    float t = (std::abs(den) > 1e-3) ? (target - seg_start) / den : 0.0;
    result.push_back(input[seg_idx].lerp(input[seg_idx + 1], t));
  }

  return result;
}

// Interpolate lane waypoints with fixed arc length spacing (0.5m)
std::vector<LanePoint> interpolate_lane(const std::vector<LanePoint> & waypoints, float step = 0.5f)
{
  if (waypoints.size() < 2) return waypoints;

  // Compute cumulative arc lengths
  std::vector<float> distances(waypoints.size(), 0.0f);
  for (size_t i = 1; i < waypoints.size(); ++i) {
    distances[i] = distances[i - 1] + euclidean_distance(waypoints[i], waypoints[i - 1]);
  }

  float total_length = distances.back();
  std::vector<float> new_distances;
  for (float d = 0.0f; d < total_length; d += step) {
    new_distances.push_back(d);
  }
  if (std::abs(new_distances.back() - total_length) > 1e-6f) {
    new_distances.push_back(total_length);
  }

  // Interpolate new waypoints
  std::vector<LanePoint> new_waypoints;
  size_t j = 0;
  for (float d : new_distances) {
    while (j < distances.size() - 2 && distances[j + 1] < d) {
      ++j;
    }

    float t = (d - distances[j]) / (distances[j + 1] - distances[j]);
    new_waypoints.push_back(linear_interpolate(waypoints[j], waypoints[j + 1], t));
  }

  // Ensure first and last points match exactly (no duplication)
  if (euclidean_distance(new_waypoints.front(), waypoints.front()) > 1e-3f) {
    new_waypoints.insert(new_waypoints.begin(), waypoints.front());
  }
  if (euclidean_distance(new_waypoints.back(), waypoints.back()) > 1e-3f) {
    new_waypoints.push_back(waypoints.back());
  }

  return new_waypoints;
}

std::vector<LaneSegment> LaneletConverter::convert_to_lane_segments() const
{
  std::vector<LaneSegment> lane_segments;
  lane_segments.reserve(lanelet_map_ptr_->laneletLayer.size());
  // parse lanelet layers
  for (const auto & lanelet : lanelet_map_ptr_->laneletLayer) {
    const auto lanelet_subtype = toSubtypeName(lanelet);
    if (!isLaneLike(lanelet_subtype)) {
      std::cerr << "Skipping lanelet ID, since it is not LaneLike: " << lanelet.id() << std::endl;
      continue;
    }
    Polyline lane_polyline(MapType::Unused);
    std::vector<BoundarySegment> left_boundary_segments;
    std::vector<BoundarySegment> right_boundary_segments;
    // TODO (Daniel avoid unnecessary copy and creation)
    // TODO(Daniel): magic number num points
    auto points = fromLinestring(lanelet.centerline3d());
    lane_polyline.assign_waypoints(interpolate_points(points, LANE_POINTS));
    const auto left_bound = lanelet.leftBound3d();
    auto left_points = fromLinestring(left_bound);
    left_boundary_segments.emplace_back(
      MapType::Unused, interpolate_points(left_points, LANE_POINTS));
    const auto right_bound = lanelet.rightBound3d();
    auto right_points = fromLinestring(right_bound);
    right_boundary_segments.emplace_back(
      MapType::Unused, interpolate_points(right_points, LANE_POINTS));

    constexpr float kph2mph = 0.621371;
    const auto & attrs = lanelet.attributes();
    bool is_intersection = attrs.find("turn_direction") != attrs.end();
    std::optional<float> speed_limit_mph =
      attrs.find("speed_limit") != attrs.end()
        ? std::make_optional(std::stof(attrs.at("speed_limit").value()) * kph2mph)
        : std::nullopt;

    // TODO(Daniel): get proper light state, use behavior_velocity_traffic_light module as guide.
    auto traffic_light = TrafficLightElement::UNKNOWN;
    lane_segments.emplace_back(
      lanelet.id(), lane_polyline, is_intersection, left_boundary_segments, right_boundary_segments,
      speed_limit_mph, traffic_light);
  }
  return lane_segments;
}

[[nodiscard]] Eigen::MatrixXf LaneletConverter::process_segments_to_matrix(
  const std::vector<LaneSegment> & lane_segments, std::map<int64_t, long> & segment_row_indices,
  float center_x, float center_y, float mask_range) const
{
  std::vector<Eigen::MatrixXf> all_segment_matrices;

  long total_rows = 0;

  for (const auto & segment : lane_segments) {
    Eigen::MatrixXf segment_matrix =
      process_segment_to_matrix(segment, center_x, center_y, mask_range);

    if (segment_matrix.rows() != LANE_POINTS) {
      throw std::runtime_error("Segment matrix rows not equal to 20");
    }
    total_rows += segment_matrix.rows();
    all_segment_matrices.push_back(segment_matrix);
  }

  // Now allocate the full matrix
  const long cols = all_segment_matrices.empty() ? 0 : all_segment_matrices[0].cols();
  Eigen::MatrixXf stacked_matrix(total_rows, cols);

  long current_row = 0;
  for (const auto & mat : all_segment_matrices) {
    stacked_matrix.middleRows(current_row, mat.rows()) = mat;
    auto id = static_cast<int64_t>(mat(0, 13));
    segment_row_indices.emplace(id, current_row);
    if (mat.rows() != LANE_POINTS) {
      throw std::runtime_error("(2)Segment matrix rows not equal to 20");
    }
    current_row += mat.rows();
  }
  return stacked_matrix;
}

Eigen::MatrixXf LaneletConverter::process_segment_to_matrix(
  const LaneSegment & segment, float center_x, float center_y, float mask_range) const
{
  if (
    segment.polyline.is_empty() || segment.left_boundaries.empty() ||
    segment.right_boundaries.empty()) {
    return {};
  }
  const auto & centerlines = segment.polyline.waypoints();
  const auto & left_boundaries = segment.left_boundaries.front().waypoints();
  const auto & right_boundaries = segment.right_boundaries.front().waypoints();
  const auto & first_waypoint = segment.polyline.waypoints()[0];

  if (
    first_waypoint.x() < center_x - mask_range * 1.1f ||
    first_waypoint.x() > center_x + mask_range * 1.1f ||
    first_waypoint.y() < center_y - mask_range * 1.1f ||
    first_waypoint.y() > center_y + mask_range * 1.1f) {
    return {};
  }

  const size_t N = centerlines.size();
  if (left_boundaries.size() != N || right_boundaries.size() != N) {
    return {};
  }

  Eigen::MatrixXf segment_data(N, 14);  // 14 = 2 + 2 + 2 + 2 + 4 + 1 + 1

  // Encode traffic light as one-hot
  Eigen::Vector4f traffic_light_vec = Eigen::Vector4f::Zero();
  switch (segment.traffic_light) {
    case 1:
      traffic_light_vec[2] = 1.0f;
      break;  // RED
    case 2:
      traffic_light_vec[1] = 1.0f;
      break;  // AMBER
    case 3:
      traffic_light_vec[0] = 1.0f;
      break;  // GREEN
    case 4:
      traffic_light_vec[3] = 1.0f;
      break;  // WHITE
    default:
      traffic_light_vec[3] = 1.0f;
      break;  // UNKNOWN
  }

  // Build each row
  for (long i = 0; i < static_cast<long>(N); ++i) {
    segment_data(i, 0) = centerlines[i].x();
    segment_data(i, 1) = centerlines[i].y();
    segment_data(i, 2) =
      i < static_cast<long>(N) - 1 ? centerlines[i + 1].x() - centerlines[i].x() : 0.0f;
    segment_data(i, 3) =
      i < static_cast<long>(N) - 1 ? centerlines[i + 1].y() - centerlines[i].y() : 0.0f;
    segment_data(i, 4) = left_boundaries[i].x();
    segment_data(i, 5) = left_boundaries[i].y();
    segment_data(i, 6) = right_boundaries[i].x();
    segment_data(i, 7) = right_boundaries[i].y();
    segment_data.block<1, 4>(i, 8) = traffic_light_vec.transpose();
    segment_data(i, 12) = segment.speed_limit_mph.value_or(0.0f);
    segment_data(i, 13) = static_cast<float>(segment.id);
  }

  return segment_data;
}

std::optional<PolylineData> LaneletConverter::convert(
  const geometry_msgs::msg::Point & position, double distance_threshold) const
{
  std::vector<LanePoint> container;
  // parse lanelet layers
  for (const auto & lanelet : lanelet_map_ptr_->laneletLayer) {
    const auto lanelet_subtype = toSubtypeName(lanelet);
    if (isLaneLike(lanelet_subtype)) {
      // convert centerlines
      if (isRoadwayLike(lanelet_subtype)) {
        auto points = fromLinestring(lanelet.centerline3d(), position, distance_threshold);
        insertLanePoints(points, container);
      }
      // convert boundaries except of virtual lines
      if (!isTurnableIntersection(lanelet)) {
        const auto left_bound = lanelet.leftBound3d();
        if (isBoundaryLike(left_bound)) {
          auto points = fromLinestring(left_bound, position, distance_threshold);
          insertLanePoints(points, container);
        }
        const auto right_bound = lanelet.rightBound3d();
        if (isBoundaryLike(right_bound)) {
          auto points = fromLinestring(right_bound, position, distance_threshold);
          insertLanePoints(points, container);
        }
      }
    } else if (isCrosswalkLike(lanelet_subtype)) {
      auto points = fromPolygon(lanelet.polygon3d(), position, distance_threshold);
      insertLanePoints(points, container);
    }
  }

  // parse linestring layers
  for (const auto & linestring : lanelet_map_ptr_->lineStringLayer) {
    if (isBoundaryLike(linestring)) {
      auto points = fromLinestring(linestring, position, distance_threshold);
      insertLanePoints(points, container);
    }
  }

  return container.size() == 0
           ? std::nullopt
           : std::make_optional<PolylineData>(
               container, max_num_polyline_, max_num_point_, point_break_distance_);
}

std::vector<LanePoint> LaneletConverter::fromLinestring(
  const lanelet::ConstLineString3d & linestring) const noexcept
{
  if (linestring.size() == 0) {
    return {};
  }

  std::vector<LanePoint> output;
  for (auto itr = linestring.begin(); itr != linestring.end(); ++itr) {
    double dx, dy, dz;
    constexpr double epsilon = 1e-6;
    if (itr == linestring.begin()) {
      dx = 0.0;
      dy = 0.0;
      dz = 0.0;
    } else {
      dx = itr->x() - (itr - 1)->x();
      dy = itr->y() - (itr - 1)->y();
      dz = itr->z() - (itr - 1)->z();
      const auto norm = std::hypot(dx, dy, dz);
      dx /= (norm + epsilon);
      dy /= (norm + epsilon);
      dz /= (norm + epsilon);
    }
    output.emplace_back(
      itr->x(), itr->y(), itr->z(), dx, dy, dz, 0.0);  // TODO(danielsanchezaran): Label ID
  }
  return output;
}

std::vector<LanePoint> LaneletConverter::fromLinestring(
  const lanelet::ConstLineString3d & linestring, const geometry_msgs::msg::Point & position,
  double distance_threshold) const noexcept
{
  if (linestring.size() == 0) {
    return {};
  }

  std::vector<LanePoint> output;
  for (auto itr = linestring.begin(); itr != linestring.end(); ++itr) {
    if (auto distance =
          std::hypot(itr->x() - position.x, itr->y() - position.y, itr->z() - position.z);
        distance > distance_threshold) {
      continue;
    }
    double dx, dy, dz;
    constexpr double epsilon = 1e-6;
    if (itr == linestring.begin()) {
      dx = 0.0;
      dy = 0.0;
      dz = 0.0;
    } else {
      dx = itr->x() - (itr - 1)->x();
      dy = itr->y() - (itr - 1)->y();
      dz = itr->z() - (itr - 1)->z();
      const auto norm = std::hypot(dx, dy, dz);
      dx /= (norm + epsilon);
      dy /= (norm + epsilon);
      dz /= (norm + epsilon);
    }
    output.emplace_back(
      itr->x(), itr->y(), itr->z(), dx, dy, dz, 0.0);  // TODO(danielsanchezaran): Label ID
  }
  return output;
}

std::vector<LanePoint> LaneletConverter::fromPolygon(
  const lanelet::CompoundPolygon3d & polygon) const noexcept
{
  if (polygon.size() == 0) {
    return {};
  }

  std::vector<LanePoint> output;
  for (auto itr = polygon.begin(); itr != polygon.end(); ++itr) {
    double dx, dy, dz;
    constexpr double epsilon = 1e-6;
    if (itr == polygon.begin()) {
      dx = 0.0;
      dy = 0.0;
      dz = 0.0;
    } else {
      dx = itr->x() - (itr - 1)->x();
      dy = itr->y() - (itr - 1)->y();
      dz = itr->z() - (itr - 1)->z();
      const auto norm = std::hypot(dx, dy, dz);
      dx /= (norm + epsilon);
      dy /= (norm + epsilon);
      dz /= (norm + epsilon);
    }
    output.emplace_back(
      itr->x(), itr->y(), itr->z(), dx, dy, dz, 0.0);  // TODO(danielsanchezaran): Label ID
  }
  return output;
}

std::vector<LanePoint> LaneletConverter::fromPolygon(
  const lanelet::CompoundPolygon3d & polygon, const geometry_msgs::msg::Point & position,
  double distance_threshold) const noexcept
{
  if (polygon.size() == 0) {
    return {};
  }

  std::vector<LanePoint> output;
  for (auto itr = polygon.begin(); itr != polygon.end(); ++itr) {
    if (auto distance =
          std::hypot(itr->x() - position.x, itr->y() - position.y, itr->z() - position.z);
        distance > distance_threshold) {
      continue;
    }
    double dx, dy, dz;
    constexpr double epsilon = 1e-6;
    if (itr == polygon.begin()) {
      dx = 0.0;
      dy = 0.0;
      dz = 0.0;
    } else {
      dx = itr->x() - (itr - 1)->x();
      dy = itr->y() - (itr - 1)->y();
      dz = itr->z() - (itr - 1)->z();
      const auto norm = std::hypot(dx, dy, dz);
      dx /= (norm + epsilon);
      dy /= (norm + epsilon);
      dz /= (norm + epsilon);
    }
    output.emplace_back(
      itr->x(), itr->y(), itr->z(), dx, dy, dz, 0.0);  // TODO(danielsanchezaran): Label ID
  }
  return output;
}
}  // namespace autoware::diffusion_planner
