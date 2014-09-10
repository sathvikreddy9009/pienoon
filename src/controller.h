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

#ifndef SPLAT_CONTROLLER_H_
#define SPLAT_CONTROLLER_H_

#include <cstdint>
#include "common.h"

namespace fpl {
namespace splat {

class Controller {
 public:
  Controller() : is_down_(0u), went_down_(0u), went_up_(0u) {}
  virtual ~Controller() {}

  // Update the current state of this controller.
  virtual void AdvanceFrame(WorldTime delta_time) = 0;

  // Returns the current set of active logical input bits.
  uint32_t is_down() const { return is_down_; }
  uint32_t went_down() const { return went_down_; }
  uint32_t went_up() const { return went_up_; }

  // Updates a one or more bits.
  void SetLogicalInputs(uint32_t bitmap, bool set);

  // Clear all the currently set logical inputs.
  void ClearAllLogicalInputs();

 protected:
  // A bitfield of currently active logical input bits.
  uint32_t is_down_;
  uint32_t went_down_;
  uint32_t went_up_;
};

}  // splat
}  // fpl

#endif  // SPLAT_CONTROLLER_H_
