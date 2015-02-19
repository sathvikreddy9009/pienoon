// Copyright 2014 Google Inc. All rights reserved.
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

#ifndef FPL_BULK_SPLINE_EVALUATOR_H_
#define FPL_BULK_SPLINE_EVALUATOR_H_

#include "compact_spline.h"

namespace fpl {

// Traverse through a set of splines in a performant way.
//
// This class should be used if you have hundreds or more splines that you need
// to traverse in a uniform manner. It stores the spline data so that this
// traversal is very fast, when done in bulk, and so we can take advantage of
// SIMD on supported processors.
//
// This class maintains a current 'x' value for each spline, and a current
// cubic-curve for the segment of the spline corresponding to that 'x'.
// In AdvanceFrame, the 'x's are incremented. If this increment pushes us to
// the next segment of a spline, the cubic-curve is reinitialized to the next
// segment of the spline. The splines are evaluated at the current 'x' in bulk.
//
class BulkSplineEvaluator {
 public:
  typedef int Index;

  // This class holds a set of splines, each is given an index from 0 to size-1.
  //
  // The number of splines can be increased or decreased with SetNumIndices().
  //   - splines are allocated or removed at the highest indices
  //
  // Unused indices are still processed every frame. You can fill these index
  // holes with MoveIndex(), to move items from the last index into the hole.
  // Once all holes have been moved to the highest indices, you can call
  // SetNumIndices() to stop processing these highest indices. Note that this
  // is exactly what fpl::IndexAllocator does. You should use that class to
  // keep your indices contiguous.
  //
  void SetNumIndices(const Index num_indices);
  void MoveIndex(const Index old_index, const Index new_index);

  // Initialize 'index' to process 'spline' starting from 'start_x'.
  // The Y() and Derivative() values are immediately available.
  void SetSpline(const Index index, const CompactSpline& spline,
                 const float start_x = 0.0f);

  // Increment x and update the Y() and Derivative() values for all indices.
  // Process all indices in bulk to efficiently traverse memory and allow SIMD
  // instructions to be effective.
  void AdvanceFrame(const float delta_x);

  // Query the current state of the spline associated with 'index'.
  bool Valid(const Index index) const;
  float X(const Index index) const { return domains_[index].x; }
  float Y(const Index index) const { return results_[index].y; }
  float Derivative(const Index index) const {
    return results_[index].derivative;
  }
  const CompactSpline* SourceSpline(const Index index) const {
    return splines_[index];
  }

  // Get the raw cubic curve for 'index'. Useful if you need to calculate the
  // second or third derivatives (which are not calculated in AdvanceFrame),
  // or plot the curve for debug reasons.
  const CubicCurve& Cubic(const Index index) const { return cubics_[index]; }
  float CubicX(const Index index) const {
    const Domain& d = domains_[index];
    return OutsideSpline(d.x_index) ? 0.0f : d.x - d.range.start();
  }

  // Get state at end of the spline.
  float EndX(const Index index) const { return splines_[index]->EndX(); }
  float EndY(const Index index) const { return splines_[index]->EndY(); }
  float EndDerivative(const Index index) const {
    return splines_[index]->EndDerivative();
  }

 private:
  void InitCubic(const Index index);
  void EvaluateIndex(const Index index);
  Index NumIndices() const { return static_cast<Index>(splines_.size()); }

  struct Domain {
    Domain() : x(0.0f), x_index(kInvalidSplineIndex) {}

    // The start and end 'x' values for the current Cubic.
    Range range;

    // The current 'x' value. We evaluate the cubic at x - range.start.
    float x;

    // Index of 'x' in 'spline'. This is the current spline node segment.
    CompactSplineIndex x_index;
  };

  struct Result {
    Result() : y(0.0f), derivative(0.0f) {}

    // Value of the spline at 'Domain::x'. Evaluated in AdvanceFrame.
    float y;

    // Value of the spline's derivative at 'Domain::x'.
    // Evaluated in AdvanceFrame.
    float derivative;
  };

  // Data is organized in struct-of-array format to match the algorithm's
  // consumption of the data.
  // - The algorithm that transitions us to the next segment of the spline
  //   looks only at data in 'domain_' (until it finds a segment that needs
  //   to be transitioned).
  // - The algorithm that updates 'results_' looks only at the data in
  //   'cubics_'.
  // These vectors grow when SetNumIndices() is called, but they never shrink.
  // So, we'll have a few reallocs (which are slow) until the highwater mark is
  // reached. Then the cost of reallocs disappears. In this way we have a
  // reasonable tradeoff between memory conservation and runtime performance.

  // Pointers to the source spline nodes. Spline data is owned externally.
  // We neither allocate or free this pointer here.
  std::vector<const CompactSpline*> splines_;

  // Currently active segment of splines_.
  // Instantiated from splines_[i].CreateInitCubic(domains_[i].x_index).
  std::vector<CubicCurve> cubics_;

  // X-related values. We keep them together so that we can quickly check if
  // x is still in the valid range of the current 'cubic_'.
  std::vector<Domain> domains_;

  // Result of evaluating the current cubic at the current x.
  std::vector<Result> results_;
};

}  // namespace fpl

#endif  // FPL_BULK_SPLINE_EVALUATOR_H_